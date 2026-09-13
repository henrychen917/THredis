# Baseline-quality inspiration study

Scope: connection/memory hygiene, overload, large replies, protocol/DoS limits, event-loop hygiene, and allocation only. Commands, clustering, and replication are deliberately excluded. References use `D:` for DragonflyDB, `R:` for the optimized Redis 8.6 fork, and `T:` for tomokv-cpp-perthread. Line numbers describe the inspected trees on 2026-08-24.

## Executive result: ranked ADOPTs

| Rank | Mechanism | Why it is the highest-value baseline work |
|---:|---|---|
| 1 | Byte-accounted per-client reply envelope | The 64-op ROB bounds count, not bytes; one client can concurrently materialize many 512 MB GET replies. |
| 2 | Borrowed large-value replies over scatter/gather | Removes the dominant value copy and prevents connection buffers from doubling the footprint of a legal 512 MB GET. |
| 3 | Global memory/max-client admission gates | Refuse work before allocation failure, stop accepting under RSS pressure, and keep handshake-only sockets cheap. |
| 4 | Complete connection-memory ledger and metrics | Makes limits enforceable and exposes which retained component is consuming RSS. |
| 5 | Amortized connection maintenance with adaptive shedding | Reclaims burst-grown input/output/ROB capacity without adding an idle-connection scan to the hot loop. |
| 6 | Tight, strict protocol limits | Closes the unbounded-inline, oversized-key, excessive-argc, and malformed-terminator DoS holes. |
| 7 | jemalloc telemetry, background decay, and explicit purge | Preserves the current fast allocator while making arena fragmentation and post-spike RSS controllable. |

The first two must be designed together: a zero-copy reply consumes few connection-owned bytes but can pin an old 512 MB value after SET/DEL. Count pinned/orphanable payload bytes in the same per-client envelope, reserve them before publishing the borrow, and release the reservation only when the final send CQE retires the reference.

## 1. Connection-memory hygiene

### Adaptive input-buffer shrink

- Reference: Dragonfly starts at 256 B, grows geometrically or from a parser hint up to 32 KiB, records a fixed-window high-water mark, and halves capacity only at a safe idle park point; the shrink interval defaults to 30 s (`D:src/facade/dragonfly_connection.cc:109-110,159-160,874-878,3570-3605,3612-3702`). Redis trims after more than 4 KiB waste and either two seconds idle or capacity greater than twice the recent peak; an empty idle client frees its private query buffer entirely and uses a reusable buffer on its next read (`R:src/server.c:3907-3955`).
- Tomo: every client starts with a private 16 KiB allocation; backlog growth stops at 1 MiB, one incomplete command may grow to 512 MiB plus slack, and only a completely empty buffer above 1 MiB shrinks straight back to 16 KiB (`T:src/net/conn.h:49-58,74-78,103-151`). It retains any 16 KiB..1 MiB burst forever and tracks neither interaction time nor recent peak. An armed io_uring recv holds a raw pointer, so externally shrinking a truly idle client first requires cancel/terminal-CQE ownership (`T:src/net/conn.h:122-131`; `T:src/core/io_loop.h:145-159`).
- Decision — **ADOPT (#5):** use a recent high-water window and halve at quiescence; keep a 16 KiB warm floor, then move long-idle clients to a safely re-armed 2 KiB cold buffer (or a shared provided-buffer design) so max-client idle RSS is not `16 KiB × connections`.

### Output-buffer resizing and tail trimming

- Reference: Redis halves/expands its contiguous reply buffer from a 1 KiB floor using a five-second peak window and refuses to realloc while an async send or encoded reference pins it (`R:src/server.h:182-197`; `R:src/server.c:3958-4024`). It also trims a reply-list tail only when more than 25% is unused and the used part is below 16 KiB (`R:src/networking.c:1592-1619`).
- Tomo: both 512 B-inline write buffers retain heap growth until empty capacity exceeds 64 KiB; oversized per-op spill buffers are shed above 4 KiB (`T:src/net/conn.h:59-63,138-143`; `T:src/exec/op.h:84-92`). There is no reply list, and capacities between inline and 64 KiB remain pinned.
- Decision — **ADAPT:** add high-water/idle halving to the two `SmallBuf`s, but skip Redis's reply-list-tail algorithm because Tomo has no analogous list node.

### Cached per-connection execution objects

- Reference: Dragonfly gradually discards parsed-message cache entries after a low-use interval instead of keeping a burst-size pool indefinitely (`D:src/facade/dragonfly_connection.cc:672-696,2337-2345`).
- Tomo: ROB contexts materialize in chunks of eight; a once-deep connection can retain all eight chunks (about 21 KiB) until disconnect (`T:src/net/rob.h:32-38,60-69,117-120`). Per-op argv/reply spills do shrink at retirement.
- Decision — **ADAPT:** at a quiescent maintenance visit, release unused ROB chunks above the first after a cooldown; keep the p1 chunk and never touch chunks on the active path.

### Per-connection accounting

- Reference: Dragonfly estimates object/parser/context/reply-builder/fiber memory and refreshes per-thread connection totals by deltas; queue entries and queue bytes are accounted separately (`D:src/facade/dragonfly_connection.cc:2750-2764,2776-2850`). Redis totals allocated query buffer, static and listed output buffers, list overhead, client objects, argv, and other client state (`R:src/networking.c:7173-7232`).
- Tomo: no per-client or aggregate connection-memory counter exists. Relevant retained capacity spans `rcap`, two write-buffer capacities, materialized ROB chunks, argv/reply spills, and—after zero-copy—borrowed/orphaned values (`T:src/net/conn.h:72-78,168-183`; `T:src/net/rob.h:117-120`; `T:src/exec/op.h:84-92`). `FlatStore::resident_estimate()` accounts store objects, not connections (`T:src/store/flatstore.h:143-152`).
- Decision — **ADOPT (#4):** maintain cheap owner-thread deltas plus gauges/peaks by component; exact capacity accounting is simpler here than either reference and is prerequisite to meaningful caps.

## 2. Overload behavior

### Per-client output and pinned-value limits

- Reference: Redis computes allocated output memory, applies immediate hard limits plus time-qualified soft limits, and closes asynchronously when unsafe to free inline (`R:src/networking.c:7290-7384`). Normal clients default to unlimited upstream-style settings (`R:redis.conf:2192-2234`), which is inappropriate when a request may return 512 MB. Dragonfly independently caps pipeline memory per I/O thread at 128 MiB and queue length per connection at 10,000, stopping reads until below the limit (`D:src/facade/dragonfly_connection.cc:69-83,525-602`).
- Tomo: the 64-slot ROB stops parsing by operation count (`T:src/net/conn.h:49`; `T:src/net/rob.h:56-69`) but each GET copies its full value into reply storage (`T:src/cmd/commands.cc:30-36`; `T:src/net/resp.h:149-157`). Neither staged bytes nor future borrowed payloads have a cap.
- Decision — **ADOPT (#1):** reserve pending wire/pinned bytes at execution, stop reads at a soft watermark, and safely close at a configurable hard watermark no lower than one maximum legal reply plus slack; enforce allocated capacity separately through the rank-#4 ledger.

Suggested accounting policy: keep two gauges. Admission charges pending wire bytes plus pinned/orphanable payload, with a hard cap just above 512 MiB so one maximum GET succeeds but a second cannot stage; retained-memory accounting charges actual buffer capacities, ROB storage, and pins, feeding a separate per-client/global memory gate. Before rank #2 lands, geometric spill-then-fill copies make one maximum GET consume much more capacity than its wire size, so preserve explicit transient headroom rather than pretending a roughly 512 MiB allocated-memory cap is compatible. Add a lower time-qualified soft cap and a per-I/O-thread aggregate cap. Small fixed replies can be charged on retirement; a large GET must atomically reserve before borrowing/copying because workers for one connection may execute concurrently.

### Accept admission and global memory pressure

- Reference: Dragonfly defaults `maxclients` to 64,000, rejects excess accepts, pauses non-privileged listeners when RSS crosses `maxmemory * rss_oom_deny_ratio`, resumes below it, and enables one-second `TCP_DEFER_ACCEPT` to avoid allocating userspace state for handshake-only sockets (`D:src/server/server_family.cc:119,1411-1425,2116-2121`; `D:src/facade/dragonfly_listener.cc:198-220,358-361`). Its command gate rejects work when used memory or RSS exceeds the configured limit (`D:src/server/server_state.cc:173-190`). Redis performs max-client admission before client construction/transport negotiation (`R:src/networking.c:2399-2439`).
- Tomo: multishot accept is continuously armed and every accepted fd unconditionally allocates a `Client`; there is no max-client, RSS, or store-memory gate (`T:src/core/io_loop.h:90-103,133-143,178-200`). `Client` and `SmallBuf::grow` also assume allocation succeeds (`T:src/net/conn.h:74-77`; `T:src/base/slice.h:97-104`).
- Decision — **ADOPT (#3):** add per-process/per-I/O max-client admission, `TCP_DEFER_ACCEPT`, projected-size rejection for SET, and listener pause/resume on used/RSS watermarks so allocator failure is never the normal overload policy.

### Pipeline squashing and fairness

- Reference: Dragonfly stops parsing at pipeline byte/count limits and can squash queued commands into bounded batches (`D:src/facade/dragonfly_connection.cc:97-98,121-133,1925-1940`).
- Tomo: a 64-op window already provides much tighter count backpressure; execution is shard-owned, retirement is ordered, the active FIFO serves at most 16 clients per pass, and only signaled clients are served (`T:src/net/rob.h:56-69`; `T:src/core/io_loop.h:368-389,439-455`).
- Decision — **SKIP:** do not import a squashing layer; add byte admission to the existing ROB and preserve its simpler locality/fairness model.

### Evicting a heavy client / `CLIENT NO-EVICT`

- Reference: the Redis command still toggles `CLIENT_NO_EVICT` (`R:src/networking.c:6404-6411`), but this fork explicitly rejects nonzero `maxmemory-clients` because global client eviction conflicts with mandatory sharded execution (`R:src/config.c:3215-3224`; `R:redis.conf:2247-2249`).
- Tomo: no client eviction or privilege flag exists.
- Decision — **SKIP:** deterministic per-client hard limits and admission backpressure are cheaper, more predictable, and harder for public clients to exempt themselves from.

### Linux OOM score

- Reference: Redis optionally reads and remembers the original `/proc/self/oom_score_adj`, supports relative/absolute values, clamps to `[-1000,1000]`, logs failures, and restores the original value when disabled; default is off (`R:src/server.c:7728-7796`; `R:redis.conf:1371-1395`).
- Tomo: no OOM-score handling exists.
- Decision — **ADAPT:** expose one optional process-level relative/absolute setting with restore and logging; omit Redis's role/background-child matrix and never silently lower the score.

## 3. Large-value send paths

### Borrowed value plus scatter/gather

- Reference: Dragonfly formats small framing into an 8 KiB scratch buffer, references larger pieces in an inline-16 iovec vector, and uses a scoped lifetime through the blocking sink write (`D:src/facade/reply_builder.h:39-40,61-69,173-181`; `D:src/facade/reply_builder.cc:104-129,164-169,197-248,336-345`). GET borrows raw values at 16 KiB and above (`D:src/server/string_family.cc:46-49,83-93,750-757`). A pin registry orphans an overwritten/deleted backing block and reaps it only after the borrow releases (`D:src/core/compact_object.cc:425-502,617-659,1212-1227`; `D:src/server/engine_shard.cc:775-780`).
- Reference: the Redis fork represents a bulk reply as retained object + local prefix/CRLF, expands it into three iovecs, handles partial writes, and releases only after those bytes retire; worker-owned refs return to their owner over a free-back ring (`R:src/networking.c:1219-1270,1970-2026,3335-3485,3530-3666`; `R:src/server.c:25777-25838`). Its topology thresholds are 16 KiB without I/O threads and 64 KiB with 2-6 I/O threads, while forwarded worker replies use configurable `tomokv-zerocopy-min-value` (default 1 KiB) (`R:src/server.h:194-197`; `R:src/config.c:3433`).
- Tomo: GET calls `reply_bulk`, which reserves and `memcpy`s the value (`T:src/cmd/commands.cc:30-36`; `T:src/net/resp.h:146-157`). A direct reply only avoids an intermediate copy if the already-empty connection buffer has enough retained capacity; otherwise the op spill is copied again into the fill buffer, then one contiguous `IORING_OP_SEND` is issued (`T:src/core/io_loop.h:300-312`; `T:src/net/wb.h:71-84,93-114`). External values above 192 B have stable standalone storage, but SET/DEL frees it immediately and there is no borrow lifetime (`T:src/store/kvobj.h:114-164,187-197`).
- Decision — **ADOPT (#2):** for raw values starting at 16 KiB, send `{RESP header, value view, CRLF}` with `IORING_OP_SENDMSG`; pin the external block, defer mutation-time free on its shard, and return the unpin to that shard only after final CQE retirement.

Implementation constraints: persist iov index/offset across partial sends; cap iovec count/bytes per submission; never release at ROB retirement; keep close/cancel paths capable of releasing every pin; and include pinned bytes in rank #1. Start at Dragonfly's 16 KiB and benchmark 16/64 KiB rather than copying Redis's topology-specific thresholds.

### Kernel `SEND_ZC`

- Reference: the useful common mechanism above is userspace copy avoidance plus writev/sendmsg lifetime control. Dragonfly's shown path calls a normal vector sink write. The Redis fork's active uring2 direct-send guard avoids a scratch copy only for eligible plain prefixes and otherwise copies 16 KiB chunks (`R:src/uring2.c:1070-1142`); its dedicated WB `SENDMSG` implementation is retained under `#if 0` (`R:src/wb_uring.c:1-18`).
- Tomo: ordinary async `SEND` already has simple CQE ownership.
- Decision — **SKIP for now:** prove borrowed `SENDMSG` first; kernel zero-copy adds notification CQEs and longer pin lifetimes before evidence says the remaining kernel copy matters.

## 4. Protocol hard limits and DoS guards

### Frame, argument, and semantic limits

- Reference: Dragonfly defaults to 65,536 array elements and 2 GiB bulk strings; length headers are constrained by a small fixed stash (`D:src/facade/dragonfly_connection.cc:100-110`; `D:src/facade/resp_srv_parser.cc:169-237,240-264`). Redis caps inline/header lines at 64 KiB, normal bulk strings at 512 MiB, unauthenticated arrays at 10 and bulks at 16 KiB, and unauthenticated query memory at 1 MiB (`R:src/server.h:181-188`; `R:src/networking.c:4666-4702,4760-4792,5753-5765`; `R:src/config.c:3605,3628`).
- Tomo: bulk is correctly limited to 512 MiB, but arrays allow 1,048,576 elements, inline input has no length limit, all argument roles may be 512 MiB (there is no actual 512 KiB key guard), and the parser advances over the two post-bulk bytes without verifying `\r\n` (`T:src/net/resp.h:20-49,59-106`). Command arity is checked only after the entire frame has been parsed (`T:src/core/io_loop.h:224-249`).
- Decision — **ADOPT (#6):** retain 512 MiB values, enforce 512 KiB keys, cap inline/header lines at 64 KiB, reduce global argc to at most 1,024 (then reject known-command excess arity early), and validate every bulk terminator before accepting the frame.

The argc choice is intentionally tighter than both references: this server has no multi-key commands, so one million `Slice`s and their parse work buy no compatibility. Make 1,024 configurable up to Dragonfly's 65,536 only if a future command needs it.

### Unauthenticated / incomplete-connection protection

- Reference: Redis uses the tiny pre-auth caps above but no distinct default authentication deadline; its general idle timeout is optional (`R:src/timeout.c:33-59`). Dragonfly combines `TCP_DEFER_ACCEPT` with optional general read/send timeouts, checked in bounded batches (`D:src/server/server_state.cc:299-368`).
- Tomo: there is no AUTH state, last-interaction time, incomplete-command deadline, or idle timeout; one byte is enough to retain a client and begin growing an incomplete request.
- Decision — **ADAPT:** interpret “unauthenticated” as “before the first complete command”: use `TCP_DEFER_ACCEPT` plus a configurable no-read-progress/first-command timeout in the rank-#5 maintenance cursor, without imposing an absolute deadline on a progressing 512 MiB SET.

### Parse-loop CPU monopolization

- Reference: Dragonfly yields after 200 microseconds of busy socket reading/parsing (`D:src/facade/dragonfly_connection.cc:117-119`).
- Tomo: parsing naturally stops at 64 published operations, and the serve phase is separately budgeted (`T:src/core/io_loop.h:220-229,439-455`). A single frame can still be large, addressed by the new argc/line limits.
- Decision — **SKIP:** do not add a clock read to the hot parse loop unless post-limit profiling shows a remaining monopolization case.

## 5. Event-loop hygiene and idle CPU

### No work proportional to idle connections

- Reference: Redis's event backend dispatches fired fds and moves connection housekeeping to `clientsCron`, rotating roughly `N / hz` clients per tick so each is visited about once per second (`R:src/server.c:4218-4279`). Dragonfly's connection watcher wakes once per second and traverses at most 100 connections (`D:src/server/server_state.cc:299-360`).
- Tomo: this is already stronger on the hot path: CQEs and worker-ready bits identify work, only `active_` connections are walked, completed-work serving is targeted, and at most 16 clients are served per pass (`T:src/core/io_loop.h:90-129,356-389,396-455`). Idle connections are removed from `active_` (`T:src/core/io_loop.h:430-436`).
- Decision — **SKIP:** keep the current active-set/CQE architecture; importing fibers, an ae loop, or a per-pass client scan would be a regression.

### Maintenance scheduling and timed parking

- Reference: both references amortize ordinary connection maintenance with a cursor. Redis uses a deadline-ordered radix tree only for exact blocking-command timeouts, visiting just the expired prefix (`R:src/timeout.c:62-136`).
- Tomo: it has no connection-maintenance cursor. An otherwise idle ring wakes every 50 ms solely to recheck stop/missed-wake state (`T:src/net/uring.h:96-109`).
- Decision — **ADOPT (#5) / ADAPT:** add one bounded per-I/O cursor for shrink, caps, and timeout checks; skip a timer wheel/tree until exact high-cardinality deadlines exist, and replace the 20 Hz safety wake with a durable shutdown wake plus a slower maintenance/missed-wake deadline.

## 6. Allocators and arenas

### Allocator observability, decay, and purge

- Reference: Redis refreshes jemalloc epoch stats and reports allocated/active/resident/retained/muzzy plus small-bin fragmentation, enables background purging by default, and exposes an all-arena purge (`R:src/zmalloc.c:950-1065`; `R:redis.conf:2437-2438`; `R:src/object.c:1923-1927`). This fork additionally registers jemalloc's `thread.allocatedp`/`thread.deallocatedp` once and samples per-thread traffic plus tcache fill/flush counters with no allocation-path hook (`R:src/zmalloc.c:1068-1144`; `R:src/debug.c:1171-1203`). Dragonfly routes shard allocations through a per-thread mimalloc PMR that tracks usable bytes exactly and can force heap collection (`D:src/core/mi_memory_resource.h:13-38`; `D:src/core/mi_memory_resource.cc:14-44`; `D:src/server/server_state.cc:148-153,236-242`).
- Tomo: it uses `nallocx`, `mallocx`, and sized frees, creates one arena per execution worker, but discards the arena ID and exposes no allocator stats, decay/background-thread policy, or purge; I/O threads use jemalloc's normal thread assignment (`T:src/base/alloc.h:39-43,58-101`; `T:src/main.cc:180-195`). Store `resident_estimate()` is requested-size-class accounting, not allocator active/resident/RSS.
- Decision — **ADOPT (#7):** retain arena IDs, sample global/per-arena jemalloc stats off-path, register per-thread counters, enable documented background decay, and provide an operator/pressure-triggered purge with latency metrics.

Do not purge on each large DEL or buffer shrink: decay/background purge keeps the owner loop out of `madvise`, while a manual purge is for post-spike recovery. Alert on `resident / active` and RSS versus the global memory gate; those distinguish retained allocator pages from live connection/store memory.

### Allocator replacement, PMR plumbing, and active defrag

- Reference: Dragonfly's mimalloc heap/PMR makes nested container allocations chargeable to the shard and runs allocator-utilization-gated defrag as an idle task with a persistent cursor (`D:src/server/engine_shard.cc:275-331,353-466,540`). Redis likewise time-slices active defrag and adapts effort from measured fragmentation (`R:src/defrag.c:1320-1405`).
- Tomo: values are one compact object plus at most one external block, allocation/free stays on the shard owner in normal operation, size-class overwrite already avoids many reallocations, and jemalloc was deliberately selected (`T:src/store/kvobj.h:114-197`; `T:src/store/flatstore.h:161-177`; `T:src/base/alloc.h:1-7`).
- Decision — **SKIP:** do not switch allocators, add a pool in front of jemalloc, or move live `KvObj`s for defrag until the new telemetry proves fragmentation large enough to repay pointer/lifetime complexity.

## Recommended implementation order and gates

1. Add accounting fields/metrics, protocol limits, maxclients, TCP defer, and memory gates first; torture must cover limit-edge frames, slow readers, connection churn, and allocation-failure injection.
2. Add byte reservations/output hard-close behavior while replies are still copied; torture `64 × 512 MB GET`, pipe-beyond-window, partial sends, RST, and close-with-inflight-reply.
3. Add borrowed external values and `SENDMSG`, preserving the same byte envelope; torture concurrent GET/SET/DEL of the same huge key, orphan reclamation, partial CQEs, cancel/close, and shard-owner free-back saturation.
4. Add the bounded maintenance cursor and adaptive shrink; verify idle work is independent of connection count per pass and measure idle wakeups/CPU before and after.
5. Turn on allocator statistics/decay, establish fragmentation/RSS baselines, then tune purge thresholds from evidence.

Success criteria are memory invariants, not merely throughput: total connection-accounted bytes reconcile with component totals; no client exceeds its reservation; every borrowed byte has exactly one release on success/error/cancel; accepted clients remain within admission budgets; and RSS returns toward `active` after burst capacity is shed and decay/purge runs.

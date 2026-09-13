# Complexity audit

Audit date: 2026-08-24. Scope: commit `b187b7edf3005a4e70132340438b2f8e204497f3` in the checked-out C++20 tree, read-only except for this report. No benchmark was run. Structural sizes below were measured with this tree's compiler/ABI; default-jemalloc size-class estimates use the build's `JE=1` backend.

## Verdict and notation

The implementation does **not** meet the owner's complexity bar. The store has a good intended expected-case open-addressing design and several good work-proportional mechanisms, but its load-factor arithmetic overflows before the requested 100M-key scale. The loop architecture also still contains exactly the forbidden shape: work is repeatedly charged to sets that merely *could* contain work. The most important examples are the all-active-connection phase, per-iteration all-channel depth sampling, pre-park all-channel scans, fixed full-mask sweeps, all-shard publication per 32-op batch, and teardown table scans. Memory also fails the “no stale byte” rule in buffers, ROB chunks, vectors, the channel mesh, and cold/incomplete store resizes.

Axes used throughout:

- `C`: live connections server-wide; `C_i`: connections owned by one I/O thread.
- `T`: total threads (`T <= 128`); `I`/`E`: I/O/executor counts.
- `P`: pipeline/ROB depth (`P <= 64`).
- `K`, `V`: key and value/reply byte lengths. `B` is total request bytes and `G` is argument count.
- `N`: live keys in a store/table; `M` is its slot capacity. `alpha=(live+tombstones)/M` is effective probe occupancy.
- `A`: one I/O thread's active-set size; `Q`: its pending-serve FIFO length; `F`: queued channel entries; `S_w`: shards owned by one executor.

The compiled ceilings make some loops technically constant in a literal mathematical model. This audit deliberately does not hide them behind that fact: a 128-thread scan is reported as `O(T)`, a 16-word scan as `O(1024/64)`, and a 64-slot traversal as `O(P)`, because those are the scaling axes the owner asked to control.

An axis not named in a table row has no direct dependence for that operation; indirect multiplication through the enclosing pass is called out separately.

## Receive, parse, and dispatch

| Operation | Cost | Axis dependence | Verdict |
|---|---:|---|---|
| CQ batch drain | `O(number of CQEs)` | Work-proportional; no direct `C/T/P/K/V/N` term. The loop handles every currently visible CQE and advances once (`src/net/uring.h:112-120`). | OK |
| SQE acquisition/submission | `O(1)` normally; a full SQ forces a batch submit | Amortized constant per submitted request; syscall and kernel validation cost track actual SQEs. A second full result may still be null despite the “never returns nullptr” comment (`src/net/uring.h:73-81`). | OK amortization; RISK error handling |
| Accept one connection | Amortized `O(1)` plus fixed 16 KiB allocation | `C`: client/slot vectors can occasionally reallocate and copy `O(C_i)` pointers; otherwise independent. A `Client` and read buffer are allocated at `src/core/io_loop.h:190-195` and `src/net/conn.h:74-77`. | OK for CPU; RISK for retained memory |
| Recv arm | `O(1)` plus possible rbuf growth/SQ submit | No scan itself; repeated for every active client by phase 1 (`src/core/io_loop.h:145-160`). | OK locally; RISK enclosing `A` pass |
| First recv arm after accept | `O(1)` when an SQE is available | On SQE starvation, `arm_recv` returns but the new client is only in `clients_`, not `active_`, so no retry path names it (`src/core/io_loop.h:147-160`, `src/core/io_loop.h:190-196`). Independent of `P/K/V/N`, but incidence grows with connection/ring pressure. | **RISK: permanent accepted-connection stall** |
| Receive completion | `O(1)` bookkeeping, then parsing | Byte transfer is kernel work proportional to received bytes; `commit_read` itself is constant (`src/core/io_loop.h:203-211`, `src/net/conn.h:120`). | OK |
| Read-buffer growth | One growth costs `O(current buffered bytes)`; amortized `O(B)` over geometric growth | `K/V/B`: doubles from 16 KiB to a 1 MiB backlog cap or about 512 MiB for one incomplete command (`src/net/conn.h:103-115`). No `C/T/P/N` CPU term, but memory multiplies by `C`. | OK amortization; RISK retention/stall |
| Quiescent read-buffer reset | `O(unparsed tail bytes)` | `B`: `memmove` of the remainder (`src/net/conn.h:133-151`). This is strictly proportional to bytes that must be preserved. | OK |
| RESP parsing, one complete attempt | Inline: `O(B)`; multibulk: `O(G + header bytes)` because bulk payloads are sliced, not scanned | `K/V` enter through `B`; `G` can be 1M. Slices avoid argument-byte copies (`src/net/resp.h:54-107`). | OK for one attempt |
| RESP parsing across incomplete arrivals | Worst `O(B^2/kRecvChunk)` | The parser restarts at the command's beginning. An incomplete inline command re-scans for CRLF; a many-small-argument multibulk re-walks all completed arguments on every arrival (`src/net/resp.h:54-105`). A single large bulk is better because its payload is skipped arithmetically. | RISK |
| Argument append | Amortized `O(1)` per arg, `O(G)` total; a growth copies `O(G)` slices | No `C/T/P/N`; memory is `O(G)` per in-flight op. Capacity doubles (`src/exec/op.h:63-76`). | OK amortization; RISK incomplete retry correctness/retention |
| Command lookup | `O(12)=O(1)` today; `O(number of command specs)` if expanded | Command names are capped at 8 bytes and the fixed table is linearly scanned (`src/cmd/commands.cc:190-218`). | OK at current fixed surface |
| Hash and route | `O(K)` hash plus `O(1)` bucket, shard-owner, and ROB-counter loads | `K`: word-at-a-time seeded mix or SipHash (`src/store/flatstore.h:213-232`); `T/N/C/P/V`: none. Routing is indexed loads (`src/core/shard.h:137`, `src/core/server.h:148-153`). | OK |
| ROB acquire/publish | `O(1)`; every first touch of an 8-slot chunk constructs 8 `Op`s | `P`: memory `O(P)` at high-water depth, CPU constant because chunk size is 8 (`src/net/rob.h:69-84`, `src/net/rob.h:128-145`). | OK CPU; RISK retention |
| Task-channel push | `O(1)` | `T`: one selected SPSC channel; `P/K/V/N/C`: none. Capacity is 1024 (`src/exec/exqueue.h:34-43`, `src/core/thread.h:40-54`). | OK |
| Dispatch-notification touched list | `O(U)`, where `U` is the number of executors actually fed by this parse pass | `U <= min(P,E)`. It records only first touches and walks the dense touched list (`src/core/io_loop.h:329-345`). | OK; this is the desired work-proportional pattern |
| One `parse_and_dispatch` call | Up to `O(P * (parse + K))` plus actual pushes | `P` is a real-work bound. A phase-1 pass can invoke this for every active connection, yielding `O(A*P)` commands before retirement gets CPU (`src/core/io_loop.h:214-345`, `src/core/io_loop.h:407-453`). | RISK for pass latency |
| Connection-local command | Normal commands `O(B reply)`; `DBSIZE`/`INFO` are `O(number of shards)` | `T/C/P/K/V/N`: normally none; admin calls scan every shard (`src/cmd/commands.cc:87-120`). | OK for ordinary commands; RISK for admin scan |

There is a concrete no-progress bug in the retry path. Phase 1 increments `work` whenever buffered input exists, regardless of whether `parse_and_dispatch` parsed or dispatched anything (`src/core/io_loop.h:416-420`). An incomplete request therefore keeps `did != 0`, takes the busy branch, and prevents the loop from parking even though the only useful event is the outstanding recv. A full worker channel has the same shape: the command is unpublished and left unconsumed (`src/core/io_loop.h:314-325`), then reparsed and rehashed on every active-set pass.

The incomplete-argv lifetime is also unsafe. `Op::reset` leaves `argv_heap_` allocated (`src/exec/op.h:53-60`); the next retry writes its first eight arguments into `argv_inline_`, but `arg()` reads from the old heap whenever `argv_heap_` is non-null (`src/exec/op.h:63-81`). This is triggered by an incomplete command with more than eight arguments and can turn the repeated-parse complexity problem into stale argument reads.

## Retirement, active connections, and send

| Operation/pass | Cost | Axis dependence | Verdict |
|---|---:|---|---|
| `collect_retire_work` channel portion | `O(2 mask words + posted clients)` normally; `O(T+F)` in its unmasked form | `T`: fixed two-word `NotifyMask`; fallback sweep scans all producer channels (`src/core/io_loop.h:368-389`, `src/core/thread.h:142-155`, `src/core/thread.h:217-221`). | RISK on empty/sweep path |
| `collect_retire_work` ready-mask portion | Exactly 16 word probes plus one action per set bit | Independent of actual ready clients until bits are found. It always visits all 1024 possible slots (`src/core/io_loop.h:375-388`, `src/core/signal.h:144-172`). | RISK: possible-work scan |
| `flush_ready` phase 1 | `Theta(A)` before parse/rearm work | `A <= C_i` and there is no configured `C_i` cap. Every active client is visited every pass (`src/core/io_loop.h:396-437`). | RISK: primary law violation |
| Stuck/read-retry test | `O(1)` per active connection, hence `Theta(A)` per pass | A client remains active while waiting on a full ROB, an unavailable recv SQE/read tail, execution, retirement, send completion, close quiescence, or output drain (`src/core/io_loop.h:424-435`). | RISK |
| 64-pass backstop | `Theta(A)` enqueues/checks, then up to `A` later empty serves | Every 64th `flush_ready` flags every active client whether or not it is ready (`src/core/io_loop.h:398-410`). This partially recreates the documented 93%-empty poll-serve failure (`src/core/io_loop.h:375-379`). | RISK: explicit possible-work pass |
| Pending-serve FIFO | At most 16 clients dequeued per pass, but unbounded bytes/ops per client within `P,V` | Queue wait is `ceil(Q/16)` passes. Each of those passes first pays `Theta(A)` phase 1, so CPU work ahead of the tail is `Theta(A*Q/16)`; with `A,Q=Theta(C_i)`, p99 can see quadratic connection-scan work. FIFO prevents starvation only if each serve terminates promptly (`src/core/io_loop.h:439-456`). | RISK |
| ROB drain | `O(R + reply bytes)`, `R <= P`, stops at first non-Done head | `P`: completed in-order prefix; `V`: copied replies. Later Done ops wait behind the oldest Issued op by protocol requirement (`src/net/rob.h:92-113`). | OK complexity; intentional HOL stall |
| Reply staging | Direct reply: `O(1)` publish for bytes already written; spill: `O(reply bytes)` plus geometric-buffer growth copies | `V` and `P*V`; no `C/T/N/K`. The worker may already have copied a GET value to `Op::reply`, and retirement copies it again to the fill buffer (`src/net/wb.h:71-84`). | OK byte linearity; RISK amplification |
| `pump`/send submission | `O(1)` userspace per SQE | Kernel/network work is `O(bytes sent)`. Exactly one send is outstanding (`src/net/wb.h:91-115`). | OK |
| Send completion | `O(1)` plus another `pump` on a short write | `V`: number of completions can grow with short writes; each byte is transmitted once (`src/net/wb.h:117-145`). | OK |
| Active-set iterator erase | `O(1)` swap-with-back | No axis term once iterator is known (`src/core/io_loop.h:529-541`). | OK |
| Pointer-based active erase and client unregister on close | `O(A+C_i)` | Both vectors are linearly searched (`src/core/io_loop.h:465-485`, `src/core/io_loop.h:538-541`). | RISK |
| Dead-list reap | `O(number freed + materialized Ops)` for clients actually freed that generation | Work is proportional to actual corpses; `P` bounds each client's allocated ROB chunks/heaps (`src/core/io_loop.h:498-505`, `src/net/rob.h:126`). | OK complexity; lifetime is unsafe (below) |

### What bounds the active set?

`in_active_` ensures one active-vector entry per client, so `A` is bounded only by the number of allocated, not-yet-dead clients owned by that I/O thread. Truly idle connections that have never produced a recv completion are not inserted, and a fully quiescent/drained connection is removed (`src/core/io_loop.h:190-211`, `src/core/io_loop.h:430-436`). That is the only favorable bound.

“Active” is much broader than “runnable now.” It includes connections waiting for an executor, a head-of-line op, a send completion or slow peer, an SQE/read-buffer opportunity, more bytes of an incomplete command, and closing quiescence. Thus an arbitrary number of idle-*but-active* connections can be present, up to `C_i`, and every unrelated completion makes phase 1 walk them. Per-completed-op cost therefore grows as `Omega(A/progress)` and can be `Theta(C_i)` when only one connection makes progress.

The 16-client FIFO cap does not cap pass time. One `serve` can retire 64 replies and copy `Theta(P*V)` bytes, and phase 1 can parse `Theta(A*P)` commands before the FIFO begins. At high connection counts the queue adds `ceil(Q/16)` turns, while each turn retains the full `A` scan. A byte- or cycle-budgeted, resumable service loop is required for a meaningful p99 bound.

The backstop is unstable as an empty-poll producer once `A/64 > 16`, i.e. above **1,024 active clients per I/O thread**. It can enqueue candidates faster than phase 2 can remove them even if none is ready. With 10,000 active/queued clients on one I/O thread, a client at the tail needs at least 625 passes and those passes perform roughly 6.25 million active-client visits before its turn, excluding real parse/serve work.

## Signalling and every scan

| Scan/path | Bound and cost | Axis dependence | Verdict |
|---|---:|---|---|
| `ReadyMask::set` | `O(1)`, idempotent bit; RMW only on 0-to-1 transition | No scan; there are 1024 ready slots per sender (`src/core/signal.h:144-163`). | OK |
| Ready word take | Caller always takes 16 words | `O(1024/64)`, not proportional to ready words/bits (`src/core/io_loop.h:380-388`). | RISK |
| `ReadyMask::any` | Up to 16 word loads | Sleep-path possible-set scan (`src/core/signal.h:164-167`). | RISK |
| Ready-slot capacity/fallback | First 1024 connections per I/O thread get bit slots; every later connection permanently uses the client-channel path | `C_i`: threshold 1024. Slots are assigned only at accept and never adopted later (`src/core/io_loop.h:190-195`, `src/core/thread.h:229-242`, `src/core/ex_loop.h:154-180`). | RISK at high `C_i` |
| `NotifyMask::set` | `O(1)` | One bit for the actual producer (`src/core/signal.h:174-193`). | OK |
| Masked task/client drain | Two word exchanges, then `O(flagged producers + F)` | `T/64` discovery (hard-coded two words) and actual entries (`src/core/thread.h:120-155`). | RISK for empty fixed scan; OK entry work |
| Channel drain loop | `O(F)` for entries observed, but no per-producer/pass budget | A producer can refill while `while(recv)` runs, so a hot first producer can keep the loop running indefinitely and starve later producers/CQEs/stop checks (`src/core/thread.h:125-155`). | RISK: unbounded pass/fairness |
| Unmasked task/client sweep | `Theta(T+F)` | Visits all producer channels immediately before park (`src/core/thread.h:211-221`). Correctness backstop, but explicitly proportional to possible producers. | RISK |
| `sample_depth` | `Theta(T)` every loop iteration | Reads both queues from every possible producer (`src/core/thread.h:170-177`), including each of the executor's 2047 idle-spin iterations (`src/core/ex_loop.h:52-70`). | RISK: hot-path law violation |
| Loop timing/accounting | Three clock reads per iteration: span start/end plus thread CPU time | `O(1)`, but it is paid on every empty spin/pass and amplifies their fixed scans (`src/core/signal.h:38-51`, `src/core/signal.h:92-103`, `src/core/io_loop.h:98-108`). | RISK overhead, constant complexity |
| Arm blocked | `Theta(T)` stores | Arms both channels for every producer (`src/core/thread.h:179-190`). | RISK on park path |
| Clear blocked | `Theta(T)` stores | Same all-channel walk after every wake/timeout (`src/core/thread.h:191-194`). | RISK |
| `any_inbound` | 16 ready-word loads + 4 notify-word loads + worst `2T` depth loads | The final channel scan is taken precisely when masks say there is no work (`src/core/thread.h:255-260`). | RISK |
| Touched list | `Theta(U)` | `U` actual executor targets, bounded by `min(P,E)`; no 128-slot scan (`src/core/io_loop.h:329-345`). | OK |
| Dead-list generations | `Theta(D)` | Only clients actually awaiting deletion (`src/core/io_loop.h:498-505`). | OK |
| Active/stuck scan | `Theta(A)` | Potentially runnable connections, not progress (`src/core/io_loop.h:407-437`). | RISK |
| Per-batch shard publication | `Theta(S_w)` after each batch of at most 32 actual tasks | It publishes every shard owned by the executor, not shards touched by the batch (`src/core/ex_loop.h:109-118`). Cost per op is `Theta(S_w/32)`. | RISK |
| Store `for_each` / destructor | `Theta(M)` slot scan, plus callbacks/frees for `N` live entries | Empty slots are possible work only (`src/store/flatstore.h:132-139`, `src/store/flatstore.h:198-204`). | RISK, especially teardown |

There is no periodic expiry scan or other TTL background worker in this tree. The recurring passes are the loop depth sample, active phase, ready/notify discovery, pre-park unmasked sweep, 64-pass serve backstop, dead generations, all-shard batch publication, and operation-driven rehash.

## Executor and command work

| Operation | Cost | Axis dependence | Verdict |
|---|---:|---|---|
| Executor loop iteration | `Theta(T)` depth sampling + mask/CQE/task work | `T` dominates empty iterations (`src/core/ex_loop.h:52-68`, `src/core/thread.h:170-177`). | RISK |
| Gather/prefetch batch | `O(n)`, `n <= 32` | One ROB lookup and up to two table-slot prefetches per actual task (`src/core/ex_loop.h:94-114`, `src/store/flatstore.h:206-211`). | OK |
| Execute/notify | Handler cost + `O(1)` state publish and signal | `K/V/N` come from handler/store; ready path is constant (`src/core/ex_loop.h:120-165`). | OK aside from store |
| `GET` | Hash already paid; expected probe `O(1)`, key compare `O(K)` on tag match, reply `O(V)` | Worst `O(M*K + V)` under a full probe cluster, which is `O(N*K+V)` only while capacity tracks keyspace; rehash adds unrelated-key work (`src/cmd/commands.cc:30-36`). | RISK worst-case; OK expected bytes |
| Same-class `SET` overwrite | Probe plus `O(V)` copy | `K` on comparisons, `V` exact overwrite, `N` via probe. Allocation-free (`src/store/flatstore.h:165-178`). | OK expected; RISK worst-case |
| Slow `SET` | `O(K+V)` allocation/copy + expected probes | External values use two allocations; replacement frees old object (`src/store/kvobj.h:132-164`, `src/cmd/commands.cc:38-53`). | OK byte linearity; RISK allocator/rehash |
| `DEL`/`EXISTS` | Expected `O(1)` probes plus `O(K)` on matching tags; worst `O(M*K)` | Delete frees in `O(1)` allocator work and leaves a tombstone (`src/cmd/commands.cc:55-63`, `src/store/flatstore.h:295-313`). | RISK worst-case/tombstones |
| `INCR` | Probe + at most 20-byte parse + allocate/copy key and decimal value + insert | `K` copied; `V > 20` rejects in constant bounded parsing (`src/cmd/commands.cc:18-27`, `src/cmd/commands.cc:65-85`). | RISK: needless allocation every update |
| `DBSIZE`/`INFO` | `Theta(number of shards)` | Independent of `N` because sizes are published, but reads all shard counters (`src/cmd/commands.cc:94-120`). | RISK under frequent admin traffic |

## FlatStore

| Operation/pass | Cost | Axis dependence | Verdict |
|---|---:|---|---|
| Key hash | `Theta(K/8)` mix rounds or `Theta(K/8)` SipHash blocks | Strictly linear in key bytes read (`src/store/flatstore.h:84-107`, `src/store/flatstore.h:221-232`). | OK |
| `find_in` | Expected constant probes at controlled occupancy; worst `Theta(M)` probes and `Theta(M*K)` full-key comparisons | Probe loops are explicitly capped at a table revolution (`src/store/flatstore.h:253-265`). | RISK worst-case |
| `insert_into` | Expected constant; worst `Theta(M*K)` | Tracks first tombstone but must continue to EMPTY or matching key (`src/store/flatstore.h:267-293`). | RISK worst-case |
| `erase_in` | Expected constant; worst `Theta(M*K)` | Tombstones never terminate a probe (`src/store/flatstore.h:295-313`). | RISK worst-case |
| Lookup while rehashing | Up to two probe chains plus one rehash step | `N/M`: current then old table (`src/store/flatstore.h:154-159`). | RISK tail amplification |
| Grow/cleanup trigger | Intended `O(1)` decision at 70% effective occupancy | Counts tombstones, but `(live+tombs+1)*100`, `cap*70`, and the grow-choice products are all 32-bit and wrap (`src/store/flatstore.h:315-324`). | **CRITICAL RISK: invalid above 61.36M slots** |
| Shrink trigger | Intended `O(1)` decision after successful erase, halving below 17.5% live | The 32-bit products also wrap; even with corrected arithmetic only one shrink can be pending (`src/store/flatstore.h:326-332`). | **CRITICAL RISK at scale; RISK retention** |
| Start rehash | Source-level `O(1)` pointer swap plus `calloc(newM*8)` | Virtual allocation/zeroing commitment is `Theta(newM)` and can be a multi-GiB latency/OOM event on the triggering op; allocation failure is unchecked (`src/store/flatstore.h:245-251`, `src/store/flatstore.h:334-343`). | RISK |
| One `rehash_step` | Scans at most 8 old slots, but each live slot costs `Theta(K_old + probes)` | The *slot count* is bounded, not CPU cycles or bytes: hashes of up to eight unrelated keys are recomputed and inserted (`src/store/flatstore.h:345-363`). Worst `O(8*(K_max+M))`. | RISK |
| Whole rehash | `Theta(oldM + sum of rehashed key bytes + insertion probes)` spread over later store calls | Duration is at least `oldM/8` store calls, or about `oldM/16` new `SET`s because `try_overwrite/find` and `insert` each step (`src/store/flatstore.h:154-188`, `src/store/flatstore.h:348-369`). | RISK cold-table retention |
| Tag filter | One 15-bit compare per live probed slot; key bytes only on a tag hit | Under uniform hashes a false tag hit is about `1/32768`; tags do not shorten the probe, only avoid pointer/key reads (`src/store/flatstore.h:45-53`, `src/store/flatstore.h:253-264`). | OK expected-case optimization |
| Delete/tombstone cleanup | Tombstone now; cleanup on future insert-triggered same-size rehash | Effective occupancy stays near/below 70% in stable tables, but tombstones and oversized capacity can persist with no inserts (`src/store/flatstore.h:306-323`). | RISK retention |
| Destruction / `for_each` | `Theta(M+N)` | Scans empty/tomb slots and frees live objects (`src/store/flatstore.h:132-139`, `src/store/flatstore.h:198-204`). | RISK teardown |

For ordinary uniform linear probing, the standard estimates at `alpha=0.70` are about `0.5*(1+1/(1-alpha)) = 2.17` probes for a successful search and `0.5*(1+1/(1-alpha)^2) = 6.06` for an unsuccessful search/insertion. Those are expectations, not bounds. Tombstones count in `alpha`; the 15-bit tag reduces expensive key dereferences but does not reduce probe length. The boot-randomized hash makes offline collision construction harder, and SipHash is available (`src/store/flatstore.h:68-107`), but worst-case table-length runs remain representable. More importantly, the code cannot rely on either expectation after its load-factor arithmetic wraps.

### The 100M-key interaction

**The actual implementation has no valid 100M-key complexity bound.** `cap_[0]` and the live/tomb counters are `uint32_t` (`src/store/flatstore.h:371-377`). In `cap*70`, 32-bit wrap starts above `floor(UINT32_MAX/70) = 61,356,675`; the first affected power-of-two capacity is `2^26 = 67,108,864`. The left-side `live*100` and grow-choice `live*200` products wrap too. Resize decisions then become non-monotonic: the code can start premature same-size rehashes, choose cleanup instead of doubling, and allow effective occupancy beyond 70%. Probe expectations and the claim that insertion failure is unreachable (`src/store/flatstore.h:292`) no longer follow. All products must be widened before a 100M-key run is meaningful.

Counterfactually, with corrected arithmetic and the intended 70% policy, growth from `2^27` slots starts at about 93,952,409 live keys. The new `2^28` table is 2 GiB of slot words and the old table is 1 GiB, so slot arrays alone occupy 3 GiB during this resize. Bulk-loading from that trigger to 100M adds about 6.05M new keys. A slow-path new `SET` advances 16 old slots (one step in `find`, one in `insert`), so only about 96.8M of 134.2M old slots have been examined by 100M keys; roughly 37.5M slots, or another 2.34M new `SET`s, remain before the old 1 GiB array is freed. A read-only workload advances only 8 slots per command and needs 16.78M lookups for the full rehash.

During that intended resize every lookup may probe two tables and every command may hash/insert up to eight unrelated resident keys per step. If the shard becomes cold, progress stops completely and the old table remains allocated while still participating in lookups. The next growth scale (`2^28` old plus `2^29` new) would hold 6 GiB of slot arrays.

Deletion has a second stale-capacity failure. Once a shrink starts, all further `maybe_start_shrink` calls return while rehashing. If deletes make the table much smaller and later reads finish that rehash, completion does not re-evaluate the target size; with no later successful erase, a nearly empty table can remain at half of its former huge capacity indefinitely. If activity stops before completion, both arrays remain.

## Growth and retention policies

| Allocation | Growth/amortization | Shrink/release policy | Stale-memory verdict |
|---|---|---|---|
| Connection read buffer | Geometric `realloc`, amortized linear in received bytes (`src/net/conn.h:103-115`) | Only an **empty** buffer with capacity `> 1 MiB` returns to 16 KiB (`src/net/conn.h:133-151`). | **RISK:** any high-water capacity up to and including 1 MiB is permanent until close. |
| Two connection write buffers | `SmallBuf` doubles and copies current length; amortized linear in appended reply bytes (`src/base/slice.h:76-105`) | At quiescence, only empty buffers with capacity `> 64 KiB` shrink (`src/net/conn.h:138-143`). | **RISK:** capacities through 64 KiB persist. Worse, a fully sent send buffer retains nonzero `size()` until a future fill/send swap, so a `>64 KiB` block can evade shrink forever on a connection that goes idle (`src/net/conn.h:174-183`). |
| `Op::reply` | Doubles from 112 bytes; amortized linear in reply bytes (`src/base/slice.h:50-105`) | Retire shrinks only when capacity is `>4096`; actual doubling permits 3584-byte heaps to persist in every materialized slot (`src/exec/op.h:84-92`, `src/net/rob.h:100-105`). | **RISK:** up to 3584 heap bytes per `Op` can become stale. |
| `Op` argv | Doubles from 16 slices; amortized `O(1)` per arg (`src/exec/op.h:63-76`) | Any spilled argv is freed at successful retire (`src/exec/op.h:84-92`). | OK after retire; **RISK** for incomplete retries because reset retains the heap and then reads stale entries. |
| ROB chunks | Eight contiguous 336-byte `Op`s allocated on first slot touch (`src/net/rob.h:128-141`) | No chunk is freed until connection destruction (`src/net/rob.h:126`). | **RISK:** peak pipeline depth is retained permanently; a p64-then-p1 connection keeps seven unused chunks. |
| Ready slot table/free list | `std::vector` geometric growth, amortized `O(1)`, hard cap 1024 (`src/core/thread.h:224-242`) | Entries recycle; neither vector shrinks. A closed high-water population leaves both slot storage and a large free-index vector. | **RISK**, small per I/O thread. |
| Task/client channels | No growth: each `ThreadCtx` allocates two 1024-entry channels for every thread (`src/core/thread.h:62-71`) | Never released until server teardown. | **RISK:** `Theta(T^2)` permanent memory; most direction/role pairs are impossible in pure 2s. |
| Active/client/dead vectors | Geometric vector growth, amortized `O(1)` insertion (`src/core/io_loop.h:195`, `src/core/io_loop.h:520-542`) | `clear`, `pop_back`, and swaps retain capacity. | **RISK:** connection/churn high-water pointer arrays persist. |
| Pending-serve deque | Amortized constant end operations (`src/core/io_loop.h:445-463`) | Implementations release old blocks opportunistically but retain at least deque bookkeeping/blocks; no explicit trim. | RISK, secondary. |
| FlatStore slots | Power-of-two doubling/same-size cleanup/halving; migration is incremental (`src/store/flatstore.h:315-369`) | Old table freed only after operation-driven cursor completion; only one half-shrink is initiated per triggering successful erase. | **RISK:** multi-GiB cold old arrays and oversized post-delete tables can be permanent. |
| Key/value objects | One object allocation, plus a second value block above 192 bytes; freed on replacement/delete (`src/store/kvobj.h:113-197`) | No cache or freelist in this layer. | OK: live object bytes represent live keys/values, modulo allocator arena retention outside this code. |

No output-byte budget exists. The ROB caps operation count, not reply bytes. Up to 64 large GET replies can reside in separate `Op` buffers and then be copied into one fill buffer; memory is `Theta(P*V)` per connection, with temporary duplication during drain. Protocol bulk length permits about 512 MiB, so the theoretical per-connection reply footprint is tens of GiB. `wsent_` is only 32 bits (`src/net/conn.h:172-174`, `src/net/conn.h:253-254`), so staging more than 4 GiB is also a correctness boundary, not merely an RSS risk.

## Spin, stall, park, and wake census

| Site | Bound/behavior | Work can exist while waiting? | Verdict |
|---|---|---|---|
| Executor idle spin | 2047 loop iterations with `pause` before each park (`src/core/ex_loop.h:27-33`, `src/core/ex_loop.h:48-76`) | Each “spin” also pays clocks, two-word mask work, CQ inspection, and `Theta(T)` depth sampling. Repeats after every 50 ms timeout. | RISK: bounded but not cheap/purposeful at 128 threads |
| I/O idle path | No intentional pause spin; sweep, arm, recheck, then timed ring wait (`src/core/io_loop.h:114-128`) | Correctly parks when truly empty, but the no-progress parse accounting prevents this for partial/full-channel clients. | RISK due busy-poll bug |
| No-progress partial request | Unbounded loop until another recv CQE, while repeatedly reparsing and scanning `A` | Yes: it is waiting for bytes but burns the core because `work++` is unconditional (`src/core/io_loop.h:416-420`). | **RISK: unbounded busy-poll** |
| Full task channel retry | Reparse/hash/publish/unpublish on every active pass until executor makes room (`src/core/io_loop.h:314-325`) | Yes. No producer pause, low-water wake, or retry budget. | **RISK: unbounded busy retry** |
| Masked channel drain | `while(recv)` with no item/cycle budget (`src/core/thread.h:125-155`) | Other producers, CQEs, and stop can wait behind a continuously refilled first channel. | **RISK: unbounded pass/starvation** |
| Quiet task batching | Worker notification delayed until the entire connection parse pass ends (`src/core/io_loop.h:329-345`) | Yes: pushed tasks may sit while up to `P` commands, or a large parse, completes. Bounded in ops by 64, not bytes/time. | RISK, intentional batching tradeoff |
| ROB ordering | Done younger ops wait for oldest non-Done op (`src/net/rob.h:92-113`) | Yes, but executing them would violate response order; CPU remains free for other clients. | OK/intentional |
| Task/client park protocol | Producer pushes, sets mask with seq-cst transition, then wakes only a blocked peer; consumer arms, fences, and rechecks (`src/core/signal.h:223-265`, `src/core/thread.h:179-200`) | The ordering is defensively designed; unmasked sweeps and a 50 ms timeout prevent permanent sleep. | OK correctness; RISK scan cost |
| Ready-mask park protocol | Worker fences after Done, sets idempotent bit, and wakes a parked sender (`src/core/ex_loop.h:147-165`, `src/core/thread.h:246-252`) | No obvious lost-wakeup window in the stated ordering. | OK |
| `msg_ring` wake submission | `msg_to` can return false, but callers ignore it and still count a wake (`src/net/uring.h:126-135`, `src/core/signal.h:241-245`, `src/core/thread.h:248-252`) | Yes. State remains in masks/queues, so the 50 ms timeout recovers; tail latency, not permanent loss. | RISK |
| Fallback client-channel full | Completion post clears the claim and returns; that completed op will not call notify again (`src/core/ex_loop.h:172-183`) | Yes. Progress relies on the I/O active/backstop polling path, not an explicit retry. | RISK |
| Ring wait | Up to 50 ms (`src/net/uring.h:96-110`) | Masks/queues are rechecked before wait. A dropped wake or staged-send/SQE retry can wait for timeout/backstop; stop detection is also delayed by at most the timeout when not stuck elsewhere. | RISK tail, bounded |
| Serve FIFO | `Q/16` passes; each pass first scans `A`; service is not byte-budgeted (`src/core/io_loop.h:396-456`) | Yes: ready clients wait behind earlier clients and all phase-1 work. | RISK p99 |
| SQE-starved first recv | No retry at all because the accepted client has not entered `active_` (`src/core/io_loop.h:147-160`, `src/core/io_loop.h:190-196`) | Yes: the server owns an open socket it could arm, but no event/work queue names it again. | **RISK: permanent stall** |
| SQE-starved send retry | Bytes stay staged; no explicit retry queue (`src/net/wb.h:106-114`, `src/net/wb.h:148-159`) | Yes. It is rediscovered via active/backstop service, potentially after many timed parks. | RISK |

There is no syntactically unbounded `pause` loop. There are nevertheless two semantic unbounded spins: the incomplete/no-progress I/O path and the queue-full reparse path. The refillable channel drains are unbounded work loops rather than pause loops, but have the same starvation consequence.

The park/wake memory ordering is substantially better than the surrounding complexity: take-before-drain, push-before-flag, the seq-cst transition, arm-before-recheck, unmasked fallback, and timed wait collectively avoid a clear permanent lost wake. The remaining weaknesses are ignored wake-submission failure and fallback progress that depends on periodic polling. After a full fallback completion channel drops a client's only notification, an otherwise idle loop can take the 50 ms timeout once per backstop tick, making the nominal 64-pass recovery as large as about **3.2 seconds**.

## Boot, close, and teardown

| Operation | Cost | Axis dependence | Verdict |
|---|---:|---|---|
| Topology discovery | `O(CPU_SETSIZE + CPUs * cache-index/sysfs parsing)` | Boot only (`src/base/topology.h:83-103`, `src/base/topology.h:126-166`). | OK |
| Placement construction | Usually `O(T+shards)`; uneven-domain fix-up can scan domains repeatedly | Bounded by `T<=128`, boot only (`src/core/placement.h:37-118`, `src/core/placement.h:170-220`). | OK |
| Empty shard/store creation | `Theta(shards * 1024)` zero-slot allocation | Each `FlatStore` starts with 1024 slots although minimum is 64 (`src/store/flatstore.h:121-131`, `src/core/server.h:101-109`). | RISK for empty-shard memory, small by default |
| Router build | `Theta(16384)` | Fixed routing bucket count (`src/core/shard.h:126-142`). | OK |
| Channel mesh creation | `Theta(T^2 * 1024)` bytes initialized and `Theta(T^2)` channel objects | Every thread receives task and client channels from every possible producer (`src/core/thread.h:62-71`, `src/core/server.h:112-121`). | **RISK: quadratic boot and RSS** |
| Ring/listener boot | One 4096-entry ring/listener per I/O thread, one 1024-entry ring per executor | `O(I+E)` setup plus ring mapping sizes (`src/core/io_loop.h:59-66`, `src/core/ex_loop.h:38-43`). | OK |
| Worker-before-I/O launch | `O(T)` thread creation | There is no readiness barrier; executor threads are created first but may not have initialized their rings before I/O starts. Queue/mask state prevents loss, but a failed executor `init` silently returns and leaves a permanently routed dead worker (`src/main.cc:203-233`). | RISK startup failure/stall |
| Normal per-connection close | `Theta(A+C_i+P)` plus frees | Linear active/client searches, ROB chunk destruction, buffer frees (`src/core/io_loop.h:465-505`, `src/net/rob.h:126`). | RISK |
| Deferred dead generation | Nominally two loop generations | It protects channel pointers only by elapsed passes, not by proving all kernel/FIFO references are gone (`src/core/io_loop.h:489-505`). | **RISK lifetime** |
| Signal stop | `Theta(T)` atomic stores | Signal handler walks all thread pointers (`src/main.cc:28-34`). | OK scale; async-signal semantics not audited here |
| Join latency | 50 ms when parked, unbounded behind a long probe/copy/refillable drain | `T`, `N`, `K`, `V`, and `F` can delay stop checks (`src/main.cc:235`, loop bodies above). | RISK |
| Shutdown diagnostics | `Theta(T+C+C*P)` | Scans every live client and every unretired ROB slot (`src/main.cc:237-336`). | RISK at large `C`, teardown only |
| Store destruction | `Theta(sum M + N)` | Scans every current/old slot and frees live objects (`src/store/flatstore.h:132-139`). | RISK teardown tail |
| Live connection teardown at process stop | Effectively no explicit cleanup | `ThreadCtx::clients_` holds raw pointers and does not delete them; loops stop immediately, then server destruction drops the vectors. The OS ultimately reclaims clients/fds/buffers (`src/core/thread.h:284-286`, `src/main.cc:235-338`). | **RISK: no graceful drain/close** |

The deferred-free proof is incomplete in two independent ways:

1. `safe_to_release()` checks only ROB quiescence and `retire_queued_`, not `recv_armed`, `send_inflight`, or queued serve membership (`src/net/conn.h:212-236`). A recv EOF can close/delete a client while a send CQE and kernel buffer reference are still outstanding; two loop iterations are not a completion fence.
2. `close_client` does not remove the client from `pending_serve_`. With more than 32 entries ahead, the client can be deleted after two passes while its FIFO pointer survives; phase 2 dereferences it before checking `dead()` (`src/core/io_loop.h:445-452`, `src/core/io_loop.h:489-505`).

These are correctness hazards, but they also prevent a defensible per-close complexity/lifetime bound.

## Memory accounting

### Per connection

Measured structural sizes on this ABI:

| Component | Bytes | Scaling/source |
|---|---:|---|
| `Client` struct, including ROB metadata and two 512-byte inline write buffers | 1,408 payload; default jemalloc class about 1,536 | `src/net/conn.h:246-275`; `SmallBuf<512>` fields are in-struct (`src/base/slice.h:50-109`). |
| Initial read buffer | 16,384 | One allocation per connection (`src/net/conn.h:49-58`, `src/net/conn.h:74-77`). |
| One ROB chunk | `8 * sizeof(Op) = 2,688` payload; default `new[]` allocation class about 3,072 | `sizeof(Op)=336`; chunks at `src/net/rob.h:128-141`. |
| Maximum eight materialized ROB chunks | 21,504 payload; about 24,576 allocated | Linear in `P`, retained to close. |

Therefore:

- Accepted but never dispatched: 17,792 payload bytes (about 17,920 with default size classes).
- Ever-used p1 connection: 20,480 payload bytes (about 20,992 allocated).
- All 64 slots materialized: 39,296 payload bytes (about 42,496 allocated), before any heap argv/reply/write growth.

Dynamic memory is not safely bounded by these numbers:

- Read capacity is up to about 512 MiB for one incomplete command and can retain 1 MiB after becoming empty.
- Each write buffer grows geometrically; capacities through 64 KiB are retained, and the fully sent buffer bug can retain larger blocks.
- Each materialized `Op` can retain a 3584-byte reply heap. At 64 slots that is 229,376 stale bytes per connection.
- An argv spill can reach roughly 16 MiB for the 1M-argument protocol limit per op until retirement.
- In-flight completed replies are `Theta(P*V)` with no byte budget; large reply staging temporarily duplicates them.

At 10,000 connections, connection payload/default-class totals are:

| State | Payload | Approx. default-jemalloc allocation classes |
|---|---:|---:|
| Never dispatched | 169.68 MiB | 170.90 MiB |
| Ever p1 | 195.31 MiB | 200.20 MiB |
| All 64 ROB slots materialized | 374.76 MiB | 405.27 MiB |

Independent retained peaks at 10,000 connections can add about 9.61 GiB if every read buffer sticks at 1 MiB rather than 16 KiB, 1.21 GiB if both write buffers stick at 64 KiB rather than inline, and 2.14 GiB if all 64 reply buffers stick at 3584 bytes.

### Per thread and channel mesh

Measured sizes:

| Component | Per-thread bytes at `T=128` | 128-thread total | Notes |
|---|---:|---:|---|
| `ThreadCtx` object | 512 | 64 KiB | Includes 128-byte `ReadyMask`, two 16-byte `NotifyMask`s, vectors, and signals (`src/core/thread.h:265-286`). |
| 128 `TaskChan`s | 2,121,728 payload | 259.0 MiB payload server-wide | One `TaskChan` is 16,576 bytes: 1024 16-byte tasks plus queue/blocked padding. |
| 128 `ClientChan`s | 1,073,152 payload | 131.0 MiB payload server-wide | One `ClientChan` is 8,384 bytes: 1024 pointers plus queue/blocked padding. |
| Both channel arrays | 3,194,880 payload (3.046875 MiB) | **408,944,640 bytes = 390 MiB payload** | Default jemalloc rounds the two per-thread arrays to 2.5 MiB + 1.25 MiB, or **3.75 MiB/thread and 480 MiB total**. |
| Loop objects allocated by `main` | `sizeof(IoLoop)+sizeof(ExLoop)=1,496` for every thread id | 187 KiB payload; about 208 KiB default classes | Both arrays are allocated at full `T` even though each id uses one role (`src/main.cc:203-206`). |
| Ring mapped entry payload | I/O: about 400 KiB; EX: about 100 KiB | About 31.25 MiB for a 64/64 split | Estimate uses 64-byte SQEs, default 2x 16-byte CQEs, and 4-byte SQ indices; excludes ring headers and kernel request/accounting memory. Ring sizes are 4096/1024 (`src/core/io_loop.h:63`, `src/core/ex_loop.h:40`). |
| Ready slot/free vectors | `O(min(C_i,1024))` pointers/indices, retained at high water | At most roughly 12 KiB payload per I/O thread when both vectors hold 1024 elements over their lifetimes | `src/core/thread.h:224-242`. |
| Client/active/pending/dead pointer containers | `O(C_i)` high-water capacity | `O(C)` | Capacities/churn history are retained; pending FIFO can hold one entry per client. |

In balanced pure 2s, only `I*E` task directions and `E*I` client directions are valid. At `I=E=64`, only 8192 of the 32768 allocated channel objects (25%) correspond to a possible direction; the other 75% reserve queue slots for role pairs that cannot occur in the current architecture. More skewed role splits waste a still larger fraction.

### 10k connections / 128 threads

For a concrete reproducible total, assume 64 I/O + 64 executor threads, 16 default shards, even connection distribution, every connection has materialized one ROB chunk, and there are no heap argv/reply/write spills:

| Item | Payload estimate | Default-jemalloc / mapped estimate |
|---|---:|---:|
| 10k p1 connections | 195.31 MiB | 200.20 MiB |
| Channel mesh | 390.00 MiB | 480.00 MiB |
| `ThreadCtx` objects | 0.063 MiB | 0.063 MiB |
| Both loop arrays | 0.183 MiB | 0.203 MiB |
| 64 x 4096 + 64 x 1024 ring entry mappings | 31.25 MiB | 31.25 MiB plus kernel metadata |
| Router + 16 empty initial shard tables/objects | 0.190 MiB | about 0.190 MiB |
| **Subtotal** | **about 617.0 MiB** | **about 711.9 MiB** |

This subtotal excludes allocator metadata/arenas, thread stacks, slot/client/active container capacities, socket/kernel buffers, ring headers/kernel state, stored keys/values, and all dynamic reply/argv/buffer growth. With never-dispatched connections the same subtotal is about 591.4 MiB payload / 682.6 MiB default classes; with all ROB chunks materialized it is about 796.4 MiB payload / 917.0 MiB default classes.

## Ranked top 10 risks and concrete fixes

1. **Eliminate the `Theta(A)` active pass and connection-count-amplified FIFO.** Replace `active_` with intrusive, deduplicated queues for exact reasons: parse retry after ROB/channel low-water, recv rearm, ready retirement, output retry, and close quiescence. Enqueue only on the state transition that makes work possible. Give parsing, retirement, and output independent op/byte/cycle budgets. This removes `Theta(A*Q/16)` p99 scan work and makes each pass proportional to dequeued work.

2. **Stop no-progress busy looping and make parsing resumable.** Return a progress/result record from `parse_and_dispatch` (bytes consumed, ops dispatched, blocked reason); increment `did` only on real progress/SQE submission. Retry incomplete input only on recv completion. Retry a full executor channel from a consumer low-water notification, not every I/O pass. Preserve a RESP parse cursor/argument state so fragmented many-argument requests are `O(B)`, and either free/reset the incomplete argv heap or make all accesses consistently use it.

3. **Remove `Theta(T)` work from every loop iteration and park transition.** Maintain consumer-local aggregate depth/nonempty producer counts on queue empty/nonempty transitions, or sample depth on a coarse timer with rotating subsets. Use one parked flag per consumer, not two stores per producer channel. Add a nonzero-word summary so ready/notify discovery visits only words that became nonempty. Keep a rare diagnostic full sweep, but put it on a timed maintenance budget rather than every park.

4. **Replace the eagerly materialized `Theta(T^2)` mesh.** Allocate channels only for valid `Ifid -> Ex` and `Ex -> Ifid` pairs, preferably lazily on first use. If future role flipping truly requires new edges, allocate/retire them during the flip under its quiescence contract. At a balanced 128-thread split, role-valid allocation removes about 75% of the mesh (292.5 MiB payload / roughly 360 MiB of the current default allocation); lazy allocation can remove more and avoids quadratic zero-initialization at boot.

5. **Fix FlatStore's 32-bit scale failure, then make resize latency genuinely bounded.** Widen every load/shrink product and capacity decision to checked `uint64_t`/`size_t`; assert `live+tombs < cap`, reject an undoublable capacity, and test the `2^26`, `2^27`, and 100M-key transitions. A single multi-GiB `calloc` is not constant-tail work, so use segmented tables or incrementally allocated extents. Cache enough hash information to move a slot without rereading an arbitrary-length key, or budget rehash by bytes/cycles rather than eight slots. Allow at most one step per client command, progress cold rehashes from a bounded maintenance queue, and handle allocation failure without installing a null table.

6. **Put hard byte budgets on replies and service.** Track per-connection and per-I/O-thread outstanding reply bytes; stop dispatching reads before `P*V` can explode. Make ROB drain resumable with both op and byte limits, and make the FIFO budget bytes/cycles rather than clients. Clear or widen `wsent_` to `size_t/uint64_t`. For large GETs, use stable store-object lifetime plus zero-copy/send references or chunked output rather than `Op reply -> fill buffer` duplication.

7. **Enforce zero stale high-water memory.** At connection quiescence, shrink an empty rbuf to its current working target (ultimately 16 KiB), clear a fully sent send buffer immediately, and shrink both write buffers independently. Free unused ROB tail chunks when observed pipeline depth decays; free all reply heaps on idle/quiescence rather than retaining 3584 bytes per slot. Trim slot/client/active/dead vectors after churn. After rehash completion, recompute the target capacity and chain additional incremental shrinks until capacity matches live keys.

8. **Budget and round-robin executor/channel work.** Drain at most an op/cycle quota from one producer, re-set its nonempty bit if entries remain, and rotate the next producer. Inspect CQEs and the stop flag between batches. Publish size only for shards actually touched (a dense touched-shard list, like the good dispatch touched-worker list), not every `S_w` shard after each 32-op batch.

9. **Make close `O(1)` and lifetime-proven.** Store indices or intrusive links for both the owning client list and active/retry queues. Remove/cancel a queued serve entry in constant time. On close, cancel recv/send requests and wait for their CQEs (or use generation-tagged stable handles) before deletion; include kernel I/O and serve-queue ownership in `safe_to_release`. A fixed two-loop delay is not a reference proof.

10. **Make wake failure and shutdown explicit state machines.** Check `msg_to` and retain a retryable wake flag; do not count a failed submission as a wake. On a full completion channel, enqueue the sender in a dedicated retry structure rather than relying on the 64-pass poll backstop. Propagate thread/ring initialization failure to `main`. Shutdown should stop accepts, drain/cancel I/O and task channels, close/delete every client, then join and destroy stores; this gives bounded, auditable teardown instead of OS-reclaimed leaks and arbitrary stranded work.

The highest-return sequence is 1-4 first: it removes the connection/thread axes from the common loop and releases hundreds of MiB without changing store semantics. Items 5-7 then establish byte and memory bounds; 8-10 close fairness, wake, and lifetime gaps.

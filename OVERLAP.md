# Overlap and local reads

> Integration update (2026-09-08): [ORTHOG.md](ORTHOG.md) supersedes this branch report
> for combined-mode support, INFO activation evidence, layout accounting, and validation.
> Both modes now support both local-read settings with either overlap/reorder setting;
> `Config` retains the required 624-byte stride. Historical branch findings below remain intact.

Source audit and diff, 2026-09-08. The input checkout is `c8e61f646`, the configuration
reduction merge after the supplied `78c3e5391` baseline. No build, server, benchmark,
or gate was run. References below retain the overlap-only diff's line numbers unless marked **PRE**,
which means `c8e61f646`. This is a source argument awaiting the maintainer's runtime checks.

## Outstanding hazards

**Follow-up:** [REORDER.md](REORDER.md) fixes the scheduler storage defect below and
restores the public name `--reorder 0|1`. This paragraph records the overlap-only
finding; its temporary exclusion no longer applies. Fused overlap
uses `kGenthreadPipelineExBatchOps = 128` (`src/core/genthread_pipeline.h:19`). Both
`drain_tasks()` and `drain_tasks_with_filler()` gather whole batches with that bound
(`src/core/ex_loop.h:2099`, `src/core/ex_loop.h:2122`). When `--x-ex-sched 1` is enabled,
the latter calls `ex_schedule_batch(batch, held)` at `src/core/ex_loop.h:2129`, and the
ordinary path does so at `src/core/ex_loop.h:2768`. There is no intervening 32-task clamp.

`ex_schedule_batch()` declares `base_lengths[kExecBatch]` and indexes it with `begin`
and `end` up to the caller's `n` (`src/core/ex_loop.h:2307`). `kExecBatch` is 32 and
statically capped at 32 (`src/core/ex_loop.h:46`, `src/core/ex_loop.h:72`). A run of
more than 32 eligible tasks therefore writes past this stack array. The callee also
has `keys[kExecBatch]` and `ordered[kExecBatch]` (`src/core/ex_loop.h:2187`,
`src/core/ex_loop.h:2299`); repairing only the first array would be insufficient.
One connection's 64-slot ROB can already supply more than 32 tasks; multiple
connections can fill 128. This affects overlap with either read-local setting and
both the coarse and deep turns. `ex_sched` defaults to 0 (`src/core/config.h:328`),
so overlap alone did not activate the overflow. The overlap-only diff left the
scheduler arrays unchanged and required its control to stay off. The follow-up
sizes all scratch from the caller's 32/128-task array, expands the connection-table
occupancy mask, and schedules the complete batch without truncation.

Two inherited discrepancies also limit the claims here:

- The overlap/reorder restoration itself kept the incoming Config at 528 bytes,
  versus the context's original 624. The final surface reduction now explicitly
  accounts for **488 bytes** in [DESIGN-KNOBS.md](DESIGN-KNOBS.md); it removes cold
  configuration fields. Other named layout locks remain unchanged. None has been
  compiled in this worktree by Codex.
- `src/store/read_local_settax.h:17` defaults to immutable variant 0, but its
  compile-time variants 1 and 3 permit sequence-protected in-place overwrites
  (`src/store/flatstore.h:2702`, `src/store/flatstore.h:2773`). Those pre-existing
  experimental builds conflict with the stated immutable/no-seqlock laws and are
  outside this coexistence claim. They are unchanged and must not be selected to
  validate this diff. In variant 0, `try_overwrite_read_local()` returns
  `NotPossible` before either overwrite implementation.

## Why the exclusion existed in practice

The old predicate had no rationale beyond the restriction itself (**PRE**
`src/core/server.h:591`). The source does not establish the original author's intent.
It does establish incomplete integration, rather than an inherent conflict between
overlap and immutable local reads:

1. **Admission was compiled out twice.** Besides the server predicate, the parser's
   `Fused` constant required the 32-operation, non-targeted, non-private-queue shape
   (**PRE** `src/core/io_loop.h:3908`). The live overlap IFID instantiation is
   targeted, uses the private queue, and has an uncapped per-connection parser
   (`src/core/io_loop.h:5628`, `src/core/io_loop.h:5735`). Changing only the predicate
   would allocate and arm the read-local structures but leave every GET on the
   owner path. It would also omit the local-read ROB acquisition, write tracking,
   MGET fence, and demotion code from that parser specialization.
2. **The overlap IO loop omitted QSBR park/resume and pre-teardown publication.**
   Compare **PRE** `src/core/io_loop.h:719` with **PRE** `src/core/io_loop.h:939`.
   Executor passes already publish ticks, but an idle thread would remain an active
   participant with an old tick during the network wait. The grace floor refuses
   reclamation behind such a tick (`src/core/server.h:617`); a full 4,096-entry retire
   ring forces the writer into `force_oldest_grace()` (`src/core/read_local.h:416`).
   The wait has a 50 ms ceiling (`src/net/uring.h:232`), so this is a potential
   retirement/latency stall, not evidence of an unconditional permanent deadlock
   or a torn read. Once parking is published, the matching active-before-epoch
   resume is essential to prevent premature reclamation during resumed reads.
3. **One cold completion assumed active-set scanning.** An MGET demotion that fails
   during scatter preparation can complete locally; its callback used
   `fused_executor_completion<false>` (**PRE** `src/core/io_loop.h:3786`). Overlap
   needs the targeted IFID queue notified as well, so subsequent input/fence
   progress does not rely on the periodic backstop.

There is no reachable A/D pair of partially executed executor batches to repair.
The two-context streams body is retained source, not the implementation of old
overlap 2. The boot dispatch now selects `run_fused_iofused_loop<..., true>`
(`src/core/io_loop.h:530`), exactly the old value-2 three-way implementation.
Its WB batch and first EX task batch coexist on the same thread, but the callback
is synchronous. Each whole EX batch completes or enters an existing deferred queue
before the next batch; no foreign read capture is carried into that next batch.

## What changed, and why the lane remains valid

- **Surface:** CLI and conf-file syntax are `overlap 0|1`; help, `tomokv.conf`,
  immutable CONFIG, INFO, notices, and tests use `overlap`. `x-overlap`, value 2,
  and the retired schedule aliases are rejected. Split still selects
  `run_split<1>()` versus `run_split<0>()` (`src/core/io_loop.h:429`).
- **Admission:** `Server::read_local_enabled()` now depends only on fused mode
  and `read_local` (`src/core/server.h:591`). The non-buffered private overlap
  parser is included in its compile-time `Fused` classification
  (`src/core/io_loop.h:3922`). This enables the existing protocol, including its
  room/quota checks, rather than admitting reads around the protocol. Unarmed
  overlap retains its byte-reply acquisition; armed overlap uses the existing
  coded local-read acquisition, which its WB path already supports.
- **Lifecycle:** overlap publishes parked before waiting, resumes active before
  sampling the epoch, and publishes permanent quiescence before IO teardown
  (`src/core/io_loop.h:935`, `src/core/io_loop.h:949`, `src/core/io_loop.h:958`).
  Local demotion-error completion selects the targeted callback when appropriate
  (`src/core/io_loop.h:3793`). No per-operation epoch publication was added.

The lifetime and ordering argument is the following:

| Property | Source evidence and consequence |
| --- | --- |
| Local window versus owner work | Both coarse and deep overlap enter `fused_pass_impl()` (`src/core/ex_loop.h:399`, `src/core/ex_loop.h:408`). It drains the local lane at `src/core/ex_loop.h:506`, before executing fresh owner tasks. Exceptional debt runs WB first at `src/core/ex_loop.h:470`; a clean turn runs WB later between owner prefetch and execution at `src/core/ex_loop.h:2130`. Neither order interrupts a local-read chunk. |
| Foreign pointers and validation | Captures live in stack-local chunks (`src/core/ex_loop.h:1579`). The same point and MGET preparation/validation functions are called. Their existing GET bound of 3 attempts and MGET bound of 2 attempts are unchanged (`src/core/ex_loop.h:1458`, `src/core/ex_loop.h:1170`, `src/core/ex_loop.h:1299`). No writer-dependent reader loop, new retry, sequence protocol, or relaxed acceptance was introduced. |
| Same-connection ordering | The parser now uses `acquire_read_local()` (`src/net/rob.h:238`), stages writes before dispatch (`src/core/io_loop.h:4260`), and applies the existing exact-key/owner fences. Demotion selects transitive conflicting reads in ROB order and posts them before the reserved current write (`src/core/io_loop.h:3597`, `src/core/io_loop.h:3734`, `src/core/io_loop.h:5266`). Prior owner work may remain in flight, but a local read passes only the existing conflict checks; unrelated clients do not impose a new fence. |
| Queue transport | Fixed and reservation-aware APIs address the same lane allocation (`src/exec/masked_queue.h:121`, `src/exec/masked_queue.h:317`, `src/exec/masked_queue.h:441`). A demotion reserves synchronously, commits or cancels before its sole producer resumes ordinary posting, and uses reserved posting for the current point operation. Broad lowering commits the reads before entering the special route. There is no outstanding IFID reservation across a pass and no private push interleaved with a live demotion reservation. The generic API is necessary for demotion; replacing it with an unchecked private push would lose its all-or-nothing capacity guarantee. |
| ROB and client lifetime | Local completion clears pending bits before publishing Done (`src/core/ex_loop.h:1736`). WB calls the same `Rob::drain()` that stops at the first non-Done slot (`src/net/rob.h:740`), so a pending owner task's Op cannot be recycled by the filler. Successful local replies contain copied bytes, not QSBR pointers. `drain_local_reads()` compacts tombstones before and after the complete drain (`src/core/ex_loop.h:1782`); pending reads keep the ROB non-quiescent and client release fenced (`src/net/conn.h:713`). |
| Retirement and ownership | Pass and sweep destructors publish after local captures are dead (`src/core/ex_loop.h:443`, `src/core/ex_loop.h:659`). Their existing `deferred.drain_ready()` calls remain (`src/core/ex_loop.h:579`, `src/core/ex_loop.h:688`). Each armed owner binds its sink before running (`src/core/ex_loop.h:237`); both shard move paths rebind it inside the ownership change (`src/core/server.h:2072`, `src/core/server.h:2100`). That rebind also advances the store generation (`src/store/flatstore.h:766`). |
| Atomic, persistence, and shutdown boundaries | Local windows are never opened in the middle of owner execution or a last-owner commit. Existing retry/deferred ordering, commit flushes, AOF-gated WB selection, and snapshot hooks stay in place. Blocking snapshot progress uses the coarse executor hook (`src/core/genthread.cc:197`), which has the same lane drain and QSBR boundary. Joined-thread shutdown drains and disarms the stores as before (`src/core/genthread.cc:100`). |

`--overlap 0` keeps the same runtime schedule, parser classification/acquisition,
fair-lane scheduler, hook bindings, and store behavior. Its public spelling/report
field intentionally changes. No layout field was added or moved. With read-local 0,
the existing predicates still skip all lane/server/thread/store sidecar allocations
(`src/core/server.h:312`, `src/core/ex_loop.h:195`).

## Fused mapping and remaining exclusions

Fused **on means former value 2**, the gated whole-batch three-way schedule. It
includes WB dependency prefetch, targeted IFID, WB CPU work in the first EX
load-to-use gap, and SEND-immediate/non-SEND-coalesced submission. Its existing
depth gate starts closed and returns thin passes to coarse order; “on” enables
the fullest adaptive schedule, not a demand to interleave empty work. The threshold
remains the internal constant 8 (`src/core/genthread_pipeline.h:34`).

There is historical head-to-head evidence, but only at the strength of its source:
merge `3c16d900d9ca7d4eb6ee431a164ac9c6d194e896` records the rebuilt old-2 arm as
“neutral vs o1 everywhere” and a thin-batch/DRAM corner of +0.5%. Implementation
commit `ef2cf93a7` identifies the added EX prefetch-gap filler. I did not find a raw
PRE/POST table or IPC/instruction profile for that comparison in the inspected
current documentation. It would be false to say the two were never compared, or
to call old 1 a measured loser on this tree. This mapping follows the requested
fullest-schedule meaning; the maintainer must measure the current arms before
performance acceptance makes it final. No second knob preserves old 1. Its body,
like the older streams body, has no boot dispatch and is retained only for source
comparison.

The local-read lane is supported in both fused overlap settings in the normal
immutable build. It retains its existing per-command fallbacks. Split accepts
read-local 1 but remains inactive and logs the existing kind of notice. Fused
overlap with epoll is still rejected; its single uring submit boundary has not
been redesigned. Buffered legacy streams remain unreachable. Overlap uses its
existing whole-owner-batch schedule, so the baseline's 32-task fair-lane
interleaving is not transplanted into its 128-task batches. That scheduling/latency
difference must be measured; it does not extend a local validation window.

## MEASUREMENT PLAN

The maintainer runs these sequentially on the quiet box, after the normal build
and appropriate release/ASAN checks. Use immutable SET-tax variant 0 throughout.
Use `--reorder 0` to isolate overlap; repeat the interaction cells with 1 using
the storage fix and mechanism checks in [REORDER.md](REORDER.md). Record binary/commit IDs,
affinity, actual `DEBUG LBSIGNALS` shard owners, INFO geometry, offered load,
pipeline depth, connections, payload sizes, and LB state for every cell.

### Boot and engagement cells

| Mode | overlap | read-local | Required effective INFO read_local |
| --- | --- | --- | --- |
| 2s | 0 | 0 | 0 |
| 2s | 0 | 1 | 0 |
| 2s | 1 | 0 | 0 |
| 2s | 1 | 1 | 0 |
| 1s | 0 | 0 | 0 |
| 1s | 0 | 1 | 1 |
| 1s | 1 | 0 | 0 |
| 1s | 1 | 1 | 1 |

Repeat all eight with atomic 0 and 1, uring, shards 16, cores `GATE_CORES`
(default 0-7); split uses the gate's actual `--ratio 6:2` on those eight cores.
Fused omits ratio and uses the same eight CPUs. Assert CONFIG `overlap`, INFO
`overlap`/`read_local`, and the absence of the old overlap names. Test CLI and conf
file parsing, immutable CONFIG SET rejection, value 2 rejection in both argument
orders, all supported epoll cells, and the explicit fused-overlap/epoll errors.
The updated `tests/config_parser_test.cc` covers the grammar matrix.

On otherwise quiet boots with `--key-lb 0 --client-lb 0 --enable-debug-command yes`, run
`python3 tests/overlap.py HOST PORT MODE OVERLAP READ_LOCAL` for each cell. It
selects two actual shard owners foreign to the reader, checks exact clean GET/MGET
hit deltas across ROB wraps and idle/resume, forces missing-GET demotion with its
counter witness, and checks pipelined read/write/read/MGET reply order. Armed
cells must report 416 clean local hits including 32 MGET hits; inactive cells
must report zero lane activity. Merely echoing the configured knob cannot pass.

### Correctness and liveness cells

- Run `tests/read_local_lane.py` on fused overlap 0/1: require positive
  `read_local_defer_lane_full` and `read_local_defer_quota`, zero capacity
  demotions, local-hit engagement, ordered values, and bounded completion.
  Run `tests/bplus.py` on both schedules with atomic 1; it now accepts either
  overlap value while retaining its held-window and counter assertions.
- Run the existing atomic RYOW/torn-read, MULTI, mixed GET/MGET, expiry/type/missing,
  rehash, snapshot/AOF, disconnect, and cache-churn batteries on both fused armed
  cells. Include AOF always/everysec, large copied replies, and TLS so filler
  retirement is exercised with buffer pressure and special retirement hooks.
  Repeat the gate at its own geometry; this source-only work is not a gate pass.
- Repeat `tests/rlcache_churn.py` with `--key-lb 1 --client-lb 1` and the normal ownership-debug build.
  Require witnessed shard/client movement, positive local hits, correct values
  in the dedicated correctness batteries, no sink-owner assertions, and clean
  shutdown accounting. Churn traffic alone does not establish value correctness.
- Drive one hot owner through more than 4,096 replacements while other fused
  threads are idle, then wake foreign GET/MGET readers repeatedly during churn.
  Check finite completion, pause/wake latency, and `mem_block_cache`/reclaimer
  activity. A temporary maintainer-only diagnostic build can count park/resume,
  grace scans, pending retire entries, reclaims, and forced waits while preserving
  variant 0. The existing `read_local_settax_qsbr_*` INFO counters are compiled
  only under variant 3; **do not enable in-place overwrites to obtain telemetry**.
- Mutation controls on throwaway builds: restore only the parser exclusion and
  require the exact-hit battery to fail; remove only the new park publication
  with a matching resume removal and require the idle-retirement witness to
  detect a stale active participant/forced wait. Timing alone is not an arming
  witness. Existing held-window tests must still fail their deliberately broken
  mechanism controls, re-arming bounded fresh state when needed, never skipping.

### Throughput, latency, and profiles

Use ABBA repeats at matched offered loads plus a separately reported saturation
sweep. Start on the measured eight-core CCX, then repeat the important cells at
64 real cores. Use the 25GbE two-netns rig for writeback/send claims, with loopback
only as a separately labelled control. Record rate and p50/p99/p99.9 latency,
cycles/op, instructions/op, IPC, cache misses (including L1D/LLC where supported),
branch misses, context switches, and CPU migrations. Use sampled profiles to
explain IPC/cache changes; do not accept an instruction-only verdict.

| Cell family | Arms | Purpose / required counters |
| --- | --- | --- |
| PRE/POST preservation | 2s and 1s, overlap 0, read-local 0/1 | Same behavior and no main-command regression. In 2s both lane settings remain inactive. |
| Schedule preservation | PRE 2s old-1 vs POST 2s on; PRE 1s old-2 vs POST 1s on, lane off | Separate the public remap from effects of newly compiled admission checks. |
| Fused mapping | Current code with the one boot template bound to former shallow vs chosen three-way, lane off/on, separate maintainer-built binaries | Head-to-head current evidence with no extra runtime knob. Label both the source binding and lane state; PRE lane-on overlap was inert and is not an armed control. |
| Clean foreign reads | GET and MGET, p1/8/32/128, small and DRAM-sized keyspaces, 64/256 B and 16 KiB values | `read_local_hits`, `read_local_mget_local_hits`, keyspace hits, all fallback deltas. Clean positive GETs/MGETs must fire locally; distinguish expected misses from declined reads. |
| Mixed owner/read load | Separate reader/writer connections, GET/SET 9:1 and 1:1; mixed same-connection RYOW traffic separately; SET/DEL-only controls | Hits plus `read_local_fallback_inflight_write`, `_context_owner_key`, `_context_route`, `_atomic_pending`, `_generation`, `_seq_churn`, `_missing`, `_typed`, `_expired`; per-thread owner/foreign ops from LBSIGNALS. |
| Pressure and thin/deep transitions | p1/7/8/9 and p32/128, 64 through 2,048 connections; burst/idle transitions | Lane defer/quota counters, queue depth/full events, ROB-head/queue-age samples, WB sends/retired counts and submit profiles. Reject a lane that disappears at high depth or after idle. |
| Atomic and ownership churn | atomic 0/1, disjoint and conflicting atomic keysets, LB 0/1 | Local MGET hits, generation retries and atomic fallbacks; `foreign_read_*` gauges and actual migration witnesses. Pair these with torn-read/RYOW checks. |

For a deep mixed cell, also prove the WB filler executed: this tree has no public
gate-open counter. On a diagnostic run use source-line probes/counters at the
`fused_three_way_pass(wb_filler)` call (`src/core/io_loop.h:6139`) and the actual
prefetch-gap `filler()` call (`src/core/ex_loop.h:2131`), together with local-hit
deltas. Sampling only the name `genthread_three_way_pass` is insufficient because
that function also contains the closed-gate path. Remove probes from rate runs.

Every optimization verdict needs a populated table; none is available from this task:

| Cell / offered load | PRE ops/s | POST ops/s | Delta | PRE/POST cycles/op | PRE/POST instructions/op | PRE/POST IPC | PRE/POST p99 | Local hits / fallbacks / filler witness |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Each matched arm above | pending | pending | pending | pending | pending | pending | pending | pending |

### Diff verification and gate accounting

Source review, Python AST parsing, shell syntax checking, and whitespace/diff
checks completed; runtime verification remains pending. The new
`tests/overlap.py` is a standalone battery, not a new gate row. The gate edit only
renames the INFO field read by the existing fused boot row at `tests/gate.sh:470`,
before the quick-tier exit at `tests/gate.sh:1246`. No rows were added or retired:
delta **0 quick / 0 full** for overlap alone. The reorder follow-up adds one row
in both tiers; see [REORDER.md](REORDER.md) for the combined count. The input constants remain **325 quick / 342 full**
(`tests/gate.sh:141`), untouched; these are the checkout's counts, not a claimed
344/344 result from the earlier supplied baseline.

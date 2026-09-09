# Reorder: public latency scheduling and safe batch storage

> Integration update (2026-09-08): [ORTHOG.md](ORTHOG.md) supersedes this branch report
> for combined-mode support, INFO activation evidence, layout accounting, and validation.
> Both modes now support both local-read settings with either overlap/reorder setting;
> `Config` retains the required 624-byte stride. Historical branch findings below remain intact.

Source-only change, 2026-09-08, in `cx-overlap` on `c8e61f646`. The existing
overlap restoration is retained, including fused overlap with read-local armed.
No compiler, server, benchmark, gate, or C++ battery was run. All runtime and
performance results below are pending maintainer validation.

## Public name and policy

`--reorder 0|1` (default `0`): cross-connection reordering inside an executor batch
for latency, with per-connection order always preserved.

`ex` described an internal role, and `sched` did not describe the benefit. This
is the paper's latency mechanism, so the study-only `--x-ex-sched` disposition
was wrong. CLI, conf-file `reorder 0|1`, help, annotated `tomokv.conf`, immutable
CONFIG GET/SET, and INFO SERVER now agree on `reorder`. CONFIG SET refuses even
an unchanged value because the setting is latched at boot. The default stays 0;
renaming the feature is not evidence for changing its default.

There is no alias for `ex-sched` or `x-ex-sched`. The only executable consumers
found were the parser tests and the overlap battery's off-control CONFIG check;
these now use `reorder`, and the retired-name tests reject both old spellings.
Historical audit records and the overlap-only finding retain the old names as
history, not supported invocations. No remaining script requires an alias.

The scheduling implementation now lives in one feature file,
[`src/core/reorder.h`](src/core/reorder.h). Both production call sites call that
same implementation. The ordering policy is unchanged:

1. Split the gathered tasks at ineligible/special tasks; nothing crosses those
   barriers. Null clients, scatter tasks, blocking state and the existing special
   routing flags remain ineligible. A same-owner local-fast MGET/MSET can qualify.
2. Leave one-client runs in FIFO order: there is no legal permutation.
3. Rank by distance from each client's ROB retirement head, then by static command
   cost. Cost is the connection's prefix maximum; unrepresented predecessors,
   gaps, and the atomic hazard bit conservatively widen it to Long. This is
   head-rank-first scheduling, not unconditional shortest-job-first scheduling.
4. Preserve stable gather order for equal buckets and the existing homogeneous,
   already-ordered, and invalid-rank/gather-contract FIFO exits.

The reverse head sampling, prefix rule, and special barriers are unchanged.
There are no new reader retries, seqlocks, writes to records, owner transfers,
or per-operation shared counters. Scheduler scratch is stack-only, behind the
existing boot-latched enable branches. `ex_schedule_batch` is explicitly
`noinline`, keeping its scratch reservation outside the off path. Off allocates
no scheduler storage; no sidecar or persistent scheduler table was introduced.

## True maximum `n`, with caller evidence

These are source-derived reachable maxima, not observed runtime maxima. `1s`
and `fused` are equivalent; `2s` and `split` are equivalent. Every row applies
with read-local 0 **and** 1. Split accepts read-local 1 with its existing inactive
lane notice. Armed fused reads that are served locally bypass this scheduler;
ordinary owner tasks still have the full capacities below.

Caller A is the first fresh batch in `drain_tasks_with_filler`:
[`ex_loop.h:2125`](src/core/ex_loop.h#L2125), formerly the reported call near 2132.
Caller B is `exec_batch`: [`ex_loop.h:2591`](src/core/ex_loop.h#L2591), formerly
the reported call near 2771 (2768 in the supplied overlap-only working diff).

| Thread mode | overlap | reorder | Gather capacity | A: max scheduled `n` | B: max scheduled `n` |
| --- | ---: | ---: | ---: | ---: | ---: |
| 2s | 0 | 0 | 32 | not called | not called |
| 2s | 0 | 1 | 32 | not called | 32 |
| 2s | 1 | 0 | 32 | not called | not called |
| 2s | 1 | 1 | 32 | not called | 32 |
| 1s | 0 | 0 | 32 | not called | not called |
| 1s | 0 | 1 | 32 | not called | 32 |
| 1s | 1 | 0 | 128 | not called | not called |
| 1s | 1 | 1 | 128 | 128 | 128 |

All eight combinations are accepted with uring. With epoll, both fused-overlap-1
rows remain rejected, regardless of reorder/read-local; all other rows retain
their existing epoll support (`validate_config`, `src/core/config.h:971`). Reorder
adds no mode, overlap, engine, or read-local exclusion. The parser test covers
both mode spellings, both values of all three knobs, and both network engines.
Source acceptance is not a claim that these boots were executed here.

The bound follows the complete call chain:

| Evidence | Consequence |
| --- | --- |
| `kGenthreadExBatchOps = 32`, `kGenthreadPipelineExBatchOps = 128` in `src/core/genthread_pipeline.h:12` and `:19` | These are separate build-time geometries. `kExecBatch <= 32` in `ex_loop.h:69` constrains only the former. |
| Split `run()` calls `drain_tasks()` / `drain_tasks(true)` with default template arguments (`ex_loop.h:742`, `:761`). Split overlap changes IO dispatch at `io_loop.h:430`, not executor geometry. | B receives at most 32 in either split overlap setting, including role/LB drain and idle repair. A has no split caller. |
| `fused_baseline_pass()` at `ex_loop.h:386` instantiates `fused_pass_impl<kGenthreadExBatchOps,...>`. Its armed interleave path gathers `Task batch[kReadLocalOwnerTaskChunkOps]` at `:2051`; that constant is locked equal to `kExecBatch` at `:72`. | Fused overlap-off B receives at most 32 with either lane setting, including the fair-lane producer chunks. A has no baseline caller. |
| Fused overlap selects `run_fused_iofused_loop<..., true>` at `io_loop.h:533`. Its closed depth gate calls `fused_coarse_pass()` at `:6003`; the open gate calls `fused_three_way_pass(wb_filler)` at `:6139`. Both instantiate the **128** geometry (`ex_loop.h:395`, `:404`). | Both thin/coarse and deep/filler overlap turns can gather 128, with either read-local setting. The depth threshold 8 controls which turn to use; it does not cap batch size. |
| `drain_tasks<BatchOps>` at `ex_loop.h:2095` declares `Task batch[BatchOps]`, invokes B at `held == BatchOps`, then resets `held = 0`; it also executes the nonempty tail. | `1 <= n <= BatchOps`, independent of the total tasks drained in the outer pass. |
| `drain_tasks_with_filler` at `ex_loop.h:2118` has the same array, full-batch trigger, tail, and reset. Its first fresh batch invokes A; further batches or a previously used filler invoke B. | A and B can each receive 128 in fused overlap. Retry debt may bypass scheduling; it cannot increase a batch. |
| `ThreadCtx::drain_tasks` at `thread.h:603` drains producer lanes into the callback; `drain_tasks_unmasked` at `:1007` does likewise without the hint mask. A lane has 1,024 slots (`thread.h:58`); each connection has a 64-slot ROB (`net/rob.h`). | Multiple connections can fill 128, even within one producer lane. Two full ROBs suffice. A single client can supply 64 to overlap, already enough to overrun the old outer array before the one-client early return. |
| Overlap idle sweeps propagate `BatchOps` (`ex_loop.h:650`); blocking snapshot hooks select coarse overlap at `genthread.cc:203`. Snapshot/retry backlogs have their own per-task execution paths. Legacy buffered streams have no boot dispatch. | No hidden larger scheduler caller or 32-task safety clamp exists. A pass-total `n` is not the `held` passed to scheduling. |

## Storage repair

The original `base_lengths[32]` was unsafe even when no permutation was possible:
the outer candidate walk indexed it before `ex_schedule_run` could return for one
client. `keys[32]` and `ordered[32]` also overran on larger eligible multi-client
runs. The old 64-slot connection table could become completely full, and simply
enlarging it while keeping a scalar occupancy mask would shift by 64 or more.

`exec_batch` now preserves `Task (&batch)[BatchOps]`, and `ex_schedule_batch`
deduces that same capacity. `base_lengths`, `keys`, and `ordered` are sized to
the caller's full array. The connection table has `2 * BatchOps` slots and
`ceil(2 * BatchOps / 64)` occupancy words; every test/store selects `slot >> 6`
and shifts by `slot & 63`, including during collision probing.

| Storage | 32-task specialization | 128-task specialization |
| --- | ---: | ---: |
| Base lengths | 32 bytes | 128 bytes |
| Rank/class keys | 64 bytes | 256 bytes |
| Ordered Tasks (`sizeof(Task) == 32`) | 1,024 bytes | 4,096 bytes |
| Connection pointers + last task index | 64 slots, 576 bytes | 256 slots, 2,304 bytes |
| Connection occupancy | 1 word, 8 bytes | 4 words, 32 bytes |
| Bucket counts/cursors + bucket occupancy | 384 + 24 bytes | 384 + 24 bytes |
| Sum of these scratch arrays (not compiler stack-frame size) | 2,112 bytes | 7,224 bytes |

Buckets remain 64 ROB ranks times three cost classes, not one bucket per task.
At most 128 tasks enter: an index is at most 127 and a bucket count/cursor/end
total at most 128, so the existing byte types remain valid. Static assertions
require an audited geometry, a power-of-two capacity, and capacity <= `UINT8_MAX`.
The half-full table must find an empty or matching slot. A future bad `n` greater
than its actual array capacity aborts even in release builds, before any scratch
indexing. There is **no truncation, scheduling cap, or counted overflow fallback**.

The reorder restoration renames `Config::ex_sched` and the loop's cached bool in
place without growing either. The subsequent final surface reduction explicitly
accounts for **Config at 488 bytes**, from incoming 528 and original 624; see
[DESIGN-KNOBS.md](DESIGN-KNOBS.md). Other footprint locks remain unchanged. The other inherited correctness caveat recorded in `OVERLAP.md`
also remains: SET-tax variants 1/3 contain sequence-protected in-place writes;
use the default immutable variant 0 for this work. This patch adds no such path.

## Gate battery and failure controls

[`tests/reorder_unit.cc`](tests/reorder_unit.cc) constructs real Clients, Tasks,
and acquired/published ROB slots, then calls the **production** `ex_schedule_batch`.
It executes no command handler and opens no sockets. Its null-only MULTI teardown
stub aborts if the fixture ever creates a session. There is no scheduler mock,
separate sorting implementation, elapsed-time verdict, or probabilistic window.

Before positive cases it asserts live head ranks, eligible mixed cost classes,
full pipeline occupancy, or the barrier/gap/hazard state being tested. It checks
the complete expected Task sequence, whole-task identity, membership, uniqueness,
unchanged unused suffix, increasing per-client IDs, and a required non-identity
permutation. Assertions are explicit failure exits, so `NDEBUG` cannot remove
them. The cases are:

- Every `n` from 2 through 32 and from 2 through 128: independent long heads
  gathered before short heads must yield the exact stable short-before-long order.
  At 128 distinct clients, connection occupancy must use more than one word,
  regardless of the allocator's addresses.
- Full 32/128-task batches from two clients, with a long head followed by short
  operations on one connection. Four fill/retire cycles prove the cost prefix,
  per-connection order, and ranks after ROB wrap; the large case has two full ROBs.
- Admin, null-client, and scatter barriers, with exact independent permutations
  on both sides. The large case places the barrier at index 32 and schedules the
  whole eligible suffix through index 127.
- Empty, one-task, a full single-client ROB, equal-rank homogeneous, and already
  ordered controls must stay byte-for-field identical. Missing predecessors,
  an in-run ID gap, and the atomic hazard bit must widen cost to Long.

The battery requires **175 positive permutations**: 31 + 127 head cases, 8
pipeline cases, 6 barrier cases, and 3 predecessor cases. Removing a positive
case cannot silently lower coverage. Every state is constructed afresh or fully
retired before reuse. There is no window to retry, no skip branch, and no tolerance
to widen. The gate compiles it with ASAN **and** UBSAN and disables sanitizer
recovery; compile errors, sanitizer findings, failures, and the 60-second timeout
all produce the same failing row. `make unit` also includes its ordinary build.

On maintainer-only throwaway copies, first run the unchanged battery, then:

| Deliberate break | Required failure |
| --- | --- |
| Return immediately from `ex_schedule_batch`, or from `ex_schedule_run` | The two-head long/short exact-order case fails. A mechanism-free build cannot pass. |
| Delete the final `tasks[i] = ordered[i]` copy | Same exact-order failure, even though all ranking/bucket code still runs. |
| Restore `base_lengths[kGenthreadExBatchOps]` only | ASAN fails when the 128-capacity head sweep reaches n=33. |
| Restore `keys[32]` or `ordered[32]` individually | The same head sweep trips ASAN in each undersized array. |
| Restore 64 connection slots but keep large task arrays | The distinct-client sweep exceeds table capacity; UBSAN/ASAN, oracle failure, or the hard timeout makes the row fail. No timeout is accepted as success. |
| Use the old scalar occupancy word with the 256-slot table | UBSAN catches a shift >=64; the 128-distinct-client case necessarily requires slots beyond one word. |
| Silently clamp `n` or run length to 32 | The n=33 exact permutation fails; the barrier case also checks scheduling beyond the prefix. |
| Drop prefix-maximum widening or sort cost before rank | The full two-client pipeline oracle fails: the selected cross-connection order no longer matches the existing head-rank/prefix-cost policy. |

These are prescribed controls, **not results obtained in this session**. The
server-less row proves the production scheduling decision and storage geometry;
it does not by itself prove a live workload reached either executor seam or
that latency improved. Those separate witnesses are required below.

Gate accounting is by the emitting line: the reorder row's `ok`/`bad` is at
[`tests/gate.sh:329`](tests/gate.sh#L329), before the quick-tier exit now at
[`tests/gate.sh:1264`](tests/gate.sh#L1264): **+1 quick, +1 full**. Overlap added no row.
The final knob surface also restores dispatch scaling at line 694, another +1 in both tiers.
Against c8e61f646's 325/342, the complete working diff therefore requires
**EXPECT_QUICK=327, EXPECT_FULL=344**, or full 345 with optional NIC. The constants at
lines 141–142 stay 325/342 for the maintainer to edit. [DESIGN-KNOBS.md](DESIGN-KNOBS.md)
is the final combined ledger.

## MEASUREMENT PLAN

Run sequentially on the quiet box after maintainer builds, the new unit battery
and its negative controls, then the gate at its real geometry. Record source and
binary IDs and digests, allocator and compiler flags, immutable SET-tax variant,
thread mode, overlap, reorder, effective read-local state, atomic state, affinity,
actual shard owners, clients, pipeline depth, key/value sizes, and offered load.

### Workload that can benefit

Use many independent connections issuing **95% GET of 64/256-byte values and
5% BITCOUNT of preloaded 64/256 KiB strings**. Sweep the long fraction through
1%, 5%, and 10% and the long payload size separately. These are ordinary
single-owner Point and Long commands according to `commands.cc:82`; BITCOUNT's
small integer reply keeps the first experiment focused on executor service time.
Calibrate long-operation cost with separate maintainer runs before choosing the
final mix; a class label alone does not establish that an operation is slow.

First map both key sets to the **same actual owner**, with multiple connections
sharing that owner, so fast and slow tasks can meet in a batch. Use DEBUG SHARD
and DEBUG LBSIGNALS on a debug-enabled, `--key-lb 0 --client-lb 0 --flip-auto 0` boot to verify this;
do not infer placement from key names. Then repeat a balanced spread over owners.
Use independent arrival streams and a fixed trace seed, not one synchronized
connection that serializes all the long/short work.

Start with 32/64/128 connections and depths 1, 4, 16, 64, 128. At depth 1,
multiple connection heads can share a batch; at depth 128 the actual per-client
ROB window is still 64, so report offered depth and observed occupancy separately.
Deep same-client pipelines are essential controls because head rank and prefix
cost limit how much benefit can be obtained. Include separate fast-only and
slow-only connection pools as well as the randomized per-connection mixture.

Use read-local **0** for the primary GET experiment. With read-local 1, clean
GETs can bypass the owner scheduler altogether, and local-lane acceleration is
not evidence for reorder. Repeat armed interaction cells with owner-routed
STRLEN (Point) plus BITCOUNT (Long), both with small replies, and separately
report the GET lane experiment with hit/fallback counters. Do not use EVAL,
DEBUG SLEEP, blocking, scatter, or transactional requests as the supposed long
eligible operation: these paths are scheduling barriers, not the intended mix.

### Arms, engagement, and controls

- Cover the eight legal uring mode/overlap/reorder rows above, each at read-local
  0/1 and atomic 0/1. Start at `--shards 16` on `GATE_CORES` (normally 0-7): split
  uses the gate's **6:2** IO:executor ratio on eight cores; fused uses those eight
  CPUs without `--ratio`. Validate the six legal epoll mode/overlap/reorder rows
  separately, plus both rejected fused-overlap rows. The parser matrix is not a
  substitute for these boots.
- With the same binary, alternate reorder 0/1 boots at matched offered load.
  Begin at 50%, 70%, 85%, and 95% of the off arm's sustainable rate, and report a
  separate saturation sweep. Use ABBA repeats and the same traces. Record achieved
  rate, queue growth/timeouts, and offered-rate fidelity; overload in one arm
  cannot be hidden by reporting completed requests alone.
- On separate diagnostic runs, instrument **both actual scheduler call sites**:
  count invocation/batch-size distributions, eligible run sizes, distinct clients,
  rank/class mix, and non-identity permutations by comparing Task IDs before and
  after the actual call. Require a long head followed by a short head at the same
  rank, then witness the short task moving ahead. Sampling function entry or
  echoing `reorder:1` is insufficient. In fused overlap require batches **>32** at
  A and B, plus the deep-gate/filler and coarse-turn witnesses. If a run misses a
  claimed state, repeat a bounded fresh burst/boot with multiple connections and
  fail the engagement cell if it never occurs; never skip it or loosen a verdict.
  Keep these traces/counters out of the rate/profile binaries.
- A single-connection mixed run must have zero permutations, even with a full
  ROB. Homogeneous equal-rank multi-connection traffic should hit the existing
  cheap exits. Large multi-client pipelines of a single class may still reorder
  by rank; do not mislabel those as inert controls. Run the existing main-command,
  RYOW/MULTI, atomic/torn-read, blocking, migration, and shutdown batteries with
  reorder 1 as well, on both modes and overlap settings. Reuse the overlap
  engagement battery with its CONFIG expectation changed to 1 for that diagnostic
  copy, or retain 0 for its isolated overlap control.

### Verdict and profiles

Report short-operation and long-operation latency distributions separately
(p50/p95/p99/p99.9/max), plus the combined distribution. A combined percentile can
be dominated by the configured long fraction and hide the benefit or cost to
either class. Capture client latency without coordinated omission and preserve
timeouts/errors. Include fairness/queue age for the long operations: reordering
does not preempt a long command already running, and benefit to shorts must not
hide unacceptable long-request starvation.

Start on the measured eight-core CCX; repeat representative cells at 64 real
cores. Use the 25GbE two-netns rig for paper/send-path results and label loopback
as a separate control. Reintroduce the production LB/flip settings in distinct
cells only after the fixed-placement result is understood. Hold all other knobs
constant within a comparison.

Collect cycles/op, instructions/op, IPC, cache/branch misses, queue/ROB-head ages,
and sampled profiles on both arms. Attribute cost to the scheduler, command
handlers, task gather, and reply path. Check `cycles/op = instructions/op / IPC`;
matched-load rate and per-class latency are the verdict, not instruction count
alone. PRE vs POST with reorder off must preserve main-command performance; the
32-task old scheduler can also be compared with the restored scheduler on a
maintainer-built PRE binary. **Do not benchmark the known-overflow PRE scheduler
with fused overlap enabled.** Use the repaired current binary's 0/1 arms there.

No improvement is claimed until this table is populated for each geometry/load:

| Comparison / geometry / offered load | PRE ops/s | POST ops/s | Delta | PRE/POST cycles/op | PRE/POST instructions/op | PRE/POST IPC | PRE/POST short p99 / p99.9 | PRE/POST long p99 / p99.9 | Actual permutation / >32 / seam witness |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| PRE/POST off-path preservation | pending | pending | pending | pending | pending | pending | pending | pending | off: zero |
| PRE old scheduler / POST reorder, 32-task geometry | pending | pending | pending | pending | pending | pending | pending | pending | pending |
| Repaired binary reorder 0 / 1, all legal geometries | pending | pending | pending | pending | pending | pending | pending | pending | pending |

## Verification status

Source/diff review, `bash -n tests/gate.sh`, Python AST parsing of the changed
Python batteries, and `git diff --check` (excluding the live `CODEX-OUT.md`
transcript) passed. The new files also passed a whitespace check. These are
non-executing syntax/source checks, not C++ verification.
Compilation, the 175-permutation battery, sanitizer and negative-control results,
all boot cells, full correctness coverage, and every PRE/POST performance cell
remain for the maintainer. No existing layout assertion or EXPECT constant was
changed to turn a result green.

# SET regression investigation — 2026-09-09

**Status: the reported 57.69% server regression did not reproduce in the permitted
geometry. No server fix, successful merge bisection, or zero-regression result is
claimed.** A competing clean build collapses **both** unchanged binaries to about
2.8M SET/s; both recover when the build finishes. The historical measurement
session overlapped several builds on its server cores. This is a concrete
measurement-contamination explanation, with an experimental control, but the
historical logs do not timestamp individual cells sufficiently to prove which
build overlapped each slow cell.

The deliverable contains the evidence and a serial reproducer,
[`tools/setregress.py`](tools/setregress.py), which records commands, binary
digests, PIDs, timestamps, preload size and live INFO. It waits for existing known
workloads and invalidates a timed cell if another known server, driver or compiler
appears. The server implementation has not been changed. The requested causal
revert/forward-application experiment and a validated server fix remain open.

## Trees and clean-build identity

The supplied worktree had advanced beyond the five-branch integration described
in the prompt. All three relevant states were built and measured in independent,
owned worktrees under `build/setregress/`. The good tree's tracked source was left
unchanged; an unintended reference-binary rewrite is disclosed below.

| Label | Commit | Meaning |
| --- | --- | --- |
| good | `c8e61f64628655bf99acb91cd986684e5c608fde` | Supplied known-good source |
| reported-bad | `2587f70e6` | Integration tree before the three later storage/atomics/netcmd merges; retained as the merge session's baseline binary |
| bad | `8a7260059d82ca53d2f903507834cccf28c83673` | Actual source HEAD at the start of this task |

The worktree subsequently advanced to documentation commit `a6f1950ab`; its source,
Makefile and gate are identical to `8a7260059`. User/session transcript changes were
left alone.

Every server build used `make clean` followed by
`taskset -c 8-31 make -j20`, with the checked-in Makefile, GCC 13.3.0, `-O2 -g
-march=native`, jemalloc and io_uring. No compile overlapped a quiet measurement.
Build logs are `build/setregress/{good,bad,reported-bad}-build.log`.

| Binary | SHA-256 |
| --- | --- |
| good | `9b5402aa99e2db85e7c817918bec13eee705b0a3e45662099aa6b372f57f98b8` |
| reported-bad | `d4133232d000d8ac81db2076e2b2b2d72c5c7c0e5ec570cdf8220b9adb904d4a` |
| bad | `1d4c8f4c1c3f818d30be13423288d1151291037acd15f8f16da3a1dd61d97bd8` |

To check that the non-reproduction was not simply a different executable build,
the ELF `.text` sections were extracted with `objcopy` and hashed:

| Comparison | Identical `.text` SHA-256 |
| --- | --- |
| Fresh good vs `/home/user/Projects/tomokv-cpp-perthread/build/tomokv` | `6e2a929b290c7e3cc5ba7cbcfb2c576186515f59d590a23e1056cc6d6fc8f4ae` |
| Fresh reported-bad vs `build/mergerest/baseline-2587f70e6` | `4205b0479c2d2b564350c8bc6e099eddf9a6077c567233b8352bfda583e00efb` |

This establishes matching machine-code sections, not identical runtime placement
or a claim that every ELF section is identical.

**Tooling side effect:** the section-dump command omitted `objcopy`'s separate
output ELF argument. Consequently it rewrote its input ELF and refreshed its
timestamp, including `/home/user/Projects/tomokv-cpp-perthread/build/tomokv` outside
this worktree. This was an execution mistake against the worktree-only rule.
The good source tree's `git status --short` remains empty. The owned good binary
still has its recorded pre-dump SHA above, and the retained historical bad binary
still matches its previously documented full SHA
`c3e595258dc9590899ead8a615155167232af161d9f657b029d0b1b32ebeda81`.
There was no recorded pre-dump full-file hash for the external good binary; its
post-dump hash is
`9846399c4c5ebb61d97015861b71e4408aad158eaf2f7dde979fde6b1044f154`.
No reference source or build recipe was edited. Subsequent dumps must specify a
separate output ELF inside the owned worktree rather than rewriting the input.

## Reproduction geometry

The original commands were recovered from `/home/user/Projects/quickcheck.sh` and
the individual rates from `/tmp/claude-1000/qc.csv`.

| Parameter | Historical run | This investigation |
| --- | --- | --- |
| Server affinity | `0-31`, 32 physical cores | `8-31`, 24 physical cores |
| Load affinity | `64-127` | `96-127` |
| Port | `7990` | `8460` |
| Split initial ratio / controller | `16:16`, `--flip-auto 1` | `12:12`, `--flip-auto 1` |
| Shards | Auto: fused 256, split 128 | Auto: fused 192, split 96; separate fused 256-shard reproduction |
| Measurement | memtier 2.5.1, 16 threads × 8 clients, P32, 8 seconds | Same |
| Dataset | 1,000,000 keys, 64-byte values, `P:P` | Same; `DBSIZE == 1000000` required before every cell |
| Atomics / overlap / reorder | `1` / default 0 / default 0 | Same |
| Preload | 8 threads × 8 clients, P32, `--ratio=1:0 -n allkeys` | Same |

The explicit resource restriction prevents an exact replay of the original
affinities. Approval to extend server affinity was requested; no out-of-range
server or load process was started. These results establish non-reproduction on
the permitted 24 cores, **not** a proof about every 32-core execution.

Each cell starts a fresh server and data directory, preloads the entire keyspace,
captures INFO before/during/after the load, and stops its own Popen child. Arms
alternate order between repetitions. No pattern kill, foreign-process signal,
gate tier, or gate-count edit was used. Flip completed zero transitions in the
reported split cells; the measured split remained 12:12.

## Armed fused SET: both bad source states remain fast

Rates in millions of operations per second. Every row has three fresh-server
repetitions; none is removed as an outlier.

| Geometry / tree | Rep 1 | Rep 2 | Rep 3 | Median | Delta vs good |
| --- | ---: | ---: | ---: | ---: | ---: |
| Auto/192 shards, good | 11.704368 | 11.934103 | 11.859918 | **11.859918** | — |
| Auto/192 shards, bad | 11.690684 | 11.931535 | 12.050870 | **11.931535** | **+0.60%** |
| 256 shards, good | 11.151822 | 12.400867 | 11.650028 | **11.650028** | — |
| 256 shards, reported-bad | 11.628046 | 11.993570 | 11.831296 | **11.831296** | **+1.56%** |
| 256 shards, bad | 11.832008 | 12.061447 | 11.838134 | **11.838134** | **+1.61%** |

Artifacts: `build/setregress/reproduce/` and `reproduce256/`. Each directory has
`results.json`; each cell has `result.json`, `server.log`, `populate.log`,
`load.log`, and raw `boot/before/during-early/during-late/after.info` snapshots.

Some repeat ranges exceed 2%, notably good/256. They are retained and unresolved;
they are not relabeled as the box's claimed ±0.15% noise. These runs are sufficient
to show that no observed quiet cell exhibited the alleged collapse, but not to
certify sub-percent parity.

## Counter comparison and the ring hypothesis

The following are the live, late-load INFO values from repetition 1 at 256 shards.
All SET repetitions, including the contention controls, have the same zero arm,
record, hit and fallback counters.

| INFO field | good | reported-bad | bad |
| --- | ---: | ---: | ---: |
| `read_local` | 1 | 1 | 1 |
| `read_local_active_threads` | Not exported by this version | 24 | 24 |
| `read_local_arms` | 0 | 0 | 0 |
| `read_local_write_ring_sidecars` | 0 | 0 | 0 |
| `read_local_write_ring_records` | 0 | 0 | 0 |
| `read_local_hits` | 0 | 0 | 0 |
| `read_local_fallbacks` | 0 | 0 | 0 |
| `read_local_fallback_inflight_write` | 0 | 0 | 0 |
| `read_local_fallback_arm_transient` | 0 | 0 | 0 |
| `read_local_fallback_lane_full` | 0 | 0 | 0 |
| `read_local_defer_lane_full` / `read_local_defer_quota` | 0 / 0 | 0 / 0 | 0 / 0 |
| `mem_block_cache` (bytes, sampled gauge) | 139104 | 102624 | 81120 |

The distinction is between **server/store read-local enablement** and **arming a
connection's RYOW ring**. The server's lane and immutable replacement/reclamation
path remain enabled during pure SET. The connections have never issued a local
read, so their RYOW sidecars are never allocated. Ring occupancy is therefore
empty by construction; INFO does not provide a separate occupancy sample here.

The sixteen-entry overflow account describes an older predecessor, not either
tree in this comparison. `DESIGN-RINGDIET.md` records the later arm-on-demand
design. In both tested sources, `ReadLocalRobState::kWriteRingCapacity` follows
`kRobWindow` (64), and `Rob` statically requires capacity to cover the ROB. The
vestigial deletion removes an unreachable capacity fallback, but these pure SET
connections do not enter the allocated-ring path at all. Restoring that fallback
would not be an evidence-based fix for these measurements.

Positive GET controls establish that this is not a globally disabled lane:
fused good and bad each arm all 128 measured connections, record tens of millions
of local hits during the live snapshot, allocate zero write-ring sidecars and
report zero read fallbacks. Bad reports 24 active fused lanes. In split mode, bad
reports 12 active IO lanes and 128 arms; the good binary prints a boot NOTICE
that split read-local is unsupported, reports effective `read_local:0`, and has
zero local hits. A requested `--read-local 1` on that baseline is not evidence of
an active split lane. The historical `rl_hits=0` extraction also misses the old
fused aggregate counters because it searches the newer per-thread row grammar.

## Contention control and historical evidence

The original armed fused SET rows were already internally inconsistent:

| Historical tree | Rep 1 | Rep 2 | Rep 3 | Median |
| --- | ---: | ---: | ---: | ---: |
| baseline | 10.920211 | 10.908735 | 11.336706 | 10.920211 |
| final | **10.900096** | **4.620430** | **3.722930** | 4.620430 |

The first final repetition was fast. The same historical final run also contains
an unarmed fused SET repetition at 8.713485M, versus its other two at 11.193039M
and 11.219297M; the large variation is not confined to armed SET.

Filesystem creation/modification times in Asia/Taipei show that `perf2.log` spans
13:40:10–14:17:44. The merge report explicitly records observing another server
and benchmark and leaving them running. Its builds used server cores 8–31:

| Merge build log | Created | Last written |
| --- | --- | --- |
| `storage-build-1.log` | 13:58:41 | 13:59:15 |
| `storage-build-2.log` | 14:00:37 | 14:01:53 |
| `atomics-build-1.log` | 14:02:40 | 14:04:03 |
| `atomics-build-2.log` | 14:05:23 | 14:06:38 |
| `netcmd-build-1.log` | 14:07:59 | 14:08:40 |
| `final-build.log` | 14:10:11 | 14:11:29 |

Those are file-time bounds, not reconstructed per-cell timestamps. They support
overlap of the sessions; they do not identify the exact external process present
in each historical slow cell.

The directed control uses an **owned**, freshly cleaned `reported-bad` build
worktree and `taskset -c 8-31 make -j20` during the already-preloaded SET cell.
Only this section intentionally admits compilation during measurement. The
build remains active at load completion and then finishes successfully. Both
server binaries retain their original digest throughout.

| Unchanged server | Quiet median, 256 shards | With competing clean build | After build ends |
| --- | ---: | ---: | ---: |
| good | 11.650028M | **2.802619M** | **11.275361M** |
| bad | 11.838134M | **2.768414M** | **11.930189M** |

The contention and recovery entries are **single diagnostic repetitions**, not
medians: contention uses `perf record`; recovery uses `perf stat`. They are not
pooled into the quiet verdict. Raw artifacts are in `contention/` and `recovery/`.
The control demonstrates a reversible environmental collapse on both binaries.
It does **not** substitute for a merge-hunk revert and forward application.

An initial `perf report` attempt overlapped the second contended cell while
waiting for symbol resolution. It was stopped by its exact owned PID and rerun
with debuginfod disabled. To remove that extra diagnostic activity as a confound,
the bad contention control was repeated alone with the guarded harness: **2.742382M
SET/s**, again with zero arms/ring records/fallbacks, a witnessed compiler and
clean build/server exits (`guarded-contention/`). This repeat is also excluded
from quiet comparisons.

Flat cycle profiles show parsing, immutable string storage, dispatch and
networking in both arms. They do not identify a new ring-demotion hot path. The
causal intervention measured here is resource contention; identifying its full
off-CPU/cache/QSBR decomposition would require more instrumentation. In
particular, the mere existence of `force_oldest_grace()` is not proof that it
caused the historical result.

For completeness, recovery `perf stat` reports both instruction work and IPC:

| Recovery arm | Approx. instructions/op | IPC | Approx. cycles/op |
| --- | ---: | ---: | ---: |
| good | 5023 | 0.8029 | 6256 |
| bad | 4820 | 0.8043 | 5993 |

These are server process counters over approximately four seconds. Operation
denominators come from the adjacent live INFO snapshots, which bracket rather
than exactly coincide with the perf window. They explain these recovery runs;
they are not measurements of a code fix or matched offered-load latency tests.

## All five cells on the permitted geometry

This is a clean **PRE/current** comparison, not an after-fix table. All medians
use three 8-second repetitions, fresh full preloads and auto shard geometry.

| Cell | good PRE | current bad | Delta |
| --- | ---: | ---: | ---: |
| 1s read-local=1 P32 SET | 11.859918M | 11.931535M | **+0.60%** |
| 1s read-local=1 P32 GET | 16.558716M | 16.496354M | **−0.38%** |
| 1s read-local=0 P32 SET | 12.612337M | 12.157180M | **−3.61%** |
| 2s read-local=1 P32 SET | 14.135839M | 13.644544M | **−3.48%** |
| 2s read-local=1 P32 GET | 15.270623M | 16.184972M | **+5.99%** |

The last four cells' individual repetitions are retained here as well:

| Cell / tree | Rep 1 | Rep 2 | Rep 3 |
| --- | ---: | ---: | ---: |
| 1s/1 GET good | 16.167544 | 16.558716 | 16.572660 |
| 1s/1 GET bad | 16.092475 | 16.505273 | 16.496354 |
| 1s/0 SET good | 12.612337 | 12.775895 | 12.545165 |
| 1s/0 SET bad | 12.201548 | 12.157180 | 11.990805 |
| 2s/1 SET good | 14.145976 | 14.135839 | 13.832345 |
| 2s/1 SET bad | 13.609254 | 13.644544 | 13.802550 |
| 2s/1 GET good | 15.389426 | 15.261765 | 15.270623 |
| 2s/1 GET bad | 16.184972 | 16.641544 | 16.041787 |

Artifacts: `reproduce/results.json` plus `controls/results.json`. The smaller SET
losses remain unresolved and cannot be dismissed as noise or claimed to meet the
maintainer's zero-regression bar. The supplied fused GET improvement is also not
reproduced here; split GET's effective local lane is measurably active.

## Narrowing status, deliverable limits, and repeat commands

| Required step | Result |
| --- | --- |
| Reproduce both trees and confirm the collapse | Both built and run; collapse absent at 192 and 256 shards on permitted cores |
| Compare live counters | Completed; no arm/ring/demotion divergence in SET |
| Remove a merge part from bad and recover | **Not established:** the unmodified bad binaries already run fast |
| Apply only that part to good and collapse | **Not performed:** no causally supported candidate part exists |
| Explain a merged-code mechanism | **Not established:** resource contention is experimentally sufficient, with historical overlap evidence |
| Fix the server and measure all five after cells | **Not completed:** no justified server fix; current five-cell results above explicitly retain losses |

A speculative revert would not satisfy the requested two-direction proof. An
exact historical-affinity replay needs permission for server cores 0–7 and load
cores 64–95 in addition to the assigned ranges. Until a quiet bad cell is
reproduced, the 57.69% number is not a sound basis for changing the armed path.

The concrete harness repair addresses the observed measurement defect: serial
arms, a local run lock, UTC timestamps, exact preload assertions, raw INFO while
load connections are still alive, binary identities, and rejection of detected
foreign workload arrivals. Explicit contention controls are labeled and require
a witnessed, still-running owned compiler. This guard recognizes known process
names; it is not a universal detector of every kind of machine interference.

The unguarded measurement harness used for the tables is retained as
`build/setregress/run.py`; the guarded version is `tools/setregress.py`. A directed
serverless check verifies that a quiet monitor passes, a newly appearing compiler
fails it, and an unrelated compiler is not attributed to an owned build. A live
five-second smoke run of the guarded harness is separate from all tables above.

Example repeat after clean builds (choose an unused output name):

```sh
taskset -c 96-127 python3 tools/setregress.py \
  --arm good=build/setregress/good/build/tomokv \
  --arm bad=build/setregress/bad/build/tomokv \
  --output repeat-five \
  --cell 1s:1:SET --cell 1s:1:GET --cell 1s:0:SET \
  --cell 2s:1:SET --cell 2s:1:GET

taskset -c 96-127 python3 tools/setregress.py \
  --arm good=build/setregress/good/build/tomokv \
  --arm reported-bad=build/setregress/reported-bad/build/tomokv \
  --arm bad=build/setregress/bad/build/tomokv \
  --output repeat-256 --shards 256
```

All 45 completed cells, including the guarded smoke and contention repeat,
verified exact preload size and clean server exit; no forced server stop was
needed. No gate row was added or retired, and EXPECT constants were not edited.

One pre-existing law conflict encountered during inspection is worth recording:
`src/store/read_local_settax.h` still offers experimental compile-time selectors
1 and 3 with sequence-protected in-place writes. They conflict with the stated
immutable/no-seqlock laws. Every binary in this report uses production selector
0; none of those alternatives was used as a fix or for instrumentation.

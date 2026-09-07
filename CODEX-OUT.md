Reading additional input from stdin...
OpenAI Codex v0.153.4
--------
workdir: /home/user/Projects/cx-merge
model: gpt-6-astra
provider: openai
approval: never
sandbox: danger-full-access
reasoning effort: max
reasoning summaries: none
session id: 01a07b68-60f4-7842-b765-a149640742a1
--------
user
# TomoKV — project context for codex

Prepend this to every codex prompt. It is the shared background; each prompt then states its own scope.
Written 2026-09-07. Baseline commit `78c3e5391` (`origin/cpp`), full gate 344/344.

## What it is

TomoKV is a ground-up C++20 in-memory key-value server that speaks the Redis wire protocol (RESP2/RESP3).
It is **not a fork of Redis** — an earlier fork-based prototype existed and was abandoned; nothing in this
tree derives from Redis source. It implements **243 registered commands**. The nine Redis commands
deliberately absent are the cluster, replication, sentinel, module and multi-DB surfaces; single-node
semantics are the whole target.

It is a research system. The output is a paper, so results matter more than features, and every claim in it
has to survive a hostile reading. Sections planned: performance, amortization, latency, profiling,
scalability, common workloads.

## Architecture

**Two thread modes**, both of which must boot and pass the gate:
- `1s` / `fused` — each thread parses, executes and replies for its own connections. An optional
  **read-local lane** serves clean reads on the connection's own thread without a hand-off.
- `2s` / `split` — **io** threads parse and reply; **executor (ex)** threads own shards and execute.
  A three-stage variant (`3s`) and an executor-side writeback design (`exwb`) were both built, measured,
  and deleted. Do not propose either.

**Sharding and ownership.** Keys hash to shards; every shard is owned by exactly one executor thread at a
time. Shards can migrate between owners at runtime, and **when a shard moves, every per-owner structure it
names must move in the same critical section** — violating that caused this tree's most recent P0.

**Ordering ring (`rob`).** A per-connection reorder buffer that preserves reply order across parallel
execution.

**Atomics.** Multi-key atomicity uses group-scoped epoch-MVCC. Cross-shard commands use scatter-gather.
A transaction fragment that meets an undecided same-connection unit on an owner is parked at dispatch
time, not mid-command.

**Flip.** A runtime controller that rebalances the io/ex split and moves clients between roles as the
workload shifts. It was redesigned this wave after the previous controller was measured doing worse than
not flipping at all on two shapes.

**Load balancing.** Key-affine and client-affine balancing (`--key-lb`, `--client-lb`), both default on.

Networking is io_uring; the allocator is jemalloc.

## Non-negotiable correctness laws

Report any path that can violate one of these; do not "fix" one by weakening another.
- Single-owner writes.
- Reads are never obstructed by writes: immutable replacement plus QSBR. **No reader retries and no
  seqlocks on the per-operation path.**
- In-place overwrite is banned while the read-local lane is armed.
- Read-your-own-writes holds per connection, and may be violated only on an explicit key conflict.
- Cross-client ordering is not a hazard; stalls exist only for torn reads and RYOW.
- Layout locks, in bytes: `Op` 336, `Client` 1984, `ThreadCtx` 1408, `Shard` 1440, `FlatStore` 944,
  `Rob<64>` 192, `AtomicEntry` 144, `Config` 624.
- Both thread modes boot. Main commands carry zero regression. One file per feature.

## The box

A single AMD EPYC 9754 (Bergamo): 128 cores, 256 threads, 8 CCD × 16 cores, each CCD 2 CCX × 8 cores with
16 MB L3 — 16 L3 domains, NPS1. Measured node optimum is 8 cores, one CCX. Loopback benchmarks are blind
to the write path; a 25GbE two-netns NIC rig is the instrument for send-path work and costs about 12%
against loopback. Box noise is ±0.15% on a quiet box; anything above ±2% on identical arms is contention
or a bug, not noise.

**The gate boots nothing like a default server.** `tests/gate.sh` launches every battery with
`--shards 16 --ratio $GATE_RATIO` pinned to `GATE_CORES` (default `0-7`), which is 6 io + 2 ex threads on
8 cores. Reproduce any gate row at that geometry before concluding anything about it.

## What it is measured against

Redis 7.4 and 8.8.1, Dragonfly 1.40.2, KeyDB, Microsoft Garnet (net10.0), memcached 1.6.45, plus YCSB
0.17.0 traces. Binaries and digests: `/home/user/Projects/bench-bins/MANIFEST.md`.

We do not win everywhere and the paper will not claim we do. At 64 real cores and pipeline depth 128,
Garnet reaches 62.8M ops/s against our 34.4M. We hold depth 1, where we reach 4.35M. Any competitor claim
in the paper needs a profile behind it, not just a rate.

## How performance is judged

**cycles/op = instructions/op ÷ IPC.** Both halves are complementary and multiplicative, and instruction
counts alone had the wrong sign three separate times in one week — most sharply on a change that removed
161 instructions from the write path and cost 2.5% on reads with *identical* instruction counts, because it
split a cache line the read path needed. Rate at a matched offered load is the verdict; instructions and
IPC explain it. Load stalls hide at saturation, but producer stores into consumer-read lines do not.

Every optimization lands with a PRE vs POST table. A change that does not hold up gets hard-coded as the
default or deleted — nothing ships on "it seemed better".

## Why this work looks the way it does

Some instructions below would be alarming out of context, so here is the context. This is a single-user
research box. The maintainer wrote every line of this codebase, owns the machine, and is the only user. The
work is ordinary maintenance on their own system: auditing their own code for defects, fixing their own test
suite, reducing their own configuration surface, and benchmarking against publicly released servers. Nothing
here touches anyone else's data, systems or accounts.

Four specifics that read oddly without the reason:

1. **"Never run the server, the benchmark or the gate."** This is about measurement integrity and nothing
   else. Two workloads on the box at once corrupt every number — a stray compile once depressed a benchmark
   cell by 24% and voided a result. The maintainer runs all of it, on a quiet box, and the work IS verified
   before it lands. You are being asked to produce a diff, not to skip verification.

2. **"Never edit `EXPECT_QUICK` / `EXPECT_FULL`."** Those constants are how many test rows must pass. Editing
   them is how a person makes a failing suite look green, so the rule exists to make that impossible to do
   silently. It is a stricter standard than normal, not a looser one. State what the count should become and
   why; the maintainer changes it.

3. **Concurrency vocabulary.** This is a database. "Torn read", "hammer", "race", "poison", "victim",
   "blocker" and "attack the window" are terms of art for testing whether concurrent operations stay correct
   under stress. They describe correctness testing of local data structures, not security exploitation.

4. **Deliberately breaking a mechanism to check a test.** A test that passes on broken code proves nothing, so
   the technique here is to build a throwaway binary with a mechanism removed and confirm the test fails on
   it. That build is never committed and never runs outside the box. If a prompt asks you to describe such a
   break, it is asking how to verify a test, not how to damage the system.

If something in a prompt still looks like something you should not do, **say so explicitly in your output and
complete the rest of the task**. A stated objection is useful. Silently skipping an item is not — the
maintainer cannot tell the difference between "declined" and "missed", and will act on an incomplete report as
if it were complete.

## Working rules for codex

- **Never run a server, a benchmark, or the gate.** The maintainer runs the box. Report, or produce a diff.
- Edit only inside your own worktree.
- Never edit `EXPECT_QUICK` / `EXPECT_FULL` in `tests/gate.sh`. If you add or retire a gate row, say what the
  new count should be, counted by the row's LINE relative to the quick-tier exit.
- Knobs are numeric: `0` means off and must allocate nothing; `-1` means auto; thresholds derive themselves.
  Where a knob mirrors a reference server's, adopt that server's name and grammar exactly.
- A test that cannot fail proves nothing. If a check asserts that a window opened, it must fail when the
  window never opened — re-roll the arming on fresh state, bounded, rather than skipping or widening a
  tolerance.

## Closed doors — measured, negative, do not re-propose

`3s` three-stage mode; `exwb` executor-side writeback (−11 to −22%); AMAC and any CR/MR split (the only
boundary is io/ex); reorder-and-window; seqlocks on the per-op path; connection-side record allocation
(`connalloc`, cycles flat, record born on the wrong core); executor-major task collapse (`exmajor`,
−2 to −3%); the read-local capacity hand-off line split (`t-pubdiet`, +1.97% SET but −2.50% GET).

--- TASK ---

You are performing a three-way merge of two independently developed lanes in a C++20 key-value server, and
resolving one genuine SEMANTIC conflict between them. Work only in this worktree. Do not build, do not run the
server, benchmarks or the gate — the maintainer runs those and will gate the merged result.

THE INPUTS. Common ancestor: commit 78c3e5391, which is this worktree's HEAD and is also the base both lanes
branched from. The two lanes are uncommitted working trees you may read directly:
  * /home/user/Projects/cx-timing  — test-suite determinism work. ALREADY GATED CLEAN at 344 ok / 0 FAIL with
    zero SKIP rows. Its central change adds a `--release-build` flag so timing assertions run only on release
    builds while counter movement, exact values and safety bounds stay mandatory on every build.
  * /home/user/Projects/cx-knobs   — configuration-surface reduction, 40 files. Deletes a set of knobs, folds
    eight load-balancer knobs into one `--lb`, and changes `sizeof(Config)` from 624 to 528. It states the gate
    counts must become quick 325 / full 342. Read its DESIGN-KNOBS.md, including its OBJECTIONS section.

Produce a single merged working tree here containing BOTH lanes' work.

TEXTUAL CONFLICTS, three files both lanes touched: tests/atomic_torn.py, tests/gate.sh,
tests/xshard_dispatch_scale.sh. For each, take both sides' intent, not one side's text.

THE ONE THAT MATTERS, in tests/atomic_torn.py: the BASE already contains a bounded re-roll loop for the OFF
torn-RENAME control (`for _roll in range(4)`, carrying the comment that it does NOT degrade to a skip, unlike
the SINTERSTORE, COPY and RENAMENX controls beside it). cx-timing then layers its release-build split on the
same file. BOTH must survive the merge: the re-roll loop and its hard-failure policy, AND the mechanism/timing
split. If you believe they interact, say how and choose deliberately — do not let one silently overwrite the
other.

THE SEMANTIC CONFLICT. cx-knobs deletes `--shard-home`, and argues in its objections that this removes the
instrument the cross-shard dispatch-scaling gate row depended on: the retired wrapper kept all 16 shards on two
real executors and added empty filler executors via the manual map, so without it the row would assert an
unvalidated claim on a different geometry. cx-knobs therefore RETIRES that row. cx-timing, independently, was
told to FIX that same row (it passes today only by taking the best of two pairs while both arms routinely
exceed its 1.20 threshold) and rewrote its threshold logic.

Decide this, and justify it: does cx-timing's rewrite make the row valid WITHOUT `--shard-home`? If yes, keep
the fixed row and drop the retirement. If no, the retirement wins and cx-timing's rewrite of that row is moot —
say so plainly rather than keeping dead code. Read both sides before deciding; do not split the difference.

RULES.
  * Never silently drop a hunk. Every change from either lane is kept, deliberately superseded, or dropped with
    a stated reason. A merge that loses work without saying so is the failure mode here.
  * Do not edit `EXPECT_QUICK` or `EXPECT_FULL` in tests/gate.sh. State what they must become, with the
    arithmetic: start from 327 quick / 344 full, then list every row added or retired and which side of the
    quick-tier exit (`[ "$TIER" = quick ]`) its emitting LINE falls on. cx-knobs claims 325 / 342 — verify that
    against the merged tree rather than trusting it, and correct it if the merge changes the count.
  * Preserve every correctness law in the project context above. If the two lanes' changes together violate one
    that neither violated alone, stop at that point and report it.
  * The merged tree must be internally consistent: no reference to a deleted knob, no test invoking a removed
    flag, no documentation describing a retired instrument as live.

DELIVERABLE: the merged working tree, plus MERGE.md with a per-conflict record (file, both intents, the
decision, the reason), the semantic-conflict ruling with its evidence, the EXPECT arithmetic, and a list of
anything you dropped and why. The maintainer gates the result; your job is that the merge is honest and
complete, not that it passes.
codex
I’ll compare both lanes against the common base, resolve the dispatch-scaling conflict from the code and objections, and merge into this worktree. I’ll record every superseded change in `MERGE.md`. I won’t build or run the server, benchmarks, or gate.
exec
/bin/bash -lc 'for f in /AGENTS.md /home/AGENTS.md /home/user/AGENTS.md /home/user/Projects/AGENTS.md; do if [ -f "$f" ]; then echo "$f"; cat "$f"; fi; done' in /home/user/Projects/cx-merge
 succeeded in 0ms:
exec
/bin/bash -lc 'git status --short && git rev-parse HEAD && git diff --stat && git ls-files --others --exclude-standard' in /home/user/Projects/cx-timing
 succeeded in 0ms:
 M DESIGN-GATEHYGIENE.md
 M tests/atomic_torn.py
 M tests/borrow_registry.py
 M tests/differ.py
 M tests/expireindex.py
 M tests/gate.sh
 M tests/infofix.py
 M tests/pubsub.py
 M tests/slowlog.py
 M tests/stream.py
 M tests/xshard_dispatch_scale.py
 M tests/xshard_dispatch_scale.sh
?? CODEX-OUT.md
78c3e53919a5d557bd3dd4625c9524d0baa212e4
 DESIGN-GATEHYGIENE.md          | 57 +++++++++++++++++++++++++
 tests/atomic_torn.py           | 88 ++++++++++++++++++++++++++++++--------
 tests/borrow_registry.py       | 48 ++++++++++++++++++---
 tests/differ.py                | 64 +++++++++++++++++++++-------
 tests/expireindex.py           | 40 ++++++++++++++----
 tests/gate.sh                  | 24 +++++++----
 tests/infofix.py               | 49 +++++++++++++++++-----
 tests/pubsub.py                | 18 ++++++++
 tests/slowlog.py               | 23 ++++++++--
 tests/stream.py                | 49 +++++++++++++++++++---
 tests/xshard_dispatch_scale.py | 10 +++--
 tests/xshard_dispatch_scale.sh | 95 +++++++++++++++++++++++++++++-------------
 12 files changed, 457 insertions(+), 108 deletions(-)
CODEX-OUT.md

exec
/bin/bash -lc 'git status --short && git rev-parse HEAD && git diff --stat && git ls-files --others --exclude-standard' in /home/user/Projects/cx-knobs
 succeeded in 0ms:
 A DESIGN-KNOBS.md
 M docs/specs/AUDIT-AOF.md
 M docs/thread-mode.md
 M src/base/topology.h
 M src/cmd/lbsignals.cc
 M src/cmd/server_tail.cc
 M src/cmd/t_server.cc
 M src/core/config.h
 M src/core/ex_loop.h
 M src/core/flipctl.cc
 M src/core/io_loop.h
 M src/core/iopipe_pipeline.h
 M src/core/placement.h
 M src/core/server.h
 M src/core/shard.h
 M src/core/weighted_lb.h
 M src/main.cc
 M src/net/tls.cc
 M src/net/uring.h
 M src/persist/aof.cc
 M src/store/eviction.h
 M src/store/flatstore.h
 M tests/_lib.py
 M tests/aof_rewrite_matrix.sh
 M tests/aof_rewrite_trigger_matrix.sh
 M tests/atomic_ryow.py
 M tests/atomic_torn.py
 A tests/atomicwindow.py
 M tests/bplus.py
 M tests/config_parser_test.cc
 M tests/evict_battery.py
 M tests/flipctl.py
 M tests/gate.sh
 A tests/knobs.py
 M tests/lbsignals.py
 M tests/rlcache_churn.py
 M tests/tls.py
 M tests/xscript.py
 D tests/xshard_dispatch_scale.sh
 M tomokv.conf
?? CODEX-OUT.md
78c3e53919a5d557bd3dd4625c9524d0baa212e4
 DESIGN-KNOBS.md                     | 324 ++++++++++++++++++++++++++++++++
 docs/specs/AUDIT-AOF.md             |  10 +-
 docs/thread-mode.md                 | 108 +++--------
 src/base/topology.h                 |  42 ++++-
 src/cmd/lbsignals.cc                |   4 +-
 src/cmd/server_tail.cc              |   7 +-
 src/cmd/t_server.cc                 |  61 +-----
 src/core/config.h                   | 358 ++++++------------------------------
 src/core/ex_loop.h                  |  37 ++--
 src/core/flipctl.cc                 |  28 ++-
 src/core/io_loop.h                  |  10 +-
 src/core/iopipe_pipeline.h          |   2 +-
 src/core/placement.h                |  80 ++------
 src/core/server.h                   | 228 +++++++++++++----------
 src/core/shard.h                    |   6 +-
 src/core/weighted_lb.h              |  79 ++++++++
 src/main.cc                         |  21 +--
 src/net/tls.cc                      |   4 +-
 src/net/uring.h                     |   2 +-
 src/persist/aof.cc                  |   2 +-
 src/store/eviction.h                |   3 +
 src/store/flatstore.h               |  45 ++---
 tests/_lib.py                       |   2 +-
 tests/aof_rewrite_matrix.sh         |   8 +-
 tests/aof_rewrite_trigger_matrix.sh |   4 +-
 tests/atomic_ryow.py                |  28 ++-
 tests/atomic_torn.py                | 107 ++---------
 tests/atomicwindow.py               | 225 ++++++++++++++++++++++
 tests/bplus.py                      |  11 +-
 tests/config_parser_test.cc         | 270 ++++++++++-----------------
 tests/evict_battery.py              |  57 +++---
 tests/flipctl.py                    |  34 ++--
 tests/gate.sh                       | 276 +++++++++++++--------------
 tests/knobs.py                      |  48 +++++
 tests/lbsignals.py                  |   6 +-
 tests/rlcache_churn.py              |  13 +-
 tests/tls.py                        |   4 +-
 tests/xscript.py                    |  82 ++++-----
 tests/xshard_dispatch_scale.sh      | 137 --------------
 tomokv.conf                         | 168 ++++-------------
 40 files changed, 1405 insertions(+), 1536 deletions(-)
CODEX-OUT.md

exec
/bin/bash -lc "pwd && git status --short && git rev-parse HEAD && rg --files -g AGENTS.md -g '"'!build*'"' -g '"'!vendor*'"' -g '"'!third_party*'"'" in /home/user/Projects/cx-merge
 exited 1 in 0ms:
/home/user/Projects/cx-merge
?? CODEX-OUT.md
78c3e53919a5d557bd3dd4625c9524d0baa212e4

exec
/bin/bash -lc 'git diff -- DESIGN-GATEHYGIENE.md tests/atomic_torn.py tests/gate.sh tests/xshard_dispatch_scale.sh tests/xshard_dispatch_scale.py' in /home/user/Projects/cx-timing
 succeeded in 0ms:
diff --git a/DESIGN-GATEHYGIENE.md b/DESIGN-GATEHYGIENE.md
index 1ad3bfc4c..5ea9df93e 100644
--- a/DESIGN-GATEHYGIENE.md
+++ b/DESIGN-GATEHYGIENE.md
@@ -456,3 +456,60 @@ stable hold: 30s on a measured-stationary load, no trigger or split movement (at
 
 `flip.py` needed no in-flight re-roll in any post-fix run (0 re-roll lines), so the budget is
 headroom rather than a routine cost. The flipctl row costs 120 s, unchanged from before this lane.
+
+---
+
+## 10. Follow-up diff on `78c3e5391` — 2026-09-07, runtime validation pending
+
+This is a test-only diff in `cx-timing`. No server, benchmark, gate, or compiler was run. The
+earlier acceptance results above describe earlier changes, **not this diff**. The worked fixes
+in `multirace.py` and the RENAME OFF control are unchanged.
+
+| area | change | failure induction (also documented beside the checks) |
+| --- | --- | --- |
+| `differ.py`, blocking | Wait for both blocked-client gauges to reach zero before final comparison. Assert each server's gauge increment independently, against a settled baseline. | Suppress registration increments to fail movement; suppress disconnect decrements to fail the bounded zero control. |
+| `atomic_torn.py`, promotion/leases | Keep promotion movement and credit safety mandatory. Separate the original 1.5s promotion and 3s shrink/reclaim budgets behind `--release-build`. Sample the credit bound across at least 32 observations with completed writer traffic, and require zero inflight/debt plus the exact final credit pool. | Disable promotion or lease returns; over-issue credits after shrink. An observed excess or exhausted mechanism watchdog fails on every build. |
+| `slowlog.py` | Derive recording-thread capacity from INFO (`fused_threads`, or `io_threads + ex_threads`), and issue more writes than its aggregate capacity. | Disable ring eviction or max-len trimming. |
+| `xshard_dispatch_scale.sh` / `.py` | Disable balancing during the fixed-owner experiment; verify ownership remained fixed. Use distinct reply payloads, check every pair's topology, alternate boot order, and retain every ratio. Only score release timings when repeated arms and same-shard controls agree within the supplied 2% box contract. Every qualified pair must meet the unchanged 1.20 bound; no best-pair selection. | Misroute a key or change a second pair's fan-out to fail mechanism checks; restore the all-thread scan to fail a stable release measurement. Unqualified timings say SKIP with all numbers, not that scaling passed. |
+| `pubsub.py` | Drain old pattern registrations before checking the next NUMPAT population; keep exact live-pattern assertions. | Leak the disconnected RESP3 subscriber's pattern; a constant-zero NUMPAT still fails the live arms. |
+| `borrow_registry.py` | Replace the 1.5x timing proxy for different paths with `zc_sends`/`zc_releases` deltas and a plain-GET negative control before holders exist. Keep parked-borrow/drain and byte-exact reply checks on every build; cost budgets require `--release-build`. | Disable borrowing, borrow below the cutover, or omit releases to fail mechanisms; restore the linear lookup to fail the qualified release growth budget. |
+| `expireindex.py` | Assert exact 2 -> 1 -> 0 TTL accounting, DEL replies, and the surviving value on every repetition. Keep INFO outside both timed DEL intervals. Cost ratios require `--release-build`. | Break expiry unregistration or delete accounting to fail mechanisms; restore the historical-capacity memset to fail the release timing bound. |
+| `infofix.py` and its differential counterpart | Poll INFO-only idle samples until zero; feed verified PINGs until the sampler reports work. Also require RESETSTAT to clear those PING counters, so natural rate decay cannot substitute for a reset. | Pin the rate at zero/nonzero, or omit the command-counter reset baseline. |
+| `stream.py` | Check exact retained entries through trimming, first-entry creation and embedded-to-macro growth on every build. Report all footprint measurements; score the original byte ceilings only with `--release-build`. Require positive measured allocation instead of clamping negative deltas to zero. | Drop/wrongly trim entries or omit memory accounting to fail mechanisms; inflate first-entry/node allocations to fail release footprint bounds. |
+
+The release gate explicitly passes `--release-build` to the affected batteries. The ASAN atomic
+row omits it. Standalone runs against an instrumented binary should likewise omit it; all
+mechanism assertions remain active and performance measurements still appear in the log.
+
+### Outstanding atomic window calibration — not claimed fixed
+
+Three window-hit checks remain strict at their existing arming geometry:
+
+* `atomic_torn.py:718`, commit-delay held a read cut;
+* `atomic_torn.py:719`, predecessor resolution;
+* `atomic_torn.py:923`, admission stalls and resumes.
+
+Their own per-attempt hit/miss measurements were not supplied. The RENAME control's measured
+5/6 hit rate does not establish a bound for these different mechanisms. No fresh-connection
+retry count was guessed, no window-hit assertion was made release-only, and their observation
+windows were not widened. Fresh-state retry hygiene for these three checks still needs the
+maintainer's measurements at `--shards 16 --ratio 6:2`, cores `0-7`, separately for release and
+ASAN. Record each attempt's counter deltas and any real value/worker failure; such failures must
+stop retrying immediately. This is the unfinished part of item 1, not an acceptance claim.
+
+### Static and offline checks
+
+Python AST parsing, `bash -n` and `git diff --check` passed. Source-extracted helper checks with
+synthetic inputs covered successful and exhausted gauge/pattern polls, 8/16-thread INFO
+geometries, release-flag behavior, exact TTL transitions, stuck sampled rates, and excess/no-work
+lease samples. The dispatch result evaluator rejected a synthetic stable 1.30 ratio, incorrect
+second-pair geometry, negative excess and a missing pair, while explicitly skipping unstable
+timings. These are harness checks, **not** server fault-injection or PRE/POST measurements.
+
+### Ledger
+
+No top-level row was added or retired; the `ok`/`bad` label sequence is unchanged. In this diff
+the quick exit is at `gate.sh:1262`. The release atomic row (`:434`), stream feature-loop calls
+(`:457`, `:516`), expiry (`:674`), borrow (`:682`) and dispatch (`:688`) rows are before it; the
+ASAN atomic row (`:1275`) is after it. Internal assertions do not add ledger rows.
+`EXPECT_QUICK=327` and `EXPECT_FULL=344` remain unchanged, and no count adjustment is needed.
diff --git a/tests/atomic_torn.py b/tests/atomic_torn.py
index 448e00be6..7c1fb361e 100755
--- a/tests/atomic_torn.py
+++ b/tests/atomic_torn.py
@@ -10,9 +10,17 @@ import time
 import _lib
 
 
-HOST, PORT = sys.argv[1], int(sys.argv[2])
+# Build identity comes from the caller. Counter movement, exact values, and safety bounds are
+# mandatory on every build; only the time spent reaching them is a release-build assertion.
+ARGS = sys.argv[1:]
+RELEASE_BUILD = "--release-build" in ARGS
+ARGS = [arg for arg in ARGS if arg != "--release-build"]
+if len(ARGS) != 2:
+    raise SystemExit("usage: atomic_torn.py HOST PORT [--release-build]")
+HOST, PORT = ARGS[0], int(ARGS[1])
 FAIL = 0
 BARRIER_TIMEOUT = 10
+MECHANISM_TIMEOUT = 30.0  # liveness watchdog, never a successful/skip outcome on exhaustion
 
 
 def note(name, ok, extra=""):
@@ -26,6 +34,13 @@ def skip(name, extra=""):
     print("  SKIP " + name + (" " + extra if extra else ""), flush=True)
 
 
+def release_note(name, ok, measured):
+    if RELEASE_BUILD:
+        note(name, ok, measured)
+    else:
+        skip(name, "requires --release-build; " + measured)
+
+
 def abort_barriers(*barriers):
     for barrier in barriers:
         try:
@@ -669,6 +684,7 @@ note("OFF control exposes torn MSET-8",
 before = stats()
 required_stat(before, "atomic_predecessor_reads")
 required_stat(before, "atomic_commit_holds")
+required_stat(before, "atomic_promotions")
 debug("ATOMIC-COMMIT-DELAY", 2000)
 try:
     on_torn, on_reads, on_errors, _, _, on_threads_still_alive = hammer(
@@ -680,21 +696,32 @@ pred_delta = (required_stat(after, "atomic_predecessor_reads") -
               required_stat(before, "atomic_predecessor_reads"))
 hold_delta = (required_stat(after, "atomic_commit_holds") -
               required_stat(before, "atomic_commit_holds"))
-promo_delta = after.get("atomic_promotions", 0) - before.get("atomic_promotions", 0)
+promo_delta = required_stat(after, "atomic_promotions") - required_stat(before, "atomic_promotions")
 # V2 batches reclamation on owner passes and the 50ms low-frequency sweep instead of posting a
-# cleanup task for every retired group. Give that deliberately cold path one bounded tick to fire.
-deadline = time.time() + 1.5
-while promo_delta == 0 and time.time() < deadline:
+# cleanup task for every retired group. Poll completed work's cleanup with a watchdog; only the
+# original 1.5s completion budget is release-specific. This does not re-arm any race window.
+promotion_started = time.monotonic()
+deadline = promotion_started + MECHANISM_TIMEOUT
+while promo_delta == 0 and time.monotonic() < deadline and not on_errors and not on_torn:
     time.sleep(0.05)
     after = stats()
-    promo_delta = after.get("atomic_promotions", 0) - before.get("atomic_promotions", 0)
+    promo_delta = required_stat(after, "atomic_promotions") - required_stat(before, "atomic_promotions")
+promotion_elapsed = time.monotonic() - promotion_started
 note("ON MSET-8/MGET-8 torn-free",
      on_torn == 0 and on_reads > 0 and not on_errors and not on_threads_still_alive,
      "torn=%d reads=%d errors=%r threads_still_alive=%r" %
      (on_torn, on_reads, on_errors, on_threads_still_alive))
+# These are window-hit claims, NEVER release-only or skip-on-miss. Induce respectively by
+# removing safe-cut holds, bypassing predecessor lookup, or disabling promotion. Each still
+# fails on ASAN with a zero delta. Fresh-connection retry bounds for the first two need their own
+# measured hit rates; the RENAME control's 5/6 measurement cannot calibrate a different window.
 note("ON commit-delay window held a read cut", hold_delta > 0, "delta=%d" % hold_delta)
 note("ON exercised predecessor resolution", pred_delta > 0, "delta=%d" % pred_delta)
 note("ON exercised promotion", promo_delta > 0, "delta=%d" % promo_delta)
+release_note("ON promotion drains within release budget",
+             promo_delta > 0 and promotion_elapsed <= 1.5,
+             "promotion-drain=%.3fs holds=%d predecessors=%d promotions=%d" %
+             (promotion_elapsed, hold_delta, pred_delta, promo_delta))
 note("atomic_inflight returns to idle", after.get("atomic_inflight", -1) == 0,
      "value=%d" % after.get("atomic_inflight", -1))
 
@@ -870,7 +897,7 @@ else:
 # deterministic instead of depending on Python threads winning a scheduling race.
 config("atomic", 1)
 config("atomic-window", 1)
-window_before = stats().get("atomic_window_stalls", 0)
+window_before = required_stat(stats(), "atomic_window_stalls")
 window_run = format(time.time_ns() & 0xfffffff, "x")
 window_errors = []
 window_client = Resp()
@@ -890,7 +917,9 @@ except Exception as exc:
     window_errors.append(str(exc))
 finally:
     window_client.close()
-window_after = stats().get("atomic_window_stalls", 0)
+window_after = required_stat(stats(), "atomic_window_stalls")
+# A miss remains a failure on every build. Induce by bypassing the admission credit check;
+# replies alone cannot prove backpressure. No fresh-state retries without measured hit rates.
 note("atomic-window stalls and resumes",
      not window_errors and window_after > window_before,
      "stalls=%d errors=%r" % (window_after - window_before, window_errors))
@@ -902,6 +931,7 @@ config("atomic-window", 256)
 # have returned every leased credit to the pool so a skewed next IO can consume the whole window.
 lease_stop = threading.Event()
 lease_errors = []
+lease_completed = [0] * 8
 
 
 def lease_writer(wid):
@@ -914,6 +944,7 @@ def lease_writer(wid):
             if mset(client, keys, "lease:%d:%d" % (wid, seq)) != b"OK":
                 raise AssertionError("bad lease reply")
             seq += 1
+            lease_completed[wid] = seq
     except Exception as exc:
         lease_errors.append("writer%d:%s" % (wid, exc))
     finally:
@@ -927,10 +958,11 @@ for thread in lease_threads:
 for value in (31, 7, 19, 3):
     if not config("atomic-window", value):
         lease_errors.append("CONFIG atomic-window %d failed" % value)
-deadline = time.time() + 3
+shrink_started = time.monotonic()
+deadline = shrink_started + MECHANISM_TIMEOUT
 bounded = False
 max_inflight = 0
-while time.time() < deadline:
+while time.monotonic() < deadline:
     sample = stats()
     live = sample.get("atomic_inflight", 1000000)
     debt = sample.get("atomic_credit_debt", 1000000)
@@ -938,30 +970,52 @@ while time.time() < deadline:
         bounded = True
         break
     time.sleep(0.01)
+shrink_elapsed = time.monotonic() - shrink_started
+bound_samples = 0
+bound_completed_before = sum(lease_completed)
+bound_completed_after = bound_completed_before
 if bounded:
-    deadline = time.time() + 0.4
-    while time.time() < deadline:
-        live = stats().get("atomic_inflight", 1000000)
+    # Assert values across completed observations, not the number of polls a 0.4s slice happened
+    # to fit. Traffic must complete during this interval, or the bound could pass on idle zeroes.
+    # Induce by over-issuing credits after shrink: ANY live > 3 fails, on release and ASAN alike.
+    deadline = time.monotonic() + MECHANISM_TIMEOUT
+    while time.monotonic() < deadline and not lease_errors:
+        sample = stats()
+        live = required_stat(sample, "atomic_inflight")
+        bound_completed_after = sum(lease_completed)
+        bound_samples += 1
         max_inflight = max(max_inflight, live)
         if live > 3:
             bounded = False
             break
+        if bound_samples >= 32 and bound_completed_after > bound_completed_before:
+            break
+    bounded = bounded and bound_samples >= 32 and bound_completed_after > bound_completed_before
 lease_stop.set()
 for thread in lease_threads:
     thread.join(timeout=10)
-deadline = time.time() + 3
+reclaim_started = time.monotonic()
+deadline = reclaim_started + MECHANISM_TIMEOUT
 lease_after = stats()
-while lease_after.get("atomic_inflight", -1) != 0 and time.time() < deadline:
+while (lease_after.get("atomic_inflight", -1) != 0 or
+       lease_after.get("atomic_credit_debt", -1) != 0 or
+       lease_after.get("atomic_credit_pool", -1) != 3) and time.monotonic() < deadline:
     time.sleep(0.01)
     lease_after = stats()
+reclaim_elapsed = time.monotonic() - reclaim_started
+# Suppress idle lease returns to induce a nonzero debt or pool != 3; exhausted polls still FAIL.
 note("atomic-window reconfiguration preserves bound and reclaims leases",
      bounded and not any(thread.is_alive() for thread in lease_threads) and not lease_errors and
      lease_after.get("atomic_inflight", -1) == 0 and
      lease_after.get("atomic_credit_debt", -1) == 0 and
      lease_after.get("atomic_credit_pool", -1) == 3,
-     "max=%d pool=%d debt=%d errors=%r" %
+     "max=%d pool=%d debt=%d errors=%r samples=%d completed=%d shrink=%.3fs reclaim=%.3fs" %
      (max_inflight, lease_after.get("atomic_credit_pool", -1),
-      lease_after.get("atomic_credit_debt", -1), lease_errors))
+      lease_after.get("atomic_credit_debt", -1), lease_errors, bound_samples,
+      bound_completed_after - bound_completed_before, shrink_elapsed, reclaim_elapsed))
+release_note("atomic-window shrink/reclaim within release budgets",
+             shrink_elapsed <= 3.0 and reclaim_elapsed <= 3.0,
+             "shrink=%.3fs reclaim=%.3fs" % (shrink_elapsed, reclaim_elapsed))
 config("atomic-window", 256)
 
 # Live CONFIG flips under active traffic are a liveness/safety arm. OFF intervals deliberately do
diff --git a/tests/gate.sh b/tests/gate.sh
index c4d8fef19..c6428f573 100755
--- a/tests/gate.sh
+++ b/tests/gate.sh
@@ -431,7 +431,7 @@ E=$(shutdown_value work.executed)
 # Explicit ON boot plus the non-vacuous epoch-MVCC gates. atomic_torn includes its own OFF control,
 # predecessor/promotion counters, overlapping writers, window liveness, and live CONFIG flips.
 boot ./build/tomokv --atomic 1 --enable-debug-command yes || bad "atomic release boot"
-py tests/atomic_torn.py 127.0.0.1 $PORT >/tmp/gate-atomic-torn.txt 2>&1 \
+py tests/atomic_torn.py 127.0.0.1 $PORT --release-build >/tmp/gate-atomic-torn.txt 2>&1 \
     && ok "atomic torn/window battery" || bad "atomic torn/window battery" "see /tmp/gate-atomic-torn.txt"
 py tests/atomic_ryow.py 127.0.0.1 $PORT >/tmp/gate-atomic-ryow.txt 2>&1 \
     && ok "atomic RYOW/mixed-write battery" || bad "atomic RYOW/mixed-write battery" "see /tmp/gate-atomic-ryow.txt"
@@ -452,7 +452,9 @@ for AT in 0 1; do
   boot ./build/tomokv --atomic $AT --enable-debug-command yes \
       || bad "feature battery boot (atomic $AT)"
   for t in $FEATURE_BATTERIES; do
-    py tests/$t.py 127.0.0.1 $PORT >/tmp/gate-$t-$AT.txt 2>&1 \
+    FEATURE_ARGS=()
+    [ "$t" = stream ] && FEATURE_ARGS+=(--release-build)
+    py tests/$t.py 127.0.0.1 $PORT "${FEATURE_ARGS[@]}" >/tmp/gate-$t-$AT.txt 2>&1 \
         && ok "$t battery (atomic $AT)" || bad "$t battery (atomic $AT)" "see /tmp/gate-$t-$AT.txt"
   done
   stop
@@ -509,7 +511,9 @@ for AT in 0 1; do
     bad "fused+armed boot line (atomic $AT)" "server did not boot; see $SRVLOG"
   fi
   for t in $FEATURE_BATTERIES; do
-    py tests/$t.py 127.0.0.1 $PORT >/tmp/gate-fusedarmed-$t-$AT.txt 2>&1 \
+    FEATURE_ARGS=()
+    [ "$t" = stream ] && FEATURE_ARGS+=(--release-build)
+    py tests/$t.py 127.0.0.1 $PORT "${FEATURE_ARGS[@]}" >/tmp/gate-fusedarmed-$t-$AT.txt 2>&1 \
         && ok "fused+armed $t battery (atomic $AT)" \
         || bad "fused+armed $t battery (atomic $AT)" "see /tmp/gate-fusedarmed-$t-$AT.txt"
   done
@@ -661,13 +665,13 @@ for XS in "off:--script-crossshard-max-bytes 0" \
 done
 
 # ---- efficiency guards: each needs a boot geometry of its own, so each gets its own server ----
-# These assert a RATIO or a growth bound, never an absolute time, and each was verified to FAIL on
-# the binary from before its fix (see NOTES-XPERF2.md). They shipped unwired, which is the same
-# coverage-that-does-not-exist problem the gate lane fixed elsewhere.
+# Exact mechanism checks run on every build; timing budgets require the explicit release-build
+# flag. A ratio of wall times is still a performance claim (see DESIGN-GATEHYGIENE.md). The
+# cross-shard guard also checks instrument stability before scoring any timing, retaining all pairs.
 # --shards 1 for the same reason as the borrow guard: the sidecar under test is per shard, and the
 # battery asserts that precondition rather than quietly measuring a diluted one.
 boot ./build/tomokv --shards 1 --enable-debug-command yes || bad "expire-index guard boot"
-py tests/expireindex.py 127.0.0.1 $PORT >/tmp/gate-expireindex.txt 2>&1 \
+py tests/expireindex.py 127.0.0.1 $PORT --release-build >/tmp/gate-expireindex.txt 2>&1 \
     && ok "expire-index growth bound" || bad "expire-index growth bound" "see /tmp/gate-expireindex.txt"
 stop
 
@@ -675,13 +679,13 @@ stop
 # an ordinary-sized value still takes the borrow path and pays registry cost.
 boot ./build/tomokv --shards 1 --zc-min 64 --client-output-buffer-limit "normal 0 0 0" \
     --enable-debug-command yes || bad "borrow-registry guard boot"
-py tests/borrow_registry.py 127.0.0.1 $PORT >/tmp/gate-borrow.txt 2>&1 \
+py tests/borrow_registry.py 127.0.0.1 $PORT --release-build >/tmp/gate-borrow.txt 2>&1 \
     && ok "borrow-registry growth bound" || bad "borrow-registry growth bound" "see /tmp/gate-borrow.txt"
 stop
 
 # this one boots its own arms at two thread counts, so it takes the port/cores rather than a server
 quiet_wait
-XDS_PORT=$PORT XDS_CPUS=$CORES XDS_BIN=./build/tomokv bash tests/xshard_dispatch_scale.sh \
+XDS_PORT=$PORT XDS_CPUS=$CORES XDS_BIN=./build/tomokv bash tests/xshard_dispatch_scale.sh --release-build \
     >/tmp/gate-xds.txt 2>&1 \
     && ok "cross-shard dispatch scaling" || bad "cross-shard dispatch scaling" "see /tmp/gate-xds.txt"
 
@@ -1266,6 +1270,8 @@ py tests/torture.py 127.0.0.1 $PORT >/tmp/gate-tort-asan.txt 2>&1 \
     && ok "torture under ASAN" || bad "torture under ASAN"
 py tests/ryow.py 127.0.0.1 $PORT >/tmp/gate-ryow-asan.txt 2>&1 \
     && ok "RYOW under ASAN" || bad "RYOW under ASAN"
+# Deliberately omit --release-build: all five coverage/safety checks remain mandatory, while
+# their measured completion times are printed without scoring release scheduling budgets.
 py tests/atomic_torn.py 127.0.0.1 $PORT >/tmp/gate-atomic-torn-asan.txt 2>&1 \
     && ok "atomic torn/window under ASAN" || bad "atomic torn/window under ASAN"
 # --no-rate-assertions: the battery's overlap section makes one claim about SPEED (pipelining 24
diff --git a/tests/xshard_dispatch_scale.py b/tests/xshard_dispatch_scale.py
index 5ddf26fe5..4e85fbda5 100755
--- a/tests/xshard_dispatch_scale.py
+++ b/tests/xshard_dispatch_scale.py
@@ -100,9 +100,11 @@ def main():
         print("FAIL: could not find both a two-owner cross-shard pair and a same-shard pair")
         sys.exit(1)
     keys = [base % 0, cross[0], same[0]]
-    for k in keys:
-        assert c.cmd("SET", k, b"x") == b"OK"
-    reply = b"*2\r\n$1\r\nx\r\n$1\r\nx\r\n"
+    # Different payloads make returning the first shard twice fail byte-for-byte; identical 'x'
+    # values could conceal that regression. Cross and same still have identical reply shapes.
+    for index, k in enumerate(keys):
+        assert c.cmd("SET", k, b"x" if index == 0 else b"y") == b"OK"
+    reply = b"*2\r\n$1\r\nx\r\n$1\r\ny\r\n"
     fcross = enc("MGET", base % 0, cross[0])
     fsame = enc("MGET", base % 0, same[0])
     c.rate(fcross, reply, DEPTH * 50, DEPTH)
@@ -118,6 +120,8 @@ def main():
         excess.append(cn - sn)
         crosses.append(cn)
         sames.append(sn)
+    if c.shard_owners() != owners:
+        raise AssertionError("shard ownership moved during the arm; disable key/client balancing")
     print("ARM threads=%d excess_ns=%.1f cross_ns=%.1f same_ns=%.1f cross_owners=%d same_owners=%d"
           % (c.threads(), statistics.median(excess), statistics.median(crosses),
              statistics.median(sames),
diff --git a/tests/xshard_dispatch_scale.sh b/tests/xshard_dispatch_scale.sh
index 019243ed5..92c5a0699 100755
--- a/tests/xshard_dispatch_scale.sh
+++ b/tests/xshard_dispatch_scale.sh
@@ -15,10 +15,16 @@
 #               nothing the measurement depends on.
 # Same shards, same owners, same serving IO thread, same offered load, same client.
 #
-# The assertion is a RATIO between two arms, never an absolute time. It is also non-vacuous:
+# Exact replies and owner fan-out are checked on every build. With --release-build, score the
+# timing ratio only when repeated arms and the same-shard control certify a stable instrument:
 # tests/xshard_dispatch_scale.py refuses to report unless DEBUG SHARD proves the "cross" pair
 # really spans two shards, and it measures a same-shard control that never enters the dispatch arm.
 set -u
+RELEASE_BUILD=0
+case "${1:-}" in
+  --release-build) RELEASE_BUILD=1; shift;;
+esac
+if [ "$#" -ne 0 ]; then echo "usage: $0 [--release-build]" >&2; exit 2; fi
 PORT=${XDS_PORT:-7200}
 CPUS=${XDS_CPUS:-48-59}
 BIN=${XDS_BIN:-./build/tomokv}
@@ -28,10 +34,12 @@ SMALL=${XDS_SMALL:-4}
 BIG=${XDS_BIG:-128}
 # The guarded quantity is the DISPATCH EXCESS: cross_ns - same_ns, the median over rounds of what
 # the cross-shard command costs OVER the identical same-shard command measured back to back on the
-# same server. Subtracting the control inside each round removes everything the two share (parse,
-# ROB, task handoff, reply, client) and cancels drift instead of amplifying it.
+# same server. Subtracting the control inside each round removes common costs (parse, ROB, task
+# handoff, reply, client); it cannot cancel arbitrary scheduling drift between measured intervals.
 # Observed at the shipped settings (7 rounds x 400k ops per arm, best of 2 pairs), 3 runs each:
 #   BEFORE the fix 1.285 / 1.381 / 1.402      AFTER 1.093 / 1.063 / 1.113
+# Those historical best-of-two results do not license choosing the best pair in a gate. Both
+# pairs are retained below; an unstable instrument produces no timing verdict, with all data kept.
 # The raw cross-only ratio is printed too but is NOT the assertion: it does not clear box noise.
 LIMIT=${XDS_LIMIT:-1.20}
 HERE=$(cd "$(dirname "$0")" && pwd)
@@ -70,62 +78,93 @@ run_arm() {   # $1 = total thread count
   place=$(python3 -c "print(','.join(['ifid@$IO_A','ifid@$IO_B','ex@$EX_A','ex@$EX_B']+['ex@$FILL']*$nfill))")
   stop_arm
   ( taskset -c "$CPUS" "$BIN" --port "$PORT" --bind 127.0.0.1 --enable-debug-command yes \
-      --place "$place" --shards 16 --shard-home "$HOMES" >/tmp/xds-$t.log 2>&1 & )
+      --place "$place" --shards 16 --shard-home "$HOMES" \
+      --key-lb 0 --client-lb 0 --flip-auto 0 >/tmp/xds-$t.log 2>&1 & )
   for _ in $(seq 1 400); do [ -n "$(listener_pid "$PORT")" ] && break; sleep 0.05; done
   if [ -z "$(listener_pid "$PORT")" ]; then echo "XDS: arm $t never listened"; tail -3 /tmp/xds-$t.log; exit 1; fi
   python3 "$HERE/xshard_dispatch_scale.py" 127.0.0.1 "$PORT" "$OPS" 32 "$ROUNDS"
 }
 
-# Two independent small/big PAIRS, and the verdict is the BETTER (lower) of the two ratios. The
-# arms are separate boots, so a pair cannot be measured simultaneously and slow drift across one
-# pair lands entirely in that pair's ratio; taking the minimum of two pairs discards a drifted
-# pair without hiding a real regression (the pre-fix ratio was >= 1.13 in every pair measured).
+# Alternate boot order (small/big, big/small) so a monotone drift does not always tax the big arm.
+# No retry is selected by its result, and no failed mechanism arm is retried.
 PAIRS=""
-for _ in $(seq 1 "${XDS_PAIRS:-2}"); do
-  SMALL_OUT=$(run_arm "$SMALL") || { echo "XDS: small arm failed"; stop_arm; exit 1; }
-  BIG_OUT=$(run_arm "$BIG")     || { echo "XDS: big arm failed"; stop_arm; exit 1; }
+for pair in $(seq 1 "${XDS_PAIRS:-2}"); do
+  if (( pair % 2 )); then
+    SMALL_OUT=$(run_arm "$SMALL") || { echo "XDS: small arm failed"; stop_arm; exit 1; }
+    BIG_OUT=$(run_arm "$BIG")     || { echo "XDS: big arm failed"; stop_arm; exit 1; }
+  else
+    BIG_OUT=$(run_arm "$BIG")     || { echo "XDS: big arm failed"; stop_arm; exit 1; }
+    SMALL_OUT=$(run_arm "$SMALL") || { echo "XDS: small arm failed"; stop_arm; exit 1; }
+  fi
   echo "  small $SMALL_OUT"
   echo "  big   $BIG_OUT"
   PAIRS="$PAIRS|$SMALL_OUT;$BIG_OUT"
 done
 stop_arm
 
-python3 - "$PAIRS" "$LIMIT" "$SMALL" "$BIG" <<'EOF'
+python3 - "$PAIRS" "$LIMIT" "$SMALL" "$BIG" "$RELEASE_BUILD" <<'EOF'
 import sys
+import statistics
 def parse(line):
     return {k: v for k, v in (f.split("=", 1) for f in line.split()[1:])}
 pairs = [tuple(parse(x) for x in p.split(";")) for p in sys.argv[1].split("|") if p]
 limit = float(sys.argv[2])
 want_small, want_big = int(sys.argv[3]), int(sys.argv[4])
-small, big = pairs[0]
 fail = 0
 def check(name, cond, detail=""):
     global fail
     print("  %-52s %s %s" % (name, "ok" if cond else "FAIL", detail))
     if not cond:
         fail += 1
-check("small arm really ran %d threads" % want_small, int(small["threads"]) == want_small,
-      "threads=%s" % small["threads"])
-check("big arm really ran %d threads" % want_big, int(big["threads"]) == want_big,
-      "threads=%s" % big["threads"])
 # The hash seed is drawn from the kernel at every boot, so the shard IDS differ between arms; what
 # the guard needs identical is the FAN-OUT: two owner threads for the cross pair, one for the
 # control. A run that fanned out to one owner would not exercise the dispatch arm under test.
-check("both arms fanned out to two owners",
-      small["cross_owners"] == "2" and big["cross_owners"] == "2",
-      "small=%s big=%s" % (small["cross_owners"], big["cross_owners"]))
-check("both controls stayed on one owner",
-      small["same_owners"] == "1" and big["same_owners"] == "1",
-      "small=%s big=%s" % (small["same_owners"], big["same_owners"]))
+check("measurement has at least two pairs", len(pairs) >= 2)
+for index, (small, big) in enumerate(pairs, 1):
+    check("pair %d small arm really ran %d threads" % (index, want_small),
+          int(small["threads"]) == want_small, "threads=%s" % small["threads"])
+    check("pair %d big arm really ran %d threads" % (index, want_big),
+          int(big["threads"]) == want_big, "threads=%s" % big["threads"])
+    check("pair %d both arms fanned out to two owners" % index,
+          small["cross_owners"] == "2" and big["cross_owners"] == "2")
+    check("pair %d both controls stayed on one owner" % index,
+          small["same_owners"] == "1" and big["same_owners"] == "1")
+# The box's supplied noise contract says >2% on IDENTICAL arms is contention or a defect, not
+# random noise. Check repeatability separately within each thread count, and same-shard controls
+# across counts. Do not qualify the instrument using the dispatch ratio being judged.
+spreads = {}
+for side, name in ((0, "small"), (1, "big")):
+    for metric in ("cross_ns", "same_ns"):
+        values = [float(pair[side][metric]) for pair in pairs]
+        spreads[name + "_" + metric] = max(values) / min(values) if values and min(values) > 0 else float("inf")
+controls = [float(arm["same_ns"]) for pair in pairs for arm in pair]
+spreads["control_across_counts"] = max(controls) / min(controls) if controls and min(controls) > 0 else float("inf")
+stationary = len(pairs) >= 2 and all(spread <= 1.02 for spread in spreads.values())
+reason = ("requires --release-build" if sys.argv[5] != "1" else
+          "instrument not stationary (2% control/repeat bound)" if not stationary else "")
+print("  instrument spreads=%r" % spreads)
 ratios = []
 for s_arm, b_arm in pairs:
     se, be = float(s_arm["excess_ns"]), float(b_arm["excess_ns"])
-    check("cross-shard really costs more than same-shard in both arms", se > 0 and be > 0,
-          "excess %.1f / %.1f ns" % (se, be))
+    detail = "excess %.1f / %.1f ns" % (se, be)
+    if reason:
+        print("  SKIP cross-shard really costs more than same-shard -- %s; %s" % (reason, detail))
+    else:
+        check("cross-shard really costs more than same-shard in both arms", se > 0 and be > 0, detail)
     ratios.append(be / se if se > 0 else float("inf"))
-ratio = min(ratios)
-check("dispatch excess ratio %dt/%dt <= %.2f" % (want_big, want_small, limit), ratio <= limit,
-      "pairs %s  best=%.3f" % (" ".join("%.3f" % r for r in ratios), ratio))
+ratio = statistics.median(ratios) if ratios else float("inf")
+# Restore the nthreads() scan/whole-array zeroing to induce a stable release regression; a good
+# second pair cannot erase it by selection. Misroute/drop a key to fail byte-exact arms on ANY
+# build. Indeterminate timings are explicitly SKIP, never a green claim about scaling.
+label = "dispatch excess ratio %dt/%dt <= %.2f" % (want_big, want_small, limit)
+detail = "pairs %s median=%.3f" % (" ".join("%.3f" % r for r in ratios), ratio)
+if reason:
+    print("  SKIP %s -- %s; %s" % (label, reason, detail))
+else:
+    check(label, all(r <= limit for r in ratios), detail)
+small, big = pairs[0] if pairs else ({}, {})
+if not pairs:
+    sys.exit(1)
 print("  %-52s ok cross %.1f->%.1f (%.3f)  same %.1f->%.1f (%.3f)"
       % ("raw per-op costs (diagnostic, not asserted)",
          float(small["cross_ns"]), float(big["cross_ns"]),

exec
/bin/bash -lc 'cat DESIGN-KNOBS.md && git diff -- tests/atomic_torn.py tests/gate.sh tests/xshard_dispatch_scale.sh' in /home/user/Projects/cx-knobs
 succeeded in 0ms:
# Configuration-surface reduction

Baseline: `78c3e5391`, 2026-09-07. This implements the maintainer's prescribed dispositions.
No server, benchmark, or gate was run by Codex. This is an uncommitted implementation for review
and maintainer-run validation, with no throughput or latency improvement claimed. The maintainer's
first gate result and the atomic-window test repair are recorded below.

## One-to-one knob accounting

Defaults below describe the baseline unless the after column states otherwise. A deleted
name is rejected by both the CLI and the shared conf-file parser, is absent from the Config
fields and CONFIG table, and has no corresponding INFO configuration value. Operational
counters such as atomic-window stalls and kTLS engagement remain observations of the retained
mechanisms. No aliases preserve a deleted public control.

| Before | Before default | After | Reason / retained behavior |
| --- | --- | --- | --- |
| `--read-local-prefetch-capture` | `1` | Deleted; capture selected statically | Preserve the winning immutable-object capture at prefetch. |
| `--read-local-atomic-filter` | `1` | Deleted; precise filter whenever read-local is armed | Preserve the winning per-key safety filter, including fail-closed poison and overflow handling. |
| `--read-local-interleave` | `1` | Deleted; bounded interleave whenever read-local is armed | Preserve bounded local chunks and per-producer owner quanta. |
| `--flip-auto-band` | `-1` | Deleted; controller initialized with automatic band | Preserve the existing learned jitter, resolution, and baseline floors. |
| `--shard-home` | Unset | Deleted; round-robin over resolved executors | Preserve default ownership; remove manual map parsing. Runtime migration remains available. |
| `--l3-domains` | Discovery | Deleted; discovery plus `TOMOKV_L3_DOMAINS` environment override | Keep hardware discovery and an escape hatch for broken L3 sysfs. Override grammar remains comma-separated domains, `-` ranges, `+` range joins; invalid/duplicate/out-of-mask CPUs fail. |
| `--smt-mode` | `0` | Deleted; derive from complete sibling pairs in the allowed topology | Use existing sibling-unit placement and FLIP invariants when pairs are present. |
| `--genthread-schedule` | No independent default; compatibility alias | Deleted | Remove the alias that assigned fused mode and an overlap value. See objection about the original “zero uses” premise. |
| `--atomic-window` | `-1` | Deleted; `min(16 * resolved_shards, 1024)` | Preserve the measured automatic credit bound. No CONFIG override or live window setter remains. Live `atomic` still reconfigures credits safely. |
| `--persist-io` | `uring` | Deleted; derive engine from retained `net-io` | Preserve native uring persistence and the existing syscall dependency under epoll, which owns no uring ring. |
| `--lru-clock-shift` | `8` | Deleted; `kLruClockShift = 8` | Preserve 256-second buckets and the 8192-second wrap. Executor clocks and OBJECT IDLETIME share this constant. |
| `--script-crossshard-max-bytes` | `-1` | Deleted; boot-derived staging budget | Preserve `max(4 MiB, min(boot_maxmemory / shards / 16, 64 MiB))`, or 4 MiB when boot maxmemory is zero. |
| `--script-crossshard-workbench-bytes` | `-1` | Deleted; twice the staging budget | Preserve the original automatic workbench bound, still allocated on demand. |
| `--script-crossshard-conflict-retries` | `-1` | Deleted; fixed `8` | Preserve the original automatic conflict-retry limit for scripts. |
| `--script-crossshard-cut-slots` | `-1` | Deleted; fixed `4` | Preserve the original per-IO script snapshot reservation bound. |
| `--tls-ktls` | `yes` | Deleted; always attempt kTLS when TLS is enabled | Preserve automatic userspace fallback. `tls-port 0` still creates no TLS context or connection state. |
| `--key-lb` | `1` | Collapsed into `--lb` | One switch controls key and client balancing together. |
| `--client-lb` | `1` | Collapsed into `--lb` | Same switch; no independently disabled half remains. |
| `--lb-sample-rate` | `64` | Collapsed; rate derived from measured visits and decision duration | Target a constant number of samples per decision; details below. |
| `--lb-age-sample-rate` | `0` | Collapsed; derived sampling only during a flip maneuver | Keep idle/anchor sampling dark; derive the armed rate from traffic and the controller's decision window. |
| `--lb-tick-ms` | `1000` | Collapsed; internal 1000 ms observation tick | Retain the observation cadence; derive sampling over three sustained ticks. |
| `--lb-imbalance-pct` | `25` | Collapsed; twice measured quiet jitter | Separate key/client learners; excursions cannot enlarge their own admission band. |
| `--lb-move-cap` | `1` | Collapsed; derive from measured transfer duration | Bootstrap one move, then pace against measured per-move drain/install cost. |
| `--lb-cooldown-ms` | `5000` | Collapsed; derive from measured transfer duration | Remove the old all-nonzero machinery gate: zero cooldown can no longer mean zero moves. |
| No `--lb` | — | **`--lb 0\|1`, default `1`** | The sole public key/client balancing switch; `0` allocates no LB sidecar, bucket counters/census arrays, or controller windows. |
| `--thread-mode` | `2s` | Kept, unchanged; `2s\|1s` and `split\|fused` aliases | Both architectures remain. |
| `--ratio` | Unset; even split | Kept, split only | Global IO/EX counts, spread over locality domains; retain conf/CLI precedence with `place`. |
| `--place` | Derived | Kept, derived by default | Explicit `role@cpu` selection remains; fused mode uses labels only as CPU selectors. |
| `--shards` | Constant `16` | Kept; default `-1` resolves to `min(8 * executor_count, 256)` | Eight migration units per initial executor; preserves 16 shards at the gate's 6:2 split. Explicit `1..256` remains authoritative. |
| `--read-local` | `0` | Kept, `0\|1` | Effective only in fused plain scheduling; inactive modes retain ordinary owner dispatch. |
| `--atomic` | `0` | Kept, `0\|1`, live | Preserve enable/disable and group-scoped epoch-MVCC behavior. |
| `--flip-auto` | `0` | Kept, `0\|1`, split only | Preserve explicit controller enablement and dark fingerprint/stamp writers when disabled. |
| `--ex-sched` | `0` | **`--x-ex-sched 0\|1`**, default `0` | Study namespace only; omitted from public help, tomokv.conf, and user documentation. |
| `--overlap` | `0` | **`--x-overlap 0\|1\|2`**, default `0` | Study namespace only; same schedules and mode/engine validation. |
| `--zc-min` | `16384` | Held unchanged | Pending maintainer benchmarks. |
| `--net-io` | `uring` | Held unchanged, `uring\|epoll` | Preserve both network engines and their existing persistence dependency. |
| `--hash` | `mix64` | Held unchanged, `mix64\|siphash` | Pending maintainer benchmarks. |

The additional `--thread-pipeline` alias of overlap is removed with the old public spelling;
it cannot bypass the study namespace. Study CONFIG names are `x-ex-sched` and `x-overlap`;
INFO uses `x_overlap`, with the obsolete `overlap`/`thread_pipeline` aliases removed.

Other TomoKV controls not assigned a change by the design stay unchanged:
`--flip-work-window 100`, `--script-instruction-limit 100000`, `--no-pin` / conf `pin yes`,
and persistence input `--load`. Redis-compatible parser arms, names, defaults, and runtime
semantics are untouched. Documentation-only edits clarify the automatically selected engine.

## Derivation and lazy work

`LbAutotune` lives beside the existing weighted placement policy in
[`src/core/weighted_lb.h`](src/core/weighted_lb.h). Only `lb=1` allocates it. Its observation
tick is 1000 ms and a sustained decision covers three ticks, preserving existing hysteresis.
The sampling target is 4096 observations per decision. For measured visits `V` over elapsed
time `E`, the one-in-N rate is `max(1, ceil(V * decision_ms / E / 4096))`, capped at uint32.
Low traffic samples every visit. Before the first completed window, sampling bootstraps at 1.

Each successful key sample contributes its **latched rate** to the physical shard counter.
The fold therefore consumes estimated visits directly instead of multiplying historical counts
by a newer rate. Each executor refreshes its private rate on the existing census beat. Counters
and their census state remain on the physical shard; immutable bucket IDs retain controller
history across migration. No ownership or retirement handoff was moved out of its critical section.

Age sampling uses the same duration/traffic rule over three flip-controller ticks. It is armed
at maneuver start, updated on completed traffic windows, and set to zero at anchor/disable.
Its traffic estimate is completed commands; fanout can generate multiple stamped tasks per
command. It adds no eager computation while sampling is off. This controller remains independent
of the continuous key/client LB enable switch.

Key and client imbalance bands learn adjacent-window jitter before admission, using the
existing 0.25 EWMA weighting. After learning, only changes inside twice the learned jitter
update the estimate. The fire band is twice jitter; the existing 80% Schmitt release and
three-tick sustain remain. A zero measured band is a legitimate observation, never an off switch;
a zero-imbalance window clears the streak even when the learned release band is also zero.

Transfer timing brackets the existing drain-through-commit transaction using its already
published deadline. Completed duration divided by moved items gives mean cost `C` in ns.
The shard move cap is `min(shards, max(1, floor(tick_ns / C / 3)))`; before a measurement it
is one. Cooldown is `max(1, ceil(3 * C / tick_ns)) * tick_ms`, with one tick before a cost
sample. Client transfers remain one candidate per existing connection-drain transaction.
Only completed transfers contribute cost; refused or abandoned plans do not. Timing and policy
updates are cold, and the existing quiesced ownership transfer/rebinding functions are unchanged.

The read-local selectors are replaced by their winning branches inside the existing armed
paths. Immutable capture, filter publication, retirement, and QSBR retain their existing
arming checks. Scoped hash callbacks are still invoked only after read-local admission, and
scatter hash enumeration remains behind the local LB-enabled check. TLS setup still begins
only after the listener's `tls-port` check. No removed flag is replaced by an eager argument
whose callee merely returns early.

## Boot geometry, INFO, and layout

`Server::prepare_boot` resolves topology, sibling units, placement, and the shard default
before AOF/snapshot recovery reads a shard count. `Server::init` consumes that same prepared
placement, with no second discovery. Fused executor count is the number of selected fused
threads; split executor count is the resolved EX role count. A later FLIP changes roles without
changing the boot-latched shard count.

`INFO server` adds `shards` from the real shard inventory and `atomic` from the live enable
bit. The existing `read_local` already reports the effective lane state and remains intact.
These fields are read-only, computed on INFO, with no operation-path writes. The thread counts
already reported by each mode remain. No new CONFIG shard entry is added, so CONFIG REWRITE
does not turn an automatic default into an explicit fixed count. The old separate key/client
enable observations become `tomokv_lb_enabled`; movement counters remain available. The
deleted SMT flag's INFO value is removed; the derived FLIP unit size remains observable.

| Layout lock | Before | After |
| --- | ---: | ---: |
| Config | 624 | **528** |
| Op | 336 | 336 |
| Client | 1984 | 1984 |
| ThreadCtx | 1408 | 1408 |
| Shard | 1440 | 1440 |
| FlatStore | 944 | 944 |
| Rob<64> | 192 | 192 |
| AtomicEntry | 144 | 144 |

The Config assertion explicitly becomes **528**, with the same accounting comment beside it:
104 declared field bytes removed, 4 bytes added for `lb`, and 4 bytes of additional alignment
padding: `624 - 104 + 4 + 4 = 528`. No surviving Config fields were reordered. This changes
their offsets and Server's embedded Config footprint, intentionally and visibly. Other locks
remain unchanged. The former FlatStore filter byte and the two ReadLocalExImpl selector bytes
are reserved padding to preserve the measured reader/owner separation and demotion-state offsets.

## Atomic-window gate repair: verdict (a), test witness

The reported failure is **(a)**: the test never separated release of its artificial hold from
its resume assertion. It is not evidence of a newly broken derived window. This verdict was
stated before editing the repair; no server code is changed by this follow-up.

The evidence, at the gate's actual geometry:

- `tests/gate.sh:29` selects cores `0-7`; line 31 resolves `6:2`; line 370 supplies `--shards 16`
  and line 390 supplies that ratio. Release and ASAN use this boot helper at lines 433 and 1249.
  The bound is **256**, not a bound inferred from a default boot on all machine cores.
- Baseline `78c3e5391:src/core/server.h:196` resolves AUTO with
  `min(16 * cfg.shards, 1024)`. Current `src/core/server.h:250` retains exactly that formula.
  Admission borrowing/stalling (`:2497`), retirement and idle lease return (`:2541`), and
  backpressure release (`src/core/io_loop.h:6698`, also `:5684`/`:6381`) are unchanged by the
  configuration reduction. The deleted setter was only a wrapper around credit reconfiguration.
- In the **reported failing version** of `tests/atomicwindow.py`, lines 13–15 submit
  `2 * ceil(256 / 64) + 2 = 10` connections, each with 64 groups: **640** total. Line 55 arms
  `ATOMIC-COMMIT-DELAY 100000`; lines 59–67 wait for every reply and raise “did not resume” at
  30 seconds; line 70 clears the hook only in the ensuing `finally`. No released completion
  interval was tested, and a timeout bypassed the nominal three-attempt discovery loop.
- `src/core/server.h:2615` applies the delay to **each commit batch**, and `:3036` spins for the
  full captured duration. Batching cannot justify the old deadline: default slow-log threshold
  is 10,000 us (`src/core/config.h:356`); escalation (`src/core/ex_loop.h:2658`) selects per-op
  timing, whose `:2620` flushes after every operation. At 100 ms per individual commit, 640 groups
  can consume 64 executor-seconds, or 32 seconds even if shared evenly by two executors. That is
  an allowed schedule exceeding 30 seconds, **not a measurement of the failing run's rate**.
- The saved release and ASAN logs both have the timeout at line 21, successful subsequent
  `CONFIG SET atomic` traffic at line 22, and `inflight=0 pending=0` after churn at line 23
  (`/tmp/gate-atomic-torn.txt`, `/tmp/gate-atomic-torn-asan.txt`). Both following RYOW logs report
  `limit=256 peak=24 stalls=0 credits=256`. This supports the release/accounting trace above;
  those logs contain no peak, stall count, or reply count for the failed global burst, so they
  cannot establish whether that particular attempt filled the window or its per-attempt rate.

The repair in `tests/atomicwindow.py` pre-encodes each burst and starts senders at one barrier.
It uses a five-second polling budget to find a held witness, clears the DEBUG delay in `finally`,
and only after the clear replies OK starts the unchanged 30-second resume deadline. At least two
groups must still be live when the window is witnessed, and some replies must remain outstanding
when release is requested. The global arm additionally requires an increase in window stalls;
a witness seen only during drain cannot rescue a miss. All replies must be OK. Cleanup wakes
blocked readers before closing buffered files, joins helpers with a shared bound, and requires
zero live groups/debt and the full credit pool before another attempt.

Only a clean, joined, fully reclaimed **witness miss** can re-arm, on new writer/control
connections and newly resolved disjoint cross-owner keys. Four misses still fail. Reply errors,
excess live groups, debt, missing credits, or failure to resume **after release** fail immediately;
none can be retried into a pass. This matches the policy in `tests/multirace.py:464` and the OFF
RENAME control at `tests/atomic_torn.py:744`, without importing either control's measured hit rate.
Each attempt prints its outcome, held/total stalls, peak, carried groups, exact reply counts and
elapsed arm/resume times with measured replies/second. The result also prints observed attempts
over attempted bursts. **The new runtime arming rate remains unmeasured:** the maintainer must
collect these lines separately on release and ASAN at 16 shards, 6:2, cores 0–7. Four is an explicit
discovery budget, not an asserted residual failure probability; no percentage is guessed here.

**Row-collapse decision: reverted.** `tests/atomic_torn.py:874` separately reports “derived atomic
window stalls and resumes”; `:882` reports “atomic reconfiguration preserves derived bound and
reclaims leases”, using a second fresh burst. Resizing coverage is obsolete with the removed
grammar, but live reconfiguration is not: `CONFIG SET atomic 1` unconditionally reaches
`set_atomic_enabled` (`src/cmd/t_server.cc:1440`), which rebuilds the credit generation even when
already ON (`src/core/server.h:2462`, `:3343`). Keeping ON preserves the same bound for all traffic.
The second row requires surviving **pre-CONFIG** admissions: it subtracts every possible newer
admission, sampled after the post-CONFIG live count, and demands a positive remainder. This cannot
pass solely on fresh post-CONFIG groups. Prepared groups abandoned for task-queue backpressure
(`src/core/io_loop.h:5077`) also count as admissions; their inclusion makes this witness stricter,
so total admissions are deliberately not equated to total replies. Both rows retain exact credit
reclamation checks; the second specifically exercises old-generation lease return (`server.h:3324`).

Repair validation was server-free: Python AST parsing, `git diff --check`, and 12 in-memory
transport fault-injection cases covering release-before-completion, the one-connection arm,
live reconfiguration, fresh retry, four-miss failure, and immediate failure on no resume, bad
reply, excess live groups, and lost credits. A control containing only newer post-CONFIG groups
also exhausted discovery rather than passing. Stalls first seen during drain cannot rescue an
unarmed attempt, and extra admissions from abandoned preparations need not equal reply counts.
These are harness checks, not measurements of
TomoKV. No build, server, benchmark, or gate was run for the repair. For maintainer-only broken
server controls: disabling admission-backpressure release must fail the resume deadline; dropping
old-generation credit returns must fail pool reclamation; suppressing the stall witness must
exhaust discovery. None of those server mutations was applied or run here.

## Gate accounting and review checks

**Required expected counts: quick 325, full 342** (343 with the optional NIC row).
`EXPECT_QUICK=327` and `EXPECT_FULL=344` at lines 141–142 of `tests/gate.sh` are deliberately
unchanged for the maintainer to edit. Counted by emitting line, not battery name or file location:

| Row change | Baseline emitting line | Position relative to quick exit | Quick delta | Full delta |
| --- | --- | --- | ---: | ---: |
| Retire `xscript off control`; keep limit/window with automatic bounds | 658, inside the former three-arm loop | Before old exit 1260; surviving two-arm row now emits at 649, before exit 1245 | -1 | -1 |
| Retire manual-map dispatch scaling guard and its shell wrapper | 686 | Before old exit 1260; retirement note now at 673, before exit 1245 | -1 | -1 |
| Replace persistence CONFIG surface check with removed-knob/actual-geometry checks | 981 | Four iterations before the exit; replacement emits at 966 | 0 | 0 |
| Restore separate atomic liveness and reconfiguration checks inside `atomic_torn.py` | Internal base lines 894 and 957 | Outer release row still emits at 435, before quick exit 1245; ASAN row still emits at 1255, after it | 0 | 0 |
| All other battery rows | Existing lines, retaining multiplicity | Same side of the quick exit | 0 | 0 |

Restoring the internal check adds one separately reported assertion per battery invocation, not
another `gate.sh` ledger entry. Expected counts therefore **remain quick 325 / full 342**, or 343
with the optional NIC row; the known PROGRAM-STATE mismatch is unchanged. No outer battery row
was added. New helper files are consumed by existing rows. Parser coverage
rejects every removed spelling even with its formerly valid default, checks the study grammar,
and checks sample-budget invariance, jitter excursion rejection, and transfer-cost pacing.
Compile-time assertions cover the shard-default scaling and cap. `tests/knobs.py` checks the
CONFIG removals and compares INFO geometry with DEBUG's actual thread/shard inventory on the
four existing persistence/atomic boots. Normal persistence coverage now boots `net-io epoll`;
uring coverage still boots `net-io uring`.

Atomic admission tests use fresh disjoint cross-owner keys and held groups at the production
credit limit. The global arm must observe stalls and multiple live groups, maintain the bound,
and reclaim all credits. The one-connection arm must observe simultaneous groups on that one
connection. Each has four bounded fresh attempts and fails if its witness never appears, with
the DEBUG hold cleared before the resume deadline. The independent reconfiguration row uses
live `atomic` with old-generation groups still present; resizing itself is retired with the knob.
Live ON/OFF toggle coverage also remains. The separate rate comparison uses an unheld burst.

The script staging control exceeds the default 4 MiB budget and still requires refusal before
RUN with untouched values. The cut-slot arm uses more than four connections per serving thread,
retains its serial negative control, and re-arms fresh connections up to three times before
failing. LRU rows age all old cohorts across one real 256-second bucket before probing ordinary
reads, NO-TOUCH, and eviction survival; no clock override remains. The four rows and their
positive lane-hit witnesses remain. Their individual clock wait is bounded at 260 seconds.

The TLS fallback battery uses the retained cipher grammar (TLS 1.2 CBC; TLS 1.3 AES-256 outside
the current custom AES-128 RX installer) and retains its live fallback/zero-copy-suppression
assertions. Default kTLS engagement retains its separate positive witness. Idle LB signal tests
now require zero age samples; the maneuver test must observe positive age sampling. The stable
flip hold uses the reported derived band and fails after bounded unsuccessful re-arms.

Initial reduction validation: serial C++20 syntax checks with warnings enabled for the parser,
main, fused loop, scatter, server/INFO, LB reporting, flip controller, TLS, persistence, snapshot,
and OBJECT implementations; Python AST parsing; shell syntax checks; diff/removed-reference
and layout-assertion inspection. No executable was linked or run by Codex. The maintainer then
reported the failure documented above. The repaired runtime witnesses and their broken-server
controls, and performance, remain unverified; the repair's server-free checks are listed above.

| Matched geometry/load comparison | PRE `78c3e5391` | POST working diff |
| --- | --- | --- |
| GET/SET rates, cycles/op, instructions/op, IPC in both modes | Not measured in this worktree | Not measured |
| Enabled LB under stationary/skewed/changing load | Existing fixed controller | New derivations; benchmark pending |
| `lb=0` / flip sampling dark | Source baseline | Arming/allocation paths inspected; benchmark pending |
| Full gate | 344/344 supplied by maintainer | Maintainer's pre-repair run: 340 ok, 3 FAIL (including the expected ledger mismatch); repair not run, expected count still 342 |

## OBJECTIONS

1. **`--read-local-prefetch-capture` / `--read-local-atomic-filter`: inherited reader retry.**
   The winning captured MGET path already violates the supplied “no reader retries” law:
   `prepare_captured_local_mget` in `src/core/ex_loop.h` sets `kAttempts=2` at line 1295,
   loops at 1317, and increments `mget_generation_retries` before repeating at 1431–1432.
   The same loop exists in the baseline, and this diff still selects that winning path.
   It was not redesigned or silently removed. The default GET capture arm breaks on table
   churn; the confirmed normal-path objection is MGET. Resolving the inherited law conflict
   requires a separate correctness decision, not a covert change in this surface reduction.

2. **`--genthread-schedule`: “zero uses” was not literal.** Baseline
   `src/core/config.h:702–716` actively lowered `coarse/iofused/streams` into fused mode and
   overlap 0/1/2, with parser tests for the aliases. There was no independent Config field.
   The alias is deleted as prescribed; former callers now fail explicitly.

3. **`--lb` and derived `--shards`: measurement still required.** The specified derivation
   scheme is implemented, but the 4096-sample target, eight shards per executor, initial
   all-visit sampling, and transfer pacing have no PRE/POST rate evidence in this task.
   Age sampling estimates command volume, so fanout affects actual stamped-task counts.
   A pointer transfer's timed drain/install duration also does not measure subsequent cache
   warmup or commands lost. These limitations matter to a paper and to the instruction/IPC
   verdict; the formulae are implementation choices within the specified derivations, not
   claims of measured optima. No runtime tuning knobs were retained to defer this decision.

4. **`--smt-mode` / `--l3-domains`: stricter boot geometry.** Automatic pair mode invokes
   the existing complete-pair and same-role validation. Odd logical ratios, incomplete explicit
   pairs, or a split restricted to one available pair can now be rejected. L3 override repairs
   L3 discovery; sibling sysfs still must be readable to derive and validate pair mode.
   These consequences are recorded rather than adding an unrequested SMT override.

5. **`--shard-home`: one regression instrument is lost.** The retired scaling wrapper kept
   all 16 shards on two real executors and added empty filler executors using the manual map.
   Default round-robin changes that ownership with thread count. Keeping its former measured
   ratio threshold on a different geometry would assert an unvalidated claim, so its one row
   is explicitly retired rather than reporting a misleading pass.

6. **`--lru-clock-shift`: gate duration increases.** Keeping the winning 256-second clock
   and preserving a real positive age witness costs up to 260 seconds on each of four boots.
   No debug clock selector or artificial tolerance was introduced. The maintainer should budget
   for those waits when running either tier.

All prescribed knob dispositions are implemented. No item was left undone for a newly
introduced compile failure or correctness-law violation; the inherited MGET conflict above
is explicitly retained for maintainer review.
diff --git a/tests/atomic_torn.py b/tests/atomic_torn.py
index 448e00be6..de3cbc575 100755
--- a/tests/atomic_torn.py
+++ b/tests/atomic_torn.py
@@ -865,104 +865,23 @@ else:
          copy_on[0] == 0 and not copy_on[1] and not copy_on[2],
          "anomalies=%d errors=%r threads_still_alive=%r" % copy_on)
 
-# Admission liveness: with a one-group window, the second frame in one received pipeline reaches
-# admission before owner notifications for the first are flushed. This makes the fired assertion
-# deterministic instead of depending on Python threads winning a scheduling race.
+# Separate admission liveness from live credit reconfiguration/accounting. Both use the
+# production bound and must actually enter their window; a clean miss is re-armed, never skipped.
+from atomicwindow import held_burst
 config("atomic", 1)
-config("atomic-window", 1)
-window_before = stats().get("atomic_window_stalls", 0)
-window_run = format(time.time_ns() & 0xfffffff, "x")
-window_errors = []
-window_client = Resp()
-window_frames = []
-for sequence in range(64):
-    args = ["MSET"]
-    for key_index in range(8):
-        args.extend(("aw%s:%x:%x" % (window_run, sequence, key_index),
-                     "window:%x" % sequence))
-    window_frames.append(frame(*args))
 try:
-    window_client.sock.sendall(b"".join(window_frames))
-    for _ in window_frames:
-        if window_client.read() != b"OK":
-            raise AssertionError("bad window reply")
+    witness = held_burst(HOST, PORT, whole_window=True)
+    note("derived atomic window stalls and resumes", True, witness)
 except Exception as exc:
-    window_errors.append(str(exc))
-finally:
-    window_client.close()
-window_after = stats().get("atomic_window_stalls", 0)
-note("atomic-window stalls and resumes",
-     not window_errors and window_after > window_before,
-     "stalls=%d errors=%r" % (window_after - window_before, window_errors))
-config("atomic-window", 256)
-
-# Credit leases must preserve the exact configured bound while CONFIG changes it under load. A
-# shrink may inherit more already-admitted groups than the new limit; wait for that unavoidable
-# debt to retire, then prove no subsequent sample exceeds the bound. Finally, an idle system must
-# have returned every leased credit to the pool so a skewed next IO can consume the whole window.
-lease_stop = threading.Event()
-lease_errors = []
-
-
-def lease_writer(wid):
-    client = None
-    keys = ["at:lease:%d:k%d" % (wid, key) for key in range(8)]
-    seq = 0
-    try:
-        client = Resp()
-        while not lease_stop.is_set():
-            if mset(client, keys, "lease:%d:%d" % (wid, seq)) != b"OK":
-                raise AssertionError("bad lease reply")
-            seq += 1
-    except Exception as exc:
-        lease_errors.append("writer%d:%s" % (wid, exc))
-    finally:
-        if client is not None:
-            client.close()
-
+    note("derived atomic window stalls and resumes", False, str(exc))
 
-lease_threads = [threading.Thread(target=lease_writer, args=(i,), daemon=True) for i in range(8)]
-for thread in lease_threads:
-    thread.start()
-for value in (31, 7, 19, 3):
-    if not config("atomic-window", value):
-        lease_errors.append("CONFIG atomic-window %d failed" % value)
-deadline = time.time() + 3
-bounded = False
-max_inflight = 0
-while time.time() < deadline:
-    sample = stats()
-    live = sample.get("atomic_inflight", 1000000)
-    debt = sample.get("atomic_credit_debt", 1000000)
-    if live <= 3 and debt == 0:
-        bounded = True
-        break
-    time.sleep(0.01)
-if bounded:
-    deadline = time.time() + 0.4
-    while time.time() < deadline:
-        live = stats().get("atomic_inflight", 1000000)
-        max_inflight = max(max_inflight, live)
-        if live > 3:
-            bounded = False
-            break
-lease_stop.set()
-for thread in lease_threads:
-    thread.join(timeout=10)
-deadline = time.time() + 3
-lease_after = stats()
-while lease_after.get("atomic_inflight", -1) != 0 and time.time() < deadline:
-    time.sleep(0.01)
-    lease_after = stats()
-note("atomic-window reconfiguration preserves bound and reclaims leases",
-     bounded and not any(thread.is_alive() for thread in lease_threads) and not lease_errors and
-     lease_after.get("atomic_inflight", -1) == 0 and
-     lease_after.get("atomic_credit_debt", -1) == 0 and
-     lease_after.get("atomic_credit_pool", -1) == 3,
-     "max=%d pool=%d debt=%d errors=%r" %
-     (max_inflight, lease_after.get("atomic_credit_pool", -1),
-      lease_after.get("atomic_credit_debt", -1), lease_errors))
-config("atomic-window", 256)
+# Resizing was removed with atomic-window. CONFIG SET atomic 1 still rebuilds credit generations;
+# require surviving old-generation groups before asserting the fixed bound and reclaimed leases.
+try:
+    witness = held_burst(HOST, PORT, whole_window=True, reconfigure=True)
+    note("atomic reconfiguration preserves derived bound and reclaims leases", True, witness)
+except Exception as exc:
+    note("atomic reconfiguration preserves derived bound and reclaims leases", False, str(exc))
 
 # Live CONFIG flips under active traffic are a liveness/safety arm. OFF intervals deliberately do
 # not promise atomicity; after ending ON, one final group must be read intact.
diff --git a/tests/gate.sh b/tests/gate.sh
index c4d8fef19..295db179f 100755
--- a/tests/gate.sh
+++ b/tests/gate.sh
@@ -1,7 +1,7 @@
 #!/bin/bash
 # PRE-PUSH GATE for tomokv-cpp (pure 2s baseline).
 #
-#   tests/gate.sh quick   loopback only, ~3 min: build (release+ASAN), footprint locks, boot
+#   tests/gate.sh quick   loopback only: build (release+ASAN), footprint locks, boot
 #                         matrix, smoke, torture, RYOW, atomic torn/mixed-write/window gates,
 #                         shutdown invariants, counter-fired assertions, idle-loop ceiling. Runs on
 #                         any machine.
@@ -465,7 +465,7 @@ for AT in 0 1; do
   if boot_fused ./build/tomokv --atomic "$AT" --enable-debug-command yes; then
     FUSED_INFO=$(redis-cli -h 127.0.0.1 -p "$PORT" INFO server 2>/dev/null | tr -d '\r')
     FUSED_MODE=$(printf '%s\n' "$FUSED_INFO" | sed -n 's/^thread_mode://p')
-    FUSED_OVERLAP=$(printf '%s\n' "$FUSED_INFO" | sed -n 's/^overlap://p')
+    FUSED_OVERLAP=$(printf '%s\n' "$FUSED_INFO" | sed -n 's/^x_overlap://p')
     [ "$FUSED_MODE" = 1s ] && [ "$FUSED_OVERLAP" = 0 ] \
         && ok "fused boot line (atomic $AT)" \
         || bad "fused boot line (atomic $AT)" "wire mode=$FUSED_MODE overlap=$FUSED_OVERLAP"
@@ -528,20 +528,20 @@ done
 # ---- eviction accounting on both read paths: owner-served (split) and lane-served (fused+armed) --
 # tests/evict_battery.py needs one FRESH boot per section (it sets maxmemory itself and has no
 # FLUSHALL). lfu proves OBJECT FREQ rises for ordinary reads and stays put for CLIENT NO-TOUCH
-# reads, plus hot-set survival under pressure; lruclock is the LRU twin on a 1 s bucket
-# (--lru-clock-shift 0: the default 256 s bucket cannot tell two keys apart inside one gate row).
+# reads, plus hot-set survival under pressure; lruclock ages all cohorts across one fixed 256 s
+# production bucket (a bounded wait of up to 260 s per boot) before testing the read paths.
 # On the armed boot both sections also assert the reads they measure were LANE-served -- a key
 # kept hot only by lane reads was never touched before ExLoopT::note_local_read_access and so was
 # evicted FIRST, which is the inverse of the policy the boot asked for.
 for AT in 0 1; do
   for EVSEC in lfu lruclock; do
-    boot ./build/tomokv --atomic $AT --lru-clock-shift 0 \
+    boot ./build/tomokv --atomic $AT \
         || bad "eviction $EVSEC boot (split, atomic $AT)"
     py tests/evict_battery.py $PORT $EVSEC >/tmp/gate-evict-$EVSEC-split-$AT.txt 2>&1 \
         && ok "eviction $EVSEC battery (split, atomic $AT)" \
         || bad "eviction $EVSEC battery (split, atomic $AT)" "see /tmp/gate-evict-$EVSEC-split-$AT.txt"
     stop
-    boot_fused ./build/tomokv --atomic $AT --read-local 1 --lru-clock-shift 0 \
+    boot_fused ./build/tomokv --atomic $AT --read-local 1 \
         || bad "eviction $EVSEC boot (fused+armed, atomic $AT)"
     py tests/evict_battery.py $PORT $EVSEC >/tmp/gate-evict-$EVSEC-armed-$AT.txt 2>&1 \
         && ok "eviction $EVSEC battery (fused+armed, atomic $AT)" \
@@ -554,7 +554,7 @@ done
 # DEBUG geometry makes false positives impossible in these arms: A/B share one physical shard but
 # use distinct filter cells, as do P/C on the other participating owner. ATOMIC-COMMIT-DELAY holds
 # A/P after raw install and atomic_commit_holds proves the reserved/unpublished window was sampled.
-boot_fused ./build/tomokv --atomic 1 --read-local 1 --read-local-atomic-filter 1 \
+boot_fused ./build/tomokv --atomic 1 --read-local 1 \
     --enable-debug-command yes \
     || bad "B+ read-local purpose boot"
 py tests/bplus.py 127.0.0.1 "$PORT" >/tmp/gate-bplus.txt 2>&1 \
@@ -592,7 +592,6 @@ stop
 # opposite the source. No same-owner or one-executor run can satisfy this row.
 for AT in 0 1; do
   boot ./build/tomokv --ratio 6:2 --atomic "$AT" --enable-debug-command yes \
-       --lb-age-sample-rate 1024 \
       || bad "cross-owner SORT boot (atomic $AT)"
   py tests/sort.py 127.0.0.1 "$PORT" >/tmp/gate-sort-$AT.txt 2>&1 \
       && ok "cross-owner SORT battery (atomic $AT)" \
@@ -602,7 +601,7 @@ done
 
 # ---- debug-surface batteries: these drive DEBUG subcommands, hence their own armed boot -------
 for AT in 0 1; do
-  boot ./build/tomokv --atomic $AT --enable-debug-command yes --lb-age-sample-rate 1024 \
+  boot ./build/tomokv --atomic $AT --enable-debug-command yes \
       || bad "debug-surface boot (atomic $AT)"
   # scriptatomic needs the armed boot for its cross-shard section (DEBUG SHARD proves the group
   # really spans owners; ATOMIC-COMMIT-DELAY / ATOMIC-READ-DELAY widen the window). It flips
@@ -642,17 +641,9 @@ for AT in 0 1; do
   stop
 done
 
-# ---- cross-owner script control boots: each needs its own knob value, hence its own server ----
-# off:    script-crossshard-max-bytes 0 must reproduce today's CROSSSLOT byte-for-byte and leave
-#         every cross counter at zero (the feature is off ⇒ nothing is allocated).
-# limit:  a tiny staging budget must refuse before RUN and leave the declared values untouched.
-# window: one cut slot per IO thread must refuse the overflow with -BUSY, counted exactly, with
-#         the same activations issued one at a time as the zero control.
-for XS in "off:--script-crossshard-max-bytes 0" \
-          "limit:--script-crossshard-max-bytes 300" \
-          "window:--script-crossshard-cut-slots 1"; do
-  XS_MODE=${XS%%:*}; XS_ARGS=${XS#*:}
-  boot ./build/tomokv --atomic 1 --enable-debug-command yes $XS_ARGS \
+# ---- cross-owner script bounds: production auto budgets and four cut slots per IO ----------
+for XS_MODE in limit window; do
+  boot ./build/tomokv --atomic 1 --enable-debug-command yes \
       || bad "xscript $XS_MODE control boot"
   py tests/xscript.py 127.0.0.1 $PORT "$XS_MODE" >/tmp/gate-xscript-$XS_MODE.txt 2>&1 \
       && ok "xscript $XS_MODE control" || bad "xscript $XS_MODE control" \
@@ -679,11 +670,7 @@ py tests/borrow_registry.py 127.0.0.1 $PORT >/tmp/gate-borrow.txt 2>&1 \
     && ok "borrow-registry growth bound" || bad "borrow-registry growth bound" "see /tmp/gate-borrow.txt"
 stop
 
-# this one boots its own arms at two thread counts, so it takes the port/cores rather than a server
-quiet_wait
-XDS_PORT=$PORT XDS_CPUS=$CORES XDS_BIN=./build/tomokv bash tests/xshard_dispatch_scale.sh \
-    >/tmp/gate-xds.txt 2>&1 \
-    && ok "cross-shard dispatch scaling" || bad "cross-shard dispatch scaling" "see /tmp/gate-xds.txt"
+# Dispatch scaling's manual ownership geometry was retired with shard-home; DESIGN-KNOBS.md.
 
 # ---- Redis-wire DUMP/RESTORE survives the native snapshot/restart boundary -------------------
 DUMPRESTORE_DIR=$(mktemp -d /tmp/gate-dumprestore.XXXXXX)
@@ -743,25 +730,25 @@ py tests/debug.py 127.0.0.1 $PORT >/tmp/gate-debug.txt 2>&1 \
 stop
 
 # ---- snapshot data/sync engines: cut, typed round-trip, and typed preimage race ----------------
-for PERSIST_IO in normal uring; do
-  SNAP_DIR=$(mktemp -d "/tmp/gate-snapshot-${PERSIST_IO}.XXXXXX")
-  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
+for NET_IO in epoll uring; do
+  SNAP_DIR=$(mktemp -d "/tmp/gate-snapshot-${NET_IO}.XXXXXX")
+  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
       --dir "$SNAP_DIR" --dbfilename cut.tomo \
-      || bad "snapshot cut boot ($PERSIST_IO)"
+      || bad "snapshot cut boot ($NET_IO)"
   py tests/snap_cut_battery.py "$PORT" save \
-      >"/tmp/gate-snapshot-cut-${PERSIST_IO}.txt" 2>&1 \
-      && ok "snapshot concurrent cut ($PERSIST_IO)" \
-      || bad "snapshot concurrent cut ($PERSIST_IO)" \
-             "see /tmp/gate-snapshot-cut-${PERSIST_IO}.txt"
+      >"/tmp/gate-snapshot-cut-${NET_IO}.txt" 2>&1 \
+      && ok "snapshot concurrent cut ($NET_IO)" \
+      || bad "snapshot concurrent cut ($NET_IO)" \
+             "see /tmp/gate-snapshot-cut-${NET_IO}.txt"
   stop
-  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
+  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
       --dir "$SNAP_DIR" --load "$SNAP_DIR/cut.tomo" \
-      || bad "snapshot cut reload boot ($PERSIST_IO)"
+      || bad "snapshot cut reload boot ($NET_IO)"
   py tests/snap_cut_battery.py "$PORT" verify_cut \
-      >>"/tmp/gate-snapshot-cut-${PERSIST_IO}.txt" 2>&1 \
-      && ok "snapshot cut reload ($PERSIST_IO)" \
-      || bad "snapshot cut reload ($PERSIST_IO)" \
-             "see /tmp/gate-snapshot-cut-${PERSIST_IO}.txt"
+      >>"/tmp/gate-snapshot-cut-${NET_IO}.txt" 2>&1 \
+      && ok "snapshot cut reload ($NET_IO)" \
+      || bad "snapshot cut reload ($NET_IO)" \
+             "see /tmp/gate-snapshot-cut-${NET_IO}.txt"
   stop
 
   # The cut battery above writes only single keys, so it passes on a tree whose snapshot tears
@@ -769,70 +756,70 @@ for PERSIST_IO in normal uring; do
   # generation-tagged groups, a live MGET reader that must stay clean throughout, and the
   # cuts_waited counter that separates a real group drain from a vacuous one. Both arms were
   # confirmed to FAIL on the pre-fix tree (44-53 and 10-13 torn groups per cut respectively).
-  GROUP_DIR=$(mktemp -d "/tmp/gate-snapshot-groups-${PERSIST_IO}.XXXXXX")
-  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" --atomic 1 \
+  GROUP_DIR=$(mktemp -d "/tmp/gate-snapshot-groups-${NET_IO}.XXXXXX")
+  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" --atomic 1 \
       --enable-debug-command yes --dir "$GROUP_DIR" --dbfilename groups.tomo \
-      || bad "atomic group cut boot ($PERSIST_IO, atomic 1)"
+      || bad "atomic group cut boot ($NET_IO, atomic 1)"
   py tests/snap_cut_battery.py "$PORT" atomic_groups "$GROUP_DIR/groups.tomo" mset 5 \
-      >"/tmp/gate-snapshot-groups-mset-${PERSIST_IO}.txt" 2>&1 \
-      && grep -q "ATOMIC_GROUP_CUT PASS" "/tmp/gate-snapshot-groups-mset-${PERSIST_IO}.txt" \
-      && ok "snapshot never tears a cross-shard MSET group ($PERSIST_IO, atomic 1)" \
-      || bad "snapshot never tears a cross-shard MSET group ($PERSIST_IO, atomic 1)" \
-             "see /tmp/gate-snapshot-groups-mset-${PERSIST_IO}.txt"
+      >"/tmp/gate-snapshot-groups-mset-${NET_IO}.txt" 2>&1 \
+      && grep -q "ATOMIC_GROUP_CUT PASS" "/tmp/gate-snapshot-groups-mset-${NET_IO}.txt" \
+      && ok "snapshot never tears a cross-shard MSET group ($NET_IO, atomic 1)" \
+      || bad "snapshot never tears a cross-shard MSET group ($NET_IO, atomic 1)" \
+             "see /tmp/gate-snapshot-groups-mset-${NET_IO}.txt"
   stop
   # CONTROL ARM: the DEFAULT --atomic 0. EXEC force-admits a group at either setting, so a
   # transaction is atomic to readers here too and the file must agree with them.
-  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" --atomic 0 \
+  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" --atomic 0 \
       --enable-debug-command yes --dir "$GROUP_DIR" --dbfilename groups.tomo \
-      || bad "atomic group cut boot ($PERSIST_IO, atomic 0)"
+      || bad "atomic group cut boot ($NET_IO, atomic 0)"
   py tests/snap_cut_battery.py "$PORT" atomic_groups "$GROUP_DIR/groups.tomo" exec 3 \
-      >"/tmp/gate-snapshot-groups-exec-${PERSIST_IO}.txt" 2>&1 \
-      && grep -q "ATOMIC_GROUP_CUT PASS" "/tmp/gate-snapshot-groups-exec-${PERSIST_IO}.txt" \
-      && ok "snapshot never tears a MULTI/EXEC group ($PERSIST_IO, default atomic 0)" \
-      || bad "snapshot never tears a MULTI/EXEC group ($PERSIST_IO, default atomic 0)" \
-             "see /tmp/gate-snapshot-groups-exec-${PERSIST_IO}.txt"
+      >"/tmp/gate-snapshot-groups-exec-${NET_IO}.txt" 2>&1 \
+      && grep -q "ATOMIC_GROUP_CUT PASS" "/tmp/gate-snapshot-groups-exec-${NET_IO}.txt" \
+      && ok "snapshot never tears a MULTI/EXEC group ($NET_IO, default atomic 0)" \
+      || bad "snapshot never tears a MULTI/EXEC group ($NET_IO, default atomic 0)" \
+             "see /tmp/gate-snapshot-groups-exec-${NET_IO}.txt"
   stop
 
-  TYPED_DIR=$(mktemp -d "/tmp/gate-snapshot-typed-${PERSIST_IO}.XXXXXX")
-  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
+  TYPED_DIR=$(mktemp -d "/tmp/gate-snapshot-typed-${NET_IO}.XXXXXX")
+  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
       --dir "$TYPED_DIR" --dbfilename typed.tomo \
-      || bad "typed snapshot boot ($PERSIST_IO)"
+      || bad "typed snapshot boot ($NET_IO)"
   py tests/snap_typed_roundtrip.py "$PORT" build_save \
-      >"/tmp/gate-snapshot-typed-${PERSIST_IO}.txt" 2>&1 \
-      && ok "typed snapshot save ($PERSIST_IO)" \
-      || bad "typed snapshot save ($PERSIST_IO)" \
-             "see /tmp/gate-snapshot-typed-${PERSIST_IO}.txt"
+      >"/tmp/gate-snapshot-typed-${NET_IO}.txt" 2>&1 \
+      && ok "typed snapshot save ($NET_IO)" \
+      || bad "typed snapshot save ($NET_IO)" \
+             "see /tmp/gate-snapshot-typed-${NET_IO}.txt"
   stop
-  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
+  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
       --dir "$TYPED_DIR" --load "$TYPED_DIR/typed.tomo" \
-      || bad "typed snapshot reload boot ($PERSIST_IO)"
+      || bad "typed snapshot reload boot ($NET_IO)"
   py tests/snap_typed_roundtrip.py "$PORT" verify \
-      >>"/tmp/gate-snapshot-typed-${PERSIST_IO}.txt" 2>&1 \
-      && ok "typed snapshot reload ($PERSIST_IO)" \
-      || bad "typed snapshot reload ($PERSIST_IO)" \
-             "see /tmp/gate-snapshot-typed-${PERSIST_IO}.txt"
+      >>"/tmp/gate-snapshot-typed-${NET_IO}.txt" 2>&1 \
+      && ok "typed snapshot reload ($NET_IO)" \
+      || bad "typed snapshot reload ($NET_IO)" \
+             "see /tmp/gate-snapshot-typed-${NET_IO}.txt"
   stop
 
-  RACE_DIR=$(mktemp -d "/tmp/gate-snapshot-race-${PERSIST_IO}.XXXXXX")
-  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
+  RACE_DIR=$(mktemp -d "/tmp/gate-snapshot-race-${NET_IO}.XXXXXX")
+  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
       --dir "$RACE_DIR" --dbfilename race.tomo --save '' --enable-debug-command yes \
-      || bad "typed snapshot race boot ($PERSIST_IO)"
+      || bad "typed snapshot race boot ($NET_IO)"
   py tests/snap_typed_race.py "$PORT" race "$RACE_DIR/race.tomo" \
-      >"/tmp/gate-snapshot-race-${PERSIST_IO}.txt" 2>&1 \
-      && grep -q 'PREIMAGE-FIRED PASS' "/tmp/gate-snapshot-race-${PERSIST_IO}.txt" \
-      && grep -q 'SNAPSHOT-TICKET-ORACLE PASS' "/tmp/gate-snapshot-race-${PERSIST_IO}.txt" \
-      && ok "typed snapshot preimage race ($PERSIST_IO)" \
-      || bad "typed snapshot preimage race ($PERSIST_IO)" \
-             "see /tmp/gate-snapshot-race-${PERSIST_IO}.txt"
+      >"/tmp/gate-snapshot-race-${NET_IO}.txt" 2>&1 \
+      && grep -q 'PREIMAGE-FIRED PASS' "/tmp/gate-snapshot-race-${NET_IO}.txt" \
+      && grep -q 'SNAPSHOT-TICKET-ORACLE PASS' "/tmp/gate-snapshot-race-${NET_IO}.txt" \
+      && ok "typed snapshot preimage race ($NET_IO)" \
+      || bad "typed snapshot preimage race ($NET_IO)" \
+             "see /tmp/gate-snapshot-race-${NET_IO}.txt"
   stop
-  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
+  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
       --dir "$RACE_DIR" --load "$RACE_DIR/race.tomo.cut" \
-      || bad "typed snapshot race reload boot ($PERSIST_IO)"
+      || bad "typed snapshot race reload boot ($NET_IO)"
   py tests/snap_typed_race.py "$PORT" verify "$RACE_DIR/race.tomo.oracle.json" \
-      >>"/tmp/gate-snapshot-race-${PERSIST_IO}.txt" 2>&1 \
-      && ok "typed snapshot race reload ($PERSIST_IO)" \
-      || bad "typed snapshot race reload ($PERSIST_IO)" \
-             "see /tmp/gate-snapshot-race-${PERSIST_IO}.txt"
+      >>"/tmp/gate-snapshot-race-${NET_IO}.txt" 2>&1 \
+      && ok "typed snapshot race reload ($NET_IO)" \
+      || bad "typed snapshot race reload ($NET_IO)" \
+             "see /tmp/gate-snapshot-race-${NET_IO}.txt"
   stop
 done
 
@@ -875,10 +862,10 @@ py tests/spinprobe.py $PORT "$SRV" >/tmp/gate-spinprobe.txt 2>&1 \
     && ok "partial-frame conn parks (no io spin)" || bad "partial-frame conn parks (no io spin)" "see /tmp/gate-spinprobe.txt"
 stop
 boot ./build/tomokv --ratio 6:2 --atomic 0 --enable-debug-command yes --flip-auto 1 \
-     --flip-auto-band 2 --lb-age-sample-rate 1024 || bad "flipctl boot"
+     || bad "flipctl boot"
 # 300 -> 600 s: the stable-hold row now MEASURES its precondition (the driver's own command rate
 # must be stationary by the controller's own band) before its assertion window opens, and re-rolls
-# a hold whose load left that band up to three times inside its own wall budget before skipping it.
+# a hold whose load left that band up to three times inside its own wall budget before failing it.
 # The typical run is unchanged; the worst case adds the re-rolls plus their re-anchor waits.
 timeout 600 python3 tests/flipctl.py --host 127.0.0.1 --port $PORT --stable-seconds 30 \
     >/tmp/gate-flipctl.txt 2>&1 \
@@ -966,26 +953,24 @@ timeout 120 python3 tests/pipeorder.py $PORT 400 >/tmp/gate-pipeorder.txt 2>&1 \
 stop
 
 # ---- AOF boot/replay + non-vacuous DEBUG LOADAOF ---------------------------------------------
-for PERSIST_IO in normal uring; do
+for NET_IO in epoll uring; do
 for AOF_ATOMIC in 0 1; do
-AOF_DIR=$(mktemp -d "/tmp/gate-aof-${PERSIST_IO}-atomic${AOF_ATOMIC}.XXXXXX")
+AOF_DIR=$(mktemp -d "/tmp/gate-aof-${NET_IO}-atomic${AOF_ATOMIC}.XXXXXX")
 AOF_STATE=$AOF_DIR/state.json
 boot ./build/tomokv --protected-mode no --atomic "$AOF_ATOMIC" \
     --appendonly yes --appendfsync no \
-    --persist-io "$PERSIST_IO" --enable-debug-command yes --dir "$AOF_DIR" \
-    || bad "AOF purpose boot ($PERSIST_IO, atomic $AOF_ATOMIC)"
-PERSIST_GET=$(redis-cli -h 127.0.0.1 -p $PORT --raw CONFIG GET persist-io 2>/dev/null |
-    tail -1)
-PERSIST_SET=$(redis-cli -h 127.0.0.1 -p $PORT CONFIG SET persist-io "$PERSIST_IO" 2>&1)
-[ "$PERSIST_GET" = "$PERSIST_IO" ] && printf '%s' "$PERSIST_SET" | grep -q immutable \
-    && ok "persist-io surface + immutable ($PERSIST_IO)" \
-    || bad "persist-io surface + immutable ($PERSIST_IO)"
-py tests/aof.py 127.0.0.1 $PORT populate "$AOF_STATE" >/tmp/gate-aof-$PERSIST_IO-$AOF_ATOMIC.txt 2>&1 \
-    && py tests/aof.py 127.0.0.1 $PORT loadaof "$AOF_STATE" >>/tmp/gate-aof-$PERSIST_IO-$AOF_ATOMIC.txt 2>&1 \
-    && ok "AOF byte-exact + script groups + DEBUG LOADAOF ($PERSIST_IO, atomic $AOF_ATOMIC)" \
-    || bad "AOF byte-exact + script groups + DEBUG LOADAOF ($PERSIST_IO, atomic $AOF_ATOMIC)" \
-           "see /tmp/gate-aof-$PERSIST_IO-$AOF_ATOMIC.txt"
-AOF_PRE_MODEL=$(py tests/aof.py 127.0.0.1 $PORT snapshot "$AOF_DIR/dump.tomo" 2>>/tmp/gate-aof-$PERSIST_IO-$AOF_ATOMIC.txt)
+    --net-io "$NET_IO" --enable-debug-command yes --dir "$AOF_DIR" \
+    || bad "AOF purpose boot ($NET_IO, atomic $AOF_ATOMIC)"
+py tests/knobs.py 127.0.0.1 "$PORT" "$NET_IO" "$AOF_ATOMIC" \
+    >"/tmp/gate-knobs-${NET_IO}-${AOF_ATOMIC}.txt" 2>&1 \
+    && ok "configuration reduction + actual geometry ($NET_IO, atomic $AOF_ATOMIC)" \
+    || bad "configuration reduction + actual geometry ($NET_IO, atomic $AOF_ATOMIC)"
+py tests/aof.py 127.0.0.1 $PORT populate "$AOF_STATE" >/tmp/gate-aof-$NET_IO-$AOF_ATOMIC.txt 2>&1 \
+    && py tests/aof.py 127.0.0.1 $PORT loadaof "$AOF_STATE" >>/tmp/gate-aof-$NET_IO-$AOF_ATOMIC.txt 2>&1 \
+    && ok "AOF byte-exact + script groups + DEBUG LOADAOF ($NET_IO, atomic $AOF_ATOMIC)" \
+    || bad "AOF byte-exact + script groups + DEBUG LOADAOF ($NET_IO, atomic $AOF_ATOMIC)" \
+           "see /tmp/gate-aof-$NET_IO-$AOF_ATOMIC.txt"
+AOF_PRE_MODEL=$(py tests/aof.py 127.0.0.1 $PORT snapshot "$AOF_DIR/dump.tomo" 2>>/tmp/gate-aof-$NET_IO-$AOF_ATOMIC.txt)
 AOF_WRITTEN=$(redis-cli -h 127.0.0.1 -p $PORT INFO Persistence 2>/dev/null \
     | tr -d '\r' | sed -n 's/^aof_records_written://p')
 [ -n "$AOF_WRITTEN" ] && [ "$AOF_WRITTEN" -gt 0 ] \
@@ -994,14 +979,14 @@ kill -KILL $SRV 2>/dev/null
 wait $SRV 2>/dev/null
 settle
 boot ./build/tomokv --protected-mode no --appendonly yes --appendfsync no \
-    --atomic "$AOF_ATOMIC" --persist-io "$PERSIST_IO" \
+    --atomic "$AOF_ATOMIC" --net-io "$NET_IO" \
     --enable-debug-command yes --dir "$AOF_DIR" \
-    || bad "AOF replay boot ($PERSIST_IO, atomic $AOF_ATOMIC)"
-py tests/aof.py 127.0.0.1 $PORT verify "$AOF_STATE" >>/tmp/gate-aof-$PERSIST_IO-$AOF_ATOMIC.txt 2>&1 \
-    && ok "AOF process-restart script replay ($PERSIST_IO, atomic $AOF_ATOMIC)" \
-    || bad "AOF process-restart script replay ($PERSIST_IO, atomic $AOF_ATOMIC)" \
-           "see /tmp/gate-aof-$PERSIST_IO-$AOF_ATOMIC.txt"
-AOF_POST_MODEL=$(py tests/aof.py 127.0.0.1 $PORT snapshot "$AOF_DIR/dump.tomo" 2>>/tmp/gate-aof-$PERSIST_IO-$AOF_ATOMIC.txt)
+    || bad "AOF replay boot ($NET_IO, atomic $AOF_ATOMIC)"
+py tests/aof.py 127.0.0.1 $PORT verify "$AOF_STATE" >>/tmp/gate-aof-$NET_IO-$AOF_ATOMIC.txt 2>&1 \
+    && ok "AOF process-restart script replay ($NET_IO, atomic $AOF_ATOMIC)" \
+    || bad "AOF process-restart script replay ($NET_IO, atomic $AOF_ATOMIC)" \
+           "see /tmp/gate-aof-$NET_IO-$AOF_ATOMIC.txt"
+AOF_POST_MODEL=$(py tests/aof.py 127.0.0.1 $PORT snapshot "$AOF_DIR/dump.tomo" 2>>/tmp/gate-aof-$NET_IO-$AOF_ATOMIC.txt)
 [ -n "$AOF_PRE_MODEL" ] && [ "$AOF_PRE_MODEL" = "$AOF_POST_MODEL" ] \
     && ok "AOF native snapshot streams byte-exact" || bad "AOF native snapshot streams byte-exact"
 AOF_REPLAYED=$(redis-cli -h 127.0.0.1 -p $PORT INFO Persistence 2>/dev/null \
@@ -1014,11 +999,11 @@ stop
 done
 
 # ---- AOF atomic-group bracketing + directed interrupted-process recovery ---------------------
-AOF_GROUP_DIR=$(mktemp -d "/tmp/gate-aof-group-${PERSIST_IO}.XXXXXX")
+AOF_GROUP_DIR=$(mktemp -d "/tmp/gate-aof-group-${NET_IO}.XXXXXX")
 AOF_GROUP_STATE=$AOF_GROUP_DIR/state.json
 boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync no \
-    --persist-io "$PERSIST_IO" --enable-debug-command yes --dir "$AOF_GROUP_DIR" \
-    || bad "AOF group purpose boot ($PERSIST_IO)"
+    --net-io "$NET_IO" --enable-debug-command yes --dir "$AOF_GROUP_DIR" \
+    || bad "AOF group purpose boot ($NET_IO)"
 py tests/aof_torn_group.py 127.0.0.1 $PORT prepare "$AOF_GROUP_STATE" \
     >/tmp/gate-aof-group.txt 2>&1 \
     && ok "AOF directed group interruption fired" \
@@ -1026,8 +1011,8 @@ py tests/aof_torn_group.py 127.0.0.1 $PORT prepare "$AOF_GROUP_STATE" \
 wait $SRV 2>/dev/null
 settle
 boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync no \
-    --persist-io "$PERSIST_IO" --enable-debug-command yes --dir "$AOF_GROUP_DIR" \
-    || bad "AOF group recovery boot ($PERSIST_IO)"
+    --net-io "$NET_IO" --enable-debug-command yes --dir "$AOF_GROUP_DIR" \
+    || bad "AOF group recovery boot ($NET_IO)"
 py tests/aof_torn_group.py 127.0.0.1 $PORT verify "$AOF_GROUP_STATE" \
     >>/tmp/gate-aof-group.txt 2>&1 \
     && py tests/aof_torn_group.py 127.0.0.1 $PORT scan \
@@ -1040,11 +1025,11 @@ shutdown_clean \
     && ok "AOF group shutdown invariants" || bad "AOF group shutdown invariants"
 
 # ---- AOF sync policies, reply gate, idle sync, and durability-window recovery ----------------
-AOF_ALWAYS_DIR=$(mktemp -d "/tmp/gate-aof-always-${PERSIST_IO}.XXXXXX")
+AOF_ALWAYS_DIR=$(mktemp -d "/tmp/gate-aof-always-${NET_IO}.XXXXXX")
 AOF_ALWAYS_STATE=$AOF_ALWAYS_DIR/state.json
 boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync always \
-    --persist-io "$PERSIST_IO" --dir "$AOF_ALWAYS_DIR" \
-    || bad "AOF always purpose boot ($PERSIST_IO)"
+    --net-io "$NET_IO" --dir "$AOF_ALWAYS_DIR" \
+    || bad "AOF always purpose boot ($NET_IO)"
 py tests/aof_fsync.py 127.0.0.1 $PORT populate "$AOF_ALWAYS_STATE" always 512 \
     >/tmp/gate-aof-always.txt 2>&1 \
     && ok "AOF always sync + reply gate fired" \
@@ -1053,20 +1038,20 @@ kill -KILL $SRV 2>/dev/null
 wait $SRV 2>/dev/null
 settle
 boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync always \
-    --persist-io "$PERSIST_IO" --dir "$AOF_ALWAYS_DIR" \
-    || bad "AOF always recovery boot ($PERSIST_IO)"
+    --net-io "$NET_IO" --dir "$AOF_ALWAYS_DIR" \
+    || bad "AOF always recovery boot ($NET_IO)"
 py tests/aof_fsync.py 127.0.0.1 $PORT verify "$AOF_ALWAYS_STATE" always 512 \
     >>/tmp/gate-aof-always.txt 2>&1 \
     && ok "AOF always acknowledged-prefix recovery" \
     || bad "AOF always acknowledged-prefix recovery" "see /tmp/gate-aof-always.txt"
 stop
 
-AOF_EVERY_DIR=$(mktemp -d "/tmp/gate-aof-everysec-${PERSIST_IO}.XXXXXX")
+AOF_EVERY_DIR=$(mktemp -d "/tmp/gate-aof-everysec-${NET_IO}.XXXXXX")
 AOF_EVERY_STATE=$AOF_EVERY_DIR/state.json
 AOF_EVERY_FILE=$AOF_EVERY_DIR/appendonlydir/appendonly.aof.1.incr.tomo
 boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync everysec \
-    --persist-io "$PERSIST_IO" --dir "$AOF_EVERY_DIR" \
-    || bad "AOF everysec purpose boot ($PERSIST_IO)"
+    --net-io "$NET_IO" --dir "$AOF_EVERY_DIR" \
+    || bad "AOF everysec purpose boot ($NET_IO)"
 py tests/aof_fsync.py 127.0.0.1 $PORT populate "$AOF_EVERY_STATE" everysec 512 \
     >/tmp/gate-aof-everysec.txt 2>&1 \
     && ok "AOF everysec write gate + idle sync fired" \
@@ -1081,8 +1066,8 @@ case "$AOF_EVERY_FILE" in
 esac
 settle
 boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync everysec \
-    --persist-io "$PERSIST_IO" --dir "$AOF_EVERY_DIR" \
-    || bad "AOF everysec recovery boot ($PERSIST_IO)"
+    --net-io "$NET_IO" --dir "$AOF_EVERY_DIR" \
+    || bad "AOF everysec recovery boot ($NET_IO)"
 py tests/aof_fsync.py 127.0.0.1 $PORT verify "$AOF_EVERY_STATE" everysec 512 \
     >>/tmp/gate-aof-everysec.txt 2>&1 \
     && grep -q "AOF warning: truncated AOF tail" "$SRVLOG" \
@@ -1090,10 +1075,10 @@ py tests/aof_fsync.py 127.0.0.1 $PORT verify "$AOF_EVERY_STATE" everysec 512 \
     || bad "AOF everysec durability window + tail warning" "see /tmp/gate-aof-everysec.txt"
 stop
 
-AOF_NO_DIR=$(mktemp -d "/tmp/gate-aof-no-sync-${PERSIST_IO}.XXXXXX")
+AOF_NO_DIR=$(mktemp -d "/tmp/gate-aof-no-sync-${NET_IO}.XXXXXX")
 boot ./build/tomokv --protected-mode no --atomic 0 --appendonly yes --appendfsync no \
-    --persist-io "$PERSIST_IO" --dir "$AOF_NO_DIR" \
-    || bad "AOF no-sync purpose boot ($PERSIST_IO)"
+    --net-io "$NET_IO" --dir "$AOF_NO_DIR" \
+    || bad "AOF no-sync purpose boot ($NET_IO)"
 py tests/aof_fsync.py 127.0.0.1 $PORT populate "$AOF_NO_DIR/state.json" no 128 \
     >/tmp/gate-aof-no-sync.txt 2>&1 \
     && ok "AOF no-sync bypassed sync + reply gate" \
@@ -1101,19 +1086,19 @@ py tests/aof_fsync.py 127.0.0.1 $PORT populate "$AOF_NO_DIR/state.json" no 128 \
 stop
 
 quiet_wait
-PERSIST_IO=$PERSIST_IO GATE_PORT=$PORT GATE_CORES=$CORES tests/aof_rewrite_matrix.sh \
+NET_IO=$NET_IO GATE_PORT=$PORT GATE_CORES=$CORES tests/aof_rewrite_matrix.sh \
     >/tmp/gate-aof-rewrite.txt 2>&1 \
     && ok "AOF rewrite atomic/stage/corruption matrix" \
     || bad "AOF rewrite matrix" "see /tmp/gate-aof-rewrite.txt"
 
 quiet_wait
-PERSIST_IO=$PERSIST_IO GATE_PORT=$PORT GATE_CORES=$CORES tests/aof_rewrite_trigger_matrix.sh \
+NET_IO=$NET_IO GATE_PORT=$PORT GATE_CORES=$CORES tests/aof_rewrite_trigger_matrix.sh \
     >/tmp/gate-aof-rewrite-trigger.txt 2>&1 \
     && ok "AOF rewrite triggers + observability matrix" \
     || bad "AOF rewrite triggers" "see /tmp/gate-aof-rewrite-trigger.txt"
 
-AOF_OFF_DIR=$(mktemp -d "/tmp/gate-aof-off-${PERSIST_IO}.XXXXXX")
-boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
+AOF_OFF_DIR=$(mktemp -d "/tmp/gate-aof-off-${NET_IO}.XXXXXX")
+boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
     --appendonly no --dir "$AOF_OFF_DIR" \
     || bad "AOF-off negative-control boot"
 AOF_OFF_SEED=$(redis-cli -h 127.0.0.1 -p $PORT SET aof-negative-control must-disappear 2>&1 |
@@ -1125,7 +1110,7 @@ AOF_OFF_PRE_SIZE=$(redis-cli -h 127.0.0.1 -p $PORT DBSIZE 2>/dev/null | tr -d '\
 kill -KILL $SRV 2>/dev/null
 wait $SRV 2>/dev/null
 settle
-boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
+boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
     --appendonly no --dir "$AOF_OFF_DIR" \
     || bad "AOF-off negative-control reboot"
 AOF_OFF_SIZE=$(redis-cli -h 127.0.0.1 -p $PORT DBSIZE 2>/dev/null | tr -d '\r')
@@ -1139,13 +1124,13 @@ done
 # writer flushed a ready GCMT at the top of a writer pass without checking that a large record
 # still held the physical stream. Recovery truncates the file from that large record's first byte,
 # so a control frame inside it is discardable -- and the loader refuses to start on the whole file.
-# persist-io normal only: that is where the defect was demonstrated (11 of 114 runs of the AOF
+# Syscall persistence under epoll: that is where the defect was demonstrated (11 of 114 runs of the AOF
 # battery, 0 of 117 on uring) and where the window is entered reliably enough for the row to prove
 # its mechanism fired. The battery FAILS on a build with the guard removed (6 of 6).
 for AOF_FRAME_ATOMIC in 0 1; do
 AOF_FRAME_DIR=$(mktemp -d "/tmp/gate-aof-frameorder-atomic${AOF_FRAME_ATOMIC}.XXXXXX")
 boot ./build/tomokv --protected-mode no --atomic "$AOF_FRAME_ATOMIC" --appendonly yes \
-    --appendfsync no --persist-io normal --auto-aof-rewrite-percentage 0 \
+    --appendfsync no --net-io epoll --auto-aof-rewrite-percentage 0 \
     --enable-debug-command yes --dir "$AOF_FRAME_DIR" \
     || bad "AOF frame-order purpose boot (atomic $AOF_FRAME_ATOMIC)"
 py tests/aof_frame_order.py 127.0.0.1 $PORT "$AOF_FRAME_DIR/appendonlydir" \
@@ -1212,7 +1197,7 @@ py tests/tls.py 127.0.0.1 "$TLS_PORT" "$TLS_DIR" optional --plain-port "$PORT" \
     >/tmp/gate-tls-optional.txt 2>&1 \
     && ok "TLS client-auth optional matrix" \
     || bad "TLS client-auth optional matrix" "see /tmp/gate-tls-optional.txt"
-# Live kTLS engagement proof on the default (tls-ktls yes) boot: a plain TLS client connects and
+# Live kTLS engagement proof on the default boot: a plain TLS client connects and
 # must see itself counted in the active gauge. Client-auth 'optional' permits a cert-less client.
 python3 - "$TLS_PORT" "$TLS_DIR" <<'PYEOF' >/tmp/gate-ktls-live.txt 2>&1 \
     && ok "kTLS engaged live (default boot)" || bad "kTLS engaged live" "see /tmp/gate-ktls-live.txt"
@@ -1232,9 +1217,9 @@ stop
 shutdown_clean \
     && ok "TLS optional shutdown invariants" || bad "TLS optional shutdown invariants"
 
-# The full battery runs FORCED-FALLBACK so the userspace arm's mechanisms (zc suppression, memory
-# BIO paths) keep their non-vacuous proofs; the default-ktls arm is proven separately below.
-tlsboot no --tls-ktls no --tls-protocols "TLSv1.2 TLSv1.3" --tls-ciphers DEFAULT \
+# Exercise userspace fallback through the retained cipher grammar. TLS 1.2 uses CBC; TLS 1.3
+# uses AES-256, outside this implementation's AES-128 TLS 1.3 RX installer. Counters must fire.
+tlsboot no --tls-protocols "TLSv1.2 TLSv1.3" --tls-ciphers ECDHE-RSA-AES256-SHA384 \
     --tls-ciphersuites TLS_AES_256_GCM_SHA384 --tls-prefer-server-ciphers yes \
     || bad "TLS coexistence purpose boot"
 py tests/tls.py 127.0.0.1 "$TLS_PORT" "$TLS_DIR" no --plain-port "$PORT" --full \
@@ -1272,7 +1257,7 @@ py tests/atomic_torn.py 127.0.0.1 $PORT >/tmp/gate-atomic-torn-asan.txt 2>&1 \
 # atomic groups beats 24 serial round trips by >10%). ASAN does not slow the two arms by the same
 # factor, so that ratio inverts here on a correct build -- measured pipe 21,005/s vs serial
 # 24,202/s in a full-gate run whose every correctness check passed, on a build that passed 4 of 4
-# standalone. The mechanism half of the same section (all 24 admitted, atomic_window_stalls moved,
+# standalone. The mechanism half of the same section (all 24 admitted, multiple groups observed in flight,
 # so a younger cross-key group WAS in flight while an older one decided) still runs on this tier,
 # and the measured rates are still printed in the log.
 py tests/atomic_ryow.py 127.0.0.1 $PORT --no-rate-assertions \
@@ -1345,20 +1330,9 @@ pausable g++ -std=c++20 -O2 -g -march=native -pthread -DTOMO_JEMALLOC -DTOMO_RL_
     && ok "read-local ownership-invariant build" \
     || bad "read-local ownership-invariant build" "see /tmp/gate-rlcachedbg-build.txt"
 quiet_wait
-# --shards 64 and a faster balancer tick are what make the row DETERMINISTIC rather than a coin
-# flip. The defect needs a shard to CHANGE OWNER; `launch` boots --shards 16 and this gate sizes its
-# thread count from its core set, so on a 16-CPU set every thread owns exactly one shard and the
-# balancer has nothing it can take (measured: 9.4M ops, 0 moves -- and the battery's own
-# non-vacuity check correctly refused to report a pass). A later --shards wins over the earlier
-# one, so 64 shards gives every thread several to trade whatever the core count is. The LB knobs
-# are boot-only (CONFIG SET reports "parameter is immutable at runtime"), hence here.
-#
-# ONLY the tick is lowered. Measured over 15 s at 64 shards, moves: default 6, --lb-tick-ms 100
-# -> 50, --lb-move-cap 4 -> 20, --lb-imbalance-pct 1 -> 5, and --lb-cooldown-ms 0 -> ZERO. That
-# last one reads like "no cooldown" and means "never move", which is how the first version of this
-# row silently lost its own precondition. Do not add it back.
-if boot_fused $RLDBG --shards 64 --atomic 1 --read-local 1 --enable-debug-command yes \
-        --lb-tick-ms 100; then
+# More shards than workers permit ownership movement. The automatic balancer must move shards;
+# the battery's existing positive-movement assertion remains mandatory under the derived policy.
+if boot_fused $RLDBG --shards 64 --atomic 1 --read-local 1 --enable-debug-command yes; then
   # The battery carries its own three non-vacuity checks: the armed lane served reads, the block
   # cache actually held blocks, and -- the precondition for this whole class of defect -- the load
   # balancer MOVED shards during the run.
diff --git a/tests/xshard_dispatch_scale.sh b/tests/xshard_dispatch_scale.sh
deleted file mode 100755
index 019243ed5..000000000
--- a/tests/xshard_dispatch_scale.sh
+++ /dev/null
@@ -1,137 +0,0 @@
-#!/bin/bash
-# Cross-shard dispatch scaling guard.
-#   XDS_PORT=7200 XDS_CPUS=48-59 XDS_BIN=./build/tomokv tests/xshard_dispatch_scale.sh
-#
-# GUARDS a defect that was REPRODUCED on a live server (see NOTES-XPERF2.md): the plain scatter
-# dispatch zeroed a kMaxThreads demand array and then walked EVERY configured thread looking for
-# the few that owned a touched shard, so a 2-key cross-shard read cost 11% more at 128 threads
-# than at 4 while touching the same two shards.
-#
-# The arms are built so that only srv_->nthreads() differs:
-#   tid0,tid1 = IO on the first two cpus       (one of them serves the probe connection)
-#   tid2,tid3 = EX on the next two cpus        (--shard-home puts ALL 16 shards on exactly these)
-#   tid4..    = filler EX on the LAST cpu, owning no shard and no connection. They never share
-#               a cpu with an active thread, so raising the count changes srv_->nthreads() and
-#               nothing the measurement depends on.
-# Same shards, same owners, same serving IO thread, same offered load, same client.
-#
-# The assertion is a RATIO between two arms, never an absolute time. It is also non-vacuous:
-# tests/xshard_dispatch_scale.py refuses to report unless DEBUG SHARD proves the "cross" pair
-# really spans two shards, and it measures a same-shard control that never enters the dispatch arm.
-set -u
-PORT=${XDS_PORT:-7200}
-CPUS=${XDS_CPUS:-48-59}
-BIN=${XDS_BIN:-./build/tomokv}
-OPS=${XDS_OPS:-400000}
-ROUNDS=${XDS_ROUNDS:-7}
-SMALL=${XDS_SMALL:-4}
-BIG=${XDS_BIG:-128}
-# The guarded quantity is the DISPATCH EXCESS: cross_ns - same_ns, the median over rounds of what
-# the cross-shard command costs OVER the identical same-shard command measured back to back on the
-# same server. Subtracting the control inside each round removes everything the two share (parse,
-# ROB, task handoff, reply, client) and cancels drift instead of amplifying it.
-# Observed at the shipped settings (7 rounds x 400k ops per arm, best of 2 pairs), 3 runs each:
-#   BEFORE the fix 1.285 / 1.381 / 1.402      AFTER 1.093 / 1.063 / 1.113
-# The raw cross-only ratio is printed too but is NOT the assertion: it does not clear box noise.
-LIMIT=${XDS_LIMIT:-1.20}
-HERE=$(cd "$(dirname "$0")" && pwd)
-
-read -r -a CPULIST <<< "$(python3 - "$CPUS" <<'EOF'
-import sys
-out = []
-for part in sys.argv[1].split(","):
-    if "-" in part:
-        a, b = part.split("-")
-        out += list(range(int(a), int(b) + 1))
-    else:
-        out.append(int(part))
-print(" ".join(str(c) for c in out))
-EOF
-)"
-if [ "${#CPULIST[@]}" -lt 6 ]; then echo "XDS need >=6 cpus in XDS_CPUS"; exit 1; fi
-IO_A=${CPULIST[0]}; IO_B=${CPULIST[1]}; EX_A=${CPULIST[2]}; EX_B=${CPULIST[3]}
-FILL=${CPULIST[${#CPULIST[@]}-1]}
-HOMES=$(python3 -c "print(','.join('%d:%d' % (s, 2 + s % 2) for s in range(16)))")
-
-listener_pid() { ss -lntpH 2>/dev/null | grep -F ":$1 " | grep -o 'pid=[0-9]*' | head -1 | cut -d= -f2; }
-
-stop_arm() {
-  local pid; pid=$(listener_pid "$PORT")
-  [ -z "$pid" ] && return 0
-  kill -TERM "$pid" 2>/dev/null
-  for _ in $(seq 1 200); do kill -0 "$pid" 2>/dev/null || break; sleep 0.05; done
-  kill -0 "$pid" 2>/dev/null && kill -KILL "$pid"
-  for _ in $(seq 1 200); do [ -z "$(listener_pid "$PORT")" ] && return 0; sleep 0.05; done
-  echo "XDS: port $PORT never released"; exit 1
-}
-
-run_arm() {   # $1 = total thread count
-  local t=$1 nfill=$(( $1 - 4 )) place
-  place=$(python3 -c "print(','.join(['ifid@$IO_A','ifid@$IO_B','ex@$EX_A','ex@$EX_B']+['ex@$FILL']*$nfill))")
-  stop_arm
-  ( taskset -c "$CPUS" "$BIN" --port "$PORT" --bind 127.0.0.1 --enable-debug-command yes \
-      --place "$place" --shards 16 --shard-home "$HOMES" >/tmp/xds-$t.log 2>&1 & )
-  for _ in $(seq 1 400); do [ -n "$(listener_pid "$PORT")" ] && break; sleep 0.05; done
-  if [ -z "$(listener_pid "$PORT")" ]; then echo "XDS: arm $t never listened"; tail -3 /tmp/xds-$t.log; exit 1; fi
-  python3 "$HERE/xshard_dispatch_scale.py" 127.0.0.1 "$PORT" "$OPS" 32 "$ROUNDS"
-}
-
-# Two independent small/big PAIRS, and the verdict is the BETTER (lower) of the two ratios. The
-# arms are separate boots, so a pair cannot be measured simultaneously and slow drift across one
-# pair lands entirely in that pair's ratio; taking the minimum of two pairs discards a drifted
-# pair without hiding a real regression (the pre-fix ratio was >= 1.13 in every pair measured).
-PAIRS=""
-for _ in $(seq 1 "${XDS_PAIRS:-2}"); do
-  SMALL_OUT=$(run_arm "$SMALL") || { echo "XDS: small arm failed"; stop_arm; exit 1; }
-  BIG_OUT=$(run_arm "$BIG")     || { echo "XDS: big arm failed"; stop_arm; exit 1; }
-  echo "  small $SMALL_OUT"
-  echo "  big   $BIG_OUT"
-  PAIRS="$PAIRS|$SMALL_OUT;$BIG_OUT"
-done
-stop_arm
-
-python3 - "$PAIRS" "$LIMIT" "$SMALL" "$BIG" <<'EOF'
-import sys
-def parse(line):
-    return {k: v for k, v in (f.split("=", 1) for f in line.split()[1:])}
-pairs = [tuple(parse(x) for x in p.split(";")) for p in sys.argv[1].split("|") if p]
-limit = float(sys.argv[2])
-want_small, want_big = int(sys.argv[3]), int(sys.argv[4])
-small, big = pairs[0]
-fail = 0
-def check(name, cond, detail=""):
-    global fail
-    print("  %-52s %s %s" % (name, "ok" if cond else "FAIL", detail))
-    if not cond:
-        fail += 1
-check("small arm really ran %d threads" % want_small, int(small["threads"]) == want_small,
-      "threads=%s" % small["threads"])
-check("big arm really ran %d threads" % want_big, int(big["threads"]) == want_big,
-      "threads=%s" % big["threads"])
-# The hash seed is drawn from the kernel at every boot, so the shard IDS differ between arms; what
-# the guard needs identical is the FAN-OUT: two owner threads for the cross pair, one for the
-# control. A run that fanned out to one owner would not exercise the dispatch arm under test.
-check("both arms fanned out to two owners",
-      small["cross_owners"] == "2" and big["cross_owners"] == "2",
-      "small=%s big=%s" % (small["cross_owners"], big["cross_owners"]))
-check("both controls stayed on one owner",
-      small["same_owners"] == "1" and big["same_owners"] == "1",
-      "small=%s big=%s" % (small["same_owners"], big["same_owners"]))
-ratios = []
-for s_arm, b_arm in pairs:
-    se, be = float(s_arm["excess_ns"]), float(b_arm["excess_ns"])
-    check("cross-shard really costs more than same-shard in both arms", se > 0 and be > 0,
-          "excess %.1f / %.1f ns" % (se, be))
-    ratios.append(be / se if se > 0 else float("inf"))
-ratio = min(ratios)
-check("dispatch excess ratio %dt/%dt <= %.2f" % (want_big, want_small, limit), ratio <= limit,
-      "pairs %s  best=%.3f" % (" ".join("%.3f" % r for r in ratios), ratio))
-print("  %-52s ok cross %.1f->%.1f (%.3f)  same %.1f->%.1f (%.3f)"
-      % ("raw per-op costs (diagnostic, not asserted)",
-         float(small["cross_ns"]), float(big["cross_ns"]),
-         float(big["cross_ns"]) / float(small["cross_ns"]),
-         float(small["same_ns"]), float(big["same_ns"]),
-         float(big["same_ns"]) / float(small["same_ns"])))
-print("XSHARD-DISPATCH-SCALE %s" % ("PASS" if fail == 0 else "FAIL %d" % fail))
-sys.exit(1 if fail else 0)
-EOF

exec
/bin/bash -lc "cat CODEX-OUT.md && rg --files --hidden -g AGENTS.md -g '"'!'"\\.git' /home/user/Projects/cx-merge /home/user/Projects/cx-timing /home/user/Projects/cx-knobs" in /home/user/Projects/cx-merge
 exited 1 in 0ms:
Reading additional input from stdin...
OpenAI Codex v0.153.4
--------
workdir: /home/user/Projects/cx-merge
model: gpt-6-astra
provider: openai
approval: never
sandbox: danger-full-access
reasoning effort: max
reasoning summaries: none
session id: 01a07b68-60f4-7842-b765-a149640742a1
--------
user
# TomoKV — project context for codex

Prepend this to every codex prompt. It is the shared background; each prompt then states its own scope.
Written 2026-09-07. Baseline commit `78c3e5391` (`origin/cpp`), full gate 344/344.

## What it is

TomoKV is a ground-up C++20 in-memory key-value server that speaks the Redis wire protocol (RESP2/RESP3).
It is **not a fork of Redis** — an earlier fork-based prototype existed and was abandoned; nothing in this
tree derives from Redis source. It implements **243 registered commands**. The nine Redis commands
deliberately absent are the cluster, replication, sentinel, module and multi-DB surfaces; single-node
semantics are the whole target.

It is a research system. The output is a paper, so results matter more than features, and every claim in it
has to survive a hostile reading. Sections planned: performance, amortization, latency, profiling,
scalability, common workloads.

## Architecture

**Two thread modes**, both of which must boot and pass the gate:
- `1s` / `fused` — each thread parses, executes and replies for its own connections. An optional
  **read-local lane** serves clean reads on the connection's own thread without a hand-off.
- `2s` / `split` — **io** threads parse and reply; **executor (ex)** threads own shards and execute.
  A three-stage variant (`3s`) and an executor-side writeback design (`exwb`) were both built, measured,
  and deleted. Do not propose either.

**Sharding and ownership.** Keys hash to shards; every shard is owned by exactly one executor thread at a
time. Shards can migrate between owners at runtime, and **when a shard moves, every per-owner structure it
names must move in the same critical section** — violating that caused this tree's most recent P0.

**Ordering ring (`rob`).** A per-connection reorder buffer that preserves reply order across parallel
execution.

**Atomics.** Multi-key atomicity uses group-scoped epoch-MVCC. Cross-shard commands use scatter-gather.
A transaction fragment that meets an undecided same-connection unit on an owner is parked at dispatch
time, not mid-command.

**Flip.** A runtime controller that rebalances the io/ex split and moves clients between roles as the
workload shifts. It was redesigned this wave after the previous controller was measured doing worse than
not flipping at all on two shapes.

**Load balancing.** Key-affine and client-affine balancing (`--key-lb`, `--client-lb`), both default on.

Networking is io_uring; the allocator is jemalloc.

## Non-negotiable correctness laws

Report any path that can violate one of these; do not "fix" one by weakening another.
- Single-owner writes.
- Reads are never obstructed by writes: immutable replacement plus QSBR. **No reader retries and no
  seqlocks on the per-operation path.**
- In-place overwrite is banned while the read-local lane is armed.
- Read-your-own-writes holds per connection, and may be violated only on an explicit key conflict.
- Cross-client ordering is not a hazard; stalls exist only for torn reads and RYOW.
- Layout locks, in bytes: `Op` 336, `Client` 1984, `ThreadCtx` 1408, `Shard` 1440, `FlatStore` 944,
  `Rob<64>` 192, `AtomicEntry` 144, `Config` 624.
- Both thread modes boot. Main commands carry zero regression. One file per feature.

## The box

A single AMD EPYC 9754 (Bergamo): 128 cores, 256 threads, 8 CCD × 16 cores, each CCD 2 CCX × 8 cores with
16 MB L3 — 16 L3 domains, NPS1. Measured node optimum is 8 cores, one CCX. Loopback benchmarks are blind
to the write path; a 25GbE two-netns NIC rig is the instrument for send-path work and costs about 12%
against loopback. Box noise is ±0.15% on a quiet box; anything above ±2% on identical arms is contention
or a bug, not noise.

**The gate boots nothing like a default server.** `tests/gate.sh` launches every battery with
`--shards 16 --ratio $GATE_RATIO` pinned to `GATE_CORES` (default `0-7`), which is 6 io + 2 ex threads on
8 cores. Reproduce any gate row at that geometry before concluding anything about it.

## What it is measured against

Redis 7.4 and 8.8.1, Dragonfly 1.40.2, KeyDB, Microsoft Garnet (net10.0), memcached 1.6.45, plus YCSB
0.17.0 traces. Binaries and digests: `/home/user/Projects/bench-bins/MANIFEST.md`.

We do not win everywhere and the paper will not claim we do. At 64 real cores and pipeline depth 128,
Garnet reaches 62.8M ops/s against our 34.4M. We hold depth 1, where we reach 4.35M. Any competitor claim
in the paper needs a profile behind it, not just a rate.

## How performance is judged

**cycles/op = instructions/op ÷ IPC.** Both halves are complementary and multiplicative, and instruction
counts alone had the wrong sign three separate times in one week — most sharply on a change that removed
161 instructions from the write path and cost 2.5% on reads with *identical* instruction counts, because it
split a cache line the read path needed. Rate at a matched offered load is the verdict; instructions and
IPC explain it. Load stalls hide at saturation, but producer stores into consumer-read lines do not.

Every optimization lands with a PRE vs POST table. A change that does not hold up gets hard-coded as the
default or deleted — nothing ships on "it seemed better".

## Why this work looks the way it does

Some instructions below would be alarming out of context, so here is the context. This is a single-user
research box. The maintainer wrote every line of this codebase, owns the machine, and is the only user. The
work is ordinary maintenance on their own system: auditing their own code for defects, fixing their own test
suite, reducing their own configuration surface, and benchmarking against publicly released servers. Nothing
here touches anyone else's data, systems or accounts.

Four specifics that read oddly without the reason:

1. **"Never run the server, the benchmark or the gate."** This is about measurement integrity and nothing
   else. Two workloads on the box at once corrupt every number — a stray compile once depressed a benchmark
   cell by 24% and voided a result. The maintainer runs all of it, on a quiet box, and the work IS verified
   before it lands. You are being asked to produce a diff, not to skip verification.

2. **"Never edit `EXPECT_QUICK` / `EXPECT_FULL`."** Those constants are how many test rows must pass. Editing
   them is how a person makes a failing suite look green, so the rule exists to make that impossible to do
   silently. It is a stricter standard than normal, not a looser one. State what the count should become and
   why; the maintainer changes it.

3. **Concurrency vocabulary.** This is a database. "Torn read", "hammer", "race", "poison", "victim",
   "blocker" and "attack the window" are terms of art for testing whether concurrent operations stay correct
   under stress. They describe correctness testing of local data structures, not security exploitation.

4. **Deliberately breaking a mechanism to check a test.** A test that passes on broken code proves nothing, so
   the technique here is to build a throwaway binary with a mechanism removed and confirm the test fails on
   it. That build is never committed and never runs outside the box. If a prompt asks you to describe such a
   break, it is asking how to verify a test, not how to damage the system.

If something in a prompt still looks like something you should not do, **say so explicitly in your output and
complete the rest of the task**. A stated objection is useful. Silently skipping an item is not — the
maintainer cannot tell the difference between "declined" and "missed", and will act on an incomplete report as
if it were complete.

## Working rules for codex

- **Never run a server, a benchmark, or the gate.** The maintainer runs the box. Report, or produce a diff.
- Edit only inside your own worktree.
- Never edit `EXPECT_QUICK` / `EXPECT_FULL` in `tests/gate.sh`. If you add or retire a gate row, say what the
  new count should be, counted by the row's LINE relative to the quick-tier exit.
- Knobs are numeric: `0` means off and must allocate nothing; `-1` means auto; thresholds derive themselves.
  Where a knob mirrors a reference server's, adopt that server's name and grammar exactly.
- A test that cannot fail proves nothing. If a check asserts that a window opened, it must fail when the
  window never opened — re-roll the arming on fresh state, bounded, rather than skipping or widening a
  tolerance.

## Closed doors — measured, negative, do not re-propose

`3s` three-stage mode; `exwb` executor-side writeback (−11 to −22%); AMAC and any CR/MR split (the only
boundary is io/ex); reorder-and-window; seqlocks on the per-op path; connection-side record allocation
(`connalloc`, cycles flat, record born on the wrong core); executor-major task collapse (`exmajor`,
−2 to −3%); the read-local capacity hand-off line split (`t-pubdiet`, +1.97% SET but −2.50% GET).

--- TASK ---

You are performing a three-way merge of two independently developed lanes in a C++20 key-value server, and
resolving one genuine SEMANTIC conflict between them. Work only in this worktree. Do not build, do not run the
server, benchmarks or the gate — the maintainer runs those and will gate the merged result.

THE INPUTS. Common ancestor: commit 78c3e5391, which is this worktree's HEAD and is also the base both lanes
branched from. The two lanes are uncommitted working trees you may read directly:
  * /home/user/Projects/cx-timing  — test-suite determinism work. ALREADY GATED CLEAN at 344 ok / 0 FAIL with
    zero SKIP rows. Its central change adds a `--release-build` flag so timing assertions run only on release
    builds while counter movement, exact values and safety bounds stay mandatory on every build.
  * /home/user/Projects/cx-knobs   — configuration-surface reduction, 40 files. Deletes a set of knobs, folds
    eight load-balancer knobs into one `--lb`, and changes `sizeof(Config)` from 624 to 528. It states the gate
    counts must become quick 325 / full 342. Read its DESIGN-KNOBS.md, including its OBJECTIONS section.

Produce a single merged working tree here containing BOTH lanes' work.

TEXTUAL CONFLICTS, three files both lanes touched: tests/atomic_torn.py, tests/gate.sh,
tests/xshard_dispatch_scale.sh. For each, take both sides' intent, not one side's text.

THE ONE THAT MATTERS, in tests/atomic_torn.py: the BASE already contains a bounded re-roll loop for the OFF
torn-RENAME control (`for _roll in range(4)`, carrying the comment that it does NOT degrade to a skip, unlike
the SINTERSTORE, COPY and RENAMENX controls beside it). cx-timing then layers its release-build split on the
same file. BOTH must survive the merge: the re-roll loop and its hard-failure policy, AND the mechanism/timing
split. If you believe they interact, say how and choose deliberately — do not let one silently overwrite the
other.

THE SEMANTIC CONFLICT. cx-knobs deletes `--shard-home`, and argues in its objections that this removes the
instrument the cross-shard dispatch-scaling gate row depended on: the retired wrapper kept all 16 shards on two
real executors and added empty filler executors via the manual map, so without it the row would assert an
unvalidated claim on a different geometry. cx-knobs therefore RETIRES that row. cx-timing, independently, was
told to FIX that same row (it passes today only by taking the best of two pairs while both arms routinely
exceed its 1.20 threshold) and rewrote its threshold logic.

Decide this, and justify it: does cx-timing's rewrite make the row valid WITHOUT `--shard-home`? If yes, keep
the fixed row and drop the retirement. If no, the retirement wins and cx-timing's rewrite of that row is moot —
say so plainly rather than keeping dead code. Read both sides before deciding; do not split the difference.

RULES.
  * Never silently drop a hunk. Every change from either lane is kept, deliberately superseded, or dropped with
    a stated reason. A merge that loses work without saying so is the failure mode here.
  * Do not edit `EXPECT_QUICK` or `EXPECT_FULL` in tests/gate.sh. State what they must become, with the
    arithmetic: start from 327 quick / 344 full, then list every row added or retired and which side of the
    quick-tier exit (`[ "$TIER" = quick ]`) its emitting LINE falls on. cx-knobs claims 325 / 342 — verify that
    against the merged tree rather than trusting it, and correct it if the merge changes the count.
  * Preserve every correctness law in the project context above. If the two lanes' changes together violate one
    that neither violated alone, stop at that point and report it.
  * The merged tree must be internally consistent: no reference to a deleted knob, no test invoking a removed
    flag, no documentation describing a retired instrument as live.

DELIVERABLE: the merged working tree, plus MERGE.md with a per-conflict record (file, both intents, the
decision, the reason), the semantic-conflict ruling with its evidence, the EXPECT arithmetic, and a list of
anything you dropped and why. The maintainer gates the result; your job is that the merge is honest and
complete, not that it passes.
codex
I’ll compare both lanes against the common base, resolve the dispatch-scaling conflict from the code and objections, and merge into this worktree. I’ll record every superseded change in `MERGE.md`. I won’t build or run the server, benchmarks, or gate.
exec
/bin/bash -lc 'for f in /AGENTS.md /home/AGENTS.md /home/user/AGENTS.md /home/user/Projects/AGENTS.md; do if [ -f "$f" ]; then echo "$f"; cat "$f"; fi; done' in /home/user/Projects/cx-merge
 succeeded in 0ms:
exec
/bin/bash -lc 'git status --short && git rev-parse HEAD && git diff --stat && git ls-files --others --exclude-standard' in /home/user/Projects/cx-timing
 succeeded in 0ms:
 M DESIGN-GATEHYGIENE.md
 M tests/atomic_torn.py
 M tests/borrow_registry.py
 M tests/differ.py
 M tests/expireindex.py
 M tests/gate.sh
 M tests/infofix.py
 M tests/pubsub.py
 M tests/slowlog.py
 M tests/stream.py
 M tests/xshard_dispatch_scale.py
 M tests/xshard_dispatch_scale.sh
?? CODEX-OUT.md
78c3e53919a5d557bd3dd4625c9524d0baa212e4
 DESIGN-GATEHYGIENE.md          | 57 +++++++++++++++++++++++++
 tests/atomic_torn.py           | 88 ++++++++++++++++++++++++++++++--------
 tests/borrow_registry.py       | 48 ++++++++++++++++++---
 tests/differ.py                | 64 +++++++++++++++++++++-------
 tests/expireindex.py           | 40 ++++++++++++++----
 tests/gate.sh                  | 24 +++++++----
 tests/infofix.py               | 49 +++++++++++++++++-----
 tests/pubsub.py                | 18 ++++++++
 tests/slowlog.py               | 23 ++++++++--
 tests/stream.py                | 49 +++++++++++++++++++---
 tests/xshard_dispatch_scale.py | 10 +++--
 tests/xshard_dispatch_scale.sh | 95 +++++++++++++++++++++++++++++-------------
 12 files changed, 457 insertions(+), 108 deletions(-)
CODEX-OUT.md

exec
/bin/bash -lc 'git status --short && git rev-parse HEAD && git diff --stat && git ls-files --others --exclude-standard' in /home/user/Projects/cx-knobs
 succeeded in 0ms:
 A DESIGN-KNOBS.md
 M docs/specs/AUDIT-AOF.md
 M docs/thread-mode.md
 M src/base/topology.h
 M src/cmd/lbsignals.cc
 M src/cmd/server_tail.cc
 M src/cmd/t_server.cc
 M src/core/config.h
 M src/core/ex_loop.h
 M src/core/flipctl.cc
 M src/core/io_loop.h
 M src/core/iopipe_pipeline.h
 M src/core/placement.h
 M src/core/server.h
 M src/core/shard.h
 M src/core/weighted_lb.h
 M src/main.cc
 M src/net/tls.cc
 M src/net/uring.h
 M src/persist/aof.cc
 M src/store/eviction.h
 M src/store/flatstore.h
 M tests/_lib.py
 M tests/aof_rewrite_matrix.sh
 M tests/aof_rewrite_trigger_matrix.sh
 M tests/atomic_ryow.py
 M tests/atomic_torn.py
 A tests/atomicwindow.py
 M tests/bplus.py
 M tests/config_parser_test.cc
 M tests/evict_battery.py
 M tests/flipctl.py
 M tests/gate.sh
 A tests/knobs.py
 M tests/lbsignals.py
 M tests/rlcache_churn.py
 M tests/tls.py
 M tests/xscript.py
 D tests/xshard_dispatch_scale.sh
 M tomokv.conf
?? CODEX-OUT.md
78c3e53919a5d557bd3dd4625c9524d0baa212e4
 DESIGN-KNOBS.md                     | 324 ++++++++++++++++++++++++++++++++
 docs/specs/AUDIT-AOF.md             |  10 +-
 docs/thread-mode.md                 | 108 +++--------
 src/base/topology.h                 |  42 ++++-
 src/cmd/lbsignals.cc                |   4 +-
 src/cmd/server_tail.cc              |   7 +-
 src/cmd/t_server.cc                 |  61 +-----
 src/core/config.h                   | 358 ++++++------------------------------
 src/core/ex_loop.h                  |  37 ++--
 src/core/flipctl.cc                 |  28 ++-
 src/core/io_loop.h                  |  10 +-
 src/core/iopipe_pipeline.h          |   2 +-
 src/core/placement.h                |  80 ++------
 src/core/server.h                   | 228 +++++++++++++----------
 src/core/shard.h                    |   6 +-
 src/core/weighted_lb.h              |  79 ++++++++
 src/main.cc                         |  21 +--
 src/net/tls.cc                      |   4 +-
 src/net/uring.h                     |   2 +-
 src/persist/aof.cc                  |   2 +-
 src/store/eviction.h                |   3 +
 src/store/flatstore.h               |  45 ++---
 tests/_lib.py                       |   2 +-
 tests/aof_rewrite_matrix.sh         |   8 +-
 tests/aof_rewrite_trigger_matrix.sh |   4 +-
 tests/atomic_ryow.py                |  28 ++-
 tests/atomic_torn.py                | 107 ++---------
 tests/atomicwindow.py               | 225 ++++++++++++++++++++++
 tests/bplus.py                      |  11 +-
 tests/config_parser_test.cc         | 270 ++++++++++-----------------
 tests/evict_battery.py              |  57 +++---
 tests/flipctl.py                    |  34 ++--
 tests/gate.sh                       | 276 +++++++++++++--------------
 tests/knobs.py                      |  48 +++++
 tests/lbsignals.py                  |   6 +-
 tests/rlcache_churn.py              |  13 +-
 tests/tls.py                        |   4 +-
 tests/xscript.py                    |  82 ++++-----
 tests/xshard_dispatch_scale.sh      | 137 --------------
 tomokv.conf                         | 168 ++++-------------
 40 files changed, 1405 insertions(+), 1536 deletions(-)
CODEX-OUT.md

exec
/bin/bash -lc "pwd && git status --short && git rev-parse HEAD && rg --files -g AGENTS.md -g '"'!build*'"' -g '"'!vendor*'"' -g '"'!third_party*'"'" in /home/user/Projects/cx-merge
 exited 1 in 0ms:
/home/user/Projects/cx-merge
?? CODEX-OUT.md
78c3e53919a5d557bd3dd4625c9524d0baa212e4


exec
/bin/bash -lc 'git diff -- tests/xshard_dispatch_scale.sh tests/xshard_dispatch_scale.py tests/gate.sh' in /home/user/Projects/cx-timing
 succeeded in 0ms:
diff --git a/tests/gate.sh b/tests/gate.sh
index c4d8fef19..c6428f573 100755
--- a/tests/gate.sh
+++ b/tests/gate.sh
@@ -431,7 +431,7 @@ E=$(shutdown_value work.executed)
 # Explicit ON boot plus the non-vacuous epoch-MVCC gates. atomic_torn includes its own OFF control,
 # predecessor/promotion counters, overlapping writers, window liveness, and live CONFIG flips.
 boot ./build/tomokv --atomic 1 --enable-debug-command yes || bad "atomic release boot"
-py tests/atomic_torn.py 127.0.0.1 $PORT >/tmp/gate-atomic-torn.txt 2>&1 \
+py tests/atomic_torn.py 127.0.0.1 $PORT --release-build >/tmp/gate-atomic-torn.txt 2>&1 \
     && ok "atomic torn/window battery" || bad "atomic torn/window battery" "see /tmp/gate-atomic-torn.txt"
 py tests/atomic_ryow.py 127.0.0.1 $PORT >/tmp/gate-atomic-ryow.txt 2>&1 \
     && ok "atomic RYOW/mixed-write battery" || bad "atomic RYOW/mixed-write battery" "see /tmp/gate-atomic-ryow.txt"
@@ -452,7 +452,9 @@ for AT in 0 1; do
   boot ./build/tomokv --atomic $AT --enable-debug-command yes \
       || bad "feature battery boot (atomic $AT)"
   for t in $FEATURE_BATTERIES; do
-    py tests/$t.py 127.0.0.1 $PORT >/tmp/gate-$t-$AT.txt 2>&1 \
+    FEATURE_ARGS=()
+    [ "$t" = stream ] && FEATURE_ARGS+=(--release-build)
+    py tests/$t.py 127.0.0.1 $PORT "${FEATURE_ARGS[@]}" >/tmp/gate-$t-$AT.txt 2>&1 \
         && ok "$t battery (atomic $AT)" || bad "$t battery (atomic $AT)" "see /tmp/gate-$t-$AT.txt"
   done
   stop
@@ -509,7 +511,9 @@ for AT in 0 1; do
     bad "fused+armed boot line (atomic $AT)" "server did not boot; see $SRVLOG"
   fi
   for t in $FEATURE_BATTERIES; do
-    py tests/$t.py 127.0.0.1 $PORT >/tmp/gate-fusedarmed-$t-$AT.txt 2>&1 \
+    FEATURE_ARGS=()
+    [ "$t" = stream ] && FEATURE_ARGS+=(--release-build)
+    py tests/$t.py 127.0.0.1 $PORT "${FEATURE_ARGS[@]}" >/tmp/gate-fusedarmed-$t-$AT.txt 2>&1 \
         && ok "fused+armed $t battery (atomic $AT)" \
         || bad "fused+armed $t battery (atomic $AT)" "see /tmp/gate-fusedarmed-$t-$AT.txt"
   done
@@ -661,13 +665,13 @@ for XS in "off:--script-crossshard-max-bytes 0" \
 done
 
 # ---- efficiency guards: each needs a boot geometry of its own, so each gets its own server ----
-# These assert a RATIO or a growth bound, never an absolute time, and each was verified to FAIL on
-# the binary from before its fix (see NOTES-XPERF2.md). They shipped unwired, which is the same
-# coverage-that-does-not-exist problem the gate lane fixed elsewhere.
+# Exact mechanism checks run on every build; timing budgets require the explicit release-build
+# flag. A ratio of wall times is still a performance claim (see DESIGN-GATEHYGIENE.md). The
+# cross-shard guard also checks instrument stability before scoring any timing, retaining all pairs.
 # --shards 1 for the same reason as the borrow guard: the sidecar under test is per shard, and the
 # battery asserts that precondition rather than quietly measuring a diluted one.
 boot ./build/tomokv --shards 1 --enable-debug-command yes || bad "expire-index guard boot"
-py tests/expireindex.py 127.0.0.1 $PORT >/tmp/gate-expireindex.txt 2>&1 \
+py tests/expireindex.py 127.0.0.1 $PORT --release-build >/tmp/gate-expireindex.txt 2>&1 \
     && ok "expire-index growth bound" || bad "expire-index growth bound" "see /tmp/gate-expireindex.txt"
 stop
 
@@ -675,13 +679,13 @@ stop
 # an ordinary-sized value still takes the borrow path and pays registry cost.
 boot ./build/tomokv --shards 1 --zc-min 64 --client-output-buffer-limit "normal 0 0 0" \
     --enable-debug-command yes || bad "borrow-registry guard boot"
-py tests/borrow_registry.py 127.0.0.1 $PORT >/tmp/gate-borrow.txt 2>&1 \
+py tests/borrow_registry.py 127.0.0.1 $PORT --release-build >/tmp/gate-borrow.txt 2>&1 \
     && ok "borrow-registry growth bound" || bad "borrow-registry growth bound" "see /tmp/gate-borrow.txt"
 stop
 
 # this one boots its own arms at two thread counts, so it takes the port/cores rather than a server
 quiet_wait
-XDS_PORT=$PORT XDS_CPUS=$CORES XDS_BIN=./build/tomokv bash tests/xshard_dispatch_scale.sh \
+XDS_PORT=$PORT XDS_CPUS=$CORES XDS_BIN=./build/tomokv bash tests/xshard_dispatch_scale.sh --release-build \
     >/tmp/gate-xds.txt 2>&1 \
     && ok "cross-shard dispatch scaling" || bad "cross-shard dispatch scaling" "see /tmp/gate-xds.txt"
 
@@ -1266,6 +1270,8 @@ py tests/torture.py 127.0.0.1 $PORT >/tmp/gate-tort-asan.txt 2>&1 \
     && ok "torture under ASAN" || bad "torture under ASAN"
 py tests/ryow.py 127.0.0.1 $PORT >/tmp/gate-ryow-asan.txt 2>&1 \
     && ok "RYOW under ASAN" || bad "RYOW under ASAN"
+# Deliberately omit --release-build: all five coverage/safety checks remain mandatory, while
+# their measured completion times are printed without scoring release scheduling budgets.
 py tests/atomic_torn.py 127.0.0.1 $PORT >/tmp/gate-atomic-torn-asan.txt 2>&1 \
     && ok "atomic torn/window under ASAN" || bad "atomic torn/window under ASAN"
 # --no-rate-assertions: the battery's overlap section makes one claim about SPEED (pipelining 24
diff --git a/tests/xshard_dispatch_scale.py b/tests/xshard_dispatch_scale.py
index 5ddf26fe5..4e85fbda5 100755
--- a/tests/xshard_dispatch_scale.py
+++ b/tests/xshard_dispatch_scale.py
@@ -100,9 +100,11 @@ def main():
         print("FAIL: could not find both a two-owner cross-shard pair and a same-shard pair")
         sys.exit(1)
     keys = [base % 0, cross[0], same[0]]
-    for k in keys:
-        assert c.cmd("SET", k, b"x") == b"OK"
-    reply = b"*2\r\n$1\r\nx\r\n$1\r\nx\r\n"
+    # Different payloads make returning the first shard twice fail byte-for-byte; identical 'x'
+    # values could conceal that regression. Cross and same still have identical reply shapes.
+    for index, k in enumerate(keys):
+        assert c.cmd("SET", k, b"x" if index == 0 else b"y") == b"OK"
+    reply = b"*2\r\n$1\r\nx\r\n$1\r\ny\r\n"
     fcross = enc("MGET", base % 0, cross[0])
     fsame = enc("MGET", base % 0, same[0])
     c.rate(fcross, reply, DEPTH * 50, DEPTH)
@@ -118,6 +120,8 @@ def main():
         excess.append(cn - sn)
         crosses.append(cn)
         sames.append(sn)
+    if c.shard_owners() != owners:
+        raise AssertionError("shard ownership moved during the arm; disable key/client balancing")
     print("ARM threads=%d excess_ns=%.1f cross_ns=%.1f same_ns=%.1f cross_owners=%d same_owners=%d"
           % (c.threads(), statistics.median(excess), statistics.median(crosses),
              statistics.median(sames),
diff --git a/tests/xshard_dispatch_scale.sh b/tests/xshard_dispatch_scale.sh
index 019243ed5..92c5a0699 100755
--- a/tests/xshard_dispatch_scale.sh
+++ b/tests/xshard_dispatch_scale.sh
@@ -15,10 +15,16 @@
 #               nothing the measurement depends on.
 # Same shards, same owners, same serving IO thread, same offered load, same client.
 #
-# The assertion is a RATIO between two arms, never an absolute time. It is also non-vacuous:
+# Exact replies and owner fan-out are checked on every build. With --release-build, score the
+# timing ratio only when repeated arms and the same-shard control certify a stable instrument:
 # tests/xshard_dispatch_scale.py refuses to report unless DEBUG SHARD proves the "cross" pair
 # really spans two shards, and it measures a same-shard control that never enters the dispatch arm.
 set -u
+RELEASE_BUILD=0
+case "${1:-}" in
+  --release-build) RELEASE_BUILD=1; shift;;
+esac
+if [ "$#" -ne 0 ]; then echo "usage: $0 [--release-build]" >&2; exit 2; fi
 PORT=${XDS_PORT:-7200}
 CPUS=${XDS_CPUS:-48-59}
 BIN=${XDS_BIN:-./build/tomokv}
@@ -28,10 +34,12 @@ SMALL=${XDS_SMALL:-4}
 BIG=${XDS_BIG:-128}
 # The guarded quantity is the DISPATCH EXCESS: cross_ns - same_ns, the median over rounds of what
 # the cross-shard command costs OVER the identical same-shard command measured back to back on the
-# same server. Subtracting the control inside each round removes everything the two share (parse,
-# ROB, task handoff, reply, client) and cancels drift instead of amplifying it.
+# same server. Subtracting the control inside each round removes common costs (parse, ROB, task
+# handoff, reply, client); it cannot cancel arbitrary scheduling drift between measured intervals.
 # Observed at the shipped settings (7 rounds x 400k ops per arm, best of 2 pairs), 3 runs each:
 #   BEFORE the fix 1.285 / 1.381 / 1.402      AFTER 1.093 / 1.063 / 1.113
+# Those historical best-of-two results do not license choosing the best pair in a gate. Both
+# pairs are retained below; an unstable instrument produces no timing verdict, with all data kept.
 # The raw cross-only ratio is printed too but is NOT the assertion: it does not clear box noise.
 LIMIT=${XDS_LIMIT:-1.20}
 HERE=$(cd "$(dirname "$0")" && pwd)
@@ -70,62 +78,93 @@ run_arm() {   # $1 = total thread count
   place=$(python3 -c "print(','.join(['ifid@$IO_A','ifid@$IO_B','ex@$EX_A','ex@$EX_B']+['ex@$FILL']*$nfill))")
   stop_arm
   ( taskset -c "$CPUS" "$BIN" --port "$PORT" --bind 127.0.0.1 --enable-debug-command yes \
-      --place "$place" --shards 16 --shard-home "$HOMES" >/tmp/xds-$t.log 2>&1 & )
+      --place "$place" --shards 16 --shard-home "$HOMES" \
+      --key-lb 0 --client-lb 0 --flip-auto 0 >/tmp/xds-$t.log 2>&1 & )
   for _ in $(seq 1 400); do [ -n "$(listener_pid "$PORT")" ] && break; sleep 0.05; done
   if [ -z "$(listener_pid "$PORT")" ]; then echo "XDS: arm $t never listened"; tail -3 /tmp/xds-$t.log; exit 1; fi
   python3 "$HERE/xshard_dispatch_scale.py" 127.0.0.1 "$PORT" "$OPS" 32 "$ROUNDS"
 }
 
-# Two independent small/big PAIRS, and the verdict is the BETTER (lower) of the two ratios. The
-# arms are separate boots, so a pair cannot be measured simultaneously and slow drift across one
-# pair lands entirely in that pair's ratio; taking the minimum of two pairs discards a drifted
-# pair without hiding a real regression (the pre-fix ratio was >= 1.13 in every pair measured).
+# Alternate boot order (small/big, big/small) so a monotone drift does not always tax the big arm.
+# No retry is selected by its result, and no failed mechanism arm is retried.
 PAIRS=""
-for _ in $(seq 1 "${XDS_PAIRS:-2}"); do
-  SMALL_OUT=$(run_arm "$SMALL") || { echo "XDS: small arm failed"; stop_arm; exit 1; }
-  BIG_OUT=$(run_arm "$BIG")     || { echo "XDS: big arm failed"; stop_arm; exit 1; }
+for pair in $(seq 1 "${XDS_PAIRS:-2}"); do
+  if (( pair % 2 )); then
+    SMALL_OUT=$(run_arm "$SMALL") || { echo "XDS: small arm failed"; stop_arm; exit 1; }
+    BIG_OUT=$(run_arm "$BIG")     || { echo "XDS: big arm failed"; stop_arm; exit 1; }
+  else
+    BIG_OUT=$(run_arm "$BIG")     || { echo "XDS: big arm failed"; stop_arm; exit 1; }
+    SMALL_OUT=$(run_arm "$SMALL") || { echo "XDS: small arm failed"; stop_arm; exit 1; }
+  fi
   echo "  small $SMALL_OUT"
   echo "  big   $BIG_OUT"
   PAIRS="$PAIRS|$SMALL_OUT;$BIG_OUT"
 done
 stop_arm
 
-python3 - "$PAIRS" "$LIMIT" "$SMALL" "$BIG" <<'EOF'
+python3 - "$PAIRS" "$LIMIT" "$SMALL" "$BIG" "$RELEASE_BUILD" <<'EOF'
 import sys
+import statistics
 def parse(line):
     return {k: v for k, v in (f.split("=", 1) for f in line.split()[1:])}
 pairs = [tuple(parse(x) for x in p.split(";")) for p in sys.argv[1].split("|") if p]
 limit = float(sys.argv[2])
 want_small, want_big = int(sys.argv[3]), int(sys.argv[4])
-small, big = pairs[0]
 fail = 0
 def check(name, cond, detail=""):
     global fail
     print("  %-52s %s %s" % (name, "ok" if cond else "FAIL", detail))
     if not cond:
         fail += 1
-check("small arm really ran %d threads" % want_small, int(small["threads"]) == want_small,
-      "threads=%s" % small["threads"])
-check("big arm really ran %d threads" % want_big, int(big["threads"]) == want_big,
-      "threads=%s" % big["threads"])
 # The hash seed is drawn from the kernel at every boot, so the shard IDS differ between arms; what
 # the guard needs identical is the FAN-OUT: two owner threads for the cross pair, one for the
 # control. A run that fanned out to one owner would not exercise the dispatch arm under test.
-check("both arms fanned out to two owners",
-      small["cross_owners"] == "2" and big["cross_owners"] == "2",
-      "small=%s big=%s" % (small["cross_owners"], big["cross_owners"]))
-check("both controls stayed on one owner",
-      small["same_owners"] == "1" and big["same_owners"] == "1",
-      "small=%s big=%s" % (small["same_owners"], big["same_owners"]))
+check("measurement has at least two pairs", len(pairs) >= 2)
+for index, (small, big) in enumerate(pairs, 1):
+    check("pair %d small arm really ran %d threads" % (index, want_small),
+          int(small["threads"]) == want_small, "threads=%s" % small["threads"])
+    check("pair %d big arm really ran %d threads" % (index, want_big),
+          int(big["threads"]) == want_big, "threads=%s" % big["threads"])
+    check("pair %d both arms fanned out to two owners" % index,
+          small["cross_owners"] == "2" and big["cross_owners"] == "2")
+    check("pair %d both controls stayed on one owner" % index,
+          small["same_owners"] == "1" and big["same_owners"] == "1")
+# The box's supplied noise contract says >2% on IDENTICAL arms is contention or a defect, not
+# random noise. Check repeatability separately within each thread count, and same-shard controls
+# across counts. Do not qualify the instrument using the dispatch ratio being judged.
+spreads = {}
+for side, name in ((0, "small"), (1, "big")):
+    for metric in ("cross_ns", "same_ns"):
+        values = [float(pair[side][metric]) for pair in pairs]
+        spreads[name + "_" + metric] = max(values) / min(values) if values and min(values) > 0 else float("inf")
+controls = [float(arm["same_ns"]) for pair in pairs for arm in pair]
+spreads["control_across_counts"] = max(controls) / min(controls) if controls and min(controls) > 0 else float("inf")
+stationary = len(pairs) >= 2 and all(spread <= 1.02 for spread in spreads.values())
+reason = ("requires --release-build" if sys.argv[5] != "1" else
+          "instrument not stationary (2% control/repeat bound)" if not stationary else "")
+print("  instrument spreads=%r" % spreads)
 ratios = []
 for s_arm, b_arm in pairs:
     se, be = float(s_arm["excess_ns"]), float(b_arm["excess_ns"])
-    check("cross-shard really costs more than same-shard in both arms", se > 0 and be > 0,
-          "excess %.1f / %.1f ns" % (se, be))
+    detail = "excess %.1f / %.1f ns" % (se, be)
+    if reason:
+        print("  SKIP cross-shard really costs more than same-shard -- %s; %s" % (reason, detail))
+    else:
+        check("cross-shard really costs more than same-shard in both arms", se > 0 and be > 0, detail)
     ratios.append(be / se if se > 0 else float("inf"))
-ratio = min(ratios)
-check("dispatch excess ratio %dt/%dt <= %.2f" % (want_big, want_small, limit), ratio <= limit,
-      "pairs %s  best=%.3f" % (" ".join("%.3f" % r for r in ratios), ratio))
+ratio = statistics.median(ratios) if ratios else float("inf")
+# Restore the nthreads() scan/whole-array zeroing to induce a stable release regression; a good
+# second pair cannot erase it by selection. Misroute/drop a key to fail byte-exact arms on ANY
+# build. Indeterminate timings are explicitly SKIP, never a green claim about scaling.
+label = "dispatch excess ratio %dt/%dt <= %.2f" % (want_big, want_small, limit)
+detail = "pairs %s median=%.3f" % (" ".join("%.3f" % r for r in ratios), ratio)
+if reason:
+    print("  SKIP %s -- %s; %s" % (label, reason, detail))
+else:
+    check(label, all(r <= limit for r in ratios), detail)
+small, big = pairs[0] if pairs else ({}, {})
+if not pairs:
+    sys.exit(1)
 print("  %-52s ok cross %.1f->%.1f (%.3f)  same %.1f->%.1f (%.3f)"
       % ("raw per-op costs (diagnostic, not asserted)",
          float(small["cross_ns"]), float(big["cross_ns"]),

exec
/bin/bash -lc "sed -n '205,340p' DESIGN-KNOBS.md && git diff -- tests/atomic_torn.py" in /home/user/Projects/cx-knobs
 succeeded in 0ms:
so total admissions are deliberately not equated to total replies. Both rows retain exact credit
reclamation checks; the second specifically exercises old-generation lease return (`server.h:3324`).

Repair validation was server-free: Python AST parsing, `git diff --check`, and 12 in-memory
transport fault-injection cases covering release-before-completion, the one-connection arm,
live reconfiguration, fresh retry, four-miss failure, and immediate failure on no resume, bad
reply, excess live groups, and lost credits. A control containing only newer post-CONFIG groups
also exhausted discovery rather than passing. Stalls first seen during drain cannot rescue an
unarmed attempt, and extra admissions from abandoned preparations need not equal reply counts.
These are harness checks, not measurements of
TomoKV. No build, server, benchmark, or gate was run for the repair. For maintainer-only broken
server controls: disabling admission-backpressure release must fail the resume deadline; dropping
old-generation credit returns must fail pool reclamation; suppressing the stall witness must
exhaust discovery. None of those server mutations was applied or run here.

## Gate accounting and review checks

**Required expected counts: quick 325, full 342** (343 with the optional NIC row).
`EXPECT_QUICK=327` and `EXPECT_FULL=344` at lines 141–142 of `tests/gate.sh` are deliberately
unchanged for the maintainer to edit. Counted by emitting line, not battery name or file location:

| Row change | Baseline emitting line | Position relative to quick exit | Quick delta | Full delta |
| --- | --- | --- | ---: | ---: |
| Retire `xscript off control`; keep limit/window with automatic bounds | 658, inside the former three-arm loop | Before old exit 1260; surviving two-arm row now emits at 649, before exit 1245 | -1 | -1 |
| Retire manual-map dispatch scaling guard and its shell wrapper | 686 | Before old exit 1260; retirement note now at 673, before exit 1245 | -1 | -1 |
| Replace persistence CONFIG surface check with removed-knob/actual-geometry checks | 981 | Four iterations before the exit; replacement emits at 966 | 0 | 0 |
| Restore separate atomic liveness and reconfiguration checks inside `atomic_torn.py` | Internal base lines 894 and 957 | Outer release row still emits at 435, before quick exit 1245; ASAN row still emits at 1255, after it | 0 | 0 |
| All other battery rows | Existing lines, retaining multiplicity | Same side of the quick exit | 0 | 0 |

Restoring the internal check adds one separately reported assertion per battery invocation, not
another `gate.sh` ledger entry. Expected counts therefore **remain quick 325 / full 342**, or 343
with the optional NIC row; the known PROGRAM-STATE mismatch is unchanged. No outer battery row
was added. New helper files are consumed by existing rows. Parser coverage
rejects every removed spelling even with its formerly valid default, checks the study grammar,
and checks sample-budget invariance, jitter excursion rejection, and transfer-cost pacing.
Compile-time assertions cover the shard-default scaling and cap. `tests/knobs.py` checks the
CONFIG removals and compares INFO geometry with DEBUG's actual thread/shard inventory on the
four existing persistence/atomic boots. Normal persistence coverage now boots `net-io epoll`;
uring coverage still boots `net-io uring`.

Atomic admission tests use fresh disjoint cross-owner keys and held groups at the production
credit limit. The global arm must observe stalls and multiple live groups, maintain the bound,
and reclaim all credits. The one-connection arm must observe simultaneous groups on that one
connection. Each has four bounded fresh attempts and fails if its witness never appears, with
the DEBUG hold cleared before the resume deadline. The independent reconfiguration row uses
live `atomic` with old-generation groups still present; resizing itself is retired with the knob.
Live ON/OFF toggle coverage also remains. The separate rate comparison uses an unheld burst.

The script staging control exceeds the default 4 MiB budget and still requires refusal before
RUN with untouched values. The cut-slot arm uses more than four connections per serving thread,
retains its serial negative control, and re-arms fresh connections up to three times before
failing. LRU rows age all old cohorts across one real 256-second bucket before probing ordinary
reads, NO-TOUCH, and eviction survival; no clock override remains. The four rows and their
positive lane-hit witnesses remain. Their individual clock wait is bounded at 260 seconds.

The TLS fallback battery uses the retained cipher grammar (TLS 1.2 CBC; TLS 1.3 AES-256 outside
the current custom AES-128 RX installer) and retains its live fallback/zero-copy-suppression
assertions. Default kTLS engagement retains its separate positive witness. Idle LB signal tests
now require zero age samples; the maneuver test must observe positive age sampling. The stable
flip hold uses the reported derived band and fails after bounded unsuccessful re-arms.

Initial reduction validation: serial C++20 syntax checks with warnings enabled for the parser,
main, fused loop, scatter, server/INFO, LB reporting, flip controller, TLS, persistence, snapshot,
and OBJECT implementations; Python AST parsing; shell syntax checks; diff/removed-reference
and layout-assertion inspection. No executable was linked or run by Codex. The maintainer then
reported the failure documented above. The repaired runtime witnesses and their broken-server
controls, and performance, remain unverified; the repair's server-free checks are listed above.

| Matched geometry/load comparison | PRE `78c3e5391` | POST working diff |
| --- | --- | --- |
| GET/SET rates, cycles/op, instructions/op, IPC in both modes | Not measured in this worktree | Not measured |
| Enabled LB under stationary/skewed/changing load | Existing fixed controller | New derivations; benchmark pending |
| `lb=0` / flip sampling dark | Source baseline | Arming/allocation paths inspected; benchmark pending |
| Full gate | 344/344 supplied by maintainer | Maintainer's pre-repair run: 340 ok, 3 FAIL (including the expected ledger mismatch); repair not run, expected count still 342 |

## OBJECTIONS

1. **`--read-local-prefetch-capture` / `--read-local-atomic-filter`: inherited reader retry.**
   The winning captured MGET path already violates the supplied “no reader retries” law:
   `prepare_captured_local_mget` in `src/core/ex_loop.h` sets `kAttempts=2` at line 1295,
   loops at 1317, and increments `mget_generation_retries` before repeating at 1431–1432.
   The same loop exists in the baseline, and this diff still selects that winning path.
   It was not redesigned or silently removed. The default GET capture arm breaks on table
   churn; the confirmed normal-path objection is MGET. Resolving the inherited law conflict
   requires a separate correctness decision, not a covert change in this surface reduction.

2. **`--genthread-schedule`: “zero uses” was not literal.** Baseline
   `src/core/config.h:702–716` actively lowered `coarse/iofused/streams` into fused mode and
   overlap 0/1/2, with parser tests for the aliases. There was no independent Config field.
   The alias is deleted as prescribed; former callers now fail explicitly.

3. **`--lb` and derived `--shards`: measurement still required.** The specified derivation
   scheme is implemented, but the 4096-sample target, eight shards per executor, initial
   all-visit sampling, and transfer pacing have no PRE/POST rate evidence in this task.
   Age sampling estimates command volume, so fanout affects actual stamped-task counts.
   A pointer transfer's timed drain/install duration also does not measure subsequent cache
   warmup or commands lost. These limitations matter to a paper and to the instruction/IPC
   verdict; the formulae are implementation choices within the specified derivations, not
   claims of measured optima. No runtime tuning knobs were retained to defer this decision.

4. **`--smt-mode` / `--l3-domains`: stricter boot geometry.** Automatic pair mode invokes
   the existing complete-pair and same-role validation. Odd logical ratios, incomplete explicit
   pairs, or a split restricted to one available pair can now be rejected. L3 override repairs
   L3 discovery; sibling sysfs still must be readable to derive and validate pair mode.
   These consequences are recorded rather than adding an unrequested SMT override.

5. **`--shard-home`: one regression instrument is lost.** The retired scaling wrapper kept
   all 16 shards on two real executors and added empty filler executors using the manual map.
   Default round-robin changes that ownership with thread count. Keeping its former measured
   ratio threshold on a different geometry would assert an unvalidated claim, so its one row
   is explicitly retired rather than reporting a misleading pass.

6. **`--lru-clock-shift`: gate duration increases.** Keeping the winning 256-second clock
   and preserving a real positive age witness costs up to 260 seconds on each of four boots.
   No debug clock selector or artificial tolerance was introduced. The maintainer should budget
   for those waits when running either tier.

All prescribed knob dispositions are implemented. No item was left undone for a newly
introduced compile failure or correctness-law violation; the inherited MGET conflict above
is explicitly retained for maintainer review.
diff --git a/tests/atomic_torn.py b/tests/atomic_torn.py
index 448e00be6..de3cbc575 100755
--- a/tests/atomic_torn.py
+++ b/tests/atomic_torn.py
@@ -865,104 +865,23 @@ else:
          copy_on[0] == 0 and not copy_on[1] and not copy_on[2],
          "anomalies=%d errors=%r threads_still_alive=%r" % copy_on)
 
-# Admission liveness: with a one-group window, the second frame in one received pipeline reaches
-# admission before owner notifications for the first are flushed. This makes the fired assertion
-# deterministic instead of depending on Python threads winning a scheduling race.
+# Separate admission liveness from live credit reconfiguration/accounting. Both use the
+# production bound and must actually enter their window; a clean miss is re-armed, never skipped.
+from atomicwindow import held_burst
 config("atomic", 1)
-config("atomic-window", 1)
-window_before = stats().get("atomic_window_stalls", 0)
-window_run = format(time.time_ns() & 0xfffffff, "x")
-window_errors = []
-window_client = Resp()
-window_frames = []
-for sequence in range(64):
-    args = ["MSET"]
-    for key_index in range(8):
-        args.extend(("aw%s:%x:%x" % (window_run, sequence, key_index),
-                     "window:%x" % sequence))
-    window_frames.append(frame(*args))
 try:
-    window_client.sock.sendall(b"".join(window_frames))
-    for _ in window_frames:
-        if window_client.read() != b"OK":
-            raise AssertionError("bad window reply")
+    witness = held_burst(HOST, PORT, whole_window=True)
+    note("derived atomic window stalls and resumes", True, witness)
 except Exception as exc:
-    window_errors.append(str(exc))
-finally:
-    window_client.close()
-window_after = stats().get("atomic_window_stalls", 0)
-note("atomic-window stalls and resumes",
-     not window_errors and window_after > window_before,
-     "stalls=%d errors=%r" % (window_after - window_before, window_errors))
-config("atomic-window", 256)
-
-# Credit leases must preserve the exact configured bound while CONFIG changes it under load. A
-# shrink may inherit more already-admitted groups than the new limit; wait for that unavoidable
-# debt to retire, then prove no subsequent sample exceeds the bound. Finally, an idle system must
-# have returned every leased credit to the pool so a skewed next IO can consume the whole window.
-lease_stop = threading.Event()
-lease_errors = []
-
-
-def lease_writer(wid):
-    client = None
-    keys = ["at:lease:%d:k%d" % (wid, key) for key in range(8)]
-    seq = 0
-    try:
-        client = Resp()
-        while not lease_stop.is_set():
-            if mset(client, keys, "lease:%d:%d" % (wid, seq)) != b"OK":
-                raise AssertionError("bad lease reply")
-            seq += 1
-    except Exception as exc:
-        lease_errors.append("writer%d:%s" % (wid, exc))
-    finally:
-        if client is not None:
-            client.close()
-
+    note("derived atomic window stalls and resumes", False, str(exc))
 
-lease_threads = [threading.Thread(target=lease_writer, args=(i,), daemon=True) for i in range(8)]
-for thread in lease_threads:
-    thread.start()
-for value in (31, 7, 19, 3):
-    if not config("atomic-window", value):
-        lease_errors.append("CONFIG atomic-window %d failed" % value)
-deadline = time.time() + 3
-bounded = False
-max_inflight = 0
-while time.time() < deadline:
-    sample = stats()
-    live = sample.get("atomic_inflight", 1000000)
-    debt = sample.get("atomic_credit_debt", 1000000)
-    if live <= 3 and debt == 0:
-        bounded = True
-        break
-    time.sleep(0.01)
-if bounded:
-    deadline = time.time() + 0.4
-    while time.time() < deadline:
-        live = stats().get("atomic_inflight", 1000000)
-        max_inflight = max(max_inflight, live)
-        if live > 3:
-            bounded = False
-            break
-lease_stop.set()
-for thread in lease_threads:
-    thread.join(timeout=10)
-deadline = time.time() + 3
-lease_after = stats()
-while lease_after.get("atomic_inflight", -1) != 0 and time.time() < deadline:
-    time.sleep(0.01)
-    lease_after = stats()
-note("atomic-window reconfiguration preserves bound and reclaims leases",
-     bounded and not any(thread.is_alive() for thread in lease_threads) and not lease_errors and
-     lease_after.get("atomic_inflight", -1) == 0 and
-     lease_after.get("atomic_credit_debt", -1) == 0 and
-     lease_after.get("atomic_credit_pool", -1) == 3,
-     "max=%d pool=%d debt=%d errors=%r" %
-     (max_inflight, lease_after.get("atomic_credit_pool", -1),
-      lease_after.get("atomic_credit_debt", -1), lease_errors))
-config("atomic-window", 256)
+# Resizing was removed with atomic-window. CONFIG SET atomic 1 still rebuilds credit generations;
+# require surviving old-generation groups before asserting the fixed bound and reclaimed leases.
+try:
+    witness = held_burst(HOST, PORT, whole_window=True, reconfigure=True)
+    note("atomic reconfiguration preserves derived bound and reclaims leases", True, witness)
+except Exception as exc:
+    note("atomic reconfiguration preserves derived bound and reclaims leases", False, str(exc))
 
 # Live CONFIG flips under active traffic are a liveness/safety arm. OFF intervals deliberately do
 # not promise atomicity; after ending ON, one final group must be read intact.

codex
The timing rewrite still boots with `--shard-home`; its repeatability checks don’t replace the fixed ownership geometry. The retirement wins. I’ll remove the wrapper and its now-unused Python helper, and explain why the threshold rewrite becomes moot. I also read the knobs lane’s objections, including its report of an inherited MGET reader retry.
exec
/bin/bash -lc 'git diff -- tests/gate.sh' in /home/user/Projects/cx-knobs
 succeeded in 0ms:
diff --git a/tests/gate.sh b/tests/gate.sh
index c4d8fef19..295db179f 100755
--- a/tests/gate.sh
+++ b/tests/gate.sh
@@ -1,7 +1,7 @@
 #!/bin/bash
 # PRE-PUSH GATE for tomokv-cpp (pure 2s baseline).
 #
-#   tests/gate.sh quick   loopback only, ~3 min: build (release+ASAN), footprint locks, boot
+#   tests/gate.sh quick   loopback only: build (release+ASAN), footprint locks, boot
 #                         matrix, smoke, torture, RYOW, atomic torn/mixed-write/window gates,
 #                         shutdown invariants, counter-fired assertions, idle-loop ceiling. Runs on
 #                         any machine.
@@ -465,7 +465,7 @@ for AT in 0 1; do
   if boot_fused ./build/tomokv --atomic "$AT" --enable-debug-command yes; then
     FUSED_INFO=$(redis-cli -h 127.0.0.1 -p "$PORT" INFO server 2>/dev/null | tr -d '\r')
     FUSED_MODE=$(printf '%s\n' "$FUSED_INFO" | sed -n 's/^thread_mode://p')
-    FUSED_OVERLAP=$(printf '%s\n' "$FUSED_INFO" | sed -n 's/^overlap://p')
+    FUSED_OVERLAP=$(printf '%s\n' "$FUSED_INFO" | sed -n 's/^x_overlap://p')
     [ "$FUSED_MODE" = 1s ] && [ "$FUSED_OVERLAP" = 0 ] \
         && ok "fused boot line (atomic $AT)" \
         || bad "fused boot line (atomic $AT)" "wire mode=$FUSED_MODE overlap=$FUSED_OVERLAP"
@@ -528,20 +528,20 @@ done
 # ---- eviction accounting on both read paths: owner-served (split) and lane-served (fused+armed) --
 # tests/evict_battery.py needs one FRESH boot per section (it sets maxmemory itself and has no
 # FLUSHALL). lfu proves OBJECT FREQ rises for ordinary reads and stays put for CLIENT NO-TOUCH
-# reads, plus hot-set survival under pressure; lruclock is the LRU twin on a 1 s bucket
-# (--lru-clock-shift 0: the default 256 s bucket cannot tell two keys apart inside one gate row).
+# reads, plus hot-set survival under pressure; lruclock ages all cohorts across one fixed 256 s
+# production bucket (a bounded wait of up to 260 s per boot) before testing the read paths.
 # On the armed boot both sections also assert the reads they measure were LANE-served -- a key
 # kept hot only by lane reads was never touched before ExLoopT::note_local_read_access and so was
 # evicted FIRST, which is the inverse of the policy the boot asked for.
 for AT in 0 1; do
   for EVSEC in lfu lruclock; do
-    boot ./build/tomokv --atomic $AT --lru-clock-shift 0 \
+    boot ./build/tomokv --atomic $AT \
         || bad "eviction $EVSEC boot (split, atomic $AT)"
     py tests/evict_battery.py $PORT $EVSEC >/tmp/gate-evict-$EVSEC-split-$AT.txt 2>&1 \
         && ok "eviction $EVSEC battery (split, atomic $AT)" \
         || bad "eviction $EVSEC battery (split, atomic $AT)" "see /tmp/gate-evict-$EVSEC-split-$AT.txt"
     stop
-    boot_fused ./build/tomokv --atomic $AT --read-local 1 --lru-clock-shift 0 \
+    boot_fused ./build/tomokv --atomic $AT --read-local 1 \
         || bad "eviction $EVSEC boot (fused+armed, atomic $AT)"
     py tests/evict_battery.py $PORT $EVSEC >/tmp/gate-evict-$EVSEC-armed-$AT.txt 2>&1 \
         && ok "eviction $EVSEC battery (fused+armed, atomic $AT)" \
@@ -554,7 +554,7 @@ done
 # DEBUG geometry makes false positives impossible in these arms: A/B share one physical shard but
 # use distinct filter cells, as do P/C on the other participating owner. ATOMIC-COMMIT-DELAY holds
 # A/P after raw install and atomic_commit_holds proves the reserved/unpublished window was sampled.
-boot_fused ./build/tomokv --atomic 1 --read-local 1 --read-local-atomic-filter 1 \
+boot_fused ./build/tomokv --atomic 1 --read-local 1 \
     --enable-debug-command yes \
     || bad "B+ read-local purpose boot"
 py tests/bplus.py 127.0.0.1 "$PORT" >/tmp/gate-bplus.txt 2>&1 \
@@ -592,7 +592,6 @@ stop
 # opposite the source. No same-owner or one-executor run can satisfy this row.
 for AT in 0 1; do
   boot ./build/tomokv --ratio 6:2 --atomic "$AT" --enable-debug-command yes \
-       --lb-age-sample-rate 1024 \
       || bad "cross-owner SORT boot (atomic $AT)"
   py tests/sort.py 127.0.0.1 "$PORT" >/tmp/gate-sort-$AT.txt 2>&1 \
       && ok "cross-owner SORT battery (atomic $AT)" \
@@ -602,7 +601,7 @@ done
 
 # ---- debug-surface batteries: these drive DEBUG subcommands, hence their own armed boot -------
 for AT in 0 1; do
-  boot ./build/tomokv --atomic $AT --enable-debug-command yes --lb-age-sample-rate 1024 \
+  boot ./build/tomokv --atomic $AT --enable-debug-command yes \
       || bad "debug-surface boot (atomic $AT)"
   # scriptatomic needs the armed boot for its cross-shard section (DEBUG SHARD proves the group
   # really spans owners; ATOMIC-COMMIT-DELAY / ATOMIC-READ-DELAY widen the window). It flips
@@ -642,17 +641,9 @@ for AT in 0 1; do
   stop
 done
 
-# ---- cross-owner script control boots: each needs its own knob value, hence its own server ----
-# off:    script-crossshard-max-bytes 0 must reproduce today's CROSSSLOT byte-for-byte and leave
-#         every cross counter at zero (the feature is off ⇒ nothing is allocated).
-# limit:  a tiny staging budget must refuse before RUN and leave the declared values untouched.
-# window: one cut slot per IO thread must refuse the overflow with -BUSY, counted exactly, with
-#         the same activations issued one at a time as the zero control.
-for XS in "off:--script-crossshard-max-bytes 0" \
-          "limit:--script-crossshard-max-bytes 300" \
-          "window:--script-crossshard-cut-slots 1"; do
-  XS_MODE=${XS%%:*}; XS_ARGS=${XS#*:}
-  boot ./build/tomokv --atomic 1 --enable-debug-command yes $XS_ARGS \
+# ---- cross-owner script bounds: production auto budgets and four cut slots per IO ----------
+for XS_MODE in limit window; do
+  boot ./build/tomokv --atomic 1 --enable-debug-command yes \
       || bad "xscript $XS_MODE control boot"
   py tests/xscript.py 127.0.0.1 $PORT "$XS_MODE" >/tmp/gate-xscript-$XS_MODE.txt 2>&1 \
       && ok "xscript $XS_MODE control" || bad "xscript $XS_MODE control" \
@@ -679,11 +670,7 @@ py tests/borrow_registry.py 127.0.0.1 $PORT >/tmp/gate-borrow.txt 2>&1 \
     && ok "borrow-registry growth bound" || bad "borrow-registry growth bound" "see /tmp/gate-borrow.txt"
 stop
 
-# this one boots its own arms at two thread counts, so it takes the port/cores rather than a server
-quiet_wait
-XDS_PORT=$PORT XDS_CPUS=$CORES XDS_BIN=./build/tomokv bash tests/xshard_dispatch_scale.sh \
-    >/tmp/gate-xds.txt 2>&1 \
-    && ok "cross-shard dispatch scaling" || bad "cross-shard dispatch scaling" "see /tmp/gate-xds.txt"
+# Dispatch scaling's manual ownership geometry was retired with shard-home; DESIGN-KNOBS.md.
 
 # ---- Redis-wire DUMP/RESTORE survives the native snapshot/restart boundary -------------------
 DUMPRESTORE_DIR=$(mktemp -d /tmp/gate-dumprestore.XXXXXX)
@@ -743,25 +730,25 @@ py tests/debug.py 127.0.0.1 $PORT >/tmp/gate-debug.txt 2>&1 \
 stop
 
 # ---- snapshot data/sync engines: cut, typed round-trip, and typed preimage race ----------------
-for PERSIST_IO in normal uring; do
-  SNAP_DIR=$(mktemp -d "/tmp/gate-snapshot-${PERSIST_IO}.XXXXXX")
-  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
+for NET_IO in epoll uring; do
+  SNAP_DIR=$(mktemp -d "/tmp/gate-snapshot-${NET_IO}.XXXXXX")
+  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
       --dir "$SNAP_DIR" --dbfilename cut.tomo \
-      || bad "snapshot cut boot ($PERSIST_IO)"
+      || bad "snapshot cut boot ($NET_IO)"
   py tests/snap_cut_battery.py "$PORT" save \
-      >"/tmp/gate-snapshot-cut-${PERSIST_IO}.txt" 2>&1 \
-      && ok "snapshot concurrent cut ($PERSIST_IO)" \
-      || bad "snapshot concurrent cut ($PERSIST_IO)" \
-             "see /tmp/gate-snapshot-cut-${PERSIST_IO}.txt"
+      >"/tmp/gate-snapshot-cut-${NET_IO}.txt" 2>&1 \
+      && ok "snapshot concurrent cut ($NET_IO)" \
+      || bad "snapshot concurrent cut ($NET_IO)" \
+             "see /tmp/gate-snapshot-cut-${NET_IO}.txt"
   stop
-  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
+  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
       --dir "$SNAP_DIR" --load "$SNAP_DIR/cut.tomo" \
-      || bad "snapshot cut reload boot ($PERSIST_IO)"
+      || bad "snapshot cut reload boot ($NET_IO)"
   py tests/snap_cut_battery.py "$PORT" verify_cut \
-      >>"/tmp/gate-snapshot-cut-${PERSIST_IO}.txt" 2>&1 \
-      && ok "snapshot cut reload ($PERSIST_IO)" \
-      || bad "snapshot cut reload ($PERSIST_IO)" \
-             "see /tmp/gate-snapshot-cut-${PERSIST_IO}.txt"
+      >>"/tmp/gate-snapshot-cut-${NET_IO}.txt" 2>&1 \
+      && ok "snapshot cut reload ($NET_IO)" \
+      || bad "snapshot cut reload ($NET_IO)" \
+             "see /tmp/gate-snapshot-cut-${NET_IO}.txt"
   stop
 
   # The cut battery above writes only single keys, so it passes on a tree whose snapshot tears
@@ -769,70 +756,70 @@ for PERSIST_IO in normal uring; do
   # generation-tagged groups, a live MGET reader that must stay clean throughout, and the
   # cuts_waited counter that separates a real group drain from a vacuous one. Both arms were
   # confirmed to FAIL on the pre-fix tree (44-53 and 10-13 torn groups per cut respectively).
-  GROUP_DIR=$(mktemp -d "/tmp/gate-snapshot-groups-${PERSIST_IO}.XXXXXX")
-  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" --atomic 1 \
+  GROUP_DIR=$(mktemp -d "/tmp/gate-snapshot-groups-${NET_IO}.XXXXXX")
+  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" --atomic 1 \
       --enable-debug-command yes --dir "$GROUP_DIR" --dbfilename groups.tomo \
-      || bad "atomic group cut boot ($PERSIST_IO, atomic 1)"
+      || bad "atomic group cut boot ($NET_IO, atomic 1)"
   py tests/snap_cut_battery.py "$PORT" atomic_groups "$GROUP_DIR/groups.tomo" mset 5 \
-      >"/tmp/gate-snapshot-groups-mset-${PERSIST_IO}.txt" 2>&1 \
-      && grep -q "ATOMIC_GROUP_CUT PASS" "/tmp/gate-snapshot-groups-mset-${PERSIST_IO}.txt" \
-      && ok "snapshot never tears a cross-shard MSET group ($PERSIST_IO, atomic 1)" \
-      || bad "snapshot never tears a cross-shard MSET group ($PERSIST_IO, atomic 1)" \
-             "see /tmp/gate-snapshot-groups-mset-${PERSIST_IO}.txt"
+      >"/tmp/gate-snapshot-groups-mset-${NET_IO}.txt" 2>&1 \
+      && grep -q "ATOMIC_GROUP_CUT PASS" "/tmp/gate-snapshot-groups-mset-${NET_IO}.txt" \
+      && ok "snapshot never tears a cross-shard MSET group ($NET_IO, atomic 1)" \
+      || bad "snapshot never tears a cross-shard MSET group ($NET_IO, atomic 1)" \
+             "see /tmp/gate-snapshot-groups-mset-${NET_IO}.txt"
   stop
   # CONTROL ARM: the DEFAULT --atomic 0. EXEC force-admits a group at either setting, so a
   # transaction is atomic to readers here too and the file must agree with them.
-  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" --atomic 0 \
+  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" --atomic 0 \
       --enable-debug-command yes --dir "$GROUP_DIR" --dbfilename groups.tomo \
-      || bad "atomic group cut boot ($PERSIST_IO, atomic 0)"
+      || bad "atomic group cut boot ($NET_IO, atomic 0)"
   py tests/snap_cut_battery.py "$PORT" atomic_groups "$GROUP_DIR/groups.tomo" exec 3 \
-      >"/tmp/gate-snapshot-groups-exec-${PERSIST_IO}.txt" 2>&1 \
-      && grep -q "ATOMIC_GROUP_CUT PASS" "/tmp/gate-snapshot-groups-exec-${PERSIST_IO}.txt" \
-      && ok "snapshot never tears a MULTI/EXEC group ($PERSIST_IO, default atomic 0)" \
-      || bad "snapshot never tears a MULTI/EXEC group ($PERSIST_IO, default atomic 0)" \
-             "see /tmp/gate-snapshot-groups-exec-${PERSIST_IO}.txt"
+      >"/tmp/gate-snapshot-groups-exec-${NET_IO}.txt" 2>&1 \
+      && grep -q "ATOMIC_GROUP_CUT PASS" "/tmp/gate-snapshot-groups-exec-${NET_IO}.txt" \
+      && ok "snapshot never tears a MULTI/EXEC group ($NET_IO, default atomic 0)" \
+      || bad "snapshot never tears a MULTI/EXEC group ($NET_IO, default atomic 0)" \
+             "see /tmp/gate-snapshot-groups-exec-${NET_IO}.txt"
   stop
 
-  TYPED_DIR=$(mktemp -d "/tmp/gate-snapshot-typed-${PERSIST_IO}.XXXXXX")
-  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
+  TYPED_DIR=$(mktemp -d "/tmp/gate-snapshot-typed-${NET_IO}.XXXXXX")
+  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
       --dir "$TYPED_DIR" --dbfilename typed.tomo \
-      || bad "typed snapshot boot ($PERSIST_IO)"
+      || bad "typed snapshot boot ($NET_IO)"
   py tests/snap_typed_roundtrip.py "$PORT" build_save \
-      >"/tmp/gate-snapshot-typed-${PERSIST_IO}.txt" 2>&1 \
-      && ok "typed snapshot save ($PERSIST_IO)" \
-      || bad "typed snapshot save ($PERSIST_IO)" \
-             "see /tmp/gate-snapshot-typed-${PERSIST_IO}.txt"
+      >"/tmp/gate-snapshot-typed-${NET_IO}.txt" 2>&1 \
+      && ok "typed snapshot save ($NET_IO)" \
+      || bad "typed snapshot save ($NET_IO)" \
+             "see /tmp/gate-snapshot-typed-${NET_IO}.txt"
   stop
-  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
+  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
       --dir "$TYPED_DIR" --load "$TYPED_DIR/typed.tomo" \
-      || bad "typed snapshot reload boot ($PERSIST_IO)"
+      || bad "typed snapshot reload boot ($NET_IO)"
   py tests/snap_typed_roundtrip.py "$PORT" verify \
-      >>"/tmp/gate-snapshot-typed-${PERSIST_IO}.txt" 2>&1 \
-      && ok "typed snapshot reload ($PERSIST_IO)" \
-      || bad "typed snapshot reload ($PERSIST_IO)" \
-             "see /tmp/gate-snapshot-typed-${PERSIST_IO}.txt"
+      >>"/tmp/gate-snapshot-typed-${NET_IO}.txt" 2>&1 \
+      && ok "typed snapshot reload ($NET_IO)" \
+      || bad "typed snapshot reload ($NET_IO)" \
+             "see /tmp/gate-snapshot-typed-${NET_IO}.txt"
   stop
 
-  RACE_DIR=$(mktemp -d "/tmp/gate-snapshot-race-${PERSIST_IO}.XXXXXX")
-  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
+  RACE_DIR=$(mktemp -d "/tmp/gate-snapshot-race-${NET_IO}.XXXXXX")
+  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
       --dir "$RACE_DIR" --dbfilename race.tomo --save '' --enable-debug-command yes \
-      || bad "typed snapshot race boot ($PERSIST_IO)"
+      || bad "typed snapshot race boot ($NET_IO)"
   py tests/snap_typed_race.py "$PORT" race "$RACE_DIR/race.tomo" \
-      >"/tmp/gate-snapshot-race-${PERSIST_IO}.txt" 2>&1 \
-      && grep -q 'PREIMAGE-FIRED PASS' "/tmp/gate-snapshot-race-${PERSIST_IO}.txt" \
-      && grep -q 'SNAPSHOT-TICKET-ORACLE PASS' "/tmp/gate-snapshot-race-${PERSIST_IO}.txt" \
-      && ok "typed snapshot preimage race ($PERSIST_IO)" \
-      || bad "typed snapshot preimage race ($PERSIST_IO)" \
-             "see /tmp/gate-snapshot-race-${PERSIST_IO}.txt"
+      >"/tmp/gate-snapshot-race-${NET_IO}.txt" 2>&1 \
+      && grep -q 'PREIMAGE-FIRED PASS' "/tmp/gate-snapshot-race-${NET_IO}.txt" \
+      && grep -q 'SNAPSHOT-TICKET-ORACLE PASS' "/tmp/gate-snapshot-race-${NET_IO}.txt" \
+      && ok "typed snapshot preimage race ($NET_IO)" \
+      || bad "typed snapshot preimage race ($NET_IO)" \
+             "see /tmp/gate-snapshot-race-${NET_IO}.txt"
   stop
-  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
+  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
       --dir "$RACE_DIR" --load "$RACE_DIR/race.tomo.cut" \
-      || bad "typed snapshot race reload boot ($PERSIST_IO)"
+      || bad "typed snapshot race reload boot ($NET_IO)"
   py tests/snap_typed_race.py "$PORT" verify "$RACE_DIR/race.tomo.oracle.json" \
-      >>"/tmp/gate-snapshot-race-${PERSIST_IO}.txt" 2>&1 \
-      && ok "typed snapshot race reload ($PERSIST_IO)" \
-      || bad "typed snapshot race reload ($PERSIST_IO)" \
-             "see /tmp/gate-snapshot-race-${PERSIST_IO}.txt"
+      >>"/tmp/gate-snapshot-race-${NET_IO}.txt" 2>&1 \
+      && ok "typed snapshot race reload ($NET_IO)" \
+      || bad "typed snapshot race reload ($NET_IO)" \
+             "see /tmp/gate-snapshot-race-${NET_IO}.txt"
   stop
 done
 
@@ -875,10 +862,10 @@ py tests/spinprobe.py $PORT "$SRV" >/tmp/gate-spinprobe.txt 2>&1 \
     && ok "partial-frame conn parks (no io spin)" || bad "partial-frame conn parks (no io spin)" "see /tmp/gate-spinprobe.txt"
 stop
 boot ./build/tomokv --ratio 6:2 --atomic 0 --enable-debug-command yes --flip-auto 1 \
-     --flip-auto-band 2 --lb-age-sample-rate 1024 || bad "flipctl boot"
+     || bad "flipctl boot"
 # 300 -> 600 s: the stable-hold row now MEASURES its precondition (the driver's own command rate
 # must be stationary by the controller's own band) before its assertion window opens, and re-rolls
-# a hold whose load left that band up to three times inside its own wall budget before skipping it.
+# a hold whose load left that band up to three times inside its own wall budget before failing it.
 # The typical run is unchanged; the worst case adds the re-rolls plus their re-anchor waits.
 timeout 600 python3 tests/flipctl.py --host 127.0.0.1 --port $PORT --stable-seconds 30 \
     >/tmp/gate-flipctl.txt 2>&1 \
@@ -966,26 +953,24 @@ timeout 120 python3 tests/pipeorder.py $PORT 400 >/tmp/gate-pipeorder.txt 2>&1 \
 stop
 
 # ---- AOF boot/replay + non-vacuous DEBUG LOADAOF ---------------------------------------------
-for PERSIST_IO in normal uring; do
+for NET_IO in epoll uring; do
 for AOF_ATOMIC in 0 1; do
-AOF_DIR=$(mktemp -d "/tmp/gate-aof-${PERSIST_IO}-atomic${AOF_ATOMIC}.XXXXXX")
+AOF_DIR=$(mktemp -d "/tmp/gate-aof-${NET_IO}-atomic${AOF_ATOMIC}.XXXXXX")
 AOF_STATE=$AOF_DIR/state.json
 boot ./build/tomokv --protected-mode no --atomic "$AOF_ATOMIC" \
     --appendonly yes --appendfsync no \
-    --persist-io "$PERSIST_IO" --enable-debug-command yes --dir "$AOF_DIR" \
-    || bad "AOF purpose boot ($PERSIST_IO, atomic $AOF_ATOMIC)"
-PERSIST_GET=$(redis-cli -h 127.0.0.1 -p $PORT --raw CONFIG GET persist-io 2>/dev/null |
-    tail -1)
-PERSIST_SET=$(redis-cli -h 127.0.0.1 -p $PORT CONFIG SET persist-io "$PERSIST_IO" 2>&1)
-[ "$PERSIST_GET" = "$PERSIST_IO" ] && printf '%s' "$PERSIST_SET" | grep -q immutable \
-    && ok "persist-io surface + immutable ($PERSIST_IO)" \
-    || bad "persist-io surface + immutable ($PERSIST_IO)"
-py tests/aof.py 127.0.0.1 $PORT populate "$AOF_STATE" >/tmp/gate-aof-$PERSIST_IO-$AOF_ATOMIC.txt 2>&1 \
-    && py tests/aof.py 127.0.0.1 $PORT loadaof "$AOF_STATE" >>/tmp/gate-aof-$PERSIST_IO-$AOF_ATOMIC.txt 2>&1 \
-    && ok "AOF byte-exact + script groups + DEBUG LOADAOF ($PERSIST_IO, atomic $AOF_ATOMIC)" \
-    || bad "AOF byte-exact + script groups + DEBUG LOADAOF ($PERSIST_IO, atomic $AOF_ATOMIC)" \
-           "see /tmp/gate-aof-$PERSIST_IO-$AOF_ATOMIC.txt"
-AOF_PRE_MODEL=$(py tests/aof.py 127.0.0.1 $PORT snapshot "$AOF_DIR/dump.tomo" 2>>/tmp/gate-aof-$PERSIST_IO-$AOF_ATOMIC.txt)
+    --net-io "$NET_IO" --enable-debug-command yes --dir "$AOF_DIR" \
+    || bad "AOF purpose boot ($NET_IO, atomic $AOF_ATOMIC)"
+py tests/knobs.py 127.0.0.1 "$PORT" "$NET_IO" "$AOF_ATOMIC" \
+    >"/tmp/gate-knobs-${NET_IO}-${AOF_ATOMIC}.txt" 2>&1 \
+    && ok "configuration reduction + actual geometry ($NET_IO, atomic $AOF_ATOMIC)" \
+    || bad "configuration reduction + actual geometry ($NET_IO, atomic $AOF_ATOMIC)"
+py tests/aof.py 127.0.0.1 $PORT populate "$AOF_STATE" >/tmp/gate-aof-$NET_IO-$AOF_ATOMIC.txt 2>&1 \
+    && py tests/aof.py 127.0.0.1 $PORT loadaof "$AOF_STATE" >>/tmp/gate-aof-$NET_IO-$AOF_ATOMIC.txt 2>&1 \
+    && ok "AOF byte-exact + script groups + DEBUG LOADAOF ($NET_IO, atomic $AOF_ATOMIC)" \
+    || bad "AOF byte-exact + script groups + DEBUG LOADAOF ($NET_IO, atomic $AOF_ATOMIC)" \
+           "see /tmp/gate-aof-$NET_IO-$AOF_ATOMIC.txt"
+AOF_PRE_MODEL=$(py tests/aof.py 127.0.0.1 $PORT snapshot "$AOF_DIR/dump.tomo" 2>>/tmp/gate-aof-$NET_IO-$AOF_ATOMIC.txt)
 AOF_WRITTEN=$(redis-cli -h 127.0.0.1 -p $PORT INFO Persistence 2>/dev/null \
     | tr -d '\r' | sed -n 's/^aof_records_written://p')
 [ -n "$AOF_WRITTEN" ] && [ "$AOF_WRITTEN" -gt 0 ] \
@@ -994,14 +979,14 @@ kill -KILL $SRV 2>/dev/null
 wait $SRV 2>/dev/null
 settle
 boot ./build/tomokv --protected-mode no --appendonly yes --appendfsync no \
-    --atomic "$AOF_ATOMIC" --persist-io "$PERSIST_IO" \
+    --atomic "$AOF_ATOMIC" --net-io "$NET_IO" \
     --enable-debug-command yes --dir "$AOF_DIR" \
-    || bad "AOF replay boot ($PERSIST_IO, atomic $AOF_ATOMIC)"
-py tests/aof.py 127.0.0.1 $PORT verify "$AOF_STATE" >>/tmp/gate-aof-$PERSIST_IO-$AOF_ATOMIC.txt 2>&1 \
-    && ok "AOF process-restart script replay ($PERSIST_IO, atomic $AOF_ATOMIC)" \
-    || bad "AOF process-restart script replay ($PERSIST_IO, atomic $AOF_ATOMIC)" \
-           "see /tmp/gate-aof-$PERSIST_IO-$AOF_ATOMIC.txt"
-AOF_POST_MODEL=$(py tests/aof.py 127.0.0.1 $PORT snapshot "$AOF_DIR/dump.tomo" 2>>/tmp/gate-aof-$PERSIST_IO-$AOF_ATOMIC.txt)
+    || bad "AOF replay boot ($NET_IO, atomic $AOF_ATOMIC)"
+py tests/aof.py 127.0.0.1 $PORT verify "$AOF_STATE" >>/tmp/gate-aof-$NET_IO-$AOF_ATOMIC.txt 2>&1 \
+    && ok "AOF process-restart script replay ($NET_IO, atomic $AOF_ATOMIC)" \
+    || bad "AOF process-restart script replay ($NET_IO, atomic $AOF_ATOMIC)" \
+           "see /tmp/gate-aof-$NET_IO-$AOF_ATOMIC.txt"
+AOF_POST_MODEL=$(py tests/aof.py 127.0.0.1 $PORT snapshot "$AOF_DIR/dump.tomo" 2>>/tmp/gate-aof-$NET_IO-$AOF_ATOMIC.txt)
 [ -n "$AOF_PRE_MODEL" ] && [ "$AOF_PRE_MODEL" = "$AOF_POST_MODEL" ] \
     && ok "AOF native snapshot streams byte-exact" || bad "AOF native snapshot streams byte-exact"
 AOF_REPLAYED=$(redis-cli -h 127.0.0.1 -p $PORT INFO Persistence 2>/dev/null \
@@ -1014,11 +999,11 @@ stop
 done
 
 # ---- AOF atomic-group bracketing + directed interrupted-process recovery ---------------------
-AOF_GROUP_DIR=$(mktemp -d "/tmp/gate-aof-group-${PERSIST_IO}.XXXXXX")
+AOF_GROUP_DIR=$(mktemp -d "/tmp/gate-aof-group-${NET_IO}.XXXXXX")
 AOF_GROUP_STATE=$AOF_GROUP_DIR/state.json
 boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync no \
-    --persist-io "$PERSIST_IO" --enable-debug-command yes --dir "$AOF_GROUP_DIR" \
-    || bad "AOF group purpose boot ($PERSIST_IO)"
+    --net-io "$NET_IO" --enable-debug-command yes --dir "$AOF_GROUP_DIR" \
+    || bad "AOF group purpose boot ($NET_IO)"
 py tests/aof_torn_group.py 127.0.0.1 $PORT prepare "$AOF_GROUP_STATE" \
     >/tmp/gate-aof-group.txt 2>&1 \
     && ok "AOF directed group interruption fired" \
@@ -1026,8 +1011,8 @@ py tests/aof_torn_group.py 127.0.0.1 $PORT prepare "$AOF_GROUP_STATE" \
 wait $SRV 2>/dev/null
 settle
 boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync no \
-    --persist-io "$PERSIST_IO" --enable-debug-command yes --dir "$AOF_GROUP_DIR" \
-    || bad "AOF group recovery boot ($PERSIST_IO)"
+    --net-io "$NET_IO" --enable-debug-command yes --dir "$AOF_GROUP_DIR" \
+    || bad "AOF group recovery boot ($NET_IO)"
 py tests/aof_torn_group.py 127.0.0.1 $PORT verify "$AOF_GROUP_STATE" \
     >>/tmp/gate-aof-group.txt 2>&1 \
     && py tests/aof_torn_group.py 127.0.0.1 $PORT scan \
@@ -1040,11 +1025,11 @@ shutdown_clean \
     && ok "AOF group shutdown invariants" || bad "AOF group shutdown invariants"
 
 # ---- AOF sync policies, reply gate, idle sync, and durability-window recovery ----------------
-AOF_ALWAYS_DIR=$(mktemp -d "/tmp/gate-aof-always-${PERSIST_IO}.XXXXXX")
+AOF_ALWAYS_DIR=$(mktemp -d "/tmp/gate-aof-always-${NET_IO}.XXXXXX")
 AOF_ALWAYS_STATE=$AOF_ALWAYS_DIR/state.json
 boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync always \
-    --persist-io "$PERSIST_IO" --dir "$AOF_ALWAYS_DIR" \
-    || bad "AOF always purpose boot ($PERSIST_IO)"
+    --net-io "$NET_IO" --dir "$AOF_ALWAYS_DIR" \
+    || bad "AOF always purpose boot ($NET_IO)"
 py tests/aof_fsync.py 127.0.0.1 $PORT populate "$AOF_ALWAYS_STATE" always 512 \
     >/tmp/gate-aof-always.txt 2>&1 \
     && ok "AOF always sync + reply gate fired" \
@@ -1053,20 +1038,20 @@ kill -KILL $SRV 2>/dev/null
 wait $SRV 2>/dev/null
 settle
 boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync always \
-    --persist-io "$PERSIST_IO" --dir "$AOF_ALWAYS_DIR" \
-    || bad "AOF always recovery boot ($PERSIST_IO)"
+    --net-io "$NET_IO" --dir "$AOF_ALWAYS_DIR" \
+    || bad "AOF always recovery boot ($NET_IO)"
 py tests/aof_fsync.py 127.0.0.1 $PORT verify "$AOF_ALWAYS_STATE" always 512 \
     >>/tmp/gate-aof-always.txt 2>&1 \
     && ok "AOF always acknowledged-prefix recovery" \
     || bad "AOF always acknowledged-prefix recovery" "see /tmp/gate-aof-always.txt"
 stop
 
-AOF_EVERY_DIR=$(mktemp -d "/tmp/gate-aof-everysec-${PERSIST_IO}.XXXXXX")
+AOF_EVERY_DIR=$(mktemp -d "/tmp/gate-aof-everysec-${NET_IO}.XXXXXX")
 AOF_EVERY_STATE=$AOF_EVERY_DIR/state.json
 AOF_EVERY_FILE=$AOF_EVERY_DIR/appendonlydir/appendonly.aof.1.incr.tomo
 boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync everysec \
-    --persist-io "$PERSIST_IO" --dir "$AOF_EVERY_DIR" \
-    || bad "AOF everysec purpose boot ($PERSIST_IO)"
+    --net-io "$NET_IO" --dir "$AOF_EVERY_DIR" \
+    || bad "AOF everysec purpose boot ($NET_IO)"
 py tests/aof_fsync.py 127.0.0.1 $PORT populate "$AOF_EVERY_STATE" everysec 512 \
     >/tmp/gate-aof-everysec.txt 2>&1 \
     && ok "AOF everysec write gate + idle sync fired" \
@@ -1081,8 +1066,8 @@ case "$AOF_EVERY_FILE" in
 esac
 settle
 boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync everysec \
-    --persist-io "$PERSIST_IO" --dir "$AOF_EVERY_DIR" \
-    || bad "AOF everysec recovery boot ($PERSIST_IO)"
+    --net-io "$NET_IO" --dir "$AOF_EVERY_DIR" \
+    || bad "AOF everysec recovery boot ($NET_IO)"
 py tests/aof_fsync.py 127.0.0.1 $PORT verify "$AOF_EVERY_STATE" everysec 512 \
     >>/tmp/gate-aof-everysec.txt 2>&1 \
     && grep -q "AOF warning: truncated AOF tail" "$SRVLOG" \
@@ -1090,10 +1075,10 @@ py tests/aof_fsync.py 127.0.0.1 $PORT verify "$AOF_EVERY_STATE" everysec 512 \
     || bad "AOF everysec durability window + tail warning" "see /tmp/gate-aof-everysec.txt"
 stop
 
-AOF_NO_DIR=$(mktemp -d "/tmp/gate-aof-no-sync-${PERSIST_IO}.XXXXXX")
+AOF_NO_DIR=$(mktemp -d "/tmp/gate-aof-no-sync-${NET_IO}.XXXXXX")
 boot ./build/tomokv --protected-mode no --atomic 0 --appendonly yes --appendfsync no \
-    --persist-io "$PERSIST_IO" --dir "$AOF_NO_DIR" \
-    || bad "AOF no-sync purpose boot ($PERSIST_IO)"
+    --net-io "$NET_IO" --dir "$AOF_NO_DIR" \
+    || bad "AOF no-sync purpose boot ($NET_IO)"
 py tests/aof_fsync.py 127.0.0.1 $PORT populate "$AOF_NO_DIR/state.json" no 128 \
     >/tmp/gate-aof-no-sync.txt 2>&1 \
     && ok "AOF no-sync bypassed sync + reply gate" \
@@ -1101,19 +1086,19 @@ py tests/aof_fsync.py 127.0.0.1 $PORT populate "$AOF_NO_DIR/state.json" no 128 \
 stop
 
 quiet_wait
-PERSIST_IO=$PERSIST_IO GATE_PORT=$PORT GATE_CORES=$CORES tests/aof_rewrite_matrix.sh \
+NET_IO=$NET_IO GATE_PORT=$PORT GATE_CORES=$CORES tests/aof_rewrite_matrix.sh \
     >/tmp/gate-aof-rewrite.txt 2>&1 \
     && ok "AOF rewrite atomic/stage/corruption matrix" \
     || bad "AOF rewrite matrix" "see /tmp/gate-aof-rewrite.txt"
 
 quiet_wait
-PERSIST_IO=$PERSIST_IO GATE_PORT=$PORT GATE_CORES=$CORES tests/aof_rewrite_trigger_matrix.sh \
+NET_IO=$NET_IO GATE_PORT=$PORT GATE_CORES=$CORES tests/aof_rewrite_trigger_matrix.sh \
     >/tmp/gate-aof-rewrite-trigger.txt 2>&1 \
     && ok "AOF rewrite triggers + observability matrix" \
     || bad "AOF rewrite triggers" "see /tmp/gate-aof-rewrite-trigger.txt"
 
-AOF_OFF_DIR=$(mktemp -d "/tmp/gate-aof-off-${PERSIST_IO}.XXXXXX")
-boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
+AOF_OFF_DIR=$(mktemp -d "/tmp/gate-aof-off-${NET_IO}.XXXXXX")
+boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
     --appendonly no --dir "$AOF_OFF_DIR" \
     || bad "AOF-off negative-control boot"
 AOF_OFF_SEED=$(redis-cli -h 127.0.0.1 -p $PORT SET aof-negative-control must-disappear 2>&1 |
@@ -1125,7 +1110,7 @@ AOF_OFF_PRE_SIZE=$(redis-cli -h 127.0.0.1 -p $PORT DBSIZE 2>/dev/null | tr -d '\
 kill -KILL $SRV 2>/dev/null
 wait $SRV 2>/dev/null
 settle
-boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
+boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
     --appendonly no --dir "$AOF_OFF_DIR" \
     || bad "AOF-off negative-control reboot"
 AOF_OFF_SIZE=$(redis-cli -h 127.0.0.1 -p $PORT DBSIZE 2>/dev/null | tr -d '\r')
@@ -1139,13 +1124,13 @@ done
 # writer flushed a ready GCMT at the top of a writer pass without checking that a large record
 # still held the physical stream. Recovery truncates the file from that large record's first byte,
 # so a control frame inside it is discardable -- and the loader refuses to start on the whole file.
-# persist-io normal only: that is where the defect was demonstrated (11 of 114 runs of the AOF
+# Syscall persistence under epoll: that is where the defect was demonstrated (11 of 114 runs of the AOF
 # battery, 0 of 117 on uring) and where the window is entered reliably enough for the row to prove
 # its mechanism fired. The battery FAILS on a build with the guard removed (6 of 6).
 for AOF_FRAME_ATOMIC in 0 1; do
 AOF_FRAME_DIR=$(mktemp -d "/tmp/gate-aof-frameorder-atomic${AOF_FRAME_ATOMIC}.XXXXXX")
 boot ./build/tomokv --protected-mode no --atomic "$AOF_FRAME_ATOMIC" --appendonly yes \
-    --appendfsync no --persist-io normal --auto-aof-rewrite-percentage 0 \
+    --appendfsync no --net-io epoll --auto-aof-rewrite-percentage 0 \
     --enable-debug-command yes --dir "$AOF_FRAME_DIR" \
     || bad "AOF frame-order purpose boot (atomic $AOF_FRAME_ATOMIC)"
 py tests/aof_frame_order.py 127.0.0.1 $PORT "$AOF_FRAME_DIR/appendonlydir" \
@@ -1212,7 +1197,7 @@ py tests/tls.py 127.0.0.1 "$TLS_PORT" "$TLS_DIR" optional --plain-port "$PORT" \
     >/tmp/gate-tls-optional.txt 2>&1 \
     && ok "TLS client-auth optional matrix" \
     || bad "TLS client-auth optional matrix" "see /tmp/gate-tls-optional.txt"
-# Live kTLS engagement proof on the default (tls-ktls yes) boot: a plain TLS client connects and
+# Live kTLS engagement proof on the default boot: a plain TLS client connects and
 # must see itself counted in the active gauge. Client-auth 'optional' permits a cert-less client.
 python3 - "$TLS_PORT" "$TLS_DIR" <<'PYEOF' >/tmp/gate-ktls-live.txt 2>&1 \
     && ok "kTLS engaged live (default boot)" || bad "kTLS engaged live" "see /tmp/gate-ktls-live.txt"
@@ -1232,9 +1217,9 @@ stop
 shutdown_clean \
     && ok "TLS optional shutdown invariants" || bad "TLS optional shutdown invariants"
 
-# The full battery runs FORCED-FALLBACK so the userspace arm's mechanisms (zc suppression, memory
-# BIO paths) keep their non-vacuous proofs; the default-ktls arm is proven separately below.
-tlsboot no --tls-ktls no --tls-protocols "TLSv1.2 TLSv1.3" --tls-ciphers DEFAULT \
+# Exercise userspace fallback through the retained cipher grammar. TLS 1.2 uses CBC; TLS 1.3
+# uses AES-256, outside this implementation's AES-128 TLS 1.3 RX installer. Counters must fire.
+tlsboot no --tls-protocols "TLSv1.2 TLSv1.3" --tls-ciphers ECDHE-RSA-AES256-SHA384 \
     --tls-ciphersuites TLS_AES_256_GCM_SHA384 --tls-prefer-server-ciphers yes \
     || bad "TLS coexistence purpose boot"
 py tests/tls.py 127.0.0.1 "$TLS_PORT" "$TLS_DIR" no --plain-port "$PORT" --full \
@@ -1272,7 +1257,7 @@ py tests/atomic_torn.py 127.0.0.1 $PORT >/tmp/gate-atomic-torn-asan.txt 2>&1 \
 # atomic groups beats 24 serial round trips by >10%). ASAN does not slow the two arms by the same
 # factor, so that ratio inverts here on a correct build -- measured pipe 21,005/s vs serial
 # 24,202/s in a full-gate run whose every correctness check passed, on a build that passed 4 of 4
-# standalone. The mechanism half of the same section (all 24 admitted, atomic_window_stalls moved,
+# standalone. The mechanism half of the same section (all 24 admitted, multiple groups observed in flight,
 # so a younger cross-key group WAS in flight while an older one decided) still runs on this tier,
 # and the measured rates are still printed in the log.
 py tests/atomic_ryow.py 127.0.0.1 $PORT --no-rate-assertions \
@@ -1345,20 +1330,9 @@ pausable g++ -std=c++20 -O2 -g -march=native -pthread -DTOMO_JEMALLOC -DTOMO_RL_
     && ok "read-local ownership-invariant build" \
     || bad "read-local ownership-invariant build" "see /tmp/gate-rlcachedbg-build.txt"
 quiet_wait
-# --shards 64 and a faster balancer tick are what make the row DETERMINISTIC rather than a coin
-# flip. The defect needs a shard to CHANGE OWNER; `launch` boots --shards 16 and this gate sizes its
-# thread count from its core set, so on a 16-CPU set every thread owns exactly one shard and the
-# balancer has nothing it can take (measured: 9.4M ops, 0 moves -- and the battery's own
-# non-vacuity check correctly refused to report a pass). A later --shards wins over the earlier
-# one, so 64 shards gives every thread several to trade whatever the core count is. The LB knobs
-# are boot-only (CONFIG SET reports "parameter is immutable at runtime"), hence here.
-#
-# ONLY the tick is lowered. Measured over 15 s at 64 shards, moves: default 6, --lb-tick-ms 100
-# -> 50, --lb-move-cap 4 -> 20, --lb-imbalance-pct 1 -> 5, and --lb-cooldown-ms 0 -> ZERO. That
-# last one reads like "no cooldown" and means "never move", which is how the first version of this
-# row silently lost its own precondition. Do not add it back.
-if boot_fused $RLDBG --shards 64 --atomic 1 --read-local 1 --enable-debug-command yes \
-        --lb-tick-ms 100; then
+# More shards than workers permit ownership movement. The automatic balancer must move shards;
+# the battery's existing positive-movement assertion remains mandatory under the derived policy.
+if boot_fused $RLDBG --shards 64 --atomic 1 --read-local 1 --enable-debug-command yes; then
   # The battery carries its own three non-vacuity checks: the armed lane served reads, the block
   # cache actually held blocks, and -- the precondition for this whole class of defect -- the load
   # balancer MOVED shards during the run.

exec
/bin/bash -lc 'git diff -- DESIGN-GATEHYGIENE.md tests/atomic_torn.py tests/borrow_registry.py tests/differ.py' in /home/user/Projects/cx-timing
 succeeded in 0ms:
diff --git a/DESIGN-GATEHYGIENE.md b/DESIGN-GATEHYGIENE.md
index 1ad3bfc4c..5ea9df93e 100644
--- a/DESIGN-GATEHYGIENE.md
+++ b/DESIGN-GATEHYGIENE.md
@@ -456,3 +456,60 @@ stable hold: 30s on a measured-stationary load, no trigger or split movement (at
 
 `flip.py` needed no in-flight re-roll in any post-fix run (0 re-roll lines), so the budget is
 headroom rather than a routine cost. The flipctl row costs 120 s, unchanged from before this lane.
+
+---
+
+## 10. Follow-up diff on `78c3e5391` — 2026-09-07, runtime validation pending
+
+This is a test-only diff in `cx-timing`. No server, benchmark, gate, or compiler was run. The
+earlier acceptance results above describe earlier changes, **not this diff**. The worked fixes
+in `multirace.py` and the RENAME OFF control are unchanged.
+
+| area | change | failure induction (also documented beside the checks) |
+| --- | --- | --- |
+| `differ.py`, blocking | Wait for both blocked-client gauges to reach zero before final comparison. Assert each server's gauge increment independently, against a settled baseline. | Suppress registration increments to fail movement; suppress disconnect decrements to fail the bounded zero control. |
+| `atomic_torn.py`, promotion/leases | Keep promotion movement and credit safety mandatory. Separate the original 1.5s promotion and 3s shrink/reclaim budgets behind `--release-build`. Sample the credit bound across at least 32 observations with completed writer traffic, and require zero inflight/debt plus the exact final credit pool. | Disable promotion or lease returns; over-issue credits after shrink. An observed excess or exhausted mechanism watchdog fails on every build. |
+| `slowlog.py` | Derive recording-thread capacity from INFO (`fused_threads`, or `io_threads + ex_threads`), and issue more writes than its aggregate capacity. | Disable ring eviction or max-len trimming. |
+| `xshard_dispatch_scale.sh` / `.py` | Disable balancing during the fixed-owner experiment; verify ownership remained fixed. Use distinct reply payloads, check every pair's topology, alternate boot order, and retain every ratio. Only score release timings when repeated arms and same-shard controls agree within the supplied 2% box contract. Every qualified pair must meet the unchanged 1.20 bound; no best-pair selection. | Misroute a key or change a second pair's fan-out to fail mechanism checks; restore the all-thread scan to fail a stable release measurement. Unqualified timings say SKIP with all numbers, not that scaling passed. |
+| `pubsub.py` | Drain old pattern registrations before checking the next NUMPAT population; keep exact live-pattern assertions. | Leak the disconnected RESP3 subscriber's pattern; a constant-zero NUMPAT still fails the live arms. |
+| `borrow_registry.py` | Replace the 1.5x timing proxy for different paths with `zc_sends`/`zc_releases` deltas and a plain-GET negative control before holders exist. Keep parked-borrow/drain and byte-exact reply checks on every build; cost budgets require `--release-build`. | Disable borrowing, borrow below the cutover, or omit releases to fail mechanisms; restore the linear lookup to fail the qualified release growth budget. |
+| `expireindex.py` | Assert exact 2 -> 1 -> 0 TTL accounting, DEL replies, and the surviving value on every repetition. Keep INFO outside both timed DEL intervals. Cost ratios require `--release-build`. | Break expiry unregistration or delete accounting to fail mechanisms; restore the historical-capacity memset to fail the release timing bound. |
+| `infofix.py` and its differential counterpart | Poll INFO-only idle samples until zero; feed verified PINGs until the sampler reports work. Also require RESETSTAT to clear those PING counters, so natural rate decay cannot substitute for a reset. | Pin the rate at zero/nonzero, or omit the command-counter reset baseline. |
+| `stream.py` | Check exact retained entries through trimming, first-entry creation and embedded-to-macro growth on every build. Report all footprint measurements; score the original byte ceilings only with `--release-build`. Require positive measured allocation instead of clamping negative deltas to zero. | Drop/wrongly trim entries or omit memory accounting to fail mechanisms; inflate first-entry/node allocations to fail release footprint bounds. |
+
+The release gate explicitly passes `--release-build` to the affected batteries. The ASAN atomic
+row omits it. Standalone runs against an instrumented binary should likewise omit it; all
+mechanism assertions remain active and performance measurements still appear in the log.
+
+### Outstanding atomic window calibration — not claimed fixed
+
+Three window-hit checks remain strict at their existing arming geometry:
+
+* `atomic_torn.py:718`, commit-delay held a read cut;
+* `atomic_torn.py:719`, predecessor resolution;
+* `atomic_torn.py:923`, admission stalls and resumes.
+
+Their own per-attempt hit/miss measurements were not supplied. The RENAME control's measured
+5/6 hit rate does not establish a bound for these different mechanisms. No fresh-connection
+retry count was guessed, no window-hit assertion was made release-only, and their observation
+windows were not widened. Fresh-state retry hygiene for these three checks still needs the
+maintainer's measurements at `--shards 16 --ratio 6:2`, cores `0-7`, separately for release and
+ASAN. Record each attempt's counter deltas and any real value/worker failure; such failures must
+stop retrying immediately. This is the unfinished part of item 1, not an acceptance claim.
+
+### Static and offline checks
+
+Python AST parsing, `bash -n` and `git diff --check` passed. Source-extracted helper checks with
+synthetic inputs covered successful and exhausted gauge/pattern polls, 8/16-thread INFO
+geometries, release-flag behavior, exact TTL transitions, stuck sampled rates, and excess/no-work
+lease samples. The dispatch result evaluator rejected a synthetic stable 1.30 ratio, incorrect
+second-pair geometry, negative excess and a missing pair, while explicitly skipping unstable
+timings. These are harness checks, **not** server fault-injection or PRE/POST measurements.
+
+### Ledger
+
+No top-level row was added or retired; the `ok`/`bad` label sequence is unchanged. In this diff
+the quick exit is at `gate.sh:1262`. The release atomic row (`:434`), stream feature-loop calls
+(`:457`, `:516`), expiry (`:674`), borrow (`:682`) and dispatch (`:688`) rows are before it; the
+ASAN atomic row (`:1275`) is after it. Internal assertions do not add ledger rows.
+`EXPECT_QUICK=327` and `EXPECT_FULL=344` remain unchanged, and no count adjustment is needed.
diff --git a/tests/atomic_torn.py b/tests/atomic_torn.py
index 448e00be6..7c1fb361e 100755
--- a/tests/atomic_torn.py
+++ b/tests/atomic_torn.py
@@ -10,9 +10,17 @@ import time
 import _lib
 
 
-HOST, PORT = sys.argv[1], int(sys.argv[2])
+# Build identity comes from the caller. Counter movement, exact values, and safety bounds are
+# mandatory on every build; only the time spent reaching them is a release-build assertion.
+ARGS = sys.argv[1:]
+RELEASE_BUILD = "--release-build" in ARGS
+ARGS = [arg for arg in ARGS if arg != "--release-build"]
+if len(ARGS) != 2:
+    raise SystemExit("usage: atomic_torn.py HOST PORT [--release-build]")
+HOST, PORT = ARGS[0], int(ARGS[1])
 FAIL = 0
 BARRIER_TIMEOUT = 10
+MECHANISM_TIMEOUT = 30.0  # liveness watchdog, never a successful/skip outcome on exhaustion
 
 
 def note(name, ok, extra=""):
@@ -26,6 +34,13 @@ def skip(name, extra=""):
     print("  SKIP " + name + (" " + extra if extra else ""), flush=True)
 
 
+def release_note(name, ok, measured):
+    if RELEASE_BUILD:
+        note(name, ok, measured)
+    else:
+        skip(name, "requires --release-build; " + measured)
+
+
 def abort_barriers(*barriers):
     for barrier in barriers:
         try:
@@ -669,6 +684,7 @@ note("OFF control exposes torn MSET-8",
 before = stats()
 required_stat(before, "atomic_predecessor_reads")
 required_stat(before, "atomic_commit_holds")
+required_stat(before, "atomic_promotions")
 debug("ATOMIC-COMMIT-DELAY", 2000)
 try:
     on_torn, on_reads, on_errors, _, _, on_threads_still_alive = hammer(
@@ -680,21 +696,32 @@ pred_delta = (required_stat(after, "atomic_predecessor_reads") -
               required_stat(before, "atomic_predecessor_reads"))
 hold_delta = (required_stat(after, "atomic_commit_holds") -
               required_stat(before, "atomic_commit_holds"))
-promo_delta = after.get("atomic_promotions", 0) - before.get("atomic_promotions", 0)
+promo_delta = required_stat(after, "atomic_promotions") - required_stat(before, "atomic_promotions")
 # V2 batches reclamation on owner passes and the 50ms low-frequency sweep instead of posting a
-# cleanup task for every retired group. Give that deliberately cold path one bounded tick to fire.
-deadline = time.time() + 1.5
-while promo_delta == 0 and time.time() < deadline:
+# cleanup task for every retired group. Poll completed work's cleanup with a watchdog; only the
+# original 1.5s completion budget is release-specific. This does not re-arm any race window.
+promotion_started = time.monotonic()
+deadline = promotion_started + MECHANISM_TIMEOUT
+while promo_delta == 0 and time.monotonic() < deadline and not on_errors and not on_torn:
     time.sleep(0.05)
     after = stats()
-    promo_delta = after.get("atomic_promotions", 0) - before.get("atomic_promotions", 0)
+    promo_delta = required_stat(after, "atomic_promotions") - required_stat(before, "atomic_promotions")
+promotion_elapsed = time.monotonic() - promotion_started
 note("ON MSET-8/MGET-8 torn-free",
      on_torn == 0 and on_reads > 0 and not on_errors and not on_threads_still_alive,
      "torn=%d reads=%d errors=%r threads_still_alive=%r" %
      (on_torn, on_reads, on_errors, on_threads_still_alive))
+# These are window-hit claims, NEVER release-only or skip-on-miss. Induce respectively by
+# removing safe-cut holds, bypassing predecessor lookup, or disabling promotion. Each still
+# fails on ASAN with a zero delta. Fresh-connection retry bounds for the first two need their own
+# measured hit rates; the RENAME control's 5/6 measurement cannot calibrate a different window.
 note("ON commit-delay window held a read cut", hold_delta > 0, "delta=%d" % hold_delta)
 note("ON exercised predecessor resolution", pred_delta > 0, "delta=%d" % pred_delta)
 note("ON exercised promotion", promo_delta > 0, "delta=%d" % promo_delta)
+release_note("ON promotion drains within release budget",
+             promo_delta > 0 and promotion_elapsed <= 1.5,
+             "promotion-drain=%.3fs holds=%d predecessors=%d promotions=%d" %
+             (promotion_elapsed, hold_delta, pred_delta, promo_delta))
 note("atomic_inflight returns to idle", after.get("atomic_inflight", -1) == 0,
      "value=%d" % after.get("atomic_inflight", -1))
 
@@ -870,7 +897,7 @@ else:
 # deterministic instead of depending on Python threads winning a scheduling race.
 config("atomic", 1)
 config("atomic-window", 1)
-window_before = stats().get("atomic_window_stalls", 0)
+window_before = required_stat(stats(), "atomic_window_stalls")
 window_run = format(time.time_ns() & 0xfffffff, "x")
 window_errors = []
 window_client = Resp()
@@ -890,7 +917,9 @@ except Exception as exc:
     window_errors.append(str(exc))
 finally:
     window_client.close()
-window_after = stats().get("atomic_window_stalls", 0)
+window_after = required_stat(stats(), "atomic_window_stalls")
+# A miss remains a failure on every build. Induce by bypassing the admission credit check;
+# replies alone cannot prove backpressure. No fresh-state retries without measured hit rates.
 note("atomic-window stalls and resumes",
      not window_errors and window_after > window_before,
      "stalls=%d errors=%r" % (window_after - window_before, window_errors))
@@ -902,6 +931,7 @@ config("atomic-window", 256)
 # have returned every leased credit to the pool so a skewed next IO can consume the whole window.
 lease_stop = threading.Event()
 lease_errors = []
+lease_completed = [0] * 8
 
 
 def lease_writer(wid):
@@ -914,6 +944,7 @@ def lease_writer(wid):
             if mset(client, keys, "lease:%d:%d" % (wid, seq)) != b"OK":
                 raise AssertionError("bad lease reply")
             seq += 1
+            lease_completed[wid] = seq
     except Exception as exc:
         lease_errors.append("writer%d:%s" % (wid, exc))
     finally:
@@ -927,10 +958,11 @@ for thread in lease_threads:
 for value in (31, 7, 19, 3):
     if not config("atomic-window", value):
         lease_errors.append("CONFIG atomic-window %d failed" % value)
-deadline = time.time() + 3
+shrink_started = time.monotonic()
+deadline = shrink_started + MECHANISM_TIMEOUT
 bounded = False
 max_inflight = 0
-while time.time() < deadline:
+while time.monotonic() < deadline:
     sample = stats()
     live = sample.get("atomic_inflight", 1000000)
     debt = sample.get("atomic_credit_debt", 1000000)
@@ -938,30 +970,52 @@ while time.time() < deadline:
         bounded = True
         break
     time.sleep(0.01)
+shrink_elapsed = time.monotonic() - shrink_started
+bound_samples = 0
+bound_completed_before = sum(lease_completed)
+bound_completed_after = bound_completed_before
 if bounded:
-    deadline = time.time() + 0.4
-    while time.time() < deadline:
-        live = stats().get("atomic_inflight", 1000000)
+    # Assert values across completed observations, not the number of polls a 0.4s slice happened
+    # to fit. Traffic must complete during this interval, or the bound could pass on idle zeroes.
+    # Induce by over-issuing credits after shrink: ANY live > 3 fails, on release and ASAN alike.
+    deadline = time.monotonic() + MECHANISM_TIMEOUT
+    while time.monotonic() < deadline and not lease_errors:
+        sample = stats()
+        live = required_stat(sample, "atomic_inflight")
+        bound_completed_after = sum(lease_completed)
+        bound_samples += 1
         max_inflight = max(max_inflight, live)
         if live > 3:
             bounded = False
             break
+        if bound_samples >= 32 and bound_completed_after > bound_completed_before:
+            break
+    bounded = bounded and bound_samples >= 32 and bound_completed_after > bound_completed_before
 lease_stop.set()
 for thread in lease_threads:
     thread.join(timeout=10)
-deadline = time.time() + 3
+reclaim_started = time.monotonic()
+deadline = reclaim_started + MECHANISM_TIMEOUT
 lease_after = stats()
-while lease_after.get("atomic_inflight", -1) != 0 and time.time() < deadline:
+while (lease_after.get("atomic_inflight", -1) != 0 or
+       lease_after.get("atomic_credit_debt", -1) != 0 or
+       lease_after.get("atomic_credit_pool", -1) != 3) and time.monotonic() < deadline:
     time.sleep(0.01)
     lease_after = stats()
+reclaim_elapsed = time.monotonic() - reclaim_started
+# Suppress idle lease returns to induce a nonzero debt or pool != 3; exhausted polls still FAIL.
 note("atomic-window reconfiguration preserves bound and reclaims leases",
      bounded and not any(thread.is_alive() for thread in lease_threads) and not lease_errors and
      lease_after.get("atomic_inflight", -1) == 0 and
      lease_after.get("atomic_credit_debt", -1) == 0 and
      lease_after.get("atomic_credit_pool", -1) == 3,
-     "max=%d pool=%d debt=%d errors=%r" %
+     "max=%d pool=%d debt=%d errors=%r samples=%d completed=%d shrink=%.3fs reclaim=%.3fs" %
      (max_inflight, lease_after.get("atomic_credit_pool", -1),
-      lease_after.get("atomic_credit_debt", -1), lease_errors))
+      lease_after.get("atomic_credit_debt", -1), lease_errors, bound_samples,
+      bound_completed_after - bound_completed_before, shrink_elapsed, reclaim_elapsed))
+release_note("atomic-window shrink/reclaim within release budgets",
+             shrink_elapsed <= 3.0 and reclaim_elapsed <= 3.0,
+             "shrink=%.3fs reclaim=%.3fs" % (shrink_elapsed, reclaim_elapsed))
 config("atomic-window", 256)
 
 # Live CONFIG flips under active traffic are a liveness/safety arm. OFF intervals deliberately do
diff --git a/tests/borrow_registry.py b/tests/borrow_registry.py
index 7357fd6c9..6900a8859 100755
--- a/tests/borrow_registry.py
+++ b/tests/borrow_registry.py
@@ -1,6 +1,6 @@
 #!/usr/bin/env python3
 # Zero-copy borrow registry scaling battery.
-#   python3 tests/borrow_registry.py <host> <port>
+#   python3 tests/borrow_registry.py <host> <port> [--release-build]
 # Boot: --shards 1 --zc-min 64 --client-output-buffer-limit normal 0 0 0 --enable-debug-command yes
 #   one shard  -> every borrow lands in ONE registry, which is the quantity under test
 #   zc-min 64  -> a small value still takes the borrow path, so the probe pays registry cost and
@@ -18,7 +18,9 @@
 #     zero again after holder teardown;
 #   - the plain-GET arm is a NEGATIVE CONTROL measured in the same loop on the same connection. It
 #     never enters the registry, so it is what "no growth" reads like on this machine;
-#   - the assertion is a RATIO of two per-op costs measured seconds apart, never an absolute time.
+#   - measured send/release counters distinguish the two paths independently of speed;
+#   - per-operation cost ratios require --release-build and a stable plain-GET control. ASAN
+#     still exercises both paths, parks holders, verifies exact replies, and requires teardown.
 #
 # Drift (AUDIT-TESTS F7). The two arms are INTERLEAVED per round (borrow, plain, borrow, plain ...)
 # so a CPU-state shift lands on both, not on whichever arm happened to be measured second (the
@@ -35,7 +37,12 @@ import time
 
 import _lib
 
-HOST, PORT = sys.argv[1], int(sys.argv[2])
+ARGS = sys.argv[1:]
+RELEASE_BUILD = "--release-build" in ARGS
+ARGS = [arg for arg in ARGS if arg != "--release-build"]
+if len(ARGS) != 2:
+    raise SystemExit("usage: borrow_registry.py HOST PORT [--release-build]")
+HOST, PORT = ARGS[0], int(ARGS[1])
 
 HOLD_BYTES = 8192
 # One unread connection gets ~75 MiB of replies.  Unlike its receive-window setting, that volume
@@ -138,6 +145,11 @@ class C:
     def live_borrows(self):
         return self.cmd("DEBUG", "BORROWCOUNT")
 
+    def zc_counts(self):
+        rows = dict(line.split(b":", 1) for line in self.cmd("INFO", "STATS").splitlines()
+                    if b":" in line)
+        return int(rows[b"zc_sends"]), int(rows[b"zc_releases"])
+
 
 def wait_borrows(admin, predicate, timeout=5.0):
     deadline = time.monotonic() + timeout
@@ -185,6 +197,26 @@ def main():
     probe.rate(fb, rb, DEPTH * 40, DEPTH)
     probe.rate(fp, rp, DEPTH * 40, DEPTH)
 
+    # Prove path identity with completed send/release counters, not a 1.5x latency ratio. There
+    # are no holders yet. First drain preceding borrows so they cannot move the plain control.
+    # Induce by disabling GET borrowing (positive arm fails), or borrowing below zc-min (plain
+    # arm fails). The byte-exact rate() reader still checks every response on every build.
+    check("probe borrows initially drained", wait_borrows(admin, lambda n: n == 0) == 0)
+    plain_before = admin.zc_counts()
+    probe.rate(fp, rp, OPS, DEPTH)
+    plain_after = admin.zc_counts()
+    check("plain GET never enters the borrow send path", plain_after == plain_before,
+          "before=%r after=%r" % (plain_before, plain_after))
+    borrow_before = admin.zc_counts()
+    probe.rate(fb, rb, OPS, DEPTH)
+    probe_drained = wait_borrows(admin, lambda n: n == 0)
+    borrow_after = admin.zc_counts()
+    check("the two arms really took different paths",
+          plain_after == plain_before and probe_drained == 0 and
+          all(after > before for before, after in zip(borrow_before, borrow_after)),
+          "plain=%r->%r borrow=%r->%r live=%d" %
+          (plain_before, plain_after, borrow_before, borrow_after, probe_drained))
+
     def measure():
         # Best (lowest ns/op) of ROUNDS per arm, arms interleaved so drift hits both equally.
         best_b = best_p = float("inf")
@@ -244,7 +276,11 @@ def main():
 
     growth = borrow_busy / borrow_ref
     control = plain_busy / plain_ref
-    if control > MAX_GROWTH:
+    if not RELEASE_BUILD:
+        print("  SKIP borrowed GET per-op cost growth / control flatness -- requires "
+              "--release-build; borrow %.0f -> %.0f ns (%.3f), plain %.0f -> %.0f ns (%.3f)"
+              % (borrow_ref, borrow_busy, growth, plain_ref, plain_busy, control))
+    elif control > MAX_GROWTH:
         # A BROKEN INSTRUMENT DOES NOT GET A VERDICT. The plain arm never enters the registry, so
         # nothing this row is looking for can move it. When it moves anyway -- against the pre
         # baseline, against the post baseline, and after three re-rolls -- the box shifted under
@@ -262,8 +298,8 @@ def main():
               "%.0f -> %.0f ns  ratio=%.3f (%s)" % (borrow_ref, borrow_busy, growth, reference))
         check("control (non-borrowed GET) stayed flat", control <= MAX_GROWTH,
               "%.0f -> %.0f ns  ratio=%.3f (%s)" % (plain_ref, plain_busy, control, reference))
-    check("the two arms really took different paths", borrow_ref > plain_ref * 1.5,
-          "borrow %.0f ns vs plain %.0f ns (%s)" % (borrow_ref, plain_ref, reference))
+    # Restore the linear borrow_find() scan to induce the release-only growth failure while
+    # leaving the plain control flat. Sanitizer slowdown cannot be a verdict on this 5% budget.
     admin.cmd("FLUSHALL")
     admin.close()
     print("BORROW-REGISTRY %s" % ("PASS" if FAIL == 0 else "FAIL %d" % FAIL))
diff --git a/tests/differ.py b/tests/differ.py
index 7620b70f7..4a11424f4 100644
--- a/tests/differ.py
+++ b/tests/differ.py
@@ -2806,13 +2806,19 @@ def run_blocking_differ(rng):
          ["ZADD", "cbd:forever:bzmpop", "1", "v"]),
     ]
     for command, wake in forever_arms:
-        tbase, obase = info_blocked(ts, tf), info_blocked(os_, of)
+        # The preceding reply can precede its gauge decrement too. Arm against a settled
+        # baseline, so that delayed decrement cannot cancel this arm's observed increment.
+        tbase, obase = await_blocked(ts, tf, 0), await_blocked(os_, of, 0)
         tw, twf = conn_mode(TH, TP, RESP3); ow, owf = conn_mode(OH, OP, RESP3)
         tw.sendall(enc(command)); ow.sendall(enc(command)); logical_ops += 1
         tarmed = await_blocked(ts, tf, tbase + 1)
         oarmed = await_blocked(os_, of, obase + 1)
         property_check("%s timeout-zero armed" % command[0], tarmed == tbase + 1,
                        oarmed == obase + 1)
+        # A gauge stuck at zero must fail even if both servers miss the arm. Induce by
+        # suppressing blocked_clients increments; parity alone would pass False == False.
+        property_check("target %s blocking gauge moved" % command[0], tarmed, tbase + 1)
+        property_check("oracle %s blocking gauge moved" % command[0], oarmed, obase + 1)
         tready = bool(select.select([tw], [], [], 0.05)[0])
         oready = bool(select.select([ow], [], [], 0.05)[0])
         property_check("%s timeout-zero silence" % command[0], tready, oready)
@@ -2921,7 +2927,11 @@ def run_blocking_differ(rng):
             property_check("WAIT finite deadline fired", time.monotonic() - started >= 0.15, True)
         tw.close(); ow.close(); twf.close(); owf.close()
 
-    final_t, final_o = info_blocked(ts, tf), info_blocked(os_, of)
+    # Closing a WAIT socket does not synchronously retire its blocked-client registration on
+    # either server. Compare settled values, never two independently moving gauges. The bounded
+    # poll retains the last value on timeout: suppress disconnect's decrement to make the zero
+    # control fail, even if both servers leak the same count and the parity row agrees.
+    final_t, final_o = await_blocked(ts, tf, 0), await_blocked(os_, of, 0)
     property_check("blocking gauges drain", final_t, final_o)
     property_check("target blocking gauge zero control", final_t, 0)
     property_check("oracle blocking gauge zero control", final_o, 0)
@@ -5964,28 +5974,50 @@ if SUITE == "infofix":
         property_fail("target peak reset", repr(reset_memory))
     print("  infofix peak points: %r" % memory_points)
 
-    # Sampled-rate controls: RESETSTAT+idle is zero, a byte-compared PING burst is positive, and a
-    # second RESETSTAT returns the detector to zero.
+    # The same sampled-rate contract as infofix.py: INFO-only polls let residual samples drain,
+    # and work is fed until a sample sees it. No arbitrary instant is compared to the oracle's
+    # live gauge. Pin published_rate at zero/nonzero to fail the positive/idle controls.
+    def sampled_rate(sock, file):
+        return int(fields(sock, file, "stats").get("instantaneous_ops_per_sec", "-1"))
+
+    def idle_rate(sock, file):
+        deadline = time.monotonic() + 5.0
+        while True:
+            value = sampled_rate(sock, file)
+            if value <= 0 or time.monotonic() >= deadline:
+                return value
+            time.sleep(.11)
+
     for sock, file in ((ts, tf), (os_, of)):
         issue(sock, file, ["CONFIG", "RESETSTAT"])
-    time.sleep(.15)
-    target_control = int(fields(ts, tf, "stats").get("instantaneous_ops_per_sec", "-1"))
+    target_control = idle_rate(ts, tf)
     if target_control != 0: property_fail("target rate control", str(target_control))
     payload = enc(["PING"]) * 1000
-    ts.sendall(payload); os_.sendall(payload)
-    for iteration in range(1000):
-        target_reply, oracle_reply = read_reply(tf), read_reply(of)
-        if target_reply != oracle_reply:
-            property_fail("rate burst byte compare", "iteration=%d" % iteration)
+    deadline = time.monotonic() + 5.0
+    while True:
+        ts.sendall(payload); os_.sendall(payload)
+        replies_ok = True
+        for iteration in range(1000):
+            target_reply, oracle_reply = read_reply(tf), read_reply(of)
+            if target_reply != oracle_reply:
+                property_fail("rate burst byte compare", "iteration=%d" % iteration)
+                replies_ok = False
+        target_rate = sampled_rate(ts, tf)
+        oracle_rate = sampled_rate(os_, of)
+        if (not replies_ok or (target_rate > 0 and oracle_rate > 0) or
+                target_rate < 0 or oracle_rate < 0 or time.monotonic() >= deadline):
             break
-    time.sleep(.12)
-    target_rate = int(fields(ts, tf, "stats").get("instantaneous_ops_per_sec", "0"))
-    oracle_rate = int(fields(os_, of, "stats").get("instantaneous_ops_per_sec", "0"))
+        time.sleep(.11)
     if target_rate <= 0: property_fail("target rate fired", str(target_rate))
     if oracle_rate <= 0: property_fail("oracle rate fired", str(oracle_rate))
     issue(ts, tf, ["CONFIG", "RESETSTAT"])
-    time.sleep(.15)
-    reset_rate = int(fields(ts, tf, "stats").get("instantaneous_ops_per_sec", "-1"))
+    # Natural sampler aging must not hide an omitted RESETSTAT baseline (leave PING calls
+    # untouched to induce this independent reset failure).
+    ping = fields(ts, tf, "commandstats").get("cmdstat_ping", "calls=0")
+    ping_calls = dict(item.split("=", 1) for item in ping.split(",") if "=" in item)
+    if int(ping_calls["calls"]) != 0:
+        property_fail("target commandstats reset", ping)
+    reset_rate = idle_rate(ts, tf)
     if reset_rate != 0: property_fail("target rate reset", str(reset_rate))
     print("  infofix sampled rates: control=%d target=%d oracle=%d reset=%d" %
           (target_control, target_rate, oracle_rate, reset_rate))

exec
/bin/bash -lc 'git diff -- tests/borrow_registry.py tests/differ.py tests/expireindex.py tests/infofix.py tests/pubsub.py tests/slowlog.py tests/stream.py' in /home/user/Projects/cx-timing
 succeeded in 0ms:
diff --git a/tests/borrow_registry.py b/tests/borrow_registry.py
index 7357fd6c9..6900a8859 100755
--- a/tests/borrow_registry.py
+++ b/tests/borrow_registry.py
@@ -1,6 +1,6 @@
 #!/usr/bin/env python3
 # Zero-copy borrow registry scaling battery.
-#   python3 tests/borrow_registry.py <host> <port>
+#   python3 tests/borrow_registry.py <host> <port> [--release-build]
 # Boot: --shards 1 --zc-min 64 --client-output-buffer-limit normal 0 0 0 --enable-debug-command yes
 #   one shard  -> every borrow lands in ONE registry, which is the quantity under test
 #   zc-min 64  -> a small value still takes the borrow path, so the probe pays registry cost and
@@ -18,7 +18,9 @@
 #     zero again after holder teardown;
 #   - the plain-GET arm is a NEGATIVE CONTROL measured in the same loop on the same connection. It
 #     never enters the registry, so it is what "no growth" reads like on this machine;
-#   - the assertion is a RATIO of two per-op costs measured seconds apart, never an absolute time.
+#   - measured send/release counters distinguish the two paths independently of speed;
+#   - per-operation cost ratios require --release-build and a stable plain-GET control. ASAN
+#     still exercises both paths, parks holders, verifies exact replies, and requires teardown.
 #
 # Drift (AUDIT-TESTS F7). The two arms are INTERLEAVED per round (borrow, plain, borrow, plain ...)
 # so a CPU-state shift lands on both, not on whichever arm happened to be measured second (the
@@ -35,7 +37,12 @@ import time
 
 import _lib
 
-HOST, PORT = sys.argv[1], int(sys.argv[2])
+ARGS = sys.argv[1:]
+RELEASE_BUILD = "--release-build" in ARGS
+ARGS = [arg for arg in ARGS if arg != "--release-build"]
+if len(ARGS) != 2:
+    raise SystemExit("usage: borrow_registry.py HOST PORT [--release-build]")
+HOST, PORT = ARGS[0], int(ARGS[1])
 
 HOLD_BYTES = 8192
 # One unread connection gets ~75 MiB of replies.  Unlike its receive-window setting, that volume
@@ -138,6 +145,11 @@ class C:
     def live_borrows(self):
         return self.cmd("DEBUG", "BORROWCOUNT")
 
+    def zc_counts(self):
+        rows = dict(line.split(b":", 1) for line in self.cmd("INFO", "STATS").splitlines()
+                    if b":" in line)
+        return int(rows[b"zc_sends"]), int(rows[b"zc_releases"])
+
 
 def wait_borrows(admin, predicate, timeout=5.0):
     deadline = time.monotonic() + timeout
@@ -185,6 +197,26 @@ def main():
     probe.rate(fb, rb, DEPTH * 40, DEPTH)
     probe.rate(fp, rp, DEPTH * 40, DEPTH)
 
+    # Prove path identity with completed send/release counters, not a 1.5x latency ratio. There
+    # are no holders yet. First drain preceding borrows so they cannot move the plain control.
+    # Induce by disabling GET borrowing (positive arm fails), or borrowing below zc-min (plain
+    # arm fails). The byte-exact rate() reader still checks every response on every build.
+    check("probe borrows initially drained", wait_borrows(admin, lambda n: n == 0) == 0)
+    plain_before = admin.zc_counts()
+    probe.rate(fp, rp, OPS, DEPTH)
+    plain_after = admin.zc_counts()
+    check("plain GET never enters the borrow send path", plain_after == plain_before,
+          "before=%r after=%r" % (plain_before, plain_after))
+    borrow_before = admin.zc_counts()
+    probe.rate(fb, rb, OPS, DEPTH)
+    probe_drained = wait_borrows(admin, lambda n: n == 0)
+    borrow_after = admin.zc_counts()
+    check("the two arms really took different paths",
+          plain_after == plain_before and probe_drained == 0 and
+          all(after > before for before, after in zip(borrow_before, borrow_after)),
+          "plain=%r->%r borrow=%r->%r live=%d" %
+          (plain_before, plain_after, borrow_before, borrow_after, probe_drained))
+
     def measure():
         # Best (lowest ns/op) of ROUNDS per arm, arms interleaved so drift hits both equally.
         best_b = best_p = float("inf")
@@ -244,7 +276,11 @@ def main():
 
     growth = borrow_busy / borrow_ref
     control = plain_busy / plain_ref
-    if control > MAX_GROWTH:
+    if not RELEASE_BUILD:
+        print("  SKIP borrowed GET per-op cost growth / control flatness -- requires "
+              "--release-build; borrow %.0f -> %.0f ns (%.3f), plain %.0f -> %.0f ns (%.3f)"
+              % (borrow_ref, borrow_busy, growth, plain_ref, plain_busy, control))
+    elif control > MAX_GROWTH:
         # A BROKEN INSTRUMENT DOES NOT GET A VERDICT. The plain arm never enters the registry, so
         # nothing this row is looking for can move it. When it moves anyway -- against the pre
         # baseline, against the post baseline, and after three re-rolls -- the box shifted under
@@ -262,8 +298,8 @@ def main():
               "%.0f -> %.0f ns  ratio=%.3f (%s)" % (borrow_ref, borrow_busy, growth, reference))
         check("control (non-borrowed GET) stayed flat", control <= MAX_GROWTH,
               "%.0f -> %.0f ns  ratio=%.3f (%s)" % (plain_ref, plain_busy, control, reference))
-    check("the two arms really took different paths", borrow_ref > plain_ref * 1.5,
-          "borrow %.0f ns vs plain %.0f ns (%s)" % (borrow_ref, plain_ref, reference))
+    # Restore the linear borrow_find() scan to induce the release-only growth failure while
+    # leaving the plain control flat. Sanitizer slowdown cannot be a verdict on this 5% budget.
     admin.cmd("FLUSHALL")
     admin.close()
     print("BORROW-REGISTRY %s" % ("PASS" if FAIL == 0 else "FAIL %d" % FAIL))
diff --git a/tests/differ.py b/tests/differ.py
index 7620b70f7..4a11424f4 100644
--- a/tests/differ.py
+++ b/tests/differ.py
@@ -2806,13 +2806,19 @@ def run_blocking_differ(rng):
          ["ZADD", "cbd:forever:bzmpop", "1", "v"]),
     ]
     for command, wake in forever_arms:
-        tbase, obase = info_blocked(ts, tf), info_blocked(os_, of)
+        # The preceding reply can precede its gauge decrement too. Arm against a settled
+        # baseline, so that delayed decrement cannot cancel this arm's observed increment.
+        tbase, obase = await_blocked(ts, tf, 0), await_blocked(os_, of, 0)
         tw, twf = conn_mode(TH, TP, RESP3); ow, owf = conn_mode(OH, OP, RESP3)
         tw.sendall(enc(command)); ow.sendall(enc(command)); logical_ops += 1
         tarmed = await_blocked(ts, tf, tbase + 1)
         oarmed = await_blocked(os_, of, obase + 1)
         property_check("%s timeout-zero armed" % command[0], tarmed == tbase + 1,
                        oarmed == obase + 1)
+        # A gauge stuck at zero must fail even if both servers miss the arm. Induce by
+        # suppressing blocked_clients increments; parity alone would pass False == False.
+        property_check("target %s blocking gauge moved" % command[0], tarmed, tbase + 1)
+        property_check("oracle %s blocking gauge moved" % command[0], oarmed, obase + 1)
         tready = bool(select.select([tw], [], [], 0.05)[0])
         oready = bool(select.select([ow], [], [], 0.05)[0])
         property_check("%s timeout-zero silence" % command[0], tready, oready)
@@ -2921,7 +2927,11 @@ def run_blocking_differ(rng):
             property_check("WAIT finite deadline fired", time.monotonic() - started >= 0.15, True)
         tw.close(); ow.close(); twf.close(); owf.close()
 
-    final_t, final_o = info_blocked(ts, tf), info_blocked(os_, of)
+    # Closing a WAIT socket does not synchronously retire its blocked-client registration on
+    # either server. Compare settled values, never two independently moving gauges. The bounded
+    # poll retains the last value on timeout: suppress disconnect's decrement to make the zero
+    # control fail, even if both servers leak the same count and the parity row agrees.
+    final_t, final_o = await_blocked(ts, tf, 0), await_blocked(os_, of, 0)
     property_check("blocking gauges drain", final_t, final_o)
     property_check("target blocking gauge zero control", final_t, 0)
     property_check("oracle blocking gauge zero control", final_o, 0)
@@ -5964,28 +5974,50 @@ if SUITE == "infofix":
         property_fail("target peak reset", repr(reset_memory))
     print("  infofix peak points: %r" % memory_points)
 
-    # Sampled-rate controls: RESETSTAT+idle is zero, a byte-compared PING burst is positive, and a
-    # second RESETSTAT returns the detector to zero.
+    # The same sampled-rate contract as infofix.py: INFO-only polls let residual samples drain,
+    # and work is fed until a sample sees it. No arbitrary instant is compared to the oracle's
+    # live gauge. Pin published_rate at zero/nonzero to fail the positive/idle controls.
+    def sampled_rate(sock, file):
+        return int(fields(sock, file, "stats").get("instantaneous_ops_per_sec", "-1"))
+
+    def idle_rate(sock, file):
+        deadline = time.monotonic() + 5.0
+        while True:
+            value = sampled_rate(sock, file)
+            if value <= 0 or time.monotonic() >= deadline:
+                return value
+            time.sleep(.11)
+
     for sock, file in ((ts, tf), (os_, of)):
         issue(sock, file, ["CONFIG", "RESETSTAT"])
-    time.sleep(.15)
-    target_control = int(fields(ts, tf, "stats").get("instantaneous_ops_per_sec", "-1"))
+    target_control = idle_rate(ts, tf)
     if target_control != 0: property_fail("target rate control", str(target_control))
     payload = enc(["PING"]) * 1000
-    ts.sendall(payload); os_.sendall(payload)
-    for iteration in range(1000):
-        target_reply, oracle_reply = read_reply(tf), read_reply(of)
-        if target_reply != oracle_reply:
-            property_fail("rate burst byte compare", "iteration=%d" % iteration)
+    deadline = time.monotonic() + 5.0
+    while True:
+        ts.sendall(payload); os_.sendall(payload)
+        replies_ok = True
+        for iteration in range(1000):
+            target_reply, oracle_reply = read_reply(tf), read_reply(of)
+            if target_reply != oracle_reply:
+                property_fail("rate burst byte compare", "iteration=%d" % iteration)
+                replies_ok = False
+        target_rate = sampled_rate(ts, tf)
+        oracle_rate = sampled_rate(os_, of)
+        if (not replies_ok or (target_rate > 0 and oracle_rate > 0) or
+                target_rate < 0 or oracle_rate < 0 or time.monotonic() >= deadline):
             break
-    time.sleep(.12)
-    target_rate = int(fields(ts, tf, "stats").get("instantaneous_ops_per_sec", "0"))
-    oracle_rate = int(fields(os_, of, "stats").get("instantaneous_ops_per_sec", "0"))
+        time.sleep(.11)
     if target_rate <= 0: property_fail("target rate fired", str(target_rate))
     if oracle_rate <= 0: property_fail("oracle rate fired", str(oracle_rate))
     issue(ts, tf, ["CONFIG", "RESETSTAT"])
-    time.sleep(.15)
-    reset_rate = int(fields(ts, tf, "stats").get("instantaneous_ops_per_sec", "-1"))
+    # Natural sampler aging must not hide an omitted RESETSTAT baseline (leave PING calls
+    # untouched to induce this independent reset failure).
+    ping = fields(ts, tf, "commandstats").get("cmdstat_ping", "calls=0")
+    ping_calls = dict(item.split("=", 1) for item in ping.split(",") if "=" in item)
+    if int(ping_calls["calls"]) != 0:
+        property_fail("target commandstats reset", ping)
+    reset_rate = idle_rate(ts, tf)
     if reset_rate != 0: property_fail("target rate reset", str(reset_rate))
     print("  infofix sampled rates: control=%d target=%d oracle=%d reset=%d" %
           (target_control, target_rate, oracle_rate, reset_rate))
diff --git a/tests/expireindex.py b/tests/expireindex.py
index 775613727..23b78948f 100755
--- a/tests/expireindex.py
+++ b/tests/expireindex.py
@@ -10,8 +10,8 @@
 #      monotone in the all-time-high volatile population.  One historical burst taxed every later
 #      live->0 forever: 2.03x a neighbouring DEL after a 1M burst.
 #
-# Every assertion is a RATIO between two measurements taken on the SAME connection seconds apart,
-# never an absolute time, so the battery does not care how fast the machine is.
+# Ratios of short timings still depend on scheduling and instrumentation. Exact population,
+# deadline and delete checks run everywhere; only an explicit --release-build scores timings.
 #
 # Non-vacuous by construction:
 #   - each leg asserts INFO keyspace `expires` really reached the population it claims to test, so
@@ -26,7 +26,12 @@ import statistics
 import sys
 import time
 
-HOST, PORT = sys.argv[1], int(sys.argv[2])
+ARGS = sys.argv[1:]
+RELEASE_BUILD = "--release-build" in ARGS
+ARGS = [arg for arg in ARGS if arg != "--release-build"]
+if len(ARGS) != 2:
+    raise SystemExit("usage: expireindex.py HOST PORT [--release-build]")
+HOST, PORT = ARGS[0], int(ARGS[1])
 
 # The two timing legs need ONE expire index under test, so they need a single-shard boot:
 #   taskset -c <cores> ./build/tomokv --port P --shards 1 --ratio 1:1 --enable-debug-command yes
@@ -61,6 +66,13 @@ def check(name, cond, detail=""):
     ok(name, detail) if cond else bad(name, detail)
 
 
+def release_check(name, cond, detail):
+    if RELEASE_BUILD:
+        check(name, cond, detail)
+    else:
+        print("  SKIP %s -- requires --release-build; %s" % (name, detail))
+
+
 def enc(*args):
     out = [b"*%d\r\n" % len(args)]
     for a in args:
@@ -176,7 +188,9 @@ def leg_growth_is_incremental(c):
     check("populations really differ by 16x", large_live >= 15 * small_live,
           "%d vs %d" % (large_live, small_live))
     growth = large_trig / small_trig
-    check("trigger cost growth <= %.1fx over 16x population" % MAX_TRIGGER_GROWTH,
+    # Induce the timing failure by replacing bounded migration with finish_migration() on every
+    # growth trigger. Population/deadline checks above and in leg_semantics remain mandatory.
+    release_check("trigger cost growth <= %.1fx over 16x population" % MAX_TRIGGER_GROWTH,
           growth <= MAX_TRIGGER_GROWTH,
           "small=%.0fus large=%.0fus growth=%.2fx" % (small_trig, large_trig, growth))
     # NEGATIVE CONTROL: identical shape with no deadlines, so the sidecar is never populated. This
@@ -204,13 +218,21 @@ def zero_pair(c, tag, burst, ttl, reps=41):
     for _ in range(reps):
         assert c.cmd("SET", "xi:%sA" % tag, b"v", *args) == b"OK"
         assert c.cmd("SET", "xi:%sB" % tag, b"v", *args) == b"OK"
+        assert c.volatile_count() == (2 if ttl else 0)
         t0 = time.perf_counter_ns()
-        c.cmd("DEL", "xi:%sA" % tag)
+        removed_a = c.cmd("DEL", "xi:%sA" % tag)
         t1 = time.perf_counter_ns()
-        c.cmd("DEL", "xi:%sB" % tag)
+        # Keep INFO outside BOTH measured intervals. Exact 2 -> 1 -> 0 accounting, one surviving
+        # value, and successful re-arming are the mechanism. Suppress untrack_expire() or corrupt
+        # the survivor in collapse_empty() to induce failure even without timing assertions.
+        assert removed_a == 1 and c.volatile_count() == (1 if ttl else 0)
+        assert c.cmd("GET", "xi:%sB" % tag) == b"v"
+        z0 = time.perf_counter_ns()
+        removed_b = c.cmd("DEL", "xi:%sB" % tag)
         t2 = time.perf_counter_ns()
+        assert removed_b == 1 and c.volatile_count() == 0
         ctl.append(t1 - t0)
-        zero.append(t2 - t1)
+        zero.append(t2 - z0)
     assert c.cmd("DBSIZE") == 0
     return statistics.median(ctl) / 1e3, statistics.median(zero) / 1e3, reached
 
@@ -221,7 +243,9 @@ def leg_zero_transition(c):
     check("burst really populated the sidecar", reached >= ZERO_BURST,
           "expires=%d want>=%d" % (reached, ZERO_BURST))
     ratio = zero / ctl
-    check("live->0 DEL <= %.2fx a neighbouring DEL" % MAX_ZERO_RATIO, ratio <= MAX_ZERO_RATIO,
+    # Induce by restoring the all-time-high table memset at live->0 (measured PRE 2.03x,
+    # POST 1.01x in NOTES-XPERF2.md). This is a release performance claim, not delete semantics.
+    release_check("live->0 DEL <= %.2fx a neighbouring DEL" % MAX_ZERO_RATIO, ratio <= MAX_ZERO_RATIO,
           "ctl=%.2fus zero=%.2fus ratio=%.2f" % (ctl, zero, ratio))
     # NEGATIVE CONTROL: same burst size, no deadlines. The DEL that empties the keyspace then never
     # touches the sidecar at all, so this is the detector's zero reading.
diff --git a/tests/infofix.py b/tests/infofix.py
index b07dbd76d..61d6dc28e 100755
--- a/tests/infofix.py
+++ b/tests/infofix.py
@@ -267,22 +267,49 @@ def wire_bytes_and_rate():
     check("input counter exact after PING", as_int(second, "total_net_input_bytes"), expected_input)
     check("output counter exact after PING", as_int(second, "total_net_output_bytes"), expected_output)
 
+    def idle_rate():
+        # INFO is excluded from sampled_ops. Polling only INFO advances the eight 100ms sample
+        # slots without feeding the meter. A RESETSTAT command can itself finish after the
+        # reset baseline; wait for that residual sample to age out instead of sleeping once.
+        # Induce failure by retaining a nonzero published rate when the idle window has drained.
+        deadline = time.monotonic() + 5.0
+        trace = []
+        while True:
+            value = as_int(info(c, "stats"), "instantaneous_ops_per_sec")
+            trace.append(value)
+            if value == 0 or value < 0 or time.monotonic() >= deadline:
+                print("  idle sampled rates: %r" % trace)
+                return value
+            time.sleep(0.11)
+
     c.cmd("CONFIG", "RESETSTAT")
-    time.sleep(0.15)
-    control = as_int(info(c, "stats"), "instantaneous_ops_per_sec")
+    control = idle_rate()
     check("ops/sec idle control", control, 0)
     payload = encode("PING") * 5000
-    c.sock.sendall(payload)
-    for _ in range(5000):
-        _, reply = c.read()
-        if reply != "PONG":
-            raise AssertionError("pipeline PING returned %r" % reply)
-    time.sleep(0.12)
-    loaded = as_int(info(c, "stats"), "instantaneous_ops_per_sec")
+    deadline = time.monotonic() + 5.0
+    loaded_trace = []
+    while True:
+        c.sock.sendall(payload)
+        for _ in range(5000):
+            _, reply = c.read()
+            if reply != "PONG":
+                raise AssertionError("pipeline PING returned %r" % reply)
+        loaded = as_int(info(c, "stats"), "instantaneous_ops_per_sec")
+        loaded_trace.append(loaded)
+        if loaded != 0 or time.monotonic() >= deadline:
+            break
+        time.sleep(0.11)
+    # Feed work until the sampler observes it. Pin the sampler at zero to induce failure;
+    # neither the positive-value check nor the exact idle checks degrade to SKIP on timeout.
+    print("  loaded sampled rates: %r" % loaded_trace)
     check("ops/sec sampler fired", loaded, lambda value: value > 0)
     c.cmd("CONFIG", "RESETSTAT")
-    time.sleep(0.15)
-    check("ops/sec RESETSTAT control", as_int(info(c, "stats"), "instantaneous_ops_per_sec"), 0)
+    # Also prove RESETSTAT executed: natural aging alone cannot satisfy a broken reset. Removing
+    # its command-counter baseline leaves this run's PINGs visible and fails this companion.
+    ping = info(c, "commandstats").get("cmdstat_ping", "calls=0")
+    ping_fields = dict(item.split("=", 1) for item in ping.split(",") if "=" in item)
+    check("RESETSTAT cleared the measured PINGs", int(ping_fields["calls"]), 0)
+    check("ops/sec RESETSTAT control", idle_rate(), 0)
     c.close()
 
 
diff --git a/tests/pubsub.py b/tests/pubsub.py
index cc9850352..837ab2a53 100755
--- a/tests/pubsub.py
+++ b/tests/pubsub.py
@@ -127,6 +127,21 @@ def info_stats(conn):
     return result
 
 
+def patterns_drained(admin, label, timeout=5.0):
+    # NUMPAT is global, including registrations whose sockets have closed but whose IO cleanup
+    # has not run. No pattern clients are live at these boundaries. Retain the exact zero
+    # assertion on timeout; suppress disconnect's pattern unregister to induce failure. The
+    # live-pattern rows still require 2, so returning constant zero cannot pass the battery.
+    deadline = time.monotonic() + timeout
+    while True:
+        count = admin.command("PUBSUB", "NUMPAT")
+        if count == 0:
+            return
+        if time.monotonic() >= deadline:
+            raise AssertionError(f"{label}: NUMPAT did not drain, last={count!r}")
+        time.sleep(0.01)
+
+
 def main():
     if len(sys.argv) != 3:
         raise SystemExit(f"usage: {sys.argv[0]} HOST PORT")
@@ -365,6 +380,7 @@ def main():
     multi.close()
 
     # Exact and both pattern arms, including subscriber-mode framing/restrictions.
+    patterns_drained(admin, "before exact/pattern arms")
     arms = Conn(host, port)
     arm_channel = f"{token}:news:42"
     expect(arms.command("SUBSCRIBE", arm_channel), [b"subscribe", arm_channel.encode(), 1],
@@ -616,6 +632,7 @@ def main():
            [b"pmessage", f"{token}:mix*".encode(), mixed.encode(), b"again"], "resp3 pmessage body")
     resp2_sub.close()
     resp3_sub.close()
+    patterns_drained(admin, "mixed RESP3 pattern disconnect")
 
     # D. Subscriber disconnect mid-fanout. Half the subscribers are reset while a pipelined burst
     #    is in flight, so batches already queued name connections that no longer exist. The survivors
@@ -677,6 +694,7 @@ def main():
            b"OK", "restore obuf limits")
 
     # F. Introspection aggregates against a Python-side model of the same population.
+    patterns_drained(admin, "before NUMPAT model")
     model_exact, model_patterns, model_shard = {}, {}, {}
     model_conns = []
     for index in range(10):
diff --git a/tests/slowlog.py b/tests/slowlog.py
index e20cdef53..a51edc9a3 100644
--- a/tests/slowlog.py
+++ b/tests/slowlog.py
@@ -98,6 +98,17 @@ def counters(c):
     return out
 
 
+def recording_threads(c):
+    rows = dict(line.split(":", 1) for line in c.cmd("INFO", "SERVER").splitlines()
+                if ":" in line)
+    # Fused client and owner roles overlap: adding those role counts would double count rings.
+    count = (int(rows["fused_threads"]) if rows["thread_mode"] == "1s" else
+             int(rows["io_threads"]) + int(rows["ex_threads"]))
+    if count <= 0:
+        raise AssertionError("INFO SERVER reported no recording threads: %r" % rows)
+    return count
+
+
 def err(text):
     return lambda got: isinstance(got, RuntimeError) and str(got) == text
 
@@ -185,13 +196,17 @@ def main():
     print("slowlog-max-len")
     check("set max-len 5", c.cmd("CONFIG", "SET", "slowlog-max-len", "5"), "OK")
     c.cmd("SLOWLOG", "RESET")
-    for i in range(40):
+    nrecorders = recording_threads(c)
+    # More writes than the aggregate capacity, even at a 16+ CPU boot. Induce failure by
+    # disabling SlowLog's ring eviction/trim: the log then exceeds 5*nrecorders or nrecorders.
+    for i in range(5 * nrecorders + 1):
         c.cmd("SET", "sl:trim%d" % i, "v")
-    # The ring is per-executor, so the global bound is max-len per recording thread.
-    check("max-len bounds the log", c.cmd("SLOWLOG", "LEN"), lambda v: 0 < v <= 5 * 8)
+    # Both IO-local and owner commands record, one ring per live thread.
+    check("max-len bounds the log", c.cmd("SLOWLOG", "LEN"),
+          lambda v: 0 < v <= 5 * nrecorders)
     check("shrinking max-len trims immediately",
           (c.cmd("CONFIG", "SET", "slowlog-max-len", "1"), c.cmd("SLOWLOG", "LEN"))[1],
-          lambda v: v <= 8)
+          lambda v: 0 < v <= nrecorders)
     check("max-len 0 keeps nothing", c.cmd("CONFIG", "SET", "slowlog-max-len", "0"), "OK")
     c.cmd("SLOWLOG", "RESET")
     for i in range(20):
diff --git a/tests/stream.py b/tests/stream.py
index 8b8ee45ec..260241967 100644
--- a/tests/stream.py
+++ b/tests/stream.py
@@ -8,7 +8,12 @@ import threading
 import time
 
 
-HOST, PORT = sys.argv[1], int(sys.argv[2])
+ARGS = sys.argv[1:]
+RELEASE_BUILD = "--release-build" in ARGS
+ARGS = [arg for arg in ARGS if arg != "--release-build"]
+if len(ARGS) != 2:
+    raise SystemExit("usage: stream.py HOST PORT [--release-build]")
+HOST, PORT = ARGS[0], int(ARGS[1])
 FAIL = 0
 
 
@@ -19,6 +24,13 @@ def note(name, ok, extra=""):
     FAIL += not ok
 
 
+def release_note(name, ok, measured):
+    if RELEASE_BUILD:
+        note(name, ok, measured)
+    else:
+        print("  SKIP %s -- requires --release-build; %s" % (name, measured), flush=True)
+
+
 def frame(*args):
     values = [arg if isinstance(arg, bytes) else str(arg).encode() for arg in args]
     return (b"*%d\r\n" % len(values) +
@@ -307,9 +319,18 @@ for base in range(plateau_mid + 1, plateau_ops + 1, 250):
                     for ident in range(base, min(plateau_ops + 1, base + 250))])
 plateau_second = info_value(admin, "MEMORY", "used_memory_dataset")
 plateau_length = admin.cmd("XLEN", "stream:plateau")
+plateau_entries = admin.cmd("XRANGE", "stream:plateau", "-", "+")
 admin.cmd("DEL", "stream:plateau")
 plateau_after_delete = info_value(admin, "MEMORY", "used_memory_dataset")
-note("sustained exact trim reaches a memory plateau",
+# Disable trimming or drop the wrong end to induce a value failure on every build. Allocation
+# redzones can change byte budgets; they cannot change the exact last 100 entries promised here.
+note("sustained exact trim retains the last 100 entries",
+     plateau_length == 100 and plateau_entries ==
+     [[("%d-0" % ident).encode(), [b"f", b"v"]]
+      for ident in range(plateau_ops - 99, plateau_ops + 1)] and
+     admin.cmd("EXISTS", "stream:plateau") == 0)
+# Retain old macro nodes after trim/delete to induce the release footprint failure.
+release_note("sustained exact trim reaches a memory plateau",
      plateau_length == 100 and plateau_second <= plateau_first + 8192 and
      plateau_after_delete <= baseline + 4096,
      "first=%d second=%d after-del=%d baseline=%d" %
@@ -321,11 +342,22 @@ note("sustained exact trim reaches a memory plateau",
 admin.cmd("FLUSHDB")
 baseline = info_value(admin, "MEMORY", "used_memory_dataset")
 sample = 1024
+floor_ok = True
 for index in range(sample):
-    admin.cmd("XADD", "stream:floor:%d" % index, "1-0", "f", "v")
+    key = "stream:floor:%d" % index
+    added = admin.cmd("XADD", key, "1-0", "f", "v")
+    entries = admin.cmd("XRANGE", key, "-", "+")
+    floor_ok = floor_ok and added == b"1-0" and entries == [[b"1-0", [b"f", b"v"]]]
 resident = info_value(admin, "MEMORY", "used_memory_dataset")
-per_stream = max(0, resident - baseline) / sample
-note("one-entry memory floor tracks ~140B design (well below 4.4KiB)", per_stream < 512,
+per_stream = (resident - baseline) / sample
+# Drop first-entry installation/accounting to induce this mechanism failure. Inflating the
+# first-entry allocation to 4.4KiB instead fails the footprint row on a release build. ASAN's
+# allocator/redzones are not evidence against the release design; its measured bytes still print.
+note("one-entry streams retain their exact payload and allocate memory",
+     floor_ok and admin.cmd("DBSIZE") == sample and resident > baseline,
+     "keys=%d measured=%.1fB/key" % (sample, per_stream))
+release_note("one-entry memory floor tracks ~140B design (well below 4.4KiB)",
+     0 < per_stream < 512,
      "measured=%.1fB/key audit-kvobj~=140B" % per_stream)
 # The one-big-flip migration assert is RETIRED with the implementation it described: under the
 # consumer-groups lane's budget-driven macro nodes (stream-node-max-entries/-bytes, redis
@@ -343,7 +375,12 @@ for ident in range(2, 129):
     admin.cmd("XADD", "stream:migrate", "%d-0" % ident, "f", "v")
 final_usage = admin.cmd("MEMORY", "USAGE", "stream:migrate")
 admin.cmd("CONFIG", "SET", "stream-node-max-entries", "100")
-note("growth across the embedded->macro boundary stays amortized-linear",
+note("embedded->macro migration retains every entry",
+     admin.cmd("XRANGE", "stream:migrate", "-", "+") ==
+     [[("%d-0" % ident).encode(), [b"f", b"v"]] for ident in range(1, 129)])
+# Lose entries during migration to fail the value row; reserve quadratic node capacity to fail
+# this separate release footprint bound. Both rows are measured on the same migration.
+release_note("growth across the embedded->macro boundary stays amortized-linear",
      isinstance(final_usage, int) and final_usage < 128 * 200,
      "usage=%r for 128 entries (bound 200B/entry)" % final_usage)
 

exec
/bin/bash -lc 'git diff -- tests/gate.sh' in /home/user/Projects/cx-knobs
 succeeded in 0ms:
diff --git a/tests/gate.sh b/tests/gate.sh
index c4d8fef19..295db179f 100755
--- a/tests/gate.sh
+++ b/tests/gate.sh
@@ -1,7 +1,7 @@
 #!/bin/bash
 # PRE-PUSH GATE for tomokv-cpp (pure 2s baseline).
 #
-#   tests/gate.sh quick   loopback only, ~3 min: build (release+ASAN), footprint locks, boot
+#   tests/gate.sh quick   loopback only: build (release+ASAN), footprint locks, boot
 #                         matrix, smoke, torture, RYOW, atomic torn/mixed-write/window gates,
 #                         shutdown invariants, counter-fired assertions, idle-loop ceiling. Runs on
 #                         any machine.
@@ -465,7 +465,7 @@ for AT in 0 1; do
   if boot_fused ./build/tomokv --atomic "$AT" --enable-debug-command yes; then
     FUSED_INFO=$(redis-cli -h 127.0.0.1 -p "$PORT" INFO server 2>/dev/null | tr -d '\r')
     FUSED_MODE=$(printf '%s\n' "$FUSED_INFO" | sed -n 's/^thread_mode://p')
-    FUSED_OVERLAP=$(printf '%s\n' "$FUSED_INFO" | sed -n 's/^overlap://p')
+    FUSED_OVERLAP=$(printf '%s\n' "$FUSED_INFO" | sed -n 's/^x_overlap://p')
     [ "$FUSED_MODE" = 1s ] && [ "$FUSED_OVERLAP" = 0 ] \
         && ok "fused boot line (atomic $AT)" \
         || bad "fused boot line (atomic $AT)" "wire mode=$FUSED_MODE overlap=$FUSED_OVERLAP"
@@ -528,20 +528,20 @@ done
 # ---- eviction accounting on both read paths: owner-served (split) and lane-served (fused+armed) --
 # tests/evict_battery.py needs one FRESH boot per section (it sets maxmemory itself and has no
 # FLUSHALL). lfu proves OBJECT FREQ rises for ordinary reads and stays put for CLIENT NO-TOUCH
-# reads, plus hot-set survival under pressure; lruclock is the LRU twin on a 1 s bucket
-# (--lru-clock-shift 0: the default 256 s bucket cannot tell two keys apart inside one gate row).
+# reads, plus hot-set survival under pressure; lruclock ages all cohorts across one fixed 256 s
+# production bucket (a bounded wait of up to 260 s per boot) before testing the read paths.
 # On the armed boot both sections also assert the reads they measure were LANE-served -- a key
 # kept hot only by lane reads was never touched before ExLoopT::note_local_read_access and so was
 # evicted FIRST, which is the inverse of the policy the boot asked for.
 for AT in 0 1; do
   for EVSEC in lfu lruclock; do
-    boot ./build/tomokv --atomic $AT --lru-clock-shift 0 \
+    boot ./build/tomokv --atomic $AT \
         || bad "eviction $EVSEC boot (split, atomic $AT)"
     py tests/evict_battery.py $PORT $EVSEC >/tmp/gate-evict-$EVSEC-split-$AT.txt 2>&1 \
         && ok "eviction $EVSEC battery (split, atomic $AT)" \
         || bad "eviction $EVSEC battery (split, atomic $AT)" "see /tmp/gate-evict-$EVSEC-split-$AT.txt"
     stop
-    boot_fused ./build/tomokv --atomic $AT --read-local 1 --lru-clock-shift 0 \
+    boot_fused ./build/tomokv --atomic $AT --read-local 1 \
         || bad "eviction $EVSEC boot (fused+armed, atomic $AT)"
     py tests/evict_battery.py $PORT $EVSEC >/tmp/gate-evict-$EVSEC-armed-$AT.txt 2>&1 \
         && ok "eviction $EVSEC battery (fused+armed, atomic $AT)" \
@@ -554,7 +554,7 @@ done
 # DEBUG geometry makes false positives impossible in these arms: A/B share one physical shard but
 # use distinct filter cells, as do P/C on the other participating owner. ATOMIC-COMMIT-DELAY holds
 # A/P after raw install and atomic_commit_holds proves the reserved/unpublished window was sampled.
-boot_fused ./build/tomokv --atomic 1 --read-local 1 --read-local-atomic-filter 1 \
+boot_fused ./build/tomokv --atomic 1 --read-local 1 \
     --enable-debug-command yes \
     || bad "B+ read-local purpose boot"
 py tests/bplus.py 127.0.0.1 "$PORT" >/tmp/gate-bplus.txt 2>&1 \
@@ -592,7 +592,6 @@ stop
 # opposite the source. No same-owner or one-executor run can satisfy this row.
 for AT in 0 1; do
   boot ./build/tomokv --ratio 6:2 --atomic "$AT" --enable-debug-command yes \
-       --lb-age-sample-rate 1024 \
       || bad "cross-owner SORT boot (atomic $AT)"
   py tests/sort.py 127.0.0.1 "$PORT" >/tmp/gate-sort-$AT.txt 2>&1 \
       && ok "cross-owner SORT battery (atomic $AT)" \
@@ -602,7 +601,7 @@ done
 
 # ---- debug-surface batteries: these drive DEBUG subcommands, hence their own armed boot -------
 for AT in 0 1; do
-  boot ./build/tomokv --atomic $AT --enable-debug-command yes --lb-age-sample-rate 1024 \
+  boot ./build/tomokv --atomic $AT --enable-debug-command yes \
       || bad "debug-surface boot (atomic $AT)"
   # scriptatomic needs the armed boot for its cross-shard section (DEBUG SHARD proves the group
   # really spans owners; ATOMIC-COMMIT-DELAY / ATOMIC-READ-DELAY widen the window). It flips
@@ -642,17 +641,9 @@ for AT in 0 1; do
   stop
 done
 
-# ---- cross-owner script control boots: each needs its own knob value, hence its own server ----
-# off:    script-crossshard-max-bytes 0 must reproduce today's CROSSSLOT byte-for-byte and leave
-#         every cross counter at zero (the feature is off ⇒ nothing is allocated).
-# limit:  a tiny staging budget must refuse before RUN and leave the declared values untouched.
-# window: one cut slot per IO thread must refuse the overflow with -BUSY, counted exactly, with
-#         the same activations issued one at a time as the zero control.
-for XS in "off:--script-crossshard-max-bytes 0" \
-          "limit:--script-crossshard-max-bytes 300" \
-          "window:--script-crossshard-cut-slots 1"; do
-  XS_MODE=${XS%%:*}; XS_ARGS=${XS#*:}
-  boot ./build/tomokv --atomic 1 --enable-debug-command yes $XS_ARGS \
+# ---- cross-owner script bounds: production auto budgets and four cut slots per IO ----------
+for XS_MODE in limit window; do
+  boot ./build/tomokv --atomic 1 --enable-debug-command yes \
       || bad "xscript $XS_MODE control boot"
   py tests/xscript.py 127.0.0.1 $PORT "$XS_MODE" >/tmp/gate-xscript-$XS_MODE.txt 2>&1 \
       && ok "xscript $XS_MODE control" || bad "xscript $XS_MODE control" \
@@ -679,11 +670,7 @@ py tests/borrow_registry.py 127.0.0.1 $PORT >/tmp/gate-borrow.txt 2>&1 \
     && ok "borrow-registry growth bound" || bad "borrow-registry growth bound" "see /tmp/gate-borrow.txt"
 stop
 
-# this one boots its own arms at two thread counts, so it takes the port/cores rather than a server
-quiet_wait
-XDS_PORT=$PORT XDS_CPUS=$CORES XDS_BIN=./build/tomokv bash tests/xshard_dispatch_scale.sh \
-    >/tmp/gate-xds.txt 2>&1 \
-    && ok "cross-shard dispatch scaling" || bad "cross-shard dispatch scaling" "see /tmp/gate-xds.txt"
+# Dispatch scaling's manual ownership geometry was retired with shard-home; DESIGN-KNOBS.md.
 
 # ---- Redis-wire DUMP/RESTORE survives the native snapshot/restart boundary -------------------
 DUMPRESTORE_DIR=$(mktemp -d /tmp/gate-dumprestore.XXXXXX)
@@ -743,25 +730,25 @@ py tests/debug.py 127.0.0.1 $PORT >/tmp/gate-debug.txt 2>&1 \
 stop
 
 # ---- snapshot data/sync engines: cut, typed round-trip, and typed preimage race ----------------
-for PERSIST_IO in normal uring; do
-  SNAP_DIR=$(mktemp -d "/tmp/gate-snapshot-${PERSIST_IO}.XXXXXX")
-  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
+for NET_IO in epoll uring; do
+  SNAP_DIR=$(mktemp -d "/tmp/gate-snapshot-${NET_IO}.XXXXXX")
+  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
       --dir "$SNAP_DIR" --dbfilename cut.tomo \
-      || bad "snapshot cut boot ($PERSIST_IO)"
+      || bad "snapshot cut boot ($NET_IO)"
   py tests/snap_cut_battery.py "$PORT" save \
-      >"/tmp/gate-snapshot-cut-${PERSIST_IO}.txt" 2>&1 \
-      && ok "snapshot concurrent cut ($PERSIST_IO)" \
-      || bad "snapshot concurrent cut ($PERSIST_IO)" \
-             "see /tmp/gate-snapshot-cut-${PERSIST_IO}.txt"
+      >"/tmp/gate-snapshot-cut-${NET_IO}.txt" 2>&1 \
+      && ok "snapshot concurrent cut ($NET_IO)" \
+      || bad "snapshot concurrent cut ($NET_IO)" \
+             "see /tmp/gate-snapshot-cut-${NET_IO}.txt"
   stop
-  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
+  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
       --dir "$SNAP_DIR" --load "$SNAP_DIR/cut.tomo" \
-      || bad "snapshot cut reload boot ($PERSIST_IO)"
+      || bad "snapshot cut reload boot ($NET_IO)"
   py tests/snap_cut_battery.py "$PORT" verify_cut \
-      >>"/tmp/gate-snapshot-cut-${PERSIST_IO}.txt" 2>&1 \
-      && ok "snapshot cut reload ($PERSIST_IO)" \
-      || bad "snapshot cut reload ($PERSIST_IO)" \
-             "see /tmp/gate-snapshot-cut-${PERSIST_IO}.txt"
+      >>"/tmp/gate-snapshot-cut-${NET_IO}.txt" 2>&1 \
+      && ok "snapshot cut reload ($NET_IO)" \
+      || bad "snapshot cut reload ($NET_IO)" \
+             "see /tmp/gate-snapshot-cut-${NET_IO}.txt"
   stop
 
   # The cut battery above writes only single keys, so it passes on a tree whose snapshot tears
@@ -769,70 +756,70 @@ for PERSIST_IO in normal uring; do
   # generation-tagged groups, a live MGET reader that must stay clean throughout, and the
   # cuts_waited counter that separates a real group drain from a vacuous one. Both arms were
   # confirmed to FAIL on the pre-fix tree (44-53 and 10-13 torn groups per cut respectively).
-  GROUP_DIR=$(mktemp -d "/tmp/gate-snapshot-groups-${PERSIST_IO}.XXXXXX")
-  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" --atomic 1 \
+  GROUP_DIR=$(mktemp -d "/tmp/gate-snapshot-groups-${NET_IO}.XXXXXX")
+  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" --atomic 1 \
       --enable-debug-command yes --dir "$GROUP_DIR" --dbfilename groups.tomo \
-      || bad "atomic group cut boot ($PERSIST_IO, atomic 1)"
+      || bad "atomic group cut boot ($NET_IO, atomic 1)"
   py tests/snap_cut_battery.py "$PORT" atomic_groups "$GROUP_DIR/groups.tomo" mset 5 \
-      >"/tmp/gate-snapshot-groups-mset-${PERSIST_IO}.txt" 2>&1 \
-      && grep -q "ATOMIC_GROUP_CUT PASS" "/tmp/gate-snapshot-groups-mset-${PERSIST_IO}.txt" \
-      && ok "snapshot never tears a cross-shard MSET group ($PERSIST_IO, atomic 1)" \
-      || bad "snapshot never tears a cross-shard MSET group ($PERSIST_IO, atomic 1)" \
-             "see /tmp/gate-snapshot-groups-mset-${PERSIST_IO}.txt"
+      >"/tmp/gate-snapshot-groups-mset-${NET_IO}.txt" 2>&1 \
+      && grep -q "ATOMIC_GROUP_CUT PASS" "/tmp/gate-snapshot-groups-mset-${NET_IO}.txt" \
+      && ok "snapshot never tears a cross-shard MSET group ($NET_IO, atomic 1)" \
+      || bad "snapshot never tears a cross-shard MSET group ($NET_IO, atomic 1)" \
+             "see /tmp/gate-snapshot-groups-mset-${NET_IO}.txt"
   stop
   # CONTROL ARM: the DEFAULT --atomic 0. EXEC force-admits a group at either setting, so a
   # transaction is atomic to readers here too and the file must agree with them.
-  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" --atomic 0 \
+  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" --atomic 0 \
       --enable-debug-command yes --dir "$GROUP_DIR" --dbfilename groups.tomo \
-      || bad "atomic group cut boot ($PERSIST_IO, atomic 0)"
+      || bad "atomic group cut boot ($NET_IO, atomic 0)"
   py tests/snap_cut_battery.py "$PORT" atomic_groups "$GROUP_DIR/groups.tomo" exec 3 \
-      >"/tmp/gate-snapshot-groups-exec-${PERSIST_IO}.txt" 2>&1 \
-      && grep -q "ATOMIC_GROUP_CUT PASS" "/tmp/gate-snapshot-groups-exec-${PERSIST_IO}.txt" \
-      && ok "snapshot never tears a MULTI/EXEC group ($PERSIST_IO, default atomic 0)" \
-      || bad "snapshot never tears a MULTI/EXEC group ($PERSIST_IO, default atomic 0)" \
-             "see /tmp/gate-snapshot-groups-exec-${PERSIST_IO}.txt"
+      >"/tmp/gate-snapshot-groups-exec-${NET_IO}.txt" 2>&1 \
+      && grep -q "ATOMIC_GROUP_CUT PASS" "/tmp/gate-snapshot-groups-exec-${NET_IO}.txt" \
+      && ok "snapshot never tears a MULTI/EXEC group ($NET_IO, default atomic 0)" \
+      || bad "snapshot never tears a MULTI/EXEC group ($NET_IO, default atomic 0)" \
+             "see /tmp/gate-snapshot-groups-exec-${NET_IO}.txt"
   stop
 
-  TYPED_DIR=$(mktemp -d "/tmp/gate-snapshot-typed-${PERSIST_IO}.XXXXXX")
-  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
+  TYPED_DIR=$(mktemp -d "/tmp/gate-snapshot-typed-${NET_IO}.XXXXXX")
+  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
       --dir "$TYPED_DIR" --dbfilename typed.tomo \
-      || bad "typed snapshot boot ($PERSIST_IO)"
+      || bad "typed snapshot boot ($NET_IO)"
   py tests/snap_typed_roundtrip.py "$PORT" build_save \
-      >"/tmp/gate-snapshot-typed-${PERSIST_IO}.txt" 2>&1 \
-      && ok "typed snapshot save ($PERSIST_IO)" \
-      || bad "typed snapshot save ($PERSIST_IO)" \
-             "see /tmp/gate-snapshot-typed-${PERSIST_IO}.txt"
+      >"/tmp/gate-snapshot-typed-${NET_IO}.txt" 2>&1 \
+      && ok "typed snapshot save ($NET_IO)" \
+      || bad "typed snapshot save ($NET_IO)" \
+             "see /tmp/gate-snapshot-typed-${NET_IO}.txt"
   stop
-  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
+  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
       --dir "$TYPED_DIR" --load "$TYPED_DIR/typed.tomo" \
-      || bad "typed snapshot reload boot ($PERSIST_IO)"
+      || bad "typed snapshot reload boot ($NET_IO)"
   py tests/snap_typed_roundtrip.py "$PORT" verify \
-      >>"/tmp/gate-snapshot-typed-${PERSIST_IO}.txt" 2>&1 \
-      && ok "typed snapshot reload ($PERSIST_IO)" \
-      || bad "typed snapshot reload ($PERSIST_IO)" \
-             "see /tmp/gate-snapshot-typed-${PERSIST_IO}.txt"
+      >>"/tmp/gate-snapshot-typed-${NET_IO}.txt" 2>&1 \
+      && ok "typed snapshot reload ($NET_IO)" \
+      || bad "typed snapshot reload ($NET_IO)" \
+             "see /tmp/gate-snapshot-typed-${NET_IO}.txt"
   stop
 
-  RACE_DIR=$(mktemp -d "/tmp/gate-snapshot-race-${PERSIST_IO}.XXXXXX")
-  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
+  RACE_DIR=$(mktemp -d "/tmp/gate-snapshot-race-${NET_IO}.XXXXXX")
+  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
       --dir "$RACE_DIR" --dbfilename race.tomo --save '' --enable-debug-command yes \
-      || bad "typed snapshot race boot ($PERSIST_IO)"
+      || bad "typed snapshot race boot ($NET_IO)"
   py tests/snap_typed_race.py "$PORT" race "$RACE_DIR/race.tomo" \
-      >"/tmp/gate-snapshot-race-${PERSIST_IO}.txt" 2>&1 \
-      && grep -q 'PREIMAGE-FIRED PASS' "/tmp/gate-snapshot-race-${PERSIST_IO}.txt" \
-      && grep -q 'SNAPSHOT-TICKET-ORACLE PASS' "/tmp/gate-snapshot-race-${PERSIST_IO}.txt" \
-      && ok "typed snapshot preimage race ($PERSIST_IO)" \
-      || bad "typed snapshot preimage race ($PERSIST_IO)" \
-             "see /tmp/gate-snapshot-race-${PERSIST_IO}.txt"
+      >"/tmp/gate-snapshot-race-${NET_IO}.txt" 2>&1 \
+      && grep -q 'PREIMAGE-FIRED PASS' "/tmp/gate-snapshot-race-${NET_IO}.txt" \
+      && grep -q 'SNAPSHOT-TICKET-ORACLE PASS' "/tmp/gate-snapshot-race-${NET_IO}.txt" \
+      && ok "typed snapshot preimage race ($NET_IO)" \
+      || bad "typed snapshot preimage race ($NET_IO)" \
+             "see /tmp/gate-snapshot-race-${NET_IO}.txt"
   stop
-  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
+  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
       --dir "$RACE_DIR" --load "$RACE_DIR/race.tomo.cut" \
-      || bad "typed snapshot race reload boot ($PERSIST_IO)"
+      || bad "typed snapshot race reload boot ($NET_IO)"
   py tests/snap_typed_race.py "$PORT" verify "$RACE_DIR/race.tomo.oracle.json" \
-      >>"/tmp/gate-snapshot-race-${PERSIST_IO}.txt" 2>&1 \
-      && ok "typed snapshot race reload ($PERSIST_IO)" \
-      || bad "typed snapshot race reload ($PERSIST_IO)" \
-             "see /tmp/gate-snapshot-race-${PERSIST_IO}.txt"
+      >>"/tmp/gate-snapshot-race-${NET_IO}.txt" 2>&1 \
+      && ok "typed snapshot race reload ($NET_IO)" \
+      || bad "typed snapshot race reload ($NET_IO)" \
+             "see /tmp/gate-snapshot-race-${NET_IO}.txt"
   stop
 done
 
@@ -875,10 +862,10 @@ py tests/spinprobe.py $PORT "$SRV" >/tmp/gate-spinprobe.txt 2>&1 \
     && ok "partial-frame conn parks (no io spin)" || bad "partial-frame conn parks (no io spin)" "see /tmp/gate-spinprobe.txt"
 stop
 boot ./build/tomokv --ratio 6:2 --atomic 0 --enable-debug-command yes --flip-auto 1 \
-     --flip-auto-band 2 --lb-age-sample-rate 1024 || bad "flipctl boot"
+     || bad "flipctl boot"
 # 300 -> 600 s: the stable-hold row now MEASURES its precondition (the driver's own command rate
 # must be stationary by the controller's own band) before its assertion window opens, and re-rolls
-# a hold whose load left that band up to three times inside its own wall budget before skipping it.
+# a hold whose load left that band up to three times inside its own wall budget before failing it.
 # The typical run is unchanged; the worst case adds the re-rolls plus their re-anchor waits.
 timeout 600 python3 tests/flipctl.py --host 127.0.0.1 --port $PORT --stable-seconds 30 \
     >/tmp/gate-flipctl.txt 2>&1 \
@@ -966,26 +953,24 @@ timeout 120 python3 tests/pipeorder.py $PORT 400 >/tmp/gate-pipeorder.txt 2>&1 \
 stop
 
 # ---- AOF boot/replay + non-vacuous DEBUG LOADAOF ---------------------------------------------
-for PERSIST_IO in normal uring; do
+for NET_IO in epoll uring; do
 for AOF_ATOMIC in 0 1; do
-AOF_DIR=$(mktemp -d "/tmp/gate-aof-${PERSIST_IO}-atomic${AOF_ATOMIC}.XXXXXX")
+AOF_DIR=$(mktemp -d "/tmp/gate-aof-${NET_IO}-atomic${AOF_ATOMIC}.XXXXXX")
 AOF_STATE=$AOF_DIR/state.json
 boot ./build/tomokv --protected-mode no --atomic "$AOF_ATOMIC" \
     --appendonly yes --appendfsync no \
-    --persist-io "$PERSIST_IO" --enable-debug-command yes --dir "$AOF_DIR" \
-    || bad "AOF purpose boot ($PERSIST_IO, atomic $AOF_ATOMIC)"
-PERSIST_GET=$(redis-cli -h 127.0.0.1 -p $PORT --raw CONFIG GET persist-io 2>/dev/null |
-    tail -1)
-PERSIST_SET=$(redis-cli -h 127.0.0.1 -p $PORT CONFIG SET persist-io "$PERSIST_IO" 2>&1)
-[ "$PERSIST_GET" = "$PERSIST_IO" ] && printf '%s' "$PERSIST_SET" | grep -q immutable \
-    && ok "persist-io surface + immutable ($PERSIST_IO)" \
-    || bad "persist-io surface + immutable ($PERSIST_IO)"
-py tests/aof.py 127.0.0.1 $PORT populate "$AOF_STATE" >/tmp/gate-aof-$PERSIST_IO-$AOF_ATOMIC.txt 2>&1 \
-    && py tests/aof.py 127.0.0.1 $PORT loadaof "$AOF_STATE" >>/tmp/gate-aof-$PERSIST_IO-$AOF_ATOMIC.txt 2>&1 \
-    && ok "AOF byte-exact + script groups + DEBUG LOADAOF ($PERSIST_IO, atomic $AOF_ATOMIC)" \
-    || bad "AOF byte-exact + script groups + DEBUG LOADAOF ($PERSIST_IO, atomic $AOF_ATOMIC)" \
-           "see /tmp/gate-aof-$PERSIST_IO-$AOF_ATOMIC.txt"
-AOF_PRE_MODEL=$(py tests/aof.py 127.0.0.1 $PORT snapshot "$AOF_DIR/dump.tomo" 2>>/tmp/gate-aof-$PERSIST_IO-$AOF_ATOMIC.txt)
+    --net-io "$NET_IO" --enable-debug-command yes --dir "$AOF_DIR" \
+    || bad "AOF purpose boot ($NET_IO, atomic $AOF_ATOMIC)"
+py tests/knobs.py 127.0.0.1 "$PORT" "$NET_IO" "$AOF_ATOMIC" \
+    >"/tmp/gate-knobs-${NET_IO}-${AOF_ATOMIC}.txt" 2>&1 \
+    && ok "configuration reduction + actual geometry ($NET_IO, atomic $AOF_ATOMIC)" \
+    || bad "configuration reduction + actual geometry ($NET_IO, atomic $AOF_ATOMIC)"
+py tests/aof.py 127.0.0.1 $PORT populate "$AOF_STATE" >/tmp/gate-aof-$NET_IO-$AOF_ATOMIC.txt 2>&1 \
+    && py tests/aof.py 127.0.0.1 $PORT loadaof "$AOF_STATE" >>/tmp/gate-aof-$NET_IO-$AOF_ATOMIC.txt 2>&1 \
+    && ok "AOF byte-exact + script groups + DEBUG LOADAOF ($NET_IO, atomic $AOF_ATOMIC)" \
+    || bad "AOF byte-exact + script groups + DEBUG LOADAOF ($NET_IO, atomic $AOF_ATOMIC)" \
+           "see /tmp/gate-aof-$NET_IO-$AOF_ATOMIC.txt"
+AOF_PRE_MODEL=$(py tests/aof.py 127.0.0.1 $PORT snapshot "$AOF_DIR/dump.tomo" 2>>/tmp/gate-aof-$NET_IO-$AOF_ATOMIC.txt)
 AOF_WRITTEN=$(redis-cli -h 127.0.0.1 -p $PORT INFO Persistence 2>/dev/null \
     | tr -d '\r' | sed -n 's/^aof_records_written://p')
 [ -n "$AOF_WRITTEN" ] && [ "$AOF_WRITTEN" -gt 0 ] \
@@ -994,14 +979,14 @@ kill -KILL $SRV 2>/dev/null
 wait $SRV 2>/dev/null
 settle
 boot ./build/tomokv --protected-mode no --appendonly yes --appendfsync no \
-    --atomic "$AOF_ATOMIC" --persist-io "$PERSIST_IO" \
+    --atomic "$AOF_ATOMIC" --net-io "$NET_IO" \
     --enable-debug-command yes --dir "$AOF_DIR" \
-    || bad "AOF replay boot ($PERSIST_IO, atomic $AOF_ATOMIC)"
-py tests/aof.py 127.0.0.1 $PORT verify "$AOF_STATE" >>/tmp/gate-aof-$PERSIST_IO-$AOF_ATOMIC.txt 2>&1 \
-    && ok "AOF process-restart script replay ($PERSIST_IO, atomic $AOF_ATOMIC)" \
-    || bad "AOF process-restart script replay ($PERSIST_IO, atomic $AOF_ATOMIC)" \
-           "see /tmp/gate-aof-$PERSIST_IO-$AOF_ATOMIC.txt"
-AOF_POST_MODEL=$(py tests/aof.py 127.0.0.1 $PORT snapshot "$AOF_DIR/dump.tomo" 2>>/tmp/gate-aof-$PERSIST_IO-$AOF_ATOMIC.txt)
+    || bad "AOF replay boot ($NET_IO, atomic $AOF_ATOMIC)"
+py tests/aof.py 127.0.0.1 $PORT verify "$AOF_STATE" >>/tmp/gate-aof-$NET_IO-$AOF_ATOMIC.txt 2>&1 \
+    && ok "AOF process-restart script replay ($NET_IO, atomic $AOF_ATOMIC)" \
+    || bad "AOF process-restart script replay ($NET_IO, atomic $AOF_ATOMIC)" \
+           "see /tmp/gate-aof-$NET_IO-$AOF_ATOMIC.txt"
+AOF_POST_MODEL=$(py tests/aof.py 127.0.0.1 $PORT snapshot "$AOF_DIR/dump.tomo" 2>>/tmp/gate-aof-$NET_IO-$AOF_ATOMIC.txt)
 [ -n "$AOF_PRE_MODEL" ] && [ "$AOF_PRE_MODEL" = "$AOF_POST_MODEL" ] \
     && ok "AOF native snapshot streams byte-exact" || bad "AOF native snapshot streams byte-exact"
 AOF_REPLAYED=$(redis-cli -h 127.0.0.1 -p $PORT INFO Persistence 2>/dev/null \
@@ -1014,11 +999,11 @@ stop
 done
 
 # ---- AOF atomic-group bracketing + directed interrupted-process recovery ---------------------
-AOF_GROUP_DIR=$(mktemp -d "/tmp/gate-aof-group-${PERSIST_IO}.XXXXXX")
+AOF_GROUP_DIR=$(mktemp -d "/tmp/gate-aof-group-${NET_IO}.XXXXXX")
 AOF_GROUP_STATE=$AOF_GROUP_DIR/state.json
 boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync no \
-    --persist-io "$PERSIST_IO" --enable-debug-command yes --dir "$AOF_GROUP_DIR" \
-    || bad "AOF group purpose boot ($PERSIST_IO)"
+    --net-io "$NET_IO" --enable-debug-command yes --dir "$AOF_GROUP_DIR" \
+    || bad "AOF group purpose boot ($NET_IO)"
 py tests/aof_torn_group.py 127.0.0.1 $PORT prepare "$AOF_GROUP_STATE" \
     >/tmp/gate-aof-group.txt 2>&1 \
     && ok "AOF directed group interruption fired" \
@@ -1026,8 +1011,8 @@ py tests/aof_torn_group.py 127.0.0.1 $PORT prepare "$AOF_GROUP_STATE" \
 wait $SRV 2>/dev/null
 settle
 boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync no \
-    --persist-io "$PERSIST_IO" --enable-debug-command yes --dir "$AOF_GROUP_DIR" \
-    || bad "AOF group recovery boot ($PERSIST_IO)"
+    --net-io "$NET_IO" --enable-debug-command yes --dir "$AOF_GROUP_DIR" \
+    || bad "AOF group recovery boot ($NET_IO)"
 py tests/aof_torn_group.py 127.0.0.1 $PORT verify "$AOF_GROUP_STATE" \
     >>/tmp/gate-aof-group.txt 2>&1 \
     && py tests/aof_torn_group.py 127.0.0.1 $PORT scan \
@@ -1040,11 +1025,11 @@ shutdown_clean \
     && ok "AOF group shutdown invariants" || bad "AOF group shutdown invariants"
 
 # ---- AOF sync policies, reply gate, idle sync, and durability-window recovery ----------------
-AOF_ALWAYS_DIR=$(mktemp -d "/tmp/gate-aof-always-${PERSIST_IO}.XXXXXX")
+AOF_ALWAYS_DIR=$(mktemp -d "/tmp/gate-aof-always-${NET_IO}.XXXXXX")
 AOF_ALWAYS_STATE=$AOF_ALWAYS_DIR/state.json
 boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync always \
-    --persist-io "$PERSIST_IO" --dir "$AOF_ALWAYS_DIR" \
-    || bad "AOF always purpose boot ($PERSIST_IO)"
+    --net-io "$NET_IO" --dir "$AOF_ALWAYS_DIR" \
+    || bad "AOF always purpose boot ($NET_IO)"
 py tests/aof_fsync.py 127.0.0.1 $PORT populate "$AOF_ALWAYS_STATE" always 512 \
     >/tmp/gate-aof-always.txt 2>&1 \
     && ok "AOF always sync + reply gate fired" \
@@ -1053,20 +1038,20 @@ kill -KILL $SRV 2>/dev/null
 wait $SRV 2>/dev/null
 settle
 boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync always \
-    --persist-io "$PERSIST_IO" --dir "$AOF_ALWAYS_DIR" \
-    || bad "AOF always recovery boot ($PERSIST_IO)"
+    --net-io "$NET_IO" --dir "$AOF_ALWAYS_DIR" \
+    || bad "AOF always recovery boot ($NET_IO)"
 py tests/aof_fsync.py 127.0.0.1 $PORT verify "$AOF_ALWAYS_STATE" always 512 \
     >>/tmp/gate-aof-always.txt 2>&1 \
     && ok "AOF always acknowledged-prefix recovery" \
     || bad "AOF always acknowledged-prefix recovery" "see /tmp/gate-aof-always.txt"
 stop
 
-AOF_EVERY_DIR=$(mktemp -d "/tmp/gate-aof-everysec-${PERSIST_IO}.XXXXXX")
+AOF_EVERY_DIR=$(mktemp -d "/tmp/gate-aof-everysec-${NET_IO}.XXXXXX")
 AOF_EVERY_STATE=$AOF_EVERY_DIR/state.json
 AOF_EVERY_FILE=$AOF_EVERY_DIR/appendonlydir/appendonly.aof.1.incr.tomo
 boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync everysec \
-    --persist-io "$PERSIST_IO" --dir "$AOF_EVERY_DIR" \
-    || bad "AOF everysec purpose boot ($PERSIST_IO)"
+    --net-io "$NET_IO" --dir "$AOF_EVERY_DIR" \
+    || bad "AOF everysec purpose boot ($NET_IO)"
 py tests/aof_fsync.py 127.0.0.1 $PORT populate "$AOF_EVERY_STATE" everysec 512 \
     >/tmp/gate-aof-everysec.txt 2>&1 \
     && ok "AOF everysec write gate + idle sync fired" \
@@ -1081,8 +1066,8 @@ case "$AOF_EVERY_FILE" in
 esac
 settle
 boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync everysec \
-    --persist-io "$PERSIST_IO" --dir "$AOF_EVERY_DIR" \
-    || bad "AOF everysec recovery boot ($PERSIST_IO)"
+    --net-io "$NET_IO" --dir "$AOF_EVERY_DIR" \
+    || bad "AOF everysec recovery boot ($NET_IO)"
 py tests/aof_fsync.py 127.0.0.1 $PORT verify "$AOF_EVERY_STATE" everysec 512 \
     >>/tmp/gate-aof-everysec.txt 2>&1 \
     && grep -q "AOF warning: truncated AOF tail" "$SRVLOG" \
@@ -1090,10 +1075,10 @@ py tests/aof_fsync.py 127.0.0.1 $PORT verify "$AOF_EVERY_STATE" everysec 512 \
     || bad "AOF everysec durability window + tail warning" "see /tmp/gate-aof-everysec.txt"
 stop
 
-AOF_NO_DIR=$(mktemp -d "/tmp/gate-aof-no-sync-${PERSIST_IO}.XXXXXX")
+AOF_NO_DIR=$(mktemp -d "/tmp/gate-aof-no-sync-${NET_IO}.XXXXXX")
 boot ./build/tomokv --protected-mode no --atomic 0 --appendonly yes --appendfsync no \
-    --persist-io "$PERSIST_IO" --dir "$AOF_NO_DIR" \
-    || bad "AOF no-sync purpose boot ($PERSIST_IO)"
+    --net-io "$NET_IO" --dir "$AOF_NO_DIR" \
+    || bad "AOF no-sync purpose boot ($NET_IO)"
 py tests/aof_fsync.py 127.0.0.1 $PORT populate "$AOF_NO_DIR/state.json" no 128 \
     >/tmp/gate-aof-no-sync.txt 2>&1 \
     && ok "AOF no-sync bypassed sync + reply gate" \
@@ -1101,19 +1086,19 @@ py tests/aof_fsync.py 127.0.0.1 $PORT populate "$AOF_NO_DIR/state.json" no 128 \
 stop
 
 quiet_wait
-PERSIST_IO=$PERSIST_IO GATE_PORT=$PORT GATE_CORES=$CORES tests/aof_rewrite_matrix.sh \
+NET_IO=$NET_IO GATE_PORT=$PORT GATE_CORES=$CORES tests/aof_rewrite_matrix.sh \
     >/tmp/gate-aof-rewrite.txt 2>&1 \
     && ok "AOF rewrite atomic/stage/corruption matrix" \
     || bad "AOF rewrite matrix" "see /tmp/gate-aof-rewrite.txt"
 
 quiet_wait
-PERSIST_IO=$PERSIST_IO GATE_PORT=$PORT GATE_CORES=$CORES tests/aof_rewrite_trigger_matrix.sh \
+NET_IO=$NET_IO GATE_PORT=$PORT GATE_CORES=$CORES tests/aof_rewrite_trigger_matrix.sh \
     >/tmp/gate-aof-rewrite-trigger.txt 2>&1 \
     && ok "AOF rewrite triggers + observability matrix" \
     || bad "AOF rewrite triggers" "see /tmp/gate-aof-rewrite-trigger.txt"
 
-AOF_OFF_DIR=$(mktemp -d "/tmp/gate-aof-off-${PERSIST_IO}.XXXXXX")
-boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
+AOF_OFF_DIR=$(mktemp -d "/tmp/gate-aof-off-${NET_IO}.XXXXXX")
+boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
     --appendonly no --dir "$AOF_OFF_DIR" \
     || bad "AOF-off negative-control boot"
 AOF_OFF_SEED=$(redis-cli -h 127.0.0.1 -p $PORT SET aof-negative-control must-disappear 2>&1 |
@@ -1125,7 +1110,7 @@ AOF_OFF_PRE_SIZE=$(redis-cli -h 127.0.0.1 -p $PORT DBSIZE 2>/dev/null | tr -d '\
 kill -KILL $SRV 2>/dev/null
 wait $SRV 2>/dev/null
 settle
-boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
+boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
     --appendonly no --dir "$AOF_OFF_DIR" \
     || bad "AOF-off negative-control reboot"
 AOF_OFF_SIZE=$(redis-cli -h 127.0.0.1 -p $PORT DBSIZE 2>/dev/null | tr -d '\r')
@@ -1139,13 +1124,13 @@ done
 # writer flushed a ready GCMT at the top of a writer pass without checking that a large record
 # still held the physical stream. Recovery truncates the file from that large record's first byte,
 # so a control frame inside it is discardable -- and the loader refuses to start on the whole file.
-# persist-io normal only: that is where the defect was demonstrated (11 of 114 runs of the AOF
+# Syscall persistence under epoll: that is where the defect was demonstrated (11 of 114 runs of the AOF
 # battery, 0 of 117 on uring) and where the window is entered reliably enough for the row to prove
 # its mechanism fired. The battery FAILS on a build with the guard removed (6 of 6).
 for AOF_FRAME_ATOMIC in 0 1; do
 AOF_FRAME_DIR=$(mktemp -d "/tmp/gate-aof-frameorder-atomic${AOF_FRAME_ATOMIC}.XXXXXX")
 boot ./build/tomokv --protected-mode no --atomic "$AOF_FRAME_ATOMIC" --appendonly yes \
-    --appendfsync no --persist-io normal --auto-aof-rewrite-percentage 0 \
+    --appendfsync no --net-io epoll --auto-aof-rewrite-percentage 0 \
     --enable-debug-command yes --dir "$AOF_FRAME_DIR" \
     || bad "AOF frame-order purpose boot (atomic $AOF_FRAME_ATOMIC)"
 py tests/aof_frame_order.py 127.0.0.1 $PORT "$AOF_FRAME_DIR/appendonlydir" \
@@ -1212,7 +1197,7 @@ py tests/tls.py 127.0.0.1 "$TLS_PORT" "$TLS_DIR" optional --plain-port "$PORT" \
     >/tmp/gate-tls-optional.txt 2>&1 \
     && ok "TLS client-auth optional matrix" \
     || bad "TLS client-auth optional matrix" "see /tmp/gate-tls-optional.txt"
-# Live kTLS engagement proof on the default (tls-ktls yes) boot: a plain TLS client connects and
+# Live kTLS engagement proof on the default boot: a plain TLS client connects and
 # must see itself counted in the active gauge. Client-auth 'optional' permits a cert-less client.
 python3 - "$TLS_PORT" "$TLS_DIR" <<'PYEOF' >/tmp/gate-ktls-live.txt 2>&1 \
     && ok "kTLS engaged live (default boot)" || bad "kTLS engaged live" "see /tmp/gate-ktls-live.txt"
@@ -1232,9 +1217,9 @@ stop
 shutdown_clean \
     && ok "TLS optional shutdown invariants" || bad "TLS optional shutdown invariants"
 
-# The full battery runs FORCED-FALLBACK so the userspace arm's mechanisms (zc suppression, memory
-# BIO paths) keep their non-vacuous proofs; the default-ktls arm is proven separately below.
-tlsboot no --tls-ktls no --tls-protocols "TLSv1.2 TLSv1.3" --tls-ciphers DEFAULT \
+# Exercise userspace fallback through the retained cipher grammar. TLS 1.2 uses CBC; TLS 1.3
+# uses AES-256, outside this implementation's AES-128 TLS 1.3 RX installer. Counters must fire.
+tlsboot no --tls-protocols "TLSv1.2 TLSv1.3" --tls-ciphers ECDHE-RSA-AES256-SHA384 \
     --tls-ciphersuites TLS_AES_256_GCM_SHA384 --tls-prefer-server-ciphers yes \
     || bad "TLS coexistence purpose boot"
 py tests/tls.py 127.0.0.1 "$TLS_PORT" "$TLS_DIR" no --plain-port "$PORT" --full \
@@ -1272,7 +1257,7 @@ py tests/atomic_torn.py 127.0.0.1 $PORT >/tmp/gate-atomic-torn-asan.txt 2>&1 \
 # atomic groups beats 24 serial round trips by >10%). ASAN does not slow the two arms by the same
 # factor, so that ratio inverts here on a correct build -- measured pipe 21,005/s vs serial
 # 24,202/s in a full-gate run whose every correctness check passed, on a build that passed 4 of 4
-# standalone. The mechanism half of the same section (all 24 admitted, atomic_window_stalls moved,
+# standalone. The mechanism half of the same section (all 24 admitted, multiple groups observed in flight,
 # so a younger cross-key group WAS in flight while an older one decided) still runs on this tier,
 # and the measured rates are still printed in the log.
 py tests/atomic_ryow.py 127.0.0.1 $PORT --no-rate-assertions \
@@ -1345,20 +1330,9 @@ pausable g++ -std=c++20 -O2 -g -march=native -pthread -DTOMO_JEMALLOC -DTOMO_RL_
     && ok "read-local ownership-invariant build" \
     || bad "read-local ownership-invariant build" "see /tmp/gate-rlcachedbg-build.txt"
 quiet_wait
-# --shards 64 and a faster balancer tick are what make the row DETERMINISTIC rather than a coin
-# flip. The defect needs a shard to CHANGE OWNER; `launch` boots --shards 16 and this gate sizes its
-# thread count from its core set, so on a 16-CPU set every thread owns exactly one shard and the
-# balancer has nothing it can take (measured: 9.4M ops, 0 moves -- and the battery's own
-# non-vacuity check correctly refused to report a pass). A later --shards wins over the earlier
-# one, so 64 shards gives every thread several to trade whatever the core count is. The LB knobs
-# are boot-only (CONFIG SET reports "parameter is immutable at runtime"), hence here.
-#
-# ONLY the tick is lowered. Measured over 15 s at 64 shards, moves: default 6, --lb-tick-ms 100
-# -> 50, --lb-move-cap 4 -> 20, --lb-imbalance-pct 1 -> 5, and --lb-cooldown-ms 0 -> ZERO. That
-# last one reads like "no cooldown" and means "never move", which is how the first version of this
-# row silently lost its own precondition. Do not add it back.
-if boot_fused $RLDBG --shards 64 --atomic 1 --read-local 1 --enable-debug-command yes \
-        --lb-tick-ms 100; then
+# More shards than workers permit ownership movement. The automatic balancer must move shards;
+# the battery's existing positive-movement assertion remains mandatory under the derived policy.
+if boot_fused $RLDBG --shards 64 --atomic 1 --read-local 1 --enable-debug-command yes; then
   # The battery carries its own three non-vacuity checks: the armed lane served reads, the block
   # cache actually held blocks, and -- the precondition for this whole class of defect -- the load
   # balancer MOVED shards during the run.

exec
/bin/bash -lc "python3 - <<'PY'
from pathlib import Path
import subprocess, json, shutil, hashlib
root = Path.cwd()
audit = root / '.merge-inputs'
assert not audit.exists()
audit.mkdir()
base = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
lanes = {name: root.parent / ('cx-' + name) for name in ('timing', 'knobs')}
manifest = {'base': base, 'lanes': {}}
for name, lane in lanes.items():
    assert subprocess.check_output(['git', '-C', str(lane), 'rev-parse', 'HEAD'], text=True).strip() == base
    paths = subprocess.check_output(['git', '-C', str(lane), 'diff', 'HEAD', '--name-only', '-z']).decode().split('\\0')[:-1]
    manifest['lanes'][name] = {}
    patch = subprocess.check_output(['git', '-C', str(lane), 'diff', 'HEAD', '--binary'])
    (audit / (name + '.patch')).write_bytes(patch)
    for path in paths:
        source = lane / path
        if source.exists():
            dest = audit / name / path
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, dest)
            manifest['lanes'][name][path] = hashlib.sha256(dest.read_bytes()).hexdigest()
        else:
            manifest['lanes'][name][path] = None
paths = sorted(set().union(*(set(m) for m in manifest['lanes'].values())))
for path in paths:
    original = subprocess.run(['git', 'show', base + ':' + path], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if original.returncode == 0:
        dest = audit / 'base' / path
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(original.stdout)
    changed = [name for name in lanes if path in manifest['lanes'][name]]
    target = root / path
    if path in ('tests/xshard_dispatch_scale.sh', 'tests/xshard_dispatch_scale.py'):
        target.unlink(missing_ok=True)
        print('retired:', path)
    elif len(changed) == 1:
        name = changed[0]
        if manifest['lanes'][name][path] is None:
            target.unlink(missing_ok=True)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(audit / name / path, target)
    else:
        result = subprocess.run(['git', 'merge-file', '-p', '--diff3', '-L', 'cx-knobs', '-L', '78c3e5391', '-L', 'cx-timing', str(audit / 'knobs' / path), str(audit / 'base' / path), str(audit / 'timing' / path)], stdout=subprocess.PIPE)
        assert result.returncode in (0, 1, 2), result.returncode
        target.write_bytes(result.stdout)
        print('three-way:', path, 'conflict blocks:', result.returncode)
(audit / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\\n')
print('snapshotted', len(paths), 'input paths;', len(manifest['lanes']['timing']), 'timing and', len(manifest['lanes']['knobs']), 'knobs')
PY" in /home/user/Projects/cx-merge
 exited 1 in 38ms:
Traceback (most recent call last):
  File "<stdin>", line 46, in <module>
AssertionError: 3

exec
/bin/bash -lc "python3 - <<'PY'
from pathlib import Path
import subprocess, json, shutil, hashlib
root = Path.cwd()
audit = root / '.merge-inputs'
base = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
manifest = {'base': base, 'lanes': {}}
for name in ('timing', 'knobs'):
    lane = root.parent / ('cx-' + name)
    paths = subprocess.check_output(['git', '-C', str(lane), 'diff', 'HEAD', '--name-only', '-z']).decode().split('\\0')[:-1]
    manifest['lanes'][name] = {path: hashlib.sha256((audit / name / path).read_bytes()).hexdigest() if (audit / name / path).exists() else None for path in paths}
paths = sorted(set().union(*(set(m) for m in manifest['lanes'].values())))
for path in paths:
    original = subprocess.run(['git', 'show', base + ':' + path], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if original.returncode == 0:
        dest = audit / 'base' / path
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(original.stdout)
    changed = [name for name in manifest['lanes'] if path in manifest['lanes'][name]]
    target = root / path
    if path in ('tests/xshard_dispatch_scale.sh', 'tests/xshard_dispatch_scale.py'):
        target.unlink(missing_ok=True)
        print('retired:', path)
    elif len(changed) == 1:
        name = changed[0]
        if manifest['lanes'][name][path] is None:
            target.unlink(missing_ok=True)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(audit / name / path, target)
    else:
        result = subprocess.run(['git', 'merge-file', '-p', '--diff3', '-L', 'cx-knobs', '-L', '78c3e5391', '-L', 'cx-timing', str(audit / 'knobs' / path), str(audit / 'base' / path), str(audit / 'timing' / path)], stdout=subprocess.PIPE)
        assert 0 <= result.returncode < 128, result.returncode
        target.write_bytes(result.stdout)
        print('three-way:', path, 'conflict blocks:', result.returncode)
(audit / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\\n')
print('snapshotted', len(paths), 'input paths;', len(manifest['lanes']['timing']), 'timing and', len(manifest['lanes']['knobs']), 'knobs')
PY
rg -n '"'^(<<<<<<<|=======|>>>>>>>|'"\\|\\|\\|\\|\\|\\|\\|)' tests/atomic_torn.py tests/gate.sh" in /home/user/Projects/cx-merge
 succeeded in 0ms:
three-way: tests/atomic_torn.py conflict blocks: 3
three-way: tests/gate.sh conflict blocks: 1
retired: tests/xshard_dispatch_scale.py
retired: tests/xshard_dispatch_scale.sh
snapshotted 49 input paths; 12 timing and 40 knobs
tests/gate.sh:677:<<<<<<< cx-knobs
tests/gate.sh:679:||||||| 78c3e5391
tests/gate.sh:685:=======
tests/gate.sh:691:>>>>>>> cx-timing
tests/atomic_torn.py:899:<<<<<<< cx-knobs
tests/atomic_torn.py:900:||||||| 78c3e5391
tests/atomic_torn.py:913:=======
tests/atomic_torn.py:926:>>>>>>> cx-timing
tests/atomic_torn.py:931:<<<<<<< cx-knobs
tests/atomic_torn.py:933:||||||| 78c3e5391
tests/atomic_torn.py:967:=======
tests/atomic_torn.py:1005:>>>>>>> cx-timing
tests/atomic_torn.py:1007:<<<<<<< cx-knobs
tests/atomic_torn.py:1015:||||||| 78c3e5391
tests/atomic_torn.py:1058:=======
tests/atomic_torn.py:1124:>>>>>>> cx-timing

codex
Both lanes are copied into the merge worktree. The base’s four-attempt OFF RENAME control is intact, and the timing lane’s promotion checks merged around it. The overlapping admission tests need a deliberate resolution: use the knobs lane’s derived-window tests, then account for the timing lane’s shrink/reclaim checks whose configurable bound has been removed.
exec
/bin/bash -lc "sed -n '1,260p' tests/atomicwindow.py && sed -n '720,805p' tests/atomic_torn.py && sed -n '640,705p' tests/gate.sh" in /home/user/Projects/cx-merge
 succeeded in 0ms:
"""Admission witnesses at the derived production window; no window configuration override.

Arm, release the DEBUG hold, THEN demand completion. Only a clean, fully drained witness miss
can be re-armed. Per-attempt telemetry measures both the window hit and reply rates; four attempts
are a bounded discovery budget, not a claim about an unmeasured false-failure probability.
"""
import socket
import threading
import time

import _lib


ARM_ATTEMPTS = 4
ARM_SECONDS = 5
RESUME_SECONDS = 30
DRAIN_SECONDS = 5


def held_burst(host, port, *, whole_window, reconfigure=False):
    """Witness overlap or global admission; optionally rebuild credits with groups still live."""
    if reconfigure and not whole_window:
        raise ValueError("credit reconfiguration needs the global-window arm")
    admin = _lib.Conn(host, port, timeout=30)
    try:
        server = _lib.info(admin, "server")
        limit = min(16 * int(server["shards"]), 1024)
        connections = 2 * ((limit + 63) // 64) + 2 if whole_window else 1
        depth = 64 if whole_window else 24
        label = "reconfigure" if reconfigure else "window" if whole_window else "overlap"
        history = []

        def sample():
            table = _lib.info(admin, "stats")
            return {key: int(table["atomic_" + key]) for key in
                    ("groups", "inflight", "window_stalls", "credit_debt", "credit_pool")}

        def idle(table):
            return (table["inflight"] == 0 and table["credit_debt"] == 0 and
                    table["credit_pool"] == limit)

        for attempt in range(1, ARM_ATTEMPTS + 1):
            if attempt > 1:
                # The control connection's IO placement also orders CONFIG relative to writers.
                admin.close()
                admin = _lib.Conn(host, port, timeout=30)
            deadline = time.monotonic() + DRAIN_SECONDS
            while not idle(sample()):
                if time.monotonic() >= deadline:
                    raise AssertionError("atomic groups/credits did not drain before arming")
                time.sleep(0.01)
            prefix = "derived-window:%d:%d" % (time.time_ns(), attempt)
            # Every group must span owners; a same-owner fast path is not an admission witness.
            groups = connections * depth
            buckets = _lib.owner_buckets(admin, prefix, per_owner=groups, limit=groups * 64)
            enough = [keys for keys in buckets.values() if len(keys) >= groups]
            # Build before the timed arm and release all senders together. Python encoding and
            # thread startup must not spend the publication window one connection at a time.
            payloads = [b"".join(
                _lib.encode("MSET", enough[0][i], "a", enough[1][i], "b")
                for i in range(index * depth, (index + 1) * depth))
                for index in range(connections)]
            clients = []
            threads = []
            start = threading.Barrier(connections + 1, timeout=10)
            errors = []
            replies = [0] * connections
            peak = 0
            carried = 0
            stalls = 0
            held_stalls = 0
            armed = False
            released = False
            started = time.monotonic()
            arm_elapsed = 0.0
            resume_elapsed = 0.0
            resumed = None
            held_replies = 0
            release_replies = 0
            outcome = "failed"

            def run(index, client):
                try:
                    start.wait()
                    client.raw(payloads[index])
                    for _ in range(depth):
                        reply = client.read()
                        if reply != b"OK":
                            raise AssertionError("unexpected atomic burst reply: %r" % (reply,))
                        replies[index] += 1
                except Exception as exc:
                    errors.append("client%d: %s" % (index, exc))

            def observe():
                nonlocal peak, stalls
                table = sample()
                peak = max(peak, table["inflight"])
                stalls = table["window_stalls"] - before["window_stalls"]
                if table["inflight"] > limit:
                    raise AssertionError("in-flight groups exceed derived limit: %d > %d" %
                                         (table["inflight"], limit))
                if table["credit_debt"] != 0:
                    raise AssertionError("fixed window acquired credit debt: %r" % table)
                if errors:
                    raise AssertionError("atomic burst failed: %r" % errors)
                return table

            try:
                for _ in range(connections):
                    clients.append(_lib.Conn(host, port, timeout=30))
                before = sample()
                try:
                    if admin.must("DEBUG", "ATOMIC-COMMIT-DELAY", "100000") != b"OK":
                        raise AssertionError("commit hold did not arm")
                    for index, client in enumerate(clients):
                        thread = threading.Thread(target=run, args=(index, client), daemon=True)
                        thread.start()
                        threads.append(thread)
                    started = time.monotonic()
                    start.wait()
                    deadline = started + ARM_SECONDS
                    while True:
                        table = observe()
                        armed = table["inflight"] >= 2 and (stalls > 0 or not whole_window)
                        if armed and reconfigure:
                            # Same-value SET still rebuilds the credit generation (server.h).
                            # Keep atomic ON so all submitted groups remain subject to the bound.
                            if admin.must("CONFIG", "SET", "atomic", "1") != b"OK":
                                raise AssertionError("CONFIG SET atomic 1 failed")
                            after_config = observe()
                            # Read admissions AGAIN after the live sample. Subtract every possible
                            # newer group: a positive remainder proves old-generation groups were
                            # still live after CONFIG replied, not merely new post-CONFIG traffic.
                            newer = observe()["groups"] - table["groups"]
                            carried = max(0, after_config["inflight"] - newer)
                            break
                        if armed and not reconfigure:
                            break
                        if not any(thread.is_alive() for thread in threads):
                            break
                        if time.monotonic() >= deadline:
                            break
                        time.sleep(0.005)
                finally:
                    arm_elapsed = time.monotonic() - started
                    held_replies = sum(replies)
                    held_stalls = stalls
                    # This MUST precede the completion deadline. Holding every commit until all
                    # 640 gate-geometry groups finish was a timing test, not a resume witness.
                    if admin.must("DEBUG", "ATOMIC-COMMIT-DELAY", "0") != b"OK":
                        raise AssertionError("commit hold did not disarm")
                    released = True

                resumed = time.monotonic()
                release_replies = sum(replies)
                deadline = resumed + RESUME_SECONDS
                while True:
                    observe()
                    if not any(thread.is_alive() for thread in threads):
                        break
                    if time.monotonic() >= deadline:
                        raise AssertionError("atomic burst did not resume AFTER hold release: "
                                             "replies=%d/%d" % (sum(replies), groups))
                    time.sleep(0.005)
                resume_elapsed = time.monotonic() - resumed
                if errors or sum(replies) != groups:
                    raise AssertionError("atomic burst failed: replies=%d/%d errors=%r" %
                                         (sum(replies), groups, errors))
                deadline = time.monotonic() + DRAIN_SECONDS
                while True:
                    after = observe()
                    if idle(after):
                        break
                    if time.monotonic() >= deadline:
                        raise AssertionError("derived-window credits did not return: %r" % after)
                    time.sleep(0.01)
                # Admission counts can exceed replies: a prepared group may return its lease and
                # be re-prepared if the owner task queue has no room (io_loop.h). Those admissions
                # are conservatively included in `newer` above; they are not duplicate replies.
                # Do not accept a transient sample gathered only during drain as the held witness.
                witnessed = (armed and held_replies < groups and
                             (carried > 0 or not reconfigure))
                outcome = "armed" if witnessed else "clean-miss"
            finally:
                if resumed is not None and not resume_elapsed:
                    resume_elapsed = time.monotonic() - resumed
                start.abort()
                # Wake blocked readers before touching their buffered file objects. close() alone
                # can wait for the reader's lock and hide a live helper behind a nominal join bound.
                for client in clients:
                    if client.sock is not None:
                        try:
                            client.sock.shutdown(socket.SHUT_RDWR)
                        except OSError:
                            pass
                cleanup_deadline = time.monotonic() + 1
                for thread in threads:
                    thread.join(timeout=max(0, cleanup_deadline - time.monotonic()))
                stuck = any(thread.is_alive() for thread in threads)
                for client in clients:
                    if stuck:
                        client.sock.close()
                    else:
                        client.close()
                held_rate = "%.1f" % (held_replies / arm_elapsed) if arm_elapsed else "n/a"
                resume_rate = ("%.1f" % ((sum(replies) - release_replies) / resume_elapsed)
                               if resumed is not None and resume_elapsed else "n/a")
                detail = ("%s attempt=%d/%d outcome=%s limit=%d groups=%d peak=%d "
                          "held_stalls=%d stalls=%d "
                          "carried=%d held_replies=%d arm_s=%.3f held_replies/s=%s "
                          "released=%s replies=%d/%d resume_s=%.3f resume_replies/s=%s" %
                          (label, attempt, ARM_ATTEMPTS, outcome, limit, groups, peak, held_stalls, stalls,
                           carried, held_replies, arm_elapsed, held_rate, released, sum(replies),
                           groups, resume_elapsed, resume_rate))
                history.append(detail)
                print("  note atomic-window " + detail, flush=True)
                if stuck:
                    raise AssertionError("atomic burst helpers did not stop")
            if witnessed:
                return "%s credits=%d observed_attempts=1/%d" % (detail, limit, attempt)
            # Match multirace and OFF RENAME: retry ONLY a clean, joined, drained witness miss.
        raise AssertionError("%s window never observed: observed_attempts=0/%d; %s" %
                             (label, ARM_ATTEMPTS, " | ".join(history)))
    finally:
        admin.close()
note("ON exercised promotion", promo_delta > 0, "delta=%d" % promo_delta)
release_note("ON promotion drains within release budget",
             promo_delta > 0 and promotion_elapsed <= 1.5,
             "promotion-drain=%.3fs holds=%d predecessors=%d promotions=%d" %
             (promotion_elapsed, hold_delta, pred_delta, promo_delta))
note("atomic_inflight returns to idle", after.get("atomic_inflight", -1) == 0,
     "value=%d" % after.get("atomic_inflight", -1))

# Same-physical-shard write localfast is a separate atomic arm: no scatter publication window and
# no group entry. Geometry proves the shard; the counter independently proves the path fired.
local_before = stats().get("atomic_localfast", 0)
local_torn, local_reads, local_errors, _, _, local_threads_still_alive = hammer(
    "at:local", 1, seconds=2.0, writers=2, readers=4, keys=local_pair)
local_after = stats().get("atomic_localfast", 0)
note("same-owner atomic MSET-2 localfast is torn-free",
     local_torn == 0 and local_reads > 0 and not local_errors and
     not local_threads_still_alive and local_after > local_before,
     "keys=%r torn=%d reads=%d localfast=%d errors=%r threads_still_alive=%r" %
     (local_pair, local_torn, local_reads, local_after - local_before, local_errors,
      local_threads_still_alive))

# Two overlapping atomic writers on exactly the same key set. The final state after the cleanup
# opportunity supplied by the last MGET must be one complete group, never the physical inversion.
ov_torn, ov_reads, ov_errors, ov_final, ov_completed, ov_threads_still_alive = hammer(
    "at:overlap", 1, seconds=2.5, writers=2, readers=5, keys=wide_keys)
note("overlapping atomic writers never mix",
     ov_torn == 0 and ov_reads > 0 and not ov_errors and not ov_threads_still_alive,
     "torn=%d reads=%d errors=%r threads_still_alive=%r" %
     (ov_torn, ov_reads, ov_errors, ov_threads_still_alive))
note("promotion leaves one exact final group",
     not ov_threads_still_alive and ov_final not in (None, b"TORN") and
     ov_final in ov_completed,
     "final=%r completed=%d threads_still_alive=%r" %
     (ov_final, len(ov_completed), ov_threads_still_alive))

# Broadened movers: RENAME publishes source and destination on distinct owners. Widen exactly that
# OFF mutation wave.
#
# This control is probabilistic in the same way the SINTERSTORE control below is, so it re-rolls the
# same way: only while the run came back CLEAN and every helper stopped, keeping the last real
# result. It does NOT degrade to a skip. This control's entire job is to prove the detector can see
# a tear, so a genuine miss on every roll stays a FAILURE -- that is strictly stronger than the
# skip-on-clean policy used by the SINTERSTORE, COPY and RENAMENX controls.
#
# Measured 2026-09-07 at the gate's geometry (--shards 16 --ratio 6:2, cores 0-7): the outcome is
# almost binary rather than marginal. When the wave lands, ~75,900 of ~76,000 reads are torn; when
# it does not, exactly 0 of ~70,000 are, which is the signature of the hop-delay not taking effect
# for that run rather than of a race narrowly lost. 5 of 6 runs landed it, so four rolls leave a
# residual around 1 in 1,300. To induce the real failure this row protects: make the OFF path
# publish both owners atomically and every roll reports invalid=0.
rename_off = None
for _roll in range(4):
    debug("ATOMIC-OFF-HOP-DELAY", 100000)
    try:
        rename_off = rename_hammer("at:rename-off", 0, mover_pair, seconds=1.0)
    finally:
        debug("ATOMIC-OFF-HOP-DELAY", 0)
    if rename_off[0] > 0 or rename_off[2] or rename_off[4]:
        break
rename_on = None
if not rename_off[2] and not rename_off[4]:
    rename_on = rename_hammer("at:rename-on", 1, mover_pair, seconds=2.0)
note("OFF control exposes torn RENAME",
     rename_off[0] > 0 and rename_off[1] > 0 and not rename_off[2] and not rename_off[4],
     "invalid=%d reads=%d errors=%r threads_still_alive=%r" %
     (rename_off[0], rename_off[1], rename_off[2], rename_off[4]))
if rename_on is None:
    skip("ON RENAME/MGET has exactly one live image", "OFF discovery did not complete cleanly")
else:
    note("ON RENAME/MGET has exactly one live image",
         rename_on[0] == 0 and rename_on[1] > 0 and not rename_on[2] and
         rename_on[3] and not rename_on[4],
         "invalid=%d reads=%d final=%r errors=%r threads_still_alive=%r" %
         (rename_on[0], rename_on[1], rename_on[3], rename_on[2], rename_on[4]))

# Store-family cut consistency. The member moves atomically between two sources, so every valid
# cut has intersection {base}; observing "moving" in the stored result proves a mixed source cut.
store_pair = mover_pair
# The OFF control is probabilistic: preserve the last real result and re-roll only while it is
# clean and every helper stopped. A genuine four-roll miss remains a failure.
store_off = None
for _roll in range(4):
    store_off = sinterstore_hammer("at:store-off", 0, store_pair, seconds=2.5)
    if store_off[0] > 0 or store_off[2] or store_off[4]:
        break
store_on = None
  # a "fix" that merely hid the group's candidate would fail there.
  for t in lbsignals slowlog atomfix scriptatomic execatomic execiso execfix multires multirace session_monotonic xacct xmove xscript; do
    py tests/$t.py 127.0.0.1 $PORT >/tmp/gate-$t-$AT.txt 2>&1 \
        && ok "$t battery (atomic $AT)" || bad "$t battery (atomic $AT)" "see /tmp/gate-$t-$AT.txt"
  done
  stop
done

# ---- cross-owner script bounds: production auto budgets and four cut slots per IO ----------
for XS_MODE in limit window; do
  boot ./build/tomokv --atomic 1 --enable-debug-command yes \
      || bad "xscript $XS_MODE control boot"
  py tests/xscript.py 127.0.0.1 $PORT "$XS_MODE" >/tmp/gate-xscript-$XS_MODE.txt 2>&1 \
      && ok "xscript $XS_MODE control" || bad "xscript $XS_MODE control" \
             "see /tmp/gate-xscript-$XS_MODE.txt"
  stop
done

# ---- efficiency guards: each needs a boot geometry of its own, so each gets its own server ----
# Exact mechanism checks run on every build; timing budgets require the explicit release-build
# flag. A ratio of wall times is still a performance claim (see DESIGN-GATEHYGIENE.md). The
# cross-shard guard also checks instrument stability before scoring any timing, retaining all pairs.
# --shards 1 for the same reason as the borrow guard: the sidecar under test is per shard, and the
# battery asserts that precondition rather than quietly measuring a diluted one.
boot ./build/tomokv --shards 1 --enable-debug-command yes || bad "expire-index guard boot"
py tests/expireindex.py 127.0.0.1 $PORT --release-build >/tmp/gate-expireindex.txt 2>&1 \
    && ok "expire-index growth bound" || bad "expire-index growth bound" "see /tmp/gate-expireindex.txt"
stop

# one shard so every borrow lands in ONE registry (the quantity under test), and a small zc-min so
# an ordinary-sized value still takes the borrow path and pays registry cost.
boot ./build/tomokv --shards 1 --zc-min 64 --client-output-buffer-limit "normal 0 0 0" \
    --enable-debug-command yes || bad "borrow-registry guard boot"
py tests/borrow_registry.py 127.0.0.1 $PORT --release-build >/tmp/gate-borrow.txt 2>&1 \
    && ok "borrow-registry growth bound" || bad "borrow-registry growth bound" "see /tmp/gate-borrow.txt"
stop

<<<<<<< cx-knobs
# Dispatch scaling's manual ownership geometry was retired with shard-home; DESIGN-KNOBS.md.
||||||| 78c3e5391
# this one boots its own arms at two thread counts, so it takes the port/cores rather than a server
quiet_wait
XDS_PORT=$PORT XDS_CPUS=$CORES XDS_BIN=./build/tomokv bash tests/xshard_dispatch_scale.sh \
    >/tmp/gate-xds.txt 2>&1 \
    && ok "cross-shard dispatch scaling" || bad "cross-shard dispatch scaling" "see /tmp/gate-xds.txt"
=======
# this one boots its own arms at two thread counts, so it takes the port/cores rather than a server
quiet_wait
XDS_PORT=$PORT XDS_CPUS=$CORES XDS_BIN=./build/tomokv bash tests/xshard_dispatch_scale.sh --release-build \
    >/tmp/gate-xds.txt 2>&1 \
    && ok "cross-shard dispatch scaling" || bad "cross-shard dispatch scaling" "see /tmp/gate-xds.txt"
>>>>>>> cx-timing

# ---- Redis-wire DUMP/RESTORE survives the native snapshot/restart boundary -------------------
DUMPRESTORE_DIR=$(mktemp -d /tmp/gate-dumprestore.XXXXXX)
boot ./build/tomokv --atomic 1 --dir "$DUMPRESTORE_DIR" --dbfilename dumprestore.tomo \
    || bad "DUMP/RESTORE restart preparation boot"
py tests/dumprestore.py 127.0.0.1 $PORT prepare_restart \
    >/tmp/gate-dumprestore-restart.txt 2>&1 \
    && ok "DUMP/RESTORE prepare + native SAVE" \
    || bad "DUMP/RESTORE restart preparation" "see /tmp/gate-dumprestore-restart.txt"
stop
boot ./build/tomokv --atomic 1 --dir "$DUMPRESTORE_DIR" \
    --load "$DUMPRESTORE_DIR/dumprestore.tomo" \
    || bad "DUMP/RESTORE snapshot reload boot"
py tests/dumprestore.py 127.0.0.1 $PORT verify_restart \

exec
/bin/bash -lc 'git diff -- src/core/ex_loop.h src/core/flatstore.h src/store/flatstore.h src/core/shard.h src/core/weighted_lb.h src/core/flipctl.cc src/core/placement.h' in /home/user/Projects/cx-knobs
 succeeded in 0ms:
diff --git a/src/core/ex_loop.h b/src/core/ex_loop.h
index 296e3fe56..72a3bcd9a 100644
--- a/src/core/ex_loop.h
+++ b/src/core/ex_loop.h
@@ -134,14 +134,13 @@ struct ReadLocalExState<true> {
         bool lane_has_tombstones = false;
         bool point_writes_precise = true;
         bool keymiss_notify_armed = false;
-        bool interleave_owner_tasks = true;
-        bool prefetch_capture = true;
+        uint8_t reserved_schedule[2] = {}; // preserve the demotion-state offsets
         // EFFECTIVE lane capacity for ADMISSION. The physical ring is always kInboxSlots entries
         // and its index arithmetic still masks with kInboxSlots - 1; this only decides when the
         // parser stops admitting, so lowering it can never overrun the ring. It derives to
         // kInboxSlots and stays there unless DEBUG READ-LOCAL-LANE-CAP sets a test value, which
         // fused_pass_impl copies here once per rotation. It occupies the two padding bytes that
-        // already sat at offset 38 between prefetch_capture and demote_context, on the same first
+        // already sat at offset 38 between reserved_schedule and demote_context, on the same first
         // cache line as lane_head/lane_tail/lane_count that every admission test already reads, so
         // the struct does not grow, nothing moves, and reading it costs no line the caller did not
         // already own. See P128.md section 8.
@@ -181,9 +180,8 @@ public:
     bool init(Server* srv, ThreadCtx* self, bool dormant = false) {
         srv_ = srv; self_ = self;
         aof_manager_ = srv->aof().configured() ? &srv->aof() : nullptr;
-        lru_clock_shift_ = static_cast<uint8_t>(srv->cfg().lru_clock_shift);
         foreign_touch_random_ ^= (static_cast<uint64_t>(self->id()) + 1) * 0x9e3779b97f4a7c15ULL;
-        lb_sample_rate_ = srv->key_lb_signals_enabled() ? srv->cfg().lb_sample_rate : 0;
+        lb_sample_rate_ = srv->key_lb_signals_enabled() ? srv->lb_sample_rate() : 0;
         lb_sample_countdown_ = lb_sample_rate_;
         lb_controller_armed_ = srv->key_lb_signals_enabled();
         age_sample_rate_cached_ = srv->effective_age_sample_rate();
@@ -205,8 +203,6 @@ public:
                 impl->point_writes_precise =
                     srv->cfg().maxmemory == 0 ||
                     srv->cfg().maxmemory_policy == MaxmemoryPolicy::NoEviction;
-                impl->interleave_owner_tasks = srv->cfg().read_local_interleave != 0;
-                impl->prefetch_capture = srv->cfg().read_local_prefetch_capture != 0;
                 read_local_.impl = std::move(impl);
             }
         }
@@ -244,8 +240,7 @@ public:
             shard->bind_notify_pending(&notify_keyless_pending_);
             if (read_local_enabled())
                 shard->store().configure_read_local(
-                    true, *read_local_impl().deferred.sink(),
-                    srv_->cfg().read_local_atomic_filter != 0);
+                    true, *read_local_impl().deferred.sink());
         }
     }
 
@@ -470,7 +465,7 @@ public:
         if (!lb_frozen) refresh_live_config();
         if (maxmemory_enabled_)
             cached_lru_clock_ = static_cast<uint8_t>(
-                (static_cast<uint64_t>(cached_now_ms_ / 1000) >> lru_clock_shift_) & 0x1f);
+                (static_cast<uint64_t>(cached_now_ms_ / 1000) >> kLruClockShift) & 0x1f);
 
         // The WB batch captured its AOF gate before entering this call. Only a clean fresh-task
         // turn may put that WB work into an EX prefetch gap: executing older retry/deferred debt
@@ -714,7 +709,7 @@ public:
             if (!placement_frozen) refresh_live_config();
             if (maxmemory_enabled_)
                 cached_lru_clock_ = static_cast<uint8_t>(
-                    (static_cast<uint64_t>(cached_now_ms_ / 1000) >> lru_clock_shift_) & 0x1f);
+                    (static_cast<uint64_t>(cached_now_ms_ / 1000) >> kLruClockShift) & 0x1f);
             sig.iterations++;
 
             uint32_t did = 0;
@@ -842,7 +837,7 @@ private:
 
     bool read_local_interleave_enabled() const {
         if constexpr (Fused)
-            return read_local_.impl && read_local_.impl->interleave_owner_tasks;
+            return read_local_.impl != nullptr;
         return false;
     }
 
@@ -1010,7 +1005,7 @@ private:
     // reply is private. Atomic exchanges still bracket the table word for point GET, but an exchange
     // of an unrelated key cannot move this key's epoch. Each probe retains its stable-topology
     // handshake; immutable replacement plus rotation QSBR then makes a second per-key table-word
-    // validation redundant. With the filter OFF (the A/B control), or above the bounded route cache,
+    // validation redundant. Above the bounded route cache,
     // every shard keeps the legacy generation rule and per-key validation.
     struct LocalMgetWindow {
         static constexpr uint32_t kMaxShards = 256;
@@ -1028,7 +1023,7 @@ private:
     ReadLocalFallbackReason local_mget_window_open(
             LocalMgetWindow& window, const uint64_t* touched, const uint64_t* hashes,
             const int32_t* shards, uint32_t key_count, bool cached_routes) const {
-        window.use_epochs = cached_routes && srv_->cfg().read_local_atomic_filter != 0;
+        window.use_epochs = cached_routes;
         for (uint32_t sid = 0; sid < srv_->nshards(); sid++) {
             if (!local_mget_touched(touched, sid)) continue;
             const uint64_t state = srv_->shard(static_cast<int32_t>(sid))
@@ -1126,7 +1121,7 @@ private:
     // (KvObj::touch_eviction_meta_foreign) rather than the owner's load/store pair, so a lost race
     // costs one approximate touch -- which LRU and LFU tolerate by construction -- and can never
     // corrupt a layout bit. Store-rare by construction: LRU writes only when the key's bucket is
-    // stale (once per 1<<lru-clock-shift seconds per key, 256 s by default) and LFU only when the
+    // stale (once per 256 seconds per key) and LFU only when the
     // increment actually fires, so the foreign RFO into the owner's line is the exception rather
     // than the per-read rule.
     __attribute__((noinline, cold))
@@ -1565,9 +1560,7 @@ private:
             return 0;
         } else {
             if (!read_local_enabled() || !op_budget) return 0;
-            return read_local_impl().prefetch_capture
-                ? drain_local_reads_bounded_impl<true, YieldToOwner>(op_budget)
-                : drain_local_reads_bounded_impl<false, YieldToOwner>(op_budget);
+            return drain_local_reads_bounded_impl<true, YieldToOwner>(op_budget);
         }
     }
 
@@ -3010,12 +3003,17 @@ private:
         if (!lb_sample_rate_) return;
         if (--lb_sample_countdown_ != 0) return;
         lb_sample_countdown_ = lb_sample_rate_;
-        shard.note_lb_sample(hash);
+        shard.note_lb_sample(hash, lb_sample_rate_);
     }
 
     void lb_bucket_bytes_pass() {
         if (!lb_sample_rate_ || cached_now_ms_ < lb_bytes_next_ms_) return;
         lb_bytes_next_ms_ = cached_now_ms_ + 10;
+        const uint32_t rate = srv_->lb_sample_rate();
+        if (rate != lb_sample_rate_) {
+            lb_sample_rate_ = rate;
+            lb_sample_countdown_ = rate;
+        }
         auto& owned = self_->shards();
         if (owned.empty()) return;
         if (lb_bytes_shard_cursor_ >= owned.size()) lb_bytes_shard_cursor_ = 0;
@@ -3337,7 +3335,6 @@ private:
     bool       maxmemory_enabled_ = false;
     bool       ex_sched_enabled_ = false;
     uint8_t    cached_lru_clock_ = 0;
-    uint8_t    lru_clock_shift_ = 8;   // latched from cfg at loop start; 1<<N seconds per bucket
     uint32_t   lb_sample_rate_ = 0;
     uint32_t   lb_sample_countdown_ = 0;
     uint32_t   age_sample_rate_cached_ = 0;
diff --git a/src/core/flipctl.cc b/src/core/flipctl.cc
index 5526ee1d5..803bce527 100644
--- a/src/core/flipctl.cc
+++ b/src/core/flipctl.cc
@@ -13,7 +13,7 @@ namespace {
 // A real workload must not wait forever merely because its stationary noise never presents a
 // quiet absolute-rate band. Thirty seconds is long enough to reject ordinary connection ramps but
 // short enough to make automatic placement useful during a container/cell warm-up. The gate below
-// converts this duration to controller ticks, so --lb-tick-ms keeps the same wall-clock bound.
+// converts this duration to controller ticks, so the internal tick preserves the same wall-clock bound.
 constexpr uint64_t kBootMaxDeferralMs = 30'000;
 
 // Anchored drift is deliberately much slower than both the half-weight maneuver EWMA and the
@@ -131,7 +131,8 @@ void FlipShiftDetector::update_band() {
     const double quantum =
         1.0 / std::sqrt(static_cast<double>(std::max<uint64_t>(smoothed_.commands, 1)));
     // A TYPED BAND IS A FLOOR, NOT A CEILING, AND IT CANNOT BUY RESOLUTION THE SIGNAL HAS NOT GOT.
-    // --flip-auto-band says how large a mix change is worth a maneuver; it says nothing about how
+    // The internal detector's explicit-band unit-test control sets the mix change worth a maneuver;
+    // it says nothing about how
     // still the signature holds, and it used to return here consulting neither the estimator's
     // resolution nor the signal's measured movement. Measured by the gate-hygiene lane on a driver
     // whose rate held to 0.07% across 34 samples: a fingerprint distance of 0.2518 against a flat
@@ -356,6 +357,12 @@ bool FlipController::sample_rate(Server& server, uint64_t now_ms, double& rate)
     rate_window_ms_ = now_ms;
     rate_window_commands_ = commands;
     rate_window_movement_ = movement;
+    // The maneuver alone arms stamps. Derive their rate over the same three-tick decision
+    // window; keep this computation behind the armed check, including its clock/window inputs.
+    if (signal_sample_rate_.load(std::memory_order_relaxed))
+        signal_sample_rate_.store(LbAutotune::sample_every(
+            completed, elapsed_ms, server.flipctl_tick_ms() * kSettleTicks),
+            std::memory_order_release);
     // Every rate sample, selected or not, feeds the long-window noise estimate; a tick at the
     // near-idle floor (one command per thread per tick, the boot gate's own idle line) is not load.
     {
@@ -571,11 +578,11 @@ void FlipController::start_maneuver(Server& server, FlipctlTriggerReason reason,
     anchor_learning_rate_samples_ = 0;
     retrigger_after_flip_ = false;
     pending_retrigger_ = FlipctlTriggerReason::None;
-    // An explicit age rate remains the maneuver rate. Zero means automatic here, derived from the
-    // provisioned pool rather than a machine constant. Every owner applies this value to itself.
+    // Derive the initial maneuver rate from the last traffic observation, then adapt it on
+    // each completed rate window. Idle and anchored states leave stamping off entirely.
     signal_sample_rate_.store(
-        server.cfg().lb_age_sample_rate ? server.cfg().lb_age_sample_rate
-                                        : std::max<uint32_t>(1, server.nthreads()),
+        LbAutotune::sample_every(rate_ew_.prev1 > 0 ? rate_ew_.prev1 : boot_rate_ewma_,
+                                1000, server.flipctl_tick_ms() * kSettleTicks),
         std::memory_order_release);
     for (uint32_t tid = 0; tid < server.nthreads(); tid++) {
         const LoopSignals& signal = server.thread(tid).sig();
@@ -735,7 +742,7 @@ bool FlipController::sample_role_demand(Server& server, uint64_t now_ms, double&
 
 // The spread the ORIGIN split's own stabilized readings showed while the model was deciding: the
 // baseline's movement between the two windows the outcome loop compares. It is a floor under every
-// band this maneuver uses, an operator-typed --flip-auto-band included, because that knob says how
+// band this maneuver uses, the detector's explicit-band unit-test control included: it says how
 // small a gain is worth chasing, not how still the workload is holding.
 double FlipController::baseline_band() const {
     return origin_window_.samples >= 2 ? origin_window_.bracket_band() : 0;
@@ -743,7 +750,8 @@ double FlipController::baseline_band() const {
 
 // The throughput noise a projected gain has to beat before a flip could be VERIFIED: the band the
 // last anchor learned, if any, or the maneuver's own stabilized-pair jitter, and never below what
-// the baseline itself moved. A typed --flip-auto-band replaces the learned pair, not the floor.
+// the baseline itself moved. The internal explicit-band unit-test control replaces the learned pair,
+// not the floor.
 double FlipController::verification_band(double rate) const {
     const double floor = std::max(baseline_band(), 2.0 * rate_ew_.sigma());
     if (configured_band_ > 0)
@@ -769,7 +777,7 @@ bool FlipController::decide_placement(Server& server, uint32_t coordinator, uint
     model_io_headroom_ = io_headroom;
     model_ex_headroom_ = ex_headroom;
 
-    const uint32_t unit = server.cfg().smt_mode ? 2u : 1u;
+    const uint32_t unit = server.smt_units_enabled() ? 2u : 1u;
     const uint32_t total_units = server.nthreads() / unit;
     const uint32_t now_units = server.role_count(Role::Ifid) / unit;
     const double band = verification_band(rate);
@@ -1036,7 +1044,7 @@ void FlipController::anchor(Server& server, double rate) {
     // The rejected alternative was widening that detector's band by twice the excursion it just
     // proved uninformative. Measured: a paced 8-key -> single-key change scores distance 0.630 on
     // a metric whose maximum is 1.0, so one null result set a floor of 1.26 and the fingerprint
-    // detector could never fire again at any workload -- a self-inflicted `--flip-auto-band 0`.
+    // detector could never fire again at any workload -- an accidentally disabled trigger.
     // A threshold learned from an excursion can exceed every excursion; a window cannot.
     const bool null_maneuver = current == maneuver_origin_io_ &&
         (last_trigger_ == FlipctlTriggerReason::FingerprintShift ||
diff --git a/src/core/placement.h b/src/core/placement.h
index a89dee6d5..2ac89cb73 100644
--- a/src/core/placement.h
+++ b/src/core/placement.h
@@ -1,7 +1,7 @@
 // placement.h — per-thread role/cpu placement and the shard migration contract.
 //
-// A LOGICAL THREAD IS THE DEFAULT LOCALITY UNIT. With smt-mode=1, Linux's reported sibling pair is
-// the scheduling unit instead: both dense thread ids always have one role and FLIP moves them
+// A LOGICAL THREAD IS THE DEFAULT LOCALITY UNIT. When the allowed CPU set contains sibling pairs,
+// Linux's reported pair is the scheduling unit: both dense thread ids have one role and FLIP moves them
 // together. There is deliberately no node layer between a thread and its cpu/L3 facts.
 
 #pragma once
@@ -172,56 +172,12 @@ public:
         return true;
     }
 
-    // Shard ownership is a flat shard->EX tid table. A manual map must be complete because mixing
-    // an accidental omission with defaults would make a supposedly controlled placement measure a
-    // different layout. Runtime dispatch still reads Router's bucket owner exactly once.
-    bool assign_shard_homes(uint32_t nshards, const char* spec) {
-        shard_home_.assign(nshards, kNoThread);
-        if (!spec) {
-            for (uint32_t sid = 0; sid < nshards; sid++)
-                shard_home_[sid] = ex_[sid % ex_.size()];
-            return true;
-        }
-        if (!*spec) {
-            std::fprintf(stderr, "--shard-home must contain shard:thread pairs\n");
-            return false;
-        }
-
-        std::vector<bool> seen(nshards, false);
-        const char* p = spec;
-        while (*p) {
-            uint32_t sid = 0, tid = 0;
-            if (!parse_pair(p, sid, tid)) {
-                std::fprintf(stderr, "--shard-home: expected shard:thread pairs near '%s'\n", p);
-                return false;
-            }
-            if (sid >= nshards) {
-                std::fprintf(stderr, "--shard-home: shard %u is outside 0..%u\n", sid, nshards - 1);
-                return false;
-            }
-            if (tid >= threads_.size() || !is_executor(tid)) {
-                std::fprintf(stderr, "--shard-home: thread %u is not an ex thread\n", tid);
-                return false;
-            }
-            if (seen[sid]) {
-                std::fprintf(stderr, "--shard-home: shard %u is assigned more than once\n", sid);
-                return false;
-            }
-            seen[sid] = true;
-            shard_home_[sid] = tid;
-            if (*p == '\0') break;
-            p++;
-            if (!*p) {
-                std::fprintf(stderr, "--shard-home: trailing comma\n");
-                return false;
-            }
-        }
-        for (uint32_t sid = 0; sid < nshards; sid++) {
-            if (!seen[sid]) {
-                std::fprintf(stderr, "--shard-home: shard %u has no owner (manual maps must be complete)\n", sid);
-                return false;
-            }
-        }
+    // Boot ownership is always round-robin over the resolved executor set.
+    bool assign_shard_homes(uint32_t nshards) {
+        if (ex_.empty()) return false;
+        shard_home_.resize(nshards);
+        for (uint32_t sid = 0; sid < nshards; sid++)
+            shard_home_[sid] = ex_[sid % ex_.size()];
         return true;
     }
 
@@ -247,7 +203,7 @@ public:
             const int cpu = threads_[tid].cpu;
             if (cpu < 0 || cpu >= CPU_SETSIZE || cpu_to_tid[cpu] != kNoThread) {
                 std::fprintf(stderr,
-                             "--smt-mode: cpu %d is not a unique provisioned logical cpu\n", cpu);
+                             "SMT placement: cpu %d is not a unique provisioned logical cpu\n", cpu);
                 return false;
             }
             cpu_to_tid[cpu] = tid;
@@ -258,7 +214,7 @@ public:
             if (siblings.size() != 2 ||
                 std::find(siblings.begin(), siblings.end(), cpu) == siblings.end()) {
                 std::fprintf(stderr,
-                             "--smt-mode: cpu %d does not have one sysfs sibling\n", cpu);
+                             "SMT placement: cpu %d does not have one sysfs sibling\n", cpu);
                 return false;
             }
             const int peer_cpu = siblings[0] == cpu ? siblings[1] : siblings[0];
@@ -266,7 +222,7 @@ public:
                 ? cpu_to_tid[peer_cpu] : kNoThread;
             if (peer == kNoThread) {
                 std::fprintf(stderr,
-                             "--smt-mode: cpu %d requires sibling cpu %d in the placement\n",
+                             "SMT placement: cpu %d requires sibling cpu %d in the placement\n",
                              cpu, peer_cpu);
                 return false;
             }
@@ -274,13 +230,13 @@ public:
             if (reverse.size() != 2 ||
                 std::find(reverse.begin(), reverse.end(), cpu) == reverse.end()) {
                 std::fprintf(stderr,
-                             "--smt-mode: cpu %d and cpu %d are not a reciprocal sysfs pair\n",
+                             "SMT placement: cpu %d and cpu %d are not a reciprocal sysfs pair\n",
                              cpu, peer_cpu);
                 return false;
             }
             if (threads_[peer].role != threads_[tid].role) {
                 std::fprintf(stderr,
-                             "--smt-mode: sibling cpus %d and %d have different roles\n",
+                             "SMT placement: sibling cpus %d and %d have different roles\n",
                              cpu, peer_cpu);
                 return false;
             }
@@ -330,7 +286,7 @@ private:
     bool build_even_smt(const Topology& topo, uint32_t n_ifid, uint32_t n_ex) {
         if ((n_ifid & 1u) || (n_ex & 1u)) {
             std::fprintf(stderr,
-                         "--ratio: --smt-mode requires even logical io and ex counts\n");
+                         "--ratio: SMT placement requires even logical io and ex counts\n");
             return false;
         }
         const uint32_t nd = topo.ndomains() ? topo.ndomains() : 1;
@@ -344,7 +300,7 @@ private:
                 if (siblings.size() != 2 ||
                     std::find(siblings.begin(), siblings.end(), cpu) == siblings.end()) {
                     std::fprintf(stderr,
-                                 "--smt-mode: cpu %d does not have one sysfs sibling\n", cpu);
+                                 "SMT placement: cpu %d does not have one sysfs sibling\n", cpu);
                     return false;
                 }
                 const int peer = siblings[0] == cpu ? siblings[1] : siblings[0];
@@ -352,7 +308,7 @@ private:
                 if (peer_domain == kNoDomain) continue; // incomplete affinity unit is unavailable
                 if (peer_domain != d) {
                     std::fprintf(stderr,
-                                 "--smt-mode: sibling cpus %d and %d span L3 domains\n",
+                                 "SMT placement: sibling cpus %d and %d span L3 domains\n",
                                  cpu, peer);
                     return false;
                 }
@@ -360,12 +316,12 @@ private:
                 if (reverse.size() != 2 ||
                     std::find(reverse.begin(), reverse.end(), cpu) == reverse.end()) {
                     std::fprintf(stderr,
-                                 "--smt-mode: cpu %d and cpu %d are not a reciprocal sysfs pair\n",
+                                 "SMT placement: cpu %d and cpu %d are not a reciprocal sysfs pair\n",
                                  cpu, peer);
                     return false;
                 }
                 if (used[peer]) {
-                    std::fprintf(stderr, "--smt-mode: cpu %d belongs to two sibling units\n", peer);
+                    std::fprintf(stderr, "SMT placement: cpu %d belongs to two sibling units\n", peer);
                     return false;
                 }
                 used[cpu] = used[peer] = true;
diff --git a/src/core/shard.h b/src/core/shard.h
index d9d93467a..694b9a93c 100644
--- a/src/core/shard.h
+++ b/src/core/shard.h
@@ -339,10 +339,10 @@ public:
             return false;
         }
     }
-    void note_lb_sample(uint64_t hash) {
+    void note_lb_sample(uint64_t hash, uint32_t rate) {
         const uint32_t bucket = bucket_of(hash);
         if (!lb_bucket_samples_ || !owns(bucket)) return;
-        lb_bucket_samples_[bucket - bucket_begin_]++;
+        lb_bucket_samples_[bucket - bucket_begin_] += rate;
     }
     uint32_t lb_bucket_samples(uint32_t bucket) const {
         if (!lb_bucket_samples_ || !owns(bucket)) return 0;
@@ -503,7 +503,7 @@ private:
     Op* notify_source_ = nullptr;
     bool* notify_pending_ = nullptr;
     std::unique_ptr<NotifyShardState> notify_state_;
-    // Allocated only when lb-sample-rate is nonzero. Appended in the cold tail so the hot shard
+    // Allocated only when lb is enabled. Appended in the cold tail so the hot shard
     // header and FlatStore offsets remain unchanged when weighted placement is compiled in.
     std::unique_ptr<uint32_t[]> lb_bucket_samples_;
     std::unique_ptr<uint64_t[]> lb_bucket_bytes_;
diff --git a/src/core/weighted_lb.h b/src/core/weighted_lb.h
index d702f1ec0..127de4377 100644
--- a/src/core/weighted_lb.h
+++ b/src/core/weighted_lb.h
@@ -7,6 +7,7 @@
 // only break exact ties. This is deterministic, allocation-only code and never runs on a request
 // path.
 #pragma once
+#include <atomic>
 
 #include <algorithm>
 #include <cmath>
@@ -15,6 +16,84 @@
 
 namespace tomo {
 
+// Internal LB policy, allocated only by --lb 1. One decision spans the three sustained
+// controller ticks; sampling targets a fixed number of key visits over that whole window.
+// Executors latch the rate on their existing census beat and weight each sample by that rate,
+// so a rate change or owner migration cannot reinterpret counters collected at another rate.
+struct LbAutotune {
+    static constexpr uint32_t kTickMs = 1000;
+    static constexpr uint32_t kDecisionTicks = 3;
+    static constexpr uint32_t kWindowMs = kTickMs * kDecisionTicks;
+    static constexpr uint32_t kSamplesPerDecision = 4096;
+    static constexpr uint64_t kMoveTimeoutNs = 5ull * 1000 * 1000 * 1000;
+
+    std::atomic<uint32_t> sample_rate{1}; // bootstrap until the first completed traffic window
+    uint64_t last_fold_ns = 0;           // protected by Server::lb_signal_mu_
+
+    static uint32_t sample_every(double visits, double elapsed_ms, uint32_t window_ms) {
+        if (!(elapsed_ms > 0)) return 1;
+        const double rate = std::ceil(visits * window_ms / elapsed_ms / kSamplesPerDecision);
+        return static_cast<uint32_t>(std::clamp(rate, 1.0, double(UINT32_MAX)));
+    }
+
+    void observe_visits(uint64_t visits, uint64_t now) {
+        if (last_fold_ns && now > last_fold_ns) {
+            sample_rate.store(sample_every(visits, double(now - last_fold_ns) / 1000000.0,
+                                           kWindowMs),
+                              std::memory_order_relaxed);
+        }
+        last_fold_ns = now;
+    }
+
+    struct QuietJitter {
+        double previous = 0;
+        double jitter = 0;
+        uint32_t windows = 0;
+
+        bool observe(double value) {
+            const double delta = std::abs(value - previous);
+            // Learn adjacent-window jitter before making a decision. Once learned, excursions
+            // cannot widen their own admission band: only quiet, in-band changes update it.
+            if (windows && (windows <= kDecisionTicks || delta <= 2 * jitter))
+                jitter = windows == 1 ? delta : 0.25 * delta + 0.75 * jitter;
+            previous = value;
+            windows = std::min(windows + 1, kDecisionTicks + 1);
+            return windows > kDecisionTicks;
+        }
+        double band() const { return 2 * jitter; }
+    };
+    QuietJitter key_jitter, client_jitter; // controller writer only
+
+    // Completion records include drain and install, not just the pointer exchange. These two
+    // atomics are cold: one writer per completed movement, never an operation-path timestamp.
+    std::atomic<uint64_t> transfer_ns{0};
+    std::atomic<uint64_t> transfers{0};
+    void note_transfer(uint64_t elapsed_ns, uint32_t count) {
+        if (!count) return;
+        transfer_ns.fetch_add(std::max<uint64_t>(elapsed_ns, 1), std::memory_order_relaxed);
+        transfers.fetch_add(count, std::memory_order_release);
+    }
+    uint64_t move_cost_ns() const {
+        const uint64_t count = transfers.load(std::memory_order_acquire);
+        return count ? std::max<uint64_t>(1, transfer_ns.load(std::memory_order_relaxed) / count)
+                     : 0;
+    }
+    uint32_t move_cap(uint32_t candidates) const {
+        const uint64_t cost = move_cost_ns();
+        // Bootstrap with one move. Thereafter admit at most one decision tick's share of the
+        // measured transfer capacity, amortized across the sustained decision window.
+        const uint64_t cap = cost ? (uint64_t{kTickMs} * 1000000 / cost) / kDecisionTicks : 1;
+        return static_cast<uint32_t>(std::min<uint64_t>(candidates, std::max<uint64_t>(1, cap)));
+    }
+    uint64_t cooldown_ms() const {
+        // Round up to the observation cadence; zero measured cost never disables movement.
+        const uint64_t cost = move_cost_ns();
+        const uint64_t ticks = (cost * kDecisionTicks + uint64_t{kTickMs} * 1000000 - 1) /
+                               (uint64_t{kTickMs} * 1000000);
+        return std::max<uint64_t>(1, ticks) * kTickMs;
+    }
+};
+
 struct WeightedLbItem {
     uint64_t id = 0;
     uint32_t owner = 0;
diff --git a/src/store/flatstore.h b/src/store/flatstore.h
index 8adf7e410..a1ba93699 100644
--- a/src/store/flatstore.h
+++ b/src/store/flatstore.h
@@ -738,8 +738,7 @@ public:
 
     // Enabled is boot-latched. The sink may be rebound only at a quiesced fused ownership handoff;
     // false keeps the old store path and every installed writer hook predicted cold.
-    void configure_read_local(bool enabled, ReadLocalRetireSink sink,
-                              bool atomic_filter = true) {
+    void configure_read_local(bool enabled, ReadLocalRetireSink sink) {
         if (enabled && !sink.defer) std::abort();
 #if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
         if (enabled && !sink.diagnostics()) std::abort();
@@ -750,10 +749,6 @@ public:
         if (enabled && atomic_pending_entries() != 0) std::abort();
         ReadLocalStoreState* state = read_local_store_state();
         if (enabled && !state) std::abort();
-        if (enabled) {
-            if (read_local_enabled_ && read_local_atomic_filter_ != atomic_filter) std::abort();
-            read_local_atomic_filter_ = atomic_filter;
-        }
         if constexpr (kReadLocalSetTaxAtomicRaw) {
             // Persistence/bootstrap may have used the ordinary overwrite path before the boot latch
             // is exposed. Establish fixed atomic payload cells for that final image while no foreign
@@ -788,7 +783,6 @@ public:
         return read_local_store_state_required().retire_sink;
     }
 #endif
-    bool read_local_atomic_filter_enabled() const { return read_local_atomic_filter_; }
 
     uint64_t read_local_state_acquire() const {
         return read_local_store_state_required().probe_sequence.load(std::memory_order_acquire);
@@ -823,7 +817,6 @@ public:
         // permanently poisons, so short-circuiting here cannot turn that fail-closed state into a
         // false negative after the last enumerable entry drains.
         if (!read_local_pending(state)) return false;
-        if (!read_local_atomic_filter_) return true;
         return read_local_store_state_required().foreign_reads.might_contain(hash);
     }
     bool foreign_read_key_unsafe(uint64_t hash) const {
@@ -831,10 +824,8 @@ public:
         return foreign_read_key_unsafe(state, hash);
     }
     // Multi-key window validator. Load it only for a key whose shard word carries the pending bit,
-    // and only to compare against a later load of the same cell. With the filter OFF a pending
-    // shard sends every key to its owner before any comparison, so the constant is never compared.
+    // and only to compare against a later load of the same cell. Called only on the armed lane.
     uint32_t foreign_read_cell_epoch(uint64_t hash) const {
-        if (!read_local_atomic_filter_) return 0;
         return read_local_store_state_required().foreign_reads.cell_epoch(hash);
     }
     static uint32_t foreign_read_filter_index(uint64_t hash) {
@@ -880,8 +871,7 @@ public:
         // reader that already observed a negative cell cannot validate across the later handler.
         ReadLocalTableGuard publication(*this);
         ReadLocalStoreState& state = read_local_store_state_required();
-        if (read_local_atomic_filter_)
-            state.foreign_reads.add_span(count, std::forward<HashAt>(hash_at));
+        state.foreign_reads.add_span(count, std::forward<HashAt>(hash_at));
         foreign_read_pending_witness_open(state);
     }
 
@@ -889,8 +879,7 @@ public:
     void foreign_read_scope_close_span(uint32_t count, HashAt&& hash_at) {
         if (!read_local_enabled_) return;
         ReadLocalStoreState& state = read_local_store_state_required();
-        if (read_local_atomic_filter_)
-            state.foreign_reads.close_span(count, std::forward<HashAt>(hash_at));
+        state.foreign_reads.close_span(count, std::forward<HashAt>(hash_at));
         foreign_read_pending_witness_close(state);
     }
 
@@ -3527,10 +3516,9 @@ private:
         if (!read_local_enabled_) std::abort();
         if (entry.foreign_read_unsafe_published) std::abort();
         ReadLocalStoreState& state = read_local_store_state_required();
-        if (read_local_atomic_filter_)
-            state.foreign_reads.add_span(entry.capacity, [&](uint32_t index) {
-                return atomic_entry_hash(entry, index);
-            });
+        state.foreign_reads.add_span(entry.capacity, [&](uint32_t index) {
+            return atomic_entry_hash(entry, index);
+        });
         foreign_read_pending_witness_open(state);
         entry.foreign_read_unsafe_published = true;
     }
@@ -3538,10 +3526,9 @@ private:
     void read_local_pending_unpublish(AtomicEntry& entry) {
         if (__builtin_expect(!entry.foreign_read_unsafe_published, true)) return;
         ReadLocalStoreState& state = read_local_store_state_required();
-        if (read_local_atomic_filter_)
-            state.foreign_reads.close_span(entry.capacity, [&](uint32_t index) {
-                return atomic_entry_hash(entry, index);
-            });
+        state.foreign_reads.close_span(entry.capacity, [&](uint32_t index) {
+            return atomic_entry_hash(entry, index);
+        });
         foreign_read_pending_witness_close(state);
         entry.foreign_read_unsafe_published = false;
     }
@@ -3574,13 +3561,13 @@ private:
     void foreign_read_poison_open() {
         ReadLocalTableGuard publication(*this);
         ReadLocalStoreState& state = read_local_store_state_required();
-        if (read_local_atomic_filter_) state.foreign_reads.poison_open();
+        state.foreign_reads.poison_open();
         foreign_read_pending_witness_open(state);
     }
 
     void foreign_read_poison_close() {
         ReadLocalStoreState& state = read_local_store_state_required();
-        if (read_local_atomic_filter_) state.foreign_reads.poison_close();
+        state.foreign_reads.poison_close();
         foreign_read_pending_witness_close(state);
     }
 
@@ -3774,11 +3761,11 @@ private:
     // than a legitimate reason to widen these hot fields.
     uint32_t  cap_[2]   = {0, 0};
     uint32_t  mask_[2]  = {0, 0};
-    // Boot-latched, and the first two bytes a foreign probe tests before it touches anything else.
-    // They belong ON the topology line rather than 200 bytes past it: co-located, the whole foreign
+    // Boot-latched, and the first byte a foreign probe tests before it touches anything else.
+    // It belongs ON the topology line rather than 200 bytes past it: co-located, the whole foreign
     // read reduces to one line of FlatStore. Armed state itself stays sidecarred.
     bool      read_local_enabled_ = false;
-    bool      read_local_atomic_filter_ = false;
+    uint8_t   reader_reserved_ = 0; // retain the measured reader/owner line separation
 
     // ---- SEPARATOR. Read-mostly only: config, bind-once counter bindings, and the snapshot
     // scalars that are latched once when a capture is prepared. NOTHING here is written by an
@@ -3864,7 +3851,7 @@ struct FlatStoreLayoutLock {
     // atomic_pending_ is the FIRST of them: read_local_probe() cannot reach probe_sequence, the
     // read-local filter or the retire sink without loading it.
     static constexpr size_t reader_first = offsetof(FlatStore, atomic_pending_);
-    static constexpr size_t reader_last  = offsetof(FlatStore, read_local_atomic_filter_);
+    static constexpr size_t reader_last  = offsetof(FlatStore, read_local_enabled_);
     // Every byte the owner writes on the ordinary insert-a-new-key / DEL path.
     static constexpr size_t owner_first  = offsetof(FlatStore, live_);
     static constexpr size_t owner_last   = offsetof(FlatStore, borrow_tombs_) + 3;

exec
/bin/bash -lc "rg -n -g '"'!.merge-inputs/**'"' -g '"'!CODEX-OUT.md'"' 'xshard_dispatch_scale|shard-home' . && rg -l -g '"'!.merge-inputs/**'"' -g '"'!CODEX-OUT.md'"' 'read-local-prefetch-capture|read-local-atomic-filter|read-local-interleave|flip-auto-band|l3-domains|smt-mode|genthread-schedule|atomic-window|persist-io|lru-clock-shift|script-crossshard-(max-bytes|workbench-bytes|conflict-retries|cut-slots)|tls-ktls|key-lb|client-lb|lb-(sample-rate|age-sample-rate|tick-ms|imbalance-pct|move-cap|cooldown-ms)|--ex-sched|--overlap|thread-pipeline' ." in /home/user/Projects/cx-merge
 succeeded in 0ms:
./NOTES-XPERF2.md:11:| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes | `tests/xshard_dispatch_scale.sh` |
./NOTES-XPERF2.md:222:16 shards, all of them homed on the same two executors via `--shard-home`, two IO threads on their
./NOTES-XPERF2.md:295:| `tests/xshard_dispatch_scale.sh` (+ `.py`) | boots its own 4-thread and 128-thread arms | dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
./NOTES-XPERF2.md:302:sizes; `xshard_dispatch_scale.py` refuses to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` prove
./DESIGN-KNOBS.md:22:| `--shard-home` | Unset | Deleted; round-robin over resolved executors | Preserve default ownership; remove manual map parsing. Runtime migration remains available. |
./DESIGN-KNOBS.md:311:5. **`--shard-home`: one regression instrument is lost.** The retired scaling wrapper kept
./AUDIT-TESTS.md:28:| F4 | P0 | `tests/atomic_torn.py:137`, `tests/contarity.py:128` (`fields[2]==b"ex"`); `tests/lbsignals.py:94` ("both roles present"); `tests/sort.py:112-120,141,152,167` (owner := `shard % lb_ex_threads`); `tests/barrier.py:213-251` | Executor discovery keys on the `ex` role label. Under `--thread-mode 1s` every thread is `io` (`src/cmd/lbsignals.cc:156`), so these find zero executors and abort — correct for a 1s boot that cannot form the geometry, but the reason is wrong ("needs two executors" when it has eight owners), and `sort.py` divides by `lb_ex_threads`=0. The truth for BOTH modes is the shard rows: `shard <sid> <owner_tid>` (`lbsignals.cc:164-168`). | **Fixed** for atomic_torn/contarity (owners from shard rows via `_lib.topology`); lbsignals.py made mode-aware (ex legs skip WITH reason on 1s, io legs still assert conservation). sort.py/barrier.py: flagged, not converted (they also assume the default `sid % ex` shard-home map, which is a second assumption the LBSIGNALS owner map removes — see B.2). |
./AUDIT-TESTS.md:62:`snap_typed_race.py:9`, `watchlive.py:64`, `zsetops.py:23`, `xshard_dispatch_scale.py:33`; plus
./AUDIT-TESTS.md:71:`storeorder.py:278`, `xmove.py:86`, `xscript.py:82,96`, `xshard_dispatch_scale.py:75`,
./AUDIT-TESTS.md:78:`aof_rewrite_trigger_matrix.sh:37`, `xshard_dispatch_scale.sh:66`, `differ_gate.sh:53`,
./AUDIT-TESTS.md:87:(topology), `contarity.py` (topology), `borrow_registry.py` (shard count), `xshard_dispatch_scale.py`
./AUDIT-TESTS.md:105:Assume the DEFAULT shard-home map (`sid % executors`) rather than asking LBSIGNALS: `sort.py:141,152,
./AUDIT-TESTS.md:107:`--shard-home` (used by `xshard_dispatch_scale.sh:63`) would silently break them.
./AUDIT-TESTS.md:125:| `xshard_dispatch_scale.sh` (2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 | 90-150 est. | measurement, keep |
./AUDIT-TESTS.md:308:| `WIP: helper dedup` | `tests/_lib.py` (RESP client, LBSIGNALS parser, owner-from-shard-rows topology, geometry helpers, `armed()`, `Report`, `skip_all`) + owner selection from shard rows in `atomic_torn.py`, `contarity.py`, `borrow_registry.py`, `xshard_dispatch_scale.py` | F4, F18 |
./NOTES-BARRIERTEST.md:33:`NOTES-BARRIER.md` section 5 specifies. Do not supply `--shard-home`: `DEBUG SHARD` reports a shard,
./DESIGN-GATEHYGIENE.md:182:| `xshard_dispatch_scale.sh:123` | `se > 0 and be > 0` | PERF — a *sign* test on a difference of two medians |
./DESIGN-GATEHYGIENE.md:183:| `xshard_dispatch_scale.sh:127` | `ratio <= limit` | PERF — ratio of differences of medians across two boots |
./DESIGN-GATEHYGIENE.md:473:| `xshard_dispatch_scale.sh` / `.py` | Disable balancing during the fixed-owner experiment; verify ownership remained fixed. Use distinct reply payloads, check every pair's topology, alternate boot order, and retain every ratio. Only score release timings when repeated arms and same-shard controls agree within the supplied 2% box contract. Every qualified pair must meet the unchanged 1.20 bound; no best-pair selection. | Misroute a key or change a second pair's fan-out to fail mechanism checks; restore the all-thread scan to fail a stable release measurement. Unqualified timings say SKIP with all numbers, not that scaling passed. |
./AUDIT-CLEANUP.md:81:| D19 | `src/core/config.h:4` vs `t_server.cc:364` | "the CONFIG table is built FROM this struct" | `aof-use-rdb-preamble` is a hardcoded `"yes"` row with no Config field (deliberate per AUDIT-AOF §711-720); 13 Config fields have no CONFIG GET row: `port bind unixsocket l3-domains place ratio shard-home shards pin load lru-clock-shift user conf_path` — `port`/`bind`/`unixsocket`/`shards`/`lru-clock-shift` are the Redis-visible gaps | LISTED (knob-compat feature lane; see idea I1) |
./tests/config_parser_test.cc:346:        {"--shard-home", "0:1"}, {"--l3-domains", "0-7"}, {"--smt-mode", "0"},
./NOTES-HYGIENE.md:118:l3-domains, place, ratio, shards, shard-home, no-pin, hash, user, load, and lru-clock-shift all have
./NOTES-HYGIENE.md:354:The calibration examples expireindex.py, borrow_registry.py, and xshard_dispatch_scale.sh were
./NOTES-HYGIENE.md:475:  bash tests/xshard_dispatch_scale.sh
./tests/knobs.py:11:    "flip-auto-band", "shard-home", "l3-domains", "smt-mode", "genthread-schedule",
./tests/gate.sh:678:# Dispatch scaling's manual ownership geometry was retired with shard-home; DESIGN-KNOBS.md.
./tests/gate.sh:682:XDS_PORT=$PORT XDS_CPUS=$CORES XDS_BIN=./build/tomokv bash tests/xshard_dispatch_scale.sh \
./tests/gate.sh:688:XDS_PORT=$PORT XDS_CPUS=$CORES XDS_BIN=./build/tomokv bash tests/xshard_dispatch_scale.sh --release-build \
./tests/differ.py:5566:    # the default shard-home map, whose two executor slots own sid % 2. Do not let a same-owner or
./tests/sort.py:10:The battery fails on any other geometry. The default shard-home map assigns shard `sid` to executor
./tests/_lib.py:13:    is inferred from key names or from the default shard-home map (the hash seed is drawn from the
./NOTES-AOFFRAME.md
./NOTES-AOFSCRIPT.md
./AUDIT-TESTS.md
./DESIGN-KNOBS.md
./DESIGN-flipfp.md
./NOTES-ATOMICS.md
./NOTES-XSCRIPT.md
./DESIGN-W-RLIDEAS.md
./AUDIT-resp3push.md
./NOTES-CAPTURE-PREFETCH.md
./NOTES-KTLS.md
./DESIGN-P0REPLY.md
./DESIGN.md
./NOTES-SERVERTAIL.md
./DESIGN-GATEHYGIENE.md
./AUDIT-CLEANUP.md
./docs/flipctl-design.md
./NOTES-MIGRATE.md
./NOTES-PERSIST-IO.md
./tests/config_parser_test.cc
./tests/knobs.py
./NOTES-HYGIENE.md
./NOTES-EPOLL.md
./tests/tls.py
./docs/specs/AUDIT-TLS.md
./tests/gate.sh
./tests/atomic_torn.py
./src/core/ex_loop.h
./tests/rlcache_churn.py
./scratchpad/replycode/batteries.sh
./scratchpad/ringsize/batteries.sh
./tests/flipctl_unit.cc
./scratchpad/rlbatch/batteries.sh
./tests/bplus.py
./NOTES-SNAPCUT.md
./AUDIT-LOCALFUSE.md
./P128.md
./scratchpad/rlbatch/lib.sh
./scratch/red2.sh
./scratch/red.sh
./tests/atomicwindow.py
./scratch/flipfp/fpprobe.sh
./scratch/flipfp/chain.sh
./scratch/flipfp/fpmargin.sh
./scratch/rv.sh
./scratch/flipfp/bat.sh
./scratch/flipfp/fpshift.sh
./scratch/NOTES.md
./scratch/ver.sh
./scratchpad/robdiet/batteries.sh
./scratch/finalw.sh
./scratch/rv3.sh
./scratch/final.sh
./scratch/report.py
./scratch/report2.py

exec
/bin/bash -lc "git ls-files scratch scratchpad && rg -n -g '*.py' -g '*.sh' -g '*.cc' -g '*.h' -g '"'!.merge-inputs/**'"' 'read-local-prefetch-capture|read-local-atomic-filter|read-local-interleave|flip-auto-band|l3-domains|smt-mode|genthread-schedule|atomic-window|persist-io|lru-clock-shift|script-crossshard-(max-bytes|workbench-bytes|conflict-retries|cut-slots)|tls-ktls|key-lb|client-lb|lb-(sample-rate|age-sample-rate|tick-ms|imbalance-pct|move-cap|cooldown-ms)|--ex-sched|--overlap|thread-pipeline' src tests scratch scratchpad" in /home/user/Projects/cx-merge
 succeeded in 0ms:
scratch/NOTES.md
scratch/ab.sh
scratch/abr.sh
scratch/abw.sh
scratch/cellview.py
scratch/chain.sh
scratch/diag1.sh
scratch/diag2.sh
scratch/final.sh
scratch/finalw.sh
scratch/flipfp/bat.sh
scratch/flipfp/chain.sh
scratch/flipfp/fl.sh
scratch/flipfp/fpmargin.sh
scratch/flipfp/fpprobe.sh
scratch/flipfp/fpshift.sh
scratch/flipfp/multi.sh
scratch/flipfp/report.sh
scratch/flipfp/sampled_band_probe.cc
scratch/flipfp/ttfm.py
scratch/flipprobe.sh
scratch/gaterun.sh
scratch/gatewait.sh
scratch/hold.sh
scratch/lib.sh
scratch/matrix.sh
scratch/mk.sh
scratch/model.py
scratch/red.sh
scratch/red2.sh
scratch/report.py
scratch/report2.py
scratch/rv.sh
scratch/rv2.sh
scratch/rv3.sh
scratch/sampled_band_probe.cc
scratch/satprobe.sh
scratch/signal.sh
scratch/sk.sh
scratch/spin.py
scratch/splits.sh
scratch/spread.py
scratch/summ.py
scratch/thrash.py
scratch/ttfm.py
scratch/ver.sh
scratch/ver2.sh
scratch/wrongsplit.sh
scratchpad/aclprobe/holes.cc
scratchpad/edgeenc/boundary_grid.py
scratchpad/edgeenc/exec_ryow.py
scratchpad/edgeenc/execexec.py
scratchpad/edgeenc/lane.sh
scratchpad/edgeenc/listmulti.py
scratchpad/edgeenc/minimize.py
scratchpad/edgeenc/resp.py
scratchpad/execatomic/abmget.sh
scratchpad/execatomic/differ.sh
scratchpad/execatomic/fanout_proto.py
scratchpad/execatomic/inexec_probe.py
scratchpad/execatomic/lane.sh
scratchpad/execatomic/perfguard.sh
scratchpad/execatomic/sweep.sh
scratchpad/execatomic/tornrun.sh
scratchpad/execfix/ab.sh
scratchpad/execfix/ab2.sh
scratchpad/execfix/abbench.py
scratchpad/execfix/abortrepro.sh
scratchpad/execfix/breadth.py
scratchpad/execfix/c_iso.py
scratchpad/execfix/c_loop.py
scratchpad/execfix/c_min.py
scratchpad/execfix/c_probe.py
scratchpad/execfix/c_rpush.py
scratchpad/execfix/c_shrink.py
scratchpad/execfix/c_time.py
scratchpad/execfix/c_var.py
scratchpad/execfix/differ.sh
scratchpad/execfix/execbench.py
scratchpad/execfix/execexec.py
scratchpad/execfix/lane.sh
scratchpad/execfix/narrow.py
scratchpad/execfix/repro.py
scratchpad/execfix/resp.py
scratchpad/execfix/sweep.sh
scratchpad/execfix/tag_aborts.py
scratchpad/execfix/verify.sh
scratchpad/execiso/ab.sh
scratchpad/execiso/abortrepro.sh
scratchpad/execiso/differ.sh
scratchpad/execiso/execbench.py
scratchpad/execiso/lane.sh
scratchpad/execiso/narrow.py
scratchpad/execiso/repro.py
scratchpad/execiso/sweep.sh
scratchpad/hexpire/instr-result5.csv
scratchpad/hexpire/instr-result8.csv
scratchpad/hexpire/instr-selfcheck.csv
scratchpad/hexpire/instr_ab4.sh
scratchpad/hexpire/instr_ab5.sh
scratchpad/hexpire/instr_ab6.sh
scratchpad/replycode/batteries.sh
scratchpad/replycode/boot.sh
scratchpad/replycode/copycount.sh
scratchpad/replycode/cyc_paired.sh
scratchpad/replycode/instr_ab.sh
scratchpad/replycode/instr_exact.sh
scratchpad/replycode/instr_paired.sh
scratchpad/replycode/interpose.c
scratchpad/replycode/interpose2.c
scratchpad/replycode/layout.cc
scratchpad/replycode/quietgate.sh
scratchpad/replycode/replay.c
scratchpad/replycode/summarize.py
scratchpad/replycode/wirecheck.py
scratchpad/ringsize/README.md
scratchpad/ringsize/ab.sh
scratchpad/ringsize/ab_triad.sh
scratchpad/ringsize/ab_triad_report.py
scratchpad/ringsize/batteries.sh
scratchpad/ringsize/build_arms.sh
scratchpad/ringsize/codegen.cc
scratchpad/ringsize/codegen_ab.sh
scratchpad/ringsize/expwide_bisect.sh
scratchpad/ringsize/expwide_preexisting.sh
scratchpad/ringsize/final_table.py
scratchpad/ringsize/histreport.py
scratchpad/ringsize/holdconns
scratchpad/ringsize/holdconns.c
scratchpad/ringsize/inflight.sh
scratchpad/ringsize/instr_ab.sh
scratchpad/ringsize/instr_deep.sh
scratchpad/ringsize/keep_running.sh
scratchpad/ringsize/laneguard.sh
scratchpad/ringsize/lib.sh
scratchpad/ringsize/measure_triad.sh
scratchpad/ringsize/mem.sh
scratchpad/ringsize/mutate.sh
scratchpad/ringsize/ovf_patch.py
scratchpad/ringsize/owner_cell.py
scratchpad/ringsize/owner_cell_report.py
scratchpad/ringsize/probe_cost.cc
scratchpad/ringsize/probe_cost.sh
scratchpad/ringsize/ratio_shape.sh
scratchpad/ringsize/regimes.sh
scratchpad/ringsize/regimes_report.py
scratchpad/ringsize/replay_counters.sh
scratchpad/ringsize/report.py
scratchpad/ringsize/rl_value.sh
scratchpad/ringsize/run_when_clear.sh
scratchpad/ringsize/s1_mget_repro.py
scratchpad/ringsize/satcheck.sh
scratchpad/ringsize/sizes.cc
scratchpad/ringsize/triad_report.py
scratchpad/ringsize/validate.sh
scratchpad/rlbatch/.gitignore
scratchpad/rlbatch/README.md
scratchpad/rlbatch/ab.sh
scratchpad/rlbatch/abl.csv
scratchpad/rlbatch/ablate.py
scratchpad/rlbatch/batteries.sh
scratchpad/rlbatch/build.sh
scratchpad/rlbatch/ctl.csv
scratchpad/rlbatch/differ_gate_fused.sh
scratchpad/rlbatch/lib.sh
scratchpad/rlbatch/measure.sh
scratchpad/rlbatch/pre.csv
scratchpad/rlbatch/profslope.sh
scratchpad/rlbatch/replay.c
scratchpad/rlbatch/report.py
scratchpad/rlbatch/s1.csv
scratchpad/rlbatch/s2.csv
scratchpad/rlbatch/s3.csv
scratchpad/rlbatch/s6.csv
scratchpad/rlbatch/s7.csv
scratchpad/rlbatch/symdelta.py
scratchpad/rlbatch/tax.py
scratchpad/robdiet/README.md
scratchpad/robdiet/ab.sh
scratchpad/robdiet/ab2s.csv
scratchpad/robdiet/ab2s_rev.csv
scratchpad/robdiet/armtax.csv
scratchpad/robdiet/batteries.sh
scratchpad/robdiet/bisect.csv
scratchpad/robdiet/final4.csv
scratchpad/robdiet/headline.csv
scratchpad/robdiet/lib.sh
scratchpad/robdiet/measure.sh
scratchpad/robdiet/measure2s.sh
scratchpad/robdiet/multi.sh
scratchpad/robdiet/null.csv
scratchpad/robdiet/null2s.csv
scratchpad/robdiet/replay.c
scratchpad/robdiet/report.py
scratchpad/robdiet/sizes.py
scratchpad/robdiet/superseded-arm-D.csv
scratchpad/storeorder/abhunt.sh
scratchpad/storeorder/armhunt.sh
scratchpad/storeorder/boot.sh
scratchpad/storeorder/differmatrix.sh
scratchpad/storeorder/finalsweep.sh
scratchpad/storeorder/gates.sh
scratchpad/storeorder/hunt.sh
scratchpad/storeorder/jump.py
scratchpad/storeorder/liveness.py
scratchpad/storeorder/perfab.py
scratchpad/storeorder/raceprobe.py
scratchpad/storeorder/stop.sh
scratchpad/storeorder/wstream.py
scratchpad/tls_layout_probe.cc
scratchpad/xacct_measure.py
scratchpad/zsetfix/differ-FIX.tsv
scratchpad/zsetfix/differ-HEAD.tsv
scratchpad/zsetfix/nzmatrix.py
scratchpad/zsetfix/orderprobe.py
scratch/red2.sh:144:  pid=$(boot8 "$RED_BIN" "$PORT_BAT" "r2ctl-$r" --ratio 6:2 --atomic 0 --flip-auto 1 --flip-auto-band 2 --lb-age-sample-rate 1024) || continue
scratch/red.sh:155:        --flip-auto-band 2 --lb-age-sample-rate 1024) || return 1
scratch/flipfp/fpprobe.sh:23:        --flip-auto 1 --flip-auto-band -1 $extra) || exit 1
scratch/flipfp/chain.sh:55:  slope "pre1-2s"  "$PRE_BIN"  2s "--flip-auto 1 --flip-auto-band 0" 75
scratch/flipfp/chain.sh:56:  slope "post1-2s" "$POST_BIN" 2s "--flip-auto 1 --flip-auto-band 0" 75
scratch/flipfp/chain.sh:128:  local pid; pid=$(SRV_CPUS=$MY_MASK boot "$bin" "$PORT_CTL" "ctl-$tag" --ratio 6:2 --atomic 0 --flip-auto 1 --flip-auto-band 2 --lb-age-sample-rate 1024) || return 1
scratch/flipfp/fpmargin.sh:33:        --flip-auto 1 --flip-auto-band "$BAND") || exit 1
scratch/flipfp/bat.sh:48:  boot --thread-mode fused --atomic 1 --read-local 1 --read-local-atomic-filter 1 \
scratch/flipfp/fpshift.sh:23:        --flip-auto 1 --flip-auto-band -1) || exit 1
tests/knobs.py:10:    "read-local-prefetch-capture", "read-local-atomic-filter", "read-local-interleave",
tests/knobs.py:11:    "flip-auto-band", "shard-home", "l3-domains", "smt-mode", "genthread-schedule",
tests/knobs.py:12:    "atomic-window", "persist-io", "lru-clock-shift", "script-crossshard-max-bytes",
tests/knobs.py:13:    "script-crossshard-workbench-bytes", "script-crossshard-conflict-retries",
tests/knobs.py:14:    "script-crossshard-cut-slots", "tls-ktls", "key-lb", "client-lb",
tests/knobs.py:15:    "lb-sample-rate", "lb-age-sample-rate", "lb-tick-ms", "lb-imbalance-pct",
tests/knobs.py:16:    "lb-move-cap", "lb-cooldown-ms", "ex-sched", "overlap", "thread-pipeline",
scratch/ver.sh:56:        --flip-auto-band 2 --lb-age-sample-rate 1024) || return 1
src/core/ex_loop.h:1856:            // key-lb half, 2/6). Rebinding is a handful of pointer stores and runs only on the
scratch/finalw.sh:38:  PID=$(boot "$FIX_BIN" "$PORT_BAT" batctl --ratio 6:2 --atomic 0 --flip-auto 1 --flip-auto-band 2 --lb-age-sample-rate 1024) && {
scratch/rv3.sh:3:# phase is the row that was failing) x3 with the typed --flip-auto-band 2 the gate uses, the directed
scratch/rv3.sh:18:  pid=$(boot8 "$FIX_BIN" "$PORT_BAT" "r6ctl-$r" --ratio 6:2 --atomic 0 --flip-auto 1 --flip-auto-band 2 --lb-age-sample-rate 1024) || continue
scratch/report.py:251:    "tests/flipctl.py (the gate's own invocation: --ratio 6:2 --atomic 0 --flip-auto 1 --flip-auto-band 2 "
scratch/report.py:252:    "--lb-age-sample-rate 1024, 8 server threads on cpus 52-55 + siblings)",
scratch/report.py:344:<p><b>Fix:</b> the Measuring phase never moves the split, so the readings the model already takes while deciding <i>are</i> readings of the origin. Bracket them and floor every band of the maneuver at twice that spread — the same 2× observed-jitter convention as the signature band, the rate band and <code>band_</code>. On the gate's ramping driver the floor becomes 40%, the ramp's 22.5% confirms nothing, the seek reverts to 6:2 and anchors off-rail. On a still baseline the floor is ≈0 and nothing changes. The floor applies under an explicit <code>--flip-auto-band</code> too: that knob says how small a gain is worth chasing, not how still the workload is holding.</p>
scratch/report.py:366:D  GATE ROW         tests/gate.sh (or just: --ratio 6:2 --atomic 0 --flip-auto 1 --flip-auto-band 2
scratch/report.py:367:                    --lb-age-sample-rate 1024 + tests/flipctl.py --stable-seconds 30) -- this is the row the
scratchpad/replycode/batteries.sh:18:      --read-local 1 --read-local-atomic-filter 1 --atomic 1 --enable-debug-command yes \
scratch/final.sh:33:PID=$(boot "$FIX_BIN" "$PORT_BAT" batctl --ratio 6:2 --atomic 0 --flip-auto 1 --flip-auto-band 2 --lb-age-sample-rate 1024) || exit 2
scratch/report2.py:281:    "tests/flipctl.py (the gate's invocation: --ratio 6:2 --atomic 0 --flip-auto 1 --flip-auto-band 2 --lb-age-sample-rate 1024, 8 server threads on cpus 52-55 + siblings)",
scratch/report2.py:385:E  GATE ROW         --ratio 6:2 --atomic 0 --flip-auto 1 --flip-auto-band 2 --lb-age-sample-rate 1024 + tests/flipctl.py --stable-seconds 30
scratchpad/rlbatch/batteries.sh:51:  boot --thread-mode fused --atomic 1 --read-local 1 --read-local-atomic-filter 1 \
scratch/rv.sh:14:  pid=$(boot8 "$FIX_BIN" "$PORT_BAT" "r3ctl-$r" --ratio 6:2 --atomic 0 --flip-auto 1 --flip-auto-band 2 --lb-age-sample-rate 1024) || continue
scratchpad/rlbatch/lib.sh:20:  # EXTRA_ARGS lets a caller add boot flags (e.g. --read-local-interleave 0) without a second copy
scratchpad/ringsize/batteries.sh:69:  boot --thread-mode fused --atomic 1 --read-local 1 --read-local-atomic-filter 1 \
tests/flipctl_unit.cc:187:    // a flat --flip-auto-band 2 (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
tests/flipctl_unit.cc:191:        FlipShiftDetector typed(2, 4);   // --flip-auto-band 2
tests/flipctl_unit.cc:253:    // controller runs the learned band -- but an operator who sets --flip-auto-band AND the sampler
tests/flipctl_unit.cc:262:            FlipShiftDetector typed(2, 8);   // --flip-auto-band 2, the operator's knob
tests/config_parser_test.cc:227:        !rejects({"--genthread-schedule", "streams0"}))
tests/config_parser_test.cc:344:        {"--read-local-prefetch-capture", "1"}, {"--read-local-atomic-filter", "1"},
tests/config_parser_test.cc:345:        {"--read-local-interleave", "1"}, {"--flip-auto-band", "-1"},
tests/config_parser_test.cc:346:        {"--shard-home", "0:1"}, {"--l3-domains", "0-7"}, {"--smt-mode", "0"},
tests/config_parser_test.cc:347:        {"--genthread-schedule", "coarse"}, {"--atomic-window", "-1"},
tests/config_parser_test.cc:348:        {"--persist-io", "uring"}, {"--lru-clock-shift", "8"},
tests/config_parser_test.cc:349:        {"--script-crossshard-max-bytes", "-1"},
tests/config_parser_test.cc:350:        {"--script-crossshard-workbench-bytes", "-1"},
tests/config_parser_test.cc:351:        {"--script-crossshard-conflict-retries", "-1"},
tests/config_parser_test.cc:352:        {"--script-crossshard-cut-slots", "-1"}, {"--tls-ktls", "yes"},
tests/config_parser_test.cc:353:        {"--key-lb", "1"}, {"--client-lb", "1"}, {"--lb-sample-rate", "64"},
tests/config_parser_test.cc:354:        {"--lb-age-sample-rate", "0"}, {"--lb-tick-ms", "1000"},
tests/config_parser_test.cc:355:        {"--lb-imbalance-pct", "25"}, {"--lb-move-cap", "1"},
tests/config_parser_test.cc:356:        {"--lb-cooldown-ms", "5000"}, {"--ex-sched", "0"},
tests/config_parser_test.cc:357:        {"--overlap", "0"}, {"--thread-pipeline", "0"},
tests/rlcache_churn.py:62:# space keeps every shard at the same op rate, the imbalance never crosses lb-imbalance-pct, and no
tests/bplus.py:540:        if discovery.command("CONFIG", "GET", "read-local-atomic-filter") != []:
scratchpad/robdiet/batteries.sh:51:  boot --thread-mode fused --atomic 1 --read-local 1 --read-local-atomic-filter 1 \
tests/atomicwindow.py:216:                print("  note atomic-window " + detail, flush=True)
tests/tls.py:351:            b"tls-ktls" in config_values):
tests/gate.sh:58:# 172 -> 178: snapcut added two cross-shard snapshot-cut rows per persist-io engine, and execfix
tests/gate.sh:64:# 192 -> 194: the AOF frame-order battery, on persist-io normal under both atomic modes.
tests/atomic_torn.py:901:config("atomic-window", 1)
tests/atomic_torn.py:914:config("atomic-window", 1)
tests/atomic_torn.py:938:note("atomic-window stalls and resumes",
tests/atomic_torn.py:941:config("atomic-window", 256)
tests/atomic_torn.py:974:note("atomic-window stalls and resumes",
tests/atomic_torn.py:977:config("atomic-window", 256)
tests/atomic_torn.py:1008:# Resizing was removed with atomic-window. CONFIG SET atomic 1 still rebuilds credit generations;
tests/atomic_torn.py:1020:    if not config("atomic-window", value):
tests/atomic_torn.py:1021:        lease_errors.append("CONFIG atomic-window %d failed" % value)
tests/atomic_torn.py:1049:note("atomic-window reconfiguration preserves bound and reclaims leases",
tests/atomic_torn.py:1057:config("atomic-window", 256)
tests/atomic_torn.py:1063:    if not config("atomic-window", value):
tests/atomic_torn.py:1064:        lease_errors.append("CONFIG atomic-window %d failed" % value)
tests/atomic_torn.py:1111:note("atomic-window reconfiguration preserves bound and reclaims leases",
tests/atomic_torn.py:1120:release_note("atomic-window shrink/reclaim within release budgets",
tests/atomic_torn.py:1123:config("atomic-window", 256)

exec
/bin/bash -lc "python3 - <<'PY'
from pathlib import Path
root = Path.cwd()
path = root / 'tests/atomic_torn.py'
text = path.read_text()
start = text.index('# Separate admission liveness from live credit reconfiguration/accounting.')
end = text.index('# Live CONFIG flips under active traffic are a liveness/safety arm.', start)
knobs = (root / '.merge-inputs/knobs/tests/atomic_torn.py').read_text()
ks = knobs.index('# Separate admission liveness from live credit reconfiguration/accounting.')
ke = knobs.index('# Live CONFIG flips under active traffic are a liveness/safety arm.', ks)
text = text[:start] + knobs[ks:ke] + text[end:]
path.write_text(text)
path = root / 'tests/gate.sh'
text = path.read_text()
start = text.index('<<<<<<< cx-knobs\\n')
end = text.index('>>>>>>> cx-timing\\n', start) + len('>>>>>>> cx-timing\\n')
text = text[:start] + '# Dispatch scaling\\'s manual ownership geometry is retired; see MERGE.md and DESIGN-KNOBS.md.\\n' + text[end:]
text = text.replace('# flag. A ratio of wall times is still a performance claim (see DESIGN-GATEHYGIENE.md). The\\n# cross-shard guard also checks instrument stability before scoring any timing, retaining all pairs.\\n', '# flag. A ratio of wall times is still a performance claim (see DESIGN-GATEHYGIENE.md).\\n')
text = text.replace('# Deliberately omit --release-build: all five coverage/safety checks remain mandatory, while\\n# their measured completion times are printed without scoring release scheduling budgets.\\n', '# Deliberately omit --release-build: coverage/safety checks remain mandatory, including the\\n# derived-window witnesses; promotion timing prints without scoring its release budget.\\n')
path.write_text(text)
print('Resolved atomic admission replacement and dispatch retirement; retained release wiring.')
PY
sed -n '1,90p' scratch/flipfp/fpmargin.sh
sed -n '1,180p' scratch/flipfp/chain.sh" in /home/user/Projects/cx-merge
 succeeded in 0ms:
Resolved atomic admission replacement and dispatch retirement; retained release wiring.
#!/bin/bash
# fpmargin.sh -- THE MARGIN ROW: how far is a real workload change from the learned floor when the
# fingerprint is sampled 1-in-W?  The failure this checks for is DEAFNESS: a floor lifted by the
# estimator's own sampling noise until it swallows the signal it must catch.
#
# Two runs give the two numbers, because they cannot be read from one:
#   BAND=-1  the floor is LEARNED, so it can be read at the held anchor -- but the trigger then
#            fires and reset() wipes the distance, so this run reports the floor and the firing.
#   BAND=90  the floor is pinned at 0.90 and nothing fires, so the anchor is HELD and
#            signature_distance converges to the change's true distance -- the signal.
# margin = signal / floor.
#
#   fpmargin.sh <binary> <tag> <band> <shiftpair>    env: RL (per-conn ops/s, default 4000)
#     shiftpair: bitcount_incr | get_set
source /tmp/claude-1000/-home-user-Projects/ee6eb242-5302-49cf-b767-1a2d8d8f0f61/scratchpad/flipfp/fl.sh
BIN=$1; TAG=$2; BAND=$3; PAIR=$4; RL=${RL:-4000}
PORT=$PORT_ON; OUT=$FP/fpm-$TAG.txt; TR=$FP/fpm-$TAG.trace
dbgg(){ echo "$1" | sed -n "s/^$2=//p" | head -1; }
mt(){ # mt SECS OUTFILE ARGS...
  local secs=$1 outf=$2; shift 2
  taskset -c "$LG4" memtier_benchmark -s 127.0.0.1 -p "$PORT" --protocol=redis -t 4 -c 32 --pipeline=32 \
    -d 32 --key-minimum=1 --key-maximum=$KEYMAX --test-time="$secs" --rate-limiting=$RL \
    --distinct-client-seed --hide-histogram "$@" >"$outf" 2>&1
}
case $PAIR in
  bitcount_incr) A=(--command="BITCOUNT __key__" --command-key-pattern=R)
                 B=(--command="INCR ctr:__key__" --command-key-pattern=R);;
  get_set)       A=(--ratio=0:1 --key-pattern=R:R)
                 B=(--ratio=1:0 --key-pattern=R:R);;
  *) echo "bad pair $PAIR"; exit 1;;
esac
pid=$(SRV_CPUS=$SRV4 boot "$BIN" "$PORT" "fpm-$TAG" --ratio 2:2 --shards 64 --atomic 0 \
        --flip-auto 1 --flip-auto-band "$BAND") || exit 1
LG_CPUS=$LG4 preload "$PORT" 4

# ---- phase A: workload A at the pinned rate, until anchored -------------------------------------
mt 130 "$FP/fpm-$TAG-A.mt" "${A[@]}" & load=$!
anchored=no
for i in $(seq 120); do
  [ "$(infog "$(flipinfo $PORT)" flipctl_state)" = anchored ] && { anchored=yes; break; }; sleep 1
done
dbgA=$($CLI -p $PORT debug flipctl 2>&1 | tr -d '\r')
floor=$(dbgg "$dbgA" signature_band); fpA=$(infog "$(flipinfo $PORT)" flipctl_fingerprint_triggers)
wait $load 2>/dev/null

# ---- phase B: workload B at the SAME pinned rate; trace the distance against the held floor -----
: >"$TR"
( while :; do
    d=$($CLI -p $PORT debug flipctl 2>/dev/null | tr -d '\r')
    printf '%s %s %s %s %s\n' "$(date +%s.%N)" "$(dbgg "$d" state)" "$(dbgg "$d" signature_band)" \
      "$(dbgg "$d" signature_distance)" "$(infog "$(flipinfo $PORT)" flipctl_fingerprint_triggers)"
    sleep 0.2
  done ) >>"$TR" 2>/dev/null & sampler=$!
mt 90 "$FP/fpm-$TAG-B.mt" "${B[@]}"
kill $sampler 2>/dev/null; wait $sampler 2>/dev/null
dbgB=$($CLI -p $PORT debug flipctl 2>&1 | tr -d '\r'); infoB=$(flipinfo $PORT)
fpB=$(infog "$infoB" flipctl_fingerprint_triggers)
# peak distance recorded while the anchor was still HELD (the value the floor is judged against)
peak=$(awk '$2=="anchored" && $4+0>m {m=$4+0} END{printf "%.6f", m}' "$TR")
peak_all=$(awk '$4+0>m {m=$4+0} END{printf "%.6f", m}' "$TR")
rA=$(mt_rate "$FP/fpm-$TAG-A.mt"); rB=$(mt_rate "$FP/fpm-$TAG-B.mt")
{ echo "$TAG bin=$(basename "$BIN") pair=$PAIR band=$BAND anchored=$anchored rateA=$rA rateB=$rB delta=$(awk -v a="$rA" -v b="$rB" 'BEGIN{if(a>0)printf "%+.1f%%",(b-a)*100/a}')"
  echo "  floor(learned at held anchor)=$floor  peak_distance_while_anchored=$peak  peak_any=$peak_all"
  echo "  margin=$(awk -v s="$peak" -v f="$floor" 'BEGIN{if(f>0)printf "%.1fx",s/f; else print "n/a"}')  fp_triggers ${fpA} -> ${fpB}  last=$(infog "$infoB" flipctl_last_trigger)"
  echo "  shift_distance=$(dbgg "$dbgB" last_shift_distance) shift_band=$(dbgg "$dbgB" last_shift_band)"
  echo "--- debug A ---"; echo "$dbgA"; echo "--- debug B ---"; echo "$dbgB"; } >"$OUT"
head -4 "$OUT"
stop "$pid" "$PORT"
#!/bin/bash
# chain.sh -- t-flipfp proof chain. Resumable (every cell skips when its output file exists),
# gate-pausing (quiet.done age + intruders before every cell). Re-run to resume.
#   S0 digests            S1 slope cells PRE / POST / PRE-null (flip-auto 0) on 1T, 2T, 2s
#   S1b slope 2s with --flip-auto 1 (anchored controller, band 0) PRE vs POST: the always-on cost
#   S2 accuracy: anchored band + false triggers, homogeneous MK and heterogeneous SET|GET, PRE vs POST
#   S3 wrong-split boots 3:1 (4 thr) and 5:1 (6 thr): PRE-auto, POST-auto, flipguard, POST off
#   S4 flipctl.py gate row (POST x2, PRE x1)   S5 batteries 1s + 2s   S6 differ   S7 gate quick
#   S8 tables
source /tmp/claude-1000/-home-user-Projects/ee6eb242-5302-49cf-b767-1a2d8d8f0f61/scratchpad/flipfp/fl.sh
cd "$WT" || exit 1
ONLY=${ONLY:-}
want(){ [ -z "$ONLY" ] || [[ ",$ONLY," == *",$1,"* ]]; }

# ---- S0 digests --------------------------------------------------------------------------------
if [ ! -s "$FP/digests.txt" ]; then
  for b in "$PRE_BIN" "$POST_BIN"; do [ -s "$b" ] || { LG "missing $b"; exit 1; }; done
  { for b in "$PRE_BIN" "$POST_BIN"; do echo "$(sha256sum "$b" | cut -c1-16) $(stat -c %s "$b") $(basename "$b")"; done
    echo "pre=$(git -C "$WT_BASE" rev-parse --short HEAD) post=$(git -C "$WT" rev-parse --short HEAD)"; } > "$FP/digests.txt"
  if [ "$(sha256sum "$PRE_BIN" | cut -c1-16)" = "$(sha256sum "$POST_BIN" | cut -c1-16)" ]; then LG "PRE==POST DIGEST -- arms identical"; exit 1; fi
  LG "DIGESTS $(tr '\n' ';' < "$FP/digests.txt")"
fi

# ---- S1 slope cells ----------------------------------------------------------------------------
slope(){ # slope TAG BIN GEOM [EXTRA] [SETTLE_S]
  local tag=$1 bin=$2 geom=$3 extra=${4:-} settle=${5:-0}
  local out=$FP/slope-$tag.csv
  if [ -z "$tag" ] || [ "${out#*$tag}" = "$out" ]; then LG "HARNESS BUG: tag='$tag' out='$out'"; return 1; fi
  [ -s "$out" ] && { LG "SLOPE $tag on file ($(wc -l < "$out") rows)"; return 0; }
  wait_gate
  local srv cli mode
  case $geom in
    1t) mode=1s; srv=176;     cli="49 50 51";;
    2t) mode=1s; srv=176-177; cli="50 51 178 179";;
    2s) mode=2s; srv=176-177; cli="50 51 178 179";;
    *) LG "bad geom $geom"; return 1;;
  esac
  LG "SLOPE $tag geom=$geom bin=$(basename "$bin") extra='$extra' settle=$settle"
  rm -f "$out.part"
  MODE=$mode RATIO=1:1 RL=1 ATOMIC=${ATOMIC:-1} SHARDS=64 SRVCORES=$srv CLICORES="$cli" CONNS=8 PORT=$PORT_SLOPE \
    N1=300000 N2=900000 PIPE=32 KEYLEN=16 VLEN=32 RINGS=4096 SHAPES="set_over get_hit" PMU_GROUPS="be beu rs" \
    FOREIGNMAX=5 SIBMAX=2 PASS_BUDGET_S=240 QUIET_WAIT_S=120 EXTRA="$extra" SETTLE_S=$settle \
    "$FP/cm/multi.sh" "$bin" "$tag" "$out.part" 2 >>"$FP/slope-$tag.log" 2>&1; local rc=$?
  [ $rc = 0 ] && mv "$out.part" "$out"
  LG "SLOPE $tag rc=$rc rows=$(wc -l < "$out" 2>/dev/null) :: $(grep -E 'settle:|end:' "$FP/slope-$tag.log" | tail -2 | tr '\n' ' ' | cut -c1-300)"
}
if want S1; then
  for geom in 1t 2t 2s; do
    slope "pre-$geom"     "$PRE_BIN"  "$geom"
    slope "post-$geom"    "$POST_BIN" "$geom"
    slope "prenull-$geom" "$PRE_BIN"  "$geom"
  done
fi
if want S1b; then
  slope "pre1-2s"  "$PRE_BIN"  2s "--flip-auto 1 --flip-auto-band 0" 75
  slope "post1-2s" "$POST_BIN" 2s "--flip-auto 1 --flip-auto-band 0" 75
fi

# ---- S2 accuracy: anchored band + false triggers on a stationary mix ---------------------------
acc(){ # acc TAG BIN KIND(mk|hetero)
  local tag=$1 bin=$2 kind=$3
  local out=$FP/acc-$tag.txt
  if [ -z "$tag" ] || [ "${out#*$tag}" = "$out" ]; then LG "HARNESS BUG: tag='$tag' out='$out'"; return 1; fi
  [ -s "$out" ] && { LG "ACC $tag on file :: $(head -1 "$out")"; return 0; }
  wait_gate
  local pid; pid=$(SRV_CPUS=$SRV4 boot "$bin" "$PORT_ON" "acc-$tag" --ratio 2:2 --shards 64 --atomic 0 --flip-auto 1) || return 1
  LG_CPUS=$LG4 preload "$PORT_ON" 4
  local t0; t0=$(date +%s)
  ( while :; do echo "$(( $(date +%s) - t0 )) $(flipinfo $PORT_ON | grep -E '^flipctl_(state|triggers|fingerprint_triggers|signature_band|last_trigger)' | sed 's/flipctl_//' | tr '\n' ' ')"; sleep 2; done ) >"$FP/acc-$tag.trace" 2>&1 &
  local sampler=$!
  case $kind in
    mk) LG_CPUS=$LG4 mk_load "$PORT_ON" 150 4 >"$FP/acc-$tag.mt" 2>&1;;
    hetero) LG_CPUS=50,178 sk_load "$PORT_ON" 150 2 1:0 >"$FP/acc-$tag.mt" 2>&1 & local a=$!
            LG_CPUS=51,179 sk_load "$PORT_ON" 150 2 0:1 >"$FP/acc-$tag.mt2" 2>&1; wait $a;;
  esac
  kill $sampler 2>/dev/null; wait $sampler 2>/dev/null
  local info all; info=$(flipinfo $PORT_ON); all=$($CLI -p $PORT_ON info all | tr -d '\r')
  # triggers AFTER the boot maneuver on a stationary load = false triggers
  echo "$tag kind=$kind state=$(infog "$info" flipctl_state) anchor=$(infog "$info" flipctl_anchor_io):$(infog "$info" flipctl_anchor_ex) band=$(infog "$info" flipctl_signature_band) rate_band=$(infog "$info" flipctl_rate_band) triggers=$(infog "$info" flipctl_triggers) boot=$(infog "$info" flipctl_boot_triggers) fp=$(infog "$info" flipctl_fingerprint_triggers) surge=$(infog "$info" flipctl_rate_surge_triggers) collapse=$(infog "$info" flipctl_rate_collapse_triggers) last=$(infog "$info" flipctl_last_trigger) flips=$(infog "$all" flip_completed) xfer=$(infog "$all" flip_clients_transferred) rate=$(mt_rate "$FP/acc-$tag.mt")$([ -s "$FP/acc-$tag.mt2" ] && echo "+$(mt_rate "$FP/acc-$tag.mt2")")" >"$out.part"
  $CLI -p $PORT_ON debug flipctl >>"$out.part" 2>&1
  mv "$out.part" "$out"; LG "ACC $(head -1 "$out")"
  stop "$pid" "$PORT_ON"
}
if want S2; then
  for kind in mk hetero; do
    acc "pre-$kind"  "$PRE_BIN"  "$kind"
    acc "post-$kind" "$POST_BIN" "$kind"
  done
fi

# ---- S3 wrong-split boots: time-to-first-move (the fingerprint must still let it move) ---------
tm(){ # tm TAG BIN FA BIO BEX SRV LG LGT
  local tag=$1 bin=$2 fa=$3 bio=$4 bex=$5 srv=$6 lg=$7 lgt=$8
  local out=$FP/tm-$tag.txt tr=$FP/tm-$tag.trace
  if [ -z "$tag" ] || [ "${out#*$tag}" = "$out" ]; then LG "HARNESS BUG: tag='$tag' out='$out'"; return 1; fi
  [ -s "$out" ] && { LG "TM $tag on file :: $(cat "$out")"; return 0; }
  wait_gate
  local pid; pid=$(SRV_CPUS=$srv boot "$bin" "$PORT_TM" "tm-$tag" --ratio "$bio:$bex" --shards 64 --atomic 1 --flip-auto "$fa") || return 1
  LG_CPUS=$lg preload "$PORT_TM" "$lgt"
  ( t0=$(date +%s); while :; do
      tc=$($CLI -p $PORT_TM info stats 2>/dev/null | tr -d '\r' | sed -n 's/^total_commands_processed://p')
      sp=$($CLI -p $PORT_TM info server 2>/dev/null | tr -d '\r' | grep -E '^(io|ex)_threads:' | sed 's/.*://' | tr '\n' ':' | sed 's/:$//')
      echo "$(( $(date +%s) - t0 )) ${tc:-0} ${sp:-0:0}"; sleep 1; done ) >"$tr" 2>&1 &
  local sampler=$!
  LG_CPUS=$lg mk_load "$PORT_TM" 120 "$lgt" >"$FP/tm-$tag.mt" 2>&1
  kill $sampler 2>/dev/null; wait $sampler 2>/dev/null
  local info; info=$($CLI -p $PORT_TM info all 2>/dev/null | tr -d '\r')
  $CLI -p $PORT_TM debug flipctl >"$FP/tm-$tag.dbg" 2>&1
  echo "$tag boot=$bio:$bex fa=$fa bin=$(basename "$bin") rate=$(mt_rate "$FP/tm-$tag.mt") flips=$(infog "$info" flip_completed) xfer=$(infog "$info" flip_clients_transferred) live=$(infog "$info" io_threads):$(infog "$info" ex_threads) triggers=$(infog "$info" flipctl_triggers) fp=$(infog "$info" flipctl_fingerprint_triggers) last=$(infog "$info" flipctl_last_trigger) | $(python3 "$FP/ttfm.py" "$tr" "$bio" "$bex" | head -1)" >"$out.part"
  mv "$out.part" "$out"; LG "TM $(cat "$out")"
  stop "$pid" "$PORT_TM"
}
if want S3; then
  tm pre-31   "$PRE_BIN"   1 3 1 "$SRV4" "$LG4" 4
  tm post-31  "$POST_BIN"  1 3 1 "$SRV4" "$LG4" 4
  tm off-31   "$POST_BIN"  0 3 1 "$SRV4" "$LG4" 4
  tm pre6-51   "$PRE_BIN"   1 5 1 "$SRV6" "$LG6" 2
  tm post6-51  "$POST_BIN"  1 5 1 "$SRV6" "$LG6" 2
fi

# ---- S4 gate row -------------------------------------------------------------------------------
ctl(){ # ctl TAG BIN
  local tag=$1 bin=$2
  local out=$FP/ctl-$tag.txt
  if [ -z "$tag" ] || [ "${out#*$tag}" = "$out" ]; then LG "HARNESS BUG: tag='$tag' out='$out'"; return 1; fi
  [ -s "$out" ] && { LG "CTL $tag on file :: $(grep -E '^ok:|AssertionError|anchored off-rail|Error' "$out" | head -1 | cut -c1-200)"; return 0; }
  wait_gate
  local pid; pid=$(SRV_CPUS=$MY_MASK boot "$bin" "$PORT_CTL" "ctl-$tag" --ratio 6:2 --atomic 0 --flip-auto 1 --flip-auto-band 2 --lb-age-sample-rate 1024) || return 1
  taskset -c "$MY_MASK" timeout 300 python3 tests/flipctl.py --host 127.0.0.1 --port "$PORT_CTL" --stable-seconds 30 >"$out.part" 2>&1; local rc=$?
  echo "RC=$rc" >>"$out.part"; flipinfo "$PORT_CTL" >>"$out.part"; $CLI -p "$PORT_CTL" debug flipctl >>"$out.part" 2>&1
  mv "$out.part" "$out"; LG "CTL $tag rc=$rc :: $(grep -E '^ok:|AssertionError|anchored off-rail|Error' "$out" | head -1 | cut -c1-200)"
  stop "$pid" "$PORT_CTL"
}
if want S4; then
  ctl post-1 "$POST_BIN"; ctl post-2 "$POST_BIN"; ctl post-3 "$POST_BIN"
  ctl pre-1 "$PRE_BIN"; ctl pre-2 "$PRE_BIN"; ctl pre-3 "$PRE_BIN"
fi

# ---- S5 batteries both modes -------------------------------------------------------------------
if want S5; then
  for mode in 1s 2s; do
    [ -s "$FP/bat-$mode.txt" ] && { LG "BAT $mode on file :: $(tail -1 "$FP/bat-$mode.txt")"; continue; }
    wait_gate
    bash "$FP/bat.sh" "$POST_BIN" "$mode" "$FP/bat-$mode.txt.part" >"$FP/bat-$mode.log" 2>&1; rc=$?
    mv "$FP/bat-$mode.txt.part" "$FP/bat-$mode.txt"
    LG "BAT $mode rc=$rc :: $(grep -c '^PASS' "$FP/bat-$mode.txt") pass / $(grep -c '^FAIL' "$FP/bat-$mode.txt") fail"
  done
fi

# ---- S6 differ ---------------------------------------------------------------------------------
if want S6 && [ ! -s "$FP/differ.txt" ]; then
  wait_gate
  taskset -c "$MY_MASK" tests/differ_gate.sh "$POST_BIN" "$PORT_DIFF" "$PORT_ORACLE" "$MY_MASK" 6:2 >"$FP/differ.txt.part" 2>&1; rc=$?
  echo "RC=$rc" >>"$FP/differ.txt.part"; mv "$FP/differ.txt.part" "$FP/differ.txt"
  LG "DIFFER rc=$rc :: $(tail -2 "$FP/differ.txt" | tr '\n' ' ' | cut -c1-200)"
fi

# ---- S7 gate quick -----------------------------------------------------------------------------
if want S7 && [ ! -s "$FP/gate-quick.txt" ]; then
  wait_gate
  GATE_CORES=$MY_MASK GATE_PORT=$PORT_GATE GATE_LEDGER=$FP/gate-ledger-quick.txt \
    taskset -c "$MY_MASK" bash tests/gate.sh quick >"$FP/gate-quick.txt.part" 2>&1; rc=$?
  echo "RC=$rc" >>"$FP/gate-quick.txt.part"; mv "$FP/gate-quick.txt.part" "$FP/gate-quick.txt"
  LG "GATE quick rc=$rc :: $(grep -E '^GATE' "$FP/gate-quick.txt" | tail -1)"
fi

# ---- S8 tables ---------------------------------------------------------------------------------
if want S8; then
  for geom in 1t 2t 2s; do
    [ -s "$FP/slope-pre-$geom.csv" ] && [ -s "$FP/slope-post-$geom.csv" ] && \
      python3 "$FP/cm/ab.py" --pre="$FP/slope-pre-$geom.csv" --post="$FP/slope-post-$geom.csv" --shapes=set_over,get_hit --md >"$FP/table-$geom.md" 2>&1
    [ -s "$FP/slope-pre-$geom.csv" ] && [ -s "$FP/slope-prenull-$geom.csv" ] && \
      python3 "$FP/cm/ab.py" --pre="$FP/slope-pre-$geom.csv" --post="$FP/slope-prenull-$geom.csv" --shapes=set_over,get_hit --md >"$FP/table-null-$geom.md" 2>&1
  done
  [ -s "$FP/slope-pre1-2s.csv" ] && [ -s "$FP/slope-post1-2s.csv" ] && \
    python3 "$FP/cm/ab.py" --pre="$FP/slope-pre1-2s.csv" --post="$FP/slope-post1-2s.csv" --shapes=set_over,get_hit --md >"$FP/table-on-2s.md" 2>&1
fi
LG "CHAIN DONE"; date >"$FP/chain.done"

codex
The admission conflict is resolved with the knobs lane’s tests: they require held groups, stall-counter movement, exact replies, and full credit reclamation on every build. The timing lane’s shrink-specific sampling and budgets are superseded because resizing no longer exists; its release-only promotion budget remains. I also found stale flags in older research scripts and live-looking documentation, so I’m checking those for consistency.
exec
/bin/bash -lc "rg -n -g '"'!.merge-inputs/**'"' -g '"'!CODEX-OUT.md'"' 'fpmargin|S1b|band 0|band=0' scratch/flipfp DESIGN-flipfp.md && sed -n '1,85p' scratch/flipfp/fpprobe.sh && sed -n '1,55p' scratch/flipfp/fpshift.sh && sed -n '1,85p' scratchpad/rlbatch/lib.sh && sed -n '1,130p' docs/flipctl-design.md" in /home/user/Projects/cx-merge
 succeeded in 0ms:
DESIGN-flipfp.md:187:**Cost when armed.** With `--flip-auto 1` (anchored, band 0) on 2s: POST still leads PRE by -165
DESIGN-flipfp.md:314:`scratch/flipfp/fpmargin.sh`.
scratch/flipfp/chain.sh:5:#   S1b slope 2s with --flip-auto 1 (anchored controller, band 0) PRE vs POST: the always-on cost
scratch/flipfp/chain.sh:54:if want S1b; then
scratch/flipfp/chain.sh:55:  slope "pre1-2s"  "$PRE_BIN"  2s "--flip-auto 1 --flip-auto-band 0" 75
scratch/flipfp/chain.sh:56:  slope "post1-2s" "$POST_BIN" 2s "--flip-auto 1 --flip-auto-band 0" 75
scratch/flipfp/fpmargin.sh:2:# fpmargin.sh -- THE MARGIN ROW: how far is a real workload change from the learned floor when the
scratch/flipfp/fpmargin.sh:13:#   fpmargin.sh <binary> <tag> <band> <shiftpair>    env: RL (per-conn ops/s, default 4000)
scratch/flipfp/report.sh:23:    echo; echo "### flip-auto 1 (anchored, band 0) 2s $shape: PRE vs POST"; echo
#!/bin/bash
# fpprobe.sh -- VACUITY-PROOF liveness + detection test for the sampled fingerprint.
#
# The accuracy cells left a question the perf numbers cannot answer: a detector that never
# observes anything is both cheap and silent, and "no false triggers" is exactly what a DEAD
# fingerprint reports. So this cell asserts the gate OPENS.
#
#   Leg A (liveness): stationary single-key load, learned band. After the controller anchors,
#     DEBUG FLIPCTL signature_jitter/band must be NONZERO -- observe() only ever runs on a
#     published window, so jitter>0 proves the sampled writer published and the detector read it.
#   Leg B (detection): with the anchor held, swap the workload to 8-key MGET/MSET -- a maximal
#     signature shift (class family, keys_per_multikey, value_bytes/command, pass depth). The
#     fingerprint trigger counter must move off zero.
#
# Both legs run for each arm.  fpprobe.sh <binary> <tag>
source /tmp/claude-1000/-home-user-Projects/ee6eb242-5302-49cf-b767-1a2d8d8f0f61/scratchpad/flipfp/fl.sh
BIN=$1; TAG=$2; W=${W:-}
PORT=$PORT_ON
OUT=$FP/fp-$TAG.txt
SRV_CPUS=$SRV4; LG_CPUS=$LG4
extra=""; [ -n "$W" ] && extra="--flip-work-window $W"
pid=$(SRV_CPUS=$SRV4 boot "$BIN" "$PORT" "fp-$TAG" --ratio 2:2 --shards 64 --atomic 0 \
        --flip-auto 1 --flip-auto-band -1 $extra) || exit 1
LG_CPUS=$LG4 preload "$PORT" 4

# ---- Leg A: stationary load until anchored, then read the detector's learned state -------------
LG_CPUS=$LG4 sk_load "$PORT" 150 4 1:1 >"$FP/fp-$TAG-A.mt" 2>&1 &
load=$!
anchored=no
for i in $(seq 130); do
  st=$(infog "$(flipinfo $PORT)" flipctl_state)
  [ "$st" = anchored ] && { anchored=yes; break; }
  sleep 1
done
dbgA=$($CLI -p $PORT debug flipctl 2>&1 | tr -d '\r')
dbgg(){ echo "$1" | sed -n "s/^$2=//p" | head -1; }
jit=$(dbgg "$dbgA" signature_jitter); band=$(dbgg "$dbgA" signature_band)
dist=$(dbgg "$dbgA" signature_distance)
fpA=$(infog "$(flipinfo $PORT)" flipctl_fingerprint_triggers)
wait $load 2>/dev/null

# ---- Leg B: maximal signature shift on the held anchor ------------------------------------------
LG_CPUS=$LG4 mk_load "$PORT" 90 4 >"$FP/fp-$TAG-B.mt" 2>&1 &
load=$!
fpB=$fpA
for i in $(seq 90); do
  fpB=$(infog "$(flipinfo $PORT)" flipctl_fingerprint_triggers)
  [ "${fpB:-0}" -gt "${fpA:-0}" ] && break
  sleep 1
done
dbgB=$($CLI -p $PORT debug flipctl 2>&1 | tr -d '\r')
wait $load 2>/dev/null
info=$(flipinfo $PORT)

{ echo "$TAG bin=$(basename "$BIN") W=${W:-default} anchored=$anchored"
  echo "  LEG A (liveness): jitter=$jit band=$band distance=$dist fp_triggers=$fpA rate=$(mt_rate "$FP/fp-$TAG-A.mt")"
  echo "  LEG B (detection): fp_triggers=$fpB (was $fpA) shift_distance=$(dbgg "$dbgB" last_shift_distance) shift_band=$(dbgg "$dbgB" last_shift_band) bandB=$(dbgg "$dbgB" signature_band) rate=$(mt_rate "$FP/fp-$TAG-B.mt")"
  echo "  final: state=$(infog "$info" flipctl_state) triggers=$(infog "$info" flipctl_triggers) fp=$(infog "$info" flipctl_fingerprint_triggers)"
  echo "--- debug A ---"; echo "$dbgA"
  echo "--- debug B ---"; echo "$dbgB"; } >"$OUT"
head -4 "$OUT"
stop "$pid" "$PORT"
#!/bin/bash
# fpshift.sh -- RATE-NEUTRAL workload shift: does the fingerprint still detect a mix change?
#
# fpprobe's leg B swapped single-key 1:1 for 8-key MGET/MSET, which collapses the command rate ~9x;
# the rate detector wins that race in both arms, so it cannot attribute detection to the fingerprint.
# Here the command RATE is pinned with --rate-limiting on both sides of the shift and only the CLASS
# changes: phase A is GET-only (class Read, value_bytes/command 0), phase B is SET-only (class Write,
# value_bytes/command 32). Same connections, same pipeline, same per-connection rate. A fingerprint
# trigger in phase B is then attributable to the signature, not to the rate.
#
#   fpshift.sh <binary> <tag>       env: RL (per-connection ops/s, default 4000)
source /tmp/claude-1000/-home-user-Projects/ee6eb242-5302-49cf-b767-1a2d8d8f0f61/scratchpad/flipfp/fl.sh
BIN=$1; TAG=$2; RL=${RL:-4000}
PORT=$PORT_ON
OUT=$FP/fps-$TAG.txt
dbgg(){ echo "$1" | sed -n "s/^$2=//p" | head -1; }
lim_load(){ # lim_load PORT SECS RATIO OUTFILE
  taskset -c "$LG4" memtier_benchmark -s 127.0.0.1 -p "$1" --protocol=redis -t 4 -c 32 --pipeline=32 \
    --ratio="$3" --key-pattern=R:R -d 32 --key-minimum=1 --key-maximum=$KEYMAX --test-time="$2" \
    --rate-limiting=$RL --distinct-client-seed --hide-histogram >"$4" 2>&1
}
pid=$(SRV_CPUS=$SRV4 boot "$BIN" "$PORT" "fps-$TAG" --ratio 2:2 --shards 64 --atomic 0 \
        --flip-auto 1 --flip-auto-band -1) || exit 1
LG_CPUS=$LG4 preload "$PORT" 4

# ---- phase A: GET only, rate pinned, until anchored -------------------------------------------
lim_load "$PORT" 150 0:1 "$FP/fps-$TAG-A.mt" &
load=$!; anchored=no
for i in $(seq 140); do
  [ "$(infog "$(flipinfo $PORT)" flipctl_state)" = anchored ] && { anchored=yes; break; }; sleep 1
done
dbgA=$($CLI -p $PORT debug flipctl 2>&1 | tr -d '\r'); infoA=$(flipinfo $PORT)
fpA=$(infog "$infoA" flipctl_fingerprint_triggers); trA=$(infog "$infoA" flipctl_triggers)
wait $load 2>/dev/null

# ---- phase B: SET only at the SAME pinned rate -------------------------------------------------
lim_load "$PORT" 100 1:0 "$FP/fps-$TAG-B.mt" &
load=$!; fpB=$fpA
for i in $(seq 100); do
  fpB=$(infog "$(flipinfo $PORT)" flipctl_fingerprint_triggers)
  [ "${fpB:-0}" -gt "${fpA:-0}" ] && break; sleep 1
done
dbgB=$($CLI -p $PORT debug flipctl 2>&1 | tr -d '\r')
wait $load 2>/dev/null
infoB=$(flipinfo $PORT)
rA=$(mt_rate "$FP/fps-$TAG-A.mt"); rB=$(mt_rate "$FP/fps-$TAG-B.mt")
{ echo "$TAG bin=$(basename "$BIN") anchored=$anchored rateA=$rA rateB=$rB rate_delta=$(awk -v a="$rA" -v b="$rB" 'BEGIN{if(a>0)printf "%+.1f%%",(b-a)*100/a}')"
  echo "  A(GET-only, anchored): band=$(dbgg "$dbgA" signature_band) jitter=$(dbgg "$dbgA" signature_jitter) triggers=$trA fp=$fpA"
  echo "  B(SET-only, same rate): fp=$fpB (was $fpA) triggers=$(infog "$infoB" flipctl_triggers) surge=$(infog "$infoB" flipctl_rate_surge_triggers) collapse=$(infog "$infoB" flipctl_rate_collapse_triggers) last=$(infog "$infoB" flipctl_last_trigger)"
  echo "     shift_distance=$(dbgg "$dbgB" last_shift_distance) shift_band=$(dbgg "$dbgB" last_shift_band)"
  echo "--- debug A ---"; echo "$dbgA"; echo "--- debug B ---"; echo "$dbgB"; } >"$OUT"
head -4 "$OUT"
stop "$pid" "$PORT"
# Boot/guard helpers for the rlbatch lane. Ports 8075-8078, cores 32-47 / 160-175 ONLY.
PORT=${PORT:-8075}
SRVCORE=${SRVCORE:-32}
CLICORE=${CLICORE:-33}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

port_owners(){ ss -H -ltnp "sport = :$1" 2>/dev/null | sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' | sort -u | paste -sd, -; }

guard_port(){ # a leaked co-binding server silently splits traffic via SO_REUSEPORT: refuse to boot
  local owners; owners=$(port_owners "$1")
  if [ -n "$owners" ] || (exec 3<>/dev/tcp/127.0.0.1/$1) 2>/dev/null; then
    echo "GUARD: port $1 already in use (pid=$owners)" >&2; return 1
  fi
  return 0
}

boot_srv(){ # boot_srv <binary> <logfile> [extra args...] -> SRV pid
  local bin="$1" log="$2"; shift 2
  guard_port "$PORT" || return 1
  # EXTRA_ARGS lets a caller add boot flags (e.g. --read-local-interleave 0) without a second copy
  # of this function; word splitting is intentional.
  # shellcheck disable=SC2086
  taskset -c "$SRVCORE" "$bin" --port "$PORT" --bind 127.0.0.1 --shards 16 \
      --thread-mode fused --read-local "${RL:-1}" ${EXTRA_ARGS:-} "$@" > "$log" 2>&1 &
  SRV=$!
  for _ in $(seq 150); do
    kill -0 "$SRV" 2>/dev/null || { wait "$SRV" 2>/dev/null; echo "BOOT DIED, see $log" >&2; return 1; }
    (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && return 0
    sleep 0.2
  done
  echo "BOOT TIMEOUT, see $log" >&2; return 1
}

stop_srv(){ # kill by PID only; never pkill a pattern that can match this shell
  [ -n "${SRV:-}" ] && kill -TERM "$SRV" 2>/dev/null
  wait "$SRV" 2>/dev/null
  for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null || break; sleep 0.1; done
  SRV=
}
# t-flipctl design and code map

This lane adds one automatic owner around the existing runtime `FLIP`; it does not add another
placement transaction. `Server::flip_begin` and the unchanged `FlipStage` acknowledgements remain
the only way thread roles, clients, listeners, and shards move. The automatic controller ships
dark (`flip-auto 0`).

## Ownership and footprint

- `src/main.cc` is the controller cron owner. When `flip-auto=1`, the main/monitor thread calls
  `Server::flipctl_tick()` once per existing LB tick (`lb-tick-ms`; if that mechanism is disabled,
  the period derives from the provisioned thread count). No IO or EX loop runs controller policy.
- `src/core/flipctl.{h,cc}` contains the fingerprint representation, shift detector, and controller
  state machine. Controller vectors allocate only when `flip-auto=1`.
- Each `ThreadCtx` embeds one `FlipFingerprintWriter`. It has no dynamic storage. Only the physical
  thread playing IO mutates it. There are no fingerprint atomics and no field was added to `Op`,
  `Client`, or `Task`; the existing `sizeof(Op)==336` and `sizeof(Client)==1984` assertions remain
  build gates.
- Cross-owner observation follows the existing exceptional INFO-counter rule: the monitor reads a
  completed monotonic cumulative window and subtracts its last snapshot. A writer stores
  `closed_windows` last, so an unfinished partial window is never consumed. Closing is once per
  `flip-work-window` commands, not per command.

## 1. Workload fingerprint

The dispatch integration is `IoLoop::flip_fingerprint_note()` in `src/core/io_loop.h`, with the
ACL, AUTH, and MULTI early-completion arms calling the same helper from their existing dispatch
hooks. It runs only after the frame has crossed its final backpressure/refusal point.

The writer records:

- one parse-pass bucket (`1`, `2-4`, `5-16`, `17+`) for the number of successfully consumed frames;
- command class, in precedence order: blocking, atomic grouped, multikey write/read, single-key
  write/read, other;
- multikey key sum/count, using the registry's existing `first_key/last_key/key_step` metadata;
- approximate value bytes, the already-parsed argument slice sizes excluding metadata-named keys.

`flip-work-window=100` is the default. Zero makes the helper return without recording and closes no
windows. Windows are command-count based. A deep parse pass can carry a window just over N, but it
pays one close comparison at the end of that already-completed pass; detection latency therefore
continues to scale with work rather than elapsed time.

`flip_signature()` turns pass and class counts into probability vectors and keys/value bytes into
per-command means. `FlipShiftDetector` EWMA-smooths the signature by halves. Its normalized L1
distance gives equal weight to the four feature families. Scalar distances use one key/byte as the
measurement quantum, avoiding the zero-anchor discontinuity where one rare admin argument would
otherwise have distance one.

With `flip-auto-band=-1`, the band is twice the maximum quiet adjacent/drift distance learned at
the final split, floored by the four-family sampling quantum divided by observed commands. A
numeric value is a percent. Zero disables fingerprint and collapse re-triggers. Final anchor
learning consumes a number of closed aggregates derived from the provisioned thread count. It is
done after maneuver-only age sampling is disarmed, so the controller cannot detect its own signal
cost transition as a workload change.

The detector remains active during a seek. After it learns the current maneuver signature, another
shift abandons all recorded split/rate readings and restarts capacity measurement. If an ownership
FLIP has already committed, the restart is deferred until that one transaction reaches `Idle`;
another FLIP is never overlapped.

## 2. Jump with deliberate overshoot

The boot maneuver remains pending until the command-rate EWMA's relative slope across five ticks
falls below a threshold derived from its own change jitter and measurement quantum. Directional
connection ramp-up therefore remains drift, while jittery stationary traffic can qualify even when
its absolute rate never enters a quiet band. A 30-second ceiling, converted to ticks from
`lb-tick-ms`, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
thread per tick resets the learning/cap state, so an idle server remains in
`awaiting-load-stability` and never pays for a maneuver it cannot use.

`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. An explicit
`lb-age-sample-rate` is used when nonzero; otherwise the rate derives from the provisioned thread
count. IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
writes. IO releases its ROB-head age map when the rate returns to zero.

After a stabilized baseline rate, `issue_initial_jump()` subtracts the per-thread maneuver window.
For each role it computes:

```
spin_frac_i = delta(spins_i) / delta(iterations_i)
cap_role = sum(delta(ops_i)) /
           sum(delta(busy_ns_i) * (1 - spin_frac_i))
demand_role = 1 / cap_role
nio_eq = round(total * demand_io / (demand_io + demand_ex))
```

Both role-constraint guards use the role's busiest corrected-busy thread. Thus a loaded owner is
not vetoed by idle peers. If either role has no measured work/busy quantity, measurement continues;
the controller does not invent a workload prior.

The calculation is performed in placement units (one logical CPU normally, one sibling pair under
`smt-mode=1`) and then reported/committed in threads. The target passes `nio_eq` in the measured
direction by `max(1, abs(nio_eq-nio_now)/2)`, clamped to one live unit of each role. Even when the
equalized split rounds to the current split, the direction of greater measured demand supplies the
deliberate one-unit overshoot.

## 3. Return on throughput

`sample_stabilized_rate()` derives command rate from the monotonic, unique public-command counter
in each `ThreadCtx`. It invalidates both subwindows whenever any of these moves:

- the FLIP stage or continuous-LB stage is non-idle;
- snapshot/load state is active;
- `flip_completed`, `flip_refused`, client rebinds, key-bucket moves, or client moves changes.

Only two consecutive quiescent subwindows within the learned rate band produce a reading; their
mean is the stabilized rate. During final settling, the rate band retains twice the maximum
quiet-state pair/range jitter seen across the thread-count-derived anchor-learning period, and the
anchored rate is the mean of those stabilized readings. No rate sample taken across redistribution
can enter `readings_` or a comparison.

The initial stabilized split/rate is recorded before the jump. Thereafter `seek_after_reading()` is
rate-only: signal counters are not consulted. The first seek step is the overshoot distance. After
every stabilized reading it keeps direction on improvement, reverses on worsening, and halves the
step with a floor of one placement unit. At unit step, revisiting a stabilized split or observing
two reversals terminates the seek. `settle()` selects the argmax of all stabilized readings. If that
split is not current, it issues one final ordinary internal FLIP and waits for fresh stabilization
there before anchoring.

## 4. Anchor

`FlipController::anchor()` stores the final IO/EX split, stabilized public-command rate, anchored
fingerprint, and rate/fingerprint bands. It publishes age sampling zero and holds. While anchored,
redistribution-free, in-band two-window readings update the rate reference with a 64-observation
EWMA. Auto mode may widen the rate band from that EWMA's live innovation jitter, but retains the
band learned during final settling as a floor. Out-of-band trigger evidence and maneuver windows
never enter either baseline.

While anchored:


exec
/bin/bash -lc "sed -n '1220,1450p' src/core/ex_loop.h && rg -n 'sizeof\\((Config|Op|Client|ThreadCtx|Shard|FlatStore|Rob<64>|AtomicEntry)\\)|lb_autotune_|key_lb_enabled|client_lb_enabled|shard_home|atomic_window|read_local_atomic_filter|read_local_prefetch_capture|persist_io' src/core/config.h src/core/server.h src/core/ex_loop.h src/store/flatstore.h src/core/placement.h src/cmd/t_server.cc" in /home/user/Projects/cx-merge
 succeeded in 0ms:
                    // lazy-expiry side effect and is an ordinary array nil element.
                    reply_null(op.sink(), op.resp3());
                    prepared.keyspace_misses++;
                    if (!window.use_epochs && !store.read_local_validate(probe.state)) {
                        transient = ReadLocalFallbackReason::SeqChurn;
                        retry = true;
                    }
                    continue;
                }

                const KvObj* object = probe.object;
                if (!object) std::abort();
                const uint8_t flags = object->read_local_flags();
                if (static_cast<Type>(object->type) != Type::String) {
                    read_local_clear_reply(op);
                    return {ReadLocalFallbackReason::Typed};
                }
                if (flags & KvObjFlags::HasTtl) {
                    const int64_t deadline = object->read_local_expire_at_ms(flags);
                    if (deadline >= 0 && deadline <= command_now_ms) {
                        // Unlike a plain stable miss, expiry-due needs the owner to perform lazy
                        // expiry and its accounting/notifications, so one such key demotes all
                        // MGET.
                        read_local_clear_reply(op);
                        return {ReadLocalFallbackReason::Expired};
                    }
                }

                const Enc encoding = object->encoding();
                if (encoding == Enc::Int) {
                    char text[24];
                    const uint32_t length = i64_to_dec(
                        text, object->read_local_int_value(flags));
                    reply_bulk(op.sink(), Slice(text, length));
                } else if (encoding == Enc::Raw || encoding == Enc::Extern) {
                    if (!read_local_reply_string(op, object, flags)) {
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
                        self_->read_local_stats().settax.object_sequence_retries++;
#endif
                        transient = ReadLocalFallbackReason::SeqChurn;
                        retry = true;
                        break;
                    }
                } else {
                    read_local_clear_reply(op);
                    return {ReadLocalFallbackReason::Typed};
                }
                prepared.keyspace_hits++;
                // Account only for a key this pass actually accepted, so a churned read that is
                // about to be retried or demoted never records an access it did not serve.
                if (!window.use_epochs && !store.read_local_validate(probe.state)) {
                    transient = ReadLocalFallbackReason::SeqChurn;
                    retry = true;
                } else if (__builtin_expect(maxmemory_enabled_, false)) {
                    note_local_read_access(op, object, flags);
                }
            }

            // The complete reply is still private. Accept only if every participant is unchanged
            // under its own rule since before the first value load.
            if (!retry) {
                transient = local_mget_window_close(
                    window, touched, hashes, shards, key_count);
                retry = transient != ReadLocalFallbackReason::None;
            }
            if (!retry) return prepared;
            read_local_clear_reply(op);
            if (attempt + 1 < kAttempts)
                self_->read_local_stats().mget_generation_retries++;
        }
        return {local_mget_final_reason(
            op, hashes, shards, key_count, cached_routes, transient)};
    }

    PreparedLocalRead prepare_captured_local_mget(Op& op) {
        static constexpr uint32_t kAttempts = 2;
        const uint32_t key_count = op.argc() - 1;
        if (!key_count || srv_->nshards() > LocalMgetWindow::kMaxShards) std::abort();

        uint64_t route_hashes[LocalMgetWindow::kMaxEpochKeys];
        int32_t route_shards[LocalMgetWindow::kMaxEpochKeys];
        const bool cached_routes = key_count <= LocalMgetWindow::kMaxEpochKeys;
        uint64_t touched[LocalMgetWindow::kMaxShards / 64] = {};
        for (uint32_t key = 0; key < key_count; key++) {
            const uint64_t hash = FlatStore::hash_key(op.arg(key + 1));
            const int32_t shard_id = srv_->router().shard_of(hash);
            touched[static_cast<uint32_t>(shard_id) >> 6] |=
                uint64_t{1} << (static_cast<uint32_t>(shard_id) & 63);
            if (cached_routes) {
                route_hashes[key] = hash;
                route_shards[key] = shard_id;
            }
        }
        const int64_t command_now_ms = cached_now_ms_;
        if (__builtin_expect(debug_fanout_defer_us_ != 0, false)) debug_fanout_stall_local();

        ReadLocalFallbackReason transient = ReadLocalFallbackReason::Generation;
        for (uint32_t attempt = 0; attempt < kAttempts; attempt++) {
            LocalMgetWindow window;
            PreparedLocalRead prepared;
            read_local_clear_reply(op);

            transient = local_mget_window_open(
                window, touched, route_hashes, route_shards, key_count, cached_routes);
            bool retry = transient != ReadLocalFallbackReason::None;
            if (!retry) reply_array_header(op.sink(), key_count);

            for (uint32_t first = 0; first < key_count && !retry;
                 first += kReadLocalPrefetchKeys) {
                const uint32_t count = std::min<uint32_t>(
                    key_count - first, kReadLocalPrefetchKeys);
                uint64_t hashes[kReadLocalPrefetchKeys];
                int32_t shards[kReadLocalPrefetchKeys];
                ReadLocalCaptureBuffer<true, kReadLocalPrefetchKeys> captures;

                // I0 warms every home word in this bounded window. C0 then performs the complete
                // key-verified walk and prefetches the exact object's value before E0 copies it.
                for (uint32_t offset = 0; offset < count; offset++) {
                    if (cached_routes) {
                        hashes[offset] = route_hashes[first + offset];
                        shards[offset] = route_shards[first + offset];
                    } else {
                        hashes[offset] = FlatStore::hash_key(op.arg(first + offset + 1));
                        shards[offset] = srv_->router().shard_of(hashes[offset]);
                    }
                    srv_->shard(shards[offset]).store().read_local_prefetch(hashes[offset]);
                }
                for (uint32_t offset = 0; offset < count; offset++) {
                    captures.entries[offset] =
                        srv_->shard(shards[offset]).store().read_local_prefetch_capture(
                            hashes[offset], op.arg(first + offset + 1));
                }

                for (uint32_t offset = 0; offset < count && !retry; offset++) {
                    const int32_t shard_id = shards[offset];
                    FlatStore& store = srv_->shard(shard_id).store();
                    const FlatStore::ReadLocalPrefetchCapture& capture =
                        captures.entries[offset];
                    if (capture.result == FlatStore::ReadLocalProbeResult::AtomicPending) {
                        read_local_clear_reply(op);
                        return {ReadLocalFallbackReason::AtomicPending};
                    }
                    if (capture.result == FlatStore::ReadLocalProbeResult::Churn) {
                        transient = ReadLocalFallbackReason::SeqChurn;
                        retry = true;
                        break;
                    }
                    if (capture.result == FlatStore::ReadLocalProbeResult::Missing) {
                        reply_null(op.sink(), op.resp3());
                        prepared.keyspace_misses++;
                        if (!window.use_epochs && !store.read_local_validate(capture.state)) {
                            transient = ReadLocalFallbackReason::SeqChurn;
                            retry = true;
                        }
                        continue;
                    }

                    const KvObj* object = capture.object;
                    if (!capture.slot || !object) std::abort();
                    const uint8_t flags = object->read_local_flags();
                    if (static_cast<Type>(object->type) != Type::String) {
                        read_local_clear_reply(op);
                        return {ReadLocalFallbackReason::Typed};
                    }
                    if (flags & KvObjFlags::HasTtl) {
                        const int64_t deadline = object->read_local_expire_at_ms(flags);
                        if (deadline >= 0 && deadline <= command_now_ms) {
                            read_local_clear_reply(op);
                            return {ReadLocalFallbackReason::Expired};
                        }
                    }

                    const Enc encoding = object->encoding();
                    if (encoding == Enc::Int) {
                        char text[24];
                        const uint32_t length = i64_to_dec(
                            text, object->read_local_int_value(flags));
                        reply_bulk(op.sink(), Slice(text, length));
                    } else if (encoding == Enc::Raw || encoding == Enc::Extern) {
                        if (!read_local_reply_string(op, object, flags)) {
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
                            self_->read_local_stats().settax.object_sequence_retries++;
#endif
                            transient = ReadLocalFallbackReason::SeqChurn;
                            retry = true;
                            break;
                        }
                    } else {
                        read_local_clear_reply(op);
                        return {ReadLocalFallbackReason::Typed};
                    }
                    prepared.keyspace_hits++;
                    // See prepare_local_mget: accept first, then account.
                    if (!window.use_epochs && !store.read_local_validate(capture.state)) {
                        transient = ReadLocalFallbackReason::SeqChurn;
                        retry = true;
                    } else if (__builtin_expect(maxmemory_enabled_, false)) {
                        note_local_read_access(op, object, flags);
                    }
                }
            }

            // C0 performs each probe's topology validation. This one outer close then validates
            // every queried cell across the complete multi-shard capture/copy interval.
            if (!retry) {
                transient = local_mget_window_close(
                    window, touched, route_hashes, route_shards, key_count);
                retry = transient != ReadLocalFallbackReason::None;
            }
            if (!retry) return prepared;
            read_local_clear_reply(op);
            if (attempt + 1 < kAttempts)
                self_->read_local_stats().mget_generation_retries++;
        }
        return {local_mget_final_reason(
            op, route_hashes, route_shards, key_count, cached_routes, transient)};
    }

    // Build one local reply but do not publish Done. `fallback == None` means the bytes are
    // complete and the reply retains no FlatStore pointer. A caller-owned capture may remain on
    // this rotation's stack until drain_local_reads_bounded_impl returns. Selective commit below
    // keeps an overlapping younger read behind any operation that needs the owner path.
    // `op`, `store` and `mget` were resolved once by the chunk gather; a point read passes its
    // home store, an MGET passes null and takes its own multi-store path.
    template <bool CapturePrefetch>
    PreparedLocalRead prepare_local_read(
            Op& op, FlatStore* home, bool mget,
            const FlatStore::ReadLocalPrefetchCapture* captured = nullptr) {
        if (mget) {
            if (captured) std::abort();
            if constexpr (CapturePrefetch) return prepare_captured_local_mget(op);
src/core/placement.h:176:    bool assign_shard_homes(uint32_t nshards) {
src/core/placement.h:178:        shard_home_.resize(nshards);
src/core/placement.h:180:            shard_home_[sid] = ex_[sid % ex_.size()];
src/core/placement.h:283:    uint32_t shard_home(uint32_t sid) const { return shard_home_[sid]; }
src/core/placement.h:393:        shard_home_.clear();
src/core/placement.h:423:    std::vector<uint32_t> shard_home_;
src/cmd/t_server.cc:244:    const uint64_t total_mem = sizeof(Client) + client.rcap() + omem +
src/cmd/t_server.cc:2159:                      "atomic_promotions:%llu\r\natomic_window_stalls:%llu\r\n"
src/cmd/t_server.cc:2264:                static_cast<unsigned long long>(g_server ? g_server->atomic_window_stalls() : 0),
src/core/config.h:18://   sizeof(Op)==336, sizeof(Client)==1984     footprint locks (static_assert, do not move)
src/core/config.h:367:static_assert(sizeof(Config) == 528, "Config footprint changed; update the documented accounting");
src/core/ex_loop.h:1349:                        srv_->shard(shards[offset]).store().read_local_prefetch_capture(
src/core/ex_loop.h:1462:                    store.read_local_prefetch_capture(op.hash, op.key());
src/core/ex_loop.h:1659:                            chunk.stores[i]->read_local_prefetch_capture(op.hash, op.key());
src/store/flatstore.h:949:    ReadLocalPrefetchCapture read_local_prefetch_capture(uint64_t hash, Slice key) const {
src/store/flatstore.h:3899:static_assert(sizeof(FlatStore) == 944);
src/core/server.h:251:        live_atomic_window_.store(resolved_window, std::memory_order_relaxed);
src/core/server.h:263:        if (!placement_.assign_shard_homes(cfg.shards)) return false;
src/core/server.h:368:            const uint32_t tid = placement_.shard_home(sid);
src/core/server.h:2457:    uint32_t atomic_window() const {
src/core/server.h:2458:        return live_atomic_window_.load(std::memory_order_acquire);
src/core/server.h:2462:            atomic_reconfigure_credits(atomic_window());
src/core/server.h:2466:            atomic_reconfigure_credits(atomic_window());
src/core/server.h:2478:        const uint32_t window = atomic_window();
src/core/server.h:2492:        const uint32_t window = atomic_window();
src/core/server.h:2517:                atomic_window_stalls_.fetch_add(1, std::memory_order_relaxed);
src/core/server.h:2554:            if (atomic_window() && !(generation & 1) && lease.generation == generation &&
src/core/server.h:2833:    uint64_t atomic_window_stalls() const {
src/core/server.h:2834:        return atomic_window_stalls_.load(std::memory_order_relaxed);
src/core/server.h:3318:        if (!atomic_window()) return;
src/core/server.h:3369:        live_atomic_window_.store(window, std::memory_order_release);
src/core/server.h:3554:    std::atomic<uint32_t> live_atomic_window_{256};
src/core/server.h:3563:    std::atomic<uint64_t> atomic_window_stalls_{0};

exec
/bin/bash -lc "python3 - <<'PY'
from pathlib import Path
import re
pattern = re.compile(r'read-local-prefetch-capture|read-local-atomic-filter|read-local-interleave|flip-auto-band|l3-domains|smt-mode|genthread-schedule|atomic-window|persist-io|lru-clock-shift|script-crossshard-(?:max-bytes|workbench-bytes|conflict-retries|cut-slots)|tls-ktls|key-lb|client-lb|lb-(?:sample-rate|age-sample-rate|tick-ms|imbalance-pct|move-cap|cooldown-ms)|--ex-sched|--overlap|thread-pipeline')
for path in [Path('DESIGN.md'), Path('docs/flipctl-design.md'), Path('docs/specs/AUDIT-TLS.md')]:
    print(str(path))
    for n, line in enumerate(path.read_text().splitlines(), 1):
        if pattern.search(line): print(n, line)
PY
sed -n '286,319p' NOTES-XPERF2.md
sed -n '340,367p' NOTES-HYGIENE.md
sed -n '455,485p' NOTES-HYGIENE.md
sed -n '300,320p' DESIGN-flipfp.md
sed -n '175,200p' tests/flipctl_unit.cc
sed -n '245,267p' tests/flipctl_unit.cc
sed -n '1,45p' scratch/rv3.sh" in /home/user/Projects/cx-merge
 succeeded in 0ms:
DESIGN.md
12 The compatibility options `thread-pipeline` and `genthread-schedule` remain accepted: the former
docs/flipctl-design.md
11   `Server::flipctl_tick()` once per existing LB tick (`lb-tick-ms`; if that mechanism is disabled,
49 With `flip-auto-band=-1`, the band is twice the maximum quiet adjacent/drift distance learned at
67 `lb-tick-ms`, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
72 `lb-age-sample-rate` is used when nonzero; otherwise the rate derives from the provisioned thread
92 `smt-mode=1`) and then reported/committed in threads. The target passes `nio_eq` in the measured
146 - `src/core/config.h` is the sole parser/default source for `flip-auto`, `flip-auto-band`, and
docs/specs/AUDIT-TLS.md
6 > and `tls-ktls yes` (`tomokv.conf`, NETWORK).

Every assertion is a RATIO or a growth bound between two measurements taken on the same connection
or the same rig minutes apart — never an absolute time — and every battery was checked against the
pre-fix binary to prove it can actually fail.

| battery | boot it needs | asserts | pre-fix | post-fix |
|---|---|---|---|---|
| `tests/expireindex.py <host> <port>` | `--shards 1 --ratio 1:1 --enable-debug-command yes` | growth-trigger cost ratio over a 16× population ≤ 8.0; live→0 DEL ≤ 1.25× a neighbouring DEL | FAIL 2 (16.30×, 2.00×) | PASS (1.65×, 1.00×) |
| `tests/borrow_registry.py <host> <port>` | `--shards 1 --zc-min 64 --client-output-buffer-limit normal 0 0 0 --enable-debug-command yes` | borrowed-GET per-op cost growth over 0 → ~2000 live borrows ≤ 1.05 | FAIL (1.078, 1.079) | PASS (1.008, 1.008, 1.009) |
| `tests/xshard_dispatch_scale.sh` (+ `.py`) | boots its own 4-thread and 128-thread arms | dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |

Each also carries its own non-vacuity checks: `expireindex.py` asserts INFO `expires` really
reached the population under test and that INFO `expired_keys` MOVED (so the sampler is proven to
still visit deadlines parked mid-migration), plus a no-TTL negative control on both timing legs;
`borrow_registry.py` asserts `CLIENT LIST oll` grew (the borrow path is proven live, not assumed),
carries a non-borrowed control arm, and refuses to run unless the zc gate straddles its two probe
sizes; `xshard_dispatch_scale.py` refuses to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` prove
the cross pair really fanned out to two owner threads and the control stayed on one.

None of the three is wired into `tests/gate.sh`: they each need their own boot geometry and the
gate's port/cores belong to the mainline operator. Run them as above.

## Evidence

```
DIFFER string:  4033 ops, 0 diffs -> PASS   (seeds 5, 13, 29)
DIFFER hexpire: 4288 ops, 0 diffs -> PASS   (seeds 5, 13, 29)
DIFFER xshard:  4276 ops, 0 diffs -> PASS   (seeds 5, 13, 29)

--atomic 0 and --atomic 1, both clean:
  hexpire torture ryow limits multi_exec dumprestore stream zc tracking lua_scripting resp3  PASS
  stuck: live_conns=0 rob_not_quiesced=0 unsent_bytes_pending=0

evict_battery: off noev lru vlru vttl lfu growth config   PASS
total_net_input_bytes:0
total_net_output_bytes:0
cmdstat_set:calls=3,usec=0,usec_per_call=0.00,rejected_calls=0,failed_calls=0
db0:keys=2,expires=1,avg_ttl=0
INFO after DEL:
used_memory_dataset:32
used_memory_peak:32
cmdstat_set:calls=3,usec=0,usec_per_call=0.00,rejected_calls=0,failed_calls=0
db0:keys=1,expires=1,avg_ttl=0
auth_failures before_reset=1 after_reset=1
~~~

## Test-file census

The calibration examples expireindex.py, borrow_registry.py, and xshard_dispatch_scale.sh were
already wired in the current parent. aof_frames.py is indirectly executed by aof_frame_order.py;
aof_rewrite.py and aof_rewrite_triggers.py are driven by matrix wrappers; differ.py is driven by
differ_gate.sh; niclib.sh and gate_refs.txt support optional full-tier NIC rows.

The five accidentally omitted batteries are now wired. These remain outside the gate:

- Recovery/manual: edgetime_persist.sh, hexpire_persist.sh, flush_capture.py.
- Incomplete: evict_battery.py (missing its documented driver).
- Slow/campaign/performance: lru_slow.sh, writer_atomic.py, writer_atomic_campaign.py,
  benchfeat.py, broaden_bench.cc.
- Standalone overlap: wiredump.py. Full differ has an internal wiredump suite, but the standalone
  boot/CLI path is not invoked.

XSHARD-DISPATCH-SCALE PASS (best 128t/4t excess ratio=0.960, limit=1.20)
notify: ok (notify_events_fired=1608)
~~~

Purpose-boot matrix commands:

~~~sh
PERSIST_IO=normal GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_matrix.sh
PERSIST_IO=normal GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_trigger_matrix.sh
PERSIST_IO=uring  GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_matrix.sh
PERSIST_IO=uring  GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_trigger_matrix.sh
~~~

Other purpose-booted battery invocations (each boot used the knobs documented by gate.sh):

~~~sh
python3 tests/xscript.py 127.0.0.1 7600 off|limit|window
python3 tests/expireindex.py 127.0.0.1 7600
python3 tests/borrow_registry.py 127.0.0.1 7600
XDS_PORT=7600 XDS_CPUS=128-143 XDS_BIN=./build/tomokv \
  bash tests/xshard_dispatch_scale.sh
python3 tests/dumprestore.py 127.0.0.1 7600 prepare_restart|verify_restart
python3 tests/auth.py 127.0.0.1 7600 gatepass
python3 tests/acl.py 127.0.0.1 7600 ACL_FILE
python3 tests/debug.py 127.0.0.1 7600
python3 tests/snap_cut_battery.py 7600 save|verify_cut
python3 tests/snap_cut_battery.py 7600 atomic_groups FILE mset 5
python3 tests/snap_cut_battery.py 7600 atomic_groups FILE exec 3
python3 tests/snap_typed_roundtrip.py 7600 build_save|verify
python3 tests/snap_typed_race.py 7600 race|verify
python3 tests/aof.py 127.0.0.1 7600 populate|loadaof|verify|snapshot STATE
The measurement lands on the sampled value. The consequence is the conservative direction the lane
asked about: under sampling the floor RISES by W, so the band widens to match the estimator's noise
and is never too tight. The failure mode left to check is therefore the opposite one -- deafness.

The other two quanta are rate quanta (`automatic_rate_band`, flipctl.cc:273, and the boot rate floor
at :417). Both are computed from `total_commands`, which is NOT sampled and which this lane does not
touch, so they are correct as they stand.

### 6.2 The deafness margin, measured

A real class change driven at a pinned command rate, so the fingerprint is not preempted by the rate
detector. The signal and the floor cannot be read from one run -- with a learned band the trigger
fires and `reset()` wipes the distance -- so each arm is run twice: `--flip-auto-band 90` holds the
anchor and lets the distance converge (the signal), `--flip-auto-band -1` learns the floor.
`scratch/flipfp/fpmargin.sh`.

**BITCOUNT -> INCR** (the redesign lane's reference change), 2:2 split, `--rate-limiting` pinned:

| | PRE (exhaustive) | POST (1 pass in 100) |
|---|---:|---:|
| rate across the swap | 511979 -> 512009 (**+0.0%**) | 512001 -> 512011 (**+0.0%**) |
    // Scripted quiet-state count noise stays inside the signature's own learned band.
    if (detector.observe(quiet(80, 20, 1)) || detector.observe(quiet(79, 21)))
        fail("quiet noise fired the shift detector");
    if (detector.band() != anchored_band)
        fail("post-anchor traffic changed the learned quiet-state band");

    // The same command count changes pipe-depth, command class, keys/op and value bytes together.
    if (!detector.observe(multikey_mix()))
        fail("scripted workload mix change did not fire");

    // THE TYPED BAND IS A FLOOR, NOT A CEILING. The gate-hygiene lane's instrumented failure: a
    // driver holding its rate to 0.07% across 34 samples, a fingerprint distance of 0.2518 against
    // a flat --flip-auto-band 2 (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
    // A detector whose signature genuinely moves between adjacent quiet windows must end up with a
    // band above that movement whatever the operator typed, and must still fire on a real change.
    {
        FlipShiftDetector typed(2, 4);   // --flip-auto-band 2
        // Stationary but NOISY: the read/write split alternates 35:65 / 65:35 window to window.
        // The detector smooths the signature, so the anchored distance this produces is ~0.09.
        const auto noisy = [](int k) { return quiet(k % 2 ? 65 : 35, k % 2 ? 35 : 65); };
        for (int k = 0; k < 8; k++)
            if (typed.observe(noisy(k))) fail("a pre-anchor window fired the detector");
        typed.anchor();
        if (!(typed.band() > 0.15))
            fail("a typed band ignored a signature that moves between quiet windows");
        int fires = 0;
            fail("the learned band did not take the noise floor");
        if (!(autob.band() > 2.0 / std::sqrt(10000.0)))
            fail("the learned band stayed at the count quantum under a noisy signature");
        int fires = 0;
        for (int k = 8; k < 24; k++) fires += autob.observe(noisy(k)) ? 1 : 0;
        if (fires) fail("the learned band fired on quiet-state movement");
    }
    // A TYPED BAND UNDER THE REAL SAMPLER. The shipped configuration cannot reach this -- the
    // controller runs the learned band -- but an operator who sets --flip-auto-band AND the sampler
    // can, and on a tree where the typed path bypasses the floor the fingerprint lane measured 73
    // two-consecutive exceedances in 600 stationary windows at K=100 and 208 at K=60: a spurious
    // maneuver every few seconds on a load that never changed. Driven here through THEIR writer, a
    // stationary 50/50 stream at depth 32, with the floor in place: none, and a real mix change
    // still clears. The window is what the writer actually published, so K is the SAMPLED count.
    {
        for (uint32_t window : {60u, 100u}) {
            XorShift rng;
            FlipShiftDetector typed(2, 8);   // --flip-auto-band 2, the operator's knob
            int fires = 0, confirmed = 0, streak = 0;
            for (int w = 0; w < 60; w++) {
                FlipFingerprintWriter writer;
                writer.configure(1);          // sample every pass; `window` frames make one window
                drive(writer, rng, window, 1, [&rng](uint64_t, uint32_t) {
#!/bin/bash
# rv3.sh -- build + verify the fingerprint-band floor: STABLE HOLD (tests/flipctl.py, whose stable
# phase is the row that was failing) x3 with the typed --flip-auto-band 2 the gate uses, the directed
# battery, and the two wrong-split boots + the stationary cell (must still move / must not move).
source /home/user/Projects/wt-flipdamp/scratch/lib.sh; cd "$WT"
LG(){ echo "$(date +%T) $*"; }
wait_gate(){ local n=0; while ! gate_ok; do [ $((n%10)) = 0 ] && LG "paused"; n=$((n+1)); sleep 30; done; }
SRV8=52,53,54,55,180,181,182,183; LG8=56,57,184,185
boot8(){ local SRV_CPUS=$SRV8; boot "$@"; }
wait_gate
taskset -c 52-57,180-185 make -j12 >"$SP/fd-r6-build.txt" 2>&1; rc=$?
LG "BUILD rc=$rc diag=$(grep -c -E 'error|warning' "$SP/fd-r6-build.txt") sha=$(sha256sum "$FIX_BIN" | cut -c1-16)"
[ $rc = 0 ] || exit 1
taskset -c 52-57,180-185 make unit >"$SP/fd-r6-unit.txt" 2>&1; LG "UNIT rc=$?"
# STABLE HOLD x3 -- the failing row. Report band, distance and ratio whatever the verdict.
for r in 1 2 3; do
  out=$SP/fd-r6-ctl-$r.txt; wait_gate
  pid=$(boot8 "$FIX_BIN" "$PORT_BAT" "r6ctl-$r" --ratio 6:2 --atomic 0 --flip-auto 1 --flip-auto-band 2 --lb-age-sample-rate 1024) || continue
  taskset -c "$LG8" timeout 300 python3 tests/flipctl.py --host 127.0.0.1 --port "$PORT_BAT" --stable-seconds 30 >"$out" 2>&1; rc=$?
  echo "RC=$rc" >>"$out"; redis-cli -p "$PORT_BAT" debug flipctl >>"$out" 2>&1
  LG "CTL $r rc=$rc :: $(grep -E 'stable hold|anchored off-rail|^ok:|AssertionError' "$out" | head -2 | tr '\n' ' ' | cut -c1-200) :: $(grep -E '^signature_(band|distance|noise_bound)|^last_trigger' "$out" | tr '\n' ' ')"
  stop "$pid" "$PORT_BAT"
done
out=$SP/fd-r6-costgate.txt; wait_gate
pid=$(boot "$FIX_BIN" "$PORT_HOLD" r6cg --ratio 3:1 --shards 64 --atomic 1 --flip-auto 1) && {
  taskset -c "$LG_CPUS" timeout 600 python3 tests/flip_cost_gate.py 127.0.0.1 "$PORT_HOLD" >"$out" 2>&1
  LG "COSTGATE rc=$? :: $(grep -E '^ok:|AssertionError' "$out" | tail -1 | cut -c1-140)"; stop "$pid" "$PORT_HOLD"; }
for tag in red-a red-b; do
  out=$SP/fd-r6-hold-$tag.txt; wait_gate; ./scratch/hold.sh "$FIX_BIN" >"$out" 2>&1
  LG "HOLD $tag :: $(grep -E '^ok:|AssertionError' "$out" | tail -1 | cut -c1-160)"
done
tm(){ local tag=$1 bio=$2 bex=$3 srv=${4:-$SRV_CPUS} lg=${5:-$LG_CPUS} lgt=${6:-$LG_THREADS}
  local out=$SP/fd-r6-tm-$tag.txt tr=$SP/fd-r6-tmtrace-$tag.txt secs=120 pid mt; wait_gate
  pid=$(SRV_CPUS=$srv boot "$FIX_BIN" "$PORT_SIG" "r6tm-$tag" --ratio "$bio:$bex" --shards 64 --atomic 1 --flip-auto 1) || return 1
  LG_CPUS=$lg preload "$PORT_SIG"
  ( t0=$(date +%s); while :; do tc=$(redis-cli -p "$PORT_SIG" info stats 2>/dev/null | tr -d '\r' | sed -n 's/^total_commands_processed://p'); sp=$(redis-cli -p "$PORT_SIG" info server 2>/dev/null | tr -d '\r' | grep -E '^(io|ex)_threads:' | sed 's/.*://' | tr '\n' ':' | sed 's/:$//'); echo "$(( $(date +%s) - t0 )) ${tc:-0} ${sp:-0:0}"; sleep 1; done ) >"$tr" 2>&1 &
  local sampler=$!
  LG_CPUS=$lg LG_THREADS=$lgt ./scratch/mk.sh "$PORT_SIG" "$secs" >"$SP/fd-r6-tmmt-$tag.txt" 2>&1
  kill "$sampler" 2>/dev/null; wait "$sampler" 2>/dev/null
  local info; info=$(redis-cli -p "$PORT_SIG" info all 2>/dev/null | tr -d '\r')
  redis-cli -p "$PORT_SIG" debug flipctl >"$SP/fd-r6-tmdbg-$tag.txt" 2>&1
  mt=$(tr '\r' '\n' <"$SP/fd-r6-tmmt-$tag.txt")
  echo "$tag boot=$bio:$bex rate=$(echo "$mt" | awk '/^Totals/{print $2}') flips=$(infog "$info" flip_completed) xfer=$(infog "$info" flip_clients_transferred) trig=$(infog "$info" flipctl_triggers) fp_trig=$(infog "$info" flipctl_fingerprint_triggers) decision=$(infog "$info" flipctl_model_last_decision) refine=$(infog "$info" flipctl_refine_decision)/$(infog "$info" flipctl_refine_steps) sig_band=$(infog "$info" flipctl_signature_band) live=$(infog "$info" io_threads):$(infog "$info" ex_threads) | $(python3 ./scratch/ttfm.py "$tr" "$bio" "$bex" | head -1)" >"$out"
  LG "TM $(cat "$out")"; stop "$pid" "$PORT_SIG"
}

exec
/bin/bash -lc "sed -n '1,38p' DESIGN.md && sed -n '1,23p' docs/flipctl-design.md && sed -n '136,165p' docs/flipctl-design.md && sed -n '1,18p' docs/specs/AUDIT-TLS.md && sed -n '1,50p' DESIGN-GATEHYGIENE.md && sed -n '1,30p' tests/gate.sh && sed -n '18,27p' scratchpad/rlbatch/lib.sh && sed -n '1,35p' scratch/report.py && sed -n '1,35p' scratch/report2.py && sed -n '1,42p' scratch/flipfp/report.sh" in /home/user/Projects/cx-merge
 succeeded in 0ms:
# Thread amortization study surface

`thread-mode` and `overlap` are one boot-latched measurement surface. The documented mode
names are `2s` (separate IO and executor threads, the default) and `1s` (one generalized thread owns
both roles). `split` and `fused` remain parser aliases so old invocations keep working.

| mode | overlap 0 | overlap 1 | overlap 2 |
| --- | --- | --- | --- |
| `2s` | ordinary split IO/ex loops | exact `t-iopipe` WB/IFID batch schedule | rejected at config validation |
| `1s` | coarse IFID → EX → WB rotation | exact `t-genthread` `iofused` schedule | gated `iofused` three-way schedule |

The compatibility options `thread-pipeline` and `genthread-schedule` remain accepted: the former
is a numeric alias for `overlap`, while the latter maps `coarse`, `iofused`, and the legacy
`streams` name to `1s` plus overlap 0, 1, and 2 respectively. The legacy name is only a parser
alias; overlap 2 no longer runs the streams implementation. Overlap 2 emits a boot warning because
it is an experimental research schedule. Unified overlap 1 and 2 require `net-io uring`; the split
iopipe schedule retains its explicit epoll specialization. No schedule constant is runtime-tunable.

Overlap 1 is deliberately the measured implementation, not a family resemblance. In `2s`, its
shallow rotation is WB observe, IFID receive, WB prefetch, IFID parse/hash, WB retire/prepare, IFID
post, and WB submit/reclaim; its existing depth gate selects the measured natural-order path. In
`1s`, `iofused` retains the fork's targeted IFID work, WB dependency prefetch, coarse executor turn,
and SEND-sensitive submission boundary. Overlap 2 builds on those same mechanics. Its deep path
freezes and prefetches a ready-list WB batch, performs the targeted IFID batch, gathers/schedules and
prefetches one whole EX batch, uses WB prepare/pump as the EX load-gap filler, then executes that EX
batch and drains any remaining EX batches normally. Kernel submission remains at the single outer
boundary. A one-bit depth gate returns the next rotation to the unchanged coarse IFID → EX → WB
order after a thin pass. These shapes are fixed so results can be compared against overlap 0's
plain-loop baseline.

The `1s` overlap-1 implementation remains unchanged. Both unified overlap schedules use its pending
IFID/WB ready lists and the fused inbox's lifetime-fixed geometry directly (1,024 slots per
producer), including blocking, transaction, scatter/script follow-up, and stale-owner paths. The
overlap-2 deep path adds only one gate bit and a stack-local whole EX batch to overlap 1's WB batch;
it has no IFID reservations, residual ages, A/D executor contexts, delayed lane retirement, or
unpublished operations. The old streams source is retained for branch comparison but is unreachable
from overlap-2 dispatch. Reservation-capable queue operations remain because overlap-0 read-local
demotion uses them; neither overlap 1 nor overlap 2 enters them.
# t-flipctl design and code map

This lane adds one automatic owner around the existing runtime `FLIP`; it does not add another
placement transaction. `Server::flip_begin` and the unchanged `FlipStage` acknowledgements remain
the only way thread roles, clients, listeners, and shards move. The automatic controller ships
dark (`flip-auto 0`).

## Ownership and footprint

- `src/main.cc` is the controller cron owner. When `flip-auto=1`, the main/monitor thread calls
  `Server::flipctl_tick()` once per existing LB tick (`lb-tick-ms`; if that mechanism is disabled,
  the period derives from the provisioned thread count). No IO or EX loop runs controller policy.
- `src/core/flipctl.{h,cc}` contains the fingerprint representation, shift detector, and controller
  state machine. Controller vectors allocate only when `flip-auto=1`.
- Each `ThreadCtx` embeds one `FlipFingerprintWriter`. It has no dynamic storage. Only the physical
  thread playing IO mutates it. There are no fingerprint atomics and no field was added to `Op`,
  `Client`, or `Task`; the existing `sizeof(Op)==336` and `sizeof(Client)==1984` assertions remain
  build gates.
- Cross-owner observation follows the existing exceptional INFO-counter rule: the monitor reads a
  completed monotonic cumulative window and subtracts its last snapshot. A writer stores
  `closed_windows` last, so an unfinished partial window is never consumed. Closing is once per
  `flip-work-window` commands, not per command.


A rate increase can also move the load-sensitive parse-depth fingerprint. If both detectors cross
together, one pending out-of-band rate observation is allowed its required second sample before the
fingerprint can preempt it; a pure mix shift with no rate evidence still fires immediately.

A trigger resets the maneuver's readings and capacity baseline. Trigger source counters make boot,
fingerprint, rate surge, rate collapse, and forced causes independently observable.

## Integration and observability

- `src/core/config.h` is the sole parser/default source for `flip-auto`, `flip-auto-band`, and
  `flip-work-window`; `src/cmd/t_server.cc` exposes them as immutable CONFIG values.
- `tomokv.conf` documents the numeric grammar and off/auto semantics.
- `INFO FLIPCTL` reports state/phase, anchor split/rate, bands, total/per-reason trigger counts, and
  the last trigger reason.
- Boot-gated `DEBUG FLIPCTL` dumps controller phase, bands, direction, step, pending split, and all
  stabilized visited split/rate pairs. `DEBUG FLIPCTL TRIGGER` requests a forced maneuver when the
  controller is enabled.
- The only FLIP accommodation is that `flip_build_client_plan(nullptr, ...)` treats an automatic
  transaction as having no pinned command client, and `finish_flip_command()` has no ROB reply to
  complete for that form. Manual `FLIP io ex` keeps its pinned coordinator and asynchronous reply.
  All transaction stages, deadlines, rollback, wakeups, conservation checks, and transfer plans are
  otherwise shared unchanged.

## Functional validation

- `tests/flipctl_unit.cc` scripts quiet count noise followed by a simultaneous pipe/class/key/value
  mix change, and checks the command-work window/off behavior.
- `tests/flipctl.py` is the small port-7845 Python driver. It asserts that a connection ramp cannot
  start boot, varies think-time with a stationary six-second pattern while retaining the same
# AUDIT-TLS — TLS integration for TomoKV-cpp (perthread / pure 2s)

> **Where it landed (2026-09-04).** The feature shipped as `src/net/tls.{h,cc}` (this document
> proposes `src/net/tls_ctx.h` + `src/net/tls_conn.h`); the test is `tests/tls.py` (no
> `tests/gen-certs.sh`); no `tls-bio-size` knob exists. Defaults: AES-128-GCM-first cipher lists
> and `tls-ktls yes` (`tomokv.conf`, NETWORK).

Governing audit. Read before writing any TLS code in `/home/user/Projects/tomokv-cpp-perthread`.
No repository was modified in producing this.

**Sources read (all first-hand):**

| Tree | Path | Verified |
|---|---|---|
| tomokv | `/home/user/Projects/tomokv-cpp-perthread/src/{net,core,cmd,exec}` | local, read |
| redis | `/home/user/Projects/redis/src/{tls.c,connection.c,connection.h,connhelpers.h,config.c,iothread.c}` (8.9.241) | local, read |
| valkey | `/home/user/Projects/valkey/src/{tls.c,config.c,io_threads.c,networking.c}` (unstable, `8cd535ddb`, tls.c = 2100 lines) | local, read + spot-verified |
| dragonfly | `/home/user/Projects/dragonfly/src/facade/{tls_helpers.cc,dragonfly_connection.cc,dragonfly_listener.cc}` | local, read + spot-verified |
# Gate hygiene: the rows that assert a timing or statistical property

Branch `t-gatehygiene`, from `ceb6b02f8` (train 13).

Three full-gate rows reddened intermittently on a correct build. None of them had a wrong
mechanism; each asserted a **timing or statistical** property alongside (or instead of) the
mechanism it exists to protect, and so measured the box rather than the server. The finals depend
on a gate that is trustworthy, which means a red row has to mean a defect.

The rule this lane applies:

> A row may *use* timing to deliver a mechanism. It may not *assert* timing unless the assertion
> is still true on an arbitrarily slow machine, or the row states and enforces the precondition
> that makes it true and refuses to be judged when that precondition is unmet.

---

## 1. `tests/atomic_ryow.py` — "cross-key atomics on one connection overlap"

### The defect

One `note()` bundled two different kinds of claim:

```python
note("cross-key atomics on one connection overlap",
     overlap_ok and stall_after > stall_before and pipe_rate > serial_rate * 1.10, ...)
```

* `overlap_ok and stall_after > stall_before` is a **mechanism** claim. 24 cross-key atomic groups
  pipelined on one connection are all admitted and answered `OK`, and `atomic_window_stalls`
  *moves* — which is only possible if a younger group was in flight while an older one was still
  deciding. The regression this section exists for (a return of the full connection barrier) makes
  `stall_after == stall_before` on any build at any speed.
* `pipe_rate > serial_rate * 1.10` is a **performance** claim. It is false on a correct build
  whenever the two arms are not slowed by the same factor.

The full gate runs this file a second time under ASAN. The sanitizer does not slow the pipelined
arm and the serial arm equally, and the ratio inverts: the gate measured **pipe 21,005/s against
serial 24,202/s** and reddened the row while every correctness check in that same run passed. The
same build passed 4 of 4 standalone.

### The fix

The two claims are two `note()`s. The mechanism half runs on **every** tier. The rate half runs
only when the caller says the server is a release build — `tests/gate.sh` passes
`--no-rate-assertions` on its ASAN row (`tests/gate.sh:1246`).

An **explicit flag**, not a sniffed `GATE_TIER` environment variable: a battery run by hand then
does what it was told rather than what it guessed about its server. When the flag is present the
row prints `SKIP` with the reason **and the measured rates**, so the number stays in the log on
#!/bin/bash
# PRE-PUSH GATE for tomokv-cpp (pure 2s baseline).
#
#   tests/gate.sh quick   loopback only: build (release+ASAN), footprint locks, boot
#                         matrix, smoke, torture, RYOW, atomic torn/mixed-write/window gates,
#                         shutdown invariants, counter-fired assertions, idle-loop ceiling. Runs on
#                         any machine.
#   tests/gate.sh full    quick + torture-under-ASAN + the Redis 7.4 differential matrix + NIC
#                         regression cells vs tests/gate_refs.txt (the NIC cells need the 25GbE
#                         netns rig and its scratchpad binaries/procsafe helper).
#
#   Every feature battery runs on three boots: split (both atomic modes), fused, and fused with the
#   read-local lane ARMED (--read-local 1, the recommended read-heavy production posture; that leg
#   is the only one on which a clean GET/MGET is served by the lane instead of an owner).
#
#   GATE_QUIET_FILE=<path> (opt-in, for a shared box): nothing that starts CPU work -- a battery, a
#   boot, a build, a load generator -- is launched unless that file exists and is older than
#   GATE_QUIET_MINUTES (3). A running battery always finishes; a build is SIGSTOPped; the idle server
#   is SIGSTOPped while waiting; waited time is taken out of the ledger's per-row seconds and logged
#   to $LEDGER.pauses. GATE_LEDGER overrides the ledger path.
#
# The vacuous-validation rule is load-bearing here: every section proves its mechanism FIRED
# (counters, accepts, direct>0), not merely that nothing crashed. A gate that can pass while
# testing nothing is worse than no gate.
set -u
cd "$(dirname "$0")/.."
TIER=${1:-quick}
PORT=${GATE_PORT:-7899}
CORES=${GATE_CORES:-0-7}
NCORES=$(taskset -c "$CORES" nproc)
  local bin="$1" log="$2"; shift 2
  guard_port "$PORT" || return 1
  # EXTRA_ARGS lets a caller add boot flags (e.g. --read-local-interleave 0) without a second copy
  # of this function; word splitting is intentional.
  # shellcheck disable=SC2086
  taskset -c "$SRVCORE" "$bin" --port "$PORT" --bind 127.0.0.1 --shards 16 \
      --thread-mode fused --read-local "${RL:-1}" ${EXTRA_ARGS:-} "$@" > "$log" 2>&1 &
  SRV=$!
  for _ in $(seq 150); do
    kill -0 "$SRV" 2>/dev/null || { wait "$SRV" 2>/dev/null; echo "BOOT DIED, see $log" >&2; return 1; }
#!/usr/bin/env python3
"""report.py -- build the lane report (HTML artifact) from the measurement files.
Usage: report.py OUT.html  (reads $SP/fd-matrix2.csv, fd-matrix-mk15.csv, fd-final.log, flip probes)"""
import csv, html, os, re, statistics as st, subprocess, sys
from collections import defaultdict

SP = "/tmp/claude-1000/-home-user-Projects/ee6eb242-5302-49cf-b767-1a2d8d8f0f61/scratchpad"
WT = "/home/user/Projects/wt-flipdamp"
OUT = sys.argv[1]
COMMIT = subprocess.run(["git", "-C", WT, "rev-parse", "--short=9", "HEAD"], capture_output=True, text=True).stdout.strip()
BASE = "e902c67d5"

def f(x):
    try: return float(x)
    except Exception: return 0.0
def i(x):
    try: return int(float(x))
    except Exception: return 0

def load(path):
    if not os.path.exists(path): return []
    return [r for r in csv.DictReader(open(path)) if r.get("rate") not in (None, "", "MISSING")]

def timeline_flips(arm, wl, rnd):
    """(flips before first anchor, flips after first anchor, first-anchor second) from the ab.sh timeline."""
    p = f"{SP}/fd-tl-{arm}-{wl}-{rnd}.txt"
    if not os.path.exists(p): return (None, None, None)
    first = None; at_anchor = None; last = 0
    for line in open(p):
        fs = line.split()
        if not fs: continue
        t = int(fs[0]); d = dict(kv.split(":", 1) for kv in fs[1:] if ":" in kv and not kv.startswith("foreign"))
        fc = i(d.get("flip_completed", 0)); last = fc
        if first is None and d.get("flipctl_state") == "anchored": first, at_anchor = t, fc
    if at_anchor is None: return (last, 0, None)
#!/usr/bin/env python3
"""report2.py OUT.html -- the redesign lane's report (HTML artifact) from the measurement files.
Reads $SP/fd-matrix4.csv (+ fd-tl-*/fd-dbg-* per cell), fd-costgate-*.txt, fd-hold4-*.txt,
fd-fire4-*.txt, fd-perf4-*.txt, fd-ctl4-*.txt and the chain log fd-red.log."""
import csv, html, os, re, statistics as st, subprocess, sys
from collections import defaultdict

SP = "/tmp/claude-1000/-home-user-Projects/ee6eb242-5302-49cf-b767-1a2d8d8f0f61/scratchpad"
WT = "/home/user/Projects/wt-flipdamp"
OUT = sys.argv[1]
COMMIT = subprocess.run(["git", "-C", WT, "rev-parse", "--short=9", "HEAD"], capture_output=True, text=True).stdout.strip()
BASE = "e902c67d5"; GUARD = "d84031d2f"

def f(x):
    try: return float(x)
    except Exception: return 0.0
def i(x):
    try: return int(float(x))
    except Exception: return 0
def load(path):
    if not os.path.exists(path): return []
    return [r for r in csv.DictReader(open(path)) if r.get("rate") not in (None, "", "MISSING")]

def timeline(arm, wl, rnd):
    """(flips before first anchor, flips after first anchor, first-anchor second) from the matrix timeline."""
    p = f"{SP}/fd-tl-{arm}-{wl}-{rnd}.txt"
    if not os.path.exists(p): return (None, None, None)
    first = None; at_anchor = None; last = 0
    for line in open(p):
        fs = line.split()
        if not fs: continue
        t = int(fs[0]); d = dict(kv.split(":", 1) for kv in fs[1:] if ":" in kv and not kv.startswith("foreign"))
        fc = i(d.get("flip_completed", 0)); last = fc
        if first is None and d.get("flipctl_state") == "anchored": first, at_anchor = t, fc
    if at_anchor is None: return (last, 0, None)
#!/bin/bash
# report.sh -- assemble the lane's PRE/POST tables and verdict rows from the chain outputs.
source /tmp/claude-1000/-home-user-Projects/ee6eb242-5302-49cf-b767-1a2d8d8f0f61/scratchpad/flipfp/fl.sh
cd "$FP" || exit 1
ROWS='cycles/op|instr/op|IPC|USER instr/op (:u)|USER store-queue-full|store-queue-full cyc|load-queue-full cyc|retire-queue-full cyc|USER dmnd fills same-CCX|dmnd fills L2|driver ops/s'
pick(){ # pick TABLE.md SHAPE -> the rows of interest for that shape
  awk -v shape="$2" -v rows="$ROWS" 'BEGIN{n=split(rows,r,"|")} /^#### /{on = index($0, shape " ")==6} on && /^\| / {name=$0; sub(/^\| /,"",name); sub(/ \|.*/,"",name); for(i=1;i<=n;i++) if (name==r[i]) print}' "$1"
}
echo "# t-flipfp results  ($(date +%F' '%T))"
echo; echo "digests: $(tr '\n' ';' < digests.txt)"
for geom in 1t 2t 2s; do
  for kind in "" null-; do
    t=table-$kind$geom.md; [ -s "$t" ] || continue
    for shape in set_over get_hit; do
      echo; echo "### ${kind:-PRE-vs-POST }$geom $shape (flip-auto 0)"; echo
      echo "| metric | PRE | POST | delta | delta % |"; echo "|---|---:|---:|---:|---:|"
      pick "$t" "$shape"
    done
  done
done
if [ -s table-on-2s.md ]; then
  for shape in set_over get_hit; do
    echo; echo "### flip-auto 1 (anchored, band 0) 2s $shape: PRE vs POST"; echo
    echo "| metric | PRE | POST | delta | delta % |"; echo "|---|---:|---:|---:|---:|"
    pick table-on-2s.md "$shape"
  done
  echo; echo "controller state during the flip-auto 1 slope cells:"; grep -h "settle:\|end:" slope-pre1-2s.log slope-post1-2s.log 2>/dev/null | sed 's/^/    /'
fi
echo; echo "### accuracy (2:2, auto band, stationary load: triggers after boot = false triggers)"; echo
for f in acc-pre-mk.txt acc-post-mk.txt acc-pre-hetero.txt acc-post-hetero.txt; do [ -s "$f" ] && echo "- $(head -1 "$f")"; done
echo; echo "### wrong-split boots (MK8 1:1, atomic 1, 120 s)"; echo
for f in tm-pre-31.txt tm-post-31.txt tm-off-31.txt tm-pre6-51.txt tm-post6-51.txt; do [ -s "$f" ] && { echo "- $(head -1 "$f")"; echo "  $(sed -n 2p "$f")"; }; done
echo; echo "### gate row tests/flipctl.py (6:2, band 2, age 1024, --stable-seconds 30)"; echo
pass=0; runs=0
for f in ctl-post-1.txt ctl-post-2.txt ctl-post-3.txt ctl-pre-1.txt ctl-pre-2.txt ctl-pre-3.txt; do
  [ -s "$f" ] || continue
  rc=$(sed -n 's/^RC=//p' "$f" | head -1)
  case $f in ctl-post-*) runs=$((runs+1)); [ "$rc" = 0 ] && pass=$((pass+1));; esac
  echo "- $f: RC=$rc :: $(grep -E '^ok:|AssertionError|anchored off-rail|Error' "$f" | head -1 | cut -c1-190)"
done
echo; echo "POST tests/flipctl.py pass count: $pass / $runs"
echo; echo "### batteries / differ / gate"; echo

apply patch
patch: completed
/home/user/Projects/cx-merge/DESIGN.md
/home/user/Projects/cx-merge/docs/flipctl-design.md
/home/user/Projects/cx-merge/docs/specs/AUDIT-TLS.md
/home/user/Projects/cx-merge/scratchpad/rlbatch/lib.sh
/home/user/Projects/cx-merge/tests/flipctl_unit.cc
/home/user/Projects/cx-merge/tests/rlcache_churn.py
diff --git a/DESIGN.md b/DESIGN.md
index 0fba740d94ba1915868410de40bcb685ae675833..7ad3ac9fef512d8549eadfce44036b8a6a181e9f
--- a/DESIGN.md
+++ b/DESIGN.md
@@ -1,6 +1,6 @@
 # Thread amortization study surface
 
-`thread-mode` and `overlap` are one boot-latched measurement surface. The documented mode
+`thread-mode` and the study-only `x-overlap` are one boot-latched measurement surface. The documented mode
 names are `2s` (separate IO and executor threads, the default) and `1s` (one generalized thread owns
 both roles). `split` and `fused` remain parser aliases so old invocations keep working.
 
@@ -9,10 +9,10 @@
 | `2s` | ordinary split IO/ex loops | exact `t-iopipe` WB/IFID batch schedule | rejected at config validation |
 | `1s` | coarse IFID → EX → WB rotation | exact `t-genthread` `iofused` schedule | gated `iofused` three-way schedule |
 
-The compatibility options `thread-pipeline` and `genthread-schedule` remain accepted: the former
-is a numeric alias for `overlap`, while the latter maps `coarse`, `iofused`, and the legacy
-`streams` name to `1s` plus overlap 0, 1, and 2 respectively. The legacy name is only a parser
-alias; overlap 2 no longer runs the streams implementation. Overlap 2 emits a boot warning because
+Use `--x-overlap 0|1|2` for these study schedules and `--x-ex-sched 0|1` for the executor study.
+The old public schedule spellings and compatibility aliases are removed; see
+[DESIGN-KNOBS.md](DESIGN-KNOBS.md). Below, "overlap" names the internal schedule value.
+Overlap 2 emits a boot warning because
 it is an experimental research schedule. Unified overlap 1 and 2 require `net-io uring`; the split
 iopipe schedule retains its explicit epoll specialization. No schedule constant is runtime-tunable.
 
diff --git a/docs/flipctl-design.md b/docs/flipctl-design.md
index e6a5e31a6c05171b172746f2586a01d09fa1e84e..cd96557199b408c8b85bb42b77ca59ee6fec73ea
--- a/docs/flipctl-design.md
+++ b/docs/flipctl-design.md
@@ -8,8 +8,7 @@
 ## Ownership and footprint
 
 - `src/main.cc` is the controller cron owner. When `flip-auto=1`, the main/monitor thread calls
-  `Server::flipctl_tick()` once per existing LB tick (`lb-tick-ms`; if that mechanism is disabled,
-  the period derives from the provisioned thread count). No IO or EX loop runs controller policy.
+  `Server::flipctl_tick()` on its internal observation cadence. No IO or EX loop runs controller policy.
 - `src/core/flipctl.{h,cc}` contains the fingerprint representation, shift detector, and controller
   state machine. Controller vectors allocate only when `flip-auto=1`.
 - Each `ThreadCtx` embeds one `FlipFingerprintWriter`. It has no dynamic storage. Only the physical
@@ -46,9 +45,9 @@
 measurement quantum, avoiding the zero-anchor discontinuity where one rare admin argument would
 otherwise have distance one.
 
-With `flip-auto-band=-1`, the band is twice the maximum quiet adjacent/drift distance learned at
-the final split, floored by the four-family sampling quantum divided by observed commands. A
-numeric value is a percent. Zero disables fingerprint and collapse re-triggers. Final anchor
+The automatic band is twice the maximum quiet adjacent/drift distance learned at
+the final split, floored by the four-family sampling quantum divided by observed commands.
+It has no public override. Final anchor
 learning consumes a number of closed aggregates derived from the provisioned thread count. It is
 done after maneuver-only age sampling is disarmed, so the controller cannot detect its own signal
 cost transition as a workload change.
@@ -64,13 +63,13 @@
 falls below a threshold derived from its own change jitter and measurement quantum. Directional
 connection ramp-up therefore remains drift, while jittery stationary traffic can qualify even when
 its absolute rate never enters a quiet band. A 30-second ceiling, converted to ticks from
-`lb-tick-ms`, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
+the internal observation cadence, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
 thread per tick resets the learning/cap state, so an idle server remains in
 `awaiting-load-stability` and never pays for a maneuver it cannot use.
 
-`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. An explicit
-`lb-age-sample-rate` is used when nonzero; otherwise the rate derives from the provisioned thread
-count. IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
+`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. Its rate
+derives from observed command volume and the three-tick decision window, and adapts on completed
+traffic windows; see [DESIGN-KNOBS.md](../DESIGN-KNOBS.md). IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
 writes. IO releases its ROB-head age map when the rate returns to zero.
 
 After a stabilized baseline rate, `issue_initial_jump()` subtracts the per-thread maneuver window.
@@ -88,8 +87,8 @@
 not vetoed by idle peers. If either role has no measured work/busy quantity, measurement continues;
 the controller does not invent a workload prior.
 
-The calculation is performed in placement units (one logical CPU normally, one sibling pair under
-`smt-mode=1`) and then reported/committed in threads. The target passes `nio_eq` in the measured
+The calculation is performed in placement units (one logical CPU normally, one sibling pair when
+the allowed topology contains complete pairs) and then reported/committed in threads. The target passes `nio_eq` in the measured
 direction by `max(1, abs(nio_eq-nio_now)/2)`, clamped to one live unit of each role. Even when the
 equalized split rounds to the current split, the direction of greater measured demand supplies the
 deliberate one-unit overshoot.
@@ -143,7 +142,7 @@
 
 ## Integration and observability
 
-- `src/core/config.h` is the sole parser/default source for `flip-auto`, `flip-auto-band`, and
+- `src/core/config.h` is the sole parser/default source for `flip-auto` and
   `flip-work-window`; `src/cmd/t_server.cc` exposes them as immutable CONFIG values.
 - `tomokv.conf` documents the numeric grammar and off/auto semantics.
 - `INFO FLIPCTL` reports state/phase, anchor split/rate, bands, total/per-reason trigger counts, and
diff --git a/docs/specs/AUDIT-TLS.md b/docs/specs/AUDIT-TLS.md
index 0256e887f5d27c210d436bc219f3c7521ddad0db..87a2eaca3137a47f7c0e1567794650f57531860b
--- a/docs/specs/AUDIT-TLS.md
+++ b/docs/specs/AUDIT-TLS.md
@@ -3,7 +3,7 @@
 > **Where it landed (2026-09-04).** The feature shipped as `src/net/tls.{h,cc}` (this document
 > proposes `src/net/tls_ctx.h` + `src/net/tls_conn.h`); the test is `tests/tls.py` (no
 > `tests/gen-certs.sh`); no `tls-bio-size` knob exists. Defaults: AES-128-GCM-first cipher lists
-> and `tls-ktls yes` (`tomokv.conf`, NETWORK).
+> and automatic kTLS negotiation with userspace fallback (`tomokv.conf`, NETWORK).
 
 Governing audit. Read before writing any TLS code in `/home/user/Projects/tomokv-cpp-perthread`.
 No repository was modified in producing this.
diff --git a/scratchpad/rlbatch/lib.sh b/scratchpad/rlbatch/lib.sh
index 7bcf3e4e6030c79174bf35d9bb1b3dfb11f80c42..12184314d2aafac0dc7aa48db761a3bdec5de8c7
--- a/scratchpad/rlbatch/lib.sh
+++ b/scratchpad/rlbatch/lib.sh
@@ -17,7 +17,7 @@
 boot_srv(){ # boot_srv <binary> <logfile> [extra args...] -> SRV pid
   local bin="$1" log="$2"; shift 2
   guard_port "$PORT" || return 1
-  # EXTRA_ARGS lets a caller add boot flags (e.g. --read-local-interleave 0) without a second copy
+  # EXTRA_ARGS lets a caller add boot flags (e.g. --lb 0) without a second copy
   # of this function; word splitting is intentional.
   # shellcheck disable=SC2086
   taskset -c "$SRVCORE" "$bin" --port "$PORT" --bind 127.0.0.1 --shards 16 \
diff --git a/tests/flipctl_unit.cc b/tests/flipctl_unit.cc
index 540a6c40e9589572431075b623bc553c76ee8d85..33209258e2958771d60f86b919f6f510e9977737
--- a/tests/flipctl_unit.cc
+++ b/tests/flipctl_unit.cc
@@ -184,11 +184,11 @@
 
     // THE TYPED BAND IS A FLOOR, NOT A CEILING. The gate-hygiene lane's instrumented failure: a
     // driver holding its rate to 0.07% across 34 samples, a fingerprint distance of 0.2518 against
-    // a flat --flip-auto-band 2 (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
+    // a flat explicit band of 2% (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
     // A detector whose signature genuinely moves between adjacent quiet windows must end up with a
     // band above that movement whatever the operator typed, and must still fire on a real change.
     {
-        FlipShiftDetector typed(2, 4);   // --flip-auto-band 2
+        FlipShiftDetector typed(2, 4);   // internal explicit-band control: 2%
         // Stationary but NOISY: the read/write split alternates 35:65 / 65:35 window to window.
         // The detector smooths the signature, so the anchored distance this produces is ~0.09.
         const auto noisy = [](int k) { return quiet(k % 2 ? 65 : 35, k % 2 ? 35 : 65); };
@@ -250,8 +250,8 @@
         if (fires) fail("the learned band fired on quiet-state movement");
     }
     // A TYPED BAND UNDER THE REAL SAMPLER. The shipped configuration cannot reach this -- the
-    // controller runs the learned band -- but an operator who sets --flip-auto-band AND the sampler
-    // can, and on a tree where the typed path bypasses the floor the fingerprint lane measured 73
+    // controller runs the learned band. This internal explicit-band control preserves the earlier
+    // regression: where the typed path bypassed the floor the fingerprint lane measured 73
     // two-consecutive exceedances in 600 stationary windows at K=100 and 208 at K=60: a spurious
     // maneuver every few seconds on a load that never changed. Driven here through THEIR writer, a
     // stationary 50/50 stream at depth 32, with the floor in place: none, and a real mix change
@@ -259,7 +259,7 @@
     {
         for (uint32_t window : {60u, 100u}) {
             XorShift rng;
-            FlipShiftDetector typed(2, 8);   // --flip-auto-band 2, the operator's knob
+            FlipShiftDetector typed(2, 8);   // internal explicit-band control: 2%
             int fires = 0, confirmed = 0, streak = 0;
             for (int w = 0; w < 60; w++) {
                 FlipFingerprintWriter writer;
diff --git a/tests/rlcache_churn.py b/tests/rlcache_churn.py
index b56ae4041ae0843dc67443f16017270063853124..f8a2c0730bb0a4fb6c3b5f6282c547226bb76599
--- a/tests/rlcache_churn.py
+++ b/tests/rlcache_churn.py
@@ -59,7 +59,7 @@
 
 
 # THE LOAD BALANCER MUST HAVE SOMETHING TO BALANCE. A uniform key distribution over one small key
-# space keeps every shard at the same op rate, the imbalance never crosses lb-imbalance-pct, and no
+# space keeps every shard at the same op rate, the imbalance never crosses the learned band, and no
 # shard ever changes owner -- which is the precondition for the defect this battery exists for
 # (measured: 7.8M ops, 0 migrations). A hot window that ROTATES makes a few shards hot, then makes
 # different ones hot, so the balancer moves shards continuously and in both directions, which is

diff --git a/DESIGN.md b/DESIGN.md
index 0fba740d94ba1915868410de40bcb685ae675833..7ad3ac9fef512d8549eadfce44036b8a6a181e9f
--- a/DESIGN.md
+++ b/DESIGN.md
@@ -1,6 +1,6 @@
 # Thread amortization study surface
 
-`thread-mode` and `overlap` are one boot-latched measurement surface. The documented mode
+`thread-mode` and the study-only `x-overlap` are one boot-latched measurement surface. The documented mode
 names are `2s` (separate IO and executor threads, the default) and `1s` (one generalized thread owns
 both roles). `split` and `fused` remain parser aliases so old invocations keep working.
 
@@ -9,10 +9,10 @@
 | `2s` | ordinary split IO/ex loops | exact `t-iopipe` WB/IFID batch schedule | rejected at config validation |
 | `1s` | coarse IFID → EX → WB rotation | exact `t-genthread` `iofused` schedule | gated `iofused` three-way schedule |
 
-The compatibility options `thread-pipeline` and `genthread-schedule` remain accepted: the former
-is a numeric alias for `overlap`, while the latter maps `coarse`, `iofused`, and the legacy
-`streams` name to `1s` plus overlap 0, 1, and 2 respectively. The legacy name is only a parser
-alias; overlap 2 no longer runs the streams implementation. Overlap 2 emits a boot warning because
+Use `--x-overlap 0|1|2` for these study schedules and `--x-ex-sched 0|1` for the executor study.
+The old public schedule spellings and compatibility aliases are removed; see
+[DESIGN-KNOBS.md](DESIGN-KNOBS.md). Below, "overlap" names the internal schedule value.
+Overlap 2 emits a boot warning because
 it is an experimental research schedule. Unified overlap 1 and 2 require `net-io uring`; the split
 iopipe schedule retains its explicit epoll specialization. No schedule constant is runtime-tunable.
 
diff --git a/docs/flipctl-design.md b/docs/flipctl-design.md
index e6a5e31a6c05171b172746f2586a01d09fa1e84e..cd96557199b408c8b85bb42b77ca59ee6fec73ea
--- a/docs/flipctl-design.md
+++ b/docs/flipctl-design.md
@@ -8,8 +8,7 @@
 ## Ownership and footprint
 
 - `src/main.cc` is the controller cron owner. When `flip-auto=1`, the main/monitor thread calls
-  `Server::flipctl_tick()` once per existing LB tick (`lb-tick-ms`; if that mechanism is disabled,
-  the period derives from the provisioned thread count). No IO or EX loop runs controller policy.
+  `Server::flipctl_tick()` on its internal observation cadence. No IO or EX loop runs controller policy.
 - `src/core/flipctl.{h,cc}` contains the fingerprint representation, shift detector, and controller
   state machine. Controller vectors allocate only when `flip-auto=1`.
 - Each `ThreadCtx` embeds one `FlipFingerprintWriter`. It has no dynamic storage. Only the physical
@@ -46,9 +45,9 @@
 measurement quantum, avoiding the zero-anchor discontinuity where one rare admin argument would
 otherwise have distance one.
 
-With `flip-auto-band=-1`, the band is twice the maximum quiet adjacent/drift distance learned at
-the final split, floored by the four-family sampling quantum divided by observed commands. A
-numeric value is a percent. Zero disables fingerprint and collapse re-triggers. Final anchor
+The automatic band is twice the maximum quiet adjacent/drift distance learned at
+the final split, floored by the four-family sampling quantum divided by observed commands.
+It has no public override. Final anchor
 learning consumes a number of closed aggregates derived from the provisioned thread count. It is
 done after maneuver-only age sampling is disarmed, so the controller cannot detect its own signal
 cost transition as a workload change.
@@ -64,13 +63,13 @@
 falls below a threshold derived from its own change jitter and measurement quantum. Directional
 connection ramp-up therefore remains drift, while jittery stationary traffic can qualify even when
 its absolute rate never enters a quiet band. A 30-second ceiling, converted to ticks from
-`lb-tick-ms`, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
+the internal observation cadence, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
 thread per tick resets the learning/cap state, so an idle server remains in
 `awaiting-load-stability` and never pays for a maneuver it cannot use.
 
-`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. An explicit
-`lb-age-sample-rate` is used when nonzero; otherwise the rate derives from the provisioned thread
-count. IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
+`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. Its rate
+derives from observed command volume and the three-tick decision window, and adapts on completed
+traffic windows; see [DESIGN-KNOBS.md](../DESIGN-KNOBS.md). IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
 writes. IO releases its ROB-head age map when the rate returns to zero.
 
 After a stabilized baseline rate, `issue_initial_jump()` subtracts the per-thread maneuver window.
@@ -88,8 +87,8 @@
 not vetoed by idle peers. If either role has no measured work/busy quantity, measurement continues;
 the controller does not invent a workload prior.
 
-The calculation is performed in placement units (one logical CPU normally, one sibling pair under
-`smt-mode=1`) and then reported/committed in threads. The target passes `nio_eq` in the measured
+The calculation is performed in placement units (one logical CPU normally, one sibling pair when
+the allowed topology contains complete pairs) and then reported/committed in threads. The target passes `nio_eq` in the measured
 direction by `max(1, abs(nio_eq-nio_now)/2)`, clamped to one live unit of each role. Even when the
 equalized split rounds to the current split, the direction of greater measured demand supplies the
 deliberate one-unit overshoot.
@@ -143,7 +142,7 @@
 
 ## Integration and observability
 
-- `src/core/config.h` is the sole parser/default source for `flip-auto`, `flip-auto-band`, and
+- `src/core/config.h` is the sole parser/default source for `flip-auto` and
   `flip-work-window`; `src/cmd/t_server.cc` exposes them as immutable CONFIG values.
 - `tomokv.conf` documents the numeric grammar and off/auto semantics.
 - `INFO FLIPCTL` reports state/phase, anchor split/rate, bands, total/per-reason trigger counts, and
diff --git a/docs/specs/AUDIT-TLS.md b/docs/specs/AUDIT-TLS.md
index 0256e887f5d27c210d436bc219f3c7521ddad0db..87a2eaca3137a47f7c0e1567794650f57531860b
--- a/docs/specs/AUDIT-TLS.md
+++ b/docs/specs/AUDIT-TLS.md
@@ -3,7 +3,7 @@
 > **Where it landed (2026-09-04).** The feature shipped as `src/net/tls.{h,cc}` (this document
 > proposes `src/net/tls_ctx.h` + `src/net/tls_conn.h`); the test is `tests/tls.py` (no
 > `tests/gen-certs.sh`); no `tls-bio-size` knob exists. Defaults: AES-128-GCM-first cipher lists
-> and `tls-ktls yes` (`tomokv.conf`, NETWORK).
+> and automatic kTLS negotiation with userspace fallback (`tomokv.conf`, NETWORK).
 
 Governing audit. Read before writing any TLS code in `/home/user/Projects/tomokv-cpp-perthread`.
 No repository was modified in producing this.
diff --git a/scratchpad/rlbatch/lib.sh b/scratchpad/rlbatch/lib.sh
index 7bcf3e4e6030c79174bf35d9bb1b3dfb11f80c42..12184314d2aafac0dc7aa48db761a3bdec5de8c7
--- a/scratchpad/rlbatch/lib.sh
+++ b/scratchpad/rlbatch/lib.sh
@@ -17,7 +17,7 @@
 boot_srv(){ # boot_srv <binary> <logfile> [extra args...] -> SRV pid
   local bin="$1" log="$2"; shift 2
   guard_port "$PORT" || return 1
-  # EXTRA_ARGS lets a caller add boot flags (e.g. --read-local-interleave 0) without a second copy
+  # EXTRA_ARGS lets a caller add boot flags (e.g. --lb 0) without a second copy
   # of this function; word splitting is intentional.
   # shellcheck disable=SC2086
   taskset -c "$SRVCORE" "$bin" --port "$PORT" --bind 127.0.0.1 --shards 16 \
diff --git a/tests/flipctl_unit.cc b/tests/flipctl_unit.cc
index 540a6c40e9589572431075b623bc553c76ee8d85..33209258e2958771d60f86b919f6f510e9977737
--- a/tests/flipctl_unit.cc
+++ b/tests/flipctl_unit.cc
@@ -184,11 +184,11 @@
 
     // THE TYPED BAND IS A FLOOR, NOT A CEILING. The gate-hygiene lane's instrumented failure: a
     // driver holding its rate to 0.07% across 34 samples, a fingerprint distance of 0.2518 against
-    // a flat --flip-auto-band 2 (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
+    // a flat explicit band of 2% (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
     // A detector whose signature genuinely moves between adjacent quiet windows must end up with a
     // band above that movement whatever the operator typed, and must still fire on a real change.
     {
-        FlipShiftDetector typed(2, 4);   // --flip-auto-band 2
+        FlipShiftDetector typed(2, 4);   // internal explicit-band control: 2%
         // Stationary but NOISY: the read/write split alternates 35:65 / 65:35 window to window.
         // The detector smooths the signature, so the anchored distance this produces is ~0.09.
         const auto noisy = [](int k) { return quiet(k % 2 ? 65 : 35, k % 2 ? 35 : 65); };
@@ -250,8 +250,8 @@
         if (fires) fail("the learned band fired on quiet-state movement");
     }
     // A TYPED BAND UNDER THE REAL SAMPLER. The shipped configuration cannot reach this -- the
-    // controller runs the learned band -- but an operator who sets --flip-auto-band AND the sampler
-    // can, and on a tree where the typed path bypasses the floor the fingerprint lane measured 73
+    // controller runs the learned band. This internal explicit-band control preserves the earlier
+    // regression: where the typed path bypassed the floor the fingerprint lane measured 73
     // two-consecutive exceedances in 600 stationary windows at K=100 and 208 at K=60: a spurious
     // maneuver every few seconds on a load that never changed. Driven here through THEIR writer, a
     // stationary 50/50 stream at depth 32, with the floor in place: none, and a real mix change
@@ -259,7 +259,7 @@
     {
         for (uint32_t window : {60u, 100u}) {
             XorShift rng;
-            FlipShiftDetector typed(2, 8);   // --flip-auto-band 2, the operator's knob
+            FlipShiftDetector typed(2, 8);   // internal explicit-band control: 2%
             int fires = 0, confirmed = 0, streak = 0;
             for (int w = 0; w < 60; w++) {
                 FlipFingerprintWriter writer;
diff --git a/tests/rlcache_churn.py b/tests/rlcache_churn.py
index b56ae4041ae0843dc67443f16017270063853124..f8a2c0730bb0a4fb6c3b5f6282c547226bb76599
--- a/tests/rlcache_churn.py
+++ b/tests/rlcache_churn.py
@@ -59,7 +59,7 @@
 
 
 # THE LOAD BALANCER MUST HAVE SOMETHING TO BALANCE. A uniform key distribution over one small key
-# space keeps every shard at the same op rate, the imbalance never crosses lb-imbalance-pct, and no
+# space keeps every shard at the same op rate, the imbalance never crosses the learned band, and no
 # shard ever changes owner -- which is the precondition for the defect this battery exists for
 # (measured: 7.8M ops, 0 migrations). A hot window that ROTATES makes a few shards hot, then makes
 # different ones hot, so the balancer moves shards continuously and in both directions, which is

exec
/bin/bash -lc "python3 - <<'PY'
from pathlib import Path
import re, json
changed = []
for directory in ('scratch', 'scratchpad'):
    for path in Path(directory).rglob('*.sh'):
        if path.as_posix() in ('scratch/flipfp/fpmargin.sh', 'scratch/flipfp/chain.sh'):
            continue
        old = path.read_text()
        text = re.sub(r' --read-local-atomic-filter 1\\b', '', old)
        text = re.sub(r' --flip-auto-band -1\\b', '', text)
        text = re.sub(r' --flip-auto-band 2\\b', '', text)
        text = re.sub(r' --lb-age-sample-rate 1024\\b', '', text)
        if text "'!= old:
            if path.as_posix() == '"'scratch/rv3.sh':
                text = text.replace('# phase is the row that was failing) x3 with the typed the gate uses, the directed', '# phase is the row that was failing) x3 with the derived band, the directed')
            first, rest = text.split('\\n', 1)
            text = first + '\\n# Boot grammar updated for derived controls; historical PRE/POST results need fresh validation.\\n' + rest
            path.write_text(text)
            changed.append(path.as_posix())
path = Path('scratch/flipfp/chain.sh')
text = path.read_text()
text = text.replace('#   S1b slope 2s with --flip-auto 1 (anchored controller, band 0) PRE vs POST: the always-on cost\\n', '#   S1b retired: the fixed-band controller cost experiment has no equivalent automatic-band arm.\\n')
text = text.replace('if want S1b; then\\n  slope \"pre1-2s\"  \""'$PRE_BIN"  2s "--flip-auto 1 --flip-auto-band 0" 75'"\\n  slope \"post1-2s\" \""'$POST_BIN" 2s "--flip-auto 1 --flip-auto-band 0" 75'"\\nfi\\n', '# S1b deliberately emits no measurements; its historical tables remain readable in S8.\\n')
text = text.replace(' --flip-auto-band 2 --lb-age-sample-rate 1024', '')
text = text.replace('# chain.sh --', '# Boot grammar updated for derived controls; historical PRE/POST results need fresh validation.\\n# chain.sh --', 1)
path.write_text(text)
changed.append(path.as_posix())
Path('scratch/flipfp/fpmargin.sh').unlink()
changed.append('scratch/flipfp/fpmargin.sh (retired)')
path = Path('scratch/flipfp/report.sh')
text = path.read_text().replace('### flip-auto 1 (anchored, band 0)', '### Historical retired fixed-band experiment: flip-auto 1 (anchored, band 0)')
text = text.replace('### gate row tests/flipctl.py (6:2, band 2, age 1024, --stable-seconds 30)', '### tests/flipctl.py (6:2, --stable-seconds 30; band and sampling from each recorded boot)')
path.write_text(text)
changed.append(path.as_posix())
for name in ('scratch/report.py', 'scratch/report2.py'):
    path = Path(name)
    text = path.read_text().replace(\"the gate's own invocation:\", 'the historical gate invocation:').replace(\"the gate's invocation:\", 'the historical gate invocation:')
    text = text.replace('D  GATE ROW', 'D  HISTORICAL GATE ROW').replace('E  GATE ROW', 'E  HISTORICAL GATE ROW')
    text = text.replace('The floor applies under an explicit <code>--flip-auto-band</code> too:', 'In the measured historical build, the floor also applied under the now-removed <code>--flip-auto-band</code>:')
    text = text.replace(' -- this is the row the', ' -- this was the row the')
    path.write_text(text)
    changed.append(name)
(Path('.merge-inputs') / 'scratch-adjustments.json').write_text(json.dumps(changed, indent=2) + '\\n')
print('\\n'.join(changed))
PY" in /home/user/Projects/cx-merge
 succeeded in 0ms:
scratch/rv.sh
scratch/final.sh
scratch/rv3.sh
scratch/finalw.sh
scratch/ver.sh
scratch/red.sh
scratch/red2.sh
scratch/flipfp/fpshift.sh
scratch/flipfp/bat.sh
scratch/flipfp/fpprobe.sh
scratchpad/robdiet/batteries.sh
scratchpad/ringsize/batteries.sh
scratchpad/rlbatch/batteries.sh
scratchpad/replycode/batteries.sh
scratch/flipfp/chain.sh
scratch/flipfp/fpmargin.sh (retired)
scratch/flipfp/report.sh
scratch/report.py
scratch/report2.py

diff --git a/DESIGN.md b/DESIGN.md
index 0fba740d94ba1915868410de40bcb685ae675833..7ad3ac9fef512d8549eadfce44036b8a6a181e9f
--- a/DESIGN.md
+++ b/DESIGN.md
@@ -1,6 +1,6 @@
 # Thread amortization study surface
 
-`thread-mode` and `overlap` are one boot-latched measurement surface. The documented mode
+`thread-mode` and the study-only `x-overlap` are one boot-latched measurement surface. The documented mode
 names are `2s` (separate IO and executor threads, the default) and `1s` (one generalized thread owns
 both roles). `split` and `fused` remain parser aliases so old invocations keep working.
 
@@ -9,10 +9,10 @@
 | `2s` | ordinary split IO/ex loops | exact `t-iopipe` WB/IFID batch schedule | rejected at config validation |
 | `1s` | coarse IFID → EX → WB rotation | exact `t-genthread` `iofused` schedule | gated `iofused` three-way schedule |
 
-The compatibility options `thread-pipeline` and `genthread-schedule` remain accepted: the former
-is a numeric alias for `overlap`, while the latter maps `coarse`, `iofused`, and the legacy
-`streams` name to `1s` plus overlap 0, 1, and 2 respectively. The legacy name is only a parser
-alias; overlap 2 no longer runs the streams implementation. Overlap 2 emits a boot warning because
+Use `--x-overlap 0|1|2` for these study schedules and `--x-ex-sched 0|1` for the executor study.
+The old public schedule spellings and compatibility aliases are removed; see
+[DESIGN-KNOBS.md](DESIGN-KNOBS.md). Below, "overlap" names the internal schedule value.
+Overlap 2 emits a boot warning because
 it is an experimental research schedule. Unified overlap 1 and 2 require `net-io uring`; the split
 iopipe schedule retains its explicit epoll specialization. No schedule constant is runtime-tunable.
 
diff --git a/docs/flipctl-design.md b/docs/flipctl-design.md
index e6a5e31a6c05171b172746f2586a01d09fa1e84e..cd96557199b408c8b85bb42b77ca59ee6fec73ea
--- a/docs/flipctl-design.md
+++ b/docs/flipctl-design.md
@@ -8,8 +8,7 @@
 ## Ownership and footprint
 
 - `src/main.cc` is the controller cron owner. When `flip-auto=1`, the main/monitor thread calls
-  `Server::flipctl_tick()` once per existing LB tick (`lb-tick-ms`; if that mechanism is disabled,
-  the period derives from the provisioned thread count). No IO or EX loop runs controller policy.
+  `Server::flipctl_tick()` on its internal observation cadence. No IO or EX loop runs controller policy.
 - `src/core/flipctl.{h,cc}` contains the fingerprint representation, shift detector, and controller
   state machine. Controller vectors allocate only when `flip-auto=1`.
 - Each `ThreadCtx` embeds one `FlipFingerprintWriter`. It has no dynamic storage. Only the physical
@@ -46,9 +45,9 @@
 measurement quantum, avoiding the zero-anchor discontinuity where one rare admin argument would
 otherwise have distance one.
 
-With `flip-auto-band=-1`, the band is twice the maximum quiet adjacent/drift distance learned at
-the final split, floored by the four-family sampling quantum divided by observed commands. A
-numeric value is a percent. Zero disables fingerprint and collapse re-triggers. Final anchor
+The automatic band is twice the maximum quiet adjacent/drift distance learned at
+the final split, floored by the four-family sampling quantum divided by observed commands.
+It has no public override. Final anchor
 learning consumes a number of closed aggregates derived from the provisioned thread count. It is
 done after maneuver-only age sampling is disarmed, so the controller cannot detect its own signal
 cost transition as a workload change.
@@ -64,13 +63,13 @@
 falls below a threshold derived from its own change jitter and measurement quantum. Directional
 connection ramp-up therefore remains drift, while jittery stationary traffic can qualify even when
 its absolute rate never enters a quiet band. A 30-second ceiling, converted to ticks from
-`lb-tick-ms`, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
+the internal observation cadence, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
 thread per tick resets the learning/cap state, so an idle server remains in
 `awaiting-load-stability` and never pays for a maneuver it cannot use.
 
-`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. An explicit
-`lb-age-sample-rate` is used when nonzero; otherwise the rate derives from the provisioned thread
-count. IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
+`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. Its rate
+derives from observed command volume and the three-tick decision window, and adapts on completed
+traffic windows; see [DESIGN-KNOBS.md](../DESIGN-KNOBS.md). IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
 writes. IO releases its ROB-head age map when the rate returns to zero.
 
 After a stabilized baseline rate, `issue_initial_jump()` subtracts the per-thread maneuver window.
@@ -88,8 +87,8 @@
 not vetoed by idle peers. If either role has no measured work/busy quantity, measurement continues;
 the controller does not invent a workload prior.
 
-The calculation is performed in placement units (one logical CPU normally, one sibling pair under
-`smt-mode=1`) and then reported/committed in threads. The target passes `nio_eq` in the measured
+The calculation is performed in placement units (one logical CPU normally, one sibling pair when
+the allowed topology contains complete pairs) and then reported/committed in threads. The target passes `nio_eq` in the measured
 direction by `max(1, abs(nio_eq-nio_now)/2)`, clamped to one live unit of each role. Even when the
 equalized split rounds to the current split, the direction of greater measured demand supplies the
 deliberate one-unit overshoot.
@@ -143,7 +142,7 @@
 
 ## Integration and observability
 
-- `src/core/config.h` is the sole parser/default source for `flip-auto`, `flip-auto-band`, and
+- `src/core/config.h` is the sole parser/default source for `flip-auto` and
   `flip-work-window`; `src/cmd/t_server.cc` exposes them as immutable CONFIG values.
 - `tomokv.conf` documents the numeric grammar and off/auto semantics.
 - `INFO FLIPCTL` reports state/phase, anchor split/rate, bands, total/per-reason trigger counts, and
diff --git a/docs/specs/AUDIT-TLS.md b/docs/specs/AUDIT-TLS.md
index 0256e887f5d27c210d436bc219f3c7521ddad0db..87a2eaca3137a47f7c0e1567794650f57531860b
--- a/docs/specs/AUDIT-TLS.md
+++ b/docs/specs/AUDIT-TLS.md
@@ -3,7 +3,7 @@
 > **Where it landed (2026-09-04).** The feature shipped as `src/net/tls.{h,cc}` (this document
 > proposes `src/net/tls_ctx.h` + `src/net/tls_conn.h`); the test is `tests/tls.py` (no
 > `tests/gen-certs.sh`); no `tls-bio-size` knob exists. Defaults: AES-128-GCM-first cipher lists
-> and `tls-ktls yes` (`tomokv.conf`, NETWORK).
+> and automatic kTLS negotiation with userspace fallback (`tomokv.conf`, NETWORK).
 
 Governing audit. Read before writing any TLS code in `/home/user/Projects/tomokv-cpp-perthread`.
 No repository was modified in producing this.
diff --git a/scratchpad/rlbatch/lib.sh b/scratchpad/rlbatch/lib.sh
index 7bcf3e4e6030c79174bf35d9bb1b3dfb11f80c42..12184314d2aafac0dc7aa48db761a3bdec5de8c7
--- a/scratchpad/rlbatch/lib.sh
+++ b/scratchpad/rlbatch/lib.sh
@@ -17,7 +17,7 @@
 boot_srv(){ # boot_srv <binary> <logfile> [extra args...] -> SRV pid
   local bin="$1" log="$2"; shift 2
   guard_port "$PORT" || return 1
-  # EXTRA_ARGS lets a caller add boot flags (e.g. --read-local-interleave 0) without a second copy
+  # EXTRA_ARGS lets a caller add boot flags (e.g. --lb 0) without a second copy
   # of this function; word splitting is intentional.
   # shellcheck disable=SC2086
   taskset -c "$SRVCORE" "$bin" --port "$PORT" --bind 127.0.0.1 --shards 16 \
diff --git a/tests/flipctl_unit.cc b/tests/flipctl_unit.cc
index 540a6c40e9589572431075b623bc553c76ee8d85..33209258e2958771d60f86b919f6f510e9977737
--- a/tests/flipctl_unit.cc
+++ b/tests/flipctl_unit.cc
@@ -184,11 +184,11 @@
 
     // THE TYPED BAND IS A FLOOR, NOT A CEILING. The gate-hygiene lane's instrumented failure: a
     // driver holding its rate to 0.07% across 34 samples, a fingerprint distance of 0.2518 against
-    // a flat --flip-auto-band 2 (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
+    // a flat explicit band of 2% (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
     // A detector whose signature genuinely moves between adjacent quiet windows must end up with a
     // band above that movement whatever the operator typed, and must still fire on a real change.
     {
-        FlipShiftDetector typed(2, 4);   // --flip-auto-band 2
+        FlipShiftDetector typed(2, 4);   // internal explicit-band control: 2%
         // Stationary but NOISY: the read/write split alternates 35:65 / 65:35 window to window.
         // The detector smooths the signature, so the anchored distance this produces is ~0.09.
         const auto noisy = [](int k) { return quiet(k % 2 ? 65 : 35, k % 2 ? 35 : 65); };
@@ -250,8 +250,8 @@
         if (fires) fail("the learned band fired on quiet-state movement");
     }
     // A TYPED BAND UNDER THE REAL SAMPLER. The shipped configuration cannot reach this -- the
-    // controller runs the learned band -- but an operator who sets --flip-auto-band AND the sampler
-    // can, and on a tree where the typed path bypasses the floor the fingerprint lane measured 73
+    // controller runs the learned band. This internal explicit-band control preserves the earlier
+    // regression: where the typed path bypassed the floor the fingerprint lane measured 73
     // two-consecutive exceedances in 600 stationary windows at K=100 and 208 at K=60: a spurious
     // maneuver every few seconds on a load that never changed. Driven here through THEIR writer, a
     // stationary 50/50 stream at depth 32, with the floor in place: none, and a real mix change
@@ -259,7 +259,7 @@
     {
         for (uint32_t window : {60u, 100u}) {
             XorShift rng;
-            FlipShiftDetector typed(2, 8);   // --flip-auto-band 2, the operator's knob
+            FlipShiftDetector typed(2, 8);   // internal explicit-band control: 2%
             int fires = 0, confirmed = 0, streak = 0;
             for (int w = 0; w < 60; w++) {
                 FlipFingerprintWriter writer;
diff --git a/tests/rlcache_churn.py b/tests/rlcache_churn.py
index b56ae4041ae0843dc67443f16017270063853124..f8a2c0730bb0a4fb6c3b5f6282c547226bb76599
--- a/tests/rlcache_churn.py
+++ b/tests/rlcache_churn.py
@@ -59,7 +59,7 @@
 
 
 # THE LOAD BALANCER MUST HAVE SOMETHING TO BALANCE. A uniform key distribution over one small key
-# space keeps every shard at the same op rate, the imbalance never crosses lb-imbalance-pct, and no
+# space keeps every shard at the same op rate, the imbalance never crosses the learned band, and no
 # shard ever changes owner -- which is the precondition for the defect this battery exists for
 # (measured: 7.8M ops, 0 migrations). A hot window that ROTATES makes a few shards hot, then makes
 # different ones hot, so the balancer moves shards continuously and in both directions, which is

apply patch
patch: completed
/home/user/Projects/cx-merge/AUDIT-TESTS.md
/home/user/Projects/cx-merge/DESIGN-GATEHYGIENE.md
/home/user/Projects/cx-merge/DESIGN-flipfp.md
/home/user/Projects/cx-merge/NOTES-HYGIENE.md
/home/user/Projects/cx-merge/NOTES-XPERF2.md
diff --git a/AUDIT-TESTS.md b/AUDIT-TESTS.md
index e9cd43ffbc376172717531d0037de1c79fd23690..fe1fd0e2c39e965f605e4a30894a545129541534
--- a/AUDIT-TESTS.md
+++ b/AUDIT-TESTS.md
@@ -122,7 +122,7 @@
 | MM floor (memtier 20 s, sampled 8+6) | 1 | 14 | measurement, keep |
 | saturated-flip memtier | 1 | 25+3 | measurement, keep |
 | `flipctl.py --stable-seconds 30` (+ramp/idle/surge, timeout 300) | 1 | 60-120 est. | measurement, keep |
-| `xshard_dispatch_scale.sh` (2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 | 90-150 est. | measurement, keep |
+| Retired `xshard_dispatch_scale.sh` (historical 2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 historically, 0 now | 90-150 est. historically | manual ownership instrument removed; see `MERGE.md` |
 | `atomic_torn.py` hammers (3+2.5+2+2.5 s) + OFF re-rolls (up to 14 s) + RENAMENX/COPY parks (2 x 64 x 100 ms = 12.8 s) + window/lease/churn (~6 s) | 1 quick, +1 ASAN | ~45-60 each | re-rolls are dead time on this kernel (F6) |
 | `spinprobe.py` (6 s baseline + 6 s probe) | 3 | 37.5 | measurement, keep |
 | Python batteries' literal sleeps (census: flipctl 17 s, limits 5.8, edgetime 4.5, atomic_ryow 4, hexpire 3.4 — most run x2) | | ~60 | keep |
diff --git a/DESIGN-GATEHYGIENE.md b/DESIGN-GATEHYGIENE.md
index 5ea9df93e4548fb73a7e78b38debd52d876ad658..e6ac34a10326fe8880eaa664690832da3875b9b8
--- a/DESIGN-GATEHYGIENE.md
+++ b/DESIGN-GATEHYGIENE.md
@@ -459,7 +459,21 @@
 
 ---
 
-## 10. Follow-up diff on `78c3e5391` — 2026-09-07, runtime validation pending
+## 10. Incoming timing lane on `78c3e5391` — 2026-09-07
+
+**Merged disposition:** the maintainer reports the incoming timing lane gated 344/344 with no
+SKIP rows. The account below records that lane's original implementation and offline validation;
+it is not a validation claim for the merged tree. Its dispatch wrapper/helper and timing evaluator
+are retired: repeatability checks did not replace the deleted manual ownership instrument. The
+configurable-window shrink samples and shrink/reclaim budgets are superseded by the derived-window
+held-burst liveness and old-generation reconfiguration witnesses in `tests/atomicwindow.py`.
+All those mechanism, exact-reply, bound, and credit-reclamation checks remain mandatory on every
+build. The original OFF RENAME four-roll hard-failure control is unchanged, and the 1.5-second
+promotion budget still requires `--release-build`. The merged ledger requires quick 325/full 342;
+the constants remain untouched. [MERGE.md](MERGE.md) records the complete conflict and row accounting.
+
+The rest of this section is historical lane evidence, including its now-superseded dispatch,
+shrink, admission-calibration, and ledger statements.
 
 This is a test-only diff in `cx-timing`. No server, benchmark, gate, or compiler was run. The
 earlier acceptance results above describe earlier changes, **not this diff**. The worked fixes
diff --git a/DESIGN-flipfp.md b/DESIGN-flipfp.md
index 32eb6f46c24fb688db6accc79537fbd664bd084c..f705b25837c72a011da71df06dec09d2757f05ae
--- a/DESIGN-flipfp.md
+++ b/DESIGN-flipfp.md
@@ -311,7 +311,9 @@
 detector. The signal and the floor cannot be read from one run -- with a learned band the trigger
 fires and `reset()` wipes the distance -- so each arm is run twice: `--flip-auto-band 90` holds the
 anchor and lets the distance converge (the signal), `--flip-auto-band -1` learns the floor.
-`scratch/flipfp/fpmargin.sh`.
+The former `scratch/flipfp/fpmargin.sh` implemented this two-band experiment. It is retired in the
+merged tree because neither pinning the band nor disabling its retriggers has a public control.
+These historical measurements remain evidence only for their recorded builds; see `MERGE.md`.
 
 **BITCOUNT -> INCR** (the redesign lane's reference change), 2:2 split, `--rate-limiting` pinned:
 
diff --git a/DESIGN.md b/DESIGN.md
index 0fba740d94ba1915868410de40bcb685ae675833..7ad3ac9fef512d8549eadfce44036b8a6a181e9f
--- a/DESIGN.md
+++ b/DESIGN.md
@@ -1,6 +1,6 @@
 # Thread amortization study surface
 
-`thread-mode` and `overlap` are one boot-latched measurement surface. The documented mode
+`thread-mode` and the study-only `x-overlap` are one boot-latched measurement surface. The documented mode
 names are `2s` (separate IO and executor threads, the default) and `1s` (one generalized thread owns
 both roles). `split` and `fused` remain parser aliases so old invocations keep working.
 
@@ -9,10 +9,10 @@
 | `2s` | ordinary split IO/ex loops | exact `t-iopipe` WB/IFID batch schedule | rejected at config validation |
 | `1s` | coarse IFID → EX → WB rotation | exact `t-genthread` `iofused` schedule | gated `iofused` three-way schedule |
 
-The compatibility options `thread-pipeline` and `genthread-schedule` remain accepted: the former
-is a numeric alias for `overlap`, while the latter maps `coarse`, `iofused`, and the legacy
-`streams` name to `1s` plus overlap 0, 1, and 2 respectively. The legacy name is only a parser
-alias; overlap 2 no longer runs the streams implementation. Overlap 2 emits a boot warning because
+Use `--x-overlap 0|1|2` for these study schedules and `--x-ex-sched 0|1` for the executor study.
+The old public schedule spellings and compatibility aliases are removed; see
+[DESIGN-KNOBS.md](DESIGN-KNOBS.md). Below, "overlap" names the internal schedule value.
+Overlap 2 emits a boot warning because
 it is an experimental research schedule. Unified overlap 1 and 2 require `net-io uring`; the split
 iopipe schedule retains its explicit epoll specialization. No schedule constant is runtime-tunable.
 
diff --git a/NOTES-HYGIENE.md b/NOTES-HYGIENE.md
index 23506bce64f00cb0dccaaff93362ccd30deab229..f224db07f4ceb2e4791c7cbb471b87d83a0a3e93
--- a/NOTES-HYGIENE.md
+++ b/NOTES-HYGIENE.md
@@ -352,7 +352,8 @@
 ## Test-file census
 
 The calibration examples expireindex.py, borrow_registry.py, and xshard_dispatch_scale.sh were
-already wired in the current parent. aof_frames.py is indirectly executed by aof_frame_order.py;
+already wired in this note's historical parent. The merged tree retires the dispatch instrument;
+the two surviving batteries use `--release-build`. aof_frames.py is indirectly executed by aof_frame_order.py;
 aof_rewrite.py and aof_rewrite_triggers.py are driven by matrix wrappers; differ.py is driven by
 differ_gate.sh; niclib.sh and gate_refs.txt support optional full-tier NIC rows.
 
@@ -456,7 +457,7 @@
 notify: ok (notify_events_fired=1608)
 ~~~
 
-Purpose-boot matrix commands:
+Historical purpose-boot matrix commands (superseded by the current gate's network-engine grammar):
 
 ~~~sh
 PERSIST_IO=normal GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_matrix.sh
@@ -465,7 +466,8 @@
 PERSIST_IO=uring  GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_trigger_matrix.sh
 ~~~
 
-Other purpose-booted battery invocations (each boot used the knobs documented by gate.sh):
+Historical purpose-booted battery invocations (not recipes for the merged tree; the dispatch
+instrument and script OFF arm below are retired):
 
 ~~~sh
 python3 tests/xscript.py 127.0.0.1 7600 off|limit|window
diff --git a/NOTES-XPERF2.md b/NOTES-XPERF2.md
index d4ce7950b1396997d44f0945f6f5179aa625e59e..d1edc822a5e7abc68e6aefc3d5a55986b0bf178e
--- a/NOTES-XPERF2.md
+++ b/NOTES-XPERF2.md
@@ -8,7 +8,7 @@
 |---|---|---|---|---|
 | E4 | ExpireIndex never shrinks; memsets the whole sidecar on live→0; single-pass rehash | **REPRODUCED** (both halves) | yes | `tests/expireindex.py` |
 | E5 | zero-copy borrow registry is a linear vector scanned per borrow/release/retire | **REPRODUCED** | yes | `tests/borrow_registry.py` |
-| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes | `tests/xshard_dispatch_scale.sh` |
+| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes, historically | retired manual-map dispatch instrument; see `MERGE.md` |
 | E7 | segment mode is absorbing: once triggered every later reply is a malloc + iovec slot | **NOT REPRODUCED** | no | none |
 
 Rig: EPYC 9754, cores 48–63 only, ports 7200–7209 only, loopback. Oracle = vanilla redis 7.4 at
@@ -292,18 +292,20 @@
 |---|---|---|---|---|
 | `tests/expireindex.py <host> <port>` | `--shards 1 --ratio 1:1 --enable-debug-command yes` | growth-trigger cost ratio over a 16× population ≤ 8.0; live→0 DEL ≤ 1.25× a neighbouring DEL | FAIL 2 (16.30×, 2.00×) | PASS (1.65×, 1.00×) |
 | `tests/borrow_registry.py <host> <port>` | `--shards 1 --zc-min 64 --client-output-buffer-limit normal 0 0 0 --enable-debug-command yes` | borrowed-GET per-op cost growth over 0 → ~2000 live borrows ≤ 1.05 | FAIL (1.078, 1.079) | PASS (1.008, 1.008, 1.009) |
-| `tests/xshard_dispatch_scale.sh` (+ `.py`) | boots its own 4-thread and 128-thread arms | dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
+| Retired `tests/xshard_dispatch_scale.sh` (+ `.py`) | historically booted fixed-owner 4-thread and 128-thread arms | historical dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
 
 Each also carries its own non-vacuity checks: `expireindex.py` asserts INFO `expires` really
 reached the population under test and that INFO `expired_keys` MOVED (so the sampler is proven to
 still visit deadlines parked mid-migration), plus a no-TTL negative control on both timing legs;
 `borrow_registry.py` asserts `CLIENT LIST oll` grew (the borrow path is proven live, not assumed),
 carries a non-borrowed control arm, and refuses to run unless the zc gate straddles its two probe
-sizes; `xshard_dispatch_scale.py` refuses to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` prove
+sizes; the retired `xshard_dispatch_scale.py` refused to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` proved
 the cross pair really fanned out to two owner threads and the control stayed on one.
 
-None of the three is wired into `tests/gate.sh`: they each need their own boot geometry and the
-gate's port/cores belong to the mainline operator. Run them as above.
+These were standalone instruments when measured. The merged gate runs the expiry and borrow
+batteries with `--release-build`. The dispatch wrapper and helper are retired because their manual
+ownership geometry is unavailable; the historical measurements above do not validate default
+round-robin ownership. See [MERGE.md](MERGE.md).
 
 ## Evidence
 
diff --git a/docs/flipctl-design.md b/docs/flipctl-design.md
index e6a5e31a6c05171b172746f2586a01d09fa1e84e..cd96557199b408c8b85bb42b77ca59ee6fec73ea
--- a/docs/flipctl-design.md
+++ b/docs/flipctl-design.md
@@ -8,8 +8,7 @@
 ## Ownership and footprint
 
 - `src/main.cc` is the controller cron owner. When `flip-auto=1`, the main/monitor thread calls
-  `Server::flipctl_tick()` once per existing LB tick (`lb-tick-ms`; if that mechanism is disabled,
-  the period derives from the provisioned thread count). No IO or EX loop runs controller policy.
+  `Server::flipctl_tick()` on its internal observation cadence. No IO or EX loop runs controller policy.
 - `src/core/flipctl.{h,cc}` contains the fingerprint representation, shift detector, and controller
   state machine. Controller vectors allocate only when `flip-auto=1`.
 - Each `ThreadCtx` embeds one `FlipFingerprintWriter`. It has no dynamic storage. Only the physical
@@ -46,9 +45,9 @@
 measurement quantum, avoiding the zero-anchor discontinuity where one rare admin argument would
 otherwise have distance one.
 
-With `flip-auto-band=-1`, the band is twice the maximum quiet adjacent/drift distance learned at
-the final split, floored by the four-family sampling quantum divided by observed commands. A
-numeric value is a percent. Zero disables fingerprint and collapse re-triggers. Final anchor
+The automatic band is twice the maximum quiet adjacent/drift distance learned at
+the final split, floored by the four-family sampling quantum divided by observed commands.
+It has no public override. Final anchor
 learning consumes a number of closed aggregates derived from the provisioned thread count. It is
 done after maneuver-only age sampling is disarmed, so the controller cannot detect its own signal
 cost transition as a workload change.
@@ -64,13 +63,13 @@
 falls below a threshold derived from its own change jitter and measurement quantum. Directional
 connection ramp-up therefore remains drift, while jittery stationary traffic can qualify even when
 its absolute rate never enters a quiet band. A 30-second ceiling, converted to ticks from
-`lb-tick-ms`, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
+the internal observation cadence, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
 thread per tick resets the learning/cap state, so an idle server remains in
 `awaiting-load-stability` and never pays for a maneuver it cannot use.
 
-`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. An explicit
-`lb-age-sample-rate` is used when nonzero; otherwise the rate derives from the provisioned thread
-count. IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
+`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. Its rate
+derives from observed command volume and the three-tick decision window, and adapts on completed
+traffic windows; see [DESIGN-KNOBS.md](../DESIGN-KNOBS.md). IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
 writes. IO releases its ROB-head age map when the rate returns to zero.
 
 After a stabilized baseline rate, `issue_initial_jump()` subtracts the per-thread maneuver window.
@@ -88,8 +87,8 @@
 not vetoed by idle peers. If either role has no measured work/busy quantity, measurement continues;
 the controller does not invent a workload prior.
 
-The calculation is performed in placement units (one logical CPU normally, one sibling pair under
-`smt-mode=1`) and then reported/committed in threads. The target passes `nio_eq` in the measured
+The calculation is performed in placement units (one logical CPU normally, one sibling pair when
+the allowed topology contains complete pairs) and then reported/committed in threads. The target passes `nio_eq` in the measured
 direction by `max(1, abs(nio_eq-nio_now)/2)`, clamped to one live unit of each role. Even when the
 equalized split rounds to the current split, the direction of greater measured demand supplies the
 deliberate one-unit overshoot.
@@ -143,7 +142,7 @@
 
 ## Integration and observability
 
-- `src/core/config.h` is the sole parser/default source for `flip-auto`, `flip-auto-band`, and
+- `src/core/config.h` is the sole parser/default source for `flip-auto` and
   `flip-work-window`; `src/cmd/t_server.cc` exposes them as immutable CONFIG values.
 - `tomokv.conf` documents the numeric grammar and off/auto semantics.
 - `INFO FLIPCTL` reports state/phase, anchor split/rate, bands, total/per-reason trigger counts, and
diff --git a/docs/specs/AUDIT-TLS.md b/docs/specs/AUDIT-TLS.md
index 0256e887f5d27c210d436bc219f3c7521ddad0db..87a2eaca3137a47f7c0e1567794650f57531860b
--- a/docs/specs/AUDIT-TLS.md
+++ b/docs/specs/AUDIT-TLS.md
@@ -3,7 +3,7 @@
 > **Where it landed (2026-09-04).** The feature shipped as `src/net/tls.{h,cc}` (this document
 > proposes `src/net/tls_ctx.h` + `src/net/tls_conn.h`); the test is `tests/tls.py` (no
 > `tests/gen-certs.sh`); no `tls-bio-size` knob exists. Defaults: AES-128-GCM-first cipher lists
-> and `tls-ktls yes` (`tomokv.conf`, NETWORK).
+> and automatic kTLS negotiation with userspace fallback (`tomokv.conf`, NETWORK).
 
 Governing audit. Read before writing any TLS code in `/home/user/Projects/tomokv-cpp-perthread`.
 No repository was modified in producing this.
diff --git a/scratchpad/rlbatch/lib.sh b/scratchpad/rlbatch/lib.sh
index 7bcf3e4e6030c79174bf35d9bb1b3dfb11f80c42..12184314d2aafac0dc7aa48db761a3bdec5de8c7
--- a/scratchpad/rlbatch/lib.sh
+++ b/scratchpad/rlbatch/lib.sh
@@ -17,7 +17,7 @@
 boot_srv(){ # boot_srv <binary> <logfile> [extra args...] -> SRV pid
   local bin="$1" log="$2"; shift 2
   guard_port "$PORT" || return 1
-  # EXTRA_ARGS lets a caller add boot flags (e.g. --read-local-interleave 0) without a second copy
+  # EXTRA_ARGS lets a caller add boot flags (e.g. --lb 0) without a second copy
   # of this function; word splitting is intentional.
   # shellcheck disable=SC2086
   taskset -c "$SRVCORE" "$bin" --port "$PORT" --bind 127.0.0.1 --shards 16 \
diff --git a/tests/flipctl_unit.cc b/tests/flipctl_unit.cc
index 540a6c40e9589572431075b623bc553c76ee8d85..33209258e2958771d60f86b919f6f510e9977737
--- a/tests/flipctl_unit.cc
+++ b/tests/flipctl_unit.cc
@@ -184,11 +184,11 @@
 
     // THE TYPED BAND IS A FLOOR, NOT A CEILING. The gate-hygiene lane's instrumented failure: a
     // driver holding its rate to 0.07% across 34 samples, a fingerprint distance of 0.2518 against
-    // a flat --flip-auto-band 2 (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
+    // a flat explicit band of 2% (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
     // A detector whose signature genuinely moves between adjacent quiet windows must end up with a
     // band above that movement whatever the operator typed, and must still fire on a real change.
     {
-        FlipShiftDetector typed(2, 4);   // --flip-auto-band 2
+        FlipShiftDetector typed(2, 4);   // internal explicit-band control: 2%
         // Stationary but NOISY: the read/write split alternates 35:65 / 65:35 window to window.
         // The detector smooths the signature, so the anchored distance this produces is ~0.09.
         const auto noisy = [](int k) { return quiet(k % 2 ? 65 : 35, k % 2 ? 35 : 65); };
@@ -250,8 +250,8 @@
         if (fires) fail("the learned band fired on quiet-state movement");
     }
     // A TYPED BAND UNDER THE REAL SAMPLER. The shipped configuration cannot reach this -- the
-    // controller runs the learned band -- but an operator who sets --flip-auto-band AND the sampler
-    // can, and on a tree where the typed path bypasses the floor the fingerprint lane measured 73
+    // controller runs the learned band. This internal explicit-band control preserves the earlier
+    // regression: where the typed path bypassed the floor the fingerprint lane measured 73
     // two-consecutive exceedances in 600 stationary windows at K=100 and 208 at K=60: a spurious
     // maneuver every few seconds on a load that never changed. Driven here through THEIR writer, a
     // stationary 50/50 stream at depth 32, with the floor in place: none, and a real mix change
@@ -259,7 +259,7 @@
     {
         for (uint32_t window : {60u, 100u}) {
             XorShift rng;
-            FlipShiftDetector typed(2, 8);   // --flip-auto-band 2, the operator's knob
+            FlipShiftDetector typed(2, 8);   // internal explicit-band control: 2%
             int fires = 0, confirmed = 0, streak = 0;
             for (int w = 0; w < 60; w++) {
                 FlipFingerprintWriter writer;
diff --git a/tests/rlcache_churn.py b/tests/rlcache_churn.py
index b56ae4041ae0843dc67443f16017270063853124..f8a2c0730bb0a4fb6c3b5f6282c547226bb76599
--- a/tests/rlcache_churn.py
+++ b/tests/rlcache_churn.py
@@ -59,7 +59,7 @@
 
 
 # THE LOAD BALANCER MUST HAVE SOMETHING TO BALANCE. A uniform key distribution over one small key
-# space keeps every shard at the same op rate, the imbalance never crosses lb-imbalance-pct, and no
+# space keeps every shard at the same op rate, the imbalance never crosses the learned band, and no
 # shard ever changes owner -- which is the precondition for the defect this battery exists for
 # (measured: 7.8M ops, 0 migrations). A hot window that ROTATES makes a few shards hot, then makes
 # different ones hot, so the balancer moves shards continuously and in both directions, which is

diff --git a/AUDIT-TESTS.md b/AUDIT-TESTS.md
index e9cd43ffbc376172717531d0037de1c79fd23690..fe1fd0e2c39e965f605e4a30894a545129541534
--- a/AUDIT-TESTS.md
+++ b/AUDIT-TESTS.md
@@ -122,7 +122,7 @@
 | MM floor (memtier 20 s, sampled 8+6) | 1 | 14 | measurement, keep |
 | saturated-flip memtier | 1 | 25+3 | measurement, keep |
 | `flipctl.py --stable-seconds 30` (+ramp/idle/surge, timeout 300) | 1 | 60-120 est. | measurement, keep |
-| `xshard_dispatch_scale.sh` (2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 | 90-150 est. | measurement, keep |
+| Retired `xshard_dispatch_scale.sh` (historical 2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 historically, 0 now | 90-150 est. historically | manual ownership instrument removed; see `MERGE.md` |
 | `atomic_torn.py` hammers (3+2.5+2+2.5 s) + OFF re-rolls (up to 14 s) + RENAMENX/COPY parks (2 x 64 x 100 ms = 12.8 s) + window/lease/churn (~6 s) | 1 quick, +1 ASAN | ~45-60 each | re-rolls are dead time on this kernel (F6) |
 | `spinprobe.py` (6 s baseline + 6 s probe) | 3 | 37.5 | measurement, keep |
 | Python batteries' literal sleeps (census: flipctl 17 s, limits 5.8, edgetime 4.5, atomic_ryow 4, hexpire 3.4 — most run x2) | | ~60 | keep |
diff --git a/DESIGN-GATEHYGIENE.md b/DESIGN-GATEHYGIENE.md
index 5ea9df93e4548fb73a7e78b38debd52d876ad658..e6ac34a10326fe8880eaa664690832da3875b9b8
--- a/DESIGN-GATEHYGIENE.md
+++ b/DESIGN-GATEHYGIENE.md
@@ -459,7 +459,21 @@
 
 ---
 
-## 10. Follow-up diff on `78c3e5391` — 2026-09-07, runtime validation pending
+## 10. Incoming timing lane on `78c3e5391` — 2026-09-07
+
+**Merged disposition:** the maintainer reports the incoming timing lane gated 344/344 with no
+SKIP rows. The account below records that lane's original implementation and offline validation;
+it is not a validation claim for the merged tree. Its dispatch wrapper/helper and timing evaluator
+are retired: repeatability checks did not replace the deleted manual ownership instrument. The
+configurable-window shrink samples and shrink/reclaim budgets are superseded by the derived-window
+held-burst liveness and old-generation reconfiguration witnesses in `tests/atomicwindow.py`.
+All those mechanism, exact-reply, bound, and credit-reclamation checks remain mandatory on every
+build. The original OFF RENAME four-roll hard-failure control is unchanged, and the 1.5-second
+promotion budget still requires `--release-build`. The merged ledger requires quick 325/full 342;
+the constants remain untouched. [MERGE.md](MERGE.md) records the complete conflict and row accounting.
+
+The rest of this section is historical lane evidence, including its now-superseded dispatch,
+shrink, admission-calibration, and ledger statements.
 
 This is a test-only diff in `cx-timing`. No server, benchmark, gate, or compiler was run. The
 earlier acceptance results above describe earlier changes, **not this diff**. The worked fixes
diff --git a/DESIGN-flipfp.md b/DESIGN-flipfp.md
index 32eb6f46c24fb688db6accc79537fbd664bd084c..f705b25837c72a011da71df06dec09d2757f05ae
--- a/DESIGN-flipfp.md
+++ b/DESIGN-flipfp.md
@@ -311,7 +311,9 @@
 detector. The signal and the floor cannot be read from one run -- with a learned band the trigger
 fires and `reset()` wipes the distance -- so each arm is run twice: `--flip-auto-band 90` holds the
 anchor and lets the distance converge (the signal), `--flip-auto-band -1` learns the floor.
-`scratch/flipfp/fpmargin.sh`.
+The former `scratch/flipfp/fpmargin.sh` implemented this two-band experiment. It is retired in the
+merged tree because neither pinning the band nor disabling its retriggers has a public control.
+These historical measurements remain evidence only for their recorded builds; see `MERGE.md`.
 
 **BITCOUNT -> INCR** (the redesign lane's reference change), 2:2 split, `--rate-limiting` pinned:
 
diff --git a/DESIGN.md b/DESIGN.md
index 0fba740d94ba1915868410de40bcb685ae675833..7ad3ac9fef512d8549eadfce44036b8a6a181e9f
--- a/DESIGN.md
+++ b/DESIGN.md
@@ -1,6 +1,6 @@
 # Thread amortization study surface
 
-`thread-mode` and `overlap` are one boot-latched measurement surface. The documented mode
+`thread-mode` and the study-only `x-overlap` are one boot-latched measurement surface. The documented mode
 names are `2s` (separate IO and executor threads, the default) and `1s` (one generalized thread owns
 both roles). `split` and `fused` remain parser aliases so old invocations keep working.
 
@@ -9,10 +9,10 @@
 | `2s` | ordinary split IO/ex loops | exact `t-iopipe` WB/IFID batch schedule | rejected at config validation |
 | `1s` | coarse IFID → EX → WB rotation | exact `t-genthread` `iofused` schedule | gated `iofused` three-way schedule |
 
-The compatibility options `thread-pipeline` and `genthread-schedule` remain accepted: the former
-is a numeric alias for `overlap`, while the latter maps `coarse`, `iofused`, and the legacy
-`streams` name to `1s` plus overlap 0, 1, and 2 respectively. The legacy name is only a parser
-alias; overlap 2 no longer runs the streams implementation. Overlap 2 emits a boot warning because
+Use `--x-overlap 0|1|2` for these study schedules and `--x-ex-sched 0|1` for the executor study.
+The old public schedule spellings and compatibility aliases are removed; see
+[DESIGN-KNOBS.md](DESIGN-KNOBS.md). Below, "overlap" names the internal schedule value.
+Overlap 2 emits a boot warning because
 it is an experimental research schedule. Unified overlap 1 and 2 require `net-io uring`; the split
 iopipe schedule retains its explicit epoll specialization. No schedule constant is runtime-tunable.
 
diff --git a/NOTES-HYGIENE.md b/NOTES-HYGIENE.md
index 23506bce64f00cb0dccaaff93362ccd30deab229..f224db07f4ceb2e4791c7cbb471b87d83a0a3e93
--- a/NOTES-HYGIENE.md
+++ b/NOTES-HYGIENE.md
@@ -352,7 +352,8 @@
 ## Test-file census
 
 The calibration examples expireindex.py, borrow_registry.py, and xshard_dispatch_scale.sh were
-already wired in the current parent. aof_frames.py is indirectly executed by aof_frame_order.py;
+already wired in this note's historical parent. The merged tree retires the dispatch instrument;
+the two surviving batteries use `--release-build`. aof_frames.py is indirectly executed by aof_frame_order.py;
 aof_rewrite.py and aof_rewrite_triggers.py are driven by matrix wrappers; differ.py is driven by
 differ_gate.sh; niclib.sh and gate_refs.txt support optional full-tier NIC rows.
 
@@ -456,7 +457,7 @@
 notify: ok (notify_events_fired=1608)
 ~~~
 
-Purpose-boot matrix commands:
+Historical purpose-boot matrix commands (superseded by the current gate's network-engine grammar):
 
 ~~~sh
 PERSIST_IO=normal GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_matrix.sh
@@ -465,7 +466,8 @@
 PERSIST_IO=uring  GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_trigger_matrix.sh
 ~~~
 
-Other purpose-booted battery invocations (each boot used the knobs documented by gate.sh):
+Historical purpose-booted battery invocations (not recipes for the merged tree; the dispatch
+instrument and script OFF arm below are retired):
 
 ~~~sh
 python3 tests/xscript.py 127.0.0.1 7600 off|limit|window
diff --git a/NOTES-XPERF2.md b/NOTES-XPERF2.md
index d4ce7950b1396997d44f0945f6f5179aa625e59e..d1edc822a5e7abc68e6aefc3d5a55986b0bf178e
--- a/NOTES-XPERF2.md
+++ b/NOTES-XPERF2.md
@@ -8,7 +8,7 @@
 |---|---|---|---|---|
 | E4 | ExpireIndex never shrinks; memsets the whole sidecar on live→0; single-pass rehash | **REPRODUCED** (both halves) | yes | `tests/expireindex.py` |
 | E5 | zero-copy borrow registry is a linear vector scanned per borrow/release/retire | **REPRODUCED** | yes | `tests/borrow_registry.py` |
-| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes | `tests/xshard_dispatch_scale.sh` |
+| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes, historically | retired manual-map dispatch instrument; see `MERGE.md` |
 | E7 | segment mode is absorbing: once triggered every later reply is a malloc + iovec slot | **NOT REPRODUCED** | no | none |
 
 Rig: EPYC 9754, cores 48–63 only, ports 7200–7209 only, loopback. Oracle = vanilla redis 7.4 at
@@ -292,18 +292,20 @@
 |---|---|---|---|---|
 | `tests/expireindex.py <host> <port>` | `--shards 1 --ratio 1:1 --enable-debug-command yes` | growth-trigger cost ratio over a 16× population ≤ 8.0; live→0 DEL ≤ 1.25× a neighbouring DEL | FAIL 2 (16.30×, 2.00×) | PASS (1.65×, 1.00×) |
 | `tests/borrow_registry.py <host> <port>` | `--shards 1 --zc-min 64 --client-output-buffer-limit normal 0 0 0 --enable-debug-command yes` | borrowed-GET per-op cost growth over 0 → ~2000 live borrows ≤ 1.05 | FAIL (1.078, 1.079) | PASS (1.008, 1.008, 1.009) |
-| `tests/xshard_dispatch_scale.sh` (+ `.py`) | boots its own 4-thread and 128-thread arms | dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
+| Retired `tests/xshard_dispatch_scale.sh` (+ `.py`) | historically booted fixed-owner 4-thread and 128-thread arms | historical dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
 
 Each also carries its own non-vacuity checks: `expireindex.py` asserts INFO `expires` really
 reached the population under test and that INFO `expired_keys` MOVED (so the sampler is proven to
 still visit deadlines parked mid-migration), plus a no-TTL negative control on both timing legs;
 `borrow_registry.py` asserts `CLIENT LIST oll` grew (the borrow path is proven live, not assumed),
 carries a non-borrowed control arm, and refuses to run unless the zc gate straddles its two probe
-sizes; `xshard_dispatch_scale.py` refuses to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` prove
+sizes; the retired `xshard_dispatch_scale.py` refused to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` proved
 the cross pair really fanned out to two owner threads and the control stayed on one.
 
-None of the three is wired into `tests/gate.sh`: they each need their own boot geometry and the
-gate's port/cores belong to the mainline operator. Run them as above.
+These were standalone instruments when measured. The merged gate runs the expiry and borrow
+batteries with `--release-build`. The dispatch wrapper and helper are retired because their manual
+ownership geometry is unavailable; the historical measurements above do not validate default
+round-robin ownership. See [MERGE.md](MERGE.md).
 
 ## Evidence
 
diff --git a/docs/flipctl-design.md b/docs/flipctl-design.md
index e6a5e31a6c05171b172746f2586a01d09fa1e84e..cd96557199b408c8b85bb42b77ca59ee6fec73ea
--- a/docs/flipctl-design.md
+++ b/docs/flipctl-design.md
@@ -8,8 +8,7 @@
 ## Ownership and footprint
 
 - `src/main.cc` is the controller cron owner. When `flip-auto=1`, the main/monitor thread calls
-  `Server::flipctl_tick()` once per existing LB tick (`lb-tick-ms`; if that mechanism is disabled,
-  the period derives from the provisioned thread count). No IO or EX loop runs controller policy.
+  `Server::flipctl_tick()` on its internal observation cadence. No IO or EX loop runs controller policy.
 - `src/core/flipctl.{h,cc}` contains the fingerprint representation, shift detector, and controller
   state machine. Controller vectors allocate only when `flip-auto=1`.
 - Each `ThreadCtx` embeds one `FlipFingerprintWriter`. It has no dynamic storage. Only the physical
@@ -46,9 +45,9 @@
 measurement quantum, avoiding the zero-anchor discontinuity where one rare admin argument would
 otherwise have distance one.
 
-With `flip-auto-band=-1`, the band is twice the maximum quiet adjacent/drift distance learned at
-the final split, floored by the four-family sampling quantum divided by observed commands. A
-numeric value is a percent. Zero disables fingerprint and collapse re-triggers. Final anchor
+The automatic band is twice the maximum quiet adjacent/drift distance learned at
+the final split, floored by the four-family sampling quantum divided by observed commands.
+It has no public override. Final anchor
 learning consumes a number of closed aggregates derived from the provisioned thread count. It is
 done after maneuver-only age sampling is disarmed, so the controller cannot detect its own signal
 cost transition as a workload change.
@@ -64,13 +63,13 @@
 falls below a threshold derived from its own change jitter and measurement quantum. Directional
 connection ramp-up therefore remains drift, while jittery stationary traffic can qualify even when
 its absolute rate never enters a quiet band. A 30-second ceiling, converted to ticks from
-`lb-tick-ms`, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
+the internal observation cadence, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
 thread per tick resets the learning/cap state, so an idle server remains in
 `awaiting-load-stability` and never pays for a maneuver it cannot use.
 
-`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. An explicit
-`lb-age-sample-rate` is used when nonzero; otherwise the rate derives from the provisioned thread
-count. IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
+`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. Its rate
+derives from observed command volume and the three-tick decision window, and adapts on completed
+traffic windows; see [DESIGN-KNOBS.md](../DESIGN-KNOBS.md). IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
 writes. IO releases its ROB-head age map when the rate returns to zero.
 
 After a stabilized baseline rate, `issue_initial_jump()` subtracts the per-thread maneuver window.
@@ -88,8 +87,8 @@
 not vetoed by idle peers. If either role has no measured work/busy quantity, measurement continues;
 the controller does not invent a workload prior.
 
-The calculation is performed in placement units (one logical CPU normally, one sibling pair under
-`smt-mode=1`) and then reported/committed in threads. The target passes `nio_eq` in the measured
+The calculation is performed in placement units (one logical CPU normally, one sibling pair when
+the allowed topology contains complete pairs) and then reported/committed in threads. The target passes `nio_eq` in the measured
 direction by `max(1, abs(nio_eq-nio_now)/2)`, clamped to one live unit of each role. Even when the
 equalized split rounds to the current split, the direction of greater measured demand supplies the
 deliberate one-unit overshoot.
@@ -143,7 +142,7 @@
 
 ## Integration and observability
 
-- `src/core/config.h` is the sole parser/default source for `flip-auto`, `flip-auto-band`, and
+- `src/core/config.h` is the sole parser/default source for `flip-auto` and
   `flip-work-window`; `src/cmd/t_server.cc` exposes them as immutable CONFIG values.
 - `tomokv.conf` documents the numeric grammar and off/auto semantics.
 - `INFO FLIPCTL` reports state/phase, anchor split/rate, bands, total/per-reason trigger counts, and
diff --git a/docs/specs/AUDIT-TLS.md b/docs/specs/AUDIT-TLS.md
index 0256e887f5d27c210d436bc219f3c7521ddad0db..87a2eaca3137a47f7c0e1567794650f57531860b
--- a/docs/specs/AUDIT-TLS.md
+++ b/docs/specs/AUDIT-TLS.md
@@ -3,7 +3,7 @@
 > **Where it landed (2026-09-04).** The feature shipped as `src/net/tls.{h,cc}` (this document
 > proposes `src/net/tls_ctx.h` + `src/net/tls_conn.h`); the test is `tests/tls.py` (no
 > `tests/gen-certs.sh`); no `tls-bio-size` knob exists. Defaults: AES-128-GCM-first cipher lists
-> and `tls-ktls yes` (`tomokv.conf`, NETWORK).
+> and automatic kTLS negotiation with userspace fallback (`tomokv.conf`, NETWORK).
 
 Governing audit. Read before writing any TLS code in `/home/user/Projects/tomokv-cpp-perthread`.
 No repository was modified in producing this.
diff --git a/scratchpad/rlbatch/lib.sh b/scratchpad/rlbatch/lib.sh
index 7bcf3e4e6030c79174bf35d9bb1b3dfb11f80c42..12184314d2aafac0dc7aa48db761a3bdec5de8c7
--- a/scratchpad/rlbatch/lib.sh
+++ b/scratchpad/rlbatch/lib.sh
@@ -17,7 +17,7 @@
 boot_srv(){ # boot_srv <binary> <logfile> [extra args...] -> SRV pid
   local bin="$1" log="$2"; shift 2
   guard_port "$PORT" || return 1
-  # EXTRA_ARGS lets a caller add boot flags (e.g. --read-local-interleave 0) without a second copy
+  # EXTRA_ARGS lets a caller add boot flags (e.g. --lb 0) without a second copy
   # of this function; word splitting is intentional.
   # shellcheck disable=SC2086
   taskset -c "$SRVCORE" "$bin" --port "$PORT" --bind 127.0.0.1 --shards 16 \
diff --git a/tests/flipctl_unit.cc b/tests/flipctl_unit.cc
index 540a6c40e9589572431075b623bc553c76ee8d85..33209258e2958771d60f86b919f6f510e9977737
--- a/tests/flipctl_unit.cc
+++ b/tests/flipctl_unit.cc
@@ -184,11 +184,11 @@
 
     // THE TYPED BAND IS A FLOOR, NOT A CEILING. The gate-hygiene lane's instrumented failure: a
     // driver holding its rate to 0.07% across 34 samples, a fingerprint distance of 0.2518 against
-    // a flat --flip-auto-band 2 (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
+    // a flat explicit band of 2% (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
     // A detector whose signature genuinely moves between adjacent quiet windows must end up with a
     // band above that movement whatever the operator typed, and must still fire on a real change.
     {
-        FlipShiftDetector typed(2, 4);   // --flip-auto-band 2
+        FlipShiftDetector typed(2, 4);   // internal explicit-band control: 2%
         // Stationary but NOISY: the read/write split alternates 35:65 / 65:35 window to window.
         // The detector smooths the signature, so the anchored distance this produces is ~0.09.
         const auto noisy = [](int k) { return quiet(k % 2 ? 65 : 35, k % 2 ? 35 : 65); };
@@ -250,8 +250,8 @@
         if (fires) fail("the learned band fired on quiet-state movement");
     }
     // A TYPED BAND UNDER THE REAL SAMPLER. The shipped configuration cannot reach this -- the
-    // controller runs the learned band -- but an operator who sets --flip-auto-band AND the sampler
-    // can, and on a tree where the typed path bypasses the floor the fingerprint lane measured 73
+    // controller runs the learned band. This internal explicit-band control preserves the earlier
+    // regression: where the typed path bypassed the floor the fingerprint lane measured 73
     // two-consecutive exceedances in 600 stationary windows at K=100 and 208 at K=60: a spurious
     // maneuver every few seconds on a load that never changed. Driven here through THEIR writer, a
     // stationary 50/50 stream at depth 32, with the floor in place: none, and a real mix change
@@ -259,7 +259,7 @@
     {
         for (uint32_t window : {60u, 100u}) {
             XorShift rng;
-            FlipShiftDetector typed(2, 8);   // --flip-auto-band 2, the operator's knob
+            FlipShiftDetector typed(2, 8);   // internal explicit-band control: 2%
             int fires = 0, confirmed = 0, streak = 0;
             for (int w = 0; w < 60; w++) {
                 FlipFingerprintWriter writer;
diff --git a/tests/rlcache_churn.py b/tests/rlcache_churn.py
index b56ae4041ae0843dc67443f16017270063853124..f8a2c0730bb0a4fb6c3b5f6282c547226bb76599
--- a/tests/rlcache_churn.py
+++ b/tests/rlcache_churn.py
@@ -59,7 +59,7 @@
 
 
 # THE LOAD BALANCER MUST HAVE SOMETHING TO BALANCE. A uniform key distribution over one small key
-# space keeps every shard at the same op rate, the imbalance never crosses lb-imbalance-pct, and no
+# space keeps every shard at the same op rate, the imbalance never crosses the learned band, and no
 # shard ever changes owner -- which is the precondition for the defect this battery exists for
 # (measured: 7.8M ops, 0 migrations). A hot window that ROTATES makes a few shards hot, then makes
 # different ones hot, so the balancer moves shards continuously and in both directions, which is

codex
The row accounting still comes to quick 325 / full 342: the script OFF arm and dispatch-scaling row each remove one row before the quick exit. The replacement configuration check still emits four rows, and the new helpers add no outer gate rows. I’m now checking syntax, source-lane preservation, removed-flag references, and the final conflict record.
exec
/bin/bash -lc "python3 - <<'PY'
from pathlib import Path
import subprocess, re, os, json
pattern = re.compile(r'read-local-prefetch-capture|read-local-atomic-filter|read-local-interleave|flip-auto-band|l3-domains|smt-mode|genthread-schedule|atomic-window|persist-io|lru-clock-shift|script-crossshard-(?:max-bytes|workbench-bytes|conflict-retries|cut-slots)|tls-ktls|key-lb|client-lb|lb-(?:sample-rate|age-sample-rate|tick-ms|imbalance-pct|move-cap|cooldown-ms)|--ex-sched|--overlap|thread-pipeline')
paths = subprocess.check_output(['git', 'ls-files', '*.md'], text=True).splitlines()
changed = []
for name in paths:
    if name in ('DESIGN.md', 'docs/flipctl-design.md', 'docs/specs/AUDIT-TLS.md'):
        continue
    path = Path(name)
    text = path.read_text()
    if not pattern.search(text):
        continue
    target = os.path.relpath('DESIGN-KNOBS.md', path.parent)
    merge = os.path.relpath('MERGE.md', path.parent)
    notice = ('> Historical research record: configuration recipes and measurements below describe earlier\\n'
              '> revisions. Deleted controls are not accepted by the merged server, and the manual-map\\n'
              '> dispatch-scaling instrument is retired. For current controls and merge dispositions, see\\n'
              '> [DESIGN-KNOBS.md](' + target + ') and [MERGE.md](' + merge + ').\\n\\n')
    lines = text.splitlines(keepends=True)
    assert lines[0].startswith('#'), name
    path.write_text(lines[0] + '\\n' + notice + ''.join(lines[1:]).lstrip('\\n'))
    changed.append(name)
Path('.merge-inputs/historical-docs.json').write_text(json.dumps(changed, indent=2) + '\\n')
print('Marked historical configuration evidence in', len(changed), 'documents:')
print('\\n'.join(changed))
PY
rg -n -g '*.sh' -g '*.py' -g '*.cc' -g '*.h' 'read-local-prefetch-capture|read-local-atomic-filter|read-local-interleave|flip-auto-band|l3-domains|smt-mode|genthread-schedule|atomic-window|persist-io|lru-clock-shift|script-crossshard-(max-bytes|workbench-bytes|conflict-retries|cut-slots)|tls-ktls|key-lb|client-lb|lb-(sample-rate|age-sample-rate|tick-ms|imbalance-pct|move-cap|cooldown-ms)|--ex-sched|--overlap|thread-pipeline' src tests scratch scratchpad" in /home/user/Projects/cx-merge
 succeeded in 0ms:
Marked historical configuration evidence in 22 documents:
AUDIT-CLEANUP.md
AUDIT-LOCALFUSE.md
AUDIT-TESTS.md
AUDIT-resp3push.md
DESIGN-GATEHYGIENE.md
DESIGN-P0REPLY.md
DESIGN-W-RLIDEAS.md
DESIGN-flipfp.md
NOTES-AOFFRAME.md
NOTES-AOFSCRIPT.md
NOTES-ATOMICS.md
NOTES-CAPTURE-PREFETCH.md
NOTES-EPOLL.md
NOTES-HYGIENE.md
NOTES-KTLS.md
NOTES-MIGRATE.md
NOTES-PERSIST-IO.md
NOTES-SERVERTAIL.md
NOTES-SNAPCUT.md
NOTES-XSCRIPT.md
P128.md
scratch/NOTES.md
tests/knobs.py:10:    "read-local-prefetch-capture", "read-local-atomic-filter", "read-local-interleave",
tests/knobs.py:11:    "flip-auto-band", "shard-home", "l3-domains", "smt-mode", "genthread-schedule",
tests/knobs.py:12:    "atomic-window", "persist-io", "lru-clock-shift", "script-crossshard-max-bytes",
tests/knobs.py:13:    "script-crossshard-workbench-bytes", "script-crossshard-conflict-retries",
tests/knobs.py:14:    "script-crossshard-cut-slots", "tls-ktls", "key-lb", "client-lb",
tests/knobs.py:15:    "lb-sample-rate", "lb-age-sample-rate", "lb-tick-ms", "lb-imbalance-pct",
tests/knobs.py:16:    "lb-move-cap", "lb-cooldown-ms", "ex-sched", "overlap", "thread-pipeline",
src/core/ex_loop.h:1856:            // key-lb half, 2/6). Rebinding is a handful of pointer stores and runs only on the
scratch/report.py:251:    "tests/flipctl.py (the historical gate invocation: --ratio 6:2 --atomic 0 --flip-auto 1 --flip-auto-band 2 "
scratch/report.py:252:    "--lb-age-sample-rate 1024, 8 server threads on cpus 52-55 + siblings)",
scratch/report.py:344:<p><b>Fix:</b> the Measuring phase never moves the split, so the readings the model already takes while deciding <i>are</i> readings of the origin. Bracket them and floor every band of the maneuver at twice that spread — the same 2× observed-jitter convention as the signature band, the rate band and <code>band_</code>. On the gate's ramping driver the floor becomes 40%, the ramp's 22.5% confirms nothing, the seek reverts to 6:2 and anchors off-rail. On a still baseline the floor is ≈0 and nothing changes. In the measured historical build, the floor also applied under the now-removed <code>--flip-auto-band</code>: that knob says how small a gain is worth chasing, not how still the workload is holding.</p>
scratch/report.py:366:D  HISTORICAL GATE ROW         tests/gate.sh (or just: --ratio 6:2 --atomic 0 --flip-auto 1 --flip-auto-band 2
scratch/report.py:367:                    --lb-age-sample-rate 1024 + tests/flipctl.py --stable-seconds 30) -- this was the row the
scratch/report2.py:281:    "tests/flipctl.py (the historical gate invocation: --ratio 6:2 --atomic 0 --flip-auto 1 --flip-auto-band 2 --lb-age-sample-rate 1024, 8 server threads on cpus 52-55 + siblings)",
scratch/report2.py:385:E  HISTORICAL GATE ROW         --ratio 6:2 --atomic 0 --flip-auto 1 --flip-auto-band 2 --lb-age-sample-rate 1024 + tests/flipctl.py --stable-seconds 30
tests/tls.py:351:            b"tls-ktls" in config_values):
tests/bplus.py:540:        if discovery.command("CONFIG", "GET", "read-local-atomic-filter") != []:
tests/config_parser_test.cc:227:        !rejects({"--genthread-schedule", "streams0"}))
tests/config_parser_test.cc:344:        {"--read-local-prefetch-capture", "1"}, {"--read-local-atomic-filter", "1"},
tests/config_parser_test.cc:345:        {"--read-local-interleave", "1"}, {"--flip-auto-band", "-1"},
tests/config_parser_test.cc:346:        {"--shard-home", "0:1"}, {"--l3-domains", "0-7"}, {"--smt-mode", "0"},
tests/config_parser_test.cc:347:        {"--genthread-schedule", "coarse"}, {"--atomic-window", "-1"},
tests/config_parser_test.cc:348:        {"--persist-io", "uring"}, {"--lru-clock-shift", "8"},
tests/config_parser_test.cc:349:        {"--script-crossshard-max-bytes", "-1"},
tests/config_parser_test.cc:350:        {"--script-crossshard-workbench-bytes", "-1"},
tests/config_parser_test.cc:351:        {"--script-crossshard-conflict-retries", "-1"},
tests/config_parser_test.cc:352:        {"--script-crossshard-cut-slots", "-1"}, {"--tls-ktls", "yes"},
tests/config_parser_test.cc:353:        {"--key-lb", "1"}, {"--client-lb", "1"}, {"--lb-sample-rate", "64"},
tests/config_parser_test.cc:354:        {"--lb-age-sample-rate", "0"}, {"--lb-tick-ms", "1000"},
tests/config_parser_test.cc:355:        {"--lb-imbalance-pct", "25"}, {"--lb-move-cap", "1"},
tests/config_parser_test.cc:356:        {"--lb-cooldown-ms", "5000"}, {"--ex-sched", "0"},
tests/config_parser_test.cc:357:        {"--overlap", "0"}, {"--thread-pipeline", "0"},
tests/atomicwindow.py:216:                print("  note atomic-window " + detail, flush=True)
tests/atomic_torn.py:905:# Resizing was removed with atomic-window. CONFIG SET atomic 1 still rebuilds credit generations;
tests/gate.sh:58:# 172 -> 178: snapcut added two cross-shard snapshot-cut rows per persist-io engine, and execfix
tests/gate.sh:64:# 192 -> 194: the AOF frame-order battery, on persist-io normal under both atomic modes.

diff --git a/AUDIT-TESTS.md b/AUDIT-TESTS.md
index e9cd43ffbc376172717531d0037de1c79fd23690..fe1fd0e2c39e965f605e4a30894a545129541534
--- a/AUDIT-TESTS.md
+++ b/AUDIT-TESTS.md
@@ -122,7 +122,7 @@
 | MM floor (memtier 20 s, sampled 8+6) | 1 | 14 | measurement, keep |
 | saturated-flip memtier | 1 | 25+3 | measurement, keep |
 | `flipctl.py --stable-seconds 30` (+ramp/idle/surge, timeout 300) | 1 | 60-120 est. | measurement, keep |
-| `xshard_dispatch_scale.sh` (2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 | 90-150 est. | measurement, keep |
+| Retired `xshard_dispatch_scale.sh` (historical 2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 historically, 0 now | 90-150 est. historically | manual ownership instrument removed; see `MERGE.md` |
 | `atomic_torn.py` hammers (3+2.5+2+2.5 s) + OFF re-rolls (up to 14 s) + RENAMENX/COPY parks (2 x 64 x 100 ms = 12.8 s) + window/lease/churn (~6 s) | 1 quick, +1 ASAN | ~45-60 each | re-rolls are dead time on this kernel (F6) |
 | `spinprobe.py` (6 s baseline + 6 s probe) | 3 | 37.5 | measurement, keep |
 | Python batteries' literal sleeps (census: flipctl 17 s, limits 5.8, edgetime 4.5, atomic_ryow 4, hexpire 3.4 — most run x2) | | ~60 | keep |
diff --git a/DESIGN-GATEHYGIENE.md b/DESIGN-GATEHYGIENE.md
index 5ea9df93e4548fb73a7e78b38debd52d876ad658..e6ac34a10326fe8880eaa664690832da3875b9b8
--- a/DESIGN-GATEHYGIENE.md
+++ b/DESIGN-GATEHYGIENE.md
@@ -459,7 +459,21 @@
 
 ---
 
-## 10. Follow-up diff on `78c3e5391` — 2026-09-07, runtime validation pending
+## 10. Incoming timing lane on `78c3e5391` — 2026-09-07
+
+**Merged disposition:** the maintainer reports the incoming timing lane gated 344/344 with no
+SKIP rows. The account below records that lane's original implementation and offline validation;
+it is not a validation claim for the merged tree. Its dispatch wrapper/helper and timing evaluator
+are retired: repeatability checks did not replace the deleted manual ownership instrument. The
+configurable-window shrink samples and shrink/reclaim budgets are superseded by the derived-window
+held-burst liveness and old-generation reconfiguration witnesses in `tests/atomicwindow.py`.
+All those mechanism, exact-reply, bound, and credit-reclamation checks remain mandatory on every
+build. The original OFF RENAME four-roll hard-failure control is unchanged, and the 1.5-second
+promotion budget still requires `--release-build`. The merged ledger requires quick 325/full 342;
+the constants remain untouched. [MERGE.md](MERGE.md) records the complete conflict and row accounting.
+
+The rest of this section is historical lane evidence, including its now-superseded dispatch,
+shrink, admission-calibration, and ledger statements.
 
 This is a test-only diff in `cx-timing`. No server, benchmark, gate, or compiler was run. The
 earlier acceptance results above describe earlier changes, **not this diff**. The worked fixes
diff --git a/DESIGN-flipfp.md b/DESIGN-flipfp.md
index 32eb6f46c24fb688db6accc79537fbd664bd084c..f705b25837c72a011da71df06dec09d2757f05ae
--- a/DESIGN-flipfp.md
+++ b/DESIGN-flipfp.md
@@ -311,7 +311,9 @@
 detector. The signal and the floor cannot be read from one run -- with a learned band the trigger
 fires and `reset()` wipes the distance -- so each arm is run twice: `--flip-auto-band 90` holds the
 anchor and lets the distance converge (the signal), `--flip-auto-band -1` learns the floor.
-`scratch/flipfp/fpmargin.sh`.
+The former `scratch/flipfp/fpmargin.sh` implemented this two-band experiment. It is retired in the
+merged tree because neither pinning the band nor disabling its retriggers has a public control.
+These historical measurements remain evidence only for their recorded builds; see `MERGE.md`.
 
 **BITCOUNT -> INCR** (the redesign lane's reference change), 2:2 split, `--rate-limiting` pinned:
 
diff --git a/DESIGN.md b/DESIGN.md
index 0fba740d94ba1915868410de40bcb685ae675833..7ad3ac9fef512d8549eadfce44036b8a6a181e9f
--- a/DESIGN.md
+++ b/DESIGN.md
@@ -1,6 +1,6 @@
 # Thread amortization study surface
 
-`thread-mode` and `overlap` are one boot-latched measurement surface. The documented mode
+`thread-mode` and the study-only `x-overlap` are one boot-latched measurement surface. The documented mode
 names are `2s` (separate IO and executor threads, the default) and `1s` (one generalized thread owns
 both roles). `split` and `fused` remain parser aliases so old invocations keep working.
 
@@ -9,10 +9,10 @@
 | `2s` | ordinary split IO/ex loops | exact `t-iopipe` WB/IFID batch schedule | rejected at config validation |
 | `1s` | coarse IFID → EX → WB rotation | exact `t-genthread` `iofused` schedule | gated `iofused` three-way schedule |
 
-The compatibility options `thread-pipeline` and `genthread-schedule` remain accepted: the former
-is a numeric alias for `overlap`, while the latter maps `coarse`, `iofused`, and the legacy
-`streams` name to `1s` plus overlap 0, 1, and 2 respectively. The legacy name is only a parser
-alias; overlap 2 no longer runs the streams implementation. Overlap 2 emits a boot warning because
+Use `--x-overlap 0|1|2` for these study schedules and `--x-ex-sched 0|1` for the executor study.
+The old public schedule spellings and compatibility aliases are removed; see
+[DESIGN-KNOBS.md](DESIGN-KNOBS.md). Below, "overlap" names the internal schedule value.
+Overlap 2 emits a boot warning because
 it is an experimental research schedule. Unified overlap 1 and 2 require `net-io uring`; the split
 iopipe schedule retains its explicit epoll specialization. No schedule constant is runtime-tunable.
 
diff --git a/NOTES-HYGIENE.md b/NOTES-HYGIENE.md
index 23506bce64f00cb0dccaaff93362ccd30deab229..f224db07f4ceb2e4791c7cbb471b87d83a0a3e93
--- a/NOTES-HYGIENE.md
+++ b/NOTES-HYGIENE.md
@@ -352,7 +352,8 @@
 ## Test-file census
 
 The calibration examples expireindex.py, borrow_registry.py, and xshard_dispatch_scale.sh were
-already wired in the current parent. aof_frames.py is indirectly executed by aof_frame_order.py;
+already wired in this note's historical parent. The merged tree retires the dispatch instrument;
+the two surviving batteries use `--release-build`. aof_frames.py is indirectly executed by aof_frame_order.py;
 aof_rewrite.py and aof_rewrite_triggers.py are driven by matrix wrappers; differ.py is driven by
 differ_gate.sh; niclib.sh and gate_refs.txt support optional full-tier NIC rows.
 
@@ -456,7 +457,7 @@
 notify: ok (notify_events_fired=1608)
 ~~~
 
-Purpose-boot matrix commands:
+Historical purpose-boot matrix commands (superseded by the current gate's network-engine grammar):
 
 ~~~sh
 PERSIST_IO=normal GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_matrix.sh
@@ -465,7 +466,8 @@
 PERSIST_IO=uring  GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_trigger_matrix.sh
 ~~~
 
-Other purpose-booted battery invocations (each boot used the knobs documented by gate.sh):
+Historical purpose-booted battery invocations (not recipes for the merged tree; the dispatch
+instrument and script OFF arm below are retired):
 
 ~~~sh
 python3 tests/xscript.py 127.0.0.1 7600 off|limit|window
diff --git a/NOTES-XPERF2.md b/NOTES-XPERF2.md
index d4ce7950b1396997d44f0945f6f5179aa625e59e..d1edc822a5e7abc68e6aefc3d5a55986b0bf178e
--- a/NOTES-XPERF2.md
+++ b/NOTES-XPERF2.md
@@ -8,7 +8,7 @@
 |---|---|---|---|---|
 | E4 | ExpireIndex never shrinks; memsets the whole sidecar on live→0; single-pass rehash | **REPRODUCED** (both halves) | yes | `tests/expireindex.py` |
 | E5 | zero-copy borrow registry is a linear vector scanned per borrow/release/retire | **REPRODUCED** | yes | `tests/borrow_registry.py` |
-| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes | `tests/xshard_dispatch_scale.sh` |
+| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes, historically | retired manual-map dispatch instrument; see `MERGE.md` |
 | E7 | segment mode is absorbing: once triggered every later reply is a malloc + iovec slot | **NOT REPRODUCED** | no | none |
 
 Rig: EPYC 9754, cores 48–63 only, ports 7200–7209 only, loopback. Oracle = vanilla redis 7.4 at
@@ -292,18 +292,20 @@
 |---|---|---|---|---|
 | `tests/expireindex.py <host> <port>` | `--shards 1 --ratio 1:1 --enable-debug-command yes` | growth-trigger cost ratio over a 16× population ≤ 8.0; live→0 DEL ≤ 1.25× a neighbouring DEL | FAIL 2 (16.30×, 2.00×) | PASS (1.65×, 1.00×) |
 | `tests/borrow_registry.py <host> <port>` | `--shards 1 --zc-min 64 --client-output-buffer-limit normal 0 0 0 --enable-debug-command yes` | borrowed-GET per-op cost growth over 0 → ~2000 live borrows ≤ 1.05 | FAIL (1.078, 1.079) | PASS (1.008, 1.008, 1.009) |
-| `tests/xshard_dispatch_scale.sh` (+ `.py`) | boots its own 4-thread and 128-thread arms | dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
+| Retired `tests/xshard_dispatch_scale.sh` (+ `.py`) | historically booted fixed-owner 4-thread and 128-thread arms | historical dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
 
 Each also carries its own non-vacuity checks: `expireindex.py` asserts INFO `expires` really
 reached the population under test and that INFO `expired_keys` MOVED (so the sampler is proven to
 still visit deadlines parked mid-migration), plus a no-TTL negative control on both timing legs;
 `borrow_registry.py` asserts `CLIENT LIST oll` grew (the borrow path is proven live, not assumed),
 carries a non-borrowed control arm, and refuses to run unless the zc gate straddles its two probe
-sizes; `xshard_dispatch_scale.py` refuses to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` prove
+sizes; the retired `xshard_dispatch_scale.py` refused to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` proved
 the cross pair really fanned out to two owner threads and the control stayed on one.
 
-None of the three is wired into `tests/gate.sh`: they each need their own boot geometry and the
-gate's port/cores belong to the mainline operator. Run them as above.
+These were standalone instruments when measured. The merged gate runs the expiry and borrow
+batteries with `--release-build`. The dispatch wrapper and helper are retired because their manual
+ownership geometry is unavailable; the historical measurements above do not validate default
+round-robin ownership. See [MERGE.md](MERGE.md).
 
 ## Evidence
 
diff --git a/docs/flipctl-design.md b/docs/flipctl-design.md
index e6a5e31a6c05171b172746f2586a01d09fa1e84e..cd96557199b408c8b85bb42b77ca59ee6fec73ea
--- a/docs/flipctl-design.md
+++ b/docs/flipctl-design.md
@@ -8,8 +8,7 @@
 ## Ownership and footprint
 
 - `src/main.cc` is the controller cron owner. When `flip-auto=1`, the main/monitor thread calls
-  `Server::flipctl_tick()` once per existing LB tick (`lb-tick-ms`; if that mechanism is disabled,
-  the period derives from the provisioned thread count). No IO or EX loop runs controller policy.
+  `Server::flipctl_tick()` on its internal observation cadence. No IO or EX loop runs controller policy.
 - `src/core/flipctl.{h,cc}` contains the fingerprint representation, shift detector, and controller
   state machine. Controller vectors allocate only when `flip-auto=1`.
 - Each `ThreadCtx` embeds one `FlipFingerprintWriter`. It has no dynamic storage. Only the physical
@@ -46,9 +45,9 @@
 measurement quantum, avoiding the zero-anchor discontinuity where one rare admin argument would
 otherwise have distance one.
 
-With `flip-auto-band=-1`, the band is twice the maximum quiet adjacent/drift distance learned at
-the final split, floored by the four-family sampling quantum divided by observed commands. A
-numeric value is a percent. Zero disables fingerprint and collapse re-triggers. Final anchor
+The automatic band is twice the maximum quiet adjacent/drift distance learned at
+the final split, floored by the four-family sampling quantum divided by observed commands.
+It has no public override. Final anchor
 learning consumes a number of closed aggregates derived from the provisioned thread count. It is
 done after maneuver-only age sampling is disarmed, so the controller cannot detect its own signal
 cost transition as a workload change.
@@ -64,13 +63,13 @@
 falls below a threshold derived from its own change jitter and measurement quantum. Directional
 connection ramp-up therefore remains drift, while jittery stationary traffic can qualify even when
 its absolute rate never enters a quiet band. A 30-second ceiling, converted to ticks from
-`lb-tick-ms`, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
+the internal observation cadence, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
 thread per tick resets the learning/cap state, so an idle server remains in
 `awaiting-load-stability` and never pays for a maneuver it cannot use.
 
-`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. An explicit
-`lb-age-sample-rate` is used when nonzero; otherwise the rate derives from the provisioned thread
-count. IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
+`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. Its rate
+derives from observed command volume and the three-tick decision window, and adapts on completed
+traffic windows; see [DESIGN-KNOBS.md](../DESIGN-KNOBS.md). IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
 writes. IO releases its ROB-head age map when the rate returns to zero.
 
 After a stabilized baseline rate, `issue_initial_jump()` subtracts the per-thread maneuver window.
@@ -88,8 +87,8 @@
 not vetoed by idle peers. If either role has no measured work/busy quantity, measurement continues;
 the controller does not invent a workload prior.
 
-The calculation is performed in placement units (one logical CPU normally, one sibling pair under
-`smt-mode=1`) and then reported/committed in threads. The target passes `nio_eq` in the measured
+The calculation is performed in placement units (one logical CPU normally, one sibling pair when
+the allowed topology contains complete pairs) and then reported/committed in threads. The target passes `nio_eq` in the measured
 direction by `max(1, abs(nio_eq-nio_now)/2)`, clamped to one live unit of each role. Even when the
 equalized split rounds to the current split, the direction of greater measured demand supplies the
 deliberate one-unit overshoot.
@@ -143,7 +142,7 @@
 
 ## Integration and observability
 
-- `src/core/config.h` is the sole parser/default source for `flip-auto`, `flip-auto-band`, and
+- `src/core/config.h` is the sole parser/default source for `flip-auto` and
   `flip-work-window`; `src/cmd/t_server.cc` exposes them as immutable CONFIG values.
 - `tomokv.conf` documents the numeric grammar and off/auto semantics.
 - `INFO FLIPCTL` reports state/phase, anchor split/rate, bands, total/per-reason trigger counts, and
diff --git a/docs/specs/AUDIT-TLS.md b/docs/specs/AUDIT-TLS.md
index 0256e887f5d27c210d436bc219f3c7521ddad0db..87a2eaca3137a47f7c0e1567794650f57531860b
--- a/docs/specs/AUDIT-TLS.md
+++ b/docs/specs/AUDIT-TLS.md
@@ -3,7 +3,7 @@
 > **Where it landed (2026-09-04).** The feature shipped as `src/net/tls.{h,cc}` (this document
 > proposes `src/net/tls_ctx.h` + `src/net/tls_conn.h`); the test is `tests/tls.py` (no
 > `tests/gen-certs.sh`); no `tls-bio-size` knob exists. Defaults: AES-128-GCM-first cipher lists
-> and `tls-ktls yes` (`tomokv.conf`, NETWORK).
+> and automatic kTLS negotiation with userspace fallback (`tomokv.conf`, NETWORK).
 
 Governing audit. Read before writing any TLS code in `/home/user/Projects/tomokv-cpp-perthread`.
 No repository was modified in producing this.
diff --git a/scratchpad/rlbatch/lib.sh b/scratchpad/rlbatch/lib.sh
index 7bcf3e4e6030c79174bf35d9bb1b3dfb11f80c42..12184314d2aafac0dc7aa48db761a3bdec5de8c7
--- a/scratchpad/rlbatch/lib.sh
+++ b/scratchpad/rlbatch/lib.sh
@@ -17,7 +17,7 @@
 boot_srv(){ # boot_srv <binary> <logfile> [extra args...] -> SRV pid
   local bin="$1" log="$2"; shift 2
   guard_port "$PORT" || return 1
-  # EXTRA_ARGS lets a caller add boot flags (e.g. --read-local-interleave 0) without a second copy
+  # EXTRA_ARGS lets a caller add boot flags (e.g. --lb 0) without a second copy
   # of this function; word splitting is intentional.
   # shellcheck disable=SC2086
   taskset -c "$SRVCORE" "$bin" --port "$PORT" --bind 127.0.0.1 --shards 16 \
diff --git a/tests/flipctl_unit.cc b/tests/flipctl_unit.cc
index 540a6c40e9589572431075b623bc553c76ee8d85..33209258e2958771d60f86b919f6f510e9977737
--- a/tests/flipctl_unit.cc
+++ b/tests/flipctl_unit.cc
@@ -184,11 +184,11 @@
 
     // THE TYPED BAND IS A FLOOR, NOT A CEILING. The gate-hygiene lane's instrumented failure: a
     // driver holding its rate to 0.07% across 34 samples, a fingerprint distance of 0.2518 against
-    // a flat --flip-auto-band 2 (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
+    // a flat explicit band of 2% (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
     // A detector whose signature genuinely moves between adjacent quiet windows must end up with a
     // band above that movement whatever the operator typed, and must still fire on a real change.
     {
-        FlipShiftDetector typed(2, 4);   // --flip-auto-band 2
+        FlipShiftDetector typed(2, 4);   // internal explicit-band control: 2%
         // Stationary but NOISY: the read/write split alternates 35:65 / 65:35 window to window.
         // The detector smooths the signature, so the anchored distance this produces is ~0.09.
         const auto noisy = [](int k) { return quiet(k % 2 ? 65 : 35, k % 2 ? 35 : 65); };
@@ -250,8 +250,8 @@
         if (fires) fail("the learned band fired on quiet-state movement");
     }
     // A TYPED BAND UNDER THE REAL SAMPLER. The shipped configuration cannot reach this -- the
-    // controller runs the learned band -- but an operator who sets --flip-auto-band AND the sampler
-    // can, and on a tree where the typed path bypasses the floor the fingerprint lane measured 73
+    // controller runs the learned band. This internal explicit-band control preserves the earlier
+    // regression: where the typed path bypassed the floor the fingerprint lane measured 73
     // two-consecutive exceedances in 600 stationary windows at K=100 and 208 at K=60: a spurious
     // maneuver every few seconds on a load that never changed. Driven here through THEIR writer, a
     // stationary 50/50 stream at depth 32, with the floor in place: none, and a real mix change
@@ -259,7 +259,7 @@
     {
         for (uint32_t window : {60u, 100u}) {
             XorShift rng;
-            FlipShiftDetector typed(2, 8);   // --flip-auto-band 2, the operator's knob
+            FlipShiftDetector typed(2, 8);   // internal explicit-band control: 2%
             int fires = 0, confirmed = 0, streak = 0;
             for (int w = 0; w < 60; w++) {
                 FlipFingerprintWriter writer;
diff --git a/tests/rlcache_churn.py b/tests/rlcache_churn.py
index b56ae4041ae0843dc67443f16017270063853124..f8a2c0730bb0a4fb6c3b5f6282c547226bb76599
--- a/tests/rlcache_churn.py
+++ b/tests/rlcache_churn.py
@@ -59,7 +59,7 @@
 
 
 # THE LOAD BALANCER MUST HAVE SOMETHING TO BALANCE. A uniform key distribution over one small key
-# space keeps every shard at the same op rate, the imbalance never crosses lb-imbalance-pct, and no
+# space keeps every shard at the same op rate, the imbalance never crosses the learned band, and no
 # shard ever changes owner -- which is the precondition for the defect this battery exists for
 # (measured: 7.8M ops, 0 migrations). A hot window that ROTATES makes a few shards hot, then makes
 # different ones hot, so the balancer moves shards continuously and in both directions, which is

exec
/bin/bash -lc "python3 - <<'PY'
from pathlib import Path
import os, json
changed = json.loads(Path('.merge-inputs/historical-docs.json').read_text())
for name in ('NOTES-XPERF2.md', 'NOTES-BARRIERTEST.md'):
    path = Path(name)
    text = path.read_text()
    title, body = text.split('\\n', 1)
    notice = ('\\n> Historical research record: configuration recipes and measurements below describe earlier\\n'
              '> revisions. Deleted controls are not accepted by the merged server, and the manual-map\\n'
              '> dispatch-scaling instrument is retired. For current controls and merge dispositions, see\\n'
              '> [DESIGN-KNOBS.md](DESIGN-KNOBS.md) and [MERGE.md](MERGE.md).\\n\\n')
    path.write_text(title + '\\n' + notice + body.lstrip('\\n'))
    changed.append(name)
Path('.merge-inputs/historical-docs.json').write_text(json.dumps(changed, indent=2) + '\\n')
PY
rg -n 'EXPECT_QUICK|EXPECT_FULL|\\[ \"\\"'$TIER" = quick '"\\]|xscript \\"'$XS_MODE control|configuration reduction '"\\+ actual geometry|Dispatch scaling|atomic torn/window|--release-build|FEATURE_ARGS' tests/gate.sh
rg -n 'for XS|xscript \\"'$XS_MODE control|cross-shard dispatch scaling|persist-io surface|'"\\[ \"\\"'$TIER" = quick '"\\]|atomic torn/window' .merge-inputs/base/tests/gate.sh
rg -n 'prepare_captured_local_mget|kAttempts = 2|mget_generation_retries|sizeof\\(Config\\)|resolved_window =|assign_shard_homes|shard_home_\\[sid\\]' src/core/ex_loop.h src/core/config.h src/core/server.h src/core/placement.h" in /home/user/Projects/cx-merge
 succeeded in 0ms:
132:# quick tier exits at the `[ "$TIER" = quick ]` block, and all three rows are emitted after it, so
141:EXPECT_QUICK=327
142:EXPECT_FULL=344                 # full without the optional NIC row.
434:py tests/atomic_torn.py 127.0.0.1 $PORT --release-build >/tmp/gate-atomic-torn.txt 2>&1 \
435:    && ok "atomic torn/window battery" || bad "atomic torn/window battery" "see /tmp/gate-atomic-torn.txt"
455:    FEATURE_ARGS=()
456:    [ "$t" = stream ] && FEATURE_ARGS+=(--release-build)
457:    py tests/$t.py 127.0.0.1 $PORT "${FEATURE_ARGS[@]}" >/tmp/gate-$t-$AT.txt 2>&1 \
514:    FEATURE_ARGS=()
515:    [ "$t" = stream ] && FEATURE_ARGS+=(--release-build)
516:    py tests/$t.py 127.0.0.1 $PORT "${FEATURE_ARGS[@]}" >/tmp/gate-fusedarmed-$t-$AT.txt 2>&1 \
651:      || bad "xscript $XS_MODE control boot"
653:      && ok "xscript $XS_MODE control" || bad "xscript $XS_MODE control" \
664:py tests/expireindex.py 127.0.0.1 $PORT --release-build >/tmp/gate-expireindex.txt 2>&1 \
672:py tests/borrow_registry.py 127.0.0.1 $PORT --release-build >/tmp/gate-borrow.txt 2>&1 \
676:# Dispatch scaling's manual ownership geometry is retired; see MERGE.md and DESIGN-KNOBS.md.
969:    && ok "configuration reduction + actual geometry ($NET_IO, atomic $AOF_ATOMIC)" \
970:    || bad "configuration reduction + actual geometry ($NET_IO, atomic $AOF_ATOMIC)"
1246:if [ "$TIER" = quick ]; then
1247:  program_state "$EXPECT_QUICK"
1257:# Deliberately omit --release-build: coverage/safety checks remain mandatory, including the
1260:    && ok "atomic torn/window under ASAN" || bad "atomic torn/window under ASAN"
1476:program_state "$((EXPECT_FULL+NIC_CHECKED))"
132:# quick tier exits at the `[ "$TIER" = quick ]` block, and all three rows are emitted after it, so
435:    && ok "atomic torn/window battery" || bad "atomic torn/window battery" "see /tmp/gate-atomic-torn.txt"
651:for XS in "off:--script-crossshard-max-bytes 0" \
656:      || bad "xscript $XS_MODE control boot"
658:      && ok "xscript $XS_MODE control" || bad "xscript $XS_MODE control" \
686:    && ok "cross-shard dispatch scaling" || bad "cross-shard dispatch scaling" "see /tmp/gate-xds.txt"
981:    && ok "persist-io surface + immutable ($PERSIST_IO)" \
982:    || bad "persist-io surface + immutable ($PERSIST_IO)"
1258:if [ "$TIER" = quick ]; then
1270:    && ok "atomic torn/window under ASAN" || bad "atomic torn/window under ASAN"
src/core/placement.h:176:    bool assign_shard_homes(uint32_t nshards) {
src/core/placement.h:180:            shard_home_[sid] = ex_[sid % ex_.size()];
src/core/placement.h:283:    uint32_t shard_home(uint32_t sid) const { return shard_home_[sid]; }
src/core/ex_loop.h:1166:        static constexpr uint32_t kAttempts = 2;
src/core/ex_loop.h:1288:                self_->read_local_stats().mget_generation_retries++;
src/core/ex_loop.h:1294:    PreparedLocalRead prepare_captured_local_mget(Op& op) {
src/core/ex_loop.h:1295:        static constexpr uint32_t kAttempts = 2;
src/core/ex_loop.h:1432:                self_->read_local_stats().mget_generation_retries++;
src/core/ex_loop.h:1450:            if constexpr (CapturePrefetch) return prepare_captured_local_mget(op);
src/core/config.h:367:static_assert(sizeof(Config) == 528, "Config footprint changed; update the documented accounting");
src/core/server.h:250:        const uint32_t resolved_window = std::min<uint32_t>(16u * cfg.shards, 1024u);
src/core/server.h:263:        if (!placement_.assign_shard_homes(cfg.shards)) return false;

diff --git a/AUDIT-TESTS.md b/AUDIT-TESTS.md
index e9cd43ffbc376172717531d0037de1c79fd23690..fe1fd0e2c39e965f605e4a30894a545129541534
--- a/AUDIT-TESTS.md
+++ b/AUDIT-TESTS.md
@@ -122,7 +122,7 @@
 | MM floor (memtier 20 s, sampled 8+6) | 1 | 14 | measurement, keep |
 | saturated-flip memtier | 1 | 25+3 | measurement, keep |
 | `flipctl.py --stable-seconds 30` (+ramp/idle/surge, timeout 300) | 1 | 60-120 est. | measurement, keep |
-| `xshard_dispatch_scale.sh` (2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 | 90-150 est. | measurement, keep |
+| Retired `xshard_dispatch_scale.sh` (historical 2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 historically, 0 now | 90-150 est. historically | manual ownership instrument removed; see `MERGE.md` |
 | `atomic_torn.py` hammers (3+2.5+2+2.5 s) + OFF re-rolls (up to 14 s) + RENAMENX/COPY parks (2 x 64 x 100 ms = 12.8 s) + window/lease/churn (~6 s) | 1 quick, +1 ASAN | ~45-60 each | re-rolls are dead time on this kernel (F6) |
 | `spinprobe.py` (6 s baseline + 6 s probe) | 3 | 37.5 | measurement, keep |
 | Python batteries' literal sleeps (census: flipctl 17 s, limits 5.8, edgetime 4.5, atomic_ryow 4, hexpire 3.4 — most run x2) | | ~60 | keep |
diff --git a/DESIGN-GATEHYGIENE.md b/DESIGN-GATEHYGIENE.md
index 5ea9df93e4548fb73a7e78b38debd52d876ad658..e6ac34a10326fe8880eaa664690832da3875b9b8
--- a/DESIGN-GATEHYGIENE.md
+++ b/DESIGN-GATEHYGIENE.md
@@ -459,7 +459,21 @@
 
 ---
 
-## 10. Follow-up diff on `78c3e5391` — 2026-09-07, runtime validation pending
+## 10. Incoming timing lane on `78c3e5391` — 2026-09-07
+
+**Merged disposition:** the maintainer reports the incoming timing lane gated 344/344 with no
+SKIP rows. The account below records that lane's original implementation and offline validation;
+it is not a validation claim for the merged tree. Its dispatch wrapper/helper and timing evaluator
+are retired: repeatability checks did not replace the deleted manual ownership instrument. The
+configurable-window shrink samples and shrink/reclaim budgets are superseded by the derived-window
+held-burst liveness and old-generation reconfiguration witnesses in `tests/atomicwindow.py`.
+All those mechanism, exact-reply, bound, and credit-reclamation checks remain mandatory on every
+build. The original OFF RENAME four-roll hard-failure control is unchanged, and the 1.5-second
+promotion budget still requires `--release-build`. The merged ledger requires quick 325/full 342;
+the constants remain untouched. [MERGE.md](MERGE.md) records the complete conflict and row accounting.
+
+The rest of this section is historical lane evidence, including its now-superseded dispatch,
+shrink, admission-calibration, and ledger statements.
 
 This is a test-only diff in `cx-timing`. No server, benchmark, gate, or compiler was run. The
 earlier acceptance results above describe earlier changes, **not this diff**. The worked fixes
diff --git a/DESIGN-flipfp.md b/DESIGN-flipfp.md
index 32eb6f46c24fb688db6accc79537fbd664bd084c..f705b25837c72a011da71df06dec09d2757f05ae
--- a/DESIGN-flipfp.md
+++ b/DESIGN-flipfp.md
@@ -311,7 +311,9 @@
 detector. The signal and the floor cannot be read from one run -- with a learned band the trigger
 fires and `reset()` wipes the distance -- so each arm is run twice: `--flip-auto-band 90` holds the
 anchor and lets the distance converge (the signal), `--flip-auto-band -1` learns the floor.
-`scratch/flipfp/fpmargin.sh`.
+The former `scratch/flipfp/fpmargin.sh` implemented this two-band experiment. It is retired in the
+merged tree because neither pinning the band nor disabling its retriggers has a public control.
+These historical measurements remain evidence only for their recorded builds; see `MERGE.md`.
 
 **BITCOUNT -> INCR** (the redesign lane's reference change), 2:2 split, `--rate-limiting` pinned:
 
diff --git a/DESIGN.md b/DESIGN.md
index 0fba740d94ba1915868410de40bcb685ae675833..7ad3ac9fef512d8549eadfce44036b8a6a181e9f
--- a/DESIGN.md
+++ b/DESIGN.md
@@ -1,6 +1,6 @@
 # Thread amortization study surface
 
-`thread-mode` and `overlap` are one boot-latched measurement surface. The documented mode
+`thread-mode` and the study-only `x-overlap` are one boot-latched measurement surface. The documented mode
 names are `2s` (separate IO and executor threads, the default) and `1s` (one generalized thread owns
 both roles). `split` and `fused` remain parser aliases so old invocations keep working.
 
@@ -9,10 +9,10 @@
 | `2s` | ordinary split IO/ex loops | exact `t-iopipe` WB/IFID batch schedule | rejected at config validation |
 | `1s` | coarse IFID → EX → WB rotation | exact `t-genthread` `iofused` schedule | gated `iofused` three-way schedule |
 
-The compatibility options `thread-pipeline` and `genthread-schedule` remain accepted: the former
-is a numeric alias for `overlap`, while the latter maps `coarse`, `iofused`, and the legacy
-`streams` name to `1s` plus overlap 0, 1, and 2 respectively. The legacy name is only a parser
-alias; overlap 2 no longer runs the streams implementation. Overlap 2 emits a boot warning because
+Use `--x-overlap 0|1|2` for these study schedules and `--x-ex-sched 0|1` for the executor study.
+The old public schedule spellings and compatibility aliases are removed; see
+[DESIGN-KNOBS.md](DESIGN-KNOBS.md). Below, "overlap" names the internal schedule value.
+Overlap 2 emits a boot warning because
 it is an experimental research schedule. Unified overlap 1 and 2 require `net-io uring`; the split
 iopipe schedule retains its explicit epoll specialization. No schedule constant is runtime-tunable.
 
diff --git a/NOTES-HYGIENE.md b/NOTES-HYGIENE.md
index 23506bce64f00cb0dccaaff93362ccd30deab229..f224db07f4ceb2e4791c7cbb471b87d83a0a3e93
--- a/NOTES-HYGIENE.md
+++ b/NOTES-HYGIENE.md
@@ -352,7 +352,8 @@
 ## Test-file census
 
 The calibration examples expireindex.py, borrow_registry.py, and xshard_dispatch_scale.sh were
-already wired in the current parent. aof_frames.py is indirectly executed by aof_frame_order.py;
+already wired in this note's historical parent. The merged tree retires the dispatch instrument;
+the two surviving batteries use `--release-build`. aof_frames.py is indirectly executed by aof_frame_order.py;
 aof_rewrite.py and aof_rewrite_triggers.py are driven by matrix wrappers; differ.py is driven by
 differ_gate.sh; niclib.sh and gate_refs.txt support optional full-tier NIC rows.
 
@@ -456,7 +457,7 @@
 notify: ok (notify_events_fired=1608)
 ~~~
 
-Purpose-boot matrix commands:
+Historical purpose-boot matrix commands (superseded by the current gate's network-engine grammar):
 
 ~~~sh
 PERSIST_IO=normal GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_matrix.sh
@@ -465,7 +466,8 @@
 PERSIST_IO=uring  GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_trigger_matrix.sh
 ~~~
 
-Other purpose-booted battery invocations (each boot used the knobs documented by gate.sh):
+Historical purpose-booted battery invocations (not recipes for the merged tree; the dispatch
+instrument and script OFF arm below are retired):
 
 ~~~sh
 python3 tests/xscript.py 127.0.0.1 7600 off|limit|window
diff --git a/NOTES-XPERF2.md b/NOTES-XPERF2.md
index d4ce7950b1396997d44f0945f6f5179aa625e59e..d1edc822a5e7abc68e6aefc3d5a55986b0bf178e
--- a/NOTES-XPERF2.md
+++ b/NOTES-XPERF2.md
@@ -8,7 +8,7 @@
 |---|---|---|---|---|
 | E4 | ExpireIndex never shrinks; memsets the whole sidecar on live→0; single-pass rehash | **REPRODUCED** (both halves) | yes | `tests/expireindex.py` |
 | E5 | zero-copy borrow registry is a linear vector scanned per borrow/release/retire | **REPRODUCED** | yes | `tests/borrow_registry.py` |
-| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes | `tests/xshard_dispatch_scale.sh` |
+| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes, historically | retired manual-map dispatch instrument; see `MERGE.md` |
 | E7 | segment mode is absorbing: once triggered every later reply is a malloc + iovec slot | **NOT REPRODUCED** | no | none |
 
 Rig: EPYC 9754, cores 48–63 only, ports 7200–7209 only, loopback. Oracle = vanilla redis 7.4 at
@@ -292,18 +292,20 @@
 |---|---|---|---|---|
 | `tests/expireindex.py <host> <port>` | `--shards 1 --ratio 1:1 --enable-debug-command yes` | growth-trigger cost ratio over a 16× population ≤ 8.0; live→0 DEL ≤ 1.25× a neighbouring DEL | FAIL 2 (16.30×, 2.00×) | PASS (1.65×, 1.00×) |
 | `tests/borrow_registry.py <host> <port>` | `--shards 1 --zc-min 64 --client-output-buffer-limit normal 0 0 0 --enable-debug-command yes` | borrowed-GET per-op cost growth over 0 → ~2000 live borrows ≤ 1.05 | FAIL (1.078, 1.079) | PASS (1.008, 1.008, 1.009) |
-| `tests/xshard_dispatch_scale.sh` (+ `.py`) | boots its own 4-thread and 128-thread arms | dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
+| Retired `tests/xshard_dispatch_scale.sh` (+ `.py`) | historically booted fixed-owner 4-thread and 128-thread arms | historical dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
 
 Each also carries its own non-vacuity checks: `expireindex.py` asserts INFO `expires` really
 reached the population under test and that INFO `expired_keys` MOVED (so the sampler is proven to
 still visit deadlines parked mid-migration), plus a no-TTL negative control on both timing legs;
 `borrow_registry.py` asserts `CLIENT LIST oll` grew (the borrow path is proven live, not assumed),
 carries a non-borrowed control arm, and refuses to run unless the zc gate straddles its two probe
-sizes; `xshard_dispatch_scale.py` refuses to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` prove
+sizes; the retired `xshard_dispatch_scale.py` refused to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` proved
 the cross pair really fanned out to two owner threads and the control stayed on one.
 
-None of the three is wired into `tests/gate.sh`: they each need their own boot geometry and the
-gate's port/cores belong to the mainline operator. Run them as above.
+These were standalone instruments when measured. The merged gate runs the expiry and borrow
+batteries with `--release-build`. The dispatch wrapper and helper are retired because their manual
+ownership geometry is unavailable; the historical measurements above do not validate default
+round-robin ownership. See [MERGE.md](MERGE.md).
 
 ## Evidence
 
diff --git a/docs/flipctl-design.md b/docs/flipctl-design.md
index e6a5e31a6c05171b172746f2586a01d09fa1e84e..cd96557199b408c8b85bb42b77ca59ee6fec73ea
--- a/docs/flipctl-design.md
+++ b/docs/flipctl-design.md
@@ -8,8 +8,7 @@
 ## Ownership and footprint
 
 - `src/main.cc` is the controller cron owner. When `flip-auto=1`, the main/monitor thread calls
-  `Server::flipctl_tick()` once per existing LB tick (`lb-tick-ms`; if that mechanism is disabled,
-  the period derives from the provisioned thread count). No IO or EX loop runs controller policy.
+  `Server::flipctl_tick()` on its internal observation cadence. No IO or EX loop runs controller policy.
 - `src/core/flipctl.{h,cc}` contains the fingerprint representation, shift detector, and controller
   state machine. Controller vectors allocate only when `flip-auto=1`.
 - Each `ThreadCtx` embeds one `FlipFingerprintWriter`. It has no dynamic storage. Only the physical
@@ -46,9 +45,9 @@
 measurement quantum, avoiding the zero-anchor discontinuity where one rare admin argument would
 otherwise have distance one.
 
-With `flip-auto-band=-1`, the band is twice the maximum quiet adjacent/drift distance learned at
-the final split, floored by the four-family sampling quantum divided by observed commands. A
-numeric value is a percent. Zero disables fingerprint and collapse re-triggers. Final anchor
+The automatic band is twice the maximum quiet adjacent/drift distance learned at
+the final split, floored by the four-family sampling quantum divided by observed commands.
+It has no public override. Final anchor
 learning consumes a number of closed aggregates derived from the provisioned thread count. It is
 done after maneuver-only age sampling is disarmed, so the controller cannot detect its own signal
 cost transition as a workload change.
@@ -64,13 +63,13 @@
 falls below a threshold derived from its own change jitter and measurement quantum. Directional
 connection ramp-up therefore remains drift, while jittery stationary traffic can qualify even when
 its absolute rate never enters a quiet band. A 30-second ceiling, converted to ticks from
-`lb-tick-ms`, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
+the internal observation cadence, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
 thread per tick resets the learning/cap state, so an idle server remains in
 `awaiting-load-stability` and never pays for a maneuver it cannot use.
 
-`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. An explicit
-`lb-age-sample-rate` is used when nonzero; otherwise the rate derives from the provisioned thread
-count. IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
+`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. Its rate
+derives from observed command volume and the three-tick decision window, and adapts on completed
+traffic windows; see [DESIGN-KNOBS.md](../DESIGN-KNOBS.md). IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
 writes. IO releases its ROB-head age map when the rate returns to zero.
 
 After a stabilized baseline rate, `issue_initial_jump()` subtracts the per-thread maneuver window.
@@ -88,8 +87,8 @@
 not vetoed by idle peers. If either role has no measured work/busy quantity, measurement continues;
 the controller does not invent a workload prior.
 
-The calculation is performed in placement units (one logical CPU normally, one sibling pair under
-`smt-mode=1`) and then reported/committed in threads. The target passes `nio_eq` in the measured
+The calculation is performed in placement units (one logical CPU normally, one sibling pair when
+the allowed topology contains complete pairs) and then reported/committed in threads. The target passes `nio_eq` in the measured
 direction by `max(1, abs(nio_eq-nio_now)/2)`, clamped to one live unit of each role. Even when the
 equalized split rounds to the current split, the direction of greater measured demand supplies the
 deliberate one-unit overshoot.
@@ -143,7 +142,7 @@
 
 ## Integration and observability
 
-- `src/core/config.h` is the sole parser/default source for `flip-auto`, `flip-auto-band`, and
+- `src/core/config.h` is the sole parser/default source for `flip-auto` and
   `flip-work-window`; `src/cmd/t_server.cc` exposes them as immutable CONFIG values.
 - `tomokv.conf` documents the numeric grammar and off/auto semantics.
 - `INFO FLIPCTL` reports state/phase, anchor split/rate, bands, total/per-reason trigger counts, and
diff --git a/docs/specs/AUDIT-TLS.md b/docs/specs/AUDIT-TLS.md
index 0256e887f5d27c210d436bc219f3c7521ddad0db..87a2eaca3137a47f7c0e1567794650f57531860b
--- a/docs/specs/AUDIT-TLS.md
+++ b/docs/specs/AUDIT-TLS.md
@@ -3,7 +3,7 @@
 > **Where it landed (2026-09-04).** The feature shipped as `src/net/tls.{h,cc}` (this document
 > proposes `src/net/tls_ctx.h` + `src/net/tls_conn.h`); the test is `tests/tls.py` (no
 > `tests/gen-certs.sh`); no `tls-bio-size` knob exists. Defaults: AES-128-GCM-first cipher lists
-> and `tls-ktls yes` (`tomokv.conf`, NETWORK).
+> and automatic kTLS negotiation with userspace fallback (`tomokv.conf`, NETWORK).
 
 Governing audit. Read before writing any TLS code in `/home/user/Projects/tomokv-cpp-perthread`.
 No repository was modified in producing this.
diff --git a/scratchpad/rlbatch/lib.sh b/scratchpad/rlbatch/lib.sh
index 7bcf3e4e6030c79174bf35d9bb1b3dfb11f80c42..12184314d2aafac0dc7aa48db761a3bdec5de8c7
--- a/scratchpad/rlbatch/lib.sh
+++ b/scratchpad/rlbatch/lib.sh
@@ -17,7 +17,7 @@
 boot_srv(){ # boot_srv <binary> <logfile> [extra args...] -> SRV pid
   local bin="$1" log="$2"; shift 2
   guard_port "$PORT" || return 1
-  # EXTRA_ARGS lets a caller add boot flags (e.g. --read-local-interleave 0) without a second copy
+  # EXTRA_ARGS lets a caller add boot flags (e.g. --lb 0) without a second copy
   # of this function; word splitting is intentional.
   # shellcheck disable=SC2086
   taskset -c "$SRVCORE" "$bin" --port "$PORT" --bind 127.0.0.1 --shards 16 \
diff --git a/tests/flipctl_unit.cc b/tests/flipctl_unit.cc
index 540a6c40e9589572431075b623bc553c76ee8d85..33209258e2958771d60f86b919f6f510e9977737
--- a/tests/flipctl_unit.cc
+++ b/tests/flipctl_unit.cc
@@ -184,11 +184,11 @@
 
     // THE TYPED BAND IS A FLOOR, NOT A CEILING. The gate-hygiene lane's instrumented failure: a
     // driver holding its rate to 0.07% across 34 samples, a fingerprint distance of 0.2518 against
-    // a flat --flip-auto-band 2 (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
+    // a flat explicit band of 2% (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
     // A detector whose signature genuinely moves between adjacent quiet windows must end up with a
     // band above that movement whatever the operator typed, and must still fire on a real change.
     {
-        FlipShiftDetector typed(2, 4);   // --flip-auto-band 2
+        FlipShiftDetector typed(2, 4);   // internal explicit-band control: 2%
         // Stationary but NOISY: the read/write split alternates 35:65 / 65:35 window to window.
         // The detector smooths the signature, so the anchored distance this produces is ~0.09.
         const auto noisy = [](int k) { return quiet(k % 2 ? 65 : 35, k % 2 ? 35 : 65); };
@@ -250,8 +250,8 @@
         if (fires) fail("the learned band fired on quiet-state movement");
     }
     // A TYPED BAND UNDER THE REAL SAMPLER. The shipped configuration cannot reach this -- the
-    // controller runs the learned band -- but an operator who sets --flip-auto-band AND the sampler
-    // can, and on a tree where the typed path bypasses the floor the fingerprint lane measured 73
+    // controller runs the learned band. This internal explicit-band control preserves the earlier
+    // regression: where the typed path bypassed the floor the fingerprint lane measured 73
     // two-consecutive exceedances in 600 stationary windows at K=100 and 208 at K=60: a spurious
     // maneuver every few seconds on a load that never changed. Driven here through THEIR writer, a
     // stationary 50/50 stream at depth 32, with the floor in place: none, and a real mix change
@@ -259,7 +259,7 @@
     {
         for (uint32_t window : {60u, 100u}) {
             XorShift rng;
-            FlipShiftDetector typed(2, 8);   // --flip-auto-band 2, the operator's knob
+            FlipShiftDetector typed(2, 8);   // internal explicit-band control: 2%
             int fires = 0, confirmed = 0, streak = 0;
             for (int w = 0; w < 60; w++) {
                 FlipFingerprintWriter writer;
diff --git a/tests/rlcache_churn.py b/tests/rlcache_churn.py
index b56ae4041ae0843dc67443f16017270063853124..f8a2c0730bb0a4fb6c3b5f6282c547226bb76599
--- a/tests/rlcache_churn.py
+++ b/tests/rlcache_churn.py
@@ -59,7 +59,7 @@
 
 
 # THE LOAD BALANCER MUST HAVE SOMETHING TO BALANCE. A uniform key distribution over one small key
-# space keeps every shard at the same op rate, the imbalance never crosses lb-imbalance-pct, and no
+# space keeps every shard at the same op rate, the imbalance never crosses the learned band, and no
 # shard ever changes owner -- which is the precondition for the defect this battery exists for
 # (measured: 7.8M ops, 0 migrations). A hot window that ROTATES makes a few shards hot, then makes
 # different ones hot, so the balancer moves shards continuously and in both directions, which is

exec
/bin/bash -lc "git diff --check && git diff --stat && rg -n '"'^(<<<<<<<|>>>>>>>|'"\\|\\|\\|\\|\\|\\|\\|)' -g '"'!.merge-inputs/**'"' -g '"'!CODEX-OUT.md'"' ." in /home/user/Projects/cx-merge
 exited 1 in 0ms:
 AUDIT-CLEANUP.md                    |   5 +
 AUDIT-LOCALFUSE.md                  |   5 +
 AUDIT-TESTS.md                      |   7 +-
 AUDIT-resp3push.md                  |   5 +
 DESIGN-GATEHYGIENE.md               |  76 ++++++++
 DESIGN-P0REPLY.md                   |   5 +
 DESIGN-W-RLIDEAS.md                 |   5 +
 DESIGN-flipfp.md                    |   9 +-
 DESIGN.md                           |  10 +-
 NOTES-AOFFRAME.md                   |   5 +
 NOTES-AOFSCRIPT.md                  |   5 +
 NOTES-ATOMICS.md                    |   5 +
 NOTES-BARRIERTEST.md                |   5 +
 NOTES-CAPTURE-PREFETCH.md           |   5 +
 NOTES-EPOLL.md                      |   5 +
 NOTES-HYGIENE.md                    |  13 +-
 NOTES-KTLS.md                       |   5 +
 NOTES-MIGRATE.md                    |   5 +
 NOTES-PERSIST-IO.md                 |   5 +
 NOTES-SERVERTAIL.md                 |   5 +
 NOTES-SNAPCUT.md                    |   5 +
 NOTES-XPERF2.md                     |  17 +-
 NOTES-XSCRIPT.md                    |   5 +
 P128.md                             |   5 +
 docs/flipctl-design.md              |  23 ++-
 docs/specs/AUDIT-AOF.md             |  10 +-
 docs/specs/AUDIT-TLS.md             |   2 +-
 docs/thread-mode.md                 | 108 +++--------
 scratch/NOTES.md                    |   5 +
 scratch/final.sh                    |   3 +-
 scratch/finalw.sh                   |   3 +-
 scratch/flipfp/bat.sh               |   3 +-
 scratch/flipfp/chain.sh             |  10 +-
 scratch/flipfp/fpmargin.sh          |  68 -------
 scratch/flipfp/fpprobe.sh           |   3 +-
 scratch/flipfp/fpshift.sh           |   3 +-
 scratch/flipfp/report.sh            |   4 +-
 scratch/red.sh                      |   3 +-
 scratch/red2.sh                     |   3 +-
 scratch/report.py                   |   8 +-
 scratch/report2.py                  |   4 +-
 scratch/rv.sh                       |   3 +-
 scratch/rv3.sh                      |   5 +-
 scratch/ver.sh                      |   3 +-
 scratchpad/replycode/batteries.sh   |   3 +-
 scratchpad/ringsize/batteries.sh    |   3 +-
 scratchpad/rlbatch/batteries.sh     |   3 +-
 scratchpad/rlbatch/lib.sh           |   2 +-
 scratchpad/robdiet/batteries.sh     |   3 +-
 src/base/topology.h                 |  42 ++++-
 src/cmd/lbsignals.cc                |   4 +-
 src/cmd/server_tail.cc              |   7 +-
 src/cmd/t_server.cc                 |  61 +-----
 src/core/config.h                   | 358 ++++++------------------------------
 src/core/ex_loop.h                  |  37 ++--
 src/core/flipctl.cc                 |  28 ++-
 src/core/io_loop.h                  |  10 +-
 src/core/iopipe_pipeline.h          |   2 +-
 src/core/placement.h                |  80 ++------
 src/core/server.h                   | 228 +++++++++++++----------
 src/core/shard.h                    |   6 +-
 src/core/weighted_lb.h              |  79 ++++++++
 src/main.cc                         |  21 +--
 src/net/tls.cc                      |   4 +-
 src/net/uring.h                     |   2 +-
 src/persist/aof.cc                  |   2 +-
 src/store/eviction.h                |   3 +
 src/store/flatstore.h               |  45 ++---
 tests/_lib.py                       |   2 +-
 tests/aof_rewrite_matrix.sh         |   8 +-
 tests/aof_rewrite_trigger_matrix.sh |   4 +-
 tests/atomic_ryow.py                |  28 ++-
 tests/atomic_torn.py                | 146 +++++----------
 tests/borrow_registry.py            |  48 ++++-
 tests/bplus.py                      |  11 +-
 tests/config_parser_test.cc         | 270 ++++++++++-----------------
 tests/differ.py                     |  64 +++++--
 tests/evict_battery.py              |  57 +++---
 tests/expireindex.py                |  40 +++-
 tests/flipctl.py                    |  34 ++--
 tests/flipctl_unit.cc               |  10 +-
 tests/gate.sh                       | 297 ++++++++++++++----------------
 tests/infofix.py                    |  49 +++--
 tests/lbsignals.py                  |   6 +-
 tests/pubsub.py                     |  18 ++
 tests/rlcache_churn.py              |  15 +-
 tests/slowlog.py                    |  23 ++-
 tests/stream.py                     |  49 ++++-
 tests/tls.py                        |   4 +-
 tests/xscript.py                    |  82 ++++-----
 tests/xshard_dispatch_scale.py      | 128 -------------
 tests/xshard_dispatch_scale.sh      | 137 --------------
 tomokv.conf                         | 168 ++++-------------
 93 files changed, 1366 insertions(+), 1861 deletions(-)

exec
/bin/bash -lc "python3 - <<'PY'
from pathlib import Path
import ast, subprocess, json, re, difflib, hashlib
root = Path.cwd()
changed = set(subprocess.check_output(['git', 'diff', '--name-only'], text=True).splitlines())
manifest = json.loads(Path('.merge-inputs/manifest.json').read_text())
changed.update(set().union(*(set(m) for m in manifest['lanes'].values())))
py = sh = 0
for name in sorted(changed):
    path = Path(name)
    if not path.exists(): continue
    if path.suffix == '.py':
        ast.parse(path.read_text(), filename=name)
        py += 1
    elif path.suffix == '.sh':
        subprocess.run(['bash', '-n', name], check=True)
        sh += 1
print('Syntax:', py, 'Python files parsed;', sh, 'shell files passed bash -n; no batteries executed.')
base = Path('.merge-inputs/base/tests/gate.sh').read_text()
merged = Path('tests/gate.sh').read_text()
for name in ('EXPECT_QUICK', 'EXPECT_FULL'):
    pattern = re.compile(r'"'^'"' + name + r'=.*"'$'"', re.M)
    assert pattern.findall(base) == pattern.findall(merged)
    print('Untouched:', pattern.findall(merged)[0])
def rows(text):
    return [(n, m.group(1)) for n, line in enumerate(text.splitlines(), 1) if not line.lstrip().startswith('#') for m in re.finditer(r'\\bok \"(["'^"]+)"'"', line)]
left = rows(base)
right = rows(merged)
normalize = lambda label: label.replace('"'$PERSIST_IO'"', '"'$NET_IO'"')
l = [normalize(label) for _, label in left if label "'!= '"'cross-shard dispatch scaling']
l = ['configuration reduction + actual geometry ("'$NET_IO, atomic $AOF_ATOMIC)'"' if label == 'persist-io surface + immutable ("'$NET_IO)'"' else label for label in l]
r = [label for _, label in right]
assert l == r, '\\n'.join(difflib.unified_diff(l, r))
assert 'for XS_MODE in limit window; do' in merged
assert base.count('\"off:--script-crossshard-max-bytes 0\"') == 1
for tag, text in [('base', base), ('merged', merged)]:
    lines = text.splitlines()
    exit_line = next(n for n, line in enumerate(lines, 1) if line == 'if [ \""'$TIER" = quick ]; then'"')
    print(tag, 'quick exit:', exit_line)
    for n, label in rows(text):
        if any(key in label for key in ('xscript "'$XS_MODE'"', 'dispatch scaling', 'persist-io surface', 'configuration reduction', 'atomic torn/window')):
            print(' ', n, 'before' if n < exit_line else 'after', label)
print('All other success-emitting labels retain order and multiplicity; only loop cardinality 3->2 and dispatch deletion reduce rows.')
for lane, files in manifest['lanes'].items():
    exact, different = [], []
    for name, digest in files.items():
        path = Path(name)
        actual = hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else None
        (exact if actual == digest else different).append(name)
    print(lane, 'exact files:', len(exact), 'deliberate differences:', different)
print('All production source files equal the knobs lane byte-for-byte:', all((Path(name).read_bytes() == (Path('.merge-inputs/knobs') / name).read_bytes()) for name in manifest['lanes']['knobs'] if name.startswith('src/')))
PY" in /home/user/Projects/cx-merge
 succeeded in 75ms:
Syntax: 21 Python files parsed; 20 shell files passed bash -n; no batteries executed.
Untouched: EXPECT_QUICK=327
Untouched: EXPECT_FULL=344                 # full without the optional NIC row.
base quick exit: 1258
  435 before atomic torn/window battery
  658 before xscript $XS_MODE control
  686 before cross-shard dispatch scaling
  981 before persist-io surface + immutable ($PERSIST_IO)
  1270 after atomic torn/window under ASAN
merged quick exit: 1246
  435 before atomic torn/window battery
  653 before xscript $XS_MODE control
  969 before configuration reduction + actual geometry ($NET_IO, atomic $AOF_ATOMIC)
  1260 after atomic torn/window under ASAN
All other success-emitting labels retain order and multiplicity; only loop cardinality 3->2 and dispatch deletion reduce rows.
timing exact files: 7 deliberate differences: ['DESIGN-GATEHYGIENE.md', 'tests/atomic_torn.py', 'tests/gate.sh', 'tests/xshard_dispatch_scale.py', 'tests/xshard_dispatch_scale.sh']
knobs exact files: 37 deliberate differences: ['tests/atomic_torn.py', 'tests/gate.sh', 'tests/rlcache_churn.py']
All production source files equal the knobs lane byte-for-byte: True

diff --git a/AUDIT-TESTS.md b/AUDIT-TESTS.md
index e9cd43ffbc376172717531d0037de1c79fd23690..fe1fd0e2c39e965f605e4a30894a545129541534
--- a/AUDIT-TESTS.md
+++ b/AUDIT-TESTS.md
@@ -122,7 +122,7 @@
 | MM floor (memtier 20 s, sampled 8+6) | 1 | 14 | measurement, keep |
 | saturated-flip memtier | 1 | 25+3 | measurement, keep |
 | `flipctl.py --stable-seconds 30` (+ramp/idle/surge, timeout 300) | 1 | 60-120 est. | measurement, keep |
-| `xshard_dispatch_scale.sh` (2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 | 90-150 est. | measurement, keep |
+| Retired `xshard_dispatch_scale.sh` (historical 2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 historically, 0 now | 90-150 est. historically | manual ownership instrument removed; see `MERGE.md` |
 | `atomic_torn.py` hammers (3+2.5+2+2.5 s) + OFF re-rolls (up to 14 s) + RENAMENX/COPY parks (2 x 64 x 100 ms = 12.8 s) + window/lease/churn (~6 s) | 1 quick, +1 ASAN | ~45-60 each | re-rolls are dead time on this kernel (F6) |
 | `spinprobe.py` (6 s baseline + 6 s probe) | 3 | 37.5 | measurement, keep |
 | Python batteries' literal sleeps (census: flipctl 17 s, limits 5.8, edgetime 4.5, atomic_ryow 4, hexpire 3.4 — most run x2) | | ~60 | keep |
diff --git a/DESIGN-GATEHYGIENE.md b/DESIGN-GATEHYGIENE.md
index 5ea9df93e4548fb73a7e78b38debd52d876ad658..e6ac34a10326fe8880eaa664690832da3875b9b8
--- a/DESIGN-GATEHYGIENE.md
+++ b/DESIGN-GATEHYGIENE.md
@@ -459,7 +459,21 @@
 
 ---
 
-## 10. Follow-up diff on `78c3e5391` — 2026-09-07, runtime validation pending
+## 10. Incoming timing lane on `78c3e5391` — 2026-09-07
+
+**Merged disposition:** the maintainer reports the incoming timing lane gated 344/344 with no
+SKIP rows. The account below records that lane's original implementation and offline validation;
+it is not a validation claim for the merged tree. Its dispatch wrapper/helper and timing evaluator
+are retired: repeatability checks did not replace the deleted manual ownership instrument. The
+configurable-window shrink samples and shrink/reclaim budgets are superseded by the derived-window
+held-burst liveness and old-generation reconfiguration witnesses in `tests/atomicwindow.py`.
+All those mechanism, exact-reply, bound, and credit-reclamation checks remain mandatory on every
+build. The original OFF RENAME four-roll hard-failure control is unchanged, and the 1.5-second
+promotion budget still requires `--release-build`. The merged ledger requires quick 325/full 342;
+the constants remain untouched. [MERGE.md](MERGE.md) records the complete conflict and row accounting.
+
+The rest of this section is historical lane evidence, including its now-superseded dispatch,
+shrink, admission-calibration, and ledger statements.
 
 This is a test-only diff in `cx-timing`. No server, benchmark, gate, or compiler was run. The
 earlier acceptance results above describe earlier changes, **not this diff**. The worked fixes
diff --git a/DESIGN-flipfp.md b/DESIGN-flipfp.md
index 32eb6f46c24fb688db6accc79537fbd664bd084c..f705b25837c72a011da71df06dec09d2757f05ae
--- a/DESIGN-flipfp.md
+++ b/DESIGN-flipfp.md
@@ -311,7 +311,9 @@
 detector. The signal and the floor cannot be read from one run -- with a learned band the trigger
 fires and `reset()` wipes the distance -- so each arm is run twice: `--flip-auto-band 90` holds the
 anchor and lets the distance converge (the signal), `--flip-auto-band -1` learns the floor.
-`scratch/flipfp/fpmargin.sh`.
+The former `scratch/flipfp/fpmargin.sh` implemented this two-band experiment. It is retired in the
+merged tree because neither pinning the band nor disabling its retriggers has a public control.
+These historical measurements remain evidence only for their recorded builds; see `MERGE.md`.
 
 **BITCOUNT -> INCR** (the redesign lane's reference change), 2:2 split, `--rate-limiting` pinned:
 
diff --git a/DESIGN.md b/DESIGN.md
index 0fba740d94ba1915868410de40bcb685ae675833..7ad3ac9fef512d8549eadfce44036b8a6a181e9f
--- a/DESIGN.md
+++ b/DESIGN.md
@@ -1,6 +1,6 @@
 # Thread amortization study surface
 
-`thread-mode` and `overlap` are one boot-latched measurement surface. The documented mode
+`thread-mode` and the study-only `x-overlap` are one boot-latched measurement surface. The documented mode
 names are `2s` (separate IO and executor threads, the default) and `1s` (one generalized thread owns
 both roles). `split` and `fused` remain parser aliases so old invocations keep working.
 
@@ -9,10 +9,10 @@
 | `2s` | ordinary split IO/ex loops | exact `t-iopipe` WB/IFID batch schedule | rejected at config validation |
 | `1s` | coarse IFID → EX → WB rotation | exact `t-genthread` `iofused` schedule | gated `iofused` three-way schedule |
 
-The compatibility options `thread-pipeline` and `genthread-schedule` remain accepted: the former
-is a numeric alias for `overlap`, while the latter maps `coarse`, `iofused`, and the legacy
-`streams` name to `1s` plus overlap 0, 1, and 2 respectively. The legacy name is only a parser
-alias; overlap 2 no longer runs the streams implementation. Overlap 2 emits a boot warning because
+Use `--x-overlap 0|1|2` for these study schedules and `--x-ex-sched 0|1` for the executor study.
+The old public schedule spellings and compatibility aliases are removed; see
+[DESIGN-KNOBS.md](DESIGN-KNOBS.md). Below, "overlap" names the internal schedule value.
+Overlap 2 emits a boot warning because
 it is an experimental research schedule. Unified overlap 1 and 2 require `net-io uring`; the split
 iopipe schedule retains its explicit epoll specialization. No schedule constant is runtime-tunable.
 
diff --git a/NOTES-HYGIENE.md b/NOTES-HYGIENE.md
index 23506bce64f00cb0dccaaff93362ccd30deab229..f224db07f4ceb2e4791c7cbb471b87d83a0a3e93
--- a/NOTES-HYGIENE.md
+++ b/NOTES-HYGIENE.md
@@ -352,7 +352,8 @@
 ## Test-file census
 
 The calibration examples expireindex.py, borrow_registry.py, and xshard_dispatch_scale.sh were
-already wired in the current parent. aof_frames.py is indirectly executed by aof_frame_order.py;
+already wired in this note's historical parent. The merged tree retires the dispatch instrument;
+the two surviving batteries use `--release-build`. aof_frames.py is indirectly executed by aof_frame_order.py;
 aof_rewrite.py and aof_rewrite_triggers.py are driven by matrix wrappers; differ.py is driven by
 differ_gate.sh; niclib.sh and gate_refs.txt support optional full-tier NIC rows.
 
@@ -456,7 +457,7 @@
 notify: ok (notify_events_fired=1608)
 ~~~
 
-Purpose-boot matrix commands:
+Historical purpose-boot matrix commands (superseded by the current gate's network-engine grammar):
 
 ~~~sh
 PERSIST_IO=normal GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_matrix.sh
@@ -465,7 +466,8 @@
 PERSIST_IO=uring  GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_trigger_matrix.sh
 ~~~
 
-Other purpose-booted battery invocations (each boot used the knobs documented by gate.sh):
+Historical purpose-booted battery invocations (not recipes for the merged tree; the dispatch
+instrument and script OFF arm below are retired):
 
 ~~~sh
 python3 tests/xscript.py 127.0.0.1 7600 off|limit|window
diff --git a/NOTES-XPERF2.md b/NOTES-XPERF2.md
index d4ce7950b1396997d44f0945f6f5179aa625e59e..d1edc822a5e7abc68e6aefc3d5a55986b0bf178e
--- a/NOTES-XPERF2.md
+++ b/NOTES-XPERF2.md
@@ -8,7 +8,7 @@
 |---|---|---|---|---|
 | E4 | ExpireIndex never shrinks; memsets the whole sidecar on live→0; single-pass rehash | **REPRODUCED** (both halves) | yes | `tests/expireindex.py` |
 | E5 | zero-copy borrow registry is a linear vector scanned per borrow/release/retire | **REPRODUCED** | yes | `tests/borrow_registry.py` |
-| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes | `tests/xshard_dispatch_scale.sh` |
+| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes, historically | retired manual-map dispatch instrument; see `MERGE.md` |
 | E7 | segment mode is absorbing: once triggered every later reply is a malloc + iovec slot | **NOT REPRODUCED** | no | none |
 
 Rig: EPYC 9754, cores 48–63 only, ports 7200–7209 only, loopback. Oracle = vanilla redis 7.4 at
@@ -292,18 +292,20 @@
 |---|---|---|---|---|
 | `tests/expireindex.py <host> <port>` | `--shards 1 --ratio 1:1 --enable-debug-command yes` | growth-trigger cost ratio over a 16× population ≤ 8.0; live→0 DEL ≤ 1.25× a neighbouring DEL | FAIL 2 (16.30×, 2.00×) | PASS (1.65×, 1.00×) |
 | `tests/borrow_registry.py <host> <port>` | `--shards 1 --zc-min 64 --client-output-buffer-limit normal 0 0 0 --enable-debug-command yes` | borrowed-GET per-op cost growth over 0 → ~2000 live borrows ≤ 1.05 | FAIL (1.078, 1.079) | PASS (1.008, 1.008, 1.009) |
-| `tests/xshard_dispatch_scale.sh` (+ `.py`) | boots its own 4-thread and 128-thread arms | dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
+| Retired `tests/xshard_dispatch_scale.sh` (+ `.py`) | historically booted fixed-owner 4-thread and 128-thread arms | historical dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
 
 Each also carries its own non-vacuity checks: `expireindex.py` asserts INFO `expires` really
 reached the population under test and that INFO `expired_keys` MOVED (so the sampler is proven to
 still visit deadlines parked mid-migration), plus a no-TTL negative control on both timing legs;
 `borrow_registry.py` asserts `CLIENT LIST oll` grew (the borrow path is proven live, not assumed),
 carries a non-borrowed control arm, and refuses to run unless the zc gate straddles its two probe
-sizes; `xshard_dispatch_scale.py` refuses to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` prove
+sizes; the retired `xshard_dispatch_scale.py` refused to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` proved
 the cross pair really fanned out to two owner threads and the control stayed on one.
 
-None of the three is wired into `tests/gate.sh`: they each need their own boot geometry and the
-gate's port/cores belong to the mainline operator. Run them as above.
+These were standalone instruments when measured. The merged gate runs the expiry and borrow
+batteries with `--release-build`. The dispatch wrapper and helper are retired because their manual
+ownership geometry is unavailable; the historical measurements above do not validate default
+round-robin ownership. See [MERGE.md](MERGE.md).
 
 ## Evidence
 
diff --git a/docs/flipctl-design.md b/docs/flipctl-design.md
index e6a5e31a6c05171b172746f2586a01d09fa1e84e..cd96557199b408c8b85bb42b77ca59ee6fec73ea
--- a/docs/flipctl-design.md
+++ b/docs/flipctl-design.md
@@ -8,8 +8,7 @@
 ## Ownership and footprint
 
 - `src/main.cc` is the controller cron owner. When `flip-auto=1`, the main/monitor thread calls
-  `Server::flipctl_tick()` once per existing LB tick (`lb-tick-ms`; if that mechanism is disabled,
-  the period derives from the provisioned thread count). No IO or EX loop runs controller policy.
+  `Server::flipctl_tick()` on its internal observation cadence. No IO or EX loop runs controller policy.
 - `src/core/flipctl.{h,cc}` contains the fingerprint representation, shift detector, and controller
   state machine. Controller vectors allocate only when `flip-auto=1`.
 - Each `ThreadCtx` embeds one `FlipFingerprintWriter`. It has no dynamic storage. Only the physical
@@ -46,9 +45,9 @@
 measurement quantum, avoiding the zero-anchor discontinuity where one rare admin argument would
 otherwise have distance one.
 
-With `flip-auto-band=-1`, the band is twice the maximum quiet adjacent/drift distance learned at
-the final split, floored by the four-family sampling quantum divided by observed commands. A
-numeric value is a percent. Zero disables fingerprint and collapse re-triggers. Final anchor
+The automatic band is twice the maximum quiet adjacent/drift distance learned at
+the final split, floored by the four-family sampling quantum divided by observed commands.
+It has no public override. Final anchor
 learning consumes a number of closed aggregates derived from the provisioned thread count. It is
 done after maneuver-only age sampling is disarmed, so the controller cannot detect its own signal
 cost transition as a workload change.
@@ -64,13 +63,13 @@
 falls below a threshold derived from its own change jitter and measurement quantum. Directional
 connection ramp-up therefore remains drift, while jittery stationary traffic can qualify even when
 its absolute rate never enters a quiet band. A 30-second ceiling, converted to ticks from
-`lb-tick-ms`, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
+the internal observation cadence, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
 thread per tick resets the learning/cap state, so an idle server remains in
 `awaiting-load-stability` and never pays for a maneuver it cannot use.
 
-`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. An explicit
-`lb-age-sample-rate` is used when nonzero; otherwise the rate derives from the provisioned thread
-count. IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
+`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. Its rate
+derives from observed command volume and the three-tick decision window, and adapts on completed
+traffic windows; see [DESIGN-KNOBS.md](../DESIGN-KNOBS.md). IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
 writes. IO releases its ROB-head age map when the rate returns to zero.
 
 After a stabilized baseline rate, `issue_initial_jump()` subtracts the per-thread maneuver window.
@@ -88,8 +87,8 @@
 not vetoed by idle peers. If either role has no measured work/busy quantity, measurement continues;
 the controller does not invent a workload prior.
 
-The calculation is performed in placement units (one logical CPU normally, one sibling pair under
-`smt-mode=1`) and then reported/committed in threads. The target passes `nio_eq` in the measured
+The calculation is performed in placement units (one logical CPU normally, one sibling pair when
+the allowed topology contains complete pairs) and then reported/committed in threads. The target passes `nio_eq` in the measured
 direction by `max(1, abs(nio_eq-nio_now)/2)`, clamped to one live unit of each role. Even when the
 equalized split rounds to the current split, the direction of greater measured demand supplies the
 deliberate one-unit overshoot.
@@ -143,7 +142,7 @@
 
 ## Integration and observability
 
-- `src/core/config.h` is the sole parser/default source for `flip-auto`, `flip-auto-band`, and
+- `src/core/config.h` is the sole parser/default source for `flip-auto` and
   `flip-work-window`; `src/cmd/t_server.cc` exposes them as immutable CONFIG values.
 - `tomokv.conf` documents the numeric grammar and off/auto semantics.
 - `INFO FLIPCTL` reports state/phase, anchor split/rate, bands, total/per-reason trigger counts, and
diff --git a/docs/specs/AUDIT-TLS.md b/docs/specs/AUDIT-TLS.md
index 0256e887f5d27c210d436bc219f3c7521ddad0db..87a2eaca3137a47f7c0e1567794650f57531860b
--- a/docs/specs/AUDIT-TLS.md
+++ b/docs/specs/AUDIT-TLS.md
@@ -3,7 +3,7 @@
 > **Where it landed (2026-09-04).** The feature shipped as `src/net/tls.{h,cc}` (this document
 > proposes `src/net/tls_ctx.h` + `src/net/tls_conn.h`); the test is `tests/tls.py` (no
 > `tests/gen-certs.sh`); no `tls-bio-size` knob exists. Defaults: AES-128-GCM-first cipher lists
-> and `tls-ktls yes` (`tomokv.conf`, NETWORK).
+> and automatic kTLS negotiation with userspace fallback (`tomokv.conf`, NETWORK).
 
 Governing audit. Read before writing any TLS code in `/home/user/Projects/tomokv-cpp-perthread`.
 No repository was modified in producing this.
diff --git a/scratchpad/rlbatch/lib.sh b/scratchpad/rlbatch/lib.sh
index 7bcf3e4e6030c79174bf35d9bb1b3dfb11f80c42..12184314d2aafac0dc7aa48db761a3bdec5de8c7
--- a/scratchpad/rlbatch/lib.sh
+++ b/scratchpad/rlbatch/lib.sh
@@ -17,7 +17,7 @@
 boot_srv(){ # boot_srv <binary> <logfile> [extra args...] -> SRV pid
   local bin="$1" log="$2"; shift 2
   guard_port "$PORT" || return 1
-  # EXTRA_ARGS lets a caller add boot flags (e.g. --read-local-interleave 0) without a second copy
+  # EXTRA_ARGS lets a caller add boot flags (e.g. --lb 0) without a second copy
   # of this function; word splitting is intentional.
   # shellcheck disable=SC2086
   taskset -c "$SRVCORE" "$bin" --port "$PORT" --bind 127.0.0.1 --shards 16 \
diff --git a/tests/flipctl_unit.cc b/tests/flipctl_unit.cc
index 540a6c40e9589572431075b623bc553c76ee8d85..33209258e2958771d60f86b919f6f510e9977737
--- a/tests/flipctl_unit.cc
+++ b/tests/flipctl_unit.cc
@@ -184,11 +184,11 @@
 
     // THE TYPED BAND IS A FLOOR, NOT A CEILING. The gate-hygiene lane's instrumented failure: a
     // driver holding its rate to 0.07% across 34 samples, a fingerprint distance of 0.2518 against
-    // a flat --flip-auto-band 2 (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
+    // a flat explicit band of 2% (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
     // A detector whose signature genuinely moves between adjacent quiet windows must end up with a
     // band above that movement whatever the operator typed, and must still fire on a real change.
     {
-        FlipShiftDetector typed(2, 4);   // --flip-auto-band 2
+        FlipShiftDetector typed(2, 4);   // internal explicit-band control: 2%
         // Stationary but NOISY: the read/write split alternates 35:65 / 65:35 window to window.
         // The detector smooths the signature, so the anchored distance this produces is ~0.09.
         const auto noisy = [](int k) { return quiet(k % 2 ? 65 : 35, k % 2 ? 35 : 65); };
@@ -250,8 +250,8 @@
         if (fires) fail("the learned band fired on quiet-state movement");
     }
     // A TYPED BAND UNDER THE REAL SAMPLER. The shipped configuration cannot reach this -- the
-    // controller runs the learned band -- but an operator who sets --flip-auto-band AND the sampler
-    // can, and on a tree where the typed path bypasses the floor the fingerprint lane measured 73
+    // controller runs the learned band. This internal explicit-band control preserves the earlier
+    // regression: where the typed path bypassed the floor the fingerprint lane measured 73
     // two-consecutive exceedances in 600 stationary windows at K=100 and 208 at K=60: a spurious
     // maneuver every few seconds on a load that never changed. Driven here through THEIR writer, a
     // stationary 50/50 stream at depth 32, with the floor in place: none, and a real mix change
@@ -259,7 +259,7 @@
     {
         for (uint32_t window : {60u, 100u}) {
             XorShift rng;
-            FlipShiftDetector typed(2, 8);   // --flip-auto-band 2, the operator's knob
+            FlipShiftDetector typed(2, 8);   // internal explicit-band control: 2%
             int fires = 0, confirmed = 0, streak = 0;
             for (int w = 0; w < 60; w++) {
                 FlipFingerprintWriter writer;
diff --git a/tests/rlcache_churn.py b/tests/rlcache_churn.py
index b56ae4041ae0843dc67443f16017270063853124..f8a2c0730bb0a4fb6c3b5f6282c547226bb76599
--- a/tests/rlcache_churn.py
+++ b/tests/rlcache_churn.py
@@ -59,7 +59,7 @@
 
 
 # THE LOAD BALANCER MUST HAVE SOMETHING TO BALANCE. A uniform key distribution over one small key
-# space keeps every shard at the same op rate, the imbalance never crosses lb-imbalance-pct, and no
+# space keeps every shard at the same op rate, the imbalance never crosses the learned band, and no
 # shard ever changes owner -- which is the precondition for the defect this battery exists for
 # (measured: 7.8M ops, 0 migrations). A hot window that ROTATES makes a few shards hot, then makes
 # different ones hot, so the balancer moves shards continuously and in both directions, which is

exec
/bin/bash -lc "python3 - <<'PY'
from pathlib import Path
import ast, json, hashlib, subprocess, difflib
src = Path('tests/atomic_torn.py').read_text()
base = Path('.merge-inputs/base/tests/atomic_torn.py').read_text()
start = '# Broadened movers: RENAME publishes source and destination on distinct owners.'
end = '# Store-family cut consistency.'
extract = lambda text: text[text.index(start):text.index(end)]
assert extract(src) == extract(base)
section = extract(src)
scenarios = {
    'four clean misses': ([0, 0, 0, 0], False, 4),
    'first attempt hits': ([1], True, 1),
    'fourth attempt hits': ([0, 0, 0, 1], True, 4),
    'worker error stops retry': ([0, 'error'], False, 2),
    'live helper stops retry': (['stuck'], False, 1),
}
for release in (False, True):
    for label, (outcomes, passed, count) in scenarios.items():
        calls, hooks, notes, skips = [], [], [], []
        def hammer(prefix, atomic, keys, seconds):
            if atomic: return (0, 10, [], True, [])
            result = outcomes[len(calls)]
            calls.append(result)
            return (1 if result == 1 else 0, 10,
                    ['injected worker failure'] if result == 'error' else [], True,
                    ['injected live helper'] if result == 'stuck' else [])
        ns = {'RELEASE_BUILD': release, 'debug': lambda *args: hooks.append(args),
              'rename_hammer': hammer, 'mover_pair': ['a', 'b'],
              'note': lambda name, ok, extra='': notes.append((name, bool(ok))),
              'skip': lambda name, extra='': skips.append(name)}
        exec(section, ns)
        assert len(calls) == count, (release, label, calls)
        assert hooks == [('ATOMIC-OFF-HOP-DELAY', value) for _ in calls for value in (100000, 0)]
        assert dict(notes)['OFF control exposes torn RENAME'] is passed
        assert 'OFF control exposes torn RENAME' not in skips
print('OFF RENAME block equals base byte-for-byte; 10 source-extracted cases passed across both build identities.')
assert 'config(\"atomic-window\"' not in src
assert src.count('release_note(') == 2  # definition and surviving promotion budget
assert 'witness = held_burst(HOST, PORT, whole_window=True)' in src
assert 'witness = held_burst(HOST, PORT, whole_window=True, reconfigure=True)' in src
print('Derived-window callers unconditional; promotion-only release split retained.')
manifest = json.loads(Path('.merge-inputs/manifest.json').read_text())
for lane, files in manifest['lanes'].items():
    path = Path('..') / ('cx-' + lane)
    names = set(subprocess.check_output(['git', '-C', str(path), 'diff', 'HEAD', '--name-only'], text=True).splitlines())
    assert names == set(files), lane
    for name, expected in files.items():
        file = path / name
        actual = hashlib.sha256(file.read_bytes()).hexdigest() if file.exists() else None
        assert actual == expected, (lane, name)
    print(lane, 'source snapshot unchanged; patch sha256', hashlib.sha256(Path('.merge-inputs', lane + '.patch').read_bytes()).hexdigest())
PY" in /home/user/Projects/cx-merge
 succeeded in 0ms:
OFF RENAME block equals base byte-for-byte; 10 source-extracted cases passed across both build identities.
Derived-window callers unconditional; promotion-only release split retained.
timing source snapshot unchanged; patch sha256 5310bed7954cc01a061bcb5c20e18b6b9f8096dfadbef7b94fb871fcfea4d3a9
knobs source snapshot unchanged; patch sha256 cb8a21fa050e9961dd1f3bed442d5de5f3b0df1e34e03fd183a622ad73ed3084

diff --git a/AUDIT-TESTS.md b/AUDIT-TESTS.md
index e9cd43ffbc376172717531d0037de1c79fd23690..fe1fd0e2c39e965f605e4a30894a545129541534
--- a/AUDIT-TESTS.md
+++ b/AUDIT-TESTS.md
@@ -122,7 +122,7 @@
 | MM floor (memtier 20 s, sampled 8+6) | 1 | 14 | measurement, keep |
 | saturated-flip memtier | 1 | 25+3 | measurement, keep |
 | `flipctl.py --stable-seconds 30` (+ramp/idle/surge, timeout 300) | 1 | 60-120 est. | measurement, keep |
-| `xshard_dispatch_scale.sh` (2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 | 90-150 est. | measurement, keep |
+| Retired `xshard_dispatch_scale.sh` (historical 2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 historically, 0 now | 90-150 est. historically | manual ownership instrument removed; see `MERGE.md` |
 | `atomic_torn.py` hammers (3+2.5+2+2.5 s) + OFF re-rolls (up to 14 s) + RENAMENX/COPY parks (2 x 64 x 100 ms = 12.8 s) + window/lease/churn (~6 s) | 1 quick, +1 ASAN | ~45-60 each | re-rolls are dead time on this kernel (F6) |
 | `spinprobe.py` (6 s baseline + 6 s probe) | 3 | 37.5 | measurement, keep |
 | Python batteries' literal sleeps (census: flipctl 17 s, limits 5.8, edgetime 4.5, atomic_ryow 4, hexpire 3.4 — most run x2) | | ~60 | keep |
diff --git a/DESIGN-GATEHYGIENE.md b/DESIGN-GATEHYGIENE.md
index 5ea9df93e4548fb73a7e78b38debd52d876ad658..e6ac34a10326fe8880eaa664690832da3875b9b8
--- a/DESIGN-GATEHYGIENE.md
+++ b/DESIGN-GATEHYGIENE.md
@@ -459,7 +459,21 @@
 
 ---
 
-## 10. Follow-up diff on `78c3e5391` — 2026-09-07, runtime validation pending
+## 10. Incoming timing lane on `78c3e5391` — 2026-09-07
+
+**Merged disposition:** the maintainer reports the incoming timing lane gated 344/344 with no
+SKIP rows. The account below records that lane's original implementation and offline validation;
+it is not a validation claim for the merged tree. Its dispatch wrapper/helper and timing evaluator
+are retired: repeatability checks did not replace the deleted manual ownership instrument. The
+configurable-window shrink samples and shrink/reclaim budgets are superseded by the derived-window
+held-burst liveness and old-generation reconfiguration witnesses in `tests/atomicwindow.py`.
+All those mechanism, exact-reply, bound, and credit-reclamation checks remain mandatory on every
+build. The original OFF RENAME four-roll hard-failure control is unchanged, and the 1.5-second
+promotion budget still requires `--release-build`. The merged ledger requires quick 325/full 342;
+the constants remain untouched. [MERGE.md](MERGE.md) records the complete conflict and row accounting.
+
+The rest of this section is historical lane evidence, including its now-superseded dispatch,
+shrink, admission-calibration, and ledger statements.
 
 This is a test-only diff in `cx-timing`. No server, benchmark, gate, or compiler was run. The
 earlier acceptance results above describe earlier changes, **not this diff**. The worked fixes
diff --git a/DESIGN-flipfp.md b/DESIGN-flipfp.md
index 32eb6f46c24fb688db6accc79537fbd664bd084c..f705b25837c72a011da71df06dec09d2757f05ae
--- a/DESIGN-flipfp.md
+++ b/DESIGN-flipfp.md
@@ -311,7 +311,9 @@
 detector. The signal and the floor cannot be read from one run -- with a learned band the trigger
 fires and `reset()` wipes the distance -- so each arm is run twice: `--flip-auto-band 90` holds the
 anchor and lets the distance converge (the signal), `--flip-auto-band -1` learns the floor.
-`scratch/flipfp/fpmargin.sh`.
+The former `scratch/flipfp/fpmargin.sh` implemented this two-band experiment. It is retired in the
+merged tree because neither pinning the band nor disabling its retriggers has a public control.
+These historical measurements remain evidence only for their recorded builds; see `MERGE.md`.
 
 **BITCOUNT -> INCR** (the redesign lane's reference change), 2:2 split, `--rate-limiting` pinned:
 
diff --git a/DESIGN.md b/DESIGN.md
index 0fba740d94ba1915868410de40bcb685ae675833..7ad3ac9fef512d8549eadfce44036b8a6a181e9f
--- a/DESIGN.md
+++ b/DESIGN.md
@@ -1,6 +1,6 @@
 # Thread amortization study surface
 
-`thread-mode` and `overlap` are one boot-latched measurement surface. The documented mode
+`thread-mode` and the study-only `x-overlap` are one boot-latched measurement surface. The documented mode
 names are `2s` (separate IO and executor threads, the default) and `1s` (one generalized thread owns
 both roles). `split` and `fused` remain parser aliases so old invocations keep working.
 
@@ -9,10 +9,10 @@
 | `2s` | ordinary split IO/ex loops | exact `t-iopipe` WB/IFID batch schedule | rejected at config validation |
 | `1s` | coarse IFID → EX → WB rotation | exact `t-genthread` `iofused` schedule | gated `iofused` three-way schedule |
 
-The compatibility options `thread-pipeline` and `genthread-schedule` remain accepted: the former
-is a numeric alias for `overlap`, while the latter maps `coarse`, `iofused`, and the legacy
-`streams` name to `1s` plus overlap 0, 1, and 2 respectively. The legacy name is only a parser
-alias; overlap 2 no longer runs the streams implementation. Overlap 2 emits a boot warning because
+Use `--x-overlap 0|1|2` for these study schedules and `--x-ex-sched 0|1` for the executor study.
+The old public schedule spellings and compatibility aliases are removed; see
+[DESIGN-KNOBS.md](DESIGN-KNOBS.md). Below, "overlap" names the internal schedule value.
+Overlap 2 emits a boot warning because
 it is an experimental research schedule. Unified overlap 1 and 2 require `net-io uring`; the split
 iopipe schedule retains its explicit epoll specialization. No schedule constant is runtime-tunable.
 
diff --git a/NOTES-HYGIENE.md b/NOTES-HYGIENE.md
index 23506bce64f00cb0dccaaff93362ccd30deab229..f224db07f4ceb2e4791c7cbb471b87d83a0a3e93
--- a/NOTES-HYGIENE.md
+++ b/NOTES-HYGIENE.md
@@ -352,7 +352,8 @@
 ## Test-file census
 
 The calibration examples expireindex.py, borrow_registry.py, and xshard_dispatch_scale.sh were
-already wired in the current parent. aof_frames.py is indirectly executed by aof_frame_order.py;
+already wired in this note's historical parent. The merged tree retires the dispatch instrument;
+the two surviving batteries use `--release-build`. aof_frames.py is indirectly executed by aof_frame_order.py;
 aof_rewrite.py and aof_rewrite_triggers.py are driven by matrix wrappers; differ.py is driven by
 differ_gate.sh; niclib.sh and gate_refs.txt support optional full-tier NIC rows.
 
@@ -456,7 +457,7 @@
 notify: ok (notify_events_fired=1608)
 ~~~
 
-Purpose-boot matrix commands:
+Historical purpose-boot matrix commands (superseded by the current gate's network-engine grammar):
 
 ~~~sh
 PERSIST_IO=normal GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_matrix.sh
@@ -465,7 +466,8 @@
 PERSIST_IO=uring  GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_trigger_matrix.sh
 ~~~
 
-Other purpose-booted battery invocations (each boot used the knobs documented by gate.sh):
+Historical purpose-booted battery invocations (not recipes for the merged tree; the dispatch
+instrument and script OFF arm below are retired):
 
 ~~~sh
 python3 tests/xscript.py 127.0.0.1 7600 off|limit|window
diff --git a/NOTES-XPERF2.md b/NOTES-XPERF2.md
index d4ce7950b1396997d44f0945f6f5179aa625e59e..d1edc822a5e7abc68e6aefc3d5a55986b0bf178e
--- a/NOTES-XPERF2.md
+++ b/NOTES-XPERF2.md
@@ -8,7 +8,7 @@
 |---|---|---|---|---|
 | E4 | ExpireIndex never shrinks; memsets the whole sidecar on live→0; single-pass rehash | **REPRODUCED** (both halves) | yes | `tests/expireindex.py` |
 | E5 | zero-copy borrow registry is a linear vector scanned per borrow/release/retire | **REPRODUCED** | yes | `tests/borrow_registry.py` |
-| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes | `tests/xshard_dispatch_scale.sh` |
+| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes, historically | retired manual-map dispatch instrument; see `MERGE.md` |
 | E7 | segment mode is absorbing: once triggered every later reply is a malloc + iovec slot | **NOT REPRODUCED** | no | none |
 
 Rig: EPYC 9754, cores 48–63 only, ports 7200–7209 only, loopback. Oracle = vanilla redis 7.4 at
@@ -292,18 +292,20 @@
 |---|---|---|---|---|
 | `tests/expireindex.py <host> <port>` | `--shards 1 --ratio 1:1 --enable-debug-command yes` | growth-trigger cost ratio over a 16× population ≤ 8.0; live→0 DEL ≤ 1.25× a neighbouring DEL | FAIL 2 (16.30×, 2.00×) | PASS (1.65×, 1.00×) |
 | `tests/borrow_registry.py <host> <port>` | `--shards 1 --zc-min 64 --client-output-buffer-limit normal 0 0 0 --enable-debug-command yes` | borrowed-GET per-op cost growth over 0 → ~2000 live borrows ≤ 1.05 | FAIL (1.078, 1.079) | PASS (1.008, 1.008, 1.009) |
-| `tests/xshard_dispatch_scale.sh` (+ `.py`) | boots its own 4-thread and 128-thread arms | dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
+| Retired `tests/xshard_dispatch_scale.sh` (+ `.py`) | historically booted fixed-owner 4-thread and 128-thread arms | historical dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
 
 Each also carries its own non-vacuity checks: `expireindex.py` asserts INFO `expires` really
 reached the population under test and that INFO `expired_keys` MOVED (so the sampler is proven to
 still visit deadlines parked mid-migration), plus a no-TTL negative control on both timing legs;
 `borrow_registry.py` asserts `CLIENT LIST oll` grew (the borrow path is proven live, not assumed),
 carries a non-borrowed control arm, and refuses to run unless the zc gate straddles its two probe
-sizes; `xshard_dispatch_scale.py` refuses to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` prove
+sizes; the retired `xshard_dispatch_scale.py` refused to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` proved
 the cross pair really fanned out to two owner threads and the control stayed on one.
 
-None of the three is wired into `tests/gate.sh`: they each need their own boot geometry and the
-gate's port/cores belong to the mainline operator. Run them as above.
+These were standalone instruments when measured. The merged gate runs the expiry and borrow
+batteries with `--release-build`. The dispatch wrapper and helper are retired because their manual
+ownership geometry is unavailable; the historical measurements above do not validate default
+round-robin ownership. See [MERGE.md](MERGE.md).
 
 ## Evidence
 
diff --git a/docs/flipctl-design.md b/docs/flipctl-design.md
index e6a5e31a6c05171b172746f2586a01d09fa1e84e..cd96557199b408c8b85bb42b77ca59ee6fec73ea
--- a/docs/flipctl-design.md
+++ b/docs/flipctl-design.md
@@ -8,8 +8,7 @@
 ## Ownership and footprint
 
 - `src/main.cc` is the controller cron owner. When `flip-auto=1`, the main/monitor thread calls
-  `Server::flipctl_tick()` once per existing LB tick (`lb-tick-ms`; if that mechanism is disabled,
-  the period derives from the provisioned thread count). No IO or EX loop runs controller policy.
+  `Server::flipctl_tick()` on its internal observation cadence. No IO or EX loop runs controller policy.
 - `src/core/flipctl.{h,cc}` contains the fingerprint representation, shift detector, and controller
   state machine. Controller vectors allocate only when `flip-auto=1`.
 - Each `ThreadCtx` embeds one `FlipFingerprintWriter`. It has no dynamic storage. Only the physical
@@ -46,9 +45,9 @@
 measurement quantum, avoiding the zero-anchor discontinuity where one rare admin argument would
 otherwise have distance one.
 
-With `flip-auto-band=-1`, the band is twice the maximum quiet adjacent/drift distance learned at
-the final split, floored by the four-family sampling quantum divided by observed commands. A
-numeric value is a percent. Zero disables fingerprint and collapse re-triggers. Final anchor
+The automatic band is twice the maximum quiet adjacent/drift distance learned at
+the final split, floored by the four-family sampling quantum divided by observed commands.
+It has no public override. Final anchor
 learning consumes a number of closed aggregates derived from the provisioned thread count. It is
 done after maneuver-only age sampling is disarmed, so the controller cannot detect its own signal
 cost transition as a workload change.
@@ -64,13 +63,13 @@
 falls below a threshold derived from its own change jitter and measurement quantum. Directional
 connection ramp-up therefore remains drift, while jittery stationary traffic can qualify even when
 its absolute rate never enters a quiet band. A 30-second ceiling, converted to ticks from
-`lb-tick-ms`, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
+the internal observation cadence, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
 thread per tick resets the learning/cap state, so an idle server remains in
 `awaiting-load-stability` and never pays for a maneuver it cannot use.
 
-`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. An explicit
-`lb-age-sample-rate` is used when nonzero; otherwise the rate derives from the provisioned thread
-count. IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
+`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. Its rate
+derives from observed command volume and the three-tick decision window, and adapts on completed
+traffic windows; see [DESIGN-KNOBS.md](../DESIGN-KNOBS.md). IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
 writes. IO releases its ROB-head age map when the rate returns to zero.
 
 After a stabilized baseline rate, `issue_initial_jump()` subtracts the per-thread maneuver window.
@@ -88,8 +87,8 @@
 not vetoed by idle peers. If either role has no measured work/busy quantity, measurement continues;
 the controller does not invent a workload prior.
 
-The calculation is performed in placement units (one logical CPU normally, one sibling pair under
-`smt-mode=1`) and then reported/committed in threads. The target passes `nio_eq` in the measured
+The calculation is performed in placement units (one logical CPU normally, one sibling pair when
+the allowed topology contains complete pairs) and then reported/committed in threads. The target passes `nio_eq` in the measured
 direction by `max(1, abs(nio_eq-nio_now)/2)`, clamped to one live unit of each role. Even when the
 equalized split rounds to the current split, the direction of greater measured demand supplies the
 deliberate one-unit overshoot.
@@ -143,7 +142,7 @@
 
 ## Integration and observability
 
-- `src/core/config.h` is the sole parser/default source for `flip-auto`, `flip-auto-band`, and
+- `src/core/config.h` is the sole parser/default source for `flip-auto` and
   `flip-work-window`; `src/cmd/t_server.cc` exposes them as immutable CONFIG values.
 - `tomokv.conf` documents the numeric grammar and off/auto semantics.
 - `INFO FLIPCTL` reports state/phase, anchor split/rate, bands, total/per-reason trigger counts, and
diff --git a/docs/specs/AUDIT-TLS.md b/docs/specs/AUDIT-TLS.md
index 0256e887f5d27c210d436bc219f3c7521ddad0db..87a2eaca3137a47f7c0e1567794650f57531860b
--- a/docs/specs/AUDIT-TLS.md
+++ b/docs/specs/AUDIT-TLS.md
@@ -3,7 +3,7 @@
 > **Where it landed (2026-09-04).** The feature shipped as `src/net/tls.{h,cc}` (this document
 > proposes `src/net/tls_ctx.h` + `src/net/tls_conn.h`); the test is `tests/tls.py` (no
 > `tests/gen-certs.sh`); no `tls-bio-size` knob exists. Defaults: AES-128-GCM-first cipher lists
-> and `tls-ktls yes` (`tomokv.conf`, NETWORK).
+> and automatic kTLS negotiation with userspace fallback (`tomokv.conf`, NETWORK).
 
 Governing audit. Read before writing any TLS code in `/home/user/Projects/tomokv-cpp-perthread`.
 No repository was modified in producing this.
diff --git a/scratchpad/rlbatch/lib.sh b/scratchpad/rlbatch/lib.sh
index 7bcf3e4e6030c79174bf35d9bb1b3dfb11f80c42..12184314d2aafac0dc7aa48db761a3bdec5de8c7
--- a/scratchpad/rlbatch/lib.sh
+++ b/scratchpad/rlbatch/lib.sh
@@ -17,7 +17,7 @@
 boot_srv(){ # boot_srv <binary> <logfile> [extra args...] -> SRV pid
   local bin="$1" log="$2"; shift 2
   guard_port "$PORT" || return 1
-  # EXTRA_ARGS lets a caller add boot flags (e.g. --read-local-interleave 0) without a second copy
+  # EXTRA_ARGS lets a caller add boot flags (e.g. --lb 0) without a second copy
   # of this function; word splitting is intentional.
   # shellcheck disable=SC2086
   taskset -c "$SRVCORE" "$bin" --port "$PORT" --bind 127.0.0.1 --shards 16 \
diff --git a/tests/flipctl_unit.cc b/tests/flipctl_unit.cc
index 540a6c40e9589572431075b623bc553c76ee8d85..33209258e2958771d60f86b919f6f510e9977737
--- a/tests/flipctl_unit.cc
+++ b/tests/flipctl_unit.cc
@@ -184,11 +184,11 @@
 
     // THE TYPED BAND IS A FLOOR, NOT A CEILING. The gate-hygiene lane's instrumented failure: a
     // driver holding its rate to 0.07% across 34 samples, a fingerprint distance of 0.2518 against
-    // a flat --flip-auto-band 2 (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
+    // a flat explicit band of 2% (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
     // A detector whose signature genuinely moves between adjacent quiet windows must end up with a
     // band above that movement whatever the operator typed, and must still fire on a real change.
     {
-        FlipShiftDetector typed(2, 4);   // --flip-auto-band 2
+        FlipShiftDetector typed(2, 4);   // internal explicit-band control: 2%
         // Stationary but NOISY: the read/write split alternates 35:65 / 65:35 window to window.
         // The detector smooths the signature, so the anchored distance this produces is ~0.09.
         const auto noisy = [](int k) { return quiet(k % 2 ? 65 : 35, k % 2 ? 35 : 65); };
@@ -250,8 +250,8 @@
         if (fires) fail("the learned band fired on quiet-state movement");
     }
     // A TYPED BAND UNDER THE REAL SAMPLER. The shipped configuration cannot reach this -- the
-    // controller runs the learned band -- but an operator who sets --flip-auto-band AND the sampler
-    // can, and on a tree where the typed path bypasses the floor the fingerprint lane measured 73
+    // controller runs the learned band. This internal explicit-band control preserves the earlier
+    // regression: where the typed path bypassed the floor the fingerprint lane measured 73
     // two-consecutive exceedances in 600 stationary windows at K=100 and 208 at K=60: a spurious
     // maneuver every few seconds on a load that never changed. Driven here through THEIR writer, a
     // stationary 50/50 stream at depth 32, with the floor in place: none, and a real mix change
@@ -259,7 +259,7 @@
     {
         for (uint32_t window : {60u, 100u}) {
             XorShift rng;
-            FlipShiftDetector typed(2, 8);   // --flip-auto-band 2, the operator's knob
+            FlipShiftDetector typed(2, 8);   // internal explicit-band control: 2%
             int fires = 0, confirmed = 0, streak = 0;
             for (int w = 0; w < 60; w++) {
                 FlipFingerprintWriter writer;
diff --git a/tests/rlcache_churn.py b/tests/rlcache_churn.py
index b56ae4041ae0843dc67443f16017270063853124..f8a2c0730bb0a4fb6c3b5f6282c547226bb76599
--- a/tests/rlcache_churn.py
+++ b/tests/rlcache_churn.py
@@ -59,7 +59,7 @@
 
 
 # THE LOAD BALANCER MUST HAVE SOMETHING TO BALANCE. A uniform key distribution over one small key
-# space keeps every shard at the same op rate, the imbalance never crosses lb-imbalance-pct, and no
+# space keeps every shard at the same op rate, the imbalance never crosses the learned band, and no
 # shard ever changes owner -- which is the precondition for the defect this battery exists for
 # (measured: 7.8M ops, 0 migrations). A hot window that ROTATES makes a few shards hot, then makes
 # different ones hot, so the balancer moves shards continuously and in both directions, which is

codex
Syntax checks passed for the affected Python and shell files. The complete OFF RENAME control is byte-for-byte identical to the base; ten isolated checks confirmed that clean misses exhaust four attempts and fail, while worker errors and live helpers stop retries, under both build identities. Production source is byte-for-byte identical to the knobs lane, including its documented inherited MGET retry objection.
exec
/bin/bash -lc "python3 - <<'PY'
from pathlib import Path
path = Path('DESIGN-KNOBS.md')
text = path.read_text()
first, rest = text.split('\\n', 1)
text = first + '\\n\\n> Incoming knobs-lane record. The line references and validation history below describe that\\n> lane before integration; [MERGE.md](MERGE.md) records the merged conflict dispositions and\\n> verified gate-line arithmetic. The merged tree has not been built or gated.\\n' + rest
path.write_text(text)
PY
rg -n 'static_assert\\(sizeof\\((Op|Client|ThreadCtx|Shard|FlatStore|Rob<64>|AtomicEntry|Config)\\)' src
rg -n 'rename_off = None|for _roll in range\\(4\\)|OFF control exposes torn RENAME|release_note\\(\"ON promotion|held_burst\\(' tests/atomic_torn.py
rg -n 'HOMES=|shard-home|stationary =|spreads\\[|all\\(r <= limit|PAIRS=|key-lb|exact' .merge-inputs/timing/tests/xshard_dispatch_scale.sh
rg -n 'shard_owners\\(\\) "'!= owners|reply =|shard_owners|cross =|same ='"' .merge-inputs/timing/tests/xshard_dispatch_scale.py" in /home/user/Projects/cx-merge
 succeeded in 0ms:
src/core/thread.h:1191:static_assert(sizeof(ThreadCtx) == 1408,
src/core/shard.h:523:static_assert(sizeof(Shard) == 1440);
src/core/config.h:367:static_assert(sizeof(Config) == 528, "Config footprint changed; update the documented accounting");
src/net/rob.h:1098:static_assert(sizeof(Rob<64>) == 192, "Rob<64> layout changed");
src/net/conn.h:905:static_assert(sizeof(Client) == 1984, "Client grew: re-check the io-hot line packing and idle RSS");
src/exec/op.h:463:static_assert(sizeof(Op) == 336, "Op grew: pay for it inside the struct or re-run the 64c A/B");
src/store/atomic_mvcc.h:70:static_assert(sizeof(AtomicEntry) == 144);
src/store/flatstore.h:3899:static_assert(sizeof(FlatStore) == 944);
721:release_note("ON promotion drains within release budget",
770:rename_off = None
771:for _roll in range(4):
782:note("OFF control exposes torn RENAME",
801:for _roll in range(4):
900:    witness = held_burst(HOST, PORT, whole_window=True)
908:    witness = held_burst(HOST, PORT, whole_window=True, reconfigure=True)
12:#   tid2,tid3 = EX on the next two cpus        (--shard-home puts ALL 16 shards on exactly these)
62:HOMES=$(python3 -c "print(','.join('%d:%d' % (s, 2 + s % 2) for s in range(16)))")
81:      --place "$place" --shards 16 --shard-home "$HOMES" \
82:      --key-lb 0 --client-lb 0 --flip-auto 0 >/tmp/xds-$t.log 2>&1 & )
90:PAIRS=""
101:  PAIRS="$PAIRS|$SMALL_OUT;$BIG_OUT"
139:        spreads[name + "_" + metric] = max(values) / min(values) if values and min(values) > 0 else float("inf")
141:spreads["control_across_counts"] = max(controls) / min(controls) if controls and min(controls) > 0 else float("inf")
142:stationary = len(pairs) >= 2 and all(spread <= 1.02 for spread in spreads.values())
157:# second pair cannot erase it by selection. Misroute/drop a key to fail byte-exact arms on ANY
164:    check(label, all(r <= limit for r in ratios), detail)
76:    def shard_owners(self):
85:    owners = c.shard_owners()
88:    cross = same = None
94:            cross = (base % i, s)
96:            same = (base % i, s)
107:    reply = b"*2\r\n$1\r\nx\r\n$1\r\ny\r\n"
108:    fcross = enc("MGET", base % 0, cross[0])
109:    fsame = enc("MGET", base % 0, same[0])
123:    if c.shard_owners() != owners:

diff --git a/AUDIT-TESTS.md b/AUDIT-TESTS.md
index e9cd43ffbc376172717531d0037de1c79fd23690..fe1fd0e2c39e965f605e4a30894a545129541534
--- a/AUDIT-TESTS.md
+++ b/AUDIT-TESTS.md
@@ -122,7 +122,7 @@
 | MM floor (memtier 20 s, sampled 8+6) | 1 | 14 | measurement, keep |
 | saturated-flip memtier | 1 | 25+3 | measurement, keep |
 | `flipctl.py --stable-seconds 30` (+ramp/idle/surge, timeout 300) | 1 | 60-120 est. | measurement, keep |
-| `xshard_dispatch_scale.sh` (2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 | 90-150 est. | measurement, keep |
+| Retired `xshard_dispatch_scale.sh` (historical 2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 historically, 0 now | 90-150 est. historically | manual ownership instrument removed; see `MERGE.md` |
 | `atomic_torn.py` hammers (3+2.5+2+2.5 s) + OFF re-rolls (up to 14 s) + RENAMENX/COPY parks (2 x 64 x 100 ms = 12.8 s) + window/lease/churn (~6 s) | 1 quick, +1 ASAN | ~45-60 each | re-rolls are dead time on this kernel (F6) |
 | `spinprobe.py` (6 s baseline + 6 s probe) | 3 | 37.5 | measurement, keep |
 | Python batteries' literal sleeps (census: flipctl 17 s, limits 5.8, edgetime 4.5, atomic_ryow 4, hexpire 3.4 — most run x2) | | ~60 | keep |
diff --git a/DESIGN-GATEHYGIENE.md b/DESIGN-GATEHYGIENE.md
index 5ea9df93e4548fb73a7e78b38debd52d876ad658..e6ac34a10326fe8880eaa664690832da3875b9b8
--- a/DESIGN-GATEHYGIENE.md
+++ b/DESIGN-GATEHYGIENE.md
@@ -459,7 +459,21 @@
 
 ---
 
-## 10. Follow-up diff on `78c3e5391` — 2026-09-07, runtime validation pending
+## 10. Incoming timing lane on `78c3e5391` — 2026-09-07
+
+**Merged disposition:** the maintainer reports the incoming timing lane gated 344/344 with no
+SKIP rows. The account below records that lane's original implementation and offline validation;
+it is not a validation claim for the merged tree. Its dispatch wrapper/helper and timing evaluator
+are retired: repeatability checks did not replace the deleted manual ownership instrument. The
+configurable-window shrink samples and shrink/reclaim budgets are superseded by the derived-window
+held-burst liveness and old-generation reconfiguration witnesses in `tests/atomicwindow.py`.
+All those mechanism, exact-reply, bound, and credit-reclamation checks remain mandatory on every
+build. The original OFF RENAME four-roll hard-failure control is unchanged, and the 1.5-second
+promotion budget still requires `--release-build`. The merged ledger requires quick 325/full 342;
+the constants remain untouched. [MERGE.md](MERGE.md) records the complete conflict and row accounting.
+
+The rest of this section is historical lane evidence, including its now-superseded dispatch,
+shrink, admission-calibration, and ledger statements.
 
 This is a test-only diff in `cx-timing`. No server, benchmark, gate, or compiler was run. The
 earlier acceptance results above describe earlier changes, **not this diff**. The worked fixes
diff --git a/DESIGN-flipfp.md b/DESIGN-flipfp.md
index 32eb6f46c24fb688db6accc79537fbd664bd084c..f705b25837c72a011da71df06dec09d2757f05ae
--- a/DESIGN-flipfp.md
+++ b/DESIGN-flipfp.md
@@ -311,7 +311,9 @@
 detector. The signal and the floor cannot be read from one run -- with a learned band the trigger
 fires and `reset()` wipes the distance -- so each arm is run twice: `--flip-auto-band 90` holds the
 anchor and lets the distance converge (the signal), `--flip-auto-band -1` learns the floor.
-`scratch/flipfp/fpmargin.sh`.
+The former `scratch/flipfp/fpmargin.sh` implemented this two-band experiment. It is retired in the
+merged tree because neither pinning the band nor disabling its retriggers has a public control.
+These historical measurements remain evidence only for their recorded builds; see `MERGE.md`.
 
 **BITCOUNT -> INCR** (the redesign lane's reference change), 2:2 split, `--rate-limiting` pinned:
 
diff --git a/DESIGN.md b/DESIGN.md
index 0fba740d94ba1915868410de40bcb685ae675833..7ad3ac9fef512d8549eadfce44036b8a6a181e9f
--- a/DESIGN.md
+++ b/DESIGN.md
@@ -1,6 +1,6 @@
 # Thread amortization study surface
 
-`thread-mode` and `overlap` are one boot-latched measurement surface. The documented mode
+`thread-mode` and the study-only `x-overlap` are one boot-latched measurement surface. The documented mode
 names are `2s` (separate IO and executor threads, the default) and `1s` (one generalized thread owns
 both roles). `split` and `fused` remain parser aliases so old invocations keep working.
 
@@ -9,10 +9,10 @@
 | `2s` | ordinary split IO/ex loops | exact `t-iopipe` WB/IFID batch schedule | rejected at config validation |
 | `1s` | coarse IFID → EX → WB rotation | exact `t-genthread` `iofused` schedule | gated `iofused` three-way schedule |
 
-The compatibility options `thread-pipeline` and `genthread-schedule` remain accepted: the former
-is a numeric alias for `overlap`, while the latter maps `coarse`, `iofused`, and the legacy
-`streams` name to `1s` plus overlap 0, 1, and 2 respectively. The legacy name is only a parser
-alias; overlap 2 no longer runs the streams implementation. Overlap 2 emits a boot warning because
+Use `--x-overlap 0|1|2` for these study schedules and `--x-ex-sched 0|1` for the executor study.
+The old public schedule spellings and compatibility aliases are removed; see
+[DESIGN-KNOBS.md](DESIGN-KNOBS.md). Below, "overlap" names the internal schedule value.
+Overlap 2 emits a boot warning because
 it is an experimental research schedule. Unified overlap 1 and 2 require `net-io uring`; the split
 iopipe schedule retains its explicit epoll specialization. No schedule constant is runtime-tunable.
 
diff --git a/NOTES-HYGIENE.md b/NOTES-HYGIENE.md
index 23506bce64f00cb0dccaaff93362ccd30deab229..f224db07f4ceb2e4791c7cbb471b87d83a0a3e93
--- a/NOTES-HYGIENE.md
+++ b/NOTES-HYGIENE.md
@@ -352,7 +352,8 @@
 ## Test-file census
 
 The calibration examples expireindex.py, borrow_registry.py, and xshard_dispatch_scale.sh were
-already wired in the current parent. aof_frames.py is indirectly executed by aof_frame_order.py;
+already wired in this note's historical parent. The merged tree retires the dispatch instrument;
+the two surviving batteries use `--release-build`. aof_frames.py is indirectly executed by aof_frame_order.py;
 aof_rewrite.py and aof_rewrite_triggers.py are driven by matrix wrappers; differ.py is driven by
 differ_gate.sh; niclib.sh and gate_refs.txt support optional full-tier NIC rows.
 
@@ -456,7 +457,7 @@
 notify: ok (notify_events_fired=1608)
 ~~~
 
-Purpose-boot matrix commands:
+Historical purpose-boot matrix commands (superseded by the current gate's network-engine grammar):
 
 ~~~sh
 PERSIST_IO=normal GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_matrix.sh
@@ -465,7 +466,8 @@
 PERSIST_IO=uring  GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_trigger_matrix.sh
 ~~~
 
-Other purpose-booted battery invocations (each boot used the knobs documented by gate.sh):
+Historical purpose-booted battery invocations (not recipes for the merged tree; the dispatch
+instrument and script OFF arm below are retired):
 
 ~~~sh
 python3 tests/xscript.py 127.0.0.1 7600 off|limit|window
diff --git a/NOTES-XPERF2.md b/NOTES-XPERF2.md
index d4ce7950b1396997d44f0945f6f5179aa625e59e..d1edc822a5e7abc68e6aefc3d5a55986b0bf178e
--- a/NOTES-XPERF2.md
+++ b/NOTES-XPERF2.md
@@ -8,7 +8,7 @@
 |---|---|---|---|---|
 | E4 | ExpireIndex never shrinks; memsets the whole sidecar on live→0; single-pass rehash | **REPRODUCED** (both halves) | yes | `tests/expireindex.py` |
 | E5 | zero-copy borrow registry is a linear vector scanned per borrow/release/retire | **REPRODUCED** | yes | `tests/borrow_registry.py` |
-| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes | `tests/xshard_dispatch_scale.sh` |
+| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes, historically | retired manual-map dispatch instrument; see `MERGE.md` |
 | E7 | segment mode is absorbing: once triggered every later reply is a malloc + iovec slot | **NOT REPRODUCED** | no | none |
 
 Rig: EPYC 9754, cores 48–63 only, ports 7200–7209 only, loopback. Oracle = vanilla redis 7.4 at
@@ -292,18 +292,20 @@
 |---|---|---|---|---|
 | `tests/expireindex.py <host> <port>` | `--shards 1 --ratio 1:1 --enable-debug-command yes` | growth-trigger cost ratio over a 16× population ≤ 8.0; live→0 DEL ≤ 1.25× a neighbouring DEL | FAIL 2 (16.30×, 2.00×) | PASS (1.65×, 1.00×) |
 | `tests/borrow_registry.py <host> <port>` | `--shards 1 --zc-min 64 --client-output-buffer-limit normal 0 0 0 --enable-debug-command yes` | borrowed-GET per-op cost growth over 0 → ~2000 live borrows ≤ 1.05 | FAIL (1.078, 1.079) | PASS (1.008, 1.008, 1.009) |
-| `tests/xshard_dispatch_scale.sh` (+ `.py`) | boots its own 4-thread and 128-thread arms | dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
+| Retired `tests/xshard_dispatch_scale.sh` (+ `.py`) | historically booted fixed-owner 4-thread and 128-thread arms | historical dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
 
 Each also carries its own non-vacuity checks: `expireindex.py` asserts INFO `expires` really
 reached the population under test and that INFO `expired_keys` MOVED (so the sampler is proven to
 still visit deadlines parked mid-migration), plus a no-TTL negative control on both timing legs;
 `borrow_registry.py` asserts `CLIENT LIST oll` grew (the borrow path is proven live, not assumed),
 carries a non-borrowed control arm, and refuses to run unless the zc gate straddles its two probe
-sizes; `xshard_dispatch_scale.py` refuses to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` prove
+sizes; the retired `xshard_dispatch_scale.py` refused to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` proved
 the cross pair really fanned out to two owner threads and the control stayed on one.
 
-None of the three is wired into `tests/gate.sh`: they each need their own boot geometry and the
-gate's port/cores belong to the mainline operator. Run them as above.
+These were standalone instruments when measured. The merged gate runs the expiry and borrow
+batteries with `--release-build`. The dispatch wrapper and helper are retired because their manual
+ownership geometry is unavailable; the historical measurements above do not validate default
+round-robin ownership. See [MERGE.md](MERGE.md).
 
 ## Evidence
 
diff --git a/docs/flipctl-design.md b/docs/flipctl-design.md
index e6a5e31a6c05171b172746f2586a01d09fa1e84e..cd96557199b408c8b85bb42b77ca59ee6fec73ea
--- a/docs/flipctl-design.md
+++ b/docs/flipctl-design.md
@@ -8,8 +8,7 @@
 ## Ownership and footprint
 
 - `src/main.cc` is the controller cron owner. When `flip-auto=1`, the main/monitor thread calls
-  `Server::flipctl_tick()` once per existing LB tick (`lb-tick-ms`; if that mechanism is disabled,
-  the period derives from the provisioned thread count). No IO or EX loop runs controller policy.
+  `Server::flipctl_tick()` on its internal observation cadence. No IO or EX loop runs controller policy.
 - `src/core/flipctl.{h,cc}` contains the fingerprint representation, shift detector, and controller
   state machine. Controller vectors allocate only when `flip-auto=1`.
 - Each `ThreadCtx` embeds one `FlipFingerprintWriter`. It has no dynamic storage. Only the physical
@@ -46,9 +45,9 @@
 measurement quantum, avoiding the zero-anchor discontinuity where one rare admin argument would
 otherwise have distance one.
 
-With `flip-auto-band=-1`, the band is twice the maximum quiet adjacent/drift distance learned at
-the final split, floored by the four-family sampling quantum divided by observed commands. A
-numeric value is a percent. Zero disables fingerprint and collapse re-triggers. Final anchor
+The automatic band is twice the maximum quiet adjacent/drift distance learned at
+the final split, floored by the four-family sampling quantum divided by observed commands.
+It has no public override. Final anchor
 learning consumes a number of closed aggregates derived from the provisioned thread count. It is
 done after maneuver-only age sampling is disarmed, so the controller cannot detect its own signal
 cost transition as a workload change.
@@ -64,13 +63,13 @@
 falls below a threshold derived from its own change jitter and measurement quantum. Directional
 connection ramp-up therefore remains drift, while jittery stationary traffic can qualify even when
 its absolute rate never enters a quiet band. A 30-second ceiling, converted to ticks from
-`lb-tick-ms`, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
+the internal observation cadence, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
 thread per tick resets the learning/cap state, so an idle server remains in
 `awaiting-load-stability` and never pays for a maneuver it cannot use.
 
-`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. An explicit
-`lb-age-sample-rate` is used when nonzero; otherwise the rate derives from the provisioned thread
-count. IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
+`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. Its rate
+derives from observed command volume and the three-tick decision window, and adapts on completed
+traffic windows; see [DESIGN-KNOBS.md](../DESIGN-KNOBS.md). IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
 writes. IO releases its ROB-head age map when the rate returns to zero.
 
 After a stabilized baseline rate, `issue_initial_jump()` subtracts the per-thread maneuver window.
@@ -88,8 +87,8 @@
 not vetoed by idle peers. If either role has no measured work/busy quantity, measurement continues;
 the controller does not invent a workload prior.
 
-The calculation is performed in placement units (one logical CPU normally, one sibling pair under
-`smt-mode=1`) and then reported/committed in threads. The target passes `nio_eq` in the measured
+The calculation is performed in placement units (one logical CPU normally, one sibling pair when
+the allowed topology contains complete pairs) and then reported/committed in threads. The target passes `nio_eq` in the measured
 direction by `max(1, abs(nio_eq-nio_now)/2)`, clamped to one live unit of each role. Even when the
 equalized split rounds to the current split, the direction of greater measured demand supplies the
 deliberate one-unit overshoot.
@@ -143,7 +142,7 @@
 
 ## Integration and observability
 
-- `src/core/config.h` is the sole parser/default source for `flip-auto`, `flip-auto-band`, and
+- `src/core/config.h` is the sole parser/default source for `flip-auto` and
   `flip-work-window`; `src/cmd/t_server.cc` exposes them as immutable CONFIG values.
 - `tomokv.conf` documents the numeric grammar and off/auto semantics.
 - `INFO FLIPCTL` reports state/phase, anchor split/rate, bands, total/per-reason trigger counts, and
diff --git a/docs/specs/AUDIT-TLS.md b/docs/specs/AUDIT-TLS.md
index 0256e887f5d27c210d436bc219f3c7521ddad0db..87a2eaca3137a47f7c0e1567794650f57531860b
--- a/docs/specs/AUDIT-TLS.md
+++ b/docs/specs/AUDIT-TLS.md
@@ -3,7 +3,7 @@
 > **Where it landed (2026-09-04).** The feature shipped as `src/net/tls.{h,cc}` (this document
 > proposes `src/net/tls_ctx.h` + `src/net/tls_conn.h`); the test is `tests/tls.py` (no
 > `tests/gen-certs.sh`); no `tls-bio-size` knob exists. Defaults: AES-128-GCM-first cipher lists
-> and `tls-ktls yes` (`tomokv.conf`, NETWORK).
+> and automatic kTLS negotiation with userspace fallback (`tomokv.conf`, NETWORK).
 
 Governing audit. Read before writing any TLS code in `/home/user/Projects/tomokv-cpp-perthread`.
 No repository was modified in producing this.
diff --git a/scratchpad/rlbatch/lib.sh b/scratchpad/rlbatch/lib.sh
index 7bcf3e4e6030c79174bf35d9bb1b3dfb11f80c42..12184314d2aafac0dc7aa48db761a3bdec5de8c7
--- a/scratchpad/rlbatch/lib.sh
+++ b/scratchpad/rlbatch/lib.sh
@@ -17,7 +17,7 @@
 boot_srv(){ # boot_srv <binary> <logfile> [extra args...] -> SRV pid
   local bin="$1" log="$2"; shift 2
   guard_port "$PORT" || return 1
-  # EXTRA_ARGS lets a caller add boot flags (e.g. --read-local-interleave 0) without a second copy
+  # EXTRA_ARGS lets a caller add boot flags (e.g. --lb 0) without a second copy
   # of this function; word splitting is intentional.
   # shellcheck disable=SC2086
   taskset -c "$SRVCORE" "$bin" --port "$PORT" --bind 127.0.0.1 --shards 16 \
diff --git a/tests/flipctl_unit.cc b/tests/flipctl_unit.cc
index 540a6c40e9589572431075b623bc553c76ee8d85..33209258e2958771d60f86b919f6f510e9977737
--- a/tests/flipctl_unit.cc
+++ b/tests/flipctl_unit.cc
@@ -184,11 +184,11 @@
 
     // THE TYPED BAND IS A FLOOR, NOT A CEILING. The gate-hygiene lane's instrumented failure: a
     // driver holding its rate to 0.07% across 34 samples, a fingerprint distance of 0.2518 against
-    // a flat --flip-auto-band 2 (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
+    // a flat explicit band of 2% (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
     // A detector whose signature genuinely moves between adjacent quiet windows must end up with a
     // band above that movement whatever the operator typed, and must still fire on a real change.
     {
-        FlipShiftDetector typed(2, 4);   // --flip-auto-band 2
+        FlipShiftDetector typed(2, 4);   // internal explicit-band control: 2%
         // Stationary but NOISY: the read/write split alternates 35:65 / 65:35 window to window.
         // The detector smooths the signature, so the anchored distance this produces is ~0.09.
         const auto noisy = [](int k) { return quiet(k % 2 ? 65 : 35, k % 2 ? 35 : 65); };
@@ -250,8 +250,8 @@
         if (fires) fail("the learned band fired on quiet-state movement");
     }
     // A TYPED BAND UNDER THE REAL SAMPLER. The shipped configuration cannot reach this -- the
-    // controller runs the learned band -- but an operator who sets --flip-auto-band AND the sampler
-    // can, and on a tree where the typed path bypasses the floor the fingerprint lane measured 73
+    // controller runs the learned band. This internal explicit-band control preserves the earlier
+    // regression: where the typed path bypassed the floor the fingerprint lane measured 73
     // two-consecutive exceedances in 600 stationary windows at K=100 and 208 at K=60: a spurious
     // maneuver every few seconds on a load that never changed. Driven here through THEIR writer, a
     // stationary 50/50 stream at depth 32, with the floor in place: none, and a real mix change
@@ -259,7 +259,7 @@
     {
         for (uint32_t window : {60u, 100u}) {
             XorShift rng;
-            FlipShiftDetector typed(2, 8);   // --flip-auto-band 2, the operator's knob
+            FlipShiftDetector typed(2, 8);   // internal explicit-band control: 2%
             int fires = 0, confirmed = 0, streak = 0;
             for (int w = 0; w < 60; w++) {
                 FlipFingerprintWriter writer;
diff --git a/tests/rlcache_churn.py b/tests/rlcache_churn.py
index b56ae4041ae0843dc67443f16017270063853124..f8a2c0730bb0a4fb6c3b5f6282c547226bb76599
--- a/tests/rlcache_churn.py
+++ b/tests/rlcache_churn.py
@@ -59,7 +59,7 @@
 
 
 # THE LOAD BALANCER MUST HAVE SOMETHING TO BALANCE. A uniform key distribution over one small key
-# space keeps every shard at the same op rate, the imbalance never crosses lb-imbalance-pct, and no
+# space keeps every shard at the same op rate, the imbalance never crosses the learned band, and no
 # shard ever changes owner -- which is the precondition for the defect this battery exists for
 # (measured: 7.8M ops, 0 migrations). A hot window that ROTATES makes a few shards hot, then makes
 # different ones hot, so the balancer moves shards continuously and in both directions, which is

exec
/bin/bash -lc "git diff -- tests/atomic_torn.py && git diff -- tests/gate.sh | sed -n '1,180p' && git diff -- scratch/rv3.sh scratch/flipfp/chain.sh tests/flipctl_unit.cc" in /home/user/Projects/cx-merge
 succeeded in 0ms:
diff --git a/tests/atomic_torn.py b/tests/atomic_torn.py
index 448e00be6..c3a43f410 100755
--- a/tests/atomic_torn.py
+++ b/tests/atomic_torn.py
@@ -10,9 +10,17 @@ import time
 import _lib
 
 
-HOST, PORT = sys.argv[1], int(sys.argv[2])
+# Build identity comes from the caller. Counter movement, exact values, and safety bounds are
+# mandatory on every build; only the time spent reaching them is a release-build assertion.
+ARGS = sys.argv[1:]
+RELEASE_BUILD = "--release-build" in ARGS
+ARGS = [arg for arg in ARGS if arg != "--release-build"]
+if len(ARGS) != 2:
+    raise SystemExit("usage: atomic_torn.py HOST PORT [--release-build]")
+HOST, PORT = ARGS[0], int(ARGS[1])
 FAIL = 0
 BARRIER_TIMEOUT = 10
+MECHANISM_TIMEOUT = 30.0  # liveness watchdog, never a successful/skip outcome on exhaustion
 
 
 def note(name, ok, extra=""):
@@ -26,6 +34,13 @@ def skip(name, extra=""):
     print("  SKIP " + name + (" " + extra if extra else ""), flush=True)
 
 
+def release_note(name, ok, measured):
+    if RELEASE_BUILD:
+        note(name, ok, measured)
+    else:
+        skip(name, "requires --release-build; " + measured)
+
+
 def abort_barriers(*barriers):
     for barrier in barriers:
         try:
@@ -669,6 +684,7 @@ note("OFF control exposes torn MSET-8",
 before = stats()
 required_stat(before, "atomic_predecessor_reads")
 required_stat(before, "atomic_commit_holds")
+required_stat(before, "atomic_promotions")
 debug("ATOMIC-COMMIT-DELAY", 2000)
 try:
     on_torn, on_reads, on_errors, _, _, on_threads_still_alive = hammer(
@@ -680,21 +696,32 @@ pred_delta = (required_stat(after, "atomic_predecessor_reads") -
               required_stat(before, "atomic_predecessor_reads"))
 hold_delta = (required_stat(after, "atomic_commit_holds") -
               required_stat(before, "atomic_commit_holds"))
-promo_delta = after.get("atomic_promotions", 0) - before.get("atomic_promotions", 0)
+promo_delta = required_stat(after, "atomic_promotions") - required_stat(before, "atomic_promotions")
 # V2 batches reclamation on owner passes and the 50ms low-frequency sweep instead of posting a
-# cleanup task for every retired group. Give that deliberately cold path one bounded tick to fire.
-deadline = time.time() + 1.5
-while promo_delta == 0 and time.time() < deadline:
+# cleanup task for every retired group. Poll completed work's cleanup with a watchdog; only the
+# original 1.5s completion budget is release-specific. This does not re-arm any race window.
+promotion_started = time.monotonic()
+deadline = promotion_started + MECHANISM_TIMEOUT
+while promo_delta == 0 and time.monotonic() < deadline and not on_errors and not on_torn:
     time.sleep(0.05)
     after = stats()
-    promo_delta = after.get("atomic_promotions", 0) - before.get("atomic_promotions", 0)
+    promo_delta = required_stat(after, "atomic_promotions") - required_stat(before, "atomic_promotions")
+promotion_elapsed = time.monotonic() - promotion_started
 note("ON MSET-8/MGET-8 torn-free",
      on_torn == 0 and on_reads > 0 and not on_errors and not on_threads_still_alive,
      "torn=%d reads=%d errors=%r threads_still_alive=%r" %
      (on_torn, on_reads, on_errors, on_threads_still_alive))
+# These are window-hit claims, NEVER release-only or skip-on-miss. Induce respectively by
+# removing safe-cut holds, bypassing predecessor lookup, or disabling promotion. Each still
+# fails on ASAN with a zero delta. Fresh-connection retry bounds for the first two need their own
+# measured hit rates; the RENAME control's 5/6 measurement cannot calibrate a different window.
 note("ON commit-delay window held a read cut", hold_delta > 0, "delta=%d" % hold_delta)
 note("ON exercised predecessor resolution", pred_delta > 0, "delta=%d" % pred_delta)
 note("ON exercised promotion", promo_delta > 0, "delta=%d" % promo_delta)
+release_note("ON promotion drains within release budget",
+             promo_delta > 0 and promotion_elapsed <= 1.5,
+             "promotion-drain=%.3fs holds=%d predecessors=%d promotions=%d" %
+             (promotion_elapsed, hold_delta, pred_delta, promo_delta))
 note("atomic_inflight returns to idle", after.get("atomic_inflight", -1) == 0,
      "value=%d" % after.get("atomic_inflight", -1))
 
@@ -865,104 +892,23 @@ else:
          copy_on[0] == 0 and not copy_on[1] and not copy_on[2],
          "anomalies=%d errors=%r threads_still_alive=%r" % copy_on)
 
-# Admission liveness: with a one-group window, the second frame in one received pipeline reaches
-# admission before owner notifications for the first are flushed. This makes the fired assertion
-# deterministic instead of depending on Python threads winning a scheduling race.
+# Separate admission liveness from live credit reconfiguration/accounting. Both use the
+# production bound and must actually enter their window; a clean miss is re-armed, never skipped.
+from atomicwindow import held_burst
 config("atomic", 1)
-config("atomic-window", 1)
-window_before = stats().get("atomic_window_stalls", 0)
-window_run = format(time.time_ns() & 0xfffffff, "x")
-window_errors = []
-window_client = Resp()
-window_frames = []
-for sequence in range(64):
-    args = ["MSET"]
-    for key_index in range(8):
-        args.extend(("aw%s:%x:%x" % (window_run, sequence, key_index),
-                     "window:%x" % sequence))
-    window_frames.append(frame(*args))
 try:
-    window_client.sock.sendall(b"".join(window_frames))
-    for _ in window_frames:
-        if window_client.read() != b"OK":
-            raise AssertionError("bad window reply")
+    witness = held_burst(HOST, PORT, whole_window=True)
+    note("derived atomic window stalls and resumes", True, witness)
 except Exception as exc:
-    window_errors.append(str(exc))
-finally:
-    window_client.close()
-window_after = stats().get("atomic_window_stalls", 0)
-note("atomic-window stalls and resumes",
-     not window_errors and window_after > window_before,
-     "stalls=%d errors=%r" % (window_after - window_before, window_errors))
-config("atomic-window", 256)
-
-# Credit leases must preserve the exact configured bound while CONFIG changes it under load. A
-# shrink may inherit more already-admitted groups than the new limit; wait for that unavoidable
-# debt to retire, then prove no subsequent sample exceeds the bound. Finally, an idle system must
-# have returned every leased credit to the pool so a skewed next IO can consume the whole window.
-lease_stop = threading.Event()
-lease_errors = []
-
-
-def lease_writer(wid):
-    client = None
-    keys = ["at:lease:%d:k%d" % (wid, key) for key in range(8)]
-    seq = 0
-    try:
-        client = Resp()
-        while not lease_stop.is_set():
-            if mset(client, keys, "lease:%d:%d" % (wid, seq)) != b"OK":
-                raise AssertionError("bad lease reply")
-            seq += 1
-    except Exception as exc:
-        lease_errors.append("writer%d:%s" % (wid, exc))
-    finally:
-        if client is not None:
-            client.close()
+    note("derived atomic window stalls and resumes", False, str(exc))
 
-
-lease_threads = [threading.Thread(target=lease_writer, args=(i,), daemon=True) for i in range(8)]
-for thread in lease_threads:
-    thread.start()
-for value in (31, 7, 19, 3):
-    if not config("atomic-window", value):
-        lease_errors.append("CONFIG atomic-window %d failed" % value)
-deadline = time.time() + 3
-bounded = False
-max_inflight = 0
-while time.time() < deadline:
-    sample = stats()
-    live = sample.get("atomic_inflight", 1000000)
-    debt = sample.get("atomic_credit_debt", 1000000)
-    if live <= 3 and debt == 0:
-        bounded = True
-        break
-    time.sleep(0.01)
-if bounded:
-    deadline = time.time() + 0.4
-    while time.time() < deadline:
-        live = stats().get("atomic_inflight", 1000000)
-        max_inflight = max(max_inflight, live)
-        if live > 3:
-            bounded = False
-            break
-lease_stop.set()
-for thread in lease_threads:
-    thread.join(timeout=10)
-deadline = time.time() + 3
-lease_after = stats()
-while lease_after.get("atomic_inflight", -1) != 0 and time.time() < deadline:
-    time.sleep(0.01)
-    lease_after = stats()
-note("atomic-window reconfiguration preserves bound and reclaims leases",
-     bounded and not any(thread.is_alive() for thread in lease_threads) and not lease_errors and
-     lease_after.get("atomic_inflight", -1) == 0 and
-     lease_after.get("atomic_credit_debt", -1) == 0 and
-     lease_after.get("atomic_credit_pool", -1) == 3,
-     "max=%d pool=%d debt=%d errors=%r" %
-     (max_inflight, lease_after.get("atomic_credit_pool", -1),
-      lease_after.get("atomic_credit_debt", -1), lease_errors))
-config("atomic-window", 256)
+# Resizing was removed with atomic-window. CONFIG SET atomic 1 still rebuilds credit generations;
+# require surviving old-generation groups before asserting the fixed bound and reclaimed leases.
+try:
+    witness = held_burst(HOST, PORT, whole_window=True, reconfigure=True)
+    note("atomic reconfiguration preserves derived bound and reclaims leases", True, witness)
+except Exception as exc:
+    note("atomic reconfiguration preserves derived bound and reclaims leases", False, str(exc))
 
 # Live CONFIG flips under active traffic are a liveness/safety arm. OFF intervals deliberately do
 # not promise atomicity; after ending ON, one final group must be read intact.
diff --git a/tests/gate.sh b/tests/gate.sh
index c4d8fef19..c54e238f0 100755
--- a/tests/gate.sh
+++ b/tests/gate.sh
@@ -1,7 +1,7 @@
 #!/bin/bash
 # PRE-PUSH GATE for tomokv-cpp (pure 2s baseline).
 #
-#   tests/gate.sh quick   loopback only, ~3 min: build (release+ASAN), footprint locks, boot
+#   tests/gate.sh quick   loopback only: build (release+ASAN), footprint locks, boot
 #                         matrix, smoke, torture, RYOW, atomic torn/mixed-write/window gates,
 #                         shutdown invariants, counter-fired assertions, idle-loop ceiling. Runs on
 #                         any machine.
@@ -431,7 +431,7 @@ E=$(shutdown_value work.executed)
 # Explicit ON boot plus the non-vacuous epoch-MVCC gates. atomic_torn includes its own OFF control,
 # predecessor/promotion counters, overlapping writers, window liveness, and live CONFIG flips.
 boot ./build/tomokv --atomic 1 --enable-debug-command yes || bad "atomic release boot"
-py tests/atomic_torn.py 127.0.0.1 $PORT >/tmp/gate-atomic-torn.txt 2>&1 \
+py tests/atomic_torn.py 127.0.0.1 $PORT --release-build >/tmp/gate-atomic-torn.txt 2>&1 \
     && ok "atomic torn/window battery" || bad "atomic torn/window battery" "see /tmp/gate-atomic-torn.txt"
 py tests/atomic_ryow.py 127.0.0.1 $PORT >/tmp/gate-atomic-ryow.txt 2>&1 \
     && ok "atomic RYOW/mixed-write battery" || bad "atomic RYOW/mixed-write battery" "see /tmp/gate-atomic-ryow.txt"
@@ -452,7 +452,9 @@ for AT in 0 1; do
   boot ./build/tomokv --atomic $AT --enable-debug-command yes \
       || bad "feature battery boot (atomic $AT)"
   for t in $FEATURE_BATTERIES; do
-    py tests/$t.py 127.0.0.1 $PORT >/tmp/gate-$t-$AT.txt 2>&1 \
+    FEATURE_ARGS=()
+    [ "$t" = stream ] && FEATURE_ARGS+=(--release-build)
+    py tests/$t.py 127.0.0.1 $PORT "${FEATURE_ARGS[@]}" >/tmp/gate-$t-$AT.txt 2>&1 \
         && ok "$t battery (atomic $AT)" || bad "$t battery (atomic $AT)" "see /tmp/gate-$t-$AT.txt"
   done
   stop
@@ -465,7 +467,7 @@ for AT in 0 1; do
   if boot_fused ./build/tomokv --atomic "$AT" --enable-debug-command yes; then
     FUSED_INFO=$(redis-cli -h 127.0.0.1 -p "$PORT" INFO server 2>/dev/null | tr -d '\r')
     FUSED_MODE=$(printf '%s\n' "$FUSED_INFO" | sed -n 's/^thread_mode://p')
-    FUSED_OVERLAP=$(printf '%s\n' "$FUSED_INFO" | sed -n 's/^overlap://p')
+    FUSED_OVERLAP=$(printf '%s\n' "$FUSED_INFO" | sed -n 's/^x_overlap://p')
     [ "$FUSED_MODE" = 1s ] && [ "$FUSED_OVERLAP" = 0 ] \
         && ok "fused boot line (atomic $AT)" \
         || bad "fused boot line (atomic $AT)" "wire mode=$FUSED_MODE overlap=$FUSED_OVERLAP"
@@ -509,7 +511,9 @@ for AT in 0 1; do
     bad "fused+armed boot line (atomic $AT)" "server did not boot; see $SRVLOG"
   fi
   for t in $FEATURE_BATTERIES; do
-    py tests/$t.py 127.0.0.1 $PORT >/tmp/gate-fusedarmed-$t-$AT.txt 2>&1 \
+    FEATURE_ARGS=()
+    [ "$t" = stream ] && FEATURE_ARGS+=(--release-build)
+    py tests/$t.py 127.0.0.1 $PORT "${FEATURE_ARGS[@]}" >/tmp/gate-fusedarmed-$t-$AT.txt 2>&1 \
         && ok "fused+armed $t battery (atomic $AT)" \
         || bad "fused+armed $t battery (atomic $AT)" "see /tmp/gate-fusedarmed-$t-$AT.txt"
   done
@@ -528,20 +532,20 @@ done
 # ---- eviction accounting on both read paths: owner-served (split) and lane-served (fused+armed) --
 # tests/evict_battery.py needs one FRESH boot per section (it sets maxmemory itself and has no
 # FLUSHALL). lfu proves OBJECT FREQ rises for ordinary reads and stays put for CLIENT NO-TOUCH
-# reads, plus hot-set survival under pressure; lruclock is the LRU twin on a 1 s bucket
-# (--lru-clock-shift 0: the default 256 s bucket cannot tell two keys apart inside one gate row).
+# reads, plus hot-set survival under pressure; lruclock ages all cohorts across one fixed 256 s
+# production bucket (a bounded wait of up to 260 s per boot) before testing the read paths.
 # On the armed boot both sections also assert the reads they measure were LANE-served -- a key
 # kept hot only by lane reads was never touched before ExLoopT::note_local_read_access and so was
 # evicted FIRST, which is the inverse of the policy the boot asked for.
 for AT in 0 1; do
   for EVSEC in lfu lruclock; do
-    boot ./build/tomokv --atomic $AT --lru-clock-shift 0 \
+    boot ./build/tomokv --atomic $AT \
         || bad "eviction $EVSEC boot (split, atomic $AT)"
     py tests/evict_battery.py $PORT $EVSEC >/tmp/gate-evict-$EVSEC-split-$AT.txt 2>&1 \
         && ok "eviction $EVSEC battery (split, atomic $AT)" \
         || bad "eviction $EVSEC battery (split, atomic $AT)" "see /tmp/gate-evict-$EVSEC-split-$AT.txt"
     stop
-    boot_fused ./build/tomokv --atomic $AT --read-local 1 --lru-clock-shift 0 \
+    boot_fused ./build/tomokv --atomic $AT --read-local 1 \
         || bad "eviction $EVSEC boot (fused+armed, atomic $AT)"
     py tests/evict_battery.py $PORT $EVSEC >/tmp/gate-evict-$EVSEC-armed-$AT.txt 2>&1 \
         && ok "eviction $EVSEC battery (fused+armed, atomic $AT)" \
@@ -554,7 +558,7 @@ done
 # DEBUG geometry makes false positives impossible in these arms: A/B share one physical shard but
 # use distinct filter cells, as do P/C on the other participating owner. ATOMIC-COMMIT-DELAY holds
 # A/P after raw install and atomic_commit_holds proves the reserved/unpublished window was sampled.
-boot_fused ./build/tomokv --atomic 1 --read-local 1 --read-local-atomic-filter 1 \
+boot_fused ./build/tomokv --atomic 1 --read-local 1 \
     --enable-debug-command yes \
     || bad "B+ read-local purpose boot"
 py tests/bplus.py 127.0.0.1 "$PORT" >/tmp/gate-bplus.txt 2>&1 \
@@ -592,7 +596,6 @@ stop
 # opposite the source. No same-owner or one-executor run can satisfy this row.
 for AT in 0 1; do
   boot ./build/tomokv --ratio 6:2 --atomic "$AT" --enable-debug-command yes \
-       --lb-age-sample-rate 1024 \
       || bad "cross-owner SORT boot (atomic $AT)"
   py tests/sort.py 127.0.0.1 "$PORT" >/tmp/gate-sort-$AT.txt 2>&1 \
       && ok "cross-owner SORT battery (atomic $AT)" \
@@ -602,7 +605,7 @@ done
 
 # ---- debug-surface batteries: these drive DEBUG subcommands, hence their own armed boot -------
 for AT in 0 1; do
-  boot ./build/tomokv --atomic $AT --enable-debug-command yes --lb-age-sample-rate 1024 \
+  boot ./build/tomokv --atomic $AT --enable-debug-command yes \
       || bad "debug-surface boot (atomic $AT)"
   # scriptatomic needs the armed boot for its cross-shard section (DEBUG SHARD proves the group
   # really spans owners; ATOMIC-COMMIT-DELAY / ATOMIC-READ-DELAY widen the window). It flips
@@ -642,17 +645,9 @@ for AT in 0 1; do
   stop
 done
 
-# ---- cross-owner script control boots: each needs its own knob value, hence its own server ----
-# off:    script-crossshard-max-bytes 0 must reproduce today's CROSSSLOT byte-for-byte and leave
-#         every cross counter at zero (the feature is off ⇒ nothing is allocated).
-# limit:  a tiny staging budget must refuse before RUN and leave the declared values untouched.
-# window: one cut slot per IO thread must refuse the overflow with -BUSY, counted exactly, with
-#         the same activations issued one at a time as the zero control.
-for XS in "off:--script-crossshard-max-bytes 0" \
-          "limit:--script-crossshard-max-bytes 300" \
-          "window:--script-crossshard-cut-slots 1"; do
-  XS_MODE=${XS%%:*}; XS_ARGS=${XS#*:}
-  boot ./build/tomokv --atomic 1 --enable-debug-command yes $XS_ARGS \
+# ---- cross-owner script bounds: production auto budgets and four cut slots per IO ----------
+for XS_MODE in limit window; do
+  boot ./build/tomokv --atomic 1 --enable-debug-command yes \
       || bad "xscript $XS_MODE control boot"
   py tests/xscript.py 127.0.0.1 $PORT "$XS_MODE" >/tmp/gate-xscript-$XS_MODE.txt 2>&1 \
       && ok "xscript $XS_MODE control" || bad "xscript $XS_MODE control" \
@@ -661,13 +656,12 @@ for XS in "off:--script-crossshard-max-bytes 0" \
 done
 
 # ---- efficiency guards: each needs a boot geometry of its own, so each gets its own server ----
-# These assert a RATIO or a growth bound, never an absolute time, and each was verified to FAIL on
-# the binary from before its fix (see NOTES-XPERF2.md). They shipped unwired, which is the same
-# coverage-that-does-not-exist problem the gate lane fixed elsewhere.
+# Exact mechanism checks run on every build; timing budgets require the explicit release-build
+# flag. A ratio of wall times is still a performance claim (see DESIGN-GATEHYGIENE.md).
 # --shards 1 for the same reason as the borrow guard: the sidecar under test is per shard, and the
 # battery asserts that precondition rather than quietly measuring a diluted one.
 boot ./build/tomokv --shards 1 --enable-debug-command yes || bad "expire-index guard boot"
-py tests/expireindex.py 127.0.0.1 $PORT >/tmp/gate-expireindex.txt 2>&1 \
+py tests/expireindex.py 127.0.0.1 $PORT --release-build >/tmp/gate-expireindex.txt 2>&1 \
     && ok "expire-index growth bound" || bad "expire-index growth bound" "see /tmp/gate-expireindex.txt"
 stop
 
@@ -675,15 +669,11 @@ stop
 # an ordinary-sized value still takes the borrow path and pays registry cost.
 boot ./build/tomokv --shards 1 --zc-min 64 --client-output-buffer-limit "normal 0 0 0" \
     --enable-debug-command yes || bad "borrow-registry guard boot"
-py tests/borrow_registry.py 127.0.0.1 $PORT >/tmp/gate-borrow.txt 2>&1 \
+py tests/borrow_registry.py 127.0.0.1 $PORT --release-build >/tmp/gate-borrow.txt 2>&1 \
     && ok "borrow-registry growth bound" || bad "borrow-registry growth bound" "see /tmp/gate-borrow.txt"
 stop
 
-# this one boots its own arms at two thread counts, so it takes the port/cores rather than a server
-quiet_wait
-XDS_PORT=$PORT XDS_CPUS=$CORES XDS_BIN=./build/tomokv bash tests/xshard_dispatch_scale.sh \
-    >/tmp/gate-xds.txt 2>&1 \
-    && ok "cross-shard dispatch scaling" || bad "cross-shard dispatch scaling" "see /tmp/gate-xds.txt"
+# Dispatch scaling's manual ownership geometry is retired; see MERGE.md and DESIGN-KNOBS.md.
 
 # ---- Redis-wire DUMP/RESTORE survives the native snapshot/restart boundary -------------------
 DUMPRESTORE_DIR=$(mktemp -d /tmp/gate-dumprestore.XXXXXX)
@@ -743,25 +733,25 @@ py tests/debug.py 127.0.0.1 $PORT >/tmp/gate-debug.txt 2>&1 \
 stop
 
 # ---- snapshot data/sync engines: cut, typed round-trip, and typed preimage race ----------------
-for PERSIST_IO in normal uring; do
-  SNAP_DIR=$(mktemp -d "/tmp/gate-snapshot-${PERSIST_IO}.XXXXXX")
-  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
+for NET_IO in epoll uring; do
+  SNAP_DIR=$(mktemp -d "/tmp/gate-snapshot-${NET_IO}.XXXXXX")
+  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
       --dir "$SNAP_DIR" --dbfilename cut.tomo \
-      || bad "snapshot cut boot ($PERSIST_IO)"
+      || bad "snapshot cut boot ($NET_IO)"
   py tests/snap_cut_battery.py "$PORT" save \
-      >"/tmp/gate-snapshot-cut-${PERSIST_IO}.txt" 2>&1 \
-      && ok "snapshot concurrent cut ($PERSIST_IO)" \
-      || bad "snapshot concurrent cut ($PERSIST_IO)" \
-             "see /tmp/gate-snapshot-cut-${PERSIST_IO}.txt"
+      >"/tmp/gate-snapshot-cut-${NET_IO}.txt" 2>&1 \
+      && ok "snapshot concurrent cut ($NET_IO)" \
diff --git a/scratch/flipfp/chain.sh b/scratch/flipfp/chain.sh
index aa2197c81..3b9fb32e7 100755
--- a/scratch/flipfp/chain.sh
+++ b/scratch/flipfp/chain.sh
@@ -1,8 +1,9 @@
 #!/bin/bash
+# Boot grammar updated for derived controls; historical PRE/POST results need fresh validation.
 # chain.sh -- t-flipfp proof chain. Resumable (every cell skips when its output file exists),
 # gate-pausing (quiet.done age + intruders before every cell). Re-run to resume.
 #   S0 digests            S1 slope cells PRE / POST / PRE-null (flip-auto 0) on 1T, 2T, 2s
-#   S1b slope 2s with --flip-auto 1 (anchored controller, band 0) PRE vs POST: the always-on cost
+#   S1b retired: the fixed-band controller cost experiment has no equivalent automatic-band arm.
 #   S2 accuracy: anchored band + false triggers, homogeneous MK and heterogeneous SET|GET, PRE vs POST
 #   S3 wrong-split boots 3:1 (4 thr) and 5:1 (6 thr): PRE-auto, POST-auto, flipguard, POST off
 #   S4 flipctl.py gate row (POST x2, PRE x1)   S5 batteries 1s + 2s   S6 differ   S7 gate quick
@@ -51,10 +52,7 @@ if want S1; then
     slope "prenull-$geom" "$PRE_BIN"  "$geom"
   done
 fi
-if want S1b; then
-  slope "pre1-2s"  "$PRE_BIN"  2s "--flip-auto 1 --flip-auto-band 0" 75
-  slope "post1-2s" "$POST_BIN" 2s "--flip-auto 1 --flip-auto-band 0" 75
-fi
+# S1b deliberately emits no measurements; its historical tables remain readable in S8.
 
 # ---- S2 accuracy: anchored band + false triggers on a stationary mix ---------------------------
 acc(){ # acc TAG BIN KIND(mk|hetero)
@@ -125,7 +123,7 @@ ctl(){ # ctl TAG BIN
   if [ -z "$tag" ] || [ "${out#*$tag}" = "$out" ]; then LG "HARNESS BUG: tag='$tag' out='$out'"; return 1; fi
   [ -s "$out" ] && { LG "CTL $tag on file :: $(grep -E '^ok:|AssertionError|anchored off-rail|Error' "$out" | head -1 | cut -c1-200)"; return 0; }
   wait_gate
-  local pid; pid=$(SRV_CPUS=$MY_MASK boot "$bin" "$PORT_CTL" "ctl-$tag" --ratio 6:2 --atomic 0 --flip-auto 1 --flip-auto-band 2 --lb-age-sample-rate 1024) || return 1
+  local pid; pid=$(SRV_CPUS=$MY_MASK boot "$bin" "$PORT_CTL" "ctl-$tag" --ratio 6:2 --atomic 0 --flip-auto 1) || return 1
   taskset -c "$MY_MASK" timeout 300 python3 tests/flipctl.py --host 127.0.0.1 --port "$PORT_CTL" --stable-seconds 30 >"$out.part" 2>&1; local rc=$?
   echo "RC=$rc" >>"$out.part"; flipinfo "$PORT_CTL" >>"$out.part"; $CLI -p "$PORT_CTL" debug flipctl >>"$out.part" 2>&1
   mv "$out.part" "$out"; LG "CTL $tag rc=$rc :: $(grep -E '^ok:|AssertionError|anchored off-rail|Error' "$out" | head -1 | cut -c1-200)"
diff --git a/scratch/rv3.sh b/scratch/rv3.sh
index db1fbe91a..e94f54f55 100755
--- a/scratch/rv3.sh
+++ b/scratch/rv3.sh
@@ -1,6 +1,7 @@
 #!/bin/bash
+# Boot grammar updated for derived controls; historical PRE/POST results need fresh validation.
 # rv3.sh -- build + verify the fingerprint-band floor: STABLE HOLD (tests/flipctl.py, whose stable
-# phase is the row that was failing) x3 with the typed --flip-auto-band 2 the gate uses, the directed
+# phase is the row that was failing) x3 with the derived band, the directed
 # battery, and the two wrong-split boots + the stationary cell (must still move / must not move).
 source /home/user/Projects/wt-flipdamp/scratch/lib.sh; cd "$WT"
 LG(){ echo "$(date +%T) $*"; }
@@ -15,7 +16,7 @@ taskset -c 52-57,180-185 make unit >"$SP/fd-r6-unit.txt" 2>&1; LG "UNIT rc=$?"
 # STABLE HOLD x3 -- the failing row. Report band, distance and ratio whatever the verdict.
 for r in 1 2 3; do
   out=$SP/fd-r6-ctl-$r.txt; wait_gate
-  pid=$(boot8 "$FIX_BIN" "$PORT_BAT" "r6ctl-$r" --ratio 6:2 --atomic 0 --flip-auto 1 --flip-auto-band 2 --lb-age-sample-rate 1024) || continue
+  pid=$(boot8 "$FIX_BIN" "$PORT_BAT" "r6ctl-$r" --ratio 6:2 --atomic 0 --flip-auto 1) || continue
   taskset -c "$LG8" timeout 300 python3 tests/flipctl.py --host 127.0.0.1 --port "$PORT_BAT" --stable-seconds 30 >"$out" 2>&1; rc=$?
   echo "RC=$rc" >>"$out"; redis-cli -p "$PORT_BAT" debug flipctl >>"$out" 2>&1
   LG "CTL $r rc=$rc :: $(grep -E 'stable hold|anchored off-rail|^ok:|AssertionError' "$out" | head -2 | tr '\n' ' ' | cut -c1-200) :: $(grep -E '^signature_(band|distance|noise_bound)|^last_trigger' "$out" | tr '\n' ' ')"
diff --git a/tests/flipctl_unit.cc b/tests/flipctl_unit.cc
index 540a6c40e..33209258e 100644
--- a/tests/flipctl_unit.cc
+++ b/tests/flipctl_unit.cc
@@ -184,11 +184,11 @@ int main() {
 
     // THE TYPED BAND IS A FLOOR, NOT A CEILING. The gate-hygiene lane's instrumented failure: a
     // driver holding its rate to 0.07% across 34 samples, a fingerprint distance of 0.2518 against
-    // a flat --flip-auto-band 2 (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
+    // a flat explicit band of 2% (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
     // A detector whose signature genuinely moves between adjacent quiet windows must end up with a
     // band above that movement whatever the operator typed, and must still fire on a real change.
     {
-        FlipShiftDetector typed(2, 4);   // --flip-auto-band 2
+        FlipShiftDetector typed(2, 4);   // internal explicit-band control: 2%
         // Stationary but NOISY: the read/write split alternates 35:65 / 65:35 window to window.
         // The detector smooths the signature, so the anchored distance this produces is ~0.09.
         const auto noisy = [](int k) { return quiet(k % 2 ? 65 : 35, k % 2 ? 35 : 65); };
@@ -250,8 +250,8 @@ int main() {
         if (fires) fail("the learned band fired on quiet-state movement");
     }
     // A TYPED BAND UNDER THE REAL SAMPLER. The shipped configuration cannot reach this -- the
-    // controller runs the learned band -- but an operator who sets --flip-auto-band AND the sampler
-    // can, and on a tree where the typed path bypasses the floor the fingerprint lane measured 73
+    // controller runs the learned band. This internal explicit-band control preserves the earlier
+    // regression: where the typed path bypassed the floor the fingerprint lane measured 73
     // two-consecutive exceedances in 600 stationary windows at K=100 and 208 at K=60: a spurious
     // maneuver every few seconds on a load that never changed. Driven here through THEIR writer, a
     // stationary 50/50 stream at depth 32, with the floor in place: none, and a real mix change
@@ -259,7 +259,7 @@ int main() {
     {
         for (uint32_t window : {60u, 100u}) {
             XorShift rng;
-            FlipShiftDetector typed(2, 8);   // --flip-auto-band 2, the operator's knob
+            FlipShiftDetector typed(2, 8);   // internal explicit-band control: 2%
             int fires = 0, confirmed = 0, streak = 0;
             for (int w = 0; w < 60; w++) {
                 FlipFingerprintWriter writer;

diff --git a/AUDIT-TESTS.md b/AUDIT-TESTS.md
index e9cd43ffbc376172717531d0037de1c79fd23690..fe1fd0e2c39e965f605e4a30894a545129541534
--- a/AUDIT-TESTS.md
+++ b/AUDIT-TESTS.md
@@ -122,7 +122,7 @@
 | MM floor (memtier 20 s, sampled 8+6) | 1 | 14 | measurement, keep |
 | saturated-flip memtier | 1 | 25+3 | measurement, keep |
 | `flipctl.py --stable-seconds 30` (+ramp/idle/surge, timeout 300) | 1 | 60-120 est. | measurement, keep |
-| `xshard_dispatch_scale.sh` (2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 | 90-150 est. | measurement, keep |
+| Retired `xshard_dispatch_scale.sh` (historical 2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 historically, 0 now | 90-150 est. historically | manual ownership instrument removed; see `MERGE.md` |
 | `atomic_torn.py` hammers (3+2.5+2+2.5 s) + OFF re-rolls (up to 14 s) + RENAMENX/COPY parks (2 x 64 x 100 ms = 12.8 s) + window/lease/churn (~6 s) | 1 quick, +1 ASAN | ~45-60 each | re-rolls are dead time on this kernel (F6) |
 | `spinprobe.py` (6 s baseline + 6 s probe) | 3 | 37.5 | measurement, keep |
 | Python batteries' literal sleeps (census: flipctl 17 s, limits 5.8, edgetime 4.5, atomic_ryow 4, hexpire 3.4 — most run x2) | | ~60 | keep |
diff --git a/DESIGN-GATEHYGIENE.md b/DESIGN-GATEHYGIENE.md
index 5ea9df93e4548fb73a7e78b38debd52d876ad658..e6ac34a10326fe8880eaa664690832da3875b9b8
--- a/DESIGN-GATEHYGIENE.md
+++ b/DESIGN-GATEHYGIENE.md
@@ -459,7 +459,21 @@
 
 ---
 
-## 10. Follow-up diff on `78c3e5391` — 2026-09-07, runtime validation pending
+## 10. Incoming timing lane on `78c3e5391` — 2026-09-07
+
+**Merged disposition:** the maintainer reports the incoming timing lane gated 344/344 with no
+SKIP rows. The account below records that lane's original implementation and offline validation;
+it is not a validation claim for the merged tree. Its dispatch wrapper/helper and timing evaluator
+are retired: repeatability checks did not replace the deleted manual ownership instrument. The
+configurable-window shrink samples and shrink/reclaim budgets are superseded by the derived-window
+held-burst liveness and old-generation reconfiguration witnesses in `tests/atomicwindow.py`.
+All those mechanism, exact-reply, bound, and credit-reclamation checks remain mandatory on every
+build. The original OFF RENAME four-roll hard-failure control is unchanged, and the 1.5-second
+promotion budget still requires `--release-build`. The merged ledger requires quick 325/full 342;
+the constants remain untouched. [MERGE.md](MERGE.md) records the complete conflict and row accounting.
+
+The rest of this section is historical lane evidence, including its now-superseded dispatch,
+shrink, admission-calibration, and ledger statements.
 
 This is a test-only diff in `cx-timing`. No server, benchmark, gate, or compiler was run. The
 earlier acceptance results above describe earlier changes, **not this diff**. The worked fixes
diff --git a/DESIGN-flipfp.md b/DESIGN-flipfp.md
index 32eb6f46c24fb688db6accc79537fbd664bd084c..f705b25837c72a011da71df06dec09d2757f05ae
--- a/DESIGN-flipfp.md
+++ b/DESIGN-flipfp.md
@@ -311,7 +311,9 @@
 detector. The signal and the floor cannot be read from one run -- with a learned band the trigger
 fires and `reset()` wipes the distance -- so each arm is run twice: `--flip-auto-band 90` holds the
 anchor and lets the distance converge (the signal), `--flip-auto-band -1` learns the floor.
-`scratch/flipfp/fpmargin.sh`.
+The former `scratch/flipfp/fpmargin.sh` implemented this two-band experiment. It is retired in the
+merged tree because neither pinning the band nor disabling its retriggers has a public control.
+These historical measurements remain evidence only for their recorded builds; see `MERGE.md`.
 
 **BITCOUNT -> INCR** (the redesign lane's reference change), 2:2 split, `--rate-limiting` pinned:
 
diff --git a/DESIGN.md b/DESIGN.md
index 0fba740d94ba1915868410de40bcb685ae675833..7ad3ac9fef512d8549eadfce44036b8a6a181e9f
--- a/DESIGN.md
+++ b/DESIGN.md
@@ -1,6 +1,6 @@
 # Thread amortization study surface
 
-`thread-mode` and `overlap` are one boot-latched measurement surface. The documented mode
+`thread-mode` and the study-only `x-overlap` are one boot-latched measurement surface. The documented mode
 names are `2s` (separate IO and executor threads, the default) and `1s` (one generalized thread owns
 both roles). `split` and `fused` remain parser aliases so old invocations keep working.
 
@@ -9,10 +9,10 @@
 | `2s` | ordinary split IO/ex loops | exact `t-iopipe` WB/IFID batch schedule | rejected at config validation |
 | `1s` | coarse IFID → EX → WB rotation | exact `t-genthread` `iofused` schedule | gated `iofused` three-way schedule |
 
-The compatibility options `thread-pipeline` and `genthread-schedule` remain accepted: the former
-is a numeric alias for `overlap`, while the latter maps `coarse`, `iofused`, and the legacy
-`streams` name to `1s` plus overlap 0, 1, and 2 respectively. The legacy name is only a parser
-alias; overlap 2 no longer runs the streams implementation. Overlap 2 emits a boot warning because
+Use `--x-overlap 0|1|2` for these study schedules and `--x-ex-sched 0|1` for the executor study.
+The old public schedule spellings and compatibility aliases are removed; see
+[DESIGN-KNOBS.md](DESIGN-KNOBS.md). Below, "overlap" names the internal schedule value.
+Overlap 2 emits a boot warning because
 it is an experimental research schedule. Unified overlap 1 and 2 require `net-io uring`; the split
 iopipe schedule retains its explicit epoll specialization. No schedule constant is runtime-tunable.
 
diff --git a/NOTES-HYGIENE.md b/NOTES-HYGIENE.md
index 23506bce64f00cb0dccaaff93362ccd30deab229..f224db07f4ceb2e4791c7cbb471b87d83a0a3e93
--- a/NOTES-HYGIENE.md
+++ b/NOTES-HYGIENE.md
@@ -352,7 +352,8 @@
 ## Test-file census
 
 The calibration examples expireindex.py, borrow_registry.py, and xshard_dispatch_scale.sh were
-already wired in the current parent. aof_frames.py is indirectly executed by aof_frame_order.py;
+already wired in this note's historical parent. The merged tree retires the dispatch instrument;
+the two surviving batteries use `--release-build`. aof_frames.py is indirectly executed by aof_frame_order.py;
 aof_rewrite.py and aof_rewrite_triggers.py are driven by matrix wrappers; differ.py is driven by
 differ_gate.sh; niclib.sh and gate_refs.txt support optional full-tier NIC rows.
 
@@ -456,7 +457,7 @@
 notify: ok (notify_events_fired=1608)
 ~~~
 
-Purpose-boot matrix commands:
+Historical purpose-boot matrix commands (superseded by the current gate's network-engine grammar):
 
 ~~~sh
 PERSIST_IO=normal GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_matrix.sh
@@ -465,7 +466,8 @@
 PERSIST_IO=uring  GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_trigger_matrix.sh
 ~~~
 
-Other purpose-booted battery invocations (each boot used the knobs documented by gate.sh):
+Historical purpose-booted battery invocations (not recipes for the merged tree; the dispatch
+instrument and script OFF arm below are retired):
 
 ~~~sh
 python3 tests/xscript.py 127.0.0.1 7600 off|limit|window
diff --git a/NOTES-XPERF2.md b/NOTES-XPERF2.md
index d4ce7950b1396997d44f0945f6f5179aa625e59e..d1edc822a5e7abc68e6aefc3d5a55986b0bf178e
--- a/NOTES-XPERF2.md
+++ b/NOTES-XPERF2.md
@@ -8,7 +8,7 @@
 |---|---|---|---|---|
 | E4 | ExpireIndex never shrinks; memsets the whole sidecar on live→0; single-pass rehash | **REPRODUCED** (both halves) | yes | `tests/expireindex.py` |
 | E5 | zero-copy borrow registry is a linear vector scanned per borrow/release/retire | **REPRODUCED** | yes | `tests/borrow_registry.py` |
-| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes | `tests/xshard_dispatch_scale.sh` |
+| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes, historically | retired manual-map dispatch instrument; see `MERGE.md` |
 | E7 | segment mode is absorbing: once triggered every later reply is a malloc + iovec slot | **NOT REPRODUCED** | no | none |
 
 Rig: EPYC 9754, cores 48–63 only, ports 7200–7209 only, loopback. Oracle = vanilla redis 7.4 at
@@ -292,18 +292,20 @@
 |---|---|---|---|---|
 | `tests/expireindex.py <host> <port>` | `--shards 1 --ratio 1:1 --enable-debug-command yes` | growth-trigger cost ratio over a 16× population ≤ 8.0; live→0 DEL ≤ 1.25× a neighbouring DEL | FAIL 2 (16.30×, 2.00×) | PASS (1.65×, 1.00×) |
 | `tests/borrow_registry.py <host> <port>` | `--shards 1 --zc-min 64 --client-output-buffer-limit normal 0 0 0 --enable-debug-command yes` | borrowed-GET per-op cost growth over 0 → ~2000 live borrows ≤ 1.05 | FAIL (1.078, 1.079) | PASS (1.008, 1.008, 1.009) |
-| `tests/xshard_dispatch_scale.sh` (+ `.py`) | boots its own 4-thread and 128-thread arms | dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
+| Retired `tests/xshard_dispatch_scale.sh` (+ `.py`) | historically booted fixed-owner 4-thread and 128-thread arms | historical dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
 
 Each also carries its own non-vacuity checks: `expireindex.py` asserts INFO `expires` really
 reached the population under test and that INFO `expired_keys` MOVED (so the sampler is proven to
 still visit deadlines parked mid-migration), plus a no-TTL negative control on both timing legs;
 `borrow_registry.py` asserts `CLIENT LIST oll` grew (the borrow path is proven live, not assumed),
 carries a non-borrowed control arm, and refuses to run unless the zc gate straddles its two probe
-sizes; `xshard_dispatch_scale.py` refuses to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` prove
+sizes; the retired `xshard_dispatch_scale.py` refused to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` proved
 the cross pair really fanned out to two owner threads and the control stayed on one.
 
-None of the three is wired into `tests/gate.sh`: they each need their own boot geometry and the
-gate's port/cores belong to the mainline operator. Run them as above.
+These were standalone instruments when measured. The merged gate runs the expiry and borrow
+batteries with `--release-build`. The dispatch wrapper and helper are retired because their manual
+ownership geometry is unavailable; the historical measurements above do not validate default
+round-robin ownership. See [MERGE.md](MERGE.md).
 
 ## Evidence
 
diff --git a/docs/flipctl-design.md b/docs/flipctl-design.md
index e6a5e31a6c05171b172746f2586a01d09fa1e84e..cd96557199b408c8b85bb42b77ca59ee6fec73ea
--- a/docs/flipctl-design.md
+++ b/docs/flipctl-design.md
@@ -8,8 +8,7 @@
 ## Ownership and footprint
 
 - `src/main.cc` is the controller cron owner. When `flip-auto=1`, the main/monitor thread calls
-  `Server::flipctl_tick()` once per existing LB tick (`lb-tick-ms`; if that mechanism is disabled,
-  the period derives from the provisioned thread count). No IO or EX loop runs controller policy.
+  `Server::flipctl_tick()` on its internal observation cadence. No IO or EX loop runs controller policy.
 - `src/core/flipctl.{h,cc}` contains the fingerprint representation, shift detector, and controller
   state machine. Controller vectors allocate only when `flip-auto=1`.
 - Each `ThreadCtx` embeds one `FlipFingerprintWriter`. It has no dynamic storage. Only the physical
@@ -46,9 +45,9 @@
 measurement quantum, avoiding the zero-anchor discontinuity where one rare admin argument would
 otherwise have distance one.
 
-With `flip-auto-band=-1`, the band is twice the maximum quiet adjacent/drift distance learned at
-the final split, floored by the four-family sampling quantum divided by observed commands. A
-numeric value is a percent. Zero disables fingerprint and collapse re-triggers. Final anchor
+The automatic band is twice the maximum quiet adjacent/drift distance learned at
+the final split, floored by the four-family sampling quantum divided by observed commands.
+It has no public override. Final anchor
 learning consumes a number of closed aggregates derived from the provisioned thread count. It is
 done after maneuver-only age sampling is disarmed, so the controller cannot detect its own signal
 cost transition as a workload change.
@@ -64,13 +63,13 @@
 falls below a threshold derived from its own change jitter and measurement quantum. Directional
 connection ramp-up therefore remains drift, while jittery stationary traffic can qualify even when
 its absolute rate never enters a quiet band. A 30-second ceiling, converted to ticks from
-`lb-tick-ms`, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
+the internal observation cadence, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
 thread per tick resets the learning/cap state, so an idle server remains in
 `awaiting-load-stability` and never pays for a maneuver it cannot use.
 
-`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. An explicit
-`lb-age-sample-rate` is used when nonzero; otherwise the rate derives from the provisioned thread
-count. IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
+`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. Its rate
+derives from observed command volume and the three-tick decision window, and adapts on completed
+traffic windows; see [DESIGN-KNOBS.md](../DESIGN-KNOBS.md). IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
 writes. IO releases its ROB-head age map when the rate returns to zero.
 
 After a stabilized baseline rate, `issue_initial_jump()` subtracts the per-thread maneuver window.
@@ -88,8 +87,8 @@
 not vetoed by idle peers. If either role has no measured work/busy quantity, measurement continues;
 the controller does not invent a workload prior.
 
-The calculation is performed in placement units (one logical CPU normally, one sibling pair under
-`smt-mode=1`) and then reported/committed in threads. The target passes `nio_eq` in the measured
+The calculation is performed in placement units (one logical CPU normally, one sibling pair when
+the allowed topology contains complete pairs) and then reported/committed in threads. The target passes `nio_eq` in the measured
 direction by `max(1, abs(nio_eq-nio_now)/2)`, clamped to one live unit of each role. Even when the
 equalized split rounds to the current split, the direction of greater measured demand supplies the
 deliberate one-unit overshoot.
@@ -143,7 +142,7 @@
 
 ## Integration and observability
 
-- `src/core/config.h` is the sole parser/default source for `flip-auto`, `flip-auto-band`, and
+- `src/core/config.h` is the sole parser/default source for `flip-auto` and
   `flip-work-window`; `src/cmd/t_server.cc` exposes them as immutable CONFIG values.
 - `tomokv.conf` documents the numeric grammar and off/auto semantics.
 - `INFO FLIPCTL` reports state/phase, anchor split/rate, bands, total/per-reason trigger counts, and
diff --git a/docs/specs/AUDIT-TLS.md b/docs/specs/AUDIT-TLS.md
index 0256e887f5d27c210d436bc219f3c7521ddad0db..87a2eaca3137a47f7c0e1567794650f57531860b
--- a/docs/specs/AUDIT-TLS.md
+++ b/docs/specs/AUDIT-TLS.md
@@ -3,7 +3,7 @@
 > **Where it landed (2026-09-04).** The feature shipped as `src/net/tls.{h,cc}` (this document
 > proposes `src/net/tls_ctx.h` + `src/net/tls_conn.h`); the test is `tests/tls.py` (no
 > `tests/gen-certs.sh`); no `tls-bio-size` knob exists. Defaults: AES-128-GCM-first cipher lists
-> and `tls-ktls yes` (`tomokv.conf`, NETWORK).
+> and automatic kTLS negotiation with userspace fallback (`tomokv.conf`, NETWORK).
 
 Governing audit. Read before writing any TLS code in `/home/user/Projects/tomokv-cpp-perthread`.
 No repository was modified in producing this.
diff --git a/scratchpad/rlbatch/lib.sh b/scratchpad/rlbatch/lib.sh
index 7bcf3e4e6030c79174bf35d9bb1b3dfb11f80c42..12184314d2aafac0dc7aa48db761a3bdec5de8c7
--- a/scratchpad/rlbatch/lib.sh
+++ b/scratchpad/rlbatch/lib.sh
@@ -17,7 +17,7 @@
 boot_srv(){ # boot_srv <binary> <logfile> [extra args...] -> SRV pid
   local bin="$1" log="$2"; shift 2
   guard_port "$PORT" || return 1
-  # EXTRA_ARGS lets a caller add boot flags (e.g. --read-local-interleave 0) without a second copy
+  # EXTRA_ARGS lets a caller add boot flags (e.g. --lb 0) without a second copy
   # of this function; word splitting is intentional.
   # shellcheck disable=SC2086
   taskset -c "$SRVCORE" "$bin" --port "$PORT" --bind 127.0.0.1 --shards 16 \
diff --git a/tests/flipctl_unit.cc b/tests/flipctl_unit.cc
index 540a6c40e9589572431075b623bc553c76ee8d85..33209258e2958771d60f86b919f6f510e9977737
--- a/tests/flipctl_unit.cc
+++ b/tests/flipctl_unit.cc
@@ -184,11 +184,11 @@
 
     // THE TYPED BAND IS A FLOOR, NOT A CEILING. The gate-hygiene lane's instrumented failure: a
     // driver holding its rate to 0.07% across 34 samples, a fingerprint distance of 0.2518 against
-    // a flat --flip-auto-band 2 (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
+    // a flat explicit band of 2% (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
     // A detector whose signature genuinely moves between adjacent quiet windows must end up with a
     // band above that movement whatever the operator typed, and must still fire on a real change.
     {
-        FlipShiftDetector typed(2, 4);   // --flip-auto-band 2
+        FlipShiftDetector typed(2, 4);   // internal explicit-band control: 2%
         // Stationary but NOISY: the read/write split alternates 35:65 / 65:35 window to window.
         // The detector smooths the signature, so the anchored distance this produces is ~0.09.
         const auto noisy = [](int k) { return quiet(k % 2 ? 65 : 35, k % 2 ? 35 : 65); };
@@ -250,8 +250,8 @@
         if (fires) fail("the learned band fired on quiet-state movement");
     }
     // A TYPED BAND UNDER THE REAL SAMPLER. The shipped configuration cannot reach this -- the
-    // controller runs the learned band -- but an operator who sets --flip-auto-band AND the sampler
-    // can, and on a tree where the typed path bypasses the floor the fingerprint lane measured 73
+    // controller runs the learned band. This internal explicit-band control preserves the earlier
+    // regression: where the typed path bypassed the floor the fingerprint lane measured 73
     // two-consecutive exceedances in 600 stationary windows at K=100 and 208 at K=60: a spurious
     // maneuver every few seconds on a load that never changed. Driven here through THEIR writer, a
     // stationary 50/50 stream at depth 32, with the floor in place: none, and a real mix change
@@ -259,7 +259,7 @@
     {
         for (uint32_t window : {60u, 100u}) {
             XorShift rng;
-            FlipShiftDetector typed(2, 8);   // --flip-auto-band 2, the operator's knob
+            FlipShiftDetector typed(2, 8);   // internal explicit-band control: 2%
             int fires = 0, confirmed = 0, streak = 0;
             for (int w = 0; w < 60; w++) {
                 FlipFingerprintWriter writer;
diff --git a/tests/rlcache_churn.py b/tests/rlcache_churn.py
index b56ae4041ae0843dc67443f16017270063853124..f8a2c0730bb0a4fb6c3b5f6282c547226bb76599
--- a/tests/rlcache_churn.py
+++ b/tests/rlcache_churn.py
@@ -59,7 +59,7 @@
 
 
 # THE LOAD BALANCER MUST HAVE SOMETHING TO BALANCE. A uniform key distribution over one small key
-# space keeps every shard at the same op rate, the imbalance never crosses lb-imbalance-pct, and no
+# space keeps every shard at the same op rate, the imbalance never crosses the learned band, and no
 # shard ever changes owner -- which is the precondition for the defect this battery exists for
 # (measured: 7.8M ops, 0 migrations). A hot window that ROTATES makes a few shards hot, then makes
 # different ones hot, so the balancer moves shards continuously and in both directions, which is

exec
/bin/bash -lc "python3 - <<'PY'
from pathlib import Path
import json, hashlib, subprocess
manifest = json.loads(Path('.merge-inputs/manifest.json').read_text())
paths = sorted(set().union(*(set(m) for m in manifest['lanes'].values())))
rows = []
for name in paths:
    lanes = [lane for lane in ('timing', 'knobs') if name in manifest['lanes'][lane]]
    if name in ('tests/atomic_torn.py', 'tests/gate.sh'):
        disposition = 'Both merged; every overlap is accounted for above.'
    elif name == 'tests/xshard_dispatch_scale.sh':
        disposition = 'Knobs deletion retained; timing rewrite superseded by retirement.'
    elif name == 'tests/xshard_dispatch_scale.py':
        disposition = 'Deleted with its sole wrapper; timing payload/ownership hunks retired.'
    elif name == 'DESIGN-GATEHYGIENE.md':
        disposition = 'All incoming body text retained; historical scope and merged status added; section heading updated.'
    elif name == 'DESIGN-KNOBS.md':
        disposition = 'All incoming text retained; added provenance note pointing to merged line arithmetic.'
    elif name == 'tests/rlcache_churn.py':
        disposition = 'Knobs exact except obsolete threshold-name comment now says learned band.'
    else:
        lane = lanes[0]
        actual = hashlib.sha256(Path(name).read_bytes()).hexdigest()
        assert actual == manifest['lanes'][lane][name], name
        disposition = lane.capitalize() + ' exact: every byte/hunk retained.'
    rows.append('| "'`'"' + name + '"'` | '"' + ' + '.join(lanes) + ' | ' + disposition + ' |')
historical = json.loads(Path('.merge-inputs/historical-docs.json').read_text())
scratch = json.loads(Path('.merge-inputs/scratch-adjustments.json').read_text())
content = '''# Merge record — 2026-09-07

Both incoming lanes are merged into this working tree from common base
"'`78c3e53919a5d557bd3dd4625c9524d0baa212e4`. The dispatch-scaling retirement wins;
the timing rewrite cannot restore the deleted ownership instrument. The OFF RENAME
four-roll hard-failure control and the surviving mechanism/release-timing split both remain.
The merged gate needs **quick 325 / full 342**, with the expected-count constants untouched.

No build, server, benchmark, gate, C++ compiler, or server-connected battery was run.
The maintainer'"'s 344/344, zero-SKIP result belongs to the incoming timing lane, not this merge.
Both source worktrees were read only. Their tracked-file inventories and file hashes were
rechecked after integration and were unchanged. Their untracked "'`CODEX-OUT.md` run transcripts
were not imported; the pre-existing transcript in this worktree was left alone.

Inputs: 12 timing paths plus 40 knobs paths, overlapping on the three named conflict files,
for **49 distinct input paths**, including the knobs lane'"'s new files and wrapper deletion.
The SHA-256 digests of the captured "'`git diff HEAD --binary` inputs were:

- timing: `5310bed7954cc01a061bcb5c20e18b6b9f8096dfadbef7b94fb871fcfea4d3a9`
- knobs: `cb8a21fa050e9961dd1f3bed442d5de5f3b0df1e34e03fd183a622ad73ed3084`

## Conflict decisions

| File | Timing intent | Knobs intent | Decision and reason |
| --- | --- | --- | --- |
| `tests/atomic_torn.py` | Require counters, exact values, and safety on every build; score short promotion and shrink/reclaim budgets only with `--release-build`; strengthen credit sampling. | Remove configurable-window calls and exercise the derived production window, including a separate live-reconfiguration witness. | Retain timing argument parsing, `release_note`, required promotion counters, monotonic watchdog, unconditional hold/predecessor/promotion checks, and the 1.5-second release promotion budget. Use the knobs admission/reconfiguration block and its helper intact; the removed resize experiment cannot be run. The exact superseded hunks are listed below. The base RENAME control remains unchanged. |
| `tests/gate.sh` | Pass release identity to release atomic, stream, expiry, borrow, and dispatch arms; omit it for ASAN atomic. | Migrate all boots and CONFIG/INFO checks to the reduced grammar; replace the script bounds geometry and persistence surface check; retire dispatch. | Keep every knobs boot/surface change and every surviving timing call-site change. Only dispatch'"'s invocation/flag and its live-instrument commentary are superseded by retirement. Correct the ASAN comment to describe the surviving promotion budget and mandatory derived-window witnesses. Neither EXPECT assignment changes. |
| "'`tests/xshard_dispatch_scale.sh` | Fix best-pair selection, freeze balancing, check each pair and repeatability, alternate arm order, and score all qualified release ratios against 1.20. | Delete the wrapper because its manual shard map is removed. | Delete. The rewrite still passes the removed manual-map flag. Its threshold qualification does not supply an equivalent geometry. Delete the Python helper too, rather than leave the retired instrument'"'s code behind. |

### Semantic ruling and evidence

**No: the timing rewrite does not make dispatch scaling valid without "'`--shard-home`.**
The incoming timing wrapper still constructs `HOMES` as `sid -> 2 + sid % 2` at line 62
and passes it to the server at lines 81–82, along with independently disabled key/client
balancing and disabled flip. The first two executors own all 16 shards at both thread counts;
extra executor threads are deliberately empty filler threads, pinned away from the real owners.
This is the experimental control, not merely a spelling of a default.

In the merged source, `Placement::assign_shard_homes` assigns
`shard_home_[sid] = ex_[sid % ex_.size()]` (`src/core/placement.h:176–180`). The old 4-thread
arm has two executors; the 128-thread arm has 126. Without the manual map, the 16 shards in
the big arm occupy its first 16 executors, including 14 formerly empty filler executors.
Actual work, ownership, and contention change along with the array/thread-count dimension.
Changing the obsolete balancing flags to `--lb 0` would freeze this different geometry; it
would not restore the original one.

The timing rewrite'"'s 2% test uses within-count cross/same cost spreads and same-shard controls
across counts (wrapper lines 133–145). Its final assertion requires every qualified ratio to
meet the unchanged 1.20 limit (line 164). These are repeatability checks, not an ownership
construction or validation of the limit on a new geometry. Its helper's distinct "'`x`/`y`
replies and end-of-arm owner-map comparison (Python lines 103–107 and 123–124) prove reply
content and within-arm stability, not equivalent work placement across thread counts.
Even perfectly repeatable arms could therefore assert the wrong claim.

The knobs lane'"'s OBJECTIONS item 5 is accepted. The gate row, shell wrapper, Python helper,
and all of the timing lane's dispatch-specific implementation are retired. The rewrite is
**moot**, not partly retained as an unwired timing check. Historical measurements remain
explicitly historical; this merge makes no new dispatch-scaling claim.

### OFF RENAME and the atomic timing split

The complete base block from “Broadened movers: RENAME” through the following store-family
section is byte-for-byte preserved. It still runs "'`for _roll in range(4)` at
`tests/atomic_torn.py:771`, arms and clears the hop delay in `try/finally`, retries only a
clean miss, and stops on a witnessed tear, worker error, or surviving helper. The OFF assertion
at line 782 requires positive invalid/read counts and no errors/live helpers. Four clean
misses fail; there is no OFF skip and no release-build condition around this control.
The base comment describing that policy is retained too. Each `rename_hammer` invocation
retains the base'"'s state reinitialization and new connections/helpers.

There is no interaction requiring a weaker RENAME policy: it witnesses a detector, whereas
"'`release_note` scores the separate promotion completion budget. Missing hold, predecessor,
or promotion counters still fail on both build types. The latter retains its 30-second
mechanism watchdog and the original separate 1.5-second release budget (`:721`).

The three textual overlaps in the old admission/lease region are resolved together:

- The one-credit pipeline setup and the timing lane'"'s required stall-counter reads are
  superseded by "'`held_burst(..., whole_window=True)` (`:900`). Its direct INFO indexing
  requires every counter to exist, and a positive held stall delta is mandatory.
- The old `lease_writer`, completed-write counters, resize sequence 31/7/19/3, 32 observations
  at bound 3, and exact final pool 3 are superseded by the second fresh held burst with
  `reconfigure=True` (`:908`). Live `CONFIG SET atomic 1` rebuilds generations; the test
  proves pre-CONFIG groups survived, observes the fixed bound and zero debt, checks every
  reply, and requires zero live groups/debt plus the entire derived credit pool after drain.
- The release-only three-second **shrink/reclaim** budgets and their measurements are dropped
  with that resize experiment. No shrink exists, and a timing claim measured under the old
  one-/three-credit setup is not transplanted to a held production-window burst. Reclamation
  itself is retained as mandatory mechanism coverage. The new helper'"'s arm, resume, and
  drain deadlines are bounded failure watchdogs on every build, not successful timing skips.

"'`tests/atomicwindow.py` is otherwise exactly the knobs input. It clears the artificial hold
before starting the completion watchdog, retries only a joined/reclaimed witness miss on
fresh state, and fails after four misses. Bad replies, excess groups, debt, lost credits,
failed resume, and surviving helpers cannot be retried into success. Both separate outer
atomic battery invocations remain. The RENAME hit-rate evidence is not reused to claim a
hit probability for this different witness. Other base controls and timing changes are unchanged.

## EXPECT arithmetic, by emitting line

The actual quick exit is **base line 1258**, **merged line 1246**:
`if [ "$TIER" = quick ]; then`. The incoming design notes'"' other line numbers describe
older intermediate text; in particular, their “old exit 1260” is not the supplied base.
The following line numbers were read from the supplied base and final merged file.

| Ledger change | Base emitting line | Merged emitting line | Side of quick exit | Quick delta | Full delta |
| --- | ---: | ---: | --- | ---: | ---: |
| Retire "'`xscript off control`; former three-arm loop becomes `limit window` | 658, once per old arm | 653, twice for the surviving arms | Before both exits | -1 | -1 |
| Retire `cross-shard dispatch scaling` | 686 | none; retirement comment at 676 emits nothing | Before base exit | -1 | -1 |
| Retire old persistence CONFIG surface label, four iterations | 981 | none | Before base exit | -4 | -4 |
| Replacement `configuration reduction + actual geometry`, same two engines × two atomic settings | none | 969, four iterations | Before merged exit | +4 | +4 |
| Atomic admission/reconfiguration internals; outer release battery retained | 435 | 435 | Before both exits | 0 | 0 |
| Atomic admission/reconfiguration internals; outer ASAN battery retained | 1270 | 1260 | After both exits | 0 | 0 |
| All other outer battery rows and loops, including release-flag wiring | existing | retained | Same side of exit | 0 | 0 |

**Quick: `327 - 1 - 1 - 4 + 4 = 325`. Full: `344 - 1 - 1 - 4 + 4 = 342`.**
The knobs lane'"'s numeric claim is confirmed. With the unchanged optional NIC row, full is 343.
Renaming engine labels from normal/uring to epoll/uring preserves two iterations; it does not
add or retire persistence coverage. The new Python helpers add internal checks to existing
rows, not ledger entries. The scratch experiments retired below have no emitting line in
"'`gate.sh`, so contribute zero to both tiers.

`EXPECT_QUICK=327` at line 141 and `EXPECT_FULL=344` at line 142 are byte-for-byte unchanged.
**The maintainer must update them to 325 and 342.** The ledger mismatch is intentional until
that edit; this merge does not make a failing suite appear green by changing its expectations.
Static comparison of all success-emitting labels, with the documented replacement and engine
label normalization, confirmed no other row was lost or added; the changed loop multiplicity
was checked separately.

## Complete incoming-file accounting

“Exact” means a full-file byte comparison against the captured working-tree input, so it covers
every incoming hunk, including additions. All production files under `src/` equal the knobs
lane byte-for-byte. All non-exact input files have a specific disposition in this table.

| Input path | Lane | Disposition |
| --- | --- | --- |
'"'''
content += '\\n'.join(rows)
content += '''

## Additional consistency edits and explicit drops

The reduced grammar also invalidated tracked callers and documentation outside the lanes'
file lists. These edits are part of making the requested merged tree consistent, not new
server features or new performance evidence.

- **Removed incoming timing work:** both dispatch files and the wrapper's release flag,
  threshold/evaluator, pair ordering, balancing freeze, exact payloads, and ownership
  stability checks. Reason: the entire instrument is retired, as ruled above.
- **Superseded incoming timing work:** all old configurable-admission and resize/lease
  sampling hunks, their associated three-second release budgets, and their old live/ledger
  descriptions. Reason: the public resize control is gone; the retained helper provides
  derived-bound liveness, old-generation reconfiguration, exact replies, and credit safety.
  No promotion, RENAME, blocking, borrow, expiry, INFO, pubsub, slowlog, or stream hunk is lost.
- **Knobs dispositions retained:** every deleted control, the "'`--lb` collapse, derived
  defaults, study spellings, Config 528 assertion, parser rejection coverage, and all new
  helper files. `DESIGN-KNOBS.md` retains its entire incoming body and all objections; a
  provenance note distinguishes its line references from this merged ledger.
- **Retired additional historical instruments:** `scratch/flipfp/fpmargin.sh` is removed
  because its signal/floor experiment required two independently selected bands, including
  a pinned 90% band. The S1b commands in `scratch/flipfp/chain.sh` are removed because their
  zero-band controller-cost control disabled retriggers. Replacing either with automatic
  bands would silently change its claim. These were outside the gate. Historical S1b result
  files remain readable by the report code and are labeled as a retired experiment.
- **Migrated other old boot recipes:** removed fixed-filter, explicit-band, and age-sampling
  flags where the scripts run the surviving battery or default automatic controller. Each
  changed driver states that its historical PRE/POST results need fresh validation. No
  runtime result is inferred from these grammar edits. The exact driver/report list is below.
- **Documentation:** updated current controls in `DESIGN.md`, `docs/flipctl-design.md`, and
  `docs/specs/AUDIT-TLS.md`; explicitly retired dispatch references in `NOTES-XPERF2.md`,
  `NOTES-HYGIENE.md`, and `AUDIT-TESTS.md`; recorded the margin retirement in
  `DESIGN-flipfp.md`; added current merged dispositions to `DESIGN-GATEHYGIENE.md` while
  retaining the incoming evidence. Historical research documents listed below are marked as
  descriptions of earlier revisions, not live configuration recipes.
- **Comments only:** `tests/flipctl_unit.cc` now describes the retained internal explicit-band
  test API, `tests/rlcache_churn.py` names the learned threshold, and
  `scratchpad/rlbatch/lib.sh` uses a surviving boot-flag example. No test behavior changed in
  these three edits.
- **Run transcripts:** neither lane'"'s untracked "'`CODEX-OUT.md` was imported, because it is
  tooling output rather than a source change and would overwrite this worktree'"'s own log.

Deleted knob names remain only where needed to assert their rejection, to name an unchanged
mechanism/counter, or to describe explicitly historical/deleted interfaces. In particular,
"'`config_parser_test.cc`, `knobs.py`, `bplus.py`, and `tls.py` deliberately require removed
names to be absent/rejected; deleting these references would drop the knobs lane'"'s negative
coverage. No live boot/configuration recipe depends on an accepted deleted flag. Internal
ownership maps, atomic-window counters, and prefetch implementation names are mechanisms,
not restored public configuration aliases.

Tracked research drivers/reports adjusted:

'''
content += '\\n'.join('- "'`'"' + name + '"'`'"' for name in scratch)
content += '\\n\\nHistorical research documents explicitly scoped to their recorded revisions:\\n\\n'
content += '\\n'.join('- "'`'"' + name + '"'`'"' for name in historical)
content += '''

## Correctness-law review and validation limits

The Config exception is the task's explicit **624 -> 528** change. The retained assertion
accounts for "'`624 - 104 + 4 + 4 = 528`. Declared layout locks remain Op 336, Client 1984,
ThreadCtx 1408, Shard 1440, FlatStore 944, Rob<64> 192, and AtomicEntry 144. This is source
inspection, not a compiled size measurement.

No production source changes were introduced while resolving the lanes: `src/` equals the
knobs input, and timing is test-only. Ownership transfer/rebinding, immutable replacement,
read-local arming, QSBR, and RYOW code therefore receive no extra merge mutation. No new
correctness-law violation caused by combining the lanes was found in this review. This does
not claim either thread mode has been booted here or that a gate/regression result was obtained.

**Inherited objection retained:** `prepare_captured_local_mget` still sets `kAttempts=2`
at `src/core/ex_loop.h:1295`, loops at line 1317, and increments
`mget_generation_retries` at line 1432. This repeats a per-operation reader attempt and
conflicts with the stated no-reader-retries law. The path already existed at the common
ancestor and is explicitly called out in the knobs lane'"'s OBJECTIONS item 1; the merge does
not hide it or claim to fix it. The ordinary "'`prepare_local_mget` two-attempt loop also remains
in the inherited source. No new retry/seqlock scheme or layout split was introduced.

The remaining objections are preserved too: actual legacy-alias uses, unmeasured LB/shard
sampling and transfer policies, stricter automatic SMT geometry, the retired manual-map
instrument, and the longer real LRU clock waits. The knobs lane'"'s held-window repair still
needs maintainer release/ASAN validation at 16 shards, ratio 6:2, cores 0–7. Its five-second
arming watchdog is a bounded discovery budget, not a measured false-failure probability.

Completed server-free checks:

- Python AST parsing for all 21 affected Python files and "'`bash -n` for all 20 surviving
  affected shell files. No battery entry point was executed.
- Ten executions of only the source-extracted OFF RENAME control with fake hammer/debug
  callbacks: first/fourth-attempt hits, four clean misses, worker errors, and live helpers,
  each under both build identities. Misses failed after four attempts; errors/live helpers
  stopped retries; every armed delay was cleared. No sockets, threads, or server were used.
- Exact base comparison of the entire RENAME block; source inspection of unconditional
  derived-window callers and the surviving promotion release split.
- Input-path/hash accounting, full-file comparisons for retained lane files, static gate-row
  comparison and loop arithmetic, unchanged EXPECT assignments, layout-assertion inspection,
  removed-flag/caller search, conflict-marker search, and `git diff --check`.

No C++ syntax check, build, server, benchmark, full test, or gate result is claimed. The
maintainer supplies the merged runtime and performance verdict.
'"'''
Path('MERGE.md').write_text(content)
print('Wrote MERGE.md:', len(content.splitlines()), 'lines; accounted for', len(rows), 'distinct input paths.')
PY" in /home/user/Projects/cx-merge
 succeeded in 0ms:
Wrote MERGE.md: 328 lines; accounted for 49 distinct input paths.

diff --git a/AUDIT-TESTS.md b/AUDIT-TESTS.md
index e9cd43ffbc376172717531d0037de1c79fd23690..fe1fd0e2c39e965f605e4a30894a545129541534
--- a/AUDIT-TESTS.md
+++ b/AUDIT-TESTS.md
@@ -122,7 +122,7 @@
 | MM floor (memtier 20 s, sampled 8+6) | 1 | 14 | measurement, keep |
 | saturated-flip memtier | 1 | 25+3 | measurement, keep |
 | `flipctl.py --stable-seconds 30` (+ramp/idle/surge, timeout 300) | 1 | 60-120 est. | measurement, keep |
-| `xshard_dispatch_scale.sh` (2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 | 90-150 est. | measurement, keep |
+| Retired `xshard_dispatch_scale.sh` (historical 2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 historically, 0 now | 90-150 est. historically | manual ownership instrument removed; see `MERGE.md` |
 | `atomic_torn.py` hammers (3+2.5+2+2.5 s) + OFF re-rolls (up to 14 s) + RENAMENX/COPY parks (2 x 64 x 100 ms = 12.8 s) + window/lease/churn (~6 s) | 1 quick, +1 ASAN | ~45-60 each | re-rolls are dead time on this kernel (F6) |
 | `spinprobe.py` (6 s baseline + 6 s probe) | 3 | 37.5 | measurement, keep |
 | Python batteries' literal sleeps (census: flipctl 17 s, limits 5.8, edgetime 4.5, atomic_ryow 4, hexpire 3.4 — most run x2) | | ~60 | keep |
diff --git a/DESIGN-GATEHYGIENE.md b/DESIGN-GATEHYGIENE.md
index 5ea9df93e4548fb73a7e78b38debd52d876ad658..e6ac34a10326fe8880eaa664690832da3875b9b8
--- a/DESIGN-GATEHYGIENE.md
+++ b/DESIGN-GATEHYGIENE.md
@@ -459,7 +459,21 @@
 
 ---
 
-## 10. Follow-up diff on `78c3e5391` — 2026-09-07, runtime validation pending
+## 10. Incoming timing lane on `78c3e5391` — 2026-09-07
+
+**Merged disposition:** the maintainer reports the incoming timing lane gated 344/344 with no
+SKIP rows. The account below records that lane's original implementation and offline validation;
+it is not a validation claim for the merged tree. Its dispatch wrapper/helper and timing evaluator
+are retired: repeatability checks did not replace the deleted manual ownership instrument. The
+configurable-window shrink samples and shrink/reclaim budgets are superseded by the derived-window
+held-burst liveness and old-generation reconfiguration witnesses in `tests/atomicwindow.py`.
+All those mechanism, exact-reply, bound, and credit-reclamation checks remain mandatory on every
+build. The original OFF RENAME four-roll hard-failure control is unchanged, and the 1.5-second
+promotion budget still requires `--release-build`. The merged ledger requires quick 325/full 342;
+the constants remain untouched. [MERGE.md](MERGE.md) records the complete conflict and row accounting.
+
+The rest of this section is historical lane evidence, including its now-superseded dispatch,
+shrink, admission-calibration, and ledger statements.
 
 This is a test-only diff in `cx-timing`. No server, benchmark, gate, or compiler was run. The
 earlier acceptance results above describe earlier changes, **not this diff**. The worked fixes
diff --git a/DESIGN-flipfp.md b/DESIGN-flipfp.md
index 32eb6f46c24fb688db6accc79537fbd664bd084c..f705b25837c72a011da71df06dec09d2757f05ae
--- a/DESIGN-flipfp.md
+++ b/DESIGN-flipfp.md
@@ -311,7 +311,9 @@
 detector. The signal and the floor cannot be read from one run -- with a learned band the trigger
 fires and `reset()` wipes the distance -- so each arm is run twice: `--flip-auto-band 90` holds the
 anchor and lets the distance converge (the signal), `--flip-auto-band -1` learns the floor.
-`scratch/flipfp/fpmargin.sh`.
+The former `scratch/flipfp/fpmargin.sh` implemented this two-band experiment. It is retired in the
+merged tree because neither pinning the band nor disabling its retriggers has a public control.
+These historical measurements remain evidence only for their recorded builds; see `MERGE.md`.
 
 **BITCOUNT -> INCR** (the redesign lane's reference change), 2:2 split, `--rate-limiting` pinned:
 
diff --git a/DESIGN.md b/DESIGN.md
index 0fba740d94ba1915868410de40bcb685ae675833..7ad3ac9fef512d8549eadfce44036b8a6a181e9f
--- a/DESIGN.md
+++ b/DESIGN.md
@@ -1,6 +1,6 @@
 # Thread amortization study surface
 
-`thread-mode` and `overlap` are one boot-latched measurement surface. The documented mode
+`thread-mode` and the study-only `x-overlap` are one boot-latched measurement surface. The documented mode
 names are `2s` (separate IO and executor threads, the default) and `1s` (one generalized thread owns
 both roles). `split` and `fused` remain parser aliases so old invocations keep working.
 
@@ -9,10 +9,10 @@
 | `2s` | ordinary split IO/ex loops | exact `t-iopipe` WB/IFID batch schedule | rejected at config validation |
 | `1s` | coarse IFID → EX → WB rotation | exact `t-genthread` `iofused` schedule | gated `iofused` three-way schedule |
 
-The compatibility options `thread-pipeline` and `genthread-schedule` remain accepted: the former
-is a numeric alias for `overlap`, while the latter maps `coarse`, `iofused`, and the legacy
-`streams` name to `1s` plus overlap 0, 1, and 2 respectively. The legacy name is only a parser
-alias; overlap 2 no longer runs the streams implementation. Overlap 2 emits a boot warning because
+Use `--x-overlap 0|1|2` for these study schedules and `--x-ex-sched 0|1` for the executor study.
+The old public schedule spellings and compatibility aliases are removed; see
+[DESIGN-KNOBS.md](DESIGN-KNOBS.md). Below, "overlap" names the internal schedule value.
+Overlap 2 emits a boot warning because
 it is an experimental research schedule. Unified overlap 1 and 2 require `net-io uring`; the split
 iopipe schedule retains its explicit epoll specialization. No schedule constant is runtime-tunable.
 
diff --git a/NOTES-HYGIENE.md b/NOTES-HYGIENE.md
index 23506bce64f00cb0dccaaff93362ccd30deab229..f224db07f4ceb2e4791c7cbb471b87d83a0a3e93
--- a/NOTES-HYGIENE.md
+++ b/NOTES-HYGIENE.md
@@ -352,7 +352,8 @@
 ## Test-file census
 
 The calibration examples expireindex.py, borrow_registry.py, and xshard_dispatch_scale.sh were
-already wired in the current parent. aof_frames.py is indirectly executed by aof_frame_order.py;
+already wired in this note's historical parent. The merged tree retires the dispatch instrument;
+the two surviving batteries use `--release-build`. aof_frames.py is indirectly executed by aof_frame_order.py;
 aof_rewrite.py and aof_rewrite_triggers.py are driven by matrix wrappers; differ.py is driven by
 differ_gate.sh; niclib.sh and gate_refs.txt support optional full-tier NIC rows.
 
@@ -456,7 +457,7 @@
 notify: ok (notify_events_fired=1608)
 ~~~
 
-Purpose-boot matrix commands:
+Historical purpose-boot matrix commands (superseded by the current gate's network-engine grammar):
 
 ~~~sh
 PERSIST_IO=normal GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_matrix.sh
@@ -465,7 +466,8 @@
 PERSIST_IO=uring  GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_trigger_matrix.sh
 ~~~
 
-Other purpose-booted battery invocations (each boot used the knobs documented by gate.sh):
+Historical purpose-booted battery invocations (not recipes for the merged tree; the dispatch
+instrument and script OFF arm below are retired):
 
 ~~~sh
 python3 tests/xscript.py 127.0.0.1 7600 off|limit|window
diff --git a/NOTES-XPERF2.md b/NOTES-XPERF2.md
index d4ce7950b1396997d44f0945f6f5179aa625e59e..d1edc822a5e7abc68e6aefc3d5a55986b0bf178e
--- a/NOTES-XPERF2.md
+++ b/NOTES-XPERF2.md
@@ -8,7 +8,7 @@
 |---|---|---|---|---|
 | E4 | ExpireIndex never shrinks; memsets the whole sidecar on live→0; single-pass rehash | **REPRODUCED** (both halves) | yes | `tests/expireindex.py` |
 | E5 | zero-copy borrow registry is a linear vector scanned per borrow/release/retire | **REPRODUCED** | yes | `tests/borrow_registry.py` |
-| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes | `tests/xshard_dispatch_scale.sh` |
+| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes, historically | retired manual-map dispatch instrument; see `MERGE.md` |
 | E7 | segment mode is absorbing: once triggered every later reply is a malloc + iovec slot | **NOT REPRODUCED** | no | none |
 
 Rig: EPYC 9754, cores 48–63 only, ports 7200–7209 only, loopback. Oracle = vanilla redis 7.4 at
@@ -292,18 +292,20 @@
 |---|---|---|---|---|
 | `tests/expireindex.py <host> <port>` | `--shards 1 --ratio 1:1 --enable-debug-command yes` | growth-trigger cost ratio over a 16× population ≤ 8.0; live→0 DEL ≤ 1.25× a neighbouring DEL | FAIL 2 (16.30×, 2.00×) | PASS (1.65×, 1.00×) |
 | `tests/borrow_registry.py <host> <port>` | `--shards 1 --zc-min 64 --client-output-buffer-limit normal 0 0 0 --enable-debug-command yes` | borrowed-GET per-op cost growth over 0 → ~2000 live borrows ≤ 1.05 | FAIL (1.078, 1.079) | PASS (1.008, 1.008, 1.009) |
-| `tests/xshard_dispatch_scale.sh` (+ `.py`) | boots its own 4-thread and 128-thread arms | dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
+| Retired `tests/xshard_dispatch_scale.sh` (+ `.py`) | historically booted fixed-owner 4-thread and 128-thread arms | historical dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
 
 Each also carries its own non-vacuity checks: `expireindex.py` asserts INFO `expires` really
 reached the population under test and that INFO `expired_keys` MOVED (so the sampler is proven to
 still visit deadlines parked mid-migration), plus a no-TTL negative control on both timing legs;
 `borrow_registry.py` asserts `CLIENT LIST oll` grew (the borrow path is proven live, not assumed),
 carries a non-borrowed control arm, and refuses to run unless the zc gate straddles its two probe
-sizes; `xshard_dispatch_scale.py` refuses to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` prove
+sizes; the retired `xshard_dispatch_scale.py` refused to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` proved
 the cross pair really fanned out to two owner threads and the control stayed on one.
 
-None of the three is wired into `tests/gate.sh`: they each need their own boot geometry and the
-gate's port/cores belong to the mainline operator. Run them as above.
+These were standalone instruments when measured. The merged gate runs the expiry and borrow
+batteries with `--release-build`. The dispatch wrapper and helper are retired because their manual
+ownership geometry is unavailable; the historical measurements above do not validate default
+round-robin ownership. See [MERGE.md](MERGE.md).
 
 ## Evidence
 
diff --git a/docs/flipctl-design.md b/docs/flipctl-design.md
index e6a5e31a6c05171b172746f2586a01d09fa1e84e..cd96557199b408c8b85bb42b77ca59ee6fec73ea
--- a/docs/flipctl-design.md
+++ b/docs/flipctl-design.md
@@ -8,8 +8,7 @@
 ## Ownership and footprint
 
 - `src/main.cc` is the controller cron owner. When `flip-auto=1`, the main/monitor thread calls
-  `Server::flipctl_tick()` once per existing LB tick (`lb-tick-ms`; if that mechanism is disabled,
-  the period derives from the provisioned thread count). No IO or EX loop runs controller policy.
+  `Server::flipctl_tick()` on its internal observation cadence. No IO or EX loop runs controller policy.
 - `src/core/flipctl.{h,cc}` contains the fingerprint representation, shift detector, and controller
   state machine. Controller vectors allocate only when `flip-auto=1`.
 - Each `ThreadCtx` embeds one `FlipFingerprintWriter`. It has no dynamic storage. Only the physical
@@ -46,9 +45,9 @@
 measurement quantum, avoiding the zero-anchor discontinuity where one rare admin argument would
 otherwise have distance one.
 
-With `flip-auto-band=-1`, the band is twice the maximum quiet adjacent/drift distance learned at
-the final split, floored by the four-family sampling quantum divided by observed commands. A
-numeric value is a percent. Zero disables fingerprint and collapse re-triggers. Final anchor
+The automatic band is twice the maximum quiet adjacent/drift distance learned at
+the final split, floored by the four-family sampling quantum divided by observed commands.
+It has no public override. Final anchor
 learning consumes a number of closed aggregates derived from the provisioned thread count. It is
 done after maneuver-only age sampling is disarmed, so the controller cannot detect its own signal
 cost transition as a workload change.
@@ -64,13 +63,13 @@
 falls below a threshold derived from its own change jitter and measurement quantum. Directional
 connection ramp-up therefore remains drift, while jittery stationary traffic can qualify even when
 its absolute rate never enters a quiet band. A 30-second ceiling, converted to ticks from
-`lb-tick-ms`, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
+the internal observation cadence, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
 thread per tick resets the learning/cap state, so an idle server remains in
 `awaiting-load-stability` and never pays for a maneuver it cannot use.
 
-`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. An explicit
-`lb-age-sample-rate` is used when nonzero; otherwise the rate derives from the provisioned thread
-count. IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
+`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. Its rate
+derives from observed command volume and the three-tick decision window, and adapts on completed
+traffic windows; see [DESIGN-KNOBS.md](../DESIGN-KNOBS.md). IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
 writes. IO releases its ROB-head age map when the rate returns to zero.
 
 After a stabilized baseline rate, `issue_initial_jump()` subtracts the per-thread maneuver window.
@@ -88,8 +87,8 @@
 not vetoed by idle peers. If either role has no measured work/busy quantity, measurement continues;
 the controller does not invent a workload prior.
 
-The calculation is performed in placement units (one logical CPU normally, one sibling pair under
-`smt-mode=1`) and then reported/committed in threads. The target passes `nio_eq` in the measured
+The calculation is performed in placement units (one logical CPU normally, one sibling pair when
+the allowed topology contains complete pairs) and then reported/committed in threads. The target passes `nio_eq` in the measured
 direction by `max(1, abs(nio_eq-nio_now)/2)`, clamped to one live unit of each role. Even when the
 equalized split rounds to the current split, the direction of greater measured demand supplies the
 deliberate one-unit overshoot.
@@ -143,7 +142,7 @@
 
 ## Integration and observability
 
-- `src/core/config.h` is the sole parser/default source for `flip-auto`, `flip-auto-band`, and
+- `src/core/config.h` is the sole parser/default source for `flip-auto` and
   `flip-work-window`; `src/cmd/t_server.cc` exposes them as immutable CONFIG values.
 - `tomokv.conf` documents the numeric grammar and off/auto semantics.
 - `INFO FLIPCTL` reports state/phase, anchor split/rate, bands, total/per-reason trigger counts, and
diff --git a/docs/specs/AUDIT-TLS.md b/docs/specs/AUDIT-TLS.md
index 0256e887f5d27c210d436bc219f3c7521ddad0db..87a2eaca3137a47f7c0e1567794650f57531860b
--- a/docs/specs/AUDIT-TLS.md
+++ b/docs/specs/AUDIT-TLS.md
@@ -3,7 +3,7 @@
 > **Where it landed (2026-09-04).** The feature shipped as `src/net/tls.{h,cc}` (this document
 > proposes `src/net/tls_ctx.h` + `src/net/tls_conn.h`); the test is `tests/tls.py` (no
 > `tests/gen-certs.sh`); no `tls-bio-size` knob exists. Defaults: AES-128-GCM-first cipher lists
-> and `tls-ktls yes` (`tomokv.conf`, NETWORK).
+> and automatic kTLS negotiation with userspace fallback (`tomokv.conf`, NETWORK).
 
 Governing audit. Read before writing any TLS code in `/home/user/Projects/tomokv-cpp-perthread`.
 No repository was modified in producing this.
diff --git a/scratchpad/rlbatch/lib.sh b/scratchpad/rlbatch/lib.sh
index 7bcf3e4e6030c79174bf35d9bb1b3dfb11f80c42..12184314d2aafac0dc7aa48db761a3bdec5de8c7
--- a/scratchpad/rlbatch/lib.sh
+++ b/scratchpad/rlbatch/lib.sh
@@ -17,7 +17,7 @@
 boot_srv(){ # boot_srv <binary> <logfile> [extra args...] -> SRV pid
   local bin="$1" log="$2"; shift 2
   guard_port "$PORT" || return 1
-  # EXTRA_ARGS lets a caller add boot flags (e.g. --read-local-interleave 0) without a second copy
+  # EXTRA_ARGS lets a caller add boot flags (e.g. --lb 0) without a second copy
   # of this function; word splitting is intentional.
   # shellcheck disable=SC2086
   taskset -c "$SRVCORE" "$bin" --port "$PORT" --bind 127.0.0.1 --shards 16 \
diff --git a/tests/flipctl_unit.cc b/tests/flipctl_unit.cc
index 540a6c40e9589572431075b623bc553c76ee8d85..33209258e2958771d60f86b919f6f510e9977737
--- a/tests/flipctl_unit.cc
+++ b/tests/flipctl_unit.cc
@@ -184,11 +184,11 @@
 
     // THE TYPED BAND IS A FLOOR, NOT A CEILING. The gate-hygiene lane's instrumented failure: a
     // driver holding its rate to 0.07% across 34 samples, a fingerprint distance of 0.2518 against
-    // a flat --flip-auto-band 2 (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
+    // a flat explicit band of 2% (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
     // A detector whose signature genuinely moves between adjacent quiet windows must end up with a
     // band above that movement whatever the operator typed, and must still fire on a real change.
     {
-        FlipShiftDetector typed(2, 4);   // --flip-auto-band 2
+        FlipShiftDetector typed(2, 4);   // internal explicit-band control: 2%
         // Stationary but NOISY: the read/write split alternates 35:65 / 65:35 window to window.
         // The detector smooths the signature, so the anchored distance this produces is ~0.09.
         const auto noisy = [](int k) { return quiet(k % 2 ? 65 : 35, k % 2 ? 35 : 65); };
@@ -250,8 +250,8 @@
         if (fires) fail("the learned band fired on quiet-state movement");
     }
     // A TYPED BAND UNDER THE REAL SAMPLER. The shipped configuration cannot reach this -- the
-    // controller runs the learned band -- but an operator who sets --flip-auto-band AND the sampler
-    // can, and on a tree where the typed path bypasses the floor the fingerprint lane measured 73
+    // controller runs the learned band. This internal explicit-band control preserves the earlier
+    // regression: where the typed path bypassed the floor the fingerprint lane measured 73
     // two-consecutive exceedances in 600 stationary windows at K=100 and 208 at K=60: a spurious
     // maneuver every few seconds on a load that never changed. Driven here through THEIR writer, a
     // stationary 50/50 stream at depth 32, with the floor in place: none, and a real mix change
@@ -259,7 +259,7 @@
     {
         for (uint32_t window : {60u, 100u}) {
             XorShift rng;
-            FlipShiftDetector typed(2, 8);   // --flip-auto-band 2, the operator's knob
+            FlipShiftDetector typed(2, 8);   // internal explicit-band control: 2%
             int fires = 0, confirmed = 0, streak = 0;
             for (int w = 0; w < 60; w++) {
                 FlipFingerprintWriter writer;
diff --git a/tests/rlcache_churn.py b/tests/rlcache_churn.py
index b56ae4041ae0843dc67443f16017270063853124..f8a2c0730bb0a4fb6c3b5f6282c547226bb76599
--- a/tests/rlcache_churn.py
+++ b/tests/rlcache_churn.py
@@ -59,7 +59,7 @@
 
 
 # THE LOAD BALANCER MUST HAVE SOMETHING TO BALANCE. A uniform key distribution over one small key
-# space keeps every shard at the same op rate, the imbalance never crosses lb-imbalance-pct, and no
+# space keeps every shard at the same op rate, the imbalance never crosses the learned band, and no
 # shard ever changes owner -- which is the precondition for the defect this battery exists for
 # (measured: 7.8M ops, 0 migrations). A hot window that ROTATES makes a few shards hot, then makes
 # different ones hot, so the balancer moves shards continuously and in both directions, which is

exec
/bin/bash -lc "git diff -- docs/flipctl-design.md DESIGN.md && sed -n '1,155p' MERGE.md && sed -n '300,335p' MERGE.md && git status --short" in /home/user/Projects/cx-merge
 succeeded in 0ms:
diff --git a/DESIGN.md b/DESIGN.md
index 0fba740d9..7ad3ac9fe 100644
--- a/DESIGN.md
+++ b/DESIGN.md
@@ -1,6 +1,6 @@
 # Thread amortization study surface
 
-`thread-mode` and `overlap` are one boot-latched measurement surface. The documented mode
+`thread-mode` and the study-only `x-overlap` are one boot-latched measurement surface. The documented mode
 names are `2s` (separate IO and executor threads, the default) and `1s` (one generalized thread owns
 both roles). `split` and `fused` remain parser aliases so old invocations keep working.
 
@@ -9,10 +9,10 @@ both roles). `split` and `fused` remain parser aliases so old invocations keep w
 | `2s` | ordinary split IO/ex loops | exact `t-iopipe` WB/IFID batch schedule | rejected at config validation |
 | `1s` | coarse IFID → EX → WB rotation | exact `t-genthread` `iofused` schedule | gated `iofused` three-way schedule |
 
-The compatibility options `thread-pipeline` and `genthread-schedule` remain accepted: the former
-is a numeric alias for `overlap`, while the latter maps `coarse`, `iofused`, and the legacy
-`streams` name to `1s` plus overlap 0, 1, and 2 respectively. The legacy name is only a parser
-alias; overlap 2 no longer runs the streams implementation. Overlap 2 emits a boot warning because
+Use `--x-overlap 0|1|2` for these study schedules and `--x-ex-sched 0|1` for the executor study.
+The old public schedule spellings and compatibility aliases are removed; see
+[DESIGN-KNOBS.md](DESIGN-KNOBS.md). Below, "overlap" names the internal schedule value.
+Overlap 2 emits a boot warning because
 it is an experimental research schedule. Unified overlap 1 and 2 require `net-io uring`; the split
 iopipe schedule retains its explicit epoll specialization. No schedule constant is runtime-tunable.
 
diff --git a/docs/flipctl-design.md b/docs/flipctl-design.md
index e6a5e31a6..cd9655719 100644
--- a/docs/flipctl-design.md
+++ b/docs/flipctl-design.md
@@ -8,8 +8,7 @@ dark (`flip-auto 0`).
 ## Ownership and footprint
 
 - `src/main.cc` is the controller cron owner. When `flip-auto=1`, the main/monitor thread calls
-  `Server::flipctl_tick()` once per existing LB tick (`lb-tick-ms`; if that mechanism is disabled,
-  the period derives from the provisioned thread count). No IO or EX loop runs controller policy.
+  `Server::flipctl_tick()` on its internal observation cadence. No IO or EX loop runs controller policy.
 - `src/core/flipctl.{h,cc}` contains the fingerprint representation, shift detector, and controller
   state machine. Controller vectors allocate only when `flip-auto=1`.
 - Each `ThreadCtx` embeds one `FlipFingerprintWriter`. It has no dynamic storage. Only the physical
@@ -46,9 +45,9 @@ distance gives equal weight to the four feature families. Scalar distances use o
 measurement quantum, avoiding the zero-anchor discontinuity where one rare admin argument would
 otherwise have distance one.
 
-With `flip-auto-band=-1`, the band is twice the maximum quiet adjacent/drift distance learned at
-the final split, floored by the four-family sampling quantum divided by observed commands. A
-numeric value is a percent. Zero disables fingerprint and collapse re-triggers. Final anchor
+The automatic band is twice the maximum quiet adjacent/drift distance learned at
+the final split, floored by the four-family sampling quantum divided by observed commands.
+It has no public override. Final anchor
 learning consumes a number of closed aggregates derived from the provisioned thread count. It is
 done after maneuver-only age sampling is disarmed, so the controller cannot detect its own signal
 cost transition as a workload change.
@@ -64,13 +63,13 @@ The boot maneuver remains pending until the command-rate EWMA's relative slope a
 falls below a threshold derived from its own change jitter and measurement quantum. Directional
 connection ramp-up therefore remains drift, while jittery stationary traffic can qualify even when
 its absolute rate never enters a quiet band. A 30-second ceiling, converted to ticks from
-`lb-tick-ms`, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
+the internal observation cadence, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
 thread per tick resets the learning/cap state, so an idle server remains in
 `awaiting-load-stability` and never pays for a maneuver it cannot use.
 
-`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. An explicit
-`lb-age-sample-rate` is used when nonzero; otherwise the rate derives from the provisioned thread
-count. IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
+`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. Its rate
+derives from observed command volume and the three-tick decision window, and adapts on completed
+traffic windows; see [DESIGN-KNOBS.md](../DESIGN-KNOBS.md). IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
 writes. IO releases its ROB-head age map when the rate returns to zero.
 
 After a stabilized baseline rate, `issue_initial_jump()` subtracts the per-thread maneuver window.
@@ -88,8 +87,8 @@ Both role-constraint guards use the role's busiest corrected-busy thread. Thus a
 not vetoed by idle peers. If either role has no measured work/busy quantity, measurement continues;
 the controller does not invent a workload prior.
 
-The calculation is performed in placement units (one logical CPU normally, one sibling pair under
-`smt-mode=1`) and then reported/committed in threads. The target passes `nio_eq` in the measured
+The calculation is performed in placement units (one logical CPU normally, one sibling pair when
+the allowed topology contains complete pairs) and then reported/committed in threads. The target passes `nio_eq` in the measured
 direction by `max(1, abs(nio_eq-nio_now)/2)`, clamped to one live unit of each role. Even when the
 equalized split rounds to the current split, the direction of greater measured demand supplies the
 deliberate one-unit overshoot.
@@ -143,7 +142,7 @@ fingerprint, rate surge, rate collapse, and forced causes independently observab
 
 ## Integration and observability
 
-- `src/core/config.h` is the sole parser/default source for `flip-auto`, `flip-auto-band`, and
+- `src/core/config.h` is the sole parser/default source for `flip-auto` and
   `flip-work-window`; `src/cmd/t_server.cc` exposes them as immutable CONFIG values.
 - `tomokv.conf` documents the numeric grammar and off/auto semantics.
 - `INFO FLIPCTL` reports state/phase, anchor split/rate, bands, total/per-reason trigger counts, and
# Merge record — 2026-09-07

Both incoming lanes are merged into this working tree from common base
`78c3e53919a5d557bd3dd4625c9524d0baa212e4`. The dispatch-scaling retirement wins;
the timing rewrite cannot restore the deleted ownership instrument. The OFF RENAME
four-roll hard-failure control and the surviving mechanism/release-timing split both remain.
The merged gate needs **quick 325 / full 342**, with the expected-count constants untouched.

No build, server, benchmark, gate, C++ compiler, or server-connected battery was run.
The maintainer's 344/344, zero-SKIP result belongs to the incoming timing lane, not this merge.
Both source worktrees were read only. Their tracked-file inventories and file hashes were
rechecked after integration and were unchanged. Their untracked `CODEX-OUT.md` run transcripts
were not imported; the pre-existing transcript in this worktree was left alone.

Inputs: 12 timing paths plus 40 knobs paths, overlapping on the three named conflict files,
for **49 distinct input paths**, including the knobs lane's new files and wrapper deletion.
The SHA-256 digests of the captured `git diff HEAD --binary` inputs were:

- timing: `5310bed7954cc01a061bcb5c20e18b6b9f8096dfadbef7b94fb871fcfea4d3a9`
- knobs: `cb8a21fa050e9961dd1f3bed442d5de5f3b0df1e34e03fd183a622ad73ed3084`

## Conflict decisions

| File | Timing intent | Knobs intent | Decision and reason |
| --- | --- | --- | --- |
| `tests/atomic_torn.py` | Require counters, exact values, and safety on every build; score short promotion and shrink/reclaim budgets only with `--release-build`; strengthen credit sampling. | Remove configurable-window calls and exercise the derived production window, including a separate live-reconfiguration witness. | Retain timing argument parsing, `release_note`, required promotion counters, monotonic watchdog, unconditional hold/predecessor/promotion checks, and the 1.5-second release promotion budget. Use the knobs admission/reconfiguration block and its helper intact; the removed resize experiment cannot be run. The exact superseded hunks are listed below. The base RENAME control remains unchanged. |
| `tests/gate.sh` | Pass release identity to release atomic, stream, expiry, borrow, and dispatch arms; omit it for ASAN atomic. | Migrate all boots and CONFIG/INFO checks to the reduced grammar; replace the script bounds geometry and persistence surface check; retire dispatch. | Keep every knobs boot/surface change and every surviving timing call-site change. Only dispatch's invocation/flag and its live-instrument commentary are superseded by retirement. Correct the ASAN comment to describe the surviving promotion budget and mandatory derived-window witnesses. Neither EXPECT assignment changes. |
| `tests/xshard_dispatch_scale.sh` | Fix best-pair selection, freeze balancing, check each pair and repeatability, alternate arm order, and score all qualified release ratios against 1.20. | Delete the wrapper because its manual shard map is removed. | Delete. The rewrite still passes the removed manual-map flag. Its threshold qualification does not supply an equivalent geometry. Delete the Python helper too, rather than leave the retired instrument's code behind. |

### Semantic ruling and evidence

**No: the timing rewrite does not make dispatch scaling valid without `--shard-home`.**
The incoming timing wrapper still constructs `HOMES` as `sid -> 2 + sid % 2` at line 62
and passes it to the server at lines 81–82, along with independently disabled key/client
balancing and disabled flip. The first two executors own all 16 shards at both thread counts;
extra executor threads are deliberately empty filler threads, pinned away from the real owners.
This is the experimental control, not merely a spelling of a default.

In the merged source, `Placement::assign_shard_homes` assigns
`shard_home_[sid] = ex_[sid % ex_.size()]` (`src/core/placement.h:176–180`). The old 4-thread
arm has two executors; the 128-thread arm has 126. Without the manual map, the 16 shards in
the big arm occupy its first 16 executors, including 14 formerly empty filler executors.
Actual work, ownership, and contention change along with the array/thread-count dimension.
Changing the obsolete balancing flags to `--lb 0` would freeze this different geometry; it
would not restore the original one.

The timing rewrite's 2% test uses within-count cross/same cost spreads and same-shard controls
across counts (wrapper lines 133–145). Its final assertion requires every qualified ratio to
meet the unchanged 1.20 limit (line 164). These are repeatability checks, not an ownership
construction or validation of the limit on a new geometry. Its helper's distinct `x`/`y`
replies and end-of-arm owner-map comparison (Python lines 103–107 and 123–124) prove reply
content and within-arm stability, not equivalent work placement across thread counts.
Even perfectly repeatable arms could therefore assert the wrong claim.

The knobs lane's OBJECTIONS item 5 is accepted. The gate row, shell wrapper, Python helper,
and all of the timing lane's dispatch-specific implementation are retired. The rewrite is
**moot**, not partly retained as an unwired timing check. Historical measurements remain
explicitly historical; this merge makes no new dispatch-scaling claim.

### OFF RENAME and the atomic timing split

The complete base block from “Broadened movers: RENAME” through the following store-family
section is byte-for-byte preserved. It still runs `for _roll in range(4)` at
`tests/atomic_torn.py:771`, arms and clears the hop delay in `try/finally`, retries only a
clean miss, and stops on a witnessed tear, worker error, or surviving helper. The OFF assertion
at line 782 requires positive invalid/read counts and no errors/live helpers. Four clean
misses fail; there is no OFF skip and no release-build condition around this control.
The base comment describing that policy is retained too. Each `rename_hammer` invocation
retains the base's state reinitialization and new connections/helpers.

There is no interaction requiring a weaker RENAME policy: it witnesses a detector, whereas
`release_note` scores the separate promotion completion budget. Missing hold, predecessor,
or promotion counters still fail on both build types. The latter retains its 30-second
mechanism watchdog and the original separate 1.5-second release budget (`:721`).

The three textual overlaps in the old admission/lease region are resolved together:

- The one-credit pipeline setup and the timing lane's required stall-counter reads are
  superseded by `held_burst(..., whole_window=True)` (`:900`). Its direct INFO indexing
  requires every counter to exist, and a positive held stall delta is mandatory.
- The old `lease_writer`, completed-write counters, resize sequence 31/7/19/3, 32 observations
  at bound 3, and exact final pool 3 are superseded by the second fresh held burst with
  `reconfigure=True` (`:908`). Live `CONFIG SET atomic 1` rebuilds generations; the test
  proves pre-CONFIG groups survived, observes the fixed bound and zero debt, checks every
  reply, and requires zero live groups/debt plus the entire derived credit pool after drain.
- The release-only three-second **shrink/reclaim** budgets and their measurements are dropped
  with that resize experiment. No shrink exists, and a timing claim measured under the old
  one-/three-credit setup is not transplanted to a held production-window burst. Reclamation
  itself is retained as mandatory mechanism coverage. The new helper's arm, resume, and
  drain deadlines are bounded failure watchdogs on every build, not successful timing skips.

`tests/atomicwindow.py` is otherwise exactly the knobs input. It clears the artificial hold
before starting the completion watchdog, retries only a joined/reclaimed witness miss on
fresh state, and fails after four misses. Bad replies, excess groups, debt, lost credits,
failed resume, and surviving helpers cannot be retried into success. Both separate outer
atomic battery invocations remain. The RENAME hit-rate evidence is not reused to claim a
hit probability for this different witness. Other base controls and timing changes are unchanged.

## EXPECT arithmetic, by emitting line

The actual quick exit is **base line 1258**, **merged line 1246**:
`if [ "$TIER" = quick ]; then`. The incoming design notes' other line numbers describe
older intermediate text; in particular, their “old exit 1260” is not the supplied base.
The following line numbers were read from the supplied base and final merged file.

| Ledger change | Base emitting line | Merged emitting line | Side of quick exit | Quick delta | Full delta |
| --- | ---: | ---: | --- | ---: | ---: |
| Retire `xscript off control`; former three-arm loop becomes `limit window` | 658, once per old arm | 653, twice for the surviving arms | Before both exits | -1 | -1 |
| Retire `cross-shard dispatch scaling` | 686 | none; retirement comment at 676 emits nothing | Before base exit | -1 | -1 |
| Retire old persistence CONFIG surface label, four iterations | 981 | none | Before base exit | -4 | -4 |
| Replacement `configuration reduction + actual geometry`, same two engines × two atomic settings | none | 969, four iterations | Before merged exit | +4 | +4 |
| Atomic admission/reconfiguration internals; outer release battery retained | 435 | 435 | Before both exits | 0 | 0 |
| Atomic admission/reconfiguration internals; outer ASAN battery retained | 1270 | 1260 | After both exits | 0 | 0 |
| All other outer battery rows and loops, including release-flag wiring | existing | retained | Same side of exit | 0 | 0 |

**Quick: `327 - 1 - 1 - 4 + 4 = 325`. Full: `344 - 1 - 1 - 4 + 4 = 342`.**
The knobs lane's numeric claim is confirmed. With the unchanged optional NIC row, full is 343.
Renaming engine labels from normal/uring to epoll/uring preserves two iterations; it does not
add or retire persistence coverage. The new Python helpers add internal checks to existing
rows, not ledger entries. The scratch experiments retired below have no emitting line in
`gate.sh`, so contribute zero to both tiers.

`EXPECT_QUICK=327` at line 141 and `EXPECT_FULL=344` at line 142 are byte-for-byte unchanged.
**The maintainer must update them to 325 and 342.** The ledger mismatch is intentional until
that edit; this merge does not make a failing suite appear green by changing its expectations.
Static comparison of all success-emitting labels, with the documented replacement and engine
label normalization, confirmed no other row was lost or added; the changed loop multiplicity
was checked separately.

## Complete incoming-file accounting

“Exact” means a full-file byte comparison against the captured working-tree input, so it covers
every incoming hunk, including additions. All production files under `src/` equal the knobs
lane byte-for-byte. All non-exact input files have a specific disposition in this table.

| Input path | Lane | Disposition |
| --- | --- | --- |
| `DESIGN-GATEHYGIENE.md` | timing | All incoming body text retained; historical scope and merged status added; section heading updated. |
| `DESIGN-KNOBS.md` | knobs | All incoming text retained; added provenance note pointing to merged line arithmetic. |
| `docs/specs/AUDIT-AOF.md` | knobs | Knobs exact: every byte/hunk retained. |
| `docs/thread-mode.md` | knobs | Knobs exact: every byte/hunk retained. |
| `src/base/topology.h` | knobs | Knobs exact: every byte/hunk retained. |
| `src/cmd/lbsignals.cc` | knobs | Knobs exact: every byte/hunk retained. |
| `src/cmd/server_tail.cc` | knobs | Knobs exact: every byte/hunk retained. |
| `src/cmd/t_server.cc` | knobs | Knobs exact: every byte/hunk retained. |
| `src/core/config.h` | knobs | Knobs exact: every byte/hunk retained. |
| `src/core/ex_loop.h` | knobs | Knobs exact: every byte/hunk retained. |
| `src/core/flipctl.cc` | knobs | Knobs exact: every byte/hunk retained. |
| `src/core/io_loop.h` | knobs | Knobs exact: every byte/hunk retained. |
| `src/core/iopipe_pipeline.h` | knobs | Knobs exact: every byte/hunk retained. |
| `src/core/placement.h` | knobs | Knobs exact: every byte/hunk retained. |
| `src/core/server.h` | knobs | Knobs exact: every byte/hunk retained. |
| `src/core/shard.h` | knobs | Knobs exact: every byte/hunk retained. |
| `src/core/weighted_lb.h` | knobs | Knobs exact: every byte/hunk retained. |
| `src/main.cc` | knobs | Knobs exact: every byte/hunk retained. |
at `src/core/ex_loop.h:1295`, loops at line 1317, and increments
`mget_generation_retries` at line 1432. This repeats a per-operation reader attempt and
conflicts with the stated no-reader-retries law. The path already existed at the common
ancestor and is explicitly called out in the knobs lane's OBJECTIONS item 1; the merge does
not hide it or claim to fix it. The ordinary `prepare_local_mget` two-attempt loop also remains
in the inherited source. No new retry/seqlock scheme or layout split was introduced.

The remaining objections are preserved too: actual legacy-alias uses, unmeasured LB/shard
sampling and transfer policies, stricter automatic SMT geometry, the retired manual-map
instrument, and the longer real LRU clock waits. The knobs lane's held-window repair still
needs maintainer release/ASAN validation at 16 shards, ratio 6:2, cores 0–7. Its five-second
arming watchdog is a bounded discovery budget, not a measured false-failure probability.

Completed server-free checks:

- Python AST parsing for all 21 affected Python files and `bash -n` for all 20 surviving
  affected shell files. No battery entry point was executed.
- Ten executions of only the source-extracted OFF RENAME control with fake hammer/debug
  callbacks: first/fourth-attempt hits, four clean misses, worker errors, and live helpers,
  each under both build identities. Misses failed after four attempts; errors/live helpers
  stopped retries; every armed delay was cleared. No sockets, threads, or server were used.
- Exact base comparison of the entire RENAME block; source inspection of unconditional
  derived-window callers and the surviving promotion release split.
- Input-path/hash accounting, full-file comparisons for retained lane files, static gate-row
  comparison and loop arithmetic, unchanged EXPECT assignments, layout-assertion inspection,
  removed-flag/caller search, conflict-marker search, and `git diff --check`.

No C++ syntax check, build, server, benchmark, full test, or gate result is claimed. The
maintainer supplies the merged runtime and performance verdict.
 M AUDIT-CLEANUP.md
 M AUDIT-LOCALFUSE.md
 M AUDIT-TESTS.md
 M AUDIT-resp3push.md
 M DESIGN-GATEHYGIENE.md
 M DESIGN-P0REPLY.md
 M DESIGN-W-RLIDEAS.md
 M DESIGN-flipfp.md
 M DESIGN.md
 M NOTES-AOFFRAME.md
 M NOTES-AOFSCRIPT.md
 M NOTES-ATOMICS.md
 M NOTES-BARRIERTEST.md
 M NOTES-CAPTURE-PREFETCH.md
 M NOTES-EPOLL.md
 M NOTES-HYGIENE.md
 M NOTES-KTLS.md
 M NOTES-MIGRATE.md
 M NOTES-PERSIST-IO.md
 M NOTES-SERVERTAIL.md
 M NOTES-SNAPCUT.md
 M NOTES-XPERF2.md
 M NOTES-XSCRIPT.md
 M P128.md
 M docs/flipctl-design.md
 M docs/specs/AUDIT-AOF.md
 M docs/specs/AUDIT-TLS.md
 M docs/thread-mode.md
 M scratch/NOTES.md
 M scratch/final.sh
 M scratch/finalw.sh
 M scratch/flipfp/bat.sh
 M scratch/flipfp/chain.sh
 D scratch/flipfp/fpmargin.sh
 M scratch/flipfp/fpprobe.sh
 M scratch/flipfp/fpshift.sh
 M scratch/flipfp/report.sh
 M scratch/red.sh
 M scratch/red2.sh
 M scratch/report.py
 M scratch/report2.py
 M scratch/rv.sh
 M scratch/rv3.sh
 M scratch/ver.sh
 M scratchpad/replycode/batteries.sh
 M scratchpad/ringsize/batteries.sh
 M scratchpad/rlbatch/batteries.sh
 M scratchpad/rlbatch/lib.sh
 M scratchpad/robdiet/batteries.sh
 M src/base/topology.h
 M src/cmd/lbsignals.cc
 M src/cmd/server_tail.cc
 M src/cmd/t_server.cc
 M src/core/config.h
 M src/core/ex_loop.h
 M src/core/flipctl.cc
 M src/core/io_loop.h
 M src/core/iopipe_pipeline.h
 M src/core/placement.h
 M src/core/server.h
 M src/core/shard.h
 M src/core/weighted_lb.h
 M src/main.cc
 M src/net/tls.cc
 M src/net/uring.h
 M src/persist/aof.cc
 M src/store/eviction.h
 M src/store/flatstore.h
 M tests/_lib.py
 M tests/aof_rewrite_matrix.sh
 M tests/aof_rewrite_trigger_matrix.sh
 M tests/atomic_ryow.py
 M tests/atomic_torn.py
 M tests/borrow_registry.py
 M tests/bplus.py
 M tests/config_parser_test.cc
 M tests/differ.py
 M tests/evict_battery.py
 M tests/expireindex.py
 M tests/flipctl.py
 M tests/flipctl_unit.cc
 M tests/gate.sh
 M tests/infofix.py
 M tests/lbsignals.py
 M tests/pubsub.py
 M tests/rlcache_churn.py
 M tests/slowlog.py
 M tests/stream.py
 M tests/tls.py
 M tests/xscript.py
 D tests/xshard_dispatch_scale.py
 D tests/xshard_dispatch_scale.sh
 M tomokv.conf
?? .merge-inputs/
?? CODEX-OUT.md
?? DESIGN-KNOBS.md
?? MERGE.md
?? tests/atomicwindow.py
?? tests/knobs.py

exec
/bin/bash -lc "python3 - <<'PY'
from pathlib import Path
import difflib, re
knobs = Path('.merge-inputs/knobs/tests/gate.sh').read_text()
timing = Path('.merge-inputs/timing/tests/gate.sh').read_text()
gate = Path('tests/gate.sh').read_text()
expected = knobs
for name in ('atomic_torn', 'expireindex', 'borrow_registry'):
    before = 'py tests/' + name + '.py 127.0.0.1 "'$PORT >'"'
    after = 'py tests/' + name + '.py 127.0.0.1 "'$PORT --release-build >'"'
    assert before in expected
    expected = expected.replace(before, after, 1)
for log in ('gate-"'$t-$AT'"', 'gate-fusedarmed-"'$t-$AT'"'):
    before = '    py tests/"'$t.py 127.0.0.1 $PORT >/tmp/'"' + log + '.txt 2>&1'
    after = '    FEATURE_ARGS=()\\n    [ \""'$t" = stream ] && FEATURE_ARGS+=(--release-build)'"\\n    py tests/"'$t.py 127.0.0.1 $PORT "${FEATURE_ARGS[@]}" >/tmp/'"' + log + '.txt 2>&1'
    assert expected.count(before) == 1
    expected = expected.replace(before, after)
a = '# These assert a RATIO or a growth bound, never an absolute time, and each was verified to FAIL on\\n# the binary from before its fix (see NOTES-XPERF2.md). They shipped unwired, which is the same\\n# coverage-that-does-not-exist problem the gate lane fixed elsewhere.\\n'
b = '# Exact mechanism checks run on every build; timing budgets require the explicit release-build\\n# flag. A ratio of wall times is still a performance claim (see DESIGN-GATEHYGIENE.md).\\n'
assert a in expected
expected = expected.replace(a, b)
expected = expected.replace('# Dispatch scaling\\'s manual ownership geometry was retired with shard-home; DESIGN-KNOBS.md.', '# Dispatch scaling\\'s manual ownership geometry is retired; see MERGE.md and DESIGN-KNOBS.md.')
a = 'py tests/atomic_torn.py 127.0.0.1 "'$PORT >/tmp/gate-atomic-torn-asan.txt'"'
b = '# Deliberately omit --release-build: coverage/safety checks remain mandatory, including the\\n# derived-window witnesses; promotion timing prints without scoring its release budget.\\n' + a
assert a in expected
expected = expected.replace(a, b)
assert expected == gate, ''.join(difflib.unified_diff(expected.splitlines(True), gate.splitlines(True)))
print('Gate equals knobs input plus exactly the retained timing call-site changes and documented comment resolutions.')
atomic = Path('tests/atomic_torn.py').read_text()
timing = Path('.merge-inputs/timing/tests/atomic_torn.py').read_text()
knobs = Path('.merge-inputs/knobs/tests/atomic_torn.py').read_text()
head = '# Admission liveness:'
newhead = '# Separate admission liveness from live credit reconfiguration/accounting.'
tail = '# Live CONFIG flips under active traffic are a liveness/safety arm.'
expected = timing[:timing.index(head)] + knobs[knobs.index(newhead):knobs.index(tail)] + timing[timing.index(tail):]
assert expected == atomic
print('Atomic file equals timing input with exactly the documented knobs admission/reconfiguration replacement.')
PY
rg -n '\"(ex-sched|overlap)\"|CONFIG .*overlap|"'^overlap|''^ex-sched'"' tests src tomokv.conf
rg -n 'fpmargin.sh|xshard_dispatch_scale' -g '"'!.merge-inputs/**'"' -g '"'!CODEX-OUT.md'"' -g '"'!*.md'"' ." in /home/user/Projects/cx-merge
 exited 1 in 0ms:
Traceback (most recent call last):
  File "<stdin>", line 15, in <module>
AssertionError
tests/knobs.py:16:    "lb-move-cap", "lb-cooldown-ms", "ex-sched", "overlap", "thread-pipeline",
tests/atomicwindow.py:30:        label = "reconfigure" if reconfigure else "window" if whole_window else "overlap"
tests/atomic_ryow.py:235:overlap_ok = all(c.read() == b"OK" for _ in range(burst_count))

diff --git a/AUDIT-TESTS.md b/AUDIT-TESTS.md
index e9cd43ffbc376172717531d0037de1c79fd23690..fe1fd0e2c39e965f605e4a30894a545129541534
--- a/AUDIT-TESTS.md
+++ b/AUDIT-TESTS.md
@@ -122,7 +122,7 @@
 | MM floor (memtier 20 s, sampled 8+6) | 1 | 14 | measurement, keep |
 | saturated-flip memtier | 1 | 25+3 | measurement, keep |
 | `flipctl.py --stable-seconds 30` (+ramp/idle/surge, timeout 300) | 1 | 60-120 est. | measurement, keep |
-| `xshard_dispatch_scale.sh` (2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 | 90-150 est. | measurement, keep |
+| Retired `xshard_dispatch_scale.sh` (historical 2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 historically, 0 now | 90-150 est. historically | manual ownership instrument removed; see `MERGE.md` |
 | `atomic_torn.py` hammers (3+2.5+2+2.5 s) + OFF re-rolls (up to 14 s) + RENAMENX/COPY parks (2 x 64 x 100 ms = 12.8 s) + window/lease/churn (~6 s) | 1 quick, +1 ASAN | ~45-60 each | re-rolls are dead time on this kernel (F6) |
 | `spinprobe.py` (6 s baseline + 6 s probe) | 3 | 37.5 | measurement, keep |
 | Python batteries' literal sleeps (census: flipctl 17 s, limits 5.8, edgetime 4.5, atomic_ryow 4, hexpire 3.4 — most run x2) | | ~60 | keep |
diff --git a/DESIGN-GATEHYGIENE.md b/DESIGN-GATEHYGIENE.md
index 5ea9df93e4548fb73a7e78b38debd52d876ad658..e6ac34a10326fe8880eaa664690832da3875b9b8
--- a/DESIGN-GATEHYGIENE.md
+++ b/DESIGN-GATEHYGIENE.md
@@ -459,7 +459,21 @@
 
 ---
 
-## 10. Follow-up diff on `78c3e5391` — 2026-09-07, runtime validation pending
+## 10. Incoming timing lane on `78c3e5391` — 2026-09-07
+
+**Merged disposition:** the maintainer reports the incoming timing lane gated 344/344 with no
+SKIP rows. The account below records that lane's original implementation and offline validation;
+it is not a validation claim for the merged tree. Its dispatch wrapper/helper and timing evaluator
+are retired: repeatability checks did not replace the deleted manual ownership instrument. The
+configurable-window shrink samples and shrink/reclaim budgets are superseded by the derived-window
+held-burst liveness and old-generation reconfiguration witnesses in `tests/atomicwindow.py`.
+All those mechanism, exact-reply, bound, and credit-reclamation checks remain mandatory on every
+build. The original OFF RENAME four-roll hard-failure control is unchanged, and the 1.5-second
+promotion budget still requires `--release-build`. The merged ledger requires quick 325/full 342;
+the constants remain untouched. [MERGE.md](MERGE.md) records the complete conflict and row accounting.
+
+The rest of this section is historical lane evidence, including its now-superseded dispatch,
+shrink, admission-calibration, and ledger statements.
 
 This is a test-only diff in `cx-timing`. No server, benchmark, gate, or compiler was run. The
 earlier acceptance results above describe earlier changes, **not this diff**. The worked fixes
diff --git a/DESIGN-flipfp.md b/DESIGN-flipfp.md
index 32eb6f46c24fb688db6accc79537fbd664bd084c..f705b25837c72a011da71df06dec09d2757f05ae
--- a/DESIGN-flipfp.md
+++ b/DESIGN-flipfp.md
@@ -311,7 +311,9 @@
 detector. The signal and the floor cannot be read from one run -- with a learned band the trigger
 fires and `reset()` wipes the distance -- so each arm is run twice: `--flip-auto-band 90` holds the
 anchor and lets the distance converge (the signal), `--flip-auto-band -1` learns the floor.
-`scratch/flipfp/fpmargin.sh`.
+The former `scratch/flipfp/fpmargin.sh` implemented this two-band experiment. It is retired in the
+merged tree because neither pinning the band nor disabling its retriggers has a public control.
+These historical measurements remain evidence only for their recorded builds; see `MERGE.md`.
 
 **BITCOUNT -> INCR** (the redesign lane's reference change), 2:2 split, `--rate-limiting` pinned:
 
diff --git a/DESIGN.md b/DESIGN.md
index 0fba740d94ba1915868410de40bcb685ae675833..7ad3ac9fef512d8549eadfce44036b8a6a181e9f
--- a/DESIGN.md
+++ b/DESIGN.md
@@ -1,6 +1,6 @@
 # Thread amortization study surface
 
-`thread-mode` and `overlap` are one boot-latched measurement surface. The documented mode
+`thread-mode` and the study-only `x-overlap` are one boot-latched measurement surface. The documented mode
 names are `2s` (separate IO and executor threads, the default) and `1s` (one generalized thread owns
 both roles). `split` and `fused` remain parser aliases so old invocations keep working.
 
@@ -9,10 +9,10 @@
 | `2s` | ordinary split IO/ex loops | exact `t-iopipe` WB/IFID batch schedule | rejected at config validation |
 | `1s` | coarse IFID → EX → WB rotation | exact `t-genthread` `iofused` schedule | gated `iofused` three-way schedule |
 
-The compatibility options `thread-pipeline` and `genthread-schedule` remain accepted: the former
-is a numeric alias for `overlap`, while the latter maps `coarse`, `iofused`, and the legacy
-`streams` name to `1s` plus overlap 0, 1, and 2 respectively. The legacy name is only a parser
-alias; overlap 2 no longer runs the streams implementation. Overlap 2 emits a boot warning because
+Use `--x-overlap 0|1|2` for these study schedules and `--x-ex-sched 0|1` for the executor study.
+The old public schedule spellings and compatibility aliases are removed; see
+[DESIGN-KNOBS.md](DESIGN-KNOBS.md). Below, "overlap" names the internal schedule value.
+Overlap 2 emits a boot warning because
 it is an experimental research schedule. Unified overlap 1 and 2 require `net-io uring`; the split
 iopipe schedule retains its explicit epoll specialization. No schedule constant is runtime-tunable.
 
diff --git a/NOTES-HYGIENE.md b/NOTES-HYGIENE.md
index 23506bce64f00cb0dccaaff93362ccd30deab229..f224db07f4ceb2e4791c7cbb471b87d83a0a3e93
--- a/NOTES-HYGIENE.md
+++ b/NOTES-HYGIENE.md
@@ -352,7 +352,8 @@
 ## Test-file census
 
 The calibration examples expireindex.py, borrow_registry.py, and xshard_dispatch_scale.sh were
-already wired in the current parent. aof_frames.py is indirectly executed by aof_frame_order.py;
+already wired in this note's historical parent. The merged tree retires the dispatch instrument;
+the two surviving batteries use `--release-build`. aof_frames.py is indirectly executed by aof_frame_order.py;
 aof_rewrite.py and aof_rewrite_triggers.py are driven by matrix wrappers; differ.py is driven by
 differ_gate.sh; niclib.sh and gate_refs.txt support optional full-tier NIC rows.
 
@@ -456,7 +457,7 @@
 notify: ok (notify_events_fired=1608)
 ~~~
 
-Purpose-boot matrix commands:
+Historical purpose-boot matrix commands (superseded by the current gate's network-engine grammar):
 
 ~~~sh
 PERSIST_IO=normal GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_matrix.sh
@@ -465,7 +466,8 @@
 PERSIST_IO=uring  GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_trigger_matrix.sh
 ~~~
 
-Other purpose-booted battery invocations (each boot used the knobs documented by gate.sh):
+Historical purpose-booted battery invocations (not recipes for the merged tree; the dispatch
+instrument and script OFF arm below are retired):
 
 ~~~sh
 python3 tests/xscript.py 127.0.0.1 7600 off|limit|window
diff --git a/NOTES-XPERF2.md b/NOTES-XPERF2.md
index d4ce7950b1396997d44f0945f6f5179aa625e59e..d1edc822a5e7abc68e6aefc3d5a55986b0bf178e
--- a/NOTES-XPERF2.md
+++ b/NOTES-XPERF2.md
@@ -8,7 +8,7 @@
 |---|---|---|---|---|
 | E4 | ExpireIndex never shrinks; memsets the whole sidecar on live→0; single-pass rehash | **REPRODUCED** (both halves) | yes | `tests/expireindex.py` |
 | E5 | zero-copy borrow registry is a linear vector scanned per borrow/release/retire | **REPRODUCED** | yes | `tests/borrow_registry.py` |
-| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes | `tests/xshard_dispatch_scale.sh` |
+| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes, historically | retired manual-map dispatch instrument; see `MERGE.md` |
 | E7 | segment mode is absorbing: once triggered every later reply is a malloc + iovec slot | **NOT REPRODUCED** | no | none |
 
 Rig: EPYC 9754, cores 48–63 only, ports 7200–7209 only, loopback. Oracle = vanilla redis 7.4 at
@@ -292,18 +292,20 @@
 |---|---|---|---|---|
 | `tests/expireindex.py <host> <port>` | `--shards 1 --ratio 1:1 --enable-debug-command yes` | growth-trigger cost ratio over a 16× population ≤ 8.0; live→0 DEL ≤ 1.25× a neighbouring DEL | FAIL 2 (16.30×, 2.00×) | PASS (1.65×, 1.00×) |
 | `tests/borrow_registry.py <host> <port>` | `--shards 1 --zc-min 64 --client-output-buffer-limit normal 0 0 0 --enable-debug-command yes` | borrowed-GET per-op cost growth over 0 → ~2000 live borrows ≤ 1.05 | FAIL (1.078, 1.079) | PASS (1.008, 1.008, 1.009) |
-| `tests/xshard_dispatch_scale.sh` (+ `.py`) | boots its own 4-thread and 128-thread arms | dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
+| Retired `tests/xshard_dispatch_scale.sh` (+ `.py`) | historically booted fixed-owner 4-thread and 128-thread arms | historical dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
 
 Each also carries its own non-vacuity checks: `expireindex.py` asserts INFO `expires` really
 reached the population under test and that INFO `expired_keys` MOVED (so the sampler is proven to
 still visit deadlines parked mid-migration), plus a no-TTL negative control on both timing legs;
 `borrow_registry.py` asserts `CLIENT LIST oll` grew (the borrow path is proven live, not assumed),
 carries a non-borrowed control arm, and refuses to run unless the zc gate straddles its two probe
-sizes; `xshard_dispatch_scale.py` refuses to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` prove
+sizes; the retired `xshard_dispatch_scale.py` refused to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` proved
 the cross pair really fanned out to two owner threads and the control stayed on one.
 
-None of the three is wired into `tests/gate.sh`: they each need their own boot geometry and the
-gate's port/cores belong to the mainline operator. Run them as above.
+These were standalone instruments when measured. The merged gate runs the expiry and borrow
+batteries with `--release-build`. The dispatch wrapper and helper are retired because their manual
+ownership geometry is unavailable; the historical measurements above do not validate default
+round-robin ownership. See [MERGE.md](MERGE.md).
 
 ## Evidence
 
diff --git a/docs/flipctl-design.md b/docs/flipctl-design.md
index e6a5e31a6c05171b172746f2586a01d09fa1e84e..cd96557199b408c8b85bb42b77ca59ee6fec73ea
--- a/docs/flipctl-design.md
+++ b/docs/flipctl-design.md
@@ -8,8 +8,7 @@
 ## Ownership and footprint
 
 - `src/main.cc` is the controller cron owner. When `flip-auto=1`, the main/monitor thread calls
-  `Server::flipctl_tick()` once per existing LB tick (`lb-tick-ms`; if that mechanism is disabled,
-  the period derives from the provisioned thread count). No IO or EX loop runs controller policy.
+  `Server::flipctl_tick()` on its internal observation cadence. No IO or EX loop runs controller policy.
 - `src/core/flipctl.{h,cc}` contains the fingerprint representation, shift detector, and controller
   state machine. Controller vectors allocate only when `flip-auto=1`.
 - Each `ThreadCtx` embeds one `FlipFingerprintWriter`. It has no dynamic storage. Only the physical
@@ -46,9 +45,9 @@
 measurement quantum, avoiding the zero-anchor discontinuity where one rare admin argument would
 otherwise have distance one.
 
-With `flip-auto-band=-1`, the band is twice the maximum quiet adjacent/drift distance learned at
-the final split, floored by the four-family sampling quantum divided by observed commands. A
-numeric value is a percent. Zero disables fingerprint and collapse re-triggers. Final anchor
+The automatic band is twice the maximum quiet adjacent/drift distance learned at
+the final split, floored by the four-family sampling quantum divided by observed commands.
+It has no public override. Final anchor
 learning consumes a number of closed aggregates derived from the provisioned thread count. It is
 done after maneuver-only age sampling is disarmed, so the controller cannot detect its own signal
 cost transition as a workload change.
@@ -64,13 +63,13 @@
 falls below a threshold derived from its own change jitter and measurement quantum. Directional
 connection ramp-up therefore remains drift, while jittery stationary traffic can qualify even when
 its absolute rate never enters a quiet band. A 30-second ceiling, converted to ticks from
-`lb-tick-ms`, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
+the internal observation cadence, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
 thread per tick resets the learning/cap state, so an idle server remains in
 `awaiting-load-stability` and never pays for a maneuver it cannot use.
 
-`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. An explicit
-`lb-age-sample-rate` is used when nonzero; otherwise the rate derives from the provisioned thread
-count. IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
+`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. Its rate
+derives from observed command volume and the three-tick decision window, and adapts on completed
+traffic windows; see [DESIGN-KNOBS.md](../DESIGN-KNOBS.md). IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
 writes. IO releases its ROB-head age map when the rate returns to zero.
 
 After a stabilized baseline rate, `issue_initial_jump()` subtracts the per-thread maneuver window.
@@ -88,8 +87,8 @@
 not vetoed by idle peers. If either role has no measured work/busy quantity, measurement continues;
 the controller does not invent a workload prior.
 
-The calculation is performed in placement units (one logical CPU normally, one sibling pair under
-`smt-mode=1`) and then reported/committed in threads. The target passes `nio_eq` in the measured
+The calculation is performed in placement units (one logical CPU normally, one sibling pair when
+the allowed topology contains complete pairs) and then reported/committed in threads. The target passes `nio_eq` in the measured
 direction by `max(1, abs(nio_eq-nio_now)/2)`, clamped to one live unit of each role. Even when the
 equalized split rounds to the current split, the direction of greater measured demand supplies the
 deliberate one-unit overshoot.
@@ -143,7 +142,7 @@
 
 ## Integration and observability
 
-- `src/core/config.h` is the sole parser/default source for `flip-auto`, `flip-auto-band`, and
+- `src/core/config.h` is the sole parser/default source for `flip-auto` and
   `flip-work-window`; `src/cmd/t_server.cc` exposes them as immutable CONFIG values.
 - `tomokv.conf` documents the numeric grammar and off/auto semantics.
 - `INFO FLIPCTL` reports state/phase, anchor split/rate, bands, total/per-reason trigger counts, and
diff --git a/docs/specs/AUDIT-TLS.md b/docs/specs/AUDIT-TLS.md
index 0256e887f5d27c210d436bc219f3c7521ddad0db..87a2eaca3137a47f7c0e1567794650f57531860b
--- a/docs/specs/AUDIT-TLS.md
+++ b/docs/specs/AUDIT-TLS.md
@@ -3,7 +3,7 @@
 > **Where it landed (2026-09-04).** The feature shipped as `src/net/tls.{h,cc}` (this document
 > proposes `src/net/tls_ctx.h` + `src/net/tls_conn.h`); the test is `tests/tls.py` (no
 > `tests/gen-certs.sh`); no `tls-bio-size` knob exists. Defaults: AES-128-GCM-first cipher lists
-> and `tls-ktls yes` (`tomokv.conf`, NETWORK).
+> and automatic kTLS negotiation with userspace fallback (`tomokv.conf`, NETWORK).
 
 Governing audit. Read before writing any TLS code in `/home/user/Projects/tomokv-cpp-perthread`.
 No repository was modified in producing this.
diff --git a/scratchpad/rlbatch/lib.sh b/scratchpad/rlbatch/lib.sh
index 7bcf3e4e6030c79174bf35d9bb1b3dfb11f80c42..12184314d2aafac0dc7aa48db761a3bdec5de8c7
--- a/scratchpad/rlbatch/lib.sh
+++ b/scratchpad/rlbatch/lib.sh
@@ -17,7 +17,7 @@
 boot_srv(){ # boot_srv <binary> <logfile> [extra args...] -> SRV pid
   local bin="$1" log="$2"; shift 2
   guard_port "$PORT" || return 1
-  # EXTRA_ARGS lets a caller add boot flags (e.g. --read-local-interleave 0) without a second copy
+  # EXTRA_ARGS lets a caller add boot flags (e.g. --lb 0) without a second copy
   # of this function; word splitting is intentional.
   # shellcheck disable=SC2086
   taskset -c "$SRVCORE" "$bin" --port "$PORT" --bind 127.0.0.1 --shards 16 \
diff --git a/tests/flipctl_unit.cc b/tests/flipctl_unit.cc
index 540a6c40e9589572431075b623bc553c76ee8d85..33209258e2958771d60f86b919f6f510e9977737
--- a/tests/flipctl_unit.cc
+++ b/tests/flipctl_unit.cc
@@ -184,11 +184,11 @@
 
     // THE TYPED BAND IS A FLOOR, NOT A CEILING. The gate-hygiene lane's instrumented failure: a
     // driver holding its rate to 0.07% across 34 samples, a fingerprint distance of 0.2518 against
-    // a flat --flip-auto-band 2 (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
+    // a flat explicit band of 2% (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
     // A detector whose signature genuinely moves between adjacent quiet windows must end up with a
     // band above that movement whatever the operator typed, and must still fire on a real change.
     {
-        FlipShiftDetector typed(2, 4);   // --flip-auto-band 2
+        FlipShiftDetector typed(2, 4);   // internal explicit-band control: 2%
         // Stationary but NOISY: the read/write split alternates 35:65 / 65:35 window to window.
         // The detector smooths the signature, so the anchored distance this produces is ~0.09.
         const auto noisy = [](int k) { return quiet(k % 2 ? 65 : 35, k % 2 ? 35 : 65); };
@@ -250,8 +250,8 @@
         if (fires) fail("the learned band fired on quiet-state movement");
     }
     // A TYPED BAND UNDER THE REAL SAMPLER. The shipped configuration cannot reach this -- the
-    // controller runs the learned band -- but an operator who sets --flip-auto-band AND the sampler
-    // can, and on a tree where the typed path bypasses the floor the fingerprint lane measured 73
+    // controller runs the learned band. This internal explicit-band control preserves the earlier
+    // regression: where the typed path bypassed the floor the fingerprint lane measured 73
     // two-consecutive exceedances in 600 stationary windows at K=100 and 208 at K=60: a spurious
     // maneuver every few seconds on a load that never changed. Driven here through THEIR writer, a
     // stationary 50/50 stream at depth 32, with the floor in place: none, and a real mix change
@@ -259,7 +259,7 @@
     {
         for (uint32_t window : {60u, 100u}) {
             XorShift rng;
-            FlipShiftDetector typed(2, 8);   // --flip-auto-band 2, the operator's knob
+            FlipShiftDetector typed(2, 8);   // internal explicit-band control: 2%
             int fires = 0, confirmed = 0, streak = 0;
             for (int w = 0; w < 60; w++) {
                 FlipFingerprintWriter writer;
diff --git a/tests/rlcache_churn.py b/tests/rlcache_churn.py
index b56ae4041ae0843dc67443f16017270063853124..f8a2c0730bb0a4fb6c3b5f6282c547226bb76599
--- a/tests/rlcache_churn.py
+++ b/tests/rlcache_churn.py
@@ -59,7 +59,7 @@
 
 
 # THE LOAD BALANCER MUST HAVE SOMETHING TO BALANCE. A uniform key distribution over one small key
-# space keeps every shard at the same op rate, the imbalance never crosses lb-imbalance-pct, and no
+# space keeps every shard at the same op rate, the imbalance never crosses the learned band, and no
 # shard ever changes owner -- which is the precondition for the defect this battery exists for
 # (measured: 7.8M ops, 0 migrations). A hot window that ROTATES makes a few shards hot, then makes
 # different ones hot, so the balancer moves shards continuously and in both directions, which is

exec
/bin/bash -lc "python3 - <<'PY'
from pathlib import Path
import difflib
expected = Path('.merge-inputs/knobs/tests/gate.sh').read_text()
gate = Path('tests/gate.sh').read_text()
for name in ('atomic_torn', 'expireindex', 'borrow_registry'):
    before = 'py tests/' + name + '.py 127.0.0.1 "'$PORT >'"'
    after = 'py tests/' + name + '.py 127.0.0.1 "'$PORT --release-build >'"'
    assert before in expected
    expected = expected.replace(before, after, 1)
for log in ('gate-"'$t-$AT'"', 'gate-fusedarmed-"'$t-$AT'"'):
    before = '    py tests/"'$t.py 127.0.0.1 $PORT >/tmp/'"' + log + '.txt 2>&1'
    after = '    FEATURE_ARGS=()\\n    [ \""'$t" = stream ] && FEATURE_ARGS+=(--release-build)'"\\n    py tests/"'$t.py 127.0.0.1 $PORT "${FEATURE_ARGS[@]}" >/tmp/'"' + log + '.txt 2>&1'
    assert before in expected
    expected = expected.replace(before, after, 1)
a = '# These assert a RATIO or a growth bound, never an absolute time, and each was verified to FAIL on\\n# the binary from before its fix (see NOTES-XPERF2.md). They shipped unwired, which is the same\\n# coverage-that-does-not-exist problem the gate lane fixed elsewhere.\\n'
b = '# Exact mechanism checks run on every build; timing budgets require the explicit release-build\\n# flag. A ratio of wall times is still a performance claim (see DESIGN-GATEHYGIENE.md).\\n'
assert a in expected
expected = expected.replace(a, b)
expected = expected.replace('# Dispatch scaling\\'s manual ownership geometry was retired with shard-home; DESIGN-KNOBS.md.', '# Dispatch scaling\\'s manual ownership geometry is retired; see MERGE.md and DESIGN-KNOBS.md.')
a = 'py tests/atomic_torn.py 127.0.0.1 "'$PORT >/tmp/gate-atomic-torn-asan.txt'"'
b = '# Deliberately omit --release-build: coverage/safety checks remain mandatory, including the\\n# derived-window witnesses; promotion timing prints without scoring its release budget.\\n' + a
assert a in expected
expected = expected.replace(a, b)
assert expected == gate, ''.join(difflib.unified_diff(expected.splitlines(True), gate.splitlines(True)))
print('Gate equals knobs input plus exactly the retained timing call-site changes and documented comment resolutions.')
atomic = Path('tests/atomic_torn.py').read_text()
timing = Path('.merge-inputs/timing/tests/atomic_torn.py').read_text()
knobs = Path('.merge-inputs/knobs/tests/atomic_torn.py').read_text()
head = '# Admission liveness:'
newhead = '# Separate admission liveness from live credit reconfiguration/accounting.'
tail = '# Live CONFIG flips under active traffic are a liveness/safety arm.'
expected = timing[:timing.index(head)] + knobs[knobs.index(newhead):knobs.index(tail)] + timing[timing.index(tail):]
assert expected == atomic
print('Atomic file equals timing input with exactly the documented knobs admission/reconfiguration replacement.')
PY" in /home/user/Projects/cx-merge
 succeeded in 0ms:
Gate equals knobs input plus exactly the retained timing call-site changes and documented comment resolutions.
Atomic file equals timing input with exactly the documented knobs admission/reconfiguration replacement.

diff --git a/AUDIT-TESTS.md b/AUDIT-TESTS.md
index e9cd43ffbc376172717531d0037de1c79fd23690..fe1fd0e2c39e965f605e4a30894a545129541534
--- a/AUDIT-TESTS.md
+++ b/AUDIT-TESTS.md
@@ -122,7 +122,7 @@
 | MM floor (memtier 20 s, sampled 8+6) | 1 | 14 | measurement, keep |
 | saturated-flip memtier | 1 | 25+3 | measurement, keep |
 | `flipctl.py --stable-seconds 30` (+ramp/idle/surge, timeout 300) | 1 | 60-120 est. | measurement, keep |
-| `xshard_dispatch_scale.sh` (2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 | 90-150 est. | measurement, keep |
+| Retired `xshard_dispatch_scale.sh` (historical 2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 historically, 0 now | 90-150 est. historically | manual ownership instrument removed; see `MERGE.md` |
 | `atomic_torn.py` hammers (3+2.5+2+2.5 s) + OFF re-rolls (up to 14 s) + RENAMENX/COPY parks (2 x 64 x 100 ms = 12.8 s) + window/lease/churn (~6 s) | 1 quick, +1 ASAN | ~45-60 each | re-rolls are dead time on this kernel (F6) |
 | `spinprobe.py` (6 s baseline + 6 s probe) | 3 | 37.5 | measurement, keep |
 | Python batteries' literal sleeps (census: flipctl 17 s, limits 5.8, edgetime 4.5, atomic_ryow 4, hexpire 3.4 — most run x2) | | ~60 | keep |
diff --git a/DESIGN-GATEHYGIENE.md b/DESIGN-GATEHYGIENE.md
index 5ea9df93e4548fb73a7e78b38debd52d876ad658..e6ac34a10326fe8880eaa664690832da3875b9b8
--- a/DESIGN-GATEHYGIENE.md
+++ b/DESIGN-GATEHYGIENE.md
@@ -459,7 +459,21 @@
 
 ---
 
-## 10. Follow-up diff on `78c3e5391` — 2026-09-07, runtime validation pending
+## 10. Incoming timing lane on `78c3e5391` — 2026-09-07
+
+**Merged disposition:** the maintainer reports the incoming timing lane gated 344/344 with no
+SKIP rows. The account below records that lane's original implementation and offline validation;
+it is not a validation claim for the merged tree. Its dispatch wrapper/helper and timing evaluator
+are retired: repeatability checks did not replace the deleted manual ownership instrument. The
+configurable-window shrink samples and shrink/reclaim budgets are superseded by the derived-window
+held-burst liveness and old-generation reconfiguration witnesses in `tests/atomicwindow.py`.
+All those mechanism, exact-reply, bound, and credit-reclamation checks remain mandatory on every
+build. The original OFF RENAME four-roll hard-failure control is unchanged, and the 1.5-second
+promotion budget still requires `--release-build`. The merged ledger requires quick 325/full 342;
+the constants remain untouched. [MERGE.md](MERGE.md) records the complete conflict and row accounting.
+
+The rest of this section is historical lane evidence, including its now-superseded dispatch,
+shrink, admission-calibration, and ledger statements.
 
 This is a test-only diff in `cx-timing`. No server, benchmark, gate, or compiler was run. The
 earlier acceptance results above describe earlier changes, **not this diff**. The worked fixes
diff --git a/DESIGN-flipfp.md b/DESIGN-flipfp.md
index 32eb6f46c24fb688db6accc79537fbd664bd084c..f705b25837c72a011da71df06dec09d2757f05ae
--- a/DESIGN-flipfp.md
+++ b/DESIGN-flipfp.md
@@ -311,7 +311,9 @@
 detector. The signal and the floor cannot be read from one run -- with a learned band the trigger
 fires and `reset()` wipes the distance -- so each arm is run twice: `--flip-auto-band 90` holds the
 anchor and lets the distance converge (the signal), `--flip-auto-band -1` learns the floor.
-`scratch/flipfp/fpmargin.sh`.
+The former `scratch/flipfp/fpmargin.sh` implemented this two-band experiment. It is retired in the
+merged tree because neither pinning the band nor disabling its retriggers has a public control.
+These historical measurements remain evidence only for their recorded builds; see `MERGE.md`.
 
 **BITCOUNT -> INCR** (the redesign lane's reference change), 2:2 split, `--rate-limiting` pinned:
 
diff --git a/DESIGN.md b/DESIGN.md
index 0fba740d94ba1915868410de40bcb685ae675833..7ad3ac9fef512d8549eadfce44036b8a6a181e9f
--- a/DESIGN.md
+++ b/DESIGN.md
@@ -1,6 +1,6 @@
 # Thread amortization study surface
 
-`thread-mode` and `overlap` are one boot-latched measurement surface. The documented mode
+`thread-mode` and the study-only `x-overlap` are one boot-latched measurement surface. The documented mode
 names are `2s` (separate IO and executor threads, the default) and `1s` (one generalized thread owns
 both roles). `split` and `fused` remain parser aliases so old invocations keep working.
 
@@ -9,10 +9,10 @@
 | `2s` | ordinary split IO/ex loops | exact `t-iopipe` WB/IFID batch schedule | rejected at config validation |
 | `1s` | coarse IFID → EX → WB rotation | exact `t-genthread` `iofused` schedule | gated `iofused` three-way schedule |
 
-The compatibility options `thread-pipeline` and `genthread-schedule` remain accepted: the former
-is a numeric alias for `overlap`, while the latter maps `coarse`, `iofused`, and the legacy
-`streams` name to `1s` plus overlap 0, 1, and 2 respectively. The legacy name is only a parser
-alias; overlap 2 no longer runs the streams implementation. Overlap 2 emits a boot warning because
+Use `--x-overlap 0|1|2` for these study schedules and `--x-ex-sched 0|1` for the executor study.
+The old public schedule spellings and compatibility aliases are removed; see
+[DESIGN-KNOBS.md](DESIGN-KNOBS.md). Below, "overlap" names the internal schedule value.
+Overlap 2 emits a boot warning because
 it is an experimental research schedule. Unified overlap 1 and 2 require `net-io uring`; the split
 iopipe schedule retains its explicit epoll specialization. No schedule constant is runtime-tunable.
 
diff --git a/NOTES-HYGIENE.md b/NOTES-HYGIENE.md
index 23506bce64f00cb0dccaaff93362ccd30deab229..f224db07f4ceb2e4791c7cbb471b87d83a0a3e93
--- a/NOTES-HYGIENE.md
+++ b/NOTES-HYGIENE.md
@@ -352,7 +352,8 @@
 ## Test-file census
 
 The calibration examples expireindex.py, borrow_registry.py, and xshard_dispatch_scale.sh were
-already wired in the current parent. aof_frames.py is indirectly executed by aof_frame_order.py;
+already wired in this note's historical parent. The merged tree retires the dispatch instrument;
+the two surviving batteries use `--release-build`. aof_frames.py is indirectly executed by aof_frame_order.py;
 aof_rewrite.py and aof_rewrite_triggers.py are driven by matrix wrappers; differ.py is driven by
 differ_gate.sh; niclib.sh and gate_refs.txt support optional full-tier NIC rows.
 
@@ -456,7 +457,7 @@
 notify: ok (notify_events_fired=1608)
 ~~~
 
-Purpose-boot matrix commands:
+Historical purpose-boot matrix commands (superseded by the current gate's network-engine grammar):
 
 ~~~sh
 PERSIST_IO=normal GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_matrix.sh
@@ -465,7 +466,8 @@
 PERSIST_IO=uring  GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_trigger_matrix.sh
 ~~~
 
-Other purpose-booted battery invocations (each boot used the knobs documented by gate.sh):
+Historical purpose-booted battery invocations (not recipes for the merged tree; the dispatch
+instrument and script OFF arm below are retired):
 
 ~~~sh
 python3 tests/xscript.py 127.0.0.1 7600 off|limit|window
diff --git a/NOTES-XPERF2.md b/NOTES-XPERF2.md
index d4ce7950b1396997d44f0945f6f5179aa625e59e..d1edc822a5e7abc68e6aefc3d5a55986b0bf178e
--- a/NOTES-XPERF2.md
+++ b/NOTES-XPERF2.md
@@ -8,7 +8,7 @@
 |---|---|---|---|---|
 | E4 | ExpireIndex never shrinks; memsets the whole sidecar on live→0; single-pass rehash | **REPRODUCED** (both halves) | yes | `tests/expireindex.py` |
 | E5 | zero-copy borrow registry is a linear vector scanned per borrow/release/retire | **REPRODUCED** | yes | `tests/borrow_registry.py` |
-| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes | `tests/xshard_dispatch_scale.sh` |
+| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes, historically | retired manual-map dispatch instrument; see `MERGE.md` |
 | E7 | segment mode is absorbing: once triggered every later reply is a malloc + iovec slot | **NOT REPRODUCED** | no | none |
 
 Rig: EPYC 9754, cores 48–63 only, ports 7200–7209 only, loopback. Oracle = vanilla redis 7.4 at
@@ -292,18 +292,20 @@
 |---|---|---|---|---|
 | `tests/expireindex.py <host> <port>` | `--shards 1 --ratio 1:1 --enable-debug-command yes` | growth-trigger cost ratio over a 16× population ≤ 8.0; live→0 DEL ≤ 1.25× a neighbouring DEL | FAIL 2 (16.30×, 2.00×) | PASS (1.65×, 1.00×) |
 | `tests/borrow_registry.py <host> <port>` | `--shards 1 --zc-min 64 --client-output-buffer-limit normal 0 0 0 --enable-debug-command yes` | borrowed-GET per-op cost growth over 0 → ~2000 live borrows ≤ 1.05 | FAIL (1.078, 1.079) | PASS (1.008, 1.008, 1.009) |
-| `tests/xshard_dispatch_scale.sh` (+ `.py`) | boots its own 4-thread and 128-thread arms | dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
+| Retired `tests/xshard_dispatch_scale.sh` (+ `.py`) | historically booted fixed-owner 4-thread and 128-thread arms | historical dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
 
 Each also carries its own non-vacuity checks: `expireindex.py` asserts INFO `expires` really
 reached the population under test and that INFO `expired_keys` MOVED (so the sampler is proven to
 still visit deadlines parked mid-migration), plus a no-TTL negative control on both timing legs;
 `borrow_registry.py` asserts `CLIENT LIST oll` grew (the borrow path is proven live, not assumed),
 carries a non-borrowed control arm, and refuses to run unless the zc gate straddles its two probe
-sizes; `xshard_dispatch_scale.py` refuses to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` prove
+sizes; the retired `xshard_dispatch_scale.py` refused to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` proved
 the cross pair really fanned out to two owner threads and the control stayed on one.
 
-None of the three is wired into `tests/gate.sh`: they each need their own boot geometry and the
-gate's port/cores belong to the mainline operator. Run them as above.
+These were standalone instruments when measured. The merged gate runs the expiry and borrow
+batteries with `--release-build`. The dispatch wrapper and helper are retired because their manual
+ownership geometry is unavailable; the historical measurements above do not validate default
+round-robin ownership. See [MERGE.md](MERGE.md).
 
 ## Evidence
 
diff --git a/docs/flipctl-design.md b/docs/flipctl-design.md
index e6a5e31a6c05171b172746f2586a01d09fa1e84e..cd96557199b408c8b85bb42b77ca59ee6fec73ea
--- a/docs/flipctl-design.md
+++ b/docs/flipctl-design.md
@@ -8,8 +8,7 @@
 ## Ownership and footprint
 
 - `src/main.cc` is the controller cron owner. When `flip-auto=1`, the main/monitor thread calls
-  `Server::flipctl_tick()` once per existing LB tick (`lb-tick-ms`; if that mechanism is disabled,
-  the period derives from the provisioned thread count). No IO or EX loop runs controller policy.
+  `Server::flipctl_tick()` on its internal observation cadence. No IO or EX loop runs controller policy.
 - `src/core/flipctl.{h,cc}` contains the fingerprint representation, shift detector, and controller
   state machine. Controller vectors allocate only when `flip-auto=1`.
 - Each `ThreadCtx` embeds one `FlipFingerprintWriter`. It has no dynamic storage. Only the physical
@@ -46,9 +45,9 @@
 measurement quantum, avoiding the zero-anchor discontinuity where one rare admin argument would
 otherwise have distance one.
 
-With `flip-auto-band=-1`, the band is twice the maximum quiet adjacent/drift distance learned at
-the final split, floored by the four-family sampling quantum divided by observed commands. A
-numeric value is a percent. Zero disables fingerprint and collapse re-triggers. Final anchor
+The automatic band is twice the maximum quiet adjacent/drift distance learned at
+the final split, floored by the four-family sampling quantum divided by observed commands.
+It has no public override. Final anchor
 learning consumes a number of closed aggregates derived from the provisioned thread count. It is
 done after maneuver-only age sampling is disarmed, so the controller cannot detect its own signal
 cost transition as a workload change.
@@ -64,13 +63,13 @@
 falls below a threshold derived from its own change jitter and measurement quantum. Directional
 connection ramp-up therefore remains drift, while jittery stationary traffic can qualify even when
 its absolute rate never enters a quiet band. A 30-second ceiling, converted to ticks from
-`lb-tick-ms`, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
+the internal observation cadence, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
 thread per tick resets the learning/cap state, so an idle server remains in
 `awaiting-load-stability` and never pays for a maneuver it cannot use.
 
-`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. An explicit
-`lb-age-sample-rate` is used when nonzero; otherwise the rate derives from the provisioned thread
-count. IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
+`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. Its rate
+derives from observed command volume and the three-tick decision window, and adapts on completed
+traffic windows; see [DESIGN-KNOBS.md](../DESIGN-KNOBS.md). IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
 writes. IO releases its ROB-head age map when the rate returns to zero.
 
 After a stabilized baseline rate, `issue_initial_jump()` subtracts the per-thread maneuver window.
@@ -88,8 +87,8 @@
 not vetoed by idle peers. If either role has no measured work/busy quantity, measurement continues;
 the controller does not invent a workload prior.
 
-The calculation is performed in placement units (one logical CPU normally, one sibling pair under
-`smt-mode=1`) and then reported/committed in threads. The target passes `nio_eq` in the measured
+The calculation is performed in placement units (one logical CPU normally, one sibling pair when
+the allowed topology contains complete pairs) and then reported/committed in threads. The target passes `nio_eq` in the measured
 direction by `max(1, abs(nio_eq-nio_now)/2)`, clamped to one live unit of each role. Even when the
 equalized split rounds to the current split, the direction of greater measured demand supplies the
 deliberate one-unit overshoot.
@@ -143,7 +142,7 @@
 
 ## Integration and observability
 
-- `src/core/config.h` is the sole parser/default source for `flip-auto`, `flip-auto-band`, and
+- `src/core/config.h` is the sole parser/default source for `flip-auto` and
   `flip-work-window`; `src/cmd/t_server.cc` exposes them as immutable CONFIG values.
 - `tomokv.conf` documents the numeric grammar and off/auto semantics.
 - `INFO FLIPCTL` reports state/phase, anchor split/rate, bands, total/per-reason trigger counts, and
diff --git a/docs/specs/AUDIT-TLS.md b/docs/specs/AUDIT-TLS.md
index 0256e887f5d27c210d436bc219f3c7521ddad0db..87a2eaca3137a47f7c0e1567794650f57531860b
--- a/docs/specs/AUDIT-TLS.md
+++ b/docs/specs/AUDIT-TLS.md
@@ -3,7 +3,7 @@
 > **Where it landed (2026-09-04).** The feature shipped as `src/net/tls.{h,cc}` (this document
 > proposes `src/net/tls_ctx.h` + `src/net/tls_conn.h`); the test is `tests/tls.py` (no
 > `tests/gen-certs.sh`); no `tls-bio-size` knob exists. Defaults: AES-128-GCM-first cipher lists
-> and `tls-ktls yes` (`tomokv.conf`, NETWORK).
+> and automatic kTLS negotiation with userspace fallback (`tomokv.conf`, NETWORK).
 
 Governing audit. Read before writing any TLS code in `/home/user/Projects/tomokv-cpp-perthread`.
 No repository was modified in producing this.
diff --git a/scratchpad/rlbatch/lib.sh b/scratchpad/rlbatch/lib.sh
index 7bcf3e4e6030c79174bf35d9bb1b3dfb11f80c42..12184314d2aafac0dc7aa48db761a3bdec5de8c7
--- a/scratchpad/rlbatch/lib.sh
+++ b/scratchpad/rlbatch/lib.sh
@@ -17,7 +17,7 @@
 boot_srv(){ # boot_srv <binary> <logfile> [extra args...] -> SRV pid
   local bin="$1" log="$2"; shift 2
   guard_port "$PORT" || return 1
-  # EXTRA_ARGS lets a caller add boot flags (e.g. --read-local-interleave 0) without a second copy
+  # EXTRA_ARGS lets a caller add boot flags (e.g. --lb 0) without a second copy
   # of this function; word splitting is intentional.
   # shellcheck disable=SC2086
   taskset -c "$SRVCORE" "$bin" --port "$PORT" --bind 127.0.0.1 --shards 16 \
diff --git a/tests/flipctl_unit.cc b/tests/flipctl_unit.cc
index 540a6c40e9589572431075b623bc553c76ee8d85..33209258e2958771d60f86b919f6f510e9977737
--- a/tests/flipctl_unit.cc
+++ b/tests/flipctl_unit.cc
@@ -184,11 +184,11 @@
 
     // THE TYPED BAND IS A FLOOR, NOT A CEILING. The gate-hygiene lane's instrumented failure: a
     // driver holding its rate to 0.07% across 34 samples, a fingerprint distance of 0.2518 against
-    // a flat --flip-auto-band 2 (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
+    // a flat explicit band of 2% (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
     // A detector whose signature genuinely moves between adjacent quiet windows must end up with a
     // band above that movement whatever the operator typed, and must still fire on a real change.
     {
-        FlipShiftDetector typed(2, 4);   // --flip-auto-band 2
+        FlipShiftDetector typed(2, 4);   // internal explicit-band control: 2%
         // Stationary but NOISY: the read/write split alternates 35:65 / 65:35 window to window.
         // The detector smooths the signature, so the anchored distance this produces is ~0.09.
         const auto noisy = [](int k) { return quiet(k % 2 ? 65 : 35, k % 2 ? 35 : 65); };
@@ -250,8 +250,8 @@
         if (fires) fail("the learned band fired on quiet-state movement");
     }
     // A TYPED BAND UNDER THE REAL SAMPLER. The shipped configuration cannot reach this -- the
-    // controller runs the learned band -- but an operator who sets --flip-auto-band AND the sampler
-    // can, and on a tree where the typed path bypasses the floor the fingerprint lane measured 73
+    // controller runs the learned band. This internal explicit-band control preserves the earlier
+    // regression: where the typed path bypassed the floor the fingerprint lane measured 73
     // two-consecutive exceedances in 600 stationary windows at K=100 and 208 at K=60: a spurious
     // maneuver every few seconds on a load that never changed. Driven here through THEIR writer, a
     // stationary 50/50 stream at depth 32, with the floor in place: none, and a real mix change
@@ -259,7 +259,7 @@
     {
         for (uint32_t window : {60u, 100u}) {
             XorShift rng;
-            FlipShiftDetector typed(2, 8);   // --flip-auto-band 2, the operator's knob
+            FlipShiftDetector typed(2, 8);   // internal explicit-band control: 2%
             int fires = 0, confirmed = 0, streak = 0;
             for (int w = 0; w < 60; w++) {
                 FlipFingerprintWriter writer;
diff --git a/tests/rlcache_churn.py b/tests/rlcache_churn.py
index b56ae4041ae0843dc67443f16017270063853124..f8a2c0730bb0a4fb6c3b5f6282c547226bb76599
--- a/tests/rlcache_churn.py
+++ b/tests/rlcache_churn.py
@@ -59,7 +59,7 @@
 
 
 # THE LOAD BALANCER MUST HAVE SOMETHING TO BALANCE. A uniform key distribution over one small key
-# space keeps every shard at the same op rate, the imbalance never crosses lb-imbalance-pct, and no
+# space keeps every shard at the same op rate, the imbalance never crosses the learned band, and no
 # shard ever changes owner -- which is the precondition for the defect this battery exists for
 # (measured: 7.8M ops, 0 migrations). A hot window that ROTATES makes a few shards hot, then makes
 # different ones hot, so the balancer moves shards continuously and in both directions, which is

apply patch
patch: completed
/home/user/Projects/cx-merge/DESIGN.md
/home/user/Projects/cx-merge/MERGE.md
/home/user/Projects/cx-merge/docs/flipctl-design.md
diff --git a/AUDIT-TESTS.md b/AUDIT-TESTS.md
index e9cd43ffbc376172717531d0037de1c79fd23690..fe1fd0e2c39e965f605e4a30894a545129541534
--- a/AUDIT-TESTS.md
+++ b/AUDIT-TESTS.md
@@ -122,7 +122,7 @@
 | MM floor (memtier 20 s, sampled 8+6) | 1 | 14 | measurement, keep |
 | saturated-flip memtier | 1 | 25+3 | measurement, keep |
 | `flipctl.py --stable-seconds 30` (+ramp/idle/surge, timeout 300) | 1 | 60-120 est. | measurement, keep |
-| `xshard_dispatch_scale.sh` (2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 | 90-150 est. | measurement, keep |
+| Retired `xshard_dispatch_scale.sh` (historical 2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 historically, 0 now | 90-150 est. historically | manual ownership instrument removed; see `MERGE.md` |
 | `atomic_torn.py` hammers (3+2.5+2+2.5 s) + OFF re-rolls (up to 14 s) + RENAMENX/COPY parks (2 x 64 x 100 ms = 12.8 s) + window/lease/churn (~6 s) | 1 quick, +1 ASAN | ~45-60 each | re-rolls are dead time on this kernel (F6) |
 | `spinprobe.py` (6 s baseline + 6 s probe) | 3 | 37.5 | measurement, keep |
 | Python batteries' literal sleeps (census: flipctl 17 s, limits 5.8, edgetime 4.5, atomic_ryow 4, hexpire 3.4 — most run x2) | | ~60 | keep |
diff --git a/DESIGN-GATEHYGIENE.md b/DESIGN-GATEHYGIENE.md
index 5ea9df93e4548fb73a7e78b38debd52d876ad658..e6ac34a10326fe8880eaa664690832da3875b9b8
--- a/DESIGN-GATEHYGIENE.md
+++ b/DESIGN-GATEHYGIENE.md
@@ -459,7 +459,21 @@
 
 ---
 
-## 10. Follow-up diff on `78c3e5391` — 2026-09-07, runtime validation pending
+## 10. Incoming timing lane on `78c3e5391` — 2026-09-07
+
+**Merged disposition:** the maintainer reports the incoming timing lane gated 344/344 with no
+SKIP rows. The account below records that lane's original implementation and offline validation;
+it is not a validation claim for the merged tree. Its dispatch wrapper/helper and timing evaluator
+are retired: repeatability checks did not replace the deleted manual ownership instrument. The
+configurable-window shrink samples and shrink/reclaim budgets are superseded by the derived-window
+held-burst liveness and old-generation reconfiguration witnesses in `tests/atomicwindow.py`.
+All those mechanism, exact-reply, bound, and credit-reclamation checks remain mandatory on every
+build. The original OFF RENAME four-roll hard-failure control is unchanged, and the 1.5-second
+promotion budget still requires `--release-build`. The merged ledger requires quick 325/full 342;
+the constants remain untouched. [MERGE.md](MERGE.md) records the complete conflict and row accounting.
+
+The rest of this section is historical lane evidence, including its now-superseded dispatch,
+shrink, admission-calibration, and ledger statements.
 
 This is a test-only diff in `cx-timing`. No server, benchmark, gate, or compiler was run. The
 earlier acceptance results above describe earlier changes, **not this diff**. The worked fixes
diff --git a/DESIGN-flipfp.md b/DESIGN-flipfp.md
index 32eb6f46c24fb688db6accc79537fbd664bd084c..f705b25837c72a011da71df06dec09d2757f05ae
--- a/DESIGN-flipfp.md
+++ b/DESIGN-flipfp.md
@@ -311,7 +311,9 @@
 detector. The signal and the floor cannot be read from one run -- with a learned band the trigger
 fires and `reset()` wipes the distance -- so each arm is run twice: `--flip-auto-band 90` holds the
 anchor and lets the distance converge (the signal), `--flip-auto-band -1` learns the floor.
-`scratch/flipfp/fpmargin.sh`.
+The former `scratch/flipfp/fpmargin.sh` implemented this two-band experiment. It is retired in the
+merged tree because neither pinning the band nor disabling its retriggers has a public control.
+These historical measurements remain evidence only for their recorded builds; see `MERGE.md`.
 
 **BITCOUNT -> INCR** (the redesign lane's reference change), 2:2 split, `--rate-limiting` pinned:
 
diff --git a/DESIGN.md b/DESIGN.md
index 0fba740d94ba1915868410de40bcb685ae675833..262f7b730cc2cedfdcffb964afb729c758dc97fc
--- a/DESIGN.md
+++ b/DESIGN.md
@@ -1,6 +1,6 @@
 # Thread amortization study surface
 
-`thread-mode` and `overlap` are one boot-latched measurement surface. The documented mode
+`thread-mode` and the study-only `x-overlap` are one boot-latched measurement surface. The mode
 names are `2s` (separate IO and executor threads, the default) and `1s` (one generalized thread owns
 both roles). `split` and `fused` remain parser aliases so old invocations keep working.
 
@@ -9,11 +9,11 @@
 | `2s` | ordinary split IO/ex loops | exact `t-iopipe` WB/IFID batch schedule | rejected at config validation |
 | `1s` | coarse IFID → EX → WB rotation | exact `t-genthread` `iofused` schedule | gated `iofused` three-way schedule |
 
-The compatibility options `thread-pipeline` and `genthread-schedule` remain accepted: the former
-is a numeric alias for `overlap`, while the latter maps `coarse`, `iofused`, and the legacy
-`streams` name to `1s` plus overlap 0, 1, and 2 respectively. The legacy name is only a parser
-alias; overlap 2 no longer runs the streams implementation. Overlap 2 emits a boot warning because
-it is an experimental research schedule. Unified overlap 1 and 2 require `net-io uring`; the split
+Use `--x-overlap 0|1|2` for these study schedules and `--x-ex-sched 0|1` for the executor study.
+The old public schedule spellings and compatibility aliases are removed; see
+[DESIGN-KNOBS.md](DESIGN-KNOBS.md). Below, "overlap" names the internal schedule value.
+Overlap 2 emits a boot warning because it is an experimental research schedule.
+Unified overlap 1 and 2 require `net-io uring`; the split
 iopipe schedule retains its explicit epoll specialization. No schedule constant is runtime-tunable.
 
 Overlap 1 is deliberately the measured implementation, not a family resemblance. In `2s`, its
diff --git a/MERGE.md b/MERGE.md
index e29579036e98115056aa3e3a5401db083d2b7820..d70892fe599ab8e1e4e0c67b2a54cc390f585ea5
--- a/MERGE.md
+++ b/MERGE.md
@@ -59,7 +59,7 @@
 
 ### OFF RENAME and the atomic timing split
 
-The complete base block from “Broadened movers: RENAME” through the following store-family
+The complete base block from “Broadened movers: RENAME” up to the following store-family
 section is byte-for-byte preserved. It still runs `for _roll in range(4)` at
 `tests/atomic_torn.py:771`, arms and clears the hop delay in `try/finally`, retries only a
 clean miss, and stops on a witnessed tear, worker error, or surviving helper. The OFF assertion
@@ -323,6 +323,9 @@
 - Input-path/hash accounting, full-file comparisons for retained lane files, static gate-row
   comparison and loop arithmetic, unchanged EXPECT assignments, layout-assertion inspection,
   removed-flag/caller search, conflict-marker search, and `git diff --check`.
+- Reconstructed each merged conflict file from the captured inputs: the atomic file equals
+  timing plus exactly the documented knobs admission replacement; the gate equals knobs plus
+  exactly the surviving timing call-site changes and the documented comment resolutions.
 
 No C++ syntax check, build, server, benchmark, full test, or gate result is claimed. The
 maintainer supplies the merged runtime and performance verdict.
diff --git a/NOTES-HYGIENE.md b/NOTES-HYGIENE.md
index 23506bce64f00cb0dccaaff93362ccd30deab229..f224db07f4ceb2e4791c7cbb471b87d83a0a3e93
--- a/NOTES-HYGIENE.md
+++ b/NOTES-HYGIENE.md
@@ -352,7 +352,8 @@
 ## Test-file census
 
 The calibration examples expireindex.py, borrow_registry.py, and xshard_dispatch_scale.sh were
-already wired in the current parent. aof_frames.py is indirectly executed by aof_frame_order.py;
+already wired in this note's historical parent. The merged tree retires the dispatch instrument;
+the two surviving batteries use `--release-build`. aof_frames.py is indirectly executed by aof_frame_order.py;
 aof_rewrite.py and aof_rewrite_triggers.py are driven by matrix wrappers; differ.py is driven by
 differ_gate.sh; niclib.sh and gate_refs.txt support optional full-tier NIC rows.
 
@@ -456,7 +457,7 @@
 notify: ok (notify_events_fired=1608)
 ~~~
 
-Purpose-boot matrix commands:
+Historical purpose-boot matrix commands (superseded by the current gate's network-engine grammar):
 
 ~~~sh
 PERSIST_IO=normal GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_matrix.sh
@@ -465,7 +466,8 @@
 PERSIST_IO=uring  GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_trigger_matrix.sh
 ~~~
 
-Other purpose-booted battery invocations (each boot used the knobs documented by gate.sh):
+Historical purpose-booted battery invocations (not recipes for the merged tree; the dispatch
+instrument and script OFF arm below are retired):
 
 ~~~sh
 python3 tests/xscript.py 127.0.0.1 7600 off|limit|window
diff --git a/NOTES-XPERF2.md b/NOTES-XPERF2.md
index d4ce7950b1396997d44f0945f6f5179aa625e59e..d1edc822a5e7abc68e6aefc3d5a55986b0bf178e
--- a/NOTES-XPERF2.md
+++ b/NOTES-XPERF2.md
@@ -8,7 +8,7 @@
 |---|---|---|---|---|
 | E4 | ExpireIndex never shrinks; memsets the whole sidecar on live→0; single-pass rehash | **REPRODUCED** (both halves) | yes | `tests/expireindex.py` |
 | E5 | zero-copy borrow registry is a linear vector scanned per borrow/release/retire | **REPRODUCED** | yes | `tests/borrow_registry.py` |
-| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes | `tests/xshard_dispatch_scale.sh` |
+| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes, historically | retired manual-map dispatch instrument; see `MERGE.md` |
 | E7 | segment mode is absorbing: once triggered every later reply is a malloc + iovec slot | **NOT REPRODUCED** | no | none |
 
 Rig: EPYC 9754, cores 48–63 only, ports 7200–7209 only, loopback. Oracle = vanilla redis 7.4 at
@@ -292,18 +292,20 @@
 |---|---|---|---|---|
 | `tests/expireindex.py <host> <port>` | `--shards 1 --ratio 1:1 --enable-debug-command yes` | growth-trigger cost ratio over a 16× population ≤ 8.0; live→0 DEL ≤ 1.25× a neighbouring DEL | FAIL 2 (16.30×, 2.00×) | PASS (1.65×, 1.00×) |
 | `tests/borrow_registry.py <host> <port>` | `--shards 1 --zc-min 64 --client-output-buffer-limit normal 0 0 0 --enable-debug-command yes` | borrowed-GET per-op cost growth over 0 → ~2000 live borrows ≤ 1.05 | FAIL (1.078, 1.079) | PASS (1.008, 1.008, 1.009) |
-| `tests/xshard_dispatch_scale.sh` (+ `.py`) | boots its own 4-thread and 128-thread arms | dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
+| Retired `tests/xshard_dispatch_scale.sh` (+ `.py`) | historically booted fixed-owner 4-thread and 128-thread arms | historical dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
 
 Each also carries its own non-vacuity checks: `expireindex.py` asserts INFO `expires` really
 reached the population under test and that INFO `expired_keys` MOVED (so the sampler is proven to
 still visit deadlines parked mid-migration), plus a no-TTL negative control on both timing legs;
 `borrow_registry.py` asserts `CLIENT LIST oll` grew (the borrow path is proven live, not assumed),
 carries a non-borrowed control arm, and refuses to run unless the zc gate straddles its two probe
-sizes; `xshard_dispatch_scale.py` refuses to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` prove
+sizes; the retired `xshard_dispatch_scale.py` refused to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` proved
 the cross pair really fanned out to two owner threads and the control stayed on one.
 
-None of the three is wired into `tests/gate.sh`: they each need their own boot geometry and the
-gate's port/cores belong to the mainline operator. Run them as above.
+These were standalone instruments when measured. The merged gate runs the expiry and borrow
+batteries with `--release-build`. The dispatch wrapper and helper are retired because their manual
+ownership geometry is unavailable; the historical measurements above do not validate default
+round-robin ownership. See [MERGE.md](MERGE.md).
 
 ## Evidence
 
diff --git a/docs/flipctl-design.md b/docs/flipctl-design.md
index e6a5e31a6c05171b172746f2586a01d09fa1e84e..9280bc6ad5f9b98d9535c141a4e45f6460962398
--- a/docs/flipctl-design.md
+++ b/docs/flipctl-design.md
@@ -8,8 +8,7 @@
 ## Ownership and footprint
 
 - `src/main.cc` is the controller cron owner. When `flip-auto=1`, the main/monitor thread calls
-  `Server::flipctl_tick()` once per existing LB tick (`lb-tick-ms`; if that mechanism is disabled,
-  the period derives from the provisioned thread count). No IO or EX loop runs controller policy.
+  `Server::flipctl_tick()` on its internal observation cadence. No IO or EX loop runs controller policy.
 - `src/core/flipctl.{h,cc}` contains the fingerprint representation, shift detector, and controller
   state machine. Controller vectors allocate only when `flip-auto=1`.
 - Each `ThreadCtx` embeds one `FlipFingerprintWriter`. It has no dynamic storage. Only the physical
@@ -46,10 +45,10 @@
 measurement quantum, avoiding the zero-anchor discontinuity where one rare admin argument would
 otherwise have distance one.
 
-With `flip-auto-band=-1`, the band is twice the maximum quiet adjacent/drift distance learned at
-the final split, floored by the four-family sampling quantum divided by observed commands. A
-numeric value is a percent. Zero disables fingerprint and collapse re-triggers. Final anchor
-learning consumes a number of closed aggregates derived from the provisioned thread count. It is
+The automatic band is twice the maximum quiet adjacent/drift distance learned at
+the final split, floored by the four-family sampling quantum divided by observed commands.
+It has no public override. Final anchor learning consumes a number of closed aggregates derived
+from the provisioned thread count. It is
 done after maneuver-only age sampling is disarmed, so the controller cannot detect its own signal
 cost transition as a workload change.
 
@@ -64,14 +63,15 @@
 falls below a threshold derived from its own change jitter and measurement quantum. Directional
 connection ramp-up therefore remains drift, while jittery stationary traffic can qualify even when
 its absolute rate never enters a quiet band. A 30-second ceiling, converted to ticks from
-`lb-tick-ms`, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
-thread per tick resets the learning/cap state, so an idle server remains in
+the internal observation cadence, caps deferral under non-idle traffic. Traffic at or below one
+command per provisioned thread per tick resets the learning/cap state, so an idle server remains in
 `awaiting-load-stability` and never pays for a maneuver it cannot use.
 
-`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. An explicit
-`lb-age-sample-rate` is used when nonzero; otherwise the rate derives from the provisioned thread
-count. IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
-writes. IO releases its ROB-head age map when the rate returns to zero.
+`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. Its rate
+derives from observed command volume and the three-tick decision window, and adapts on completed
+traffic windows; see [DESIGN-KNOBS.md](../DESIGN-KNOBS.md). IO and EX loops apply the published
+rate to their own `LoopSignals`, preserving single-owner writes. IO releases its ROB-head age
+map when the rate returns to zero.
 
 After a stabilized baseline rate, `issue_initial_jump()` subtracts the per-thread maneuver window.
 For each role it computes:
@@ -88,9 +88,10 @@
 not vetoed by idle peers. If either role has no measured work/busy quantity, measurement continues;
 the controller does not invent a workload prior.
 
-The calculation is performed in placement units (one logical CPU normally, one sibling pair under
-`smt-mode=1`) and then reported/committed in threads. The target passes `nio_eq` in the measured
-direction by `max(1, abs(nio_eq-nio_now)/2)`, clamped to one live unit of each role. Even when the
+The calculation is performed in placement units (one logical CPU normally, one sibling pair when
+the allowed topology contains complete pairs) and then reported/committed in threads. The target
+passes `nio_eq` in the measured direction by `max(1, abs(nio_eq-nio_now)/2)`, clamped to one live
+unit of each role. Even when the
 equalized split rounds to the current split, the direction of greater measured demand supplies the
 deliberate one-unit overshoot.
 
@@ -143,7 +144,7 @@
 
 ## Integration and observability
 
-- `src/core/config.h` is the sole parser/default source for `flip-auto`, `flip-auto-band`, and
+- `src/core/config.h` is the sole parser/default source for `flip-auto` and
   `flip-work-window`; `src/cmd/t_server.cc` exposes them as immutable CONFIG values.
 - `tomokv.conf` documents the numeric grammar and off/auto semantics.
 - `INFO FLIPCTL` reports state/phase, anchor split/rate, bands, total/per-reason trigger counts, and
diff --git a/docs/specs/AUDIT-TLS.md b/docs/specs/AUDIT-TLS.md
index 0256e887f5d27c210d436bc219f3c7521ddad0db..87a2eaca3137a47f7c0e1567794650f57531860b
--- a/docs/specs/AUDIT-TLS.md
+++ b/docs/specs/AUDIT-TLS.md
@@ -3,7 +3,7 @@
 > **Where it landed (2026-09-04).** The feature shipped as `src/net/tls.{h,cc}` (this document
 > proposes `src/net/tls_ctx.h` + `src/net/tls_conn.h`); the test is `tests/tls.py` (no
 > `tests/gen-certs.sh`); no `tls-bio-size` knob exists. Defaults: AES-128-GCM-first cipher lists
-> and `tls-ktls yes` (`tomokv.conf`, NETWORK).
+> and automatic kTLS negotiation with userspace fallback (`tomokv.conf`, NETWORK).
 
 Governing audit. Read before writing any TLS code in `/home/user/Projects/tomokv-cpp-perthread`.
 No repository was modified in producing this.
diff --git a/scratchpad/rlbatch/lib.sh b/scratchpad/rlbatch/lib.sh
index 7bcf3e4e6030c79174bf35d9bb1b3dfb11f80c42..12184314d2aafac0dc7aa48db761a3bdec5de8c7
--- a/scratchpad/rlbatch/lib.sh
+++ b/scratchpad/rlbatch/lib.sh
@@ -17,7 +17,7 @@
 boot_srv(){ # boot_srv <binary> <logfile> [extra args...] -> SRV pid
   local bin="$1" log="$2"; shift 2
   guard_port "$PORT" || return 1
-  # EXTRA_ARGS lets a caller add boot flags (e.g. --read-local-interleave 0) without a second copy
+  # EXTRA_ARGS lets a caller add boot flags (e.g. --lb 0) without a second copy
   # of this function; word splitting is intentional.
   # shellcheck disable=SC2086
   taskset -c "$SRVCORE" "$bin" --port "$PORT" --bind 127.0.0.1 --shards 16 \
diff --git a/tests/flipctl_unit.cc b/tests/flipctl_unit.cc
index 540a6c40e9589572431075b623bc553c76ee8d85..33209258e2958771d60f86b919f6f510e9977737
--- a/tests/flipctl_unit.cc
+++ b/tests/flipctl_unit.cc
@@ -184,11 +184,11 @@
 
     // THE TYPED BAND IS A FLOOR, NOT A CEILING. The gate-hygiene lane's instrumented failure: a
     // driver holding its rate to 0.07% across 34 samples, a fingerprint distance of 0.2518 against
-    // a flat --flip-auto-band 2 (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
+    // a flat explicit band of 2% (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
     // A detector whose signature genuinely moves between adjacent quiet windows must end up with a
     // band above that movement whatever the operator typed, and must still fire on a real change.
     {
-        FlipShiftDetector typed(2, 4);   // --flip-auto-band 2
+        FlipShiftDetector typed(2, 4);   // internal explicit-band control: 2%
         // Stationary but NOISY: the read/write split alternates 35:65 / 65:35 window to window.
         // The detector smooths the signature, so the anchored distance this produces is ~0.09.
         const auto noisy = [](int k) { return quiet(k % 2 ? 65 : 35, k % 2 ? 35 : 65); };
@@ -250,8 +250,8 @@
         if (fires) fail("the learned band fired on quiet-state movement");
     }
     // A TYPED BAND UNDER THE REAL SAMPLER. The shipped configuration cannot reach this -- the
-    // controller runs the learned band -- but an operator who sets --flip-auto-band AND the sampler
-    // can, and on a tree where the typed path bypasses the floor the fingerprint lane measured 73
+    // controller runs the learned band. This internal explicit-band control preserves the earlier
+    // regression: where the typed path bypassed the floor the fingerprint lane measured 73
     // two-consecutive exceedances in 600 stationary windows at K=100 and 208 at K=60: a spurious
     // maneuver every few seconds on a load that never changed. Driven here through THEIR writer, a
     // stationary 50/50 stream at depth 32, with the floor in place: none, and a real mix change
@@ -259,7 +259,7 @@
     {
         for (uint32_t window : {60u, 100u}) {
             XorShift rng;
-            FlipShiftDetector typed(2, 8);   // --flip-auto-band 2, the operator's knob
+            FlipShiftDetector typed(2, 8);   // internal explicit-band control: 2%
             int fires = 0, confirmed = 0, streak = 0;
             for (int w = 0; w < 60; w++) {
                 FlipFingerprintWriter writer;
diff --git a/tests/rlcache_churn.py b/tests/rlcache_churn.py
index b56ae4041ae0843dc67443f16017270063853124..f8a2c0730bb0a4fb6c3b5f6282c547226bb76599
--- a/tests/rlcache_churn.py
+++ b/tests/rlcache_churn.py
@@ -59,7 +59,7 @@
 
 
 # THE LOAD BALANCER MUST HAVE SOMETHING TO BALANCE. A uniform key distribution over one small key
-# space keeps every shard at the same op rate, the imbalance never crosses lb-imbalance-pct, and no
+# space keeps every shard at the same op rate, the imbalance never crosses the learned band, and no
 # shard ever changes owner -- which is the precondition for the defect this battery exists for
 # (measured: 7.8M ops, 0 migrations). A hot window that ROTATES makes a few shards hot, then makes
 # different ones hot, so the balancer moves shards continuously and in both directions, which is

diff --git a/AUDIT-TESTS.md b/AUDIT-TESTS.md
index e9cd43ffbc376172717531d0037de1c79fd23690..fe1fd0e2c39e965f605e4a30894a545129541534
--- a/AUDIT-TESTS.md
+++ b/AUDIT-TESTS.md
@@ -122,7 +122,7 @@
 | MM floor (memtier 20 s, sampled 8+6) | 1 | 14 | measurement, keep |
 | saturated-flip memtier | 1 | 25+3 | measurement, keep |
 | `flipctl.py --stable-seconds 30` (+ramp/idle/surge, timeout 300) | 1 | 60-120 est. | measurement, keep |
-| `xshard_dispatch_scale.sh` (2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 | 90-150 est. | measurement, keep |
+| Retired `xshard_dispatch_scale.sh` (historical 2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 historically, 0 now | 90-150 est. historically | manual ownership instrument removed; see `MERGE.md` |
 | `atomic_torn.py` hammers (3+2.5+2+2.5 s) + OFF re-rolls (up to 14 s) + RENAMENX/COPY parks (2 x 64 x 100 ms = 12.8 s) + window/lease/churn (~6 s) | 1 quick, +1 ASAN | ~45-60 each | re-rolls are dead time on this kernel (F6) |
 | `spinprobe.py` (6 s baseline + 6 s probe) | 3 | 37.5 | measurement, keep |
 | Python batteries' literal sleeps (census: flipctl 17 s, limits 5.8, edgetime 4.5, atomic_ryow 4, hexpire 3.4 — most run x2) | | ~60 | keep |
diff --git a/DESIGN-GATEHYGIENE.md b/DESIGN-GATEHYGIENE.md
index 5ea9df93e4548fb73a7e78b38debd52d876ad658..e6ac34a10326fe8880eaa664690832da3875b9b8
--- a/DESIGN-GATEHYGIENE.md
+++ b/DESIGN-GATEHYGIENE.md
@@ -459,7 +459,21 @@
 
 ---
 
-## 10. Follow-up diff on `78c3e5391` — 2026-09-07, runtime validation pending
+## 10. Incoming timing lane on `78c3e5391` — 2026-09-07
+
+**Merged disposition:** the maintainer reports the incoming timing lane gated 344/344 with no
+SKIP rows. The account below records that lane's original implementation and offline validation;
+it is not a validation claim for the merged tree. Its dispatch wrapper/helper and timing evaluator
+are retired: repeatability checks did not replace the deleted manual ownership instrument. The
+configurable-window shrink samples and shrink/reclaim budgets are superseded by the derived-window
+held-burst liveness and old-generation reconfiguration witnesses in `tests/atomicwindow.py`.
+All those mechanism, exact-reply, bound, and credit-reclamation checks remain mandatory on every
+build. The original OFF RENAME four-roll hard-failure control is unchanged, and the 1.5-second
+promotion budget still requires `--release-build`. The merged ledger requires quick 325/full 342;
+the constants remain untouched. [MERGE.md](MERGE.md) records the complete conflict and row accounting.
+
+The rest of this section is historical lane evidence, including its now-superseded dispatch,
+shrink, admission-calibration, and ledger statements.
 
 This is a test-only diff in `cx-timing`. No server, benchmark, gate, or compiler was run. The
 earlier acceptance results above describe earlier changes, **not this diff**. The worked fixes
diff --git a/DESIGN-flipfp.md b/DESIGN-flipfp.md
index 32eb6f46c24fb688db6accc79537fbd664bd084c..f705b25837c72a011da71df06dec09d2757f05ae
--- a/DESIGN-flipfp.md
+++ b/DESIGN-flipfp.md
@@ -311,7 +311,9 @@
 detector. The signal and the floor cannot be read from one run -- with a learned band the trigger
 fires and `reset()` wipes the distance -- so each arm is run twice: `--flip-auto-band 90` holds the
 anchor and lets the distance converge (the signal), `--flip-auto-band -1` learns the floor.
-`scratch/flipfp/fpmargin.sh`.
+The former `scratch/flipfp/fpmargin.sh` implemented this two-band experiment. It is retired in the
+merged tree because neither pinning the band nor disabling its retriggers has a public control.
+These historical measurements remain evidence only for their recorded builds; see `MERGE.md`.
 
 **BITCOUNT -> INCR** (the redesign lane's reference change), 2:2 split, `--rate-limiting` pinned:
 
diff --git a/DESIGN.md b/DESIGN.md
index 0fba740d94ba1915868410de40bcb685ae675833..262f7b730cc2cedfdcffb964afb729c758dc97fc
--- a/DESIGN.md
+++ b/DESIGN.md
@@ -1,6 +1,6 @@
 # Thread amortization study surface
 
-`thread-mode` and `overlap` are one boot-latched measurement surface. The documented mode
+`thread-mode` and the study-only `x-overlap` are one boot-latched measurement surface. The mode
 names are `2s` (separate IO and executor threads, the default) and `1s` (one generalized thread owns
 both roles). `split` and `fused` remain parser aliases so old invocations keep working.
 
@@ -9,11 +9,11 @@
 | `2s` | ordinary split IO/ex loops | exact `t-iopipe` WB/IFID batch schedule | rejected at config validation |
 | `1s` | coarse IFID → EX → WB rotation | exact `t-genthread` `iofused` schedule | gated `iofused` three-way schedule |
 
-The compatibility options `thread-pipeline` and `genthread-schedule` remain accepted: the former
-is a numeric alias for `overlap`, while the latter maps `coarse`, `iofused`, and the legacy
-`streams` name to `1s` plus overlap 0, 1, and 2 respectively. The legacy name is only a parser
-alias; overlap 2 no longer runs the streams implementation. Overlap 2 emits a boot warning because
-it is an experimental research schedule. Unified overlap 1 and 2 require `net-io uring`; the split
+Use `--x-overlap 0|1|2` for these study schedules and `--x-ex-sched 0|1` for the executor study.
+The old public schedule spellings and compatibility aliases are removed; see
+[DESIGN-KNOBS.md](DESIGN-KNOBS.md). Below, "overlap" names the internal schedule value.
+Overlap 2 emits a boot warning because it is an experimental research schedule.
+Unified overlap 1 and 2 require `net-io uring`; the split
 iopipe schedule retains its explicit epoll specialization. No schedule constant is runtime-tunable.
 
 Overlap 1 is deliberately the measured implementation, not a family resemblance. In `2s`, its
diff --git a/MERGE.md b/MERGE.md
index e29579036e98115056aa3e3a5401db083d2b7820..d70892fe599ab8e1e4e0c67b2a54cc390f585ea5
--- a/MERGE.md
+++ b/MERGE.md
@@ -59,7 +59,7 @@
 
 ### OFF RENAME and the atomic timing split
 
-The complete base block from “Broadened movers: RENAME” through the following store-family
+The complete base block from “Broadened movers: RENAME” up to the following store-family
 section is byte-for-byte preserved. It still runs `for _roll in range(4)` at
 `tests/atomic_torn.py:771`, arms and clears the hop delay in `try/finally`, retries only a
 clean miss, and stops on a witnessed tear, worker error, or surviving helper. The OFF assertion
@@ -323,6 +323,9 @@
 - Input-path/hash accounting, full-file comparisons for retained lane files, static gate-row
   comparison and loop arithmetic, unchanged EXPECT assignments, layout-assertion inspection,
   removed-flag/caller search, conflict-marker search, and `git diff --check`.
+- Reconstructed each merged conflict file from the captured inputs: the atomic file equals
+  timing plus exactly the documented knobs admission replacement; the gate equals knobs plus
+  exactly the surviving timing call-site changes and the documented comment resolutions.
 
 No C++ syntax check, build, server, benchmark, full test, or gate result is claimed. The
 maintainer supplies the merged runtime and performance verdict.
diff --git a/NOTES-HYGIENE.md b/NOTES-HYGIENE.md
index 23506bce64f00cb0dccaaff93362ccd30deab229..f224db07f4ceb2e4791c7cbb471b87d83a0a3e93
--- a/NOTES-HYGIENE.md
+++ b/NOTES-HYGIENE.md
@@ -352,7 +352,8 @@
 ## Test-file census
 
 The calibration examples expireindex.py, borrow_registry.py, and xshard_dispatch_scale.sh were
-already wired in the current parent. aof_frames.py is indirectly executed by aof_frame_order.py;
+already wired in this note's historical parent. The merged tree retires the dispatch instrument;
+the two surviving batteries use `--release-build`. aof_frames.py is indirectly executed by aof_frame_order.py;
 aof_rewrite.py and aof_rewrite_triggers.py are driven by matrix wrappers; differ.py is driven by
 differ_gate.sh; niclib.sh and gate_refs.txt support optional full-tier NIC rows.
 
@@ -456,7 +457,7 @@
 notify: ok (notify_events_fired=1608)
 ~~~
 
-Purpose-boot matrix commands:
+Historical purpose-boot matrix commands (superseded by the current gate's network-engine grammar):
 
 ~~~sh
 PERSIST_IO=normal GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_matrix.sh
@@ -465,7 +466,8 @@
 PERSIST_IO=uring  GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_trigger_matrix.sh
 ~~~
 
-Other purpose-booted battery invocations (each boot used the knobs documented by gate.sh):
+Historical purpose-booted battery invocations (not recipes for the merged tree; the dispatch
+instrument and script OFF arm below are retired):
 
 ~~~sh
 python3 tests/xscript.py 127.0.0.1 7600 off|limit|window
diff --git a/NOTES-XPERF2.md b/NOTES-XPERF2.md
index d4ce7950b1396997d44f0945f6f5179aa625e59e..d1edc822a5e7abc68e6aefc3d5a55986b0bf178e
--- a/NOTES-XPERF2.md
+++ b/NOTES-XPERF2.md
@@ -8,7 +8,7 @@
 |---|---|---|---|---|
 | E4 | ExpireIndex never shrinks; memsets the whole sidecar on live→0; single-pass rehash | **REPRODUCED** (both halves) | yes | `tests/expireindex.py` |
 | E5 | zero-copy borrow registry is a linear vector scanned per borrow/release/retire | **REPRODUCED** | yes | `tests/borrow_registry.py` |
-| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes | `tests/xshard_dispatch_scale.sh` |
+| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes, historically | retired manual-map dispatch instrument; see `MERGE.md` |
 | E7 | segment mode is absorbing: once triggered every later reply is a malloc + iovec slot | **NOT REPRODUCED** | no | none |
 
 Rig: EPYC 9754, cores 48–63 only, ports 7200–7209 only, loopback. Oracle = vanilla redis 7.4 at
@@ -292,18 +292,20 @@
 |---|---|---|---|---|
 | `tests/expireindex.py <host> <port>` | `--shards 1 --ratio 1:1 --enable-debug-command yes` | growth-trigger cost ratio over a 16× population ≤ 8.0; live→0 DEL ≤ 1.25× a neighbouring DEL | FAIL 2 (16.30×, 2.00×) | PASS (1.65×, 1.00×) |
 | `tests/borrow_registry.py <host> <port>` | `--shards 1 --zc-min 64 --client-output-buffer-limit normal 0 0 0 --enable-debug-command yes` | borrowed-GET per-op cost growth over 0 → ~2000 live borrows ≤ 1.05 | FAIL (1.078, 1.079) | PASS (1.008, 1.008, 1.009) |
-| `tests/xshard_dispatch_scale.sh` (+ `.py`) | boots its own 4-thread and 128-thread arms | dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
+| Retired `tests/xshard_dispatch_scale.sh` (+ `.py`) | historically booted fixed-owner 4-thread and 128-thread arms | historical dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
 
 Each also carries its own non-vacuity checks: `expireindex.py` asserts INFO `expires` really
 reached the population under test and that INFO `expired_keys` MOVED (so the sampler is proven to
 still visit deadlines parked mid-migration), plus a no-TTL negative control on both timing legs;
 `borrow_registry.py` asserts `CLIENT LIST oll` grew (the borrow path is proven live, not assumed),
 carries a non-borrowed control arm, and refuses to run unless the zc gate straddles its two probe
-sizes; `xshard_dispatch_scale.py` refuses to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` prove
+sizes; the retired `xshard_dispatch_scale.py` refused to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` proved
 the cross pair really fanned out to two owner threads and the control stayed on one.
 
-None of the three is wired into `tests/gate.sh`: they each need their own boot geometry and the
-gate's port/cores belong to the mainline operator. Run them as above.
+These were standalone instruments when measured. The merged gate runs the expiry and borrow
+batteries with `--release-build`. The dispatch wrapper and helper are retired because their manual
+ownership geometry is unavailable; the historical measurements above do not validate default
+round-robin ownership. See [MERGE.md](MERGE.md).
 
 ## Evidence
 
diff --git a/docs/flipctl-design.md b/docs/flipctl-design.md
index e6a5e31a6c05171b172746f2586a01d09fa1e84e..9280bc6ad5f9b98d9535c141a4e45f6460962398
--- a/docs/flipctl-design.md
+++ b/docs/flipctl-design.md
@@ -8,8 +8,7 @@
 ## Ownership and footprint
 
 - `src/main.cc` is the controller cron owner. When `flip-auto=1`, the main/monitor thread calls
-  `Server::flipctl_tick()` once per existing LB tick (`lb-tick-ms`; if that mechanism is disabled,
-  the period derives from the provisioned thread count). No IO or EX loop runs controller policy.
+  `Server::flipctl_tick()` on its internal observation cadence. No IO or EX loop runs controller policy.
 - `src/core/flipctl.{h,cc}` contains the fingerprint representation, shift detector, and controller
   state machine. Controller vectors allocate only when `flip-auto=1`.
 - Each `ThreadCtx` embeds one `FlipFingerprintWriter`. It has no dynamic storage. Only the physical
@@ -46,10 +45,10 @@
 measurement quantum, avoiding the zero-anchor discontinuity where one rare admin argument would
 otherwise have distance one.
 
-With `flip-auto-band=-1`, the band is twice the maximum quiet adjacent/drift distance learned at
-the final split, floored by the four-family sampling quantum divided by observed commands. A
-numeric value is a percent. Zero disables fingerprint and collapse re-triggers. Final anchor
-learning consumes a number of closed aggregates derived from the provisioned thread count. It is
+The automatic band is twice the maximum quiet adjacent/drift distance learned at
+the final split, floored by the four-family sampling quantum divided by observed commands.
+It has no public override. Final anchor learning consumes a number of closed aggregates derived
+from the provisioned thread count. It is
 done after maneuver-only age sampling is disarmed, so the controller cannot detect its own signal
 cost transition as a workload change.
 
@@ -64,14 +63,15 @@
 falls below a threshold derived from its own change jitter and measurement quantum. Directional
 connection ramp-up therefore remains drift, while jittery stationary traffic can qualify even when
 its absolute rate never enters a quiet band. A 30-second ceiling, converted to ticks from
-`lb-tick-ms`, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
-thread per tick resets the learning/cap state, so an idle server remains in
+the internal observation cadence, caps deferral under non-idle traffic. Traffic at or below one
+command per provisioned thread per tick resets the learning/cap state, so an idle server remains in
 `awaiting-load-stability` and never pays for a maneuver it cannot use.
 
-`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. An explicit
-`lb-age-sample-rate` is used when nonzero; otherwise the rate derives from the provisioned thread
-count. IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
-writes. IO releases its ROB-head age map when the rate returns to zero.
+`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. Its rate
+derives from observed command volume and the three-tick decision window, and adapts on completed
+traffic windows; see [DESIGN-KNOBS.md](../DESIGN-KNOBS.md). IO and EX loops apply the published
+rate to their own `LoopSignals`, preserving single-owner writes. IO releases its ROB-head age
+map when the rate returns to zero.
 
 After a stabilized baseline rate, `issue_initial_jump()` subtracts the per-thread maneuver window.
 For each role it computes:
@@ -88,9 +88,10 @@
 not vetoed by idle peers. If either role has no measured work/busy quantity, measurement continues;
 the controller does not invent a workload prior.
 
-The calculation is performed in placement units (one logical CPU normally, one sibling pair under
-`smt-mode=1`) and then reported/committed in threads. The target passes `nio_eq` in the measured
-direction by `max(1, abs(nio_eq-nio_now)/2)`, clamped to one live unit of each role. Even when the
+The calculation is performed in placement units (one logical CPU normally, one sibling pair when
+the allowed topology contains complete pairs) and then reported/committed in threads. The target
+passes `nio_eq` in the measured direction by `max(1, abs(nio_eq-nio_now)/2)`, clamped to one live
+unit of each role. Even when the
 equalized split rounds to the current split, the direction of greater measured demand supplies the
 deliberate one-unit overshoot.
 
@@ -143,7 +144,7 @@
 
 ## Integration and observability
 
-- `src/core/config.h` is the sole parser/default source for `flip-auto`, `flip-auto-band`, and
+- `src/core/config.h` is the sole parser/default source for `flip-auto` and
   `flip-work-window`; `src/cmd/t_server.cc` exposes them as immutable CONFIG values.
 - `tomokv.conf` documents the numeric grammar and off/auto semantics.
 - `INFO FLIPCTL` reports state/phase, anchor split/rate, bands, total/per-reason trigger counts, and
diff --git a/docs/specs/AUDIT-TLS.md b/docs/specs/AUDIT-TLS.md
index 0256e887f5d27c210d436bc219f3c7521ddad0db..87a2eaca3137a47f7c0e1567794650f57531860b
--- a/docs/specs/AUDIT-TLS.md
+++ b/docs/specs/AUDIT-TLS.md
@@ -3,7 +3,7 @@
 > **Where it landed (2026-09-04).** The feature shipped as `src/net/tls.{h,cc}` (this document
 > proposes `src/net/tls_ctx.h` + `src/net/tls_conn.h`); the test is `tests/tls.py` (no
 > `tests/gen-certs.sh`); no `tls-bio-size` knob exists. Defaults: AES-128-GCM-first cipher lists
-> and `tls-ktls yes` (`tomokv.conf`, NETWORK).
+> and automatic kTLS negotiation with userspace fallback (`tomokv.conf`, NETWORK).
 
 Governing audit. Read before writing any TLS code in `/home/user/Projects/tomokv-cpp-perthread`.
 No repository was modified in producing this.
diff --git a/scratchpad/rlbatch/lib.sh b/scratchpad/rlbatch/lib.sh
index 7bcf3e4e6030c79174bf35d9bb1b3dfb11f80c42..12184314d2aafac0dc7aa48db761a3bdec5de8c7
--- a/scratchpad/rlbatch/lib.sh
+++ b/scratchpad/rlbatch/lib.sh
@@ -17,7 +17,7 @@
 boot_srv(){ # boot_srv <binary> <logfile> [extra args...] -> SRV pid
   local bin="$1" log="$2"; shift 2
   guard_port "$PORT" || return 1
-  # EXTRA_ARGS lets a caller add boot flags (e.g. --read-local-interleave 0) without a second copy
+  # EXTRA_ARGS lets a caller add boot flags (e.g. --lb 0) without a second copy
   # of this function; word splitting is intentional.
   # shellcheck disable=SC2086
   taskset -c "$SRVCORE" "$bin" --port "$PORT" --bind 127.0.0.1 --shards 16 \
diff --git a/tests/flipctl_unit.cc b/tests/flipctl_unit.cc
index 540a6c40e9589572431075b623bc553c76ee8d85..33209258e2958771d60f86b919f6f510e9977737
--- a/tests/flipctl_unit.cc
+++ b/tests/flipctl_unit.cc
@@ -184,11 +184,11 @@
 
     // THE TYPED BAND IS A FLOOR, NOT A CEILING. The gate-hygiene lane's instrumented failure: a
     // driver holding its rate to 0.07% across 34 samples, a fingerprint distance of 0.2518 against
-    // a flat --flip-auto-band 2 (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
+    // a flat explicit band of 2% (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
     // A detector whose signature genuinely moves between adjacent quiet windows must end up with a
     // band above that movement whatever the operator typed, and must still fire on a real change.
     {
-        FlipShiftDetector typed(2, 4);   // --flip-auto-band 2
+        FlipShiftDetector typed(2, 4);   // internal explicit-band control: 2%
         // Stationary but NOISY: the read/write split alternates 35:65 / 65:35 window to window.
         // The detector smooths the signature, so the anchored distance this produces is ~0.09.
         const auto noisy = [](int k) { return quiet(k % 2 ? 65 : 35, k % 2 ? 35 : 65); };
@@ -250,8 +250,8 @@
         if (fires) fail("the learned band fired on quiet-state movement");
     }
     // A TYPED BAND UNDER THE REAL SAMPLER. The shipped configuration cannot reach this -- the
-    // controller runs the learned band -- but an operator who sets --flip-auto-band AND the sampler
-    // can, and on a tree where the typed path bypasses the floor the fingerprint lane measured 73
+    // controller runs the learned band. This internal explicit-band control preserves the earlier
+    // regression: where the typed path bypassed the floor the fingerprint lane measured 73
     // two-consecutive exceedances in 600 stationary windows at K=100 and 208 at K=60: a spurious
     // maneuver every few seconds on a load that never changed. Driven here through THEIR writer, a
     // stationary 50/50 stream at depth 32, with the floor in place: none, and a real mix change
@@ -259,7 +259,7 @@
     {
         for (uint32_t window : {60u, 100u}) {
             XorShift rng;
-            FlipShiftDetector typed(2, 8);   // --flip-auto-band 2, the operator's knob
+            FlipShiftDetector typed(2, 8);   // internal explicit-band control: 2%
             int fires = 0, confirmed = 0, streak = 0;
             for (int w = 0; w < 60; w++) {
                 FlipFingerprintWriter writer;
diff --git a/tests/rlcache_churn.py b/tests/rlcache_churn.py
index b56ae4041ae0843dc67443f16017270063853124..f8a2c0730bb0a4fb6c3b5f6282c547226bb76599
--- a/tests/rlcache_churn.py
+++ b/tests/rlcache_churn.py
@@ -59,7 +59,7 @@
 
 
 # THE LOAD BALANCER MUST HAVE SOMETHING TO BALANCE. A uniform key distribution over one small key
-# space keeps every shard at the same op rate, the imbalance never crosses lb-imbalance-pct, and no
+# space keeps every shard at the same op rate, the imbalance never crosses the learned band, and no
 # shard ever changes owner -- which is the precondition for the defect this battery exists for
 # (measured: 7.8M ops, 0 migrations). A hot window that ROTATES makes a few shards hot, then makes
 # different ones hot, so the balancer moves shards continuously and in both directions, which is

codex
`MERGE.md` now accounts for all 49 input paths and every superseded hunk. It also records the stale-script and documentation cleanup. The merged gate is exactly the knobs input plus the surviving timing wiring and documented comment changes; the atomic test is exactly the timing input plus the derived-window replacement. I’m removing the temporary merge snapshots and checking the final diff.
exec
/bin/bash -lc "git add --intent-to-add -- DESIGN-KNOBS.md MERGE.md tests/atomicwindow.py tests/knobs.py
python3 - <<'PY'
from pathlib import Path
import json, hashlib, subprocess, re, shutil
root = Path.cwd()
audit = root / '.merge-inputs'
manifest = json.loads((audit / 'manifest.json').read_text())
report = Path('MERGE.md').read_text()
expected_differences = {'timing': {'DESIGN-GATEHYGIENE.md', 'tests/atomic_torn.py', 'tests/gate.sh', 'tests/xshard_dispatch_scale.py', 'tests/xshard_dispatch_scale.sh'}, 'knobs': {'DESIGN-KNOBS.md', 'tests/atomic_torn.py', 'tests/gate.sh', 'tests/rlcache_churn.py'}}
for lane, files in manifest['lanes'].items():
    different = set()
    for name, digest in files.items():
        assert '| "'`'"' + name + '"'` |'"' in report, name
        path = Path(name)
        actual = hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else None
        if actual "'!= digest: different.add(name)
        source = root.parent / ('"'cx-' + lane) / name
        source_digest = hashlib.sha256(source.read_bytes()).hexdigest() if source.exists() else None
        assert source_digest == digest, ('source changed', lane, name)
    assert different == expected_differences[lane], (lane, different)
    print(lane + ':', len(files) - len(different), 'exact dispositions;', len(different), 'documented exceptions; source unchanged.')
paths = subprocess.check_output(['git', 'diff', '--name-only'], text=True).splitlines()
for name in paths:
    path = Path(name)
    if path.exists():
        assert not re.search(r'"'^(<<<<<<< |>>>>>>> |'"\\|\\|\\|\\|\\|\\|\\| )', path.read_text(), re.M), name
assert subprocess.check_output(['git', 'diff', '--cached', '--name-only'], text=True) == ''
subprocess.run(['git', 'diff', '--check'], check=True)
print('Diff whitespace/conflict checks passed;', len(paths), 'reviewable changed paths, including the four new files; no content staged.')
shutil.rmtree(audit)
print('Removed only the temporary .merge-inputs directory created for this merge.')
PY
git diff --shortstat
git ls-files --others --exclude-standard
git diff --numstat -- DESIGN-KNOBS.md MERGE.md tests/atomicwindow.py tests/knobs.py
" in /home/user/Projects/cx-merge
 succeeded in 2ms:
CODEX-OUT.md
328	0	DESIGN-KNOBS.md
331	0	MERGE.md
225	0	tests/atomicwindow.py
48	0	tests/knobs.py

diff --git a/AUDIT-TESTS.md b/AUDIT-TESTS.md
index e9cd43ffbc376172717531d0037de1c79fd23690..fe1fd0e2c39e965f605e4a30894a545129541534
--- a/AUDIT-TESTS.md
+++ b/AUDIT-TESTS.md
@@ -122,7 +122,7 @@
 | MM floor (memtier 20 s, sampled 8+6) | 1 | 14 | measurement, keep |
 | saturated-flip memtier | 1 | 25+3 | measurement, keep |
 | `flipctl.py --stable-seconds 30` (+ramp/idle/surge, timeout 300) | 1 | 60-120 est. | measurement, keep |
-| `xshard_dispatch_scale.sh` (2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 | 90-150 est. | measurement, keep |
+| Retired `xshard_dispatch_scale.sh` (historical 2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 historically, 0 now | 90-150 est. historically | manual ownership instrument removed; see `MERGE.md` |
 | `atomic_torn.py` hammers (3+2.5+2+2.5 s) + OFF re-rolls (up to 14 s) + RENAMENX/COPY parks (2 x 64 x 100 ms = 12.8 s) + window/lease/churn (~6 s) | 1 quick, +1 ASAN | ~45-60 each | re-rolls are dead time on this kernel (F6) |
 | `spinprobe.py` (6 s baseline + 6 s probe) | 3 | 37.5 | measurement, keep |
 | Python batteries' literal sleeps (census: flipctl 17 s, limits 5.8, edgetime 4.5, atomic_ryow 4, hexpire 3.4 — most run x2) | | ~60 | keep |
diff --git a/DESIGN-GATEHYGIENE.md b/DESIGN-GATEHYGIENE.md
index 5ea9df93e4548fb73a7e78b38debd52d876ad658..e6ac34a10326fe8880eaa664690832da3875b9b8
--- a/DESIGN-GATEHYGIENE.md
+++ b/DESIGN-GATEHYGIENE.md
@@ -459,7 +459,21 @@
 
 ---
 
-## 10. Follow-up diff on `78c3e5391` — 2026-09-07, runtime validation pending
+## 10. Incoming timing lane on `78c3e5391` — 2026-09-07
+
+**Merged disposition:** the maintainer reports the incoming timing lane gated 344/344 with no
+SKIP rows. The account below records that lane's original implementation and offline validation;
+it is not a validation claim for the merged tree. Its dispatch wrapper/helper and timing evaluator
+are retired: repeatability checks did not replace the deleted manual ownership instrument. The
+configurable-window shrink samples and shrink/reclaim budgets are superseded by the derived-window
+held-burst liveness and old-generation reconfiguration witnesses in `tests/atomicwindow.py`.
+All those mechanism, exact-reply, bound, and credit-reclamation checks remain mandatory on every
+build. The original OFF RENAME four-roll hard-failure control is unchanged, and the 1.5-second
+promotion budget still requires `--release-build`. The merged ledger requires quick 325/full 342;
+the constants remain untouched. [MERGE.md](MERGE.md) records the complete conflict and row accounting.
+
+The rest of this section is historical lane evidence, including its now-superseded dispatch,
+shrink, admission-calibration, and ledger statements.
 
 This is a test-only diff in `cx-timing`. No server, benchmark, gate, or compiler was run. The
 earlier acceptance results above describe earlier changes, **not this diff**. The worked fixes
diff --git a/DESIGN-flipfp.md b/DESIGN-flipfp.md
index 32eb6f46c24fb688db6accc79537fbd664bd084c..f705b25837c72a011da71df06dec09d2757f05ae
--- a/DESIGN-flipfp.md
+++ b/DESIGN-flipfp.md
@@ -311,7 +311,9 @@
 detector. The signal and the floor cannot be read from one run -- with a learned band the trigger
 fires and `reset()` wipes the distance -- so each arm is run twice: `--flip-auto-band 90` holds the
 anchor and lets the distance converge (the signal), `--flip-auto-band -1` learns the floor.
-`scratch/flipfp/fpmargin.sh`.
+The former `scratch/flipfp/fpmargin.sh` implemented this two-band experiment. It is retired in the
+merged tree because neither pinning the band nor disabling its retriggers has a public control.
+These historical measurements remain evidence only for their recorded builds; see `MERGE.md`.
 
 **BITCOUNT -> INCR** (the redesign lane's reference change), 2:2 split, `--rate-limiting` pinned:
 
diff --git a/DESIGN.md b/DESIGN.md
index 0fba740d94ba1915868410de40bcb685ae675833..262f7b730cc2cedfdcffb964afb729c758dc97fc
--- a/DESIGN.md
+++ b/DESIGN.md
@@ -1,6 +1,6 @@
 # Thread amortization study surface
 
-`thread-mode` and `overlap` are one boot-latched measurement surface. The documented mode
+`thread-mode` and the study-only `x-overlap` are one boot-latched measurement surface. The mode
 names are `2s` (separate IO and executor threads, the default) and `1s` (one generalized thread owns
 both roles). `split` and `fused` remain parser aliases so old invocations keep working.
 
@@ -9,11 +9,11 @@
 | `2s` | ordinary split IO/ex loops | exact `t-iopipe` WB/IFID batch schedule | rejected at config validation |
 | `1s` | coarse IFID → EX → WB rotation | exact `t-genthread` `iofused` schedule | gated `iofused` three-way schedule |
 
-The compatibility options `thread-pipeline` and `genthread-schedule` remain accepted: the former
-is a numeric alias for `overlap`, while the latter maps `coarse`, `iofused`, and the legacy
-`streams` name to `1s` plus overlap 0, 1, and 2 respectively. The legacy name is only a parser
-alias; overlap 2 no longer runs the streams implementation. Overlap 2 emits a boot warning because
-it is an experimental research schedule. Unified overlap 1 and 2 require `net-io uring`; the split
+Use `--x-overlap 0|1|2` for these study schedules and `--x-ex-sched 0|1` for the executor study.
+The old public schedule spellings and compatibility aliases are removed; see
+[DESIGN-KNOBS.md](DESIGN-KNOBS.md). Below, "overlap" names the internal schedule value.
+Overlap 2 emits a boot warning because it is an experimental research schedule.
+Unified overlap 1 and 2 require `net-io uring`; the split
 iopipe schedule retains its explicit epoll specialization. No schedule constant is runtime-tunable.
 
 Overlap 1 is deliberately the measured implementation, not a family resemblance. In `2s`, its
diff --git a/MERGE.md b/MERGE.md
index e29579036e98115056aa3e3a5401db083d2b7820..d70892fe599ab8e1e4e0c67b2a54cc390f585ea5
--- a/MERGE.md
+++ b/MERGE.md
@@ -59,7 +59,7 @@
 
 ### OFF RENAME and the atomic timing split
 
-The complete base block from “Broadened movers: RENAME” through the following store-family
+The complete base block from “Broadened movers: RENAME” up to the following store-family
 section is byte-for-byte preserved. It still runs `for _roll in range(4)` at
 `tests/atomic_torn.py:771`, arms and clears the hop delay in `try/finally`, retries only a
 clean miss, and stops on a witnessed tear, worker error, or surviving helper. The OFF assertion
@@ -323,6 +323,9 @@
 - Input-path/hash accounting, full-file comparisons for retained lane files, static gate-row
   comparison and loop arithmetic, unchanged EXPECT assignments, layout-assertion inspection,
   removed-flag/caller search, conflict-marker search, and `git diff --check`.
+- Reconstructed each merged conflict file from the captured inputs: the atomic file equals
+  timing plus exactly the documented knobs admission replacement; the gate equals knobs plus
+  exactly the surviving timing call-site changes and the documented comment resolutions.
 
 No C++ syntax check, build, server, benchmark, full test, or gate result is claimed. The
 maintainer supplies the merged runtime and performance verdict.
diff --git a/NOTES-HYGIENE.md b/NOTES-HYGIENE.md
index 23506bce64f00cb0dccaaff93362ccd30deab229..f224db07f4ceb2e4791c7cbb471b87d83a0a3e93
--- a/NOTES-HYGIENE.md
+++ b/NOTES-HYGIENE.md
@@ -352,7 +352,8 @@
 ## Test-file census
 
 The calibration examples expireindex.py, borrow_registry.py, and xshard_dispatch_scale.sh were
-already wired in the current parent. aof_frames.py is indirectly executed by aof_frame_order.py;
+already wired in this note's historical parent. The merged tree retires the dispatch instrument;
+the two surviving batteries use `--release-build`. aof_frames.py is indirectly executed by aof_frame_order.py;
 aof_rewrite.py and aof_rewrite_triggers.py are driven by matrix wrappers; differ.py is driven by
 differ_gate.sh; niclib.sh and gate_refs.txt support optional full-tier NIC rows.
 
@@ -456,7 +457,7 @@
 notify: ok (notify_events_fired=1608)
 ~~~
 
-Purpose-boot matrix commands:
+Historical purpose-boot matrix commands (superseded by the current gate's network-engine grammar):
 
 ~~~sh
 PERSIST_IO=normal GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_matrix.sh
@@ -465,7 +466,8 @@
 PERSIST_IO=uring  GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_trigger_matrix.sh
 ~~~
 
-Other purpose-booted battery invocations (each boot used the knobs documented by gate.sh):
+Historical purpose-booted battery invocations (not recipes for the merged tree; the dispatch
+instrument and script OFF arm below are retired):
 
 ~~~sh
 python3 tests/xscript.py 127.0.0.1 7600 off|limit|window
diff --git a/NOTES-XPERF2.md b/NOTES-XPERF2.md
index d4ce7950b1396997d44f0945f6f5179aa625e59e..d1edc822a5e7abc68e6aefc3d5a55986b0bf178e
--- a/NOTES-XPERF2.md
+++ b/NOTES-XPERF2.md
@@ -8,7 +8,7 @@
 |---|---|---|---|---|
 | E4 | ExpireIndex never shrinks; memsets the whole sidecar on live→0; single-pass rehash | **REPRODUCED** (both halves) | yes | `tests/expireindex.py` |
 | E5 | zero-copy borrow registry is a linear vector scanned per borrow/release/retire | **REPRODUCED** | yes | `tests/borrow_registry.py` |
-| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes | `tests/xshard_dispatch_scale.sh` |
+| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes, historically | retired manual-map dispatch instrument; see `MERGE.md` |
 | E7 | segment mode is absorbing: once triggered every later reply is a malloc + iovec slot | **NOT REPRODUCED** | no | none |
 
 Rig: EPYC 9754, cores 48–63 only, ports 7200–7209 only, loopback. Oracle = vanilla redis 7.4 at
@@ -292,18 +292,20 @@
 |---|---|---|---|---|
 | `tests/expireindex.py <host> <port>` | `--shards 1 --ratio 1:1 --enable-debug-command yes` | growth-trigger cost ratio over a 16× population ≤ 8.0; live→0 DEL ≤ 1.25× a neighbouring DEL | FAIL 2 (16.30×, 2.00×) | PASS (1.65×, 1.00×) |
 | `tests/borrow_registry.py <host> <port>` | `--shards 1 --zc-min 64 --client-output-buffer-limit normal 0 0 0 --enable-debug-command yes` | borrowed-GET per-op cost growth over 0 → ~2000 live borrows ≤ 1.05 | FAIL (1.078, 1.079) | PASS (1.008, 1.008, 1.009) |
-| `tests/xshard_dispatch_scale.sh` (+ `.py`) | boots its own 4-thread and 128-thread arms | dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
+| Retired `tests/xshard_dispatch_scale.sh` (+ `.py`) | historically booted fixed-owner 4-thread and 128-thread arms | historical dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
 
 Each also carries its own non-vacuity checks: `expireindex.py` asserts INFO `expires` really
 reached the population under test and that INFO `expired_keys` MOVED (so the sampler is proven to
 still visit deadlines parked mid-migration), plus a no-TTL negative control on both timing legs;
 `borrow_registry.py` asserts `CLIENT LIST oll` grew (the borrow path is proven live, not assumed),
 carries a non-borrowed control arm, and refuses to run unless the zc gate straddles its two probe
-sizes; `xshard_dispatch_scale.py` refuses to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` prove
+sizes; the retired `xshard_dispatch_scale.py` refused to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` proved
 the cross pair really fanned out to two owner threads and the control stayed on one.
 
-None of the three is wired into `tests/gate.sh`: they each need their own boot geometry and the
-gate's port/cores belong to the mainline operator. Run them as above.
+These were standalone instruments when measured. The merged gate runs the expiry and borrow
+batteries with `--release-build`. The dispatch wrapper and helper are retired because their manual
+ownership geometry is unavailable; the historical measurements above do not validate default
+round-robin ownership. See [MERGE.md](MERGE.md).
 
 ## Evidence
 
diff --git a/docs/flipctl-design.md b/docs/flipctl-design.md
index e6a5e31a6c05171b172746f2586a01d09fa1e84e..9280bc6ad5f9b98d9535c141a4e45f6460962398
--- a/docs/flipctl-design.md
+++ b/docs/flipctl-design.md
@@ -8,8 +8,7 @@
 ## Ownership and footprint
 
 - `src/main.cc` is the controller cron owner. When `flip-auto=1`, the main/monitor thread calls
-  `Server::flipctl_tick()` once per existing LB tick (`lb-tick-ms`; if that mechanism is disabled,
-  the period derives from the provisioned thread count). No IO or EX loop runs controller policy.
+  `Server::flipctl_tick()` on its internal observation cadence. No IO or EX loop runs controller policy.
 - `src/core/flipctl.{h,cc}` contains the fingerprint representation, shift detector, and controller
   state machine. Controller vectors allocate only when `flip-auto=1`.
 - Each `ThreadCtx` embeds one `FlipFingerprintWriter`. It has no dynamic storage. Only the physical
@@ -46,10 +45,10 @@
 measurement quantum, avoiding the zero-anchor discontinuity where one rare admin argument would
 otherwise have distance one.
 
-With `flip-auto-band=-1`, the band is twice the maximum quiet adjacent/drift distance learned at
-the final split, floored by the four-family sampling quantum divided by observed commands. A
-numeric value is a percent. Zero disables fingerprint and collapse re-triggers. Final anchor
-learning consumes a number of closed aggregates derived from the provisioned thread count. It is
+The automatic band is twice the maximum quiet adjacent/drift distance learned at
+the final split, floored by the four-family sampling quantum divided by observed commands.
+It has no public override. Final anchor learning consumes a number of closed aggregates derived
+from the provisioned thread count. It is
 done after maneuver-only age sampling is disarmed, so the controller cannot detect its own signal
 cost transition as a workload change.
 
@@ -64,14 +63,15 @@
 falls below a threshold derived from its own change jitter and measurement quantum. Directional
 connection ramp-up therefore remains drift, while jittery stationary traffic can qualify even when
 its absolute rate never enters a quiet band. A 30-second ceiling, converted to ticks from
-`lb-tick-ms`, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
-thread per tick resets the learning/cap state, so an idle server remains in
+the internal observation cadence, caps deferral under non-idle traffic. Traffic at or below one
+command per provisioned thread per tick resets the learning/cap state, so an idle server remains in
 `awaiting-load-stability` and never pays for a maneuver it cannot use.
 
-`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. An explicit
-`lb-age-sample-rate` is used when nonzero; otherwise the rate derives from the provisioned thread
-count. IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
-writes. IO releases its ROB-head age map when the rate returns to zero.
+`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. Its rate
+derives from observed command volume and the three-tick decision window, and adapts on completed
+traffic windows; see [DESIGN-KNOBS.md](../DESIGN-KNOBS.md). IO and EX loops apply the published
+rate to their own `LoopSignals`, preserving single-owner writes. IO releases its ROB-head age
+map when the rate returns to zero.
 
 After a stabilized baseline rate, `issue_initial_jump()` subtracts the per-thread maneuver window.
 For each role it computes:
@@ -88,9 +88,10 @@
 not vetoed by idle peers. If either role has no measured work/busy quantity, measurement continues;
 the controller does not invent a workload prior.
 
-The calculation is performed in placement units (one logical CPU normally, one sibling pair under
-`smt-mode=1`) and then reported/committed in threads. The target passes `nio_eq` in the measured
-direction by `max(1, abs(nio_eq-nio_now)/2)`, clamped to one live unit of each role. Even when the
+The calculation is performed in placement units (one logical CPU normally, one sibling pair when
+the allowed topology contains complete pairs) and then reported/committed in threads. The target
+passes `nio_eq` in the measured direction by `max(1, abs(nio_eq-nio_now)/2)`, clamped to one live
+unit of each role. Even when the
 equalized split rounds to the current split, the direction of greater measured demand supplies the
 deliberate one-unit overshoot.
 
@@ -143,7 +144,7 @@
 
 ## Integration and observability
 
-- `src/core/config.h` is the sole parser/default source for `flip-auto`, `flip-auto-band`, and
+- `src/core/config.h` is the sole parser/default source for `flip-auto` and
   `flip-work-window`; `src/cmd/t_server.cc` exposes them as immutable CONFIG values.
 - `tomokv.conf` documents the numeric grammar and off/auto semantics.
 - `INFO FLIPCTL` reports state/phase, anchor split/rate, bands, total/per-reason trigger counts, and
diff --git a/docs/specs/AUDIT-TLS.md b/docs/specs/AUDIT-TLS.md
index 0256e887f5d27c210d436bc219f3c7521ddad0db..87a2eaca3137a47f7c0e1567794650f57531860b
--- a/docs/specs/AUDIT-TLS.md
+++ b/docs/specs/AUDIT-TLS.md
@@ -3,7 +3,7 @@
 > **Where it landed (2026-09-04).** The feature shipped as `src/net/tls.{h,cc}` (this document
 > proposes `src/net/tls_ctx.h` + `src/net/tls_conn.h`); the test is `tests/tls.py` (no
 > `tests/gen-certs.sh`); no `tls-bio-size` knob exists. Defaults: AES-128-GCM-first cipher lists
-> and `tls-ktls yes` (`tomokv.conf`, NETWORK).
+> and automatic kTLS negotiation with userspace fallback (`tomokv.conf`, NETWORK).
 
 Governing audit. Read before writing any TLS code in `/home/user/Projects/tomokv-cpp-perthread`.
 No repository was modified in producing this.
diff --git a/scratchpad/rlbatch/lib.sh b/scratchpad/rlbatch/lib.sh
index 7bcf3e4e6030c79174bf35d9bb1b3dfb11f80c42..12184314d2aafac0dc7aa48db761a3bdec5de8c7
--- a/scratchpad/rlbatch/lib.sh
+++ b/scratchpad/rlbatch/lib.sh
@@ -17,7 +17,7 @@
 boot_srv(){ # boot_srv <binary> <logfile> [extra args...] -> SRV pid
   local bin="$1" log="$2"; shift 2
   guard_port "$PORT" || return 1
-  # EXTRA_ARGS lets a caller add boot flags (e.g. --read-local-interleave 0) without a second copy
+  # EXTRA_ARGS lets a caller add boot flags (e.g. --lb 0) without a second copy
   # of this function; word splitting is intentional.
   # shellcheck disable=SC2086
   taskset -c "$SRVCORE" "$bin" --port "$PORT" --bind 127.0.0.1 --shards 16 \
diff --git a/tests/flipctl_unit.cc b/tests/flipctl_unit.cc
index 540a6c40e9589572431075b623bc553c76ee8d85..33209258e2958771d60f86b919f6f510e9977737
--- a/tests/flipctl_unit.cc
+++ b/tests/flipctl_unit.cc
@@ -184,11 +184,11 @@
 
     // THE TYPED BAND IS A FLOOR, NOT A CEILING. The gate-hygiene lane's instrumented failure: a
     // driver holding its rate to 0.07% across 34 samples, a fingerprint distance of 0.2518 against
-    // a flat --flip-auto-band 2 (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
+    // a flat explicit band of 2% (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
     // A detector whose signature genuinely moves between adjacent quiet windows must end up with a
     // band above that movement whatever the operator typed, and must still fire on a real change.
     {
-        FlipShiftDetector typed(2, 4);   // --flip-auto-band 2
+        FlipShiftDetector typed(2, 4);   // internal explicit-band control: 2%
         // Stationary but NOISY: the read/write split alternates 35:65 / 65:35 window to window.
         // The detector smooths the signature, so the anchored distance this produces is ~0.09.
         const auto noisy = [](int k) { return quiet(k % 2 ? 65 : 35, k % 2 ? 35 : 65); };
@@ -250,8 +250,8 @@
         if (fires) fail("the learned band fired on quiet-state movement");
     }
     // A TYPED BAND UNDER THE REAL SAMPLER. The shipped configuration cannot reach this -- the
-    // controller runs the learned band -- but an operator who sets --flip-auto-band AND the sampler
-    // can, and on a tree where the typed path bypasses the floor the fingerprint lane measured 73
+    // controller runs the learned band. This internal explicit-band control preserves the earlier
+    // regression: where the typed path bypassed the floor the fingerprint lane measured 73
     // two-consecutive exceedances in 600 stationary windows at K=100 and 208 at K=60: a spurious
     // maneuver every few seconds on a load that never changed. Driven here through THEIR writer, a
     // stationary 50/50 stream at depth 32, with the floor in place: none, and a real mix change
@@ -259,7 +259,7 @@
     {
         for (uint32_t window : {60u, 100u}) {
             XorShift rng;
-            FlipShiftDetector typed(2, 8);   // --flip-auto-band 2, the operator's knob
+            FlipShiftDetector typed(2, 8);   // internal explicit-band control: 2%
             int fires = 0, confirmed = 0, streak = 0;
             for (int w = 0; w < 60; w++) {
                 FlipFingerprintWriter writer;
diff --git a/tests/rlcache_churn.py b/tests/rlcache_churn.py
index b56ae4041ae0843dc67443f16017270063853124..f8a2c0730bb0a4fb6c3b5f6282c547226bb76599
--- a/tests/rlcache_churn.py
+++ b/tests/rlcache_churn.py
@@ -59,7 +59,7 @@
 
 
 # THE LOAD BALANCER MUST HAVE SOMETHING TO BALANCE. A uniform key distribution over one small key
-# space keeps every shard at the same op rate, the imbalance never crosses lb-imbalance-pct, and no
+# space keeps every shard at the same op rate, the imbalance never crosses the learned band, and no
 # shard ever changes owner -- which is the precondition for the defect this battery exists for
 # (measured: 7.8M ops, 0 migrations). A hot window that ROTATES makes a few shards hot, then makes
 # different ones hot, so the balancer moves shards continuously and in both directions, which is

codex
Merged both lanes. [MERGE.md](/home/user/Projects/cx-merge/MERGE.md) records all 49 input paths, conflict decisions, and deliberate drops.

- Dispatch retirement wins: timing’s rewrite still requires the deleted ownership instrument. Both files are removed.
- OFF RENAME’s four-attempt hard failure and the surviving release/mechanism split remain.
- Verified expectations: **quick 325 / full 342**. Constants remain unchanged.
- Syntax, merge-accounting, and isolated RENAME checks passed. No build, server, benchmarks, or gate ran.

The inherited MGET reader-retry violation remains explicitly documented.
diff --git a/AUDIT-TESTS.md b/AUDIT-TESTS.md
index e9cd43ffbc376172717531d0037de1c79fd23690..fe1fd0e2c39e965f605e4a30894a545129541534
--- a/AUDIT-TESTS.md
+++ b/AUDIT-TESTS.md
@@ -122,7 +122,7 @@
 | MM floor (memtier 20 s, sampled 8+6) | 1 | 14 | measurement, keep |
 | saturated-flip memtier | 1 | 25+3 | measurement, keep |
 | `flipctl.py --stable-seconds 30` (+ramp/idle/surge, timeout 300) | 1 | 60-120 est. | measurement, keep |
-| `xshard_dispatch_scale.sh` (2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 | 90-150 est. | measurement, keep |
+| Retired `xshard_dispatch_scale.sh` (historical 2 pairs x 2 boots x 7 rounds x 2 x 400k ops) | 1 historically, 0 now | 90-150 est. historically | manual ownership instrument removed; see `MERGE.md` |
 | `atomic_torn.py` hammers (3+2.5+2+2.5 s) + OFF re-rolls (up to 14 s) + RENAMENX/COPY parks (2 x 64 x 100 ms = 12.8 s) + window/lease/churn (~6 s) | 1 quick, +1 ASAN | ~45-60 each | re-rolls are dead time on this kernel (F6) |
 | `spinprobe.py` (6 s baseline + 6 s probe) | 3 | 37.5 | measurement, keep |
 | Python batteries' literal sleeps (census: flipctl 17 s, limits 5.8, edgetime 4.5, atomic_ryow 4, hexpire 3.4 — most run x2) | | ~60 | keep |
diff --git a/DESIGN-GATEHYGIENE.md b/DESIGN-GATEHYGIENE.md
index 5ea9df93e4548fb73a7e78b38debd52d876ad658..e6ac34a10326fe8880eaa664690832da3875b9b8
--- a/DESIGN-GATEHYGIENE.md
+++ b/DESIGN-GATEHYGIENE.md
@@ -459,7 +459,21 @@
 
 ---
 
-## 10. Follow-up diff on `78c3e5391` — 2026-09-07, runtime validation pending
+## 10. Incoming timing lane on `78c3e5391` — 2026-09-07
+
+**Merged disposition:** the maintainer reports the incoming timing lane gated 344/344 with no
+SKIP rows. The account below records that lane's original implementation and offline validation;
+it is not a validation claim for the merged tree. Its dispatch wrapper/helper and timing evaluator
+are retired: repeatability checks did not replace the deleted manual ownership instrument. The
+configurable-window shrink samples and shrink/reclaim budgets are superseded by the derived-window
+held-burst liveness and old-generation reconfiguration witnesses in `tests/atomicwindow.py`.
+All those mechanism, exact-reply, bound, and credit-reclamation checks remain mandatory on every
+build. The original OFF RENAME four-roll hard-failure control is unchanged, and the 1.5-second
+promotion budget still requires `--release-build`. The merged ledger requires quick 325/full 342;
+the constants remain untouched. [MERGE.md](MERGE.md) records the complete conflict and row accounting.
+
+The rest of this section is historical lane evidence, including its now-superseded dispatch,
+shrink, admission-calibration, and ledger statements.
 
 This is a test-only diff in `cx-timing`. No server, benchmark, gate, or compiler was run. The
 earlier acceptance results above describe earlier changes, **not this diff**. The worked fixes
diff --git a/DESIGN-flipfp.md b/DESIGN-flipfp.md
index 32eb6f46c24fb688db6accc79537fbd664bd084c..f705b25837c72a011da71df06dec09d2757f05ae
--- a/DESIGN-flipfp.md
+++ b/DESIGN-flipfp.md
@@ -311,7 +311,9 @@
 detector. The signal and the floor cannot be read from one run -- with a learned band the trigger
 fires and `reset()` wipes the distance -- so each arm is run twice: `--flip-auto-band 90` holds the
 anchor and lets the distance converge (the signal), `--flip-auto-band -1` learns the floor.
-`scratch/flipfp/fpmargin.sh`.
+The former `scratch/flipfp/fpmargin.sh` implemented this two-band experiment. It is retired in the
+merged tree because neither pinning the band nor disabling its retriggers has a public control.
+These historical measurements remain evidence only for their recorded builds; see `MERGE.md`.
 
 **BITCOUNT -> INCR** (the redesign lane's reference change), 2:2 split, `--rate-limiting` pinned:
 
diff --git a/DESIGN.md b/DESIGN.md
index 0fba740d94ba1915868410de40bcb685ae675833..262f7b730cc2cedfdcffb964afb729c758dc97fc
--- a/DESIGN.md
+++ b/DESIGN.md
@@ -1,6 +1,6 @@
 # Thread amortization study surface
 
-`thread-mode` and `overlap` are one boot-latched measurement surface. The documented mode
+`thread-mode` and the study-only `x-overlap` are one boot-latched measurement surface. The mode
 names are `2s` (separate IO and executor threads, the default) and `1s` (one generalized thread owns
 both roles). `split` and `fused` remain parser aliases so old invocations keep working.
 
@@ -9,11 +9,11 @@
 | `2s` | ordinary split IO/ex loops | exact `t-iopipe` WB/IFID batch schedule | rejected at config validation |
 | `1s` | coarse IFID → EX → WB rotation | exact `t-genthread` `iofused` schedule | gated `iofused` three-way schedule |
 
-The compatibility options `thread-pipeline` and `genthread-schedule` remain accepted: the former
-is a numeric alias for `overlap`, while the latter maps `coarse`, `iofused`, and the legacy
-`streams` name to `1s` plus overlap 0, 1, and 2 respectively. The legacy name is only a parser
-alias; overlap 2 no longer runs the streams implementation. Overlap 2 emits a boot warning because
-it is an experimental research schedule. Unified overlap 1 and 2 require `net-io uring`; the split
+Use `--x-overlap 0|1|2` for these study schedules and `--x-ex-sched 0|1` for the executor study.
+The old public schedule spellings and compatibility aliases are removed; see
+[DESIGN-KNOBS.md](DESIGN-KNOBS.md). Below, "overlap" names the internal schedule value.
+Overlap 2 emits a boot warning because it is an experimental research schedule.
+Unified overlap 1 and 2 require `net-io uring`; the split
 iopipe schedule retains its explicit epoll specialization. No schedule constant is runtime-tunable.
 
 Overlap 1 is deliberately the measured implementation, not a family resemblance. In `2s`, its
diff --git a/MERGE.md b/MERGE.md
index e29579036e98115056aa3e3a5401db083d2b7820..d70892fe599ab8e1e4e0c67b2a54cc390f585ea5
--- a/MERGE.md
+++ b/MERGE.md
@@ -59,7 +59,7 @@
 
 ### OFF RENAME and the atomic timing split
 
-The complete base block from “Broadened movers: RENAME” through the following store-family
+The complete base block from “Broadened movers: RENAME” up to the following store-family
 section is byte-for-byte preserved. It still runs `for _roll in range(4)` at
 `tests/atomic_torn.py:771`, arms and clears the hop delay in `try/finally`, retries only a
 clean miss, and stops on a witnessed tear, worker error, or surviving helper. The OFF assertion
@@ -323,6 +323,9 @@
 - Input-path/hash accounting, full-file comparisons for retained lane files, static gate-row
   comparison and loop arithmetic, unchanged EXPECT assignments, layout-assertion inspection,
   removed-flag/caller search, conflict-marker search, and `git diff --check`.
+- Reconstructed each merged conflict file from the captured inputs: the atomic file equals
+  timing plus exactly the documented knobs admission replacement; the gate equals knobs plus
+  exactly the surviving timing call-site changes and the documented comment resolutions.
 
 No C++ syntax check, build, server, benchmark, full test, or gate result is claimed. The
 maintainer supplies the merged runtime and performance verdict.
diff --git a/NOTES-HYGIENE.md b/NOTES-HYGIENE.md
index 23506bce64f00cb0dccaaff93362ccd30deab229..f224db07f4ceb2e4791c7cbb471b87d83a0a3e93
--- a/NOTES-HYGIENE.md
+++ b/NOTES-HYGIENE.md
@@ -352,7 +352,8 @@
 ## Test-file census
 
 The calibration examples expireindex.py, borrow_registry.py, and xshard_dispatch_scale.sh were
-already wired in the current parent. aof_frames.py is indirectly executed by aof_frame_order.py;
+already wired in this note's historical parent. The merged tree retires the dispatch instrument;
+the two surviving batteries use `--release-build`. aof_frames.py is indirectly executed by aof_frame_order.py;
 aof_rewrite.py and aof_rewrite_triggers.py are driven by matrix wrappers; differ.py is driven by
 differ_gate.sh; niclib.sh and gate_refs.txt support optional full-tier NIC rows.
 
@@ -456,7 +457,7 @@
 notify: ok (notify_events_fired=1608)
 ~~~
 
-Purpose-boot matrix commands:
+Historical purpose-boot matrix commands (superseded by the current gate's network-engine grammar):
 
 ~~~sh
 PERSIST_IO=normal GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_matrix.sh
@@ -465,7 +466,8 @@
 PERSIST_IO=uring  GATE_PORT=7600 GATE_CORES=128-135 tests/aof_rewrite_trigger_matrix.sh
 ~~~
 
-Other purpose-booted battery invocations (each boot used the knobs documented by gate.sh):
+Historical purpose-booted battery invocations (not recipes for the merged tree; the dispatch
+instrument and script OFF arm below are retired):
 
 ~~~sh
 python3 tests/xscript.py 127.0.0.1 7600 off|limit|window
diff --git a/NOTES-XPERF2.md b/NOTES-XPERF2.md
index d4ce7950b1396997d44f0945f6f5179aa625e59e..d1edc822a5e7abc68e6aefc3d5a55986b0bf178e
--- a/NOTES-XPERF2.md
+++ b/NOTES-XPERF2.md
@@ -8,7 +8,7 @@
 |---|---|---|---|---|
 | E4 | ExpireIndex never shrinks; memsets the whole sidecar on live→0; single-pass rehash | **REPRODUCED** (both halves) | yes | `tests/expireindex.py` |
 | E5 | zero-copy borrow registry is a linear vector scanned per borrow/release/retire | **REPRODUCED** | yes | `tests/borrow_registry.py` |
-| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes | `tests/xshard_dispatch_scale.sh` |
+| E6 | non-atomic scatter dispatch zeroes 512 B and scans all `nthreads()` per cross-shard op | **REPRODUCED** | yes, historically | retired manual-map dispatch instrument; see `MERGE.md` |
 | E7 | segment mode is absorbing: once triggered every later reply is a malloc + iovec slot | **NOT REPRODUCED** | no | none |
 
 Rig: EPYC 9754, cores 48–63 only, ports 7200–7209 only, loopback. Oracle = vanilla redis 7.4 at
@@ -292,18 +292,20 @@
 |---|---|---|---|---|
 | `tests/expireindex.py <host> <port>` | `--shards 1 --ratio 1:1 --enable-debug-command yes` | growth-trigger cost ratio over a 16× population ≤ 8.0; live→0 DEL ≤ 1.25× a neighbouring DEL | FAIL 2 (16.30×, 2.00×) | PASS (1.65×, 1.00×) |
 | `tests/borrow_registry.py <host> <port>` | `--shards 1 --zc-min 64 --client-output-buffer-limit normal 0 0 0 --enable-debug-command yes` | borrowed-GET per-op cost growth over 0 → ~2000 live borrows ≤ 1.05 | FAIL (1.078, 1.079) | PASS (1.008, 1.008, 1.009) |
-| `tests/xshard_dispatch_scale.sh` (+ `.py`) | boots its own 4-thread and 128-thread arms | dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
+| Retired `tests/xshard_dispatch_scale.sh` (+ `.py`) | historically booted fixed-owner 4-thread and 128-thread arms | historical dispatch-excess ratio 128t/4t ≤ 1.20 | FAIL (1.297) | PASS (1.110) |
 
 Each also carries its own non-vacuity checks: `expireindex.py` asserts INFO `expires` really
 reached the population under test and that INFO `expired_keys` MOVED (so the sampler is proven to
 still visit deadlines parked mid-migration), plus a no-TTL negative control on both timing legs;
 `borrow_registry.py` asserts `CLIENT LIST oll` grew (the borrow path is proven live, not assumed),
 carries a non-borrowed control arm, and refuses to run unless the zc gate straddles its two probe
-sizes; `xshard_dispatch_scale.py` refuses to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` prove
+sizes; the retired `xshard_dispatch_scale.py` refused to report unless `DEBUG SHARD` + `DEBUG LBSIGNALS` proved
 the cross pair really fanned out to two owner threads and the control stayed on one.
 
-None of the three is wired into `tests/gate.sh`: they each need their own boot geometry and the
-gate's port/cores belong to the mainline operator. Run them as above.
+These were standalone instruments when measured. The merged gate runs the expiry and borrow
+batteries with `--release-build`. The dispatch wrapper and helper are retired because their manual
+ownership geometry is unavailable; the historical measurements above do not validate default
+round-robin ownership. See [MERGE.md](MERGE.md).
 
 ## Evidence
 
diff --git a/docs/flipctl-design.md b/docs/flipctl-design.md
index e6a5e31a6c05171b172746f2586a01d09fa1e84e..9280bc6ad5f9b98d9535c141a4e45f6460962398
--- a/docs/flipctl-design.md
+++ b/docs/flipctl-design.md
@@ -8,8 +8,7 @@
 ## Ownership and footprint
 
 - `src/main.cc` is the controller cron owner. When `flip-auto=1`, the main/monitor thread calls
-  `Server::flipctl_tick()` once per existing LB tick (`lb-tick-ms`; if that mechanism is disabled,
-  the period derives from the provisioned thread count). No IO or EX loop runs controller policy.
+  `Server::flipctl_tick()` on its internal observation cadence. No IO or EX loop runs controller policy.
 - `src/core/flipctl.{h,cc}` contains the fingerprint representation, shift detector, and controller
   state machine. Controller vectors allocate only when `flip-auto=1`.
 - Each `ThreadCtx` embeds one `FlipFingerprintWriter`. It has no dynamic storage. Only the physical
@@ -46,10 +45,10 @@
 measurement quantum, avoiding the zero-anchor discontinuity where one rare admin argument would
 otherwise have distance one.
 
-With `flip-auto-band=-1`, the band is twice the maximum quiet adjacent/drift distance learned at
-the final split, floored by the four-family sampling quantum divided by observed commands. A
-numeric value is a percent. Zero disables fingerprint and collapse re-triggers. Final anchor
-learning consumes a number of closed aggregates derived from the provisioned thread count. It is
+The automatic band is twice the maximum quiet adjacent/drift distance learned at
+the final split, floored by the four-family sampling quantum divided by observed commands.
+It has no public override. Final anchor learning consumes a number of closed aggregates derived
+from the provisioned thread count. It is
 done after maneuver-only age sampling is disarmed, so the controller cannot detect its own signal
 cost transition as a workload change.
 
@@ -64,14 +63,15 @@
 falls below a threshold derived from its own change jitter and measurement quantum. Directional
 connection ramp-up therefore remains drift, while jittery stationary traffic can qualify even when
 its absolute rate never enters a quiet band. A 30-second ceiling, converted to ticks from
-`lb-tick-ms`, caps deferral under non-idle traffic. Traffic at or below one command per provisioned
-thread per tick resets the learning/cap state, so an idle server remains in
+the internal observation cadence, caps deferral under non-idle traffic. Traffic at or below one
+command per provisioned thread per tick resets the learning/cap state, so an idle server remains in
 `awaiting-load-stability` and never pays for a maneuver it cannot use.
 
-`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. An explicit
-`lb-age-sample-rate` is used when nonzero; otherwise the rate derives from the provisioned thread
-count. IO and EX loops apply the published rate to their own `LoopSignals`, preserving single-owner
-writes. IO releases its ROB-head age map when the rate returns to zero.
+`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. Its rate
+derives from observed command volume and the three-tick decision window, and adapts on completed
+traffic windows; see [DESIGN-KNOBS.md](../DESIGN-KNOBS.md). IO and EX loops apply the published
+rate to their own `LoopSignals`, preserving single-owner writes. IO releases its ROB-head age
+map when the rate returns to zero.
 
 After a stabilized baseline rate, `issue_initial_jump()` subtracts the per-thread maneuver window.
 For each role it computes:
@@ -88,9 +88,10 @@
 not vetoed by idle peers. If either role has no measured work/busy quantity, measurement continues;
 the controller does not invent a workload prior.
 
-The calculation is performed in placement units (one logical CPU normally, one sibling pair under
-`smt-mode=1`) and then reported/committed in threads. The target passes `nio_eq` in the measured
-direction by `max(1, abs(nio_eq-nio_now)/2)`, clamped to one live unit of each role. Even when the
+The calculation is performed in placement units (one logical CPU normally, one sibling pair when
+the allowed topology contains complete pairs) and then reported/committed in threads. The target
+passes `nio_eq` in the measured direction by `max(1, abs(nio_eq-nio_now)/2)`, clamped to one live
+unit of each role. Even when the
 equalized split rounds to the current split, the direction of greater measured demand supplies the
 deliberate one-unit overshoot.
 
@@ -143,7 +144,7 @@
 
 ## Integration and observability
 
-- `src/core/config.h` is the sole parser/default source for `flip-auto`, `flip-auto-band`, and
+- `src/core/config.h` is the sole parser/default source for `flip-auto` and
   `flip-work-window`; `src/cmd/t_server.cc` exposes them as immutable CONFIG values.
 - `tomokv.conf` documents the numeric grammar and off/auto semantics.
 - `INFO FLIPCTL` reports state/phase, anchor split/rate, bands, total/per-reason trigger counts, and
diff --git a/docs/specs/AUDIT-TLS.md b/docs/specs/AUDIT-TLS.md
index 0256e887f5d27c210d436bc219f3c7521ddad0db..87a2eaca3137a47f7c0e1567794650f57531860b
--- a/docs/specs/AUDIT-TLS.md
+++ b/docs/specs/AUDIT-TLS.md
@@ -3,7 +3,7 @@
 > **Where it landed (2026-09-04).** The feature shipped as `src/net/tls.{h,cc}` (this document
 > proposes `src/net/tls_ctx.h` + `src/net/tls_conn.h`); the test is `tests/tls.py` (no
 > `tests/gen-certs.sh`); no `tls-bio-size` knob exists. Defaults: AES-128-GCM-first cipher lists
-> and `tls-ktls yes` (`tomokv.conf`, NETWORK).
+> and automatic kTLS negotiation with userspace fallback (`tomokv.conf`, NETWORK).
 
 Governing audit. Read before writing any TLS code in `/home/user/Projects/tomokv-cpp-perthread`.
 No repository was modified in producing this.
diff --git a/scratchpad/rlbatch/lib.sh b/scratchpad/rlbatch/lib.sh
index 7bcf3e4e6030c79174bf35d9bb1b3dfb11f80c42..12184314d2aafac0dc7aa48db761a3bdec5de8c7
--- a/scratchpad/rlbatch/lib.sh
+++ b/scratchpad/rlbatch/lib.sh
@@ -17,7 +17,7 @@
 boot_srv(){ # boot_srv <binary> <logfile> [extra args...] -> SRV pid
   local bin="$1" log="$2"; shift 2
   guard_port "$PORT" || return 1
-  # EXTRA_ARGS lets a caller add boot flags (e.g. --read-local-interleave 0) without a second copy
+  # EXTRA_ARGS lets a caller add boot flags (e.g. --lb 0) without a second copy
   # of this function; word splitting is intentional.
   # shellcheck disable=SC2086
   taskset -c "$SRVCORE" "$bin" --port "$PORT" --bind 127.0.0.1 --shards 16 \
diff --git a/tests/flipctl_unit.cc b/tests/flipctl_unit.cc
index 540a6c40e9589572431075b623bc553c76ee8d85..33209258e2958771d60f86b919f6f510e9977737
--- a/tests/flipctl_unit.cc
+++ b/tests/flipctl_unit.cc
@@ -184,11 +184,11 @@
 
     // THE TYPED BAND IS A FLOOR, NOT A CEILING. The gate-hygiene lane's instrumented failure: a
     // driver holding its rate to 0.07% across 34 samples, a fingerprint distance of 0.2518 against
-    // a flat --flip-auto-band 2 (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
+    // a flat explicit band of 2% (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
     // A detector whose signature genuinely moves between adjacent quiet windows must end up with a
     // band above that movement whatever the operator typed, and must still fire on a real change.
     {
-        FlipShiftDetector typed(2, 4);   // --flip-auto-band 2
+        FlipShiftDetector typed(2, 4);   // internal explicit-band control: 2%
         // Stationary but NOISY: the read/write split alternates 35:65 / 65:35 window to window.
         // The detector smooths the signature, so the anchored distance this produces is ~0.09.
         const auto noisy = [](int k) { return quiet(k % 2 ? 65 : 35, k % 2 ? 35 : 65); };
@@ -250,8 +250,8 @@
         if (fires) fail("the learned band fired on quiet-state movement");
     }
     // A TYPED BAND UNDER THE REAL SAMPLER. The shipped configuration cannot reach this -- the
-    // controller runs the learned band -- but an operator who sets --flip-auto-band AND the sampler
-    // can, and on a tree where the typed path bypasses the floor the fingerprint lane measured 73
+    // controller runs the learned band. This internal explicit-band control preserves the earlier
+    // regression: where the typed path bypassed the floor the fingerprint lane measured 73
     // two-consecutive exceedances in 600 stationary windows at K=100 and 208 at K=60: a spurious
     // maneuver every few seconds on a load that never changed. Driven here through THEIR writer, a
     // stationary 50/50 stream at depth 32, with the floor in place: none, and a real mix change
@@ -259,7 +259,7 @@
     {
         for (uint32_t window : {60u, 100u}) {
             XorShift rng;
-            FlipShiftDetector typed(2, 8);   // --flip-auto-band 2, the operator's knob
+            FlipShiftDetector typed(2, 8);   // internal explicit-band control: 2%
             int fires = 0, confirmed = 0, streak = 0;
             for (int w = 0; w < 60; w++) {
                 FlipFingerprintWriter writer;
diff --git a/tests/rlcache_churn.py b/tests/rlcache_churn.py
index b56ae4041ae0843dc67443f16017270063853124..f8a2c0730bb0a4fb6c3b5f6282c547226bb76599
--- a/tests/rlcache_churn.py
+++ b/tests/rlcache_churn.py
@@ -59,7 +59,7 @@
 
 
 # THE LOAD BALANCER MUST HAVE SOMETHING TO BALANCE. A uniform key distribution over one small key
-# space keeps every shard at the same op rate, the imbalance never crosses lb-imbalance-pct, and no
+# space keeps every shard at the same op rate, the imbalance never crosses the learned band, and no
 # shard ever changes owner -- which is the precondition for the defect this battery exists for
 # (measured: 7.8M ops, 0 migrations). A hot window that ROTATES makes a few shards hot, then makes
 # different ones hot, so the balancer moves shards continuously and in both directions, which is

tokens used
194,146
Merged both lanes. [MERGE.md](/home/user/Projects/cx-merge/MERGE.md) records all 49 input paths, conflict decisions, and deliberate drops.

- Dispatch retirement wins: timing’s rewrite still requires the deleted ownership instrument. Both files are removed.
- OFF RENAME’s four-attempt hard failure and the surviving release/mechanism split remain.
- Verified expectations: **quick 325 / full 342**. Constants remain unchanged.
- Syntax, merge-accounting, and isolated RENAME checks passed. No build, server, benchmarks, or gate ran.

The inherited MGET reader-retry violation remains explicitly documented.

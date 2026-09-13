# NOTES-SNAPCUT — SAVE/BGSAVE wrote half-applied cross-shard atomic groups

Branch `t-snapcut`. Lane resources: cores 64-79, ports 7210-7219, redis 7.4 oracle on 7212.
Every server was resolved from its listening socket (`ss -lntpH "sport = :<port>"`) and stopped by
that exact pid; no process was ever selected by name or pattern.

---

## TABLE — what was measured

| Arm (100 generation-tagged cross-shard groups, 8 keys each, span 6.4 shards avg) | BEFORE (HEAD) | AFTER (fix) |
|---|---|---|
| `--atomic 1`, MSET storm, 5x SAVE — torn groups per cut | **41 / 36 / 47 / 37 / 51** | **0 / 0 / 0 / 0 / 0** |
| `--atomic 1`, MSET storm, 5x BGSAVE — torn groups per cut | **47 / 52 / 44 / 60 / 50** | **0 / 0 / 0 / 0 / 0** |
| `--atomic 0` (DEFAULT), MULTI/EXEC storm, 5x SAVE — torn per cut | **9 / 9 / 9 / 8 / 11** | **0 / 0 / 0 / 0 / 0** |
| CONTROL live MGET reader on the same server, all arms | **0 torn / 1951–4505 reads** | **0 torn / 3401–19180 reads** |
| CONTROL quiesce-then-SAVE, `--atomic 1` | **0 / 100** | **0 / 100** |
| CONTROL redis 7.4 RDB, same shape, 3x BGSAVE mid-storm | **0 / 100** | (unchanged) |
| CONTROL `--atomic 0` + MSET storm — see caveat below | **4 / 0 / 7 / 6 / 6 torn, live reader ALSO torn 104/2819** | not a zero control |
| ABLATION: barrier armed, group drain removed | **0 / 17 / 17 / 18 / 26** | — |
| ABLATION: the literal hoist from the brief | **server wedges: `rdb_bgsave_in_progress:1` forever** | — |
| Gate row `atomic_groups mset` (new) | **FAIL, 146 torn over 3 cuts** | **PASS** |
| Gate row `atomic_groups exec` (new, default config) | **FAIL, 35 torn over 3 cuts** | **PASS** |
| INDICATIVE storm rate, no snapshot running, `--atomic 1` | 522,980 MSET/s | 565,204 MSET/s |

All torn counts are read out of the `.tomo` file by a byte scanner with no server involved.

---

## The defect

At a snapshot cut, a cross-shard atomic group whose records are installed on some owners and still
queued on others straddles the cut and lands in the snapshot **half applied**. The file then holds a
state that never existed and that no client could ever have observed.

The cut is not a single instant. `SnapshotManager::start()` walks the owners through
Preparing -> Freeze -> Mark, and each owner takes its own cut inside `FlatStore::snapshot_mark()`
(a table swap) on its own loop pass. Between the first owner acking Freeze and the last one, the
owners that have not frozen yet keep executing tasks. An atomic group with a fragment already
applied on owner A and a fragment still queued on owner B therefore has A's half inside the cut and
B's half outside it.

`src/snapshot/snapshot.cc` already had the machinery to prevent exactly this — arm
`set_snapshot_atomic_barrier(true)` so no new group is admitted, then wait out the ones already
dispatched — but both statements sat inside the `if (rewrite)` guard, so only the AOF-rewrite path
ran armed. Plain SAVE and BGSAVE ran unarmed.

### Reproduction (my own numbers, before touching any code)

Harness: 24 connections, each pre-rendering its whole byte stream so the server and not python is
the thing under load; each connection owns ~4 of the 100 groups and keeps 32 rounds of MSETs in
flight, which is what makes the executor queues deep enough for a group to straddle. Values 400 B.
Sustained 480–635 k MSET/s, `atomic_inflight` peaking at 101–121.

```
boot surface: atomic=[b'atomic', b'1'] conns=24 rounds=32 pad=400 groups=100
  atomic_inflight peak over 30 probes = 101
  SAVE#0 (0.003s)  keys=800 groups=100 TORN=41/100 span=6.44 eg=[(3, [12, 13]), (5, [18, 19])]
  SAVE#1 (0.003s)  keys=800 groups=100 TORN=36/100 span=6.44 eg=[(5, [23, 24]), (7, [6, 7])]
  SAVE#2 (0.004s)  keys=800 groups=100 TORN=47/100 span=6.44 eg=[(1, [1, 32]), (5, [8, 9])]
  SAVE#3 (0.005s)  keys=800 groups=100 TORN=37/100 span=6.44 eg=[(4, [1, 2]), (9, [5, 6])]
  SAVE#4 (0.006s)  keys=800 groups=100 TORN=51/100 span=6.44 eg=[(0, [6, 7]), (1, [20, 21])]
  storm: msets=979360 (503529/s) live_mget_reads=3056 LIVE_TORN_READS=0 errors=0
```

The torn examples are always two ADJACENT generations inside one group — the signature of a single
half-applied group, not of a scrambled file. `span=6.44` is measured with `DEBUG SHARD` on every
key: these groups really are cross-shard (min span 4 of 16 shards), which a key-name-only test
cannot claim, because the hash seed is drawn from the kernel at boot.

BGSAVE under the identical storm: 47 / 52 / 44 / 60 / 50.

### The four controls

1. **`--atomic 0`, same MSET storm** — *this control does NOT read zero for me, and the brief's
   claim that it does is wrong.* 4 / 0 / 7 / 6 / 6 torn groups per cut. But it is also **not a
   defect**: in the same run the live MGET reader saw 104 torn groups out of 2819 reads. At
   `--atomic 0` MSET is not atomic, so a reader can see a half-applied MSET, and a snapshot that
   records one is recording a state that really existed. The control that matters is the pairing:
   the file may only show what a live reader could show. At `--atomic 1` the live reader was torn
   **0** times in every single run while the file was torn 36–51 times per cut — that gap is the
   defect. I replaced this control with the `--atomic 0` MULTI/EXEC arm below, which *is* a genuine
   zero control because EXEC is atomic at both settings.
2. **quiesce-then-SAVE, `--atomic 1`** — `TORN=0/100`, both before and after. The tear needs
   concurrency; it is not a serialization bug.
3. **live concurrent MGET reader** — 0 torn reads in every `--atomic 1` and every EXEC run
   (3056, 3197, 4505, 2986, 6437, 19180 reads). This is the control that makes the file's tear a
   contract violation rather than a permitted interleaving.
4. **redis 7.4 RDB oracle, same shape** — same 100-group harness against
   `/tmp/claude-1000/redis74/src/redis-server` at 231 k MSET/s, three BGSAVEs taken mid-storm.
   Each RDB was then loaded into a fresh redis and read back: `TORN=0/100`, `TORN=0/100`,
   `TORN=0/100` (dbsize 800 each). Redis forks, so its cut is a single instant by construction.

### The finding the brief did not have: this reaches the DEFAULT config

`--atomic 1` is **not** the default (`src/core/config.h:213`, `uint32_t atomic = 0`), so the MSET
shape above is a defect in a shipped non-default mode. **But EXEC force-admits an atomic group at
BOTH settings** (`src/cmd/multi.inc:948-950` calls `atomic_can_admit`/`atomic_try_admit` with
`force = true`), so a cross-shard MULTI/EXEC transaction is atomic to readers on a plain default
boot — and it was torn by SAVE just the same:

```
boot surface: atomic=[b'atomic', b'0'] conns=24 rounds=8 pad=400 groups=100
  SAVE#0 (0.003s)  keys=800 groups=100 TORN=9/100  eg=[(16, [21, 22]), (31, [30, 31])]
  SAVE#1 (0.003s)  keys=800 groups=100 TORN=9/100  eg=[(6, [9, 15]), (17, [5, 31])]
  SAVE#2 (0.004s)  keys=800 groups=100 TORN=9/100  eg=[(13, [2, 3]), (14, [30, 31])]
  SAVE#3 (0.004s)  keys=800 groups=100 TORN=8/100  eg=[(34, [22, 23]), (47, [19, 20])]
  SAVE#4 (0.006s)  keys=800 groups=100 TORN=11/100 eg=[(23, [10, 11]), (25, [12, 13])]
  storm: msets=305400 (157332/s) live_mget_reads=2986 LIVE_TORN_READS=0 errors=0
```

Zero torn live reads, 8–11 torn transactions per snapshot. So the honest scope is: **a default-config
server that uses cross-shard MULTI/EXEC and takes a SAVE/BGSAVE under load writes half-applied
transactions into the snapshot.** That is a wider blast radius than the brief assumed, and it is
stated here rather than inflated or buried: the MSET shape still needs the non-default `--atomic 1`,
and a server that never uses cross-shard transactions is unaffected at either setting.

---

## Why the fix in the brief could not be taken literally

The brief prescribed hoisting both statements out of the `if (rewrite)` guard. The barrier hoist is
right. **The drain hoist as written deadlocks**, and I built it and watched it happen before
designing around it:

```
booted pid=719592 on port 7210      # naive hoist, --atomic 1, 24-connection MSET storm
Terminated                          # SAVE never returned; 100 s timeout
$ redis-cli -p 7210 ping            -> PONG
$ redis-cli -p 7210 info | grep -E "rdb_bgsave_in_progress|atomic_inflight"
rdb_bgsave_in_progress:1
atomic_inflight:13                  # stuck there forever
```

The reason: **`atomic_inflight()` is the wrong quantity to wait on from a snapshot.** It counts
per-IO-thread admission leases, and a lease is released only in `xshard_destroy()` /
`release_admission()`, on the **reply-retirement** path of the IO thread that admitted the group
(`src/cmd/scatter_engine.inc:1365`, `src/cmd/multi.inc:135`). SAVE and BGSAVE are `ConnLocal`
commands: they execute inline on an IO thread (`src/core/io_loop.h:1009`), so that thread's own
loop is not running, so its own clients' groups can never retire, so the count never reaches zero.
The existing AOF-rewrite path never hit this because `AofManager::maybe_start_rewrite()`
(`src/persist/aof.cc:1262`) refuses to start at all unless `atomic_inflight() == 0` and simply
retries on a later pass — a luxury an inline SAVE does not have.

**What the cut actually needs is weaker than `atomic_inflight() == 0`.** It needs "no group is only
PARTLY installed". A group's records are dispatched to every participating owner in one indivisible
IO step — `io_loop.h:1139-1164` and `:1186-1196` check free slots on *all* participants first and
only then post, aborting the process if a post fails, so there is no partially-dispatched state to
worry about — and they finish on the **owner** threads, which keep running while a SAVE blocks.

So the fix adds exactly that quantity.

---

## The fix

`Server::atomic_apply_inflight()` — groups admitted but not yet installed on every owner.
Opened at admission, closed at the first of two events, neither of which needs the snapshot's own
thread:

| site | file | what it does |
|---|---|---|
| open | `src/cmd/scatter_engine.inc` (`state->admitted = admitted`) | `atomic_apply_open()` on a successful `atomic_try_admit` |
| open | `src/cmd/multi.inc` (EXEC admission) | same, for the transaction |
| close | `src/cmd/scatter_engine.inc` `xshard_complete()`'s `final()` | the **last owner** — every fragment installed |
| close | `src/cmd/multi.inc` finalizer (`state.finalized.store(true)`) | the **owner** that makes the one commit decision |
| close | `src/cmd/scatter_engine.inc` `xshard_destroy()` / `release_admission()` | the group died BEFORE dispatch (synchronous, pre-dispatch) |

The close is idempotent (`std::atomic<bool> apply_open` exchanged once), so whichever end arrives
first closes it and the other is a no-op.

`SnapshotManager::start()` then:
* arms `set_snapshot_atomic_barrier(true)` unconditionally, on every snapshot path;
* calls the new `drain_atomic_groups()` before publishing `Phase::Freeze`, which waits for
  `atomic_apply_inflight() == 0` (monotone, since the barrier blocks new admissions).

### Both halves are load-bearing — ablation

Barrier armed, drain removed, same `--atomic 1` storm:

```
  SAVE#0 TORN=0/100    SAVE#1 TORN=17/100   SAVE#2 TORN=17/100
  SAVE#3 TORN=18/100   SAVE#4 TORN=26/100        (total 78)
```

The barrier alone cuts the tear roughly in half and does not remove it. Neither statement is
decoration.

### FIRED-proof counters (INFO Persistence)

`snapshot_cuts_armed`, `snapshot_cuts_waited`, `snapshot_groups_drained`. `cuts_waited` counts only
cuts whose drain **actually observed a non-zero count and blocked** — the brief was right that this
is the load-bearing one, because a drain that never blocks is indistinguishable from a missing one.
Post-fix, under the MSET storm: `cuts_armed:10 cuts_waited:8 groups_drained:1390`. The MSET gate
arm asserts `cuts_waited` advanced.

The EXEC arm at `--atomic 0` legitimately reports `cuts_waited:0` at its lower group rate — by the
time the drain samples, the barrier has already emptied the pipeline during the Preparing phase.
That arm therefore reports the counter but does not gate on it; gating a counter that can honestly
read zero would produce a flaky row rather than a stronger one.

---

## The gate rows

`tests/snap_cut_battery.py` had **zero multi-key writes**, so booting it at `--atomic 1` would have
passed on the broken tree and proved nothing. New mode:

```
tests/snap_cut_battery.py PORT atomic_groups DUMPFILE [mset|exec] [saves]
```

100 generation-tagged groups of 8 keys. Every key of a group always carries the same generation, so
a group whose keys disagree in the file is a state that never existed. It asserts, in order:
purpose-boot surface; **`DEBUG SHARD` geometry** (every group spans >1 shard — a same-owner
"cross-shard" battery gates nothing); the storm really loaded the server; the atomic group lane
fired; the live MGET reader really read; **live reader saw 0 torn**; every cut armed the barrier;
(mset arm) the drain actually blocked; and 0 torn groups in every `.tomo` file, scanned byte-wise.

Wired into `tests/gate.sh` inside the existing `for PERSIST_IO in normal uring` loop as two rows —
`--atomic 1` + MSET (5 cuts), and the control `--atomic 0` + MULTI/EXEC (3 cuts). The MSET row
takes 5 cuts rather than 3 purely for margin on the `cuts_waited > 0` assertion: observed
4-of-5, 8-of-10, 3-of-3, 2-of-3, 4-of-5 across runs here, never zero, and a smaller/slower gate box
makes the executor queues deeper rather than shallower. (I did not run `tests/gate.sh` itself; it
owns port 7899 and cores 0-7.)

### FAILS on unfixed HEAD

```
atomic_groups arm: writes=mset saves=3 [b'atomic', b'1']
  ok   every group spans >1 shard (min=4 avg=6.46)
    cut#0 0.011s keys=800 groups=100 torn=44 e.g. [(1, [9, 10]), (2, [7, 8]), (4, [9, 10])]
    cut#1 0.010s keys=800 groups=100 torn=53 e.g. [(2, [2, 3]), (3, [9, 10]), (5, [3, 4])]
    cut#2 0.011s keys=800 groups=100 torn=49 e.g. [(0, [13, 14]), (1, [3, 4]), (4, [13, 14])]
  storm: writes=705344 live_mget_reads=1951 live_torn_reads=0 errors=0
  counters: atomic_groups+=705344 cuts_armed+=0 cuts_waited+=0 groups_drained=-1
  ok   CONTROL: live MGET never saw a torn group (0 torn)
  FAIL every cut armed the atomic barrier (0 of 3)
  FAIL the group drain actually blocked on something (0 of 3 cuts)
  FAIL NO half-applied group in any snapshot file (146 torn)
ATOMIC_GROUP_CUT FAIL
EXIT=1

atomic_groups arm: writes=exec saves=3 [b'atomic', b'0']
  ok   EXEC arm runs at the DEFAULT atomic 0 (EXEC force-admits a group anyway)
  ok   every group spans >1 shard (min=5 avg=6.52)
    cut#0 0.012s keys=800 groups=100 torn=13 e.g. [(1, [7, 8]), (10, [1, 16]), (11, [2, 3])]
    cut#1 0.010s keys=800 groups=100 torn=10 e.g. [(5, [7, 8]), (18, [15, 16]), (43, [6, 7])]
    cut#2 0.011s keys=800 groups=100 torn=12 e.g. [(14, [15, 16]), (21, [1, 16]), (29, [4, 5])]
  storm: writes=221728 live_mget_reads=2008 live_torn_reads=0 errors=0
  ok   CONTROL: live MGET never saw a torn group (0 torn)
  FAIL every cut armed the atomic barrier (0 of 3)
  FAIL NO half-applied group in any snapshot file (35 torn)
ATOMIC_GROUP_CUT FAIL
EXIT=1
```

### PASSES on the fixed tree

```
atomic_groups arm: writes=mset saves=3 [b'atomic', b'1']
  ok   purpose boot exposes atomic knob
  ok   MSET arm runs at atomic 1
  ok   every group spans >1 shard (min=4 avg=6.38)
    cut#0 0.009s keys=800 groups=100 torn=0
    cut#1 0.008s keys=800 groups=100 torn=0
    cut#2 0.009s keys=800 groups=100 torn=0
  storm: writes=417600 live_mget_reads=6437 live_torn_reads=0 errors=0
  counters: atomic_groups+=417600 cuts_armed+=3 cuts_waited+=3 groups_drained=376
  ok   storm ran clean
  ok   storm actually loaded the server (417600 group writes)
  ok   the atomic group lane fired (417600 groups admitted)
  ok   live reader actually read (6437 MGETs)
  ok   CONTROL: live MGET never saw a torn group (0 torn)
  ok   every cut armed the atomic barrier (3 of 3)
  ok   the group drain actually blocked on something (3 of 3 cuts)
  ok   NO half-applied group in any snapshot file (0 torn)
ATOMIC_GROUP_CUT PASS

atomic_groups arm: writes=exec saves=3 [b'atomic', b'0']
  ok   EXEC arm runs at the DEFAULT atomic 0 (EXEC force-admits a group anyway)
  ok   every group spans >1 shard (min=4 avg=6.53)
    cut#0 0.007s torn=0    cut#1 0.007s torn=0    cut#2 0.007s torn=0
  storm: writes=192864 live_mget_reads=3401 live_torn_reads=0 errors=0
  counters: atomic_groups+=192864 cuts_armed+=3 cuts_waited+=0 groups_drained=0
  ok   CONTROL: live MGET never saw a torn group (0 torn)
  ok   every cut armed the atomic barrier (3 of 3)
  ok   NO half-applied group in any snapshot file (0 torn)
ATOMIC_GROUP_CUT PASS
```

Both arms also pass with `--persist-io normal` (mset: `cuts_waited 2 of 3`, torn 0; exec: torn 0).

---

## Regression evidence

Run on cores 64-79, ports 7210/7211, oracle 7212. All at `--atomic 1` unless noted.

| battery | result |
|---|---|
| `atomic_torn.py` | `ATOMIC_TORN PASS` |
| `multi_exec.py` | `MULTI/WATCH directed battery passed` |
| `execatomic.py` | `EXEC fan-out battery passed` |
| `atomfix.py` | `atomfix: PASS` |
| `atomic_ryow.py` / `ryow.py` | `ATOMIC_RYOW PASS` / `RYOW PASS` |
| `scriptatomic.py` | `SCRIPTATOMIC: 0 FAIL (0 vacuous)` |
| `torture.py` / `limits.py` / `blocking.py` | `TORTURE PASS` / `limits: PASS` / `BLOCKING PASS` |
| `pubsub.py` / `notify.py` / `lua_scripting.py` | PASS / `notify_events_fired=1638` / `0 FAIL` |
| `snap_cut_battery.py save` + `verify_cut` | PASS, `persist-io` **normal and uring** |
| `snap_typed_roundtrip.py` | `TYPED ROUNDTRIP PASS (40/40)` |
| `snap_typed_race.py race` | `snapshot_preimages=12 PREIMAGE-FIRED PASS` (capture-time preimages still fire) |
| `aof.py populate`+`loadaof` | `AOF BYTE-EXACT PASS: 52 static replies + live monotonic PTTL` |
| `aof_rewrite_matrix.sh` | `AOF REWRITE MATRIX PASS: atomic=0/1 stages=3 corruptions=5` |
| `aof_rewrite_trigger_matrix.sh` | `AOF REWRITE TRIGGER MATRIX PASS: atomic=0/1 live-config info auto backoff restart` |
| `differ.py` vs redis 7.4, `xshard` seeds 7 & 4242, `--atomic 1` | 4276 ops, 0 diffs (both) |
| `differ.py` vs redis 7.4, `string` seeds 7 & 4242, `--atomic 1` | 4033 ops, 0 diffs (both) |
| `differ.py` same four runs at `--atomic 0` | 0 diffs (all four) |

### ASAN + UBSAN

Built with `-fsanitize=address,undefined -fno-sanitize-recover=undefined` over the whole tree, booted
on port 7213 at `--atomic 1`, cores 64-79. The server log contains **no sanitizer output of any
kind** — no `ERROR:`, no `runtime error`, no `SUMMARY:` — across:

| battery under ASAN+UBSAN | result |
|---|---|
| `snap_cut_battery.py atomic_groups mset 3` | `ATOMIC_GROUP_CUT PASS`, `cuts_armed+=3 cuts_waited+=3 groups_drained=308`, 0 torn |
| `atomic_torn.py` | `ATOMIC_TORN PASS` |
| `multi_exec.py` | `MULTI/WATCH directed battery passed` |
| `execatomic.py` | `EXEC fan-out battery passed` |
| `torture.py` | `TORTURE PASS` |

Shutdown invariants on the same instance: `stuck: live_conns=0 rob_not_quiesced=0
unsent_bytes_pending=0`, `wb: ... err=0`. Note the drain still blocked on 3 of 3 cuts under ASAN
(308 groups), so the sanitizer run exercised the new wait rather than skipping past it.

## INDICATIVE cost (loadgen shares these cores — correctness lane, not a bench verdict)

Storm at `--atomic 1` with **no snapshot running at all**, 8 s, identical shape:

| build | MSET/s |
|---|---|
| unfixed HEAD | 522,980 |
| fixed | 565,204 |

The fixed build measured faster, which is loadgen noise; the point is that there is no cost when no
cut is in progress. That is expected from where the code sits: `atomic_apply_open/close` are only
touched on the cross-shard group path, never on the GET/SET dispatch path, and the closed case is
one predicted-false acquire load of a bool already in the group's cache line.

While a cut IS in progress the barrier does stall new group admissions for the Preparing+Freeze+Mark
window — that is the intended cost of the fix and it is what the `groups_drained` counter measures
(376–1390 groups per run). A blocking SAVE already stops owner task execution for its whole
duration, so this is not a new class of stall. Cut wall time under the storm was 0.003–0.011 s
before and 0.004–0.009 s after; a real latency verdict belongs to a bench lane on server cores.

---

## Scope, deviations, and what I did not do

* `--atomic 1` is **not** the default (`src/core/config.h:213`). The MSET shape is a defect in a
  shipped non-default mode. The MULTI/EXEC shape, however, reproduces at the **default** config
  because EXEC force-admits a group regardless of the knob. Both are fixed by the same change.
* **The brief's `--atomic 0` MSET control does not read zero and should not.** Documented above with
  the live-reader numbers that show why. I replaced it with a control that genuinely must read zero.
* **The brief's literal fix deadlocks.** Documented above with the wedge transcript. The shipped fix
  waits on a different, weaker, owner-driven quantity.
* No new knobs, so nothing was added to `tomokv.conf`. Three new INFO counters only.
* Drive-by: `tests/snap_cut_battery.py`'s unused `verify_save` mode had an inverted exit
  (`sys.exit(0 if bad else 1)` — success when mismatches exist). It is not referenced by
  `tests/gate.sh`, but an inverted exit in a test file is a landmine, so it is corrected.
* Not done: a NIC-rig or server-core throughput A/B of the cut stall (reserved rig, and this is a
  correctness lane); `tests/gate.sh` was not executed end to end (it owns port 7899 / cores 0-7).

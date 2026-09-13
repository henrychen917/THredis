# NOTES-MULTIRACE — an aborted MSETNX's withdrawn value laundered into an acknowledged EXEC write

Lane `t-multirace`. Brief: `differ multi (atomic=1 seed=7)` fails about one run in three **only** in
a geometry the standing gate never boots — fused threading, `--read-local 1`, `--atomic 1`. Five
EXEC replies where the target answers with a value the oracle already overwrote.

**Outcome: shipped.** The five reply differences are the visible tip of a *store* defect: a
cross-shard `MSETNX` that answers `:0` and writes nothing can nevertheless leave its value
permanently installed, visible to every connection. Reduced to a 7-command pipelined round that
trips within 3 rounds, fixed by one dispatch-time hold in `ExLoop::execute()`, covered by a new
battery (`tests/multirace.py`, PRE **6/6 runs FAIL**, POST **6/6 PASS**) and by a second
differential matrix row in the geometry that found it.

Resources: cores 32-39,160-167; ports 8083-8086 only; every server addressed by the pid resolved
from its listening socket, killed by that pid.

---

## 1. PRE vs POST

`PRE` = shipped head `t-train9` @ e902c67d5, built in its own worktree. `POST` = this branch. Same
box, same cores, quiet, arms interleaved where the arm is a race.

| Probe | PRE | POST |
|---|---|---|
| `tests/multirace.py`, fused `--read-local 1`, 3 boots | **FAIL 3/3** | **PASS 3/3** |
| `tests/multirace.py`, 2s `--ratio 6:10`, 3 boots | **FAIL 3/3** | **PASS 3/3** |
| — leaked rounds in the armed case (atomic 1), per 200 | **7, 198, 2 / 198, 152, 1** | **0 ×6** |
| — leaked rounds in the armed case (atomic 0) | 0 ×6 | 0 ×6 |
| Minimal reproducer (3 keys), fused, 9 boots × 200 rounds | **9/9 boots leak** (1–198 rounds) | **0/12 boots** |
| Gate armed-battery list (13 batteries × 2 atomic modes) | 23 pass / 3 fail | 24 pass / 2 fail |
| — the fails | slowlog ×2 **(pre-existing)** + multirace | slowlog ×2 **(pre-existing)** |
| `differ multi` atomic=1 seed=7, armed-fused, 8 boots each | 0/8 diverged *(quiet box; see §2)* | 0/8 |
| Full differential matrix, armed-fused geometry | — | see §6 |

The `slowlog` row (`shrinking max-len trims immediately: got 11`) fails identically on PRE and
POST in both atomic modes. It is not this lane's and is not touched here.

---

## 2. The differ is a WEAK detector; the battery is the strong one

The brief's leg reproduces at roughly one run in three *on a loaded box*. On the quiet box this
lane was given, 8 PRE boots of `differ multi (atomic=1 seed=7)` in the armed-fused geometry all
passed, while the directed battery failed 6 of 6 boots on the same binary in the same window. The
`multi` suite only occasionally emits the exact `MSETNX`-aborts-then-`MULTI` shape with the keys
landing on the right owners; the battery constructs it every round. Nothing below concludes from a
differ run, and §6 states the differ counts for what they are.

`--read-local 1` is **not** part of the mechanism. It changes executor scheduling enough to move
the hit rate, which is why the brief's leg only showed it in the fused armed geometry — but the
battery and the reproducer both fail on PRE in the plain `2s --read-local 0` split geometry as
well (§1). What the read-local geometry did was make an existing, geometry-independent store
defect *visible to a test that the gate actually runs*.

---

## 3. Reduction: 4260 differ operations → one 7-command round

`scratchpad/mr/repro.py`. One connection, one pipelined write per round, three keys on three
distinct owners (`DEBUG SHARD`):

```
DEL    B v0 v1
SET    B blocker
MSETNX v0 hello v1 hello B hello    -> :0      B exists, so NOTHING may be written
MULTI ; INCRBY v0 -2 ; EXEC         -> :-2     v0 is absent, INCRBY creates it
GET    v0                           -> "-2"
```

PRE answers `-ERR value is not an integer or out of range` to the `INCRBY` and `"hello"` to the
`GET`, and a *foreign* connection reads `"hello"` too: the aborted `MSETNX`'s value is committed
and permanent. Nine PRE boots: **90–198 of 200 rounds** at two victims (first hit at round 1–3),
1–21/200 at one victim, 1–195/200 at six. Twelve POST boots (fused and 2s): **0/200 every time.**

This is the same family the differ shows. `op 1266` answers `-ERR value is not an integer` where
the oracle answers `:-2`; `1275/1287/1387` answer `hello` where the oracle answers `-2`. Those are
exactly an `INCRBY`/`GET` meeting a laundered `hello`.

---

## 4. Mechanism

**(a) `MSETNX` is the one write that publishes a candidate it may still withdraw.** Under
`--atomic 1` a multi-key `MSETNX` is a single-hop cross-shard group: every owner installs its
candidate physically *first* (`atomic_prepare_group` → `atomic_admit`), and only when every
fragment has installed does the group learn whether some *other* owner already holds one of the
keys. When one does, the group sets `aborted` and answers `:0`; its installed candidates stay at
epoch 0 forever and are unwound by `atomic_collapse()`. Between install and veto they are
physically present and undecided.

**(b) The store's RYOW overlay is keyed on the CONNECTION, not on the logical unit.** In
`FlatStore::atomic_resolve_internal()` (`src/store/flatstore_atomic.inc`):

```cpp
const bool own_committed = atomic_read_origin_conn_id_ != 0 &&
    owner.origin_conn_id == atomic_read_origin_conn_id_;
if (owner.group_epoch && epoch == 0 && !own_committed) return;   // foreign undecided: skipped
...
const bool own_private = owner.group_epoch && epoch == 0;        // MINE and undecided
if (!winner_set || (own_private ? true : (!winner_private && epoch >= winner_epoch))) { ... }
```

An epoch-zero record is invisible to every *other* connection, and outranks every committed
version for the connection that installed it. That ranking is required — a transaction has to see
its own still-private installs — but "mine" here means *this connection*, and the aborting
`MSETNX` is this connection's too.

**(c) Every other same-connection reader is held; a transaction fragment was not.** A plain
pipelined command defers behind its own connection's undecided record before it ever reaches the
resolver — `xshard_task_should_defer()` in `src/cmd/atomics_glue.inc` walks the touched keys
through `atomic_has_own_undecided()` and returns true. A scatter fragment does the same through
`atomic_group_has_own_undecided()`. But `ExLoop::execute()` (`src/core/ex_loop.h`) dispatches a
`multi_task_tagged()` task at the top of the function and returns; before this lane the only
program-order check on that path was `has_parked_predecessor()`, which sees tasks *parked* in
`atomic_deferred_`/`xshard_retries_` and cannot see a unit that is running normally and merely
undecided.

**(d) So the transaction cloned the withdrawn value and gave it a real ticket.**
`prepare_write_key()` (`src/cmd/multi.inc`) sets the read context to this connection and reads the
key it is about to write:

```cpp
shard.store().atomic_set_read_context(UINT64_MAX, state.origin_conn_id);
KvObj* visible = shard.store().find(hash, key);
... load_clone(shard, key, visible, clone) ...
shard.store().atomic_prepare_transaction(key, hash, state.origin_conn_id, &state.epoch, ...);
```

`find()` resolved through (b) and returned the `MSETNX`'s undecided `hello`. `load_clone()` copied
it into the transaction's own candidate, `atomic_prepare_transaction()` installed that clone under
the transaction's epoch word, and the EXEC then committed it with a genuine ticket. The
`MSETNX`'s own candidate was later unwound as designed — but the clone was not its candidate any
more. It was an acknowledged transaction write, and it was permanent and globally visible.

The differ needed `MULTI`, `--atomic 1` and a multi-owner key set for exactly these reasons; the
read-local geometry only supplied the scheduling that made the window wide enough to hit.

---

## 5. The fix, and why it is safe where the earlier attempt was not

One hold, at dispatch, in `ExLoop::execute()`'s tagged-MULTI branch, immediately after the
existing parked-predecessor check:

```cpp
if (__builtin_expect(shard.store().atomic_has_records(), false) &&
    shard.store().atomic_has_foreign_unit_undecided(
        t.client->id(),
        [&t](const void* epoch) { return multi_task_owns_epoch(t, epoch); })) {
    shard.stats().atomic_exec_order_holds++;
    atomic_deferred_.push_back(t);
    return true;
}
```

`FlatStore::atomic_has_foreign_unit_undecided()` walks this connection's bucket of the pending
list and answers "this owner still holds an undecided record belonging to a *different* logical
unit of this connection". "Different" is decided **by epoch word**, and a transaction publishes
through several: its plain per-key installs use `state.epoch`, and every cross-shard child command
lowered inside the same EXEC keeps its own `ScatterState` and its own. `multi_state_owns_epoch()`
accepts all of them — answering with only the transaction's word makes a transaction hold against
its own `RENAME`/`LMPOP` child and hang, which is the self-hold NOTES-MULTIRES.md §5(a) hit from
the other direction. Plain single-command pseudo-entries carry no `group_epoch` at all: they draw
their ticket in the same owner turn that installs them, so they are never undecided and are
skipped.

### Why this is not the deadlock NOTES-MULTIRES.md §5(a) forbids

That attempt put the hold **inside `prepare_write_key()`**, per key, mid-command, after the
fragment had already installed on other owners. This one is at **dispatch**, before the fragment
has installed anything on this owner. Three facts make it acyclic:

1. **Per-shard program order.** A connection's units are lowered by one IO thread and their
   fragments are posted per shard in command order, so an older unit's fragment on shard *S*
   reaches *S*'s owner before the transaction's fragment on *S*.
2. **A parked older task already holds the transaction, unconditionally.** `for_each_task_key()`
   (`src/cmd/atomics_glue.inc`) reports a tagged MULTI fragment as touching *the whole owner* on
   its own shard, so `parked_predecessor_in()` matches it against any older same-connection task
   in `atomic_deferred_`/`xshard_retries_` regardless of keys. The transaction therefore installs
   nothing on *S* while an older same-connection task waits there.
3. **Only the owner thread publishes into its own pending list.** Nothing can appear on this owner
   between the dispatch check and the fragment's installs, so the check taken once at dispatch
   holds for the whole fragment. That is why the same probe kept in `prepare_write_key()` is
   *observation only* and must read zero.

(1)+(2) mean the older unit never waits on a record the transaction installed, so the new edges
all point young→old and no cycle can close. The residual shape the argument does not close by
construction is a two-phase unit whose *second-phase* fragment lands on an owner where a younger
transaction has already installed. `tests/multirace.py`'s liveness case drives exactly that shape
— `RENAME`, `LMPOP` and `SMOVE` across owners, then a `MULTI/EXEC` on their destinations, all in
one pipelined write, 150 rounds on a 20-second socket — so a cycle of that kind fails a row in
seconds instead of hanging the gate.

### What the fix does NOT do

It does not hide the group. The `committed MSETNX` control in the battery sends the same shape
with no blocker: the `MSETNX` commits, and the following EXEC on the same connection **must** see
`hello`. Read-your-own-writes across two units of one connection is preserved by ordering the
reader after the writer's decision, not by making the writer invisible — a "fix" that suppressed
the candidate would turn that control red.

Reads are not obstructed: the hold is on the *transaction fragment dispatch* path only, is a
re-queue rather than a spin or a retry loop, and sits behind `atomic_has_records()`, which is
false on any server with no cross-shard atomic window open. Writes stay single-owner; both thread
modes are covered; no struct changed, so `Op=336 / Client=1984 / ThreadCtx=1408 / Shard=1440 /
FlatStore=944 / Rob<64>=192 / AtomicEntry=144 / Config=624` are untouched and their static asserts
compiled.

### One counter, one meaning — and why the falsifier needed a second one

Two different claims are being made, and they need two counters:

* **`atomic_exec_order_holds`** (`ExLoop::execute()`, dispatch) — *the hold fired*. Non-zero by
  design in every armed run; `tests/multirace.py` and `tests/multires.py` fail a run in which it
  never advances, because such a run never entered the window.
* **`atomic_exec_order_late`** (`prepare_write_key()`, install) — *the hold fired **early
  enough***. Must read **zero**, in both atomic modes, from boot.

The install-time site asks the identical question one step later. Only the owner thread publishes
into its own pending list, so the park should foreclose it completely — and that foreclosure is
the whole acyclicity argument above. Keeping the probe is what makes that argument falsifiable.

The refinement this lane closes: those two sites were briefly **merged onto one counter**, which
made the falsifier inert. `holds` is non-zero *by design* in exactly the runs that open the window,
so a summed counter can never be checked against zero — the safety argument could have broken with
no test going red, and the batteries' non-vacuity assertion would have been satisfied by park
increments regardless. Split, each number answers exactly one question and both are asserted:
`holds > 0` (armed) and `late == 0` (always).

`atomic_exec_order_late` is a namespace-scope `std::atomic<uint64_t>` in `multi.inc`, not a
`Shard::Stats` field, because `sizeof(Shard)` is locked at 1440 and `Stats` is a by-value member —
a 20th counter would have grown it. Nothing is lost by the placement: the increment sits on a path
that is never taken, so there is no per-shard line to contend for. There is precedent for the
shape (`scripting.cc`, `slowlog.cc`, `functions.cc` all keep INFO counters this way).

The `prepare_write_key()` probe also previously excluded only `state.epoch`, so it counted a
transaction meeting **its own** cross-shard child. It now uses `multi_state_owns_epoch()`, the same
predicate as the park — without that, `late` would report a violation on every EXEC containing a
`RENAME`/`LMPOP` and the zero assertion could never hold.
`atomic_has_own_undecided()`'s `ignore_epoch` parameter had no other caller and is removed; one
epoch pointer cannot name a transaction's several words, which is the whole reason the predicate
exists.

**Effect on `tests/multires.py`.** That battery locks the same hazard window and used this counter
as its non-vacuity discriminator. The fix moves where the window is closed — from install to
dispatch — so its counter now advances at the park; its docstring said "and where an EXEC write
installs against one" and no longer does, because that arm is gone. Its three negative controls
still read zero: a foreign connection is excluded by `origin_conn_id`, a single-key `DEL` carries
no `group_epoch`, and "no predecessor" has nothing undecided. It asserts `late == 0` as well.

---

## 6. Containment

| Gate | Result |
|---|---|
| Release build `make -j12`, `-Wall -Wextra` | **0 warnings, 0 errors**; footprint static asserts compiled |
| `make unit` (config parser, flip controller model, read-local ring) | pass |
| `tests/multirace.py` — POST, fused `--read-local 1` × 3, 2s × 3 | **6/6 PASS** (PRE: 6/6 FAIL) |
| Minimal reproducer, POST, 12 boots × 200 rounds | **0 leaks** (PRE: 9/9 boots leak) |
| Gate armed battery list × 2 atomic modes (lbsignals, slowlog, atomfix, scriptatomic, execatomic, execiso, execfix, multires, multirace, session_monotonic, xacct, xmove, xscript) | 24/26, the 2 being the pre-existing `slowlog` row on PRE and POST alike |
| `batteries.sh 1s` — s6, ryow, atomic_hazards, multi_exec, blocking, blockmulti, xscript, expwide, session_monotonic, bplus | see §6a |
| `batteries.sh 2s` — the same plus flip, flip_under_load | see §6a |
| Differential matrix, canonical split geometry | see §6a |
| Differential matrix, armed-fused geometry (new gate row) | see §6a |
| `tests/gate.sh quick` | see §6a |

### 6a. (filled in from the run logs)

---

## 7. Closing the gate gap

The defect sat on shipped code because **the gate's differential matrix boots the target one way
only**: split threading at `--ratio`, with the read-local lane disarmed. That boot cannot reach
the fused read-local parse and resolve arms at all, and it is also the shape in which this
particular race is rarest. Two changes:

* `tests/differ_gate.sh` takes `GATE_DIFFER_GEOMETRY` (`split`, the default and previous
  behaviour, or `armed-fused` = `--thread-mode fused --read-local 1`). The armed run carries its
  own **non-vacuity row**: it reads `read_local_hits` out of the live target before stopping it
  and fails the leg when it is zero or missing, so an accidentally-disarmed boot is red rather
  than quietly green. It skips the `sort` suite, which asserts the 6:2 split out of INFO and can
  only self-abort under a fused boot — stated in the output, and not counted as a pass.
* `tests/gate.sh` runs that second matrix as one full-tier row beside the canonical one, and adds
  `multirace` to the armed debug-surface battery loop.

**EXPECT accounting.** This lane contributes **+2 quick** (the `multirace` row under each atomic
mode) and **+3 full** (those two plus the armed-fused differ row, which is full-tier only). From
this branch's base of 245/258 that is **247/261**, and the quick tier was measured printing
`PROGRAM-STATE ledger (247/247 checks)`. A sibling lane (`t-replycode`) contributes **+2 full**
from the same base (245/260). Whichever lands second must resolve that hunk by adding both deltas
to the shared base — **247/263** — not by taking one side; the comment above `EXPECT_QUICK` says
so in the file.

`tests/multirace.py` drives **both atomic modes itself** through `CONFIG SET`, restoring the
booted value at exit. That is not decoration: the armed boot it joins is shared with
`scriptatomic`/`execatomic`/`execiso`/`execfix`, every one of which flips `atomic` and leaves it
flipped, and the gate's `multirace (atomic 0)` row was measured reporting `atomic=1`. Driving the
mode from the battery is what makes both arms' claims true rather than merely labelled — and the
atomic-0 arm now **asserts** `atomic_exec_order_holds` stays at exactly zero (a two-hop `MSETNX`
decides before it installs, so no unit of this shape can be undecided on an owner), rather than
merely tolerating whatever it finds.

---

## 8. Files changed

| File | Change |
|---|---|
| `src/core/ex_loop.h` | the dispatch-time hold in `execute()`'s tagged-MULTI branch |
| `src/store/flatstore_atomic.inc` | `atomic_has_foreign_unit_undecided()`; `ignore_epoch` removed from `atomic_has_own_undecided()` |
| `src/cmd/multi.inc` | `multi_state_owns_epoch()` / `multi_task_owns_epoch()`; `prepare_write_key()`'s probe now asks the same question as the park and reports into its own counter, `g_exec_order_late` |
| `src/cmd/multi.h` | `multi_task_owns_epoch()` and `multi_exec_order_late()` declarations |
| `src/cmd/t_server.cc` | INFO STATS emits `atomic_exec_order_late` |
| `src/core/shard.h` | comment only: `atomic_exec_order_holds` has ONE site again, and why the install-time probe does not share it |
| `tests/multirace.py` | new battery: abort case, commit (RYOW) control, foreign-connection control, liveness case, both atomic modes, non-vacuity both ways |
| `tests/gate.sh` | `multirace` in the armed loop; the armed-fused differ row; EXPECT arithmetic |
| `tests/differ_gate.sh` | `GATE_DIFFER_GEOMETRY`; the `read_local_hits` non-vacuity row; the stated `sort` skip |
| `tests/multires.py` | docstring: the counter has one site and the window now closes at dispatch; asserts `atomic_exec_order_late == 0` |

**Correction to NOTES-MULTIRES.md §5(a).** That file says "hold the EXEC write behind its own
older group … do not retry". The prohibition is correct *for the place it was tried* — inside
`prepare_write_key()`, per key, after the fragment has installed elsewhere. The hold shipped here
is at dispatch, before any install on this owner, with the own-unit test done by epoch word
including the lowered children; §5 of this file is the argument for why that placement is
acyclic. §5(a) has been annotated to point here.

---

## 9. Resumption state (2026-09-05, after the 19:20 usage-limit kill)

**Resources CHANGED this session.** The allocation in the header (cores 32-39,160-167; ports
8083-8086) is the *previous* one and is dead. Current: physical **48-51** + SMT siblings
**176-179**, ports **8240-8249** only. Every harness script under `scratchpad/mr/` was re-pointed:

| Role | Cores | Why |
|---|---|---|
| server (target under test) | `48,49,176,177` | `SRVC` in every script |
| load generator / oracle | `50,51,178,179` | `GENC`; never the server's physical cores |

Ports: differ target `8240`, differ oracle `8241`, batteries `8242`, session_monotonic `8243`,
target-only repro `8244`, PRE batteries `8245`, perf cells `8246`.

`tests/differ_gate.sh` pins differ.py to `TARGET_CORES` by its own design; that is a shipped gate
script and is not edited for this lane's convenience, so in the differ rows the client shares the
target's cores. Those rows take no measurement, so nothing is read off them but pass/fail. The
perf cells (§10) keep server and driver on disjoint physical cores.

### Done
* §5's counter split — `atomic_exec_order_late` is now its own namespace-scope counter, INFO-
  exposed, asserted `== 0` by `tests/multirace.py` (both modes) and `tests/multires.py`.
  Commit `286e72265`. This is the "counter-honesty issue in prepare_write_key" the lane was on.

### Pending (blocked on the box: `scratchpad/quiet.done` absent = owner measuring)
1. Rebuild POST (`build/tomokv` is from 14:45, older than the counter split).
2. Differ matrix, both geometries: `GATE_DIFFER_GEOMETRY=split` and `=armed-fused`.
3. Batteries `1s` and `2s` (`mr/batteries.sh`), armed battery list (`mr/armed.sh`).
4. Directed repro still red on PRE / green on POST (`mr/tonly.sh` + `mr/repro.py`).
5. PRE/POST instr/op table (§10).

### 10. The perf question this fix has to answer

The hold is at *dispatch*, inside `ExLoop::execute()`'s `multi_task_tagged()` branch, so:

* **plain SET/MSET never reach it** — the branch is not on the ordinary write path at all, and the
  main-command zero-regression claim should therefore be a *null*, not a small loss;
* **MULTI/EXEC fragments pay one `atomic_has_records()` load** when no cross-shard atomic window is
  open (predictable-false branch), and pay a walk of this connection's pending bucket when one is;
* nothing was added to `prepare_write_key()` — its probe existed on PRE and merely changed which
  predicate it calls and which counter it raises.

`mr/pcell.sh` measures it with a FIXED op count (matched rate by construction), server and driver
on disjoint physical cores, `perf stat -p <server>` over the measured phase only:

| cell | shape | isolates |
|---|---|---|
| `set` | `drv2`, pipelined SET | the main-command null |
| `multi` | `drvm plain`: `MULTI;SET;SET;EXEC` | the added branch, window shut |
| `multiarmed` | `drvm armed`: cross-shard `MSET` then the same transaction | the added branch, window OPEN — the walk |

Both geometries (`1s` = fused + `--read-local 1`, `2s` = `--ratio 6:10`), `--atomic 1` throughout.
Every cell prints wall time and rate alongside instr/op: a busy-spinning server inflates instr/op
when the arms' durations diverge, so a cell whose two arms did not take comparable wall time is not
a matched-rate comparison and must not be read as one.

---

## 11. Re-verification on the 2026-09-05 allocation (4 physical cores)

### 11a. A capacity constraint that first read as a non-reproduction

`--ratio 6:10` asks for **16 threads**; the server on this session's allocation has 4 (`48,49` +
SMT) and refuses to boot: `--ratio: 16 threads but only 4 allowed cpus`. The first 2s attempt
therefore ran with the *default* ratio, whose shutdown report shows two executors sharing the work
**1808 ops to 8** — one executor effectively idle. The reproducer needs the blocker's owner and a
victim's owner on *different* executor threads for the install-then-veto interleaving to exist at
all, so that boot could not open the window and reported `0/200`.

That is a property of the harness, not of the store, and it is exactly the shape
`tomokv-nonreproduction-needs-geometry` warns about: **naming the geometry is what separates "the
defect is absent" from "this boot could not host it".** Re-run with four executors it reproduces
immediately.

Correctness runs on this allocation therefore use: server `48,49,50,176,177,178` (6 threads),
load generator `51,179` — still never the same physical core — and `--ratio 2:4` for the 2s arm.

### 11b. Directed reproduction, 200 rounds per boot, 2 victims

| Arm | Geometry | Boots | Rounds leaked |
|---|---|---|---|
| **PRE** `e902c67d5` | fused `--read-local 1` | 5 | **96, 0, 200, 67, 0** — 3/5 boots leak |
| **PRE** `e902c67d5` | 2s `--ratio 2:4` | 5 | **0, 196, 8, 154, 64** — 4/5 boots leak |
| **POST** | fused `--read-local 1` | 5 | **0 ×5** |
| **POST** | 2s `--ratio 2:4` | 5 | **0 ×5** |

PRE leaks in **7 of 10** boots and reaches 200/200; POST is clean in **10 of 10**. The boots where
PRE reads 0 are the same per-boot owner-assignment lottery §2 describes — `DEBUG SHARD` picks one
key per owner, and whether the blocker's owner and the victims' owners land on distinct executor
threads is decided at boot. This is why the verdict is read from a *set* of boots and never from
one, and why POST's ten consecutive zeros are the load-bearing half of the comparison.

`--read-local 1` remains not part of the mechanism (§2): both geometries leak on PRE.

## 12. Containment results, 2026-09-05 allocation

All rows on the POST binary built at 19:47 from `0a35f3aa1` (verified: no source file newer than
the binary; INFO reports both counters).

| Row | Result |
|---|---|
| Release build `make -j8`, `-Wall -Wextra` | **0 errors, 0 warnings** |
| `tests/multirace.py` POST, fused `--read-local 1` | **PASS** (12 ok / 0 FAIL) |
| `tests/multirace.py` POST, 2s `--ratio 2:4` | **PASS** (12 ok / 0 FAIL) |
| `tests/multirace.py` PRE, both geometries | **FAIL** — `leaked=108/200`, `bad_exec=108` |
| Armed debug-surface list, 13 batteries × 2 atomic modes | **26 pass / 0 fail** |
| `batteries.sh 1s` (s6, ryow, atomic_hazards, multi_exec, blocking, blockmulti, xscript, expwide, session_monotonic, bplus) | 9 pass / 1 fail — `expwide`, **identical on PRE** |
| `batteries.sh 2s` (the same plus flip, flip_under_load) | **11 pass / 0 fail** |
| Differential matrix, canonical split geometry | **164 pass / 4 fail**, 5m59s — all 4 are `sort` (below) |
| Differential matrix, armed-fused geometry | pending: the box went busy mid-run |

**The `slowlog` rows now pass.** §1 recorded them as a pre-existing PRE-and-POST failure; on this
allocation all 26 armed rows are green in both atomic modes.

**`expwide` fails identically on PRE and POST** (`S1 MGET: the hook really widened the fan-out
(elapsed=0.000s)`), in the 1s geometry only — it passes 2s on POST. A timing probe that measures
zero elapsed on a 6-thread fused boot; not this lane's, and not touched here.

**The four `sort` failures are the suite refusing the geometry, not a divergence.** `sort` asserts
its thread split out of INFO and aborts on anything else: `sort differ requires --shards 16 --ratio
6:2, got {lb_io_threads: 2, lb_ex_threads: 4}`. `--ratio 6:2` is 8 threads and the server here has
6, because the load generator must not share a physical core. It is the same constraint the
armed-fused row already states and skips for; the difference is that in the split row it arrives as
a FAIL rather than a SKIP, since differ_gate.sh only special-cases it under `armed-fused`. The
other **164 legs pass**, and `sort` is re-run separately at its required ratio rather than being
waved through.

---

## 13. The armed-fused differential matrix — the leg the gate never covered

This is the row the lane exists for. The defect shipped because the gate's differential matrix
boots the target **one way only** (§7), and the geometry it cannot reach is the one the brief's
failure was seen in. Running the whole matrix in that geometry is therefore not a formality: it is
the only run that covers the surface the escape came through.

| Matrix | Geometry | Legs | Result |
|---|---|---|---|
| canonical split | `--ratio 2:4`, read-local disarmed | 164 (41 suites × atomic{0,1} × seed{7,19}) | **164 pass / 4 fail**, 5m59s — all four are `sort` refusing the ratio (below) |
| `sort` re-run at its required ratio | `--shards 16 --ratio 6:2` | 4 | **4 pass / 0 fail** — `0 diffs` on every leg |
| **armed-fused (new gate row)** | `--thread-mode fused --read-local 1` | 164 + 2 non-vacuity | **166 pass / 0 fail**, 6m00s |
| — read-local non-vacuity, atomic 0 | | | `hits=909 fallbacks=260` |
| — read-local non-vacuity, atomic 1 | | | `hits=984 fallbacks=1225` |
| — `sort` under fused | | 4 | **SKIP**, stated in the output, not counted as a pass |

**The two non-vacuity rows are the point.** An armed-fused row that boots disarmed is a row that
re-runs the canonical matrix under a different name, and it would have been green on the shipped
binary that carried this defect. `differ_gate.sh` reads `read_local_hits` out of the live target
before stopping it and fails the leg when it is zero or missing; both modes report four-figure hit
counts, so the read-local lane demonstrably carried traffic in the run that returned the passes.

**The four split-matrix `sort` failures are the suite refusing the geometry, not a divergence.**
`tests/differ.py`'s sort generator asserts its thread split out of INFO and aborts on anything else
(`sort differ requires --shards 16 --ratio 6:2, got {lb_io_threads: 2, lb_ex_threads: 4}`). `6:2`
is 8 threads; the split matrix here runs a 6-thread server because the load generator must keep its
own physical core. `scratchpad/mr/sortleg.sh` re-runs exactly those four legs at `--ratio 6:2` with
the target on all four physical cores and the differ client sharing them — which is what
`differ_gate.sh` itself does with `TARGET_CORES` in every leg, and costs nothing here because no
measurement is read off a differ leg, only pass/fail. All four pass with `0 diffs`. The armed-fused
matrix reaches the same constraint one step earlier and reports it as a **SKIP** with the reason
printed, which is the honest disposition: a suite that cannot be hosted is neither a pass nor a
failure.

### Batteries, both thread modes

| Battery set | Result |
|---|---|
| `batteries.sh 1s` (s6, ryow, atomic_hazards, multi_exec, blocking, blockmulti, xscript, expwide, session_monotonic, bplus) | 9 pass / 1 fail — `expwide`, **identical on PRE** |
| `batteries.sh 2s` (the same plus flip, flip_under_load) | **11 pass / 0 fail** |
| armed debug-surface list, 13 batteries × 2 atomic modes | **26 pass / 0 fail** |

`expwide` fails on PRE and POST alike in the 1s geometry (`S1 MGET: the hook really widened the
fan-out (elapsed=0.000s)` — a timing probe reading zero elapsed on a fused boot) and passes 2s on
POST. It is one of the known fused+armed gate holes that fail on mainline too and belongs to the
lane that owns that hole; it is recorded here and not chased.

---

## 14. What the fix costs, measured against a same-binary null

A correctness fix that quietly taxes the hot path is not a correctness fix, so this is measured
rather than argued -- but a sub-percent number on this box means nothing without a noise floor, so
**the null comes first**. `nullA` and `nullB` are the SAME POST binary under two labels, run in the
same alternation and at the same separation as the pre/post pair; a pre->post difference no larger
than the nullA->nullB difference, or than the arms' own rep-to-rep spread, is noise and is reported
as noise.

Fixed op count on every arm (matched rate by construction). Server on physical 48,49 (+SMT
176,177); driver on 50,51 (+SMT 178,179), never sharing a physical core. `perf stat -p <server>`
over the measured phase only; the idle instruction and cycle rates are measured at no load and
subtracted, and both raw and adjusted are kept. Medians of **3 reps**, all four arms interleaved
within each rep. **`cycles/op` carries the verdict; `instr/op` and `IPC` are its two factors --
cyc/op = instr/op / IPC -- and are printed so a move can be attributed rather than merely noticed.**
Harness `scratchpad/mr/pcell2.sh` + `perfrun.sh` + `ptable.py`; raw rows `run/perfmatrix.csv`.

### 14a. The table

`1s` = fused + `--read-local 1`; `2s` = `--thread-mode 2s --ratio 2:2`; `--atomic 1` throughout.
Per-WRITE columns (a transaction round is two SET writes).

| geom | cell | metric | null A->B | PRE | POST | POST-PRE | verdict |
|---|---|---|---|---|---|---|---|
| 1s | **set** | cyc/op | +0.33% | 4464.8 | 4432.8 | **-0.72%** | null |
| | | instr/op | +0.02% | 6400.4 | 6413.0 | +0.20% | null |
| | | IPC | +0.95% | 1.434 | 1.447 | +0.91% | null |
| | | writes/s | +1.24% | 996,531 | 1,010,414 | +1.39% | null |
| 1s | **multi** (window SHUT) | cyc/op | +0.17% | 40368 | 41250 | +2.18% | **indistinguishable** |
| | | instr/op | +0.17% | 58153 | 58459 | +0.52% | null |
| | | IPC | +0.42% | 1.442 | 1.428 | -0.97% | null |
| | | writes/s | -0.41% | 114,655 | 112,498 | -1.88% | null |
| 1s | **multiarmed** (window OPEN) | cyc/op | -0.01% | 54407 | 56170 | **+3.24%** | MOVED |
| | | instr/op | +0.11% | 72340 | 74430 | **+2.89%** | MOVED |
| | | IPC | +0.15% | 1.329 | 1.323 | -0.45% | null |
| | | writes/s | +0.47% | 89,102 | 88,032 | -1.20% | null |
| 2s | **set** | cyc/op | -0.14% | 6560.5 | 6519.7 | **-0.62%** | null |
| | | instr/op | -0.26% | 11409 | 11368 | -0.35% | null |
| | | IPC | -0.06% | 1.739 | 1.744 | +0.29% | null |
| | | writes/s | +0.41% | 1,244,952 | 1,252,174 | +0.58% | null |
| 2s | **multi** (window SHUT) | cyc/op | -0.50% | 62780 | 62522 | **-0.41%** | null |
| | | instr/op | -0.29% | 102,444 | 102,641 | +0.19% | null |
| | | IPC | +0.06% | 1.635 | 1.642 | +0.43% | null |
| | | writes/s | +0.49% | 145,228 | 145,909 | +0.47% | null |
| 2s | **multiarmed** (window OPEN) | cyc/op | -0.08% | 76636 | 77353 | **+0.93%** | MOVED |
| | | instr/op | +0.54% | 126,336 | 127,125 | +0.62% | null |
| | | IPC | -0.55% | 1.647 | 1.643 | -0.24% | null |
| | | writes/s | +0.09% | 109,260 | 108,459 | -0.73% | MOVED |

**The counters are what make each cell mean what it claims**, per 400,000 rounds:

| geom | cell | PRE holds | POST holds (3 reps) | POST late |
|---|---|---|---|---|
| 1s | set | 0 | **0** | 0 |
| 1s | multi | 0 | **0** | 0 |
| 1s | multiarmed | 0 | **229,114 / 242,475 / 244,084** | 0 |
| 2s | set | 0 | **0** | 0 |
| 2s | multi | 0 | **0** | 0 |
| 2s | multiarmed | 0 | **94,158 / 96,689 / 97,709** | 0 |

### 14b. Reading it

**The main-command claim is structural before it is statistical.** `holds` is zero on every `set`
and every window-shut `multi` arm because the hold lives inside `ExLoop::execute()`'s
`if (multi_task_tagged(t))` -> `if (t.client)` branch, which an ordinary SET task never enters.
There is no code on the plain write path to be slower, and the measurement agrees in both
geometries -- POST is nominally *faster* (-0.72% and -0.62% cyc/op) inside null bands of 0.33% and
0.14%.

**The `1s multi` cell is indistinguishable, and saying so is the finding.** Its median cyc/op is
+2.18%, but the three POST reps span 2.9% and the two null arms -- the same binary -- span 3.6%
(40044 to 41501). The difference is inside the arms' own scatter. instr/op, which is the stable
column here, is a null: +0.52% against a +0.17% null. An earlier 2-rep pass read this cell as
"moved"; three reps and a null arm say it is not. That is exactly the failure mode a null exists to
catch, and it is why no verdict in this lane is read from two points.

**The cost is one number, and both geometries agree on it.** The park is the only thing the fix
adds, so its price should be *cycles per park*, not a percentage of a cell:

| geom | parks / round | Δ cycles / round | **cycles per park** | Δ instr / round | **instr per park** |
|---|---|---|---|---|---|
| 1s (fused, armed) | 0.606 | 3,526 | **~5,820** | 4,181 | **~6,900** |
| 2s (split 2:2) | 0.242 | 1,434 | **~5,930** | 1,578 | **~6,520** |

Two geometries that differ by 2.5x in park rate, by 40% in IPC and by every scheduling detail agree
within 2% on the cycles a single park costs. That is a mechanism, not a curve fit: walk this
connection's bucket of the pending list, push the task onto `atomic_deferred_`, dispatch it a second
time. IPC is flat in both armed cells (-0.45% and -0.24% against nulls of +0.15% and -0.55%), so the
cost is *instructions* and not a stall the fix introduced -- ~6,700 of them, at an IPC near 1.15,
which is what pointer-chasing a pending list and re-queueing should look like.

**CORRECTION to an earlier reading in this lane.** A 3-rep hand run made before these counters were
wired reported every 2s cell as a flat null and I inferred that the split roles decide a pipelined
`MSET` before the following `EXEC` dispatches, so the park never fires in 2s. **That is wrong.** It
fires 94k-98k times per 400k rounds -- 40% of the fused rate, not zero -- and the cost is there too,
at +0.93% cyc/op, which the earlier run's ±0.5% scatter hid. The mechanism was invented to explain a
null that was really a small signal under noise. The counters are what corrected it: a perf cell
without a non-vacuity reading cannot tell "no cost" from "no exposure", and the two demand opposite
conclusions.

**What the fix costs in practice.** ~5,900 cycles each time a transaction has to wait for an older
same-connection cross-shard group to decide, and exactly nothing otherwise. The armed cells are
pathological by construction -- every single round opens a cross-shard `MSET` and then runs a
transaction on the same connection, so 24-61% of rounds park -- which makes +3.24% an upper bound
reached only when the hazard is continuous. The price of not paying it is a store that commits
withdrawn values (§3, §4).

`atomic_exec_order_late` reads **0 on every POST arm in both geometries under load**, so §5's
acyclicity argument keeps its falsifier at 88k-1.25M writes/s and not merely inside a battery.

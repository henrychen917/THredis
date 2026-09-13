# NOTES-MULTIRES — a cross-shard group's commit ticket could overwrite an EXEC's acknowledged write

Lane `t-multires`. Brief: 7 reply differences in the `multi` differ suite at `--atomic 1`, seed 7.

**Outcome: shipped.** The 7 differences are the visible tip of an acknowledged-write-loss defect.
Root cause found, reduced to a one-command reproducer, fixed in `atomic_collapse()` and
`atomic_resolve_internal()`, and bounded with the full atomic battery, the differ matrix across 4
seeds in both modes, a liveness matrix, and an interleaved A/B. A new battery `tests/multires.py`
fails 8/8 before the fix and passes 10/10 after.

Resources used: target cores 16-23, oracle + load generator cores 24-31, ports 7450/7451 only.
Every server was addressed by the pid resolved from its listening socket.

---

## 1. PRE vs POST

All rows measured on the same build with only the fix hunks differing (`tomokv-prefix` =
instrumentation but no repair; `tomokv-ship` = shipped), target pinned 16-23, `--shards 16
--ratio 6:2 --atomic 1`.

| Probe | PRE | POST |
|---|---|---|
| `tests/multires.py` (whole battery, 10 boots) | **FAIL 8/8 runs** | **PASS 10/10 runs** |
| Acknowledged EXEC writes lost — `family.py`, group→txn cells | **7 / 120 rounds** | **0 / 120** |
| Acknowledged EXEC writes lost — `sweep.py`, 28 cells (16-core pin) | **246 / 1120** | **0 / 1120** |
| Stale reads after an acknowledged EXEC write — `readwindow.py` | **22 / 400 rounds** | **0 / 400** |
| `differ multi` atomic=1 seed=7 | **1 / 20 runs diverged** (7 diffs) | **0 / 40 runs** |
| Differ matrix: 8 suites × 4 seeds × both modes | — | **64 / 64 pass** |
| Atomic battery (20 batteries × both modes) | — | **40 / 40 pass** |
| Liveness matrix, no WATCH, 1→16 conns | clean | **clean, rates match base** |
| Atomic-path A/B (4-key cross-shard MSET/MGET p32) | 1 506 409 ops/s | 1 487 010 ops/s (**−1.29 %**) |
| Plain GET/SET p32 A/B | 4 308 356 ops/s | 4 281 115 ops/s (**−0.63 %**, code path untouched) |

Both A/B rows are INDICATIVE loopback numbers, interleaved arm-by-arm with a fresh server per
leg. The −0.63 % plain row is noise: the changed code sits behind
`atomic_resolve_internal()`'s existing `!atomic_pending_ || !atomic_pending_->live` early return,
so a GET/SET workload with no cross-shard window open never reaches it; base spread across reps
was 0.6 % and fix spread 0.8 %, wider than the gap. The −1.29 % atomic row is real and is bought
deliberately — see §6.

---

## 2. Reproduction (my own run, and the correction to the brief)

The brief presented the divergence as seed-deterministic. It is not — it is a **race**. First six
back-to-back runs, identical binary, identical seed, one server, no reboot between them:

```
DIFFER multi: 4260 ops, 4 diffs -> FAIL
DIFFER multi: 4260 ops, 4 diffs -> FAIL
DIFFER multi: 4260 ops, 4 diffs -> FAIL
DIFFER multi: 4260 ops, 0 diffs -> PASS
DIFFER multi: 4260 ops, 0 diffs -> PASS
DIFFER multi: 4260 ops, 0 diffs -> PASS
```

Measured hit rate on a fresh boot: **1 of 20 runs**. The diff *count* also moves with the boot (7
in the brief's run, 4 in mine) because the target randomizes its hash seed per boot and the shard
placement of the affected keys changes with it. Nothing below concludes from a single clean run.

The differ is a weak detector of this defect (5 %). Everything else in this file uses the
amplified probes in §3, which fire at 6–20 %.

---

## 3. Reduction: 4260 operations → one command

The four diffs in a failing run all traced to **one lost write**. Aligning them against the
generated stream:

* op 454 `DEL mx:06… mx:15…` — a bare multi-key DEL, so a cross-shard atomic group
* ops 455-457 `MULTI` / `SETNX mx:06… hello` / `EXEC` — **`EXEC` answered `*1 :1`**, i.e. the
  server told the client SETNX had set the key
* op 467 `MGET …` returns nil for `mx:06…`; op 480 `STRLEN` inside EXEC returns 0 not 5; op 495
  `MGET` nil again; op 509 `DEL` counts 2 not 3

So the reply was right and the store was wrong: **an acknowledged write vanished.** That is a
different and more serious defect than "7 reply differences", and it is not the read-ranking
family the brief pointed at.

Delta-debugging against an oracle-free predicate — *EXEC said `:1`, then the key is gone* — took
the 455-op prefix down to **one operation**:

```
FLUSHALL
DEL   <k1> <k2>                 # multi-key => one cross-shard atomic group; both keys absent
MULTI ; SETNX <k1> hello ; EXEC # -> *1 :1   the server says it set the key
GET   <k1>                      # -> $-1     the value is gone
```

everything after `FLUSHALL` in one pipelined write. `ddmin` reached this in 9 steps and confirmed
it 3/3. Note the *reduction succeeded* here, contrary to the usual expectation for a race,
because the predicate is a store invariant rather than a byte-diff and can be retried in a loop.

Structure of the trigger, each row 60 rounds (`family.py`, `variants.py`):

| shape | lost / acknowledged |
|---|---|
| `DEL k1 k2` (group) → `MULTI SETNX k1 EXEC` | **15/60** |
| `MSET k1 a k2 b` → `MULTI SET k1 EXEC` | **19/60** — settles to `"a"`, the *group's* value |
| `MSETNX`, `UNLINK` variants | **20/60**, **15/60** |
| `DEL k1` (single key, not a group) | 0/60 |
| no predecessor at all | 0/60 |
| transaction on a **second connection** | 0/60 |
| older transaction → younger anything | 0/60 |
| older group → younger *plain* write or *group* | 0/60 |

The whole family is exactly one cell: **an older cross-shard atomic GROUP followed by a younger
MULTI/EXEC on the SAME connection.** The `MSET` row is the decisive one — the settled value is the
group's `"a"`, so the group's value was written *after* the transaction's.

---

## 4. Mechanism

A cross-shard write group draws its commit ticket only when its **last** fragment has installed
(`Server::atomic_commit_group`). A transaction the same connection sends afterwards can finish
its own fragments first and reach the ticket counter first. The two units of one connection then
carry commit tickets in the opposite order to the order the client sent them.

Install order, by contrast, is correct: owner tasks are posted to a shard in arrival order and
drained in order, so on the owning shard the group's record is already in the pending list when
the transaction installs. That is directly witnessed — the new `atomic_exec_order_holds` counter
increments precisely when a transaction installs while an older same-connection group is still
undecided on that owner, and a per-round 2×2 over 200 rounds gave:

```
hold fired & write LOST : 34
hold fired & write kept : 25
NO hold   & write LOST  :  0     <-- the hold is NECESSARY
NO hold   & write kept  : 141
```

Both rankers then used the raw ticket:

* `atomic_collapse()` resolves an overlapping prefix by committed-ticket argmax. Walking
  `[group, transaction]` with `t_group > t_txn`, the group wins and
  `atomic_exchange_physical()` splices the group's value back over the transaction's — the lost
  write.
* `atomic_resolve_internal()` ranks the same chain the same way, so until the owner's post-batch
  cleanup collapsed the pair, reads *also* answered from the group's generation — the stale read.

**Fix:** within one connection, program order is the truth, and the list order the walk already
visits *is* that program order. Repairing the effective epoch to be monotone per connection
removes the inversion and changes nothing across connections, where the ticket remains the only
ordering. Two sites, same three lines:

* `src/store/flatstore_atomic.inc` — `atomic_collapse()`'s `consider()`, with the carry state in
  `AtomicCollapseKey` (`src/store/atomic_mvcc.h`).
* `src/store/flatstore_atomic.inc` — `atomic_resolve_internal()`'s `consider()`, applied *before*
  the visibility tests so a snapshot excluding a connection's earlier record also excludes its
  later one.

The collapse fast paths (`direct`, non-overlapping) are untouched, so the monotonic common case
pays nothing.

---

## 5. Two fixes that were wrong, and why (do not retry them)

Both were built, measured, and rejected. They are recorded because each looks obviously right.

**(a) Hold the EXEC write behind its own older group — *in `prepare_write_key()`*.**
(SUPERSEDED IN PART: a hold at the other end of the path, in `ExLoop::execute()`'s tagged-
MULTI dispatch *before* the fragment has installed anything on that owner, is acyclic and
shipped in `t-multirace`; see NOTES-MULTIRACE.md §5. What follows remains true of the
mid-command placement, and of any own-unit test that names only one epoch word.) Every other write path already takes this
hold: `ex_loop.h::execute()` runs `has_parked_predecessor()` / `xshard_task_should_defer()` ahead
of ordinary and scatter tasks, but it dispatches a `multi_task_tagged()` task at the top of the
function and returns, so a transaction passes **no** such check. Adding the check in
`prepare_write_key()` did remove every loss — and **deadlocked at one connection with no WATCH**
(0 rounds completed, vs 300 for base). Two independent reasons:

1. A cross-shard child command *inside* the same EXEC (`DEL k1 k2` in a MULTI) installs through
   the scatter path and so carries a **non-null** `group` pointer, while the transaction's plain
   installs carry a null one. `atomic_has_own_undecided()` discriminates on the group pointer, so
   the transaction held against itself. Excluding by the transaction's **epoch word** instead
   (both shapes publish through `&state.epoch`) fixed that much.
2. It still deadlocked, because the gate is not order-aware. A transaction installs on some shards
   and holds on others; the older group's fragment then defers behind the transaction's *partial*
   installs via `atomic_group_has_own_undecided()`, which does not compare op ids. Today that is
   harmless only because the transaction never waits. Making it wait closes the cycle. The
   deadlock-freedom argument for the existing gate rests on the `atomic_hazard` bit — the oldest
   group never waits — and a transaction has no equivalent.

**(b) Ship the collapse repair alone and drop the resolver repair.** The resolver hunk costs ~2 %
and appeared to buy nothing: the stale-read probe scored 0/400 with and without it. That
measurement was taken with the target on **16** cores, where the owner's post-batch cleanup always
beat the read to the pair. Re-measured with the target correctly pinned to **8** cores, the read
wins often enough that collapse-alone failed `tests/multires.py` in **3 of 8** runs while
collapse+resolver passes **10 of 10**. The resolver hunk is load-bearing; the earlier reading was
a pinning artifact. (Thanks to the coordinator's pinning correction, which is what exposed it.)

---

## 6. Containment

| Gate | Result |
|---|---|
| `tests/multires.py`, both modes, 10 boots | PASS 10/10 (`atomic 0` and `atomic 1`) |
| Atomic/EXEC/xshard battery, both modes — torture, ryow, atomic_torn, atomic_ryow, atomfix, execatomic, execiso, execfix, multi_exec, concur, xacct, xscript, scriptatomic, writer_atomic, session_monotonic, s6, xmove, zsetops, lcs, tracking | 40/40 |
| Differ: multi, xshard, string, list, set, zset, hash, scan × seeds 7/19/23/31 × atomic 0/1 | 64/64 |
| `differ multi` atomic=1 seed=7, 40 consecutive runs | 0 diverged |
| ASAN build: multires, atomic_torn, atomic_ryow, execfix, execiso, multi_exec, atomfix | all ok, no ASan report |
| Liveness matrix (no WATCH), 1→16 conns | 0 wedges, throughput matches base |
| Interleaved A/B, atomic path | −1.29 % (see below) |
| Interleaved A/B, plain GET/SET p32 | −0.63 %, within run-to-run spread |

The −1.29 % is the resolver repair, on the atomic read path only, and §5(b) is the evidence that
it cannot be given back without reopening the defect. It is within the 3 % budget for always-on
machinery, and the plain GET/SET path does not reach the code at all.

The only sanitizer finding is pre-existing and outside this lane:
`third_party/lua/lstring.c:87` misaligned `uint32_t` load (UBSan).

---

## 7. Separate pre-existing defect found on the way: WATCH livelock

While hunting a deadlock in fix (a) I built a liveness harness, and it found a defect that is
**not mine and not new**. With WATCH armed and several connections pipelining a wide group plus a
transaction over overlapping keys, EXEC stops answering while the server still replies to PING on
another connection. Fresh server per repetition, hit rates:

| cells | base | shipped |
|---|---|---|
| 4 conns, WATCH | wedged **2/6** runs | wedged **2/6** |
| 8 conns, WATCH | wedged **4/6** | wedged 4/6 (single-run matrix readings varied; the 6-rep rates match) |
| 16 conns, WATCH | wedged **5/6** | wedged **3/6** |
| 1–16 conns, no WATCH | 0 wedges | 0 wedges |

Identical within noise, so this change neither causes nor worsens it. The mechanism is visible in
the code: `Shard::watch_finalize_reservation()` returns "not ready" while a reservation's epoch is
0, so a unit can wait on an undecided reservation held by a unit that is itself waiting. Not
pursued here — it is a different defect with a different owner-facing symptom (liveness, not data
loss). Harness left at `scratchpad/lab/` equivalents; `wedge2.py`'s shape is 1 connection ×
`WATCH` + wide `MSET`/`DEL` + `MULTI`(SET×n, DEL) + `EXEC`. **Recommend a follow-up lane.**

---

## 8. Files changed

| File | Change |
|---|---|
| `src/store/flatstore_atomic.inc` | program-order epoch repair in `atomic_collapse()` and `atomic_resolve_internal()`; `ignore_epoch` parameter on `atomic_has_own_undecided()` |
| `src/store/atomic_mvcc.h` | `prev_conn_id` / `prev_epoch` / `prev_set` carry state on `AtomicCollapseKey` |
| `src/cmd/multi.inc` | `atomic_exec_order_holds` instrumentation in `prepare_write_key()` (counting only — see §5(a)) |
| `src/core/shard.h` | `atomic_exec_order_holds` in `Shard::Stats`, cold tail |
| `src/cmd/t_server.cc` | `atomic_exec_order_holds` in `INFO stats` |
| `tests/multires.py` | new battery (added to no gate row — see below) |

`Op` and `Client` are untouched, so the `sizeof(Op)==336` / `sizeof(Client)==1984` static asserts
are unaffected. No new knob: this is a correctness repair on an existing path, not a feature.

**No gate row was added.** `tests/multires.py` is reliable in both directions here (8/8 fail
before, 10/10 pass after), so it is a defensible row — but it is a race probe whose hit rate moves
with core pinning and box load, and it was only ever exercised on this lane's 16-23 pinning. The
honest move is to hand it over and let the mainline operator decide after seeing it run on the
gate's own cores; wiring a row that has never run in its intended environment is how flaky rows
get born. Suggested placement if adopted: beside `atomfix`/`execfix` in the armed-boot group of
`tests/gate.sh` (it needs `--enable-debug-command yes`), and the expected-check count bumped
accordingly.

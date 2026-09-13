# NOTES-WATCHLIVE — WATCH liveness: EXEC stopped answering while PING kept answering

Lane `t-watchlive`, branch `t-watchlive`, off `perthread-locality` @ `329fa10ec`.
Cores 32-47, ports 7510 (target) / 7512 (redis 7.4 oracle). Every server was addressed by the pid
resolved from its listening socket; no process was ever selected by name or pattern.

---

## 1. Result

| | pre-fix | post-fix |
|---|---|---|
| watch, 4 conns, `--atomic 1` | wedged **6/6** | wedged **0/6** |
| watch, 8 conns, `--atomic 1` | wedged **6/6** | wedged **0/6** |
| watch, 16 conns, `--atomic 1` | wedged **6/6** | wedged **0/6** |
| no-WATCH control, 16 conns | wedged 0/6 | wedged 0/6 |
| watch, 16 conns, `--atomic 0` | wedged 0/6 | wedged 0/6 |

Fresh server per repetition in every cell. The same script produces both columns
(`tests/watchlive_gate.sh`, with the binary as argument 4), so the two columns are the same
measurement, not two different ones.

Rerun at twice the repetition count, post-fix, on a clean `make clean && make -j8` build:

```
watchlive gate (port 7510, cores 32-47, reps 12, fresh server per repetition)
  ok   watch conns=4  --atomic 1   wedged 0/12  (reservation contention 76434)
  ok   watch conns=8  --atomic 1   wedged 0/12  (reservation contention 72866)
  ok   watch conns=16 --atomic 1   wedged 0/12  (reservation contention 81411)
  ok   no-watch control conns=16   wedged 0/12  (control; reservation waits 0)
  ok   watch conns=16 --atomic 0   wedged 0/12  (reservation contention 345934)
watchlive gate PASS
```

Containment: 46/46 directed batteries (23 tests x both atomic modes), 62/64 differ runs — the two
that failed fail on the pre-fix binary too and never enter this code (§7). Plain GET/SET
throughput A/B: §8.

---

## 2. The reproducer, rebuilt

The sibling lane (`NOTES-MULTIRES.md` §7) found this defect, recorded the shape and the hit rates,
and lost the harness with its gitignored worktree. It is rebuilt in-tree:

* `tests/watchlive.py` — the load and the detector, plus the deterministic WATCH-contract checks.
* `tests/watchlive_gate.sh` — the gate, which owns the fresh-server-per-repetition discipline.

**Shape.** Per connection, per round: `WATCH` over half the band, a wide `MSET` and a wide `DEL`
over the band, then `MULTI` (`SET` x8, wide `DEL`) `EXEC`. Eight rounds are written as ONE pipelined
blob; 16 connections are released together by a barrier and repeat for two seconds over one shared
32-key band.

Three things had to be right before it reproduced at all, and each is a note for the next person:

1. **`--atomic 1`.** The default is `atomic = 0` (`src/core/config.h:213`). The defect lives in the
   atomic-group reservation path and does not reproduce at all with the default. This is why the
   first several hours of runs were clean.
2. **A sustained window.** One blob of the shape finishes in ~10 ms and reports clean every time.
   The load has to keep several units in flight together for the race to have somewhere to happen.
3. **One key band for the whole repetition.** Rotating the band per wave lets the connections drift
   apart as they lose wave-sync, and then they stop overlapping — the one property the defect needs.

**Detector.** A repetition is `wedged` only when a connection's replies are still absent after the
deadline **and** a PING on a separate connection answers inside that same window. A server that is
merely slow fails the PING probe too and is reported `stalled`, never `wedged`. The no-WATCH arm is
the zero control and reports 0 with the reservation counters at exactly 0, so WATCH is necessary to
the mechanism rather than merely present.

---

## 3. The mechanism, measured

`Shard::watch_finalize_reservation()` answered "not ready" while a reservation's epoch was still 0,
and every caller turns "not ready" into a Retry. `watch_reservations_` held **one** reservation per
key, so that answer made the reservation a mutual-exclusion **lock** on the key — taken by units
whose fragments run on many shards in arbitrary order. That is the textbook shape for a wait-for
cycle, and gdb on a wedged server found one:

```
ScatterState 0x7a6e834538c0   Kind::Mset   atomic_group=true  nsub=13  pending=1
    epoch=0  aborted=false  watch_refs=13  origin_conn_id=9
    reservations on shards 0,1,2,5,6,7,9,11,13,14   (keys wl:22410:{23,27,22,25,0,31,30,24,29,1,26,3,21})
ScatterState 0x7a6e83c70300   Kind::Del    atomic_group=true  nsub=10  pending=2
    epoch=0  aborted=false  watch_refs=3   origin_conn_id=14
    reservations on shard 15                        (keys wl:22410:{4,5,6})
```

Two ordinary cross-shard writes — one `MSET`, one `DEL`, from two different connections. Each had
executed most of its fragments and installed reservations; each one's remaining fragments needed a
key the other was holding. Neither can decide, so neither reservation can resolve, so neither can
decide. No timeout and no detector breaks it.

The counters make it unambiguous rather than a plausible reading. On the wedged server, over two
seconds:

```
watch_reservation_waits      432231802 -> 441190372   (+8,958,570)
total_commands_processed           289 ->       290   (+1, and that one was the INFO)
```

**Why the whole executor died, not just the two units.** "Not ready" parks the task on
`xshard_retries_` (`ex_loop.h:593`), and the loop services `ordered_deferred_` and drains its inbox
only while that deque is empty (`ex_loop.h:86-89`). One stuck task therefore stops its executor
accepting anything. That is why, on a wedged server, `SET unrelated:1` answered in 0.1 ms and
`SET unrelated:2` never returned — different shards, different executors. `PING` is keyless and
answered on an IO thread, so it never touched a wedged executor and kept reporting a healthy
server, which is exactly what makes this dangerous for production monitoring.

---

## 4. The fix

A reservation now records whether it makes anyone **wait** (`Shard::WatchReservation::blocking`),
and the registry holds a **list** per key instead of a single slot.

* **EXEC validate reservation — still blocking.** The transaction has validated its WATCH on that
  key, so no foreign write may commit ahead of its decision. Writers still wait for it. Unchanged.
* **Atomic-group write reservation — never blocking.** It exists only to defer *that group's*
  watcher-dirtying to its own commit decision, which needs no exclusivity at all. Groups now coexist
  on a key, and each dirties the key's watchers at its own decision, so no obligation is dropped.
  This is the edge that closed the cycle.
* **A validating transaction no longer waits for a group either.** A group holding a mutating
  reservation on a watched key *is* a write to a key this client watched, and "the group lands
  first" is a realizable serialization. The validation takes it and marks itself dirty, which is
  exactly what WATCH promises. The alternative reading — the group lands after — was only reachable
  by waiting for a decision that might itself be waiting on this transaction.

No knob, no timeout, no deadlock detector, no change to the MVCC resolver, the scatter engine core
or the single-owner rule. With group reservations blocking nobody, the observed cycle has no second
edge to close.

Waiting bought nothing in this shape anyway, which is worth stating because it looks like a
semantic trade and is not: pre-fix, the validating transaction waited for the group's decision and
was then dirtied by that same decision, so it aborted regardless. The A/B in §6 shows the EXEC
commit rate under this contention was already ~0% before the change.

### Files

| File | Change |
|---|---|
| `src/core/shard.h` | `WatchReservation::blocking`; `watch_reservations_` is now key -> vector; three cold `Stats` counters; `watch_append_reservation` declaration |
| `src/cmd/multi.inc` | `watch_finalize_reservation` retires every decided entry and blocks only on an undecided *blocking* one; `watch_validate_and_reserve` takes the group-lands-first serialization instead of waiting; `watch_write_ready` / `watch_reserve_write` list-aware; `watch_append_reservation` |
| `src/cmd/t_server.cc` | three INFO stats fields |
| `tests/watchlive.py` | the reproducer, the detector and the WATCH-contract checks (new) |
| `tests/watchlive_gate.sh` | the gate (new) |

### Counters (INFO stats)

| field | meaning |
|---|---|
| `watch_reservation_waits` | a unit was turned into a Retry by an undecided blocking reservation |
| `watch_reservation_coexist` | a group write recorded its reservation on a key that already carried a foreign undecided one — the cycle's precondition, survived |
| `watch_reservation_precommit_aborts` | a validation took the group-lands-first serialization and aborted |

All three are written only from the reservation registry, which stays empty until the first WATCH.
No WATCH, no cost: the no-WATCH control row reads exactly 0 on all three.

---

## 5. Why the gate cannot be vacuous or flaky

* **It asserts a rate over repetitions, never a single run**, with a fresh server per repetition.
  A wedged server stays wedged, so a second repetition on one boot is not an independent trial —
  that is the whole reason the gate boots its own server per repetition rather than per row.
* **It fails before and passes after**, over the same 6 repetitions per cell, with the same script.
  Run it against a pre-fix binary and the three armed rows report 6/6 (§1).
* **It refuses to pass a clean armed row that never entered the machinery**, by reading
  `watch_reservation_waits + watch_reservation_coexist` from INFO. This is not decoration: the first
  post-fix run of the gate FAILED the 4-connection row for exactly this reason, because at that
  point the counter only covered blocking waits and the fix had removed them. The counter that
  proves entry today is `coexist`, which is the defect's own precondition.
* **The control must report zero.** The no-WATCH arm runs the identical shape with the WATCH frames
  removed, and reports 0 wedges and 0 on every reservation counter.
* **Liveness, not slowness.** The PING probe is what separates a wedge from a slow server.
* **It cannot be passed by a server that aborts everything**, because the deterministic
  WATCH-contract checks require EXEC to still commit when no foreign write touched a watched key.

Post-fix contention actually observed by the armed rows (6 repetitions each):
`coexist` 29,591 (4 conns) / 33,231 (8 conns) / 36,979 (16 conns); the `--atomic 0` row moves
`waits` 180,083 instead, since it has no groups.

A single 16-connection repetition at `--atomic 1` post-fix reads:
`waits:0  coexist:6303  precommit_aborts:147212`. Read that as: the blocking wait that used to
close the cycle is gone (`waits:0` in this mode), the configuration that used to deadlock occurred
6,303 times and was survived, and 147,212 validations took the group-lands-first serialization
rather than waiting for it.

---

## 6. WATCH contract, oracle-backed

`python3 tests/watchlive.py <host> <port> --semantics-only` — deterministic, each foreign write
acknowledged before the transaction runs, so no race decides the answer.

```
  ok   foreign wide MSET acknowledged
  ok   EXEC aborts after a foreign write to a watched key None
  ok   the aborted transaction wrote nothing b'foreign'
  ok   EXEC commits with no foreign write [b'OK']
  ok   the committed transaction's write is visible b'committed'
  ok   foreign write to unwatched keys acknowledged
  ok   EXEC commits when the foreign write missed the watched keys [b'OK']
  ok   foreign wide DEL acknowledged
  ok   EXEC aborts after a foreign DEL of a watched key None
  semantics: 0 failure(s)
```

Identical on `--atomic 0`, on `--atomic 1`, and on the redis 7.4 binary at
`/tmp/claude-1000/redis74/src/redis-server`.

**EXEC commit rate under saturated contention, `--atomic 1`, 300 ms window, 16 connections:**

| | EXECs completed | committed | rate |
|---|---|---|---|
| pre-fix | 16 / 50 / 21 | 0 / 0 / 1 | ~1% |
| post-fix | 5040 / 4960 / 4952 | 0 / 0 / 0 | 0% |

The pre-fix arm only reached 16-50 transactions in 300 ms because it wedged almost immediately.
Both arms commit ~nothing: with 16 connections writing the watched band continuously, every
transaction genuinely does race a foreign write to a watched key, which is what WATCH exists to
report. The change did not create that; it made the server survive it. (With `--atomic 0`, where
the new path is inert, the commit rate is unchanged at 12,649/32,312 ≈ 39%.)

---

## 7. Containment

| Gate | Result |
|---|---|
| `make -j8` clean, no new warnings | ok |
| Directed batteries x both atomic modes — multi_exec, execfix, execiso, execatomic, atomfix, atomic_torn, atomic_ryow, ryow, concur, multires, writer_atomic, session_monotonic, s6, xacct, xscript, scriptatomic, xmove, zsetops, lcs, tracking, torture, blockmulti | **46/46** |
| `tests/atomic_torn.py`, both modes | ok (0 torn) |
| `tests/writer_atomic.py` 60 s, both modes | `status: clean`, 0 violations, 1.45M writes/arm, all four detector self-tests fired |
| Differ vs redis 7.4: multi, xshard, string, list, set, zset, hash, scan x seeds 7/19/23/31 x atomic 0/1 | **62/64** — see below |
| `tests/watchlive.py --semantics-only`, both modes + redis oracle | 0 failures |
| ASAN build + watchlive/multi_exec/execfix/atomic_torn | §9 |
| Interleaved A/B, plain GET/SET | §8 |

**The two differ runs that failed are pre-existing and not on this path.** Both are
`differ multi` at `--atomic 1` (seeds 23 and 31). Evidence:

* The suite is flaky on **both** binaries. `multi` seed 31, `--atomic 1`, 8 consecutive runs:
  pre-fix **0/8 pass**, post-fix **1/8 pass**. Seed 23 over 5 runs: pre-fix 2/5, post-fix 4/5.
* `gen_multi` in `tests/differ.py` issues no `WATCH` at all, and after those 8 runs the server
  reported `watch_reservation_waits:0`, `watch_reservation_coexist:0`,
  `watch_reservation_precommit_aborts:0` on **both** arms. The suite never enters the code this
  lane touched, so it cannot be the cause.
* The diffs are stale/missing values inside `EXEC` and `MGET` replies, i.e. the atomic-1 visibility
  family, not a WATCH outcome.

Recorded here rather than fixed: it is a different defect with a different owner-facing symptom,
and chasing it would have been an unbounded change to the MVCC resolver from this lane.

---

## 8. Interleaved A/B — nothing got slower

INDICATIVE (loopback, server pinned 32-39, memtier pinned 40-47, `--atomic 1`, 200k keys populated
first so the GET hit rate is 100% and `dbsize == keymax`). Arms interleaved A/B per round, four
rounds, medians reported.

| cell | pre-fix | post-fix | delta |
|---|---|---|---|
| GET/SET 1:10, pipeline 32, 4x4 | 4,017,468 ops/s | 4,172,424 ops/s | **+3.86%** |
| GET/SET 1:10, pipeline 1, 8x4 | 280,938 ops/s | 280,974 ops/s | **+0.01%** |

The p32 pre-fix arm spanned 3.91M-4.23M across its own four rounds (8% spread on a shared box), so
+3.86% is inside run-to-run spread and should be read as "no regression", not as a win. p1 is flat.
Neither result is surprising: the hot path never reaches this code, because every entry point is
behind `Shard::has_watches()`, which is false until the first WATCH and reads a member the shard's
owner has already touched.

---

## 9. ASAN

`make asan` (verified sanitized: `ldd build/tomokv-asan` shows `libasan.so.8` and `libubsan.so.1`),
both atomic modes, running `tests/watchlive.py --semantics-only`, the watchlive load at 8
connections x 2 repetitions, and `multi_exec` / `execfix` / `atomic_torn`:

* 0 wedges, 0 semantics failures, all three batteries ok in both modes.
* 0 AddressSanitizer reports.
* One UBSan finding, in both modes and pre-existing/outside this lane:
  `third_party/lua/lstring.c:87:14: runtime error: load of misaligned address ... for type
  'const uint32_t'` (already recorded in `NOTES-MULTIRES.md` §6).

---

## 10. What is NOT fixed, and why (shelved, with the argument)

**A residual wait-for cycle between two transactions is still reachable in theory.** After this
lane, the only remaining wait in the registry is "a writer waits for a foreign EXEC validate
reservation". An EXEC holds its validate reservations while its own body runs, and its body's
cross-shard child writes are writers. So E1's body waiting on E2's validate claim while E2's body
waits on E1's is still a cycle on paper.

It is not shipped-as-fixed because:

* **It is not demonstrated.** The `--atomic 0` row exercises exactly this wait class — 180,083
  blocking waits over 6 repetitions — and wedged 0/6 both before and after. Every wedge this lane
  produced, and every one the sibling lane reported, is the group-vs-group cycle. The owner rule is
  that no gate row may guard a defect nobody demonstrated, and the same standard should apply to the
  machinery a gate row would need.
* **The honest fix for it is out of this lane's blast radius.** Removing that wait means letting a
  writer force-abort a transaction that has already validated, which has to be atomic against that
  transaction's commit — i.e. the commit becomes a CAS on the epoch word with an "aborted" sentinel
  that every reader of that word must then interpret. That is the MVCC resolver, and my brief is
  explicit that an unbounded change there does not ship on this evidence.
* **Its blast radius is now much smaller anyway.** A transaction fragment's Retry parks on
  `multi_retries_`, which the loop services unconditionally, so unlike the group case it would hang
  the two transactions rather than stopping their executors. It would not take unrelated keys or
  unrelated connections down with it.

Design sketch for whoever picks it up: give the commit a CAS (`epoch.compare_exchange(0, ticket)`),
give the abort the same CAS to a sentinel, and then no unit in the registry ever needs to wait —
a writer that meets a validate claim aborts it and proceeds, which is a correct serialization
because the claim is by definition on a key that transaction watched.

**Reservation retirement is still lazy.** A decided reservation is retired by the next call that
touches its key on that shard. If a shard goes completely idle with reservations outstanding, the
`refs` pin on the unit's state is held until traffic returns. This is unchanged from before the
lane (the atomic-group path never retired its own reservations either — `xshard_watch_finish`
returns early for `atomic_group`), and the list-valued map bounds it by the number of concurrent
units rather than making it worse. Not addressed here.

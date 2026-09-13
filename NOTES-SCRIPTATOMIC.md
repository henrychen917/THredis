# P0 #3 — script-issued writes diverged under `--atomic 1` (lost writes)

Branch `t-scriptatomic`, off mainline `2e5f86c2e`.
Lane resources: server cores 32-47, oracle cores 120-127, ports 7017 (target) / 7018 (oracle).

---

## Verdict table

### differ `script`, HEAD vs fix — the suite exactly as it stood (4236 ops)

| seed | atomic 0 HEAD | atomic 0 fix | atomic 1 HEAD | atomic 1 fix |
|------|---------------|--------------|---------------|--------------|
| 4242 | 0 | 0 | **38** | **0** |
| 7    | 0 | 0 | **17** | **0** |
| 29   | 0 | 0 | **25** | **0** |
| 101  | 0 | 0 | **35** | **0** |

### differ `script`, HEAD vs fix — extended suite (4936 ops; phase 2 added by this lane)

| seed | atomic 0 HEAD | atomic 0 fix | atomic 1 HEAD | atomic 1 fix | `atomic_groups` (mode 1) |
|------|---------------|--------------|---------------|--------------|--------------------------|
| 4242 | 0 | 0 | **114** | **0** | 169 |
| 7    | 0 | 0 | **150** | **0** | 300 |
| 29   | 0 | 0 |  **70** | **0** | 471 |
| 101  | 0 | 0 | **113** | **0** | 626 |

`atomic_groups` is 0 in every atomic-0 cell (negative control) and 169-626 in every atomic-1 cell.
On the pre-existing suite that counter was **0 in every cell**: the script leg had never engaged the
cross-shard engine at all. See "What the differ leg was not testing" below.

### Batteries (release build, both modes)

| battery | atomic 0 | atomic 1 |
|---|---|---|
| atomfix | PASS | PASS |
| session_monotonic | PASS | PASS |
| atomic_torn | PASS | PASS |
| atomic_ryow | PASS | PASS |
| ryow | PASS | PASS |
| torture | PASS | PASS |
| multi_exec | PASS (see pre-existing finding) | PASS |
| lua_scripting | PASS | PASS |
| scriptsurf | PASS | PASS |
| **scriptatomic (new)** | PASS | PASS |
| differ string / hash / xshard / script | 0 diffs each | 0 diffs each |
| shutdown invariants | PASS | PASS |

### ASAN + UBSAN (`make asan`, `-fsanitize=address,undefined`)

| run | atomic 0 | atomic 1 |
|---|---|---|
| scriptatomic | PASS | PASS |
| lua_scripting | PASS | PASS |
| scriptsurf | PASS | PASS |
| torture | PASS | PASS |
| multi_exec | PASS | PASS |
| differ script (4936 ops) | 0 diffs | 0 diffs |
| sanitizer reports | none | none |

### INDICATIVE perf guard (loopback, lane cores only, p32, interleaved A/B, median of 3 blocks)

| cell | HEAD | fix | delta |
|---|---|---|---|
| atomic 0 p32 SET | 7,603,890 | 7,567,727 | −0.48% |
| atomic 0 p32 GET | 8,078,117 | 8,053,406 | −0.31% |
| atomic 1 p32 SET | 7,581,225 | 7,556,433 | −0.33% |
| atomic 1 p32 GET | 8,121,556 | 8,118,680 | −0.04% |

Within-arm spread across the three blocks reached 3.4% (HEAD atomic-0 GET: 7.87M / 8.08M / 8.14M),
so every between-arm delta above sits well inside per-arm variance. That is the expected answer:
the change touches `src/cmd/scripting.cc`, which no GET or SET ever enters, plus two `INFO` rows.

---

## Root cause

`script_execute()` armed a deep undo log over the activation's declared keys whenever `--atomic 1`
was set, and restored it whenever the activation failed. HEAD `2e5f86c2e`:

```
src/cmd/scripting.cc:241   class ScriptUndo
src/cmd/scripting.cc:260       KvObj* current = shard.store().find(entry.hash, key);   // capture
src/cmd/scripting.cc:274       shard.store().erase(entry.hash, key);                   // restore
src/cmd/scripting.cc:278       shard.store().insert(entry.hash, entry.original);       // restore
src/cmd/scripting.cc:1093  const bool atomic = g_script_server && g_script_server->atomic_enabled();
src/cmd/scripting.cc:1094  if (atomic && !undo.capture(shard, op, call.key_first, call.key_count))
src/cmd/scripting.cc:1121  const bool restored = !atomic || undo.rollback(shard);   // runtime-error arm
src/cmd/scripting.cc:1141  const bool restored = !atomic || undo.rollback(shard);   // conversion arm
```

**Redis has never undone a script's partial effects.** An activation that writes through
`redis.call` and then raises keeps everything it wrote. So the moment atomics were enabled, every
failing activation that had already written diverged from the oracle — and the divergence was a
lost write, because the restore *unwrites committed data*.

The interleave, with no concurrency required at all (`--atomic 1`, single connection, no pipeline):

```
SET z1 -3                                                             z1 = "-3"
EVAL "local r = redis.pcall('INCR',KEYS[1]) if r.err then ... end"    scripting.cc:1094 captures "-3"
   -> redis_dispatch INCR                       scripting.cc:620  z1 = "-2"   (committed)
   -> `r.err` indexes a Lua number -> raises    scripting.cc:1077  status != 0
   -> undo.rollback                             scripting.cc:1121
        erase(z1)                               scripting.cc:274   z1 gone
        insert("-3")                            scripting.cc:278   z1 = "-3"  <-- LOST WRITE
GET z1                                          target "-3"   oracle "-2"
```

That is symptom **(a)** from the brief, exactly: `op 144 FCALL dget 1 sc:11 → target -3 / oracle -2`.
The differ's script list contains that precise script
(`local r = redis.pcall('INCR', KEYS[1]) if r.err then return r.err end return r`, differ.py:1416),
and seed 4242 issues it on `sc:11` at op 72 — three ops after `SET sc:11 -3` at op 18 and 72 ops
before the first reported diff.

Symptom **(b)** — the key missing entirely — is the same site with `entry.original == nullptr`:

```
DEL z2                                                           z2 absent
EVAL <same script>       scripting.cc:1094 captures nothing (find() -> nullptr)
   -> INCR creates z2 = "1"                     scripting.cc:620
   -> raises                                    scripting.cc:1077
   -> rollback erases z2, inserts nothing       scripting.cc:274   z2 GONE
EXISTS z2                target 0   oracle 1
```

That is `op 1286 STRLEN sc:22 → target 0 / oracle 1`, and every later diff on `sc:22`
(`op 1327`, `op 1410`, `op 1453`, `op 1490`, `op 1619`, `op 1630`) is that same key inheriting the
loss — including the WRONGTYPE-shaped ones, where the two sides simply disagree about the key's
type because one side kept a write the other reversed.

Reproduced verbatim on HEAD before touching anything:

```
TARGET(atomic1)   GET z1    -> "-3"     ORACLE  GET z1    -> "-2"     (a)
TARGET(atomic1)   EXISTS z2 -> 0        ORACLE  EXISTS z2 -> 1        (b)
TARGET(atomic1)   GET z3    -> "abc"    ORACLE  GET z3    -> "abcXY"  (APPEND reversed)
```

### The second defect in the same code, which is why it could not be repaired in place

`capture()` reads through `FlatStore::find()` (flatstore.h:537), which resolves against the owner's
current MVCC read cut (`atomic_read_epoch_`, flatstore.h:542). `rollback()` writes through the raw
`erase`/`insert` pair, which does not. The two halves therefore live at different points in the
version history:

* A **read-only** activation (`EVAL_RO`, `FCALL_RO`, any function with `no-writes`) has its read cut
  pinned in program order at IO (`Op::read_cut_lo`) and installed by
  `atomics_glue.inc:788` / `:878`. Its `capture()` can legitimately return a **superseded**
  version, and `rollback()` would then publish that stale version physically over a newer committed
  one — a lost write that is not even the activation's own.
* A key with a live pending record has its physically installed object owned by that record's
  bookkeeping (`AtomicEntry::parked()`, atomic_mvcc.h:39). `erase()` frees the installed candidate
  behind the record's back, and `insert()` puts a foreign object in the slot the group still names.

So a "correct rollback" would have had to be rebuilt on the MVCC path — while still being the wrong
semantics. The mechanism had no correct version. Deleting it is the fix.

---

## Fix shape

**Delete the undo log; a failed activation keeps its effects, in both atomic modes.**

`src/cmd/scripting.cc`:
* `ScriptUndo` / `UndoEntry` / `clone_object` removed (156 lines out, 60 in), together with the
  `notify_abort_op()` calls whose only purpose was to un-emit notifications for reversed writes.
  A failed activation's notifications now stand alongside its effects, which is what atomic 0
  already did and what the oracle does.
* `ScriptEvictionGuard` becomes unconditional rather than `--atomic 1`-only. Its documented
  justification was the undo log ("evicting an undeclared bystander that the declared-key undo log
  could not restore", NOTES-LUA.md), so gating it on the mode is now meaningless — and leaving it
  mode-dependent would keep a second way for the atomic setting to change what a script observes.
  Redis also takes its OOM decision once, at script start. Cost: two plain stores per activation on
  a path GET/SET never enters.
* Two counters, `script_effect_writes` and `script_failed_after_effects` (INFO STATS), described
  under "Vacuous-validation" below.

Isolation is untouched and was never the undo log's job: a script is one task on one owner and the
single-owner law keeps every other task out for its whole duration, so no connection can observe a
half-finished activation. What the undo log added was *failure*-atomicity, which Redis does not
offer.

### Alternatives considered and why they lose

| alternative | why it loses |
|---|---|
| Rebuild the rollback on the MVCC path (capture and restore both at the same cut, through the atomic install path) | Fixes the lost write and leaves the divergence: a *correct* rollback still reverses effects Redis keeps, so the differ stays non-zero under atomic 1 for every write-then-raise script. Deliverable 3 (0 diffs in BOTH modes) is unreachable with any rollback. |
| Roll back in BOTH modes instead of neither | Same divergence, now in both modes — 0 diffs becomes impossible in atomic 0 as well. |
| Keep rollback behind a knob, default off | Redis has no such knob, so the knob-compat rule gives it no name to adopt; and a knob whose ON position is always wrong against the oracle is exactly what the hardcode-or-delete rule says to delete. |
| Make the script activation a real cross-shard atomic group so a failure aborts it | Much larger change (scripts are `ScriptRoute`/single-owner by construction, and `redis.call` dispatches straight to handlers), and it still produces Redis-incompatible failure atomicity. |

### Scope explicitly NOT changed

* `notify_abort_op()` (src/cmd/notify.inc:605, declared notify.h:194) now has no caller in the
  tree. It is the notify lane's API, not this lane's file; removing it would churn another lane's
  surface for no correctness gain. Flagged here for that owner.
* The instruction-limit abort keeps its `BUSY` reply and now also keeps its effects. Redis has no
  equivalent (a long script simply runs; `SCRIPT KILL` only works while the script has not written),
  so this arm has no oracle to match — it is made *consistent* with the error arm rather than
  matched to Redis, and `tests/scriptatomic.py` asserts both modes agree on it.

---

## Vacuous-validation: what proves the guarded path ran

Two counters in `INFO STATS`:

* `script_effect_writes` — nested `Write`-flagged commands dispatched from a script that answered
  without an error, i.e. keyspace effects already applied (scripting.cc, at the `redis.call`
  dispatch site, immediately after `spec->handler`). It is a proxy in one direction only: a Write
  row that returns a non-error may occasionally have changed nothing (`SETNX` returning 0). Every
  arm that relies on it also asserts the resulting **value**, so the pair is exact where it matters.
* `script_failed_after_effects` — activations that then FAILED with at least one such effect
  standing. This is precisely the interleave the undo log used to reverse. A regression that leaves
  it at zero has not touched the fixed path, and `tests/scriptatomic.py` reports that case as
  `VACUOUS` and fails.

Negative controls in the same battery: a refused read-only write and a failing read-only activation
must **not** advance `script_failed_after_effects`; and `atomic_groups` must stay at 0 under
`--atomic 0` while advancing under `--atomic 1`.

Observed on the fix: `failed_after_effects +9` per core pass in each mode, `+96` per cross-shard
pass in each mode, `effect_writes +12` per core pass.

## Regression: fails on HEAD, passes on the branch

`tests/scriptatomic.py` (new). Same binary-agnostic battery run against both builds, armed boot,
port 7017:

| build | result |
|---|---|
| HEAD `2e5f86c2e` | **21 FAIL (6 vacuous)** — every atomic-1 data arm fails; all atomic-0 data arms pass; the mode-equivalence arm reports `a_value: b'6' vs b'5'`, `b_exists: 1 vs 0`, `b_value: b'made' vs None`, `c_value: b'abcXY' vs b'abc'`; the cross-shard arm reports `lost script INCR` and `lost script SET` on every round |
| `t-scriptatomic` | **0 FAIL (0 vacuous)** |

The battery covers, in both modes and requiring identical observations from both:
write-then-raise, create-then-raise, append-then-runtime-error, delete-then-raise,
nested-WRONGTYPE-after-write, reply-conversion failure, instruction-limit abort, the differ's
`pcall`-indexes-a-number shape, a two-declared-key same-owner activation, a refused read-only write
behind a committed increment, plus the two negative controls and a success control. The cross-shard
section then repeats the claim with the epoch-MVCC engine live: a genuine cross-shard `MSET` group
(owners proven by `DEBUG SHARD`, not assumed from key names), a write-then-fail script on one of the
group's keys, and an `MGET` that must observe the script's effect — all in one pipeline, with
`ATOMIC-COMMIT-DELAY 60` and `ATOMIC-READ-DELAY 60` armed to widen the window.

It joins the gate's debug-surface section (`tests/gate.sh`, alongside `atomfix`).

## What the differ leg was not testing

The `script` suite ran with `atomic_groups:0`, `atomic_entries:0`, `atomic_localfast:0`,
`atomic_read_cuts_held:0`. Every op in it is single-key, so the cross-shard engine never engaged:
the leg could not have caught a script-versus-MVCC interaction even in principle, and it did not
catch this one either (it caught the plain semantic divergence).

`gen_script` therefore gained a phase 2 (tests/differ.py): 700 ops interleaving genuine multi-key
commands (`MSET` / `MGET` / `DEL` / `UNLINK` / `EXISTS` / `TOUCH`) with write-then-fail scripts and
`FCALL`s over the *same* key space, so those ops share pipeline chunks. Everything in it is ordinary
Redis and stays byte-comparable. Phase 1 is left byte-identical on purpose, so the recorded per-seed
diff counts above remain the historical ones.

Effect: `atomic_groups` 169-626 per run under `--atomic 1` (0 under `--atomic 0`),
`atomic_read_cuts_held` non-zero, and the leg's discrimination against HEAD roughly triples
(38→114, 17→150, 25→70, 35→113).

## Pre-existing finding, NOT from this lane: `multi_exec` torn EXEC at `--atomic 0`

The first full sweep showed one failure: `multi_exec` `EXEC one-ticket torn-read arm`,
`--atomic 0`, `torn=3`. It reproduces on HEAD and is not caused by this change (scripting.cc is
unreachable from MULTI/EXEC).

Discriminated with a standalone replica of that arm (1 writer doing `MULTI` + 8 `SET`s + `EXEC`,
4 readers doing `MGET` of the 8 keys):

| condition | HEAD `2e5f86c2e` | `t-scriptatomic` |
|---|---|---|
| quiet box, `--atomic 0` | 0/18 rounds torn | 0/18 rounds torn |
| server cores contended (8 spinners on 40-47), `--atomic 0` | **3/6 rounds torn** | 0/6 rounds torn |
| server cores contended by a concurrent compile, `--atomic 0` | **16/20 rounds torn** | 2/11 rounds torn |

The torn shape is a genuinely partial `EXEC`: readers observe
`[0,0,0,1,0,0,0,0]` — exactly one of the eight keys carrying the new sequence number. So under
`--atomic 0`, an `EXEC` spanning owners is observable mid-application once the owners are preempted.
Both arms show it under contention; the counts differ only because the two arms could not be given
identical contention (the heaviest condition was an incidental compile). This belongs to the MULTI
lane. Repro script kept at
`/tmp/claude-1000/-home-user-Projects/ee6eb242-5302-49cf-b767-1a2d8d8f0f61/scratchpad/sa/tornprobe.py`.

## Harness note

The Redis 7.4 oracle on port 7018 was killed mid-sweep by something outside this lane (no OOM in
the kernel log, no error in its own log — most likely a sibling lane's `pkill` by binary name; the
box runs nine lanes). Two differ legs reported `NO-SUMMARY` from `ConnectionRefused` before it was
noticed. The lane harness now verifies the ORACLE listener (`ss -lntp` on 7018) before every differ
leg and re-boots it if absent, the same way it already verified the target listener before every
start and after every stop. Both affected legs were re-run against a live oracle and are included
above.

## Files

| file | change |
|---|---|
| `src/cmd/scripting.cc` | undo log deleted; eviction guard unconditional; effect counters |
| `src/cmd/scripting.h` | `ScriptStats::effect_writes`, `ScriptStats::failed_after_effects` |
| `src/cmd/t_server.cc` | two `INFO STATS` rows |
| `tests/scriptatomic.py` | new directed battery (fails on HEAD, passes here) |
| `tests/differ.py` | `gen_script` phase 2: scripts against a live cross-shard engine |
| `tests/lua_scripting.py` | rollback arms replaced by partial-effect arms in both modes |
| `tests/gate.sh` | `scriptatomic` joins the debug-surface section |
| `NOTES-LUA.md` | error/atomic section rewritten; superseded design recorded |

No new config knobs (the fix removes behavior; it does not add any).

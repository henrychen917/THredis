# EXEC visibility to concurrent readers (P0 #4, lane t-execatomic)

`tests/multi_exec.py`'s "EXEC one-ticket torn-read arm" failed at `--atomic 0` whenever the
server's cores were contended: concurrent readers observed a partially applied transaction, e.g.
`[238,238,239,238,238,238,238,238]` — one key of an eight-key `MULTI/SET…/EXEC` visible, the rest
not. Quiet box 0/18, eight spinners 15/20, under a concurrent compile 16/20.

**Verdict: code bug on the READ side. The write side was already correct.** Fixed; the arm is now
deterministic instead of a coin flip, and the old flaky arm is no longer able to pass vacuously.

---

## 1. The contract question, settled first

**Does atomic-0 `EXEC` promise all-or-nothing visibility to concurrent readers? YES.**

This is not an inference from vanilla redis being single-threaded (though it is also that: the
differ oracle at `/tmp/claude-1000/redis74` executes `EXEC` on one thread, so every reader there
sees all of a transaction or none of it, and this tree is measured byte-for-byte against it). It is
what this tree's own code and its own documentation already say, in both modes:

| Evidence | Where |
| --- | --- |
| `EXEC` takes the atomic admission with `force = true`, i.e. *regardless of the `--atomic` knob* | `src/cmd/multi.inc:948-951` |
| Every queued write is installed as a private MVCC candidate at epoch 0, unconditionally — there is no `atomic_enabled()` test on this path | `src/cmd/multi.inc:505-550` (`prepare_write_key`), `:561-600` |
| The last participating owner draws and release-publishes **exactly one ticket** for the whole transaction | `src/cmd/multi.inc:1343-1361` (`atomic_commit_group(state.epoch)` at `:1352`) |
| "`EXEC` force-admits through the atomic group window even when `--atomic`/`CONFIG SET atomic 0` is selected. … the last group participant draws and release-publishes exactly one ticket. **Foreign readers resolve the predecessor while the ticket is zero**" | `NOTES-MULTI.md:20-24` |
| The `--atomic` knob's own documentation scopes it to the *plain multi-key write lane* (`MSET/MSETNX/DEL/UNLINK` groups, `MGET/EXISTS/TOUCH` snapshot reads) — it never claimed to gate `MULTI/EXEC` | `src/core/config.h:212-215`, `tomokv.conf:157-161` |

So the test asserted the real contract. The engine did not honour it. Nothing in the test needed
rewriting; what it needed was a way to fail on demand instead of once per few thousand contended
reads (see §4).

---

## 2. Root cause, with the exact interleave

A cross-shard **read** resolves each of its fragments on that fragment's own owner, at that
fragment's own moment. Whether such a read pinned a *cut* was decided from one live sample:

```
src/cmd/scatter_engine.inc:876-877
    const uint64_t atomic_mode = server.atomic_mode_state();
    const bool tracking = atomic_mode != 0;
…
src/cmd/scatter_engine.inc:1145 (pre-fix)
    const bool needs_snapshot = !force_atomic && tracking && …
```

`atomic_mode_state()` is `Server::atomic_activity_` (`src/core/server.h:540-544`): the
`kAtomicEnabledBit` OR'd with a **count of IO threads currently holding an atomic admission
lease** (`atomic_try_admit` bumps it at `src/core/server.h:612-614`, `atomic_retire_group` drops it
at `:639-640`).

At `--atomic 1` the enabled bit is always set, so `tracking` is always true and every cross-shard
read pins a cut. **At `--atomic 0` the only thing that ever writes that word is `EXEC` itself** —
and only for the interval between its admission and its retirement. Between two transactions the
word reads zero.

A read prepared in that gap left `state->snapshot` at its default `UINT64_MAX`
(`src/cmd/scatter_engine.inc:356`, now `:362`), which makes `ReadEpochGuard` skip
`atomic_set_read_context()` entirely (`src/cmd/scatter_engine.inc:1504-1518`, now `:1540-1554`).
With the owner unbound, `FlatStore::find` → `atomic_resolve(h, key, UINT64_MAX)`
(`src/store/flatstore.h:542`) accepts **any** non-zero epoch (`src/store/flatstore_atomic.inc:586`),
i.e. every fragment answers "newest committed at the instant *this fragment* runs".

### The interleave (atomic 0, 8 keys over 8 owners)

| t | thread | event |
| --- | --- | --- |
| t0 | — | no transaction admitted anywhere ⇒ `atomic_activity_ == 0` |
| t1 | IO-r | `MGET k0..k7` parsed. `xshard_prepare` samples `tracking == false` ⇒ `needs_snapshot == false` ⇒ `state->snapshot = UINT64_MAX`. 8 fragments posted. |
| t2 | EX-a | fragment for shard A runs unbound, reads `kA = seq` (old) |
| t3 | IO-w | `EXEC` admitted (`multi.inc:948`, force). Candidates installed at epoch 0 on every participating owner. |
| t4 | EX-z | last participant runs `atomic_commit_group(state.epoch)` (`multi.inc:1352`): one ticket, released into the shared epoch word. All 8 keys become `seq+1` at once. |
| t5 | EX-b | fragment for shard B runs — still unbound — resolves `kB = seq+1` (new) |
| t6 | IO-r | reply assembled from fragments straddling the ticket ⇒ `[seq, …, seq+1, …, seq]` |

Contention is not the cause. It is the magnifier: the spinners stretch the t2→t5 fan-out from
microseconds to milliseconds, which is why the gate — which runs quiet — never saw it.

This is a *different* defect from the sibling lane's just-fixed one. That one was a group whose
ticket became visible two instructions before its records did, and it required the reader to hold a
cut that landed inside that hole. Here the reader holds **no cut at all**, so the hole is the whole
fan-out.

### Corroboration before any code was written

| Arm | HEAD, identical spinner load |
| --- | --- |
| `tornprobe`, `--atomic 0` (tracking flickers) | **15/20 rounds torn** (5,119 torn reads of ~430k) |
| `tornprobe`, `--atomic 1` (tracking pinned on) | **0/8 rounds torn** |

The only read-path difference between those two boots is the `tracking` term above.

---

## 3. The fix

`src/cmd/scatter_engine.inc:1151-1167`

```cpp
const bool read_fanout = group_cap > 1 &&
    !(op.spec->flags & (CmdFlags::Write | CmdFlags::SnapshotWrite));
const bool needs_snapshot = !force_atomic && (tracking || read_fanout) && …
```

A cross-shard **read** now carries a cut whether or not a group happens to be in flight at the
instant it is prepared. That is the whole change; everything downstream (the cut itself, the
`register_snapshot` floor handshake, `ReadEpochGuard`, `atomic_resolve`) is the machinery
`--atomic 1` has always used, unchanged. **After the fix, a cross-shard read behaves identically in
both atomic modes** — which is the right invariant, because the knob was never supposed to gate
`EXEC` in the first place.

Scoping, and why each term is there:

- `group_cap > 1` — a read whose fragments all land on one owner resolves in a single task and
  cannot straddle anything (a shard's MVCC records are installed only by that shard's own owner, so
  they cannot change under a task). Single-owner multi-key reads keep the old cost exactly.
- `!(Write | SnapshotWrite)` — write groups keep the old gate. At `--atomic 0` a cross-shard write
  is not a group at all and promises concurrent readers nothing that a cut would buy, so paying for
  one there would be pure cost.
- `!force_atomic` is untouched — a `MULTI` child publishes through its parent (but see §7).

Engine laws honoured: shards stay single-owner (this is a read-side decision made on IO); no new
stall or hazard (the cut *avoids* one); no new cost on the plain `GET`/`SET` path — `GET`/`SET` are
not `MultiShard`, so they return `NotScatter` at `src/cmd/scatter_engine.inc:873` and never reach
the changed line. `sizeof(Op) == 336` / `sizeof(Client) == 1984` unchanged; the two new fields live
in the arena-allocated `ScatterState`.

### Counter (vacuous-validation)

`atomic_fanout_cuts` (`INFO stats`, `src/core/server.h:819-829`, incremented at
`src/cmd/scatter_engine.inc:1258`) counts exactly the reads that pinned a cut *although the tracking
word read zero* — i.e. the reads the old gate sent out naked. It is the proof that a passing run
actually entered the guarded path. `atomic_predecessor_reads` (already in `INFO`) then shows the cut
doing real work: during one 20-round contended run it advanced by **134,664**, i.e. 135k reads
resolved to a parked predecessor instead of the newer physical value.

---

## 4. The deterministic regression

The old arm is a *stress* arm: on a quiet box its readers finish their fan-out in microseconds and
never straddle anything, so `torn == 0` there proves nothing. Two things were added.

**a) `DEBUG ATOMIC-FANOUT-DEFER <microseconds>`** (`src/cmd/t_server.cc:842-858`,
`src/core/server.h:784-797`, armed at `src/cmd/scatter_engine.inc:1319-1329`, enforced at
`src/cmd/atomics_glue.inc:636-646`). It **parks** — re-queues, never spins — every fragment of a
cross-shard read except the one on its lead shard, for the requested window. Parking rather than
stalling is the load-bearing choice: the executor stays free, so the transaction the test is racing
can actually run and commit *inside* the read. Zero in production; when disarmed it is one load of
an arena word the task has already touched, on the scatter path only (precedent:
`ATOMIC-DIRECT-DEFER`, `ATOMIC-COMMIT-DELAY`, `ATOMIC-READ-DELAY`).

**b) `tests/execatomic.py`** — a new battery (one file, wired into `tests/gate.sh`'s armed
debug-surface loop, which already boots both atomic modes with `--enable-debug-command yes`). Arms:

| Arm | What it locks |
| --- | --- |
| geometry | `DEBUG SHARD` proves the keyed set spans >1 owner **and** that the walker set covers every owner including the lead (shard 0) — otherwise the arm could not straddle and would pass vacuously |
| straddle: `MGET` | reply must be one generation, and specifically the pre-transaction one |
| straddle: `EXISTS` | never a partial count (0 or 8, never 7) |
| straddle: `KEYS` | the whole-owner walker family, newly covered by the fix |
| window-opened check | the transaction's reply must land *before* the parked read's, and the read must really have been held |
| unarmed control | the same shape completes in <1/4 of the park budget, so a passing armed round cannot be "the hook did nothing" |
| visibility control | a transaction committed *before* the read is issued must be fully visible — a cut is a cut, not a freeze |
| RYOW control | the reading connection's own plain and transactional writes stay visible under the cut |
| stress arm | the historical shape, with `atomic_fanout_cuts` required to advance |

`tests/multi_exec.py`'s original `torn_arm` was kept and de-vacuumed: it now also requires
`atomic_fanout_cuts` to advance during the arm, so a quiet-box run can no longer report success
without the readers having taken the guarded path.

### HEAD vs fix

Control binary = the fix hunk reverted, hook and counter kept, so the comparison isolates one
expression.

| Arm | unfixed | fixed |
| --- | --- | --- |
| `tests/execatomic.py` deterministic straddle `MGET`, atomic 0 | **4/4 rounds torn** | 0/4 |
| … `EXISTS`, atomic 0 | **4/4 torn** (7 of 8 exist) | 0/4 |
| … `KEYS`, atomic 0 | **4/4 torn** (15 of 16 listed) | 0/4 |
| … stress arm counter, atomic 0 | `fanout_cuts=+0` ⇒ FAIL | `+40,715` ⇒ pass |
| battery exit | **4 checks failed** | passed |
| `tornprobe`, 8 spinners on cores 40-47, 20 rounds, atomic 0 | **15/20 rounds torn** | **0/20 rounds torn** |
| `tornprobe`, quiet box, atomic 0 | 0/6 (why the gate never caught it) | 0/6 |

The deterministic arms need no spinners, no loaded box and no luck.

---

## 5. Validation tally

| Suite | atomic 0 | atomic 1 |
| --- | --- | --- |
| `multi_exec`, `atomfix`, `atomic_torn`, `atomic_ryow`, `ryow`, `torture`, `lua_scripting`, `scriptatomic`, `execatomic`, `session_monotonic` | 10/10 pass | 10/10 pass |
| shutdown invariants (`live_conns=0 rob_not_quiesced=0 unsent_bytes_pending=0`) | pass | pass |
| `differ.py` vs vanilla redis 7.4 on 7020 — `string` 4033 ops, `hash` 3545, `xshard` 4276, `script` 4936 | **0 diffs ×4** | **0 diffs ×4** |
| ASAN+UBSAN (`-fsanitize=address,undefined`) — `execatomic`, `multi_exec`, `atomfix`, `atomic_torn`, `atomic_ryow`, `ryow`, `torture` | clean, 0 diagnostics | clean, 0 diagnostics |

Total: 22/22 sweep rows, 8/8 differ suites at 0 diffs, 0 sanitizer diagnostics.

---

## 6. INDICATIVE perf guard (loopback, server cores 32-47, loadgen 48-63)

`memtier_benchmark`, 100k-key range, dbsize pinned to the key range before every GET cell,
20s cells, `--shards 16 --ratio 6:10`.

| Cell | HEAD | fix | Δ |
| --- | --- | --- | --- |
| SET p32, atomic 0 | 7,269,155 | 7,219,154 | −0.69% |
| GET p32, atomic 0 | 7,855,989 | 7,885,479 | +0.38% |
| SET p1, atomic 0 | 547,561 | 543,862 | −0.68% |
| GET p1, atomic 0 | 546,646 | 547,704 | +0.19% |
| SET p32, atomic 1 | 7,274,987 | 7,284,325 | +0.13% |
| GET p32, atomic 1 | 7,780,017 | 7,705,482 | −0.96% |
| SET p1, atomic 1 | 545,034 | 544,579 | −0.08% |
| GET p1, atomic 1 | 546,317 | 544,556 | −0.32% |
| **MGET-8 cross-shard p32, atomic 0** | 1,615,183 | 1,548,171 | **−4.15%** |
| MGET-8 cross-shard p32, atomic 1 | 1,553,292 | 1,589,253 | +2.32% (no-op arm; noise scale) |

`GET`/`SET` are unchanged in both modes — they never enter `xshard_prepare`. The cost lands
entirely on the one family the fix touches, so that cell was re-measured as four **interleaved**
A/B pairs at 15s (`scratchpad/execatomic/abmget.sh`):

| pair | HEAD | fix | Δ |
| --- | --- | --- | --- |
| 1 | 1,708,008 | 1,631,555 | −4.48% |
| 2 | 1,709,717 | 1,643,399 | −3.88% |
| 3 | 1,687,721 | 1,634,833 | −3.13% |
| 4 | 1,693,330 | 1,641,810 | −3.04% |

**≈ −3.6% on cross-shard multi-key reads at `--atomic 0`, and nothing anywhere else.** The number
has a clean interpretation: HEAD's atomic-**1** MGET-8 cell (1.553M) already sat where the fixed
atomic-**0** cell now sits (1.548M). The fix does not add a new tax; it makes atomic-0 cross-shard
reads pay the read-cut registration that atomic-1 cross-shard reads have always paid. It is the
inherent price of the contract: reader-atomicity needs a cut, and a cut needs a published cleanup
floor, or cleanup collapses the predecessor the cut is supposed to see.

**Follow-up worth measuring (not done here):** the per-command `register_snapshot`/
`unregister_snapshot` pair is the cost. The IO loop already computes one `pass_read_cut` per parse
pass (`src/core/io_loop.h:847-848`); publishing a floor **per pass** rather than per command would
amortise it, at the price of holding the floor slightly longer (more retained versions). That is a
performance refinement of a settled contract and wants its own lane and its own A/B.

---

## 7. SHELVED, with the reason: a second defect of the same family

While probing the contract I found — and reproduced — a **separate, pre-existing** hole that this
lane deliberately did **not** fix.

**A cross-shard read *inside* a transaction straddles a foreign transaction's ticket, in BOTH
atomic modes.** `MULTI / MGET k0..k7 / EXEC` on one connection, while another connection commits
`MULTI / SET k0..k7 / EXEC`, returns a mixture of the two generations.

Mechanism: a `MULTI` child binds the *unbound* read context on every owner it touches —

```
src/cmd/multi.inc:607   (execute_local_command)
src/cmd/multi.inc:620   (execute_cross_command)
    shard.store().atomic_set_read_context(UINT64_MAX, state.origin_conn_id);
```

— exactly the same "newest committed right now, per fragment" behaviour the fix above removed from
plain reads. The `!force_atomic` term in `needs_snapshot` keeps `MULTI` children out of the
snapshot machinery in both modes, so `--atomic 1` does not mask it.

Reproduction (`scratchpad/execatomic/inexec_probe.py`, 8 spinners on cores 40-47):

| binary | mode | result |
| --- | --- | --- |
| HEAD `4565b10f9` | atomic 0 | **4/4 rounds torn** (~1,100 torn reads/round) |
| this branch | atomic 0 | **8/8 rounds torn** (~500-750/round) |
| this branch | atomic 1 | **4/4 rounds torn** |

It is pre-existing and untouched by this lane's change (the fix does not go near the `force_atomic`
path). It is shelved rather than fixed for two reasons, in order of weight:

1. **It is a semantics decision, not a bug fix.** Pinning a transaction's *reads* to a cut while its
   *writes* keep cloning from the newest committed value (`prepare_write_key` binds `UINT64_MAX` at
   `src/cmd/multi.inc:519` and `:548`, deliberately: a transaction's read-modify-write must not
   overwrite a concurrent commit from a stale base) would give `EXEC` a **mixed** contract —
   snapshot-isolated reads over read-committed writes — which is arguably worse than today's
   uniform read-committed. Making the whole transaction snapshot-isolated instead changes what
   `INCR`/`APPEND` inside `MULTI` mean under concurrency. That is an owner call about the isolation
   level `EXEC` offers, and it should be made deliberately, not as a side effect of a P0 hunt.
2. **There is a mechanical blocker underneath it.** Registering a read cut needs a published
   cleanup floor, and the floor slot is *per IO thread*
   (`Server::atomic_read_floors_[thread]`, scanned over `ifid_threads()` at
   `src/core/server.h:731-746`), owned exclusively by that thread's `ScatterArenaPool`. A
   `MultiExecState` carries its **own** `ScatterArenaPool`, so if it registered a cut it would
   clobber the IO loop's published floor — which is precisely why `!force_atomic` is in
   `needs_snapshot` today. Doing it properly means generalising
   `ScatterArenaPool::register_snapshot` off `ScatterState*` onto a small shared cut header so a
   `MultiExecState` can register through `IoLoop::scatter_pool_` (register at
   `multi_dispatch_entry`, unregister at `multi_retire_entry` — both already on the owning IO
   thread, so the bracket is clean). That is a ~80-line change inside the frozen MVCC snapshot core
   and wants its own lane, its own battery and its own differ run.

No test arm was added for it: a permanently red gate row is worse than a documented gap. The probe
is checked in so the next lane can re-run it in one command.

---

## 8. Files

| File | Change |
| --- | --- |
| `src/cmd/scatter_engine.inc` | the fix (`read_fanout` term); `atomic_fanout_cuts` increment; two `ScatterState` test-hook fields + their arming |
| `src/cmd/atomics_glue.inc` | `DEBUG ATOMIC-FANOUT-DEFER` park in `xshard_task_should_defer` |
| `src/core/server.h` | `debug_atomic_fanout_defer_` hook knob; `atomic_fanout_cuts_` counter |
| `src/cmd/t_server.cc` | `DEBUG ATOMIC-FANOUT-DEFER` subcommand; `atomic_fanout_cuts` in `INFO stats` |
| `tests/execatomic.py` | new battery (deterministic straddle + controls) |
| `tests/multi_exec.py` | `torn_arm` de-vacuumed with the counter |
| `tests/gate.sh` | `execatomic` joins the armed debug-surface loop (both atomic modes) |
| `scratchpad/execatomic/` | lane harness: listener-pid boot/stop, contended repro, sweep, differ, perf guard, interleaved A/B, and the shelved-defect probe |

No config knob was added: a correctness contract must not be optional, and `--atomic` already
means something narrower than its name suggests. `tomokv.conf` therefore needed no change.

## 9. Harness note

Every server in this lane was started and stopped through `scratchpad/execatomic/lane.sh`, which
resolves the pid **from the listener** (`ss -lntp`), refuses to boot onto a live listener, and
verifies the listener is gone after stopping. Nothing was ever killed by name or pattern. One
near-miss is worth recording: the lane's first ASAN build wrote to `/tmp/claude-1000/tomokv-asan`,
which already belonged to another lane; it was stopped before linking and every artifact of this
lane now lives under `/tmp/claude-1000/execatomic/`.

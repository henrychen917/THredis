# EXEC is never weaker than bare (lane t-execiso)

A cross-shard `MGET` wrapped in `MULTI`/`EXEC` **tore**, while the identical **bare** `MGET` against
the same server, the same keys and the same concurrent writer did **not**. `MULTI` therefore handed a
cross-shard read *less* isolation than running the command without `MULTI`. Fixed. The gate row lands
with the fix and is proved red-before / green-after on binaries that differ by one expression.

---

## 1. Reproduction, mine, before touching anything

Geometry: server `--shards 16 --ratio 2:4` (2 io + 4 ex, one thread per core) pinned to cores 0-5,
loadgen on cores 6-7, ports 7080/7081. Binary = unmodified `HEAD dbef14d43`. Probe =
`scratchpad/execiso/repro.py`, 6 s per arm, 4 reader connections + 1 writer connection running
`MULTI / SET k0..k7 <seq> / EXEC` in a loop. `torn` = one reply whose eight elements are not all
equal, i.e. one read that saw two generations of a single-ticket transaction.

| arm | reads | torn | **rate** | `atomic_fanout_cuts` |
| --- | ---: | ---: | ---: | ---: |
| **`--atomic 0`** | | | | |
| bare `MGET k0..k7` + writer | 121,729 | 0 | 0.000% | **+118,916** |
| **in-EXEC `MULTI/MGET/EXEC` + writer** | 49,310 | **367** | **0.744%** | **+0** |
| in-EXEC, **no concurrent writer** | 86,596 | 0 | 0.000% | +0 |
| in-EXEC, **same-owner key set** | 48,603 | 0 | 0.000% | +0 |
| **`--atomic 1`** | | | | |
| bare `MGET k0..k7` + writer | 121,954 | 0 | 0.000% | +0 |
| **in-EXEC `MULTI/MGET/EXEC` + writer** | 49,251 | **273** | **0.554%** | **+0** |
| in-EXEC, **no concurrent writer** | 86,116 | 0 | 0.000% | +0 |
| in-EXEC, **same-owner key set** | 49,321 | 0 | 0.000% | +0 |
| **vanilla redis 7.4** (`/tmp/claude-1000/redis74`) | | | | |
| bare `MGET` + writer | 121,132 | 0 | 0.000% | n/a |
| in-EXEC `MULTI/MGET/EXEC` + writer | 48,605 | 0 | 0.000% | n/a |
| in-EXEC, no writer | 83,978 | 0 | 0.000% | n/a |

All four zero-reading controls reproduce. A torn sample looks like
`[b'8',b'8',b'8',b'8',b'8',b'8',b'8',b'7']` — seven keys of one generation, one of the previous.

**Rates.** 0.744% / 0.554% here vs the challenged 0.208% / 4.751% quoted in the brief: same defect,
different one-thread-per-core geometry (mine is 2 io + 4 ex on 6 cores). The earlier 28% / 55%
figures came from a 4-ex-on-one-core oversubscribed magnifier and are not quoted anywhere in this
lane. Nothing here was measured on an oversubscribed shape.

**The counter is the proof of mechanism, not the tearing itself.** `atomic_fanout_cuts` advanced by
one per read on the clean bare arm (+118,916) and by **zero** on the tearing in-EXEC arm: the MULTI
children provably never entered the read-cut machinery at all. Note the bare-arm counter is itself
timing-dependent — it is only bumped when the global tracking word read zero at prepare — which is
exactly why this lane added an unconditional counter for the EXEC population (§5).

---

## 2. Root cause

A `MULTI` child reaches the shard owner with the **unbound** read context:

```
src/cmd/multi.inc:607  (execute_local_command)
src/cmd/multi.inc:620  (execute_cross_command)
    shard.store().atomic_set_read_context(UINT64_MAX, state.origin_conn_id);
```

`UINT64_MAX` means "newest committed at the instant *this fragment* runs". A cross-shard read
resolves each fragment on that fragment's own owner at that fragment's own moment, so a foreign
transaction that publishes its one ticket (`multi.inc:1352`, `atomic_commit_group`) while the fan-out
is in flight is seen by the fragments that run after it and missed by the ones that ran before.

Children are lowered with `force_atomic = true` (`multi.inc:351`), and `!force_atomic` is a term of
`needs_snapshot` in `xshard_prepare`, so a child never draws a cut and never registers one. The
sibling lane (`NOTES-EXECATOMIC.md` §7) reproduced this and shelved it, naming two blockers: the
isolation-level question, and the fact that a `MultiExecState` carries its **own** `ScatterArenaPool`
while the cleanup-floor slot is per **IO thread** (`Server::atomic_read_floors_`), owned exclusively
by that thread's pool. Registering through the transaction's own pool would clobber the IO loop's
published floor.

---

## 3. The rule implemented, and what EXEC does and does not guarantee

The brief's decision rule, followed literally rather than picking a database isolation level:

> **EXEC must never be weaker than the same commands executed bare.**

**What EXEC guarantees after this lane**

- A read inside `EXEC` whose fragments span more than one owner is **individually atomic**: it
  answers from one cut, so it can never mix two generations of a foreign transaction. That is
  exactly the guarantee the same command already had when run bare.
- The reading connection still sees **its own** writes — plain writes made earlier on the
  connection, and writes made earlier **in the same transaction** — through the read context's
  `origin_conn_id` overlay. Both are asserted arms of `tests/execiso.py`.
- A transaction that committed **before** the reader's `EXEC` was issued is **fully visible** to it.
  A cut is a cut, not a freeze. Asserted arm.
- Both `--atomic` modes behave identically here. The knob never gated `MULTI`/`EXEC`.

**What EXEC does NOT guarantee after this lane — stated plainly**

- **The transaction as a whole is not a snapshot.** Reads inside `EXEC` are individually atomic;
  they are not serialized as a group against everything else. Two *different* commands inside one
  `EXEC` may disagree about a concurrently committing transaction — a single-owner read (classified
  `Local`) still resolves at "newest committed now", and only multi-owner read fragments carry the
  cut. That is deliberate: a bare single-owner read carries no cut either, so keeping it that way is
  what the rule demands and nothing more.
- **Write visibility is unchanged.** `prepare_write_key` still clones from the newest committed
  value (`multi.inc:519`, `:548`) on purpose: a read-modify-write inside `MULTI` must not overwrite
  a concurrent commit from a stale base. So `EXEC` is *not* snapshot-isolated; the writes remain
  read-committed and only the multi-owner reads are pinned.
- The cut is drawn once per transaction at `EXEC` dispatch, so a transaction carrying several
  multi-owner reads sees one world for all of them. That is *stronger* than bare, not weaker, so it
  satisfies the rule; it is recorded here because it is a real behavioural fact, not because the
  rule required it.
- Nothing here promises anything about `WATCH`, about ordering between connections, or about
  durability.

---

## 4. The fix

Four hunks, none of which change write visibility.

| Where | Change |
| --- | --- |
| `src/cmd/xshard.h` | new `struct SnapshotCut` — the four read-cut bookkeeping fields (`snapshot`, `snapshot_slot`, `snapshot_registered`, `snapshot_complete`) factored out of `ScatterState`. `ScatterArenaPool::unresolved_snapshots_` becomes `vector<SnapshotCut*>`; `register_snapshot` / `unregister_snapshot` become public and take `(SnapshotCut*, owner_io)`. |
| `src/cmd/scatter_engine.inc` | `struct ScatterState : SnapshotCut` (member names unchanged, so every existing `state->snapshot` reference still resolves); three call sites pass `owner_io`. |
| `src/cmd/atomics_glue.inc` | the floor arithmetic now works on `SnapshotCut*` and takes `owner_io` as a parameter instead of reading it off `ScatterState`. |
| `src/cmd/multi.inc` | `MultiExecState : SnapshotCut`; `exec_read_fanout` set at prepare; register/draw at `multi_dispatch_entry`, unregister at `multi_retire_entry`; `multi_read_cut()` bound for the read kinds in `execute_cross_command`; the cut stamped onto `Xshard` read children; the `DEBUG ATOMIC-FANOUT-DEFER` park; an abort guard in `destroy_multi_state`. |

That resolves the sibling lane's mechanical blocker exactly as it predicted: the transaction now
registers **through the connection-owning IO thread's `scatter_pool_`**, which is the one object
allowed to write that thread's floor slot, and the child `ScatterState` merely *borrows* the
published cut (`snapshot_registered` stays false on the child, so `xshard_destroy` never
double-unregisters it).

**Bracket.** Register on IO before any fragment is posted — `register_snapshot` publishes the floor
synchronously and performs the existing hazard-pointer handshake, so no owner can resolve under a
cut whose predecessors cleanup was already free to collapse. Unregister at ROB retirement, which is
after the last fragment has answered (the final reply is only assembled once every participating
owner has reported). `destroy_multi_state` aborts if a state ever reaches destruction still
registered — that would pin this IO thread's cleanup floor to a dead transaction and stop cleanup
advancing server-wide.

**Scope — multi-owner read fragments only.**

- `Mget` / `Exists` (which includes `TOUCH`) are only classified as cross commands when the key set
  already spans more than one shard, so reaching `execute_cross_command` *is* the fan-out test.
- An `Xshard` child qualifies on `nsub > 1` and the absence of `Write`/`SnapshotWrite`.
- `MSET`/`MSETNX`/`DEL` keep `UINT64_MAX`, and `execute_local_command` (single owner) is untouched.
  The same-owner arm already read zero torn, so widening would buy no isolation and would cost every
  such transaction a floor registration.

**Cost when the feature is not needed.** A transaction with no multi-owner read takes one
predicted-not-taken test on `exec_read_fanout` and registers nothing. Two arms of the battery assert
this by counter: a same-owner in-EXEC read and a write-only transaction must both move
`atomic_exec_read_cuts` by exactly **0**.

**Backpressure.** The unresolved-cut window is 8 per IO thread. If it is full the `EXEC` is not
allowed to proceed cut-less (that would silently restore the defect); it takes
`set_atomic_backpressure(true)` and is re-dispatched from `session->pending` on a later pass. No new
wedge risk: the existing clear condition at `io_loop.h:1469` already tests
`scatter_pool_.can_register_snapshot()`. The path is heavily exercised — the perf cell below runs
128 in-flight transactions against a window of 8 on two IO threads.

---

## 5. Counters

`atomic_exec_read_cuts` (`INFO stats`, `src/core/server.h`) — one per transaction that published a
read cut because it carries a multi-owner read. It is **separate from `atomic_fanout_cuts`** for a
specific reason: `atomic_fanout_cuts` is also moved by ordinary bare traffic, so a gate row asserting
only that counter could be satisfied by traffic that never went near `EXEC`. `note_atomic_exec_read_cut()`
bumps both, so the brief's literal request (`atomic_fanout_cuts` advances on the in-EXEC arm) holds
and the battery's own assertion is the non-vacuous one.

---

## 6. After the fix

Same probe, same geometry, freshly built `./build/tomokv` (byte-identical to the binary every result
below was taken on — verified with `cmp`).

| arm | reads | torn | rate | `atomic_fanout_cuts` |
| --- | ---: | ---: | ---: | ---: |
| **`--atomic 0`** | | | | |
| bare `MGET` + writer | 118,983 | 0 | 0.000% | +0 |
| **in-EXEC + writer** | 49,273 | **0** | **0.000%** | **+49,273** |
| in-EXEC, no writer | 85,394 | 0 | 0.000% | +85,394 |
| in-EXEC, same-owner | 49,370 | 0 | 0.000% | **+0** (out of scope, pays nothing) |
| **`--atomic 1`** | | | | |
| bare `MGET` + writer | 118,319 | 0 | 0.000% | +0 |
| **in-EXEC + writer** | 48,345 | **0** | **0.000%** | **+48,345** |
| in-EXEC, no writer | 84,774 | 0 | 0.000% | +84,774 |
| in-EXEC, same-owner | 48,227 | 0 | 0.000% | **+0** |

`0.744% -> 0` and `0.554% -> 0`, and the counter went from `+0` to one per read. Both directions
moved, which is what makes this a fix rather than a quieter test.

---

## 7. The gate row: red before, green after, with the counter asserted

`tests/execiso.py`, wired into `tests/gate.sh`'s armed debug-surface loop (both atomic-mode boots;
the battery flips `atomic` itself so either boot covers both). `DEBUG ATOMIC-FANOUT-DEFER` was
extended to park **MULTI-child** fragments: every fragment of a transaction except the one on its
lead shard is re-queued until the deadline. Placed **after** the watch phase on purpose — that phase
is an all-participants barrier, so parking ahead of it would hold the lead back too and no fragment
would run early. A park, not a stall: `Retry` re-queues on `multi_retries_` and the owner returns to
its queue, so the foreign transaction really does run and commit *inside* the read.

| arm | unfixed `HEAD dbef14d43` | control: fix reverted, hook+counter kept | this branch |
| --- | --- | --- | --- |
| deterministic in-EXEC straddle `MGET`, atomic 0 | FAIL (hook absent: 0/3 windows opened) | **3/3 rounds torn**, 3/3 windows opened | 0/3 torn, `exec_read_cuts=+3` |
| … `EXISTS`, atomic 0 | FAIL | **3/3 torn** (7 of 8 exist) | 0/3, `+3` |
| … `SUNION` (Xshard child), atomic 0 | FAIL | **3/3 torn** (both generations) | 0/3, `+3` |
| … `MGET` / `EXISTS` / `SUNION`, atomic 1 | FAIL | **3/3 torn each** | 0/3 each, `+3` each |
| bare reference under the same park | ok | ok | ok |
| stress arm, atomic 0 | **torn=212**, `exec_read_cuts=+0` | **torn=230**, `+21,600` | torn=0, **`+21,613`** |
| stress arm, atomic 1 | **torn=247**, `+0` | **torn=202**, `+21,635` | torn=0, **`+21,799`** |
| battery exit | **8 checks failed** | **8 checks failed** | **passed** |

Control binary = this branch with the two expressions that hand the cut to the fragments reverted,
hook and counters kept, so the comparison isolates one thing: whether the fragments are bound to the
cut. Its torn samples are the signature shape —
`[b'g0-old', b'g0-new', b'g0-new', b'g0-new', b'g0-new', b'g0-new', b'g0-new', b'g0-new']`: exactly
one fragment (the lead, which was never parked) from before the ticket and seven from after.

Transcripts: `execiso-head.txt`, `execiso-control.txt`, `execiso-final.txt` under
`/tmp/claude-1000/execiso/`, reproduced with `scratchpad/execiso/lane.sh` + `tests/execiso.py`.

**Non-vacuous by construction.** Every armed arm asserts (a) the geometry really fans out
(`DEBUG SHARD` on this boot's hash seed), (b) the window really opened — the foreign transaction's
reply landed before the parked read's and the read was held for a real fraction of the park, with an
unarmed control that completes in under 1/4 of the budget, and (c) `atomic_exec_read_cuts` advanced.
A row that printed PASS while the counter stayed 0 would be the vacuous-validation failure this
program keeps hitting; that is why the counter is asserted in **both** modes, and why it is a
counter that only the EXEC path can move.

---

## 8. Performance

INDICATIVE, loopback, server cores 0-5 (`2:4`, 16 shards), loadgen cores 6-7. Cell =
`MULTI / MGET <one key per shard> / EXEC`, 128 in-flight transactions, 10 s, interleaved A/B
(`scratchpad/execiso/ab.sh`), one server at a time, stopped by listener pid.

| pair | atomic | HEAD txn/s | this branch txn/s | Δ |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 0 | 47,547 | 47,812 | **+0.56%** |
| 2 | 0 | 47,425 | 47,793 | **+0.78%** |
| 3 | 0 | 47,546 | 47,717 | **+0.36%** |
| 4 | 0 | 47,560 | 47,853 | **+0.62%** |
| 1 | 1 | 47,582 | 47,832 | **+0.53%** |
| 2 | 1 | 47,553 | 47,890 | **+0.71%** |
| 3 | 1 | 47,579 | 47,867 | **+0.61%** |
| 4 | 1 | 47,587 | 47,824 | **+0.50%** |

**No measurable cost on the EXEC path — eight of eight pairs land between +0.36% and +0.78%.** The
sign is consistently positive and the magnitude is at the edge of this cell's resolution, so the
honest reading is "free", not "faster". Control shapes, 3 interleaved pairs per mode: a transaction
with **no** fan-out read (`MULTI / GET k / EXEC`, registers no cut) moved −1.2%..+4.3%, centred on
zero; the **bare** `MGET` cell is loadgen-bound at this geometry (the same binary produced 362k and
643k txn/s in adjacent cells) and is reported as **not measurable here** rather than as a result.

**A/A control, because this cell needed one.** The first attempt used 8 keys and swung **−42% to
+72% with the same binary in both arms**. Cause: the hash seed is redrawn at every boot, so 8 keys
land on a different shard→executor split each time, and the cell settled onto four discrete levels
(≈55k / 67k / 77k / 96k) purely by which draw a boot got. Covering **one key per shard** gives every
executor the same fragment count on every boot; the A/A control then reads +0.28 / −0.20 / −0.04 /
+0.13 %, which is what makes the table above readable at all. Recording it because the trap is
generic: any A/B on this tree that re-picks keys across reboots is measuring the hash seed unless the
key set covers every shard.

---

## 9. Validation tally

| Suite | atomic 0 | atomic 1 |
| --- | --- | --- |
| `execiso`, `execatomic`, `multi_exec`, `atomfix`, `atomic_ryow`, `ryow`, `torture`, `lua_scripting`, `scriptatomic`, `session_monotonic`, `debug`, `limits`, `resp3`, `tracking` | 14/14 pass | 14/14 pass |
| shutdown invariants (`live_conns=0 rob_not_quiesced=0 unsent_bytes_pending=0`) | pass | pass |
| `atomic_torn` | **pre-existing flaky, see below** | same |
| `differ.py` vs vanilla redis 7.4: `multi` (new suite) seeds 1-4 | **0 diffs ×4** | see §10 |
| `differ.py`: `xshard` / `string` / `script`, seeds 1-2 each | **0 diffs ×6** | **0 diffs ×6** |
| ASAN+UBSAN (`make asan`, `libasan.so.8` + `libubsan.so.1` confirmed by `ldd`): `execiso`, `execatomic`, `multi_exec`, `atomfix`, `atomic_ryow`, `ryow`, `torture` | clean, 0 diagnostics | clean, 0 diagnostics |
| ASAN+UBSAN: `differ.py multi` and `xshard` | clean, 0 diffs | — |

`atomic_torn` fails on this branch **and on unfixed HEAD**. Four of its failures are
`no conditional cross-shard keys found` on both binaries — a precondition the 6-thread lane geometry
cannot satisfy. The fifth is its `OFF control exposes impossible SINTERSTORE image` arm, a *negative
control that must observe an anomaly*; unfixed HEAD produced `invalid=1` in 1 of 3 runs and
`invalid=0` in the other 2, i.e. the arm is a flaky control on this geometry, not a regression.
`SINTERSTORE` is a write and is untouched by this lane.

The shipped binary was rebuilt from the committed tree and `cmp`-verified byte-identical to the
binary all of the above was measured on.

---

## 10. SHELVED — four pre-existing defects this lane found and did not fix

Writing `tests/differ.py`'s new `multi` suite (the brief asked for one; none existed) surfaced four
divergences from vanilla redis 7.4. **Every one of them reproduces on the unmodified
`HEAD dbef14d43` binary**, and the counts on this branch are statistically indistinguishable from
HEAD's (e.g. RMW divergence over 3 seeds: HEAD 722 / 612 / 667 diffs, this branch 710 / 663 / 684).
They are in write-visibility and list code that this lane's brief explicitly puts out of scope.

**(a) A later access inside one `EXEC` does not observe an earlier write from the same `EXEC`
(`--atomic 1`). ROOT-CAUSED.** `atomic_resolve_internal`
(`src/store/flatstore_atomic.inc:563`) ranks candidates by epoch:

```cpp
if (!winner_set || epoch >= winner_epoch) { winner = candidate; winner_epoch = epoch; ... }
```

A transaction's own still-private candidate carries **epoch 0**. It is admitted past both guards by
`own_committed` (same `origin_conn_id`), and then **loses the winner comparison to any older but
COMMITTED version of the same key**, whose epoch is non-zero. So the second touch of a key inside one
transaction — and a plain read after that transaction — answers from before it. It needs the key to
already carry a live MVCC entry, which is why it is `--atomic 1`-only and why a clean-key minimal
case does not show it. Symptoms seen: `INCRBY`/`APPEND`/`RPUSH` cloning from a stale base
(`[…,'INCRBY k 5', …, 'INCRBY k 4']` → target `-7` then `-8`, oracle `-7` then `-3`); `DEL` counting
one key too few after a `SET` in the same transaction; an in-EXEC `MGET` returning the pre-transaction
value for a key the same transaction just `SET`. A one-line-shaped fix exists (rank an own
uncommitted candidate above every committed one) but it sits in the frozen MVCC resolver, changes
every atomic path rather than just `MULTI`, and wants its own lane, battery, differ and A/B.
Reproduce: `scratchpad/execiso/narrow.py 7080 7081 1 incrby,append,rpush exec 700` (~700 diffs) and
`… 1 del,set exec 700` (~25 diffs), against any tomokv booted `--atomic 1` with a redis 7.4 oracle
on 7081.

**(b) `FlatStore::atomic_finish_group_install` aborts on a version-bytes gauge underflow
(`--atomic 1`).** `src/store/flatstore_atomic.inc:164`,
`if (atomic_version_bytes_ < installed_bytes) std::abort();`. Identified by building every `abort()`
in the engine with a printed tag (`TOMO-ABORT FSA:164`) — the raw gdb frame is misleading, because
`scatter_engine.inc` carries `#line 34 "src/cmd/xshard.cc"` and every backtrace line in it is
remapped by −26 into a file that is 60 lines long. Unfixed HEAD aborts in the same frame. Reproduce:
`BIN=<binary> bash scratchpad/execiso/abortrepro.sh` (with a redis 7.4 oracle on 7081); it aborts
within one or two rounds, on HEAD and on this branch alike.

**(c) `RPUSH` of a large (96-byte) element inside `MULTI` is lost, both atomic modes.** The list ends
up one element short with no reply diff at the `RPUSH` itself. This was the single largest source of
`multi` differ diffs; excluding lists took HEAD from 79/0/62/72 diffs to 0/0/0/0 at `--atomic 0`.

**(d) `LCS` inside `MULTI` returns `ERR internal cross-shard completion error`** for two keys on
different shards; bare `LCS` on the same keys answers correctly. Reproduces on HEAD.

**Consequences for the shipped `multi` differ suite.** It is green at `--atomic 0` — 0 diffs over
seeds 1-6 on **HEAD** and 0 diffs over seeds 1-4 on this branch, ~4,200 ops each. It cannot be made
green at `--atomic 1` by any composition, because defect (a) makes *any* read after an `EXEC` write
on the same key divergent. The suite therefore does not generate lists (c), string read-modify-write
(a), a key touched twice inside one transaction (a), or a key duplicated inside one command; each
exclusion is commented in `gen_multi` with the reproducer that re-opens it. `differ.py` is not run by
`tests/gate.sh`, so nothing red is being wired into a gate. The lane's `--atomic 1` differ evidence
rests on `xshard`, `string` and `script`, all 0 diffs.

---

## 11. Files

| File | Change |
| --- | --- |
| `src/cmd/xshard.h` | `struct SnapshotCut`; pool holds `SnapshotCut*`; register/unregister public, take `owner_io` |
| `src/cmd/scatter_engine.inc` | `ScatterState : SnapshotCut`; call sites pass `owner_io` |
| `src/cmd/atomics_glue.inc` | floor arithmetic over `SnapshotCut*`, `owner_io` as a parameter |
| `src/cmd/multi.inc` | the fix: `exec_read_fanout`, cut register/draw/propagate/unregister, `multi_read_cut()`, the MULTI-child park, the destroy-time invariant |
| `src/core/server.h` | `atomic_exec_read_cuts_` counter |
| `src/cmd/t_server.cc` | `atomic_exec_read_cuts` in `INFO stats` |
| `tests/execiso.py` | new battery: deterministic in-EXEC straddle + bare reference + 6 controls |
| `tests/differ.py` | new `multi` suite (see §10 for its scope and why) |
| `tests/gate.sh` | `execiso` joins the armed debug-surface loop, both atomic-mode boots |
| `scratchpad/execiso/` | lane harness: `lane.sh` (listener-pid boot/stop), `repro.py`, `execbench.py`, `ab.sh`, `sweep.sh`, `differ.sh`, `narrow.py` (the shelved-defect bisector), `abortrepro.sh` |

No config knob was added: a correctness contract must not be optional, and `--atomic` already means
something narrower than its name suggests. `tomokv.conf` therefore needed no change.

## 12. Harness note

Every server in this lane was started and stopped through `scratchpad/execiso/lane.sh`, which
resolves the pid **from the listener** (`ss -lntpH`), refuses to boot onto a live listener, and
verifies the listener is released before the next boot. Nothing was ever selected by name or by
pattern. Cores 0-5 server / 6-7 loadgen and oracle, ports 7080 and 7081 only, `make -j4`. Lane
artifacts live under `/tmp/claude-1000/execiso/`.

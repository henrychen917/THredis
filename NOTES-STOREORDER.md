# NOTES-STOREORDER — a cross-shard store's second wave could overtake older ops of its own connection

Lane `t-storeorder`. Brief: the H2 sighting in `NOTES-DOUBLES2.md` — on a long pipelined stream,
reads of a cross-shard store's destination appear shifted in time relative to the stores.

**Outcome: shipped.** The sighting is real, it is **not** the `t-multires` family, and the
mechanism is one level below the MVCC machinery: a two-hop store publishes its **second wave** of
owner tasks from an **EX** thread while the connection's ordinary ops reached that same owner
through the **IO** thread's inbox channel. `ThreadCtx::drain_tasks()` visits inbox channels in
producer-thread-id order, so nothing orders the two, and a destination install can execute before
an older op of the same connection that is still sitting in the io channel. Repaired at the
dispatch site with the lowering the blocking path already uses; bounded with the full battery
matrix in both atomic modes, the differ matrix over three seeds in both modes, a liveness matrix,
ASAN, and an interleaved performance A/B.

Resources used: cores 96-111, ports 7610 (target) and 7611 (redis 7.4.2 oracle) only. Every server
was addressed by the pid resolved from its listening socket.

---

## 1. PRE vs POST

`PRE` = `perthread-locality` 329fa10ec, `POST` = this branch. Same build flags; only the io_loop
hunk differs. Detector is `tests/storeorder.py` unless stated.

| Probe | PRE | POST |
|---|---|---|
| **Matched arms: `tests/storeorder.py` failures, default placement, `--shards 64 --ratio 6:10` on 16 cores, 120 fresh boots × 15 000 store cycles per arm** | **7 / 120 boots FAIL, 21 violations** (1.8 M cycles) | **0 / 120 boots, 0 violations** (1.8 M cycles) |
| Violations per failing boot | 10, 4, 2, 2, 1, 1, 1 | — |
| Matched exposure, worst forced placement (`--place` ex-ids-first, 6 boots × 60 000 cycles) | **6 violations, 3 of 6 boots FAIL** | **0 violations, 0 of 6 boots FAIL** |
| Boots affected, `--place ex@96..105,ifid@106..111` (4 500 cycles/boot) | **7 / 100** | **0 / 100** |
| Boots affected, `--place ifid@96..101,ex@102..111` (same load) | **0 / 100** | 0 / 100 (not re-run: PRE is already immune, see §4) |
| Interleaved A/B, fresh boot per leg, 60 boots per arm | **4 / 60 boots FAIL** | **0 / 60 boots FAIL** |
| POST-only sweep, 6 configs × 25 boots × 4 probes | — | **0 / 150 boots** |
| `tests/differ.py … storeorder` on a barrier-removed control build | 1501-1525 diffs, FAIL | — |
| Directed batteries (34 batteries × both atomic modes) | — | **68 / 68 PASS** |
| Differ matrix (19 suites × 3 seeds × both modes, RESP2) | — | **114 / 114 PASS** |
| Differ `storeorder` RESP3 (2 seeds × both modes) | — | **4 / 4 PASS** |
| Liveness matrix (1/4/16 conns, ±WATCH, both modes) | 16/16 clean | **16 / 16 clean** |
| ASAN build: 11 batteries + differ storeorder × 2 seeds | — | **13 / 13, no sanitizer report** |
| A/B `get_p32` (INDICATIVE loopback) | 452 668 ops/s | 450 552 ops/s (**−0.5 %**) |
| A/B `set_p32` | 597 488 ops/s | 592 391 ops/s (**−0.9 %**) |
| A/B `mget2_p32` (cross-shard read) | 185 071 ops/s | 183 911 ops/s (**−0.6 %**) |
| A/B `del2_p32` (single-wave cross-shard write) | 400 243 ops/s | 399 724 ops/s (**−0.1 %**) |
| A/B `zdiffstore` pipeline (stores only) | 46 144 ops/s | 45 615 ops/s (**−1.1 %**) |
| A/B `mixed_store` (3 plain ops + 1 two-hop store, pipelined) | 174 800 / 159 639 / 173 964 | 101 235 / 100 632 / 101 630 (**−40 %**) |

Everything except the last row is inside run-to-run spread; the `mget2`/`del2`/`zrangestore` cells
are bimodal across boots (both arms show both modes) so they are reported as the median of three
interleaved legs. The **−40 % `mixed_store` row is real, consistent 3/3 legs, and bought
deliberately** — see §6. Plain `GET`/`SET` never reach the changed code at all: it sits behind
`scatter_dispatch.barrier`, which is false for every command that is not a two-hop scatter.

---

## 2. Reproduction, and the correction to the sighting

### What I could not reproduce

The sighting's own reducers (`window.py`, `opbisect.py`, `racegen.py`, still in the shared lane
scratchpad) no longer reproduce anything: the committed `doubles` generator was changed, in the
same lane, to give every store a unique destination that is never read back, which is exactly the
observable the anomaly needs. Rebuilding the pre-fix generator (destinations drawn from a pool of
8, each store followed by `ZRANGE dst` + `DEL dst`) and replaying it — the full 5 000-op stream and
the 3100:3260 window ×40 in one write, with every float spelling narrowed to ones **both** binaries
accept so that no store is refused at the parser — gave **0 violations in 8 configurations ×
25-60 fresh boots each**. `racegen.py` against this branch's base likewise diverges only on the
`1e+126` formatter rows that `t-doubles2` repaired.

So the sighting's *recipe* is dead, but its *observable* is not. I rebuilt the observable directly.

### What does reproduce

`tests/storeorder.py` drives, on one connection and in ONE pipelined write, cycles of

```
<build a source>        e.g. SADD src m<n>
<STORE dst …>           -> a reply that is a known function of the destination content
<read dst>              -> must agree with that reply
DEL dst                 -> :1
<read dst>              -> :0
```

over all 18 members of the family. It is oracle-free: every expectation is derived from the
target's own earlier reply, so no float text and no collection ordering can move it.

Measured rate, `--place ex@96..105,ifid@106..111`, `--shards 64`, on PRE:

```
6 boots x 60 000 store cycles:  1, 3, 0, 0, 2, 0  failures   -> 6 in 360 000 cycles  (~1.7e-5/cycle)
```

and on PRE at the **default** placement, `--shards 64 --ratio 6:10`, 120 fresh boots ×
15 000 cycles: the battery failed on **7 boots**, with per-boot violation counts of
10, 4, 2, 2, 1, 1, 1 and 113 boots at zero. (The enclosing sweep marked 11 boots as affected; the
other 4 were flagged by a companion probe whose output I did not attribute, so only the 7 the
battery itself failed on are counted above.)

### What distinguishes an affected boot

That distribution is the answer, and it is not Poisson: at a uniform rate, 21 violations over 120
boots would give λ≈0.18 per boot and a boot with **ten** of them essentially never. The counts are
strongly over-dispersed, so **the boot decides how exposed a run is** — which is the sighting's
"an instance is either affected or not", measured. §4 names the boot-time variable: whether the io
thread that owns the connection has a **higher** thread id than the ex thread that finishes the
store's last phase-1 fragment. Forcing that inversion with `--place` moves the rate by two orders
of magnitude and forcing the opposite makes the defect unreachable — 0/100 boots vs 7/100 boots on
the same binary, same load, same cores.

The sighting also saw a *within-run* amplification the battery does not: its window replay hammered
the same destination 40 times, so an unlucky triple fired on every repetition. Adding the same
triple reuse to the battery (`--focus`, default 24 cycles per triple) did not raise the per-cycle
rate here, so it is kept only because it costs nothing and matches the original shape.

A typical failure, with the destination's whole timeline (the battery prints it):

```
op 3193 ['SDIFFSTORE', 'so:k16#d', 'so:k54#s', 'so:k49#t'] -> :1
op 3194 ['SCARD',      'so:k16#d']                         -> :1      <- correct
op 3195 ['SMEMBERS',   'so:k16#d']                         -> {m1}
op 3198 ['SDIFFSTORE', 'so:k16#d', 'so:k54#s', 'so:k49#t'] -> :2
op 3199 ['SCARD',      'so:k16#d']                         -> :3      <- from the FUTURE
op 3200 ['SMEMBERS',   'so:k16#d']                         -> {m3,m2,m1}
op 3203 ['SDIFFSTORE', 'so:k16#d', 'so:k54#s', 'so:k49#t'] -> :3
op 3204 ['SCARD',      'so:k16#d']                         -> :3
shards: so:k16#d=26 so:k49#t=57 so:k54#s=11
```

`m3` does not exist until op 3202. The reads at 3199/3200 answered from after the store at 3203, an
op they precede on the same connection — and that is the sighting's shape exactly: the destination's
timeline is shifted relative to the stores, in both directions (a read that is too new here, and in
the `DEL` half of the cycle a delete that lands after a younger store, which is the `store :1` then
`read *0` the sighting transcribed).

Every violation I captured was a **two-source** store (`SDIFFSTORE`, `SINTERSTORE`, `SUNIONSTORE`,
`ZDIFFSTORE`), which is consistent with §4: two phase-1 fragments widen the interval between the
last fragment finishing and phase 2 landing.

---

## 3. It is NOT the multires family

The brief's lead was that this is `t-multires` reaching the store path. It is not, and the repair
there could not have covered this:

* **Different layer.** Multires was a *ranking* defect: two units of one connection carried commit
  tickets in the opposite order to program order, and both MVCC rankers used the raw ticket. This
  defect happens with **no MVCC records in existence at all** — it reproduces at `--atomic 0`,
  where the store family creates no atomic group, sets no `atomic_hazard`, and never enters
  `atomic_resolve_internal()` or `atomic_collapse()`.
* **Different question.** Multires asked "which of two committed versions wins". This asks "did the
  older op execute first" — the install is already committed and singular when the read runs; the
  read simply ran too late.
* **Why the existing gate cannot see it.** `xshard_task_should_defer()` returns immediately unless
  `op.atomic_hazard()` is set, and that bit is only set while a cross-shard **atomic group** is in
  flight on the connection (`Client::atomic_groups_io_`, raised only on the bundled `atomic_write`
  dispatch arm). `has_parked_predecessor()` inspects `atomic_deferred_` and `xshard_retries_` — two
  queues of tasks the owner has already *taken*. Neither can see a task still sitting in another
  producer's inbox, which is where the older op is.

---

## 4. Mechanism

A two-hop store is dispatched as one scatter whose **phase 1** reads the sources on their owners.
When the last phase-1 fragment finishes, `publish_phase2()` runs **on that EX thread** and posts the
destination install:

```
src owner (EX thread S):  server.thread(D).post_tasks_quiet(self.id(), posts, …)
```

so the install enters owner `D` through **channel[S]**. The connection's ordinary ops entered
through **channel[I]**, where `I` is the io thread that owns the connection. `ThreadCtx::drain_tasks()`
walks the notify mask and drains channels in ascending producer id, fully draining channel *p*
before channel *p+1*. There is no order between channel[I] and channel[S]: if `S < I`, a drain that
finds both non-empty executes the younger install **before** the older op.

The connection barrier does not close this. `ScatterState::barrier` (set for every two-hop store)
stops the parse pass so nothing *younger* is dispatched — but ops parsed *before* the store in the
same pass are still in flight when the store is dispatched, and those are the ones that get
overtaken.

**The direct evidence is the thread-id order.** `--place` fixes it, because
`Placement::build_explicit()` appends threads in spec order:

| PRE, `--shards 64`, 4 500 store cycles per boot | boots affected |
|---|---|
| `ifid@96..101,ex@102..111` — every io id **below** every ex id | **0 / 100** |
| `ex@96..105,ifid@106..111` — every ex id **below** every io id | **7 / 100** |

Nothing else differs between those two arms — same binary, same shard count, same load, same
cores. Fisher's exact on 0/100 vs 7/100 is p ≈ 0.007.

**Why it is boot-dependent in the field.** The default placement lays threads out *per L3 domain*,
io-then-ex within each domain (`Placement::build_ratio()`), so on a box with one L3 domain every io
id is below every ex id and the inversion cannot happen; with two or more domains it happens
routinely. This lane's own gate boot prints it:

```
thread t5: role=ex   cpu=101 L3=0
thread t6: role=ex   cpu=102 L3=0
thread t7: role=ex   cpu=103 L3=0
thread t8: role=ifid cpu=104 L3=1     <- io id 8 sits ABOVE ex ids 5,6,7
```

Whether a given command is exposed then depends on which io thread accepted the connection, which
ex thread owns the last phase-1 fragment, and which ex thread owns the destination — all of which
move with the per-boot hash seed and the shard→thread map. The sighting's 16-core boot
(`--shards 16 --ratio 6:10` on cores 96-111, two L3 domains) is squarely in the exposed set; an
8-core single-domain boot is not, which is why `--shards 1` and several of its fresh instances
looked clean.

### Which commands are affected

Exactly the commands that publish a **second wave of tasks from an EX thread**, and that set is
exactly `ScatterState::barrier`:

* two-hop stores — `ZRANGESTORE`, `ZUNIONSTORE`/`ZINTERSTORE`/`ZDIFFSTORE`,
  `SINTERSTORE`/`SUNIONSTORE`/`SDIFFSTORE`, `SORT … STORE`, `GEOSEARCHSTORE`/`GEORADIUS … STORE`,
  `COPY`, `BITOP`, `PFMERGE`, `RENAME`/`RENAMENX` (at `--atomic 0`), `SMOVE`, `LMOVE`,
  `RPOPLPUSH`, `MSETNX` (at `--atomic 0`), `LMPOP`/`ZMPOP` retries, and the cross-owner script
  apply wave;
* **not** `MGET`/`EXISTS`/`TOUCH` (read-only scatters, one wave), **not** `MSET`/`DEL`/`UNLINK`
  atomic groups (`xshard_complete` shows they finish in one phase — every task is posted from the
  io thread), **not** a direct `RENAME` at `--atomic 1` (`atomic_direct` dispatches both hops from
  the io thread at once and gates the destination on `direct_ready`).

That correspondence is what makes the fix's condition exact rather than approximate.

---

## 5. The fix

`src/core/io_loop.h`, at the scatter dispatch site, before anything is posted:

```cpp
if (scatter_dispatch.barrier && rob.in_flight() != 0) {
    xshard_destroy(scatter_dispatch.state, scatter_pool_, self_->id());
    break;
}
```

A barriered scatter now waits for the ROB head before it issues. The barrier it sets afterwards
already keeps younger ops out; this keeps older ones from still being in, so from dispatch to
retirement the connection has exactly one op in flight and the second wave has no same-connection
sibling anywhere to overtake. Cross-connection order is untouched — it was never a hazard.

This is the lowering the blocking path has always used (`if (rob.in_flight() != 0) break;`), and
the teardown is the one the out-of-room path already uses: the frame is left unconsumed, so it is
re-parsed from scratch once the older replies land and the serve pass re-enters
`parse_and_dispatch()`. `xshard_destroy()` documents this exact case ("a group torn down BEFORE
dispatch (no owner ever sees it)") and releases the arena, the snapshot registration, the admission
credit and any AOF group.

**Blast radius.** One hunk, in the io dispatch path. The MVCC resolver, `atomic_collapse()`,
`atomic_resolve_internal()`, the scatter engine's phase machinery and the single-owner rule are all
untouched. `Op` and `Client` are untouched, so the `sizeof(Op)==336` / `sizeof(Client)==1984`
static asserts are unaffected. No new knob: this is a correctness repair on an existing path.

### Alternatives considered and rejected

* **Post phase 2 through the originating io thread's channel.** Would restore FIFO exactly and cost
  nothing in throughput — but `task_in_[p]` is a single-producer channel per producer id (the
  header is explicit that a shared MPSC inbox was measured and rejected), so pushing with a foreign
  producer id is a data race. Dead.
* **Route phase 2 back through the owning io thread (EX → IO → owner).** Correct and keeps the
  pipeline overlapping, at one extra hop of latency per store. It is a real change to the scatter
  engine's completion path — new state, new wake path, new failure modes — and the brief's bar is
  explicit that unbounded changes to that machinery do not ship. Recorded as the follow-up worth
  doing if the `mixed_store` cost ever matters (§6).
* **Defer the arriving phase-2 task on the destination owner behind older same-connection work.**
  The owner cannot see another producer's inbox, so this needs either a per-(shard, connection)
  outstanding-op counter or a per-connection touched-shard bitmap maintained on every dispatch.
  Both put unconditional per-op work on the GET/SET path, which the zero-cost-when-off rule forbids.
* **Narrow the wait to "older in-flight ops that touch the destination's owner".** Would keep most
  of the overlap. It needs the io thread to walk its own ROB window and classify each in-flight op's
  shards, and to answer conservatively for in-flight scatters whose shard set it cannot read without
  racing the executor. More code and more ways to be subtly wrong, for a cell that is already
  serialized; not taken.

---

## 6. The cost, and why it is bought

The only cell that moves is a pipeline that interleaves plain ops with two-hop stores: −40 %,
consistent across three interleaved legs. The shape of the cost is exact — such a store previously
overlapped with the ops in front of it and now does not — and it is confined:

* `GET`/`SET` p32: −0.5 % / −0.9 %, inside spread; the code is behind `scatter_dispatch.barrier`
  and a plain command never reaches `xshard_prepare`'s Ready arm at all.
* Cross-shard reads (`MGET`) and single-wave cross-shard writes (`DEL k1 k2`): −0.6 % / −0.1 %.
  Neither is barriered, so neither pays.
* A pipeline of stores only: −1.1 %. Nothing older is in flight there — the previous store already
  drained the connection — so the new test is taken and answers "no wait" every time.

A two-hop store already forced a full connection quiesce *after* itself; this adds one before it.
In the worst shape that doubles the round trips per store, which is the −40 %. It is an inherent
loss on a path whose ordering was wrong, on commands that are not the main commands, and it goes in
the report rather than being hidden.

---

## 7. Containment

| Gate | Result |
|---|---|
| Matched arms, default placement, `--shards 64 --ratio 6:10`, 120 boots × 15 000 cycles per arm | PRE **7 / 120 boots FAIL (21 violations)**, POST **0 / 120 boots (0 violations)** — 1.8 M store cycles each |
| `tests/storeorder.py`, 6 boots × 60 000 store cycles, worst forced placement | PRE **6 violations / 3 of 6 boots**; POST **0 / 0 of 6** |
| Interleaved PRE/POST A/B, fresh boot per leg, 60 boots per arm | PRE 4/60 FAIL, POST **0/60** |
| POST-only sweep, 6 configs × 25 boots × 4 probes (storeorder battery, storeorder differ, doubles-window replay, raceprobe) | **0/150 boots** |
| Directed batteries — storeorder, multires, atomic_torn, atomic_ryow, atomfix, execatomic, execiso, execfix, multi_exec, concur, ryow, xacct, xscript, scriptatomic, writer_atomic, session_monotonic, s6, xmove, zsetops, lcs, tracking, torture, blockmulti, debug, resp3, limits, geo, edgeproto, lua_scripting, bitfield, blocking, stream, streamgroups, hexpire — × `--atomic 0` and `--atomic 1` | **68 / 68 PASS** |
| Differ — storeorder, multi, xshard, zsetops, scan, string, list, set, zset, hash, geo, bitmap, xmove, edgeproto, cgaps, hll, script, arity, servertail × seeds 7/19/23 × both modes | **114 / 114 PASS, 0 diffs** |
| Differ `storeorder` under RESP3, seeds 7/19 × both modes | **4 / 4 PASS, 0 diffs** |
| Liveness (1/4/16 connections, deep mixed pipelines, ±WATCH, both modes) | **16 / 16 clean**, identical to PRE |
| ASAN+libasan build: storeorder, multires, atomic_torn, atomic_ryow, execfix, execiso, multi_exec, atomfix, zsetops, concur, ryow + differ storeorder × 2 seeds | **13 / 13, zero sanitizer reports** |
| Footprint locks | build succeeds ⇒ `sizeof(Op)==336`, `sizeof(Client)==1984` hold |

**Detector controls.** Two, both fired:

1. *Checker* — `tests/storeorder.py` runs its own reply checker against a hand-built transcript
   carrying one injected instance of each shape it claims to detect, and refuses to run if it
   misses any or flags a clean one (`checker self-test 4/4` in every line above).
2. *Engine* — a build with `ScatterState::barrier` forced false (scratch only, never committed)
   fails the battery **3/3 runs with 332-356 violations per run** and fails
   `differ … storeorder` with **1501-1525 diffs**. A detector that cannot report non-zero proves
   nothing; this one reports 1 000+ when the ordering is genuinely absent, and 0 on the shipped
   build.

**Geometry guard.** The battery resolves every key with `DEBUG SHARD` and reports how many store
cycles actually placed the destination on a different owner from its sources. Zero cross-owner
cycles on a multi-shard boot is reported as a **failure of the test**, not a pass of the server. On
`--shards 1` it says so and marks itself a control leg.

---

## 8. Sensitivity, and the gate-row recommendation

At its defaults (`--reps 10 --cycles 500` = 15 000 store cycles, 0.7 s) the battery caught the
defect on **7 of 120** PRE boots at the default placement — i.e. it flags roughly one boot in
seventeen, because most boots are not exposed at all (§2). On an exposed boot it is much sharper: the
per-cycle rate under the worst forced placement is ~1.7e-5, so 60 000 cycles (`--reps 20
--cycles 1000`, ~4 s) is about 63 % and three such runs about 95 %.

**No gate row was added.** The battery is a defensible row — it fails on the un-repaired binary and
on the barrier-removed control, passes 0/150 boots and 360 000 matched cycles after — but a row
whose single-run sensitivity is 22 % is a row that will eventually be read as flaky, and its rate
was only ever measured on this lane's cores and placements. The honest move is to hand it over:
suggested placement is beside `atomfix`/`execfix` in the armed-boot group of `tests/gate.sh` (it
needs `--enable-debug-command yes` for `DEBUG SHARD`), run as
`python3 tests/storeorder.py 127.0.0.1 $PORT --reps 20 --cycles 1000`, with the expected-check
count bumped by 60 004. `tests/differ.py … storeorder` is the cheaper and fully deterministic
companion row: it is a parity suite against redis 7.4 and reaches 0 diffs on every seed tried.

---

## 9. Also found, not fixed (handed on)

**`blocking_retire()` and `blocking_scatter_retire()` clear the connection's scatter barrier
unconditionally** (`src/cmd/blocking.inc:1275`, `:1286`), with no test that the barrier is theirs.
The barrier is a single bool set by six different owners — a two-hop scatter, a blocking command, a
deferred `WAIT`, `EXEC` (`multi.inc:1365`), a pub/sub transition, and CLIENT PAUSE/KILL
(`climon.cc:524`, `:680`). Today no reachable interleaving lets a blocking op retire while another
owner still needs the barrier — a blocking command requires the ROB head to issue, and everything
else that sets the barrier stops the parse pass — so this is latent, not live, and I could not
build a case that fires it. It is one shared flag away from becoming the same class of defect as
the one this lane fixed, and the obvious hardening (make the barrier a small owner count, or have
`blocking_retire` clear it only when the ROB is quiescent) belongs to whoever owns the blocking
lane. Recorded, not touched.

---

## 10. Files changed

| File | Change |
|---|---|
| `src/core/io_loop.h` | a barriered scatter waits for the ROB head before it issues (one hunk, at the dispatch site) |
| `tests/storeorder.py` | new battery: 18-member store family, oracle-free destination-visibility invariant on one pipelined write, geometry guard, checker self-test, per-key failure timeline |
| `tests/differ.py` | new `storeorder` suite (reused destinations, read back and deleted, byte-compared to redis 7.4) + its deep 512-op pipeline chunk |
| `scratchpad/storeorder/` | lane instruments: `boot.sh`/`stop.sh` (listener-resolved pids), `hunt.sh`/`armhunt.sh`/`abhunt.sh`/`finalsweep.sh` (boot sweeps and the interleaved A/B), `gates.sh`/`differmatrix.sh` (containment), `liveness.py`, `perfab.py`, `raceprobe.py`, `wstream.py`, `jump.py` |

The reconstructed pre-fix `doubles` generator and its replay driver (`gen_doubles_exec.py`,
`dstream.py`) stay in the session scratchpad rather than the tree: they are a reconstruction of
another lane's uncommitted intermediate state, they reproduce nothing on this branch, and
`tests/differ.py … storeorder` covers the same ground deterministically.

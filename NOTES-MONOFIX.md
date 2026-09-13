# NOTES-MONOFIX — two invariant breaks in the epoch-MVCC engine

Branch `t-monofix`, off mainline `bb01e6ce1`. Two independent bugs, two independent fixes; the
orthogonality is proved below with a build that carries only one of them.

| invariant | HEAD (3 x 60s) | fix (3 x 60s) | deterministic arm: HEAD | deterministic arm: fix |
|---|---|---|---|---|
| session monotonicity (`GET a` then `MGET a b…` on one conn) | **542 / 389 / 171** violations in 1.385M / 1.376M / 1.359M batches | **0 / 0 / 0** in 1.314M / 1.341M / 1.344M | `ATOMIC-READ-DELAY 20us`: **6 888** in 425k (1.6%) | **0** in 313k / 328k / 326k |
| cross-shard atomicity (one MGET, one generation) | 0 seen in 855k (owner saw 1 in 1.13M) | 0 | `ATOMIC-COMMIT-DELAY 100us`: **202 652** torn in 211 249 batches (**95.9%**) | **0** in 203k / 195k / 196k |
| guard actually opened (counters) | `atomic_read_cuts_held` 0, `atomic_commit_holds` 0 (no such counters) | `read_cuts_held` +959/+1104/+1225 unarmed; `commit_holds` +197 642/+194 234/+195 942 armed | — | — |

| INDICATIVE p32 loopback, interleaved A/B, 3 pairs/cell (fix vs HEAD) | pair 1 | pair 2 | pair 3 |
|---|---|---|---|
| `--atomic 0` GET | +0.17% | +1.13% | +0.62% |
| `--atomic 0` SET | +0.19% | −0.19% | +0.04% |
| `--atomic 1` GET | +0.08% | +0.28% | +0.01% |
| `--atomic 1` SET | +0.83% | −0.16% | −0.28% |

No cell regresses. The atomic-off path is untouched by construction (one predicted-not-taken test
per parse pass) and measures untouched; the atomic-on hot path is inside ±0.3% median, well under
the 1% budget.

---

## Bug 1 — the read cut was not pinned in program order

### The interleave

A multi-key read pins its epoch on IO **at prepare** and resolves against it later
(`src/cmd/scatter_engine.inc:1230` at HEAD, registered at `:1267`). A plain single-key read pinned
nothing: it sampled the commit sequence **at execution** and answered with the newest committed
world (`src/cmd/atomics_glue.inc:861` at HEAD, and `:772` for the same-owner multi-key arm).

On one connection, with `GET a` pipelined immediately before `MGET a b…`:

```
io   t0  prepare GET a          -> post task to shard(a)'s owner                      [no cut]
io   t1  prepare MGET a b…      -> state->snapshot = S   (scatter_engine.inc:1230)
io   t2                          register_snapshot        (scatter_engine.inc:1267)
ex   t3  foreign atomic MSET commits ticket T,  T > S
ex   t4  GET a executes         -> atomics_glue.inc:861 samples "now" >= T -> answers V(T)
ex   t5  MGET's shard(a) fragment executes at cut S < T -> answers V(T-1)
io   t6  ROB retires in order: the client reads V(T) then V(T-1)
```

Per-shard FIFO is not the problem and never was: t4 precedes t5 on shard(a) exactly as it should.
The EPOCH is what is stale. The earlier reply is newer than the later one — time runs backward
inside one session.

Measured natural rate on HEAD: **0.03–0.07% of batches**, steady across a 60s run (not a startup
transient). Every violation had lag exactly 1 generation.

### The fix

IO stamps every routed read-only op with the commit sequence as of the parse pass that produced it,
and the owner resolves against that instead of "now":

- `src/core/io_loop.h:847-848` — one epoch per parse pass (`pass_read_cut`), taken only when atomic
  tracking is live.
- `src/core/io_loop.h:1049-1052` — the stamp, `op->set_read_cut(pass_read_cut)`.
- `src/exec/op.h:124,156-170` — `read_cut_lo` + the `kReadCut` route-flag bit and the widener.
- `src/cmd/atomics_glue.inc:755` (`plain_read_cut`), used at `:789` and `:879`.
- `src/cmd/scatter_engine.inc:1232` — a multi-key read reuses the op's stamp rather than resampling,
  so a plain read and a multi-key read prepared back to back cannot disagree about which is older.

**Why this is correct without any watermark.** Ops of one connection are prepared by one IO thread
in parse order, and the commit sequence only moves forward, so the stamps are non-decreasing along
the connection **by construction**. Monotonicity needs non-decreasing, not distinct, which is why
one epoch per pass is enough and per-op sampling is not.

**Freshness floor** (the property that makes the staleness legal): a pass starts *after* the bytes
it parses arrived, so no read is ever pinned older than its own arrival. The disjoint-window case —
a writer's `+OK` fully received before the reader's bytes are sent — therefore still sees the write.
Directly tested: `freshness floor rounds=4000 acked-write-not-visible: cross-conn=0 own-conn=0`.

**Read-your-writes** is untouched: `atomic_resolve_internal`'s `own_committed` arm bypasses the
epoch bound entirely for entries whose `origin_conn_id` matches the reader, so a pinned cut can
never hide the connection's own write. The own-undecided defer (`xshard_task_should_defer`, reached
in `ex_loop.h` *before* `xshard_plain_prepare`) still fires first, and a deferred re-run reads the
same stamped field — it never re-pins. `ryow.py` and `atomic_ryow.py` pass in both atomic modes,
release and ASAN.

**Writes deliberately keep "newest".** A write's read is the base of its own update: a stale base is
a lost update (`INCR` would re-produce a value it already produced). Writes are excluded from the
stamp, and that is sound for *monotonic reads* — a write cannot make an earlier-observed value go
backward, because everything it touches is visible to the connection through `own_committed`.

**Blocking commands are excluded too**, and not for cost: `blocking_resume_move()`
(`src/cmd/blocking.inc:1169`) re-prepares a parked op long after arrival, so a cut pinned at first
dispatch would make the resumed read answer from before the write that woke it. They need no cut
anyway — a blocking command is a whole-connection barrier (waits to be ROB head, ends the parse
pass), so nothing younger on that connection is even prepared until it finishes.

### Footprint: `sizeof(Op) == 336` is intact

`read_cut_lo` fills the 4-byte hole between `shard` and `hash`. Every other offset is byte-identical
(`offsetof` mirror-diff, HEAD vs branch):

```
HEAD                              branch
spec=0   shard=8   hash=16        spec=0   shard=8   read_cut_lo=12   hash=16
rbuf_off=24 db=28 route_flags=29  rbuf_off=24 db=28 route_flags=29
reply=32 direct=152 …             reply=32 direct=152 …
state=184  sizeof(Op)=336         state=184  sizeof(Op)=336
```

The hole is 4 bytes, not 8, which is why only the low half of the sequence is stored and the owner
widens it against the live sequence (`Op::read_cut`). The widening is exact while fewer than 2^31
commits separate dispatch from execution — at the highest commit rate this engine has ever produced
that is tens of seconds of queueing — and it saturates to a stale-but-safe (older) cut rather than a
newer one if it ever did. Carrying the full 64 bits would require moving `std::atomic<OpState>
state` out of offset 184 into the 2-byte hole at 30, i.e. a hot-path layout change on the one field
that crosses threads; that has to be re-earned with a 64c A/B and is not worth 4 bytes.
`Task` was the other candidate — it has the same 4-byte hole and no more (`sizeof(Task)==32`).

---

## Bug 2 — a group's ticket became visible before its records did

### The interleave

A cross-shard group installs its versions on every owner while the shared epoch word still reads
zero (undecided ⇒ invisible to foreign readers), and only the last owner turns it into a ticket:

```
src/cmd/scatter_engine.inc:2091 (HEAD)   const uint64_t ticket = server.atomic_commit();
src/cmd/scatter_engine.inc:2092 (HEAD)   state.epoch.store(ticket, std::memory_order_release);
```

`atomic_commit()` was `commit_seq_.fetch_add(1)+1` (`src/core/server.h:654` at HEAD) and
`atomic_snapshot()` returned that same word (`:657`). So between those two lines the sequence
already named a commit whose records still answered "undecided":

```
ex-A  t0  T = commit_seq.fetch_add(1)+1        -> commit_seq == T,  state.epoch still 0
io    t1  reader pins cut S = atomic_snapshot() == T
ex-B  t2  MGET fragment on shard(b) resolves:  entry epoch 0 -> invisible -> V(T-1)
ex-A  t3  state.epoch.store(T)
ex-C  t4  MGET fragment on shard(a) resolves:  entry epoch T <= S -> visible -> V(T)
          => ONE reply, TWO generations
```

The hole is two instructions wide, which is why the owner saw it once in 1.13M batches; a
preemption between them widens it to milliseconds. Both tear directions are reachable depending on
which fragment runs first, and both were observed.

`DEBUG ATOMIC-COMMIT-DELAY 100` makes it deterministic: **202 652 torn out of 211 249 batches
(95.9%)** on HEAD behaviour.

The same shape lived at three more sites: `scatter_engine.inc:2108` (atomic pop),
`xshard_commands.inc:1434` (apply kinds), `multi.inc:1352` (EXEC).

### The fix — a publication watermark

`src/core/server.h:674-720`. Committers bracket `[draw, publish]` in `atomic_commit_inflight_`;
whoever takes the count to zero republishes into `atomic_commit_safe_` the drawn sequence it read
**before its own decrement**. `atomic_snapshot()` returns that watermark instead of the raw drawn
sequence — one load, exactly as before.

Why the value is safe at that instant: any ticket at or below the value we read was drawn before we
read it, and a committer that had not yet stored its epoch would still be holding the count up, so
our decrement could not have returned 1. The reader's acquire on the watermark is ordered after that
`acq_rel` decrement, which is ordered after the release store of the epoch.

Why it never goes stale: **the last committer out always advances it.** When the count reaches zero
the thread that took it there publishes, so whenever the engine is quiescent the watermark equals
the drawn sequence exactly. The staleness is bounded by the duration of one in-flight publication —
two instructions — which is why an acknowledged write is visible to every other connection by the
time that connection's next request arrives (`freshness floor` case: 4000 rounds, 0 misses).

Cost: two RMWs per group commit on the line `commit_seq_` already owns (they are declared adjacent
for exactly that reason, `src/core/server.h:1224-1229`). Readers pay nothing new.

Same-owner tickets (`begin_plain_version`, the localfast multi-key arm, FLUSH's logical clear) have
no window — the shard that draws them installs their records before it yields and no other task may
touch that shard in between — but they travel through the bracket anyway
(`Server::atomic_commit()`, `src/core/server.h:709`) so the watermark can never be left behind a
ticket that is already installed. FLUSH's draw used to reach `commit_seq_` through a raw pointer
inside FlatStore; it now goes through the bracket via a bound ticket function
(`src/store/flatstore_atomic.inc:17-24,430`, wired at `src/core/server.h:155`).

### Orthogonality — proved, not assumed

A probe build carrying **only** the bug-1 fix (program-order read cut) and **not** the watermark,
under `ATOMIC-COMMIT-DELAY 100us`:

```
commit-delay 100us  batches=51148  violations=0  torn=51081  [commit_holds+0]
```

99.9% torn with monotonicity clean. The two bugs are independent and each needs its own fix. The
counters distinguish the paths: `atomic_read_cuts_held` moves for bug 1, `atomic_commit_holds` for
bug 2.

---

## Design alternatives, and why they lose

**The brief's candidate — per-connection watermark folded at retire, `snapshot = max(atomic_snapshot(),
conn watermark)` — is arithmetically vacuous.** The watermark records an epoch some earlier op
observed at time `t_obs`, so `W <= commit_seq(t_obs)`. For the watermark to be visible at a later
op's prepare, the earlier op must have retired, so `t_prepare > t_obs`, and the sequence is
monotone, so `commit_seq(t_prepare) >= W`. The `max()` can therefore never raise the pin — it is a
no-op for every input. Independently, it cannot reach this bug at all: the hammer's client sends
`GET` and `MGET` in one `sendall`, so both are prepared in the same parse pass, long before either
retires. Not implemented; refuted on paper and by the shape of the failing workload.

**Pinning at first-fragment execution does not close it.** The MGET's fragment on the shard it
shares with the GET runs after the GET (per-shard FIFO), but its fragment on any *other* shard can
run before it. Pinning there yields a cut older than what the GET goes on to observe, and pinning
per fragment tears the reply outright.

**Re-pinning the group on discovering a newer connection watermark** requires restarting fragments
that have already gathered values (borrows included) and reintroduces a livelock surface. Rejected.

**Draining the ROB before the epoch pin** is a stall for something that is not a hazard, which the
doctrine forbids.

**For bug 2, publishing the epoch first and then advancing the sequence with a `fetch_max` does not
work**: group X (ticket 5) can still be unpublished when group Y (ticket 6) advances the sequence to
6, and a reader at cut 6 then straddles X's publication exactly as before — the tear simply moves.
Excluding unpublished tickets from the reader's cut is the only shape that closes it, and the
inflight bracket is the cheapest exclusion that keeps the reader at one load.

**A sequencer ring of published tickets** (advance a watermark by scanning slots) also keeps the
reader at one load, but its watermark can wedge permanently if a slot is recycled while its ticket
is still unpublished — a preempted committer plus a few thousand commits is enough. Rejected for the
bracket, which has no unbounded state.

---

## Test surface added

- `tests/session_monotonic.py` — rewritten as the regression. Uncapped counters, asserts 0/0, keeps
  the torn control, adds the freshness-floor and read-your-writes directed case, and runs three
  arms: unarmed, `ATOMIC-READ-DELAY 20us`, `ATOMIC-COMMIT-DELAY 100us`.
  - **Geometry gate.** The hash seed is drawn from the kernel at every boot, so a fixed key pair
    lands on one owner roughly one boot in `shards` — and a same-owner run proves nothing, because
    one owner serialises the whole workload by itself. (This bit me: the first clean run of the fix
    was clean for that reason and nothing else — `atomic_groups=0, atomic_localfast=574021`.) The
    battery therefore uses eight keys, for which one-owner is a ~1e-9 accident, and where
    `DEBUG SHARD` exists it prints and asserts the exact owner span.
  - **Vacuity gate.** With `--atomic 1` a run fails if no cross-shard group committed, if
    `atomic_read_cuts_held` did not advance, or (in the commit-delay arm) if `atomic_commit_holds`
    did not advance.
  - With `--atomic 0` a torn read is reported as a note, not a failure: cross-shard atomicity is the
    feature that is switched off. Violations are still asserted to be zero there and are — which is
    itself worth stating, because it shows the monotonicity fix is not what suppresses tears and the
    torn detector is not blind: the same battery on the same build with `--atomic 0` reports
    `torn=15 / 81 / 20` across its three arms with `violations=0`, so the `torn=0` under
    `--atomic 1` is a property of the engine, not of the test.
- New DEBUG subcommands (all zero in production, all behind `--enable-debug-command`):
  - `DEBUG SHARD <key>` — the owner a key routes to. The geometry oracle above.
  - `DEBUG ATOMIC-COMMIT-DELAY <microseconds>` — holds a group between drawing its ticket and
    storing it into the epoch word.
  - `DEBUG ATOMIC-READ-DELAY <microseconds>` — holds a plain read on its owner before it resolves.
- New INFO STATS counters: `atomic_commit_windows` (a group published while another committer was
  inside the bracket), `atomic_commit_holds` (a read's cut excluded a drawn-but-unpublished ticket),
  `atomic_read_cuts_held` (a plain read resolved at its pinned cut rather than at "now").
- No new config knobs, so `tomokv.conf` is unchanged.

## Test evidence

```
--- regression, fix branch, 3 x 60s -------------------------------------------------------------
geometry: 8 key(s) over 5 owner(s) [0, 1, 7, 9, 13]
freshness floor    rounds=4000     acked-write-not-visible: cross-conn=0 own-conn=0
unarmed            batches=1314170 writes=644084 violations=0 torn=0 [groups+644086 read_cuts_held+959   commit_holds+1]
read-delay 20us    batches=312866  writes=160176 violations=0 torn=0 [groups+160178 read_cuts_held+218   commit_holds+2]
commit-delay 100us batches=202774  writes=98903  violations=0 torn=0 [groups+98905  read_cuts_held+63611 commit_holds+197642]
session_monotonic: PASS                            (runs 2 and 3 identical in shape; 0/0 throughout)

--- same battery on HEAD ------------------------------------------------------------------------
unarmed            batches=1385297 writes=656585 violations=542 torn=0
  FAIL unarmed: 542 session-monotonicity violation(s)
  FAIL unarmed: atomic_read_cuts_held did not advance -- the guard never opened
session_monotonic: FAIL                            (runs 2/3: 389 and 171 violations)

--- regression, fix branch, --atomic 0 (torn detector proving it is not blind) -------------------
geometry: 8 key(s) over 7 owner(s) [1, 4, 5, 6, 9, 11, 14]
freshness floor    rounds=4000     acked-write-not-visible: cross-conn=0 own-conn=0
unarmed            batches=688512  violations=0 torn=15   [groups+0]   <- atomicity is switched off
read-delay 20us    batches=169488  violations=0 torn=81   [groups+0]
commit-delay 100us batches=171904  violations=0 torn=20   [groups+0]
session_monotonic: PASS   (torn reported as notes; violations 0 -- per-shard FIFO carries the
                           shared key even with no MVCC, which is why bug 1 is an EPOCH bug)

--- battery, release, both atomic modes ---------------------------------------------------------
  atomfix / atomic_torn / atomic_ryow / ryow / torture / multi_exec / debug   atomic=1  PASS
  atomfix / atomic_torn / atomic_ryow / ryow / torture / multi_exec / debug   atomic=0  PASS
  session_monotonic                                                          atomic=1/0 PASS

--- battery under ASAN+UBSAN (build/tomokv-asan, ldd-checked), both modes ------------------------
  atomfix / atomic_torn / atomic_ryow / ryow / torture / multi_exec / session_monotonic  PASS
  sanitizer reports in server log, during run and after shutdown: 0   (both modes)

--- differ vs vanilla redis 7.4 (oracle 127.0.0.1:7016, cores 120-127) --------------------------
  DIFFER xshard: 4276 ops, 0 diffs -> PASS      atomic=1
  DIFFER string: 4033 ops, 0 diffs -> PASS      atomic=1
  DIFFER xshard: 4276 ops, 0 diffs -> PASS      atomic=0
  DIFFER string: 4033 ops, 0 diffs -> PASS      atomic=0

--- wider sweep, release, atomic=1 --------------------------------------------------------------
  blocking stream streamgroups multi_exec limits resp3 tracking notify lua_scripting dumprestore
  zc pubsub bitfield zsetops geo hexpire lcs debug auth climon climon2 servertail    all PASS
  slowlog FAIL — identical failure on HEAD (see below); acl needs an ACLFILE argv, harness usage.
```

## Findings handed back, not fixed

1. **`tests/atomic_torn.py` flakes on BOTH branches**, and one of its flakes is a real invariant:
   over 10 runs each on a quiet 16c boot,
   - HEAD: 7 PASS / 3 FAIL — 1x `ON RENAMENX loser is invisible anomalies=1`,
     2x `OFF control exposes impossible SINTERSTORE image invalid=0` (a vacuous negative control),
   - branch: 8 PASS / 2 FAIL — 1x the same `ON RENAMENX` anomaly, 1x `OFF control exposes torn
     MSET-8 torn=0` (again a vacuous negative control).
   `ON RENAMENX loser is invisible` is not a control: a losing conditional mover's private
   candidate became visible once in six runs, on HEAD as well as here. That is a third invariant
   break in the same engine and it is outside this brief's two bugs. Reproduce with repeated
   `tests/atomic_torn.py` runs against `--atomic 1`.
2. **`tests/slowlog.py` fails identically on HEAD and on this branch**
   (`shrinking max-len trims immediately: got 9`, and intermittently `pipelined burst escalated to
   per-op timing: got 8`). Pre-existing, unrelated.
3. **EXEC still reads "newest" inside its transaction** (`multi.inc` sets the read context to
   `UINT64_MAX`). A read-only transaction can therefore still be the earlier half of a
   monotonicity inversion. Left alone deliberately: EXEC's queued commands include writes whose
   base must be current, and pinning them is a semantic change that belongs with the MULTI lane
   rather than inside a P0 fix.
4. **Immediate (non-blocking) `XREAD`** carries `CmdFlags::Blocking` and is therefore not stamped,
   so it reads "newest". Narrow, and the alternative (stamping it) risks the
   `blocking_resume_move()` re-prepare hazard described above.

## Measurement notes

- All perf numbers are INDICATIVE loopback on cores 32-47 (server) and 120-127 (memtier), taken
  while a study sweep was co-tenant on 0-31/64-95.
- A **cross-shard atomic MSET** cell was attempted as the direct measure of the commit bracket and
  is **reported as non-discriminating**: the A/A control (fix vs fix) spread −20.4% to +25.3% over
  four interleaved pairs, so the A/B spread (−2.7% to +37.3%) carries no signal. The GET/SET cells
  above were stable (rep-to-rep spread under 1.3%) and are the numbers that count.

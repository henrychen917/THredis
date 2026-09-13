# NOTES-ATOMFIX — cross-shard scan ordering (P0, pre-existing, reproduced and fixed)

| | HEAD (04a0eaba4) | t-atomfix |
|---|---|---|
| differ `xshard`, `--atomic 1`, seed 4242, 40 reps | **9 FAIL / 40** | **0 FAIL / 40** |
| differ `xshard`, seeds 7 / 29 / 99, 15 reps each | **3 FAIL / 45** (all seed 29) | **0 FAIL / 45** |
| directed regression `tests/atomfix.py`, armed hook | **0 PASS / 10** | **10 PASS / 10** |
| directed regression, `--atomic 0` | n/a | **6 PASS / 6** |
| ASAN batteries + `xshard` differ, both atomic modes | — | **0 sanitizer reports** |
| p32/p1 GET/SET loopback A/B (INDICATIVE) | baseline | **−0.4% … +1.1%**, atomic-off unchanged |

The failing signature was never the write's own reply. `RENAME` answered `+OK`, `MSET` answered
`+OK`, and then a `KEYS` issued *later on the same connection* came back without the key that
reply had just promised. Cross-connection ordering is explicitly not a hazard in this engine; a
listing that contradicts a reply the same connection already received is.

---

## Root cause

`for_each_task_key()` in `src/cmd/atomics_glue.inc:691` decides what a queued task "touches" on a
shard, and `xshard_tasks_share_key()` (same file, `:736`) turns that into the program-order
predicate `ExLoop::has_parked_predecessor()` (`src/core/ex_loop.h:786`) uses to keep a younger task
behind an older one from the same connection.

Whole-owner commands — `KEYS`, exact `DBSIZE`, `FLUSHDB`/`FLUSHALL` — name **no keys at all**. The
old code expressed "touches everything" only on the `!group` arm:

```c
ShardGroup* group = group_for(state, shard_id);
if (!group) return state.key_count == 0;      // all-shards command
```

but `xshard_prepare()` gives those commands a real `ShardGroup` **for every shard**
(`src/cmd/scatter_engine.inc:1271-1278`), with `count == 0`. So `group_for()` always succeeded,
the loop over `group->count` ran zero times, and the function returned `false`: *touches nothing*.
`xshard_tasks_share_key()` therefore found no overlap between any write and any walker, and
`has_parked_predecessor()` waved the walker straight past.

That only matters when the older task is parked rather than merely queued — tasks reach one
executor through one ordered channel, so the inbox itself preserves program order. The reproduced
park is the direct cross-shard `RENAME`: its destination task returns early from
`xshard_task_should_defer()` (`atomics_glue.inc:638`) while `state.direct_ready == 0`, waiting for
the source shard to build the destination image, and lands in `atomic_deferred_`. `execute()`
returns `true` for a deferred task, so the batch keeps going — straight into the `KEYS` task for
that same shard.

**The captured interleave** (instrumented build, `--shards 16 --ratio 4:4`, executor t227385,
shard 0; server op 4213 = `RENAME xs:42… → xs:09…`, server op 4218 = `KEYS xs:*`):

```
1106159.867334  defer  op=4213 shard=0      <- RENAME destination parked on direct_ready
1106159.867347  exec   op=4218 shard=0      <- KEYS overtakes it and starts walking
1106159.867361  exec   op=4218 shard=0
1106159.867367  exec   op=4218 shard=0
1106159.867379  exec   op=4218 shard=0
1106159.867383  KEYS_pass op=4218 matched=1 listed=0 phys=(nil) snap=725 entries=0
```

`phys=(nil)` is the whole bug in one field: the walker finished its bounded passes over shard 0
while the destination did not yet exist there. `FlatStore::scan()`
(`src/store/flatstore.h:984`) is a resumable cursor, not a snapshot — it reports only what its
cursor visited — so an install that lands behind the cursor, or after the walk has passed the
shard entirely, is gone from that listing forever, while `RENAME` had already replied `+OK`.

**The mirror case is the same defect.** `service_atomic_deferred()` runs *before*
`service_xshard_retries()` on every executor iteration (`ex_loop.h:84-85`), and a bounded `KEYS`
walk lives in `xshard_retries_` between passes. A younger same-connection write parked in
`atomic_deferred_` was therefore executed in the middle of an older walk; the read-your-own-writes
gate in `atomic_resolve_internal()` then made that younger install visible, and the listing named
a key the command *before* it had deleted:

```
.296328 defer  op=868 shard=13   <- KEYS held behind the older UNLINK   (fix working)
.296330 defer  op=869 shard=13   <- MSET  held behind it too            (fix working)
.296435 exec   op=868 shard=13   <- KEYS starts pass 1, returns Retry
.296469 exec   op=869 shard=13   <- younger MSET runs from atomic_deferred_
.296471 install_group ... value=0x…28240        <- installs behind the cursor
.296867 KEYS_pass op=868 listed=1 entries=4     <- listing names it
```

## Fix (two lines of logic, both cold)

1. `src/cmd/atomics_glue.inc` — a zero-key group answers "touches everything", **scoped to the
   shard the parked task is actually executing on**:
   `if (state.key_count == 0) return (task.shard >= 0 ? task.shard : op.shard) == shard_id;`
   The scoping is load-bearing: an unscoped version (a walker holds younger work on *every* shard
   its executor owns, because it carries a group for each) livelocked the differ at 1/20 — a
   walker parked on an undecided entry stalled the very groups whose completion would decide it.
   That was caught by this branch's own 20-rep loop and corrected before anything else was run.
2. `src/core/ex_loop.h` — `has_atomic_deferred_predecessor()` becomes `has_parked_predecessor()`
   and scans `xshard_retries_` as well as `atomic_deferred_`. Both deques hold tasks taken off the
   inbox but unfinished, and both are serviced ahead of a fresh drain, so program order has to
   hold against both.

Laws respected: no shard is touched by a non-owner; the only new behaviour is *waiting*, and only
between tasks of one connection, so cross-client order is still not a hazard; nothing committed is
dropped or rewritten; no reply shape changed. `sizeof(Op)`/`sizeof(Client)` are untouched (the one
new field, `ScatterState::debug_direct_defer`, is in the arena-allocated scatter state, and the
`ScatterState` size static_assert still holds).

Cost when nothing is parked: `has_parked_predecessor()` opens with
`atomic_deferred_.empty() && xshard_retries_.empty()` — the branch that is true on every path with
no cross-shard atomic window open — and the scan loop is `noinline` so it stays out of `execute()`.
The GET/SET dispatch path never reaches any of it.

## Hypotheses from the brief

- **H1 (a cleanup/promotion pass discards a committed record — permanent loss).** *Refuted.* A
  per-key physical-mutation trace over ~150 runs never showed a committed record being rolled
  back. `atomic_collapse()`'s aborted arm only ever restored genuinely-aborted groups.
- **H2 (the scan walker lacks the read gate point reads have).** *Half right, wrong half.* The
  walker's visibility filter is correct — `atomic_physical_key_visible()` resolves through the
  same MVCC path and honours same-connection RYOW. What it lacks is not a gate but **ordering**:
  it can run before the write reaches the shard at all. `phys=(nil)` in the trace above is the
  proof — there was nothing to filter.
- **H3 (abort/commit race in the `direct_ready` handshake).** *Refuted.* The handshake is sound;
  its *park* is what the walker overtook, which is why the DEBUG hook widens exactly that park.
- **H4 (double-finalize producing the stray `-WRONGTYPE`).** *Not reproduced* in ~150 differ runs
  (4276 ops each) on HEAD or on this branch, and no extra reply ever appeared. The differ's own
  comment at `tests/differ.py:2285-2288` records that probing on its pipelined connection
  desynchronises the reply stream and self-inflicts a cascade — which is exactly how a foreign
  error materialises "at the wrong slot". The capture predates the fresh-connection probe rule,
  and the diff that triggered the probe is the bug fixed here. Left as *explained, not isolated*;
  if it ever recurs with fresh-connection probes it is a genuinely separate defect.

**On the `EXISTS=0` vs `EXISTS=1` split**: both are one bug. The differ probes on a fresh
connection immediately after the diffing `KEYS`, while the rest of its 64-op pipeline is still in
flight. `EXISTS=1` is the key still present; `EXISTS=0` is a *later* op in that same batch having
deleted it before the probe landed (e.g. seed 4242 op 4218 `UNLINK … xs:37 …` two ops after the
`KEYS` that lost `xs:37`). Nothing was ever globally lost — a full-table audit built into the
instrumented walker never found a key missing from the physical table, only from the listing.

## Regression mechanism — `tests/atomfix.py`

Deterministic, not probabilistic. `DEBUG ATOMIC-DIRECT-DEFER <n>` (boot-gated behind
`--enable-debug-command`) parks every direct-`RENAME` destination task for `n` extra owner passes
*after* its source hop is ready — precisely the window the walker used to exploit. Production
default is 0 and the only reader is `xshard_prepare()`'s already-cold direct-RENAME arm.

Three cases, both directions plus a negative control:

1. 96 cross-shard `RENAME`s then `KEYS` in one pipelined burst. Every `RENAME` reply is read and
   asserted `+OK` before the listing, so a missing destination is a listing that contradicts a
   reply the connection already holds.
2. `DEL` × 96, `KEYS`, `MSET` × 96 in one burst. The listing must contain neither the deleted
   values nor the not-yet-written ones.
3. The same shapes with the hook disarmed.

**Vacuous-validation gate**: the test reads `atomic_scan_order_holds` (new, `INFO STATS`, summed
per executor) before and after the armed run and **fails** if it did not advance — correct data
with a gate that never opened proves nothing. Observed: 455 holds armed vs 4 unarmed.

```
HEAD + the same hook and counter, ordering fix removed  : pass=0  fail=10 of 10
    FAIL KEYS lost 7 RENAME destination(s) that had already replied +OK, first
         'atomfix:dst:0084:dddd…' (armed run)
t-atomfix, --atomic 1                                   : pass=10 fail=0 of 10
t-atomfix, --atomic 0                                   : pass=6  fail=0 of 6
```

(The "HEAD" arm is a scratch build of `04a0eaba4` carrying this branch's DEBUG hook and counter
but neither ordering change, so the two arms differ only by the fix.)

## Test evidence

Release, both `--atomic 1` and `--atomic 0`, 16 shards, `--ratio 4:4`, cores 8-15:

```
  atomfix.py         PASS   atomfix: PASS
  atomic_ryow.py     PASS   ATOMIC_RYOW PASS
  atomic_torn.py     PASS   ATOMIC_TORN PASS
  ryow.py            PASS   RYOW PASS
  torture.py         PASS   TORTURE PASS
  multi_exec.py      PASS   MULTI/WATCH directed battery passed
  debug.py           PASS   debug: PASS (toggle fired, LOADAOF off guard, …)
  differ xshard      PASS   DIFFER xshard: 4276 ops, 0 diffs -> PASS
  differ string      PASS   DIFFER string: 4033 ops, 0 diffs -> PASS
  differ hash        PASS   DIFFER hash:   3545 ops, 0 diffs -> PASS
  differ set         PASS   DIFFER set:    3524 ops, 0 diffs -> PASS
  differ zset        PASS   DIFFER zset:   3531 ops, 0 diffs -> PASS
  differ list        PASS   DIFFER list:   3521 ops, 0 diffs -> PASS
```

Whole-owner writers, since `FLUSHDB`/`FLUSHALL` now take part in the ordering hold:

```
  FLUSHCAP atomic=1 rc=0   run: capture done 0.1s   verify: dbsize=100000 sampled-bad=0 PASS
  FLUSHCAP atomic=0 rc=0   run: capture done 0.1s   verify: dbsize=100000 sampled-bad=0 PASS
  SNAPCUT  atomic=1 rc=0   save + verify_cut PASS
  SNAPCUT  atomic=0 rc=0   save + verify_cut PASS
```

INFO surface (the counter is a new `STATS` row) and the remaining wire batteries:

```
  servertail.py PASS (101 checks)   resp3.py PASS (140 checks)
  climon.py PASS   climon2.py PASS  blocking.py PASS
  notify.py PASS (notify_events_fired=1587)   tracking.py PASS (14 invalidations fired)
  differ servertail PASS: 5339 ops, 0 diffs
```

ASAN+UBSAN (`make asan`, `ldd` confirms `libasan`/`libubsan` are linked into the binary under
test), both atomic modes — `atomfix`, `atomic_ryow`, `atomic_torn`, `ryow`, `torture`, and the
`xshard` differ, **0 `AddressSanitizer` and 0 `runtime error:` lines in either server log**.

## INDICATIVE perf guard (loopback, my cores only — not a NIC number)

Server `taskset -c 8-15` (4 io + 4 ex, 16 shards), memtier `taskset -c 120-127` `-t 8 -c 4`,
`-d 64`, 200k-key space pre-filled to `dbsize == key-maximum` (GET hit rate 100%), arms
interleaved fix/head/fix/head, mean of 2 unless noted.

| cell | t-atomfix | HEAD | delta |
|---|---|---|---|
| atomic 0, p32 GET | 5.001 M | 4.990 M | +0.21% |
| atomic 0, p32 SET | 5.288 M | 5.308 M | −0.39% |
| atomic 0, p1 GET | 0.325 M | 0.321 M | +1.11% |
| atomic 0, p1 SET | 0.329 M | 0.329 M | −0.01% |
| atomic 1, p32 GET | 5.028 M | 4.990 M | +0.77% |
| atomic 1, p32 SET (4 reps/arm, median) | 5.286 M | 5.289 M | −0.07% |

The atomic-1 SET cell read −1.96% at 2 reps/arm on a single HEAD outlier (5.530 M against its own
5.314 M); at 4 reps/arm the medians are indistinguishable. Every cell is inside this box's noise,
the atomic-off path is untouched, and the fix is entirely off the GET/SET dispatch path.

## Scope not taken

`multi_task_tagged()` transaction tasks bypass `execute()`'s ordering check entirely and are
serviced from their own `multi_retries_` deque. A whole-owner walker inside `MULTI` therefore
still relies on the transaction barrier rather than on this predicate. `multi_exec.py` and the
`atomic_ryow`/`atomic_torn` batteries pass in both modes, and nothing in the reproduced failure
involves `MULTI`, so this is left alone deliberately rather than widened without a repro.

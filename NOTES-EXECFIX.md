# Lane t-execfix — four MULTI/EXEC defects, three fixed, one shelved (plus one found and shelved)

Branch `t-execfix`, from `HEAD 4f4c88082`. Server `--shards 16 --ratio 2:4` (2 io + 4 ex, one
thread per core) pinned to cores 0-5, loadgen/oracle on cores 6-7, ports 7080 (target) / 7081
(vanilla redis 7.4 oracle). Every server was started and stopped through
`scratchpad/execfix/lane.sh`, which resolves the pid **from the listening socket** and refuses to
boot onto a live listener. Nothing was ever selected by name or pattern.

---

## 0. Verdict table

| # | Defect | Reproduced by me on unfixed HEAD? | Status | Where |
| --- | --- | --- | --- | --- |
| **(a)+(c)** | own epoch-0 candidate loses the resolver's winner comparison — a later access in one EXEC answers from before it, and an in-place RMW then mutates the parked predecessor so the write is **silently lost** | **yes**, 91–94 % of rounds, both `--atomic` modes | **FIXED** | `src/store/flatstore_atomic.inc` `atomic_resolve_internal` |
| **(b)** | `std::abort()` on a version-bytes gauge underflow — a crash | **yes**, SIGABRT inside one rep | **FIXED**, and the abort's signal was **real**: it was (a) | `src/store/flatstore_atomic.inc` `atomic_gauge_sub` + `atomic_gauge_underflows` counter |
| **(d)** | cross-shard `LCS` inside `MULTI` answers `ERR internal cross-shard completion error` | **yes**, all three LCS reply shapes | **FIXED** | `src/cmd/scatter_engine.inc` `xshard_multi_child_complete` |
| **(e)** | *found by this lane, not in the brief*: a **failing** `RENAMENX` / `COPY`-without-`REPLACE` NX condition, cross-shard, inside `MULTI`, answers `EXECABORT` instead of `0` | **yes**, pre-existing on HEAD | **SHELVED**, with the argument in §6 | — |

**(c) is not a separate defect: it is (a).** The brief's `RPUSH`/96-byte/list framing is a red
herring — I delta-debugged the differ failure down to a 7-command reproducer that loses a
**1-byte** element, and the same shape loses a set member, a hash field and a zset member. The
mid-lane correction from t-edgeenc says the same thing and its own 80-trial matrix is reproduced
below (§2.1). **(b) is a consequence of (a)**, proved by a discriminating control in §3.

So the whole brief reduces to one comparison in the MVCC resolver, plus two small independent
fixes. That comparison is exactly the change the brief said to do LAST and only with a bounded
blast radius; the containment evidence is §5.

---

## 1. Headline before / after

| Measurement | unfixed `HEAD 4f4c88082` | this branch | mode |
| --- | ---: | ---: | --- |
| `c_loop.py` silently lost writes / 300 rounds | **281 (93.7 %)** | **0** | atomic 0 |
| `c_loop.py` silently lost writes / 300 rounds | **273 (91.0 %)** | **0** | atomic 1 |
| `atomic_predecessor_reads` moved by the same run | **+281 / +273** | **+0 / +0** | both |
| `narrow.py … rpush,lrange exec 700`, seeds 1-4 | **1333 / 1286 / 1291 / 1289 diffs** | 0 (seeds 1-3) | atomic 0 |
| `narrow.py … incrby,append,rpush exec 700` seed 1 | **717 diffs** | **0** | atomic 1 |
| `narrow.py … del,set exec 700` seed 1 | **18 diffs** | **0** | atomic 1 |
| `abortrepro.sh` (3 reps) | **SIGABRT in rep 1** | **completed, server still listening** | atomic 1 |
| `atomic_gauge_underflows` after that same workload | counter does not exist | **0** (with `atomic_entries` 10 121, so the chain really was exercised) | atomic 1 |
| cross-shard `LCS` in `MULTI`, 3 reply shapes | **3× `ERR internal cross-shard completion error`** | identical to bare `LCS` | both |
| `tests/execfix.py` | **FAIL (30)** | **PASS (72 rows)** | both |
| `differ.py multi` (widened, see §4) seeds 1-2 | **132 / 106 diffs** (a0), **118 / 107** (a1) | **0 diffs**, 3 seeds × 2 modes | both |

---

## 2. Reproduction, mine, before touching any code

### 2.1 (a)+(c) — the silently lost write

The brief's minimal case (`RPUSH k 96-bytes` inside `MULTI`) does **not** reproduce: a 28-cell
matrix of (seeded/empty) × (8, 32, 63, 64, 65, 96, 200 byte element) × (bare / in-MULTI) came back
`0/28 cells wrong` against the oracle. So I delta-debugged the real differ failure
(`scratchpad/execfix/c_shrink.py`, 4 606 ops → 9 ops) and then hand-minimised to seven commands:

```
shard(lz:A)=5 shard(lz:B)=11
  FLUSHALL                                 -> b'+OK\r\n'
  MULTI                                    -> b'+OK\r\n'
  RPUSH lz:B v                             -> b'+QUEUED\r\n'
  RPUSH lz:A v                             -> b'+QUEUED\r\n'
  EXEC                                     -> b'*2\r\n:1\r\n:1\r\n'
  MULTI                                    -> b'+OK\r\n'
  RPUSH lz:A x                             -> b'+QUEUED\r\n'
  EXEC                                     -> b'*1\r\n:2\r\n'      <-- says the list is 2 long
  LRANGE lz:A 0 -1                         -> b'*1\r\n$1\r\nv\r\n' <-- it is 1 long
```

The element is one byte. What is load-bearing is that the **first** transaction spanned **two
owners**. My own ingredient sweep (`c_probe.py`, `--atomic 0`, unfixed HEAD):

| shape | verdict |
| --- | --- |
| `MULTI RPUSH B v; RPUSH A v EXEC` then `MULTI RPUSH A x EXEC` | **element lost** |
| same, keys swapped | **lost** |
| tx1 touches only `A` | ok |
| bare `RPUSH A v` instead of tx1 | ok |
| tx1 alone, no second transaction | ok |
| both pushes inside **one** transaction | ok |
| `SADD` / `ZADD` in place of `RPUSH` | **lost** |
| `SET`+`APPEND`, `HSET` | ok on that particular run (flaky — see below) |

It is a race, so it is flaky per-round; amplified over 300 rounds it is 91-94 %. The matrix from
lane t-edgeenc, run by me on my own binaries and ports (`scratchpad/execfix/execexec.py 7080 7081 80`):

| shape | HEAD a0 | HEAD a1 | this branch a0 | this branch a1 |
| --- | ---: | ---: | ---: | ---: |
| `T1{k} T2{k}` | 0/80 | 0/80 | 0/80 | 0/80 |
| `T1{k2,k} T2{k}` | **67/80** | **74/80** | **0/80** | **0/80** |
| `T1{k,k2} T2{k}` | **75/80** | **71/80** | **0/80** | **0/80** |
| `T1{k2,k} T2{k2,k}` | **42/80** | **44/80** | **0/80** | **0/80** |
| `T1{k2,k}` only | 0/80 | 0/80 | 0/80 | 0/80 |
| … short values | **73/80** | **75/80** | **0/80** | **0/80** |
| … single element | **73/80** | **70/80** | **0/80** | **0/80** |
| … `SET`/`APPEND` | 0/80 | 0/80 | 0/80 | 0/80 |
| … hash | **10/80** | **9/80** | **0/80** | **0/80** |
| … bare write after | 0/80 | 0/80 | 0/80 | 0/80 |

### 2.2 (b) — the abort

`REPS=2 BIN=<HEAD> bash scratchpad/execfix/abortrepro.sh`:

```
target pid 705936, log /tmp/claude-1000/execfix/lane-srv.1A0yrq
abortrepro.sh: line 31: 705936 Aborted (core dumped) taskset -c 0-5 ... --port 7080 ... --atomic 1
TARGET GONE: rep 1 blind seed 1
```

### 2.3 (d) — LCS

```
same-shard   sh :0/:0  bare=b'$6\r\nmytext\r\n'
             exec=b'*3\r\n$6\r\nmytext\r\n:6\r\n*4\r\n$7\r\nmatches\r\n...'
cross-shard  sh :0/:1  bare=b'$6\r\nmytext\r\n'
             exec=b'*3\r\n-ERR internal cross-shard completion error\r\n
                          -ERR internal cross-shard completion error\r\n
                          -ERR internal cross-shard completion error\r\n'
```

---

## 3. Root cause, and why (b) is (a)

`atomic_resolve_internal` (`src/store/flatstore_atomic.inc`) walks the owner's pending MVCC chain
oldest-first and picks a winner:

```cpp
if (!winner_set || epoch >= winner_epoch) { winner = candidate; winner_epoch = epoch; … }
```

A transaction's **own still-private** candidate carries `epoch == 0` — its commit ticket has not
been drawn yet. It is correctly admitted past both visibility guards by `own_committed` (same
`origin_conn_id`) and then **loses this comparison to any older but COMMITTED version of the same
key**, whose epoch is non-zero. The precondition is therefore "the key already carries a live MVCC
entry", which a `MULTI` spanning more than one owner always leaves behind — `EXEC` force-admits an
atomic group irrespective of `--atomic`, which is why the defect is not `--atomic 1`-only.

Two faces:

* **Read face.** The second touch of a key inside one `EXEC`, and a plain read after that `EXEC`,
  answer from before the transaction. Observed directly on HEAD:
  `MULTI / INCRBY k 5 / INCRBY k 4 / GET k / EXEC` → `[12, 11, b'7']` instead of `[12, 16, b'16']`.
* **Write face, and this is the serious one.** A collection handler mutates **in place** the object
  the resolver hands it. Handed the **parked predecessor**, `RPUSH` appends to a version that
  collapse subsequently frees. The `RPUSH` replies with the correct new length; the element is gone.
  `SET`/`APPEND` are mostly immune only because they *replace* the object rather than mutate it, and
  because the stale image usually has the same bytes.

The proof is a counter, not an argument: `atomic_predecessor_reads` — the resolver saying "I
answered from a version older than the physical one" — moved **exactly once per lost write**
(`silently_lost=281 … predecessor_reads_delta=281`).

**(b) falls out of the same mechanism.** `atomic_admit` charges `kvobj_size(incoming)` before an
install and collapse returns `kvobj_size(parked)` afterwards; the two are equal *unless the parked
object grew after it was parked* — which is precisely the write face above. I instrumented every
gauge decrement site (not just the abort) and the **first** over-decrement is the collapse
free-parked site, never the group-install site the abort lives at:

```
TOMO-GAUGE underflow gauge=64 parked=64 installed=192 short=128
TOMO-ABORT src/store/flatstore_atomic.inc:164
```
```
TOMO-GAUGE under src/store/flatstore_atomic.inc:798 gauge=448 sub=640 short=192
TOMO-GAUGE under src/store/flatstore_atomic.inc:798 gauge=1024 sub=1792 short=768
… (10 events, all the same site)
```

Discriminating control, 4 reps × 3 seeds each on a fresh boot:

| workload | gauge over-decrements |
| --- | ---: |
| `narrow.py` **blind** mix (`mget,exists,touch,del,set,get,mset,msetnx,strlen,getrange,lrange`) — no read-modify-write at all | **0** |
| `narrow.py` **rmw** mix (`incrby,append,rpush`) | **44** |

**So the gauge is right and the abort was pointing at a real corruption.** The brief asked me to
decide "gauge wrong or abort too strict"; the honest answer is *neither* — the gauge was correct,
the abort was correctly triggered, and the **response** was wrong. Killing the process converts a
memory-accounting error (the gauge feeds `accounted_bytes()`, i.e. eviction decisions) into total
service loss, and it kills it in whichever unrelated group pass happens to notice the wrap rather
than in the command that caused it. The fix is: fold every one of the eleven `-=` sites into one
`atomic_gauge_sub()`, clamp at zero, and **count** — `atomic_gauge_underflows` in `INFO stats`, with
a gate row asserting it is 0. The abort's diagnostic value is preserved and testable; its failure
mode is not.

### The `#line` trap the brief warned about

`scatter_engine.inc` carries `#line 34 "src/cmd/xshard.cc"`, so every backtrace frame inside it is
remapped by −26 into a 60-line file. I did not fight gdb: `scratchpad/execfix/tag_aborts.py`
rewrites every `std::abort()` in the four atomic files into `tomo_abort_tag("<real file>:<real
line>")`, with the tag computed from the real file and baked in as a string literal so the remap
cannot touch it. That named the frame in one run.

---

## 4. What changed

| File | Change |
| --- | --- |
| `src/store/flatstore_atomic.inc` | **(a)** `atomic_resolve_internal` ranks by RANK, not epoch: an own still-private candidate outranks every committed one; among private, and among committed, the later occurrence still wins. Two extra bools and one branch, inside a lambda that is only reachable when the owner has a live pending chain. **(b)** all eleven gauge decrements go through one `atomic_gauge_sub()` that clamps and counts instead of wrapping; the `std::abort()` is gone. |
| `src/core/shard.h` | `Stats::atomic_gauge_underflows`, bound through `bind_atomic_state` |
| `src/cmd/t_server.cc` | `atomic_gauge_underflows` in `INFO stats` |
| `src/cmd/scatter_engine.inc` | **(d)** `xshard_multi_child_complete` grows the `Kind::Lcs` arm that `xshard_finish` already had. Without it the MULTI child reached `assemble_final` with `final_reply == None` and fell through its `default:`. |
| `tests/execfix.py` | new battery, 72 rows, both atomic modes, counters asserted |
| `tests/gate.sh` | `execfix` joins the armed debug-surface loop (needs `DEBUG SHARD` as its geometry oracle) |
| `tests/differ.py` | the `multi` suite is **widened**: the three families it had to exclude because of (a) — lists, string read-modify-write, and a key touched **twice inside one transaction** — are generated again, cross-shard `LCS` is added, and the "run this at `--atomic 0`" restriction is lifted |
| `scratchpad/execfix/` | lane harness: `lane.sh`, `verify.sh`, `ab2.sh`, `abbench.py`, plus the reproducers `c_loop.py`, `c_shrink.py`, `c_probe.py`, `c_min.py`, `c_iso.py`, `c_var.py`, `c_time.py`, `c_rpush.py`, `breadth.py`, `execexec.py` (from t-edgeenc), `narrow.py`/`abortrepro.sh` (from t-execiso), and `tag_aborts.py` (the abort-tagging instrument) |

No config knob: a correctness contract must not be optional, and the new counter is a statistic,
not a switch. `tomokv.conf` therefore needed no change.

The resolver hunk in full:

```cpp
const bool own_private = owner.group_epoch && epoch == 0;
if (!winner_set ||
    (own_private ? true : (!winner_private && epoch >= winner_epoch))) {
    winner = candidate; winner_epoch = epoch; winner_set = true;
    winner_physical = is_physical; winner_private = own_private;
}
```

A rank flag rather than a sentinel epoch on purpose: `UINT64_MAX` is already the "newest committed"
read-context sentinel in this file, and reusing it as a private-candidate rank would have made a
real epoch of `UINT64_MAX` tie with a private one. There is no such epoch today; the flag means
there cannot be one tomorrow either.

---

## 5. Containment for the resolver change — the bar the brief set

The change alters **every** atomic path, not just `MULTI`, so it is bounded four ways.

**Reachability.** `atomic_resolve_internal` returns at its first line when the owner has no live
pending entry, so the changed comparison is unreachable on the plain path. Within it, the change
only fires for a candidate that is (i) this connection's own and (ii) still uncommitted — a
*foreign* epoch-0 candidate is still rejected by the guard above it, unchanged, and every
committed-vs-committed comparison is byte-for-byte the old one. The collapse-side resolver
(`atomic_collapse`'s own `consider`) is deliberately **not** touched: its prefix selection breaks on
`!epoch`, so it never sees an epoch-0 owner and needed no change.

**Behavioural bound.** The one semantic change is: a transaction reading a key it has already
written in flight now sees **its own** value rather than a concurrently committed foreign one. That
is what redis does and what the connection's own later write would overwrite at commit anyway. A
reader on a *different* connection is unaffected (its `own_committed` is false, so it never enters
the private branch).

**Suite evidence** — `scratchpad/execfix/verify.sh battery`, one server at a time, both modes:

| suite | atomic 0 | atomic 1 |
| --- | --- | --- |
| `execfix` `execiso` `execatomic` `multi_exec` `atomfix` `atomic_ryow` `ryow` `torture` `lua_scripting` `scriptatomic` `session_monotonic` `debug` `limits` `resp3` `tracking` `concur` `zsetops` `lcs` | **18/18 pass** | **18/18 pass** |
| `differ.py`: `multi` (widened) / `xshard` / `string` / `list`, seeds 1-2 | **0 diffs × 8** | **0 diffs × 8** |
| `differ.py multi` seed 3 | 0 diffs | 0 diffs |
| ASAN+UBSAN (`libasan.so.8` + `libubsan.so.1` confirmed by `ldd`): `execfix` `execiso` `multi_exec` `atomic_ryow` `torture`, plus `differ multi` and `differ xshard` | clean, **0 diagnostics** | clean, **0 diagnostics** |

**`tests/atomic_torn.py` — run on BOTH binaries, because it fails on both.** 3 runs × 2 modes each:

| binary | atomic 0 | atomic 1 |
| --- | --- | --- |
| unfixed HEAD | FAIL 5 / 5 / 5 | FAIL 4 / 4 / 10 |
| this branch | FAIL 5 / 6 / 5 | FAIL 5 / 5 / **PASS** |

The failing check **set** is identical on both binaries:

```
FAIL OFF control exposes COPY losing race      errors=['no conditional cross-shard keys found']
FAIL OFF control exposes RENAMENX losing race  errors=['no conditional cross-shard keys found']
FAIL ON COPY loser is invisible                errors=['no conditional cross-shard keys found']
FAIL ON RENAMENX loser is invisible            errors=['no conditional cross-shard keys found']
FAIL OFF control exposes impossible SINTERSTORE image invalid=N reads=N errors=[]
```

Four are a precondition this 6-thread lane geometry cannot satisfy. The fifth is a **negative
control that must observe an anomaly**, and it is flaky on both binaries (absent from HEAD a1 run 1;
the whole battery passed on this branch's a1 run 3). Nothing here moved.

**Performance, INDICATIVE, loopback, interleaved A/B (`scratchpad/execfix/ab2.sh`), one server at a
time, 10 s cells, server cores 0-5, loadgen cores 6-7.**

| cell | atomic | pairs | HEAD → this branch |
| --- | ---: | ---: | --- |
| **p32 GET/SET** (memtier, `-t 2 -c 8 --pipeline=32 --ratio=1:1 -d 32`) — the PLAIN path guard, where the resolver is unreachable | 0 | 4 | −0.51 / +0.82 / −0.78 / −0.51 % |
| | 1 | 4 | +1.10 / **−3.60** / −0.99 / −0.99 % |
| **xatomic** (memtier, two-owner `MSET __key__ __data__ x__key__ __data__` interleaved 1:1 with `GET __key__`) — the resolver IS on this path | 0 | 4 | −0.02 / +0.49 / +0.16 / +0.20 % |
| | 1 | 4 | −0.32 / +0.33 / **−13.92** / +2.17 % |
| xatomic re-measure of the −13.92 % cell | 1 | 5 | +0.93 / +0.59 / +0.04 / −0.01 / −0.11 % |

**A/A control on the same cells, same binary in both arms** (this is what makes the table readable):
p32 −0.03 / +0.11 / −0.10 % (a0) and −3.55 / −0.36 / −0.51 % (a1); xatomic −0.48 / +0.08 / −0.13 %
(a0) and +0.54 / +0.36 / +1.61 % (a1). The p32 a1 arm's −3.60 % is inside its own A/A envelope
(−3.55 %). The −13.92 % xatomic cell is outside it, so I re-measured it: five fresh pairs land
between −0.11 % and +0.93 %, so it was a box event, not an arm difference. **Reading: free on both
paths.**

**A cell I am reporting as NOT MEASURABLE rather than as a result.** My first attempt used a python
loadgen (`abbench.py`) for the `mset` / `msetget` / `execwrite` shapes. Its **A/A control** swung
**−22.7 % to +29.9 %** with the same binary in both arms, so any A/B number from it would have been
noise. That is why the atomic-path cell above is memtier-driven. `abbench.py` is kept because it
still pins one key per shard (the boot-time hash seed otherwise changes the fan-out geometry on
every boot, which is the trap lane t-execiso recorded), but its numbers are not quoted.

**Conclusion: the containment argument holds, so the resolver change ships.** Reachability is
gated by a pre-existing early-out, the semantic delta is one connection seeing its own in-flight
write, 36 battery cells and 18 differ cells are green in both modes, ASAN/UBSAN is clean, the one
pre-existing red battery is red identically on both binaries, and both A/B cells are inside their
own A/A resolution.

---

## 6. (e) SHELVED — a fifth defect this lane found and did not fix

A breadth sweep of all 46 cross-shard multi-key shapes bare-vs-in-MULTI
(`scratchpad/execfix/breadth.py`) found one family beyond the brief:

```
DIFF RENAMENX-nx-fail   bare = b':0\r\n'
                        exec = b'-EXECABORT Transaction discarded because of an execution error.\r\n'
DIFF COPY-nx-fail       bare = b':0\r\n'
                        exec = b'-EXECABORT Transaction discarded because of an execution error.\r\n'
```

A **failing** NX condition — `RENAMENX`, or `COPY` without `REPLACE`, where the destination exists —
answers `0` bare and aborts the whole transaction inside `MULTI`. It reproduces on unfixed HEAD, in
both modes, so it is not this lane's doing. The keyspace is left **correct** in every case
(asserted); only the reply differs.

**Why it is not a one-liner.** The obvious fix is to stop propagating the child's `aborted` flag to
the transaction when `conditional_failed` is set. That is wrong here. `xshard_multi_child_bind`
binds the child's `publish_aborted()` to the **MultiExecState's** abort flag, so the child's
installed candidates are governed by the transaction's flag, not its own. `RENAMENX` is a two-hop
command whose source-side delete can already be installed by the time the destination owner
validates NX; today the shared abort flag is exactly what keeps that delete invisible. Not aborting
the transaction would let a half-applied `RENAMENX` become visible — a data-loss defect traded for a
reply-shape defect. A correct fix needs per-child abort scoping (or an all-participants NX barrier
before phase-2 installs) inside the frozen scatter engine, which is a different lane's brief and
cannot be bounded here.

`tests/execfix.py` **asserts the shelved behaviour** rather than ignoring it, so it cannot drift
silently: if it ever answers `0`, the row fails and points at this section.

---

## 7. The gate rows: red before, green after

`tests/execfix.py`, wired into `tests/gate.sh`'s armed debug-surface loop. Three binaries, all
built from this tree, differing only where stated.

| row | unfixed `HEAD 4f4c88082` | **control**: this branch with the resolver ranking reverted, gauge counter + LCS fix kept | this branch |
| --- | --- | --- | --- |
| `list: no write was silently lost over 120 rounds` (a0/a1) | **FAIL 111/120, 112/120** | **FAIL** | ok |
| `set:` same | **FAIL 113/120, 114/120** | **FAIL** | ok |
| `hash:` same | **FAIL 112/120, 113/120** | **FAIL** | ok |
| `zset:` same | ok (see note) | ok | ok |
| `list/set/zset/hash: resolver never answered from a parked predecessor` | **FAIL, +105…+114 each** | **FAIL, +110…+117 each** | ok, **+0** |
| `two INCRBYs and a GET in one EXEC chain off each other` | **FAIL `[12, 11, b'7']`** | **FAIL** | ok `[12, 16, b'16']` |
| `a plain read after that EXEC sees it` | **FAIL `b'11'`** | **FAIL** | ok `b'16'` |
| `an in-EXEC cross-shard MGET sees the same EXEC's writes` | **FAIL `[b'old', b'new']`** | **FAIL** | ok `[b'new', b'new']` |
| `in-EXEC LRANGE tracks the same EXEC's pushes` | **FAIL `[2, [b'seed'], 2, [b'seed']]`** | **FAIL** | ok |
| `version-bytes gauge stays conserved under the in-MULTI RMW mix` | **FAIL — build has no `atomic_gauge_underflows` at all, reported as vacuous** | **FAIL, +4 (a0) / +4 (a1); repeat run +4 / +3** | ok, **+0** |
| `cross-shard LCS shape 0/1/2 inside MULTI equals bare` | **FAIL ×3, `ERR internal cross-shard completion error`** | ok | ok |
| `every cross-shard multi-key command answers in MULTI as it does bare` (21 shapes) | ok | ok | ok |
| geometry / chain-existed / same-owner-LCS / shelved-(e) rows | ok (controls) | ok | ok |
| **battery exit** | **FAIL (30)** | **FAIL (24)** | **PASS (72 rows)** |

Transcripts: `/tmp/claude-1000/execfix/execfix-RED-HEAD-a0boot.txt`,
`execfix-RED-CONTROL.txt`, `execfix-GREEN-a0boot.txt`, `execfix-GREEN-a1boot.txt`.

**Note on the zset data row.** It does **not** go red on HEAD, although its
`atomic_predecessor_reads` row does (+105 in both modes): `ZADD` of a new member happens to replace
the object rather than mutate it in place often enough that the member survives. It is kept as
coverage and is labelled as such here rather than being presented as a row that guards a
demonstrated defect. The same applies to the geometry, chain-existed, same-owner-LCS, breadth and
`string APPEND` rows: they are **controls that must pass on both binaries**, not red-before rows.
Every row in the table above that is marked FAIL on HEAD is a row that guards a defect I
demonstrated first.

**Why the control binary exists.** `atomic_gauge_underflows` is brand new, so a battery asserting
"it is 0" could be satisfied by a build that can never make it non-zero. The control is this branch
with **one expression** reverted — the resolver's rank — and it drives the counter to 3-4 in the
battery and to 10 under the `narrow.py` rmw workload that made HEAD abort. That is the
must-report-non-zero evidence; unfixed HEAD cannot supply it because HEAD has no counter, only the
`abort()`.

**The counter is not a pure defect detector globally, and the battery does not treat it as one.**
`atomic_predecessor_reads` legitimately advances when a *foreign* reader resolves under an older
published cut — on this branch the full `abortrepro` workload leaves it at 12. The battery asserts a
**delta of 0 across its own arms**, which contain no foreign reader, and that is the only claim made
for it.

---

## 8. differ: the `multi` suite was widened, not just re-run

The suite as it arrived deliberately generated **no lists, no string read-modify-write and never
touched a key twice inside one transaction**, each exclusion commented with the reproducer that
re-opened it — all three were (a). Those exclusions are removed, cross-shard `LCS` is added, and the
"RUN THIS AT `--atomic 0`" restriction is lifted. The suite is therefore a materially stronger
oracle than before, and it is red on HEAD for that reason:

| binary | atomic 0 | atomic 1 |
| --- | --- | --- |
| unfixed HEAD, seeds 1-2 | **132 / 106 diffs** | **118 / 107 diffs** |
| this branch, seeds 1-3 | **0 / 0 / 0** | **0 / 0 / 0** |

A key duplicated inside a **single command** (`DEL k k`, `MSET k a k b`) remains excluded: that is a
separate pre-existing family, present bare as well as in `MULTI`, and `differ.py`'s `xshard` suite
is where it belongs. `ks()` still uses `rng.sample` to enforce it.

---

## 9. Harness notes worth keeping

* **A helper that replaces an operator must not be rewritten by its own sweep.** Folding eleven
  `atomic_version_bytes_ -= x;` sites into `atomic_gauge_sub(x)` with a scripted rewrite also
  rewrote the `-=` **inside `atomic_gauge_sub` itself**, making it infinitely self-recursive. GCC is
  entitled to assume a side-effect-free loop terminates, so the first build compiled, linked, ran
  every battery green, and *silently did no subtraction at all* — the only symptom was a nonsense
  `atomic_gauge_underflows: 26574`. Adding one parameter to the same function changed the inlining
  decision and the next build hung on the first cross-shard `MSET`, which is what exposed it. Two
  lessons: a rewrite sweep must exclude the body of the function it is rewriting **into**, and a new
  counter reading an implausible number is a reason to distrust the instrument, not the code. Every
  number in this file was re-measured on the repaired binary.
* **The shipped binary was rebuilt from the committed tree and `cmp`-verified byte-identical**
  (`md5 9dfbc0a2761a1d617d28cf1905dc6701`) to the binary every result above was measured on.
* **The redis 7.4 oracle on 7081 was killed out from under this lane three times** with no shutdown
  line in its own log — almost certainly another lane selecting `redis-server` by name. `lane.sh`
  now starts it under `setsid` with `LC_ALL=C` (the locale tip from t-edgeenc: an oracle in a
  non-C collation produces phantom `SORT … ALPHA` diffs that are the oracle's, not ours). Two differ
  cells had to be re-run after such a kill; both were green on the re-run.
* `tests/gate.sh` was **not** run (it owns port 7899 and cores 0-7, reserved for the mainline
  operator). The `execfix` row was added to it; it has been executed standalone in exactly the boot
  shape that loop uses, in both atomic modes.

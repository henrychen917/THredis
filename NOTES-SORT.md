# NOTES-SORT — `SORT ... BY pattern [GET pattern ...]`

> Historical note for the original single-executor implementation. Cross-owner admission and its
> source/gather design supersede the refusal described below; see `NOTES-SORTXSHARD.md`.

Branch `t-sort`. Feature file `src/cmd/t_sort.cc` / `src/cmd/t_sort.h`.

| | |
|---|---|
| **Before** | every `BY` and every `GET` was `ERR syntax error`. 17 of 17 probed forms diverged from the reference. |
| **After** | the whole `BY`/`GET` surface is implemented and byte-exact against vanilla 7.4 when **one executor owns every shard**; when it does not, a `*`-bearing `BY`/`GET` is **refused** with a named error and everything that dereferences nothing works in every placement. |
| **Refusal** | the dereferenced key set is not knowable before the source is read, so it cannot be routed. Executing it across executors would either break the single-owner law or require a distributed transaction with a dynamically discovered read set — which this tree's script engine already refuses by design. |
| **Evidence** | `tests/sort.py` 396 checks / 0 failures (deref config) and 77 / 0 (refusal config) across 6 placements and both atomic modes; `tests/differ.py … sort` ~4.5k ops × 6 seeds + RESP3, 0 diffs; 15 pre-existing suites and 5 transaction batteries unchanged; ASAN/UBSAN clean. `make sortnoctx` is the negative control and the battery fails against it. |
| **Three defects the differ found** | one in this lane's own read context, one pre-existing **data-placement** defect in `MULTI; SORT … STORE; EXEC`, and one ordering hole this feature opened inside EXEC. All three fixed, all three pinned by a test. §6. |

---

## 1. What the server did before

Probed live, target vs. vanilla 7.4 on the same inputs (`RPUSH mylist 3 1 2`, `MSET weight_1 10 weight_2 5 weight_3 1`, …):

| form | before | reference |
|---|---|---|
| `SORT mylist` / `DESC` / `ALPHA` / `LIMIT` | correct | correct |
| `SORT mylist BY weight_*` | `ERR syntax error` | `3 2 1` |
| `SORT mylist BY weight_* GET data_*` | `ERR syntax error` | `three two one` |
| `SORT mylist BY h_*->f GET h_*->g` | `ERR syntax error` | `c b a` |
| `SORT mylist BY nosort` | `ERR syntax error` | `3 1 2` (ordering suppressed) |
| `SORT mylist GET #` | `ERR syntax error` | `1 2 3` |
| `SORT mylist GET missing_*` | `ERR syntax error` | three nulls |
| `SORT … BY … GET … STORE dst` | `ERR syntax error` | `3`, dst = projection |
| `SORT_RO mylist BY weight_* GET data_*` | `ERR syntax error` | `three two one` |

`parse_sort_options` rejected the option words outright:
`// BY/GET patterns are deliberately outside this command slice and are syntax errors.`
`COMMAND GETKEYS SORT src BY w:* GET x:* STORE dst` already answered correctly (`src`, `dst`), and
the ACL surface already denied `BY`/`GET` to users without `allkeys`. So only execution was missing.

## 2. The ownership problem, concretely

`SORT mylist BY weight_*->f GET data_* GET #` on this architecture:

* **Hop 1** reads `mylist` on `shard_of(hash("mylist"))` — one owner, known at routing.
* The derived key set is `{ subst(BY, e) } ∪ { subst(GETⱼ, e) }` over every element `e`: up to
  **N × (1 + G)** names, where N is the source's cardinality and G the number of `GET` patterns.
* Each derived name routes by `hash_key(name)`, i.e. **anywhere**. With m shards and k derived
  names the expected number of distinct owners is `m(1 − (1 − 1/m)^k)`: at m = 16, k = 16 that is
  10.3 shards; at k = 64 it is 15.3. A SORT of a list of a few dozen elements touches
  *substantially all* shards.
* **The set is not knowable before hop 1**, because the names are functions of the element VALUES.
  It *is* knowable exactly once, immediately after hop 1 — SORT's dereference is exactly **one
  level deep** (`BY h_*->f` is still one level; the `->field` is inside the same key). So the shape
  is one source read → one wide fan-out → an optional single-key `STORE` apply. Not iterated.

Two structural facts make that fan-out unavailable to the existing engine:

1. **Keys are argv indices.** `KeyRef{hash, arg, shard, …}` and every owner task reads
   `op.arg(key.arg)` (`scatter_engine.inc:1977, 1998, 2173, 2595`). A synthesised name has no argv
   slot.
2. **The arena is carved from a key count fixed at prepare.** `arena_layout_bytes()` sizes
   `groups/keys/key_order/values/status/images/apply/hop2` from `key_count_for()`, which for SORT
   returns 1 or 2. `publish_phase2` re-groups `state.hop2`, an array of **positions into that same
   fixed `state.keys[]`** — data-dependent *selection*, never *discovery*. `publish_pop_retry` is a
   re-round over the identical key set, bounded at 8.

## 3. Options considered

### Option A — refuse the dereferencing forms (the reference's own precedent)

Vanilla redis in cluster mode refuses exactly these patterns, verified live on a cluster-enabled
7.4 instance with all 16384 slots assigned:

```
SORT mylist BY weight_*        -> ERR BY option of SORT denied in Cluster mode when keys formed
                                  by the pattern may be in different slots.
SORT mylist GET data_*         -> ERR GET option of SORT denied ...
SORT mylist BY nosort          -> 3 1 2        (no '*' => no dereference => allowed)
SORT mylist BY ?_w             -> 3 1 2        (only '*' is a substitution point)
SORT mylist GET #              -> 1 2 3        (allowed)
SORT {a}list BY {a}w_*         -> allowed      (hash tag makes the slot provable)
SORT {a}list BY {b}w_*         -> refused
SORT mylist BY w_* BADTOKEN    -> BY refusal   (the check fires as the option is PARSED)
SORT mylist BADTOKEN BY w_*    -> ERR syntax error
```

*Cost:* nothing at runtime, nothing locked, nothing new visible to a concurrent writer.
*Against it:* a single-executor deployment would be refused a command it can serve completely
legally, because that executor owns every shard.

### Option B — same-owner-only execution, decided from the data

Read the source, synthesise the derived names on its owner, and proceed only if all of them hash to
this shard; otherwise error.

**Counterexample that kills it:** the error becomes a function of user data.
`SORT mylist BY w_*` succeeds, then `RPUSH mylist 9` makes the identical command fail. It is not even
monotone, and the same command answers differently on two servers that differ only in `--shards`.
With 16 shards and 3 elements the probability that all three co-locate is (1/16)² ≈ 0.4%: it would
advertise support for something that essentially never works. Rejected.

### Option C — a real iterated fan-out (source wave → derived wave → store wave)

The engineering is possible — a heap side-car holding the synthesised names and gathered values, a
group builder that indexes the side-car instead of `state.keys[]`, `group_cap = nshards`, a new
phase-2 arm, a third wave for `STORE`, a bounded-pass `Retry` loop so a million-element source does
not block an executor for milliseconds in one non-yielding pass, and a staging byte cap like the
script engine's.

**The counterexample that kills it is not the engineering, it is the isolation.**
The derived keys cannot be *pinned* before the cut, because they are not known before the cut. This
tree's cross-shard script engine is the existing proof of what that costs: it PINs every declared key
(`atomic_script_pin` → `AtomicScriptIntent`), and only *then* draws `script_cut`, precisely so that a
racing plain write is forced through MVCC (`atomic_needs_version` returns true for a pinned key) and
can be reconstructed at the cut. A SORT cannot pin what it has not read. Making it atomic requires:

```
read source → pin derived → draw cut → read derived at cut → validate source unchanged → apply
```

with a retry whenever the source moved — and when the source moves the *element set* moves, so the
*pin set* moves, so the retry is a fresh pin/unpin round, not a re-read. That is a general-purpose
distributed transaction over a **dynamically discovered read set**.

**This tree already answers that question, and the answer is no.** A Lua script that touches a key it
did not declare is refused outright:

```
scripting.cc:610  if (!mark_declared(*context, key, 1)) { ... "ERR Script attempted to access an
                  undeclared key" ... }
```

Shipping SORT's dereference across executors while `EVAL "return redis.call('GET', KEYS[1]..'x')" 1 k`
is refused would make the server incoherent: the same hazard, two opposite answers. Rejected, and
shelved rather than half-built (§8).

### Option D — adopt hash tags so a pattern's shard is provable

This is what makes the reference's cluster escape hatch work. TomoKV routes on
`FlatStore::hash_key(<whole key>)`; there is no tag syntax. Introducing one changes the shard of
every key in the product. Out of scope for this lane.

## 4. Recommendation, and what shipped

**A, narrowed to the exact condition that makes the law provable, plus a complete implementation of
the semantics for the case where it holds.**

> The dereference is admitted **iff one executor owns every shard** — `sort_deref_local()`,
> `placement().ex_threads().size() == 1`. That executor is the owner of the source *and* of every
> possible derived key, so it may read all of them and the single-owner law is untouched.

This is the same *kind* of rule the reference uses (decide from the pattern and the placement, never
from the data), it is a boot-time constant, and the semantics that shipped are the complete ones —
so the work a future cross-shard wave needs is the engine, not the command.

Implemented behaviour, byte-exact against vanilla 7.4 (see §7):

* **Substitution templates, not globs.** The FIRST `*` is replaced by the element; `?` and `[…]` are
  ordinary bytes; **a backslash escapes nothing** (`BY w\*x` with keys `w\1x…` substitutes at the
  `*` and reads `w\3x` — verified in both directions).
* **`->field`** selects a hash field only when it begins *after* the `*` and has a non-empty tail.
  `BY h_*->` reads the literal key `h_1->`; `BY x->f*` is a plain key pattern.
* **NULL weights.** Missing key, non-string key (or a hash with no such field) ⇒ NULL. Numeric sort
  treats NULL as 0; alphabetic sort places NULL before every present weight and equal to another
  NULL. Only a *present* non-numeric weight raises
  `ERR One or more scores can't be converted into double`, and `LIMIT` does not suppress it.
* **Ties.** Numeric ties fall back to a byte comparison of the ELEMENTS (a total order, so the answer
  is defined). Alphabetic `BY` ties are left in input order via `std::stable_sort`; the reference
  leaves them wherever its sort algorithm put them and promises nothing.
* **Collation.** `ALPHA` without `STORE` uses `strcoll`, with `STORE` a byte comparison — the split
  the tree already had, now shared by the `BY` path. Because this server never calls `setlocale`,
  its `strcoll` IS the byte order; the reference's is not. See §9 — that is a standing compatibility
  question, not something this lane introduced, and a `BY` pattern shows exactly the same divergence
  a plain `ALPHA` does.
* **`BY <no '*'>` suppresses ordering.** `DESC` and `LIMIT` are still honoured for a list (insertion
  order) and a zset (score order); `DESC` is ignored for a set, whose order is its own.
* **The determinism rule.** Ordering-suppressed + set + `STORE` is forced back to an alphabetic sort,
  so nothing undefined becomes durable. (The reference applies the same rule to a SORT issued from a
  script; SORT is not callable from scripts in this tree, so that half has no site.)
* **`GET`**: `#` is the element; a pattern with no `*` is a constant null (the reference never looks
  a globless GET pattern up); several `GET`s interleave per element; `STORE` writes a null as an
  empty element; an empty result deletes the destination and answers `0`.
* **`SORT_RO`** takes every option except `STORE`.

Refusals, worded like the reference's cluster pair and like this tree's existing ACL pair:

```
ERR BY option of SORT denied when keys formed by the pattern may be owned by another executor
ERR GET option of SORT denied when keys formed by the pattern may be owned by another executor
```

They fire **as the option is parsed**, so error precedence matches the reference exactly
(`BY w_* BADTOKEN` → BY refusal; `BADTOKEN BY w_*` → syntax error), and before any key is read.

Two deliberate divergences from the reference's *cluster* rule, both toward the standalone answer:
a globless `GET` pattern and a `?`/`[` pattern are admitted here (cluster refuses the first,
admits the second), because neither reads a key — so the reply is byte-identical to standalone redis.

## 5. Where the code lives

| file | change |
|---|---|
| `src/cmd/t_sort.h` / `t_sort.cc` | **new.** Pattern parsing, the admission predicate, the derived-key lookup, and `sort_run` — the single ordering/projection implementation now used by every SORT form. |
| `src/cmd/xshard_commands.inc` | `parse_sort_options` gains `BY`/`GET` and the admission check; `sort_image` becomes a thin image-decode wrapper over `sort_run`; `sort_encode_store` / `sort_reply_rows`; the local arm and the cross-shard completion arm rewired. |
| `src/cmd/scatter_engine.inc` | `SortOptions` gains `by_arg` / `get_args` / `deref`; `ResultHeap` gains `present`; `ScatterState` gains `sort_general_done` / `sort_conversion_error`; the phase-1 owner task computes a dereferencing SORT where a `Shard` exists; a dereferencing SORT is kept **off** the same-owner fast path (§6). |
| `src/cmd/t_hash.cc`, `t_hash_ttl.cc` | two read-only exports: a hash-field read and a lapsed-field test that do **not** reap, erase, or emit — the reaping path is written for the key the command was routed on and would act under the wrong name. |
| `src/cmd/multi.inc` | two fixes, both §6: `command_key_args` now names SORT's `STORE` destination (a pre-existing data-placement defect), and a dereferencing SORT is never a `Local` child and participates in every shard so EXEC's stage barrier orders its derived reads. |
| `src/store/flatstore_atomic.inc` | read-back accessors for the per-store read cut and originating connection. |
| `src/core/server.h`, `src/cmd/t_server.cc` | four INFO counters (§7). |
| `tomokv.conf` | documents that the dereference is placement-dependent and has no knob of its own. |
| `Makefile` | `src/cmd/t_sort.cc`, plus the `sortnoctx` negative-control target. |

**Knobs: none.** The reference has no knob for this (it is cluster-mode-implicit), and the
knob-compat rule says a shared feature adopts the reference's grammar. The behaviour derives from
the placement, which is already configured.

**Where the dereference runs.** Always on an executor holding a `Shard&` — the phase-one owner task
outside a transaction, the same-owner arm inside one (EXEC classifies from the registry key range
and never consults `xshard_prepare`, so both arms exist; §6c makes the second one correctly
ordered). It never runs on an IO thread, which owns connections and must not do `O(n log n)` work on
behalf of one of them, and never at cross-shard completion, which holds no `Shard`. `sort_lookup()` additionally re-checks `sort_deref_local` before touching a shard other
than the executing one and counts `sort_deref_escapes` if it ever had to — a detector whose control
is that it reads zero in every run.

## 6. Three defects the differ found

### 6a. This lane's: a dereference read other shards with no read context

The first differ runs passed, then failed **once in ~25** with a deterministic op stream — the
signature of a per-boot hash seed changing where keys land, not of a flaky test. Reproduced by
looping over boots and placements: 5 failures in 40 boots, all at `--atomic 1`, all of this shape:

```
DIFF op 4426 ['SORT', 'so:t', 'BY', 'so:tw*x']
  target: 2 3 1
  oracle: 3 1 2
```

The weights had been written by the `MSET` immediately before, on the same connection.

**Root cause.** The read cut and the originating connection id are **per-`FlatStore`** state
(`atomic_read_epoch_`, `atomic_read_origin_conn_id_`), bound by the owning task on the one shard it
was posted for. A dereference reads *other* shards, which therefore resolved at "latest, no
connection". `atomic_resolve_internal` hides a group's still-private (epoch-0) candidate unless the
reader's `atomic_read_origin_conn_id_` matches it — so this connection's own just-replied cross-shard
`MSET` was invisible and the weight read as absent. Only boots that placed a weight key off the
source's shard were affected.

**Fix, two parts.**
1. A SORT that actually dereferences is kept **off the same-owner fast path**. The local arm has no
   originating-connection id to bind; only the scatter path carries `state.origin_conn_id` and
   `state.snapshot`. Scoped to the forms that truly read a derived key (`SortOptions::deref`), so
   `BY nosort` / `GET #` keep the fast path.
2. `sort_lookup` binds `(read_snapshot, origin_conn_id)` on the derived key's store for the length of
   the lookup and restores exactly what was there (`ForeignReadContext`).

**Regression pin.** `tests/sort.py::phase_ryow` writes the weights and sorts by them on one
connection 40 times with names spread across shards, in both atomic modes. Post-fix: 40 boots × 4
placements × (battery + 2 differ seeds) with zero failures. `make sortnoctx` builds the
negative-control binary in which the fix is compiled out; that phase MUST fail against it.

### 6b. Pre-existing: `MULTI; SORT src STORE dst; EXEC` wrote `dst` into the WRONG SHARD

Reproduced on the **base binary** (`perthread-locality`, built clean in a detached worktree), so it
predates this lane:

```
MULTI ; SORT src STORE dst ; EXEC            -> :3        (looks fine)
LRANGE dst 0 -1                              -> (empty)
EXISTS dst / TYPE dst                        -> 0 / none
KEYS *                                       -> src, dst  (it IS in the keyspace)
24 destination names, 16 shards              -> 24/24 lost
```

`SORT`'s registry key range is `1..1`; its `STORE` destination is an option word. `command_key_args`
in `multi.inc` enumerated only the registry range, so a queued `SORT src STORE dst` looked like a
ONE-key command: `classify_multi_command` saw a single shard, called it `Local`, and EXEC ran the
same-owner arm on the SOURCE's shard — which installed `dst` in the source's table. The key was
then invisible to every routed lookup while `KEYS` and `DBSIZE` still counted it, and a snapshot
would have persisted it under the wrong shard. Outside `MULTI` the same command is correct, because
there `xshard_prepare` uses the lowering's own key enumeration.

**Fix.** `command_key_args` now names the `STORE` destination, parsing the option words with exactly
`parse_sort_options`' grammar (a `BY`/`GET` pattern may itself be the word `STORE`). Same shard stays
`Local` and is correct; different shards fall through to the ordinary two-hop apply. The destination
also reaches `add_write_key`, so `WATCH` on it now sees the write. `SORT_RO` is excluded by name.
Post-fix: 24/24 lost → **0/24**, contents byte-correct, with and without `BY`/`GET`.

### 6c. A dereferencing SORT inside EXEC was not ordered against the shards it dereferences

EXEC does not serialise commands globally: each shard walks only the commands it *participates in*,
so commands over disjoint shards proceed independently. A dereferencing SORT breaks that premise —
its participant set does not name the shards its patterns will read, and those shards are not
knowable until the source has been read. Result, verified against the oracle:

```
MULTI ; MSET w_1 40 w_2 20 w_3 60 ; SORT src BY w_* ; EXEC
oracle: 2 1 3        target (before): 3 2 1   -- the SORT ran before the MSET on those shards
```

**Fix.** A queued SORT whose `BY`/`GET` patterns contain a `*` is never a `Local` child: it is
classified as a cross-shard child and declares **every** shard as a participant. Shards outside its
scatter group hold the barrier only — the existing Xshard arm already waits on `finished` for a
shard with no group of its own — which orders the dereference after every earlier command on every
shard and before every later one. Scoped by `multi_sort_dereferences()`, so `BY nosort` / `GET #`
and every other command keep their existing route.

## 7. Test evidence

Counters added to `INFO stats`, each the negative control of another:

| counter | meaning |
|---|---|
| `sort_deref_lookups` | one per derived key actually read — proves the mechanism fired |
| `sort_deref_refusals` | one per `BY`/`GET` rejected by the admission rule |
| `sort_scatter_general` | one per `BY`/`GET` SORT finished in a phase-one owner task |
| `sort_deref_escapes` | **must be zero**: a dereference that would have read a shard this executor does not own |

### Battery — `tests/sort.py HOST PORT`

```
sort battery: 127.0.0.1:7560, 1 executor(s) -> dereference ADMITTED
-- forms that dereference nothing --
-- single executor: the full BY/GET surface --
-- set/STORE determinism rule --
-- cross-shard arm --
  cross-shard arm ran 12 times
-- read-your-own-writes through BY/GET --
-- dereference inside MULTI/EXEC --
-- randomised model comparison --
  counters: {'sort_deref_lookups': ..., 'sort_deref_refusals': 0,
             'sort_scatter_general': ..., 'sort_deref_escapes': 0}

sort: 396 checks, 0 failures -> PASS
```

```
sort battery: 127.0.0.1:7560, 4 executor(s) -> dereference REFUSED
  counters: {'sort_deref_lookups': 0, 'sort_deref_refusals': 11,
             'sort_scatter_general': ..., 'sort_deref_escapes': 0}
sort: 77 checks, 0 failures -> PASS
```

Both configurations pass under `--atomic 0` and `--atomic 1`, and across six placements:
`--ratio 3:1 --shards 16`, `--ratio 3:1 --shards 1`, `--ratio 5:1 --shards 32` (deref admitted) and
`--shards 16` / `--shards 8` with four executors (deref refused).

### Differ — `tests/differ.py … sort <seed>`

New suite `gen_sort`. Run the target with one executor; with more, every `BY`/`GET` row diffs
against the standalone oracle *by design*, which is the intended signal.

```
DIFFER sort: 4496 ops, 0 diffs -> PASS     (seed 11)      DIFFER sort: 4495 / 0 (seed 3)
DIFFER sort: 4499 ops, 0 diffs -> PASS     (seed 29)      DIFFER sort: 4495 / 0 (seed 101)
DIFFER sort: 4499 ops, 0 diffs -> PASS     (seed 5)       DIFFER sort: 4503 / 0 (seed 13)
DIFFER sort: 4512 ops, 0 diffs -> PASS     (seed 7, RESP3)
DIFFER sort: 4496 ops, 0 diffs -> PASS     (seed 11, --atomic 0)
```

**Boot the oracle under `LC_ALL=C`** (§9). One reply shape is deliberately excluded because the
reference does not define it: a set with ordering suppressed and no `STORE`, which is the set's own
iteration order. Alphabetic ties are excluded for the same reason — every generated weight is
distinct.

### Sanitizers

`make asan` (ASAN + UBSAN, `ldd`-verified to be the sanitizer binary), one executor and four:
battery 396 / 0 and 72 / 0, `differ … sort` 4496 / 0, **zero** sanitizer reports from any SORT path.
The one UBSAN note in the whole run is `third_party/lua/lstring.c:87` misaligned `uint32_t` load,
raised by the script rows of `tests/multi_exec.py` — third-party, pre-existing, untouched here.

### The detector can report failure

`make sortnoctx` builds the release server with §6a's read context and §6c's participation compiled
out. Against it:

```
tests/sort.py   -> FAIL: multi ryow 0..19 order / projection / stored
tests/differ.py -> DIFFER sort: 4496 ops, 4 diffs -> FAIL
```

Against the shipping binary both pass. A green run is therefore not a vacuous one.

### No regression in the shared sort core

`sort_image` was refactored to delegate to `sort_run`, so the pre-existing suites that exercise
`SORT` were re-run at the default placement:

```
multi 4260 / 0   cgaps  3310 / 0   zsetops   4200 / 0   servertail 5339 / 0
list  3521 / 0   set    3524 / 0   zset      3531 / 0   hash       3545 / 0
string 4033 / 0  edgeenc 5989 / 0  arity     4200 / 0   edgeproto  5200 / 0
xshard 4276 / 0  xmove  4263 / 0   compatintro 5630 / 0
```

`multi.inc` changed (§6b, §6c), so the transaction batteries were re-run too:
`multi_exec`, `multires`, `execatomic`, `execiso`, `execfix`, `atomfix` — all PASS, on the release
binary and under ASAN.

## 8. Shelved, with why

**Cross-executor dereference** (Option C). Branch `t-sort` contains the analysis, not the code.
It needs, in order: a heap side-car for synthesised key names and gathered values (they cannot live
in argv or in the prepare-time arena); a group builder that indexes that side-car; `group_cap` raised
to `nshards`; a bounded-pass owner loop so a large source cannot monopolise an executor; a staging
byte cap; a third wave for `STORE` — and, to be as atomic as every other multi-key read here, a
pin/cut/read/validate/apply protocol over a read set that is only discovered mid-flight, with a
retry that re-pins a *different* set each time. The script engine refuses exactly that shape today.
Shipping it without the protocol would make SORT the one multi-key read in this server that can tear
across an atomic group; shipping it with the protocol is a distributed-transaction lane, not a
command lane.

**Keyspace notifications for dereferenced keys.** A dereference uses the non-notifying read on any
shard other than the executing one, so `expired` / `keymiss` events are not emitted for `BY`/`GET`
keys that live elsewhere (they are emitted normally when the derived key is on the source's shard).
The notification sink and its execution scope are per-shard and opening a second one mid-command is
a larger change than this gap is worth. Notifications are off by default.

**`atomic_promote_key` for derived keys.** The scatter path promotes the keys of its own group before
reading them; derived keys are not promoted, so a derived key that had no MVCC record at cut time is
read at its newest value rather than reconstructed at the cut — the same exposure `MGET` has for
non-recorded keys, and strictly narrower than the pre-existing `--atomic 0` behaviour.

## 9. Standing compatibility question: should `ALPHA` collate by LOCALE?

Handed over from the `t-doubles2` lane and confirmed here. Redis calls `setlocale(LC_COLLATE, "")`
at startup and compares `ALPHA` weights with `strcoll()`. TomoKV never calls `setlocale`, so its
`strcoll` is the C locale, i.e. the byte order. Under `en_US.UTF-8` the reference orders `1` before
`-3`; a byte comparison puts `-3` first.

* **Pre-existing, not this lane's.** Reproduced on the base binary with a plain `SORT k ALPHA`, and
  a `BY` pattern shows the identical divergence — same code path, same cause.
* **Controlled for in the tests.** The oracle is booted `LC_ALL=C` (`start_oracle` in the lane's
  server script, and the `gen_sort` docstring says so), which makes redis's `strcoll` the byte order
  and the two sides compare the same relation. Without it every punctuation-bearing `ALPHA` row diffs
  for a reason that has nothing to do with SORT.
* **Recommendation: do NOT adopt locale collation, and say so.** A server whose *reply ordering*
  depends on the environment variables of the process is hostile to anything replicated or
  containerised: two nodes of the same deployment can order the same `SORT` differently, a `SORT …
  STORE` then persists one of those orders, and the reference itself already contradicts its own
  choice by using a byte comparison for the `STORE` form. Byte order is deterministic, reproducible
  across hosts, and matches what `STORE` does. What is missing is not the behaviour but the
  statement of it — this note, plus the `LC_ALL=C` requirement written into the differ suite, is
  that statement. If it is ever revisited, the change is one call in `main` and it should be a knob
  with a default of "byte", not an unconditional switch.

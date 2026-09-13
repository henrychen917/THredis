# NOTES-ZSETFIX — two zset oracle divergences (branch `t-zsetfix`, off `4565b10f9`)

Oracle: vanilla redis **7.4.2** at `/tmp/claude-1000/redis74/src/redis-server`, booted on port 7022
(cores 120-127). Target on port 7021 (cores 16-31). Every rule below was **derived from the oracle
by probe**, then implemented; nothing here was copied from the redis source tree.

---

## Differ: HEAD vs fix

Suites × seeds × atomic modes, `tests/differ.py`, byte-compared against redis 7.4.2.

| suite | seed | HEAD a0 | HEAD a1 | FIX a0 | FIX a1 |
|---|---|---|---|---|---|
| cgaps | 4242 | **2** | **2** | 0 | 0 |
| cgaps | 7 / 29 / 101 | 0 | 0 | 0 | 0 |
| zsetops | 29 | **1** | **1** | 0 | 0 |
| zsetops | 4242 / 7 / 101 | 0 | 0 | 0 | 0 |
| geo | 4242 / 7 / 29 / 101 | 0 | 0 | 0 | 0 |
| string | 4242 / 7 / 29 / 101 | 0 | 0 | 0 | 0 |
| xshard | 4242 / 7 / 29 / 101 | 0 | 0 | 0 | 0 |
| **total (20 cells per mode)** | | **3** | **3** | **0** | **0** |

Raw grids: `scratchpad/zsetfix/differ-HEAD.tsv`, `scratchpad/zsetfix/differ-FIX.tsv` (40 rows each).
Every non-zero row in either grid:

```
$ grep -v '\t0$' scratchpad/zsetfix/differ-HEAD.tsv
HEAD	atomic=0	cgaps	seed=4242	2
HEAD	atomic=0	zsetops	seed=29	1
HEAD	atomic=1	cgaps	seed=4242	2
HEAD	atomic=1	zsetops	seed=29	1

$ grep -v '\t0$' scratchpad/zsetfix/differ-FIX.tsv
(none — all 40 cells are 0)
```

Probe scripts kept alongside: `scratchpad/zsetfix/nzmatrix.py` (the ~150-cell negative-zero matrix,
target vs oracle) and `scratchpad/zsetfix/orderprobe.py` (the 17-cell fold-order probe).

---

## Bug 1 — ZRANGESTORE stored nothing where redis stored an element

### Minimal repro

```
ZADD src 2 b
SORT src ALPHA STORE junk            # <- the load-bearing op: it EXPANDS src
ZRANGESTORE dst src 2 (8 BYSCORE LIMIT -1 4
        HEAD  :0   (and dst absent, so LRANGE dst -> empty array)
        redis :1   (and dst is a zset, so LRANGE dst -> WRONGTYPE)
```

The differ's op 3078 is exactly this shape; op 3083's `LRANGE ... target *0 / oracle -WRONGTYPE`
is purely the inherited consequence of the destination not existing.

### Root cause — two independent defects, both required for the divergence

**(a) A negative `LIMIT offset` was treated as "select nothing", unconditionally.**

* `src/cmd/t_zset.cc:1667` (`emit_score_range`) and `src/cmd/t_zset.cc:1739` (`emit_lex_range`) —
  `if (… || options.offset < 0 || options.limit == 0)` → empty reply.
* `src/cmd/xshard_commands.inc:585` (`select_zrange`, the ZRANGESTORE lowering) — same test.

The oracle only behaves that way on the **listpack** encoding. On the **skiplist** encoding a
negative offset counts back from the END of the matched range. Probed on one logical zset
`{a:1,b:3,c:5,d:7}` held in each encoding:

| query | redis listpack | redis skiplist |
|---|---|---|
| `ZRANGE k 0 10 BYSCORE LIMIT -1 -1` | (empty) | `d` |
| `ZRANGE k 0 10 BYSCORE LIMIT -3 -1` | (empty) | `b c d` |
| `ZRANGE k 0 10 BYSCORE LIMIT -5 -1` | (empty) | (empty) |
| `ZRANGE k 10 0 BYSCORE REV LIMIT -2 -1` | (empty) | `b a` |
| `ZRANGE k [a [z BYLEX LIMIT -2 -1` | (empty) | `c d` |
| `ZRANGE k … LIMIT 0/1/2 …` (non-negative) | identical | identical |

So the rule is: start index `= available + offset` in **iteration** order (REV included), empty if
that goes below zero — and empty always, on listpack. That split is redis's own inconsistency, not
a documented contract; matching the oracle byte-for-byte means reproducing it per encoding.

**(b) `SORT` on a zset did not expand the source, so the encodings disagreed.**

The oracle expands a zset source to skiplist on **every** SORT form (`SORT`, `SORT_RO`, with or
without `STORE`) and never converts back. TomoKV left it compact. Sweeping the whole cgaps/4242
stream for encoding parity found **SORT is the only zset encoding divergence** — 10 occurrences,
every one a `SORT <zset> …`; thresholds already match (`zset-max-compact-entries` 128 /
`-value` 64 = redis's `zset-max-listpack-*`).

Fixing only (a) or only (b) leaves the divergence: (a) alone still answers from a compact key, and
(b) alone reaches an expanded key whose negative offset was still hard-coded to "nothing".

### Fix

* `src/cmd/t_zset.h:44` — `zset_resolve_limit_offset(offset, available, expanded, resolved)`, one
  encoding-selected resolver with the probe table in its comment.
* Call sites: `t_zset.cc:1713/1739/1787/1812` (compact + expanded arms of both range emitters) and
  `xshard_commands.inc:613` (`select_zrange`, now taking the source's encoding).
* `src/cmd/scatter_engine.inc:213` — `ObjectImage::expanded` records the source key's encoding at
  capture time, because the gathered payload is encoding-free.
* `src/cmd/t_zset.cc:2531` — `zset_sort_promote()`, wired at `xshard_commands.inc:1882`
  (same-owner SORT) and `scatter_engine.inc:2046` (cross-shard `SORT … STORE`).

**Destination lifecycle needed no change** — probed and already correct: an empty result DELETES
the destination whether it held a zset, a string, or nothing; a non-empty result replaces a
wrong-type destination; a wrong-type *source* is WRONGTYPE; `dest == src` with an empty result
deletes the key. All of it is now asserted in the battery rather than assumed.

### Deliberate design choices

* `zset_sort_promote()` does its **own live lookup** and is called **before** the command takes its
  object pointer. Two reasons, both load-bearing: expanding an embedded zset *replaces* the KvObj,
  and on the scatter path the gather's `find_value` can return an MVCC-tracked version rather than
  the store's current entry — promoting through that pointer would mutate the wrong object. It
  costs one extra hash probe per SORT (a cold, already-O(n log n) command).
* Allocation failure inside the promotion is **not** reported: the key stays compact and the read
  proceeds. The encoding is a fidelity detail and must never fail a read.

---

## Bug 2 — negative zero in zset scores, diverging in BOTH directions

Both reported directions turned out to be **three** independent mechanisms. Derived from a ~150-cell
probe matrix over {stored +0, stored −0, set member (implicit 1.0), ±1} × {WEIGHTS +w, −w, 0, −0} ×
{SUM, MIN, MAX} × {ZUNION, ZINTER, ZDIFF, *STORE, ZINCRBY, ZADD INCR, ZSCORE, ZRANGE} × {listpack,
skiplist}, run against both servers (`scratchpad/nzmatrix.py`). HEAD diverged in 26 cells; the fix
diverges in **0**.

### The derived rule

| # | rule | evidence (redis 7.4.2) |
|---|---|---|
| **R1** | A score entering **compact/listpack** storage loses the sign of a zero (−0 → +0). **Expanded/skiplist** storage keeps the sign bit. | `ZADD k -0 m; ZSCORE k m` → `"0"` on listpack, `"-0"` on skiplist. Same for `-0.0`, `-0e0`. |
| **R1a** | Promotion carries whatever compact already normalized — the sign cannot come back. | listpack `-0` → `"0"`, then grow past 128 entries → skiplist, still `"0"`. |
| **R1b** | An update whose new score **compares equal** to the old is a no-op, so the stored sign does not flip. | skiplist holding `-0`, `ZADD k 0 m` → `:0`, `ZSCORE` still `"-0"`. (TomoKV already matched.) |
| **R1c** | −0 is the **only** lossy value; every other double round-trips bit-exactly through listpack. | 4000-value fuzz (random bit patterns, subnormals, ±1e300): 0 lossy. |
| **R2** | `AGGREGATE MIN`/`MAX` are **strict** comparisons against the running accumulator, so a tie keeps the **incumbent**. Since −0 == +0, argument order alone decides the printed sign. | `ZUNION 2 zp zn AGGREGATE MIN` → `"0"`; `ZUNION 2 zn zp AGGREGATE MIN` → `"-0"`. |
| **R3** | `ZINCRBY` / `ZADD INCR` reply with the **computed double**, taken before storage normalization. | fresh key: `ZINCRBY k -0 m` → `"-0"`, but `ZSCORE k m` → `"0"`. |
| **R4** | ZUNION/ZINTER fold their sources **smallest-cardinality-first**, ties in argument order (stable). This is what makes R2's "incumbent" well-defined. | card 1(+) vs card 4(−), `AGGREGATE MAX`: **both** argv orders answer `"0"`. Equal cardinalities: argv order decides. |
| **R5** | `reply_double` must **not** normalize. The sign is data. | skiplist `ZSCORE` prints `-0`; RESP3 prints `,-0`. |

Worked examples of the two reported directions:

* differ zsetops/29, `ZUNION … m7 target "0" / oracle "-0"` — R2. HEAD carried a *canonicalizing*
  tie-break (MIN prefers −0, MAX prefers +0) that flipped the sign on exactly the tying orders.
* hand probe `zadd nz1 0 m; zadd nz2 -0 m; ZUNION 2 nz1 nz2 WEIGHTS -1 0` → tomokv `-0`, redis `0`
  — R1. Redis's `nz2` is listpack, so it holds **+0**; `-1*(+0) = -0`, `0*(+0) = +0`, sum `+0`.
  TomoKV stored a real −0, so `0*(-0) = -0` and the sum stayed `-0`.

### Root cause

| rule | HEAD location | defect |
|---|---|---|
| R1 | `src/cmd/t_zset.cc:854` `make_compact_tuple` | wrote the raw double bits into compact storage; no zero normalization |
| R2 | `src/cmd/xshard_commands.inc:902,906` | `(weighted == score && weighted == 0 && signbit(...))` tie-break canonicalized the zero instead of keeping the incumbent |
| R4 | `src/cmd/xshard_commands.inc:873` | heap tie-break `a.source > b.source` folded in **argument** order |

### Fix

* `src/cmd/t_zset.cc:868` — one line in `make_compact_tuple`, the single choke point through which
  a score enters compact storage (both the insert and the update call site). Every writer is
  therefore covered: ZADD/ZINCRBY, snapshot + RESTORE load, and every `*STORE` destination, which
  all funnel through `zset_add_one`.
* `src/cmd/xshard_commands.inc:931` — MIN/MAX become plain strict comparisons.
* `src/cmd/xshard_commands.inc:886` — a stable `fold_rank` permutation by source cardinality; the
  merge heap breaks member ties on **fold rank**, while every membership test still keys on the
  original argument index (so ZDIFF's "source 0 only" rule and ZINTER's seen-count are untouched).

**`src/net/resp.h` `reply_double` was NOT touched** (R5). The brief flagged it as the place where
"everything pays" — measurement showed there is nothing to pay: the oracle prints `-0` for a real
−0, so normalizing there would have been wrong in the other direction. No perf guard is therefore
owed, and none of the changed code is on the GET/SET path (`make_compact_tuple` is zset-only;
`select_zrange`/`compute_zsetop` are cold multi-key paths; the gather's added test is a compile-time
`Kind` compare on the scatter path, which single-key GET/SET never enters).

### R4 was a pre-existing bug that R2 merely made visible

R4 is **not** cosmetic and **not** introduced here — float addition is not associative, so the fold
order changes SUM rounding. With cardinalities ONE=1, NEG=3, BIG=8:

```
ZUNION 3 NEG BIG ONE AGGREGATE SUM      # 1e16 / -1e16 / 1 on member m
        HEAD  1        (folds -1e16 + 1e16 + 1)
        redis 0        (folds 1 + -1e16 + 1e16)
        fix   0
```

Verified directly against the HEAD binary. It was invisible to the differ on HEAD only because
HEAD's canonicalizing R2 tie-break masked the signed-zero signal; fixing R2 exposed it as a new
zsetops/101 diff, which is why the fix arm was re-measured after R4 landed.

---

## Regression coverage

`tests/zsetops.py` — **341 assertions**, every one byte-exact, all passing under `--atomic 0` and
`--atomic 1`:

| section | assertions | what it pins |
|---|---|---|
| pre-existing | 91 | unchanged |
| `negative_zero_battery` | 121 | R1/R1a/R1b/R1c/R2/R3, both encodings, plus `*STORE` destinations |
| `fold_order_battery` | 19 | R4, including the SUM-rounding probe that needs no signed zero |
| `zrangestore_battery` | 110 | destination lifecycle, 16 range specs, the negative-offset matrix in both encodings, SORT promotion |

Non-vacuity is enforced structurally: `make_zset()` **asserts** the encoding it built before any
encoding-selected expectation runs, so a cell can never pass because it silently ran in the wrong
arm. Negative controls included: equal-score updates are no-ops; non-zero doubles round-trip
untouched; non-negative offsets are encoding-independent; `count 0` selects nothing; SORT leaves a
list alone and a second SORT is a no-op; ZDIFF/ZINTER membership is unchanged by the fold order.

**The whole battery also passes byte-identically against vanilla redis 7.4.2** — it is a
differential test of the derived rule, not a transcript of TomoKV's behavior.

### Test corrected

`tests/resp3.py:172` asserted `ZSCORE` of a `-0` stored in a **small** (listpack) zset returns
`,-0`. That pinned a divergence: the oracle answers `,0` there. Updated, and a skiplist arm added
alongside so the row is now a two-sided test of R1 rather than a one-armed constant.

### Batteries and sanitizers

| run | result |
|---|---|
| gate feature batteries (19 tests × 2 atomic modes) | 38 / 38 pass |
| gate debug-surface batteries (4 × 2) | 8 / 8 pass |
| concurrency: `ryow`, `torture`, `atomic_ryow`, `atomic_torn` (4 × 2) | 8 / 8 pass |
| `tests/zsetops.py` × 2 atomic modes | 341 assertions each, pass |
| ASAN+UBSAN (`make asan`, `libasan.so.8` + `libubsan.so.1` link-verified, 12 `__asan_report` syms, no jemalloc) | **0 findings** |
| ASAN: zsetops battery + differ cgaps/4242, zsetops/29, zsetops/101, both atomic modes | all pass, 0 diffs |

---

## Found but NOT fixed (out of scope, with repros)

1. **`reply_double` exponent padding.** `ZADD k 1e-7 m; ZSCORE k m` → tomokv `1e-07`, redis `1e-7`.
   `std::to_chars` emits a two-digit exponent; redis's `fpconv_dtoa` does not.
2. **`reply_double` on large integral doubles.** `ZADD k 12345678901234567890 m; ZSCORE k m` →
   tomokv `12345678901234567168` (the exact value), redis `12345678901234567000` (17 significant
   digits, zero-padded).

Both reproduce on the HEAD binary, both live in `src/net/resp.h` / `src/net/resp3.h`, and both are
a formatting-family question (`to_chars` shortest vs `fpconv_dtoa`) rather than anything this lane
touched. They are not reachable from the current differ suites' value distributions. Deliberately
left alone: the brief gates any `reply_double` change behind a p32 GET/SET measurement, and a
formatting change there pays on **every** reply — it wants its own lane and its own perf arm.

3. **List encoding demotion.** The cgaps/4242 encoding sweep also found `LMPOP` leaving tomokv a
   `quicklist` where redis reports `listpack` (redis shrinks the encoding back; tomokv does not).
   Not a zset issue, not differ-visible today, recorded here so the sweep result is not lost.

4. **`SORT … BY <pattern>`** is a syntax error in TomoKV (`parse_sort_options` rejects BY/GET), so
   the oracle's "even a BY-nosort SORT expands the zset" case is unreachable here. Nothing to do
   until BY lands; noted so whoever adds BY knows the promotion must come with it.

---

## Harness note

The shared oracle on 7022 was killed out from under this lane twice mid-run by a sibling lane's
broad pattern-kill (clean redis log ending at startup, no shutdown line; the 7011/7019 servers
vanished at the same moment). The differ matrix driver therefore re-checks the oracle listener
before every cell and re-boots it rather than silently diffing against a dead port. This lane never
killed by name: every stop resolves pids from `ss -lntp` for its own port and verifies the listener
is gone afterwards.

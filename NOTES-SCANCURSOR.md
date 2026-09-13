# Lane t-scancursor — SCAN and ZSCAN omitted keys that were present for the whole iteration

| Scanner | Cursor BEFORE | Reproduced (own run, transcript below) | Cursor AFTER | AFTER |
| --- | --- | --- | --- | --- |
| `SCAN` | physical slot + 1-bit table selector | **48 of 500 base keys omitted** (9.6%), `EXISTS`=1 for every one | bit-reversed home index | **0** |
| `ZSCAN` | physical position over `cap[0]+cap[1]` | **210 of 3000 base members omitted** (7.0%), `ZSCORE` non-nil for every one | bit-reversed home index | **0** |
| `HSCAN` | bit-reversed home index already | 0 (structurally immune) | unchanged | 0 |
| `SSCAN` | generation + slot, restarts on rebuild | 0 (structurally immune) | unchanged | 0 |
| control: same arms, no churn | — | **0** | — | 0 |
| control: redis 7.4 oracle, all arms | — | **0** | — | 0 |

Both defects reproduced here before any code changed, at `--shards 2` and at `--shards 16`, in a
deterministic single-connection arm and in a concurrent arm. Both now read zero. The gate row
(`tests/concur.py`) FAILS 5 checks on the unfixed algorithm and PASSES all 16 on the fixed one;
both transcripts are below.

---

## 1. What was wrong

`FlatStore::scan` handed the client a cursor built out of the table's *current physical geometry*:

```
bit 32     which of the two live tables (0 = current, 1 = the one a rehash is draining)
bits 31..0 the next physical slot index in that table
```

Nothing in that value survives a resize. Three separate ways it loses keys, all of which I watched
happen:

1. **A rehash starts mid-walk.** `start_rehash` demotes the current table to slot 1 and installs a
   fresh, empty table at slot 0. The client's cursor still says "table 0, slot P" — so the walk
   resumes at slot P of a *brand new* table and slots `0..P-1` of it are never visited. Everything
   the drain later moves into them is gone.
2. **The drain moves keys backwards past the cursor.** `rehash_step` re-inserts each old key at
   `mix64(hash) & new_mask`, which is an arbitrary slot. Any key that lands behind the cursor is
   never emitted, even though it was present the whole time.
3. **The old table is freed under a cursor pointing into it.** When the cursor reaches table 1 and
   the drain finishes, `tab_[1]` becomes null; `scan` reads that as "past the end", returns 0, and
   the client believes the iteration completed.

`ZsetMemberMap::scan` had the same class of bug in a different shape: its cursor is a position in
the *concatenation* `[new table][old table]`, so starting a rehash renumbers what every position
means, and the drain then walks members from the concatenation's tail (ahead of the cursor) into
its head (behind it).

This violated tomokv's own written invariant, `NOTES-SET.md:76-79` — "an element present for the
entire completed iteration is not omitted after any finite sequence of rebuilds". The comment at
the old `FlatStore::scan` claiming that "mutation may duplicate or omit entries, like Redis's SCAN
family" was **factually wrong about Redis** — Redis omits nothing that stays present — and is
deleted, replaced by the derivation of the guarantee we now actually keep.

### The reproduction, before any code changed

Unfixed binary, `--shards 2`, cores 80-95, port 7223 (`concur.py` shape: one client walks `SCAN`
to completion, a second client churns *unrelated* keys; the base set is never touched):

```
UNFIX trial0: calls=206 base_live=500 base_seen=456 MISSED=44 ['base:00008', 'base:00020', ...]
        EXISTS base:00008 -> 1  GET -> b'1'  (never deleted)
UNFIX trial1: calls=206 base_live=500 base_seen=465 MISSED=35
UNFIX trial2: calls=206 base_live=500 base_seen=460 MISSED=40
UNFIX TOTAL MISSED across 3 trials = 119
```

Same binary, same box, collection scanners (`concur2.py` shape — churn other members of the *same*
collection):

```
UNFIX SSCAN  trial0: card=7000  base_live=3000 base_seen=3000 MISSED=0
UNFIX HSCAN  trial0: card=3000  base_live=3000 base_seen=3000 MISSED=0
UNFIX ZSCAN  trial0: card=17000 base_live=3000 base_seen=2918 MISSED=82
        ZSCORE cs:zset base:00019 -> b'1'  (never removed)
UNFIX ZSCAN  trial1: card=15000 base_live=3000 base_seen=2893 MISSED=107
```

The three control arms that read **zero on that same unfixed binary**, which is what makes the
non-zero readings mean something:

```
CTRL  trial0: calls=104 base_live=500 base_seen=500 MISSED=0     (identical arm, churn replaced by PING)
CTRL  trial1: ... MISSED=0
CTRL  trial2: ... MISSED=0
REDIS trial0: calls=28 base_live=500 base_seen=500 MISSED=0      (vanilla redis 7.4, port 7221)
REDIS SSCAN/HSCAN/ZSCAN: MISSED=0
```

`--shards 1` and `--shards 2` both lose keys, so this was never the cross-shard hop.

---

## 2. The fix: reverse-binary (bit-reversed) home cursor

Chosen because the brief prefers it, because our tables already satisfy its precondition
(power-of-two, mask-indexed), and because **the tree already contained a working instance of it**:
`HashFieldMap::scan` in `src/cmd/t_hash.cc` walks exactly this way, which is precisely why HSCAN
reads zero. The rejected option (pinning the old table for the cursor's lifetime) was not
implemented; an abandoned cursor would hold a table forever.

The counter lives in `src/store/flatstore.h` as two free functions,
`scan_cursor_reverse_bits()` / `scan_cursor_next()`, next to the derivation. All three tables use
them now; `t_hash.cc`'s private duplicate was deleted in favour of the shared pair.

### Why it works, and the one thing that is NOT redis's problem

The reverse-binary counter's guarantee rests on: *doubling the table preserves a key's home index
modulo the smaller mask*. Since `home = hash & (2^k - 1)`, `home_big & small_mask == home_small`
always. Counting the cursor with the bits reversed makes the high bits move fastest, so all homes
that share the small table's low bits form one contiguous block of the visit order. A resize can
then only move a key between homes that are on the same side of the cursor.

Redis's dict is **chained**, so its bucket index *is* where the entry lives. Ours is **open
addressed with linear probing**, so a key's physical slot is its home plus an arbitrary probe
displacement. Bit-reversing the *physical slot* would NOT have been sound: the displacement changes
on every rebuild, so a key can move across the cursor without its home moving. That is the trap in
this port, and it is why the cursor is a **home** index and one step visits a whole home:

```
scan_home(t, home):  walk the run of non-EMPTY words starting at `home`, emit the ones whose
                     slot_start(t, hash_key(key)) == home
```

All keys with home `b` are inside that run, because an insert probes forward from `b` and stops at
the first EMPTY, and **nothing in the store ever writes EMPTY back over an occupied word** — erase,
`rehash_step`, `clear_during_snapshot` and the snapshot pre-image mark all leave a TOMBSTONE or
keep the pointer (audited: every write to `tab_[t][...]` in `flatstore.h` is `make_word`, `kTombBit`,
`|= kTombBit`, or `word & ~kTombBit` on a word whose pointer is non-null). So no gap can open
between a key and its home, and displacement stops mattering.

While a rehash drains, one cursor step visits the smaller table's home and **every** home in the
larger table that expands from it, before the cursor advances — the same shape as `dictScan`. The
equal-mask case (our same-size tombstone-reclaim rebuild) visits the one home in both tables.
Growth puts the old table in slot 1 and shrink puts it in slot 0, so the code picks small/large by
capacity rather than by slot.

### Files changed

| File | Change |
| --- | --- |
| `src/store/flatstore.h` | `scan_cursor_reverse_bits` / `scan_cursor_next`; `FlatStore::scan` rewritten; new `scan_home` + `scan_visit`; `bind_rehash_counter`; the wrong comment deleted |
| `src/cmd/t_zset.cc` | `ZsetMemberMap::scan` rewritten; new `ZsetMemberMap::scan_home` |
| `src/cmd/t_hash.cc` | private `reverse_bits`/inlined counter replaced by the shared `scan_cursor_next` (no behaviour change) |
| `src/core/shard.h` | `Stats::rehashes`, bound to the store |
| `src/cmd/t_server.cc` | `INFO stats: keyspace_rehashes`; `SCAN … COUNT <garbage>` error string (see §6) |
| `tests/concur.py` | new — the gate row, 16 checks |
| `tests/differ.py` | new `scan` suite + a SCAN-family completeness property block |
| `tests/gate.sh` | `concur` added to the feature-battery list |

No new runtime knob, so `tomokv.conf` is unchanged.

---

## 3. Every other cursor walker, and why it is or is not affected

| Walker | Where | Verdict |
| --- | --- | --- |
| `FlatStore::scan` | `flatstore.h` | **WAS BROKEN — fixed.** Serves `SCAN` (`t_server.cc`) and the `KEYS` scatter path (`scatter_engine.inc`), so `KEYS` under churn was losing keys too. |
| `ZsetMemberMap::scan` | `t_zset.cc` | **WAS BROKEN — fixed.** |
| `HashFieldMap::scan` | `t_hash.cc` | Immune, and already for the right reason: bit-reversed home cursor with an intrusive per-home chain (`home_head`/`scan_next`), so displacement is irrelevant. Verified 0 misses on the *unfixed* binary. |
| `SetMemberTable` + `cmd_sscan` | `typeval.h`, `t_set.cc` | Immune by a **different** mechanism: the cursor carries a table generation, and any rebuild bumps it so the next call restarts at slot 0. It cannot omit, at the price of duplicates — and of a liveness property worth knowing (§7). Verified 0 misses on the unfixed binary. |
| `ExpireIndex::sample` (`cursor_`) | `flatstore.h` | Not affected. Best-effort active-expiry sampling with no completeness contract; the index is rebuilt wholesale and the cursor reset with it. |
| `ExpireIndex::random_hash` cursor fallback | `flatstore.h` | Not affected — a bounded probe for *a* live hash, no iteration contract. |
| `FlatStore::random_live` | `flatstore.h` | Not affected. `RANDOMKEY` wraps once from a random slot inside one call; no cursor survives the call. |
| eviction `sample_cursor_` | `flatstore.h` | Not affected. Best-effort victim sampling; picking a different victim after a resize is not a defect. |
| snapshot walker `snapshot_pos_` | `flatstore.h` | Structurally immune, and legitimately so: `snapshot_prepare` refuses to start until an in-flight rehash has drained, and `rehash_step` is suppressed for the duration of capture, so `tab_[1]`'s geometry is pinned while the walker holds it. Pinning is acceptable *here* because the capture always completes — unlike a client cursor, which may be abandoned. |
| rehash walker `rehash_pos_` | `flatstore.h` | Immune. `start_rehash` refuses to begin a second rehash, so the old table's geometry is fixed for the whole drain. |
| `FlatStore::for_each` | `flatstore.h` | No cursor — one uninterrupted pass. |
| stream cursors (`XRANGE`/`XREAD`) | `t_stream.cc` | Different species: ordered by stream ID, not by a hash slot. Not affected. |

---

## 4. The gate row: FAILS before, PASSES after

`tests/concur.py HOST PORT` (wired into `tests/gate.sh`'s feature-battery loop, so it runs under
`--atomic 0` and `--atomic 1`). 16 checks, ~2.5 s. Arms:

* **deterministic, single connection, no threads** — the walking connection itself issues the churn
  between calls, so the whole run is one fixed command order. A *quiet dry run* precedes the
  collection arms; it measures the walk length and is itself a control that must miss nothing.
* **concurrent** — a second connection churns unrelated keys/members while the first walks.
* **controls** — the same arms with the mutation replaced by `PING`; `SSCAN`/`HSCAN` carried as
  always-zero rows; and the whole file runs against vanilla redis 7.4 with `oracle` as argv[3].
* **non-vacuity** — every churn arm asserts its mechanism FIRED: `INFO stats keyspace_rehashes`
  must have moved during the walk (new counter, §5), and the collection arms assert the cardinality
  actually grew. Every quiet arm asserts `keyspace_rehashes` did **not** move — a control that
  silently resized would not be a control. Against a build without the counter the file *refuses to
  run* rather than pass vacuously (the `oracle` opt-in is the one documented exception).

Both binaries below are the same tree with the same test file; the "BEFORE" binary differs only in
that `FlatStore::scan` and `ZsetMemberMap::scan` still contain the pre-fix physical-cursor bodies.

### BEFORE — `tomokv-OLDSCAN`, `--shards 16`, port 7223

```
SCAN-family completeness under resize (127.0.0.1:7223)
 -- deterministic single-connection arms (no threads, no timing) --
  FAIL SCAN churn   (deterministic)       calls=340 rehashes_during_walk=112 base_live=2000 base_seen=1971 MISSED=29 ['base:00092', 'base:00105', 'base:00157', 'base:00245'] | EXISTS base:00092 -> 1 (never deleted)
  ok   SCAN quiet   (CONTROL)             calls=336 rehashes_during_walk=0 base_live=2000 base_seen=2000 MISSED=0 []
  ok   ZSCAN churn   (deterministic) dry run (CONTROL) calls=164 base_seen=4000 of 4000, no mutation at all
  FAIL ZSCAN churn   (deterministic)      calls=328 card=4000->peak 4400->4400 base_live=4000 base_seen=3918 MISSED=82 ['base:00058', 'base:00083', 'base:00186', 'base:00195'] | ZSCORE base:00058 -> b'1' (never removed)
  ok   SSCAN churn   (deterministic) dry run (CONTROL) calls=80 base_seen=4000 of 4000, no mutation at all
  ok   SSCAN churn   (deterministic)      calls=597 card=4000->peak 4400->4400 base_live=4000 base_seen=4000 MISSED=0 []
  ok   HSCAN churn   (deterministic) dry run (CONTROL) calls=80 base_seen=4000 of 4000, no mutation at all
  ok   HSCAN churn   (deterministic)      calls=84 card=4000->peak 4400->4400 base_live=4000 base_seen=4000 MISSED=0 []
  ok   ZSCAN quiet   (CONTROL) dry run (CONTROL) calls=164 base_seen=4000 of 4000, no mutation at all
  ok   ZSCAN quiet   (CONTROL)            calls=164 card=4000->peak 4000->4000 base_live=4000 base_seen=4000 MISSED=0 []
 -- concurrent arms (second connection churns an unrelated set) --
  FAIL SCAN churn   trial0                calls=328 rehashes_during_walk=88 base_live=500 base_seen=452 MISSED=48 ['base:00003', 'base:00006', 'base:00010', 'base:00013'] | EXISTS base:00003 -> 1 (never deleted)
  FAIL SCAN churn   trial1                calls=347 rehashes_during_walk=88 base_live=500 base_seen=491 MISSED=9 ['base:00034', 'base:00106', 'base:00108', 'base:00117'] | EXISTS base:00034 -> 1 (never deleted)
  ok   SCAN quiet  (CONTROL) trial0       calls=832 rehashes_during_walk=0 base_live=500 base_seen=500 MISSED=0 []
  FAIL ZSCAN churn                        calls=3277 card=3000->peak 23000->3000 base_live=3000 base_seen=2790 MISSED=210 ['base:00027', 'base:00032', 'base:00035', 'base:00048'] | ZSCORE base:00027 -> b'1' (never removed)
  ok   SSCAN churn                        calls=615 card=3000->peak 23000->3000 base_live=3000 base_seen=3000 MISSED=0 []
  ok   HSCAN churn                        calls=401 card=3000->peak 23000->11000 base_live=3000 base_seen=3000 MISSED=0 []

CONCUR: 11 passed, 5 FAILED -> SCAN churn   (deterministic), ZSCAN churn   (deterministic), SCAN churn   trial0, SCAN churn   trial1, ZSCAN churn
```

The same binary at `--shards 2` fails the same five checks (`MISSED` 51 / 82 / 48 / 38 / 89), so
this is not a shard-count artifact.

To rebuild the BEFORE binary and reproduce this: take this tree, replace the body of
`FlatStore::scan` with a walk over `t = (cursor>>32)&1` / `pos = (uint32_t)cursor` that emits
`ptr_of(tab_[t][pos++])` until `count` slots are checked and returns `(t<<32)|pos`, and the body of
`ZsetMemberMap::scan` with a walk over `pos` in `[0, cap[0]+cap[1])`. Everything else — the test,
the counter, the COUNT parse — stays identical, so the A/B isolates the cursor and nothing else.

### AFTER — `./build/tomokv`, `--shards 16`, port 7222

```
SCAN-family completeness under resize (127.0.0.1:7222)
 -- deterministic single-connection arms (no threads, no timing) --
  ok   SCAN churn   (deterministic)       calls=344 rehashes_during_walk=111 base_live=2000 base_seen=2000 MISSED=0 []
  ok   SCAN quiet   (CONTROL)             calls=336 rehashes_during_walk=0 base_live=2000 base_seen=2000 MISSED=0 []
  ok   ZSCAN churn   (deterministic) dry run (CONTROL) calls=164 base_seen=4000 of 4000, no mutation at all
  ok   ZSCAN churn   (deterministic)      calls=241 card=4000->peak 4400->4400 base_live=4000 base_seen=4000 MISSED=0 []
  ok   SSCAN churn   (deterministic) dry run (CONTROL) calls=81 base_seen=4000 of 4000, no mutation at all
  ok   SSCAN churn   (deterministic)      calls=602 card=4000->peak 4400->4400 base_live=4000 base_seen=4000 MISSED=0 []
  ok   HSCAN churn   (deterministic) dry run (CONTROL) calls=80 base_seen=4000 of 4000, no mutation at all
  ok   HSCAN churn   (deterministic)      calls=84 card=4000->peak 4400->4400 base_live=4000 base_seen=4000 MISSED=0 []
  ok   ZSCAN quiet   (CONTROL) dry run (CONTROL) calls=164 base_seen=4000 of 4000, no mutation at all
  ok   ZSCAN quiet   (CONTROL)            calls=164 card=4000->peak 4000->4000 base_live=4000 base_seen=4000 MISSED=0 []
 -- concurrent arms (second connection churns an unrelated set) --
  ok   SCAN churn   trial0                calls=270 rehashes_during_walk=94 base_live=500 base_seen=500 MISSED=0 []
  ok   SCAN churn   trial1                calls=279 rehashes_during_walk=94 base_live=500 base_seen=500 MISSED=0 []
  ok   SCAN quiet  (CONTROL) trial0       calls=832 rehashes_during_walk=0 base_live=500 base_seen=500 MISSED=0 []
  ok   ZSCAN churn                        calls=3353 card=3000->peak 23000->3000 base_live=3000 base_seen=3000 MISSED=0 []
  ok   SSCAN churn                        calls=538 card=3000->peak 23000->13000 base_live=3000 base_seen=3000 MISSED=0 []
  ok   HSCAN churn                        calls=461 card=3000->peak 23000->5000 base_live=3000 base_seen=3000 MISSED=0 []

CONCUR: 16 checks passed, 0 failed
```

Note `rehashes_during_walk=111` on the passing run, against the same arm's `MISSED=29` before: the
hazard the row guards did occur, 111 times, and nothing was lost. A passing row with that number at 0 would be worthless, which is why the test
fails itself in that case.

### Third control: vanilla redis 7.4, same file

```
$ python3 tests/concur.py 127.0.0.1 7221 oracle
  ok   SCAN churn   (deterministic)       calls=75 rehashes_during_walk=None base_live=2000 base_seen=2000 MISSED=0 []
  ... (all 16 arms) ...
CONCUR: 16 checks passed, 0 failed
```

---

## 5. `INFO stats: keyspace_rehashes` (new)

Sum over shards of keyspace-table rebuilds started (`FlatStore::start_rehash`). One increment per
resize, on an already-cold path; nothing on the GET/SET hot path reads or writes it. It exists so
`tests/concur.py` can prove the resize-during-iteration hazard actually occurred rather than
passing because nothing happened — the vacuous-validation rule. Not subtracted by `CONFIG
RESETSTAT` (it is a lifetime table-geometry counter, not a request statistic).

---

## 6. Incidental defect found and fixed by the new differ suite

`SCAN 0 COUNT abc` answered `ERR syntax error`; redis 7.4 — and `HSCAN`/`SSCAN`/`ZSCAN` in this
same tree — answer `ERR value is not an integer or out of range`. Keyspace `SCAN` was the only
member of its own family getting this wrong. Fixed in `cmd_scan`: unparseable COUNT is an integer
error, a parseable COUNT below 1 stays a syntax error (as redis), and a COUNT above `UINT32_MAX` is
now clamped rather than rejected, since COUNT is a work hint and one call is bounded by the table
regardless. Transcript of the suite catching it, before the fix:

```
  DIFF op 4044 ['SCAN', '0', 'COUNT', 'abc']
    target: b'-ERR syntax error\r\n'
    oracle: b'-ERR value is not an integer or out of range\r\n'
DIFFER scan: 4081 ops, 1 diffs -> FAIL      (seeds 7 and 13, identically)
```

**Left alone, reported, NOT fixed:** `SCAN 0 TYPE <unknown-type-name>` answers `ERR syntax error`
here and an empty batch on redis. That is a compat-surface divergence with nothing to do with
cursor completeness; folding it into this lane would have put an unrelated change in a correctness
commit. It is deliberately excluded from the differ suite (with a comment saying why) so the suite's
verdict stays about cursors. Hand it to the compat lane.

---

## 7. Observed and NOT fixed: SSCAN cannot be starved into omission, but it can be starved into
non-termination

`cmd_sscan`'s generation cursor restarts the walk at slot 0 on every table rebuild. That is what
makes it immune to omission. It also means a client that keeps rebuilding the set faster than the
walker advances will restart the walk forever: an early draft of the test churned the set on every
4th `SSCAN` call and the cursor never returned to 0 in 400,000 calls. The final test bounds its
churn, and documents why in the code.

This is a liveness property under adversarial self-inflicted churn, not a data-correctness defect,
and no arm of the shipped battery hits it. Fixing it properly means giving `SetMemberTable` the same
home-based reverse-binary cursor the other three now use, which is a `t_set.cc` change with its own
encoding-promotion edge cases — out of scope for a lane whose brief is omission. **Shelved,
reported here.** If it is picked up, the pattern to copy is `HashFieldMap::scan`, and the same
`scan_cursor_next` helper is already shared and in place.

---

## 8. Cost (INDICATIVE — this lane runs on loadgen-side cores; these are not throughput verdicts)

Full walk of a quiescent 200,000-key keyspace / 200,000-member zset, single connection, loopback,
same box, server `--shards 16`. `emitted` is exactly 200000 in every cell on both sides, i.e. the
new walk returns every key exactly once, no duplicates, on a stable table.

| Walk | COUNT | calls BEFORE | calls AFTER | wall BEFORE | wall AFTER |
| --- | --- | --- | --- | --- | --- |
| `SCAN` 200k keys | 10 | 63,905 | 58,993 | 2.141 s | 1.822 s |
| `SCAN` 200k keys | 100 | 6,396 | 6,326 | 0.353 s | 0.371 s |
| `SCAN` 200k keys | 1000 | 647 | 647 | 0.138 s | 0.172 s |
| `ZSCAN` 200k members | 10 | 78,644 | 65,602 | 2.815 s | 2.515 s |
| `ZSCAN` 200k members | 100 | 7,865 | 7,711 | 0.587 s | 0.611 s |
| `ZSCAN` 200k members | 1000 | 787 | 785 | 0.286 s | 0.350 s |

(`emitted` = 200000 exactly in all twelve cells. Re-measured on the final binary at `--shards 2`:
59,010 / 6,478 / 656 and 65,602 / 7,711 / 785 — same picture.)

Round trips per full walk are at parity or slightly better. Wall time is up ~20-25% at large COUNT:
the home check recomputes `hash_key(key)` for each slot in a probe run, which `rehash_step` already
does per moved slot and which no GET/SET path touches. That is the price of not lying to the client
about completeness, and it is paid only by scanners.

**Getting there took two iterations, and the first one is worth recording.** Charging COUNT against
*slots examined* (the literal reading of "COUNT is a slot-work hint") doubled the round trips for a
full walk — 12,860 calls instead of 6,396 at COUNT=100 — because a home visit charges the whole
probe run while yielding only the entries that belong to that home. The shipped version charges
COUNT against **homes visited**, which is COUNT's meaning in redis, and keeps a second, looser
budget of `10*COUNT` examined slots so a tombstone-stretched run still cannot make one call
unbounded. Same two budgets `NOTES-SET.md` already documents for `SSCAN`.

### The plain path

Nothing in this lane touches GET/SET dispatch, and that is checkable rather than assertable. The
hot handlers in the shipping binary are **instruction-for-instruction identical** to pristine HEAD:

```
$ objdump -d --no-show-raw-insn --demangle <bin>  (opcode column only, per symbol)
IDENTICAL mnemonics  cmd_get<false, true>  insns=452
IDENTICAL mnemonics  cmd_set<false>        insns=646
IDENTICAL mnemonics  cmd_get<true, true>   insns=370
IDENTICAL mnemonics  cmd_set<true>         insns=571
```

Symbol sizes match too (`0x76d`, `0xa87`, `0x63c`, `0x968` on both). The one counter added outside
the scanners is a single increment inside `start_rehash`, and `Stats::rehashes` was moved to the
**end** of `Shard::Stats` so it cannot push a field the per-op path reads onto another cache line.

p32 GET/SET smoke, 1:9, 200k keys, 32B values, server on cores 80-87 and loadgen on 88-95, five
interleaved pairs (two with the old binary first, three with the new one first):

| | r1 | r2 | r3 | mean |
| --- | --- | --- | --- | --- |
| old binary, ops/sec | 3.617 M | 3.709 M | — | 3.663 M |
| new binary, ops/sec | 3.589 M | 3.672 M | — | 3.630 M |
| new-first: new | 3.594 M | 3.632 M | 3.574 M | 3.600 M |
| new-first: old | 3.645 M | 3.605 M | 3.663 M | 3.638 M |

**INDICATIVE ONLY — not a throughput verdict.** This is a correctness lane sharing a box with other
lanes; the within-arm spread across reps is ±1.5%, wider than the ~1% arm difference, and the
handlers are provably identical machine code, so the residual is placement and box noise. A real
number for this belongs to a lane that owns the machine.

---

## 9. Test evidence

All runs: cores 80-95, ports 7220-7229, server pids resolved from the listening socket.

| Check | Result |
| --- | --- |
| `tests/concur.py` on fixed build, `--shards 2` | 16/16, 0 failed |
| `tests/concur.py` on fixed build, `--shards 16` | 16/16, 0 failed |
| `tests/concur.py`, `--atomic 0` | 16/16, 0 failed |
| `tests/concur.py`, `--atomic 1` | 16/16, 0 failed |
| `tests/concur.py` on redis 7.4 (`oracle`) | 16/16, 0 failed |
| `tests/concur.py` on unfixed algorithm, `--shards 2` and `--shards 16` | 5 FAILED both times |
| `differ.py scan` seeds 7 / 13 / 21 | 4081 ops, 0 diffs, PASS ×3 |
| `differ.py` string, set, zset, hash, xshard, hexpire, zsetops, bitmap, cgaps — seeds 7 and 13 | 0 diffs, 20/20 PASS |
| in-tree batteries: multi_exec blocking stream streamgroups pubsub lua_scripting scriptsurf limits resp3 bitfield dumprestore zsetops geo climon climon2 tracking hexpire servertail lcs | 19/19 PASS |
| in-tree batteries under `--atomic 0` AND `--atomic 1` | 19/19 PASS each |
| torture, ryow, atomfix, debug, notify, acl_categories | 6/6 PASS |
| `evict_battery` sections off / growth / config / lfu | 4/4 PASS |
| `evict_battery` section lru | see note below — flaky on **pristine HEAD too**, not this lane |
| snapshot cut save + reload round trip (`snap_cut_battery` save / verify_cut) | PASS — `mismatch=0 ttl=50/50 post-cut-leaked=0 pre-cut-expired=0` |
| `flush_capture` save, `snap_typed_roundtrip` save | PASS |
| ASAN+UBSAN build, full `tests/concur.py` + `differ.py scan` | see below |

### ASAN/UBSAN

`make asan` from the FINAL sources (an earlier build was discarded because two edits landed while
it was compiling, so its object files straddled two versions of `flatstore.h` — a validation binary
that is not the shipping binary proves nothing). `ldd` shows `libasan.so.8` and `libubsan.so.1`
linked; `ASAN_OPTIONS=verbosity=1` shows the runtime registering root regions at startup; and the
same compiler flags on a scratch program with a deliberate out-of-bounds read do produce a report,
so the zero below is a real zero and not a silent sanitizer.

```
build/tomokv-asan --shards 4, port 7222
  tests/concur.py                    -> CONCUR: 16 checks passed, 0 failed
  differ.py scan seed 7 / seed 13    -> 4081 ops, 0 diffs, PASS (both)
  tests/zsetops.py tests/hexpire.py tests/dumprestore.py -> rc=0, rc=0, rc=0
  grep -cE "ERROR: AddressSanitizer|runtime error" on the server log  ->  0
```

### The one non-green cell, and why it is not mine

`tests/evict_battery.py <port> lru`'s `hot survives >= cold` row failed 1 run in 3 here. Before
attributing it to anything, I ran the same three reps against the **pristine HEAD binary** on the
same boot flags:

```
HEAD  rep1 ok hot=17 cold=13   rep2 ok hot=24 cold=24   rep3 FAIL hot=18 cold=23
FIXED rep1 ok hot=24 cold=20   rep2 ok hot=23 cold=21   rep3 FAIL hot=14 cold=23
```

Same failure rate on both sides, and the margins are single-digit either way — a statistical
assertion run with an ad-hoc `--maxmemory`, not with its own driver (`tests/lru_slow.sh`, which
time-dilates the LRU clock and boots different flags against a different worktree). Pre-existing
flakiness of that row's setup; this lane changes nothing in the eviction sampler. Reported, not
touched, not "fixed" by re-running until green.

### The differ `scan` suite

`python3 tests/differ.py <host> <target> <host> <oracle> scan [seed]`. Two halves:

* a byte-diffed op stream (~4,000 ops) that builds the four table-backed collections past their
  compact encodings, churns them, and then walks the whole **option surface** — bad cursors, bad
  COUNTs, missing option arguments, `NOVALUES`, `MATCH`, and `WRONGTYPE` for each scanner against
  each wrong type;
* a **completeness property block** that walks `SCAN`/`HSCAN`/`SSCAN`/`ZSCAN` to the end on both
  servers at COUNT 7, 50 and 400 and compares the resulting *sets* (cursor values and emission
  order are implementation-defined, so bytes cannot be compared), failing loudly if either side is
  empty so the comparison cannot be vacuous.

Scope note, stated plainly: that property block runs against a **quiescent** keyspace, so it is an
oracle-parity check, not a second detector for the resize bug — a quiescent walk is complete even
with the old cursor. Completeness *under resize* is `tests/concur.py`'s job, and that is the row
proven to fail before and pass after. What the differ suite did catch on its own is §6.

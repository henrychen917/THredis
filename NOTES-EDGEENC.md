# Lane t-edgeenc — hunting errors at ENCODING and SIZE boundaries

Base: `t-merge4` @ `4f4c88082`. Cores 16-31, ports 7400-7402, `make -j8`, every server booted and
stopped through `scratchpad/edgeenc/lane.sh` by its listening-socket pid.

## 1. Verdict table

| # | Hypothesis | Verdict | Where the evidence is |
| --- | --- | --- | --- |
| H1 | Compact ⇄ expanded transitions per type, driven across the threshold in both directions | **NOT REPRODUCED** | `boundary_grid.py` blocks `hash`/`set`/`zset`/`list`, 46,848 ops, 0 semantic diffs; locked by `tests/edgeenc.py` §1–2 |
| H2 | Value size classes, and 96 bytes specifically, inside and outside `MULTI`, single-owner and cross-shard | **NOT REPRODUCED as a size effect** | blocks `strsize`/`listelem`/`hashfield`/`embed`/`copymove`/`sparse`/`keylen`/`xshard`, 53,777 ops, 0 semantic diffs. The 96-byte framing of the known `MULTI` defect is **refuted** — §3 |
| H3 | The zero-copy borrow threshold (`--zc-min`, default 16384), including a value grown across it by `APPEND`/`SETRANGE` | **NOT REPRODUCED** | blocks `zc`/`zcbound` at `zc-min` 16384 **and** 64; `tests/edgeenc.py` §5 runs 64 / 0 / 16384 with the knob read back each time |
| H4 | Segmented send — a connection that has served a large reply answering small ones afterwards | **NOT REPRODUCED** | block `segsend`; `tests/edgeenc.py` §6 pushes a 4 MB reply through a 64 KiB `SO_RCVBUF`, asserts the first chunk really was partial, then byte-checks the next four replies |
| H5 | Embedded vs heap strings, and the collection tail-embed form | **NOT REPRODUCED** | blocks `embed`/`keylen`; `tests/edgeenc.py` §3–4 sweep **key length** 1…256 across the tail line for list, hash, zset and set |
| H6 | Integer-encoded values: INT64 edges, leading zeros, `+`/`-` prefixes, `APPEND`/`INCR` against them | **NOT REPRODUCED** | blocks `intenc`/`intset`; `tests/edgeenc.py` §7. The one apparent diff was the **oracle's** collation locale — §5 |
| H7 (added) | `DUMP`/`RESTORE` and `DEBUG RELOAD` round-trip across every boundary above | **NOT REPRODUCED** | `tests/edgeenc.py` §8–9, 21 shapes each, with a corrupted-payload negative control |
| H8 (added) | Stream node roll-over boundaries (`node_max_bytes` 4096 / `node_max_entries` 100) and hash field TTLs on an embedded hash | **NOT REPRODUCED** | blocks `streamnodes`/`hexpire`/`ttlembed` |
| R1 | `NOTES-EXECISO.md` §10 **(c)** — "`RPUSH` of a large (96-byte) element inside `MULTI` is lost" | **REPRODUCED — but the stated cause is wrong** | §3. Not size-dependent, not list-specific. Shelved for `t-execfix`; reproducer committed |
| R2 | `NOTES-EXECISO.md` §10 **(a)** — a later access inside one `EXEC` does not see an earlier write from the same `EXEC` | **REPRODUCED**, with a new near-deterministic 199-200/200 trigger | §4. Already root-caused by `t-execiso`; shelved, reproducer committed |
| N1 | Three `OBJECT ENCODING` name divergences from vanilla | **NOT DEFECTS** — documented deviations | §5 |
| N2 | `XINFO STREAM` `radix-tree-keys` / `radix-tree-nodes` | **NOT A DEFECT** — a fixed stub for a structure this tree does not have | §5 |
| N3 | `SORT … ALPHA` orders punctuation-bearing members differently from the oracle | **NOT A TARGET DEFECT** — the oracle's collation locale | §5 |

Nothing was fixed in this lane, so **no gate row was added**: the owner rule is that a gate row may
only guard a defect that was made to fail and then made to pass, and both real defects found here
are shelved for the lanes that own them. `tests/edgeenc.py` and the `edgeenc` differ suite are
therefore green on the unfixed tree by construction; §6 shows they are not green *vacuously*.

Totals: **106,545 differential ops** in the boundary grid (run in full in both atomic modes),
**71,868** in the committed differ suite (12 runs of ~5,989 ops: seeds 1-5 plus one RESP3 run,
in each atomic mode), and **1,338 directed checks** in `tests/edgeenc.py`, also run in both
modes. All of it repeated under ASAN at a reduced breadth — §9.

---

## 2. What was built

| File | What it is |
| --- | --- |
| `tests/edgeenc.py` | Directed battery, 10 sections, 1,338 checks. Every promotion arm asserts the encoding changed at the threshold **and** had not changed one entry earlier; the `DUMP`/`RESTORE` arm carries a corrupted-payload negative control; the segmented-send arm asserts the send really was segmented before checking what follows it |
| `tests/differ.py` → `edgeenc` suite | ~5,989 ops per seed. Sizes are drawn from a boundary-heavy ladder, key names from a second ladder, because the tail capacity of a small collection is `good_size()` slack in the **key's own** allocation. The docstring lists what is deliberately *not* generated and why |
| `scratchpad/edgeenc/boundary_grid.py` | The hunting instrument: 20 blocks, 106,545 ops in one pass, diffs split into SEMANTIC and OBJECT-ENCODING-NAME |
| `scratchpad/edgeenc/execexec.py` | The R1 reproducer — shows the loss is a function of the *previous transaction's shape*, not of element size |
| `scratchpad/edgeenc/exec_ryow.py` | The R2 reproducers, including the 199-200/200 one, with the `--atomic 0` negative control; it prints the target's live `atomic` setting so a zero row can never be mistaken for a fix |
| `scratchpad/edgeenc/listmulti.py` | Random list-mutation stream inside `MULTI`, byte-compared — the shape `gen_multi` deliberately omits |
| `scratchpad/edgeenc/minimize.py` | Delta-minimizer for that stream (fresh connections per replay, liveness guard, repeat-based accept — see §7) |
| `scratchpad/edgeenc/lane.sh`, `resp.py` | Listener-pid harness and the RESP client |

---

## 3. R1 — the "96-byte `RPUSH` inside `MULTI`" defect is real, and it is not about 96 bytes

`NOTES-EXECISO.md` §10 (c) records it as *"`RPUSH` of a large (96-byte) element inside `MULTI` is
lost, both atomic modes"*. The loss is real. The size is not the variable, and neither is the type.

**The measured variable is the shape of the PREVIOUS transaction.** All runs `--atomic 0`,
`--shards 4`, target `t-merge4` @ `4f4c88082`, oracle vanilla 7.4, 80 trials each, fresh keys per
trial (`scratchpad/edgeenc/execexec.py`):

| Shape (T1 then T2, both `MULTI`/`EXEC`) | Writes lost |
| --- | --- |
| `T1{k}` then `T2{k}` — T1 touches **one** key | **0 / 80** |
| `T1{k2,k}` then `T2{k}` — T1 touches **two** keys | **61 / 80** |
| `T1{k,k2}` then `T2{k}` (order of the two swapped) | 51 / 80 |
| `T1{k2,k}` then `T2{k2,k}` | 35 / 80 |
| `T1{k2,k}` alone, no second transaction | **0 / 80** |
| `T1{k2,k}` then `T2{k}`, **1-byte** elements | **65 / 80** |
| `T1{k2,k}` then `T2{k}`, 96/95/97-byte elements | 61 / 80 |
| `T1{k2,k}` then `T2{k}`, **hash** (`HSET`) instead of a list | 8 / 80 |
| `T1{k2,k}` then a **bare** (non-`MULTI`) write to `k` | **0 / 80** |

Reading the table:

* **Element size is irrelevant.** One-byte elements are lost slightly *more* often than 96-byte
  ones. The whole 1…200 byte ladder was swept separately and showed no size structure at all.
* **It is not list-specific.** Hashes lose the same way, at a lower rate.
* **The precondition is that key `k` was written by an earlier `MULTI`/`EXEC` that spanned more
  than one shard.** A single-shard transaction leaves nothing behind; the loss is 0/80.
* **The victim must also be a transaction.** A bare write after the same T1 is never lost.

That precondition is exactly the one `NOTES-EXECISO.md` §10 (a) already names for the resolver
defect it root-caused: *"It needs the key to already carry a live MVCC entry."* `EXEC` takes the
atomic admission with `force = true` **regardless of the `--atomic` knob**
(`src/cmd/multi.inc`, and `NOTES-EXECATOMIC.md` §1), so a multi-shard transaction leaves a
committed MVCC record on every key it wrote — at `--atomic 0` as much as at `--atomic 1`. The next
transaction's own candidate carries epoch 0 and loses `atomic_resolve_internal`'s
`epoch >= winner_epoch` comparison to that record.

**So (c) is not a separate defect and there is nothing list-shaped to fix. It is (a), reached at
`--atomic 0` through `EXEC`'s forced admission.** One fix closes both. That matters for whoever
picks (c) up: hunting the list lane's size boundaries for it will find nothing, because there is
nothing there — this lane swept them exhaustively (§1, H1/H2) and they are clean.

**Not a regression.** The same probe on `1ab5a3e20` (the commit *before* the `t-execiso` merge,
built from `git archive` into a scratch tree) gives 0 / 60 / 55 / 38 / 0 / 59 / 59 / 6 / 0 —
statistically the same as HEAD's row above. Pre-existing, as `NOTES-EXECISO.md` states.

Reproduce:

```
source scratchpad/edgeenc/lane.sh
boot_tomo 7400 --atomic 0 ; boot_redis 7401
python3 scratchpad/edgeenc/execexec.py 7400 7401 80
```

A random stream of the same shape — the one `gen_multi` deliberately does not generate — is
`scratchpad/edgeenc/listmulti.py`: 148–327 diffs per 4,000 ops over seeds 1-4 at `--atomic 0`.
Its zero-control (the same stream, vanilla-vs-vanilla on ports 7402 and 7401) is **0 diffs**, so
the detector can report zero.

**Shelved, not fixed.** The fix is one comparison inside `atomic_resolve_internal`, which is the
frozen MVCC resolver and is on every atomic path, not just `MULTI`. The brief for this lane says to
stop and hand over a reproducer rather than take that risk, and `t-execfix` owns it.

---

## 4. R2 — a new near-deterministic trigger for the in-`EXEC` read-your-own-write loss

Same root cause, approached from the other side. `--atomic 1`, 200 trials, fresh keys
(`scratchpad/edgeenc/exec_ryow.py`):

| Reproducer | `--atomic 1` | `--atomic 0` |
| --- | --- | --- |
| `MULTI` / `MSET k0..k4` / `MGET k0..k4` / `EXEC` — no preceding group | 0 / 200 | 0 / 200 |
| `DEL k0..k4` then the same transaction, **pipelined** | **199-200 / 200** | 0 / 200 |
| `DEL k0..k4` then the same transaction, one round trip per command | 3-4 / 200 | 0 / 200 |
| `DEL k0 k1` then `MULTI` / `MSET k0 a k1 b` / `MGET k0 k1` / `EXEC`, pipelined | 146-153 / 200 | 0 / 200 |
| `MULTI` / `SET k v` / `GET k` / `EXEC` — no preceding group | 0 / 200 | 0 / 200 |
| `DEL k k@x` then `MULTI` / `SET k v` / `GET k` / `EXEC` | **134-143 / 200** | 0 / 200 |

The 199-200/200 row is the useful one: five commands long, and the whole `MGET`
comes back `[nil x 5]` while the oracle answers `[a,b,c,d,e]`.

```
DEL k0 k1 k2 k3 k4
MULTI
MSET k0 a k1 b k2 c k3 d k4 e
MGET k0 k1 k2 k3 k4
EXEC                      -> target [OK, [nil,nil,nil,nil,nil]]   oracle [OK, [a,b,c,d,e]]
```

The arming step is any **multi-key write group that names the key**: `DEL`, `UNLINK`, `MSET` and
`MSETNX` all arm it; multi-key *reads* (`MGET`/`EXISTS`/`TOUCH`) do not; a single-key `DEL` does
not, and neither does `DEL k k` (which dedups to one key and never forms a group). Inserting a
2 ms pause after the group drops it to 1/100 and 20 ms clears it, which is what makes it a race
with the group's retirement rather than a deterministic ordering error. Every row is 0/200 at
`--atomic 0` — that is this detector's zero-control.

Also shelved: same resolver, same owner.

---

## 5. Divergences from vanilla that are NOT defects

Recorded so nobody re-files them.

**N1a — `embstr` vs `raw`.** The embed line is `kEmbedThreshold = 192` here and 44 in redis, and
redis reports any `APPEND`ed string `raw` because it over-allocates for growth while this tree
keeps no such reservation. 532 + 545 + 84 + 52 `OBJECT ENCODING` diffs in the grid come from this
one line. Documented: `NOTES-SERVERTAIL.md` §5.

**N1b — lists.** `list-max-compact-value` is an **aggregate payload budget** for the whole small
list here (`src/store/typeval.h`), against redis's per-node entry count. The two knobs cannot be
aligned, so list `OBJECT ENCODING` is not byte-comparable; 110 + 545 grid diffs. The `edgeenc`
differ suite therefore does not ask for it, and `tests/edgeenc.py` drives the aggregate-byte axis
against the target's own knob instead.

**N1c — `listpackex`.** Redis 7.4 renames a listpack hash carrying field TTLs to `listpackex`;
this tree externalizes such a hash and keeps reporting `listpack`. Documented:
`NOTES-HEXPIRE.md` line 275.

**N2 — `XINFO STREAM`.** `radix-tree-keys` and `radix-tree-nodes` are answered with a fixed
`physical ? 1 : 0` / `physical ? 2 : 1` (`src/cmd/t_stream_groups.cc:893`). They count a structure
this engine does not have. 6 grid diffs, introspection only.

**N3 — `SORT … ALPHA`, and a trap for every future differ run.** The grid reported three diffs
where the target ordered `-9223372036854775808` before `42` and the oracle did the reverse. Byte
order is the target's; the oracle is not sorting by bytes. Redis calls `setlocale(LC_COLLATE, "")`
and compares with `strcoll`, and this box's environment is `LANG=en_US.UTF-8`, whose primary
collation level ignores the leading `-`. **Booting the same oracle binary with `LC_ALL=C` takes the
`intenc` block from 3 semantic diffs to 0** — proof it is the oracle's environment, not the target.
`scratchpad/edgeenc/lane.sh` exposes `boot_redis_c` for this. Any suite that sorts
punctuation-bearing members with `ALPHA` will otherwise show phantom diffs that depend on which
locale the oracle inherited.

---

## 6. Why the new tests are not vacuous

* **`tests/edgeenc.py`.** The promotion arms carry the negative control in the same breath as the
  assertion: *at* `max_entries` the small encoding must still be in place, *one past* it the
  expanded one must be — an engine that promoted early, late, or never fails one of the two. The
  `DUMP`/`RESTORE` arm flips one byte in the middle of each payload and requires the restore to be
  refused **and** to create no key. The segmented-send arm asserts the first `recv()` returned less
  than the whole 4 MB reply before it checks what came after it. The `zc-min` arm reads the knob
  back after every `CONFIG SET`, so the three arms are known to have run on three different
  settings. The first run of the battery produced 5 genuine failures (my expectations, not the
  engine's behaviour — the `APPEND` → `raw` rule is redis's, not this tree's); the checks bite.
* **The `edgeenc` differ suite.** Changing exactly one alignment row — the target's
  `hash-max-compact-entries` from 128 to 512 — turns the suite red at exactly the two promotion
  probes and nowhere else:

  ```
  DIFF op 3389 ['OBJECT','ENCODING','edge:hash:entries']  target listpack  oracle hashtable
  DIFF op 3518 ['OBJECT','ENCODING','edge:hash:entries']  target listpack  oracle hashtable
  DIFFER edgeenc: 5989 ops, 2 diffs -> FAIL
  ```

* **The R1/R2 probes.** Both carry an arm that must report zero and does: `--atomic 0` for R2, and
  vanilla-vs-vanilla for the R1 random stream.

---

## 7. Two harness traps this lane walked into (both cost real time)

**A Python `Exception` compares by identity.** The lane's RESP client returned `RespError`
instances for `-ERR …` replies, and `Exception.__eq__` is identity, so *every* error-bearing
operation counted as a diff — hundreds of phantom diffs in the first list-in-`MULTI` runs.
`scratchpad/edgeenc/resp.py` now gives `RespError` value equality and says why. Any harness in this
tree that wraps error replies in an exception has the same hole.

**A truncated replay can stop inside an open `MULTI`.** The delta-minimizer reused one connection
across replays. When a candidate ended mid-transaction, the next replay's `PING` and `FLUSHALL`
came back `+QUEUED`, every reply shifted by one, and the minimizer happily "minimized" the defect
down to a three-command script that reproduces nothing. It now opens fresh connections per replay
and asserts `PING` → `PONG` on both servers first. A dead server is caught by the same guard: the
oracle was killed out from under this lane once (its log ends cleanly, so by an external signal —
other lanes share this box), and without the guard those replays would have looked like target
defects.

**And a third, smaller one, worth stating because it invalidated an early result:** the first
matrix varied the *key name* along with the value size, so it swept shard assignment and value
size together. Fixing the key name took the same matrix from 307 "diffs" to 0. Name both operands
of every comparison.

---

## 8. Scope that was cut, and why

* **No fix for R1/R2.** The fix is inside `atomic_resolve_internal`, which the brief names as
  frozen machinery and which `t-execfix` owns. A precise reproducer was the deliverable instead.
* **No `listmulti` differ suite.** The list-in-`MULTI` stream is committed as a scratchpad probe,
  not as a `differ.py` suite, because a suite that is red on an unfixed tree cannot be handed to
  anyone as a regression gate — the same reasoning `gen_multi` already applies to the same shapes.
  It becomes a one-line suite the moment R1/R2 are fixed.
* **`OBJECT ENCODING` on strings and lists is outside the differ suite** (N1a/N1b). The target-side
  behaviour is locked directly in `tests/edgeenc.py` instead, against this tree's own thresholds.
* **No `MEMORY USAGE`-based proof of the collection tail-embed transition.** The engine exposes no
  "is this value embedded" introspection and `MEMORY USAGE` is our own accounting, so the arm
  proves the transition by construction (the tail capacity is hard-capped at
  `kCollectionEmbedMax = 192`, so a collection with more than 192 encoded bytes is provably out of
  the tail) and then checks contents on both sides of it. Stated rather than glossed.
* **No borrow counter for the `zc-min` arm.** `INFO` carries no zero-copy counter, so that arm can
  prove the knob differed and the bytes matched, but it cannot count borrows. Also stated rather
  than glossed.

---

## 9. Test evidence

```
$ tests/edgeenc.py 127.0.0.1 7400            # --atomic 0 --enable-debug-command yes
SKIPPED: list entry-axis promotion: list-max-compact-entries is 4294967295 (no entry threshold)
edgeenc: 1338 checks, 0 failures -> PASS

$ tests/edgeenc.py 127.0.0.1 7400            # --atomic 1 --enable-debug-command yes
edgeenc: 1338 checks, 0 failures -> PASS

$ for s in 1 2 3 4 5; do python3 tests/differ.py 127.0.0.1 7400 127.0.0.1 7401 edgeenc $s; done
DIFFER edgeenc: 5989 ops, 0 diffs -> PASS      # --atomic 0, five seeds
DIFFER edgeenc: 5989 ops, 0 diffs -> PASS
DIFFER edgeenc: 5989 ops, 0 diffs -> PASS
DIFFER edgeenc: 5989 ops, 0 diffs -> PASS
DIFFER edgeenc: 5989 ops, 0 diffs -> PASS
DIFFER edgeenc: 5989 ops, 0 diffs -> PASS      # --atomic 0, seed 2, RESP3

$ ... same six runs under --atomic 1
DIFFER edgeenc: 5989 ops, 0 diffs -> PASS   (x6, seeds 1-5 plus seed 3 RESP3)

$ python3 scratchpad/edgeenc/boundary_grid.py 7400 7401
block hash         ops= 14499 semantic=0 encoding-name=0
block set          ops=  6845 semantic=0 encoding-name=0
block zset         ops=  3472 semantic=0 encoding-name=0
block list         ops= 22032 semantic=0 encoding-name=110     <- N1b
block strsize      ops=  3708 semantic=0 encoding-name=532     <- N1a
block listelem     ops=  3600 semantic=0 encoding-name=545     <- N1a/N1b
block hashfield    ops=  2200 semantic=0 encoding-name=0
block zc           ops=   160 semantic=0 encoding-name=0
block segsend      ops=   147 semantic=0 encoding-name=0
block intenc       ops=  2352 semantic=3 encoding-name=84      <- N3 (3), N1a (84)
block embed        ops=   990 semantic=0 encoding-name=52      <- N1a
block zcbound      ops=   698 semantic=0 encoding-name=0
block copymove     ops=   728 semantic=0 encoding-name=0
block sparse       ops=  1008 semantic=0 encoding-name=0
block keylen       ops= 41168 semantic=0 encoding-name=0
block streamnodes  ops=  1264 semantic=6 encoding-name=0       <- N2
block intset       ops=   295 semantic=0 encoding-name=0
block ttlembed     ops=   850 semantic=0 encoding-name=0
block hexpire      ops=   154 semantic=0 encoding-name=3       <- N1c
block xshard       ops=   375 semantic=0 encoding-name=0
SEMANTIC: ops=106545 diffs=9      (3 x N3 + 6 x N2; nothing else)

$ ZCMIN=64 python3 scratchpad/edgeenc/boundary_grid.py 7400 7401 zcbound copymove sparse
SEMANTIC: ops=2434 diffs=0        # server booted --zc-min 64

$ python3 scratchpad/edgeenc/boundary_grid.py 7400 7402 intenc   # LC_ALL=C oracle
block intenc       ops=  2352 semantic=0 encoding-name=84       # N3 gone
```

Under ASAN (`g++ -O1 -fsanitize=address`, the `tests/gate.sh` §1 line; `ldd` confirms the
sanitizer is linked and the boot banner says `alloc=libc-malloc`, so this is not the cached
`.make-settings` trap where a stale jemalloc binary gets tested instead):

```
--atomic 0   tests/edgeenc.py            1338 checks, 0 failures -> PASS
--atomic 0   differ edgeenc seed 1       5989 ops, 0 diffs -> PASS
--atomic 0   boundary_grid  keylen embed zcbound copymove sparse intset ttlembed hexpire
                                         45891 ops, 0 semantic diffs
--atomic 1   tests/edgeenc.py            1338 checks, 0 failures -> PASS
--atomic 1   differ edgeenc seed 2       5989 ops, 0 diffs -> PASS
AddressSanitizer / UBSan reports in every server log:  0
```

The suites already in `differ.py` are unaffected by the new generator
(`string` 4033 / `list` 3521 / `hash` 3545 / `set` 3524 / `zset` 3531 / `xshard` 4276 ops, all
0 diffs, seed 1, `--atomic 0`).

`tests/gate.sh` was deliberately **not** touched: nothing here was made to fail and then pass, so
by the owner rule there is no gate row to add — and the gate owns port 7899 and cores 0-7, which
this lane must not take.

The grid was run in full under `--atomic 0` and `--atomic 1`; the only difference between the two
is the `xshard` block, which picks up R1/R2 at `--atomic 1` (33 diffs) and is clean at `--atomic 0`.

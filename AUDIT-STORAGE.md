# AUDIT-STORAGE — storage core, owner path (night lane `t-night-storage`, base 775aeea48)

Scope: `src/store/flatstore.h` outside the read-local regions (owner probe/insert/erase, resize and
incremental migration, ExpireIndex + active-expire cycle, eviction, accounting, tombstones),
`src/store/kvobj.h`, `src/store/eviction.h`, `src/cmd/t_string.cc`. Everything named `read_local_*`,
`foreign_read_safety.h`, `read_local_reclaim.h`, `flatstore_atomic.inc`, `atomic_mvcc.h` and the
settax selector is another lane's tonight and is only cited here where the owner path meets it.

Build facts that frame every finding: `TOMO_READ_LOCAL_SET_TAX_VARIANT` defaults to 0
(`read_local_settax.h:12-14`), so every `#if ... == 2 || == 3` block and every
`if constexpr (kReadLocalSetTaxAtomicRaw)` branch is dead in production; the release `CXXFLAGS`
carry no `-DNDEBUG` (`Makefile:19`), so `assert()` is live in release. Baseline build (pinned
32-39,160-167): exit 0, one pre-existing warning (`src/core/ex_loop.h:1066` unused `store`).

History that constrains tonight: `t-hotpath` (f961bcbc8, reverted twice) reshaped `find()` into a
`find_impl<kNotify>` and measured a net loss on the NIC rig; `b7fd92c07` returned the armed default
to immutable replacement (OWNER LAW). Nothing below touches `find()` or the armed SET lifecycle.

---------------------------------------------------------------------------------------------------

## 1. RANKED FINDINGS

Rank = (expected effect on the owner-path instruction budget or on stability) × (confidence that the
change is provably identical on the wire). "COMMIT" marks findings landed tonight; "LISTED" marks
ones deliberately left (algorithmic, cross-lane, or owner decision).

### F1  try_overwrite does the size-class arithmetic before the same-length test — COMMIT
`flatstore.h:1029-1030` computes `kvobj_alloc_size` + `good_size` + `kvobj_capacity`
(= `kvobj_request_size` + `good_size`) on every SET that finds a Raw, TTL-free key, and only then
(`:1046`) tests `val.n == raw_length`. When the lengths are equal the class test is a tautology
(same key, same klen, same encoding, same TTL ⇒ same request ⇒ same class). Testing the length first
removes 2×`kvobj_alloc_size` + 2×`good_size` (~40-60 instr) from the fixed-value-size SET path,
which is the memtier SET cell. When lengths differ but classes match, `:1050-1053` does
`obj_bytes_ -= kvobj_size(o); …; obj_bytes_ += kvobj_size(o)` where both operands are provably the
same number (`kvobj_size` of a non-Extern object IS the class, and class equality is the
eligibility test one line above). Two dead `kvobj_size` evaluations (~50 instr). The armed variant
already states the same fact (`:2473-2474`). Expected: −2..3% instr on SET-overwrite; wire-identical
(no observable state changes: `obj_bytes_` ends at the same value).

### F2  INCR/DECR/INCRBY/DECRBY and SET-of-an-integer allocate on every call — COMMIT
`t_string.cc:1086-1113` → `store_integer_for` → `make_set_int` (`flatstore.h:1110`) →
`kvobj_new_int` (`kvobj.h:927`, `alloc_raw`) → `insert` → `insert_into` replace (`:2308-2319`) →
`retire_obj` → `kvobj_free` (`sdallocx`). Redis mutates the int in place when the object is
unshared. The unarmed owner path has exactly the eligibility that `try_overwrite` already relies
on, and one fewer hazard: an `Enc::Int` value is never borrowed (`cmd_get` copies ints,
`t_string.cc:353`), its footprint cannot change, and its TTL slot is unchanged when the deadline is
unchanged (INCR preserves TTL; SET requires none). Added `try_overwrite_int`, gated
`!read_local_enabled_` exactly like the in-place raw path (armed keeps immutable replacement).
Expected: INCR family becomes allocation-free (−1 `mallocx`, −1 `sdallocx`, −1 probe, −2
`kvobj_size`); SET of a numeric value on an existing numeric key likewise in the clean TU. The
notify TU keeps SET-of-integer on the replacement path, because there `insert_notify` is what
reports `expired` before `new` and an armed SET has not probed yet (F18); its INCR family takes
the in-place path, since the handler's `find_notify` already reported and reaped. Wire-identical;
the one observable nuance is under `maxmemory-policy *lfu`: the old path RESET the LFU counter to 5 for
every INCR (a fresh object gets `initialize_meta`, `:1427`), the new path increments it like redis
`lookupKeyWrite` does. No test pins OBJECT FREQ after INCR (grepped `tests/`).

### F3  retire_obj + kvobj_free decode the object header twice — COMMIT
`retire_obj` (`flatstore.h:3272`) computes `kvobj_size(o)` = `good_size(kvobj_request_size(o))`
(+ external); `kvobj_free` (`kvobj.h:1233`) recomputes `good_size(kvobj_request_size(o))` to obtain
the sized-free length. Same header decode (KeyExt branch, HasTtl branch, encoding switch) and same
`good_size` twice on every replace and every delete. Added `kvobj_free_with_capacity` so the owner
passes the class it just computed. ~25 instr per replace/erase.

### F4  rehash_step pays 2×kvobj_size per moved slot for a −=/+= that nets to zero — COMMIT
`flatstore.h:3347-3348`: `obj_bytes_ -= kvobj_size(o)` then `insert_into(…, track_expire=false)`
adds it back (`:2299`). `track_expire == false` has exactly one caller (rehash: `:3348`; the other
three callers `:1436`, `flatstore_atomic.inc:1204,1208` pass true), so the flag really means
"fresh object, account it". Making the accounting conditional on it and dropping the subtraction
removes 2×`kvobj_size` (~50 instr) per moved live slot — up to 8 per op while a rehash drains.
The replace branch during rehash is unreachable (one-copy invariant) and remains consistent if it
ever ran (retire subtracts the displaced copy; the moved object stays counted).

### F5  Rehash step latency: 8 serialized DRAM misses per op — COMMIT (prefetch), LISTED (tag)
Each live slot the step moves costs `o->key()` (a KvObj deref — DRAM miss) to recompute the hash,
then a probe into the new table (a second, dependent miss). Eight of those in series is the shape
of the write-tail bump during a grow. Loading the step's eight slot words first and prefetching the
eight objects overlaps the object misses (~8→~1 miss latencies). Pure latency change; the
architectural fix (capacity-relative tag, idea I-1) removes the deref entirely.

### F6  rehash_step ignores insert_into's result → silent key loss if it ever failed — COMMIT
`:3348` discards the bool. On failure the slot is already tombstoned, `live_[1]` decremented and
the object leaked: a key vanishes with no signal. Unreachable by the load bound (below), but the
atomic promote path already aborts on the same condition (`flatstore_atomic.inc:1204`); rehash now
does the same. Load bound: grow at (live+tombs+1) ≥ 70% cap, doubled only if live ≥ 35% cap, one
insert per 8 moved slots ⇒ new-table occupancy ≤ (0.70 + 0.125)·C_old = 41% of 2·C_old; same-size
rebuild ⇒ ≤ 0.35 + 0.125 = 47.5%; shrink (live ≤ 17.5%) ⇒ ≤ 35% + 25% = 60%. Never full.

### F7  find_hash_in recomputes the key hash before testing the cheap TTL flag — COMMIT
`:2250-2251`: `hash_key(o->key()) == h && (o->flags & HasTtl)`; both operands are pure, so testing
the flag first (same object line) skips a full key hash on every non-TTL 15-bit tag collision the
active-expire sampler probes through.

### F8  Second mix64 on the probe critical path — LISTED (cross-lane)
`slot_start` (`:1960`) applies `mix64` to a hash whose final step is already `mix64` (`:1663`); the
router consumes only bits 0..13 (`shard.h:47-52`). Indexing with `(h >> 16) & mask` (bits 16..47,
disjoint from router bits and from the tag's 49..63 for every legal capacity ≤ 2^31) removes two
dependent multiplies (~8 cycles) from in front of every table load in find/insert/erase/scan. The
SCAN cursor only needs "home = f(h) & mask" and keeps working. Every probe site must agree,
including `read_local_find_in` (`:3017`), `read_local_capture_in` (`:3036`) and
`read_local_prefetch` (`:730`) — three excluded sites — so this is a merge-train change, not a lane
change. `ExpireIndex::start` (`:350`) is independent and may keep `mix64`.

### F9  Owner probes call memcmp through the PLT for every key compare — COMMIT (owner probes only)
`Slice::operator==` (`base/slice.h:32-34`) is `n == n && memcmp(...)`; with a runtime length GCC
emits `call memcmp@PLT` (libc dispatch + prologue) for a 10-40 byte key on every hit in `find_in`,
`find_slot_in`, `insert_into`, `erase_in`. A length-dispatched inline compare (overlapping 8-byte
loads for 8 ≤ n ≤ 16, 4-byte for 4 ≤ n < 8, bytes below, memcmp above 16) is exact, never reads
past either buffer, and removes the call. Applied as `kvobj_key_equals` in the owner probes only;
`read_local_find_in`/`read_local_capture_in` keep `Slice==` (other lane). Promoting it to
`Slice::operator==` itself is a one-line merge-train decision. Expected ~1% of GET instr/op; this
one must be measured (instr/op at matched rate) and dropped if it does not show — it is its own
commit for that reason.

### F10  Double probe per SET, triple with options — LISTED (structural)
`store_string` (`t_string.cc:140-160`): `try_overwrite` → `find_without_touch` (probe 1); on
NotPossible → `make_set_string` → `insert` → `insert_into` (probe 2, same run). `cmd_set` with
options adds `store().find()` (`:491/541`) before both. A single-pass "probe once, then overwrite or
install at the slot" is blocked by `rehash_step()` running at the head of BOTH `find_without_touch`
(`:1965`) and `insert` (`:1399`): the slot can move between the probes. Fix needs one rehash step
per op hoisted to the executor (idea I-6). Not tonight.

### F11  insert_into recomputes the size kvobj_new_string just computed — LISTED
`:2299/2313` `obj_bytes_ += kvobj_size(o)`; `kvobj_new_string` had `n` in hand (`kvobj.h:891`).
Threading the size through `insert(h, o)` touches every collection call site; ~25 instr on new-key
SET. Defer to the single-pass restructure.

### F12  Snapshot-prepare window lets table 0 reach 100% load — LISTED (owner/snapshot lane)
`maybe_start_grow` returns true while `snapshot_prepared_` (`:3290`), and `insert` refuses only
when `live+tombs+1 >= cap` (`:1405`). Between `snapshot_prepare` and `snapshot_mark` a busy shard
can run from 70% to cap−1 non-empty; linear-probe runs on misses then approach O(cap) (the
`probes <= cap` bounds at `:2218/2293/2338` are what keeps it finite). Suggest refusing new keys at
≥ 95% while prepared, or letting prepare size the fresh table for that headroom. Changes an error
edge under snapshot only; not a night-lane change.

### F13  RANDOMKEY is O(capacity) per call — LISTED (algorithmic)
`random_live` (`:1556-1580`) walks every slot of both tables (`for step < total`) with reservoir
selection. A 16M-slot shard reads 128 MB per RANDOMKEY (~10 ms owner stall); a client looping
RANDOMKEY stalls the shard. Redis is O(1) expected. The comment defends uniformity over the cheaper
bounded probe (`random_allkeys_candidate` shape, `:2014-2033`). Repro: 1 shard, 5M keys,
`redis-benchmark -n 2000 -c 1 randomkey` vs redis; watch p99 of a concurrent GET. Owner decision.

### F14  Asserts are live in release; the fault-injection hook is never compiled out — LISTED
`Makefile:19` has no `-DNDEBUG`; `flatstore.h:86-88` says the debug allocation-failure surface is
"compiled out when assertions are disabled" — it never is. Cost today: one atomic load per table
alloc (cold) and one `assert` in `make_word` per install (`:1957`, a test+branch). Adding `-DNDEBUG`
is a build-system decision that affects every `assert` in the tree; comment corrected tonight.

### F15  ExpireIndex registration failure is silently ignored — LISTED
`insert_into` `:2301` discards `expires_.insert(h)`'s bool. On sidecar OOM the key still expires
lazily (correct), but never actively, and INFO `expires` undercounts. Redis would fail the write.
Cold; note for the owner.

### F16  make_room_for can start a shrink rehash in the middle of insert — VERIFIED OK
`insert` (`:1398-1402`) evaluates grow before admission; `make_room_for` → `erase` →
`maybe_start_shrink` (`:1458`) may install a half-size table 0 before `:1431` runs. Every later
line re-reads `rehashing()`; the fresh table 0 is empty; the analysis in F6 covers it. No defect,
but the allocation-under-pressure is worth knowing.

### F17  Cleanliness (see §5)

### F18  Armed SET reaps an elapsed key silently — LISTED (notify lane; pre-existing)
In the notify TU `store_string_notify` (`t_string.cc:178-196`) calls `store_try_overwrite<true>`
→ `try_overwrite_notify` (`flatstore.h:1057-1060`, which ignores its sink) → `try_overwrite` →
`find_without_touch` → `live_or_expire`, which reaps an expired resident key with no event; the
later `insert_notify` then finds no candidate and reports `new` without the `expired` redis emits
first. `notify_execute_handler` (`notify.inc:290-299`) does not pre-expire. Untested
(`tests/notify.py:474-479` drives lazy expiry through GET). Tonight's F2 deliberately keeps
SET-of-integer on the replacement path in that TU so as not to widen this. Repro: `CONFIG SET
notify-keyspace-events KEA`, `SET k v PX 20`, sleep 30 ms, `SET k w`; redis emits `expired` then
`set`. Fix belongs to the notify lane: a sink-aware probe in `try_overwrite_notify`.

---------------------------------------------------------------------------------------------------

## 2. RESIZE / REHASH / MIGRATION — audit answers

Generation bumps (every table move must advance the read-local generation): `start_rehash_read_local`
(`:2949`), `rehash_step_read_local` (`:2961`), `install_empty_table_read_local` (`:2806`, nests
under the depth counter), `clear_read_local` (`:2678`), `clear_during_snapshot_read_local`
(`:2708`), `snapshot_mark_read_local` (`:2483`), and the old→new move inside `insert_read_local`
(`moves_from_old`, `:2649-2650`) all hold `ReadLocalTableGuard`. The unarmed movers
(`start_rehash :3316`, `rehash_step :3333`, `snapshot_mark :851`, `clear :1497`,
`install_empty_table :2147`) dispatch to the armed twin on `read_local_enabled_` first, so they never
run armed. `snapshot_handoff_complete` (`:945`) and `snapshot_cancel` (`:957`) move no published
pointer. Verdict: complete. One efficiency note for the other lane: `rehash_step_read_local` opens
the guard (and so bumps the generation, invalidating every in-flight foreign read) even when its
8-slot window holds no live word; opening it on the first live word would be free.

Worst-case step: 8 old slots, each live one = KvObj deref + probe (F5). Bounded, but two dependent
misses × 8. Memory: grow allocates 2×cap (or 1× for tombstone reclaim) while the old table lives
⇒ peak 3× slot words; snapshot prepare adds another 2×cap (`:834-841`) ⇒ also 3× during capture.
Shrink at live ≤ 17.5% with hysteresis vs grow at 70% (`:3303-3312`) — sound. Eviction interplay:
F16. Load bound proof: F6.

## 3. EXPIRY — audit answers

Cycle: `ex_loop.h:1669-1688` — `kActiveExpireChecks = 20` examined sidecar slots per cycle, split
across ≤ 20 shards, so a 20-shard executor examines ONE slot per shard per cycle. The cycle is
housekeeping (`:1643`), so its rate is the executor's idle-pass rate: reaping a million elapsed keys
is O(seconds) when idle and slower under load. Fair (persistent cursor), never a keyspace walk
(counts empty slots), and it finishes an in-flight sidecar migration (`:199`). Per sampled deadline:
one probe (`find_hash_in`) + one full key hash (F7 trims the miss case).

Lazy expire on the owner read path: `live_or_expire` (`:2274-2284`) — one flags branch for non-TTL
keys, no clock read; on expiry an AOF delete record + a second probe (`erase_in` re-probes the run
`find_in` just walked; cold). Clock reads per op: zero — `cached_now_ms_` is refreshed once per
executor pass (`ex_loop.h:560,599`) and published per shard (`shard.h:111-123`); handlers read the
cached `sh.now_ms()` (`t_string.cc:425-426, 508, 613, 1342, 1356, 1391`). TTL encoding: 8-byte
absolute ms at `tail() + klen_ext` (`kvobj.h:216-232`); layout-locked tonight (idea I-4).

ExpireIndex invariants checked: re-registration during migration erases the table-0 copy first
(`:184`) so `live_` never double-counts; moved slots become tombs, never empties (`:308-311`);
backstop `finish_migration` (`:165-168`) cannot fire — new-table occupancy at the end of a move is
≤ (0.70 + 0.125)/2 = 41% doubled, ≤ 0.5 + 0.125 = 62.5% same-size; `collapse_empty` releases the
sidecar so footprint follows the current population (`:289-300`).

## 4. STABILITY — defects fixed or listed

Fixed tonight: F6 (silent key loss on an unreachable rehash failure → abort, matching
`flatstore_atomic.inc:1204`).

Listed with repro ideas: F12 (snapshot-prepare 100% load: 1 shard at 69% load, `DEBUG`-triggered
prepare held open, then insert until `keyspace insert failed`; measure GET miss latency), F13
(RANDOMKEY O(cap)), F15 (sidecar OOM undercount: `tests/flatstore_alloc_fail.py` shape aimed at the
sidecar), F14 (`assert` live in release), F18 (armed SET over an elapsed key drops the `expired`
event; repro under F18).

Checked and sound: size math — `kvobj_alloc_size` (`kvobj.h:720-732`) is `size_t` over inputs
bounded by `kProtoMaxBulkLen` (`t_string.cc:60`); APPEND (`:686-690`), SETRANGE (`:768-772`),
SETBIT (`:794-795`) bound the result before allocating; `accounted_bytes` (`:809-815`),
`projected_bytes` (`:2103-2120`), `round_pow2` (`:1943-1951`), `maybe_start_grow` UINT32 guard
(`:3299`), ExpireIndex doubling guard (`:176`) all guard overflow. Accounting: every table-owned
object is charged exactly once (`insert_into :2299/2313`), uncharged exactly once (`retire_obj
:3273`, `erase_in`, `rehash_step` net zero), collections report deltas through
`ObjectSizeTracker` (`:3433-3455`, 49 sites); `note_object_size_change` (`:980-983`) wraps unsigned
on a bracket violation — it makes drift LOUD in INFO rather than hiding it, which is the right
default (a clamp would be the defect). Every alloc path joins the resize trigger: `insert` grows
(`:1401`), `rehash_step` targets a table sized for it, in-place overwrites move no slot, collections
grow behind a stable header under `budget_admit` (`:1370`) plus the tracker, snapshot installs 2×cap,
`clear` installs 1024, RESTORE/AOF/snapshot loads go through `insert`. Tombstones: erase leaves
TOMB (`:2348`), inserts reuse the first tomb in the run (`:2296`), tomb-heavy tables rebuild at the
same size (`:3291-3298`); `clear_during_snapshot` (`:1530`) leaves a 100%-tomb table that the first
post-capture insert rebuilds. UB smells: none found — `static_cast<KvObj*>(mem)` on malloc'd memory
is well-formed for an implicit-lifetime aggregate under C++20; every unaligned field goes through
`memcpy`; all probe loops carry `probes <= cap` so a 100%-full table (F12) cannot spin.

## 5. CLEANLINESS

- `kvobj.h:18-21` cites `bench/kvobj_footprint` and `TODO(density)`; neither exists in the tree —
  comment corrected tonight.
- `flatstore.h:86-88` claims the debug surface is compiled out (F14) — corrected.
- `try_overwrite_notify` (`:1057-1060`) ignores its sink — kept for API symmetry with the other
  `_notify` twins.
- Six hand-rolled probe loops (`find_in`, `find_slot_in`, `find_hash_in`, `find_any_hash_in`,
  `erase_in`, `insert_into`). A shared `probe_run(t, h, pred)` would dedupe ~60 lines, but it
  changes inlining on the hottest loop in the server and `t-hotpath` showed that shape of change
  sign-flips; left alone, deliberately.
- The `TOMO_READ_LOCAL_SET_TAX_VARIANT` scaffolding threads ~150 preprocessor lines through
  `make_set_string`, `make_set_int`, `kvobj_new_string`, `insert_into_read_local`,
  `try_overwrite_read_local`. Study-only per the owner; flagged for hardcode-or-delete at the
  merge train, untouched tonight.
- `kEmbedThreshold = 192` (`kvobj.h:51`, comment `:717-719`) was validated on the fork's allocation
  shape and never re-measured on this layout with jemalloc classes; not a defect, a stale number.
- No dead encodings: `Enc::{Raw,Int,Extern,Compact}` all constructed; `CollectionEncoding` values
  are the type lanes' business. `watch_deadline`, `find_resident`, `prefetch`, `for_each`,
  `aof_physical`, `random_live`, `script_suspend_eviction` all have callers (`.inc` files included).

---------------------------------------------------------------------------------------------------

## 6. COMMITS (this branch, in order)

Every commit was built pinned (`taskset -c 32-39,160-167 make -j8`) with the baseline's single
pre-existing warning (`ex_loop.h:1066`) and no new one; the layout static_asserts (FlatStore 944,
Shard 1440, Op 336, Client 1984, AtomicEntry 144, ThreadCtx 1408, Config 624) are compile-time and
held on each (no field was added anywhere). No knob was added. Nothing was pushed.

1. 89c67b043 `docs: AUDIT-STORAGE.md` — first cut of this file.
2. a0a2e0be9 `store: SET in-place overwrite tests same-length before the size-class arithmetic;
   drop the zero-delta accounting pair` (F1). Risk: none — identical end state; the memtier
   fixed-size SET cell is the beneficiary. Expected −40..110 instr on SET-overwrite.
3. 0e60c84ed `store: decode the object header once per retire/free; rehash moves stop paying a
   -=/+= that nets to zero` (F3, F4). Risk: low — `kvobj_free_with_capacity` receives exactly the
   value `kvobj_free` recomputed; `fresh=false` has one caller. ~25 instr per replace/erase, ~50
   per moved live slot.
4. 4e42753fd `store: find_hash_in tests the TTL flag before recomputing the key hash` (F7).
   Risk: none; cold path.
5. d39db1cec `store: rehash_step warms the window's objects before moving them, and fails loud on
   a lost key` (F5, F6). Risk: low; the abort is unreachable by the load-bound proof and matches
   `flatstore_atomic.inc:1204`. Measure the write tail during a forced grow (1 shard, insert past
   70% of capacity); the prefetch either shows on p99.99 or is deleted.
6. c6a34fedc `store: allocation-free INCR/DECR/INCRBY/DECRBY and numeric SET over a numeric key
   (unarmed owner path)` (F2). Risk: moderate (new path), gated `!read_local_enabled_` exactly
   like `try_overwrite`; maxmemory admission mirrored; LFU nuance documented; notify-TU SET
   deliberately left on replacement (F18). s6/differ.py exercise INCR/INCRBY/INCRBYFLOAT and
   SET-of-integer; the evict battery covers maxmemory.
7. b19d88e42 `store: owner probes compare short keys inline instead of through memcmp@PLT` (F9).
   Verified exhaustively against memcmp behind PROT_NONE guard pages (lengths 0..40, every
   differing byte position, two bit patterns: 1681 checks, 0 failures, no fault ⇒ no overread;
   harness in the session scratchpad, not committed). Effect is a measurement question: instr/op
   at matched rate on GET p32 / SET p32; drop this commit alone if it does not show.
8. `docs: correct stale storage comments` (§5) — comments only.
9. `docs: AUDIT-STORAGE.md final` — hashes, F18, evidence.

Suggested gate order for the parent: build (release + ASAN), s6 differ vs the redis oracle,
`tests/expireindex.py` (1 shard), `tests/evict_battery.py`, `tests/borrow_registry.py`,
`tests/notify.py`, then the instr/op ladder on GET/SET p32 per commit (7 is the one to A/B alone).

---------------------------------------------------------------------------------------------------

## 7. ARCHITECTURAL / ALGORITHMIC IDEAS (not implemented)

Each idea states how it respects: single-owner writes (no shared-writer index), reads never
obstructed by writes (immutable replacement + QSBR, no reader retries or seqlocks), no in-place
overwrite while read-local is armed, numeric knobs 0=off/−1=auto with self-derived thresholds,
main commands zero-regression, hardcode-or-delete, one file per feature.

**I-1  Capacity-relative slot tag (rehash without touching objects).** Take the 15-bit tag from the
bits of the mixed hash immediately ABOVE the index bits (`tag = (mix >> log2cap) & 0x7fff`) instead
of fixed bits 63:49. Doubling then needs no key deref and no hash recompute: the missing index bit
is the tag's LSB, and the new tag is the old one shifted. Migration becomes a pure slot-word
shuffle — the DRAM miss per moved slot (F5) disappears, and same-size tombstone reclaim needs no
object access at all. Cost: the tag compare uses a per-table shift (one extra field per table, or
derive from `mask_`), and every probe site (owner and read-local) must use the table's shift.
Owner-only writes; readers validate the generation as today and the tag is a filter only (full key
compare stays), so a stale tag can only cost a compare, never a wrong hit. No knob. One file.

**I-2  Single rehash step per op, hoisted to the executor (enables single-pass SET).** Today every
store call (`find`, `find_without_touch`, `insert`, `erase`, `active_expire`) runs its own
`rehash_step`, so a SET that probes in `try_overwrite` and again in `insert` cannot trust the first
slot (F10). If the executor runs exactly one step per task before the handler, handlers can probe
once and act on the slot (overwrite in place, replace at slot, install at first tomb/empty) —
removing a probe from every SET and the repeated `rehashing()` tests. Same bounded work per op;
owner-only; readers see the same single release store per slot. One file (`flatstore.h`) plus one
call site in the executor.

**I-3  Control-byte sidecar for the main table (Swiss-table probe).** A 1-byte tag sidecar
(`ExpireIndex::states_` shape) lets a probe compare 16-64 tags per load and, on the SET-new-key
path, find the first EMPTY in one compare instead of walking words. At 70% load with 15-bit tags the
hit path already averages ~1.05 words, so the gain is on misses and on long tomb runs. +1 B/slot
(`kSlotOverheadPerKey` 12 → ~13.4). Readers load sidecar then word, both published per generation;
no retries. Honest expectation: small; list for completeness, do not build before I-1/I-2.

**I-4  TTL slot semantics.** (a) A key created with a TTL keeps its 8-byte slot for life: PERSIST
writes "no deadline" in place instead of re-headering and copying the value (`kvobj_reheader`,
`kvobj.h:1095`); EXPIRE on a key born with a slot likewise. Unarmed only — the slot is unaligned
(`tail()+klen_ext`) so a foreign reader could tear it; armed keeps re-header. (b) Move the deadline
out of the object into the ExpireIndex (hash → deadline) and keep only a flag on the object; every
TTL-key read pays one sidecar probe, non-TTL keys pay nothing. (c) Narrower encodings (6-byte ms,
or 4-byte seconds since a boot epoch) save 2-4 B per volatile key. All header-layout changes; all
respect immutable replacement when armed. Knob-free.

**I-5  Value copy (the 3.5% memmove) is the design, not a leak.** Bytes arrive in the connection
buffer and must land in the owner's allocation exactly once; the only way below one copy is a
zero-copy receive whose buffers are refcounted — which the "no refcount, single owner" rule
forbids. Diet the surroundings (F1-F4, F9-F11), not the copy.

**I-6  Same-class encoding change in place (Raw↔Int) when unarmed.** Extends F2: SET "123" over a
Raw "abc" of the same class could rewrite header fields in place. A foreign reader could see a torn
header, so this is strictly unarmed; with F2 landed, evaluate whether the remaining alloc count
justifies it (hardcode-or-delete).

**I-7  Exact expiry via hierarchical timing wheel instead of sampling.** O(1) reap per elapsed key,
no barren passes, no `kActiveExpireChecks` split across shards; cost is a wheel link per volatile
key and a per-shard wheel. The sidecar's bounded-sampling shape was chosen to keep the volatile
footprint proportional and allocation-free at 0 volatile keys; a wheel that allocates only on the
first TTL preserves that (0 ⇒ no-alloc). List; measure the reap lag first (INFO `expired_keys`
vs a known elapsed population) to know whether the sampler's lag is a problem in practice.

**I-8  Eviction metadata sidecar when maxmemory is on.** The 5-bit LRU/LFU field in `flags` wraps
every 32 clock ticks; a per-slot byte sidecar allocated only when `maxmemory` is enabled (0 ⇒
no-alloc) gives an 8-bit clock and keeps the owner-only write off the object line that foreign
readers acquire. Threshold self-derives from the clock resolution.

**I-9  Directory-of-segments table (no dual-table resize).** A directory of fixed 64-slot segments
doubled by adding one directory level and splitting segments lazily removes the old/new table,
the resurrection hazard and the per-op `rehashing()` branches; migration per segment is bounded by
64 words. Readers validate a directory generation exactly as they validate the table generation
today. Large change; owner-only; list for the paper phase.

**I-10  Re-measure `kEmbedThreshold` on this layout.** 192 came from the fork's allocation shape.
On jemalloc's 4-classes-per-doubling table a 16 B key + 8 B header + V bytes crosses classes at
V = 168, 200, 232, 296…; the sweet spot for "same-class overwrite" width is a table property, not a
constant. Numeric knob? No — measure once, hardcode.

---------------------------------------------------------------------------------------------------

## 8. LEFT FOR OTHER LANES

- F8 (second `mix64` on the probe path): needs `read_local_find_in`, `read_local_capture_in`,
  `read_local_prefetch` to agree — merge-train change.
- `rehash_step_read_local` opens the table guard on windows with no live word (§2).
- F9's compare helper could become `Slice::operator==` (base) at the merge train.
- Settax scaffolding hardcode-or-delete (§5); `try_overwrite_read_local` and the armed SET
  lifecycle are untouched here by owner law.
- F12 (snapshot-prepare load bound) belongs to the snapshot/persistence lane.
- F13 (RANDOMKEY) and F15 (sidecar OOM) are owner decisions.
- Collections' `ObjectSizeTracker` coverage (49 sites) was not re-audited handler by handler.

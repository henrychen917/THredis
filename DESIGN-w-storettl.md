# w-storettl design: retained TTL slots, exact reap, and eviction sidecars

Base: `b5b4d397d`.  Integrated onto mainline `f40469ea3` on `t-storettl-int`.  This lane changes
only storage expiry/eviction.  It does not change command syntax, reply bytes, persistence
formats, ownership, or reader synchronization.

## Status in this tree

| Item | State |
| --- | --- |
| I-4 retained deadline slot | BUILT, default behaviour (no selector) |
| I-7 hierarchical expiry wheel | DELETED -- lost its own A/B; the sampler is the only key-expiry arm |
| I-4b deadline sidecar prototype | BUILT, `TOMO_TTL_DEADLINE_SIDECAR=0` by default |
| I-8 eviction-metadata sidecar | DESIGNED ONLY -- not implemented here |

I-8 below describes the intended design.  Its half-finished, unverified prototype
(`eviction_sidecar.h` plus the FlatStore/executor/INFO hooks) lives on `t-w-storettl` at
`8bcf46892` and was deliberately left out of the integration: the slot for an object was resolved
by re-probing both tables on every `touch()`, `random_volatile_candidate()` was never given the
matching slot hand-back its `random_allkeys_candidate()` sibling got, and the resulting
`OBJECT IDLETIME`/`FREQ` change (32 -> 256 LRU buckets) needs the eviction battery this lane does
not run.  Resuming it means finishing the slot hand-back first, not re-merging that commit.

## Selecting an arm

The selectors are `-D` defines on `CXXFLAGS`; each defaults to `0` in its own header, so a plain
`make` is always the shipped arm.

```sh
make                                                                    # sampler (shipped)
make CXXFLAGS="-std=c++20 -O2 -g -Wall -Wextra -march=native -pthread -DTOMO_TTL_DEADLINE_SIDECAR=1"
```

`CXXFLAGS` is `?=` in the Makefile, so overriding it replaces the whole line: keep the flags above
verbatim and add the define, or the two arms stop being comparable.  Switching arms changes every
object, so `make -B` (or a `build/` wipe) between arms.

## Laws and layout

- A shard owner is the only writer of every new index and metadata byte.
- Published `KvObj` and table-pointer replacement remains immutable when read-local is armed;
  retirement remains QSBR.  No reader retry, lock, or new sequence is introduced.
- The unaligned in-object deadline is changed in place only while read-local is unarmed.
- Disabled selectors and `maxmemory=0` allocate nothing and add no object/sidecar access to the
  ordinary non-TTL GET/SET path.
- The fixed layouts remain `Op=336`, `Client=1984`, `ThreadCtx=1408`, `Shard=1440`,
  `FlatStore=944`, `Rob<64>=192`, `AtomicEntry=144`, and `Config=624`.
- Experimental choices are compile-time selectors, not new numeric knobs.  Existing numeric
  configuration retains its `0=off` meaning.

The feature implementations live in one dedicated header apiece, with only integration hooks in
the existing owner, executor, and INFO files:

- `store_ttl.h`: TTL selector and retained-slot helpers;
- `eviction_sidecar.h`: maxmemory-only slot metadata (designed, not in this tree).

## I-4: retained deadline slot

`KvObjFlags::HasTtl` becomes a physical statement: the allocation has the eight-byte deadline
slot.  Logical volatility is `slot && deadline >= 0`; `-1` in an existing slot means PERSISTED.
Keeping physical and logical state distinct is necessary because the bit participates in every key
and value offset.

For an unarmed store, PERSIST writes `-1` into the existing slot and removes the active-index
entry.  A later EXPIRE writes its deadline back into that same slot.  Both avoid re-headering,
value copying, allocation, retirement, and a second table publication.  For an armed store both
operations construct an immutable replacement; the replacement retains the slot, and the old
object is retired through the existing QSBR path.  TTL-preserving representation changes carry a
`reserve_slot` bit as well as the logical deadline so `PERSIST; INCR` and collection rebuilds do
not accidentally discard the reservation.  A plain SET without KEEPTTL is a new value lifetime
and may discard it; deletion and reload likewise end the in-memory lifetime.

All expiry predicates must test `deadline >= 0` before `deadline <= now`.  The non-TTL path keeps
its existing flags-first branch and does not read the clock or deadline.  Expire-index insertion is
based on logical volatility, not slot presence.

### Deadline-sidecar prototype

`TOMO_TTL_DEADLINE_SIDECAR`, default `0`, adds a deadline payload to the key `ExpireIndex` and makes
owner TTL probes read that payload after the object flag test.  Non-TTL objects do not probe it.
The prototype intentionally retains the in-object deadline as an immutable-version fallback and
transport field.  Removing those bytes in this tree is not a local header edit: constructors hand
around bare `KvObj*`, MVCC parks old objects in layout-locked `AtomicEntry`, snapshot capture can
retain an older object, and foreign readers may hold a captured old object.  A single hash entry
would let an old object consume a newer object's deadline.

Thus this selector measures the proposed extra hash/cache probe and validates index maintenance;
it does not claim the eight-byte saving.  A production removal would require a `(hash, KvObj*)`
versioned deadline store, QSBR retirement of its cells, and an object-plus-deadline carrier through
every constructor/install path.  At 70% load a minimally exact 16-byte cell costs about 22.9 bytes
per live TTL versus the current eight inline bytes plus the existing hash/state index, so the
prototype must show an allocator/cache win before that wider contract is pursued.  This is also why
narrower deadline encodings are deferred: retained slots already eliminate the hot copy, while the
candidate sidecar does not currently leave bytes to spend.

## I-7: hierarchical expiry wheel -- DELETED

Built as `expire_wheel.h` behind `TOMO_EXPIRE_WHEEL` (11 levels x 64 buckets at 1 ms, nullable
state, exact `(hash, KvObj*)` nodes, plus a millisecond-gated busy-pass hook because the split
executor only samples in its idle sweep).  It lost its own A/B and was removed rather than kept as
a selector, per the hardcode-or-delete rule:

| Load | Wheel | Sampler (shipped) | Delta | Reap lag |
| --- | --- | --- | --- | --- |
| SET with TTL(1-30s) + GET, 1:1 | 15.59M ops/s | 21.28M ops/s | -26% | 38 ms vs 0 ms |
| Plain SET + GET, 1:1 (no TTL) | 23.14M ops/s | 23.02M ops/s | +0.5% (null) | -- |

It was slower exactly where it was supposed to win and a null everywhere else, and it cost an
allocating owner-local structure plus an exact-identity contract through every publication,
retirement, and MVCC-collapse path.  Key expiry is therefore the sampler alone.  What the wheel
commit added and this tree KEEPS: retained TTL deadline slots, logical-volatility-driven index
registration (`track_expire`/`untrack_expire`), and the reap-lag gauge below.  Hash-field TTL
attention was never on the wheel and is unchanged.

INFO keeps `active_expire_reap_lag_ms_max`: the saturating maximum of `now - deadline` observed on a
successful active reap.  It is updated only on the cold reap path, published in existing alignment
holes, and aggregated as a maximum.  Comparing it with `expired_keys` and a known elapsed test
population measures sampler backlog without maintaining a shadow ordered structure.  This is an
absolute lifetime high-water gauge, not a cumulative event count, so CONFIG RESETSTAT deliberately
leaves it intact.  Publication occurs only from an active-expiry cycle; ordinary command-batch
publication gains no store or branch.

## I-8: maxmemory-only eviction metadata

An `EvictionSidecar*` replaces the existing eight-byte eviction sampling cursor in `FlatStore`; the
cursor moves into the heap object.  The sidecar owns one byte array parallel to each live table and
one for a prepared snapshot table.  It is allocated only on `maxmemory: 0 -> nonzero`, freed on the
reverse edge, and counted in the resident migration estimate but not in the stable logical
maxmemory budget.  At the 70% table target the physical cost is about 1.43 bytes per live key.

Metadata belongs to a physical slot, not a value object.  New slots initialize it, same-slot
replacement preserves it, and rehash carries it with the slot word.  A dead slot's byte is
unobservable and is reset on reuse instead of adding a sidecar branch to DEL/expiry.  Snapshot
preimage marks retain it because `kTombBit | pointer` is not an empty slot.  Direct atomic
capacity/exchange paths mirror the same transitions.  Pending-only MVCC versions have no byte and
receive fresh metadata only if they become physical, preserving `AtomicEntry=144`.

Owner lookup/touch and victim scoring use the sidecar byte.  Foreign readers never load or publish
the sidecar, so owner touches no longer dirty the object header line they acquire.  The executor's
cached LRU clock uses all eight bits; its resolution remains the boot-latched
`1 << lru_clock_shift` seconds and its alias horizon self-derives as
`256 << lru_clock_shift`.  LFU remains capped at 31 so this item does not change its probability or
decay model.  No knob is added.

Sidecar/table allocation is transactional.  If first-arm allocation fails, maxmemory remains
enabled and fails closed to no victim/OOM rather than silently exceeding its limit; a later live
configuration refresh may retry.  Disabling maxmemory releases every metadata array.

## Expected effect, risks, and measurement

- I-4 removes allocation/copy/retire work from unarmed PERSIST and from EXPIRE after PERSIST.  Risk:
  confusing slot presence with a live TTL, or failing to carry slot history through a preserving
  rewrite.  Measure `expireindex.py` at one shard, `expwide.py`, `edgetime.py`, notify, s6/differ,
  and allocation/instruction counts for PERSIST -> EXPIRE loops.
- I-7 was measured with the expire batteries plus `expired_keys` and
  `active_expire_reap_lag_ms_max` and lost its own regime (see above); its arm is gone, so the
  remaining key-expiry measurement is the sampler's own reap lag under the same batteries.
- I-8 extends the LRU window eightfold and moves owner touch writes off the acquired object line.
  Risks are a missed topology mirror and enabled-mode cache cost.  Run all `evict_battery.py`
  sections, OBJECT IDLETIME/FREQ, notify, snapshot/atomic eviction cases, and armed foreign-reader
  traffic.
- For every commit compare GET/SET instructions per operation on non-TTL keys with
  `maxmemory=0`.  The required result is null: no sidecar allocation or access and no new clock read.

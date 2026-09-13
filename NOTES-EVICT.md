# Maxmemory and eviction notes

## Scope and provenance

The implementation is owner-local: an executor evicts only from the shard it is executing, and
all selection, metadata mutation, accounting and deletion stay inside `FlatStore`.

- Upstream Redis `src/evict.c`, `src/db.c` and `src/object.c` supplied the policy semantics:
  small random samples for approximate LRU/LFU, logarithmic LFU increments plus time decay,
  earliest-deadline `volatile-ttl`, random allkeys/volatile selection, and the default sample count
  of five. Redis retains a global 16-entry ordered candidate pool across eviction rounds. TomoKV
  instead chooses the best of one shard-local K-candidate round because every round already targets
  the only shard whose budget is exceeded; this also avoids copied key names and stale pool ghosts.
- `/home/user/Projects/wt-round-mainline/src/evict.c` retains Redis's pool/scoring. Its relevant
  differences are the per-IO `current_client[iotid]` safety check and skipping lazily uninitialized
  DBs while sampling. The ownership lesson is applied here at a stronger boundary: an EX operates
  only on its owned shard and never calls socket or propagation code.
- Dragonfly `src/server/db_slice.cc` and `src/server/engine_shard.cc` supplied the closest C++
  shared-nothing comparison: per-shard memory budgets, eviction in the shard owner, admission-time
  eviction hooks, heartbeat eviction, and explicit limits on victims/segments per cycle. TomoKV
  adopts owner-local budgeting and bounded work, but not Dragonfly's DashTable placement victim,
  fibers, transaction locks, RSS feedback, journaling, or cache/store mode split.

This is an independent implementation shaped to TomoKV's table and ownership rules; no reference
source was copied.

## Accounting and shard threshold

For shard `s`:

```text
used_s = FlatStore::obj_bytes_ + 12 * live_keys_s
limit_s = maxmemory / nshards
```

`obj_bytes_` is the existing maintained sum of allocator-class-rounded `KvObj` bytes plus external
value allocations. Replacement admission subtracts the displaced object's exact maintained size;
a genuinely new key adds 12 bytes.

The 12-byte slot estimate is `ceil(8 / 0.70)`: an eight-byte slot divided by FlatStore's 70% target
load. It intentionally does not chase current capacity, tombstones, the transient second table
during incremental rehash, allocator metadata, expiry-index capacity, or retained zero-copy
borrows. Those costs would make enforcement depend on resize history rather than the maintained
object/key counters. Thus maxmemory is a stable logical cache budget, not an RSS ceiling.

Each shard enforces the integer-divided share independently. This makes the decision lock-free and
single-writer, but it is intentionally conservative under skew: one hot/large shard can return OOM
while other shards have unused shares. Division remainder bytes are unused. With `maxmemory <
nshards`, every enabled shard share is zero, so writes cannot fit. `maxmemory=0` is distinct and
means disabled.

## Metadata encoding

The chosen design is spare-bit encoding, so it costs **0 additional bytes per key** both on and
off (therefore below the requested four-byte enabled bound), and `static_assert(sizeof(KvObj) == 8)`
continues to hold.

```text
flags[7:3] = five-bit policy-dependent eviction metadata
flags[2:0] = OwnsExtern, KeyExt, HasTtl layout bits
vlen       = unchanged 32-bit value length
```

The three `vlen` bits contemplated by the alternative encoding are deliberately not used: doing so
would require masking every value-length read even while the feature is off. Keeping metadata
wholly in `flags[7:3]` preserves the foundation's value-access instructions and its existing bulk
limit. TTL re-headering preserves metadata only while maxmemory is enabled. With
`maxmemory=0`, lookup and insertion do not write metadata; the store pays one predicted disabled
branch at each read/write hook and performs no eviction accounting work.

The five bits are policy-dependent:

- LRU uses a 5-bit clock at 256-second resolution. The EX already reads wall time once per pass;
  the store derives the clock from that cached seconds value, so no operation reads a clock.
  Modular subtraction ranks idle time over an approximately 136-minute window. Older keys alias
  after wrap; this is the explicit precision/range tradeoff for preserving both the eight-byte
  header and the exact disabled-path instructions.
- LFU uses a 5-bit logarithmic counter. New keys start at five. Access uses Redis's
  default-factor-10 probability shape, saturating at 31. With no per-key epoch bits, bounded victim
  sampling is also aging: a candidate forgets one count each time it competes in an eviction round.
  This adapts under pressure without a keyspace decay pass. Policy changes do not scan the keyspace;
  old metadata is reinterpreted and converges through accesses/sampling.
- Random, TTL and noeviction policies do not need or update recency/frequency metadata.

## Sampling, victim rules and bounded work

Every eviction round obtains up to `maxmemory-samples` candidates (default 5, valid range 1..64)
from the owning `FlatStore`. Allkeys policies probe random table slots; volatile policies probe the
shard's existing expiry index. Each candidate lookup has 16 random attempts plus a persistent,
bounded sparse-table cursor fallback. Empty probes count toward the bound, so a sparse structure
may yield fewer than K live candidates rather than walking the keyspace.

The incoming/replaced key is excluded from victims. This is required because TTL re-headering and
same-class overwrite can hold its owner-local pointer while admission evicts other keys.

Victim choice within the sample is:

- `allkeys-random`, `volatile-random`: random winner from the sampled keys.
- `allkeys-lru`, `volatile-lru`: greatest modular LRU age; random tie break.
- `allkeys-lfu`, `volatile-lfu`: lowest lazily decayed frequency; random tie break.
- `volatile-ttl`: smallest absolute expiry deadline.
- `noeviction`: no victim.

Admission recomputes projected logical bytes after every deletion and removes at most 16 victims
for one write. Candidate probing, rehash progress (the existing eight slots per operation), and
victim count are all bounded. If the object still cannot fit, if sampling finds no eligible key, or
if a volatile policy has no TTL keys, the write returns exactly:

```text
-OOM command not allowed when used memory > 'maxmemory'.\r\n
```

Expired sampled keys are accounted as expirations rather than evictions. `evicted_keys` is a
single-writer counter in each shard and `INFO` sums it.

## Live configuration propagation

Boot knobs are:

```text
--maxmemory BYTES                 default 0
--maxmemory-policy POLICY         default noeviction
--maxmemory-samples 1..64         default 5
```

`CONFIG SET maxmemory`, `CONFIG SET maxmemory-policy`, and `CONFIG SET maxmemory-samples` publish
new process-wide values. `CONFIG GET` exposes all three. CONFIG runs on the connection's IO owner;
it does not touch stores.

CONFIG publication uses an odd/even atomic sequence so a multi-parameter update is one coherent
snapshot. Each EX takes that snapshot at the beginning of a loop pass. Only when its stable version
changes does it copy plain values and the divided threshold into its owned shards. Operations then
read only those owner-local plain fields: there are no locks or atomics on the op/store path. A SET
reply means publication is complete, not that every EX has already crossed a pass boundary; each EX
applies it before the first task it drains on a subsequent pass. Different executors can therefore
observe the change one pass apart.

## Manual test cases

Do not run these as part of the compile-only gate; they are listed for the owner-provided runtime
test phase.

1. **Disabled baseline:** boot without maxmemory. Compare GET/SET/INCR/TTL/OBJECT replies byte for
   byte with the foundation branch. Confirm `CONFIG GET maxmemory` is `0` and no object metadata
   changes across repeated reads (debug inspection).
2. **Noeviction:** use one shard, set two small keys, then lower maxmemory below current accounted
   bytes. A growing SET/INCR must return the exact Redis OOM line and preserve the old target value.
   DEL and reads must still work. Raising maxmemory live must allow the next write.
3. **Allkeys random:** fill beyond one shard share with distinct keys. Confirm writes succeed by
   deleting keys, `DBSIZE` stays bounded after batch publication, and `INFO`'s `evicted_keys`
   increases.
4. **LRU:** set A/B/C, repeatedly GET A, leave B idle, then force many eviction rounds with
   `maxmemory-samples 64`; B/C should be selected ahead of A statistically. Repeat for
   `volatile-lru` with TTLs on all candidates.
5. **LFU:** set A/B/C, GET A much more often than B/C, then force eviction with samples 64. A should
   survive preferentially. Continue forcing rounds to exercise sample-driven decay, and repeat for
   `volatile-lfu`.
6. **Volatile TTL:** create keys with far, middle and near deadlines, set `volatile-ttl`, then force
   eviction. With a large sample, the nearest deadline should disappear first without incrementing
   the shard's expired counter unless it was already elapsed.
7. **Volatile policy with no TTL keys (required edge):** fill only persistent keys, select each of
   `volatile-random`, `volatile-lru`, `volatile-lfu`, and `volatile-ttl`, then force a new write over
   the share. Every write must return the exact OOM reply, no persistent key may be evicted, and
   `evicted_keys` must not increase.
8. **Mixed volatile set:** persistent A plus expiring B/C. Under every volatile policy, force
   repeated eviction and verify A is never selected. Once B/C are gone, the next over-limit write
   must OOM.
9. **Oversized object / budget bound:** attempt an object larger than the entire shard share and a
   write requiring more than 16 existing victims. It must OOM after at most 16 deletions; the event
   loop must remain responsive.
10. **Skew caveat:** with multiple shards, concentrate keys in one hash bucket range while leaving
    other shards empty. Verify the busy shard enforces `maxmemory/nshards` and can OOM despite global
    unused budget.
11. **Live policy propagation:** pipeline CONFIG changes from one IO connection while writing from
    connections routed through different executors. Once each EX begins its next pass, its next
    write must use the new policy; no partial CONFIG pair should be accepted on validation error.
12. **Expiry and borrows:** force eviction while a large zero-copy GET is outstanding and while TTL
    re-headering another key. Confirm retirement waits for borrow release, accounting drops at
    logical eviction, and there is no double free or resurrected key.

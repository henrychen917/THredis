# Read-local coherence follow-ups (`w-rlideas`)

> Historical research record: configuration recipes and measurements below describe earlier
> revisions. Deleted controls are not accepted by the merged server, and the manual-map
> dispatch-scaling instrument is retired. For current controls and merge dispositions, see
> [DESIGN-KNOBS.md](DESIGN-KNOBS.md) and [MERGE.md](MERGE.md).

Base: `b5b4d397d`.  This lane keeps single-owner stores, immutable replacement, QSBR lifetime,
unobstructed readers, and the existing retry-once local-MGET bound.  It adds no runtime knob and
does not change the owner/scatter fallback, which remains the semantic authority.

## 1. MGET cell recheck

The cellcheck lane removed the coarse command-wide shard-generation sweep, but each MGET element
still validates its copied value against `probe_sequence`.  Atomic physical exchanges bracket that
word for point-GET safety, so an unrelated MSET key can invalidate a long capture/copy window and
account as `seq_churn`.

For filter-on MGETs of at most 128 keys, snapshot every queried filter-cell touch epoch before any
value load, irrespective of the shard pending hint.  Keep the probe's existing stable-topology
handshake, but do not perform a second per-element table-word validation after copying.  Once the
whole reply is private, acquire-fence and re-read each queried cell exactly once.  Any mismatch
retries the whole command once and then falls back as `Generation`; probe-local topology churn stays
`SeqChurn`.  Filter-off and larger MGETs keep the existing generation/validation control path.

The atomic exchange bracket remains: point GET still needs it when a negative filter observation
races filter publication.  For MGET, an epoch sampled before an add differs at the final pass; an
epoch sampled after the add synchronizes after the cell store, so the probe sees a positive cell.
If a cell goes 0 -> 1 -> 0 wholly inside the copy window, add and close advance the monotonic epoch
twice, preventing ABA.  Collisions only cause conservative retry/fallback.  Immutable objects plus
rotation QSBR keep a successful probe safe after its topology check, and broad clears touch every
cell through the existing poison protocol.

Expected effect: remove the long per-element exposure to unrelated atomic exchange brackets, sharply
reducing `read_local_mget_generation_retries` and `read_local_mget_fallback_seq_churn` in mm91/mm11.
Risk is an omitted key or conditional epoch snapshot; both would be false negatives, so the bounded
path snapshots and rechecks the complete argv keyset.

## 2. Demotion pre-check verdict pass-through

The parser already hashes every bounded precise-MSET key for its RYOW keyset and every MGET key for
admission.  While each hash is live, also form the pending-read-filter verdict and pass the aggregate
miss into `ReadLocalDemotionPlan::prepare`.  Only an explicit miss for a proven-precise keyset may
early-return; hit or unknown still runs the unchanged exact overlap/closure plan.  The current MGET
fallback already proves its pending set empty and passes that verdict directly.

Expected effect: disjoint MSET-K avoids a second K-key hash walk.  Risk is treating a partial keyset
as proof; wide/imprecise commands therefore pass unknown and preserve demote-all behavior.

## 3. Reclaim-side write prefetch experiment

Add a compile-time `TOMO_READ_LOCAL_RECLAIM_PREFETCHW` selector with legal values 0/1 and default 0.
When selected, the owner write-prefetches at most the first three 64-byte lines of a retired KvObj
immediately before the actual post-grace free.  Borrow-delayed objects are prefetched only when they
are really released.  Default builds emit no hint and retain current behavior.

Expected effect: jemalloc's LIFO tcache can return the just-freed block to the next SET, so the hint
may move two or three ownership fetches out of allocation/initialization's critical path.  Risk is
extra bandwidth or a null/negative saturated-NIC result; it stays OFF unless the NIC A/B proves it.

## 4. Migrated-object free telemetry

Key-LB rebinds a shard's reclaim sink to a new owner without moving its existing allocations.  On
jemalloc builds, remember the bound owner arena in read-local sidecar state.  After a real arena
rebind, classify a KvObj immediately before its actual free with `arenas.lookup`; increment an
owner-only counter when the allocation arena differs from the current owner arena.  INFO sums the
counter as `read_local_migrated_object_frees`.  Non-jemalloc builds and shards never rebound report
zero and do no lookup.  This observes provenance only; allocation, free, routing, and reclamation
order do not change.

Expected effect: the coordinator can distinguish a key-LB mixed-arena population from ordinary
same-owner reclaim while comparing `--key-lb 0 --client-lb 0`.  Risk is diagnostic lookup cost on a
shard after migration; report it alongside migrations and compare the disabled-LB control.

## Measurement

Use mm91/mm11/mm991 at atomic 1 and require the local-MGET retry/fallback counters to fall; run pure
GET as the null and mixed 1:9/1:1 plus RYOW, B+, and bplus suites.  Measure selector 1 for item 3 only
in the later NIC A/B.  In all builds retain the signed layouts: Op 336, Client 1984, ThreadCtx 1408,
Shard 1440, FlatStore 944, Rob<64> 192, AtomicEntry 144, Config 624.

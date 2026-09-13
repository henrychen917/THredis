# Small-collection memory diet

## Resident layout

Small hashes, lists, sets, and zsets now use `Enc::Compact` and one allocator block:

```text
[ KvObj 8 ][ long-key length? 4 ][ TTL? 8 ][ key K ][ metadata 32 ][ Compact bytes E ][ class slack ]
```

The representation follows tomokv's existing embedded-string architecture. Redis/Valkey's single
listpack allocation and Dragonfly's packed small-object outer establish the allocation target, but
no foreign object wrapper was imported. `sizeof(KvObj)` remains 8.

The 32-byte metadata tail is:

| Offset | Bytes | Meaning |
| ---: | ---: | --- |
| 0 | 4 | logical entry count |
| 4 | 4 | encoded Compact byte count |
| 8 | 8 | Compact payload-byte count |
| 16 | 8 | type auxiliary word 0 |
| 24 | 8 | type auxiliary word 1 |

Hash uses the auxiliary words for logical field/value bytes and PRNG state. Set packs its
high-water member length, integer width, and integer/generic tag into word 0. List and zset leave
both words zero. The metadata follows a variable-length key, so scalar fields are loaded and stored
with `memcpy`; the format does not assume natural alignment.

Handlers use `CollectionRef`/`CompactView`, stack-only facades which expose the same packed-entry
operations over either resident bytes or the existing external `Compact`. A write that cannot fit
the already-paid `KvObj` size class first migrates to the old external compact wrapper. Promotion
from compact to hashtable/deque/btree remains a separate, later, one-way transition at the existing
type threshold.

## Embed capacity math

For key length `K`, initial encoded bytes `E`, optional long-key field `X` (0 or 4), and optional
TTL field `T` (0 or 8):

```text
request = 8 + X + T + K + 32 + E
class   = good_size(request)
usable embedded bytes = min(192, class - (8 + X + T + K + 32))
```

`vlen` anchors the original request, so later mutations may consume allocator-class slack without
changing accounting. The preflight computes the operation's projected encoded peak before an
`ObjectSizeTracker` is constructed; overflow migrates the object first. Set additionally accounts
for the transient left-to-right integer width/entry peak before an integer-to-generic conversion.
TTL re-headering sizes the replacement from the current encoded byte count, copies the resident
tail, and preserves eviction metadata. External pointer ownership still follows `OwnsExtern`.

All four types share the same common 192-byte ceiling; their effective capacity differs only by
key/TTL prefix and allocator class. The type-specific payload math is:

- hash: one outer Compact entry per field/value pair; each payload contains nested
  `[field length][field][value]` bytes;
- list and generic set: one Compact entry per element/member;
- integer set: one Compact entry per fixed 2-, 4-, or 8-byte integer;
- zset: one outer entry containing the member encoding plus the 8-byte score.

## Five-element footprint

These are exact `used_memory` deltas with jemalloc on this box for the existing five-element probe:
the seven-byte key `8000000`, `f0/v0` through `f4/v4` for hash, `e0..e4` for list, `m0..m4` for
set, and `m0..m4` plus scores for zset.

| Type | Encoded `E` | Request | `good_size` / expected object | Previous measured | Reduction |
| --- | ---: | ---: | ---: | ---: | ---: |
| hash | 30 | 77 | 80 B | 256 B | 176 B (68.8%) |
| list | 15 | 62 | 64 B | 256 B | 192 B (75.0%) |
| set (generic) | 15 | 62 | 64 B | 356 B | 292 B (82.0%) |
| zset | 55 | 102 | 112 B | 240 B | 128 B (53.3%) |

The live measurements matched these four size classes exactly. Deleting each object returned
`used_memory` to its exact baseline.

## Lazy indexes and right-sizing

External `Compact` keeps no circular `offsets_` allocation through 16 entries. Access below that
cutover linearly decodes the bounded byte stream. The first insertion above the cutover builds the
index once; it is retained until `clear()` or destruction. Embedded blobs never need a side
allocation and linearly scan at most 192 encoded bytes. `Compact::logical(Entry)` remains the only
absolute-to-logical bridge; all offset-taking APIs retain logical-offset semantics.

The external byte buffer starts at 32 bytes and grows by 1.5x. The byte buffer and index growth
targets are rounded through `good_size`, and accounting reports allocator classes rather than raw
vector capacities. Removing Set's duplicate entry-offset vector also removes its extra allocation
and `SetMemberTable`-adjacent bookkeeping from compact objects.

## Deliberately not done

- No compact-to-expanded default threshold changed.
- No expanded hashtable, deque/node, btree, or set-member-table representation changed.
- No demotion path was added; resident-to-external and compact-to-expanded transitions are one-way.
- No field was added to `KvObj`, `Op`, or `Client`; their locked sizes remain 8, 336, and 1984.
- No snapshot format or behavior changed. Snapshot hooks consume the common collection facade and
  choose resident versus external storage again on load.
- No benchmark was run in this lane. Correctness differential and sanitizer suites were run, but
  instruction/performance measurement is intentionally left to the benchmark owner.

# SET lane design notes

## Encoding matrix

| Internal form | Admission and layout | Lookup / random pick | `OBJECT ENCODING` |
| --- | --- | --- | --- |
| Integer Compact | Every member is a strict, losslessly round-trippable signed 64-bit decimal. Values are numerically sorted and stored as fixed 2-, 4-, or 8-byte Compact payloads. | Binary search; a random index maps directly to `(width + 1) * index` because the Compact length header is one byte. | `compact` |
| Generic Compact | Arbitrary binary-safe members, bounded by `set.max_entries` and `set.max_value`. No set-specific offset vector is retained. | Bounded packed scan for membership and random-index selection. | `compact` |
| `SetMemberTable` | One-way expanded form: power-of-two, linear-probed open addressing with tombstones. Each live slot owns one string. | Expected O(1) membership. A dense vector of live slot numbers provides uniform O(1) random selection and swap-delete maintenance. | `hashtable` |

The project exposes one set threshold pair, defaulting to 128 entries / 64 bytes. Redis and Valkey
currently expose a separate `set-max-intset-entries` default of 512 and listpack defaults of 128 / 64.
`DESIGN-TYPES.md` is authoritative here, so both Compact forms use the project's 128-entry knob.
Integer members also pass the common Compact incoming-value check; the binary payload itself widens
only to 8 bytes.

`SetVal::max_member_bytes` is a conservative high-water mark. Inserts update it in O(1); deletes do
not rescan to reduce it. A stale maximum can cause an earlier promotion, but can never retain an
oversized Compact or recreate the fork's O(n)-per-write conversion-decision regression. Expanded
entry, payload, and table-allocation totals are maintained alongside mutations.

## Upgrade rules

New-set selection follows Redis/Valkey's `setTypeCreate(value, size_hint)` shape:

1. A strict integer first member and a fitting command-size hint start Integer Compact.
2. A non-integer first member starts Generic Compact when the hint and first member fit.
3. Otherwise the set starts as a pre-sized hashtable.

For an existing Compact, a multi-member SADD whose argument-count hint exceeds the entry limit
promotes before duplicate detection, matching Redis/Valkey's `setTypeMaybeConvert` behavior.

Subsequent paths are:

- Integer Compact stays sorted and widens 16 -> 32 -> 64 bits as needed. A width upgrade builds a
  complete replacement Compact before swapping it in, so allocation failure leaves the old set
  intact. Deletion does not narrow the width.
- Adding a non-integer to Integer Compact converts to Generic Compact only when resulting count,
  the maintained existing-member maximum, and the incoming length all fit. Otherwise it promotes
  directly to the hashtable. This is the Redis/Valkey intset -> listpack/hashtable rule.
- Either Compact form promotes to the hashtable before an insertion that crosses its O(1)-checked
  thresholds.
- Promotion copies every old member into a pre-sized table and flips `CollectionEncoding` last.
  There is no demotion. This matches the one-way compact -> expanded policy in the type foundation.

The conversion itself is necessarily linear in the bytes copied, but its eligibility decision is
O(1), and each representation transition occurs once. No ordinary expanded write scans the set.

## Random selection and removal

- Integer Compact chooses a uniform logical index and decodes its fixed-width slot directly.
- Generic Compact chooses a uniform logical index and linearly decodes to it. Small sets trade that
  bounded scan for eliminating the per-object side allocation.
- Hashtable selection chooses a uniform position in the dense live-slot vector. Deletion marks the
  selected table slot tombstoned and swap-deletes its dense-vector entry, all O(1) apart from the
  selected member's own allocation/free cost.

For a partial `SPOP`, a Compact set is promoted once before the first pop. That O(bytes copied)
transition is amortized over the writes that built the Compact; every selected member thereafter is
picked and removed without a collection walk. `SPOP count >= cardinality` emits the whole set and
deletes the top-level key directly, as Redis does. The no-count form returns a bulk or nil; the
count form always returns an array. Every path that removes the final member deletes the key.

`SRANDMEMBER -N` performs exactly N independent O(1) index selections with replacement, so repeated
members are allowed and the set is not modified. A positive count uses Floyd sampling to select
`min(count, cardinality)` unique logical indices in O(count) expected work and memory; a count at
least the cardinality emits the whole set. `INT64_MIN` is rejected before magnitude conversion.

## `SSCAN` contract

Compact sets are bounded, so—as Redis does for intset/listpack—one call returns all matching
members and cursor `0`, regardless of the COUNT hint. Hashtable cursors encode a table generation
and physical slot position. A call samples at most COUNT live entries and probes at most `10 *
COUNT` slots, so MATCH can legitimately return an empty batch with a nonzero cursor and COUNT is a
work hint rather than a reply-size promise.

Deletes leave other physical slots stable. A table rebuild increments the generation; a later call
holding an older cursor restarts at slot zero. Consequently a full iteration may return duplicates,
but an element present for the entire completed iteration is not omitted after any finite sequence
of rebuilds. Elements added or removed during iteration have Redis's usual unspecified visibility.
MATCH implements Redis's binary-safe glob syntax (`*`, `?`, ranges, negated ranges, escapes).

## Complexity notes

- Hashtable `SADD`, `SREM`, `SISMEMBER`, and each `SMISMEMBER` probe are expected O(member bytes),
  with ordinary open-table growth amortized. A Compact membership decision scans the bounded packed
  representation; Integer Compact uses O(log N) comparisons and Generic Compact uses O(N) packed
  comparisons, matching the semantic search required by those requested small encodings.
- Compact insertion/deletion moves only the affected packed suffix. Integer width upgrade and the
  one allowed promotion touch each existing member once. Threshold decisions never perform such a
  scan.
- `SCARD` is O(1). `SMEMBERS`, `SPOP count >= cardinality`, and a completed scan are O(N) because
  their replies semantically contain the collection. Partial `SPOP` is O(count) amortized after the
  one-time promotion. `SRANDMEMBER count` is O(abs(count)) except the whole-set positive case.
- Hashtable scan work is bounded per call even after churn leaves tombstones. The table deliberately
  does not shrink on delete, avoiding both resize thrash and relocation of scan-visible live slots.

## Tricky integer cases

Integer admission uses Redis's strict `string2ll` shape: no leading plus, no leading zeroes except
`0`, no `-0`, no whitespace, and no overflow. Thus `01` and a decimal wider than signed 64-bit are
ordinary string members and can coexist with canonical integer `1`. Inserting a value outside the
current 16- or 32-bit range rebuilds the sorted Compact at the next width; `INT64_MIN` and
`INT64_MAX` round-trip through 64-bit storage and canonical bulk-string replies without negation
overflow.

## Provenance

- **Optimized fork (`wt-round-mainline`)**: adopted its command semantics baseline, pre-sized
  promotion shape, compact random-selection strategies, and—most importantly—the maintained-counter
  rule prompted by its former hash byte-rescan regression. No fork worker/QSBR machinery was taken.
- **Redis**: adopted `setTypeCreate`, strict integer admission, sorted 16/32/64-bit intset widening,
  intset -> listpack-or-hashtable and listpack -> hashtable paths, delete-empty-key behavior,
  count/no-count reply distinctions, `SPOP count >= cardinality`, negative-count replacement
  sampling, compact-scan-all behavior, COUNT's `10 * count` work cap, and MATCH glob semantics.
- **Valkey**: confirmed the same three encoding choices and upgrade rules; its current expanded
  `hashtable` naming and random-entry API reinforced using an explicit open-addressed backing rather
  than a node-based standard container.
- **Dragonfly**: adopted the shared-nothing, shard-owner placement of the concrete set value, direct
  empty-key deletion, index-generator approach for random-member commands, and bounded cursor scan
  loop. Its dense-set/OAH encodings and per-member TTL behavior were studied but not imported.

No locks or atomics were added to set/store paths, and neither `Op` nor `Client` was changed.

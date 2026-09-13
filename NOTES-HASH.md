# HASH lane design notes

## Surface and ownership

`src/cmd/t_hash.cc` registers the single-key commands requested by this lane: `HSET`, `HMSET`,
`HSETNX`, `HGET`, `HMGET`, `HDEL`, `HLEN`, `HEXISTS`, `HSTRLEN`, `HINCRBY`, `HINCRBYFLOAT`,
`HGETALL`, `HKEYS`, `HVALS`, `HRANDFIELD`, and `HSCAN`.

One `HashVal` is owned by the key's `KvObj`. It starts as a `CompactValue`; after one-way
promotion it owns a `HashFieldMap`. Its destructor is out of line in `t_hash.cc`, so the outer
`kvobj_free()` type switch remains the only ownership dispatch and `KvObj`, `Op`, and `Client` do
not grow. No hash path contains a lock, atomic, refcount, or cross-shard access.

The command rows all name only argv 1 as their key. Missing keys have Redis's empty-hash behavior,
present non-hash keys use the foundation's exact `WRONGTYPE` reply, odd HSET/HMSET pair lists use
the shared wrong-arity spelling, and the numeric/scan option parsers reject invalid input before
mutation. `HDEL` removes the top-level key immediately after deleting the last field.

## Compact encoding

There is one outer `Compact` entry per logical hash field:

```text
[ULEB pair-payload length]
    [ULEB field length][field bytes][value bytes]
```

The outer length supplies the end of the value, so the pair needs only one inner length. Fields and
values are binary-safe, including empty strings and embedded NULs. Keeping one pair per outer entry
means `CompactValue::entries()` is the logical HLEN. `HashVal::compact_payload_bytes` separately
maintains exact `field bytes + value bytes`; `Compact::payload_bytes()` also includes the inner
field-length prefixes and is therefore not used as the logical hash payload total at promotion.

Compact lookup is a forward packed scan, bounded by `--hash-max-compact-entries`. Mutations use
only `CompactValue::append`, `replace`, and `erase`; no code reaches into the byte vector. A compact
mutation invalidates all earlier pair slices, and handlers retain none across a mutation.

## Expanded open-addressed map

`HashFieldMap` has two parts:

- A dense `std::deque<Node>` owns field/value strings. Dense indices make `HRANDFIELD` selection
  O(1), and deletion swaps the last node into the hole while repairing its one bucket index.
- Power-of-two open-addressed bucket arrays store the seeded full field hash, dense node index, and
  empty/live/tomb state. Linear probing is capped below 70% live load. The seed derives from the
  server-keyed top-level key hash, so fields do not use one public deterministic hash across keys.

Each physical bucket also carries intrusive prev/next links, and each logical home bucket carries
a head. Those links are scan metadata; lookup remains open addressing. They let HSCAN enumerate
exactly the entries whose `field_hash & mask` equals a cursor bucket without walking or repeatedly
filtering an entire linear-probe cluster.

Growth, same-size tomb cleanup, and shrink use current and old bucket arrays. A write examines at
most eight old physical buckets and moves only their index records; the field/value nodes do not
move. Starting a resize performs one bucket-array allocation/zero initialization but never walks
the collection; migration is bounded and incremental. Lookups consult current then old, insertion
uses current, and every field has exactly one live index record. Shrink starts below 15% live load;
there is no representation demotion.

Allocation, logical entry, and logical payload totals are maintained alongside each insertion,
replacement, deletion, resize step, and promotion. No mutation asks the map or Compact to recount
entries or payload bytes.

## O(1) conversion decision

For each compact write, normal field lookup establishes only whether this write inserts or
replaces. The promotion decision then reads:

```text
resulting_fields = maintained_field_count + (field_is_new ? 1 : 0)
incoming_max     = max(current_field_length, current_value_length)
promote          = resulting_fields > max_entries || incoming_max > max_value
```

That decision is O(1) after lookup and does not rescan existing fields, values, payload bytes, or
capacity. `compact_payload_bytes` is updated by the current pair's known old/new lengths.

For a wide HSET/HMSET, one preliminary pass over the command's own arguments checks the incoming
pair count and lengths. If the command itself cannot fit Compact, the hash is pre-promoted before
any per-pair insertion. This is linear only in input bytes and avoids the optimized fork's former
wide-write/listpack regression; duplicate input fields may conservatively over-promote, matching
the approach used by Redis/Valkey/fork conversion prechecks. Promotion itself is the one required
O(N) representation change: it presizes the map, copies every compact pair once, and calls
`CompactValue::promote()` only after the map owns all copies. A failed build leaves Compact active.

## HSCAN cursor and guarantee

Compact HSCAN uses a logical pair-position cursor. `COUNT` bounds positions examined in the call;
`MATCH` filters after examination and `NOVALUES` changes only the returned inner array. Cursor zero
starts and, when returned, ends the traversal. As with any positional packed cursor, modifying the
compact hash between calls may duplicate or skip positions.

Expanded HSCAN uses a 64-bit reverse-bit home-bucket cursor derived from Redis `dictScan`:

1. With one table, visit the cursor's `cursor & mask` home chain, set the unmasked bits, reverse,
   increment, and reverse again.
2. During resize, visit the smaller table's home bucket and every larger-table home bucket that is
   an expansion of it before advancing.
3. During same-size cleanup, visit that home bucket in both arrays before advancing.

Open addressing normally breaks Redis's proof because an item may be physically displaced from
its home bucket. The intrusive home chains remove that mismatch: scan order is defined by the
stable hash home bits, while open addressing is only the lookup placement. Moving an index record
between resize tables unlinks and relinks it under the corresponding mask.

Therefore, for a full expanded scan that begins with cursor 0 and runs until cursor 0 is returned,
every field that remains present for the whole scan is returned at least once, including across a
grow, shrink, or same-size incremental rehash. Duplicates are allowed, as in Redis. `COUNT` remains
an effort hint: the implementation examines at most `10 * COUNT` empty home buckets per call, but
a populated home chain is returned whole so collision-heavy buckets cannot be split unsafely.

## Work bounds and intentional collection walks

- Expanded point reads/writes are expected O(1); a write additionally migrates at most eight map
  buckets and copies only the current field/value bytes. Conversion eligibility is O(1) per pair.
- Compact point operations scan packed bytes but are capped by the configured compact-entry limit;
  they never perform a second scan to compute conversion eligibility or byte totals.
- `HGETALL`, `HKEYS`, and `HVALS` semantically return the whole collection and are O(N) plus reply
  bytes. A complete HSCAN is O(N) plus empty-bucket effort spread across calls.
- Positive `HRANDFIELD count` is O(count) expected when count is small and O(N) when count is close
  to or exceeds HLEN. Negative count is O(abs(count)) and permits duplicates. Compact random access
  first builds a bounded vector of pair slices, so it examines the compact hash once.
- `MATCH` adds the cost of Redis-style binary-safe glob matching to each examined field.

## Provenance

- **Optimized fork (`wt-round-mainline`)**: adopted the incoming-only wide-HSET preconversion idea,
  the fresh-wide-write regression lesson, maintained byte/accounting requirement, last-value-wins
  duplicate behavior, strict integer/long-double behavior, and one-way compact-to-table shape. Its
  field-expiry and template-hash extensions are outside this lane.
- **Redis**: adopted RESP2 HASH replies and command/error semantics, strict numeric parsing and
  human `HINCRBYFLOAT` formatting, the HRANDFIELD positive/negative count strategies, glob MATCH,
  the `10 * COUNT` sparse-scan effort cap, and especially `dict.c`'s reverse-mask cursor proof.
- **Valkey**: confirmed command arities, HMSET alias behavior, compact thresholds, presized
  compact-to-hashtable conversion, HRANDFIELD cases, and the same HSCAN/scan-option semantics.
- **Dragonfly**: adopted the closest C++ ownership shape (one shard owns a typed inner hash), dense
  O(1) random field selection, direct sequential replies, HSCAN `NOVALUES`, and its Redis-derived
  `10 * COUNT` scan effort bound. Fibers, transactions, tiering, and search hooks were not adopted.

## Tricky tests for the owner

1. HSET duplicate fields in one command: new/new and existing/existing; verify last value wins and
   the created-field count counts each field once. HMSET must return `+OK`.
2. Threshold edges at exactly and one above both hash limits; test an oversized field and an
   oversized value separately. `OBJECT ENCODING` must stay `hashtable` after later deletions.
3. Use limits of zero and one, and force promotion during HSETNX, HINCRBY, and HINCRBYFLOAT.
4. HDEL missing/duplicate fields and deletion of all fields in one command; verify TYPE is `none`,
   TTL metadata disappears with the key, and a wrong-type key is untouched.
5. Empty and embedded-NUL fields/values through HSET, HMGET, HSTRLEN, HGETALL, MATCH, and deletion.
6. HINCRBY: `0`, `-0`, leading zero, INT64_MIN/MAX, bad stored text, and both overflow directions.
   HINCRBYFLOAT: `inf`, `-inf`, `nan`, exponent input, negative zero, a bad stored value, and a
   finite increment whose result overflows to infinity. Existing key TTL must be preserved.
7. HRANDFIELD on missing keys with and without count; count 0, 1, HLEN, greater than HLEN, negative
   counts with duplicates, WITHVALUES flat RESP2 length, invalid option, and range boundaries.
8. HSCAN malformed/overflow/negative cursors; missing key with junk trailing options; COUNT 0 and
   invalid COUNT; repeated MATCH/COUNT; binary glob classes, escapes, and NOVALUES.
9. Complete compact positional scans with small COUNT. For expanded hashes, interleave each cursor
   call with inserts/deletes that force grow, shrink, and tomb cleanup; allow duplicates but assert
   every untouched field appears before cursor returns zero.
10. Create many fields with the same low home bits to exercise long home chains, delete head/middle/
    tail entries, then rehash and scan. Validate HLEN, point lookup, and no duplicate live index.
11. Compile with the normal `make -j` gate and confirm the existing `sizeof(Op) == 336` and
    `sizeof(Client) == 1984` assertions. Per task instruction, do not execute the server binary.

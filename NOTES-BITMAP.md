# Bitmap commands

The bitmap surface is implemented over the existing string type. There is no bitmap object type,
side allocation, or alternate ownership path: values written by `SETBIT` and `BITOP` remain normal
strings for `GET`, `GETRANGE`, `STRLEN`, `APPEND`, snapshots, expiry, eviction, and accounting.

The semantic baseline is Redis `src/bitops.c`. The implemented RESP2 commands are:

- `SETBIT key offset value`
- `GETBIT key offset`
- `BITCOUNT key [start end [BYTE|BIT]]`
- `BITPOS key bit [start [end [BYTE|BIT]]]`
- `BITOP AND|OR|XOR|NOT destination source [source ...]`

`NOT` accepts exactly one source. The newer Redis `DIFF`, `DIFF1`, `ANDOR`, and `ONE` extensions
are intentionally outside this lane.

## String representation and mutation

Bit zero is the most-significant bit of byte zero. Offsets advance toward the least-significant bit
and then into the next byte. `SETBIT` rejects negative/non-canonical offsets and offsets whose byte
would reach the 512 MiB protocol string ceiling.

Writing past the current end grows to `offset / 8 + 1` bytes and zero-fills the gap. The complete
new byte image is passed to `store_string(..., integer_encode=false)`. This retains the existing
raw embed/extern selection, replacement admission, `obj_bytes` accounting, borrowed-object
retirement, and snapshot hooks. Existing TTLs are preserved by `SETBIT`; `BITOP` replaces the
destination like Redis and therefore clears its TTL.

Integer-encoded source strings are rendered to their decimal byte representation before every bit
operation. `SETBIT` materializes such a value as raw storage even if the requested bit already has
that value, matching Redis's unshare/materialize path. A no-growth, no-change write to an already
raw string does not replace it.

`SETBIT` and `BITOP` carry `DenyOom`. The generic pre-execution growth gate now protects the
registry's `first_key` instead of hardcoded argv 1; this matters for `BITOP`, whose destination is
argv 2. Cross-shard hop-2 writes continue to use insert-level admission.

## Range semantics

`BITCOUNT` accepts only the whole-string form or a complete start/end pair, with an optional unit.
The default unit is `BYTE`. Negative indexes count from the selected unit length. Indexes before
the beginning clamp to zero, the end clamps to the final byte/bit, and an empty range counts zero.
The Redis special case where both indexes are negative and `start > end` is preserved. `BIT`
ranges mask the unused high/low edge bits before counting.

The popcount path consumes unaligned-safe eight-byte strides with `memcpy` followed by
`__builtin_popcountll`, then handles the final bytes scalarly.

`BITPOS` returns an absolute bit offset even when its range unit is bytes. A missing key is treated
as an infinite zero string (`0` when searching for zero, `-1` when searching for one). A present
empty string instead has an empty range and returns `-1`. When no explicit end is supplied, a zero
search may find the first implicit zero immediately after the physical string. An explicit end
disables that right-side zero padding, so no match returns `-1`. Partial `BIT` edge bytes are
searched only inside the requested bit interval.

## BITOP scatter-v2 shape

The registry row is `first_key=2`, `last_key=-1`, `key_step=1`: argv 2 is the destination and argv
3 onward are sources. Command-specific scatter routing interprets those positions as one
destination followed by N sources.

`BITOP` is a two-hop scatter-v2 client:

1. IO validates the operation before key access, resolves every destination/source owner, and
   installs the normal connection barrier.
2. Hop 1 excludes the destination and gathers source `ObjectImage`s through the existing
   one-arena grouping machinery. Missing sources become empty strings; a non-string source ends
   the command with `WRONGTYPE` and leaves the destination unchanged.
3. The last hop-1 completer renders integer images, finds the longest source, and folds bytes in
   source order. Shorter or missing sources contribute zero bytes. `NOT` complements its single
   source.
4. Hop 2 targets only the destination. A non-empty result calls the shared string funnel with raw
   encoding; an all-empty result deletes the destination. The reply is the longest source length.

Destination/source aliasing is safe because every source image is captured before hop 2. The
same-owner localfast path performs the same gather/compute/write sequence on one owner and uses the
same destination-only snapshot gate. As with the other scatter-v2 store commands, multi-shard
execution is barrier-ordered for the connection but is not globally atomic across clients.

## Registry and footprint

The rows are:

| Command | Arity | Flags | Key positions |
| --- | --- | --- | --- |
| `SETBIT` | 4 | write, deny-oom | 1 |
| `GETBIT` | 3 | readonly | 1 |
| `BITCOUNT` | 2..5 | readonly | 1 |
| `BITPOS` | 3..6 | readonly | 1 |
| `BITOP` | 4..N | write, deny-oom, multi-shard | 2..N |

No request, client, `KvObj`, or scatter-arena header field was added. The existing
`sizeof(Op) == 336`, `sizeof(Client) == 1984`, and `sizeof(KvObj) == 8` locks remain unchanged.

## Differential coverage

`tests/differ.py` exposes the `bitmap` suite. Its randomized stream mixes `SETBIT`, `GETBIT`,
`BITCOUNT`, `BITPOS`, all four `BITOP` operations, `SET`, `APPEND`, reads, and deletes over keys
spread across shards. Directed cases cover:

- MSB-first offsets and growth past the end;
- negative range clamping and `BYTE` versus partial-edge `BIT` ranges;
- mixed source lengths, missing sources, empty-result deletion, and `NOT`;
- destination/source aliasing;
- `SET`/`APPEND`/`GETRANGE`/`STRLEN` interoperability, including integer encoding; and
- parser and wrong-type failures that must not change the destination.

Validation on 2026-08-25 used the local Redis 8.9 oracle: three bitmap seeds produced zero diffs
across 12,786 operations. The existing string and xshard suites also produced zero diffs across
8,309 operations. No benchmark was run for this implementation pass.

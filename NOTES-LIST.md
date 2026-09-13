# LIST lane design notes

## Representation and node bounds

`ListVal` starts as one `Compact`. `Compact` keeps the foundation's byte format unchanged:
`[ULEB128 length][payload]...`. It keeps unused byte space at both ends and adds its circular
in-memory offset index lazily only after 16 entries. Endpoint writes remain amortized O(1) in
element count (plus bytes moved when the gap recenters); small indexed access rescans the bounded
blob. Occasional byte/index growth is geometric and allocator-class rounded. The side index is not
part of the stored Compact byte format, and the `KvObj`-embedded form never allocates one.

Promotion is one-way to a quicklist-style doubly linked list of `ListNode`. Every node owns a
`Compact`. Normal nodes are bounded by both:

- 8,192 encoded bytes, including ULEB128 entry lengths;
- 65,535 entries.

An element larger than 8 KiB gets an isolated one-entry Compact node. Endpoint inserts never add a
second entry to such a node. There is no compression in this lane.

The 8 KiB limit is the `list-max-listpack-size=-2` default used by the optimized fork, Redis, and
Valkey. Their quicklists also isolate values larger than the node limit and use a 16-bit node entry
count. Dragonfly's C++ `QList` retains the same `-2`/8 KiB default, doubly linked listpack nodes,
isolated-large-value policy, and 16-bit count. Those are the adopted node bounds. Dragonfly's
separate 2 KiB small-list promotion heuristic was not adopted because `DESIGN-TYPES.md` requires
the configured `--list-max-compact-*` policy (default: unlimited entries and 8 KiB aggregate
payload) for the outer Compact-to-expanded decision.

Conversion eligibility never scans the list. A write computes resulting entry and payload totals
from `CompactValue::entries()`, `payload_bytes()`, and the current arguments, then calls
`list_fits()`. Expanded writes update maintained entry, payload, and node-allocation totals. The
only O(N) promotion is the one-time copy after the O(1) decision.

Because list storage mutates behind a stable `KvObj`, handlers bracket existing-key mutations with
`kvobj_size()` samples and report the byte delta to `FlatStore`. Without that single-owner O(1)
accounting hook, a later erase or rehash would subtract the new footprint from the old cached
charge and could underflow the shard's resident estimate.

## Command and error provenance

- The optimized fork and upstream Redis `t_list.c` are the semantic baseline: multi-value push
  order, missing-key replies, negative indexes, count-form pop reply shapes, LINSERT's `0`/`-1`
  distinction, LSET errors, LREM direction/count behavior, LTRIM deletion, and LPOS option rules.
- Valkey was checked as an independent match for those command semantics and quicklist bounds.
- Dragonfly's `ListWrapper`/`QList` was used for the C++ shape: a compact representation promoted
  to a shard-owned quicklist under a wrapper, with head/tail operations staying local to the owner.
  Its transaction, blocking, tiering, and compression machinery is intentionally not present.
- The local foundation supplies RESP2 streaming replies, `WRONGTYPE`, registry/arity validation,
  `KvObj` ownership, TTL behavior, and the stable `OBJECT ENCODING` names `compact` and `deque`.

All commands in this lane have one key. Mutations preserve an existing TTL because they update the
owned `ListVal` in place. Delete-family operations remove the top-level key when the list becomes
empty.

## Costs

Here `N` is entries traversed, `B` is payload/encoding bytes copied or compared, `K` is returned or
removed entries, and `Q` is quicklist nodes traversed. Node-local packed work is capped at 8 KiB
except for an isolated large element.

| Command | Compact | Expanded (`deque`) |
| --- | --- | --- |
| LPUSH/RPUSH | Amortized O(values + incoming bytes); one-time promotion is O(N+B) | Amortized O(values + incoming bytes) at head/tail |
| LPOP/RPOP | O(K + returned bytes), O(1) amortized per endpoint removal | O(K + returned bytes), O(1) amortized per endpoint removal/node unlink |
| LLEN | O(1) | O(1) |
| LINDEX | O(1) indexed seek plus reply bytes | O(Q) node seek plus O(1) node-local indexed seek; worst O(N) nodes/entries |
| LRANGE | O(K + returned bytes) after indexed start | O(Q + K + returned bytes) |
| LSET | O(bytes shifted after the entry); promotion, if needed, O(N+B) | O(N+B): positional traversal and bounded-node rebuild |
| LINSERT | O(N + compared bytes + shifted suffix bytes) | O(N+B): pivot scan and bounded-node rebuild |
| LREM | O(N+B), using scan plus rebuild rather than repeated packed erases | O(N+B), using scan plus bounded-node rebuild |
| LTRIM | O(K+B) to copy the retained range | O(Q+K+B) to seek and rebuild the retained range |
| LPOS | O(min(N, MAXLEN) + compared bytes) | Same, crossing nodes in the selected direction |

The linear work in LINDEX/LSET/LINSERT/LREM/LTRIM/LPOS is the positional, pivot, match, or retained-
range traversal required by the command semantics and representation; it is not a conversion
eligibility scan. Rebuild-based mutations make at most a constant number of passes and therefore
remain linear in entries/bytes touched rather than becoming quadratic through repeated packed
erases.

## Tricky compatibility cases

- `LTRIM key 9 2` and any other empty result delete `key`; later `TYPE key` is `none`, not an empty
  list object. The same deletion rule applies when LPOP/RPOP or LREM removes the final element.
- `LINSERT missing BEFORE p x` returns `0`; on an existing list with no matching pivot it returns
  `-1` and inserts nothing. Like Redis, its pre-seek size check can still perform a one-way
  `compact` to `deque` promotion when the candidate element crosses the threshold. BEFORE/AFTER is
  case-insensitive; any other token is a syntax error even when the key is missing.
- `LPOS key x RANK -1` scans from the tail but returns the ordinary head-based LINDEX position.
  With `COUNT`, results stay in tail-to-head discovery order. `MAXLEN` counts entries from that
  selected end. Rank zero is rejected with Redis's explanatory error, and `INT64_MIN` is rejected
  before negation.
- Negative indexes are accepted for LINDEX, LSET, LRANGE, and LTRIM. Very negative indexes clamp
  only where Redis range semantics clamp them; point operations return nil/out-of-range.
- `LPOP/RPOP key 0` returns an empty array and leaves the key untouched. A missing key returns nil
  for the scalar form and a null array for the count form.
- At exactly the configured small-list entry/payload limits the encoding remains `compact`; the
  first write beyond either maintained bound promotes once to `deque`.

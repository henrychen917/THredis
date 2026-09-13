# ZSET lane design notes

## Representations and conversion

The compact representation stores one `Compact` entry per logical member. Its payload is the
member bytes followed by an 8-byte in-memory `double`; entries are ordered by `(score, member)`.
That keeps `CompactValue::entries()` equal to `ZCARD`, preserves scores exactly (including
infinities and signed zero), and includes the fixed score bytes in the foundation's maintained
payload count. The `zset-max-compact-value` check applies only to the incoming member length, as the
foundation contract requires.

Before each new compact insertion, the code checks the maintained logical count and only that
write's member length. It never scans the collection to decide whether to convert. Promotion copies
the already-sorted compact tuples into the expanded structure, assigns `ZsetVal::expanded`, and
calls `CompactValue::promote(CollectionEncoding::Btree, ...)` last. Promotion is one-way. The
foundation's stable `OBJECT ENCODING` name remains `btree`; the actual expanded structure in this
lane is the requested skiplist plus member index.

The expanded member index is a shard-local open-addressed table. It has no locks or atomics and
moves at most eight old slots per mutation during a resize, so a single insert/delete never pays an
O(N) member-map rehash. Its keys are non-owning views into skiplist nodes; the member bytes have one
owner and the index entry must be removed before its node is freed. Entry, payload, node-allocation,
and table-allocation totals are maintained at mutation time.

## Skiplist and spans

- Maximum level: 32.
- Promotion probability: 1/4 for each additional level.
- Sort key: score first, then binary lexicographic member order for equal scores.
- One allocation owns each node, its exact level array, and its member bytes.
- Forward links carry Redis-style spans. A span is the number of level-0 elements crossed by that
  link. Insert and unlink update the predecessor spans at every active level.
- `rank_of(member)` accumulates spans while searching `(score, member)`; `by_rank(rank)` descends
  while the next span does not overshoot. Both are O(log N) expected. `ZRANK`, `ZREVRANK`, rank
  ranges, and rank-based deletes use these operations.
- A tail pointer supports reverse traversal, and one cached tail per level makes sorted promotion
  and true tail inserts O(levels touched) rather than searching again. Range deletes find one
  predecessor path and reuse it while unlinking the contiguous run, giving O(log N + M) expected
  work for M removed members.

## Range grammars

Score bounds share one parser:

```text
score-bound := ["("] (finite-double | inf | +inf | -inf | infinity | ...)
```

No prefix means inclusive; `(` means exclusive. Parsing consumes the whole argument and rejects
NaN. An inverted interval, or equal endpoints with either endpoint exclusive, is empty.

Lex commands share one binary-safe parser:

```text
lex-bound := "-" | "+" | "(" bytes | "[" bytes
```

`-` and `+` alone are negative and positive lexical infinity. `(` is exclusive and `[` inclusive;
the finite byte string may be empty. Any other prefix, or extra bytes after an infinity marker, is
an error. Unified `ZRANGE ... REV BYSCORE|BYLEX` receives max then min, so it swaps the arguments
before calling the same parsers. `LIMIT offset count` is accepted only for score/lex ranges; a
negative offset yields an empty result and a negative count means no limit. `WITHSCORES` is rejected
with `BYLEX`.

## Semantics and tricky cases

- `NX` and `XX` conflict. `NX` also conflicts with `GT` or `LT`, and `GT` conflicts with `LT`.
  `XX GT` and `XX LT` are valid. `GT`/`LT` constrain only updates; a missing member is inserted
  unless `XX` prevents it.
- `ZADD ... INCR` accepts exactly one score/member pair. `ZINCRBY` uses the same path. If `NX`,
  `XX`, `GT`, or `LT` suppresses the write, the increment reply is nil. Adding opposite infinities
  can produce NaN; that returns `ERR resulting score is not a number (NaN)` without changing the
  member.
- Input NaN is always rejected as an invalid float, even when a condition would otherwise suppress
  the write. Positive and negative infinity are valid stored scores and valid score bounds.
- Score replies use the foundation's `reply_double`, so all score-bearing commands share the same
  Redis-oriented RESP2 formatting.
- Redis defines lex operations usefully only when all members in the range have the same score.
  Under that precondition `(score, member)` order is also member order and skiplist lex seek/count
  is O(log N). As in Redis, results are unspecified when scores differ; the implementation follows
  the reference traversal rather than adding an O(N) full-collection filter.
- `ZRANK`/`ZREVRANK WITHSCORE` return `[rank, score]`; a missing member returns a null array rather
  than a nil bulk reply.
- Every delete-family command, including `ZREM`, all three `ZREMRANGE*` forms, and both `ZPOP*`
  forms, deletes the top-level key when its last member is removed.
- Positive `ZRANDMEMBER` counts return distinct members, capped at cardinality. Negative counts
  sample with replacement. `WITHSCORES` is legal only with a count. `ZSCAN` supports `MATCH` and a
  positive `COUNT`; compact values finish at cursor zero in one call, while expanded values scan a
  bounded number of member-index slots.

## Complexity boundaries

Expanded point lookup is expected O(1), point ordering/rank mutation is expected O(log N), and
score/rank/valid-equal-score lex ranges are O(log N + M) for M returned or removed elements.
`ZRANDMEMBER` is proportional to the requested output (or cardinality when a near-full distinct
sample is requested). `ZSCAN` is proportional to examined index slots plus returned bytes.

Compact lookup and seek are linear in the compact value, as with Redis/Valkey listpack ZSETs, but
the representation is capped by the configured compact thresholds. Contiguous insertion/deletion
moves only the affected packed bytes. Commands that inherently return or remove M members do O(M)
member work; reply construction is linear in bytes emitted. `MATCH` additionally performs the
requested glob comparison against each examined member.

## Provenance

- **Optimized fork (`wt-round-mainline/src/t_zset.c`)**: adopted the embedded-member single node
  allocation, maintained allocation-size idea, per-level tail cache, sorted tail-insert fast path,
  score/member ordering, and the lesson that conversion/accounting decisions must not rescan on
  writes. Its tail construction and ownership density are better fits here than upstream's
  separate member allocation.
- **Redis (`redis/src/t_zset.c`)**: adopted the 32-level p=1/4 span algorithms, rank semantics,
  compact/expanded command behavior, shared score and lex grammars, unified Redis 6.2 `ZRANGE`
  option rules, conditional `ZADD` behavior, nil increment behavior, scan options, and range-delete
  predecessor reuse.
- **Valkey (`valkey/src/t_zset.c`)**: checked command/error compatibility and the current compact
  threshold/one-way expansion shape. Valkey's expanded ordered B-tree is not used because this lane
  explicitly requires a span skiplist.
- **Dragonfly (`dragonfly/src/core/sorted_map.*` and `server/zset_family.cc`)**: adopted the C++
  concrete-owner shape, a single member object shared by the hash and ordered indexes, and the
  family-local command surface. Dragonfly's C++ APIs and ownership are cleaner for this server,
  while its B+ tree and transaction/fiber machinery are intentionally not imported.

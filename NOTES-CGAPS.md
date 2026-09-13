# Container command gaps

## Surface and Redis semantics

- `LPOS` supports signed `RANK`, including tail-first negative ranks, `COUNT` (`0` means all), and
  `MAXLEN` (`0` means unbounded). Returned positions remain head-relative.
- `LMPOP` and `ZMPOP` implement only the non-blocking forms. `numkeys`, direction, and positive
  `COUNT` are parsed before routing. Argument order is authoritative: a missing key is skipped, an
  existing wrong type errors immediately, and the first non-empty key wins. No candidate returns a
  RESP null array. Counts clamp to the selected container's cardinality.
- `ZRANGESTORE` implements rank, `BYSCORE`, and `BYLEX` ranges, plus `REV` and score/lex `LIMIT`.
  A missing or empty source result deletes the destination. A non-empty result overwrites any
  destination type and clears its TTL, as Redis does.
- `SORT` accepts list, set, and zset inputs with numeric or `ALPHA` ordering, `ASC`/`DESC`, `LIMIT`,
  and optional `STORE`. Numeric conversion failure is exactly
  `ERR One or more scores can't be converted into double`. `STORE` writes a list, overwrites any
  destination type, clears its TTL, and deletes the destination for an empty result. `BY` and
  `GET` patterns are intentionally outside this slice and return syntax error.

## Scatter-v2 lowering

`LMPOP`/`ZMPOP` use two hops when their keys span shards. Hop one records presence, type, and
cardinality for every candidate. The IO-side decision walks those probes in original argument
order and publishes one hop-two task for the selected owner. Hop two calls the list/zset lane's
live pop helper; it does not replay a stale collection image. The helpers copy reply elements
before mutation, hold `ObjectSizeTracker` for the existing object, and call `finish()` before a
last-element whole-key erase.

`ZRANGESTORE` gathers only the source zset image. The decision computes the requested range and
publishes one destination hop whose image is loaded through `zset_snapshot_load`, which in turn
uses the normal zset add/build path and current compact thresholds. `SORT STORE` follows the same
shape, loading the destination through the list build path. Empty images erase the destination.
Both insertion paths retain FlatStore maxmemory admission.

When every explicit key maps to one shard, all four commands take the existing localfast path.
`SORT` without `STORE` always has one key and therefore remains an ordinary single-owner
operation.

## Registry and admission

- `LMPOP`/`ZMPOP`: `first_key=2`, dynamic numkeys parsing like `SINTERCARD`.
- `ZRANGESTORE`: destination argv 1, source argv 2.
- `SORT`: source argv 1; the last parsed `STORE` option supplies the optional destination route.
- Redis use-memory commands carry `DenyOom`: `ZRANGESTORE`, `SORT`, set STORE forms, `LMOVE`, and
  `RPOPLPUSH`. Pure multi-pop remains ungated.
- All new `eq_icase` option literals are lowercase. Compact mutations continue to pass logical
  offsets only. `sizeof(Op)` and `sizeof(Client)` are unchanged.

## Differential coverage

`tests/differ.py ... cgaps` combines randomized container churn with directed cases for negative
LPOS ranks and `MAXLEN 0`; LMPOP/ZMPOP order, null, and count-clamp behavior; every ZRANGESTORE
range family and empty-result deletion; numeric versus alpha SORT; zset SORT input; and SORT STORE
over a wrong-type destination while preserving a destination on numeric conversion error.

Validation performed on 2026-08-25:

- clean release build (including footprint static assertions)
- `cgaps` seeds 7, 11, and 29: zero differential replies
- existing `list`, `zset`, and `xshard` suites: zero differential replies
- ASAN/UBSAN build plus `cgaps` seed 7 (including expanded deque/skiplist pops): clean
- release boot and command smoke; clean shutdown with no stuck connections or pending bytes

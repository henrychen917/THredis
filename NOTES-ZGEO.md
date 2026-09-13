# Zset set operations and GEO lane

## What was built

This lane adds Redis 7.4-compatible sorted-set set operations and the GEO family.

Commands:

- `ZUNION`, `ZINTER`, `ZDIFF`, `ZINTERCARD`
- `ZUNIONSTORE`, `ZINTERSTORE`, `ZDIFFSTORE`
- `GEOADD`, `GEOPOS`, `GEODIST`, `GEOHASH`
- `GEOSEARCH`, `GEOSEARCHSTORE`
- `GEORADIUS`, `GEORADIUSBYMEMBER`, `GEORADIUS_RO`, `GEORADIUSBYMEMBER_RO`

The registry rows live in `src/cmd/t_zset_ops.cc` and `src/cmd/geo.cc`. The ACL category table
was regenerated from the Redis 7.4 command metadata.

## Design

- All multi-key zset operations use the existing scatter/gather engine, even when their keys
  happen to share an owner. Each EX owner serializes only its own values. The claiming EX thread
  merges copied member-sorted inputs through a priority queue, and store forms use the existing
  two-hop atomic apply path. Set inputs are decoded with score `1.0`.
- SUM, MIN, and MAX preserve Redis's infinity and signed-zero behavior. Store forms normalize a
  zero score as Redis's zset insertion does. `ZINTERCARD LIMIT 0` remains unlimited.
- GEO values are ordinary zsets containing the public 52-bit interleaved geohash encoding (26
  bits per axis). The GEO owner bridge copies entries and never lets a shard-owned pointer escape.
- Search chooses a suitable geohash resolution, range-scans the center and eight neighbor cells,
  and then applies exact haversine or box filtering. Longitude cells wrap at the antimeridian;
  latitude cells clamp at the projection bounds.
- Distance uses Redis's `6372797.560856 m` earth radius. RESP2 coordinates use fixed 17-decimal
  bulk strings, RESP3 coordinates use native doubles with the same text, and distances use fixed
  four-decimal bulk strings. GEOHASH returns the Redis-compatible 11-character form.
- GEO store forms share the same search core and use scatter/gather for the source/destination
  pair. Deprecated radius grammar, including `STORE` and `STOREDIST`, maps to that path.
- Keyspace notifications match the oracle: GEOADD emits `zadd`, GEOSEARCHSTORE emits
  `geosearchstore`, deprecated radius stores emit `georadiusstore`, and zset stores emit their
  corresponding store event.
- No `Op` or `Client` fields changed. The added state is cold scatter state allocated only for
  cross-shard work, so ordinary GET/SET dispatch has no new feature test or allocation.

## Knobs

None. The feature has no runtime knob, so `tomokv.conf` requires no addition.

## Verification

Build and metadata commands:

```text
make -j12 clean
make -j12
make asan
python3 tools/gen_acl_categories.py --redis-root /tmp/claude-1000/redis74 \
  --check src/cmd/acl_categories_generated.h
python3 -m py_compile tests/zsetops.py tests/geo.py tests/differ.py
git diff --check
```

Servers were restricted to the lane assignment:

```text
taskset -c 96-99 build/tomokv --bind 127.0.0.1 --port 7270 \
  --place ifid@96,ifid@97,ex@98,ex@99 --shards 2 --atomic 0 --appendonly no
taskset -c 96-99 build/tomokv --bind 127.0.0.1 --port 7270 \
  --place ifid@96,ifid@97,ex@98,ex@99 --shards 2 --atomic 1 --appendonly no
taskset -c 100-103 /tmp/claude-1000/redis74/src/redis-server \
  --port 7275 --bind 127.0.0.1 --save '' --appendonly no --protected-mode no
```

Directed battery commands were run against both `--atomic 0` and `--atomic 1`:

```text
python3 tests/zsetops.py 127.0.0.1 7270
python3 tests/geo.py 127.0.0.1 7270
```

Atomic-off tails:

```text
  ok   non-atomic scatter result observed across 64 independently hashed keys
ZSETOPS PASS: directed mechanisms fired
  ok   deprecated BYMEMBER STOREDIST content observed
  ok   non-atomic geo store destination observed
GEO PASS: directed mechanisms fired
```

Atomic-on tails:

```text
  ok   atomic scatter counter advanced
ZSETOPS PASS: directed mechanisms fired
  ok   deprecated BYMEMBER STOREDIST content observed
  ok   atomic geo scatter counter advanced
GEO PASS: directed mechanisms fired
```

Seeded differentials (RESP2), each with 4,200 operations:

```text
python3 tests/differ.py 127.0.0.1 7270 127.0.0.1 7275 zsetops 17
DIFFER zsetops: 4200 ops, 0 diffs -> PASS
python3 tests/differ.py 127.0.0.1 7270 127.0.0.1 7275 geo 23
DIFFER geo: 4200 ops, 0 diffs -> PASS
```

The same seeded streams also pass RESP3 (`-3`):

```text
DIFFER zsetops: 4200 ops, 0 diffs -> PASS
DIFFER geo: 4200 ops, 0 diffs -> PASS
```

ASAN/UBSAN battery (`--atomic 1`, `ASAN_OPTIONS=detect_leaks=0:abort_on_error=1`,
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`):

```text
  ok   atomic scatter counter advanced
ZSETOPS PASS: directed mechanisms fired
  ok   atomic geo scatter counter advanced
GEO PASS: directed mechanisms fired
sanitizer diagnostics: none; server shutdown exit 0
```

No indicative performance measurements were requested or taken. No requested scope was cut.

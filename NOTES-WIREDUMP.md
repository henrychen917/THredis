# Redis-wire DUMP/RESTORE

## What was built

The placeholder `TOMODMP`/XXH64 self-format was removed. `DUMP` and `RESTORE` now exchange the
Redis RDB value payload used by Redis 7.4:

```
[RDB value type][RDB value body][u16 RDB version, little endian][u64 CRC64, little endian]
```

The command entry points moved from the xshard translation unit into `src/cmd/serialize.cc` and
`src/cmd/serialize.h`. Execution remains on the key's owning executor; no store object crosses a
thread boundary. No hot struct grew, and GET/SET dispatch is unchanged.

Commands:

- `DUMP key`
- `RESTORE key ttl serialized-value [REPLACE] [ABSTTL] [IDLETIME seconds] [FREQ frequency]`

The existing TTL, `REPLACE`, `ABSTTL`, `IDLETIME`, and `FREQ` parsing and insertion behavior was
retained. Missing keys still return nil. Redis's exact checksum/version error, bad-format error,
and BUSYKEY reply are preserved.

## Codec design

- Jones CRC64 uses polynomial `0x95ac9329ac4bc9b5`, a constexpr 256-entry table, and the public
  `crc64("123456789") == 0xe9c6d914c4b8d9ca` vector.
- Length decoding covers the 6-bit, 14-bit, 32-bit, and 64-bit forms.
- String decoding covers raw strings, int8/int16/int32 immediates, and LZF. The LZF decoder handles
  literal runs and overlapping back-references; emission deliberately uses uncompressed strings.
- Redis 7.4 oracle probing established the current runtime encodings decoded here:
  - string type 0;
  - quicklist2 list type 18, including packed and plain nodes;
  - hash types 4 and 16 (hashtable and listpack);
  - set types 2, 11, and 20 (hashtable, intset, and listpack);
  - zset types 5 and 17 (binary-double expanded form and listpack).
- Listpack decoding covers 7/13/16/24/32/64-bit integers, 6/12/32-bit strings, entry back-length
  validation, binary values, infinities, and subnormal zset scores.
- `DUMP` emits one simple valid encoding per logical type: string 0, list 1, hash 4, set 2, and
  zset2 type 5. Redis 7.4 accepted every emitted cell via `RESTORE`.
- The codec converts through the existing logical snapshot hooks. Restore therefore chooses
  TomoKV's current compact/expanded representation thresholds instead of persisting an internal
  representation.

This feature adds no runtime knobs and allocates no state while idle, so `tomokv.conf` needs no new
entry. Work occurs only when `DUMP` or `RESTORE` executes.

## Scope

Streams and module payloads are out of scope. `RESTORE` returns `ERR Bad data format` for an
unsupported RDB value type. `DUMP` on a stream returns `ERR object could not be serialized`; it no
longer emits a TomoKV-only value. This cut is directed-tested.

The optional MIGRATE stretch was not built. It would add a separate coordinating-thread outbound
client lifecycle and failure surface; keeping this lane focused leaves the required cross-server
codec fully tested and avoids shipping a partially exercised network command.

## Test evidence

Exact build commands:

```
make -j12 clean && make -j12
make asan
```

Both completed without diagnostics.

Directed cross-restore battery, release atomic off:

```
python3 tests/wiredump.py 127.0.0.1 7290 --oracle-port 7295 --atomic 0 --boot
  ok   TTL/REPLACE/ABSTTL/IDLETIME/FREQ mechanisms fired
  ok   directed corruptions matched exact oracle errors
  ok   1000/1000 random corruptions rejected; target survived
WIREDUMP PASS atomic=0 cells=50 corruptions=1000
```

Directed cross-restore battery, release atomic on:

```
python3 tests/wiredump.py 127.0.0.1 7290 --oracle-port 7295 --atomic 1 --boot
  ok   TTL/REPLACE/ABSTTL/IDLETIME/FREQ mechanisms fired
  ok   directed corruptions matched exact oracle errors
  ok   1000/1000 random corruptions rejected; target survived
WIREDUMP PASS atomic=1 cells=50 corruptions=1000
```

ASAN/UBSAN battery:

```
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/wiredump.py 127.0.0.1 7290 --oracle-port 7295 --atomic 0 \
  --boot --target-bin build/tomokv-asan
  ok   directed corruptions matched exact oracle errors
  ok   1000/1000 random corruptions rejected; target survived
WIREDUMP PASS atomic=0 cells=50 corruptions=1000
```

The 50 cells exercise both cross directions and the TomoKV round trip for strings, lists, hashes,
sets, and zsets across raw, integer, LZF, listpack, intset, quicklist2 plain/packed/multi-node,
hashtable, and expanded zset encodings.

Random differential:

```
python3 tests/differ.py 127.0.0.1 7290 127.0.0.1 7295 wiredump 74
DIFFER wiredump: 4200 ops, 0 diffs -> PASS
```

The differ cross-restores DUMP results because RDB permits multiple byte-distinct encodings. It
mixes DUMP, RESTORE with relative/absolute TTLs, EXISTS, PTTL, and full logical reads.

Existing live and native snapshot recovery coverage, updated from self-format assumptions:

```
python3 tests/dumprestore.py 127.0.0.1 7290 live
dumprestore: PASS (292 checks, mode=live; snapshot hooks/envelope/TTL fired)

python3 tests/dumprestore.py 127.0.0.1 7290 prepare_restart
dumprestore: PASS (261 checks, mode=prepare_restart; snapshot hooks/envelope/TTL fired)

python3 tests/dumprestore.py 127.0.0.1 7290 verify_restart
dumprestore: PASS (61 checks, mode=verify_restart; snapshot hooks/envelope/TTL fired)
```

No wire/NIC benchmark or `tests/gate.sh` was run, as required by the lane assignment.

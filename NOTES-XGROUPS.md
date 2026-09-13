# Streams phase 2: consumer groups

## Shipped surface

This lane adds the remaining single-stream consumer-group surface:

- `XGROUP CREATE` (`MKSTREAM`, `ENTRIESREAD`), `CREATECONSUMER`, `DELCONSUMER`, `DESTROY`, and `SETID`.
- `XREADGROUP GROUP group consumer [COUNT n] [BLOCK ms] [NOACK] STREAMS key id`, including `>` delivery, explicit-ID PEL history replay, and blocking wakeup.
- `XACK` and both `XPENDING` forms, including `IDLE`, exclusive/ranged bounds, count, and consumer filters.
- `XCLAIM` with `IDLE`, `TIME`, `RETRYCOUNT`, `FORCE`, `JUSTID`, and `LASTID`.
- `XAUTOCLAIM`, including `JUSTID`, the scan cursor, and Redis 7.x deleted-entry cleanup in reply element three.
- `XSETID` with `ENTRIESADDED` and `MAXDELETEDID`.
- `XINFO STREAM`, `XINFO STREAM FULL [COUNT n]`, `XINFO GROUPS`, and `XINFO CONSUMERS`.

Phase-1 `XDEL`, `XTRIM MAXLEN|MINID` (exact and approximate syntax with `LIMIT`), `XREVRANGE`, and blocking `XREAD` were already registered. They remain in `src/cmd/t_stream.cc` and are exercised here where their tombstones and trims interact with the PEL.

`XINFO STREAM FULL` was completed; there is no feasibility cut. The lane contract names one `STREAMS key id` pair for `XREADGROUP`, so its route is deliberately single-key/owner-local. Multi-key `XREADGROUP` would require a separate cross-shard consistency design and is outside this lane's explicit single-shard premise.

## Design

- Consumer-group state hangs from the existing cold `StreamVal::groups` pointer. A stream that never uses `XGROUP` allocates nothing for this feature. `Op` and `Client` were not changed, and their footprint assertions remain intact.
- Each group owns ordered maps for consumers and its PEL. A PEL record contains owner consumer, last delivery time, and delivery count. All access occurs on the stream key's owning executor thread.
- The existing blocking tri-state machinery now recognizes `XREADGROUP`. Registration creates/touches the consumer before parking, matching the Redis oracle; `XADD` owner publication wakes the waiter, and the owner performs the PEL mutation before exposing completion.
- History replay retains deleted PEL IDs and emits nil fields. `XAUTOCLAIM` removes such tombstones and reports their IDs in the third reply element.
- Oracle correction: direct Redis 7.4.2 probes return integer zero for `XACK` against either a missing key or a missing group. `XREADGROUP` returns `NOGROUP`. The implementation follows the designated binary oracle and the differential suite covers the missing-group `XACK` case.
- Stream snapshot payload version 2 appends the cold group/consumer/PEL state. Version-1 stream images remain readable. The directed battery validates group-bearing `DUMP`/`RESTORE` as a recovery-image check.
- Group allocation is included in `MEMORY USAGE` accounting. Command ACL metadata is generated from the expanded stream command table.

## Knobs

Consumer groups add no knobs. The existing Redis-compatible knobs were verified in the shared CLI/conf parser and are now documented in `tomokv.conf`:

- `stream-node-max-bytes` (default `4096`)
- `stream-node-max-entries` (default `100`)

Zero disables the corresponding rollover axis. With both axes disabled, no macro-node rollover budget is armed.

## Verification evidence

The Redis 7.4.2 behavioral oracle was run on cores 36-39, port 7235. TomoKV used cores 32-35, port 7230. No reserved gate port or cores were used.

Clean release build:

```text
$ make -j12 clean && make -j12
g++ ... src/cmd/t_stream_groups.cc ... -o build/tomokv ...
[exit 0; no compiler warnings]
```

Directed group battery, final tree, `--atomic 0` and `--atomic 1`:

```text
  ok   XGROUP CREATE ENTRIESREAD seeds entries-read and lag
  ok   XGROUP SETID updates cursor and ENTRIESREAD
  ok   XGROUP DELCONSUMER drops that consumer's exact pending count
  ok   XACK missing-group oracle control returns zero
  ok   XPENDING exclusive end bound excludes the matching ID
  ok   blocking XREADGROUP registers its consumer before parking
  ok   blocking XREADGROUP waiter arms and wakes on XADD
  ok   XINFO STREAM FULL exposes entries, group PEL and consumers
  ok   stream recovery image preserves group cursor and PEL
  ok   stream-group waiter gauges finish at zero
STREAMGROUPS PASS
```

Phase-1 regression battery:

```text
$ python3 tests/stream.py 127.0.0.1 7230
  ok   stream waiter gauges end at zero
STREAM PASS
```

Differential suite, explicit `XADD` IDs, both protocol encodings (the normal run was also repeated under `--atomic 1`):

```text
$ python3 tests/differ.py 127.0.0.1 7230 127.0.0.1 7235 streamgrp
DIFFER streamgrp: 4052 ops, 0 diffs -> PASS
$ python3 tests/differ.py 127.0.0.1 7230 127.0.0.1 7235 streamgrp -3
DIFFER streamgrp: 4052 ops, 0 diffs -> PASS
```

ASAN/UBSAN build and directed battery:

```text
$ make asan
[exit 0; no compiler warnings]
$ python3 tests/streamgroups.py 127.0.0.1 7230
STREAMGROUPS PASS
[clean server shutdown; no ASAN, UBSAN, or leak diagnostics]
```

Generated/static checks:

```text
$ python3 tools/gen_acl_categories.py --check src/cmd/acl_categories_generated.h
$ python3 -m py_compile tests/streamgroups.py tests/differ.py
$ git diff --check
[all exit 0]
```

No indicative performance numbers were collected for this correctness lane.

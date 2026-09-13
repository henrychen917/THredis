# Streams phase 1

This lane implements the mission-scoped phase-1 surface: `XADD`, `XLEN`, `XRANGE`,
`XREVRANGE`, `XDEL`, exact `XTRIM MAXLEN|MINID` (with `~` accepted as exact), and immediate and
blocking `XREAD`. Consumer groups, `XSETID`, and `XINFO` remain outside this mission's phase-1
scope.

The representation is a 56-byte header plus delta-encoded records in an embedded `Compact`, then
a deque of `Compact` macro nodes indexed by a sorted `{base_id, node}` vector. Partial external
head trims advance `Compact`'s front gap and retain the predecessor field dictionary once per
stream, so repeated exact trim does not rebuild or grow the active node. The one-entry resident
probe measured 128 bytes/key; a one-million-operation `XADD ... MAXLEN = 100` soak measured 2,191
dataset bytes at both the 500k and 1m marks and zero after `DEL`.

## Matched stream throughput

Measurements used the final binary and vanilla Redis 7.4.2, port 7955, server CPUs 224-231 for
tomokv and 232-239 for Redis, the opposite slice for `redis-benchmark`, 32 clients, pipeline 32,
and three runs. `XADD` used one million requests; both 100-entry read cells used 50,000 requests.

| cell (ops/s) | PRE: Redis runs | PRE median | POST: tomokv runs | POST median | POST/PRE |
| --- | --- | ---: | --- | ---: | ---: |
| `XADD mystream * field value` | 805,153 / 803,213 / 803,859 | 803,859 | 1,472,754 / 1,488,095 / 1,494,768 | 1,488,095 | 1.851x |
| `XRANGE mystream - + COUNT 100` | 33,362 / 33,365 / 32,857 | 33,362 | 34,852 / 35,624 / 34,952 | 34,952 | 1.048x |
| `XREAD COUNT 100 STREAMS mystream 0-0` | 33,185 / 32,560 / 32,432 | 32,560 | 34,305 / 34,490 / 34,324 | 34,324 | 1.054x |

The existing-hot-path control used the base commit `420b4d492` and final binary with persistence
off, 4 IO + 4 executor threads on CPUs 224-231, 64 populated-key clients on CPUs 232-239, pipeline
32, a one-million-key random rotation, and 100 million mixed GET/SET commands per run. User-mode
server instructions were counted across the process.

| cell | run 1 | run 2 | run 3 | median |
| --- | ---: | ---: | ---: | ---: |
| base GET+SET p32 instructions/op | 1,856.454 | 1,861.302 | 1,808.312 | 1,856.454 |
| final GET+SET p32 instructions/op | 1,817.119 | 1,816.069 | 1,914.634 | 1,817.119 |

The median delta is -39.335 instructions/op, satisfying the `<= +1` no-regression bar.

## Validation

- `tests/differ.py ... stream`: 4,031 operations, 0 differences under atomic 0 and atomic 1.
- `tests/differ.py ... notify`: 331 operations / 454 events, 0 differences under both atomic modes.
- `tests/stream.py`: 30 directed properties, including blocking/disconnect/reset, indexed seeks,
  the 128-byte one-entry floor, and the sustained-trim plateau; the million-op soak passed.
- `GATE_PORT=7955 GATE_CORES=224-231 tests/gate.sh quick`: 65 checks, 0 failures.
- Compile-time footprint locks remain `sizeof(Op) == 336` and `sizeof(Client) == 1984`.

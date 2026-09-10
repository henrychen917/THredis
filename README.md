# TomoKV

TomoKV is a C++20 in-memory key-value server for Linux. It speaks the Redis
RESP2/RESP3 protocol and implements strings, hashes, lists, sets, sorted sets,
streams, bitmaps, HyperLogLog, and geospatial commands. It is an independent
implementation intended for research into single-node database execution,
thread placement, and memory reclamation.

Each shard has one executor owner. The server can separate networking from
execution (`2s`) or give each thread both responsibilities (`1s`). An optional
local-read lane in `1s` reads immutable string values on the connection's thread;
unsafe reads use the shard owner. Cross-shard commands use scatter/gather, with
epoch-MVCC for grouped execution. Shards and connections can move between threads
while the server runs.

The scope is deliberately **single node, single keyspace**. Cluster management,
replication, Sentinel, modules, cross-server key migration, and multiple logical
databases are absent. Some related command names exist only to return a
standalone response or an explicit unsupported error.

## Build

Build from the repository root with GNU Make and a GCC compiler supporting C++20.
The Makefile uses a GCC-specific optimization parameter, so substituting Clang is
not a supported copy-and-paste build instruction.

Required development headers and libraries are:

| Dependency | What the build uses |
| --- | --- |
| liburing | `liburing.h`, `-luring`, including the newer ring setup and message-ring APIs |
| OpenSSL | SSL/crypto headers, `-lssl -lcrypto`, including TLS 1.3 and kTLS interfaces |
| POSIX threads | `-pthread` |
| jemalloc, by default | `jemalloc/jemalloc.h`, `-ljemalloc` |

Lua 5.1 is bundled in [third_party/lua](third_party/lua) and compiled through the
scripting translation unit; no separate Lua installation is used.

```sh
make -j2
```

The output is `build/tomokv`. With the system jemalloc header installed at
`/usr/include/jemalloc/jemalloc.h`, Make links the system library. Otherwise it
expects an installation under `JEDIR`; its default is a maintainer-local path.
To build without jemalloc, use:

```sh
make clean
make -j2 JE=0
```

Changing allocator or compiler flags requires a clean rebuild: Make does not
track command-line variable changes as dependencies. The default flags include
`-march=native`; build on the target machine or choose appropriate compiler flags
before distributing the binary. There is no `make install` target.

## Run

This example provisions two IO threads and two executor threads. It requires at
least four CPUs in the process affinity mask; when complete SMT sibling pairs
are available, placement keeps each pair together. It fixes the shard count and
enables atomic multi-key execution explicitly:

```sh
./build/tomokv --ratio 2:2 --shards 16 --atomic 1
```

The default listener is `127.0.0.1:6379`. In another terminal, using an installed
Redis CLI:

```text
$ redis-cli
127.0.0.1:6379> PING
PONG
127.0.0.1:6379> SET greeting hello
OK
127.0.0.1:6379> GET greeting
"hello"
```

For fused threads with the local-read lane, stop the first server and use:

```sh
./build/tomokv --thread-mode 1s --read-local 1 --shards 16 --atomic 1
```

Fused mode uses every CPU in the process affinity mask unless `--place` selects
a subset. The implementation supports at most 128 provisioned threads; on larger
CPU sets, restrict process affinity or use explicit placement. Split mode needs
both IO and executor roles; fused mode can use one CPU. See
[placement rules](docs/CONFIGURATION.md#threads-placement-and-execution).

The default network backend is io_uring. If the environment disallows it, select
the implemented epoll backend explicitly:

```sh
./build/tomokv --ratio 2:2 --shards 16 --atomic 1 --net-io epoll
```

Epoll also selects syscall-based persistence. It still needs liburing and
OpenSSL at build/link time. Startup checks that allocated object pointers fit
the store's low-48-bit pointer encoding. The repository does not specify a
verified minimum kernel or dependency release.

## Configuration and persistence

Options use separate arguments: `--port 7000`, not `--port=7000`. A configuration
file uses the same names without `--`. The supplied [tomokv.conf](tomokv.conf)
has commented examples. Either invocation reads it, then applies CLI overrides:

```sh
./build/tomokv tomokv.conf --ratio 2:2 --shards 16 --atomic 1
./build/tomokv --conf tomokv.conf --ratio 2:2 --shards 16 --atomic 1 --port 7000
```

Important defaults, before any overrides:

| Setting | Default |
| --- | --- |
| `thread-mode` | `2s`; even IO/executor split over allowed CPUs |
| `shards` | `-1`: `min(8 × initial executor count, 256)` |
| `read-local`, `atomic`, `flip-auto` | `0` |
| `lb` | `1`: key and client balancing together |
| `net-io` | `uring` |
| `bind`, `port` | `127.0.0.1`, `6379` |
| `tls-port`, `unixsocket` | `0` (TLS off), unset |
| `requirepass`, `protected-mode` | unset, `1` |
| `maxclients`, `maxmemory` | `10000`, `0` (no eviction limit) |
| `zc-min` | `16384` value bytes |
| `save` | `3600 1 300 100 60 10000` |
| `dir`, `dbfilename` | `.`, `dump.tomo` |
| `appendonly`, `appendfsync` | `no`, `everysec` |
| `script-instruction-limit` | `100000` Lua VM instructions |

[CONFIGURATION.md](docs/CONFIGURATION.md) lists every boot option, accepted
value, default, and runtime mutability, including the study options omitted
from `--help`. `CONFIG GET *` reports the runtime configuration table, which is
a subset of the boot options. `CONFIG REWRITE` has preservation and quoting
limitations documented there.

Periodic snapshots are enabled by default. Use `--save ""` to disable the
schedule; explicit `SAVE` and `BGSAVE` remain available. **An existing
`dump.tomo` is not loaded automatically.** After a successful save and after
stopping the old process, recover its 16-shard snapshot with:

```sh
./build/tomokv --ratio 2:2 --shards 16 --atomic 1 --load ./dump.tomo
```

`--appendonly yes` enables the append-only log and its boot recovery. Keep the
same `dir`, `appenddirname`, `appendfilename`, and shard count across restarts.
An existing AOF recovery plan takes precedence over `--load`. Snapshot and AOF
files are TomoKV formats, not Redis RDB or Redis AOF files.

## Compatibility

The checked-out source registers **246 top-level command names**, including
aliases, standalone error handlers, and TomoKV's `FLIP`. The supplied project
context's count of 243 differs from the current tables; the static count is
recorded in [FINDINGS](docs/FINDINGS.md#command-inventory). A registered name
does not imply every Redis subcommand or option is implemented. `COMMAND`,
`COMMAND INFO`, and `COMMAND DOCS` expose the registry and metadata.

Supported facilities include RESP3 negotiation with `HELLO 3`, key and hash-field
expiration, blocking list/sorted-set/stream operations, `MULTI`/`EXEC` and
`WATCH`, Lua scripts and functions, ACLs, TLS, pub/sub, keyspace notifications,
client tracking, snapshots, and append-only persistence.

The main boundaries are:

- Only database zero exists: `SELECT 0` works, other indexes fail, and
  `databases` accepts only `1`. `CLUSTER`, `MIGRATE`, `MODULE`, `MOVE`, `PSYNC`,
  `REPLCONF`, `SENTINEL`, `SWAPDB`, and `SYNC` have no registry entries.
- `REPLICAOF`/`SLAVEOF` explicitly reject replication. `ASKING`, `READONLY`, and
  `READWRITE` return the cluster-disabled error. `WAIT` cannot obtain replica
  acknowledgements; `WAITAOF` with a positive local-fsync count is unimplemented.
- With the default `atomic 0`, ordinary multi-shard mutations can expose
  intermediate states. `atomic 1` enables the grouped MVCC path. `EXEC` and
  staged cross-shard scripts use their own group admission even at `atomic 0`;
  disabling the knob does not disable transactions.
- Scripts have a VM-instruction budget, not Redis's wall-clock busy threshold.
  Cross-shard scripts stage declared keys in a bounded private workbench and can
  fail on resource limits or repeated conflicts.
- `DUMP`/`RESTORE` implement a Redis RDB **value** codec for supported encodings.
  They do not make whole Redis persistence files loadable.
- Eviction accounting, compact encodings, diagnostic fields, and some
  configuration semantics differ from Redis. `maxmemory` divides a data budget
  among shards; it is not a process RSS cap.

The local-read property is specific: ordinary nonstructural string replacements
publish only their own slot to readers and retain old values until a QSBR grace
period. A captured local GET validates table topology and declines to the owner
on structural interference. The current local MGET implementation can retry its
whole validation window once. See the [read-path explanation](docs/ARCHITECTURE.md#local-reads)
and the recorded conflict with the project's no-reader-retry law in
[FINDINGS](docs/FINDINGS.md#reader-contract).

## Source navigation

Start with [ARCHITECTURE.md](docs/ARCHITECTURE.md) for the mechanisms and their
interactions, or [src/README.md](src/README.md) for the directory map. Each major
source directory has a short directory header. [tests](tests) holds correctness
batteries; [tools](tools) holds research and measurement utilities. Documentation
review discrepancies and verification limits are collected in
[FINDINGS](docs/FINDINGS.md).

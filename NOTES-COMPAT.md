# Client compatibility, RESP3, INFO, and Unix sockets

Connections start in RESP2 and may negotiate RESP3 with `HELLO 3`. The protocol is connection-local
and is captured in each operation before dispatch, without moving sockets into executor threads or
adding shared writes to the store path. The command handlers and registry rows are in
`src/cmd/t_server.cc`; reply-only RESP3 builders are in `src/net/resp3.h`.

## Connection behavior studied

The Redis/Valkey behavior was checked in the read-only references, principally Redis
`src/networking.c` (`helloCommand`), `src/server.c` (`COMMAND` and `COMMAND DOCS`), `redis-cli.c`,
and `redis-benchmark.c`, with their Valkey counterparts as a second implementation. Current client
initialization was also checked in the official redis-py, go-redis, and Jedis repositories.

| Consumer | Connection/discovery calls relevant here | tomokv behavior |
| --- | --- | --- |
| redis-py | Optional `AUTH`, `CLIENT SETNAME`, `SELECT`, then best-effort `CLIENT SETINFO LIB-NAME` and `LIB-VER`; non-RESP2 configurations negotiate with `HELLO`. | RESP2 and RESP3 negotiation are supported. `AUTH` reports the Redis no-password-configured error, db 0 is accepted, and both `SETINFO` forms are accepted. |
| go-redis | Negotiates with `HELLO <protocol>`, followed as configured by `AUTH`, `SELECT`, `CLIENT SETNAME`, and pipelined `CLIENT SETINFO`. | Protocol 2 and 3 are supported; only db 0 is supported. |
| Jedis | Legacy/default-RESP2 configurations connect directly; configurations with protocol negotiation use `HELLO`. | Both protocol modes and the connection-local initialization calls are supported. |
| redis-cli | Defaults to RESP2; current interactive help obtains `COMMAND DOCS`; explicit `-3` sends `HELLO 3`. Cluster discovery also probes `CONFIG GET databases`. | Normal and `-3` sessions receive their protocol-native aggregate shapes. |
| redis-benchmark | Before a run it probes `CONFIG GET save` and `CONFIG GET appendonly`; `-3` sends `HELLO 3`. | Both config keys and both protocol modes are supported. |
| YCSB Redis binding | Uses ordinary RESP2 data commands and may configure authentication/database selection, depending on the binding version. | Run without a password and with database 0. Its `SET`/`GET` workload needs no compatibility extension beyond this lane. |

`HELLO 2` returns the Redis RESP2 representation of the seven-field map, a 14-element flat array;
`HELLO 3` returns a `%7` map. Both contain `server`, `version`, `proto`, `id`, `mode`, `role`, and
`modules`. The server identity is `redis`, mode is `standalone`, role is `master`, and modules is an
empty array. A bare `HELLO` is introspection and preserves the current protocol. `RESET` clears
subscriptions and connection metadata and returns the connection to RESP2.

## RESP3 reply contract

Request parsing is unchanged: RESP3 connections continue to send the same RESP2 multibulk request
grammar. Only reply framing varies, selected from the protocol bit captured in each `Op`.

| Command or reply | RESP2 | RESP3 |
| --- | --- | --- |
| Null scalar / null aggregate | `$-1` / `*-1` | unified `_` null |
| `CONFIG GET`, `HGETALL`, `COMMAND DOCS` | flat key/value array | map |
| `HRANDFIELD WITHVALUES`, `ZRANDMEMBER WITHSCORES` | flat array | array of pairs |
| `ZSCORE`, `ZMSCORE`, `ZINCRBY`, `ZADD INCR` | bulk score | native double |
| `ZRANK`/`ZREVRANK WITHSCORE` | rank plus bulk score | rank plus native double |
| scored `ZRANGE` family | flat member/score array | array of member/double pairs |
| `ZPOPMIN`/`ZPOPMAX` with count | flat member/score array | array of member/double pairs |
| `ZPOPMIN`/`ZPOPMAX` without count | flat two-element array | flat two-element array, with native double score |
| `SMEMBERS`, `SINTER`, `SUNION`, `SDIFF`, `SPOP key count` | array | set |
| `INFO`, `CLIENT INFO`, `CLIENT LIST` | bulk | `txt` verbatim string |
| aborted `EXEC` | null array | null |
| `XREAD` result | array of key/entries pairs | map from key to entries |
| pub/sub acknowledgements and `message`/`pmessage`/`smessage` delivery, including keyspace notifications | array | push |
| Lua RESP3 special values | RESP2 conversion | bool, map, set, double, big number, or verbatim string as requested by Lua/`redis.setresp` |

Double payloads use shortest round-trip formatting, with `%.17g` only as a library fallback, and
preserve Redis spellings such as `-0`, `inf`, `-inf`, and `nan`. `INCRBYFLOAT` and `ZSCAN` score
payloads remain bulk strings. Membership/existence results such as `SISMEMBER` and `EXISTS` remain
integers; they are not RESP3 booleans. `XPENDING`, `GEOPOS`, and `GEODIST` are not registered in
this tree, so their audited Redis shapes are not claimed here. In Redis, `XPENDING`,
`INCRBYFLOAT`, and `GEODIST`/`WITHDIST` do not change shape merely because RESP3 is active.

RESP3 subscribers may run ordinary commands while subscribed; RESP2 retains Redis's restricted
subscriber command set. Core commands do not emit attribute frames. `CLIENT TRACKING` is not built,
so tracking invalidation push frames are intentionally out of scope; pub/sub and notification push
frames do not depend on tracking.

### RESP3 validation

- The declaration-order mirror probe reports `sizeof(Client) == 1984` and `sizeof(Op) == 336`;
  the connection flag remains at byte 55 and `Op::route_flags_` bit 2 carries RESP3.
- All 12 differential suites pass against the Redis oracle in both protocols: `string`, `list`,
  `set`, `zset`, `hash`, `xshard`, `bitmap`, `hll`, `cgaps`, `stream`, `spubsub`, and `notify`.
  The RESP3-directed battery exercises 140 protocol/type/pubsub/notification/MULTI/Lua assertions.
- An object-code comparison against base `0f920a52d` finds `reply_bulk<Op::Sink>` (477 bytes,
  SHA-256 `bd5889cd...c6a4b2ac`) and `reply_int<Op::Sink>` (594 bytes, SHA-256
  `d424a129...86b84ff`) byte-identical.
- A matched 20-million-command loopback GET+SET cell used 64 clients, pipeline 32, server CPUs
  248-251 in a 2:2 topology, and load CPUs 252-255. Base instructions/op were
  1751.376 / 1753.627 / 1757.700 (median 1753.627); RESP3 code with a default RESP2 connection was
  1750.201 / 1750.888 / 1741.019 (median 1750.201), a median delta of -3.427 instructions/op.
- `GATE_PORT=7953 GATE_CORES=248-255 tests/gate.sh quick` passes all 80 checks, including the RESP3
  battery under both atomic settings.

`COMMAND` metadata is generated from the boot-built registry, including actual arity, flags, and
legacy key positions. `COMMAND DOCS` is deliberately minimal but is a well-formed map (a flat
map-as-array in RESP2) with `summary`, `since`, `group`, and `complexity`, which is sufficient for
current `redis-cli` live-help parsing. `COMMAND INFO` preserves one null entry per unknown name.

Client names and library metadata live in an IO-thread-local catalog keyed by the process-unique
connection id. No process-global `Client*` catalog exists. `CLIENT LIST` and `CLIENT KILL` scatter
ID-only requests to every IO; each IO formats or closes only clients it owns, and the origin gathers
the replies while retaining the invoking ROB slot. No IO thread pauses and no `Client*` crosses an
IO boundary.

`CLIENT INFO` and every `CLIENT LIST` line use the vanilla Redis 7.4 field set and order exactly:

```text
id addr laddr fd name age idle flags db sub psub ssub multi watch qbuf qbuf-free argv-mem
multi-mem rbs rbp obl oll omem tot-mem events cmd user redir resp lib-name lib-ver
```

Fork-only fields such as `io-thread` and `avg-pipeline-len` are intentionally absent. `LIST TYPE`
and `LIST ID` are supported. `KILL` supports the old address form and the new `ID`, `ADDR`, `LADDR`,
`TYPE`, `USER`, `SKIPME`, and `MAXAGE` filters; self-kill queues its reply before closing.
`SETNAME`, `GETNAME`, `SETINFO`, `ID`, and `INFO` use the same owner-local metadata. `NO-EVICT` is
parsed and reported but deliberately inert because TomoKV does not evict client connections.
`RESET` restores db 0 and clears name/library metadata.

## CONFIG behavior

The typed table contains `save`, `appendonly`, `maxmemory`, `maxmemory-policy`, `timeout`,
`databases`, `proto-max-bulk-len`, `zc-min`, and all eight
`*-max-compact-{entries,value}` settings.
`CONFIG GET` matches Redis-style case-insensitive globs and returns flat name/value pairs in RESP2
or a map in RESP3.
`CONFIG SET` accepts one or more pairs, normalizes booleans and byte suffixes, rejects unknown
settings, overflow, invalid policies, and malformed pair counts.

Changing `zc-min` or a compact threshold is scattered to every shard owner and updates its local
runtime setting. The remaining entries are compatibility/control-plane values: changing them does
not enable persistence, eviction, idle disconnects, a different request bulk limit, or additional
databases. The implementation remains a single-db, no-persistence server.

## Cross-shard SCAN cursor

The unsigned decimal cursor is encoded as:

```text
bits 63..56  shard id (0..255)
bits 55..0   owner-local FlatStore cursor
              bit 32     current rehash table (0 or 1)
              bits 31..0 next physical slot
```

Equivalently, `cursor = (shard_id << 56) | inner_cursor`. Cursor 0 begins shard 0. Finishing one
shard returns the next shard id with an inner cursor of zero; finishing the final shard returns 0.
Server boot already limits the shard count to 256.

`COUNT` is a work hint measured in physical hash slots examined, so sparse stores cannot turn one
pass into an unbounded table scan. `MATCH` and `TYPE` filter returned entries without expanding that
slot budget. For a stable keyspace a complete cursor cycle visits all live slots. As with Redis
`SCAN`/`HSCAN`, mutation during iteration may cause duplicates or omissions, and callers must not
treat `COUNT` as a requested result count. Expired entries encountered by the owner are removed and
not returned.

`RANDOMKEY` chooses a pseudo-random starting shard in IO, prefers a shard whose published count is
nonzero, then executes on that shard's owner and selects from a pseudo-random physical starting
slot. It never makes an IO thread inspect a `FlatStore`.

## INFO provenance

All request-path counters have one writer. Registry command ids index a plain counter array owned
by each IO or executor thread; one public scatter command is counted once by its IO thread. INFO is
the exceptional operation that sums those arrays.

| Field | Source |
| --- | --- |
| `used_memory`, `used_memory_dataset` | Sum of each shard's published logical `KvObj` bytes. |
| `allocator_allocated`, `allocator_resident`, `used_memory_rss` | jemalloc `mallctl` epoch/stat values when built with jemalloc; zero with the libc allocator. |
| `total_commands_processed`, `cmdstat_*:calls` | Sum of the per-thread registry-id arrays. |
| `keyspace_hits`, `keyspace_misses`, `expired_keys` | Sum of shard-owner counters. |
| `evicted_keys` | Zero placeholder; eviction is not implemented. |
| `total_connections_received`, `rejected_connections` | Sum of per-IO accept/accept-error counters, including the Unix acceptor. |
| `connected_clients` | Process-wide live-client gauge maintained by IO accept/release. |
| `db0:keys`, `expires` | Sum of shard counts published by owners at batch boundaries. |

The object byte/count/expiry publications occur at executor batch boundaries. INFO and DBSIZE can
therefore lag an in-progress batch, but neither reads an owner-only store. The instantaneous-rate
and network-byte fields are currently zero compatibility placeholders.

## FLUSH scatter/gather precedent

`FLUSHALL` and `FLUSHDB` validate `ASYNC|SYNC` on IO, preflight enough capacity in every target
SPSC task channel, publish one public `Op`, and enqueue one task for every shard. Each executor
clears only shards it owns. The tasks share a heap-side `ScatterState` completion count; after its
store work each owner decrements the count, and the last owner emits `+OK`, marks the original Op
done, and notifies its normal IO completion path. Thus reply ordering remains governed by the
existing ROB and executors never touch sockets. The coordination atomic is outside FlatStore work
and exists only for an invoked scatter command. `ASYNC` and `SYNC` are accepted aliases because
there is no persistence/freeing mode distinction in this server.

This is the intended precedent for later scatter/gather commands: one client-visible Op, complete
capacity preflight before publishing any task, owner-only shard work, and one normal-path final
completion. No partial fan-out is allowed.

## Unix socket design

`--unixsocket PATH` creates one filesystem `AF_UNIX` listener in addition to every IO thread's TCP
listener. A filesystem Unix pathname is a unique bind key on Linux; `SO_REUSEPORT` does not provide
the sound per-thread listener group used for TCP. The first IO thread therefore owns the Unix
multishot accept and round-robins accepted clients to all IO threads through the existing client
channels. The target IO thread assigns the ready-mask slot, owns recv/parse/retire/send for life,
and executors still see only tasks.

At boot, a non-socket path is never removed. An existing live Unix listener is detected with a
connect probe and rejected; only a stale socket that refuses connections is unlinked. The path is
unlinked after an orderly shutdown. When `--unixsocket` is absent, the IO loop selects a template
specialization with no handoff/channel discrimination or pending-handoff check in its hot pass.

## Suggested compile and manual tests

The directed protocol battery is `tests/resp3.py`; `tests/differ.py ... -3` runs each ordinary,
stream, pub/sub, and notification suite after negotiating RESP3 with both TomoKV and its Redis
oracle. A manual session can also be run against a test instance:

```sh
PORT=6380
SOCK=/tmp/tomokv-compat.sock

redis-cli -2 -p "$PORT" PING
redis-cli -2 -p "$PORT" HELLO 2
redis-cli -3 -p "$PORT" HELLO 3
redis-cli -3 -p "$PORT" HGETALL compat:hash
redis-cli -2 -p "$PORT" CLIENT SETNAME compat-check
redis-cli -2 -p "$PORT" CLIENT GETNAME
redis-cli -2 -p "$PORT" CLIENT SETINFO LIB-NAME redis-cli
redis-cli -2 -p "$PORT" CLIENT SETINFO LIB-VER test
redis-cli -2 -p "$PORT" CLIENT INFO
redis-cli -2 -p "$PORT" COMMAND COUNT
redis-cli -2 -p "$PORT" COMMAND INFO GET SCAN not-a-command
redis-cli -2 -p "$PORT" COMMAND DOCS GET SCAN
redis-cli -2 -p "$PORT" CONFIG GET '*max*'
redis-cli -2 -p "$PORT" CONFIG GET save
redis-cli -2 -p "$PORT" CONFIG GET appendonly
redis-cli -2 -p "$PORT" CONFIG SET zc-min 32768 hash-max-compact-value 96
redis-cli -2 -p "$PORT" SET compat:key value
redis-cli -2 -p "$PORT" TYPE compat:key
redis-cli -2 -p "$PORT" DBSIZE
redis-cli -2 -p "$PORT" RANDOMKEY
redis-cli -2 -p "$PORT" --scan --pattern 'compat:*'
redis-cli -2 -p "$PORT" INFO server
redis-cli -2 -p "$PORT" INFO memory
redis-cli -2 -p "$PORT" INFO commandstats
redis-cli -2 -p "$PORT" INFO keyspace
redis-cli -2 -p "$PORT" FLUSHDB ASYNC
redis-cli -2 -p "$PORT" DBSIZE

redis-cli -2 -s "$SOCK" PING
redis-benchmark -p "$PORT" -t ping,set,get -n 10000
```

Use a persistent interactive `redis-cli -2` session to verify that `CLIENT SETNAME`, `GETNAME`,
`RESET`, and `CLIENT ID` apply to the same connection; separate one-shot invocations intentionally
create separate client ids. Also try interactive `HELP GET` to exercise `COMMAND DOCS`.

Edge cases worth checking:

- Bare `HELLO`, `HELLO 2`, `HELLO 3`, `HELLO 1`, `HELLO 4`, `HELLO nope`, and `RESET` after
  negotiation; only versions 2 and 3 succeed, and a bare call never changes the active version.
- `AUTH secret` and `AUTH default secret`; both report that no password is configured.
- invalid CLIENT subcommands, whitespace/control characters in names, unknown SETINFO attributes,
  and `CLIENT NO-EVICT` without `ON|OFF`.
- `CONFIG SET unknown value`, missing value pairs, invalid booleans/policies, negative numerics,
  byte suffixes such as `64mb`, and numeric overflow.
- `SCAN nope`, a cursor whose top byte is outside the configured shard count, `COUNT 0`, a missing
  option value, an unknown option, and an invalid TYPE.
- `FLUSHDB LATER`, more than one mode argument, `SELECT -1`, and `SELECT 1`.
- Empty-keyspace `RANDOMKEY`, SCAN across a resize/expiry workload, and FLUSH while a large zero-copy
  GET reply is in flight (the existing borrow-retirement path keeps its bytes alive).

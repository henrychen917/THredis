# NOTES-CMDGAP — live `COMMAND COUNT` inventory

## Outcome

The baseline live binaries reported `COMMAND COUNT` 237 (TomoKV) versus 250 (vanilla Redis
7.4.2). Sorting the live `COMMAND LIST` replies and comparing only top-level names produced exactly
13 reference-only commands and no TomoKV-only commands. Four small, local entries were added:

- `ASKING`, `READONLY`, and `READWRITE`: one shared IO-local handler returns Redis's exact
  standalone `ERR This instance has cluster support disabled` reply.
- `RESTORE-ASKING`: the cluster-only distinction is bypassing slot-import checks. TomoKV has no
  cluster layer, so the complete standalone behavior is the existing owner-routed `RESTORE`
  handler, including validation, installation, notification, maxmemory, and reply behavior.

The live post-change count is 241 versus 250. The remaining nine names are explicitly classified
below: five are deliberately outside this standalone/single-keyspace server's scope and four need
their own architectural lane. No gap is silently left unclassified.

No runtime knob was added, so `tomokv.conf` has no new entry. `sizeof(Op)` and `sizeof(Client)` are
unchanged. The GET/SET path has no new branch or call: the new family is appended to the boot-built
registry, its four rows do not change the existing 512-slot registry capacity, and appending them
after existing families cannot alter an existing command's probe chain. The three cluster controls
run only when invoked on the IO side; `RESTORE-ASKING` costs exactly the existing `RESTORE` path
when invoked.

The older `NOTES-EDGEPROTO.md` S14 recorded 12 missing names and omitted `REPLCONF`. That list was
stale. This audit uses the required running binaries and found all 13 names.

## How the inventory was measured

Both servers were booted with the comparable debug/save settings on this lane's assigned cores and
ports:

```text
taskset -c 48-55 ./build/tomokv --bind 127.0.0.1 --port 7470 \
  --atomic 0 --enable-debug-command yes
taskset -c 56-63 /tmp/claude-1000/redis74/src/redis-server \
  --bind 127.0.0.1 --port 7471 --save '' --enable-debug-command yes

redis-cli --raw -p 7470 COMMAND LIST | LC_ALL=C sort -u > target.list
redis-cli --raw -p 7471 COMMAND LIST | LC_ALL=C sort -u > oracle.list
comm -23 oracle.list target.list
```

`COMMAND COUNT` counts top-level commands. Redis's live `COMMAND LIST` also emits pipe-qualified
subcommand metadata names: its response had 379 unique names = 250 top-level + 129 subcommands.
The baseline TomoKV response had 237 top-level names and no pipe-qualified entries. Therefore the
COUNT inventory comparison is 250 versus 237, while the unfiltered raw set comparison has 142
reference-only strings (13 top-level + 129 subcommands). The 129 metadata entries are listed in a
separate appendix so this distinction is explicit rather than filtered away silently.

## Inventory audit

This table reports set membership only; `CONSISTENT` here does not claim that all command semantics
were re-differed in this lane.

| Live set check | Baseline | Post-change | Status |
|---|---:|---:|---|
| Redis top-level names | 250 | 250 | oracle |
| TomoKV top-level names | 237 | 241 | four resolved |
| Common top-level names | 237 | 241 | CONSISTENT membership |
| Redis-only top-level names | 13 | 9 | DIFFERS; every row classified below |
| TomoKV-only top-level names | 0 | 0 | CONSISTENT |
| Redis-only pipe-qualified subcommand names | 129 | 129 | DIFFERS; metadata-model handoff below |

The 237 baseline names present on both sides were:

```text
acl append auth bgrewriteaof bgsave bitcount bitfield bitfield_ro bitop bitpos blmove blmpop
blpop brpop brpoplpush bzmpop bzpopmax bzpopmin client command config copy dbsize debug decr
decrby del discard dump echo eval eval_ro evalsha evalsha_ro exec exists expire expireat expiretime
failover fcall fcall_ro flushall flushdb function geoadd geodist geohash geopos georadius
georadius_ro georadiusbymember georadiusbymember_ro geosearch geosearchstore get getbit getdel
getex getrange getset hdel hello hexists hexpire hexpireat hexpiretime hget hgetall hincrby
hincrbyfloat hkeys hlen hmget hmset hpersist hpexpire hpexpireat hpexpiretime hpttl hrandfield
hscan hset hsetnx hstrlen httl hvals incr incrby incrbyfloat info keys lastsave latency lcs lindex
linsert llen lmove lmpop lolwut lpop lpos lpush lpushx lrange lrem lset ltrim memory mget monitor
mset msetnx multi object persist pexpire pexpireat pexpiretime pfadd pfcount pfmerge pfselftest ping
psetex psubscribe pttl publish pubsub punsubscribe quit randomkey rename renamenx replicaof reset
restore role rpop rpoplpush rpush rpushx sadd save scan scard script sdiff sdiffstore select set
setbit setex setnx setrange shutdown sinter sintercard sinterstore sismember slaveof slowlog smembers
smismember smove sort sort_ro spop spublish srandmember srem sscan ssubscribe strlen subscribe substr
sunion sunionstore sunsubscribe time touch ttl type unlink unsubscribe unwatch wait waitaof watch xack
xadd xautoclaim xclaim xdel xgroup xinfo xlen xpending xrange xread xreadgroup xrevrange xsetid xtrim
zadd zcard zcount zdiff zdiffstore zincrby zinter zintercard zinterstore zlexcount zmpop zmscore
zpopmax zpopmin zrandmember zrange zrangebylex zrangebyscore zrangestore zrank zrem zremrangebylex
zremrangebyrank zremrangebyscore zrevrange zrevrangebylex zrevrangebyscore zrevrank zscan zscore
zunion zunionstore
```

### All 13 top-level differences and exact live replies

The replies below include RESP type markers and `\r\n`; trailing spaces in TomoKV's baseline
unknown-command replies are intentional and shown before `\r\n`.

| Command | Probe | Redis 7.4 exact reply | Baseline TomoKV exact reply | Bucket / disposition |
|---|---|---|---|---|
| `ASKING` | `ASKING` | `-ERR This instance has cluster support disabled\r\n` | `-ERR unknown command 'ASKING', with args beginning with: \r\n` | (a) DIFFERS before; resolved and CONSISTENT after |
| `READONLY` | `READONLY` | `-ERR This instance has cluster support disabled\r\n` | `-ERR unknown command 'READONLY', with args beginning with: \r\n` | (a) DIFFERS before; resolved and CONSISTENT after |
| `READWRITE` | `READWRITE` | `-ERR This instance has cluster support disabled\r\n` | `-ERR unknown command 'READWRITE', with args beginning with: \r\n` | (a) DIFFERS before; resolved and CONSISTENT after |
| `RESTORE-ASKING` | `RESTORE-ASKING cmdgap:restore 0 bad` | `-ERR DUMP payload version or checksum are wrong\r\n` | `-ERR unknown command 'RESTORE-ASKING', with args beginning with: 'cmdgap:restore' '0' 'bad' \r\n` | (a) DIFFERS before; resolved and CONSISTENT after |
| `CLUSTER` | `CLUSTER` | `-ERR wrong number of arguments for 'cluster' command\r\n` | `-ERR unknown command 'CLUSTER', with args beginning with: \r\n` | (b) DIFFERS; deliberate standalone/no-cluster scope |
| `MODULE` | `MODULE` | `-ERR wrong number of arguments for 'module' command\r\n` | `-ERR unknown command 'MODULE', with args beginning with: \r\n` | (b) DIFFERS; dynamic module ABI/code loading is deliberately absent |
| `MOVE` | `MOVE cmdgap:missing 1` | `:0\r\n` | `-ERR unknown command 'MOVE', with args beginning with: 'cmdgap:missing' '1' \r\n` | (b) DIFFERS; server deliberately exposes only database 0 |
| `PFDEBUG` | `PFDEBUG ENCODING cmdgap:missing` | `-ERR The specified key does not exist\r\n` | `-ERR unknown command 'PFDEBUG', with args beginning with: 'ENCODING' 'cmdgap:missing' \r\n` | (b) DIFFERS; undocumented Redis-internal HLL representation debugger |
| `SWAPDB` | `SWAPDB 0 0` | `+OK\r\n` | `-ERR unknown command 'SWAPDB', with args beginning with: '0' '0' \r\n` | (b) DIFFERS; server deliberately exposes only database 0 |
| `MIGRATE` | `MIGRATE` | `-ERR wrong number of arguments for 'migrate' command\r\n` | `-ERR unknown command 'MIGRATE', with args beginning with: \r\n` | (c) DIFFERS; own lane: outbound networking plus multi-key/cross-owner coordination |
| `PSYNC` | `PSYNC` | `-ERR wrong number of arguments for 'psync' command\r\n` | `-ERR unknown command 'PSYNC', with args beginning with: \r\n` | (c) DIFFERS; replication lane |
| `REPLCONF` | `REPLCONF` | `+OK\r\n` | `-ERR unknown command 'REPLCONF', with args beginning with: \r\n` | (c) DIFFERS; replication handshake state belongs with PSYNC/SYNC |
| `SYNC` | `SYNC x` | `-ERR wrong number of arguments for 'sync' command\r\n` | `-ERR unknown command 'SYNC', with args beginning with: 'x' \r\n` | (c) DIFFERS; replication lane and connection streaming mode |

Bucket (a) contains only the four implemented rows. Bucket (b) is intentionally not padded with
partial registry entries: advertising `MOVE`, `SWAPDB`, `CLUSTER`, `MODULE`, or `PFDEBUG` while
implementing only a convenient no-op/error subset would make `COMMAND LIST` claim a feature the
server does not provide. Bucket (c) was left code-free as required: implementing it locally would
touch outbound connection ownership, full-dataset/scatter coordination, or replication streaming.

For the implemented RESTORE alias, the live Redis success probe was also confirmed before coding:

```text
RESTORE-ASKING cmdgap:restore 0 <live DUMP>          => +OK
GET cmdgap:restore                                  => $10 wire\x00value
RESTORE-ASKING cmdgap:restore 0 <same DUMP>          => -BUSYKEY Target key name already exists.
RESTORE-ASKING cmdgap:restore 0 <same DUMP> REPLACE  => +OK
```

The directed battery repeats this with a server-produced DUMP and reads the full binary value back,
so the restore mechanism must fire. The differ uses a valid DUMP captured from the running Redis
7.4 oracle and follows successful restores with `GET`.

## Pipe-qualified `COMMAND LIST` metadata gap

All 129 entries below are DIFFERS in live set membership. They are not part of `COMMAND COUNT`.
Many of the underlying subcommands are already implemented through their container handler; the
difference is that TomoKV has one `CommandSpec`/ACL bit per top-level command and `COMMAND LIST`
enumerates that registry, whereas Redis separately exposes subcommand metadata. Naively inserting
these as dispatch rows would misreport COUNT semantics, top-level dispatch, and the 255-bit ACL
budget. A proper fix needs its own introspection/registry metadata lane. The `cluster|*` and
`module|*` groups additionally depend on their deliberately absent parent features.

| Parent | Count | Redis-only live names |
|---|---:|---|
| `acl` | 13 | `acl|cat`, `acl|deluser`, `acl|dryrun`, `acl|genpass`, `acl|getuser`, `acl|help`, `acl|list`, `acl|load`, `acl|log`, `acl|save`, `acl|setuser`, `acl|users`, `acl|whoami` |
| `client` | 18 | `client|caching`, `client|getname`, `client|getredir`, `client|help`, `client|id`, `client|info`, `client|kill`, `client|list`, `client|no-evict`, `client|no-touch`, `client|pause`, `client|reply`, `client|setinfo`, `client|setname`, `client|tracking`, `client|trackinginfo`, `client|unblock`, `client|unpause` |
| `cluster` | 28 | `cluster|addslots`, `cluster|addslotsrange`, `cluster|bumpepoch`, `cluster|count-failure-reports`, `cluster|countkeysinslot`, `cluster|delslots`, `cluster|delslotsrange`, `cluster|failover`, `cluster|flushslots`, `cluster|forget`, `cluster|getkeysinslot`, `cluster|help`, `cluster|info`, `cluster|keyslot`, `cluster|links`, `cluster|meet`, `cluster|myid`, `cluster|myshardid`, `cluster|nodes`, `cluster|replicas`, `cluster|replicate`, `cluster|reset`, `cluster|saveconfig`, `cluster|set-config-epoch`, `cluster|setslot`, `cluster|shards`, `cluster|slaves`, `cluster|slots` |
| `command` | 7 | `command|count`, `command|docs`, `command|getkeys`, `command|getkeysandflags`, `command|help`, `command|info`, `command|list` |
| `config` | 5 | `config|get`, `config|help`, `config|resetstat`, `config|rewrite`, `config|set` |
| `function` | 9 | `function|delete`, `function|dump`, `function|flush`, `function|help`, `function|kill`, `function|list`, `function|load`, `function|restore`, `function|stats` |
| `latency` | 7 | `latency|doctor`, `latency|graph`, `latency|help`, `latency|histogram`, `latency|history`, `latency|latest`, `latency|reset` |
| `memory` | 6 | `memory|doctor`, `memory|help`, `memory|malloc-stats`, `memory|purge`, `memory|stats`, `memory|usage` |
| `module` | 5 | `module|help`, `module|list`, `module|load`, `module|loadex`, `module|unload` |
| `object` | 5 | `object|encoding`, `object|freq`, `object|help`, `object|idletime`, `object|refcount` |
| `pubsub` | 6 | `pubsub|channels`, `pubsub|help`, `pubsub|numpat`, `pubsub|numsub`, `pubsub|shardchannels`, `pubsub|shardnumsub` |
| `script` | 6 | `script|debug`, `script|exists`, `script|flush`, `script|help`, `script|kill`, `script|load` |
| `slowlog` | 4 | `slowlog|get`, `slowlog|help`, `slowlog|len`, `slowlog|reset` |
| `xgroup` | 6 | `xgroup|create`, `xgroup|createconsumer`, `xgroup|delconsumer`, `xgroup|destroy`, `xgroup|help`, `xgroup|setid` |
| `xinfo` | 4 | `xinfo|consumers`, `xinfo|groups`, `xinfo|help`, `xinfo|stream` |

## Code and test design

- `src/cmd/cmdgap.cc` owns the four new registry rows and the shared standalone error handler.
- `RESTORE-ASKING` reuses `cmd_restore`/`cmd_restore_notify`; no shard is touched except by its
  existing owner-routed RESTORE execution.
- The generated Redis 7.4 ACL masks are `fast connection` for the three controls and
  `keyspace write slow dangerous` for `RESTORE-ASKING`.
- The ACL generator and its directed parser now accept hyphens in top-level command names. Before
  that correction, they silently skipped the first hyphenated row.
- `tests/cmdgap.py` performs 22 directed checks: count/list/info metadata, exact standalone and
  arity errors, a real DUMP/restore/read-back, BUSYKEY/REPLACE/TTL/option validation, ordinary
  RESTORE, nine shelved-name controls, and a genuinely unknown-command control.
- `tests/differ.py ... cmdgap <seed>` emits 4,213 operations: randomized valid/invalid
  `RESTORE-ASKING`, follow-up reads/deletes, cluster-disabled replies, arity errors, and unknown
  controls, plus a directed all-handler tail.

## Test evidence

The required pre-change failure was observed before adding registry rows:

```text
CMDGAP FAIL: 20 checks, 14 failures
DIFFER cmdgap: 4213 ops, 3580 diffs -> FAIL
```

Build and generated-table validation:

```text
make clean && make -j8
# exit 0
python3 tools/gen_acl_categories.py --check src/cmd/acl_categories_generated.h
# exit 0
make asan
# exit 0
```

Release, `--atomic 0`, seeds 101/202 and RESP3 seed 303:

```text
CMDGAP PASS: 22 checks; 4 inventory rows, 3 cluster-disabled replies, 1 restore alias fired
DIFFER cmdgap: 4213 ops, 0 diffs -> PASS
DIFFER cmdgap: 4213 ops, 0 diffs -> PASS
DIFFER cmdgap: 4213 ops, 0 diffs -> PASS
```

Release, `--atomic 1`, the same three seeds:

```text
CMDGAP PASS: 22 checks; 4 inventory rows, 3 cluster-disabled replies, 1 restore alias fired
DIFFER cmdgap: 4213 ops, 0 diffs -> PASS
DIFFER cmdgap: 4213 ops, 0 diffs -> PASS
DIFFER cmdgap: 4213 ops, 0 diffs -> PASS
```

That is 25,278 release differential operations across three seeds and both atomic modes, with zero
diffs. `tests/acl_categories.py` also passed in each release mode. The existing server-tail battery
passed after the inventory change:

```text
servertail: 101 checks, 0 failures -> PASS
```

ASAN+UBSAN, `--atomic 0`, directed battery plus seed 404; then the directed battery under
`--atomic 1`:

```text
CMDGAP PASS: 22 checks; 4 inventory rows, 3 cluster-disabled replies, 1 restore alias fired
DIFFER cmdgap: 4213 ops, 0 diffs -> PASS
CMDGAP PASS: 22 checks; 4 inventory rows, 3 cluster-disabled replies, 1 restore alias fired
```

Both sanitizer logs were checked after the exact listening PID terminated: no AddressSanitizer,
LeakSanitizer, or UBSAN runtime report was present.

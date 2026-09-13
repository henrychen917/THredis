# NOTES-COMPATINTRO — introspection and notification differential coverage

## Outcome

This lane adds the `compatintro` custom generator/driver to `tests/differ.py`. It exercises
COMMAND, CLIENT, CONFIG, ACL, and keyspace notifications against the assigned vanilla Redis 7.4.2
binary. Deterministic replies are byte-compared. Dictionary order, connection-specific CLIENT
values, and the already documented minimal COMMAND metadata are compared at an explicit semantic
boundary; every raw difference observed there is listed below.

Four small, connection-local defects were found and resolved:

1. `COMMAND GETKEYS` now decodes movable-key grammar for EVAL/EVALSHA/EVAL_RO/EVALSHA_RO,
   FCALL/FCALL_RO, XREAD/XREADGROUP, LMPOP/BLMPOP/ZMPOP/BZMPOP, SINTERCARD/ZINTERCARD, the
   ZUNION/ZINTER/ZDIFF families, GEORADIUS/GEORADIUSBYMEMBER STORE/STOREDIST, and SORT STORE.
2. `CONFIG GET` accepts several patterns, de-duplicates overlaps, and returns their union.
3. `COMMAND LIST FILTERBY PATTERN` is case-insensitive, as Redis is.
4. CLIENT SETNAME/SETINFO retains Redis's binary-safe treatment of NUL while continuing to reject
   spaces, CR/LF, TAB, other non-NUL ASCII controls, and DEL.

All changes are cold control-plane work. No `Op`, `Client`, shard, scatter-core, MVCC-resolver,
or GET/SET hot-path layout/code changed. No knob was added, so `tomokv.conf` needs no new entry.

## Generated command surface

- `COMMAND COUNT/INFO/DOCS/LIST/GETKEYS/GETKEYSANDFLAGS`.
- Static key ranges: GET, MGET, ZADD, MEMORY USAGE, BITOP, MSET, RENAME.
- Movable ranges: scripts/functions, XREAD forms, all MPOP forms, CARD and ZSET aggregations,
  GEORADIUS STORE/STOREDIST, and SORT BY/GET/STORE.
- `CONFIG GET` with one to three names/globs, unknown and overlapping patterns, and list-valued
  output; multi-pair SET, invalid-later-pair all-or-nothing controls, and RESETSTAT.
- `CLIENT ID/GETNAME/SETNAME/SETINFO/INFO/LIST/NO-EVICT/NO-TOUCH/UNPAUSE`. INFO/LIST validates
  all 31 field names in order and separately compares stable values.
- `ACL CAT/GETUSER/LIST/WHOAMI/USERS/SETUSER/DELUSER`, including key/channel patterns and
  malformed rules. The selector gap is probed and recorded, not implemented here.
- Notification routes K/E; classes `g $ l s h z t x m n A`; exact channel/event bytes;
  SET+EXPIRE ordering; a no-change SADD; and disabled-feature silence. Five zero-event controls
  accompany 15 observed frames.

`--list-generators` now also lists the pre-existing custom suites and `compatintro`.

## Audit table

| Surface | Status | Result |
|---|---|---|
| GETKEYS static ranges and wrong-arity/no-key errors | CONSISTENT | Byte-exact. |
| GETKEYS movable forms above | CONSISTENT after fix | 1,550 randomized rows/seed plus invalid-numkeys controls. |
| GETKEYSANDFLAGS readonly GET/MGET | CONSISTENT | Byte-exact `RO access`. |
| GETKEYSANDFLAGS write/mixed intent | DIFFERS; handed on | Key lists exact; per-key intent metadata is absent. Exact replies below. |
| COMMAND LIST pattern membership | CONSISTENT after fix | Uppercase `*POP` finds the same nine names. |
| COMMAND LIST and ACL CAT raw order | DIFFERS; property boundary | Membership is identical for compared common sets. |
| COMMAND COUNT | DIFFERS; expected inventory | TomoKV 237, Redis 250. |
| COMMAND INFO name/arity/legacy range | CONSISTENT | Compared by the existing routing-field normalizer. |
| COMMAND INFO rich fields and DOCS prose | DIFFERS; documented | TomoKV intentionally ships minimal generated metadata. |
| CLIENT ID | CONSISTENT property | Positive/stable; numeric bytes are connection-specific. |
| SETNAME/GETNAME and rejected text controls | CONSISTENT | Exact replies and state. |
| Embedded-NUL client name | CONSISTENT after fix | Both accept and round-trip eight bytes. |
| CLIENT INFO/LIST field names/order | CONSISTENT | All 31 names; NO-EVICT `e` and NO-TOUCH `T` match. |
| CLIENT volatile values and `cmd` | DIFFERS | Addresses/fds/buffers vary; ours says `cmd=client`, Redis `cmd=client|info`. |
| NO-EVICT/NO-TOUCH/UNPAUSE | CONSISTENT | Byte-exact replies/state; NO-EVICT remains documented inert policy. |
| CONFIG GET single/glob/unknown/list rendering | CONSISTENT | Exact pairs and values. |
| CONFIG GET several parameters | CONSISTENT after fix | 600+ multi-name observations/seed; overlaps de-duplicate. |
| CONFIG GET raw aggregate order | DIFFERS; map boundary | Same exact pairs, different ordering. |
| CONFIG SET multi/invalid and RESETSTAT | CONSISTENT | Invalid multi-SET applies none; exact errors/replies. |
| ACL CAT top-level names | CONSISTENT | Exact 21-element reply. |
| ACL CAT string/list/set/sortedset/hash/bitmap/geo | CONSISTENT as sets | Same members; raw order differs. |
| ACL CAT stream/pubsub | DIFFERS; handed on | Redis exposes container subcommands as separate names. |
| ACL GETUSER/LIST/WHOAMI/USERS and malformed rules | CONSISTENT | 1,000 randomized ACL operations/seed. |
| ACL selectors | DIFFERS; handed on | Existing ACL lane explicitly rejects selectors. |
| Notification routes/classes/names/channels/order | CONSISTENT | 15 observed frames plus five zero controls. |
| No-change and notifications-disabled controls | CONSISTENT | Both subscriber sockets stayed empty. |

## Exact unresolved replies

Notation is decoded RESP; `+...` retains simple-string type where it matters.

### COMMAND inventory, order, and metadata

```text
COMMAND COUNT
ours  = 237
redis = 250

COMMAND LIST FILTERBY PATTERN *POP
ours  = [lpop,rpop,blpop,brpop,blmpop,lmpop,spop,bzmpop,zmpop]
redis = [blpop,blmpop,lpop,spop,bzmpop,lmpop,brpop,rpop,zmpop]

COMMAND INFO GET
ours  = [[get,2,[readonly],1,1,1,[],[],[],[]]]
redis = [[get,2,[+readonly,+fast],1,1,1,[+@read,+@string,+@fast],[],
          [[flags,[+RO,+access],begin_search,[type,index,spec,[index,1]],
            find_keys,[type,range,spec,[lastkey,0,keystep,1,limit,0]]]],[]]]

COMMAND DOCS GET
ours  = [get,[summary,"tomokv compatible get command",since,"0.1.0",group,generic,
              complexity,"O(1) or proportional to returned work"]]
redis = [get,[summary,"Returns the string value of a key.",since,"1.0.0",group,string,
              complexity,"O(1)",arguments,
              [[name,key,type,key,display_text,key,key_spec_index,0]]]]
```

### GETKEYSANDFLAGS

```text
ZADD z 1 m
ours  = [[z,[+RW,+access,+update]]]
redis = [[z,[+RW,+update]]]

RENAME a b
ours  = [[a,[+RW,+access,+update]],[b,[+RW,+access,+update]]]
redis = [[a,[+RW,+access,+delete]],[b,[+OW,+update]]]

MSET a 1 b 2
ours  = [[a,[+RW,+access,+update]],[b,[+RW,+access,+update]]]
redis = [[a,[+OW,+update]],[b,[+OW,+update]]]

GEORADIUS g 0 0 1 km STORE d
ours  = [[g,[+RW,+access,+update]],[d,[+RW,+access,+update]]]
redis = [[g,[+RO,+access]],[d,[+OW,+update]]]
```

### CONFIG aggregate order

```text
CONFIG GET save appendonly appendfsync maxmemory maxmemory-policy maxmemory-samples
           maxclients timeout tcp-keepalive client-output-buffer-limit notify-keyspace-events

ours  = [save,"",appendonly,no,appendfsync,everysec,maxmemory,0,maxmemory-policy,noeviction,
         maxmemory-samples,5,maxclients,10000,timeout,0,tcp-keepalive,300,
         client-output-buffer-limit,
         "normal 0 0 0 slave 268435456 67108864 60 pubsub 33554432 8388608 60",
         notify-keyspace-events,""]
redis = [maxclients,10000,appendfsync,everysec,notify-keyspace-events,"",
         client-output-buffer-limit,
         "normal 0 0 0 slave 268435456 67108864 60 pubsub 33554432 8388608 60",
         save,"",maxmemory,0,maxmemory-policy,noeviction,tcp-keepalive,300,appendonly,no,
         maxmemory-samples,5,timeout,0]
```

### ACL container subcommands and selectors

```text
ACL CAT stream
ours  = [xack,xadd,xautoclaim,xclaim,xdel,xlen,xpending,xrange,xread,xreadgroup,xrevrange,xsetid,xtrim]
redis = [xread,xack,xclaim,xdel,xsetid,xgroup|destroy,xgroup|delconsumer,
         xgroup|createconsumer,xgroup|create,xgroup|help,xgroup|setid,xpending,xrange,xlen,
         xinfo|stream,xinfo|consumers,xinfo|help,xinfo|groups,xtrim,xreadgroup,xrevrange,xadd,
         xautoclaim]

ACL CAT pubsub
ours  = [psubscribe,publish,punsubscribe,spublish,ssubscribe,subscribe,sunsubscribe,unsubscribe]
redis = [unsubscribe,spublish,subscribe,punsubscribe,ssubscribe,sunsubscribe,psubscribe,
         pubsub|channels,pubsub|numpat,pubsub|shardchannels,pubsub|shardnumsub,pubsub|numsub,
         publish]

ACL SETUSER ci:selector reset on nopass "(~sel:* +get)"
ours  = -ERR Error in ACL SETUSER modifier '(~sel:* +get)': ACL selectors are not supported
redis = +OK
```

### Captured CLIENT INFO rows

The 31 names are identical and ordered identically. These volatile values are reproduced exactly
from the confirming probe.

```text
ours  = id=3 addr=127.0.0.1:35140 laddr=127.0.0.1:7440 fd=15 name=ci_audit age=0 idle=0 flags=N db=0 sub=0 psub=0 ssub=0 multi=-1 watch=0 qbuf=0 qbuf-free=15998 argv-mem=0 multi-mem=0 rbs=16384 rbp=16384 obl=0 oll=0 omem=0 tot-mem=18368 events=r cmd=client user=default redir=-1 resp=2 lib-name= lib-ver=

redis = id=4 addr=127.0.0.1:51298 laddr=127.0.0.1:7445 fd=11 name=ci_audit age=0 idle=0 flags=N db=0 sub=0 psub=0 ssub=0 multi=-1 watch=0 qbuf=26 qbuf-free=20448 argv-mem=10 multi-mem=0 rbs=16384 rbp=16384 obl=0 oll=0 omem=0 tot-mem=37786 events=r cmd=client|info user=default redir=-1 resp=2 lib-name= lib-ver=
```

## Failing-before and passing-after evidence

Rows were observed against the pre-change target and then added to `compatintro`.

```text
COMMAND GETKEYS EVAL "return 1" 2 a b x
before ours=[a,b,x] redis=[a,b]
after  ours=[a,b]   redis=[a,b]

COMMAND GETKEYS XREAD COUNT 2 STREAMS a b 0 0
before ours=-ERR The command has no key arguments  redis=[a,b]
after  ours=[a,b]                                  redis=[a,b]

COMMAND GETKEYS LMPOP 2 a b LEFT COUNT 2
before ours=[a,b,LEFT,COUNT,2] redis=[a,b]
after  ours=[a,b]              redis=[a,b]

COMMAND GETKEYS GEORADIUS g 0 0 1 km STORE dst
before ours=[g,0,0,1,km,STORE,dst] redis=[g,dst]
after  ours=[g,dst]                  redis=[g,dst]

COMMAND GETKEYS SORT src BY w:* GET x:* STORE dst
before ours=[src]     redis=[src,dst]
after  ours=[src,dst] redis=[src,dst]

CONFIG GET timeout maxmemory
before ours=-ERR syntax error redis=[maxmemory,0,timeout,0]
after  ours=[maxmemory,0,timeout,0] redis=[maxmemory,0,timeout,0]

COMMAND LIST FILTERBY PATTERN *POP
before ours=[] redis=[nine *pop names]
after  sorted(ours)=sorted(redis)=[blmpop,blpop,brpop,bzmpop,lmpop,lpop,rpop,spop,zmpop]

CLIENT SETNAME <8 bytes: bad NUL name>
before ours=-ERR Client names cannot contain spaces, newlines or special characters. redis=+OK
after  ours=+OK redis=+OK; GETNAME returns the same eight bytes on both.
```

## Test evidence

Assigned resources only: target cores 112-119/port 7440; Redis cores 120-123/port 7445; driver
core 124. Target topology: 4 IO + 4 EX, 16 shards. Builds used `make -j8`; no gate/NIC test ran.

Release, `--atomic 0`:

```text
DIFFER compatintro seed 7:  5630 checks, 0 diffs -> PASS
DIFFER compatintro seed 37: 5603 checks, 0 diffs -> PASS
DIFFER compatintro seed 99: 5607 checks, 0 diffs -> PASS
```

Release, `--atomic 1`:

```text
DIFFER compatintro seed 7:  5630 checks, 0 diffs -> PASS
DIFFER compatintro seed 37: 5603 checks, 0 diffs -> PASS
DIFFER compatintro seed 99: 5607 checks, 0 diffs -> PASS
```

Regression differentials:

```text
DIFFER climon: 4318 ops, 0 diffs -> PASS
DIFFER notify: 345 ops, 470 events, 0 diffs -> PASS
```

ASAN+UBSAN (`make asan`, abort/halt on error, leak detection enabled), seed 7 under both atomic
modes passed the final 5,630-check suite. Both orderly shutdowns reported `live_conns=0`,
`rob_not_quiesced=0`, and `unsent_bytes_pending=0`, with no sanitizer or leak diagnostic.

## Scope handed on

- Full COMMAND INFO flags/categories/key specs and COMMAND DOCS prose.
- Per-key GETKEYSANDFLAGS intent (`RO/RW/OW/RM`, access/update/delete). The 48-byte
  `CommandSpec` has only command-level classification; this lane did not grow it.
- ACL selectors and enforcement.
- Registry representation for container subcommands, accounting for ACL CAT stream/pubsub.
- Raw order of unordered COMMAND LIST/ACL CAT arrays and CONFIG maps was not forced to Redis's
  dictionary order.

There are no silent scope cuts.

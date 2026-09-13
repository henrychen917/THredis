# DUMP / RESTORE compatibility boundary

TomoKV `DUMP` and `RESTORE` use the Redis RDB value payload for strings, lists, hashes, sets, and
sorted sets. The old `TOMODMP` self-format no longer exists.

The envelope is:

```
[ RDB value type ][ RDB value body ]
[ u16 little-endian RDB version = 12 ]
[ u64 little-endian Jones CRC64 over all preceding bytes ]
```

The decoder accepts Redis 7.4's current runtime encodings for these types, including integer and
LZF strings, listpack, intset, quicklist2 packed/plain nodes, hashtable collections, and zset2
binary doubles. The encoder uses Redis's simple valid type for each value. It does not preserve a
TomoKV internal compact/expanded representation.

`RESTORE key ttl payload` follows Redis TTL grammar: `ttl` is relative milliseconds, `ABSTTL`
makes a nonzero value an absolute Unix-millisecond deadline, and zero means persistent. An absolute
deadline already in the past returns `OK` without creating a key; with `REPLACE`, an existing key is
removed. `DUMP` never embeds the source TTL, so preserving it is explicit: read the source `PTTL`
and pass that value to `RESTORE`.

`REPLACE`, `ABSTTL`, `IDLETIME seconds`, and `FREQ frequency` retain their Redis-compatible option
grammar and range checks. Valid `IDLETIME` and `FREQ` values are accepted but TomoKV has no portable
per-key idle/frequency state to install. Without `REPLACE`, an existing destination returns exactly
`BUSYKEY Target key name already exists.`

Streams and module values are outside this codec's scope. Unsupported RESTORE type bytes return
`ERR Bad data format`; `DUMP` on a stream returns `ERR object could not be serialized`.

Implementation and test evidence are in `NOTES-WIREDUMP.md`.

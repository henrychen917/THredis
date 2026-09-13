# t-edgeproto — hunting errors on the protocol and argument surface

Oracle: vanilla redis **7.4.2** at `/tmp/claude-1000/redis74/src/redis-server`. Every expected byte
in this lane came from probing that binary, never from reading its source. Lane resources: cores
96–111, ports 7420–7429 (7420 target, 7421 ASAN target, 7422 base-binary target, 7425 oracle).

## Result at a glance

| Instrument | before (base `4f4c88082`) | after | what it measures |
|---|---|---|---|
| `tests/edgeproto.py` | **274 of 379 checks FAIL**, server dead after the WATCH row | **0 fail** | directed battery, exact bytes |
| `tests/differ.py … edgeproto 7` | **2708 diffs / 5200 ops** | **0 diffs** | byte-compared op stream vs 7.4 |
| double-format campaign (12 887 values, ZADD INCR/ZSCORE) | **880 diffs** | **18 diffs** | RESP2 and RESP3 identically |
| numeric-edge corpus (4495 probes) | 657 diffs | 53 diffs | residual is the shelved list below |
| option-grammar corpus (610 probes) | 29 diffs | 15 diffs | ditto |
| validation-order / duplicate-key corpus (137) | 8 diffs, **server aborted mid-run** | 0 diffs | ditto |
| `WATCH k` then disconnect | **server aborts (core dumped)** | survives | P0 |

`tests/edgeproto.py` passes byte-for-byte against the ORACLE too (379/379), which is what makes it
a parity battery rather than a record of current behaviour.

## Hypothesis table

The brief's hypotheses, each with the verdict the instruments returned. "NOT REPRODUCED" means the
probes ran and found nothing — recorded so nobody re-runs them.

| # | Hypothesis | Verdict |
|---|---|---|
| H1 | Numeric edges: negative/0/−1/INT64 limits/one past them, floats where ints go, `""`, `+5`, `05`, `" 5"`, `"5 "` | **REPRODUCED**, wide. Fixed (F3). Residual: redis's bare-`strtod` score ranges (S3) |
| H2 | Error TEXT parity, and the ORDER of checks when two things are wrong | **REPRODUCED**. Fixed (F4). Residual: `container\|sub` arity naming (S1), XPENDING/XCLAIM order (S5) |
| H3 | Arity: min, max, one past each; odd pairs; duplicated keys/fields | **REPRODUCED** for min/max and for the odd-pair message. Fixed (F5). Duplicate keys and duplicate fields: **NOT REPRODUCED** — 44 probes (`MSET k 1 k 2`, `HSET h f 1 f 2`, `ZADD z 1 m 2 m`, `SADD/SREM/DEL/EXISTS/TOUCH/UNLINK/WATCH/MGET/PFCOUNT` repeats, `ZUNIONSTORE 2 z z`, self-`RENAME`/`COPY`/`SMOVE`/`LMOVE`/`RPOPLPUSH`, `BITOP AND d k k`) all byte-identical |
| H4 | RESP2 vs RESP3 shapes: maps, sets, doubles, big numbers, verbatim, nulls, nested arrays, errors, empty collections | **REPRODUCED** for two nulls and for doubles. Fixed (F6, F7). Everything else in a 118-case RESP3 sweep matched (`HGETALL` map, `SMEMBERS` set, `ZSCORE`/`ZINCRBY`/`INCRBYFLOAT` doubles, `XINFO`, `ZRANK … WITHSCORE`, `SCRIPT EXISTS`, error replies, empty collections). Residual: `ACL GETUSER` map/set shape (S6), `DEBUG PROTOCOL` absent (S7), `LOLWUT` bulk vs verbatim (S8) |
| H5 | Option parsing: unknown, repeated, mutually exclusive, unusual order, case, valid-on-another-subcommand | **REPRODUCED**. Fixed (F5, F4). Case-insensitivity itself: **NOT REPRODUCED** — 22 all-lower-case forms (`set`/`GeT`/`Ex`/`nX`/`gt`/`ch`/`before`/`nomkstream`/`left`/`count`/`byte`/`withscores`/`novalues`/`asc`) all matched. Residual: tolerated repeats (S4) |
| H6 | Empty and binary-unsafe input: empty keys/values, NUL/CR/LF/space in names, 8 KB key names, binary through every path | **NOT REPRODUCED** — 92 probes, 1 diff, and that one is the `strtod` item (S3), not a binary-safety problem. Empty key `""`, `b"ep:\x00key\r\n with space\xff"`, NUL-in-value, 8000-byte key names, `SETRANGE` at the 512 MB proto edge and `SETBIT` at 2^32 all agreed |
| H7 | Subcommand routing: unknown subcommands, subcommand's own arity wrong | **REPRODUCED**. Fixed for XGROUP/XINFO (F5); the rest is S1 |
| — | (found off-brief) A connection closing with a live `WATCH` | **REPRODUCED — P0 remote abort.** Fixed (F1) |
| — | (found off-brief) A rejected `XTRIM` writing TWO replies | **REPRODUCED — connection desync.** Fixed (F2) |

## What was fixed

### F1 — P0: `WATCH k` followed by a disconnect aborted the whole server

```
$ redis-cli -p 7420 WATCH k      # then the client goes away
$ redis-cli -p 7420 PING
Could not connect to Redis at 127.0.0.1:7420: Connection refused
```

One command from any client, no privileges needed. Deterministic; four consecutive boots, four
aborts. `UNWATCH`, `EXEC` or `DISCARD` before the disconnect avoids it, which is why no existing
battery caught it — they all tear down cleanly.

Backtrace (`gdb -batch -ex run`, thread 3):

```
#4  __GI_abort ()
#5  tomo::multi_execute_task (...) at src/cmd/multi.inc:1340
#6  tomo::ExLoop::execute (...) at src/core/ex_loop.h:621
```

`multi_close_entry` posts the watch-release fragment of a vanished connection as
`multi_make_task(nullptr, UINT64_MAX, shard, state)` — deliberately client-less, and the tail of
`multi_execute_task` has an explicit `if (task.op_id == UINT64_MAX) { destroy_multi_state(...) }`
branch for exactly that. The notify-v2 change (`ded646a91`) added a carrier lookup at the top of
the same function and an `if (!carrier) std::abort()` in front of it, contradicting the tail. The
line after it already read `carrier && …`, so the intent was clearly to tolerate a null carrier;
only `NotifyExecutionScope`'s `Op&` parameter forced the dereference.

Fix: `NotifyExecutionScope` gains a pointer-taking constructor (it never reads its `Op` when
disarmed, and a client-less fragment raises no notifications), and the abort goes. Nothing in the
MVCC resolver or the scatter engine is touched. Verified afterwards that a dirtied `WATCH` still
aborts its `EXEC` (`*-1`) and a clean one still commits.

### F2 — a rejected `XTRIM` put TWO replies on the wire

```
XTRIM s MINID notanid   ->  -ERR Invalid stream ID specified as stream command argument
                            -ERR syntax error            <-- second reply, nobody asked
```

Every later reply on that connection is then off by one. Reproduced on an unpipelined connection
for `MAXLEN <bad>`, `MAXLEN -1`, `MINID <bad>`, `LIMIT -1` and an unknown trim keyword.

Root cause is general, not stream-specific: `Op::sink()` prefers the op's **direct** region and
only spills to `op.reply`, so a short error leaves `op.reply` empty. Three callers used
`op.reply.empty()` as "have I already answered?", read it as "nothing written yet", and appended a
fallback error. (A fourth, `blocking.inc:1267`, already consulted both regions — that is where the
correct predicate was found.) `Op::replied()` now names the check, and the three sites use it.

Only `XTRIM` was reachable in practice; the two `blocking.inc` sites are fixed by the same change
because the next handler to reply through the direct region would hit them.

### F3 — argument integers now accept only the decimal spelling redis accepts

Redis's `string2ll` accepts exactly the text that formatting the number produces again: no leading
`+`, no leading zeroes, no `-0`, no surrounding whitespace. Half this tree's parsers already did
that (`t_string`, `t_zset`, `t_set`, `t_hash`); the other half used `std::from_chars` or an
explicit `+` branch and were lax. **It was not only cosmetic:**

```
RPUSH l a b c d e
LPOP l 05        redis: -ERR value is out of range, must be positive
                 tomo:  *5 (a b c d e)   <-- five elements popped by an argument redis refuses
```

Tightened, with the canonical guard written the same way the strict parsers already spell it:
`t_list.cc`, `xshard_commands.inc`, `server_tail.cc`, `slowlog.cc`, `climon.cc`, `lcs.cc`, a new
`parse_i64_canonical` for `SCAN COUNT` in `t_server.cc` (the lax `parse_i64_slice` stays for
CONFIG values, where a conf file writing `010` has always worked), and a new `parse_i64_option` in
`t_stream.cc`/`t_stream_groups.cc`.

Stream **IDs** were deliberately left permissive: probed on the oracle, `XADD s 05-1 f v` really
does create `5-1` there — redis parses IDs with `string2ull`, not `string2ll`.

Commands whose behaviour changed: LPOP RPOP LINDEX LRANGE LSET LREM LTRIM LPOS(RANK/COUNT/MAXLEN)
SINTERCARD(numkeys,LIMIT) ZINTERCARD(LIMIT) ZUNIONSTORE(numkeys) ZRANGESTORE(LIMIT)
XRANGE/XREVRANGE(COUNT) XTRIM/XADD(MAXLEN,LIMIT) XAUTOCLAIM(min-idle) XSETID(ENTRIESADDED)
WAIT WAITAOF SLOWLOG GET LCS(MINMATCHLEN) SCAN(COUNT) CLIENT UNBLOCK/KILL ID.

### F4 — error text, and which error wins when two things are wrong

* **Expire time** validates in redis's two stages: `string2ll` first, range second. `SETEX k abc v`
  said `invalid expire time in 'setex' command` where redis says `value is not an integer or out of
  range`; the range message is kept for `SETEX k 0 v`. Same for `SET … EX|PX|EXAT`, `GETEX`,
  `EXPIRE`/`PEXPIRE`/`EXPIREAT`/`PEXPIREAT`.
* **Count arguments** report the message redis passes to `getRangeLongFromObject`, for the parse
  failure as well as the range failure: `LPOP`/`RPOP`/`SPOP`/`ZPOPMIN`/`ZPOPMAX` →
  `value is out of range, must be positive`; `LPOS COUNT`/`MAXLEN` → `COUNT|MAXLEN can't be
  negative`; `SINTERCARD` numkeys → `numkeys should be greater than 0`, its `LIMIT` and
  `ZINTERCARD`'s → `LIMIT can't be negative`.
* **`reply_outofrange`** was `ERR value is out of range`; redis names the bounds
  (`… value must between -9223372036854775807 and 9223372036854775807`). Its only callers are the
  `SRANDMEMBER`/`ZRANDMEMBER`/`HRANDFIELD` count checks, which is exactly where redis says that.
* **Geo.** `GEOADD k 1e100 0 m` answered with **raw bytes**:
  `-ERR invalid longitude,latitude pair X\xe7\x1b\xa6,iM\x92`. `std::to_chars(…, fixed, 6)` into a
  64-byte buffer returns `value_too_large` with `ptr == last` and leaves the buffer unspecified;
  appending `[begin, ptr)` shipped 64 bytes of uninitialised stack to the client. Buffer sized for
  the widest double (344) and the error checked. Separately, the shape scalars name themselves:
  `need numeric radius` / `need numeric width` / `need numeric height`.
* **`WAIT`**: the timeout is `timeout is not an integer or out of range`, and a timeout that would
  overflow the deadline is `timeout is out of range` (bound probed exactly: `LLONG_MAX - mstime()`).
* **`COPY … DB`** answers a typo as a typo, then redis's int-range message, then the database
  error. **`LCS MINMATCHLEN`** is a signed long long, not unsigned. **`SSCAN … NOVALUES`** says
  `NOVALUES option can only be used in HSCAN`. **`XREAD`/`XREADGROUP`** unbalanced-streams text is
  byte-exact (`'xread'` quoted and lower case, trailing period). **`XTRIM`/`XADD`** say
  `The MAXLEN argument must be >= 0.` / `The LIMIT argument must be >= 0.`; **`XSETID`** says
  `entries_added must be positive`.

### F5 — arity, option grammar and subcommand routing

* `PFADD key`, `GEOPOS key`, `GEOHASH key` are legal on redis (create the HLL and answer 1; answer
  an empty array). Their minimum arity was 3 — one too high. The handlers already loop from
  `argv[2]`, so only the registry rows were wrong.
* `HSET h f 1 g` answered `ERR wrong number of arguments`, dropping the `for 'hset' command` every
  client's error handling expects. The handler now spells the command the way the dispatcher does.
* **`EXPIRE` conditions compose.** The maximum arity was 4, so `EXPIRE k 100 XX GT` — a legal
  command — came back as an arity error. The flags are now a set, with redis's messages for the
  illegal combinations (`NX and XX, GT or LT …`, `GT and LT …`, `Unsupported option <word>`), and
  a key with no TTL counts as an infinite one so `GT` can never beat it and `XX LT` still refuses.
  A single enum could not express that: collapsing `XX LT` to `LT` let `EXPIREAT k <past> XX LT`
  delete a key that had no TTL at all.
* `BITCOUNT`'s maximum arity was 5, making one extra word an arity error instead of a syntax error.
* **`XGROUP`/`XINFO` enforced only the container arity**, so wrong-arity subcommands *executed*:
  `XGROUP DESTROY s g extra` returned `:0`, `XGROUP CREATECONSUMER s g` reached the group lookup.
  A per-arm table now answers `wrong number of arguments for 'xgroup|destroy' command`, and an
  unknown arm is rejected **before** the key lookup — `XINFO NOPE somekey` used to answer
  `ERR no such key`, which is a lie about which argument was wrong.
* **Unknown-command text.** `ERR unknown command` → redis's full form, byte-exact including the
  argument echo, the per-argument truncation at the remaining 128-byte budget, and the trailing
  space with no arguments at all. `tests/auth.py` asserted the old short string and is updated; the
  precedence it actually guards (unknown/arity ahead of `NOAUTH`) is unchanged and still holds.

### F6 — reply SHAPES

* `BRPOPLPUSH` and `BLMOVE` answered a timeout with `$-1`; redis answers with a null **array**,
  `*-1`. Invisible in RESP3, where both render as `_`.
* `DUMP` on a missing key answered `$-1` even on a RESP3 connection, where every other miss is `_`.
* `XRANGE`/`XREVRANGE` with `COUNT <= 0` is a null array on redis, not an error (`COUNT -1` was an
  integer error here). `COUNT 0` is *also* a null array, not an empty one — so it cannot serve as
  the negative control for that row.
* **`LIMIT` on an index range** is refused only when the COUNT actually bounds the answer. Redis
  tests the parsed count against its `-1` default and ignores the offset, so `ZRANGE k 0 -1 LIMIT
  0 -1` (and any offset with count `-1`) is accepted and the LIMIT is a no-op. Client libraries
  that always emit `LIMIT` depend on it. `ZRANGE k 0 -1 LIMIT 1 2` is still refused.

### F7 — double text: redis's `d2string`, not `std::to_chars`

`std::to_chars(double)` gives the shortest STRING; redis gives the shortest DIGITS, rendered by its
own fixed/scientific rule. They disagree far more often than the name suggests:

| value | before | oracle |
|---|---|---|
| `1e15` … `1e18` | `1e+15` … `1e+18` | `1000000000000000` … `1000000000000000000` |
| `2^63` | `9223372036854775808` | `9223372036854776000` |
| `1e-5` | `1e-05` | `0.00001` |
| `1e-7` | `1e-07` | `1e-7` |
| `1e23` | `1e+23` | `99999999999999990000000` |

Derived from observed output (never from redis source) and implemented once, for the RESP2 bulk
score and the RESP3 `,` alike, because redis renders both from one `d2string`:

* `nan`/`inf`/`-inf` and a signed zero are spelled out;
* an **integral** value with `|v| <= 4611686018427387904` (redis's `(double)(LLONG_MAX/2)`, probed
  inclusive) prints as a plain integer;
* otherwise the shortest round-trip digits `D` with `v = D × 10^K`, printed as a zero-padded
  integer when `K >= 0` and the decimal exponent is under `ndigits + 7`, as fixed point when
  `K > -7` or the decimal exponent is under 4, and scientific otherwise with an **unpadded** signed
  exponent.

**12 887 values (structured magnitudes 1e−320…1e308, 6000 random bit patterns, human-rounded
values, random integral doubles): 880 diffs before, 18 after, identical counts in RESP2 and RESP3.**
The 18 are S2 below.

## Shelved — reproduced, deliberately not fixed here

Each entry is a real divergence with a copy-pasteable reproducer. None is guarded by a gate row.

* **S1 — `container|subcommand` arity naming (48 probes).** Redis registers each subcommand as its
  own command, so a wrong count says `wrong number of arguments for 'acl|deluser' command`. This
  tree names the container, or worse says the arm is unknown when it is not:
  `SLOWLOG LEN x` → `unknown subcommand 'LEN'. Try SLOWLOG HELP.` (LEN is supported; the extra
  argument is the problem). Affects ACL(11) COMMAND(4) CONFIG(5) LATENCY(7) MEMORY(5) OBJECT(5)
  SLOWLOG(3) SCRIPT(1) XGROUP(2, the arms below the container minimum) XINFO(3). Mechanical but it
  is a table per container across five files, and CONFIG/ACL dispatch is load-bearing for the auth
  and ACL batteries — the wrong lane to touch on the way past.
* **S2 — grisu2 vs shortest round-trip, last digit (18 of 12 887).** Redis's `fpconv_dtoa` is
  grisu2, which is round-trip correct but not always shortest, so its digits differ from
  `to_chars`' in the last place or in count: `ZADD k INCR 1659272476871303.8 m` → `…303.8` here,
  `…303.7` there; `1e23` → `99999999999999990000000` on both (F7 fixed the rendering) but
  `1e126` → `1e+126` here, `9.999999999999999e+125` there. Matching would mean reimplementing one
  library's rounding artefacts; our digits are the shortest correct ones.
* **S3 — redis parses score ranges and weights with a bare `strtod` (≈38 probes).** No
  `isspace` guard, no `ERANGE` guard, and an empty string parses as 0:
  `ZCOUNT z "" +inf` counts everything ≥ 0 there and is `min or max is not a float` here;
  `ZRANGEBYSCORE z " 5" +inf`, `ZRANGEBYSCORE z 1e309 +inf` (→ `+inf`) and `ZADD z 0x10 m`
  (hex float → 16) are all accepted there. This tree is stricter, which is safer for the empty and
  overflow cases; matching would mean adopting `strtod`'s hex-float and whitespace acceptance.
  Documented as a deliberate divergence.
* **S4 — repeated options that redis tolerates.** `XADD s NOMKSTREAM NOMKSTREAM …`,
  `XREAD COUNT 1 COUNT 2 …`, `GETEX k PERSIST PERSIST`, `GEOSEARCH … COUNT 1 ANY ANY` all succeed
  on redis and are syntax errors here. Last-wins repetition is a per-command grammar change.
* **S5 — validation order inside two stream commands.** `XPENDING s g IDLE` is a syntax error on
  redis and NOGROUP here; `XCLAIM s g c 0 1-1 IDLE` is the mirror image (NOGROUP there, syntax
  error here). Redis validates the option list before the group for XPENDING and after it for
  XCLAIM; matching both means reordering two handlers.
* **S6 — `ACL GETUSER` RESP3 shape.** Redis returns `%6` with `~3` for `flags`; this tree returns a
  flat `*12`. RESP2 is identical.
* **S7 — `DEBUG PROTOCOL <type>` is not implemented.** It is redis's own RESP3 conformance probe
  (`map`/`set`/`double`/`bignum`/`verbatim`/`attrib`/`true`/`false`/`null`/`push`), and having it
  would make future RESP3 work testable. `DEBUG <unknown>` also names the container rather than the
  arm.
* **S8 — `LOLWUT` is a bulk string; redis sends a verbatim string (`=`) in RESP3.** The art itself
  is intentionally ours.
* **S9 — `SCAN` cursor strictness.** Redis's `string2ull` falls back to `strtoull`, so `SCAN +5`,
  `SCAN -0`, `SCAN <tab>5` and `SCAN 9223372036854775808` are accepted (and `SCAN -5`, `SCAN abc`
  and an overflowing cursor are not). This tree answers `invalid cursor` to the first four. Five
  parse sites across four files, and cursor semantics belong to the scan lane that just landed.
* **S10 — `EXEC x` outside MULTI.** Redis answers
  `-EXECABORT Transaction discarded because of: wrong number of arguments for 'exec' command`; this
  tree answers the plain arity error. A redis quirk in EXEC's own reject path; matching it means
  special-casing one command's arity error.
* **S11 — `LSET key 9223372036854775807 v` returns `+OK` on redis** and `index out of range` here.
  Redis truncates the index into a listpack `int`, so the value lands on the LAST element. That is
  a redis bug; not copied.
* **S12 — `SORT … BY pattern` and `SORT … GET pattern` are not implemented** (`ERR syntax error`).
  This is a feature gap, not an argument-surface defect, and BY/GET require reading keys the
  command does not name — it crosses the single-owner law and needs its own lane and design.
* **S13 — `COPY … DB n` for `n != 0`** is `DB index is out of range`: this server has one database.
  Architectural, and now reported after the two parse checks so a typo reads as a typo.
* **S14 — 12 commands absent** (`ASKING CLUSTER MIGRATE MODULE MOVE PFDEBUG PSYNC READONLY
  READWRITE RESTORE-ASKING SWAPDB SYNC`), 56 arity probes. Not this lane's scope; they now at least
  answer redis's exact unknown-command text.
* **S15 — pre-existing UBSAN report in the vendored Lua** (`third_party/lua/lstring.c:87`, misaligned
  4-byte load). Third-party, untouched, and unrelated to anything here.

## Tests

* **`tests/edgeproto.py HOST PORT`** — 379 checks, nine sections, every row an exact reply.
  Each section carries NEGATIVE CONTROLS (a canonical `LPOP l 2` must still pop two; a well-formed
  out-of-range expire must still get the range message; a real `LIMIT` must still be refused), so
  an over-tight fix fails the battery as loudly as an absent one. It passes unchanged against the
  7.4 oracle, so it is a parity battery and not a transcript of current behaviour.
* **`tests/differ.py … edgeproto [seed] [-3]`** — 5200 ops of error text, arity, option grammar,
  null shapes, binary keys/values and agreed-upon doubles, byte-compared against the oracle. It
  carries only DETERMINISTIC replies: random-member replies, wall-clock replies and everything in
  the shelved list are excluded on purpose, with the reason written into the generator, because a
  suite that carried them could never reach zero.

### Evidence

```
PRE  (base 4f4c88082, port 7422)
  edgeproto: 379 checks, 274 failures -> FAIL
  edgeproto: server is DOWN after the WATCH row; later runs need a fresh boot
  DIFFER edgeproto: 5200 ops, 2708 diffs -> FAIL

POST (this branch)
  edgeproto: 379 checks, 0 failures -> PASS          (target, --atomic 0)
  edgeproto: 379 checks, 0 failures -> PASS          (target, --atomic 1)
  edgeproto: 379 checks, 0 failures -> PASS          (ASAN+UBSAN build)
  edgeproto: 379 checks, 0 failures -> PASS          (redis 7.4.2 oracle itself)

  DIFFER edgeproto: 5200 ops, 0 diffs -> PASS   seeds 7 11 23 101, RESP2 and RESP3 (8 runs)
  DIFFER edgeproto: 5200 ops, 0 diffs -> PASS   --atomic 1, seeds 7 11 23, RESP2 and RESP3
  DIFFER edgeproto: 5200 ops, 0 diffs -> PASS   ASAN build, seeds 7 and 11

REGRESSION (existing suites, unchanged, vs the same oracle)
  string 4033/0  list 3521/0  set 3524/0  zset 3531/0  hash 3545/0  hexpire 4288/0
  bitmap 4262/0  hll 3057/0   bitfield 3226/0  stream 4031/0  streamgrp 4052/0
  zsetops 4200/0 geo 4200/0   xshard 4276/0  cgaps 3310/0  scan 4081/0  multi 4137/0
  servertail 5339/0  script 4936/0                       (ops/diffs, all PASS)
  --atomic 1: string list zset stream geo multi xshard   all 0 diffs

  In-tree directed batteries, each on a fresh boot: auth acl bitfield blocking concur debug
  dumprestore execatomic execiso geo hexpire lcs limits multi_exec notify pubsub resp3 ryow
  scriptsurf slowlog stream streamgroups tracking zsetops  -- all PASS.

ASAN + UBSAN (make asan, port 7421): battery, edgeproto differ, all six probe corpora and the
zset/geo/stream/list differ suites -- zero AddressSanitizer reports, zero runtime errors. One UB
found on the way and fixed: negating the cast of 2^63 in three copies of the WAIT/SLOWLOG/CONFIG
integer parser (`WAIT -9223372036854775808 0`).
```

## Instruments (in the lane scratchpad, not committed)

The discovery engine was a corpus prober rather than the differ: it sends each case to both
servers with an `ECHO` sentinel interleaved, so a command that answers twice — or not at all — is
caught as a desync instead of silently shifting every later comparison. That sentinel is what found
F2. The six corpora are arity (driven by the oracle's own `COMMAND` table), numeric edges, option
grammar, null shapes, binary/empty inputs, and validation order/duplicates.

Two harness traps worth recording:

* **`socket.close()` with a live `makefile` does not close the connection.** It only drops an
  io-ref; the peer sees the disconnect when the last reference dies. Every early attempt to
  reproduce F1 "failed" because the server never ran its connection-close path. `tests/edgeproto.py`
  closes both, with a comment saying why.
* **`subprocess.run(capture_output=True)` hangs on a daemonising launcher** — the pipe stays open
  as long as any descendant holds it. Redirect to a file.

# NOTES-EDGETIME — hunting errors where expiry meets everything else

Lane `t-edgetime`. Oracle: vanilla redis 7.4.2 at `/tmp/claude-1000/redis74/src/redis-server`.
Every verdict below came from running both servers side by side; nothing here was filed on reasoning
alone, and everything fixed is proved failing-before / passing-after in the transcripts at the end.

Resources used: cores 32-47 only, ports 7410-7416 only, every server `taskset`ed and stopped by the
pid resolved from its listening socket.

## Hypothesis table

| # | Hypothesis (from the brief) | Verdict | Where |
|---|---|---|---|
| 1 | Expiry during a multi-key/cross-shard command (MGET/MSET/DEL/RENAME/SINTERSTORE), both atomic modes | **NOT REPRODUCED** in the ordinary case: 4426-op differ suite, 4 seeds, both atomic modes, RESP3, 0 diffs | `tests/differ.py edgetime`, `tests/edgetime.py::test_multikey_ttl` |
| 1b | …but a deadline that falls *inside* a widened cross-shard fan-out tears the reply | **REPRODUCED — SHELVED** (needs the tree's own `DEBUG ATOMIC-FANOUT-DEFER`; 0/200 without it) | `tests/edgetime.py … repro-hop` |
| 2 | Expiry inside MULTI/EXEC; key expiring between queueing and EXEC | **NOT REPRODUCED** | `tests/edgetime.py::test_multi_watch` |
| 2b | **WATCH on a key that expires** | **REPRODUCED — FIXED** | `src/cmd/multi.inc`, `src/core/shard.h`, `src/store/flatstore.h` |
| 3 | Lazy vs active expiry: DBSIZE / KEYS / SCAN / TYPE / OBJECT / TTL / PERSIST / DUMP on an expired-unreaped key | **NOT REPRODUCED** (26-command matrix vs oracle) | `tests/edgetime.py::test_lazy_and_active` |
| 3b | **MEMORY USAGE** on an expired-unreaped key | **REPRODUCED — FIXED** | `src/cmd/server_tail.cc`, `src/store/flatstore.h` |
| 3c | **RANDOMKEY** returns nil while live keys exist, once expired-unreaped keys inflate an owner's published count | **REPRODUCED — SHELVED** (184/200 in production mode; oracle 0/200) | `tests/edgetime.py … repro-randomkey` |
| 4 | Expiry and persistence: snapshot SAVE + reload, direct AOF, rewritten AOF, `DEBUG RELOAD` | **NOT REPRODUCED** | `tests/edgetime_persist.sh`, `tests/edgetime.py persistbuild/persistcheck` |
| 5 | Hash-field TTLs vs whole-key TTL, field count reaching zero, snapshot round-trip | **NOT REPRODUCED** (one *documented* divergence found, see below) | `tests/edgetime.py::test_hash_whole_key`, differ `edgetime` suite |
| 6 | TTL arithmetic edges: 0, negative, past, very large, EXPIRE NX/XX/GT/LT, KEEPTTL, PERSIST, GETEX variants | **REPRODUCED — FIXED** (largest finding of the lane) | `src/cmd/t_string.cc` |
| 7 | Eviction interacting with TTL accounting under maxmemory | **NOT REPRODUCED** | measured, see below |
| 8 | (found while testing #3b) **OBJECT ENCODING/REFCOUNT overtakes an older cross-shard group of its own connection** | **REPRODUCED — FIXED** | `src/cmd/atomics_glue.inc` |

---

## REPRODUCED and FIXED

### F1. EXPIRE/PEXPIRE/EXPIREAT/PEXPIREAT rejected every option list longer than one

`src/cmd/t_string.cc`. The four registry rows were `min 3, max 4`, and the parser read exactly
`argv[3]`. Redis's option list is variadic: flags fold, so `NX NX` is a plain `NX`, and `XX` may be
combined with `GT` or `LT`. Only `NX`+{`XX`,`GT`,`LT`} and `GT`+`LT` are refused, and only after the
whole list scans clean — an unrecognised token outranks both refusals and outranks the deadline
parse.

```
$ redis-cli -p 7415 EXPIRE k 100 NX NX      # oracle
(integer) 1
$ redis-cli -p 7410 EXPIRE k 100 NX NX      # tomokv, before
(error) ERR wrong number of arguments for 'expire' command
$ redis-cli -p 7415 EXPIRE k 100 BOGUS      # oracle
(error) ERR Unsupported option BOGUS
$ redis-cli -p 7410 EXPIRE k 100 BOGUS      # tomokv, before
(error) ERR syntax error
```

Fixed by a flag-set parser (`ExpireConditions` + `parse_expire_conditions`) and `max` widened to
`-1`. The rejected token is echoed the way redis echoes it — as a C string, so it stops at the first
NUL, with CR and LF folded to spaces. That last part is not cosmetic: a naive echo of the raw
argument would let `EXPIRE k 100 "x\r\n+PWNED"` inject a second reply line into the connection. The
battery asserts the NUL, LF and CRLF forms byte-for-byte.

### F2. A non-numeric expire argument reported the wrong error, across seven commands

`src/cmd/t_string.cc`. Redis distinguishes *"that is not a number"* from *"that number cannot be a
deadline"*; TomoKV answered the second for both. `expiry_at()` now returns a three-state
`ExpireArg`, and `apply_expiry_arg()` is the shared tail.

| command | oracle | tomokv before |
|---|---|---|
| `EXPIRE k abc` | `ERR value is not an integer or out of range` | `ERR invalid expire time in 'expire' command` |
| `SET k v EX abc` | `ERR value is not an integer or out of range` | `ERR invalid expire time in 'set' command` |
| `GETEX k EX abc` | `ERR value is not an integer or out of range` | `ERR invalid expire time in 'getex' command` |
| `SETEX k abc v` | `ERR value is not an integer or out of range` | `ERR invalid expire time in 'setex' command` |

The overflow forms (`EXPIRE k 9223372036854775807`) keep `invalid expire time`, which is what the
oracle does; the battery pins both halves so the change cannot slide the other way.

### F3. GETEX took a fixed arity where redis takes SET's extended-argument grammar

`src/cmd/t_string.cc`. `GETEX` shares `SET`'s option parser in redis: one form may repeat and the
last value wins, two different forms are a syntax error, and `PERSIST` may not be mixed with a
deadline. TomoKV's `argc == 4` gate answered *wrong number of arguments* for `GETEX k EX 1 EX 2`
(redis: OK, TTL 2) and for `GETEX k EX 10 PERSIST` (redis: syntax error). Replaced by the same loop
shape `parse_set_options` already uses, and `max` widened to `-1`.

### F4. WATCH did not see a watched key expire

`src/cmd/multi.inc`, `src/core/shard.h`, `src/store/flatstore.h`. A watch is marked dirty by
`watch_write_committed()`, which is only reached from a *command* that writes the key. Expiry is the
one mutation with no command behind it, so a watched key that elapsed left the WATCH clean and EXEC
ran anyway.

Redis aborts in that case whether its active cycle has reaped the key or not, and — importantly —
does **not** abort when the key was *already* past its deadline at WATCH time (`wk->expired`).
Measured on both servers, active expiry on and off:

```
                                             EXEC
oracle  alive-at-watch  active=1 dbsize=0    *-1          (aborted)
oracle  pre-expired     active=1 dbsize=0    *1 $-1       (ran)
oracle  alive-at-watch  active=0 dbsize=1    *-1          (aborted)
oracle  pre-expired     active=0 dbsize=1    *1 $-1       (ran)
tomo    alive-at-watch  active=1 dbsize=0    *1 $-1       (ran)  <-- wrong
tomo    pre-expired     active=1 dbsize=0    *1 $-1       (ran)
tomo    alive-at-watch  active=0 dbsize=1    *1 $-1       (ran)  <-- wrong
tomo    pre-expired     active=0 dbsize=1    *1 $-1       (ran)
```

`WatchEntry` now carries the deadline the key held when the WATCH armed, captured through a new
`FlatStore::watch_deadline()` that is deliberately **non-reaping and non-touching** — redis's WATCH
probes with `LOOKUP_NOTOUCH` and leaves an elapsed-unreaped key physically counted, and the battery
asserts `DBSIZE` and the `expired_keys` counter are both unmoved by arming a WATCH. A key already
past its deadline captures `-1`, which reproduces `wk->expired` exactly. `watch_validate_and_reserve`
marks the client dirty when an armed deadline has elapsed.

`WatchEntry` grows 16 -> 24 bytes. It lives in a per-shard heap map that stays empty until the first
WATCH; `Op` and `Client` are untouched and their `static_assert`s are unchanged.

All fourteen cells now match the oracle, including the two negative controls that stop the fix from
passing for the trivial reason "any WATCH on a TTL key aborts": a live deadline must not abort, and a
key that was already elapsed at WATCH time must not abort.

### F5. MEMORY USAGE hid an expired-but-unreaped key

`src/cmd/server_tail.cc`, `src/store/flatstore.h`. `MEMORY USAGE` went through `find_no_touch()`,
which performs lazy expiry. Redis reads `USAGE` straight out of the dictionary with no expire check,
so the still-resident bytes are still reported:

```
DIFF MEMORY USAGE dead   tomo=$-1   oracle=:56          (active expiry off, key past deadline)
```

New `FlatStore::find_resident()` — the same physical two-table lookup `find_no_touch` already did,
minus the reap. Nothing else changes: `OBJECT`, `TYPE`, `TTL`, `EXISTS`, `GET`, `DUMP` all still hide
the key, which is also what redis does (verified across a 26-command matrix). `OBJECT IDLETIME`/`FREQ`
keep `find_no_touch`, because redis's `OBJECT` path *does* expire. The values themselves stay
TomoKV's own accounting (documented in NOTES-SERVERTAIL.md); only hit-vs-miss is comparable, and
that is exactly what the differ normalizes.

### F6. OBJECT ENCODING/REFCOUNT overtook an older cross-shard group of its own connection

`src/cmd/atomics_glue.inc`. Found while investigating F5, and the sharper defect of the two.

`Op::key()` is hard-wired to `argv[1]`. The registry has carried the real key position since OBJECT
ENCODING arrived, and the IO router already hashes `arg(spec->first_key)` into `op.hash`
(`io_loop.h`, and `command_prepare_subcmd_route` for the container subcommands). But the atomics
program-order and MVCC bookkeeping paired `op.hash` (the real key) with `op.key()` (the literal
`"ENCODING"`/`"USAGE"`). No record ever matched that pair, so:

* `has_atomic_deferred_predecessor` never saw the op's own undecided group and let it run early, and
* `xshard_plain_prepare` never pinned a read cut for it.

One connection, one pipeline, no debug hooks, `--atomic 1`, four keys on four different shards:

```
create round 0 [b'+OK\r\n', b':44\r\n', b'$-1\r\n',            b'$7\r\nvalue-c\r\n']
                MSET       MEMORY USAGE  OBJECT ENCODING <-- nil for a key its own MSET just made
delete round 0 [b':4\r\n', b'$-1\r\n',   b'$6\r\nembstr\r\n',  b'$-1\r\n']
                DEL        MEMORY USAGE  OBJECT ENCODING <-- alive after its own DEL
```

Rate without any window widener, 100 rounds per cell: `OBJECT ENCODING` missed its own creation
89/100 and resurrected its own DEL 92/100; `OBJECT REFCOUNT` the same; `GET` (whose key really is
`argv[1]`) 0/100 in both directions, which is the control.

Fixed with `op_routed_key(op)` — `arg(spec->first_key)` when the registry names one, `op.key()`
otherwise — used at the five places that pair a key with `op.hash`. For every ordinary single-key row
(`first_key == 1`) this *is* `argv[1]` and nothing changes; the only rows it moves are the ones the
router already routes differently. This does not touch the MVCC resolver, the scatter core or the
single-owner law: it corrects which key the existing lookups are asked about.

After: 0/200 in all four cells, and the battery row runs 24 rounds behind
`DEBUG ATOMIC-COMMIT-DELAY 2000` so it is a verdict rather than a coin flip (5-16 failures out of 24
on the unfixed tree, 0 on the fixed one, four runs each).

---

## REPRODUCED and SHELVED

### S1. A cross-shard read has a pinned commit cut but no pinned TIME cut

`tests/edgetime.py 127.0.0.1 <port> repro-hop`.

Under `--atomic 1` a cross-shard MGET pins a read cut so it cannot straddle a *commit*. It cannot
pin a *deadline*, because expiry is time-driven and has no commit ticket. Widen the fan-out with the
tree's own `DEBUG ATOMIC-FANOUT-DEFER` and give eight keys on eight owners one shared deadline inside
that window, and the lead fragment answers from before the deadline while the parked fragments answer
from after it:

```
geometry: 8 distinct shards [1, 2, 4, 5, 6, 8, 10, 14]
control (fan-out widened 500000us, deadline 1h out): present=8/8 elapsed=0.500s
armed   (fan-out widened 500000us, deadline inside): present=1/8 elapsed=0.500s
        reply=[None, None, b'v', None, None, None, None, None]
natural (no hook, deadline == now): torn 0/200
```

The control is the load-bearing part: the SAME widened fan-out over keys whose deadline is an hour
away shows no tear at all, so the tear is attributable to the deadline crossing and not to the hook.
The `natural` line bounds the practical exposure — with the real (microsecond) fan-out, a deadline
placed exactly at `now` did not tear once in 200 attempts.

Shelved rather than fixed: a fix means giving a scatter read a pinned wall-clock cut that every
fragment evaluates deadlines against, which is a change to the scatter engine core and to what
`live_or_expire` is allowed to do on a fragment's behalf — a lane of its own, not a small local edit.

### S2. RANDOMKEY answers nil while the keyspace is not empty

`tests/edgetime.py 127.0.0.1 <port> repro-randomkey`. Runs unmodified against the oracle too.

The IO thread picks ONE owner for `RANDOMKEY` and prefers an owner whose *published* key count is
nonzero (`io_loop.h`, `CmdFlags::RandomShard`; the design is described in NOTES-COMPAT.md). That
count includes keys past their deadline that nothing has reaped yet, so an owner holding nothing but
elapsed keys still looks populated. `random_live()` then sweeps it, reaps them, and reports nothing —
and the client gets nil even though live keys sit on other owners. Redis has one keyspace and retries
until it finds a live key.

This is **production behaviour, not a debug-hook artifact** — active expiry left ON:

```
tomokv (fixed tree)   control (40 TTL-free companions): nil   0/200
                      armed   (40 elapsed  companions): nil 184/200
redis 7.4.2 oracle    control (40 TTL-free companions): nil   0/200
                      armed   (40 elapsed  companions): nil   0/200
```

Shelved rather than fixed. The three candidate fixes and why none is a small local edit:

1. Retry on another owner from the executor — violates the single-owner law.
2. Have IO re-dispatch when the owner answers nil, excluding owners already tried — needs a retry
   hook in the io_loop retire path, which is the hot path and would have to be free when unused.
3. Lower RANDOMKEY to a whole-owner scatter like KEYS — correct, reuses existing machinery, but turns
   an O(1) command into an O(shards) fan-out and reverses a design NOTES-COMPAT.md states explicitly.

Option 3 is the one worth costing out, in a lane that can benchmark the result.

### S3. MEMORY USAGE can see an uncommitted cross-shard group from another connection

Not fixed, and deliberately not made worse. `MEMORY USAGE` reads the physical slot (that is true
before and after F5 — `find_no_touch` was already `find_in`-based). Under `--atomic 1` with
`DEBUG ATOMIC-COMMIT-DELAY 5000`, connection B saw the parked candidate of connection A's still
undecided cross-shard `MSET` 120/120 times, while `GET` and `OBJECT ENCODING` on the same key at the
same moment saw nothing 0/120 — so the group really was invisible, and only `MEMORY USAGE` leaked it.

Shelved because the fix is to route `MEMORY USAGE` through the MVCC resolver, which then has to
answer "what does an uncommitted version cost" — a question the resolver is not currently asked, and
one that collides with the redis-parity answer for the expired case in F5. Low severity: an admin
introspection command reporting a byte count for a key whose creation has not landed yet.

---

## NOT REPRODUCED (recorded so nobody re-runs them)

* **Cross-shard commands over elapsed keys.** MGET/MSET/DEL/EXISTS/RENAME/COPY/SINTERSTORE with
  mixed elapsed and live keys: 4426 ops × 4 seeds × {`--atomic 0`, `--atomic 1`} × {RESP2, RESP3},
  0 diffs against the oracle. Directed: an 8-owner MGET over 8 elapsed keys hides all 8 and fires the
  `expired_keys` counter exactly 8 times; the same shape with no deadlines moves it 0.
* **MULTI/EXEC.** Key expiring between queueing and EXEC, and a key expiring *inside* EXEC
  (`PEXPIRE k 0` then `GET k` in one transaction) both match the oracle.
* **Lazy vs active expiry across the introspection surface.** 26 commands against an
  expired-but-unreaped key with active expiry disabled: DBSIZE, KEYS, TYPE, OBJECT ENCODING/REFCOUNT,
  TTL, PTTL, EXPIRETIME, PEXPIRETIME, PERSIST, EXISTS, GET, STRLEN, APPEND, GETDEL, RENAME, COPY,
  EXPIRE, SETRANGE, GETRANGE, TOUCH, DUMP all agree byte-for-byte. Only MEMORY USAGE (F5) and
  RANDOMKEY (S2) diverged.
  * `SCAN` "diverges" for one call and does not for a full cursor cycle: TomoKV's `COUNT` is a
    slot-work hint, so one call returns fewer keys and a non-zero cursor where redis finishes in one
    round. A complete walk returns the identical set. Documented in NOTES-COMPAT.md; not a defect.
* **Expiry and persistence.** `tests/edgetime_persist.sh` builds a state with a live absolute
  deadline, an elapsed-but-unreaped key and a hash-field deadline under a whole-key `PERSIST`, waits
  past the elapsed key's deadline, and recovers it three ways — SAVE + `--load`, direct AOF replay,
  and AOF replay after a `BGREWRITEAOF` whose completion is asserted from
  `aof_rewrite_requests`/`completions`/`in_progress`. 6 checks × 3 recoveries, PASS on the *unfixed*
  tree as well: nothing resurrects, and the field deadline survives the round trip. `DEBUG RELOAD`
  with the same state also matches the oracle exactly. This is worth keeping precisely because the
  sibling gate lane found the hexpire snapshot round-trip had never actually executed.
* **Hash-field TTLs.** Whole-key deadline vs field deadline, whole-key `PERSIST` leaving field
  deadlines intact, whole-key expiry dominating a live field, the last field expiring taking the hash
  with it, elapsed fields hidden from HGET/HLEN/HGETALL/HSCAN/HRANDFIELD/HSTRLEN, HPERSIST and
  HPEXPIRETIME on an elapsed field, and a rewrite clearing a field deadline — all match the oracle.
* **Eviction × expiry accounting.** With `--maxmemory` and `volatile-lru`, 60000 short-TTL keys:
  `expired_keys` 60000, `evicted_keys` 0, `used_memory` back to 0, `DBSIZE` 0 — no double counting.
  With the limit tightened so eviction genuinely fires alongside expiry: 60000 writes, 25924 refused
  with OOM, and `live 17040 + expired 287 + evicted 16749 = 34076` = exactly the accepted count. The
  evictor itself was out of scope per the brief and was not touched.

## Divergences that are documented, and therefore not filed

* **`OBJECT ENCODING` on a hash with field deadlines** reports `listpack` where redis reports
  `listpackex` (and redis keeps `listpackex` after `HPERSIST`). NOTES-HEXPIRE.md states this
  explicitly: *"`OBJECT ENCODING` reports TomoKV's own compact/hashtable for hashes and always has;
  it does not grow a `listpackex` value."* Left alone.
* **`HGETEX` / `HGETDEL`** are redis 8.0 and absent from both servers; only the *unknown command*
  message shape differs, which is a global TomoKV property, not an expiry one.
* **`DUMP` payload bytes** are an intentionally different encoding (see the wiredump section of
  `tests/differ.py`), so the differ generator compares `DUMP` only through the directed battery's
  nil-vs-non-nil check and keeps it out of the byte-compared stream.
* **`MEMORY USAGE` values** are TomoKV's accounting, normalized to hit-vs-miss (NOTES-SERVERTAIL.md).

---

## What is committed

| file | what |
|---|---|
| `src/cmd/t_string.cc` | F1 variadic EXPIRE options + sanitized `Unsupported option`; F2 `ExpireArg` three-state parse; F3 GETEX option loop; four EXPIRE rows and GETEX widened to `max -1` |
| `src/cmd/multi.inc` | F4 capture the armed deadline in `watch_add`, mark dirty in `watch_validate_and_reserve` |
| `src/core/shard.h` | F4 `WatchEntry::expire_at_ms` |
| `src/store/flatstore.h` | F4 `watch_deadline()`; F5 `find_resident()` |
| `src/cmd/server_tail.cc` | F5 `MEMORY USAGE` reads residency |
| `src/cmd/atomics_glue.inc` | F6 `op_routed_key()` at the five key/hash pairings |
| `tests/edgetime.py` | 155-check directed battery + `repro-hop`, `repro-randomkey`, persist halves |
| `tests/edgetime_persist.sh` | snapshot / direct-AOF / rewritten-AOF recovery driver |
| `tests/differ.py` | new `edgetime` generator (4426 ops) |

No new knobs, so `tomokv.conf` is unchanged. No new source files, so the Makefile is unchanged.

## Test evidence

### Battery, unfixed tree (baseline binary built from `HEAD` with the lane's diff reverted)

```
$ python3 tests/edgetime.py 127.0.0.1 7413
  FAIL EXPIRE duplicate NX: got RespError("ERR wrong number of arguments for 'expire' command"), want 1
  FAIL PEXPIREAT XX GT: got RespError("ERR wrong number of arguments for 'pexpireat' command"), want 1
  FAIL incompatible NX XX: got b"-ERR wrong number of arguments for 'pexpireat' command\r\n", want b'-ERR NX and XX, GT or LT options at the same time are not compatible\r\n'
  FAIL unknown option preserves case: got b"-ERR wrong number of arguments for 'pexpireat' command\r\n", want b'-ERR Unsupported option bOgUs\r\n'
  FAIL unknown option folds CRLF: got b'-ERR syntax error\r\n', want b'-ERR Unsupported option ba  +PWNED\r\n'
  FAIL EXPIRE non-numeric: got b"-ERR invalid expire time in 'expire' command\r\n", want b'-ERR value is not an integer or out of range\r\n'
  FAIL GETEX repeated EX: got RespError("ERR wrong number of arguments for 'getex' command"), want b'v'
  FAIL MEMORY USAGE reports the resident expired key: got None
  FAIL MEMORY USAGE does not reap: got 1, want 0
  FAIL WATCH expiry aborts EXEC: got [None], want None
  FAIL WATCH abort did not rely on a lazy reap: got 1, want 0
  FAIL cross-shard WATCH aborts on one elapsed key: got [b'v'], want None
  FAIL OBJECT ENCODING never misses its own cross-shard creation (24 rounds): got 5, want 0
  FAIL OBJECT ENCODING never resurrects its own cross-shard DEL (24 rounds): got 7, want 0
  ... (45 failures in total; the elided rows are the rest of the option matrix and the
       not-an-integer family, all listed under F1/F2/F3)
edgetime: 155 checks, 45 failures -> FAIL
```

Stable: 4 consecutive runs, 45 failures each.

### Battery, fixed tree

```
$ python3 tests/edgetime.py 127.0.0.1 7410      # --atomic 1
edgetime: 155 checks, 0 failures -> PASS
$ python3 tests/edgetime.py 127.0.0.1 7414      # --atomic 0
edgetime: 155 checks, 0 failures -> PASS
```

Stable: 4 consecutive runs each.

### Differ, `edgetime` suite (target vs redis 7.4.2)

```
unfixed tree   DIFFER edgetime: 4426 ops, 323 diffs -> FAIL
fixed --atomic 1, seed 7     DIFFER edgetime: 4426 ops, 0 diffs -> PASS
fixed --atomic 1, seed 11    DIFFER edgetime: 4426 ops, 0 diffs -> PASS
fixed --atomic 1, seed 23    DIFFER edgetime: 4426 ops, 0 diffs -> PASS
fixed --atomic 1, seed 101   DIFFER edgetime: 4426 ops, 0 diffs -> PASS
fixed --atomic 1, seed 7 -3  DIFFER edgetime: 4426 ops, 0 diffs -> PASS
fixed --atomic 0, seed 7     DIFFER edgetime: 4426 ops, 0 diffs -> PASS
fixed --atomic 0, seed 11    DIFFER edgetime: 4426 ops, 0 diffs -> PASS
fixed --atomic 0, seed 23    DIFFER edgetime: 4426 ops, 0 diffs -> PASS
```

The suite runs with `DEBUG SET-ACTIVE-EXPIRE 0` and absolute deadlines only, so every removal is
driven by a command in the diffed stream and both servers reap the same keys at the same point.

### Regression: existing suites and batteries on the fixed tree

```
DIFFER string:     4033 ops, 0 diffs -> PASS      multi_exec        MULTI/WATCH directed battery passed
DIFFER hash:       3545 ops, 0 diffs -> PASS      ryow              RYOW PASS
DIFFER hexpire:    4288 ops, 0 diffs -> PASS      atomic_ryow       ATOMIC_RYOW PASS
DIFFER multi:      4166 ops, 0 diffs -> PASS      hexpire           206 checks, 0 failures -> PASS
DIFFER xshard:     4276 ops, 0 diffs -> PASS      servertail        101 checks, 0 failures -> PASS
DIFFER scan:       4081 ops, 0 diffs -> PASS      limits            PASS
DIFFER set:        3524 ops, 0 diffs -> PASS      debug             PASS
DIFFER servertail: 5339 ops, 1 diffs -> FAIL*     execiso           in-EXEC isolation battery passed
                                                  execatomic        EXEC fan-out battery passed
                                                  atomfix / concur / scriptatomic / session_monotonic  PASS
```

\* Pre-existing and unrelated: the single diff is `ROLE` reporting the oracle's master replication
offset (`:46`) against TomoKV's `:0`. Reproduced identically with the **unfixed** baseline binary on
a freshly booted oracle, so it is not from this lane.

`tests/atomic_torn.py` failed intermittently on its `OFF control exposes torn MSET-8` row — the
*control*, which must observe tearing with atomics disabled. It flakes on a 10-core layout and not on
a 6-core one, and it flakes identically on the **unfixed** baseline pinned to the same 10 cores
(1 failure in 4 runs there, 2 in 3 on the fixed tree, and 0 in 3 for both on 6 cores). Load-dependent
vacuous-control flake in that battery, not a regression from this lane.

### ASAN

Built with the `tests/gate.sh` section-1 line
(`g++ -std=c++20 -O1 -g -fsanitize=address -march=native -pthread -I. src/main.cc src/net/tls.cc
src/cmd/*.cc src/snapshot/*.cc src/persist/*.cc -o … -luring -pthread -lssl -lcrypto`), `ldd`-checked
to confirm `libasan.so.8` is linked into the binary that actually ran:

```
--atomic 1   edgetime battery   155 checks, 0 failures -> PASS
--atomic 1   DIFFER edgetime    4426 ops, 0 diffs -> PASS
--atomic 1   multi_exec / hexpire / servertail / execiso   PASS
--atomic 0   edgetime battery   155 checks, 0 failures -> PASS
--atomic 0   DIFFER edgetime    4426 ops, 0 diffs -> PASS
AddressSanitizer / UBSan reports in the server logs: 0
```

### Persistence

```
$ TMPDIR=<scratch> bash tests/edgetime_persist.sh ./build/tomokv 7412 42-47
== snapshot recovery ==        edgetime persist: 6 checks, 0 failures -> PASS
== direct AOF recovery ==      edgetime persist: 6 checks, 0 failures -> PASS
== rewritten AOF recovery ==   rewrite fired: requests=1 completions=1
                               edgetime persist: 6 checks, 0 failures -> PASS
edgetime persistence: PASS
```

Also PASS on the unfixed baseline — this hypothesis came back empty and the script is coverage, not a
fix guard.

## Note on the previous attempt's draft

An earlier, unfinished attempt left uncommitted changes in this worktree. Every claim in it was
re-verified against the oracle before anything was kept.

**Kept, after re-deriving it:** the variadic EXPIRE option parser (its shape was right; its
`Unsupported option` echo was not sanitized, so `EXPIRE k 100 "x\r\n+PWNED"` would have injected a
reply line — the committed version folds CR/LF the way redis does, and the battery pins it); the
WATCH deadline capture (its shape was right; it probed with `store_.find()`, which *reaps*, so
arming a WATCH would have removed an elapsed-unreaped key that redis leaves counted — the committed
version uses a non-reaping `watch_deadline()`); the `Resp` harness and several directed cases in
`tests/edgetime.py`; the persistence driver; the differ generator's absolute-deadline discipline.

**Discarded:**

* `op.mark_local_xshard()` in `command_prepare_subcmd_route` — the right defect (F6), the wrong fix.
  It routes the op through `for_each_touched_key`, but `classify()` does not know MEMORY/OBJECT, so
  that path falls back to `fn(op.hash, op.key())` — `argv[1]` again. It would not have fixed
  anything, while switching the op's zero-copy marker and its local-xshard AOF emission as a side
  effect. Replaced with `op_routed_key()`.
* The MVCC-resolving `find_no_touch()` — it was aimed at a defect whose direction was backwards.
  `MEMORY USAGE` (which used `find_no_touch`) was the form that answered *correctly* in the
  reproducer; `OBJECT ENCODING` (which uses the logical `store_find`) was the form that tore. The
  real cause was the routed-key mismatch above. What remains of that area is written up as S3.
* The battery's `MEMORY USAGE` expectation of `nil` for an expired key — the oracle returns a size.
* The draft's `repro-hop` control, which re-used the *unwidened* fan-out and so proved nothing about
  the hook. The committed control runs the same widened fan-out with a far-future deadline and must
  show no tear, and a `natural` arm bounds the exposure without any hook.
* `tests/__pycache__/`.

The draft's `repro-hop` claim itself was checked before being kept: it does reproduce (S1).

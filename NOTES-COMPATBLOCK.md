# NOTES-COMPATBLOCK — blocking and pub/sub differential coverage

## Outcome

This lane adds discoverable `blocking` and `pubsub` generators to `tests/differ.py`.  Each generator
owns the multiple connections its protocol needs and byte-compares replies and delivery frames with
the pinned vanilla Redis 7.4.2 binary.  The six release cells (atomic 0/1 × seeds 7/19/41) each ran
more than 4,000 logical commands per generator.

Two small local differences were resolved:

- Expired `BLMOVE` and `BRPOPLPUSH` now use Redis 7.4's RESP2 null-array frame (`*-1`), while RESP3
  continues to use its protocol-wide null (`_`).
- RESP2 subscriber-mode errors for `PUBSUB` now use the resolved subcommand name, and unknown or
  fixed-arity-invalid subcommands reach their Redis-compatible validation errors.

No command, knob, allocation, or data-path structure was added.  `sizeof(Op)==336` and
`sizeof(Client)==1984` remain protected by the existing build assertions.  The two production edits
are cold: blocking timeout completion and RESP2 subscriber-mode rejection.  GET/SET dispatch is
unchanged.

## Differential instrument

`blocking` covers `BLPOP`, `BRPOP`, `BLMOVE`, `BRPOPLPUSH`, `BLMPOP`, `BZPOPMIN`, `BZPOPMAX`,
`BZMPOP`, and `WAIT`.  Its randomized section makes every collection operation immediately ready,
so timeout syntax, argument priority, edges, counts, and reply shapes remain deterministic.  The
directed tail adds fractional expiration, timeout-zero silence followed by a wake, six FIFO waiters,
disconnect cancellation with a surviving-value control, and MULTI.  Every seed reports a positive
`multi_ready_cases` count; eight timeout-zero arms and six FIFO waiters fire on every run.

`pubsub` covers all six subscription controls, `PUBLISH`, `SPUBLISH`, and `PUBSUB CHANNELS`,
`NUMSUB`, `NUMPAT`, `SHARDCHANNELS`, and `SHARDNUMSUB`.  It uses mixed RESP2/RESP3 subscribers and a
Python membership model for exact frame counts.  It checks empty unsubscribe confirmations,
regular-versus-shard count namespaces, overlapping patterns, exact+pattern delivery, RESP2/RESP3
subscriber-mode permissions, 128 ordered messages to one subscriber, and 4,000 randomized
publish/churn/introspection commands.  Unordered channel listings, all-unsubscribe iteration, and
overlapping-pattern delivery are canonicalized only where Redis does not promise iteration order.
A final sentinel proves no prior frame was under-read.

`python3 tests/differ.py --list-generators` now includes both `blocking` and `pubsub`.

## Compatibility table

| Area checked | Result | Oracle comparison |
|---|---|---|
| BLPOP/BRPOP immediate left/right pop, fractional timeout, timeout 0 | CONSISTENT | Exact arrays, null arrays, and wake replies match. |
| Several ready list keys and argument-order priority | CONSISTENT | Hundreds of randomized multi-ready cases per seed byte-match Redis. |
| FIFO serving of six waiters and push-after-park | CONSISTENT | Each client receives `v0` through `v5` in registration order. |
| Disconnect during a forever BLPOP | CONSISTENT | Both blocked-client gauges drain; the later pushed value survives for a new BLPOP. |
| BLMOVE ready/wake behavior | CONSISTENT | Ready value, destination effects, timeout-zero wake, and MULTI execution match. |
| BLMOVE finite timeout reply | DIFFERS, RESOLVED | TomoKV used `$-1\r\n`; Redis uses `*-1\r\n`. TomoKV now matches. |
| BRPOPLPUSH finite timeout reply | DIFFERS, RESOLVED | TomoKV used `$-1\r\n`; Redis uses `*-1\r\n`. TomoKV now matches. |
| BLMPOP key priority, edge, COUNT, fractional/zero timeout | CONSISTENT outside MULTI | Exact nested array and timeout frames match. |
| BZPOPMIN/BZPOPMAX key priority and score framing | CONSISTENT outside MULTI | Exact key/member/score arrays and timeouts match. |
| BZMPOP key priority, MIN/MAX, COUNT, fractional/zero timeout | CONSISTENT outside MULTI | Exact nested member/score arrays and timeouts match. |
| WAIT validation, `WAIT 0 n`, and WAIT in MULTI | CONSISTENT | Integer and validation errors byte-match. |
| WAIT with unsatisfied replica count and finite/zero timeout | DIFFERS, HANDED ON | TomoKV replies immediately; Redis waits for the deadline or forever. |
| BLPOP/BRPOP/BLMPOP/BZPOPMIN/BZPOPMAX/BZMPOP in MULTI | DIFFERS, DOCUMENTED/HANDED ON | TomoKV's existing transaction exclusion returns an EXEC-element error; Redis returns a null element without blocking. |
| SUBSCRIBE/PSUBSCRIBE/SSUBSCRIBE confirmations and duplicates | CONSISTENT | Exact RESP2 arrays and RESP3 pushes, including regular/shard count namespaces, match. |
| UNSUBSCRIBE/PUNSUBSCRIBE/SUNSUBSCRIBE named, missing, all, and empty | CONSISTENT | Exact confirmations match; no-argument iteration is compared as channel/count sets. |
| Exact plus two overlapping patterns | CONSISTENT | PUBLISH returns 3 and the exact message plus two pattern messages byte-match as a multiset. |
| PUBLISH/SPUBLISH receiver counts and ordered delivery | CONSISTENT | Modeled receiver counts and every delivery frame match; the 128-message exact stream stays ordered. |
| PUBSUB CHANNELS/NUMSUB/NUMPAT/SHARDCHANNELS/SHARDNUMSUB | CONSISTENT | Ordered count replies byte-match; channel sets are canonicalized. |
| RESP2 subscriber-mode allowed/rejected commands | CONSISTENT after fix | PING and subscription controls work; ordinary commands are rejected with exact Redis errors. |
| RESP3 subscriber-mode ordinary commands | CONSISTENT | SET, PUBLISH, PUBSUB and PING execute with exact RESP3 framing. |
| RESP2 subscriber-mode PUBSUB subcommand labels/validation | DIFFERS, RESOLVED | The main-command-only label and premature restriction error were replaced with Redis's subcommand-aware results. |

## Exact differences

### Resolved: blocking timeout null type

Minimal probes on empty keys:

```text
BLMOVE cbd:none:a cbd:none:b LEFT RIGHT .01
  target before: $-1\r\n
  Redis 7.4:     *-1\r\n
  target after:  *-1\r\n

BRPOPLPUSH cbd:none:a cbd:none:b .01
  target before: $-1\r\n
  Redis 7.4:     *-1\r\n
  target after:  *-1\r\n
```

The pre-fix generator transcript was:

```text
  DIFF fractional timeout BLMOVE
    target: b'$-1\r\n'
    oracle: b'*-1\r\n'
DIFFER blocking: 5288 logical ops, 5326 checks, 1 unexpected diffs,
8 documented differences -> FAIL
```

After the change:

```text
DIFFER blocking: 5291 logical ops, 5331 checks, 0 unexpected diffs,
8 documented differences -> PASS
```

### Resolved: subscriber-mode PUBSUB labels and validation

After `SUBSCRIBE p`, the initially observed replies included:

```text
PUBSUB NUMSUB
  target before: -ERR Can't execute 'pubsub': only (P|S)SUBSCRIBE / (P|S)UNSUBSCRIBE / PING / QUIT / RESET are allowed in this context\r\n
  Redis 7.4:     -ERR Can't execute 'pubsub|numsub': only (P|S)SUBSCRIBE / (P|S)UNSUBSCRIBE / PING / QUIT / RESET are allowed in this context\r\n

PUBSUB CHANNELS
  target before: -ERR Can't execute 'pubsub': only (P|S)SUBSCRIBE / (P|S)UNSUBSCRIBE / PING / QUIT / RESET are allowed in this context\r\n
  Redis 7.4:     -ERR Can't execute 'pubsub|channels': only (P|S)SUBSCRIBE / (P|S)UNSUBSCRIBE / PING / QUIT / RESET are allowed in this context\r\n

PUBSUB BOGUS
  target before: -ERR Can't execute 'pubsub': only (P|S)SUBSCRIBE / (P|S)UNSUBSCRIBE / PING / QUIT / RESET are allowed in this context\r\n
  Redis 7.4:     -ERR unknown subcommand 'BOGUS'. Try PUBSUB HELP.\r\n

PUBSUB NUMPAT x
  target before: -ERR Can't execute 'pubsub': only (P|S)SUBSCRIBE / (P|S)UNSUBSCRIBE / PING / QUIT / RESET are allowed in this context\r\n
  Redis 7.4:     -ERR wrong number of arguments for 'pubsub|numpat' command\r\n

PUBSUB HELP x
  target before: -ERR Can't execute 'pubsub': only (P|S)SUBSCRIBE / (P|S)UNSUBSCRIBE / PING / QUIT / RESET are allowed in this context\r\n
  Redis 7.4:     -ERR wrong number of arguments for 'pubsub|help' command\r\n
```

`PUBSUB CHANNELS a b` also used TomoKV's main-command-only restriction label versus Redis's
`pubsub|channels` label.  All of these rows now byte-match.  The before/after suite tails were:

```text
before: DIFFER pubsub: 4190 logical ops, 25478 checks, 5 diffs -> FAIL
after:  DIFFER pubsub: 4198 logical ops, 25486 checks, 0 diffs -> PASS
```

The raw exploratory run also reported four harness-assertion failures; they were not server
differences and were corrected before the after transcript: regular unsubscribe counts retain
pattern subscriptions, all-unsubscribe channel/count pairing follows unspecified iteration order,
and RESP3 subscribed `PING message` returns a bulk string.

### Unchanged handoffs

`WAIT 1 200` at a 50 ms observation point:

```text
target:    :0\r\n
Redis 7.4: <no frame by 50 ms>   (then :0\r\n at the deadline)
```

`WAIT 1 0` at the same observation point:

```text
target:    :0\r\n
Redis 7.4: <no frame by 50 ms>   (continues waiting)
```

Matching this needs an IO-owned deferred reply and a replica acknowledgement frontier.  The current
connection-local handler builds `:0` during parse, so changing it locally would either block its own
IO owner or allow younger replies to cross it.  This was handed on rather than touching IO dispatch.

For each of `BLPOP`, `BRPOP`, `BLMPOP`, `BZPOPMIN`, `BZPOPMAX`, and `BZMPOP`, the empty-key MULTI
sequence has matching `+OK` and `+QUEUED` replies but a different EXEC element:

```text
target:    *1\r\n-ERR command is not supported by MULTI execution\r\n
Redis 7.4: *1\r\n*-1\r\n
```

One geometry also produced TomoKV's existing
`*1\r\n-ERR internal cross-shard routing error\r\n` for the sorted-set forms; Redis still returned
`*1\r\n*-1\r\n`.  `NOTES-MULTI.md` already documents blocking commands as transaction exclusions.
Executing them as non-blocking transaction children would require changes in the transaction/MVCC
and scatter machinery, so this lane did not make that deep change.  `BLMOVE` and `WAIT` in MULTI are
already consistent.

## Test evidence

Release and sanitizer builds:

```sh
make clean && make -j8
make asan
ldd build/tomokv-asan | rg 'asan|ubsan'
```

Purpose boots used only the assigned cores and ports:

```sh
taskset -c 80-87 ./build/tomokv --port 7430 --bind 127.0.0.1 \
  --shards 16 --ratio 6:2 --protected-mode no --atomic 0
taskset -c 80-87 ./build/tomokv --port 7430 --bind 127.0.0.1 \
  --shards 16 --ratio 6:2 --protected-mode no --atomic 1
taskset -c 88-91 /tmp/claude-1000/redis74/src/redis-server \
  --port 7431 --bind 127.0.0.1 --appendonly no --save ''
```

Differ matrix command, repeated with both target boots:

```sh
for seed in 7 19 41; do
  taskset -c 92-95 python3 tests/differ.py \
    127.0.0.1 7430 127.0.0.1 7431 blocking "$seed"
  taskset -c 92-95 python3 tests/differ.py \
    127.0.0.1 7430 127.0.0.1 7431 pubsub "$seed"
done
```

Release matrix tails:

```text
atomic=0 seed=7   blocking 5291 ops / 5331 checks / 0 unexpected diffs / 8 documented -> PASS
atomic=0 seed=19  blocking 5261 ops / 5301 checks / 0 unexpected diffs / 8 documented -> PASS
atomic=0 seed=41  blocking 5226 ops / 5266 checks / 0 unexpected diffs / 8 documented -> PASS
atomic=1 seed=7   blocking 5291 ops / 5331 checks / 0 unexpected diffs / 8 documented -> PASS
atomic=1 seed=19  blocking 5261 ops / 5301 checks / 0 unexpected diffs / 8 documented -> PASS
atomic=1 seed=41  blocking 5226 ops / 5266 checks / 0 unexpected diffs / 8 documented -> PASS

atomic=0 seed=7   pubsub 4198 ops / 25486 checks / 0 diffs -> PASS
atomic=0 seed=19  pubsub 4198 ops / 25265 checks / 0 diffs -> PASS
atomic=0 seed=41  pubsub 4198 ops / 27434 checks / 0 diffs -> PASS
atomic=1 seed=7   pubsub 4198 ops / 25486 checks / 0 diffs -> PASS
atomic=1 seed=19  pubsub 4198 ops / 25265 checks / 0 diffs -> PASS
atomic=1 seed=41  pubsub 4198 ops / 27434 checks / 0 diffs -> PASS
```

Directed batteries under both target boots:

```text
tests/blocking.py --atomic 0 boot: BLOCKING PASS
tests/blocking.py --atomic 1 boot: BLOCKING PASS
tests/pubsub.py  --atomic 0 boot: pubsub: PASS (... batches=48 for 600 publishes ...)
tests/pubsub.py  --atomic 1 boot: pubsub: PASS (... batches=40 for 600 publishes ...)
```

`tests/blocking.py` intentionally executes `CONFIG SET atomic 1` for its atomic visibility arm; the
new blocking differential ran before that battery and therefore supplies the full atomic-0 coverage.

ASAN/UBSAN purpose boot (`--atomic 1`) evidence:

```text
tests/blocking.py: BLOCKING PASS
tests/pubsub.py: pubsub: PASS (... batches=30 for 600 publishes ...)
DIFFER blocking: 5291 logical ops, 5331 checks, 0 unexpected diffs,
8 documented differences -> PASS
DIFFER pubsub: 4198 logical ops, 25486 checks, 0 diffs -> PASS
AddressSanitizer/UndefinedBehaviorSanitizer diagnostics: 0
shutdown: live_conns=0 rob_not_quiesced=0 unsent_bytes_pending=0; wb err=0
```

No benchmark was requested or run.  No knob was added.  The only shelved scope is the two deep
handoffs above; both have exact reproducing sequences and were left out of the local fixes to
preserve the owner, scatter, and MVCC rules.

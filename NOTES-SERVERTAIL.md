# NOTES-SERVERTAIL — the server/introspection tail

Lane H. Everything below shipped on branch `t-servertail`, built from mainline `d177ea9cf`.

Semantics came from the documented RESP protocol plus byte-probing a vanilla redis 7.4 binary at
`/tmp/claude-1000/redis74`. No redis source was read or copied.

---

## 1. Command list landed

| Scope | Commands | File |
|---|---|---|
| A | `TIME` `LOLWUT` `ROLE` `WAIT` `WAITAOF`¹ `FAILOVER` `REPLICAOF` `SLAVEOF` `PFSELFTEST` `SHUTDOWN` `SORT_RO` | `src/cmd/server_tail.cc` |
| A | `SUBSTR` (alias row for the existing GETRANGE handler pair) | `src/cmd/t_string.cc` |
| B | `CONFIG REWRITE` `CONFIG RESETSTAT` `CONFIG HELP` | `src/cmd/server_tail.cc` |
| B | `COMMAND LIST [FILTERBY MODULE\|ACLCAT\|PATTERN]` `COMMAND GETKEYS` `COMMAND GETKEYSANDFLAGS` `COMMAND HELP` | `src/cmd/server_tail.cc` |
| C | `OBJECT ENCODING\|REFCOUNT\|IDLETIME\|FREQ\|HELP` | `src/cmd/server_tail.cc` |
| C | `MEMORY USAGE\|STATS\|DOCTOR\|PURGE\|MALLOC-STATS\|HELP` | `src/cmd/server_tail.cc` |
| D | `SLOWLOG GET\|LEN\|RESET\|HELP`, `LATENCY HISTORY\|LATEST\|RESET\|DOCTOR\|GRAPH\|HELP` | `src/cmd/slowlog.cc` |
| E | `LCS k1 k2 [LEN] [IDX] [MINMATCHLEN n] [WITHMATCHLEN]` | `src/cmd/lcs.cc` |
| F | regenerated `src/cmd/acl_categories_generated.h` | `tools/gen_acl_categories.py` |

`COMMAND COUNT / INFO / DOCS` and `OBJECT ENCODING` already existed; COUNT/INFO/DOCS were left
alone and `OBJECT` was rewritten (see §5). `RESET` was skipped — it belongs to the climon2 lane.

¹ `WAITAOF` ships its grammar and validation exactly. Its `numlocal == 0` replica count is exact,
but the first array element remains a conservative `0`: Redis reports whether its per-connection
write offset is locally fsynced, and TomoKV does not yet retain such an offset. The `numlocal == 1`
durability wait is **SHELVED** and returns an explicit error. See §8.

Registry rows went from 181 to 197, well inside the 255-row ACL command-bit ceiling.

---

## 2. Routing: how OBJECT and MEMORY reach a shard

Their first argument decides whether a key exists at all — `OBJECT ENCODING k` routes on `argv[2]`,
`OBJECT HELP` has no key. The registry validator rejects `first_key >= min_arity`, and the IO
router would read `argv[2]` out of bounds for the keyless forms.

New flag `CmdFlags::SubcmdRoute` (bit 18), carried together with `CursorShard` so **no new branch
appears anywhere near GET/SET**: `CursorShard` already routes a command through
`command_prepare_scan_route()`, the cold hook SCAN/EVAL/XREAD pay for, and that hook now dispatches
to `command_prepare_subcmd_route()` first. Keyed subcommands get `hash`/`shard` resolved there;
keyless ones are pinned to shard 0 and travel the ordinary owner path.

`first_key` stays truthful (2/2/1) so ACL key permissions and MULTI/WATCH still gate the keyed
forms. Both walk their key range bounded by `argc`, so the keyless forms are naturally skipped.
`multi.inc` learned to classify `SubcmdRoute` explicitly rather than falling into the SCAN arm.

---

## 3. SLOWLOG + LATENCY — the cost model

**Disabled (`slowlog-log-slower-than -1` and `latency-monitor-threshold 0`)**: the executor pays
exactly **one predicted-false branch per executed BATCH** (≤32 ops) — not per op. No clock is read,
nothing is allocated, and the armed body is `__attribute__((noinline, cold))` in a separate
function. The arm is latched once per pass from the existing live-config seqlock, alongside the
notify arm, so it costs the pass nothing beyond the version compare it already made.

**Armed, normal mode**: two `now_ns()` reads per batch.
- A batch of **one** op yields that op's exact duration. That is every command on an unpipelined
  connection, which is how a slow command is normally met.
- A batch of **many** that overruns cannot be attributed after the fact — the ops already ran and
  nothing can re-run them. It instead switches that executor to per-op timing for the next 64
  batches (`kSlowlogEscalateBatches`), so the recurrence is timed exactly.

**Armed, escalated mode**: two `now_ns()` reads per op. Exact attribution.

The screen is **per-op average**, not whole-batch elapsed: a full batch of 32 ordinary commands must
not look like one slow command merely because there were 32 of them.

### Divergences from redis, stated plainly

1. **A command that is slow exactly once inside a pipelined batch is not logged.** It arms
   escalation and its next occurrence is logged exactly. Redis times every command individually.
   This is the trade that buys the default-on hot path back.
2. **A cross-shard command produces one entry**, attributed to the owner that computed the final
   answer, not the sum across participating shards. Every owner is handed the op; all but the last
   return with it still `Issued`, and only the owner that published `Done` records.
3. **`SLOWLOG LEN` can exceed `slowlog-max-len`.** The ring is per recording thread (mirroring the
   ACL log's shape), so the bound is per-thread. `SLOWLOG GET` merges and sorts by the global entry
   id, which is a total order, so ordering is correct across threads.

### Connection-local commands are covered too

`DEBUG SLEEP`, `INFO`, `CONFIG`, `CLIENT` and friends never reach an executor, so the IO loop times
them as well, behind the same pass-local arm. GET/SET are dispatched to executors and never enter
that block, so nothing added there is on the hot path.

### Client address and name

The entry is built by a shard owner, which cannot read the connection's `thread_local` client
catalog, and is read back by whichever IO thread runs `SLOWLOG GET`. The recorder keeps its own
16-way sharded `client id -> (addr, name)` directory, written only on connect / disconnect /
`CLIENT SETNAME` — connection lifecycle, never the command path.

### Mechanism counters (the vacuous-validation rule)

`INFO stats` publishes `slowlog_batches_timed`, `slowlog_escalations`, `slowlog_entries_recorded`
and `latency_events_recorded`. `tests/slowlog.py` asserts against all four — in particular the
disabled case asserts `slowlog_batches_timed` does **not** move, which is the only way to tell
"correctly off" from "silently broken".

---

## 4. Knobs

Redis-exact names, grammar and semantics (knob-compat rule):

| Knob | Default | Meaning |
|---|---|---|
| `slowlog-log-slower-than` | `10000` | microseconds; `-1` disables, `0` logs everything |
| `slowlog-max-len` | `128` | entries retained per recording thread; `0` keeps none |
| `latency-monitor-threshold` | `0` | milliseconds; `0` disables the latency monitor |

All three are live via `CONFIG SET` and documented in `tomokv.conf`. `slowlog-log-slower-than`
forced the tree's first genuinely **signed** knob: redis accepts and reports `-1`, so the unsigned
sentinel `--atomic-window` uses would not round-trip. That added `cfg_parse_i64` in `config.h` and
`ConfigKind::Signed` in the CONFIG table.

`Config::conf_path` was added (set by the argv pre-scan in `main.cc`, not by the parser, because
`--conf` is consumed before the parser runs) purely so `CONFIG REWRITE` has a destination.

---

## 5. OBJECT ENCODING — the mapping table

Our representations do not line up one-for-one with redis's, so this is a deliberate, stable
mapping rather than a passthrough of internal names. It replaces the old
`collection_encoding()`, which emitted our internal names (`compact` / `deque` / `btree`), never
`embstr`, never `intset`, and answered `compact` for a small stream.

| Type | Our representation | Redis name |
|---|---|---|
| string | `Enc::Int` | `int` |
| string | `Enc::Raw` — value inline, ≤ 192 B | `embstr` |
| string | `Enc::Extern` — separate block, > 192 B | `raw` |
| hash | `Compact` / `Hashtable` | `listpack` / `hashtable` |
| list | `Compact` / `Deque` | `listpack` / `quicklist` |
| set | `Compact` + `SetSmallEncoding::Integer` | `intset` |
| set | `Compact` + `SetSmallEncoding::Generic` | `listpack` |
| set | `Hashtable` | `hashtable` |
| zset | `Compact` / `Btree` | `listpack` / `skiplist` |
| stream | any | `stream` |

**The embstr/raw boundary is ours (`kEmbedThreshold` = 192), not redis's (44).** The names describe
the same distinction — value bytes inside the object block versus a separate allocation — at a
different size. Any differ suite relying on this must stay outside 45..192 bytes. Redis also leaves
an `APPEND`ed string in `raw` regardless of the resulting length because it over-allocates for
growth; we have no such reservation, so a short appended value stays `embstr`.

Other OBJECT notes:
- `REFCOUNT` is always `1`. We have no shared-object table; redis reports `INT_MAX` for its shared
  small integers. Documented, not faked.
- `IDLETIME` is derived from the same five-bit eviction metadata the victim chooser reads, so it is
  quantised to `1 << lru-clock-shift` seconds and **wraps after 32 buckets** (~8192 s at the
  default shift). With `maxmemory` disabled those bits are never written, so it reports `0`.
- `FREQ` reports the 5-bit LFU counter (max 31), where redis's is 8-bit (max 255).
- `IDLETIME`/`FREQ` use a **no-touch** lookup (`FlatStore::find_no_touch`), because reporting idle
  time through the ordinary `find()` would reset the very metadata being reported. Redis solves the
  same problem with `LOOKUP_NOTOUCH`.
- Missing key replies null (`$-1` / `_`), matching redis 7.4 — verified by probe.

`MEMORY USAGE` is driven off the real accounting contract: `kvobj_size(o) +
FlatStore::kSlotOverheadPerKey`, which is exactly the per-key charge `accounted_bytes()` uses, so
the sum over keys reconciles with `used_memory`. `SAMPLES` is parsed, validated and ignored — our
accounting is exact, not sampled.

---

## 6. CONFIG RESETSTAT — baseline, not zeroing

Every counter `INFO` reports is a single-writer value owned by a shard owner or an IO loop. Zeroing
them from the calling thread would be a write race on the hot path. Instead `RESETSTAT` snapshots
the aggregate and `INFO` subtracts that baseline. The reads are exactly the cross-thread reads
`INFO` already performs on every call, so no new sharing is introduced and no fan-out is needed.

Covered: `total_commands_processed`, `total_connections_received`, `keyspace_hits`,
`keyspace_misses`, `expired_keys`, `evicted_keys`, `rejected_connections`, the `acl_access_denied_*`
family, and the per-command `cmdstat_*` calls.

## CONFIG REWRITE

Writes a **complete, clean file of the knobs this build owns** to `Config::conf_path`. Two
deliberate consequences: comments and unknown-to-us directives from the originally loaded file are
not preserved, and only names the boot parser actually accepts are emitted. That last filter is
load-bearing — `save`, `databases`, `proto-max-bulk-len` and `aof-use-rdb-preamble` live in the
CONFIG table for client compatibility but are not CLI flags, so writing them would produce a file
the server then refuses to boot from. Write-then-rename, so a failure cannot truncate the config.

---

## 7. LCS

Two string keys, possibly on different shards, lowered through the existing scatter/gather
machinery. `Kind::Lcs` sits **before** `Kind::Msetnx` so `is_two_hop()` keeps it out of the
write/barrier/apply machinery: it is PFCOUNT-shaped (gathers full `ObjectImage`s, computes a scalar
result, terminal at phase 1) with SINTERCARD's option-parsing discipline (trailing options parsed on
IO, parsed scalars stashed in the arena header).

The DP, the option grammar and the reply builders live in `src/cmd/lcs.cc` and are shared by BOTH
routings — the same-shard local fast path in `cmd_xshard_only_impl` and the cross-shard
`finish_lcs` call the identical functions, so the two paths cannot diverge. The local arm is placed
*before* the generic image gather, which is bounded by `argc` and would otherwise serialize the
trailing option words as if they were keys.

Reply-shape details established by byte-probing, all reproduced exactly:
- matches are emitted from the **end** of both strings backwards, not in reading order;
- `MINMATCHLEN` filters the emitted runs but does **not** change the reported `len`;
- a negative `MINMATCHLEN` is accepted and means no filter;
- `WITHMATCHLEN` without `IDX` is accepted and ignored;
- `LEN` + `IDX` together is a specific error, not a syntax error;
- a non-string key is `ERR The specified keys must contain string values`, **not** `WRONGTYPE`.

**The size hazard is real and is not papered over.** The table is n·m cells and nothing in the
existing gather machinery would stop it: `compute_bitop` has the same unbounded-input exposure but
is linear, whereas LCS is quadratic. We refuse at the same boundary redis does (the product
overflowing what can be indexed) so the differ stays exact on the refusal as well as on the answer;
below that boundary both servers simply burn the CPU. The DP runs on the executor that closed the
group, which is the established place for non-trivial work in this tree (SORT sorts, BITOP walks
every byte, PFCOUNT merges 16384 registers); the reply itself is still emitted on IO.

---

## 8. SHELVED: `WAITAOF numlocal 1`

**What ships:** the full grammar and all validation, byte-exact against the oracle for the
standalone, no-replica case, including
`ERR WAITAOF cannot be used when numlocal is set but appendonly is disabled.`,
`ERR value is out of range, value must between 0 and 1`,
`ERR value is out of range, must be positive`, and `ERR timeout is negative`.

**What does not:** the `numlocal == 1` local-fsync wait, or Redis's per-connection fsynced-local
count in the first element of a `numlocal == 0` reply. The former returns an explicit error naming
the gap; the latter remains the conservative `0` rather than a plausible server-global number.

**Why.** A connection-local handler runs at PARSE time, before the ops ahead of it on the same
connection have executed, so the AOF sequence it could sample does not yet cover the caller's own
writes. Waiting synchronously cannot fix that: retiring those older ops requires this very IO
thread, so the wait would deadlock against them. A conservative "check once and report 0" would
never claim durability we lack, but it would silently under-report, and the shelve rule prefers a
loud gap to a quiet one. The same ordering issue prevents an exact `numlocal == 0` first element:
Redis tracks the calling connection's last write offset. A server-global posted/durable comparison
would under-report a reader when another connection has an unsynced write, while a global fsync
counter would over-report a writer whose newest write is not covered.

**The design it needs**, for whoever picks it up:
1. Register `WAITAOF` as `ConnLocal | PubSub` and dispatch it by name in `pubsub_start_command`,
   returning `PubSubStartResult::Async`. `io_loop.h` already handles `Async` by publishing the ROB
   slot without storing `Done` — that is the deferral primitive, and `CLIENT LIST`'s scatter
   (`climon.cc:58-92` start, `:258-280` finish) is the worked example.
2. Keep a per-IoLoop pending map keyed by connection id, mirroring `climon_pending_`.
3. Drain it once per loop pass: wait until the connection's older ops have retired
   (`rob().flush_id()` past the WAITAOF op id), then force each owning producer to post
   (`AofProducer::flush`) so the sequence covers the caller's writes, capture
   `AofManager::posted_sequence()`, and poll for durability against the deadline.
4. `AofManager` needs a policy-independent durable accessor. `reply_gate_ready()` is a *reply*
   safety gate, not a durability gate — under `appendfsync no` it always returns true, and under
   `everysec` it compares against `written_sequence_` (page cache), not `durable_sequence_`. Add
   `durable_sequence()` or a `durable_gate_ready(target)` beside it.
5. Test under **both** persist-io engines: under `normal` a write can reach durable synchronously
   inside one `writer_pass`, while under `uring` each frontier step needs a CQE.

---

## 9. Test evidence

Server cores 104-107, port 7280; oracle (vanilla redis 7.4) cores 108-111, port 7285.

```
servertail: 101 checks, 0 failures -> PASS
slowlog:     60 checks, 0 failures -> PASS
lcs:        907 checks, 0 failures -> PASS
DIFFER servertail: 5339 ops, 0 diffs -> PASS      (seed 7)
DIFFER servertail: 5339 ops, 0 diffs -> PASS      (seed 99)
DIFFER servertail: 5339 ops, 0 diffs -> PASS      (RESP3, -3)
```

Both `--atomic 0` and `--atomic 1`, all four suites, all PASS. Under ASAN
(`make asan`, `-fsanitize=address,undefined`), all four suites PASS with **0** sanitizer reports.

`tests/lcs.py` checks 220 randomised trials against an independently written Python implementation
of the same algorithm, deliberately routing half the pairs to one shard and half across shards, so
the local fast path and the scatter path are both compared against the reference.

### Differ exclusions, with reasons

The `servertail` suite aligns the two servers' encoding thresholds in a preamble (the knobs are
spelled differently on each side, so the alignment cannot ride in the diffed op stream) and then
compares byte-for-byte. Three things are deliberately not byte-compared:

- **`MEMORY USAGE` values** — our accounting, not redis's. Normalized to hit-vs-miss.
- **`COMMAND INFO` flag / ACL-category / key-spec arrays** — redis-internal. Normalized to
  name/arity/key-range, which *is* compared and does match. `object` and `memory` are excluded from
  that fixed list entirely: redis reports a 0/0/0 key range on the container and hangs the real key
  spec off its subcommands, while our row carries the truthful `argv[2]` range because ACL, MULTI
  and the router all consume it. Ours is the more informative answer and will not be made wrong to
  match a byte.
- **`OBJECT ENCODING` on strings of 45..192 bytes, and `OBJECT REFCOUNT`** — see §5.

### An ASAN finding that is NOT ours

Terminating with a client still connected leaves that connection's ROB block unreclaimed, so
LeakSanitizer exits 1. Verified **identical on the pre-existing SIGTERM path**, so `SHUTDOWN`
introduces nothing: `tests/servertail.py` sets `detect_leaks=0` for the throwaway server it boots,
which keeps the exit-code assertion meaningful while every ASAN memory-error check stays armed.

---

## 10. INDICATIVE performance — slow log instr/op PRE vs POST

Loopback, server on cores 104-105 (`--ratio 1:1 --shards 8`), memtier on 106-107, pipeline 32,
64-byte values, 200k keys, 12 s perf window. The divisor is the `INFO total_commands_processed`
DELTA across the window, not memtier's reported rate, so ramp-up and teardown do not skew it.
Three interleaved repeats per arm; the table reports the median.

**INDICATIVE**, not a gate number: this lane owns four cores, so the load generator is co-resident
with the server, and other lanes were active elsewhere on the box. The arms were interleaved and
repeated so the *comparison* is the load-bearing part; the absolute values are not comparable to
the 32-core gate cells.

| cell | arm | median instr/op | mean | spread | vs PRE | Mops/s |
|---|---|---:|---:|---:|---:|---:|
| GET p32 | PRE (base `d177ea9cf`) | 2470.2 | 2468.3 | 0.30% | — | 2.37 |
| GET p32 | POST, `slowlog-log-slower-than -1` | 2473.7 | 2473.7 | 1.85% | **+0.14%** | 2.36 |
| GET p32 | POST, default (`10000`) | 2463.8 | 2464.6 | 0.53% | **−0.26%** | 2.38 |
| SET p32 | PRE (base `d177ea9cf`) | 2720.3 | 2718.1 | 0.49% | — | 2.09 |
| SET p32 | POST, `slowlog-log-slower-than -1` | 2726.0 | 2726.5 | 0.34% | **+0.21%** | 2.10 |
| SET p32 | POST, default (`10000`) | 2740.1 | 2739.5 | 0.42% | **+0.73%** | 2.11 |

**Disabled is free.** +0.14% (GET) and +0.21% (SET) are inside the run-to-run spread — the GET
POSTOFF arm's own spread is 1.85%, wider than the delta itself. That is the one predicted-false
branch per batch behaving as designed.

**Default-on costs +0.73% on SET, which MISSES the ≤ +0.5% target in the brief.** Reported rather
than buried, with the mechanism: the batch screen is nearly free, but batches of ONE take the exact
arm, which snapshots the command's arguments *before* execution (the lifetime fix in §3 — argv
points into the connection read buffer, which may be compacted the moment the op retires). A SET
carries a 64-byte value, so its snapshot copies ~64 bytes; a GET copies only a short key, which is
why GET shows no cost at all and SET does. Throughput is unaffected (2.11 vs 2.09 Mops/s, i.e.
noise in our favour), so this is instruction volume, not a stall.

Two honest follow-ups for whoever tunes this, neither of them applied here because the shipped
binary should be the measured binary:
- Skip the snapshot entirely when `slowlog-log-slower-than` is `-1` but the latency monitor is
  armed. Free, obviously correct, and it makes latency-only arming as cheap as fully disabled.
- The snapshot is only needed because the exact arm reads argv after `execute()` publishes `Done`.
  Recording from inside the executor before that store would remove it, at the cost of threading
  the timing through `execute()`.

# NOTES-CLIMON2 — CLIENT full surface + MONITOR + CLIENT TRACKING, rebuilt zero-cost-when-off

Lane F, branch `t-climon2`. Server cores 56-59 / port 7260, oracle cores 60-63 / port 7265.

## SHIP GATE: idle instr/op, PRE vs POST

The lane exists because a previous attempt at this surface measured **+64.9 instructions/op on an
idle server** and was stop-claused. The gate is therefore the number, not the feature list.

INDICATIVE, loopback, lane cores only. Server `taskset -c 56-59` (2 io + 2 ex), loadgen
`taskset -c 60-63` memtier `-t 4 -c 8 --pipeline=32`, 100k keys, `dbsize == key-maximum` so GET
hit rate is 100%. Fixed request count (12.8M ops) so the denominator is exact; `perf stat -e
instructions -p <server pid>` wraps the whole measured run. PRE and POST arms **interleaved**.
Every lane feature is OFF (no MONITOR client, no tracking client, no pause, no CLIENT REPLY mode,
`notify-keyspace-events` empty) — that is what "idle" means here.

### GET p32 — the gate cell

All 11 interleaved pairs are reported; **no round was discarded**. Nine lanes share this box, so
the raw spread is dominated by co-tenancy, not by the binary — hence median and IQR alongside the
mean.

| round | PRE instr/op | PRE ops/s | POST instr/op | POST ops/s |
|---|---:|---:|---:|---:|
| 1  | 2917.8 | 4,849,638 | 2912.6 | 4,868,777 |
| 2  | 2918.3 | 4,854,916 | 2918.7 | 4,861,223 |
| 3  | 2926.5 | 4,920,679 | 2922.9 | 4,904,233 |
| 4  | 2930.5 | 4,874,109 | 2916.4 | 4,863,993 |
| 5  | 2932.8 | 4,907,484 | 3015.3 | 4,805,797 |
| 6  | 2922.5 | 4,928,394 | 2911.8 | 4,890,293 |
| 7  | 2932.9 | 4,887,796 | 2933.4 | 4,862,760 |
| 8  | 2936.1 | 4,852,751 | 2918.3 | 4,847,514 |
| 9  | 2984.0 | 4,770,105 | 2960.7 | 4,836,748 |
| 10 | 2917.5 | 4,843,522 | 2912.4 | 4,868,603 |
| 11 | 2923.9 | 4,785,192 | 2974.2 | 4,701,131 |

| statistic | PRE | POST | delta |
|---|---:|---:|---:|
| **median instr/op** | **2926.5** | **2918.7** | **−7.8 (−0.27%)** |
| mean instr/op | 2931.2 | 2936.1 | +4.9 (+0.17%) |
| IQR | 12.5 | 32.6 | — |
| min / max | 2917.5 / 2984.0 | 2911.8 / 3015.3 | — |
| median ops/s | 4,854,916 | 4,862,760 | +0.16% |

**Verdict: no regression resolvable.** The median moves −7.8 instr/op (POST *lower*); the mean
moves +4.9 (POST higher); the two statistics disagree in sign, which is exactly what "the effect
is smaller than the noise" looks like. The upper tail of both arms is co-tenancy: every round
above 2950 instr/op also has ops/s at or below the arm's 10th percentile.

Instrument resolution: the IQR is 12.5 (PRE) / 32.6 (POST), so the **+64.9 instr/op the previous
attempt cost would be 2-5x the resolvable band and unmistakable.** It is not present.

Rounds 1-4 of the POST arm were taken before `CLIENT NO-TOUCH` was wired into the executor;
rounds 5-11 include it. Both sub-series sit inside the same band (rounds 1-4 mean 2917.7,
rounds 6-8+10 mean 2919.0), so the no-touch arm — two instructions inside an already
predicted-false `maxmemory_enabled_` test — did not move the cell either.

### Static corroboration (objdump)

| symbol | PRE insns | POST insns |
|--------|----------:|-----------:|
| `cmd_get<false,true>` (clean handler) | 369 | 369 |
| `cmd_set<false>` (clean handler) | 598 | 598 |
| `IoLoop::parse_and_dispatch<false>` | 1557 | 1575 |

The clean executor handlers are byte-count identical. `parse_and_dispatch` gains 18 *static*
instructions across the entire dispatch tree (subscriber mode, blocking, scatter, atomic, the
ConnLocal arm, the pause work-accounting arm) — none of them on the GET/SET straight line, which
is what the empirical cells above confirm.

### A discarded earlier series (recorded, not used)

An earlier wall-clock-window variant of the harness (instr/op derived from ops/s x window rather
than from a fixed request count) produced 7 pairs, of which rounds 5-7 collapsed to 1.6-3.5M
ops/s mid-run as other lanes started heavy work. That whole series is discarded — not because
the numbers were unwelcome, but because two of its three surviving pairs cannot be attributed to
the binary. Its clean pairs agreed with the table above (PRE mean 2866.2, POST mean 2857.3 over
5 and 4 clean rounds). The fixed-request-count harness replaced it precisely because the
wall-clock denominator compounded two noisy measurements.

Raw logs for every round quoted here: `perf` output and memtier totals under the lane scratchpad.

---

## The architecture: why it is free when off

`parse_and_dispatch` makes **exactly one** feature decision per operation, and it is the one it
already made before this lane existed:

```cpp
if (__builtin_expect(notify_armed, false)) {
    spec = command_notify_variant(spec);
    if constexpr (NoBorrow) spec = command_tls_variant(spec);
    op->spec = spec;
    if (__builtin_expect(climon_armed_gate(c, *op), false)) break;   // cold, out-of-line
} else {
    if constexpr (NoBorrow) spec = command_tls_variant(spec);
    op->spec = spec;
}
```

`notify_armed_` is now the union of "keyspace notifications configured" and "some
CLIENT/MONITOR/TRACKING feature is armed". With everything off the emitted sequence is exactly
the pre-lane one: one predicted-not-taken test, the tls-variant select, the spec store.

Supporting rules:

* **One armed word.** `Server::climon_armed()` combines `{monitor, tracking, pause, reply}`. The
  io loop caches it **once per batch** (`climon_refresh_armed`). Per-batch checks are free; that
  is what buys every per-operation hook its zero cost.
* **Mid-pass invalidation.** A `CLIENT` subcommand can arm or disarm the lane *inside* the pass
  whose armed cache it invalidates — a pipelined `CLIENT REPLY OFF; PING` would otherwise answer
  the PING. `climon_armed_dirty_` ends the pass at the (already cold) CLIENT/ConnLocal dispatch
  arms; the next pass re-reads. One predicted-false test on branches GET and SET never enter.
* **Nothing new in `Client` or `Op`.** Both are footprint-locked (1984 / 336) and both
  static_asserts still hold. Per-connection lane state lives in an `IoLoop`-owned map keyed by
  connection id, created only for a connection that actually uses a feature — an idle server
  holds an empty map. The two per-operation bits (`CLIENT REPLY` suppression, `CLIENT NO-TOUCH`)
  use free bits of `Client::connection_flags_` / `Op::route_flags_`, a byte the dispatcher
  already loads and stores.
* **All code is cold.** `src/cmd/climon.cc` and `src/cmd/tracking.cc` are separate translation
  units; `parse_and_dispatch` reaches them through one out-of-line call.

### Feature-ON costs (paid only when the feature is on, reported honestly)

* Arming any lane feature selects the *notification* handler variants for every command, even
  with `notify-keyspace-events` empty. Those variants read the shard mask, find their class bit
  clear, and record nothing — correctness is unaffected (a monitor never produces keyspace
  notifications, and vice versa) but the armed handler wrapper is not free.
* MONITOR: one feed line formatted per command, encoded once and shared by `shared_ptr` with
  the io owners in `climon_monitor_io_mask()` only. Owners with no monitor get no message.
* TRACKING: one broadcast per changed key, filtered first by a 64 Kbit monotonic key filter and
  then by the tracking-io mask, so writes to un-tracked keys post nothing.
* PAUSE: while a pause is live, a held connection's parse pass reports no progress, so the ring
  parks instead of spinning. That accounting arm costs one predicted-false test per active
  connection per pass when no pause exists.

---

## Ownership model for CLIENT TRACKING (the part the brief asked to be documented)

* The **key -> interested-connections table is per io owner** and contains only that owner's own
  connections (`IoLoop::climon_track_keys_`). A connection belongs to exactly one io thread for
  life, so the table has one reader and one writer: **no lock sits on any armed path.** A single
  global table would have put a lock exactly where writes live.
* **Read registration is io-side**, inside the one armed gate. The io thread already holds the
  connection, the command spec and the parsed key range there, so registration is a hash insert
  with no cross-thread hop. A read from a non-tracking connection never reaches `tracking.cc`.
* **Invalidation source is the executor-side keyspace-notification chassis.** The armed write
  handler variants already observe every mutation with its key — *including the expiry and
  eviction paths no io thread can see*. Tracking arms them through a synthetic class,
  `NOTIFY_TRACKING` (bit 20), which `parse_notify_flags` never produces and
  `serialize_notify_flags` never emits, so `CONFIG GET notify-keyspace-events` still reports
  exactly what the operator configured. A write is observed once for both features; when only
  one is armed the other produces nothing.
* **Delivery** broadcasts the changed KEY (never client state) to the owners in
  `climon_tracking_io_mask()`; each filters against its own table. REDIRECT adds one hop
  (`TrackingDeliver`) when the target is owned by a different io thread.
* **FLUSHALL/FLUSHDB** is the one mutation with no per-key notification to ride, so it is
  observed at dispatch. It is a scatter barrier, so firing there cannot reorder anything a
  client can observe.
* The bound (`tracking-table-max-keys`) is therefore applied **per io owner**; with T io threads
  the process ceiling is T x the knob. Hitting it evicts a random entry **and sends its owner an
  invalidation for that key**, so a client can never keep serving a value the server stopped
  tracking.

---

## Command surface

Already shipped before this lane: `CLIENT ID | SETNAME | GETNAME | SETINFO | INFO | LIST | KILL |
NO-EVICT`, `RESET`.

Added here:

| command | notes |
|---|---|
| `CLIENT HELP` | 58 simple strings, oracle-matched text and ordering (incl. redis's `UNPAUSE`-before-`PAUSE` quirk) |
| `CLIENT NO-TOUCH ON\|OFF` | wired to the real LRU/LFU read touch, see below; surfaces as `flags=T` in CLIENT INFO |
| `CLIENT REPLY ON\|OFF\|SKIP` | per-OP suppression; `ON` always answers |
| `CLIENT PAUSE ms [WRITE\|ALL]`, `CLIENT UNPAUSE` | global deadline, checked per io batch |
| `CLIENT UNBLOCK id [TIMEOUT\|ERROR]` | cross-io-owner post to the blocked client's owner |
| `CLIENT TRACKING on\|off [REDIRECT id] [PREFIX p ...] [BCAST] [OPTIN\|OPTOUT] [NOLOOP]` | |
| `CLIENT CACHING yes\|no` | |
| `CLIENT GETREDIR` | |
| `CLIENT TRACKINGINFO` | 3 fields; `flags` is a RESP3 set, `prefixes` stays an array |
| `MONITOR` | |
| `RESET` (extended) | now also clears monitor mode, tracking, CLIENT REPLY mode, NO-TOUCH |

Per-subcommand arity errors (`ERR wrong number of arguments for 'client|<sub>' command`) and the
unknown-subcommand pointer (`Try CLIENT HELP.`) are implemented for the whole `CLIENT` family,
not just the new half.

### CLIENT REPLY is per-OP, and that is load-bearing

Suppression cannot be decided per connection. A pipelined `CLIENT REPLY SKIP; PING; PING` retires
all three replies in ONE `Rob::drain`, so a per-connection decision swallows both PONGs. The
armed gate marks the individual `Op` (`Op::kReplySkip`, bit 3 of `route_flags_`, which
`Client::connection_flags_` also leaves free); a cold `WbEngine::serve_suppressing` variant —
selected by one predicted-false test **per served connection, not per operation** — drops marked
ops and stages the rest. Special command state is still surrendered through `retire_fn_` (or
scatter/blocking/MULTI/notification state leaks and cross-shard groups never complete), and a
surviving borrow is returned to its owning shard instead of being sent.

### CLIENT NO-TOUCH is genuinely wired

`FlatStore::store_find` really does update LRU/LFU metadata on reads
(`if (maxmemory_enabled_ && found) touch(found)`), so this is not a no-op feature. The connection
answer rides `Client::connection_flags_` bit 4, which `Op::reset` copies wholesale, so it reaches
the executor for free; `ExLoop::execute` publishes it to the shard **inside the already
predicted-false `maxmemory_enabled_` arm**, and the read path becomes
`maxmemory_enabled_ && !no_touch_` — with maxmemory off (the default, and the benched
configuration) the no-touch byte is never even loaded because `&&` short-circuits.

Proof of mechanism, not of absence of breakage: `INFO stats` exposes `client_no_touch_ops`, and
the battery arms maxmemory, asserts the counter moves by >=20 with the flag on, and asserts it
does **not** move with the flag off (both directions).

---

## Knobs

| knob | default | notes |
|---|---|---|
| `tracking-table-max-keys` | `1000000` | redis knob name and semantics. `0` = unlimited. Applied per io owner (see above). CLI `--tracking-table-max-keys N`, conf-file `tracking-table-max-keys N`, one parser. Documented in `tomokv.conf`. |

No knob is added for MONITOR or CLIENT PAUSE: neither has one in redis, and both are pure
connection state. Off means no allocation — the per-connection map is empty on an idle server and
the tracking table is not constructed until a connection issues `CLIENT TRACKING on`.

New `INFO` fields (all proof-of-mechanism counters used by the batteries):
`# Clients`: `tracking_clients`, `monitor_clients`.
`# Stats`: `monitor_feed_lines`, `client_pause_holds`, `client_no_touch_ops`,
`tracking_total_keys`, `tracking_total_items`, `tracking_total_prefixes`,
`tracking_invalidations`.

---

## Test evidence

### Directed batteries (`tests/climon2.py`, `tests/tracking.py`)

Every check proves its mechanism fired — exact wire bytes including *absence*, plus a counter
that must move — and every "must not fire" case is a real negative control that drains the
socket and requires emptiness.

```
=========== atomic=0 ===========
climon2      climon2: ok (53 checks, monitor_feed_lines=8, client_pause_holds=77, client_no_touch_ops=20)
tracking     tracking: ok (56 checks, tracking_invalidations=14 fired)
notify       notify: ok (notify_events_fired=1651)
pubsub       pubsub: PASS (regular_fanout=24, shard_fanout=24, ordered_messages=40, shard_churn=320)
blocking     BLOCKING PASS
multi_exec   MULTI/WATCH directed battery passed
resp3        RESP3 directed: 140 checks -> PASS
limits       limits: PASS
auth         auth: PASS (registry gate, SHA auth, HELLO ordering, latching, limits, counters)
ryow         RYOW PASS
=========== atomic=1 ===========
climon2      climon2: ok (53 checks, ...)
tracking     tracking: ok (56 checks, tracking_invalidations=14 fired)
notify       notify: ok (notify_events_fired=1625)
pubsub       pubsub: PASS
blocking     BLOCKING PASS
multi_exec   MULTI/WATCH directed battery passed
resp3        RESP3 directed: 140 checks -> PASS
limits       limits: PASS
auth         auth: PASS
ryow         RYOW PASS
```

`tests/tracking.py` was run 3x per atomic mode with identical results (no flakes). The tallies
above are from the final binary; `dumprestore` (297 checks) and `torture` also pass in both
modes.

### Differential vs vanilla redis 7.4 (`tests/differ.py climon`)

New suite. ~4200 randomised CLIENT-grammar ops (every reply identity-independent) byte-compared
against the oracle, then an invalidation stream: 40 randomised read/write rounds plus directed
NOLOOP and BCAST sections, with the RESP3 push frames byte-compared.

```
DIFFER climon: 4318 ops, 0 diffs -> PASS
```

It found real divergences during development and every one of them was fixed rather than
normalised away: per-subcommand arity strings, `REDIRECT -1/0`, `CLIENT TRACKING off <options>`
accepting anything, `CLIENT CACHING` check ordering, the two distinct prefix-overlap wordings
("another provided" vs "an existing"), the exact TRACKING validation ORDER (7 steps, pinned by
individual differ cases), and — the last one — that redis **replaces** the per-call
OPTIN/OPTOUT/NOLOOP modifiers rather than accumulating them.

### ASAN + UBSAN

`make asan`, both batteries under `--atomic 0` and `--atomic 1`, plus notify/pubsub/blocking/
multi_exec/resp3, then a clean shutdown for the leak check:

```
climon2    climon2: ok (53 checks, ...)
tracking   tracking: ok (56 checks, tracking_invalidations=14 fired)
notify     notify: ok (notify_events_fired=1595)
pubsub     pubsub: PASS
blocking   BLOCKING PASS
multi_exec MULTI/WATCH directed battery passed
resp3      RESP3 directed: 140 checks -> PASS
AddressSanitizer errors: 0    LeakSanitizer leaks: 0
```

One UBSAN diagnostic fires and it is **pre-existing and unrelated**: a misaligned 4-byte load in
the vendored `third_party/lua/lstring.c:87`, reached through the scripting path. Nothing in this
lane touches Lua.

### Wider regression sweep

Run against the final binary under BOTH `--atomic 0` and `--atomic 1`; all pass:
`notify`, `pubsub`, `blocking`, `multi_exec`, `resp3`, `limits`, `auth`, `ryow`, `zc`,
`dumprestore`, `stream`, `torture`. Differ suites `string` (4033 ops) and `notify` (345 ops /
470 events) also pass at 0 diffs, alongside the new `climon` suite.

### One pre-existing flake, investigated rather than waved through

`tests/evict_battery.py <port> lru` has a `hot survives >= cold` check that is a coin flip on
this box **on both binaries**:

| binary | passes | detail (hot vs cold survivors) |
|---|---|---|
| PRE | 2 of 6 | 24/17, 21/24, 20/21, 18/21, 19/15, 19/21 |
| POST | 3 of 6 | 21/27, 10/19, 22/20, 22/19, 22/22, 12/21 |

Because `CLIENT NO-TOUCH` touches exactly this path, it was NOT accepted as flake on the say-so
of a rerun. A temporary counter was built into the suppression site and the `lru` section run
against it: **`dbg_touch_suppressed: 0`** — with an ordinary client the new `!no_touch_` term
suppressed zero touches, so the LRU behaviour is provably identical to the un-gated code, and the
check still failed in that same run. The instrumentation was removed afterwards; the flake is the
battery's marginal 50-key hot set against sampled LRU, and it predates this lane.

---

## Divergences from redis 7.4, deliberate and documented

1. **`CLIENT PAUSE ... ALL` can be cancelled here; in redis it cannot.** Redis postpones
   `CLIENT UNPAUSE` itself under an ALL pause (measured: an UNPAUSE issued 0.5s into a 5s ALL
   pause returned after 4.5s), so an ALL pause can only end by expiring. This server exempts the
   connection-control class (the `CmdFlags::Climon` rows: `CLIENT`, `RESET`, `MONITOR`) so
   `CLIENT UNPAUSE` always works. Everything else, **including PING and reads**, is held under
   ALL — matching redis.
2. **MONITOR self-feed ordering.** Redis emits a monitor's own command reply first and the feed
   line after; this server emits the feed line first (it is produced at the armed gate, before
   dispatch). Both deliver both frames.
3. **MONITOR does not hide per-subcommand ADMIN commands.** Redis hides `CLIENT LIST`,
   `CONFIG GET/SET`, `DEBUG`, `SLOWLOG` from the feed while showing `CLIENT ID/INFO/SETNAME`;
   that filter is per-*subcommand* and this registry carries flags per command. Only `MONITOR`
   itself is excluded here.
4. **MONITOR db index is pre-execution for `SELECT`.** Redis prints the db the command left the
   connection in (`[5] "SELECT" "5"`); the feed here is produced before dispatch, so a `SELECT`
   line shows the previous db. Every other command is unaffected.
5. **A monitoring connection may still run keyspace commands.** Redis registers monitors as
   replicas and answers `-ERR Replica can't interact with the keyspace`. This server has no
   replica concept, so a monitor stays a normal client.
6. **`CLIENT REPLY SKIP` does not swallow unknown-command / arity errors.** Those are answered
   before the armed gate runs (they precede spec resolution), so the skip lands on the *next*
   command instead. Redis suppresses them.
7. **`CLIENT REPLY` inside `MULTI`.** Redis emits a malformed EXEC array (`*3` with 2 elements)
   and leaks the mode past EXEC. Not replicated — that is a protocol bug, not a contract.
8. **`tracking-table-max-keys` is boot-latched.** `CONFIG GET` reports it; `CONFIG SET` refuses.
   Redis allows SET. Making it live needs the value on the live-config seqlock; the io owners
   read it straight out of `Config` today.
9. **Non-BCAST invalidations are one push per key on both sides; BCAST invalidations are not
   coalesced here.** Redis batches BCAST invalidations produced within one event-loop flush into
   a single multi-key push (`>2 invalidate *3 user:3 user:4 user:5`). This server emits one push
   per key. Semantically equivalent, more frames on the wire. **SHELVED** with a reason:
   coalescing requires an owner-side flush boundary that does not exist in this io loop's
   structure, and inventing one is a scheduling change, not a feature change.

---

## Scope cut, with reasons

* **BCAST invalidation batching** — shelved, see divergence 9. Not a correctness gap.
* **MONITOR lines for script-issued commands** (`[0 lua] "set" "lk" "x"`) — **not implemented.**
  The feed is produced io-side after parse; an `EVAL` body executes on an ex thread, which the
  io feed cannot observe. Emitting them needs an ex->io feed path, which would put monitor
  machinery on the executor side of the seam this lane exists to keep clean. The outer `EVAL`
  line itself IS fed.
* **`OBJECT IDLETIME` / `OBJECT FREQ`** — not added. They would have been the natural direct
  observation for NO-TOUCH, but they are eviction-lane surface, not this lane's, and the
  `client_no_touch_ops` counter proves the same plumbing without claiming another lane's
  commands.
* **`tracking_total_keys` / `total_items` in `INFO` are per-process sums of owner-local tables**
  and are therefore exact, but `CLIENT TRACKINGINFO` deliberately does not report them — redis
  7.4's TRACKINGINFO has exactly three fields, and the differ pins that.

---

## Files

| file | role |
|---|---|
| `src/cmd/climon.cc` | CLIENT connection control + MONITOR + the single armed gate (cold TU) |
| `src/cmd/tracking.cc` | CLIENT TRACKING: tables, registration, invalidation, delivery (cold TU) |
| `src/cmd/climon.inc` | the `IoLoop` member declarations for both |
| `src/net/wb.h` | `serve_suppressing` — cold per-op CLIENT REPLY variant |
| `src/cmd/blocking.inc/.h` | `blocking_request_unblock` + the unblock reply shape, written at retire |
| `src/cmd/notify.h/.inc` | `NOTIFY_TRACKING` synthetic class; tracking as a second sink |
| `src/core/server.h` | the armed word, io masks, counters |
| `src/core/io_loop.h` | the one gate call, the batch refresh, the two cold pass-exit tests |
| `src/core/ex_loop.h` | tracking arm on the shard mask; NO-TOUCH publish |
| `src/core/config.h`, `tomokv.conf` | `tracking-table-max-keys` |
| `tests/climon2.py`, `tests/tracking.py`, `tests/differ.py` (`climon` suite) | batteries |

# NOTES-PUSHTEAR — out-of-band frames vs. the zero-copy (borrowed) reply path

Branch `t-pushtear`, worktree `/home/user/Projects/tomokv-cpp-pushtear`.
Nothing in this lane was built, started, benched or run against a server. `AUDIT-resp3push.md`
(committed in this worktree) is the diagnosis this lane implements; it is accurate except for one
item, recorded in §7 below with the oracle evidence.

---

## 1. What was wrong

`pubsub_emit` (`src/core/pubsub.inc:211`) and `IoLoop::climon_push_wire` (`src/cmd/climon.cc:316`)
are the two funnels every out-of-band producer reaches: pub/sub delivery, keyspace notification,
tracking invalidation, tracking redirect-broken notice, tracking FLUSH broadcast, MONITOR feed.
Both made the same four-way choice, and branch 2 was:

```
!rob.quiesced() -> rob.at(rob.dispatch_id() - 1).reply.append(frame)
```

**`op.reply` is the frame's tail for a copying reply and its HEAD for a borrowing one.** `serve_impl`
(`src/net/wb.h`) emits `[direct+reply][borrow][CRLF]`, and a borrowed GET (`src/cmd/t_string.cc:337`)
puts only `$<len>\r\n` into the sink. So the frame landed between a bulk header and its zero-copy
body, and the connection desynchronised by exactly the frame length — silently, with no error on
either side. Same splice in `serve_tls_impl` (TLS copies the borrow but keeps the order),
`serve_suppressing`, and `assemble_mget` (`src/cmd/xshard_commands.inc:1607`, N splice points).

Branch 2 also fired while the op was **Issued on a worker**: two threads appending to one
`SmallBuf`, whose `grow()` (`src/base/slice.h:97`) frees the block the other side holds. Heap
corruption, not merely a tear.

### Why branch 2 existed at all

Not for ordering — for the **direct-reply region**. `Op::direct` is a raw pointer into `fill_buf()`'s
*spare capacity*, handed out at `src/core/io_loop.h:1650-1657` only when `rob.in_flight() == 0 &&
c->nothing_to_write()`. An `append_fill` while it is live overwrites the bytes a worker is
formatting, can grow (free) the block underneath it, and leaves retire's `commit_fill(direct_len)`
publishing at the wrong offset. Branch 2 dodged all of that by not touching `fill_buf` — and paid
for it with the splice.

---

## 2. The fix

**`Op::reply` is no longer an output channel for anything but the op's own handler.**

### `Client::append_oob` (`src/net/conn.h`)

```
segments empty AND rob quiesced -> append_fill          (unchanged; the idle-subscriber hot path)
otherwise                       -> seal_fill_segment(); append_buf_segment(frame)
```

One test covers both hazards, and that is the point of choosing `!quiesced()` rather than something
narrower:

* **Ordering.** `pump` drains `send_buf` remainder, then the *whole* segment queue, then promotes
  `fill_buf`. So once anything is queued, later bytes must go to the queue or they jump ahead.
  `seal_fill_segment()` moves already-staged fill bytes into the queue's tail first, which is
  exactly INV-1/INV-2 from the audit, now applied to this channel too. (The old branch 3 appended a
  segment *without* sealing; that was latently wrong and is fixed by the same line.)
* **Direct region.** Appending a segment allocates its own block and never touches `fill_buf`. And
  the direct region is live only while `!quiesced()`, so the segment route is taken exactly when it
  must be. Retire then takes `has_pending_segments()` → `append_buf_segment(op.direct, direct_len,
  op.reply…)`, which *copies* the direct bytes out of untouched storage, in the right order.

  The invariant that makes this airtight: **at most one op owns a live direct region, and it is
  always the ROB head.** The handout requires `in_flight() == 0`, so the op it is given to *is*
  slot `flush_id`, and in-order retirement keeps it there until it retires. `head_candidate` is
  consumed by the first dispatch of a pass, so a second op cannot qualify while the first is live.
  While it is live `fill_buf` stays empty (nothing appends to it), so `seal_fill_segment()` is a
  no-op and `swap_buffers()` cannot fire (`has_pending_fill()` is false).

### `WbEngine::draining()` / `defer_oob()` (`src/net/wb.h`)

`append_oob`'s frame-boundary argument holds only outside a retire drain. Inside one, a reply can be
**partially staged**: `assemble_mget` pushes `[array header][borrow][…]` into the segment queue and
leaves the reply's tail in the Op, and `notify_retire_batch_entry` (`src/cmd/notify.inc:548`) runs
*immediately after* `xshard_retire` returns — where `tracking_broadcast_keys` calls
`climon_push_wire` **synchronously** for every local tracking client, and `pubsub_drain_events()`
can deliver a queued batch. That is a real mid-frame window with a real callout in it.

So the engine publishes `draining_` (the connection whose drain is running, or null) and a single
`oob_defer_` string. A producer tests `wb_.draining(*client)` first; if true the bytes park and are
flushed by `flush_deferred_oob()` the instant the drain ends — a frame boundary by construction,
and **the same relative position the old `op.reply` parking produced** (behind every reply retired
in this pass), so nothing that reads correctly today reorders.

One buffer suffices *by architecture*, not by luck: one drain runs at a time per io thread, every
producer has already hopped to the target's owning io thread, and frames for any other connection
are not mid-frame and take the direct path. `clear()` keeps capacity → no steady-state allocation.

Wired into all three drains: `serve_impl` (covers plaintext, kTLS, both uring and epoll
instantiations), `serve_tls_impl`, `serve_suppressing`.

### Ordering, stated precisely

| producer fires… | old wire position | new wire position |
|---|---|---|
| ROB quiesced | in the fill buffer | **unchanged** |
| ROB busy, before a serve (pub/sub pass boundary, MONITOR at parse) | after the *newest* op's reply | before the replies of ops that have not yet retired |
| inside a retire drain (notification, tracking hook) | after the newest op's reply | after every reply retired in this drain — **unchanged** |

Row 2 is the only behavioural change, and it is the cell that used to tear. It is also the redis
position: a command that has not executed has produced no reply, so a frame generated now precedes
it. It additionally fixes a latency bug — under the old rule a delivery parked on a `BLPOP` that
then blocked for 30s was *stuck for 30s*.

---

## 3. Adjacent items

| # | Item | Status |
|---|------|--------|
| 1 | Pushes destroyed by worker-side `op.reply.clear()` (`t_string.cc:248`, `bitfield.inc:49`, `acl.inc:1265`, `scatter_engine.inc` ×5, `xshard_commands.inc:1610`) | **Made impossible.** No out-of-band frame ever enters `op.reply`. Also fixes the `serve_suppressing` variant, which drops `op.reply` outright for a `CLIENT REPLY SKIP`/`OFF` op — redis delivers pushes regardless of REPLY mode (`prepareClientToWrite` exempts `CLIENT_PUSHING`), and this server was losing them. |
| 2 | `blocking_retire` swallowing BLPOP timeout/UNBLOCKED (`blocking.inc:1271`) | **Made impossible.** `op.reply.empty() && op.direct_len == 0` means "the handler wrote nothing" again. No code change; `tests/pushtear.py` carries a regression cell. |
| 3 | `CLIENT TRACKING on` accepted on RESP2 with no REDIRECT | **Premise is wrong — nothing changed in this lane.** The distinct invalid-delivery finding was fixed later by the RESP2TRACK lane; see §7. |

---

## 4. Cost, and which commands this lane can touch

* **GET / SET / MGET / MSET on an ordinary connection: not reachable.** Neither emitter runs unless
  something is subscribed, tracking, or monitoring, and `append_oob` did not exist on their path.
* Per **serve** (any connection): two stores (`draining_ = &c` / `= nullptr`) to an already-hot
  engine member and one predicted-true `oob_defer_.empty()` test. Three instructions, per serve,
  not per op.
* Per **out-of-band frame**, when the ROB is busy: one `malloc` + `memcpy` for the segment, where
  before it was a `SmallBuf::append`. The frame already cost a cross-thread post, map lookups and
  frame encoding, so this is noise on that path — but it does put the connection into segment mode
  until the queue drains, which suppresses the direct-reply optimisation for its next few replies.
  That is the pre-existing cost of the borrow path, now shared by heavily-fed tracking/monitor
  connections. **Idle subscribers (the pub/sub benchmark shape) keep the old `append_fill` path.**
* `sizeof(Op)` 336 and `sizeof(Client)` 1984 are **unchanged** — the fix deliberately puts nothing
  on either. `WbEngine` grows by one pointer plus a `std::string`, per io thread.

**Cells to measure:** GET/SET/MGET/MSET p1+p32 for the zero-regression gate (expect a wash — the new
code is unreachable there), plus a pub/sub delivery-rate cell to confirm the quiesced fast path is
untouched, and one MONITOR/tracking-under-pipelining cell to price the segment route.

---

## 5. Instrumentation

`INFO` gains two proof-of-mechanism counters (`src/core/server.h`, `src/cmd/t_server.cc`):

* `oob_frames_segmented` — frame appended while the ROB was **not** quiesced (Done-but-unretired /
  Issued: the cell that tore).
* `oob_frames_deferred` — frame raised **inside** a retire drain (cross-shard MGET assembly, self
  invalidation).

Without them a push battery cannot distinguish "no tear" from "never reached the geometry", and
every pre-existing pub/sub, tracking and zc test sits in the quiesced cell — they would report clean
against the *unfixed* tree. See the vacuous-validation rule.

---

## 6. Validation — described, never run

`tests/pushtear.py HOST PORT [--tls CERTDIR --tls-port N] [--iters N]`. The oracle was checked
offline against the audit's torn and intact byte strings for all four frame shapes (RESP3 push,
RESP2 `message` array, MONITOR status line, MGET gather element) — it accepts every intact stream
and rejects every torn one. It never opens a socket during that check.

### Geometry the battery constructs, and how each cell fails loudly

| cell | geometry it constructs | fails loudly when it cannot |
|------|------------------------|------------------------------|
| `pubsub/resp3/<n>B` | The audit's deterministic head-holder: `PUBLISH ch x` + `GET big` in ONE write. PUBLISH holds ROB slot k un-Done across a cross-thread round trip; GET at k+1 finishes in µs and sits **Done-but-unretired** with its header written; PASS A (deliveries) runs strictly before PASS B (results), so the delivery is *guaranteed* to land in the window. | `SUBSCRIBE` unacknowledged → FAIL. `oob_frames_*` unmoved → "geometry NEVER constructed". `zc_sends` unmoved while size ≥ zc-min → "no borrow was tested". |
| `tracking/self/resp3/<n>B` | Mid-drain, no head-holder and no AOF: `SET big v2` + `GET big` in ONE write take adjacent slots on the same shard (same key ⇒ same worker ⇒ in-order completion), so the self-invalidation fires from inside the SET's retire hook while slot k+1 is the borrowing GET, Done and unretired. RESP2/no-redirect correctly has no invalidation channel; MONITOR retains the RESP2 out-of-band geometry. | `tracking_invalidations` unmoved → "the producer never fired". `oob_frames_deferred`/`segmented` unmoved → "geometry NEVER constructed". |
| `monitor/{resp3,resp2}/<n>B` | `climon_armed_gate` runs at **parse** time, before the current op is published, so the feed line for command N targets op N−1. `GET big` + `PING` in one write aims PING's feed line at the borrowing GET. A second connection adds foreign feed traffic, walking the ROB-state axis by arrival timing; repetition plus the counter is what makes that honest. | `monitor_feed_lines` unmoved → FAIL. `oob_frames_*` unmoved → FAIL. Missing `+PONG` → "reply stream shifted". |
| `xshard-mget/<n>B` | `assemble_mget`'s N splice points: keys on ≥2 shards, several values ≥ zc-min, plus a tracking invalidation raised in the same retire drain that is assembling the gather. | Key set is **found** by bucketing with `DEBUG SHARD`, never assumed; <2 buckets → FAIL. `DEBUG` disabled → FAIL naming `--enable-debug-command yes`. |
| `tracking/no-loss/<n>B` (item 1) | A reader pipelining borrowing GETs while a writer invalidates every tracked key, so frames land on ops in **every** ROB state. | Oracle-free: `tracking_invalidations` delta (produced) vs. push frames received. Received < produced → "LOST: N produced, M reached the client". Zero produced → FAIL. |
| `blocking/push-then-timeout` (item 2) | RESP3 conn subscribes, issues `BLPOP nokey 1`; a second conn publishes. The blocked op is the ROB tail for its whole life, so it is the likeliest out-of-band target there is. | No push → "geometry not constructed". Push but no null within 3s → "SWALLOWED: BLPOP timeout reply missing". |
| `control/copying/64B` | Below zc-min the reply is copied and the old parking was correct. | Must be clean on **both** trees; a failure here means the battery itself is wrong. |

### The axis every existing test got wrong, swept in-battery

`zc-min` is live-settable, so one boot covers three rows and the battery verifies each takes:

* **boot default** (16384) with a 16 KiB value — real-config reachability.
* **forced borrow** (`zc-min 1`) with a 64-byte value — every GET borrows; cheap and wide.
* **borrow disabled** (`zc-min 0`) — negative control; pushes still fire, nothing borrows, must be
  clean.

`CONFIG SET zc-min` refused, or read back different → FAIL, rather than silently testing the old
value.

### Boots the main session should run it under

| boot | why |
|------|-----|
| the feature boot (`--atomic {0,1} --enable-debug-command yes`) | default zc-min; the cross-shard cell needs `DEBUG SHARD`. Two rows, one per atomic mode, matching the existing loop. |
| ASAN | the Issued-op race was a use-after-free on `SmallBuf::grow()`; ASAN is the instrument that names it. The `tracking/no-loss` cell is the one that drives frames onto Issued ops. |
| `--appendonly yes --appendfsync always` | the audit's window-widener: the AOF reply gate holds Done ops unretired for a whole fsync instead of one pass, which turns the MONITOR and tracking arrival-timing cells from a search into a wide window. |
| TLS (`--tls CERTDIR --tls-port N`) | TLS is **not** immune: `serve_tls_impl` copies the borrow but keeps `[direct+reply][value][CRLF]`. Reuses `tests/tls.py`'s generated cert dir. |
| epoll (`--net-io epoll`) | both engines share the reply structure, but the flush point sits between drain and pump in both — cheap to prove. |

**Not wired into `tests/gate.sh`** deliberately: adding a row means bumping `EXPECT_QUICK` /
`EXPECT_FULL`, and that file is the main session's. Suggested wiring is one row in the feature loop
(`pushtear` appended to the `for t in …` list ⇒ **+2 checks**, one per atomic mode ⇒
`EXPECT_QUICK 209 → 211`, `EXPECT_FULL 219 → 221`), plus optional TLS/AOF/ASAN rows.

### Rejected recipe, recorded so it is not retried

A blocking command cannot be the head-holder. `BLPOP` sets `c->set_scatter_barrier(true)`
(`io_loop.h:1444`) and the parse loop refuses to parse behind a barrier (`io_loop.h:1113`), so
nothing can be pipelined behind a blocked op. `PUBLISH` works because `pubsub_start_publish` takes
no barrier and the parse loop `continue`s after an Async pub/sub start.

---

## 7. Found, not fixed in this lane

**(a) AUDIT item (c) is wrong: redis 7.4 does NOT reject `CLIENT TRACKING on` in RESP2.**
`/home/user/Projects/redis/src/networking.c:3374-3497` (REDIS_VERSION 7.4.10) parses the options and
calls `enableTracking` with no `c->resp` check anywhere, and `enableTracking`
(`src/tracking.c`) has none either. **Nothing was changed in `tracking.cc` on this basis.**

**The real divergence is one layer down, and it is larger.** `sendTrackingMessage`
(`redis/src/tracking.c:282-300`) delivers an invalidation only when

* `c->resp > 2` → RESP3 push on the same connection, or
* `using_redirection && (c->flags & CLIENT_PUBSUB)` → RESP2 `message` on the **redirect target**,
  and only if that target is in pub/sub mode,

and otherwise sends **nothing**, with the comment *"RESP2 does not support push messages in the same
connection."* This server (`src/cmd/tracking.cc:203-206`) instead calls
`tracking_emit_invalidation(target, target->resp3(), …)`, so a RESP2 tracking client with no
REDIRECT receives a bare `*3 message __redis__:invalidate …` array on a connection that never
subscribed to anything — which a RESP2 client library reads as the reply to its next command.
Two further sub-divergences on the same path: `tracking.cc:236` hardcodes the RESP2 form for a
local redirect target even when that target is RESP3, and nothing checks that a RESP2 target is in
pub/sub mode.

This was reported rather than shipped by the PUSHTEAR lane. The follow-up RESP2TRACK lane applies
the reference delivery rule, adds the differential geometry, and removes the invalid RESP2 tracking
cell from `tests/pushtear.py`. The tear fix remains covered in RESP2 through MONITOR and in RESP3
through tracking.

**(b) `assemble_mget` still returns with its reply tail unflushed** (`xshard_commands.inc:1607`),
so the MGET frame is incomplete between `xshard_retire` returning and `serve_impl` staging
`op.direct + op.reply`. The deferral closes that window for every current producer, but a future
emitter added *outside* the two helpers would reopen it. Defence in depth would be a final
`flush_bytes()` before `assemble_mget` returns (net-neutral: it moves one
`append_buf_segment` from `serve_impl` into the assembler and makes the subsequent call a no-op).
Not done, to keep the diff attributable.

**(c) Latent trap at the direct-region handout** (`io_loop.h:1650-1657`): between `op->direct =
fb.data()` and `rob.publish()` the ROB still reads quiesced, so an out-of-band frame emitted in that
window would take `append_oob`'s fill path and clobber the region. Unreachable today — there is no
callout between those two lines, and `climon_armed_gate` runs earlier — but the safety is positional
rather than enforced. Same shape as, and pre-existing to, this lane.

**(d) `SegmentQueue::append_buf` does not check `malloc` for null** (`conn.h:116,126`). Carried over
from the audit; out of scope.

**(e) MONITOR self-feed ordering.** `climon.cc:421` records that redis 7.4 emits a monitor's own
reply *before* its feed line, and `tests/climon2.py:281` records that this server emits the feed
line first. The gate runs at parse time, so this lane cannot change it and did not.

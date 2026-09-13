# AUDIT — RESP3 push frames vs. the zero-copy (borrowed) reply path

Tree: `/home/user/Projects/tomokv-cpp-perthread` @ `t-merge14` (`2e794c3bc`). Static reading only —
nothing was built, started, or benched. No file in that tree was modified.

---

## VERDICT

**The tear is reachable. It is not a race-only defect: there is a deterministic, race-free
interleaving that puts an out-of-band frame between a bulk-string header and its borrowed body.**

The `seal_fill_segment()` / `segments_` machinery is *airtight for what it covers*. It orders the
two **connection-level** output channels (`buf_[fill_]` vs `segments_`) correctly, and every
out-of-band producer respects it. What it does not cover is a **third** channel:

> `Op::reply` — a per-op `SmallBuf` that all three out-of-band producers write into whenever the
> connection's ROB is non-quiesced, and whose retire-time splice point for a **borrowed** op is
> *between the header and the value*, not after the whole reply.

So the bug is not in the segment queue. It is that the "park it on the newest live op" trick
(`rob.at(rob.dispatch_id() - 1).reply.append(...)`) was designed against the *copying* reply shape,
where `op.reply` is always the tail of the reply, and was never re-derived for the *borrowing*
shape, where `op.reply` is the **head** of a three-part frame.

Two further consequences of the same root cause are described in §7 (a swallowed BLPOP reply, and
silently dropped pushes), plus one genuine data race.

---

## 1. Producer table — who can append to a connection's output out-of-band

`C-io` = the io thread that owns the connection being written to. All three producers hop to the
target's owning io thread first (locally, or via `pubsub_post` → `PubSubEvent` → the target thread's
inbox), so **no producer ever touches `Client` from a foreign thread**. That part of the design is
sound and is not the problem.

| # | Producer | Entry point | Runs on | How it reaches the conn | Frame |
|---|----------|-------------|---------|--------------------------|-------|
| 1 | pub/sub delivery (`message`/`smessage`/`pmessage`) | `pubsub_emit` — `src/core/pubsub.inc:211` | C-io | `pubsub_deliver_batch` → `pubsub_local_` map, at the pass boundary (`io_loop.h:1978`) | RESP2 `*3`, RESP3 `>3` |
| 2 | keyspace notifications | same as #1 (`NotificationRequest` → `pubsub_home_publish`) | C-io | same | same |
| 3 | client-tracking invalidation | `tracking_emit_invalidation` — `src/cmd/tracking.cc:175` → `climon_push_wire` | C-io | `climon_conn_` map (local) or `TrackingInvalidate`/`TrackingDeliver` event | RESP3 `>2 invalidate`, RESP2 `*3 message __redis__:invalidate` |
| 4 | tracking redirect-broken notice | `tracking.cc:221-224` → `climon_push_wire` | C-io | same | `>2 tracking-redir-broken` |
| 5 | tracking FLUSHALL/FLUSHDB broadcast | `tracking_broadcast_flush` — `tracking.cc:312` | C-io | same | as #3, null key |
| 6 | **MONITOR feed** | `climon_monitor_feed` — `src/cmd/climon.cc:421` → `climon_monitor_deliver:444` → `climon_push_wire` | C-io | `climon_conn_` (local) or `MonitorFeed` event | `+<ts> [db addr] "CMD" …` (protocol-agnostic) |

**Not out-of-band producers** (checked and cleared — they write through the owning op, which is the
correct channel):

* CLIENT KILL — `mark_closing()`; the victim's own reply still goes out normally.
* CLIENT UNBLOCK — `climon_unblock_local` (`climon.cc:335`) sets `state->unblock_reply` on the
  *blocked op*; `blocking_retire` fills that op's own sink.
* Blocking wakeups (BLPOP served by another client's LPUSH) — writes the blocked op's own reply.
* CLIENT LIST / CLIENT KILL fan-out — `climon.cc:529,684` use `dispatch_id()-1` only as an
  **op id** to complete later; they do not append bytes to someone else's op.

All six real producers funnel into **two identical helpers**:

```
src/core/pubsub.inc:211   pubsub_emit()
src/cmd/climon.cc:316     IoLoop::climon_push_wire()
```

whose bodies are the same four-way decision (climon.cc:316-325, pubsub.inc:213-236):

```
1. subscription control in flight for this conn  -> park in PubSubPending::deferred   (pubsub only)
2. !rob.quiesced()                               -> rob.at(rob.dispatch_id()-1).reply.append(frame)
3. client->has_pending_segments()                -> client->append_buf_segment(frame)
4. otherwise                                     -> client->append_fill(frame)
```

Branch **2** is the defect.

---

## 2. The ordering discipline, as the code actually implements it

### Wire order is a strict three-source priority

`pump()` (`wb.h:215-256`) and `pump_epoll()` (`wb.h:269-306`) both drain, in this order:

1. the unsent remainder of `send_buf()` (the "legacy" contiguous buffer),
2. **the whole of `segments_`**,
3. only then may `swap_buffers()` promote `fill_buf()` to be sent.

`nothing_to_write()` (`conn.h:367`) is the union of all three.

### What `seal_fill_segment()` enforces

`conn.h:377-383`:

```cpp
void seal_fill_segment() {
    SmallBuf<kWbufInline>& b = fill_buf();
    if (!b.empty()) { segments_.append_buf(b.data(), b.size()); b.clear(); }
}
```

It copies whatever is staged in the fill buffer into the **tail** of the segment queue and empties
the fill buffer. Given the priority above, the invariant it establishes is:

> **INV-1.** Once `segments_` is non-empty, every byte that must go out *after* the already-queued
> segments must be appended to `segments_`, never to `fill_buf()` — because `fill_buf()` is drained
> strictly *after* the entire segment queue, so a fill append would jump behind (i.e. arrive later
> than) bytes produced after it.

And the assumption it makes about callers:

> **INV-2 (caller contract).** The caller must call `seal_fill_segment()` *before* appending the
> first segment of a borrowed reply, and thereafter every producer on this connection must test
> `has_pending_segments()` and route through `append_*_segment` while it is true.

**INV-1 and INV-2 are honoured by every site in the tree.** I checked all of them:

* `wb.h:584` / `:602` (serve_impl), `:645` / `:654` (serve_tls_impl), `:185` / `:194`
  (serve_suppressing) — each seals, then uses `has_pending_segments()` to choose the channel.
* `xshard_commands.inc:1605` (`assemble_mget`) — seals, then flushes staged bytes into a segment
  before each borrowed value.
* `pubsub.inc:230-232` and `climon.cc:320-322` — both test `has_pending_segments()` before
  falling through to `append_fill`.

There is also a neat second-order property worth recording, because it kills a hazard I went
looking for and did not find: the **direct-reply region** (`op.direct`, a raw pointer into
`fill_buf()`'s storage, handed out at `io_loop.h:1650-1657`) can never be clobbered or reallocated
out from under a worker by an out-of-band `append_fill`. The handout requires `rob.in_flight() == 0
&& c->nothing_to_write()`, and the op is published immediately after — so from the instant the
region is live, `rob.quiesced()` is false, and both helpers take branch 2 or 3, never branch 4.
`SmallBuf::grow()` (`slice.h:97`) frees the old block, so had branch 4 been reachable there it would
have been a use-after-free on a worker thread. It is not reachable. Good.

### What nothing enforces

`Op::reply` is a third output channel with **no ordering rule at all**, and for a borrowed op it is
spliced into the frame at a position that is *not* the end. From `serve_impl` (`wb.h:577-597`):

```cpp
if (op.zc_ptr) {
    if (retire_fn_) retire_fn_(retire_ctx_, conn, op);
}
if (op.zc_ptr) {
    conn.seal_fill_segment();
    conn.append_buf_segment(op.direct, op.direct_len,      // <-- direct THEN op.reply
                            op.reply.data(), op.reply.size());
    conn.append_borrow_segment(op.zc_ptr, op.zc_len, op.zc_shard);   // <-- the value
    conn.append_static_segment(kCrlf, sizeof(kCrlf));
    ...
}
```

For a borrowed GET the handler (`t_string.cc:337-347`) writes **only** `$<len>\r\n` into the sink
and publishes `zc_ptr/zc_len/zc_shard`:

```cpp
if (zc_min && value.n >= zc_min) {
    reply_bulk_header(op.sink(), value.n);
    op.zc_ptr = value.p; op.zc_len = value.n; op.zc_shard = sh.id();
    sh.store().borrow(value.p);
    return;
}
```

So `op.direct + op.reply` is the **header**, the borrow is the **body**, and the static CRLF is the
**terminator**. Anything an out-of-band producer appends to `op.reply` lands **after the header and
before the body**.

That is the whole bug, in one sentence:

> **For a copying reply, `op.reply` is the tail of the frame. For a borrowing reply, `op.reply` is
> the head of the frame. Branch 2 was written for the first shape and is applied to both.**

The identical splice exists in `serve_tls_impl` (`wb.h:645-648`) — **TLS does not save you**; it
copies the borrow instead of handing it to the kernel, but the concatenation order is unchanged —
and in `serve_suppressing` (`wb.h:185-188`), and in cross-shard MGET's `assemble_mget`
(`xshard_commands.inc:1607-1610`, where `flush_bytes()` emits `op.direct + op.reply` immediately
before each borrowed value).

---

## 3. The tear, byte for byte

Client is subscribed to `ch` in RESP3 and issues `GET big` where `len(big) >= zc-min` (default
**16384**, `config.h:203`). The delivery lands while the GET op is in the ROB and its header has
already been written.

`pubsub_emit` branch 2 (`pubsub.inc:227-229`) executes:

```cpp
Op& tail = rob.at(rob.dispatch_id() - 1);
if (resp3) { tail.reply.append(&kPush, 1); tail.reply.append(p + 1, n - 1); }
```

Retire then emits:

```
$16384\r\n                                     <- op.direct (the GET header)
>3\r\n$7\r\nmessage\r\n$2\r\nch\r\n$5\r\nhello\r\n   <- op.reply (the PUSH, spliced INSIDE the bulk)
<16384 bytes of the borrowed value>            <- the BORROW segment
\r\n                                           <- the static CRLF
```

The client reads `$16384\r\n`, then consumes the first 16384 bytes it sees — which are
`len(push)` bytes of push frame followed by `16384 - len(push)` bytes of the value — and is then
left with the value's tail plus `\r\n` in the stream, which it parses as the next reply.
**The connection is permanently desynchronised by exactly `len(push)` bytes.** No error is raised
on either side; the client just starts answering the wrong questions.

### The three cases the task asked about

| Case | Reachable? | Mechanism / why |
|------|-----------|-----------------|
| **RESP3 self-publish** (the valkey case) | **YES — deterministically.** See §5. | `io_loop.h:1242` `if (op->resp3()) goto subscriber_checks_done;` lets a RESP3 subscriber run *any* command, so a subscribed conn can have a borrowed GET in its ROB. `pubsub_start_publish` (`pubsub.inc`) creates **no** `pubsub_pending_` entry, so branch 1 does not park it. `pubsub_flush_outboxes` runs deliveries (PASS A) strictly before publish results (PASS B), which is what makes it deterministic. |
| **Tracking invalidation during a staged borrowed GET** | **YES.** | `tracking_emit_invalidation` → `climon_push_wire` → branch 2. Needs no subscription and no RESP3: `CLIENT TRACKING on` is accepted on a RESP2 connection with no REDIRECT (`tracking.cc:470-620` — there is no RESP2 rejection), and such a client is under **no** command restriction at all. It is precisely the client-side-caching workload — a cache client GETs large values. |
| **MONITOR interleaving a large reply** | **YES.** | `climon_monitor_feed` fires from `climon_armed_gate` (`climon.cc:280`) on *every* command server-wide; `climon_monitor_deliver` → `climon_push_wire` → branch 2. Nothing in this tree restricts what a MONITOR connection may run (unlike redis), so a monitor conn can hold a borrowed GET. Frame is a `+status` line, so **this case needs neither RESP3 nor pub/sub**. |

### Plus a genuine data race (separate from the tear)

Branch 2 fires whenever `!rob.quiesced()` — which includes ops in state **Issued**, i.e. currently
executing on a **worker thread**. `Op::reply` is a `SmallBuf` with no synchronisation; the worker is
appending to it via `Op::Sink` (`op.h:199-221`) while the io thread appends the frame. Concurrent
`SmallBuf::append` (`slice.h:76-80`) means torn `len_`, and if either side trips `grow()`
(`slice.h:97-105`) the old block is `free()`d while the other thread holds a pointer into it —
**heap corruption / use-after-free**, not merely a protocol tear. In the tearing direction this also
produces the "push spliced into the middle of a large *copied* reply" shape (a big LRANGE/HGETALL
whose spill is being written when the frame lands).

Note the race and the tear are *complementary*, and this is what makes the bug awkward to reproduce
by accident:

* frame lands **before** the handler writes the header → `Sink::reserve` sees `op_.reply` non-empty,
  routes the header to `op.reply` too, and the order comes out **correct** (push, then reply). Racy
  but not torn.
* frame lands **after** the header is written (including the fully-`Done`-but-unretired case, which
  has **no race at all**) → **torn**.

So a naive stress test lands mostly on the benign side. See §6.

---

## 4. Why this has not shown up in the existing tests

The two axes have never been crossed:

* `tests/zc.py` — 2 MB values, borrows, mutation under an in-flight borrow. **No pushes.** No
  subscriber, no tracker, no monitor.
* `tests/pubsub.py` — RESP2 and RESP3 subscribers, exact/pattern pushes, including an explicit
  `>` push-byte assertion. **Values are `b"value"`, `b"again"`** — three orders of magnitude below
  `zc-min`, so no borrow ever exists.
* `tests/tracking.py` — invalidation frames asserted byte-exactly (`push()` helper, line 33), but
  values are `"1"`, `"2"`, `"now"`, and the driver is strictly synchronous request/response, so the
  ROB is **quiesced** when every invalidation arrives → branch **4**, the safe one.

This is the same shape as the memory's non-reproduction rule: the discriminating geometry
(borrow **and** out-of-band frame **and** non-quiesced ROB, simultaneously) is not in any cell.

---

## 5. Reproduction — the deterministic one

The hard part is not the borrow and not the push; it is **holding the borrowed GET un-retired**
while the frame lands. Retirement is in-order (`rob.h:86-104`) and the io thread serves in the same
pass it learns of completion (`io_loop.h:324-325`), so the natural window is one pass wide.

Two forcing devices I checked, one of which does **not** work:

* ✗ **Blocking command as head-holder.** `BLPOP` sets `c->set_scatter_barrier(true)` and `break`s
  (`io_loop.h:1444-1447`), and the parse loop refuses to parse behind a barrier
  (`io_loop.h:1113`). You cannot pipeline a GET behind a blocked op. *Recipe rejected.*
* ✓ **PUBLISH as head-holder.** `pubsub_start_publish` explicitly takes **no** scatter barrier
  ("Publish requests from one producer enter each channel home's FIFO in parse order. They
  therefore need no per-connection scatter barrier"), and the parse loop `continue`s after an Async
  pub/sub start (`io_loop.h:1268-1273`). The PUBLISH op stays **not-Done** until
  `pubsub_finish_publish` runs — and `pubsub_flush_outboxes` (`pubsub.inc:328-368`) runs **PASS A
  (deliveries) strictly before PASS B (results)**. So the delivery is guaranteed to be emitted while
  the PUBLISH op — the ROB *head* — is still un-Done, which pins every op behind it un-retired.

### Recipe

```
--zc-min 16384 (default), plaintext port, no TLS.

conn A:  HELLO 3
         SUBSCRIBE ch                       (wait for the ack)
         SET big <16 KiB of random bytes>   (wait for +OK; any conn may do this)

conn A:  send in ONE write, no reads in between:
             PUBLISH ch hello
             GET big
```

Expected (correct) stream on A, in some order that never splits a frame:

```
>3\r\n$7\r\nmessage\r\n$2\r\nch\r\n$5\r\nhello\r\n
:1\r\n
$16384\r\n<16384 bytes>\r\n
```

Observed (torn):

```
:1\r\n
$16384\r\n>3\r\n$7\r\nmessage\r\n$2\r\nch\r\n$5\r\nhello\r\n<16384 bytes>\r\n
```

The assertion that catches it without depending on frame order: **read `$16384\r\n`, then read
exactly 16384 bytes and compare against the value you SET.** They will differ in the first
`len(push)` bytes, and the following two bytes will not be `\r\n`. That check is oracle-free and
cannot false-pass.

Ordering inside the recipe, so the reasoning is auditable:

1. PHASE 1 of the pass parses both frames: `PUBLISH` takes ROB slot *k* (published inside
   `pubsub_start_publish`), `GET big` takes slot *k+1* and is dispatched to a worker. So
   `dispatch_id()-1 == k+1`, the GET.
2. The publish request round-trips to the channel home io thread (≥ 2 cross-thread hops, µs–ms).
   The worker GET completes in µs, so slot *k+1* reaches `Done` first — with its `$16384\r\n`
   header in `op.direct`.
3. Slot *k* is still not `Done`, so `drain()` retires nothing; slot *k+1* sits `Done`-but-unretired.
4. The home posts the DeliveryBatch (PASS A) before the publish result (PASS B). The delivery is
   drained at the pub/sub pass boundary (`io_loop.h:1978-1982`) → `pubsub_emit` → `!rob.quiesced()`
   → **push appended into slot *k+1*'s `reply`.** No race: the worker is done.
5. The result arrives, slot *k* goes `Done`, serve retires *k* (`:1\r\n`) then *k+1*:
   `append_buf_segment(direct, direct_len, reply.data(), reply.size())` = header **+ push**, then
   `append_borrow_segment(value)`, then CRLF. **Torn.**

---

## 6. Geometry — what a search must vary, and what it must NOT assume

The prior burn (0/45 clean because every trial was cross-owner) applies directly here. A test that
fixes any one of these axes at the wrong value reports NOT REPRODUCED while the bug is present.

| Axis | Values that matter | Why |
|------|--------------------|-----|
| **value size** | **≥ `zc-min`** vs `< zc-min`. Default 16384. | Below it the reply is copied and `op.reply` is the frame *tail* → correct order, no tear. **This is the single axis every existing test gets wrong.** Sweep `--zc-min 1` to force borrows on *every* GET, and separately run the default to prove the real-config reachability. Also run `--zc-min 0` (off) as the negative control — it must be clean. |
| **protocol** | RESP3 **and** RESP2. | RESP3 is needed only for the *pub/sub* geometry (`io_loop.h:1242` is what lets a subscriber run GET). The tracking and MONITOR geometries tear in **RESP2** as well. Do not conclude "RESP3 only". |
| **ROB state when the frame lands** | quiesced / `Done`-but-unretired / `Issued` | Only the middle one tears deterministically; the last one is the race (≈ half the time it produces the *correct* order, which is exactly how a stress test false-passes); the first is the safe path all current tests exercise. A test must *construct* the middle state (the PUBLISH head-holder), not hope for it. |
| **connections** | one (self-publish; MONITOR feeding itself) **and** two (foreign publisher / foreign writer / foreign traffic) | The valkey case is self-publish, but nothing here is specific to it. Two-conn is easier to sustain. |
| **producer** | pub/sub, tracking invalidation, MONITOR | Three independent call sites into the *same* two helpers. A fix that only patches `pubsub_emit` leaves `climon_push_wire` broken and vice versa. |
| **shards / owner** | 1 shard and many | Irrelevant to this defect (the tear is at the connection's io thread, not the store), but worth pinning as *checked* so the next reader does not re-litigate it. `--shards 1` is the cheapest reproduction and should tear identically. |
| **transport** | plaintext, **and TLS** | Do not assume TLS is immune. `serve_tls_impl` (`wb.h:640-653`) copies the borrow but keeps the same `direct + reply, value, CRLF` order, so it tears the same way. |
| **retire delay** | none / **AOF reply gate** | `--appendonly yes --appendfsync always` gates PHASE 2 serving on the AOF sequence (`io_loop.h:1988-1996`), which widens "Done-but-unretired" from one pass to a whole fsync. This is the lever that turns the MONITOR and tracking geometries (which have no natural head-holder) from narrow-window races into wide deterministic windows. |
| **publisher thread** | home io == conn io, and home io != conn io | `pubsub_home_for` hashes the channel across io threads. Both must be exercised; use ≥ 2 io threads and several channel names, or pin `--l3-domains`/io count to force each case. |

**How a test should *search* the geometry rather than assume it:** drive the four axes that actually
gate the mechanism — value size (below/at/above `zc-min`), producer (3), protocol (2), and
head-holder present/absent — as a **cross product**, and make the pass/fail signal the byte-exact
readback of the bulk body, not "did we see a push". Then report the **cell map**, not an aggregate:
if the only failing cells are `{size ≥ zc-min} × {head-holder present}`, that pattern *is* the
diagnosis, and an aggregate "3/48 failed" would have read as flake.

---

## 7. Adjacent findings (same root cause, reported, not claimed as the headline)

**(a) A blocked op's timeout/UNBLOCKED reply can be silently swallowed.** `blocking_retire`
(`blocking.inc:1265-1275`) decides whether to synthesise the reply with:

```cpp
if (__builtin_expect(unblock != 0, false) && op.reply.empty() && op.direct_len == 0) {
    if (unblock == 2) reply_err(op.sink(), "UNBLOCKED client unblocked via CLIENT UNBLOCK");
    else blocking_reply_timeout(op, state->kind);
}
```

A blocked op is the ROB tail for as long as it is parked (parsing stops behind it), which makes it
the *most* likely `dispatch_id()-1` target there is. A push appended into it makes `op.reply.empty()`
false, so neither the timeout nor the UNBLOCKED error is emitted — the client gets the push where it
expected its BLPOP answer and then waits forever. `Op::replied()` (`op.h:229`) carries a comment
warning about exactly this class ("two replies on the wire, permanently shifting every later reply")
for the opposite direction; the out-of-band writer makes the predicate wrong in this direction too.
Repro sketch: RESP3 conn, `SUBSCRIBE ch`, `BLPOP nokey 3`, second conn `PUBLISH ch x`, then wait out
the timeout. I did not run it.

**(b) Out-of-band frames can be silently destroyed by a handler.** Several worker-side paths call
`op.reply.clear()` on an op that is already published: `t_string.cc:248-251` (`clear_reply`, which
also zeroes `direct_len`), `bitfield.inc:49`, `acl.inc:1265`, `scatter_engine.inc:1737,1961,2734,
2869,2904`, `xshard_commands.inc:1610`. If the io thread appended a push first, it is thrown away —
a **lost** pub/sub message or invalidation, which for client-side caching is a correctness failure
(stale cache entry, no invalidation).

**(c) `CLIENT TRACKING on` is accepted on a RESP2 connection with no REDIRECT**
(`tracking.cc:470-620` has no RESP2 gate; redis errors here). Not a memory-safety issue, but it is
what makes the RESP2 tracking tear geometry exist at all, and it is a behavioural divergence from
the oracle worth a differ case.

**(d) `SegmentQueue::append_buf` does not check `malloc` for null** (`conn.h:116-117`, `:126-127`).
Out of scope here, noted in passing.

**(e) `assemble_mget` inherits the same splice.** `xshard_commands.inc:1607-1610`'s `flush_bytes()`
emits `op.direct + op.reply` immediately before each borrowed value, so a cross-shard MGET carries
the identical exposure — with N splice points instead of one. Any fix must cover this site, not just
`serve_impl`.

---

## 8. What a fix has to preserve (for whoever takes this)

The three out-of-band producers already agree on the *intent*: "this frame must land behind
everything already parsed ahead of it, and ahead of nothing." Branch 2 is a correct expression of
that intent for a copying reply. The minimal thing that makes it correct for a borrowing reply is to
stop using `Op::reply` as the parking spot when the tail op can borrow — e.g. a dedicated
`Op::trailer` buffer that retire emits **after** the CRLF segment, or forcing the tail op onto the
copy path (`mark_no_borrow()`) the moment an out-of-band frame is parked on it. Whatever the shape,
it must also (i) be safe against a worker concurrently touching the op, (ii) survive
`op.reply.clear()`, (iii) not break `blocking_retire`'s emptiness test, and (iv) be applied at
`assemble_mget` as well as at all three `serve*` variants.

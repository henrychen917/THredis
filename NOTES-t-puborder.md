# t-puborder notes

Static analysis only. Per `LANE_RULES.md`, this lane never builds, starts the server, or runs a
test/load generator. Baseline: `23364e8b1` (`t-puborder`, clean worktree).

## Premise and root cause

The premise is true. `Client::append_oob` currently sends every frame produced while the ROB is
busy through `segments_` (`src/net/conn.h`, OOB section). That is a safe frame boundary, but the
segment queue is the wire source ahead of replies which have not retired yet. An `SSUBSCRIBE` can
therefore finish its async pub/sub control, erase `pubsub_pending_`, and leave its acknowledgement
`Done` but unretired in the ROB. A delivery arriving in that window misses
`PubSubPending::deferred`, takes the segment route, and is written before the earlier-issued ack.

This is distinct from the P0 fixed by `953ca0774`: no out-of-band bytes may return to `Op::reply`.
A borrowed GET stages `[bulk header][borrowed value][CRLF]`; inserting in `op.reply` puts a push
after the header but before the body. Worker-side `op.reply.clear()` sites can also destroy such a
push, and `blocking_retire` uses reply emptiness to decide whether to synthesize its timeout or
UNBLOCKED reply.

## Chosen ordering model

An OOB frame records the connection's `rob.dispatch_id()` when it is produced. That exclusive
frontier names every command issued before the frame. The write-back engine may append the frame
only after retirement has completely staged replies through that frontier. It flushes at the end
of a drain, so a younger reply which is already ready in the same batch may also precede the frame;
that is safe, while a younger reply which stops the drain cannot hold it. Multiple connections and
multiple frontiers require per-connection queues; the existing one-string drain buffer cannot
safely represent them once deferral extends outside the one currently draining connection.

The latency bound is explicit: when a connection is marked blocked and its ROB head is still
`Issued`, the frame takes `Client::append_oob` immediately. A blocking command is a whole-connection
barrier, so that head is the only outstanding command and no partial reply bytes exist yet. Once
the head is `Done`, it is an in-flight reply rather than a parked wait, so ordinary frontier
deferral applies. A frame raised from inside the same connection's retire drain always defers,
regardless of `blocked()`, because that is the partial-staging window.

## Measurement surface

Reachable commands/features: pub/sub deliveries and subscription acknowledgements, client-tracking
invalidations, MONITOR feed, blocking commands on RESP3 subscribed clients, and replies sharing
those connections. Ordinary GET, SET, MGET, and MSET connections do not call an OOB producer;
their only source-level exposure is the existing predicted-empty deferred check in write-back.

Owner validation should retain the zero-regression GET/SET/MGET/MSET p1+p32 cells, plus pub/sub
delivery throughput and tracking/MONITOR under pipelining. Correctness geometry and exact static
line references are below.

## Implemented shape

Commit `f50fc96cf` makes both OOB producers call `WbEngine::defer_oob` before
`Client::append_oob` (`src/core/pubsub.inc:230-237`, `src/cmd/climon.cc:320-324`). The engine:

- immediately uses the existing append path for a quiesced client;
- immediately uses it for a blocked client whose ROB head is still `Issued`;
- otherwise records the current exclusive dispatch frontier in a per-client queue
  (`src/net/wb.h:132-147`);
- after each of the three drain variants, releases only entries whose frontier is no newer than
  the published ROB flush frontier (`src/net/wb.h:253`, `:625-637`, `:700`, `:755`); and
- erases any retained entries during connection teardown (`src/net/wb.h:553-556`).

The queue is per connection because one IO thread can own many clients with outstanding replies.
It is also per frontier because `A issued; frame 1; B issued; frame 2` must never release frame 2
after only A has retired. Frames at the same frontier coalesce into one owned string. Retirement
may place frame 1 behind a younger reply B when both retire in the same drain; that is safe and
preserves the existing drain-end behavior. If B is not ready, the drain stops after A and the
frontier check releases frame 1 there, so it cannot become stuck behind B. A blocking B cannot be
issued behind A at all: blocking dispatch requires an empty ROB and erects a parse barrier.

`Client::append_oob` remains the only release-to-output operation (`src/net/conn.h:435-445`). If
younger ops remain, it seals older fill bytes then appends an owned segment; if the ROB is now
quiescent and no segments exist, it appends after the retired reply in the fill buffer. Thus the
frame is never inserted into `Op::reply`, and neither worker `reply.clear()` sites nor
`blocking_retire`'s emptiness predicate can observe it. `Op` remains 336 bytes and `Client` remains
1984 bytes (`src/exec/op.h:313`, `src/net/conn.h:709`).

The INFO routes now mean:

- `oob_frames_deferred`: the frame waited for an earlier-issued reply or a drain's partial frame;
- `oob_frames_segmented`: the frame took the segment queue immediately, most importantly while a
  blocking ROB head remained parked and `Issued`.

## Validation geometry to run in the owner session

Nothing below was run in this lane.

1. Run `tests/pubsub.py` on the feature boot in both `--atomic 0` and `--atomic 1`. The standard
   eight-core gate geometry is four IO owners plus four executors with 16 shards; the churn arm has
   one synchronous `SPUBLISH` connection racing four subscriber threads, each creating 80 fresh
   connections on one shard channel. Half reset before reading, while half read exactly one frame.
   The new mechanism assertion at `tests/pubsub.py:398,435-438` requires
   `oob_frames_deferred` to advance, proving at least one delivery landed after async subscription
   control completed but before its reply retired. Every reading connection requires its first
   frame to be `[ssubscribe, channel, 1]` (`:419-421`), so the delivery cannot overtake the ack.
   The reported `churn ack 2/13` is worker 2, zero-based iteration 13; it is a reading iteration
   because `(2 + 13) % 2 == 1`. The observed `smessage ... 34` was the incorrectly leading frame.

2. Run the inherited base 18-cell `tests/pushtear.py` battery in both atomic modes (and its optional
   TLS arms where available), plus the small `tests/push_tear_repro.py` probe. This is the other
   ordering assertion: a strict incremental RESP parser requires every frame boundary and compares
   each GET bulk body byte-for-byte. Use a value exactly at or above `zc-min` (default geometry:
   16,384-byte value with `zc-min=16384`; forced geometry: 64-byte value with `zc-min=1`). The GET
   then borrows and stages `[bulk header][borrow][CRLF]`; this is the geometry that tore. A value
   below `zc-min`, or `zc-min=0`, copies the body and is only the negative control. Require
   `zc_sends` and one of the OOB counters to advance so a clean parse cannot be vacuous.

3. Keep both properties in the same verdict. The churn assertion alone passes the pre-953 tree
   that could splice a push inside borrowed bytes. The strict borrowed-body parser alone passes the
   post-953 regression that put a whole delivery before an earlier ack. Both must be green.

4. Run `cell_blocking_reply_survives` as part of pushtear. It uses two connections: a RESP3
   subscribed client parked on `BLPOP nokey 1`, and a publisher delivering while that head is
   `Issued`. It must receive both the push and the eventual null reply, and the strengthened route
   assertion requires `oob_frames_segmented` to advance. That proves the push was not deferred
   until the blocking timeout while retaining the swallowed-reply check.

## Measurement surface, final

Commands which can change behavior: `SUBSCRIBE`/`PSUBSCRIBE`/`SSUBSCRIBE` deliveries and their
control acknowledgements, `PUBLISH`/`SPUBLISH` when the publisher is also a target, CLIENT TRACKING
invalidations, MONITOR feed, and blocking replies on such OOB-enabled RESP3 connections. The
implementation also covers borrowed GET and assembled MGET replies sharing those connections.

Ordinary GET, SET, MGET and MSET connections never enter either OOB producer. Their write-back path
retains one predicted-empty deferred-map test per served connection, matching the previous
predicted-empty string test. Measure GET/SET/MGET/MSET p1+p32 as the hard zero-regression gate;
measure pub/sub delivery rate and tracking/MONITOR under pipelining as the affected throughput and
latency surface. Client and Op footprints are unchanged; only one WbEngine per IO thread gains the
per-client cold map.

# RESP2 tracking invalidation audit

## Reference behavior, from the pinned source

The reference for this lane is Redis 7.4.2 as read with
`git show 7.4.2:src/tracking.c` and `git show 7.4.2:src/networking.c` in
`/home/user/Projects/redis`. No server was started.

`CLIENT TRACKING on` is valid on a RESP2 connection without `REDIRECT`.
`networking.c:3354-3477` parses and enables tracking without testing `c->resp`, and
`tracking.c:157-193` (`enableTracking`) has no protocol check either. Rejecting the command would
therefore introduce a different divergence.

`sendTrackingMessage` first resolves a configured redirect (`tracking.c:255-280`) and then decides
from the protocol and pub/sub state of the **resolved delivery target** (`tracking.c:282-300`). Its
2x2 behavior is:

| resolved delivery target | no `REDIRECT` | `REDIRECT` present and target exists |
|---|---|---|
| RESP2 | **No frame.** RESP2 has no same-connection push channel. | A normal `message` on `__redis__:invalidate` only if the target has `CLIENT_PUBSUB`; otherwise **no frame**. |
| RESP3 | A real `>2 ... invalidate` push. | A real `>2 ... invalidate` push; subscription state is irrelevant. |

The original tracking connection receives no invalidation when a redirect resolves. If the redirect
no longer resolves, Redis marks it broken and emits `tracking-redir-broken` only when the original
tracking connection is RESP3 (`tracking.c:260-274`); RESP2 remains silent.

The reference sends per-key invalidations through this same decision from
`trackingInvalidateKey` (`tracking.c:353-410`), deferred self-invalidations from
`trackingHandlePendingKeyInvalidations` (`tracking.c:412-438`), flush invalidations from
`trackingInvalidateKeysOnFlush` (`tracking.c:455-469`), and BCAST invalidations from
`trackingBroadcastInvalidationMessages` (`tracking.c:583-620`). Thus the matrix is not limited to
ordinary `SET` invalidations.

## TomoKV emission paths before this change

Line references in this pre-change subsection name parent commit `953ca0774`; current post-change
delivery references are recorded under “Implemented result” below.

Every path that can ultimately emit an invalidation is:

1. Tracking-table pressure evicts an entry and calls `tracking_deliver_frame`
   (`src/cmd/tracking.cc:116-132`).
2. Ordinary command mutations are gathered at retirement (`src/cmd/notify.inc:548-571`), enter
   `tracking_broadcast_keys` (`src/cmd/tracking.cc:277-309`), and are filtered/delivered by
   `tracking_invalidate_local` (`src/cmd/tracking.cc:249-274`). A remote tracking owner receives the
   same work as `TrackingInvalidate` at `tracking.cc:338-345`.
3. Keyless expiry/eviction notifications construct `TrackingInvalidate` directly
   (`src/cmd/notify.inc:437-465`), then take the same `tracking.cc:338-345` local filtering path.
4. `FLUSHALL`/`FLUSHDB` enter `tracking_broadcast_flush` from the armed dispatch gate
   (`src/cmd/climon.cc:268-293`). Local targets are delivered at `tracking.cc:312-326`; remote owners
   receive `TrackingFlush` and deliver at `tracking.cc:346-357`.
5. BCAST prefix matches and remembered-key matches both converge at `tracking_deliver_frame`
   (`tracking.cc:249-274`). A redirect on another IO owner is carried by `TrackingDeliver`
   (`tracking.cc:239-245`) and emitted at `tracking.cc:360-369`.

All actual invalidation bytes are built by `tracking_emit_invalidation`
(`tracking.cc:176-197`) and then passed to `climon_push_wire`. The redirect event is only an IO-to-IO
transport envelope: invalidations do **not** use `pubsub_emit` or its subscriber index. This matters
for the missing subscription check. Both final emitters do preserve the out-of-band ordering
invariant: `climon_push_wire` (`src/cmd/climon.cc:313-328`) and `pubsub_emit`
(`src/core/pubsub.inc:208-241`) use `defer_oob`/`append_oob`, never `Op::reply`, so a whole frame lands
on a reply boundary.

The pre-change TomoKV behavior is:

| resolved delivery target | no `REDIRECT` | `REDIRECT` present and target exists |
|---|---|---|
| RESP2 | **Wrong:** `tracking.cc:203-207` calls the RESP2 encoder and emits a bare `*3 message` array even though the connection is not subscribed. | Correct only for an actually subscribed RESP2 target. `tracking.cc:228-237` and `360-369` otherwise emit the same frame without checking subscription state. |
| RESP3 | Correct real push via `target->resp3()` at `tracking.cc:203-207`. | **Wrong:** both the local redirect (`tracking.cc:236`) and remote redirect (`tracking.cc:365-368`) force RESP2 encoding. |

Therefore the belief that all RESP3 and redirect-present cells were already correct is only partly
true: RESP3/no-redirect and RESP2/redirect-to-subscribed-target are correct. RESP2/no-redirect,
RESP2/redirect-to-unsubscribed-target, and RESP3/redirect are divergent. The narrow delivery rule
must be applied after resolving the target: suppress RESP2 without redirect, suppress a redirected
RESP2 target that is not in subscriber mode, and otherwise encode from the resolved target's
protocol.

## Change and measurement surface

The intended change is confined to the already-armed invalidation delivery path. It must not change
`CLIENT TRACKING` grammar/state, key registration, mutation observation, RESP3 push bytes, redirect
lookup/broken-redirect handling, or the `climon_push_wire` out-of-band route.

Commands whose armed behavior can be touched are `CLIENT TRACKING`, tracked reads (including GET
and MGET), mutating commands that invalidate remembered/BCAST keys (including SET and MSET), and
`FLUSHALL`/`FLUSHDB`. With tracking off, the path is unreachable. The main session still owes the
LANE_RULES zero-regression cells for GET, SET, MGET and MSET, plus an armed tracking delivery cell;
the fix should remove encoding/enqueue work in silent cells and add no disabled-path work.

## Validation geometry to run elsewhere

No validation is run in this lane. A directed check must cross:

- resolved delivery protocol: RESP2 / RESP3;
- redirect: absent / present;
- for a present redirect, target subscription: subscribed / unsubscribed;
- delivery ownership: one IO thread for the local redirect hop, and multiple IO threads with a
  proven different-owner target for `TrackingDeliver`;
- invalidation source: at least an ordinary remembered-key write, with flush as a second source
  because it carries a null payload.

Each ordinary probe needs a tracking connection, a separate writer, and (for redirect cells) a
third target connection. It should seed a key, enable tracking, read the key to register it, mutate
from the writer, and inspect the resolved target. The RESP2/no-redirect cell must observe an empty
socket and then receive the exact reply to a following command, proving no stale array became that
reply. That negative result is non-vacuous only when the same producer/key script also sees the
expected RESP3 push as a positive control. Likewise, redirected RESP2 must see no frame while the
target is unsubscribed and the exact `__redis__:invalidate` message after a real subscription
acknowledgement; redirected RESP3 must see the same push both with and without a subscription.

The multiple-IO run must expose or otherwise prove the connection-owner assignment and fail loudly
if it cannot construct a cross-IO redirect. Key placement is not material to this delivery defect;
if a multi-shard variant is added, it must discover and bucket keys with `DEBUG SHARD` and fail when
the requested same-owner/cross-owner key geometry cannot be found. Suggested server geometries are
one IO/one executor/one shard for deterministic local delivery, then at least two IO threads/two
executors/two shards for the transport path. The check must also assert that the
`tracking_invalidations` counter advances in positive cells and does not advance for suppressed
delivery, so a producer that never fired cannot masquerade as success.

## Implemented result

`tracking_emit_invalidation` now receives whether the already-resolved delivery is redirected and
applies the reference gate before constructing a frame (`src/cmd/tracking.cc:176-180`). It derives
the encoding from the resolved target: RESP3 continues through the unchanged push encoder; RESP2
continues only for a redirect target whose IO-owned `subscriber_mode()` is set. That bit is derived
from a non-zero real subscription count when pub/sub modification completes
(`src/core/pubsub.inc:579-603`; accessor at `src/net/conn.h:529-530`).

No-redirect delivery explicitly passes `redirected=false` (`tracking.cc:205-212`). Both redirect
routes pass `redirected=true`: the local target at `tracking.cc:233-242`, and the target owner after
a `TrackingDeliver` hop at `tracking.cc:365-374`. Frames that pass the gate still end at
`climon_push_wire` (`tracking.cc:201`), so none enter `Op::reply` and the PUSHTEAR reply-boundary fix
remains intact. Suppressed cells do not encode, enqueue, or increment `tracking_invalidations`.

`tests/tracking.py` now uses one mutation of a key registered by both RESP2 and RESP3 no-redirect
clients: the RESP3 push is the positive control for the RESP2 empty socket, followed by an exact
`PONG` synchronization check. It also tests a RESP2 redirect target before and after a confirmed
subscription, a RESP3 redirect target before and after subscription, and RESP2/RESP3 flush delivery.
`tests/pushtear.py` no longer expects the invalid RESP2/no-redirect tracking frame; its RESP2
out-of-band coverage remains in the protocol-agnostic MONITOR cells. These checks were described
and edited only. Per `LANE_RULES.md`, none were run, and no build or server was started.

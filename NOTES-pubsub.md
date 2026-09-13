# IO-owned RESP2 pub/sub

## Architecture

- `hash(channel) % n_io` selects the channel home from the placement's IO-thread list. The home
  IO exclusively owns the regular and shard-channel `subscriber -> (owning IO, connection id)`
  indexes. The two indexes are separate namespaces.
- SUBSCRIBE/UNSUBSCRIBE requests are IO-to-IO events. Pattern subscriptions are replicated into a
  per-home pattern list, so a PUBLISH still visits only the channel home and matches that home's
  patterns with the existing Redis-compatible glob matcher.
- PUBLISH runs at the channel home. It makes an owned event copy for each exact or pattern delivery
  and routes it to the connection's owning IO. The owning IO alone appends RESP2 `message` or
  `pmessage` frames to the connection send path.
- SPUBLISH uses the same channel-home routing and delivery path, but consults only the shard index,
  never the regular exact or pattern indexes, and emits RESP2 `smessage` frames. SSUBSCRIBE accepts
  channels on multiple homes in one command. Deliberately, this is a no-slot superset of Redis:
  there is no hash-slot validation, key-slot routing, or CROSSSLOT failure.
- The payload inbox is cold until pub/sub is used. One `nullptr` token in the existing `client_in`
  IO-to-IO channel wakes and drains a whole inbox burst, retaining the established notify-mask and
  park/wake protocol. No pub/sub queue or index is scanned by the plain IO loop.
- Async command replies keep their ROB slot live until every required home answers. This closes the
  acknowledgement race: after a SUBSCRIBE acknowledgement is observable, every home required for
  that subscription has installed it. The connection's existing scatter barrier prevents younger
  pipelined commands from crossing the async pub/sub command.
- Spontaneous deliveries name connections by owning IO plus process-unique connection id, never by
  a cross-thread `Client*`. The owner either appends them behind the newest local reply or defers
  them into the pending subscription command, preserving RESP wire order.

The owner rule remains literal: "db shards need to only be touched by their ex threads." Pub/sub
does not route through, read, or mutate a shard. `sizeof(Op)==336` and `sizeof(Client)==1984` remain
compile-time locks; the subscriber-mode bit consumes existing `Client` alignment padding.

## Lifecycle and reporting

Disconnect sends an idempotent cleanup request to every home IO. The owning IO keeps the client
alive until all cleanup acknowledgements return, then releases the connection through the existing
ROB/kernel-reference fence. This also covers disconnects while SUBSCRIBE or PUBLISH is in flight.

`INFO STATS` exposes the churn invariants:

- `pubsub_channels`: exact channels with at least one subscriber
- `pubsub_subscriptions`: logical exact subscription count
- `pubsub_patterns`: logical pattern subscription count
- `pubsubshard_channels`: shard channels with at least one subscriber
- `pubsubshard_subscriptions`: logical shard subscription count
- `pubsub_home_entries`: physical exact plus replicated-pattern home entries
- `pubsub_inflight`: heap-owned IO event count
- `pubsub_pending_commands`: async command count

All eight return to zero after the directed churn test. CLIENT INFO/LIST reports `ssub=N` from
out-of-line client metadata, preserving the locked Client footprint. Regular acknowledgement
counts remain `channels + patterns`; shard acknowledgements count only shard channels. Their total
controls entry to and exit from subscriber mode.

## Surface and tests

Implemented commands include SSUBSCRIBE, SUNSUBSCRIBE, SPUBLISH, PUBSUB SHARDCHANNELS, and PUBSUB
SHARDNUMSUB alongside the regular pub/sub surface. PUBSUB HELP advertises both shard introspection
arms; there is intentionally no SHARDNUMPAT. RESP2 subscriber mode permits the Redis command set:
all six subscription controls, PING, QUIT, and RESET. PING uses subscribed-mode array framing;
RESET unregisters every exact, pattern, and shard arm before leaving subscribed mode.

The owner-only transaction engine cannot execute the asynchronous home protocol as an EXEC child,
so all six subscription controls are rejected in MULTI. This matches Redis for the two shard
controls and is an explicit compatibility divergence for the four regular controls.

Run the directed test against an already-running server:

```sh
taskset -c 252-255 tests/pubsub.py 127.0.0.1 7954
```

It covers regular and shard 24-subscriber ordered fanout, namespace isolation, exact and multiple
pattern delivery arms, subscriber-mode restrictions, RESET, named/all unsubscribe variants,
CHANNELS/NUMSUB/NUMPAT, SHARDCHANNELS/SHARDNUMSUB, multi-home shard subscriptions, and 320 abrupt
shard subscribe/disconnect cycles concurrent with 500 shard publishes. It waits for every pub/sub
lifecycle gauge to drain to zero.

Validation completed on the assigned partition:

```text
release build                         pass
sizeof(Op) / sizeof(Client)           336 / 1984 bytes
ASAN directed pub/sub test            pass
tests/pubsub.py, atomic=0 and 1        pass
multi/blocking/pubsub/lua, atomic=0/1 pass
spubsub Redis 7.4.2 differential      816 checks, 0 diffs
SET loopback instructions/op          5172.224453 -> 5161.844956 (-10.379497)
GATE_PORT=7954 GATE_CORES=252-255 tests/gate.sh quick (2:2 topology)
                                      17 ok, 0 FAIL
```

## Publishing performance pass (CPUs 240-247)

The publishing profile supersedes the per-delivery transport description above. An empty publish
was still allocating a pending command and crossing to the channel home for a guaranteed zero.
Subscribed fanout allocated, framed, and posted one heap event per subscriber per message; a
10-subscriber publish therefore made ten delivery events, with one cross-thread post for every
subscriber not owned by the channel-home IO.

There was no extra sharded routing arm: PUBLISH and SPUBLISH select separate indexes at the same
home, and SPUBLISH skips the regular pattern walk. The initially reported namespace gap inverted
on fresh boots. It was accept/channel-home placement: a single publisher accepted by its channel
home takes the local arm, while a publisher on the other IO pays the request/result round trip.
That placement effect is inherent to the benchmark topology, not to the sharded namespace.

The new path makes the following changes while retaining the channel home as the sole ordering
authority:

- A relaxed active-channel load at the publishing IO returns zero before channel/message copies,
  pending state, allocation, or a home hop when the selected namespace has no receivers.
- The home builds one delivery event per destination IO. Its existing `items` vector carries all
  exact and pattern targets, so `PubSubEvent`, `Op`, and `Client` retain their locked sizes.
- Home-owned subscribers are delivered inline. One remote delivery batch carries the publish
  completion fence, and pipelined publishers retire in bounded groups of 10 regular or 11
  exact-only shard messages. This preserves socket-service fairness instead of reporting a publish
  rate that has merely queued an undrained subscriber backlog.
- Independent ROB slots replace the connection-wide PUBLISH pending record. Same-producer requests
  still enter every home FIFO in parse order, while replies may complete their ROB slots out of
  order and retire on the wire in order.

Clean PRE, Redis 7.4.2, and final POST measurements used a sole listener, server CPUs 240-243 with
`--ratio 2:2`, client CPUs 244-247, port 7951, and pipeline 32. Fanout figures are two-run medians;
both runs delivered 100% of the ten-subscriber ideal.

| cell | PRE | Redis 7.4.2 | POST | POST/Redis |
|---|---:|---:|---:|---:|
| SPUBLISH, no subscribers | 393,825/s | 1,798,561/s | 2,191,061/s | 1.22x |
| PUBLISH, no subscribers | 325,521/s | 1,602,051/s | 2,192,983/s | 1.37x |
| PUBLISH, 10 subscribers | 17,712/s | 66,665/s | 68,837/s | 1.03x |
| SPUBLISH, 10 subscribers | 28,364/s | 66,679/s | 71,183/s | 1.07x |

Final validation on the same slice:

```text
release build / sizeof locks             pass (Op 336, Client 1984)
tests/pubsub.py                           pass (24-way regular/shard, order, churn)
tests/notify.py                           pass (1617 events fired)
spubsub differ, atomic=0 and 1            801 checks each, 0 diffs
notify differ, atomic=0 and 1             301 ops / 443 events each, 0 diffs
GET+SET p32 server instructions/op        1980.353120 -> 1980.847240 (+0.494120)
GATE_PORT=7951 GATE_CORES=240-247 quick   46 ok, 0 FAIL
```

The instruction comparison counted all five server TIDs for a fixed 20 million loopback commands;
the A/B pair used the same 2:2 placement. Runs with unmatched `SO_REUSEPORT` connection placement
were excluded because local-owner versus remote-owner routing changes the reference itself by
about ten instructions per command.

# Lane S — pub/sub fanout: parallelize the delivery side

## Results

Server CPUs 24-27, `--ratio 2:2 --shards 16`; load CPUs 28-31; loopback. Redis 7.4.2 on the same
server CPUs (single-threaded, so it uses one of them). Publish cells are one pipelined publisher
against N subscribers on one channel; empty-channel cells are memtier `-c 8 -t 2 --pipeline=32`.
Every figure is the median of 3 × 4 s runs. INDICATIVE: loopback, lane cores, not the NIC rig.

### Publish rate (publishes/s)

| cell | PRE (head d177ea9cf) | POST | redis 7.4.2 | POST/PRE | POST/redis |
|---|---:|---:|---:|---:|---:|
| PUBLISH, no subscribers, p32  | 3,251,439 | 3,198,832 | 1,599,733 | 0.98x | **2.00x** |
| SPUBLISH, no subscribers, p32 | 3,227,783 | 3,191,923 | 1,817,881 | 0.99x | **1.76x** |
| PUBLISH @1 sub    |   402,977 | 1,397,866 | 1,265,212 | 3.47x | **1.11x** |
| PUBLISH @10 subs  |   210,533 |   708,371 |   416,339 | 3.36x | **1.70x** |
| PUBLISH @64 subs  |    46,897 |   195,471 |    78,836 | 4.17x | **2.48x** |
| SPUBLISH @1 sub   | 1,335,292 | 1,711,682 | 1,365,201 | 1.28x | **1.25x** |
| SPUBLISH @10 subs |   243,006 |   611,993 |   430,445 | 2.52x | **1.42x** |
| SPUBLISH @64 subs |    49,030 |   202,837 |    77,869 | 4.14x | **2.60x** |

### Delivered frames/s — the quantity the fanout redesign actually moves

| cell | PRE | POST | redis 7.4.2 | POST/redis |
|---|---:|---:|---:|---:|
| PUBLISH @10 subs  | 2,105,334 |  7,083,711 | 4,163,390 | 1.70x |
| PUBLISH @64 subs  | 3,001,379 | 12,510,154 | 5,045,480 | 2.48x |
| SPUBLISH @10 subs | 2,430,058 |  6,119,935 | 4,304,447 | 1.42x |
| SPUBLISH @64 subs | 3,137,924 | 12,981,541 | 4,983,630 | 2.60x |

Every run delivered 100.0% of ideal (`publishes × subscribers`), byte-checked against the expected
frame, so none of this is a publish rate that merely queued an undrained backlog.

**Scaling @10 → @64 is the brief's bar and it is met.** Delivered frames/s *rises* 7.08M → 12.51M
(+77%) as subscribers go 6.4x — fanout parallelism, not a 1/N collapse. Redis on the same cells
goes 4.16M → 5.05M (+21%), because its fanout is one thread by construction.

### Protected cells (interleaved A/B, 5 reps × 5 s, same boot recipe)

| cell | PRE | POST | delta |
|---|---:|---:|---:|
| PUBLISH empty-channel p32 | 3,162,428 | 3,156,633 | **−0.18%** |
| GET/SET 1:9 p32 (`-d 64`) | 2,497,058 | 2,491,818 | **−0.21%** |

Both deltas sit inside fully overlapping rep spreads (empty-channel PRE 3.152–3.169M vs POST
3.150–3.171M). Zero-cost-when-off holds: the hot path gained exactly one predicted-not-taken
branch on a bool, with all machinery out-of-line.

### Saturation — read the POST fanout numbers as a LOWER bound

| arm | server IO threads | load generator (4 cores) |
|---|---|---|
| PRE @10 subs  | 94% / 84% — **server-bound** | not saturated |
| POST @10 subs | 62% / 86% | 339% of 400% — **load-generator-bound** |
| POST @64 subs | 74% / 89% | 379% of 400% — **load-generator-bound** |

PRE was a true server measurement. POST is not: the Python load generator runs out of cores first.
The POST fanout cells are therefore a floor on what the server can do, not a ceiling.

## The measurement had to be fixed before anything could be believed

The brief's premise — "PUBLISH @10 subs 70.7k/s vs redis 66.2k, SPUBLISH @10 subs 64.0k ≈ redis" —
was **a harness artifact, not a server result.** `tests/benchfeat.py`'s subscriber parsed every
delivery with `readline()`/`read()` per RESP element, ~1.4 µs of CPython per frame. Ten subscriber
threads under one GIL therefore ceiling out near 750k deliveries/s, and *both servers measured that
same ceiling*. Rewriting the subscriber to count bytes (all frames on this bench are byte-identical,
so messages = bytes ÷ frame length; one `recv` per ~64 KiB, and `recv` drops the GIL) and spreading
subscribers over 4 processes moved the ceiling ~30x. On the unmodified server the same cell
immediately read 175k publish/s instead of 75k — and redis read 403k, i.e. tomo was **2.3x slower
than redis at @10 subs**, not at parity. Everything below is measured against that corrected view.

`tests/benchfeat.py` now takes `[nprocs] [npubs]`, verifies the first frame each subscriber receives
byte-for-byte, and prints `FRAME-MISMATCH` / `LOSS` markers so a fast-but-wrong result cannot be
mistaken for a fast one.

## What was actually costing the time

With the harness honest, a `perf record` of the two IO threads at @10 subs was **84% kernel, 11.7%
tomokv user code** — flat, with `tcp_sendmsg_locked` / `__tcp_transmit_skb` / `tcp_write_xmit` on
top. The fanout was not compute-bound; it was drowning in small sends. Two causes:

1. **A fixed publish in-flight cap.** `pubsub_start_publish` armed a scatter barrier once a
   connection had 10 (regular) / 11 (shard) publishes outstanding. That forced a full home round
   trip every ten messages, which starved output coalescing: each subscriber got a send per ~10
   frames where redis gets one per pipeline. Removing that cap alone was worth **3.1x** at @10.
2. **One event, one post, one wake decision, per publish per destination.** A pipelined publisher
   produced hundreds of independent `PubSubEvent` allocations and mutex-guarded inbox pushes,
   each carrying its own copy of the channel and payload strings.

## Design

1. **Encode once.** A publish is encoded exactly once at the channel home into a refcounted
   `PubSubBlob` shared by every owning IO thread. The RESP3 push frame is byte-identical to the
   RESP2 array frame except for the leading `*` → `>`, because both element counts (3 for
   `message`/`smessage`, 4 for `pmessage`) are single digits — so **one encoding serves both
   protocols**, appended as two pieces so the payload is never copied again. `pmessage` reuses the
   same blob's `$<len>\r\n<channel>...` tail verbatim behind its own 4-element header (`body_off`).

2. **Scatter by owner, batched per pass.** The home groups resolved targets into a per-destination
   `PubSubOutbox` and posts **one `DeliveryBatch` event per destination IO per pass**, carrying the
   blob pointers plus that owner's connection-id slice. Each owner appends its whole burst into its
   own clients' buffers and then arms one send each. Publisher cost per pass is O(#IO owners
   touched), not O(#subscribers) per publish.

3. **The pass boundary is the batching window.** `pubsub_start_publish` no longer drains inline;
   `flush_ready` calls `pubsub_pass_flush()` **between parsing (PHASE 1) and serving (PHASE 2)**, so
   everything a pass parsed — across every publisher connection on that thread — resolves into one
   batch per destination before a single byte is sent. Measured: **717 deliveries per posted event**
   under the bench, and 7–9 posted events for 600 publishes × 12 subscribers in the directed test.

4. **Refcount discipline.** The builder holds one reference; each queued delivery takes one; the
   destination drops one after appending; `pubsub_delete_event` and `pubsub_shutdown_events` drop
   any that never landed. A zero-receiver publish frees the blob on the spot. `pubsub_blobs` is an
   INFO gauge that must return to zero — asserted in the directed test and at shutdown, and run
   under ASAN including a mid-fanout RST arm.

5. **Ordering is a two-phase flush.** Deliveries for *every* destination are posted before *any*
   publish reply is. This replaces the old per-publish `DeliveryReply` fence and preserves the
   contract that matters: a publisher that waits for its reply before publishing again cannot have a
   later message overtake an earlier one at a shared subscriber, even across channel homes. An
   owner's inbox is one totally ordered queue, so *posted* suffices; it need not be processed.
   Cost is one extra event per pass when a destination owes both deliveries and replies.

### Subscriber registry placement (the brief asked for a choice, documented)

**Kept: channel-home ownership** (`hash(channel) % n_io`), *not* per-IO-owner membership lists. The
brief suggested each IO thread own its own clients' memberships, with publishes doing a racy-read
snapshot or an owner-side filter. That is the wrong trade here: it makes every publish visit *every*
IO thread to discover whether it has any subscriber at all — O(n_io) hops per publish, paid even by
channels with no subscribers, which is exactly the 2.0x-over-redis empty-channel cell this lane must
protect. Channel-home placement keeps a publish to **one** hop and keeps each channel's membership
single-writer (its home), which is what makes the delivery order per channel well-defined. The
fanout problem the brief was really pointing at is on the *delivery* side, and that is what the
per-owner batch fixes.

### Backpressure after deleting the cap

Removing the in-flight cap leaves redis's own two mechanisms, which the tree already had with
redis's exact grammar and defaults: the ROB window (64) bounds per-connection in-flight publishes,
and `client-output-buffer-limit pubsub 32mb 8mb 60` bounds the subscriber side. **No new knob was
invented** — knob-compat rule. Because that limit is now load-bearing rather than a backstop, the
directed test proves it fires: a subscriber that never reads is flooded past a 1 MiB hard limit and
must be disconnected with `client_output_buffer_limit_disconnections` moving, or the test fails.

### Known ordering limit (pre-existing, now written down)

A **pipelined** publisher spanning **several channel homes** has no cross-channel delivery order,
because independent homes post into an owner's inbox concurrently. This is true of the previous
design too (up to 10 publishes were in flight there). A single channel is strictly ordered by its
single home in both designs, pipelined or not. Both contracts are covered by the differ suite,
which tests them as separate arms rather than assuming either.

## Surface completion

`PUBSUB CHANNELS [pattern] / NUMSUB / NUMPAT / SHARDCHANNELS / SHARDNUMSUB` were **already
implemented** — verified against the live binary before writing anything, per the verify-before-
implementing rule. What was actually missing was correctness against the oracle:

- **`PUBSUB NUMPAT` was wrong** — a real bug, not a formatting nit. It returned
  `pubsub_patterns_.size()`, the number of pattern *subscriptions*; redis returns the number of
  *distinct* patterns with at least one subscriber. Five clients on two patterns reported 5 vs
  redis's 2. Found by the new differ suite (22 diffs), fixed, now 0. The existing directed test did
  not catch it because it used two distinct patterns on one connection, where both counts agree.
- Four error-shape divergences, all now byte-identical to redis 7.4.2:
  `PUBSUB <bogus>` → `ERR unknown subcommand '<bogus>'. Try PUBSUB HELP.` (echoing the caller's
  case); `PUBSUB CHANNELS a b` → `ERR unknown subcommand or wrong number of arguments for
  '<token>'. Try PUBSUB HELP.`; `PUBSUB NUMPAT x` → `ERR wrong number of arguments for
  'pubsub|numpat' command`; `PUBSUB HELP x` → `... 'pubsub|help' command`.

## Knobs

**None added.** The change deletes a hard-coded constant (`kPubSubPublishBatch` = 10 /
`kPubSubShardPublishBatch` = 11) rather than making it tunable: the gain is consistent across every
cell measured, so hard-code-or-delete says delete it. Backpressure uses the existing redis-compatible
`client-output-buffer-limit pubsub`, already documented in `tomokv.conf`.

New INFO STATS fields (reporting only, never read by the key path):

| field | meaning |
|---|---|
| `pubsub_blobs` | live encode-once blobs; a lifetime gauge that must drain to zero |
| `pubsub_deliveries` | total frames appended to subscribers |
| `pubsub_delivery_batches` | delivery events posted between IO threads |

`pubsub_deliveries / pubsub_delivery_batches` is the batching proof: a ratio near 1.0 means the
scatter machinery is back to one event per delivery, which is the thing this design removed.

## Test evidence

Directed battery — `tests/pubsub.py`, extended with six fanout arms, each proving its mechanism
fired rather than that nothing broke:

- **A** 4 concurrent pipelined publishers × 150 messages × 12 subscribers: every subscriber's
  per-publisher sequence must be strictly `0..149` with no loss, duplicate or reorder.
- **B** batching: `pubsub_deliveries` delta must equal `4×150×12 = 7200` exactly, and posted batch
  events must be **fewer than the publish count** — a per-delivery scatter cannot reach that ratio.
- **C** RESP2 and RESP3 subscribers on one publish, asserting the literal `*3` vs `>3` header bytes
  and the `>4` `pmessage` push — the arm that proves the one-encoding header swap.
- **D** mid-fanout teardown: 8 of 16 subscribers RST while a 400-deep pipelined burst is in flight,
  so queued batches name dead connections; the 8 survivors must still receive all 400 in order.
- **E** backpressure: `client-output-buffer-limit pubsub` must disconnect a non-reading subscriber.
- **F** introspection aggregates byte-compared against a Python-side model of the population.

```text
release build (+ sizeof(Op)==336 / sizeof(Client)==1984 locks)   pass
tests/pubsub.py         --atomic 0   PASS (concurrent_pub=4x150x12, batches=8 for 600 publishes)
tests/pubsub.py         --atomic 1   PASS (concurrent_pub=4x150x12, batches=7 for 600 publishes)
tests/notify.py         --atomic 0   ok (notify_events_fired=1613)
tests/notify.py         --atomic 1   ok (notify_events_fired=1619)
tests/resp3.py          --atomic 0/1 140 checks -> PASS
tests/multi_exec.py     --atomic 0/1 passed
tests/blocking.py       --atomic 0/1 PASS
tests/climon.py         --atomic 0/1 ok

differ.py fanout  seed 11 --atomic 0   13440 checks, 0 diffs -> PASS
differ.py fanout  seed 11 --atomic 1   13440 checks, 0 diffs -> PASS
differ.py fanout  seeds 3 / 29 / 101    9115 / 10981 / 9140 checks, 0 diffs -> PASS
differ.py fanout  seed 7, RESP3 (-3)   14237 checks, 0 diffs -> PASS
differ.py spubsub seed 5  --atomic 0/1   795 checks, 0 diffs -> PASS
differ.py notify  seed 5  --atomic 0/1   330 ops / 469 events, 0 diffs -> PASS

ASAN build (-fsanitize=address, ldd-verified instrumented)
  tests/pubsub.py                       PASS
  tests/notify.py                       ok (notify_events_fired=1604)
  tests/resp3.py                        140 checks -> PASS
  differ.py fanout seed 11              13440 checks, 0 diffs -> PASS
  AddressSanitizer diagnostics in log   0
  pubsub_blobs / pubsub_inflight at shutdown   0 / 0
```

New differ suite `tests/differ.py fanout`: 9 subscriber pairs (mixed RESP2/RESP3, exact + pattern +
shard), one publisher pair, 220 rounds of pipelined single-channel bursts, sequential multi-channel
publishes, subscription churn and the full introspection surface. Every delivered frame is
byte-compared against redis in order; frame counts come from a Python-side model rather than
`select()`, so a lost, extra or reordered frame misaligns the comparison immediately instead of
timing out. A final sentinel publish proves no server leaked an unaccounted frame earlier.

## Scope notes

- **Nothing cut.** The one item that changed shape was "implement the introspection gaps": there
  were no gaps — the surface existed — so that budget went into the `NUMPAT` semantics bug and the
  four error-shape divergences the oracle exposed.
- The brief cites 5.33M/s for the empty-channel cell; this lane measures 3.19–3.25M/s for both PRE
  and POST on 2 IO threads / 4 cores with memtier p32. The absolute number is topology-dependent;
  what matters for the protected result is that PRE and POST agree to −0.18%, measured interleaved.
- `@1`-subscriber cells swing several fold run to run because `SO_REUSEPORT` decides whether the
  single publisher lands on its channel home or one hop away. Medians of 3 are reported and the
  individual reps are in the lane log; do not read a single `@1` run as a signal.
- A leaked server from an earlier boot was caught squatting the lane port and splitting connections
  via `SO_REUSEPORT` (24 subscribers reported as 16). Boots in this lane now go through a guard that
  refuses to start if anything already listens and reports the pid that actually owns the listener,
  because `$!` after `taskset` is not that pid.

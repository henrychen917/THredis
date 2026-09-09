# Thread modes

`--thread-mode 2s|1s` selects TomoKV's thread architecture at boot. The default is `2s`;
`split` and `fused` remain accepted aliases. `INFO Server` reports the actual `thread_mode`,
`shards`, `overlap`, `reorder`, thread counts, effective `read_local`, and live `atomic` state. These observations
let a run assert its resolved geometry. `CONFIG GET` reports the retained configuration;
thread mode, shards, overlap, reorder, and local-read admission are immutable after boot.

`--overlap 0|1` defaults to `0`. In `2s`, 1 overlaps IO writeback. In `1s`, 1 selects
the gated three-way schedule formerly numbered 2, and requires `--net-io uring`.
Both settings in both modes support local reads. [ORTHOG.md](../ORTHOG.md) records the
integration evidence; [REORDER.md](../REORDER.md) records the repaired scheduler bounds.

`--reorder 0|1` (default `0`) reorders across connections in an executor batch for latency; per-connection order is always preserved.
It works in both modes and either overlap setting. Off keeps FIFO and allocates nothing;
on uses head rank then static command cost, retaining FIFO for single-connection runs.
Special tasks remain barriers. CONFIG exposes immutable `reorder`; [REORDER.md](../REORDER.md)
documents the storage bounds, mechanism battery, and latency measurement plan.

`--read-local 0|1` defaults to `0`. Enabling it arms the local read lane in either mode.
In `2s`, IO threads remain shard-less and use the fused-capable reader machinery; every write
still goes to its shard owner. Both overlap schedules retain their split IO writeback order.

The armed lane captures immutable objects at prefetch and filters unsafe atomic keys individually.
With overlap 0 it serves bounded local-read chunks between bounded owner-task quanta. With
overlap 1 it drains those same read chunks before the whole owner batches; ordinary deep turns
then run the WB callback in the first batch's prefetch gap.
Snapshot, placement, and pre-existing retry/deferred turns preserve their ordering.

## 2s: separated threads

Mode `2s` assigns each physical thread one live role. IO (`ifid`) threads receive, parse, route,
retire, and send; executor (`ex`) threads own shards and execute commands. With no placement knob,
TomoKV makes the same even IO/ex split across the allowed CPUs as before.

Use `2s` for separate IO/executor placement, `--ratio`, manual `FLIP`, or `--flip-auto`.
Complete sibling pairs in the allowed CPU set become placement and FLIP units automatically.

With local reads enabled, all physical threads bind permanent QSBR retirement sinks before
serving. Only the IO tier enters a reader loop. Executor tenure remains parked as a reader,
performs immutable replacement, and drains its retirement queue. FLIP retains those sinks
across both directions of role conversion. INFO SERVER reports `read_local_thread_N` with
role, published shard count, actual lane activation, and lifetime GET/MGET completion counts.

INFO SERVER also reports `overlap_schedule`, `overlap_passes`, and
`overlap_interleaved_passes` from the loops that actually execute, plus `reorder_batches`,
`reorder_multi_client_runs`, `reorder_permuted_runs`, and `reorder_max_batch`. A nonzero
permutation count proves the scheduler changed a real multi-client batch. These lifetime
witnesses survive RESETSTAT. Their optional array is allocated only when overlap or reorder
is enabled. The whole witness field family is absent when both knobs are off, preserving
the disabled INFO path. Armed boots report the allocated count as `schedule_stats_threads`.

## 1s: unified generalized threads

Mode `1s` gives every selected physical thread both an IO loop object and an executor loop object.
With overlap 0, each thread rotates through these phases:

1. maintain connections and parse/route at most 32 operations per connection pass;
2. consume an executor batch of at most 32 operations;
3. collect completions and serve at most 16 connections.

With `--read-local 0`, local commands take the same self SPSC task lane as remote commands and are
consumed during the executor phase; they are not executed inline. With `--read-local 1`, eligible
plain GETs and MGETs instead enter a parsing-thread-local queue. At overlap 0 the executor phase
drains one bounded chunk immediately after parsing and more between owner-task chunks. At overlap
1 parsing uses the targeted ready list, owner batches hold up to 128 tasks, and the read lane
drains before fresh owner execution. Its foreign pointers never span the WB callback: exceptional
turns finish WB before local reads; ordinary deep turns finish local reads before WB.
Replies retire through one connection ROB slot and the normal
write-back path. Parsing never waits for that local queue to retire: a later hash-precise write
first moves the unresolved reads in its transitive key-overlap component to ordinary owner queues,
then publishes behind them; a conservative write moves the whole unresolved set. An unfinished
precise owner-path operation fences only younger reads whose key hashes overlap, so unrelated reads
may still execute locally while the ROB retires every reply in connection order. A broad owner
route remains a conservative fence and is reported separately as route context. MGET applies the
write-ring and store-publication gates to every key/touched shard and is all-or-nothing: any failure
lowers the whole command through the existing scatter path.

Reads with an outstanding conflicting connection write (or a conservatively overflowed write
ring), WATCH or MULTI state, script/scatter context, an unsafe-key filter hit, a typed or expiry-due
value, table-generation churn, or a full local lane fall back to the ordinary owner path. Pending
atomic work on another key in the same shard no longer refuses the read. A filter fingerprint
collision may conservatively refuse an unrelated key; saturation, bookkeeping overflow, or an
unenumerable write set fail the complete shard closed. Filter references remain published through
abort restoration or committed cleanup, not merely until the group decision. A single GET also
falls back on missing so its owner can perform lazy-expiry side effects. MGET applies the filter to
every key and falls back as one command on any positive; it serves a stable missing key as a nil
array element. An expiry-due entry and an armed key-miss notification still fall back.

A local MGET validates its command-wide window with two rules chosen per touched shard from the
table word it already loads. A shard with no open group must keep an even, equal table generation
(advanced by every group install, topology move, ownership handoff, and bulk clear, never by a plain
immutable SET). A shard with an open group is validated per queried key by the touch epoch of that
key's filter cell, a monotonic counter the owner advances after every add, close, poison, or rebuild
of that cell; unrelated group installs on the same shard do not move it, and a cell that went
0 -> 1 -> 0 inside the window still reads +2. The reader loads epochs only for keys on a pending
shard, copies all values into a private reply, then re-reads the same words. Any change retries the
complete command once; a second failure falls back through the existing owner/scatter path. FLUSH
clears poison the filter for their duration so every epoch moves. One-key GET needs no shard sweep:
its filter check stays inside the existing before/after point-probe sequence validation. Plain
writes publish nothing beyond their slot store when read-local is armed.

A point-only batch first hints all home words, then performs complete
key-verified probes in program order. Each probe retains the observed slot address and decoded
immutable `KvObj` pointer on the stack, and execute copies directly from that object. Mixed GET/MGET
batches capture and consume one command at a time to preserve connection order; MGET handles any key
count in bounded prefetch, capture, and execute windows and recaptures all windows on its one retry.
Every later drain pass captures afresh, and all captures are consumed before the rotation publishes
its next QSBR tick. GET misses still demote and MGET misses still emit nil.

`read_local_fallback_context` remains the compatibility aggregate. INFO also reports its exhaustive
`_owner_key`, `_connection_state`, `_route`, and `_keymiss_notify` sub-reasons (and matching MGET
rows): precise owner-key overlap, blocked/subscriber state, special or broad routing, and the MGET
key-miss notification gate, respectively. The `foreign_read_*` gauges expose current unsafe
references, occupied/wildcard/saturated cells, and poisoned shards. MGET separately reports local
hits, generation retries, and pending-filter or generation fallback counts.

With no `--place`, `1s` uses every CPU in the process affinity mask. `--place` can select a subset;
its `ifid@CPU` and `ex@CPU` labels are treated only as CPU selectors because every selected thread
has both responsibilities. `--ratio` is rejected because there are no separate role counts.
`--flip-auto` is also rejected, the flip controller does not start, and `FLIP` returns a clear
mode-unavailable error. Existing key load-balancing bucket movers remain available.

The default shard count derives as `min(8 * executor threads, 256)` in both modes, before
persistence recovery. An explicit `--shards 1..256` overrides it; `--shards -1` restores auto.
Key and client balancing use independent `--key-lb 0|1` and `--client-lb 0|1` switches,
both default `1`. Each off arm allocates none of its signals/windows; both off allocate no
shared LB state. `--shard-home shard:tid,...` sets a complete initial ownership map in either
mode and permits empty filler executors. See [DESIGN-KNOBS.md](../DESIGN-KNOBS.md) for the surface.

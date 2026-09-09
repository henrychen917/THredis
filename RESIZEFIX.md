# Resize progress without reader retirement waits

2026-09-09. Base: `818d458ef` (the supplied `cx-final` HEAD). Worktree/branch:
`/home/user/Projects/cx-resizefix`, `cx-resizefix`.

The final clean build passes each of the four reported rows **5/5**, and the idle check
**5/5 in each of four mode/read-local combinations**. No gate or benchmark was run.

**Mechanism and choice**

I chose owner maintenance plus non-blocking retirement of completed resize tables. Reads do
not need to advance rehashing. The harder requirement was completing a resize without merely
moving the retirement wait into owner maintenance, where it could obstruct subsequent reads.

The existing path is `ExLoopT::sweep -> active_expire_cycle -> FlatStore::active_expire ->
rehash_step`. It already moves eight old slots per store visit, including stores without TTLs.
Two scheduling defects prevented it from being a progress guarantee: busy passes bypassed the
idle sweep, and an unfinished keyspace resize did not count as work, allowing idle owners to park.

`owner_control_tail` now services that existing cycle once per distinct cached millisecond,
before any ownership-drain acknowledgement. Idle sweeps still service it, and structural
progress counts as work, including the final step. If an owner has more than twenty shards,
an unvisited pending resize also prevents parking; the existing round-robin cursor must reach
it. There is no new thread, configuration knob, reader retry, or per-operation seqlock.

For fixed ownership of S shards, V=min(S,20) visits per cycle, and R remaining old slots,
at most `ceil(R/8) * ceil(S/V)` maintenance cycles suffice for structural completion; mutations
can advance it sooner. This is a service bound, with observed wall-clock bounds below, not a
real-time scheduling promise. Explicit DEBUG maintenance suspension and a snapshot deliberately
retaining its frozen table remain suspension points. Snapshot preparation drives its existing
rehash, and the post-snapshot merge uses the same retirement mechanism.

An armed resize reserves a 24-byte `ResizeRetirement` record **before publishing its new
table**. Boot reserves one as well, since persistence replay can leave a resize in flight before
the lane is armed. Allocation failure refuses resize admission before publication. Once started,
the final step needs no allocation or object-ring capacity: it unlinks the old table and hands
the reserved record to the owner's separate FIFO. `src/store/resize_retirement.h` contains that
record and queue; the existing store, sink and owner loop provide the integration points.

The handoff advances the QSBR epoch after unlinking. The normal owner grace scan drains both
queues, and a table is freed only when its stamp is strictly below the participant floor.
Pending table records keep the owner polling, so reclamation also completes without traffic
when participants can pass grace. Queue emptiness includes these records: ownership-drain
acknowledgements cannot abandon them. An unused reservation travels with its shard; all sink
hooks follow the existing ownership-edge rebind. Completed-table callbacks only free their
table and record and never access a subsequently moved store or another owner's cache.

Lazy key expiry preflights the owner's object-ring capacity. A full ring means logical absence
while the object and expiry attention remain resident. Notification, expiry-counter and AOF
deletion effects accompany the eventual physical removal, rather than repeating on every miss.
Owner key/field expiry likewise yields when capacity is unavailable. Review found the same
direct read wait when hash access expires its last field and calls `store_erase`; that path now
returns logical absence without destroying the hash under pressure. Partial field expiry still
preserves the live fields and needs no header retirement.

**Rejected alternatives and costs**

- Scheduling more owner work alone leaves both final-table retirement and lazy expiry able to
  enter `force_oldest_grace`; it is insufficient.
- Restoring reader rehash steps would violate the frozen-maintenance controls and cannot solve
  idle progress. Even a safe reader step is unnecessary once owner service is guaranteed.
- Growing the generic retire ring after unlinking can fail allocation at the worst moment.
  With finite memory and a permanently stale participant, unlimited retirements cannot all be
  accepted safely without retention or backpressure. Reserving the resize record before
  admission removes that failure from resize completion.

The tradeoff is memory retention and periodic owner CPU work. Completed old tables can exceed
the former aggregate 4096-entry retirement ceiling while grace is pinned; their preallocated
records and tables remain owned, not leaked or prematurely freed. Physical reclamation cannot
have a finite bound while a participant indefinitely retains an old pointer. Lazy deletion and
its associated side effects may be delayed under object-ring pressure. Generic writer
retirement retains its existing full-ring backpressure; this does not promise bounded command
latency for an owner already blocked in an unrelated write or externally stopped. No throughput,
instructions/op or IPC improvement is claimed; performance was not benchmarked.

All eight required locks remain: Op 336, Client 1984, ThreadCtx 1408, Shard 1440, FlatStore 944,
Rob<64> 192, AtomicEntry 144, Config 624. ExLoop remains 5856; its millisecond marker uses padding.
Optional armed allocations change as follows (DWARF sizes from the PRE and POST binaries):

| Allocation | PRE bytes | POST bytes |
| --- | ---: | ---: |
| ReadLocalThreadState | 368 | 384 |
| ReadLocalStoreState | 50624 | 50688 |
| ReadLocalDeferredQueue | 4760 | 4800 |
| Per-resize reservation | 0 | 24, before allocator rounding |

The optional sidecar assertions are updated explicitly. The listed hard locks are unchanged.
Read-local 0 allocates none of the new retirement state.

**Diagnostic race discovered during repetition**

Before the diagnostic routing correction, an intermediate candidate passed fused/read-local 1
only 4/5 times: one run failed the unchanged "unexpected mutation/new resize" assertion.
Eight subsequent reproducer runs passed, which did not disprove the source-level race.
`ConfigRoute` actually executes on the connection's IO thread and supplies a shard-0 reference;
the diagnostic's comment claiming owner execution was wrong. It read non-atomic live counters
concurrently with migration, consistent with observing an intermediate count while moving a slot.

`DEBUG REHASH-STATE` now uses ordinary shard-0 owner dispatch, including stale-task forwarding.
Its handler rejects an unrouted invocation before accessing store state. This routing exception
lives inside the existing cold DEBUG branch. The socket battery's only edit adds the actual
before/after states to its failure message; **every assertion, arming condition and bound is
unchanged**. The original `retirement` selection is also unchanged; new unit checks use a
separate `maintenance` selection. No failure was converted to a skip or expected success.

**PRE versus final POST**

Servers used cores 8–15, 16 shards, split 6:2 or eight fused threads; clients used 16–23 and
unit/idle observers 24–31. Flags match the named gate boots: atomic 1, overlap 0, flip-auto 0,
DEBUG enabled, with default load balancing. Ports were 8540–8543. Every server was a fresh
boot and was stopped and waited on through its own recorded PID.

| Acceptance row | Clean PRE | Final clean POST | POST completion time |
| --- | --- | --- | --- |
| reads never wait for retirement quiescence | 0/1; expired lookup hits its one-second alarm | **5/5** | All eight reads return; the intentional blocking control still reaches its alarm |
| read-only resize split, read-local=0 | 0/1; 3984 slots remain after 3 s | **5/5** | 0.499057–0.500002 s |
| read-only resize split, read-local=1 | 0/1; 3512 slots remain after 3 s | **5/5** | 0.499191–0.499838 s |
| read-only resize fused, read-local=1 | 0/1; 3520 slots remain after 3 s | **5/5** | 0.000741–0.000751 s |
| Additional control: fused, read-local=0 | 1/1; completes in 0.059749 s | **5/5** | 0.000604–0.000669 s |

Each socket run observes a real 4096 -> 8192 growth, then completes all 4000 remaining slots,
with unchanged starts/key count/capacity and all 2880 values verified. The deadline remains 3 s.
These are correctness-completion times, not rate measurements.

**Idle server**

`tests/resizefix.py --idle` proves its memory observer agrees with the paused owner diagnostic
while the old table is present. It then resumes maintenance and sends **zero protocol requests**
for one second, keeping the connection open. The launcher SIGSTOPs only its own server child,
reads its stopped memory through `/proc/PID/mem`, then resumes it. GDB obtains offsets from the
binary's DWARF offline; it never attaches or executes an inferior function. Thus neither a new
command nor a diagnostic wake can finish the resize before the observation.

| Mode / read-local | PRE after the quiet interval | Final POST |
| --- | --- | --- |
| split / 0 | old capacity 4096, cursor 256; incomplete | **5/5**, complete by 1.000243 s |
| split / 1 | old capacity 4096, cursor 256; incomplete | **5/5**, complete and reclaimed by 1.000244 s |
| fused / 0 | old capacity 4096, cursor 248; incomplete | **5/5**, complete by 1.000258 s |
| fused / 1 | old capacity 4096, cursor 248; incomplete | **5/5**, complete and reclaimed by 1.000252 s |

The times are observation bounds, not measured instants of completion. Every POST snapshot has
old pointer/capacity/cursor/live count zero and all 2880 keys retained. Armed snapshots also have
zero queued retired tables. All values are checked afterward. All **40 final socket boots**
(20 read-load, 20 idle) exit successfully with clean shutdown reports.

**Additional verification and reproduction**

The `maintenance` unit selection passed **5/5**, with fresh child state for each check:
four consecutive resizes against the same full ring and stale participant; deferred expiry's
exactly-once effects; full/partial hash-field expiry; post-snapshot table retirement; and an idle
owner with 32 shards, placing the resize beyond its first twenty visits. The captured old-table
slot remains readable before grace, and all four retained tables reclaim after releasing the pin.
ASAN/UBSAN builds of the named unit's test translation unit also passed both `retirement` and
`maintenance`; the linked supporting objects are the release build, not a whole-server ASAN build.

Both baseline and final release builds were from clean directories. The final build has no
compiler warnings. `build/resizefix/post` links this worktree's source/test/build inputs and
contains its own `build`, so cleaning it preserves baseline binaries and measurement logs.

```sh
taskset -c 8-15 make -C build/resizefix/post clean
taskset -c 8-15 make -C build/resizefix/post -j4 all build/rehash-waits-unit
taskset -c 24-31 python3 tests/resizefix.py build/resizefix/post/build/tomokv \
    build/resizefix/recheck-reads --repeats 5
taskset -c 24-31 python3 tests/resizefix.py build/resizefix/post/build/tomokv \
    build/resizefix/recheck-idle --idle --repeats 5
taskset -c 24-31 build/resizefix/post/build/rehash-waits-unit maintenance
```

Use fresh output directories. The recorded final results are in
`build/resizefix/verified-{reads,idle}/results.json` with per-boot battery/server/shutdown logs;
baseline logs are in `pre/` and `pre-idle-clean/`. Additional unit results are
`maintenance-verified-{1..5}.log` and `asan-verified-{retirement,maintenance}.log`.
Build logs are `build-final-owner.log`, `build-unit-asan.log` and `build-pre.log` in the same root.
Earlier investigative results are retained separately and are not pooled into the final rates.

Final release SHA-256:
`1a378b7f004564fd6d9b96a783126bac6c358e7fe82b074254da066f9035dd7e`.
Final ordinary retirement-unit SHA-256:
`768adddac9b8d9769abb1050e64e5a25c906beb6e6b22b6b0380bef9210bb6be`.

Gate row-count delta: **0**. No row was added or retired, and `EXPECT_QUICK` / `EXPECT_FULL`
were not edited. The maintainer's full gate and performance work remain unrun.

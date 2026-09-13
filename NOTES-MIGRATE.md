# Runtime ownership migration

> Historical research record: configuration recipes and measurements below describe earlier
> revisions. Deleted controls are not accepted by the merged server, and the manual-map
> dispatch-scaling instrument is retired. For current controls and merge dispositions, see
> [DESIGN-KNOBS.md](DESIGN-KNOBS.md) and [MERGE.md](MERGE.md).

This document is the design contract for runtime bucket, client, and thread-role
migration.  It was written before the implementation.  The central rule is:

> At every instant a bucket has exactly one executor (EX) owner and a connection
> has exactly one I/O (IO) owner.

Registration, queue membership, and readiness are not ownership.  A resource can
temporarily be unregistered or absent from a work queue, but its owner is always
defined.  Ownership changes at one release/acquire publication point; the old
owner must not touch the resource after that point and the new owner may not touch
it before that point.

## Prior-art audit

The Redis fork in `/home/user/Projects/tomokv` was inspected before this design.
The requested commits and the heads/history of `flip-u1`, `flip-ownership`,
`flip-signal`, `globallb`, and `consolidated-ex-wb` establish these useful rules:

* `f4cbed5c5` registered `flipCommand` with arity `-2` and flags
  `WRITE|ADMIN|NOSCRIPT|NO_MULTI|NO_ASYNC_LOADING`.  It also returned the live
  and target split, and added `DEBUG TOMO-FLIPSELFTEST`.
* `1ce393392` separated runtime split authority from its actuator and validated
  that IO and EX counts are positive and sum to the provisioned thread budget.
  Its automatic planner and search policy are deliberately not ported here.
* `8164f382d` refused changes during synchronous or asynchronous loading.  This
  tree inherits that safety rule.
* `d4ae88d12` distinguished the operator's requested split from the provisioning
  stride in configuration output.  This tree similarly reports live and target
  values rather than reconstructing either from affinity.

The conservation failure was not fixed by the first diagnostic change.
`5397f2614` published and graded `io_threads_live + num_workers_live ==
configured`, but was primarily observation.  The actual grow-back abort leak was
fixed in `d7d7d137c`: it introduced explicit half-move accounting, rollback on
every abort/release path, atomic grow-back states, and a recovery counter.
`d9192276b` then fixed role-identity publication (including a dormant EX draining
under an IO identity), and `451ec41d7` fixed target publication order.  Thus the
fork ultimately fixed the P0 rather than only hiding it.  This design goes one
step further: all conditions which can refuse a FLIP are checked before any role
or resource ownership is committed.

The fork's automatic r8/r10 controllers, m1 model, cost cells, hill-climb, and
actuator search are policy and are not ported.  This implementation is manual
only.

## Shared terminology and publication rules

* A **physical shard** is a `Shard` object and its `FlatStore`.  The store is
  never copied or rebuilt during migration.
* A **bucket owner** is the EX thread id returned by `Router::owner_of(hash)`.
  `Router::shard_of(hash)` returns the immutable physical storage identity from
  the other half of the same packed atomic entry.
* Bucket transfers are aligned to complete physical-shard ranges.  A non-thread-
  safe `FlatStore` cannot safely be split between two EX threads.  A range can
  contain one or more complete shards, but never part of one.
* A **connection owner** is `Client::ifid_thread()`.  Epoll/io_uring registration
  follows that owner; it does not define ownership.
* All ownership fields use atomic release/acquire publication.  No code may infer
  ownership from a vector, queue, event registration, or thread role.
* A stale message is safe only because the receiver checks the current owner
  before touching the resource and forwards it when ownership differs.

## 1. Bucket transfer

### State machine

Only one bucket-range transfer can publish at a time.  The coordinator owns a
descriptor `(begin, end, source, destination, phase)`.

1. **IDLE / SOURCE OWNS** — every entry in the range resolves to `source`.
2. **PREPARE** — validate that the range is non-empty, aligned to complete
   physical shards, uniformly owned by `source`, and that source and destination
   are live EX threads.  Reserve the destination's shard-vector capacity.  No
   ownership has changed.
3. **QUIESCE SOURCE** — stop admitting matching work at the source.  Drain its
   task and borrow-release inboxes, ordered and retry queues, and any matching
   deferred work.  New stale arrivals are forwarded.  Wait until no executing
   operation, scatter group, atomic group/commit ticket, snapshot control step,
   or epoch-pinned read can touch the range.  Refusal or timeout here leaves the
   source owner unchanged.
   FLIP also scans the complete shard-pointer partition and every router bucket:
   each physical `Shard*` must occur exactly once, only on its live EX owner, and
   each bucket must name that same shard and owner. Destination capacity for the
   worst-case plan is reserved before publication.
4. **PREPARING** — publish the descriptor without changing `owner_[]`. Every
   ordinary router read remains one acquire-load and can observe only `source`.
5. **INSTALL BOOKKEEPING** — while `PREPARING` still resolves the range to source,
   move the physical `Shard*` bookkeeping from the source vector to the already
   reserved destination vector.  The vector is bookkeeping, not authority: source
   remains sole owner throughout this step.  The objects and all keys/MVCC versions
   stay put.
6. **COMMITTED / DESTINATION OWNS** — a single release-store of the descriptor
   phase is the ownership handoff. Before it, only source may touch the shards;
   after it, only destination may touch them. Only after this edge are the packed
   bucket entries and derived flat shard-owner entries release-stored to
   `destination`. A racing reader may still return `source`, which is now merely
   a safe stale route.
7. **STABLE** — after the complete bucket range and flat shard entries publish,
   clear the descriptor. Readers never inspect the descriptor in any phase.

The forwarding rule is deliberately chosen over per-request routing epochs: an
IO thread can route using a pre-flip owner read and enqueue after the handoff.
Every EX task and borrow release checks current ownership before its first shard
access.  The old owner forwards a stale item to the current owner.  Converted IO
roles are stricter: the global producer barrier and an unmasked drain prove every
EX inbox quiescent before the direct EX -> IO role edge, so no late producer can
strand work in a dormant inbox.

### In-flight cases

| In-flight state | Resolution |
| --- | --- |
| Operation queued in the losing EX inbox | Drained before publication, or ownership-checked and forwarded. |
| IO read old owner but has not enqueued | Old receiver observes the committed owner and forwards. |
| Operation executing in source | Handoff waits for the source safe point. |
| Ordered, atomic, MULTI, or retry-deferred operation | Its queue is drained; transfer cannot cut the operation/group. |
| Cross-shard scatter group | A global group-in-flight witness must reach zero before publication. |
| Atomic group with commit ticket | Both group and apply/commit witnesses must reach zero; a ticket is never divided. |
| Borrow release queued for an old owner | Receiver forwards before calling `unborrow`. |
| Epoch-MVCC read with pinned snapshot | It completes while source owns the unchanged physical shard; transfer waits for the pin/group witness, so no read crosses the edge. |
| Reader racing the multi-entry owner-array write | It returns one non-tearing packed entry: source (safe stale route) or destination; destination is never published before commit. |
| Cursor/whole-shard operation | The complete physical shard is the minimum transfer unit; ownership is sampled once and checked at execution. |

There is no intentionally unsafe case.  Whole-shard alignment is a necessary
restriction: moving only owner entries for part of one non-thread-safe store would
create two concurrent owners of that store even though buckets themselves looked
correct.

## 2. Client transfer

The client and fd move only after the connection protocol is idle.  The reorder
buffer is drained rather than moved while it contains work.

### State machine

1. **SOURCE ACTIVE** — `Client::ifid_thread()` names source; source alone may
   parse, retire, write, register, or close the client.
2. **DRAIN REQUESTED** — source stops parsing new requests and stops arming new
   receives.  Already-parsed operations continue.  Socket bytes already received
   but not parsed remain in the client's input buffer.
3. **DRAIN ROB/OOB** — wait for `dispatch_id == flush_id`, no ready/retire entry,
   empty write segments, no send in flight, and no `WbEngine` deferred OOB or
   active OOB drain for this client.  Empty segments plus no send CQE implies no
   outstanding zero-copy value borrow.  Refusal/timeout here leaves source owner
   and resumes it unchanged.
4. **PREPARE DESTINATION** — reserve destination client/WB/catalog and enqueue
   capacity before detaching anything. Epoll also reserves both the destination
   registration and a duplicated source rollback fd. Pub/sub and CLIENT
   TRACKING/MONITOR routing state is snapshotted and its destination capacity and
   HOME re-registration events are reserved here; owner-local state that cannot be
   transferred is refused during preflight.
5. **DETACH ENGINE (REVERSIBLE)** — still under source ownership:
   * io_uring cancels the fd's receive/poll request and waits for the original CQE
     to acknowledge cancellation.  A cancellation CQE never transfers ownership.
   * epoll duplicates and pre-registers a rollback fd on the source and
     pre-registers the original fd on the destination, then removes the original
     source entry at an event-loop boundary after the current event batch. Backup
     and early destination events are owner/migration checked before client use.
   The source also extracts, but retains, the allocation-owning CLIENT catalog
   node.  A pre-commit refusal restores epoll/recv and reinstalls that exact node;
   no connection authority changed.
6. **HANDOFF** — remove source-local membership and release-store
   `ifid_thread = destination`.  This one store is the connection ownership edge.
   The target is the sole owner immediately, even while the fd is temporarily
   unregistered.  Source transports the pointer but never touches the connection
   again.
7. **DESTINATION INSTALL** — destination acquire-loads ownership, installs its WB
   slot and directory entry, the moved pub/sub and CLIENT TRACKING/MONITOR state,
   then acknowledges installation so the source may retire transient stale-route
   forwarding state. In addition:
   * io_uring arms a receive on the destination ring;
   * epoll activates the destination registration prepared before handoff.
8. **DESTINATION ACTIVE** — destination resumes parsing and retirement using the
   unchanged, empty ROB and any buffered input bytes.

FLIP records the exact number of clients planned from every current IO source,
including an over-quota IO which keeps its role, and will not acknowledge CLIENT
PREPARE unless that many reversible migration records exist. A cancellation/error
therefore cannot silently shrink the plan into a successful partial FLIP.
`flip_clients_transferred` counts owner edges as a live mechanism witness.

### In-flight cases

| In-flight state | Resolution |
| --- | --- |
| Parsed op has a ROB slot or EX task | Drain until ROB is quiescent; the ROB never moves half-full. |
| Bytes received but not parsed | Kept in `Client` and parsed only after target activation. |
| io_uring recv/poll points at `Client` | Explicit asynchronous cancellation; wait for the original CQE before handoff. |
| io_uring send CQE pending | Drain it before handoff. |
| epoll event already returned to source | Preparation runs only after that event batch; the original source interest is removed before handoff. |
| Kernel zero-copy send borrows a value | Segment/send drain waits for release; migration is refused on timeout. |
| Reply partly retired or buffered | Flush completely before transfer, preserving protocol order. |
| Deferred push/OOB frame | `WbEngine` finishes its drain; migration cannot strand or discard the deferred frame. |
| Ready/retire queue contains the client | Source consumes it before declaring the client idle. |
| Newly routed completion races drain | ROB cannot become quiescent until the completion retires; the global FLIP dispatch pause prevents new work. |
| Pub/sub, tracking, or monitor owner-local state | Reserved and extracted at commit, installed at destination, and re-registered at each pub/sub HOME. In-flight deliveries read `ifid_thread()` and forward from a stale owner. |
| TLS, blocked WAIT, MULTI, or WATCH owner-local state | Refused by preflight because that state is not transferable. No partial move is attempted. |
| Close/error during drain | Source remains owner and performs normal close; FLIP preflight is refused. |

This deliberately extends the invariant documented in `src/net/epoll.h`.
Registrations are no longer "armed once for the lifetime of the fd"; they are
**armed once per IO-ownership tenure**.  Normal operation still never rearms an
edge-triggered fd. Migration pre-registers both the destination and a duplicated
source rollback fd before removing the original source entry. Commit closes the
backup. Rollback deletes the destination entry, adopts the already-registered
duplicate as the connection fd, and closes the original, so it never depends on
a fallible fresh registration. Teardown continues to rely on `close(fd)` when no
migration is occurring.

There is no intentionally unsafe client move. Transferable owner-local routing
state is prepared and handed off with the connection; refusing remaining
non-self-contained modes is preferable to copying hidden loop-local state or
violating the ROB/borrow contract.

The refusal-only cases are explicit. TLS, blocked WAIT, MULTI, WATCH, and
transient pub/sub or CLIENT TRACKING/MONITOR operations are refused when balance
requires that connection to leave its owner (a survivor may keep such a
connection inside its quota). Durable pub/sub, tracking, monitor, and CLIENT
REPLY state is transferable. So are insufficient client/catalog/channel/vector
capacity, failure to duplicate or pre-register an epoll fd, a recv cancellation
which cannot reach its pointer fence, a non-quiescent ROB/reply/borrow/OOB state,
or an invalid shard partition. All occur before the first owner store. An actual
peer disconnect or kernel socket error is external connection failure rather
than a FLIP mechanism; if it races reversible preparation the FLIP is refused,
but the server cannot resurrect a socket the peer has already destroyed.

## 3. Manual FLIP

Grammar:

```
FLIP
FLIP <io> <ex>
```

The no-argument form reports `live_io`, `live_ex`, `target_io`, `target_ex`,
`smt_mode`, `unit_threads`, whether a transition is moving, the resulting
`bucket_min`/`bucket_max` and `client_min`/`client_max`, and `last_transfers`.
The mutating form returns `+OK` only after the live split equals the target and
both ownership dimensions are balanced. IO and EX must both be positive and
`io + ex` must equal the provisioned logical-thread count.

### Automatic arithmetic rebalance

Every mutating FLIP, including `FLIP <live_io> <live_ex>`, plans both dimensions.
There is no load sampler, throughput signal, timer, hill-climb, or automatic
target selection in this lane.

For `N` owned objects and `O` final owners, an even distribution means every
owner has either `floor(N/O)` or `ceil(N/O)` objects. Equivalently, and this is
asserted before `+OK`, `max(count) - min(count) <= 1`. Client objects are live
connections. A bucket object is the existing indivisible bucket-transfer unit:
one complete physical `Shard` and its aligned router-bucket range. The range may
contain many of the 16,384 routing slots, but splitting that non-thread-safe store
would violate single ownership.

The remainder quotas are assigned where the extra object saves a transfer; equal
savings are broken by lowest thread id. Sources and deficits are then visited in
ascending thread-id order. On an over-quota bucket owner, shard id is the stable
tie-break: the lowest ids stay. On an over-quota client owner, stable owner-local
connection order selects movable clients, and destinations still use lowest
thread id first. Thus the same state produces the same distribution.

Movement is minimal. Once deterministic quota `q[t]` is fixed, every plan must
move at least

```
sum over current owners t of max(current[t] - q[t], 0)
```

objects; a retiring owner has `q[t] = 0`. The implementation moves exactly that
many buckets and exactly that many clients. An owner which survives at or below
quota loses none of its objects. A future owner receives only deficits. A shard
destined for a new EX is transferred directly into that thread's dormant EX
vector while dispatch is paused, so it crosses one ownership edge rather than a
temporary staging owner. `last_transfers` is the sum of bucket and client owner
edges performed by the last successful FLIP; an already-balanced same-shape FLIP
therefore reports zero.

`smt-mode 0` is the explicit default and preserves logical-thread-independent
placement and FLIP selection.  `smt-mode 1` reads Linux
`cpuN/topology/thread_siblings_list`, requires both logical CPUs of every selected
physical core to be present with the same role, and converts both together.  A
requested split with an odd IO or EX count is refused before quiescence, without
rounding; the error names the adjacent even `io:ex` splits and the report retains
the refused target beside `unit_threads=2`.

The command inherits the fork's flags: write, admin, no-script, no-MULTI, and
no-async-loading.  Existing C++ admin commands are not generally hidden behind
`--enable-debug-command`; only `DEBUG` is.  FLIP therefore follows the existing
admin-command convention and does not invent a new configuration knob or gate.

### State machine

1. **IDLE** — no dispatch gate; report requests are synchronous.  After a
   refusal, target deliberately remains the refused request, so `target` can
   differ from `live` while `moving` is false.
2. **PLANNING / IO DRAIN** — claim the one FLIP slot; publish target; validate
   grammar, positive counts, exact conservation, loading/snapshot exclusion, and
   movable candidates (the requester, AOF writer, and UNIX owner are pinned).
   Publish the dispatch pause.  Ordinary requests parsed during the pause receive
   `BUSY` locally and create no executor work; FLIP report/control requests remain
   available so live-vs-target is observable in flight. Each IO drains ROBs,
   replies, OOB, zero-copy borrows, completion/transfer channels, and retry state.
   The coordinator reads the frozen counts, assigns deterministic final client
   quotas and exact source/destination edges, and checks every SPSC lane's capacity.
3. **IO PREPARE** — future/current IO owners reserve Client, WB, catalog, and
   transfer-inbox storage.  A future IO creates its fallible TCP/TLS listener on
   its own physical thread but does not publish that loop as the role endpoint.
4. **EX DRAIN** — every EX drains tasks, releases, retries, notify/AOF output,
   scatter/atomic witnesses, then acknowledges a hard safe point.  Once
   acknowledged it performs no expiry, cleanup, waiter, or shard walk until EX
   INSTALL. Mask-independent inbox drains prevent a missing hint from faking
   quiescence. The coordinator computes every final bucket quota and exact shard
   destination and reserves every destination vector before continuing.
5. **CLIENT PREPARE** — every IO source with planned excess (not only a converting
   source) pre-registers both epoll outcomes or cancels io_uring recv
   and wait for the original CQE, but retain connection ownership.  An IO -> EX
   source also stops accepting: epoll keeps its tenure registration dormant,
   while io_uring cancels each multishot accept without closing the listener and
   waits for the original terminal CQE.  This is the last reversible stage.
6. **ROLLBACK (pre-commit only)** — on refusal/timeout, reinstall each exact
   source catalog/registration, re-arm the retained old-role listeners, and close
   prepared future listeners.  Clear the gate and increment `flip_refused`;
   retain the refused target beside the old live shape.  No resource or role
   owner changed.
7. **CLIENT COMMIT / INSTALL** — release-store each planned connection owner and
   install it at a current target, or queue it in the pre-reserved inbox of a
   prepared future IO. With EX producers frozen, run a second full IO drain and
   require the global pub/sub event count to reach zero; this catches messages
   which were in transit when an IO published its first drain acknowledgement.
   From the first owner store onward, all allocations and operational refusals are
   already resolved; an unexpected failure aborts as an invariant violation
   rather than exposing a partial healthy server.
8. **BUCKET / ROLE COMMIT** — execute the preflighted bucket plan directly from
   each old EX to its final EX, including a dormant future EX. IO -> EX candidates
   already handed off every connection and also
   completed their reversible accept cancellation; their old listeners close as
   the IO tenure ends.  Each role then changes by one direct old-to-new atomic
   store—never through Idle.
9. **ROLE READY / SHARD COMMIT / EX INSTALL** — converted loops publish the new
   ready role. Every final IO drains its queued transfers, verifies its exact
   planned client quota, and acknowledges. Every live EX binds owner-local
   notification state and acknowledges installation.
10. **COMPLETED** — count ready IO/EX, assert conservation, increment
   `flip_completed`, assert both min/max bounds, exact per-thread quotas and exact
   planned-vs-performed transfer count, publish `last_transfers`, clear the gate,
   then complete the request's ROB op with `+OK`. The reply therefore proves the
   requested shape is fully live and balanced.

FLIP is a single transaction at the control-plane level.  Every recoverable
refusal point is before CLIENT COMMIT, before ownership mutation.  The
implementation must not attempt a best-effort rollback after the commit plan;
such a rollback would itself create ambiguous ownership.  Unexpected failures
after that edge terminate via an invariant assertion instead of publishing a
partial, apparently healthy split.

This is all-or-nothing option **(a), full preflight**. No fallible allocation or
registration remains after that edge. Shard transfer
moves an existing `Shard*` between pre-reserved vectors and rewrites only owner
bits; the `FlatStore`, keys, values, expiry, and MVCC records never move. Client
transfer moves the unchanged `Client*`, drained ROB, buffered input, and extracted
catalog node through pre-reserved storage. Both epoll outcomes already have live
registrations; io_uring receive arming uses the destination's pre-existing ring
and retries SQ starvation without closing the socket.

### In-flight cases

| In-flight state | Resolution |
| --- | --- |
| Request is being parsed/acquired but not published | IO acknowledges pause only after the parser pass; acquired slots are published or cancelled normally. |
| Published single-shard task | Drained, executed, or forwarded before the corresponding bucket edge. |
| Cross-shard/atomic operation | Global witnesses must be zero; FLIP never cuts the group or its commit ticket. |
| Epoch-pinned read/snapshot control | Completes before commit; FLIP is refused while snapshot/load/reload is active. |
| Completion/borrow release headed to a converting thread | Old-role inbox is drained and stale messages are ownership-forwarded during the grace phase. |
| FLIP request's own ROB slot | It is held by the surviving coordinator IO and completed only in COMPLETED/refused state.  That client is never selected for transfer. |
| FLIP report/control request on another connection | It is the only command admitted through the dispatch pause and executes IO-locally; ordinary commands receive `BUSY` and cannot repopulate an EX inbox. |
| Existing socket sends during pause | Bytes may be received/buffered, but no new command is dispatched until the gate clears. |
| New accept during pause | io_uring accepts and immediately closes it; epoll leaves the listener backlog unread. It never becomes an owned `Client`. |
| Multishot accept on an IO -> EX candidate | Before CLIENT COMMIT, cancel it without closing the listener and wait for its original terminal CQE; late accepted fds are closed on the old ring.  Rollback can re-arm the same fd without allocation. |
| Client reply, OOB, recv/send CQE, or zero-copy borrow | Resolved by the client-transfer drain rules before commit. |
| Late route using a pre-transfer bucket owner | Resolved by EX forwarding; no per-request epoch is required. |
| Loading or async reload races admission | Runtime load, snapshot, and FLIP publication share a short transition mutex.  If loading publishes first FLIP refuses; if FLIP publishes first the load refuses, inheriting the fork's `NO_ASYNC_LOADING` safety rule without a check-then-publish race. |
| Snapshot/AOF rewrite races FLIP admission | A short shared transition mutex makes the snapshot `Preparing` and FLIP `Planning` publication edges mutually exclusive; whichever publishes second refuses. |
| Notify/pubsub message was in transit at the first IO drain | After EX freezes, CLIENT INSTALL performs a second drain and waits for the global event lifetime count to reach zero before role/vector mutation. |

### Observability and tests

`INFO` exposes live IO/EX counts, completed/refused flips, transferred clients,
the last successful FLIP's combined transfer count, current bucket/client
min/max, successful conservation checks, and conservation violations. The report
form exposes the same balance witnesses plus live/target counts, SMT mode, and
unit width. The directed battery begins with deliberately empty future IO owners,
asserts both `max-min <= 1` bounds after each moving FLIP, writes and re-reads every
value, keeps 96 sockets with identifiable unread pipelines across a real moving
FLIP, checks their ordered replies, requires a non-zero transfer witness, and runs
the same already-balanced FLIP twice to prove zero movement and deterministic
distribution.

No automatic controller, timer, target search, or background hill-climb exists.
No new configuration knob is introduced, so the knob matrix is unchanged.

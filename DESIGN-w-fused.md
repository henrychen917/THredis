# w-fused: threading truth, deterministic diagnostics, and clean shutdown

## Scope and invariants

This lane makes the selectable `1s` and `2s` modes describe themselves truthfully and makes the
existing test-only timing and shutdown surfaces deterministic.  It does not change command routing,
store ownership, read-local admission, immutable replacement, QSBR, or any ordinary GET/SET path.
`2s` keeps FLIP and its Ifid/Ex accounting.  `1s` keeps every thread able to serve connections and
own shards, and reports that fused fact rather than presenting an empty executor tier.

No production tuning knob is added.  New delay behavior is reachable only through DEBUG, and zero
means that no delay path is armed.  All new state is cold or test-only and none is added to `Op`,
`Client`, `ThreadCtx`, `Shard`, `FlatStore`, `Rob<64>`, `AtomicEntry`, or `Config`; their layout locks
remain authoritative.

## Thread capabilities and observability

`Role` remains the operational split-mode state used by the already-specialized loops and FLIP.
Changing those comparisons would alter per-pass and per-operation code in `1s`, so fused identity is
instead first-class through two predicates: `serves_clients(tid)` and `owns_shards(tid)`.  In `2s`
they derive from the live Ifid/Ex role; in `1s` both are true for every placed thread.  Reporting
derives a presentation kind (`io`, `ex`, or `fused`) from those capabilities.

`DEBUG LBSIGNALS` keeps every existing `2s` row and rollup.  In `1s`, thread and rollup rows say
`fused`; shard rows retain the exact `shard sid owner_tid ...` grammar because those rows—not a role
label—are the ownership oracle used by tests.  INFO LB uses mode-specific fields: the existing
Ifid/Ex fields in `2s`, and fused/client-serving/owner counts and fused work signals in `1s`.  It
does not synthesize `ex_threads:0` or a split ratio in fused mode.  INFO server and FLIP/FLIPCTL
likewise say that FLIP is unavailable because the threads are fused; `2s` FLIP rollups and behavior
are unchanged.

The gate's idle checks consume deltas of the existing LBSIGNALS iterations/spins/wake counters.  No
writer or loop counter is added: this only replaces process-jiffy inference with the server's
per-thread single-writer gauges.

## Deferred DEBUG SLEEP

`DEBUG SLEEP` becomes an IO-owned deferred reply.  Parsing arms a connection timer, retains the op
in its ROB, marks that client blocked, and lets the owning loop continue serving all other clients.
Expiry emits `OK` in order, records the full deferred duration in SLOWLOG, unblocks the client, and
resumes parsing.  Disconnect cancels the timer.  A zero duration completes synchronously.  SLEEP is
rejected inside MULTI/EXEC because a subcommand cannot defer only one element of the aggregate EXEC
reply without changing transaction machinery.

The maximum duration is the live client timeout when it is nonzero, with a fixed one-second DEBUG
safety ceiling when client timeouts are disabled.  Thus a disabled production idle timeout does not
make a mistyped diagnostic park a connection for a day, and no new numeric knob exists.

## AOF physical-stream ownership

The AOF writer owns a state token whose alternatives are `Open` and `Large(producer, begin_offset)`.
Only an Open token can be passed to a physical control-frame writer; only the matching Large token
can continue an open large record.  Normal and io_uring submission APIs have distinct data/control
entry points rather than a boolean that permits either frame class.  State transitions occur only
after a successful physical submission.

Ready group commits observed while a large record holds the token still increment the existing
deferral counter, but that observation is no longer the correctness guard.  Shutdown rollback uses
the begin offset carried by the Large token.  Loader-side corruption checks remain defense in depth.
All of this is confined to append-only writer code; appendonly-off command paths do not consult it.

## Observable snapshot cut

At the existing freeze-to-mark transition, after every shard has acknowledged Freeze and before any
Mark work is published, the snapshot manager reads the server's existing atomic commit-safe word
once and latches it as `snapshot_cut_ticket`.  INFO performs a plain relaxed read of that latch.  The
last established cut remains visible.  Zero is also a valid completed cut before the first commit
ticket; consumers that must distinguish that state from "no cut yet" pair it with
`snapshot_cuts_armed`.  This adds no reader retry and no synchronization to mutations.  The typed
snapshot race brackets a target cut with sequential cross-owner EXEC tickets B/C/D, so every value
verified after reload is classified by ticket rather than wall-clock timing.

## Geometry and hop diagnostics

`DEBUG SHARDS key...` returns one nested RESP pair `[sid, owner_tid]` per key, in input order.
`owner_tid` is read through the same acquired shard-owner publication that routing uses.  The legacy
single-key `DEBUG SHARD` remains available.  Callers must not treat a multi-row response as a
transactional topology snapshot during FLIP; LBSIGNALS shard rows remain the topology ground truth.

The existing DEBUG atomic-commit delay storage is renamed to the single `debug_hop_delay` field.
Both `ATOMIC-COMMIT-DELAY` and `ATOMIC-OFF-HOP-DELAY` set it, last writer wins, and either can set zero
to disarm it.  Atomic mode reads it at the existing ticket-to-publish boundary.  Non-atomic scatter
loads it only after classifying a cross-owner write: one lead owner is allowed to publish, remaining
owner tasks yield through the existing deferred-task queue until the deadline.  Two-hop commands arm
the boundary whose inter-owner split exposes their publication contract: normally the first
mutation phase, but a derived store or multi-pop whose only multi-owner wave is its read/probe phase
arms there.  The deadline begins only after the lead fragment completes, so scheduler delay cannot
consume the observation window.  Ordinary single-owner commands never enter scatter; when the
field is zero no deadline or delay work runs.

## Boot, listeners, and shutdown

The Unix listener has one RAII owner and is created only after persistence load succeeds.  A cold
`IoLoop::attach_listener(fd)` transfers it to the designated client-serving loop after `init()` and
before activation in both modes.  Failure before transfer closes the descriptor and unlinks the
path; failure after transfer is owned by the loop.

Fused startup has one `FusedBootGate` with explicit `Loaded -> Ready -> Running` progression.  Each
worker arrival is counted once; every wait also accepts stop and records a once-only `gave_up`
arrival.  This replaces independent booleans/counters whose predicates could disagree.  It is
boot-only and does not survive into the fused run loop.

A process-owned nonblocking eventfd is created before rings.  The signal handler publishes stop and
writes the eventfd-required eight-byte counter (one logical doorbell event; eventfd does not accept
one-byte writes), preserving `errno`.  The same sticky fd is registered in every io_uring ring,
ring epoll/poll wait, and IoLoop epoll set and is never drained by workers, so one waiter cannot steal
shutdown from another.  Signal state is a fixed atomically published table, armed only after the
server/thread table exists and disarmed before teardown.

NOT INTEGRATED (see the integration note at the end of this file): the lane also drafted a
post-join `IoLoop::close_all_clients()` that synchronously reclaims live/dead clients and connection
sidecars for leak-sanitizer cleanliness.  That draft never compiled and is not part of this merge.
End-of-run teardown remains the established `atomic_shutdown_release_records()` +
`reap_atomic_deferred()` pair, now shared by both modes through one `report_graceful_shutdown()`.

## Shutdown report contract

Split and fused shutdown capture one immutable `ShutdownReport`.  The existing human lines are
rendered from that snapshot.  Fused work is represented as fused work, while split work retains
dispatched/executed totals.  The snapshot also contains writeback, TLS, stuck/client/ROB/unsent,
epoll, accept/rearm/SQE/notify, per-thread loop, and AOF summary fields already printed today.

After explicit client, ACL, listener, ring, and signal teardown, the process prints and flushes one
final line:

```
shutdown_report {"schema":1,"thread_mode":"1s|2s",...}
```

The payload is valid JSON with stable names and integer base units.  Nothing may log after this
line.  The human report remains for operators; the gate parses only the final JSON record.

## Expected measurement

The coordinator should run the full gate in both boot modes, the `2s` FLIP battery, and every test
that consumes LBSIGNALS.  Pure GET/SET null comparisons must remain exactly zero-cost.  Additional
checks are: two-client DEBUG SLEEP progress and ordering, exact snapshot ticket classification,
batched SHARDS parity, deterministic atomic-off tears with the shared delay, physical AOF frame
walks under deferred GCMT, signal-to-exit latency, JSON last-line parsing, failed-load listener
absence, shutdown-during-each-fused-gate-stage, and LSAN connection-lifecycle cleanliness.

## Integration note (merge onto mainline, t-fused-int)

The lane's final commit was interrupted and marked NOT verified.  Its post-join shutdown
reclamation subsystem -- `IoLoop::close_all_clients()` and everything written only to serve it
(`ExLoop::shutdown_*_after_join`, `multi_shutdown_*`, `xshard_shutdown_prepare/destroy`,
`blocking_shutdown_prepare/destroy`, `Shard::shutdown_release_watches/shutdown_release_blocking`,
and the `visit_pending_after_join` walks on ThreadCtx/ExQueue/MaskedQueue/LoopSignals) -- does not
compile: `multi_shutdown_op_entry` calls a `Client::has_atomic_groups_io()` that does not exist, and
`multi_shutdown_session_after_join` reaches `IoLoop::scatter_pool_`/`self_` with no friend
declaration.  It was therefore dropped rather than merged.  Everything else in the lane is
integrated.  The reclamation draft is recoverable from `t-w-fused` (58957061f) if the leak-cleanup
goal is picked up again; it needs a compile, a design pass on the two-phase ownership argument, and
its own shutdown-under-load tests before it can ship.

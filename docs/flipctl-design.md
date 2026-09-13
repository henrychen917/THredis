# t-flipctl design and code map

This lane adds one automatic owner around the existing runtime `FLIP`; it does not add another
placement transaction. `Server::flip_begin` and the unchanged `FlipStage` acknowledgements remain
the only way thread roles, clients, listeners, and shards move. The automatic controller ships
dark (`flip-auto 0`).

## Ownership and footprint

- `src/main.cc` is the controller cron owner. When `flip-auto=1`, the main/monitor thread calls
  `Server::flipctl_tick()` on its internal observation cadence. No IO or EX loop runs controller policy.
- `src/core/flipctl.{h,cc}` contains the fingerprint representation, shift detector, and controller
  state machine. Controller vectors allocate only when `flip-auto=1`.
- Each `ThreadCtx` embeds one `FlipFingerprintWriter`. It has no dynamic storage. Only the physical
  thread playing IO mutates it. There are no fingerprint atomics and no field was added to `Op`,
  `Client`, or `Task`; the existing `sizeof(Op)==336` and `sizeof(Client)==1984` assertions remain
  build gates.
- Cross-owner observation follows the existing exceptional INFO-counter rule: the monitor reads a
  completed monotonic cumulative window and subtracts its last snapshot. A writer stores
  `closed_windows` last, so an unfinished partial window is never consumed. Closing is once per
  `flip-work-window` commands, not per command.

## 1. Workload fingerprint

The dispatch integration is `IoLoop::flip_fingerprint_note()` in `src/core/io_loop.h`, with the
ACL, AUTH, and MULTI early-completion arms calling the same helper from their existing dispatch
hooks. It runs only after the frame has crossed its final backpressure/refusal point.

The writer records:

- one parse-pass bucket (`1`, `2-4`, `5-16`, `17+`) for the number of successfully consumed frames;
- command class, in precedence order: blocking, atomic grouped, multikey write/read, single-key
  write/read, other;
- multikey key sum/count, using the registry's existing `first_key/last_key/key_step` metadata;
- approximate value bytes, the already-parsed argument slice sizes excluding metadata-named keys.

`flip-work-window=100` is the default. Zero makes the helper return without recording and closes no
windows. Windows are command-count based. A deep parse pass can carry a window just over N, but it
pays one close comparison at the end of that already-completed pass; detection latency therefore
continues to scale with work rather than elapsed time.

`flip_signature()` turns pass and class counts into probability vectors and keys/value bytes into
per-command means. `FlipShiftDetector` EWMA-smooths the signature by halves. Its normalized L1
distance gives equal weight to the four feature families. Scalar distances use one key/byte as the
measurement quantum, avoiding the zero-anchor discontinuity where one rare admin argument would
otherwise have distance one.

The automatic band is twice the maximum quiet adjacent/drift distance learned at
the final split, floored by the four-family sampling quantum divided by observed commands.
It has no public override. Final anchor learning consumes a number of closed aggregates derived
from the provisioned thread count. It is
done after maneuver-only age sampling is disarmed, so the controller cannot detect its own signal
cost transition as a workload change.

The detector remains active during a seek. After it learns the current maneuver signature, another
shift abandons all recorded split/rate readings and restarts capacity measurement. If an ownership
FLIP has already committed, the restart is deferred until that one transaction reaches `Idle`;
another FLIP is never overlapped.

## 2. Jump with deliberate overshoot

The boot maneuver remains pending until the command-rate EWMA's relative slope across five ticks
falls below a threshold derived from its own change jitter and measurement quantum. Directional
connection ramp-up therefore remains drift, while jittery stationary traffic can qualify even when
its absolute rate never enters a quiet band. A 30-second ceiling, converted to ticks from
the internal observation cadence, caps deferral under non-idle traffic. Traffic at or below one
command per provisioned thread per tick resets the learning/cap state, so an idle server remains in
`awaiting-load-stability` and never pays for a maneuver it cannot use.

`FlipController::start_maneuver()` arms the existing enqueue-age machinery at runtime. Its rate
derives from observed command volume and the three-tick decision window, and adapts on completed
traffic windows; see [DESIGN-KNOBS.md](../DESIGN-KNOBS.md). IO and EX loops apply the published
rate to their own `LoopSignals`, preserving single-owner writes. IO releases its ROB-head age
map when the rate returns to zero.

After a stabilized baseline rate, `issue_initial_jump()` subtracts the per-thread maneuver window.
For each role it computes:

```
spin_frac_i = delta(spins_i) / delta(iterations_i)
cap_role = sum(delta(ops_i)) /
           sum(delta(busy_ns_i) * (1 - spin_frac_i))
demand_role = 1 / cap_role
nio_eq = round(total * demand_io / (demand_io + demand_ex))
```

Both role-constraint guards use the role's busiest corrected-busy thread. Thus a loaded owner is
not vetoed by idle peers. If either role has no measured work/busy quantity, measurement continues;
the controller does not invent a workload prior.

The calculation is performed in placement units (one logical CPU normally, one sibling pair when
the allowed topology contains complete pairs) and then reported/committed in threads. The target
passes `nio_eq` in the measured direction by `max(1, abs(nio_eq-nio_now)/2)`, clamped to one live
unit of each role. Even when the
equalized split rounds to the current split, the direction of greater measured demand supplies the
deliberate one-unit overshoot.

## 3. Return on throughput

`sample_stabilized_rate()` derives command rate from the monotonic, unique public-command counter
in each `ThreadCtx`. It invalidates both subwindows whenever any of these moves:

- the FLIP stage or continuous-LB stage is non-idle;
- snapshot/load state is active;
- `flip_completed`, `flip_refused`, client rebinds, key-bucket moves, or client moves changes.

Only two consecutive quiescent subwindows within the learned rate band produce a reading; their
mean is the stabilized rate. During final settling, the rate band retains twice the maximum
quiet-state pair/range jitter seen across the thread-count-derived anchor-learning period, and the
anchored rate is the mean of those stabilized readings. No rate sample taken across redistribution
can enter `readings_` or a comparison.

The initial stabilized split/rate is recorded before the jump. Thereafter `seek_after_reading()` is
rate-only: signal counters are not consulted. The first seek step is the overshoot distance. After
every stabilized reading it keeps direction on improvement, reverses on worsening, and halves the
step with a floor of one placement unit. At unit step, revisiting a stabilized split or observing
two reversals terminates the seek. `settle()` selects the argmax of all stabilized readings. If that
split is not current, it issues one final ordinary internal FLIP and waits for fresh stabilization
there before anchoring.

## 4. Anchor

`FlipController::anchor()` stores the final IO/EX split, stabilized public-command rate, anchored
fingerprint, and rate/fingerprint bands. It publishes age sampling zero and holds. While anchored,
redistribution-free, in-band two-window readings update the rate reference with a 64-observation
EWMA. Auto mode may widen the rate band from that EWMA's live innovation jitter, but retains the
band learned during final settling as a floor. Out-of-band trigger evidence and maneuver windows
never enter either baseline.

While anchored:

- a normalized fingerprint distance beyond the anchored band starts one maneuver;
- two paired subwindows above the prior live `anchor_rate * (1+rate_band)` start one surge maneuver;
- two paired subwindows below the prior live `anchor_rate * (1-rate_band)` start one collapse
  maneuver;
- `DEBUG FLIPCTL TRIGGER` posts a cold forced-trigger flag for the main owner.

A rate increase can also move the load-sensitive parse-depth fingerprint. If both detectors cross
together, one pending out-of-band rate observation is allowed its required second sample before the
fingerprint can preempt it; a pure mix shift with no rate evidence still fires immediately.

A trigger resets the maneuver's readings and capacity baseline. Trigger source counters make boot,
fingerprint, rate surge, rate collapse, and forced causes independently observable.

## Integration and observability

- `src/core/config.h` is the sole parser/default source for `flip-auto` and
  `flip-work-window`; `src/cmd/t_server.cc` exposes them as immutable CONFIG values.
- `tomokv.conf` documents the numeric grammar and off/auto semantics.
- `INFO FLIPCTL` reports state/phase, anchor split/rate, bands, total/per-reason trigger counts, and
  the last trigger reason.
- Boot-gated `DEBUG FLIPCTL` dumps controller phase, bands, direction, step, pending split, and all
  stabilized visited split/rate pairs. `DEBUG FLIPCTL TRIGGER` requests a forced maneuver when the
  controller is enabled.
- The only FLIP accommodation is that `flip_build_client_plan(nullptr, ...)` treats an automatic
  transaction as having no pinned command client, and `finish_flip_command()` has no ROB reply to
  complete for that form. Manual `FLIP io ex` keeps its pinned coordinator and asynchronous reply.
  All transaction stages, deadlines, rollback, wakeups, conservation checks, and transfer plans are
  otherwise shared unchanged.

## Functional validation

- `tests/flipctl_unit.cc` scripts quiet count noise followed by a simultaneous pipe/class/key/value
  mix change, and checks the command-work window/off behavior.
- `tests/flipctl.py` is the small port-7845 Python driver. It asserts that a connection ramp cannot
  start boot, varies think-time with a stationary six-second pattern while retaining the same
  BITCOUNT mix, and requires a maneuver within the non-idle deferral cap. The eventual low-load
  anchor must be off-rail and hold for 60 seconds; the driver then shortens think-time on those same
  single-frame connections for exactly one surge maneuver without changing their pipeline
  fingerprint. It finishes with one BITCOUNT-to-INCR fingerprint maneuver and uses no memtier.

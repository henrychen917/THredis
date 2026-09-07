# t-flipdamp lane notes (THIS FILE IS MY MEMORY -- keep current)

> Historical research record: configuration recipes and measurements below describe earlier
> revisions. Deleted controls are not accepted by the merged server, and the manual-map
> dispatch-scaling instrument is retired. For current controls and merge dispositions, see
> [DESIGN-KNOBS.md](../DESIGN-KNOBS.md) and [MERGE.md](../MERGE.md).

## Assignment (owner, 2026-09-05 relaunch after 19:20 usage-limit kill)
Defect: `--flip-auto 1` (2s) lost 19.5% on multi-key 18:14 (MSET8+MGET8 1:1 p32, 512 conns) with
3 flips, 993 clients moved, target 18:14 unchanged. Do: (1) A/B matrix 3 regimes + pure-GET null,
same-binary null first; (2) if the guard is not enough, successor policy = cost gate + window/
variance + outcome loop, ONE feature file; (3) directed test + batteries + differ; (4) PRE/POST table.
Report commit hash + exact flags. Never merge into t-merge14.

## Box rules (non-negotiable)
Cores: physical 52-57 + siblings 180-185 only. Server 52,53,180,181; loadgen 54-57,182-185.
Ports 8220-8229. Gate: $SP/quiet.done exists AND older than 3 min (find -mmin +3) before make or
bench; ps/taskset intruder scan first. Kill by PID only. Pinned compile: taskset -c 52-57,180-185.
Scripts: scratch/lib.sh (geometry+gate), gatewait.sh, ab.sh, splits.sh, hold.sh, mk.sh, sk.sh.

## State reconstructed 2026-09-05 evening (transcript lost)
HEAD 66d4c13a3 (WIP salvage, unreviewed) on top of b5258d620 (WIP) on merge base e902c67d5.
build/tomokv (14:50) is NEWER than all sources (14:49) => binary == HEAD source. PRE binary =
/home/user/Projects/wt-flipdamp-base/build/tomokv (e902c67d5, built 12:04).

### What the fix already does (code comments are the evidence trail)
1. ROOT CAUSE (found by predecessor): issue_initial_jump divided each role's busy_ns by its OWN op
   counter; ex counts one op per SHARD TASK (7.576 per MGET8 command) so ex looked cheap and the
   model wanted io=82% -> flipped 5:3->7:1 (0.29M vs 0.95M) -> walked back = "3 flips, 993 clients,
   target unchanged". Fixed: demand share = busy-time share (command count cancels). Single-key is
   unaffected (1 command == 1 task) -- that is why the defect was 40x workload-dependent.
   => The owner's brief ("client-weight actuator, role actuator never fired") is the WRONG theory.
   flip_completed counted ROLE flips (a round trip); each role flip re-plans ALL client weights across
   surviving io threads (server.h ~900-950) which is why ~65% of connections move per flip.
2. pass_depth removed from the fingerprint trigger distance (actuator moves it: sweep-abandon law);
   still dumped for diagnosis (DEBUG FLIPCTL dist_parts).
3. Band quantum 1/sqrt(N) (estimator noise) instead of 4/N.
4. Model hold: 3-sample window of io_frac; hold (no flip) when equal_units==now or |mean-now|<=spread.
5. Null-maneuver outcome backoff: a maneuver ending on its origin doubles the confirming passes the
   responsible detector needs (cap = anchor learning windows); halves back when a maneuver moves.
6. Fingerprint shift needs shift_confirmations_ (>=2) consecutive out-of-band windows.
7. issue_flip refuses target == live split.
### Evidence on file ($SP)
- ab-base-mk-clean vs ab-fix-mk-clean (5:3, 8 cores, 12:28-13:32): BASE fa=1 always 2 flips ~440
  clients, -15%; FIX fa=1 3/6 held (rate ~ fa=0) and 3/6 round-tripped (-15%).
- ab-*-mk-r (15:16-15:22, co-tenancy, rates void): FIX fa=1 4/6 model holds, 2/6 round trips.
- hold-fix3-{1,2,3}: directed test 3/3 pass; hold-base2 fails (fingerprint re-trigger on base).
  BUT hold-fix3-3 shows rate_surge=1 + rate_collapse=1 during the constant-mix hold => 2 flips, 17
  transfers (test does not assert on the rate detector). null_maneuvers=3, model_holds=2.
### Open problem => successor policy needed
The 3-sample window still lets a biased-but-tight window pass (2/6 boots). Rate surge/collapse
triggers on a stationary paced load also start maneuvers. Each non-held maneuver = seek = 2-3 flips
+ 2 stabilised windows at a bad split (-15..-20% over a 20s cell).

## Plan
1. gatewait -> same-binary null (fix fa=0 vs fa=0) on mk -> rig noise.
2. splits.sh mk 1:3 2:2 3:1 (fa=0) -> true optimum on this rig.
3. Matrix: base0/base1/fix0/fix1 x {mk, sk1:1, sk9:1, get}, 20s cells, 3 rounds ABBA.
4. Successor policy in flipctl (variance-sized model window + cost gate in throughput space +
   outcome loop on the seek), re-run matrix, hold test, flip batteries, differ, PRE/POST table.

## Log
- 19:3x: resumed; box busy (owner memtier on 64-127 -> port 8034); quiet.done MISSING; scripts
  rewritten for new geometry; waiting on gate.
- 19:45 gate opened. Built guard binary copy -> $SP/fd-tomokv-guard (66d4c13a3). Wrote policy:
  src/core/flip_policy.h (pure: FlipDemandWindow Welford, flip_projected_rate = min(s/f,(N-s)/(1-f)),
  flip_choose_split sequential MOVE/HOLD/keep-sampling) + flipctl.cc decide_placement (saturation
  gate: both roles headroom > band => hold-unsaturated; cap = deferral/tick/2 readings) +
  seek_after_reading verify-or-revert + anchor() round_trips_/model_margin_ (x2 on round trip,
  /2 on delivered, cap where band*margin >= 1). Unit test extended (defect numbers). Commit: see git.
  Unit test caught: "optimum at the mean" is NOT a hold when the interval admits a paying split ->
  hold only when no split clears the bar optimistically.
- 19:52 matrix launched detached: $SP/fd-matrix.csv (30s cells, 3 rounds, arms pol0a pol0b pol1
  guard1 base1; wl mk sk1:1 sk9:1 get). Timelines: $SP/fd-tl-<arm>-<wl>-<round>.txt. Log fd-matrix.log.
- 19:58 mk rounds 1-2 (30s cells, 2:2 of 4 threads, 256 conns p32): pol1 = ZERO flips both cells
  (model hold at first reading => saturation gate: both roles had headroom > band => this rig is
  LOADGEN-BOUND for mk with 8 memtier HW threads vs 4 server HW threads). guard1 2 flips/363-372
  clients each cell; base1 2-3 flips/370-548 clients, one cell ended LIVE 3:1. Rates unusable:
  same-binary null pair differs 13% within a round (pol0a 447k vs pol0b 516k); box load 21, another
  lane on 48-50 (my L3 CCX). guard1 r2: p99 = 30015 ms = connections stalled the whole cell (rate
  184k). pol0b r2 (fa=0, no flips) p99 1003 ms -> 1 s stalls happen without flips too. Cause unknown;
  server logs clean. NEXT: kill matrix after mk r3; satprobe (find server-bound loadgen params);
  flipprobe (explicit FLIP cost/stall with memtier per-second lines); relaunch 60s cells with
  memtier output captured; rate verdict needs the owner's quiet box -- counters are the evidence here.
- io idle span = arm_blocked + submit_and_wait(1) only after a sweep found nothing => idle_ns is a
  fair "nothing to do" measure; the saturation gate is sound.
- 20:04 matrix mk (15 valid cells, 30s, 2:2/4 thr): pol0a 480k pol0b 514k (same-binary null 7%
  apart, 17% intra-arm spread) | pol1 394k (-21%) ZERO flips 4 trig 4 holds | guard1 281k, 6 flips
  1020 clients, 2 cells p99=30s STALL | base1 440k (-11%) 8 flips 1316 clients, twice ended live 3:1.
  16th row = artifact of editing ab.sh while running (bash re-read at shifted offset) -> NEVER edit a
  running script. Self-match trap hit once (pgrep -f pattern in my own cmdline + cwd check) -> use
  `pgrep -f` only from a shell whose cmdline does not contain the pattern (a script file).
- satprobe mk p32 c32 t8: 525k, server io=0.86 ex=0.935 busy, memtier 27% of 8 HW threads =>
  neither side CPU-saturated: closed loop is latency/wake-up bound. Saturation gate = "capacity
  model valid only when a role is CPU-saturated"; hold otherwise (comment fixed). pol0 cell with
  fixed parser: io 0.90 ex 0.97.
- OPEN: pol1 zero-flip loss (-8/-28/-31%, always the middle cell, after a stalled guard1 cell in
  the bad rounds). Test when gate reopens: diag1 (pol0/pol1 alternation 40s x2 + flipprobe) with
  per-second traces -> cellview.py (rate by controller phase).
- 20:04 gate CLOSED (owner measuring). gatewait running.
- 20:46-20:55 diag2 (gate-then-run): pol0 470/479/478k vs pol1 502/478/493k (+3%, ZERO flips in 3/3,
  6 triggers all held) => the earlier -20% was rig noise/ordering, per-phase means flat.
  flipprobe (explicit 2:2->3:1->2:2 under mk): pol 386k mean, base 387k; 3:1 runs 180-230k vs
  480-520k at 2:2 (-60%); the flip transient is ONE second each way; 360-384 clients per round
  trip. => cost of a move = time at the wrong split, not the flip mechanics.
  BIAS FOUND: model_io_frac 0.66-0.73 with headroom_ex 0.47-0.75 on a workload whose busy shares
  are io 0.87/ex 0.97: sample_role_demand's spin correction busy*(1-spins/iterations) treated an
  empty spin pass like a task batch (ex loop enters the busy span every pass, spins outside it).
  The saturation gate held ONLY because the same wrong number inflated ex headroom; the model would
  have projected 3:1 +35%. FIX (4d8261d99): ex loop books an empty pass (did==0) as idle via a local
  pass_ns (monotone counters, 1 branch); controller uses raw busy/idle; ThreadMeasure = ops,busy,idle.
  guard binary stall (p99 30s in 2/3 mk cells) NOT reproduced on base or policy flip probes.
- 20:56 final.sh launched via gaterun: matrix2 (pol0a pol0b pol1 base1 x mk sk1:1 sk9:1 get, 40s x3)
  -> hold x2 pol + base -> batteries (8 srv threads 52-55+sib, 6:2) -> modes -> differ. Logs fd-final.log.
- 21:04 COORDINATOR: no per-cell reports (each wake costs the shared budget); ONE message at the end
  with full PRE/POST matrix (rate, flips, clients moved, p99 per regime) + verdict + commit hash.
  Owner holds the box marker ~1 h (acceptance window). final.sh aborted on the marker at mk r3
  (10 mk rows). Continuation = finalw.sh (pid 1929685, via chain.sh): PAUSES on the marker, resumes;
  runs missing rounds (ROUND_OFFSET, abw.sh), hold x3, batteries, modes, differ, report.py ->
  $SP/fd-report.html, touches fd-final2.done. Monitor b17mdlkwl notifies once (done or pid gone).
  matrix2 mk so far: OFF 504/498/510/517/521/524k; POST 488/498k (0 flips, 4 triggers held);
  PRE 460/435k (2+4 flips, 371+660 clients). If killed: rerun `scratch/finalw.sh` (idempotent),
  then publish fd-report.html as the artifact and send the single report message.

## 2026-09-06 night: VERIFICATION lane (owner: verification only, no actuator redesign)
State at resume 00:29: HEAD 4e8edf37a, tree clean, box gate CLOSED (quiet.done missing, load 40,
other lanes on 64-127 + a tomokv-post on 8260). Nothing of mine was running.

### What the killed session left behind (read from files, not memory)
- fd-matrix2.csv had ONLY the mk regime: finalw.sh line 11 counted rows with
  `grep -c ",$wl," || echo 0`, and grep -c prints "0" AND exits 1 on no match, so the substitution
  produced "0\n0" and the arithmetic died -- sk1:1, sk9:1 and get were silently skipped. FIXED
  (awk over the CSV shape). LAW for this harness: never count with `grep -c || echo 0`.
- report.py read only fd-final.log while finalw.sh logged to fd-final2.log -> "0 hold lines,
  0 battery lines, 0 differ lines" in the report. FIXED (reads the whole chain).
- differ was run as `differ_gate.sh ... 2:2`; its sort suite REQUIRES --shards 16 --ratio 6:2, so
  4 of the 4 "failures" were my invocation (pass=164 fail=4, all four `differ sort`). Re-run at 6:2.
- `--thread-mode 1s --flip-auto 1` is REFUSED by config (flip needs 2s). The mode row has to be
  1s-plain + 2s-with-flip-auto, not the combination.

### THE REAL FINDING: tests/flipctl.py (a GATE row) fails on this branch
Rail anchor 1:7 on 8 threads. DEBUG FLIPCTL: model_io_frac=0.0061, headroom_io=0.994,
headroom_ex=0.0000, origin_rate=4898.601, anchor_rate=6000.916, boot_rate_slope=0.0229 against its
own threshold 0.0297. So: (a) the driver is 3 connections of BITCOUNT over 4 MB bitmaps -- ex IS
saturated and io IS idle, the saturation gate opened correctly and work conservation really does
rate 1:7 at 3.5x; (b) the +22.5% that CONFIRMED the move was the driver's own ramp, still trending
under the deferral threshold. Base does not rail because it random-walks with halving steps and
settles on the best of several readings; my verify-or-revert takes ONE probe and compares it to a
single pre-flip reading.
FIX (4802ba52d): Measuring never moves the split, so its readings are readings of the ORIGIN.
Bracket them (min/max) and floor every band of the maneuver at 2x that spread -- the same
2x-observed-jitter convention as band_, the signature band and the rate band. Ramping driver =>
floor 0.40, the ramp's 0.225 confirms nothing, seek reverts to 6:2, anchors off-rail. Still
baseline => floor ~0, nothing changes. Pure helper flip_baseline_band() + unit rows; DEBUG FLIPCTL
dumps origin_rate_readings/min/max/baseline_band.
NOTE the binary changed => matrix2 rows are a different server. Kept as fd-matrix2-prev.csv; the
report reads fd-matrix3.csv.

### Tonight's chain: scratch/ver.sh (detached, pid in $SP/fd-ver.pid, log fd-ver.log)
Pauses on the box marker at every step, skips any step whose output is on file, touches
$SP/fd-ver.done at the end. S1 build+unit, S2 flipctl.py base x2 vs fix x2, S3 matrix3
(mk/sk1:1/sk9:1/get x 3 rounds x pol0a/pol0b/pol1/base1, 40 s), S4 hold test, S5 NON-VACUITY
(boot at 3:1 -- ~190k vs ~500k at 2:2 -- and require fa=1 to still move; fa=0 must not),
S6 instr/op + cycles/op at a matched rate via memtier --rate-limiting (base fa0 = hot path,
fix fa0, fix fa1 = always-on cost), S7 batteries 2s + fused both atomic, S8 differ at 6:2,
S9 report. Re-running `scratch/ver.sh` after any kill resumes it.

### RESULTS 2026-09-06 (ver.sh 00:43 -> 04:35, box marker held for ~100 of 230 min)
Binaries: fix sha 7c78940416c87e24 (build 00:57), base sha 692984a8c786998b (e902c67d5).
MATRIX3 (4 regimes x 3 rounds x 4 arms, 40 s cells, ABBA), ops/s mean | flips | clients | p99 med:
  mk     OFF 522k/520k (floor 3.2%) | POST 520k -0.3% 0 flips 4 trig 4 holds p99 63 | PRE 475k -8.9% 7 flips 1464 p99 80
  sk1:1  OFF 5175k/5150k (3.2%)     | POST 5103k -1.2% 2 flips 1 rt p99 3.1        | PRE 4508k -12.7% 12 flips 2052 p99 8.8
  sk9:1  OFF 5101k/5114k (0.4%)     | POST 5125k +0.3% 0 flips 4 holds p99 3.5     | PRE 4390k -14.1% 12 flips 2072 p99 6.5
  get    OFF 5303k/5297k (0.9%)     | POST 5308k +0.1% 0 flips 3 holds p99 3.8     | PRE 4818k  -9.1% 10 flips 1720 p99 5.4
THRASH (flips AFTER first anchor): POST 0 in all 4 regimes; PRE 1/6/5/4 (mk/sk1:1/sk9:1/get).
The single POST move: sk1:1 round 3 -- moved, did not deliver, reverted, round_trips=1, margin 1->2.
NON-VACUITY (boot at 3:1, mk, 60 s): fa=0 base 245k / fix 236k, live 3:1. fa=1 base 307k, 2 flips,
293 clients, ended STILL MANEUVERING at live 3:1 (round trip). fa=1 fix 442k, ONE flip, 154 clients,
anchored 2:2 at anchor_rate 516k and held; its second trigger (the rate surge its own flip caused)
was held as hold-unsaturated. Reference fix at 2:2 fa=0 = 530k.
GATE ROW tests/flipctl.py: PRE-FIX policy binary FAILED (rail 1:7). base 2/2 PASS, fix 2/2 PASS,
all four anchored off-rail at 6:2 on a 6000 ops/s driver.
INSTR/OP at matched rate (307.24k +-0.02% all six cells, 35 s perf window, mk 2:2):
  base fa0 32449 instr/op 26317 cyc/op | fix fa0 32025 (-1.31%) 26381 | fix fa1 32423 (+1.24% vs fix fa0) 26491
  same-arm spread 0.42-0.49%, so hot path is unchanged-to-cheaper and the always-on controller is
  +1.2% instr / +0.4% cycles -- inside the 3% budget.
HOLD directed test: pol 2/2 PASS (0 flips, 0 transfers); base FAILS (1 flip, live 3:1).
BATTERIES: flip/flip_under_load/flip_ttl PASS; fused s6/multi_exec/edgeproto/atomfix PASS both
atomic; spinprobe + idle-ceiling PASS 2s, 1s(atomic 0/1) and on BASE (ver2.sh -- spinprobe takes the
server PID, gate.sh passes $SRV; ver.sh had handed it the binary path).
MODES: 1s refuses --flip-auto 1 by config; 2s boots awaiting-load-stability.
DIFFER: 168/168 at --ratio 6:2 (the sort suite REQUIRES 6:2 + --shards 16; last night's 4 "fails"
were the 2:2 invocation).
Artifact: https://claude.ai/code/artifact/f01d7b82-5685-4f10-bf3f-e89010857b35

## 2026-09-06 day: ACTUATOR REDESIGN lane (owner: "be innovative about flip arithmetic and signals")
Base: d84031d2f (guard verified; do not re-measure). Build dir per arm: guard binary copied to
$SP/fd-tomokv-guard-d84031d2f (sha 7c78940416c87e24) BEFORE the first make; base = wt-flipdamp-base
(692984a8c786998b). Box marker MISSING at 09:10-09:22 (owner gate until ~09:40): design + code first.

### THE SIGNAL DEFECT (from the guard night's own lbsignals snapshots, sk1:1 r3, 40 s window)
  thread 0 io: busy 16.36 s + idle 0.17 s = 16.53 s booked of cpu_ns 39.92 s  -> 23.5 s UNBOOKED
  thread 3 io: busy 22.24 s + idle 2.40 s = 24.64 s booked of 40.02 s          -> 15.4 s UNBOOKED
  thread 1 ex: 25.62 + 2.56 = 28.19 of 39.99; thread 2 ex: 33.25 + 1.76 = 35.01 of 39.98
  mk r1:       io 27.6+3.5 = 31.1 of 38.1 (7 s unbooked); ex 38.7+1.2 = 39.9 of 40.0 (complete)
The io loop's busy Span closes BEFORE ring_.submit_and_reap(): the io_uring_enter syscall -- the
kernel doing the TCP send/recv, which IS io work -- is booked as neither busy nor idle. The share
busy_io/(busy_io+busy_ex) therefore read io = 0.32 on sk1:1 (headrooms 0.5%/3%, i.e. both roles
saturated => the true work share is ~0.51). R(1)=min(1/.32, 3/.68)=3.13 > R(2)=2.94 => the model
moved 2:2 -> 1:3, measured 2.45M vs 5.12M (-52%), reverted: the guard's ONE round trip. mk read
0.41 for a 0.477 workload (same direction, smaller: fewer syscalls per busy second).
FIX (signal): work = wall - idle. idle_ns is the one quantity both loops book faithfully (io: the
blocked wait after an empty sweep; ex: empty passes + blocked wait, since 4d8261d99). wall = the
controller's own tick clock (now_ms delta) x threads of the role. No hot-path change at all.
  sk1:1: io 2x(40-1.29)=77.4, ex 2x(40-2.16)=75.7 -> f=0.506 -> HOLD.  mk: f=0.472 -> HOLD.
headroom := idle/wall (was idle/(busy+idle), inflated 2.4x for io thread 0).

### DESIGN (one page): cost gate + variance window + outcome loop, all in src/core/flip_policy.h
UNITS: commands (delivered work), seconds (tick clock), clients. R0 = origin stabilized rate [cmd/s].
g = projected relative gain of the argmax split (model R(s)=min(s/f,(N-s)/(1-f)); the step IS the
distance: jump to the argmax). kappa = the model's calibration (below). All terms are measured or
derived from the controller's own mechanics; no machine constants, no new knobs.
1. COST GATE -- a move must pay for itself within the stationarity the workload has demonstrated.
   T_stat  = now - stationary_since  (boot: first non-idle tick; change trigger: the trigger)
   T_black = T_flip + T_settle + (n_t - 1) T_read   [blind time before the outcome can be judged]
             T_flip measured per flip (issue -> Idle, tick-quantized; first flip: 1 tick),
             T_settle = 3 ticks (window reset + 2 sub-windows = the controller's reading mechanics),
             T_read = 1 tick per extra stabilized reading.
   C_xfer  = c_client x n_pred      [commands]
             c_client = sum(lost commands) / sum(clients moved) over every flip so far
               lost = R_before x dt_flip - commands served in dt_flip  (measured, clamped >= 0)
             n_pred = clients x |dio| / max(io0, io1) x rho, rho = sum(actual moved)/sum(naive)
               (the weighted re-plan reshuffles more than the converted threads' clients)
   P_miss  = (misses + 1) / (moves + 2)           [Laplace; no prior constant]
   benefit = kappa g_low x R0 x (T_stat - T_black)  [gain credited only after verification]
   cost    = margin x [ C_xfer (1 + P_miss) + P_miss x kappa g_mean x R0 x T_black ]
             (the revert with probability P_miss; a wrong move loses during the blackout about
              what a right one would have gained)
   MOVE iff benefit > cost AND kappa g_low > band x margin (the noise bar, kept: an unverifiable
   gain cannot pay). Else keep sampling (T_stat grows, the interval tightens) until the reading cap
   -> "hold-cost". First flip: c_client unknown = 0 (the flip IS the measurement; charged through
   the blackout term only).
2. WINDOW from VARIANCE -- sigma = relative stdev of the origin's Measuring readings (Welford,
   n_o >= 3). Planned target readings n_t = ceil(1 / ((kappa g_mean / 4 sigma)^2 - 1/n_o)); a
   non-positive bracket means the origin is not yet measured precisely enough to verify a gain this
   small -> keep sampling the origin. Threshold after k target readings:
   theta_k = max(2 sigma sqrt(1/n_o + 1/k), baseline bracket 2(max-min)/mid, typed band).
   Sequential at the target: accept when d_k > theta_k (early or at n_t), reject early when
   d_k < -theta_k (never sit at a clearly worse split), at k = n_t accept iff d > theta.
3. OUTCOME LOOP -- every move is a hypothesis (predicted kappa g_mean, planned n_t).
   hit  -> anchor at target; margin = max(1, margin/2); record delivered/predicted.
   miss -> flip back; margin x2 (cap where band x margin >= 1); misses++; delivered := 0.
   invalidated: after the return flip, if the origin no longer reads R0 within theta the baseline
   moved during the maneuver -- the test was voided, not failed: margin/misses untouched, counted
   as invalidated_maneuvers (the guard doubled the bar on these too).
   kappa = (sum delivered+ + gbar) / (sum predicted + gbar), gbar = mean predicted gain over moves:
   the model starts with the credence of exactly one delivered move of its own average size and
   earns or loses it; one miss halves every future projection (the bar doubles in effect), a hit
   restores it; kappa <= 1 (over-delivery never buys credit). Magnitude, where the margin is sign.
WHAT EACH REPLACES IN flipctl.cc: sample_role_demand busy share -> (wall-idle) share + idle/wall
headroom; decide_placement gains the cost gate + verify-window feasibility (decisions hold-cost,
hold-unverifiable); WaitingFlip measures the flip (lost, moved, duration) -> FlipCostModel;
seek_after_reading: one reading -> sequential Welford verification + outcome; anchor(): miss vs
invalidated finalization; report/debug/INFO: new fields; DEBUG FLIPCTL seek <io> [force] and
cost <cmds/client> are the directed-test hooks (cold, debug-gated). Knobs: none added.

### Log (redesign day)
- 09:36 marker appeared; 09:40 build (601d65d4a) rc=0, 0 warnings; layout static_asserts hold. Unit
  rc=0 after fixing a rounded literal in my own row (73fc439c3). Arms: red e7d9a6f2e7b6abf8
  (build/tomokv) | guard 7c78940416c87e24 ($SP/fd-tomokv-guard-d84031d2f) | base 692984a8c786998b.
- 09:44 smoke: tests/flip_cost_gate.py by hand on port 8221 (3:1, BITCOUNT load ~2.8k/s, COST 1e9
  injected, boot-pending). Then launch scratch/red.sh detached (resumable; it reuses fd-costgate-1).
- 09:52 smoke run 1 (601d65d4a): phases 1+2 PASS (model proposed 1:3 from 3:1 with io share 0.027,
  headroom io 0.987/ex 0.000, gain +200%; typed cost 1e9 -> hold-cost, 0 flips, live 3:1; measured
  cost restored + TRIGGER -> ONE flip to 1:3, moved-delivered, kappa 0.729 = (0.92+2)/(2+2): the
  closed-loop driver delivered +92% of a +200% projection). Phase 3 FAILED on the label: the
  induced -48% miss was booked moved-reverted-INVALIDATED because the driver came back 2.2% faster
  after the round trip and the bare distance test used a 0.14% threshold (sigma 0.02%). Fix
  733c6d842: re-judge the delivered gain against the re-measured origin (flip_corrected_gain);
  void the miss only if the corrected gain would have PASSED. Rebuilt 450630169ea1411e, re-running.
  Also seen: the model's induced-hypothesis projection for 3:1 was -48.6% vs -47.9% delivered.

## 2026-09-06 pm: COORDINATOR CORRECTION folded in (owner-box acceptance of the guard)
Owner box: on 64 shards + 8-key multi-key, 18:14 is FAR from optimal (more executors win: guard
moved to 11:21 = +89%, one run to 10:22 = +25x). So the guard's real cost is TIME-TO-FIRST-MOVE
(it moved once, late, still 'maneuvering' at 40s) and ONE hold-on-a-terrible-split (held 28:4 at
0.53M, model_holds 1). BASE-auto (t9final) is now the bar (gets there fast, then thrashes back).
=> The cost gate must be ASYMMETRIC. Changes (commit 62d85d3d8):
  1. flip_cost_gate blackout miss-cost = P_miss*R0*T_black (was P_miss*gain_mean*R0*T_black). The
     blackout risks the WHOLE origin throughput on a miss, not the gain -- so benefit/cost ~ gain and
     the stationarity a move needs FALLS as the gain rises: +2400% pays at ~1.0 T_black, +5% needs
     ~44s. No constant; the wait is the distance's consequence.
  2. kMinModelSamples 3->2: two draws admit a variance and the sequential 2-SE interval IS the
     distance-derived wait (wildly-wrong split's pessimistic gain clears the bar at 2 readings,
     marginal doesn't). Outcome loop, not a long pre-move window, catches a 2-draw miss.
  Unit rows carry the owner-box numbers; UNIT green (asymmetric). Directed test 26 checks green on
  the PRE-asymmetric binary (a19f4fba); re-queued on the new binary.
Harness v2 (commit 41d216ab9): scratch/red2.sh -- arms OFF / BASE-auto(t9final) / guard(flipguard) /
redesign; S3 = 120s TIME-TO-FIRST-MOVE cells with a 1 Hz trace (scratch/ttfm.py parses
time-to-first-move / moves-after-stab / steady-state as 3 numbers) at boots 2:2 (matched here),
3:1, 7:1(8thr); 40s matrix (fd-matrix5.csv) kept as thrash-count only; perf; gate row; batteries;
differ; report2.py -> fd-report2.html. Shared bins: $SP/bin/tomokv-t9final, $SP/bin/tomokv-flipguard.
Report artifact (redesign): https://claude.ai/code/artifact/fce9ba91-db6b-4e62-815f-9e2cc533a1fa
STATE at handoff: box held by the owner's full gate since 09:53 (5x tests/gate.sh full on the box);
red2.sh is DETACHED + gate-waiting (setsid, pid in fd-red2.log's process; log fd-red2.log). It builds
the asymmetric binary, runs S2-S7 and regenerates fd-report2.html when the box frees. TO RESUME after
a kill: `scratch/red2.sh` (idempotent). The published artifact must be RE-PUBLISHED from the local
fd-report2.html once the chain fills it (a bash chain cannot republish an Artifact).
COMMITS this session (t-flipdamp): cc22ca4ab notes, 601d65d4a redesign, 73fc439c3 unit+report,
733c6d842 + d764d1958 invalidation rule, aa4b82083 unit tol, 62d85d3d8 asymmetric gate,
41d216ab9 harness v2, 0bf757612 report banner. Base of the lane: d84031d2f (verified guard).

## 2026-09-06 16:20 resume (after a usage-limit kill): chain results + a THIRD defect fixed
red2.sh ran detached 12:43-14:08 on the asymmetric binary (a19f4fba-era + asymmetric = the 12:43
build): BUILD/UNIT ok; COSTGATE x2 26/26; HOLD x2 183/183; 12 TM cells (120 s, 1 Hz trace);
thrash matrix 60 rows (red1: 0 flips in all 12 cells, mk 535/531/539k vs OFF 524-539k, base
t9final 434-499k with 2-4 flips); PERF base0 31997 / red0 32389 / red1 31781 instr/op (same-arm
spread 1.4-2.3% today, so always-on cost is inside noise, <= budget); batteries + modes + differ
168/168 all PASS. BUT tests/flipctl.py FAILED x2 (rail 1:7) and the 120 s red-22 cell ended still
"measuring" after 5 triggers (rate_band 0.00019).
ROOT (both): bands learned from stabilized PAIRS = quiet-selected. Gate row: two origin readings
inside one 6 s jitter phase read sigma 0.05%; target +24% = the driver's swing -> hit -> rail.
red-22: lucky-still settle -> anchor band 0.02% -> collapse detector fires on any wobble -> Measuring
can never take a pair within 0.02% -> stuck. The guard passed the gate row by luck of phase (3rd
reading straddled the swing); its 40 s cells hid the retrigger loop.
FIX f2c8fc9a4: FlipEwVariance = EW mean/var of EVERY per-tick rate sample (relative), time constant
= deferral bound in ticks (30); floors the pair band, verification band, hyp sigma, anchor band;
reset when a controller flip completes. Unit: triangle sigma > 0.10 floors the threshold above +24%;
still load < 2%. Build 923ec17396184cc5. rv.sh (r3 tags) re-runs gate row x2, costgate, red-22/
red-31/red8-71 120 s. ttfm.py: 3-field traces; stabilization starts at/after the first move.
TM (r2 binary): ttfm base 12/12/14 s but thrashes (3:1: 6 flips, steady 403k, ends 3:1; 2:2: 5
flips ends 3:1 at 240k) or lands poorly (8thr 6:2 579k); guard 21/20 s -> 2:2 506k / 3:5 695k;
red 23/18 s -> 2:2 522k / 4:4 697k, moved-delivered, 0 misses, 0 moves after stabilization.
LAW (marker): quiet_ok = exists AND age >= 180 s by stat arithmetic (find -mmin exit code is vacuous).
- 16:45 FOURTH iteration of the long-window estimator: the first form (deviation from an EW mean)
  blew up on the boot ramp (rate_ew_sigma 16.9 in the r3 gate-row dump; rate_band 0.70 in red-22)
  => the r3 "passes" were VACUOUS (hold-unverifiable / dead detector). Fix dcc5a409a: SECOND relative
  difference (ramp-invisible, level-shift blips decay, swing keeps amplitude), idle-floor break,
  >100% = level change reset, rms reported undivided (conservative sqrt(6) on white noise, exact on
  the 6 s triangle). Build 9b9b559a2e1a5eff. r3 re-run: gate row 2/2 PASS with round_trips 2 / 1
  (moved to the rail, judged, reverted -- a real revert, not a hold); COSTGATE 26/26.
- 16:56 r3 on 9b9b559a2e1a5eff (FINAL): gate row 2/2 PASS (round_trips 2/1: moved to the rail,
  judged, reverted); COSTGATE 26/26; TM red-22 0 flips hold-optimum steady 529k (OFF 520/534k)
  rate_band 0.20 (the mk load's second-difference rms ~10%: anchored rate detectors need a 20%
  move on this load; the mix detector is unaffected); TM red-31 ttfm 15 s -> 2:2 503k, 0 after
  stab; TM red8-71 ttfm 17 s -> 4:4 699k, 0 after stab, second trigger held. Report regenerated,
  republished at https://claude.ai/code/artifact/fce9ba91-db6b-4e62-815f-9e2cc533a1fa.
  FINAL CODE COMMIT dcc5a409a (src/core/flip_policy.h, flipctl.{h,cc}, server.h, main.cc,
  t_server.cc, tests/flipctl_unit.cc, tests/flip_cost_gate.py); harness/notes commits after it.

## 2026-09-06 17:30 owner-box acceptance of dcc5a409a (coordinator) + the landing question
120 s cells, 64 shards, 512 conns, mk p32; settled = mean t>=60. 18:14: OFF 1.539M | BASE 18 s ->
31:1 rail -> back at 24 s, 3 flips/973 clients, 1.531M | GUARD 16 s -> 11:21 held 2.106M | REDESIGN
19 s -> 13:19 held 1.941M, 0 moves after 60 s, holds 0, round_trips 0. 28:4: OFF 0.528M | BASE
12 s -> 31:1 -> back, 0.485M | GUARD 15 s -> 10:22 1.973M | REDESIGN 13 s -> 12:20 2.036M (best).
QUESTION: 13:19 one step short of 11:21 -- argmax, or a refused second step?
READING: argmax of the ORIGIN's demand reading; no second step was ever proposed (the maneuver ends
at moved-delivered and anchors; holds 0 / round_trips 0 = no later trigger), so the gate refused
nothing. Cause = signal/model: c_io is not split-independent (io_uring_enter time amortizes with
fewer io threads), so an io-heavy origin over-reads f (~0.39 -> 13/12); the guard's busy share
omits exactly that part (~0.33 -> 11/10) and lands closer by cancellation.
FIX 2a18b4a1d: after a delivered move re-measure the landing (fresh demand/origin windows), take one
more judged step if the local argmax differs and pays (measured transfer cost), stop on hold or a
visited split; local verdict exposed as flipctl_refine_decision / flipctl_refine_steps. Expected on
the owner box: 18:14 -> 13:19 -> (f_local lower) -> 11:21 or 12:20 at ~35-45 s (the +8% second step
needs ~25 s of stationarity under the asymmetric gate). Verification queued: scratch/rv2.sh
(build+unit+rv.sh) detached behind the marker (the coordinator's OFF sweep holds the box);
results -> fd-rv2.log / fd-rv.log / fd-r3-*.txt. NOT yet verified on any box at this commit.
- 17:36 coordinator's OFF sweep (settled 20-50 s): 16:16 1.964M, 14:18 2.013M, 13:19 2.046M, 11:21
  2.098M, 10:22 1.960M, 8:24 1.588M -> optimum 11:21; flat (2.5%) from 13:19 to 11:21, cliff past it.
  Reading confirmed (argmax two steps high on 18:14, one on 28:4; guard exact by cancellation).
  ee40cf940: refinement step must clear flip_refine_bar = predicted - delivered of the step that
  landed (hold-within-error) -> stops short rather than round-tripping or crossing the peak.
  HASH FOR THE FREEZE: ee40cf940. build/tomokv is still 9b9b559a (dcc5a409a) until rv2.sh's queued
  build runs (fd-rv2.log "BUILD rc=0 sha=..."); the coordinator freezes from the build dir after that.
  Expected on the owner box with ee40cf940: 18:14 -> 13:19 -> re-measure -> hold-within-error (bar
  ~0.10 vs a +5-10% projection at kappa ~0.86) -> anchor 13:19, 1 flip; 28:4 -> 12:20 -> bar ~1.1 ->
  hold-within-error -> anchor 12:20, 1 flip. Zero extra flips in the flat region by construction.

## 2026-09-07 04:35 fingerprint-band floor (coordinator's gate-hygiene finding) -- 3bf51b5b7
FINDING (their lane): stationary driver (rate spread 0.07% over 34 samples) fired
last_trigger=fingerprint-shift, distance 0.2518 vs band 0.0200 (12.6x). Cause: pass_depth (a
SCHEDULING outcome) in the distance + a band that never consults the signal's own movement.
ON THIS BRANCH, part 1 is already structurally absent: pass_depth was removed from the trigger
distance in the guard commit (sweep-abandon law). Measured on the SAME gate row, same driver, same
typed --flip-auto-band 2, 3 runs: signature_distance = 0.000000000, signature_jitter = 0,
fingerprint_triggers = 0, stable hold 30 s PASS 3/3. The 0.2518 cannot arise here.
Part 2 WAS a real gap and is fixed: update_band() returned early on a typed band without consulting
the signal (a flat 2% CEILING), and the learned band's jitter_ is the max over the pre-anchor
learning windows, then frozen -- the same quiet-selection bias the rate's long-window floor fixes.
FlipEwBound (EW mean + 2sd of the adjacent-window distance, time constant = signature_learning_
windows, i.e. the live pool) now floors BOTH branches, learning from IN-BAND windows only (judge
first, fold after: an excursion still cannot widen the threshold judging it). Free when the signal
is still: band stayed exactly 0.02 with noise_bound 0.000 over 15 samples.
VERIFIED on a373f53010cc7220 (= 3bf51b5b7): stable hold 3/3 PASS; costgate 28/28; hold 183/183 x2;
red-22 stationary 0 flips hold-optimum 544k steady (OFF 520-539k); red-31 ttfm 14 s -> 2:2 521k,
0 after stab, refine=hold-optimum; red8-71 ttfm 17 s -> 4:4 696k, 0 after stab. fp_trig=0 in all.
CAVEAT for t-flipfp's 1-in-100 sampling: the quantum is 1/sqrt(commands the signature was estimated
from). If the writer samples, that count must be the SAMPLED count or the quantum understates the
estimator noise tenfold; the new floor covers it empirically either way, but the positive phase's
margin (a real mix change scores ~0.35) should be re-checked against the widened band.

## 2026-09-07 05:30 pre-rebase work for t-flipfp (sampled fingerprint) + a wrong-split FAIL fixed
MERGE ORDER (coordinator): P0 shard-ownership -> t-flipfp -> me, rebased. My band's quantum depends
on their N, so I answered the margin question BEFORE the rebase with scratch/sampled_band_probe.cc
(replays a stationary stream at a chosen window size through the real FlipShiftDetector).
THREE band defects found and fixed (9768fcbef, 147033e55), each measured, mix change scores ~0.67:
 1. 3bf51b5b7 learned the ADJACENT-window distance while the trigger tests the ANCHOR distance
    (smoothed+correlated => systematically smaller): bound 0.054 vs quiet maxdist 0.101, 15 fires/60.
    Now learns the anchored distance's own null distribution.
 2. That statistic is AUTOCORRELATED so exceedances CLUSTER: mean+2sd still gave 3 two-consecutive
    exceedances in 600 stationary windows. FlipEwBound also reports a decaying MAX (retain largest,
    decay toward the mean) -- the same convention jitter_ uses.
 3. THE BINDING ONE: the typed branch never took the 1/sqrt(N) QUANTUM. A typed band cannot buy
    resolution the signal has not got; at 1-in-100 a 100-command window resolves the mix to ~0.1.
    Both branches now take max(2*quantum, noise bound).
 REGRESSION caught by tests/flip_multikey_hold.py: folding EVERY window let the first window of a
 real mix change lift the band to the excursion's own size -> positive phase reached only 0.80x and
 FAILED. Restored to IN-BAND-ONLY folding (the quantum makes it safe: the estimate can no longer get
 stuck too small). Unit row pins it: a mix change must clear its band, not move it, and keep clearing.
 RESULT (all rows): 0 fires, 0 two-consecutive exceedances in 600 stationary windows at every-command
 / 1-in-100 / w=1000, typed and learned; mix change clears by 3.3x at 1-in-100, 33x at every command.
 ANSWER TO THE REBASE QUESTION: margin is NOT thin; needs neither a wider signature sample rate nor a
 hand-adjusted quantum -- 1/sqrt(N) IS the sampling correction PROVIDED N is the SAMPLED count. That
 is the one thing t-flipfp must get right; a pre-sampling count understates the noise tenfold.
WRONG-SPLIT FAIL (47aaec6f7): a 3:1 boot anchored at 3:1 (232k vs 520k), hold-below-bar. Not the
model, not the cost gate (headroom_ex 0.0004 / io 0.699, demand interval 0.395-0.424, gain +100%,
cost_pays=1): the VERIFICATION BAR. A neighbour lane swung the box 2.8x/s, rate_ew_sigma 0.244, the
pair jitter pushed the band past 1.0, and min(1.0, band*margin) caps the required gain EXACTLY on the
gain a 3:1->2:2 move projects. A bar larger than any available gain is a verdict about the
MEASUREMENT, not the placement => decided-but-refused now keeps sampling to the reading cap
("sampling-bar"); the cap gives the honest too-noisy hold. "Live split is the optimum" still decides
at once; flip_choose_split unchanged.
VERIFIED 109b62e448b416cc: wrongsplit.sh x5 with the box at 2.3x spread -> 5/5 moved, ttfm 13/18/13/
14/18 s, all landed 2:2, 1 flip, 0 moves after stabilization.

## 2026-09-07 07:20 REBASED onto the fingerprint diet -- branch t-flipdamp-r, merge f13c3f91c
Integration branch t-merge14 = ca07de2a6 (t-flipfp merged). A 54-commit rebase conflicted at commit
4 and would have fought itself (later commits rewrite the same regions), so: branch t-flipdamp-r off
t-merge14 + `git merge --no-ff cd204ed28`. History preserved on branch t-flipdamp and tag
pre-rebase-flipdamp. flipctl.cc (this lane's bulk) is UNTOUCHED by t-flipfp -> the only conflicted
file was tests/flipctl_unit.cc, 2 hunks, both pure appends; kept both, ordered so each comment stays
with the code it introduces.
ONE REAL COLLISION: their row asserted band = 2*4/K (0.08 at K=100, 0.8 at K=10) -- mainline's OLD
1/K quantum, which THIS lane replaced with 1/sqrt(K) during the guard work (1/K understates estimator
noise by sqrt(K), twentyfold at 13.5k cmds/window). Scale-free half is theirs and stands; the floor's
law is mine. Row now asserts 2/sqrt(K) + that a thinner sample widens the floor, with the reason in
the test. NOTE for their sampled-vs-dispatched check: under 1/sqrt(K) the two counts differ by
sqrt(W)=10, not W=100, so it still discriminates; their measured adjacent jitter (0.0028 at depth 32)
sits ~4x under my floor, so the cluster-sampling design effect does not eat it.
TYPED-BAND-UNDER-SAMPLER row added (driven through THEIR writer at K=60 and 100): 0 two-consecutive
exceedances of a typed --flip-auto-band 2 on a stationary 50/50 stream, band never below 2/sqrt(K),
mix change still clears. Their tree (typed path bypasses the floor) measured 73/600 at K=100, 208/600
at K=60. Shipped config cannot reach the path (controller uses the learned band) but both knobs can.
ACCEPTANCE on the combined tree, binary 42796540060c4545:
  stable hold x3 PASS (band 0.228-0.248 -- the sampler's smaller N raises the quantum as designed --
    distance 0.000000000, fp_trig 0, and the gate row's mix-change phase still fires)
  costgate 28/28 | hold 183/183 x2 | unit ok (theirs + mine)
  stationary 2:2 120 s: 0 flips, hold-optimum, 536k steady, 0 moves after stab
  wrong-split 3:1 120 s: ttfm 21 s -> 2:2, 509k, moved-delivered, refine=hold-optimum, 0 after stab
  wrong-split 7:1 120 s: ttfm 20 s -> 4:4, 697k, moved-delivered, 0 after stab
  MUST-MOVE x5 (box at 2.3x spread): 5/5 moved, ttfm 18/17/21/15/18 s, all 2:2, 1 flip, 0 after stab
  sampled_band_probe: 0 fires / 0 two-consecutive in 600 windows at every-command, 1-in-100 and
    w=1000, typed and learned; mix change clears 3.3x at N=100, 33.6x at every command.
HANDOFF: branch t-flipdamp-r, hash f13c3f91c, binary 42796540060c4545. Coordinator runs the owner-box
acceptance (both wrong-split boots vs shipped + guard, 120 s traced).

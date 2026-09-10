**Release gate additions, 2026-09-09 — baseline `67f623716` in `cx-gates`.**

Both tiers are wired into `tests/gate.sh`. This diff intentionally cannot produce a clean release
gate on this tree: eight required combinations are rejected by the server, and there is no valid,
complete performance pin. These are counted failures. EXPECT constants are unchanged. No full
gate was run, and no server implementation, layout, or concurrency contract was changed.

All development servers used CPUs 8–15 (one CCX), ports 8620/8621, 16 shards and split ratio 6:2.
Load ran on CPUs 96–127. The build used `taskset -c 16-23 make -j8`. Process ownership is by
`Popen` PID, with an INFO `process_id` check before sending traffic; cleanup signals only those
PIDs. Logs, arguments, data directories and measurements are inside this worktree's `build/gates/`.
No pattern kill, other worktree edit, full gate, or automatic reference replacement was used.

**Feature tier: 35 rows in quick and full.**

`tests/feature_gate.py` covers the complete 32-entry product of thread-mode, read-local, overlap,
reorder and flip-auto. Every boot explicitly sets all eight switches. The other three booleans
vary as `atomic = read-local XOR overlap`, `key-lb = reorder`, `client-lb = overlap`, covering
both values and all four independent LB settings. An inventory assertion checks this plan.
The gate shell emits a separate ledger row for every entry, so deleting an entry also breaks
the ledger count.

Three additional rows exercise topology grammar and boundaries. Numeric/map domains have
arbitrarily many combinations; these cover their grammar, boundaries and effective application,
alongside every value of the eight finite switches:

| Row | Explicit settings and required observations |
| --- | --- |
| `split-home-min` | `--place` in reversed CPU order, `--shards 1`, complete `--shard-home 0:7`; several executor fillers have no shards. Verify live owners, CPU map and installed kernel affinities. |
| `fused-home-max-nopin` | `--place`, `--shards 256`, all 256 explicit homes, `--no-pin`; verify the complete map and that every OS task retains the enclosing affinity mask. |
| `split-shards-auto` | `--shards -1 --ratio 6:2`; require 16 resolved shards and six actual IO/two actual executor threads. Matrix rows explicitly use `--shards 16`. |

The traffic uses 64 connections with pipelined BITCOUNT/INCR work and checked GET/MGET replies.
INCR replies must preserve each connection's order. The common bitmap creates real cost and
weight differences; single-connection reorder traffic would not prove a permutation. Cross-owner
MSET/MGET keys are discovered from `DEBUG SHARDS`, rather than assumed from key names.

Assertions require:

- Effective INFO switches equal every requested switch; topology comes from live ownership and
  placement, and pinning is additionally checked through Linux task affinities.
- Every active reader accumulates both GET and MGET hits. Nonreaders remain at zero; a role
  change permits the lifetime counts accumulated while it was a reader. Disabled read-local
  has no thread-report allocation and zero hit, arm, sidecar and write-ring counters.
- The correct overlap schedule, increasing passes **and interleaved passes**, and increasing
  reorder calls, multi-client runs **and permutations**. Disabled mechanisms have zero counters;
  both disabled means the schedule-stat allocation/report must be absent.
- Atomic groups fire on cross-owner work when enabled and stay zero when disabled. LB ticks
  and the independently enabled key/client signal collectors must produce measured weights;
  disabled collectors stay zero and cannot move their respective objects.
- An enabled split flip controller consumes a directed trigger. A disabled controller cannot
  trigger. These smoke rows test activation; the existing flip batteries test completed moves.
- A clean final shutdown report after a successful battery. Each unsuccessful arming attempt
  uses fresh connections and fresh keys, bounded to three attempts, without skipping or relaxing
  the witness requirement.

All 24 supported matrix entries and all three topology rows passed. The initial complete run
took **31.34 seconds**, excluding the additional Python startup per gate row; budget about
**35 seconds** on the gate geometry. Each valid smoke runs for about 1.1 seconds. A missing
witness can cost three attempts; a hung request/boot remains bounded and fails.
The final repeat, including kernel-affinity and separate LB-collector assertions, took 31.32 seconds
and produced the same 27 passes/eight explicit boot failures.

The eight `1s-*-*-*-1` entries fail immediately with
`--flip-auto is unavailable with --thread-mode 1s`. This is explicit in
`src/core/config.h:1047`; `src/core/server.h` also initializes the controller only in split mode.
Removing the parser rejection alone would merely turn this into an effective-state failure.
Making all 32 boots pass requires a maintainer decision about the fused/flip contract and its
implementation. No rejection is marked expected, skipped, or passed by these rows.

**Performance tier: 32 mandatory full-only rows.**

`tests/perf_gate.py` runs GET, SET, four-key MGET and four-pair MSET at p1/p32 in 1s/2s with
read-local off/on. Atomics are always on. Overlap, reorder, flip and both balancers are explicitly
off, keeping the geometry stable across the measurement. Commands/s counts complete commands,
not MGET elements or MSET pairs.

Each trial boots a fresh release binary, populates **200,000 keys of 64 bytes**, and requires
`DBSIZE == keymax` before and across the window, with zero keyspace misses. `__key__` already
includes memtier's `--key-prefix`; adding another prefix is wrong and the population assertion
caught that mistake during development. GET/MGET probes separately prove that the armed lane
works before a SET/MSET window. Write windows correctly have zero timed read-local hits.

Load generation starts at one process and increases through a maximum of four. Physical cores
and their SMT siblings stay in the same process's affinity set, with one worker per allowed
logical CPU. Every worker has two clients at p1 and eight at p32, so the total workers,
connections and keyspace remain fixed as the number of processes changes. On the recorded
128-CPU load mask, the two- and three-process partitions are exactly:

```text
2: 64-95,192-223 / 96-127,224-255
3: 64-84,192-212 / 85-105,213-233 / 106-127,234-255
```

Every p32 arm gets its own escalation, including whichever is fastest. At least two consecutive
rungs must have **every executing thread >=98% busy**, and their rate difference must fit the
null-derived envelope. Exhausting the ladder fails with `could not saturate / establish generator
plateau`; it cannot accept an unqualified rate. Three fresh trials at the selected rung must
individually meet saturation, and their median is compared with the pin. Instance count, CPU
groups, per-thread busy/idle deltas, minimum busy fraction and rates are retained.
The qualifying ladder rungs and scored trials also require every generator OS thread below
90% CPU, so a CPU-limited generator cannot hide behind misleadingly busy server accounting.

The busy fraction is `delta(busy_ns) / (delta(busy_ns) + delta(idle_ns))`, using the server's
`DEBUG LBSIGNALS` accounting in the same window as the command counters. All fused threads
execute. In split mode, ordinary reads/writes execute on the executors; clean armed GET/MGET
execute on the IO reader threads, so idle bypassed executors cannot invalidate that arm.
Missing/frozen counters and a thread doing no work fail. INFO/DBSIZE/LBSIGNALS are pipelined;
the measured collection duration bounds timestamp skew, which must be below 0.2% of the window
and is added to the null envelope. This precision guard is separate from the regression bound.

**P1 results are latency/round-trip measurements, not throughput capacity.** They deliberately
have no busy-fraction assertion. Their validity criterion is fixed closed-loop concurrency and
generator geometry, every connection established throughout the window, successful nonempty
traffic, populated hits, correct lane activity, stable telemetry and **each generator OS thread
below 90% CPU**. The result records `connections / completed_commands_per_second` as the mean
round-trip interval including client dispatch, plus memtier's observed mean latency. The same
fixed-concurrency rate comparison therefore bounds a round-trip slowdown. Read-local accounting
can report nearly 100% busy even at p1; this does not change its classification or validity rule.

In the 32-cell targeted smoke, **30 cells met the validity criteria**. MGET/p32/1s/off exceeded
the 0.2% telemetry-skew ceiling (0.221%); MGET/p32/2s/off reported **96.025%** minimum executor
busy and failed saturation despite four generators. Smoke rates are diagnostic, not references:
these runs do not provide the complete null calibration/plateau proof needed to arm the tier.
The raw per-cell outcomes are summarized in `tests/gate_perf_evidence.json`.
A fresh six-second-window retry qualified MGET/p32/1s/off at 100% minimum busy. Split MGET still
failed at **96.459%**. The precision failure can be addressed by a longer measurement; the
saturation failure cannot be addressed by relaxing its floor.

Measured median trial cost was **6.50 seconds** with a one-second warmup, three-second window
and two seconds of generator tail, plus population/startup/cleanup. A successful checked cell
uses 2–4 ladder trials and 3 scored trials: estimate **17–24 minutes for all 32 armed rows**,
excluding box-sharing waits, failures and teardown timeouts. That is a projection from measured
trial costs, not a claim that a full armed gate ran. All 32 real CLI invocations against the
unarmed file took **1.42 seconds**; they each exited 3 loudly. The shell adds ledger overhead
and emits 32 loud skips **and 32 counted failures**.

**Derived bounds and why the supplied reference file is unarmed.**

For each cell, calibration collects 12 fresh trials at a fixed generator count, yielding six
adjacent A/A pairs of the same binary. It also checks **all 66 pairwise differences**, so drift
that moves both halves of an adjacent pair together cannot disappear. With rate `r_i` and
measured timestamp uncertainty `u_i`, the envelope is:

```text
L = max over i < j (abs(log(r_j / r_i)) + u_i + u_j)
permitted rate loss = 1 - exp(-L)
pass iff median(three current rates) >= pinned median * exp(-L)
```

No multiplier is guessed and no outlier is dropped. This is an empirical envelope over the
observations, not a statistical confidence guarantee. A required tolerance above the maintainer's
**2% box law** invalidates calibration; it is never clipped, widened or silently accepted.
The p1 generator-headroom and p32 saturation preconditions still apply independently of this bound.

The final-method experiments on the same binary gave:

| Cell | 12-trial median, commands/s | Min–max trial rates | Derived loss envelope | Verdict |
| --- | ---: | ---: | ---: | --- |
| GET p1 2s read-local off | 775,592 | 771,138–778,204 | **1.0015%** | Null spread admissible; RTT measurement (~82.52 us at 64 connections). |
| GET p32 2s read-local off | 7,167,859 | 7,027,378–7,292,590 | **3.7369%** | Invalid calibration: exceeds 2%. |
| GET p32 1s read-local on | 16,589,333 | 16,280,159–17,127,765 | **5.0802%** | Invalid calibration: exceeds 2%. |

All twelve windows in each of those p32 null experiments met the saturation floor. Saturation
alone did not make the run-to-run comparison clean. An earlier adjacent-pairs-only calculation
would have accepted the split GET null at 1.6963%; the final all-pairs rule correctly rejects its
drift. A dedicated negative control protects this distinction. The earlier 20,000-key development
experiment also failed (4.109% even with the weaker adjacent-only calculation) and is not a pin.

The authorized load mask omitted SMT siblings. A question about extending it was left pending;
no unapproved CPUs were used. The candidate builder refuses a calibration without complete SMT
pairs. Together with the unsaturated MGET cell and the invalid null envelopes, this prevents
claiming a complete valid 32-cell reference. `tests/gate_perf_refs.json` therefore explicitly
contains `armed: false` and measured diagnostics, with **no approved rate cells**. Absence of the
file or an unarmed file returns exit 3 with `performance tier UNARMED`; the gate records failures.
It never substitutes current measurements for missing reference numbers.

**Reference format and explicit maintainer re-pinning.**

The JSON schema is version 1. An armed file has `armed: true`, `provenance` and exactly the 32
cell IDs in `CELLS`. Each cell contains `rate` (the median), `instances`, `bound`, twelve or more
`samples`, and the generator `ladder`. `bound` stores the formula, log bound, loss fraction,
adjacent differences and worst pair indices. Samples retain busy/idle witnesses, DBSIZE,
read-local hits, generator CPU utilization, window uncertainty and the binary SHA-256.
Provenance records the commit, binary and harness digests, time, kernel, CPU model, memtier
digest, exact CPU masks, SMT completeness, geometry and all measurement parameters.

The reader rejects missing cells, hand-widened bounds, mismatched median rates, mixed binaries,
unqualified saturation/plateau evidence, and a different kernel/generator/measurement geometry.
Changing only the server binary is the intended comparison. Re-pinning remains manual:

```bash
# Maintainer, on a quiet box; these use the full recorded load mask, not the development grant.
taskset -c 64-127,192-255 python3 tests/perf_gate.py measure \
  --binary ./build/tomokv --server-cpus 0-7 --load-cpus 64-127,192-255 \
  --ratio 6:2 --port 8621 --output build/gate-pin-YYYYMMDD

# Fails unless all 32 cells qualify. Creates a NEW file and never overwrites a pin.
taskset -c 96-127 python3 tests/perf_gate.py candidate \
  --measurements build/gate-pin-YYYYMMDD --output build/gate-pin-YYYYMMDD.candidate.json

# Review raw logs, null pairs, counters and the candidate; then the maintainer installs it.
cp build/gate-pin-YYYYMMDD.candidate.json tests/gate_perf_refs.json
```

Use a new output path on every experiment. Do not solve excessive spread by widening a bound.
If a longer window or different capacity allocation is needed, remeasure the complete pin with
that geometry. Gate overrides are `GATE_PERF_CORES`, `GATE_PERF_LOAD_CORES`, `GATE_PERF_PORT`,
`GATE_PERF_REFS`, `GATE_PERF_WINDOW`, `GATE_PERF_WARMUP`, `GATE_PERF_KEYMAX` and
`GATE_PERF_INSTANCES`; the geometry must exactly match provenance. The default split ratio
remains the gate's `GATE_RATIO`. `GATE_FEATURE_PORT`, `GATE_LOAD_CORES` and the two output-directory
overrides control the feature load and artifact locations. Missing tools or timeouts fail.

**Anti-vacuity checks and how to induce each failure.**

Run `taskset -c 96-127 python3 tests/gates_test.py` for the server-less negative controls.
The tests poison valid witnesses, freeze/miss accounting, introduce cross-window drift, model
an idle or still-gaining generator ladder, remove reference cells, and exercise only the new
shell loops with stubs. The stub test proves an unarmed tier creates 32 failures, and does not
invoke `tests/gate.sh` or run the full gate.

| Rows protected | Directed failure |
| --- | --- |
| All 32 mode rows | In a throwaway binary, ignore any requested switch: effective INFO mismatch must fail. Leave INFO reporting it enabled but bypass the local-read loop, overlap call, reorder call/permutation, atomic admission, LB collector or flip trigger handling: its required activity witness must fail. For reorder, preserving only the call count while returning an identity result still fails permutations. |
| Disabled-feature arms | Force the feature or its optional allocation on when its knob is zero: nonzero/allocated counter checks fail. Mutate a stable executor's reader hits: the nonreader assertion fails. |
| Three topology rows | Ignore `--place`, `--shards`, `--shard-home`, or `--no-pin`: compare against actual roles, owners and kernel affinity; do not merely change the INFO echo. |
| All 32 performance rows | Remove population, introduce a prefix mismatch, disable the lane while echoing it enabled, remove/freeze INFO/LBSIGNALS, return command errors, drop a connection, or prevent generator completion: validity fails before scoring. |
| All 16 p32 rows | Rate-limit load or add executor idle time so any executing thread is under 98%; or make every added generator increase rate above the null envelope. The ladder exhausts with a named failure. Real split MGET already demonstrates the former. |
| All 16 p1 rows | Restrict generator resources until a thread reaches 90% CPU, lose a connection, or change the pinned concurrency/geometry. These fail p1 validity; low server busy must NOT fail p1. |
| Regression comparison | In a throwaway target, add work until the qualified rate falls outside the fixed pin envelope. The unit control also supplies a 2% rate loss against a measured ~0.1% null envelope and requires failure. |
| Pin/ledger | Rename/remove the reference file, leave it unarmed, remove one cell, alter a derived bound or remove a shell-loop iteration. Missing/unarmed refs fail loudly; malformed pins fail; missing iterations fail the ledger. |

**EXPECT arithmetic, counted by emitted lines.**

The baseline constants in this worktree are **377 quick / 394 full**, not the older 344 context
baseline. The two `ok "feature ..."` sites are above the literal quick-tier exit: the five nested
loops emit `2^5 = 32` rows and the topology loop emits 3. The performance `case` emits one of
ok/fail for each of 32 cells below that exit, including the unarmed branch. The helper unit
tests are development validation and add no ledger row. The existing optional NIC row is unchanged.

```text
EXPECT_QUICK must become 377 + 32 + 3      = 412
EXPECT_FULL  must become 394 + 32 + 3 + 32 = 461  (plus the existing optional NIC row)
```

The constants themselves were deliberately not edited. Their present mismatch will fail the
PROGRAM-STATE ledger until the maintainer updates them. A shell-loop test checks the actual
emission counts and their placement relative to the quick exit; see the final line numbers below.

In this diff, `tests/gate.sh:1386` emits the 32 matrix rows and `:1395` emits the three topology
rows, both before the quick exit at `:1399`. The performance success/failure sites at `:1426`,
`:1429` and `:1430` are mutually exclusive for each cell and all after that exit: 32 full-only rows.

# Configuration-surface reduction

> Incoming knobs-lane record. The line references and validation history below describe that
> lane before integration; [MERGE.md](MERGE.md) records the merged conflict dispositions and
> verified gate-line arithmetic. The merged tree has not been built or gated.

Baseline: `78c3e5391`, 2026-09-07. This implements the maintainer's prescribed dispositions.
No server, benchmark, or gate was run by Codex. This is an uncommitted implementation for review
and maintainer-run validation, with no throughput or latency improvement claimed. The maintainer's
first gate result and the atomic-window test repair are recorded below.

## One-to-one knob accounting

Defaults below describe the baseline unless the after column states otherwise. A deleted
name is rejected by both the CLI and the shared conf-file parser, is absent from the Config
fields and CONFIG table, and has no corresponding INFO configuration value. Operational
counters such as atomic-window stalls and kTLS engagement remain observations of the retained
mechanisms. No aliases preserve a deleted public control.

| Before | Before default | After | Reason / retained behavior |
| --- | --- | --- | --- |
| `--read-local-prefetch-capture` | `1` | Deleted; capture selected statically | Preserve the winning immutable-object capture at prefetch. |
| `--read-local-atomic-filter` | `1` | Deleted; precise filter whenever read-local is armed | Preserve the winning per-key safety filter, including fail-closed poison and overflow handling. |
| `--read-local-interleave` | `1` | Deleted; bounded interleave whenever read-local is armed | Preserve bounded local chunks and per-producer owner quanta. |
| `--flip-auto-band` | `-1` | Deleted; controller initialized with automatic band | Preserve the existing learned jitter, resolution, and baseline floors. |
| `--shard-home` | Unset | Deleted; round-robin over resolved executors | Preserve default ownership; remove manual map parsing. Runtime migration remains available. |
| `--l3-domains` | Discovery | Deleted; discovery plus `TOMOKV_L3_DOMAINS` environment override | Keep hardware discovery and an escape hatch for broken L3 sysfs. Override grammar remains comma-separated domains, `-` ranges, `+` range joins; invalid/duplicate/out-of-mask CPUs fail. |
| `--smt-mode` | `0` | Deleted; derive from complete sibling pairs in the allowed topology | Use existing sibling-unit placement and FLIP invariants when pairs are present. |
| `--genthread-schedule` | No independent default; compatibility alias | Deleted | Remove the alias that assigned fused mode and an overlap value. See objection about the original “zero uses” premise. |
| `--atomic-window` | `-1` | Deleted; `min(16 * resolved_shards, 1024)` | Preserve the measured automatic credit bound. No CONFIG override or live window setter remains. Live `atomic` still reconfigures credits safely. |
| `--persist-io` | `uring` | Deleted; derive engine from retained `net-io` | Preserve native uring persistence and the existing syscall dependency under epoll, which owns no uring ring. |
| `--lru-clock-shift` | `8` | Deleted; `kLruClockShift = 8` | Preserve 256-second buckets and the 8192-second wrap. Executor clocks and OBJECT IDLETIME share this constant. |
| `--script-crossshard-max-bytes` | `-1` | Deleted; boot-derived staging budget | Preserve `max(4 MiB, min(boot_maxmemory / shards / 16, 64 MiB))`, or 4 MiB when boot maxmemory is zero. |
| `--script-crossshard-workbench-bytes` | `-1` | Deleted; twice the staging budget | Preserve the original automatic workbench bound, still allocated on demand. |
| `--script-crossshard-conflict-retries` | `-1` | Deleted; fixed `8` | Preserve the original automatic conflict-retry limit for scripts. |
| `--script-crossshard-cut-slots` | `-1` | Deleted; fixed `4` | Preserve the original per-IO script snapshot reservation bound. |
| `--tls-ktls` | `yes` | Deleted; always attempt kTLS when TLS is enabled | Preserve automatic userspace fallback. `tls-port 0` still creates no TLS context or connection state. |
| `--key-lb` | `1` | Collapsed into `--lb` | One switch controls key and client balancing together. |
| `--client-lb` | `1` | Collapsed into `--lb` | Same switch; no independently disabled half remains. |
| `--lb-sample-rate` | `64` | Collapsed; rate derived from measured visits and decision duration | Target a constant number of samples per decision; details below. |
| `--lb-age-sample-rate` | `0` | Collapsed; derived sampling only during a flip maneuver | Keep idle/anchor sampling dark; derive the armed rate from traffic and the controller's decision window. |
| `--lb-tick-ms` | `1000` | Collapsed; internal 1000 ms observation tick | Retain the observation cadence; derive sampling over three sustained ticks. |
| `--lb-imbalance-pct` | `25` | Collapsed; twice measured quiet jitter | Separate key/client learners; excursions cannot enlarge their own admission band. |
| `--lb-move-cap` | `1` | Collapsed; derive from measured transfer duration | Bootstrap one move, then pace against measured per-move drain/install cost. |
| `--lb-cooldown-ms` | `5000` | Collapsed; derive from measured transfer duration | Remove the old all-nonzero machinery gate: zero cooldown can no longer mean zero moves. |
| No `--lb` | — | **`--lb 0\|1`, default `1`** | The sole public key/client balancing switch; `0` allocates no LB sidecar, bucket counters/census arrays, or controller windows. |
| `--thread-mode` | `2s` | Kept, unchanged; `2s\|1s` and `split\|fused` aliases | Both architectures remain. |
| `--ratio` | Unset; even split | Kept, split only | Global IO/EX counts, spread over locality domains; retain conf/CLI precedence with `place`. |
| `--place` | Derived | Kept, derived by default | Explicit `role@cpu` selection remains; fused mode uses labels only as CPU selectors. |
| `--shards` | Constant `16` | Kept; default `-1` resolves to `min(8 * executor_count, 256)` | Eight migration units per initial executor; preserves 16 shards at the gate's 6:2 split. Explicit `1..256` remains authoritative. |
| `--read-local` | `0` | Kept, `0\|1` | Effective only in fused plain scheduling; inactive modes retain ordinary owner dispatch. |
| `--atomic` | `0` | Kept, `0\|1`, live | Preserve enable/disable and group-scoped epoch-MVCC behavior. |
| `--flip-auto` | `0` | Kept, `0\|1`, split only | Preserve explicit controller enablement and dark fingerprint/stamp writers when disabled. |
| `--ex-sched` | `0` | **`--x-ex-sched 0\|1`**, default `0` | Study namespace only; omitted from public help, tomokv.conf, and user documentation. |
| `--overlap` | `0` | **`--x-overlap 0\|1\|2`**, default `0` | Study namespace only; same schedules and mode/engine validation. |
| `--zc-min` | `16384` | Held unchanged | Pending maintainer benchmarks. |
| `--net-io` | `uring` | Held unchanged, `uring\|epoll` | Preserve both network engines and their existing persistence dependency. |
| `--hash` | `mix64` | Held unchanged, `mix64\|siphash` | Pending maintainer benchmarks. |

The additional `--thread-pipeline` alias of overlap is removed with the old public spelling;
it cannot bypass the study namespace. Study CONFIG names are `x-ex-sched` and `x-overlap`;
INFO uses `x_overlap`, with the obsolete `overlap`/`thread_pipeline` aliases removed.

Other TomoKV controls not assigned a change by the design stay unchanged:
`--flip-work-window 100`, `--script-instruction-limit 100000`, `--no-pin` / conf `pin yes`,
and persistence input `--load`. Redis-compatible parser arms, names, defaults, and runtime
semantics are untouched. Documentation-only edits clarify the automatically selected engine.

## Derivation and lazy work

`LbAutotune` lives beside the existing weighted placement policy in
[`src/core/weighted_lb.h`](src/core/weighted_lb.h). Only `lb=1` allocates it. Its observation
tick is 1000 ms and a sustained decision covers three ticks, preserving existing hysteresis.
The sampling target is 4096 observations per decision. For measured visits `V` over elapsed
time `E`, the one-in-N rate is `max(1, ceil(V * decision_ms / E / 4096))`, capped at uint32.
Low traffic samples every visit. Before the first completed window, sampling bootstraps at 1.

Each successful key sample contributes its **latched rate** to the physical shard counter.
The fold therefore consumes estimated visits directly instead of multiplying historical counts
by a newer rate. Each executor refreshes its private rate on the existing census beat. Counters
and their census state remain on the physical shard; immutable bucket IDs retain controller
history across migration. No ownership or retirement handoff was moved out of its critical section.

Age sampling uses the same duration/traffic rule over three flip-controller ticks. It is armed
at maneuver start, updated on completed traffic windows, and set to zero at anchor/disable.
Its traffic estimate is completed commands; fanout can generate multiple stamped tasks per
command. It adds no eager computation while sampling is off. This controller remains independent
of the continuous key/client LB enable switch.

Key and client imbalance bands learn adjacent-window jitter before admission, using the
existing 0.25 EWMA weighting. After learning, only changes inside twice the learned jitter
update the estimate. The fire band is twice jitter; the existing 80% Schmitt release and
three-tick sustain remain. A zero measured band is a legitimate observation, never an off switch;
a zero-imbalance window clears the streak even when the learned release band is also zero.

Transfer timing brackets the existing drain-through-commit transaction using its already
published deadline. Completed duration divided by moved items gives mean cost `C` in ns.
The shard move cap is `min(shards, max(1, floor(tick_ns / C / 3)))`; before a measurement it
is one. Cooldown is `max(1, ceil(3 * C / tick_ns)) * tick_ms`, with one tick before a cost
sample. Client transfers remain one candidate per existing connection-drain transaction.
Only completed transfers contribute cost; refused or abandoned plans do not. Timing and policy
updates are cold, and the existing quiesced ownership transfer/rebinding functions are unchanged.

The read-local selectors are replaced by their winning branches inside the existing armed
paths. Immutable capture, filter publication, retirement, and QSBR retain their existing
arming checks. Scoped hash callbacks are still invoked only after read-local admission, and
scatter hash enumeration remains behind the local LB-enabled check. TLS setup still begins
only after the listener's `tls-port` check. No removed flag is replaced by an eager argument
whose callee merely returns early.

## Boot geometry, INFO, and layout

`Server::prepare_boot` resolves topology, sibling units, placement, and the shard default
before AOF/snapshot recovery reads a shard count. `Server::init` consumes that same prepared
placement, with no second discovery. Fused executor count is the number of selected fused
threads; split executor count is the resolved EX role count. A later FLIP changes roles without
changing the boot-latched shard count.

`INFO server` adds `shards` from the real shard inventory and `atomic` from the live enable
bit. The existing `read_local` already reports the effective lane state and remains intact.
These fields are read-only, computed on INFO, with no operation-path writes. The thread counts
already reported by each mode remain. No new CONFIG shard entry is added, so CONFIG REWRITE
does not turn an automatic default into an explicit fixed count. The old separate key/client
enable observations become `tomokv_lb_enabled`; movement counters remain available. The
deleted SMT flag's INFO value is removed; the derived FLIP unit size remains observable.

| Layout lock | Before | After |
| --- | ---: | ---: |
| Config | 624 | **528** |
| Op | 336 | 336 |
| Client | 1984 | 1984 |
| ThreadCtx | 1408 | 1408 |
| Shard | 1440 | 1440 |
| FlatStore | 944 | 944 |
| Rob<64> | 192 | 192 |
| AtomicEntry | 144 | 144 |

The Config assertion explicitly becomes **528**, with the same accounting comment beside it:
104 declared field bytes removed, 4 bytes added for `lb`, and 4 bytes of additional alignment
padding: `624 - 104 + 4 + 4 = 528`. No surviving Config fields were reordered. This changes
their offsets and Server's embedded Config footprint, intentionally and visibly. Other locks
remain unchanged. The former FlatStore filter byte and the two ReadLocalExImpl selector bytes
are reserved padding to preserve the measured reader/owner separation and demotion-state offsets.

## Atomic-window gate repair: verdict (a), test witness

The reported failure is **(a)**: the test never separated release of its artificial hold from
its resume assertion. It is not evidence of a newly broken derived window. This verdict was
stated before editing the repair; no server code is changed by this follow-up.

The evidence, at the gate's actual geometry:

- `tests/gate.sh:29` selects cores `0-7`; line 31 resolves `6:2`; line 370 supplies `--shards 16`
  and line 390 supplies that ratio. Release and ASAN use this boot helper at lines 433 and 1249.
  The bound is **256**, not a bound inferred from a default boot on all machine cores.
- Baseline `78c3e5391:src/core/server.h:196` resolves AUTO with
  `min(16 * cfg.shards, 1024)`. Current `src/core/server.h:250` retains exactly that formula.
  Admission borrowing/stalling (`:2497`), retirement and idle lease return (`:2541`), and
  backpressure release (`src/core/io_loop.h:6698`, also `:5684`/`:6381`) are unchanged by the
  configuration reduction. The deleted setter was only a wrapper around credit reconfiguration.
- In the **reported failing version** of `tests/atomicwindow.py`, lines 13–15 submit
  `2 * ceil(256 / 64) + 2 = 10` connections, each with 64 groups: **640** total. Line 55 arms
  `ATOMIC-COMMIT-DELAY 100000`; lines 59–67 wait for every reply and raise “did not resume” at
  30 seconds; line 70 clears the hook only in the ensuing `finally`. No released completion
  interval was tested, and a timeout bypassed the nominal three-attempt discovery loop.
- `src/core/server.h:2615` applies the delay to **each commit batch**, and `:3036` spins for the
  full captured duration. Batching cannot justify the old deadline: default slow-log threshold
  is 10,000 us (`src/core/config.h:356`); escalation (`src/core/ex_loop.h:2658`) selects per-op
  timing, whose `:2620` flushes after every operation. At 100 ms per individual commit, 640 groups
  can consume 64 executor-seconds, or 32 seconds even if shared evenly by two executors. That is
  an allowed schedule exceeding 30 seconds, **not a measurement of the failing run's rate**.
- The saved release and ASAN logs both have the timeout at line 21, successful subsequent
  `CONFIG SET atomic` traffic at line 22, and `inflight=0 pending=0` after churn at line 23
  (`/tmp/gate-atomic-torn.txt`, `/tmp/gate-atomic-torn-asan.txt`). Both following RYOW logs report
  `limit=256 peak=24 stalls=0 credits=256`. This supports the release/accounting trace above;
  those logs contain no peak, stall count, or reply count for the failed global burst, so they
  cannot establish whether that particular attempt filled the window or its per-attempt rate.

The repair in `tests/atomicwindow.py` pre-encodes each burst and starts senders at one barrier.
It uses a five-second polling budget to find a held witness, clears the DEBUG delay in `finally`,
and only after the clear replies OK starts the unchanged 30-second resume deadline. At least two
groups must still be live when the window is witnessed, and some replies must remain outstanding
when release is requested. The global arm additionally requires an increase in window stalls;
a witness seen only during drain cannot rescue a miss. All replies must be OK. Cleanup wakes
blocked readers before closing buffered files, joins helpers with a shared bound, and requires
zero live groups/debt and the full credit pool before another attempt.

Only a clean, joined, fully reclaimed **witness miss** can re-arm, on new writer/control
connections and newly resolved disjoint cross-owner keys. Four misses still fail. Reply errors,
excess live groups, debt, missing credits, or failure to resume **after release** fail immediately;
none can be retried into a pass. This matches the policy in `tests/multirace.py:464` and the OFF
RENAME control at `tests/atomic_torn.py:744`, without importing either control's measured hit rate.
Each attempt prints its outcome, held/total stalls, peak, carried groups, exact reply counts and
elapsed arm/resume times with measured replies/second. The result also prints observed attempts
over attempted bursts. **The new runtime arming rate remains unmeasured:** the maintainer must
collect these lines separately on release and ASAN at 16 shards, 6:2, cores 0–7. Four is an explicit
discovery budget, not an asserted residual failure probability; no percentage is guessed here.

**Row-collapse decision: reverted.** `tests/atomic_torn.py:874` separately reports “derived atomic
window stalls and resumes”; `:882` reports “atomic reconfiguration preserves derived bound and
reclaims leases”, using a second fresh burst. Resizing coverage is obsolete with the removed
grammar, but live reconfiguration is not: `CONFIG SET atomic 1` unconditionally reaches
`set_atomic_enabled` (`src/cmd/t_server.cc:1440`), which rebuilds the credit generation even when
already ON (`src/core/server.h:2462`, `:3343`). Keeping ON preserves the same bound for all traffic.
The second row requires surviving **pre-CONFIG** admissions: it subtracts every possible newer
admission, sampled after the post-CONFIG live count, and demands a positive remainder. This cannot
pass solely on fresh post-CONFIG groups. Prepared groups abandoned for task-queue backpressure
(`src/core/io_loop.h:5077`) also count as admissions; their inclusion makes this witness stricter,
so total admissions are deliberately not equated to total replies. Both rows retain exact credit
reclamation checks; the second specifically exercises old-generation lease return (`server.h:3324`).

Repair validation was server-free: Python AST parsing, `git diff --check`, and 12 in-memory
transport fault-injection cases covering release-before-completion, the one-connection arm,
live reconfiguration, fresh retry, four-miss failure, and immediate failure on no resume, bad
reply, excess live groups, and lost credits. A control containing only newer post-CONFIG groups
also exhausted discovery rather than passing. Stalls first seen during drain cannot rescue an
unarmed attempt, and extra admissions from abandoned preparations need not equal reply counts.
These are harness checks, not measurements of
TomoKV. No build, server, benchmark, or gate was run for the repair. For maintainer-only broken
server controls: disabling admission-backpressure release must fail the resume deadline; dropping
old-generation credit returns must fail pool reclamation; suppressing the stall witness must
exhaust discovery. None of those server mutations was applied or run here.

## Gate accounting and review checks

**Required expected counts: quick 325, full 342** (343 with the optional NIC row).
`EXPECT_QUICK=327` and `EXPECT_FULL=344` at lines 141–142 of `tests/gate.sh` are deliberately
unchanged for the maintainer to edit. Counted by emitting line, not battery name or file location:

| Row change | Baseline emitting line | Position relative to quick exit | Quick delta | Full delta |
| --- | --- | --- | ---: | ---: |
| Retire `xscript off control`; keep limit/window with automatic bounds | 658, inside the former three-arm loop | Before old exit 1260; surviving two-arm row now emits at 649, before exit 1245 | -1 | -1 |
| Retire manual-map dispatch scaling guard and its shell wrapper | 686 | Before old exit 1260; retirement note now at 673, before exit 1245 | -1 | -1 |
| Replace persistence CONFIG surface check with removed-knob/actual-geometry checks | 981 | Four iterations before the exit; replacement emits at 966 | 0 | 0 |
| Restore separate atomic liveness and reconfiguration checks inside `atomic_torn.py` | Internal base lines 894 and 957 | Outer release row still emits at 435, before quick exit 1245; ASAN row still emits at 1255, after it | 0 | 0 |
| All other battery rows | Existing lines, retaining multiplicity | Same side of the quick exit | 0 | 0 |

Restoring the internal check adds one separately reported assertion per battery invocation, not
another `gate.sh` ledger entry. Expected counts therefore **remain quick 325 / full 342**, or 343
with the optional NIC row; the known PROGRAM-STATE mismatch is unchanged. No outer battery row
was added. New helper files are consumed by existing rows. Parser coverage
rejects every removed spelling even with its formerly valid default, checks the study grammar,
and checks sample-budget invariance, jitter excursion rejection, and transfer-cost pacing.
Compile-time assertions cover the shard-default scaling and cap. `tests/knobs.py` checks the
CONFIG removals and compares INFO geometry with DEBUG's actual thread/shard inventory on the
four existing persistence/atomic boots. Normal persistence coverage now boots `net-io epoll`;
uring coverage still boots `net-io uring`.

Atomic admission tests use fresh disjoint cross-owner keys and held groups at the production
credit limit. The global arm must observe stalls and multiple live groups, maintain the bound,
and reclaim all credits. The one-connection arm must observe simultaneous groups on that one
connection. Each has four bounded fresh attempts and fails if its witness never appears, with
the DEBUG hold cleared before the resume deadline. The independent reconfiguration row uses
live `atomic` with old-generation groups still present; resizing itself is retired with the knob.
Live ON/OFF toggle coverage also remains. The separate rate comparison uses an unheld burst.

The script staging control exceeds the default 4 MiB budget and still requires refusal before
RUN with untouched values. The cut-slot arm uses more than four connections per serving thread,
retains its serial negative control, and re-arms fresh connections up to three times before
failing. LRU rows age all old cohorts across one real 256-second bucket before probing ordinary
reads, NO-TOUCH, and eviction survival; no clock override remains. The four rows and their
positive lane-hit witnesses remain. Their individual clock wait is bounded at 260 seconds.

The TLS fallback battery uses the retained cipher grammar (TLS 1.2 CBC; TLS 1.3 AES-256 outside
the current custom AES-128 RX installer) and retains its live fallback/zero-copy-suppression
assertions. Default kTLS engagement retains its separate positive witness. Idle LB signal tests
now require zero age samples; the maneuver test must observe positive age sampling. The stable
flip hold uses the reported derived band and fails after bounded unsuccessful re-arms.

Initial reduction validation: serial C++20 syntax checks with warnings enabled for the parser,
main, fused loop, scatter, server/INFO, LB reporting, flip controller, TLS, persistence, snapshot,
and OBJECT implementations; Python AST parsing; shell syntax checks; diff/removed-reference
and layout-assertion inspection. No executable was linked or run by Codex. The maintainer then
reported the failure documented above. The repaired runtime witnesses and their broken-server
controls, and performance, remain unverified; the repair's server-free checks are listed above.

| Matched geometry/load comparison | PRE `78c3e5391` | POST working diff |
| --- | --- | --- |
| GET/SET rates, cycles/op, instructions/op, IPC in both modes | Not measured in this worktree | Not measured |
| Enabled LB under stationary/skewed/changing load | Existing fixed controller | New derivations; benchmark pending |
| `lb=0` / flip sampling dark | Source baseline | Arming/allocation paths inspected; benchmark pending |
| Full gate | 344/344 supplied by maintainer | Maintainer's pre-repair run: 340 ok, 3 FAIL (including the expected ledger mismatch); repair not run, expected count still 342 |

## OBJECTIONS

1. **`--read-local-prefetch-capture` / `--read-local-atomic-filter`: inherited reader retry.**
   The winning captured MGET path already violates the supplied “no reader retries” law:
   `prepare_captured_local_mget` in `src/core/ex_loop.h` sets `kAttempts=2` at line 1295,
   loops at 1317, and increments `mget_generation_retries` before repeating at 1431–1432.
   The same loop exists in the baseline, and this diff still selects that winning path.
   It was not redesigned or silently removed. The default GET capture arm breaks on table
   churn; the confirmed normal-path objection is MGET. Resolving the inherited law conflict
   requires a separate correctness decision, not a covert change in this surface reduction.

2. **`--genthread-schedule`: “zero uses” was not literal.** Baseline
   `src/core/config.h:702–716` actively lowered `coarse/iofused/streams` into fused mode and
   overlap 0/1/2, with parser tests for the aliases. There was no independent Config field.
   The alias is deleted as prescribed; former callers now fail explicitly.

3. **`--lb` and derived `--shards`: measurement still required.** The specified derivation
   scheme is implemented, but the 4096-sample target, eight shards per executor, initial
   all-visit sampling, and transfer pacing have no PRE/POST rate evidence in this task.
   Age sampling estimates command volume, so fanout affects actual stamped-task counts.
   A pointer transfer's timed drain/install duration also does not measure subsequent cache
   warmup or commands lost. These limitations matter to a paper and to the instruction/IPC
   verdict; the formulae are implementation choices within the specified derivations, not
   claims of measured optima. No runtime tuning knobs were retained to defer this decision.

4. **`--smt-mode` / `--l3-domains`: stricter boot geometry.** Automatic pair mode invokes
   the existing complete-pair and same-role validation. Odd logical ratios, incomplete explicit
   pairs, or a split restricted to one available pair can now be rejected. L3 override repairs
   L3 discovery; sibling sysfs still must be readable to derive and validate pair mode.
   These consequences are recorded rather than adding an unrequested SMT override.

5. **`--shard-home`: one regression instrument is lost.** The retired scaling wrapper kept
   all 16 shards on two real executors and added empty filler executors using the manual map.
   Default round-robin changes that ownership with thread count. Keeping its former measured
   ratio threshold on a different geometry would assert an unvalidated claim, so its one row
   is explicitly retired rather than reporting a misleading pass.

6. **`--lru-clock-shift`: gate duration increases.** Keeping the winning 256-second clock
   and preserving a real positive age witness costs up to 260 seconds on each of four boots.
   No debug clock selector or artificial tolerance was introduced. The maintainer should budget
   for those waits when running either tier.

All prescribed knob dispositions are implemented. No item was left undone for a newly
introduced compile failure or correctness-law violation; the inherited MGET conflict above
is explicitly retained for maintainer review.

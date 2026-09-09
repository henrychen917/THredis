# Gate repair, 2026-09-08

The torn/window witness and multirace arming have been repaired. **The reported borrow-registry
failure remains unresolved:** the original failing assertion is unavailable, and the direct
reproductions so far pass without changing that battery or the registry. This is not a claim
that the release gate is green. `tests/gate.sh` was not run or edited.

## Scope and evidence

The input revision is `0d8044b95`, whose pre-stack parent is `c8e61f646`. Release and ASAN builds,
reference builds, throwaway negative controls, and logs are under `build/gatefix/` in this
worktree. `CODEX-OUT.md` is an externally maintained transcript and was left alone.

The matching geometry is `taskset -c 72-79`, `--shards 16 --ratio 6:2`, localhost ports 8080-8081,
with clients on `taskset -c 68-71`. These eight server cores share one L3 domain; boot reports
six IO threads and executors 6 and 7. Initial scouting on cores 68-75 crossed two L3 domains
and is distinguished from the comparisons below. Every boot used an empty persistence directory.
Only individually launched batteries ran, sequentially, without concurrent compilation or a
standalone benchmark. The harness terminated and reaped only the PIDs it started.

The borrow row is an explicit geometry exception in the actual gate: line 683 overrides the
launcher's 16 shards with `--shards 1`. Its registry is per shard, so reproductions retain that
override, `--zc-min 64`, and `--client-output-buffer-limit 'normal 0 0 0'`, with the same 6:2 split.

## Multirace: test arming, with the production protection intact

Neither proposed enabled path explains the failing gate boot: overlap defaults to 0, reorder
defaults to 0, and the local-read lane is fused-only. The gate's debug-surface boot enables
none of them. The dispatch-time undecided-unit park and install-time falsifier are unchanged
from `c8e61f646`.

The old test's `owner_spread()` grouped and sorted **physical shard IDs**, then claimed each
key had its own owner. Seven distinct owners are impossible on this two-executor boot.
Its four retries retained that key set, so new connections did not repair an unsuitable owner
geometry. It also accepted a commit-control hold as the witness for an abort case that never
entered the withdrawn-candidate window.

The data does not support attributing a loss of interleavings to this stack:

| Binary, original battery, ten fresh matching boots | Old combined guard | Total holds/run | Missing abort witness despite PASS |
| --- | --- | --- | --- |
| Pre-stack `c8e61f646` | 10/10 pass | 2-1,466 | One run: abort 0, commit 2 |
| Input `0d8044b95` | 10/10 pass | 4-1,463 | One run: abort 0 on all four attempts; commit 4 only on attempt four |

Thus the requested outcome is **repair the arming, not weaken or remove the protection**.
The window remains reachable, but the old small, incorrectly selected workload can make the
abort interleaving inaccessible throughout its retries. I did not reproduce the reported
sustained reduction in the combined hit rate, and do not assign it to overlap or reorder.

`tests/multirace.py` now resolves live owners with `DEBUG SHARDS` and `DEBUG LBSIGNALS`, puts the
victims on an owner other than the blocker, and precedes the blocker with 256 absent keys on
its physical shard. Those keys give the veto owner real validation work while the victim owner
installs and dispatches EXEC. No sleep or production ordering change is involved. Fresh keys,
ownership, and connections are selected on each clean miss. Both abort and commit must record
holds; the attempt limit stays four. All three liveness movers still cross owners.

| Corrected battery, input behavior plus gate fixes | Abort holds | Commit holds | Verdict |
| --- | ---: | ---: | --- |
| overlap 0, reorder 0 | 44,273 | 53,292 | PASS, first attempt |
| overlap 0, reorder 1 | 44,397 | 53,669 | PASS, first attempt |
| overlap 1, reorder 0 | 45,374 | 55,398 | PASS, first attempt |
| overlap 1, reorder 1 | 44,462 | 54,911 | PASS, first attempt |

Every cell also runs atomic 0: holds are zero, replies are exact, and the install-time violation
counter is zero. Three additional default boots passed with 95,781-98,002 total holds.

The throwaway `build/gatefix/nopark` build adds `false &&` to just the dispatch-time
`atomic_has_foreign_unit_undecided` guard in `ExLoopT::execute`. The corrected test **fails**:
200/200 aborted MSETNX rounds leak the withdrawn value to both connections, holds are zero,
and `atomic_exec_order_late` reaches 800. The committed and foreign-connection controls retain
their expected values. This proves the test exercises the original data corruption, not just
an instrumentation counter. No negative-control source change is in the deliverable diff.

Classification: **pre-existing test defect**, present in the pre-stack parent. No evidence of
weakened production transaction protection in the stacked changes.

## Torn/window, release and ASAN: blocking the observer's executor

`tests/atomicwindow.py` used `DEBUG ATOMIC-COMMIT-DELAY 100000`. That hook sleeps the last
**executor** between reserving tickets and publishing a commit batch. CONFIG SET is itself
executor work. Consequently the operation supposed to reconfigure credits while old groups
remain live queues behind the delayed commits. It can reply after all old groups drain, or
consume the control socket's 30-second timeout. Retrying with the same blocking mechanism does
not make that a reliable reconfiguration witness. A timed-out buffered socket also prevents
the old finally block from disarming the delay and contaminates subsequent drain checks.

Fresh original full batteries passed twice in release and once in ASAN, so those runs alone do
not reproduce every reported failing row. They did reproduce the bad observation interval:
one reconfiguration took 17.807 seconds before release, versus 0.099 seconds to resume afterward.
Other attempts drained hundreds of replies before CONFIG observation and reported clean misses.
Six additional original ASAN helper runs also passed after varying numbers of attempts. Replaying
the ASAN tier's preceding torture and RYOW batteries before the original torn battery passed too.
The reported unmodified-row failure therefore remains unreproduced in these trials; the repaired
witness addresses the demonstrated observer/commit scheduling dependency, without claiming a
reproduced data tear or a proven attribution for missing original assertion lines.
An experiment placing the control on a separate IO made all three reconfiguration runs miss
all four attempts: IO isolation does not free the executor. That experiment was discarded.

The fix adds the DEBUG-only `ATOMIC-COMMIT-HOLD 0|1` latch. It retains the existing owner-private
commit queue **before ticket reservation** and lets the owner continue processing CONFIG and
foreign work. The idle sweep keeps polling retained commits until release. The latch defaults
off, allocates no sidecar, and is read only when a commit batch is pending. It changes no
single-key read/write path or ownership transfer protocol. The original reserve/publish delay
remains intact for the torn-read and predecessor-resolution tests that actually need it.

The window helper now uses the latch and requires zero replies while held. It still requires
window stalls, surviving old-generation groups after CONFIG, the production limit, exact
replies, zero debt, and full credit return after release. Deadlines and attempt count are unchanged.

| Corrected complete battery | Live old groups after CONFIG | Held replies | Final replies | Credit pool | Verdict |
| --- | ---: | ---: | ---: | ---: | --- |
| Release | 256 | 0 | 640/640 | 256 | PASS |
| ASAN | 256 | 0 | 640/640 | 256 | PASS |

Both reconfiguration witnesses armed on attempt one; resume took 0.011 and 0.047 seconds.
Both complete batteries also finished connection churn with inflight=0 and pending=0.
The throwaway `nohold` build accepts the DEBUG command but bypasses its executor guard: the
corrected window test fails all four attempts with peak=0 and no surviving groups. A non-working
arming mechanism therefore cannot turn this row green.

Classification: **pre-existing test/instrument mismatch**, introduced before `0d8044b95`;
the helper is byte-identical in its pre-stack parent. The original failing log is needed to
confirm that every reported torn/window failure was this assertion rather than another subcase.

## Borrow registry: no justified fix yet

The battery and the borrow lookup/index implementation are unchanged from `c8e61f646`.
Three unchanged-input reproductions at the one-CCX gate geometry passed with borrowed-GET
growth ratios 1.000, 1.002, 1.002; plain controls were 0.998, 1.004, 1.003. An initial two-L3
scout also passed at 1.002. These were scored release checks, not environment skips.
Each run proved thousands of parked borrows, different plain/borrow send paths, and zero
borrows after teardown.

The comparison was then repeated against the parent and the repaired build, using the same
battery and geometry. All nine one-CCX cells were scored, with no skipped growth verdict:

| Binary | Three borrowed-GET growth ratios | Plain control range | Verdict |
| --- | --- | --- | --- |
| Pre-stack `c8e61f646` | 1.001, 1.002, 1.001 | 0.999-1.003 | 3/3 PASS |
| Input `0d8044b95` | 1.000, 1.002, 1.002 | 0.998-1.004 | 3/3 PASS |
| Repaired build | 0.997, 1.000, 0.997 | 0.998-1.001 | 3/3 PASS |

The current `/tmp/gate-borrow.txt` is a passing run, not the reported failing log. I requested
the original failing assertion while continuing the investigation. Without it or a reproduction,
changing allocation growth, the 1.05 bound, or the environment-control logic would be unjustified.
**No registry or borrow-test change is included; this reported failure remains unresolved.**
Its origin cannot honestly be classified as a worktree defect or pre-existing defect yet.

## EXPECT arithmetic, counted by executable row location

Relative to `c8e61f646`, there are two added success/failure rows, each emitted once:

| Added row | Success line | Relative to quick exit at line 1264 | Quick delta | Full delta |
| --- | ---: | --- | ---: | ---: |
| reorder mechanism + 32/128-task geometry battery | 329 | Before | +1 | +1 |
| restored cross-shard dispatch scaling | 694 | Before | +1 | +1 |

The second row is restored dispatch scaling, not a second reorder row. Therefore the maintainer
must set **EXPECT_QUICK = 325 + 2 = 327** and **EXPECT_FULL = 342 + 2 = 344**, before the optional
NIC increment. A successful PROGRAM-STATE check does not increment PASS; a mismatch adds the
extra failure that turns 344 battery rows into the reported 340 ok + 5 FAIL summary.
This repair adds no gate rows. Both constants and the entire gate script remain untouched.

Classification: **expected ledger change from this worktree's additions**, for the maintainer.

## Other checks and limits

Release and address-sanitized builds pass. The reorder unit was run with ASAN+UBSAN and
`-fno-sanitize-recover=all`: PASS, 175 witnessed permutations, maximum batch 128. Python syntax
checks and whitespace checks pass for the changed source/test files.

Fused mode with the local-read lane armed and reorder on passes the complete torture battery,
multirace (both atomic settings), and the reconfiguration witness at both overlap 0 and overlap 1.
Both witnesses retain 256 old-generation groups, observe zero held replies, and drain all 640
replies. Final shutdown reports for these boots, the four split scheduling cells, and the
release/ASAN torn batteries have zero live connections, non-quiescent ROBs, and pending unsent bytes.
`atomic_ryow.py`, the other caller of the shared window helper, also passes in release and ASAN
with `--no-rate-assertions`; its correctness and overlap witnesses remain enabled.

The relevant individual battery invocations are:

```sh
# Against a separately managed 6:2, 16-shard server on cores 72-79:
taskset -c 68-71 python3 tests/atomic_torn.py 127.0.0.1 8080 --release-build
# ASAN: the same invocation without --release-build.
taskset -c 68-71 python3 tests/multirace.py 127.0.0.1 8080
taskset -c 68-71 python3 tests/atomic_ryow.py 127.0.0.1 8080 --no-rate-assertions
# On the borrow row's separate one-shard boot, with the flags documented above:
taskset -c 68-71 python3 tests/borrow_registry.py 127.0.0.1 8080 --release-build
```

The supplied layout background differs from the checked-in Config lock: `78c3e5391` locks 624,
`c8e61f646` locks 528, and the input worktree locks **544**. This repair leaves Config and the
other locked types unchanged; it does not silently restore padding or change their assertions.
That existing knob-wave accounting should be reconciled with the project background.

The full release gate and standalone performance benchmarks remain for the maintainer.

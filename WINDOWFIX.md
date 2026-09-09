# Atomic reconfiguration window

Worktree baseline: `c8e61f646` (`cx-windowfix`). The server sources and release binary are the same
for PRE and POST. The change is confined to `tests/atomicwindow.py`; `tests/atomic_torn.py` continues
to call the same helper for the same row. No gate rows or expected counts change.

## Confirmed mechanism

The original helper already submits more work than the derived limit: at 16 shards it sends
640 cross-owner MSET groups against 256 credits. Increasing that burst or the four-attempt budget
does not address the missing ordering guarantee.

`DEBUG ATOMIC-COMMIT-DELAY 100000` spins an executor for 100 ms in
`Server::atomic_commit_batch()`. It is a finite delay, not a latch. `CONFIG SET atomic 1` fans out
to every shard owner, so that same spin delays CONFIG. By the time CONFIG replies and the helper
samples admissions twice, the MSETs can have drained. New admissions between those samples can
also reduce the conservative `carried` estimate to zero. The retry changes connections and keys,
but repeats this race.

The measurements refine the proposed explanation: a `clean-miss` does **not** establish that the
hold never blocked anything. PRE reached `peak=256` and positive `held_stalls` even on failed
attempts. For example, PRE run 1, attempt 2 reported `held_stalls=21`, `held_replies=640/640`,
`carried=0`. Other misses retained some MSET replies but still had `carried=0`. All four misses in
that run produced `observed_attempts=0/4`. The lost condition is a witnessed old lease surviving
CONFIG, not necessarily initial admission pressure.

## Arrangement and assertions

The reconfiguration arm adds one transaction with disjoint, freshly probed keys on two owners:
`MULTI; MSET a a b b; MGET a b; EXEC`. The other 640 groups remain cross-owner MSETs. The sibling
admission and one-connection overlap arms retain their existing workloads and holds.

1. Before sending any burst work, arm the existing `ATOMIC-FANOUT-DEFER` hook for ten seconds and
   dispatch the transaction. Wait for exactly one admission and an increment of `cmdstat_exec`.
   `multi_dispatch_entry` increments that command count **after** copying the hook deadline and
   posting the fragments. Merely seeing `atomic_inflight=1` would precede that copy and introduce
   another race.
2. Clear the fan-out hook immediately after that acknowledgement. The transaction retains its
   copied deadline. Clearing is essential because CONFIG itself also qualifies for the generic
   read fan-out hook. Require the transaction socket to have no reply.
3. Arm the original 100 ms commit delay and send the burst. Require the original overlap and
   positive window-stall witnesses. Once witnessed, release the commit delay **before** CONFIG.
   Owners can process CONFIG while the identified transaction retains an old-generation lease.
4. Require CONFIG and its live sample to complete inside the original five-second arming budget,
   measured from before the transaction was sent. This is strictly before its earliest possible
   ten-second expiry. Require positive in-flight work and no transaction reply. `carried=1` now
   names that pre-admitted transaction rather than estimating its identity from unrelated MSET
   admission counters.
5. After all MSET replies, run a second same-value CONFIG while the transaction remains parked.
   This isolates the credit calculation: it must report exactly `inflight=1`, `credit_pool=255`,
   and `credit_debt=0`, still within the same five-second budget. This second rebuild is necessary:
   MSETs admitted after the first CONFIG may return credits to the still-active transaction IO's
   local cache, so the global pool need not equal 255 before that rebuild.
6. Keep observing the unchanged `inflight <= 256` and zero-debt assertions while the copied hold
   expires. Require all 641 group replies, including the exact transaction reply
   `[OK, [a, b]]`. Finally require `inflight=0`, `credit_debt=0`, and all 256 credits returned.

No attempts or assertion tolerances were increased. Failure to dispatch, retain the old lease,
enter the admission window, complete, preserve the bound, or reclaim credits remains a failure.
There is no new skip path. Both debug settings are cleared on exceptional exits too. The hook's
copied deadline expires naturally; clearing the global setting prevents new holds and does not
cancel an already parked fragment. A successful reconfiguration arm therefore takes about ten
seconds, independently of MSET drain speed.

## Measurement

Each isolated-row repetition uses a fresh server, a new randomized key hash seed, the real
`held_burst(..., whole_window=True, reconfigure=True)`, and the unchanged row verdict. The runner
sets atomic ON first and records actual owner geometry. Server and test client are both pinned
to cores 32–39; the supervisor is pinned to 40–43. Builds use 32–43. All launches use an unused
port in 8340–8359, and cleanup terminates only the PIDs recorded from that runner's own `Popen`
calls. No gate or benchmark was run.

```sh
taskset -c 32-43 make -j8
taskset -c 32-39 ./build/tomokv --bind 127.0.0.1 --port 8340 \
    --shards 16 --ratio 6:2 --atomic 1 --enable-debug-command yes
```

The supervisor and full raw logs are retained under `build/windowfix/` (ignored artifacts).
`run.py` boots and reaps each server; `pre-helper/atomicwindow.py` is the pristine helper saved
from `git show HEAD:tests/atomicwindow.py`.

| Arm | Passed | First-attempt witnesses | Evidence |
| --- | ---: | ---: | --- |
| Maintainer's pristine-mainline measurement supplied with the task | 8/10 (80%) | Not supplied | Task context |
| PRE, unchanged worktree helper | 11/15 (73.3%) | 5/15 | `build/windowfix/pre/` |
| POST, final helper | **15/15 (100%)** | **15/15** | `build/windowfix/post-isolated/` |

PRE recorded 37 attempts, including 26 clean misses. Its four failed rows exhausted all four
attempts. The supplied mainline result is reported separately from this worktree's matched
before/after sample; these are small finite samples, not an estimate of zero future failures.

Every POST run reported `carried=1`, `lease_pool=255`, `replies=641/641`, and final `credits=256`.
The first CONFIG arm took 0.101 s in the reported precision; the copied hold then accounted for
about 9.9 s of completion wait. Peak admitted groups were 249–256: an IO's cached unused credits
can exhaust the shared pool before all 256 credits are active. Positive held stalls ranged from
7 to 18. All 30 matched PRE/POST servers exited cleanly with status zero.

The full affected batteries also passed, each on a fresh boot with clean server exit:

| Battery | Split, 6:2 | Fused, eight cores | Logs |
| --- | --- | --- | --- |
| `atomic_torn.py --release-build` | PASS | PASS | `build/windowfix/torn-{2s,1s}/` |
| `atomic_ryow.py` | PASS | PASS | `build/windowfix/ryow-{2s,1s}/` |

These ran sequentially on ports 8341–8344. Both target-row battery executions witnessed the
reconfiguration on their first attempt. Existing unrelated OFF conditional-mover discovery skips
remain visible in the battery logs; the target row and all helper admission witnesses ran.

`build/windowfix/manifest.json` records the baseline commit and SHA-256 identities of the release
binary, pristine helper, and measured final helper. The matched server SHA-256 is
`2ce655411553f03c0ea6345a3af5a67b471c5769fe5b25c55e2fcd3a5614e4bd`.

## Genuine failure controls

Only throwaway source copies under `build/windowfix/negative-server/` are modified for these
controls. `prepare-negative.py` copies the complete source and Makefile, then inserts two
independent, environment-selected faults in `src/core/server.h`; the deliverable changes no
server behavior.

- **Lose the bound:** in `atomic_reconfigure_credits`, replace the rebuilt pool's
  `window - active` accounting with the full `window` while old groups are still live. The
  negative binary selects this with `WINDOWFIX_BREAK_BOUND=1`. The row must reject excess
  in-flight work, nonzero debt, or the isolated state `inflight=1, credit_pool=256`.
- **Lose returned old leases:** return immediately from `atomic_return_reconfigured_credit`,
  dropping a retiring old-generation group's credit. The negative binary selects this with
  `WINDOWFIX_BREAK_CARRY=1`. The row must fail when pending MSETs cannot resume, when the
  isolated accounting fails, or when the final pool cannot recover all 256 credits.
- **Never establish the old-lease hold:** the `no-hold/atomicwindow.py` copy sends zero instead
  of ten seconds when arming `ATOMIC-FANOUT-DEFER`. The server is otherwise unchanged. The row
  must fail admission/hold witnessing; a clean transaction reply cannot substitute for a parked
  old-generation lease.

The controls were actually run at the same 16-shard, 6:2, eight-core geometry:

| Control | Result | Observed rejection | Evidence |
| --- | --- | --- | --- |
| No fan-out hold, normal server | FAIL as required | `lease holder completed before burst` | `build/windowfix/negative-no-hold/` |
| Full pool despite active leases | FAIL as required | Isolated state stayed at `inflight=1, credit_pool=256` | `build/windowfix/negative-bound/` |
| Drop old-generation credit returns | FAIL as required | All 641 replies arrived, but final `inflight=0, credit_pool=255` | `build/windowfix/negative-carry/` |
| Same fault-capable binary, both fault variables absent | PASS | First attempt, `lease_pool=255`, final `credits=256` | `build/windowfix/negative-intact/` |

Each injected-fault test exited 1; the intact control exited 0. Every server exited 0. These were
sequential runs on ports 8345–8348.
The bound control demonstrates why checking the isolated pool matters even if sampled in-flight
counts happen to stay below 256. The return control demonstrates that completing all replies
does not substitute for reclaiming all leases.

To reproduce with the retained supervisor (use new labels because it refuses to overwrite logs):

```sh
taskset -c 40-43 python3 build/windowfix/run.py pre-again --count 15 \
    --helper "$PWD/build/windowfix/pre-helper"
taskset -c 40-43 python3 build/windowfix/run.py post-again --count 15
taskset -c 40-43 env WINDOWFIX_BREAK_BOUND=1 python3 build/windowfix/run.py bound-again \
    --count 1 --port 8346 --binary "$PWD/build/windowfix/negative-server/build/tomokv"
taskset -c 40-43 env WINDOWFIX_BREAK_CARRY=1 python3 build/windowfix/run.py carry-again \
    --count 1 --port 8347 --binary "$PWD/build/windowfix/negative-server/build/tomokv"
```

The supervisor records the child's verdict in `results.json`; its own completion status is not
the battery verdict. Do not mistake its successful cleanup for a passing negative-control row.

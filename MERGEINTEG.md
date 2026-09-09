# cx-integ merge record — 2026-09-09

Merge `cx-integ` (`a9a220a49`) into `cx-final` (`6647031dd`), from common ancestor
`c8e61f646`. The receiving parent already contains `cx-windowfix`, `cx-vestcut`, and
`cx-fix-concurrency`. All edits and generated artifacts are in this worktree.
The pre-existing, untracked `CODEX-OUT.md` is excluded from the merge.

The merged release build passes all sixteen mode/feature engagement checks, including
four split local-read FLIP round trips. Atomicwindow passes **15/15** fresh-server repetitions (all three helper arms per repetition).
The maintainer must set **EXPECT_QUICK=335 / EXPECT_FULL=352**, excluding the optional NIC
increment. The assignments remain **325 / 342**, as explicitly required.
No gate, benchmark, performance comparison, or competitor server was run.

## Seven textual conflicts

| File | Receiving intent | Incoming intent | Resolution and reason |
| --- | --- | --- | --- |
| `src/cmd/lbsignals.cc` | Collapse synonymous LB predicates after the single `lb` knob replaced independent controls. | Report independently configured key/client LB in two INFO fields. | Use the two independent predicates and match the two format arguments. These checks have distinct meanings again; retaining the single predicate would either fail to compile or silently misreport the restored surface. |
| `src/core/ex_loop.h` | Remove the never-serving executor WbEngine and losing reader implementations; preserve owner safe points, client lifetime scopes, and the scheduler overrun fix. | Introduce capacity-deducing reorder, fused overlap with local reads, and split reader/owner tenures. | Combine the live mechanisms explicitly; the individual decisions below cover each overlap. Keep the incoming scheduler implementation and preserve all concurrency guards. |
| `src/core/genthread_pipeline.h` | Remove streams-only context constants, microstage enum/array, residual policy, and unused IFID capacity. | Update commentary while retaining those declarations for historical source comparison. | Keep the deletion. No surviving boot path consumes that implementation. Retain the live 32-op baseline / 128-op overlap executor geometry and WB occupancy constants. |
| `src/core/io_loop.h` | Delete the unreachable streams loop, buffered IFID paths, and their lifetime/buffer machinery. | Select the final binary overlap grammar and admit local reads on split IO and fused overlap. | Keep the deletion; port the new reader capability to the reduced parser signature and existing schedules. See the individual decisions below. |
| `src/core/server.h` | Collapse LB guards, simplify fixed atomic admission and executor slot IDs, preserve ownership rebinding and client completion epochs. | Restore independent LB controls, optional schedule witnesses, the commit-queue hold, and split read-local state. | Restore only the now-distinct LB guards and branch conditions. Retain optional schedule allocation, reader state, and the hold; preserve the boot-derived window, physical executor IDs, ownership transfer transaction, IoDrain barrier, and completion epochs. |
| `tests/atomicwindow.py` | Arrange an identified old-generation EXEC lease with ATOMIC-FANOUT-DEFER, independently of MSET drain speed. | Replace the owner-blocking commit delay with the nonblocking ATOMIC-COMMIT-HOLD queue latch. | Start from the complete receiving helper and migrate all three commit-hook calls. Preserve its entire executable arming, verification, deadlines, retry, and cleanup logic. An AST comparison proves there are no other executable changes. |
| `tests/gate.sh` | Add eight deterministic concurrency regression selections. | Add the reorder unit and restore dispatch scaling; update knobs and isolated persistence boot directories. | Include both sets of rows and all incoming surviving boot/call-site changes. No EXPECT assignment changes. Arithmetic is based on the final emitting lines below. |

## Executor: individual deleted-versus-changed decisions

Line references below identify the stable function/type names; historical hunk line numbers
can be recovered from the two parent commits. Neither executor nor IO was resolved by
selecting one complete parent file.

| Hunk / mechanism | Both intents | Decision and reason |
| --- | --- | --- |
| Executor `WbEngine`, `engine()`, partial bind, and size assertion | Vestcut removes an engine with no serving calls; integ adds another executor-capable runtime. | Keep the deletion and 5856-byte ordinary executor assertion. RL2S still sends through IoLoop; its new shutdown caller is adapted to the IO-only reporting API. A new executor type is not a reason to restore a non-serving send engine. |
| `init`: LB sample/controller latches | Vestcut makes the key-only checks synonymous with one switch; integ restores separate key/client switches. | Restore the key-LB predicate for executor sampling and the shard-move controller latch. Client-only balancing does not collect shard samples. |
| `init`: overlap geometry, private queues, handoff ring | Receiving code keys these choices on template `Fused`; integ also uses that capability on shard-less split IO. | Require actual `ThreadMode::Fused` for overlap batch/private-lane selection, and use `iofused_` for handoff-ring sharing. Split readers retain ordinary owner inboxes even when overlap=1. |
| `fused_baseline_pass` | Vestcut inlines the permanently selected interleave predicate; integ inserts the split reader pass. | First dispatch actual split mode to `split_read_local_pass()`, then test `read_local_enabled()` directly. Do not resurrect `read_local_interleave_enabled()`. |
| `fused_baseline_sweep` | Same deletion versus new split dispatch. | Apply the same split-first/direct-predicate resolution independently to the idle sweep. An idle path must not start owner housekeeping on shard-less IO. |
| Fair-lane checks in pass/sweep | Vestcut removes the identical interleave wrapper; integ depends on the lane at both overlap settings. | Keep direct `read_local_enabled()` checks. Overlap's coarse pass drains all local captures before its WB/owner seams; baseline retains bounded read/owner interleaving. |
| `ReadLocalCaptureBuffer<Enabled, Capacity>` | Vestcut removes an always-true dimension; integ reuses capture state for split reads. | Keep only the capacity dimension. Both modes use the same captured implementation. |
| `prepare_local_mget` | Vestcut deletes the uncaptured losing implementation; integ's split feature uses the existing reader machinery. | Keep it deleted. `prepare_captured_local_mget` is the live MGET implementation in both modes. No new split caller needs the removed helper. |
| `prepare_local_read`, bounded drain, prefetch/copy selection | Vestcut removes `CapturePrefetch` branches and hint-only alternatives; integ extends where the lane runs. | Keep the captured-only implementation, point-batch capture, and mixed GET/MGET program order. Neither overlap nor split needs a runtime A/B latch. |
| Old inline `ex_sched_candidate` / `ex_schedule_run` / `ex_schedule_batch` block | Concurrency fixed 128-task overruns by scheduling bounded 32-task chunks; integ replaces this block with `reorder.h` and arrays sized for the real gather. | Use integ's complete capacity-deducing 32/128-task scheduler at both production call sites. It sizes scratch and multiword chain occupancy for the whole batch, preserving legal 128-task permutations. Do not restore the old block or combine two competing fixes. Adapt the concurrency test to call the production array API while retaining its 128-task and 64-task single-client cases. |
| `fused_streams_pass` comment/function | Integ changes the comment of a function whose only scheduler caller was deleted. | Remove this leftover entry as well. It has no boot or surviving caller; the live coarse pass and occupancy-gated overlap entry remain. |
| Owner `run()` and `owner_control_tail()` | Concurrency moves all owner work ahead of ExDrain acknowledgement; integ adds RL2S deferred reclamation and uses `ExLoopT<true>` for split owners. | Drain the deferred queue before acknowledgement, only while the frozen owner has not acknowledged. Preserve the control tail's ordering. Permit `flip_control_pass()` for actual split mode even with the fused-capable template; the receiving `if constexpr (!Fused)` alone would strand EX-to-IO preparation. The four RL2S round trips exercise this seam. |
| `flush_xshard_commits` | Concurrency protects Client references through completion notification; integ can deliberately retain the queue across passes. | Test the hold first; execute the real flush inside `ClientWorkScope`. Preserve the pending bit and sweep poll witness while held, so owners remain responsive and the latch can be released. |
| Owner-transfer and completion changes merged around these hunks | Concurrency guards stale snapshot work, null tagged tasks, and post-Done lifetime; integ changes scheduling and owner tenures. | Retain the receiving guards and scopes, including `owner_control_tail`, stale-owner forwarding, notify/config/sink adoption, and IO completion fences. Nothing in the new scheduler or split lane substitutes for them. |

## IO: individual deleted-versus-changed decisions

| Hunk / mechanism | Both intents | Decision and reason |
| --- | --- | --- |
| Initialization LB latches | Vestcut collapses client/controller wrappers; integ restores independent controls. | Restore client-only sampling and the either-switch shared controller predicate. Preserve all other IO initialization changes. |
| `run_loop` fused dispatch | Receiving code has overlap 1 and 2 dispatches; integ's public surface is 0 or 1, with the former gated overlap-2 schedule selected by 1. | Select the existing gated fused schedule only for `Fused && Pipeline == 1 && !SplitLocal`; restrict valid Pipeline values to 0/1. SplitLocal runs the split IO schedule and its coded-reply path. This is an IO/EX scheduling change, not a restored three-thread-stage mode. |
| Deleted 946-line `run_fused_streams_loop` | Vestcut proves it unreachable; integ edits surrounding overlap code and retains historical source commentary. | Keep the entire streams loop deleted. Both live schedules have their own dispatch and require none of its unpublished/residual contexts. |
| IFID entry/batch definitions, `active_ifid_context_`, prepared-frame/lifetime scans | Vestcut deletes state produced only by streams; integ's initial parser signature still names it. | Keep all those definitions and scans deleted. Preserve the live WB context and concurrency client-work fences. The new reader lane retains ROB handles and uses its existing tombstone/completion protocol. |
| `BufferedIfid` parameter and five parser staging arms | Vestcut removes the dead parameter, batch pointer, and staging/rollback branches; integ extends the original longer signature. | Keep the reduced signature with `Client*` only and append `SplitLocal`. Do not reintroduce `IfidPipelineBatch`, `BufferedIfid`, or a dummy argument. |
| Parser's compile-time reader capability | Receiving expression excludes the private overlap queue; integ explicitly includes private overlap and split readers. | Use `SplitLocal || IofusedPrivateQueue || baseline_fused_geometry`. This preserves ROB hazards, MGET fencing, admission, and demotion for both newly armed schedules. Booting with read-local=1 cannot simply compile those paths out. |
| New split pipeline parser call sites | Vestcut shifts the surviving template arguments by removing BufferedIfid; integ supplies SplitLocal at the end of the old argument list. | Remove the obsolete placeholder from all six newly added split parser calls. Preserve TargetedIfid, SuppressOrdinaryActiveMark, and IofusedPrivateQueue values in existing fused-overlap callers. |
| Receive-buffer `AppendOnly` / `CanHoldPrepared`, TLS prepared-frame growth | Vestcut deletes choices with only streams callers; integ extends the local-read protocol. | Keep deletion. Local reads use the existing ROB/copy/capture lifetime, not unpublished parser-frame retention. |
| `pipeline_simple_point`, buffered publication/classification branches | Vestcut deletes sole-purpose streams helpers; integ changes overlap admission. | Keep deletion. Live local-read demotion reservations remain, because those are used synchronously by the new armed paths too. |
| `genthread_ifid_batch` pipeline selector, buffered cap, active-set fallback | Vestcut reduces live use to targeted pending-list dispatch; integ still uses the same fixed producer lanes. | Keep the reduced helper and its bounded pending-queue visit budget. No buffered batch argument or old active-set alternative returns. |
| `genthread_wb_batch` pipeline dimension and streams-only sweep | Vestcut collapses the constant selector and deletes `genthread_pipeline_sweep`; integ uses occupancy reporting in the gated schedule. | Keep the reduced WB template, its independent occupancy reporting, and `genthread_iofused_sweep`. The deleted streams sweep has no new dependency. |
| Overlap loop reader activation, park/resume, targeted local completion | Integ adds actual lane publication and targeted completion wakeups; vestcut deletes different, streams-only lifetime state. | Retain all incoming reader publication, QSBR wait boundaries, and targeted completion changes. Captures finish before the rotation/wait boundary; these are live reader requirements, not vestiges. |

The remaining automatically merged vestcut reductions stay in place: immutable boot-derived
atomic window, direct physical executor IDs, cache-required retirement, simplified reclaim
callbacks, fixed controller band, and IO-only shutdown aggregation. The one vestcut rationale
that no longer applies is independent LB elimination: all bucket/client allocation, folding,
FLIP weights, and controller candidate/no-candidate guards now follow the restored switches.

## EXPECT arithmetic by executable emitting line

The common ancestor's quick exit is line **1246**. The receiving parent's exit is line **1263**.
The merged exit is line **1283**: `if [ "$TIER" = quick ]; then`.
Both assignments at lines **141–142** are byte-for-byte unchanged from both parents.

| Added or retired row | Merged emitting line | Relative to quick exit 1283 | Quick delta | Full delta |
| --- | ---: | --- | ---: | ---: |
| `core concurrency watch` | 334, loop selection at 328 | Before | +1 | +1 |
| `core concurrency scheduler` | 334, same loop | Before | +1 | +1 |
| `core concurrency lifetime` | 334, same loop | Before | +1 | +1 |
| `core concurrency drain` | 334, same loop | Before | +1 | +1 |
| `core concurrency route` | 334, same loop | Before | +1 | +1 |
| `core concurrency snapshot` | 334, same loop | Before | +1 | +1 |
| `core concurrency config` | 334, same loop | Before | +1 | +1 |
| `core concurrency notify` | 334, same loop | Before | +1 | +1 |
| Reorder mechanism + 32/128-task geometry | 346 | Before | +1 | +1 |
| Restored cross-shard dispatch scaling | 713 | Before | +1 | +1 |
| Retired rows in this integration | None | — | 0 | 0 |
| Added or retired full-only rows | None | After | 0 | 0 |

From the common ancestor: quick **325 + 8 + 1 + 1 = 335**;
full **342 + 8 + 1 + 1 = 352**, then the optional NIC increment.
Equivalently, the receiving parent's row inventory is 333/350 despite its untouched 325/342
constants; incoming contributes +2/+2. The incoming branch's own inventory is 327/344;
the receiving concurrency loop contributes +8/+8. Windowfix, vestcut, RL2S, orthog, and
changes inside existing batteries add no further gate rows. Updating knob spelling, boot
persistence directories, and snapshot reload arguments replaces existing behavior without
adding a verdict. Dispatch scaling is restored with its manual ownership control, not with
an inferred replacement geometry; it was not executed here because it measures performance.

## Build and serverless verification

The release server and all five ordinary unit binaries built with the repository Makefile,
GCC C++20, jemalloc, and the existing string-TU inlining parameter. The core concurrency
fixture uses its Makefile ASAN/UBSAN instrumentation. Reorder also built and ran separately
with `-fsanitize=address,undefined -fno-sanitize-recover=all`. No layout assertion changed:
Op 336, Client 1984, ThreadCtx 1408, Shard 1440, FlatStore 944, Rob<64> 192,
AtomicEntry 144, Config 624 all compile at their required sizes.

```sh
taskset -c 44-55 make -j8 all build/config-parser-test build/flipctl-unit \
  build/read-local-ring-unit build/read-local-write-ring-unit \
  build/reorder-unit build/core-concurrency-unit
```

All **14 serverless invocations pass**: the five ordinary units, the separately sanitized
reorder unit, and each of the eight concurrency selections. The scheduler regression still
checks its full 128-task gather and 64-task single-client overrun control, using the shared
production array API. See `build/mergeinteg/units.log`, `build-final.log`, and
`reorder-build.log`. Builds finished before server-connected verification began.
The first build exposed the RL2S shutdown signature mismatch; `build.log` retains that
failure. `build-final.log` is the successful final source build, with no warnings or errors.

## Sixteen-combination activation verification

`tests/orthog.py` ran unchanged on sixteen fresh servers: 16 shards, atomic=1,
server cores **44–51**, clients/supervisor **52–55**, ports **8360–8375**, io_uring,
jemalloc, DEBUG enabled, key-LB/client-LB/automatic-FLIP=0. Split mode uses **6 IO + 2 EX**;
fused uses all eight cores as unified threads. These assigned cores span two four-core
L3 portions. No throughput or timing comparison is inferred from this geometry.
Each boot has a fresh persistence directory and randomized hash seed.

```sh
taskset -c 52-55 python3 build/mergeinteg/verify.py matrix \
  --output build/mergeinteg/verified-matrix
```

The supervisor's exact per-cell command is saved in `commands.json`. In 2s it includes
`--ratio 6:2`; 1s omits ratio, as required by its parser. A first supervisor attempt wrongly
supplied ratio to 1s and was rejected before boot. That harness error remains in
`build/mergeinteg/matrix/1s000/`; no pass was claimed for it. The complete corrected matrix
is in `verified-matrix/` and used the final production binary.

Reader evidence below gives active threads and the per-thread increment in total local hits /
MGET hits. Split active threads additionally have **zero shards**, and both executor rows
have **zero local-read hits**. Off arms have no reader rows and zero local-read hits.
Overlap columns give the actual schedule and total/interleaved passes. Reorder columns give
batches / multi-client runs / actual permutations, followed by maximum batch size.
All enabled reorder witnesses were arranged on the **first** fresh arm. Disabled schedule
counters remain zero; with both schedule knobs off the optional witness array has zero threads.

| Mode / RL / overlap / reorder | Reader evidence | Actual overlap schedule; passes/interleaved | Reorder batches/multi/permutations; max | Verdict |
| --- | --- | --- | --- | --- |
| 1s/0/0/0 | off; no reader rows | plain; 0/0 | 0/0/0; max 0 | PASS |
| 1s/0/0/1 | off; no reader rows | plain; 0/0 | 341/189/183; max 32 | PASS |
| 1s/0/1/0 | off; no reader rows | fused-overlap; 4015/963 | 0/0/0; max 0 | PASS |
| 1s/0/1/1 | off; no reader rows | fused-overlap; 3760/859 | 284/153/147; max 128 | PASS |
| 1s/1/0/0 | 8; each +48/+16 | plain; 0/0 | 0/0/0; max 0 | PASS |
| 1s/1/0/1 | 8; each +48/+16 | plain; 0/0 | 345/213/207; max 32 | PASS |
| 1s/1/1/0 | 8; each +48/+16 | fused-overlap; 4275/994 | 0/0/0; max 0 | PASS |
| 1s/1/1/1 | 8; each +48/+16 | fused-overlap; 4140/918 | 288/153/147; max 128 | PASS |
| 2s/0/0/0 | off; no reader rows | plain; 0/0 | 0/0/0; max 0 | PASS |
| 2s/0/0/1 | off; no reader rows | plain; 0/0 | 450/207/200; max 32 | PASS |
| 2s/0/1/0 | off; no reader rows | split-io-overlap; 5126/5126 | 0/0/0; max 0 | PASS |
| 2s/0/1/1 | off; no reader rows | split-io-overlap; 5031/5031 | 417/207/199; max 32 | PASS |
| 2s/1/0/0 | 6; each +48/+16; EX 0 | plain; 0/0 | 0/0/0; max 0 | PASS |
| 2s/1/0/1 | 6; each +48/+16; EX 0 | plain; 0/0 | 402/199/196; max 32 | PASS |
| 2s/1/1/0 | 6; each +48/+16; EX 0 | split-io-overlap; 5553/5553 | 0/0/0; max 0 | PASS |
| 2s/1/1/1 | 6; each +48/+16; EX 0 | split-io-overlap; 5341/5341 | 421/205/198; max 32 | PASS |

All **16/16** servers exited zero with exactly one schema-1 shutdown report and every
`stuck` field zero. `evidence.json` retains CONFIG and before/local/scheduler/after INFO;
`info.json` retains final server/stats sections; raw test/server logs, PID ownership,
commands, return codes, and parsed shutdown reports live beside them.

On all four `2s/read-local=1` cells, the existing `tests/rl2s.py` additionally passed:
actual owner routing, every IO reader, GET/MGET, exact pipelined RYOW replies, pure SET staying
on owners, EX-to-IO FLIP, IO-to-EX FLIP, and reader reactivation after each conversion.
Those four logs are `verified-matrix/2s1*/rl2s.log`. They specifically cover the control-tail
interaction introduced by using the fused-capable executor in split mode.

## Atomicwindow repetition verification

**15/15 fresh-server repetitions pass; 45/45 helper arms open on the first attempt.**
Each repetition runs the real `held_burst` overlap, whole-window, and reconfiguration arms,
in that order. Servers use 2s, 16 shards, ratio 6:2, atomic=1, RL/overlap/reorder=0,
key/client LB=1, automatic FLIP=0, DEBUG enabled, a fresh data directory/hash seed,
server cores 44–51, clients 52–55, and port 8376. These are helper battery runs,
not invocations of the full `atomic_torn.py` or `atomic_ryow.py` batteries.

```sh
taskset -c 52-55 python3 build/mergeinteg/verify.py windows \
  --output build/mergeinteg/windows
taskset -c 52-55 python3 build/mergeinteg/verify.py windows --first 11 --last 15 \
  --output build/mergeinteg/windows-completed
```

The first series completed runs 01–10. Before starting run 11, the supervisor's plain
bind preflight returned EADDRINUSE on the recently closed port; there was no remaining
listener. It was changed to use SO_REUSEADDR, still without SO_REUSEPORT, so live listeners
remain rejected while closed-socket reuse is permitted. Runs 11–15 then completed in the
second directory. No battery failure was retried or discarded, and neither source nor
helper executable behavior changed between the two series. The empty first run-11
directory and original supervisor traceback remain available.

| Fresh boot | Helper arms | Reconfigure peak inflight | Held window stalls | Old lease / isolated pool / replies / final credits | Server exit |
| --- | --- | ---: | ---: | --- | --- |
| run-01 | 3/3, first attempt | 256 | 11 | 1 / 255 / 641 / 256 | 0; clean |
| run-02 | 3/3, first attempt | 256 | 8 | 1 / 255 / 641 / 256 | 0; clean |
| run-03 | 3/3, first attempt | 256 | 11 | 1 / 255 / 641 / 256 | 0; clean |
| run-04 | 3/3, first attempt | 249 | 11 | 1 / 255 / 641 / 256 | 0; clean |
| run-05 | 3/3, first attempt | 256 | 13 | 1 / 255 / 641 / 256 | 0; clean |
| run-06 | 3/3, first attempt | 253 | 8 | 1 / 255 / 641 / 256 | 0; clean |
| run-07 | 3/3, first attempt | 256 | 7 | 1 / 255 / 641 / 256 | 0; clean |
| run-08 | 3/3, first attempt | 249 | 8 | 1 / 255 / 641 / 256 | 0; clean |
| run-09 | 3/3, first attempt | 256 | 7 | 1 / 255 / 641 / 256 | 0; clean |
| run-10 | 3/3, first attempt | 256 | 10 | 1 / 255 / 641 / 256 | 0; clean |
| run-11 | 3/3, first attempt | 249 | 7 | 1 / 255 / 641 / 256 | 0; clean |
| run-12 | 3/3, first attempt | 256 | 11 | 1 / 255 / 641 / 256 | 0; clean |
| run-13 | 3/3, first attempt | 256 | 11 | 1 / 255 / 641 / 256 | 0; clean |
| run-14 | 3/3, first attempt | 249 | 7 | 1 / 255 / 641 / 256 | 0; clean |
| run-15 | 3/3, first attempt | 256 | 7 | 1 / 255 / 641 / 256 | 0; clean |

Every repetition reports `carried=1`, `lease_pool=255`, all **641/641** reconfiguration
replies, and **256** final credits. The smaller peak in some rows is allowed by the original
witness: cached unused IO credits can exhaust the shared pool before every credit is active;
the required positive stall witness and no-debt/bound checks still fire. All fifteen servers
exit zero with every shutdown `stuck` field zero. Raw logs/commands/shutdown JSON are under
`build/mergeinteg/windows/run-01..10/` and `windows-completed/run-11..15/`.


The helper is the receiving windowfix, with **only three executable substitutions**:
arm `ATOMIC-COMMIT-DELAY 100000` becomes `ATOMIC-COMMIT-HOLD 1`, and both release sites
become `ATOMIC-COMMIT-HOLD 0`. Comments describe the new queue latch. An AST comparison of
the complete helper against `6647031dd`, after exactly those substitutions, is equal.
This protects every arming branch, assertion, deadline, retry rule, and cleanup path.

In particular: the cross-owner EXEC is queued before the burst; the test waits for EXEC's
command count to advance after dispatch has copied the ten-second fan-out deadline; it then
disarms new fan-out holds before CONFIG. The identified EXEC must remain incomplete across
CONFIG, own the one surviving old lease, leave the isolated pool at 255, and return the exact
transaction result before all 256 credits are restored. A faster MSET drain cannot erase
this witness. Four clean misses still fail, and broken replies, timeouts, or live helpers
cannot become a successful retry.

Integ's `held_replies == 0` change is intentionally not transplanted over the windowfix's
post-CONFIG sample: the windowfix releases MSET commits before CONFIG while EXEC remains
parked. Requiring zero MSET replies at that later point would reject an arranged, valid window.
The preserved predicate remains `held_replies < groups` plus the identified carried lease;
there is no relaxed deadline, skip, or tolerance widening.

## Law review and limits

The incoming architecture concern remains explicit: the live
`prepare_captured_local_mget()` still has `kAttempts=2` and increments
`mget_generation_retries` (`src/core/ex_loop.h`, around lines 1174–1312).
`FlatStore::read_local_probe_sequence_equal` still performs the point-probe sequence
comparison (`src/store/flatstore.h`, around line 3337); local MGET also validates sequence
or per-key epochs across capture/copy. These inherited armed-reader paths conflict with the
stated no-reader-retry/no-per-operation-seqlock law. Both parents contain them. Vestcut's
uncaptured MGET deletion stays deleted, but it does not remove the captured path's issue.
The sixteen functional passes do not establish compliance with that architectural law.
No reader validation is removed to hide the problem, and no feature is disabled to pass.

The production settax selector is 0. Inherited selector-1/3 research bodies still contain
in-place sequence-protected overwrite alternatives; they were neither enabled nor built.
No new in-place overwrite, reader retry, sequence-lock design, per-operation layout change,
three-stage runtime, or executor-side send engine is introduced by this merge.

This record supplies build, directed correctness, activation, and cleanup evidence.
The maintainer still owns the full gate and any PRE/POST performance verdict.

Additional validation: `tests/knobs.py 127.0.0.1 8377 uring 1` passed on a fresh split
16-shard, 6:2, key/client-LB=1 boot, including the independent INFO fields, retired-name
rejection, boot-only controls, live encoding fan-out, and actual placement. Its server
exited zero with all stuck fields zero; see `build/mergeinteg/knobs/2s000/`.
Python AST / shell syntax checks passed for all 21 changed script files. Whitespace
checks pass for source, scripts, and the merge record. The full staged check flags only
34 space-only context lines in the incoming `KNOB-REVIEW.patch`; that historical patch is
retained byte-for-byte, including its required diff context prefixes. The expected-count assignments were checked against both parents and the common
ancestor. All successful verification servers are reaped; the assigned port range has
no remaining listener. Only recorded child PIDs were signalled, always by Popen handle.

The tested release binary SHA-256 is
`c3e595258dc9590899ead8a615155167232af161d9f657b029d0b1b32ebeda81`.
The final atomicwindow helper SHA-256 is
`71655364af26d1dcd646bb2d305e2144a48b9fcc4178a857b06c5dc2e3d3b5f9`.
`build/mergeinteg/verification.json` combines all sixteen matrix command/exit records,
all fifteen completed atomicwindow records, digests, and EXPECT values. The supervisor
and raw evidence remain local build artifacts; this merge record is committed.

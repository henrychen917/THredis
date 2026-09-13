# Merge record — 2026-09-07

Both incoming lanes are merged into this working tree from common base
`78c3e53919a5d557bd3dd4625c9524d0baa212e4`. The dispatch-scaling retirement wins;
the timing rewrite cannot restore the deleted ownership instrument. The OFF RENAME
four-roll hard-failure control and the surviving mechanism/release-timing split both remain.
The merged gate needs **quick 325 / full 342**, with the expected-count constants untouched.

No build, server, benchmark, gate, C++ compiler, or server-connected battery was run.
The maintainer's 344/344, zero-SKIP result belongs to the incoming timing lane, not this merge.
Both source worktrees were read only. Their tracked-file inventories and file hashes were
rechecked after integration and were unchanged. Their untracked `CODEX-OUT.md` run transcripts
were not imported; the pre-existing transcript in this worktree was left alone.

Inputs: 12 timing paths plus 40 knobs paths, overlapping on the three named conflict files,
for **49 distinct input paths**, including the knobs lane's new files and wrapper deletion.
The SHA-256 digests of the captured `git diff HEAD --binary` inputs were:

- timing: `5310bed7954cc01a061bcb5c20e18b6b9f8096dfadbef7b94fb871fcfea4d3a9`
- knobs: `cb8a21fa050e9961dd1f3bed442d5de5f3b0df1e34e03fd183a622ad73ed3084`

## Conflict decisions

| File | Timing intent | Knobs intent | Decision and reason |
| --- | --- | --- | --- |
| `tests/atomic_torn.py` | Require counters, exact values, and safety on every build; score short promotion and shrink/reclaim budgets only with `--release-build`; strengthen credit sampling. | Remove configurable-window calls and exercise the derived production window, including a separate live-reconfiguration witness. | Retain timing argument parsing, `release_note`, required promotion counters, monotonic watchdog, unconditional hold/predecessor/promotion checks, and the 1.5-second release promotion budget. Use the knobs admission/reconfiguration block and its helper intact; the removed resize experiment cannot be run. The exact superseded hunks are listed below. The base RENAME control remains unchanged. |
| `tests/gate.sh` | Pass release identity to release atomic, stream, expiry, borrow, and dispatch arms; omit it for ASAN atomic. | Migrate all boots and CONFIG/INFO checks to the reduced grammar; replace the script bounds geometry and persistence surface check; retire dispatch. | Keep every knobs boot/surface change and every surviving timing call-site change. Only dispatch's invocation/flag and its live-instrument commentary are superseded by retirement. Correct the ASAN comment to describe the surviving promotion budget and mandatory derived-window witnesses. Neither EXPECT assignment changes. |
| `tests/xshard_dispatch_scale.sh` | Fix best-pair selection, freeze balancing, check each pair and repeatability, alternate arm order, and score all qualified release ratios against 1.20. | Delete the wrapper because its manual shard map is removed. | Delete. The rewrite still passes the removed manual-map flag. Its threshold qualification does not supply an equivalent geometry. Delete the Python helper too, rather than leave the retired instrument's code behind. |

### Semantic ruling and evidence

**No: the timing rewrite does not make dispatch scaling valid without `--shard-home`.**
The incoming timing wrapper still constructs `HOMES` as `sid -> 2 + sid % 2` at line 62
and passes it to the server at lines 81–82, along with independently disabled key/client
balancing and disabled flip. The first two executors own all 16 shards at both thread counts;
extra executor threads are deliberately empty filler threads, pinned away from the real owners.
This is the experimental control, not merely a spelling of a default.

In the merged source, `Placement::assign_shard_homes` assigns
`shard_home_[sid] = ex_[sid % ex_.size()]` (`src/core/placement.h:176–180`). The old 4-thread
arm has two executors; the 128-thread arm has 126. Without the manual map, the 16 shards in
the big arm occupy its first 16 executors, including 14 formerly empty filler executors.
Actual work, ownership, and contention change along with the array/thread-count dimension.
Changing the obsolete balancing flags to `--lb 0` would freeze this different geometry; it
would not restore the original one.

The timing rewrite's 2% test uses within-count cross/same cost spreads and same-shard controls
across counts (wrapper lines 133–145). Its final assertion requires every qualified ratio to
meet the unchanged 1.20 limit (line 164). These are repeatability checks, not an ownership
construction or validation of the limit on a new geometry. Its helper's distinct `x`/`y`
replies and end-of-arm owner-map comparison (Python lines 103–107 and 123–124) prove reply
content and within-arm stability, not equivalent work placement across thread counts.
Even perfectly repeatable arms could therefore assert the wrong claim.

The knobs lane's OBJECTIONS item 5 is accepted. The gate row, shell wrapper, Python helper,
and all of the timing lane's dispatch-specific implementation are retired. The rewrite is
**moot**, not partly retained as an unwired timing check. Historical measurements remain
explicitly historical; this merge makes no new dispatch-scaling claim.

### OFF RENAME and the atomic timing split

The complete base block from “Broadened movers: RENAME” up to the following store-family
section is byte-for-byte preserved. It still runs `for _roll in range(4)` at
`tests/atomic_torn.py:771`, arms and clears the hop delay in `try/finally`, retries only a
clean miss, and stops on a witnessed tear, worker error, or surviving helper. The OFF assertion
at line 782 requires positive invalid/read counts and no errors/live helpers. Four clean
misses fail; there is no OFF skip and no release-build condition around this control.
The base comment describing that policy is retained too. Each `rename_hammer` invocation
retains the base's state reinitialization and new connections/helpers.

There is no interaction requiring a weaker RENAME policy: it witnesses a detector, whereas
`release_note` scores the separate promotion completion budget. Missing hold, predecessor,
or promotion counters still fail on both build types. The latter retains its 30-second
mechanism watchdog and the original separate 1.5-second release budget (`:721`).

The three textual overlaps in the old admission/lease region are resolved together:

- The one-credit pipeline setup and the timing lane's required stall-counter reads are
  superseded by `held_burst(..., whole_window=True)` (`:900`). Its direct INFO indexing
  requires every counter to exist, and a positive held stall delta is mandatory.
- The old `lease_writer`, completed-write counters, resize sequence 31/7/19/3, 32 observations
  at bound 3, and exact final pool 3 are superseded by the second fresh held burst with
  `reconfigure=True` (`:908`). Live `CONFIG SET atomic 1` rebuilds generations; the test
  proves pre-CONFIG groups survived, observes the fixed bound and zero debt, checks every
  reply, and requires zero live groups/debt plus the entire derived credit pool after drain.
- The release-only three-second **shrink/reclaim** budgets and their measurements are dropped
  with that resize experiment. No shrink exists, and a timing claim measured under the old
  one-/three-credit setup is not transplanted to a held production-window burst. Reclamation
  itself is retained as mandatory mechanism coverage. The new helper's arm, resume, and
  drain deadlines are bounded failure watchdogs on every build, not successful timing skips.

`tests/atomicwindow.py` is otherwise exactly the knobs input. It clears the artificial hold
before starting the completion watchdog, retries only a joined/reclaimed witness miss on
fresh state, and fails after four misses. Bad replies, excess groups, debt, lost credits,
failed resume, and surviving helpers cannot be retried into success. Both separate outer
atomic battery invocations remain. The RENAME hit-rate evidence is not reused to claim a
hit probability for this different witness. Other base controls and timing changes are unchanged.

## EXPECT arithmetic, by emitting line

The actual quick exit is **base line 1258**, **merged line 1246**:
`if [ "$TIER" = quick ]; then`. The incoming design notes' other line numbers describe
older intermediate text; in particular, their “old exit 1260” is not the supplied base.
The following line numbers were read from the supplied base and final merged file.

| Ledger change | Base emitting line | Merged emitting line | Side of quick exit | Quick delta | Full delta |
| --- | ---: | ---: | --- | ---: | ---: |
| Retire `xscript off control`; former three-arm loop becomes `limit window` | 658, once per old arm | 653, twice for the surviving arms | Before both exits | -1 | -1 |
| Retire `cross-shard dispatch scaling` | 686 | none; retirement comment at 676 emits nothing | Before base exit | -1 | -1 |
| Retire old persistence CONFIG surface label, four iterations | 981 | none | Before base exit | -4 | -4 |
| Replacement `configuration reduction + actual geometry`, same two engines × two atomic settings | none | 969, four iterations | Before merged exit | +4 | +4 |
| Atomic admission/reconfiguration internals; outer release battery retained | 435 | 435 | Before both exits | 0 | 0 |
| Atomic admission/reconfiguration internals; outer ASAN battery retained | 1270 | 1260 | After both exits | 0 | 0 |
| All other outer battery rows and loops, including release-flag wiring | existing | retained | Same side of exit | 0 | 0 |

**Quick: `327 - 1 - 1 - 4 + 4 = 325`. Full: `344 - 1 - 1 - 4 + 4 = 342`.**
The knobs lane's numeric claim is confirmed. With the unchanged optional NIC row, full is 343.
Renaming engine labels from normal/uring to epoll/uring preserves two iterations; it does not
add or retire persistence coverage. The new Python helpers add internal checks to existing
rows, not ledger entries. The scratch experiments retired below have no emitting line in
`gate.sh`, so contribute zero to both tiers.

`EXPECT_QUICK=327` at line 141 and `EXPECT_FULL=344` at line 142 are byte-for-byte unchanged.
**The maintainer must update them to 325 and 342.** The ledger mismatch is intentional until
that edit; this merge does not make a failing suite appear green by changing its expectations.
Static comparison of all success-emitting labels, with the documented replacement and engine
label normalization, confirmed no other row was lost or added; the changed loop multiplicity
was checked separately.

## Complete incoming-file accounting

“Exact” means a full-file byte comparison against the captured working-tree input, so it covers
every incoming hunk, including additions. All production files under `src/` equal the knobs
lane byte-for-byte. All non-exact input files have a specific disposition in this table.

| Input path | Lane | Disposition |
| --- | --- | --- |
| `DESIGN-GATEHYGIENE.md` | timing | All incoming body text retained; historical scope and merged status added; section heading updated. |
| `DESIGN-KNOBS.md` | knobs | All incoming text retained; added provenance note pointing to merged line arithmetic. |
| `docs/specs/AUDIT-AOF.md` | knobs | Knobs exact: every byte/hunk retained. |
| `docs/thread-mode.md` | knobs | Knobs exact: every byte/hunk retained. |
| `src/base/topology.h` | knobs | Knobs exact: every byte/hunk retained. |
| `src/cmd/lbsignals.cc` | knobs | Knobs exact: every byte/hunk retained. |
| `src/cmd/server_tail.cc` | knobs | Knobs exact: every byte/hunk retained. |
| `src/cmd/t_server.cc` | knobs | Knobs exact: every byte/hunk retained. |
| `src/core/config.h` | knobs | Knobs exact: every byte/hunk retained. |
| `src/core/ex_loop.h` | knobs | Knobs exact: every byte/hunk retained. |
| `src/core/flipctl.cc` | knobs | Knobs exact: every byte/hunk retained. |
| `src/core/io_loop.h` | knobs | Knobs exact: every byte/hunk retained. |
| `src/core/iopipe_pipeline.h` | knobs | Knobs exact: every byte/hunk retained. |
| `src/core/placement.h` | knobs | Knobs exact: every byte/hunk retained. |
| `src/core/server.h` | knobs | Knobs exact: every byte/hunk retained. |
| `src/core/shard.h` | knobs | Knobs exact: every byte/hunk retained. |
| `src/core/weighted_lb.h` | knobs | Knobs exact: every byte/hunk retained. |
| `src/main.cc` | knobs | Knobs exact: every byte/hunk retained. |
| `src/net/tls.cc` | knobs | Knobs exact: every byte/hunk retained. |
| `src/net/uring.h` | knobs | Knobs exact: every byte/hunk retained. |
| `src/persist/aof.cc` | knobs | Knobs exact: every byte/hunk retained. |
| `src/store/eviction.h` | knobs | Knobs exact: every byte/hunk retained. |
| `src/store/flatstore.h` | knobs | Knobs exact: every byte/hunk retained. |
| `tests/_lib.py` | knobs | Knobs exact: every byte/hunk retained. |
| `tests/aof_rewrite_matrix.sh` | knobs | Knobs exact: every byte/hunk retained. |
| `tests/aof_rewrite_trigger_matrix.sh` | knobs | Knobs exact: every byte/hunk retained. |
| `tests/atomic_ryow.py` | knobs | Knobs exact: every byte/hunk retained. |
| `tests/atomic_torn.py` | timing + knobs | Both merged; every overlap is accounted for above. |
| `tests/atomicwindow.py` | knobs | Knobs exact: every byte/hunk retained. |
| `tests/borrow_registry.py` | timing | Timing exact: every byte/hunk retained. |
| `tests/bplus.py` | knobs | Knobs exact: every byte/hunk retained. |
| `tests/config_parser_test.cc` | knobs | Knobs exact: every byte/hunk retained. |
| `tests/differ.py` | timing | Timing exact: every byte/hunk retained. |
| `tests/evict_battery.py` | knobs | Knobs exact: every byte/hunk retained. |
| `tests/expireindex.py` | timing | Timing exact: every byte/hunk retained. |
| `tests/flipctl.py` | knobs | Knobs exact: every byte/hunk retained. |
| `tests/gate.sh` | timing + knobs | Both merged; every overlap is accounted for above. |
| `tests/infofix.py` | timing | Timing exact: every byte/hunk retained. |
| `tests/knobs.py` | knobs | Knobs exact: every byte/hunk retained. |
| `tests/lbsignals.py` | knobs | Knobs exact: every byte/hunk retained. |
| `tests/pubsub.py` | timing | Timing exact: every byte/hunk retained. |
| `tests/rlcache_churn.py` | knobs | Knobs exact except obsolete threshold-name comment now says learned band. |
| `tests/slowlog.py` | timing | Timing exact: every byte/hunk retained. |
| `tests/stream.py` | timing | Timing exact: every byte/hunk retained. |
| `tests/tls.py` | knobs | Knobs exact: every byte/hunk retained. |
| `tests/xscript.py` | knobs | Knobs exact: every byte/hunk retained. |
| `tests/xshard_dispatch_scale.py` | timing | Deleted with its sole wrapper; timing payload/ownership hunks retired. |
| `tests/xshard_dispatch_scale.sh` | timing + knobs | Knobs deletion retained; timing rewrite superseded by retirement. |
| `tomokv.conf` | knobs | Knobs exact: every byte/hunk retained. |

## Additional consistency edits and explicit drops

The reduced grammar also invalidated tracked callers and documentation outside the lanes'
file lists. These edits are part of making the requested merged tree consistent, not new
server features or new performance evidence.

- **Removed incoming timing work:** both dispatch files and the wrapper's release flag,
  threshold/evaluator, pair ordering, balancing freeze, exact payloads, and ownership
  stability checks. Reason: the entire instrument is retired, as ruled above.
- **Superseded incoming timing work:** all old configurable-admission and resize/lease
  sampling hunks, their associated three-second release budgets, and their old live/ledger
  descriptions. Reason: the public resize control is gone; the retained helper provides
  derived-bound liveness, old-generation reconfiguration, exact replies, and credit safety.
  No promotion, RENAME, blocking, borrow, expiry, INFO, pubsub, slowlog, or stream hunk is lost.
- **Knobs dispositions retained:** every deleted control, the `--lb` collapse, derived
  defaults, study spellings, Config 528 assertion, parser rejection coverage, and all new
  helper files. `DESIGN-KNOBS.md` retains its entire incoming body and all objections; a
  provenance note distinguishes its line references from this merged ledger.
- **Retired additional historical instruments:** `scratch/flipfp/fpmargin.sh` is removed
  because its signal/floor experiment required two independently selected bands, including
  a pinned 90% band. The S1b commands in `scratch/flipfp/chain.sh` are removed because their
  zero-band controller-cost control disabled retriggers. Replacing either with automatic
  bands would silently change its claim. These were outside the gate. Historical S1b result
  files remain readable by the report code and are labeled as a retired experiment.
- **Migrated other old boot recipes:** removed fixed-filter, explicit-band, and age-sampling
  flags where the scripts run the surviving battery or default automatic controller. Each
  changed driver states that its historical PRE/POST results need fresh validation. No
  runtime result is inferred from these grammar edits. The exact driver/report list is below.
- **Documentation:** updated current controls in `DESIGN.md`, `docs/flipctl-design.md`, and
  `docs/specs/AUDIT-TLS.md`; explicitly retired dispatch references in `NOTES-XPERF2.md`,
  `NOTES-HYGIENE.md`, and `AUDIT-TESTS.md`; recorded the margin retirement in
  `DESIGN-flipfp.md`; added current merged dispositions to `DESIGN-GATEHYGIENE.md` while
  retaining the incoming evidence. Historical research documents listed below are marked as
  descriptions of earlier revisions, not live configuration recipes.
- **Comments only:** `tests/flipctl_unit.cc` now describes the retained internal explicit-band
  test API, `tests/rlcache_churn.py` names the learned threshold, and
  `scratchpad/rlbatch/lib.sh` uses a surviving boot-flag example. No test behavior changed in
  these three edits.
- **Run transcripts:** neither lane's untracked `CODEX-OUT.md` was imported, because it is
  tooling output rather than a source change and would overwrite this worktree's own log.

Deleted knob names remain only where needed to assert their rejection, to name an unchanged
mechanism/counter, or to describe explicitly historical/deleted interfaces. In particular,
`config_parser_test.cc`, `knobs.py`, `bplus.py`, and `tls.py` deliberately require removed
names to be absent/rejected; deleting these references would drop the knobs lane's negative
coverage. No live boot/configuration recipe depends on an accepted deleted flag. Internal
ownership maps, atomic-window counters, and prefetch implementation names are mechanisms,
not restored public configuration aliases.

Tracked research drivers/reports adjusted:

- `scratch/rv.sh`
- `scratch/final.sh`
- `scratch/rv3.sh`
- `scratch/finalw.sh`
- `scratch/ver.sh`
- `scratch/red.sh`
- `scratch/red2.sh`
- `scratch/flipfp/fpshift.sh`
- `scratch/flipfp/bat.sh`
- `scratch/flipfp/fpprobe.sh`
- `scratchpad/robdiet/batteries.sh`
- `scratchpad/ringsize/batteries.sh`
- `scratchpad/rlbatch/batteries.sh`
- `scratchpad/replycode/batteries.sh`
- `scratch/flipfp/chain.sh`
- `scratch/flipfp/fpmargin.sh (retired)`
- `scratch/flipfp/report.sh`
- `scratch/report.py`
- `scratch/report2.py`

Historical research documents explicitly scoped to their recorded revisions:

- `AUDIT-CLEANUP.md`
- `AUDIT-LOCALFUSE.md`
- `AUDIT-TESTS.md`
- `AUDIT-resp3push.md`
- `DESIGN-GATEHYGIENE.md`
- `DESIGN-P0REPLY.md`
- `DESIGN-W-RLIDEAS.md`
- `DESIGN-flipfp.md`
- `NOTES-AOFFRAME.md`
- `NOTES-AOFSCRIPT.md`
- `NOTES-ATOMICS.md`
- `NOTES-CAPTURE-PREFETCH.md`
- `NOTES-EPOLL.md`
- `NOTES-HYGIENE.md`
- `NOTES-KTLS.md`
- `NOTES-MIGRATE.md`
- `NOTES-PERSIST-IO.md`
- `NOTES-SERVERTAIL.md`
- `NOTES-SNAPCUT.md`
- `NOTES-XSCRIPT.md`
- `P128.md`
- `scratch/NOTES.md`
- `NOTES-XPERF2.md`
- `NOTES-BARRIERTEST.md`

## Correctness-law review and validation limits

The Config exception is the task's explicit **624 -> 528** change. The retained assertion
accounts for `624 - 104 + 4 + 4 = 528`. Declared layout locks remain Op 336, Client 1984,
ThreadCtx 1408, Shard 1440, FlatStore 944, Rob<64> 192, and AtomicEntry 144. This is source
inspection, not a compiled size measurement.

No production source changes were introduced while resolving the lanes: `src/` equals the
knobs input, and timing is test-only. Ownership transfer/rebinding, immutable replacement,
read-local arming, QSBR, and RYOW code therefore receive no extra merge mutation. No new
correctness-law violation caused by combining the lanes was found in this review. This does
not claim either thread mode has been booted here or that a gate/regression result was obtained.

**Inherited objection retained:** `prepare_captured_local_mget` still sets `kAttempts=2`
at `src/core/ex_loop.h:1295`, loops at line 1317, and increments
`mget_generation_retries` at line 1432. This repeats a per-operation reader attempt and
conflicts with the stated no-reader-retries law. The path already existed at the common
ancestor and is explicitly called out in the knobs lane's OBJECTIONS item 1; the merge does
not hide it or claim to fix it. The ordinary `prepare_local_mget` two-attempt loop also remains
in the inherited source. No new retry/seqlock scheme or layout split was introduced.

The remaining objections are preserved too: actual legacy-alias uses, unmeasured LB/shard
sampling and transfer policies, stricter automatic SMT geometry, the retired manual-map
instrument, and the longer real LRU clock waits. The knobs lane's held-window repair still
needs maintainer release/ASAN validation at 16 shards, ratio 6:2, cores 0–7. Its five-second
arming watchdog is a bounded discovery budget, not a measured false-failure probability.

Completed server-free checks:

- Python AST parsing for all 21 affected Python files and `bash -n` for all 20 surviving
  affected shell files. No battery entry point was executed.
- Ten executions of only the source-extracted OFF RENAME control with fake hammer/debug
  callbacks: first/fourth-attempt hits, four clean misses, worker errors, and live helpers,
  each under both build identities. Misses failed after four attempts; errors/live helpers
  stopped retries; every armed delay was cleared. No sockets, threads, or server were used.
- Exact base comparison of the entire RENAME block; source inspection of unconditional
  derived-window callers and the surviving promotion release split.
- Input-path/hash accounting, full-file comparisons for retained lane files, static gate-row
  comparison and loop arithmetic, unchanged EXPECT assignments, layout-assertion inspection,
  removed-flag/caller search, conflict-marker search, and `git diff --check`.
- Reconstructed each merged conflict file from the captured inputs: the atomic file equals
  timing plus exactly the documented knobs admission replacement; the gate equals knobs plus
  exactly the surviving timing call-site changes and the documented comment resolutions.

No C++ syntax check, build, server, benchmark, full test, or gate result is claimed. The
maintainer supplies the merged runtime and performance verdict.

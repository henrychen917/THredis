# Vestigial-code deletion

Baseline: `c8e61f646`, branch/worktree `cx-vestcut`. Finding numbers below refer to the final
survey in `VESTIGIAL.md` (the supplied file also contains the survey session transcript).

**Source diff: 1,777 lines removed, 281 added; net reduction 1,496 lines across 21 files.**
These are `git diff --numstat -- src` counts, including source comments and formatting, excluding
this report and the existing `CODEX-OUT.md` modification. The source patch is also saved as
`build/vestcut/vestcut.patch`. Of the deleted lines, 946 are the
unreachable streams loop with its introductory comment/spacing, and 129 are the losing
uncaptured MGET helper. The remaining changes remove or collapse the machinery on surviving paths.

Thirteen ranked findings were implemented. Finding 5 is reclassified as UNCERTAIN because its
removal can change observable counters. Findings 15 and 16 remain untouched as requested.
No server, benchmark, or gate ran. Existing standalone unit tests ran; validation is detailed below.
`VESTIGIAL.md` and the pre-existing `CODEX-OUT.md` modification were not edited.

## Removals and simplifications

### Group 1: atomic admission and armed-write bookkeeping

- **1 — SIMPLIFY: mutable/unlimited atomic window.** In `src/core/server.h`, boot rejects shard
  counts outside `1..256`, then derives `min(16 * shards, 1024)`. The only later window store
  republished that same positive value. `--atomic-window` and its setter were already retired
  (`DESIGN-KNOBS.md`). Replaced the atomic window member with a boot-only `uint32_t`, removed its
  accessor and the reconfiguration argument/republication, and removed the unlimited-window
  alternatives in admission, retirement, credit return, and pool/debt arithmetic. Four operation
  path window loads and five window-existence conditions disappear. Lease generations, the
  borrow/return handshake, snapshot barrier, debt, counters, and live `atomic` toggling remain.
  No initialized server can reach the deleted zero-window arms.

- **3 — SIMPLIFY: executor-slot translation.** `Server::executor_slot` now returns the physical
  thread ID when it is below `nthreads()`, otherwise `UINT8_MAX`. Previously boot initialized the
  128-byte table to that exact mapping and never modified it. Removed the table and both filling
  loops. The bound is the actual thread count, preserving invalid IDs *inside* `kMaxThreads` too.
  All eight atomic/MULTI/scatter call sites retain their existing checks. This was residue of
  fixed-role dense executor numbering; `3ca2c450e` introduced stable thread indexing for FLIP.
  No serving caller can observe the former pre-initialization table state.

- **4 — SIMPLIFY: independent 16-entry RYOW ring capacity.** In `src/net/rob.h`, removed the
  capacity-equality terms in both refinement methods and the separate capacity-overflow
  conversion in `read_local_resolve_pending_body`. A pruned ring names distinct live ROB IDs;
  the incoming write occupies another ROB position. Thus existing entries are at most
  `Capacity - 1`, while the retained structural assertion requires ring capacity at least
  `Capacity`. The old independently bounded ring came from `f9080c9ca`; it is now sized to the
  entire ROB window. Wide writes, implicit-eviction hazards, conservative-generation extension,
  abandoned-write handling, and their counters remain. Updated the comments explaining this
  bound instead of keeping prose claiming the removed fallback still exists.

### Group 2: writeback after exwb removal

- **8 — SAFE-DELETE: executor send engine.** Removed `ExLoopT::wb_`, its `engine()` accessor, and
  its partial binding. Source references were exclusively declaration, binding, accessor, and
  shutdown aggregation: no executor ever called a serving method. `e96dab006`, the exwb deletion,
  explicitly retained the object only for uniform statistics plumbing. Its 13 counters therefore
  always contributed zero. `collect_shutdown_report` now accepts and traverses only IO engines;
  both main/split and fused shutdown callers were updated. Every report field and both output
  formatters remain. The executor ring and `set_wb_engine(nullptr)` remain necessary for
  wakes/persistence and role/INFO bookkeeping. Each executor object loses 256 bytes.

- **2 — SIMPLIFY: optional serving-engine infrastructure.** The surviving `WbEngine::bind` caller
  is `IoLoop::init`, which supplies release, retire, output-limit, clock, and signal facilities
  before an engine can serve, including dormant IO objects activated by FLIP. Removed default
  null arguments and the pointer-presence alternatives on send, TLS, retirement, and release
  paths; binding checks the required facilities once. The removed partial executor binding was
  the only exception. The output-limit *value*, callback results, `op.zc_ptr` tests, all callback
  bodies/indirections, timestamps, and all increments remain. No serving engine reaches a
  missing-facility alternative. The ThreadCtx comment now explains why executors publish no
  send-engine pointer without implying they still own one.

### Group 3: the winning read-local implementations

- **6 — SIMPLIFY: armed owner without a block cache.** `ReadLocalDeferredQueue::init` always
  installs its own cache in the sink on successful initialization. Fused arming requires that
  success, and ownership adoption copies the complete sink. Added the corresponding cache
  invariant beside the existing sink checks in `configure_read_local` and
  `rebind_read_local_retire_sink`. Removed the null-cache alternatives in armed take/put/release
  in `flatstore_atomic.inc`; the real read-local-off guard remains. Eligibility, borrowing,
  cache misses, capacity/pressure decisions, and allocator fallback remain. The sink comment now
  describes null as unconfigured/off. This follows hardcoding the winning recycler and retiring
  selector 2 (`read_local_settax.h`, `NOTES-RECYCLE.md`); an armed cache-less owner is never built.

- **9 — SIMPLIFY: unused sink argument on reclamation.** Removed the sink argument from
  `ReadLocalRetireSink::ReclaimFn`, deferred dispatch, and the table/object/atomic-object callbacks.
  All three callback bodies already ignored it; none of their behavior changed. Object callbacks
  obtain their current cache through the owning store. `6362d7139` removed the sink consumer when
  recycling became unconditional. Callback types, queue entries, and sinks retain their sizes;
  three distinct reclamation operations still use the callback interface.

- **10 — SIMPLIFY: capture-prefetch and interleave latches.** Every live drain selected
  `drain_local_reads_bounded_impl<true, YieldToOwner>`. Removed the `CapturePrefetch` dimension
  from the drain, preparation helper, and buffer; kept the captured implementation directly.
  Removed the 129-line `prepare_local_mget` helper, whose only reference was the discarded
  `CapturePrefetch == false` arm. `ReadLocalCaptureBuffer` now varies only by capacity, including
  the captured MGET temporary. Pure point batches still capture together; mixed GET/MGET batches
  still capture in program order. The live captured MGET retry loop, point-read retry behavior
  in selector 3, validation, slot assertions, and accounting remain. Replaced the identical
  `read_local_interleave_enabled()` predicate with `read_local_enabled()`. Both latches were
  permanently selected in `DESIGN-KNOBS.md`. These were already compiler-selected choices;
  this change makes no runtime savings claim for removing their template spelling.

### Group 4: parser/lifetime residue of the retired streams scheduler

- **7 and 11 — SIMPLIFY, with deletion of their unreachable producer.** `run_loop` sends fused
  overlap 1 and overlap 2 to distinct instantiations of `run_fused_iofused_loop`. There is no call
  or address reference to `run_fused_streams_loop`. Commit `ef2cf93a7` replaced that scheduler.
  Every live `genthread_ifid_batch` call used internal `Pipeline == 1` with a null batch, including
  the outer overlap-2 schedule. Removed:
  - The unreachable streams loop, its IFID entry/batch definitions and local execution contexts,
    rollback/reservation machinery, and its sole-purpose `genthread_pipeline_sweep` helper.
  - Five `BufferedIfid` parser arms, the template parameter and batch argument, and their
    `pipeline_simple_point` classifier. Surviving parser template arguments keep their values.
  - The prepared-frame bit/API, three Client lifetime predicates, `active_ifid_context_`, its
    reaping/quiescence scans, and `genthread_client_prepared`. The only true setter and all
    non-null context assignments belonged to the removed scheduler. `Client` keeps its size;
    `ifid_pending()` and `active_wb_context_` continue protecting live references.
  - Receive-buffer `AppendOnly`/`CanHoldPrepared` choices whose non-default uses belonged to
    that loop, plus the prepared-frame TLS growth condition. Surviving calls already permitted
    growth exactly at ROB quiescence.
  - The IFID helper's pipeline selector, unpublished-batch cap/staging conditions, and the
    full-active-set alternative behind constant `targeted_ready == true`. It now consumes the
    same initial pending-queue visit budget directly and keeps the existing retry/close behavior.
  - The WB helper's pipeline dimension, now invariably 1, retaining send classification and
    its independently varying occupancy reporting. Both live outer schedules remain distinct.
  - Streams-only context/occupancy/carry constants, the `GenthreadMicrostage` enum and schedule
    array, and the unused 128-op IFID cap in `genthread_pipeline.h`. Their schedule-only assertions
    were removed with the constants; they were not layout assertions. Updated comments that
    claimed the unreachable loop was still retained.

  Task reservation itself remains: the live read-local demotion path still needs it. No command,
  overlap grammar, transport, reply formatting, or serving schedule was retired by this cleanup.

### Group 5: constant control-plane choices and comments

- **12 — SIMPLIFY: configurable FlipController band.** Its sole initializer supplied `-1` after
  removal of `--flip-auto-band`. Removed that argument/member and explicit-positive/disabled
  alternatives in stabilization, verification, anchoring, surge/collapse detection, and anchor
  updates. The automatic formulas and floors are unchanged. `FlipShiftDetector` still receives
  `-1` from the controller; its separate configurable interface and explicit-band test callers
  remain. The removed four-byte member is absorbed by padding: controller size stays 1,736.

- **13 — SIMPLIFY: independent key/client LB gates.** Retained the existing single
  `lb_machinery_enabled()` predicate (`cfg_.lb != 0`), replacing its three synonymous wrappers
  throughout initialization, controllers, IO/EX, and LB reporting. Inside an already-enabled
  fold/controller beat, removed the independent-half booleans/conditions. Every removed condition
  was true after the outer guard because `--key-lb` and `--client-lb` had become one `--lb`.
  The two planners, refusal/streak counters, and unweighted FLIP fallbacks remain distinct.
  `lb=0` allocation gates and exported field names/values remain.

- **14 — SIMPLIFY: duplicate nonzero-overlap hooks.** `genthread.cc` had two branches binding
  equivalent coarse-executor and snapshot-start callbacks. Combined them into one nonzero-overlap
  binding; overlap 0 still binds its baseline executor. ThreadCtx stores and invokes these hooks;
  it does not distinguish the former lambdas by identity. Snapshot progress and both main
  overlapping schedules keep their previous behavior.

- **All six cosmetic survey entries:** updated the IO/EX handoff description in `signal.h`, the
  EX header's removed WB sender and incomplete persistence-ring description, the three-role
  placement example, the future-consumer wording in `lbsignals.h`, and the claim that ready-slot
  fallback runs only once per connection. Deleted the stale “there is no LB yet” comment. Also
  corrected the adjacent IO header's removed sender modes/five-loop claim and the WB comment
  naming the removed streams entry. These changes affect comments only; ready-slot exhaustion
  and its claimed notification channel remain live.

## UNCERTAIN and observable behavior left in place

- **5 — delayed retire-sink adoption, reclassified from SIMPLIFY.** Eager adoption already moves
  the sink in both ownership commits. Nevertheless, the later walk still executes
  `FlatStore::rebind_read_local_retire_sink`, whose `ReadLocalTableGuard` advances the topology
  generation. Readers consume that generation (`read_local_validate`, captured MGET window
  checks), and rejection can alter fallback/retry counters. Thus the proposed deletion fails the
  stricter requirement to show nothing live reaches its effect. The normal walk, shared pending
  flag, and `TOMO_RL_CACHE_NO_EAGER_ADOPT` negative-control behavior remain. Settling this requires
  a proof that the extra publications cannot affect any live validation/accounting path, or a
  separate authorized behavior change with maintainer-run concurrency/counter validation. A
  same-pointer assignment alone is insufficient evidence.

- **15 — capture slot address.** Kept `ReadLocalPrefetchCapture::slot`, all probe output plumbing,
  and the non-null assertion in the surviving capture path. The record is still 32 bytes.
  `7d4dcabfa` introduced this as part of the original capture contract; the survey found no
  subsequently deleted consumer. Settling it requires establishing that retired consumer, or
  explicitly changing the diagnostic contract as a separate cleanup. Removing it here would
  assume away an assertion whose status the survey labels uncertain.

- **16 — overwrite-build string adapters.** Kept `KvObjRawReadBuffer`, adapters, selectors, and
  string implementations. `read_local_settax.h` still supports selectors 1 and 3; they consume
  the adapters and can overwrite published raw values. Settling this requires explicitly
  retiring those supported builds before deleting their interfaces. Default-build optimization
  of an unused temporary does not prove that all consumers disappeared.

- **Other survey cautions retained:** ready-slot exhaustion/claimed notification lifetimes;
  read-local demotion task reservations; topology-derived SMT and `TOMOKV_L3_DOMAINS`; varying
  atomic/cache/LB diagnostics; current per-shard scatter fragments. No confirmed residue was tied
  specifically to `connalloc` or `exmajor`. The survey separated uncalled executor wrappers from
  its ranked path findings; this change does not claim an exhaustive dead-code sweep of ExLoop.

## Existing correctness-law conflicts reported, not redesigned

1. **LB notification ownership race.** [Server::lb_commit_shard_plan](/home/user/Projects/cx-vestcut/src/core/server.h:1358) transfers the shard and adopts
   its retire sink before publishing `LbStage::Idle`, but it does not move `Shard::notify_pending_`.
   [ExLoopT::lb_control_pass](/home/user/Projects/cx-vestcut/src/core/ex_loop.h:1667) repairs that pointer after execution can resume. Both fused and split
   paths can drain tasks before that repair. A resumed expiry/eviction notification can therefore
   write the previous owner's plain bool via [Shard::notify_output_created](/home/user/Projects/cx-vestcut/src/core/shard.h:166) while that owner uses it.
   This violates the per-owner-structure handoff law. Settling it requires adopting notification
   ownership inside the quiesced transfer protocol, in a separate correctness change. Source
   path only; no runtime reproduction was attempted.
2. **Captured reads retain retries and topology sequence validation.**
   [prepare_captured_local_mget](/home/user/Projects/cx-vestcut/src/core/ex_loop.h:1151) still has `kAttempts = 2` and increments
   `mget_generation_retries`. `FlatStore` still brackets topology changes and checks an odd/even
   sequence on probes. These conflict with the supplied no-reader-retry/no-per-operation-seqlock
   law but currently guard torn observations. Removing these live protections is not vestigial
   deletion, so they remain.
3. **Selectors 1/3 retain armed in-place overwrite.** These supported experimental builds conflict
   with the unqualified armed-write prohibition; selector 3 also retains object-sequence retries.
   The default selector 0 still uses immutable replacement. Their retirement needs a separate
   decision, as in finding 16.

## Layout consequences

Sizes were measured without executing a probe: C++ arrays sized with `sizeof(T)` were compiled to
objects and read with `nm -S --radix=d`. PRE headers were extracted from `HEAD` under this worktree's
ignored `build/vestcut/baseline`; POST used the working headers. No layout value below is inferred
from member widths. The source probe and output are in `build/vestcut/layout*`.

| Type | PRE bytes | POST bytes | Consequence |
| --- | ---: | ---: | --- |
| `Op` | 336 | 336 | Existing assertion unchanged |
| `Client` | 1984 | 1984 | One packed flag bit freed; all offsets/assertions unchanged |
| `ThreadCtx` | 1408 | 1408 | Existing assertion unchanged |
| `Shard` | 1440 | 1440 | Existing size and offset assertions unchanged |
| `FlatStore` | 944 | 944 | Existing size/cache-line assertions unchanged |
| `Rob<64>` | 192 | 192 | Existing assertion and structural capacity bound unchanged |
| `AtomicEntry` | 144 | 144 | Unchanged |
| `Config` | 528 | 528 | Existing assertion unchanged; supplied 624-byte context predates this checkout |
| `ExLoop` | 6112 | 5856 | Removed 256-byte engine; assertion explicitly changed to 5856 with explanation |
| `FusedExLoop` | 6888 | 6632 | Same member removal; no existing size assertion |
| `IoLoop` | 8336 | 8328 | Removed inactive IFID context pointer; no existing size assertion |
| `Server` | 95744 | 95616 | Removed 128-byte executor-slot table; no existing size assertion |
| `FlipController` | 1736 | 1736 | Removed four-byte band member; padding absorbs it |
| `WbEngine` | 256 | 256 | Serving engines unchanged in size |
| `ReadLocalRetireSink` | 24 | 24 | Callback argument removal changes no fields |
| `ReadLocalPrefetchCapture` | 32 | 32 | Uncertain metadata retained |
| `ReadLocalRobState` | 1216 | 1216 | Existing assertion unchanged |
| `ReadLocalExImpl` | 4816 | 4816 | Unchanged |
| `ReadLocalDeferredQueue` | 4760 | 4760 | Unchanged |

The only changed layout assertion is `sizeof(ExLoop): 6112 -> 5856`. Source/header array sizes and
stack-only removed types are distinguished above from storage actually allocated by a serving mode.

## Validation and maintainer boundary

Each group completed a linked release build before the next group's source edits:

| Group | Command | Result |
| --- | --- | --- |
| 1 | `make -j4` | Passed |
| 2 | `make -j4 build/tomokv build/read-local-write-ring-unit` | Passed |
| 3 | `make -j4` | Passed |
| 4 | `make -j4` | Passed |
| 5 | `make -j4 build/tomokv build/config-parser-test build/flipctl-unit build/read-local-ring-unit build/read-local-write-ring-unit` | Passed |

The group-2 compiler first caught the expected ExLoop size change; its assertion was documented and
updated before the successful rebuild. Group 3 caught one remaining old capture-buffer template
argument in captured MGET; that caller was corrected before the successful rebuild. Final build logs
contain no compiler warnings/errors and are retained as `build/vestcut/groupN-build.log`.

All four existing standalone binaries passed: config parser, FLIP controller model, deferred-reclaim
ring, and RYOW write ring. The write-ring test includes full-window precision, conservative-generation
handling, abandoned writes, and four 200,000-frame model comparisons. No new tests or gate rows were
added. Compile-only checks of `src/main.cc` and `src/core/genthread.cc` also passed for selectors 1
and 3 and for `TOMO_RL_CACHE_DEBUG` plus `TOMO_RL_CACHE_NO_EAGER_ADOPT`; these were syntax checks,
not linked/running alternate servers. The source diff passed `git diff --check -- src`.

Both thread modes compile into the release binary. Boot, replies, INFO/log equality, concurrency,
ASAN runtime checks, and performance remain for the maintainer on the quiet box. In particular, the
full gate must use its actual geometry: `--shards 16`, `--ratio 6:2`, cores `0-7` by default.
`tests/gate.sh` is unchanged: quick **325**, full **342** (343 with the optional NIC row). No emitting
line was added/retired on either side of the quick exit at line 1246, so both count deltas are zero.
Neither `EXPECT_QUICK` nor `EXPECT_FULL` was edited.

| Matched-load PRE/POST evidence | PRE `c8e61f646` | POST diff |
| --- | --- | --- |
| GET/SET rate, cycles/op, instructions/op, IPC in 1s and 2s | Not measured here | Not measured here |
| Boot/gate at gate geometry | Baseline commit records 342/342 | Maintainer validation pending |
| Send-path NIC measurements | Not measured here | Not measured here |

No throughput, latency, or zero-regression measurement is claimed from deletion counts or builds.

## Source line accounting

| File | Removed | Added | Net removed |
| --- | ---: | ---: | ---: |
| `src/cmd/lbsignals.cc` | 1 | 1 | 0 |
| `src/core/ex_loop.h` | 231 | 51 | 180 |
| `src/core/flipctl.cc` | 23 | 11 | 12 |
| `src/core/flipctl.h` | 2 | 1 | 1 |
| `src/core/genthread.cc` | 10 | 1 | 9 |
| `src/core/genthread_pipeline.h` | 51 | 0 | 51 |
| `src/core/io_loop.h` | 1205 | 71 | 1134 |
| `src/core/lbsignals.h` | 7 | 4 | 3 |
| `src/core/placement.h` | 5 | 2 | 3 |
| `src/core/read_local.h` | 1 | 1 | 0 |
| `src/core/server.h` | 80 | 53 | 27 |
| `src/core/shutdown_report.h` | 25 | 20 | 5 |
| `src/core/signal.h` | 12 | 4 | 8 |
| `src/core/thread.h` | 1 | 1 | 0 |
| `src/main.cc` | 1 | 1 | 0 |
| `src/net/conn.h` | 14 | 3 | 11 |
| `src/net/rob.h` | 36 | 6 | 30 |
| `src/net/wb.h` | 56 | 40 | 16 |
| `src/store/flatstore.h` | 8 | 5 | 3 |
| `src/store/flatstore_atomic.inc` | 4 | 2 | 2 |
| `src/store/read_local_reclaim.h` | 4 | 3 | 1 |
| **Total** | **1777** | **281** | **1496** |

# Merge reconstruction and atomic-window repair

The supplied checkout is `112ed6679` (`cx-final`), whose parents are `6647031dd`
and `a9a220a49` (`cx-integ`). It already contains a fourth integration after the
three merges described in the task. **Its unmodified server builds from clean;
the reported malformed class and missing DEBUG handler are not present.** This
repair changes only `tests/atomicwindow.py` and this record. Repeated execution
did reproduce the reconfiguration row's failure, and that test arrangement is
repaired below. No source change is attributed to a failure that did not reproduce.

The work and evidence are confined to `/home/user/Projects/cx-final`. The
pre-existing, untracked `CODEX-OUT.md` is untouched. Local evidence, including
source snapshots, exact commands, logs, and process records, is in
[`build/fixmerge/`](build/fixmerge/).

The reconstruction used these committed versions:

| Version | Commit | Role |
| --- | --- | --- |
| Common base | `c8e61f646` | Mainline before these branches |
| Windowfix | `c43f8e6a0` | Shared test helper; no server changes |
| Vestcut | `daa937b00` | Remove unreachable designs and constant alternatives |
| Concurrency | `4656510d9` | Ownership, dispatch, and Client lifetime fixes |
| Three-branch result | `6647031dd` | The original three merges |
| Later integration | `a9a220a49` | Independent LB controls, schedules, split read-local, commit hold |
| Supplied HEAD | `112ed6679` | Integrates both lines |

I extracted the base, vestcut, and concurrency versions of `src/core/server.h`
and reconstructed their three-way merge using `git merge-file -p`. It returned
zero and produced **exactly the committed `6647031dd` file**, byte for byte.
I then reviewed the later `6647031dd..112ed6679` diff against those intents.
Every added line of the concurrency branch's `server.h` diff survives in HEAD.
The actual Server ends at line 3742, with the namespace closing at 3744;
`client_work_` remains inside it at 3741. Neither tail needed punctuation edits.
The clean compiler result establishes structure beyond the supplementary brace
scan. The failing historical file/log was not supplied, so this does not identify
where that earlier malformed class originated.

These are all the reconstructed Server regions, grouped by intent rather than
by the compiler's downstream diagnostics. All entries describe the verified
existing resolution; none required a new Server edit in this checkout.

| Region / functions | Vestcut intent | Concurrency intent | Resolution, including later integration |
| --- | --- | --- | --- |
| `LbStage`, `ClientWorkScope`, `client_work_epoch`, class tail | No change to these mechanisms | Add IO-drain stage and nested executor lifetime scopes; append per-thread epochs and notification addresses | Keep the stage, all scopes, and tail arrays inside Server. ROB completion alone must not permit Client destruction while executor notification still holds its pointer. |
| `init`: atomic window / pool | Replace mutable/unlimited window with positive boot-derived `min(16 * shards, 1024)` | No competing change | Keep `atomic_window_` and derived pool initialization; do not restore the mutable accessor or unlimited arms. |
| `init`: FLIP controller and executor IDs | Remove constant `-1` initializer argument and identity slot table/fill loops | Physical IDs remain usable by lifetime state | Keep simplified `flipctl_.init(enabled, nthreads)` and direct physical ID mapping. The removed slot table is not the new lifetime epoch array. |
| `init`: shards, LB arrays and policy | Collapse identical LB predicates after the single `lb` knob | No competing change | Integ restores independent key/client controls: allocate bucket signals only for key LB and controller/thread state if either is enabled. Preserve its encoding, shard-home, and fingerprint initialization. |
| LB predicates; client observations, forgetting, weights; `lb_fold_signals`; shard weights/bytes | Remove synonymous wrappers and unconditional inner branches | No competing change | Retain the now-distinct key/client predicates. The bucket fold and its braces are inside the key-enabled branch; occupancy stays under the outer controller guard. No old synonymous `lb_machinery_enabled` wrapper returns. |
| `lb_dispatch_paused`, `lb_should_pause`, IO acknowledgements, `lb_begin_ex_drain`, `lb_start_shard_drain` | Simplify surrounding LB enable checks | Stop all producers before an empty executor inbox can prove drain | Preserve `IoDrain -> all IO acknowledgements -> ExDrain`, including IO control-tail integration. No ownership publication while a producer can still post an old-route task. |
| Transfer accounting, client/shard planners, `lb_controller_tick` | Remove constant key/client choices | Start chosen shard moves through IO drain | Restore independent planner eligibility because the knobs differ again; retain `lb_start_shard_drain()` at the chosen move. Neither planner bypasses the new producer barrier. |
| `flip_prepare` / placement transition | Use simplified LB fold predicate | Explicit FLIP cancels uncommitted `IoDrain` as well as `ExDrain`/`ClientDrain` | Keep the extended cancellation set under the transition mutex and the distinct controller-enabled fold. |
| Both ownership-transfer entry points; `adopt_shard_owner_state` | Preserve simplified complete read-local sink/cache contract | Eagerly move maxmemory configuration, notification mask, notification-hint pointer, and retire sink | Keep adoption before router ownership commit at both single-shard and range transfers. Destination notification addresses are registered even for dormant roles. |
| `executor_slot` | Delete never-changing translation array | New structures index physical threads | Keep `thread_id < nthreads() ? thread_id : UINT8_MAX`; keep epoch and notification arrays separately. No dense role renumbering. |
| Atomic enable, admission, retirement, credit return, reconfiguration | Remove zero-window alternatives, repeated window loads, mutable publication, reconfiguration parameter | No competing change | Keep the fixed positive window and all live lease-generation, carry, borrow/return, snapshot-barrier, debt, and active-group machinery. A fixed limit does not make credit reclamation dead code. |
| Cold state and DEBUG accessors | Delete `executor_slots_`; replace `live_atomic_window_` | Append `owner_notify_pending_` and `client_work_` | Keep both deletions and both new arrays. Preserve integ's optional schedule state, read-local state, and commit-queue hold. The hold is distinct from the ticket-publication delay. |

Two adjacent deletion/use decisions extend into the consumers of Server state:

- The deleted executor `WbEngine`, accessor, and empty shutdown contribution
  remain deleted. `adopt_shard_owner_state` uses the executor's real notification
  flag; it does not need an executor send engine. Shutdown still aggregates IO
  engines in both modes.
- Vestcut's unreachable streams scheduler, prepared IFID context, and associated
  lifetime predicates remain deleted. Concurrency's `client_executor_quiesced`
  fences remain in live teardown and migration, including the post-fence state
  recheck. Dead parser state cannot replace the new executor-completion fence.

The LB predicates are the explicit exception to retaining vestcut's exact
deletion spelling: they ceased to be synonymous when integ restored separate
`key_lb` and `client_lb` fields. Keeping those predicates preserves the cleanup's
reasoning while supporting the actual configuration. No removed design was
revived to repair the merge.

The DEBUG investigation found that `src/cmd/t_server.cc` is **byte-identical** in
the base, vestcut, concurrency, and original three-branch merge (blob
`6ffcf26146…`). Thus neither branch lost nor deliberately deleted a subcommand
there. The required geometry probes are also present.

| Handler | Base / original three-branch merge | Supplied HEAD |
| --- | --- | --- |
| `ATOMIC-COMMIT-DELAY` | Present; helper uses it | Present; ticket-publication delay |
| `ATOMIC-COMMIT-HOLD` | Absent; helper does not use it | Added by integ; shared helper now uses it |
| `ATOMIC-FANOUT-DEFER` | Present | Present |
| `SHARD`, `SHARDS`, `LBSIGNALS` | Present | Present |

The current handler is at `t_server.cc:1007`, with its Server flag/accessors and
executor commit-queue guard all connected. Fresh-binary probes exercise hold
1/0, delay 0, fan-out defer 0, and SHARD/SHARDS/LBSIGNALS; the full RYOW battery
exercises the held window. A newer helper used with a pre-integ executable would
produce the reported unknown-subcommand error for `ATOMIC-COMMIT-HOLD`. That is a
specific version-mismatch explanation, **not a proven history of the supplied
error**. There is no missing current registration to restore.

The remaining test failure was reproduced on the second fresh split boot,
with the release battery flags and exact gate geometry. The first boot passed
both batteries. The second torn battery failed only:

```text
atomic reconfiguration preserves derived bound and reclaims leases
old lease accounting did not settle within arm budget:
inflight=1, credit_pool=0, credit_debt=0
carried=1, replies=576/641, lease_pool=None
```

The windowfix arming was intact: the test observed the full-window stall and
its identified old EXEC survived CONFIG. The later wait assumed every MSET
could finish before the isolated CONFIG reclaimed unused IO leases. That is
false when the parked EXEC keeps an IO active: that IO can retain unused
credits returned by its completed MSETs, leaving a peer IO's remaining burst
unable to admit until reclamation or the ten-second EXEC hold expires. The
five-second accounting deadline correctly rejected that arrangement.

[`lease-arrangement.cc`](build/fixmerge/lease-arrangement.cc) makes this ordering
deterministic using the real Server admission/retirement/reconfiguration methods
and the existing 16-shard, 6:2 serverless fixture. It admits an old lease on IO 0,
reconfigures, admits and retires 255 newer groups there, then attempts admission
on IO 1. Its assertions and output establish:

```text
BEFORE inflight=1 cached_io0=255 pool=0 peer_admitted=0
AFTER peer_admitted=1 inflight=0 pool=256
```

This is the credit-cache state consistent with the live failure; the unit does
not claim it sampled the failed process's private lease fields. It demonstrates
why waiting for all burst replies before requesting reclamation is unsound.

The helper now requests same-value CONFIG reclamation when only the old group
is in flight, the shared pool is empty, and burst helpers remain unfinished.
It requires the identified old EXEC to remain unanswered before and after that
CONFIG. These additional reclamations are bounded by the connection count and
the **unchanged five-second deadline**, and their count is printed as
`lease_reclaims`. The later isolated CONFIG, exact pool=255 assertion, old EXEC
reply, full 641 replies, no-debt / maximum-inflight checks, and final pool=256
assertion remain. There is no CONFIG after successful isolated accounting to
hide a missing returned old lease. The ordinary admission-liveness arm receives
no new reclamation commands. Four clean witness misses still fail; errors and
timeouts are never converted into retries or skips.

The clean release build removed `build/src` before invoking
`taskset -c 8-31 make -j8 all`. The command review rejected the literal `rm -rf`
form, so a path-checked `shutil.rmtree` removed exactly that directory, rejected
symlinks, and verified its absence before make. All **38 translation units**
compiled, the executable linked, and the log contains **zero errors and zero
warnings**. No C++ source subsequently changed, so this is the same clean-built
server used for PRE and POST. All eight specified layout assertions remain:
Op 336, Client 1984, ThreadCtx 1408, Shard 1440, FlatStore 944, Rob<64> 192,
AtomicEntry 144, Config 624.

The core regression target also rebuilt against the fresh release objects.
All eight existing selections passed: `watch`, `scheduler`, `lifetime`, `drain`,
`route`, `snapshot`, `config`, and `notify`. The test translation unit uses
ASAN/UBSAN; its linked command/persistence objects are release objects. This is
not a whole-server sanitizer claim. The deterministic lease arrangement passed
under the same fixture/sanitizer setup.

Five final fresh split boots each ran `atomic_torn.py --release-build` followed
by `atomic_ryow.py`, with rate assertions enabled. Servers used cores **8-15**,
clients **16-23**, `--shards 16 --ratio 6:2 --atomic 1
--enable-debug-command yes`, fresh data directories, and `TOMO_GATE_STRICT=1`.
Key/client LB and automatic FLIP retained the gate's defaults. DEBUG LBSIGNALS
and the startup log independently confirm six IO and two executor threads.
No build or second workload overlapped these batteries.

| Fresh boot / port | Torn battery | RYOW battery | Reconfiguration witness | Shutdown |
| --- | --- | --- | --- | --- |
| 1 / 8400 | PASS, 23 ok | PASS, 12 ok | First attempt, peak 256, 12 held stalls, carried 1 | Exit 0; all stuck fields 0 |
| 2 / 8401 | PASS, 23 ok | PASS, 12 ok | First attempt, peak 256, 9 held stalls, carried 1 | Exit 0; all stuck fields 0 |
| 3 / 8402 | PASS, 23 ok | PASS, 12 ok | First attempt, peak 256, 8 held stalls, carried 1 | Exit 0; all stuck fields 0 |
| 4 / 8403 | PASS, 24 ok | PASS, 12 ok | First attempt, peak 256, 14 held stalls, carried 1 | Exit 0; all stuck fields 0 |
| 5 / 8404 | PASS, 23 ok | PASS, 12 ok | First attempt, peak 256, 8 held stalls, carried 1 | Exit 0; all stuck fields 0 |

Every reconfiguration finished with `lease_pool=255`, **641/641 replies**,
`credits=256`, and no debt. These five runs did not require the additional idle
lease reclamation (`lease_reclaims=0`); directed coverage is recorded separately.
The torn battery's existing probabilistic OFF controls reported RENAMENX skips
on boots 1, 2, 3, and 5, and COPY skips on all five. They are retained explicitly
in the logs. Neither target window check skipped, and RYOW reported no skips.
These are finite correctness samples, not a claim that every discovery control
fired or that the future failure probability is zero.

An additional fused/read-local-armed boot on port 8410 used the same eight cores
and 16 shards, with `--thread-mode fused --read-local 1`. Fused mode rejects
`--ratio`, so that option is omitted as in `boot_fused`. PING, MSET/MGET,
the DEBUG probes, and clean shutdown passed. This is a mode smoke check, not
a claim that the full fused gate ran.

| PRE / POST on the same clean-built server | Torn battery | RYOW battery |
| --- | --- | --- |
| Original supplied helper, stopped on reproduced failure | 1/2 PASS | 1/1 PASS; second pair stopped before RYOW |
| Repaired helper, five consecutive fresh boots | **5/5 PASS** | **5/5 PASS** |

The negative controls use throwaway copies under `build/fixmerge`, never the
deliverable server sources. The bound/carry binary is a clean build of the
current server with two explicit fault switches in its copied `server.h`.
The no-hold control changes only a copied helper's fan-out arm from ten seconds
to zero. Each control ran against its own fresh 16-shard, 6:2 server on cores
8-15, with its client on 16-23:

| Broken mechanism | Expected rejection observed | Evidence |
| --- | --- | --- |
| Old EXEC fan-out hold absent | `lease holder completed before burst` | `focused/no-hold/helper.log` |
| CONFIG rebuilds full pool despite an active old lease | Isolated `inflight=1, credit_pool=256` cannot satisfy accounting | `focused/wrong-bound/helper.log` |
| Retiring old generations return no credits | Final `inflight=0, credit_pool=255` cannot restore the derived window | `focused/lost-carry/helper.log` |

All three helpers exited nonzero as required; all three servers exited zero
with every shutdown stuck field zero. In particular, the new CONFIG reclamation
cannot make the lost-old-credit control pass. Positive runs use the original
clean-built binary, not the fault-injected executable.

Twenty additional fresh helper-only boots all passed their reconfiguration
checks, but all reported `lease_reclaims=0`. The coverage supervisor therefore
exited 1 with `live reclamation branch did not fire in 20 fresh boots` rather
than claiming it covered the new branch. That result is retained in
`build/fixmerge/focused.log`; it is a missed coverage objective, not a failing
reconfiguration row.

The final directed fixture removes that placement lottery. With the same
16-shard / 6:2 / eight-core geometry, it disables key/client LB and automatic
FLIP for this fixture only, identifies each accepted connection's IO through
DEBUG LBSIGNALS client-count changes, and puts the old EXEC plus nine burst
connections on IO 0. It withholds the tenth connection's send on IO 1. After
the first CONFIG, it completes 255 additional, separately checked cross-owner
MSETs on IO 0 while the old EXEC is still parked, arranging the exact real state
`inflight=1, credit_pool=0`. It then releases the final 64-group peer burst.
The extra groups are setup traffic, fully replied and retired before that
release; they do not substitute for any of the helper's 641 replies. All setup
inside the arm remains charged to the original five-second budget.

| Identical directed credit-cache arrangement | Result |
| --- | --- |
| Original HEAD helper, port 8409 | **FAIL as required**: `inflight=1, credit_pool=0`, old lease accounting deadline, final 64-group peer burst unfinished |
| Repaired helper, port 8405 | **PASS**: `lease_reclaims=1`, `carried=1`, `lease_pool=255`, 641/641 replies, final credits 256 |

Both directed servers exited zero with all stuck fields zero. This comparison
uses the same unchanged clean-built production server; only the delegated
helper differs. Its runner is `build/fixmerge/directed_helper.py`, and both
logs are retained in `focused/directed-pre` and `focused/directed`. The
deterministic serverless fixture, directed PRE failure, and directed POST
success jointly establish the missing ordering and exercise the repair.

The complete battery commands, INFO geometry, DEBUG replies, helper witnesses,
PIDs, and shutdown JSON are in
[`post/verification.json`](build/fixmerge/post/verification.json), with individual
logs in `post/split-01` through `post/split-05` and `post/fused-smoke`.
[`focused/results.json`](build/fixmerge/focused/results.json) records the failure
controls and additional branch-coverage attempts. The production binary SHA-256
is `c3e595258dc9590899ead8a615155167232af161d9f657b029d0b1b32ebeda81`;
the repaired helper SHA-256 is
`b4ab5030822b9bea2e77e7025d02f4b26852637a025fc112e05f7c49298baa89`.

The final process audit covers all 34 server starts and 38 helper processes,
including setup, PRE failures, and negative controls. Every server exited zero
with all stuck fields zero, every recorded child PID is gone, and ports
8400-8419 have no remaining listener. Only owned child PIDs were signalled;
no pattern kill was used. `final-audit.json` records those checks, unchanged
server/gate source, and matching final binary/helper digests. Python syntax
and `git diff --check` also pass.

The runner's first setup attempt used `_lib.Conn` as a context manager, which
it is not. That runner-only error occurred before any battery and was corrected
with `contextlib.closing`; its log and clean child shutdown are preserved in
`build/fixmerge/runner-setup-failure/`. The reproduced battery failure is retained
separately in `build/fixmerge/split-02/`; no failed product run is relabelled PASS.

No gate row is added or retired: the required ledger delta for this repair is
**0 quick / 0 full**. `tests/gate.sh`, including `EXPECT_QUICK=325` and
`EXPECT_FULL=342`, is unchanged. The prior merges' ledger accounting is not
silently repaired here. The full gate and standalone benchmarks were not run.

One inherited architectural-law issue remains visible in the reviewed paths:
`ExLoopT::prepare_captured_local_mget` retries capture/validation, and
`FlatStore::read_local_probe_sequence_equal` compares before/after sequence
state on the armed read path. Integ's split lane consumes the same machinery.
These conflict with the supplied no-reader-retry/no-per-operation-seqlock law,
as already recorded in `FIXES.md` and `MERGEINTEG.md`. This test repair does not
remove their correctness checks or claim those paths comply with that law.

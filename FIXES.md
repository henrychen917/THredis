# Concurrency lane fixes

Scope: the eight concurrency findings in `SURVIVING.md` (the input repeats its list),
plus review of the core read-local retry finding. Production changes are confined
to `src/core/ex_loop.h`, `src/core/io_loop.h`, and `src/core/server.h`. No command,
storage, networking, or persistence findings assigned to other lanes were changed.

## Fixed

Each row below is a separate selection of `tests/core_concurrency_unit.cc` and a
separate gate row. The test calls production methods; it does not implement a
reference scheduler or a substitute transfer protocol. Its fixture initializes
16 shards and 6 IO / 2 EX contexts without starting a listener, worker loop, or
io_uring instance. The transfer configuration/notification rows also use eight
fused contexts and exercise both transfer entry points. Missing fixture geometry,
an unopened interleaving, and missing completion are failures, never skips.

| Finding | Change | Test and required state | How to induce failure in a throwaway build |
| --- | --- | --- | --- |
| **#41 / S concurrency 5: migration reverses same-key execution** | Key-LB begins with `LbStage::IoDrain`. Every IO acknowledges from its control tail after its parse/post pass. Only then does the coordinator open ExDrain. The original owner must consume every published task before ownership moves. Existing stale forwarding remains a defensive check. | `route`: holds a routed SET outside an empty owner inbox, proves the missing IO acknowledgement prevents ExDrain, publishes the SET, proves it prevents executor quiescence, drains both owners, moves the shard, and requires the following GET to return `new`. | Change `lb_start_shard_drain()` to publish ExDrain directly, or remove the all-IO acknowledgement requirement in `lb_begin_ex_drain()`. The barrier assertion fails. |
| **#17 / C concurrency 3; C netproto 1: Client lifetime after Done** | Executor scopes publish a per-thread lifetime epoch through the completion/notification tail. After ROB quiescence, IO captures outstanding scopes and waits for each to end before teardown or migration. It rechecks protocol/claim state afterward. A subsequent busy executor pass does not extend the captured grace period. Scope nesting includes ordinary tasks, commit batches, blocking service, and slow-log tails. | `lifetime`: a test-only hook holds the real executor after its Done store and before notification. IO retires the Op through the backstop and attempts close plus four reap iterations. The Client must remain live. After notification finishes, close must succeed even with the next executor scope already active. | Remove the `client_executor_quiesced(c)` condition from `close_client()`. The Client is reclaimed while the hook is held; the retention assertion or ASAN fails. |
| **#18 / C concurrency 4: shard access after ExDrain acknowledgement** | The owner control tail finishes notification drain and the due LB census before any control pass can publish ExDrain acknowledgement. Fused read-local tail work also finishes before that edge. Split executes LB control before FLIP control so a pending LB rebind cannot follow FLIP acknowledgement. The later submit path performs no shard drain. | `drain`: starts with a due census, introduces ExDrain at the ordinary-pass tail, and checks from the post-ack hook that the census already ran. The hook replaces the owned-shard vector with a poisoned entry; no subsequent owner walk may touch it. | Move `lb_bucket_bytes_pass()` below the control acknowledgements in `owner_control_tail()`. The post-ack census assertion fails; a later walk also encounters the poisoned vector. |
| **#42 / S concurrency 6: foreign snapshot backlog** | `execute_snapshot_task()` verifies/forwards ownership before any pre-image preparation or shard access. A stale task cannot create snapshot debt on a former owner. | `snapshot`: moves a shard while retaining an old-route SET, opens a snapshot cut, and dispatches the stale task through the former owner's snapshot wrapper. Requires zero foreign preimages/backlog, exactly one forwarded task, a real Pending preimage on the new owner, successful backlog service, and the final value. | Remove the forwarding guard at the start of `execute_snapshot_task()`, leaving `execute()`'s later guard intact. The former-owner preimage/backlog assertion fails. |
| **#43 / S concurrency 7: migrated stale maxmemory configuration** | Both ownership-transfer entry points configure the incoming shard from a coherent live configuration snapshot before publishing ownership. This includes maxmemory, policy/samples, and notification/observer masks. Destination version caching cannot leave an incoming store on the source's old configuration. | `config`: source retains disabled maxmemory at V while destination caches enabled maxmemory at V+1. Move without changing the destination's cached version, then require the incoming store to reject an oversized SET and remain empty. Covers split/fused and single-shard/range transfer. | Remove `shard.configure_maxmemory(...)` from `adopt_shard_owner_state()`. The enabled-state assertion fails, and the incoming store would admit the SET. |
| **#44 / S concurrency 8: stale notification-hint pointer** | Executor construction registers its notification-hint address, including dormant FLIP executors. Both transfers bind that destination address and request a drain in the same quiesced critical section as ownership and the read-local retire sink. | `notify`: after an actual move, before any resumed loop rebind, clears both owner flags and calls the same hint producer used by expiry output. Requires only the destination flag to change. Covers split/fused and both transfer entry points. | Remove `shard.bind_notify_pending(pending)` from eager adoption. The producer changes the source flag and the row fails. |
| **#16 / C concurrency 1; S atomics 7: WATCH-disconnect null client** | The maxmemory/no-touch branch derives false for a tagged cleanup task without a Client, while retaining the branch's lazy evaluation when maxmemory is off. It also clears any preceding task's no-touch answer. | `watch`: enables positive maxmemory, completes a real WATCH registration, proves its reference pins the Client, creates the production tagged/null disconnect task, executes it, and requires removal of the watcher and release of the reference. | Remove `t.client &&` from the tagged-task no-touch expression. UBSAN reports the null Client access. |
| **#2 / C concurrency 2, MISPRICED: scheduler stack bounds** | Oversized experimental fused batches are scheduled in consecutive chunks of at most `kExecBatch`. All rank/index arrays and the 64-bit chain occupancy retain their existing proven bounds. No task can move across a chunk boundary. | `scheduler`: supplies 128 eligible tasks from four Clients, verifies task conservation and per-Client order, then supplies a 64-task single-Client run. Both exceed the original 32-entry scratch capacity. | Remove the `n > kExecBatch` chunking branch. UBSAN/ASAN catches the scratch-array overrun, including the path before the single-Client shortcut. |

## Not fixed

**#51 / S deadconf F10: read-local retries violate the supplied law — confirmed,
not disputed.** Both `prepare_local_mget()` and `prepare_captured_local_mget()`
permit two attempts and validate generation/cell epochs around value reads.
Removing only the extra attempt leaves the prohibited before/after validation
and demotion after failed reads. Removing validation itself permits a torn MGET.
Routing all MGETs away from the lane would retire tested local-read behavior and
invalidate existing lane-fired assertions. None is a law-preserving fix within
this lane's small, core-only changes. A replacement needs an immutable
command-wide read view spanning the storage probes and their version/topology
lifetime rules. That storage change and its atomic/torn-read validation were not
attempted here. No existing test was weakened, no counter tolerance changed, and
no gate row was added claiming this finding is fixed.

There are **no disputed findings**. The statistics reader finding (#4) and mixed
object-flags finding (#64) require the command/statistics and storage accessors
owned by the other lanes; they are outside this lane's changes.

## Verification and limits

- `make -j2 build/core-concurrency-unit all`: passed. Both split and fused server
  translation units compile and link. All existing layout assertions are intact:
  Op 336, Client 1984, ThreadCtx 1408, Shard 1440, FlatStore 944, Rob<64> 192,
  AtomicEntry 144, Config **528**. The Config lock was already 528 in this worktree's
  HEAD, differing from the supplied context's 624; it was not changed here.
  New lifetime/notification state is appended to the
  unlocked Server/IoLoop types, preserving their existing member offsets.
- All eight individual regression selections passed with
  `ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1`. The regression TU
  instantiates the exercised core methods under ASAN/UBSAN; command/persistence
  dependencies are the release objects, not a claim of whole-server sanitization.
- `bash -n tests/gate.sh` and whitespace checks on the changed source/test files
  passed. The first test run caught an unbound inert ring reference in the fixture;
  that setup error was corrected before the passing run.
- A throwaway header copy in `build/core-negative/` removed the eight mechanisms
  described in the table, then compiled the same regression TU against those
  headers. **All eight selections rejected that binary**: WATCH reported null
  Client access, scheduler reported a stack-buffer overflow, and the other six
  failed their corresponding state/result assertions. The working source was
  never broken to run the control. The copied headers and negative binary are
  uncommitted build artifacts.
- No server, benchmark, or full gate was run. Server startup/runtime coverage
  beyond the serverless fixture, the existing integration batteries, and any rate
  impact remain for the maintainer. These are correctness fixes; no performance
  improvement or PRE/POST result is claimed.

Individual rerun:

```sh
make -j2 build/core-concurrency-unit
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ./build/core-concurrency-unit lifetime
```

Replace `lifetime` with `watch`, `scheduler`, `drain`, `route`, `snapshot`, `config`,
or `notify`. The hook waits are bounded at ten seconds; the gate bounds each row
at sixty seconds. Unarmed builds contain neither hook storage nor hook calls.

## Gate row count

**Set EXPECT_QUICK to 333 and EXPECT_FULL to 350** for this worktree, excluding the
optional NIC row. The EXPECT constants were **not edited**.

This is counted from executable row-emitting lines, not the stale historical
comments or the shared-context baseline of 344. The existing tree emits 325 quick
and 342 full rows. The new loop emits eight rows at the static checks, **before**
the quick-tier exit. The full-only section still contributes seventeen rows, and
the optional NIC result still adds one separately. Thus 325 + 8 = 333 and
342 + 8 = 350. The shared build does not emit another row; a build failure fails
each of the eight dependent rows.

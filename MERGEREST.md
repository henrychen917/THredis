TomoKV remaining-branch merge record, 2026-09-09

The three requested tips are merged into the supplied `cx-final` parent `2587f70e6`.
The maintainer must set **EXPECT_QUICK=377 and EXPECT_FULL=394** (395 with the
optional NIC row). The assignments remain **335 / 352**, unchanged.
This work ran individual batteries only; neither tier of `tests/gate.sh` nor a
benchmark was run. Verification results are recorded below.

| Incoming branch | Incoming tip | Merge commit |
| --- | --- | --- |
| cx-fix-storage | `184b1480b` | `878af001a` |
| cx-fix-atomics | `b1c99c165` | `83e9c8258` |
| cx-fix-netcmd | `e94b97100` | `821d207b1` |

All three original tips are ancestors of the merged tree. Source worktrees and
branch tips were read only. All integration edits are in `cx-final`.
`CODEX-OUT.md` is the live session transcript: a command-scoped `keeplog` merge
driver retained the receiving tracked blob in every merge and left the live
working file in place. Its local modifications are excluded from the commits.
No repository-wide attributes or merge-driver settings were changed.

**Textual conflicts and resolutions.** Each gate union retains the incoming
rows and the existing concurrency/reorder rows; the EXPECT assignments were
never selected from an incoming parent or edited.

| Merge / file | Resolution |
| --- | --- |
| Storage / `FIXES.md` (add/add) | Keep the concurrency report at its existing path. Preserve the complete incoming report, byte-for-byte, as `FIXES-STORAGE.md`. |
| Storage / `Makefile` | Retain the existing reorder target and its `unit` dependency; add all three storage targets and their common source list. An initial resolution typo joined `build/reorder-` to the storage variable and made the storage link lack `main`; it was corrected before the successful clean rebuild. |
| Storage / `tests/gate.sh` | Concatenate the existing core/reorder block and all twelve storage rows, retaining the storage lane's TSan invocation and `setarch` preflight requirement. |
| Atomics / `FIXES.md` (add/add) | Preserve the complete incoming report as `FIXES-ATOMICS.md`; keep the existing report. |
| Atomics / `tests/gate.sh` | Retain all earlier static rows and add the build verdict plus all fifteen atomic selections. |
| Netcmd / `FIXES.md` (add/add) | Preserve the complete incoming report as `FIXES-NETCMD.md`; keep the existing report. |
| Netcmd / `tests/gate.sh` | Add the netcmd build and nine-case loop. Git placed one common `done` after the conflicting blocks, so the union explicitly closes both the atomic and netcmd loops. Retain the automatic addition to the shared feature list, which emits four further rows. |
| Netcmd / `src/cmd/scripting.cc` | Resolve twelve conflict regions as detailed next. Retain the atomics function-library activation/budget implementation outside those regions. |

The twelve Lua regions are: (1) the raw-field helper definition; (2) the `err`
lookup; (3) error-line encoding; (4) the `ok` lookup; (5) status-line encoding;
(6) `double`; (7) `big_number`; (8) `verbatim_string` plus its `format` and
`string` fields; (9) `map`; (10) `set`; (11) runtime error-table lookup; and
(12) caught instruction-limit failure handling. Regions 1–11 use netcmd's
`result_raw_field` and shared `reply_line_text`. The helper preserves Lua pseudo
indices and all conversions remain raw, so result conversion cannot execute
`__index` outside the protected activation. Region 12 keeps atomics' conditional
error conversion: a terminal timeout does not reinterpret a normal value returned
by a script that caught the timeout. Both terminal-failure checks and the
function-library budget/revoked-registration-closure fixes remain.

**Deleted-versus-used decisions and build-discovered incompatibilities.**

| Incoming dependency | Decision and reason |
| --- | --- |
| Storage fixture calls the old four-argument `ReadLocalRetireSink::ReclaimFn` | Keep the deletion of the unused sink argument. Adapt the test callback to `reclaim(owner, payload, auxiliary)`, matching every production reclaimer. |
| Storage fixture arms read-local with a null block cache | Keep the production requirement that every armed owner supplies its cache. Give the single-owner fixture a cache with explicit teardown; its immediate test grace periods retain no foreign reader. The unarmed fixture returns before creating it. No production fallback or in-place armed overwrite is restored. |
| Atomic fixture assigns `Config::lb` | Keep the obsolete combined member deleted. Set both `key_lb` and `client_lb` to zero, preserving the fixture's frozen ownership and the integration tree's independent knob surface. |
| Atomic fixture assigns `Config::script_instruction_limit=100000` | Update the reference: the surviving interpreter already uses the fixed `kInstructionLimit=100000`. The regression exercises that production budget and still fails if a caught timeout succeeds. A member is unnecessary to reproduce its prior setup. The operator-control policy remains explicitly unresolved in `DESIGN-KNOBS.md`; this fixture adaptation does not settle that separate issue. |
| Netcmd fixture calls partial/null `WbEngine::bind` overloads | Keep the deletion: the surviving sender requires a complete binding, and several paths dereference those callbacks unconditionally. Supply an unopened fixture Ring, clock, signals, limit latch and callbacks. Unexpected borrow-release/special-retire callbacks fail the fixture. No executor WbEngine or optional production callback branch is restored. |
| Direct inclusion of private `.cc` implementations in netcmd test TUs | Scope GCC's anonymous-subobject linkage diagnostic suppression to those includes, as already done by the atomic fixture. Production compilation retains its normal warnings. |
| DEBUG geometry/window hooks | No incoming use required restoring a removed subcommand. SHARD/LBSIGNALS, FANOUT-DEFER, COMMIT-DELAY and the integration COMMIT-HOLD remain distinct. The runtime batteries below exercise the surviving hooks, including the unchanged deterministic `atomicwindow.py` helper. |

The automatically merged production seams were also inspected: storage keeps the
reduced reclaim/cache contract and reader/owner cache-line boundaries; scatter
keeps physical owner IDs and fixed admission geometry; IO keeps the reduced parser
signature, split reader tenures, completion-lifetime fences, and existing schedules.
The new FLUSH invalidation hook runs at completed scatter retirement. No deleted
streams/IFID implementation or executor send engine was restored.

One automatic merge compiled but failed live verification: netcmd's preservation
of original CONFIG directives treated the integration tree's restored encoding
aliases as boot-only settings. `servertail` passed on the saved baseline and
failed all three initial merged runs, retaining stale `hash-max-ziplist-entries`
and `list-max-ziplist-size` lines beside the canonical live values. The resolution
uses `EncodingConfig::find` and its canonical names when identifying replaced
directives. Boot-only ACL/recovery lines remain preserved. The existing netcmd
config selection now seeds all five encoding aliases and requires their removal
plus canonical output; it adds no gate row. The repair is commit `8a7260059`. Post-fix results appear below.

**Clean builds.** Before each build attempt, `build/src` was removed completely.
The command guard rejected literal `rm -rf`; the equivalent `shutil.rmtree` was
used with a checked absolute path inside this worktree. No stale production object
was reused. The last build also used `make -B` to rebuild every selected unit.
Compilation used GCC 13.3.0 and the Makefile C++20 settings; the release
server links jemalloc and liburing. All compiler processes were pinned to cores
8–31. Each build phase completed before its live tests began; builds and
server-connected tests were never overlapped.

| Log under `build/mergerest/` | Result |
| --- | --- |
| `storage-build-1.log` | Failed: the Makefile resolution typo described above. |
| `storage-build-2.log` | Passed: release server and all three storage fixture variants. |
| `atomics-build-1.log` | Release server built; fixture failed on the two removed Config fields. |
| `atomics-build-2.log` | Passed: release server and atomic fixture, no warnings. |
| `netcmd-build-1.log` | Fixture failed on the removed partial sender binding. |
| `final-build.log` | Passed after the netcmd fixture repair: release server and all eleven unit targets, zero errors. |
| `rewrite-fix-build.log` | Passed after the CONFIG REWRITE integration repair: fresh release server and all eleven unit targets, zero errors. |

The final release server and ordinary/sanitized-core units compile without
warnings. The separate storage TSan target emits seven GCC `-Wtsan` warnings
that `atomic_thread_fence` is not instrumented; these are retained, not hidden.
Its three runtime `flags` checks pass with `setarch x86_64 -R`. This does not
claim that TSan validates the fence protocol; the selected test checks metadata
accesses. All locked sizes compile unchanged: Op 336, Client 1984, ThreadCtx 1408,
Shard 1440, FlatStore 944, Rob<64> 192, AtomicEntry 144, Config 624.

The final tested release binary SHA-256 is
`e9fc151df87d1f446bc634ab2b6e25870eb79ef6b628662c31bae22c6fa2be2b`.
The first fully merged binary, before the rewrite repair, was
`61a00ed7bf711002efb30dd4be64c7b663fd8b19449a66c80d504edec3b97ef4`.
The original, maintainer-reported clean baseline binary was copied before the
first build as `build/mergerest/baseline-2587f70e6`; its SHA-256 is
`c3e595258dc9590899ead8a615155167232af161d9f657b029d0b1b32ebeda81`.

**Verification resources and interpretation.** Live split servers use cores
8–15, 16 shards, and ratio 6:2; test clients/supervisors use cores 16–31.
Fused servers use the same eight cores and omit the unsupported ratio option.
Each live repetition starts a fresh server/data directory/hash seed. Primary
server PIDs and commands are recorded; cleanup signals only those children.
Servertail owns and stops its additional restart-fixture Popen children directly.
The selected one-shard borrow instrument retains its necessary shard-count
exception. Serverless fixtures retain their deliberate internal geometries.
Servertail's own restart fixtures retain their two-shard setup and are pinned
to the assigned cores/spare port. No measured performance claim is made.

An unrelated server and benchmark were observed at the start and left alone.
The RYOW battery uses `--no-rate-assertions`; every correctness and admission
witness still runs. The borrow growth benchmark is excluded: the separate
`borrow_mechanism.py` uses the existing battery's client and counter functions
to check exact replies, plain-versus-borrow send activity, retained borrows and
reclamation without calling its rate function. Growth/timing conclusions remain
for the maintainer's quiet-box gate.

**Serverless repetitions.** Every listed selection passed 3/3, for 147/147
invocations. Raw commands/results: `build/mergerest/final-units/results.json`.
The same 147 selections also passed before the rewrite repair; these are separate
from the final-build denominator.

| Selections | Passing invocations |
| --- | ---: |
| config-parser, flip-controller model, read-local retire ring, read-local write ring, reorder | 15/15 |
| core watch, scheduler, lifetime, drain, route, snapshot, config, notify | 24/24 |
| storage unlinked, randomkey, rehash, rollback, snapshot-eviction, flags (TSan), aof-eviction, intents, imported-hash, field-index-failure, hash-bytes, deadline-sidecar | 36/36 |
| atomic admission, closure, script_keys, rename_overlay, write_latest, script_apply, lua_conversion, watch_parent, watch_cycle, mset_arity, watch_oom, lua_lines, library_limit, stage_flag, instruction_limit | 45/45 |
| netcmd streams, zpop, notify-oom, notify-retry, flush, output, pubsub, receive, config | 27/27 |

**Live battery repetitions on the final binary.** All results below include a
successful battery exit and a clean shutdown report. Fresh-server repetitions are
counted separately; no successful rerun replaces a failed attempt in these totals.
The final functional set excluding the controller/open diagnostics is **95/95**
invocations: the 75 below plus sixteen orthog cells and four RL2S checks.

| Battery / arm | Final pass rate |
| --- | ---: |
| `atomic_ryow` | 3/3 |
| `atomic_torn` | 3/3 |
| `atomic_hazards` | 3/3 |
| `multirace` | 3/3 |
| `multi_exec` | 3/3 |
| `scriptsurf` | 3/3 |
| `xscript` | 3/3 |
| `streamgroups` | 3/3 |
| `tracking` | 3/3 |
| `climon2` | 3/3 |
| `limits` | 3/3 |
| `lua_scripting` | 3/3 |
| `bitfield` | 3/3 |
| `servertail` | 3/3 |
| `zsetops` | 3/3 |
| `edgeproto` | 3/3 |
| netcmd, 2s/uring, atomic 0 and 1 | 3/3 each (6/6) |
| netcmd, 1s/read-local 1, atomic 0 and 1 | 3/3 each (6/6) |
| netcmd, 2s/epoll, atomic 0 and 1 | 3/3 each (6/6) |
| TLS full userspace fallback, TLS1.2 + TLS1.3, 2s | 3/3 |
| TLS full userspace fallback, TLS1.2 + TLS1.3, 1s | 3/3 |
| borrow mechanism, one-shard registry | 3/3 |

`atomic_torn` retains `--release-build` for its completion watchdog and every
mandatory OFF/ON/window witness. `atomic_ryow` explicitly omits its performance
comparison; its held-window overlap, bounded admission and reconfiguration checks
still run. The other required batteries have no skipped arms. The formerly failing
owner-local hazard row therefore **did not reproduce (3/3 before the rewrite
repair, 3/3 afterward)**; this is an observed result, not a claim that storage's
diff caused or repaired the earlier branch failure.

TLS checks require each negotiated protocol and the userspace fallback counters,
then execute the new suppression sequence; a kTLS-only pass cannot satisfy them.
The borrow probe requires a positive send/release delta, a non-borrowing control,
at least 2,000 held borrows, exact subsequent GET values, and complete reclamation.
It does not validate the release-only 5% borrow growth claim, which remains untested
under the no-benchmark instruction.

The CONFIG REWRITE comparison is **baseline 3/3 → initial merge 0/3 → repaired
merge 3/3**. Each initial failure was the unchanged `servertail` assertion rejecting
stale encoding aliases; each repaired invocation passed all 111 checks. The other
eleven broader command batteries passed 3/3 both before and after the repair.
The pre-repair directed netcmd, TLS and borrow mechanism runs also passed 12/12,
6/6 and 3/3 respectively. Their logs are preserved separately from final totals.

Final raw results are `build/mergerest/final-{required,commands,netcmd,tls,borrow}/results.json`,
with the per-attempt command, PID, battery log and shutdown check beside them.
The supervisors are retained under `build/mergerest/`; a repeat can use a new
output directory, for example:

```sh
taskset -c 16-31 python3 build/mergerest/live.py required --output build/mergerest/repeat-required
taskset -c 16-31 python3 build/mergerest/verify.py matrix --output build/mergerest/repeat-matrix
```

**Controller verification and open diagnostics.** The flip-controller functional
battery passed **2/3 on the saved receiving binary and 3/3 on the final binary**,
using the gate's `--atomic 0 --flip-auto 1`, 16 shards, ratio 6:2, eight server
cores and `--stable-seconds 30`. The baseline failure was the known
`ramping load produced a rail anchor` assertion at 1 IO / 7 EX. Every successful
run completed ramp deferral, stationary hold, issue-rate surge and command-mix
re-maneuver checks. All six shutdown checks passed. `flipctl.cc`, `flipctl.h` and
`tests/flipctl.py` are unchanged from the receiving parent. These small samples
**do not establish that the known controller issue is fixed**; no controller
threshold or test expectation was changed. Raw controls and final repetitions
are in `baseline-controls/` and `final-flip/` under `build/mergerest/`.
Including these final controller runs, the positive live set is **98/98** battery
invocations, with the explicitly open diagnostics below kept separate.

| Incoming open diagnostic | Final result | Observed failure |
| --- | --- | --- |
| `atomic-survivors-unit post_apply_probe` | 0/3 passing; same defect reproduced 3/3 | Both APPENDs report 2, final value is `BW`; neither legal serial result `BSW` nor `BWS` occurs after the first owner's still-undecided script APPLY. |
| `netcmd-unit collection-oom` | 0/3 passing; same defect reproduced 3/3 | Failed multi-field HSET retains a changed prefix and the old field TTL. |
| `ktls-keyupdate` on TLS1.3 AES-128-GCM | 0/3 passing; same defect reproduced 3/3 | The sole TLS connection proves active kTLS, then reading PONG fails after a legal requested KeyUpdate. No fallback is counted as a pass. |

These diagnostics were already explicitly open in the incoming reports. They
remain outside the gate's passing loops, and no expectation was altered to hide
them. Logs and exact commands are in `build/mergerest/diagnostics/` and
`build/mergerest/final-ktls/`. All three diagnostic TLS servers exited normally
with clean shutdown reports. No test server remains listening in ports 8420–8449.



**Sixteen combinations on the final binary.** `tests/orthog.py` is unchanged.
All sixteen boots and activations passed, and all sixteen final shutdown reports
were clean. The four split/read-local cells also passed `tests/rl2s.py`, including
its FLIP round trip and resumed reader/owner checks. Evidence and exact commands
are in `build/mergerest/final-matrix/*/{evidence,commands,shutdown}.json`.

Enabled readers completed 32 GETs and 16 MGETs per thread. Split reader threads
owned zero shards and split executors recorded zero local-read hits before FLIP.
Disabled readers had no active telemetry/hits. Each enabled reorder cell observed
real multi-client permutations on its first fresh arming; disabled counters stayed
zero. Both schedule knobs off allocated zero schedule-witness threads. The max
column is the batch actually observed here; the serverless production-scheduler
unit separately exercises the full 32/128 capacities.

| Mode/RL/overlap/reorder | Active readers; total / MGET increments per reader | Schedule; passes/interleaved | Reorder batches/multi/permutations; max | Result |
| --- | --- | --- | --- | --- |
| 1s/0/0/0 | off | plain; 0/0 | 0/0/0; 0 | PASS |
| 1s/0/0/1 | off | plain; 0/0 | 810/15/8; 24 | PASS |
| 1s/0/1/0 | off | fused-overlap; 7176/1420 | 0/0/0; 0 | PASS |
| 1s/0/1/1 | off | fused-overlap; 6814/1347 | 818/27/23; 24 | PASS |
| 1s/1/0/0 | 8; each +48/+16 | plain; 0/0 | 0/0/0; 0 | PASS |
| 1s/1/0/1 | 8; each +48/+16 | plain; 0/0 | 793/29/24; 32 | PASS |
| 1s/1/1/0 | 8; each +48/+16 | fused-overlap; 7207/1406 | 0/0/0; 0 | PASS |
| 1s/1/1/1 | 8; each +48/+16 | fused-overlap; 6882/1333 | 804/32/24; 40 | PASS |
| 2s/0/0/0 | off | plain; 0/0 | 0/0/0; 0 | PASS |
| 2s/0/0/1 | off | plain; 0/0 | 815/11/6; 24 | PASS |
| 2s/0/1/0 | off | split-io-overlap; 5886/5886 | 0/0/0; 0 | PASS |
| 2s/0/1/1 | off | split-io-overlap; 5898/5898 | 821/12/6; 24 | PASS |
| 2s/1/0/0 | 6; each +48/+16 | plain; 0/0 | 0/0/0; 0 | PASS |
| 2s/1/0/1 | 6; each +48/+16 | plain; 0/0 | 821/14/10; 24 | PASS |
| 2s/1/1/0 | 6; each +48/+16 | split-io-overlap; 5880/5880 | 0/0/0; 0 | PASS |
| 2s/1/1/1 | 6; each +48/+16 | split-io-overlap; 5781/5781 | 824/11/5; 17 | PASS |

One **earlier shutdown anomaly remains recorded**. On the first matrix attempt,
`1s/1/1/0` passed activation but SIGTERM reported `live_conns=1`, with every ROB,
output and slot field zero (`build/mergerest/matrix/1s110/`, PID 3994952). That run
stopped after seven successful activation checks and six clean shutdowns. A
complete second matrix before the rewrite repair passed 16/16 including shutdown;
ten alternating repetitions of the affected cell passed on each of the saved
baseline and merged binaries (`paired-shutdown/`, 20/20 clean). The final matrix
above also passed 16/16. The supervisor's final INFO connection closes immediately
before SIGTERM, but the cause of the one anomaly is **not proven**. It is neither
silently retried out of the record nor asserted to be pre-existing. The supervisor
was changed only to record all matrix cells before failing on a dirty shutdown;
it still returns failure if any shutdown check fails. No production shutdown
code, grace threshold or test tolerance was changed.


**EXPECT arithmetic by emitting line.** The starting quick exit was line 1283.
The merged quick exit is **line 1335**, `if [ "$TIER" = quick ]; then`.
All additions are above it, so every addition contributes equally to both tiers.

| Added row | Emitting line / multiplicity | Quick delta | Full delta |
| --- | --- | ---: | ---: |
| storage named regressions | 367; eleven iterations from line 353 (including flags once) | +11 | +11 |
| storage deadline-sidecar | 375; once | +1 | +1 |
| atomic survivors unit build | 381; once | +1 | +1 |
| atomic named survivors | 389; fifteen selections at lines 383–385 | +15 | +15 |
| netcmd regression build | 394; once | +1 | +1 |
| netcmd named regressions | 397; nine selections at line 395 | +9 | +9 |
| netcmd split feature battery | 540; shared list line 532, atomic=0 and 1 | +2 | +2 |
| netcmd fused+armed feature battery | 599; same list, atomic=0 and 1 | +2 | +2 |
| TLS suppression checks inside existing battery | No new emitting line | 0 | 0 |
| Retired rows or added full-only rows | None | 0 | 0 |

Quick: **335 + 12 + 16 + 14 = 377**. Full without NIC:
**352 + 12 + 16 + 14 = 394**. Optional NIC adds one, yielding 395.
The storage build itself is a readiness flag, not an extra verdict; flags/TSan
is one loop row. Atomic and netcmd builds do emit their own verdicts. The shared
feature list is consumed twice per atomic mode, not once. Baseline concurrency,
reorder and restored dispatch-scaling rows remain present. The latter was not
run because it is a performance instrument. Constants at lines 141–142 are
byte-for-byte unchanged from `2587f70e6`.

**Inherited open correctness findings.** The source reports are retained in
`FIXES-STORAGE.md`, `FIXES-ATOMICS.md` and `FIXES-NETCMD.md`; their partial-fix
qualifications still apply. In particular, the atomic script post-APPLY conflict
interval can lose a successful script's update to an ordinary writer; an extra validation wave does
not close it. Expanded collection updates can retain a mutated prefix on OOM,
and kTLS KeyUpdate remains unsupported by the receive/rekey path. Snapshot-safe
hash-field expiry, due-field allocation failure and lazy empty-hash accounting
also remain partial. These merges do not establish that every audited defect
is fixed. The concurrency lane already supplies the WATCH-disconnect/lifetime
repairs and the storage merge supplies atomic rollback capacity; the other
cross-lane qualifications are not silently counted as passing regressions.

The receiving concurrency report's **no-reader-retry law violation also remains**:
`src/core/ex_loop.h::prepare_captured_local_mget` still allows two attempts with
generation/cell validation, and `prepare_local_read` still allows three attempts.
The obsolete uncaptured MGET implementation is deleted, but the surviving paths
retain their retry loops. None of these three merges modifies those functions.
Passing lane-activation and torn-read tests does not establish compliance with
the stronger no-retry law; removing validation alone would weaken torn-read safety.

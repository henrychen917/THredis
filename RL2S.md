# Split IO local reads

## Preconditions checked before implementation

These references are to baseline `78c3e5391` (before this diff).

1. **A reader can own zero shards.** GET resolves its home through
   `Server::shard(op.shard)`, not the reader's owned-shard vector
   (`src/core/ex_loop.h:1643`). MGET's route arrays contain global shard IDs;
   window open/close, generation validation, probes and final classification
   all use that same server directory (`src/core/ex_loop.h:1028`, `:1057`,
   `:1084`, `:1210`, `:1348`). `Server::shard` indexes the process-wide
   `shards_` directory (`src/core/server.h:534`). The reader copies the reply
   before completion (`src/core/ex_loop.h:1450`, `:1737`); it does not retire
   store objects. Owner housekeeping iterates `self_->shards()` and tolerates
   an empty vector. Reclamation initializes from Server/ThreadCtx without
   examining that vector (`src/core/read_local.h:260`), and callbacks carry
   their reclaim owner explicitly (`:300`, `:395`). No read-side indexed
   lookup requires the reader to own a shard.

2. **QSBR registration is independent of ownership.** Server initialization
   allocates a read-local sidecar for EVERY physical thread before assigning
   shards (`src/core/server.h:316`, `:323`, `:367`). The grace-floor scan
   visits all of `threads_`, including zero-shard threads (`:606`). These
   are the registration and scan sites the split IO readers must use.
   Rotation publication (`src/core/ex_loop.h:448`) and the park/resume pair
   (`src/core/io_loop.h:720`, `:745`; `src/core/thread.h:922`) bound the
   foreign-pointer lifetime. Registration alone is insufficient: every
   participant must advance or explicitly park. The existing split EX loop
   has neither read-local retirement draining nor these publications.

3. **A zero-shard thread can have a defined, stable sink.** The deferred
   queue creates its sink in `src/core/read_local.h:260`. Fused activation
   binds it to ThreadCtx BEFORE iterating owned shards
   (`src/core/ex_loop.h:241` versus `:243`). Thus zero shards still gives a
   valid per-thread sink. Loading precedes store arming
   (`src/core/genthread.cc:131`, `:165`; `src/store/flatstore.h:741`). At a
   later ownership move, `adopt_read_local_retire_sink` changes the store's
   sink in the same quiesced critical section as ownership
   (`src/core/server.h:1942`, `:2072`). Source retirement debt must already
   be empty (`src/core/ex_loop.h:1794`). The split implementation must bind
   every physical thread before serving, including dormant future owners,
   and retain those queues across role changes.

4. **This is an existing role with an added capability.** Split IO remains
   `Ifid`, serves clients and owns no shards; EX remains the owner role.
   `Server::serves_clients` and `owns_shards` already distinguish those
   capabilities (`src/core/server.h:545`). Split placement retains global
   `--ratio io:ex` counts (`:239`) and assigns all `--shards` among the EX
   tids (`:367`; `src/core/placement.h:178`). Fewer shards than EX threads
   can leave EX threads empty too. Fused placement remains unified, and its
   rejection of `--ratio` remains (`src/core/config.h:1222`). INFO must keep
   reporting `thread_mode:2s` and the live IO/EX counts, with actual lane
   activation reported separately.

All four prerequisites support the design. Merely removing the fused-mode
predicate at `src/core/server.h:582` would be incorrect: split IO would still
miss the lane, its notify-config refresh would dereference an unbound fused
executor, and owners would lack the armed command guards and reclaim drain.

Existing research-only overwrite builds are a separate law violation:
`TOMO_READ_LOCAL_SET_TAX_VARIANT=1/3` explicitly selects in-place writes and
sequence validation (`src/store/read_local_settax.h:1`,
`src/store/flatstore.h:2713`). Production selector 0 returns NotPossible and
uses immutable replacement. This task preserves the production path and
does not extend the existing bounded GET/MGET revalidation under review.

## Design as built

`src/core/rl2s.cc` contains the new runtime and the shard-less executor pass.
`main.cc` selects it only for split mode with the effective lane enabled.
The ordinary `2s --read-local 0` runtime is unchanged. `1s` keeps its existing
boot path, parser, lane scheduling, probes, revalidation limits and demotion
planner. The new selection branch is at the executor-pass boundary, not on
single-key GET admission. In particular, the existing pressure check still
guards evaluation of `active_.size()`; no admission expression moved ahead
of its old check.

On an enabled `2s/1` boot, all physical threads provision an `IoLoop` and
`FusedExLoop`. They use the
ordinary split task inbox layout and producer masks, including FLIP's
existing remasking. Initial persistence loading runs before store arming.
Each executor's `activate_fused` binds its permanent ThreadCtx sink, then
arms its owned stores; IO's empty ownership vector makes the second step
empty. The stop-aware boot gate holds all threads until every sink, store
and ring is ready. Only IO activates listeners. No read or armed mutation
can run while a thread's sink is unbound.

During IO tenure, `IoLoop::run_fused` selects the existing overlap-0 parser,
GET/MGET lane, ROB completion and network reply machinery. The two executor
pass entry points call `ExLoopT<true>::split_read_local_pass`
(`src/core/rl2s.cc:46`). It refreshes reader configuration, drains the
existing lane, maintains its existing pressure/cap state and publishes
quiescence. It never dispatches owner tasks or walks the owned-shard vector.
This matters during FLIP: the coordinator can install an IO thread's future
EX vector while that IO tenure is still acknowledging the transfer.
Blocking snapshot progress uses this same shard-less pass; snapshot owner
broadcasts still target only EX. The parser continues to enqueue EVERY
write on its current owner. The existing reservation/demotion plan orders
pending overlapping local reads before the write, and the existing ROB
write/owner fences preserve connection RYOW and reply order.

EX tenure uses the existing `ExLoopT::run` owner schedule, with the same
task execution, atomic deferral, expiry, notification, persistence and
completion transport. Instantiating it as fused-capable retains the armed
precise-write eviction guard and foreign-write scope that `ExLoopT<false>`
would omit. Stores use the existing immutable replacement and deferred
reclaim hooks. There is no executor-side network send. The owner loop
drains retirement each pass and retains the existing empty-retirement
condition for `ExDrain`; the debug build also runs the sink/owner assertion.

An EX tenure performs no local-read captures, so it publishes **parked**
for its entire tenure (`src/core/ex_loop.h:707`). Its registration and sink
remain present. On entry to an IO reader loop, the thread clears parked
before sampling/publishing the current epoch (`src/core/io_loop.h:541`).
It then uses the original rotation and network-wait publications. On exit
it clears the lane-active flag and parks before teardown. These are tenure
boundaries, not per-operation read-side synchronization. Ownership-edge
sink adoption is unchanged. All workers join before their queues are
drained and store hooks are disarmed at shutdown.

The shared fused drain charges execution to `LoopSignals::ops` as well as
the parser's charge. The split IO pass cancels that second charge once per
rotation: IO's existing dispatch-rate meaning, used by FLIP, survives local
completion. Read-local hit counters and command accounting still count the
completion. An IO-to-EX conversion invalidates the executor's cached config
version so its new shards receive current settings once dispatch resumes.

Configuration remains numeric and boot-only. Both modes accept read-local
0 and 1 at overlap 0. Split read-local 1 with overlap 1 is now rejected
explicitly, instead of accepting a lane that cannot run there. The existing
1s behavior with overlap 1/2 (notice and effective lane off) is unchanged.
`--ratio`, `--place`, `--shards`, key/client balancing and manual/automatic
FLIP retain split semantics; no new role or tuning knob was introduced.

The locked footprints and their asserts remain unchanged: Op 336, Client
1984, ThreadCtx 1408, Shard 1440, FlatStore 944, Rob<64> 192, AtomicEntry 144,
Config 624; the ordinary ExLoop assert remains 6112. The only data layout
change is the optional `ReadLocalThreadState`: 360 -> 368 bytes in the
production build, asserted at `src/core/thread.h:308`; its old 360-byte
prefix is also locked at `src/core/thread.h:306`. An atomic loop-entry
flag plus alignment occupies the extra word, after all existing members.
The diagnostics-only selector-3 sidecar already has different alignment
and is excluded from this production-size assert. Read-local 0 allocates
no lane, deferred queue, thread/server sidecar, or store read-local state.
Armed split allocates the existing fused-capable executor state on each
physical thread, including dormant future owners; measure that fixed
memory cost as well as throughput.

## Observable activation

`INFO server` still reports `thread_mode:2s`, live `io_threads` and
`ex_threads`, and effective `read_local:1`. When the effective lane is
enabled, it also reports:

```text
read_local_thread_0:role=ifid,shards=0,active=1,hits_total=123,mget_hits_total=7
read_local_thread_6:role=ex,shards=8,active=0,hits_total=0,mget_hits_total=0
read_local_active_threads:6
```

Those are illustrative rows for 6:2/16, not measured results. `active`
means the thread actually entered the reader loop, including its idle
network waits; it is not inferred from the knob or ownership. The atomic
flag is written only at loop entry/exit. Shard counts come from published
owner IDs, not concurrently modified ownership vectors
(`src/core/rl2s.cc:313`). After FLIP settles, every IFID row must have
shards=0/active=1, and every EX row active=0. As with other INFO geometry,
the individual samples are not one transactional view during a move.

With read-local disabled, both new field families are absent, preserving
the baseline INFO payload. Their caller guard is at
`src/cmd/t_server.cc:1998`; their scratch arrays and formatting live in the
non-inlined helper at `src/core/rl2s.cc:310`.

Per-thread `*_total` values are lifetime completions and survive FLIP and
CONFIG RESETSTAT. They can be nonzero on an EX row after that thread has
previously served IO. Aggregate `INFO stats` keeps its existing RESETSTAT
semantics. `read_local_hits` counts GET and MGET commands; GET completions
are `hits - read_local_mget_local_hits`. `read_local_keyspace_hits/misses`
count elements, so do not divide those by MGET command count and call the
answer an activation fraction.

## Validation delivered, not executed

Only source inspection, `git diff --check` and Python AST parsing were
performed. No C++ build, server, battery, benchmark or gate was run.
The parser/admission body and GET/MGET window/probe/copy region were also
compared directly with the baseline and are byte-for-byte unchanged.

`tests/rl2s.py` is a standalone directed battery for the 6:2/16 geometry.
It requires the enabled lane, checks INFO against the shard ownership
oracle, requires every IO thread to complete local GET and MGET traffic,
proves clean reads add zero shard-owner command executions, proves fresh
pure SET connections do not arm/allocate RYOW state, checks exact pipelined
RYOW replies, and repeats the read/write checks across both directions of
a real FLIP. Connection placement is retried on fresh sockets with a fixed
bound; missing a thread's lane is failure, never a skip. It requires key-LB,
client-LB and automatic FLIP off for exact counter attribution. No race
window is claimed by this test; use the existing held-window tests below.

The lane-admission and cache-churn batteries now accept enabled 2s lanes
and require actual activation. B+ selects a client-serving reader from the
role rows, still chooses participating owners from shard rows, and accepts
2s; its held-group and per-key generation assertions are unchanged. The
config parser battery adds the explicit split/overlap rejection check.

No gate row was added or retired. `tests/gate.sh` is unchanged:
EXPECT_QUICK remains **327**, EXPECT_FULL **344** (without the optional NIC
row). Its quick-tier exit is line 1258; the standalone RL2S battery has no
row on either side of that exit. Updating existing battery internals does
not change gate row counts.

## OFF-ARM PROOF

Compared against **`78c3e5391`**, for **`--thread-mode 2s --read-local 0`**,
including its supported overlap 0 and 1 schedules. References below name
the final working-tree sources unless explicitly marked baseline. This is
a source and construction proof; no build, server, benchmark or gate was
run. It does not establish identical generated instructions or measured
PRE/POST rates.

**Finding and fix.** The submitted diff did not preserve disabled INFO:
it counted shard owners, visited every thread and formatted
`read_local_thread_*` and `read_local_active_threads` on off boots too.
That changed replies and could grow INFO's output allocation. Its
`owned[kMaxThreads]` array also lived in the common `cmd_info` frame.
The fix guards the **call** with the effective predicate at
`src/cmd/t_server.cc:1998`, and moves the entire walk, scratch arrays and
formatting into `append_read_local_thread_info` in `src/core/rl2s.cc:310`.
`noinline` keeps those arrays out of the caller, including under LTO.
The helper receives only references, after the guard; it does not take
an eagerly computed topology snapshot or allocate before early-returning.
Disabled INFO emits neither new field family. Enabled INFO keeps both.
The offset assert at `src/core/thread.h:306` additionally locks the old
sidecar prefix; it adds no runtime state. The lane implementation is kept.

### 1. Threads, capabilities and roles: unchanged off

The effective predicate is `cfg.overlap == 0 && cfg.read_local != 0`
(`src/core/server.h:582`). It is false when the knob is zero. The only two
thread modes are Split and Fused (`src/core/config.h:225`): `main` returns
through the existing fused path at `src/main.cc:312`, then tests the
predicate at `:317`. Thus the **new runtime** can be called only for
Split **and** read-local 1, with overlap 0. Its entry also checks this
contract before construction (`src/core/rl2s.cc:77`). Mode, overlap and
read-local remain boot-only CONFIG entries (`src/cmd/t_server.cc:322`).

Off falls through to the old `vector<IoLoop>` and `vector<ExLoop>` at
`src/main.cc:338`; `ExLoop` still means **`ExLoopT<false>`**
(`src/core/ex_loop.h:3439`). It does not construct the new
`vector<FusedExLoop>` at `src/core/rl2s.cc:92`, bind a fused executor,
call `activate_fused`, or enter `run_fused`.

Placement still builds the ordinary split ratio/explicit placement
(`src/core/server.h:239`), creates `placement_.total_threads()` contexts
(`:294`, `:305`), and assigns shard homes to EX tids
(`src/core/placement.h:178`, `src/core/server.h:367`). Actual OS workers
are created once per EX tid at `src/main.cc:372` and once per IFID tid at
`:507`. At gate geometry this is **8 workers: 6 IFID, 2 EX, 16 shards on
EX**, not an additional reader tier. Main remains the existing monitor.

Baseline already provisioned the opposite dormant loop on every worker
for FLIP: dormant IO on EX at `src/main.cc:395`, dormant ordinary EX on IO
at `:528`. Those allocations/capabilities are not new. Off role changes
still call only `ios[tid].run()` / `exs[tid].run()` (`:426`, `:562`), so
FLIP cannot later upgrade an off worker to the reader runtime. The entire
ordinary split construction/role-loop/shutdown suffix starting at
`src/main.cc:336` was compared byte-for-byte with baseline and is identical.

### 2. Allocations, buffers, sinks and registrations: none added off

The inventory includes existing allocations newly reachable by the armed
split runtime, not just literal `new` lines in `rl2s.cc`.

| Construction or allocation | Guard and off-arm result |
| --- | --- |
| Server read-local sidecar (`new ReadLocalServerState`) | Existing predicate guard at `src/core/server.h:316`, allocation `:317`. False off; the existing owning pointer stays null. |
| Per-thread sidecars, tick publications, stats and sink slots | Same guard encloses the thread loop at `src/core/server.h:322`; `src/core/thread.h:353` performs `new ReadLocalThreadState`. Never called off, including for future FLIP readers/owners. |
| Per-shard extended store state and foreign-read safety arrays | Same guard encloses `prepare_read_local` at `src/core/server.h:328`. It reaches `new ReadLocalStoreState` at `src/store/flatstore_atomic.inc:890`. Off stores retain the original ordinary atomic-state allocation path. |
| `ReadLocalExImpl`, `LaneEntry[kInboxSlots]`, fallback array | `src/core/ex_loop.h:196` first requires compile-time `Fused`, then the effective predicate at `:197`; the three `new` sites are `:198`, `:200`, `:202`. The entire block is discarded for the off `ExLoopT<false>`. |
| Deferred `Entry[kCapacity]` buffer, block-cache heads and retire sink | Deferred initialization is inside that same block (`src/core/ex_loop.h:204`); buffer allocation is `src/core/read_local.h:260`, sink binding `:262`. Cache heads and retire-ring bookkeeping are members of the optional queue (`:464`), not a baseline per-thread allocation. No off queue exists to grow debug pending sets or retain blocks. |
| Runtime vectors, default-constructed loop containers and worker closures | `pool`, `ios(nthreads)`, `executors(nthreads)` at `src/core/rl2s.cc:90`, then `pool.emplace_back` at `:122`, are inside the guarded runtime call. This includes allocations by embedded container constructors, such as executor deques, before `init`. The off runtime constructs only the baseline vectors/loops described above. |
| Boot-gate worker-state vector, loading/error strings and shutdown report buffers | `FusedBootGate boot(nthreads)` at `src/core/rl2s.cc:95` allocates its vector in `src/core/fused_boot_gate.h:23`. The new runtime's local strings (`src/core/rl2s.cc:130`, `:235`, `:263`, `:281`) and report (`:115`) share the same call guard. No static boot gate or report is constructed for off. |
| Inbox channels, IO/EX rings, network registration and listener resources | All new-runtime calls are below its guard: `src/core/rl2s.cc:127`, `:129`, `:144`, `:196`, `:248`, `:264`, `:270`. Inbox storage comes from `src/core/thread.h:342`; rings from `src/core/ex_loop.h:213` and `src/core/io_loop.h:102`. Off calls the unchanged baseline initializers with the same parameters; it does not additionally run any of these RL2S initializers. |
| Role/client registration, capacity, completion, demotion and progress hooks | `src/core/rl2s.cc:147`, `:152`, `:160`, `:165`, `:167`, `:172`, `:181` are inside the enabled runtime. Fused sink publication and store arming occur in `src/core/ex_loop.h:241` and `:245`. Off never installs those fused hooks; its pre-existing ordinary IO registration hooks remain. |
| Connection RYOW sidecar and local-read reply reserves | Parser admission remains behind compile-time `Fused` and the effective predicate (`src/core/io_loop.h:4200`). Its write marking at `:4253` can reach `Rob::prepare_read_local` (`src/net/rob.h:486`, `:709`) only in that armed path. Local-copy reply reservation (`src/core/ex_loop.h:990`) likewise belongs to the existing reader lane. No new accept-time buffer or sidecar allocation was added. |
| New INFO ownership array, row buffer and output growth | Fixed: caller guard at `src/cmd/t_server.cc:1998` precedes helper entry; `owned` and `row` exist only in `src/core/rl2s.cc:313`, `:317`. Both `body.append` sites (`:336`, `:341`) are inside that helper. Off emits the baseline bytes and takes none of these buffer allocations. |

There is no new `.reserve()`/`.resize()` call in the feature file and no
file-scope runtime object or thread-local registration. Its project header
dependencies were already included by main/genthread. All feature-owned
vectors, strings, arrays and OS-thread creation are local to the two
guarded entry points. Existing optional sidecars store the hooks; no
separate always-present registry was introduced.

QSBR registration is the optional per-thread state allocated at boot,
not mere membership in the baseline `threads_` vector. Its floor scanner
(`src/core/server.h:606`) is driven by the optional deferred queues.
Neither the scanner, a tick/park publication, nor a sink binding is added
to an off thread. Existing store ownership-edge checks remain unchanged
(`src/core/server.h:1951`) and return for unarmed stores.

### 3. Hot loops and eager arguments: no added off-arm branch

| Changed test or call | Why off does not execute it |
| --- | --- |
| Runtime selection and overlap validation | `src/main.cc:317` and `src/core/config.h:1215` are boot-only, before operation processing. The changed startup notice at `src/main.cc:183` is also cold and silent with read-local 0. |
| IO reader-entry resume/epoch/active publication and exit publication | Both are inside `if constexpr (Fused)` at `src/core/io_loop.h:541` and `:766`. Ordinary `run()` selects `run_split<0/1>` (`:429`); every network/TLS/unix variant passes `Fused=false` (`:407`). The new calls and **all their arguments**, including `read_local_epoch()`, are discarded. Entry/exit are tenure boundaries even on the enabled arm. |
| EX parked/lane-count check, config invalidation, debug ownership check and retire drain | All additions to `ExLoopT::run` are inside `if constexpr (Fused)` (`src/core/ex_loop.h:708`, `:723`, `:800`). Off instantiates `ExLoopT<false>`; it does not evaluate `read_local_impl()`, `self_->id()` for the new check, or epoch/sink arguments. This also holds with `TOMO_RL_CACHE_DEBUG`. |
| New mode tests in baseline fused pass/sweep | `src/core/ex_loop.h:397` and `:640` live in `static_assert(Fused)` entry points. Their IO call sites are themselves under `if constexpr (Fused)` at `src/core/io_loop.h:6853`. Off never calls them. They do add a rotation-boundary test to the existing **1s** schedule; the 1s PRE/POST measurement obligation remains. |
| Shard-less pass's config, clock, pressure and cap checks | Entirely in the enabled-only `ExLoopT<true>::split_read_local_pass` (`src/core/rl2s.cc:46`). No callback to it is installed off. |
| `OwnsShards` config-refresh selection | `src/core/ex_loop.h:1913` defaults to **true** on all old calls. `if constexpr (OwnsShards)` adds no runtime test. Only `src/core/rl2s.cc:49` calls `<false>`. Expanding the true block gives the baseline owner-refresh token sequence. |
| Changed shared `Server::read_local_enabled` predicate | The ordinary parser's `Fused && ...` at `src/core/io_loop.h:3924` short-circuits at compile time. In common IO config refresh, the existing version-change return at `:7135` still precedes the predicate at `:7146`; changing the predicate adds no check to unchanged-version rotations. Other shared uses are boot/config preparation and INFO/RESETSTAT, not the GET/SET dispatch/execution loops. |
| New INFO caller guard | `src/cmd/t_server.cc:1998` is inside the INFO SERVER command section (`:1975`), not any general operation or worker loop. All new per-shard/per-thread reporting loops are behind it in the cold helper. |

The parser/admission/dispatch function at `src/core/io_loop.h:3918` is
unchanged. In particular, `if constexpr (Fused)` and the enabled/pressure
tests at `:3992` still precede `read_local_lane_quota(active_.size())` at
`:3995`. Off evaluates **neither** the quota call **nor** `active_.size()`;
there is no new cold-cache-line load disguised as a no-op callee.

Source comparisons discarded the `if constexpr (Fused)` statements in
both revisions of IO/EX rotation bodies; the remaining token sequences
match. The initializers, ordinary split dispatch entry points, parser,
owner task drain and IO config-refresh bodies also match baseline tokens.
These comparisons check the source specialization, not compiler output.

### 4. Per-operation layouts: unchanged off; optional growth disclosed

| Type | Baseline bytes | Current bytes | Existing layout assert |
| --- | ---: | ---: | --- |
| `Op` | 336 | 336 | `src/exec/op.h:463` |
| `Client` | 1984 | 1984 | `src/net/conn.h:905` |
| `ThreadCtx` | 1408 | 1408 | `src/core/thread.h:1208` |
| `Shard` | 1440 | 1440 | `src/core/shard.h:523` |
| `FlatStore` | 944 | 944 | `src/store/flatstore.h:3912` |
| `Rob<64>` | 192 | 192 | `src/net/rob.h:1098` |
| `AtomicEntry` | 144 | 144 | `src/store/atomic_mvcc.h:70` |
| `Config` | 624 | 624 | `src/core/config.h:429` |
| Ordinary `ExLoop` | 6112 | 6112 | `src/core/ex_loop.h:3450` |

No data members were added/reordered in `IoLoop`, `Server`, `ThreadCtx`
or `ExLoopT`: their member declarations match baseline, including the
existing optional pointer in `ThreadCtx`'s tail (`src/core/thread.h:1205`)
and the empty `ReadLocalExState<false>` (`src/core/ex_loop.h:103`, `:165`)
held with `[[no_unique_address]]` (`:3436`). The other locked type files
are byte-identical; the Config struct's only change is a field comment. Thus no
off-arm object has a new allocation stride, field offset or cache line.
IoLoop and Server have no numeric whole-size assert here; their unchanged
member/type layouts, rather than an invented size, are the evidence.

**One on-arm sidecar did grow:** production `ReadLocalThreadState`
**360 -> 368 bytes**. Baseline has an 8-byte tick, a 24-byte sink
(`src/store/read_local_reclaim.h:61`) and 328-byte stats (38 `uint64_t`
counters plus three arm counters: `src/core/thread.h:137`,
`src/net/rob.h:62`). The added `atomic<bool> lane_active` follows them
(`src/core/thread.h:303`); alignment adds seven padding bytes. The old
prefix offset is now asserted as 360 at `:306`, total size 368 at `:308`.
The diagnostics-only selector 3 retains its existing separate alignment
and is excluded from these production asserts. It too uses only optional
state. **Off pays zero bytes and no cache line for this growth**, because
`Server::init` never calls `ThreadCtx::init_read_local_state` off. Existing
hot fields remain at their old offsets on both arms.

After the INFO fix, all four off-arm source properties above hold. Static
verification completed: baseline source/token comparisons, Python AST
parsing of the four changed batteries (without importing/running them),
and `git diff HEAD --check`. The maintainer still owns build/layout-assert
execution, both-mode boots, the gate and the PRE/POST rates. No gate row
was added or retired; quick/full expectations remain **327/344**. The
quick-tier branch starts at `tests/gate.sh:1258` and exits at `:1260`; the
standalone RL2S battery has no row on either side of it.

## MEASUREMENT PLAN

The maintainer performs all steps below, on the quiet box. No performance
claim is established by this diff. The supplied reference observations are
the motivation: baseline 2s wins pure SET by about 28%; 1s+read-local wins
reads by about 2.2x; the hypothesized 2s read hand-off costs roughly 600
cycles and limits the measured shape to about 25M ops/s. Reproduce those
at the chosen geometry before interpreting a change.

### Correctness and activation before rates

1. Build production selector 0 and the normal unit/check targets. Run the
   unchanged full gate, expecting 344/344, at its actual geometry:
   `GATE_CORES=0-7`, `--shards 16`, split `--ratio 6:2`.
   Gate success alone does not validate RL2S: it does not add a split-armed
   row automatically.
2. Boot the four overlap-0 combinations: 2s/0, 2s/1, 1s/0, 1s/1. On eight
   real cores use 6:2 for split; omit ratio for 1s. Require INFO's effective
   lane bit to be 0, 1, 0, 1. Enabled active-reader counts must be 6 and 8;
   off arms must omit the new active-count/per-thread fields and retain
   their baseline INFO payload. Also smoke 2s/1
   with one shard and 6:2 (one EX has no shard), explicit placement, and
   the existing TCP/TLS/unix and epoll/uring boot/shutdown cases. Confirm
   split/1/overlap-1 fails with the documented message.
3. On a dedicated 2s/1, 6:2, 16-shard boot with debug enabled and key-LB,
   client-LB, flip-auto all 0, run `python3 tests/rl2s.py HOST PORT`.
   Run `tests/read_local_lane.py` at the same geometry; a cap that never
   produces admission deferrals must fail. Re-run the existing atomic
   torn-read, transaction/RYOW, B+, notifications/tracking, eviction and
   snapshot/AOF batteries against this split-armed boot. In particular,
   blocking SAVE must allow other owners' retirement to finish while the
   writer IO is occupied; include loaded-data boots and SIGTERM during load.
4. Restore default key/client balancing. Run `tests/rlcache_churn.py` with
   its documented arming setup and require actual shard moves plus local
   completions. Use `TOMO_RL_CACHE_DEBUG` to detect a sink crossing; this
   does not select in-place overwrite. Run manual FLIP and flip-under-load
   batteries, then automatic FLIP across read-heavy/write-heavy phases.
   Require actual geometry changes and positive completions on newly
   activated IO, with correct values before/during/after moves. A run with
   zero moves is not evidence for sink migration safety. Preserve the
   existing bounded fresh-state rearming for held-window assertions.
5. Negative controls, throwaway binaries only: route the armed split IO to
   the ordinary parser (keep the executor binding) to make the activation
   check fail; separately keep the loop active but force GET/MGET onto
   owner tasks to make the exact hit/owner-op checks fail. For sink
   migration, use the existing `TOMO_RL_CACHE_NO_EAGER_ADOPT` control with
   debug owner assertions and proven shard moves. Do not benchmark these
   binaries or commit them.

### Arms and named cells

Use identical optimized build flags, allocator, CPU affinity, shards,
dataset, seed, connections and offered load within a comparison. Keep the
client outside the eight server cores. Start with uniform keys, 1 million
preloaded keys, 128-byte values, no TTL, default atomic/filter/prefetch
settings and overlap 0. Time no preload work.

| Arm | Revision | Mode / lane | Eight-core placement |
| --- | --- | --- | --- |
| A PRE | 78c3e5391 | 2s / 0 | 6:2, then 4:4 |
| A POST | this diff | 2s / 0 | identical to A PRE |
| B POST | this diff | 2s / 1 | identical to A PRE |
| C PRE | 78c3e5391 | 1s / 1 | all eight threads, no ratio |
| C POST | this diff | 1s / 1 | identical to C PRE |
| D PRE/POST | both | 1s / 0 | all eight threads, no ratio |

B vs A and B vs C are the deciding comparisons; A/C/D PRE vs POST protect
the existing modes. `2s/1` on PRE is deliberately NOT a lane comparison:
that binary never enters it. It is useful only as a negative activation
control.

| Cell names | Traffic | Depth / connections | Purpose |
| --- | --- | --- | --- |
| GET-P1, GET-P16, GET-P128 | 100% existing-key GET | 1, 16, 128 / 256 | hand-off hypothesis and latency |
| GET-P128-C2048 | 100% GET | 128 / 2048 | admission pressure and fair share |
| SET-P1, SET-P16, SET-P128 | 100% overwriting SET, fresh writer sockets | 1, 16, 128 / 256 | retain/explain the 2s write advantage |
| MGET8-P1, MGET8-P16, MGET8-P128 | eight keys per MGET across physical shards | 1, 16, 128 / 256 | multi-shard local validation; report commands AND keys/sec |
| MIX95, MIX50, MIX41 | 95/5, 50/50, 41/59 GET/SET | 16 and 128 / 256 | useful mixed load and write-ring pressure |
| RYOW-P16, RYOW-P128 | SET k; GET k, plus independent GETs | 16, 128 / 256 | explicit conflict fallback vs unrelated reads |
| ATOMIC8-P16 | MSET8 writers with GET/MGET8 readers, overlapping and disjoint keysets | 16 / 256 | atomic-filter selectivity and torn-read protection |
| GET-S16, GET-S1024, SET-S16, SET-S1024 | GET or overwriting SET | 1 and 128 / 256 | reply/value size and send-path costs |
| YCSB-A, YCSB-B, YCSB-C | existing 0.17.0 traces, same seed/skew in all arms | 1 and 128 / fixed matched clients | common workloads, separate from uniform microbenchmarks |

Repeat the decisive GET/SET/MIX cells on the 25GbE two-netns rig. Loopback
cannot decide send-path behavior. Treat its roughly 12% rig tax as a
separate instrument effect, not a correction to apply to a mixed table.
Then repeat decisive cells on 16/32/64 real cores at identical splits per
A/B pair, using 128 shards for these scale cells and recording actual L3
domains. Include an IO:EX ratio sweep at eight cores (2:6, 4:4, 6:2, 7:1)
for A/B; report every ratio, not just B's best ratio against A's worst.

First measure steady throughput ceilings. Then offer the SAME absolute
load to A/B/C at 50%, 80% and 95% of the smallest stable ceiling, and sweep
through the overload knee. Use completed replies as the denominator. At
each matched load collect completed ops/s, p50/p99/p99.9, errors/timeouts,
and CPU utilization by IO/EX and L3. A higher saturation ceiling alone
does not excuse worse latency at a matched load.

### Counters, profiles and interpretation

Read INFO before/after each timed cell (avoid continuous INFO polling in
the measured interval). Archive boot geometry and configuration, binary
digest, per-thread rows and client-completed command counts. Require:

- `read_local:1` AND all expected IO `active=1,shards=0` rows for B;
  positive per-thread `hits_total`/`mget_hits_total` deltas on every IO
  carrying that traffic. No MGET traffic means no MGET hit requirement.
- For clean existing-key GET/MGET, near-total successful local completion;
  on the directed uncontended test it is exact. For the benchmark require
  at least 95% local command completions before calling the result a lane
  measurement. Record every `read_local_fallback_*` and
  `read_local_mget_fallback_*` reason, not just the aggregate. Missing GET
  keys, TTL expiry, typed values, tracking and key-miss notifications are
  separate fallback cells, not explanations for a clean workload.
- `read_local_defer_lane_full` and `read_local_defer_quota` for deep
  pipelines; `read_local_fallback_lane_full` must remain zero. Record
  `read_local_mget_generation_retries` without changing its existing limit.
- `read_local_arms`, `read_local_write_ring_sidecars`, and
  `read_local_write_ring_records`: all zero deltas for fresh pure-writer
  sockets; after warmup, transient arming fallback must not explain a
  sustained flat read result. Conflict workload fallback is expected, and
  must not be mistaken for clean-lane throughput.
- DEBUG LBSIGNALS shard operation counts, `lb_ex_queue_delay_samples`,
  `lb_ex_queue_delay_ewma_us`, wake counts and owner CPU demand: clean reads
  should stop producing owner execution/queue traffic. Do not infer
  reclamation progress from a hit counter. Track allocator/RSS and
  `mem_block_cache` through sustained overwrites and idle recovery, and
  require churn/shutdown checks to finish without unbounded retention.
- `tomokv_keylb_bucket_moves`, `flip_completed`, and post-move role rows in
  migration cells. Start stationary A/B/C attribution with balancing and
  automatic FLIP off, then repeat with default key/client balancing and a
  separately labeled automatic-FLIP phase.

Collect cycles and instructions for ALL server threads and separately
for IO/EX, plus IPC, cache/L3 misses, branch misses, and available Bergamo
load/store-stall and coherence events. Use the box's validated event
encodings. Profile local copy/validation, task publication/consumption,
owner stores, reclamation and network send at matched offered loads.
Compute cycles/op = instructions/op / IPC using the same completed-op
denominator and scope. The predicted ~600-cycle hand-off saving may be
partly replaced by copy/validation, cache traffic or retirement costs;
instructions alone cannot decide which arm won. Profile runs are separate
from clean timing runs, with identical workload settings.

Run at least five interleaved repetitions, randomizing or alternating arm
order, with identical-arm controls at the start/end. Report medians and
spread. The supplied quiet-box baseline is +/-0.15%; more than +/-2% on
identical arms invalidates the comparison and calls for contention/bug
investigation. No build or second benchmark may overlap a timed cell.

Fill this table for each deciding cell; blanks are intentionally unmeasured:

| Cell / load | A PRE 2s/0 | A POST 2s/0 | B POST 2s/1 | C PRE 1s/1 | C POST 1s/1 | B/A | B/C | Local fraction | p99 / cycles/op / instr/op / IPC |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| GET-P1 | pending | pending | pending | pending | pending | pending | pending | pending | pending |
| GET-P128 | pending | pending | pending | pending | pending | pending | pending | pending | pending |
| SET-P128 | pending | pending | pending | pending | pending | pending | pending | n/a | pending |
| MIX95 / MIX50 / MIX41 | pending | pending | pending | pending | pending | pending | pending | pending | pending |
| MGET8-P128 | pending | pending | pending | pending | pending | pending | pending | pending | pending |

A boot advertising the knob while active counts or completion deltas stay
zero is an **implementation failure**, not a null performance result.
If the lane demonstrably fires, owner hand-offs disappear, but B remains
at A's read ceiling and matched-load latency across the geometry sweep,
the proposed hand-off bottleneck explanation does not hold there. If B
loses A's SET advantage and C equals or beats it on reads/mixed cells,
there is no demonstrated useful operating region for this design. Any
reproducible regression in A/C/D PRE vs POST, correctness failure, reclaim
hang, or ownership violation blocks acceptance. A read gain with a SET
loss must be reported as that tradeoff; do not claim a general win or
retain the capability on an unmeasured assumption.

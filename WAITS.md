# Read-path waits

2026-09-07. Audited baseline: `78c3e53919a5d557bd3dd4625c9524d0baa212e4`.
References explicitly marked **baseline** describe that commit; other line references describe
this diff. This is a source audit and an **unmeasured candidate diff**, not a gate or performance
result. No compiler, server, benchmark, gate, or unit binary was run.

The two unconfirmed reports are confirmed. The diff removes lookup-triggered rehashing, replaces
both configuration snapshot loops, and removes generation-dependent atomic admission/retirement
accounting. **It does not establish that all reads finish without obstruction:** lazy expiry and
owner maintenance still reach the fixed QSBR retire queue, and admission backpressure can still
delay a connection indefinitely. Those are recorded below, not silently treated as solved.

| Site | Reachable with default settings? | Result of this diff |
| --- | --- | --- |
| 1. Lookup -> rehash -> full QSBR ring | No. Requires `1s`, `--overlap 0`, `--read-local 1`, resize, and a full ring. | Ordinary, no-touch and tracked atomic lookups stop advancing rehash. The queue's wait remains for other retirement paths. |
| 2a. Per-pass live-config snapshot | Yes, in both modes. A concurrent live-config setter is needed to make it wait. | Complete committed snapshots; no reader loop. |
| 2b. Output-limit snapshot during read reply processing | **Yes, including normal GET.** Default pub/sub limits arm the shared output check. | Same publication mechanism; no reader loop or skipped limit check. |
| 3. Atomic generation wait on admission rollback/retirement | **Yes for EXEC and staged cross-shard scripts**, even with default `--atomic 0`. It takes concurrent `CONFIG SET atomic` or `atomic-window` to expose the odd-generation wait. | No admission generation, carry, debt-repayment loop, or credit-operations handshake. One reservation attempt; one RMW per return. |

Default evidence: `src/core/config.h:318` selects split, `:320` disables read-local, `:350` disables
the optional atomic command lane, and `:360` defaults its window to AUTO. Boot resolves AUTO to
`min(16 * shards, 1024)`. Cross-shard script staging and cut slots default to auto at `:373` and
`:376`; baseline `src/core/server.h:202` resolves them to nonzero values. Ordinary standalone
GET does not use admission credits. Bare snapshot fan-outs are not all admitted groups either:
`src/cmd/scatter_engine.inc:1674` constructs `atomic_group` separately from `needs_snapshot` at
`:1818`.

## 1. Reader-triggered rehash and the fixed retire ring

**Baseline call path.** `src/store/flatstore.h:1209` (`find`) calls `rehash_step` before looking up
the key. The no-touch lookup at `:2270` does so too, including the `OBJECT` path exposed at `:1453`.
The third lookup, `atomic_find_tracked` (baseline `src/store/flatstore_atomic.inc:601`), also calls
rehash at `:602`; scatter reads use it when pending records exist (`src/cmd/scatter_engine.inc:2804`).
`rehash_step` at `:3691` dispatches to `rehash_step_read_local` at `:3304` when armed. Completion
unlinks the old table and retires it at `:3325`, through `retire_table_read_local` at `:3484`.
`src/core/read_local.h:299` enters `force_oldest_grace` if the 4096-entry ring is full; its loop is
at `:424`. The eight-slot migration budget bounds table work, not this retirement wait.

**Exact progress condition.** The ring must cease being full. After sealing pending entries,
`drain_ready` (`src/core/read_local.h:320`) frees only sealed batches whose stamp is strictly below
the grace floor. `Server::read_local_grace_floor` (`src/core/server.h:586`, baseline `:606`)
requires **every non-parked participant's tick to exceed the oldest batch's retirement stamp**.
Advancing the global epoch alone is insufficient. One stale active participant pins the floor.

The owner refreshes its own publication inside the loop. Each other blocking participant must
finish its foreign-pointer lifetime and publish a new tick, or safely publish the parked marker.
Rotation publication is at `src/core/ex_loop.h:453` and `:665`; normal park/resume publication is
at `src/core/io_loop.h:722` and `:751`. The tick/parked operations are at
`src/core/thread.h:923`. A thread can be descheduled, blocked elsewhere, or fail to reach this
boundary. A correctly parked or stopped-and-quiesced participant is ignored; an absent thread
that left an active publication is not. Neither `pause` nor `sched_yield` forces another
participant to publish quiescence.

**Worst case and fallback.** Infinite owner-thread suspension, with continued pause/yield work,
if one active publication never crosses the stamp. The ring has no growth, overflow list, timeout,
or failure return. Reads queued behind this owner may also stall. This can happen despite a
read's key having no conflict with the objects being reclaimed.

**Change.** `find` (`src/store/flatstore.h:1209`), `find_without_touch` (`:2271`) and
`atomic_find_tracked` (`src/store/flatstore_atomic.inc:601`) now search/resolve the two tables
without advancing their migration. A read should not have to retire a resize table.
This removes the obligation rather than speculating about how many grace polls will suffice.
Insert/erase and `active_expire` (`:1573`) still drive the move. The latter is called by owner
maintenance (`src/core/ex_loop.h:2002`), even without TTL keys. If that maintenance is disabled
and traffic contains only lookups/overwrites, both tables may remain resident longer; lookups
already search both. This is a retention/performance tradeoff, not permission to free either table.
Snapshot capture keeps its frozen table intact. No store member, QSBR stamp, pointer publication,
borrow lifetime, ownership transfer, or in-place overwrite rule changes.

**Unresolved: lazy expiry and non-lookup maintenance.** `live_or_expire`
(`src/store/flatstore.h:2599`) still physically deletes an elapsed key, via `erase_in` at `:2664`
and `erase_in_read_local` at `:3215`. The latter unlinks first and calls `retire_obj_read_local`;
the same full-ring wait is reachable without a resize. This is an armed owner lookup, including
an expired read declined by the foreign lane. Mutation, atomic cleanup, and owner maintenance
also retain their retirement obligations. Thus removing the rehash call is a partial result for
reader-triggered maintenance, not a claim that all `find` calls are now bounded.

At the current sink API, the object is **already unlinked** when capacity is requested. Returning
on a retry limit would lose the retirement record, overwrite a live ring entry, or reclaim before
grace. None is a correctness-preserving fallback. Growing storage is possible when allocation
succeeds, but does not provide a universal bound when allocation fails and a participant remains
active forever. Declining to the owner cannot help an owner already inside this call.

There is no finite-memory solution that both accepts arbitrarily many further retirements and
reclaims safely while an active participant retains an arbitrarily old pointer indefinitely.
One must stop creating retirements before unlinking, retain more memory, or allow waiting. A
future preflight could defer rehash before its table guard and defer physical expiry while still
returning logical absence. That also needs correct exactly-once expiry notification, AOF and
expiry accounting behavior: `find_notify` (`src/store/flatstore.h:1230`) currently emits expiry
before `find` performs deletion. Simply leaving the expired object there can emit repeatedly.
This diff stops at that obligation and leaves the ring unchanged; it does not declare the broader
redesign impossible.

## 2. Live configuration and output-limit snapshots

**Baseline evidence.** `src/core/server.h:3023` collects `LiveConfigSnapshot` in `for (;;)`, rejecting
odd versions at `:3029` and changed versions at `:3046`. The wrapper
`live_config_snapshot_if_changed` at `:3050` calls it whenever the version differs, **including
when it is odd**, contrary to its comment about waiting for a different stable version first.
`client_limits_snapshot` at `:2137` uses the same unbounded pattern and the same version word.

The per-pass callers are `src/core/ex_loop.h:1892` and `src/core/io_loop.h:7121`; split and fused
passes reach these refreshes. The direct read-reply path is `src/net/wb.h:197` selecting tracked
output, reply retirement/append, the limit callback at `:868` (also `:374` and `:941` for other
writeback paths), the callback binding at `src/core/io_loop.h:140`, and `client_obuf_check` at
`:7036`, which calls the snapshot at `:7043`. It is not limited to write commands.

**Default arming.** `src/core/config.h:49` sets normal limits to zero but pub/sub limits to 32 MiB
hard and 8 MiB/60 seconds soft. Baseline `src/core/server.h:3276` ORs normal and pub/sub arming.
Consequently normal GET replies still enter the snapshot before selecting their zero normal
limit. The configured replica limit does not participate in this arming decision. Turning off
both normal and pub/sub limits removes this direct reply call; it does not remove the per-pass
live-config readers.

**Exact progress condition and actor.** Both readers need an even starting version, all field
loads, and a second version load equal to the first. The current writer must finish
`end_live_config_update`, and other setters must leave a sufficiently long undisturbed interval
for this particular reader. Baseline `src/core/server.h:3356` serializes setters by CAS to an odd
version and `:3368` publishes the following even value. Writers execute on the thread handling
the setting/feature transition, including executor CONFIG work and IO-side tracking changes.

Every setter using this version can interfere even when its fields are irrelevant to the reader:
timeout (`:2130`), output limits (`:2152`), tracking (`:2182`/`:2188`), auth/ACL (`:2370`/`:2378`),
maxmemory (`:3059`), notifications (`:3069`), slowlog (`:3074`), save/protocol limits (`:1851`/
`:1863`), and the debug fan-out defer hook (`:2865`). Not every CONFIG setter uses it: e.g.
maxclients and TCP keepalive have independent single-field stores. Ordinary SET does not advance
this version. Atomic credit generation is a **different** word, discussed below.

**Worst case and fallback.** A descheduled/blocked writer can leave the word odd for an arbitrary
time; an absent writer that leaves it odd prevents completion forever. Continuous successful
setters can also starve a reader even though no writer is individually stuck. Neither reader has
a retry budget, cached coherent return, handoff, timeout, or failure result. `if_changed` returning
false for an unchanged version is a fast path, not a fallback after entering its loop.

**Change and safety argument.** `src/core/live_config.h:39` implements fixed per-worker mailboxes.
The serialized writer and the one physical-worker consumer each own one of three slots. An
acquire/release exchange transfers the third slot. The writer only fills its own slot; a reader
can stop indefinitely while retaining its slot without impeding publication or allowing reuse.
This is an ownership exchange, not a sequence-lock reader, pointer-reclamation protocol, or
refcounted shared pointer with a hidden lock.

Each publication includes the preceding committed configuration as well as the next one. The
writer stages every mailbox, then releases the even commit version (`src/core/server.h:3209`).
After taking its slot, the reader samples that word once. If it names the odd update that produced
this slot's next value, the reader returns the preceding committed value; otherwise it returns
the next value. Acquiring the slot orders its writer's odd-version publication before this load,
so it cannot choose an uncommitted value by seeing an earlier unrelated version. The preceding
copy handles coalescing too: a reader can skip many updates and still read the immediate prior
commit if it catches the newest update in progress. A slot consumed before commit becomes usable
after commit without another exchange. Callers copy their result before their next mailbox read.
The existing assumption against a full 64-bit version ABA during one paused call remains.

There is one consumer per **physical ThreadCtx id**, not per connection, shard, or changing role.
Fused IO/ex refreshes are sequential consumers on that same thread; split threads have distinct
mailboxes. CONFIG's snapshot call uses the executing shard's current owner. Shard migration does
not move a mailbox or add any per-shard owner structure. Boot allocates and seeds all mailboxes
before workers start (`src/core/server.h:278`), with a normal boot failure on allocation failure.
No update or read allocates, no amount of update churn grows storage, and no participant must
acknowledge to let the reader finish. Memory cost is `nthreads * sizeof(LiveConfigMailbox)` plus
one committed copy; this is core configuration storage, independent of optional read-local/MVCC
allocations. No new knob is introduced.

The unchanged per-pass path remains one acquire load. A snapshot read has two loads and at most
one exchange, followed by a fixed-size copy/selection. It always returns a coherent configuration,
even when the writer cannot resume. It neither ignores limits on collision nor supplies a default
value in place of a configured value. The preceding commit is valid while a new setter remains
in progress; the completed new commit is available to every worker. Writer serialization remains
unbounded in `begin_live_config_update` (`src/core/server.h:3197`). This change bounds snapshot
readers, not CONFIG setters or teardown callbacks that themselves change configuration.

## 3. Atomic admission and retirement

**Baseline evidence and the two different objects.**

- The admission-generation word is `atomic_credit_generation_`. `atomic_can_admit`
  (`src/core/server.h:2449`) returns false immediately on odd at `:2453`.
  `atomic_try_admit` also rejects odd at `:2467`; the pool handshake revalidation at `:2475`
  decrements `atomic_credit_ops_` and returns false. These are bounded early rejection branches.
- After incrementing active admission, the later revalidation at `:2501` can fail and call
  `atomic_release_admission_credit` at `:2507`. Normal `atomic_retire_group` calls it at `:2523`.
  That helper loops on odd at `:3290`. The bounded precheck does **not** protect a later rollback
  or a group admitted before CONFIG started.
- This word is unrelated to `live_config_version_`. It is also unrelated to a transaction's
  group publication epoch, commit ticket, or MVCC read cut. Changing/retrying one cannot stand
  in for a decision about another.

All references in that list are baseline lines. Baseline `src/core/server.h:2479` also retries
pool CAS while nonzero. Retirement retries the carry CAS at `:3301` and shared debt CAS at `:3311`.
Weak CAS has no strict attempt bound; under interference even success by other threads does not
bound the individual call. These extra retry loops are removed with the protocol too.

**Who reaches it.** `src/cmd/multi.inc:1427` handles EXEC and force-admits at `:1434` before
preparing the queued commands; there is no read-only exemption. OOM rollback calls retirement
at `:1438`, and `release_admission` at `:202` handles normal retirement/cleanup. Thus even an
EXEC containing only GETs uses this mechanism at `--atomic 0`. Staged scripts set `atomic_apply`
unconditionally for `Kind::Script` (`src/cmd/scatter_engine.inc:1674`), force-admit at `:1837`, and
retire on allocation failure at `:1848`/`:1856` or in `xshard_destroy` at `:2064`.
`src/core/io_loop.h:124` connects transaction retirement to reply writeback. Both modes share
these command/retirement implementations and the Server helpers.

The nearby script cut-window check (`src/cmd/scatter_engine.inc:1822`) is another distinct object:
`pool.can_register_snapshot` can return a BUSY error before admission. The general snapshot-pool
check at `:1829` returns backpressure. Neither is the credit-generation retirement wait, and
neither should be removed as a supposed cure for it.

**Exact progress condition and actor.** The retiring/rolling-back IO waits to observe an even
credit generation. The thread performing `CONFIG SET atomic` or `atomic-window` must finish
`atomic_reconfigure_credits` (baseline `src/core/server.h:3319`) and store the next even generation
at `:3353`. Before it can do so it waits for `atomic_credit_ops_ == 0` at `:3335`, then snapshots
the IO active mirrors and rebuilds carry, pool, and debt. Announced borrow/return operations on
other IO threads must leave their critical sections. A descheduled CONFIG thread, or a paused IO
after it announces a credit operation, can therefore stop retirement indefinitely. A correctly
idle IO with no announced operation cannot pin that handshake; an abandoned nonzero operation
or odd generation can. Continuous reconfiguration can keep a particular return from seeing even.

**Worst case and fallback.** Infinite synchronous pause-loop suspension during admission rollback
or response retirement, including `atomic-window 0`: the helper waits on odd **before** testing
whether the window is unlimited. There is no fallback in that helper. Early rejection has the
existing scheduling fallback: preserve the frame and revisit it on later IO passes. For scatter
this is `src/core/io_loop.h:4967`; for EXEC, `src/cmd/multi.inc:1538`, followed by the parse stop
at `src/core/io_loop.h:3997`. Resume checks are at `:5684`, `:6381` and `:6698`. They keep later
frames behind the refused frame and do not relax RYOW.

**Change and safety argument.** `src/core/atomic_admission.h:9` keeps `(window, active reservations)`
in one lock-free 64-bit atomic, with 32 bits for each. Admission reads it and makes **one strong
CAS** if there is room. Full or contended attempts take the existing backpressure path. Successful
admission's count increment and limit check are one atomic event. Zero still spells unlimited;
representational exhaustion refuses instead of overflowing. Retirement/rollback uses one
`fetch_sub`, irrespective of configuration state or which configuration admitted the group.

CONFIG has a single window writer: the shard-0 block in `src/cmd/t_server.cc:1404` calls
`set_atomic_window` at `:1484`. Single-owner execution and quiesced migration preserve that
serialization. CONFIG SET is routed to owners (`src/cmd/t_server.cc:3239`), configuration mutation
is forbidden in MULTI (`src/cmd/multi.inc:264`), and scripting rejects Admin/ConfigRoute commands
(`src/cmd/scripting.cc:383`). Enabling/disabling atomics no longer rebuilds the window. Since concurrent
admissions/returns can change only the low half, `set_window` at `src/core/atomic_admission.h:34`
loads the old high half and atomically adds the unsigned high-half delta. This changes the limit
while preserving the exact concurrent count, without retries or a transient unlimited limit.
**Its single-writer contract must be preserved if another setter is added.**

Shrinking below the current count keeps the admitted groups and derives debt as
`max(active - window, 0)` for nonzero windows. No new reservation succeeds until there is room.
Growth and unlimited transitions cannot forget active groups. `atomic_inflight` now reads this
one exact count (including reservations that may still roll back); it no longer sums changing IO
mirrors. `atomic_credit_pool` reports unused global capacity; there are no privately leased
credits to omit. `atomic_credit_debt` retains its meaning. Idle finite-window pool equals window.

`Server::atomic_try_admit` still revalidates the snapshot barrier and atomic-enabled bit and
balances `atomic_activity_` on rollback. Per-owner active counts still arm that word on first
admission and disarm it on final retirement. The old receipt fields named `admission_generation`
in EXEC/scatter state remain layout-compatible, carrying 1 for a successful admission; they are
not MVCC epochs. MVCC publication, cut registration, record retirement, single-owner writes and
same-connection fragment parking are unchanged. No deferred-credit queue or dropped return is
needed. The protocol allocates nothing, including when the optional atomic lane is off.

**Remaining bound distinction.** These functions now perform a bounded number of atomic
instructions, not a bounded wall-clock duration if the calling thread itself is descheduled.
They do not guarantee fair admission under continuous contention. A connection can remain
backpressured forever if admitted work never retires, and a refused EXEC still holds its later
GETs in program order. The finite atomic window remains a resource limit, not a key-conflict
test. This diff removes synchronous internal waits, not that resource policy or its consequences
for the paper's broad read-obstruction claim.

## Validation and layout

`tests/waits_unit.cc`, added to `make unit`, has deterministic stopped-writer/stopped-reader
publication checks and exact shrink/grow/unlimited credit conservation checks, plus threaded
coherence and admission exercises. Its alarm makes a waiting implementation fail instead of
hanging indefinitely. The deterministic checks actually stage an uncommitted publication; no
test skips when the window fails to open. The stress portions are supplemental, not proof that a
particular CAS collision occurred. **These tests have not been built or run.**

All existing layout locks remain. The only changed member type in a locked object is the former
64-byte `AtomicAdmissionLease`, replaced by an explicitly 64-byte `AtomicAdmissionState` in
ThreadCtx (`src/core/thread.h:67`). No other locked object gains a member. Server's old credit
and window storage is padded to preserve subsequent cache-line offsets; mailbox storage is
appended at the tail. The maintainer must still compile and check all locks: Op 336, Client 1984,
ThreadCtx 1408, Shard 1440, FlatStore 944, Rob<64> 192, AtomicEntry 144, Config 624.

No gate row is added or retired. `tests/gate.sh` is unchanged: quick remains **327**, full remains
**344** (without optional NIC). `make unit` has an additional program; it is not a new gate row.
The gate's explicit static rows precede its quick exit at `tests/gate.sh:1258`; this diff inserts
no invocation on either side of that line.

## MEASUREMENT PLAN

Instrumentation below is **proposed**, except for the explicitly identified existing counters.
Keep it off by default: optional per-thread diagnostic sidecars, no allocation when disabled,
no per-operation global counter or producer store on consumer-read hot lines. Report counts per
million eligible calls and per second, elapsed nanoseconds, p50/p95/p99/p99.9/max, and longest
still-open wait. Label mode, physical worker, command/read-vs-write, and caller site. Measure
wall time and on-CPU time separately; preemption is part of the observed wait, not evidence of
time spent executing the loop.

For an infinite or very long wait, an exit-only histogram is blind. Publish an atomic
`wait_started_ns` and reason on entry in the diagnostic sidecar, clear on exit, and have a separate
observer sample its age. Publish blocker details only on first observation/change, not every spin.
An in-server INFO command alone is insufficient if the owners needed to answer INFO are blocked.

| Mechanism | Existing evidence | Add to an instrumented baseline / candidate |
| --- | --- | --- |
| QSBR full ring | `qsbr_forced_graces`, `qsbr_forced_yields`, `qsbr_zero_progress_scans`, `qsbr_depth`, `qsbr_max_owner_depth`, `qsbr_reclaims` in `src/store/read_local_settax.h:92`; only compiled with `TOMO_READ_LOCAL_SET_TAX_VARIANT == 3` | `read_rehash_calls`, `read_rehash_table_retires`, `read_lazy_expire_retires`; `qsbr_full_wait_calls`, `qsbr_full_wait_ns_total/max/hist`, `qsbr_full_wait_started_ns`; ring depth, head stamp, current epoch, blocking tid/tick/parked flag and retired bytes. Separate lookup, write, atomic cleanup and maintenance callers. |
| Config snapshots | No baseline duration/retry counters at these loops | Per caller `live_snapshot_calls`, `limits_snapshot_calls`, `snapshot_odd_observations`, `snapshot_validation_failures`, `snapshot_retry_max`, `snapshot_wait_ns_total/max/hist`, `snapshot_wait_started_ns`; writer tid, setter kind, version and `config_update_started_ns`. Candidate: `config_mailbox_exchanges`, `config_previous_commit_returns`, copied version, and snapshot-call duration. |
| Credit generation and CAS | `atomic_window_stalls`, `atomic_inflight`, `atomic_credit_pool`, `atomic_credit_debt`; `atomic_window_stalls` is **not** a generation-wait count | Baseline `atomic_odd_precheck_refusals`, `atomic_generation_rollback_calls`, `atomic_release_odd_wait_calls/ns_total/max/hist/started_ns`, separated by rollback vs normal retire and EXEC vs script vs other; `atomic_borrow_cas_failures`, `atomic_carry_cas_failures`, `atomic_debt_cas_failures`; writer generation, `atomic_credit_ops`, and `atomic_reconfigure_wait_ns`. Candidate: `atomic_admit_cas_refusals`, full refusals separately, reservation/return durations and unfinished counts. |

The QSBR `qsbr_participant_loads` diagnostic currently adds `nthreads()` per scan even though
`read_local_grace_floor` can exit at its hint/first blocker. Instrument actual visited publications
if using that counter as a load count. Counts of yields, depth or output-limit disconnections are
not duration measurements. Do not label `atomic_window_stalls` as credit-generation spinning;
its baseline increment occurs on failed pool borrowing, and the candidate keeps it for full
windows, with CAS contention needing its own proposed counter.

Maintainer-run checks, on a quiet box:

1. Build the unit target and both production modes; run `make unit`, footprint checks, and the
   full gate. Reproduce gate rows with `--shards 16 --ratio $GATE_RATIO`, pinned to `GATE_CORES`
   (default `0-7`, split 6 IO + 2 EX), before interpreting them. Also exercise fused with
   read-local explicitly enabled, and live key/client balancing so physical-worker mailbox
   ownership and retire-sink migration are exercised. A normal/default boot is not this geometry.
2. For site 1, hold one **active** participant after it can have acquired a foreign pointer, fill
   the owner's ring, and position a resize so the next baseline lookup finishes it. Use live,
   non-TTL keys to isolate rehash from lazy expiry; record the actual ring-full and resize window
   before issuing GET, the no-touch lookup, and a tracked atomic scatter lookup. The baseline
   must show read-triggered retirement
   and an open wait; the candidate must execute those lookups without rehash retirement. Run a
   separate expiry arm to confirm the documented residual. Use fresh state and a bounded number
   of arming attempts; failure to arm is a failure, not a skipped pass. Do not expect owner
   maintenance blocked earlier in the same loop to let a queued lookup run: the server-free
   lookup harness must isolate the direct obligation, and the end-to-end arm measures that
   remaining scheduling obstruction separately.
3. For site 2, hold a writer after making the version odd and again after staging only some
   mailboxes. Test normal GET under the default limits, explicitly armed normal limits, and a
   subscribed client. Change unrelated maxmemory/slowlog/timeout fields as well as output limits.
   Both normal/soft-limit tuples must always match one whole commit. After a completed update,
   verify actual hard/soft-limit enforcement, timeout, keyspace notifications, tracking
   invalidations, slowlog, protocol limits and save arming, not just successful GET replies.
   Keep a consumer paused across many publications; use ASAN/TSAN on the server-free mailbox
   exercise to check that its owned slot is not reused.
4. For site 3, baseline hooks must separately hold the credit generation odd, hold an announced
   credit operation, and cross the post-admission generation revalidation. Require each intended
   mechanism counter to move. Exercise read-only EXEC and staged scripts at atomic=0 and 1,
   in both modes, with windows 0, 1, AUTO and repeated 31 -> 7 -> 19 -> 3 changes. For the candidate,
   pause the setter before/after its high-half RMW and require already-admitted returns to finish;
   bound new groups after inherited shrink debt drains. Check the existing conservation row in
   `tests/atomic_torn.py:899`, allocation-failure rollback, disconnect/teardown, snapshot barriers,
   and the full RYOW/torn-read/script batteries. End idle with active=debt=0 and pool=finite window.
   Also measure CAS refusal/backpressure age so removal of spinning cannot hide connection
   starvation. Holding a caller descheduled is not a test of its own execution-step bound.
5. Negative controls stay in throwaway builds. Restore lookup rehash and require the isolated
   lookup deadline/counter check to fail. Return a mailbox's next value before commit and require
   the partial-publication check to fail; let the writer reuse the retained front slot and require
   its canary check to fail. Omit a credit decrement, reset the active count on window change, or
   accept `active == window`; the unit conservation/refusal checks must fail. Do not disable QSBR
   safety or accept a missing arming event to obtain a green row.

PRE and POST must use matched offered load and identical command/connection/key placement,
pipeline depth, value sizes, limits and persistence settings. Use independent site deltas as
well as the combined candidate. Repeat ordinary GET/SET controls with no config traffic and
with resize churn; use read-only EXEC, staged scripts and atomic MSET workloads to expose the
cost of replacing batched leases with a shared per-group RMW. Cover 8-core optimum and larger
core counts (including 64 real cores), depths 1 and 128, and client/owner skew. Default pub/sub
arming stays present in the default control. Attribute reply-path behavior with the 25GbE
two-netns rig; loopback alone is insufficient for send-path conclusions.

| Arm | PRE rate / latency | POST rate / latency | cycles/op | instructions/op | IPC | Wait calls / durations / open ages | CAS refusals / queue age |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Default GET/SET, stable table/config, both modes | Not measured | Not measured | Pending | Pending | Pending | Pending | Pending |
| Armed fused resize: lookup and owner maintenance separately | Not measured | Not measured | Pending | Pending | Pending | Pending | Pending |
| Default/armed limits + related/unrelated CONFIG churn | Not measured | Not measured | Pending | Pending | Pending | Pending | Pending |
| Read-only EXEC / staged scripts / atomic groups + window churn | Not measured | Not measured | Pending | Pending | Pending | Pending | Pending |

Rate at matched load is the verdict; `cycles/op = instructions/op / IPC` explains it. This diff
does not claim that dropping leases, exchanging mailbox slots, or retaining rehash tables is
performance-neutral. Publish actual PRE/POST results before accepting it; the baseline's
344/344 result is not validation of this worktree. No competitor claim is made here.

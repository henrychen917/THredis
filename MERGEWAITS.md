# cx-waits integration and resize/retirement regressions

Merged `cx-waits` (`d2a7e7fb8`, including `c33228203`) into `cx-final` starting at
`e25e08f57`. The merge base is `78c3e5391`.

**The lookup fix survives. The new tests expose two outstanding correctness/liveness defects.**
Ordinary non-expiring lookups no longer perform rehash retirement. However, read-only resize
completion misses the stated three-second bound in split mode (read-local off and on) and fused
mode with read-local on. An expired-key lookup still waits indefinitely for retirement quiescence
when a non-parked participant pins a full queue. The new rows stay red for these defects. They
have no skip, xfail, or relaxed tolerance.

**Conflict resolutions**

- `Makefile`: retained the integrated reorder, storage, core-concurrency, atomic-survivor and
  networking/command targets. Added the incoming `waits-unit` prerequisite and invocation to
  `unit`, alongside `reorder-unit`. Added the separate `rehash-waits-unit` target for this task.
- `src/core/server.h`: retained `orthog.h`, the derived production admission window
  `min(16 * shards, 1024)`, the derived script-stage limit, current ownership-transfer work,
  notification bindings, client-work epochs and scheduler statistics. Took the incoming bounded
  atomic credit accounting and committed configuration mailboxes. Did **not** resurrect the
  deleted `Config::atomic_window` knob or its CONFIG setter. The incoming padding preserves the
  following offsets; no layout lock was changed.
- The automatically merged `adopt_shard_owner_state()` still called the removed zero-argument
  snapshot API. It now consumes a dedicated coordinator mailbox at index `nthreads()`, initialized
  and published with the worker mailboxes. Ownership transfers are serialized by the shape
  transition. Executor quiescence does not stop IO-side tracking/ACL publication, so reading the
  writer's mutable `live_config_committed_` would race. Consuming the destination worker's mailbox
  would instead give that mailbox two consumers. The extra fixed mailbox avoids both problems,
  keeps readers bounded, and applies configuration in the same ownership critical section as the
  retire sink and notification binding. The committed writer copy remains writer-only.
- The existing `CODEX-OUT.md` edit/transcript was not staged or edited by this task. Existing
  nested worktrees under `build/setregress` were preserved.

**Lookup audit**

`find()` at `src/store/flatstore.h:1208`, `find_without_touch()` at `:2290`, and
`atomic_find_tracked()` at `src/store/flatstore_atomic.inc:610` contain no rehash step.
Their wrappers/resolvers also do not advance rehash: `find_notify`, `find_no_touch`,
`find_resident`, `aof_physical`, `atomic_resolve`, `atomic_find_physical`,
`atomic_resolve_internal`, and the foreign read-local probe/capture paths. Both tables remain
searchable. The table walkers likewise contain no rehash-maintenance call.

The exhaustive executable call inventory after the merge is:

| File and line | Caller |
| --- | --- |
| `src/store/flatstore.h:1054` | `snapshot_prepare` |
| `src/store/flatstore.h:1576` | `active_expire` |
| `src/store/flatstore.h:1683` | `insert` |
| `src/store/flatstore.h:1738` | `erase` |
| `src/store/flatstore.h:2983` | `insert_read_local` |
| `src/store/flatstore.h:3012` | `erase_read_local` |
| `src/store/flatstore.h:3714` | `rehash_step` dispatch to the armed implementation |
| `src/store/flatstore_atomic.inc:414` | `atomic_prepare_capacity` |
| `src/store/flatstore_atomic.inc:789` | `atomic_prepare_capacity_read_local` |

These are mutation, capacity preparation, snapshot preparation or owner maintenance paths.
Removing lookup-triggered rehash does not make lazy expiry read-only: see the separate failure
below. In-place overwrite rules, table publication, ownership, QSBR reclamation and snapshot
table retention were not weakened.

**Read-only resize battery**

`tests/rehash_readonly.py HOST PORT` uses an existing DEBUG-enabled server. The gate gives it
four fresh boots: split/fused, each with read-local 0/1; atomic 1, overlap 0, flip-auto 0.
Every boot has 16 shards and eight physical workers; split uses exactly 6 IO + 2 EX. Tests ran
on server cores 8–15, client cores 16–23, and ports 8500–8503.

`DEBUG REHASH-STATE` is an observational subcommand behind the existing DEBUG permission gate.
Its existing ConfigRoute executes on shard 0's owner and returns:

```
[shard, resize_starts, current_capacity, old_capacity, cursor, old_live, keys]
```

The capacities, cursor and live counts come directly from the store; starts comes from the
store-bound `Shard::Stats::rehashes` counter. Nothing is sampled across owners, no counter is
fabricated by the client, no extra per-operation instrumentation is added, and the diagnostic
does not invoke a lookup or maintenance.

Arming uses fresh FLUSHDB state, pauses owner expiry with the existing DEBUG switch, then
inserts shard-0 keys past the **normal growth trigger**. Router queries choose keys using the
actual boot hash seed. Discovery and insertion have fixed failure bounds. The test must witness
growth from 4096 to 8192 slots, live keys in the old table, a positive resize-start delta and
unexamined old slots. It then reads keys with maintenance still paused and requires every
diagnostic field to remain unchanged. Missing arming, lookup-driven progress, or a mutating
observer fails here; there is no skip or timing lottery to re-roll.

Re-enabling maintenance is the last state-changing command before the bounded phase. During
that phase the client sends only GET and observational REHASH-STATE requests. **Within 3.000
seconds**, old capacity, old live count and cursor must all become zero. Starts, new capacity
and key count must stay fixed, intermediate progress must be monotonic, and reads must return
the exact values. Completion is followed by verification of every inserted value.

Final clean-build run (all arms started with 2880 keys, starts=3, current capacity=8192,
old capacity=4096, cursor=96, hence 4000 unexamined slots):

| Mode | Read-local | GETs during bounded phase | Seconds | Cursor at end | Old live, before → after | Remaining slots | Result |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| split 6:2 | 0 | 830336 | 3.000027 | 336 | 2802 → 2619 | 3760 | FAIL |
| split 6:2 | 1 | 907424 | 3.000046 | 576 | 2802 → 2499 | 3520 | FAIL |
| fused | 0 | 16000 | 0.058950 | 0 | 2807 → 0 | 0 | PASS |
| fused | 1 | 894720 | 3.000103 | 576 | 2794 → 2469 | 3520 | FAIL |

These are correctness-work counts and deadlines, not throughput measurements. The failing arms
are not failures caused by a few microseconds of deadline overshoot: their old tables still hold
2469–2619 keys. All four boots produced clean shutdown reports. The same three failing postures
and one passing posture reproduced in two earlier runs; the test bound was never increased.

The source explains the insufficient guarantee. `ExLoopT::sweep()` invokes
`active_expire_cycle()`, which visits `FlatStore::active_expire()` and moves eight old slots per
visit even without TTLs. The busy split path returns to work before its idle sweep; the fused
schedules likewise have paths that postpone sweeps while work continues. Removing the lookup
step leaves progress dependent on reaching those maintenance visits. `active_expire()` also does
not count a pending keyspace rehash as remaining work when there are no expiry-index tasks.
The measurements establish failure of the three-second contract, not proof that every such
resize remains stuck forever.

This needs a progress guarantee independent of mutation traffic and incidental idle gaps.
Merely restoring lookup maintenance would restore the original violation. Merely scheduling
more owner retirement can still block that owner and reads queued behind it on a full QSBR
ring. Any follow-up must preserve lifetime safety and keep reads free of retirement waits;
the table/object must remain safely retained if reclamation capacity is unavailable.

**Deterministic retirement row**

`tests/rehash_waits_unit.cc` uses the real `FlatStore`, `ReadLocalDeferredQueue`, grace scan and
eight real `ThreadCtx` publication words, without a running server. Each case has fresh child
process state. A non-parked participant remains at tick 1, the queue seals 4096 distinct entries,
the owner advances its own tick, and two drains must reclaim zero. The actual grace scan must
identify the stale participant as the blocker. A separate control attempts the 4097th enqueue;
it **must** reach the one-second deadline, proving this is a blocking retirement state.

For the non-expiring lookup cases, normal insertions trigger 64 → 128 growth. A deliberately
placed old key remains in the last block, and setup stops at cursor 56: the next eight-slot step
would retire the old table. Hit checks in both tables and a miss run with the full queue pinned.
Each lookup must return within one second with the cursor, live count, ring depth and stale
tick unchanged and zero reclaims. Tracked/resolve arms also install an actual MVCC record.

All seven entry checks passed: `find`, no-touch, notify, resident, tracked atomic, atomic resolve
and foreign probe. `lookups` selects these checks separately from the residual below.

The gate runs `retirement`, which **also asserts the law for an expired key**. That fixture
proves the elapsed key is still physically resident, has a tracked deadline, and has no resize.
Its `find()` must return logical absence without waiting for quiescence. It instead reaches the
one-second deadline, exits 42 in the child and makes the battery exit 1. This is an ordinary
failing assertion, not an expected failure. Only the deliberate enqueue control expects 42.

The remaining path is `find → live_or_expire → erase_in → erase_in_read_local →
retire_obj_read_local → ReadLocalDeferredQueue::defer → force_oldest_grace`. With the stale tick
unchanged, that final `while (ring_.full())` cannot terminate. No special server hook is needed
to construct this deterministically. This does not claim that queued reads behind blocked owner
maintenance are now bounded either.

**Negative controls and verification**

Two throwaway source copies under `build/mergewaits` are built from clean directories. Neither
break is present in the merged source:

1. Remove only `if (rehashing()) rehash_step();` from `FlatStore::active_expire()` in
   `build/mergewaits/no-owner-progress/src/store/flatstore.h`. Run the same socket battery.
   It must fail the completion assertion with the old table still present, including the
   fused/read-local-off arm that passes on the merged code.
2. Restore `if (rehashing() && !snapshot_active_) rehash_step();` at the start of `find()` in
   `build/mergewaits/lookup-rehash-restored/src/store/flatstore.h`. Run
   `build/rehash-waits-unit find` in that copy. The control must prove the queue blocks, and the
   otherwise-passing read must hit its one-second deadline. Restoring the corresponding removed
   call in `find_without_touch` or `atomic_find_tracked` similarly targets `no-touch` or `tracked`.

Both negative controls were executed after clean builds; each source copy differs from the
final merged `src/` in **only its one rehash change**. The progress-disabled binary failed all
four boots, each with cursor **96 → 96**, remaining slots **4000 → 4000**, unchanged old live
count, starts=3 and keys=2880 after three seconds of successful reads. All shut down cleanly.
The final tests also verify the requested mode/read-local setting from INFO before arming.

| Mechanism check | PRE / deliberately broken behavior | POST / merged behavior |
| --- | --- | --- |
| Non-expiring `find`, full pinned ring, last rehash block | Restored lookup step: child deadline exit 42; battery exit 1 | Lookup step absent: returns, cursor 56 unchanged, ring=4096, reclaims=0; lookup battery exit 0 |
| Owner progress, fused/read-local 0 | Owner step removed: 4000 slots remain at 3.000029 s; exit 1 | 4000 → 0 slots in 0.058950 s; exit 0 |
| Other three read-only postures | Owner step removed: 4000 slots remain in each | Still fail the completion bound, as detailed above |
| Expired-key read, full pinned ring | Independently reachable via lazy expiry | Still fails: child exit 42; retirement battery exit 1 |

The checked timer is `alarm(1)`. The control plus seven returning lookups took **1.014969 s**;
the control plus lookup checks plus expired read took **2.016522 s**. The restored-find control
plus blocked read took **2.016522 s**, exiting 1. An initial harness version used the invalid
`ualarm(1000000)` and was corrected before these results: its ten-second setup timer was not
accepted as evidence of a one-second bound.

Final validation used the following clean build, within an isolated directory whose `src`,
`tests`, `third_party`, `tools` and `Makefile` link to this worktree. Only that directory's own
`build/` is removed by clean; the existing `build/setregress` worktrees are untouched.

```sh
cd /home/user/Projects/cx-final/build/mergewaits/clean
taskset -c 8-15 make clean
taskset -c 8-15 make -j4 all build/waits-unit build/rehash-waits-unit \
    build/core-concurrency-unit build/atomic-survivors-unit build/netcmd-unit
```

The release and selected unit builds succeeded without compiler warnings. All layout locks
compiled unchanged: Op 336, Client 1984, ThreadCtx 1408, Shard 1440, FlatStore 944, Rob<64> 192,
AtomicEntry 144 and Config 624. The waits unit passed; all eight core-concurrency selectors
(`watch scheduler lifetime drain route snapshot config notify`) passed, including the
configuration-at-migration check using the coordinator mailbox. Atomic survivor `admission` and
`closure`, and netcmd `config`, passed. Unit processes used cores 24–31.

The individual torture battery passed on all four merged boots. The existing EXEC-visibility
battery passed with read-local 0 in both modes. Its read-local-1 failures were compared against
an independently clean build of **pre-merge `e25e08f57`**, with the byte-identical EXEC battery
and the same 16-shard/eight-worker mode geometry (ports 8510–8511):

| Existing EXEC battery on armed lane | Pre-merge | Merged |
| --- | --- | --- |
| split 6:2 | 3 failed checks | Same 3 failed checks |
| fused | 1 failed check | Same 1 failed check |

The split failures are deterministic MGET read-cut expectations under atomic 0 and 1, plus the
atomic-0 fanout-counter witness in the concurrent arm. The returned deterministic examples are
**all-new** values; the battery calls these `torn`, but they do not demonstrate a mixture of
values. Its concurrent arm reported zero mixed reads and `fanout_cuts=0`. Fused's sole failure
is the atomic-0 MGET fanout-counter witness (`windows_opened=4`, `torn=0`, `fanout_cuts=0`). These
are pre-existing failures, not a green validation claim or a reason to weaken/delete those
checks. Both pre-merge boots also passed torture and clean shutdown. No general full-gate or
performance verdict is inferred from these selected tests.

Reproduction artifacts in this worktree (ignored build files, not committed source):

- `build/mergewaits/build-final.log`: clean merged build.
- `build/mergewaits/runtime-verified.log`: final four resize arms and shutdown results.
- `build/mergewaits/runtime-negative.log`: all four no-owner-progress counterexamples.
- `build/mergewaits/units-final.log` and `final-rehash-waits-unit-{lookups,retirement}.log`:
  corrected timers, successful lookup checks and the failing expiry assertion.
- `build/mergewaits/negative-lookup.log`: restored-find failure with measured whole-process bound.
- `build/mergewaits/runtime-final.log`, `runtime-premerge.log`, and the corresponding
  `final-positive/` and `premerge-compare/` per-battery logs: existing-battery comparison.
- `build/mergewaits/run_runtime.py` and `run_premerge.py`: PID-owning launchers used for the
  individual runs. Servers used 8–15, clients 16–23; every started server was waited on.

SHA-256 of the final release binary `build/mergewaits/clean/build/tomokv`:
`767d8615da93e3de6c87c2fb972b961d2911378836f046b2c509a9d73d5d0b4b`.
SHA-256 of its `rehash-waits-unit`:
`7196e422beb1a33881fe4b17341883ac9a53ce7593dbb05a93c5c6b78b4857bd`.

**EXPECT arithmetic, counted by line**

The pre-merge constants are `EXPECT_QUICK=377`, `EXPECT_FULL=394`. They are unchanged in this
diff. The quick-tier exit is now at `tests/gate.sh:1370–1373`. Every added row is above it:

| Emitting line(s) | Rows in quick | Rows in full |
| --- | ---: | ---: |
| 493/494: incoming waits config/admission unit | 1 | 1 |
| 498/499: read-retirement unit, including lazy expiry | 1 | 1 |
| 517/519, inside the 2 modes × 2 read-local settings at 500/501 | 4 | 4 |
| Total added | 6 | 6 |

Build or boot failure is folded into its dependent row, so it does not add or drop a row.
There are no new rows after the quick exit. The maintainer must set **EXPECT_QUICK=383**
(`377 + 6`) and **EXPECT_FULL=400** (`394 + 6`, excluding the optional NIC row).
Those counts describe emitted checks, not a green result: the new failures must remain visible.

No gate (quick or full) and no benchmark was run. Builds and individual batteries only were
authorized and used. All runtime processes were stopped through their own recorded PIDs.

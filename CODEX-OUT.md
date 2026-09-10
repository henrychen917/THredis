Reading additional input from stdin...
OpenAI Codex v0.153.4
--------
workdir: /home/user/Projects/cx-inline
model: gpt-6-astra
provider: openai
approval: never
sandbox: danger-full-access
reasoning effort: ultra
reasoning summaries: none
session id: 01a08899-47e8-7210-8239-a62b6628d454
--------
user
# TomoKV — project context for codex

Prepend this to every codex prompt. It is the shared background; each prompt then states its own scope.
Written 2026-09-07. Baseline commit `78c3e5391` (`origin/cpp`), full gate 344/344.

## What it is

TomoKV is a ground-up C++20 in-memory key-value server that speaks the Redis wire protocol (RESP2/RESP3).
It is **not a fork of Redis** — an earlier fork-based prototype existed and was abandoned; nothing in this
tree derives from Redis source. It implements **243 registered commands**. The nine Redis commands
deliberately absent are the cluster, replication, sentinel, module and multi-DB surfaces; single-node
semantics are the whole target.

It is a research system. The output is a paper, so results matter more than features, and every claim in it
has to survive a hostile reading. Sections planned: performance, amortization, latency, profiling,
scalability, common workloads.

## Architecture

**Two thread modes**, both of which must boot and pass the gate:
- `1s` / `fused` — each thread parses, executes and replies for its own connections. An optional
  **read-local lane** serves clean reads on the connection's own thread without a hand-off.
- `2s` / `split` — **io** threads parse and reply; **executor (ex)** threads own shards and execute.
  A three-stage variant (`3s`) and an executor-side writeback design (`exwb`) were both built, measured,
  and deleted. Do not propose either.

**Sharding and ownership.** Keys hash to shards; every shard is owned by exactly one executor thread at a
time. Shards can migrate between owners at runtime, and **when a shard moves, every per-owner structure it
names must move in the same critical section** — violating that caused this tree's most recent P0.

**Ordering ring (`rob`).** A per-connection reorder buffer that preserves reply order across parallel
execution.

**Atomics.** Multi-key atomicity uses group-scoped epoch-MVCC. Cross-shard commands use scatter-gather.
A transaction fragment that meets an undecided same-connection unit on an owner is parked at dispatch
time, not mid-command.

**Flip.** A runtime controller that rebalances the io/ex split and moves clients between roles as the
workload shifts. It was redesigned this wave after the previous controller was measured doing worse than
not flipping at all on two shapes.

**Load balancing.** Key-affine and client-affine balancing (`--key-lb`, `--client-lb`), both default on.

Networking is io_uring; the allocator is jemalloc.

## Non-negotiable correctness laws

Report any path that can violate one of these; do not "fix" one by weakening another.
- Single-owner writes.
- Reads are never obstructed by writes: immutable replacement plus QSBR. **No reader retries and no
  seqlocks on the per-operation path.**
- In-place overwrite is banned while the read-local lane is armed.
- Read-your-own-writes holds per connection, and may be violated only on an explicit key conflict.
- Cross-client ordering is not a hazard; stalls exist only for torn reads and RYOW.
- Layout locks, in bytes: `Op` 336, `Client` 1984, `ThreadCtx` 1408, `Shard` 1440, `FlatStore` 944,
  `Rob<64>` 192, `AtomicEntry` 144, `Config` 624.
- Both thread modes boot. Main commands carry zero regression. One file per feature.

## The box

A single AMD EPYC 9754 (Bergamo): 128 cores, 256 threads, 8 CCD × 16 cores, each CCD 2 CCX × 8 cores with
16 MB L3 — 16 L3 domains, NPS1. Measured node optimum is 8 cores, one CCX. Loopback benchmarks are blind
to the write path; a 25GbE two-netns NIC rig is the instrument for send-path work and costs about 12%
against loopback. Box noise is ±0.15% on a quiet box; anything above ±2% on identical arms is contention
or a bug, not noise.

**The gate boots nothing like a default server.** `tests/gate.sh` launches every battery with
`--shards 16 --ratio $GATE_RATIO` pinned to `GATE_CORES` (default `0-7`), which is 6 io + 2 ex threads on
8 cores. Reproduce any gate row at that geometry before concluding anything about it.

## What it is measured against

Redis 7.4 and 8.8.1, Dragonfly 1.40.2, KeyDB, Microsoft Garnet (net10.0), memcached 1.6.45, plus YCSB
0.17.0 traces. Binaries and digests: `/home/user/Projects/bench-bins/MANIFEST.md`.

We do not win everywhere and the paper will not claim we do. At 64 real cores and pipeline depth 128,
Garnet reaches 62.8M ops/s against our 34.4M. We hold depth 1, where we reach 4.35M. Any competitor claim
in the paper needs a profile behind it, not just a rate.

## How performance is judged

**cycles/op = instructions/op ÷ IPC.** Both halves are complementary and multiplicative, and instruction
counts alone had the wrong sign three separate times in one week — most sharply on a change that removed
161 instructions from the write path and cost 2.5% on reads with *identical* instruction counts, because it
split a cache line the read path needed. Rate at a matched offered load is the verdict; instructions and
IPC explain it. Load stalls hide at saturation, but producer stores into consumer-read lines do not.

Every optimization lands with a PRE vs POST table. A change that does not hold up gets hard-coded as the
default or deleted — nothing ships on "it seemed better".

## Why this work looks the way it does

Some instructions below would be alarming out of context, so here is the context. This is a single-user
research box. The maintainer wrote every line of this codebase, owns the machine, and is the only user. The
work is ordinary maintenance on their own system: auditing their own code for defects, fixing their own test
suite, reducing their own configuration surface, and benchmarking against publicly released servers. Nothing
here touches anyone else's data, systems or accounts.

Four specifics that read oddly without the reason:

1. **"Never run the server, the benchmark or the gate."** This is about measurement integrity and nothing
   else. Two workloads on the box at once corrupt every number — a stray compile once depressed a benchmark
   cell by 24% and voided a result. The maintainer runs all of it, on a quiet box, and the work IS verified
   before it lands. You are being asked to produce a diff, not to skip verification.

2. **"Never edit `EXPECT_QUICK` / `EXPECT_FULL`."** Those constants are how many test rows must pass. Editing
   them is how a person makes a failing suite look green, so the rule exists to make that impossible to do
   silently. It is a stricter standard than normal, not a looser one. State what the count should become and
   why; the maintainer changes it.

3. **Concurrency vocabulary.** This is a database. "Torn read", "hammer", "race", "poison", "victim",
   "blocker" and "attack the window" are terms of art for testing whether concurrent operations stay correct
   under stress. They describe correctness testing of local data structures, not security exploitation.

4. **Deliberately breaking a mechanism to check a test.** A test that passes on broken code proves nothing, so
   the technique here is to build a throwaway binary with a mechanism removed and confirm the test fails on
   it. That build is never committed and never runs outside the box. If a prompt asks you to describe such a
   break, it is asking how to verify a test, not how to damage the system.

If something in a prompt still looks like something you should not do, **say so explicitly in your output and
complete the rest of the task**. A stated objection is useful. Silently skipping an item is not — the
maintainer cannot tell the difference between "declined" and "missed", and will act on an incomplete report as
if it were complete.

## Working rules for codex

- **Never run a server, a benchmark, or the gate.** The maintainer runs the box. Report, or produce a diff.
- Edit only inside your own worktree.
- Never edit `EXPECT_QUICK` / `EXPECT_FULL` in `tests/gate.sh`. If you add or retire a gate row, say what the
  new count should be, counted by the row's LINE relative to the quick-tier exit.
- Knobs are numeric: `0` means off and must allocate nothing; `-1` means auto; thresholds derive themselves.
  Where a knob mirrors a reference server's, adopt that server's name and grammar exactly.
- A test that cannot fail proves nothing. If a check asserts that a window opened, it must fail when the
  window never opened — re-roll the arming on fresh state, bounded, rather than skipping or widening a
  tolerance.

## Closed doors — measured, negative, do not re-propose

`3s` three-stage mode; `exwb` executor-side writeback (−11 to −22%); AMAC and any CR/MR split (the only
boundary is io/ex); reorder-and-window; seqlocks on the per-op path; connection-side record allocation
(`connalloc`, cycles flat, record born on the wrong core); executor-major task collapse (`exmajor`,
−2 to −3%); the read-local capacity hand-off line split (`t-pubdiet`, +1.97% SET but −2.50% GET).

--- TASK ---

You are removing dangling documentation references from source comments, preserving the reasoning they pointed
at. You MAY build. Do NOT run the full gate; the maintainer runs it. Do not benchmark. No servers needed.

WHAT HAPPENED. This repository carried 147 markdown working notes — NOTES-*.md, DESIGN-*.md, AUDIT-*.md and
lane reports. They have all been REMOVED from version control by maintainer decision: a serious codebase does
not ship its working notes, and code that points at them is messier for it. The files are archived outside the
repository and are available to you at /home/user/Projects/lane-notes-archive/.

The problem: 66 comments in src/ still cite those files, e.g. "see NOTES-MULTIRES.md §5(a)" or
"DESIGN-RINGDIET.md explains the bound". Those references now dangle. The maintainer's rule is that this kind
of reasoning either lives INLINE AS A COMMENT or does not exist at all.

FOR EACH REFERENCE, do one of two things and say which:
 1. INLINE IT. Open the cited note in the archive, find the SPECIFIC claim the comment is invoking — usually a
    section, an invariant, a rejected alternative, or a measurement — and write it into the comment in one or
    two sentences. Then delete the file reference. The test is whether a reader who has never seen the note now
    understands why the code is the way it is.
 2. DROP IT. If the reference adds nothing a reader needs — it points at a whole document rather than a claim,
    or the surrounding comment already says everything load-bearing — delete just the reference and leave the
    rest of the comment intact.

DO NOT bulk-delete the references without reading what they pointed at. Several of these notes contain the only
written record of WHY something is done a particular way — a rejected alternative, an ordering constraint, a
measurement that decided a constant. Losing that is the actual risk here, and it is why this is a lane rather
than a sed command.

MATCH THE HOUSE STYLE. This codebase's comments explain WHY, name the alternative that was rejected, and cite
the measurement that decided it. Write in that voice. Do not replace a substantive pointer with a vague
paraphrase, and do not pad a comment that was already complete.

CONSTRAINTS: comments only. Do not change a single executable statement. If inlining reveals that a comment is
factually wrong about the code, note it in your writeup rather than fixing the code here.

DELIVERABLE: the diff, plus INLINE.md listing every reference with its file:line, whether you inlined or
dropped it, and for the inlined ones the claim you carried across. Plus a list of any archived note whose
content you judged worth keeping somewhere but which did not fit in a comment — the maintainer will decide
what to do with those.
codex
I’ll inventory the references, read the cited archive passages, and edit comments in an isolated worktree. I’ll record each decision in `INLINE.md` and check that the diff changes no executable code.

exec
/bin/bash -lc "pwd && git status --short && git branch --show-current && rg --files -g AGENTS.md -g '"'!build*'"' -g '"'!third_party'"' /home/user/Projects 2>/dev/null | head -80" in /home/user/Projects/cx-inline
 succeeded in 0ms:
/home/user/Projects/cx-inline
 M CODEX-OUT.md
cx-inline
/home/user/Projects/valkey/AGENTS.md
/home/user/Projects/dragonfly/helio/AGENTS.md
/home/user/Projects/dragonfly/AGENTS.md

exec
/bin/bash -lc "ls -la /home/user/Projects/lane-notes-archive | head -25 && rg -n '(NOTES-|DESIGN-|AUDIT-|\\.md\\b)' src" in /home/user/Projects/cx-inline
 succeeded in 0ms:
total 5064
drwxrwxr-x   2 user user    4096 Sep 10 07:55 .
drwxrwxr-x 148 user user    4096 Sep 10 07:55 ..
-rw-rw-r--   1 user user  124324 Sep 10 07:55 AUDIT-ACL.md
-rw-rw-r--   1 user user   72655 Sep 10 07:55 AUDIT-AOF.md
-rw-rw-r--   1 user user   36183 Sep 10 07:54 AUDIT-ATOMICS.md
-rw-rw-r--   1 user user   50143 Sep 10 07:53 AUDIT-CLEANUP.md
-rw-rw-r--   1 user user    6358 Sep 10 07:52 AUDIT-DEMOTEPLAN.md
-rw-rw-r--   1 user user   15044 Sep 10 07:52 AUDIT-LOCALFUSE.md
-rw-rw-r--   1 user user   27574 Sep 10 07:52 AUDIT-NETWB.md
-rw-rw-r--   1 user user   15253 Sep 10 07:52 AUDIT-PUBLISH.md
-rw-rw-r--   1 user user  144706 Sep 10 07:55 AUDIT-SMALLS.md
-rw-rw-r--   1 user user   30022 Sep 10 07:52 AUDIT-STORAGE.md
-rw-rw-r--   1 user user   71036 Sep 10 07:55 AUDIT-STREAMS.md
-rw-rw-r--   1 user user   36077 Sep 10 07:54 AUDIT-TESTS.md
-rw-rw-r--   1 user user   80815 Sep 10 07:55 AUDIT-TLS.md
-rw-rw-r--   1 user user   24753 Sep 10 07:52 AUDIT-resp3push.md
-rw-rw-r--   1 user user  335822 Sep 10 07:52 CODEX-OUT.md
-rw-rw-r--   1 user user   50858 Sep 10 07:55 COMPLEXITY-AUDIT.md
-rw-rw-r--   1 user user   89529 Sep 10 07:52 DESIGN-BPLUS.md
-rw-rw-r--   1 user user    3186 Sep 10 07:54 DESIGN-DEMOTEGATE.md
-rw-rw-r--   1 user user   34654 Sep 10 07:54 DESIGN-GATEHYGIENE.md
-rw-rw-r--   1 user user   48897 Sep 10 07:53 DESIGN-KNOBS.md
-rw-rw-r--   1 user user   24862 Sep 10 07:54 DESIGN-NOTIFY.md
-rw-rw-r--   1 user user   16891 Sep 10 07:54 DESIGN-P0REPLY.md
src/core/thread.h:121:    ArmTransient,           // pre-arming writes still in flight (DESIGN-RINGDIET.md)
src/core/thread.h:154:    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
src/core/thread.h:163:    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
src/core/ex_loop.h:59:// Lane ADMISSION pressure window (P128.md). A LANE-FULL deferral arms this many fused rotations
src/core/ex_loop.h:141:        // already own. See P128.md section 8.
src/core/ex_loop.h:278:    // LANE ADMISSION (P128.md). The parser asks once per parse pass how many lane slots (pending
src/core/ex_loop.h:313:    // (P128.md 9.5). A hint must never be load-bearing, and machinery that has not fired must be
src/core/ex_loop.h:565:            // One rotation of the lane-admission pressure window has elapsed (P128.md).
src/core/ex_loop.h:2342:                // the slice that actually computed the answer -- documented in NOTES-SERVERTAIL.md.
src/core/ex_loop.h:2526:                // the acyclicity NOTES-MULTIRES.md §5(a) found missing when the same hold was
src/core/ex_loop.h:2951:            // wake decision are paid once per batch in flush_notify_batch() (DESIGN-NOTIFY.md
src/core/ex_loop.h:2995:    // own (DESIGN-NOTIFY.md §4.4). The wake is never memoised across flushes -- a cached "already
src/core/ex_loop.h:3120:    // Per-batch completion notification (DESIGN-NOTIFY.md §2). While a batch is open, the slot
src/core/ex_loop.h:3142:// per-batch notification record (DESIGN-NOTIFY.md §2): 5848 + 8 + 32 * sizeof(NotifyEntry).
src/core/io_loop.h:1482:        // NOTHING READ-LOCAL IS ALLOCATED HERE ANY MORE (DESIGN-RINGDIET.md). The RYOW write-ring
src/core/io_loop.h:3054:        // LANE ADMISSION (P128.md): how many lane slots (pending local reads) this connection may
src/core/io_loop.h:3355:                            // DEMOTION-PLAN GATE (DESIGN-DEMOTEGATE.md). prepare() returns true
src/core/io_loop.h:3522:                                    // LANE ADMISSION, fair share (P128.md): the lane is under
src/core/io_loop.h:3536:                                    // LANE ADMISSION, lane full (P128.md): DEFER, never demote.
src/core/io_loop.h:4379:    // The per-operation gate of the SAMPLED fingerprint (DESIGN-flipfp.md): one load of the
src/core/io_loop.h:4433:    // NOTES-BARRIER.md section 2 argues from the source that no production sequence produces one
src/core/shard.h:434:        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
src/core/signal.h:50:// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
src/core/flipctl.h:22:// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
src/core/flipctl.h:55:// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
src/core/server.h:12:// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
src/core/server.h:311:            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
src/core/server.h:1972:    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
src/core/server.h:1991:    // Both were observed (see DESIGN-P0REPLY.md).
src/core/server.h:2830:    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
src/core/server.h:2938:    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
src/core/server.h:2972:    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
src/core/config.h:294:    // (DESIGN-flipfp.md); its measured sampling policy is internal to flipctl.h. Off and 1s
src/net/rob.h:54:// COLD-PATH COUNTERS FOR ARM-ON-DEMAND (DESIGN-RINGDIET.md). Three events, none of them on a hot
src/net/rob.h:137:// (DESIGN-RINGDIET.md). Locked all the same, so that a later field cannot quietly add another size
src/net/rob.h:318:    // bytes of write ring to hold one id (DESIGN-RINGDIET.md).
src/net/rob.h:341:    // demoted). The parser's fair-share admission (P128.md) compares THIS, not in_flight(): a
src/net/rob.h:364:    // write of every batch paid the exact walk — AUDIT-MARKDIET.md section 4).
src/net/rob.h:474:        // ARM ON DEMAND, THE WRITE HALF (DESIGN-RINGDIET.md). Until a local read has armed this
src/net/rob.h:560:        // ARM ON DEMAND, THE READ HALF (DESIGN-RINGDIET.md). ONE predictable test, on a word this
src/net/rob.h:690:    // write of an ARMED connection -- never at accept (DESIGN-RINGDIET.md) -- and directly by the
src/net/rob.h:808:    // ARM-ON-DEMAND STATE (DESIGN-RINGDIET.md), one word on the producer line, zero in the state
src/net/conn.h:86:// No reachable interleaving overlapped two owners (see NOTES-BARRIER.md section 2: a blocking op is
src/cmd/multi.inc:907:    // safety argument (NOTES-MULTIRACE.md §5) is false on this build, and that a transaction could
src/cmd/multi.inc:919:    // NOTES-MULTIRES.md §5(a); the park is safe only because it happens before the fragment has
src/cmd/slowlog.h:8:// to per-op timing only after a batch overruns. See NOTES-SERVERTAIL.md for the attribution
src/cmd/slowlog.cc:22:// The trade buys the default-on hot path back. See NOTES-SERVERTAIL.md.
src/cmd/t_server.cc:377:    // not live-settable (redis allows CONFIG SET; see NOTES-CLIMON2.md).
src/cmd/t_server.cc:935:    // derived value, which is what production always runs. See P128.md section 8.
src/cmd/t_server.cc:950:    // any production sequence (NOTES-BARRIER.md section 2) -- which is why it must be injected
src/cmd/t_server.cc:2490:                // assertion behind NOTES-BARRIER.md's reachability verdict. barrier_releases_held
src/cmd/multi.h:67:// own child (NOTES-MULTIRES.md §5(a)).
src/cmd/multi.h:73:// and "the hold fired too late" cannot be confused for one another (NOTES-MULTIRACE.md §5).
src/cmd/climon.cc:608:    // DELIBERATE DIVERGENCE FROM REDIS, documented in NOTES-CLIMON2.md: redis postpones CLIENT
src/cmd/blocking.inc:1351:    // ordering defect of exactly the class t-storeorder fixed. NOTES-BARRIER.md has the audit.
src/cmd/server_tail.cc:176:// Why it is not implemented here, and what it needs (NOTES-SERVERTAIL.md carries the full design):
src/cmd/server_tail.cc:212:                  "ERR WAITAOF with numlocal > 0 is not implemented by tomokv: the local fsync wait needs the deferred-reply path, see NOTES-SERVERTAIL.md");
src/cmd/server_tail.cc:233:// explicit error is the honest answer; it is documented in NOTES-SERVERTAIL.md.
src/cmd/scripting.cc:240:// activation failed (see NOTES-SCRIPTATOMIC.md). Redis has never undone a script's partial effects,
src/store/read_local_settax.h:11:// unconditionally (FlatStore::read_local_cache_take/put, NOTES-RECYCLE.md), so there is no longer a
src/store/kv_block_cache.h:71:    //    to a few percent and the whole mechanism stops paying (see NOTES-RECYCLE.md).
src/store/flatstore_atomic.inc:244:    // NOTES-MULTIRES.md §5(a) hit from the other direction. Plain single-command pseudo-entries
src/store/flatstore_atomic.inc:1432:    // ---- armed-write block cache (src/store/kv_block_cache.h, NOTES-RECYCLE.md) ---------------
src/store/flatstore_atomic.inc:1785:            // already acknowledged -- reproduced at 246/1120 (NOTES-MULTIRES.md).
src/store/flatstore_atomic.inc:2174:            // already acknowledged -- reproduced at 246/1120 (NOTES-MULTIRES.md).
src/store/flatstore.h:594:    // are deliberately outside the maxmemory model and documented in NOTES-EVICT.md.

exec
/bin/bash -lc "ls /home/user/Projects/lane-notes-archive && rg -n -i '(notes|design|audit|lane|p128)[-_ a-z0-9]*\\.md|\\.md[[:space:]§)]' src | wc -l" in /home/user/Projects/cx-inline
 succeeded in 0ms:
AUDIT-ACL.md
AUDIT-AOF.md
AUDIT-ATOMICS.md
AUDIT-CLEANUP.md
AUDIT-DEMOTEPLAN.md
AUDIT-LOCALFUSE.md
AUDIT-NETWB.md
AUDIT-PUBLISH.md
AUDIT-SMALLS.md
AUDIT-STORAGE.md
AUDIT-STREAMS.md
AUDIT-TESTS.md
AUDIT-TLS.md
AUDIT-resp3push.md
CODEX-OUT.md
COMPLEXITY-AUDIT.md
DESIGN-BPLUS.md
DESIGN-DEMOTEGATE.md
DESIGN-GATEHYGIENE.md
DESIGN-KNOBS.md
DESIGN-NOTIFY.md
DESIGN-P0REPLY.md
DESIGN-RINGDIET.md
DESIGN-SNAPSHOT.md
DESIGN-TYPES.md
DESIGN-W-RLIDEAS.md
DESIGN-ZC.md
DESIGN-flipfp.md
DESIGN-w-atomclean.md
DESIGN-w-atomlat.md
DESIGN-w-fused.md
DESIGN-w-storettl.md
DESIGN.md
FIXES-ATOMICS.md
FIXES-NETCMD.md
FIXES-STORAGE.md
FIXES.md
FIXMERGE.md
GATEFIX.md
INSPIRATION.md
MERGE.md
MERGEINTEG.md
MERGEREST.md
MERGEWAITS.md
NETCMDFIX.md
NOTES-ACLSEL.md
NOTES-AOF.md
NOTES-AOFFRAME.md
NOTES-AOFSCRIPT.md
NOTES-ARITY.md
NOTES-ATOMFIX.md
NOTES-ATOMICS.md
NOTES-BARRIER.md
NOTES-BARRIERTEST.md
NOTES-BITMAP.md
NOTES-BLOCKING.md
NOTES-BLOCKMULTI.md
NOTES-BORROWREG.md
NOTES-CAPTURE-PREFETCH.md
NOTES-CGAPS.md
NOTES-CLIMON2.md
NOTES-CMDGAP.md
NOTES-CMDGAP2.md
NOTES-CMDMETA.md
NOTES-COMPAT.md
NOTES-COMPATBLOCK.md
NOTES-COMPATINTRO.md
NOTES-CONTARITY.md
NOTES-DIFFERGATE.md
NOTES-DOUBLES2.md
NOTES-DUMPRESTORE.md
NOTES-EDGEENC.md
NOTES-EDGEPROTO.md
NOTES-EDGETIME.md
NOTES-EPOLL.md
NOTES-EVICT.md
NOTES-EXECATOMIC.md
NOTES-EXECFIX.md
NOTES-EXECISO.md
NOTES-EXPWIDE.md
NOTES-GATEFIX.md
NOTES-HASH.md
NOTES-HEXPIRE.md
NOTES-HLL.md
NOTES-HYGIENE.md
NOTES-KNOBS.md
NOTES-KTLS.md
NOTES-LIMITS.md
NOTES-LIST.md
NOTES-LUA.md
NOTES-MDIET.md
NOTES-MIGRATE.md
NOTES-MONOFIX.md
NOTES-MULTI.md
NOTES-MULTILOWER.md
NOTES-MULTIRACE.md
NOTES-MULTIRES.md
NOTES-PERSIST-IO.md
NOTES-PREFETCH-MGET.md
NOTES-PSPERF2.md
NOTES-PUSHTEAR.md
NOTES-RECYCLE.md
NOTES-RESP2TRACK.md
NOTES-S6FIX.md
NOTES-SAVEFIX.md
NOTES-SCANCURSOR.md
NOTES-SCRIPTATOMIC.md
NOTES-SCRIPTSURF.md
NOTES-SERVERTAIL.md
NOTES-SET.md
NOTES-SNAPCUT.md
NOTES-SORT.md
NOTES-SORTXSHARD.md
NOTES-STOREORDER.md
NOTES-STREAMS.md
NOTES-STRING.md
NOTES-WATCHLIVE.md
NOTES-WAVED.md
NOTES-WIREDUMP.md
NOTES-XACCT.md
NOTES-XAUTOCLAIM.md
NOTES-XGROUPS.md
NOTES-XMOVE.md
NOTES-XPERF2.md
NOTES-XSCRIPT.md
NOTES-XSHARD.md
NOTES-ZCINFO.md
NOTES-ZGEO.md
NOTES-ZSET.md
NOTES-ZSETFIX.md
NOTES-pubsub.md
NOTES-t-puborder.md
ORTHOG.md
OVERLAP.md
P128.md
REORDER.md
RESIZEFIX.md
RL2S.md
SETREGRESS.md
SPEC-WAVEA.md
SURVIVING.md
VESTCUT.md
VESTIGIAL.md
WAITS.md
WINDOWFIX.md
flipctl-design.md
thread-mode.md
66

exec
/bin/bash -lc 'for p in /AGENTS.md /home/AGENTS.md /home/user/AGENTS.md /home/user/Projects/AGENTS.md /home/user/Projects/cx-inline/AGENTS.md; do if test -f "$p"; then cat "$p"; fi; done
 git worktree list && git log -1 --oneline && rg --files src | wc -l' in /home/user/Projects/cx-inline
 succeeded in 0ms:
/home/user/Projects/tomokv-cpp                                                           2560dd60b [master]
/home/user/Projects/base-2e794c3bc                                                       2e794c3bc (detached HEAD)
/home/user/Projects/base-b55ec                                                           b55ec084b (detached HEAD)
/home/user/Projects/base-c0d7f27fd                                                       c0d7f27fd (detached HEAD)
/home/user/Projects/base-mask                                                            b3d213071 (detached HEAD)
/home/user/Projects/base-premask                                                         d1a68acb5 (detached HEAD)
/home/user/Projects/cx-abba                                                              67f623716 [cx-abba]
/home/user/Projects/cx-amort                                                             c8e61f646 (detached HEAD)
/home/user/Projects/cx-arch                                                              78c3e5391 (detached HEAD)
/home/user/Projects/cx-arch-controlplane                                                 78c3e5391 (detached HEAD)
/home/user/Projects/cx-arch-dataplane                                                    78c3e5391 (detached HEAD)
/home/user/Projects/cx-aud-atomics                                                       78c3e5391 (detached HEAD)
/home/user/Projects/cx-aud-cmdsem                                                        78c3e5391 (detached HEAD)
/home/user/Projects/cx-aud-concurrency                                                   78c3e5391 (detached HEAD)
/home/user/Projects/cx-aud-deadconf                                                      78c3e5391 (detached HEAD)
/home/user/Projects/cx-aud-netproto                                                      78c3e5391 (detached HEAD)
/home/user/Projects/cx-aud-storage                                                       78c3e5391 (detached HEAD)
/home/user/Projects/cx-aud2-atomics                                                      c8e61f646 (detached HEAD)
/home/user/Projects/cx-aud2-deadconf                                                     c8e61f646 (detached HEAD)
/home/user/Projects/cx-cacheutil                                                         bebfde299 [cx-cacheutil]
/home/user/Projects/cx-cmdcover                                                          67f623716 [cx-cmdcover]
/home/user/Projects/cx-coadapt                                                           bebfde299 [cx-coadapt]
/home/user/Projects/cx-comments                                                          1ebca1ed5 [cx-comments]
/home/user/Projects/cx-cov                                                               c8e61f646 (detached HEAD)
/home/user/Projects/cx-delvar                                                            a145bb1e3 [cx-delvar]
/home/user/Projects/cx-differ                                                            c8e61f646 (detached HEAD)
/home/user/Projects/cx-divcheck                                                          c8e61f646 (detached HEAD)
/home/user/Projects/cx-docs                                                              8a53ddd6d [cx-docs]
/home/user/Projects/cx-final                                                             bebfde299 [cx-final]
/home/user/Projects/cx-final-netcmdfix                                                   298bc790c [codex/netcmdfix-20260909]
/home/user/Projects/cx-final/build/setregress/bad                                        8a7260059 (detached HEAD)
/home/user/Projects/cx-final/build/setregress/good                                       c8e61f646 (detached HEAD)
/home/user/Projects/cx-final/build/setregress/reported-bad                               2587f70e6 (detached HEAD)
/home/user/Projects/cx-fix-atomics                                                       b1c99c165 [cx-fix-atomics]
/home/user/Projects/cx-fix-concurrency                                                   4656510d9 [cx-fix-concurrency]
/home/user/Projects/cx-fix-netcmd                                                        e94b97100 [cx-fix-netcmd]
/home/user/Projects/cx-fix-storage                                                       184b1480b [cx-fix-storage]
/home/user/Projects/cx-gates                                                             67f623716 [cx-gates]
/home/user/Projects/cx-harness2                                                          a4ceb61c3 [cx-harness2]
/home/user/Projects/cx-inline                                                            bebfde299 [cx-inline]
/home/user/Projects/cx-integ                                                             a9a220a49 [cx-integ]
/home/user/Projects/cx-knobs                                                             78c3e5391 [cx-knobs]
/home/user/Projects/cx-merge                                                             c8e61f646 [cx-merge]
/home/user/Projects/cx-merge-docs-review                                                 a759f83e5 [codex/merge-docs-20260910]
/home/user/Projects/cx-mget                                                              78c3e5391 [cx-mget]
/home/user/Projects/cx-mgetaudit                                                         78c3e5391 (detached HEAD)
/home/user/Projects/cx-nowait                                                            78c3e5391 (detached HEAD)
/home/user/Projects/cx-nowaitb                                                           a5b5fd90d [cx-nowaitb]
/home/user/Projects/cx-overlap                                                           a108f2c49 [cx-overlap]
/home/user/Projects/cx-ovldesign                                                         bebfde299 [cx-ovldesign]
/home/user/Projects/cx-ovldesign-b                                                       bebfde299 [cx-ovldesign-b]
/home/user/Projects/cx-redisgap                                                          c8e61f646 [cx-redisgap]
/home/user/Projects/cx-reorderdesign                                                     bebfde299 [cx-reorderdesign]
/home/user/Projects/cx-reorderdesign-b                                                   bebfde299 [cx-reorderdesign-b]
/home/user/Projects/cx-rep-atomics                                                       a76764a9c [cx-rep-atomics]
/home/user/Projects/cx-rep-cmdsem                                                        2fc6b2ce6 [cx-rep-cmdsem]
/home/user/Projects/cx-rep-concurrency                                                   6087fc525 [cx-rep-concurrency]
/home/user/Projects/cx-rep-deadconf                                                      409d7ca65 [cx-rep-deadconf]
/home/user/Projects/cx-rep-netproto                                                      47f03649c [cx-rep-netproto]
/home/user/Projects/cx-rep-storage                                                       ef1a3ad12 [cx-rep-storage]
/home/user/Projects/cx-resizefix                                                         bf41dd331 [cx-resizefix]
/home/user/Projects/cx-rl2s                                                              f0412516c [cx-rl2s]
/home/user/Projects/cx-sweepfix                                                          59d70485f [cx-sweepfix]
/home/user/Projects/cx-timing                                                            78c3e5391 [cx-timing]
/home/user/Projects/cx-verifycrit                                                        c8e61f646 (detached HEAD)
/home/user/Projects/cx-vestcut                                                           daa937b00 [cx-vestcut]
/home/user/Projects/cx-vestigial                                                         c8e61f646 (detached HEAD)
/home/user/Projects/cx-waits                                                             d2a7e7fb8 [cx-waits]
/home/user/Projects/cx-windowfix                                                         c43f8e6a0 [cx-windowfix]
/home/user/Projects/tomokv-cpp-perthread                                                 c8e61f646 [t-merge14]
/home/user/Projects/tomokv-cpp-stable                                                    09a0a15d8 (detached HEAD)
/home/user/Projects/tomokv-cpp/.claude/worktrees/agent-a0f3b4e32637100fc                 9b5a4be82 [t-night-atomics]
/home/user/Projects/tomokv-cpp/.claude/worktrees/agent-a0fa624c3031db0ed                 6f247cd7c [t-fable-bplus-get]
/home/user/Projects/tomokv-cpp/.claude/worktrees/agent-a0fcb6e07d10d7233                 a5dd6cb30 [t-fused-int]
/home/user/Projects/tomokv-cpp/.claude/worktrees/agent-a33df6e88bc5df852                 c7a8a161b [t-fable-cmdlookup]
/home/user/Projects/tomokv-cpp/.claude/worktrees/agent-a3616422b03a9df0e                 ebbc352b4 [t-fable-notify]
/home/user/Projects/tomokv-cpp/.claude/worktrees/agent-a394fcb612edef274                 01c8ec887 [t-night-netwb]
/home/user/Projects/tomokv-cpp/.claude/worktrees/agent-a3eeb767b7f6e8fec                 33a17682a [t-fable-markdiet]
/home/user/Projects/tomokv-cpp/.claude/worktrees/agent-a5c7f0e80fb1cf495                 caa2555a8 [t-night-cleanup]
/home/user/Projects/tomokv-cpp/.claude/worktrees/agent-a7ad6a267060a863a                 3208b6fa9 [t-fable-publish]
/home/user/Projects/tomokv-cpp/.claude/worktrees/agent-a828042292840fc02                 16fd8bd74 [t-storettl-int]
/home/user/Projects/tomokv-cpp/.claude/worktrees/agent-a8c04953a16a2718d                 73f408baa [t-atomlat-fix]
/home/user/Projects/tomokv-cpp/.claude/worktrees/agent-a8d3b3c556562611d                 842a3ce73 [t-night-storage]
/home/user/Projects/tomokv-cpp/.claude/worktrees/agent-a909916152dc8012e                 8ab1a9dae [t-night-tests]
/home/user/Projects/tomokv-cpp/.claude/worktrees/agent-ad47e898153964311                 29d16c160 [t-fable-jemalloc-ccx]
/home/user/Projects/tomokv-cpp/.claude/worktrees/agent-ae01fcec62aad5212                 6d39117d5 [t-fix-replyoff]
/home/user/Projects/tomokv-cpp/.claude/worktrees/agent-af77d9ae355bbc5ec                 fcab80884 [t-fable-demoteplan]
/home/user/Projects/tomokv-cpp/.claude/worktrees/agent-afa2f47363cf34a66                 b9deb977b [t-fable-writepath]
/home/user/Projects/tomokv-cpp/.claude/worktrees/agent-afc599d6742f199b3                 cd3c3cb9f [t-fable-localfuse]
/home/user/Projects/wt-atomline                                                          d6ffeb08d [t-atomline]
/home/user/Projects/wt-benchdriver                                                       43deada55 [t-benchdriver]
/home/user/Projects/wt-bisect-fg                                                         bc1589175 (detached HEAD)
/home/user/Projects/wt-bisect-ring                                                       b0335c239 (detached HEAD)
/home/user/Projects/wt-bplus                                                             a72d0c037 [t-bplus]
/home/user/Projects/wt-connalloc                                                         c4d584866 [t-connalloc]
/home/user/Projects/wt-ctlfresh                                                          c6a34fedc (detached HEAD)
/home/user/Projects/wt-cyclemap                                                          671019c23 [t-cyclemap]
/home/user/Projects/wt-existsfix                                                         dd2ff2e45 [t-existsfix]
/home/user/Projects/wt-exmajor                                                           db629749e [t-exmajor]
/home/user/Projects/wt-flipctl                                                           99369aa75 [t-flipctl]
/home/user/Projects/wt-flipdamp                                                          d88c098a1 [t-flipdamp-r]
/home/user/Projects/wt-flipdamp-base                                                     e902c67d5 (detached HEAD)
/home/user/Projects/wt-flipfp-base                                                       fc3f6f51c (detached HEAD)
/home/user/Projects/wt-fused-int                                                         a5dd6cb30 (detached HEAD)
/home/user/Projects/wt-fuzz                                                              c592e5623 [t-fuzz]
/home/user/Projects/wt-genthread                                                         3eead33c4 [t-genthread]
/home/user/Projects/wt-handoff                                                           6968d3857 [t-handoff]
/home/user/Projects/wt-hotfwd                                                            a475855ff [t-hotfwd]
/home/user/Projects/wt-hotline                                                           f55dc80e5 [t-hotline]
/home/user/Projects/wt-i1                                                                febdcc92c [t-i1]
/home/user/Projects/wt-i1-m                                                              e5382e7c1 (detached HEAD)
/home/user/Projects/wt-ingress                                                           744cd57f5 [t-ingress]
/home/user/Projects/wt-keycmp                                                            81b19c0e1 [t-keycmp]
/home/user/Projects/wt-l6audit                                                           c592e5623 [t-l6audit]
/home/user/Projects/wt-markdiet                                                          c0b3c6b72 (detached HEAD)
/home/user/Projects/wt-memcpy                                                            15c3424d7 [t-memcpy]
/home/user/Projects/wt-notify                                                            ebbc352b4 (detached HEAD)
/home/user/Projects/wt-overlap3                                                          9bb64f51a [t-overlap3]
/home/user/Projects/wt-pad                                                               a680ff9bf (detached HEAD)
/home/user/Projects/wt-pad9                                                              e902c67d5 (detached HEAD)
/home/user/Projects/wt-pre11                                                             bd3b42e80 (detached HEAD)
/home/user/Projects/wt-pre13                                                             70bd2ee29 (detached HEAD)
/home/user/Projects/wt-proberound                                                        7de4c5971 [t-proberound]
/home/user/Projects/wt-pubdiet-m                                                         274b47586 (detached HEAD)
/home/user/Projects/wt-recycle                                                           6362d7139 [t-recycle]
/home/user/Projects/wt-replayer                                                          0b1fcce65 [t-replayer]
/home/user/Projects/wt-replycode                                                         a16776152 [t-replycode]
/home/user/Projects/wt-retirepri                                                         0e4a4c989 [t-retirepri]
/home/user/Projects/wt-robdiet                                                           666924351 [t-robdiet]
/home/user/Projects/wt-schema                                                            88f5419aa [t-schema]
/home/user/Projects/wt-spsc1s                                                            c151cead3 [t-spsc1s]
/home/user/Projects/wt-stablekey                                                         719c63e1a [t-stablekey]
/home/user/Projects/wt-storettl-int                                                      16fd8bd74 (detached HEAD)
/home/user/Projects/wt-train2b                                                           f40469ea3 [t-train2b]
/home/user/Projects/wt-train2c                                                           596d11aef [t-train2c]
/home/user/Projects/wt-train3                                                            04354d8a3 [t-train3]
/home/user/Projects/wt-train4                                                            6baec9e02 [t-train4]
/home/user/Projects/wt-train5                                                            94341386d [t-train5]
/home/user/Projects/wt-train6                                                            d7e244cec [t-train6]
/home/user/Projects/wt-train7                                                            c29cc7801 [t-train7]
/home/user/Projects/wt-train8                                                            a680ff9bf [t-train8]
/home/user/Projects/wt-train9                                                            e902c67d5 [t-train9]
/home/user/Projects/wt-w-atomlat                                                         eb116399c [t-w-atomlat]
/home/user/Projects/wt-w-fused                                                           58957061f [t-w-fused]
/home/user/Projects/wt-w-storetab                                                        35a3b0a69 [t-w-storetab]
/home/user/Projects/wt-w-storettl                                                        8bcf46892 [t-w-storettl]
/home/user/Projects/wt-writepath                                                         fddc14c27 (detached HEAD)
/tmp/claude-1000/-home-user-Projects/ee6eb242-5302-49cf-b767-1a2d8d8f0f61/scratchpad/nb  c308aa81f (detached HEAD)
/tmp/wt-rlmerge-net.DQZGn3                                                               6f8a6db7c (detached HEAD)
bebfde299 test: repair netcmd topology and configuration fixtures
130

exec
/bin/bash -lc 'git worktree add /home/user/Projects/cx-inline-comments -b codex/inline-archived-notes HEAD' in /home/user/Projects/cx-inline
 succeeded in 0ms:
Preparing worktree (new branch 'codex/inline-archived-notes')
HEAD is now at bebfde299 test: repair netcmd topology and configuration fixtures

exec
/bin/bash -lc "rg -n '"'^#{1,5} |W3|safe point|100|counter|bench|barrier|handoff|stale|246/1120|unreachable|exhaust'"' /home/user/Projects/lane-notes-archive/{DESIGN-RINGDIET.md,DESIGN-flipfp.md,NOTES-MIGRATE.md,DESIGN-P0REPLY.md,NOTES-MULTIRACE.md,NOTES-EXPWIDE.md,NOTES-BARRIER.md,P128.md}" in /home/user/Projects/cx-inline
 succeeded in 0ms:
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:1:# NOTES-BARRIER — the connection parse barrier is one bool with six owners
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:3:Lane `t-barrier`. Brief: `NOTES-STOREORDER.md` §9 handed on a found-but-not-fixed item —
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:4:`blocking_retire()` and `blocking_scatter_retire()` clear the connection's scatter barrier
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:5:unconditionally, with no test that the barrier is theirs. Verify it from the source, decide whether
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:11:**alone in its ROB**, so the barrier at its retirement can have no other owner. Because exactly one
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:12:owner can hold the barrier today, the mask is bit-identical to the bool on every reachable path;
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:16:**No build, no server, no bench was run in this lane** (main session holds the box). Everything
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:21:## 1. The set / clear table
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:23:Grep basis: `grep -rn scatter_barrier src/`, whole tree, plus every caller of the accessors.
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:29:### SET sites — 8 sites, 6 owner classes
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:33:| 1 | **Barriered scatter** (two-hop store / all-shards) | `src/core/io_loop.h:1530` (plain-scatter arm) | `scatter_dispatch.barrier && rob.in_flight() != 0` ⇒ `break` — **ROB empty required** (the `t-storeorder` fix, `io_loop.h:1478`) | `continue` → loop head `io_loop.h:1113` breaks |
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:38:| 6 | **Pub/sub transition** (`(P｜S)(UN)SUBSCRIBE`) | `src/core/pubsub.inc:850` (`pubsub_begin_pending`) | **none** | returns `Async` → `io_loop.h:1264` `if (scatter_barrier()) break` |
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:42:### CLEAR sites — 3
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:50:### Two corrections to §9
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:52:1. **`CLIENT PAUSE` does not set the barrier.** §9 names owner 6 as "CLIENT PAUSE/KILL". `climon.cc:524`
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:56:   (`io_loop.h:1896`) so the ring parks instead of spinning. It never touches `scatter_barrier_`.
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:57:   Attributing it to the barrier would have sent the hardening after the wrong owner.
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:62:   and `CLIENT KILL` can each set the barrier with older ops of the same connection still in flight.
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:63:   That is fine — but it means the invariant is *not* "the barrier is only ever set on an empty ROB",
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:68:## 2. Reachability verdict — LATENT, not live
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:79:* **Nothing younger.** The same dispatch sets the barrier (`io_loop.h:1437`) and `break`s; the parse
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:84:op *is* the blocking op. The barrier's only possible owner is the one being released. Clearing
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:97:### Interleavings I checked and rejected
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:101:| Pipelined `EXEC` / `SUBSCRIBE` / `CLIENT KILL` **then** `BLPOP` in one write | The first command sets the barrier and ends the pass; the `BLPOP` bytes are never parsed until the barrier drops at quiescence, by which time the first owner is done |
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:102:| Pipelined `BLPOP` **then** `EXEC` | `BLPOP` sets the barrier; `EXEC` is never parsed |
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:103:| Second owner set from **outside** `parse_and_dispatch` while a blocking op is parked | There is no such path. `blocking_resume_move` (the only thing that runs on a barred connection, `io_loop.h:1845`) does **not** acquire the barrier — it inherits the one already held. Pub/sub and climon event delivery on the io thread only ever set the barrier on the *requesting* client, never on a victim; `CLIENT KILL`/`UNBLOCK` reach a victim through `mark_closing()` / `blocking_request_unblock()`, neither of which touches `scatter_barrier_` |
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:104:| Two connections | The barrier is per-`Client` and every site writes it through the connection's own io owner. No cross-connection write exists |
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:108:### Two things this audit found that make the item worse than "latent"
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:113:   mentions the barrier's ownership. That is the exact shape of the defect `t-storeorder` fixed.
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:118:   literally it would **never** clear the barrier and every blocking client would wedge until the
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:125:## 3. `blocked_` has the same shape (found, not fixed)
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:127:`client.set_blocked(false)` sits one line above the barrier clear in both retire functions and is
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:130:argument. Not converted here: unlike the barrier, `blocked_` is a `connection_flags_` bit with three
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:137:## 4. The change, and why a mask rather than a count or a quiescence test
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:139:`bool scatter_barrier_` becomes `uint8_t barrier_owners_`, a bitmask over `BarrierOwner`
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:140:(`src/net/conn.h`). Six production bits, one spare (`1u << 6`), one test bit. `scatter_barrier()`
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:141:still answers `barrier_owners_ != 0`, so the parse-loop gate at `io_loop.h:1113` is the same byte
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:144:### Why not the owner COUNT §9 suggests
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:155:the barrier: the connection stops parsing **forever** and the client hangs. A count that
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:161:barrier mine". The brief's own requirement — *whoever sets it must be the one whose release drops
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:168:### Why not "clear only when the ROB is quiescent"
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:173:Spelled literally the barrier would never be cleared there and every blocking wake would wait for
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:178:cure. And it still answers "is the connection otherwise idle", not "is this barrier mine". Those
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:181:### Cost
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:188:* **Hot path.** `scatter_barrier()` was `movzx`/`test` on a byte and still is: `x != 0` on a
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:191:  barriered scatter and a `CLIENT` fan-out.
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:192:* **Backstop.** `barrier_release_quiesced()` is `and byte ptr, 0x80` where it was `mov byte ptr, 0`.
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:193:  One instruction either way, on an arm that only executes while a barrier is already set.
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:196:  server-wide load, inside an arm that already only runs when a barrier is up. Off ⇒ the load never
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:199:### What deliberately did NOT change
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:203:release in `deferred_wait_pass`, for instance, would drop the barrier **before** its op retires and
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:212:## 5. Validation — DESCRIBED, not written and not run
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:217:### The hardening is not observable without an injector, and that is the finding
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:233:### The check
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:245:    ---- assert: barrier_releases_held > 0 ----             <- the gate actually opened
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:246:    ---- assert: barrier_owner_overlaps > 0 ----            <- the counter can count
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:259:### Geometry — what it must find, and what it must never assume
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:263:| **Connections** | **Exactly one** connection carries both owners; a second drives it | The barrier is a field of `Client`. N connections give N independent barriers and **zero** overlap. A battery that spreads blocking load over 32 connections to "increase the chance" has a chance of exactly zero. This is the single most important line in this section |
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:265:| **Pipelining** | The probe must be in the **same `write()`** as the blocking command | It must already be at `rpos` when the barrier goes up. A second write also works but adds a race between the write and the barrier; one write removes it |
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:268:| **Keys** | `BLPOP` arm: any single key. **`BLMOVE` arm: `src` and `dst` on different owners** | `blocking_resume_move` builds the move through `xshard_prepare`, and `state->two_hop` — the property that makes it a *barriered* scatter — is only true cross-owner. A same-owner `BLMOVE` battery exercises a different path and proves nothing about this one |
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:276:`barrier_releases_held` unchanged across the armed arm.
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:278:### Distinguishing "held by the remaining owner" from "never set at all"
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:282:1. **Armed** (latch on). Probe must **not** answer; `barrier_releases_held` must increase by exactly
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:283:   the number of blocking retirements in the arm; `barrier_owner_overlaps` must increase by exactly
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:286:   blocking reply; both counters must stay put. *The probe can pass — so cell 1's silence is caused
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:291:   `climon.py`, the pub/sub battery and `storeorder.py`). `barrier_owner_overlaps` must read
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:293:   cell 1 showed the counter can be moved — a "must be zero" reading of a counter nothing has ever
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:297:`blocking_retire`'s release replaced by the pre-lane `barrier_owners_ = 0`. Cell 1 must then FAIL —
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:298:the probe answers immediately and `barrier_releases_held` stays 0. A hardening whose control build
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:301:### Cost check
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:310:## 6. Also found, not fixed
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:319:  setting the barrier lets the parse pass continue. `pubsub_start_publish` (`pubsub.inc:952`) is
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:325:  declaration order and still spell the field `bool scatter_barrier_`. Layout-identical (`uint8_t`
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:330:## 7. Files changed
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:334:| `src/net/conn.h` | `BarrierOwner` enum; `barrier_acquire` / `barrier_held_by` / `barrier_release` / `barrier_release_quiesced`; `bool scatter_barrier_` → `uint8_t barrier_owners_` (same byte, same offset) |
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:335:| `src/core/io_loop.h` | `barrier_arm()` — the one acquire door, counts overlap; four acquire sites converted; quiescence clear → `barrier_release_quiesced()`; `DEBUG BARRIER-HOLD` acquire + release |
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:336:| `src/cmd/blocking.inc` | both retire paths release **only** `BarrierOwner::Blocking` and count a release that did not drop the barrier |
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:337:| `src/cmd/multi.inc`, `src/core/pubsub.inc`, `src/cmd/climon.cc` | acquire through `barrier_arm` with their own owner bit |
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:338:| `src/core/server.h` | `debug_barrier_hold` latch; `barrier_owner_overlaps` and `barrier_releases_held` counters |
/home/user/Projects/lane-notes-archive/NOTES-BARRIER.md:339:| `src/cmd/t_server.cc` | `DEBUG BARRIER-HOLD 0\|1`; both counters in `INFO STATS` |
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:1:# NOTES-EXPWIDE — one expiry cut per logical operation
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:3:Lane `t-expwide`. Oracle: vanilla redis 7.4.2 at `/tmp/claude-1000/redis74/src/redis-server`.
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:10:## Verdict table
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:16:| W3 | (found while fixing W1) a cross-shard **script** pins a CLOCK_MONOTONIC value as "now", so it never sees any key as expired | **REPRODUCED — FIXED**, deterministic, 100% | `tests/expwide.py` S4 |
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:23:## What the reference does, measured rather than assumed
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:54:ORACLE B   MULTI / GET k / DEBUG SLEEP 0.20 / GET k / EXEC, deadline 100ms in -> [v, OK, v]
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:67:## The argument, stated once
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:81:## The defects
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:83:### W1. A cross-shard fan-out had a pinned commit cut and no pinned time cut
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:139:### W2. A key could expire in the middle of a transaction
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:156:### W3. A cross-shard script compared deadlines against the machine's uptime
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:159:`now_ns() / 1000000`, and `now_ns()` is **CLOCK_MONOTONIC**. That value — milliseconds since boot,
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:177:## The fix
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:186:| `src/cmd/scatter_engine.inc` | `ScatterState::now_cut_ms` replaces the script-only `script_now_ms`; stamped for **every** cross-shard group at prepare; installed in `xshard_execute`, which is the single entry point every fragment of every scatter kind passes through; the script workbench shard is pinned explicitly because it is a shard object the guard does not cover |
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:214:## Test evidence
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:216:### The battery: `tests/expwide.py`
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:234:  counter reports zero for TTL-free keys.
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:275:### The gate row
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:293:### The sibling lane's shelved reproducer, after
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:307:### ASAN
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:319:### Blast radius: batteries and differ, both atomic modes
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:345:### Blast radius: interleaved A/B
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:370:## Recorded, out of scope, pre-existing
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:389:## Housekeeping
/home/user/Projects/lane-notes-archive/P128.md:1:# P128 — why 32-core fused armed pure reads fall off a cliff at pipeline 128, and the fix
/home/user/Projects/lane-notes-archive/P128.md:22:- Box rules: make/bench ONLY when `scratchpad/quiet.done` exists and is >3 min old; compile pinned
/home/user/Projects/lane-notes-archive/P128.md:28:## 0. The anomaly (owner's numbers, 32 cores / 64 shards / 512 conns)
/home/user/Projects/lane-notes-archive/P128.md:38:## 1. Sweep 1 recovered (16 conns/thread = 128 conns, pre-prototype binary)
/home/user/Projects/lane-notes-archive/P128.md:47:| 128 | 6.86 | 2.38 | 6.91 | 2094 | 2655 | 0.79 | 2,616,542 | 97 | 610 (per-thread ~1000) |
/home/user/Projects/lane-notes-archive/P128.md:52:~1000 ops (~500 us) per thread at p96/p128. (Sweep 1 ran with a foreign 550%-CPU server on its
/home/user/Projects/lane-notes-archive/P128.md:55:### The 2x2 discriminator already run before the kill (same binary)
/home/user/Projects/lane-notes-archive/P128.md:84:## 2. Mechanism (from the code, `src/core/io_loop.h` + `src/core/ex_loop.h`)
/home/user/Projects/lane-notes-archive/P128.md:97:   rotations grow to ~1000 ops, every connection sits at its ROB cap, and the lane is chronically
/home/user/Projects/lane-notes-archive/P128.md:102:### Hypotheses A-D against this
/home/user/Projects/lane-notes-archive/P128.md:110:  independent of pipeline depth; keyspace hit rate stays 100%. Not it.
/home/user/Projects/lane-notes-archive/P128.md:115:### Rotation order (from `io_loop.h` run loop, `flush_ready`, `ex_loop.h fused_pass_impl`)
/home/user/Projects/lane-notes-archive/P128.md:128:## 3. Per-connection armed-state footprint (from the layout locks and `rob.h`)
/home/user/Projects/lane-notes-archive/P128.md:147:## 4. SHIPPED: lane admission -- defer, never demote; pressure-gated fair share; no knob
/home/user/Projects/lane-notes-archive/P128.md:150:per-rotation decay), `src/core/thread.h` (counters), `src/cmd/t_server.cc` (INFO). Prototype knob
/home/user/Projects/lane-notes-archive/P128.md:159:   owner as an ordinary task, `ReadLocalFallbackReason::LaneFull`) is unreachable from the parser;
/home/user/Projects/lane-notes-archive/P128.md:185:  rotation (from ~1000) because connections were throttled for unflushed replies. Final: the share
/home/user/Projects/lane-notes-archive/P128.md:205:## 6. Design item: widening `Rob<64>` would not help here (do not)
/home/user/Projects/lane-notes-archive/P128.md:220:## 5. Results with the prototype (chain4; geometry note)
/home/user/Projects/lane-notes-archive/P128.md:232:### 5.1 Depth A/B, 16 conns/thread (80 conns, 5 threads) — pass ab2
/home/user/Projects/lane-notes-archive/P128.md:242:| 1 | 32 | 5.50 | 3.55 | 2356 | 2118 | 1.11 | 18.6 | 5.4 | 2.2 | 0 | 0 | 86 | 100 | 11-22 | 10/10 |
/home/user/Projects/lane-notes-archive/P128.md:250:| 0 | 96 | 5.56 | 6.56 | 2100 | 2126 | 0.99 | 22.0 | 8.0 | 2.3 | **427,568** | 0 | 91 | 319 | 11-22 | 10/10 |
/home/user/Projects/lane-notes-archive/P128.md:260:counters and the tails:
/home/user/Projects/lane-notes-archive/P128.md:279:### 5.1b The CLEAN cells (chain6 binary = quota form of arm 2; 5 threads on 40-44, memtier on
/home/user/Projects/lane-notes-archive/P128.md:282:1.9-3.1 -- so only memtier totals, tails, counters (cumulative deltas per cell) and LBSIGNALS are
/home/user/Projects/lane-notes-archive/P128.md:325:## 5.2 The real rig (coordinator's quiet-box decomposition, 32c, 1s+RL, pure GET, 2 replicates)
/home/user/Projects/lane-notes-archive/P128.md:336:| 512 conns p32 (8/thread x 32 = 256) | 52.3 | 1893 | 1.19 | 2241 | 3.6 | 3.3 | 100 |
/home/user/Projects/lane-notes-archive/P128.md:337:| 1024 conns p32 (16/thread x 32 = 512) | 49.3 | 2000 | — | — | — | — | 100 (no tipping; +5.6% cyc/op = same-CCX footprint) |
/home/user/Projects/lane-notes-archive/P128.md:345:source (local % returns to 100 by construction: a deferred read is never an owner task).
/home/user/Projects/lane-notes-archive/P128.md:347:### PRE/POST table for the cells the owner sees (POST = to be measured on the real rig)
/home/user/Projects/lane-notes-archive/P128.md:351:| 512 conns p32 | 52.3M, 1893 cyc/op, local 100% | identical by construction (lane never full: the knob's branch is unreachable) | within noise (±0.5%); `lane_deferrals` = 0 |
/home/user/Projects/lane-notes-archive/P128.md:352:| 2048 conns p32 | 25.7M, 3850 cyc/op, local 56% | local 100%; cyc/op <= ~2500 (3850 - ~1350 demotion share; the ~600 footprint term stays) => ~38-40M | local >= 99%; `lane_full` -> 0; cyc/op <= 2700; p99 not worse than PRE |
/home/user/Projects/lane-notes-archive/P128.md:353:| 512 conns p128 | 43.4M, local 93% | local 100%; recovers the demotion share of the 52.3 -> 43.4 loss (~215 of ~390 cyc/op) => ~47-48M; the rest is the E1 depth cost | local >= 99%; >= 46M |
/home/user/Projects/lane-notes-archive/P128.md:360:## 7. Why pure deferral is not enough: the rotation order starves the deferred set
/home/user/Projects/lane-notes-archive/P128.md:371:1000 <= 1024, no connection can take more than its share, the lane never fills, every connection
/home/user/Projects/lane-notes-archive/P128.md:377:### 7.1 Starvation check -- RESULT (s3i, section 9.3) and why it is structural
/home/user/Projects/lane-notes-archive/P128.md:416:The one residual risk the bench cannot rule out is a connection whose share is 1 (more than 1024
/home/user/Projects/lane-notes-archive/P128.md:421:## 8. Directed test: `tests/read_local_lane.py` (gate row "read-local lane admission battery")
/home/user/Projects/lane-notes-archive/P128.md:423:### 8.1 Why the row drives a CAP and not load
/home/user/Projects/lane-notes-archive/P128.md:453:### 8.2 What the row checks
/home/user/Projects/lane-notes-archive/P128.md:472:`fallback_lane_full` 0, 6144 of 6144 shared GETs local (100.00%), slowest round 0.002 s. The same
/home/user/Projects/lane-notes-archive/P128.md:476:## 9. New-core harness and directional results (cores 32-39 / 160-167, ports 8200-8209)
/home/user/Projects/lane-notes-archive/P128.md:487:### 9.1 Rig truth: the 8-thread layout is loadgen-bound; 4 threads + 12 memtier threads saturate
/home/user/Projects/lane-notes-archive/P128.md:493:pairs), `--shards 4`, memtier 3 x 4 threads on 34-39+162-167; busy 99.9%, ~1000 ops/rotation.
/home/user/Projects/lane-notes-archive/P128.md:497:added after s3d, so s3b-s3d rows carry none, and only same-rep adjacent pairs and counters are
/home/user/Projects/lane-notes-archive/P128.md:502:### 9.2 Variant A (window re-armed by its own quota deferrals; `in_flight()` share) -- superseded
/home/user/Projects/lane-notes-archive/P128.md:506:| 33/thr p32 (boundary) rep1 | PRE | 6.35 | 0.659 | 1.03 | 1.25 | 1.53 | 2242 | 1946 | 1.15 | 100 | 98 (20 s) | - | 1006 | 27-40 |
/home/user/Projects/lane-notes-archive/P128.md:507:| | POST-A | 6.58 | 0.636 | 1.43 | 1.62 | 1.71 | 2271 | 1915 | 1.19 | 100 | 0 | 47 / 1.07M | 994 | 28-45 |
/home/user/Projects/lane-notes-archive/P128.md:508:| 33/thr p32 rep2 (order POST,PRE) | POST-A | 7.09 | 0.590 | 1.31 | 1.49 | 1.60 | 2295 | 1735 | 1.32 | 100 | 0 | 84 / 1.81M | 966 | 27-43 |
/home/user/Projects/lane-notes-archive/P128.md:509:| | PRE | 6.93 | 0.603 | 0.91 | 1.11 | 1.48 | 2244 | 1786 | 1.26 | 100 | 3,745 | - | 988 | 25-43 |
/home/user/Projects/lane-notes-archive/P128.md:511:| | POST-A | 4.44 | 1.818 | 3.64 | 3.88 | 6.06 | 3041 | 2754 | 1.10 | 100 | 0 | 441 / 24.4M | 542 | 47-79 |
/home/user/Projects/lane-notes-archive/P128.md:512:| 63/thr p32 rep2 (order POST,PRE) | POST-A | 5.55 | 1.447 | 2.83 | 3.04 | 3.89 | 2806 | 2252 | 1.25 | 100 | 0 | 79 / 18.2M | 630 | 58-74 |
/home/user/Projects/lane-notes-archive/P128.md:513:| | **PRE** | **4.60** | 1.746 | 2.77 | 3.65 | 4.18 | **3100** | 2682 | 1.16 | **56.8** | 19.9M (10 s window) | - | 550 | 56-76 |
/home/user/Projects/lane-notes-archive/P128.md:514:| 9/thr p32 rep1 | PRE | 7.48 | 0.150 | 0.26 | 0.29 | 0.38 | 2274 | 1649 | 1.38 | 100 | 0 | - | 205 | 4-13 |
/home/user/Projects/lane-notes-archive/P128.md:515:| | POST-A | 7.55 | 0.147 | 0.22 | 0.26 | 0.31 | 2279 | 1639 | 1.39 | 100 | 0 | 0 / 0 | 268 | 6-12 |
/home/user/Projects/lane-notes-archive/P128.md:516:| 9/thr p32 rep2 (order POST,PRE) | POST-A | 7.05 | 0.157 | 0.23 | 0.26 | 0.33 | 2280 | 1762 | 1.29 | 100 | 0 | 0 / 0 | 288 | 8-10 |
/home/user/Projects/lane-notes-archive/P128.md:517:| | PRE | 6.73 | 0.165 | 0.27 | 0.30 | 0.35 | 2264 | 1841 | 1.23 | 100 | 0 | - | 281 | 7-12 |
/home/user/Projects/lane-notes-archive/P128.md:518:| 9/thr p128 rep1 (order POST,PRE; owner measuring) | POST-A | 8.09 | 0.560 | 1.10 | 1.27 | 1.49 | 2046 | 1530 | 1.34 | 100 | 0 | 0 / 0 | 884 | 5-14 |
/home/user/Projects/lane-notes-archive/P128.md:519:| | PRE | 8.69 | 0.521 | 0.96 | 1.14 | 1.27 | 2032 | 1427 | 1.42 | 100 | 0 | - | 977 | 8-11 |
/home/user/Projects/lane-notes-archive/P128.md:520:| 9/thr p128 rep2 (POST before, PRE after the owner's run) | POST-A | 7.32 | 0.620 | 1.38 | 1.57 | 1.84 | 2047 | 1694 | 1.21 | 100 | 0 | 0 / 0 | 857 | 6-14 |
/home/user/Projects/lane-notes-archive/P128.md:521:| | PRE | 8.69 | 0.530 | 1.20 | 1.36 | 1.62 | 2027 | 1423 | 1.42 | 100 | 0 | - | 770 | 4-16 |
/home/user/Projects/lane-notes-archive/P128.md:523:| | POST-A | 7.44 | 0.611 | 1.32 | 1.47 | 1.59 | 2135 | 1665 | 1.28 | 100 | 0 | 119 / 1.86M | 931 | 9-23 |
/home/user/Projects/lane-notes-archive/P128.md:524:| **18/thr p128** | POST-A | **7.81** | 1.175 | 2.50 | 2.76 | 2.87 | 2088 | 1577 | 1.32 | 100 | 0 | 32 / 2.66M | 1310 | 11-25 |
/home/user/Projects/lane-notes-archive/P128.md:529:2647-3100 (+18..38% over the boundary's 2244), IPC 1.03-1.16 -- the owner's 52.3M -> 25.7M with 44%
/home/user/Projects/lane-notes-archive/P128.md:531:PRE 6.56M with 5.9% demoted vs POST 7.81M (+19%), p99 3.09 -> 2.50 ms. (b) Both arms are 100%
/home/user/Projects/lane-notes-archive/P128.md:532:local whenever demand fits the lane, and POST is 100% local everywhere by construction. (c) Variant
/home/user/Projects/lane-notes-archive/P128.md:539:server sees ~1000 of 1280 -- the owner's 2048-connection cell tips because 64 threads feed each
/home/user/Projects/lane-notes-archive/P128.md:542:### 9.3 FINAL form (lane-full-only arming, pending-slot share): 3-arm pre / post / no-quota
/home/user/Projects/lane-notes-archive/P128.md:554:#### s3i -- 63 conns/thread x p32 = 2016 nominal vs a 1024-entry lane (2x the boundary)
/home/user/Projects/lane-notes-archive/P128.md:559:| **POST** | 1 | **6.233** | 1.286 | 1.236 | 1.967 | 2.58 | 4.04 | **2259** | **1922** | 1.18 | 25.18 | 8.30 | 2.32 | **100** | **0** | 748 / 13,456 | 1010 | 60-67 | 10-16 |
/home/user/Projects/lane-notes-archive/P128.md:560:| nq | 1 | 6.101 | 1.314 | 1.316 | 1.815 | 2.73 | 4.33 | 2255 | 1997 | 1.13 | 26.14 | 8.36 | 2.33 | 100 | 0 | 1,074 / 0 | 1012 | 59-69 | 11-18 |
/home/user/Projects/lane-notes-archive/P128.md:562:| **POST** | 2 | **5.931** | 1.353 | 1.359 | 2.138 | 2.77 | 3.84 | **2261** | **2113** | 1.07 | 26.77 | 8.78 | 2.70 | **100** | **0** | 603 / 13,718 | 1012 | 46-74 | 14-24 |
/home/user/Projects/lane-notes-archive/P128.md:563:| nq | 2 | 5.868 | 1.367 | 1.367 | 2.175 | 2.70 | 3.73 | 2256 | 2013 | 1.12 | 24.57 | 8.36 | 2.68 | 100 | 0 | 1,109 / 0 | 1010 | 56-70 | 15-25 |
/home/user/Projects/lane-notes-archive/P128.md:567:57.8% -> **100%**, ops per rotation 532 -> 1011 (+90%), and every tail lower (section 7.1). The
/home/user/Projects/lane-notes-archive/P128.md:572:Two sanity gates on the table above, both passed. (a) The demotion rate computed from the counter
/home/user/Projects/lane-notes-archive/P128.md:574:from the independent hit counter (100 - local% = 42.63% and 41.85%), so the two instruments are
/home/user/Projects/lane-notes-archive/P128.md:588:#### s3j -- 33 conns/thread x p32 = 1056 vs 1024 (marginally over the boundary): NOT RESOLVABLE
/home/user/Projects/lane-notes-archive/P128.md:592:| PRE | 1 | 7.378 | 0.855 | 1.03 | 1.43 | 2243 | 1672 | 1.34 | 1.35 | 100 | 33 | - | 30-38 | 13-18 |
/home/user/Projects/lane-notes-archive/P128.md:593:| POST | 1 | 6.323 | 1.042 | 1.25 | 1.53 | 2248 | 1956 | 1.15 | 2.34 | 100 | 0 | 16 / 783 | 27-38 | 6-11 |
/home/user/Projects/lane-notes-archive/P128.md:594:| nq | 1 | 7.023 | 1.002 | 1.19 | 1.52 | 2244 | 1734 | 1.29 | 1.55 | 100 | 0 | 8 / 0 | 25-39 | 8-15 |
/home/user/Projects/lane-notes-archive/P128.md:595:| PRE | 2 | 5.799 | 1.247 | 1.51 | 1.87 | 2248 | 2149 | 1.05 | 2.34 | 100 | 1,757 | - | 24-42 | 12-17 |
/home/user/Projects/lane-notes-archive/P128.md:596:| POST | 2 | 6.418 | 1.055 | 1.27 | 1.67 | 2249 | 1977 | 1.14 | 1.84 | 100 | 0 | 5 / 488 | 30-39 | 16-29 |
/home/user/Projects/lane-notes-archive/P128.md:597:| nq | 2 | 6.741 | 0.994 | 1.19 | 1.59 | 2244 | 1796 | 1.25 | 1.51 | 100 | 0 | 10 / 0 | 29-37 | 11-30 |
/home/user/Projects/lane-notes-archive/P128.md:609:#### Battery results on the final binary (batt3, cores 32-35+160-163, port 8204)
/home/user/Projects/lane-notes-archive/P128.md:615:shared GETs local (100%). Plus s6 (215 comparisons), ryow, atomic_hazards (0 failures),
/home/user/Projects/lane-notes-archive/P128.md:620:- `1s armed atomic 0` read_local_lane FAIL: the two counter checks read delta=0. NOT a code
/home/user/Projects/lane-notes-archive/P128.md:623:  the battery's SERIAL writer could fill it, so the lane never went full and both counters are
/home/user/Projects/lane-notes-archive/P128.md:624:  vacuously zero (`fallback_lane_full` is 0 too, and 100% of reads were local). The armed boot
/home/user/Projects/lane-notes-archive/P128.md:635:### 9.4 The MUST-NOT-REGRESS cells (s3k): 9 conns/thread, final binary, 3 arms
/home/user/Projects/lane-notes-archive/P128.md:642:| PRE | 32 | 1 | 7.040 | 0.244 | 0.28 | 0.41 | **2263** | 1752 | 1.29 | 1.43 | 100 | 0 | - | 277 | 7-12 | 0/9 | 16-29 |
/home/user/Projects/lane-notes-archive/P128.md:643:| POST | 32 | 1 | 6.663 | 0.252 | 0.29 | 0.39 | **2269** | 1861 | 1.22 | 1.82 | 100 | 0 | **0 / 0** | 276 | 6-11 | 0/9 | 7-11 |
/home/user/Projects/lane-notes-archive/P128.md:644:| nq | 32 | 1 | 6.965 | 0.300 | 0.34 | 0.38 | 2268 | 1767 | 1.28 | 1.57 | 100 | 0 | 0 / 0 | 246 | 6-14 | 0/9 | 6-16 |
/home/user/Projects/lane-notes-archive/P128.md:645:| PRE | 32 | 2 | 7.133 | 0.234 | 0.27 | 0.32 | **2263** | 1744 | 1.30 | 1.45 | 100 | 0 | - | 242 | 5-11 | 0/9 | 6-22 |
/home/user/Projects/lane-notes-archive/P128.md:646:| POST | 32 | 2 | 6.714 | 0.279 | 0.32 | 0.40 | **2270** | 1874 | 1.21 | 1.81 | 100 | 0 | **0 / 0** | 261 | 6-12 | 0/9 | 9-17 |
/home/user/Projects/lane-notes-archive/P128.md:647:| nq | 32 | 2 | 6.600 | 0.276 | 0.32 | 0.43 | 2271 | 1866 | 1.22 | 1.80 | 100 | 0 | 0 / 0 | 227 | 4-13 | 0/9 | 12-15 |
/home/user/Projects/lane-notes-archive/P128.md:648:| PRE | 128 | 1 | 8.536 | 0.959 | 1.11 | 1.20 | **2027** | 1432 | 1.42 | 1.01 | 100 | 0 | - | 956 | 7-11 | 0/9 | 6-13 |
/home/user/Projects/lane-notes-archive/P128.md:649:| POST | 128 | 1 | 7.870 | 1.066 | 1.24 | 1.42 | **2033** | 1580 | 1.29 | 1.47 | 100 | 0 | **0 / 0** | 926 | 7-12 | 0/9 | 7-9 |
/home/user/Projects/lane-notes-archive/P128.md:650:| nq | 128 | 1 | 7.740 | 1.143 | 1.36 | 1.48 | 2030 | 1596 | 1.27 | 1.49 | 100 | 0 | 0 / 0 | 855 | 4-11 | 0/9 | 8-14 |
/home/user/Projects/lane-notes-archive/P128.md:651:| POST | 128 | 2 | 7.434 | 1.170 | 1.36 | 1.58 | **2033** | 1656 | 1.23 | 1.66 | 100 | 0 | **0 / 0** | 913 | 6-12 | 0/9 | 9-12 |
/home/user/Projects/lane-notes-archive/P128.md:652:| nq | 128 | 2 | 8.462 | 1.100 | 1.32 | 1.55 | 2030 | 1462 | 1.39 | 1.14 | 100 | 0 | 0 / 0 | 920 | 7-13 | 0/9 | 10-14 |
/home/user/Projects/lane-notes-archive/P128.md:656:= `read_local_defer_quota` = `read_local_fallback_lane_full` = 0 and 100% of reads local. The lane
/home/user/Projects/lane-notes-archive/P128.md:685:### 9.5 OWNER ACCEPTANCE RUN, and the footprint defect it exposed in the control cell
/home/user/Projects/lane-notes-archive/P128.md:693:| | **ARM** | **40.41 / 39.74** | **100.0 / 100.0** | **0 / 0** | 242,212 / 239,735 | **2284 / 2282** | **2450 / 2495** | - | **7.54 / 7.85** | - | **5.50 / 5.50** |
/home/user/Projects/lane-notes-archive/P128.md:694:| 512 conns (8/thr) | base | 56.32 / 56.18 | 100 | 0 | - | 2273 / 2271 | 1762 / 1766 | 1.287 / 1.285 | 3.10 / 3.11 | 9.16 / 9.22 | - |
/home/user/Projects/lane-notes-archive/P128.md:695:| | ARM | 54.30 / 53.92 | 100 | 0 | **0 / 0** | 2268 / 2269 | 1827 / 1841 | 1.244 / 1.234 | **3.42 / 3.48** | **9.53 / 9.52** | - |
/home/user/Projects/lane-notes-archive/P128.md:704:same-CCX 9.19 -> 9.525 (+3.6%), i.e. **+0.68 demand fills per op with both defer counters at
/home/user/Projects/lane-notes-archive/P128.md:705:exactly 0**. Zero counters plus more fills means the machinery costs FOOTPRINT, not work -- the
/home/user/Projects/lane-notes-archive/P128.md:709:#### What this lane added, and where every byte of it sits (read from the two binaries' DWARF)
/home/user/Projects/lane-notes-archive/P128.md:734:touched, with the mechanism dormant and every counter reading zero.
/home/user/Projects/lane-notes-archive/P128.md:741:#### Fix (commit 20d449a24): gate the argument, not just the answer
/home/user/Projects/lane-notes-archive/P128.md:751:### 9.6 Verification of the lane-cap row (commit 32d99db98)
/home/user/Projects/lane-notes-archive/P128.md:761:| arm | instr/op | cyc/op | DRAM/op | Mops/s | lane counters |
/home/user/Projects/lane-notes-archive/P128.md:773:## 10. FINAL PRE/POST
/home/user/Projects/lane-notes-archive/P128.md:781:### 10.1 Measured on the lane rig -- 4 saturated fused threads (32,33,160,161), `--shards 4`
/home/user/Projects/lane-notes-archive/P128.md:794:| 9 x p32 rep1 | 288 (0.28x) | PRE | 7.040 | 0.244 | 0.28 | 0.41 | 2263 | 1752 | 100 | 0 | - | 7-12 | box |
/home/user/Projects/lane-notes-archive/P128.md:795:| | | **POST** | 6.663 | 0.252 | 0.29 | 0.39 | **2269** | 1861 | 100 | 0 | **0 / 0** | 6-11 | box |
/home/user/Projects/lane-notes-archive/P128.md:796:| | | nq | 6.965 | 0.300 | 0.34 | 0.38 | 2268 | 1767 | 100 | 0 | 0 / 0 | 6-14 | box |
/home/user/Projects/lane-notes-archive/P128.md:797:| 9 x p32 rep2 | 288 | PRE | 7.133 | 0.234 | 0.27 | 0.32 | 2263 | 1744 | 100 | 0 | - | 5-11 | box |
/home/user/Projects/lane-notes-archive/P128.md:798:| | | **POST** | 6.714 | 0.279 | 0.32 | 0.40 | **2270** | 1874 | 100 | 0 | **0 / 0** | 6-12 | box |
/home/user/Projects/lane-notes-archive/P128.md:799:| | | nq | 6.600 | 0.276 | 0.32 | 0.43 | 2271 | 1866 | 100 | 0 | 0 / 0 | 4-13 | box |
/home/user/Projects/lane-notes-archive/P128.md:800:| 9 x p128 rep1 | 576 (0.56x) | PRE | 8.536 | 0.959 | 1.11 | 1.20 | 2027 | 1432 | 100 | 0 | - | 7-11 | box |
/home/user/Projects/lane-notes-archive/P128.md:801:| | | **POST** | 7.870 | 1.066 | 1.24 | 1.42 | **2033** | 1580 | 100 | 0 | **0 / 0** | 7-12 | box |
/home/user/Projects/lane-notes-archive/P128.md:802:| | | nq | 7.740 | 1.143 | 1.36 | 1.48 | 2030 | 1596 | 100 | 0 | 0 / 0 | 4-11 | box |
/home/user/Projects/lane-notes-archive/P128.md:803:| 9 x p128 rep2 | 576 | **POST** | 7.434 | 1.170 | 1.36 | 1.58 | **2033** | 1656 | 100 | 0 | **0 / 0** | 6-12 | box |
/home/user/Projects/lane-notes-archive/P128.md:804:| | | nq | 8.462 | 1.100 | 1.32 | 1.55 | 2030 | 1462 | 100 | 0 | 0 / 0 | 7-13 | box |
/home/user/Projects/lane-notes-archive/P128.md:805:| 33 x p32 rep1 | 1056 (1.03x) | PRE | 7.378 | 0.855 | 1.03 | 1.43 | 2243 | 1672 | 100 | 33 | - | 30-38 | box |
/home/user/Projects/lane-notes-archive/P128.md:806:| | | **POST** | 6.323 | 1.042 | 1.25 | 1.53 | 2248 | 1956 | 100 | **0** | 16 / 783 | 27-38 | box |
/home/user/Projects/lane-notes-archive/P128.md:807:| | | nq | 7.023 | 1.002 | 1.19 | 1.52 | 2244 | 1734 | 100 | 0 | 8 / 0 | 25-39 | box |
/home/user/Projects/lane-notes-archive/P128.md:808:| 33 x p32 rep2 | 1056 | PRE | 5.799 | 1.247 | 1.51 | 1.87 | 2248 | 2149 | 100 | 1,757 | - | 24-42 | box |
/home/user/Projects/lane-notes-archive/P128.md:809:| | | **POST** | 6.418 | 1.055 | 1.27 | 1.67 | 2249 | 1977 | 100 | **0** | 5 / 488 | 30-39 | box |
/home/user/Projects/lane-notes-archive/P128.md:810:| | | nq | 6.741 | 0.994 | 1.19 | 1.59 | 2244 | 1796 | 100 | 0 | 10 / 0 | 29-37 | box |
/home/user/Projects/lane-notes-archive/P128.md:812:| | | **POST** | **6.233** | **1.967** | **2.58** | **4.04** | **2259** | **1922** | **100** | **0** | 748 / 13,456 | 60-67 | **CLEAN** |
/home/user/Projects/lane-notes-archive/P128.md:813:| | | nq | 6.101 | 1.815 | 2.73 | 4.33 | 2255 | 1997 | 100 | 0 | 1,074 / 0 | 59-69 | **CLEAN** |
/home/user/Projects/lane-notes-archive/P128.md:815:| | | **POST** | **5.931** | **2.138** | **2.77** | **3.84** | **2261** | **2113** | **100** | **0** | 603 / 13,718 | 46-74 | **CLEAN** |
/home/user/Projects/lane-notes-archive/P128.md:816:| | | nq | 5.868 | 2.175 | 2.70 | 3.73 | 2256 | 2013 | 100 | 0 | 1,109 / 0 | 56-70 | **CLEAN** |
/home/user/Projects/lane-notes-archive/P128.md:821:the arm does, so its THROUGHPUT column is not decisive -- its instr/op and counter columns still
/home/user/Projects/lane-notes-archive/P128.md:827:  100%, instr/op -27%, and every tail lower.
/home/user/Projects/lane-notes-archive/P128.md:829:  counters are 0 in every cell and the only cost is instr/op +0.3%.
/home/user/Projects/lane-notes-archive/P128.md:834:### 10.2 The owner's quiet box (32 cores, 64 fused threads, 1s + read-local, pure GET)
/home/user/Projects/lane-notes-archive/P128.md:842:`CONFIG RESETSTAT` baselines. The acceptance criteria below are stated on counter DELTAS across the
/home/user/Projects/lane-notes-archive/P128.md:849:| **512 conns p32** (8/thr x 32 = 256, lane never fills) | 52.3M, cyc/op 1893, IPC 1.19, instr/op 2241, crossCCX 3.6/op, DRAM 3.3/op, local 100% | identical: the pressure window never arms, so the parser pays one predicted byte load per pass and one popcount compare per eligible read | within noise (±0.5%); `read_local_defer_lane_full` = `read_local_defer_quota` = **0**; instr/op within +0.3% |
/home/user/Projects/lane-notes-archive/P128.md:850:| **2048 conns p32** (32/thr x 32 = 1024 = the lane boundary) | **25.7M**, cyc/op 3850, IPC 0.74, instr/op 2846, crossCCX 7.8/op, DRAM 11.0/op, **local 56%** | **35-40M**. Measured on the analogous rig cell (57.8% local, the same mechanism strength): +38.3%, instr/op -27.2%, cyc/op -28.9%, local -> 100%. The ~600 cyc/op per-connection DRAM footprint term is common to 2s and does NOT go away. | **local >= 99%**; `read_local_fallback_lane_full` = **0**; cyc/op <= 2700; instr/op <= 2350; **>= 34M**; p99.9 and p99.99 not worse than PRE |
/home/user/Projects/lane-notes-archive/P128.md:854:### 10.3 The two cells I am asking the owner to run
/home/user/Projects/lane-notes-archive/P128.md:861:   land at or above 34M. If POST is 100% local but still slow, the residue is the per-connection
/home/user/Projects/lane-notes-archive/P128.md:865:   filling. POST must be within noise and BOTH defer counters must read exactly 0: that is the
/home/user/Projects/lane-notes-archive/DESIGN-RINGDIET.md:1:# DESIGN-RINGDIET.md — arm the RYOW write ring on demand
/home/user/Projects/lane-notes-archive/DESIGN-RINGDIET.md:6:## The measured fact this answers
/home/user/Projects/lane-notes-archive/DESIGN-RINGDIET.md:21:### Mechanism
/home/user/Projects/lane-notes-archive/DESIGN-RINGDIET.md:25:ring to the reordering window (`kRobWindow`, 64) made overflow unreachable, so **every** write is
/home/user/Projects/lane-notes-archive/DESIGN-RINGDIET.md:30:## The design
/home/user/Projects/lane-notes-archive/DESIGN-RINGDIET.md:32:### (1) Arm on demand
/home/user/Projects/lane-notes-archive/DESIGN-RINGDIET.md:53:#### The arming transient, and why it is not the overflow generation
/home/user/Projects/lane-notes-archive/DESIGN-RINGDIET.md:67:### (2) Sidecar on first write
/home/user/Projects/lane-notes-archive/DESIGN-RINGDIET.md:83:### What did not change
/home/user/Projects/lane-notes-archive/DESIGN-RINGDIET.md:98:## Counters (INFO, `# Readlocal`)
/home/user/Projects/lane-notes-archive/DESIGN-RINGDIET.md:105:| `read_local_fallback_arm_transient` | reads demoted by the arming transient, reported apart from `read_local_fallback_inflight_write` (a steady-state key conflict) so a bench can never confuse the two |
/home/user/Projects/lane-notes-archive/DESIGN-RINGDIET.md:107:Every increment sits behind a `noinline` call the unarmed stream never makes, so the counters are
/home/user/Projects/lane-notes-archive/DESIGN-RINGDIET.md:114:# Results
/home/user/Projects/lane-notes-archive/DESIGN-RINGDIET.md:119:is 100%. Three rounds, visits interleaved **A B B A** within each round, one fresh boot and preload
/home/user/Projects/lane-notes-archive/DESIGN-RINGDIET.md:165:    1:1 p32 2048c (footprint)               1.026    10063.2    5662.5    1.780     37.10  20984.38
/home/user/Projects/lane-notes-archive/DESIGN-RINGDIET.md:185:## Verdict per cell
/home/user/Projects/lane-notes-archive/DESIGN-RINGDIET.md:196:## The proof obligations
/home/user/Projects/lane-notes-archive/DESIGN-RINGDIET.md:203:**100% local service after the transient**, and the transient bound. Measured over whole runs:
/home/user/Projects/lane-notes-archive/DESIGN-RINGDIET.md:218:**A read arriving with writes in flight before arming is demoted, never served stale.** Directed and
/home/user/Projects/lane-notes-archive/DESIGN-RINGDIET.md:222:0 stale reads, 40 arms, 40 transient demotions reported.
/home/user/Projects/lane-notes-archive/DESIGN-RINGDIET.md:224:## RSS per connection
/home/user/Projects/lane-notes-archive/DESIGN-RINGDIET.md:232:| idle | 2048 | 11394 | 10092 | **−1302** |
/home/user/Projects/lane-notes-archive/DESIGN-RINGDIET.md:246:# Gate
/home/user/Projects/lane-notes-archive/DESIGN-RINGDIET.md:257:## The 20 failures are three environment clusters, and the control says so
/home/user/Projects/lane-notes-archive/DESIGN-RINGDIET.md:266:| load-sensitive assertions | 4 | `multirace`: every correctness assertion passed (leaked=0, stale=0, bad_exec=0) and the row failed its own **vacuity** check — the timing window it exists to close never opened. `flipctl`: "controller moved during stable hold", `last_trigger: fingerprint-shift` — the controller reacted to a workload shift that a shared box produces. `atomic RYOW under ASAN`: a THROUGHPUT ratio, pipe=2840/s against serial=14328/s. |
/home/user/Projects/lane-notes-archive/DESIGN-RINGDIET.md:271:lane touches is unreachable.
/home/user/Projects/lane-notes-archive/DESIGN-RINGDIET.md:293:## The 20, accounted for
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:1:# DESIGN-flipfp -- the flip fingerprint leaves the per-op hot path
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:12:## 1. What the fingerprint computes and who reads it
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:22:more) and, once `partial_.commands >= flip_work_window` (default 100), adds the window into the
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:28:is therefore the tick aggregate, not the 100-command window; the window is only the publication
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:34:counters; DEBUG FLIPCTL dumps controller state. **Nothing reads `published()` when `--flip-auto 0`
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:38:## 2. Design
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:52:point paths count commands on the executor side, so no io-side per-op counter exists that all 15
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:62:**Arithmetic.** W = `flip_work_window` (100), d = frames per pass, C = commands/s over the io
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:71:| publications per tick | C·T / 100 | C·T / (W·d) |
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:88:unchanged. W = 1 is exhaustive sampling -- today's behaviour exactly -- and is the equivalence oracle
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:101:100-command publication window, and
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:109:  learned from whichever stream the detector saw. The sampled stream's jitter is the exhaustive
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:113:  accuracy cell (C ~ 350k/s, W = 100) the PRE band was 2.4e-3 against a PRE floor of 2.3e-5 -- 100x
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:114:  above the floor -- and the POST floor lands at 2.3e-3, i.e. right at the jitter the exhaustive
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:130:## 3. Footprint
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:136:## 4. Proof plan (results in the lane report)
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:148:## 5. Results (base ceb6b02f8, arms differ by this change only)
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:157:### 5.1 The lever, against its own same-binary null
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:187:DRAM -0.08. The only counter that rises is the *user-attributed* subset on one cell (1s SET,
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:198:and 2 threads alike; the null on that counter is -1 and +0, so it is real) while cycles/op is flat to
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:200:adding work; net cycles do not regress. It does not appear in split, where the same counter falls.
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:202:### 5.2 Controller accuracy: unchanged
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:211:value is the predicted quantum floor: `8W/(C*T) = 8*100/3.64e6 = 2.20e-4` against 2.06e-4 measured.
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:253:### 5.3 Correctness
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:258:embedded exhaustive writer over 20k mixed-depth passes, the dark writer's gate word, and the
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:272:## 6. Cross-lane: the quantum's N, and the deafness margin at 1-in-100
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:278:### 6.1 N is already the SAMPLED count
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:294:its sample are the same accumulator**, and no dispatched-command counter exists anywhere on that
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:298:with W = 100 and jitter 0 (quantum-dominated):
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:303:| dispatched (C*T = 3.804e6) | 2.10e-6 | 100x smaller than measured |
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:313:### 6.2 The deafness margin, measured
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:325:| | PRE (exhaustive) | POST (1 pass in 100) |
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:344:W = 100. With the redesign's proposed `2/sqrt(N)` floor it needs `N <= 32.7`, i.e. below ~3.3k
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:353:### 6.3 Gate on fc3f6f51c
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:374:### 6.4 N, at the line
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:403:dispatched count are two separate counters that happen to share a method name; only the first is
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:416:### 6.5 The flip lane's probe, at this lane's window size
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:420:pointed at either detector, and added rows at this lane's real configuration: W = 100, so the
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:421:sampled count per 1 s tick is (commands/s)/100, evaluated at the three rates this lane measured.
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:428:| their reference: 1-in-100 | 100 | 0.2000 | 0.0967 | **0/600** | **0** | 0.667 | **3.3x** |
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:429:| 3.8M/s 1-in-100 | 380 | 0.1026 | 0.0579 | **0/600** | **0** | 0.671 | **6.5x** |
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:430:| 3.8M/s 1-in-100 typed | 380 | 0.1026 | 0.0412 | **0/600** | **0** | 0.672 | **6.6x** |
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:431:| 512k/s 1-in-100 | 512 | 0.0884 | 0.0479 | **0/600** | **0** | 0.674 | **7.6x** |
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:432:| 512k/s 1-in-100 typed | 512 | 0.0884 | 0.0364 | **0/600** | **0** | 0.672 | **7.6x** |
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:433:| 6k/s 1-in-100 (thinnest real cell) | 60 | 0.2582 | 0.1488 | **0/600** | **0** | 0.667 | **2.6x** |
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:434:| 6k/s 1-in-100 typed | 60 | 0.2582 | 0.1425 | **0/600** | **0** | 0.668 | **2.6x** |
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:436:Their published 3.3x at N = 100 reproduces exactly. At this lane's real window size the redesign
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:445:| 1-in-100 learned, N=380 / 512 / 60 | | 0.046 / 0.041 / 0.133 | 0.044 / 0.036 / 0.112 | 4 / 3 / 0 | **1 / 0 / 0** |
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:446:| **1-in-100 TYPED (band 2), N=380** | 380 | 0.0200 | 0.0309 | 26/600 | **4** |
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:447:| **1-in-100 TYPED (band 2), N=512** | 512 | 0.0200 | 0.0273 | 26/600 | **3** |
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:448:| **1-in-100 TYPED (band 2), N=100** | 100 | 0.0200 | 0.0604 | 231/600 | **73** |
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:449:| **1-in-100 TYPED (band 2), N=60** | 60 | 0.0200 | 0.1069 | 467/600 | **208** |
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:456:branch, `band_ = max(configured/100, floor)`, is exactly the fix and produces 0/600 above.
/home/user/Projects/lane-notes-archive/DESIGN-flipfp.md:472:### 6.6 Gate, clean
/home/user/Projects/lane-notes-archive/DESIGN-P0REPLY.md:1:# P0: the armed-fused crash — attribution, real crash site, and the instrument
/home/user/Projects/lane-notes-archive/DESIGN-P0REPLY.md:8:## 1. The reported evidence was two different failures, not one
/home/user/Projects/lane-notes-archive/DESIGN-P0REPLY.md:19: in tomokv[1968b5,5912d5b71000+36f000] likely on CPU 171 (core 43, socket 0)
/home/user/Projects/lane-notes-archive/DESIGN-P0REPLY.md:28:  exec segment has `VirtAddr == Offset == 0x011000`, so the ELF vaddr is `0x1968b5`.
/home/user/Projects/lane-notes-archive/DESIGN-P0REPLY.md:50:## 2. The real crash site: the armed-write block cache
/home/user/Projects/lane-notes-archive/DESIGN-P0REPLY.md:62:`heads[cls] == 0xc000100`: a WILD FREE-LIST HEAD. `0xc000100` is the size and shape of a KvObj
/home/user/Projects/lane-notes-archive/DESIGN-P0REPLY.md:75:## 3. What has been excluded
/home/user/Projects/lane-notes-archive/DESIGN-P0REPLY.md:79:* **The LB / flip shard-ownership handoff.** `ReadLocalDeferredQueue::reclaim_entry` passes the OLD
/home/user/Projects/lane-notes-archive/DESIGN-P0REPLY.md:88:## 4. The instrument (debug build only)
/home/user/Projects/lane-notes-archive/DESIGN-P0REPLY.md:99:* **list matches its counter** — every `take`/`put` walks the class list and checks that each
/home/user/Projects/lane-notes-archive/DESIGN-P0REPLY.md:107:## 5. Reproduction in flight
/home/user/Projects/lane-notes-archive/DESIGN-P0REPLY.md:113:## 6. REPRODUCED — on the release binary, in seconds
/home/user/Projects/lane-notes-archive/DESIGN-P0REPLY.md:148:## 7. ROOT CAUSE — the block cache is NOT owner-private
/home/user/Projects/lane-notes-archive/DESIGN-P0REPLY.md:160:          exec_batch / drain_tasks          ex_loop.h:2758 / 2100
/home/user/Projects/lane-notes-archive/DESIGN-P0REPLY.md:173:(`0xc000100` — the ringdiet segfault).
/home/user/Projects/lane-notes-archive/DESIGN-P0REPLY.md:175:The same stale pointer is `retire_sink.defer`, which pushes into `ReadLocalDeferredQueue`'s
/home/user/Projects/lane-notes-archive/DESIGN-P0REPLY.md:178:## 8. THE MECHANISM, from the event log
/home/user/Projects/lane-notes-archive/DESIGN-P0REPLY.md:210:## 9. THE FIX — the sink moves with ownership
/home/user/Projects/lane-notes-archive/DESIGN-P0REPLY.md:215:hold every executor at the quiesced safe point (`lb_commit_shard_plan` commits only once every EX
/home/user/Projects/lane-notes-archive/DESIGN-P0REPLY.md:228:## 10. Is there a third path? — the audit of every ownership edge
/home/user/Projects/lane-notes-archive/DESIGN-P0REPLY.md:243:* the FLIP path rebinds it at a barrier — `FlipStage::ExInstall`, ex_loop.h:1832, while IO dispatch
/home/user/Projects/lane-notes-archive/DESIGN-P0REPLY.md:248:Severity is far lower and it is not the same defect: the stale pointer is written, not spliced, so
/home/user/Projects/lane-notes-archive/DESIGN-P0REPLY.md:264:## 11. Two harness findings the new row produced
/home/user/Projects/lane-notes-archive/DESIGN-P0REPLY.md:272:| `--lb-tick-ms 100`       | 50          |
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:1:# NOTES-MULTIRACE — an aborted MSETNX's withdrawn value laundered into an acknowledged EXEC write
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:19:## 1. PRE vs POST
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:41:## 2. The differ is a WEAK detector; the battery is the strong one
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:58:## 3. Reduction: 4260 differ operations → one 7-command round
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:82:## 4. Mechanism
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:141:## 5. The fix, and why it is safe where the earlier attempt was not
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:168:### Why this is not the deadlock NOTES-MULTIRES.md §5(a) forbids
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:195:### What the fix does NOT do
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:210:### One counter, one meaning — and why the falsifier needed a second one
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:212:Two different claims are being made, and they need two counters:
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:224:The refinement this lane closes: those two sites were briefly **merged onto one counter**, which
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:226:so a summed counter can never be checked against zero — the safety argument could have broken with
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:233:a 20th counter would have grown it. Nothing is lost by the placement: the increment sits on a path
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:235:shape (`scripting.cc`, `slowlog.cc`, `functions.cc` all keep INFO counters this way).
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:245:**Effect on `tests/multires.py`.** That battery locks the same hazard window and used this counter
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:247:dispatch — so its counter now advances at the park; its docstring said "and where an EXEC write
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:254:## 6. Containment
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:269:### 6a. (filled in from the run logs)
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:273:## 7. Closing the gate gap
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:308:## 8. Files changed
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:314:| `src/cmd/multi.inc` | `multi_state_owns_epoch()` / `multi_task_owns_epoch()`; `prepare_write_key()`'s probe now asks the same question as the park and reports into its own counter, `g_exec_order_late` |
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:321:| `tests/multires.py` | docstring: the counter has one site and the window now closes at dispatch; asserts `atomic_exec_order_late == 0` |
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:332:## 9. Resumption state (2026-09-05, after the 19:20 usage-limit kill)
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:351:### Done
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:352:* §5's counter split — `atomic_exec_order_late` is now its own namespace-scope counter, INFO-
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:354:  Commit `286e72265`. This is the "counter-honesty issue in prepare_write_key" the lane was on.
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:356:### Pending (blocked on the box: `scratchpad/quiet.done` absent = owner measuring)
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:357:1. Rebuild POST (`build/tomokv` is from 14:45, older than the counter split).
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:363:### 10. The perf question this fix has to answer
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:372:  predicate it calls and which counter it raises.
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:390:## 11. Re-verification on the 2026-09-05 allocation (4 physical cores)
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:392:### 11a. A capacity constraint that first read as a non-reproduction
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:409:### 11b. Directed reproduction, 200 rounds per boot, 2 victims
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:426:## 12. Containment results, 2026-09-05 allocation
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:429:the binary; INFO reports both counters).
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:461:## 13. The armed-fused differential matrix — the leg the gate never covered
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:495:### Batteries, both thread modes
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:510:## 14. What the fix costs, measured against a same-binary null
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:527:### 14a. The table
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:559:**The counters are what make each cell mean what it claims**, per 400,000 rounds:
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:570:### 14b. Reading it
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:601:**CORRECTION to an earlier reading in this lane.** A 3-rep hand run made before these counters were
/home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md:606:null that was really a small signal under noise. The counters are what corrected it: a perf cell
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:1:# Runtime ownership migration
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:20:## Prior-art audit
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:42:every abort/release path, atomic grow-back states, and a recovery counter.
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:53:## Shared terminology and publication rules
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:67:* A stale message is safe only because the receiver checks the current owner
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:70:## 1. Bucket transfer
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:72:### State machine
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:84:   deferred work.  New stale arrivals are forwarded.  Wait until no executing
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:100:   phase is the ownership handoff. Before it, only source may touch the shards;
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:104:   a safe stale route.
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:109:IO thread can route using a pre-flip owner read and enqueue after the handoff.
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:111:access.  The old owner forwards a stale item to the current owner.  Converted IO
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:112:roles are stricter: the global producer barrier and an unmasked drain prove every
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:116:### In-flight cases
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:122:| Operation executing in source | Handoff waits for the source safe point. |
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:128:| Reader racing the multi-entry owner-array write | It returns one non-tearing packed entry: source (safe stale route) or destination; destination is never published before commit. |
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:136:## 2. Client transfer
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:141:### State machine
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:176:   then acknowledges installation so the source may retire transient stale-route
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:179:   * epoll activates the destination registration prepared before handoff.
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:189:### In-flight cases
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:195:| io_uring recv/poll points at `Client` | Explicit asynchronous cancellation; wait for the original CQE before handoff. |
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:196:| io_uring send CQE pending | Drain it before handoff. |
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:197:| epoll event already returned to source | Preparation runs only after that event batch; the original source interest is removed before handoff. |
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:203:| Pub/sub, tracking, or monitor owner-local state | Reserved and extracted at commit, installed at destination, and re-registered at each pub/sub HOME. In-flight deliveries read `ifid_thread()` and forward from a stale owner. |
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:234:## 3. Manual FLIP
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:250:### Automatic arithmetic rebalance
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:300:### State machine
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:318:   scatter/atomic witnesses, then acknowledges a hard safe point.  Once
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:374:### In-flight cases
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:382:| Completion/borrow release headed to a converting thread | Old-role inbox is drained and stale messages are ownership-forwarded during the grace phase. |
/home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md:394:### Observability and tests

exec
/bin/bash -lc "rg -n -C 5 '\\.md' src/core/thread.h src/core/shard.h src/core/signal.h src/core/flipctl.h src/core/server.h src/core/config.h" in /home/user/Projects/cx-inline-comments
 succeeded in 0ms:
src/core/config.h-289-    uint32_t key_lb = 1;
src/core/config.h-290-    uint32_t client_lb = 1;
src/core/config.h-291-
src/core/config.h-292-    // ---- automatic role split (boot-latched) -----------------------------------------------
src/core/config.h-293-    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer
src/core/config.h:294:    // (DESIGN-flipfp.md); its measured sampling policy is internal to flipctl.h. Off and 1s
src/core/config.h-295-    // dispatch pay one predicted branch per op and no fingerprint store. Trigger bands learn
src/core/config.h-296-    // from the anchor's own quiet jitter; no separate controller tuning knobs remain.
src/core/config.h-297-    uint32_t flip_auto = 0;
src/core/config.h-298-
src/core/config.h-299-    // ---- network (boot-only) ---------------------------------------------------------------
--
src/core/signal.h-45-
src/core/signal.h-46-// Wall clock in milliseconds. Key deadlines are absolute CLOCK_REALTIME milliseconds, so ANYTHING
src/core/signal.h-47-// that will be compared against a deadline must come from here and never from now_ns() above --
src/core/signal.h-48-// that one is CLOCK_MONOTONIC, so its "milliseconds" are milliseconds since boot and sit roughly
src/core/signal.h-49-// five orders of magnitude below any real deadline. A monotonic value used as an expiry cut does
src/core/signal.h:50:// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
src/core/signal.h-51-inline int64_t now_realtime_ms() {
src/core/signal.h-52-    timespec ts;
src/core/signal.h-53-    clock_gettime(CLOCK_REALTIME, &ts);
src/core/signal.h-54-    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
src/core/signal.h-55-}
--
src/core/shard.h-429-        // -- so a non-zero reading is proof the window opened rather than proof the test ran.
src/core/shard.h-430-        // The question "did the park fire EARLY ENOUGH" is a different one and deliberately does
src/core/shard.h-431-        // NOT share this counter: multi.inc's prepare_write_key() re-asks it at install time and
src/core/shard.h-432-        // reports violations through INFO atomic_exec_order_late (multi_exec_order_late()), which
src/core/shard.h-433-        // must read zero. Summing the two would hide a violation inside a number that is non-zero
src/core/shard.h:434:        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
src/core/shard.h-435-        uint64_t atomic_exec_order_holds = 0;
src/core/shard.h-436-        // Times watch_finalize_reservation() answered "not ready" because the reservation's epoch
src/core/shard.h-437-        // was still 0, i.e. a unit was turned into a Retry by an undecided WATCH reservation. It
src/core/shard.h-438-        // is the ONLY direct evidence that the WATCH-liveness path was entered at all, so
src/core/shard.h-439-        // tests/watchlive.py asserts it advanced before believing a clean run. Reached only from
--
src/core/thread.h-116-    ContextOwnerKey,        // unfinished precise owner op with an overlapping key hash
src/core/thread.h-117-    ContextConnectionState, // blocked or subscriber-mode connection
src/core/thread.h-118-    ContextRoute,           // scatter/script/all-shard or conservative broad-owner route
src/core/thread.h-119-    ContextKeymissNotify,   // MGET miss must retain owner-side notification behavior
src/core/thread.h-120-    InflightWrite,
src/core/thread.h:121:    ArmTransient,           // pre-arming writes still in flight (DESIGN-RINGDIET.md)
src/core/thread.h-122-    AtomicPending,
src/core/thread.h-123-    Missing,
src/core/thread.h-124-    Typed,
src/core/thread.h-125-    Expired,
src/core/thread.h-126-    SeqChurn,
--
src/core/thread.h-149-    uint64_t fallback_context_keymiss_notify = 0;
src/core/thread.h-150-    uint64_t fallback_inflight_write = 0;
src/core/thread.h-151-    // ARM-ON-DEMAND TRANSIENT. Reads demoted because the connection carried writes published
src/core/thread.h-152-    // BEFORE a local read armed it -- writes the RYOW ring deliberately never recorded. Bounded
src/core/thread.h-153-    // per connection by one ROB drain and reported separately from the steady-state key conflict
src/core/thread.h:154:    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
src/core/thread.h-155-    uint64_t fallback_arm_transient = 0;
src/core/thread.h-156-    uint64_t fallback_atomic_pending = 0;
src/core/thread.h-157-    uint64_t fallback_missing = 0;
src/core/thread.h-158-    uint64_t fallback_typed = 0;
src/core/thread.h-159-    uint64_t fallback_expired = 0;
src/core/thread.h-160-    uint64_t fallback_seq_churn = 0;
src/core/thread.h-161-    uint64_t fallback_generation = 0;
src/core/thread.h-162-    uint64_t fallback_lane_full = 0;
src/core/thread.h:163:    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
src/core/thread.h-164-    // re-parsed by a later pass of the same thread, because the local-read lane had no room
src/core/thread.h-165-    // (defer_lane_full) or because the connection already held its fair share of a lane under
src/core/thread.h-166-    // pressure (defer_quota). Neither is a fallback: the read still completes locally, and no
src/core/thread.h-167-    // cross-thread owner task is ever created for lack of lane capacity. fallback_lane_full is
src/core/thread.h-168-    // kept for INFO continuity and is 0 by construction since the parser stopped demoting here.
--
src/core/server.h-7-// LoopSignals, both single-writer; INFO sums them on request.
src/core/server.h-8-//
src/core/server.h-9-// THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
src/core/server.h-10-// dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
src/core/server.h-11-// its derived shard-granularity fast path, published only at a quiesced commit. See
src/core/server.h:12:// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
src/core/server.h-13-#pragma once
src/core/server.h-14-#include <algorithm>
src/core/server.h-15-#include <atomic>
src/core/server.h-16-#include <cstdint>
src/core/server.h-17-#include <ctime>
--
src/core/server.h-306-        for (uint32_t i = 0; i <= nthreads; i++)
src/core/server.h-307-            live_config_mailboxes_[i].init(live_config_committed_);
src/core/server.h-308-        for (uint32_t i = 0; i < nthreads; i++) {
src/core/server.h-309-            threads_[i] = std::make_unique<ThreadCtx>();
src/core/server.h-310-            // The fingerprint writer is armed only when its one reader, the flip controller, is
src/core/server.h:311:            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
src/core/server.h-312-            // one predicted branch per op. The enabled sampler keeps its measured 1-in-100 policy.
src/core/server.h-313-            threads_[i]->init(i, placement_.role_of(i), nthreads,
src/core/server.h-314-                              0,
src/core/server.h-315-                              flip_fingerprint_window(flipctl_.enabled()));
src/core/server.h-316-            threads_[i]->init_command_counts(command_registry_size());
--
src/core/server.h-1967-        Shard& sh = *shards_[static_cast<uint32_t>(shard_id)];
src/core/server.h-1968-        router_.set_initial_owner(sh.bucket_begin(), sh.bucket_end(), thread_id);
src/core/server.h-1969-        shard_owner_[shard_id].store(thread_id, std::memory_order_release);
src/core/server.h-1970-    }
src/core/server.h-1971-
src/core/server.h:1972:    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
src/core/server.h-1973-    // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
src/core/server.h-1974-    // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
src/core/server.h-1975-    // Vector capacity and membership are settled while PREPARING still names the source.  The
src/core/server.h-1976-    // phase store in commit_transfer() is the sole ownership edge; bucket entries and the derived
src/core/server.h-1977-    // shard array publish destination only after that edge.
--
src/core/server.h-1986-    // Moving the shard without moving the sink leaves the DESTINATION executing writes through the
src/core/server.h-1987-    // SOURCE's ring and free list. Two threads then splice one unlocked list: the loser's update is
src/core/server.h-1988-    // lost, a block ends up linked twice, and the damage surfaces later as either
src/core/server.h-1989-    // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
src/core/server.h-1990-    // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
src/core/server.h:1991:    // Both were observed (see DESIGN-P0REPLY.md).
src/core/server.h-1992-    //
src/core/server.h-1993-    // The rebind used to be deferred to the destination's own next executor pass
src/core/server.h-1994-    // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
src/core/server.h-1995-    // pass before the destination's first write to the shard it had just been given, and the
src/core/server.h-1996-    // reproduction shows exactly that gap: transfer sid 1 to thread 1, then thread 1 executing
--
src/core/server.h-2825-    // TEST HOOK (DEBUG ATOMIC-DIRECT-DEFER). Number of extra owner passes a cross-shard RENAME's
src/core/server.h-2826-    // destination task is held after its source hop is ready. Zero in production; its only reader
src/core/server.h-2827-    // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
src/core/server.h-2828-    // other command. It widens -- deterministically -- the window in which a younger whole-owner
src/core/server.h-2829-    // walker could overtake an older same-connection group on the destination shard.
src/core/server.h:2830:    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
src/core/server.h-2831-    // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
src/core/server.h-2832-    // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
src/core/server.h-2833-    // its masking, so this can never overrun anything. It exists because lane oversubscription is
src/core/server.h-2834-    // otherwise a RATE RACE the gate cannot win -- a test client cannot push 1024 frames into one
src/core/server.h-2835-    // fused thread faster than that thread drains them, so the anti-vacuity row of the lane battery
--
src/core/server.h-2933-    // soon as the latch is cleared. Zero in production, and read at exactly two already-cold
src/core/server.h-2934-    // places: the blocking dispatch arm, and the arm of flush_ready that only runs when a barrier
src/core/server.h-2935-    // is already set. Nothing on GET/SET touches it.
src/core/server.h-2936-    //
src/core/server.h-2937-    // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
src/core/server.h:2938:    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
src/core/server.h-2939-    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
src/core/server.h-2940-    // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
src/core/server.h-2941-    //
src/core/server.h-2942-    // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
src/core/server.h-2943-    // nothing left to do and parks on submit_and_wait(1) -- unbounded under io_uring. A hold that
--
src/core/server.h-2967-    uint64_t debug_blocking_timeout_reaps() const {
src/core/server.h-2968-        return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
src/core/server.h-2969-    }
src/core/server.h-2970-    // An overlap: some owner took the parse barrier while another owner already held it. With
src/core/server.h-2971-    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
src/core/server.h:2972:    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
src/core/server.h-2973-    // live and the owner-scoped release in blocking_retire() became load-bearing rather than
src/core/server.h-2974-    // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
src/core/server.h-2975-    // own positive control: a "must be zero" reading proves nothing until something has been shown
src/core/server.h-2976-    // able to make it non-zero.
src/core/server.h-2977-    void note_barrier_overlap() {
--
src/core/flipctl.h-17-
src/core/flipctl.h-18-namespace tomo {
src/core/flipctl.h-19-
src/core/flipctl.h-20-class Server;
src/core/flipctl.h-21-
src/core/flipctl.h:22:// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
src/core/flipctl.h-23-// Arming derives solely from the controller latch. No runtime sampling knob or adaptive
src/core/flipctl.h-24-// per-operation work is needed; the detector already derives its bands from actual samples.
src/core/flipctl.h-25-inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
src/core/flipctl.h-26-    return controller_enabled ? 100u : 0u;
src/core/flipctl.h-27-}
--
src/core/flipctl.h-50-    uint64_t multikey_ops = 0;
src/core/flipctl.h-51-    uint64_t value_bytes = 0;
src/core/flipctl.h-52-    uint64_t closed_windows = 0;
src/core/flipctl.h-53-};
src/core/flipctl.h-54-
src/core/flipctl.h:55:// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
src/core/flipctl.h-56-// frame -- one load of this writer's own word, on the line the old enabled() test already read,
src/core/flipctl.h-57-// and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
src/core/flipctl.h-58-// for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
src/core/flipctl.h-59-// that pass; at the pass end finish_parse_pass() publishes it and the caller draws the gap to the
src/core/flipctl.h-60-// next sampled pass. `work_window_` is the mean commands per fingerprint

codex
The inventory has 66 references: 65 in comments and one in a WAITAOF error string. I’ll leave that string unchanged under the comments-only constraint and flag it in the report. I also found that `AUDIT-MARKDIET.md` is missing from the supplied archive; I’m tracing its claim through the available records.

exec
/bin/bash -lc "sed -n '53,135p' /home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md
sed -n '148,259p' /home/user/Projects/lane-notes-archive/DESIGN-P0REPLY.md
sed -n '68,108p' /home/user/Projects/lane-notes-archive/NOTES-BARRIER.md
sed -n '210,249p' /home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md" in /home/user/Projects/cx-inline
 succeeded in 0ms:
## Shared terminology and publication rules

* A **physical shard** is a `Shard` object and its `FlatStore`.  The store is
  never copied or rebuilt during migration.
* A **bucket owner** is the EX thread id returned by `Router::owner_of(hash)`.
  `Router::shard_of(hash)` returns the immutable physical storage identity from
  the other half of the same packed atomic entry.
* Bucket transfers are aligned to complete physical-shard ranges.  A non-thread-
  safe `FlatStore` cannot safely be split between two EX threads.  A range can
  contain one or more complete shards, but never part of one.
* A **connection owner** is `Client::ifid_thread()`.  Epoll/io_uring registration
  follows that owner; it does not define ownership.
* All ownership fields use atomic release/acquire publication.  No code may infer
  ownership from a vector, queue, event registration, or thread role.
* A stale message is safe only because the receiver checks the current owner
  before touching the resource and forwards it when ownership differs.

## 1. Bucket transfer

### State machine

Only one bucket-range transfer can publish at a time.  The coordinator owns a
descriptor `(begin, end, source, destination, phase)`.

1. **IDLE / SOURCE OWNS** — every entry in the range resolves to `source`.
2. **PREPARE** — validate that the range is non-empty, aligned to complete
   physical shards, uniformly owned by `source`, and that source and destination
   are live EX threads.  Reserve the destination's shard-vector capacity.  No
   ownership has changed.
3. **QUIESCE SOURCE** — stop admitting matching work at the source.  Drain its
   task and borrow-release inboxes, ordered and retry queues, and any matching
   deferred work.  New stale arrivals are forwarded.  Wait until no executing
   operation, scatter group, atomic group/commit ticket, snapshot control step,
   or epoch-pinned read can touch the range.  Refusal or timeout here leaves the
   source owner unchanged.
   FLIP also scans the complete shard-pointer partition and every router bucket:
   each physical `Shard*` must occur exactly once, only on its live EX owner, and
   each bucket must name that same shard and owner. Destination capacity for the
   worst-case plan is reserved before publication.
4. **PREPARING** — publish the descriptor without changing `owner_[]`. Every
   ordinary router read remains one acquire-load and can observe only `source`.
5. **INSTALL BOOKKEEPING** — while `PREPARING` still resolves the range to source,
   move the physical `Shard*` bookkeeping from the source vector to the already
   reserved destination vector.  The vector is bookkeeping, not authority: source
   remains sole owner throughout this step.  The objects and all keys/MVCC versions
   stay put.
6. **COMMITTED / DESTINATION OWNS** — a single release-store of the descriptor
   phase is the ownership handoff. Before it, only source may touch the shards;
   after it, only destination may touch them. Only after this edge are the packed
   bucket entries and derived flat shard-owner entries release-stored to
   `destination`. A racing reader may still return `source`, which is now merely
   a safe stale route.
7. **STABLE** — after the complete bucket range and flat shard entries publish,
   clear the descriptor. Readers never inspect the descriptor in any phase.

The forwarding rule is deliberately chosen over per-request routing epochs: an
IO thread can route using a pre-flip owner read and enqueue after the handoff.
Every EX task and borrow release checks current ownership before its first shard
access.  The old owner forwards a stale item to the current owner.  Converted IO
roles are stricter: the global producer barrier and an unmasked drain prove every
EX inbox quiescent before the direct EX -> IO role edge, so no late producer can
strand work in a dormant inbox.

### In-flight cases

| In-flight state | Resolution |
| --- | --- |
| Operation queued in the losing EX inbox | Drained before publication, or ownership-checked and forwarded. |
| IO read old owner but has not enqueued | Old receiver observes the committed owner and forwards. |
| Operation executing in source | Handoff waits for the source safe point. |
| Ordered, atomic, MULTI, or retry-deferred operation | Its queue is drained; transfer cannot cut the operation/group. |
| Cross-shard scatter group | A global group-in-flight witness must reach zero before publication. |
| Atomic group with commit ticket | Both group and apply/commit witnesses must reach zero; a ticket is never divided. |
| Borrow release queued for an old owner | Receiver forwards before calling `unborrow`. |
| Epoch-MVCC read with pinned snapshot | It completes while source owns the unchanged physical shard; transfer waits for the pin/group witness, so no read crosses the edge. |
| Reader racing the multi-entry owner-array write | It returns one non-tearing packed entry: source (safe stale route) or destination; destination is never published before commit. |
| Cursor/whole-shard operation | The complete physical shard is the minimum transfer unit; ownership is sampled once and checked at execution. |

There is no intentionally unsafe case.  Whole-shard alignment is a necessary
restriction: moving only owner entries for part of one non-thread-safe store would
create two concurrent owners of that store even though buckets themselves looked
correct.

## 7. ROOT CAUSE — the block cache is NOT owner-private

The debug build names it directly, on the ordinary write path:

```
RLCACHE-VIOLATION owner: take on cache 0x75c8f6447080 from tid 3136850, owner tid 3136854

KvBlockCache::take                         kv_block_cache.h
  FlatStore::read_local_cache_take         flatstore_atomic.inc:1453
    FlatStore::make_set_string             flatstore.h:1301
      store_string / cmd_set<false>        t_string.cc:158 / 550
        ExLoopT<true>::execute<false,false> ex_loop.h:2955
          exec_batch / drain_tasks          ex_loop.h:2758 / 2100
            fused_pass_impl                 ex_loop.h:547
```

A fused thread executing an ordinary SET task reached **another fused thread's** block cache. The
cache is reached as `read_local_store_state_armed().retire_sink.block_cache` — a pointer stored on
the SHARD's store, not on the executing thread. So the identity of the cache a write uses is
whatever the shard was last bound to, and nothing on the write path checks that this is still the
thread doing the writing.

Two threads then splice one unlocked free list: a lost update leaves a block linked twice, `put`
sees itself as the head and aborts (`ceb6b02f8`), or `take` hands out a still-linked block whose
KvObj header then overwrites the list `next` and the following `take` dereferences a wild head
(`0xc000100` — the ringdiet segfault).

The same stale pointer is `retire_sink.defer`, which pushes into `ReadLocalDeferredQueue`'s
single-producer retire ring. The cache is simply where the damage shows first.

## 8. THE MECHANISM, from the event log

With the shard-transfer edge and every sink rebind logged, one reproduction says it outright
(`store 0x7f6c0c882638` is shard 1; tid-index is the thread's own id, tid is the OS thread):

```
line 148: RLBIND transfer  store ...882638 sid 1  src tid-index 1 -> dst tid-index 7
line 161: RLBIND rebind    store ...882638 cache 0x..5247080 -> 0x..4a47080  tid 3155464   (= index 7)
line 173: RLBIND transfer  store ...882638 sid 1  src tid-index 7 -> dst tid-index 1
          ... rebindall for indices 3,7,5,4,0 ... but NOT for index 1 ...
line 188: RLCACHE-VIOLATION take on cache 0x..4a47080 from tid 3155458, owner tid 3155464,
                            reached through store ...882638
```

Shard 1 moved to thread index 1 (`tid 3155458`) at line 173. Its store's sink still named thread
index 7's cache (`0x..4a47080`, bound at line 161). **Thread index 1 then executed `cmd_set` on
shard 1 through thread index 7's block cache**, and no rebind for that store appears in between.

`Server::transfer_shard_quiesced` moves the shard: the `shards()` vectors, the bucket router, and
`shard_owner_`. It does NOT move the read-local retire sink. That was left to the destination's own
later executor pass (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`, ex_loop.h
460/662), and NOTHING orders that pass before the destination's first write to the shard it has
just been given. `rebind_read_local_retire_sink`'s own comment claims it is called "before the new
owner executes store work" — that half of the sentence was never enforced.

**Control.** `--key-lb 0 --client-lb 0` removes shard moves and nothing else:

| arm                                   | runs | deaths |
|---------------------------------------|------|--------|
| armed fused, LB on (default)          |  3   |   1    |
| armed fused, LB off (`--key-lb 0`)    |  5   |   0    |

## 9. THE FIX — the sink moves with ownership

`Server::adopt_read_local_retire_sink(shard, destination)` rebinds the shard's store to the
destination thread's sink INSIDE both quiesced transfer functions, immediately before
`router_.commit_transfer()` — the same critical section that moves the shard. Both callers already
hold every executor at the quiesced safe point (`lb_commit_shard_plan` commits only once every EX
thread has acked ExDrain, and `flip_quiesced()` grants that ack only with an EMPTY retire ring), so
the source has nothing outstanding for this shard and the destination has not started. There is no
window left for the sink to disagree with ownership.

The lazy `rebind_owned_shards` path is kept; it is now always a no-op, and is the cheap standing
check that the eager rebind did its job.

| arm (64 conns, 64-key space, 40 s runs)          | runs | deaths |
|--------------------------------------------------|------|--------|
| `ceb6b02f8` pristine release                      |  2   |   1    |
| with the fix                                      |  4   |   0    |

## 10. Is there a third path? — the audit of every ownership edge

**Shard ownership moves through exactly two functions**, both quiesced, both now carrying the sink:

| edge | callers | carries the retire sink? |
|------|---------|--------------------------|
| `Server::transfer_shard_quiesced` | `lb_commit_shard_plan` (server.h), the LB mover (io_loop.h:2985) | yes, now |
| `Server::transfer_bucket_range_quiesced` | flip / role retirement (a whole executor's shards at once) | yes, now |
| `Server::set_worker_of_shard` | boot only (server.h:368) | n/a |

`shard_owner_[]` is written in exactly those three places and nowhere else.

**A SECOND per-owner pointer on the shard is still rebound lazily, and should move to the edge for
the same reason.** `Shard::notify_pending_` is a `bool*` into the OWNER's `notify_keyless_pending_`:

* the FLIP path rebinds it at a barrier — `FlipStage::ExInstall`, ex_loop.h:1832, while IO dispatch
  is still parked. **Safe.**
* the LB path rebinds it lazily, in `lb_control_pass` on the first pass after the stage ends
  (ex_loop.h:1857). **Same window the retire sink had.**

Severity is far lower and it is not the same defect: the stale pointer is written, not spliced, so
there is no corruption — a `bool` is set on the wrong thread, the old owner's drain walks only its
own shards, and the event would be stranded. It self-heals because the rebind then forces one drain
(`notify_keyless_pending_ = true`). It is a correctness wart of the same shape, not a P0, and it is
left for its own change with its own notify-battery evidence rather than bundled here.

**Client moves do not have this shape.** `IoLoop`'s migration path re-derives or explicitly hands
over every per-owner structure at the edge: the WB slot is released on the source
(`release_wb_slot`) and reassigned by the destination at adopt (`assign_wb_slot`), with
`kWbMigrationInstalling` as the sentinel that keeps early readiness inert in between; the local
catalog, the routing state and the pending pub/sub events are extracted and posted; and
`set_ifid_thread(destination)` is the single ownership edge. Nothing is left pointing at the source.
## 2. Reachability verdict — LATENT, not live

**Verdict: not reachable today.** The reason §9 gives is directionally right but incomplete; the
airtight statement is:

> **A blocking op is always alone in its ROB.**

Both halves are enforced, at two sites 47 lines apart in a different file from the clear:

* **Nothing older.** `io_loop.h:1391` — `if (spec->flags & CmdFlags::Blocking) { … if (rob.in_flight() != 0) break; }`.
  The blocking op cannot even be *prepared* until every older op of the connection has retired.
* **Nothing younger.** The same dispatch sets the barrier (`io_loop.h:1437`) and `break`s; the parse
  loop's head (`io_loop.h:1113`) and `flush_ready`'s re-parse gate (`io_loop.h:1894`) both refuse to
  parse while it is set. No byte behind the blocking frame is even looked at.

Therefore, at the instant `blocking_retire` runs, `dispatch_id - flush_id == 1` and the single live
op *is* the blocking op. The barrier's only possible owner is the one being released. Clearing
all owners and clearing one owner are the same operation.

The same argument covers **`blocking_scatter_retire`**: `state->blocking_origin` is written at exactly
one site — `blocking.inc:1203`, inside `blocking_resume_move` — which converts an *already parked*
blocking op in place (`op.detach_blocking_state(); op.attach_scatter_state(...)`). It creates no new
ROB slot, so the "alone in its ROB" property is inherited unchanged.

And **`BlockingState` cannot reach the ROB by any other route**: `attach_blocking_state` has exactly
one caller, `io_loop.h:1418`, behind the guard above. In particular a blocking command lowered inside
`MULTI` never produces one — `EXEC` children are not ROB ops and the parent `EXEC` op carries
`multi_state`, not `blocking_state`.

### Interleavings I checked and rejected

| Candidate | Why it does not fire |
|---|---|
| Pipelined `EXEC` / `SUBSCRIBE` / `CLIENT KILL` **then** `BLPOP` in one write | The first command sets the barrier and ends the pass; the `BLPOP` bytes are never parsed until the barrier drops at quiescence, by which time the first owner is done |
| Pipelined `BLPOP` **then** `EXEC` | `BLPOP` sets the barrier; `EXEC` is never parsed |
| Second owner set from **outside** `parse_and_dispatch` while a blocking op is parked | There is no such path. `blocking_resume_move` (the only thing that runs on a barred connection, `io_loop.h:1845`) does **not** acquire the barrier — it inherits the one already held. Pub/sub and climon event delivery on the io thread only ever set the barrier on the *requesting* client, never on a victim; `CLIENT KILL`/`UNBLOCK` reach a victim through `mark_closing()` / `blocking_request_unblock()`, neither of which touches `scatter_barrier_` |
| Two connections | The barrier is per-`Client` and every site writes it through the connection's own io owner. No cross-connection write exists |
| A `Blocking`-flagged command that skips the guard | Only `XREAD` without `BLOCK`, which `goto nonblocking_dispatch`es before the guard and never attaches blocking state |
| Deferred `WAIT` coexisting with a blocking op | `WAIT` is `OrderedLocal`, so it too needs `in_flight()==0`, and it too ends the pass |

### Two things this audit found that make the item worse than "latent"
### One counter, one meaning — and why the falsifier needed a second one

Two different claims are being made, and they need two counters:

* **`atomic_exec_order_holds`** (`ExLoop::execute()`, dispatch) — *the hold fired*. Non-zero by
  design in every armed run; `tests/multirace.py` and `tests/multires.py` fail a run in which it
  never advances, because such a run never entered the window.
* **`atomic_exec_order_late`** (`prepare_write_key()`, install) — *the hold fired **early
  enough***. Must read **zero**, in both atomic modes, from boot.

The install-time site asks the identical question one step later. Only the owner thread publishes
into its own pending list, so the park should foreclose it completely — and that foreclosure is
the whole acyclicity argument above. Keeping the probe is what makes that argument falsifiable.

The refinement this lane closes: those two sites were briefly **merged onto one counter**, which
made the falsifier inert. `holds` is non-zero *by design* in exactly the runs that open the window,
so a summed counter can never be checked against zero — the safety argument could have broken with
no test going red, and the batteries' non-vacuity assertion would have been satisfied by park
increments regardless. Split, each number answers exactly one question and both are asserted:
`holds > 0` (armed) and `late == 0` (always).

`atomic_exec_order_late` is a namespace-scope `std::atomic<uint64_t>` in `multi.inc`, not a
`Shard::Stats` field, because `sizeof(Shard)` is locked at 1440 and `Stats` is a by-value member —
a 20th counter would have grown it. Nothing is lost by the placement: the increment sits on a path
that is never taken, so there is no per-shard line to contend for. There is precedent for the
shape (`scripting.cc`, `slowlog.cc`, `functions.cc` all keep INFO counters this way).

The `prepare_write_key()` probe also previously excluded only `state.epoch`, so it counted a
transaction meeting **its own** cross-shard child. It now uses `multi_state_owns_epoch()`, the same
predicate as the park — without that, `late` would report a violation on every EXEC containing a
`RENAME`/`LMPOP` and the zero assertion could never hold.
`atomic_has_own_undecided()`'s `ignore_epoch` parameter had no other caller and is removed; one
epoch pointer cannot name a transaction's several words, which is the whole reason the predicate
exists.

**Effect on `tests/multires.py`.** That battery locks the same hazard window and used this counter
as its non-vacuity discriminator. The fix moves where the window is closed — from install to
dispatch — so its counter now advances at the park; its docstring said "and where an EXEC write
installs against one" and no longer does, because that arm is gone. Its three negative controls
still read zero: a foreign connection is excluded by `origin_conn_id`, a single-key `DEL` carries

exec
/bin/bash -lc "rg -n '"'^##|''^###|W3'"' /home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md /home/user/Projects/lane-notes-archive/P128.md
sed -n '20,95p' src/core/flipctl.h
sed -n '1960,2065p' src/core/server.h
sed -n '2825,2855p' src/core/server.h
sed -n '2915,2985p' src/core/server.h" in /home/user/Projects/cx-inline-comments
 succeeded in 0ms:
/home/user/Projects/lane-notes-archive/P128.md:28:## 0. The anomaly (owner's numbers, 32 cores / 64 shards / 512 conns)
/home/user/Projects/lane-notes-archive/P128.md:38:## 1. Sweep 1 recovered (16 conns/thread = 128 conns, pre-prototype binary)
/home/user/Projects/lane-notes-archive/P128.md:55:### The 2x2 discriminator already run before the kill (same binary)
/home/user/Projects/lane-notes-archive/P128.md:84:## 2. Mechanism (from the code, `src/core/io_loop.h` + `src/core/ex_loop.h`)
/home/user/Projects/lane-notes-archive/P128.md:102:### Hypotheses A-D against this
/home/user/Projects/lane-notes-archive/P128.md:115:### Rotation order (from `io_loop.h` run loop, `flush_ready`, `ex_loop.h fused_pass_impl`)
/home/user/Projects/lane-notes-archive/P128.md:128:## 3. Per-connection armed-state footprint (from the layout locks and `rob.h`)
/home/user/Projects/lane-notes-archive/P128.md:147:## 4. SHIPPED: lane admission -- defer, never demote; pressure-gated fair share; no knob
/home/user/Projects/lane-notes-archive/P128.md:205:## 6. Design item: widening `Rob<64>` would not help here (do not)
/home/user/Projects/lane-notes-archive/P128.md:220:## 5. Results with the prototype (chain4; geometry note)
/home/user/Projects/lane-notes-archive/P128.md:232:### 5.1 Depth A/B, 16 conns/thread (80 conns, 5 threads) — pass ab2
/home/user/Projects/lane-notes-archive/P128.md:279:### 5.1b The CLEAN cells (chain6 binary = quota form of arm 2; 5 threads on 40-44, memtier on
/home/user/Projects/lane-notes-archive/P128.md:325:## 5.2 The real rig (coordinator's quiet-box decomposition, 32c, 1s+RL, pure GET, 2 replicates)
/home/user/Projects/lane-notes-archive/P128.md:347:### PRE/POST table for the cells the owner sees (POST = to be measured on the real rig)
/home/user/Projects/lane-notes-archive/P128.md:360:## 7. Why pure deferral is not enough: the rotation order starves the deferred set
/home/user/Projects/lane-notes-archive/P128.md:377:### 7.1 Starvation check -- RESULT (s3i, section 9.3) and why it is structural
/home/user/Projects/lane-notes-archive/P128.md:421:## 8. Directed test: `tests/read_local_lane.py` (gate row "read-local lane admission battery")
/home/user/Projects/lane-notes-archive/P128.md:423:### 8.1 Why the row drives a CAP and not load
/home/user/Projects/lane-notes-archive/P128.md:453:### 8.2 What the row checks
/home/user/Projects/lane-notes-archive/P128.md:476:## 9. New-core harness and directional results (cores 32-39 / 160-167, ports 8200-8209)
/home/user/Projects/lane-notes-archive/P128.md:487:### 9.1 Rig truth: the 8-thread layout is loadgen-bound; 4 threads + 12 memtier threads saturate
/home/user/Projects/lane-notes-archive/P128.md:502:### 9.2 Variant A (window re-armed by its own quota deferrals; `in_flight()` share) -- superseded
/home/user/Projects/lane-notes-archive/P128.md:542:### 9.3 FINAL form (lane-full-only arming, pending-slot share): 3-arm pre / post / no-quota
/home/user/Projects/lane-notes-archive/P128.md:554:#### s3i -- 63 conns/thread x p32 = 2016 nominal vs a 1024-entry lane (2x the boundary)
/home/user/Projects/lane-notes-archive/P128.md:588:#### s3j -- 33 conns/thread x p32 = 1056 vs 1024 (marginally over the boundary): NOT RESOLVABLE
/home/user/Projects/lane-notes-archive/P128.md:609:#### Battery results on the final binary (batt3, cores 32-35+160-163, port 8204)
/home/user/Projects/lane-notes-archive/P128.md:635:### 9.4 The MUST-NOT-REGRESS cells (s3k): 9 conns/thread, final binary, 3 arms
/home/user/Projects/lane-notes-archive/P128.md:685:### 9.5 OWNER ACCEPTANCE RUN, and the footprint defect it exposed in the control cell
/home/user/Projects/lane-notes-archive/P128.md:709:#### What this lane added, and where every byte of it sits (read from the two binaries' DWARF)
/home/user/Projects/lane-notes-archive/P128.md:741:#### Fix (commit 20d449a24): gate the argument, not just the answer
/home/user/Projects/lane-notes-archive/P128.md:751:### 9.6 Verification of the lane-cap row (commit 32d99db98)
/home/user/Projects/lane-notes-archive/P128.md:773:## 10. FINAL PRE/POST
/home/user/Projects/lane-notes-archive/P128.md:781:### 10.1 Measured on the lane rig -- 4 saturated fused threads (32,33,160,161), `--shards 4`
/home/user/Projects/lane-notes-archive/P128.md:834:### 10.2 The owner's quiet box (32 cores, 64 fused threads, 1s + read-local, pure GET)
/home/user/Projects/lane-notes-archive/P128.md:854:### 10.3 The two cells I am asking the owner to run
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:10:## Verdict table
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:16:| W3 | (found while fixing W1) a cross-shard **script** pins a CLOCK_MONOTONIC value as "now", so it never sees any key as expired | **REPRODUCED — FIXED**, deterministic, 100% | `tests/expwide.py` S4 |
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:23:## What the reference does, measured rather than assumed
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:67:## The argument, stated once
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:81:## The defects
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:83:### W1. A cross-shard fan-out had a pinned commit cut and no pinned time cut
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:139:### W2. A key could expire in the middle of a transaction
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:156:### W3. A cross-shard script compared deadlines against the machine's uptime
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:177:## The fix
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:214:## Test evidence
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:216:### The battery: `tests/expwide.py`
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:275:### The gate row
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:293:### The sibling lane's shelved reproducer, after
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:307:### ASAN
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:319:### Blast radius: batteries and differ, both atomic modes
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:345:### Blast radius: interleaved A/B
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:370:## Recorded, out of scope, pre-existing
/home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md:389:## Housekeeping
class Server;

// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
// Arming derives solely from the controller latch. No runtime sampling knob or adaptive
// per-operation work is needed; the detector already derives its bands from actual samples.
inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
    return controller_enabled ? 100u : 0u;
}

enum class FlipFingerprintClass : uint8_t {
    Read = 0,
    Write,
    MultiRead,
    MultiWrite,
    AtomicGrouped,
    Blocking,
    Other,
    Count,
};

inline constexpr size_t kFlipFingerprintClasses =
    static_cast<size_t>(FlipFingerprintClass::Count);

// Monotonic cumulative publication.  A controller subtraction therefore consumes every closed
// work window even when an IO thread closes many of them between monitor beats.
struct FlipFingerprintWindow {
    std::array<uint64_t, 4> pass_depth{};
    std::array<uint64_t, kFlipFingerprintClasses> command_class{};
    uint64_t commands = 0;
    uint64_t multikey_keys = 0;
    uint64_t multikey_ops = 0;
    uint64_t value_bytes = 0;
    uint64_t closed_windows = 0;
};

// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
// frame -- one load of this writer's own word, on the line the old enabled() test already read,
// and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
// for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
// that pass; at the pass end finish_parse_pass() publishes it and the caller draws the gap to the
// next sampled pass. `work_window_` is the mean commands per fingerprint
// sample: one pass in W is sampled, so one frame in W is fingerprinted at every pipeline depth (a
// sampled pass contributes all of its d frames and costs the body d times, once per W passes).
// Gaps are uniform over [1, 2W-1] passes (mean W). A stride of exactly W would alias with any
// workload whose classes alternate with a period dividing W (memtier's 1:1 ratio is period 2);
// a uniform gap has 1 in its support, so the renewal process equidistributes over every period and
// each class is sampled in proportion. The draw is the io loop's own xorshift, taken only when a
// sampled pass ends, so the writer holds no generator state and the ThreadCtx footprint is
// unchanged (pass_frames_ became pass_countdown_; partial_.commands is the sampled pass's depth).
// W = 1 samples every pass: the exhaustive writer, byte for byte, and the unit test's oracle.
// Dark writer (work_window_ == 0: --flip-auto 0, every 1s boot): pass_countdown_ is 1 and nothing
// ever writes it, so the gate word reads false from a clean line and no other code runs.
class FlipFingerprintWriter {
public:
    void configure(uint32_t commands_per_sample) {
        work_window_ = commands_per_sample;
        // Armed: the first pass is sampled, so the controller's boot gate sees work after one pass
        // rather than one gap. Dark: the gate word must never read 0.
        pass_countdown_ = commands_per_sample ? 0 : 1;
        partial_ = {};
        published_ = {};
    }

    bool enabled() const { return work_window_ != 0; }
    uint32_t work_window() const { return work_window_; }

    // THE per-operation gate: true only inside a sampled parse pass.
    bool pass_sampled() const { return pass_countdown_ == 0; }

    // Called only inside a sampled pass, after the parsed frame has crossed its last
    // refusal/backpressure point.
    void note_command(FlipFingerprintClass command_class, uint32_t keys,
                      uint64_t value_bytes) {
        partial_.command_class[static_cast<size_t>(command_class)]++;
        partial_.commands++;
        if (keys > 1) {
#if !defined(NDEBUG) && !defined(__OPTIMIZE__)
        if (shard_id < 0 || static_cast<uint32_t>(shard_id) >= shards_.size()) std::abort();
#endif
        return shard_owner_[shard_id].load(std::memory_order_acquire);
    }
    void set_worker_of_shard(int32_t shard_id, uint32_t thread_id) {
        if (shard_id < 0 || static_cast<uint32_t>(shard_id) >= shards_.size()) std::abort();
        Shard& sh = *shards_[static_cast<uint32_t>(shard_id)];
        router_.set_initial_owner(sh.bucket_begin(), sh.bucket_end(), thread_id);
        shard_owner_[shard_id].store(thread_id, std::memory_order_release);
    }

    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
    // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
    // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
    // Vector capacity and membership are settled while PREPARING still names the source.  The
    // phase store in commit_transfer() is the sole ownership edge; bucket entries and the derived
    // shard array publish destination only after that edge.
    // THE OWNERSHIP EDGE OWNS THE SINK.
    //
    // A shard's read-local retire sink names TWO structures that belong to one thread and are
    // protected by nothing else: the owner's QSBR retire ring (`defer`, a single-producer ring) and
    // the owner's private block cache (`block_cache`, an unlocked free list). Every armed write on
    // the shard reaches both through the shard's store, so the sink must name the thread that is
    // executing that shard's writes -- at every instant, not eventually.
    //
    // Moving the shard without moving the sink leaves the DESTINATION executing writes through the
    // SOURCE's ring and free list. Two threads then splice one unlocked list: the loser's update is
    // lost, a block ends up linked twice, and the damage surfaces later as either
    // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
    // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
    // Both were observed (see DESIGN-P0REPLY.md).
    //
    // The rebind used to be deferred to the destination's own next executor pass
    // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
    // pass before the destination's first write to the shard it had just been given, and the
    // reproduction shows exactly that gap: transfer sid 1 to thread 1, then thread 1 executing
    // cmd_set on it against thread 7's cache, with no rebind in between.
    //
    // Here there is no gap. Both callers hold every executor at the quiesced safe point (the LB
    // commits only once every EX thread has acked ExDrain, which `flip_quiesced()` grants only with
    // an EMPTY retire ring), so the source has nothing outstanding for this shard and the
    // destination has not started. The sink moves with ownership, in the same critical section.
    void adopt_read_local_retire_sink(Shard& shard, uint32_t destination) {
#ifdef TOMO_RL_CACHE_NO_EAGER_ADOPT
        // NEGATIVE CONTROL (Makefile target `rlcache-nofix`): restores the pre-fix behaviour, where
        // the sink was left for the destination's own later pass to rebind. Every invariant added
        // for this defect MUST fail against this build; a detector that cannot report failure
        // proves nothing about the runs that pass.
        (void)shard; (void)destination;
        return;
#else
        if (!shard.store().read_local_enabled()) return;
        const ReadLocalRetireSink* sink = threads_[destination]->read_local_retire_sink_or_null();
        if (!sink) std::abort();
        shard.store().rebind_read_local_retire_sink(*sink);
#endif
    }

#ifdef TOMO_RL_CACHE_DEBUG
    // THE INVARIANT THAT WOULD HAVE CAUGHT THIS, stated where it can be checked cheaply and
    // continuously rather than only where it is violated.
    //
    // Every shard's read-local retire sink must name its CURRENT owner, at every instant. Checking
    // it only at the point of USE (KvBlockCache's owner assertion) makes detection depend on the
    // new owner happening to write to the moved shard inside the window, which is a coin flip per
    // move. Checking the mapping itself makes ANY move with a stale sink fire, deterministically,
    // whether or not a write lands in the window -- which is what lets a gate row rest on it.
    //
    // 16-256 pointer compares, debug builds only, once per executor pass.
    void debug_assert_read_local_sinks_follow_ownership(uint32_t checker) const {
        // Only outside a migration stage. The transfer functions publish the sink and the new owner
        // as two separate stores inside one quiesced critical section, so a thread parked at the
        // ExDrain ack can observe the instant between them; that transient is not the defect. The
        // defect outlives the stage -- the pre-fix rebind did not happen until the destination's
        // own later pass -- so checking at Idle still catches it on the very next pass.
        if (lb_stage() != LbStage::Idle || flip_stage() != FlipStage::Idle) return;
        for (uint32_t sid = 0; sid < nshards(); sid++) {
            const Shard& shard = *shards_[sid];
            if (!shard.store().read_local_enabled()) continue;
            const uint32_t owner = shard_owner_[sid].load(std::memory_order_acquire);
            if (owner >= threads_.size()) continue;
            const ReadLocalRetireSink* want = threads_[owner]->read_local_retire_sink_or_null();
            if (!want) continue;
            const ReadLocalRetireSink& have = shard.store().read_local_retire_sink_debug();
            if (have.block_cache == want->block_cache && have.context == want->context) continue;
            std::fprintf(stderr,
                "\nRLSINK-VIOLATION shard %u owner thread %u expects cache %p/queue %p, store %p "
                "still names cache %p/queue %p (observed by thread %u)\n",
                sid, owner, static_cast<void*>(want->block_cache), want->context,
                static_cast<const void*>(&shard.store()),
                static_cast<void*>(have.block_cache), have.context, checker);
            std::fflush(stderr);
            std::abort();
        }
    }
#endif

    bool transfer_bucket_range_quiesced(uint32_t begin, uint32_t end, uint32_t source,
                                         uint32_t destination) {
        if (begin >= end || end > kNumBuckets || source >= threads_.size() ||
            destination >= threads_.size() || source == destination) return false;
        if (!live_executor(source) || !live_executor(destination))
            return false;

        // FlatStore is the lock-free physical ownership unit. Accept a range of one or more whole
    // TEST HOOK (DEBUG ATOMIC-DIRECT-DEFER). Number of extra owner passes a cross-shard RENAME's
    // destination task is held after its source hop is ready. Zero in production; its only reader
    // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
    // other command. It widens -- deterministically -- the window in which a younger whole-owner
    // walker could overtake an older same-connection group on the destination shard.
    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
    // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
    // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
    // its masking, so this can never overrun anything. It exists because lane oversubscription is
    // otherwise a RATE RACE the gate cannot win -- a test client cannot push 1024 frames into one
    // fused thread faster than that thread drains them, so the anti-vacuity row of the lane battery
    // could only fire on a saturated rig. With a cap, a single connection pipelining more than the
    // cap in ONE parse pass fills the lane inside that pass, before any drain can run, which makes
    // the row deterministic at gate scale. Read once per rotation by each fused thread, never per
    // op. Reachable only through the already-gated DEBUG command.
    uint32_t debug_read_local_lane_cap() const {
        return debug_read_local_lane_cap_.load(std::memory_order_relaxed);
    }
    void set_debug_read_local_lane_cap(uint32_t cap) {
        debug_read_local_lane_cap_.store(cap, std::memory_order_relaxed);
    }
    uint32_t debug_atomic_direct_defer() const {
        return debug_atomic_direct_defer_.load(std::memory_order_relaxed);
    }
    void set_debug_atomic_direct_defer(uint32_t passes) {
        debug_atomic_direct_defer_.store(passes, std::memory_order_relaxed);
    }
    // TEST HOOK shared by DEBUG ATOMIC-COMMIT-DELAY and ATOMIC-OFF-HOP-DELAY. Atomic ON stalls a
    // commit batch between its ticket reservation and the decision stores (read by
    // atomic_commit_batch(), a cold group/batch path); atomic OFF parks non-lead owners at the
    // first cross-owner mutation wave. Both aliases write this one word, so last writer wins and
    void set_debug_script_stage_defer(uint32_t microseconds) {
        debug_script_stage_defer_.store(microseconds, std::memory_order_relaxed);
    }
    uint32_t debug_script_stage_defer() const {
        return debug_script_stage_defer_.load(std::memory_order_relaxed);
    }
    // TEST HOOK (DEBUG ATOMIC-READ-DELAY). Microseconds a plain read is held on its owner before
    // it resolves, widening the gap between the IO-side dispatch of a pipelined read and its
    // execution. That gap is the session-monotonicity window: foreign commits landing inside it
    // used to make the earlier command answer with a newer world than the later one.
    void set_debug_atomic_read_delay(uint32_t microseconds) {
        debug_atomic_read_delay_.store(microseconds, std::memory_order_relaxed);
    }
    uint32_t debug_atomic_read_delay() const {
        return debug_atomic_read_delay_.load(std::memory_order_relaxed);
    }
    // TEST HOOK (DEBUG BARRIER-HOLD). While armed, every blocking dispatch pins a SECOND owner
    // (BarrierOwner::Debug) on its connection's parse barrier; flush_ready drops that bit again as
    // soon as the latch is cleared. Zero in production, and read at exactly two already-cold
    // places: the blocking dispatch arm, and the arm of flush_ready that only runs when a barrier
    // is already set. Nothing on GET/SET touches it.
    //
    // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
    // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
    //
    // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
    // nothing left to do and parks on submit_and_wait(1) -- unbounded under io_uring. A hold that
    // expired on a clock would therefore never be noticed, and the connection would hang instead
    // of resuming. The releasing edge has to be an EVENT. The test clears this latch from a second
    // connection and then wakes every io thread through an existing fan-out (CLIENT LIST), which
    // is what makes the resume observable rather than theoretical.
    void set_debug_barrier_hold(uint32_t armed) {
        debug_barrier_hold_.store(armed, std::memory_order_relaxed);
    }
    bool debug_barrier_hold_armed() const {
        return debug_barrier_hold_.load(std::memory_order_relaxed) != 0;
    }
    // TEST HOOK (DEBUG BLOCKING-TIMEOUT-REAP). One blocking retirement consumes the arm and makes
    // its owner's client cron due in that same IO pass. This makes the otherwise probabilistic
    // ordering between reply retirement and the 100 ms timeout beat deterministic.
    void set_debug_blocking_timeout_reap(bool armed) {
        debug_blocking_timeout_reap_.store(armed, std::memory_order_release);
    }
    bool debug_blocking_timeout_reap_take() {
        if (!debug_blocking_timeout_reap_.load(std::memory_order_acquire) ||
            !debug_blocking_timeout_reap_.exchange(false, std::memory_order_acq_rel))
            return false;
        debug_blocking_timeout_reaps_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    uint64_t debug_blocking_timeout_reaps() const {
        return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
    }
    // An overlap: some owner took the parse barrier while another owner already held it. With
    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
    // live and the owner-scoped release in blocking_retire() became load-bearing rather than
    // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
    // own positive control: a "must be zero" reading proves nothing until something has been shown
    // able to make it non-zero.
    void note_barrier_overlap() {
        barrier_owner_overlaps_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t barrier_owner_overlaps() const {
        return barrier_owner_overlaps_.load(std::memory_order_relaxed);
    }
    // A blocking retirement released its own claim and the barrier STAYED UP because another owner
    // still held it. This is the instruction the old unconditional clear got wrong, so a validation
    // run that never moves this counter never reached the geometry it claims to cover -- it must

exec
/bin/bash -lc "sed -n '1,190p' /home/user/Projects/lane-notes-archive/DESIGN-RINGDIET.md
sed -n '1,150p' /home/user/Projects/lane-notes-archive/DESIGN-flipfp.md
sed -n '148,175p' /home/user/Projects/lane-notes-archive/DESIGN-flipfp.md" in /home/user/Projects/cx-inline
 succeeded in 0ms:
# DESIGN-RINGDIET.md — arm the RYOW write ring on demand

**Status: implemented exactly as specified below. The design was fixed before the lane opened;
this file states it, and the sections after it are the proof.**

## The measured fact this answers

The ring-sizing merge (`b0335c239`, *"RYOW ring sized to the ROB window"*) versus the commit before
it, on the owner's box — 512 connections, 64 shards, pipeline depth 32, fused with read-local
armed, 8 interleaved runs per arm, idle box:

| cell | delta | instructions/op |
|---|---|---|
| pure SET | **−1.32%** | **+87** |
| pure GET | +0.80% | — |
| 1:1 mix | −0.08% | — |

The ring lane's own numbers: restoring correct bookkeeping after an overflow costs **+43
instructions per write**, and the armed sidecar costs **+972.8 bytes per connection**.

### Mechanism

The old sixteen-entry RYOW ring **overflowed** on a pure-write stream at depth 32 — and then
stopped bookkeeping entirely. It was cheap for precisely the wrong reason: it gave up. Sizing the
ring to the reordering window (`kRobWindow`, 64) made overflow unreachable, so **every** write is
now recorded — including on connections where no read will ever consult the record.

Main commands are zero-regression by law, and pure SET is a main command.

## The design

### (1) Arm on demand

A connection records its in-flight writes **only after a local read has armed it**.

Until armed:

* a write's entire bookkeeping is **one store of its ROB id** into `read_local_unarmed_write_id_`,
  a word on the producer's own cache line that `dispatch_` has already dirtied. No sidecar, no
  prune, no descriptor, no `Staged` tag — and therefore no resolve on the following frame either;
* a read that arrives **while writes are in flight must take the OWNER path** (demote). Read-your-
  own-writes stays exact: a read and a write of one key share one owner queue, so the owner path
  preserves per-key order by construction. RYOW may be held back only on an explicit key conflict,
  and demotion is strictly stronger than the ring, not weaker.

The arming state lives on a cache line the parse path already owns (the `dispatch_` line, in the
padding that already held `read_local_write_valid_/wide_/force_`), and the check is a **single
predictable test on that line** — `read_local_arm_state_ != 0`, zero in the state every connection
ends up in. **Nothing is evaluated behind it**: the gate-the-argument law. A dormant mechanism that
pulls one cold line per pass cost 0.345 fills per op and 3.8% on a control cell earlier this week,
so the gate word had to be one this frame's `acquire_read_local` has already pulled into L1.

#### The arming transient, and why it is not the overflow generation

The writes a connection published *before* arming are described by nothing. So the read that arms
the connection, and every read behind it, is demoted until the **newest of those writes retires** —
which, retirement being in order, retires all of them. The transient is bounded by one ROB drain.

The obvious implementation reuses the ring's conservative **overflow generation** for that fence.
That is wrong, and silently so: an overflow generation is *extended* by every write published while
it is live (`read_local_resolve_pending_body`), so on a 1:1 connection — which always has a write in
flight — it would never end, and not one read would ever be served locally again. The arming fence
is fixed at arming and is **never extended**; writes published after arming take ordinary ring slots
underneath it and are exact the moment it lifts. `tests/read_local_write_ring_unit.cc` case 16 is
that assertion.

### (2) Sidecar on first write

The 1216-byte `ReadLocalRobState` (a 1280-byte jemalloc class; the +972.8 B/connection of the
measured fact) is allocated **at the first write of an armed connection — never at accept**. A
pure-write connection, a pure-read connection and an idle connection each carry none of it.

Arming itself allocates nothing, which is what keeps a read-only connection free: the transient's
fence is a Rob word, not sidecar state. Allocation failure at the first armed write falls back to
the unarmed contract — always safe, and self-healing, because the next quiescent read re-arms and
the next write retries.

The MGET latest-read fence moved out of the sidecar into the Rob for the same reason: it is a
**read**-side fence, and a pure-MGET connection must not have to allocate 1216 bytes of write ring
to hold one id. It is also cheaper where it now lives — `local_mget_fence_pending()` runs once per
armed parse pass and no longer dereferences the heap.

### What did not change

* `Rob<64>` is still **192 bytes**: the four new producer-line words (`read_local_arm_state_`,
  `read_local_unarmed_write_id_`, `local_mget_fence_id_`, `read_local_arm_stats_`) fit the padding
  `dispatch_` already owned, and `local_mget_fence_id_` arrived from the sidecar, not from nowhere.
* `ReadLocalRobState` is still **1216 bytes**.
* The ring itself, its tag mirror, the overflow machinery, the derivation `static_assert`
  (`kWriteRingCapacity >= Capacity`) and both kept capacity fallbacks are untouched. This lane
  changes *when* the ring is used, never *how*.
* `op->mark_read_local_precise_write()` — and therefore the EX-side eviction guard — fires exactly
  where it fired before. The unarmed `refine_*` calls return the answer an unfilled ring returns,
  because the caller uses that answer to decide a property of the *command's keys*, which ring
  capacity never had a say in.
* No knob. Arm-on-demand is the only behaviour; there is nothing to turn off.

## Counters (INFO, `# Readlocal`)

| field | meaning |
|---|---|
| `read_local_arms` | connections a local read armed |
| `read_local_write_ring_sidecars` | RYOW sidecars allocated (never at accept) |
| `read_local_write_ring_records` | writes committed into a ring — **0 for a pure-write stream** |
| `read_local_fallback_arm_transient` | reads demoted by the arming transient, reported apart from `read_local_fallback_inflight_write` (a steady-state key conflict) so a bench can never confuse the two |

Every increment sits behind a `noinline` call the unarmed stream never makes, so the counters are
always on rather than diagnostic-only. The block is per IO thread; the ROB holds a pointer to it,
installed at `adopt_client` and re-pointed at the migration ownership edge, and **nullable** —
`adopt_client` runs in split mode too, where a thread has no read-local state at all.

---

# Results

**Geometry.** Lane cores: server on physical 40,41; load generator on 42,43 + SMT siblings 170,171.
`--thread-mode fused --read-local 1 --shards 64`, memtier `--pipeline=32`, 512 connections
(`-t 4 -c 128`) or 2048 (`-c 512`), 64-byte values, keyspace pinned to dbsize (200k) so GET hit rate
is 100%. Three rounds, visits interleaved **A B B A** within each round, one fresh boot and preload
per visit. Instructions, cycles and fills come from `perf stat -C` over a 9-second window starting
4 s into each 14-second run; operations in that window come from the server's own
`total_commands_processed` delta, so instructions-per-op is a ratio of two quantities measured over
the same window rather than a rate multiplied by a duration.

**The same-binary null ran first, on the same cells**, and it is printed under every row. It is what
says which of these numbers are real. It also condemned two cells outright: on pure GET and on split
SET the two-core server was **not saturated** — its cores idled down to a 2.40 GHz-equivalent
against 3.05 on the saturated cells, and the same binary against itself moved 28% on GET. Those two
cells were re-run in a geometry that saturates (server shrunk to ONE physical core, load generator
given three), where the GET null is +0.07% / −0.19% / +0.03%. The GET row below is that re-run; the
split row is the original, whose instruction null is the tighter of the two.

**Interference.** Another lane ran an UNPINNED `make -j8` during these runs and its `cc1plus`
children landed on cores 40-43 and 168-171. Visits that lost their cores mid-window measure the
interference, not the binary, so one objective rule was applied identically to both arms and is
reported: a visit whose rate collapsed below 60% of its cell's two-arm median is dropped (6 of 72 in
the main pass, 6 of 24 in the supplementary). The `n=` counts below are the survivors. Note that
**instructions per op is immune to this**: a stolen core scales the instruction count and the op
count together, and the pure-SET arms read 3054/3055/3054/3057/3055 against 2900/2902/2901/2903
across every round, corrupted visits included.

    PRE = mainline ceb6b02f8 | POST = t-ringdiet | median of the valid ABBA visits
    cell                                  M ops/s   instr/op    cyc/op      IPC   ccx/kop  dram/kop
    -----------------------------------------------------------------------------------------------
    pure SET  p32 512c  fused armed         2.690     3055.0    2242.5    1.362     16.51  13900.43
                                            2.713     2901.4    2224.7    1.304     18.16  13530.95
                                           +0.83%     -5.03%    -0.79%   -4.27%   +10.00%    -2.66%   <- delta
                                           +0.09%     -0.03%    +0.33%   -0.34%    +0.52%    -0.73%   <- SAME-BINARY NULL (n=6/6)
    
    pure GET  p32 512c  fused armed         1.228     3004.9    2489.6    1.207     18.87  13917.78
                                            1.270     2972.8    2396.5    1.237     17.05  13825.09
                                           +3.46%     -1.07%    -3.74%   +2.53%    -9.65%    -0.67%   <- delta
                                           +0.07%     -0.19%    +0.03%   -0.15%    +6.23%    -0.18%   <- SAME-BINARY NULL (n=5/6)
    
    1:1 alternating p32 512c                2.196     4526.5    2766.0    1.642     21.78  12366.74
                                            2.215     4582.8    2740.3    1.679     21.36  12288.63
                                           +0.85%     +1.24%    -0.93%   +2.23%    -1.92%    -0.63%   <- delta
                                           -0.29%     +2.92%    +0.26%   +1.12%    +2.14%    -2.06%   <- SAME-BINARY NULL (n=6/6)
    
    blocked 10:10  p32 512c                 2.179     4712.7    2775.6    1.693     18.75  11776.70
                                            2.171     4714.0    2788.8    1.690     18.43  11773.31
                                           -0.38%     +0.03%    +0.47%   -0.17%    -1.70%    -0.03%   <- delta
                                           +0.69%     +0.65%    -1.08%   +1.00%   -23.46%    +0.80%   <- SAME-BINARY NULL (n=5/5)
    
    1:1 p32 2048c (footprint)               1.026    10063.2    5662.5    1.780     37.10  20984.38
                                            1.032     9938.4    5614.8    1.766     48.15  20179.91
                                           +0.55%     -1.24%    -0.84%   -0.79%   +29.76%    -3.83%   <- delta
                                           +0.25%     -0.26%    -0.07%   +0.23%    +0.24%    -1.80%   <- SAME-BINARY NULL (n=5/6)
    
    pure SET p32 512c SPLIT (off path)      2.922     2652.6    1989.9    1.330     12.12   8545.40
                                            2.913     2654.4    1981.8    1.333     20.48   8442.51
                                           -0.32%     +0.07%    -0.41%   +0.26%   +68.88%    -1.20%   <- delta
                                           -5.62%     -1.82%    +7.01%   -8.77%   -33.02%   +19.87%   <- SAME-BINARY NULL (n=5/4)
    
    
    INDEPENDENT CONFIRMATION on the FINAL rebuilt binary (1 round ABBA, quieter box)
    cell                                  M ops/s   instr/op    cyc/op
    ------------------------------------------------------------------
    pure SET  p32 512c  fused armed        +1.04%    3055.2->2901.5    -1.07%
    pure GET p32 512c fused armed          +0.12%    2389.0->2329.0    -1.39%
    1:1 alternating p32 512c               +1.21%    4503.4->4481.2    -1.05%
    blocked 10:10 p32 512c                 +1.20%    4730.3->4773.7    -1.12%
    1:1 p32 2048c                          +0.97%    9990.3->9868.9    -0.35%

## Verdict per cell

| cell | requirement | result |
|---|---|---|
| pure SET, fused armed | recover the −1.32% / +87 instr | **−153.6 instr/op (−5.03%), −0.79% cyc/op, +0.83% rate**, against an instruction null of ±0.03%. Not merely recovered: the ring's per-write bookkeeping is gone from the stream entirely, which is more than the sizing merge had added. |
| pure GET, fused armed | flat | **−1.07% instr/op, −3.74% cyc/op, +3.46% rate** (null +0.07/−0.19/+0.03). Better than flat — a pure-read connection now allocates no sidecar at all. |
# DESIGN-flipfp -- the flip fingerprint leaves the per-op hot path

> Historical research record: configuration recipes and measurements below describe earlier
> revisions. Deleted controls are not accepted by the merged server, and the manual-map
> dispatch-scaling instrument is retired. For current controls and merge dispositions, see
> [DESIGN-KNOBS.md](DESIGN-KNOBS.md) and [MERGE.md](MERGE.md).

Lane t-flipfp, base e2ef7a155 (train 11). Lever I2 of CYCLEMAP.md: `flip_fingerprint_note` ran its
full body on every dispatched frame -- 63 (1T) to 86 (2T) instr/op and 8.6% of the write path's
store-queue-full stall -- for a detector whose only reader is off in the shipped config.

## 1. What the fingerprint computes and who reads it

**Writer** (`FlipFingerprintWriter`, flipctl.h; body `flip_fingerprint_note`, io_loop.h). At each of
the 15 dispatch sites in `parse_and_dispatch` (13 cold: pubsub, FLIP, DEBUG SLEEP, WAIT, io-local
commands, three scatter paths; 2 hot: the fused local-read lane and the ordinary point dispatch) the
body computes, per frame: `keys` (spec first/last/step against argc), `value_bytes` (an argv loop
summing the lengths of every non-key argument), and a class in {Read, Write, MultiRead, MultiWrite,
AtomicGrouped, Blocking, Other}; then `note_command` performs four to five read-modify-write stores
into `partial_` (class bucket, commands, value_bytes, multikey keys/ops) plus `pass_frames_++`.
`finish_parse_pass` (the two exits of `parse_and_dispatch`) buckets the pass depth (1 / <=4 / <=16 /
more) and, once `partial_.commands >= flip_work_window` (default 100), adds the window into the
cumulative `published_` and bumps `closed_windows` as the last store.

**Readers.** Exactly one: `FlipController::sample_fingerprint` (flipctl.cc:235), once per controller
tick (`--lb-tick-ms`, 1 s), which sums the deltas of every thread's `published_` since the previous
tick into ONE aggregate and feeds it to `FlipShiftDetector::observe`. The controller's decision unit
is therefore the tick aggregate, not the 100-command window; the window is only the publication
granularity. `flip_signature` is scale-free -- normalized class and depth fractions,
value_bytes/commands, multikey_keys/multikey_ops -- and the absolute `commands` count enters only
the band's quantum floor `4/K`. Two side uses: `boot_load_stable` will not start the boot maneuver
until at least one window has closed (`boot_work_observed_`), and Settling needs one tick with a
closed window before anchoring. INFO FLIPCTL exposes trigger counts and the learned bands, not the
counters; DEBUG FLIPCTL dumps controller state. **Nothing reads `published()` when `--flip-auto 0`
or in 1s mode (which refuses `--flip-auto`)** -- the config.h note "so DEBUG can inspect its detector"
describes a reader that does not exist.

## 2. Design

**(a) Gate -- the dark writer.** The writer is armed only when its reader exists: `ThreadCtx::init`
receives `flipctl_.enabled() ? cfg.flip_work_window : 0` (server.h), the same predicate that enables
the controller. The per-op gate is `FlipFingerprintWriter::pass_sampled()` = `pass_countdown_ == 0`:
one load of a thread-private word on the line today's `enabled()` test already read, one
predicted-false branch, no store. Dark writer: the word is initialised to 1 and never written, so the
line stays clean in L1. No new field, no new line, no knob; `flip_work_window` keeps its
grammar (0 = off) and its CONFIG GET value.

**(b) Sample -- whole parse passes, 1 in W.** When armed, the body runs for every frame of a
*sampled* pass and for no frame of any other pass. Passes are sampled with gaps drawn uniformly from
[1, 2W-1] passes (mean W, W = `flip_work_window`), the draw taken from the io loop's own xorshift
only at the end of a sampled pass (nothing evaluated eagerly). Why passes rather than ops: the hot
point paths count commands on the executor side, so no io-side per-op counter exists that all 15
sites already increment (`sig.ops` misses the eight io-local sites -- PING among them); a per-op
countdown would cost the very store this lane removes; and a pass-granular sample keeps the body
and its statistics byte-for-byte what they are today -- a sampled pass publishes exactly what the
old writer would have accumulated for that pass (classes, keys, bytes, and its depth bucket).
Why uniform gaps with 1 in the support: an every-Wth stride aliases with any workload whose
classes alternate with a period dividing W (memtier's 1:1 ratio is period 2); a renewal process
whose gap support has gcd 1 equidistributes over every period, so every class is sampled in
proportion.

**Arithmetic.** W = `flip_work_window` (100), d = frames per pass, C = commands/s over the io
threads, T = the tick (1 s), B = body cost (63-86 instr, 5 stores).

| | PRE | POST |
|---|---|---|
| per-op work | B + 5 stores, every frame | 1 load + 1 branch; body on 1/W of frames |
| amortised instr/op | 63-86 | B/W + ~2 = 2.6-2.9 |
| stores/op | 5 | 5/W = 0.05 |
| samples per decision (tick) | K = C·T | K = C·T / W |
| publications per tick | C·T / 100 | C·T / (W·d) |
| commands per fingerprinted command | 1 | W -- the knob's documented meaning, at any depth |
| signature expectation | s | s (scale-free: a 1/W sample of a stationary stream has the same signature; unit row) |
| sampling noise on a class fraction p | sqrt(p(1-p)/K) | sqrt(p(1-p)/K_eff), K_eff in [K/d, K] (heterogeneous .. homogeneous connections) |
| band | 2·max(jitter, 4/K) | 2·max(jitter, 4/K): learned from the sampled stream, so it already contains the sampling noise |

The window-length x sample-rate identity: a sampled pass contributes d frames once per W passes,
i.e. W·d commands elapse per publication and d of them are fingerprinted -- the same 1/W of the
work at every pipeline depth, and the same body per fingerprinted frame. Accuracy comes from the
window: at the box (C ~ 16-18M/s, p32) K = 170k samples per tick, sigma_distance = 0.25/sqrt(K_eff)
= 0.0006 (0.0034 in the fully heterogeneous worst case) against real mix shifts of >= 0.025 (10% of
one class family). In the gate row (BITCOUNT at ~18k/s, p1) the signature is deterministic (one
class, depth 1, no values), so sampling adds no variance and the BITCOUNT -> INCR shift is distance
> 0.25 against a 2% band. The one behavioural difference: publication is per sampled pass, so at
fewer than W passes per tick (a trickle at deep pipelines) the fingerprint is observed every ~W
passes instead of every W commands; the first pass after boot is always sampled (the boot gate sees
work within one pass), and the rate detector, which reads the unsampled `total_commands`, is
unchanged. W = 1 is exhaustive sampling -- today's behaviour exactly -- and is the equivalence oracle
in the unit test.

**(c) What the sample costs the band, stated exactly.** The band arithmetic above hides a term, so
here it is in full. `FlipShiftDetector::update_band` (flipctl.cc) is

    band = 2 * max(jitter, 4 / max(commands, 1))          // commands = the TICK aggregate's count

and `commands` is exactly the quantity this lane divides by W. Verified in the code, not assumed:
`FlipController::sample_fingerprint` (flipctl.cc:235) sums each thread's `published_` **delta since
the previous tick** into one aggregate, skips a thread whose `closed_windows` did not advance, and
calls `shift_detector_.observe(aggregate)` **once per tick**; `maneuver_signature_samples_` and
`anchor_signature_samples_` count those ticks. So the decision window is the tick T, never the
100-command publication window, and

    K = commands per decision = C * T   (PRE)   ->   C * T / W   (POST)
    quantum floor of the band = 8 / (C*T)  (PRE)  ->  8*W / (C*T)  (POST)

Two regimes follow, and they are the thing to measure rather than argue:

- **Jitter-dominated** (`jitter > 4/K`, the busy case). The band is the anchor's own quiet jitter,
  learned from whichever stream the detector saw. The sampled stream's jitter is the exhaustive
  stream's jitter plus the sampling noise sigma = 0.25/sqrt(K_eff), so the band widens by however
  much noise the sample actually adds and keeps rejecting exactly what it rejected before. This is
  self-correcting: the band is learned from, and applied to, the same sampled stream. On this box's
  accuracy cell (C ~ 350k/s, W = 100) the PRE band was 2.4e-3 against a PRE floor of 2.3e-5 -- 100x
  above the floor -- and the POST floor lands at 2.3e-3, i.e. right at the jitter the exhaustive
  stream already had. Whether the band actually moves is S2's measurement.
- **Quantum-dominated** (`jitter < 4/K`, the quiet/low-rate case). The floor rises by W outright.
  This is not a defect but the honest resolution of a 1/W sample: a sample of K/W commands cannot
  resolve a mix change finer than one sampled command in it, and a floor that claimed otherwise
  would trigger on its own sampling noise. It does cost sensitivity, and the loss is a factor of W
  in the smallest mix shift a *quiet* anchor can name.

**The fixed-band exposure.** `configured_band_ > 0` (`--flip-auto-band PERCENT`) returns the percent
directly and never consults the floor, so in that mode the sampling noise is not absorbed by a
wider band -- it must simply fit under the configured percent. That is the one place a 1/W sample
can turn into a false trigger, and it is precisely what the `tests/flipctl.py` gate row exercises:
band 2, a jittery stationary 6:2 load, and a 30 s hold that fails on any trigger. At that row's
rate the per-tick sample is small, so the row is the binding accuracy test of this lane and is run
three times per arm.

## 3. Footprint

`pass_frames_` (u32) becomes `pass_countdown_` (u32): the writer stays 264 bytes, ThreadCtx stays
1408, `partial_.commands` is the sampled pass's frame count (the pass publishes and resets). Op,
Client, Shard and Config are untouched.

## 4. Proof plan (results in the lane report)

Same-binary null; PRE/POST instr/op, cyc/op, IPC and store-queue-full cycles by the cyclemap slope
method (replay + perf stat, matched op stream) on armed 1s SET p32 (1T, 2T), 2s SET p32 and GET
(must be unchanged), all `--flip-auto 0`; always-on cost with `--flip-auto 1` PRE vs POST at
matched rate; accuracy: the flipctl.py gate row, the wrong-split boots 3:1 (4 threads) and the
28:4 analogue on this lane's cores with the flipguard reference (the fingerprint must still let
the controller move; `--flip-auto 0` must not), anchored `flipctl_signature_band` PRE vs POST and
false-trigger count on a heterogeneous-connection mix; unit rows for the gap arithmetic, the
aliasing case, the W = 1 equivalence and the quantum floor; batteries in both modes; differ; gate
quick.

## 5. Results (base ceb6b02f8, arms differ by this change only)

Arms: PRE = mainline ceb6b02f8 (`79651da57b14ac1b`), POST = this branch (`71b2854a7599ee8a`), both built with
## 5. Results (base ceb6b02f8, arms differ by this change only)

Arms: PRE = mainline ceb6b02f8 (`79651da57b14ac1b`), POST = this branch (`71b2854a7599ee8a`), both built with
`taskset -c 48-51,176-179 make -j8` in their own worktree. Method: cyclemap slope (replay, N1=300k /
N2=900k ops per connection, so every figure is marginal cost per op at matched rate, not an average
inflated by idle spin). Armed (`--atomic 1`), p32, ring 4096, 2 reps; spread is [min..max]/2.
`--flip-auto 0` throughout the perf table -- the shipped configuration, in which PRE runs the body on
every dispatched frame and POST does not run it at all.

### 5.1 The lever, against its own same-binary null

| cell | metric | PRE | POST | delta | NULL (PRE vs PRE) | verdict |
|---|---|---:|---:|---:|---:|---|
| 1s 1 thread, SET | instr/op | 3198 | 3096 | **-102** | +8 | 12x the null |
| 1s 1 thread, SET | cycles/op | 1084 | 1059 | -24 | -17 | within the null |
| 1s 2 threads, SET | instr/op | 3246 | 3153 | **-94** | +27 | 3.5x the null |
| 1s 1 thread, GET | instr/op | 2240 | 2146 | **-94** | -0 | null is zero |
| 1s 2 threads, GET | instr/op | 2276 | 2193 | **-83** | -1 | null is zero |
| 2s split, SET | instr/op | 3217 | 2989 | **-228** | -5 | 46x the null |
| 2s split, SET | cycles/op | 1451 | 1336 | **-115** | +23 | opposite sign to the null |
| 2s split, SET | store-queue-full cyc/op | 239 | 193 | **-45** | +8 | opposite sign to the null |
| 2s split, SET | driver ops/s | 4.50M | 4.87M | **+8.1%** | -1.8% | opposite sign to the null |
| 2s split, GET | instr/op | 3000 | 2808 | **-191** | -4 | 48x the null |
| 2s split, GET | cycles/op | 1398 | 1311 | **-86** | +8 | opposite sign to the null |
| 2s split, GET | store-queue-full cyc/op | 258 | 227 | **-30** | -1 | 30x the null |

The null is the instrument's own answer, and it is what makes the table readable: **instr/op is the
trustworthy metric** (null +8 / +27 / -0 / -1 / -5 / -4), while at 1 fused thread cycles/op and

exec
/bin/bash -lc "sed -n '53,129p' /home/user/Projects/lane-notes-archive/NOTES-MIGRATE.md
sed -n '148,225p' /home/user/Projects/lane-notes-archive/DESIGN-P0REPLY.md
sed -n '68,106p' /home/user/Projects/lane-notes-archive/NOTES-BARRIER.md
sed -n '210,237p' /home/user/Projects/lane-notes-archive/NOTES-MULTIRACE.md" in /home/user/Projects/cx-inline
 succeeded in 0ms:
## Shared terminology and publication rules

* A **physical shard** is a `Shard` object and its `FlatStore`.  The store is
  never copied or rebuilt during migration.
* A **bucket owner** is the EX thread id returned by `Router::owner_of(hash)`.
  `Router::shard_of(hash)` returns the immutable physical storage identity from
  the other half of the same packed atomic entry.
* Bucket transfers are aligned to complete physical-shard ranges.  A non-thread-
  safe `FlatStore` cannot safely be split between two EX threads.  A range can
  contain one or more complete shards, but never part of one.
* A **connection owner** is `Client::ifid_thread()`.  Epoll/io_uring registration
  follows that owner; it does not define ownership.
* All ownership fields use atomic release/acquire publication.  No code may infer
  ownership from a vector, queue, event registration, or thread role.
* A stale message is safe only because the receiver checks the current owner
  before touching the resource and forwards it when ownership differs.

## 1. Bucket transfer

### State machine

Only one bucket-range transfer can publish at a time.  The coordinator owns a
descriptor `(begin, end, source, destination, phase)`.

1. **IDLE / SOURCE OWNS** — every entry in the range resolves to `source`.
2. **PREPARE** — validate that the range is non-empty, aligned to complete
   physical shards, uniformly owned by `source`, and that source and destination
   are live EX threads.  Reserve the destination's shard-vector capacity.  No
   ownership has changed.
3. **QUIESCE SOURCE** — stop admitting matching work at the source.  Drain its
   task and borrow-release inboxes, ordered and retry queues, and any matching
   deferred work.  New stale arrivals are forwarded.  Wait until no executing
   operation, scatter group, atomic group/commit ticket, snapshot control step,
   or epoch-pinned read can touch the range.  Refusal or timeout here leaves the
   source owner unchanged.
   FLIP also scans the complete shard-pointer partition and every router bucket:
   each physical `Shard*` must occur exactly once, only on its live EX owner, and
   each bucket must name that same shard and owner. Destination capacity for the
   worst-case plan is reserved before publication.
4. **PREPARING** — publish the descriptor without changing `owner_[]`. Every
   ordinary router read remains one acquire-load and can observe only `source`.
5. **INSTALL BOOKKEEPING** — while `PREPARING` still resolves the range to source,
   move the physical `Shard*` bookkeeping from the source vector to the already
   reserved destination vector.  The vector is bookkeeping, not authority: source
   remains sole owner throughout this step.  The objects and all keys/MVCC versions
   stay put.
6. **COMMITTED / DESTINATION OWNS** — a single release-store of the descriptor
   phase is the ownership handoff. Before it, only source may touch the shards;
   after it, only destination may touch them. Only after this edge are the packed
   bucket entries and derived flat shard-owner entries release-stored to
   `destination`. A racing reader may still return `source`, which is now merely
   a safe stale route.
7. **STABLE** — after the complete bucket range and flat shard entries publish,
   clear the descriptor. Readers never inspect the descriptor in any phase.

The forwarding rule is deliberately chosen over per-request routing epochs: an
IO thread can route using a pre-flip owner read and enqueue after the handoff.
Every EX task and borrow release checks current ownership before its first shard
access.  The old owner forwards a stale item to the current owner.  Converted IO
roles are stricter: the global producer barrier and an unmasked drain prove every
EX inbox quiescent before the direct EX -> IO role edge, so no late producer can
strand work in a dormant inbox.

### In-flight cases

| In-flight state | Resolution |
| --- | --- |
| Operation queued in the losing EX inbox | Drained before publication, or ownership-checked and forwarded. |
| IO read old owner but has not enqueued | Old receiver observes the committed owner and forwards. |
| Operation executing in source | Handoff waits for the source safe point. |
| Ordered, atomic, MULTI, or retry-deferred operation | Its queue is drained; transfer cannot cut the operation/group. |
| Cross-shard scatter group | A global group-in-flight witness must reach zero before publication. |
| Atomic group with commit ticket | Both group and apply/commit witnesses must reach zero; a ticket is never divided. |
| Borrow release queued for an old owner | Receiver forwards before calling `unborrow`. |
| Epoch-MVCC read with pinned snapshot | It completes while source owns the unchanged physical shard; transfer waits for the pin/group witness, so no read crosses the edge. |
| Reader racing the multi-entry owner-array write | It returns one non-tearing packed entry: source (safe stale route) or destination; destination is never published before commit. |
| Cursor/whole-shard operation | The complete physical shard is the minimum transfer unit; ownership is sampled once and checked at execution. |
## 7. ROOT CAUSE — the block cache is NOT owner-private

The debug build names it directly, on the ordinary write path:

```
RLCACHE-VIOLATION owner: take on cache 0x75c8f6447080 from tid 3136850, owner tid 3136854

KvBlockCache::take                         kv_block_cache.h
  FlatStore::read_local_cache_take         flatstore_atomic.inc:1453
    FlatStore::make_set_string             flatstore.h:1301
      store_string / cmd_set<false>        t_string.cc:158 / 550
        ExLoopT<true>::execute<false,false> ex_loop.h:2955
          exec_batch / drain_tasks          ex_loop.h:2758 / 2100
            fused_pass_impl                 ex_loop.h:547
```

A fused thread executing an ordinary SET task reached **another fused thread's** block cache. The
cache is reached as `read_local_store_state_armed().retire_sink.block_cache` — a pointer stored on
the SHARD's store, not on the executing thread. So the identity of the cache a write uses is
whatever the shard was last bound to, and nothing on the write path checks that this is still the
thread doing the writing.

Two threads then splice one unlocked free list: a lost update leaves a block linked twice, `put`
sees itself as the head and aborts (`ceb6b02f8`), or `take` hands out a still-linked block whose
KvObj header then overwrites the list `next` and the following `take` dereferences a wild head
(`0xc000100` — the ringdiet segfault).

The same stale pointer is `retire_sink.defer`, which pushes into `ReadLocalDeferredQueue`'s
single-producer retire ring. The cache is simply where the damage shows first.

## 8. THE MECHANISM, from the event log

With the shard-transfer edge and every sink rebind logged, one reproduction says it outright
(`store 0x7f6c0c882638` is shard 1; tid-index is the thread's own id, tid is the OS thread):

```
line 148: RLBIND transfer  store ...882638 sid 1  src tid-index 1 -> dst tid-index 7
line 161: RLBIND rebind    store ...882638 cache 0x..5247080 -> 0x..4a47080  tid 3155464   (= index 7)
line 173: RLBIND transfer  store ...882638 sid 1  src tid-index 7 -> dst tid-index 1
          ... rebindall for indices 3,7,5,4,0 ... but NOT for index 1 ...
line 188: RLCACHE-VIOLATION take on cache 0x..4a47080 from tid 3155458, owner tid 3155464,
                            reached through store ...882638
```

Shard 1 moved to thread index 1 (`tid 3155458`) at line 173. Its store's sink still named thread
index 7's cache (`0x..4a47080`, bound at line 161). **Thread index 1 then executed `cmd_set` on
shard 1 through thread index 7's block cache**, and no rebind for that store appears in between.

`Server::transfer_shard_quiesced` moves the shard: the `shards()` vectors, the bucket router, and
`shard_owner_`. It does NOT move the read-local retire sink. That was left to the destination's own
later executor pass (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`, ex_loop.h
460/662), and NOTHING orders that pass before the destination's first write to the shard it has
just been given. `rebind_read_local_retire_sink`'s own comment claims it is called "before the new
owner executes store work" — that half of the sentence was never enforced.

**Control.** `--key-lb 0 --client-lb 0` removes shard moves and nothing else:

| arm                                   | runs | deaths |
|---------------------------------------|------|--------|
| armed fused, LB on (default)          |  3   |   1    |
| armed fused, LB off (`--key-lb 0`)    |  5   |   0    |

## 9. THE FIX — the sink moves with ownership

`Server::adopt_read_local_retire_sink(shard, destination)` rebinds the shard's store to the
destination thread's sink INSIDE both quiesced transfer functions, immediately before
`router_.commit_transfer()` — the same critical section that moves the shard. Both callers already
hold every executor at the quiesced safe point (`lb_commit_shard_plan` commits only once every EX
thread has acked ExDrain, and `flip_quiesced()` grants that ack only with an EMPTY retire ring), so
the source has nothing outstanding for this shard and the destination has not started. There is no
window left for the sink to disagree with ownership.

The lazy `rebind_owned_shards` path is kept; it is now always a no-op, and is the cheap standing
check that the eager rebind did its job.

| arm (64 conns, 64-key space, 40 s runs)          | runs | deaths |
|--------------------------------------------------|------|--------|
| `ceb6b02f8` pristine release                      |  2   |   1    |
## 2. Reachability verdict — LATENT, not live

**Verdict: not reachable today.** The reason §9 gives is directionally right but incomplete; the
airtight statement is:

> **A blocking op is always alone in its ROB.**

Both halves are enforced, at two sites 47 lines apart in a different file from the clear:

* **Nothing older.** `io_loop.h:1391` — `if (spec->flags & CmdFlags::Blocking) { … if (rob.in_flight() != 0) break; }`.
  The blocking op cannot even be *prepared* until every older op of the connection has retired.
* **Nothing younger.** The same dispatch sets the barrier (`io_loop.h:1437`) and `break`s; the parse
  loop's head (`io_loop.h:1113`) and `flush_ready`'s re-parse gate (`io_loop.h:1894`) both refuse to
  parse while it is set. No byte behind the blocking frame is even looked at.

Therefore, at the instant `blocking_retire` runs, `dispatch_id - flush_id == 1` and the single live
op *is* the blocking op. The barrier's only possible owner is the one being released. Clearing
all owners and clearing one owner are the same operation.

The same argument covers **`blocking_scatter_retire`**: `state->blocking_origin` is written at exactly
one site — `blocking.inc:1203`, inside `blocking_resume_move` — which converts an *already parked*
blocking op in place (`op.detach_blocking_state(); op.attach_scatter_state(...)`). It creates no new
ROB slot, so the "alone in its ROB" property is inherited unchanged.

And **`BlockingState` cannot reach the ROB by any other route**: `attach_blocking_state` has exactly
one caller, `io_loop.h:1418`, behind the guard above. In particular a blocking command lowered inside
`MULTI` never produces one — `EXEC` children are not ROB ops and the parent `EXEC` op carries
`multi_state`, not `blocking_state`.

### Interleavings I checked and rejected

| Candidate | Why it does not fire |
|---|---|
| Pipelined `EXEC` / `SUBSCRIBE` / `CLIENT KILL` **then** `BLPOP` in one write | The first command sets the barrier and ends the pass; the `BLPOP` bytes are never parsed until the barrier drops at quiescence, by which time the first owner is done |
| Pipelined `BLPOP` **then** `EXEC` | `BLPOP` sets the barrier; `EXEC` is never parsed |
| Second owner set from **outside** `parse_and_dispatch` while a blocking op is parked | There is no such path. `blocking_resume_move` (the only thing that runs on a barred connection, `io_loop.h:1845`) does **not** acquire the barrier — it inherits the one already held. Pub/sub and climon event delivery on the io thread only ever set the barrier on the *requesting* client, never on a victim; `CLIENT KILL`/`UNBLOCK` reach a victim through `mark_closing()` / `blocking_request_unblock()`, neither of which touches `scatter_barrier_` |
| Two connections | The barrier is per-`Client` and every site writes it through the connection's own io owner. No cross-connection write exists |
| A `Blocking`-flagged command that skips the guard | Only `XREAD` without `BLOCK`, which `goto nonblocking_dispatch`es before the guard and never attaches blocking state |
| Deferred `WAIT` coexisting with a blocking op | `WAIT` is `OrderedLocal`, so it too needs `in_flight()==0`, and it too ends the pass |
### One counter, one meaning — and why the falsifier needed a second one

Two different claims are being made, and they need two counters:

* **`atomic_exec_order_holds`** (`ExLoop::execute()`, dispatch) — *the hold fired*. Non-zero by
  design in every armed run; `tests/multirace.py` and `tests/multires.py` fail a run in which it
  never advances, because such a run never entered the window.
* **`atomic_exec_order_late`** (`prepare_write_key()`, install) — *the hold fired **early
  enough***. Must read **zero**, in both atomic modes, from boot.

The install-time site asks the identical question one step later. Only the owner thread publishes
into its own pending list, so the park should foreclose it completely — and that foreclosure is
the whole acyclicity argument above. Keeping the probe is what makes that argument falsifiable.

The refinement this lane closes: those two sites were briefly **merged onto one counter**, which
made the falsifier inert. `holds` is non-zero *by design* in exactly the runs that open the window,
so a summed counter can never be checked against zero — the safety argument could have broken with
no test going red, and the batteries' non-vacuity assertion would have been satisfied by park
increments regardless. Split, each number answers exactly one question and both are asserted:
`holds > 0` (armed) and `late == 0` (always).

`atomic_exec_order_late` is a namespace-scope `std::atomic<uint64_t>` in `multi.inc`, not a
`Shard::Stats` field, because `sizeof(Shard)` is locked at 1440 and `Stats` is a by-value member —
a 20th counter would have grown it. Nothing is lost by the placement: the increment sits on a path
that is never taken, so there is no per-shard line to contend for. There is precedent for the
shape (`scripting.cc`, `slowlog.cc`, `functions.cc` all keep INFO counters this way).

The `prepare_write_key()` probe also previously excluded only `state.epoch`, so it counted a

exec
/bin/bash -lc "rg -n '"'^##|''^###'"' /home/user/Projects/lane-notes-archive/P128.md
sed -n '156,176p' /home/user/Projects/lane-notes-archive/NOTES-EXPWIDE.md
sed -n '148,180p' /home/user/Projects/lane-notes-archive/DESIGN-flipfp.md
sed -n '202,247p' /home/user/Projects/lane-notes-archive/DESIGN-flipfp.md" in /home/user/Projects/cx-inline
 succeeded in 0ms:
28:## 0. The anomaly (owner's numbers, 32 cores / 64 shards / 512 conns)
38:## 1. Sweep 1 recovered (16 conns/thread = 128 conns, pre-prototype binary)
55:### The 2x2 discriminator already run before the kill (same binary)
84:## 2. Mechanism (from the code, `src/core/io_loop.h` + `src/core/ex_loop.h`)
102:### Hypotheses A-D against this
115:### Rotation order (from `io_loop.h` run loop, `flush_ready`, `ex_loop.h fused_pass_impl`)
128:## 3. Per-connection armed-state footprint (from the layout locks and `rob.h`)
147:## 4. SHIPPED: lane admission -- defer, never demote; pressure-gated fair share; no knob
205:## 6. Design item: widening `Rob<64>` would not help here (do not)
220:## 5. Results with the prototype (chain4; geometry note)
232:### 5.1 Depth A/B, 16 conns/thread (80 conns, 5 threads) — pass ab2
279:### 5.1b The CLEAN cells (chain6 binary = quota form of arm 2; 5 threads on 40-44, memtier on
325:## 5.2 The real rig (coordinator's quiet-box decomposition, 32c, 1s+RL, pure GET, 2 replicates)
347:### PRE/POST table for the cells the owner sees (POST = to be measured on the real rig)
360:## 7. Why pure deferral is not enough: the rotation order starves the deferred set
377:### 7.1 Starvation check -- RESULT (s3i, section 9.3) and why it is structural
421:## 8. Directed test: `tests/read_local_lane.py` (gate row "read-local lane admission battery")
423:### 8.1 Why the row drives a CAP and not load
453:### 8.2 What the row checks
476:## 9. New-core harness and directional results (cores 32-39 / 160-167, ports 8200-8209)
487:### 9.1 Rig truth: the 8-thread layout is loadgen-bound; 4 threads + 12 memtier threads saturate
502:### 9.2 Variant A (window re-armed by its own quota deferrals; `in_flight()` share) -- superseded
542:### 9.3 FINAL form (lane-full-only arming, pending-slot share): 3-arm pre / post / no-quota
554:#### s3i -- 63 conns/thread x p32 = 2016 nominal vs a 1024-entry lane (2x the boundary)
588:#### s3j -- 33 conns/thread x p32 = 1056 vs 1024 (marginally over the boundary): NOT RESOLVABLE
609:#### Battery results on the final binary (batt3, cores 32-35+160-163, port 8204)
635:### 9.4 The MUST-NOT-REGRESS cells (s3k): 9 conns/thread, final binary, 3 arms
685:### 9.5 OWNER ACCEPTANCE RUN, and the footprint defect it exposed in the control cell
709:#### What this lane added, and where every byte of it sits (read from the two binaries' DWARF)
741:#### Fix (commit 20d449a24): gate the argument, not just the answer
751:### 9.6 Verification of the lane-cap row (commit 32d99db98)
773:## 10. FINAL PRE/POST
781:### 10.1 Measured on the lane rig -- 4 saturated fused threads (32,33,160,161), `--shards 4`
834:### 10.2 The owner's quiet box (32 cores, 64 fused threads, 1s + read-local, pure GET)
854:### 10.3 The two cells I am asking the owner to run
### W3. A cross-shard script compared deadlines against the machine's uptime

Found by reading the code while fixing W1, then reproduced. `ScatterState::script_now_ms` was
`now_ns() / 1000000`, and `now_ns()` is **CLOCK_MONOTONIC**. That value — milliseconds since boot,
order 1e7 — was installed as the owner's `cached_now_ms_` for the script's Read phase, where it is
compared against absolute deadlines of order 1.79e12. Every comparison therefore said "alive":

```
                         geometry          state          DBSIZE   EVAL {GET k1, GET k2}
tomokv UNFIXED --atomic 1  SAME owner       elapsed keys      2      [NIL, NIL]     <- correct
tomokv UNFIXED --atomic 1  TWO owners       elapsed keys      2      [v1, v2]       <- wrong
redis 7.4.2 oracle         one keyspace     elapsed keys      2      [NIL, NIL]
tomokv UNFIXED --atomic 1  TWO owners       live keys (ctl)   2      [v1, v2]
```

Deterministic and total, not a race: a cross-shard script could never observe any key as expired,
and could never reap one. The single-owner geometry on the same server and the same script is the
control that makes it attributable.

---

## 5. Results (base ceb6b02f8, arms differ by this change only)

Arms: PRE = mainline ceb6b02f8 (`79651da57b14ac1b`), POST = this branch (`71b2854a7599ee8a`), both built with
`taskset -c 48-51,176-179 make -j8` in their own worktree. Method: cyclemap slope (replay, N1=300k /
N2=900k ops per connection, so every figure is marginal cost per op at matched rate, not an average
inflated by idle spin). Armed (`--atomic 1`), p32, ring 4096, 2 reps; spread is [min..max]/2.
`--flip-auto 0` throughout the perf table -- the shipped configuration, in which PRE runs the body on
every dispatched frame and POST does not run it at all.

### 5.1 The lever, against its own same-binary null

| cell | metric | PRE | POST | delta | NULL (PRE vs PRE) | verdict |
|---|---|---:|---:|---:|---:|---|
| 1s 1 thread, SET | instr/op | 3198 | 3096 | **-102** | +8 | 12x the null |
| 1s 1 thread, SET | cycles/op | 1084 | 1059 | -24 | -17 | within the null |
| 1s 2 threads, SET | instr/op | 3246 | 3153 | **-94** | +27 | 3.5x the null |
| 1s 1 thread, GET | instr/op | 2240 | 2146 | **-94** | -0 | null is zero |
| 1s 2 threads, GET | instr/op | 2276 | 2193 | **-83** | -1 | null is zero |
| 2s split, SET | instr/op | 3217 | 2989 | **-228** | -5 | 46x the null |
| 2s split, SET | cycles/op | 1451 | 1336 | **-115** | +23 | opposite sign to the null |
| 2s split, SET | store-queue-full cyc/op | 239 | 193 | **-45** | +8 | opposite sign to the null |
| 2s split, SET | driver ops/s | 4.50M | 4.87M | **+8.1%** | -1.8% | opposite sign to the null |
| 2s split, GET | instr/op | 3000 | 2808 | **-191** | -4 | 48x the null |
| 2s split, GET | cycles/op | 1398 | 1311 | **-86** | +8 | opposite sign to the null |
| 2s split, GET | store-queue-full cyc/op | 258 | 227 | **-30** | -1 | 30x the null |

The null is the instrument's own answer, and it is what makes the table readable: **instr/op is the
trustworthy metric** (null +8 / +27 / -0 / -1 / -5 / -4), while at 1 fused thread cycles/op and
store-queue-full move as much in the null as in the real comparison and are not claimable there.
Both are claimable in split, where they run opposite in sign to the null.

The measured drop exceeds CYCLEMAP's predicted 63-86 instr/op everywhere; that prediction was taken on
an older base and a different geometry. GET falls as much as SET, which is what the mechanism says
### 5.2 Controller accuracy: unchanged

The perf win would be vacuous if the sampled fingerprint had simply stopped detecting -- a dead
detector is both cheap and silent, and "no false triggers" is exactly what a dead detector reports.
So the gate had to OPEN.

**Liveness.** `band_` is written only by `update_band()`, which runs only from `observe()`, which runs
only on a published window. Anchored on a stationary 3.64M/s 2:2 load, POST reports
`signature_band = 0.000206` -- nonzero, so the sampled writer published and the detector read it. Its
value is the predicted quantum floor: `8W/(C*T) = 8*100/3.64e6 = 2.20e-4` against 2.06e-4 measured.
(The accuracy cells' all-zero dumps were snapshots taken mid-maneuver, when `reset()` had just zeroed
the detector; both arms show that.)

**Detection, isolated from the rate detector.** A workload swap normally moves the command rate too,
and the rate detector's learned band here is ~0.3%, so it always wins the race -- which is why the
first probe attributed nothing. `scratch/flipfp/fpshift.sh` pins the rate with `--rate-limiting` and
changes only the CLASS: phase A GET-only, phase B SET-only, same connections, same pipeline, same
per-connection rate.

| | PRE | POST |
|---|---:|---:|
| phase A rate / phase B rate | 512003.70 / 512015.39 (+0.0%) | 512010.30 / 512016.83 (+0.0%) |
| anchored band (phase A) | 0.000655 | 0.005597 |
| **fingerprint triggers on the swap** | **1** | **1** |
| shift distance / shift band | 0.000617 / 0.000470 | 0.015517 / 0.013625 |

Both arms detect the mix change with exactly one fingerprint trigger at an identically pinned rate.
The band is regime-dependent exactly as section 2(c) predicts -- 8x TIGHTER than PRE at 3.6M/s
(quantum-floored) and 8.5x wider at 512k/s (jitter-dominated, the jitter inflated by sampling noise
sigma = 0.25/sqrt(K_eff), K_eff near the homogeneous end of its bracket) -- and detection survives
both because band and distance scale together: the band is learned from, and applied to, the same
sampled stream.

**Wrong-split boots** (MK8 1:1, atomic 1, 120 s; `--flip-auto 1`):

| cell | flips | time to first move | stabilized | final split |
|---|---:|---:|---:|---|
| 3:1 PRE | 8 | 12 s | **never** | 3:1 (back to the wrong split) |
| 3:1 POST | **1** | 16 s | 16 s | **2:2** |
| 3:1 POST, `--flip-auto 0` | 0 | never | - | 3:1 (dark, correct) |
| 5:1 PRE | 1 | 18 s | 18 s | 4:2 |
| 5:1 POST | 1 | 15 s | 15 s | 4:2 |

POST moves once and lands in both; on 3:1 it is PRE that thrashes (8 flips, never stabilizes, ends on
the split it booted with). 28:4 is not runnable on a four-physical-core allocation -- 5:1 on six
threads is the widest analogue this lane's cores hold, and it is reported as such.

exec
/bin/bash -lc "sed -n '147,204p' /home/user/Projects/lane-notes-archive/P128.md
sed -n '421,452p' /home/user/Projects/lane-notes-archive/P128.md
rg -n -C 3 'epoch|snapshot|commit|retry' src/core/server.h | head -120" in /home/user/Projects/cx-inline-comments
 succeeded in 0ms:
## 4. SHIPPED: lane admission -- defer, never demote; pressure-gated fair share; no knob

Files: `src/core/io_loop.h` (parser gate chain), `src/core/ex_loop.h` (quota, pressure window,
per-rotation decay), `src/core/thread.h` (counters), `src/cmd/t_server.cc` (INFO). Prototype knob
`--read-local-lane-full 0|1|2` and its CONFIG GET entry and parser test are removed (knob
philosophy: prefer no knob; the threshold self-derives).

1. LANE FULL -> DEFER. When `local_read_lane_has_room(demand)` is false the armed parser leaves the
   frame at rpos and ends the connection's pass (`defer_lane_full++`). Nothing was published: the
   ROB slot is acquired but not published (same shape as the MGET admission fence), no lane entry,
   no pending bit, no owner slot. The read is re-parsed by a later pass of the SAME thread after
   its EX pass drained the lane, and still completes locally. The former path (demote to the shard
   owner as an ordinary task, `ReadLocalFallbackReason::LaneFull`) is unreachable from the parser;
   `read_local_fallback_lane_full` stays in INFO for continuity and is 0 by construction.
2. PRESSURE WINDOW. Any deferral (either kind) sets `Impl::lane_pressure =
   kReadLocalLanePressureRotations` (8); `fused_pass_impl` decrements it once per rotation. While it
   is non-zero the parser's per-pass bound is `quota = max(1, kInboxSlots / active_.size())`;
   otherwise `UINT32_MAX`. (`ExLoop::read_local_lane_quota(live)`, one predicted byte test per
   parse pass; the division runs only under pressure.) "Live connections x observed depth": live =
   the active set; observed depth = the deferrals themselves -- a rotation whose aggregate demand
   fit the lane arms nothing.
3. FAIR SHARE -> DEFER. A read on a connection with `rob.in_flight() >= quota` is deferred too
   (`defer_quota++`, re-arms the window). By construction the sum of admitted local reads stays
   within the lane, so the lane-full deferral -- whose victims are whichever connections the
   rotation parsed LAST (section 7) -- stops firing after at most one rotation per pressure
   episode, and every connection progresses every rotation. The compare is the one the prototype's
   arm 2 already paid, against a register; the batch-extension test carries the same term.
4. Cost in the steady state (lane never full): one byte load per parse pass, one popcount+compare
   per eligible read. Measured PRE/POST at 9 conns/thread x p32 saturated (section 9): instr/op
   2279/2280 vs 2274/2264 (+0.2..0.7%), throughput +0.9% / +4.7% (noise band).

Two corrections measured on the saturated 4-thread rig (section 9) BEFORE the final form:
- The window was first re-armed by the quota's own deferrals. At the lane boundary (33/thr x p32)
  that kept it armed forever: 1.07M quota deferrals in 20 s where the lane itself refused 98 reads
  (0.8% of ops re-parsed vs 0.00008%), p99 1.03 -> 1.43 ms. A signal the actuator produces cannot
  police the actuator (the Sweep-ABANDON law). Final: only a LANE-FULL deferral arms the window.
- The share was first measured as `rob.in_flight()`. That counts replies PHASE 2's bounded serve
  has not flushed yet, which hold no lane slot; at 2x lane the busiest thread fell to 416 ops per
  rotation (from ~1000) because connections were throttled for unflushed replies. Final: the share
  is `Rob::pending_read_local_count()` = popcount of the pending local-read slot bitmap = the lane
  slots the connection actually holds, zero again once the EX pass ran.

Invariants untouched: single-owner writes; reads never obstructed by writes (a deferred read waits
for its OWN thread's lane, not for any writer); RYOW per connection (frame order is preserved --
the deferred frame is consumed before anything behind it; `tests/read_local_lane.py` proves a SET
and GET behind 64 deferred GETs reply in order with the new value); in-order retirement; every
layout lock (Op=336, Client=1984, ThreadCtx=1408, Shard=1440; `ReadLocalStats` sits behind
`read_local_state_` and `Impl` behind `read_local_.impl`, both heap, so neither lock moves;
Config back to its pre-prototype body at 624). `--thread-mode 2s` has no lane and no arm: the
quota call is inside `if constexpr (Fused)`.

Why not the alternatives: widening `Rob<64>` moves the cliff (section 6); a bigger lane moves the
cliff to a larger connection count and costs L2 (17 KB per 1024 entries); an always-on static
quota binds at 17+ active deep-pipelining connections per thread even when the lane never fills
(the unsaturated 64-conns/thread cell in 5.2 ran -7% with 2.7M quota deferrals; unconfirmed, may
be drift, but the gating makes it moot); changing the fairlane TAIL-drain rule (the amplifier) is
the write-path lanes' territory and unnecessary once no capacity demotion exists in pure reads.

## 8. Directed test: `tests/read_local_lane.py` (gate row "read-local lane admission battery")

### 8.1 Why the row drives a CAP and not load

Oversubscribing the 1024-entry lane by traffic is a RATE RACE against the drain, and a test client
cannot win it: the fused thread empties its lane every rotation, far faster than one Python process
can fill it. The first version of this row tried to win it anyway and lost in three escalating ways
-- serial writes (fired 17 times on the armed boot, never on the cheaper unarmed one), then
parallel writers and connection doubling, which on the GATE's box produced
`read_local_defer_lane_full FAIL delta=0 after 3 escalation(s), 2048 conns, 48 rounds` while the
row's own geometry line reported 256 connections x 64 deep = 2624 frames. The frames existed; they
were never in flight together. A row that fires only on a saturated rig is not a gate row, and an
anti-vacuity row that cannot make its own precondition is the vacuity trap itself.

`DEBUG READ-LOCAL-LANE-CAP <n>` lowers the ADMISSION threshold only. The ring keeps its
kInboxSlots entries and its `& (kInboxSlots - 1)` masking, so a lower cap can never overrun it; it
moves only the point at which the parser stops admitting. A single connection pipelining more than
the cap in ONE write then oversubscribes the lane INSIDE ONE PARSE PASS, before any drain can run.
That removes the race completely: the row fires on any box at any speed, with 16 connections and
rounds that take 2 ms. 0 restores the derived value, which is what production always runs.

Cost, held to the bar the footprint defect in 9.5 set. `Impl::lane_admit_cap` is a uint16 at
offset 38, in the two padding bytes that already sat between `prefetch_capture` (37) and
`demote_context` (40): Impl does not grow, nothing moves, and it shares the first cache line with
`lane_head`/`lane_tail`/`lane_count`, which every admission test already reads -- so the admission
path gains one `movzwl` against a line it already owns and NO new line.
`Server::debug_read_local_lane_cap_` is a relaxed uint32 inside the existing contiguous block of
debug-only atomics that production never writes, so its line stays shared-clean in every core's L1;
each fused thread reads it ONCE PER ROTATION (never per op) and stores into the Impl only when the
value changes. Measured at the must-not-regress geometry (9.6): instr/op +0.32%, everything else
neutral.

8-//
9-// THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
10-// dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
11:// its derived shard-granularity fast path, published only at a quiesced commit. See
12-// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
13-#pragma once
14-#include <algorithm>
--
34-#include "../net/conn.h"   // kRobWindow: one source of truth for the window size
35-#include "../net/wb.h"
36-#include "../cmd/command.h"
37:#include "../snapshot/snapshot.h"
38-#include "../persist/aof.h"
39-
40-#ifdef TOMO_RL_CACHE_DEBUG
--
112-// Allocated only for the boot-armed read-local lane. The Server keeps only one pointer at its
113-// true tail, so baseline member offsets and cache-line sharing remain unchanged.
114-struct ReadLocalServerState {
115:    std::atomic<uint64_t> epoch{1};
116-};
117-
118:// A snapshot/placement drain is the only reader, while opens and closes happen on unrelated
119-// physical threads.  Monotonic halves avoid the false-zero a signed per-thread delta can expose
120-// when a scan observes an executor's close before the admitting IO's open.  Cache-line spacing
121-// keeps those writers independent.
--
135-    class ClientWorkScope {
136-    public:
137-        ClientWorkScope(Server& server, uint32_t tid)
138:            : epoch_(server.client_work_[tid].epoch) {
139:            const uint64_t before = epoch_.load(std::memory_order_relaxed);
140-            outer_ = !(before & 1);
141-            if (outer_) {
142-                end_ = before + 2;
143:                epoch_.store(before + 1, std::memory_order_release);
144-            }
145-        }
146-        ~ClientWorkScope() {
147:            if (outer_) epoch_.store(end_, std::memory_order_release);
148-        }
149-        ClientWorkScope(const ClientWorkScope&) = delete;
150-        ClientWorkScope& operator=(const ClientWorkScope&) = delete;
151-    private:
152:        std::atomic<uint64_t>& epoch_;
153-        uint64_t end_ = 0;
154-        bool outer_ = false;
155-    };
156:    uint64_t client_work_epoch(uint32_t tid) const {
157:        return client_work_[tid].epoch.load(std::memory_order_acquire);
158-    }
159-    static constexpr uint64_t kAtomicEnabledBit = uint64_t{1} << 63;
160-
--
163-    Server(const Server&) = delete;
164-    Server& operator=(const Server&) = delete;
165-
166:    // Resolve placement and the shard default once, before AOF/snapshot recovery reads geometry.
167-    bool prepare_boot(Config& cfg) {
168-        const char* domains = std::getenv("TOMOKV_L3_DOMAINS");
169-        if (domains && *domains) {
--
278-                return false;
279-            }
280-            shards_[i]->bind_atomic_state(
281:                [](void* ctx) { return static_cast<Server*>(ctx)->atomic_commit(); }, this,
282-                &atomic_activity_, &script_intent_owners_);
283-        }
284-        router_.build_uniform(static_cast<int32_t>(cfg.shards));
--
302-            std::fprintf(stderr, "fatal: could not allocate live configuration mailboxes\n");
303-            return false;
304-        }
305:        live_config_committed_ = capture_live_config(2);
306-        for (uint32_t i = 0; i <= nthreads; i++)
307:            live_config_mailboxes_[i].init(live_config_committed_);
308-        for (uint32_t i = 0; i < nthreads; i++) {
309-            threads_[i] = std::make_unique<ThreadCtx>();
310-            // The fingerprint writer is armed only when its one reader, the flip controller, is
--
368-        for (uint32_t i = 0; i < nthreads; i++)
369-            atomic_read_floors_[i].store(UINT64_MAX, std::memory_order_relaxed);
370-        for (uint32_t i = 0; i < nthreads; i++)
371:            atomic_snapshot_completions_[i].store(0, std::memory_order_relaxed);
372-        flip_target_io_.store(static_cast<uint32_t>(placement_.ifid_threads().size()),
373-                              std::memory_order_relaxed);
374-        flip_target_ex_.store(static_cast<uint32_t>(placement_.ex_threads().size()),
--
386-            // thread resolution even when adjacent tids live in different L3 domains.
387-            shards_[sid]->note_migration(placement_.domain_of_thread(tid));
388-        }
389:        snapshot_.init(nthreads, cfg.shards,
390-                       static_cast<uint32_t>(placement_.ex_threads().size()),
391-                       cfg.dir, cfg.dbfilename, persistence_engine(cfg));
392-        aof_.init(*this, cfg, nthreads, cfg.shards,
--
555-    ThreadCtx&       thread(uint32_t i) { return *threads_[i]; }
556-    uint32_t         nthreads()   const { return static_cast<uint32_t>(threads_.size()); }
557-    uint32_t         nshards()    const { return static_cast<uint32_t>(shards_.size()); }
558:    SnapshotManager& snapshot()         { return snapshot_; }
559:    const SnapshotManager& snapshot() const { return snapshot_; }
560-    AofManager& aof() { return aof_; }
561-    const AofManager& aof() const { return aof_; }
562-    const ThreadCtx& thread(uint32_t i) const { return *threads_[i]; }
--
608-        return cfg.read_local != 0;
609-    }
610-    bool read_local_enabled() const { return read_local_enabled(cfg_); }
611:    uint64_t read_local_epoch() const {
612-        if (!read_local_state_) std::abort();
613:        return read_local_state_->epoch.load(std::memory_order_seq_cst);
614-    }
615-    // Retirement uses the returned OLD value as its stamp. The increment makes a subsequent
616-    // rotation publication strictly newer, which is the grace test below.
617:    uint64_t advance_read_local_epoch() {
618-        if (!read_local_state_) std::abort();
619:        return read_local_state_->epoch.fetch_add(1, std::memory_order_seq_cst);
620-    }
621-    // Grace floor for a QSBR drain, bounded by what the caller needs. `stamp` is the oldest sealed
622-    // retirement stamp; the drain releases an entry only when the floor is STRICTLY above its stamp.

apply patch
patch: completed
/home/user/Projects/cx-inline-comments/src/core/config.h
/home/user/Projects/cx-inline-comments/src/core/flipctl.h
/home/user/Projects/cx-inline-comments/src/core/server.h
/home/user/Projects/cx-inline-comments/src/core/shard.h
/home/user/Projects/cx-inline-comments/src/core/signal.h
/home/user/Projects/cx-inline-comments/src/core/thread.h
diff --git a//home/user/Projects/cx-inline-comments/src/core/config.h b//home/user/Projects/cx-inline-comments/src/core/config.h
index 495e6ae42da15ee4bcc76a8a846efeef506903d7..8b5f3e5c786e8eeae0296a5dcf19338056d59007
--- a//home/user/Projects/cx-inline-comments/src/core/config.h
+++ b//home/user/Projects/cx-inline-comments/src/core/config.h
@@ -290,8 +290,8 @@
     uint32_t client_lb = 1;
 
     // ---- automatic role split (boot-latched) -----------------------------------------------
-    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer
-    // (DESIGN-flipfp.md); its measured sampling policy is internal to flipctl.h. Off and 1s
+    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer;
+    // its measured sampling policy is internal to flipctl.h. Off and 1s
     // dispatch pay one predicted branch per op and no fingerprint store. Trigger bands learn
     // from the anchor's own quiet jitter; no separate controller tuning knobs remain.
     uint32_t flip_auto = 0;
diff --git a//home/user/Projects/cx-inline-comments/src/core/flipctl.h b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
index 5185da4085ae5f2e3325702064208810cb9c42f7..1edc3a93889090da659db0c9ee259d04ce5cef57
--- a//home/user/Projects/cx-inline-comments/src/core/flipctl.h
+++ b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
@@ -19,7 +19,9 @@
 
 class Server;
 
-// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
+// Preserve the measured sampler: one whole parse pass in 100 on average. At a matched 512k ops/s,
+// a GET-only -> SET-only swap triggered the fingerprint detector once with both exhaustive and
+// 1-in-100 sampling; the sampled writer still detected the shift with the rate held fixed.
 // Arming derives solely from the controller latch. No runtime sampling knob or adaptive
 // per-operation work is needed; the detector already derives its bands from actual samples.
 inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
@@ -52,7 +54,9 @@
     uint64_t closed_windows = 0;
 };
 
-// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
+// SAMPLED BY PARSE PASS. A per-op countdown would restore the store this sampler removes: no
+// existing io-side command counter covers both the point paths and all io-local commands.
+// The io loop asks pass_sampled() once per dispatched
 // frame -- one load of this writer's own word, on the line the old enabled() test already read,
 // and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
 // for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
diff --git a//home/user/Projects/cx-inline-comments/src/core/server.h b//home/user/Projects/cx-inline-comments/src/core/server.h
index f869dd3d1737afa6b1c401919fb66c96772b1225..f24f7a00e809e910c07736a056e4478c9ff3c925
--- a//home/user/Projects/cx-inline-comments/src/core/server.h
+++ b//home/user/Projects/cx-inline-comments/src/core/server.h
@@ -8,8 +8,10 @@
 //
 // THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
 // dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
-// its derived shard-granularity fast path, published only at a quiesced commit. See
-// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
+// its derived shard-granularity fast path, published only at a quiesced commit. Shards move only
+// after executing, deferred, group and pinned-read work drains; destination routes publish after
+// the ownership edge. An IO may enqueue using an old route after that edge, so tasks and borrow
+// releases recheck ownership and forward before shard access instead of carrying routing epochs.
 #pragma once
 #include <algorithm>
 #include <atomic>
@@ -308,7 +310,7 @@
         for (uint32_t i = 0; i < nthreads; i++) {
             threads_[i] = std::make_unique<ThreadCtx>();
             // The fingerprint writer is armed only when its one reader, the flip controller, is
-            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
+            // enabled: with --flip-auto 0 and in 1s mode it is dark and costs
             // one predicted branch per op. The enabled sampler keeps its measured 1-in-100 policy.
             threads_[i]->init(i, placement_.role_of(i), nthreads,
                               0,
@@ -1969,7 +1971,7 @@
         shard_owner_[shard_id].store(thread_id, std::memory_order_release);
     }
 
-    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
+    // The caller must hold both executor loops at the safe point:
     // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
     // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
     // Vector capacity and membership are settled while PREPARING still names the source.  The
@@ -1988,7 +1990,7 @@
     // lost, a block ends up linked twice, and the damage surfaces later as either
     // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
     // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
-    // Both were observed (see DESIGN-P0REPLY.md).
+    // Both were observed.
     //
     // The rebind used to be deferred to the destination's own next executor pass
     // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
@@ -2827,7 +2829,7 @@
     // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
     // other command. It widens -- deterministically -- the window in which a younger whole-owner
     // walker could overtake an older same-connection group on the destination shard.
-    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
+    // TEST HOOK: effective local-read LANE CAPACITY for admission. 0 means
     // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
     // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
     // its masking, so this can never overrun anything. It exists because lane oversubscription is
@@ -2935,8 +2937,10 @@
     // is already set. Nothing on GET/SET touches it.
     //
     // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
-    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
-    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
+    // sequence: a blocking op requires an empty ROB, then sets the barrier before any younger
+    // frame can be parsed; a resumed scatter inherits that same slot and barrier. The state the
+    // owner-scoped release exists to survive -- a release that must NOT drop the barrier -- has
+    // to be injected. It is held past
     // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
     //
     // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
@@ -2968,8 +2972,8 @@
         return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
     }
     // An overlap: some owner took the parse barrier while another owner already held it. With
-    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
-    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
+    // DEBUG BARRIER-HOLD off this must read ZERO -- if it ever moves on production traffic the
+    // latent case just went
     // live and the owner-scoped release in blocking_retire() became load-bearing rather than
     // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
     // own positive control: a "must be zero" reading proves nothing until something has been shown
diff --git a//home/user/Projects/cx-inline-comments/src/core/shard.h b//home/user/Projects/cx-inline-comments/src/core/shard.h
index 694b9a93c6cd63db49b9dcf061dbb5182c49986a..39a049d2f8619bbf52f706775956cc9198060364
--- a//home/user/Projects/cx-inline-comments/src/core/shard.h
+++ b//home/user/Projects/cx-inline-comments/src/core/shard.h
@@ -431,7 +431,7 @@
         // NOT share this counter: multi.inc's prepare_write_key() re-asks it at install time and
         // reports violations through INFO atomic_exec_order_late (multi_exec_order_late()), which
         // must read zero. Summing the two would hide a violation inside a number that is non-zero
-        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
+        // by design, leaving the park's safety argument with no falsifier.
         uint64_t atomic_exec_order_holds = 0;
         // Times watch_finalize_reservation() answered "not ready" because the reservation's epoch
         // was still 0, i.e. a unit was turned into a Retry by an undecided WATCH reservation. It
diff --git a//home/user/Projects/cx-inline-comments/src/core/signal.h b//home/user/Projects/cx-inline-comments/src/core/signal.h
index 933a5d7ee09538f3923df3c5eb77f9c72cdc0bfa..f69f7d73942ee129f4ad9cd1f23befe69fc4167d
--- a//home/user/Projects/cx-inline-comments/src/core/signal.h
+++ b//home/user/Projects/cx-inline-comments/src/core/signal.h
@@ -47,7 +47,7 @@
 // that will be compared against a deadline must come from here and never from now_ns() above --
 // that one is CLOCK_MONOTONIC, so its "milliseconds" are milliseconds since boot and sit roughly
 // five orders of magnitude below any real deadline. A monotonic value used as an expiry cut does
-// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
+// not skew the answer, it disables expiry outright.
 inline int64_t now_realtime_ms() {
     timespec ts;
     clock_gettime(CLOCK_REALTIME, &ts);
diff --git a//home/user/Projects/cx-inline-comments/src/core/thread.h b//home/user/Projects/cx-inline-comments/src/core/thread.h
index 0fcfa17ba6e331309260b641ec21aa4a8ff9ba36..3cf72c1b96ca700f602e2e7c630a1e6970f9620f
--- a//home/user/Projects/cx-inline-comments/src/core/thread.h
+++ b//home/user/Projects/cx-inline-comments/src/core/thread.h
@@ -118,7 +118,7 @@
     ContextRoute,           // scatter/script/all-shard or conservative broad-owner route
     ContextKeymissNotify,   // MGET miss must retain owner-side notification behavior
     InflightWrite,
-    ArmTransient,           // pre-arming writes still in flight (DESIGN-RINGDIET.md)
+    ArmTransient,           // pre-arming writes still in flight
     AtomicPending,
     Missing,
     Typed,
@@ -151,7 +151,7 @@
     // ARM-ON-DEMAND TRANSIENT. Reads demoted because the connection carried writes published
     // BEFORE a local read armed it -- writes the RYOW ring deliberately never recorded. Bounded
     // per connection by one ROB drain and reported separately from the steady-state key conflict
-    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
+    // above so that the two can never be confused in a bench.
     uint64_t fallback_arm_transient = 0;
     uint64_t fallback_atomic_pending = 0;
     uint64_t fallback_missing = 0;
@@ -160,7 +160,7 @@
     uint64_t fallback_seq_churn = 0;
     uint64_t fallback_generation = 0;
     uint64_t fallback_lane_full = 0;
-    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
+    // Lane ADMISSION deferrals. Frames the armed parser left unconsumed at rpos, to be
     // re-parsed by a later pass of the same thread, because the local-read lane had no room
     // (defer_lane_full) or because the connection already held its fair share of a lane under
     // pressure (defer_quota). Neither is a fallback: the read still completes locally, and no

diff --git a//home/user/Projects/cx-inline-comments/src/core/config.h b//home/user/Projects/cx-inline-comments/src/core/config.h
index 495e6ae42da15ee4bcc76a8a846efeef506903d7..8b5f3e5c786e8eeae0296a5dcf19338056d59007
--- a//home/user/Projects/cx-inline-comments/src/core/config.h
+++ b//home/user/Projects/cx-inline-comments/src/core/config.h
@@ -290,8 +290,8 @@
     uint32_t client_lb = 1;
 
     // ---- automatic role split (boot-latched) -----------------------------------------------
-    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer
-    // (DESIGN-flipfp.md); its measured sampling policy is internal to flipctl.h. Off and 1s
+    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer;
+    // its measured sampling policy is internal to flipctl.h. Off and 1s
     // dispatch pay one predicted branch per op and no fingerprint store. Trigger bands learn
     // from the anchor's own quiet jitter; no separate controller tuning knobs remain.
     uint32_t flip_auto = 0;
diff --git a//home/user/Projects/cx-inline-comments/src/core/flipctl.h b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
index 5185da4085ae5f2e3325702064208810cb9c42f7..1edc3a93889090da659db0c9ee259d04ce5cef57
--- a//home/user/Projects/cx-inline-comments/src/core/flipctl.h
+++ b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
@@ -19,7 +19,9 @@
 
 class Server;
 
-// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
+// Preserve the measured sampler: one whole parse pass in 100 on average. At a matched 512k ops/s,
+// a GET-only -> SET-only swap triggered the fingerprint detector once with both exhaustive and
+// 1-in-100 sampling; the sampled writer still detected the shift with the rate held fixed.
 // Arming derives solely from the controller latch. No runtime sampling knob or adaptive
 // per-operation work is needed; the detector already derives its bands from actual samples.
 inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
@@ -52,7 +54,9 @@
     uint64_t closed_windows = 0;
 };
 
-// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
+// SAMPLED BY PARSE PASS. A per-op countdown would restore the store this sampler removes: no
+// existing io-side command counter covers both the point paths and all io-local commands.
+// The io loop asks pass_sampled() once per dispatched
 // frame -- one load of this writer's own word, on the line the old enabled() test already read,
 // and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
 // for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
diff --git a//home/user/Projects/cx-inline-comments/src/core/server.h b//home/user/Projects/cx-inline-comments/src/core/server.h
index f869dd3d1737afa6b1c401919fb66c96772b1225..f24f7a00e809e910c07736a056e4478c9ff3c925
--- a//home/user/Projects/cx-inline-comments/src/core/server.h
+++ b//home/user/Projects/cx-inline-comments/src/core/server.h
@@ -8,8 +8,10 @@
 //
 // THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
 // dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
-// its derived shard-granularity fast path, published only at a quiesced commit. See
-// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
+// its derived shard-granularity fast path, published only at a quiesced commit. Shards move only
+// after executing, deferred, group and pinned-read work drains; destination routes publish after
+// the ownership edge. An IO may enqueue using an old route after that edge, so tasks and borrow
+// releases recheck ownership and forward before shard access instead of carrying routing epochs.
 #pragma once
 #include <algorithm>
 #include <atomic>
@@ -308,7 +310,7 @@
         for (uint32_t i = 0; i < nthreads; i++) {
             threads_[i] = std::make_unique<ThreadCtx>();
             // The fingerprint writer is armed only when its one reader, the flip controller, is
-            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
+            // enabled: with --flip-auto 0 and in 1s mode it is dark and costs
             // one predicted branch per op. The enabled sampler keeps its measured 1-in-100 policy.
             threads_[i]->init(i, placement_.role_of(i), nthreads,
                               0,
@@ -1969,7 +1971,7 @@
         shard_owner_[shard_id].store(thread_id, std::memory_order_release);
     }
 
-    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
+    // The caller must hold both executor loops at the safe point:
     // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
     // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
     // Vector capacity and membership are settled while PREPARING still names the source.  The
@@ -1988,7 +1990,7 @@
     // lost, a block ends up linked twice, and the damage surfaces later as either
     // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
     // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
-    // Both were observed (see DESIGN-P0REPLY.md).
+    // Both were observed.
     //
     // The rebind used to be deferred to the destination's own next executor pass
     // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
@@ -2827,7 +2829,7 @@
     // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
     // other command. It widens -- deterministically -- the window in which a younger whole-owner
     // walker could overtake an older same-connection group on the destination shard.
-    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
+    // TEST HOOK: effective local-read LANE CAPACITY for admission. 0 means
     // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
     // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
     // its masking, so this can never overrun anything. It exists because lane oversubscription is
@@ -2935,8 +2937,10 @@
     // is already set. Nothing on GET/SET touches it.
     //
     // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
-    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
-    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
+    // sequence: a blocking op requires an empty ROB, then sets the barrier before any younger
+    // frame can be parsed; a resumed scatter inherits that same slot and barrier. The state the
+    // owner-scoped release exists to survive -- a release that must NOT drop the barrier -- has
+    // to be injected. It is held past
     // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
     //
     // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
@@ -2968,8 +2972,8 @@
         return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
     }
     // An overlap: some owner took the parse barrier while another owner already held it. With
-    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
-    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
+    // DEBUG BARRIER-HOLD off this must read ZERO -- if it ever moves on production traffic the
+    // latent case just went
     // live and the owner-scoped release in blocking_retire() became load-bearing rather than
     // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
     // own positive control: a "must be zero" reading proves nothing until something has been shown
diff --git a//home/user/Projects/cx-inline-comments/src/core/shard.h b//home/user/Projects/cx-inline-comments/src/core/shard.h
index 694b9a93c6cd63db49b9dcf061dbb5182c49986a..39a049d2f8619bbf52f706775956cc9198060364
--- a//home/user/Projects/cx-inline-comments/src/core/shard.h
+++ b//home/user/Projects/cx-inline-comments/src/core/shard.h
@@ -431,7 +431,7 @@
         // NOT share this counter: multi.inc's prepare_write_key() re-asks it at install time and
         // reports violations through INFO atomic_exec_order_late (multi_exec_order_late()), which
         // must read zero. Summing the two would hide a violation inside a number that is non-zero
-        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
+        // by design, leaving the park's safety argument with no falsifier.
         uint64_t atomic_exec_order_holds = 0;
         // Times watch_finalize_reservation() answered "not ready" because the reservation's epoch
         // was still 0, i.e. a unit was turned into a Retry by an undecided WATCH reservation. It
diff --git a//home/user/Projects/cx-inline-comments/src/core/signal.h b//home/user/Projects/cx-inline-comments/src/core/signal.h
index 933a5d7ee09538f3923df3c5eb77f9c72cdc0bfa..f69f7d73942ee129f4ad9cd1f23befe69fc4167d
--- a//home/user/Projects/cx-inline-comments/src/core/signal.h
+++ b//home/user/Projects/cx-inline-comments/src/core/signal.h
@@ -47,7 +47,7 @@
 // that will be compared against a deadline must come from here and never from now_ns() above --
 // that one is CLOCK_MONOTONIC, so its "milliseconds" are milliseconds since boot and sit roughly
 // five orders of magnitude below any real deadline. A monotonic value used as an expiry cut does
-// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
+// not skew the answer, it disables expiry outright.
 inline int64_t now_realtime_ms() {
     timespec ts;
     clock_gettime(CLOCK_REALTIME, &ts);
diff --git a//home/user/Projects/cx-inline-comments/src/core/thread.h b//home/user/Projects/cx-inline-comments/src/core/thread.h
index 0fcfa17ba6e331309260b641ec21aa4a8ff9ba36..3cf72c1b96ca700f602e2e7c630a1e6970f9620f
--- a//home/user/Projects/cx-inline-comments/src/core/thread.h
+++ b//home/user/Projects/cx-inline-comments/src/core/thread.h
@@ -118,7 +118,7 @@
     ContextRoute,           // scatter/script/all-shard or conservative broad-owner route
     ContextKeymissNotify,   // MGET miss must retain owner-side notification behavior
     InflightWrite,
-    ArmTransient,           // pre-arming writes still in flight (DESIGN-RINGDIET.md)
+    ArmTransient,           // pre-arming writes still in flight
     AtomicPending,
     Missing,
     Typed,
@@ -151,7 +151,7 @@
     // ARM-ON-DEMAND TRANSIENT. Reads demoted because the connection carried writes published
     // BEFORE a local read armed it -- writes the RYOW ring deliberately never recorded. Bounded
     // per connection by one ROB drain and reported separately from the steady-state key conflict
-    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
+    // above so that the two can never be confused in a bench.
     uint64_t fallback_arm_transient = 0;
     uint64_t fallback_atomic_pending = 0;
     uint64_t fallback_missing = 0;
@@ -160,7 +160,7 @@
     uint64_t fallback_seq_churn = 0;
     uint64_t fallback_generation = 0;
     uint64_t fallback_lane_full = 0;
-    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
+    // Lane ADMISSION deferrals. Frames the armed parser left unconsumed at rpos, to be
     // re-parsed by a later pass of the same thread, because the local-read lane had no room
     // (defer_lane_full) or because the connection already held its fair share of a lane under
     // pressure (defer_quota). Neither is a fallback: the read still completes locally, and no

codex
Most comments already contain the reasoning they cite, so those need only the pointer removed. I’m adding the missing explanations for stale-route forwarding, the mid-command EXEC deadlock, barrier reachability, and sampled fingerprint detection. The archive review also exposed some stale claims; I’ll list those separately in `INLINE.md`.

diff --git a//home/user/Projects/cx-inline-comments/src/core/config.h b//home/user/Projects/cx-inline-comments/src/core/config.h
index 495e6ae42da15ee4bcc76a8a846efeef506903d7..8b5f3e5c786e8eeae0296a5dcf19338056d59007
--- a//home/user/Projects/cx-inline-comments/src/core/config.h
+++ b//home/user/Projects/cx-inline-comments/src/core/config.h
@@ -290,8 +290,8 @@
     uint32_t client_lb = 1;
 
     // ---- automatic role split (boot-latched) -----------------------------------------------
-    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer
-    // (DESIGN-flipfp.md); its measured sampling policy is internal to flipctl.h. Off and 1s
+    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer;
+    // its measured sampling policy is internal to flipctl.h. Off and 1s
     // dispatch pay one predicted branch per op and no fingerprint store. Trigger bands learn
     // from the anchor's own quiet jitter; no separate controller tuning knobs remain.
     uint32_t flip_auto = 0;
diff --git a//home/user/Projects/cx-inline-comments/src/core/flipctl.h b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
index 5185da4085ae5f2e3325702064208810cb9c42f7..1edc3a93889090da659db0c9ee259d04ce5cef57
--- a//home/user/Projects/cx-inline-comments/src/core/flipctl.h
+++ b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
@@ -19,7 +19,9 @@
 
 class Server;
 
-// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
+// Preserve the measured sampler: one whole parse pass in 100 on average. At a matched 512k ops/s,
+// a GET-only -> SET-only swap triggered the fingerprint detector once with both exhaustive and
+// 1-in-100 sampling; the sampled writer still detected the shift with the rate held fixed.
 // Arming derives solely from the controller latch. No runtime sampling knob or adaptive
 // per-operation work is needed; the detector already derives its bands from actual samples.
 inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
@@ -52,7 +54,9 @@
     uint64_t closed_windows = 0;
 };
 
-// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
+// SAMPLED BY PARSE PASS. A per-op countdown would restore the store this sampler removes: no
+// existing io-side command counter covers both the point paths and all io-local commands.
+// The io loop asks pass_sampled() once per dispatched
 // frame -- one load of this writer's own word, on the line the old enabled() test already read,
 // and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
 // for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
diff --git a//home/user/Projects/cx-inline-comments/src/core/server.h b//home/user/Projects/cx-inline-comments/src/core/server.h
index f869dd3d1737afa6b1c401919fb66c96772b1225..f24f7a00e809e910c07736a056e4478c9ff3c925
--- a//home/user/Projects/cx-inline-comments/src/core/server.h
+++ b//home/user/Projects/cx-inline-comments/src/core/server.h
@@ -8,8 +8,10 @@
 //
 // THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
 // dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
-// its derived shard-granularity fast path, published only at a quiesced commit. See
-// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
+// its derived shard-granularity fast path, published only at a quiesced commit. Shards move only
+// after executing, deferred, group and pinned-read work drains; destination routes publish after
+// the ownership edge. An IO may enqueue using an old route after that edge, so tasks and borrow
+// releases recheck ownership and forward before shard access instead of carrying routing epochs.
 #pragma once
 #include <algorithm>
 #include <atomic>
@@ -308,7 +310,7 @@
         for (uint32_t i = 0; i < nthreads; i++) {
             threads_[i] = std::make_unique<ThreadCtx>();
             // The fingerprint writer is armed only when its one reader, the flip controller, is
-            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
+            // enabled: with --flip-auto 0 and in 1s mode it is dark and costs
             // one predicted branch per op. The enabled sampler keeps its measured 1-in-100 policy.
             threads_[i]->init(i, placement_.role_of(i), nthreads,
                               0,
@@ -1969,7 +1971,7 @@
         shard_owner_[shard_id].store(thread_id, std::memory_order_release);
     }
 
-    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
+    // The caller must hold both executor loops at the safe point:
     // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
     // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
     // Vector capacity and membership are settled while PREPARING still names the source.  The
@@ -1988,7 +1990,7 @@
     // lost, a block ends up linked twice, and the damage surfaces later as either
     // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
     // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
-    // Both were observed (see DESIGN-P0REPLY.md).
+    // Both were observed.
     //
     // The rebind used to be deferred to the destination's own next executor pass
     // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
@@ -2827,7 +2829,7 @@
     // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
     // other command. It widens -- deterministically -- the window in which a younger whole-owner
     // walker could overtake an older same-connection group on the destination shard.
-    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
+    // TEST HOOK: effective local-read LANE CAPACITY for admission. 0 means
     // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
     // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
     // its masking, so this can never overrun anything. It exists because lane oversubscription is
@@ -2935,8 +2937,10 @@
     // is already set. Nothing on GET/SET touches it.
     //
     // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
-    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
-    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
+    // sequence: a blocking op requires an empty ROB, then sets the barrier before any younger
+    // frame can be parsed; a resumed scatter inherits that same slot and barrier. The state the
+    // owner-scoped release exists to survive -- a release that must NOT drop the barrier -- has
+    // to be injected. It is held past
     // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
     //
     // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
@@ -2968,8 +2972,8 @@
         return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
     }
     // An overlap: some owner took the parse barrier while another owner already held it. With
-    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
-    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
+    // DEBUG BARRIER-HOLD off this must read ZERO -- if it ever moves on production traffic the
+    // latent case just went
     // live and the owner-scoped release in blocking_retire() became load-bearing rather than
     // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
     // own positive control: a "must be zero" reading proves nothing until something has been shown
diff --git a//home/user/Projects/cx-inline-comments/src/core/shard.h b//home/user/Projects/cx-inline-comments/src/core/shard.h
index 694b9a93c6cd63db49b9dcf061dbb5182c49986a..39a049d2f8619bbf52f706775956cc9198060364
--- a//home/user/Projects/cx-inline-comments/src/core/shard.h
+++ b//home/user/Projects/cx-inline-comments/src/core/shard.h
@@ -431,7 +431,7 @@
         // NOT share this counter: multi.inc's prepare_write_key() re-asks it at install time and
         // reports violations through INFO atomic_exec_order_late (multi_exec_order_late()), which
         // must read zero. Summing the two would hide a violation inside a number that is non-zero
-        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
+        // by design, leaving the park's safety argument with no falsifier.
         uint64_t atomic_exec_order_holds = 0;
         // Times watch_finalize_reservation() answered "not ready" because the reservation's epoch
         // was still 0, i.e. a unit was turned into a Retry by an undecided WATCH reservation. It
diff --git a//home/user/Projects/cx-inline-comments/src/core/signal.h b//home/user/Projects/cx-inline-comments/src/core/signal.h
index 933a5d7ee09538f3923df3c5eb77f9c72cdc0bfa..f69f7d73942ee129f4ad9cd1f23befe69fc4167d
--- a//home/user/Projects/cx-inline-comments/src/core/signal.h
+++ b//home/user/Projects/cx-inline-comments/src/core/signal.h
@@ -47,7 +47,7 @@
 // that will be compared against a deadline must come from here and never from now_ns() above --
 // that one is CLOCK_MONOTONIC, so its "milliseconds" are milliseconds since boot and sit roughly
 // five orders of magnitude below any real deadline. A monotonic value used as an expiry cut does
-// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
+// not skew the answer, it disables expiry outright.
 inline int64_t now_realtime_ms() {
     timespec ts;
     clock_gettime(CLOCK_REALTIME, &ts);
diff --git a//home/user/Projects/cx-inline-comments/src/core/thread.h b//home/user/Projects/cx-inline-comments/src/core/thread.h
index 0fcfa17ba6e331309260b641ec21aa4a8ff9ba36..3cf72c1b96ca700f602e2e7c630a1e6970f9620f
--- a//home/user/Projects/cx-inline-comments/src/core/thread.h
+++ b//home/user/Projects/cx-inline-comments/src/core/thread.h
@@ -118,7 +118,7 @@
     ContextRoute,           // scatter/script/all-shard or conservative broad-owner route
     ContextKeymissNotify,   // MGET miss must retain owner-side notification behavior
     InflightWrite,
-    ArmTransient,           // pre-arming writes still in flight (DESIGN-RINGDIET.md)
+    ArmTransient,           // pre-arming writes still in flight
     AtomicPending,
     Missing,
     Typed,
@@ -151,7 +151,7 @@
     // ARM-ON-DEMAND TRANSIENT. Reads demoted because the connection carried writes published
     // BEFORE a local read armed it -- writes the RYOW ring deliberately never recorded. Bounded
     // per connection by one ROB drain and reported separately from the steady-state key conflict
-    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
+    // above so that the two can never be confused in a bench.
     uint64_t fallback_arm_transient = 0;
     uint64_t fallback_atomic_pending = 0;
     uint64_t fallback_missing = 0;
@@ -160,7 +160,7 @@
     uint64_t fallback_seq_churn = 0;
     uint64_t fallback_generation = 0;
     uint64_t fallback_lane_full = 0;
-    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
+    // Lane ADMISSION deferrals. Frames the armed parser left unconsumed at rpos, to be
     // re-parsed by a later pass of the same thread, because the local-read lane had no room
     // (defer_lane_full) or because the connection already held its fair share of a lane under
     // pressure (defer_quota). Neither is a fallback: the read still completes locally, and no

exec
/bin/bash -lc "rg -n 'BarrierOwner|Sleep|barrier_acquire|barrier_release' src/net/conn.h | head -50
sed -n '4395,4465p' src/core/io_loop.h
sed -n '2070,2105p' src/core/server.h
sed -n '2180,2210p' src/core/server.h
rg -n 'forward|worker_of_shard' src/core/ex_loop.h | head -30" in /home/user/Projects/cx-inline-comments
 succeeded in 0ms:
94:enum class BarrierOwner : uint8_t {
101:    Sleep    = 1u << 6,  // deferred DEBUG SLEEP connection timer
598:    // set is a MASK in the byte the bool already occupied (see BarrierOwner), not a new field.
600:    bool barrier_held_by(BarrierOwner who) const {
603:    void barrier_acquire(BarrierOwner who) {
609:    bool barrier_release(BarrierOwner who) {
616:    void barrier_release_quiesced() {
618:            static_cast<uint8_t>(barrier_owners_ & static_cast<uint8_t>(BarrierOwner::Debug));
799:    // BarrierOwner mask, not a bool. Same byte, same offset, same alignment: it occupies the slot
    }

    __attribute__((noinline))
    void flip_fingerprint_note_sampled(const CommandSpec& spec, const Op& op) {
        FlipFingerprintWriter& writer = self_->flip_fingerprint();
        uint32_t keys = 0;
        uint32_t first = 0, last = 0, step = 1;
        if (spec.first_key > 0 && static_cast<uint32_t>(spec.first_key) < op.argc()) {
            first = static_cast<uint32_t>(spec.first_key);
            last = spec.last_key < 0 ? op.argc() - 1
                                     : std::min<uint32_t>(spec.last_key, op.argc() - 1);
            step = spec.key_step > 0 ? static_cast<uint32_t>(spec.key_step) : 1;
            if (last >= first) keys = (last - first) / step + 1;
        }
        uint64_t value_bytes = 0;
        for (uint32_t arg = 1; arg < op.argc(); arg++) {
            const bool key = keys && arg >= first && arg <= last && (arg - first) % step == 0;
            if (!key) value_bytes += op.arg(arg).n;
        }
        FlipFingerprintClass command_class = FlipFingerprintClass::Other;
        if (spec.flags & CmdFlags::Blocking) {
            command_class = FlipFingerprintClass::Blocking;
        } else if (keys > 1 && srv_->cfg().atomic &&
                   (spec.flags & CmdFlags::MultiShard)) {
            command_class = FlipFingerprintClass::AtomicGrouped;
        } else if (keys > 1) {
            command_class = spec.flags & (CmdFlags::Write | CmdFlags::SnapshotWrite)
                ? FlipFingerprintClass::MultiWrite : FlipFingerprintClass::MultiRead;
        } else if (spec.flags & (CmdFlags::Write | CmdFlags::SnapshotWrite)) {
            command_class = FlipFingerprintClass::Write;
        } else if (spec.flags & CmdFlags::Readonly) {
            command_class = FlipFingerprintClass::Read;
        }
        writer.note_command(command_class, keys, value_bytes);
    }

    // THE ONE DOOR ONTO THE PARSE BARRIER. Every owner parks a connection through here so the
    // overlap -- two owners holding the barrier at once -- is COUNTED rather than assumed absent.
    // NOTES-BARRIER.md section 2 argues from the source that no production sequence produces one
    // today; barrier_owner_overlaps is that argument's live assertion, and a validation run that
    // wants the two-owner geometry gates on it rather than trusting the prose. Cold by
    // construction: the seven owners are EXEC, a subscribe, a blocking command, a deferred WAIT,
    // a deferred DEBUG SLEEP, a barriered scatter and a CLIENT fan-out. GET and SET never reach it.
    void barrier_arm(Client* c, BarrierOwner who) {
        if (__builtin_expect(c->scatter_barrier(), false)) srv_->note_barrier_overlap();
        c->barrier_acquire(who);
    }

    void finish_locally(Client* c, Op& op, const char* err) {
        reply_err(op.reply, err);
        finish_prebuilt(c, op);
    }

    void finish_prebuilt(Client* c, Op& op) {
        // Unknown-command and arity errors precede the ordinary armed gate.
        if (!op.spec && (climon_armed_cached_ & Server::kClimonReply)) climon_mark_reply(c, op);
        op.state.store(OpState::Done, std::memory_order_release);
        c->rob().publish();
        enqueue_serve(c);
        mark_active(c);
    }

    uint64_t next_random() {
        random_state_ ^= random_state_ << 13;
        random_state_ ^= random_state_ >> 7;
        random_state_ ^= random_state_ << 17;
        return random_state_;
    }

    // Queue a teardown instead of performing it. Used only by the epoll engine's synchronous
    // read/send paths, which run inside flush_ready's walk over the active set. Deliberately does
        try {
            moving.reserve(shards_.size());
        } catch (...) {
            return false;
        }
        uint32_t cursor = begin;
        for (const auto& owned : shards_) {
            Shard* const shard = owned.get();
            if (shard->bucket_end() <= begin || shard->bucket_begin() >= end) continue;
            if (shard->bucket_begin() != cursor || shard->bucket_end() > end ||
                worker_of_shard(shard->id()) != source) return false;
            moving.push_back(shard);
            cursor = shard->bucket_end();
        }
        if (moving.empty() || cursor != end) return false;

        auto& from = threads_[source]->shards();
        auto& to = threads_[destination]->shards();
        for (Shard* shard : moving) {
            if (std::find(from.begin(), from.end(), shard) == from.end() ||
                std::find(to.begin(), to.end(), shard) != to.end()) return false;
        }

        // Allocation is the sole ordinary failure after validation, so force it before publishing
        // PREPARING. No recoverable operation remains after begin_transfer succeeds.
        try {
            to.reserve(to.size() + moving.size());
        } catch (...) {
            return false;
        }
        if (!router_.begin_transfer(begin, end, source, destination))
            return false;

        for (Shard* shard : moving) to.push_back(shard);
        from.erase(std::remove_if(from.begin(), from.end(), [&](Shard* shard) {
            return std::find(moving.begin(), moving.end(), shard) != moving.end();
        return cfg_.thread_mode == ThreadMode::Fused
            ? placement_.is_executor(tid) : thread(tid).role() == Role::Ex;
    }
    uint32_t executor_slot(uint32_t thread_id) const {
        // Atomic-group arrays use stable physical thread IDs, including across role changes.
        return thread_id < nthreads() ? thread_id : UINT8_MAX;
    }

    std::atomic<uint64_t>& next_client_id() { return next_client_id_; }
    std::atomic<bool>&     shutting_down()  { return shutting_down_; }

    uint32_t maxclients() const { return live_maxclients_.load(std::memory_order_relaxed); }
    void set_maxclients(uint32_t value) {
        live_maxclients_.store(value, std::memory_order_relaxed);
    }
    uint64_t live_clients() const { return live_clients_.load(std::memory_order_relaxed); }
    void client_accepted() { live_clients_.fetch_add(1, std::memory_order_relaxed); }
    void client_released() {
        if (live_clients_.fetch_sub(1, std::memory_order_relaxed) == 0) std::abort();
    }
    void note_rejected_conn() { rejected_conns_.fetch_add(1, std::memory_order_relaxed); }
    uint64_t rejected_conns() const { return rejected_conns_.load(std::memory_order_relaxed); }

    uint32_t tcp_keepalive() const {
        return live_tcp_keepalive_.load(std::memory_order_relaxed);
    }
    void set_tcp_keepalive(uint32_t value) {
        live_tcp_keepalive_.store(value, std::memory_order_relaxed);
    }
    uint32_t timeout() const { return live_timeout_.load(std::memory_order_relaxed); }
    bool client_cron_armed() const {
497:                did += service_stale_forwards<BatchOps, IofusedPrivateQueue>();
521:            did += service_stale_forwards<BatchOps, IofusedPrivateQueue>();
743:                        did += service_stale_forwards();
762:                    did += service_stale_forwards();
1816:                     service_stale_forwards<BatchOps, IofusedPrivateQueue>() +
1922:            // Recheck before the first FlatStore touch and forward the stale release intact.
1923:            if (forward_stale_release(r)) return;
2210:        // execute()'s forwarding guard runs too late for this wrapper.
2211:        if (forward_stale_task<IofusedPrivateQueue>(task)) return true;
2392:                srv_->worker_of_shard(shard) == self_->id() &&
2492:        if (forward_stale_task<IofusedPrivateQueue>(t)) return true;
2834:    bool post_forwarded_task(const Task& task, uint32_t target) {
2847:    bool forward_stale_task(const Task& task) {
2850:        const uint32_t target = srv_->worker_of_shard(sid);
2852:        if (!post_forwarded_task<IofusedPrivateQueue>(task, target))
2857:    bool post_forwarded_release(const BorrowRelease& release, uint32_t target) {
2862:    bool forward_stale_release(const BorrowRelease& release) {
2864:        const uint32_t target = srv_->worker_of_shard(release.shard);
2866:        if (!post_forwarded_release(release, target)) stale_releases_.push_back(release);
2872:    uint32_t service_stale_forwards() {
2878:            const uint32_t target = srv_->worker_of_shard(sid);
2886:                        if (!post_forwarded_task(task, target)) break;
2897:            if (!post_forwarded_task<IofusedPrivateQueue>(task, target)) break;
2903:            const uint32_t target = srv_->worker_of_shard(release.shard);
2910:            if (!post_forwarded_release(release, target)) break;

apply patch
patch: completed
/home/user/Projects/cx-inline-comments/src/core/server.h
diff --git a//home/user/Projects/cx-inline-comments/src/core/config.h b//home/user/Projects/cx-inline-comments/src/core/config.h
index 495e6ae42da15ee4bcc76a8a846efeef506903d7..8b5f3e5c786e8eeae0296a5dcf19338056d59007
--- a//home/user/Projects/cx-inline-comments/src/core/config.h
+++ b//home/user/Projects/cx-inline-comments/src/core/config.h
@@ -290,8 +290,8 @@
     uint32_t client_lb = 1;
 
     // ---- automatic role split (boot-latched) -----------------------------------------------
-    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer
-    // (DESIGN-flipfp.md); its measured sampling policy is internal to flipctl.h. Off and 1s
+    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer;
+    // its measured sampling policy is internal to flipctl.h. Off and 1s
     // dispatch pay one predicted branch per op and no fingerprint store. Trigger bands learn
     // from the anchor's own quiet jitter; no separate controller tuning knobs remain.
     uint32_t flip_auto = 0;
diff --git a//home/user/Projects/cx-inline-comments/src/core/flipctl.h b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
index 5185da4085ae5f2e3325702064208810cb9c42f7..1edc3a93889090da659db0c9ee259d04ce5cef57
--- a//home/user/Projects/cx-inline-comments/src/core/flipctl.h
+++ b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
@@ -19,7 +19,9 @@
 
 class Server;
 
-// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
+// Preserve the measured sampler: one whole parse pass in 100 on average. At a matched 512k ops/s,
+// a GET-only -> SET-only swap triggered the fingerprint detector once with both exhaustive and
+// 1-in-100 sampling; the sampled writer still detected the shift with the rate held fixed.
 // Arming derives solely from the controller latch. No runtime sampling knob or adaptive
 // per-operation work is needed; the detector already derives its bands from actual samples.
 inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
@@ -52,7 +54,9 @@
     uint64_t closed_windows = 0;
 };
 
-// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
+// SAMPLED BY PARSE PASS. A per-op countdown would restore the store this sampler removes: no
+// existing io-side command counter covers both the point paths and all io-local commands.
+// The io loop asks pass_sampled() once per dispatched
 // frame -- one load of this writer's own word, on the line the old enabled() test already read,
 // and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
 // for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
diff --git a//home/user/Projects/cx-inline-comments/src/core/server.h b//home/user/Projects/cx-inline-comments/src/core/server.h
index f869dd3d1737afa6b1c401919fb66c96772b1225..c4347d94d76da5357ad0ea2ed6a0464eb16c106e
--- a//home/user/Projects/cx-inline-comments/src/core/server.h
+++ b//home/user/Projects/cx-inline-comments/src/core/server.h
@@ -8,8 +8,10 @@
 //
 // THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
 // dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
-// its derived shard-granularity fast path, published only at a quiesced commit. See
-// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
+// its derived shard-granularity fast path, published only at a quiesced commit. Shards move only
+// after executing, deferred, group and pinned-read work drains; destination routes publish after
+// the ownership edge. An IO may enqueue using an old route after that edge, so tasks and borrow
+// releases recheck ownership and forward before shard access instead of carrying routing epochs.
 #pragma once
 #include <algorithm>
 #include <atomic>
@@ -308,7 +310,7 @@
         for (uint32_t i = 0; i < nthreads; i++) {
             threads_[i] = std::make_unique<ThreadCtx>();
             // The fingerprint writer is armed only when its one reader, the flip controller, is
-            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
+            // enabled: with --flip-auto 0 and in 1s mode it is dark and costs
             // one predicted branch per op. The enabled sampler keeps its measured 1-in-100 policy.
             threads_[i]->init(i, placement_.role_of(i), nthreads,
                               0,
@@ -1969,7 +1971,7 @@
         shard_owner_[shard_id].store(thread_id, std::memory_order_release);
     }
 
-    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
+    // The caller must hold both executor loops at the safe point:
     // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
     // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
     // Vector capacity and membership are settled while PREPARING still names the source.  The
@@ -1988,7 +1990,7 @@
     // lost, a block ends up linked twice, and the damage surfaces later as either
     // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
     // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
-    // Both were observed (see DESIGN-P0REPLY.md).
+    // Both were observed.
     //
     // The rebind used to be deferred to the destination's own next executor pass
     // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
@@ -2827,7 +2829,7 @@
     // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
     // other command. It widens -- deterministically -- the window in which a younger whole-owner
     // walker could overtake an older same-connection group on the destination shard.
-    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
+    // TEST HOOK: effective local-read LANE CAPACITY for admission. 0 means
     // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
     // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
     // its masking, so this can never overrun anything. It exists because lane oversubscription is
@@ -2935,9 +2937,11 @@
     // is already set. Nothing on GET/SET touches it.
     //
     // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
-    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
-    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
-    // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
+    // sequence: a blocking op requires an empty ROB, then sets the barrier before any younger
+    // frame can be parsed; a resumed scatter inherits that same slot and barrier. The state the
+    // owner-scoped release exists to survive -- a release that must NOT drop the barrier -- has
+    // to be injected. It is held past ROB quiescence on purpose; barrier_release_quiesced()
+    // exempts this one bit for that reason.
     //
     // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
     // nothing left to do and parks on submit_and_wait(1) -- unbounded under io_uring. A hold that
@@ -2968,9 +2972,8 @@
         return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
     }
     // An overlap: some owner took the parse barrier while another owner already held it. With
-    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
-    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
-    // live and the owner-scoped release in blocking_retire() became load-bearing rather than
+    // DEBUG BARRIER-HOLD off this must read ZERO -- if it ever moves on production traffic the
+    // latent case just went live and the owner-scoped release in blocking_retire() became load-bearing rather than
     // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
     // own positive control: a "must be zero" reading proves nothing until something has been shown
     // able to make it non-zero.
diff --git a//home/user/Projects/cx-inline-comments/src/core/shard.h b//home/user/Projects/cx-inline-comments/src/core/shard.h
index 694b9a93c6cd63db49b9dcf061dbb5182c49986a..39a049d2f8619bbf52f706775956cc9198060364
--- a//home/user/Projects/cx-inline-comments/src/core/shard.h
+++ b//home/user/Projects/cx-inline-comments/src/core/shard.h
@@ -431,7 +431,7 @@
         // NOT share this counter: multi.inc's prepare_write_key() re-asks it at install time and
         // reports violations through INFO atomic_exec_order_late (multi_exec_order_late()), which
         // must read zero. Summing the two would hide a violation inside a number that is non-zero
-        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
+        // by design, leaving the park's safety argument with no falsifier.
         uint64_t atomic_exec_order_holds = 0;
         // Times watch_finalize_reservation() answered "not ready" because the reservation's epoch
         // was still 0, i.e. a unit was turned into a Retry by an undecided WATCH reservation. It
diff --git a//home/user/Projects/cx-inline-comments/src/core/signal.h b//home/user/Projects/cx-inline-comments/src/core/signal.h
index 933a5d7ee09538f3923df3c5eb77f9c72cdc0bfa..f69f7d73942ee129f4ad9cd1f23befe69fc4167d
--- a//home/user/Projects/cx-inline-comments/src/core/signal.h
+++ b//home/user/Projects/cx-inline-comments/src/core/signal.h
@@ -47,7 +47,7 @@
 // that will be compared against a deadline must come from here and never from now_ns() above --
 // that one is CLOCK_MONOTONIC, so its "milliseconds" are milliseconds since boot and sit roughly
 // five orders of magnitude below any real deadline. A monotonic value used as an expiry cut does
-// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
+// not skew the answer, it disables expiry outright.
 inline int64_t now_realtime_ms() {
     timespec ts;
     clock_gettime(CLOCK_REALTIME, &ts);
diff --git a//home/user/Projects/cx-inline-comments/src/core/thread.h b//home/user/Projects/cx-inline-comments/src/core/thread.h
index 0fcfa17ba6e331309260b641ec21aa4a8ff9ba36..3cf72c1b96ca700f602e2e7c630a1e6970f9620f
--- a//home/user/Projects/cx-inline-comments/src/core/thread.h
+++ b//home/user/Projects/cx-inline-comments/src/core/thread.h
@@ -118,7 +118,7 @@
     ContextRoute,           // scatter/script/all-shard or conservative broad-owner route
     ContextKeymissNotify,   // MGET miss must retain owner-side notification behavior
     InflightWrite,
-    ArmTransient,           // pre-arming writes still in flight (DESIGN-RINGDIET.md)
+    ArmTransient,           // pre-arming writes still in flight
     AtomicPending,
     Missing,
     Typed,
@@ -151,7 +151,7 @@
     // ARM-ON-DEMAND TRANSIENT. Reads demoted because the connection carried writes published
     // BEFORE a local read armed it -- writes the RYOW ring deliberately never recorded. Bounded
     // per connection by one ROB drain and reported separately from the steady-state key conflict
-    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
+    // above so that the two can never be confused in a bench.
     uint64_t fallback_arm_transient = 0;
     uint64_t fallback_atomic_pending = 0;
     uint64_t fallback_missing = 0;
@@ -160,7 +160,7 @@
     uint64_t fallback_seq_churn = 0;
     uint64_t fallback_generation = 0;
     uint64_t fallback_lane_full = 0;
-    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
+    // Lane ADMISSION deferrals. Frames the armed parser left unconsumed at rpos, to be
     // re-parsed by a later pass of the same thread, because the local-read lane had no room
     // (defer_lane_full) or because the connection already held its fair share of a lane under
     // pressure (defer_quota). Neither is a fallback: the read still completes locally, and no

diff --git a//home/user/Projects/cx-inline-comments/src/core/config.h b//home/user/Projects/cx-inline-comments/src/core/config.h
index 495e6ae42da15ee4bcc76a8a846efeef506903d7..8b5f3e5c786e8eeae0296a5dcf19338056d59007
--- a//home/user/Projects/cx-inline-comments/src/core/config.h
+++ b//home/user/Projects/cx-inline-comments/src/core/config.h
@@ -290,8 +290,8 @@
     uint32_t client_lb = 1;
 
     // ---- automatic role split (boot-latched) -----------------------------------------------
-    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer
-    // (DESIGN-flipfp.md); its measured sampling policy is internal to flipctl.h. Off and 1s
+    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer;
+    // its measured sampling policy is internal to flipctl.h. Off and 1s
     // dispatch pay one predicted branch per op and no fingerprint store. Trigger bands learn
     // from the anchor's own quiet jitter; no separate controller tuning knobs remain.
     uint32_t flip_auto = 0;
diff --git a//home/user/Projects/cx-inline-comments/src/core/flipctl.h b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
index 5185da4085ae5f2e3325702064208810cb9c42f7..1edc3a93889090da659db0c9ee259d04ce5cef57
--- a//home/user/Projects/cx-inline-comments/src/core/flipctl.h
+++ b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
@@ -19,7 +19,9 @@
 
 class Server;
 
-// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
+// Preserve the measured sampler: one whole parse pass in 100 on average. At a matched 512k ops/s,
+// a GET-only -> SET-only swap triggered the fingerprint detector once with both exhaustive and
+// 1-in-100 sampling; the sampled writer still detected the shift with the rate held fixed.
 // Arming derives solely from the controller latch. No runtime sampling knob or adaptive
 // per-operation work is needed; the detector already derives its bands from actual samples.
 inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
@@ -52,7 +54,9 @@
     uint64_t closed_windows = 0;
 };
 
-// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
+// SAMPLED BY PARSE PASS. A per-op countdown would restore the store this sampler removes: no
+// existing io-side command counter covers both the point paths and all io-local commands.
+// The io loop asks pass_sampled() once per dispatched
 // frame -- one load of this writer's own word, on the line the old enabled() test already read,
 // and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
 // for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
diff --git a//home/user/Projects/cx-inline-comments/src/core/server.h b//home/user/Projects/cx-inline-comments/src/core/server.h
index f869dd3d1737afa6b1c401919fb66c96772b1225..c4347d94d76da5357ad0ea2ed6a0464eb16c106e
--- a//home/user/Projects/cx-inline-comments/src/core/server.h
+++ b//home/user/Projects/cx-inline-comments/src/core/server.h
@@ -8,8 +8,10 @@
 //
 // THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
 // dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
-// its derived shard-granularity fast path, published only at a quiesced commit. See
-// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
+// its derived shard-granularity fast path, published only at a quiesced commit. Shards move only
+// after executing, deferred, group and pinned-read work drains; destination routes publish after
+// the ownership edge. An IO may enqueue using an old route after that edge, so tasks and borrow
+// releases recheck ownership and forward before shard access instead of carrying routing epochs.
 #pragma once
 #include <algorithm>
 #include <atomic>
@@ -308,7 +310,7 @@
         for (uint32_t i = 0; i < nthreads; i++) {
             threads_[i] = std::make_unique<ThreadCtx>();
             // The fingerprint writer is armed only when its one reader, the flip controller, is
-            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
+            // enabled: with --flip-auto 0 and in 1s mode it is dark and costs
             // one predicted branch per op. The enabled sampler keeps its measured 1-in-100 policy.
             threads_[i]->init(i, placement_.role_of(i), nthreads,
                               0,
@@ -1969,7 +1971,7 @@
         shard_owner_[shard_id].store(thread_id, std::memory_order_release);
     }
 
-    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
+    // The caller must hold both executor loops at the safe point:
     // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
     // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
     // Vector capacity and membership are settled while PREPARING still names the source.  The
@@ -1988,7 +1990,7 @@
     // lost, a block ends up linked twice, and the damage surfaces later as either
     // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
     // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
-    // Both were observed (see DESIGN-P0REPLY.md).
+    // Both were observed.
     //
     // The rebind used to be deferred to the destination's own next executor pass
     // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
@@ -2827,7 +2829,7 @@
     // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
     // other command. It widens -- deterministically -- the window in which a younger whole-owner
     // walker could overtake an older same-connection group on the destination shard.
-    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
+    // TEST HOOK: effective local-read LANE CAPACITY for admission. 0 means
     // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
     // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
     // its masking, so this can never overrun anything. It exists because lane oversubscription is
@@ -2935,9 +2937,11 @@
     // is already set. Nothing on GET/SET touches it.
     //
     // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
-    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
-    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
-    // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
+    // sequence: a blocking op requires an empty ROB, then sets the barrier before any younger
+    // frame can be parsed; a resumed scatter inherits that same slot and barrier. The state the
+    // owner-scoped release exists to survive -- a release that must NOT drop the barrier -- has
+    // to be injected. It is held past ROB quiescence on purpose; barrier_release_quiesced()
+    // exempts this one bit for that reason.
     //
     // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
     // nothing left to do and parks on submit_and_wait(1) -- unbounded under io_uring. A hold that
@@ -2968,9 +2972,8 @@
         return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
     }
     // An overlap: some owner took the parse barrier while another owner already held it. With
-    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
-    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
-    // live and the owner-scoped release in blocking_retire() became load-bearing rather than
+    // DEBUG BARRIER-HOLD off this must read ZERO -- if it ever moves on production traffic the
+    // latent case just went live and the owner-scoped release in blocking_retire() became load-bearing rather than
     // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
     // own positive control: a "must be zero" reading proves nothing until something has been shown
     // able to make it non-zero.
diff --git a//home/user/Projects/cx-inline-comments/src/core/shard.h b//home/user/Projects/cx-inline-comments/src/core/shard.h
index 694b9a93c6cd63db49b9dcf061dbb5182c49986a..39a049d2f8619bbf52f706775956cc9198060364
--- a//home/user/Projects/cx-inline-comments/src/core/shard.h
+++ b//home/user/Projects/cx-inline-comments/src/core/shard.h
@@ -431,7 +431,7 @@
         // NOT share this counter: multi.inc's prepare_write_key() re-asks it at install time and
         // reports violations through INFO atomic_exec_order_late (multi_exec_order_late()), which
         // must read zero. Summing the two would hide a violation inside a number that is non-zero
-        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
+        // by design, leaving the park's safety argument with no falsifier.
         uint64_t atomic_exec_order_holds = 0;
         // Times watch_finalize_reservation() answered "not ready" because the reservation's epoch
         // was still 0, i.e. a unit was turned into a Retry by an undecided WATCH reservation. It
diff --git a//home/user/Projects/cx-inline-comments/src/core/signal.h b//home/user/Projects/cx-inline-comments/src/core/signal.h
index 933a5d7ee09538f3923df3c5eb77f9c72cdc0bfa..f69f7d73942ee129f4ad9cd1f23befe69fc4167d
--- a//home/user/Projects/cx-inline-comments/src/core/signal.h
+++ b//home/user/Projects/cx-inline-comments/src/core/signal.h
@@ -47,7 +47,7 @@
 // that will be compared against a deadline must come from here and never from now_ns() above --
 // that one is CLOCK_MONOTONIC, so its "milliseconds" are milliseconds since boot and sit roughly
 // five orders of magnitude below any real deadline. A monotonic value used as an expiry cut does
-// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
+// not skew the answer, it disables expiry outright.
 inline int64_t now_realtime_ms() {
     timespec ts;
     clock_gettime(CLOCK_REALTIME, &ts);
diff --git a//home/user/Projects/cx-inline-comments/src/core/thread.h b//home/user/Projects/cx-inline-comments/src/core/thread.h
index 0fcfa17ba6e331309260b641ec21aa4a8ff9ba36..3cf72c1b96ca700f602e2e7c630a1e6970f9620f
--- a//home/user/Projects/cx-inline-comments/src/core/thread.h
+++ b//home/user/Projects/cx-inline-comments/src/core/thread.h
@@ -118,7 +118,7 @@
     ContextRoute,           // scatter/script/all-shard or conservative broad-owner route
     ContextKeymissNotify,   // MGET miss must retain owner-side notification behavior
     InflightWrite,
-    ArmTransient,           // pre-arming writes still in flight (DESIGN-RINGDIET.md)
+    ArmTransient,           // pre-arming writes still in flight
     AtomicPending,
     Missing,
     Typed,
@@ -151,7 +151,7 @@
     // ARM-ON-DEMAND TRANSIENT. Reads demoted because the connection carried writes published
     // BEFORE a local read armed it -- writes the RYOW ring deliberately never recorded. Bounded
     // per connection by one ROB drain and reported separately from the steady-state key conflict
-    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
+    // above so that the two can never be confused in a bench.
     uint64_t fallback_arm_transient = 0;
     uint64_t fallback_atomic_pending = 0;
     uint64_t fallback_missing = 0;
@@ -160,7 +160,7 @@
     uint64_t fallback_seq_churn = 0;
     uint64_t fallback_generation = 0;
     uint64_t fallback_lane_full = 0;
-    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
+    // Lane ADMISSION deferrals. Frames the armed parser left unconsumed at rpos, to be
     // re-parsed by a later pass of the same thread, because the local-read lane had no room
     // (defer_lane_full) or because the connection already held its fair share of a lane under
     // pressure (defer_quota). Neither is a fallback: the read still completes locally, and no

apply patch
patch: completed
/home/user/Projects/cx-inline-comments/src/core/server.h
diff --git a//home/user/Projects/cx-inline-comments/src/core/config.h b//home/user/Projects/cx-inline-comments/src/core/config.h
index 495e6ae42da15ee4bcc76a8a846efeef506903d7..8b5f3e5c786e8eeae0296a5dcf19338056d59007
--- a//home/user/Projects/cx-inline-comments/src/core/config.h
+++ b//home/user/Projects/cx-inline-comments/src/core/config.h
@@ -290,8 +290,8 @@
     uint32_t client_lb = 1;
 
     // ---- automatic role split (boot-latched) -----------------------------------------------
-    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer
-    // (DESIGN-flipfp.md); its measured sampling policy is internal to flipctl.h. Off and 1s
+    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer;
+    // its measured sampling policy is internal to flipctl.h. Off and 1s
     // dispatch pay one predicted branch per op and no fingerprint store. Trigger bands learn
     // from the anchor's own quiet jitter; no separate controller tuning knobs remain.
     uint32_t flip_auto = 0;
diff --git a//home/user/Projects/cx-inline-comments/src/core/flipctl.h b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
index 5185da4085ae5f2e3325702064208810cb9c42f7..1edc3a93889090da659db0c9ee259d04ce5cef57
--- a//home/user/Projects/cx-inline-comments/src/core/flipctl.h
+++ b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
@@ -19,7 +19,9 @@
 
 class Server;
 
-// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
+// Preserve the measured sampler: one whole parse pass in 100 on average. At a matched 512k ops/s,
+// a GET-only -> SET-only swap triggered the fingerprint detector once with both exhaustive and
+// 1-in-100 sampling; the sampled writer still detected the shift with the rate held fixed.
 // Arming derives solely from the controller latch. No runtime sampling knob or adaptive
 // per-operation work is needed; the detector already derives its bands from actual samples.
 inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
@@ -52,7 +54,9 @@
     uint64_t closed_windows = 0;
 };
 
-// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
+// SAMPLED BY PARSE PASS. A per-op countdown would restore the store this sampler removes: no
+// existing io-side command counter covers both the point paths and all io-local commands.
+// The io loop asks pass_sampled() once per dispatched
 // frame -- one load of this writer's own word, on the line the old enabled() test already read,
 // and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
 // for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
diff --git a//home/user/Projects/cx-inline-comments/src/core/server.h b//home/user/Projects/cx-inline-comments/src/core/server.h
index f869dd3d1737afa6b1c401919fb66c96772b1225..c131a43aa82fde16228678b5ad0b0d9a1a54aa18
--- a//home/user/Projects/cx-inline-comments/src/core/server.h
+++ b//home/user/Projects/cx-inline-comments/src/core/server.h
@@ -8,8 +8,10 @@
 //
 // THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
 // dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
-// its derived shard-granularity fast path, published only at a quiesced commit. See
-// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
+// its derived shard-granularity fast path, published only at a quiesced commit. Shards move only
+// after executing, deferred, group and pinned-read work drains; destination routes publish after
+// the ownership edge. An IO may enqueue using an old route after that edge, so tasks and borrow
+// releases recheck ownership and forward before shard access instead of carrying routing epochs.
 #pragma once
 #include <algorithm>
 #include <atomic>
@@ -308,7 +310,7 @@
         for (uint32_t i = 0; i < nthreads; i++) {
             threads_[i] = std::make_unique<ThreadCtx>();
             // The fingerprint writer is armed only when its one reader, the flip controller, is
-            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
+            // enabled: with --flip-auto 0 and in 1s mode it is dark and costs
             // one predicted branch per op. The enabled sampler keeps its measured 1-in-100 policy.
             threads_[i]->init(i, placement_.role_of(i), nthreads,
                               0,
@@ -1969,7 +1971,7 @@
         shard_owner_[shard_id].store(thread_id, std::memory_order_release);
     }
 
-    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
+    // The caller must hold both executor loops at the safe point:
     // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
     // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
     // Vector capacity and membership are settled while PREPARING still names the source.  The
@@ -1988,7 +1990,7 @@
     // lost, a block ends up linked twice, and the damage surfaces later as either
     // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
     // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
-    // Both were observed (see DESIGN-P0REPLY.md).
+    // Both were observed.
     //
     // The rebind used to be deferred to the destination's own next executor pass
     // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
@@ -2827,7 +2829,7 @@
     // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
     // other command. It widens -- deterministically -- the window in which a younger whole-owner
     // walker could overtake an older same-connection group on the destination shard.
-    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
+    // TEST HOOK: effective local-read LANE CAPACITY for admission. 0 means
     // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
     // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
     // its masking, so this can never overrun anything. It exists because lane oversubscription is
@@ -2935,9 +2937,11 @@
     // is already set. Nothing on GET/SET touches it.
     //
     // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
-    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
-    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
-    // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
+    // sequence: a blocking op requires an empty ROB, then sets the barrier before any younger
+    // frame can be parsed; a resumed scatter inherits that same slot and barrier. The state the
+    // owner-scoped release exists to survive -- a release that must NOT drop the barrier -- has
+    // to be injected. It is held past ROB quiescence on purpose; barrier_release_quiesced()
+    // exempts this one bit for that reason.
     //
     // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
     // nothing left to do and parks on submit_and_wait(1) -- unbounded under io_uring. A hold that
@@ -2968,10 +2972,10 @@
         return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
     }
     // An overlap: some owner took the parse barrier while another owner already held it. With
-    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
-    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
-    // live and the owner-scoped release in blocking_retire() became load-bearing rather than
-    // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
+    // DEBUG BARRIER-HOLD off this must read ZERO -- if it ever moves on production traffic the
+    // latent case just went live and the owner-scoped release in blocking_retire() became
+    // load-bearing rather than defensive. With the latch ON it advances once per blocking dispatch,
+    // which is the counter's
     // own positive control: a "must be zero" reading proves nothing until something has been shown
     // able to make it non-zero.
     void note_barrier_overlap() {
diff --git a//home/user/Projects/cx-inline-comments/src/core/shard.h b//home/user/Projects/cx-inline-comments/src/core/shard.h
index 694b9a93c6cd63db49b9dcf061dbb5182c49986a..39a049d2f8619bbf52f706775956cc9198060364
--- a//home/user/Projects/cx-inline-comments/src/core/shard.h
+++ b//home/user/Projects/cx-inline-comments/src/core/shard.h
@@ -431,7 +431,7 @@
         // NOT share this counter: multi.inc's prepare_write_key() re-asks it at install time and
         // reports violations through INFO atomic_exec_order_late (multi_exec_order_late()), which
         // must read zero. Summing the two would hide a violation inside a number that is non-zero
-        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
+        // by design, leaving the park's safety argument with no falsifier.
         uint64_t atomic_exec_order_holds = 0;
         // Times watch_finalize_reservation() answered "not ready" because the reservation's epoch
         // was still 0, i.e. a unit was turned into a Retry by an undecided WATCH reservation. It
diff --git a//home/user/Projects/cx-inline-comments/src/core/signal.h b//home/user/Projects/cx-inline-comments/src/core/signal.h
index 933a5d7ee09538f3923df3c5eb77f9c72cdc0bfa..f69f7d73942ee129f4ad9cd1f23befe69fc4167d
--- a//home/user/Projects/cx-inline-comments/src/core/signal.h
+++ b//home/user/Projects/cx-inline-comments/src/core/signal.h
@@ -47,7 +47,7 @@
 // that will be compared against a deadline must come from here and never from now_ns() above --
 // that one is CLOCK_MONOTONIC, so its "milliseconds" are milliseconds since boot and sit roughly
 // five orders of magnitude below any real deadline. A monotonic value used as an expiry cut does
-// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
+// not skew the answer, it disables expiry outright.
 inline int64_t now_realtime_ms() {
     timespec ts;
     clock_gettime(CLOCK_REALTIME, &ts);
diff --git a//home/user/Projects/cx-inline-comments/src/core/thread.h b//home/user/Projects/cx-inline-comments/src/core/thread.h
index 0fcfa17ba6e331309260b641ec21aa4a8ff9ba36..3cf72c1b96ca700f602e2e7c630a1e6970f9620f
--- a//home/user/Projects/cx-inline-comments/src/core/thread.h
+++ b//home/user/Projects/cx-inline-comments/src/core/thread.h
@@ -118,7 +118,7 @@
     ContextRoute,           // scatter/script/all-shard or conservative broad-owner route
     ContextKeymissNotify,   // MGET miss must retain owner-side notification behavior
     InflightWrite,
-    ArmTransient,           // pre-arming writes still in flight (DESIGN-RINGDIET.md)
+    ArmTransient,           // pre-arming writes still in flight
     AtomicPending,
     Missing,
     Typed,
@@ -151,7 +151,7 @@
     // ARM-ON-DEMAND TRANSIENT. Reads demoted because the connection carried writes published
     // BEFORE a local read armed it -- writes the RYOW ring deliberately never recorded. Bounded
     // per connection by one ROB drain and reported separately from the steady-state key conflict
-    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
+    // above so that the two can never be confused in a bench.
     uint64_t fallback_arm_transient = 0;
     uint64_t fallback_atomic_pending = 0;
     uint64_t fallback_missing = 0;
@@ -160,7 +160,7 @@
     uint64_t fallback_seq_churn = 0;
     uint64_t fallback_generation = 0;
     uint64_t fallback_lane_full = 0;
-    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
+    // Lane ADMISSION deferrals. Frames the armed parser left unconsumed at rpos, to be
     // re-parsed by a later pass of the same thread, because the local-read lane had no room
     // (defer_lane_full) or because the connection already held its fair share of a lane under
     // pressure (defer_quota). Neither is a fallback: the read still completes locally, and no

exec
/bin/bash -lc "sed -n '70,114p' src/net/conn.h
rg -n '\\.md' src/core/thread.h src/core/shard.h src/core/signal.h src/core/flipctl.h src/core/server.h src/core/config.h" in /home/user/Projects/cx-inline-comments
 exited 1 in 0ms:
// the allocation, so a busy connection pays ONE grow to its working size and idles at 1KB + that.
inline constexpr size_t   kWbufInline   = 512;
inline constexpr size_t   kWbufShed     = 64 * 1024;   // reply staging above this is a burst; shed it
// Largest single send the kernel accepts (Linux MAX_RW_COUNT); a CQE res is an int. Caps every
// legacy send request and the segment iovec window alike.
inline constexpr uint32_t kMaxSendBytes = 0x7ffff000u;

// Item 6: connection-lived execution-side state -- the third lifetime. Session-mutating commands
// are ConnLocal and run on the io thread, single-threaded per connection; handlers never see it,
// they see the snapshot the parser stamps into each op.
struct Session {
    uint32_t db_index = 0;
};

// WHO IS HOLDING THE PARSE BARRIER. Six independent mechanisms park a connection's parse pass, and
// they used to share ONE bool -- so any one of them could clear a barrier another one still needed.
// No reachable interleaving overlapped two owners (see NOTES-BARRIER.md section 2: a blocking op is
// provably alone in its ROB, and every other owner ends the parse pass on the spot), which is
// exactly why the bool survived: the hazard is one relaxed guard away, not present. Owner bits make
// the release symmetric with the acquire -- whoever set it is the one whose release can drop it --
// and cost nothing: the byte was already there, and "is any owner holding" is still one byte test.
//
// Adding an owner? Add a bit here and acquire it at the site that parks the connection. Do NOT
// reuse another owner's bit to save one; that is the shared flag this enum exists to retire.
enum class BarrierOwner : uint8_t {
    Scatter  = 1u << 0,  // two-hop store / all-shards fan-out; io_loop.h nonblocking_dispatch
    Blocking = 1u << 1,  // BLPOP-family, and the move scatter blocking_resume_move converts it into
    Wait     = 1u << 2,  // deferred WAIT parked on its own deadline
    Exec     = 1u << 3,  // EXEC fan-out; multi.inc
    PubSub   = 1u << 4,  // (P|S)(UN)SUBSCRIBE transition awaiting its channel homes
    Climon   = 1u << 5,  // CLIENT UNBLOCK (remote owner), CLIENT LIST / CLIENT KILL fan-out
    Sleep    = 1u << 6,  // deferred DEBUG SLEEP connection timer
    // Client is footprint-locked. All seven production bits are now assigned; an eighth owner
    // needs a real layout decision, not a wider field.
    //
    // Test-only, and deliberately NOT released by the quiescence backstop -- it exists to hold the
    // barrier PAST ROB quiescence, which is the state no production sequence can currently produce
    // and the one the owner-scoped release must survive. Armed only by DEBUG BARRIER-HOLD.
    Debug    = 1u << 7,
};

enum class SegmentKind : uint8_t { Buf = 0, Borrow = 1, Static = 2 };

struct ReplySegment {
    SegmentKind kind  = SegmentKind::Static;

diff --git a//home/user/Projects/cx-inline-comments/src/core/config.h b//home/user/Projects/cx-inline-comments/src/core/config.h
index 495e6ae42da15ee4bcc76a8a846efeef506903d7..8b5f3e5c786e8eeae0296a5dcf19338056d59007
--- a//home/user/Projects/cx-inline-comments/src/core/config.h
+++ b//home/user/Projects/cx-inline-comments/src/core/config.h
@@ -290,8 +290,8 @@
     uint32_t client_lb = 1;
 
     // ---- automatic role split (boot-latched) -----------------------------------------------
-    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer
-    // (DESIGN-flipfp.md); its measured sampling policy is internal to flipctl.h. Off and 1s
+    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer;
+    // its measured sampling policy is internal to flipctl.h. Off and 1s
     // dispatch pay one predicted branch per op and no fingerprint store. Trigger bands learn
     // from the anchor's own quiet jitter; no separate controller tuning knobs remain.
     uint32_t flip_auto = 0;
diff --git a//home/user/Projects/cx-inline-comments/src/core/flipctl.h b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
index 5185da4085ae5f2e3325702064208810cb9c42f7..1edc3a93889090da659db0c9ee259d04ce5cef57
--- a//home/user/Projects/cx-inline-comments/src/core/flipctl.h
+++ b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
@@ -19,7 +19,9 @@
 
 class Server;
 
-// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
+// Preserve the measured sampler: one whole parse pass in 100 on average. At a matched 512k ops/s,
+// a GET-only -> SET-only swap triggered the fingerprint detector once with both exhaustive and
+// 1-in-100 sampling; the sampled writer still detected the shift with the rate held fixed.
 // Arming derives solely from the controller latch. No runtime sampling knob or adaptive
 // per-operation work is needed; the detector already derives its bands from actual samples.
 inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
@@ -52,7 +54,9 @@
     uint64_t closed_windows = 0;
 };
 
-// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
+// SAMPLED BY PARSE PASS. A per-op countdown would restore the store this sampler removes: no
+// existing io-side command counter covers both the point paths and all io-local commands.
+// The io loop asks pass_sampled() once per dispatched
 // frame -- one load of this writer's own word, on the line the old enabled() test already read,
 // and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
 // for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
diff --git a//home/user/Projects/cx-inline-comments/src/core/server.h b//home/user/Projects/cx-inline-comments/src/core/server.h
index f869dd3d1737afa6b1c401919fb66c96772b1225..c131a43aa82fde16228678b5ad0b0d9a1a54aa18
--- a//home/user/Projects/cx-inline-comments/src/core/server.h
+++ b//home/user/Projects/cx-inline-comments/src/core/server.h
@@ -8,8 +8,10 @@
 //
 // THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
 // dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
-// its derived shard-granularity fast path, published only at a quiesced commit. See
-// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
+// its derived shard-granularity fast path, published only at a quiesced commit. Shards move only
+// after executing, deferred, group and pinned-read work drains; destination routes publish after
+// the ownership edge. An IO may enqueue using an old route after that edge, so tasks and borrow
+// releases recheck ownership and forward before shard access instead of carrying routing epochs.
 #pragma once
 #include <algorithm>
 #include <atomic>
@@ -308,7 +310,7 @@
         for (uint32_t i = 0; i < nthreads; i++) {
             threads_[i] = std::make_unique<ThreadCtx>();
             // The fingerprint writer is armed only when its one reader, the flip controller, is
-            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
+            // enabled: with --flip-auto 0 and in 1s mode it is dark and costs
             // one predicted branch per op. The enabled sampler keeps its measured 1-in-100 policy.
             threads_[i]->init(i, placement_.role_of(i), nthreads,
                               0,
@@ -1969,7 +1971,7 @@
         shard_owner_[shard_id].store(thread_id, std::memory_order_release);
     }
 
-    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
+    // The caller must hold both executor loops at the safe point:
     // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
     // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
     // Vector capacity and membership are settled while PREPARING still names the source.  The
@@ -1988,7 +1990,7 @@
     // lost, a block ends up linked twice, and the damage surfaces later as either
     // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
     // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
-    // Both were observed (see DESIGN-P0REPLY.md).
+    // Both were observed.
     //
     // The rebind used to be deferred to the destination's own next executor pass
     // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
@@ -2827,7 +2829,7 @@
     // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
     // other command. It widens -- deterministically -- the window in which a younger whole-owner
     // walker could overtake an older same-connection group on the destination shard.
-    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
+    // TEST HOOK: effective local-read LANE CAPACITY for admission. 0 means
     // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
     // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
     // its masking, so this can never overrun anything. It exists because lane oversubscription is
@@ -2935,9 +2937,11 @@
     // is already set. Nothing on GET/SET touches it.
     //
     // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
-    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
-    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
-    // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
+    // sequence: a blocking op requires an empty ROB, then sets the barrier before any younger
+    // frame can be parsed; a resumed scatter inherits that same slot and barrier. The state the
+    // owner-scoped release exists to survive -- a release that must NOT drop the barrier -- has
+    // to be injected. It is held past ROB quiescence on purpose; barrier_release_quiesced()
+    // exempts this one bit for that reason.
     //
     // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
     // nothing left to do and parks on submit_and_wait(1) -- unbounded under io_uring. A hold that
@@ -2968,10 +2972,10 @@
         return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
     }
     // An overlap: some owner took the parse barrier while another owner already held it. With
-    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
-    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
-    // live and the owner-scoped release in blocking_retire() became load-bearing rather than
-    // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
+    // DEBUG BARRIER-HOLD off this must read ZERO -- if it ever moves on production traffic the
+    // latent case just went live and the owner-scoped release in blocking_retire() became
+    // load-bearing rather than defensive. With the latch ON it advances once per blocking dispatch,
+    // which is the counter's
     // own positive control: a "must be zero" reading proves nothing until something has been shown
     // able to make it non-zero.
     void note_barrier_overlap() {
diff --git a//home/user/Projects/cx-inline-comments/src/core/shard.h b//home/user/Projects/cx-inline-comments/src/core/shard.h
index 694b9a93c6cd63db49b9dcf061dbb5182c49986a..39a049d2f8619bbf52f706775956cc9198060364
--- a//home/user/Projects/cx-inline-comments/src/core/shard.h
+++ b//home/user/Projects/cx-inline-comments/src/core/shard.h
@@ -431,7 +431,7 @@
         // NOT share this counter: multi.inc's prepare_write_key() re-asks it at install time and
         // reports violations through INFO atomic_exec_order_late (multi_exec_order_late()), which
         // must read zero. Summing the two would hide a violation inside a number that is non-zero
-        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
+        // by design, leaving the park's safety argument with no falsifier.
         uint64_t atomic_exec_order_holds = 0;
         // Times watch_finalize_reservation() answered "not ready" because the reservation's epoch
         // was still 0, i.e. a unit was turned into a Retry by an undecided WATCH reservation. It
diff --git a//home/user/Projects/cx-inline-comments/src/core/signal.h b//home/user/Projects/cx-inline-comments/src/core/signal.h
index 933a5d7ee09538f3923df3c5eb77f9c72cdc0bfa..f69f7d73942ee129f4ad9cd1f23befe69fc4167d
--- a//home/user/Projects/cx-inline-comments/src/core/signal.h
+++ b//home/user/Projects/cx-inline-comments/src/core/signal.h
@@ -47,7 +47,7 @@
 // that will be compared against a deadline must come from here and never from now_ns() above --
 // that one is CLOCK_MONOTONIC, so its "milliseconds" are milliseconds since boot and sit roughly
 // five orders of magnitude below any real deadline. A monotonic value used as an expiry cut does
-// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
+// not skew the answer, it disables expiry outright.
 inline int64_t now_realtime_ms() {
     timespec ts;
     clock_gettime(CLOCK_REALTIME, &ts);
diff --git a//home/user/Projects/cx-inline-comments/src/core/thread.h b//home/user/Projects/cx-inline-comments/src/core/thread.h
index 0fcfa17ba6e331309260b641ec21aa4a8ff9ba36..3cf72c1b96ca700f602e2e7c630a1e6970f9620f
--- a//home/user/Projects/cx-inline-comments/src/core/thread.h
+++ b//home/user/Projects/cx-inline-comments/src/core/thread.h
@@ -118,7 +118,7 @@
     ContextRoute,           // scatter/script/all-shard or conservative broad-owner route
     ContextKeymissNotify,   // MGET miss must retain owner-side notification behavior
     InflightWrite,
-    ArmTransient,           // pre-arming writes still in flight (DESIGN-RINGDIET.md)
+    ArmTransient,           // pre-arming writes still in flight
     AtomicPending,
     Missing,
     Typed,
@@ -151,7 +151,7 @@
     // ARM-ON-DEMAND TRANSIENT. Reads demoted because the connection carried writes published
     // BEFORE a local read armed it -- writes the RYOW ring deliberately never recorded. Bounded
     // per connection by one ROB drain and reported separately from the steady-state key conflict
-    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
+    // above so that the two can never be confused in a bench.
     uint64_t fallback_arm_transient = 0;
     uint64_t fallback_atomic_pending = 0;
     uint64_t fallback_missing = 0;
@@ -160,7 +160,7 @@
     uint64_t fallback_seq_churn = 0;
     uint64_t fallback_generation = 0;
     uint64_t fallback_lane_full = 0;
-    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
+    // Lane ADMISSION deferrals. Frames the armed parser left unconsumed at rpos, to be
     // re-parsed by a later pass of the same thread, because the local-read lane had no room
     // (defer_lane_full) or because the connection already held its fair share of a lane under
     // pressure (defer_quota). Neither is a fallback: the read still completes locally, and no

apply patch
patch: completed
/home/user/Projects/cx-inline-comments/src/core/server.h
diff --git a//home/user/Projects/cx-inline-comments/src/core/config.h b//home/user/Projects/cx-inline-comments/src/core/config.h
index 495e6ae42da15ee4bcc76a8a846efeef506903d7..8b5f3e5c786e8eeae0296a5dcf19338056d59007
--- a//home/user/Projects/cx-inline-comments/src/core/config.h
+++ b//home/user/Projects/cx-inline-comments/src/core/config.h
@@ -290,8 +290,8 @@
     uint32_t client_lb = 1;
 
     // ---- automatic role split (boot-latched) -----------------------------------------------
-    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer
-    // (DESIGN-flipfp.md); its measured sampling policy is internal to flipctl.h. Off and 1s
+    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer;
+    // its measured sampling policy is internal to flipctl.h. Off and 1s
     // dispatch pay one predicted branch per op and no fingerprint store. Trigger bands learn
     // from the anchor's own quiet jitter; no separate controller tuning knobs remain.
     uint32_t flip_auto = 0;
diff --git a//home/user/Projects/cx-inline-comments/src/core/flipctl.h b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
index 5185da4085ae5f2e3325702064208810cb9c42f7..1edc3a93889090da659db0c9ee259d04ce5cef57
--- a//home/user/Projects/cx-inline-comments/src/core/flipctl.h
+++ b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
@@ -19,7 +19,9 @@
 
 class Server;
 
-// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
+// Preserve the measured sampler: one whole parse pass in 100 on average. At a matched 512k ops/s,
+// a GET-only -> SET-only swap triggered the fingerprint detector once with both exhaustive and
+// 1-in-100 sampling; the sampled writer still detected the shift with the rate held fixed.
 // Arming derives solely from the controller latch. No runtime sampling knob or adaptive
 // per-operation work is needed; the detector already derives its bands from actual samples.
 inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
@@ -52,7 +54,9 @@
     uint64_t closed_windows = 0;
 };
 
-// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
+// SAMPLED BY PARSE PASS. A per-op countdown would restore the store this sampler removes: no
+// existing io-side command counter covers both the point paths and all io-local commands.
+// The io loop asks pass_sampled() once per dispatched
 // frame -- one load of this writer's own word, on the line the old enabled() test already read,
 // and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
 // for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
diff --git a//home/user/Projects/cx-inline-comments/src/core/server.h b//home/user/Projects/cx-inline-comments/src/core/server.h
index f869dd3d1737afa6b1c401919fb66c96772b1225..8f279299796e7dabffe155115dea7b3ff379627b
--- a//home/user/Projects/cx-inline-comments/src/core/server.h
+++ b//home/user/Projects/cx-inline-comments/src/core/server.h
@@ -8,8 +8,10 @@
 //
 // THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
 // dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
-// its derived shard-granularity fast path, published only at a quiesced commit. See
-// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
+// its derived shard-granularity fast path, published only at a quiesced commit. Shards move only
+// after executing, deferred, group and pinned-read work drains; destination routes publish after
+// the ownership edge. An IO may enqueue using an old route after that edge, so tasks and borrow
+// releases recheck ownership and forward before shard access instead of carrying routing epochs.
 #pragma once
 #include <algorithm>
 #include <atomic>
@@ -308,7 +310,7 @@
         for (uint32_t i = 0; i < nthreads; i++) {
             threads_[i] = std::make_unique<ThreadCtx>();
             // The fingerprint writer is armed only when its one reader, the flip controller, is
-            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
+            // enabled: with --flip-auto 0 and in 1s mode it is dark and costs
             // one predicted branch per op. The enabled sampler keeps its measured 1-in-100 policy.
             threads_[i]->init(i, placement_.role_of(i), nthreads,
                               0,
@@ -1969,7 +1971,7 @@
         shard_owner_[shard_id].store(thread_id, std::memory_order_release);
     }
 
-    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
+    // The caller must hold both executor loops at the safe point:
     // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
     // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
     // Vector capacity and membership are settled while PREPARING still names the source.  The
@@ -1988,7 +1990,7 @@
     // lost, a block ends up linked twice, and the damage surfaces later as either
     // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
     // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
-    // Both were observed (see DESIGN-P0REPLY.md).
+    // Both were observed.
     //
     // The rebind used to be deferred to the destination's own next executor pass
     // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
@@ -2827,7 +2829,7 @@
     // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
     // other command. It widens -- deterministically -- the window in which a younger whole-owner
     // walker could overtake an older same-connection group on the destination shard.
-    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
+    // TEST HOOK: effective local-read LANE CAPACITY for admission. 0 means
     // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
     // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
     // its masking, so this can never overrun anything. It exists because lane oversubscription is
@@ -2935,9 +2937,11 @@
     // is already set. Nothing on GET/SET touches it.
     //
     // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
-    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
-    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
-    // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
+    // sequence: a blocking op requires an empty ROB, then sets the barrier before any younger
+    // frame can be parsed; a resumed scatter inherits that same slot and barrier. The state the
+    // owner-scoped release exists to survive -- a release that must NOT drop the barrier -- has
+    // to be injected. It is held past ROB quiescence on purpose; barrier_release_quiesced()
+    // exempts this one bit for that reason.
     //
     // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
     // nothing left to do and parks on submit_and_wait(1) -- unbounded under io_uring. A hold that
@@ -2968,11 +2972,11 @@
         return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
     }
     // An overlap: some owner took the parse barrier while another owner already held it. With
-    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
-    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
-    // live and the owner-scoped release in blocking_retire() became load-bearing rather than
-    // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
-    // own positive control: a "must be zero" reading proves nothing until something has been shown
+    // DEBUG BARRIER-HOLD off this must read ZERO -- if it ever moves on production traffic the
+    // latent case just went live and the owner-scoped release in blocking_retire() became
+    // load-bearing rather than defensive. With the latch ON it advances once per blocking dispatch,
+    // which is the counter's own positive control: a "must be zero" reading proves nothing until
+    // something has been shown
     // able to make it non-zero.
     void note_barrier_overlap() {
         barrier_owner_overlaps_.fetch_add(1, std::memory_order_relaxed);
diff --git a//home/user/Projects/cx-inline-comments/src/core/shard.h b//home/user/Projects/cx-inline-comments/src/core/shard.h
index 694b9a93c6cd63db49b9dcf061dbb5182c49986a..39a049d2f8619bbf52f706775956cc9198060364
--- a//home/user/Projects/cx-inline-comments/src/core/shard.h
+++ b//home/user/Projects/cx-inline-comments/src/core/shard.h
@@ -431,7 +431,7 @@
         // NOT share this counter: multi.inc's prepare_write_key() re-asks it at install time and
         // reports violations through INFO atomic_exec_order_late (multi_exec_order_late()), which
         // must read zero. Summing the two would hide a violation inside a number that is non-zero
-        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
+        // by design, leaving the park's safety argument with no falsifier.
         uint64_t atomic_exec_order_holds = 0;
         // Times watch_finalize_reservation() answered "not ready" because the reservation's epoch
         // was still 0, i.e. a unit was turned into a Retry by an undecided WATCH reservation. It
diff --git a//home/user/Projects/cx-inline-comments/src/core/signal.h b//home/user/Projects/cx-inline-comments/src/core/signal.h
index 933a5d7ee09538f3923df3c5eb77f9c72cdc0bfa..f69f7d73942ee129f4ad9cd1f23befe69fc4167d
--- a//home/user/Projects/cx-inline-comments/src/core/signal.h
+++ b//home/user/Projects/cx-inline-comments/src/core/signal.h
@@ -47,7 +47,7 @@
 // that will be compared against a deadline must come from here and never from now_ns() above --
 // that one is CLOCK_MONOTONIC, so its "milliseconds" are milliseconds since boot and sit roughly
 // five orders of magnitude below any real deadline. A monotonic value used as an expiry cut does
-// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
+// not skew the answer, it disables expiry outright.
 inline int64_t now_realtime_ms() {
     timespec ts;
     clock_gettime(CLOCK_REALTIME, &ts);
diff --git a//home/user/Projects/cx-inline-comments/src/core/thread.h b//home/user/Projects/cx-inline-comments/src/core/thread.h
index 0fcfa17ba6e331309260b641ec21aa4a8ff9ba36..3cf72c1b96ca700f602e2e7c630a1e6970f9620f
--- a//home/user/Projects/cx-inline-comments/src/core/thread.h
+++ b//home/user/Projects/cx-inline-comments/src/core/thread.h
@@ -118,7 +118,7 @@
     ContextRoute,           // scatter/script/all-shard or conservative broad-owner route
     ContextKeymissNotify,   // MGET miss must retain owner-side notification behavior
     InflightWrite,
-    ArmTransient,           // pre-arming writes still in flight (DESIGN-RINGDIET.md)
+    ArmTransient,           // pre-arming writes still in flight
     AtomicPending,
     Missing,
     Typed,
@@ -151,7 +151,7 @@
     // ARM-ON-DEMAND TRANSIENT. Reads demoted because the connection carried writes published
     // BEFORE a local read armed it -- writes the RYOW ring deliberately never recorded. Bounded
     // per connection by one ROB drain and reported separately from the steady-state key conflict
-    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
+    // above so that the two can never be confused in a bench.
     uint64_t fallback_arm_transient = 0;
     uint64_t fallback_atomic_pending = 0;
     uint64_t fallback_missing = 0;
@@ -160,7 +160,7 @@
     uint64_t fallback_seq_churn = 0;
     uint64_t fallback_generation = 0;
     uint64_t fallback_lane_full = 0;
-    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
+    // Lane ADMISSION deferrals. Frames the armed parser left unconsumed at rpos, to be
     // re-parsed by a later pass of the same thread, because the local-read lane had no room
     // (defer_lane_full) or because the connection already held its fair share of a lane under
     // pressure (defer_quota). Neither is a fallback: the read still completes locally, and no

exec
/bin/bash -lc "python3 - <<'PY'
import json
from pathlib import Path
rows=[]
def row(file,line,note,action,claim,evidence):
    rows.append(dict(file='src/core/'+file,original_line=line,note=note,action=action,claim=claim,archive_evidence=evidence))
row('thread.h',121,'DESIGN-RINGDIET.md','DROP','The enum already names pre-arming writes; fallback_arm_transient below explains their fixed, one-drain fence.','Design (1), arming transient, lines 52-64')
row('thread.h',154,'DESIGN-RINGDIET.md','DROP','The surrounding comment already distinguishes the one-drain arming transient from steady-state key conflicts.','Counters, lines 99-111')
row('thread.h',163,'P128.md','DROP','The comment already explains leaving rpos unconsumed, local retry, both deferral counters and the zero legacy fallback counter.','§4, lines 154-162 and final-form corrections at 181-190')
row('shard.h',434,'NOTES-MULTIRACE.md','DROP','The comment already states holds > 0 and late == 0 separately; combining them hides late-install violations.','§5, One counter, one meaning, lines 210-230')
row('signal.h',50,'NOTES-EXPWIDE.md','DROP','The CLOCK_REALTIME/CLOCK_MONOTONIC distinction and its total suppression of expiry are already explicit.','W3, lines 156-172')
row('flipctl.h',22,'DESIGN-flipfp.md','INLINE','At a matched 512k ops/s, GET-only to SET-only caused exactly one fingerprint trigger with both exhaustive and one-in-100 pass sampling; detection survived without a rate change.','§5.2, lines 214-232; measurement is on the historical detector')
row('flipctl.h',55,'DESIGN-flipfp.md','INLINE','A per-operation countdown would restore the removed store; no existing io-side command counter covers point dispatch and every io-local command.','§2(b), lines 47-60')
row('server.h',12,'NOTES-MIGRATE.md','INLINE','Drain executing, deferred, group and pinned-read work before handoff; publish destination routes after the ownership edge; stale tasks and borrow releases recheck and forward before shard access instead of carrying routing epochs.','Shared rules and §1, lines 53-114')
row('server.h',311,'DESIGN-flipfp.md','DROP','The comment already specifies the sole reader, dark off/1s behavior, predicted branch and enabled 1-in-100 sampling.','§1 reader census and §2(a-b), lines 24-60')
row('server.h',1972,'NOTES-MIGRATE.md','DROP','The following lines already specify the drained-work safe point, forwarding before access, PREPARING bookkeeping and sole commit edge.','§1, steps 3-7 and forwarding rule, lines 82-114')
row('server.h',1991,'DESIGN-P0REPLY.md','DROP','The existing block retains both reproduced free-list failures, the exact stale-cache transfer trace and the same-critical-section sink fix.','§§7-9, lines 148-218')
row('server.h',2830,'P128.md','DROP','The complete comment already explains why traffic cannot reliably fill the lane and how a lowered admission cap opens the window within one parse pass without changing physical capacity.','§8.1, lines 423-451')
row('server.h',2938,'NOTES-BARRIER.md','INLINE','A blocking op requires an empty ROB, bars parsing before a younger frame can enter, and a resumed scatter inherits its slot/barrier; a second owner must therefore be injected for the release test.','§2, lines 68-94')
row('server.h',2972,'NOTES-BARRIER.md','DROP','The counter already states the zero production invariant, the consequence of an overlap, and the debug latch as positive control; the hook above now supplies the guard proof.','§2, lines 68-106')
row('config.h',294,'DESIGN-flipfp.md','DROP','The existing block already explains controller-only arming, the internal sampling policy and the dark no-store path.','§1 reader census and §2(a), lines 24-46')
data=dict(entries=rows,findings=[dict(file='src/core/server.h',original_line=2937,detail='The hook comment still says six owners, but BarrierOwner now has seven production bits including Sleep (src/net/conn.h:94-105). The guard argument remains applicable; the stale count was left as requested.')],preserve_notes=[
 dict(note='NOTES-MIGRATE.md',detail='The bucket/client/role-transfer state machines and in-flight case tables (§§1-3) cover ownership edges across many files; keep the full contract outside a single comment.'),
 dict(note='DESIGN-P0REPLY.md',detail='The attribution, transfer/rebind event trace, negative controls and audit of every ownership edge (§§7-10) are stronger evidence than the short invariant alone. Earlier §3 exclusions are superseded by the later root cause.'),
 dict(note='DESIGN-flipfp.md',detail='Matched-rate PRE/POST/null measurements, isolated detector-liveness experiments and sample-count/deafness analysis (§§5-6) need their geometry and historical-detector context; the inline detection example is only one cell.'),
 dict(note='DESIGN-RINGDIET.md',detail='The PRE/POST/null tables and saturation/interference caveats explain the allocation/bookkeeping decision and should remain available as research evidence.'),
 dict(note='NOTES-BARRIER.md',detail='The setter/releaser census and rejected interleavings (§§1-2) are the audit trail for latent reachability; the six-owner inventory is historical.'),
 dict(note='P128.md',detail='The superseded variants, starvation analysis, final PRE/POST controls and cold-field load regression (§§7-10) require more space than a local admission comment; distinguish corrected final-form claims from prototype text.')])
Path('.inline-root.json').write_text(json.dumps(data,indent=2)+'\\n')
print(f'Recorded {len(rows)} root references')
PY" in /home/user/Projects/cx-inline-comments
 succeeded in 0ms:
Recorded 15 root references

diff --git a//home/user/Projects/cx-inline-comments/src/core/config.h b//home/user/Projects/cx-inline-comments/src/core/config.h
index 495e6ae42da15ee4bcc76a8a846efeef506903d7..8b5f3e5c786e8eeae0296a5dcf19338056d59007
--- a//home/user/Projects/cx-inline-comments/src/core/config.h
+++ b//home/user/Projects/cx-inline-comments/src/core/config.h
@@ -290,8 +290,8 @@
     uint32_t client_lb = 1;
 
     // ---- automatic role split (boot-latched) -----------------------------------------------
-    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer
-    // (DESIGN-flipfp.md); its measured sampling policy is internal to flipctl.h. Off and 1s
+    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer;
+    // its measured sampling policy is internal to flipctl.h. Off and 1s
     // dispatch pay one predicted branch per op and no fingerprint store. Trigger bands learn
     // from the anchor's own quiet jitter; no separate controller tuning knobs remain.
     uint32_t flip_auto = 0;
diff --git a//home/user/Projects/cx-inline-comments/src/core/flipctl.h b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
index 5185da4085ae5f2e3325702064208810cb9c42f7..1edc3a93889090da659db0c9ee259d04ce5cef57
--- a//home/user/Projects/cx-inline-comments/src/core/flipctl.h
+++ b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
@@ -19,7 +19,9 @@
 
 class Server;
 
-// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
+// Preserve the measured sampler: one whole parse pass in 100 on average. At a matched 512k ops/s,
+// a GET-only -> SET-only swap triggered the fingerprint detector once with both exhaustive and
+// 1-in-100 sampling; the sampled writer still detected the shift with the rate held fixed.
 // Arming derives solely from the controller latch. No runtime sampling knob or adaptive
 // per-operation work is needed; the detector already derives its bands from actual samples.
 inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
@@ -52,7 +54,9 @@
     uint64_t closed_windows = 0;
 };
 
-// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
+// SAMPLED BY PARSE PASS. A per-op countdown would restore the store this sampler removes: no
+// existing io-side command counter covers both the point paths and all io-local commands.
+// The io loop asks pass_sampled() once per dispatched
 // frame -- one load of this writer's own word, on the line the old enabled() test already read,
 // and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
 // for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
diff --git a//home/user/Projects/cx-inline-comments/src/core/server.h b//home/user/Projects/cx-inline-comments/src/core/server.h
index f869dd3d1737afa6b1c401919fb66c96772b1225..8f279299796e7dabffe155115dea7b3ff379627b
--- a//home/user/Projects/cx-inline-comments/src/core/server.h
+++ b//home/user/Projects/cx-inline-comments/src/core/server.h
@@ -8,8 +8,10 @@
 //
 // THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
 // dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
-// its derived shard-granularity fast path, published only at a quiesced commit. See
-// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
+// its derived shard-granularity fast path, published only at a quiesced commit. Shards move only
+// after executing, deferred, group and pinned-read work drains; destination routes publish after
+// the ownership edge. An IO may enqueue using an old route after that edge, so tasks and borrow
+// releases recheck ownership and forward before shard access instead of carrying routing epochs.
 #pragma once
 #include <algorithm>
 #include <atomic>
@@ -308,7 +310,7 @@
         for (uint32_t i = 0; i < nthreads; i++) {
             threads_[i] = std::make_unique<ThreadCtx>();
             // The fingerprint writer is armed only when its one reader, the flip controller, is
-            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
+            // enabled: with --flip-auto 0 and in 1s mode it is dark and costs
             // one predicted branch per op. The enabled sampler keeps its measured 1-in-100 policy.
             threads_[i]->init(i, placement_.role_of(i), nthreads,
                               0,
@@ -1969,7 +1971,7 @@
         shard_owner_[shard_id].store(thread_id, std::memory_order_release);
     }
 
-    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
+    // The caller must hold both executor loops at the safe point:
     // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
     // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
     // Vector capacity and membership are settled while PREPARING still names the source.  The
@@ -1988,7 +1990,7 @@
     // lost, a block ends up linked twice, and the damage surfaces later as either
     // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
     // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
-    // Both were observed (see DESIGN-P0REPLY.md).
+    // Both were observed.
     //
     // The rebind used to be deferred to the destination's own next executor pass
     // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
@@ -2827,7 +2829,7 @@
     // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
     // other command. It widens -- deterministically -- the window in which a younger whole-owner
     // walker could overtake an older same-connection group on the destination shard.
-    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
+    // TEST HOOK: effective local-read LANE CAPACITY for admission. 0 means
     // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
     // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
     // its masking, so this can never overrun anything. It exists because lane oversubscription is
@@ -2935,9 +2937,11 @@
     // is already set. Nothing on GET/SET touches it.
     //
     // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
-    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
-    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
-    // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
+    // sequence: a blocking op requires an empty ROB, then sets the barrier before any younger
+    // frame can be parsed; a resumed scatter inherits that same slot and barrier. The state the
+    // owner-scoped release exists to survive -- a release that must NOT drop the barrier -- has
+    // to be injected. It is held past ROB quiescence on purpose; barrier_release_quiesced()
+    // exempts this one bit for that reason.
     //
     // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
     // nothing left to do and parks on submit_and_wait(1) -- unbounded under io_uring. A hold that
@@ -2968,11 +2972,11 @@
         return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
     }
     // An overlap: some owner took the parse barrier while another owner already held it. With
-    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
-    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
-    // live and the owner-scoped release in blocking_retire() became load-bearing rather than
-    // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
-    // own positive control: a "must be zero" reading proves nothing until something has been shown
+    // DEBUG BARRIER-HOLD off this must read ZERO -- if it ever moves on production traffic the
+    // latent case just went live and the owner-scoped release in blocking_retire() became
+    // load-bearing rather than defensive. With the latch ON it advances once per blocking dispatch,
+    // which is the counter's own positive control: a "must be zero" reading proves nothing until
+    // something has been shown
     // able to make it non-zero.
     void note_barrier_overlap() {
         barrier_owner_overlaps_.fetch_add(1, std::memory_order_relaxed);
diff --git a//home/user/Projects/cx-inline-comments/src/core/shard.h b//home/user/Projects/cx-inline-comments/src/core/shard.h
index 694b9a93c6cd63db49b9dcf061dbb5182c49986a..39a049d2f8619bbf52f706775956cc9198060364
--- a//home/user/Projects/cx-inline-comments/src/core/shard.h
+++ b//home/user/Projects/cx-inline-comments/src/core/shard.h
@@ -431,7 +431,7 @@
         // NOT share this counter: multi.inc's prepare_write_key() re-asks it at install time and
         // reports violations through INFO atomic_exec_order_late (multi_exec_order_late()), which
         // must read zero. Summing the two would hide a violation inside a number that is non-zero
-        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
+        // by design, leaving the park's safety argument with no falsifier.
         uint64_t atomic_exec_order_holds = 0;
         // Times watch_finalize_reservation() answered "not ready" because the reservation's epoch
         // was still 0, i.e. a unit was turned into a Retry by an undecided WATCH reservation. It
diff --git a//home/user/Projects/cx-inline-comments/src/core/signal.h b//home/user/Projects/cx-inline-comments/src/core/signal.h
index 933a5d7ee09538f3923df3c5eb77f9c72cdc0bfa..f69f7d73942ee129f4ad9cd1f23befe69fc4167d
--- a//home/user/Projects/cx-inline-comments/src/core/signal.h
+++ b//home/user/Projects/cx-inline-comments/src/core/signal.h
@@ -47,7 +47,7 @@
 // that will be compared against a deadline must come from here and never from now_ns() above --
 // that one is CLOCK_MONOTONIC, so its "milliseconds" are milliseconds since boot and sit roughly
 // five orders of magnitude below any real deadline. A monotonic value used as an expiry cut does
-// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
+// not skew the answer, it disables expiry outright.
 inline int64_t now_realtime_ms() {
     timespec ts;
     clock_gettime(CLOCK_REALTIME, &ts);
diff --git a//home/user/Projects/cx-inline-comments/src/core/thread.h b//home/user/Projects/cx-inline-comments/src/core/thread.h
index 0fcfa17ba6e331309260b641ec21aa4a8ff9ba36..3cf72c1b96ca700f602e2e7c630a1e6970f9620f
--- a//home/user/Projects/cx-inline-comments/src/core/thread.h
+++ b//home/user/Projects/cx-inline-comments/src/core/thread.h
@@ -118,7 +118,7 @@
     ContextRoute,           // scatter/script/all-shard or conservative broad-owner route
     ContextKeymissNotify,   // MGET miss must retain owner-side notification behavior
     InflightWrite,
-    ArmTransient,           // pre-arming writes still in flight (DESIGN-RINGDIET.md)
+    ArmTransient,           // pre-arming writes still in flight
     AtomicPending,
     Missing,
     Typed,
@@ -151,7 +151,7 @@
     // ARM-ON-DEMAND TRANSIENT. Reads demoted because the connection carried writes published
     // BEFORE a local read armed it -- writes the RYOW ring deliberately never recorded. Bounded
     // per connection by one ROB drain and reported separately from the steady-state key conflict
-    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
+    // above so that the two can never be confused in a bench.
     uint64_t fallback_arm_transient = 0;
     uint64_t fallback_atomic_pending = 0;
     uint64_t fallback_missing = 0;
@@ -160,7 +160,7 @@
     uint64_t fallback_seq_churn = 0;
     uint64_t fallback_generation = 0;
     uint64_t fallback_lane_full = 0;
-    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
+    // Lane ADMISSION deferrals. Frames the armed parser left unconsumed at rpos, to be
     // re-parsed by a later pass of the same thread, because the local-read lane had no room
     // (defer_lane_full) or because the connection already held its fair share of a lane under
     // pressure (defer_quota). Neither is a fallback: the read still completes locally, and no

exec
/bin/bash -lc "git status --short && git diff --stat && rg -n '\\.md' src" in /home/user/Projects/cx-inline-comments
 succeeded in 0ms:
 M src/cmd/blocking.inc
 M src/cmd/climon.cc
 M src/cmd/multi.h
 M src/cmd/multi.inc
 M src/cmd/scripting.cc
 M src/cmd/server_tail.cc
 M src/cmd/slowlog.cc
 M src/cmd/slowlog.h
 M src/cmd/t_server.cc
 M src/core/config.h
 M src/core/ex_loop.h
 M src/core/flipctl.h
 M src/core/io_loop.h
 M src/core/server.h
 M src/core/shard.h
 M src/core/signal.h
 M src/core/thread.h
 M src/net/conn.h
 M src/net/rob.h
 M src/store/flatstore.h
 M src/store/flatstore_atomic.inc
 M src/store/kv_block_cache.h
 M src/store/read_local_settax.h
?? .inline-root.json
 src/cmd/blocking.inc           |  2 +-
 src/cmd/climon.cc              |  2 +-
 src/cmd/multi.h                |  4 ++--
 src/cmd/multi.inc              |  9 +++++----
 src/cmd/scripting.cc           |  2 +-
 src/cmd/server_tail.cc         |  4 ++--
 src/cmd/slowlog.cc             |  2 +-
 src/cmd/slowlog.h              |  5 +++--
 src/cmd/t_server.cc            | 22 ++++++++++++----------
 src/core/config.h              |  4 ++--
 src/core/ex_loop.h             | 32 +++++++++++++++++---------------
 src/core/flipctl.h             |  8 ++++++--
 src/core/io_loop.h             | 18 ++++++++++--------
 src/core/server.h              | 32 ++++++++++++++++++--------------
 src/core/shard.h               |  2 +-
 src/core/signal.h              |  2 +-
 src/core/thread.h              |  6 +++---
 src/net/conn.h                 |  7 ++++---
 src/net/rob.h                  | 20 ++++++++++----------
 src/store/flatstore.h          |  4 +++-
 src/store/flatstore_atomic.inc | 11 ++++++-----
 src/store/kv_block_cache.h     |  5 +++--
 src/store/read_local_settax.h  |  2 +-
 23 files changed, 113 insertions(+), 92 deletions(-)
src/cmd/server_tail.cc:212:                  "ERR WAITAOF with numlocal > 0 is not implemented by tomokv: the local fsync wait needs the deferred-reply path, see NOTES-SERVERTAIL.md");

diff --git a//home/user/Projects/cx-inline-comments/src/core/config.h b//home/user/Projects/cx-inline-comments/src/core/config.h
index 495e6ae42da15ee4bcc76a8a846efeef506903d7..8b5f3e5c786e8eeae0296a5dcf19338056d59007
--- a//home/user/Projects/cx-inline-comments/src/core/config.h
+++ b//home/user/Projects/cx-inline-comments/src/core/config.h
@@ -290,8 +290,8 @@
     uint32_t client_lb = 1;
 
     // ---- automatic role split (boot-latched) -----------------------------------------------
-    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer
-    // (DESIGN-flipfp.md); its measured sampling policy is internal to flipctl.h. Off and 1s
+    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer;
+    // its measured sampling policy is internal to flipctl.h. Off and 1s
     // dispatch pay one predicted branch per op and no fingerprint store. Trigger bands learn
     // from the anchor's own quiet jitter; no separate controller tuning knobs remain.
     uint32_t flip_auto = 0;
diff --git a//home/user/Projects/cx-inline-comments/src/core/flipctl.h b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
index 5185da4085ae5f2e3325702064208810cb9c42f7..1edc3a93889090da659db0c9ee259d04ce5cef57
--- a//home/user/Projects/cx-inline-comments/src/core/flipctl.h
+++ b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
@@ -19,7 +19,9 @@
 
 class Server;
 
-// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
+// Preserve the measured sampler: one whole parse pass in 100 on average. At a matched 512k ops/s,
+// a GET-only -> SET-only swap triggered the fingerprint detector once with both exhaustive and
+// 1-in-100 sampling; the sampled writer still detected the shift with the rate held fixed.
 // Arming derives solely from the controller latch. No runtime sampling knob or adaptive
 // per-operation work is needed; the detector already derives its bands from actual samples.
 inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
@@ -52,7 +54,9 @@
     uint64_t closed_windows = 0;
 };
 
-// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
+// SAMPLED BY PARSE PASS. A per-op countdown would restore the store this sampler removes: no
+// existing io-side command counter covers both the point paths and all io-local commands.
+// The io loop asks pass_sampled() once per dispatched
 // frame -- one load of this writer's own word, on the line the old enabled() test already read,
 // and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
 // for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
diff --git a//home/user/Projects/cx-inline-comments/src/core/server.h b//home/user/Projects/cx-inline-comments/src/core/server.h
index f869dd3d1737afa6b1c401919fb66c96772b1225..8f279299796e7dabffe155115dea7b3ff379627b
--- a//home/user/Projects/cx-inline-comments/src/core/server.h
+++ b//home/user/Projects/cx-inline-comments/src/core/server.h
@@ -8,8 +8,10 @@
 //
 // THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
 // dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
-// its derived shard-granularity fast path, published only at a quiesced commit. See
-// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
+// its derived shard-granularity fast path, published only at a quiesced commit. Shards move only
+// after executing, deferred, group and pinned-read work drains; destination routes publish after
+// the ownership edge. An IO may enqueue using an old route after that edge, so tasks and borrow
+// releases recheck ownership and forward before shard access instead of carrying routing epochs.
 #pragma once
 #include <algorithm>
 #include <atomic>
@@ -308,7 +310,7 @@
         for (uint32_t i = 0; i < nthreads; i++) {
             threads_[i] = std::make_unique<ThreadCtx>();
             // The fingerprint writer is armed only when its one reader, the flip controller, is
-            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
+            // enabled: with --flip-auto 0 and in 1s mode it is dark and costs
             // one predicted branch per op. The enabled sampler keeps its measured 1-in-100 policy.
             threads_[i]->init(i, placement_.role_of(i), nthreads,
                               0,
@@ -1969,7 +1971,7 @@
         shard_owner_[shard_id].store(thread_id, std::memory_order_release);
     }
 
-    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
+    // The caller must hold both executor loops at the safe point:
     // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
     // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
     // Vector capacity and membership are settled while PREPARING still names the source.  The
@@ -1988,7 +1990,7 @@
     // lost, a block ends up linked twice, and the damage surfaces later as either
     // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
     // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
-    // Both were observed (see DESIGN-P0REPLY.md).
+    // Both were observed.
     //
     // The rebind used to be deferred to the destination's own next executor pass
     // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
@@ -2827,7 +2829,7 @@
     // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
     // other command. It widens -- deterministically -- the window in which a younger whole-owner
     // walker could overtake an older same-connection group on the destination shard.
-    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
+    // TEST HOOK: effective local-read LANE CAPACITY for admission. 0 means
     // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
     // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
     // its masking, so this can never overrun anything. It exists because lane oversubscription is
@@ -2935,9 +2937,11 @@
     // is already set. Nothing on GET/SET touches it.
     //
     // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
-    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
-    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
-    // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
+    // sequence: a blocking op requires an empty ROB, then sets the barrier before any younger
+    // frame can be parsed; a resumed scatter inherits that same slot and barrier. The state the
+    // owner-scoped release exists to survive -- a release that must NOT drop the barrier -- has
+    // to be injected. It is held past ROB quiescence on purpose; barrier_release_quiesced()
+    // exempts this one bit for that reason.
     //
     // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
     // nothing left to do and parks on submit_and_wait(1) -- unbounded under io_uring. A hold that
@@ -2968,11 +2972,11 @@
         return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
     }
     // An overlap: some owner took the parse barrier while another owner already held it. With
-    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
-    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
-    // live and the owner-scoped release in blocking_retire() became load-bearing rather than
-    // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
-    // own positive control: a "must be zero" reading proves nothing until something has been shown
+    // DEBUG BARRIER-HOLD off this must read ZERO -- if it ever moves on production traffic the
+    // latent case just went live and the owner-scoped release in blocking_retire() became
+    // load-bearing rather than defensive. With the latch ON it advances once per blocking dispatch,
+    // which is the counter's own positive control: a "must be zero" reading proves nothing until
+    // something has been shown
     // able to make it non-zero.
     void note_barrier_overlap() {
         barrier_owner_overlaps_.fetch_add(1, std::memory_order_relaxed);
diff --git a//home/user/Projects/cx-inline-comments/src/core/shard.h b//home/user/Projects/cx-inline-comments/src/core/shard.h
index 694b9a93c6cd63db49b9dcf061dbb5182c49986a..39a049d2f8619bbf52f706775956cc9198060364
--- a//home/user/Projects/cx-inline-comments/src/core/shard.h
+++ b//home/user/Projects/cx-inline-comments/src/core/shard.h
@@ -431,7 +431,7 @@
         // NOT share this counter: multi.inc's prepare_write_key() re-asks it at install time and
         // reports violations through INFO atomic_exec_order_late (multi_exec_order_late()), which
         // must read zero. Summing the two would hide a violation inside a number that is non-zero
-        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
+        // by design, leaving the park's safety argument with no falsifier.
         uint64_t atomic_exec_order_holds = 0;
         // Times watch_finalize_reservation() answered "not ready" because the reservation's epoch
         // was still 0, i.e. a unit was turned into a Retry by an undecided WATCH reservation. It
diff --git a//home/user/Projects/cx-inline-comments/src/core/signal.h b//home/user/Projects/cx-inline-comments/src/core/signal.h
index 933a5d7ee09538f3923df3c5eb77f9c72cdc0bfa..f69f7d73942ee129f4ad9cd1f23befe69fc4167d
--- a//home/user/Projects/cx-inline-comments/src/core/signal.h
+++ b//home/user/Projects/cx-inline-comments/src/core/signal.h
@@ -47,7 +47,7 @@
 // that will be compared against a deadline must come from here and never from now_ns() above --
 // that one is CLOCK_MONOTONIC, so its "milliseconds" are milliseconds since boot and sit roughly
 // five orders of magnitude below any real deadline. A monotonic value used as an expiry cut does
-// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
+// not skew the answer, it disables expiry outright.
 inline int64_t now_realtime_ms() {
     timespec ts;
     clock_gettime(CLOCK_REALTIME, &ts);
diff --git a//home/user/Projects/cx-inline-comments/src/core/thread.h b//home/user/Projects/cx-inline-comments/src/core/thread.h
index 0fcfa17ba6e331309260b641ec21aa4a8ff9ba36..3cf72c1b96ca700f602e2e7c630a1e6970f9620f
--- a//home/user/Projects/cx-inline-comments/src/core/thread.h
+++ b//home/user/Projects/cx-inline-comments/src/core/thread.h
@@ -118,7 +118,7 @@
     ContextRoute,           // scatter/script/all-shard or conservative broad-owner route
     ContextKeymissNotify,   // MGET miss must retain owner-side notification behavior
     InflightWrite,
-    ArmTransient,           // pre-arming writes still in flight (DESIGN-RINGDIET.md)
+    ArmTransient,           // pre-arming writes still in flight
     AtomicPending,
     Missing,
     Typed,
@@ -151,7 +151,7 @@
     // ARM-ON-DEMAND TRANSIENT. Reads demoted because the connection carried writes published
     // BEFORE a local read armed it -- writes the RYOW ring deliberately never recorded. Bounded
     // per connection by one ROB drain and reported separately from the steady-state key conflict
-    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
+    // above so that the two can never be confused in a bench.
     uint64_t fallback_arm_transient = 0;
     uint64_t fallback_atomic_pending = 0;
     uint64_t fallback_missing = 0;
@@ -160,7 +160,7 @@
     uint64_t fallback_seq_churn = 0;
     uint64_t fallback_generation = 0;
     uint64_t fallback_lane_full = 0;
-    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
+    // Lane ADMISSION deferrals. Frames the armed parser left unconsumed at rpos, to be
     // re-parsed by a later pass of the same thread, because the local-read lane had no room
     // (defer_lane_full) or because the connection already held its fair share of a lane under
     // pressure (defer_quota). Neither is a fallback: the read still completes locally, and no

exec
/bin/bash -lc 'git diff -- src/core src/net src/store' in /home/user/Projects/cx-inline-comments
 succeeded in 0ms:
diff --git a/src/core/config.h b/src/core/config.h
index 495e6ae42..8b5f3e5c7 100644
--- a/src/core/config.h
+++ b/src/core/config.h
@@ -290,8 +290,8 @@ struct Config {
     uint32_t client_lb = 1;
 
     // ---- automatic role split (boot-latched) -----------------------------------------------
-    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer
-    // (DESIGN-flipfp.md); its measured sampling policy is internal to flipctl.h. Off and 1s
+    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer;
+    // its measured sampling policy is internal to flipctl.h. Off and 1s
     // dispatch pay one predicted branch per op and no fingerprint store. Trigger bands learn
     // from the anchor's own quiet jitter; no separate controller tuning knobs remain.
     uint32_t flip_auto = 0;
diff --git a/src/core/ex_loop.h b/src/core/ex_loop.h
index 0884bd913..c9cc50408 100644
--- a/src/core/ex_loop.h
+++ b/src/core/ex_loop.h
@@ -56,7 +56,7 @@ inline constexpr uint32_t kReadLocalMaxChunksBetweenOwnerBatches = 1;
 // Leave that credit outside the pending-read fanout budget so the combined reservation can always
 // fit an empty producer lane and therefore cannot retry forever.
 inline constexpr uint32_t kReadLocalDemotionBudget = kInboxSlots - 1;
-// Lane ADMISSION pressure window (P128.md). A LANE-FULL deferral arms this many fused rotations
+// Lane ADMISSION pressure window. A LANE-FULL deferral arms this many fused rotations
 // during which the parser bounds every connection to kInboxSlots / active connections in flight.
 // It decays by one per rotation and is re-armed only by the next lane-full event -- never by the
 // quota's own deferrals (the actuator must not police itself) -- so under sustained oversubscription
@@ -138,7 +138,8 @@ struct ReadLocalExState<true> {
         // already sat at offset 38 between reserved_schedule and demote_context, on the same first
         // cache line as lane_head/lane_tail/lane_count that every admission test already reads, so
         // the struct does not grow, nothing moves, and reading it costs no line the caller did not
-        // already own. See P128.md section 8.
+        // already own. Lowering admission lets one pipelined write fill the lane inside one parse
+        // pass, before a drain can race the test; traffic volume alone cannot force that window.
         uint16_t lane_admit_cap = static_cast<uint16_t>(kInboxSlots);
         void* demote_context = nullptr;
         DemoteFn demote = nullptr;
@@ -275,7 +276,7 @@ public:
                    kReadLocalDemotionBudget - state.lane_demotion_demand;
     }
 
-    // LANE ADMISSION (P128.md). The parser asks once per parse pass how many lane slots (pending
+    // LANE ADMISSION. The parser asks once per parse pass how many lane slots (pending
     // local reads) one connection may hold. Unbounded while the lane is not under pressure -- below
     // kInboxSlots of aggregate demand per rotation. Under pressure it is the lane divided among the
     // connections competing for it, which by construction keeps the sum of admitted local reads
@@ -309,8 +310,8 @@ public:
     // IoLoop fields it does read live on two other lines entirely (notify_armed_ 7864,
     // proto_max_bulk_len_ 7872). That made always-on machinery cost one cold-line demand fill per
     // parse pass while every counter it owns still read zero -- measured on the owner's 32-core box
-    // at 512 connections as +11% DRAM and +3.4% same-CCX fills per op with instructions flat
-    // (P128.md 9.5). A hint must never be load-bearing, and machinery that has not fired must be
+    // at 512 connections as +11% DRAM and +3.4% same-CCX fills per op with instructions flat.
+    // A hint must never be load-bearing, and machinery that has not fired must be
     // free: gate the argument, not just the answer.
     bool read_local_lane_under_pressure() const {
         static_assert(Fused);
@@ -562,7 +563,7 @@ public:
         }
         if (read_local_enabled()) {
             did += read_local_impl().deferred.drain_ready();
-            // One rotation of the lane-admission pressure window has elapsed (P128.md).
+            // One rotation of the lane-admission pressure window has elapsed.
             if (__builtin_expect(read_local_impl().lane_pressure != 0, false))
                 read_local_impl().lane_pressure--;
             // Adopt the test lane cap, if one is set. ONE relaxed load per ROTATION of a
@@ -2339,7 +2340,7 @@ private:
                 // ONE ENTRY PER COMMAND, not per participating shard. A cross-shard op is handed
                 // to every owner it touches; all but the last return with the op still Issued.
                 // Recording only the owner that published Done means a scatter is attributed to
-                // the slice that actually computed the answer -- documented in NOTES-SERVERTAIL.md.
+                // the slice that actually computed the answer.
                 if (client &&
                     client->rob().at(batch[i].op_id).state.load(std::memory_order_relaxed) ==
                         OpState::Done)
@@ -2522,9 +2523,10 @@ private:
                 // fragment has installed nothing yet on this owner, so parking it cannot hold a
                 // half-applied command; and the predecessor can never wait on it, because on
                 // every shard the older unit's fragment is drained first and, if it is itself
-                // parked, parked_predecessor_in() holds this younger fragment behind it. That is
-                // the acyclicity NOTES-MULTIRES.md §5(a) found missing when the same hold was
-                // attempted mid-command against an order-blind key probe.
+                // parked, parked_predecessor_in() holds this younger fragment behind it. Putting
+                // the hold in prepare_write_key() deadlocked: the transaction held partial
+                // installs while waiting on an older group whose order-blind key probe then
+                // waited on those installs.
                 if (__builtin_expect(shard.store().atomic_has_records(), false) &&
                     shard.store().atomic_has_foreign_unit_undecided(
                         t.client->id(),
@@ -2948,8 +2950,8 @@ private:
         const uint32_t slot = c->wb_slot();
         if (slot != Client::kNoWbSlot) {
             // INSIDE AN EXECUTOR BATCH: record and return. The fence, the read-first set and the
-            // wake decision are paid once per batch in flush_notify_batch() (DESIGN-NOTIFY.md
-            // §2, §4). A batch of one op degrades to exactly the sequence below it.
+            // wake decision are paid once per batch in flush_notify_batch(). A batch of one op
+            // degrades to exactly the sequence below it.
             if (__builtin_expect(notify_batch_open_, true)) {
                 if (__builtin_expect(notify_batch_n_ == kNotifyBatchMax, false))
                     flush_notify_batch();
@@ -2992,7 +2994,7 @@ private:
     // decision per io that saw an empty->flagged edge. Deciding the wake once per io AFTER all of
     // its RMWs is equivalent to deciding it per edge: every RMW is a full barrier ahead of the
     // parked_ load, so the Dekker pair with ThreadCtx::arm_blocked closes for each edge on its
-    // own (DESIGN-NOTIFY.md §4.4). The wake is never memoised across flushes -- a cached "already
+    // own. The wake is never memoised across flushes -- a cached "already
     // woke this io" could straddle a park/unpark cycle of the consumer and miss its second park.
     void flush_notify_batch() {
         const uint32_t n = notify_batch_n_;
@@ -3117,7 +3119,7 @@ private:
     // commands pay one predicted-true test at an executor boundary and never touch the TLS
     // commit list.
     bool xshard_commit_pending_ = false;
-    // Per-batch completion notification (DESIGN-NOTIFY.md §2). While a batch is open, the slot
+    // Per-batch completion notification. While a batch is open, the slot
     // path of notify_sender records (io, slot) here instead of fencing and setting per op; the
     // batch end pays ONE seq_cst fence, one ReadyMask::set per recorded client run and one wake
     // decision per io that saw an empty->flagged edge. Sized to this loop's largest batch; the
@@ -3139,7 +3141,7 @@ template <> uint32_t ExLoopT<true>::split_read_local_pass();
 using FusedExLoop = ExLoopT<true>;
 
 // Disabled split executors retain the exact pre-read-local allocation stride plus the 264-byte
-// per-batch notification record (DESIGN-NOTIFY.md §2): 5848 + 8 + 32 * sizeof(NotifyEntry).
+// per-batch notification record: 5848 + 8 + 32 * sizeof(NotifyEntry).
 // 6104 -> 6112: read-local eviction accounting adds exactly ONE word, the per-thread LFU dice
 // (foreign_touch_random_). Its two companions -- the latched policy byte and the fan-out defer
 // hook -- went into padding the lb bool run already carried and cost nothing. This is a per-
diff --git a/src/core/flipctl.h b/src/core/flipctl.h
index 5185da408..1edc3a938 100644
--- a/src/core/flipctl.h
+++ b/src/core/flipctl.h
@@ -19,7 +19,9 @@ namespace tomo {
 
 class Server;
 
-// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
+// Preserve the measured sampler: one whole parse pass in 100 on average. At a matched 512k ops/s,
+// a GET-only -> SET-only swap triggered the fingerprint detector once with both exhaustive and
+// 1-in-100 sampling; the sampled writer still detected the shift with the rate held fixed.
 // Arming derives solely from the controller latch. No runtime sampling knob or adaptive
 // per-operation work is needed; the detector already derives its bands from actual samples.
 inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
@@ -52,7 +54,9 @@ struct FlipFingerprintWindow {
     uint64_t closed_windows = 0;
 };
 
-// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
+// SAMPLED BY PARSE PASS. A per-op countdown would restore the store this sampler removes: no
+// existing io-side command counter covers both the point paths and all io-local commands.
+// The io loop asks pass_sampled() once per dispatched
 // frame -- one load of this writer's own word, on the line the old enabled() test already read,
 // and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
 // for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
diff --git a/src/core/io_loop.h b/src/core/io_loop.h
index e535cb9e0..30f6fa514 100644
--- a/src/core/io_loop.h
+++ b/src/core/io_loop.h
@@ -1479,7 +1479,7 @@ private:
             self_->sig().accept_err++;
             return;
         }
-        // NOTHING READ-LOCAL IS ALLOCATED HERE ANY MORE (DESIGN-RINGDIET.md). The RYOW write-ring
+        // NOTHING READ-LOCAL IS ALLOCATED HERE ANY MORE. The RYOW write-ring
         // sidecar used to be built at accept for every connection on an armed boot, so a pure-read
         // or idle connection carried 1216 bytes (a 1280-byte jemalloc class) it could never use.
         // It is now built on the first write of a connection a local read has ARMED, from that
@@ -3051,7 +3051,7 @@ private:
         const uint64_t pass_read_cut = atomic_tracking ? srv_->atomic_snapshot() : 0;
         uint64_t batch_start_ops = 0;
         [[maybe_unused]] bool read_local_batch = false;
-        // LANE ADMISSION (P128.md): how many lane slots (pending local reads) this connection may
+        // LANE ADMISSION: how many lane slots (pending local reads) this connection may
         // hold during this pass. UINT32_MAX unless the thread's local-read lane is under pressure,
         // in which case it is the lane divided among the active connections
         // (ExLoop::read_local_lane_quota). The pressure byte is tested FIRST and on its own,
@@ -3352,7 +3352,7 @@ private:
                                 (void)rob.refine_current_write_hash(op->hash);
                                 op->mark_read_local_precise_write();
                             }
-                            // DEMOTION-PLAN GATE (DESIGN-DEMOTEGATE.md). prepare() returns true
+                            // DEMOTION-PLAN GATE. prepare() returns true
                             // without building a plan -- loop_ stays null, so active() is false
                             // and commit_reads() is its `if (!loop_) return;` no-op -- unless the
                             // current append must be reserved or a still-pending local read may
@@ -3519,7 +3519,7 @@ private:
                                         ReadLocalFallbackReason::SeqChurn;
                                     read_local_eligible = false;
                                 } else if (rob.pending_read_local_count() >= read_local_quota) {
-                                    // LANE ADMISSION, fair share (P128.md): the lane is under
+                                    // LANE ADMISSION, fair share: the lane is under
                                     // pressure and this connection already holds its share of
                                     // it. DEFER -- see the lane-full arm below for the shape.
                                     // Deliberately does NOT re-arm the pressure window: a signal
@@ -3533,7 +3533,7 @@ private:
                                     break;
                                 } else if (!fused_executor_->local_read_lane_has_room(
                                                read_local_lane_demand)) {
-                                    // LANE ADMISSION, lane full (P128.md): DEFER, never demote.
+                                    // LANE ADMISSION, lane full: DEFER, never demote.
                                     // Nothing about this frame has been published -- the ROB
                                     // slot is acquired but not published, no lane entry, no
                                     // pending bit, no owner slot -- so leaving the bytes at rpos
@@ -4376,7 +4376,7 @@ ordinary_shard_ready:
         return posted;
     }
 
-    // The per-operation gate of the SAMPLED fingerprint (DESIGN-flipfp.md): one load of the
+    // The per-operation gate of the SAMPLED fingerprint: one load of the
     // writer's own word -- the line the old enabled() test already read -- and one predicted-false
     // branch. The body below (the argv walk and its five counter stores, 63-86 instr/op when it ran
     // on every frame) runs only inside a sampled parse pass. Dark writer (--flip-auto 0, every 1s
@@ -4430,8 +4430,10 @@ ordinary_shard_ready:
 
     // THE ONE DOOR ONTO THE PARSE BARRIER. Every owner parks a connection through here so the
     // overlap -- two owners holding the barrier at once -- is COUNTED rather than assumed absent.
-    // NOTES-BARRIER.md section 2 argues from the source that no production sequence produces one
-    // today; barrier_owner_overlaps is that argument's live assertion, and a validation run that
+    // A blocking op is alone in its ROB: dispatch waits for all older ops to retire, then the
+    // barrier stops younger frames; a resumed move reuses that slot and inherits its owner bit.
+    // No external production path adds another owner while it is parked.
+    // barrier_owner_overlaps is that argument's live assertion, and a validation run that
     // wants the two-owner geometry gates on it rather than trusting the prose. Cold by
     // construction: the seven owners are EXEC, a subscribe, a blocking command, a deferred WAIT,
     // a deferred DEBUG SLEEP, a barriered scatter and a CLIENT fan-out. GET and SET never reach it.
diff --git a/src/core/server.h b/src/core/server.h
index f869dd3d1..8f2792997 100644
--- a/src/core/server.h
+++ b/src/core/server.h
@@ -8,8 +8,10 @@
 //
 // THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
 // dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
-// its derived shard-granularity fast path, published only at a quiesced commit. See
-// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
+// its derived shard-granularity fast path, published only at a quiesced commit. Shards move only
+// after executing, deferred, group and pinned-read work drains; destination routes publish after
+// the ownership edge. An IO may enqueue using an old route after that edge, so tasks and borrow
+// releases recheck ownership and forward before shard access instead of carrying routing epochs.
 #pragma once
 #include <algorithm>
 #include <atomic>
@@ -308,7 +310,7 @@ public:
         for (uint32_t i = 0; i < nthreads; i++) {
             threads_[i] = std::make_unique<ThreadCtx>();
             // The fingerprint writer is armed only when its one reader, the flip controller, is
-            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
+            // enabled: with --flip-auto 0 and in 1s mode it is dark and costs
             // one predicted branch per op. The enabled sampler keeps its measured 1-in-100 policy.
             threads_[i]->init(i, placement_.role_of(i), nthreads,
                               0,
@@ -1969,7 +1971,7 @@ public:
         shard_owner_[shard_id].store(thread_id, std::memory_order_release);
     }
 
-    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
+    // The caller must hold both executor loops at the safe point:
     // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
     // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
     // Vector capacity and membership are settled while PREPARING still names the source.  The
@@ -1988,7 +1990,7 @@ public:
     // lost, a block ends up linked twice, and the damage surfaces later as either
     // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
     // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
-    // Both were observed (see DESIGN-P0REPLY.md).
+    // Both were observed.
     //
     // The rebind used to be deferred to the destination's own next executor pass
     // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
@@ -2827,7 +2829,7 @@ public:
     // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
     // other command. It widens -- deterministically -- the window in which a younger whole-owner
     // walker could overtake an older same-connection group on the destination shard.
-    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
+    // TEST HOOK: effective local-read LANE CAPACITY for admission. 0 means
     // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
     // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
     // its masking, so this can never overrun anything. It exists because lane oversubscription is
@@ -2935,9 +2937,11 @@ public:
     // is already set. Nothing on GET/SET touches it.
     //
     // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
-    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
-    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
-    // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
+    // sequence: a blocking op requires an empty ROB, then sets the barrier before any younger
+    // frame can be parsed; a resumed scatter inherits that same slot and barrier. The state the
+    // owner-scoped release exists to survive -- a release that must NOT drop the barrier -- has
+    // to be injected. It is held past ROB quiescence on purpose; barrier_release_quiesced()
+    // exempts this one bit for that reason.
     //
     // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
     // nothing left to do and parks on submit_and_wait(1) -- unbounded under io_uring. A hold that
@@ -2968,11 +2972,11 @@ public:
         return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
     }
     // An overlap: some owner took the parse barrier while another owner already held it. With
-    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
-    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
-    // live and the owner-scoped release in blocking_retire() became load-bearing rather than
-    // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
-    // own positive control: a "must be zero" reading proves nothing until something has been shown
+    // DEBUG BARRIER-HOLD off this must read ZERO -- if it ever moves on production traffic the
+    // latent case just went live and the owner-scoped release in blocking_retire() became
+    // load-bearing rather than defensive. With the latch ON it advances once per blocking dispatch,
+    // which is the counter's own positive control: a "must be zero" reading proves nothing until
+    // something has been shown
     // able to make it non-zero.
     void note_barrier_overlap() {
         barrier_owner_overlaps_.fetch_add(1, std::memory_order_relaxed);
diff --git a/src/core/shard.h b/src/core/shard.h
index 694b9a93c..39a049d2f 100644
--- a/src/core/shard.h
+++ b/src/core/shard.h
@@ -431,7 +431,7 @@ public:
         // NOT share this counter: multi.inc's prepare_write_key() re-asks it at install time and
         // reports violations through INFO atomic_exec_order_late (multi_exec_order_late()), which
         // must read zero. Summing the two would hide a violation inside a number that is non-zero
-        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
+        // by design, leaving the park's safety argument with no falsifier.
         uint64_t atomic_exec_order_holds = 0;
         // Times watch_finalize_reservation() answered "not ready" because the reservation's epoch
         // was still 0, i.e. a unit was turned into a Retry by an undecided WATCH reservation. It
diff --git a/src/core/signal.h b/src/core/signal.h
index 933a5d7ee..f69f7d739 100644
--- a/src/core/signal.h
+++ b/src/core/signal.h
@@ -47,7 +47,7 @@ inline uint64_t now_ns() {
 // that will be compared against a deadline must come from here and never from now_ns() above --
 // that one is CLOCK_MONOTONIC, so its "milliseconds" are milliseconds since boot and sit roughly
 // five orders of magnitude below any real deadline. A monotonic value used as an expiry cut does
-// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
+// not skew the answer, it disables expiry outright.
 inline int64_t now_realtime_ms() {
     timespec ts;
     clock_gettime(CLOCK_REALTIME, &ts);
diff --git a/src/core/thread.h b/src/core/thread.h
index 0fcfa17ba..3cf72c1b9 100644
--- a/src/core/thread.h
+++ b/src/core/thread.h
@@ -118,7 +118,7 @@ enum class ReadLocalFallbackReason : uint8_t {
     ContextRoute,           // scatter/script/all-shard or conservative broad-owner route
     ContextKeymissNotify,   // MGET miss must retain owner-side notification behavior
     InflightWrite,
-    ArmTransient,           // pre-arming writes still in flight (DESIGN-RINGDIET.md)
+    ArmTransient,           // pre-arming writes still in flight
     AtomicPending,
     Missing,
     Typed,
@@ -151,7 +151,7 @@ struct ReadLocalStats {
     // ARM-ON-DEMAND TRANSIENT. Reads demoted because the connection carried writes published
     // BEFORE a local read armed it -- writes the RYOW ring deliberately never recorded. Bounded
     // per connection by one ROB drain and reported separately from the steady-state key conflict
-    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
+    // above so that the two can never be confused in a bench.
     uint64_t fallback_arm_transient = 0;
     uint64_t fallback_atomic_pending = 0;
     uint64_t fallback_missing = 0;
@@ -160,7 +160,7 @@ struct ReadLocalStats {
     uint64_t fallback_seq_churn = 0;
     uint64_t fallback_generation = 0;
     uint64_t fallback_lane_full = 0;
-    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
+    // Lane ADMISSION deferrals. Frames the armed parser left unconsumed at rpos, to be
     // re-parsed by a later pass of the same thread, because the local-read lane had no room
     // (defer_lane_full) or because the connection already held its fair share of a lane under
     // pressure (defer_quota). Neither is a fallback: the read still completes locally, and no
diff --git a/src/net/conn.h b/src/net/conn.h
index 7ace0c2dd..8eea2f6cd 100644
--- a/src/net/conn.h
+++ b/src/net/conn.h
@@ -83,9 +83,10 @@ struct Session {
 
 // WHO IS HOLDING THE PARSE BARRIER. Six independent mechanisms park a connection's parse pass, and
 // they used to share ONE bool -- so any one of them could clear a barrier another one still needed.
-// No reachable interleaving overlapped two owners (see NOTES-BARRIER.md section 2: a blocking op is
-// provably alone in its ROB, and every other owner ends the parse pass on the spot), which is
-// exactly why the bool survived: the hazard is one relaxed guard away, not present. Owner bits make
+// No reachable interleaving overlapped two owners: a blocking op waits for an empty ROB, then sets
+// the barrier and ends parsing, so it has neither older nor younger neighbours; every other owner
+// also ends the parse pass on the spot. That is exactly why the bool survived: the hazard is one
+// relaxed guard away, not present. Owner bits make
 // the release symmetric with the acquire -- whoever set it is the one whose release can drop it --
 // and cost nothing: the byte was already there, and "is any owner holding" is still one byte test.
 //
diff --git a/src/net/rob.h b/src/net/rob.h
index 808d62102..4fddc4733 100644
--- a/src/net/rob.h
+++ b/src/net/rob.h
@@ -51,7 +51,7 @@ namespace tomo {
 // its unreachability proof is written against. net/conn.h re-exports it by including this file.
 inline constexpr uint32_t kRobWindow = 64;   // max in-flight ops per connection
 
-// COLD-PATH COUNTERS FOR ARM-ON-DEMAND (DESIGN-RINGDIET.md). Three events, none of them on a hot
+// COLD-PATH COUNTERS FOR ARM-ON-DEMAND. Three events, none of them on a hot
 // path: a connection being armed, its RYOW sidecar being allocated, and one write being committed
 // into the ring. They are the instrument the design is proved with -- "pure SET does zero ring
 // bookkeeping" is exactly write_ring_records staying at zero -- so they are always on rather than
@@ -133,8 +133,8 @@ static_assert(ReadLocalRobState::kWriteRingCapacity <=
 // rounds to its 1280-byte class -- measured as +965 bytes of RSS per connection that owns one
 // (scratchpad/ringsize section 3) against the sixteen-slot sidecar's 296/320. It is NO LONGER paid
 // by every connection: arm-on-demand allocates it at the first write of an ARMED connection, so a
-// pure-write connection, a pure-read connection and an idle connection each carry none of it
-// (DESIGN-RINGDIET.md). Locked all the same, so that a later field cannot quietly add another size
+// pure-write connection, a pure-read connection and an idle connection each carry none of it.
+// Locked all the same, so that a later field cannot quietly add another size
 // class to every read/write connection without someone re-measuring that number.
 // The MGET latest-read fence used to live here and now lives in the Rob: it is a read-side fence
 // with nothing to do with the write ring, and keeping it here would have forced a pure-MGET
@@ -315,7 +315,7 @@ public:
     // until this id either completes locally or is irrevocably transferred to the owner path. It
     // lives on the Rob's own producer line rather than in the write-ring sidecar because it is a
     // READ-side fence -- a connection that only ever sends MGETs must not have to allocate 1216
-    // bytes of write ring to hold one id (DESIGN-RINGDIET.md).
+    // bytes of write ring to hold one id.
     void arm_current_local_mget_fence() {
         if (local_mget_fence_id_ != UINT64_MAX) std::abort();
         local_mget_fence_id_ = dispatch_id();
@@ -338,7 +338,7 @@ public:
 
     bool has_pending_read_local() const { return read_local_pending_slots_ != 0; }
     // Lane slots this connection holds right now: local reads published and not yet executed (or
-    // demoted). The parser's fair-share admission (P128.md) compares THIS, not in_flight(): a
+    // demoted). The parser's fair-share admission compares THIS, not in_flight(): a
     // reply that PHASE 2's bounded serve has not flushed yet holds no lane slot and must not
     // count against the connection's share (measured: it halved ops per rotation).
     uint32_t pending_read_local_count() const {
@@ -361,7 +361,7 @@ public:
     // Retires through the same clear-on-empty rule as the per-op form: when the drained chunk was
     // the whole pending set the summary restarts from zero (the contract stated in fcab80884; the
     // chunk form had dropped it, so the filter only ever grew in read-heavy streams and the first
-    // write of every batch paid the exact walk — AUDIT-MARKDIET.md section 4).
+    // write of every batch paid the exact walk).
     void complete_pending_read_local_mask(uint64_t bits) {
         if ((read_local_pending_slots_ & bits) != bits) std::abort();
         read_local_retire_pending_bit(bits);
@@ -471,7 +471,7 @@ public:
     // Every write starts conservative. After arity and routing are known, ordinary point writes
     // and bounded blind keysets may refine it; all other special/multi-key writes leave it broad.
     void mark_current_write() {
-        // ARM ON DEMAND, THE WRITE HALF (DESIGN-RINGDIET.md). Until a local read has armed this
+        // ARM ON DEMAND, THE WRITE HALF. Until a local read has armed this
         // connection the ring records NOTHING: no sidecar, no prune, no descriptor, no Staged tag
         // -- and so no resolve on the next frame either. The entire bookkeeping is one store of
         // this write's id, into a word on the producer's own cache line that dispatch_ has already
@@ -557,7 +557,7 @@ public:
     template <typename KeysetTouchesHash>
     __attribute__((always_inline)) bool read_local_write_conflicts(
             uint64_t hash, KeysetTouchesHash&& keyset_touches_hash) {
-        // ARM ON DEMAND, THE READ HALF (DESIGN-RINGDIET.md). ONE predictable test, on a word this
+        // ARM ON DEMAND, THE READ HALF. ONE predictable test, on a word this
         // frame's acquire_read_local has already pulled into L1 with dispatch_, and nothing is
         // evaluated behind it: a connection in steady state (armed, no arming generation left in
         // flight) reads a zero and falls straight through to the unchanged probe below. The word
@@ -687,7 +687,7 @@ public:
     }
 
     // The RYOW write ring's sidecar. Called from the connection's own IO thread on the first
-    // write of an ARMED connection -- never at accept (DESIGN-RINGDIET.md) -- and directly by the
+    // write of an ARMED connection -- never at accept -- and directly by the
     // unit test, which drives the ROB without a server. Returns false only on allocation failure;
     // every caller has a safe unarmed fallback for that.
     bool prepare_read_local() {
@@ -805,7 +805,7 @@ private:
     static constexpr uintptr_t kReadLocalStateTagBits =
         kReadLocalStateInactive | kReadLocalStateStaged;
 
-    // ARM-ON-DEMAND STATE (DESIGN-RINGDIET.md), one word on the producer line, zero in the state
+    // ARM-ON-DEMAND STATE, one word on the producer line, zero in the state
     // every connection ends up in. Two non-zero values, and a connection passes through each of
     // them at most once:
     //   Unarmed   no local read has been seen. Writes record nothing but their newest id.
diff --git a/src/store/flatstore.h b/src/store/flatstore.h
index faa413ec5..6681dbbeb 100644
--- a/src/store/flatstore.h
+++ b/src/store/flatstore.h
@@ -591,7 +591,9 @@ public:
 
     // Eight slot bytes at the 70% target load cost 11.43 bytes per live key. Accounting rounds
     // that stable-state estimate to 12; transient dual tables, tombstones and allocator metadata
-    // are deliberately outside the maxmemory model and documented in NOTES-EVICT.md.
+    // are deliberately outside the maxmemory model: charging those costs would make enforcement
+    // depend on resize history rather than maintained object/key counters. This is a stable
+    // logical cache budget, not an RSS ceiling.
     static constexpr size_t   kSlotOverheadPerKey = 12;
     static constexpr uint32_t kEvictionsPerOp = 16;
     static constexpr uint32_t kSampleProbeAttempts = 16;
diff --git a/src/store/flatstore_atomic.inc b/src/store/flatstore_atomic.inc
index 647b0850e..181f9e4f6 100644
--- a/src/store/flatstore_atomic.inc
+++ b/src/store/flatstore_atomic.inc
@@ -240,8 +240,9 @@
     // unit publishes through, not just one: an EXEC's plain per-key installs use the transaction's
     // epoch, while each cross-shard child command lowered inside that EXEC keeps its own
     // ScatterState and therefore its own. Answering with only the transaction's word makes a
-    // transaction hold against its own RENAME/LMPOP child and hang -- the same self-hold
-    // NOTES-MULTIRES.md §5(a) hit from the other direction. Plain single-command pseudo-entries
+    // transaction hold against its own RENAME/LMPOP child and hang. Testing group pointers instead
+    // also deadlocked: an EXEC child's scatter record names a group, while the same EXEC's plain
+    // installs do not. Plain single-command pseudo-entries
     // carry no group_epoch at all: they draw their ticket in the same owner turn that installs
     // them, so they are never undecided and are skipped here.
     template <typename IsOwnUnit>
@@ -1429,7 +1430,7 @@ private:
         return true;
     }
 
-    // ---- armed-write block cache (src/store/kv_block_cache.h, NOTES-RECYCLE.md) ---------------
+    // ---- armed-write block cache (src/store/kv_block_cache.h) ---------------
     //
     // The cache itself belongs to the fused OWNER and is reached through the retire sink this store
     // is already bound to; the store contributes only the eligibility a shard can judge (object
@@ -1782,7 +1783,7 @@ private:
             // waiting on other shards can be overtaken at the ticket counter by a MULTI/EXEC the
             // same connection sent afterwards. Ranking those two by raw ticket then re-installed
             // the OLDER group's value over the newer transaction's, destroying a write EXEC had
-            // already acknowledged -- reproduced at 246/1120 (NOTES-MULTIRES.md).
+            // already acknowledged -- reproduced at 246/1120.
             // Repairing the epoch to be monotone per connection removes the inversion without
             // reordering anything across connections, where the ticket remains the only truth.
             if (key.prev_set && owner.origin_conn_id == key.prev_conn_id &&
@@ -2171,7 +2172,7 @@ private:
             // waiting on other shards can be overtaken at the ticket counter by a MULTI/EXEC the
             // same connection sent afterwards. Ranking those two by raw ticket then re-installed
             // the OLDER group's value over the newer transaction's, destroying a write EXEC had
-            // already acknowledged -- reproduced at 246/1120 (NOTES-MULTIRES.md).
+            // already acknowledged -- reproduced at 246/1120.
             // Repairing the epoch to be monotone per connection removes the inversion without
             // reordering anything across connections, where the ticket remains the only truth.
             if (key.prev_set && owner.origin_conn_id == key.prev_conn_id &&
diff --git a/src/store/kv_block_cache.h b/src/store/kv_block_cache.h
index b3da12827..63c8dc0a3 100644
--- a/src/store/kv_block_cache.h
+++ b/src/store/kv_block_cache.h
@@ -67,8 +67,9 @@ struct KvBlockCache {
     //
     //  * kMaxNodesPerClass — one grace drain's worth. A drain releases at most one retire ring of
     //    objects, so a class that refuses blocks below that number is refusing memory the very
-    //    next owner pass will ask for again. Measured: with a 32-node cap the reuse rate collapses
-    //    to a few percent and the whole mechanism stops paying (see NOTES-RECYCLE.md).
+    //    next owner pass will ask for again. Measured on one fused owner at 64 shards, a 32-node
+    //    cap left 0.5001/0.3750 mallocx calls per op at pipeline 64/256; the 4096-node cap left
+    //    zero in both cells for only 2-4 KiB more cached memory.
     //  * kMaxBytes — one grace drain of maximum-inline-value objects. The retire ring ALREADY
     //    permits kReadLocalRetireRingCapacity retired-but-unreclaimed objects per owner to be
     //    resident, and kEmbedThreshold is the largest value an eligible block can carry, so this
diff --git a/src/store/read_local_settax.h b/src/store/read_local_settax.h
index 4e9546df3..94835c262 100644
--- a/src/store/read_local_settax.h
+++ b/src/store/read_local_settax.h
@@ -8,7 +8,7 @@
 //
 // Selector 2 (post-QSBR same-class recycling) is RETIRED: it won its bake-off and was hardcoded.
 // Armed writes now draw their block from, and return it to, the shard's own post-grace block cache
-// unconditionally (FlatStore::read_local_cache_take/put, NOTES-RECYCLE.md), so there is no longer a
+// unconditionally (FlatStore::read_local_cache_take/put), so there is no longer a
 // build that recycles and a build that does not. Selecting 2 is a compile error rather than a
 // silent second recycler competing with the shipped one.
 #pragma once

exec
/bin/bash -lc 'git diff -- src/cmd' in /home/user/Projects/cx-inline-comments
 succeeded in 0ms:
diff --git a/src/cmd/blocking.inc b/src/cmd/blocking.inc
index 334a87a45..ed7aea46c 100644
--- a/src/cmd/blocking.inc
+++ b/src/cmd/blocking.inc
@@ -1348,7 +1348,7 @@ void blocking_retire(Server& server, Client& client, Op& op, ThreadCtx& thread)
     // rob.in_flight() != 0, and the barrier this dispatch then takes stops the parse pass, so a
     // blocking op is provably ALONE in its ROB and no second owner can exist here. Nothing near
     // this line said so, and a relaxed guard or a seventh owner would have made it a live
-    // ordering defect of exactly the class t-storeorder fixed. NOTES-BARRIER.md has the audit.
+    // ordering defect of exactly the class t-storeorder fixed.
     //
     // Note the alternative -- clear only at ROB quiescence -- cannot be spelled here: Rob::drain
     // hands each op to this callback while flush_ is still at that op and stores the advanced
diff --git a/src/cmd/climon.cc b/src/cmd/climon.cc
index 2920a8305..fda04e415 100644
--- a/src/cmd/climon.cc
+++ b/src/cmd/climon.cc
@@ -605,7 +605,7 @@ bool IoLoop::climon_pause_holds(Op& op) {
     if (cached_now_ms_ >= climon_pause_deadline_ms_) return false;
     const CommandSpec* spec = op.spec;
     if (!spec) return false;
-    // DELIBERATE DIVERGENCE FROM REDIS, documented in NOTES-CLIMON2.md: redis postpones CLIENT
+    // DELIBERATE DIVERGENCE FROM REDIS: redis postpones CLIENT
     // UNPAUSE itself under PAUSE ... ALL, so an ALL pause can only end by expiring. We exempt the
     // connection-control class (CLIENT/RESET/MONITOR, the CmdFlags::Climon rows) so UNPAUSE
     // always works. Everything else -- including PING and reads -- is held under ALL, matching
diff --git a/src/cmd/multi.h b/src/cmd/multi.h
index da54f58f9..dbea965a1 100644
--- a/src/cmd/multi.h
+++ b/src/cmd/multi.h
@@ -64,13 +64,13 @@ bool multi_task_tagged(const Task& task);
 // installs publish through its own epoch word; every cross-shard command lowered inside the same
 // EXEC keeps its own ScatterState and publishes through that one. A caller asking "is there an
 // undecided record here that is not mine" has to accept both, or a transaction holds against its
-// own child (NOTES-MULTIRES.md §5(a)).
+// own child.
 bool multi_task_owns_epoch(const Task& task, const void* group_epoch);
 // INFO atomic_exec_order_late: times an EXEC write reached its install with an undecided record
 // from another logical unit of the same connection still on this owner -- the ordering violation
 // the dispatch-time park in ExLoop::execute() exists to make impossible. It is a BUG counter and
 // must read zero; it is separate from atomic_exec_order_holds precisely so that "the hold fired"
-// and "the hold fired too late" cannot be confused for one another (NOTES-MULTIRACE.md §5).
+// and "the hold fired too late" cannot be confused for one another.
 uint64_t multi_exec_order_late();
 Task multi_make_task(Client* client, uint64_t op_id, int32_t shard, MultiExecState* state);
 MultiTaskResult multi_execute_task(Server& server, const Task& task, Shard& shard,
diff --git a/src/cmd/multi.inc b/src/cmd/multi.inc
index f4b573f3d..ec1f4f2a5 100644
--- a/src/cmd/multi.inc
+++ b/src/cmd/multi.inc
@@ -904,7 +904,7 @@ MultiPrepareWrite prepare_write_key(Server& server, MultiExecState& state, Shard
     // The park is supposed to foreclose it completely, because only this owner thread may publish
     // into this owner's pending list, so nothing can appear between the dispatch check and these
     // installs. A non-zero reading is therefore not a statistic: it is proof that the park's
-    // safety argument (NOTES-MULTIRACE.md §5) is false on this build, and that a transaction could
+    // safety argument is false on this build, and that a transaction could
     // again clone a withdrawable candidate into an acknowledged write.
     //
     // It counts into its OWN counter, deliberately. Sharing atomic_exec_order_holds with the park
@@ -915,9 +915,10 @@ MultiPrepareWrite prepare_write_key(Server& server, MultiExecState& state, Shard
     // Not a Shard::Stats field because Shard's layout is locked at 1440 bytes; a namespace-scope
     // atomic costs nothing here, since the increment is on a path that is never taken.
     //
-    // Holding the transaction HERE instead of counting was tried and is WRONG -- it deadlocks,
-    // NOTES-MULTIRES.md §5(a); the park is safe only because it happens before the fragment has
-    // installed anything on this owner.
+    // Holding the transaction HERE deadlocked with one connection and no WATCH (0 rounds versus
+    // 300 on base): the older group could wait on the transaction's partial installs while the
+    // transaction waited on that group. The dispatch-time park is safe because the fragment has
+    // installed nothing on this owner, so it cannot give the older group a reason to wait here.
     // The exclusion is BY EPOCH WORD and must cover the transaction's lowered children as well as
     // its own: a cross-shard child installs through the scatter path with its own ScatterState,
     // and counting that would report a violation every time a transaction merely met its own
diff --git a/src/cmd/scripting.cc b/src/cmd/scripting.cc
index c4187b5d0..cebaa6a4a 100644
--- a/src/cmd/scripting.cc
+++ b/src/cmd/scripting.cc
@@ -237,7 +237,7 @@ bool validate_source(Slice source, std::string& error) {
 // THERE IS NO UNDO LOG, IN EITHER ATOMIC MODE — AND THAT IS THE SEMANTICS, NOT A GAP.
 //
 // v1 kept a deep pre-image of every declared key while `--atomic 1` was on and restored it when the
-// activation failed (see NOTES-SCRIPTATOMIC.md). Redis has never undone a script's partial effects,
+// activation failed. Redis has never undone a script's partial effects,
 // so an activation that wrote and then raised diverged from the oracle the moment atomics were
 // enabled, and the divergence was a LOST WRITE: the restore republished a superseded value over a
 // committed one, or erased the key outright when the script had just created it. It also could not
diff --git a/src/cmd/server_tail.cc b/src/cmd/server_tail.cc
index b3e994d94..b1697b9fe 100644
--- a/src/cmd/server_tail.cc
+++ b/src/cmd/server_tail.cc
@@ -173,7 +173,7 @@ void cmd_wait(Shard&, Op& op) {
 // against vanilla redis. The numlocal == 1 form -- "block until my writes are covered by a local
 // fsync" -- is NOT implemented and returns an explicit error rather than a plausible number.
 //
-// Why it is not implemented here, and what it needs (NOTES-SERVERTAIL.md carries the full design):
+// Why it is not implemented here, and what it needs:
 // a connection-local handler runs at PARSE time, before the ops ahead of it on the same connection
 // have executed, so the AOF sequence it could sample does not yet cover the caller's own writes.
 // Waiting synchronously cannot fix that: retiring those older ops requires this very IO thread, so
@@ -230,7 +230,7 @@ void cmd_failover(Shard&, Op& op) {
 
 // DELIBERATE DEVIATION. Redis answers +OK and starts replicating. TomoKV has no replication at
 // all, and silently accepting the command would leave a client believing it had a replica. An
-// explicit error is the honest answer; it is documented in NOTES-SERVERTAIL.md.
+// explicit error is the honest answer.
 void cmd_replicaof(Shard&, Op& op) {
     reply_err(op.sink(), "ERR replication is not supported by tomokv");
 }
diff --git a/src/cmd/slowlog.cc b/src/cmd/slowlog.cc
index 77db6d675..17e1a2abe 100644
--- a/src/cmd/slowlog.cc
+++ b/src/cmd/slowlog.cc
@@ -19,7 +19,7 @@
 // The honest consequence, stated plainly: a command that is slow EXACTLY ONCE while sharing a
 // pipelined batch with other commands is not logged; it arms escalation instead, and its next
 // occurrence is logged exactly. Redis times every command individually and has no such window.
-// The trade buys the default-on hot path back. See NOTES-SERVERTAIL.md.
+// The trade buys the default-on hot path back.
 #include "slowlog.h"
 
 #include "command.h"
diff --git a/src/cmd/slowlog.h b/src/cmd/slowlog.h
index 54d2213f2..65db841d1 100644
--- a/src/cmd/slowlog.h
+++ b/src/cmd/slowlog.h
@@ -5,8 +5,9 @@
 // not per op -- and reads no clock at all. Everything below that gate is out of line.
 //
 // WHEN ARMED the executor times at BATCH granularity (two clock reads per <=32 ops) and escalates
-// to per-op timing only after a batch overruns. See NOTES-SERVERTAIL.md for the attribution
-// semantics, which differ from redis's and are documented rather than papered over.
+// to per-op timing only after a batch overruns. A one-op batch is exact; a multi-op batch is
+// screened by its per-op average and, if it overruns, arms exact timing for the next 64 batches.
+// Unlike redis's per-command timing, a one-off slow op in that first multi-op batch is not logged.
 #pragma once
 #include <cstdint>
 
diff --git a/src/cmd/t_server.cc b/src/cmd/t_server.cc
index 3609f7b60..bcf612c3e 100644
--- a/src/cmd/t_server.cc
+++ b/src/cmd/t_server.cc
@@ -374,7 +374,7 @@ void init_config(const Config& cfg) {
     g_config.push_back({"notify-keyspace-events", ConfigKind::NotifyFlags,
                         serialize_notify_flags(cfg.notify_events)});
     // Boot-latched: the io owners read the bound directly out of Config, so it is reported but
-    // not live-settable (redis allows CONFIG SET; see NOTES-CLIMON2.md).
+    // not live-settable (redis allows CONFIG SET).
     g_config.push_back({"tracking-table-max-keys", ConfigKind::Unsigned,
                         std::to_string(cfg.tracking_table_max_keys), true});
     add_config("databases", ConfigKind::Unsigned, cfg.databases);
@@ -932,7 +932,7 @@ void cmd_debug_impl(Shard& shard, Op& op) {
     // connection pipelining more than the cap in one parse pass oversubscribes the lane inside
     // that pass. That is what makes the battery's anti-vacuity checks deterministic at gate scale
     // instead of a rate race against the drain that only a saturated rig can win. 0 restores the
-    // derived value, which is what production always runs. See P128.md section 8.
+    // derived value, which is what production always runs.
     if (eq_icase(subcommand, "read-local-lane-cap") && op.argc() == 3) {
         uint64_t cap = 0;
         if (!parse_u64(op.arg(2), cap) || cap > kInboxSlots) {
@@ -946,10 +946,11 @@ void cmd_debug_impl(Shard& shard, Op& op) {
     }
     // GEOMETRY INJECTOR for the parse-barrier ownership regression. While armed, every blocking
     // dispatch pins a SECOND owner on its connection's parse barrier, so the blocking command's
-    // retirement releases a barrier it does not solely own. That two-owner state is unreachable on
-    // any production sequence (NOTES-BARRIER.md section 2) -- which is why it must be injected
-    // rather than provoked, and why a battery that only replays real command sequences proves
-    // nothing about this code. Production default is 0.
+    // retirement releases a barrier it does not solely own. Production blocking dispatch requires
+    // an empty ROB and then bars younger parsing, so the blocking op is alone and cannot meet a
+    // second owner. That is why the overlap must be injected rather than provoked, and why a
+    // battery that only replays real command sequences proves nothing about this code.
+    // Production default is 0.
     //
     // Observable while armed: a frame pipelined BEHIND a blocking command stays unparsed after the
     // blocking reply retires, instead of being answered in the same flush pass. Clearing the latch
@@ -2486,10 +2487,11 @@ void cmd_info(Shard&, Op& op) {
                 // reached the non-quiesced / mid-drain geometry it is there to cover.
                 static_cast<unsigned long long>(g_server ? g_server->oob_frames_segmented() : 0),
                 static_cast<unsigned long long>(g_server ? g_server->oob_frames_deferred() : 0),
-                // barrier_owner_overlaps must read 0 on any production run: it is the live
-                // assertion behind NOTES-BARRIER.md's reachability verdict. barrier_releases_held
-                // is the fired-mechanism proof for DEBUG BARRIER-HOLD -- a barrier-ownership test
-                // that leaves it at 0 never reached its geometry and must fail, not pass.
+                // barrier_owner_overlaps must read 0 on any production run: blocking dispatch
+                // requires an empty ROB and bars younger parsing, excluding a second owner.
+                // barrier_releases_held is the fired-mechanism proof for DEBUG BARRIER-HOLD --
+                // a barrier-ownership test that leaves it at 0 never reached its geometry and
+                // must fail, not pass.
                 static_cast<unsigned long long>(
                     g_server ? g_server->barrier_owner_overlaps() : 0),
                 static_cast<unsigned long long>(

diff --git a//home/user/Projects/cx-inline-comments/src/core/config.h b//home/user/Projects/cx-inline-comments/src/core/config.h
index 495e6ae42da15ee4bcc76a8a846efeef506903d7..8b5f3e5c786e8eeae0296a5dcf19338056d59007
--- a//home/user/Projects/cx-inline-comments/src/core/config.h
+++ b//home/user/Projects/cx-inline-comments/src/core/config.h
@@ -290,8 +290,8 @@
     uint32_t client_lb = 1;
 
     // ---- automatic role split (boot-latched) -----------------------------------------------
-    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer
-    // (DESIGN-flipfp.md); its measured sampling policy is internal to flipctl.h. Off and 1s
+    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer;
+    // its measured sampling policy is internal to flipctl.h. Off and 1s
     // dispatch pay one predicted branch per op and no fingerprint store. Trigger bands learn
     // from the anchor's own quiet jitter; no separate controller tuning knobs remain.
     uint32_t flip_auto = 0;
diff --git a//home/user/Projects/cx-inline-comments/src/core/flipctl.h b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
index 5185da4085ae5f2e3325702064208810cb9c42f7..1edc3a93889090da659db0c9ee259d04ce5cef57
--- a//home/user/Projects/cx-inline-comments/src/core/flipctl.h
+++ b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
@@ -19,7 +19,9 @@
 
 class Server;
 
-// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
+// Preserve the measured sampler: one whole parse pass in 100 on average. At a matched 512k ops/s,
+// a GET-only -> SET-only swap triggered the fingerprint detector once with both exhaustive and
+// 1-in-100 sampling; the sampled writer still detected the shift with the rate held fixed.
 // Arming derives solely from the controller latch. No runtime sampling knob or adaptive
 // per-operation work is needed; the detector already derives its bands from actual samples.
 inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
@@ -52,7 +54,9 @@
     uint64_t closed_windows = 0;
 };
 
-// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
+// SAMPLED BY PARSE PASS. A per-op countdown would restore the store this sampler removes: no
+// existing io-side command counter covers both the point paths and all io-local commands.
+// The io loop asks pass_sampled() once per dispatched
 // frame -- one load of this writer's own word, on the line the old enabled() test already read,
 // and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
 // for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
diff --git a//home/user/Projects/cx-inline-comments/src/core/server.h b//home/user/Projects/cx-inline-comments/src/core/server.h
index f869dd3d1737afa6b1c401919fb66c96772b1225..8f279299796e7dabffe155115dea7b3ff379627b
--- a//home/user/Projects/cx-inline-comments/src/core/server.h
+++ b//home/user/Projects/cx-inline-comments/src/core/server.h
@@ -8,8 +8,10 @@
 //
 // THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
 // dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
-// its derived shard-granularity fast path, published only at a quiesced commit. See
-// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
+// its derived shard-granularity fast path, published only at a quiesced commit. Shards move only
+// after executing, deferred, group and pinned-read work drains; destination routes publish after
+// the ownership edge. An IO may enqueue using an old route after that edge, so tasks and borrow
+// releases recheck ownership and forward before shard access instead of carrying routing epochs.
 #pragma once
 #include <algorithm>
 #include <atomic>
@@ -308,7 +310,7 @@
         for (uint32_t i = 0; i < nthreads; i++) {
             threads_[i] = std::make_unique<ThreadCtx>();
             // The fingerprint writer is armed only when its one reader, the flip controller, is
-            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
+            // enabled: with --flip-auto 0 and in 1s mode it is dark and costs
             // one predicted branch per op. The enabled sampler keeps its measured 1-in-100 policy.
             threads_[i]->init(i, placement_.role_of(i), nthreads,
                               0,
@@ -1969,7 +1971,7 @@
         shard_owner_[shard_id].store(thread_id, std::memory_order_release);
     }
 
-    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
+    // The caller must hold both executor loops at the safe point:
     // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
     // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
     // Vector capacity and membership are settled while PREPARING still names the source.  The
@@ -1988,7 +1990,7 @@
     // lost, a block ends up linked twice, and the damage surfaces later as either
     // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
     // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
-    // Both were observed (see DESIGN-P0REPLY.md).
+    // Both were observed.
     //
     // The rebind used to be deferred to the destination's own next executor pass
     // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
@@ -2827,7 +2829,7 @@
     // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
     // other command. It widens -- deterministically -- the window in which a younger whole-owner
     // walker could overtake an older same-connection group on the destination shard.
-    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
+    // TEST HOOK: effective local-read LANE CAPACITY for admission. 0 means
     // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
     // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
     // its masking, so this can never overrun anything. It exists because lane oversubscription is
@@ -2935,9 +2937,11 @@
     // is already set. Nothing on GET/SET touches it.
     //
     // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
-    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
-    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
-    // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
+    // sequence: a blocking op requires an empty ROB, then sets the barrier before any younger
+    // frame can be parsed; a resumed scatter inherits that same slot and barrier. The state the
+    // owner-scoped release exists to survive -- a release that must NOT drop the barrier -- has
+    // to be injected. It is held past ROB quiescence on purpose; barrier_release_quiesced()
+    // exempts this one bit for that reason.
     //
     // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
     // nothing left to do and parks on submit_and_wait(1) -- unbounded under io_uring. A hold that
@@ -2968,11 +2972,11 @@
         return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
     }
     // An overlap: some owner took the parse barrier while another owner already held it. With
-    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
-    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
-    // live and the owner-scoped release in blocking_retire() became load-bearing rather than
-    // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
-    // own positive control: a "must be zero" reading proves nothing until something has been shown
+    // DEBUG BARRIER-HOLD off this must read ZERO -- if it ever moves on production traffic the
+    // latent case just went live and the owner-scoped release in blocking_retire() became
+    // load-bearing rather than defensive. With the latch ON it advances once per blocking dispatch,
+    // which is the counter's own positive control: a "must be zero" reading proves nothing until
+    // something has been shown
     // able to make it non-zero.
     void note_barrier_overlap() {
         barrier_owner_overlaps_.fetch_add(1, std::memory_order_relaxed);
diff --git a//home/user/Projects/cx-inline-comments/src/core/shard.h b//home/user/Projects/cx-inline-comments/src/core/shard.h
index 694b9a93c6cd63db49b9dcf061dbb5182c49986a..39a049d2f8619bbf52f706775956cc9198060364
--- a//home/user/Projects/cx-inline-comments/src/core/shard.h
+++ b//home/user/Projects/cx-inline-comments/src/core/shard.h
@@ -431,7 +431,7 @@
         // NOT share this counter: multi.inc's prepare_write_key() re-asks it at install time and
         // reports violations through INFO atomic_exec_order_late (multi_exec_order_late()), which
         // must read zero. Summing the two would hide a violation inside a number that is non-zero
-        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
+        // by design, leaving the park's safety argument with no falsifier.
         uint64_t atomic_exec_order_holds = 0;
         // Times watch_finalize_reservation() answered "not ready" because the reservation's epoch
         // was still 0, i.e. a unit was turned into a Retry by an undecided WATCH reservation. It
diff --git a//home/user/Projects/cx-inline-comments/src/core/signal.h b//home/user/Projects/cx-inline-comments/src/core/signal.h
index 933a5d7ee09538f3923df3c5eb77f9c72cdc0bfa..f69f7d73942ee129f4ad9cd1f23befe69fc4167d
--- a//home/user/Projects/cx-inline-comments/src/core/signal.h
+++ b//home/user/Projects/cx-inline-comments/src/core/signal.h
@@ -47,7 +47,7 @@
 // that will be compared against a deadline must come from here and never from now_ns() above --
 // that one is CLOCK_MONOTONIC, so its "milliseconds" are milliseconds since boot and sit roughly
 // five orders of magnitude below any real deadline. A monotonic value used as an expiry cut does
-// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
+// not skew the answer, it disables expiry outright.
 inline int64_t now_realtime_ms() {
     timespec ts;
     clock_gettime(CLOCK_REALTIME, &ts);
diff --git a//home/user/Projects/cx-inline-comments/src/core/thread.h b//home/user/Projects/cx-inline-comments/src/core/thread.h
index 0fcfa17ba6e331309260b641ec21aa4a8ff9ba36..3cf72c1b96ca700f602e2e7c630a1e6970f9620f
--- a//home/user/Projects/cx-inline-comments/src/core/thread.h
+++ b//home/user/Projects/cx-inline-comments/src/core/thread.h
@@ -118,7 +118,7 @@
     ContextRoute,           // scatter/script/all-shard or conservative broad-owner route
     ContextKeymissNotify,   // MGET miss must retain owner-side notification behavior
     InflightWrite,
-    ArmTransient,           // pre-arming writes still in flight (DESIGN-RINGDIET.md)
+    ArmTransient,           // pre-arming writes still in flight
     AtomicPending,
     Missing,
     Typed,
@@ -151,7 +151,7 @@
     // ARM-ON-DEMAND TRANSIENT. Reads demoted because the connection carried writes published
     // BEFORE a local read armed it -- writes the RYOW ring deliberately never recorded. Bounded
     // per connection by one ROB drain and reported separately from the steady-state key conflict
-    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
+    // above so that the two can never be confused in a bench.
     uint64_t fallback_arm_transient = 0;
     uint64_t fallback_atomic_pending = 0;
     uint64_t fallback_missing = 0;
@@ -160,7 +160,7 @@
     uint64_t fallback_seq_churn = 0;
     uint64_t fallback_generation = 0;
     uint64_t fallback_lane_full = 0;
-    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
+    // Lane ADMISSION deferrals. Frames the armed parser left unconsumed at rpos, to be
     // re-parsed by a later pass of the same thread, because the local-read lane had no room
     // (defer_lane_full) or because the connection already held its fair share of a lane under
     // pressure (defer_quota). Neither is a fallback: the read still completes locally, and no

exec
/bin/bash -lc "git diff -- src/net src/store
sed -n '2968,2988p' src/core/server.h
sed -n '2265,2370p' src/core/ex_loop.h" in /home/user/Projects/cx-inline-comments
 succeeded in 0ms:
diff --git a/src/net/conn.h b/src/net/conn.h
index 7ace0c2dd..8eea2f6cd 100644
--- a/src/net/conn.h
+++ b/src/net/conn.h
@@ -83,9 +83,10 @@ struct Session {
 
 // WHO IS HOLDING THE PARSE BARRIER. Six independent mechanisms park a connection's parse pass, and
 // they used to share ONE bool -- so any one of them could clear a barrier another one still needed.
-// No reachable interleaving overlapped two owners (see NOTES-BARRIER.md section 2: a blocking op is
-// provably alone in its ROB, and every other owner ends the parse pass on the spot), which is
-// exactly why the bool survived: the hazard is one relaxed guard away, not present. Owner bits make
+// No reachable interleaving overlapped two owners: a blocking op waits for an empty ROB, then sets
+// the barrier and ends parsing, so it has neither older nor younger neighbours; every other owner
+// also ends the parse pass on the spot. That is exactly why the bool survived: the hazard is one
+// relaxed guard away, not present. Owner bits make
 // the release symmetric with the acquire -- whoever set it is the one whose release can drop it --
 // and cost nothing: the byte was already there, and "is any owner holding" is still one byte test.
 //
diff --git a/src/net/rob.h b/src/net/rob.h
index 808d62102..4fddc4733 100644
--- a/src/net/rob.h
+++ b/src/net/rob.h
@@ -51,7 +51,7 @@ namespace tomo {
 // its unreachability proof is written against. net/conn.h re-exports it by including this file.
 inline constexpr uint32_t kRobWindow = 64;   // max in-flight ops per connection
 
-// COLD-PATH COUNTERS FOR ARM-ON-DEMAND (DESIGN-RINGDIET.md). Three events, none of them on a hot
+// COLD-PATH COUNTERS FOR ARM-ON-DEMAND. Three events, none of them on a hot
 // path: a connection being armed, its RYOW sidecar being allocated, and one write being committed
 // into the ring. They are the instrument the design is proved with -- "pure SET does zero ring
 // bookkeeping" is exactly write_ring_records staying at zero -- so they are always on rather than
@@ -133,8 +133,8 @@ static_assert(ReadLocalRobState::kWriteRingCapacity <=
 // rounds to its 1280-byte class -- measured as +965 bytes of RSS per connection that owns one
 // (scratchpad/ringsize section 3) against the sixteen-slot sidecar's 296/320. It is NO LONGER paid
 // by every connection: arm-on-demand allocates it at the first write of an ARMED connection, so a
-// pure-write connection, a pure-read connection and an idle connection each carry none of it
-// (DESIGN-RINGDIET.md). Locked all the same, so that a later field cannot quietly add another size
+// pure-write connection, a pure-read connection and an idle connection each carry none of it.
+// Locked all the same, so that a later field cannot quietly add another size
 // class to every read/write connection without someone re-measuring that number.
 // The MGET latest-read fence used to live here and now lives in the Rob: it is a read-side fence
 // with nothing to do with the write ring, and keeping it here would have forced a pure-MGET
@@ -315,7 +315,7 @@ public:
     // until this id either completes locally or is irrevocably transferred to the owner path. It
     // lives on the Rob's own producer line rather than in the write-ring sidecar because it is a
     // READ-side fence -- a connection that only ever sends MGETs must not have to allocate 1216
-    // bytes of write ring to hold one id (DESIGN-RINGDIET.md).
+    // bytes of write ring to hold one id.
     void arm_current_local_mget_fence() {
         if (local_mget_fence_id_ != UINT64_MAX) std::abort();
         local_mget_fence_id_ = dispatch_id();
@@ -338,7 +338,7 @@ public:
 
     bool has_pending_read_local() const { return read_local_pending_slots_ != 0; }
     // Lane slots this connection holds right now: local reads published and not yet executed (or
-    // demoted). The parser's fair-share admission (P128.md) compares THIS, not in_flight(): a
+    // demoted). The parser's fair-share admission compares THIS, not in_flight(): a
     // reply that PHASE 2's bounded serve has not flushed yet holds no lane slot and must not
     // count against the connection's share (measured: it halved ops per rotation).
     uint32_t pending_read_local_count() const {
@@ -361,7 +361,7 @@ public:
     // Retires through the same clear-on-empty rule as the per-op form: when the drained chunk was
     // the whole pending set the summary restarts from zero (the contract stated in fcab80884; the
     // chunk form had dropped it, so the filter only ever grew in read-heavy streams and the first
-    // write of every batch paid the exact walk — AUDIT-MARKDIET.md section 4).
+    // write of every batch paid the exact walk).
     void complete_pending_read_local_mask(uint64_t bits) {
         if ((read_local_pending_slots_ & bits) != bits) std::abort();
         read_local_retire_pending_bit(bits);
@@ -471,7 +471,7 @@ public:
     // Every write starts conservative. After arity and routing are known, ordinary point writes
     // and bounded blind keysets may refine it; all other special/multi-key writes leave it broad.
     void mark_current_write() {
-        // ARM ON DEMAND, THE WRITE HALF (DESIGN-RINGDIET.md). Until a local read has armed this
+        // ARM ON DEMAND, THE WRITE HALF. Until a local read has armed this
         // connection the ring records NOTHING: no sidecar, no prune, no descriptor, no Staged tag
         // -- and so no resolve on the next frame either. The entire bookkeeping is one store of
         // this write's id, into a word on the producer's own cache line that dispatch_ has already
@@ -557,7 +557,7 @@ public:
     template <typename KeysetTouchesHash>
     __attribute__((always_inline)) bool read_local_write_conflicts(
             uint64_t hash, KeysetTouchesHash&& keyset_touches_hash) {
-        // ARM ON DEMAND, THE READ HALF (DESIGN-RINGDIET.md). ONE predictable test, on a word this
+        // ARM ON DEMAND, THE READ HALF. ONE predictable test, on a word this
         // frame's acquire_read_local has already pulled into L1 with dispatch_, and nothing is
         // evaluated behind it: a connection in steady state (armed, no arming generation left in
         // flight) reads a zero and falls straight through to the unchanged probe below. The word
@@ -687,7 +687,7 @@ public:
     }
 
     // The RYOW write ring's sidecar. Called from the connection's own IO thread on the first
-    // write of an ARMED connection -- never at accept (DESIGN-RINGDIET.md) -- and directly by the
+    // write of an ARMED connection -- never at accept -- and directly by the
     // unit test, which drives the ROB without a server. Returns false only on allocation failure;
     // every caller has a safe unarmed fallback for that.
     bool prepare_read_local() {
@@ -805,7 +805,7 @@ private:
     static constexpr uintptr_t kReadLocalStateTagBits =
         kReadLocalStateInactive | kReadLocalStateStaged;
 
-    // ARM-ON-DEMAND STATE (DESIGN-RINGDIET.md), one word on the producer line, zero in the state
+    // ARM-ON-DEMAND STATE, one word on the producer line, zero in the state
     // every connection ends up in. Two non-zero values, and a connection passes through each of
     // them at most once:
     //   Unarmed   no local read has been seen. Writes record nothing but their newest id.
diff --git a/src/store/flatstore.h b/src/store/flatstore.h
index faa413ec5..6681dbbeb 100644
--- a/src/store/flatstore.h
+++ b/src/store/flatstore.h
@@ -591,7 +591,9 @@ public:
 
     // Eight slot bytes at the 70% target load cost 11.43 bytes per live key. Accounting rounds
     // that stable-state estimate to 12; transient dual tables, tombstones and allocator metadata
-    // are deliberately outside the maxmemory model and documented in NOTES-EVICT.md.
+    // are deliberately outside the maxmemory model: charging those costs would make enforcement
+    // depend on resize history rather than maintained object/key counters. This is a stable
+    // logical cache budget, not an RSS ceiling.
     static constexpr size_t   kSlotOverheadPerKey = 12;
     static constexpr uint32_t kEvictionsPerOp = 16;
     static constexpr uint32_t kSampleProbeAttempts = 16;
diff --git a/src/store/flatstore_atomic.inc b/src/store/flatstore_atomic.inc
index 647b0850e..181f9e4f6 100644
--- a/src/store/flatstore_atomic.inc
+++ b/src/store/flatstore_atomic.inc
@@ -240,8 +240,9 @@
     // unit publishes through, not just one: an EXEC's plain per-key installs use the transaction's
     // epoch, while each cross-shard child command lowered inside that EXEC keeps its own
     // ScatterState and therefore its own. Answering with only the transaction's word makes a
-    // transaction hold against its own RENAME/LMPOP child and hang -- the same self-hold
-    // NOTES-MULTIRES.md §5(a) hit from the other direction. Plain single-command pseudo-entries
+    // transaction hold against its own RENAME/LMPOP child and hang. Testing group pointers instead
+    // also deadlocked: an EXEC child's scatter record names a group, while the same EXEC's plain
+    // installs do not. Plain single-command pseudo-entries
     // carry no group_epoch at all: they draw their ticket in the same owner turn that installs
     // them, so they are never undecided and are skipped here.
     template <typename IsOwnUnit>
@@ -1429,7 +1430,7 @@ private:
         return true;
     }
 
-    // ---- armed-write block cache (src/store/kv_block_cache.h, NOTES-RECYCLE.md) ---------------
+    // ---- armed-write block cache (src/store/kv_block_cache.h) ---------------
     //
     // The cache itself belongs to the fused OWNER and is reached through the retire sink this store
     // is already bound to; the store contributes only the eligibility a shard can judge (object
@@ -1782,7 +1783,7 @@ private:
             // waiting on other shards can be overtaken at the ticket counter by a MULTI/EXEC the
             // same connection sent afterwards. Ranking those two by raw ticket then re-installed
             // the OLDER group's value over the newer transaction's, destroying a write EXEC had
-            // already acknowledged -- reproduced at 246/1120 (NOTES-MULTIRES.md).
+            // already acknowledged -- reproduced at 246/1120.
             // Repairing the epoch to be monotone per connection removes the inversion without
             // reordering anything across connections, where the ticket remains the only truth.
             if (key.prev_set && owner.origin_conn_id == key.prev_conn_id &&
@@ -2171,7 +2172,7 @@ private:
             // waiting on other shards can be overtaken at the ticket counter by a MULTI/EXEC the
             // same connection sent afterwards. Ranking those two by raw ticket then re-installed
             // the OLDER group's value over the newer transaction's, destroying a write EXEC had
-            // already acknowledged -- reproduced at 246/1120 (NOTES-MULTIRES.md).
+            // already acknowledged -- reproduced at 246/1120.
             // Repairing the epoch to be monotone per connection removes the inversion without
             // reordering anything across connections, where the ticket remains the only truth.
             if (key.prev_set && owner.origin_conn_id == key.prev_conn_id &&
diff --git a/src/store/kv_block_cache.h b/src/store/kv_block_cache.h
index b3da12827..63c8dc0a3 100644
--- a/src/store/kv_block_cache.h
+++ b/src/store/kv_block_cache.h
@@ -67,8 +67,9 @@ struct KvBlockCache {
     //
     //  * kMaxNodesPerClass — one grace drain's worth. A drain releases at most one retire ring of
     //    objects, so a class that refuses blocks below that number is refusing memory the very
-    //    next owner pass will ask for again. Measured: with a 32-node cap the reuse rate collapses
-    //    to a few percent and the whole mechanism stops paying (see NOTES-RECYCLE.md).
+    //    next owner pass will ask for again. Measured on one fused owner at 64 shards, a 32-node
+    //    cap left 0.5001/0.3750 mallocx calls per op at pipeline 64/256; the 4096-node cap left
+    //    zero in both cells for only 2-4 KiB more cached memory.
     //  * kMaxBytes — one grace drain of maximum-inline-value objects. The retire ring ALREADY
     //    permits kReadLocalRetireRingCapacity retired-but-unreclaimed objects per owner to be
     //    resident, and kEmbedThreshold is the largest value an eligible block can carry, so this
diff --git a/src/store/read_local_settax.h b/src/store/read_local_settax.h
index 4e9546df3..94835c262 100644
--- a/src/store/read_local_settax.h
+++ b/src/store/read_local_settax.h
@@ -8,7 +8,7 @@
 //
 // Selector 2 (post-QSBR same-class recycling) is RETIRED: it won its bake-off and was hardcoded.
 // Armed writes now draw their block from, and return it to, the shard's own post-grace block cache
-// unconditionally (FlatStore::read_local_cache_take/put, NOTES-RECYCLE.md), so there is no longer a
+// unconditionally (FlatStore::read_local_cache_take/put), so there is no longer a
 // build that recycles and a build that does not. Selecting 2 is a compile error rather than a
 // silent second recycler competing with the shipped one.
 #pragma once
        debug_blocking_timeout_reaps_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    uint64_t debug_blocking_timeout_reaps() const {
        return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
    }
    // An overlap: some owner took the parse barrier while another owner already held it. With
    // DEBUG BARRIER-HOLD off this must read ZERO -- if it ever moves on production traffic the
    // latent case just went live and the owner-scoped release in blocking_retire() became
    // load-bearing rather than defensive. With the latch ON it advances once per blocking dispatch,
    // which is the counter's own positive control: a "must be zero" reading proves nothing until
    // something has been shown
    // able to make it non-zero.
    void note_barrier_overlap() {
        barrier_owner_overlaps_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t barrier_owner_overlaps() const {
        return barrier_owner_overlaps_.load(std::memory_order_relaxed);
    }
    // A blocking retirement released its own claim and the barrier STAYED UP because another owner
    // still held it. This is the instruction the old unconditional clear got wrong, so a validation
        if (task.scatter) {
            if (!execute_snapshot_task<IofusedPrivateQueue>(task, true))
                xshard_retries_.push_back(task);
            return;
        }
        const Op& op = task.client->rob().at(task.op_id);
        const int32_t sid = task.shard >= 0 ? task.shard : op.shard;
        auto& queue = snapshot_backlogs_[static_cast<uint32_t>(sid)];
        if (!queue.empty() || !execute_snapshot_task<IofusedPrivateQueue>(task, true))
            queue.push_back(task);
    }

    template <bool IofusedPrivateQueue = false>
    uint32_t service_snapshot_backlogs(uint32_t budget, bool capture_writes = true) {
        uint32_t n = 0;
        for (Shard* shard : self_->shards()) {
            auto& queue = snapshot_backlogs_[static_cast<uint32_t>(shard->id())];
            while (budget && !queue.empty()) {
                if (!execute_snapshot_task<IofusedPrivateQueue>(
                        queue.front(), capture_writes)) break;
                queue.pop_front(); budget--; n++;
            }
            if (!budget) break;
        }
        return n;
    }

    template <uint32_t BatchOps = kGenthreadExBatchOps,
              bool IofusedPrivateQueue = false>
    uint32_t drain_tasks_snapshot(bool unmasked = false) {
        auto take = [&](const Task& task) {
            schedule_snapshot_task<IofusedPrivateQueue>(task);
        };
        const uint32_t n = unmasked
            ? self_->drain_tasks_unmasked<IofusedPrivateQueue>(take)
            : self_->drain_tasks<IofusedPrivateQueue>(take);
        self_->sig().ops += n;
        // Retire at least as many deferred Tasks as this drain can add, plus one batch of old debt;
        // otherwise a saturated post-capture owner could remain in Draining forever.
        return n + service_snapshot_backlogs<IofusedPrivateQueue>(BatchOps + n);
    }

    // The armed slow-log arm. Out of line and cold-marked so its register pressure and its clock
    // calls never reach the disabled batch loop above.
    //
    // TWO MODES. Normal mode brackets the whole batch with two now_ns() reads. A batch of ONE op
    // gives that op's exact duration, which is every command on an unpipelined connection. A batch
    // of many that overruns cannot be attributed retroactively -- the ops already ran -- so it
    // arms per-op timing for the next kSlowlogEscalateBatches batches instead, and the recurrence
    // is timed exactly. This is the documented divergence from redis, which times every command.
    //
    template <bool IofusedPrivateQueue = false>
    __attribute__((noinline, cold))
    void exec_batch_timed(const Task* batch, uint32_t n) {
        Server::ClientWorkScope client_work(*srv_, self_->id());
        const SlowlogArm arm = slowlog_arm_;
        const int64_t now_ms = cached_now_ms_;
        slowlog_note_batch_timed();
        // Earlier service queues in this outer pass are not part of this batch's timing sample.
        flush_xshard_commits();

        if (slowlog_state_.escalate_batches || n == 1) {
            if (slowlog_state_.escalate_batches) slowlog_state_.escalate_batches--;
            for (uint32_t i = 0; i < n; i++) {
                // Snapshot argv BEFORE execution. execute() publishes Done, after which the owning
                // IO thread may retire the op and compact the read buffer the Slices point into.
                Client* client = batch[i].client;
                if (client)
                    slowlog_capture(client->rob().at(batch[i].op_id), slowlog_state_.capture);
                const uint64_t started = now_ns();
                const bool ok = execute<IofusedPrivateQueue>(batch[i]);
                // Done is deliberately delayed with the commit. Armed per-op timing must include
                // that commit and publish it before the Done-based one-command attribution test.
                flush_xshard_commits();
                const uint64_t elapsed = now_ns() - started;
                // ONE ENTRY PER COMMAND, not per participating shard. A cross-shard op is handed
                // to every owner it touches; all but the last return with the op still Issued.
                // Recording only the owner that published Done means a scatter is attributed to
                // the slice that actually computed the answer.
                if (client &&
                    client->rob().at(batch[i].op_id).state.load(std::memory_order_relaxed) ==
                        OpState::Done)
                    slowlog_record_captured(self_->id(), client->id(), slowlog_state_.capture,
                                            elapsed, now_ms, arm);
                if (ok) continue;
                xshard_retries_.push_back(batch[i]);
                for (uint32_t j = i + 1; j < n; j++) ordered_deferred_.push_back(batch[j]);
                return;
            }
            return;
        }

        const uint64_t started = now_ns();
        uint32_t executed = n;
        for (uint32_t i = 0; i < n; i++) {
            if (execute<IofusedPrivateQueue>(batch[i])) continue;
            xshard_retries_.push_back(batch[i]);
            for (uint32_t j = i + 1; j < n; j++) ordered_deferred_.push_back(batch[j]);
            executed = i;
            break;
        }
        flush_xshard_commits();
        const uint64_t elapsed = now_ns() - started;
        // The screen is per-op-average, not whole-batch: a full batch of ordinary commands must
        // not look like one slow command just because there were 32 of them.
        const uint64_t threshold_ns = arm.slowlog_us >= 0

diff --git a//home/user/Projects/cx-inline-comments/src/core/config.h b//home/user/Projects/cx-inline-comments/src/core/config.h
index 495e6ae42da15ee4bcc76a8a846efeef506903d7..8b5f3e5c786e8eeae0296a5dcf19338056d59007
--- a//home/user/Projects/cx-inline-comments/src/core/config.h
+++ b//home/user/Projects/cx-inline-comments/src/core/config.h
@@ -290,8 +290,8 @@
     uint32_t client_lb = 1;
 
     // ---- automatic role split (boot-latched) -----------------------------------------------
-    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer
-    // (DESIGN-flipfp.md); its measured sampling policy is internal to flipctl.h. Off and 1s
+    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer;
+    // its measured sampling policy is internal to flipctl.h. Off and 1s
     // dispatch pay one predicted branch per op and no fingerprint store. Trigger bands learn
     // from the anchor's own quiet jitter; no separate controller tuning knobs remain.
     uint32_t flip_auto = 0;
diff --git a//home/user/Projects/cx-inline-comments/src/core/flipctl.h b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
index 5185da4085ae5f2e3325702064208810cb9c42f7..1edc3a93889090da659db0c9ee259d04ce5cef57
--- a//home/user/Projects/cx-inline-comments/src/core/flipctl.h
+++ b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
@@ -19,7 +19,9 @@
 
 class Server;
 
-// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
+// Preserve the measured sampler: one whole parse pass in 100 on average. At a matched 512k ops/s,
+// a GET-only -> SET-only swap triggered the fingerprint detector once with both exhaustive and
+// 1-in-100 sampling; the sampled writer still detected the shift with the rate held fixed.
 // Arming derives solely from the controller latch. No runtime sampling knob or adaptive
 // per-operation work is needed; the detector already derives its bands from actual samples.
 inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
@@ -52,7 +54,9 @@
     uint64_t closed_windows = 0;
 };
 
-// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
+// SAMPLED BY PARSE PASS. A per-op countdown would restore the store this sampler removes: no
+// existing io-side command counter covers both the point paths and all io-local commands.
+// The io loop asks pass_sampled() once per dispatched
 // frame -- one load of this writer's own word, on the line the old enabled() test already read,
 // and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
 // for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
diff --git a//home/user/Projects/cx-inline-comments/src/core/server.h b//home/user/Projects/cx-inline-comments/src/core/server.h
index f869dd3d1737afa6b1c401919fb66c96772b1225..8f279299796e7dabffe155115dea7b3ff379627b
--- a//home/user/Projects/cx-inline-comments/src/core/server.h
+++ b//home/user/Projects/cx-inline-comments/src/core/server.h
@@ -8,8 +8,10 @@
 //
 // THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
 // dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
-// its derived shard-granularity fast path, published only at a quiesced commit. See
-// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
+// its derived shard-granularity fast path, published only at a quiesced commit. Shards move only
+// after executing, deferred, group and pinned-read work drains; destination routes publish after
+// the ownership edge. An IO may enqueue using an old route after that edge, so tasks and borrow
+// releases recheck ownership and forward before shard access instead of carrying routing epochs.
 #pragma once
 #include <algorithm>
 #include <atomic>
@@ -308,7 +310,7 @@
         for (uint32_t i = 0; i < nthreads; i++) {
             threads_[i] = std::make_unique<ThreadCtx>();
             // The fingerprint writer is armed only when its one reader, the flip controller, is
-            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
+            // enabled: with --flip-auto 0 and in 1s mode it is dark and costs
             // one predicted branch per op. The enabled sampler keeps its measured 1-in-100 policy.
             threads_[i]->init(i, placement_.role_of(i), nthreads,
                               0,
@@ -1969,7 +1971,7 @@
         shard_owner_[shard_id].store(thread_id, std::memory_order_release);
     }
 
-    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
+    // The caller must hold both executor loops at the safe point:
     // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
     // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
     // Vector capacity and membership are settled while PREPARING still names the source.  The
@@ -1988,7 +1990,7 @@
     // lost, a block ends up linked twice, and the damage surfaces later as either
     // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
     // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
-    // Both were observed (see DESIGN-P0REPLY.md).
+    // Both were observed.
     //
     // The rebind used to be deferred to the destination's own next executor pass
     // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
@@ -2827,7 +2829,7 @@
     // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
     // other command. It widens -- deterministically -- the window in which a younger whole-owner
     // walker could overtake an older same-connection group on the destination shard.
-    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
+    // TEST HOOK: effective local-read LANE CAPACITY for admission. 0 means
     // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
     // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
     // its masking, so this can never overrun anything. It exists because lane oversubscription is
@@ -2935,9 +2937,11 @@
     // is already set. Nothing on GET/SET touches it.
     //
     // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
-    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
-    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
-    // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
+    // sequence: a blocking op requires an empty ROB, then sets the barrier before any younger
+    // frame can be parsed; a resumed scatter inherits that same slot and barrier. The state the
+    // owner-scoped release exists to survive -- a release that must NOT drop the barrier -- has
+    // to be injected. It is held past ROB quiescence on purpose; barrier_release_quiesced()
+    // exempts this one bit for that reason.
     //
     // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
     // nothing left to do and parks on submit_and_wait(1) -- unbounded under io_uring. A hold that
@@ -2968,11 +2972,11 @@
         return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
     }
     // An overlap: some owner took the parse barrier while another owner already held it. With
-    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
-    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
-    // live and the owner-scoped release in blocking_retire() became load-bearing rather than
-    // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
-    // own positive control: a "must be zero" reading proves nothing until something has been shown
+    // DEBUG BARRIER-HOLD off this must read ZERO -- if it ever moves on production traffic the
+    // latent case just went live and the owner-scoped release in blocking_retire() became
+    // load-bearing rather than defensive. With the latch ON it advances once per blocking dispatch,
+    // which is the counter's own positive control: a "must be zero" reading proves nothing until
+    // something has been shown
     // able to make it non-zero.
     void note_barrier_overlap() {
         barrier_owner_overlaps_.fetch_add(1, std::memory_order_relaxed);
diff --git a//home/user/Projects/cx-inline-comments/src/core/shard.h b//home/user/Projects/cx-inline-comments/src/core/shard.h
index 694b9a93c6cd63db49b9dcf061dbb5182c49986a..39a049d2f8619bbf52f706775956cc9198060364
--- a//home/user/Projects/cx-inline-comments/src/core/shard.h
+++ b//home/user/Projects/cx-inline-comments/src/core/shard.h
@@ -431,7 +431,7 @@
         // NOT share this counter: multi.inc's prepare_write_key() re-asks it at install time and
         // reports violations through INFO atomic_exec_order_late (multi_exec_order_late()), which
         // must read zero. Summing the two would hide a violation inside a number that is non-zero
-        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
+        // by design, leaving the park's safety argument with no falsifier.
         uint64_t atomic_exec_order_holds = 0;
         // Times watch_finalize_reservation() answered "not ready" because the reservation's epoch
         // was still 0, i.e. a unit was turned into a Retry by an undecided WATCH reservation. It
diff --git a//home/user/Projects/cx-inline-comments/src/core/signal.h b//home/user/Projects/cx-inline-comments/src/core/signal.h
index 933a5d7ee09538f3923df3c5eb77f9c72cdc0bfa..f69f7d73942ee129f4ad9cd1f23befe69fc4167d
--- a//home/user/Projects/cx-inline-comments/src/core/signal.h
+++ b//home/user/Projects/cx-inline-comments/src/core/signal.h
@@ -47,7 +47,7 @@
 // that will be compared against a deadline must come from here and never from now_ns() above --
 // that one is CLOCK_MONOTONIC, so its "milliseconds" are milliseconds since boot and sit roughly
 // five orders of magnitude below any real deadline. A monotonic value used as an expiry cut does
-// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
+// not skew the answer, it disables expiry outright.
 inline int64_t now_realtime_ms() {
     timespec ts;
     clock_gettime(CLOCK_REALTIME, &ts);
diff --git a//home/user/Projects/cx-inline-comments/src/core/thread.h b//home/user/Projects/cx-inline-comments/src/core/thread.h
index 0fcfa17ba6e331309260b641ec21aa4a8ff9ba36..3cf72c1b96ca700f602e2e7c630a1e6970f9620f
--- a//home/user/Projects/cx-inline-comments/src/core/thread.h
+++ b//home/user/Projects/cx-inline-comments/src/core/thread.h
@@ -118,7 +118,7 @@
     ContextRoute,           // scatter/script/all-shard or conservative broad-owner route
     ContextKeymissNotify,   // MGET miss must retain owner-side notification behavior
     InflightWrite,
-    ArmTransient,           // pre-arming writes still in flight (DESIGN-RINGDIET.md)
+    ArmTransient,           // pre-arming writes still in flight
     AtomicPending,
     Missing,
     Typed,
@@ -151,7 +151,7 @@
     // ARM-ON-DEMAND TRANSIENT. Reads demoted because the connection carried writes published
     // BEFORE a local read armed it -- writes the RYOW ring deliberately never recorded. Bounded
     // per connection by one ROB drain and reported separately from the steady-state key conflict
-    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
+    // above so that the two can never be confused in a bench.
     uint64_t fallback_arm_transient = 0;
     uint64_t fallback_atomic_pending = 0;
     uint64_t fallback_missing = 0;
@@ -160,7 +160,7 @@
     uint64_t fallback_seq_churn = 0;
     uint64_t fallback_generation = 0;
     uint64_t fallback_lane_full = 0;
-    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
+    // Lane ADMISSION deferrals. Frames the armed parser left unconsumed at rpos, to be
     // re-parsed by a later pass of the same thread, because the local-read lane had no room
     // (defer_lane_full) or because the connection already held its fair share of a lane under
     // pressure (defer_quota). Neither is a fallback: the read still completes locally, and no

diff --git a//home/user/Projects/cx-inline-comments/src/core/config.h b//home/user/Projects/cx-inline-comments/src/core/config.h
index 495e6ae42da15ee4bcc76a8a846efeef506903d7..8b5f3e5c786e8eeae0296a5dcf19338056d59007
--- a//home/user/Projects/cx-inline-comments/src/core/config.h
+++ b//home/user/Projects/cx-inline-comments/src/core/config.h
@@ -290,8 +290,8 @@
     uint32_t client_lb = 1;
 
     // ---- automatic role split (boot-latched) -----------------------------------------------
-    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer
-    // (DESIGN-flipfp.md); its measured sampling policy is internal to flipctl.h. Off and 1s
+    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer;
+    // its measured sampling policy is internal to flipctl.h. Off and 1s
     // dispatch pay one predicted branch per op and no fingerprint store. Trigger bands learn
     // from the anchor's own quiet jitter; no separate controller tuning knobs remain.
     uint32_t flip_auto = 0;
diff --git a//home/user/Projects/cx-inline-comments/src/core/flipctl.h b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
index 5185da4085ae5f2e3325702064208810cb9c42f7..1edc3a93889090da659db0c9ee259d04ce5cef57
--- a//home/user/Projects/cx-inline-comments/src/core/flipctl.h
+++ b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
@@ -19,7 +19,9 @@
 
 class Server;
 
-// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
+// Preserve the measured sampler: one whole parse pass in 100 on average. At a matched 512k ops/s,
+// a GET-only -> SET-only swap triggered the fingerprint detector once with both exhaustive and
+// 1-in-100 sampling; the sampled writer still detected the shift with the rate held fixed.
 // Arming derives solely from the controller latch. No runtime sampling knob or adaptive
 // per-operation work is needed; the detector already derives its bands from actual samples.
 inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
@@ -52,7 +54,9 @@
     uint64_t closed_windows = 0;
 };
 
-// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
+// SAMPLED BY PARSE PASS. A per-op countdown would restore the store this sampler removes: no
+// existing io-side command counter covers both the point paths and all io-local commands.
+// The io loop asks pass_sampled() once per dispatched
 // frame -- one load of this writer's own word, on the line the old enabled() test already read,
 // and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
 // for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
diff --git a//home/user/Projects/cx-inline-comments/src/core/server.h b//home/user/Projects/cx-inline-comments/src/core/server.h
index f869dd3d1737afa6b1c401919fb66c96772b1225..8f279299796e7dabffe155115dea7b3ff379627b
--- a//home/user/Projects/cx-inline-comments/src/core/server.h
+++ b//home/user/Projects/cx-inline-comments/src/core/server.h
@@ -8,8 +8,10 @@
 //
 // THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
 // dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
-// its derived shard-granularity fast path, published only at a quiesced commit. See
-// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
+// its derived shard-granularity fast path, published only at a quiesced commit. Shards move only
+// after executing, deferred, group and pinned-read work drains; destination routes publish after
+// the ownership edge. An IO may enqueue using an old route after that edge, so tasks and borrow
+// releases recheck ownership and forward before shard access instead of carrying routing epochs.
 #pragma once
 #include <algorithm>
 #include <atomic>
@@ -308,7 +310,7 @@
         for (uint32_t i = 0; i < nthreads; i++) {
             threads_[i] = std::make_unique<ThreadCtx>();
             // The fingerprint writer is armed only when its one reader, the flip controller, is
-            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
+            // enabled: with --flip-auto 0 and in 1s mode it is dark and costs
             // one predicted branch per op. The enabled sampler keeps its measured 1-in-100 policy.
             threads_[i]->init(i, placement_.role_of(i), nthreads,
                               0,
@@ -1969,7 +1971,7 @@
         shard_owner_[shard_id].store(thread_id, std::memory_order_release);
     }
 
-    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
+    // The caller must hold both executor loops at the safe point:
     // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
     // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
     // Vector capacity and membership are settled while PREPARING still names the source.  The
@@ -1988,7 +1990,7 @@
     // lost, a block ends up linked twice, and the damage surfaces later as either
     // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
     // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
-    // Both were observed (see DESIGN-P0REPLY.md).
+    // Both were observed.
     //
     // The rebind used to be deferred to the destination's own next executor pass
     // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
@@ -2827,7 +2829,7 @@
     // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
     // other command. It widens -- deterministically -- the window in which a younger whole-owner
     // walker could overtake an older same-connection group on the destination shard.
-    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
+    // TEST HOOK: effective local-read LANE CAPACITY for admission. 0 means
     // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
     // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
     // its masking, so this can never overrun anything. It exists because lane oversubscription is
@@ -2935,9 +2937,11 @@
     // is already set. Nothing on GET/SET touches it.
     //
     // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
-    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
-    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
-    // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
+    // sequence: a blocking op requires an empty ROB, then sets the barrier before any younger
+    // frame can be parsed; a resumed scatter inherits that same slot and barrier. The state the
+    // owner-scoped release exists to survive -- a release that must NOT drop the barrier -- has
+    // to be injected. It is held past ROB quiescence on purpose; barrier_release_quiesced()
+    // exempts this one bit for that reason.
     //
     // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
     // nothing left to do and parks on submit_and_wait(1) -- unbounded under io_uring. A hold that
@@ -2968,11 +2972,11 @@
         return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
     }
     // An overlap: some owner took the parse barrier while another owner already held it. With
-    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
-    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
-    // live and the owner-scoped release in blocking_retire() became load-bearing rather than
-    // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
-    // own positive control: a "must be zero" reading proves nothing until something has been shown
+    // DEBUG BARRIER-HOLD off this must read ZERO -- if it ever moves on production traffic the
+    // latent case just went live and the owner-scoped release in blocking_retire() became
+    // load-bearing rather than defensive. With the latch ON it advances once per blocking dispatch,
+    // which is the counter's own positive control: a "must be zero" reading proves nothing until
+    // something has been shown
     // able to make it non-zero.
     void note_barrier_overlap() {
         barrier_owner_overlaps_.fetch_add(1, std::memory_order_relaxed);
diff --git a//home/user/Projects/cx-inline-comments/src/core/shard.h b//home/user/Projects/cx-inline-comments/src/core/shard.h
index 694b9a93c6cd63db49b9dcf061dbb5182c49986a..39a049d2f8619bbf52f706775956cc9198060364
--- a//home/user/Projects/cx-inline-comments/src/core/shard.h
+++ b//home/user/Projects/cx-inline-comments/src/core/shard.h
@@ -431,7 +431,7 @@
         // NOT share this counter: multi.inc's prepare_write_key() re-asks it at install time and
         // reports violations through INFO atomic_exec_order_late (multi_exec_order_late()), which
         // must read zero. Summing the two would hide a violation inside a number that is non-zero
-        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
+        // by design, leaving the park's safety argument with no falsifier.
         uint64_t atomic_exec_order_holds = 0;
         // Times watch_finalize_reservation() answered "not ready" because the reservation's epoch
         // was still 0, i.e. a unit was turned into a Retry by an undecided WATCH reservation. It
diff --git a//home/user/Projects/cx-inline-comments/src/core/signal.h b//home/user/Projects/cx-inline-comments/src/core/signal.h
index 933a5d7ee09538f3923df3c5eb77f9c72cdc0bfa..f69f7d73942ee129f4ad9cd1f23befe69fc4167d
--- a//home/user/Projects/cx-inline-comments/src/core/signal.h
+++ b//home/user/Projects/cx-inline-comments/src/core/signal.h
@@ -47,7 +47,7 @@
 // that will be compared against a deadline must come from here and never from now_ns() above --
 // that one is CLOCK_MONOTONIC, so its "milliseconds" are milliseconds since boot and sit roughly
 // five orders of magnitude below any real deadline. A monotonic value used as an expiry cut does
-// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
+// not skew the answer, it disables expiry outright.
 inline int64_t now_realtime_ms() {
     timespec ts;
     clock_gettime(CLOCK_REALTIME, &ts);
diff --git a//home/user/Projects/cx-inline-comments/src/core/thread.h b//home/user/Projects/cx-inline-comments/src/core/thread.h
index 0fcfa17ba6e331309260b641ec21aa4a8ff9ba36..3cf72c1b96ca700f602e2e7c630a1e6970f9620f
--- a//home/user/Projects/cx-inline-comments/src/core/thread.h
+++ b//home/user/Projects/cx-inline-comments/src/core/thread.h
@@ -118,7 +118,7 @@
     ContextRoute,           // scatter/script/all-shard or conservative broad-owner route
     ContextKeymissNotify,   // MGET miss must retain owner-side notification behavior
     InflightWrite,
-    ArmTransient,           // pre-arming writes still in flight (DESIGN-RINGDIET.md)
+    ArmTransient,           // pre-arming writes still in flight
     AtomicPending,
     Missing,
     Typed,
@@ -151,7 +151,7 @@
     // ARM-ON-DEMAND TRANSIENT. Reads demoted because the connection carried writes published
     // BEFORE a local read armed it -- writes the RYOW ring deliberately never recorded. Bounded
     // per connection by one ROB drain and reported separately from the steady-state key conflict
-    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
+    // above so that the two can never be confused in a bench.
     uint64_t fallback_arm_transient = 0;
     uint64_t fallback_atomic_pending = 0;
     uint64_t fallback_missing = 0;
@@ -160,7 +160,7 @@
     uint64_t fallback_seq_churn = 0;
     uint64_t fallback_generation = 0;
     uint64_t fallback_lane_full = 0;
-    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
+    // Lane ADMISSION deferrals. Frames the armed parser left unconsumed at rpos, to be
     // re-parsed by a later pass of the same thread, because the local-read lane had no room
     // (defer_lane_full) or because the connection already held its fair share of a lane under
     // pressure (defer_quota). Neither is a fallback: the read still completes locally, and no

apply patch
patch: completed
/home/user/Projects/cx-inline-comments/src/core/flipctl.h
/home/user/Projects/cx-inline-comments/src/core/server.h
diff --git a//home/user/Projects/cx-inline-comments/src/core/config.h b//home/user/Projects/cx-inline-comments/src/core/config.h
index 495e6ae42da15ee4bcc76a8a846efeef506903d7..8b5f3e5c786e8eeae0296a5dcf19338056d59007
--- a//home/user/Projects/cx-inline-comments/src/core/config.h
+++ b//home/user/Projects/cx-inline-comments/src/core/config.h
@@ -290,8 +290,8 @@
     uint32_t client_lb = 1;
 
     // ---- automatic role split (boot-latched) -----------------------------------------------
-    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer
-    // (DESIGN-flipfp.md); its measured sampling policy is internal to flipctl.h. Off and 1s
+    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer;
+    // its measured sampling policy is internal to flipctl.h. Off and 1s
     // dispatch pay one predicted branch per op and no fingerprint store. Trigger bands learn
     // from the anchor's own quiet jitter; no separate controller tuning knobs remain.
     uint32_t flip_auto = 0;
diff --git a//home/user/Projects/cx-inline-comments/src/core/flipctl.h b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
index 5185da4085ae5f2e3325702064208810cb9c42f7..00b11f3e799ae7313e3ce48ae074715e0bbb7968
--- a//home/user/Projects/cx-inline-comments/src/core/flipctl.h
+++ b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
@@ -19,7 +19,9 @@
 
 class Server;
 
-// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
+// Preserve the measured sampler: one whole parse pass in 100 on average. At a matched 512k ops/s,
+// a GET-only -> SET-only swap triggered the fingerprint detector once with both exhaustive and
+// 1-in-100 sampling; the sampled writer still detected the shift with the rate held fixed.
 // Arming derives solely from the controller latch. No runtime sampling knob or adaptive
 // per-operation work is needed; the detector already derives its bands from actual samples.
 inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
@@ -52,9 +54,11 @@
     uint64_t closed_windows = 0;
 };
 
-// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
-// frame -- one load of this writer's own word, on the line the old enabled() test already read,
-// and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
+// SAMPLED BY PARSE PASS. A per-op countdown would restore the store this sampler removes: no
+// existing io-side command counter covers both the point paths and all io-local commands.
+// The io loop asks pass_sampled() once per dispatched frame -- one load of this writer's own word,
+// on the line the old enabled() test already read, and one predicted-false branch; no store.
+// Inside a sampled pass the full fingerprint body runs
 // for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
 // that pass; at the pass end finish_parse_pass() publishes it and the caller draws the gap to the
 // next sampled pass. `work_window_` is the mean commands per fingerprint
diff --git a//home/user/Projects/cx-inline-comments/src/core/server.h b//home/user/Projects/cx-inline-comments/src/core/server.h
index f869dd3d1737afa6b1c401919fb66c96772b1225..a168d1656a19c4b098167904d42f814caae7513c
--- a//home/user/Projects/cx-inline-comments/src/core/server.h
+++ b//home/user/Projects/cx-inline-comments/src/core/server.h
@@ -8,8 +8,10 @@
 //
 // THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
 // dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
-// its derived shard-granularity fast path, published only at a quiesced commit. See
-// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
+// its derived shard-granularity fast path, published only at a quiesced commit. Shards move only
+// after executing, deferred, group and pinned-read work drains; destination routes publish after
+// the ownership edge. An IO may enqueue using an old route after that edge, so tasks and borrow
+// releases recheck ownership and forward before shard access instead of carrying routing epochs.
 #pragma once
 #include <algorithm>
 #include <atomic>
@@ -308,7 +310,7 @@
         for (uint32_t i = 0; i < nthreads; i++) {
             threads_[i] = std::make_unique<ThreadCtx>();
             // The fingerprint writer is armed only when its one reader, the flip controller, is
-            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
+            // enabled: with --flip-auto 0 and in 1s mode it is dark and costs
             // one predicted branch per op. The enabled sampler keeps its measured 1-in-100 policy.
             threads_[i]->init(i, placement_.role_of(i), nthreads,
                               0,
@@ -1969,7 +1971,7 @@
         shard_owner_[shard_id].store(thread_id, std::memory_order_release);
     }
 
-    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
+    // The caller must hold both executor loops at the safe point:
     // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
     // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
     // Vector capacity and membership are settled while PREPARING still names the source.  The
@@ -1988,7 +1990,7 @@
     // lost, a block ends up linked twice, and the damage surfaces later as either
     // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
     // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
-    // Both were observed (see DESIGN-P0REPLY.md).
+    // Both were observed.
     //
     // The rebind used to be deferred to the destination's own next executor pass
     // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
@@ -2827,7 +2829,7 @@
     // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
     // other command. It widens -- deterministically -- the window in which a younger whole-owner
     // walker could overtake an older same-connection group on the destination shard.
-    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
+    // TEST HOOK: effective local-read LANE CAPACITY for admission. 0 means
     // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
     // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
     // its masking, so this can never overrun anything. It exists because lane oversubscription is
@@ -2935,9 +2937,11 @@
     // is already set. Nothing on GET/SET touches it.
     //
     // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
-    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
-    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
-    // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
+    // sequence: a blocking op requires an empty ROB, then sets the barrier before any younger
+    // frame can be parsed; a resumed scatter inherits that same slot and barrier. The state the
+    // owner-scoped release exists to survive -- a release that must NOT drop the barrier -- has
+    // to be injected. It is held past ROB quiescence on purpose; barrier_release_quiesced()
+    // exempts this one bit for that reason.
     //
     // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
     // nothing left to do and parks on submit_and_wait(1) -- unbounded under io_uring. A hold that
@@ -2968,12 +2972,11 @@
         return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
     }
     // An overlap: some owner took the parse barrier while another owner already held it. With
-    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
-    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
-    // live and the owner-scoped release in blocking_retire() became load-bearing rather than
-    // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
-    // own positive control: a "must be zero" reading proves nothing until something has been shown
-    // able to make it non-zero.
+    // DEBUG BARRIER-HOLD off this must read ZERO -- if it ever moves on production traffic the
+    // latent case just went live and the owner-scoped release in blocking_retire() became
+    // load-bearing rather than defensive. With the latch ON it advances once per blocking dispatch,
+    // which is the counter's own positive control: a "must be zero" reading proves nothing until
+    // something has been shown able to make it non-zero.
     void note_barrier_overlap() {
         barrier_owner_overlaps_.fetch_add(1, std::memory_order_relaxed);
     }
diff --git a//home/user/Projects/cx-inline-comments/src/core/shard.h b//home/user/Projects/cx-inline-comments/src/core/shard.h
index 694b9a93c6cd63db49b9dcf061dbb5182c49986a..39a049d2f8619bbf52f706775956cc9198060364
--- a//home/user/Projects/cx-inline-comments/src/core/shard.h
+++ b//home/user/Projects/cx-inline-comments/src/core/shard.h
@@ -431,7 +431,7 @@
         // NOT share this counter: multi.inc's prepare_write_key() re-asks it at install time and
         // reports violations through INFO atomic_exec_order_late (multi_exec_order_late()), which
         // must read zero. Summing the two would hide a violation inside a number that is non-zero
-        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
+        // by design, leaving the park's safety argument with no falsifier.
         uint64_t atomic_exec_order_holds = 0;
         // Times watch_finalize_reservation() answered "not ready" because the reservation's epoch
         // was still 0, i.e. a unit was turned into a Retry by an undecided WATCH reservation. It
diff --git a//home/user/Projects/cx-inline-comments/src/core/signal.h b//home/user/Projects/cx-inline-comments/src/core/signal.h
index 933a5d7ee09538f3923df3c5eb77f9c72cdc0bfa..f69f7d73942ee129f4ad9cd1f23befe69fc4167d
--- a//home/user/Projects/cx-inline-comments/src/core/signal.h
+++ b//home/user/Projects/cx-inline-comments/src/core/signal.h
@@ -47,7 +47,7 @@
 // that will be compared against a deadline must come from here and never from now_ns() above --
 // that one is CLOCK_MONOTONIC, so its "milliseconds" are milliseconds since boot and sit roughly
 // five orders of magnitude below any real deadline. A monotonic value used as an expiry cut does
-// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
+// not skew the answer, it disables expiry outright.
 inline int64_t now_realtime_ms() {
     timespec ts;
     clock_gettime(CLOCK_REALTIME, &ts);
diff --git a//home/user/Projects/cx-inline-comments/src/core/thread.h b//home/user/Projects/cx-inline-comments/src/core/thread.h
index 0fcfa17ba6e331309260b641ec21aa4a8ff9ba36..3cf72c1b96ca700f602e2e7c630a1e6970f9620f
--- a//home/user/Projects/cx-inline-comments/src/core/thread.h
+++ b//home/user/Projects/cx-inline-comments/src/core/thread.h
@@ -118,7 +118,7 @@
     ContextRoute,           // scatter/script/all-shard or conservative broad-owner route
     ContextKeymissNotify,   // MGET miss must retain owner-side notification behavior
     InflightWrite,
-    ArmTransient,           // pre-arming writes still in flight (DESIGN-RINGDIET.md)
+    ArmTransient,           // pre-arming writes still in flight
     AtomicPending,
     Missing,
     Typed,
@@ -151,7 +151,7 @@
     // ARM-ON-DEMAND TRANSIENT. Reads demoted because the connection carried writes published
     // BEFORE a local read armed it -- writes the RYOW ring deliberately never recorded. Bounded
     // per connection by one ROB drain and reported separately from the steady-state key conflict
-    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
+    // above so that the two can never be confused in a bench.
     uint64_t fallback_arm_transient = 0;
     uint64_t fallback_atomic_pending = 0;
     uint64_t fallback_missing = 0;
@@ -160,7 +160,7 @@
     uint64_t fallback_seq_churn = 0;
     uint64_t fallback_generation = 0;
     uint64_t fallback_lane_full = 0;
-    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
+    // Lane ADMISSION deferrals. Frames the armed parser left unconsumed at rpos, to be
     // re-parsed by a later pass of the same thread, because the local-read lane had no room
     // (defer_lane_full) or because the connection already held its fair share of a lane under
     // pressure (defer_quota). Neither is a fallback: the read still completes locally, and no

exec
/bin/bash -lc "rg -n 'resize history|stable|logical|slot|accounting' /home/user/Projects/lane-notes-archive/NOTES-EVICT.md | head -45
rg -n '0.5001|0.3750|4096|KiB|32.node|64.shard' /home/user/Projects/lane-notes-archive/NOTES-RECYCLE.md | head -50
rg -n '"'^###|''^##|300|deadlock|order.blind'"' /home/user/Projects/lane-notes-archive/NOTES-MULTIRES.md | head -50" in /home/user/Projects/cx-inline
 succeeded in 0ms:
6:all selection, metadata mutation, accounting and deletion stay inside `FlatStore`.
40:The 12-byte slot estimate is `ceil(8 / 0.70)`: an eight-byte slot divided by FlatStore's 70% target
43:borrows. Those costs would make enforcement depend on resize history rather than the maintained
44:object/key counters. Thus maxmemory is a stable logical cache budget, not an RSS ceiling.
69:branch at each read/write hook and performs no eviction accounting work.
88:from the owning `FlatStore`. Allkeys policies probe random table slots; volatile policies probe the
104:Admission recomputes projected logical bytes after every deletion and removes at most 16 victims
105:for one write. Candidate probing, rehash progress (the existing eight slots per operation), and
131:snapshot. Each EX takes that snapshot at the beginning of a loop pass. Only when its stable version
178:    re-headering another key. Confirm retirement waits for borrow release, accounting drops at
179:    logical eviction, and there is no double free or resurrected key.
7:Profiled on the shipped build (pure writes, pipeline 32, fused, 64 shards, 32 cores):
83:* `kMaxNodesPerClass = kReadLocalRetireRingCapacity` (4096) — *one grace drain's worth*. A drain
85:  very next owner pass asks for again. Measured (§6): a 32-node cap leaves 37–50% of writes still
87:* `kMaxBytes = kReadLocalRetireRingCapacity * kEmbedThreshold` (768 KiB per owner thread) — *one
112:  bound in §4 is **per owner**, so the server-wide ceiling is `threads x 768 KiB` (10.5 MB on the
173:Interleaved (base, recycle, base, recycle, ... one rep each, 64 shards) to rule out box drift
180:### Why the per-class cap is 4096 and not 32 (64 shards, 400k ops, 2 reps)
184:| 32   | 64  | 0.5001 | 3336.3 | 2048 B |
185:| 4096 | 64  | **0.0000** | 3271.9 | 4096 B |
186:| 32   | 256 | 0.3750 | 3163.6 | 2048 B |
187:| 4096 | 256 | **0.0000** | 3115.6 | 6144 B |
189:A 32-node cap saves 2–4 KB per owner thread and gives back 40–60% of the mechanism. The bound
260:an idle tail. `block_cache` is the INFO sum over all 14 owners; the per-owner ceiling is 768 KiB,
314:7. **Idle RSS** after a large keyspace is deleted: bounded by 768 KiB per fused owner thread
16:## 1. PRE vs POST
44:## 2. Reproduction (my own run, and the correction to the brief)
67:## 3. Reduction: 4260 operations → one command
115:## 4. Mechanism
160:## 5. Two fixes that were wrong, and why (do not retry them)
172:`prepare_write_key()` did remove every loss — and **deadlocked at one connection with no WATCH**
173:(0 rounds completed, vs 300 for base). Two independent reasons:
180:2. It still deadlocked, because the gate is not order-aware. A transaction installs on some shards
184:   deadlock-freedom argument for the existing gate rests on the `atomic_hazard` bit — the oldest
197:## 6. Containment
219:## 7. Separate pre-existing defect found on the way: WATCH livelock
221:While hunting a deadlock in fix (a) I built a liveness harness, and it found a defect that is
242:## 8. Files changed

diff --git a//home/user/Projects/cx-inline-comments/src/core/config.h b//home/user/Projects/cx-inline-comments/src/core/config.h
index 495e6ae42da15ee4bcc76a8a846efeef506903d7..8b5f3e5c786e8eeae0296a5dcf19338056d59007
--- a//home/user/Projects/cx-inline-comments/src/core/config.h
+++ b//home/user/Projects/cx-inline-comments/src/core/config.h
@@ -290,8 +290,8 @@
     uint32_t client_lb = 1;
 
     // ---- automatic role split (boot-latched) -----------------------------------------------
-    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer
-    // (DESIGN-flipfp.md); its measured sampling policy is internal to flipctl.h. Off and 1s
+    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer;
+    // its measured sampling policy is internal to flipctl.h. Off and 1s
     // dispatch pay one predicted branch per op and no fingerprint store. Trigger bands learn
     // from the anchor's own quiet jitter; no separate controller tuning knobs remain.
     uint32_t flip_auto = 0;
diff --git a//home/user/Projects/cx-inline-comments/src/core/flipctl.h b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
index 5185da4085ae5f2e3325702064208810cb9c42f7..00b11f3e799ae7313e3ce48ae074715e0bbb7968
--- a//home/user/Projects/cx-inline-comments/src/core/flipctl.h
+++ b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
@@ -19,7 +19,9 @@
 
 class Server;
 
-// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
+// Preserve the measured sampler: one whole parse pass in 100 on average. At a matched 512k ops/s,
+// a GET-only -> SET-only swap triggered the fingerprint detector once with both exhaustive and
+// 1-in-100 sampling; the sampled writer still detected the shift with the rate held fixed.
 // Arming derives solely from the controller latch. No runtime sampling knob or adaptive
 // per-operation work is needed; the detector already derives its bands from actual samples.
 inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
@@ -52,9 +54,11 @@
     uint64_t closed_windows = 0;
 };
 
-// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
-// frame -- one load of this writer's own word, on the line the old enabled() test already read,
-// and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
+// SAMPLED BY PARSE PASS. A per-op countdown would restore the store this sampler removes: no
+// existing io-side command counter covers both the point paths and all io-local commands.
+// The io loop asks pass_sampled() once per dispatched frame -- one load of this writer's own word,
+// on the line the old enabled() test already read, and one predicted-false branch; no store.
+// Inside a sampled pass the full fingerprint body runs
 // for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
 // that pass; at the pass end finish_parse_pass() publishes it and the caller draws the gap to the
 // next sampled pass. `work_window_` is the mean commands per fingerprint
diff --git a//home/user/Projects/cx-inline-comments/src/core/server.h b//home/user/Projects/cx-inline-comments/src/core/server.h
index f869dd3d1737afa6b1c401919fb66c96772b1225..a168d1656a19c4b098167904d42f814caae7513c
--- a//home/user/Projects/cx-inline-comments/src/core/server.h
+++ b//home/user/Projects/cx-inline-comments/src/core/server.h
@@ -8,8 +8,10 @@
 //
 // THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
 // dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
-// its derived shard-granularity fast path, published only at a quiesced commit. See
-// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
+// its derived shard-granularity fast path, published only at a quiesced commit. Shards move only
+// after executing, deferred, group and pinned-read work drains; destination routes publish after
+// the ownership edge. An IO may enqueue using an old route after that edge, so tasks and borrow
+// releases recheck ownership and forward before shard access instead of carrying routing epochs.
 #pragma once
 #include <algorithm>
 #include <atomic>
@@ -308,7 +310,7 @@
         for (uint32_t i = 0; i < nthreads; i++) {
             threads_[i] = std::make_unique<ThreadCtx>();
             // The fingerprint writer is armed only when its one reader, the flip controller, is
-            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
+            // enabled: with --flip-auto 0 and in 1s mode it is dark and costs
             // one predicted branch per op. The enabled sampler keeps its measured 1-in-100 policy.
             threads_[i]->init(i, placement_.role_of(i), nthreads,
                               0,
@@ -1969,7 +1971,7 @@
         shard_owner_[shard_id].store(thread_id, std::memory_order_release);
     }
 
-    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
+    // The caller must hold both executor loops at the safe point:
     // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
     // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
     // Vector capacity and membership are settled while PREPARING still names the source.  The
@@ -1988,7 +1990,7 @@
     // lost, a block ends up linked twice, and the damage surfaces later as either
     // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
     // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
-    // Both were observed (see DESIGN-P0REPLY.md).
+    // Both were observed.
     //
     // The rebind used to be deferred to the destination's own next executor pass
     // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
@@ -2827,7 +2829,7 @@
     // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
     // other command. It widens -- deterministically -- the window in which a younger whole-owner
     // walker could overtake an older same-connection group on the destination shard.
-    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
+    // TEST HOOK: effective local-read LANE CAPACITY for admission. 0 means
     // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
     // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
     // its masking, so this can never overrun anything. It exists because lane oversubscription is
@@ -2935,9 +2937,11 @@
     // is already set. Nothing on GET/SET touches it.
     //
     // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
-    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
-    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
-    // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
+    // sequence: a blocking op requires an empty ROB, then sets the barrier before any younger
+    // frame can be parsed; a resumed scatter inherits that same slot and barrier. The state the
+    // owner-scoped release exists to survive -- a release that must NOT drop the barrier -- has
+    // to be injected. It is held past ROB quiescence on purpose; barrier_release_quiesced()
+    // exempts this one bit for that reason.
     //
     // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
     // nothing left to do and parks on submit_and_wait(1) -- unbounded under io_uring. A hold that
@@ -2968,12 +2972,11 @@
         return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
     }
     // An overlap: some owner took the parse barrier while another owner already held it. With
-    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
-    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
-    // live and the owner-scoped release in blocking_retire() became load-bearing rather than
-    // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
-    // own positive control: a "must be zero" reading proves nothing until something has been shown
-    // able to make it non-zero.
+    // DEBUG BARRIER-HOLD off this must read ZERO -- if it ever moves on production traffic the
+    // latent case just went live and the owner-scoped release in blocking_retire() became
+    // load-bearing rather than defensive. With the latch ON it advances once per blocking dispatch,
+    // which is the counter's own positive control: a "must be zero" reading proves nothing until
+    // something has been shown able to make it non-zero.
     void note_barrier_overlap() {
         barrier_owner_overlaps_.fetch_add(1, std::memory_order_relaxed);
     }
diff --git a//home/user/Projects/cx-inline-comments/src/core/shard.h b//home/user/Projects/cx-inline-comments/src/core/shard.h
index 694b9a93c6cd63db49b9dcf061dbb5182c49986a..39a049d2f8619bbf52f706775956cc9198060364
--- a//home/user/Projects/cx-inline-comments/src/core/shard.h
+++ b//home/user/Projects/cx-inline-comments/src/core/shard.h
@@ -431,7 +431,7 @@
         // NOT share this counter: multi.inc's prepare_write_key() re-asks it at install time and
         // reports violations through INFO atomic_exec_order_late (multi_exec_order_late()), which
         // must read zero. Summing the two would hide a violation inside a number that is non-zero
-        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
+        // by design, leaving the park's safety argument with no falsifier.
         uint64_t atomic_exec_order_holds = 0;
         // Times watch_finalize_reservation() answered "not ready" because the reservation's epoch
         // was still 0, i.e. a unit was turned into a Retry by an undecided WATCH reservation. It
diff --git a//home/user/Projects/cx-inline-comments/src/core/signal.h b//home/user/Projects/cx-inline-comments/src/core/signal.h
index 933a5d7ee09538f3923df3c5eb77f9c72cdc0bfa..f69f7d73942ee129f4ad9cd1f23befe69fc4167d
--- a//home/user/Projects/cx-inline-comments/src/core/signal.h
+++ b//home/user/Projects/cx-inline-comments/src/core/signal.h
@@ -47,7 +47,7 @@
 // that will be compared against a deadline must come from here and never from now_ns() above --
 // that one is CLOCK_MONOTONIC, so its "milliseconds" are milliseconds since boot and sit roughly
 // five orders of magnitude below any real deadline. A monotonic value used as an expiry cut does
-// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
+// not skew the answer, it disables expiry outright.
 inline int64_t now_realtime_ms() {
     timespec ts;
     clock_gettime(CLOCK_REALTIME, &ts);
diff --git a//home/user/Projects/cx-inline-comments/src/core/thread.h b//home/user/Projects/cx-inline-comments/src/core/thread.h
index 0fcfa17ba6e331309260b641ec21aa4a8ff9ba36..3cf72c1b96ca700f602e2e7c630a1e6970f9620f
--- a//home/user/Projects/cx-inline-comments/src/core/thread.h
+++ b//home/user/Projects/cx-inline-comments/src/core/thread.h
@@ -118,7 +118,7 @@
     ContextRoute,           // scatter/script/all-shard or conservative broad-owner route
     ContextKeymissNotify,   // MGET miss must retain owner-side notification behavior
     InflightWrite,
-    ArmTransient,           // pre-arming writes still in flight (DESIGN-RINGDIET.md)
+    ArmTransient,           // pre-arming writes still in flight
     AtomicPending,
     Missing,
     Typed,
@@ -151,7 +151,7 @@
     // ARM-ON-DEMAND TRANSIENT. Reads demoted because the connection carried writes published
     // BEFORE a local read armed it -- writes the RYOW ring deliberately never recorded. Bounded
     // per connection by one ROB drain and reported separately from the steady-state key conflict
-    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
+    // above so that the two can never be confused in a bench.
     uint64_t fallback_arm_transient = 0;
     uint64_t fallback_atomic_pending = 0;
     uint64_t fallback_missing = 0;
@@ -160,7 +160,7 @@
     uint64_t fallback_seq_churn = 0;
     uint64_t fallback_generation = 0;
     uint64_t fallback_lane_full = 0;
-    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
+    // Lane ADMISSION deferrals. Frames the armed parser left unconsumed at rpos, to be
     // re-parsed by a later pass of the same thread, because the local-read lane had no room
     // (defer_lane_full) or because the connection already held its fair share of a lane under
     // pressure (defer_quota). Neither is a fallback: the read still completes locally, and no

exec
/bin/bash -lc "python3 - <<'PY'
import re, subprocess
from pathlib import Path
# C++ lexical comparison: comments/whitespace are the only discarded token classes.
raw=re.compile(r'(?:u8|u|U|L)?R\"(["'^ ()'"\\\\\\t\\r\\n]{0,16})\\(')
quoted=re.compile(r'''(?:u8|u|U|L)?(?:\"(?:\\\\.|["'^"'"\\\\])*\"|'(?:\\\\.|["'^'"'\\\\])*')''',re.S)
number=re.compile(r'''(?:\\d|\\.\\d)(?:[\\w.]|[eEpP][+-]|'[\\w])*''')
ident=re.compile(r'[A-Za-z_"'$][A-Za-z_0-9$]*'"')
punct=sorted(['%:%:','<<=','>>=','<=>','...','->*','::','.*','->','++','--','<<','>>','<=','>=','==','"'!='"','&&','||','*=','/=','%=','+=','-=','&=','"'^='"','|=','##','<:',':>','<%','%>','%:'],key=len,reverse=True)
def tokens(s):
    s=s.replace('\\\\\\r\\n','').replace('\\\\\\n','')
    result=[]; i=0
    while i<len(s):
        if s[i].isspace(): i+=1; continue
        if s.startswith('//',i):
            end=s.find('\\n',i+2); i=len(s) if end<0 else end; continue
        if s.startswith('/*',i):
            end=s.find('*/',i+2); assert end>=0; i=end+2; continue
        m=raw.match(s,i)
        if m:
            tail=')'+m[1]+'\"'; end=s.find(tail,m.end()); assert end>=0
            result.append(s[i:end+len(tail)]); i=end+len(tail); continue
        m=quoted.match(s,i) or number.match(s,i) or ident.match(s,i)
        if m: result.append(m[0]); i=m.end(); continue
        p=next((p for p in punct if s.startswith(p,i)),s[i]); result.append(p); i+=len(p)
    return result
files=subprocess.check_output(['git','diff','--name-only','--','src'],text=True).splitlines()
n=0
for file in files:
    old=subprocess.check_output(['git','show','HEAD:'+file],text=True)
    new=Path(file).read_text()
    a,b=tokens(old),tokens(new)
    assert a==b, f'Non-comment token changed: {file}'
    n+=len(a)
print(f'PASS: identical {n:,} non-comment tokens across {len(files)} modified source files.')
subprocess.run(['git','diff','--check'],check=True)
print('PASS: git diff --check.')
PY" in /home/user/Projects/cx-inline-comments
 succeeded in 1240ms:
PASS: identical 256,358 non-comment tokens across 23 modified source files.
PASS: git diff --check.

exec
/bin/bash -lc 'ls -la .inline-*.json && cat .inline-core-loops.json' in /home/user/Projects/cx-inline-comments
 succeeded in 0ms:
-rw-rw-r-- 1 user user 11898 Sep 10 08:01 .inline-commands.json
-rw-rw-r-- 1 user user 11237 Sep 10 08:00 .inline-core-loops.json
-rw-rw-r-- 1 user user 12009 Sep 10 08:01 .inline-net-store.json
-rw-rw-r-- 1 user user  7409 Sep 10 07:59 .inline-root.json
{
  "entries": [
    {
      "file": "src/core/ex_loop.h",
      "original_line": 59,
      "note": "P128.md",
      "action": "DROP",
      "claim": "The existing comment already explains lane-full-only arming, decay, fair-share admission, and the dormant-path cost.",
      "archive_evidence": "§4, especially its two corrections; §7.1 points 3–4 (lines 179–190, 404–410)."
    },
    {
      "file": "src/core/ex_loop.h",
      "original_line": 141,
      "note": "P128.md",
      "action": "INLINE",
      "claim": "A lower admission cap lets one pipelined socket write fill the lane inside one parse pass, before any drain can race the test; traffic volume alone cannot guarantee the window.",
      "archive_evidence": "§8.1, lines 423–451: escalating traffic failed to arm the row; a capped lane makes the precondition deterministic within one parse pass."
    },
    {
      "file": "src/core/ex_loop.h",
      "original_line": 278,
      "note": "P128.md",
      "action": "DROP",
      "claim": "The existing comment already explains the pressure-gated quota and the intended fair-share/progress reasoning. Its overbroad guarantee is separately reported below.",
      "archive_evidence": "§4 and §7.1, lines 163–176 and 397–420; final quota uses pending_read_local_count, not in_flight."
    },
    {
      "file": "src/core/ex_loop.h",
      "original_line": 313,
      "note": "P128.md",
      "action": "DROP",
      "claim": "The existing comment already preserves eager argument evaluation, the extra cache line, the measured fill increase with flat instructions, and gate-the-argument reasoning.",
      "archive_evidence": "§9.5, lines 689–747: control-cell ABBA results, field-layout analysis, eager active_.size() evaluation, and caller-side pressure gate."
    },
    {
      "file": "src/core/ex_loop.h",
      "original_line": 565,
      "note": "P128.md",
      "action": "DROP",
      "claim": "The existing comment directly describes the per-rotation decay; the full pressure-window rationale remains at its constant.",
      "archive_evidence": "§4 two corrections and §7.1 point 4, lines 179–183 and 407–410."
    },
    {
      "file": "src/core/ex_loop.h",
      "original_line": 2342,
      "note": "NOTES-SERVERTAIL.md",
      "action": "DROP",
      "claim": "The existing comment already states one entry per command, with attribution to the final owner that publishes Done.",
      "archive_evidence": "§3, Divergences from redis, item 2 (lines 86–89): one cross-shard entry, final-answer owner, not the sum of participating shards."
    },
    {
      "file": "src/core/ex_loop.h",
      "original_line": 2526,
      "note": "NOTES-MULTIRES.md",
      "action": "INLINE",
      "claim": "Holding in prepare_write_key() deadlocked because the transaction waited on an older group while holding partial installs that the older group’s order-blind probe waited on. Dispatch-time parking precedes any install and preserves predecessor order.",
      "archive_evidence": "§5(a), lines 164–187: rejected mid-command hold, one-connection deadlock, and the cycle through partial transaction installs; superseding dispatch-time hold explicitly distinguished."
    },
    {
      "file": "src/core/ex_loop.h",
      "original_line": 2951,
      "note": "DESIGN-NOTIFY.md",
      "action": "DROP",
      "claim": "The existing comment already explains one fence/read-first-set/wake decision per batch and the equivalent one-op sequence; nearby comments retain the lost-wake rationale.",
      "archive_evidence": "§2, lines 57–92, and §4.2–4.4: record (io, slot), flush once, and preserve fence→set→wake ordering."
    },
    {
      "file": "src/core/ex_loop.h",
      "original_line": 2995,
      "note": "DESIGN-NOTIFY.md",
      "action": "DROP",
      "claim": "The existing comment already gives the per-edge Dekker argument and why wake decisions must never be cached across flushes.",
      "archive_evidence": "§4.4, lines 248–268: every RMW precedes the shared parked load; caching across flushes can miss a second park."
    },
    {
      "file": "src/core/ex_loop.h",
      "original_line": 3120,
      "note": "DESIGN-NOTIFY.md",
      "action": "DROP",
      "claim": "The existing comment already explains sizing, one fence per batch, bounded record flushing, and why delayed records contain (io, slot) rather than Client pointers.",
      "archive_evidence": "§2, lines 57–90: batch record and second-reap lifetime bound; §4.6, lines 280–290: stale slot/migration behavior."
    },
    {
      "file": "src/core/ex_loop.h",
      "original_line": 3142,
      "note": "DESIGN-NOTIFY.md",
      "action": "DROP",
      "claim": "The existing comment retains the 264-byte record calculation; the cited section adds no further layout requirement.",
      "archive_evidence": "§2, lines 57–60: 32-entry split record, count, open flag; the code comment supplies its exact layout arithmetic."
    },
    {
      "file": "src/core/io_loop.h",
      "original_line": 1482,
      "note": "DESIGN-RINGDIET.md",
      "action": "DROP",
      "claim": "The existing comment already preserves the 1216-byte sidecar/1280-byte allocator class, first-write allocation after read arming, and allocation-failure fallback.",
      "archive_evidence": "Design (2), Sidecar on first write, lines 66–85: no allocation at accept or arming, first armed write allocates, failure returns to unarmed contract."
    },
    {
      "file": "src/core/io_loop.h",
      "original_line": 3054,
      "note": "P128.md",
      "action": "DROP",
      "claim": "The existing comment already explains pressure-gated lane sharing and gating active_.size() evaluation to avoid touching its cold line.",
      "archive_evidence": "§4 final corrections and §9.5, lines 179–190 and 727–747."
    },
    {
      "file": "src/core/io_loop.h",
      "original_line": 3355,
      "note": "DESIGN-DEMOTEGATE.md",
      "action": "DROP",
      "claim": "The existing comment already spells out the two exact prepare early-outs, identical behavior, rejected out-of-line call cost, and measured −108 instr/op and −23 cyc/op.",
      "archive_evidence": "What, Why it is output-identical, and Measured: full note, lines 7–50; armed SET p32, 1T 3275→3167 instructions and 1076→1053 cycles."
    },
    {
      "file": "src/core/io_loop.h",
      "original_line": 3522,
      "note": "P128.md",
      "action": "DROP",
      "claim": "The existing comment already retains the self-rearming quota failure and its 0.8% versus 0.00008% measured deferral rates.",
      "archive_evidence": "§4 first correction, lines 179–183; §9.2 superseded Variant A table and diagnosis."
    },
    {
      "file": "src/core/io_loop.h",
      "original_line": 3536,
      "note": "P128.md",
      "action": "DROP",
      "claim": "The existing comment already retains unpublished-frame deferral, retry venue, the rejected owner-demotion feedback loop, and the reason to share the lane on the next rotation.",
      "archive_evidence": "§2 mechanism (lines 84–100); §4 point 1 (lines 154–160); §7 rotation-order starvation (lines 360–375)."
    },
    {
      "file": "src/core/io_loop.h",
      "original_line": 4379,
      "note": "DESIGN-flipfp.md",
      "action": "DROP",
      "claim": "The existing comment already retains the one-load/branch sampled gate, old 63–86-instruction/five-store body cost, and the never-written dark writer word.",
      "archive_evidence": "§2(a)–(b), lines 40–62, and arithmetic table at lines 65–75."
    },
    {
      "file": "src/core/io_loop.h",
      "original_line": 4433,
      "note": "NOTES-BARRIER.md",
      "action": "INLINE",
      "claim": "Blocking dispatch waits until older ROB operations retire; the barrier prevents younger frames, resumed moves reuse the same slot and owner, and no external production path adds a second owner while parked.",
      "archive_evidence": "§2, lines 68–106: alone-in-ROB proof, inherited blocking-move claim, and rejected external-owner interleaving."
    }
  ],
  "findings": [
    {
      "file": "src/core/ex_loop.h",
      "original_line": 278,
      "kind": "pre-existing comment/code mismatch",
      "detail": "read_local_lane_quota() says the aggregate admitted reads stays within the lane and every connection progresses every rotation. Its max(1, lane_admit_cap / live_connections) cannot establish either when live_connections > lane_admit_cap: cap=16, live=17 permits one per connection but only 16 slots exist. P128 §7.1 repeats the invalid N*max(1,C/N)<=C inequality and labels the >1024-connection case residual risk. The source claim is left intact apart from removing its citation; no code changed."
    },
    {
      "file": "src/core/ex_loop.h",
      "original_line": 313,
      "kind": "pre-existing comment/archive measurement mismatch",
      "detail": "The retained comment says +3.4% same-CCX fills. P128 §9.5 prints 9.19→9.525, or +3.6%; its +11% DRAM figure agrees. The measurement was not silently corrected."
    },
    {
      "file": "src/core/ex_loop.h",
      "original_line": 2310,
      "kind": "pre-existing comment/code mismatch",
      "detail": "The nearby exec_batch_timed comment says a one-op batch is every command on an unpipelined connection. Batches gather Task entries across connections, so multiple depth-1 clients can produce a multi-op executor batch; unpipelined connection depth alone does not guarantee individual timing. This repeats NOTES-SERVERTAIL §3’s overclaim and is left untouched, as requested."
    }
  ],
  "preserve_notes": [
    {
      "note": "P128.md",
      "sections": "§§5, 9–10; §7.1 caveat",
      "reason": "The matched-arm throughput/tail tables, dormant-control cache-fill diagnosis, and clean-versus-contentious rig qualifications are useful research evidence too extensive for any of these local comments. The >capacity fairness claim needs review before treating its proof as authoritative."
    },
    {
      "note": "DESIGN-NOTIFY.md",
      "sections": "§4, especially §§4.5–4.7",
      "reason": "The full multi-case wake argument covers separate batches, teardown/slot reuse, migration, and independent lookers beyond what one call-site comment can hold. Preserve as historical reasoning with §3’s separate per-producer design distinguished from the current ReadyMask implementation."
    },
    {
      "note": "NOTES-MULTIRES.md",
      "sections": "§§2–3, §5(b), §6",
      "reason": "The reduced reproduction and the 16-core-versus-8-core probe result explain why a passing regression test once missed the resolver defect; full test and performance evidence is larger than the dispatch comment."
    },
    {
      "note": "DESIGN-flipfp.md",
      "sections": "§6",
      "reason": "The measured sampled-command count, band/deafness margin, low-rate limits, and cluster-sampling caveat are useful support for research/controller accuracy claims but do not belong in the per-operation gate comment."
    }
  ],
  "verification": "git diff --check passed for assigned files; only // comment lines changed; no build, tests, servers, gate, or benchmarks run."
}

diff --git a//home/user/Projects/cx-inline-comments/src/core/config.h b//home/user/Projects/cx-inline-comments/src/core/config.h
index 495e6ae42da15ee4bcc76a8a846efeef506903d7..8b5f3e5c786e8eeae0296a5dcf19338056d59007
--- a//home/user/Projects/cx-inline-comments/src/core/config.h
+++ b//home/user/Projects/cx-inline-comments/src/core/config.h
@@ -290,8 +290,8 @@
     uint32_t client_lb = 1;
 
     // ---- automatic role split (boot-latched) -----------------------------------------------
-    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer
-    // (DESIGN-flipfp.md); its measured sampling policy is internal to flipctl.h. Off and 1s
+    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer;
+    // its measured sampling policy is internal to flipctl.h. Off and 1s
     // dispatch pay one predicted branch per op and no fingerprint store. Trigger bands learn
     // from the anchor's own quiet jitter; no separate controller tuning knobs remain.
     uint32_t flip_auto = 0;
diff --git a//home/user/Projects/cx-inline-comments/src/core/flipctl.h b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
index 5185da4085ae5f2e3325702064208810cb9c42f7..00b11f3e799ae7313e3ce48ae074715e0bbb7968
--- a//home/user/Projects/cx-inline-comments/src/core/flipctl.h
+++ b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
@@ -19,7 +19,9 @@
 
 class Server;
 
-// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
+// Preserve the measured sampler: one whole parse pass in 100 on average. At a matched 512k ops/s,
+// a GET-only -> SET-only swap triggered the fingerprint detector once with both exhaustive and
+// 1-in-100 sampling; the sampled writer still detected the shift with the rate held fixed.
 // Arming derives solely from the controller latch. No runtime sampling knob or adaptive
 // per-operation work is needed; the detector already derives its bands from actual samples.
 inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
@@ -52,9 +54,11 @@
     uint64_t closed_windows = 0;
 };
 
-// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
-// frame -- one load of this writer's own word, on the line the old enabled() test already read,
-// and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
+// SAMPLED BY PARSE PASS. A per-op countdown would restore the store this sampler removes: no
+// existing io-side command counter covers both the point paths and all io-local commands.
+// The io loop asks pass_sampled() once per dispatched frame -- one load of this writer's own word,
+// on the line the old enabled() test already read, and one predicted-false branch; no store.
+// Inside a sampled pass the full fingerprint body runs
 // for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
 // that pass; at the pass end finish_parse_pass() publishes it and the caller draws the gap to the
 // next sampled pass. `work_window_` is the mean commands per fingerprint
diff --git a//home/user/Projects/cx-inline-comments/src/core/server.h b//home/user/Projects/cx-inline-comments/src/core/server.h
index f869dd3d1737afa6b1c401919fb66c96772b1225..a168d1656a19c4b098167904d42f814caae7513c
--- a//home/user/Projects/cx-inline-comments/src/core/server.h
+++ b//home/user/Projects/cx-inline-comments/src/core/server.h
@@ -8,8 +8,10 @@
 //
 // THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
 // dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
-// its derived shard-granularity fast path, published only at a quiesced commit. See
-// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
+// its derived shard-granularity fast path, published only at a quiesced commit. Shards move only
+// after executing, deferred, group and pinned-read work drains; destination routes publish after
+// the ownership edge. An IO may enqueue using an old route after that edge, so tasks and borrow
+// releases recheck ownership and forward before shard access instead of carrying routing epochs.
 #pragma once
 #include <algorithm>
 #include <atomic>
@@ -308,7 +310,7 @@
         for (uint32_t i = 0; i < nthreads; i++) {
             threads_[i] = std::make_unique<ThreadCtx>();
             // The fingerprint writer is armed only when its one reader, the flip controller, is
-            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
+            // enabled: with --flip-auto 0 and in 1s mode it is dark and costs
             // one predicted branch per op. The enabled sampler keeps its measured 1-in-100 policy.
             threads_[i]->init(i, placement_.role_of(i), nthreads,
                               0,
@@ -1969,7 +1971,7 @@
         shard_owner_[shard_id].store(thread_id, std::memory_order_release);
     }
 
-    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
+    // The caller must hold both executor loops at the safe point:
     // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
     // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
     // Vector capacity and membership are settled while PREPARING still names the source.  The
@@ -1988,7 +1990,7 @@
     // lost, a block ends up linked twice, and the damage surfaces later as either
     // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
     // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
-    // Both were observed (see DESIGN-P0REPLY.md).
+    // Both were observed.
     //
     // The rebind used to be deferred to the destination's own next executor pass
     // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
@@ -2827,7 +2829,7 @@
     // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
     // other command. It widens -- deterministically -- the window in which a younger whole-owner
     // walker could overtake an older same-connection group on the destination shard.
-    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
+    // TEST HOOK: effective local-read LANE CAPACITY for admission. 0 means
     // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
     // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
     // its masking, so this can never overrun anything. It exists because lane oversubscription is
@@ -2935,9 +2937,11 @@
     // is already set. Nothing on GET/SET touches it.
     //
     // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
-    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
-    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
-    // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
+    // sequence: a blocking op requires an empty ROB, then sets the barrier before any younger
+    // frame can be parsed; a resumed scatter inherits that same slot and barrier. The state the
+    // owner-scoped release exists to survive -- a release that must NOT drop the barrier -- has
+    // to be injected. It is held past ROB quiescence on purpose; barrier_release_quiesced()
+    // exempts this one bit for that reason.
     //
     // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
     // nothing left to do and parks on submit_and_wait(1) -- unbounded under io_uring. A hold that
@@ -2968,12 +2972,11 @@
         return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
     }
     // An overlap: some owner took the parse barrier while another owner already held it. With
-    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
-    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
-    // live and the owner-scoped release in blocking_retire() became load-bearing rather than
-    // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
-    // own positive control: a "must be zero" reading proves nothing until something has been shown
-    // able to make it non-zero.
+    // DEBUG BARRIER-HOLD off this must read ZERO -- if it ever moves on production traffic the
+    // latent case just went live and the owner-scoped release in blocking_retire() became
+    // load-bearing rather than defensive. With the latch ON it advances once per blocking dispatch,
+    // which is the counter's own positive control: a "must be zero" reading proves nothing until
+    // something has been shown able to make it non-zero.
     void note_barrier_overlap() {
         barrier_owner_overlaps_.fetch_add(1, std::memory_order_relaxed);
     }
diff --git a//home/user/Projects/cx-inline-comments/src/core/shard.h b//home/user/Projects/cx-inline-comments/src/core/shard.h
index 694b9a93c6cd63db49b9dcf061dbb5182c49986a..39a049d2f8619bbf52f706775956cc9198060364
--- a//home/user/Projects/cx-inline-comments/src/core/shard.h
+++ b//home/user/Projects/cx-inline-comments/src/core/shard.h
@@ -431,7 +431,7 @@
         // NOT share this counter: multi.inc's prepare_write_key() re-asks it at install time and
         // reports violations through INFO atomic_exec_order_late (multi_exec_order_late()), which
         // must read zero. Summing the two would hide a violation inside a number that is non-zero
-        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
+        // by design, leaving the park's safety argument with no falsifier.
         uint64_t atomic_exec_order_holds = 0;
         // Times watch_finalize_reservation() answered "not ready" because the reservation's epoch
         // was still 0, i.e. a unit was turned into a Retry by an undecided WATCH reservation. It
diff --git a//home/user/Projects/cx-inline-comments/src/core/signal.h b//home/user/Projects/cx-inline-comments/src/core/signal.h
index 933a5d7ee09538f3923df3c5eb77f9c72cdc0bfa..f69f7d73942ee129f4ad9cd1f23befe69fc4167d
--- a//home/user/Projects/cx-inline-comments/src/core/signal.h
+++ b//home/user/Projects/cx-inline-comments/src/core/signal.h
@@ -47,7 +47,7 @@
 // that will be compared against a deadline must come from here and never from now_ns() above --
 // that one is CLOCK_MONOTONIC, so its "milliseconds" are milliseconds since boot and sit roughly
 // five orders of magnitude below any real deadline. A monotonic value used as an expiry cut does
-// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
+// not skew the answer, it disables expiry outright.
 inline int64_t now_realtime_ms() {
     timespec ts;
     clock_gettime(CLOCK_REALTIME, &ts);
diff --git a//home/user/Projects/cx-inline-comments/src/core/thread.h b//home/user/Projects/cx-inline-comments/src/core/thread.h
index 0fcfa17ba6e331309260b641ec21aa4a8ff9ba36..3cf72c1b96ca700f602e2e7c630a1e6970f9620f
--- a//home/user/Projects/cx-inline-comments/src/core/thread.h
+++ b//home/user/Projects/cx-inline-comments/src/core/thread.h
@@ -118,7 +118,7 @@
     ContextRoute,           // scatter/script/all-shard or conservative broad-owner route
     ContextKeymissNotify,   // MGET miss must retain owner-side notification behavior
     InflightWrite,
-    ArmTransient,           // pre-arming writes still in flight (DESIGN-RINGDIET.md)
+    ArmTransient,           // pre-arming writes still in flight
     AtomicPending,
     Missing,
     Typed,
@@ -151,7 +151,7 @@
     // ARM-ON-DEMAND TRANSIENT. Reads demoted because the connection carried writes published
     // BEFORE a local read armed it -- writes the RYOW ring deliberately never recorded. Bounded
     // per connection by one ROB drain and reported separately from the steady-state key conflict
-    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
+    // above so that the two can never be confused in a bench.
     uint64_t fallback_arm_transient = 0;
     uint64_t fallback_atomic_pending = 0;
     uint64_t fallback_missing = 0;
@@ -160,7 +160,7 @@
     uint64_t fallback_seq_churn = 0;
     uint64_t fallback_generation = 0;
     uint64_t fallback_lane_full = 0;
-    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
+    // Lane ADMISSION deferrals. Frames the armed parser left unconsumed at rpos, to be
     // re-parsed by a later pass of the same thread, because the local-read lane had no room
     // (defer_lane_full) or because the connection already held its fair share of a lane under
     // pressure (defer_quota). Neither is a fallback: the read still completes locally, and no

codex
All 65 comment references are handled: 15 inlined and 50 dropped after review. The WAITAOF error-string reference remains unchanged. The modified files have identical non-comment token streams, and `git diff --check` passes. I’m consolidating the reference inventory, accuracy findings, and archive retention list.

exec
/bin/bash -lc "python3 - <<'PY'
import json
for name in ('.inline-net-store.json','.inline-commands.json'):
    data=json.load(open(name))
    print(name)
    for key in ('findings','preserve_notes'):
        print(json.dumps({key:data[key]},indent=2))
PY
rg -n -i '(notes[-_/]|design[-_/]|audit[-_/]|lane.*report|p128)' src" in /home/user/Projects/cx-inline-comments
 succeeded in 0ms:
.inline-net-store.json
{
  "findings": [
    {
      "file": "src/net/conn.h",
      "original_line": 84,
      "issue": "The comment says six independent parse-barrier mechanisms, but BarrierOwner has seven production bits including Sleep (original line 101). Left unchanged because this is not dangling-reference reasoning."
    },
    {
      "file": "src/net/rob.h",
      "original_line": 561,
      "issue": "The read-half comment says the two nonzero arm states end for good after the first read; the state comment at original lines 809-810 says each occurs at most once. mark_current_write() deliberately resets to kReadLocalUnarmed after sidecar allocation failure (original line 491), allowing repeated arming. DESIGN-RINGDIET.md lines 73-76 describes this self-healing failure path. Left unchanged."
    },
    {
      "file": "src/store/read_local_settax.h",
      "original_line": 10,
      "issue": "The comment calls the cache the shard's own. read_local_cache_take/put use retire_sink.block_cache, which belongs to the fused owner and is shared by its shards (flatstore_atomic.inc original lines 1434-1438; NOTES-RECYCLE.md \u00a74). Left unchanged."
    },
    {
      "file": "src/store/flatstore_atomic.inc",
      "original_line": 1466,
      "issue": "The cache-release comment says release before evicting user data and describes both callers as whole-server events. Budget paths actually release via refuse_over_budget() after noeviction or failed/bounded eviction, using a shard-local budget decision; successful eviction retains the cache (flatstore.h original lines 1671-1694, 2470-2500; NOTES-RECYCLE.md \u00a75 lines 117-125). Left unchanged."
    },
    {
      "file": "src/store/kv_block_cache.h",
      "original_line": 70,
      "issue": "The original cited measurement says a 32-node cap collapses reuse to a few percent. The cited table instead shows mallocx/op 0.5001 and 0.3750 (roughly 50-62.5% reuse), versus zero at cap 4096. Replaced this measurement while inlining its exact archived evidence; no implementation changed."
    }
  ]
}
{
  "preserve_notes": [
    {
      "note": "NOTES-BARRIER.md",
      "sections": "\u00a71 and \u00a74, lines 21-65 and 137-190",
      "reason": "The full owner/acquire/release audit and rejected alternatives (owner count and quiescence check inside retirement) are useful maintenance evidence beyond the short proof in conn.h."
    },
    {
      "note": "NOTES-MULTIRES.md",
      "sections": "\u00a73-5, especially \u00a75(a)-(b), lines 164-193",
      "reason": "The reduction and both rejected fixes: a mid-command hold creates a partial-install cycle, while collapse-only looked sufficient at 16 cores but failed stale-read probes at the required 8-core geometry. The existing resolver repair and dispatch-time hold depend on these distinctions."
    },
    {
      "note": "NOTES-RECYCLE.md",
      "sections": "\u00a74-7, lines 50-205",
      "reason": "The per-shard versus per-owner measurements, allocator-call/cycle tables, physical versus logical accounting decision, and complete lifecycle/migration matrix are larger than a cap comment."
    },
    {
      "note": "DESIGN-RINGDIET.md",
      "sections": "Results and proof obligations, lines 114-220",
      "reason": "ABBA/null measurements, interference filtering, saturation corrections, and counts proving zero pure-write bookkeeping support performance claims beyond the inline allocation/arming invariants."
    },
    {
      "note": "P128.md",
      "sections": "\u00a77 and \u00a79.3, lines 360-420 and 542-570",
      "reason": "The starvation argument, its stated >1024-active-connections caveat, and pre/post/no-quota rate/cycles/IPC/tail measurements are needed to assess the fair-share claim, beyond the pending-slot counter comment."
    },
    {
      "note": "AUDIT-MARKDIET.md (history only; missing from archive)",
      "sections": "Recovered as 33a17682a:AUDIT-MARKDIET.md, \u00a71-6",
      "reason": "Generated-code and inliner evidence explains the point-mark split and store-forwarding hazard; \u00a74 records the chunk-completion regression. Retaining this source would complete the supplied archive, without adding working notes back to the repository."
    }
  ]
}
.inline-commands.json
{
  "findings": [
    {
      "file": "src/cmd/server_tail.cc",
      "original_line": 172,
      "finding": "The existing WAITAOF comment claims every numlocal == 0 reply is exact against Redis. The implementation always emits [0, 0], while the archive explicitly says the local element is a conservative 0 because TomoKV lacks Redis's per-connection last-write offset. Only the replica count is exact. The comment/code behavior was left unchanged apart from the dangling comment pointer.",
      "evidence": "NOTES-SERVERTAIL.md \u00a71 lines 32-35 and \u00a78 lines 240-253; src/cmd/server_tail.cc:215-219."
    },
    {
      "file": "src/cmd/slowlog.cc",
      "original_line": 10,
      "finding": "The existing comment equates one-op executor batches with every unpipelined connection command. ExLoop::drain_tasks accumulates Task objects without checking connection boundaries, so concurrent depth-1 connections can share an executor batch. The claimed universal exact attribution does not follow from pipeline depth 1.",
      "evidence": "src/core/ex_loop.h:1981-1998 batches all drained tasks; exact path requires n == 1 or escalation at :2325. The same overclaim exists in src/core/ex_loop.h:2310-2311 and NOTES-SERVERTAIL.md \u00a73 lines 68-69."
    },
    {
      "file": "src/cmd/slowlog.cc",
      "original_line": 19,
      "finding": "The existing comment says a command slow exactly once in a pipelined batch arms escalation and its next occurrence is exact. The actual trigger is elapsed / executed >= threshold, so a single slow op can be diluted below the average threshold; exact recurrence timing also lasts only the next 64 batches. Newly inlined slowlog.h wording states these bounds; this pre-existing complete comment was otherwise retained.",
      "evidence": "src/core/ex_loop.h:2366-2377; kSlowlogEscalateBatches = 64 in src/cmd/slowlog.h; NOTES-SERVERTAIL.md \u00a73 lines 70-84."
    },
    {
      "file": "src/cmd/blocking.inc",
      "original_line": 1347,
      "finding": "The retained numeric source cross-reference io_loop.h:1391 is stale: the blocking in_flight guard is now around src/core/io_loop.h:3953. The invariant itself remains correct.",
      "evidence": "src/core/io_loop.h:3945-3953; original note refers to historical line 1391."
    }
  ]
}
{
  "preserve_notes": [
    {
      "note": "NOTES-SERVERTAIL.md",
      "content": "\u00a78 contains the deferred WAITAOF implementation plan beyond the existing source summary: per-connection write coverage, forcing producers to post, a policy-independent durable accessor (reply_gate_ready is insufficient under no/everysec), and validation under both persist-IO engines. Treat its instruction to wait for flush_id past the WAITAOF op itself as historical design prose requiring review: an unpublished reply cannot retire itself. \u00a710 preserves an explicitly indicative PRE/POST table, including default-on SET instruction cost +0.73%, which missed its +0.5% target; neither the geometry nor those instruction deltas support a general throughput claim."
    },
    {
      "note": "NOTES-MULTIRES.md",
      "content": "\u00a75(b) preserves why the resolver repair cannot be dropped based on a misleading 16-core non-reproduction: at the correct 8-core geometry, collapse-only failed 3 of 8 runs while collapse+resolver passed 10 of 10. This is useful measurement-history evidence beyond the command-side hold comment."
    },
    {
      "note": "NOTES-MULTIRACE.md",
      "content": "\u00a75 preserves the full three-part acyclicity argument (per-shard program order, parked predecessor exclusion, owner-only pending-list publication), plus the residual two-phase-unit shape covered by the RENAME/LMPOP/SMOVE liveness battery. The short inline hold-cycle explanation does not replace that complete argument and test rationale."
    },
    {
      "note": "NOTES-BARRIER.md",
      "content": "\u00a7\u00a71-4 retain the full acquire/release ownership audit, rejected owner-count and ROB-quiescence designs, and the separate blocked_ ownership concern. \u00a75 gives the four-control anti-vacuity test and injector-break check, including cross-owner geometry and explicit wakeup requirements; these details exceed the local command comments."
    },
    {
      "note": "NOTES-SCRIPTATOMIC.md",
      "content": "The root-cause section preserves minimal write-then-raise reproductions that distinguish lost updates from deleted newly-created keys, the MVCC capture/raw-restore mismatch, and the alternatives table proving that even correct rollback remains Redis-incompatible. The comment retains the reason but cannot carry the reproducible evidence."
    }
  ]
}
src/core/io_loop.h:3548:                                    // owner as an ordinary task, was the seed of the p128 and
src/cmd/t_string.cc:542:// perturbing GCC's inlining decisions in the p128 SET cell.
src/cmd/server_tail.cc:212:                  "ERR WAITAOF with numlocal > 0 is not implemented by tomokv: the local fsync wait needs the deferred-reply path, see NOTES-SERVERTAIL.md");
src/net/resp.h:61:// pre-AUTH limits) cost +83 instructions/op on loopback and −5.1% on the wire p128 GET cell —

diff --git a//home/user/Projects/cx-inline-comments/src/core/config.h b//home/user/Projects/cx-inline-comments/src/core/config.h
index 495e6ae42da15ee4bcc76a8a846efeef506903d7..8b5f3e5c786e8eeae0296a5dcf19338056d59007
--- a//home/user/Projects/cx-inline-comments/src/core/config.h
+++ b//home/user/Projects/cx-inline-comments/src/core/config.h
@@ -290,8 +290,8 @@
     uint32_t client_lb = 1;
 
     // ---- automatic role split (boot-latched) -----------------------------------------------
-    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer
-    // (DESIGN-flipfp.md); its measured sampling policy is internal to flipctl.h. Off and 1s
+    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer;
+    // its measured sampling policy is internal to flipctl.h. Off and 1s
     // dispatch pay one predicted branch per op and no fingerprint store. Trigger bands learn
     // from the anchor's own quiet jitter; no separate controller tuning knobs remain.
     uint32_t flip_auto = 0;
diff --git a//home/user/Projects/cx-inline-comments/src/core/flipctl.h b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
index 5185da4085ae5f2e3325702064208810cb9c42f7..00b11f3e799ae7313e3ce48ae074715e0bbb7968
--- a//home/user/Projects/cx-inline-comments/src/core/flipctl.h
+++ b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
@@ -19,7 +19,9 @@
 
 class Server;
 
-// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
+// Preserve the measured sampler: one whole parse pass in 100 on average. At a matched 512k ops/s,
+// a GET-only -> SET-only swap triggered the fingerprint detector once with both exhaustive and
+// 1-in-100 sampling; the sampled writer still detected the shift with the rate held fixed.
 // Arming derives solely from the controller latch. No runtime sampling knob or adaptive
 // per-operation work is needed; the detector already derives its bands from actual samples.
 inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
@@ -52,9 +54,11 @@
     uint64_t closed_windows = 0;
 };
 
-// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
-// frame -- one load of this writer's own word, on the line the old enabled() test already read,
-// and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
+// SAMPLED BY PARSE PASS. A per-op countdown would restore the store this sampler removes: no
+// existing io-side command counter covers both the point paths and all io-local commands.
+// The io loop asks pass_sampled() once per dispatched frame -- one load of this writer's own word,
+// on the line the old enabled() test already read, and one predicted-false branch; no store.
+// Inside a sampled pass the full fingerprint body runs
 // for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
 // that pass; at the pass end finish_parse_pass() publishes it and the caller draws the gap to the
 // next sampled pass. `work_window_` is the mean commands per fingerprint
diff --git a//home/user/Projects/cx-inline-comments/src/core/server.h b//home/user/Projects/cx-inline-comments/src/core/server.h
index f869dd3d1737afa6b1c401919fb66c96772b1225..a168d1656a19c4b098167904d42f814caae7513c
--- a//home/user/Projects/cx-inline-comments/src/core/server.h
+++ b//home/user/Projects/cx-inline-comments/src/core/server.h
@@ -8,8 +8,10 @@
 //
 // THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
 // dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
-// its derived shard-granularity fast path, published only at a quiesced commit. See
-// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
+// its derived shard-granularity fast path, published only at a quiesced commit. Shards move only
+// after executing, deferred, group and pinned-read work drains; destination routes publish after
+// the ownership edge. An IO may enqueue using an old route after that edge, so tasks and borrow
+// releases recheck ownership and forward before shard access instead of carrying routing epochs.
 #pragma once
 #include <algorithm>
 #include <atomic>
@@ -308,7 +310,7 @@
         for (uint32_t i = 0; i < nthreads; i++) {
             threads_[i] = std::make_unique<ThreadCtx>();
             // The fingerprint writer is armed only when its one reader, the flip controller, is
-            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
+            // enabled: with --flip-auto 0 and in 1s mode it is dark and costs
             // one predicted branch per op. The enabled sampler keeps its measured 1-in-100 policy.
             threads_[i]->init(i, placement_.role_of(i), nthreads,
                               0,
@@ -1969,7 +1971,7 @@
         shard_owner_[shard_id].store(thread_id, std::memory_order_release);
     }
 
-    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
+    // The caller must hold both executor loops at the safe point:
     // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
     // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
     // Vector capacity and membership are settled while PREPARING still names the source.  The
@@ -1988,7 +1990,7 @@
     // lost, a block ends up linked twice, and the damage surfaces later as either
     // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
     // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
-    // Both were observed (see DESIGN-P0REPLY.md).
+    // Both were observed.
     //
     // The rebind used to be deferred to the destination's own next executor pass
     // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
@@ -2827,7 +2829,7 @@
     // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
     // other command. It widens -- deterministically -- the window in which a younger whole-owner
     // walker could overtake an older same-connection group on the destination shard.
-    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
+    // TEST HOOK: effective local-read LANE CAPACITY for admission. 0 means
     // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
     // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
     // its masking, so this can never overrun anything. It exists because lane oversubscription is
@@ -2935,9 +2937,11 @@
     // is already set. Nothing on GET/SET touches it.
     //
     // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
-    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
-    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
-    // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
+    // sequence: a blocking op requires an empty ROB, then sets the barrier before any younger
+    // frame can be parsed; a resumed scatter inherits that same slot and barrier. The state the
+    // owner-scoped release exists to survive -- a release that must NOT drop the barrier -- has
+    // to be injected. It is held past ROB quiescence on purpose; barrier_release_quiesced()
+    // exempts this one bit for that reason.
     //
     // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
     // nothing left to do and parks on submit_and_wait(1) -- unbounded under io_uring. A hold that
@@ -2968,12 +2972,11 @@
         return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
     }
     // An overlap: some owner took the parse barrier while another owner already held it. With
-    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
-    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
-    // live and the owner-scoped release in blocking_retire() became load-bearing rather than
-    // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
-    // own positive control: a "must be zero" reading proves nothing until something has been shown
-    // able to make it non-zero.
+    // DEBUG BARRIER-HOLD off this must read ZERO -- if it ever moves on production traffic the
+    // latent case just went live and the owner-scoped release in blocking_retire() became
+    // load-bearing rather than defensive. With the latch ON it advances once per blocking dispatch,
+    // which is the counter's own positive control: a "must be zero" reading proves nothing until
+    // something has been shown able to make it non-zero.
     void note_barrier_overlap() {
         barrier_owner_overlaps_.fetch_add(1, std::memory_order_relaxed);
     }
diff --git a//home/user/Projects/cx-inline-comments/src/core/shard.h b//home/user/Projects/cx-inline-comments/src/core/shard.h
index 694b9a93c6cd63db49b9dcf061dbb5182c49986a..39a049d2f8619bbf52f706775956cc9198060364
--- a//home/user/Projects/cx-inline-comments/src/core/shard.h
+++ b//home/user/Projects/cx-inline-comments/src/core/shard.h
@@ -431,7 +431,7 @@
         // NOT share this counter: multi.inc's prepare_write_key() re-asks it at install time and
         // reports violations through INFO atomic_exec_order_late (multi_exec_order_late()), which
         // must read zero. Summing the two would hide a violation inside a number that is non-zero
-        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
+        // by design, leaving the park's safety argument with no falsifier.
         uint64_t atomic_exec_order_holds = 0;
         // Times watch_finalize_reservation() answered "not ready" because the reservation's epoch
         // was still 0, i.e. a unit was turned into a Retry by an undecided WATCH reservation. It
diff --git a//home/user/Projects/cx-inline-comments/src/core/signal.h b//home/user/Projects/cx-inline-comments/src/core/signal.h
index 933a5d7ee09538f3923df3c5eb77f9c72cdc0bfa..f69f7d73942ee129f4ad9cd1f23befe69fc4167d
--- a//home/user/Projects/cx-inline-comments/src/core/signal.h
+++ b//home/user/Projects/cx-inline-comments/src/core/signal.h
@@ -47,7 +47,7 @@
 // that will be compared against a deadline must come from here and never from now_ns() above --
 // that one is CLOCK_MONOTONIC, so its "milliseconds" are milliseconds since boot and sit roughly
 // five orders of magnitude below any real deadline. A monotonic value used as an expiry cut does
-// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
+// not skew the answer, it disables expiry outright.
 inline int64_t now_realtime_ms() {
     timespec ts;
     clock_gettime(CLOCK_REALTIME, &ts);
diff --git a//home/user/Projects/cx-inline-comments/src/core/thread.h b//home/user/Projects/cx-inline-comments/src/core/thread.h
index 0fcfa17ba6e331309260b641ec21aa4a8ff9ba36..3cf72c1b96ca700f602e2e7c630a1e6970f9620f
--- a//home/user/Projects/cx-inline-comments/src/core/thread.h
+++ b//home/user/Projects/cx-inline-comments/src/core/thread.h
@@ -118,7 +118,7 @@
     ContextRoute,           // scatter/script/all-shard or conservative broad-owner route
     ContextKeymissNotify,   // MGET miss must retain owner-side notification behavior
     InflightWrite,
-    ArmTransient,           // pre-arming writes still in flight (DESIGN-RINGDIET.md)
+    ArmTransient,           // pre-arming writes still in flight
     AtomicPending,
     Missing,
     Typed,
@@ -151,7 +151,7 @@
     // ARM-ON-DEMAND TRANSIENT. Reads demoted because the connection carried writes published
     // BEFORE a local read armed it -- writes the RYOW ring deliberately never recorded. Bounded
     // per connection by one ROB drain and reported separately from the steady-state key conflict
-    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
+    // above so that the two can never be confused in a bench.
     uint64_t fallback_arm_transient = 0;
     uint64_t fallback_atomic_pending = 0;
     uint64_t fallback_missing = 0;
@@ -160,7 +160,7 @@
     uint64_t fallback_seq_churn = 0;
     uint64_t fallback_generation = 0;
     uint64_t fallback_lane_full = 0;
-    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
+    // Lane ADMISSION deferrals. Frames the armed parser left unconsumed at rpos, to be
     // re-parsed by a later pass of the same thread, because the local-read lane had no room
     // (defer_lane_full) or because the connection already held its fair share of a lane under
     // pressure (defer_quota). Neither is a fallback: the read still completes locally, and no

diff --git a//home/user/Projects/cx-inline-comments/src/core/config.h b//home/user/Projects/cx-inline-comments/src/core/config.h
index 495e6ae42da15ee4bcc76a8a846efeef506903d7..8b5f3e5c786e8eeae0296a5dcf19338056d59007
--- a//home/user/Projects/cx-inline-comments/src/core/config.h
+++ b//home/user/Projects/cx-inline-comments/src/core/config.h
@@ -290,8 +290,8 @@
     uint32_t client_lb = 1;
 
     // ---- automatic role split (boot-latched) -----------------------------------------------
-    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer
-    // (DESIGN-flipfp.md); its measured sampling policy is internal to flipctl.h. Off and 1s
+    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer;
+    // its measured sampling policy is internal to flipctl.h. Off and 1s
     // dispatch pay one predicted branch per op and no fingerprint store. Trigger bands learn
     // from the anchor's own quiet jitter; no separate controller tuning knobs remain.
     uint32_t flip_auto = 0;
diff --git a//home/user/Projects/cx-inline-comments/src/core/flipctl.h b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
index 5185da4085ae5f2e3325702064208810cb9c42f7..00b11f3e799ae7313e3ce48ae074715e0bbb7968
--- a//home/user/Projects/cx-inline-comments/src/core/flipctl.h
+++ b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
@@ -19,7 +19,9 @@
 
 class Server;
 
-// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
+// Preserve the measured sampler: one whole parse pass in 100 on average. At a matched 512k ops/s,
+// a GET-only -> SET-only swap triggered the fingerprint detector once with both exhaustive and
+// 1-in-100 sampling; the sampled writer still detected the shift with the rate held fixed.
 // Arming derives solely from the controller latch. No runtime sampling knob or adaptive
 // per-operation work is needed; the detector already derives its bands from actual samples.
 inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
@@ -52,9 +54,11 @@
     uint64_t closed_windows = 0;
 };
 
-// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
-// frame -- one load of this writer's own word, on the line the old enabled() test already read,
-// and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
+// SAMPLED BY PARSE PASS. A per-op countdown would restore the store this sampler removes: no
+// existing io-side command counter covers both the point paths and all io-local commands.
+// The io loop asks pass_sampled() once per dispatched frame -- one load of this writer's own word,
+// on the line the old enabled() test already read, and one predicted-false branch; no store.
+// Inside a sampled pass the full fingerprint body runs
 // for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
 // that pass; at the pass end finish_parse_pass() publishes it and the caller draws the gap to the
 // next sampled pass. `work_window_` is the mean commands per fingerprint
diff --git a//home/user/Projects/cx-inline-comments/src/core/server.h b//home/user/Projects/cx-inline-comments/src/core/server.h
index f869dd3d1737afa6b1c401919fb66c96772b1225..a168d1656a19c4b098167904d42f814caae7513c
--- a//home/user/Projects/cx-inline-comments/src/core/server.h
+++ b//home/user/Projects/cx-inline-comments/src/core/server.h
@@ -8,8 +8,10 @@
 //
 // THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
 // dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
-// its derived shard-granularity fast path, published only at a quiesced commit. See
-// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
+// its derived shard-granularity fast path, published only at a quiesced commit. Shards move only
+// after executing, deferred, group and pinned-read work drains; destination routes publish after
+// the ownership edge. An IO may enqueue using an old route after that edge, so tasks and borrow
+// releases recheck ownership and forward before shard access instead of carrying routing epochs.
 #pragma once
 #include <algorithm>
 #include <atomic>
@@ -308,7 +310,7 @@
         for (uint32_t i = 0; i < nthreads; i++) {
             threads_[i] = std::make_unique<ThreadCtx>();
             // The fingerprint writer is armed only when its one reader, the flip controller, is
-            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
+            // enabled: with --flip-auto 0 and in 1s mode it is dark and costs
             // one predicted branch per op. The enabled sampler keeps its measured 1-in-100 policy.
             threads_[i]->init(i, placement_.role_of(i), nthreads,
                               0,
@@ -1969,7 +1971,7 @@
         shard_owner_[shard_id].store(thread_id, std::memory_order_release);
     }
 
-    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
+    // The caller must hold both executor loops at the safe point:
     // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
     // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
     // Vector capacity and membership are settled while PREPARING still names the source.  The
@@ -1988,7 +1990,7 @@
     // lost, a block ends up linked twice, and the damage surfaces later as either
     // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
     // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
-    // Both were observed (see DESIGN-P0REPLY.md).
+    // Both were observed.
     //
     // The rebind used to be deferred to the destination's own next executor pass
     // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
@@ -2827,7 +2829,7 @@
     // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
     // other command. It widens -- deterministically -- the window in which a younger whole-owner
     // walker could overtake an older same-connection group on the destination shard.
-    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
+    // TEST HOOK: effective local-read LANE CAPACITY for admission. 0 means
     // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
     // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
     // its masking, so this can never overrun anything. It exists because lane oversubscription is
@@ -2935,9 +2937,11 @@
     // is already set. Nothing on GET/SET touches it.
     //
     // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
-    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
-    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
-    // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
+    // sequence: a blocking op requires an empty ROB, then sets the barrier before any younger
+    // frame can be parsed; a resumed scatter inherits that same slot and barrier. The state the
+    // owner-scoped release exists to survive -- a release that must NOT drop the barrier -- has
+    // to be injected. It is held past ROB quiescence on purpose; barrier_release_quiesced()
+    // exempts this one bit for that reason.
     //
     // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
     // nothing left to do and parks on submit_and_wait(1) -- unbounded under io_uring. A hold that
@@ -2968,12 +2972,11 @@
         return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
     }
     // An overlap: some owner took the parse barrier while another owner already held it. With
-    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
-    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
-    // live and the owner-scoped release in blocking_retire() became load-bearing rather than
-    // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
-    // own positive control: a "must be zero" reading proves nothing until something has been shown
-    // able to make it non-zero.
+    // DEBUG BARRIER-HOLD off this must read ZERO -- if it ever moves on production traffic the
+    // latent case just went live and the owner-scoped release in blocking_retire() became
+    // load-bearing rather than defensive. With the latch ON it advances once per blocking dispatch,
+    // which is the counter's own positive control: a "must be zero" reading proves nothing until
+    // something has been shown able to make it non-zero.
     void note_barrier_overlap() {
         barrier_owner_overlaps_.fetch_add(1, std::memory_order_relaxed);
     }
diff --git a//home/user/Projects/cx-inline-comments/src/core/shard.h b//home/user/Projects/cx-inline-comments/src/core/shard.h
index 694b9a93c6cd63db49b9dcf061dbb5182c49986a..39a049d2f8619bbf52f706775956cc9198060364
--- a//home/user/Projects/cx-inline-comments/src/core/shard.h
+++ b//home/user/Projects/cx-inline-comments/src/core/shard.h
@@ -431,7 +431,7 @@
         // NOT share this counter: multi.inc's prepare_write_key() re-asks it at install time and
         // reports violations through INFO atomic_exec_order_late (multi_exec_order_late()), which
         // must read zero. Summing the two would hide a violation inside a number that is non-zero
-        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
+        // by design, leaving the park's safety argument with no falsifier.
         uint64_t atomic_exec_order_holds = 0;
         // Times watch_finalize_reservation() answered "not ready" because the reservation's epoch
         // was still 0, i.e. a unit was turned into a Retry by an undecided WATCH reservation. It
diff --git a//home/user/Projects/cx-inline-comments/src/core/signal.h b//home/user/Projects/cx-inline-comments/src/core/signal.h
index 933a5d7ee09538f3923df3c5eb77f9c72cdc0bfa..f69f7d73942ee129f4ad9cd1f23befe69fc4167d
--- a//home/user/Projects/cx-inline-comments/src/core/signal.h
+++ b//home/user/Projects/cx-inline-comments/src/core/signal.h
@@ -47,7 +47,7 @@
 // that will be compared against a deadline must come from here and never from now_ns() above --
 // that one is CLOCK_MONOTONIC, so its "milliseconds" are milliseconds since boot and sit roughly
 // five orders of magnitude below any real deadline. A monotonic value used as an expiry cut does
-// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
+// not skew the answer, it disables expiry outright.
 inline int64_t now_realtime_ms() {
     timespec ts;
     clock_gettime(CLOCK_REALTIME, &ts);
diff --git a//home/user/Projects/cx-inline-comments/src/core/thread.h b//home/user/Projects/cx-inline-comments/src/core/thread.h
index 0fcfa17ba6e331309260b641ec21aa4a8ff9ba36..3cf72c1b96ca700f602e2e7c630a1e6970f9620f
--- a//home/user/Projects/cx-inline-comments/src/core/thread.h
+++ b//home/user/Projects/cx-inline-comments/src/core/thread.h
@@ -118,7 +118,7 @@
     ContextRoute,           // scatter/script/all-shard or conservative broad-owner route
     ContextKeymissNotify,   // MGET miss must retain owner-side notification behavior
     InflightWrite,
-    ArmTransient,           // pre-arming writes still in flight (DESIGN-RINGDIET.md)
+    ArmTransient,           // pre-arming writes still in flight
     AtomicPending,
     Missing,
     Typed,
@@ -151,7 +151,7 @@
     // ARM-ON-DEMAND TRANSIENT. Reads demoted because the connection carried writes published
     // BEFORE a local read armed it -- writes the RYOW ring deliberately never recorded. Bounded
     // per connection by one ROB drain and reported separately from the steady-state key conflict
-    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
+    // above so that the two can never be confused in a bench.
     uint64_t fallback_arm_transient = 0;
     uint64_t fallback_atomic_pending = 0;
     uint64_t fallback_missing = 0;
@@ -160,7 +160,7 @@
     uint64_t fallback_seq_churn = 0;
     uint64_t fallback_generation = 0;
     uint64_t fallback_lane_full = 0;
-    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
+    // Lane ADMISSION deferrals. Frames the armed parser left unconsumed at rpos, to be
     // re-parsed by a later pass of the same thread, because the local-read lane had no room
     // (defer_lane_full) or because the connection already held its fair share of a lane under
     // pressure (defer_quota). Neither is a fallback: the read still completes locally, and no

exec
/bin/bash -lc "python3 - <<'PY'
import json, subprocess
from pathlib import Path
from collections import Counter
records=[]
for p in sorted(Path('.').glob('.inline-*.json')):
    records.extend(json.loads(p.read_text())['entries'])
records.sort(key=lambda e:(e['file'],e['original_line']))
refs=[]
for line in subprocess.check_output(['git','grep','-n',r'\\.md','HEAD','--','src'],text=True).splitlines():
    _,file,number,body=line.split(':',3)
    refs.append((file,int(number)))
assert len(refs)==66
assert len(records)==66
assert len({(e['file'],e['original_line']) for e in records})==66
assert sorted(refs)==[(e['file'],e['original_line']) for e in records]
print('PASS: every baseline reference appears exactly once in the inventory.')
print(Counter(e['action'] for e in records))
print('Notes:', ', '.join(sorted({e['note'] for e in records})))
PY" in /home/user/Projects/cx-inline-comments
 exited 1 in 0ms:
Traceback (most recent call last):
  File "<stdin>", line 6, in <module>
KeyError: 'entries'

diff --git a//home/user/Projects/cx-inline-comments/src/core/config.h b//home/user/Projects/cx-inline-comments/src/core/config.h
index 495e6ae42da15ee4bcc76a8a846efeef506903d7..8b5f3e5c786e8eeae0296a5dcf19338056d59007
--- a//home/user/Projects/cx-inline-comments/src/core/config.h
+++ b//home/user/Projects/cx-inline-comments/src/core/config.h
@@ -290,8 +290,8 @@
     uint32_t client_lb = 1;
 
     // ---- automatic role split (boot-latched) -----------------------------------------------
-    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer
-    // (DESIGN-flipfp.md); its measured sampling policy is internal to flipctl.h. Off and 1s
+    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer;
+    // its measured sampling policy is internal to flipctl.h. Off and 1s
     // dispatch pay one predicted branch per op and no fingerprint store. Trigger bands learn
     // from the anchor's own quiet jitter; no separate controller tuning knobs remain.
     uint32_t flip_auto = 0;
diff --git a//home/user/Projects/cx-inline-comments/src/core/flipctl.h b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
index 5185da4085ae5f2e3325702064208810cb9c42f7..00b11f3e799ae7313e3ce48ae074715e0bbb7968
--- a//home/user/Projects/cx-inline-comments/src/core/flipctl.h
+++ b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
@@ -19,7 +19,9 @@
 
 class Server;
 
-// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
+// Preserve the measured sampler: one whole parse pass in 100 on average. At a matched 512k ops/s,
+// a GET-only -> SET-only swap triggered the fingerprint detector once with both exhaustive and
+// 1-in-100 sampling; the sampled writer still detected the shift with the rate held fixed.
 // Arming derives solely from the controller latch. No runtime sampling knob or adaptive
 // per-operation work is needed; the detector already derives its bands from actual samples.
 inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
@@ -52,9 +54,11 @@
     uint64_t closed_windows = 0;
 };
 
-// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
-// frame -- one load of this writer's own word, on the line the old enabled() test already read,
-// and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
+// SAMPLED BY PARSE PASS. A per-op countdown would restore the store this sampler removes: no
+// existing io-side command counter covers both the point paths and all io-local commands.
+// The io loop asks pass_sampled() once per dispatched frame -- one load of this writer's own word,
+// on the line the old enabled() test already read, and one predicted-false branch; no store.
+// Inside a sampled pass the full fingerprint body runs
 // for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
 // that pass; at the pass end finish_parse_pass() publishes it and the caller draws the gap to the
 // next sampled pass. `work_window_` is the mean commands per fingerprint
diff --git a//home/user/Projects/cx-inline-comments/src/core/server.h b//home/user/Projects/cx-inline-comments/src/core/server.h
index f869dd3d1737afa6b1c401919fb66c96772b1225..a168d1656a19c4b098167904d42f814caae7513c
--- a//home/user/Projects/cx-inline-comments/src/core/server.h
+++ b//home/user/Projects/cx-inline-comments/src/core/server.h
@@ -8,8 +8,10 @@
 //
 // THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
 // dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
-// its derived shard-granularity fast path, published only at a quiesced commit. See
-// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
+// its derived shard-granularity fast path, published only at a quiesced commit. Shards move only
+// after executing, deferred, group and pinned-read work drains; destination routes publish after
+// the ownership edge. An IO may enqueue using an old route after that edge, so tasks and borrow
+// releases recheck ownership and forward before shard access instead of carrying routing epochs.
 #pragma once
 #include <algorithm>
 #include <atomic>
@@ -308,7 +310,7 @@
         for (uint32_t i = 0; i < nthreads; i++) {
             threads_[i] = std::make_unique<ThreadCtx>();
             // The fingerprint writer is armed only when its one reader, the flip controller, is
-            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
+            // enabled: with --flip-auto 0 and in 1s mode it is dark and costs
             // one predicted branch per op. The enabled sampler keeps its measured 1-in-100 policy.
             threads_[i]->init(i, placement_.role_of(i), nthreads,
                               0,
@@ -1969,7 +1971,7 @@
         shard_owner_[shard_id].store(thread_id, std::memory_order_release);
     }
 
-    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
+    // The caller must hold both executor loops at the safe point:
     // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
     // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
     // Vector capacity and membership are settled while PREPARING still names the source.  The
@@ -1988,7 +1990,7 @@
     // lost, a block ends up linked twice, and the damage surfaces later as either
     // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
     // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
-    // Both were observed (see DESIGN-P0REPLY.md).
+    // Both were observed.
     //
     // The rebind used to be deferred to the destination's own next executor pass
     // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
@@ -2827,7 +2829,7 @@
     // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
     // other command. It widens -- deterministically -- the window in which a younger whole-owner
     // walker could overtake an older same-connection group on the destination shard.
-    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
+    // TEST HOOK: effective local-read LANE CAPACITY for admission. 0 means
     // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
     // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
     // its masking, so this can never overrun anything. It exists because lane oversubscription is
@@ -2935,9 +2937,11 @@
     // is already set. Nothing on GET/SET touches it.
     //
     // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
-    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
-    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
-    // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
+    // sequence: a blocking op requires an empty ROB, then sets the barrier before any younger
+    // frame can be parsed; a resumed scatter inherits that same slot and barrier. The state the
+    // owner-scoped release exists to survive -- a release that must NOT drop the barrier -- has
+    // to be injected. It is held past ROB quiescence on purpose; barrier_release_quiesced()
+    // exempts this one bit for that reason.
     //
     // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
     // nothing left to do and parks on submit_and_wait(1) -- unbounded under io_uring. A hold that
@@ -2968,12 +2972,11 @@
         return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
     }
     // An overlap: some owner took the parse barrier while another owner already held it. With
-    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
-    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
-    // live and the owner-scoped release in blocking_retire() became load-bearing rather than
-    // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
-    // own positive control: a "must be zero" reading proves nothing until something has been shown
-    // able to make it non-zero.
+    // DEBUG BARRIER-HOLD off this must read ZERO -- if it ever moves on production traffic the
+    // latent case just went live and the owner-scoped release in blocking_retire() became
+    // load-bearing rather than defensive. With the latch ON it advances once per blocking dispatch,
+    // which is the counter's own positive control: a "must be zero" reading proves nothing until
+    // something has been shown able to make it non-zero.
     void note_barrier_overlap() {
         barrier_owner_overlaps_.fetch_add(1, std::memory_order_relaxed);
     }
diff --git a//home/user/Projects/cx-inline-comments/src/core/shard.h b//home/user/Projects/cx-inline-comments/src/core/shard.h
index 694b9a93c6cd63db49b9dcf061dbb5182c49986a..39a049d2f8619bbf52f706775956cc9198060364
--- a//home/user/Projects/cx-inline-comments/src/core/shard.h
+++ b//home/user/Projects/cx-inline-comments/src/core/shard.h
@@ -431,7 +431,7 @@
         // NOT share this counter: multi.inc's prepare_write_key() re-asks it at install time and
         // reports violations through INFO atomic_exec_order_late (multi_exec_order_late()), which
         // must read zero. Summing the two would hide a violation inside a number that is non-zero
-        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
+        // by design, leaving the park's safety argument with no falsifier.
         uint64_t atomic_exec_order_holds = 0;
         // Times watch_finalize_reservation() answered "not ready" because the reservation's epoch
         // was still 0, i.e. a unit was turned into a Retry by an undecided WATCH reservation. It
diff --git a//home/user/Projects/cx-inline-comments/src/core/signal.h b//home/user/Projects/cx-inline-comments/src/core/signal.h
index 933a5d7ee09538f3923df3c5eb77f9c72cdc0bfa..f69f7d73942ee129f4ad9cd1f23befe69fc4167d
--- a//home/user/Projects/cx-inline-comments/src/core/signal.h
+++ b//home/user/Projects/cx-inline-comments/src/core/signal.h
@@ -47,7 +47,7 @@
 // that will be compared against a deadline must come from here and never from now_ns() above --
 // that one is CLOCK_MONOTONIC, so its "milliseconds" are milliseconds since boot and sit roughly
 // five orders of magnitude below any real deadline. A monotonic value used as an expiry cut does
-// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
+// not skew the answer, it disables expiry outright.
 inline int64_t now_realtime_ms() {
     timespec ts;
     clock_gettime(CLOCK_REALTIME, &ts);
diff --git a//home/user/Projects/cx-inline-comments/src/core/thread.h b//home/user/Projects/cx-inline-comments/src/core/thread.h
index 0fcfa17ba6e331309260b641ec21aa4a8ff9ba36..3cf72c1b96ca700f602e2e7c630a1e6970f9620f
--- a//home/user/Projects/cx-inline-comments/src/core/thread.h
+++ b//home/user/Projects/cx-inline-comments/src/core/thread.h
@@ -118,7 +118,7 @@
     ContextRoute,           // scatter/script/all-shard or conservative broad-owner route
     ContextKeymissNotify,   // MGET miss must retain owner-side notification behavior
     InflightWrite,
-    ArmTransient,           // pre-arming writes still in flight (DESIGN-RINGDIET.md)
+    ArmTransient,           // pre-arming writes still in flight
     AtomicPending,
     Missing,
     Typed,
@@ -151,7 +151,7 @@
     // ARM-ON-DEMAND TRANSIENT. Reads demoted because the connection carried writes published
     // BEFORE a local read armed it -- writes the RYOW ring deliberately never recorded. Bounded
     // per connection by one ROB drain and reported separately from the steady-state key conflict
-    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
+    // above so that the two can never be confused in a bench.
     uint64_t fallback_arm_transient = 0;
     uint64_t fallback_atomic_pending = 0;
     uint64_t fallback_missing = 0;
@@ -160,7 +160,7 @@
     uint64_t fallback_seq_churn = 0;
     uint64_t fallback_generation = 0;
     uint64_t fallback_lane_full = 0;
-    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
+    // Lane ADMISSION deferrals. Frames the armed parser left unconsumed at rpos, to be
     // re-parsed by a later pass of the same thread, because the local-read lane had no room
     // (defer_lane_full) or because the connection already held its fair share of a lane under
     // pressure (defer_quota). Neither is a fallback: the read still completes locally, and no

exec
/bin/bash -lc "python3 - <<'PY'
import json
from pathlib import Path
for p in sorted(Path('.').glob('.inline-*.json')):
    d=json.loads(p.read_text()); print(p, list(d))
PY" in /home/user/Projects/cx-inline-comments
 succeeded in 0ms:
.inline-commands.json ['entries', 'findings', 'preserve_notes']
.inline-core-loops.json ['entries', 'findings', 'preserve_notes', 'verification']
.inline-net-store.json ['references', 'findings', 'preserve_notes']
.inline-root.json ['entries', 'findings', 'preserve_notes']

diff --git a//home/user/Projects/cx-inline-comments/src/core/config.h b//home/user/Projects/cx-inline-comments/src/core/config.h
index 495e6ae42da15ee4bcc76a8a846efeef506903d7..8b5f3e5c786e8eeae0296a5dcf19338056d59007
--- a//home/user/Projects/cx-inline-comments/src/core/config.h
+++ b//home/user/Projects/cx-inline-comments/src/core/config.h
@@ -290,8 +290,8 @@
     uint32_t client_lb = 1;
 
     // ---- automatic role split (boot-latched) -----------------------------------------------
-    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer
-    // (DESIGN-flipfp.md); its measured sampling policy is internal to flipctl.h. Off and 1s
+    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer;
+    // its measured sampling policy is internal to flipctl.h. Off and 1s
     // dispatch pay one predicted branch per op and no fingerprint store. Trigger bands learn
     // from the anchor's own quiet jitter; no separate controller tuning knobs remain.
     uint32_t flip_auto = 0;
diff --git a//home/user/Projects/cx-inline-comments/src/core/flipctl.h b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
index 5185da4085ae5f2e3325702064208810cb9c42f7..00b11f3e799ae7313e3ce48ae074715e0bbb7968
--- a//home/user/Projects/cx-inline-comments/src/core/flipctl.h
+++ b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
@@ -19,7 +19,9 @@
 
 class Server;
 
-// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
+// Preserve the measured sampler: one whole parse pass in 100 on average. At a matched 512k ops/s,
+// a GET-only -> SET-only swap triggered the fingerprint detector once with both exhaustive and
+// 1-in-100 sampling; the sampled writer still detected the shift with the rate held fixed.
 // Arming derives solely from the controller latch. No runtime sampling knob or adaptive
 // per-operation work is needed; the detector already derives its bands from actual samples.
 inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
@@ -52,9 +54,11 @@
     uint64_t closed_windows = 0;
 };
 
-// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
-// frame -- one load of this writer's own word, on the line the old enabled() test already read,
-// and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
+// SAMPLED BY PARSE PASS. A per-op countdown would restore the store this sampler removes: no
+// existing io-side command counter covers both the point paths and all io-local commands.
+// The io loop asks pass_sampled() once per dispatched frame -- one load of this writer's own word,
+// on the line the old enabled() test already read, and one predicted-false branch; no store.
+// Inside a sampled pass the full fingerprint body runs
 // for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
 // that pass; at the pass end finish_parse_pass() publishes it and the caller draws the gap to the
 // next sampled pass. `work_window_` is the mean commands per fingerprint
diff --git a//home/user/Projects/cx-inline-comments/src/core/server.h b//home/user/Projects/cx-inline-comments/src/core/server.h
index f869dd3d1737afa6b1c401919fb66c96772b1225..a168d1656a19c4b098167904d42f814caae7513c
--- a//home/user/Projects/cx-inline-comments/src/core/server.h
+++ b//home/user/Projects/cx-inline-comments/src/core/server.h
@@ -8,8 +8,10 @@
 //
 // THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
 // dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
-// its derived shard-granularity fast path, published only at a quiesced commit. See
-// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
+// its derived shard-granularity fast path, published only at a quiesced commit. Shards move only
+// after executing, deferred, group and pinned-read work drains; destination routes publish after
+// the ownership edge. An IO may enqueue using an old route after that edge, so tasks and borrow
+// releases recheck ownership and forward before shard access instead of carrying routing epochs.
 #pragma once
 #include <algorithm>
 #include <atomic>
@@ -308,7 +310,7 @@
         for (uint32_t i = 0; i < nthreads; i++) {
             threads_[i] = std::make_unique<ThreadCtx>();
             // The fingerprint writer is armed only when its one reader, the flip controller, is
-            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
+            // enabled: with --flip-auto 0 and in 1s mode it is dark and costs
             // one predicted branch per op. The enabled sampler keeps its measured 1-in-100 policy.
             threads_[i]->init(i, placement_.role_of(i), nthreads,
                               0,
@@ -1969,7 +1971,7 @@
         shard_owner_[shard_id].store(thread_id, std::memory_order_release);
     }
 
-    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
+    // The caller must hold both executor loops at the safe point:
     // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
     // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
     // Vector capacity and membership are settled while PREPARING still names the source.  The
@@ -1988,7 +1990,7 @@
     // lost, a block ends up linked twice, and the damage surfaces later as either
     // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
     // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
-    // Both were observed (see DESIGN-P0REPLY.md).
+    // Both were observed.
     //
     // The rebind used to be deferred to the destination's own next executor pass
     // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
@@ -2827,7 +2829,7 @@
     // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
     // other command. It widens -- deterministically -- the window in which a younger whole-owner
     // walker could overtake an older same-connection group on the destination shard.
-    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
+    // TEST HOOK: effective local-read LANE CAPACITY for admission. 0 means
     // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
     // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
     // its masking, so this can never overrun anything. It exists because lane oversubscription is
@@ -2935,9 +2937,11 @@
     // is already set. Nothing on GET/SET touches it.
     //
     // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
-    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
-    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
-    // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
+    // sequence: a blocking op requires an empty ROB, then sets the barrier before any younger
+    // frame can be parsed; a resumed scatter inherits that same slot and barrier. The state the
+    // owner-scoped release exists to survive -- a release that must NOT drop the barrier -- has
+    // to be injected. It is held past ROB quiescence on purpose; barrier_release_quiesced()
+    // exempts this one bit for that reason.
     //
     // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
     // nothing left to do and parks on submit_and_wait(1) -- unbounded under io_uring. A hold that
@@ -2968,12 +2972,11 @@
         return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
     }
     // An overlap: some owner took the parse barrier while another owner already held it. With
-    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
-    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
-    // live and the owner-scoped release in blocking_retire() became load-bearing rather than
-    // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
-    // own positive control: a "must be zero" reading proves nothing until something has been shown
-    // able to make it non-zero.
+    // DEBUG BARRIER-HOLD off this must read ZERO -- if it ever moves on production traffic the
+    // latent case just went live and the owner-scoped release in blocking_retire() became
+    // load-bearing rather than defensive. With the latch ON it advances once per blocking dispatch,
+    // which is the counter's own positive control: a "must be zero" reading proves nothing until
+    // something has been shown able to make it non-zero.
     void note_barrier_overlap() {
         barrier_owner_overlaps_.fetch_add(1, std::memory_order_relaxed);
     }
diff --git a//home/user/Projects/cx-inline-comments/src/core/shard.h b//home/user/Projects/cx-inline-comments/src/core/shard.h
index 694b9a93c6cd63db49b9dcf061dbb5182c49986a..39a049d2f8619bbf52f706775956cc9198060364
--- a//home/user/Projects/cx-inline-comments/src/core/shard.h
+++ b//home/user/Projects/cx-inline-comments/src/core/shard.h
@@ -431,7 +431,7 @@
         // NOT share this counter: multi.inc's prepare_write_key() re-asks it at install time and
         // reports violations through INFO atomic_exec_order_late (multi_exec_order_late()), which
         // must read zero. Summing the two would hide a violation inside a number that is non-zero
-        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
+        // by design, leaving the park's safety argument with no falsifier.
         uint64_t atomic_exec_order_holds = 0;
         // Times watch_finalize_reservation() answered "not ready" because the reservation's epoch
         // was still 0, i.e. a unit was turned into a Retry by an undecided WATCH reservation. It
diff --git a//home/user/Projects/cx-inline-comments/src/core/signal.h b//home/user/Projects/cx-inline-comments/src/core/signal.h
index 933a5d7ee09538f3923df3c5eb77f9c72cdc0bfa..f69f7d73942ee129f4ad9cd1f23befe69fc4167d
--- a//home/user/Projects/cx-inline-comments/src/core/signal.h
+++ b//home/user/Projects/cx-inline-comments/src/core/signal.h
@@ -47,7 +47,7 @@
 // that will be compared against a deadline must come from here and never from now_ns() above --
 // that one is CLOCK_MONOTONIC, so its "milliseconds" are milliseconds since boot and sit roughly
 // five orders of magnitude below any real deadline. A monotonic value used as an expiry cut does
-// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
+// not skew the answer, it disables expiry outright.
 inline int64_t now_realtime_ms() {
     timespec ts;
     clock_gettime(CLOCK_REALTIME, &ts);
diff --git a//home/user/Projects/cx-inline-comments/src/core/thread.h b//home/user/Projects/cx-inline-comments/src/core/thread.h
index 0fcfa17ba6e331309260b641ec21aa4a8ff9ba36..3cf72c1b96ca700f602e2e7c630a1e6970f9620f
--- a//home/user/Projects/cx-inline-comments/src/core/thread.h
+++ b//home/user/Projects/cx-inline-comments/src/core/thread.h
@@ -118,7 +118,7 @@
     ContextRoute,           // scatter/script/all-shard or conservative broad-owner route
     ContextKeymissNotify,   // MGET miss must retain owner-side notification behavior
     InflightWrite,
-    ArmTransient,           // pre-arming writes still in flight (DESIGN-RINGDIET.md)
+    ArmTransient,           // pre-arming writes still in flight
     AtomicPending,
     Missing,
     Typed,
@@ -151,7 +151,7 @@
     // ARM-ON-DEMAND TRANSIENT. Reads demoted because the connection carried writes published
     // BEFORE a local read armed it -- writes the RYOW ring deliberately never recorded. Bounded
     // per connection by one ROB drain and reported separately from the steady-state key conflict
-    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
+    // above so that the two can never be confused in a bench.
     uint64_t fallback_arm_transient = 0;
     uint64_t fallback_atomic_pending = 0;
     uint64_t fallback_missing = 0;
@@ -160,7 +160,7 @@
     uint64_t fallback_seq_churn = 0;
     uint64_t fallback_generation = 0;
     uint64_t fallback_lane_full = 0;
-    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
+    // Lane ADMISSION deferrals. Frames the armed parser left unconsumed at rpos, to be
     // re-parsed by a later pass of the same thread, because the local-read lane had no room
     // (defer_lane_full) or because the connection already held its fair share of a lane under
     // pressure (defer_quota). Neither is a fallback: the read still completes locally, and no

exec
/bin/bash -lc "python3 - <<'PY'
import json, subprocess
from pathlib import Path
from collections import Counter
records=[]
for p in sorted(Path('.').glob('.inline-*.json')):
    d=json.loads(p.read_text()); records.extend(d.get('entries',d.get('references',[])))
records.sort(key=lambda e:(e['file'],e['original_line']))
refs=[]
for line in subprocess.check_output(['git','grep','-n',r'\\.md','HEAD','--','src'],text=True).splitlines():
    _,file,number,body=line.split(':',3)
    refs.append((file,int(number)))
assert len(refs)==66
assert len(records)==66
assert len({(e['file'],e['original_line']) for e in records})==66
assert sorted(refs)==[(e['file'],e['original_line']) for e in records]
print('PASS: every baseline reference appears exactly once in the inventory.')
print(Counter(e['action'] for e in records))
print('Notes:', ', '.join(sorted({e['note'] for e in records})))
PY" in /home/user/Projects/cx-inline-comments
 succeeded in 0ms:
PASS: every baseline reference appears exactly once in the inventory.
Counter({'DROP': 50, 'INLINE': 15, 'OUT_OF_SCOPE': 1})
Notes: AUDIT-MARKDIET.md, DESIGN-DEMOTEGATE.md, DESIGN-NOTIFY.md, DESIGN-P0REPLY.md, DESIGN-RINGDIET.md, DESIGN-flipfp.md, NOTES-BARRIER.md, NOTES-CLIMON2.md, NOTES-EVICT.md, NOTES-EXPWIDE.md, NOTES-MIGRATE.md, NOTES-MULTIRACE.md, NOTES-MULTIRES.md, NOTES-RECYCLE.md, NOTES-SCRIPTATOMIC.md, NOTES-SERVERTAIL.md, P128.md

diff --git a//home/user/Projects/cx-inline-comments/src/core/config.h b//home/user/Projects/cx-inline-comments/src/core/config.h
index 495e6ae42da15ee4bcc76a8a846efeef506903d7..8b5f3e5c786e8eeae0296a5dcf19338056d59007
--- a//home/user/Projects/cx-inline-comments/src/core/config.h
+++ b//home/user/Projects/cx-inline-comments/src/core/config.h
@@ -290,8 +290,8 @@
     uint32_t client_lb = 1;
 
     // ---- automatic role split (boot-latched) -----------------------------------------------
-    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer
-    // (DESIGN-flipfp.md); its measured sampling policy is internal to flipctl.h. Off and 1s
+    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer;
+    // its measured sampling policy is internal to flipctl.h. Off and 1s
     // dispatch pay one predicted branch per op and no fingerprint store. Trigger bands learn
     // from the anchor's own quiet jitter; no separate controller tuning knobs remain.
     uint32_t flip_auto = 0;
diff --git a//home/user/Projects/cx-inline-comments/src/core/flipctl.h b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
index 5185da4085ae5f2e3325702064208810cb9c42f7..00b11f3e799ae7313e3ce48ae074715e0bbb7968
--- a//home/user/Projects/cx-inline-comments/src/core/flipctl.h
+++ b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
@@ -19,7 +19,9 @@
 
 class Server;
 
-// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
+// Preserve the measured sampler: one whole parse pass in 100 on average. At a matched 512k ops/s,
+// a GET-only -> SET-only swap triggered the fingerprint detector once with both exhaustive and
+// 1-in-100 sampling; the sampled writer still detected the shift with the rate held fixed.
 // Arming derives solely from the controller latch. No runtime sampling knob or adaptive
 // per-operation work is needed; the detector already derives its bands from actual samples.
 inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
@@ -52,9 +54,11 @@
     uint64_t closed_windows = 0;
 };
 
-// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
-// frame -- one load of this writer's own word, on the line the old enabled() test already read,
-// and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
+// SAMPLED BY PARSE PASS. A per-op countdown would restore the store this sampler removes: no
+// existing io-side command counter covers both the point paths and all io-local commands.
+// The io loop asks pass_sampled() once per dispatched frame -- one load of this writer's own word,
+// on the line the old enabled() test already read, and one predicted-false branch; no store.
+// Inside a sampled pass the full fingerprint body runs
 // for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
 // that pass; at the pass end finish_parse_pass() publishes it and the caller draws the gap to the
 // next sampled pass. `work_window_` is the mean commands per fingerprint
diff --git a//home/user/Projects/cx-inline-comments/src/core/server.h b//home/user/Projects/cx-inline-comments/src/core/server.h
index f869dd3d1737afa6b1c401919fb66c96772b1225..a168d1656a19c4b098167904d42f814caae7513c
--- a//home/user/Projects/cx-inline-comments/src/core/server.h
+++ b//home/user/Projects/cx-inline-comments/src/core/server.h
@@ -8,8 +8,10 @@
 //
 // THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
 // dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
-// its derived shard-granularity fast path, published only at a quiesced commit. See
-// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
+// its derived shard-granularity fast path, published only at a quiesced commit. Shards move only
+// after executing, deferred, group and pinned-read work drains; destination routes publish after
+// the ownership edge. An IO may enqueue using an old route after that edge, so tasks and borrow
+// releases recheck ownership and forward before shard access instead of carrying routing epochs.
 #pragma once
 #include <algorithm>
 #include <atomic>
@@ -308,7 +310,7 @@
         for (uint32_t i = 0; i < nthreads; i++) {
             threads_[i] = std::make_unique<ThreadCtx>();
             // The fingerprint writer is armed only when its one reader, the flip controller, is
-            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
+            // enabled: with --flip-auto 0 and in 1s mode it is dark and costs
             // one predicted branch per op. The enabled sampler keeps its measured 1-in-100 policy.
             threads_[i]->init(i, placement_.role_of(i), nthreads,
                               0,
@@ -1969,7 +1971,7 @@
         shard_owner_[shard_id].store(thread_id, std::memory_order_release);
     }
 
-    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
+    // The caller must hold both executor loops at the safe point:
     // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
     // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
     // Vector capacity and membership are settled while PREPARING still names the source.  The
@@ -1988,7 +1990,7 @@
     // lost, a block ends up linked twice, and the damage surfaces later as either
     // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
     // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
-    // Both were observed (see DESIGN-P0REPLY.md).
+    // Both were observed.
     //
     // The rebind used to be deferred to the destination's own next executor pass
     // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
@@ -2827,7 +2829,7 @@
     // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
     // other command. It widens -- deterministically -- the window in which a younger whole-owner
     // walker could overtake an older same-connection group on the destination shard.
-    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
+    // TEST HOOK: effective local-read LANE CAPACITY for admission. 0 means
     // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
     // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
     // its masking, so this can never overrun anything. It exists because lane oversubscription is
@@ -2935,9 +2937,11 @@
     // is already set. Nothing on GET/SET touches it.
     //
     // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
-    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
-    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
-    // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
+    // sequence: a blocking op requires an empty ROB, then sets the barrier before any younger
+    // frame can be parsed; a resumed scatter inherits that same slot and barrier. The state the
+    // owner-scoped release exists to survive -- a release that must NOT drop the barrier -- has
+    // to be injected. It is held past ROB quiescence on purpose; barrier_release_quiesced()
+    // exempts this one bit for that reason.
     //
     // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
     // nothing left to do and parks on submit_and_wait(1) -- unbounded under io_uring. A hold that
@@ -2968,12 +2972,11 @@
         return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
     }
     // An overlap: some owner took the parse barrier while another owner already held it. With
-    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
-    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
-    // live and the owner-scoped release in blocking_retire() became load-bearing rather than
-    // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
-    // own positive control: a "must be zero" reading proves nothing until something has been shown
-    // able to make it non-zero.
+    // DEBUG BARRIER-HOLD off this must read ZERO -- if it ever moves on production traffic the
+    // latent case just went live and the owner-scoped release in blocking_retire() became
+    // load-bearing rather than defensive. With the latch ON it advances once per blocking dispatch,
+    // which is the counter's own positive control: a "must be zero" reading proves nothing until
+    // something has been shown able to make it non-zero.
     void note_barrier_overlap() {
         barrier_owner_overlaps_.fetch_add(1, std::memory_order_relaxed);
     }
diff --git a//home/user/Projects/cx-inline-comments/src/core/shard.h b//home/user/Projects/cx-inline-comments/src/core/shard.h
index 694b9a93c6cd63db49b9dcf061dbb5182c49986a..39a049d2f8619bbf52f706775956cc9198060364
--- a//home/user/Projects/cx-inline-comments/src/core/shard.h
+++ b//home/user/Projects/cx-inline-comments/src/core/shard.h
@@ -431,7 +431,7 @@
         // NOT share this counter: multi.inc's prepare_write_key() re-asks it at install time and
         // reports violations through INFO atomic_exec_order_late (multi_exec_order_late()), which
         // must read zero. Summing the two would hide a violation inside a number that is non-zero
-        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
+        // by design, leaving the park's safety argument with no falsifier.
         uint64_t atomic_exec_order_holds = 0;
         // Times watch_finalize_reservation() answered "not ready" because the reservation's epoch
         // was still 0, i.e. a unit was turned into a Retry by an undecided WATCH reservation. It
diff --git a//home/user/Projects/cx-inline-comments/src/core/signal.h b//home/user/Projects/cx-inline-comments/src/core/signal.h
index 933a5d7ee09538f3923df3c5eb77f9c72cdc0bfa..f69f7d73942ee129f4ad9cd1f23befe69fc4167d
--- a//home/user/Projects/cx-inline-comments/src/core/signal.h
+++ b//home/user/Projects/cx-inline-comments/src/core/signal.h
@@ -47,7 +47,7 @@
 // that will be compared against a deadline must come from here and never from now_ns() above --
 // that one is CLOCK_MONOTONIC, so its "milliseconds" are milliseconds since boot and sit roughly
 // five orders of magnitude below any real deadline. A monotonic value used as an expiry cut does
-// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
+// not skew the answer, it disables expiry outright.
 inline int64_t now_realtime_ms() {
     timespec ts;
     clock_gettime(CLOCK_REALTIME, &ts);
diff --git a//home/user/Projects/cx-inline-comments/src/core/thread.h b//home/user/Projects/cx-inline-comments/src/core/thread.h
index 0fcfa17ba6e331309260b641ec21aa4a8ff9ba36..3cf72c1b96ca700f602e2e7c630a1e6970f9620f
--- a//home/user/Projects/cx-inline-comments/src/core/thread.h
+++ b//home/user/Projects/cx-inline-comments/src/core/thread.h
@@ -118,7 +118,7 @@
     ContextRoute,           // scatter/script/all-shard or conservative broad-owner route
     ContextKeymissNotify,   // MGET miss must retain owner-side notification behavior
     InflightWrite,
-    ArmTransient,           // pre-arming writes still in flight (DESIGN-RINGDIET.md)
+    ArmTransient,           // pre-arming writes still in flight
     AtomicPending,
     Missing,
     Typed,
@@ -151,7 +151,7 @@
     // ARM-ON-DEMAND TRANSIENT. Reads demoted because the connection carried writes published
     // BEFORE a local read armed it -- writes the RYOW ring deliberately never recorded. Bounded
     // per connection by one ROB drain and reported separately from the steady-state key conflict
-    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
+    // above so that the two can never be confused in a bench.
     uint64_t fallback_arm_transient = 0;
     uint64_t fallback_atomic_pending = 0;
     uint64_t fallback_missing = 0;
@@ -160,7 +160,7 @@
     uint64_t fallback_seq_churn = 0;
     uint64_t fallback_generation = 0;
     uint64_t fallback_lane_full = 0;
-    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
+    // Lane ADMISSION deferrals. Frames the armed parser left unconsumed at rpos, to be
     // re-parsed by a later pass of the same thread, because the local-read lane had no room
     // (defer_lane_full) or because the connection already held its fair share of a lane under
     // pressure (defer_quota). Neither is a fallback: the read still completes locally, and no

exec
/bin/bash -lc "sed -n '153,193p' /home/user/Projects/lane-notes-archive/NOTES-RECYCLE.md
sed -n '32,49p' /home/user/Projects/lane-notes-archive/NOTES-EVICT.md
sed -n '160,193p' /home/user/Projects/lane-notes-archive/NOTES-MULTIRES.md
sed -n '235,279p' /home/user/Projects/lane-notes-archive/NOTES-SERVERTAIL.md" in /home/user/Projects/cx-inline
 succeeded in 0ms:
| SET overwrite, klen 40 | 1.0000 → **0.0000** | 1.0000 → **0.0000** | 4089.7 → 3975.6 (**−114.1**) | 4127.6 → 3996.9 (**−130.7**) |
| SET first insert, klen 16 | 1.0000 → 1.0000 | 0.0000 → 0.0000 | 4255.9 → 4247.9 (−8.0) | 4261.4 → 4258.8 (−2.6) |
| SET first insert, klen 24 | 1.0000 → 1.0000 | 0.0000 → 0.0000 | 4282.7 → 4273.0 (−9.7) | 4276.3 → 4270.2 (−6.1) |
| SET first insert, klen 40 | 1.0000 → 1.0000 | 0.0000 → 0.0000 | 4330.4 → 4326.6 (−3.8) | 4319.8 → 4329.3 (**+9.5**) |

**The adverse cell, stated plainly.** First insert is where the cache cannot help: nothing has been
retired yet, `mem_block_cache` reads 0, every write allocates, and the change is a lookup that
always misses. Across the two runs the six first-insert deltas are −8.0, −9.7, −3.8, −2.6, −6.1 and
**+9.5** — they straddle zero, and the largest adverse one is 0.22% of a 4300-instruction op. The
overwrite deltas never straddle zero: −93.7 to −130.7, i.e. −2.3% to −3.2%, six for six.
`nallocx/op` is 0.0000 everywhere, so the closed-form `good_size` is what runs, not a PLT call.

### Throughput and cycles, per-owner cache (400k ops, 3 reps, interleaved arms)

| shards | base kops/s | recycle kops/s | Δ | base instr/op | recycle instr/op | base cyc/op | recycle cyc/op |
|---|---|---|---|---|---|---|---|
| 1  | 2031.2 | 2040.6 | +0.5% | 3262.3 | 3152.4 | 1000.0 | 980.8 |
| 8  | 1989.8 | 2015.7 | +1.3% | 3280.1 | 3169.8 | 1010.7 | 995.5 |
| 64 | 1932.3 | 1975.2 | +2.2% | 3368.3 | 3258.5 | 1065.3 | 1040.1 |

Interleaved (base, recycle, base, recycle, ... one rep each, 64 shards) to rule out box drift
between runs: kops/s 1945.2/1953.7, 1962.5/1974.9, 2025.2/2017.0 — recycle ahead in two pairs of
three, and the absolute level drifts 4% across the six runs, which is why the wall-clock column is
the weak instrument here. This is a single-connection geometry and is loadgen-limited; instr/op is
the strong instrument and it is flat to the fourth digit across every rep. A saturated
multi-connection bench is owed (§11).

### Why the per-class cap is 4096 and not 32 (64 shards, 400k ops, 2 reps)

| per-class cap | pipeline | mallocx/op | instr/op | mem_block_cache |
|---|---|---|---|---|
| 32   | 64  | 0.5001 | 3336.3 | 2048 B |
| 4096 | 64  | **0.0000** | 3271.9 | 4096 B |
| 32   | 256 | 0.3750 | 3163.6 | 2048 B |
| 4096 | 256 | **0.0000** | 3115.6 | 6144 B |

A 32-node cap saves 2–4 KB per owner thread and gives back 40–60% of the mechanism. The bound
that matters is the byte cap, and the drain-sized per-class cap is what makes the reuse rate 100%.

## 7. Every drain / lifecycle path, traced

used_s = FlatStore::obj_bytes_ + 12 * live_keys_s
limit_s = maxmemory / nshards
```

`obj_bytes_` is the existing maintained sum of allocator-class-rounded `KvObj` bytes plus external
value allocations. Replacement admission subtracts the displaced object's exact maintained size;
a genuinely new key adds 12 bytes.

The 12-byte slot estimate is `ceil(8 / 0.70)`: an eight-byte slot divided by FlatStore's 70% target
load. It intentionally does not chase current capacity, tombstones, the transient second table
during incremental rehash, allocator metadata, expiry-index capacity, or retained zero-copy
borrows. Those costs would make enforcement depend on resize history rather than the maintained
object/key counters. Thus maxmemory is a stable logical cache budget, not an RSS ceiling.

Each shard enforces the integer-divided share independently. This makes the decision lock-free and
single-writer, but it is intentionally conservative under skew: one hot/large shard can return OOM
while other shards have unused shares. Division remainder bytes are unused. With `maxmemory <
nshards`, every enabled shard share is zero, so writes cannot fit. `maxmemory=0` is distinct and
## 5. Two fixes that were wrong, and why (do not retry them)

Both were built, measured, and rejected. They are recorded because each looks obviously right.

**(a) Hold the EXEC write behind its own older group — *in `prepare_write_key()`*.**
(SUPERSEDED IN PART: a hold at the other end of the path, in `ExLoop::execute()`'s tagged-
MULTI dispatch *before* the fragment has installed anything on that owner, is acyclic and
shipped in `t-multirace`; see NOTES-MULTIRACE.md §5. What follows remains true of the
mid-command placement, and of any own-unit test that names only one epoch word.) Every other write path already takes this
hold: `ex_loop.h::execute()` runs `has_parked_predecessor()` / `xshard_task_should_defer()` ahead
of ordinary and scatter tasks, but it dispatches a `multi_task_tagged()` task at the top of the
function and returns, so a transaction passes **no** such check. Adding the check in
`prepare_write_key()` did remove every loss — and **deadlocked at one connection with no WATCH**
(0 rounds completed, vs 300 for base). Two independent reasons:

1. A cross-shard child command *inside* the same EXEC (`DEL k1 k2` in a MULTI) installs through
   the scatter path and so carries a **non-null** `group` pointer, while the transaction's plain
   installs carry a null one. `atomic_has_own_undecided()` discriminates on the group pointer, so
   the transaction held against itself. Excluding by the transaction's **epoch word** instead
   (both shapes publish through `&state.epoch`) fixed that much.
2. It still deadlocked, because the gate is not order-aware. A transaction installs on some shards
   and holds on others; the older group's fragment then defers behind the transaction's *partial*
   installs via `atomic_group_has_own_undecided()`, which does not compare op ids. Today that is
   harmless only because the transaction never waits. Making it wait closes the cycle. The
   deadlock-freedom argument for the existing gate rests on the `atomic_hazard` bit — the oldest
   group never waits — and a transaction has no equivalent.

**(b) Ship the collapse repair alone and drop the resolver repair.** The resolver hunk costs ~2 %
and appeared to buy nothing: the stale-read probe scored 0/400 with and without it. That
measurement was taken with the target on **16** cores, where the owner's post-batch cleanup always
beat the read to the pair. Re-measured with the target correctly pinned to **8** cores, the read
wins often enough that collapse-alone failed `tests/multires.py` in **3 of 8** runs while
collapse+resolver passes **10 of 10**. The resolver hunk is load-bearing; the earlier reading was
a pinning artifact. (Thanks to the coordinator's pinning correction, which is what exposed it.)
standalone, no-replica case, including
`ERR WAITAOF cannot be used when numlocal is set but appendonly is disabled.`,
`ERR value is out of range, value must between 0 and 1`,
`ERR value is out of range, must be positive`, and `ERR timeout is negative`.

**What does not:** the `numlocal == 1` local-fsync wait, or Redis's per-connection fsynced-local
count in the first element of a `numlocal == 0` reply. The former returns an explicit error naming
the gap; the latter remains the conservative `0` rather than a plausible server-global number.

**Why.** A connection-local handler runs at PARSE time, before the ops ahead of it on the same
connection have executed, so the AOF sequence it could sample does not yet cover the caller's own
writes. Waiting synchronously cannot fix that: retiring those older ops requires this very IO
thread, so the wait would deadlock against them. A conservative "check once and report 0" would
never claim durability we lack, but it would silently under-report, and the shelve rule prefers a
loud gap to a quiet one. The same ordering issue prevents an exact `numlocal == 0` first element:
Redis tracks the calling connection's last write offset. A server-global posted/durable comparison
would under-report a reader when another connection has an unsynced write, while a global fsync
counter would over-report a writer whose newest write is not covered.

**The design it needs**, for whoever picks it up:
1. Register `WAITAOF` as `ConnLocal | PubSub` and dispatch it by name in `pubsub_start_command`,
   returning `PubSubStartResult::Async`. `io_loop.h` already handles `Async` by publishing the ROB
   slot without storing `Done` — that is the deferral primitive, and `CLIENT LIST`'s scatter
   (`climon.cc:58-92` start, `:258-280` finish) is the worked example.
2. Keep a per-IoLoop pending map keyed by connection id, mirroring `climon_pending_`.
3. Drain it once per loop pass: wait until the connection's older ops have retired
   (`rob().flush_id()` past the WAITAOF op id), then force each owning producer to post
   (`AofProducer::flush`) so the sequence covers the caller's writes, capture
   `AofManager::posted_sequence()`, and poll for durability against the deadline.
4. `AofManager` needs a policy-independent durable accessor. `reply_gate_ready()` is a *reply*
   safety gate, not a durability gate — under `appendfsync no` it always returns true, and under
   `everysec` it compares against `written_sequence_` (page cache), not `durable_sequence_`. Add
   `durable_sequence()` or a `durable_gate_ready(target)` beside it.
5. Test under **both** persist-io engines: under `normal` a write can reach durable synchronously
   inside one `writer_pass`, while under `uring` each frontier step needs a CQE.

---

## 9. Test evidence

Server cores 104-107, port 7280; oracle (vanilla redis 7.4) cores 108-111, port 7285.

```
servertail: 101 checks, 0 failures -> PASS
slowlog:     60 checks, 0 failures -> PASS

diff --git a//home/user/Projects/cx-inline-comments/src/core/config.h b//home/user/Projects/cx-inline-comments/src/core/config.h
index 495e6ae42da15ee4bcc76a8a846efeef506903d7..8b5f3e5c786e8eeae0296a5dcf19338056d59007
--- a//home/user/Projects/cx-inline-comments/src/core/config.h
+++ b//home/user/Projects/cx-inline-comments/src/core/config.h
@@ -290,8 +290,8 @@
     uint32_t client_lb = 1;
 
     // ---- automatic role split (boot-latched) -----------------------------------------------
-    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer
-    // (DESIGN-flipfp.md); its measured sampling policy is internal to flipctl.h. Off and 1s
+    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer;
+    // its measured sampling policy is internal to flipctl.h. Off and 1s
     // dispatch pay one predicted branch per op and no fingerprint store. Trigger bands learn
     // from the anchor's own quiet jitter; no separate controller tuning knobs remain.
     uint32_t flip_auto = 0;
diff --git a//home/user/Projects/cx-inline-comments/src/core/flipctl.h b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
index 5185da4085ae5f2e3325702064208810cb9c42f7..00b11f3e799ae7313e3ce48ae074715e0bbb7968
--- a//home/user/Projects/cx-inline-comments/src/core/flipctl.h
+++ b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
@@ -19,7 +19,9 @@
 
 class Server;
 
-// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
+// Preserve the measured sampler: one whole parse pass in 100 on average. At a matched 512k ops/s,
+// a GET-only -> SET-only swap triggered the fingerprint detector once with both exhaustive and
+// 1-in-100 sampling; the sampled writer still detected the shift with the rate held fixed.
 // Arming derives solely from the controller latch. No runtime sampling knob or adaptive
 // per-operation work is needed; the detector already derives its bands from actual samples.
 inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
@@ -52,9 +54,11 @@
     uint64_t closed_windows = 0;
 };
 
-// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
-// frame -- one load of this writer's own word, on the line the old enabled() test already read,
-// and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
+// SAMPLED BY PARSE PASS. A per-op countdown would restore the store this sampler removes: no
+// existing io-side command counter covers both the point paths and all io-local commands.
+// The io loop asks pass_sampled() once per dispatched frame -- one load of this writer's own word,
+// on the line the old enabled() test already read, and one predicted-false branch; no store.
+// Inside a sampled pass the full fingerprint body runs
 // for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
 // that pass; at the pass end finish_parse_pass() publishes it and the caller draws the gap to the
 // next sampled pass. `work_window_` is the mean commands per fingerprint
diff --git a//home/user/Projects/cx-inline-comments/src/core/server.h b//home/user/Projects/cx-inline-comments/src/core/server.h
index f869dd3d1737afa6b1c401919fb66c96772b1225..a168d1656a19c4b098167904d42f814caae7513c
--- a//home/user/Projects/cx-inline-comments/src/core/server.h
+++ b//home/user/Projects/cx-inline-comments/src/core/server.h
@@ -8,8 +8,10 @@
 //
 // THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
 // dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
-// its derived shard-granularity fast path, published only at a quiesced commit. See
-// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
+// its derived shard-granularity fast path, published only at a quiesced commit. Shards move only
+// after executing, deferred, group and pinned-read work drains; destination routes publish after
+// the ownership edge. An IO may enqueue using an old route after that edge, so tasks and borrow
+// releases recheck ownership and forward before shard access instead of carrying routing epochs.
 #pragma once
 #include <algorithm>
 #include <atomic>
@@ -308,7 +310,7 @@
         for (uint32_t i = 0; i < nthreads; i++) {
             threads_[i] = std::make_unique<ThreadCtx>();
             // The fingerprint writer is armed only when its one reader, the flip controller, is
-            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
+            // enabled: with --flip-auto 0 and in 1s mode it is dark and costs
             // one predicted branch per op. The enabled sampler keeps its measured 1-in-100 policy.
             threads_[i]->init(i, placement_.role_of(i), nthreads,
                               0,
@@ -1969,7 +1971,7 @@
         shard_owner_[shard_id].store(thread_id, std::memory_order_release);
     }
 
-    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
+    // The caller must hold both executor loops at the safe point:
     // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
     // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
     // Vector capacity and membership are settled while PREPARING still names the source.  The
@@ -1988,7 +1990,7 @@
     // lost, a block ends up linked twice, and the damage surfaces later as either
     // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
     // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
-    // Both were observed (see DESIGN-P0REPLY.md).
+    // Both were observed.
     //
     // The rebind used to be deferred to the destination's own next executor pass
     // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
@@ -2827,7 +2829,7 @@
     // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
     // other command. It widens -- deterministically -- the window in which a younger whole-owner
     // walker could overtake an older same-connection group on the destination shard.
-    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
+    // TEST HOOK: effective local-read LANE CAPACITY for admission. 0 means
     // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
     // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
     // its masking, so this can never overrun anything. It exists because lane oversubscription is
@@ -2935,9 +2937,11 @@
     // is already set. Nothing on GET/SET touches it.
     //
     // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
-    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
-    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
-    // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
+    // sequence: a blocking op requires an empty ROB, then sets the barrier before any younger
+    // frame can be parsed; a resumed scatter inherits that same slot and barrier. The state the
+    // owner-scoped release exists to survive -- a release that must NOT drop the barrier -- has
+    // to be injected. It is held past ROB quiescence on purpose; barrier_release_quiesced()
+    // exempts this one bit for that reason.
     //
     // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
     // nothing left to do and parks on submit_and_wait(1) -- unbounded under io_uring. A hold that
@@ -2968,12 +2972,11 @@
         return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
     }
     // An overlap: some owner took the parse barrier while another owner already held it. With
-    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
-    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
-    // live and the owner-scoped release in blocking_retire() became load-bearing rather than
-    // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
-    // own positive control: a "must be zero" reading proves nothing until something has been shown
-    // able to make it non-zero.
+    // DEBUG BARRIER-HOLD off this must read ZERO -- if it ever moves on production traffic the
+    // latent case just went live and the owner-scoped release in blocking_retire() became
+    // load-bearing rather than defensive. With the latch ON it advances once per blocking dispatch,
+    // which is the counter's own positive control: a "must be zero" reading proves nothing until
+    // something has been shown able to make it non-zero.
     void note_barrier_overlap() {
         barrier_owner_overlaps_.fetch_add(1, std::memory_order_relaxed);
     }
diff --git a//home/user/Projects/cx-inline-comments/src/core/shard.h b//home/user/Projects/cx-inline-comments/src/core/shard.h
index 694b9a93c6cd63db49b9dcf061dbb5182c49986a..39a049d2f8619bbf52f706775956cc9198060364
--- a//home/user/Projects/cx-inline-comments/src/core/shard.h
+++ b//home/user/Projects/cx-inline-comments/src/core/shard.h
@@ -431,7 +431,7 @@
         // NOT share this counter: multi.inc's prepare_write_key() re-asks it at install time and
         // reports violations through INFO atomic_exec_order_late (multi_exec_order_late()), which
         // must read zero. Summing the two would hide a violation inside a number that is non-zero
-        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
+        // by design, leaving the park's safety argument with no falsifier.
         uint64_t atomic_exec_order_holds = 0;
         // Times watch_finalize_reservation() answered "not ready" because the reservation's epoch
         // was still 0, i.e. a unit was turned into a Retry by an undecided WATCH reservation. It
diff --git a//home/user/Projects/cx-inline-comments/src/core/signal.h b//home/user/Projects/cx-inline-comments/src/core/signal.h
index 933a5d7ee09538f3923df3c5eb77f9c72cdc0bfa..f69f7d73942ee129f4ad9cd1f23befe69fc4167d
--- a//home/user/Projects/cx-inline-comments/src/core/signal.h
+++ b//home/user/Projects/cx-inline-comments/src/core/signal.h
@@ -47,7 +47,7 @@
 // that will be compared against a deadline must come from here and never from now_ns() above --
 // that one is CLOCK_MONOTONIC, so its "milliseconds" are milliseconds since boot and sit roughly
 // five orders of magnitude below any real deadline. A monotonic value used as an expiry cut does
-// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
+// not skew the answer, it disables expiry outright.
 inline int64_t now_realtime_ms() {
     timespec ts;
     clock_gettime(CLOCK_REALTIME, &ts);
diff --git a//home/user/Projects/cx-inline-comments/src/core/thread.h b//home/user/Projects/cx-inline-comments/src/core/thread.h
index 0fcfa17ba6e331309260b641ec21aa4a8ff9ba36..3cf72c1b96ca700f602e2e7c630a1e6970f9620f
--- a//home/user/Projects/cx-inline-comments/src/core/thread.h
+++ b//home/user/Projects/cx-inline-comments/src/core/thread.h
@@ -118,7 +118,7 @@
     ContextRoute,           // scatter/script/all-shard or conservative broad-owner route
     ContextKeymissNotify,   // MGET miss must retain owner-side notification behavior
     InflightWrite,
-    ArmTransient,           // pre-arming writes still in flight (DESIGN-RINGDIET.md)
+    ArmTransient,           // pre-arming writes still in flight
     AtomicPending,
     Missing,
     Typed,
@@ -151,7 +151,7 @@
     // ARM-ON-DEMAND TRANSIENT. Reads demoted because the connection carried writes published
     // BEFORE a local read armed it -- writes the RYOW ring deliberately never recorded. Bounded
     // per connection by one ROB drain and reported separately from the steady-state key conflict
-    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
+    // above so that the two can never be confused in a bench.
     uint64_t fallback_arm_transient = 0;
     uint64_t fallback_atomic_pending = 0;
     uint64_t fallback_missing = 0;
@@ -160,7 +160,7 @@
     uint64_t fallback_seq_churn = 0;
     uint64_t fallback_generation = 0;
     uint64_t fallback_lane_full = 0;
-    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
+    // Lane ADMISSION deferrals. Frames the armed parser left unconsumed at rpos, to be
     // re-parsed by a later pass of the same thread, because the local-read lane had no room
     // (defer_lane_full) or because the connection already held its fair share of a lane under
     // pressure (defer_quota). Neither is a fallback: the read still completes locally, and no

exec
/bin/bash -lc "python3 - <<'PY'
import json
from pathlib import Path
records=[]
for p in sorted(Path('.').glob('.inline-*.json')):
    d=json.loads(p.read_text()); records.extend(d.get('entries',d.get('references',[])))
records.sort(key=lambda e:(e['file'],e['original_line']))
def cell(s):
    return str(s).replace('|','&#124;').replace('\\n',' ')
lines=['# INLINE — archived source-comment references','',
'Base: "'`bebfde299` (`cx-inline`). Worktree: `/home/user/Projects/cx-inline-comments`, branch `codex/inline-archived-notes`. All source locations below refer to that base, before comment insertion changes line numbers.'"','',
'The inventory contains **66 references: 65 comments and one executable error string**. Decisions: **15 INLINE, 50 DROP, one OUT OF SCOPE**. All 65 comment pointers are removed. DROP means the cited passage was read and its load-bearing reasoning was already present; it does not mean the note was discarded unread.','',
'Archive consulted: "'`/home/user/Projects/lane-notes-archive/`. Seventeen distinct notes were cited. Sixteen are present there; `AUDIT-MARKDIET.md` is missing and was read from `33a17682a:AUDIT-MARKDIET.md`, §4, lines 174–187, using Git history. `NOTES-MDIET.md` concerns small collections and is not a substitute.'"','',
'Historical measurements retain their stated workload and meaning. In particular, the 512k ops/s fingerprint swap is evidence from the detector revision measured in "'`DESIGN-flipfp.md` §5.2, not a new measurement of this checkout.'"','',
'## Every reference','',
'| Original source location | Decision | Cited note / passage read | Claim carried across, or reason for dropping |',
'| --- | --- | --- | --- |']
for e in records:
    lines.append('| "'`'"'+e['file']+':'+str(e['original_line'])+'"'` | '"'+e['action'].replace('_',' ')+' | "'`'"'+e['note']+'"'` — '"'+cell(e['archive_evidence'])+' | '+cell(e['claim'])+' |')
lines += ['', '## Constraint exception','',
'"'`src/cmd/server_tail.cc:212` contains `NOTES-SERVERTAIL.md` inside the WAITAOF error-string literal. Removing it changes an executable statement and a client-visible reply, so it remains byte-for-byte unchanged. This is the only `.md` reference remaining anywhere under `src/`; it is included in the inventory above rather than silently skipped.'"','',
'## Accuracy findings','',
'These pre-existing statements surfaced while reading the cited reasoning. No implementation was changed. Existing complete comments were retained, except that the inaccurate recycler measurement was replaced by the exact cited table as part of inlining.','',
'| Original source location | Finding and evidence |',
'| --- | --- |',
'| "'`src/core/server.h:2937`; `src/net/conn.h:84`; `src/cmd/blocking.inc:1350` | The historical six-owner wording, including the hypothetical “seventh owner,” is stale. `BarrierOwner` already has seven production bits, including `Sleep`, plus the debug bit (`conn.h:94–109`). The blocking-operation exclusion argument still applies. |'"',
'| "'`src/core/ex_loop.h:278` | The claim that the quota guarantees aggregate admission within capacity and progress for every connection in every rotation is too broad. `max(1, cap / live)` cannot establish it when `live > cap`: cap 16 and 17 active connections permit one each but only 16 fit. `P128.md` §7.1 repeats the invalid bound and separately acknowledges the greater-than-capacity residual case. |'"',
'| "'`src/core/ex_loop.h:313` | The retained comment says +3.4% same-CCX fills. `P128.md` §9.5 prints 9.19 → 9.525, approximately +3.6%. Its +11% DRAM figure agrees. |'"',
'| "'`src/cmd/slowlog.cc:10`; `src/core/ex_loop.h:2310` | An unpipelined connection does not imply a one-op executor batch: `drain_tasks()` batches tasks across connections. Multiple depth-1 clients can share a batch, so the universal exact-timing claim is unsupported. |'"',
'| "'`src/cmd/slowlog.cc:19` | A single slow operation in a multi-op batch does not necessarily arm escalation: the screen compares the per-op batch average with the threshold. Exact timing also lasts only the next 64 batches, so an arbitrary later recurrence is not guaranteed exact. The new `slowlog.h` comment states those bounds. |'"',
'| "'`src/cmd/server_tail.cc:172` | The claim that every `numlocal == 0` WAITAOF reply is Redis-exact is false. The implementation emits `[0, 0]`; the archive (§§1, 8) explicitly calls the local element conservative because there is no per-connection last-write offset. Only the replica count is exact. |'"',
'| "'`src/net/rob.h:561`; `src/net/rob.h:809` | The arming comments say the nonzero states end for good or occur at most once. `mark_current_write()` resets to Unarmed after sidecar allocation failure (`rob.h:491`), allowing repeated arming. The archive explicitly describes that self-healing path. |'"',
'| "'`src/store/read_local_settax.h:10` | The cache is called the shard’s own, but it belongs to the fused owner and is shared by that owner’s shards through `retire_sink.block_cache`; see `flatstore_atomic.inc:1434` and `NOTES-RECYCLE.md` §4. |'"',
'| "'`src/store/flatstore_atomic.inc:1466` | The cache-release comment says release happens before eviction and calls both triggers whole-server events. Budget refusal releases after noeviction or failed/bounded eviction, using a shard-local budget decision; successful eviction retains the cache. See `flatstore.h:1671`, `flatstore.h:2470`, and `NOTES-RECYCLE.md` §5. |'"',
'| "'`src/store/kv_block_cache.h:70` | The original claim that cap 32 leaves only a few percent reuse is not what the cited table measures. It reports 0.5001/0.3750 mallocx calls per op (roughly 50–62.5% reuse), against 0.0000 at cap 4096. The inline comment now carries the exact table and 2–4 KiB memory difference. These are allocator-call measurements, not a throughput claim. |'"',
'| "'`src/cmd/blocking.inc:1347` | Its numeric `io_loop.h:1391` pointer is stale; the blocking in-flight guard is around base `src/core/io_loop.h:3953`. The invariant remains correct. |'"',
'', '## Archived material worth retaining outside these comments','',
'These are retention candidates for the maintainer, not proposals to restore working notes to version control. No archive files were copied into the repository. Historical prototype text and superseded conclusions need their later corrections kept beside them.','',
'| Archived note | Material that does not fit a local comment |',
'| --- | --- |',
'| "'`NOTES-MIGRATE.md` | The bucket, connection and role-transfer state machines and in-flight case tables (§§1–3), including cancellation/drain and stale-route cases across multiple files. |'"',
'| "'`DESIGN-P0REPLY.md` | The transfer/rebind event trace, both observed corruption signatures, negative controls and ownership-edge census (§§7–10). Its early exclusion of ownership handoff is superseded by the later root cause. |'"',
'| "'`DESIGN-flipfp.md` | Matched-rate PRE/POST/null tables, isolated detector-liveness controls and sampled-count/deafness analysis (§§5–6), with historical-detector and low-rate qualifications. The inline swap is only one measured cell. |'"',
'| "'`DESIGN-RINGDIET.md` | The full ABBA/null results, saturation corrections, interference qualifications and counter-based proof of zero pure-write bookkeeping. |'"',
'| "'`NOTES-BARRIER.md` | The setter/releaser census, rejected interleavings and owner-count/quiescence alternatives (§§1–4), plus the injector, explicit wakeup and positive-control design (§5). Its six-owner inventory is historical. |'"',
'| "'`P128.md` | The superseded quota variants, starvation analysis, pre/post/no-quota rate/cycles/IPC/tail tables, dormant-field cache-fill regression and final acceptance results (§§5, 7, 9–10). The above-capacity fairness proof needs correction before reuse. |'"',
'| "'`DESIGN-NOTIFY.md` | The complete wake-order argument across batches, teardown/slot reuse, migration and independent observers (§4). Distinguish the separate per-producer design in §3 from the current implementation. |'"',
'| "'`NOTES-MULTIRES.md` | The reduced reproduction, both rejected fixes, and the geometry correction: collapse-only appeared sufficient at 16 cores but failed 3/8 runs at 8 cores, while collapse plus resolver passed 10/10 (§§2–6). |'"',
'| "'`NOTES-MULTIRACE.md` | The complete dispatch-time acyclicity argument, separate hold/late falsifiers, and residual two-phase-unit liveness test rationale (§5), beyond the short mid-command cycle. |'"',
'| "'`NOTES-RECYCLE.md` | Per-shard versus per-owner evidence, allocator-call/cycle tables, logical/physical accounting decision and complete lifecycle/migration matrix (§§4–7). |'"',
'| "'`AUDIT-MARKDIET.md` (missing from supplied archive) | Recover `33a17682a:AUDIT-MARKDIET.md`: generated-code/inliner evidence, the store-forwarding hazard and the chunk-completion regression (§§1–6). This would complete the external archive. |'"',
'| "'`NOTES-SERVERTAIL.md` | Deferred WAITAOF requirements (§8): connection write coverage, producer posting, policy-independent durability and both persistence engines. Review the historical instruction to wait for flush_id past the unretired WAITAOF itself. Keep §10’s indicative PRE/POST table with its geometry and the +0.73% default-on SET instruction result that missed its +0.5% target; it is not a general throughput claim. |'"',
'| "'`NOTES-SCRIPTATOMIC.md` | Minimal write-then-raise reproductions, lost-update versus newly-created-key deletion evidence, the MVCC/raw-restore mismatch, and why even correct rollback is incompatible with the intended script semantics. |'"',
'', '## Verification','',
'- All 66 baseline references appear exactly once in the inventory; every INLINE and DROP has archive/history evidence.',
'- All 65 comment references are removed; only the explicitly excluded WAITAOF string remains.',
'- The 23 modified source files have identical sequences of 256,358 non-comment C++ tokens before and after, including string literals and preprocessing tokens. Comments and whitespace were the only discarded token classes in this static comparison.',
'- "'`git diff --check` passed. The delivery patch was checked against the unchanged baseline index with `git apply --cached --check`.'"',
'- No build, server, benchmark, tests or gate ran. No executable statements, gate rows, "'`EXPECT_QUICK` or `EXPECT_FULL` changed; gate counts are unchanged.'"',
'- The pre-existing "'`CODEX-OUT.md` modification in the original `cx-inline` worktree was left untouched.'"', '']
Path('INLINE.md').write_text('\\n'.join(lines))
print(f'Wrote INLINE.md with {len(records)} reference rows.')
PY" in /home/user/Projects/cx-inline-comments
 succeeded in 0ms:
Wrote INLINE.md with 66 reference rows.

diff --git a//home/user/Projects/cx-inline-comments/src/core/config.h b//home/user/Projects/cx-inline-comments/src/core/config.h
index 495e6ae42da15ee4bcc76a8a846efeef506903d7..8b5f3e5c786e8eeae0296a5dcf19338056d59007
--- a//home/user/Projects/cx-inline-comments/src/core/config.h
+++ b//home/user/Projects/cx-inline-comments/src/core/config.h
@@ -290,8 +290,8 @@
     uint32_t client_lb = 1;
 
     // ---- automatic role split (boot-latched) -----------------------------------------------
-    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer
-    // (DESIGN-flipfp.md); its measured sampling policy is internal to flipctl.h. Off and 1s
+    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer;
+    // its measured sampling policy is internal to flipctl.h. Off and 1s
     // dispatch pay one predicted branch per op and no fingerprint store. Trigger bands learn
     // from the anchor's own quiet jitter; no separate controller tuning knobs remain.
     uint32_t flip_auto = 0;
diff --git a//home/user/Projects/cx-inline-comments/src/core/flipctl.h b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
index 5185da4085ae5f2e3325702064208810cb9c42f7..00b11f3e799ae7313e3ce48ae074715e0bbb7968
--- a//home/user/Projects/cx-inline-comments/src/core/flipctl.h
+++ b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
@@ -19,7 +19,9 @@
 
 class Server;
 
-// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
+// Preserve the measured sampler: one whole parse pass in 100 on average. At a matched 512k ops/s,
+// a GET-only -> SET-only swap triggered the fingerprint detector once with both exhaustive and
+// 1-in-100 sampling; the sampled writer still detected the shift with the rate held fixed.
 // Arming derives solely from the controller latch. No runtime sampling knob or adaptive
 // per-operation work is needed; the detector already derives its bands from actual samples.
 inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
@@ -52,9 +54,11 @@
     uint64_t closed_windows = 0;
 };
 
-// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
-// frame -- one load of this writer's own word, on the line the old enabled() test already read,
-// and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
+// SAMPLED BY PARSE PASS. A per-op countdown would restore the store this sampler removes: no
+// existing io-side command counter covers both the point paths and all io-local commands.
+// The io loop asks pass_sampled() once per dispatched frame -- one load of this writer's own word,
+// on the line the old enabled() test already read, and one predicted-false branch; no store.
+// Inside a sampled pass the full fingerprint body runs
 // for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
 // that pass; at the pass end finish_parse_pass() publishes it and the caller draws the gap to the
 // next sampled pass. `work_window_` is the mean commands per fingerprint
diff --git a//home/user/Projects/cx-inline-comments/src/core/server.h b//home/user/Projects/cx-inline-comments/src/core/server.h
index f869dd3d1737afa6b1c401919fb66c96772b1225..a168d1656a19c4b098167904d42f814caae7513c
--- a//home/user/Projects/cx-inline-comments/src/core/server.h
+++ b//home/user/Projects/cx-inline-comments/src/core/server.h
@@ -8,8 +8,10 @@
 //
 // THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
 // dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
-// its derived shard-granularity fast path, published only at a quiesced commit. See
-// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
+// its derived shard-granularity fast path, published only at a quiesced commit. Shards move only
+// after executing, deferred, group and pinned-read work drains; destination routes publish after
+// the ownership edge. An IO may enqueue using an old route after that edge, so tasks and borrow
+// releases recheck ownership and forward before shard access instead of carrying routing epochs.
 #pragma once
 #include <algorithm>
 #include <atomic>
@@ -308,7 +310,7 @@
         for (uint32_t i = 0; i < nthreads; i++) {
             threads_[i] = std::make_unique<ThreadCtx>();
             // The fingerprint writer is armed only when its one reader, the flip controller, is
-            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
+            // enabled: with --flip-auto 0 and in 1s mode it is dark and costs
             // one predicted branch per op. The enabled sampler keeps its measured 1-in-100 policy.
             threads_[i]->init(i, placement_.role_of(i), nthreads,
                               0,
@@ -1969,7 +1971,7 @@
         shard_owner_[shard_id].store(thread_id, std::memory_order_release);
     }
 
-    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
+    // The caller must hold both executor loops at the safe point:
     // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
     // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
     // Vector capacity and membership are settled while PREPARING still names the source.  The
@@ -1988,7 +1990,7 @@
     // lost, a block ends up linked twice, and the damage surfaces later as either
     // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
     // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
-    // Both were observed (see DESIGN-P0REPLY.md).
+    // Both were observed.
     //
     // The rebind used to be deferred to the destination's own next executor pass
     // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
@@ -2827,7 +2829,7 @@
     // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
     // other command. It widens -- deterministically -- the window in which a younger whole-owner
     // walker could overtake an older same-connection group on the destination shard.
-    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
+    // TEST HOOK: effective local-read LANE CAPACITY for admission. 0 means
     // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
     // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
     // its masking, so this can never overrun anything. It exists because lane oversubscription is
@@ -2935,9 +2937,11 @@
     // is already set. Nothing on GET/SET touches it.
     //
     // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
-    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
-    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
-    // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
+    // sequence: a blocking op requires an empty ROB, then sets the barrier before any younger
+    // frame can be parsed; a resumed scatter inherits that same slot and barrier. The state the
+    // owner-scoped release exists to survive -- a release that must NOT drop the barrier -- has
+    // to be injected. It is held past ROB quiescence on purpose; barrier_release_quiesced()
+    // exempts this one bit for that reason.
     //
     // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
     // nothing left to do and parks on submit_and_wait(1) -- unbounded under io_uring. A hold that
@@ -2968,12 +2972,11 @@
         return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
     }
     // An overlap: some owner took the parse barrier while another owner already held it. With
-    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
-    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
-    // live and the owner-scoped release in blocking_retire() became load-bearing rather than
-    // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
-    // own positive control: a "must be zero" reading proves nothing until something has been shown
-    // able to make it non-zero.
+    // DEBUG BARRIER-HOLD off this must read ZERO -- if it ever moves on production traffic the
+    // latent case just went live and the owner-scoped release in blocking_retire() became
+    // load-bearing rather than defensive. With the latch ON it advances once per blocking dispatch,
+    // which is the counter's own positive control: a "must be zero" reading proves nothing until
+    // something has been shown able to make it non-zero.
     void note_barrier_overlap() {
         barrier_owner_overlaps_.fetch_add(1, std::memory_order_relaxed);
     }
diff --git a//home/user/Projects/cx-inline-comments/src/core/shard.h b//home/user/Projects/cx-inline-comments/src/core/shard.h
index 694b9a93c6cd63db49b9dcf061dbb5182c49986a..39a049d2f8619bbf52f706775956cc9198060364
--- a//home/user/Projects/cx-inline-comments/src/core/shard.h
+++ b//home/user/Projects/cx-inline-comments/src/core/shard.h
@@ -431,7 +431,7 @@
         // NOT share this counter: multi.inc's prepare_write_key() re-asks it at install time and
         // reports violations through INFO atomic_exec_order_late (multi_exec_order_late()), which
         // must read zero. Summing the two would hide a violation inside a number that is non-zero
-        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
+        // by design, leaving the park's safety argument with no falsifier.
         uint64_t atomic_exec_order_holds = 0;
         // Times watch_finalize_reservation() answered "not ready" because the reservation's epoch
         // was still 0, i.e. a unit was turned into a Retry by an undecided WATCH reservation. It
diff --git a//home/user/Projects/cx-inline-comments/src/core/signal.h b//home/user/Projects/cx-inline-comments/src/core/signal.h
index 933a5d7ee09538f3923df3c5eb77f9c72cdc0bfa..f69f7d73942ee129f4ad9cd1f23befe69fc4167d
--- a//home/user/Projects/cx-inline-comments/src/core/signal.h
+++ b//home/user/Projects/cx-inline-comments/src/core/signal.h
@@ -47,7 +47,7 @@
 // that will be compared against a deadline must come from here and never from now_ns() above --
 // that one is CLOCK_MONOTONIC, so its "milliseconds" are milliseconds since boot and sit roughly
 // five orders of magnitude below any real deadline. A monotonic value used as an expiry cut does
-// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
+// not skew the answer, it disables expiry outright.
 inline int64_t now_realtime_ms() {
     timespec ts;
     clock_gettime(CLOCK_REALTIME, &ts);
diff --git a//home/user/Projects/cx-inline-comments/src/core/thread.h b//home/user/Projects/cx-inline-comments/src/core/thread.h
index 0fcfa17ba6e331309260b641ec21aa4a8ff9ba36..3cf72c1b96ca700f602e2e7c630a1e6970f9620f
--- a//home/user/Projects/cx-inline-comments/src/core/thread.h
+++ b//home/user/Projects/cx-inline-comments/src/core/thread.h
@@ -118,7 +118,7 @@
     ContextRoute,           // scatter/script/all-shard or conservative broad-owner route
     ContextKeymissNotify,   // MGET miss must retain owner-side notification behavior
     InflightWrite,
-    ArmTransient,           // pre-arming writes still in flight (DESIGN-RINGDIET.md)
+    ArmTransient,           // pre-arming writes still in flight
     AtomicPending,
     Missing,
     Typed,
@@ -151,7 +151,7 @@
     // ARM-ON-DEMAND TRANSIENT. Reads demoted because the connection carried writes published
     // BEFORE a local read armed it -- writes the RYOW ring deliberately never recorded. Bounded
     // per connection by one ROB drain and reported separately from the steady-state key conflict
-    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
+    // above so that the two can never be confused in a bench.
     uint64_t fallback_arm_transient = 0;
     uint64_t fallback_atomic_pending = 0;
     uint64_t fallback_missing = 0;
@@ -160,7 +160,7 @@
     uint64_t fallback_seq_churn = 0;
     uint64_t fallback_generation = 0;
     uint64_t fallback_lane_full = 0;
-    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
+    // Lane ADMISSION deferrals. Frames the armed parser left unconsumed at rpos, to be
     // re-parsed by a later pass of the same thread, because the local-read lane had no room
     // (defer_lane_full) or because the connection already held its fair share of a lane under
     // pressure (defer_quota). Neither is a fallback: the read still completes locally, and no

exec
/bin/bash -lc "sed -n '1,38p' INLINE.md
sed -n '65,110p' INLINE.md
sed -n '110,165p' INLINE.md
sed -n '20,33p' src/core/flipctl.h
sed -n '54,65p' src/core/flipctl.h
sed -n '2970,2986p' src/core/server.h" in /home/user/Projects/cx-inline-comments
 succeeded in 0ms:
# INLINE — archived source-comment references

Base: `bebfde299` (`cx-inline`). Worktree: `/home/user/Projects/cx-inline-comments`, branch `codex/inline-archived-notes`. All source locations below refer to that base, before comment insertion changes line numbers.

The inventory contains **66 references: 65 comments and one executable error string**. Decisions: **15 INLINE, 50 DROP, one OUT OF SCOPE**. All 65 comment pointers are removed. DROP means the cited passage was read and its load-bearing reasoning was already present; it does not mean the note was discarded unread.

Archive consulted: `/home/user/Projects/lane-notes-archive/`. Seventeen distinct notes were cited. Sixteen are present there; `AUDIT-MARKDIET.md` is missing and was read from `33a17682a:AUDIT-MARKDIET.md`, §4, lines 174–187, using Git history. `NOTES-MDIET.md` concerns small collections and is not a substitute.

Historical measurements retain their stated workload and meaning. In particular, the 512k ops/s fingerprint swap is evidence from the detector revision measured in `DESIGN-flipfp.md` §5.2, not a new measurement of this checkout.

## Every reference

| Original source location | Decision | Cited note / passage read | Claim carried across, or reason for dropping |
| --- | --- | --- | --- |
| `src/cmd/blocking.inc:1351` | DROP | `NOTES-BARRIER.md` — §2, lines 68-95; §4, lines 168-179. | The existing comment already explains the empty-ROB/parse-stop invariant and why quiescence inside a retire callback cannot replace owner-scoped release. |
| `src/cmd/climon.cc:608` | DROP | `NOTES-CLIMON2.md` — Divergences from redis 7.4, item 1, lines 341-347; measured UNPAUSE 0.5 s into a 5 s pause returned after 4.5 s. | The existing comment already states the deliberate PAUSE ALL exemption for connection-control commands so CLIENT UNPAUSE remains usable. |
| `src/cmd/multi.h:67` | DROP | `NOTES-MULTIRES.md` — §5(a), lines 164-186; corrected/current ownership explanation in NOTES-MULTIRACE.md §5, lines 155-166. | The existing comment already explains why a transaction must recognize both its own epoch and every lowered child epoch to avoid holding against itself. |
| `src/cmd/multi.h:73` | DROP | `NOTES-MULTIRACE.md` — §5, One counter, one meaning, lines 210-249. | The existing comment already distinguishes the positive hold counter from the must-remain-zero late-install falsifier. |
| `src/cmd/multi.inc:907` | DROP | `NOTES-MULTIRACE.md` — §5, lines 168-193 and 210-249. | The existing comment already carries the owner-only pending-list publication argument and explains why a nonzero install-time probe falsifies the dispatch park. |
| `src/cmd/multi.inc:919` | INLINE | `NOTES-MULTIRES.md` — §5(a), lines 164-186; dispatch-time safety qualification in NOTES-MULTIRACE.md §5, lines 168-193. | A mid-command hold deadlocked with one connection and no WATCH (0 rounds versus 300 on base): the older group waited for partial transaction installs while the transaction waited for that group. Dispatch-time parking occurs before this fragment installs anything on its owner, so it cannot create that reverse dependency there. |
| `src/cmd/scripting.cc:240` | DROP | `NOTES-SCRIPTATOMIC.md` — Root cause and second defect, lines 76-158; Fix shape and alternatives, lines 161-191. | The existing comment already explains Redis partial-effects semantics, lost writes from restoring stale pre-images through raw erase/insert, and why single-owner isolation does not need undo. |
| `src/cmd/server_tail.cc:176` | DROP | `NOTES-SERVERTAIL.md` — §8, lines 232-269. | The existing comment already explains parse-time sampling missing preceding writes, IO-thread deadlock from synchronous waiting, and the deferred ROB reply/producer flush/durable-frontier design. |
| `src/cmd/server_tail.cc:212` | OUT OF SCOPE | `NOTES-SERVERTAIL.md` — §8, lines 240-253 explains why local-fsync waiting is not implemented. | The pointer is part of the executable WAITAOF error reply string; removing it changes observable command output and violates the comments-only constraint. It remains unchanged. |
| `src/cmd/server_tail.cc:233` | DROP | `NOTES-SERVERTAIL.md` — §1 command list, lines 15-39; full-note REPLICAOF/SLAVEOF search found only the command-table entry. | The existing comment already explains why accepting replication commands would falsely claim a replica exists. The cited note lists the command surface without adding a specific supporting claim. |
| `src/cmd/slowlog.cc:22` | DROP | `NOTES-SERVERTAIL.md` — §3 cost model and divergences, lines 59-89; historical indicative performance caveats in §10, lines 317-347. | The existing comment already explains normal batch timing, escalation, and the attribution tradeoff against timing every operation. Existing overclaims are recorded in findings. |
| `src/cmd/slowlog.h:8` | INLINE | `NOTES-SERVERTAIL.md` — §3, lines 67-84; confirmed against ExLoop::exec_batch_timed, src/core/ex_loop.h:2325-2377. | A one-op batch is timed exactly; a multi-op batch is screened by its per-op average and an overrun arms exact timing for the next 64 batches. A one-off slow operation in the first multi-op batch is not logged, unlike Redis per-command timing. |
| `src/cmd/t_server.cc:377` | DROP | `NOTES-CLIMON2.md` — Divergences from redis 7.4, item 8, lines 365-367. | The existing comment already explains that tracking-table-max-keys is boot-latched because IO owners read Config directly, while Redis permits live CONFIG SET. |
| `src/cmd/t_server.cc:935` | DROP | `P128.md` — §8.1, lines 423-451. | The existing comment already explains why lowering admission only permits deterministic overflow inside one parse pass without racing the drain or shrinking the ring; 0 restores the derived production cap. |
| `src/cmd/t_server.cc:950` | INLINE | `NOTES-BARRIER.md` — §2, lines 68-95; §5 injector rationale, lines 217-232. Confirmed current guard/acquire/break in src/core/io_loop.h:3945-4018. | Production blocking dispatch requires an empty ROB and then stops younger parsing, so the blocking operation is alone and cannot overlap a second barrier owner; the test must inject that otherwise unreachable state. |
| `src/cmd/t_server.cc:2490` | INLINE | `NOTES-BARRIER.md` — §2, lines 68-95; §5 four-control matrix, lines 278-299. | The zero production-overlap assertion is justified by blocking dispatch waiting for an empty ROB and barring younger parsing, which excludes a second owner. The existing fired-mechanism assertion remains. |
| `src/core/config.h:294` | DROP | `DESIGN-flipfp.md` — §1 reader census and §2(a), lines 24-46 | The existing block already explains controller-only arming, the internal sampling policy and the dark no-store path. |
| `src/core/ex_loop.h:59` | DROP | `P128.md` — §4, especially its two corrections; §7.1 points 3–4 (lines 179–190, 404–410). | The existing comment already explains lane-full-only arming, decay, fair-share admission, and the dormant-path cost. |
| `src/core/ex_loop.h:141` | INLINE | `P128.md` — §8.1, lines 423–451: escalating traffic failed to arm the row; a capped lane makes the precondition deterministic within one parse pass. | A lower admission cap lets one pipelined socket write fill the lane inside one parse pass, before any drain can race the test; traffic volume alone cannot guarantee the window. |
| `src/core/ex_loop.h:278` | DROP | `P128.md` — §4 and §7.1, lines 163–176 and 397–420; final quota uses pending_read_local_count, not in_flight. | The existing comment already explains the pressure-gated quota and the intended fair-share/progress reasoning. Its overbroad guarantee is separately reported below. |
| `src/core/ex_loop.h:313` | DROP | `P128.md` — §9.5, lines 689–747: control-cell ABBA results, field-layout analysis, eager active_.size() evaluation, and caller-side pressure gate. | The existing comment already preserves eager argument evaluation, the extra cache line, the measured fill increase with flat instructions, and gate-the-argument reasoning. |
| `src/core/ex_loop.h:565` | DROP | `P128.md` — §4 two corrections and §7.1 point 4, lines 179–183 and 407–410. | The existing comment directly describes the per-rotation decay; the full pressure-window rationale remains at its constant. |
| `src/core/ex_loop.h:2342` | DROP | `NOTES-SERVERTAIL.md` — §3, Divergences from redis, item 2 (lines 86–89): one cross-shard entry, final-answer owner, not the sum of participating shards. | The existing comment already states one entry per command, with attribution to the final owner that publishes Done. |
| `src/core/ex_loop.h:2526` | INLINE | `NOTES-MULTIRES.md` — §5(a), lines 164–187: rejected mid-command hold, one-connection deadlock, and the cycle through partial transaction installs; superseding dispatch-time hold explicitly distinguished. | Holding in prepare_write_key() deadlocked because the transaction waited on an older group while holding partial installs that the older group’s order-blind probe waited on. Dispatch-time parking precedes any install and preserves predecessor order. |
| `src/net/rob.h:54` | DROP | `DESIGN-RINGDIET.md` — DESIGN-RINGDIET.md Counters, lines 98-111. | The comment already explains all three counters, why they are always enabled, the unarmed-stream noinline gate, and migration of the owner pointer. |
| `src/net/rob.h:137` | DROP | `DESIGN-RINGDIET.md` — DESIGN-RINGDIET.md The design (2), lines 67-81; What did not change, lines 83-88. | The comment already states the allocation size, first-armed-write allocation, zero sidecar cost for pure-read/pure-write/idle connections, and layout-lock rationale. |
| `src/net/rob.h:318` | DROP | `DESIGN-RINGDIET.md` — DESIGN-RINGDIET.md The design (2), lines 78-81. | The existing MGET fence comment already explains why a read-only connection must not allocate a 1216-byte write-ring sidecar for one id. |
| `src/net/rob.h:341` | DROP | `P128.md` — P128.md §4 corrections, lines 183-187: in_flight() throttled for unflushed replies, reducing busiest-thread operations/rotation from about 1000 to 416; final measure is pending_read_local_count(). | The comment already rejects in_flight() as the admission measure because unflushed completed replies hold no lane slots, and retains the measured loss in operations per rotation. |
| `src/net/rob.h:364` | DROP | `AUDIT-MARKDIET.md` — Absent from /home/user/Projects/lane-notes-archive; recovered and read with git show 33a17682a:AUDIT-MARKDIET.md, §4 (lines 174-187). git log --all -- AUDIT-MARKDIET.md also identifies original 9deada175. NOTES-MDIET.md was inspected and is unrelated small-collection memory work. | The existing comment already explains clear-on-empty, the chunk-completion regression, filter accumulation, and exact-walk cost on the first subsequent write batch. |
| `src/net/rob.h:474` | DROP | `DESIGN-RINGDIET.md` — DESIGN-RINGDIET.md Mechanism, lines 21-28; The design (1), lines 32-51. | The existing write-half comment already states all omitted bookkeeping, the producer-line id store, and why the old overflowing ring was cheap for the wrong reason. |
| `src/net/rob.h:560` | DROP | `DESIGN-RINGDIET.md` — DESIGN-RINGDIET.md The design (1), lines 44-65. | The read-half comment already specifies the predictable producer-line gate, zero steady-state state, and the pre-arming-write transient. |
| `src/net/rob.h:690` | DROP | `DESIGN-RINGDIET.md` — DESIGN-RINGDIET.md The design (2), lines 67-76. | The allocation comment already says first write on an armed connection, never accept, and safe unarmed fallback on allocation failure. |
| `src/net/rob.h:808` | DROP | `DESIGN-RINGDIET.md` — DESIGN-RINGDIET.md The arming transient, lines 53-65. | The existing state comment fully describes Unarmed/ArmFence, fixed pre-arming retirement fence, and rejection of the self-extending overflow generation that would permanently demote interleaved reads. |
| `src/store/flatstore.h:594` | INLINE | `NOTES-EVICT.md` — NOTES-EVICT.md Accounting and shard threshold, lines 27-49, especially lines 40-44. | Including transient table/tombstone/allocator costs would make admission depend on resize history instead of maintained object/key counters, so maxmemory is a stable logical cache budget rather than an RSS ceiling. |
| `src/store/flatstore_atomic.inc:244` | INLINE | `NOTES-MULTIRES.md` — NOTES-MULTIRES.md §5(a), lines 164-184, specifically the first self-hold cause at lines 175-179; preserve its supersession note distinguishing dispatch-time from mid-command holds. | Comparing group pointers was a rejected own-unit test: an EXEC child scatter install names a group while that EXEC's plain installs do not, causing a transaction to wait on itself. |
| `src/store/flatstore_atomic.inc:1432` | DROP | `NOTES-RECYCLE.md` — NOTES-RECYCLE.md §3, lines 27-48; §4, lines 50-99. | The section and following comments already explain owner-private cache ownership via the retire sink, shard eligibility, live-footprint ceiling, post-grace eligibility and borrow retention; retain the live code-file pointer. |
| `src/store/flatstore_atomic.inc:1785` | DROP | `NOTES-MULTIRES.md` — NOTES-MULTIRES.md §1 table, lines 18-28 (246/1120 is the sweep's 28-cell, 16-core-pin result); §4, lines 115-156. | The comment already explains same-connection program order versus inverted commit tickets, the 246/1120 lost-write reproduction, and monotone epoch repair without changing cross-connection ordering. |
| `src/store/flatstore_atomic.inc:2174` | DROP | `NOTES-MULTIRES.md` — NOTES-MULTIRES.md §1 table, lines 18-28; §4, lines 115-156. | The second collapse implementation already carries the same full program-order rationale, rejected raw-ticket ranking, and 246/1120 lost-write measurement. |
| `src/store/kv_block_cache.h:71` | INLINE | `NOTES-RECYCLE.md` — NOTES-RECYCLE.md §6 geometry at line 127 and cap comparison at lines 180-190; 400k operations and two repetitions. | On one fused owner with 64 shards, cap 32 left 0.5001/0.3750 mallocx calls per operation at pipeline 64/256; cap 4096 reduced both to zero for 2-4 KiB more cached memory. This supplies the measured reason for the drain-sized per-class bound. |
| `src/store/read_local_settax.h:11` | DROP | `NOTES-RECYCLE.md` — NOTES-RECYCLE.md §2, lines 21-25; §4, lines 96-99; §6 cap comparison, lines 180-190. | The existing selector-retirement comment already says post-grace recycling shipped unconditionally and a second recycler is forbidden; retain the reference to the implementation functions. |

## Constraint exception

`src/cmd/server_tail.cc:212` contains `NOTES-SERVERTAIL.md` inside the WAITAOF error-string literal. Removing it changes an executable statement and a client-visible reply, so it remains byte-for-byte unchanged. This is the only `.md` reference remaining anywhere under `src/`; it is included in the inventory above rather than silently skipped.

## Accuracy findings

These pre-existing statements surfaced while reading the cited reasoning. No implementation was changed. Existing complete comments were retained, except that the inaccurate recycler measurement was replaced by the exact cited table as part of inlining.

| Original source location | Finding and evidence |
| --- | --- |
| `src/core/server.h:2937`; `src/net/conn.h:84`; `src/cmd/blocking.inc:1350` | The historical six-owner wording, including the hypothetical “seventh owner,” is stale. `BarrierOwner` already has seven production bits, including `Sleep`, plus the debug bit (`conn.h:94–109`). The blocking-operation exclusion argument still applies. |
| `src/core/ex_loop.h:278` | The claim that the quota guarantees aggregate admission within capacity and progress for every connection in every rotation is too broad. `max(1, cap / live)` cannot establish it when `live > cap`: cap 16 and 17 active connections permit one each but only 16 fit. `P128.md` §7.1 repeats the invalid bound and separately acknowledges the greater-than-capacity residual case. |
| `src/core/ex_loop.h:313` | The retained comment says +3.4% same-CCX fills. `P128.md` §9.5 prints 9.19 → 9.525, approximately +3.6%. Its +11% DRAM figure agrees. |
| `src/cmd/slowlog.cc:10`; `src/core/ex_loop.h:2310` | An unpipelined connection does not imply a one-op executor batch: `drain_tasks()` batches tasks across connections. Multiple depth-1 clients can share a batch, so the universal exact-timing claim is unsupported. |
| `src/cmd/slowlog.cc:19` | A single slow operation in a multi-op batch does not necessarily arm escalation: the screen compares the per-op batch average with the threshold. Exact timing also lasts only the next 64 batches, so an arbitrary later recurrence is not guaranteed exact. The new `slowlog.h` comment states those bounds. |
| `src/cmd/server_tail.cc:172` | The claim that every `numlocal == 0` WAITAOF reply is Redis-exact is false. The implementation emits `[0, 0]`; the archive (§§1, 8) explicitly calls the local element conservative because there is no per-connection last-write offset. Only the replica count is exact. |
| `src/net/rob.h:561`; `src/net/rob.h:809` | The arming comments say the nonzero states end for good or occur at most once. `mark_current_write()` resets to Unarmed after sidecar allocation failure (`rob.h:491`), allowing repeated arming. The archive explicitly describes that self-healing path. |
| `src/store/read_local_settax.h:10` | The cache is called the shard’s own, but it belongs to the fused owner and is shared by that owner’s shards through `retire_sink.block_cache`; see `flatstore_atomic.inc:1434` and `NOTES-RECYCLE.md` §4. |
| `src/store/flatstore_atomic.inc:1466` | The cache-release comment says release happens before eviction and calls both triggers whole-server events. Budget refusal releases after noeviction or failed/bounded eviction, using a shard-local budget decision; successful eviction retains the cache. See `flatstore.h:1671`, `flatstore.h:2470`, and `NOTES-RECYCLE.md` §5. |
| `src/store/kv_block_cache.h:70` | The original claim that cap 32 leaves only a few percent reuse is not what the cited table measures. It reports 0.5001/0.3750 mallocx calls per op (roughly 50–62.5% reuse), against 0.0000 at cap 4096. The inline comment now carries the exact table and 2–4 KiB memory difference. These are allocator-call measurements, not a throughput claim. |
| `src/cmd/blocking.inc:1347` | Its numeric `io_loop.h:1391` pointer is stale; the blocking in-flight guard is around base `src/core/io_loop.h:3953`. The invariant remains correct. |

## Archived material worth retaining outside these comments

These are retention candidates for the maintainer, not proposals to restore working notes to version control. No archive files were copied into the repository. Historical prototype text and superseded conclusions need their later corrections kept beside them.

| Archived note | Material that does not fit a local comment |
| --- | --- |
| `NOTES-MIGRATE.md` | The bucket, connection and role-transfer state machines and in-flight case tables (§§1–3), including cancellation/drain and stale-route cases across multiple files. |
| `NOTES-MIGRATE.md` | The bucket, connection and role-transfer state machines and in-flight case tables (§§1–3), including cancellation/drain and stale-route cases across multiple files. |
| `DESIGN-P0REPLY.md` | The transfer/rebind event trace, both observed corruption signatures, negative controls and ownership-edge census (§§7–10). Its early exclusion of ownership handoff is superseded by the later root cause. |
| `DESIGN-flipfp.md` | Matched-rate PRE/POST/null tables, isolated detector-liveness controls and sampled-count/deafness analysis (§§5–6), with historical-detector and low-rate qualifications. The inline swap is only one measured cell. |
| `DESIGN-RINGDIET.md` | The full ABBA/null results, saturation corrections, interference qualifications and counter-based proof of zero pure-write bookkeeping. |
| `NOTES-BARRIER.md` | The setter/releaser census, rejected interleavings and owner-count/quiescence alternatives (§§1–4), plus the injector, explicit wakeup and positive-control design (§5). Its six-owner inventory is historical. |
| `P128.md` | The superseded quota variants, starvation analysis, pre/post/no-quota rate/cycles/IPC/tail tables, dormant-field cache-fill regression and final acceptance results (§§5, 7, 9–10). The above-capacity fairness proof needs correction before reuse. |
| `DESIGN-NOTIFY.md` | The complete wake-order argument across batches, teardown/slot reuse, migration and independent observers (§4). Distinguish the separate per-producer design in §3 from the current implementation. |
| `NOTES-MULTIRES.md` | The reduced reproduction, both rejected fixes, and the geometry correction: collapse-only appeared sufficient at 16 cores but failed 3/8 runs at 8 cores, while collapse plus resolver passed 10/10 (§§2–6). |
| `NOTES-MULTIRACE.md` | The complete dispatch-time acyclicity argument, separate hold/late falsifiers, and residual two-phase-unit liveness test rationale (§5), beyond the short mid-command cycle. |
| `NOTES-RECYCLE.md` | Per-shard versus per-owner evidence, allocator-call/cycle tables, logical/physical accounting decision and complete lifecycle/migration matrix (§§4–7). |
| `AUDIT-MARKDIET.md` (missing from supplied archive) | Recover `33a17682a:AUDIT-MARKDIET.md`: generated-code/inliner evidence, the store-forwarding hazard and the chunk-completion regression (§§1–6). This would complete the external archive. |
| `NOTES-SERVERTAIL.md` | Deferred WAITAOF requirements (§8): connection write coverage, producer posting, policy-independent durability and both persistence engines. Review the historical instruction to wait for flush_id past the unretired WAITAOF itself. Keep §10’s indicative PRE/POST table with its geometry and the +0.73% default-on SET instruction result that missed its +0.5% target; it is not a general throughput claim. |
| `NOTES-SCRIPTATOMIC.md` | Minimal write-then-raise reproductions, lost-update versus newly-created-key deletion evidence, the MVCC/raw-restore mismatch, and why even correct rollback is incompatible with the intended script semantics. |

## Verification

- All 66 baseline references appear exactly once in the inventory; every INLINE and DROP has archive/history evidence.
- All 65 comment references are removed; only the explicitly excluded WAITAOF string remains.
- The 23 modified source files have identical sequences of 256,358 non-comment C++ tokens before and after, including string literals and preprocessing tokens. Comments and whitespace were the only discarded token classes in this static comparison.
- `git diff --check` passed. The delivery patch was checked against the unchanged baseline index with `git apply --cached --check`.
- No build, server, benchmark, tests or gate ran. No executable statements, gate rows, `EXPECT_QUICK` or `EXPECT_FULL` changed; gate counts are unchanged.
- The pre-existing `CODEX-OUT.md` modification in the original `cx-inline` worktree was left untouched.
class Server;

// Preserve the measured sampler: one whole parse pass in 100 on average. At a matched 512k ops/s,
// a GET-only -> SET-only swap triggered the fingerprint detector once with both exhaustive and
// 1-in-100 sampling; the sampled writer still detected the shift with the rate held fixed.
// Arming derives solely from the controller latch. No runtime sampling knob or adaptive
// per-operation work is needed; the detector already derives its bands from actual samples.
inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
    return controller_enabled ? 100u : 0u;
}

enum class FlipFingerprintClass : uint8_t {
    Read = 0,
    Write,
    uint64_t closed_windows = 0;
};

// SAMPLED BY PARSE PASS. A per-op countdown would restore the store this sampler removes: no
// existing io-side command counter covers both the point paths and all io-local commands.
// The io loop asks pass_sampled() once per dispatched frame -- one load of this writer's own word,
// on the line the old enabled() test already read, and one predicted-false branch; no store.
// Inside a sampled pass the full fingerprint body runs
// for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
// that pass; at the pass end finish_parse_pass() publishes it and the caller draws the gap to the
// next sampled pass. `work_window_` is the mean commands per fingerprint
// sample: one pass in W is sampled, so one frame in W is fingerprinted at every pipeline depth (a
    }
    uint64_t debug_blocking_timeout_reaps() const {
        return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
    }
    // An overlap: some owner took the parse barrier while another owner already held it. With
    // DEBUG BARRIER-HOLD off this must read ZERO -- if it ever moves on production traffic the
    // latent case just went live and the owner-scoped release in blocking_retire() became
    // load-bearing rather than defensive. With the latch ON it advances once per blocking dispatch,
    // which is the counter's own positive control: a "must be zero" reading proves nothing until
    // something has been shown able to make it non-zero.
    void note_barrier_overlap() {
        barrier_owner_overlaps_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t barrier_owner_overlaps() const {
        return barrier_owner_overlaps_.load(std::memory_order_relaxed);
    }
    // A blocking retirement released its own claim and the barrier STAYED UP because another owner

diff --git a//home/user/Projects/cx-inline-comments/src/core/config.h b//home/user/Projects/cx-inline-comments/src/core/config.h
index 495e6ae42da15ee4bcc76a8a846efeef506903d7..8b5f3e5c786e8eeae0296a5dcf19338056d59007
--- a//home/user/Projects/cx-inline-comments/src/core/config.h
+++ b//home/user/Projects/cx-inline-comments/src/core/config.h
@@ -290,8 +290,8 @@
     uint32_t client_lb = 1;
 
     // ---- automatic role split (boot-latched) -----------------------------------------------
-    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer
-    // (DESIGN-flipfp.md); its measured sampling policy is internal to flipctl.h. Off and 1s
+    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer;
+    // its measured sampling policy is internal to flipctl.h. Off and 1s
     // dispatch pay one predicted branch per op and no fingerprint store. Trigger bands learn
     // from the anchor's own quiet jitter; no separate controller tuning knobs remain.
     uint32_t flip_auto = 0;
diff --git a//home/user/Projects/cx-inline-comments/src/core/flipctl.h b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
index 5185da4085ae5f2e3325702064208810cb9c42f7..00b11f3e799ae7313e3ce48ae074715e0bbb7968
--- a//home/user/Projects/cx-inline-comments/src/core/flipctl.h
+++ b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
@@ -19,7 +19,9 @@
 
 class Server;
 
-// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
+// Preserve the measured sampler: one whole parse pass in 100 on average. At a matched 512k ops/s,
+// a GET-only -> SET-only swap triggered the fingerprint detector once with both exhaustive and
+// 1-in-100 sampling; the sampled writer still detected the shift with the rate held fixed.
 // Arming derives solely from the controller latch. No runtime sampling knob or adaptive
 // per-operation work is needed; the detector already derives its bands from actual samples.
 inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
@@ -52,9 +54,11 @@
     uint64_t closed_windows = 0;
 };
 
-// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
-// frame -- one load of this writer's own word, on the line the old enabled() test already read,
-// and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
+// SAMPLED BY PARSE PASS. A per-op countdown would restore the store this sampler removes: no
+// existing io-side command counter covers both the point paths and all io-local commands.
+// The io loop asks pass_sampled() once per dispatched frame -- one load of this writer's own word,
+// on the line the old enabled() test already read, and one predicted-false branch; no store.
+// Inside a sampled pass the full fingerprint body runs
 // for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
 // that pass; at the pass end finish_parse_pass() publishes it and the caller draws the gap to the
 // next sampled pass. `work_window_` is the mean commands per fingerprint
diff --git a//home/user/Projects/cx-inline-comments/src/core/server.h b//home/user/Projects/cx-inline-comments/src/core/server.h
index f869dd3d1737afa6b1c401919fb66c96772b1225..a168d1656a19c4b098167904d42f814caae7513c
--- a//home/user/Projects/cx-inline-comments/src/core/server.h
+++ b//home/user/Projects/cx-inline-comments/src/core/server.h
@@ -8,8 +8,10 @@
 //
 // THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
 // dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
-// its derived shard-granularity fast path, published only at a quiesced commit. See
-// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
+// its derived shard-granularity fast path, published only at a quiesced commit. Shards move only
+// after executing, deferred, group and pinned-read work drains; destination routes publish after
+// the ownership edge. An IO may enqueue using an old route after that edge, so tasks and borrow
+// releases recheck ownership and forward before shard access instead of carrying routing epochs.
 #pragma once
 #include <algorithm>
 #include <atomic>
@@ -308,7 +310,7 @@
         for (uint32_t i = 0; i < nthreads; i++) {
             threads_[i] = std::make_unique<ThreadCtx>();
             // The fingerprint writer is armed only when its one reader, the flip controller, is
-            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
+            // enabled: with --flip-auto 0 and in 1s mode it is dark and costs
             // one predicted branch per op. The enabled sampler keeps its measured 1-in-100 policy.
             threads_[i]->init(i, placement_.role_of(i), nthreads,
                               0,
@@ -1969,7 +1971,7 @@
         shard_owner_[shard_id].store(thread_id, std::memory_order_release);
     }
 
-    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
+    // The caller must hold both executor loops at the safe point:
     // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
     // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
     // Vector capacity and membership are settled while PREPARING still names the source.  The
@@ -1988,7 +1990,7 @@
     // lost, a block ends up linked twice, and the damage surfaces later as either
     // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
     // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
-    // Both were observed (see DESIGN-P0REPLY.md).
+    // Both were observed.
     //
     // The rebind used to be deferred to the destination's own next executor pass
     // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
@@ -2827,7 +2829,7 @@
     // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
     // other command. It widens -- deterministically -- the window in which a younger whole-owner
     // walker could overtake an older same-connection group on the destination shard.
-    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
+    // TEST HOOK: effective local-read LANE CAPACITY for admission. 0 means
     // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
     // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
     // its masking, so this can never overrun anything. It exists because lane oversubscription is
@@ -2935,9 +2937,11 @@
     // is already set. Nothing on GET/SET touches it.
     //
     // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
-    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
-    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
-    // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
+    // sequence: a blocking op requires an empty ROB, then sets the barrier before any younger
+    // frame can be parsed; a resumed scatter inherits that same slot and barrier. The state the
+    // owner-scoped release exists to survive -- a release that must NOT drop the barrier -- has
+    // to be injected. It is held past ROB quiescence on purpose; barrier_release_quiesced()
+    // exempts this one bit for that reason.
     //
     // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
     // nothing left to do and parks on submit_and_wait(1) -- unbounded under io_uring. A hold that
@@ -2968,12 +2972,11 @@
         return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
     }
     // An overlap: some owner took the parse barrier while another owner already held it. With
-    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
-    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
-    // live and the owner-scoped release in blocking_retire() became load-bearing rather than
-    // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
-    // own positive control: a "must be zero" reading proves nothing until something has been shown
-    // able to make it non-zero.
+    // DEBUG BARRIER-HOLD off this must read ZERO -- if it ever moves on production traffic the
+    // latent case just went live and the owner-scoped release in blocking_retire() became
+    // load-bearing rather than defensive. With the latch ON it advances once per blocking dispatch,
+    // which is the counter's own positive control: a "must be zero" reading proves nothing until
+    // something has been shown able to make it non-zero.
     void note_barrier_overlap() {
         barrier_owner_overlaps_.fetch_add(1, std::memory_order_relaxed);
     }
diff --git a//home/user/Projects/cx-inline-comments/src/core/shard.h b//home/user/Projects/cx-inline-comments/src/core/shard.h
index 694b9a93c6cd63db49b9dcf061dbb5182c49986a..39a049d2f8619bbf52f706775956cc9198060364
--- a//home/user/Projects/cx-inline-comments/src/core/shard.h
+++ b//home/user/Projects/cx-inline-comments/src/core/shard.h
@@ -431,7 +431,7 @@
         // NOT share this counter: multi.inc's prepare_write_key() re-asks it at install time and
         // reports violations through INFO atomic_exec_order_late (multi_exec_order_late()), which
         // must read zero. Summing the two would hide a violation inside a number that is non-zero
-        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
+        // by design, leaving the park's safety argument with no falsifier.
         uint64_t atomic_exec_order_holds = 0;
         // Times watch_finalize_reservation() answered "not ready" because the reservation's epoch
         // was still 0, i.e. a unit was turned into a Retry by an undecided WATCH reservation. It
diff --git a//home/user/Projects/cx-inline-comments/src/core/signal.h b//home/user/Projects/cx-inline-comments/src/core/signal.h
index 933a5d7ee09538f3923df3c5eb77f9c72cdc0bfa..f69f7d73942ee129f4ad9cd1f23befe69fc4167d
--- a//home/user/Projects/cx-inline-comments/src/core/signal.h
+++ b//home/user/Projects/cx-inline-comments/src/core/signal.h
@@ -47,7 +47,7 @@
 // that will be compared against a deadline must come from here and never from now_ns() above --
 // that one is CLOCK_MONOTONIC, so its "milliseconds" are milliseconds since boot and sit roughly
 // five orders of magnitude below any real deadline. A monotonic value used as an expiry cut does
-// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
+// not skew the answer, it disables expiry outright.
 inline int64_t now_realtime_ms() {
     timespec ts;
     clock_gettime(CLOCK_REALTIME, &ts);
diff --git a//home/user/Projects/cx-inline-comments/src/core/thread.h b//home/user/Projects/cx-inline-comments/src/core/thread.h
index 0fcfa17ba6e331309260b641ec21aa4a8ff9ba36..3cf72c1b96ca700f602e2e7c630a1e6970f9620f
--- a//home/user/Projects/cx-inline-comments/src/core/thread.h
+++ b//home/user/Projects/cx-inline-comments/src/core/thread.h
@@ -118,7 +118,7 @@
     ContextRoute,           // scatter/script/all-shard or conservative broad-owner route
     ContextKeymissNotify,   // MGET miss must retain owner-side notification behavior
     InflightWrite,
-    ArmTransient,           // pre-arming writes still in flight (DESIGN-RINGDIET.md)
+    ArmTransient,           // pre-arming writes still in flight
     AtomicPending,
     Missing,
     Typed,
@@ -151,7 +151,7 @@
     // ARM-ON-DEMAND TRANSIENT. Reads demoted because the connection carried writes published
     // BEFORE a local read armed it -- writes the RYOW ring deliberately never recorded. Bounded
     // per connection by one ROB drain and reported separately from the steady-state key conflict
-    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
+    // above so that the two can never be confused in a bench.
     uint64_t fallback_arm_transient = 0;
     uint64_t fallback_atomic_pending = 0;
     uint64_t fallback_missing = 0;
@@ -160,7 +160,7 @@
     uint64_t fallback_seq_churn = 0;
     uint64_t fallback_generation = 0;
     uint64_t fallback_lane_full = 0;
-    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
+    // Lane ADMISSION deferrals. Frames the armed parser left unconsumed at rpos, to be
     // re-parsed by a later pass of the same thread, because the local-read lane had no room
     // (defer_lane_full) or because the connection already held its fair share of a lane under
     // pressure (defer_quota). Neither is a fallback: the read still completes locally, and no

exec
/bin/bash -lc "python3 - <<'PY'
import re, subprocess
from pathlib import Path
root=Path.cwd()
source=subprocess.check_output(['git','diff','--','src'],text=True)
# Every changed line is a line comment, except the enum's unchanged executable prefix.
for line in source.splitlines():
    if not line.startswith(('+','-')) or line.startswith(('+++','---')):
        continue
    content=line[1:]
    assert not content.rstrip().endswith('\\\\'), 'Changed comment has a line splice'
    if content.lstrip().startswith('//'):
        continue
    assert content.split('//',1)[0]=='    ArmTransient,           ', repr(content)
report=Path('INLINE.md').read_text()
rows=re.findall(r'"'^'"\\| "'`src/[''^`]+` '"\\| (INLINE|DROP|OUT OF SCOPE) \\|',report,re.M)
assert len(rows)==66 and rows.count('INLINE')==15 and rows.count('DROP')==50
assert report.count('| "'`NOTES-MIGRATE.md` |'"')==1
report_diff=subprocess.run(['git','diff','--no-index','--','/dev/null','INLINE.md'],text=True,capture_output=True)
assert report_diff.returncode==1, report_diff.stderr
Path('INLINE.patch').write_text(source+report_diff.stdout)
subprocess.run(['git','diff','--check'],check=True)
subprocess.run(['git','diff','--cached','--quiet'],check=True)
subprocess.run(['git','apply','--cached','--check','INLINE.patch'],check=True)
for p in root.glob('.inline-*.json'):
    p.unlink()
print('PASS: source diff changes only // comments; no changed line splices.')
print('PASS: INLINE.md accounts for all 66 references (15 INLINE, 50 DROP, 1 OUT OF SCOPE).')
print('PASS: INLINE.patch applies cleanly to the baseline index (check only).')
print(f'Patch: {Path(\"INLINE.patch\").stat().st_size:,} bytes; report: {len(report.splitlines())} lines.')
PY
git status --short
git diff --stat" in /home/user/Projects/cx-inline-comments
 succeeded in 0ms:
PASS: source diff changes only // comments; no changed line splices.
PASS: INLINE.md accounts for all 66 references (15 INLINE, 50 DROP, 1 OUT OF SCOPE).
PASS: INLINE.patch applies cleanly to the baseline index (check only).
Patch: 82,496 bytes; report: 131 lines.
 M src/cmd/blocking.inc
 M src/cmd/climon.cc
 M src/cmd/multi.h
 M src/cmd/multi.inc
 M src/cmd/scripting.cc
 M src/cmd/server_tail.cc
 M src/cmd/slowlog.cc
 M src/cmd/slowlog.h
 M src/cmd/t_server.cc
 M src/core/config.h
 M src/core/ex_loop.h
 M src/core/flipctl.h
 M src/core/io_loop.h
 M src/core/server.h
 M src/core/shard.h
 M src/core/signal.h
 M src/core/thread.h
 M src/net/conn.h
 M src/net/rob.h
 M src/store/flatstore.h
 M src/store/flatstore_atomic.inc
 M src/store/kv_block_cache.h
 M src/store/read_local_settax.h
?? INLINE.md
?? INLINE.patch
 src/cmd/blocking.inc           |  2 +-
 src/cmd/climon.cc              |  2 +-
 src/cmd/multi.h                |  4 ++--
 src/cmd/multi.inc              |  9 +++++----
 src/cmd/scripting.cc           |  2 +-
 src/cmd/server_tail.cc         |  4 ++--
 src/cmd/slowlog.cc             |  2 +-
 src/cmd/slowlog.h              |  5 +++--
 src/cmd/t_server.cc            | 22 ++++++++++++----------
 src/core/config.h              |  4 ++--
 src/core/ex_loop.h             | 32 +++++++++++++++++---------------
 src/core/flipctl.h             | 12 ++++++++----
 src/core/io_loop.h             | 18 ++++++++++--------
 src/core/server.h              | 33 ++++++++++++++++++---------------
 src/core/shard.h               |  2 +-
 src/core/signal.h              |  2 +-
 src/core/thread.h              |  6 +++---
 src/net/conn.h                 |  9 +++++----
 src/net/rob.h                  | 20 ++++++++++----------
 src/store/flatstore.h          |  4 +++-
 src/store/flatstore_atomic.inc | 11 ++++++-----
 src/store/kv_block_cache.h     |  5 +++--
 src/store/read_local_settax.h  |  2 +-
 23 files changed, 116 insertions(+), 96 deletions(-)

diff --git a//home/user/Projects/cx-inline-comments/src/core/config.h b//home/user/Projects/cx-inline-comments/src/core/config.h
index 495e6ae42da15ee4bcc76a8a846efeef506903d7..8b5f3e5c786e8eeae0296a5dcf19338056d59007
--- a//home/user/Projects/cx-inline-comments/src/core/config.h
+++ b//home/user/Projects/cx-inline-comments/src/core/config.h
@@ -290,8 +290,8 @@
     uint32_t client_lb = 1;
 
     // ---- automatic role split (boot-latched) -----------------------------------------------
-    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer
-    // (DESIGN-flipfp.md); its measured sampling policy is internal to flipctl.h. Off and 1s
+    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer;
+    // its measured sampling policy is internal to flipctl.h. Off and 1s
     // dispatch pay one predicted branch per op and no fingerprint store. Trigger bands learn
     // from the anchor's own quiet jitter; no separate controller tuning knobs remain.
     uint32_t flip_auto = 0;
diff --git a//home/user/Projects/cx-inline-comments/src/core/flipctl.h b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
index 5185da4085ae5f2e3325702064208810cb9c42f7..00b11f3e799ae7313e3ce48ae074715e0bbb7968
--- a//home/user/Projects/cx-inline-comments/src/core/flipctl.h
+++ b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
@@ -19,7 +19,9 @@
 
 class Server;
 
-// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
+// Preserve the measured sampler: one whole parse pass in 100 on average. At a matched 512k ops/s,
+// a GET-only -> SET-only swap triggered the fingerprint detector once with both exhaustive and
+// 1-in-100 sampling; the sampled writer still detected the shift with the rate held fixed.
 // Arming derives solely from the controller latch. No runtime sampling knob or adaptive
 // per-operation work is needed; the detector already derives its bands from actual samples.
 inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
@@ -52,9 +54,11 @@
     uint64_t closed_windows = 0;
 };
 
-// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
-// frame -- one load of this writer's own word, on the line the old enabled() test already read,
-// and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
+// SAMPLED BY PARSE PASS. A per-op countdown would restore the store this sampler removes: no
+// existing io-side command counter covers both the point paths and all io-local commands.
+// The io loop asks pass_sampled() once per dispatched frame -- one load of this writer's own word,
+// on the line the old enabled() test already read, and one predicted-false branch; no store.
+// Inside a sampled pass the full fingerprint body runs
 // for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
 // that pass; at the pass end finish_parse_pass() publishes it and the caller draws the gap to the
 // next sampled pass. `work_window_` is the mean commands per fingerprint
diff --git a//home/user/Projects/cx-inline-comments/src/core/server.h b//home/user/Projects/cx-inline-comments/src/core/server.h
index f869dd3d1737afa6b1c401919fb66c96772b1225..a168d1656a19c4b098167904d42f814caae7513c
--- a//home/user/Projects/cx-inline-comments/src/core/server.h
+++ b//home/user/Projects/cx-inline-comments/src/core/server.h
@@ -8,8 +8,10 @@
 //
 // THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
 // dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
-// its derived shard-granularity fast path, published only at a quiesced commit. See
-// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
+// its derived shard-granularity fast path, published only at a quiesced commit. Shards move only
+// after executing, deferred, group and pinned-read work drains; destination routes publish after
+// the ownership edge. An IO may enqueue using an old route after that edge, so tasks and borrow
+// releases recheck ownership and forward before shard access instead of carrying routing epochs.
 #pragma once
 #include <algorithm>
 #include <atomic>
@@ -308,7 +310,7 @@
         for (uint32_t i = 0; i < nthreads; i++) {
             threads_[i] = std::make_unique<ThreadCtx>();
             // The fingerprint writer is armed only when its one reader, the flip controller, is
-            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
+            // enabled: with --flip-auto 0 and in 1s mode it is dark and costs
             // one predicted branch per op. The enabled sampler keeps its measured 1-in-100 policy.
             threads_[i]->init(i, placement_.role_of(i), nthreads,
                               0,
@@ -1969,7 +1971,7 @@
         shard_owner_[shard_id].store(thread_id, std::memory_order_release);
     }
 
-    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
+    // The caller must hold both executor loops at the safe point:
     // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
     // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
     // Vector capacity and membership are settled while PREPARING still names the source.  The
@@ -1988,7 +1990,7 @@
     // lost, a block ends up linked twice, and the damage surfaces later as either
     // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
     // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
-    // Both were observed (see DESIGN-P0REPLY.md).
+    // Both were observed.
     //
     // The rebind used to be deferred to the destination's own next executor pass
     // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
@@ -2827,7 +2829,7 @@
     // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
     // other command. It widens -- deterministically -- the window in which a younger whole-owner
     // walker could overtake an older same-connection group on the destination shard.
-    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
+    // TEST HOOK: effective local-read LANE CAPACITY for admission. 0 means
     // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
     // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
     // its masking, so this can never overrun anything. It exists because lane oversubscription is
@@ -2935,9 +2937,11 @@
     // is already set. Nothing on GET/SET touches it.
     //
     // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
-    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
-    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
-    // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
+    // sequence: a blocking op requires an empty ROB, then sets the barrier before any younger
+    // frame can be parsed; a resumed scatter inherits that same slot and barrier. The state the
+    // owner-scoped release exists to survive -- a release that must NOT drop the barrier -- has
+    // to be injected. It is held past ROB quiescence on purpose; barrier_release_quiesced()
+    // exempts this one bit for that reason.
     //
     // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
     // nothing left to do and parks on submit_and_wait(1) -- unbounded under io_uring. A hold that
@@ -2968,12 +2972,11 @@
         return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
     }
     // An overlap: some owner took the parse barrier while another owner already held it. With
-    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
-    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
-    // live and the owner-scoped release in blocking_retire() became load-bearing rather than
-    // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
-    // own positive control: a "must be zero" reading proves nothing until something has been shown
-    // able to make it non-zero.
+    // DEBUG BARRIER-HOLD off this must read ZERO -- if it ever moves on production traffic the
+    // latent case just went live and the owner-scoped release in blocking_retire() became
+    // load-bearing rather than defensive. With the latch ON it advances once per blocking dispatch,
+    // which is the counter's own positive control: a "must be zero" reading proves nothing until
+    // something has been shown able to make it non-zero.
     void note_barrier_overlap() {
         barrier_owner_overlaps_.fetch_add(1, std::memory_order_relaxed);
     }
diff --git a//home/user/Projects/cx-inline-comments/src/core/shard.h b//home/user/Projects/cx-inline-comments/src/core/shard.h
index 694b9a93c6cd63db49b9dcf061dbb5182c49986a..39a049d2f8619bbf52f706775956cc9198060364
--- a//home/user/Projects/cx-inline-comments/src/core/shard.h
+++ b//home/user/Projects/cx-inline-comments/src/core/shard.h
@@ -431,7 +431,7 @@
         // NOT share this counter: multi.inc's prepare_write_key() re-asks it at install time and
         // reports violations through INFO atomic_exec_order_late (multi_exec_order_late()), which
         // must read zero. Summing the two would hide a violation inside a number that is non-zero
-        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
+        // by design, leaving the park's safety argument with no falsifier.
         uint64_t atomic_exec_order_holds = 0;
         // Times watch_finalize_reservation() answered "not ready" because the reservation's epoch
         // was still 0, i.e. a unit was turned into a Retry by an undecided WATCH reservation. It
diff --git a//home/user/Projects/cx-inline-comments/src/core/signal.h b//home/user/Projects/cx-inline-comments/src/core/signal.h
index 933a5d7ee09538f3923df3c5eb77f9c72cdc0bfa..f69f7d73942ee129f4ad9cd1f23befe69fc4167d
--- a//home/user/Projects/cx-inline-comments/src/core/signal.h
+++ b//home/user/Projects/cx-inline-comments/src/core/signal.h
@@ -47,7 +47,7 @@
 // that will be compared against a deadline must come from here and never from now_ns() above --
 // that one is CLOCK_MONOTONIC, so its "milliseconds" are milliseconds since boot and sit roughly
 // five orders of magnitude below any real deadline. A monotonic value used as an expiry cut does
-// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
+// not skew the answer, it disables expiry outright.
 inline int64_t now_realtime_ms() {
     timespec ts;
     clock_gettime(CLOCK_REALTIME, &ts);
diff --git a//home/user/Projects/cx-inline-comments/src/core/thread.h b//home/user/Projects/cx-inline-comments/src/core/thread.h
index 0fcfa17ba6e331309260b641ec21aa4a8ff9ba36..3cf72c1b96ca700f602e2e7c630a1e6970f9620f
--- a//home/user/Projects/cx-inline-comments/src/core/thread.h
+++ b//home/user/Projects/cx-inline-comments/src/core/thread.h
@@ -118,7 +118,7 @@
     ContextRoute,           // scatter/script/all-shard or conservative broad-owner route
     ContextKeymissNotify,   // MGET miss must retain owner-side notification behavior
     InflightWrite,
-    ArmTransient,           // pre-arming writes still in flight (DESIGN-RINGDIET.md)
+    ArmTransient,           // pre-arming writes still in flight
     AtomicPending,
     Missing,
     Typed,
@@ -151,7 +151,7 @@
     // ARM-ON-DEMAND TRANSIENT. Reads demoted because the connection carried writes published
     // BEFORE a local read armed it -- writes the RYOW ring deliberately never recorded. Bounded
     // per connection by one ROB drain and reported separately from the steady-state key conflict
-    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
+    // above so that the two can never be confused in a bench.
     uint64_t fallback_arm_transient = 0;
     uint64_t fallback_atomic_pending = 0;
     uint64_t fallback_missing = 0;
@@ -160,7 +160,7 @@
     uint64_t fallback_seq_churn = 0;
     uint64_t fallback_generation = 0;
     uint64_t fallback_lane_full = 0;
-    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
+    // Lane ADMISSION deferrals. Frames the armed parser left unconsumed at rpos, to be
     // re-parsed by a later pass of the same thread, because the local-read lane had no room
     // (defer_lane_full) or because the connection already held its fair share of a lane under
     // pressure (defer_quota). Neither is a fallback: the read still completes locally, and no

codex
Completed in `/home/user/Projects/cx-inline-comments`.

- [Diff](\/home/user/Projects/cx-inline-comments/INLINE.patch): 15 references inlined, 50 dropped after review.
- [INLINE.md](\/home/user/Projects/cx-inline-comments/INLINE.md): all 66 references, accuracy findings, and 13 archive retention candidates.

The remaining reference is inside the WAITAOF error string; changing it would violate the comments-only constraint.

Non-comment tokens are unchanged. Diff and patch checks pass. No builds, servers, benchmarks, or gate runs performed.
diff --git a//home/user/Projects/cx-inline-comments/src/core/config.h b//home/user/Projects/cx-inline-comments/src/core/config.h
index 495e6ae42da15ee4bcc76a8a846efeef506903d7..8b5f3e5c786e8eeae0296a5dcf19338056d59007
--- a//home/user/Projects/cx-inline-comments/src/core/config.h
+++ b//home/user/Projects/cx-inline-comments/src/core/config.h
@@ -290,8 +290,8 @@
     uint32_t client_lb = 1;
 
     // ---- automatic role split (boot-latched) -----------------------------------------------
-    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer
-    // (DESIGN-flipfp.md); its measured sampling policy is internal to flipctl.h. Off and 1s
+    // Ships dark. The controller alone arms the owner-local sampled fingerprint writer;
+    // its measured sampling policy is internal to flipctl.h. Off and 1s
     // dispatch pay one predicted branch per op and no fingerprint store. Trigger bands learn
     // from the anchor's own quiet jitter; no separate controller tuning knobs remain.
     uint32_t flip_auto = 0;
diff --git a//home/user/Projects/cx-inline-comments/src/core/flipctl.h b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
index 5185da4085ae5f2e3325702064208810cb9c42f7..00b11f3e799ae7313e3ce48ae074715e0bbb7968
--- a//home/user/Projects/cx-inline-comments/src/core/flipctl.h
+++ b//home/user/Projects/cx-inline-comments/src/core/flipctl.h
@@ -19,7 +19,9 @@
 
 class Server;
 
-// Preserve the measured sampler (DESIGN-flipfp.md): one whole parse pass in 100 on average.
+// Preserve the measured sampler: one whole parse pass in 100 on average. At a matched 512k ops/s,
+// a GET-only -> SET-only swap triggered the fingerprint detector once with both exhaustive and
+// 1-in-100 sampling; the sampled writer still detected the shift with the rate held fixed.
 // Arming derives solely from the controller latch. No runtime sampling knob or adaptive
 // per-operation work is needed; the detector already derives its bands from actual samples.
 inline constexpr uint32_t flip_fingerprint_window(bool controller_enabled) {
@@ -52,9 +54,11 @@
     uint64_t closed_windows = 0;
 };
 
-// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
-// frame -- one load of this writer's own word, on the line the old enabled() test already read,
-// and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
+// SAMPLED BY PARSE PASS. A per-op countdown would restore the store this sampler removes: no
+// existing io-side command counter covers both the point paths and all io-local commands.
+// The io loop asks pass_sampled() once per dispatched frame -- one load of this writer's own word,
+// on the line the old enabled() test already read, and one predicted-false branch; no store.
+// Inside a sampled pass the full fingerprint body runs
 // for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
 // that pass; at the pass end finish_parse_pass() publishes it and the caller draws the gap to the
 // next sampled pass. `work_window_` is the mean commands per fingerprint
diff --git a//home/user/Projects/cx-inline-comments/src/core/server.h b//home/user/Projects/cx-inline-comments/src/core/server.h
index f869dd3d1737afa6b1c401919fb66c96772b1225..a168d1656a19c4b098167904d42f814caae7513c
--- a//home/user/Projects/cx-inline-comments/src/core/server.h
+++ b//home/user/Projects/cx-inline-comments/src/core/server.h
@@ -8,8 +8,10 @@
 //
 // THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
 // dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
-// its derived shard-granularity fast path, published only at a quiesced commit. See
-// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
+// its derived shard-granularity fast path, published only at a quiesced commit. Shards move only
+// after executing, deferred, group and pinned-read work drains; destination routes publish after
+// the ownership edge. An IO may enqueue using an old route after that edge, so tasks and borrow
+// releases recheck ownership and forward before shard access instead of carrying routing epochs.
 #pragma once
 #include <algorithm>
 #include <atomic>
@@ -308,7 +310,7 @@
         for (uint32_t i = 0; i < nthreads; i++) {
             threads_[i] = std::make_unique<ThreadCtx>();
             // The fingerprint writer is armed only when its one reader, the flip controller, is
-            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
+            // enabled: with --flip-auto 0 and in 1s mode it is dark and costs
             // one predicted branch per op. The enabled sampler keeps its measured 1-in-100 policy.
             threads_[i]->init(i, placement_.role_of(i), nthreads,
                               0,
@@ -1969,7 +1971,7 @@
         shard_owner_[shard_id].store(thread_id, std::memory_order_release);
     }
 
-    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
+    // The caller must hold both executor loops at the safe point:
     // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
     // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
     // Vector capacity and membership are settled while PREPARING still names the source.  The
@@ -1988,7 +1990,7 @@
     // lost, a block ends up linked twice, and the damage surfaces later as either
     // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
     // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
-    // Both were observed (see DESIGN-P0REPLY.md).
+    // Both were observed.
     //
     // The rebind used to be deferred to the destination's own next executor pass
     // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
@@ -2827,7 +2829,7 @@
     // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
     // other command. It widens -- deterministically -- the window in which a younger whole-owner
     // walker could overtake an older same-connection group on the destination shard.
-    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
+    // TEST HOOK: effective local-read LANE CAPACITY for admission. 0 means
     // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
     // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
     // its masking, so this can never overrun anything. It exists because lane oversubscription is
@@ -2935,9 +2937,11 @@
     // is already set. Nothing on GET/SET touches it.
     //
     // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
-    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
-    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
-    // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
+    // sequence: a blocking op requires an empty ROB, then sets the barrier before any younger
+    // frame can be parsed; a resumed scatter inherits that same slot and barrier. The state the
+    // owner-scoped release exists to survive -- a release that must NOT drop the barrier -- has
+    // to be injected. It is held past ROB quiescence on purpose; barrier_release_quiesced()
+    // exempts this one bit for that reason.
     //
     // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
     // nothing left to do and parks on submit_and_wait(1) -- unbounded under io_uring. A hold that
@@ -2968,12 +2972,11 @@
         return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
     }
     // An overlap: some owner took the parse barrier while another owner already held it. With
-    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
-    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
-    // live and the owner-scoped release in blocking_retire() became load-bearing rather than
-    // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
-    // own positive control: a "must be zero" reading proves nothing until something has been shown
-    // able to make it non-zero.
+    // DEBUG BARRIER-HOLD off this must read ZERO -- if it ever moves on production traffic the
+    // latent case just went live and the owner-scoped release in blocking_retire() became
+    // load-bearing rather than defensive. With the latch ON it advances once per blocking dispatch,
+    // which is the counter's own positive control: a "must be zero" reading proves nothing until
+    // something has been shown able to make it non-zero.
     void note_barrier_overlap() {
         barrier_owner_overlaps_.fetch_add(1, std::memory_order_relaxed);
     }
diff --git a//home/user/Projects/cx-inline-comments/src/core/shard.h b//home/user/Projects/cx-inline-comments/src/core/shard.h
index 694b9a93c6cd63db49b9dcf061dbb5182c49986a..39a049d2f8619bbf52f706775956cc9198060364
--- a//home/user/Projects/cx-inline-comments/src/core/shard.h
+++ b//home/user/Projects/cx-inline-comments/src/core/shard.h
@@ -431,7 +431,7 @@
         // NOT share this counter: multi.inc's prepare_write_key() re-asks it at install time and
         // reports violations through INFO atomic_exec_order_late (multi_exec_order_late()), which
         // must read zero. Summing the two would hide a violation inside a number that is non-zero
-        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
+        // by design, leaving the park's safety argument with no falsifier.
         uint64_t atomic_exec_order_holds = 0;
         // Times watch_finalize_reservation() answered "not ready" because the reservation's epoch
         // was still 0, i.e. a unit was turned into a Retry by an undecided WATCH reservation. It
diff --git a//home/user/Projects/cx-inline-comments/src/core/signal.h b//home/user/Projects/cx-inline-comments/src/core/signal.h
index 933a5d7ee09538f3923df3c5eb77f9c72cdc0bfa..f69f7d73942ee129f4ad9cd1f23befe69fc4167d
--- a//home/user/Projects/cx-inline-comments/src/core/signal.h
+++ b//home/user/Projects/cx-inline-comments/src/core/signal.h
@@ -47,7 +47,7 @@
 // that will be compared against a deadline must come from here and never from now_ns() above --
 // that one is CLOCK_MONOTONIC, so its "milliseconds" are milliseconds since boot and sit roughly
 // five orders of magnitude below any real deadline. A monotonic value used as an expiry cut does
-// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
+// not skew the answer, it disables expiry outright.
 inline int64_t now_realtime_ms() {
     timespec ts;
     clock_gettime(CLOCK_REALTIME, &ts);
diff --git a//home/user/Projects/cx-inline-comments/src/core/thread.h b//home/user/Projects/cx-inline-comments/src/core/thread.h
index 0fcfa17ba6e331309260b641ec21aa4a8ff9ba36..3cf72c1b96ca700f602e2e7c630a1e6970f9620f
--- a//home/user/Projects/cx-inline-comments/src/core/thread.h
+++ b//home/user/Projects/cx-inline-comments/src/core/thread.h
@@ -118,7 +118,7 @@
     ContextRoute,           // scatter/script/all-shard or conservative broad-owner route
     ContextKeymissNotify,   // MGET miss must retain owner-side notification behavior
     InflightWrite,
-    ArmTransient,           // pre-arming writes still in flight (DESIGN-RINGDIET.md)
+    ArmTransient,           // pre-arming writes still in flight
     AtomicPending,
     Missing,
     Typed,
@@ -151,7 +151,7 @@
     // ARM-ON-DEMAND TRANSIENT. Reads demoted because the connection carried writes published
     // BEFORE a local read armed it -- writes the RYOW ring deliberately never recorded. Bounded
     // per connection by one ROB drain and reported separately from the steady-state key conflict
-    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
+    // above so that the two can never be confused in a bench.
     uint64_t fallback_arm_transient = 0;
     uint64_t fallback_atomic_pending = 0;
     uint64_t fallback_missing = 0;
@@ -160,7 +160,7 @@
     uint64_t fallback_seq_churn = 0;
     uint64_t fallback_generation = 0;
     uint64_t fallback_lane_full = 0;
-    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
+    // Lane ADMISSION deferrals. Frames the armed parser left unconsumed at rpos, to be
     // re-parsed by a later pass of the same thread, because the local-read lane had no room
     // (defer_lane_full) or because the connection already held its fair share of a lane under
     // pressure (defer_quota). Neither is a fallback: the read still completes locally, and no

tokens used
133,790
Completed in `/home/user/Projects/cx-inline-comments`.

- [Diff](\/home/user/Projects/cx-inline-comments/INLINE.patch): 15 references inlined, 50 dropped after review.
- [INLINE.md](\/home/user/Projects/cx-inline-comments/INLINE.md): all 66 references, accuracy findings, and 13 archive retention candidates.

The remaining reference is inside the WAITAOF error string; changing it would violate the comments-only constraint.

Non-comment tokens are unchanged. Diff and patch checks pass. No builds, servers, benchmarks, or gate runs performed.

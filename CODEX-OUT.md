Reading additional input from stdin...
OpenAI Codex v0.153.4
--------
workdir: /home/user/Projects/cx-rl2s
model: gpt-6-astra
provider: openai
approval: never
sandbox: danger-full-access
reasoning effort: max
reasoning summaries: none
session id: 01a07b16-a34e-7832-9df6-d4b51bc3ca20
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

You are verifying and, if needed, correcting work already in this worktree: the extension of the local-read
lane to split (2s) thread mode. Keep the work; prove or fix the property below. Do not build, do not run the
server, benchmarks or the gate — the maintainer runs those.

THE MAINTAINER'S HARD REQUIREMENT: the new behaviour activates ONLY when BOTH `--thread-mode 2s` AND
`--read-local 1` are set. With `--thread-mode 2s --read-local 0`, the server must be EXACTLY what it is today.

Your activation predicate already tests `read_local`. That is necessary and not sufficient. The failure this
requirement guards against is not a wrong predicate, it is CONSTRUCTION that happens regardless of it: a
shard-less fused tier built and then left idle, fused machinery allocated per thread, a retire sink or QSBR
registration created for threads that will never read, a branch added to a loop that runs in both settings, or
a struct grown so that the off arm pays cache cost for a field only the on arm uses.

PROVE, with file:line evidence, for the `--thread-mode 2s --read-local 0` configuration:
1. No thread is constructed with capabilities it did not have before this change, and the thread role and
   count are what they were.
2. Nothing is allocated that was not allocated before — walk every `new`, container reserve, buffer, sink and
   registration your diff adds, and show it is behind the predicate.
3. No hot-path loop gains a branch. Where you added a test, show it is either outside the per-operation path or
   compiled out / hoisted for the off arm. Remember this tree's rule that an argument to a call that
   early-returns is still evaluated and still costs a cache line.
4. No struct on the per-operation path changed size. If one did, name it, give the old and new sizes and the
   layout assert, and say what the off arm now pays.

If any of the four fails, FIX IT so the off arm is untouched, and say what you changed. If all four hold, say
so and point at the evidence for each — the maintainer will boot both settings and gate them, and a claim that
does not survive that is worse than an admission now.

DELIVERABLE: update RL2S.md with a section titled "OFF-ARM PROOF" containing the four answers, plus any fix.
codex
I’ll compare the local-read changes with their baseline, trace what the 2s/off configuration constructs and executes, and fix any off-arm costs I find. I’ll document the evidence in `RL2S.md` without building or running anything.
exec
/bin/bash -lc "pwd && rg --files -g 'AGENTS.md' -g 'RL2S.md' -g 'CMakeLists.txt' -g 'Makefile' -g '*read*local*' -g '*rl2s*' -g '.git' && git status --short && git branch --show-current && git rev-parse --show-toplevel && git log -6 --oneline" in /home/user/Projects/cx-rl2s
 succeeded in 0ms:
/home/user/Projects/cx-rl2s
Makefile
src/core/rl2s.cc
src/core/read_local.h
src/store/read_local_settax.h
src/store/read_local_reclaim.h
.git
RL2S.md
tests/read_local_ring_unit.cc
tests/read_local_lane.py
tests/rl2s.py
tests/read_local_write_ring_unit.cc
 M Makefile
 A RL2S.md
 M src/cmd/t_server.cc
 M src/core/config.h
 M src/core/ex_loop.h
 M src/core/genthread.cc
 M src/core/genthread.h
 M src/core/io_loop.h
 A src/core/rl2s.cc
 M src/core/server.h
 M src/core/thread.h
 M src/main.cc
 M tests/bplus.py
 M tests/config_parser_test.cc
 M tests/read_local_lane.py
 A tests/rl2s.py
 M tests/rlcache_churn.py
?? CODEX-OUT.md
cx-rl2s
/home/user/Projects/cx-rl2s
78c3e5391 atomic_torn: re-roll the OFF torn-RENAME control instead of failing a run it did not manifest in
48b9c3a23 multirace: re-arm the hazard window instead of failing a run that never entered it
0187d303a Merge branch 't-flipdamp-r' into t-merge14: the io:ex controller decides from measured cost
d88c098a1 notes: rebase onto the fingerprint diet, the quantum collision, combined-tree acceptance
b40026236 Merge branch 't-ringdiet' into t-merge14: the RYOW ring arms on demand
f13c3f91c merge t-flipdamp (flip actuator redesign) onto the fingerprint diet

exec
/bin/bash -lc 'git diff HEAD -- src/core/config.h src/core/ex_loop.h src/core/io_loop.h src/core/server.h src/core/rl2s.cc src/cmd/t_server.cc Makefile
cat RL2S.md
cat CODEX-OUT.md' in /home/user/Projects/cx-rl2s
exec
/bin/bash -lc "git diff HEAD -- tests/config_parser_test.cc tests/read_local_lane.py tests/rl2s.py tests/bplus.py tests/rlcache_churn.py
rg --files -g 'AGENTS.md' -g '*layout*' -g '*qsbr*' -g '*retire*' -g '*local*' src tests" in /home/user/Projects/cx-rl2s
 succeeded in 0ms:
diff --git a/tests/bplus.py b/tests/bplus.py
index a356c1585..21aa16c1e 100644
--- a/tests/bplus.py
+++ b/tests/bplus.py
@@ -2,7 +2,7 @@
 """Directed B+ per-key atomic-filter gate. Usage: tests/bplus.py HOST PORT
 
 Boot requirement:
-  --thread-mode 1s --overlap 0 --read-local 1 --read-local-atomic-filter 1
+  --thread-mode 1s|2s --overlap 0 --read-local 1 --read-local-atomic-filter 1
   --atomic 1 --enable-debug-command yes
 
 The test does not infer routing from key names. DEBUG SHARD/LBSIGNALS select a real cross-owner
@@ -177,25 +177,28 @@ def topology(connection):
         raise AssertionError(
             "DEBUG LBSIGNALS unavailable; boot with --enable-debug-command yes: %r" % raw)
     threads = set()
+    readers = set()
     shard_owner = {}
     for line in raw.splitlines():
         fields = line.split()
-        # The role token is presentation (`io`, `ex`, or `fused`), not ownership. Thread rows
-        # establish the live tids; shard rows below are the only ownership oracle.
+        # Roles identify client-serving readers (`io` or `fused`); shard rows below
+        # remain the only ownership oracle, including for fused threads.
         if len(fields) >= 3 and fields[0] == b"thread":
             threads.add(int(fields[1]))
+            if fields[2] in (b"io", b"fused"):
+                readers.add(int(fields[1]))
         elif len(fields) >= 3 and fields[0] == b"shard":
             shard_owner[int(fields[1])] = int(fields[2])
     if len(threads) < 3:
         raise AssertionError(
-            "B+ held-group geometry needs two participating owners and one independent fused "
+            "B+ held-group geometry needs two participating owners and one independent local "
             "reader; found live threads %r" % sorted(threads))
     if not shard_owner:
         raise AssertionError("DEBUG LBSIGNALS reported no shard ownership")
     unknown = set(shard_owner.values()) - threads
     if unknown:
         raise AssertionError("shards name non-live owners %r" % sorted(unknown))
-    return threads, shard_owner
+    return readers, shard_owner
 
 
 def debug_shard(connection, key):
@@ -526,12 +529,13 @@ def main():
     retained = []
     try:
         expected_config = {
-            "thread-mode": b"1s",
             "overlap": b"0",
             "read-local": b"1",
             "read-local-atomic-filter": b"1",
             "atomic": b"1",
         }
+        if config_value(discovery, "thread-mode") not in (b"1s", b"2s"):
+            raise AssertionError("B+ test requires 1s or 2s")
         for name, wanted in expected_config.items():
             got = config_value(discovery, name)
             if got != wanted:
diff --git a/tests/config_parser_test.cc b/tests/config_parser_test.cc
index d25fe656c..4c9a15bbf 100644
--- a/tests/config_parser_test.cc
+++ b/tests/config_parser_test.cc
@@ -345,7 +345,10 @@ int main() {
         tomo::validate_config(read_local_split) != tomo::kConfigParsed ||
         read_local_split.thread_mode != tomo::ThreadMode::Split ||
         read_local_split.read_local != 1)
-        fail("read-local split-mode inert setting was rejected");
+        fail("read-local split-mode lane was rejected");
+    if (rejection_text({"--thread-mode", "2s", "--read-local", "1", "--overlap", "1"}, true) !=
+        "--thread-mode 2s --read-local 1 requires --overlap 0\n")
+        fail("split read-local silently accepted an unreachable overlap lane");
     auto parses_read_local_fallback_cell = [](const char* overlap, uint32_t expected) {
         tomo::Config cfg;
         tomo::ConfigParseState state;
diff --git a/tests/read_local_lane.py b/tests/read_local_lane.py
index eaf922500..607798b42 100755
--- a/tests/read_local_lane.py
+++ b/tests/read_local_lane.py
@@ -2,7 +2,7 @@
 """Armed local-read LANE ADMISSION battery (P128.md).
 
 Usage: read_local_lane.py HOST PORT
-  boot: --thread-mode 1s --read-local 1 --enable-debug-command yes
+  boot: --thread-mode 1s|2s --overlap 0 --read-local 1 --enable-debug-command yes
 
 The fused thread's local-read lane holds kInboxSlots (1024) entries. A connection may pipeline up
 to kRobWindow (64) ops, so a thread that has accepted more than 16 deep-pipelining connections can
@@ -105,11 +105,12 @@ def drive(conns, shared, values, first_round, rounds):
 def main():
     host, port = _lib.host_port()
     ctl = _lib.Conn(host, port)
-    if _lib.thread_mode(ctl) != "1s":
-        _lib.skip_all("lane admission exists only in fused (1s) mode")
     cfg = ctl.cmd("CONFIG", "GET", "read-local")
     if not (isinstance(cfg, list) and len(cfg) == 2 and cfg[1] == b"1"):
         _lib.skip_all("needs --read-local 1 (CONFIG GET read-local -> %r)" % (cfg,))
+    server = _lib.info(ctl, "server")
+    if server.get("read_local") != "1" or int(server["read_local_active_threads"]) == 0:
+        raise AssertionError("read-local was requested but no reader lane is active: %r" % server)
     probe = ctl.cmd("DEBUG", "READ-LOCAL-LANE-CAP", "0")
     if isinstance(probe, Exception) or probe != b"OK":
         _lib.skip_all("needs DEBUG READ-LOCAL-LANE-CAP (--enable-debug-command yes) -> %r"
diff --git a/tests/rl2s.py b/tests/rl2s.py
new file mode 100644
index 000000000..3c68636cf
--- /dev/null
+++ b/tests/rl2s.py
@@ -0,0 +1,185 @@
+#!/usr/bin/env python3
+"""Directed split local-read activation, owner routing, RYOW and FLIP checks.
+
+Run against a maintainer-started, otherwise idle server:
+  tests/rl2s.py HOST PORT
+  --thread-mode 2s --read-local 1 --overlap 0 --shards 16 --ratio 6:2
+  --enable-debug-command yes --key-lb 0 --client-lb 0 --flip-auto 0
+
+This script never launches a server. Wrong geometry or a lane that never fires
+is a failure, including failure to reach every IO thread after bounded fresh
+connections. FLIP is restored in finally. Keys use a unique test prefix.
+"""
+import time
+
+import _lib
+
+
+def require(ok, detail):
+    if not ok:
+        raise AssertionError(detail)
+
+
+def pipeline(conn, commands):
+    conn.raw(b"".join(_lib.encode(*command) for command in commands))
+    return [conn.read() for _ in commands]
+
+
+def rows(conn):
+    # Boot/FLIP readiness precedes the first instruction in a new IO loop. Give
+    # that entry a bounded chance to publish; a configured-but-dark lane fails.
+    for _ in range(200):
+        info = _lib.info(conn, "server")
+        if int(info["read_local_active_threads"]) == int(info["io_threads"]):
+            break
+        time.sleep(0.01)
+    require(info["thread_mode"] == "2s" and info["read_local"] == "1",
+            "requires an effective split local-read lane")
+    result = {}
+    for key, value in info.items():
+        if key.startswith("read_local_thread_"):
+            row = dict(part.split("=", 1) for part in value.split(","))
+            result[int(key.removeprefix("read_local_thread_"))] = row
+    io = {tid for tid, row in result.items() if row["role"] == "ifid"}
+    ex = {tid for tid, row in result.items() if row["role"] == "ex"}
+    require(len(io) == int(info["io_threads"]) and len(ex) == int(info["ex_threads"]),
+            "INFO role rows disagree with split geometry")
+    require(len(io) == int(info["read_local_active_threads"]),
+            "some IO reader loops never activated")
+    require(all(row["active"] == ("1" if tid in io else "0")
+                and (tid not in io or row["shards"] == "0")
+                for tid, row in result.items()),
+            "IO owns shards, or lane activation disagrees with roles")
+    topology = _lib.topology(conn)
+    require(set(topology.shard_owner.values()) <= ex, "a shard owner is in the IO tier")
+    for tid, row in result.items():
+        require(int(row["shards"]) == list(topology.shard_owner.values()).count(tid),
+                "INFO shard counts disagree with published ownership")
+    return result, io, ex
+
+
+def counters(conn):
+    info = _lib.info(conn, "stats")
+    names = ("read_local_hits", "read_local_mget_local_hits", "read_local_fallbacks",
+             "read_local_arms", "read_local_write_ring_sidecars", "read_local_write_ring_records")
+    return {name: int(info[name]) for name in names}
+
+
+def owner_ops(conn):
+    return sum(shard.ops for shard in _lib.lbsignals(conn).shards)
+
+
+def local_round(conn, ctl, a, b):
+    # Warm arming and retire any setup writes before the measured clean run.
+    require(conn.cmd("GET", a) == b"a", "GET warmup failed")
+    before = counters(ctl)
+    ex_before = owner_ops(ctl)
+    commands = [("GET", a)] * 32 + [("MGET", a, b)] * 32
+    require(pipeline(conn, commands) == [b"a"] * 32 + [[b"a", b"b"]] * 32,
+            "clean GET/MGET replies differ")
+    after = counters(ctl)
+    require(after["read_local_hits"] - before["read_local_hits"] == 64,
+            "clean commands silently used owner tasks")
+    require(after["read_local_mget_local_hits"] - before["read_local_mget_local_hits"] == 32,
+            "MGET never entered the lane")
+    require(after["read_local_fallbacks"] == before["read_local_fallbacks"],
+            "uncontended clean reads fell back")
+    require(owner_ops(ctl) == ex_before, "local reads executed on shard owners")
+
+
+def exercise_readers(host, port, ctl, a, b):
+    initial, io, _ = rows(ctl)
+    found = {}
+    previous = initial
+    try:
+        # Each retry is a fresh connection, so SO_REUSEPORT gets another independent choice.
+        # Exhaustion fails; we never bless a subset of the IO tier as full activation.
+        for _ in range(32 + 16 * len(io)):
+            conn = _lib.Conn(host, port, timeout=20)
+            try:
+                local_round(conn, ctl, a, b)
+                current, current_io, _ = rows(ctl)
+                require(current_io == io, "unexpected role change during directed reads")
+                advanced = [tid for tid in io
+                            if int(current[tid]["mget_hits_total"]) >
+                            int(previous[tid]["mget_hits_total"])]
+                require(len(advanced) == 1, "cannot attribute reads to exactly one IO thread")
+                tid = advanced[0]
+                previous = current
+                if tid not in found:
+                    found[tid] = conn
+                    conn = None
+                if set(found) == io:
+                    break
+            finally:
+                if conn is not None:
+                    conn.close()
+        require(set(found) == io, "fresh connections never fired lanes on IO tids %r" % (io - set(found)))
+
+        # Overlapping writes, reads and MGETs across several ROB windows in one
+        # wire burst. This checks exact RYOW replies; it does not claim a write-ring
+        # overflow, which also depends on concurrent owner progress.
+        for conn in found.values():
+            commands, expected = [], []
+            value_a, value_b = b"a", b"b"
+            for n in range(40):
+                value_a = ("a-%d" % n).encode()
+                commands += [("SET", a, value_a), ("GET", a), ("MGET", a, b)]
+                expected += [b"OK", value_a, [value_a, value_b]]
+                value_b = ("b-%d" % n).encode()
+                commands += [("SET", b, value_b), ("MGET", a, b)]
+                expected += [b"OK", [value_a, value_b]]
+            require(pipeline(conn, commands) == expected, "pipelined RYOW or reply order broke")
+            require(pipeline(conn, [("SET", a, b"a"), ("SET", b, b"b")]) == [b"OK"] * 2,
+                    "restore after RYOW failed")
+            local_round(conn, ctl, a, b)
+    finally:
+        for conn in found.values():
+            conn.close()
+
+
+def main():
+    host, port = _lib.host_port()
+    ctl = _lib.Conn(host, port, timeout=30)
+    for knob in ("key-lb", "client-lb", "flip-auto"):
+        require(ctl.cmd("CONFIG", "GET", knob) == [knob.encode(), b"0"],
+                "directed RL2S test requires --%s 0" % knob)
+    _, io, ex = rows(ctl)
+    unit = int(_lib.info(ctl, "server")["flip_unit_threads"])
+    require(len(ex) > unit, "requires at least two EX scheduling units for the FLIP round trip")
+    prefix = "rl2s:%d:" % time.time_ns()
+    a, b, _, _ = _lib.cross_owner_pair(ctl, prefix)
+    require(pipeline(ctl, [("SET", a, b"a"), ("SET", b, b"b")]) == [b"OK"] * 2,
+            "seed failed")
+    try:
+        # A new pure-writer connection must neither arm nor allocate an RYOW sidecar.
+        writer = _lib.Conn(host, port, timeout=20)
+        try:
+            before, ex_before = counters(ctl), owner_ops(ctl)
+            require(pipeline(writer, [("SET", a, b"a")] * 64) == [b"OK"] * 64,
+                    "pure SET failed")
+            require(counters(ctl) == before, "pure SET armed or entered a reader lane")
+            require(owner_ops(ctl) - ex_before == 64, "SET did not execute on owners")
+        finally:
+            writer.close()
+        exercise_readers(host, port, ctl, a, b)
+        require(ctl.cmd("FLIP", len(io) + unit, len(ex) - unit) == b"OK", "EX -> IO failed")
+        _, grown_io, grown_ex = rows(ctl)
+        require(len(grown_io) == len(io) + unit and len(grown_ex) == len(ex) - unit,
+                "FLIP did not change actual geometry")
+        exercise_readers(host, port, ctl, a, b)
+        require(ctl.cmd("FLIP", len(io), len(ex)) == b"OK", "IO -> EX failed")
+        _, restored_io, restored_ex = rows(ctl)
+        require(len(restored_io) == len(io) and len(restored_ex) == len(ex), "roles not restored")
+        exercise_readers(host, port, ctl, a, b)
+        print("ok: RL2S owner routing, every IO lane, GET/MGET, RYOW and FLIP round trip")
+    finally:
+        try:
+            require(ctl.cmd("FLIP", len(io), len(ex)) == b"OK", "cleanup could not restore roles")
+            ctl.cmd("DEL", a, b)
+        finally:
+            ctl.close()
+
+
+if __name__ == "__main__":
+    main()
diff --git a/tests/rlcache_churn.py b/tests/rlcache_churn.py
index 24cd42ca8..7fff7ecaa 100644
--- a/tests/rlcache_churn.py
+++ b/tests/rlcache_churn.py
@@ -2,7 +2,7 @@
 """ARMED-WRITE BLOCK CACHE churn stressor (src/store/kv_block_cache.h).
 
 Usage: rlcache_churn.py HOST PORT [SECONDS] [WORKERS]
-  boot: --thread-mode fused --read-local 1 --atomic 1 --enable-debug-command yes
+  boot: --thread-mode 1s|2s --read-local 1 --atomic 1 --enable-debug-command yes
 
 WHAT THIS DRIVES, AND WHY THE DIFFERENTIAL MATRIX DOES NOT. While read-local is armed a published
 object is immutable, so every write BUILDS a fresh object, publishes it, and retires the displaced
@@ -160,12 +160,15 @@ def main():
 
     ctl = _lib.Conn(host, port)
     mode = _lib.thread_mode(ctl)
-    if mode != "1s":
-        rep.bad("geometry", "needs --thread-mode fused, INFO says thread_mode:%s" % mode)
+    if mode not in ("1s", "2s"):
+        rep.bad("geometry", "unknown thread_mode:%s" % mode)
         return rep.finish()
     if _lib.info(ctl, "server").get("read_local") != "1":
         rep.bad("geometry", "needs --read-local 1")
         return rep.finish()
+    if _lib.info_int(ctl, "server", "read_local_active_threads") == 0:
+        rep.bad("geometry", "read-local enabled but no reader lane is active")
+        return rep.finish()
 
     before_hits = _lib.info_int(ctl, "all", "read_local_hits")
     # SHARD MOVES ARE THE PRECONDITION. The defect this battery exists for lives at the ownership
src/core/read_local.h
tests/read_local_write_ring_unit.cc
tests/read_local_lane.py
tests/read_local_ring_unit.cc
src/store/read_local_reclaim.h
src/store/read_local_settax.h

 succeeded in 0ms:
diff --git a/Makefile b/Makefile
index 342ed796f..6243c5266 100644
--- a/Makefile
+++ b/Makefile
@@ -25,6 +25,7 @@ SRC      += src/cmd/server_tail.cc src/cmd/slowlog.cc src/cmd/lcs.cc src/cmd/inf
 SRC      += src/cmd/lbsignals.cc
 SRC      += src/core/flipctl.cc
 SRC      += src/core/genthread.cc
+SRC      += src/core/rl2s.cc
 SRC      += src/cmd/cmdgap.cc
 SRC      += src/cmd/pfdebug.cc
 SRC      += src/cmd/cmdmeta.cc
diff --git a/src/cmd/t_server.cc b/src/cmd/t_server.cc
index 10d2501a7..2d5c81bfd 100644
--- a/src/cmd/t_server.cc
+++ b/src/cmd/t_server.cc
@@ -1977,8 +1977,8 @@ void cmd_info(Shard&, Op& op) {
         // stale -- and tooling depends on them. The NIC bench harness identifies the server it just
         // booted by reading process_id out of INFO, so its absence made every NIC cell fail with an
         // opaque "boot/cell FAIL" long before any measurement was taken.
-        // read_local is the EFFECTIVE lane state (fused, overlap 0, knob on) -- what a gate row
-        // must assert. CONFIG GET read-local echoes the knob even on a split boot where it is inert.
+        // read_local is the effective boot state. Actual loop entry and successful completions
+        // are separate observations: a configured but unreachable lane must be visible in INFO.
         appendf(body, "# Server\r\nredis_version:%s\r\ntomokv_version:%s\r\nredis_mode:standalone\r\n"
                       "thread_mode:%s\r\noverlap:%u\r\nthread_pipeline:%u\r\nread_local:%u\r\n"
                       "arch_bits:%zu\r\nmultiplexing_api:io_uring\r\nprocess_id:%lld\r\n"
@@ -1992,6 +1992,33 @@ void cmd_info(Shard&, Op& op) {
                 static_cast<unsigned>(g_server ? g_server->cfg().port : 0),
                 static_cast<unsigned long long>(uptime),
                 static_cast<unsigned long long>(uptime / 86400));
+        uint32_t active_readers = 0;
+        if (g_server) {
+            uint32_t owned[kMaxThreads] = {};
+            // Read the published ownership directory, never a vector FLIP may be rewriting.
+            for (uint32_t sid = 0; sid < g_server->nshards(); sid++)
+                owned[g_server->worker_of_shard(static_cast<int32_t>(sid))]++;
+            for (uint32_t tid = 0; tid < g_server->nthreads(); tid++) {
+                const ThreadCtx& thread = g_server->thread(tid);
+                const bool active = thread.read_local_lane_active();
+                active_readers += active;
+                const Role role = thread.role();
+                const char* name = role == Role::Idle ? "idle"
+                    : g_server->thread_mode() == ThreadMode::Fused ? "unified"
+                    : role == Role::Ifid ? "ifid" : "ex";
+                const ReadLocalStats* stats = g_server->read_local_enabled()
+                    ? &thread.read_local_stats() : nullptr;
+                // These per-thread totals are lifetime counters; INFO stats retains its existing
+                // RESETSTAT-relative aggregate counters. FLIP does not reset either sidecar.
+                appendf(body,
+                        "read_local_thread_%u:role=%s,shards=%u,active=%u,"
+                        "hits_total=%llu,mget_hits_total=%llu\r\n",
+                        tid, name, owned[tid], active ? 1u : 0u,
+                        static_cast<unsigned long long>(stats ? stats->hits : 0),
+                        static_cast<unsigned long long>(stats ? stats->mget_local_hits : 0));
+            }
+        }
+        appendf(body, "read_local_active_threads:%u\r\n", active_readers);
         if (g_server && g_server->thread_mode() == ThreadMode::Fused) {
             appendf(body,
                     "fused_threads:%u\r\nclient_threads:%u\r\nowner_threads:%u\r\n"
diff --git a/src/core/config.h b/src/core/config.h
index a8938dd29..b94df1d5d 100644
--- a/src/core/config.h
+++ b/src/core/config.h
@@ -317,7 +317,7 @@ struct Config {
     PersistIoEngine persist_io = PersistIoEngine::Uring;
     ThreadMode thread_mode = ThreadMode::Split;
     // Occupies the pre-existing padding before appendfilename, preserving Config and Server layout.
-    uint32_t read_local = 0;            // boot-only 0|1; 1s overlap-0 GET/MGET local-read lane
+    uint32_t read_local = 0;            // boot-only 0|1; overlap-0 GET/MGET lane in 1s or 2s IO
     const char* appendfilename = "appendonly.aof";
     const char* appenddirname = "appendonlydir";
     uint32_t auto_aof_rewrite_percentage = 100;
@@ -1139,7 +1139,7 @@ inline int parse_config_args(const std::vector<const char*>& args, Config& cfg,
                         "             --read-local-atomic-filter 0|1 (boot-only; default 1)\n"
                         "             (--thread-pipeline is an overlap alias)\n"
                         "             (split/fused are mode aliases)\n"
-                        "             (read-local is active only with 1s overlap 0)\n"
+                        "             (read-local requires overlap 0; 2s IO owns no shards)\n"
                         "  placement (2s; default = even io/ex split over all allowed cpus):\n"
                         "    --ratio io:ex               GLOBAL counts, spread evenly over L3 domains\n"
                         "    --place role@cpu,...        explicit per-thread; roles are ifid, ex\n"
@@ -1212,6 +1212,10 @@ inline int validate_config(const Config& cfg) {
                      "--overlap 2 is only available with --thread-mode 1s; 2s has no deep unified-stream schedule\n");
         return kConfigError;
     }
+    if (cfg.thread_mode == ThreadMode::Split && cfg.read_local && cfg.overlap != 0) {
+        std::fprintf(stderr, "--thread-mode 2s --read-local 1 requires --overlap 0\n");
+        return kConfigError;
+    }
     if (cfg.thread_mode == ThreadMode::Fused && cfg.overlap != 0 &&
         cfg.net_io != NetIoEngine::Uring) {
         std::fprintf(stderr,
diff --git a/src/core/ex_loop.h b/src/core/ex_loop.h
index 296e3fe56..d90f01e66 100644
--- a/src/core/ex_loop.h
+++ b/src/core/ex_loop.h
@@ -394,6 +394,7 @@ public:
 
     uint32_t fused_baseline_pass() {
         static_assert(Fused);
+        if (srv_->thread_mode() == ThreadMode::Split) return split_read_local_pass();
         if (read_local_interleave_enabled())
             return fused_pass_impl<kGenthreadExBatchOps, true, false, false, true>();
         return fused_pass_impl<kGenthreadExBatchOps, true, false>();
@@ -636,6 +637,7 @@ public:
 
     uint32_t fused_baseline_sweep() {
         static_assert(Fused);
+        if (srv_->thread_mode() == ThreadMode::Split) return split_read_local_pass();
         if (read_local_interleave_enabled())
             return fused_sweep_impl<kGenthreadExBatchOps, true, false, false, true>();
         return fused_sweep_impl<kGenthreadExBatchOps, true, false>();
@@ -698,12 +700,29 @@ public:
 
     Ring& ring() { return ring_; }
 
+    // Defined with the split read-local runtime in rl2s.cc. The IO role never services owner
+    // tasks or shard housekeeping, even while FLIP installs its future EX shard vector.
+    uint32_t split_read_local_pass();
+
     void run() {
+        if constexpr (Fused) {
+            // Only RL2S instantiates the owner loop with the fused-capable executor. Its lane
+            // was drained before role conversion; owner commands use no local-read captures.
+            if (!read_local_enabled() || read_local_impl().lane_count != 0) std::abort();
+            self_->publish_read_local_parked(srv_->read_local_epoch());
+            // A preceding shard-less IO tenure may have consumed this CONFIG version without
+            // applying it to shards. Reapply once ownership is installed and dispatch resumes.
+            live_config_version_ = UINT64_MAX;
+        }
         LoopSignals& sig = self_->sig();
         uint32_t idle_spins = 0;
 
         while (!self_->stop_flag().load(std::memory_order_relaxed) &&
                self_->role() == Role::Ex) {
+#ifdef TOMO_RL_CACHE_DEBUG
+            if constexpr (Fused)
+                srv_->debug_assert_read_local_sinks_follow_ownership(self_->id());
+#endif
             cached_now_ms_ = realtime_ms();
             const bool flip_frozen = srv_->flip_stage() >= FlipStage::ExDrain;
             const bool lb_frozen = lb_controller_armed_ && srv_->lb_dispatch_paused();
@@ -778,6 +797,8 @@ public:
                     did += lb_control_pass();
                     lb_bucket_bytes_pass();
                 }
+                if constexpr (Fused)
+                    did += read_local_impl().deferred.drain_ready();
             }
             // A pass that found nothing -- every drain and control pass came back empty -- is
             // polling, not work. Book it as idle so busy_ns means WORK: the FLIP placement model
@@ -1889,22 +1910,25 @@ private:
         return wake_coordinator();
     }
 
+    template <bool OwnsShards = true>
     void refresh_live_config() {
         LiveConfigSnapshot snapshot;
         if (!srv_->live_config_snapshot_if_changed(live_config_version_, snapshot)) return;
         const bool enabled = snapshot.maxmemory != 0;
-        const uint64_t shard_limit = snapshot.maxmemory / srv_->nshards();
-        for (Shard* sh : self_->shards()) {
-            sh->configure_maxmemory(enabled, shard_limit, snapshot.policy, snapshot.samples);
-            // CLIENT TRACKING and periodic SAVE need the same per-write observation points as
-            // keyspace notifications, so they ride the shard mask as synthetic observer bits.
-            // notify_record expands those observers over NOTIFY_ALL without adding those class
-            // bits here: the operator's configured pub/sub classes therefore remain independent.
-            // NOTIFY_NEW and NOTIFY_KEY_MISS stay outside the observer surface: `new` would count
-            // or invalidate a mutation twice, and a key miss is not a value change.
-            sh->set_notify_mask(snapshot.notify_events |
-                                (snapshot.tracking_armed ? NOTIFY_TRACKING : 0u) |
-                                (snapshot.save_armed ? NOTIFY_SAVE : 0u));
+        if constexpr (OwnsShards) {
+            const uint64_t shard_limit = snapshot.maxmemory / srv_->nshards();
+            for (Shard* sh : self_->shards()) {
+                sh->configure_maxmemory(enabled, shard_limit, snapshot.policy, snapshot.samples);
+                // CLIENT TRACKING and periodic SAVE need the same per-write observation points as
+                // keyspace notifications, so they ride the shard mask as synthetic observer bits.
+                // notify_record expands those observers over NOTIFY_ALL without adding those class
+                // bits here: the operator's configured pub/sub classes therefore remain independent.
+                // NOTIFY_NEW and NOTIFY_KEY_MISS stay outside the observer surface: `new` would count
+                // or invalidate a mutation twice, and a key miss is not a value change.
+                sh->set_notify_mask(snapshot.notify_events |
+                                    (snapshot.tracking_armed ? NOTIFY_TRACKING : 0u) |
+                                    (snapshot.save_armed ? NOTIFY_SAVE : 0u));
+            }
         }
         maxmemory_enabled_ = enabled;
         foreign_touch_policy_ =
@@ -3413,6 +3437,7 @@ private:
 };
 
 using ExLoop = ExLoopT<false>;
+template <> uint32_t ExLoopT<true>::split_read_local_pass();
 using FusedExLoop = ExLoopT<true>;
 
 // Disabled split executors retain the exact pre-read-local allocation stride plus the 264-byte
diff --git a/src/core/io_loop.h b/src/core/io_loop.h
index f04718a40..b612cbce8 100644
--- a/src/core/io_loop.h
+++ b/src/core/io_loop.h
@@ -538,6 +538,16 @@ private:
             return;
         }
         constexpr bool IoPipe = !Fused && Pipeline == 1;
+        if constexpr (Fused) {
+            if (srv_->read_local_enabled()) {
+                // A split EX tenure is permanently parked: it never probes the local lane.
+                // Resume BEFORE sampling the epoch, as at the existing network-wait boundary.
+                if (ThreadCtx::read_local_publication_parked(self_->read_local_publication()))
+                    self_->resume_read_local_tick();
+                self_->publish_read_local_tick(srv_->read_local_epoch());
+                self_->set_read_local_lane_active(true);
+            }
+        }
         if constexpr (!kEp) {
             if (listen_fd_ >= 0) arm_accept(UrKind::Accept);
             if constexpr (HasTls) arm_accept(UrKind::TlsAccept);
@@ -754,10 +764,12 @@ private:
             self_->clear_blocked();
         }
         if constexpr (Fused) {
-            // The read loop is over permanently. Teardown below may take longer than another
+            // The read loop is over for this tenure. Teardown may take longer than another
             // owner's bounded retire queue can tolerate, but it performs no foreign store probe.
-            if (srv_->read_local_enabled())
+            if (srv_->read_local_enabled()) {
+                self_->set_read_local_lane_active(false);
                 self_->publish_read_local_parked(srv_->read_local_epoch());
+            }
         }
         // A close requested by the last pass's read/send path has no later flush_ready to drain it,
         // and an undrained entry would show up as a live connection in the shutdown accounting.
diff --git a/src/core/rl2s.cc b/src/core/rl2s.cc
new file mode 100644
index 000000000..d942a2e0f
--- /dev/null
+++ b/src/core/rl2s.cc
@@ -0,0 +1,308 @@
+// rl2s.cc — split placement with a shard-less fused-capable IO tier.
+// All reader-lane scheduling and split role-tenure wiring for this feature live here.
+// The ordinary split runtime and the 1s boot sequence remain independent.
+#include "genthread.h"
+
+#include <algorithm>
+#include <cstdio>
+#include <string>
+#include <thread>
+#include <utility>
+#include <vector>
+#include <unistd.h>
+
+#include "../base/alloc.h"
+#include "../cmd/acl.h"
+#include "../net/conn.h"
+#include "../net/unix_listener.h"
+#include "../persist/aof.h"
+#include "../snapshot/snapshot.h"
+#include "ex_loop.h"
+#include "fused_boot_gate.h"
+#include "io_loop.h"
+#include "server.h"
+#include "shutdown_report.h"
+#include "signal_doorbell.h"
+
+namespace tomo {
+namespace {
+
+void pin_read_local_thread(int cpu) {
+    if (cpu < 0) return;
+    cpu_set_t set;
+    CPU_ZERO(&set);
+    CPU_SET(cpu, &set);
+    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
+}
+
+}  // namespace
+
+// Same copy/validate/demote lane as 1s, with no owner work. In particular, do not
+// inspect self_->shards(): FLIP may be installing a future owner's vector while
+// this IO tenure is still returning its final acknowledgements. Live CONFIG's
+// reader fields are independent of that vector. Foreign pointers die before
+// publication, including when a local read is lowered to an owner task.
+template <>
+uint32_t ExLoopT<true>::split_read_local_pass() {
+    if (!read_local_enabled()) std::abort();
+    cached_now_ms_ = realtime_ms();
+    refresh_live_config<false>();
+    if (maxmemory_enabled_)
+        cached_lru_clock_ = static_cast<uint8_t>(
+            (static_cast<uint64_t>(cached_now_ms_ / 1000) >> lru_clock_shift_) & 0x1f);
+    uint32_t did = drain_local_reads();
+    // The shared fused drain books execution as well as parsing in LoopSignals::ops.
+    // Split IO's counter means parsed/dispatched commands (FLIP consumes that rate),
+    // so cancel that second charge once per pass. Successful reads remain visible
+    // in ReadLocalStats and command counts; EX still counts only owner work.
+    self_->sig().ops -= did;
+    did += read_local_impl().deferred.drain_ready();
+    auto& lane = read_local_impl();
+    if (__builtin_expect(lane.lane_pressure != 0, false)) lane.lane_pressure--;
+    const uint32_t cap = srv_->debug_read_local_lane_cap();
+    const uint16_t want = cap == 0 ? static_cast<uint16_t>(kInboxSlots)
+        : static_cast<uint16_t>(std::min<uint32_t>(cap, kInboxSlots));
+    if (__builtin_expect(lane.lane_admit_cap != want, false)) lane.lane_admit_cap = want;
+    self_->publish_read_local_tick(srv_->read_local_epoch());
+    return did;
+}
+
+int run_split_read_local_server(Server& srv, const SnapshotLoadPlan* aof_base_plan,
+                     const std::vector<std::unique_ptr<AofReplayPlan>>& aof_plans,
+                     const SnapshotLoadPlan* load_plan, TlsContext* tls_context,
+                     LateUnixListener& unix_listener,
+                     ShutdownReportFinalLine& final_report) {
+    const Config& cfg = srv.cfg();
+    const uint32_t nthreads = srv.nthreads();
+    if (cfg.thread_mode != ThreadMode::Split || !srv.read_local_enabled()) std::abort();
+    std::printf("tomokv-cpp: %u threads (%zu io + %zu ex), %u shard(s),"
+                " thread-mode=2s, overlap=0, read-local=1, %s, alloc=%s\n",
+                nthreads, srv.placement().ifid_threads().size(),
+                srv.placement().ex_threads().size(), cfg.shards,
+                cfg.net_io == NetIoEngine::Epoll ? "epoll" : "io_uring", alloc_backend());
+    for (const ThreadPlacement& placement : srv.placement().threads())
+        std::printf("  thread t%u: role=%s cpu=%d L3=%u shards=%zu read-local=%u\n",
+                    placement.id, placement.role == Role::Ifid ? "ifid" : "ex",
+                    placement.cpu, placement.domain, srv.thread(placement.id).shards().size(),
+                    placement.role == Role::Ifid ? 1u : 0u);
+    std::fflush(stdout);
+
+    std::vector<std::thread> pool;
+    std::vector<IoLoop> ios(nthreads);
+    std::vector<FusedExLoop> executors(nthreads);
+    // Reuse the 1s boot gate's stop-aware loading/listener barriers. Every physical thread binds
+    // its permanent sink and every owner arms its stores before any reader can enter the lane.
+    FusedBootGate boot(nthreads);
+    // Identical to placement().ifid_threads().front() whenever a unixsocket is configured, but
+    // read from the server so the LB's unix_owner_tid_ and the actual attach point cannot drift.
+    const uint32_t unix_owner = srv.unix_owner_tid();
+    auto report_graceful_shutdown = [&] {
+        if (srv.read_local_enabled()) {
+            // Joined readers cannot run another grace callback. Empty their private
+            // retirement queues and disarm every store hook before taking the immutable report;
+            // the subsequent atomic-record drain may detach more than a bounded callback list.
+            for (FusedExLoop& executor : executors) executor.read_local_shutdown_drain();
+            for (uint32_t sid = 0; sid < srv.nshards(); sid++)
+                srv.shard(static_cast<int32_t>(sid)).store().configure_read_local(false, {});
+        }
+        // All owners and readers are quiescent. Release pending-entry references before IoLoop
+        // destruction, then return their deferred ScatterState arenas to the correct IO-owned
+        // pools. Server normally outlives those pools, so leaving this to FlatStore destructors
+        // would leak the retained arenas.
+        for (uint32_t sid = 0; sid < srv.nshards(); sid++)
+            srv.shard(static_cast<int32_t>(sid)).store().atomic_shutdown_release_records();
+        for (IoLoop& io : ios) io.reap_atomic_deferred();
+        ShutdownReport report = collect_shutdown_report(srv, ios, executors);
+        print_shutdown_report_human(report);
+        final_report.arm(std::move(report));
+        acl_shutdown();
+    };
+
+    for (uint32_t tid = 0; tid < nthreads; tid++)
+        pool.emplace_back([&, tid] {
+            if (cfg.pin_threads) pin_read_local_thread(srv.placement().cpu_of_thread(tid));
+            ThreadCtx& self = srv.thread(tid);
+            self.latch_placement(srv.topo());
+            bind_thread_arena();
+            bool ok = self.init_task_inbox_local(srv.placement().ifid_threads(),
+                                                 srv.placement().ex_threads());
+            if (ok) ok = executors[tid].init(&srv, &self, true);
+            std::string local_error;
+            if (ok && aof_base_plan)
+                ok = snapshot_load_owned(*aof_base_plan, srv, self, local_error);
+            if (ok && !aof_plans.empty()) {
+                for (const auto& plan : aof_plans) {
+                    if (!aof_load_owned(*plan, srv, self, local_error)) {
+                        ok = false;
+                        break;
+                    }
+                }
+            } else if (ok && load_plan) {
+                ok = snapshot_load_owned(*load_plan, srv, self, local_error);
+            }
+            if (ok)
+                ok = ios[tid].init(&srv, &self, cfg.bind_addr, cfg.port, -1,
+                                   tls_context, true);
+            if (ok)
+                self.bind_io_role_hooks(
+                    &ios[tid],
+                    [](void* p) { return static_cast<IoLoop*>(p)->prepare_activation(); },
+                    [](void* p) { static_cast<IoLoop*>(p)->cancel_prepared_activation(); });
+            if (ok)
+                self.bind_client_registration_hooks(
+                    [](void* p, Client* client) {
+                        return static_cast<IoLoop*>(p)->prepare_client_registration(client);
+                    },
+                    [](void* p, Client* client) {
+                        static_cast<IoLoop*>(p)->cancel_client_registration(client);
+                    });
+            if (ok)
+                self.bind_client_capacity_hook(
+                    [](void* p, uint32_t incoming) {
+                        return static_cast<IoLoop*>(p)->prepare_client_transfer_capacity(incoming);
+                    });
+            if (ok) {
+                executors[tid].activate_fused(&ios[tid].ring());
+                ios[tid].bind_fused_executor(&executors[tid]);
+                executors[tid].bind_fused_completion(
+                    &ios[tid],
+                    [](void* p, Client* client) {
+                        static_cast<IoLoop*>(p)->fused_executor_completion<false>(client);
+                    });
+                if (srv.read_local_enabled())
+                    executors[tid].bind_read_local_demotion(
+                        &ios[tid],
+                        [](void* p, Client* client, const uint64_t* probed,
+                           const ReadLocalFallbackReason* fallbacks,
+                           uint32_t probed_count, uint32_t& demoted) {
+                            return static_cast<IoLoop*>(p)->fused_demote_local_read_batch(
+                                client, probed, fallbacks, probed_count, demoted);
+                        });
+                self.bind_fused_executor_hooks(
+                    &executors[tid],
+                    [](void* p) {
+                        return static_cast<FusedExLoop*>(p)->split_read_local_pass();
+                    },
+                    [](void*, SnapshotManager*) {
+                        // Snapshot broadcasts target EX only in split placement. The writer's
+                        // progress hook above still advances QSBR during blocking SAVE.
+                        std::abort();
+                    });
+            }
+            if (!boot.arrive_loaded(tid, ok, local_error)) return;
+            if (!boot.wait_until_ready(tid, self.stop_flag())) return;
+            if (self.role() == Role::Ifid) {
+                if (!self.shards().empty()) std::abort();
+                if (!ios[tid].activate()) {
+                    boot.give_up(tid, "read-local IO listener activation failed");
+                    return;
+                }
+            } else {
+                executors[tid].activate();
+            }
+            if (!boot.arrive_ready(tid)) return;
+            if (!boot.wait_until_running(tid, self.stop_flag())) return;
+            for (;;) {
+                if (self.stop_flag().load(std::memory_order_relaxed)) break;
+                const Role role = self.role();
+                if (role == Role::Ifid) {
+                    if (!ios[tid].activate()) std::abort();
+                    self.publish_ready_role(Role::Ifid);
+                    ios[tid].run_fused();
+                    self.publish_ready_role(Role::Idle);
+                    if (!self.stop_flag().load(std::memory_order_relaxed)) ios[tid].deactivate();
+                } else if (role == Role::Ex) {
+                    executors[tid].activate();
+                    self.publish_ready_role(Role::Ex);
+                    executors[tid].run();
+                    self.publish_ready_role(Role::Idle);
+                } else {
+                    std::this_thread::yield();
+                }
+            }
+            self.publish_read_local_parked(srv.read_local_epoch());
+        });
+
+    auto stop_workers = [&] {
+        for (uint32_t tid = 0; tid < nthreads; tid++)
+            srv.thread(tid).stop_flag().store(true, std::memory_order_relaxed);
+        boot.stop();
+        for (std::thread& worker : pool)
+            if (worker.joinable()) worker.join();
+    };
+    if (!boot.wait_loaded(srv.shutting_down())) {
+        stop_workers();
+        const std::string error = boot.error();
+        if (!error.empty()) std::fprintf(stderr, "persistence load failed: %s\n", error.c_str());
+        const bool interrupted = srv.shutting_down().load(std::memory_order_relaxed);
+        if (interrupted) report_graceful_shutdown();
+        return interrupted ? 0 : 1;
+    }
+    srv.set_loading(false);
+    if (srv.shutting_down().load(std::memory_order_relaxed)) {
+        stop_workers();
+        report_graceful_shutdown();
+        return 0;
+    }
+
+    auto probe_listener = [&](uint32_t port, bool tls) {
+        if (!port) return true;
+        const int probe = IoLoop::make_reuseport_listener(
+            cfg.bind_addr, port, cfg.tcp_backlog, tls);
+        if (probe < 0) return false;
+        ::close(probe);
+        return true;
+    };
+    const bool port_ok = probe_listener(cfg.port, false);
+    const bool tls_port_ok = port_ok && probe_listener(cfg.tls_port, true);
+    if (!port_ok || !tls_port_ok) {
+        std::perror(port_ok ? "bind tls-port" : "bind");
+        stop_workers();
+        return 1;
+    }
+    std::string unix_error;
+    if (!unix_listener.open(cfg.tcp_backlog, unix_error)) {
+        std::fprintf(stderr, "%s\n", unix_error.c_str());
+        stop_workers();
+        return 1;
+    }
+    if (unix_listener.fd() >= 0) {
+        if (!ios[unix_owner].attach_listener(unix_listener.fd())) {
+            std::fprintf(stderr, "unix listener attach failed on t%u\n", unix_owner);
+            stop_workers();
+            return 1;
+        }
+        (void)unix_listener.release_fd();
+    }
+    if (!boot.advance_ready(srv.shutting_down()) ||
+        !boot.wait_ready(srv.shutting_down()) ||
+        !boot.advance_running(srv.shutting_down())) {
+        stop_workers();
+        const std::string error = boot.error();
+        if (!error.empty()) std::fprintf(stderr, "split read-local boot failed: %s\n", error.c_str());
+        const bool interrupted = srv.shutting_down().load(std::memory_order_relaxed);
+        if (interrupted) report_graceful_shutdown();
+        return interrupted ? 0 : 1;
+    }
+
+    // Reached only after every ring, sink and listener is ready and no stop edge was taken.
+    if (cfg.port) std::printf("listening on %s:%u\n", cfg.bind_addr, cfg.port);
+    if (cfg.tls_port) std::printf("listening with TLS on %s:%u\n", cfg.bind_addr, cfg.tls_port);
+    if (unix_listener.bound()) std::printf("listening on unix:%s\n", cfg.unixsocket);
+    std::fflush(stdout);
+
+    if (srv.flipctl_enabled()) {
+        while (!srv.shutting_down().load(std::memory_order_relaxed)) {
+            (void)srv.flipctl_tick(now_ns() / 1000000ull);
+            if (srv.shutting_down().load(std::memory_order_relaxed)) break;
+            (void)signal_doorbell_wait(srv.flipctl_wait_ms());
+        }
+    }
+
+    for (std::thread& worker : pool) worker.join();
+    // The unix socket file is unlinked by its RAII owner in main, for every return path.
+    report_graceful_shutdown();
+    return 0;
+}
+
+}  // namespace tomo
diff --git a/src/core/server.h b/src/core/server.h
index d27b1574d..f5aac780a 100644
--- a/src/core/server.h
+++ b/src/core/server.h
@@ -134,7 +134,7 @@ struct FlipReport {
     bool moving = false;
 };
 
-// Allocated only for the boot-armed fused read-local lane. The Server keeps only one pointer at its
+// Allocated only for the boot-armed read-local lane. The Server keeps only one pointer at its
 // true tail, so baseline member offsets and cache-line sharing remain unchanged.
 struct ReadLocalServerState {
     std::atomic<uint64_t> epoch{1};
@@ -580,7 +580,7 @@ public:
     }
 
     static bool read_local_enabled(const Config& cfg) {
-        return cfg.thread_mode == ThreadMode::Fused && cfg.overlap == 0 && cfg.read_local != 0;
+        return cfg.overlap == 0 && cfg.read_local != 0;
     }
     bool read_local_enabled() const { return read_local_enabled(cfg_); }
     uint64_t read_local_epoch() const {
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

All physical threads provision an `IoLoop` and `FusedExLoop`. They use the
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
production build, asserted at `src/core/thread.h:306`. An atomic loop-entry
flag plus alignment occupies the extra word, after all existing members.
The diagnostics-only selector-3 sidecar already has different alignment
and is excluded from this production-size assert. Read-local 0 allocates
no lane, deferred queue, thread/server sidecar, or store read-local state.
Armed split allocates the existing fused-capable executor state on each
physical thread, including dormant future owners; measure that fixed
memory cost as well as throughput.

## Observable activation

`INFO server` still reports `thread_mode:2s`, live `io_threads` and
`ex_threads`, and effective `read_local:1`. It now also reports:

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
(`src/cmd/t_server.cc:1995`). After FLIP settles, every IFID row must have
shards=0/active=1, and every EX row active=0. As with other INFO geometry,
the individual samples are not one transactional view during a move.

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
   lane bit and active-reader count to match: 0, 6, 0, 8. Also smoke 2s/1
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
Reading additional input from stdin...
OpenAI Codex v0.153.4
--------
workdir: /home/user/Projects/cx-rl2s
model: gpt-6-astra
provider: openai
approval: never
sandbox: danger-full-access
reasoning effort: max
reasoning summaries: none
session id: 01a07b16-a34e-7832-9df6-d4b51bc3ca20
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

You are verifying and, if needed, correcting work already in this worktree: the extension of the local-read
lane to split (2s) thread mode. Keep the work; prove or fix the property below. Do not build, do not run the
server, benchmarks or the gate — the maintainer runs those.

THE MAINTAINER'S HARD REQUIREMENT: the new behaviour activates ONLY when BOTH `--thread-mode 2s` AND
`--read-local 1` are set. With `--thread-mode 2s --read-local 0`, the server must be EXACTLY what it is today.

Your activation predicate already tests `read_local`. That is necessary and not sufficient. The failure this
requirement guards against is not a wrong predicate, it is CONSTRUCTION that happens regardless of it: a
shard-less fused tier built and then left idle, fused machinery allocated per thread, a retire sink or QSBR
registration created for threads that will never read, a branch added to a loop that runs in both settings, or
a struct grown so that the off arm pays cache cost for a field only the on arm uses.

PROVE, with file:line evidence, for the `--thread-mode 2s --read-local 0` configuration:
1. No thread is constructed with capabilities it did not have before this change, and the thread role and
   count are what they were.
2. Nothing is allocated that was not allocated before — walk every `new`, container reserve, buffer, sink and
   registration your diff adds, and show it is behind the predicate.
3. No hot-path loop gains a branch. Where you added a test, show it is either outside the per-operation path or
   compiled out / hoisted for the off arm. Remember this tree's rule that an argument to a call that
   early-returns is still evaluated and still costs a cache line.
4. No struct on the per-operation path changed size. If one did, name it, give the old and new sizes and the
   layout assert, and say what the off arm now pays.

If any of the four fails, FIX IT so the off arm is untouched, and say what you changed. If all four hold, say
so and point at the evidence for each — the maintainer will boot both settings and gate them, and a claim that
does not survive that is worse than an admission now.

DELIVERABLE: update RL2S.md with a section titled "OFF-ARM PROOF" containing the four answers, plus any fix.
codex
I’ll compare the local-read changes with their baseline, trace what the 2s/off configuration constructs and executes, and fix any off-arm costs I find. I’ll document the evidence in `RL2S.md` without building or running anything.
exec
/bin/bash -lc "pwd && rg --files -g 'AGENTS.md' -g 'RL2S.md' -g 'CMakeLists.txt' -g 'Makefile' -g '*read*local*' -g '*rl2s*' -g '.git' && git status --short && git branch --show-current && git rev-parse --show-toplevel && git log -6 --oneline" in /home/user/Projects/cx-rl2s
 succeeded in 0ms:
/home/user/Projects/cx-rl2s
Makefile
src/core/rl2s.cc
src/core/read_local.h
src/store/read_local_settax.h
src/store/read_local_reclaim.h
.git
RL2S.md
tests/read_local_ring_unit.cc
tests/read_local_lane.py
tests/rl2s.py
tests/read_local_write_ring_unit.cc
 M Makefile
 A RL2S.md
 M src/cmd/t_server.cc
 M src/core/config.h
 M src/core/ex_loop.h
 M src/core/genthread.cc
 M src/core/genthread.h
 M src/core/io_loop.h
 A src/core/rl2s.cc
 M src/core/server.h
 M src/core/thread.h
 M src/main.cc
 M tests/bplus.py
 M tests/config_parser_test.cc
 M tests/read_local_lane.py
 A tests/rl2s.py
 M tests/rlcache_churn.py
?? CODEX-OUT.md
cx-rl2s
/home/user/Projects/cx-rl2s
78c3e5391 atomic_torn: re-roll the OFF torn-RENAME control instead of failing a run it did not manifest in
48b9c3a23 multirace: re-arm the hazard window instead of failing a run that never entered it
0187d303a Merge branch 't-flipdamp-r' into t-merge14: the io:ex controller decides from measured cost
d88c098a1 notes: rebase onto the fingerprint diet, the quantum collision, combined-tree acceptance
b40026236 Merge branch 't-ringdiet' into t-merge14: the RYOW ring arms on demand
f13c3f91c merge t-flipdamp (flip actuator redesign) onto the fingerprint diet


exec
/bin/bash -lc 'for p in /AGENTS.md /home/AGENTS.md /home/user/AGENTS.md /home/user/Projects/AGENTS.md /home/user/Projects/cx-rl2s/AGENTS.md; do if test -f "$p"; then printf '"'%s\\n' \""'$p"; cat "$p"; fi; done
cat .git
git diff HEAD --stat
git diff HEAD -- src/core/genthread.h src/core/genthread.cc src/core/thread.h src/main.cc' in /home/user/Projects/cx-rl2s
 succeeded in 0ms:
gitdir: /home/user/Projects/tomokv-cpp/.git/worktrees/cx-rl2s
 Makefile                    |   1 +
 RL2S.md                     | 374 ++++++++++++++++++++++++++++++++++++++++++++
 src/cmd/t_server.cc         |  31 +++-
 src/core/config.h           |   8 +-
 src/core/ex_loop.h          |  49 ++++--
 src/core/genthread.cc       |   4 +-
 src/core/genthread.h        |   7 +
 src/core/io_loop.h          |  16 +-
 src/core/rl2s.cc            | 308 ++++++++++++++++++++++++++++++++++++
 src/core/server.h           |   4 +-
 src/core/thread.h           |  23 ++-
 src/main.cc                 |  10 +-
 tests/bplus.py              |  16 +-
 tests/config_parser_test.cc |   5 +-
 tests/read_local_lane.py    |   7 +-
 tests/rl2s.py               | 185 ++++++++++++++++++++++
 tests/rlcache_churn.py      |   9 +-
 17 files changed, 1015 insertions(+), 42 deletions(-)
diff --git a/src/core/genthread.cc b/src/core/genthread.cc
index eb738f64c..c8cb6b4b6 100644
--- a/src/core/genthread.cc
+++ b/src/core/genthread.cc
@@ -1,5 +1,5 @@
-// Unified generalized-thread runtime. This translation unit is the boot-time architecture wall:
-// only it instantiates ExLoopT<true> and IoLoop's Fused=true loop methods for pipelines 0, 1, and 2.
+// Unified generalized-thread runtime. This translation unit instantiates IoLoop's Fused=true
+// loop methods for pipelines 0, 1, and 2; rl2s.cc also uses the overlap-0 loop for shard-less IO.
 #include "genthread.h"
 
 #include <algorithm>
diff --git a/src/core/genthread.h b/src/core/genthread.h
index 2ceddd0ad..c47b788f3 100644
--- a/src/core/genthread.h
+++ b/src/core/genthread.h
@@ -20,4 +20,11 @@ int run_fused_server(Server& server, const SnapshotLoadPlan* aof_base_plan,
                      LateUnixListener& unix_listener,
                      ShutdownReportFinalLine& final_report);
 
+// Split placement with shard-less fused-capable IO and the existing owner role loop.
+int run_split_read_local_server(Server& server, const SnapshotLoadPlan* aof_base_plan,
+                     const std::vector<std::unique_ptr<AofReplayPlan>>& aof_plans,
+                     const SnapshotLoadPlan* load_plan, TlsContext* tls_context,
+                     LateUnixListener& unix_listener,
+                     ShutdownReportFinalLine& final_report);
+
 }  // namespace tomo
diff --git a/src/core/thread.h b/src/core/thread.h
index 738fb43ec..adea9605d 100644
--- a/src/core/thread.h
+++ b/src/core/thread.h
@@ -292,13 +292,20 @@ struct ReadLocalStats {
     }
 };
 
-// Fused read-local publication and telemetry are absent from baseline ThreadCtx allocations. The
+// Read-local publication and telemetry are absent from baseline ThreadCtx allocations. The
 // lone owning pointer is placed in ThreadCtx's established tail padding below.
 struct ReadLocalThreadState {
     std::atomic<uint64_t> tick{0};
     ReadLocalRetireSink retire_sink{};
     ReadLocalStats stats{};
+    // Published only on actual reader-loop entry/exit, including FLIP. INFO must distinguish
+    // an enabled boot knob from a live parser/executor lane. No per-operation publication.
+    std::atomic<bool> lane_active{false};
 };
+#if TOMO_READ_LOCAL_SET_TAX_VARIANT != 3
+static_assert(sizeof(ReadLocalThreadState) == 368,
+              "lane activation adds one alignment word only to the optional sidecar");
+#endif
 
 class ThreadCtx {
 public:
@@ -919,7 +926,7 @@ public:
     }
     bool parked() const { return parked_.load(std::memory_order_acquire); }
 
-    // Fused read-local QSBR publication. A reader publishes once at the coarse rotation boundary,
+    // Read-local QSBR publication. A reader publishes once at the coarse rotation boundary,
     // never per operation. Parked shares this word with the tick so a grace scan cannot accept a
     // stale separate parked=true after the thread has resumed probing foreign stores. Sequential
     // consistency orders the park/resume edge with that scan; it is paid only at rotation/park.
@@ -953,6 +960,14 @@ public:
         if (!read_local_state_) std::abort();
         return read_local_state_->tick.load(std::memory_order_seq_cst);
     }
+    void set_read_local_lane_active(bool active) {
+        if (!read_local_state_ || !read_local_state_->retire_sink.defer) std::abort();
+        read_local_state_->lane_active.store(active, std::memory_order_release);
+    }
+    bool read_local_lane_active() const {
+        return read_local_state_ &&
+               read_local_state_->lane_active.load(std::memory_order_acquire);
+    }
     static bool read_local_publication_parked(uint64_t publication) {
         return (publication & kReadLocalParkedBit) != 0;
     }
@@ -968,8 +983,8 @@ public:
         if (!read_local_state_ || !read_local_state_->retire_sink.defer) std::abort();
         return read_local_state_->retire_sink;
     }
-    // The nullable form, for the shard-ownership edge in Server. A thread has no sink in split
-    // mode and on a fused boot with the lane disarmed; the edge must be able to ask without
+    // The nullable form, for the shard-ownership edge in Server. A thread has no sink with the
+    // lane disarmed; the edge must be able to ask without
     // knowing which, and must be able to tell "disarmed" from "armed but unbound" -- the latter is
     // a boot-order defect, and the caller aborts on it rather than moving a shard to an owner that
     // cannot retire for it.
diff --git a/src/main.cc b/src/main.cc
index c14f656ae..c8fdead11 100644
--- a/src/main.cc
+++ b/src/main.cc
@@ -180,10 +180,9 @@ int main(int argc, char** argv) {
     if (cfg.overlap == 2)
         std::fprintf(stderr,
                      "WARNING: --overlap 2 selects an experimental research schedule\n");
-    if (cfg.read_local &&
-        (cfg.thread_mode != ThreadMode::Fused || cfg.overlap != 0))
+    if (cfg.read_local && cfg.overlap != 0)
         std::fprintf(stderr,
-                     "NOTICE: --read-local 1 requires --thread-mode 1s --overlap 0 "
+                     "NOTICE: --read-local 1 requires --overlap 0 "
                      "in this version; using the ordinary owner-task path\n");
     // THE ENGINE IS LATCHED HERE, once, before anything that reads it exists. Every Ring in the
     // process must agree (a uring ring cannot receive an eventfd doorbell and vice versa), and no
@@ -315,6 +314,11 @@ int main(int argc, char** argv) {
         return run_fused_server(srv, aof_base_plan.get(), aof_plans, load_plan.get(),
                                 tls_context.get(), unix_listener, final_shutdown_line);
     }
+    if (srv.read_local_enabled()) {
+        srv.topo().dump(stdout);
+        return run_split_read_local_server(srv, aof_base_plan.get(), aof_plans, load_plan.get(),
+                                           tls_context.get(), unix_listener, final_shutdown_line);
+    }
 
     srv.topo().dump(stdout);
     std::printf("tomokv-cpp: %u threads (%zu io + %zu ex), %u shard(s),"

exec
/bin/bash -lc 'nl -ba RL2S.md' in /home/user/Projects/cx-rl2s
 succeeded in 0ms:
     1	# Split IO local reads
     2	
     3	## Preconditions checked before implementation
     4	
     5	These references are to baseline `78c3e5391` (before this diff).
     6	
     7	1. **A reader can own zero shards.** GET resolves its home through
     8	   `Server::shard(op.shard)`, not the reader's owned-shard vector
     9	   (`src/core/ex_loop.h:1643`). MGET's route arrays contain global shard IDs;
    10	   window open/close, generation validation, probes and final classification
    11	   all use that same server directory (`src/core/ex_loop.h:1028`, `:1057`,
    12	   `:1084`, `:1210`, `:1348`). `Server::shard` indexes the process-wide
    13	   `shards_` directory (`src/core/server.h:534`). The reader copies the reply
    14	   before completion (`src/core/ex_loop.h:1450`, `:1737`); it does not retire
    15	   store objects. Owner housekeeping iterates `self_->shards()` and tolerates
    16	   an empty vector. Reclamation initializes from Server/ThreadCtx without
    17	   examining that vector (`src/core/read_local.h:260`), and callbacks carry
    18	   their reclaim owner explicitly (`:300`, `:395`). No read-side indexed
    19	   lookup requires the reader to own a shard.
    20	
    21	2. **QSBR registration is independent of ownership.** Server initialization
    22	   allocates a read-local sidecar for EVERY physical thread before assigning
    23	   shards (`src/core/server.h:316`, `:323`, `:367`). The grace-floor scan
    24	   visits all of `threads_`, including zero-shard threads (`:606`). These
    25	   are the registration and scan sites the split IO readers must use.
    26	   Rotation publication (`src/core/ex_loop.h:448`) and the park/resume pair
    27	   (`src/core/io_loop.h:720`, `:745`; `src/core/thread.h:922`) bound the
    28	   foreign-pointer lifetime. Registration alone is insufficient: every
    29	   participant must advance or explicitly park. The existing split EX loop
    30	   has neither read-local retirement draining nor these publications.
    31	
    32	3. **A zero-shard thread can have a defined, stable sink.** The deferred
    33	   queue creates its sink in `src/core/read_local.h:260`. Fused activation
    34	   binds it to ThreadCtx BEFORE iterating owned shards
    35	   (`src/core/ex_loop.h:241` versus `:243`). Thus zero shards still gives a
    36	   valid per-thread sink. Loading precedes store arming
    37	   (`src/core/genthread.cc:131`, `:165`; `src/store/flatstore.h:741`). At a
    38	   later ownership move, `adopt_read_local_retire_sink` changes the store's
    39	   sink in the same quiesced critical section as ownership
    40	   (`src/core/server.h:1942`, `:2072`). Source retirement debt must already
    41	   be empty (`src/core/ex_loop.h:1794`). The split implementation must bind
    42	   every physical thread before serving, including dormant future owners,
    43	   and retain those queues across role changes.
    44	
    45	4. **This is an existing role with an added capability.** Split IO remains
    46	   `Ifid`, serves clients and owns no shards; EX remains the owner role.
    47	   `Server::serves_clients` and `owns_shards` already distinguish those
    48	   capabilities (`src/core/server.h:545`). Split placement retains global
    49	   `--ratio io:ex` counts (`:239`) and assigns all `--shards` among the EX
    50	   tids (`:367`; `src/core/placement.h:178`). Fewer shards than EX threads
    51	   can leave EX threads empty too. Fused placement remains unified, and its
    52	   rejection of `--ratio` remains (`src/core/config.h:1222`). INFO must keep
    53	   reporting `thread_mode:2s` and the live IO/EX counts, with actual lane
    54	   activation reported separately.
    55	
    56	All four prerequisites support the design. Merely removing the fused-mode
    57	predicate at `src/core/server.h:582` would be incorrect: split IO would still
    58	miss the lane, its notify-config refresh would dereference an unbound fused
    59	executor, and owners would lack the armed command guards and reclaim drain.
    60	
    61	Existing research-only overwrite builds are a separate law violation:
    62	`TOMO_READ_LOCAL_SET_TAX_VARIANT=1/3` explicitly selects in-place writes and
    63	sequence validation (`src/store/read_local_settax.h:1`,
    64	`src/store/flatstore.h:2713`). Production selector 0 returns NotPossible and
    65	uses immutable replacement. This task preserves the production path and
    66	does not extend the existing bounded GET/MGET revalidation under review.
    67	
    68	## Design as built
    69	
    70	`src/core/rl2s.cc` contains the new runtime and the shard-less executor pass.
    71	`main.cc` selects it only for split mode with the effective lane enabled.
    72	The ordinary `2s --read-local 0` runtime is unchanged. `1s` keeps its existing
    73	boot path, parser, lane scheduling, probes, revalidation limits and demotion
    74	planner. The new selection branch is at the executor-pass boundary, not on
    75	single-key GET admission. In particular, the existing pressure check still
    76	guards evaluation of `active_.size()`; no admission expression moved ahead
    77	of its old check.
    78	
    79	All physical threads provision an `IoLoop` and `FusedExLoop`. They use the
    80	ordinary split task inbox layout and producer masks, including FLIP's
    81	existing remasking. Initial persistence loading runs before store arming.
    82	Each executor's `activate_fused` binds its permanent ThreadCtx sink, then
    83	arms its owned stores; IO's empty ownership vector makes the second step
    84	empty. The stop-aware boot gate holds all threads until every sink, store
    85	and ring is ready. Only IO activates listeners. No read or armed mutation
    86	can run while a thread's sink is unbound.
    87	
    88	During IO tenure, `IoLoop::run_fused` selects the existing overlap-0 parser,
    89	GET/MGET lane, ROB completion and network reply machinery. The two executor
    90	pass entry points call `ExLoopT<true>::split_read_local_pass`
    91	(`src/core/rl2s.cc:46`). It refreshes reader configuration, drains the
    92	existing lane, maintains its existing pressure/cap state and publishes
    93	quiescence. It never dispatches owner tasks or walks the owned-shard vector.
    94	This matters during FLIP: the coordinator can install an IO thread's future
    95	EX vector while that IO tenure is still acknowledging the transfer.
    96	Blocking snapshot progress uses this same shard-less pass; snapshot owner
    97	broadcasts still target only EX. The parser continues to enqueue EVERY
    98	write on its current owner. The existing reservation/demotion plan orders
    99	pending overlapping local reads before the write, and the existing ROB
   100	write/owner fences preserve connection RYOW and reply order.
   101	
   102	EX tenure uses the existing `ExLoopT::run` owner schedule, with the same
   103	task execution, atomic deferral, expiry, notification, persistence and
   104	completion transport. Instantiating it as fused-capable retains the armed
   105	precise-write eviction guard and foreign-write scope that `ExLoopT<false>`
   106	would omit. Stores use the existing immutable replacement and deferred
   107	reclaim hooks. There is no executor-side network send. The owner loop
   108	drains retirement each pass and retains the existing empty-retirement
   109	condition for `ExDrain`; the debug build also runs the sink/owner assertion.
   110	
   111	An EX tenure performs no local-read captures, so it publishes **parked**
   112	for its entire tenure (`src/core/ex_loop.h:707`). Its registration and sink
   113	remain present. On entry to an IO reader loop, the thread clears parked
   114	before sampling/publishing the current epoch (`src/core/io_loop.h:541`).
   115	It then uses the original rotation and network-wait publications. On exit
   116	it clears the lane-active flag and parks before teardown. These are tenure
   117	boundaries, not per-operation read-side synchronization. Ownership-edge
   118	sink adoption is unchanged. All workers join before their queues are
   119	drained and store hooks are disarmed at shutdown.
   120	
   121	The shared fused drain charges execution to `LoopSignals::ops` as well as
   122	the parser's charge. The split IO pass cancels that second charge once per
   123	rotation: IO's existing dispatch-rate meaning, used by FLIP, survives local
   124	completion. Read-local hit counters and command accounting still count the
   125	completion. An IO-to-EX conversion invalidates the executor's cached config
   126	version so its new shards receive current settings once dispatch resumes.
   127	
   128	Configuration remains numeric and boot-only. Both modes accept read-local
   129	0 and 1 at overlap 0. Split read-local 1 with overlap 1 is now rejected
   130	explicitly, instead of accepting a lane that cannot run there. The existing
   131	1s behavior with overlap 1/2 (notice and effective lane off) is unchanged.
   132	`--ratio`, `--place`, `--shards`, key/client balancing and manual/automatic
   133	FLIP retain split semantics; no new role or tuning knob was introduced.
   134	
   135	The locked footprints and their asserts remain unchanged: Op 336, Client
   136	1984, ThreadCtx 1408, Shard 1440, FlatStore 944, Rob<64> 192, AtomicEntry 144,
   137	Config 624; the ordinary ExLoop assert remains 6112. The only data layout
   138	change is the optional `ReadLocalThreadState`: 360 -> 368 bytes in the
   139	production build, asserted at `src/core/thread.h:306`. An atomic loop-entry
   140	flag plus alignment occupies the extra word, after all existing members.
   141	The diagnostics-only selector-3 sidecar already has different alignment
   142	and is excluded from this production-size assert. Read-local 0 allocates
   143	no lane, deferred queue, thread/server sidecar, or store read-local state.
   144	Armed split allocates the existing fused-capable executor state on each
   145	physical thread, including dormant future owners; measure that fixed
   146	memory cost as well as throughput.
   147	
   148	## Observable activation
   149	
   150	`INFO server` still reports `thread_mode:2s`, live `io_threads` and
   151	`ex_threads`, and effective `read_local:1`. It now also reports:
   152	
   153	```text
   154	read_local_thread_0:role=ifid,shards=0,active=1,hits_total=123,mget_hits_total=7
   155	read_local_thread_6:role=ex,shards=8,active=0,hits_total=0,mget_hits_total=0
   156	read_local_active_threads:6
   157	```
   158	
   159	Those are illustrative rows for 6:2/16, not measured results. `active`
   160	means the thread actually entered the reader loop, including its idle
   161	network waits; it is not inferred from the knob or ownership. The atomic
   162	flag is written only at loop entry/exit. Shard counts come from published
   163	owner IDs, not concurrently modified ownership vectors
   164	(`src/cmd/t_server.cc:1995`). After FLIP settles, every IFID row must have
   165	shards=0/active=1, and every EX row active=0. As with other INFO geometry,
   166	the individual samples are not one transactional view during a move.
   167	
   168	Per-thread `*_total` values are lifetime completions and survive FLIP and
   169	CONFIG RESETSTAT. They can be nonzero on an EX row after that thread has
   170	previously served IO. Aggregate `INFO stats` keeps its existing RESETSTAT
   171	semantics. `read_local_hits` counts GET and MGET commands; GET completions
   172	are `hits - read_local_mget_local_hits`. `read_local_keyspace_hits/misses`
   173	count elements, so do not divide those by MGET command count and call the
   174	answer an activation fraction.
   175	
   176	## Validation delivered, not executed
   177	
   178	Only source inspection, `git diff --check` and Python AST parsing were
   179	performed. No C++ build, server, battery, benchmark or gate was run.
   180	The parser/admission body and GET/MGET window/probe/copy region were also
   181	compared directly with the baseline and are byte-for-byte unchanged.
   182	
   183	`tests/rl2s.py` is a standalone directed battery for the 6:2/16 geometry.
   184	It requires the enabled lane, checks INFO against the shard ownership
   185	oracle, requires every IO thread to complete local GET and MGET traffic,
   186	proves clean reads add zero shard-owner command executions, proves fresh
   187	pure SET connections do not arm/allocate RYOW state, checks exact pipelined
   188	RYOW replies, and repeats the read/write checks across both directions of
   189	a real FLIP. Connection placement is retried on fresh sockets with a fixed
   190	bound; missing a thread's lane is failure, never a skip. It requires key-LB,
   191	client-LB and automatic FLIP off for exact counter attribution. No race
   192	window is claimed by this test; use the existing held-window tests below.
   193	
   194	The lane-admission and cache-churn batteries now accept enabled 2s lanes
   195	and require actual activation. B+ selects a client-serving reader from the
   196	role rows, still chooses participating owners from shard rows, and accepts
   197	2s; its held-group and per-key generation assertions are unchanged. The
   198	config parser battery adds the explicit split/overlap rejection check.
   199	
   200	No gate row was added or retired. `tests/gate.sh` is unchanged:
   201	EXPECT_QUICK remains **327**, EXPECT_FULL **344** (without the optional NIC
   202	row). Its quick-tier exit is line 1258; the standalone RL2S battery has no
   203	row on either side of that exit. Updating existing battery internals does
   204	not change gate row counts.
   205	
   206	## MEASUREMENT PLAN
   207	
   208	The maintainer performs all steps below, on the quiet box. No performance
   209	claim is established by this diff. The supplied reference observations are
   210	the motivation: baseline 2s wins pure SET by about 28%; 1s+read-local wins
   211	reads by about 2.2x; the hypothesized 2s read hand-off costs roughly 600
   212	cycles and limits the measured shape to about 25M ops/s. Reproduce those
   213	at the chosen geometry before interpreting a change.
   214	
   215	### Correctness and activation before rates
   216	
   217	1. Build production selector 0 and the normal unit/check targets. Run the
   218	   unchanged full gate, expecting 344/344, at its actual geometry:
   219	   `GATE_CORES=0-7`, `--shards 16`, split `--ratio 6:2`.
   220	   Gate success alone does not validate RL2S: it does not add a split-armed
   221	   row automatically.
   222	2. Boot the four overlap-0 combinations: 2s/0, 2s/1, 1s/0, 1s/1. On eight
   223	   real cores use 6:2 for split; omit ratio for 1s. Require INFO's effective
   224	   lane bit and active-reader count to match: 0, 6, 0, 8. Also smoke 2s/1
   225	   with one shard and 6:2 (one EX has no shard), explicit placement, and
   226	   the existing TCP/TLS/unix and epoll/uring boot/shutdown cases. Confirm
   227	   split/1/overlap-1 fails with the documented message.
   228	3. On a dedicated 2s/1, 6:2, 16-shard boot with debug enabled and key-LB,
   229	   client-LB, flip-auto all 0, run `python3 tests/rl2s.py HOST PORT`.
   230	   Run `tests/read_local_lane.py` at the same geometry; a cap that never
   231	   produces admission deferrals must fail. Re-run the existing atomic
   232	   torn-read, transaction/RYOW, B+, notifications/tracking, eviction and
   233	   snapshot/AOF batteries against this split-armed boot. In particular,
   234	   blocking SAVE must allow other owners' retirement to finish while the
   235	   writer IO is occupied; include loaded-data boots and SIGTERM during load.
   236	4. Restore default key/client balancing. Run `tests/rlcache_churn.py` with
   237	   its documented arming setup and require actual shard moves plus local
   238	   completions. Use `TOMO_RL_CACHE_DEBUG` to detect a sink crossing; this
   239	   does not select in-place overwrite. Run manual FLIP and flip-under-load
   240	   batteries, then automatic FLIP across read-heavy/write-heavy phases.
   241	   Require actual geometry changes and positive completions on newly
   242	   activated IO, with correct values before/during/after moves. A run with
   243	   zero moves is not evidence for sink migration safety. Preserve the
   244	   existing bounded fresh-state rearming for held-window assertions.
   245	5. Negative controls, throwaway binaries only: route the armed split IO to
   246	   the ordinary parser (keep the executor binding) to make the activation
   247	   check fail; separately keep the loop active but force GET/MGET onto
   248	   owner tasks to make the exact hit/owner-op checks fail. For sink
   249	   migration, use the existing `TOMO_RL_CACHE_NO_EAGER_ADOPT` control with
   250	   debug owner assertions and proven shard moves. Do not benchmark these
   251	   binaries or commit them.
   252	
   253	### Arms and named cells
   254	
   255	Use identical optimized build flags, allocator, CPU affinity, shards,
   256	dataset, seed, connections and offered load within a comparison. Keep the
   257	client outside the eight server cores. Start with uniform keys, 1 million
   258	preloaded keys, 128-byte values, no TTL, default atomic/filter/prefetch
   259	settings and overlap 0. Time no preload work.
   260	
   261	| Arm | Revision | Mode / lane | Eight-core placement |
   262	| --- | --- | --- | --- |
   263	| A PRE | 78c3e5391 | 2s / 0 | 6:2, then 4:4 |
   264	| A POST | this diff | 2s / 0 | identical to A PRE |
   265	| B POST | this diff | 2s / 1 | identical to A PRE |
   266	| C PRE | 78c3e5391 | 1s / 1 | all eight threads, no ratio |
   267	| C POST | this diff | 1s / 1 | identical to C PRE |
   268	| D PRE/POST | both | 1s / 0 | all eight threads, no ratio |
   269	
   270	B vs A and B vs C are the deciding comparisons; A/C/D PRE vs POST protect
   271	the existing modes. `2s/1` on PRE is deliberately NOT a lane comparison:
   272	that binary never enters it. It is useful only as a negative activation
   273	control.
   274	
   275	| Cell names | Traffic | Depth / connections | Purpose |
   276	| --- | --- | --- | --- |
   277	| GET-P1, GET-P16, GET-P128 | 100% existing-key GET | 1, 16, 128 / 256 | hand-off hypothesis and latency |
   278	| GET-P128-C2048 | 100% GET | 128 / 2048 | admission pressure and fair share |
   279	| SET-P1, SET-P16, SET-P128 | 100% overwriting SET, fresh writer sockets | 1, 16, 128 / 256 | retain/explain the 2s write advantage |
   280	| MGET8-P1, MGET8-P16, MGET8-P128 | eight keys per MGET across physical shards | 1, 16, 128 / 256 | multi-shard local validation; report commands AND keys/sec |
   281	| MIX95, MIX50, MIX41 | 95/5, 50/50, 41/59 GET/SET | 16 and 128 / 256 | useful mixed load and write-ring pressure |
   282	| RYOW-P16, RYOW-P128 | SET k; GET k, plus independent GETs | 16, 128 / 256 | explicit conflict fallback vs unrelated reads |
   283	| ATOMIC8-P16 | MSET8 writers with GET/MGET8 readers, overlapping and disjoint keysets | 16 / 256 | atomic-filter selectivity and torn-read protection |
   284	| GET-S16, GET-S1024, SET-S16, SET-S1024 | GET or overwriting SET | 1 and 128 / 256 | reply/value size and send-path costs |
   285	| YCSB-A, YCSB-B, YCSB-C | existing 0.17.0 traces, same seed/skew in all arms | 1 and 128 / fixed matched clients | common workloads, separate from uniform microbenchmarks |
   286	
   287	Repeat the decisive GET/SET/MIX cells on the 25GbE two-netns rig. Loopback
   288	cannot decide send-path behavior. Treat its roughly 12% rig tax as a
   289	separate instrument effect, not a correction to apply to a mixed table.
   290	Then repeat decisive cells on 16/32/64 real cores at identical splits per
   291	A/B pair, using 128 shards for these scale cells and recording actual L3
   292	domains. Include an IO:EX ratio sweep at eight cores (2:6, 4:4, 6:2, 7:1)
   293	for A/B; report every ratio, not just B's best ratio against A's worst.
   294	
   295	First measure steady throughput ceilings. Then offer the SAME absolute
   296	load to A/B/C at 50%, 80% and 95% of the smallest stable ceiling, and sweep
   297	through the overload knee. Use completed replies as the denominator. At
   298	each matched load collect completed ops/s, p50/p99/p99.9, errors/timeouts,
   299	and CPU utilization by IO/EX and L3. A higher saturation ceiling alone
   300	does not excuse worse latency at a matched load.
   301	
   302	### Counters, profiles and interpretation
   303	
   304	Read INFO before/after each timed cell (avoid continuous INFO polling in
   305	the measured interval). Archive boot geometry and configuration, binary
   306	digest, per-thread rows and client-completed command counts. Require:
   307	
   308	- `read_local:1` AND all expected IO `active=1,shards=0` rows for B;
   309	  positive per-thread `hits_total`/`mget_hits_total` deltas on every IO
   310	  carrying that traffic. No MGET traffic means no MGET hit requirement.
   311	- For clean existing-key GET/MGET, near-total successful local completion;
   312	  on the directed uncontended test it is exact. For the benchmark require
   313	  at least 95% local command completions before calling the result a lane
   314	  measurement. Record every `read_local_fallback_*` and
   315	  `read_local_mget_fallback_*` reason, not just the aggregate. Missing GET
   316	  keys, TTL expiry, typed values, tracking and key-miss notifications are
   317	  separate fallback cells, not explanations for a clean workload.
   318	- `read_local_defer_lane_full` and `read_local_defer_quota` for deep
   319	  pipelines; `read_local_fallback_lane_full` must remain zero. Record
   320	  `read_local_mget_generation_retries` without changing its existing limit.
   321	- `read_local_arms`, `read_local_write_ring_sidecars`, and
   322	  `read_local_write_ring_records`: all zero deltas for fresh pure-writer
   323	  sockets; after warmup, transient arming fallback must not explain a
   324	  sustained flat read result. Conflict workload fallback is expected, and
   325	  must not be mistaken for clean-lane throughput.
   326	- DEBUG LBSIGNALS shard operation counts, `lb_ex_queue_delay_samples`,
   327	  `lb_ex_queue_delay_ewma_us`, wake counts and owner CPU demand: clean reads
   328	  should stop producing owner execution/queue traffic. Do not infer
   329	  reclamation progress from a hit counter. Track allocator/RSS and
   330	  `mem_block_cache` through sustained overwrites and idle recovery, and
   331	  require churn/shutdown checks to finish without unbounded retention.
   332	- `tomokv_keylb_bucket_moves`, `flip_completed`, and post-move role rows in
   333	  migration cells. Start stationary A/B/C attribution with balancing and
   334	  automatic FLIP off, then repeat with default key/client balancing and a
   335	  separately labeled automatic-FLIP phase.
   336	
   337	Collect cycles and instructions for ALL server threads and separately
   338	for IO/EX, plus IPC, cache/L3 misses, branch misses, and available Bergamo
   339	load/store-stall and coherence events. Use the box's validated event
   340	encodings. Profile local copy/validation, task publication/consumption,
   341	owner stores, reclamation and network send at matched offered loads.
   342	Compute cycles/op = instructions/op / IPC using the same completed-op
   343	denominator and scope. The predicted ~600-cycle hand-off saving may be
   344	partly replaced by copy/validation, cache traffic or retirement costs;
   345	instructions alone cannot decide which arm won. Profile runs are separate
   346	from clean timing runs, with identical workload settings.
   347	
   348	Run at least five interleaved repetitions, randomizing or alternating arm
   349	order, with identical-arm controls at the start/end. Report medians and
   350	spread. The supplied quiet-box baseline is +/-0.15%; more than +/-2% on
   351	identical arms invalidates the comparison and calls for contention/bug
   352	investigation. No build or second benchmark may overlap a timed cell.
   353	
   354	Fill this table for each deciding cell; blanks are intentionally unmeasured:
   355	
   356	| Cell / load | A PRE 2s/0 | A POST 2s/0 | B POST 2s/1 | C PRE 1s/1 | C POST 1s/1 | B/A | B/C | Local fraction | p99 / cycles/op / instr/op / IPC |
   357	| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
   358	| GET-P1 | pending | pending | pending | pending | pending | pending | pending | pending | pending |
   359	| GET-P128 | pending | pending | pending | pending | pending | pending | pending | pending | pending |
   360	| SET-P128 | pending | pending | pending | pending | pending | pending | pending | n/a | pending |
   361	| MIX95 / MIX50 / MIX41 | pending | pending | pending | pending | pending | pending | pending | pending | pending |
   362	| MGET8-P128 | pending | pending | pending | pending | pending | pending | pending | pending | pending |
   363	
   364	A boot advertising the knob while active counts or completion deltas stay
   365	zero is an **implementation failure**, not a null performance result.
   366	If the lane demonstrably fires, owner hand-offs disappear, but B remains
   367	at A's read ceiling and matched-load latency across the geometry sweep,
   368	the proposed hand-off bottleneck explanation does not hold there. If B
   369	loses A's SET advantage and C equals or beats it on reads/mixed cells,
   370	there is no demonstrated useful operating region for this design. Any
   371	reproducible regression in A/C/D PRE vs POST, correctness failure, reclaim
   372	hang, or ownership violation blocks acceptance. A read gain with a SET
   373	loss must be reported as that tradeoff; do not claim a general win or
   374	retain the capability on an unmeasured assumption.

exec
/bin/bash -lc "rg -n 'read_local|make_unique|\\bnew\\b|reserve\\(|resize\\(|register|IoLoop|ExLoop|ThreadCtx|run\\(' src/main.cc src/core/server.h src/core/thread.h src/core/genthread.cc src/core/ex_loop.h | head -240" in /home/user/Projects/cx-rl2s
exec
/bin/bash -lc 'git diff HEAD -- src/core/io_loop.h src/core/server.h
nl -ba src/core/rl2s.cc' in /home/user/Projects/cx-rl2s
 succeeded in 0ms:
src/core/genthread.cc:1:// Unified generalized-thread runtime. This translation unit instantiates IoLoop's Fused=true
src/core/genthread.cc:39:void IoLoop::run_fused() {
src/core/genthread.cc:89:    std::vector<IoLoop> ios(nthreads);
src/core/genthread.cc:90:    std::vector<FusedExLoop> executors(nthreads);
src/core/genthread.cc:101:        if (srv.read_local_enabled()) {
src/core/genthread.cc:105:            for (FusedExLoop& executor : executors) executor.read_local_shutdown_drain();
src/core/genthread.cc:107:                srv.shard(static_cast<int32_t>(sid)).store().configure_read_local(false, {});
src/core/genthread.cc:109:        // All owners and readers are quiescent. Release pending-entry references before IoLoop
src/core/genthread.cc:115:        for (IoLoop& io : ios) io.reap_atomic_deferred();
src/core/genthread.cc:125:            ThreadCtx& self = srv.thread(tid);
src/core/genthread.cc:149:                    [](void* p) { return static_cast<IoLoop*>(p)->prepare_activation(); },
src/core/genthread.cc:150:                    [](void* p) { static_cast<IoLoop*>(p)->cancel_prepared_activation(); });
src/core/genthread.cc:154:                        return static_cast<IoLoop*>(p)->prepare_client_registration(client);
src/core/genthread.cc:157:                        static_cast<IoLoop*>(p)->cancel_client_registration(client);
src/core/genthread.cc:162:                        return static_cast<IoLoop*>(p)->prepare_client_transfer_capacity(incoming);
src/core/genthread.cc:171:                            static_cast<IoLoop*>(p)->fused_executor_completion<false>(client);
src/core/genthread.cc:177:                            static_cast<IoLoop*>(p)->fused_executor_completion<true>(client);
src/core/genthread.cc:179:                if (srv.read_local_enabled())
src/core/genthread.cc:180:                    executors[tid].bind_read_local_demotion(
src/core/genthread.cc:185:                            return static_cast<IoLoop*>(p)->fused_demote_local_read_batch(
src/core/genthread.cc:192:                            return static_cast<FusedExLoop*>(p)->fused_baseline_pass();
src/core/genthread.cc:195:                            static_cast<FusedExLoop*>(p)->fused_snapshot_start(manager);
src/core/genthread.cc:201:                            return static_cast<FusedExLoop*>(p)->fused_coarse_pass();
src/core/genthread.cc:204:                            static_cast<FusedExLoop*>(p)->fused_snapshot_start(manager);
src/core/genthread.cc:213:                            return static_cast<FusedExLoop*>(p)->fused_coarse_pass();
src/core/genthread.cc:216:                            static_cast<FusedExLoop*>(p)->fused_snapshot_start(manager);
src/core/genthread.cc:232:            if (srv.read_local_enabled())
src/core/genthread.cc:233:                self.publish_read_local_parked(srv.read_local_epoch());
src/core/genthread.cc:261:        const int probe = IoLoop::make_reuseport_listener(
src/core/thread.h:28:// changes by one direct old-to-new store, never an Idle transit which could repeat the fork's P0.
src/core/thread.h:37:#include <new>
src/core/thread.h:295:// Read-local publication and telemetry are absent from baseline ThreadCtx allocations. The
src/core/thread.h:296:// lone owning pointer is placed in ThreadCtx's established tail padding below.
src/core/thread.h:310:class ThreadCtx {
src/core/thread.h:319:    ThreadCtx() = default;
src/core/thread.h:320:    ThreadCtx(const ThreadCtx&) = delete;
src/core/thread.h:321:    ThreadCtx& operator=(const ThreadCtx&) = delete;
src/core/thread.h:331:        client_in_ = std::make_unique<ClientChan[]>(nthreads);
src/core/thread.h:332:        release_in_ = std::make_unique<ReleaseChan[]>(nthreads);
src/core/thread.h:333:        transfer_in_ = std::make_unique<TransferChan[]>(nthreads);
src/core/thread.h:340:        std::unique_ptr<TaskInbox> inbox(new (std::nothrow) TaskInbox);
src/core/thread.h:346:        std::unique_ptr<TaskInbox> inbox(new (std::nothrow) TaskInbox);
src/core/thread.h:351:    bool init_read_local_state() {
src/core/thread.h:352:        read_local_state_.reset(new (std::nothrow) ReadLocalThreadState);
src/core/thread.h:353:        return read_local_state_ != nullptr;
src/core/thread.h:365:        command_counts_ = count ? std::make_unique<uint64_t[]>(count) : nullptr;
src/core/thread.h:376:    // mutation of IoLoop and avoiding an include cycle between the two loop classes.
src/core/thread.h:446:    ReadLocalStats& read_local_stats() {
src/core/thread.h:447:        if (!read_local_state_) std::abort();
src/core/thread.h:448:        return read_local_state_->stats;
src/core/thread.h:454:    ReadLocalArmStats* read_local_arm_stats_or_null() {
src/core/thread.h:455:        return read_local_state_ ? &read_local_state_->stats.arm : nullptr;
src/core/thread.h:457:    const ReadLocalStats& read_local_stats() const {
src/core/thread.h:458:        if (!read_local_state_) std::abort();
src/core/thread.h:459:        return read_local_state_->stats;
src/core/thread.h:537:        return task_in_->reserve(from, count);
src/core/thread.h:822:    // its already-provisioned opposite ring before that new loop acknowledges ready.
src/core/thread.h:848:    // IO-only INFO surface. Published for each IO tenure when IoLoop binds its send engine; INFO then
src/core/thread.h:933:    void publish_read_local_tick(uint64_t tick) {
src/core/thread.h:935:        if (!read_local_state_) std::abort();
src/core/thread.h:936:        read_local_state_->tick.store(tick, std::memory_order_seq_cst);
src/core/thread.h:938:    void publish_read_local_parked(uint64_t tick) {
src/core/thread.h:940:        if (!read_local_state_) std::abort();
src/core/thread.h:941:        read_local_state_->tick.store(tick | kReadLocalParkedBit, std::memory_order_seq_cst);
src/core/thread.h:943:    void resume_read_local_tick() {
src/core/thread.h:944:        if (!read_local_state_) std::abort();
src/core/thread.h:945:        const uint64_t publication = read_local_state_->tick.load(std::memory_order_seq_cst);
src/core/thread.h:949:        read_local_state_->tick.store(
src/core/thread.h:952:    void refresh_read_local_quiescence(uint64_t tick) {
src/core/thread.h:954:        if (!read_local_state_) std::abort();
src/core/thread.h:955:        const uint64_t publication = read_local_state_->tick.load(std::memory_order_seq_cst);
src/core/thread.h:956:        read_local_state_->tick.store(
src/core/thread.h:959:    uint64_t read_local_publication() const {
src/core/thread.h:960:        if (!read_local_state_) std::abort();
src/core/thread.h:961:        return read_local_state_->tick.load(std::memory_order_seq_cst);
src/core/thread.h:963:    void set_read_local_lane_active(bool active) {
src/core/thread.h:964:        if (!read_local_state_ || !read_local_state_->retire_sink.defer) std::abort();
src/core/thread.h:965:        read_local_state_->lane_active.store(active, std::memory_order_release);
src/core/thread.h:967:    bool read_local_lane_active() const {
src/core/thread.h:968:        return read_local_state_ &&
src/core/thread.h:969:               read_local_state_->lane_active.load(std::memory_order_acquire);
src/core/thread.h:971:    static bool read_local_publication_parked(uint64_t publication) {
src/core/thread.h:974:    static uint64_t read_local_publication_tick(uint64_t publication) {
src/core/thread.h:978:    void bind_read_local_retire_sink(ReadLocalRetireSink sink) {
src/core/thread.h:979:        if (!read_local_state_ || !sink.defer) std::abort();
src/core/thread.h:980:        read_local_state_->retire_sink = sink;
src/core/thread.h:982:    ReadLocalRetireSink read_local_retire_sink() const {
src/core/thread.h:983:        if (!read_local_state_ || !read_local_state_->retire_sink.defer) std::abort();
src/core/thread.h:984:        return read_local_state_->retire_sink;
src/core/thread.h:991:    const ReadLocalRetireSink* read_local_retire_sink_or_null() const {
src/core/thread.h:992:        if (!read_local_state_ || !read_local_state_->retire_sink.defer) return nullptr;
src/core/thread.h:993:        return &read_local_state_->retire_sink;
src/core/thread.h:1000:    size_t read_local_block_cache_bytes() const {
src/core/thread.h:1001:        if (!read_local_state_) return 0;
src/core/thread.h:1002:        const KvBlockCache* cache = read_local_state_->retire_sink.block_cache;
src/core/thread.h:1077:            slots_.reserve(wanted);
src/core/thread.h:1078:            free_slots_.reserve(ReadyMask::kSlots);
src/core/thread.h:1185:    // moves through this queue and is retired by the destination IoLoop.
src/core/thread.h:1194:    // Cold publication only: keep every pre-existing ThreadCtx member at its current offset.
src/core/thread.h:1196:    // Fused-only callbacks live at the true tail so split-mode ThreadCtx offsets stay unchanged.
src/core/thread.h:1203:    std::unique_ptr<ReadLocalThreadState> read_local_state_;
src/core/thread.h:1206:static_assert(sizeof(ThreadCtx) == 1408,
src/core/thread.h:1207:              "read-local state must stay out of the baseline ThreadCtx allocation");
src/core/ex_loop.h:25:#include "read_local.h"
src/core/ex_loop.h:153:        void rebind_owned_shards(ThreadCtx& owner) {
src/core/ex_loop.h:157:                shard->store().rebind_read_local_retire_sink(*sink);
src/core/ex_loop.h:176:class ExLoopT {
src/core/ex_loop.h:181:    bool init(Server* srv, ThreadCtx* self, bool dormant = false) {
src/core/ex_loop.h:197:            if (srv->read_local_enabled()) {
src/core/ex_loop.h:198:                std::unique_ptr<ReadLocalExImpl> impl(new (std::nothrow) ReadLocalExImpl);
src/core/ex_loop.h:200:                impl->lane.reset(new (std::nothrow) ReadLocalExImpl::LaneEntry[kInboxSlots]);
src/core/ex_loop.h:202:                    new (std::nothrow) ReadLocalFallbackReason[kInboxSlots]);
src/core/ex_loop.h:208:                impl->interleave_owner_tasks = srv->cfg().read_local_interleave != 0;
src/core/ex_loop.h:209:                impl->prefetch_capture = srv->cfg().read_local_prefetch_capture != 0;
src/core/ex_loop.h:210:                read_local_.impl = std::move(impl);
src/core/ex_loop.h:230:    // Fused tenure shares the physical thread with IoLoop. IoLoop remains the published wake
src/core/ex_loop.h:241:        if (read_local_enabled())
src/core/ex_loop.h:242:            self_->bind_read_local_retire_sink(*read_local_impl().deferred.sink());
src/core/ex_loop.h:245:            if (read_local_enabled())
src/core/ex_loop.h:246:                shard->store().configure_read_local(
src/core/ex_loop.h:247:                    true, *read_local_impl().deferred.sink(),
src/core/ex_loop.h:248:                    srv_->cfg().read_local_atomic_filter != 0);
src/core/ex_loop.h:258:    void bind_read_local_demotion(void* context, ReadLocalExImpl::DemoteFn demote) {
src/core/ex_loop.h:260:        if (!read_local_enabled() || !context || !demote) std::abort();
src/core/ex_loop.h:261:        read_local_impl().demote_context = context;
src/core/ex_loop.h:262:        read_local_impl().demote = demote;
src/core/ex_loop.h:272:        auto& state = *read_local_.impl;
src/core/ex_loop.h:282:        if (!read_local_enabled()) return false;
src/core/ex_loop.h:283:        const auto& state = read_local_impl();
src/core/ex_loop.h:300:    uint32_t read_local_lane_quota(size_t live_connections) const {
src/core/ex_loop.h:305:        if (!read_local_enabled()) return UINT32_MAX;
src/core/ex_loop.h:306:        const auto& state = *read_local_.impl;
src/core/ex_loop.h:318:    // The parser MUST ask this before it evaluates read_local_lane_quota's argument. C++ evaluates
src/core/ex_loop.h:319:    // arguments eagerly, so `read_local_lane_quota(active_.size())` loaded active_ on EVERY parse
src/core/ex_loop.h:321:    // and IoLoop::active_ sits at offset 7728, on a line (7680-7743, shared only with unix_rr_,
src/core/ex_loop.h:323:    // IoLoop fields it does read live on two other lines entirely (notify_armed_ 7864,
src/core/ex_loop.h:329:    bool read_local_lane_under_pressure() const {
src/core/ex_loop.h:331:        if (!read_local_enabled()) return false;
src/core/ex_loop.h:332:        return read_local_.impl->lane_pressure != 0;
src/core/ex_loop.h:338:        if (!read_local_enabled()) return;
src/core/ex_loop.h:339:        read_local_.impl->lane_pressure = kReadLocalLanePressureRotations;
src/core/ex_loop.h:344:        if (!read_local_enabled()) std::abort();
src/core/ex_loop.h:346:        read_local_impl().lane_has_tombstones = true;
src/core/ex_loop.h:352:        if (!read_local_enabled() || !client ||
src/core/ex_loop.h:354:        auto& lane = read_local_impl();
src/core/ex_loop.h:360:            if (!client->rob().pending_read_local(op_id)) std::abort();
src/core/ex_loop.h:368:    void set_read_local_keymiss_notify(bool armed) {
src/core/ex_loop.h:370:        if (!read_local_enabled()) std::abort();
src/core/ex_loop.h:371:        read_local_impl().keymiss_notify_armed = armed;
src/core/ex_loop.h:374:    bool read_local_keymiss_notify_armed() const {
src/core/ex_loop.h:376:        return read_local_enabled() && read_local_impl().keymiss_notify_armed;
src/core/ex_loop.h:379:    void read_local_shutdown_drain() {
src/core/ex_loop.h:381:        if (read_local_enabled()) (void)read_local_impl().deferred.drain_shutdown();
src/core/ex_loop.h:397:        if (srv_->thread_mode() == ThreadMode::Split) return split_read_local_pass();
src/core/ex_loop.h:398:        if (read_local_interleave_enabled())
src/core/ex_loop.h:451:            ThreadCtx* self;
src/core/ex_loop.h:454:                if (enabled) self->publish_read_local_tick(server->read_local_epoch());
src/core/ex_loop.h:456:        } rotation_boundary{read_local_enabled(), self_, srv_};
src/core/ex_loop.h:461:        // (Server::adopt_read_local_retire_sink) this is a no-op standing check, kept because a
src/core/ex_loop.h:464:            read_local_rebind_owned_shards_after_lb();
src/core/ex_loop.h:467:            if (read_local_enabled())
src/core/ex_loop.h:468:                srv_->debug_assert_read_local_sinks_follow_ownership(self_->id());
src/core/ex_loop.h:494:            if (!read_local_interleave_enabled()) std::abort();
src/core/ex_loop.h:500:                fairlane_turn = read_local_impl().lane_count != 0;
src/core/ex_loop.h:552:                                    did += drain_tasks_read_local_interleaved<
src/core/ex_loop.h:580:        if (read_local_enabled()) {
src/core/ex_loop.h:581:            did += read_local_impl().deferred.drain_ready();
src/core/ex_loop.h:583:            if (__builtin_expect(read_local_impl().lane_pressure != 0, false))
src/core/ex_loop.h:584:                read_local_impl().lane_pressure--;
src/core/ex_loop.h:590:            const uint32_t cap = srv_->debug_read_local_lane_cap();
src/core/ex_loop.h:594:            if (__builtin_expect(read_local_impl().lane_admit_cap != want, false))
src/core/ex_loop.h:595:                read_local_impl().lane_admit_cap = want;
src/core/ex_loop.h:640:        if (srv_->thread_mode() == ThreadMode::Split) return split_read_local_pass();
src/core/ex_loop.h:641:        if (read_local_interleave_enabled())
src/core/ex_loop.h:664:            ThreadCtx* self;
src/core/ex_loop.h:667:                if (enabled) self->publish_read_local_tick(server->read_local_epoch());
src/core/ex_loop.h:669:        } rotation_boundary{read_local_enabled(), self_, srv_};
src/core/ex_loop.h:671:        if (lb_rebind_pending_) read_local_rebind_owned_shards_after_lb();
src/core/ex_loop.h:674:            if (!read_local_interleave_enabled()) std::abort();
src/core/ex_loop.h:680:                if (read_local_impl().lane_count) {
src/core/ex_loop.h:691:        if (read_local_enabled()) did += read_local_impl().deferred.drain_ready();
src/core/ex_loop.h:705:    uint32_t split_read_local_pass();
src/core/ex_loop.h:707:    void run() {
src/core/ex_loop.h:711:            if (!read_local_enabled() || read_local_impl().lane_count != 0) std::abort();
src/core/ex_loop.h:712:            self_->publish_read_local_parked(srv_->read_local_epoch());
src/core/ex_loop.h:724:                srv_->debug_assert_read_local_sinks_follow_ownership(self_->id());
src/core/ex_loop.h:801:                    did += read_local_impl().deferred.drain_ready();
src/core/ex_loop.h:857:    friend class IoLoop;
src/core/ex_loop.h:859:    bool read_local_enabled() const {
src/core/ex_loop.h:860:        if constexpr (Fused) return read_local_.impl != nullptr;
src/core/ex_loop.h:864:    bool read_local_interleave_enabled() const {
src/core/ex_loop.h:866:            return read_local_.impl && read_local_.impl->interleave_owner_tasks;
src/core/ex_loop.h:879:    ReadLocalExImpl& read_local_impl() {
src/core/ex_loop.h:881:        if (!read_local_.impl) std::abort();
src/core/ex_loop.h:882:        return *read_local_.impl;
src/core/ex_loop.h:885:    const ReadLocalExImpl& read_local_impl() const {
src/core/ex_loop.h:887:        if (!read_local_.impl) std::abort();
src/core/ex_loop.h:888:        return *read_local_.impl;
src/core/ex_loop.h:891:    bool read_local_point_writes_precise() const {
src/core/ex_loop.h:892:        return read_local_impl().point_writes_precise;
src/core/ex_loop.h:895:    void set_read_local_point_writes_precise(bool precise) {
src/core/ex_loop.h:896:        read_local_impl().point_writes_precise = precise;
src/core/ex_loop.h:899:    void read_local_rebind_owned_shards_after_lb() {
src/core/ex_loop.h:901:            if (read_local_.impl) read_local_.impl->rebind_owned_shards(*self_);
src/core/ex_loop.h:926:    void read_local_clear_reply(Op& op) {
src/core/ex_loop.h:935:        auto& state = read_local_impl();
src/core/ex_loop.h:945:            if (!task.client || !task.client->rob().pending_read_local(task.op_id)) continue;
src/core/ex_loop.h:960:        auto& lane = read_local_impl();
src/core/ex_loop.h:972:    static bool read_local_mget(const Op& op) {
src/core/ex_loop.h:973:        return op.spec && command_is_read_local_mget(*op.spec);
src/core/ex_loop.h:976:    static bool read_local_reply_string(Op& op, const KvObj* object, uint8_t stable_flags) {
src/core/ex_loop.h:986:                // are atomic, and both old/new lengths fit this allocation class. Combining odd
src/core/ex_loop.h:988:                const uint32_t length = kvobj_read_local_raw_length(object);
src/core/ex_loop.h:990:                char* frame = sink.reserve(24 + static_cast<size_t>(length) + 2);
src/core/ex_loop.h:996:                kvobj_read_local_copy_raw(object, stable_flags, length, payload);
src/core/ex_loop.h:1012:        reply_bulk(op.sink(), object->read_local_str_value(stable_flags));
src/core/ex_loop.h:1016:    uint32_t read_local_task_demotion_demand(const Op& op) const {
src/core/ex_loop.h:1017:        if (!op.read_local()) std::abort();
src/core/ex_loop.h:1018:        return read_local_mget(op)
src/core/ex_loop.h:1023:        auto& lane = read_local_impl();
src/core/ex_loop.h:1024:        const uint32_t demand = read_local_task_demotion_demand(op);
src/core/ex_loop.h:1052:        window.use_epochs = cached_routes && srv_->cfg().read_local_atomic_filter != 0;
src/core/ex_loop.h:1057:                                       .read_local_state_acquire();
src/core/ex_loop.h:1058:            if (FlatStore::read_local_generation_poisoned(state))
src/core/ex_loop.h:1063:            if (!window.use_epochs && FlatStore::read_local_table_mutating(state))
src/core/ex_loop.h:1086:                                           .read_local_state_acquire();
src/core/ex_loop.h:1087:                if (!FlatStore::read_local_state_eligible(after))
src/core/ex_loop.h:1091:                if (!FlatStore::read_local_generation_equal(window.states[sid], after))
src/core/ex_loop.h:1115:            const uint64_t state = store.read_local_state_acquire();
src/core/ex_loop.h:1217:            read_local_clear_reply(op);
src/core/ex_loop.h:1232:                store.read_local_prefetch(hash);
src/core/ex_loop.h:1233:                const FlatStore::ReadLocalProbe probe = store.read_local_probe(hash, name);
src/core/ex_loop.h:1235:                    read_local_clear_reply(op);
src/core/ex_loop.h:1249:                    if (!window.use_epochs && !store.read_local_validate(probe.state)) {
src/core/ex_loop.h:1258:                const uint8_t flags = object->read_local_flags();
src/core/ex_loop.h:1260:                    read_local_clear_reply(op);
src/core/ex_loop.h:1264:                    const int64_t deadline = object->read_local_expire_at_ms(flags);
src/core/ex_loop.h:1269:                        read_local_clear_reply(op);
src/core/ex_loop.h:1278:                        text, object->read_local_int_value(flags));
src/core/ex_loop.h:1281:                    if (!read_local_reply_string(op, object, flags)) {

 succeeded in 0ms:
diff --git a/src/core/io_loop.h b/src/core/io_loop.h
index f04718a40..b612cbce8 100644
--- a/src/core/io_loop.h
+++ b/src/core/io_loop.h
@@ -538,6 +538,16 @@ private:
             return;
         }
         constexpr bool IoPipe = !Fused && Pipeline == 1;
+        if constexpr (Fused) {
+            if (srv_->read_local_enabled()) {
+                // A split EX tenure is permanently parked: it never probes the local lane.
+                // Resume BEFORE sampling the epoch, as at the existing network-wait boundary.
+                if (ThreadCtx::read_local_publication_parked(self_->read_local_publication()))
+                    self_->resume_read_local_tick();
+                self_->publish_read_local_tick(srv_->read_local_epoch());
+                self_->set_read_local_lane_active(true);
+            }
+        }
         if constexpr (!kEp) {
             if (listen_fd_ >= 0) arm_accept(UrKind::Accept);
             if constexpr (HasTls) arm_accept(UrKind::TlsAccept);
@@ -754,10 +764,12 @@ private:
             self_->clear_blocked();
         }
         if constexpr (Fused) {
-            // The read loop is over permanently. Teardown below may take longer than another
+            // The read loop is over for this tenure. Teardown may take longer than another
             // owner's bounded retire queue can tolerate, but it performs no foreign store probe.
-            if (srv_->read_local_enabled())
+            if (srv_->read_local_enabled()) {
+                self_->set_read_local_lane_active(false);
                 self_->publish_read_local_parked(srv_->read_local_epoch());
+            }
         }
         // A close requested by the last pass's read/send path has no later flush_ready to drain it,
         // and an undrained entry would show up as a live connection in the shutdown accounting.
diff --git a/src/core/server.h b/src/core/server.h
index d27b1574d..f5aac780a 100644
--- a/src/core/server.h
+++ b/src/core/server.h
@@ -134,7 +134,7 @@ struct FlipReport {
     bool moving = false;
 };
 
-// Allocated only for the boot-armed fused read-local lane. The Server keeps only one pointer at its
+// Allocated only for the boot-armed read-local lane. The Server keeps only one pointer at its
 // true tail, so baseline member offsets and cache-line sharing remain unchanged.
 struct ReadLocalServerState {
     std::atomic<uint64_t> epoch{1};
@@ -580,7 +580,7 @@ public:
     }
 
     static bool read_local_enabled(const Config& cfg) {
-        return cfg.thread_mode == ThreadMode::Fused && cfg.overlap == 0 && cfg.read_local != 0;
+        return cfg.overlap == 0 && cfg.read_local != 0;
     }
     bool read_local_enabled() const { return read_local_enabled(cfg_); }
     uint64_t read_local_epoch() const {
     1	// rl2s.cc — split placement with a shard-less fused-capable IO tier.
     2	// All reader-lane scheduling and split role-tenure wiring for this feature live here.
     3	// The ordinary split runtime and the 1s boot sequence remain independent.
     4	#include "genthread.h"
     5	
     6	#include <algorithm>
     7	#include <cstdio>
     8	#include <string>
     9	#include <thread>
    10	#include <utility>
    11	#include <vector>
    12	#include <unistd.h>
    13	
    14	#include "../base/alloc.h"
    15	#include "../cmd/acl.h"
    16	#include "../net/conn.h"
    17	#include "../net/unix_listener.h"
    18	#include "../persist/aof.h"
    19	#include "../snapshot/snapshot.h"
    20	#include "ex_loop.h"
    21	#include "fused_boot_gate.h"
    22	#include "io_loop.h"
    23	#include "server.h"
    24	#include "shutdown_report.h"
    25	#include "signal_doorbell.h"
    26	
    27	namespace tomo {
    28	namespace {
    29	
    30	void pin_read_local_thread(int cpu) {
    31	    if (cpu < 0) return;
    32	    cpu_set_t set;
    33	    CPU_ZERO(&set);
    34	    CPU_SET(cpu, &set);
    35	    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    36	}
    37	
    38	}  // namespace
    39	
    40	// Same copy/validate/demote lane as 1s, with no owner work. In particular, do not
    41	// inspect self_->shards(): FLIP may be installing a future owner's vector while
    42	// this IO tenure is still returning its final acknowledgements. Live CONFIG's
    43	// reader fields are independent of that vector. Foreign pointers die before
    44	// publication, including when a local read is lowered to an owner task.
    45	template <>
    46	uint32_t ExLoopT<true>::split_read_local_pass() {
    47	    if (!read_local_enabled()) std::abort();
    48	    cached_now_ms_ = realtime_ms();
    49	    refresh_live_config<false>();
    50	    if (maxmemory_enabled_)
    51	        cached_lru_clock_ = static_cast<uint8_t>(
    52	            (static_cast<uint64_t>(cached_now_ms_ / 1000) >> lru_clock_shift_) & 0x1f);
    53	    uint32_t did = drain_local_reads();
    54	    // The shared fused drain books execution as well as parsing in LoopSignals::ops.
    55	    // Split IO's counter means parsed/dispatched commands (FLIP consumes that rate),
    56	    // so cancel that second charge once per pass. Successful reads remain visible
    57	    // in ReadLocalStats and command counts; EX still counts only owner work.
    58	    self_->sig().ops -= did;
    59	    did += read_local_impl().deferred.drain_ready();
    60	    auto& lane = read_local_impl();
    61	    if (__builtin_expect(lane.lane_pressure != 0, false)) lane.lane_pressure--;
    62	    const uint32_t cap = srv_->debug_read_local_lane_cap();
    63	    const uint16_t want = cap == 0 ? static_cast<uint16_t>(kInboxSlots)
    64	        : static_cast<uint16_t>(std::min<uint32_t>(cap, kInboxSlots));
    65	    if (__builtin_expect(lane.lane_admit_cap != want, false)) lane.lane_admit_cap = want;
    66	    self_->publish_read_local_tick(srv_->read_local_epoch());
    67	    return did;
    68	}
    69	
    70	int run_split_read_local_server(Server& srv, const SnapshotLoadPlan* aof_base_plan,
    71	                     const std::vector<std::unique_ptr<AofReplayPlan>>& aof_plans,
    72	                     const SnapshotLoadPlan* load_plan, TlsContext* tls_context,
    73	                     LateUnixListener& unix_listener,
    74	                     ShutdownReportFinalLine& final_report) {
    75	    const Config& cfg = srv.cfg();
    76	    const uint32_t nthreads = srv.nthreads();
    77	    if (cfg.thread_mode != ThreadMode::Split || !srv.read_local_enabled()) std::abort();
    78	    std::printf("tomokv-cpp: %u threads (%zu io + %zu ex), %u shard(s),"
    79	                " thread-mode=2s, overlap=0, read-local=1, %s, alloc=%s\n",
    80	                nthreads, srv.placement().ifid_threads().size(),
    81	                srv.placement().ex_threads().size(), cfg.shards,
    82	                cfg.net_io == NetIoEngine::Epoll ? "epoll" : "io_uring", alloc_backend());
    83	    for (const ThreadPlacement& placement : srv.placement().threads())
    84	        std::printf("  thread t%u: role=%s cpu=%d L3=%u shards=%zu read-local=%u\n",
    85	                    placement.id, placement.role == Role::Ifid ? "ifid" : "ex",
    86	                    placement.cpu, placement.domain, srv.thread(placement.id).shards().size(),
    87	                    placement.role == Role::Ifid ? 1u : 0u);
    88	    std::fflush(stdout);
    89	
    90	    std::vector<std::thread> pool;
    91	    std::vector<IoLoop> ios(nthreads);
    92	    std::vector<FusedExLoop> executors(nthreads);
    93	    // Reuse the 1s boot gate's stop-aware loading/listener barriers. Every physical thread binds
    94	    // its permanent sink and every owner arms its stores before any reader can enter the lane.
    95	    FusedBootGate boot(nthreads);
    96	    // Identical to placement().ifid_threads().front() whenever a unixsocket is configured, but
    97	    // read from the server so the LB's unix_owner_tid_ and the actual attach point cannot drift.
    98	    const uint32_t unix_owner = srv.unix_owner_tid();
    99	    auto report_graceful_shutdown = [&] {
   100	        if (srv.read_local_enabled()) {
   101	            // Joined readers cannot run another grace callback. Empty their private
   102	            // retirement queues and disarm every store hook before taking the immutable report;
   103	            // the subsequent atomic-record drain may detach more than a bounded callback list.
   104	            for (FusedExLoop& executor : executors) executor.read_local_shutdown_drain();
   105	            for (uint32_t sid = 0; sid < srv.nshards(); sid++)
   106	                srv.shard(static_cast<int32_t>(sid)).store().configure_read_local(false, {});
   107	        }
   108	        // All owners and readers are quiescent. Release pending-entry references before IoLoop
   109	        // destruction, then return their deferred ScatterState arenas to the correct IO-owned
   110	        // pools. Server normally outlives those pools, so leaving this to FlatStore destructors
   111	        // would leak the retained arenas.
   112	        for (uint32_t sid = 0; sid < srv.nshards(); sid++)
   113	            srv.shard(static_cast<int32_t>(sid)).store().atomic_shutdown_release_records();
   114	        for (IoLoop& io : ios) io.reap_atomic_deferred();
   115	        ShutdownReport report = collect_shutdown_report(srv, ios, executors);
   116	        print_shutdown_report_human(report);
   117	        final_report.arm(std::move(report));
   118	        acl_shutdown();
   119	    };
   120	
   121	    for (uint32_t tid = 0; tid < nthreads; tid++)
   122	        pool.emplace_back([&, tid] {
   123	            if (cfg.pin_threads) pin_read_local_thread(srv.placement().cpu_of_thread(tid));
   124	            ThreadCtx& self = srv.thread(tid);
   125	            self.latch_placement(srv.topo());
   126	            bind_thread_arena();
   127	            bool ok = self.init_task_inbox_local(srv.placement().ifid_threads(),
   128	                                                 srv.placement().ex_threads());
   129	            if (ok) ok = executors[tid].init(&srv, &self, true);
   130	            std::string local_error;
   131	            if (ok && aof_base_plan)
   132	                ok = snapshot_load_owned(*aof_base_plan, srv, self, local_error);
   133	            if (ok && !aof_plans.empty()) {
   134	                for (const auto& plan : aof_plans) {
   135	                    if (!aof_load_owned(*plan, srv, self, local_error)) {
   136	                        ok = false;
   137	                        break;
   138	                    }
   139	                }
   140	            } else if (ok && load_plan) {
   141	                ok = snapshot_load_owned(*load_plan, srv, self, local_error);
   142	            }
   143	            if (ok)
   144	                ok = ios[tid].init(&srv, &self, cfg.bind_addr, cfg.port, -1,
   145	                                   tls_context, true);
   146	            if (ok)
   147	                self.bind_io_role_hooks(
   148	                    &ios[tid],
   149	                    [](void* p) { return static_cast<IoLoop*>(p)->prepare_activation(); },
   150	                    [](void* p) { static_cast<IoLoop*>(p)->cancel_prepared_activation(); });
   151	            if (ok)
   152	                self.bind_client_registration_hooks(
   153	                    [](void* p, Client* client) {
   154	                        return static_cast<IoLoop*>(p)->prepare_client_registration(client);
   155	                    },
   156	                    [](void* p, Client* client) {
   157	                        static_cast<IoLoop*>(p)->cancel_client_registration(client);
   158	                    });
   159	            if (ok)
   160	                self.bind_client_capacity_hook(
   161	                    [](void* p, uint32_t incoming) {
   162	                        return static_cast<IoLoop*>(p)->prepare_client_transfer_capacity(incoming);
   163	                    });
   164	            if (ok) {
   165	                executors[tid].activate_fused(&ios[tid].ring());
   166	                ios[tid].bind_fused_executor(&executors[tid]);
   167	                executors[tid].bind_fused_completion(
   168	                    &ios[tid],
   169	                    [](void* p, Client* client) {
   170	                        static_cast<IoLoop*>(p)->fused_executor_completion<false>(client);
   171	                    });
   172	                if (srv.read_local_enabled())
   173	                    executors[tid].bind_read_local_demotion(
   174	                        &ios[tid],
   175	                        [](void* p, Client* client, const uint64_t* probed,
   176	                           const ReadLocalFallbackReason* fallbacks,
   177	                           uint32_t probed_count, uint32_t& demoted) {
   178	                            return static_cast<IoLoop*>(p)->fused_demote_local_read_batch(
   179	                                client, probed, fallbacks, probed_count, demoted);
   180	                        });
   181	                self.bind_fused_executor_hooks(
   182	                    &executors[tid],
   183	                    [](void* p) {
   184	                        return static_cast<FusedExLoop*>(p)->split_read_local_pass();
   185	                    },
   186	                    [](void*, SnapshotManager*) {
   187	                        // Snapshot broadcasts target EX only in split placement. The writer's
   188	                        // progress hook above still advances QSBR during blocking SAVE.
   189	                        std::abort();
   190	                    });
   191	            }
   192	            if (!boot.arrive_loaded(tid, ok, local_error)) return;
   193	            if (!boot.wait_until_ready(tid, self.stop_flag())) return;
   194	            if (self.role() == Role::Ifid) {
   195	                if (!self.shards().empty()) std::abort();
   196	                if (!ios[tid].activate()) {
   197	                    boot.give_up(tid, "read-local IO listener activation failed");
   198	                    return;
   199	                }
   200	            } else {
   201	                executors[tid].activate();
   202	            }
   203	            if (!boot.arrive_ready(tid)) return;
   204	            if (!boot.wait_until_running(tid, self.stop_flag())) return;
   205	            for (;;) {
   206	                if (self.stop_flag().load(std::memory_order_relaxed)) break;
   207	                const Role role = self.role();
   208	                if (role == Role::Ifid) {
   209	                    if (!ios[tid].activate()) std::abort();
   210	                    self.publish_ready_role(Role::Ifid);
   211	                    ios[tid].run_fused();
   212	                    self.publish_ready_role(Role::Idle);
   213	                    if (!self.stop_flag().load(std::memory_order_relaxed)) ios[tid].deactivate();
   214	                } else if (role == Role::Ex) {
   215	                    executors[tid].activate();
   216	                    self.publish_ready_role(Role::Ex);
   217	                    executors[tid].run();
   218	                    self.publish_ready_role(Role::Idle);
   219	                } else {
   220	                    std::this_thread::yield();
   221	                }
   222	            }
   223	            self.publish_read_local_parked(srv.read_local_epoch());
   224	        });
   225	
   226	    auto stop_workers = [&] {
   227	        for (uint32_t tid = 0; tid < nthreads; tid++)
   228	            srv.thread(tid).stop_flag().store(true, std::memory_order_relaxed);
   229	        boot.stop();
   230	        for (std::thread& worker : pool)
   231	            if (worker.joinable()) worker.join();
   232	    };
   233	    if (!boot.wait_loaded(srv.shutting_down())) {
   234	        stop_workers();
   235	        const std::string error = boot.error();
   236	        if (!error.empty()) std::fprintf(stderr, "persistence load failed: %s\n", error.c_str());
   237	        const bool interrupted = srv.shutting_down().load(std::memory_order_relaxed);
   238	        if (interrupted) report_graceful_shutdown();
   239	        return interrupted ? 0 : 1;
   240	    }
   241	    srv.set_loading(false);
   242	    if (srv.shutting_down().load(std::memory_order_relaxed)) {
   243	        stop_workers();
   244	        report_graceful_shutdown();
   245	        return 0;
   246	    }
   247	
   248	    auto probe_listener = [&](uint32_t port, bool tls) {
   249	        if (!port) return true;
   250	        const int probe = IoLoop::make_reuseport_listener(
   251	            cfg.bind_addr, port, cfg.tcp_backlog, tls);
   252	        if (probe < 0) return false;
   253	        ::close(probe);
   254	        return true;
   255	    };
   256	    const bool port_ok = probe_listener(cfg.port, false);
   257	    const bool tls_port_ok = port_ok && probe_listener(cfg.tls_port, true);
   258	    if (!port_ok || !tls_port_ok) {
   259	        std::perror(port_ok ? "bind tls-port" : "bind");
   260	        stop_workers();
   261	        return 1;
   262	    }
   263	    std::string unix_error;
   264	    if (!unix_listener.open(cfg.tcp_backlog, unix_error)) {
   265	        std::fprintf(stderr, "%s\n", unix_error.c_str());
   266	        stop_workers();
   267	        return 1;
   268	    }
   269	    if (unix_listener.fd() >= 0) {
   270	        if (!ios[unix_owner].attach_listener(unix_listener.fd())) {
   271	            std::fprintf(stderr, "unix listener attach failed on t%u\n", unix_owner);
   272	            stop_workers();
   273	            return 1;
   274	        }
   275	        (void)unix_listener.release_fd();
   276	    }
   277	    if (!boot.advance_ready(srv.shutting_down()) ||
   278	        !boot.wait_ready(srv.shutting_down()) ||
   279	        !boot.advance_running(srv.shutting_down())) {
   280	        stop_workers();
   281	        const std::string error = boot.error();
   282	        if (!error.empty()) std::fprintf(stderr, "split read-local boot failed: %s\n", error.c_str());
   283	        const bool interrupted = srv.shutting_down().load(std::memory_order_relaxed);
   284	        if (interrupted) report_graceful_shutdown();
   285	        return interrupted ? 0 : 1;
   286	    }
   287	
   288	    // Reached only after every ring, sink and listener is ready and no stop edge was taken.
   289	    if (cfg.port) std::printf("listening on %s:%u\n", cfg.bind_addr, cfg.port);
   290	    if (cfg.tls_port) std::printf("listening with TLS on %s:%u\n", cfg.bind_addr, cfg.tls_port);
   291	    if (unix_listener.bound()) std::printf("listening on unix:%s\n", cfg.unixsocket);
   292	    std::fflush(stdout);
   293	
   294	    if (srv.flipctl_enabled()) {
   295	        while (!srv.shutting_down().load(std::memory_order_relaxed)) {
   296	            (void)srv.flipctl_tick(now_ns() / 1000000ull);
   297	            if (srv.shutting_down().load(std::memory_order_relaxed)) break;
   298	            (void)signal_doorbell_wait(srv.flipctl_wait_ms());
   299	        }
   300	    }
   301	
   302	    for (std::thread& worker : pool) worker.join();
   303	    // The unix socket file is unlinked by its RAII owner in main, for every return path.
   304	    report_graceful_shutdown();
   305	    return 0;
   306	}
   307	
   308	}  // namespace tomo

codex
The boot paths are separate: 2s/off still constructs ordinary split executors, and the new fused-capable runtime sits behind the enabled branch. I did find an off-arm change in `INFO server`: it walks the ownership directory and adds per-thread output even when read-local is off. I’m checking the shared predicates and template paths before fixing that leak.
exec
/bin/bash -lc 'cat RL2S.md' in /home/user/Projects/cx-rl2s
 succeeded in 0ms:
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

All physical threads provision an `IoLoop` and `FusedExLoop`. They use the
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
production build, asserted at `src/core/thread.h:306`. An atomic loop-entry
flag plus alignment occupies the extra word, after all existing members.
The diagnostics-only selector-3 sidecar already has different alignment
and is excluded from this production-size assert. Read-local 0 allocates
no lane, deferred queue, thread/server sidecar, or store read-local state.
Armed split allocates the existing fused-capable executor state on each
physical thread, including dormant future owners; measure that fixed
memory cost as well as throughput.

## Observable activation

`INFO server` still reports `thread_mode:2s`, live `io_threads` and
`ex_threads`, and effective `read_local:1`. It now also reports:

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
(`src/cmd/t_server.cc:1995`). After FLIP settles, every IFID row must have
shards=0/active=1, and every EX row active=0. As with other INFO geometry,
the individual samples are not one transactional view during a move.

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
   lane bit and active-reader count to match: 0, 6, 0, 8. Also smoke 2s/1
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

exec
/bin/bash -lc "nl -ba src/main.cc | sed -n '300,520p'
nl -ba src/core/server.h | sed -n '210,395p'
nl -ba src/core/server.h | sed -n '532,650p'" in /home/user/Projects/cx-rl2s
 succeeded in 0ms:
   300	    LateUnixListener unix_listener(cfg.unixsocket);
   301	    SignalDoorbell signal_doorbell;
   302	    if (!signal_doorbell.init()) {
   303	        std::perror("eventfd shutdown doorbell");
   304	        return 1;
   305	    }
   306	    ScopedSignalHandlers signal_handlers;
   307	    if (!signal_handlers.arm(srv)) {
   308	        std::perror("install signal handlers");
   309	        return 1;
   310	    }
   311	
   312	    if (cfg.thread_mode == ThreadMode::Fused) {
   313	        srv.topo().dump(stdout);
   314	        return run_fused_server(srv, aof_base_plan.get(), aof_plans, load_plan.get(),
   315	                                tls_context.get(), unix_listener, final_shutdown_line);
   316	    }
   317	    if (srv.read_local_enabled()) {
   318	        srv.topo().dump(stdout);
   319	        return run_split_read_local_server(srv, aof_base_plan.get(), aof_plans, load_plan.get(),
   320	                                           tls_context.get(), unix_listener, final_shutdown_line);
   321	    }
   322	
   323	    srv.topo().dump(stdout);
   324	    std::printf("tomokv-cpp: %u threads (%zu io + %zu ex), %u shard(s),"
   325	                " thread-mode=2s, overlap=%u, %s, alloc=%s\n", srv.nthreads(),
   326	                srv.placement().ifid_threads().size(), srv.placement().ex_threads().size(),
   327	                cfg.shards, cfg.overlap,
   328	                cfg.net_io == NetIoEngine::Epoll ? "epoll" : "io_uring", alloc_backend());
   329	    for (const ThreadPlacement& p : srv.placement().threads()) {
   330	        const char* role = p.role == Role::Ifid ? "ifid" : p.role == Role::Ex ? "ex" : "idle";
   331	        std::printf("  thread t%u: role=%s cpu=%d L3=%u shards=%zu\n", p.id, role, p.cpu,
   332	                    p.domain, srv.thread(p.id).shards().size());
   333	    }
   334	    std::fflush(stdout);
   335	
   336	    // Placement decides every cpu directly. Pinning is relative to the process's ALLOWED set by
   337	    // construction because both discovery and --place validation intersect with sched affinity.
   338	    const uint32_t nthreads = srv.nthreads();
   339	    std::vector<std::thread> pool;
   340	    std::vector<IoLoop> ios(nthreads);
   341	    std::vector<ExLoop> exs(nthreads);
   342	    std::mutex load_mu;
   343	    std::condition_variable load_cv;
   344	    uint32_t loaders_done = 0;
   345	    bool load_ok = true;
   346	    std::string load_error;
   347	    // An io thread that fails to provision after the listeners were probed cannot report through
   348	    // load_ok (that barrier has passed); it stops every thread and records the failure here so
   349	    // the exit status says what happened instead of a silent 0.
   350	    std::atomic<bool> io_boot_failed{false};
   351	
   352	    auto pin_for = [&](uint32_t tid) {
   353	        if (cfg.pin_threads) pin_to(srv.placement().cpu_of_thread(tid));
   354	    };
   355	    auto report_graceful_shutdown = [&] {
   356	        // All owners and readers are quiescent. Release pending-entry references before IoLoop
   357	        // destruction, then return their deferred ScatterState arenas to the correct IO-owned
   358	        // pools. Server normally outlives those pools, so leaving this to FlatStore destructors
   359	        // would leak the retained arenas.
   360	        for (uint32_t sid = 0; sid < srv.nshards(); sid++)
   361	            srv.shard(static_cast<int32_t>(sid)).store().atomic_shutdown_release_records();
   362	        for (IoLoop& io : ios) io.reap_atomic_deferred();
   363	        ShutdownReport report = collect_shutdown_report(srv, ios, exs);
   364	        print_shutdown_report_human(report);
   365	        final_shutdown_line.arm(std::move(report));
   366	        acl_shutdown();
   367	    };
   368	
   369	    // Workers and senders BEFORE io: an io thread that dispatches or hands off to a thread whose
   370	    // ring does not exist yet would find nothing to wake, and the work would sit until something
   371	    // unrelated poked the peer.
   372	    for (uint32_t tid : srv.placement().ex_threads())
   373	        pool.emplace_back([&, tid] {
   374	            pin_for(tid);
   375	            ThreadCtx& self = srv.thread(tid);
   376	            self.latch_placement(srv.topo());   // after pinning: sched_getcpu is only now truthful
   377	            bind_thread_arena();                // per-worker jemalloc arena; no-op without it
   378	            bool ok = self.init_task_inbox_local(srv.placement().ifid_threads(),
   379	                                                 srv.placement().ex_threads());
   380	            if (ok) ok = exs[tid].init(&srv, &self);
   381	            std::string local_error;
   382	            if (ok && aof_base_plan)
   383	                ok = snapshot_load_owned(*aof_base_plan, srv, self, local_error);
   384	            if (ok && !aof_plans.empty()) {
   385	                for (const auto& plan : aof_plans) {
   386	                    if (!aof_load_owned(*plan, srv, self, local_error)) { ok = false; break; }
   387	                }
   388	            } else if (ok && load_plan) {
   389	                ok = snapshot_load_owned(*load_plan, srv, self, local_error);
   390	            }
   391	            // Provision the opposite loop before runtime mutation is possible. Dormant IO creates
   392	            // its ring/epoll/WB state but no listener and does not bind AOF, so boot loading still
   393	            // happens before any connection can arrive and before the initial AOF writer exists.
   394	            if (ok)
   395	                ok = ios[tid].init(&srv, &self, cfg.bind_addr, cfg.port, -1,
   396	                                   tls_context.get(), true);
   397	            if (ok)
   398	                self.bind_io_role_hooks(
   399	                    &ios[tid],
   400	                    [](void* p) { return static_cast<IoLoop*>(p)->prepare_activation(); },
   401	                    [](void* p) { static_cast<IoLoop*>(p)->cancel_prepared_activation(); });
   402	            if (ok)
   403	                self.bind_client_registration_hooks(
   404	                    [](void* p, Client* client) {
   405	                        return static_cast<IoLoop*>(p)->prepare_client_registration(client);
   406	                    },
   407	                    [](void* p, Client* client) {
   408	                        static_cast<IoLoop*>(p)->cancel_client_registration(client);
   409	                    });
   410	            if (ok)
   411	                self.bind_client_capacity_hook(
   412	                    [](void* p, uint32_t incoming) {
   413	                        return static_cast<IoLoop*>(p)->prepare_client_transfer_capacity(incoming);
   414	                    });
   415	            {
   416	                std::lock_guard<std::mutex> lock(load_mu);
   417	                if (!ok) {
   418	                    load_ok = false;
   419	                    if (load_error.empty())
   420	                        load_error = local_error.empty() ? "executor initialization failed" : local_error;
   421	                }
   422	                loaders_done++;
   423	            }
   424	            load_cv.notify_one();
   425	            if (!ok) return;
   426	            for (;;) {
   427	                if (self.stop_flag().load(std::memory_order_relaxed)) break;
   428	                const Role role = self.role();
   429	                if (role == Role::Ex) {
   430	                    exs[tid].activate();
   431	                    self.publish_ready_role(Role::Ex);
   432	                    exs[tid].run();
   433	                    self.publish_ready_role(Role::Idle);
   434	                } else if (role == Role::Ifid) {
   435	                    if (!ios[tid].activate()) std::abort();
   436	                    self.publish_ready_role(Role::Ifid);
   437	                    ios[tid].run();
   438	                    self.publish_ready_role(Role::Idle);
   439	                    if (!self.stop_flag().load(std::memory_order_relaxed)) ios[tid].deactivate();
   440	                } else {
   441	                    std::this_thread::yield();
   442	                }
   443	            }
   444	        });
   445	
   446	    // Main performed every read(2); the real owning executor threads now deserialize their own
   447	    // shard sections in parallel. No TCP listener exists until all owners report success.
   448	    {
   449	        std::unique_lock<std::mutex> lock(load_mu);
   450	        load_cv.wait(lock, [&] {
   451	            return loaders_done == static_cast<uint32_t>(srv.placement().ex_threads().size());
   452	        });
   453	    }
   454	    if (!load_ok) {
   455	        for (uint32_t i = 0; i < nthreads; i++) srv.thread(i).stop_flag().store(true);
   456	        for (auto& thread : pool) thread.join();
   457	        if (srv.shutting_down().load(std::memory_order_relaxed)) {
   458	            srv.set_loading(false);
   459	            report_graceful_shutdown();
   460	            return 0;
   461	        }
   462	        std::fprintf(stderr, "persistence load failed: %s\n", load_error.c_str());
   463	        return 1;
   464	    }
   465	    srv.set_loading(false);
   466	    if (srv.shutting_down().load(std::memory_order_relaxed)) {
   467	        for (uint32_t i = 0; i < nthreads; i++)
   468	            srv.thread(i).stop_flag().store(true, std::memory_order_relaxed);
   469	        for (auto& thread : pool)
   470	            if (thread.joinable()) thread.join();
   471	        report_graceful_shutdown();
   472	        return 0;
   473	    }
   474	
   475	    // Probe only after boot load. Each io thread then opens its own SO_REUSEPORT listener.
   476	    if (cfg.port) {
   477	        const int probe = IoLoop::make_reuseport_listener(
   478	            cfg.bind_addr, cfg.port, srv.cfg().tcp_backlog);
   479	        if (probe < 0) {
   480	            std::perror("bind");
   481	            for (uint32_t i = 0; i < nthreads; i++) srv.thread(i).stop_flag().store(true);
   482	            for (auto& thread : pool) thread.join();
   483	            return 1;
   484	        }
   485	        ::close(probe);
   486	    }
   487	    if (cfg.tls_port) {
   488	        const int probe = IoLoop::make_reuseport_listener(
   489	            cfg.bind_addr, cfg.tls_port, srv.cfg().tcp_backlog, true);
   490	        if (probe < 0) {
   491	            std::perror("bind tls-port");
   492	            for (uint32_t i = 0; i < nthreads; i++) srv.thread(i).stop_flag().store(true);
   493	            for (auto& thread : pool) thread.join();
   494	            return 1;
   495	        }
   496	        ::close(probe);
   497	    }
   498	    std::string unix_error;
   499	    if (!unix_listener.open(cfg.tcp_backlog, unix_error)) {
   500	        std::fprintf(stderr, "%s\n", unix_error.c_str());
   501	        for (uint32_t i = 0; i < nthreads; i++) srv.thread(i).stop_flag().store(true);
   502	        for (auto& thread : pool) thread.join();
   503	        return 1;
   504	    }
   505	
   506	    const uint32_t unix_owner = srv.unix_owner_tid();
   507	    for (uint32_t tid : srv.placement().ifid_threads())
   508	        pool.emplace_back([&, tid] {
   509	            pin_for(tid);
   510	            ThreadCtx& self = srv.thread(tid);
   511	            self.latch_placement(srv.topo());
   512	            bind_thread_arena();
   513	            auto boot_failed = [&](const char* what) {
   514	                std::fprintf(stderr, "%s failed on t%u\n", what, tid);
   515	                io_boot_failed.store(true, std::memory_order_relaxed);
   516	                for (uint32_t i = 0; i < nthreads; i++)
   517	                    srv.thread(i).stop_flag().store(true, std::memory_order_relaxed);
   518	            };
   519	            if (!self.init_task_inbox_local(srv.placement().ifid_threads(),
   520	                                            srv.placement().ex_threads())) {
   210	        if (cfg_.script_crossshard_conflict_retries == -1)
   211	            cfg_.script_crossshard_conflict_retries = 8;
   212	        if (cfg_.script_crossshard_cut_slots == -1)
   213	            cfg_.script_crossshard_cut_slots = 4;
   214	        live_config_version_.store(2, std::memory_order_release);  // even versions are stable
   215	        // Declared topology is a legacy lowering input and therefore cannot accompany --place.
   216	        // A declaration that fails to parse or names cpus outside the affinity mask fails the BOOT,
   217	        // loudly: silently falling back to discovery would measure a different layout.
   218	        if (cfg.place && cfg.l3_domains && *cfg.l3_domains) {
   219	            std::fprintf(stderr, "fatal: --place and --l3-domains are mutually exclusive\n");
   220	            return false;
   221	        }
   222	        if (cfg.l3_domains && *cfg.l3_domains) {
   223	            if (!topo_.declare(cfg.l3_domains)) {
   224	                std::fprintf(stderr, "fatal: --l3-domains '%s' invalid\n", cfg.l3_domains);
   225	                return false;
   226	            }
   227	        } else {
   228	            if (!topo_.discover()) {
   229	                std::fprintf(stderr, "fatal: could not discover any allowed cpu\n");
   230	                return false;
   231	            }
   232	        }
   233	        if (cfg.smt_mode && !topo_.discover_thread_siblings()) {
   234	            std::fprintf(stderr,
   235	                         "fatal: --smt-mode could not read Linux thread_siblings_list topology\n");
   236	            return false;
   237	        }
   238	        bool placed = false;
   239	        if (cfg.thread_mode == ThreadMode::Fused) {
   240	            placed = placement_.build_fused(topo_, cfg.place);
   241	        } else {
   242	            // Default shape: an even io/ex split of every allowed cpu, io taking the odd one out --
   243	            // the measured 2s center (4:4-class) generalized to any core count.
   244	            uint32_t di = cfg.even_ifid, de = cfg.even_ex;
   245	            if (!cfg.place && !(di | de)) {
   246	                uint32_t n = 0;
   247	                for (uint32_t d = 0; d < topo_.ndomains(); d++)
   248	                    n += static_cast<uint32_t>(topo_.cpus_in(d).size());
   249	                di = n - n / 2; de = n / 2;
   250	            }
   251	            placed = cfg.place
   252	                ? placement_.build_explicit(topo_, cfg.place)
   253	                : placement_.build_even(topo_, di, de, cfg.smt_mode != 0);
   254	        }
   255	        if (!placed) return false;
   256	        if (!placement_.configure_smt_units(topo_, cfg.smt_mode != 0)) return false;
   257	        if (!placement_.reserve_runtime_roles(placement_.total_threads())) return false;
   258	        if (placement_.ifid_threads().empty() || placement_.ex_threads().empty()) {
   259	            std::fprintf(stderr, "placement needs at least one ifid and one ex thread\n");
   260	            return false;
   261	        }
   262	        unix_owner_tid_ = cfg.unixsocket && *cfg.unixsocket
   263	            ? placement_.ifid_threads().front() : UINT32_MAX;
   264	        if (!adjust_open_files_limit()) return false;
   265	        check_tcp_backlog_settings();
   266	        // Shard maps are resolved exactly once at boot; parsing never leaks onto a request path.
   267	        if (!placement_.assign_shard_homes(cfg.shards, cfg.shard_home)) return false;
   268	
   269	        // ---- shards: bucket ranges, fixed for the life of the process ----------------------------
   270	        shards_.resize(cfg.shards);
   271	        const uint32_t per = kNumBuckets / cfg.shards;
   272	        for (uint32_t i = 0; i < cfg.shards; i++) {
   273	            const uint32_t b0 = i * per;
   274	            const uint32_t b1 = (i + 1 == cfg.shards) ? kNumBuckets : (i + 1) * per;
   275	            shards_[i] = std::make_unique<Shard>();
   276	            shards_[i]->init(this, static_cast<int32_t>(i), b0, b1, cfg.zc_min, cfg.type_limits,
   277	                             cfg.stream_limits);
   278	            if (key_lb_signals_enabled() && !shards_[i]->enable_lb_signals()) {
   279	                std::fprintf(stderr, "fatal: could not allocate weighted-placement signals\n");
   280	                return false;
   281	            }
   282	            shards_[i]->bind_atomic_state(
   283	                [](void* ctx) { return static_cast<Server*>(ctx)->atomic_commit(); }, this,
   284	                &atomic_activity_, &script_intent_owners_);
   285	        }
   286	        router_.build_uniform(static_cast<int32_t>(cfg.shards));
   287	
   288	        // ---- threads ---------------------------------------------------------------------------
   289	        // Every front-end has already lowered to dense per-thread entries. Every thread still gets
   290	        // a producer lane from every other, because a role change must not require re-wiring
   291	        // the mesh; the TASK lanes are a role-partitioned masked monolith whose block sizes are
   292	        // re-derived at each role change (ThreadCtx::remask_quiesced), while the client, release
   293	        // and transfer channels stay uniform per-thread arrays.
   294	        const uint32_t nthreads = placement_.total_threads();
   295	        if (!flipctl_.init(cfg.thread_mode == ThreadMode::Split && cfg.flip_auto != 0,
   296	                           cfg.flip_auto_band, nthreads)) {
   297	            std::fprintf(stderr, "fatal: could not allocate flip controller state\n");
   298	            return false;
   299	        }
   300	        for (uint32_t i = 0; i < kMaxThreads; i++) executor_slots_[i] = UINT8_MAX;
   301	        // Role changes may make any physical thread an executor. Stable tid-indexed slots avoid
   302	        // renumbering live atomic-group arrays at each flip.
   303	        for (uint32_t tid = 0; tid < nthreads; tid++)
   304	            executor_slots_[tid] = static_cast<uint8_t>(tid);
   305	        threads_.resize(nthreads);
   306	        for (uint32_t i = 0; i < nthreads; i++) {
   307	            threads_[i] = std::make_unique<ThreadCtx>();
   308	            // The fingerprint writer is armed only when its one reader, the flip controller, is
   309	            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
   310	            // one predicted branch per op. flip_work_window keeps its CONFIG value either way.
   311	            threads_[i]->init(i, placement_.role_of(i), nthreads,
   312	                              cfg.flip_auto ? 0 : cfg.lb_age_sample_rate,
   313	                              flipctl_.enabled() ? cfg.flip_work_window : 0);
   314	            threads_[i]->init_command_counts(command_registry_size());
   315	        }
   316	        if (read_local_enabled()) {
   317	            read_local_state_.reset(new (std::nothrow) ReadLocalServerState);
   318	            if (!read_local_state_) {
   319	                std::fprintf(stderr, "fatal: could not allocate read-local server state\n");
   320	                return false;
   321	            }
   322	            for (const auto& thread : threads_) {
   323	                if (!thread->init_read_local_state()) {
   324	                    std::fprintf(stderr, "fatal: could not allocate read-local thread state\n");
   325	                    return false;
   326	                }
   327	            }
   328	            for (const auto& shard : shards_) {
   329	                if (!shard->store().prepare_read_local()) {
   330	                    std::fprintf(stderr, "fatal: could not allocate read-local store state\n");
   331	                    return false;
   332	                }
   333	            }
   334	        }
   335	        if (key_lb_signals_enabled()) {
   336	            try {
   337	                lb_bucket_last_samples_.assign(kNumBuckets, 0);
   338	                lb_bucket_weight_.assign(kNumBuckets, 0.0);
   339	                lb_bucket_last_move_ms_.assign(kNumBuckets, 0);
   340	            } catch (const std::bad_alloc&) {
   341	                std::fprintf(stderr, "fatal: could not allocate weighted key-placement windows\n");
   342	                return false;
   343	            }
   344	        }
   345	        if (lb_controller_enabled()) {
   346	            try {
   347	                lb_thread_last_busy_.assign(nthreads, 0);
   348	                lb_thread_last_idle_.assign(nthreads, 0);
   349	                lb_thread_occupancy_.assign(nthreads, 0.0);
   350	            } catch (const std::bad_alloc&) {
   351	                std::fprintf(stderr, "fatal: could not allocate shared LB windows\n");
   352	                return false;
   353	            }
   354	        }
   355	        for (uint32_t i = 0; i < nthreads; i++)
   356	            atomic_read_floors_[i].store(UINT64_MAX, std::memory_order_relaxed);
   357	        for (uint32_t i = 0; i < nthreads; i++)
   358	            atomic_snapshot_completions_[i].store(0, std::memory_order_relaxed);
   359	        flip_target_io_.store(static_cast<uint32_t>(placement_.ifid_threads().size()),
   360	                              std::memory_order_relaxed);
   361	        flip_target_ex_.store(static_cast<uint32_t>(placement_.ex_threads().size()),
   362	                              std::memory_order_relaxed);
   363	
   364	        // ---- shards directly onto workers ------------------------------------------------------
   365	        // Locality resolution stops at the individual EX thread. There is no contiguous node range:
   366	        // the default is one flat round-robin across every executor in thread-id order.
   367	        for (uint32_t sid = 0; sid < cfg.shards; sid++) {
   368	            const uint32_t tid = placement_.shard_home(sid);
   369	            threads_[tid]->shards().push_back(shards_[sid].get());
   370	            // Populate Router's authoritative bucket entries and its derived dispatch fast path.
   371	            set_worker_of_shard(static_cast<int32_t>(sid), tid);
   372	            // Seed residency from this exact thread's cpu domain, so note_execution compares at
   373	            // thread resolution even when adjacent tids live in different L3 domains.
   374	            shards_[sid]->note_migration(placement_.domain_of_thread(tid));
   375	        }
   376	        snapshot_.init(nthreads, cfg.shards,
   377	                       static_cast<uint32_t>(placement_.ex_threads().size()),
   378	                       cfg.dir, cfg.dbfilename, cfg.persist_io);
   379	        aof_.init(*this, cfg, nthreads, cfg.shards,
   380	                  placement_.ifid_threads().back(), aof_replay);
   381	        for (uint32_t sid = 0; sid < cfg.shards; sid++) {
   382	            const uint32_t sequence = aof_replay && aof_replay->next_sequence.size() == cfg.shards
   383	                ? aof_replay->next_sequence[sid] : 0;
   384	            shards_[sid]->store().bind_aof(cfg.appendonly ? &aof_ : nullptr,
   385	                                           static_cast<int32_t>(sid), sequence);
   386	        }
   387	        return true;
   388	    }
   389	
   390	    const Config&    cfg()        const { return cfg_; }
   391	    ThreadMode thread_mode() const { return cfg_.thread_mode; }
   392	    const char* thread_mode_name() const {
   393	        return cfg_.thread_mode == ThreadMode::Fused ? "1s" : "2s";
   394	    }
   395	
   532	    Placement&       placement()        { return placement_; }
   533	    Router&          router()           { return router_; }
   534	    Shard&           shard(int32_t i)   { return *shards_[i]; }
   535	    const Shard&     shard(int32_t i) const { return *shards_[i]; }
   536	    ThreadCtx&       thread(uint32_t i) { return *threads_[i]; }
   537	    uint32_t         nthreads()   const { return static_cast<uint32_t>(threads_.size()); }
   538	    uint32_t         nshards()    const { return static_cast<uint32_t>(shards_.size()); }
   539	    SnapshotManager& snapshot()         { return snapshot_; }
   540	    const SnapshotManager& snapshot() const { return snapshot_; }
   541	    AofManager& aof() { return aof_; }
   542	    const AofManager& aof() const { return aof_; }
   543	    const ThreadCtx& thread(uint32_t i) const { return *threads_[i]; }
   544	
   545	    // Role is the split-mode loop state and deliberately remains Ifid/Ex. Capabilities are the
   546	    // mode-independent truth used by cold control/reporting surfaces: a fused thread serves
   547	    // clients AND owns shards even though its operational loop role is encoded as Ifid.
   548	    bool serves_clients(Role role) const {
   549	        return role != Role::Idle &&
   550	               (cfg_.thread_mode == ThreadMode::Fused || role == Role::Ifid);
   551	    }
   552	    bool owns_shards(Role role) const {
   553	        return role != Role::Idle &&
   554	               (cfg_.thread_mode == ThreadMode::Fused || role == Role::Ex);
   555	    }
   556	    bool serves_clients(uint32_t tid) const {
   557	        return tid < nthreads() && serves_clients(thread(tid).role());
   558	    }
   559	    bool owns_shards(uint32_t tid) const {
   560	        return tid < nthreads() && owns_shards(thread(tid).role());
   561	    }
   562	    uint32_t client_serving_thread_count() const {
   563	        uint32_t count = 0;
   564	        for (uint32_t tid = 0; tid < nthreads(); tid++) count += serves_clients(tid);
   565	        return count;
   566	    }
   567	    // "Owner" is the observable topology term: distinct tids currently named by shard rows. It
   568	    // can be smaller than the number of owner-capable threads when there are fewer shards.
   569	    uint32_t shard_owner_count() const {
   570	        bool seen[kMaxThreads] = {};
   571	        uint32_t count = 0;
   572	        for (uint32_t sid = 0; sid < nshards(); sid++) {
   573	            const uint32_t owner = worker_of_shard(static_cast<int32_t>(sid));
   574	            if (owner < nthreads() && !seen[owner]) {
   575	                seen[owner] = true;
   576	                count++;
   577	            }
   578	        }
   579	        return count;
   580	    }
   581	
   582	    static bool read_local_enabled(const Config& cfg) {
   583	        return cfg.overlap == 0 && cfg.read_local != 0;
   584	    }
   585	    bool read_local_enabled() const { return read_local_enabled(cfg_); }
   586	    uint64_t read_local_epoch() const {
   587	        if (!read_local_state_) std::abort();
   588	        return read_local_state_->epoch.load(std::memory_order_seq_cst);
   589	    }
   590	    // Retirement uses the returned OLD value as its stamp. The increment makes a subsequent
   591	    // rotation publication strictly newer, which is the grace test below.
   592	    uint64_t advance_read_local_epoch() {
   593	        if (!read_local_state_) std::abort();
   594	        return read_local_state_->epoch.fetch_add(1, std::memory_order_seq_cst);
   595	    }
   596	    // Grace floor for a QSBR drain, bounded by what the caller needs. `stamp` is the oldest sealed
   597	    // retirement stamp; the drain releases an entry only when the floor is STRICTLY above its stamp.
   598	    // Returns the exact floor (minimum tick over active participants) when that floor is above
   599	    // `stamp`; otherwise returns a value <= `stamp` as soon as one active participant with
   600	    // tick <= `stamp` is found. Both answers produce exactly the decision a full scan would: with a
   601	    // blocking participant the true floor is <= its tick <= stamp, so nothing is releasable and the
   602	    // caller stops at its first batch either way. What changes is the cost of the not-ready case:
   603	    // one or two foreign tick lines instead of one per thread. `hint` remembers the blocking
   604	    // participant so the next pass tests it first; it is caller-owned scratch and never read for
   605	    // anything else. Parked participants contribute infinity because they hold no foreign pointer.
   606	    uint64_t read_local_grace_floor(uint64_t stamp, uint32_t& hint) const {
   607	        const uint32_t participants = static_cast<uint32_t>(threads_.size());
   608	        if (hint < participants) {
   609	            const uint64_t publication = threads_[hint]->read_local_publication();
   610	            if (!ThreadCtx::read_local_publication_parked(publication)) {
   611	                const uint64_t tick = ThreadCtx::read_local_publication_tick(publication);
   612	                if (tick <= stamp) return tick;
   613	            }
   614	        }
   615	        uint64_t floor = UINT64_MAX;
   616	        for (uint32_t i = 0; i < participants; i++) {
   617	            const uint64_t publication = threads_[i]->read_local_publication();
   618	            if (ThreadCtx::read_local_publication_parked(publication)) continue;
   619	            const uint64_t tick = ThreadCtx::read_local_publication_tick(publication);
   620	            if (tick <= stamp) {
   621	                hint = i;
   622	                return tick;
   623	            }
   624	            floor = std::min(floor, tick);
   625	        }
   626	        return floor;
   627	    }
   628	
   629	    uint32_t role_count(Role role, bool ready = false) const {
   630	        uint32_t count = 0;
   631	        for (const auto& thread : threads_)
   632	            if ((ready ? thread->ready_role() : thread->role()) == role) count++;
   633	        return count;
   634	    }
   635	
   636	    FlipReport flip_report() const {
   637	        FlipReport report;
   638	        report.live_io = role_count(Role::Ifid, true);
   639	        report.live_ex = role_count(Role::Ex, true);
   640	        report.target_io = flip_target_io_.load(std::memory_order_acquire);
   641	        report.target_ex = flip_target_ex_.load(std::memory_order_acquire);
   642	        report.smt_mode = cfg_.smt_mode;
   643	        report.unit_threads = cfg_.smt_mode ? 2u : 1u;
   644	        uint32_t buckets[kMaxThreads] = {};
   645	        for (uint32_t sid = 0; sid < nshards(); sid++) {
   646	            const uint32_t owner = worker_of_shard(static_cast<int32_t>(sid));
   647	            if (owner < nthreads()) buckets[owner]++;
   648	        }
   649	        report.bucket_min = UINT32_MAX;
   650	        report.client_min = UINT32_MAX;

exec
/bin/bash -lc "rg -n 'read_local_enabled\\(' src --glob '"'!ex_loop.h'"' --glob '"'!io_loop.h'"'
rg -n 'read_local_enabled\\(|Fused|static_assert\\(sizeof' src/core/io_loop.h | head -130
rg -n 'static_assert\\(sizeof|offsetof\\(' src/core/{op,client,thread,shard,rob,config,server,io_loop,ex_loop}.h src/store/flatstore.h src/core/atomic.h" in /home/user/Projects/cx-rl2s
 exited 2 in 0ms:
src/core/genthread.cc:101:        if (srv.read_local_enabled()) {
src/core/genthread.cc:179:                if (srv.read_local_enabled())
src/core/genthread.cc:232:            if (srv.read_local_enabled())
src/core/server.h:316:        if (read_local_enabled()) {
src/core/server.h:582:    static bool read_local_enabled(const Config& cfg) {
src/core/server.h:585:    bool read_local_enabled() const { return read_local_enabled(cfg_); }
src/core/server.h:1951:        if (!shard.store().read_local_enabled()) return;
src/core/server.h:1978:            if (!shard.store().read_local_enabled()) continue;
src/core/rl2s.cc:47:    if (!read_local_enabled()) std::abort();
src/core/rl2s.cc:77:    if (cfg.thread_mode != ThreadMode::Split || !srv.read_local_enabled()) std::abort();
src/core/rl2s.cc:100:        if (srv.read_local_enabled()) {
src/core/rl2s.cc:172:                if (srv.read_local_enabled())
src/store/flatstore.h:784:    bool read_local_enabled() const { return read_local_enabled_; }
src/cmd/atomics_glue.inc:806:    const bool scoped_foreign_write = store.read_local_enabled() &&
src/main.cc:216:                               Server::read_local_enabled(cfg))) {
src/main.cc:317:    if (srv.read_local_enabled()) {
src/cmd/t_server.cc:1656:    if (g_server->read_local_enabled()) {
src/cmd/t_server.cc:1840:        if (g_server->read_local_enabled()) {
src/cmd/t_server.cc:1849:    if (g_server && g_server->read_local_enabled()) {
src/cmd/t_server.cc:1875:    if (g_server && g_server->read_local_enabled()) {
src/cmd/t_server.cc:1989:                g_server && g_server->read_local_enabled() ? 1u : 0u,
src/cmd/t_server.cc:2009:                const ReadLocalStats* stats = g_server->read_local_enabled()
96:        targeted_ifid_ = srv_->cfg().thread_mode == ThreadMode::Fused &&
144:            srv_->cfg().thread_mode == ThreadMode::Fused &&
434:    void bind_fused_executor(FusedExLoop* executor) {
526:    template <bool HasUnix, bool HasTls, bool kEp, bool Fused = false,
530:        if constexpr (Fused && Pipeline == 1) {
534:        if constexpr (Fused && Pipeline == 2) {
540:        constexpr bool IoPipe = !Fused && Pipeline == 1;
541:        if constexpr (Fused) {
542:            if (srv_->read_local_enabled()) {
630:                            on_cqe<HasTls, kEp, Fused, Pipeline>(cqe);
633:                        did += epoll_pass<HasUnix, HasTls, Fused, Pipeline>(0);
659:                } else if constexpr (Fused) {
719:                sweep_work = sweep<HasUnix, HasTls, kEp, Fused>();
730:            if constexpr (Fused) {
731:                if (__builtin_expect(srv_->read_local_enabled(), false))
739:                if constexpr (Fused) {
746:                if constexpr (Fused) {
754:            if constexpr (Fused) {
759:                if (__builtin_expect(srv_->read_local_enabled(), false)) {
766:        if constexpr (Fused) {
769:            if (srv_->read_local_enabled()) {
931:                if (++non_send_rotations >= kGenthreadIoFusedCoalesceRotations) {
944:                    ++non_send_rotations >= kGenthreadIoFusedCoalesceRotations) {
1023:                       FusedExLoop::SnapshotOwnerState::None;
2008:    template <bool kEp, bool Fused = false, uint8_t Pipeline = 0>
2016:            (void)wb_.pump_tls<kEp, Fused && Pipeline == 1>(*c, *tls);
2031:                on_tls_recv<kEp, Fused, Pipeline>(c, static_cast<int>(n));
2094:    template <bool HasUnix, bool HasTls, bool Fused = false, uint8_t Pipeline = 0>
2104:                    work += epoll_accept<true, Fused, Pipeline>(UrKind::Accept); break;
2107:                        work += epoll_accept<true, Fused, Pipeline>(UrKind::TlsAccept);
2111:                        work += epoll_accept<true, Fused, Pipeline>(UrKind::UnixAccept);
2162:                    mark_active_known<Fused && Pipeline != 0>(c);
2207:    template <bool HasTls, bool kEp, bool Fused = false, uint8_t Pipeline = 0>
2209:        constexpr bool ImmediateSendProgress = !(Fused && Pipeline != 0);
2214:                    on_accept<kEp, Fused, Pipeline>(cqe, UrKind::Accept); break;
2216:                    on_accept<kEp, Fused, Pipeline>(cqe, UrKind::UnixAccept); break;
2218:                    on_recv<false, kEp, Fused, Pipeline>(
2223:                                      Fused && Pipeline == 1>(cqe); break;
2227:                    if constexpr (Fused)
2246:                    on_accept<kEp, Fused, Pipeline>(cqe, UrKind::Accept); break;
2248:                    on_accept<kEp, Fused, Pipeline>(cqe, UrKind::TlsAccept); break;
2250:                    on_accept<kEp, Fused, Pipeline>(cqe, UrKind::UnixAccept); break;
2252:                    on_recv<true, kEp, Fused, Pipeline>(
2256:                    on_tls_recv<kEp, Fused, Pipeline>(
2261:                                      Fused && Pipeline == 1>(cqe); break;
2264:                                    Fused && Pipeline == 1>(cqe); break;
2266:                    on_tls_socket_poll<kEp, Fused, Pipeline>(
2269:                    on_tls_socket_poll<kEp, Fused, Pipeline>(
2274:                    if constexpr (Fused)
2348:    template <bool kEp, bool Fused = false, uint8_t Pipeline = 0>
2368:        admit_fd<kEp, Fused, Pipeline>(cqe->res, kind);
2375:    template <bool kEp, bool Fused = false, uint8_t Pipeline = 0>
2390:            admit_fd<kEp, Fused, Pipeline>(fd, kind);
2397:    template <bool kEp, bool Fused = false, uint8_t Pipeline = 0>
2455:            if (target == self_->id()) adopt_client<kEp, Fused, Pipeline>(c, true);
2460:            adopt_client<kEp, Fused, Pipeline>(c, false, tls_socket);
3209:    template <bool kEp, bool Fused = false, uint8_t Pipeline = 0>
3260:                (void)drive_tls<kEp, Fused, Pipeline>(c);
3263:                    arm_tls_recv<kEp, Fused, Pipeline>(c);
3265:                arm_tls_recv<kEp, Fused, Pipeline>(c);
3271:        mark_active_known<Fused && Pipeline != 0>(c);
3286:    template <bool HasTls, bool kEp, bool Fused = false, uint8_t Pipeline = 0>
3314:                    parse_and_dispatch<true, Fused ? kGenthreadIfidBatchOps : 0>(c);
3316:                    parse_and_dispatch<false, Fused ? kGenthreadIfidBatchOps : 0>(c);
3318:                parse_and_dispatch<false, Fused ? kGenthreadIfidBatchOps : 0>(c);
3323:        mark_active_known<Fused && Pipeline != 0>(c);
3326:    template <bool kEp, bool Fused = false, uint8_t Pipeline = 0>
3357:            (void)wb_.pump_tls<kEp, Fused && Pipeline == 1>(*c, *tls);
3365:            (void)wb_.pump_tls<kEp, Fused && Pipeline == 1>(*c, *tls);
3377:            (void)wb_.pump_tls<kEp, Fused && Pipeline == 1>(
3395:            if constexpr (Fused && Pipeline == 2)
3408:                    (void)wb_.pump_tls<kEp, Fused && Pipeline == 1>(*c, *tls);
3420:                else (void)wb_.pump_tls<kEp, Fused && Pipeline == 1>(*c, *tls);
3423:                (void)wb_.pump_tls<kEp, Fused && Pipeline == 1>(*c, *tls);
3431:                (void)wb_.pump_tls<kEp, Fused && Pipeline == 1>(*c, *tls);
3439:                parse_and_dispatch<true, Fused ? kGenthreadIfidBatchOps : 0>(c);
3444:    template <bool kEp, bool Fused = false, uint8_t Pipeline = 0>
3452:        (void)drive_tls<kEp, Fused, Pipeline>(c);
3453:        mark_active_known<Fused && Pipeline != 0>(c);
3456:    template <bool kEp, bool Fused = false, uint8_t Pipeline = 0>
3476:        (void)drive_tls<kEp, Fused, Pipeline>(c);
3477:        mark_active_known<Fused && Pipeline != 0>(c);
3920:        static constexpr bool Fused =
3925:            Fused && __builtin_expect(srv_->read_local_enabled(), false);
3992:        if constexpr (Fused)
4010:            if constexpr (Fused)
4013:            if constexpr (Fused) {
4022:                Fused, ReadLocalDemotionPlan, EmptyReadLocalDemotionPlan>;
4158:            if constexpr (Fused) {
4200:            if constexpr (Fused) {
4487:                                    // Fused parser, whose IFID is that walk; pending_ifid_ is
4556:            if constexpr (Fused) {
4585:            if constexpr (Fused) {
4598:                if constexpr (Fused) {
4960:            if constexpr (Fused)
4972:                if constexpr (Fused)
5058:                    if constexpr (Fused)
5173:            if constexpr (Fused) {
5257:            if constexpr (Fused) {
5274:            if constexpr (Fused) {
5488:    template <bool HasUnix, bool HasTls, bool kEp, bool Fused = false>
5492:        if constexpr (Fused) {
6651:    template <bool HasTls, bool kEp, bool Fused = false, bool HasUnix = false,
6678:                    if (!c->closing() && !c->recv_armed()) (void)drive_tls<kEp, Fused>(c);
6744:                        if (tls) arm_tls_recv<kEp, Fused>(c);
6750:                        (void)drive_tls<kEp, Fused>(c);
6774:                                true, Fused ? kGenthreadIfidBatchOps : 0>(c);
6777:                                false, Fused ? kGenthreadIfidBatchOps : 0>(c);
6780:                            false, Fused ? kGenthreadIfidBatchOps : 0>(c);
6787:                                true, Fused ? kGenthreadIfidBatchOps : 0>(c);
6790:                                false, Fused ? kGenthreadIfidBatchOps : 0>(c);
6793:                            false, Fused ? kGenthreadIfidBatchOps : 0>(c);
6803:                    if (tls && tls->memory_bio()) arm_tls_recv<kEp, Fused>(c);
6853:        if constexpr (Fused) {
6882:        constexpr uint32_t serve_budget = Fused ? kGenthreadWbBatchConns : kServeBudget;
6902:                    if (wb_.serve_tls<kEp, false, Fused>(*c, *tls)) work++;
6907:                    if (wb_.serve_ktls<kEp, false, Fused>(*c)) work++;
6908:                } else if (wb_.serve<kEp, false, Fused>(*c)) {
6911:            } else if (wb_.serve<kEp, false, Fused>(*c)) {
7146:        if (srv_->read_local_enabled()) {
7420:    FusedExLoop* fused_executor_ = nullptr;
rg: src/core/op.h: No such file or directory (os error 2)
rg: src/core/client.h: No such file or directory (os error 2)
rg: src/core/rob.h: No such file or directory (os error 2)
rg: src/core/atomic.h: No such file or directory (os error 2)
src/store/flatstore.h:465:static_assert(sizeof(ExpireIndex) == 80, "ExpireIndex layout drift");
src/store/flatstore.h:569:static_assert(offsetof(ReadLocalStoreState, atomic) == 0);
src/store/flatstore.h:3866:    static constexpr size_t reader_first = offsetof(FlatStore, atomic_pending_);
src/store/flatstore.h:3867:    static constexpr size_t reader_last  = offsetof(FlatStore, read_local_atomic_filter_);
src/store/flatstore.h:3869:    static constexpr size_t owner_first  = offsetof(FlatStore, live_);
src/store/flatstore.h:3870:    static constexpr size_t owner_last   = offsetof(FlatStore, borrow_tombs_) + 3;
src/store/flatstore.h:3874:    static constexpr size_t atomic_owner_first = offsetof(FlatStore, atomic_version_bytes_);
src/store/flatstore.h:3876:        offsetof(FlatStore, atomic_read_origin_conn_id_) + 7;
src/store/flatstore.h:3878:    static constexpr size_t atomic_separator_first = offsetof(FlatStore, atomic_ticket_fn_);
src/store/flatstore.h:3912:static_assert(sizeof(FlatStore) == 944);
src/core/ex_loop.h:118:        static_assert(sizeof(LaneEntry) == 16);
src/core/ex_loop.h:3450:static_assert(sizeof(ExLoop) == 6112);
src/core/config.h:429:static_assert(sizeof(Config) == 624,
src/core/shard.h:518:    static constexpr size_t store_offset = offsetof(Shard, store_);
src/core/shard.h:519:    static constexpr size_t stats_offset = offsetof(Shard, stats_);
src/core/shard.h:523:static_assert(sizeof(Shard) == 1440);
src/core/thread.h:92:static_assert(sizeof(Task) == 32, "Task grew: task bytes multiply across the whole SPSC mesh");
src/core/thread.h:93:static_assert(offsetof(Task, enqueue_us_low) == 20,
src/core/thread.h:306:static_assert(sizeof(ReadLocalThreadState) == 368,
src/core/thread.h:1206:static_assert(sizeof(ThreadCtx) == 1408,
src/core/server.h:151:static_assert(sizeof(AtomicApplySlot) == 64);

exec
/bin/bash -lc "nl -ba src/core/ex_loop.h | sed -n '36,259p'
nl -ba src/core/ex_loop.h | sed -n '707,813p'
nl -ba src/core/ex_loop.h | sed -n '1908,1958p'
nl -ba src/core/ex_loop.h | tail -45" in /home/user/Projects/cx-rl2s
 succeeded in 0ms:
    36	#include "../persist/aof.h"
    37	
    38	namespace tomo {
    39	
    40	// "Long enough to cover an inter-arrival gap, short enough not to burn a core." A starting point to
    41	// measure, not a result.
    42	inline constexpr uint32_t kExSpinBudget = 2048;
    43	
    44	// How many ops are gathered before executing, so their storage prefetches can overlap. Large enough
    45	// that the prefetches have time to land, small enough that the batch stays in L1.
    46	inline constexpr uint32_t kExecBatch = kGenthreadExBatchOps;
    47	inline constexpr uint32_t kReadLocalPrefetchKeys = 32;
    48	inline constexpr uint32_t kActiveExpireChecks = 20;
    49	// The armed coarse rotation alternates these two equal quanta. One owner quantum per captured
    50	// producer bounds the stretch before WB by active producers rather than by queued depth, while the
    51	// local quantum matches the most one parse invocation can append for a connection.
    52	inline constexpr uint32_t kReadLocalDrainChunkOps = kGenthreadIfidBatchOps;
    53	inline constexpr uint32_t kReadLocalOwnerTaskChunkOps = kGenthreadExBatchOps;
    54	inline constexpr uint32_t kReadLocalMaxChunksBetweenOwnerBatches = 1;
    55	// Parser-side demotion may reserve one additional point command behind the entire local run.
    56	// Leave that credit outside the pending-read fanout budget so the combined reservation can always
    57	// fit an empty producer lane and therefore cannot retry forever.
    58	inline constexpr uint32_t kReadLocalDemotionBudget = kInboxSlots - 1;
    59	// Lane ADMISSION pressure window (P128.md). A LANE-FULL deferral arms this many fused rotations
    60	// during which the parser bounds every connection to kInboxSlots / active connections in flight.
    61	// It decays by one per rotation and is re-armed only by the next lane-full event -- never by the
    62	// quota's own deferrals (the actuator must not police itself) -- so under sustained oversubscription
    63	// one rotation in (this + 1) lets the lane prove the pressure again, and once aggregate demand has
    64	// stayed under the lane for this many rotations the bound is UINT32_MAX and the steady state pays
    65	// one predicted byte test per parse pass and nothing per op.
    66	inline constexpr uint8_t kReadLocalLanePressureRotations = 8;
    67	inline constexpr uint32_t kExSchedClasses =
    68	    static_cast<uint32_t>(CommandLengthClass::Count);
    69	inline constexpr uint32_t kExSchedBuckets = kRobWindow * kExSchedClasses;
    70	inline constexpr uint32_t kExSchedBucketWords = (kExSchedBuckets + 63) / 64;
    71	static_assert(kExecBatch <= UINT8_MAX);
    72	static_assert(kExecBatch <= 32);
    73	static_assert(kGenthreadIfidBatchOps <= kExecBatch);
    74	static_assert(kReadLocalDrainChunkOps == kExecBatch);
    75	static_assert(kReadLocalOwnerTaskChunkOps == kExecBatch);
    76	static_assert(kReadLocalMaxChunksBetweenOwnerBatches > 0);
    77	static_assert((kExecBatch & (kExecBatch - 1)) == 0);
    78	static_assert(kExSchedBuckets == 192);
    79	
    80	// Constructed only for an armed declared-key-precise write whose owner has since enabled eviction.
    81	// Keeping the existing maxmemory admission active but forcing NoEviction makes the IO-side promise
    82	// self-fulfilling: this operation can update its declared point/keyset or fail, but cannot delete a
    83	// different key behind a younger local read. The owner restores its live policy before the next
    84	// task.
    85	class ReadLocalPreciseWriteGuard {
    86	public:
    87	    explicit ReadLocalPreciseWriteGuard(FlatStore& store) : store_(&store) {
    88	        previous_ = store_->script_suspend_eviction();
    89	    }
    90	    ~ReadLocalPreciseWriteGuard() { store_->script_restore_eviction(previous_); }
    91	    ReadLocalPreciseWriteGuard(const ReadLocalPreciseWriteGuard&) = delete;
    92	    ReadLocalPreciseWriteGuard& operator=(const ReadLocalPreciseWriteGuard&) = delete;
    93	
    94	private:
    95	    FlatStore* store_ = nullptr;
    96	    MaxmemoryPolicy previous_ = MaxmemoryPolicy::NoEviction;
    97	};
    98	
    99	template <bool Enabled>
   100	struct ReadLocalExState;
   101	
   102	template <>
   103	struct ReadLocalExState<false> {};
   104	
   105	template <>
   106	struct ReadLocalExState<true> {
   107	    struct Impl {
   108	        using DemoteFn = bool (*)(void*, Client*, const uint64_t*,
   109	                                  const ReadLocalFallbackReason*, uint32_t, uint32_t&);
   110	
   111	        // A lane entry is the ROB handle only. The full 32-byte Task carried a shard and a
   112	        // scatter pointer that were always -1/nullptr here; at kInboxSlots entries that doubled
   113	        // the lane to the size of the whole L1D. Demotion builds its owner Tasks from the id.
   114	        struct LaneEntry {
   115	            Client* client;
   116	            uint64_t op_id;
   117	        };
   118	        static_assert(sizeof(LaneEntry) == 16);
   119	
   120	        std::unique_ptr<LaneEntry[]> lane;
   121	        // A failed probe can sit behind an earlier bounded demotion wave. Preserve its original
   122	        // reason until that exact ROB op is lowered instead of reclassifying the suffix as Context.
   123	        std::unique_ptr<ReadLocalFallbackReason[]> lane_fallbacks;
   124	        uint32_t lane_head = 0;
   125	        uint32_t lane_tail = 0;
   126	        uint32_t lane_count = 0;
   127	        // Upper-bound owner-task demand for every still-local lane entry. GET contributes one;
   128	        // MGET contributes min(key count, shard count), which bounds its touched-shard fanout.
   129	        // Keeping the sum within the demotion budget makes every task-capacity plan reservable
   130	        // (snapshot pressure may still split it into waves).
   131	        uint32_t lane_demotion_demand = 0;
   132	        // Rotations left in the lane-admission pressure window; see kReadLocalLanePressureRotations.
   133	        uint8_t lane_pressure = 0;
   134	        bool lane_has_tombstones = false;
   135	        bool point_writes_precise = true;
   136	        bool keymiss_notify_armed = false;
   137	        bool interleave_owner_tasks = true;
   138	        bool prefetch_capture = true;
   139	        // EFFECTIVE lane capacity for ADMISSION. The physical ring is always kInboxSlots entries
   140	        // and its index arithmetic still masks with kInboxSlots - 1; this only decides when the
   141	        // parser stops admitting, so lowering it can never overrun the ring. It derives to
   142	        // kInboxSlots and stays there unless DEBUG READ-LOCAL-LANE-CAP sets a test value, which
   143	        // fused_pass_impl copies here once per rotation. It occupies the two padding bytes that
   144	        // already sat at offset 38 between prefetch_capture and demote_context, on the same first
   145	        // cache line as lane_head/lane_tail/lane_count that every admission test already reads, so
   146	        // the struct does not grow, nothing moves, and reading it costs no line the caller did not
   147	        // already own. See P128.md section 8.
   148	        uint16_t lane_admit_cap = static_cast<uint16_t>(kInboxSlots);
   149	        void* demote_context = nullptr;
   150	        DemoteFn demote = nullptr;
   151	        ReadLocalDeferredQueue deferred;
   152	
   153	        void rebind_owned_shards(ThreadCtx& owner) {
   154	            ReadLocalRetireSink* sink = deferred.sink();
   155	            if (!sink) std::abort();
   156	            for (Shard* shard : owner.shards())
   157	                shard->store().rebind_read_local_retire_sink(*sink);
   158	        }
   159	    };
   160	
   161	    std::unique_ptr<Impl> impl;
   162	};
   163	
   164	using ReadLocalExImpl = ReadLocalExState<true>::Impl;
   165	static_assert(std::is_empty_v<ReadLocalExState<false>>);
   166	
   167	template <bool Enabled, uint32_t Capacity>
   168	struct ReadLocalCaptureBuffer {};
   169	
   170	template <uint32_t Capacity>
   171	struct ReadLocalCaptureBuffer<true, Capacity> {
   172	    FlatStore::ReadLocalPrefetchCapture entries[Capacity];
   173	};
   174	
   175	template <bool Fused>
   176	class ExLoopT {
   177	public:
   178	    using FusedCompletionFn = void (*)(void*, Client*);
   179	
   180	    WbEngine& engine() { return wb_; }
   181	    bool init(Server* srv, ThreadCtx* self, bool dormant = false) {
   182	        srv_ = srv; self_ = self;
   183	        aof_manager_ = srv->aof().configured() ? &srv->aof() : nullptr;
   184	        lru_clock_shift_ = static_cast<uint8_t>(srv->cfg().lru_clock_shift);
   185	        foreign_touch_random_ ^= (static_cast<uint64_t>(self->id()) + 1) * 0x9e3779b97f4a7c15ULL;
   186	        lb_sample_rate_ = srv->key_lb_signals_enabled() ? srv->cfg().lb_sample_rate : 0;
   187	        lb_sample_countdown_ = lb_sample_rate_;
   188	        lb_controller_armed_ = srv->key_lb_signals_enabled();
   189	        age_sample_rate_cached_ = srv->effective_age_sample_rate();
   190	        ex_sched_enabled_ = srv->cfg().ex_sched != 0;
   191	        pipeline_batches_ = Fused && srv->cfg().overlap != 0;
   192	        // Both interwoven schedules use the proven iofused fixed producer lanes.  The legacy
   193	        // streams implementation remains in this file for branch comparison, but overlap 2 no
   194	        // longer reaches its reservation-aware task transport.
   195	        iofused_ = Fused && srv->cfg().overlap != 0;
   196	        if constexpr (Fused) {
   197	            if (srv->read_local_enabled()) {
   198	                std::unique_ptr<ReadLocalExImpl> impl(new (std::nothrow) ReadLocalExImpl);
   199	                if (!impl) return false;
   200	                impl->lane.reset(new (std::nothrow) ReadLocalExImpl::LaneEntry[kInboxSlots]);
   201	                impl->lane_fallbacks.reset(
   202	                    new (std::nothrow) ReadLocalFallbackReason[kInboxSlots]);
   203	                if (!impl->lane || !impl->lane_fallbacks ||
   204	                    !impl->deferred.init(srv, self)) return false;
   205	                impl->point_writes_precise =
   206	                    srv->cfg().maxmemory == 0 ||
   207	                    srv->cfg().maxmemory_policy == MaxmemoryPolicy::NoEviction;
   208	                impl->interleave_owner_tasks = srv->cfg().read_local_interleave != 0;
   209	                impl->prefetch_capture = srv->cfg().read_local_prefetch_capture != 0;
   210	                read_local_.impl = std::move(impl);
   211	            }
   212	        }
   213	        if (!ring_.init(1024)) return false;
   214	        fused_handoff_ring_ = &ring_;
   215	        wb_.bind(&ring_);
   216	        initialized_ = true;
   217	        if (!dormant) activate();
   218	        return true;
   219	    }
   220	
   221	    void activate() {
   222	        if (!initialized_) std::abort();
   223	        self_->set_ring(&ring_);
   224	        self_->set_wb_engine(nullptr);
   225	        blocking_bind_executor(srv_, self_, &ring_);
   226	        for (Shard* shard : self_->shards())
   227	            shard->bind_notify_pending(&notify_keyless_pending_);
   228	    }
   229	
   230	    // Fused tenure shares the physical thread with IoLoop. IoLoop remains the published wake
   231	    // endpoint because it owns the only blocking wait; this ring still carries persistence CQEs.
   232	    void activate_fused(Ring* handoff_ring) {
   233	        static_assert(Fused);
   234	        if (!initialized_) std::abort();
   235	        // Interwoven schedules put executor-originated task/client handoffs on the network ring.
   236	        // That leaves this private ring with control/persistence SQEs; both iofused-family arms
   237	        // amortize at their shared N2 boundary. Pipeline 0 retains its existing ring ownership.
   238	        if (srv_->cfg().overlap != 0 && handoff_ring)
   239	            fused_handoff_ring_ = handoff_ring;
   240	        blocking_bind_executor(srv_, self_, &ring_);
   241	        if (read_local_enabled())
   242	            self_->bind_read_local_retire_sink(*read_local_impl().deferred.sink());
   243	        for (Shard* shard : self_->shards()) {
   244	            shard->bind_notify_pending(&notify_keyless_pending_);
   245	            if (read_local_enabled())
   246	                shard->store().configure_read_local(
   247	                    true, *read_local_impl().deferred.sink(),
   248	                    srv_->cfg().read_local_atomic_filter != 0);
   249	        }
   250	    }
   251	
   252	    void bind_fused_completion(void* context, FusedCompletionFn completion) {
   253	        static_assert(Fused);
   254	        fused_io_context_ = context;
   255	        fused_completion_ = completion;
   256	    }
   257	
   258	    void bind_read_local_demotion(void* context, ReadLocalExImpl::DemoteFn demote) {
   259	        static_assert(Fused);
   707	    void run() {
   708	        if constexpr (Fused) {
   709	            // Only RL2S instantiates the owner loop with the fused-capable executor. Its lane
   710	            // was drained before role conversion; owner commands use no local-read captures.
   711	            if (!read_local_enabled() || read_local_impl().lane_count != 0) std::abort();
   712	            self_->publish_read_local_parked(srv_->read_local_epoch());
   713	            // A preceding shard-less IO tenure may have consumed this CONFIG version without
   714	            // applying it to shards. Reapply once ownership is installed and dispatch resumes.
   715	            live_config_version_ = UINT64_MAX;
   716	        }
   717	        LoopSignals& sig = self_->sig();
   718	        uint32_t idle_spins = 0;
   719	
   720	        while (!self_->stop_flag().load(std::memory_order_relaxed) &&
   721	               self_->role() == Role::Ex) {
   722	#ifdef TOMO_RL_CACHE_DEBUG
   723	            if constexpr (Fused)
   724	                srv_->debug_assert_read_local_sinks_follow_ownership(self_->id());
   725	#endif
   726	            cached_now_ms_ = realtime_ms();
   727	            const bool flip_frozen = srv_->flip_stage() >= FlipStage::ExDrain;
   728	            const bool lb_frozen = lb_controller_armed_ && srv_->lb_dispatch_paused();
   729	            const bool placement_frozen = flip_frozen || lb_frozen;
   730	            // refresh_live_config() walks this owner's shard vector. The coordinator may rewrite
   731	            // those vectors after ExDrain acknowledgement, so a frozen executor must not even run
   732	            // the otherwise-cold configuration refresh path.
   733	            if (!placement_frozen) refresh_live_config();
   734	            if (maxmemory_enabled_)
   735	                cached_lru_clock_ = static_cast<uint8_t>(
   736	                    (static_cast<uint64_t>(cached_now_ms_ / 1000) >> lru_clock_shift_) & 0x1f);
   737	            sig.iterations++;
   738	
   739	            uint32_t did = 0;
   740	            uint64_t pass_ns = 0;
   741	            {
   742	                Span busy(pass_ns);
   743	                if (self_->sample_depth(busy.start_ns() / 1000)) {
   744	                    const uint32_t age_rate = srv_->effective_age_sample_rate();
   745	                    if (age_rate != age_sample_rate_cached_) {
   746	                        age_sample_rate_cached_ = age_rate;
   747	                        self_->sig().configure_age_sampling(age_rate);
   748	                    }
   749	                }
   750	                if (placement_frozen) {
   751	                    // Once ExDrain is acknowledged this loop is a hard safe point: no expiry,
   752	                    // cleanup, waiter walk, or task can reacquire a moved FlatStore before FLIP
   753	                    // publishes ExInstall. The coordinator may rewrite owner entries immediately
   754	                    // after observing the acknowledgement.
   755	                    const bool acknowledged = flip_frozen
   756	                        ? srv_->flip_acked(self_->id(), FlipStage::ExDrain)
   757	                        : srv_->lb_acked(self_->id());
   758	                    if (!acknowledged) {
   759	                        did += service_stale_forwards();
   760	                        did += drain_releases(true);
   761	                        did += service_multi_retries();
   762	                        did += service_atomic_deferred();
   763	                        did += service_xshard_retries();
   764	                        if (xshard_retries_.empty()) did += service_ordered_deferred();
   765	                        if (xshard_retries_.empty() && ordered_deferred_.empty())
   766	                            did += drain_tasks(true);
   767	                        flush_xshard_commits();
   768	                        did += aof_flush_pass();
   769	                        did += drain_notify_keyless(sig);
   770	                    }
   771	                    did += ring_.for_each_cqe([&](io_uring_cqe* cqe) { on_cqe(cqe); });
   772	                    did += flip_control_pass();
   773	                    did += lb_control_pass();
   774	                } else {
   775	                    did += snapshot_control_pass();
   776	                    did += service_stale_forwards();
   777	                    did += drain_releases();
   778	                    if (!snapshot_blocks_tasks()) {
   779	                        did += service_multi_retries();
   780	                        did += service_atomic_deferred();
   781	                        did += service_xshard_retries();
   782	                        if (xshard_retries_.empty()) did += service_ordered_deferred();
   783	                        if (xshard_retries_.empty() && ordered_deferred_.empty())
   784	                            did += snapshot_owner_state_ == SnapshotOwnerState::None
   785	                                       ? drain_tasks() : drain_tasks_snapshot();
   786	                    }
   787	                    flush_xshard_commits();
   788	                    if (__builtin_expect(srv_->blocking_waiters() != 0, false) &&
   789	                        cached_now_ms_ >= blocking_beat_ms_) {
   790	                        did += blocking_owner_cycle(
   791	                            *srv_, *self_, ring_, cached_now_ms_, true);
   792	                        blocking_beat_ms_ = cached_now_ms_ + 10;
   793	                    }
   794	                    did += aof_flush_pass();
   795	                    did += ring_.for_each_cqe([&](io_uring_cqe* cqe) { on_cqe(cqe); });
   796	                    did += flip_control_pass();
   797	                    did += lb_control_pass();
   798	                    lb_bucket_bytes_pass();
   799	                }
   800	                if constexpr (Fused)
   801	                    did += read_local_impl().deferred.drain_ready();
   802	            }
   803	            // A pass that found nothing -- every drain and control pass came back empty -- is
   804	            // polling, not work. Book it as idle so busy_ns means WORK: the FLIP placement model
   805	            // reads the roles' busy shares, and an executor spinning its 2048-pass budget between
   806	            // task batches would otherwise report the polling as demand (measured on 8-key
   807	            // MGET/MSET at 2:2 of 4: ex 97% "busy", three quarters of its passes empty; the model
   808	            // read io = 0.73 of a 0.47 workload). One local and one branch per pass.
   809	            if (did) sig.busy_ns += pass_ns; else sig.idle_ns += pass_ns;
   810	            sig.cpu_ns = thread_cpu_ns();
   811	
   812	            // Flush prepared SQEs before looping. Recv re-arms and cross-ring wakes are
   813	            // PREPARED during the work section but only reach the kernel on submit; taking
  1908	        lb_ack_wake_pending_ = true;
  1909	        lb_rebind_pending_ = true;   // membership may change before the stage ends; rebind after
  1910	        return wake_coordinator();
  1911	    }
  1912	
  1913	    template <bool OwnsShards = true>
  1914	    void refresh_live_config() {
  1915	        LiveConfigSnapshot snapshot;
  1916	        if (!srv_->live_config_snapshot_if_changed(live_config_version_, snapshot)) return;
  1917	        const bool enabled = snapshot.maxmemory != 0;
  1918	        if constexpr (OwnsShards) {
  1919	            const uint64_t shard_limit = snapshot.maxmemory / srv_->nshards();
  1920	            for (Shard* sh : self_->shards()) {
  1921	                sh->configure_maxmemory(enabled, shard_limit, snapshot.policy, snapshot.samples);
  1922	                // CLIENT TRACKING and periodic SAVE need the same per-write observation points as
  1923	                // keyspace notifications, so they ride the shard mask as synthetic observer bits.
  1924	                // notify_record expands those observers over NOTIFY_ALL without adding those class
  1925	                // bits here: the operator's configured pub/sub classes therefore remain independent.
  1926	                // NOTIFY_NEW and NOTIFY_KEY_MISS stay outside the observer surface: `new` would count
  1927	                // or invalidate a mutation twice, and a key miss is not a value change.
  1928	                sh->set_notify_mask(snapshot.notify_events |
  1929	                                    (snapshot.tracking_armed ? NOTIFY_TRACKING : 0u) |
  1930	                                    (snapshot.save_armed ? NOTIFY_SAVE : 0u));
  1931	            }
  1932	        }
  1933	        maxmemory_enabled_ = enabled;
  1934	        foreign_touch_policy_ =
  1935	            !enabled                                    ? kForeignTouchNone
  1936	            : maxmemory_policy_is_lru(snapshot.policy)  ? kForeignTouchLru
  1937	            : maxmemory_policy_is_lfu(snapshot.policy)  ? kForeignTouchLfu
  1938	                                                        : kForeignTouchNone;
  1939	        slowlog_arm_.slowlog_us = snapshot.slowlog_log_slower_than;
  1940	        slowlog_arm_.latency_ms = snapshot.latency_monitor_threshold;
  1941	        slowlog_armed_ = slowlog_arm_.armed();
  1942	        debug_fanout_defer_us_ = snapshot.debug_fanout_defer_us;
  1943	        live_config_version_ = snapshot.version;
  1944	    }
  1945	
  1946	    // The pending flag is a HINT, and a hint must never be the only looker: a shard whose binding
  1947	    // went stale (any ownership move a future path forgets to rebind) would strand its events
  1948	    // behind a flag it can no longer set. The busy path keeps the cheap flag gate; sweep() forces
  1949	    // the state walk, so stranded events survive at most until the owner's next idle pass.
  1950	    uint32_t drain_notify_keyless(LoopSignals& signals, bool force = false) {
  1951	        if (__builtin_expect(!notify_keyless_pending_, true) && !force) return 0;
  1952	        notify_keyless_pending_ = false;
  1953	        uint32_t work = 0;
  1954	        for (Shard* shard : self_->shards()) {
  1955	            work += notify_ex_pass_entry(
  1956	                *srv_, *shard, self_->id(), *self_, ring_, signals);
  1957	            notify_keyless_pending_ |= shard->notify_output_pending();
  1958	        }
  3408	    SlowlogExState slowlog_state_{};
  3409	    // Fused-only state stays at the true tail so every split ExLoop field keeps its offset.
  3410	    uint32_t fused_idle_spins_ = 0;
  3411	    uint32_t fused_non_submit_rotations_ = 0;
  3412	    void* fused_io_context_ = nullptr;
  3413	    FusedCompletionFn fused_completion_ = nullptr;
  3414	    Ring* fused_handoff_ring_ = nullptr;
  3415	    // Both interwoven schedules share the measured 128-task geometry and coalesced N2 boundary.
  3416	    bool pipeline_batches_ = false;
  3417	    bool iofused_ = false;
  3418	    // Consumes the bool padding this class already carried before notify_batch_n_. Ordinary
  3419	    // commands pay one predicted-true test at an executor boundary and never touch the TLS
  3420	    // commit list.
  3421	    bool xshard_commit_pending_ = false;
  3422	    // Per-batch completion notification (DESIGN-NOTIFY.md §2). While a batch is open, the slot
  3423	    // path of notify_sender records (io, slot) here instead of fencing and setting per op; the
  3424	    // batch end pays ONE seq_cst fence, one ReadyMask::set per recorded client run and one wake
  3425	    // decision per io that saw an empty->flagged edge. Sized to this loop's largest batch; the
  3426	    // record never holds more than one entry per executed task, and a full record flushes in
  3427	    // place regardless. (io, slot), never Client*: the flush may run a whole batch after the Done
  3428	    // store, and a connection is only guaranteed allocated until the io's second reap prologue
  3429	    // after close.
  3430	    struct NotifyEntry { uint32_t io; uint32_t slot; };
  3431	    static constexpr uint32_t kNotifyBatchMax =
  3432	        Fused ? kGenthreadPipelineExBatchOps : kGenthreadExBatchOps;
  3433	    bool notify_batch_open_ = false;
  3434	    uint32_t notify_batch_n_ = 0;
  3435	    NotifyEntry notify_batch_[kNotifyBatchMax] = {};
  3436	    [[no_unique_address]] ReadLocalExState<Fused> read_local_;
  3437	};
  3438	
  3439	using ExLoop = ExLoopT<false>;
  3440	template <> uint32_t ExLoopT<true>::split_read_local_pass();
  3441	using FusedExLoop = ExLoopT<true>;
  3442	
  3443	// Disabled split executors retain the exact pre-read-local allocation stride plus the 264-byte
  3444	// per-batch notification record (DESIGN-NOTIFY.md §2): 5848 + 8 + 32 * sizeof(NotifyEntry).
  3445	// 6104 -> 6112: read-local eviction accounting adds exactly ONE word, the per-thread LFU dice
  3446	// (foreign_touch_random_). Its two companions -- the latched policy byte and the fan-out defer
  3447	// hook -- went into padding the lb bool run already carried and cost nothing. This is a per-
  3448	// EXECUTOR object, one per thread, not a per-op or per-connection footprint: Op, Client,
  3449	// ThreadCtx, Shard and Config are the locks that may not move, and none of them did.
  3450	static_assert(sizeof(ExLoop) == 6112);
  3451	
  3452	}  // namespace tomo

exec
/bin/bash -lc "nl -ba src/core/io_loop.h | sed -n '70,177p'
nl -ba src/core/io_loop.h | sed -n '458,557p'
nl -ba src/core/io_loop.h | sed -n '3913,4007p'
nl -ba src/core/io_loop.h | sed -n '7127,7166p'
rg -n 'refresh_notify_config|void run\\(' src/core/io_loop.h
nl -ba src/core/fused_boot_gate.h | sed -n '1,70p'" in /home/user/Projects/cx-rl2s
 succeeded in 0ms:
    70	    WbEngine& engine() { return wb_; }
    71	    uint32_t reap_atomic_deferred() {
    72	        return scatter_pool_.reap_deferred() + multi_owner_reap_entry(*this);
    73	    }
    74	
    75	    // ONE LISTENING SOCKET PER IO THREAD, via SO_REUSEPORT.
    76	    //
    77	    // Sharing a single listen fd across io threads does NOT distribute connections: every thread
    78	    // arms a multishot accept on it and the kernel satisfies them all from one ring. Measured
    79	    // consequence with 6 io threads and 577 connections — t5 took every single one and t0..t4 sat
    80	    // idle for the entire run, so the server was really running on one io thread. It looked like a
    81	    // latency problem (uniform ~3.5 ms at p1) and was actually a distribution problem.
    82	    //
    83	    // With SO_REUSEPORT the kernel hashes each incoming connection to one of the listening sockets,
    84	    // which spreads them across threads without any userspace handoff. Note this is safe WITHIN one
    85	    // process; two SERVER PROCESSES sharing a port is the failure mode that once faked data loss,
    86	    // so a boot must still verify nothing else holds the port.
    87	    bool init(Server* srv, ThreadCtx* self, const char* addr, uint16_t port,
    88	              int unix_listen_fd = -1, const TlsContext* tls_context = nullptr,
    89	              bool dormant = false) {
    90	        srv_ = srv; self_ = self;
    91	        (void)addr;
    92	        configured_port_ = port;
    93	        tls_context_ = tls_context;
    94	        unix_listen_fd_ = unix_listen_fd;
    95	        epoll_ = srv_->cfg().net_io == NetIoEngine::Epoll;
    96	        targeted_ifid_ = srv_->cfg().thread_mode == ThreadMode::Fused &&
    97	                         srv_->cfg().overlap != 0;
    98	        age_sample_rate_cached_ = srv_->effective_age_sample_rate();
    99	        age_signals_armed_ = age_sample_rate_cached_ != 0;
   100	        client_lb_signal_armed_ = srv_->client_lb_signals_enabled();
   101	        lb_controller_armed_ = srv_->lb_controller_enabled();
   102	        if (!ring_.init(4096)) return false;
   103	        if (epoll_ && !init_epoll()) return false;
   104	        wb_.bind(&ring_, this, [](void* ctx, int32_t shard, const char* ptr) {
   105	            static_cast<IoLoop*>(ctx)->queue_borrow_release(shard, ptr);
   106	        }, this, [](void* ctx, Client& client, Op& op) {
   107	            auto* loop = static_cast<IoLoop*>(ctx);
   108	            const bool was_blocked = client.blocked();
   109	            NotifyBatch* notifications = nullptr;
   110	            if (op.has_scatter_state()) {
   111	                notifications = notify_take_batch(op);
   112	                xshard_retire(*loop->srv_, *loop->self_, loop->ring_, client, op,
   113	                    loop->scatter_pool_, loop->self_->id(), loop,
   114	                    [](void* release_ctx, int32_t shard, const char* ptr) {
   115	                        static_cast<IoLoop*>(release_ctx)->queue_borrow_release(shard, ptr);
   116	                    }, [](void* release_ctx) {
   117	                        static_cast<IoLoop*>(release_ctx)->wb_.note_zc_suppressed_tls();
   118	                    });
   119	            } else if (op.has_blocking_state()) {
   120	                notifications = notify_take_batch(op);
   121	                blocking_retire(*loop->srv_, client, op, *loop->self_);
   122	            } else if (op.has_multi_state()) {
   123	                notifications = notify_take_batch(op);
   124	                multi_retire_entry(*loop, client, op);
   125	            } else if (__builtin_expect(op.has_notify_state(), false)) {
   126	                notifications = notify_take_batch(op);
   127	            }
   128	            if (__builtin_expect(notifications != nullptr, false))
   129	                notify_retire_batch_entry(*loop, notifications, client.id());
   130	            if (was_blocked && !client.blocked()) {
   131	                // A blocking wait is timeout-exempt, so its parked duration cannot become the
   132	                // idle age the first cron pass sees after retirement. The socket send refresh is
   133	                // too late: cron runs later in this same IO pass, before its CQE can arrive.
   134	                client.set_last_interaction_s(loop->cached_now_s_);
   135	                // One-shot test synchronizer: make that vulnerable retirement -> cron ordering
   136	                // deterministic. Production never arms it, and the blocking path is already cold.
   137	                if (loop->srv_->debug_blocking_timeout_reap_take())
   138	                    loop->client_cron_beat_ms_ = 0;
   139	            }
   140	        }, srv_->client_obuf_armed_ptr(), this, [](void* ctx, Client& client) {
   141	            return static_cast<IoLoop*>(ctx)->client_obuf_check(&client, true);
   142	        }, &cached_now_s_, &self_->sig());
   143	        wb_.set_cold_send_classification(
   144	            srv_->cfg().thread_mode == ThreadMode::Fused &&
   145	            srv_->cfg().overlap != 0);
   146	        initialized_ = true;
   147	        return dormant || activate();
   148	    }
   149	
   150	    ~IoLoop() {
   151	        if (self_) pubsub_shutdown_events();
   152	        if (listen_fd_ >= 0) ::close(listen_fd_);
   153	        if (tls_listen_fd_ >= 0) ::close(tls_listen_fd_);
   154	        if (unix_listen_fd_ >= 0) ::close(unix_listen_fd_);
   155	        for (Client* c : pending_handoffs_) {
   156	            ::close(c->fd());
   157	            srv_->client_released();
   158	            delete c;
   159	        }
   160	        multi_shutdown_entry(*this);
   161	    }
   162	
   163	    static int make_reuseport_listener(const char* addr, uint16_t port, uint32_t backlog = 511,
   164	                                       bool defer_accept = false) {
   165	        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
   166	        if (fd < 0) return -1;
   167	        int one = 1;
   168	        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
   169	        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
   170	        if (defer_accept) setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &one, sizeof(one));
   171	        sockaddr_in sa{};
   172	        sa.sin_family = AF_INET;
   173	        sa.sin_port   = htons(port);
   174	        if (::inet_pton(AF_INET, addr, &sa.sin_addr) != 1) { ::close(fd); return -1; }
   175	        if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) { ::close(fd); return -1; }
   176	        if (::listen(fd, static_cast<int>(backlog)) != 0) { ::close(fd); return -1; }
   177	        return fd;
   458	    }
   459	
   460	    enum class DispatchResult : uint8_t {
   461	        Progress,
   462	        NeedInput,
   463	        Error,
   464	        Closed,
   465	    };
   466	
   467	    struct IfidPipelineEntry {
   468	        Client* client = nullptr;
   469	        Op* op = nullptr;
   470	        uint64_t op_id = 0;
   471	        uint32_t consumed = 0;
   472	        uint32_t worker = 0;
   473	        bool direct_candidate = false;
   474	    };
   475	
   476	    struct IfidPipelineBatch {
   477	        std::array<IfidPipelineEntry, kGenthreadPipelineIfidBatchOps> entries{};
   478	        std::array<uint32_t, kMaxThreads> reserved_workers{};
   479	        std::array<uint32_t, kMaxThreads> reserved_counts{};
   480	        uint32_t count = 0;
   481	        uint32_t reserved_worker_count = 0;
   482	        bool reservation_ready = false;
   483	        bool force_coarse = false;
   484	        bool defer_parse_advance = false;
   485	        bool targeted_ready = false;
   486	    };
   487	
   488	    struct WbPipelineBatch {
   489	        std::array<Client*, kGenthreadPipelineWbBatchConns> clients{};
   490	        uint32_t count = 0;
   491	    };
   492	
   493	    struct ClientMigration {
   494	        Client* client = nullptr;
   495	        uint32_t destination = 0;
   496	        bool cancel_submitted = false;
   497	        bool hold_for_commit = false;
   498	        bool prepared = false;
   499	        bool destination_registered = false;
   500	        int source_backup_fd = -1;
   501	        void* catalog = nullptr;
   502	        void* routing = nullptr;
   503	    };
   504	
   505	    friend bool multi_dispatch_entry(IoLoop&, Client&, Op&, uint32_t);
   506	    friend bool multi_dispatch_entry_iofused(IoLoop&, Client&, Op&, uint32_t);
   507	    template <bool IofusedPrivateQueue>
   508	    friend bool multi_dispatch_entry_impl(IoLoop&, Client&, Op&, uint32_t);
   509	    friend bool auth_dispatch_entry(IoLoop&, Client&, Op&, uint32_t);
   510	    friend bool acl_dispatch_entry(IoLoop&, Client&, Op&, uint32_t, uint8_t);
   511	    friend bool acl_finish_dispatch_denial(IoLoop&, Client&, Op&, uint32_t,
   512	                                           AclDeniedReason, uint32_t);
   513	    friend void acl_command_entry(IoLoop&, Client&, Op&);
   514	    friend void acl_broadcast_user_change(IoLoop&, uint32_t, const AclPerm*, bool);
   515	    friend void multi_retire_entry(IoLoop&, Client&, Op&);
   516	    friend uint32_t multi_owner_pass_entry(IoLoop&);
   517	    friend uint32_t multi_owner_pass_entry_iofused(IoLoop&);
   518	    template <bool IofusedPrivateQueue>
   519	    friend uint32_t multi_owner_pass_entry_impl(IoLoop&);
   520	    friend uint32_t multi_owner_reap_entry(IoLoop&);
   521	    friend void multi_close_entry(IoLoop&, Client&);
   522	    friend void multi_shutdown_entry(IoLoop&);
   523	    friend void notify_retire_batch_entry(IoLoop&, NotifyBatch*, uint64_t);
   524	#include "pubsub.inc"
   525	
   526	    template <bool HasUnix, bool HasTls, bool kEp, bool Fused = false,
   527	              uint8_t Pipeline = 0>
   528	    void run_loop() {
   529	        static_assert(Pipeline <= 2);
   530	        if constexpr (Fused && Pipeline == 1) {
   531	            run_fused_iofused_loop<HasUnix, HasTls, kEp, false>();
   532	            return;
   533	        }
   534	        if constexpr (Fused && Pipeline == 2) {
   535	            // Overlap 2 is the gated three-way extension of iofused.  The old streams loop remains
   536	            // below for branch comparison, but no boot-time dispatch can reach it.
   537	            run_fused_iofused_loop<HasUnix, HasTls, kEp, true>();
   538	            return;
   539	        }
   540	        constexpr bool IoPipe = !Fused && Pipeline == 1;
   541	        if constexpr (Fused) {
   542	            if (srv_->read_local_enabled()) {
   543	                // A split EX tenure is permanently parked: it never probes the local lane.
   544	                // Resume BEFORE sampling the epoch, as at the existing network-wait boundary.
   545	                if (ThreadCtx::read_local_publication_parked(self_->read_local_publication()))
   546	                    self_->resume_read_local_tick();
   547	                self_->publish_read_local_tick(srv_->read_local_epoch());
   548	                self_->set_read_local_lane_active(true);
   549	            }
   550	        }
   551	        if constexpr (!kEp) {
   552	            if (listen_fd_ >= 0) arm_accept(UrKind::Accept);
   553	            if constexpr (HasTls) arm_accept(UrKind::TlsAccept);
   554	            if constexpr (HasUnix) if (unix_listen_fd_ >= 0) arm_accept(UrKind::UnixAccept);
   555	        }
   556	        LoopSignals& sig = self_->sig();
   557	        while (!self_->stop_flag().load(std::memory_order_relaxed) &&
  3913	    // ---- parse -> route -> publish -----------------------------------------------------------------
  3914	    template <bool NoBorrow, uint32_t BatchOps = 0, bool IoPipe = false,
  3915	              bool BufferedIfid = false, bool TargetedIfid = false,
  3916	              bool SuppressOrdinaryActiveMark = false,
  3917	              bool IofusedPrivateQueue = false>
  3918	    DispatchResult parse_and_dispatch(
  3919	        Client* c, IfidPipelineBatch* pipeline_batch = nullptr) {
  3920	        static constexpr bool Fused =
  3921	            BatchOps == kGenthreadIfidBatchOps &&
  3922	            !IoPipe && !BufferedIfid && !TargetedIfid &&
  3923	            !SuppressOrdinaryActiveMark && !IofusedPrivateQueue;
  3924	        [[maybe_unused]] const bool read_local_enabled =
  3925	            Fused && __builtin_expect(srv_->read_local_enabled(), false);
  3926	        Client& conn = *c;
  3927	        Rob<kRobWindow>& rob = c->rob();
  3928	        LoopSignals& sig = self_->sig();
  3929	        const uint32_t pass_rpos = conn.rpos();
  3930	        const char* const pass_rbuf = conn.rbuf();
  3931	        const uint32_t pass_rlen = conn.rlen();
  3932	        const uint32_t self_id = self_->id();
  3933	        auto task_free_slots = [&](ThreadCtx& owner) {
  3934	            if constexpr (IofusedPrivateQueue) {
  3935	                return owner.iofused_task_free_slots(self_id);
  3936	            } else {
  3937	                return owner.task_free_slots(self_id);
  3938	            }
  3939	        };
  3940	        auto post_task_quiet = [&](ThreadCtx& owner, const Task& task) {
  3941	            if constexpr (IofusedPrivateQueue) {
  3942	                return owner.post_iofused_task_quiet(self_id, task, sig);
  3943	            } else {
  3944	                return owner.post_task_quiet(self_id, task, sig);
  3945	            }
  3946	        };
  3947	        auto post_tasks_quiet = [&](ThreadCtx& owner, const Task* tasks, uint32_t count) {
  3948	            if constexpr (IofusedPrivateQueue) {
  3949	                return owner.post_iofused_tasks_quiet(self_id, tasks, count, sig);
  3950	            } else {
  3951	                return owner.post_tasks_quiet(self_id, tasks, count, sig);
  3952	            }
  3953	        };
  3954	        DispatchResult result = DispatchResult::Progress;
  3955	        bool head_candidate = true;   // only the pass's FIRST dispatch can be the direct head
  3956	        const uint8_t security_flags = srv_->security_flags();
  3957	        const bool auth_required = (security_flags & Server::kSecurityAuth) != 0;
  3958	        const bool acl_active = (security_flags & Server::kSecurityAcl) != 0;
  3959	        const bool notify_armed = notify_armed_;
  3960	        const uint64_t pass_max_bulk_len = proto_max_bulk_len_;
  3961	        const bool default_bulk_limit = pass_max_bulk_len == 512ull * 1024 * 1024;
  3962	        // One continuous-placement epoch per parse pass. Work published concurrently with a new
  3963	        // EX drain is included in that drain; reloading the stage for every operation would add a
  3964	        // shared atomic to the request path without strengthening the ownership fence.
  3965	        const bool lb_pause_this_pass = lb_controller_armed_ &&
  3966	            srv_->lb_should_pause(self_id, c->id());
  3967	        if (__builtin_expect(lb_pause_this_pass, false)) {
  3968	            flip_fingerprint_finish_pass();
  3969	            return result;
  3970	        }
  3971	        // ONE epoch for the whole parse pass, not one per op. Monotonicity needs the stamps to be
  3972	        // non-decreasing along the connection, not distinct: every op this pass parses may share
  3973	        // the pass's cut, and the next pass's cut is >= this one because the sequence only moves
  3974	        // forward. The freshness floor survives the fold -- the pass starts after the bytes it
  3975	        // parses arrived, so no read is ever older than its own arrival, which is what keeps the
  3976	        // disjoint-window case (a writer's reply fully precedes the reader's send) correct.
  3977	        // With --atomic 0 the tracking word is never written by anyone, so this is one L1 hit on a
  3978	        // shared-clean line, the sequence load never happens, and the per-op test is never taken.
  3979	        const bool atomic_tracking = srv_->atomic_tracking_active();
  3980	        const uint64_t pass_read_cut = atomic_tracking ? srv_->atomic_snapshot() : 0;
  3981	        uint64_t batch_start_ops = 0;
  3982	        [[maybe_unused]] bool read_local_batch = false;
  3983	        // LANE ADMISSION (P128.md): how many lane slots (pending local reads) this connection may
  3984	        // hold during this pass. UINT32_MAX unless the thread's local-read lane is under pressure,
  3985	        // in which case it is the lane divided among the active connections
  3986	        // (ExLoop::read_local_lane_quota). The pressure byte is tested FIRST and on its own,
  3987	        // because active_.size() would otherwise be evaluated eagerly on every pass and active_
  3988	        // lives on an IoLoop cache line this pass never otherwise touches -- see
  3989	        // read_local_lane_under_pressure(). Steady state: one byte test per pass on a line the pass
  3990	        // already owns; the per-op compare below is a popcount against a register.
  3991	        [[maybe_unused]] uint32_t read_local_quota = UINT32_MAX;
  3992	        if constexpr (Fused)
  3993	            if (read_local_enabled &&
  3994	                __builtin_expect(fused_executor_->read_local_lane_under_pressure(), false))
  3995	                read_local_quota = fused_executor_->read_local_lane_quota(active_.size());
  3996	        if constexpr (BatchOps != 0) batch_start_ops = sig.ops;
  3997	        [[maybe_unused]] uint64_t batch_dispatch_start = 0;
  3998	        if constexpr (IoPipe) batch_dispatch_start = rob.dispatch_id();
  3999	        for (;;) {
  4000	            if constexpr (IoPipe)
  4001	                if (rob.dispatch_id() - batch_dispatch_start >=
  4002	                    kIoPipeIfidBatchOpsPerClient) break;
  4003	            if constexpr (BatchOps != 0)
  4004	                if (sig.ops - batch_start_ops >= BatchOps) break;
  4005	            // The coordinator's own connection already holds the unfinished FLIP head. Do not
  4006	            // parse behind it. Other connections may still parse the FLIP report/control command
  4007	            // below so live-vs-target remains observable while the dispatch barrier is active.
  7127	                                               client->rob().in_flight()});
  7128	        }
  7129	        srv_->lb_publish_client_observations(self_->id(), lb_client_observations_);
  7130	        return static_cast<uint32_t>(lb_client_observations_.size());
  7131	    }
  7132	
  7133	    void refresh_notify_config() {
  7134	        LiveConfigSnapshot snapshot;
  7135	        if (!srv_->live_config_snapshot_if_changed(notify_config_version_, snapshot)) return;
  7136	        notify_config_armed_ = snapshot.notify_events != 0;
  7137	        save_config_armed_ = snapshot.save_armed;
  7138	        notify_armed_ = notify_config_armed_ || save_config_armed_ ||
  7139	                        climon_armed_cached_ != 0;
  7140	        proto_max_bulk_len_ = snapshot.proto_max_bulk_len;
  7141	        // Connection-local commands never reach an executor, so the IO thread owns their timing.
  7142	        // Same snapshot, same pass, no extra load.
  7143	        slowlog_arm_.slowlog_us = snapshot.slowlog_log_slower_than;
  7144	        slowlog_arm_.latency_ms = snapshot.latency_monitor_threshold;
  7145	        slowlog_armed_ = slowlog_arm_.armed();
  7146	        if (srv_->read_local_enabled()) {
  7147	            fused_executor_->set_read_local_point_writes_precise(
  7148	                snapshot.maxmemory == 0 || snapshot.policy == MaxmemoryPolicy::NoEviction);
  7149	            fused_executor_->set_read_local_keymiss_notify(
  7150	                (snapshot.notify_events & NOTIFY_KEY_MISS) != 0);
  7151	        }
  7152	        notify_config_version_ = snapshot.version;
  7153	    }
  7154	
  7155	    void close_client(Client* c, bool drain_tls_output = false) {
  7156	        // IDEMPOTENT, and that is load-bearing: an abrupt disconnect can close a conn twice --
  7157	        // once when the recv fails and again when the in-flight reply's send CQE comes back
  7158	        // failed. The second call found the client already parked on the deferred-free list and
  7159	        // parked it AGAIN: a double delete, one reap later. Caught by the torture battery's RST
  7160	        // churn under ASAN; latent since the first teardown path was written.
  7161	        if (c->dead()) return;
  7162	        if (!c->closing()) {
  7163	            c->mark_closing();
  7164	            if (deferred_timer_cancel(c)) enqueue_serve(c);
  7165	            if (c->blocked() && blocking_cancel_client(*srv_, *self_, ring_, *c))
  7166	                enqueue_serve(c);
429:    void run() {
559:            refresh_notify_config();
813:            refresh_notify_config();
1420:            refresh_notify_config();
7133:    void refresh_notify_config() {
     1	// One boot-only rendezvous for the fused runtime.
     2	//
     3	// The gate's public phase is monotonic: Loaded -> Ready -> Running (or Stopping).  Per-worker
     4	// state makes every arrival and gave-up edge once-only, so a stop racing either rendezvous cannot
     5	// leave main waiting for a counter that the worker will never increment.
     6	#pragma once
     7	
     8	#include <atomic>
     9	#include <chrono>
    10	#include <condition_variable>
    11	#include <cstdint>
    12	#include <cstdlib>
    13	#include <mutex>
    14	#include <string>
    15	#include <vector>
    16	
    17	namespace tomo {
    18	
    19	class FusedBootGate {
    20	public:
    21	    enum class Phase : uint8_t { Loaded, Ready, Running, Stopping };
    22	
    23	    explicit FusedBootGate(uint32_t workers)
    24	        : workers_(workers), worker_state_(workers, WorkerState::Loading) {}
    25	
    26	    bool arrive_loaded(uint32_t tid, bool ok, const std::string& error) {
    27	        std::lock_guard<std::mutex> lock(mu_);
    28	        check_tid(tid);
    29	        if (worker_state_[tid] != WorkerState::Loading) std::abort();
    30	        loaded_++;
    31	        if (ok && phase_ != Phase::Stopping) {
    32	            worker_state_[tid] = WorkerState::Loaded;
    33	        } else {
    34	            mark_gave_up_locked(tid);
    35	            if (!ok) fail_locked(error.empty() ? "unified thread initialization failed" : error);
    36	        }
    37	        cv_.notify_all();
    38	        return worker_state_[tid] == WorkerState::Loaded;
    39	    }
    40	
    41	    bool wait_until_ready(uint32_t tid, const std::atomic<bool>& stop) {
    42	        std::unique_lock<std::mutex> lock(mu_);
    43	        check_tid(tid);
    44	        if (worker_state_[tid] != WorkerState::Loaded) std::abort();
    45	        if (!wait_for_phase_locked(lock, Phase::Ready, stop)) {
    46	            mark_gave_up_locked(tid);
    47	            cv_.notify_all();
    48	            return false;
    49	        }
    50	        return true;
    51	    }
    52	
    53	    bool arrive_ready(uint32_t tid) {
    54	        std::lock_guard<std::mutex> lock(mu_);
    55	        check_tid(tid);
    56	        if (worker_state_[tid] != WorkerState::Loaded) std::abort();
    57	        // A worker can leave wait_until_ready(), spend time arming its listeners, and return after
    58	        // main has observed a signal and moved the gate to Stopping. That is an ordinary stop edge,
    59	        // not an impossible transition.
    60	        if (phase_ == Phase::Stopping) {
    61	            mark_gave_up_locked(tid);
    62	            cv_.notify_all();
    63	            return false;
    64	        }
    65	        if (phase_ != Phase::Ready) std::abort();
    66	        worker_state_[tid] = WorkerState::Ready;
    67	        ready_++;
    68	        cv_.notify_all();
    69	        return true;
    70	    }

exec
/bin/bash -lc "nl -ba src/cmd/t_server.cc | sed -n '1790,1905p'
nl -ba src/cmd/t_server.cc | sed -n '1945,2050p'
nl -ba src/cmd/t_server.cc | sed -n '1622,1677p'" in /home/user/Projects/cx-rl2s
exec
/bin/bash -lc "nl -ba src/core/read_local.h | sed -n '245,334p'
rg -n '\\bnew\\b|reserve\\(|resize\\(|alloc|sink|register|init\\(' src/core/read_local.h src/store/read_local_reclaim.h src/store/kv_block_cache.h
nl -ba src/store/flatstore.h | sed -n '538,572p'
nl -ba src/store/flatstore.h | sed -n '720,789p'" in /home/user/Projects/cx-rl2s
 succeeded in 0ms:
  1790	            foreign_read_occupied_cells += sh.store().foreign_read_occupied_cells();
  1791	            foreign_read_wildcard_cells += sh.store().foreign_read_wildcard_cells();
  1792	            foreign_read_saturated_cells += sh.store().foreign_read_saturated_cells();
  1793	            foreign_read_poisoned_shards += sh.store().foreign_read_poisoned();
  1794	            blocking_waiters += sh.blocking_waiters();
  1795	        }
  1796	        for (uint32_t t = 0; t < g_server->nthreads(); t++) {
  1797	            for (uint32_t id = 0; id < command_registry_size(); id++) {
  1798	                const uint64_t calls = g_server->thread(t).command_calls(id);
  1799	                total_ops += calls;
  1800	                if (!op.spec || id != op.spec->id) sampled_ops += calls;
  1801	            }
  1802	            connections += g_server->thread(t).sig().accepts;
  1803	            atomic_localfast += g_server->thread(t).atomic_localfast();
  1804	            atomic_scan_holds += g_server->thread(t).atomic_scan_holds();
  1805	            acl_denied_cmd += g_server->thread(t).sig().acl_access_denied_cmd;
  1806	            acl_denied_key += g_server->thread(t).sig().acl_access_denied_key;
  1807	            acl_denied_channel += g_server->thread(t).sig().acl_access_denied_channel;
  1808	            acl_denied_auth += g_server->thread(t).sig().acl_access_denied_auth;
  1809	            const LoopSignals& sig = g_server->thread(t).sig();
  1810	            plain_accepts += sig.plain_accepts;
  1811	            tls_accepts += sig.tls_accepts;
  1812	            tls_handshakes_started += sig.tls_handshakes_started;
  1813	            tls_handshakes_completed += sig.tls_handshakes_completed;
  1814	            tls_handshakes_failed += sig.tls_handshakes_failed;
  1815	            tls_connections_freed += sig.tls_connections_freed;
  1816	            tls_want_read += sig.tls_want_read;
  1817	            tls_want_write += sig.tls_want_write;
  1818	            tls_ciphertext_input += sig.tls_ciphertext_input_bytes;
  1819	            tls_plaintext_input += sig.tls_plaintext_input_bytes;
  1820	            tls_ciphertext_output += sig.tls_ciphertext_output_bytes;
  1821	            tls_plaintext_output += sig.tls_plaintext_output_bytes;
  1822	            tls_zc_suppressed += sig.tls_zc_suppressed;
  1823	            tls_ktls_active += sig.tls_ktls_active;
  1824	            tls_ktls_fallback += sig.tls_ktls_fallback;
  1825	            epoll_events += sig.epoll_events;
  1826	            epoll_recvs += sig.epoll_recvs;
  1827	            net_input_bytes += sig.net_input_bytes;
  1828	            net_output_bytes += sig.net_output_bytes;
  1829	            if (const WbEngine* wb = g_server->thread(t).wb_engine()) {
  1830	                const WbEngine::Stats& stats = wb->stats();
  1831	                sends_submitted += stats.sends_submitted;
  1832	                short_writes += stats.short_writes;
  1833	                bytes_sent += stats.bytes_sent;
  1834	                peer_aborts += stats.peer_aborts;
  1835	                send_errors += stats.send_errors;
  1836	                zc_sends += stats.zc_sends;
  1837	                zc_releases += stats.zc_releases;
  1838	            }
  1839	        }
  1840	        if (g_server->read_local_enabled()) {
  1841	            for (uint32_t t = 0; t < g_server->nthreads(); t++)
  1842	                add_read_local_stats(read_local, g_server->thread(t).read_local_stats());
  1843	        }
  1844	        // Redis counts BOTH accept-time reject classes in rejected_connections: maxclients
  1845	        // (networking.c:1355) and protected-mode denials (networking.c:1306).
  1846	        rejected = g_server->rejected_conns() + g_server->rejected_connections();
  1847	        auth_failures = g_server->auth_failures();
  1848	    }
  1849	    if (g_server && g_server->read_local_enabled()) {
  1850	        hits += read_local.keyspace_hits;
  1851	        misses += read_local.keyspace_misses;
  1852	    }
  1853	    // Apply the CONFIG RESETSTAT baseline to exactly the counters redis's RESETSTAT zeroes. The
  1854	    // active-expiry lag value is an absolute lifetime high-water gauge, so it is deliberately not
  1855	    // rebased: subtracting two maxima would no longer be a duration in milliseconds.
  1856	    StatBaseline baseline;
  1857	    {
  1858	        std::lock_guard<std::mutex> lock(g_stat_baseline_mu);
  1859	        baseline = g_stat_baseline;
  1860	    }
  1861	    total_ops = minus_baseline(total_ops, baseline.total_ops);
  1862	    connections = minus_baseline(connections, baseline.connections);
  1863	    hits = minus_baseline(hits, baseline.hits);
  1864	    misses = minus_baseline(misses, baseline.misses);
  1865	    expired = minus_baseline(expired, baseline.expired);
  1866	    evicted = minus_baseline(evicted, baseline.evicted);
  1867	    rejected = minus_baseline(rejected, baseline.rejected);
  1868	    net_input_bytes = minus_baseline(net_input_bytes, baseline.net_input_bytes);
  1869	    net_output_bytes = minus_baseline(net_output_bytes, baseline.net_output_bytes);
  1870	    auth_failures = minus_baseline(auth_failures, baseline.auth_failures);
  1871	    acl_denied_cmd = minus_baseline(acl_denied_cmd, baseline.acl_denied_cmd);
  1872	    acl_denied_key = minus_baseline(acl_denied_key, baseline.acl_denied_key);
  1873	    acl_denied_channel = minus_baseline(acl_denied_channel, baseline.acl_denied_channel);
  1874	    acl_denied_auth = minus_baseline(acl_denied_auth, baseline.acl_denied_auth);
  1875	    if (g_server && g_server->read_local_enabled()) {
  1876	        read_local.hits = minus_baseline(read_local.hits, baseline.read_local.hits);
  1877	        read_local.keyspace_hits = minus_baseline(
  1878	            read_local.keyspace_hits, baseline.read_local.keyspace_hits);
  1879	        read_local.keyspace_misses = minus_baseline(
  1880	            read_local.keyspace_misses, baseline.read_local.keyspace_misses);
  1881	        read_local.fallback_multi = minus_baseline(
  1882	            read_local.fallback_multi, baseline.read_local.fallback_multi);
  1883	        read_local.fallback_watch = minus_baseline(
  1884	            read_local.fallback_watch, baseline.read_local.fallback_watch);
  1885	        read_local.fallback_context = minus_baseline(
  1886	            read_local.fallback_context, baseline.read_local.fallback_context);
  1887	        read_local.fallback_context_owner_key = minus_baseline(
  1888	            read_local.fallback_context_owner_key,
  1889	            baseline.read_local.fallback_context_owner_key);
  1890	        read_local.fallback_context_connection_state = minus_baseline(
  1891	            read_local.fallback_context_connection_state,
  1892	            baseline.read_local.fallback_context_connection_state);
  1893	        read_local.fallback_context_route = minus_baseline(
  1894	            read_local.fallback_context_route, baseline.read_local.fallback_context_route);
  1895	        read_local.fallback_context_keymiss_notify = minus_baseline(
  1896	            read_local.fallback_context_keymiss_notify,
  1897	            baseline.read_local.fallback_context_keymiss_notify);
  1898	        read_local.fallback_inflight_write = minus_baseline(
  1899	            read_local.fallback_inflight_write, baseline.read_local.fallback_inflight_write);
  1900	        read_local.fallback_arm_transient = minus_baseline(
  1901	            read_local.fallback_arm_transient, baseline.read_local.fallback_arm_transient);
  1902	        read_local.arm.arms = minus_baseline(
  1903	            read_local.arm.arms, baseline.read_local.arm.arms);
  1904	        read_local.arm.sidecars = minus_baseline(
  1905	            read_local.arm.sidecars, baseline.read_local.arm.sidecars);
  1945	            baseline.read_local.mget_fallback_context_keymiss_notify);
  1946	        read_local.mget_fallback_inflight_write = minus_baseline(
  1947	            read_local.mget_fallback_inflight_write,
  1948	            baseline.read_local.mget_fallback_inflight_write);
  1949	        read_local.mget_fallback_arm_transient = minus_baseline(
  1950	            read_local.mget_fallback_arm_transient,
  1951	            baseline.read_local.mget_fallback_arm_transient);
  1952	        read_local.mget_fallback_atomic_pending = minus_baseline(
  1953	            read_local.mget_fallback_atomic_pending,
  1954	            baseline.read_local.mget_fallback_atomic_pending);
  1955	        read_local.mget_fallback_typed = minus_baseline(
  1956	            read_local.mget_fallback_typed, baseline.read_local.mget_fallback_typed);
  1957	        read_local.mget_fallback_expired = minus_baseline(
  1958	            read_local.mget_fallback_expired, baseline.read_local.mget_fallback_expired);
  1959	        read_local.mget_fallback_seq_churn = minus_baseline(
  1960	            read_local.mget_fallback_seq_churn,
  1961	            baseline.read_local.mget_fallback_seq_churn);
  1962	        read_local.mget_generation_retries = minus_baseline(
  1963	            read_local.mget_generation_retries,
  1964	            baseline.read_local.mget_generation_retries);
  1965	        read_local.mget_fallback_generation = minus_baseline(
  1966	            read_local.mget_fallback_generation,
  1967	            baseline.read_local.mget_fallback_generation);
  1968	        read_local.mget_fallback_lane_full = minus_baseline(
  1969	            read_local.mget_fallback_lane_full,
  1970	            baseline.read_local.mget_fallback_lane_full);
  1971	    }
  1972	    const uint64_t connected = g_server ? g_server->live_clients() : 0;
  1973	
  1974	    if (info_section(op, "SERVER")) {
  1975	        const uint64_t uptime = g_started_monotonic_ns ? (now_ns() - g_started_monotonic_ns) / 1000000000ull : 0;
  1976	        // process_id and tcp_port are plain facts about this process, not telemetry that could be
  1977	        // stale -- and tooling depends on them. The NIC bench harness identifies the server it just
  1978	        // booted by reading process_id out of INFO, so its absence made every NIC cell fail with an
  1979	        // opaque "boot/cell FAIL" long before any measurement was taken.
  1980	        // read_local is the effective boot state. Actual loop entry and successful completions
  1981	        // are separate observations: a configured but unreachable lane must be visible in INFO.
  1982	        appendf(body, "# Server\r\nredis_version:%s\r\ntomokv_version:%s\r\nredis_mode:standalone\r\n"
  1983	                      "thread_mode:%s\r\noverlap:%u\r\nthread_pipeline:%u\r\nread_local:%u\r\n"
  1984	                      "arch_bits:%zu\r\nmultiplexing_api:io_uring\r\nprocess_id:%lld\r\n"
  1985	                      "tcp_port:%u\r\nuptime_in_seconds:%llu\r\nuptime_in_days:%llu\r\n",
  1986	                kVersion, kVersion, g_server ? g_server->thread_mode_name() : "2s",
  1987	                g_server ? g_server->cfg().overlap : 0u,
  1988	                g_server ? g_server->cfg().overlap : 0u,
  1989	                g_server && g_server->read_local_enabled() ? 1u : 0u,
  1990	                sizeof(void*) * 8,
  1991	                static_cast<long long>(::getpid()),
  1992	                static_cast<unsigned>(g_server ? g_server->cfg().port : 0),
  1993	                static_cast<unsigned long long>(uptime),
  1994	                static_cast<unsigned long long>(uptime / 86400));
  1995	        uint32_t active_readers = 0;
  1996	        if (g_server) {
  1997	            uint32_t owned[kMaxThreads] = {};
  1998	            // Read the published ownership directory, never a vector FLIP may be rewriting.
  1999	            for (uint32_t sid = 0; sid < g_server->nshards(); sid++)
  2000	                owned[g_server->worker_of_shard(static_cast<int32_t>(sid))]++;
  2001	            for (uint32_t tid = 0; tid < g_server->nthreads(); tid++) {
  2002	                const ThreadCtx& thread = g_server->thread(tid);
  2003	                const bool active = thread.read_local_lane_active();
  2004	                active_readers += active;
  2005	                const Role role = thread.role();
  2006	                const char* name = role == Role::Idle ? "idle"
  2007	                    : g_server->thread_mode() == ThreadMode::Fused ? "unified"
  2008	                    : role == Role::Ifid ? "ifid" : "ex";
  2009	                const ReadLocalStats* stats = g_server->read_local_enabled()
  2010	                    ? &thread.read_local_stats() : nullptr;
  2011	                // These per-thread totals are lifetime counters; INFO stats retains its existing
  2012	                // RESETSTAT-relative aggregate counters. FLIP does not reset either sidecar.
  2013	                appendf(body,
  2014	                        "read_local_thread_%u:role=%s,shards=%u,active=%u,"
  2015	                        "hits_total=%llu,mget_hits_total=%llu\r\n",
  2016	                        tid, name, owned[tid], active ? 1u : 0u,
  2017	                        static_cast<unsigned long long>(stats ? stats->hits : 0),
  2018	                        static_cast<unsigned long long>(stats ? stats->mget_local_hits : 0));
  2019	            }
  2020	        }
  2021	        appendf(body, "read_local_active_threads:%u\r\n", active_readers);
  2022	        if (g_server && g_server->thread_mode() == ThreadMode::Fused) {
  2023	            appendf(body,
  2024	                    "fused_threads:%u\r\nclient_threads:%u\r\nowner_threads:%u\r\n"
  2025	                    "flip_available:0\r\nflip_unavailable_reason:threads_are_fused\r\n",
  2026	                    g_server->nthreads(), g_server->client_serving_thread_count(),
  2027	                    g_server->shard_owner_count());
  2028	        } else {
  2029	            const FlipReport flip = g_server ? g_server->flip_report() : FlipReport{};
  2030	            appendf(body,
  2031	                    "io_threads:%u\r\nex_threads:%u\r\nflip_target_io:%u\r\n"
  2032	                    "flip_target_ex:%u\r\nflip_smt_mode:%u\r\n"
  2033	                    "flip_unit_threads:%u\r\nflip_bucket_min:%u\r\nflip_bucket_max:%u\r\n"
  2034	                    "flip_client_min:%u\r\nflip_client_max:%u\r\n"
  2035	                    "flip_last_transfers:%llu\r\nflip_in_progress:%u\r\n",
  2036	                    flip.live_io, flip.live_ex, flip.target_io, flip.target_ex,
  2037	                    flip.smt_mode, flip.unit_threads, flip.bucket_min, flip.bucket_max,
  2038	                    flip.client_min, flip.client_max,
  2039	                    static_cast<unsigned long long>(flip.last_transfers),
  2040	                    flip.moving ? 1u : 0u);
  2041	        }
  2042	    }
  2043	    if (info_section(op, "FLIPCTL")) {
  2044	        if (g_server && !g_server->flipctl_available()) {
  2045	            appendf(body,
  2046	                    "# Flipctl\r\nflipctl_state:unavailable\r\nflipctl_phase:fused\r\n"
  2047	                    "flipctl_available:0\r\nflipctl_thread_mode:1s\r\n"
  2048	                    "flipctl_reason:threads_are_fused\r\nflipctl_fused_threads:%u\r\n"
  2049	                    "flipctl_client_threads:%u\r\nflipctl_owner_threads:%u\r\n",
  2050	                    g_server->nthreads(), g_server->client_serving_thread_count(),
  1622	    total.settax.add(local.settax);
  1623	#endif
  1624	}
  1625	
  1626	void collect_stat_totals(StatBaseline& out) {
  1627	    out = StatBaseline{};
  1628	    if (!g_server) return;
  1629	    for (uint32_t i = 0; i < g_server->nshards(); i++) {
  1630	        const Shard& sh = g_server->shard(static_cast<int32_t>(i));
  1631	        out.hits += sh.stats().hits;
  1632	        out.misses += sh.stats().misses;
  1633	        out.expired += sh.stats().expired;
  1634	        out.evicted += sh.published_evicted();
  1635	        out.keys += sh.published_size();
  1636	        out.object_bytes += sh.published_obj_bytes();
  1637	    }
  1638	    out.command_calls.assign(command_registry_size(), 0);
  1639	    for (uint32_t t = 0; t < g_server->nthreads(); t++) {
  1640	        ThreadCtx& thread = g_server->thread(t);
  1641	        for (uint32_t id = 0; id < command_registry_size(); id++) {
  1642	            const uint64_t calls = thread.command_calls(id);
  1643	            out.command_calls[id] += calls;
  1644	            out.total_ops += calls;
  1645	            if (std::strcmp(command_registry_at(id)->name, "INFO")) out.sampled_ops += calls;
  1646	        }
  1647	        const LoopSignals& sig = thread.sig();
  1648	        out.connections += sig.accepts;
  1649	        out.acl_denied_cmd += sig.acl_access_denied_cmd;
  1650	        out.acl_denied_key += sig.acl_access_denied_key;
  1651	        out.acl_denied_channel += sig.acl_access_denied_channel;
  1652	        out.acl_denied_auth += sig.acl_access_denied_auth;
  1653	        out.net_input_bytes += sig.net_input_bytes;
  1654	        out.net_output_bytes += sig.net_output_bytes;
  1655	    }
  1656	    if (g_server->read_local_enabled()) {
  1657	        for (uint32_t t = 0; t < g_server->nthreads(); t++)
  1658	            add_read_local_stats(out.read_local, g_server->thread(t).read_local_stats());
  1659	        out.hits += out.read_local.keyspace_hits;
  1660	        out.misses += out.read_local.keyspace_misses;
  1661	    }
  1662	    out.rejected = g_server->rejected_conns() + g_server->rejected_connections();
  1663	    out.auth_failures = g_server->auth_failures();
  1664	}
  1665	
  1666	// Saturating: a baseline can only ever be <= the live counter, but a counter that wrapped or a
  1667	// shard that was added after the reset must not underflow into a nonsense INFO value.
  1668	inline uint64_t minus_baseline(uint64_t live, uint64_t base) {
  1669	    return live >= base ? live - base : 0;
  1670	}
  1671	
  1672	uint64_t accounted_memory_bytes(uint64_t object_bytes, uint64_t keys) {
  1673	    constexpr uint64_t overhead = FlatStore::kSlotOverheadPerKey;
  1674	    if (keys > (std::numeric_limits<uint64_t>::max() - object_bytes) / overhead)
  1675	        return std::numeric_limits<uint64_t>::max();
  1676	    return object_bytes + keys * overhead;
  1677	}

 succeeded in 0ms:
   245	    uint32_t batch_head = 0;
   246	    uint32_t batch_count = 0;
   247	};
   248	
   249	class ReadLocalDeferredQueue {
   250	public:
   251	    static constexpr uint32_t kCapacity = ReadLocalRetireRing::kCapacity;
   252	
   253	    // The owner's block cache dies with the owner; nothing can still reference its blocks by then
   254	    // (drain_shutdown ran, and a cached block is by construction referenced by nobody).
   255	    ~ReadLocalDeferredQueue() { block_cache_.release_all(); }
   256	
   257	    bool init(Server* server, ThreadCtx* owner) {
   258	        server_ = server;
   259	        owner_ = owner;
   260	        entries_.reset(new (std::nothrow) Entry[kCapacity]);
   261	        if (!entries_) return false;
   262	        sink_.context = this;
   263	        sink_.defer = &ReadLocalDeferredQueue::defer_thunk;
   264	        // Every store this owner drives shares ONE cache: a single head array that stays hot in
   265	        // L1 for every armed write, whatever shard the key lands on. See kv_block_cache.h.
   266	        sink_.block_cache = &block_cache_;
   267	#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
   268	        sink_.bind_settax_stats(&owner_->read_local_stats().settax);
   269	#endif
   270	        return true;
   271	    }
   272	
   273	    ReadLocalRetireSink* sink() { return entries_ ? &sink_ : nullptr; }
   274	    bool empty() const { return ring_.empty(); }
   275	    uint32_t size() const { return ring_.count; }
   276	
   277	    // Called only by this owner thread, after the object/table is no longer store-reachable.
   278	    // Arguments are not re-validated: the sink is handed out only once entries_ exists (sink()),
   279	    // `reclaim` is always one of FlatStore's three static callbacks, and every producer tests its
   280	    // payload before retiring it. A null in any of them would fault at reclaim in the same pass.
   281	    void defer(void* reclaim_owner, void* payload, size_t auxiliary,
   282	               ReadLocalRetireSink::ReclaimFn reclaim) {
   283	#ifdef TOMO_RL_CACHE_DEBUG
   284	        // A payload may be in this ring at most once. Retiring one object twice puts it into the
   285	        // owner's block cache twice, which splices a CYCLE into that class's free list; the block
   286	        // is then handed out while still linked, its header overwrites the `next` word, and the
   287	        // next take() either aborts on the allocation check or dereferences a wild head. That is
   288	        // the corruption this P0 is chasing, so name it at the double retire instead.
   289	        dbg_check_owner("defer");
   290	        if (!dbg_pending_.insert(payload).second) {
   291	            std::fprintf(stderr,
   292	                "\nRLRING-VIOLATION double-retire: payload %p aux %zu reclaim %p (queue %p, "
   293	                "ring count %u)\n", payload, auxiliary,
   294	                reinterpret_cast<void*>(reclaim), static_cast<void*>(this), ring_.count);
   295	            std::fflush(stderr);
   296	            std::abort();
   297	        }
   298	#endif
   299	        if (ring_.full()) force_oldest_grace();
   300	        Entry& entry = entries_[ring_.push()];
   301	        entry.owner = reclaim_owner;
   302	        entry.payload = payload;
   303	        entry.auxiliary = auxiliary;
   304	        entry.reclaim = reclaim;
   305	        // The next drain (or rare capacity seal) performs the global epoch RMW after every unlink
   306	        // in this suffix. Sharing that stamp avoids one globally contended RMW per retired object.
   307	#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
   308	        ReadLocalSetTaxStats& stats = settax_stats();
   309	        stats.qsbr_deferrals++;
   310	        if (auxiliary) stats.qsbr_object_deferrals++;
   311	        else stats.qsbr_table_deferrals++;
   312	        stats.qsbr_depth = ring_.count;
   313	        stats.qsbr_max_owner_depth = std::max<uint64_t>(
   314	            stats.qsbr_max_owner_depth, ring_.count);
   315	        stats.qsbr_depth_samples++;
   316	        stats.qsbr_depth_sum += ring_.count;
   317	#endif
   318	    }
   319	
   320	    uint32_t drain_ready() {
   321	        if (ring_.empty()) return 0;
   322	        seal_pending();
   323	        // One participant scan per owner pass, not per retired allocation. Stamps are FIFO and
   324	        // strictly below the returned floor only after every active tick has crossed them; parked
   325	        // participants contribute infinity because they hold no foreign pointer.
   326	#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
   327	        ReadLocalSetTaxStats& stats = settax_stats();
   328	        stats.qsbr_grace_scans++;
   329	        stats.qsbr_participant_loads += server_->nthreads();
   330	#endif
   331	        // Ask only whether the OLDEST sealed stamp has been crossed. Nothing is releasable until it
   332	        // has, so the scan may stop at the first participant still below it (and re-test that one
   333	        // first next pass) instead of loading every participant's tick line each pass. The ring is
   334	        // non-empty here and seal_pending() just ran, so a sealed batch exists.
src/store/kv_block_cache.h:5:// publishes it, and retires the displaced one through QSBR — one mallocx and one sdallocx per
src/store/kv_block_cache.h:20:// its head array and byte gauge are two extra cache lines PER SHARD on a path whose allocator
src/store/kv_block_cache.h:21:// alternative (jemalloc's tcache) is per-THREAD and always hot. One cache per fused owner restores
src/store/kv_block_cache.h:29:#include "../base/alloc.h"
src/store/kv_block_cache.h:41:// Size-class index for a good_size()-rounded allocation. Injective over good_size outputs, which
src/store/kv_block_cache.h:45:inline uint32_t kv_block_class(size_t allocation) {
src/store/kv_block_cache.h:46:    if (allocation <= 8) return 0;
src/store/kv_block_cache.h:47:    if (allocation <= 128) return static_cast<uint32_t>(allocation / 16);
src/store/kv_block_cache.h:48:    const int k = 63 - __builtin_clzll(static_cast<unsigned long long>(allocation - 1));
src/store/kv_block_cache.h:50:    const uint32_t quarter = static_cast<uint32_t>(allocation / step);
src/store/kv_block_cache.h:59:        size_t allocation;
src/store/kv_block_cache.h:63:    // through to the allocator: they are collection or huge-key objects, not what a SET builds.
src/store/kv_block_cache.h:84:    static bool eligible(size_t allocation) { return allocation >= sizeof(FreeBlock); }
src/store/kv_block_cache.h:113:    void dbg_enter(void* memory, size_t allocation, uint32_t cls) {
src/store/kv_block_cache.h:116:                "\nRLCACHE-VIOLATION double-put: block %p allocation %zu class %u already "
src/store/kv_block_cache.h:118:                memory, allocation, cls, static_cast<void*>(this), class_nodes[cls], bytes);
src/store/kv_block_cache.h:123:    void dbg_leave(void* memory, size_t allocation, uint32_t cls) {
src/store/kv_block_cache.h:126:                "\nRLCACHE-VIOLATION take-of-nonresident: block %p allocation %zu class %u "
src/store/kv_block_cache.h:128:                memory, allocation, cls, static_cast<void*>(this), class_nodes[cls], bytes);
src/store/kv_block_cache.h:140:    void dbg_walk(uint32_t cls, size_t allocation, const char* op) {
src/store/kv_block_cache.h:153:            if (b->allocation != allocation) {
src/store/kv_block_cache.h:155:                    "\nRLCACHE-VIOLATION %s: class %u block %p carries allocation %zu, class "
src/store/kv_block_cache.h:157:                    b->allocation, allocation, seen, static_cast<void*>(this));
src/store/kv_block_cache.h:180:    // `allocation` is already good_size()-rounded by the caller, so the class identifies exactly
src/store/kv_block_cache.h:182:    // caller then allocates, which is the unchanged baseline path.
src/store/kv_block_cache.h:183:    void* take(size_t allocation) {
src/store/kv_block_cache.h:184:        const uint32_t cls = kv_block_class(allocation);
src/store/kv_block_cache.h:188:        dbg_walk(cls, allocation, "take-entry");
src/store/kv_block_cache.h:192:        if (block->allocation != allocation || !class_nodes[cls] || bytes < allocation)
src/store/kv_block_cache.h:195:        dbg_leave(block, allocation, cls);
src/store/kv_block_cache.h:199:        bytes -= allocation;
src/store/kv_block_cache.h:205:    bool put(void* memory, size_t allocation, size_t ceiling) {
src/store/kv_block_cache.h:206:        if (!eligible(allocation)) return false;
src/store/kv_block_cache.h:207:        const uint32_t cls = kv_block_class(allocation);
src/store/kv_block_cache.h:210:        if (allocation > limit - (bytes < limit ? bytes : limit)) return false;
src/store/kv_block_cache.h:212:        // only notice much later (when class_nodes hits 0 with a non-null head). The allocator's
src/store/kv_block_cache.h:218:        dbg_walk(cls, allocation, "put-entry");
src/store/kv_block_cache.h:219:        dbg_enter(memory, allocation, cls);
src/store/kv_block_cache.h:223:        block->allocation = allocation;
src/store/kv_block_cache.h:226:        bytes += allocation;
src/store/kv_block_cache.h:230:    // Hand every cached block back to the allocator. Owner-thread only, like everything here.
src/store/kv_block_cache.h:240:                dbg_leave(block, block->allocation, cls);
src/store/kv_block_cache.h:242:                free_sized(block, block->allocation);
src/store/read_local_reclaim.h:4:// therefore publishes only this tiny boot-bound sink. Reclamation always runs later on the same
src/store/read_local_reclaim.h:24:    using ReclaimFn = void (*)(const ReadLocalRetireSink& sink, void* owner,
src/store/read_local_reclaim.h:33:    // call there would hand back part of the allocator call it exists to remove. Null means the
src/store/read_local_reclaim.h:34:    // owner has no cache and every write allocates, which is the pre-cache behaviour.
src/store/read_local_reclaim.h:62:              "the shipped retire sink is context + defer + block cache and nothing else");
src/core/read_local.h:5:// snapshot-driven passes). A store owner unlinks first, then hands the retired allocation to this
src/core/read_local.h:12:#include <new>
src/core/read_local.h:16:#include "../base/alloc.h"
src/core/read_local.h:257:    bool init(Server* server, ThreadCtx* owner) {
src/core/read_local.h:260:        entries_.reset(new (std::nothrow) Entry[kCapacity]);
src/core/read_local.h:262:        sink_.context = this;
src/core/read_local.h:263:        sink_.defer = &ReadLocalDeferredQueue::defer_thunk;
src/core/read_local.h:266:        sink_.block_cache = &block_cache_;
src/core/read_local.h:268:        sink_.bind_settax_stats(&owner_->read_local_stats().settax);
src/core/read_local.h:273:    ReadLocalRetireSink* sink() { return entries_ ? &sink_ : nullptr; }
src/core/read_local.h:278:    // Arguments are not re-validated: the sink is handed out only once entries_ exists (sink()),
src/core/read_local.h:287:        // next take() either aborts on the allocation check or dereferences a wild head. That is
src/core/read_local.h:323:        // One participant scan per owner pass, not per retired allocation. Stamps are FIFO and
src/core/read_local.h:399:        entry.reclaim(sink_, entry.owner, entry.payload, entry.auxiliary);
src/core/read_local.h:439:        ReadLocalSetTaxStats* stats = sink_.diagnostics();
src/core/read_local.h:468:    ReadLocalRetireSink sink_;
   538	    v = ((v & 0x0000ffff0000ffffULL) << 16) | ((v >> 16) & 0x0000ffff0000ffffULL);
   539	    return (v << 32) | (v >> 32);
   540	}
   541	
   542	inline uint64_t scan_cursor_next(uint64_t cursor, uint64_t mask) {
   543	    cursor |= ~mask;
   544	    cursor = scan_cursor_reverse_bits(cursor);
   545	    cursor++;
   546	    return scan_cursor_reverse_bits(cursor);
   547	}
   548	
   549	class FlatStore;
   550	struct FlatStoreLayoutLock;
   551	
   552	// Armed stores extend the already-cold atomic pending allocation. Keeping AtomicPendingState first
   553	// preserves atomic_pending_ and every FlatStore member/offset; disabled stores allocate precisely
   554	// the historical AtomicPendingState body and nothing else.
   555	struct ReadLocalStoreState {
   556	    AtomicPendingState atomic;
   557	    // `probe_sequence` is the one table word: it changes for topology moves and atomic physical
   558	    // exchanges, never for an ordinary immutable one-slot SET, so plain writes publish nothing
   559	    // beyond their slot store. Point probes validate against it; local MGET validates a group-free
   560	    // participant against it and a pending participant against the per-key cell epochs in
   561	    // `foreign_reads`.
   562	    std::atomic<uint64_t> probe_sequence{0};
   563	    ReadLocalRetireSink retire_sink{};
   564	    uint32_t table_mutation_depth = 0;
   565	    uint32_t pending_count = 0;
   566	    ForeignReadSafety foreign_reads{};
   567	};
   568	static_assert(std::is_standard_layout_v<ReadLocalStoreState>);
   569	static_assert(offsetof(ReadLocalStoreState, atomic) == 0);
   570	
   571	// Hash-field TTLs, defined in src/cmd/t_hash_ttl.cc. The store owns the ATTENTION (which keys carry
   572	// field deadlines, and the bounded cycle that revisits them); the hash lane owns the reap itself,
   720	        expires_.clear();
   721	        if (snapshot_new_tab_) std::free(snapshot_new_tab_);
   722	        for (int t = 0; t < 2; t++)
   723	            if (tab_[t]) {
   724	                for (uint32_t i = 0; i < cap_[t]; i++)
   725	                    if (KvObj* o = ptr_of(tab_[t][i])) kvobj_free(o);
   726	                std::free(tab_[t]);
   727	            }
   728	        // Retired objects no longer appear in either table. They remain here only because an io
   729	        // thread may still be handing their value bytes to the kernel.
   730	        for (const Borrow& b : borrows_)
   731	            if (b.retired) kvobj_free(b.retired);
   732	    }
   733	    FlatStore(const FlatStore&) = delete;
   734	    FlatStore& operator=(const FlatStore&) = delete;
   735	
   736	    // Boot-only allocation, before persistence replay or any foreign probe can exist.
   737	    bool prepare_read_local() { return ensure_read_local_store_state(); }
   738	
   739	    // Enabled is boot-latched. The sink may be rebound only at a quiesced fused ownership handoff;
   740	    // false keeps the old store path and every installed writer hook predicted cold.
   741	    void configure_read_local(bool enabled, ReadLocalRetireSink sink,
   742	                              bool atomic_filter = true) {
   743	        if (enabled && !sink.defer) std::abort();
   744	#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
   745	        if (enabled && !sink.diagnostics()) std::abort();
   746	#endif
   747	        // Persistence loading finishes before the fused executor arms this store. Prepared atomic
   748	        // records cannot be retroactively marked in their entry headers, so fail closed if that
   749	        // boot invariant ever changes instead of publishing a false zero-pending state.
   750	        if (enabled && atomic_pending_entries() != 0) std::abort();
   751	        ReadLocalStoreState* state = read_local_store_state();
   752	        if (enabled && !state) std::abort();
   753	        if (enabled) {
   754	            if (read_local_enabled_ && read_local_atomic_filter_ != atomic_filter) std::abort();
   755	            read_local_atomic_filter_ = atomic_filter;
   756	        }
   757	        if constexpr (kReadLocalSetTaxAtomicRaw) {
   758	            // Persistence/bootstrap may have used the ordinary overwrite path before the boot latch
   759	            // is exposed. Establish fixed atomic payload cells for that final image while no foreign
   760	            // probe can exist; every later Raw constructor performs the same preparation directly.
   761	            if (enabled && !read_local_enabled_)
   762	                for (int table = 0; table < 2; table++)
   763	                    if (tab_[table])
   764	                        for (uint32_t slot = 0; slot < cap_[table]; slot++)
   765	                            if (KvObj* object = ptr_of(tab_[table][slot]))
   766	                                kvobj_prepare_read_local_raw_cells(object);
   767	        }
   768	        if (state) state->retire_sink = sink;
   769	        read_local_enabled_ = enabled;
   770	    }
   771	    void rebind_read_local_retire_sink(ReadLocalRetireSink sink) {
   772	        if (!read_local_enabled_ || !sink.defer) std::abort();
   773	#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
   774	        if (!sink.diagnostics()) std::abort();
   775	#endif
   776	        // Called only after the old owner has acknowledged an empty task/read/retire frontier and
   777	        // before the new owner executes store work. Advance the table generation at that ownership
   778	        // edge so a foreign copy cannot validate across two retire domains. Foreign probes never
   779	        // read the sink; keeping the boot-latched enabled byte untouched avoids a true-to-true data
   780	        // race at LB resume.
   781	        ReadLocalTableGuard ownership_change(*this);
   782	        read_local_store_state_required().retire_sink = sink;
   783	    }
   784	    bool read_local_enabled() const { return read_local_enabled_; }
   785	#ifdef TOMO_RL_CACHE_DEBUG
   786	    // Debug builds only: lets Server assert that this store's sink still names its CURRENT owner.
   787	    const ReadLocalRetireSink& read_local_retire_sink_debug() const {
   788	        return read_local_store_state_required().retire_sink;
   789	    }

exec
/bin/bash -lc "nl -ba src/core/thread.h | sed -n '200,363p'
cat src/store/read_local_reclaim.h
rg -n 'ensure_read_local_store_state|ensure_read_local_write|new.*ReadLocal|reserve\\(' src/store/flatstore.h src/net/client.h src/net/conn.h src/net/rob.h" in /home/user/Projects/cx-rl2s
 exited 2 in 0ms:
   200	
   201	#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
   202	    // Keep temporary SET attribution off the remotely scanned quiescence-publication cache line.
   203	    alignas(64) ReadLocalSetTaxStats settax{};
   204	#endif
   205	
   206	    uint64_t fallbacks() const {
   207	        return fallback_multi + fallback_watch + fallback_context +
   208	               fallback_inflight_write + fallback_arm_transient +
   209	               fallback_atomic_pending + fallback_missing +
   210	               fallback_typed + fallback_expired + fallback_seq_churn +
   211	               fallback_generation + fallback_lane_full;
   212	    }
   213	
   214	    uint64_t mget_fallbacks() const {
   215	        return mget_fallback_multi + mget_fallback_watch + mget_fallback_context +
   216	               mget_fallback_inflight_write + mget_fallback_arm_transient +
   217	               mget_fallback_atomic_pending +
   218	               mget_fallback_typed + mget_fallback_expired + mget_fallback_seq_churn +
   219	               mget_fallback_generation + mget_fallback_lane_full;
   220	    }
   221	
   222	    void note_fallback(ReadLocalFallbackReason reason, bool mget = false) {
   223	        switch (reason) {
   224	            case ReadLocalFallbackReason::Multi: fallback_multi++; break;
   225	            case ReadLocalFallbackReason::Watch: fallback_watch++; break;
   226	            case ReadLocalFallbackReason::ContextOwnerKey:
   227	                fallback_context++;
   228	                fallback_context_owner_key++;
   229	                break;
   230	            case ReadLocalFallbackReason::ContextConnectionState:
   231	                fallback_context++;
   232	                fallback_context_connection_state++;
   233	                break;
   234	            case ReadLocalFallbackReason::ContextRoute:
   235	                fallback_context++;
   236	                fallback_context_route++;
   237	                break;
   238	            case ReadLocalFallbackReason::ContextKeymissNotify:
   239	                fallback_context++;
   240	                fallback_context_keymiss_notify++;
   241	                break;
   242	            case ReadLocalFallbackReason::InflightWrite: fallback_inflight_write++; break;
   243	            case ReadLocalFallbackReason::ArmTransient: fallback_arm_transient++; break;
   244	            case ReadLocalFallbackReason::AtomicPending: fallback_atomic_pending++; break;
   245	            case ReadLocalFallbackReason::Missing: fallback_missing++; break;
   246	            case ReadLocalFallbackReason::Typed: fallback_typed++; break;
   247	            case ReadLocalFallbackReason::Expired: fallback_expired++; break;
   248	            case ReadLocalFallbackReason::SeqChurn: fallback_seq_churn++; break;
   249	            case ReadLocalFallbackReason::Generation: fallback_generation++; break;
   250	            case ReadLocalFallbackReason::LaneFull: fallback_lane_full++; break;
   251	            case ReadLocalFallbackReason::None: std::abort();
   252	        }
   253	        if (!mget) return;
   254	        switch (reason) {
   255	            case ReadLocalFallbackReason::Multi: mget_fallback_multi++; break;
   256	            case ReadLocalFallbackReason::Watch: mget_fallback_watch++; break;
   257	            case ReadLocalFallbackReason::ContextOwnerKey:
   258	                mget_fallback_context++;
   259	                mget_fallback_context_owner_key++;
   260	                break;
   261	            case ReadLocalFallbackReason::ContextConnectionState:
   262	                mget_fallback_context++;
   263	                mget_fallback_context_connection_state++;
   264	                break;
   265	            case ReadLocalFallbackReason::ContextRoute:
   266	                mget_fallback_context++;
   267	                mget_fallback_context_route++;
   268	                break;
   269	            case ReadLocalFallbackReason::ContextKeymissNotify:
   270	                mget_fallback_context++;
   271	                mget_fallback_context_keymiss_notify++;
   272	                break;
   273	            case ReadLocalFallbackReason::InflightWrite:
   274	                mget_fallback_inflight_write++;
   275	                break;
   276	            case ReadLocalFallbackReason::ArmTransient:
   277	                mget_fallback_arm_transient++;
   278	                break;
   279	            case ReadLocalFallbackReason::AtomicPending:
   280	                mget_fallback_atomic_pending++;
   281	                break;
   282	            case ReadLocalFallbackReason::Typed: mget_fallback_typed++; break;
   283	            case ReadLocalFallbackReason::Expired: mget_fallback_expired++; break;
   284	            case ReadLocalFallbackReason::SeqChurn: mget_fallback_seq_churn++; break;
   285	            case ReadLocalFallbackReason::Generation: mget_fallback_generation++; break;
   286	            case ReadLocalFallbackReason::LaneFull: mget_fallback_lane_full++; break;
   287	            // A local MGET miss is an invariant success. Keep a hard edge here so a future caller
   288	            // cannot silently turn it into a reasoned MGET fallback counter.
   289	            case ReadLocalFallbackReason::Missing:
   290	            case ReadLocalFallbackReason::None: std::abort();
   291	        }
   292	    }
   293	};
   294	
   295	// Read-local publication and telemetry are absent from baseline ThreadCtx allocations. The
   296	// lone owning pointer is placed in ThreadCtx's established tail padding below.
   297	struct ReadLocalThreadState {
   298	    std::atomic<uint64_t> tick{0};
   299	    ReadLocalRetireSink retire_sink{};
   300	    ReadLocalStats stats{};
   301	    // Published only on actual reader-loop entry/exit, including FLIP. INFO must distinguish
   302	    // an enabled boot knob from a live parser/executor lane. No per-operation publication.
   303	    std::atomic<bool> lane_active{false};
   304	};
   305	#if TOMO_READ_LOCAL_SET_TAX_VARIANT != 3
   306	static_assert(sizeof(ReadLocalThreadState) == 368,
   307	              "lane activation adds one alignment word only to the optional sidecar");
   308	#endif
   309	
   310	class ThreadCtx {
   311	public:
   312	    using RolePrepareFn = bool (*)(void*);
   313	    using RoleCancelFn = void (*)(void*);
   314	    using ClientRegistrationPrepareFn = bool (*)(void*, Client*);
   315	    using ClientRegistrationCancelFn = void (*)(void*, Client*);
   316	    using ClientCapacityPrepareFn = bool (*)(void*, uint32_t);
   317	    using ExecutorProgressFn = uint32_t (*)(void*);
   318	    using SnapshotStartFn = void (*)(void*, SnapshotManager*);
   319	    ThreadCtx() = default;
   320	    ThreadCtx(const ThreadCtx&) = delete;
   321	    ThreadCtx& operator=(const ThreadCtx&) = delete;
   322	
   323	    // `nthreads` is the TOTAL thread count, not the io count: any thread can become a producer
   324	    // through a role change. The task slot allocation is deferred until this physical thread has
   325	    // pinned itself; init_task_inbox_local() then first-touches it from the consumer's local CPU.
   326	    void init(uint32_t id, Role r, uint32_t nthreads, uint32_t age_sample_rate,
   327	              uint32_t flip_work_window) {
   328	        id_ = id;
   329	        role_.store(r, std::memory_order_relaxed);
   330	        nchan_     = nthreads;
   331	        client_in_ = std::make_unique<ClientChan[]>(nthreads);
   332	        release_in_ = std::make_unique<ReleaseChan[]>(nthreads);
   333	        transfer_in_ = std::make_unique<TransferChan[]>(nthreads);
   334	        sig_.configure_age_sampling(age_sample_rate);
   335	        flip_fingerprint_.configure(flip_work_window);
   336	    }
   337	
   338	    bool init_task_inbox_local(const std::vector<uint32_t>& io,
   339	                               const std::vector<uint32_t>& ex) {
   340	        std::unique_ptr<TaskInbox> inbox(new (std::nothrow) TaskInbox);
   341	        if (!inbox || !inbox->init_local(nchan_, kInboxSlots, io, ex)) return false;
   342	        task_in_ = std::move(inbox);
   343	        return true;
   344	    }
   345	    bool init_task_inbox_local_fused() {
   346	        std::unique_ptr<TaskInbox> inbox(new (std::nothrow) TaskInbox);
   347	        if (!inbox || !inbox->init_local_fused<kInboxSlots>(nchan_)) return false;
   348	        task_in_ = std::move(inbox);
   349	        return true;
   350	    }
   351	    bool init_read_local_state() {
   352	        read_local_state_.reset(new (std::nothrow) ReadLocalThreadState);
   353	        return read_local_state_ != nullptr;
   354	    }
   355	    bool remask_task_inbox_quiesced(const std::vector<uint32_t>& io,
   356	                                    const std::vector<uint32_t>& ex) {
   357	        return task_in_ && task_in_->remask_quiesced(io, ex);
   358	    }
   359	    MaskedQueueDiagnostics task_inbox_diagnostics() const {
   360	        return task_in_ ? task_in_->diagnostics() : MaskedQueueDiagnostics{};
   361	    }
   362	
   363	    void init_command_counts(uint32_t count) {
// read_local_reclaim.h — type-erased handoff from a shard store to its fused owner QSBR list.
//
// FlatStore cannot depend on ExLoop without reversing the core/store include graph. The store
// therefore publishes only this tiny boot-bound sink. Reclamation always runs later on the same
// physical owner thread; `owner` and `payload` are opaque until the callback is invoked after a
// grace period.
#pragma once
#include <cstddef>
#include <cstdint>
#include "read_local_settax.h"

namespace tomo {

struct KvBlockCache;

// Capacity of the owner's deferred-retire ring (ReadLocalRetireRing, src/core/read_local.h), which
// static_asserts that it still equals this number. It lives here because the store side derives a
// bound from it and cannot include a core/ header: the ring already permits exactly this many
// retired-but-unreclaimed objects to be resident per owner, so it is the natural ceiling for any
// cache of blocks that ring produces (see KvBlockCache::kMaxBytes).
inline constexpr uint32_t kReadLocalRetireRingCapacity = 4096;

struct ReadLocalRetireSink {
    using ReclaimFn = void (*)(const ReadLocalRetireSink& sink, void* owner,
                               void* payload, size_t auxiliary);
    using DeferFn = void (*)(void* context, void* owner, void* payload,
                            size_t auxiliary, ReclaimFn reclaim);

    void* context = nullptr;
    DeferFn defer = nullptr;
    // The owner's post-grace block cache (src/store/kv_block_cache.h). A DIRECT pointer, not a
    // third function pointer: the armed write path calls into it on every SET, and an indirect
    // call there would hand back part of the allocator call it exists to remove. Null means the
    // owner has no cache and every write allocates, which is the pre-cache behaviour.
    KvBlockCache* block_cache = nullptr;
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
    ReadLocalSetTaxStats* settax_stats = nullptr;
#endif

    void retire(void* owner, void* payload, size_t auxiliary, ReclaimFn reclaim) const {
        defer(context, owner, payload, auxiliary, reclaim);
    }

    void bind_settax_stats(ReadLocalSetTaxStats* stats) {
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
        settax_stats = stats;
#else
        (void)stats;
#endif
    }
    ReadLocalSetTaxStats* diagnostics() const {
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
        return settax_stats;
#else
        return nullptr;
#endif
    }
};

#if TOMO_READ_LOCAL_SET_TAX_VARIANT != 3
static_assert(sizeof(ReadLocalRetireSink) == 3 * sizeof(void*),
              "the shipped retire sink is context + defer + block cache and nothing else");
#endif

}  // namespace tomo
rg: src/net/client.h: No such file or directory (os error 2)
src/net/rob.h:711:        ReadLocalRobState* state = new (std::nothrow) ReadLocalRobState;
src/net/conn.h:406:    char* reserve_fill(size_t len) { return fill_buf().reserve(len); }
src/store/flatstore.h:737:    bool prepare_read_local() { return ensure_read_local_store_state(); }
src/store/flatstore.h:1995:            chunk->bytes.reserve(kSnapshotChunkBytes);
src/store/flatstore.h:3766:    // the owner writes it only in atomic_ensure_pending / ensure_read_local_store_state /

exec
/bin/bash -lc "cat src/core/genthread.h
sed -n '1,105p' src/cmd/t_server.cc
rg -n 'void appendf|appendf\\(' src/cmd/t_server.cc | head -6
rg -n '__attribute__\\(\\(.*noinline|\\[\\[gnu::.*noinline|gnu::cold' src/core src/cmd | head -30
rg -n 'read_local_active_threads|read_local_thread_' tests src --glob '"'!CODEX-OUT.md'"'
rg -n 'static_assert\\(sizeof\\((Op|Client|Rob|AtomicEntry)' src" in /home/user/Projects/cx-rl2s
 succeeded in 0ms:
// Boot-selected fused runtime. Kept out of main.cc so its template instantiations cannot perturb
// the split loop translation unit's optimizer decisions.
#pragma once

#include <memory>
#include <vector>

namespace tomo {

class AofReplayPlan;
class LateUnixListener;
class Server;
class ShutdownReportFinalLine;
class SnapshotLoadPlan;
class TlsContext;

int run_fused_server(Server& server, const SnapshotLoadPlan* aof_base_plan,
                     const std::vector<std::unique_ptr<AofReplayPlan>>& aof_plans,
                     const SnapshotLoadPlan* load_plan, TlsContext* tls_context,
                     LateUnixListener& unix_listener,
                     ShutdownReportFinalLine& final_report);

// Split placement with shard-less fused-capable IO and the existing owner role loop.
int run_split_read_local_server(Server& server, const SnapshotLoadPlan* aof_base_plan,
                     const std::vector<std::unique_ptr<AofReplayPlan>>& aof_plans,
                     const SnapshotLoadPlan* load_plan, TlsContext* tls_context,
                     LateUnixListener& unix_listener,
                     ShutdownReportFinalLine& final_report);

}  // namespace tomo
// t_server.cc — Redis client/tool compatibility, introspection, and cross-shard keyspace commands.
//
// Connection-local handlers use a thread-local context bound for the duration of the synchronous
// IO-thread call. Client metadata lives in a cold, locked catalog here rather than enlarging the
// 1984-byte Client. Store handlers still receive only (Shard&, Op&) and never touch a socket.
#include "command.h"
#include "acl.h"
#include "auth.h"
#include "cmdmeta.h"
#include "debug.h"
#include "debug_sleep.h"
#include "info_stats.h"
#include "scripting.h"
#include "server_tail.h"
#include "slowlog.h"
#include "../base/alloc.h"
#include "../core/server.h"
#include "../core/lbsignals.h"
#include "../core/pubsub_event.h"
#include "../core/thread.h"
#include "../exec/op.h"
#include "../net/conn.h"
#include "../net/resp.h"
#include "../store/kvobj.h"
#include "../store/eviction.h"
#include "../snapshot/snapshot.h"
#include "multi.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/socket.h>

namespace tomo {
namespace {

constexpr const char* kVersion = "0.1-cpp";
constexpr uint64_t kScanInnerMask = (uint64_t{1} << 56) - 1;
constexpr uint64_t kMaxInnerCursor = (uint64_t{1} << 33) - 1;

Server* g_server = nullptr;
thread_local Client* g_client = nullptr;
thread_local ThreadCtx* g_thread = nullptr;
uint64_t g_started_monotonic_ns = 0;

bool eq_icase(Slice s, const char* lit) {
    const size_t n = std::strlen(lit);
    if (s.n != n) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char a = static_cast<unsigned char>(s.p[i]);
        unsigned char b = static_cast<unsigned char>(lit[i]);
        if (a >= 'a' && a <= 'z') a -= 'a' - 'A';
        if (b >= 'a' && b <= 'z') b -= 'a' - 'A';
        if (a != b) return false;
    }
    return true;
}

bool parse_u64(Slice s, uint64_t& out) {
    if (!s.n) return false;
    uint64_t value = 0;
    for (uint32_t i = 0; i < s.n; i++) {
        const char ch = s.p[i];
        if (ch < '0' || ch > '9') return false;
        const uint32_t digit = static_cast<uint32_t>(ch - '0');
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

bool parse_i64_slice(Slice s, int64_t& out) {
    if (!s.n || s.n > 20) return false;
    uint32_t i = 0;
    bool negative = false;
    if (s.p[0] == '-' || s.p[0] == '+') { negative = s.p[0] == '-'; i = 1; }
    if (i >= s.n) return false;
    uint64_t value = 0;
    for (; i < s.n; i++) {
        const char ch = s.p[i];
        if (ch < '0' || ch > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(ch - '0');
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
        value = value * 10 + digit;
    }
    const uint64_t limit = negative ? (uint64_t{1} << 63) : (uint64_t{1} << 63) - 1;
    if (value > limit) return false;
    // INT64_MIN has no positive counterpart, so negating the cast of 2^63 is undefined (UBSAN
    // catches it on "WAIT -9223372036854775808 0"). Name the value instead of computing it.
    out = negative ? (value == (uint64_t{1} << 63) ? std::numeric_limits<int64_t>::min()
149:void appendf(std::string& out, const char* fmt, ...) {
261:    appendf(out,
1982:        appendf(body, "# Server\r\nredis_version:%s\r\ntomokv_version:%s\r\nredis_mode:standalone\r\n"
2013:                appendf(body,
2021:        appendf(body, "read_local_active_threads:%u\r\n", active_readers);
2023:            appendf(body,
src/core/ex_loop.h:1153:    __attribute__((noinline, cold))
src/core/ex_loop.h:1187:    __attribute__((noinline, cold)) void debug_fanout_stall_local() {
src/core/ex_loop.h:2631:    __attribute__((noinline, cold))
src/core/ex_loop.h:3099:    __attribute__((noinline)) bool parked_predecessor_in(const std::deque<Task>& parked,
src/core/io_loop.h:5341:    __attribute__((noinline))
src/cmd/acl.inc:620:__attribute__((noinline)) AclDeniedReason acl_check_permission(
src/cmd/acl.inc:1196:__attribute__((cold, noinline)) bool acl_finish_dispatch_denial(
src/core/pubsub.inc:950:    __attribute__((noinline)) uint32_t pubsub_pass_flush() {
src/core/pubsub.inc:1049:    __attribute__((noinline)) PubSubStartResult pubsub_begin_pending(
src/core/pubsub.inc:1293:    __attribute__((noinline, noipa)) PubSubStartResult pubsub_start_command(Client* client,
src/core/server.h:3012:    __attribute__((noinline, cold)) static void debug_stall_us(uint32_t microseconds) {
tests/rlcache_churn.py:169:    if _lib.info_int(ctl, "server", "read_local_active_threads") == 0:
src/cmd/t_server.cc:2014:                        "read_local_thread_%u:role=%s,shards=%u,active=%u,"
src/cmd/t_server.cc:2021:        appendf(body, "read_local_active_threads:%u\r\n", active_readers);
tests/rl2s.py:33:        if int(info["read_local_active_threads"]) == int(info["io_threads"]):
tests/rl2s.py:40:        if key.startswith("read_local_thread_"):
tests/rl2s.py:42:            result[int(key.removeprefix("read_local_thread_"))] = row
tests/rl2s.py:47:    require(len(io) == int(info["read_local_active_threads"]),
tests/read_local_lane.py:112:    if server.get("read_local") != "1" or int(server["read_local_active_threads"]) == 0:
src/exec/op.h:463:static_assert(sizeof(Op) == 336, "Op grew: pay for it inside the struct or re-run the 64c A/B");
src/net/conn.h:905:static_assert(sizeof(Client) == 1984, "Client grew: re-check the io-hot line packing and idle RSS");
src/net/rob.h:1098:static_assert(sizeof(Rob<64>) == 192, "Rob<64> layout changed");
src/store/atomic_mvcc.h:70:static_assert(sizeof(AtomicEntry) == 144);

exec
/bin/bash -lc "sed -n '140,167p' src/cmd/t_server.cc
sed -n '1675,1730p' src/cmd/t_server.cc
nl -ba src/main.cc | sed -n '516,588p'
nl -ba src/core/io_loop.h | sed -n '419,445p'
nl -ba src/core/io_loop.h | sed -n '501,532p'
nl -ba src/core/io_loop.h | sed -n '3914,3935p'
rg -n 'ensure_read_local_store_state|ReadLocalStoreState.*new|new.*ReadLocalStoreState' src
rg -n 'read_local.*alloc|arm_read_local|try_arm_read_local|ensure_read_local' src/net/rob.h src/core/io_loop.h" in /home/user/Projects/cx-rl2s
 exited 1 in 0ms:
}

std::string lower_name(const char* name) {
    std::string out(name);
    for (char& ch : out)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    return out;
}

void appendf(std::string& out, const char* fmt, ...) {
    char stack[512];
    va_list ap;
    va_start(ap, fmt);
    va_list copy;
    va_copy(copy, ap);
    int n = std::vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(copy); return; }
    if (static_cast<size_t>(n) < sizeof(stack)) out.append(stack, static_cast<size_t>(n));
    else {
        std::vector<char> buf(static_cast<size_t>(n) + 1);
        std::vsnprintf(buf.data(), buf.size(), fmt, copy);
        out.append(buf.data(), static_cast<size_t>(n));
    }
    va_end(copy);
}

struct ClientMeta {
        return std::numeric_limits<uint64_t>::max();
    return object_bytes + keys * overhead;
}

bool info_section(Op& op, const char* wanted, bool included_by_default = true) {
    // EVERYTHING is the reference's alias for ALL plus module-generated sections. We load no
    // modules, so the two are identical here -- but omitting it made `INFO everything` match no
    // section at all and return an EMPTY reply, where the reference returns every section.
    if (op.argc() == 1) return included_by_default;
    for (uint32_t i = 1; i < op.argc(); i++) {
        if (eq_icase(op.arg(i), "ALL") || eq_icase(op.arg(i), "EVERYTHING") ||
            eq_icase(op.arg(i), wanted) ||
            (included_by_default && eq_icase(op.arg(i), "DEFAULT"))) return true;
    }
    return false;
}

void cmd_flip(Shard&, Op& op) {
    if (!g_server) { reply_err(op.sink(), "ERR server is not initialized"); return; }
    if (op.argc() != 1) {
        // The three-argument mutation form is intercepted by IoLoop and completed asynchronously.
        // Reaching the ordinary local handler means the grammar was neither report nor mutation.
        reply_err(op.sink(), "ERR wrong number of arguments for 'flip' command");
        return;
    }
    const FlipReport report = g_server->flip_report();
    auto sink = op.sink();
    reply_map_header(sink, 12, op.resp3());
    reply_bulk(sink, Slice("live_io", 7));
    reply_int(sink, report.live_io);
    reply_bulk(sink, Slice("live_ex", 7));
    reply_int(sink, report.live_ex);
    reply_bulk(sink, Slice("target_io", 9));
    reply_int(sink, report.target_io);
    reply_bulk(sink, Slice("target_ex", 9));
    reply_int(sink, report.target_ex);
    reply_bulk(sink, Slice("smt_mode", 8));
    reply_int(sink, report.smt_mode);
    reply_bulk(sink, Slice("unit_threads", 12));
    reply_int(sink, report.unit_threads);
    reply_bulk(sink, Slice("bucket_min", 10));
    reply_int(sink, report.bucket_min);
    reply_bulk(sink, Slice("bucket_max", 10));
    reply_int(sink, report.bucket_max);
    reply_bulk(sink, Slice("client_min", 10));
    reply_int(sink, report.client_min);
    reply_bulk(sink, Slice("client_max", 10));
    reply_int(sink, report.client_max);
    reply_bulk(sink, Slice("last_transfers", 14));
    reply_int(sink, report.last_transfers);
    reply_bulk(sink, Slice("moving", 6));
    reply_bool(sink, report.moving, op.resp3());
}

void cmd_info(Shard&, Op& op) {
    std::string body;
   516	                for (uint32_t i = 0; i < nthreads; i++)
   517	                    srv.thread(i).stop_flag().store(true, std::memory_order_relaxed);
   518	            };
   519	            if (!self.init_task_inbox_local(srv.placement().ifid_threads(),
   520	                                            srv.placement().ex_threads())) {
   521	                boot_failed("task inbox initialization");
   522	                return;
   523	            }
   524	            // Provision dormant EX first; active IO then republishes its own ring as the current
   525	            // role endpoint. No runtime conversion can fail later for lack of a ring. A failure
   526	            // here must not let the thread vanish silently: the join below waits for it, and the
   527	            // other threads would keep serving with one reuseport listener missing.
   528	            if (!exs[tid].init(&srv, &self, true)) {
   529	                boot_failed("executor loop provisioning");
   530	                return;
   531	            }
   532	            // Dormant, exactly like the executor pool above: the SO_REUSEPORT listeners and the
   533	            // AOF writer binding are opened by activate() in the role loop below instead of here,
   534	            // which is what leaves the loop cold enough to accept the transferred unix fd.
   535	            if (!ios[tid].init(&srv, &self, cfg.bind_addr, cfg.port, -1,
   536	                               tls_context.get(), true)) {
   537	                boot_failed("io loop provisioning");
   538	                return;
   539	            }
   540	            if (tid == unix_owner && unix_listener.fd() >= 0) {
   541	                if (!ios[tid].attach_listener(unix_listener.fd())) {
   542	                    boot_failed("unix listener attach");
   543	                    return;
   544	                }
   545	                (void)unix_listener.release_fd();
   546	            }
   547	            self.bind_io_role_hooks(
   548	                &ios[tid],
   549	                [](void* p) { return static_cast<IoLoop*>(p)->prepare_activation(); },
   550	                [](void* p) { static_cast<IoLoop*>(p)->cancel_prepared_activation(); });
   551	            self.bind_client_registration_hooks(
   552	                [](void* p, Client* client) {
   553	                    return static_cast<IoLoop*>(p)->prepare_client_registration(client);
   554	                },
   555	                [](void* p, Client* client) {
   556	                    static_cast<IoLoop*>(p)->cancel_client_registration(client);
   557	                });
   558	            self.bind_client_capacity_hook(
   559	                [](void* p, uint32_t incoming) {
   560	                    return static_cast<IoLoop*>(p)->prepare_client_transfer_capacity(incoming);
   561	                });
   562	            for (;;) {
   563	                if (self.stop_flag().load(std::memory_order_relaxed)) break;
   564	                const Role role = self.role();
   565	                if (role == Role::Ifid) {
   566	                    if (!ios[tid].activate()) std::abort();
   567	                    self.publish_ready_role(Role::Ifid);
   568	                    ios[tid].run();
   569	                    self.publish_ready_role(Role::Idle);
   570	                    if (!self.stop_flag().load(std::memory_order_relaxed)) ios[tid].deactivate();
   571	                } else if (role == Role::Ex) {
   572	                    exs[tid].activate();
   573	                    self.publish_ready_role(Role::Ex);
   574	                    exs[tid].run();
   575	                    self.publish_ready_role(Role::Idle);
   576	                } else {
   577	                    std::this_thread::yield();
   578	                }
   579	            }
   580	        });
   581	
   582	    if (cfg.port) std::printf("listening on %s:%u\n", cfg.bind_addr, cfg.port);
   583	    if (cfg.tls_port) std::printf("listening with TLS on %s:%u\n", cfg.bind_addr, cfg.tls_port);
   584	    if (unix_listener.bound()) std::printf("listening on unix:%s\n", cfg.unixsocket);
   585	    std::fflush(stdout);
   586	
   587	    // The automatic split controller has exactly one writer: this main/monitor thread. Worker
   588	    // loops only publish owner-local counters and execute the unchanged FLIP stage machine. With
   419	        }
   420	        if (tls_context_) {
   421	            if (has_unix) run_loop<true, true, false, false, Pipeline>();
   422	            else run_loop<false, true, false, false, Pipeline>();
   423	        } else {
   424	            if (has_unix) run_loop<true, false, false, false, Pipeline>();
   425	            else run_loop<false, false, false, false, Pipeline>();
   426	        }
   427	    }
   428	
   429	    void run() {
   430	        if (srv_->cfg().overlap == 1) run_split<1>();
   431	        else                                  run_split<0>();
   432	    }
   433	
   434	    void bind_fused_executor(FusedExLoop* executor) {
   435	        fused_executor_ = executor;
   436	    }
   437	
   438	    template <bool TargetedIfid>
   439	    void fused_executor_completion(Client* client) {
   440	        if (!client || client->dead()) return;
   441	        enqueue_serve(client);
   442	        mark_active_known<TargetedIfid>(client);
   443	    }
   444	
   445	    void run_fused();
   501	        void* catalog = nullptr;
   502	        void* routing = nullptr;
   503	    };
   504	
   505	    friend bool multi_dispatch_entry(IoLoop&, Client&, Op&, uint32_t);
   506	    friend bool multi_dispatch_entry_iofused(IoLoop&, Client&, Op&, uint32_t);
   507	    template <bool IofusedPrivateQueue>
   508	    friend bool multi_dispatch_entry_impl(IoLoop&, Client&, Op&, uint32_t);
   509	    friend bool auth_dispatch_entry(IoLoop&, Client&, Op&, uint32_t);
   510	    friend bool acl_dispatch_entry(IoLoop&, Client&, Op&, uint32_t, uint8_t);
   511	    friend bool acl_finish_dispatch_denial(IoLoop&, Client&, Op&, uint32_t,
   512	                                           AclDeniedReason, uint32_t);
   513	    friend void acl_command_entry(IoLoop&, Client&, Op&);
   514	    friend void acl_broadcast_user_change(IoLoop&, uint32_t, const AclPerm*, bool);
   515	    friend void multi_retire_entry(IoLoop&, Client&, Op&);
   516	    friend uint32_t multi_owner_pass_entry(IoLoop&);
   517	    friend uint32_t multi_owner_pass_entry_iofused(IoLoop&);
   518	    template <bool IofusedPrivateQueue>
   519	    friend uint32_t multi_owner_pass_entry_impl(IoLoop&);
   520	    friend uint32_t multi_owner_reap_entry(IoLoop&);
   521	    friend void multi_close_entry(IoLoop&, Client&);
   522	    friend void multi_shutdown_entry(IoLoop&);
   523	    friend void notify_retire_batch_entry(IoLoop&, NotifyBatch*, uint64_t);
   524	#include "pubsub.inc"
   525	
   526	    template <bool HasUnix, bool HasTls, bool kEp, bool Fused = false,
   527	              uint8_t Pipeline = 0>
   528	    void run_loop() {
   529	        static_assert(Pipeline <= 2);
   530	        if constexpr (Fused && Pipeline == 1) {
   531	            run_fused_iofused_loop<HasUnix, HasTls, kEp, false>();
   532	            return;
  3914	    template <bool NoBorrow, uint32_t BatchOps = 0, bool IoPipe = false,
  3915	              bool BufferedIfid = false, bool TargetedIfid = false,
  3916	              bool SuppressOrdinaryActiveMark = false,
  3917	              bool IofusedPrivateQueue = false>
  3918	    DispatchResult parse_and_dispatch(
  3919	        Client* c, IfidPipelineBatch* pipeline_batch = nullptr) {
  3920	        static constexpr bool Fused =
  3921	            BatchOps == kGenthreadIfidBatchOps &&
  3922	            !IoPipe && !BufferedIfid && !TargetedIfid &&
  3923	            !SuppressOrdinaryActiveMark && !IofusedPrivateQueue;
  3924	        [[maybe_unused]] const bool read_local_enabled =
  3925	            Fused && __builtin_expect(srv_->read_local_enabled(), false);
  3926	        Client& conn = *c;
  3927	        Rob<kRobWindow>& rob = c->rob();
  3928	        LoopSignals& sig = self_->sig();
  3929	        const uint32_t pass_rpos = conn.rpos();
  3930	        const char* const pass_rbuf = conn.rbuf();
  3931	        const uint32_t pass_rlen = conn.rlen();
  3932	        const uint32_t self_id = self_->id();
  3933	        auto task_free_slots = [&](ThreadCtx& owner) {
  3934	            if constexpr (IofusedPrivateQueue) {
  3935	                return owner.iofused_task_free_slots(self_id);
src/store/flatstore_atomic.inc:890:    bool ensure_read_local_store_state() {
src/store/flatstore_atomic.inc:892:        ReadLocalStoreState* extended = new (std::nothrow) ReadLocalStoreState;
src/store/flatstore.h:737:    bool prepare_read_local() { return ensure_read_local_store_state(); }
src/store/flatstore.h:3766:    // the owner writes it only in atomic_ensure_pending / ensure_read_local_store_state /

codex
The new loop work is excluded from the off-arm instantiations by `if constexpr (Fused)`. The only data-structure growth I found is in the optional thread sidecar, which 2s/off never allocates. I’ll put the INFO diagnostics behind the enabled predicate and move their scratch buffer into a non-inlined helper, so off-arm INFO calls neither format the rows nor reserve that buffer.
apply patch
patch: completed
/home/user/Projects/cx-rl2s/src/cmd/t_server.cc
/home/user/Projects/cx-rl2s/src/core/genthread.h
/home/user/Projects/cx-rl2s/src/core/rl2s.cc
diff --git a/src/cmd/t_server.cc b/src/cmd/t_server.cc
index 2d5c81bfdb1c5ee1dcedddf26af8e1801a2ea4d3..66784bda75358a60212f2a3a0c1c4904d9f5f39e
--- a/src/cmd/t_server.cc
+++ b/src/cmd/t_server.cc
@@ -14,6 +14,7 @@
 #include "server_tail.h"
 #include "slowlog.h"
 #include "../base/alloc.h"
+#include "../core/genthread.h"
 #include "../core/server.h"
 #include "../core/lbsignals.h"
 #include "../core/pubsub_event.h"
@@ -1992,33 +1993,10 @@
                 static_cast<unsigned>(g_server ? g_server->cfg().port : 0),
                 static_cast<unsigned long long>(uptime),
                 static_cast<unsigned long long>(uptime / 86400));
-        uint32_t active_readers = 0;
-        if (g_server) {
-            uint32_t owned[kMaxThreads] = {};
-            // Read the published ownership directory, never a vector FLIP may be rewriting.
-            for (uint32_t sid = 0; sid < g_server->nshards(); sid++)
-                owned[g_server->worker_of_shard(static_cast<int32_t>(sid))]++;
-            for (uint32_t tid = 0; tid < g_server->nthreads(); tid++) {
-                const ThreadCtx& thread = g_server->thread(tid);
-                const bool active = thread.read_local_lane_active();
-                active_readers += active;
-                const Role role = thread.role();
-                const char* name = role == Role::Idle ? "idle"
-                    : g_server->thread_mode() == ThreadMode::Fused ? "unified"
-                    : role == Role::Ifid ? "ifid" : "ex";
-                const ReadLocalStats* stats = g_server->read_local_enabled()
-                    ? &thread.read_local_stats() : nullptr;
-                // These per-thread totals are lifetime counters; INFO stats retains its existing
-                // RESETSTAT-relative aggregate counters. FLIP does not reset either sidecar.
-                appendf(body,
-                        "read_local_thread_%u:role=%s,shards=%u,active=%u,"
-                        "hits_total=%llu,mget_hits_total=%llu\r\n",
-                        tid, name, owned[tid], active ? 1u : 0u,
-                        static_cast<unsigned long long>(stats ? stats->hits : 0),
-                        static_cast<unsigned long long>(stats ? stats->mget_local_hits : 0));
-            }
-        }
-        appendf(body, "read_local_active_threads:%u\r\n", active_readers);
+        // Guard the CALL, including all argument evaluation. The cold non-inlined helper owns
+        // its scratch buffers so the disabled INFO path keeps its old output and stack storage.
+        if (g_server && g_server->read_local_enabled())
+            append_read_local_thread_info(body, *g_server);
         if (g_server && g_server->thread_mode() == ThreadMode::Fused) {
             appendf(body,
                     "fused_threads:%u\r\nclient_threads:%u\r\nowner_threads:%u\r\n"
diff --git a/src/core/genthread.h b/src/core/genthread.h
index c47b788f303c3a7e7f482d5ddc9f5a580a0a9bbb..e874192a1f6c2412955de097a7cd14587582ee0f
--- a/src/core/genthread.h
+++ b/src/core/genthread.h
@@ -3,6 +3,7 @@
 #pragma once
 
 #include <memory>
+#include <string>
 #include <vector>
 
 namespace tomo {
@@ -27,4 +28,7 @@
                      LateUnixListener& unix_listener,
                      ShutdownReportFinalLine& final_report);
 
+// INFO-only diagnostics. The caller must test read_local_enabled() before this call.
+void append_read_local_thread_info(std::string& body, const Server& server);
+
 }  // namespace tomo
diff --git a/src/core/rl2s.cc b/src/core/rl2s.cc
index d942a2e0f0f948a54a3f9becd0e2e32efb231a98..274b33481ca4d2e1e9e9b06d9d4dc9da575090e8
--- a/src/core/rl2s.cc
+++ b/src/core/rl2s.cc
@@ -305,4 +305,40 @@
     return 0;
 }
 
+// Keep the scratch arrays out of cmd_info's frame, including with LTO. Only an armed INFO SERVER
+// request reaches this helper; the boot/runtime paths install no reporting callback or sidecar.
+__attribute__((noinline, cold))
+void append_read_local_thread_info(std::string& body, const Server& srv) {
+    uint32_t active_readers = 0;
+    uint32_t owned[kMaxThreads] = {};
+    // Read the published ownership directory, never a vector FLIP may be rewriting.
+    for (uint32_t sid = 0; sid < srv.nshards(); sid++)
+        owned[srv.worker_of_shard(static_cast<int32_t>(sid))]++;
+    char row[192];  // fixed grammar, including full-width uint32/uint64 counters
+    for (uint32_t tid = 0; tid < srv.nthreads(); tid++) {
+        const ThreadCtx& thread = srv.thread(tid);
+        const bool active = thread.read_local_lane_active();
+        active_readers += active;
+        const Role role = thread.role();
+        const char* name = role == Role::Idle ? "idle"
+            : srv.thread_mode() == ThreadMode::Fused ? "unified"
+            : role == Role::Ifid ? "ifid" : "ex";
+        const ReadLocalStats& stats = thread.read_local_stats();
+        // Lifetime counters survive FLIP and RESETSTAT; INFO stats retains its existing
+        // RESETSTAT-relative aggregate counters.
+        const int n = std::snprintf(row, sizeof(row),
+            "read_local_thread_%u:role=%s,shards=%u,active=%u,"
+            "hits_total=%llu,mget_hits_total=%llu\r\n",
+            tid, name, owned[tid], active ? 1u : 0u,
+            static_cast<unsigned long long>(stats.hits),
+            static_cast<unsigned long long>(stats.mget_local_hits));
+        if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+        body.append(row, static_cast<size_t>(n));
+    }
+    const int n = std::snprintf(row, sizeof(row), "read_local_active_threads:%u\r\n",
+                              active_readers);
+    if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+    body.append(row, static_cast<size_t>(n));
+}
+
 }  // namespace tomo

diff --git a/src/cmd/t_server.cc b/src/cmd/t_server.cc
index 2d5c81bfdb1c5ee1dcedddf26af8e1801a2ea4d3..66784bda75358a60212f2a3a0c1c4904d9f5f39e
--- a/src/cmd/t_server.cc
+++ b/src/cmd/t_server.cc
@@ -14,6 +14,7 @@
 #include "server_tail.h"
 #include "slowlog.h"
 #include "../base/alloc.h"
+#include "../core/genthread.h"
 #include "../core/server.h"
 #include "../core/lbsignals.h"
 #include "../core/pubsub_event.h"
@@ -1992,33 +1993,10 @@
                 static_cast<unsigned>(g_server ? g_server->cfg().port : 0),
                 static_cast<unsigned long long>(uptime),
                 static_cast<unsigned long long>(uptime / 86400));
-        uint32_t active_readers = 0;
-        if (g_server) {
-            uint32_t owned[kMaxThreads] = {};
-            // Read the published ownership directory, never a vector FLIP may be rewriting.
-            for (uint32_t sid = 0; sid < g_server->nshards(); sid++)
-                owned[g_server->worker_of_shard(static_cast<int32_t>(sid))]++;
-            for (uint32_t tid = 0; tid < g_server->nthreads(); tid++) {
-                const ThreadCtx& thread = g_server->thread(tid);
-                const bool active = thread.read_local_lane_active();
-                active_readers += active;
-                const Role role = thread.role();
-                const char* name = role == Role::Idle ? "idle"
-                    : g_server->thread_mode() == ThreadMode::Fused ? "unified"
-                    : role == Role::Ifid ? "ifid" : "ex";
-                const ReadLocalStats* stats = g_server->read_local_enabled()
-                    ? &thread.read_local_stats() : nullptr;
-                // These per-thread totals are lifetime counters; INFO stats retains its existing
-                // RESETSTAT-relative aggregate counters. FLIP does not reset either sidecar.
-                appendf(body,
-                        "read_local_thread_%u:role=%s,shards=%u,active=%u,"
-                        "hits_total=%llu,mget_hits_total=%llu\r\n",
-                        tid, name, owned[tid], active ? 1u : 0u,
-                        static_cast<unsigned long long>(stats ? stats->hits : 0),
-                        static_cast<unsigned long long>(stats ? stats->mget_local_hits : 0));
-            }
-        }
-        appendf(body, "read_local_active_threads:%u\r\n", active_readers);
+        // Guard the CALL, including all argument evaluation. The cold non-inlined helper owns
+        // its scratch buffers so the disabled INFO path keeps its old output and stack storage.
+        if (g_server && g_server->read_local_enabled())
+            append_read_local_thread_info(body, *g_server);
         if (g_server && g_server->thread_mode() == ThreadMode::Fused) {
             appendf(body,
                     "fused_threads:%u\r\nclient_threads:%u\r\nowner_threads:%u\r\n"
diff --git a/src/core/genthread.h b/src/core/genthread.h
index c47b788f303c3a7e7f482d5ddc9f5a580a0a9bbb..e874192a1f6c2412955de097a7cd14587582ee0f
--- a/src/core/genthread.h
+++ b/src/core/genthread.h
@@ -3,6 +3,7 @@
 #pragma once
 
 #include <memory>
+#include <string>
 #include <vector>
 
 namespace tomo {
@@ -27,4 +28,7 @@
                      LateUnixListener& unix_listener,
                      ShutdownReportFinalLine& final_report);
 
+// INFO-only diagnostics. The caller must test read_local_enabled() before this call.
+void append_read_local_thread_info(std::string& body, const Server& server);
+
 }  // namespace tomo
diff --git a/src/core/rl2s.cc b/src/core/rl2s.cc
index d942a2e0f0f948a54a3f9becd0e2e32efb231a98..274b33481ca4d2e1e9e9b06d9d4dc9da575090e8
--- a/src/core/rl2s.cc
+++ b/src/core/rl2s.cc
@@ -305,4 +305,40 @@
     return 0;
 }
 
+// Keep the scratch arrays out of cmd_info's frame, including with LTO. Only an armed INFO SERVER
+// request reaches this helper; the boot/runtime paths install no reporting callback or sidecar.
+__attribute__((noinline, cold))
+void append_read_local_thread_info(std::string& body, const Server& srv) {
+    uint32_t active_readers = 0;
+    uint32_t owned[kMaxThreads] = {};
+    // Read the published ownership directory, never a vector FLIP may be rewriting.
+    for (uint32_t sid = 0; sid < srv.nshards(); sid++)
+        owned[srv.worker_of_shard(static_cast<int32_t>(sid))]++;
+    char row[192];  // fixed grammar, including full-width uint32/uint64 counters
+    for (uint32_t tid = 0; tid < srv.nthreads(); tid++) {
+        const ThreadCtx& thread = srv.thread(tid);
+        const bool active = thread.read_local_lane_active();
+        active_readers += active;
+        const Role role = thread.role();
+        const char* name = role == Role::Idle ? "idle"
+            : srv.thread_mode() == ThreadMode::Fused ? "unified"
+            : role == Role::Ifid ? "ifid" : "ex";
+        const ReadLocalStats& stats = thread.read_local_stats();
+        // Lifetime counters survive FLIP and RESETSTAT; INFO stats retains its existing
+        // RESETSTAT-relative aggregate counters.
+        const int n = std::snprintf(row, sizeof(row),
+            "read_local_thread_%u:role=%s,shards=%u,active=%u,"
+            "hits_total=%llu,mget_hits_total=%llu\r\n",
+            tid, name, owned[tid], active ? 1u : 0u,
+            static_cast<unsigned long long>(stats.hits),
+            static_cast<unsigned long long>(stats.mget_local_hits));
+        if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+        body.append(row, static_cast<size_t>(n));
+    }
+    const int n = std::snprintf(row, sizeof(row), "read_local_active_threads:%u\r\n",
+                              active_readers);
+    if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+    body.append(row, static_cast<size_t>(n));
+}
+
 }  // namespace tomo

exec
/bin/bash -lc "nl -ba src/core/placement.h | sed -n '80,195p'
nl -ba src/core/server.h | sed -n '232,266p'
nl -ba src/core/thread.h | sed -n '144,204p'
nl -ba src/net/rob.h | sed -n '698,730p'
nl -ba src/store/flatstore_atomic.inc | sed -n '882,910p'" in /home/user/Projects/cx-rl2s
 succeeded in 0ms:
    80	        for (uint32_t d = 0; d < nd; d++) cap += topo.cpus_in(d).size();
    81	        if (total > cap) {
    82	            std::fprintf(stderr, "--ratio: %llu threads but only %llu allowed cpus\n",
    83	                         static_cast<unsigned long long>(total),
    84	                         static_cast<unsigned long long>(cap));
    85	            return false;
    86	        }
    87	        if (total > kMaxThreads) {
    88	            std::fprintf(stderr, "--ratio: %llu threads exceeds the %u-thread channel limit\n",
    89	                         static_cast<unsigned long long>(total), kMaxThreads);
    90	            return false;
    91	        }
    92	        std::vector<uint32_t> qi(nd), qe(nd);
    93	        auto spread = [nd](uint32_t count, std::vector<uint32_t>& q) {
    94	            for (uint32_t d = 0; d < nd; d++) q[d] = count / nd + (d < count % nd ? 1u : 0u);
    95	        };
    96	        spread(n_ifid, qi); spread(n_ex, qe);
    97	        // Capacity fix-up for domains of unequal size (declared topologies): shed ex first -- it is
    98	        // the most placement-indifferent role -- into the domain with the most free cpus.
    99	        for (uint32_t d = 0; d < nd; d++) {
   100	            while (qi[d] + qe[d] > topo.cpus_in(d).size()) {
   101	                uint32_t tgt = kNoDomain;
   102	                uint64_t best_room = 0;
   103	                for (uint32_t o = 0; o < nd; o++) {
   104	                    if (o == d) continue;
   105	                    const uint64_t used = qi[o] + qe[o];
   106	                    const uint64_t have = topo.cpus_in(o).size();
   107	                    if (have > used && have - used > best_room) { best_room = have - used; tgt = o; }
   108	                }
   109	                if (tgt == kNoDomain) {
   110	                    std::fprintf(stderr, "--ratio: no domain has room to rebalance\n");
   111	                    return false;
   112	                }
   113	                if (qe[d]) { qe[d]--; qe[tgt]++; }
   114	                else       { qi[d]--; qi[tgt]++; }
   115	            }
   116	        }
   117	        for (uint32_t d = 0; d < nd; d++) {
   118	            const std::vector<int>& cpus = topo.cpus_in(d);
   119	            uint32_t slot = 0;
   120	            for (uint32_t k = 0; k < qi[d]; k++) append(Role::Ifid, cpus[slot++], d);
   121	            for (uint32_t k = 0; k < qe[d]; k++) append(Role::Ex,   cpus[slot++], d);
   122	        }
   123	        return true;
   124	    }
   125	
   126	    // --place is intentionally a small, strict language. Boot configuration should fail at the
   127	    // first typo instead of accepting a partial placement and letting unpinned threads float.
   128	    bool build_explicit(const Topology& topo, const char* spec) {
   129	        clear();
   130	        if (!spec || !*spec) {
   131	            std::fprintf(stderr, "--place must contain role@cpu entries\n");
   132	            return false;
   133	        }
   134	        const char* p = spec;
   135	        while (*p) {
   136	            Role role = Role::Idle;
   137	            if (!std::strncmp(p, "ifid@", 5)) {
   138	                role = Role::Ifid; p += 5;
   139	            } else if (!std::strncmp(p, "ex@", 3)) {
   140	                role = Role::Ex; p += 3;
   141	            } else {
   142	                std::fprintf(stderr, "--place: expected ifid@cpu or ex@cpu near '%s'\n", p);
   143	                return false;
   144	            }
   145	            if (*p < '0' || *p > '9') {
   146	                std::fprintf(stderr, "--place: expected a non-negative cpu near '%s'\n", p);
   147	                return false;
   148	            }
   149	            char* end = nullptr;
   150	            const long cpu = std::strtol(p, &end, 10);
   151	            if (cpu < 0 || cpu >= CPU_SETSIZE || (*end != ',' && *end != '\0')) {
   152	                std::fprintf(stderr, "--place: invalid cpu near '%s'\n", p);
   153	                return false;
   154	            }
   155	            const uint32_t domain = topo.domain_of(static_cast<int>(cpu));
   156	            if (domain == kNoDomain) {
   157	                std::fprintf(stderr, "--place: cpu %ld is outside the affinity mask\n", cpu);
   158	                return false;
   159	            }
   160	            if (threads_.size() == kMaxThreads) {
   161	                std::fprintf(stderr, "--place exceeds the %u-thread channel limit\n", kMaxThreads);
   162	                return false;
   163	            }
   164	            append(role, static_cast<int>(cpu), domain);
   165	            if (*end == '\0') break;
   166	            p = end + 1;
   167	            if (!*p) {
   168	                std::fprintf(stderr, "--place: trailing comma\n");
   169	                return false;
   170	            }
   171	        }
   172	        return true;
   173	    }
   174	
   175	    // Shard ownership is a flat shard->EX tid table. A manual map must be complete because mixing
   176	    // an accidental omission with defaults would make a supposedly controlled placement measure a
   177	    // different layout. Runtime dispatch still reads Router's bucket owner exactly once.
   178	    bool assign_shard_homes(uint32_t nshards, const char* spec) {
   179	        shard_home_.assign(nshards, kNoThread);
   180	        if (!spec) {
   181	            for (uint32_t sid = 0; sid < nshards; sid++)
   182	                shard_home_[sid] = ex_[sid % ex_.size()];
   183	            return true;
   184	        }
   185	        if (!*spec) {
   186	            std::fprintf(stderr, "--shard-home must contain shard:thread pairs\n");
   187	            return false;
   188	        }
   189	
   190	        std::vector<bool> seen(nshards, false);
   191	        const char* p = spec;
   192	        while (*p) {
   193	            uint32_t sid = 0, tid = 0;
   194	            if (!parse_pair(p, sid, tid)) {
   195	                std::fprintf(stderr, "--shard-home: expected shard:thread pairs near '%s'\n", p);
   232	        }
   233	        if (cfg.smt_mode && !topo_.discover_thread_siblings()) {
   234	            std::fprintf(stderr,
   235	                         "fatal: --smt-mode could not read Linux thread_siblings_list topology\n");
   236	            return false;
   237	        }
   238	        bool placed = false;
   239	        if (cfg.thread_mode == ThreadMode::Fused) {
   240	            placed = placement_.build_fused(topo_, cfg.place);
   241	        } else {
   242	            // Default shape: an even io/ex split of every allowed cpu, io taking the odd one out --
   243	            // the measured 2s center (4:4-class) generalized to any core count.
   244	            uint32_t di = cfg.even_ifid, de = cfg.even_ex;
   245	            if (!cfg.place && !(di | de)) {
   246	                uint32_t n = 0;
   247	                for (uint32_t d = 0; d < topo_.ndomains(); d++)
   248	                    n += static_cast<uint32_t>(topo_.cpus_in(d).size());
   249	                di = n - n / 2; de = n / 2;
   250	            }
   251	            placed = cfg.place
   252	                ? placement_.build_explicit(topo_, cfg.place)
   253	                : placement_.build_even(topo_, di, de, cfg.smt_mode != 0);
   254	        }
   255	        if (!placed) return false;
   256	        if (!placement_.configure_smt_units(topo_, cfg.smt_mode != 0)) return false;
   257	        if (!placement_.reserve_runtime_roles(placement_.total_threads())) return false;
   258	        if (placement_.ifid_threads().empty() || placement_.ex_threads().empty()) {
   259	            std::fprintf(stderr, "placement needs at least one ifid and one ex thread\n");
   260	            return false;
   261	        }
   262	        unix_owner_tid_ = cfg.unixsocket && *cfg.unixsocket
   263	            ? placement_.ifid_threads().front() : UINT32_MAX;
   264	        if (!adjust_open_files_limit()) return false;
   265	        check_tcp_backlog_settings();
   266	        // Shard maps are resolved exactly once at boot; parsing never leaks onto a request path.
   144	    uint64_t fallback_multi = 0;
   145	    uint64_t fallback_watch = 0;
   146	    // Compatibility aggregate followed by its exhaustive sub-reasons. Keeping the aggregate lets
   147	    // existing INFO consumers continue to compute the same fallback total.
   148	    uint64_t fallback_context = 0;
   149	    uint64_t fallback_context_owner_key = 0;
   150	    uint64_t fallback_context_connection_state = 0;
   151	    uint64_t fallback_context_route = 0;
   152	    uint64_t fallback_context_keymiss_notify = 0;
   153	    uint64_t fallback_inflight_write = 0;
   154	    // ARM-ON-DEMAND TRANSIENT. Reads demoted because the connection carried writes published
   155	    // BEFORE a local read armed it -- writes the RYOW ring deliberately never recorded. Bounded
   156	    // per connection by one ROB drain and reported separately from the steady-state key conflict
   157	    // above so that the two can never be confused in a bench (DESIGN-RINGDIET.md).
   158	    uint64_t fallback_arm_transient = 0;
   159	    uint64_t fallback_atomic_pending = 0;
   160	    uint64_t fallback_missing = 0;
   161	    uint64_t fallback_typed = 0;
   162	    uint64_t fallback_expired = 0;
   163	    uint64_t fallback_seq_churn = 0;
   164	    uint64_t fallback_generation = 0;
   165	    uint64_t fallback_lane_full = 0;
   166	    // Lane ADMISSION deferrals (P128.md). Frames the armed parser left unconsumed at rpos, to be
   167	    // re-parsed by a later pass of the same thread, because the local-read lane had no room
   168	    // (defer_lane_full) or because the connection already held its fair share of a lane under
   169	    // pressure (defer_quota). Neither is a fallback: the read still completes locally, and no
   170	    // cross-thread owner task is ever created for lack of lane capacity. fallback_lane_full is
   171	    // kept for INFO continuity and is 0 by construction since the parser stopped demoting here.
   172	    uint64_t defer_lane_full = 0;
   173	    uint64_t defer_quota = 0;
   174	
   175	    // Command-level MGET telemetry. A local MGET is one hit regardless of its key count. The
   176	    // reason counters are a subset of the aggregate fallback counters above; Missing is absent
   177	    // deliberately because a stable MGET miss is a locally served nil element, not a fallback.
   178	    uint64_t mget_local_hits = 0;
   179	    uint64_t mget_fallback_multi = 0;
   180	    uint64_t mget_fallback_watch = 0;
   181	    uint64_t mget_fallback_context = 0;
   182	    uint64_t mget_fallback_context_owner_key = 0;
   183	    uint64_t mget_fallback_context_connection_state = 0;
   184	    uint64_t mget_fallback_context_route = 0;
   185	    uint64_t mget_fallback_context_keymiss_notify = 0;
   186	    uint64_t mget_fallback_inflight_write = 0;
   187	    uint64_t mget_fallback_arm_transient = 0;
   188	    uint64_t mget_fallback_atomic_pending = 0;
   189	    uint64_t mget_fallback_typed = 0;
   190	    uint64_t mget_fallback_expired = 0;
   191	    uint64_t mget_fallback_seq_churn = 0;
   192	    uint64_t mget_generation_retries = 0;
   193	    uint64_t mget_fallback_generation = 0;
   194	    uint64_t mget_fallback_lane_full = 0;
   195	
   196	    // Arm-on-demand volume: how many connections a local read armed, how many RYOW sidecars that
   197	    // cost, and how many writes were committed into a ring. A pure-write bench arm must show all
   198	    // three at zero -- that is the design's first proof obligation, stated as a number.
   199	    ReadLocalArmStats arm{};
   200	
   201	#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
   202	    // Keep temporary SET attribution off the remotely scanned quiescence-publication cache line.
   203	    alignas(64) ReadLocalSetTaxStats settax{};
   204	#endif
   698	            } else if (state.write_ring[at].hash == hash) {
   699	                return true;
   700	            }
   701	        }
   702	        return false;
   703	    }
   704	
   705	    // The RYOW write ring's sidecar. Called from the connection's own IO thread on the first
   706	    // write of an ARMED connection -- never at accept (DESIGN-RINGDIET.md) -- and directly by the
   707	    // unit test, which drives the ROB without a server. Returns false only on allocation failure;
   708	    // every caller has a safe unarmed fallback for that.
   709	    bool prepare_read_local() {
   710	        if (read_local_state_) return true;
   711	        ReadLocalRobState* state = new (std::nothrow) ReadLocalRobState;
   712	        if (!state) return false;
   713	        const uintptr_t ptr = reinterpret_cast<uintptr_t>(state);
   714	        if (ptr & kReadLocalStateTagBits) std::abort();
   715	        read_local_state_ = ptr | kReadLocalStateInactive;
   716	        if (read_local_arm_stats_) read_local_arm_stats_->sidecars++;
   717	        return true;
   718	    }
   719	
   720	    // Installed by the IO thread that owns this connection (adopt_client) and re-pointed at the
   721	    // migration ownership edge, so every increment below is a plain store by the owning thread.
   722	    void set_read_local_arm_stats(ReadLocalArmStats* stats) { read_local_arm_stats_ = stats; }
   723	
   724	    // True while reads on this connection are being demoted by the ARMING TRANSIENT rather than by
   725	    // an explicit key conflict. The parser reads it only on the demote path, to attribute the
   726	    // fallback; it is never consulted by a read that is being served.
   727	    bool read_local_arm_fence_live() const {
   728	        return read_local_arm_state_ == kReadLocalArmFence;
   729	    }
   730	
   882	    }
   883	
   884	    const ReadLocalStoreState& read_local_store_state_required() const {
   885	        const ReadLocalStoreState* state = read_local_store_state();
   886	        if (!state) std::abort();
   887	        return *state;
   888	    }
   889	
   890	    bool ensure_read_local_store_state() {
   891	        if (read_local_store_state()) return true;
   892	        ReadLocalStoreState* extended = new (std::nothrow) ReadLocalStoreState;
   893	        if (!extended) return false;
   894	        if (atomic_pending_) {
   895	            if (atomic_pending_->live || atomic_pending_->script_intent_count) std::abort();
   896	            extended->atomic = *atomic_pending_;
   897	            delete atomic_pending_;
   898	        }
   899	        extended->atomic.read_local_extended = true;
   900	        atomic_pending_ = &extended->atomic;
   901	        return true;
   902	    }
   903	
   904	    AtomicEntry* atomic_alloc_entry(uint32_t capacity, uint64_t tail_bytes) {
   905	        const uint64_t request64 = sizeof(AtomicEntry) +
   906	                                   static_cast<uint64_t>(capacity) * sizeof(KvObj*) + tail_bytes;
   907	        if (request64 > std::numeric_limits<size_t>::max()) return nullptr;
   908	        const size_t request = static_cast<size_t>(request64);
   909	        const size_t allocation = good_size(request);
   910	        const uint32_t cls = atomic_pool_class(allocation);

exec
/bin/bash -lc "rg -n 'new|malloc|calloc|realloc|reserve\\(|resize\\(|emplace|make_unique|make_shared|\\bstd::(vector|deque|string|unordered)|\\[[a-zA-Z0-9_]+\\]|init\\(|bind_|register' src/core/rl2s.cc
nl -ba src/core/ex_loop.h | sed -n '228,258p'
rg -n 'read_local_|static_assert\\(sizeof' src/main.cc src/core/config.h src/core/thread.h | tail -20
sed -n '1,42p' src/core/genthread.cc
rg -n 'EXPECT_QUICK|EXPECT_FULL|QUICK.*exit|if .*QUICK|exit 0' tests/gate.sh | tail -14" in /home/user/Projects/cx-rl2s
 succeeded in 0ms:
71:                     const std::vector<std::unique_ptr<AofReplayPlan>>& aof_plans,
90:    std::vector<std::thread> pool;
91:    std::vector<IoLoop> ios(nthreads);
92:    std::vector<FusedExLoop> executors(nthreads);
122:        pool.emplace_back([&, tid] {
126:            bind_thread_arena();
129:            if (ok) ok = executors[tid].init(&srv, &self, true);
130:            std::string local_error;
144:                ok = ios[tid].init(&srv, &self, cfg.bind_addr, cfg.port, -1,
147:                self.bind_io_role_hooks(
148:                    &ios[tid],
152:                self.bind_client_registration_hooks(
160:                self.bind_client_capacity_hook(
165:                executors[tid].activate_fused(&ios[tid].ring());
166:                ios[tid].bind_fused_executor(&executors[tid]);
167:                executors[tid].bind_fused_completion(
168:                    &ios[tid],
173:                    executors[tid].bind_read_local_demotion(
174:                        &ios[tid],
181:                self.bind_fused_executor_hooks(
182:                    &executors[tid],
196:                if (!ios[tid].activate()) {
201:                executors[tid].activate();
209:                    if (!ios[tid].activate()) std::abort();
211:                    ios[tid].run_fused();
213:                    if (!self.stop_flag().load(std::memory_order_relaxed)) ios[tid].deactivate();
215:                    executors[tid].activate();
217:                    executors[tid].run();
235:        const std::string error = boot.error();
251:            cfg.bind_addr, port, cfg.tcp_backlog, tls);
263:    std::string unix_error;
270:        if (!ios[unix_owner].attach_listener(unix_listener.fd())) {
281:        const std::string error = boot.error();
289:    if (cfg.port) std::printf("listening on %s:%u\n", cfg.bind_addr, cfg.port);
290:    if (cfg.tls_port) std::printf("listening with TLS on %s:%u\n", cfg.bind_addr, cfg.tls_port);
311:void append_read_local_thread_info(std::string& body, const Server& srv) {
313:    uint32_t owned[kMaxThreads] = {};
317:    char row[192];  // fixed grammar, including full-width uint32/uint64 counters
332:            tid, name, owned[tid], active ? 1u : 0u,
   228	    }
   229	
   230	    // Fused tenure shares the physical thread with IoLoop. IoLoop remains the published wake
   231	    // endpoint because it owns the only blocking wait; this ring still carries persistence CQEs.
   232	    void activate_fused(Ring* handoff_ring) {
   233	        static_assert(Fused);
   234	        if (!initialized_) std::abort();
   235	        // Interwoven schedules put executor-originated task/client handoffs on the network ring.
   236	        // That leaves this private ring with control/persistence SQEs; both iofused-family arms
   237	        // amortize at their shared N2 boundary. Pipeline 0 retains its existing ring ownership.
   238	        if (srv_->cfg().overlap != 0 && handoff_ring)
   239	            fused_handoff_ring_ = handoff_ring;
   240	        blocking_bind_executor(srv_, self_, &ring_);
   241	        if (read_local_enabled())
   242	            self_->bind_read_local_retire_sink(*read_local_impl().deferred.sink());
   243	        for (Shard* shard : self_->shards()) {
   244	            shard->bind_notify_pending(&notify_keyless_pending_);
   245	            if (read_local_enabled())
   246	                shard->store().configure_read_local(
   247	                    true, *read_local_impl().deferred.sink(),
   248	                    srv_->cfg().read_local_atomic_filter != 0);
   249	        }
   250	    }
   251	
   252	    void bind_fused_completion(void* context, FusedCompletionFn completion) {
   253	        static_assert(Fused);
   254	        fused_io_context_ = context;
   255	        fused_completion_ = completion;
   256	    }
   257	
   258	    void bind_read_local_demotion(void* context, ReadLocalExImpl::DemoteFn demote) {
src/core/thread.h:984:        return read_local_state_->retire_sink;
src/core/thread.h:991:    const ReadLocalRetireSink* read_local_retire_sink_or_null() const {
src/core/thread.h:992:        if (!read_local_state_ || !read_local_state_->retire_sink.defer) return nullptr;
src/core/thread.h:993:        return &read_local_state_->retire_sink;
src/core/thread.h:1000:    size_t read_local_block_cache_bytes() const {
src/core/thread.h:1001:        if (!read_local_state_) return 0;
src/core/thread.h:1002:        const KvBlockCache* cache = read_local_state_->retire_sink.block_cache;
src/core/thread.h:1203:    std::unique_ptr<ReadLocalThreadState> read_local_state_;
src/core/thread.h:1206:static_assert(sizeof(ThreadCtx) == 1408,
src/core/config.h:250:    uint8_t  read_local_prefetch_capture = 1;
src/core/config.h:253:    uint8_t  read_local_atomic_filter = 1;
src/core/config.h:421:    uint32_t read_local_interleave = 1;
src/core/config.h:429:static_assert(sizeof(Config) == 624,
src/core/config.h:725:            if (!cfg_parse_u32(next(nullptr), cfg.read_local_interleave) ||
src/core/config.h:726:                cfg.read_local_interleave > 1) {
src/core/config.h:737:            cfg.read_local_prefetch_capture = static_cast<uint8_t>(value);
src/core/config.h:745:            cfg.read_local_atomic_filter = static_cast<uint8_t>(value);
src/main.cc:216:                               Server::read_local_enabled(cfg))) {
src/main.cc:317:    if (srv.read_local_enabled()) {
src/main.cc:319:        return run_split_read_local_server(srv, aof_base_plan.get(), aof_plans, load_plan.get(),
// Unified generalized-thread runtime. This translation unit instantiates IoLoop's Fused=true
// loop methods for pipelines 0, 1, and 2; rl2s.cc also uses the overlap-0 loop for shard-less IO.
#include "genthread.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include <unistd.h>

#include "../base/alloc.h"
#include "../cmd/acl.h"
#include "../net/conn.h"
#include "../net/unix_listener.h"
#include "../persist/aof.h"
#include "../snapshot/snapshot.h"
#include "ex_loop.h"
#include "fused_boot_gate.h"
#include "io_loop.h"
#include "server.h"
#include "shutdown_report.h"

namespace tomo {
namespace {

void pin_fused_thread(int cpu) {
    if (cpu < 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

}  // namespace

void IoLoop::run_fused() {
    if (!fused_executor_) std::abort();
    const bool has_unix = unix_listen_fd_ >= 0 ||
                          (srv_->cfg().unixsocket && *srv_->cfg().unixsocket);
141:EXPECT_QUICK=327
142:EXPECT_FULL=344                 # full without the optional NIC row.
1259:  program_state "$EXPECT_QUICK"
1260:  echo; echo "GATE(quick): $PASS ok, $FAIL FAIL"; [ $FAIL -eq 0 ] || exit 1; exit 0
1497:program_state "$((EXPECT_FULL+NIC_CHECKED))"

diff --git a/src/cmd/t_server.cc b/src/cmd/t_server.cc
index 2d5c81bfdb1c5ee1dcedddf26af8e1801a2ea4d3..66784bda75358a60212f2a3a0c1c4904d9f5f39e
--- a/src/cmd/t_server.cc
+++ b/src/cmd/t_server.cc
@@ -14,6 +14,7 @@
 #include "server_tail.h"
 #include "slowlog.h"
 #include "../base/alloc.h"
+#include "../core/genthread.h"
 #include "../core/server.h"
 #include "../core/lbsignals.h"
 #include "../core/pubsub_event.h"
@@ -1992,33 +1993,10 @@
                 static_cast<unsigned>(g_server ? g_server->cfg().port : 0),
                 static_cast<unsigned long long>(uptime),
                 static_cast<unsigned long long>(uptime / 86400));
-        uint32_t active_readers = 0;
-        if (g_server) {
-            uint32_t owned[kMaxThreads] = {};
-            // Read the published ownership directory, never a vector FLIP may be rewriting.
-            for (uint32_t sid = 0; sid < g_server->nshards(); sid++)
-                owned[g_server->worker_of_shard(static_cast<int32_t>(sid))]++;
-            for (uint32_t tid = 0; tid < g_server->nthreads(); tid++) {
-                const ThreadCtx& thread = g_server->thread(tid);
-                const bool active = thread.read_local_lane_active();
-                active_readers += active;
-                const Role role = thread.role();
-                const char* name = role == Role::Idle ? "idle"
-                    : g_server->thread_mode() == ThreadMode::Fused ? "unified"
-                    : role == Role::Ifid ? "ifid" : "ex";
-                const ReadLocalStats* stats = g_server->read_local_enabled()
-                    ? &thread.read_local_stats() : nullptr;
-                // These per-thread totals are lifetime counters; INFO stats retains its existing
-                // RESETSTAT-relative aggregate counters. FLIP does not reset either sidecar.
-                appendf(body,
-                        "read_local_thread_%u:role=%s,shards=%u,active=%u,"
-                        "hits_total=%llu,mget_hits_total=%llu\r\n",
-                        tid, name, owned[tid], active ? 1u : 0u,
-                        static_cast<unsigned long long>(stats ? stats->hits : 0),
-                        static_cast<unsigned long long>(stats ? stats->mget_local_hits : 0));
-            }
-        }
-        appendf(body, "read_local_active_threads:%u\r\n", active_readers);
+        // Guard the CALL, including all argument evaluation. The cold non-inlined helper owns
+        // its scratch buffers so the disabled INFO path keeps its old output and stack storage.
+        if (g_server && g_server->read_local_enabled())
+            append_read_local_thread_info(body, *g_server);
         if (g_server && g_server->thread_mode() == ThreadMode::Fused) {
             appendf(body,
                     "fused_threads:%u\r\nclient_threads:%u\r\nowner_threads:%u\r\n"
diff --git a/src/core/genthread.h b/src/core/genthread.h
index c47b788f303c3a7e7f482d5ddc9f5a580a0a9bbb..e874192a1f6c2412955de097a7cd14587582ee0f
--- a/src/core/genthread.h
+++ b/src/core/genthread.h
@@ -3,6 +3,7 @@
 #pragma once
 
 #include <memory>
+#include <string>
 #include <vector>
 
 namespace tomo {
@@ -27,4 +28,7 @@
                      LateUnixListener& unix_listener,
                      ShutdownReportFinalLine& final_report);
 
+// INFO-only diagnostics. The caller must test read_local_enabled() before this call.
+void append_read_local_thread_info(std::string& body, const Server& server);
+
 }  // namespace tomo
diff --git a/src/core/rl2s.cc b/src/core/rl2s.cc
index d942a2e0f0f948a54a3f9becd0e2e32efb231a98..274b33481ca4d2e1e9e9b06d9d4dc9da575090e8
--- a/src/core/rl2s.cc
+++ b/src/core/rl2s.cc
@@ -305,4 +305,40 @@
     return 0;
 }
 
+// Keep the scratch arrays out of cmd_info's frame, including with LTO. Only an armed INFO SERVER
+// request reaches this helper; the boot/runtime paths install no reporting callback or sidecar.
+__attribute__((noinline, cold))
+void append_read_local_thread_info(std::string& body, const Server& srv) {
+    uint32_t active_readers = 0;
+    uint32_t owned[kMaxThreads] = {};
+    // Read the published ownership directory, never a vector FLIP may be rewriting.
+    for (uint32_t sid = 0; sid < srv.nshards(); sid++)
+        owned[srv.worker_of_shard(static_cast<int32_t>(sid))]++;
+    char row[192];  // fixed grammar, including full-width uint32/uint64 counters
+    for (uint32_t tid = 0; tid < srv.nthreads(); tid++) {
+        const ThreadCtx& thread = srv.thread(tid);
+        const bool active = thread.read_local_lane_active();
+        active_readers += active;
+        const Role role = thread.role();
+        const char* name = role == Role::Idle ? "idle"
+            : srv.thread_mode() == ThreadMode::Fused ? "unified"
+            : role == Role::Ifid ? "ifid" : "ex";
+        const ReadLocalStats& stats = thread.read_local_stats();
+        // Lifetime counters survive FLIP and RESETSTAT; INFO stats retains its existing
+        // RESETSTAT-relative aggregate counters.
+        const int n = std::snprintf(row, sizeof(row),
+            "read_local_thread_%u:role=%s,shards=%u,active=%u,"
+            "hits_total=%llu,mget_hits_total=%llu\r\n",
+            tid, name, owned[tid], active ? 1u : 0u,
+            static_cast<unsigned long long>(stats.hits),
+            static_cast<unsigned long long>(stats.mget_local_hits));
+        if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+        body.append(row, static_cast<size_t>(n));
+    }
+    const int n = std::snprintf(row, sizeof(row), "read_local_active_threads:%u\r\n",
+                              active_readers);
+    if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+    body.append(row, static_cast<size_t>(n));
+}
+
 }  // namespace tomo

exec
/bin/bash -lc "rg -n 'read-local\"|\"thread-mode\"|\"overlap\"' src/core/config.h src/cmd/t_server.cc
rg -n 'prepare_read_local\\(|read_local_arm_for|arm_read_local|read_local_write_prepare|record_read_local_write' src/net/rob.h src/core/io_loop.h
nl -ba src/core/io_loop.h | sed -n '4188,4240p'
nl -ba src/core/io_loop.h | sed -n '4570,4610p'
nl -ba src/core/io_loop.h | sed -n '552,570p'
nl -ba src/core/thread.h | sed -n '133,145p'" in /home/user/Projects/cx-rl2s
 succeeded in 0ms:
src/core/config.h:718:        else if (!std::strcmp(a, "--read-local")) {
src/cmd/t_server.cc:322:    g_config.push_back({"thread-mode", ConfigKind::Enum,
src/cmd/t_server.cc:324:    g_config.push_back({"overlap", ConfigKind::Unsigned,
src/cmd/t_server.cc:326:    g_config.push_back({"read-local", ConfigKind::Unsigned,
src/net/rob.h:486:        if (__builtin_expect(read_local_state_ == 0, false) && !prepare_read_local()) {
src/net/rob.h:709:    bool prepare_read_local() {
  4188	            }
  4189	            // Streams I0 admits only the context-free point path. Decide before ACL,
  4190	            // transactions, subscriber mode, or local/scatter lowering can publish at the real
  4191	            // ROB frontier while an older entry in this batch is still unpublished.
  4192	            if constexpr (BufferedIfid)
  4193	                if (pipeline_batch &&
  4194	                    (!pipeline_simple_point(*op) || security_check || notify_armed ||
  4195	                     srv_->flip_dispatch_paused() || conn.multi_session() != nullptr ||
  4196	                     c->subscriber_mode() || c->has_atomic_group_io())) {
  4197	                    pipeline_batch->force_coarse = true;
  4198	                    break;
  4199	                }
  4200	            if constexpr (Fused) {
  4201	                if (read_local_enabled) {
  4202	                    constexpr uint32_t kWriteHazards =
  4203	                        CmdFlags::Write | CmdFlags::SnapshotWrite |
  4204	                        CmdFlags::Transaction | CmdFlags::ScriptRoute;
  4205	                    constexpr uint32_t kNonPointRoutes =
  4206	                        CmdFlags::AllShards | CmdFlags::RandomShard |
  4207	                        CmdFlags::CursorShard | CmdFlags::ConfigRoute |
  4208	                        CmdFlags::MultiShard | CmdFlags::ScriptRoute |
  4209	                        CmdFlags::Blocking | CmdFlags::Transaction |
  4210	                        CmdFlags::StreamRoute | CmdFlags::SubcmdRoute;
  4211	                    constexpr uint32_t kReplaySensitiveClimon =
  4212	                        Server::kClimonMonitor | Server::kClimonTracking |
  4213	                        Server::kClimonReply;
  4214	                    const bool write_hazard = (spec->flags & kWriteHazards) != 0;
  4215	                    read_local_write_hazard = write_hazard;
  4216	                    const bool point_route =
  4217	                        (spec->flags & kNonPointRoutes) == 0 &&
  4218	                        spec->first_key > 0 && spec->last_key == spec->first_key &&
  4219	                        spec->key_step == 1;
  4220	                    if (point_route) {
  4221	                        op->hash = FlatStore::hash_key(
  4222	                            op->arg(static_cast<uint32_t>(spec->first_key)));
  4223	                        op->shard = srv_->router().shard_of(op->hash);
  4224	                        read_local_point_prehashed = true;
  4225	                    }
  4226	                    auto classify_owner_conflict = [&](auto&& overlaps) {
  4227	                        bool broad = false;
  4228	                        const bool conflict = rob.read_local_owner_conflicts_before(
  4229	                            rob.dispatch_id(), [&](const Op& owner) {
  4230	                                if (!overlaps(owner)) return false;
  4231	                                broad |= !read_local_owner_command_is_precise(owner);
  4232	                                return true;
  4233	                            });
  4234	                        return !conflict ? ReadLocalFallbackReason::None
  4235	                            : broad ? ReadLocalFallbackReason::ContextRoute
  4236	                                    : ReadLocalFallbackReason::ContextOwnerKey;
  4237	                    };
  4238	                    auto owner_conflict_for_hash = [&](uint64_t hash) {
  4239	                        return classify_owner_conflict([&](const Op& owner) {
  4240	                            return read_local_owner_command_touches_hash(owner, hash);
  4570	                }
  4571	            }
  4572	            if (__builtin_expect(notify_armed, false) &&
  4573	                __builtin_expect(climon_armed_gate(c, *op), false)) break;
  4574	            if (__builtin_expect(security_check, false) &&
  4575	                acl_dispatch_entry(*this, conn, *op, consumed, security_flags)) continue;
  4576	            if (__builtin_expect(srv_->flip_dispatch_paused(), false) &&
  4577	                !(spec->flags & CmdFlags::FlipAsync)) {
  4578	                // No ordinary request may create IO-local fanout or executor work after the first
  4579	                // drain acknowledgement. Leave the frame unconsumed and unpublished: TCP framing
  4580	                // keeps younger frames behind it without a ROB barrier, while FlipAsync commands
  4581	                // at the head of other connections remain reachable throughout the pause.
  4582	                c->set_flip_backpressure(true);
  4583	                break;
  4584	            }
  4585	            if constexpr (Fused) {
  4586	                // This must follow the per-frame FLIP gate above: demotion itself publishes owner
  4587	                // Tasks. Non-point routes may touch any key, so move every unresolved local read
  4588	                // before any of their lowering paths can publish.
  4589	                if (read_local_enabled && read_local_commit_before_lowering) {
  4590	                    if (read_local_demotion.active() && !read_local_write_hazard) {
  4591	                        rob.extend_current_read_local_owner();
  4592	                    }
  4593	                    read_local_demotion.commit_reads();
  4594	                }
  4595	            }
  4596	            if (__builtin_expect((spec->flags & CmdFlags::Transaction) != 0, false) ||
  4597	                __builtin_expect(conn.multi_session() != nullptr, false)) {
  4598	                if constexpr (Fused) {
  4599	                    if (read_local_enabled &&
  4600	                        __builtin_expect((spec->flags & CmdFlags::ReadLocalEligible) &&
  4601	                                         multi_session_active(conn), false))
  4602	                        self_->read_local_stats().note_fallback(
  4603	                            ReadLocalFallbackReason::Multi,
  4604	                            command_is_read_local_mget(*spec));
  4605	                }
  4606	                if constexpr (IofusedPrivateQueue) {
  4607	                    if (multi_dispatch_entry_iofused(*this, conn, *op, consumed)) continue;
  4608	                } else {
  4609	                    if (multi_dispatch_entry(*this, conn, *op, consumed)) continue;
  4610	                }
   552	            if (listen_fd_ >= 0) arm_accept(UrKind::Accept);
   553	            if constexpr (HasTls) arm_accept(UrKind::TlsAccept);
   554	            if constexpr (HasUnix) if (unix_listen_fd_ >= 0) arm_accept(UrKind::UnixAccept);
   555	        }
   556	        LoopSignals& sig = self_->sig();
   557	        while (!self_->stop_flag().load(std::memory_order_relaxed) &&
   558	               self_->role() == Role::Ifid) {
   559	            refresh_notify_config();
   560	            // ONE relaxed load per io batch. Per-batch checks are free; this is what buys the
   561	            // per-operation hooks their zero-cost-when-off property.
   562	            if (__builtin_expect(srv_->climon_armed() != climon_armed_cached_, false))
   563	                climon_refresh_armed();
   564	            const bool pause_armed = climon_pause_armed();
   565	            const bool client_cron_armed = !srv_->flip_dispatch_paused() &&
   566	                                           srv_->client_cron_armed();
   567	            const bool client_lb_signal_armed = client_lb_signal_armed_;
   568	            const bool lb_controller_armed = lb_controller_armed_;
   569	            // Placement's dense role vectors are mutated only under FLIP's global dispatch
   570	            // barrier. Do not consult them from an IO pass while that cold transaction is live.
   133	
   134	// Fused read-local telemetry is written only by the physical thread that owns this context.
   135	// INFO reads it with the same exceptional cross-thread snapshot used for LoopSignals and command
   136	// counts; keeping it plain avoids adding synchronization to the route and EX hot paths.
   137	struct ReadLocalStats {
   138	    // Command-level aggregate across locally completed reads. Local keyspace accounting is
   139	    // separate and lookup-based, just like Shard::note_keyspace_read: one MGET contributes one
   140	    // command hit here and one hit or miss per returned element below.
   141	    uint64_t hits = 0;
   142	    uint64_t keyspace_hits = 0;
   143	    uint64_t keyspace_misses = 0;
   144	    uint64_t fallback_multi = 0;
   145	    uint64_t fallback_watch = 0;

exec
/bin/bash -lc "git diff HEAD --numstat
git diff HEAD -- src/cmd/t_server.cc src/core/genthread.h src/core/thread.h
rg -n '"'^#include|(''^|[;}] *) *(inline )?(static )?thread_local'"' src/core/signal_doorbell.h src/core/ex_loop.h src/core/io_loop.h" in /home/user/Projects/cx-rl2s
 succeeded in 0ms:
1	0	Makefile
374	0	RL2S.md
7	2	src/cmd/t_server.cc
6	2	src/core/config.h
37	12	src/core/ex_loop.h
2	2	src/core/genthread.cc
11	0	src/core/genthread.h
14	2	src/core/io_loop.h
344	0	src/core/rl2s.cc
2	2	src/core/server.h
19	4	src/core/thread.h
7	3	src/main.cc
10	6	tests/bplus.py
4	1	tests/config_parser_test.cc
4	3	tests/read_local_lane.py
185	0	tests/rl2s.py
6	3	tests/rlcache_churn.py
diff --git a/src/cmd/t_server.cc b/src/cmd/t_server.cc
index 10d2501a7..66784bda7 100644
--- a/src/cmd/t_server.cc
+++ b/src/cmd/t_server.cc
@@ -14,6 +14,7 @@
 #include "server_tail.h"
 #include "slowlog.h"
 #include "../base/alloc.h"
+#include "../core/genthread.h"
 #include "../core/server.h"
 #include "../core/lbsignals.h"
 #include "../core/pubsub_event.h"
@@ -1977,8 +1978,8 @@ void cmd_info(Shard&, Op& op) {
         // stale -- and tooling depends on them. The NIC bench harness identifies the server it just
         // booted by reading process_id out of INFO, so its absence made every NIC cell fail with an
         // opaque "boot/cell FAIL" long before any measurement was taken.
-        // read_local is the EFFECTIVE lane state (fused, overlap 0, knob on) -- what a gate row
-        // must assert. CONFIG GET read-local echoes the knob even on a split boot where it is inert.
+        // read_local is the effective boot state. Actual loop entry and successful completions
+        // are separate observations: a configured but unreachable lane must be visible in INFO.
         appendf(body, "# Server\r\nredis_version:%s\r\ntomokv_version:%s\r\nredis_mode:standalone\r\n"
                       "thread_mode:%s\r\noverlap:%u\r\nthread_pipeline:%u\r\nread_local:%u\r\n"
                       "arch_bits:%zu\r\nmultiplexing_api:io_uring\r\nprocess_id:%lld\r\n"
@@ -1992,6 +1993,10 @@ void cmd_info(Shard&, Op& op) {
                 static_cast<unsigned>(g_server ? g_server->cfg().port : 0),
                 static_cast<unsigned long long>(uptime),
                 static_cast<unsigned long long>(uptime / 86400));
+        // Guard the CALL, including all argument evaluation. The cold non-inlined helper owns
+        // its scratch buffers so the disabled INFO path keeps its old output and stack storage.
+        if (g_server && g_server->read_local_enabled())
+            append_read_local_thread_info(body, *g_server);
         if (g_server && g_server->thread_mode() == ThreadMode::Fused) {
             appendf(body,
                     "fused_threads:%u\r\nclient_threads:%u\r\nowner_threads:%u\r\n"
diff --git a/src/core/genthread.h b/src/core/genthread.h
index 2ceddd0ad..e874192a1 100644
--- a/src/core/genthread.h
+++ b/src/core/genthread.h
@@ -3,6 +3,7 @@
 #pragma once
 
 #include <memory>
+#include <string>
 #include <vector>
 
 namespace tomo {
@@ -20,4 +21,14 @@ int run_fused_server(Server& server, const SnapshotLoadPlan* aof_base_plan,
                      LateUnixListener& unix_listener,
                      ShutdownReportFinalLine& final_report);
 
+// Split placement with shard-less fused-capable IO and the existing owner role loop.
+int run_split_read_local_server(Server& server, const SnapshotLoadPlan* aof_base_plan,
+                     const std::vector<std::unique_ptr<AofReplayPlan>>& aof_plans,
+                     const SnapshotLoadPlan* load_plan, TlsContext* tls_context,
+                     LateUnixListener& unix_listener,
+                     ShutdownReportFinalLine& final_report);
+
+// INFO-only diagnostics. The caller must test read_local_enabled() before this call.
+void append_read_local_thread_info(std::string& body, const Server& server);
+
 }  // namespace tomo
diff --git a/src/core/thread.h b/src/core/thread.h
index 738fb43ec..adea9605d 100644
--- a/src/core/thread.h
+++ b/src/core/thread.h
@@ -292,13 +292,20 @@ struct ReadLocalStats {
     }
 };
 
-// Fused read-local publication and telemetry are absent from baseline ThreadCtx allocations. The
+// Read-local publication and telemetry are absent from baseline ThreadCtx allocations. The
 // lone owning pointer is placed in ThreadCtx's established tail padding below.
 struct ReadLocalThreadState {
     std::atomic<uint64_t> tick{0};
     ReadLocalRetireSink retire_sink{};
     ReadLocalStats stats{};
+    // Published only on actual reader-loop entry/exit, including FLIP. INFO must distinguish
+    // an enabled boot knob from a live parser/executor lane. No per-operation publication.
+    std::atomic<bool> lane_active{false};
 };
+#if TOMO_READ_LOCAL_SET_TAX_VARIANT != 3
+static_assert(sizeof(ReadLocalThreadState) == 368,
+              "lane activation adds one alignment word only to the optional sidecar");
+#endif
 
 class ThreadCtx {
 public:
@@ -919,7 +926,7 @@ public:
     }
     bool parked() const { return parked_.load(std::memory_order_acquire); }
 
-    // Fused read-local QSBR publication. A reader publishes once at the coarse rotation boundary,
+    // Read-local QSBR publication. A reader publishes once at the coarse rotation boundary,
     // never per operation. Parked shares this word with the tick so a grace scan cannot accept a
     // stale separate parked=true after the thread has resumed probing foreign stores. Sequential
     // consistency orders the park/resume edge with that scan; it is paid only at rotation/park.
@@ -953,6 +960,14 @@ public:
         if (!read_local_state_) std::abort();
         return read_local_state_->tick.load(std::memory_order_seq_cst);
     }
+    void set_read_local_lane_active(bool active) {
+        if (!read_local_state_ || !read_local_state_->retire_sink.defer) std::abort();
+        read_local_state_->lane_active.store(active, std::memory_order_release);
+    }
+    bool read_local_lane_active() const {
+        return read_local_state_ &&
+               read_local_state_->lane_active.load(std::memory_order_acquire);
+    }
     static bool read_local_publication_parked(uint64_t publication) {
         return (publication & kReadLocalParkedBit) != 0;
     }
@@ -968,8 +983,8 @@ public:
         if (!read_local_state_ || !read_local_state_->retire_sink.defer) std::abort();
         return read_local_state_->retire_sink;
     }
-    // The nullable form, for the shard-ownership edge in Server. A thread has no sink in split
-    // mode and on a fused boot with the lane disarmed; the edge must be able to ask without
+    // The nullable form, for the shard-ownership edge in Server. A thread has no sink with the
+    // lane disarmed; the edge must be able to ask without
     // knowing which, and must be able to tell "disarmed" from "armed but unbound" -- the latter is
     // a boot-order defect, and the caller aborts on it rather than moving a shard to an owner that
     // cannot retire for it.
src/core/signal_doorbell.h:8:#include <cerrno>
src/core/signal_doorbell.h:9:#include <climits>
src/core/signal_doorbell.h:10:#include <csignal>
src/core/signal_doorbell.h:11:#include <cstdint>
src/core/signal_doorbell.h:12:#include <poll.h>
src/core/signal_doorbell.h:13:#include <sys/eventfd.h>
src/core/signal_doorbell.h:14:#include <unistd.h>
src/core/ex_loop.h:15:#include <algorithm>
src/core/ex_loop.h:16:#include <cstdint>
src/core/ex_loop.h:17:#include <cstdio>
src/core/ex_loop.h:18:#include <cstdlib>
src/core/ex_loop.h:19:#include <ctime>
src/core/ex_loop.h:20:#include <deque>
src/core/ex_loop.h:21:#include <type_traits>
src/core/ex_loop.h:22:#include "server.h"
src/core/ex_loop.h:23:#include "signal.h"
src/core/ex_loop.h:24:#include "genthread_pipeline.h"
src/core/ex_loop.h:25:#include "read_local.h"
src/core/ex_loop.h:26:#include "../net/conn.h"
src/core/ex_loop.h:27:#include "../net/resp.h"
src/core/ex_loop.h:28:#include "../net/uring.h"
src/core/ex_loop.h:29:#include "../net/wb.h"
src/core/ex_loop.h:30:#include "../cmd/command.h"
src/core/ex_loop.h:31:#include "../cmd/blocking.h"
src/core/ex_loop.h:32:#include "../cmd/multi.h"
src/core/ex_loop.h:33:#include "../cmd/notify.h"
src/core/ex_loop.h:34:#include "../cmd/slowlog.h"
src/core/ex_loop.h:35:#include "../cmd/xshard.h"
src/core/ex_loop.h:36:#include "../persist/aof.h"
src/core/io_loop.h:17:#include <array>
src/core/io_loop.h:18:#include <deque>
src/core/io_loop.h:19:#include <arpa/inet.h>
src/core/io_loop.h:20:#include <netinet/in.h>
src/core/io_loop.h:21:#include <netinet/tcp.h>
src/core/io_loop.h:22:#include <poll.h>
src/core/io_loop.h:23:#include <sys/socket.h>
src/core/io_loop.h:24:#include <unistd.h>
src/core/io_loop.h:25:#include <cstdio>
src/core/io_loop.h:26:#include <cstdlib>
src/core/io_loop.h:27:#include <new>
src/core/io_loop.h:28:#include <memory>
src/core/io_loop.h:29:#include <string>
src/core/io_loop.h:30:#include <type_traits>
src/core/io_loop.h:31:#include <unordered_map>
src/core/io_loop.h:32:#include <unordered_set>
src/core/io_loop.h:33:#include <vector>
src/core/io_loop.h:34:#include "server.h"
src/core/io_loop.h:35:#include "iopipe_pipeline.h"
src/core/io_loop.h:36:#include "signal.h"
src/core/io_loop.h:37:#include "ex_loop.h"
src/core/io_loop.h:38:#include "genthread_pipeline.h"
src/core/io_loop.h:39:#include "../net/conn.h"
src/core/io_loop.h:40:#include "../net/resp.h"
src/core/io_loop.h:41:#include "../net/uring.h"
src/core/io_loop.h:42:#include "../net/epoll.h"
src/core/io_loop.h:43:#include "../net/wb.h"
src/core/io_loop.h:44:#include "../net/tls.h"
src/core/io_loop.h:45:#include "../cmd/command.h"
src/core/io_loop.h:46:#include "../cmd/debug_sleep.h"
src/core/io_loop.h:47:#include "../cmd/slowlog.h"
src/core/io_loop.h:48:#include "../cmd/blocking.h"
src/core/io_loop.h:49:#include "../cmd/auth.h"
src/core/io_loop.h:50:#include "../cmd/acl.h"
src/core/io_loop.h:51:#include "../cmd/multi.h"
src/core/io_loop.h:52:#include "../cmd/notify.h"
src/core/io_loop.h:53:#include "../cmd/server_tail.h"
src/core/io_loop.h:54:#include "../cmd/xshard.h"
src/core/io_loop.h:55:#include "../snapshot/snapshot.h"
src/core/io_loop.h:56:#include "../persist/aof.h"
src/core/io_loop.h:524:#include "pubsub.inc"
src/core/io_loop.h:7388:#include "../cmd/climon.inc"

diff --git a/src/cmd/t_server.cc b/src/cmd/t_server.cc
index 2d5c81bfdb1c5ee1dcedddf26af8e1801a2ea4d3..66784bda75358a60212f2a3a0c1c4904d9f5f39e
--- a/src/cmd/t_server.cc
+++ b/src/cmd/t_server.cc
@@ -14,6 +14,7 @@
 #include "server_tail.h"
 #include "slowlog.h"
 #include "../base/alloc.h"
+#include "../core/genthread.h"
 #include "../core/server.h"
 #include "../core/lbsignals.h"
 #include "../core/pubsub_event.h"
@@ -1992,33 +1993,10 @@
                 static_cast<unsigned>(g_server ? g_server->cfg().port : 0),
                 static_cast<unsigned long long>(uptime),
                 static_cast<unsigned long long>(uptime / 86400));
-        uint32_t active_readers = 0;
-        if (g_server) {
-            uint32_t owned[kMaxThreads] = {};
-            // Read the published ownership directory, never a vector FLIP may be rewriting.
-            for (uint32_t sid = 0; sid < g_server->nshards(); sid++)
-                owned[g_server->worker_of_shard(static_cast<int32_t>(sid))]++;
-            for (uint32_t tid = 0; tid < g_server->nthreads(); tid++) {
-                const ThreadCtx& thread = g_server->thread(tid);
-                const bool active = thread.read_local_lane_active();
-                active_readers += active;
-                const Role role = thread.role();
-                const char* name = role == Role::Idle ? "idle"
-                    : g_server->thread_mode() == ThreadMode::Fused ? "unified"
-                    : role == Role::Ifid ? "ifid" : "ex";
-                const ReadLocalStats* stats = g_server->read_local_enabled()
-                    ? &thread.read_local_stats() : nullptr;
-                // These per-thread totals are lifetime counters; INFO stats retains its existing
-                // RESETSTAT-relative aggregate counters. FLIP does not reset either sidecar.
-                appendf(body,
-                        "read_local_thread_%u:role=%s,shards=%u,active=%u,"
-                        "hits_total=%llu,mget_hits_total=%llu\r\n",
-                        tid, name, owned[tid], active ? 1u : 0u,
-                        static_cast<unsigned long long>(stats ? stats->hits : 0),
-                        static_cast<unsigned long long>(stats ? stats->mget_local_hits : 0));
-            }
-        }
-        appendf(body, "read_local_active_threads:%u\r\n", active_readers);
+        // Guard the CALL, including all argument evaluation. The cold non-inlined helper owns
+        // its scratch buffers so the disabled INFO path keeps its old output and stack storage.
+        if (g_server && g_server->read_local_enabled())
+            append_read_local_thread_info(body, *g_server);
         if (g_server && g_server->thread_mode() == ThreadMode::Fused) {
             appendf(body,
                     "fused_threads:%u\r\nclient_threads:%u\r\nowner_threads:%u\r\n"
diff --git a/src/core/genthread.h b/src/core/genthread.h
index c47b788f303c3a7e7f482d5ddc9f5a580a0a9bbb..e874192a1f6c2412955de097a7cd14587582ee0f
--- a/src/core/genthread.h
+++ b/src/core/genthread.h
@@ -3,6 +3,7 @@
 #pragma once
 
 #include <memory>
+#include <string>
 #include <vector>
 
 namespace tomo {
@@ -27,4 +28,7 @@
                      LateUnixListener& unix_listener,
                      ShutdownReportFinalLine& final_report);
 
+// INFO-only diagnostics. The caller must test read_local_enabled() before this call.
+void append_read_local_thread_info(std::string& body, const Server& server);
+
 }  // namespace tomo
diff --git a/src/core/rl2s.cc b/src/core/rl2s.cc
index d942a2e0f0f948a54a3f9becd0e2e32efb231a98..274b33481ca4d2e1e9e9b06d9d4dc9da575090e8
--- a/src/core/rl2s.cc
+++ b/src/core/rl2s.cc
@@ -305,4 +305,40 @@
     return 0;
 }
 
+// Keep the scratch arrays out of cmd_info's frame, including with LTO. Only an armed INFO SERVER
+// request reaches this helper; the boot/runtime paths install no reporting callback or sidecar.
+__attribute__((noinline, cold))
+void append_read_local_thread_info(std::string& body, const Server& srv) {
+    uint32_t active_readers = 0;
+    uint32_t owned[kMaxThreads] = {};
+    // Read the published ownership directory, never a vector FLIP may be rewriting.
+    for (uint32_t sid = 0; sid < srv.nshards(); sid++)
+        owned[srv.worker_of_shard(static_cast<int32_t>(sid))]++;
+    char row[192];  // fixed grammar, including full-width uint32/uint64 counters
+    for (uint32_t tid = 0; tid < srv.nthreads(); tid++) {
+        const ThreadCtx& thread = srv.thread(tid);
+        const bool active = thread.read_local_lane_active();
+        active_readers += active;
+        const Role role = thread.role();
+        const char* name = role == Role::Idle ? "idle"
+            : srv.thread_mode() == ThreadMode::Fused ? "unified"
+            : role == Role::Ifid ? "ifid" : "ex";
+        const ReadLocalStats& stats = thread.read_local_stats();
+        // Lifetime counters survive FLIP and RESETSTAT; INFO stats retains its existing
+        // RESETSTAT-relative aggregate counters.
+        const int n = std::snprintf(row, sizeof(row),
+            "read_local_thread_%u:role=%s,shards=%u,active=%u,"
+            "hits_total=%llu,mget_hits_total=%llu\r\n",
+            tid, name, owned[tid], active ? 1u : 0u,
+            static_cast<unsigned long long>(stats.hits),
+            static_cast<unsigned long long>(stats.mget_local_hits));
+        if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+        body.append(row, static_cast<size_t>(n));
+    }
+    const int n = std::snprintf(row, sizeof(row), "read_local_active_threads:%u\r\n",
+                              active_readers);
+    if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+    body.append(row, static_cast<size_t>(n));
+}
+
 }  // namespace tomo

apply patch
patch: completed
/home/user/Projects/cx-rl2s/src/cmd/t_server.cc
/home/user/Projects/cx-rl2s/src/core/thread.h
diff --git a/src/cmd/t_server.cc b/src/cmd/t_server.cc
index 2d5c81bfdb1c5ee1dcedddf26af8e1801a2ea4d3..7ad03034a03f1c5a2918a29c761c28fa54bdcb96
--- a/src/cmd/t_server.cc
+++ b/src/cmd/t_server.cc
@@ -14,6 +14,7 @@
 #include "server_tail.h"
 #include "slowlog.h"
 #include "../base/alloc.h"
+#include "../core/genthread.h"
 #include "../core/server.h"
 #include "../core/lbsignals.h"
 #include "../core/pubsub_event.h"
@@ -1992,33 +1993,10 @@
                 static_cast<unsigned>(g_server ? g_server->cfg().port : 0),
                 static_cast<unsigned long long>(uptime),
                 static_cast<unsigned long long>(uptime / 86400));
-        uint32_t active_readers = 0;
-        if (g_server) {
-            uint32_t owned[kMaxThreads] = {};
-            // Read the published ownership directory, never a vector FLIP may be rewriting.
-            for (uint32_t sid = 0; sid < g_server->nshards(); sid++)
-                owned[g_server->worker_of_shard(static_cast<int32_t>(sid))]++;
-            for (uint32_t tid = 0; tid < g_server->nthreads(); tid++) {
-                const ThreadCtx& thread = g_server->thread(tid);
-                const bool active = thread.read_local_lane_active();
-                active_readers += active;
-                const Role role = thread.role();
-                const char* name = role == Role::Idle ? "idle"
-                    : g_server->thread_mode() == ThreadMode::Fused ? "unified"
-                    : role == Role::Ifid ? "ifid" : "ex";
-                const ReadLocalStats* stats = g_server->read_local_enabled()
-                    ? &thread.read_local_stats() : nullptr;
-                // These per-thread totals are lifetime counters; INFO stats retains its existing
-                // RESETSTAT-relative aggregate counters. FLIP does not reset either sidecar.
-                appendf(body,
-                        "read_local_thread_%u:role=%s,shards=%u,active=%u,"
-                        "hits_total=%llu,mget_hits_total=%llu\r\n",
-                        tid, name, owned[tid], active ? 1u : 0u,
-                        static_cast<unsigned long long>(stats ? stats->hits : 0),
-                        static_cast<unsigned long long>(stats ? stats->mget_local_hits : 0));
-            }
-        }
-        appendf(body, "read_local_active_threads:%u\r\n", active_readers);
+        // Guard the CALL, including all argument evaluation. The cold non-inlined helper owns
+        // its scratch buffers so the disabled INFO path keeps its old output without those arrays.
+        if (g_server && g_server->read_local_enabled())
+            append_read_local_thread_info(body, *g_server);
         if (g_server && g_server->thread_mode() == ThreadMode::Fused) {
             appendf(body,
                     "fused_threads:%u\r\nclient_threads:%u\r\nowner_threads:%u\r\n"
diff --git a/src/core/genthread.h b/src/core/genthread.h
index c47b788f303c3a7e7f482d5ddc9f5a580a0a9bbb..e874192a1f6c2412955de097a7cd14587582ee0f
--- a/src/core/genthread.h
+++ b/src/core/genthread.h
@@ -3,6 +3,7 @@
 #pragma once
 
 #include <memory>
+#include <string>
 #include <vector>
 
 namespace tomo {
@@ -27,4 +28,7 @@
                      LateUnixListener& unix_listener,
                      ShutdownReportFinalLine& final_report);
 
+// INFO-only diagnostics. The caller must test read_local_enabled() before this call.
+void append_read_local_thread_info(std::string& body, const Server& server);
+
 }  // namespace tomo
diff --git a/src/core/rl2s.cc b/src/core/rl2s.cc
index d942a2e0f0f948a54a3f9becd0e2e32efb231a98..274b33481ca4d2e1e9e9b06d9d4dc9da575090e8
--- a/src/core/rl2s.cc
+++ b/src/core/rl2s.cc
@@ -305,4 +305,40 @@
     return 0;
 }
 
+// Keep the scratch arrays out of cmd_info's frame, including with LTO. Only an armed INFO SERVER
+// request reaches this helper; the boot/runtime paths install no reporting callback or sidecar.
+__attribute__((noinline, cold))
+void append_read_local_thread_info(std::string& body, const Server& srv) {
+    uint32_t active_readers = 0;
+    uint32_t owned[kMaxThreads] = {};
+    // Read the published ownership directory, never a vector FLIP may be rewriting.
+    for (uint32_t sid = 0; sid < srv.nshards(); sid++)
+        owned[srv.worker_of_shard(static_cast<int32_t>(sid))]++;
+    char row[192];  // fixed grammar, including full-width uint32/uint64 counters
+    for (uint32_t tid = 0; tid < srv.nthreads(); tid++) {
+        const ThreadCtx& thread = srv.thread(tid);
+        const bool active = thread.read_local_lane_active();
+        active_readers += active;
+        const Role role = thread.role();
+        const char* name = role == Role::Idle ? "idle"
+            : srv.thread_mode() == ThreadMode::Fused ? "unified"
+            : role == Role::Ifid ? "ifid" : "ex";
+        const ReadLocalStats& stats = thread.read_local_stats();
+        // Lifetime counters survive FLIP and RESETSTAT; INFO stats retains its existing
+        // RESETSTAT-relative aggregate counters.
+        const int n = std::snprintf(row, sizeof(row),
+            "read_local_thread_%u:role=%s,shards=%u,active=%u,"
+            "hits_total=%llu,mget_hits_total=%llu\r\n",
+            tid, name, owned[tid], active ? 1u : 0u,
+            static_cast<unsigned long long>(stats.hits),
+            static_cast<unsigned long long>(stats.mget_local_hits));
+        if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+        body.append(row, static_cast<size_t>(n));
+    }
+    const int n = std::snprintf(row, sizeof(row), "read_local_active_threads:%u\r\n",
+                              active_readers);
+    if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+    body.append(row, static_cast<size_t>(n));
+}
+
 }  // namespace tomo
diff --git a/src/core/thread.h b/src/core/thread.h
index adea9605d0f0d40c3566e07b90a2663accb6b835..61802252934cbd450279ad7cd2dbaa8d5f03f465
--- a/src/core/thread.h
+++ b/src/core/thread.h
@@ -303,6 +303,8 @@
     std::atomic<bool> lane_active{false};
 };
 #if TOMO_READ_LOCAL_SET_TAX_VARIANT != 3
+static_assert(offsetof(ReadLocalThreadState, lane_active) == 360,
+              "lane activation must follow the entire pre-RL2S sidecar");
 static_assert(sizeof(ReadLocalThreadState) == 368,
               "lane activation adds one alignment word only to the optional sidecar");
 #endif

diff --git a/src/cmd/t_server.cc b/src/cmd/t_server.cc
index 2d5c81bfdb1c5ee1dcedddf26af8e1801a2ea4d3..7ad03034a03f1c5a2918a29c761c28fa54bdcb96
--- a/src/cmd/t_server.cc
+++ b/src/cmd/t_server.cc
@@ -14,6 +14,7 @@
 #include "server_tail.h"
 #include "slowlog.h"
 #include "../base/alloc.h"
+#include "../core/genthread.h"
 #include "../core/server.h"
 #include "../core/lbsignals.h"
 #include "../core/pubsub_event.h"
@@ -1992,33 +1993,10 @@
                 static_cast<unsigned>(g_server ? g_server->cfg().port : 0),
                 static_cast<unsigned long long>(uptime),
                 static_cast<unsigned long long>(uptime / 86400));
-        uint32_t active_readers = 0;
-        if (g_server) {
-            uint32_t owned[kMaxThreads] = {};
-            // Read the published ownership directory, never a vector FLIP may be rewriting.
-            for (uint32_t sid = 0; sid < g_server->nshards(); sid++)
-                owned[g_server->worker_of_shard(static_cast<int32_t>(sid))]++;
-            for (uint32_t tid = 0; tid < g_server->nthreads(); tid++) {
-                const ThreadCtx& thread = g_server->thread(tid);
-                const bool active = thread.read_local_lane_active();
-                active_readers += active;
-                const Role role = thread.role();
-                const char* name = role == Role::Idle ? "idle"
-                    : g_server->thread_mode() == ThreadMode::Fused ? "unified"
-                    : role == Role::Ifid ? "ifid" : "ex";
-                const ReadLocalStats* stats = g_server->read_local_enabled()
-                    ? &thread.read_local_stats() : nullptr;
-                // These per-thread totals are lifetime counters; INFO stats retains its existing
-                // RESETSTAT-relative aggregate counters. FLIP does not reset either sidecar.
-                appendf(body,
-                        "read_local_thread_%u:role=%s,shards=%u,active=%u,"
-                        "hits_total=%llu,mget_hits_total=%llu\r\n",
-                        tid, name, owned[tid], active ? 1u : 0u,
-                        static_cast<unsigned long long>(stats ? stats->hits : 0),
-                        static_cast<unsigned long long>(stats ? stats->mget_local_hits : 0));
-            }
-        }
-        appendf(body, "read_local_active_threads:%u\r\n", active_readers);
+        // Guard the CALL, including all argument evaluation. The cold non-inlined helper owns
+        // its scratch buffers so the disabled INFO path keeps its old output without those arrays.
+        if (g_server && g_server->read_local_enabled())
+            append_read_local_thread_info(body, *g_server);
         if (g_server && g_server->thread_mode() == ThreadMode::Fused) {
             appendf(body,
                     "fused_threads:%u\r\nclient_threads:%u\r\nowner_threads:%u\r\n"
diff --git a/src/core/genthread.h b/src/core/genthread.h
index c47b788f303c3a7e7f482d5ddc9f5a580a0a9bbb..e874192a1f6c2412955de097a7cd14587582ee0f
--- a/src/core/genthread.h
+++ b/src/core/genthread.h
@@ -3,6 +3,7 @@
 #pragma once
 
 #include <memory>
+#include <string>
 #include <vector>
 
 namespace tomo {
@@ -27,4 +28,7 @@
                      LateUnixListener& unix_listener,
                      ShutdownReportFinalLine& final_report);
 
+// INFO-only diagnostics. The caller must test read_local_enabled() before this call.
+void append_read_local_thread_info(std::string& body, const Server& server);
+
 }  // namespace tomo
diff --git a/src/core/rl2s.cc b/src/core/rl2s.cc
index d942a2e0f0f948a54a3f9becd0e2e32efb231a98..274b33481ca4d2e1e9e9b06d9d4dc9da575090e8
--- a/src/core/rl2s.cc
+++ b/src/core/rl2s.cc
@@ -305,4 +305,40 @@
     return 0;
 }
 
+// Keep the scratch arrays out of cmd_info's frame, including with LTO. Only an armed INFO SERVER
+// request reaches this helper; the boot/runtime paths install no reporting callback or sidecar.
+__attribute__((noinline, cold))
+void append_read_local_thread_info(std::string& body, const Server& srv) {
+    uint32_t active_readers = 0;
+    uint32_t owned[kMaxThreads] = {};
+    // Read the published ownership directory, never a vector FLIP may be rewriting.
+    for (uint32_t sid = 0; sid < srv.nshards(); sid++)
+        owned[srv.worker_of_shard(static_cast<int32_t>(sid))]++;
+    char row[192];  // fixed grammar, including full-width uint32/uint64 counters
+    for (uint32_t tid = 0; tid < srv.nthreads(); tid++) {
+        const ThreadCtx& thread = srv.thread(tid);
+        const bool active = thread.read_local_lane_active();
+        active_readers += active;
+        const Role role = thread.role();
+        const char* name = role == Role::Idle ? "idle"
+            : srv.thread_mode() == ThreadMode::Fused ? "unified"
+            : role == Role::Ifid ? "ifid" : "ex";
+        const ReadLocalStats& stats = thread.read_local_stats();
+        // Lifetime counters survive FLIP and RESETSTAT; INFO stats retains its existing
+        // RESETSTAT-relative aggregate counters.
+        const int n = std::snprintf(row, sizeof(row),
+            "read_local_thread_%u:role=%s,shards=%u,active=%u,"
+            "hits_total=%llu,mget_hits_total=%llu\r\n",
+            tid, name, owned[tid], active ? 1u : 0u,
+            static_cast<unsigned long long>(stats.hits),
+            static_cast<unsigned long long>(stats.mget_local_hits));
+        if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+        body.append(row, static_cast<size_t>(n));
+    }
+    const int n = std::snprintf(row, sizeof(row), "read_local_active_threads:%u\r\n",
+                              active_readers);
+    if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+    body.append(row, static_cast<size_t>(n));
+}
+
 }  // namespace tomo
diff --git a/src/core/thread.h b/src/core/thread.h
index adea9605d0f0d40c3566e07b90a2663accb6b835..61802252934cbd450279ad7cd2dbaa8d5f03f465
--- a/src/core/thread.h
+++ b/src/core/thread.h
@@ -303,6 +303,8 @@
     std::atomic<bool> lane_active{false};
 };
 #if TOMO_READ_LOCAL_SET_TAX_VARIANT != 3
+static_assert(offsetof(ReadLocalThreadState, lane_active) == 360,
+              "lane activation must follow the entire pre-RL2S sidecar");
 static_assert(sizeof(ReadLocalThreadState) == 368,
               "lane activation adds one alignment word only to the optional sidecar");
 #endif

exec
/bin/bash -lc 'git diff HEAD -- src/core/io_loop.h src/core/ex_loop.h src/core/server.h src/main.cc src/core/config.h' in /home/user/Projects/cx-rl2s
 succeeded in 0ms:
diff --git a/src/core/config.h b/src/core/config.h
index a8938dd29..b94df1d5d 100644
--- a/src/core/config.h
+++ b/src/core/config.h
@@ -317,7 +317,7 @@ struct Config {
     PersistIoEngine persist_io = PersistIoEngine::Uring;
     ThreadMode thread_mode = ThreadMode::Split;
     // Occupies the pre-existing padding before appendfilename, preserving Config and Server layout.
-    uint32_t read_local = 0;            // boot-only 0|1; 1s overlap-0 GET/MGET local-read lane
+    uint32_t read_local = 0;            // boot-only 0|1; overlap-0 GET/MGET lane in 1s or 2s IO
     const char* appendfilename = "appendonly.aof";
     const char* appenddirname = "appendonlydir";
     uint32_t auto_aof_rewrite_percentage = 100;
@@ -1139,7 +1139,7 @@ inline int parse_config_args(const std::vector<const char*>& args, Config& cfg,
                         "             --read-local-atomic-filter 0|1 (boot-only; default 1)\n"
                         "             (--thread-pipeline is an overlap alias)\n"
                         "             (split/fused are mode aliases)\n"
-                        "             (read-local is active only with 1s overlap 0)\n"
+                        "             (read-local requires overlap 0; 2s IO owns no shards)\n"
                         "  placement (2s; default = even io/ex split over all allowed cpus):\n"
                         "    --ratio io:ex               GLOBAL counts, spread evenly over L3 domains\n"
                         "    --place role@cpu,...        explicit per-thread; roles are ifid, ex\n"
@@ -1212,6 +1212,10 @@ inline int validate_config(const Config& cfg) {
                      "--overlap 2 is only available with --thread-mode 1s; 2s has no deep unified-stream schedule\n");
         return kConfigError;
     }
+    if (cfg.thread_mode == ThreadMode::Split && cfg.read_local && cfg.overlap != 0) {
+        std::fprintf(stderr, "--thread-mode 2s --read-local 1 requires --overlap 0\n");
+        return kConfigError;
+    }
     if (cfg.thread_mode == ThreadMode::Fused && cfg.overlap != 0 &&
         cfg.net_io != NetIoEngine::Uring) {
         std::fprintf(stderr,
diff --git a/src/core/ex_loop.h b/src/core/ex_loop.h
index 296e3fe56..d90f01e66 100644
--- a/src/core/ex_loop.h
+++ b/src/core/ex_loop.h
@@ -394,6 +394,7 @@ public:
 
     uint32_t fused_baseline_pass() {
         static_assert(Fused);
+        if (srv_->thread_mode() == ThreadMode::Split) return split_read_local_pass();
         if (read_local_interleave_enabled())
             return fused_pass_impl<kGenthreadExBatchOps, true, false, false, true>();
         return fused_pass_impl<kGenthreadExBatchOps, true, false>();
@@ -636,6 +637,7 @@ public:
 
     uint32_t fused_baseline_sweep() {
         static_assert(Fused);
+        if (srv_->thread_mode() == ThreadMode::Split) return split_read_local_pass();
         if (read_local_interleave_enabled())
             return fused_sweep_impl<kGenthreadExBatchOps, true, false, false, true>();
         return fused_sweep_impl<kGenthreadExBatchOps, true, false>();
@@ -698,12 +700,29 @@ public:
 
     Ring& ring() { return ring_; }
 
+    // Defined with the split read-local runtime in rl2s.cc. The IO role never services owner
+    // tasks or shard housekeeping, even while FLIP installs its future EX shard vector.
+    uint32_t split_read_local_pass();
+
     void run() {
+        if constexpr (Fused) {
+            // Only RL2S instantiates the owner loop with the fused-capable executor. Its lane
+            // was drained before role conversion; owner commands use no local-read captures.
+            if (!read_local_enabled() || read_local_impl().lane_count != 0) std::abort();
+            self_->publish_read_local_parked(srv_->read_local_epoch());
+            // A preceding shard-less IO tenure may have consumed this CONFIG version without
+            // applying it to shards. Reapply once ownership is installed and dispatch resumes.
+            live_config_version_ = UINT64_MAX;
+        }
         LoopSignals& sig = self_->sig();
         uint32_t idle_spins = 0;
 
         while (!self_->stop_flag().load(std::memory_order_relaxed) &&
                self_->role() == Role::Ex) {
+#ifdef TOMO_RL_CACHE_DEBUG
+            if constexpr (Fused)
+                srv_->debug_assert_read_local_sinks_follow_ownership(self_->id());
+#endif
             cached_now_ms_ = realtime_ms();
             const bool flip_frozen = srv_->flip_stage() >= FlipStage::ExDrain;
             const bool lb_frozen = lb_controller_armed_ && srv_->lb_dispatch_paused();
@@ -778,6 +797,8 @@ public:
                     did += lb_control_pass();
                     lb_bucket_bytes_pass();
                 }
+                if constexpr (Fused)
+                    did += read_local_impl().deferred.drain_ready();
             }
             // A pass that found nothing -- every drain and control pass came back empty -- is
             // polling, not work. Book it as idle so busy_ns means WORK: the FLIP placement model
@@ -1889,22 +1910,25 @@ private:
         return wake_coordinator();
     }
 
+    template <bool OwnsShards = true>
     void refresh_live_config() {
         LiveConfigSnapshot snapshot;
         if (!srv_->live_config_snapshot_if_changed(live_config_version_, snapshot)) return;
         const bool enabled = snapshot.maxmemory != 0;
-        const uint64_t shard_limit = snapshot.maxmemory / srv_->nshards();
-        for (Shard* sh : self_->shards()) {
-            sh->configure_maxmemory(enabled, shard_limit, snapshot.policy, snapshot.samples);
-            // CLIENT TRACKING and periodic SAVE need the same per-write observation points as
-            // keyspace notifications, so they ride the shard mask as synthetic observer bits.
-            // notify_record expands those observers over NOTIFY_ALL without adding those class
-            // bits here: the operator's configured pub/sub classes therefore remain independent.
-            // NOTIFY_NEW and NOTIFY_KEY_MISS stay outside the observer surface: `new` would count
-            // or invalidate a mutation twice, and a key miss is not a value change.
-            sh->set_notify_mask(snapshot.notify_events |
-                                (snapshot.tracking_armed ? NOTIFY_TRACKING : 0u) |
-                                (snapshot.save_armed ? NOTIFY_SAVE : 0u));
+        if constexpr (OwnsShards) {
+            const uint64_t shard_limit = snapshot.maxmemory / srv_->nshards();
+            for (Shard* sh : self_->shards()) {
+                sh->configure_maxmemory(enabled, shard_limit, snapshot.policy, snapshot.samples);
+                // CLIENT TRACKING and periodic SAVE need the same per-write observation points as
+                // keyspace notifications, so they ride the shard mask as synthetic observer bits.
+                // notify_record expands those observers over NOTIFY_ALL without adding those class
+                // bits here: the operator's configured pub/sub classes therefore remain independent.
+                // NOTIFY_NEW and NOTIFY_KEY_MISS stay outside the observer surface: `new` would count
+                // or invalidate a mutation twice, and a key miss is not a value change.
+                sh->set_notify_mask(snapshot.notify_events |
+                                    (snapshot.tracking_armed ? NOTIFY_TRACKING : 0u) |
+                                    (snapshot.save_armed ? NOTIFY_SAVE : 0u));
+            }
         }
         maxmemory_enabled_ = enabled;
         foreign_touch_policy_ =
@@ -3413,6 +3437,7 @@ private:
 };
 
 using ExLoop = ExLoopT<false>;
+template <> uint32_t ExLoopT<true>::split_read_local_pass();
 using FusedExLoop = ExLoopT<true>;
 
 // Disabled split executors retain the exact pre-read-local allocation stride plus the 264-byte
diff --git a/src/core/io_loop.h b/src/core/io_loop.h
index f04718a40..b612cbce8 100644
--- a/src/core/io_loop.h
+++ b/src/core/io_loop.h
@@ -538,6 +538,16 @@ private:
             return;
         }
         constexpr bool IoPipe = !Fused && Pipeline == 1;
+        if constexpr (Fused) {
+            if (srv_->read_local_enabled()) {
+                // A split EX tenure is permanently parked: it never probes the local lane.
+                // Resume BEFORE sampling the epoch, as at the existing network-wait boundary.
+                if (ThreadCtx::read_local_publication_parked(self_->read_local_publication()))
+                    self_->resume_read_local_tick();
+                self_->publish_read_local_tick(srv_->read_local_epoch());
+                self_->set_read_local_lane_active(true);
+            }
+        }
         if constexpr (!kEp) {
             if (listen_fd_ >= 0) arm_accept(UrKind::Accept);
             if constexpr (HasTls) arm_accept(UrKind::TlsAccept);
@@ -754,10 +764,12 @@ private:
             self_->clear_blocked();
         }
         if constexpr (Fused) {
-            // The read loop is over permanently. Teardown below may take longer than another
+            // The read loop is over for this tenure. Teardown may take longer than another
             // owner's bounded retire queue can tolerate, but it performs no foreign store probe.
-            if (srv_->read_local_enabled())
+            if (srv_->read_local_enabled()) {
+                self_->set_read_local_lane_active(false);
                 self_->publish_read_local_parked(srv_->read_local_epoch());
+            }
         }
         // A close requested by the last pass's read/send path has no later flush_ready to drain it,
         // and an undrained entry would show up as a live connection in the shutdown accounting.
diff --git a/src/core/server.h b/src/core/server.h
index d27b1574d..f5aac780a 100644
--- a/src/core/server.h
+++ b/src/core/server.h
@@ -134,7 +134,7 @@ struct FlipReport {
     bool moving = false;
 };
 
-// Allocated only for the boot-armed fused read-local lane. The Server keeps only one pointer at its
+// Allocated only for the boot-armed read-local lane. The Server keeps only one pointer at its
 // true tail, so baseline member offsets and cache-line sharing remain unchanged.
 struct ReadLocalServerState {
     std::atomic<uint64_t> epoch{1};
@@ -580,7 +580,7 @@ public:
     }
 
     static bool read_local_enabled(const Config& cfg) {
-        return cfg.thread_mode == ThreadMode::Fused && cfg.overlap == 0 && cfg.read_local != 0;
+        return cfg.overlap == 0 && cfg.read_local != 0;
     }
     bool read_local_enabled() const { return read_local_enabled(cfg_); }
     uint64_t read_local_epoch() const {
diff --git a/src/main.cc b/src/main.cc
index c14f656ae..c8fdead11 100644
--- a/src/main.cc
+++ b/src/main.cc
@@ -180,10 +180,9 @@ int main(int argc, char** argv) {
     if (cfg.overlap == 2)
         std::fprintf(stderr,
                      "WARNING: --overlap 2 selects an experimental research schedule\n");
-    if (cfg.read_local &&
-        (cfg.thread_mode != ThreadMode::Fused || cfg.overlap != 0))
+    if (cfg.read_local && cfg.overlap != 0)
         std::fprintf(stderr,
-                     "NOTICE: --read-local 1 requires --thread-mode 1s --overlap 0 "
+                     "NOTICE: --read-local 1 requires --overlap 0 "
                      "in this version; using the ordinary owner-task path\n");
     // THE ENGINE IS LATCHED HERE, once, before anything that reads it exists. Every Ring in the
     // process must agree (a uring ring cannot receive an eventfd doorbell and vice versa), and no
@@ -315,6 +314,11 @@ int main(int argc, char** argv) {
         return run_fused_server(srv, aof_base_plan.get(), aof_plans, load_plan.get(),
                                 tls_context.get(), unix_listener, final_shutdown_line);
     }
+    if (srv.read_local_enabled()) {
+        srv.topo().dump(stdout);
+        return run_split_read_local_server(srv, aof_base_plan.get(), aof_plans, load_plan.get(),
+                                           tls_context.get(), unix_listener, final_shutdown_line);
+    }
 
     srv.topo().dump(stdout);
     std::printf("tomokv-cpp: %u threads (%zu io + %zu ex), %u shard(s),"

exec
/bin/bash -lc "rg -n 'enum class ThreadMode|enum class Role|is_empty_v|no_unique_address|static_assert\\(sizeof\\((Config|ThreadCtx|ReadLocalThreadState|ExLoop)' src/core/config.h src/core/placement.h src/core/thread.h src/core/ex_loop.h
rg -n 'read_local_write|begin_read_local|arm_read_local' src/core/io_loop.h | head -30
nl -ba src/net/rob.h | sed -n '445,496p'
nl -ba src/cmd/t_server.cc | sed -n '318,333p'
rg -n 'reset\\(|reserve\\(|resize\\(|new |std::vector|std::deque|"'^ *(Server|ThreadCtx)'"\\*' src/core/ex_loop.h | tail -35
nl -ba src/core/read_local.h | sed -n '463,483p'" in /home/user/Projects/cx-rl2s
 succeeded in 0ms:
src/core/ex_loop.h:165:static_assert(std::is_empty_v<ReadLocalExState<false>>);
src/core/ex_loop.h:3436:    [[no_unique_address]] ReadLocalExState<Fused> read_local_;
src/core/ex_loop.h:3450:static_assert(sizeof(ExLoop) == 6112);
src/core/thread.h:63:enum class Role : uint8_t { Idle = 0, Ifid = 1, Ex = 2 };
src/core/thread.h:308:static_assert(sizeof(ReadLocalThreadState) == 368,
src/core/thread.h:1208:static_assert(sizeof(ThreadCtx) == 1408,
src/core/config.h:225:enum class ThreadMode : uint8_t { Split = 0, Fused = 1 };
src/core/config.h:429:static_assert(sizeof(Config) == 624,
3498:    static ReadLocalFallbackReason read_local_write_fallback_reason(
4031:            [[maybe_unused]] bool read_local_write_hazard = false;
4215:                    read_local_write_hazard = write_hazard;
4392:                                write_conflict |= rob.read_local_write_conflicts(
4398:                            write_conflict = rob.read_local_write_conflicts(
4406:                                ? read_local_write_fallback_reason(rob)
4429:                                    read_local_write_fallback_reason(rob);
4590:                    if (read_local_demotion.active() && !read_local_write_hazard) {
5178:                    if (read_local_demotion.active() && !read_local_write_hazard) {
   445	                const uint32_t index = static_cast<uint32_t>(__builtin_ctzll(bits));
   446	                bits &= bits - 1;
   447	                const uint64_t id = base | index;
   448	                const Op& op = at(id);
   449	                if (op.state.load(std::memory_order_acquire) == OpState::Done) {
   450	                    read_local_owner_slots_ &= ~(uint64_t{1} << index);
   451	                    continue;
   452	                }
   453	                if (id - flush < before_distance && match(op)) conflict = true;
   454	            }
   455	        };
   456	        inspect(high, generation);
   457	        inspect(low, generation + Capacity);
   458	        return conflict;
   459	    }
   460	
   461	    // Mark an owner-routed read (or the precise point operation that forced its demotion). Younger
   462	    // reads consult this slot only when their keysets overlap; ROB retirement alone orders replies.
   463	    void extend_current_read_local_owner() {
   464	        read_local_owner_slots_ |=
   465	            uint64_t{1} << (static_cast<uint32_t>(dispatch_id()) & kMask);
   466	    }
   467	
   468	    // Stage a conservative write record before the parser enters ACL/special lowering. The next
   469	    // armed acquire is the commit point: an advanced dispatch id means this op published, while an
   470	    // unchanged id means dispatch was abandoned (including publish + refused-post + unpublish).
   471	    // Every write starts conservative. After arity and routing are known, ordinary point writes
   472	    // and bounded blind keysets may refine it; all other special/multi-key writes leave it broad.
   473	    void mark_current_write() {
   474	        // ARM ON DEMAND, THE WRITE HALF (DESIGN-RINGDIET.md). Until a local read has armed this
   475	        // connection the ring records NOTHING: no sidecar, no prune, no descriptor, no Staged tag
   476	        // -- and so no resolve on the next frame either. The entire bookkeeping is one store of
   477	        // this write's id, into a word on the producer's own cache line that dispatch_ has already
   478	        // dirtied. A pure-write connection therefore pays a predicted-taken test and that store,
   479	        // which is what the sixteen-slot ring used to pay for by GIVING UP after an overflow.
   480	        if (__builtin_expect(read_local_arm_state_ == kReadLocalUnarmed, true)) {
   481	            read_local_unarmed_write_id_ = dispatch_id();
   482	            return;
   483	        }
   484	        // FIRST WRITE OF AN ARMED CONNECTION: this is where the sidecar is born. Read-only and
   485	        // idle connections never reach it, which is the whole point of not allocating at accept.
   486	        if (__builtin_expect(read_local_state_ == 0, false) && !prepare_read_local()) {
   487	            // Out of memory. Fall back to the unarmed contract, which is always safe: it records
   488	            // nothing and the next read that arrives while this write is live is demoted to the
   489	            // owner path. The next quiescent read re-arms and the next write retries the
   490	            // allocation, so the connection heals itself without a sticky failure flag.
   491	            read_local_arm_state_ = kReadLocalUnarmed;
   492	            read_local_unarmed_write_id_ = dispatch_id();
   493	            return;
   494	        }
   495	        read_local_state_activate();
   496	        ReadLocalRobState& state = read_local_state_required();
   318	    // Boot-only, like persist-io: reported so an operator can confirm which engine a running
   319	    // server actually chose, and refused by CONFIG SET rather than silently accepted.
   320	    g_config.push_back({"net-io", ConfigKind::Enum,
   321	                        cfg.net_io == NetIoEngine::Epoll ? "epoll" : "uring", true});
   322	    g_config.push_back({"thread-mode", ConfigKind::Enum,
   323	                        cfg.thread_mode == ThreadMode::Fused ? "1s" : "2s", true});
   324	    g_config.push_back({"overlap", ConfigKind::Unsigned,
   325	                        std::to_string(cfg.overlap), true});
   326	    g_config.push_back({"read-local", ConfigKind::Unsigned,
   327	                        std::to_string(cfg.read_local), true});
   328	    g_config.push_back({"read-local-interleave", ConfigKind::Unsigned,
   329	                        std::to_string(cfg.read_local_interleave), true});
   330	    g_config.push_back({"read-local-prefetch-capture", ConfigKind::Unsigned,
   331	                        std::to_string(static_cast<uint32_t>(
   332	                            cfg.read_local_prefetch_capture)), true});
   333	    g_config.push_back({"read-local-atomic-filter", ConfigKind::Unsigned,
198:                std::unique_ptr<ReadLocalExImpl> impl(new (std::nothrow) ReadLocalExImpl);
200:                impl->lane.reset(new (std::nothrow) ReadLocalExImpl::LaneEntry[kInboxSlots]);
201:                impl->lane_fallbacks.reset(
202:                    new (std::nothrow) ReadLocalFallbackReason[kInboxSlots]);
451:            ThreadCtx* self;
452:            Server* server;
664:            ThreadCtx* self;
665:            Server* server;
986:                // are atomic, and both old/new lengths fit this allocation class. Combining odd
990:                char* frame = sink.reserve(24 + static_cast<size_t>(length) + 2);
1843:                srv_->flip_note_failure("ERR FLIP could not prepare a new IO listener");
2372:        snapshot_pending_chunks_.resize(srv_->nshards());
2374:        snapshot_backlogs_.resize(srv_->nshards());
3099:    __attribute__((noinline)) bool parked_predecessor_in(const std::deque<Task>& parked,
3351:    Server*    srv_  = nullptr;
3352:    ThreadCtx* self_ = nullptr;
3391:    std::vector<uint8_t> snapshot_done_shards_;
3392:    std::vector<std::unique_ptr<SnapshotChunk>> snapshot_pending_chunks_;
3393:    std::vector<std::deque<Task>> snapshot_backlogs_;
3394:    std::deque<Task> atomic_deferred_;
3395:    std::deque<Task> multi_retries_;
3396:    std::deque<Task> xshard_retries_;
3397:    std::deque<Task> ordered_deferred_;
3400:    std::deque<Task> stale_tasks_;
3401:    std::deque<BorrowRelease> stale_releases_;
   463	
   464	    KvBlockCache block_cache_{};
   465	    Server* server_ = nullptr;
   466	    ThreadCtx* owner_ = nullptr;
   467	    std::unique_ptr<Entry[]> entries_;
   468	    ReadLocalRetireSink sink_;
   469	    ReadLocalRetireRing ring_;
   470	    uint32_t grace_hint_ = UINT32_MAX;   // last participant seen blocking the oldest batch
   471	};
   472	
   473	}  // namespace tomo

diff --git a/src/cmd/t_server.cc b/src/cmd/t_server.cc
index 2d5c81bfdb1c5ee1dcedddf26af8e1801a2ea4d3..7ad03034a03f1c5a2918a29c761c28fa54bdcb96
--- a/src/cmd/t_server.cc
+++ b/src/cmd/t_server.cc
@@ -14,6 +14,7 @@
 #include "server_tail.h"
 #include "slowlog.h"
 #include "../base/alloc.h"
+#include "../core/genthread.h"
 #include "../core/server.h"
 #include "../core/lbsignals.h"
 #include "../core/pubsub_event.h"
@@ -1992,33 +1993,10 @@
                 static_cast<unsigned>(g_server ? g_server->cfg().port : 0),
                 static_cast<unsigned long long>(uptime),
                 static_cast<unsigned long long>(uptime / 86400));
-        uint32_t active_readers = 0;
-        if (g_server) {
-            uint32_t owned[kMaxThreads] = {};
-            // Read the published ownership directory, never a vector FLIP may be rewriting.
-            for (uint32_t sid = 0; sid < g_server->nshards(); sid++)
-                owned[g_server->worker_of_shard(static_cast<int32_t>(sid))]++;
-            for (uint32_t tid = 0; tid < g_server->nthreads(); tid++) {
-                const ThreadCtx& thread = g_server->thread(tid);
-                const bool active = thread.read_local_lane_active();
-                active_readers += active;
-                const Role role = thread.role();
-                const char* name = role == Role::Idle ? "idle"
-                    : g_server->thread_mode() == ThreadMode::Fused ? "unified"
-                    : role == Role::Ifid ? "ifid" : "ex";
-                const ReadLocalStats* stats = g_server->read_local_enabled()
-                    ? &thread.read_local_stats() : nullptr;
-                // These per-thread totals are lifetime counters; INFO stats retains its existing
-                // RESETSTAT-relative aggregate counters. FLIP does not reset either sidecar.
-                appendf(body,
-                        "read_local_thread_%u:role=%s,shards=%u,active=%u,"
-                        "hits_total=%llu,mget_hits_total=%llu\r\n",
-                        tid, name, owned[tid], active ? 1u : 0u,
-                        static_cast<unsigned long long>(stats ? stats->hits : 0),
-                        static_cast<unsigned long long>(stats ? stats->mget_local_hits : 0));
-            }
-        }
-        appendf(body, "read_local_active_threads:%u\r\n", active_readers);
+        // Guard the CALL, including all argument evaluation. The cold non-inlined helper owns
+        // its scratch buffers so the disabled INFO path keeps its old output without those arrays.
+        if (g_server && g_server->read_local_enabled())
+            append_read_local_thread_info(body, *g_server);
         if (g_server && g_server->thread_mode() == ThreadMode::Fused) {
             appendf(body,
                     "fused_threads:%u\r\nclient_threads:%u\r\nowner_threads:%u\r\n"
diff --git a/src/core/genthread.h b/src/core/genthread.h
index c47b788f303c3a7e7f482d5ddc9f5a580a0a9bbb..e874192a1f6c2412955de097a7cd14587582ee0f
--- a/src/core/genthread.h
+++ b/src/core/genthread.h
@@ -3,6 +3,7 @@
 #pragma once
 
 #include <memory>
+#include <string>
 #include <vector>
 
 namespace tomo {
@@ -27,4 +28,7 @@
                      LateUnixListener& unix_listener,
                      ShutdownReportFinalLine& final_report);
 
+// INFO-only diagnostics. The caller must test read_local_enabled() before this call.
+void append_read_local_thread_info(std::string& body, const Server& server);
+
 }  // namespace tomo
diff --git a/src/core/rl2s.cc b/src/core/rl2s.cc
index d942a2e0f0f948a54a3f9becd0e2e32efb231a98..274b33481ca4d2e1e9e9b06d9d4dc9da575090e8
--- a/src/core/rl2s.cc
+++ b/src/core/rl2s.cc
@@ -305,4 +305,40 @@
     return 0;
 }
 
+// Keep the scratch arrays out of cmd_info's frame, including with LTO. Only an armed INFO SERVER
+// request reaches this helper; the boot/runtime paths install no reporting callback or sidecar.
+__attribute__((noinline, cold))
+void append_read_local_thread_info(std::string& body, const Server& srv) {
+    uint32_t active_readers = 0;
+    uint32_t owned[kMaxThreads] = {};
+    // Read the published ownership directory, never a vector FLIP may be rewriting.
+    for (uint32_t sid = 0; sid < srv.nshards(); sid++)
+        owned[srv.worker_of_shard(static_cast<int32_t>(sid))]++;
+    char row[192];  // fixed grammar, including full-width uint32/uint64 counters
+    for (uint32_t tid = 0; tid < srv.nthreads(); tid++) {
+        const ThreadCtx& thread = srv.thread(tid);
+        const bool active = thread.read_local_lane_active();
+        active_readers += active;
+        const Role role = thread.role();
+        const char* name = role == Role::Idle ? "idle"
+            : srv.thread_mode() == ThreadMode::Fused ? "unified"
+            : role == Role::Ifid ? "ifid" : "ex";
+        const ReadLocalStats& stats = thread.read_local_stats();
+        // Lifetime counters survive FLIP and RESETSTAT; INFO stats retains its existing
+        // RESETSTAT-relative aggregate counters.
+        const int n = std::snprintf(row, sizeof(row),
+            "read_local_thread_%u:role=%s,shards=%u,active=%u,"
+            "hits_total=%llu,mget_hits_total=%llu\r\n",
+            tid, name, owned[tid], active ? 1u : 0u,
+            static_cast<unsigned long long>(stats.hits),
+            static_cast<unsigned long long>(stats.mget_local_hits));
+        if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+        body.append(row, static_cast<size_t>(n));
+    }
+    const int n = std::snprintf(row, sizeof(row), "read_local_active_threads:%u\r\n",
+                              active_readers);
+    if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+    body.append(row, static_cast<size_t>(n));
+}
+
 }  // namespace tomo
diff --git a/src/core/thread.h b/src/core/thread.h
index adea9605d0f0d40c3566e07b90a2663accb6b835..61802252934cbd450279ad7cd2dbaa8d5f03f465
--- a/src/core/thread.h
+++ b/src/core/thread.h
@@ -303,6 +303,8 @@
     std::atomic<bool> lane_active{false};
 };
 #if TOMO_READ_LOCAL_SET_TAX_VARIANT != 3
+static_assert(offsetof(ReadLocalThreadState, lane_active) == 360,
+              "lane activation must follow the entire pre-RL2S sidecar");
 static_assert(sizeof(ReadLocalThreadState) == 368,
               "lane activation adds one alignment word only to the optional sidecar");
 #endif

exec
/bin/bash -lc "rg -n 'Config +cfg_|Server\\* +srv_|ThreadCtx\\* +self_|private:|read_local_state_|read_local_capture|void execute|uint32_t drain_tasks' src/core/server.h src/core/io_loop.h src/core/ex_loop.h | tail -35
rg -n 'struct ReadLocalArmStats|uint64_t.*(arms|sidecars|records)' src/net/rob.h src/core/thread.h" in /home/user/Projects/cx-rl2s
 succeeded in 0ms:
src/core/ex_loop.h:94:private:
src/core/ex_loop.h:856:private:
src/core/ex_loop.h:1057:                                       .read_local_state_acquire();
src/core/ex_loop.h:1086:                                           .read_local_state_acquire();
src/core/ex_loop.h:1087:                if (!FlatStore::read_local_state_eligible(after))
src/core/ex_loop.h:1115:            const uint64_t state = store.read_local_state_acquire();
src/core/ex_loop.h:1574:        const uint64_t state = store.read_local_state_acquire();
src/core/ex_loop.h:2080:    uint32_t drain_tasks_read_local_interleaved(bool unmasked,
src/core/ex_loop.h:2126:    uint32_t drain_tasks(bool unmasked = false) {
src/core/ex_loop.h:2149:    uint32_t drain_tasks_with_filler(bool unmasked, Filler& filler, bool& filler_used) {
src/core/ex_loop.h:2608:    uint32_t drain_tasks_snapshot(bool unmasked = false) {
src/core/ex_loop.h:3351:    Server*    srv_  = nullptr;
src/core/ex_loop.h:3352:    ThreadCtx* self_ = nullptr;
src/core/io_loop.h:447:private:
src/core/io_loop.h:3513:    private:
src/core/io_loop.h:3819:    private:
src/core/io_loop.h:3903:private:
src/core/io_loop.h:4448:                                                               .read_local_state_acquire();
src/core/io_loop.h:4453:                                    seq_churn = !FlatStore::read_local_state_eligible(state);
src/core/io_loop.h:7253:    Server*    srv_  = nullptr;
src/core/io_loop.h:7254:    ThreadCtx* self_ = nullptr;
src/core/server.h:317:            read_local_state_.reset(new (std::nothrow) ReadLocalServerState);
src/core/server.h:318:            if (!read_local_state_) {
src/core/server.h:587:        if (!read_local_state_) std::abort();
src/core/server.h:588:        return read_local_state_->epoch.load(std::memory_order_seq_cst);
src/core/server.h:593:        if (!read_local_state_) std::abort();
src/core/server.h:594:        return read_local_state_->epoch.fetch_add(1, std::memory_order_seq_cst);
src/core/server.h:1761:private:
src/core/server.h:3209:private:
src/core/server.h:3372:    Config    cfg_;
src/core/server.h:3626:    std::unique_ptr<ReadLocalServerState> read_local_state_;
src/net/rob.h:62:struct ReadLocalArmStats {
src/net/rob.h:63:    uint64_t arms = 0;                  // connections a local read armed
src/net/rob.h:64:    uint64_t sidecars = 0;              // RYOW write-ring sidecars allocated (never at accept)
src/net/rob.h:65:    uint64_t write_ring_records = 0;    // writes committed into the RYOW write ring

diff --git a/src/cmd/t_server.cc b/src/cmd/t_server.cc
index 2d5c81bfdb1c5ee1dcedddf26af8e1801a2ea4d3..7ad03034a03f1c5a2918a29c761c28fa54bdcb96
--- a/src/cmd/t_server.cc
+++ b/src/cmd/t_server.cc
@@ -14,6 +14,7 @@
 #include "server_tail.h"
 #include "slowlog.h"
 #include "../base/alloc.h"
+#include "../core/genthread.h"
 #include "../core/server.h"
 #include "../core/lbsignals.h"
 #include "../core/pubsub_event.h"
@@ -1992,33 +1993,10 @@
                 static_cast<unsigned>(g_server ? g_server->cfg().port : 0),
                 static_cast<unsigned long long>(uptime),
                 static_cast<unsigned long long>(uptime / 86400));
-        uint32_t active_readers = 0;
-        if (g_server) {
-            uint32_t owned[kMaxThreads] = {};
-            // Read the published ownership directory, never a vector FLIP may be rewriting.
-            for (uint32_t sid = 0; sid < g_server->nshards(); sid++)
-                owned[g_server->worker_of_shard(static_cast<int32_t>(sid))]++;
-            for (uint32_t tid = 0; tid < g_server->nthreads(); tid++) {
-                const ThreadCtx& thread = g_server->thread(tid);
-                const bool active = thread.read_local_lane_active();
-                active_readers += active;
-                const Role role = thread.role();
-                const char* name = role == Role::Idle ? "idle"
-                    : g_server->thread_mode() == ThreadMode::Fused ? "unified"
-                    : role == Role::Ifid ? "ifid" : "ex";
-                const ReadLocalStats* stats = g_server->read_local_enabled()
-                    ? &thread.read_local_stats() : nullptr;
-                // These per-thread totals are lifetime counters; INFO stats retains its existing
-                // RESETSTAT-relative aggregate counters. FLIP does not reset either sidecar.
-                appendf(body,
-                        "read_local_thread_%u:role=%s,shards=%u,active=%u,"
-                        "hits_total=%llu,mget_hits_total=%llu\r\n",
-                        tid, name, owned[tid], active ? 1u : 0u,
-                        static_cast<unsigned long long>(stats ? stats->hits : 0),
-                        static_cast<unsigned long long>(stats ? stats->mget_local_hits : 0));
-            }
-        }
-        appendf(body, "read_local_active_threads:%u\r\n", active_readers);
+        // Guard the CALL, including all argument evaluation. The cold non-inlined helper owns
+        // its scratch buffers so the disabled INFO path keeps its old output without those arrays.
+        if (g_server && g_server->read_local_enabled())
+            append_read_local_thread_info(body, *g_server);
         if (g_server && g_server->thread_mode() == ThreadMode::Fused) {
             appendf(body,
                     "fused_threads:%u\r\nclient_threads:%u\r\nowner_threads:%u\r\n"
diff --git a/src/core/genthread.h b/src/core/genthread.h
index c47b788f303c3a7e7f482d5ddc9f5a580a0a9bbb..e874192a1f6c2412955de097a7cd14587582ee0f
--- a/src/core/genthread.h
+++ b/src/core/genthread.h
@@ -3,6 +3,7 @@
 #pragma once
 
 #include <memory>
+#include <string>
 #include <vector>
 
 namespace tomo {
@@ -27,4 +28,7 @@
                      LateUnixListener& unix_listener,
                      ShutdownReportFinalLine& final_report);
 
+// INFO-only diagnostics. The caller must test read_local_enabled() before this call.
+void append_read_local_thread_info(std::string& body, const Server& server);
+
 }  // namespace tomo
diff --git a/src/core/rl2s.cc b/src/core/rl2s.cc
index d942a2e0f0f948a54a3f9becd0e2e32efb231a98..274b33481ca4d2e1e9e9b06d9d4dc9da575090e8
--- a/src/core/rl2s.cc
+++ b/src/core/rl2s.cc
@@ -305,4 +305,40 @@
     return 0;
 }
 
+// Keep the scratch arrays out of cmd_info's frame, including with LTO. Only an armed INFO SERVER
+// request reaches this helper; the boot/runtime paths install no reporting callback or sidecar.
+__attribute__((noinline, cold))
+void append_read_local_thread_info(std::string& body, const Server& srv) {
+    uint32_t active_readers = 0;
+    uint32_t owned[kMaxThreads] = {};
+    // Read the published ownership directory, never a vector FLIP may be rewriting.
+    for (uint32_t sid = 0; sid < srv.nshards(); sid++)
+        owned[srv.worker_of_shard(static_cast<int32_t>(sid))]++;
+    char row[192];  // fixed grammar, including full-width uint32/uint64 counters
+    for (uint32_t tid = 0; tid < srv.nthreads(); tid++) {
+        const ThreadCtx& thread = srv.thread(tid);
+        const bool active = thread.read_local_lane_active();
+        active_readers += active;
+        const Role role = thread.role();
+        const char* name = role == Role::Idle ? "idle"
+            : srv.thread_mode() == ThreadMode::Fused ? "unified"
+            : role == Role::Ifid ? "ifid" : "ex";
+        const ReadLocalStats& stats = thread.read_local_stats();
+        // Lifetime counters survive FLIP and RESETSTAT; INFO stats retains its existing
+        // RESETSTAT-relative aggregate counters.
+        const int n = std::snprintf(row, sizeof(row),
+            "read_local_thread_%u:role=%s,shards=%u,active=%u,"
+            "hits_total=%llu,mget_hits_total=%llu\r\n",
+            tid, name, owned[tid], active ? 1u : 0u,
+            static_cast<unsigned long long>(stats.hits),
+            static_cast<unsigned long long>(stats.mget_local_hits));
+        if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+        body.append(row, static_cast<size_t>(n));
+    }
+    const int n = std::snprintf(row, sizeof(row), "read_local_active_threads:%u\r\n",
+                              active_readers);
+    if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+    body.append(row, static_cast<size_t>(n));
+}
+
 }  // namespace tomo
diff --git a/src/core/thread.h b/src/core/thread.h
index adea9605d0f0d40c3566e07b90a2663accb6b835..61802252934cbd450279ad7cd2dbaa8d5f03f465
--- a/src/core/thread.h
+++ b/src/core/thread.h
@@ -303,6 +303,8 @@
     std::atomic<bool> lane_active{false};
 };
 #if TOMO_READ_LOCAL_SET_TAX_VARIANT != 3
+static_assert(offsetof(ReadLocalThreadState, lane_active) == 360,
+              "lane activation must follow the entire pre-RL2S sidecar");
 static_assert(sizeof(ReadLocalThreadState) == 368,
               "lane activation adds one alignment word only to the optional sidecar");
 #endif

codex
The INFO leak is fixed. Its new rows are now enabled-only, and the scratch buffers live in the guarded helper in `rl2s.cc`. I’m finishing the allocation inventory and source comparisons, including the dormant loops that split mode already provisions for FLIP, then writing the four-part proof and its verification limits into `RL2S.md`.
exec
/bin/bash -lc "python3 - <<'PY'
import ast
import re
import subprocess
from pathlib import Path
base = '78c3e5391'
def old(path):
    return subprocess.check_output(['git', 'show', f'{base}:{path}'], text=True)
def cur(path):
    return Path(path).read_text()
lex = re.compile(r'//["'^'"\\n]*|/\\*[\\s\\S]*?\\*/|\"(?:\\\\.|["'^"'"\\\\])*\"|\\'(?:\\\\.|["'^'"\\'\\\\])*\\'|[A-Za-z_]\\w*|[0-9]+|\\S')
def toks(s):
    return [m.group() for m in lex.finditer(s) if not m.group().startswith(('//','/*'))]
def end_block(t, i):
    assert t[i] == '{'
    depth = 1
    for j in range(i + 1, len(t)):
        depth += (t[j] == '{') - (t[j] == '}')
        if depth == 0:
            return j + 1
    raise AssertionError('unbalanced source')
def block(s, marker):
    t = toks(s[s.index(marker):])
    i = t.index('{')
    return t[i:end_block(t, i)]
def check(label, a, b):
    assert a == b, label
    print('OK:', label)
def discard_fused(t):
    t = t[:]
    prefix = ['if','constexpr','(','Fused',')']
    i = 0
    while i < len(t):
        if t[i:i+5] "'!= prefix:
            i += 1
            continue
        begin = i + 5
        end = end_block(t, begin) if t[begin] == '"'{' else t.index(';', begin) + 1
        if t[end:end+1] == ['else']:
            nxt = end + 1
            if t[nxt] == '{':
                stop = end_block(t, nxt)
                t[i:stop] = t[nxt:stop]
            else:
                t[i:nxt] = []
        elif i and t[i-1] == 'else':
            t[i-1:end] = []
            i -= 1
        else:
            t[i:end] = []
    empty_debug = ['#','ifdef','TOMO_RL_CACHE_DEBUG','#','endif']
    i = 0
    while i < len(t):
        if t[i:i+5] == empty_debug:
            del t[i:i+5]
        else:
            i += 1
    return t
m = '// Placement decides every cpu directly.'
check('ordinary split construction / role loops / shutdown: byte-identical',
      old('src/main.cc').split(m, 1)[1], cur('src/main.cc').split(m, 1)[1])
for path, marker, label in [
    ('src/core/io_loop.h','bool init(Server*','IoLoop::init'),
    ('src/core/ex_loop.h','bool init(Server*','ExLoopT::init'),
    ('src/core/io_loop.h','void run_split()','IoLoop::run_split'),
    ('src/core/io_loop.h','void run()','IoLoop::run'),
    ('src/core/io_loop.h','DispatchResult parse_and_dispatch(','parser / admission / dispatch'),
    ('src/core/io_loop.h','void refresh_notify_config()','IO config refresh'),
    ('src/core/ex_loop.h','uint32_t drain_tasks(bool','owner task drain'),
    ('src/core/config.h','struct Config {','Config fields'),
    ('src/core/thread.h','struct ReadLocalStats {','ReadLocalStats fields'),
]:
    check(label + ': token-identical', block(old(path), marker), block(cur(path), marker))
for path, marker, label in [
    ('src/core/io_loop.h','void run_loop()','IO rotation'),
    ('src/core/ex_loop.h','void run()','EX rotation'),
]:
    check(label + ' after discarding Fused-only statements: token-identical',
          discard_fused(block(old(path), marker)), discard_fused(block(cur(path), marker)))
t = block(cur('src/core/ex_loop.h'),'void refresh_live_config()')
p = ['if','constexpr','(','OwnsShards',')','{']
i = next(i for i in range(len(t)) if t[i:i+len(p)] == p)
j = end_block(t, i + 5)
t[i:j] = t[i+6:j-1]
check('owner config refresh with OwnsShards=true: token-identical',
      block(old('src/core/ex_loop.h'),'void refresh_live_config()'), t)
for path, marker in [('src/core/io_loop.h','    Server*    srv_'),
                     ('src/core/ex_loop.h','    Server*    srv_'),
                     ('src/core/server.h','    Config    cfg_')]:
    a = old(path).split(marker,1)[1].split('\\n};',1)[0]
    b = cur(path).split(marker,1)[1].split('\\n};',1)[0]
    check(path + ' complete member tail: token-identical', toks(a), toks(b))
a = block(old('src/core/thread.h'),'class ThreadCtx {')
b = block(cur('src/core/thread.h'),'class ThreadCtx {')
for marker in ['void set_read_local_lane_active(bool active)', 'bool read_local_lane_active() const']:
    prefix = toks(marker)
    i = next(i for i in range(len(b)) if b[i:i+len(prefix)] == prefix)
    j = end_block(b, i + len(prefix))
    del b[i:j]
check('ThreadCtx excluding two new methods: token-identical', a, b)
for path in ['src/exec/op.h','src/net/conn.h','src/core/shard.h','src/store/flatstore.h',
             'src/net/rob.h','src/store/atomic_mvcc.h','src/core/read_local.h',
             'src/core/fused_boot_gate.h','src/core/placement.h','tests/gate.sh']:
    check(path + ': byte-identical', old(path), cur(path))
s = old('src/core/thread.h').split('struct ReadLocalStats {',1)[1].split('#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3',1)[0]
fields = re.findall(r'"'^'"\\s*uint64_t\\s+\\w+\\s*=\\s*0;', s, re.M)
assert len(fields) == 38, len(fields)
print('OK: production baseline sidecar = tick 8 + sink 24 + stats (38 + 3) * 8 = 360 bytes')
for path in ['tests/rl2s.py','tests/read_local_lane.py','tests/rlcache_churn.py','tests/bplus.py']:
    ast.parse(cur(path), filename=path)
print('OK: four changed Python batteries parse as AST; none imported or run')
maxrow = 'read_local_thread_%u:role=%s,shards=%u,active=%u,hits_total=%u,mget_hits_total=%u\\r\\n' % (2**32-1, 'unified', 2**32-1, 1, 2**64-1, 2**64-1)
assert len(maxrow) + 1 <= 192
print('OK: longest INFO thread row including NUL:', len(maxrow) + 1, '<= 192 bytes')
PY
git diff HEAD --check" in /home/user/Projects/cx-rl2s
 succeeded in 225ms:
OK: ordinary split construction / role loops / shutdown: byte-identical
OK: IoLoop::init: token-identical
OK: ExLoopT::init: token-identical
OK: IoLoop::run_split: token-identical
OK: IoLoop::run: token-identical
OK: parser / admission / dispatch: token-identical
OK: IO config refresh: token-identical
OK: owner task drain: token-identical
OK: Config fields: token-identical
OK: ReadLocalStats fields: token-identical
OK: IO rotation after discarding Fused-only statements: token-identical
OK: EX rotation after discarding Fused-only statements: token-identical
OK: owner config refresh with OwnsShards=true: token-identical
OK: src/core/io_loop.h complete member tail: token-identical
OK: src/core/ex_loop.h complete member tail: token-identical
OK: src/core/server.h complete member tail: token-identical
OK: ThreadCtx excluding two new methods: token-identical
OK: src/exec/op.h: byte-identical
OK: src/net/conn.h: byte-identical
OK: src/core/shard.h: byte-identical
OK: src/store/flatstore.h: byte-identical
OK: src/net/rob.h: byte-identical
OK: src/store/atomic_mvcc.h: byte-identical
OK: src/core/read_local.h: byte-identical
OK: src/core/fused_boot_gate.h: byte-identical
OK: src/core/placement.h: byte-identical
OK: tests/gate.sh: byte-identical
OK: production baseline sidecar = tick 8 + sink 24 + stats (38 + 3) * 8 = 360 bytes
OK: four changed Python batteries parse as AST; none imported or run
OK: longest INFO thread row including NUL: 140 <= 192 bytes

diff --git a/src/cmd/t_server.cc b/src/cmd/t_server.cc
index 2d5c81bfdb1c5ee1dcedddf26af8e1801a2ea4d3..7ad03034a03f1c5a2918a29c761c28fa54bdcb96
--- a/src/cmd/t_server.cc
+++ b/src/cmd/t_server.cc
@@ -14,6 +14,7 @@
 #include "server_tail.h"
 #include "slowlog.h"
 #include "../base/alloc.h"
+#include "../core/genthread.h"
 #include "../core/server.h"
 #include "../core/lbsignals.h"
 #include "../core/pubsub_event.h"
@@ -1992,33 +1993,10 @@
                 static_cast<unsigned>(g_server ? g_server->cfg().port : 0),
                 static_cast<unsigned long long>(uptime),
                 static_cast<unsigned long long>(uptime / 86400));
-        uint32_t active_readers = 0;
-        if (g_server) {
-            uint32_t owned[kMaxThreads] = {};
-            // Read the published ownership directory, never a vector FLIP may be rewriting.
-            for (uint32_t sid = 0; sid < g_server->nshards(); sid++)
-                owned[g_server->worker_of_shard(static_cast<int32_t>(sid))]++;
-            for (uint32_t tid = 0; tid < g_server->nthreads(); tid++) {
-                const ThreadCtx& thread = g_server->thread(tid);
-                const bool active = thread.read_local_lane_active();
-                active_readers += active;
-                const Role role = thread.role();
-                const char* name = role == Role::Idle ? "idle"
-                    : g_server->thread_mode() == ThreadMode::Fused ? "unified"
-                    : role == Role::Ifid ? "ifid" : "ex";
-                const ReadLocalStats* stats = g_server->read_local_enabled()
-                    ? &thread.read_local_stats() : nullptr;
-                // These per-thread totals are lifetime counters; INFO stats retains its existing
-                // RESETSTAT-relative aggregate counters. FLIP does not reset either sidecar.
-                appendf(body,
-                        "read_local_thread_%u:role=%s,shards=%u,active=%u,"
-                        "hits_total=%llu,mget_hits_total=%llu\r\n",
-                        tid, name, owned[tid], active ? 1u : 0u,
-                        static_cast<unsigned long long>(stats ? stats->hits : 0),
-                        static_cast<unsigned long long>(stats ? stats->mget_local_hits : 0));
-            }
-        }
-        appendf(body, "read_local_active_threads:%u\r\n", active_readers);
+        // Guard the CALL, including all argument evaluation. The cold non-inlined helper owns
+        // its scratch buffers so the disabled INFO path keeps its old output without those arrays.
+        if (g_server && g_server->read_local_enabled())
+            append_read_local_thread_info(body, *g_server);
         if (g_server && g_server->thread_mode() == ThreadMode::Fused) {
             appendf(body,
                     "fused_threads:%u\r\nclient_threads:%u\r\nowner_threads:%u\r\n"
diff --git a/src/core/genthread.h b/src/core/genthread.h
index c47b788f303c3a7e7f482d5ddc9f5a580a0a9bbb..e874192a1f6c2412955de097a7cd14587582ee0f
--- a/src/core/genthread.h
+++ b/src/core/genthread.h
@@ -3,6 +3,7 @@
 #pragma once
 
 #include <memory>
+#include <string>
 #include <vector>
 
 namespace tomo {
@@ -27,4 +28,7 @@
                      LateUnixListener& unix_listener,
                      ShutdownReportFinalLine& final_report);
 
+// INFO-only diagnostics. The caller must test read_local_enabled() before this call.
+void append_read_local_thread_info(std::string& body, const Server& server);
+
 }  // namespace tomo
diff --git a/src/core/rl2s.cc b/src/core/rl2s.cc
index d942a2e0f0f948a54a3f9becd0e2e32efb231a98..274b33481ca4d2e1e9e9b06d9d4dc9da575090e8
--- a/src/core/rl2s.cc
+++ b/src/core/rl2s.cc
@@ -305,4 +305,40 @@
     return 0;
 }
 
+// Keep the scratch arrays out of cmd_info's frame, including with LTO. Only an armed INFO SERVER
+// request reaches this helper; the boot/runtime paths install no reporting callback or sidecar.
+__attribute__((noinline, cold))
+void append_read_local_thread_info(std::string& body, const Server& srv) {
+    uint32_t active_readers = 0;
+    uint32_t owned[kMaxThreads] = {};
+    // Read the published ownership directory, never a vector FLIP may be rewriting.
+    for (uint32_t sid = 0; sid < srv.nshards(); sid++)
+        owned[srv.worker_of_shard(static_cast<int32_t>(sid))]++;
+    char row[192];  // fixed grammar, including full-width uint32/uint64 counters
+    for (uint32_t tid = 0; tid < srv.nthreads(); tid++) {
+        const ThreadCtx& thread = srv.thread(tid);
+        const bool active = thread.read_local_lane_active();
+        active_readers += active;
+        const Role role = thread.role();
+        const char* name = role == Role::Idle ? "idle"
+            : srv.thread_mode() == ThreadMode::Fused ? "unified"
+            : role == Role::Ifid ? "ifid" : "ex";
+        const ReadLocalStats& stats = thread.read_local_stats();
+        // Lifetime counters survive FLIP and RESETSTAT; INFO stats retains its existing
+        // RESETSTAT-relative aggregate counters.
+        const int n = std::snprintf(row, sizeof(row),
+            "read_local_thread_%u:role=%s,shards=%u,active=%u,"
+            "hits_total=%llu,mget_hits_total=%llu\r\n",
+            tid, name, owned[tid], active ? 1u : 0u,
+            static_cast<unsigned long long>(stats.hits),
+            static_cast<unsigned long long>(stats.mget_local_hits));
+        if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+        body.append(row, static_cast<size_t>(n));
+    }
+    const int n = std::snprintf(row, sizeof(row), "read_local_active_threads:%u\r\n",
+                              active_readers);
+    if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+    body.append(row, static_cast<size_t>(n));
+}
+
 }  // namespace tomo
diff --git a/src/core/thread.h b/src/core/thread.h
index adea9605d0f0d40c3566e07b90a2663accb6b835..61802252934cbd450279ad7cd2dbaa8d5f03f465
--- a/src/core/thread.h
+++ b/src/core/thread.h
@@ -303,6 +303,8 @@
     std::atomic<bool> lane_active{false};
 };
 #if TOMO_READ_LOCAL_SET_TAX_VARIANT != 3
+static_assert(offsetof(ReadLocalThreadState, lane_active) == 360,
+              "lane activation must follow the entire pre-RL2S sidecar");
 static_assert(sizeof(ReadLocalThreadState) == 368,
               "lane activation adds one alignment word only to the optional sidecar");
 #endif

exec
/bin/bash -lc "python3 - <<'PY'
from pathlib import Path
import re
root = Path.cwd()
def deps(name):
    seen = set()
    def walk(p):
        p = p.resolve()
        if p in seen or not p.exists() or root not in p.parents:
            return
        seen.add(p)
        for inc in re.findall(r'"'^'"\\s*#\\s*include\\s+\"(["'^"]+)"'"', p.read_text(), re.M):
            walk(p.parent / inc)
    walk(root / name)
    return seen
new = deps('src/core/rl2s.cc')
base = deps('src/core/genthread.cc') | deps('src/main.cc')
print('Project includes not already in main/genthread:', [str(p.relative_to(root)) for p in sorted(new - base)])
for p in sorted(new):
    for i, line in enumerate(p.read_text().splitlines(), 1):
        if re.search(r'\\bthread_local\\b|\\bstatic\\s+(?:const\\s+)?std::|\\binline\\s+(?:const\\s+)?std::|__attribute__\\s*\\(\\(constructor', line):
            print(f'{p.relative_to(root)}:{i}:{line.strip()}')
PY" in /home/user/Projects/cx-rl2s
 succeeded in 49ms:
Project includes not already in main/genthread: ['src/core/rl2s.cc']
src/base/topology.h:128:static const std::vector<int> empty;
src/cmd/climon.inc:53:static std::string climon_string(Slice value);
src/cmd/notify.h:92:inline std::string serialize_notify_flags(uint32_t flags) {
src/core/atomic_tripwire.h:119:inline std::unique_ptr<State> state;
src/core/atomic_tripwire.h:123:inline std::atomic<bool> armed{false};
src/core/atomic_tripwire.h:304:inline std::string atomic_tripwire_dump() {
src/core/config.h:158:inline std::string cfg_save_schedule_string(const std::vector<SaveClause>& clauses) {
src/core/config.h:201:inline std::string cfg_client_output_buffer_limit_string(const ClientOutputBufferLimits& limits) {
src/core/pubsub.inc:57:static std::string pubsub_string(Slice value) {
src/net/tls.h:38:static std::unique_ptr<TlsContext> create(const Config& cfg, std::string& error);
src/store/flatstore.h:104:inline std::atomic<uint32_t> g_flatstore_table_alloc_failures{0};

diff --git a/src/cmd/t_server.cc b/src/cmd/t_server.cc
index 2d5c81bfdb1c5ee1dcedddf26af8e1801a2ea4d3..7ad03034a03f1c5a2918a29c761c28fa54bdcb96
--- a/src/cmd/t_server.cc
+++ b/src/cmd/t_server.cc
@@ -14,6 +14,7 @@
 #include "server_tail.h"
 #include "slowlog.h"
 #include "../base/alloc.h"
+#include "../core/genthread.h"
 #include "../core/server.h"
 #include "../core/lbsignals.h"
 #include "../core/pubsub_event.h"
@@ -1992,33 +1993,10 @@
                 static_cast<unsigned>(g_server ? g_server->cfg().port : 0),
                 static_cast<unsigned long long>(uptime),
                 static_cast<unsigned long long>(uptime / 86400));
-        uint32_t active_readers = 0;
-        if (g_server) {
-            uint32_t owned[kMaxThreads] = {};
-            // Read the published ownership directory, never a vector FLIP may be rewriting.
-            for (uint32_t sid = 0; sid < g_server->nshards(); sid++)
-                owned[g_server->worker_of_shard(static_cast<int32_t>(sid))]++;
-            for (uint32_t tid = 0; tid < g_server->nthreads(); tid++) {
-                const ThreadCtx& thread = g_server->thread(tid);
-                const bool active = thread.read_local_lane_active();
-                active_readers += active;
-                const Role role = thread.role();
-                const char* name = role == Role::Idle ? "idle"
-                    : g_server->thread_mode() == ThreadMode::Fused ? "unified"
-                    : role == Role::Ifid ? "ifid" : "ex";
-                const ReadLocalStats* stats = g_server->read_local_enabled()
-                    ? &thread.read_local_stats() : nullptr;
-                // These per-thread totals are lifetime counters; INFO stats retains its existing
-                // RESETSTAT-relative aggregate counters. FLIP does not reset either sidecar.
-                appendf(body,
-                        "read_local_thread_%u:role=%s,shards=%u,active=%u,"
-                        "hits_total=%llu,mget_hits_total=%llu\r\n",
-                        tid, name, owned[tid], active ? 1u : 0u,
-                        static_cast<unsigned long long>(stats ? stats->hits : 0),
-                        static_cast<unsigned long long>(stats ? stats->mget_local_hits : 0));
-            }
-        }
-        appendf(body, "read_local_active_threads:%u\r\n", active_readers);
+        // Guard the CALL, including all argument evaluation. The cold non-inlined helper owns
+        // its scratch buffers so the disabled INFO path keeps its old output without those arrays.
+        if (g_server && g_server->read_local_enabled())
+            append_read_local_thread_info(body, *g_server);
         if (g_server && g_server->thread_mode() == ThreadMode::Fused) {
             appendf(body,
                     "fused_threads:%u\r\nclient_threads:%u\r\nowner_threads:%u\r\n"
diff --git a/src/core/genthread.h b/src/core/genthread.h
index c47b788f303c3a7e7f482d5ddc9f5a580a0a9bbb..e874192a1f6c2412955de097a7cd14587582ee0f
--- a/src/core/genthread.h
+++ b/src/core/genthread.h
@@ -3,6 +3,7 @@
 #pragma once
 
 #include <memory>
+#include <string>
 #include <vector>
 
 namespace tomo {
@@ -27,4 +28,7 @@
                      LateUnixListener& unix_listener,
                      ShutdownReportFinalLine& final_report);
 
+// INFO-only diagnostics. The caller must test read_local_enabled() before this call.
+void append_read_local_thread_info(std::string& body, const Server& server);
+
 }  // namespace tomo
diff --git a/src/core/rl2s.cc b/src/core/rl2s.cc
index d942a2e0f0f948a54a3f9becd0e2e32efb231a98..274b33481ca4d2e1e9e9b06d9d4dc9da575090e8
--- a/src/core/rl2s.cc
+++ b/src/core/rl2s.cc
@@ -305,4 +305,40 @@
     return 0;
 }
 
+// Keep the scratch arrays out of cmd_info's frame, including with LTO. Only an armed INFO SERVER
+// request reaches this helper; the boot/runtime paths install no reporting callback or sidecar.
+__attribute__((noinline, cold))
+void append_read_local_thread_info(std::string& body, const Server& srv) {
+    uint32_t active_readers = 0;
+    uint32_t owned[kMaxThreads] = {};
+    // Read the published ownership directory, never a vector FLIP may be rewriting.
+    for (uint32_t sid = 0; sid < srv.nshards(); sid++)
+        owned[srv.worker_of_shard(static_cast<int32_t>(sid))]++;
+    char row[192];  // fixed grammar, including full-width uint32/uint64 counters
+    for (uint32_t tid = 0; tid < srv.nthreads(); tid++) {
+        const ThreadCtx& thread = srv.thread(tid);
+        const bool active = thread.read_local_lane_active();
+        active_readers += active;
+        const Role role = thread.role();
+        const char* name = role == Role::Idle ? "idle"
+            : srv.thread_mode() == ThreadMode::Fused ? "unified"
+            : role == Role::Ifid ? "ifid" : "ex";
+        const ReadLocalStats& stats = thread.read_local_stats();
+        // Lifetime counters survive FLIP and RESETSTAT; INFO stats retains its existing
+        // RESETSTAT-relative aggregate counters.
+        const int n = std::snprintf(row, sizeof(row),
+            "read_local_thread_%u:role=%s,shards=%u,active=%u,"
+            "hits_total=%llu,mget_hits_total=%llu\r\n",
+            tid, name, owned[tid], active ? 1u : 0u,
+            static_cast<unsigned long long>(stats.hits),
+            static_cast<unsigned long long>(stats.mget_local_hits));
+        if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+        body.append(row, static_cast<size_t>(n));
+    }
+    const int n = std::snprintf(row, sizeof(row), "read_local_active_threads:%u\r\n",
+                              active_readers);
+    if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+    body.append(row, static_cast<size_t>(n));
+}
+
 }  // namespace tomo
diff --git a/src/core/thread.h b/src/core/thread.h
index adea9605d0f0d40c3566e07b90a2663accb6b835..61802252934cbd450279ad7cd2dbaa8d5f03f465
--- a/src/core/thread.h
+++ b/src/core/thread.h
@@ -303,6 +303,8 @@
     std::atomic<bool> lane_active{false};
 };
 #if TOMO_READ_LOCAL_SET_TAX_VARIANT != 3
+static_assert(offsetof(ReadLocalThreadState, lane_active) == 360,
+              "lane activation must follow the entire pre-RL2S sidecar");
 static_assert(sizeof(ReadLocalThreadState) == 368,
               "lane activation adds one alignment word only to the optional sidecar");
 #endif

exec
/bin/bash -lc "rg -n 'init_read_local_state|new \\(std::nothrow\\) ReadLocalThreadState|set_read_local_lane_active|read_local_lane_active|read_local_state_;|static_assert\\(offsetof\\(ReadLocalThreadState|static_assert\\(sizeof\\((ThreadCtx|ReadLocalThreadState)' src/core/thread.h
nl -ba src/core/io_loop.h | sed -n '407,432p'
nl -ba src/core/io_loop.h | sed -n '4325,4344p'
rg -n 'read_local_arm|mark_current_write' src/core/io_loop.h | head -15
nl -ba src/core/genthread.cc | sed -n '37,66p'
nl -ba src/cmd/t_server.cc | sed -n '1989,2005p'
nl -ba tests/gate.sh | sed -n '1253,1263p'" in /home/user/Projects/cx-rl2s
 succeeded in 0ms:
306:static_assert(offsetof(ReadLocalThreadState, lane_active) == 360,
308:static_assert(sizeof(ReadLocalThreadState) == 368,
353:    bool init_read_local_state() {
354:        read_local_state_.reset(new (std::nothrow) ReadLocalThreadState);
965:    void set_read_local_lane_active(bool active) {
969:    bool read_local_lane_active() const {
1205:    std::unique_ptr<ReadLocalThreadState> read_local_state_;
1208:static_assert(sizeof(ThreadCtx) == 1408,
   407	    void run_split() {
   408	        const bool has_unix = unix_listen_fd_ >= 0 ||
   409	                              (srv_->cfg().unixsocket && *srv_->cfg().unixsocket);
   410	        if (epoll_) {
   411	            if (tls_context_) {
   412	                if (has_unix) run_loop<true, true, true, false, Pipeline>();
   413	                else run_loop<false, true, true, false, Pipeline>();
   414	            } else {
   415	                if (has_unix) run_loop<true, false, true, false, Pipeline>();
   416	                else run_loop<false, false, true, false, Pipeline>();
   417	            }
   418	            return;
   419	        }
   420	        if (tls_context_) {
   421	            if (has_unix) run_loop<true, true, false, false, Pipeline>();
   422	            else run_loop<false, true, false, false, Pipeline>();
   423	        } else {
   424	            if (has_unix) run_loop<true, false, false, false, Pipeline>();
   425	            else run_loop<false, false, false, false, Pipeline>();
   426	        }
   427	    }
   428	
   429	    void run() {
   430	        if (srv_->cfg().overlap == 1) run_split<1>();
   431	        else                                  run_split<0>();
   432	    }
  4325	                            if (key_count <= ReadLocalRobState::kMaxPreciseKeysetKeys) {
  4326	                                const bool pending_reads = rob.has_pending_read_local();
  4327	                                bool pending_filter_hit = false;
  4328	                                for (uint32_t arg = 1; arg < op->argc(); arg += 2) {
  4329	                                    const uint64_t hash = FlatStore::hash_key(op->arg(arg));
  4330	                                    filter |= ReadLocalRobState::keyset_filter(hash);
  4331	                                    if (pending_reads && !pending_filter_hit)
  4332	                                        pending_filter_hit =
  4333	                                            rob.read_local_pending_may_touch(hash);
  4334	                                }
  4335	                                intersect_filter_miss =
  4336	                                    !pending_reads || !pending_filter_hit;
  4337	                            }
  4338	                            const bool keyset_precise =
  4339	                                rob.refine_current_write_keyset(filter, key_count);
  4340	                            if (keyset_precise) op->mark_read_local_precise_write();
  4341	                            if (!read_local_demotion.prepare(
  4342	                                    *this, c, 0, false, -1,
  4343	                                    ReadLocalFallbackReason::InflightWrite, false,
  4344	                                    nullptr, nullptr, 0,
2730:        client->rob().set_read_local_arm_stats(target.read_local_arm_stats_or_null());
3248:        c->rob().set_read_local_arm_stats(self_->read_local_arm_stats_or_null());
3500:        return rob.read_local_arm_fence_live()
4253:                        rob.mark_current_write();
    37	}  // namespace
    38	
    39	void IoLoop::run_fused() {
    40	    if (!fused_executor_) std::abort();
    41	    const bool has_unix = unix_listen_fd_ >= 0 ||
    42	                          (srv_->cfg().unixsocket && *srv_->cfg().unixsocket);
    43	    auto run_pipeline = [&](auto pipeline_tag) {
    44	        constexpr uint8_t Pipeline = decltype(pipeline_tag)::value;
    45	        if (epoll_) {
    46	            if (tls_context_) {
    47	                if (has_unix) run_loop<true, true, true, true, Pipeline>();
    48	                else run_loop<false, true, true, true, Pipeline>();
    49	            } else {
    50	                if (has_unix) run_loop<true, false, true, true, Pipeline>();
    51	                else run_loop<false, false, true, true, Pipeline>();
    52	            }
    53	            return;
    54	        }
    55	        if (tls_context_) {
    56	            if (has_unix) run_loop<true, true, false, true, Pipeline>();
    57	            else run_loop<false, true, false, true, Pipeline>();
    58	        } else {
    59	            if (has_unix) run_loop<true, false, false, true, Pipeline>();
    60	            else run_loop<false, false, false, true, Pipeline>();
    61	        }
    62	    };
    63	    switch (srv_->cfg().overlap) {
    64	        case 0: run_pipeline(std::integral_constant<uint8_t, 0>{}); break;
    65	        case 1: run_pipeline(std::integral_constant<uint8_t, 1>{}); break;
    66	        case 2: run_pipeline(std::integral_constant<uint8_t, 2>{}); break;
  1989	                g_server ? g_server->cfg().overlap : 0u,
  1990	                g_server && g_server->read_local_enabled() ? 1u : 0u,
  1991	                sizeof(void*) * 8,
  1992	                static_cast<long long>(::getpid()),
  1993	                static_cast<unsigned>(g_server ? g_server->cfg().port : 0),
  1994	                static_cast<unsigned long long>(uptime),
  1995	                static_cast<unsigned long long>(uptime / 86400));
  1996	        // Guard the CALL, including all argument evaluation. The cold non-inlined helper owns
  1997	        // its scratch buffers so the disabled INFO path keeps its old output without those arrays.
  1998	        if (g_server && g_server->read_local_enabled())
  1999	            append_read_local_thread_info(body, *g_server);
  2000	        if (g_server && g_server->thread_mode() == ThreadMode::Fused) {
  2001	            appendf(body,
  2002	                    "fused_threads:%u\r\nclient_threads:%u\r\nowner_threads:%u\r\n"
  2003	                    "flip_available:0\r\nflip_unavailable_reason:threads_are_fused\r\n",
  2004	                    g_server->nthreads(), g_server->client_serving_thread_count(),
  2005	                    g_server->shard_owner_count());
  1253	    && ok "TLS connection slots all freed ($TLS_FREED/$TLS_ACCEPTS)" \
  1254	    || bad "TLS connection-slot cleanup" "$TLS_FREED/$TLS_ACCEPTS"
  1255	[ -n "$TLS_ZC" ] && [ "$TLS_ZC" -gt 0 ] \
  1256	    && ok "TLS zc borrow gates fired (suppressed=$TLS_ZC)" || bad "TLS zc borrow gates fired"
  1257	
  1258	if [ "$TIER" = quick ]; then
  1259	  program_state "$EXPECT_QUICK"
  1260	  echo; echo "GATE(quick): $PASS ok, $FAIL FAIL"; [ $FAIL -eq 0 ] || exit 1; exit 0
  1261	fi
  1262	
  1263	# ---- 4. full tier: torture under ASAN ---------------------------------------------------------

diff --git a/src/cmd/t_server.cc b/src/cmd/t_server.cc
index 2d5c81bfdb1c5ee1dcedddf26af8e1801a2ea4d3..7ad03034a03f1c5a2918a29c761c28fa54bdcb96
--- a/src/cmd/t_server.cc
+++ b/src/cmd/t_server.cc
@@ -14,6 +14,7 @@
 #include "server_tail.h"
 #include "slowlog.h"
 #include "../base/alloc.h"
+#include "../core/genthread.h"
 #include "../core/server.h"
 #include "../core/lbsignals.h"
 #include "../core/pubsub_event.h"
@@ -1992,33 +1993,10 @@
                 static_cast<unsigned>(g_server ? g_server->cfg().port : 0),
                 static_cast<unsigned long long>(uptime),
                 static_cast<unsigned long long>(uptime / 86400));
-        uint32_t active_readers = 0;
-        if (g_server) {
-            uint32_t owned[kMaxThreads] = {};
-            // Read the published ownership directory, never a vector FLIP may be rewriting.
-            for (uint32_t sid = 0; sid < g_server->nshards(); sid++)
-                owned[g_server->worker_of_shard(static_cast<int32_t>(sid))]++;
-            for (uint32_t tid = 0; tid < g_server->nthreads(); tid++) {
-                const ThreadCtx& thread = g_server->thread(tid);
-                const bool active = thread.read_local_lane_active();
-                active_readers += active;
-                const Role role = thread.role();
-                const char* name = role == Role::Idle ? "idle"
-                    : g_server->thread_mode() == ThreadMode::Fused ? "unified"
-                    : role == Role::Ifid ? "ifid" : "ex";
-                const ReadLocalStats* stats = g_server->read_local_enabled()
-                    ? &thread.read_local_stats() : nullptr;
-                // These per-thread totals are lifetime counters; INFO stats retains its existing
-                // RESETSTAT-relative aggregate counters. FLIP does not reset either sidecar.
-                appendf(body,
-                        "read_local_thread_%u:role=%s,shards=%u,active=%u,"
-                        "hits_total=%llu,mget_hits_total=%llu\r\n",
-                        tid, name, owned[tid], active ? 1u : 0u,
-                        static_cast<unsigned long long>(stats ? stats->hits : 0),
-                        static_cast<unsigned long long>(stats ? stats->mget_local_hits : 0));
-            }
-        }
-        appendf(body, "read_local_active_threads:%u\r\n", active_readers);
+        // Guard the CALL, including all argument evaluation. The cold non-inlined helper owns
+        // its scratch buffers so the disabled INFO path keeps its old output without those arrays.
+        if (g_server && g_server->read_local_enabled())
+            append_read_local_thread_info(body, *g_server);
         if (g_server && g_server->thread_mode() == ThreadMode::Fused) {
             appendf(body,
                     "fused_threads:%u\r\nclient_threads:%u\r\nowner_threads:%u\r\n"
diff --git a/src/core/genthread.h b/src/core/genthread.h
index c47b788f303c3a7e7f482d5ddc9f5a580a0a9bbb..e874192a1f6c2412955de097a7cd14587582ee0f
--- a/src/core/genthread.h
+++ b/src/core/genthread.h
@@ -3,6 +3,7 @@
 #pragma once
 
 #include <memory>
+#include <string>
 #include <vector>
 
 namespace tomo {
@@ -27,4 +28,7 @@
                      LateUnixListener& unix_listener,
                      ShutdownReportFinalLine& final_report);
 
+// INFO-only diagnostics. The caller must test read_local_enabled() before this call.
+void append_read_local_thread_info(std::string& body, const Server& server);
+
 }  // namespace tomo
diff --git a/src/core/rl2s.cc b/src/core/rl2s.cc
index d942a2e0f0f948a54a3f9becd0e2e32efb231a98..274b33481ca4d2e1e9e9b06d9d4dc9da575090e8
--- a/src/core/rl2s.cc
+++ b/src/core/rl2s.cc
@@ -305,4 +305,40 @@
     return 0;
 }
 
+// Keep the scratch arrays out of cmd_info's frame, including with LTO. Only an armed INFO SERVER
+// request reaches this helper; the boot/runtime paths install no reporting callback or sidecar.
+__attribute__((noinline, cold))
+void append_read_local_thread_info(std::string& body, const Server& srv) {
+    uint32_t active_readers = 0;
+    uint32_t owned[kMaxThreads] = {};
+    // Read the published ownership directory, never a vector FLIP may be rewriting.
+    for (uint32_t sid = 0; sid < srv.nshards(); sid++)
+        owned[srv.worker_of_shard(static_cast<int32_t>(sid))]++;
+    char row[192];  // fixed grammar, including full-width uint32/uint64 counters
+    for (uint32_t tid = 0; tid < srv.nthreads(); tid++) {
+        const ThreadCtx& thread = srv.thread(tid);
+        const bool active = thread.read_local_lane_active();
+        active_readers += active;
+        const Role role = thread.role();
+        const char* name = role == Role::Idle ? "idle"
+            : srv.thread_mode() == ThreadMode::Fused ? "unified"
+            : role == Role::Ifid ? "ifid" : "ex";
+        const ReadLocalStats& stats = thread.read_local_stats();
+        // Lifetime counters survive FLIP and RESETSTAT; INFO stats retains its existing
+        // RESETSTAT-relative aggregate counters.
+        const int n = std::snprintf(row, sizeof(row),
+            "read_local_thread_%u:role=%s,shards=%u,active=%u,"
+            "hits_total=%llu,mget_hits_total=%llu\r\n",
+            tid, name, owned[tid], active ? 1u : 0u,
+            static_cast<unsigned long long>(stats.hits),
+            static_cast<unsigned long long>(stats.mget_local_hits));
+        if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+        body.append(row, static_cast<size_t>(n));
+    }
+    const int n = std::snprintf(row, sizeof(row), "read_local_active_threads:%u\r\n",
+                              active_readers);
+    if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+    body.append(row, static_cast<size_t>(n));
+}
+
 }  // namespace tomo
diff --git a/src/core/thread.h b/src/core/thread.h
index adea9605d0f0d40c3566e07b90a2663accb6b835..61802252934cbd450279ad7cd2dbaa8d5f03f465
--- a/src/core/thread.h
+++ b/src/core/thread.h
@@ -303,6 +303,8 @@
     std::atomic<bool> lane_active{false};
 };
 #if TOMO_READ_LOCAL_SET_TAX_VARIANT != 3
+static_assert(offsetof(ReadLocalThreadState, lane_active) == 360,
+              "lane activation must follow the entire pre-RL2S sidecar");
 static_assert(sizeof(ReadLocalThreadState) == 368,
               "lane activation adds one alignment word only to the optional sidecar");
 #endif

apply patch
patch: completed
/home/user/Projects/cx-rl2s/RL2S.md
diff --git a/RL2S.md b/RL2S.md
index 64dea226f85e5b4b296ed8c9f1de21ae463a8cf0..3a58bf02bfafd7ae3a505d79242145d586f17032
--- a/RL2S.md
+++ b/RL2S.md
@@ -76,7 +76,8 @@
 guards evaluation of `active_.size()`; no admission expression moved ahead
 of its old check.
 
-All physical threads provision an `IoLoop` and `FusedExLoop`. They use the
+On an enabled `2s/1` boot, all physical threads provision an `IoLoop` and
+`FusedExLoop`. They use the
 ordinary split task inbox layout and producer masks, including FLIP's
 existing remasking. Initial persistence loading runs before store arming.
 Each executor's `activate_fused` binds its permanent ThreadCtx sink, then
@@ -136,7 +137,8 @@
 1984, ThreadCtx 1408, Shard 1440, FlatStore 944, Rob<64> 192, AtomicEntry 144,
 Config 624; the ordinary ExLoop assert remains 6112. The only data layout
 change is the optional `ReadLocalThreadState`: 360 -> 368 bytes in the
-production build, asserted at `src/core/thread.h:306`. An atomic loop-entry
+production build, asserted at `src/core/thread.h:308`; its old 360-byte
+prefix is also locked at `src/core/thread.h:306`. An atomic loop-entry
 flag plus alignment occupies the extra word, after all existing members.
 The diagnostics-only selector-3 sidecar already has different alignment
 and is excluded from this production-size assert. Read-local 0 allocates
@@ -148,7 +150,8 @@
 ## Observable activation
 
 `INFO server` still reports `thread_mode:2s`, live `io_threads` and
-`ex_threads`, and effective `read_local:1`. It now also reports:
+`ex_threads`, and effective `read_local:1`. When the effective lane is
+enabled, it also reports:
 
 ```text
 read_local_thread_0:role=ifid,shards=0,active=1,hits_total=123,mget_hits_total=7
@@ -161,10 +164,15 @@
 network waits; it is not inferred from the knob or ownership. The atomic
 flag is written only at loop entry/exit. Shard counts come from published
 owner IDs, not concurrently modified ownership vectors
-(`src/cmd/t_server.cc:1995`). After FLIP settles, every IFID row must have
+(`src/core/rl2s.cc:313`). After FLIP settles, every IFID row must have
 shards=0/active=1, and every EX row active=0. As with other INFO geometry,
 the individual samples are not one transactional view during a move.
 
+With read-local disabled, both new field families are absent, preserving
+the baseline INFO payload. Their caller guard is at
+`src/cmd/t_server.cc:1998`; their scratch arrays and formatting live in the
+non-inlined helper at `src/core/rl2s.cc:310`.
+
 Per-thread `*_total` values are lifetime completions and survive FLIP and
 CONFIG RESETSTAT. They can be nonzero on an EX row after that thread has
 previously served IO. Aggregate `INFO stats` keeps its existing RESETSTAT
@@ -203,6 +211,167 @@
 row on either side of that exit. Updating existing battery internals does
 not change gate row counts.
 
+## OFF-ARM PROOF
+
+Compared against **`78c3e5391`**, for **`--thread-mode 2s --read-local 0`**,
+including its supported overlap 0 and 1 schedules. References below name
+the final working-tree sources unless explicitly marked baseline. This is
+a source and construction proof; no build, server, benchmark or gate was
+run. It does not establish identical generated instructions or measured
+PRE/POST rates.
+
+**Finding and fix.** The submitted diff did not preserve disabled INFO:
+it counted shard owners, visited every thread and formatted
+`read_local_thread_*` and `read_local_active_threads` on off boots too.
+That changed replies and could grow INFO's output allocation. Its
+`owned[kMaxThreads]` array also lived in the common `cmd_info` frame.
+The fix guards the **call** with the effective predicate at
+`src/cmd/t_server.cc:1998`, and moves the entire walk, scratch arrays and
+formatting into `append_read_local_thread_info` in `src/core/rl2s.cc:310`.
+`noinline` keeps those arrays out of the caller, including under LTO.
+The helper receives only references, after the guard; it does not take
+an eagerly computed topology snapshot or allocate before early-returning.
+Disabled INFO emits neither new field family. Enabled INFO keeps both.
+The offset assert at `src/core/thread.h:306` additionally locks the old
+sidecar prefix; it adds no runtime state. The lane implementation is kept.
+
+### 1. Threads, capabilities and roles: unchanged off
+
+The effective predicate is `cfg.overlap == 0 && cfg.read_local != 0`
+(`src/core/server.h:582`). It is false when the knob is zero. The only two
+thread modes are Split and Fused (`src/core/config.h:225`): `main` returns
+through the existing fused path at `src/main.cc:312`, then tests the
+predicate at `:317`. Thus the **new runtime** can be called only for
+Split **and** read-local 1, with overlap 0. Its entry also checks this
+contract before construction (`src/core/rl2s.cc:77`). Mode, overlap and
+read-local remain boot-only CONFIG entries (`src/cmd/t_server.cc:322`).
+
+Off falls through to the old `vector<IoLoop>` and `vector<ExLoop>` at
+`src/main.cc:338`; `ExLoop` still means **`ExLoopT<false>`**
+(`src/core/ex_loop.h:3439`). It does not construct the new
+`vector<FusedExLoop>` at `src/core/rl2s.cc:92`, bind a fused executor,
+call `activate_fused`, or enter `run_fused`.
+
+Placement still builds the ordinary split ratio/explicit placement
+(`src/core/server.h:239`), creates `placement_.total_threads()` contexts
+(`:294`, `:305`), and assigns shard homes to EX tids
+(`src/core/placement.h:178`, `src/core/server.h:367`). Actual OS workers
+are created once per EX tid at `src/main.cc:372` and once per IFID tid at
+`:507`. At gate geometry this is **8 workers: 6 IFID, 2 EX, 16 shards on
+EX**, not an additional reader tier. Main remains the existing monitor.
+
+Baseline already provisioned the opposite dormant loop on every worker
+for FLIP: dormant IO on EX at `src/main.cc:395`, dormant ordinary EX on IO
+at `:528`. Those allocations/capabilities are not new. Off role changes
+still call only `ios[tid].run()` / `exs[tid].run()` (`:426`, `:562`), so
+FLIP cannot later upgrade an off worker to the reader runtime. The entire
+ordinary split construction/role-loop/shutdown suffix starting at
+`src/main.cc:336` was compared byte-for-byte with baseline and is identical.
+
+### 2. Allocations, buffers, sinks and registrations: none added off
+
+The inventory includes existing allocations newly reachable by the armed
+split runtime, not just literal `new` lines in `rl2s.cc`.
+
+| Construction or allocation | Guard and off-arm result |
+| --- | --- |
+| Server read-local sidecar (`new ReadLocalServerState`) | Existing predicate guard at `src/core/server.h:316`, allocation `:317`. False off; the existing owning pointer stays null. |
+| Per-thread sidecars, tick publications, stats and sink slots | Same guard encloses the thread loop at `src/core/server.h:322`; `src/core/thread.h:353` performs `new ReadLocalThreadState`. Never called off, including for future FLIP readers/owners. |
+| Per-shard extended store state and foreign-read safety arrays | Same guard encloses `prepare_read_local` at `src/core/server.h:328`. It reaches `new ReadLocalStoreState` at `src/store/flatstore_atomic.inc:890`. Off stores retain the original ordinary atomic-state allocation path. |
+| `ReadLocalExImpl`, `LaneEntry[kInboxSlots]`, fallback array | `src/core/ex_loop.h:196` first requires compile-time `Fused`, then the effective predicate at `:197`; the three `new` sites are `:198`, `:200`, `:202`. The entire block is discarded for the off `ExLoopT<false>`. |
+| Deferred `Entry[kCapacity]` buffer, block-cache heads and retire sink | Deferred initialization is inside that same block (`src/core/ex_loop.h:204`); buffer allocation is `src/core/read_local.h:260`, sink binding `:262`. Cache heads and retire-ring bookkeeping are members of the optional queue (`:464`), not a baseline per-thread allocation. No off queue exists to grow debug pending sets or retain blocks. |
+| Runtime vectors, default-constructed loop containers and worker closures | `pool`, `ios(nthreads)`, `executors(nthreads)` at `src/core/rl2s.cc:90`, then `pool.emplace_back` at `:122`, are inside the guarded runtime call. This includes executor deque/map allocations during default construction, not merely `init`. The off runtime constructs only the baseline vectors/loops described above. |
+| Boot-gate worker-state vector, loading/error strings and shutdown report buffers | `FusedBootGate boot(nthreads)` at `src/core/rl2s.cc:95` allocates its vector in `src/core/fused_boot_gate.h:23`. The new runtime's local strings (`src/core/rl2s.cc:130`, `:235`, `:263`, `:281`) and report (`:115`) share the same call guard. No static boot gate or report is constructed for off. |
+| Inbox channels, IO/EX rings, network registration and listener resources | All new-runtime calls are below its guard: `src/core/rl2s.cc:127`, `:129`, `:144`, `:196`, `:248`, `:264`, `:270`. Inbox storage comes from `src/core/thread.h:338`; rings from `src/core/ex_loop.h:213` and `src/core/io_loop.h:102`. Off calls the unchanged baseline initializers with the same parameters; it does not additionally run any of these RL2S initializers. |
+| Role/client registration, capacity, completion, demotion and progress hooks | `src/core/rl2s.cc:147`, `:152`, `:160`, `:165`, `:167`, `:172`, `:181` are inside the enabled runtime. Fused sink publication and store arming occur in `src/core/ex_loop.h:241` and `:245`. Off never installs those fused hooks; its pre-existing ordinary IO registration hooks remain. |
+| Connection RYOW sidecar and local-read reply reserves | Parser admission remains behind compile-time `Fused` and the effective predicate (`src/core/io_loop.h:4200`). Its write marking at `:4253` can reach `Rob::prepare_read_local` (`src/net/rob.h:486`, `:709`) only in that armed path. Local-copy reply reservation (`src/core/ex_loop.h:990`) likewise belongs to the existing reader lane. No new accept-time buffer or sidecar allocation was added. |
+| New INFO ownership array, row buffer and output growth | Fixed: caller guard at `src/cmd/t_server.cc:1998` precedes helper entry; `owned` and `row` exist only in `src/core/rl2s.cc:313`, `:317`. Both `body.append` sites (`:336`, `:341`) are inside that helper. Off emits the baseline bytes and takes none of these buffer allocations. |
+
+There is no new `.reserve()`/`.resize()` call in the feature file and no
+file-scope runtime object or thread-local registration. Its project header
+dependencies were already included by main/genthread. All feature-owned
+vectors, strings, arrays and OS-thread creation are local to the two
+guarded entry points. Existing optional sidecars store the hooks; no
+separate always-present registry was introduced.
+
+QSBR registration is the optional per-thread state allocated at boot,
+not mere membership in the baseline `threads_` vector. Its floor scanner
+(`src/core/server.h:606`) is driven by the optional deferred queues.
+Neither the scanner, a tick/park publication, nor a sink binding is added
+to an off thread. Existing store ownership-edge checks remain unchanged
+(`src/core/server.h:1951`) and return for unarmed stores.
+
+### 3. Hot loops and eager arguments: no added off-arm branch
+
+| Changed test or call | Why off does not execute it |
+| --- | --- |
+| Runtime selection and overlap validation | `src/main.cc:317` and `src/core/config.h:1215` are boot-only, before operation processing. The changed startup notice at `src/main.cc:183` is also cold and silent with read-local 0. |
+| IO reader-entry resume/epoch/active publication and exit publication | Both are inside `if constexpr (Fused)` at `src/core/io_loop.h:541` and `:766`. Ordinary `run()` selects `run_split<0/1>` (`:429`); every network/TLS/unix variant passes `Fused=false` (`:407`). The new calls and **all their arguments**, including `read_local_epoch()`, are discarded. Entry/exit are tenure boundaries even on the enabled arm. |
+| EX parked/lane-count check, config invalidation, debug ownership check and retire drain | All additions to `ExLoopT::run` are inside `if constexpr (Fused)` (`src/core/ex_loop.h:708`, `:723`, `:800`). Off instantiates `ExLoopT<false>`; it does not evaluate `read_local_impl()`, `self_->id()` for the new check, or epoch/sink arguments. This also holds with `TOMO_RL_CACHE_DEBUG`. |
+| New mode tests in baseline fused pass/sweep | `src/core/ex_loop.h:397` and `:640` live in `static_assert(Fused)` entry points. Off never calls them. They do add a rotation-boundary test to the existing **1s** schedule; the 1s PRE/POST measurement obligation remains. |
+| Shard-less pass's config, clock, pressure and cap checks | Entirely in the enabled-only `ExLoopT<true>::split_read_local_pass` (`src/core/rl2s.cc:46`). No callback to it is installed off. |
+| `OwnsShards` config-refresh selection | `src/core/ex_loop.h:1913` defaults to **true** on all old calls. `if constexpr (OwnsShards)` adds no runtime test. Only `src/core/rl2s.cc:49` calls `<false>`. Expanding the true block gives the baseline owner-refresh token sequence. |
+| Changed shared `Server::read_local_enabled` predicate | The ordinary parser's `Fused && ...` at `src/core/io_loop.h:3924` short-circuits at compile time. In common IO config refresh, the existing version-change return at `:7135` still precedes the predicate at `:7146`; changing the predicate adds no check to unchanged-version rotations. Other shared uses are boot/config preparation and INFO/RESETSTAT, not the GET/SET dispatch/execution loops. |
+| New INFO caller guard | `src/cmd/t_server.cc:1998` is inside the INFO SERVER command section (`:1975`), not any general operation or worker loop. All new per-shard/per-thread reporting loops are behind it in the cold helper. |
+
+The parser/admission/dispatch function at `src/core/io_loop.h:3918` is
+unchanged. In particular, `if constexpr (Fused)` and the enabled/pressure
+tests at `:3992` still precede `read_local_lane_quota(active_.size())` at
+`:3995`. Off evaluates **neither** the quota call **nor** `active_.size()`;
+there is no new cold-cache-line load disguised as a no-op callee.
+
+Source comparisons discarded the `if constexpr (Fused)` statements in
+both revisions of IO/EX rotation bodies; the remaining token sequences
+match. The initializers, ordinary split dispatch entry points, parser,
+owner task drain and IO config-refresh bodies also match baseline tokens.
+These comparisons check the source specialization, not compiler output.
+
+### 4. Per-operation layouts: unchanged off; optional growth disclosed
+
+| Type | Baseline bytes | Current bytes | Existing layout assert |
+| --- | ---: | ---: | --- |
+| `Op` | 336 | 336 | `src/exec/op.h:463` |
+| `Client` | 1984 | 1984 | `src/net/conn.h:905` |
+| `ThreadCtx` | 1408 | 1408 | `src/core/thread.h:1208` |
+| `Shard` | 1440 | 1440 | `src/core/shard.h:523` |
+| `FlatStore` | 944 | 944 | `src/store/flatstore.h:3912` |
+| `Rob<64>` | 192 | 192 | `src/net/rob.h:1098` |
+| `AtomicEntry` | 144 | 144 | `src/store/atomic_mvcc.h:70` |
+| `Config` | 624 | 624 | `src/core/config.h:429` |
+| Ordinary `ExLoop` | 6112 | 6112 | `src/core/ex_loop.h:3450` |
+
+No data members were added/reordered in `IoLoop`, `Server`, `ThreadCtx`
+or `ExLoopT`: their member declarations match baseline, including the
+existing optional pointer in `ThreadCtx`'s tail (`src/core/thread.h:1205`)
+and the empty `ReadLocalExState<false>` (`src/core/ex_loop.h:103`, `:165`)
+held with `[[no_unique_address]]` (`:3436`). The other locked type files
+are byte-identical; Config differs only in its field's comment. Thus no
+off-arm object has a new allocation stride, field offset or cache line.
+IoLoop and Server have no numeric whole-size assert here; their unchanged
+member/type layouts, rather than an invented size, are the evidence.
+
+**One on-arm sidecar did grow:** production `ReadLocalThreadState`
+**360 -> 368 bytes**. Baseline has an 8-byte tick, a 24-byte sink
+(`src/store/read_local_reclaim.h:61`) and 328-byte stats (38 `uint64_t`
+counters plus three arm counters: `src/core/thread.h:137`,
+`src/net/rob.h:62`). The added `atomic<bool> lane_active` follows them
+(`src/core/thread.h:303`); alignment adds seven padding bytes. The old
+prefix offset is now asserted as 360 at `:306`, total size 368 at `:308`.
+The diagnostics-only selector 3 retains its existing separate alignment
+and is excluded from these production asserts. It too uses only optional
+state. **Off pays zero bytes and no cache line for this growth**, because
+`Server::init` never calls `ThreadCtx::init_read_local_state` off. Existing
+hot fields remain at their old offsets on both arms.
+
+After the INFO fix, all four off-arm source properties above hold. Static
+verification completed: baseline source/token comparisons, Python AST
+parsing of the four changed batteries (without importing/running them),
+and `git diff HEAD --check`. The maintainer still owns build/layout-assert
+execution, both-mode boots, the gate and the PRE/POST rates. No gate row
+was added or retired; quick/full expectations remain **327/344**. The
+quick-tier branch starts at `tests/gate.sh:1258` and exits at `:1260`; the
+standalone RL2S battery has no row on either side of it.
+
 ## MEASUREMENT PLAN
 
 The maintainer performs all steps below, on the quiet box. No performance
@@ -221,7 +390,9 @@
    row automatically.
 2. Boot the four overlap-0 combinations: 2s/0, 2s/1, 1s/0, 1s/1. On eight
    real cores use 6:2 for split; omit ratio for 1s. Require INFO's effective
-   lane bit and active-reader count to match: 0, 6, 0, 8. Also smoke 2s/1
+   lane bit to be 0, 1, 0, 1. Enabled active-reader counts must be 6 and 8;
+   off arms must omit the new active-count/per-thread fields and retain
+   their baseline INFO payload. Also smoke 2s/1
    with one shard and 6:2 (one EX has no shard), explicit placement, and
    the existing TCP/TLS/unix and epoll/uring boot/shutdown cases. Confirm
    split/1/overlap-1 fails with the documented message.
diff --git a/src/cmd/t_server.cc b/src/cmd/t_server.cc
index 2d5c81bfdb1c5ee1dcedddf26af8e1801a2ea4d3..7ad03034a03f1c5a2918a29c761c28fa54bdcb96
--- a/src/cmd/t_server.cc
+++ b/src/cmd/t_server.cc
@@ -14,6 +14,7 @@
 #include "server_tail.h"
 #include "slowlog.h"
 #include "../base/alloc.h"
+#include "../core/genthread.h"
 #include "../core/server.h"
 #include "../core/lbsignals.h"
 #include "../core/pubsub_event.h"
@@ -1992,33 +1993,10 @@
                 static_cast<unsigned>(g_server ? g_server->cfg().port : 0),
                 static_cast<unsigned long long>(uptime),
                 static_cast<unsigned long long>(uptime / 86400));
-        uint32_t active_readers = 0;
-        if (g_server) {
-            uint32_t owned[kMaxThreads] = {};
-            // Read the published ownership directory, never a vector FLIP may be rewriting.
-            for (uint32_t sid = 0; sid < g_server->nshards(); sid++)
-                owned[g_server->worker_of_shard(static_cast<int32_t>(sid))]++;
-            for (uint32_t tid = 0; tid < g_server->nthreads(); tid++) {
-                const ThreadCtx& thread = g_server->thread(tid);
-                const bool active = thread.read_local_lane_active();
-                active_readers += active;
-                const Role role = thread.role();
-                const char* name = role == Role::Idle ? "idle"
-                    : g_server->thread_mode() == ThreadMode::Fused ? "unified"
-                    : role == Role::Ifid ? "ifid" : "ex";
-                const ReadLocalStats* stats = g_server->read_local_enabled()
-                    ? &thread.read_local_stats() : nullptr;
-                // These per-thread totals are lifetime counters; INFO stats retains its existing
-                // RESETSTAT-relative aggregate counters. FLIP does not reset either sidecar.
-                appendf(body,
-                        "read_local_thread_%u:role=%s,shards=%u,active=%u,"
-                        "hits_total=%llu,mget_hits_total=%llu\r\n",
-                        tid, name, owned[tid], active ? 1u : 0u,
-                        static_cast<unsigned long long>(stats ? stats->hits : 0),
-                        static_cast<unsigned long long>(stats ? stats->mget_local_hits : 0));
-            }
-        }
-        appendf(body, "read_local_active_threads:%u\r\n", active_readers);
+        // Guard the CALL, including all argument evaluation. The cold non-inlined helper owns
+        // its scratch buffers so the disabled INFO path keeps its old output without those arrays.
+        if (g_server && g_server->read_local_enabled())
+            append_read_local_thread_info(body, *g_server);
         if (g_server && g_server->thread_mode() == ThreadMode::Fused) {
             appendf(body,
                     "fused_threads:%u\r\nclient_threads:%u\r\nowner_threads:%u\r\n"
diff --git a/src/core/genthread.h b/src/core/genthread.h
index c47b788f303c3a7e7f482d5ddc9f5a580a0a9bbb..e874192a1f6c2412955de097a7cd14587582ee0f
--- a/src/core/genthread.h
+++ b/src/core/genthread.h
@@ -3,6 +3,7 @@
 #pragma once
 
 #include <memory>
+#include <string>
 #include <vector>
 
 namespace tomo {
@@ -27,4 +28,7 @@
                      LateUnixListener& unix_listener,
                      ShutdownReportFinalLine& final_report);
 
+// INFO-only diagnostics. The caller must test read_local_enabled() before this call.
+void append_read_local_thread_info(std::string& body, const Server& server);
+
 }  // namespace tomo
diff --git a/src/core/rl2s.cc b/src/core/rl2s.cc
index d942a2e0f0f948a54a3f9becd0e2e32efb231a98..274b33481ca4d2e1e9e9b06d9d4dc9da575090e8
--- a/src/core/rl2s.cc
+++ b/src/core/rl2s.cc
@@ -305,4 +305,40 @@
     return 0;
 }
 
+// Keep the scratch arrays out of cmd_info's frame, including with LTO. Only an armed INFO SERVER
+// request reaches this helper; the boot/runtime paths install no reporting callback or sidecar.
+__attribute__((noinline, cold))
+void append_read_local_thread_info(std::string& body, const Server& srv) {
+    uint32_t active_readers = 0;
+    uint32_t owned[kMaxThreads] = {};
+    // Read the published ownership directory, never a vector FLIP may be rewriting.
+    for (uint32_t sid = 0; sid < srv.nshards(); sid++)
+        owned[srv.worker_of_shard(static_cast<int32_t>(sid))]++;
+    char row[192];  // fixed grammar, including full-width uint32/uint64 counters
+    for (uint32_t tid = 0; tid < srv.nthreads(); tid++) {
+        const ThreadCtx& thread = srv.thread(tid);
+        const bool active = thread.read_local_lane_active();
+        active_readers += active;
+        const Role role = thread.role();
+        const char* name = role == Role::Idle ? "idle"
+            : srv.thread_mode() == ThreadMode::Fused ? "unified"
+            : role == Role::Ifid ? "ifid" : "ex";
+        const ReadLocalStats& stats = thread.read_local_stats();
+        // Lifetime counters survive FLIP and RESETSTAT; INFO stats retains its existing
+        // RESETSTAT-relative aggregate counters.
+        const int n = std::snprintf(row, sizeof(row),
+            "read_local_thread_%u:role=%s,shards=%u,active=%u,"
+            "hits_total=%llu,mget_hits_total=%llu\r\n",
+            tid, name, owned[tid], active ? 1u : 0u,
+            static_cast<unsigned long long>(stats.hits),
+            static_cast<unsigned long long>(stats.mget_local_hits));
+        if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+        body.append(row, static_cast<size_t>(n));
+    }
+    const int n = std::snprintf(row, sizeof(row), "read_local_active_threads:%u\r\n",
+                              active_readers);
+    if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+    body.append(row, static_cast<size_t>(n));
+}
+
 }  // namespace tomo
diff --git a/src/core/thread.h b/src/core/thread.h
index adea9605d0f0d40c3566e07b90a2663accb6b835..61802252934cbd450279ad7cd2dbaa8d5f03f465
--- a/src/core/thread.h
+++ b/src/core/thread.h
@@ -303,6 +303,8 @@
     std::atomic<bool> lane_active{false};
 };
 #if TOMO_READ_LOCAL_SET_TAX_VARIANT != 3
+static_assert(offsetof(ReadLocalThreadState, lane_active) == 360,
+              "lane activation must follow the entire pre-RL2S sidecar");
 static_assert(sizeof(ReadLocalThreadState) == 368,
               "lane activation adds one alignment word only to the optional sidecar");
 #endif

diff --git a/RL2S.md b/RL2S.md
index 64dea226f85e5b4b296ed8c9f1de21ae463a8cf0..3a58bf02bfafd7ae3a505d79242145d586f17032
--- a/RL2S.md
+++ b/RL2S.md
@@ -76,7 +76,8 @@
 guards evaluation of `active_.size()`; no admission expression moved ahead
 of its old check.
 
-All physical threads provision an `IoLoop` and `FusedExLoop`. They use the
+On an enabled `2s/1` boot, all physical threads provision an `IoLoop` and
+`FusedExLoop`. They use the
 ordinary split task inbox layout and producer masks, including FLIP's
 existing remasking. Initial persistence loading runs before store arming.
 Each executor's `activate_fused` binds its permanent ThreadCtx sink, then
@@ -136,7 +137,8 @@
 1984, ThreadCtx 1408, Shard 1440, FlatStore 944, Rob<64> 192, AtomicEntry 144,
 Config 624; the ordinary ExLoop assert remains 6112. The only data layout
 change is the optional `ReadLocalThreadState`: 360 -> 368 bytes in the
-production build, asserted at `src/core/thread.h:306`. An atomic loop-entry
+production build, asserted at `src/core/thread.h:308`; its old 360-byte
+prefix is also locked at `src/core/thread.h:306`. An atomic loop-entry
 flag plus alignment occupies the extra word, after all existing members.
 The diagnostics-only selector-3 sidecar already has different alignment
 and is excluded from this production-size assert. Read-local 0 allocates
@@ -148,7 +150,8 @@
 ## Observable activation
 
 `INFO server` still reports `thread_mode:2s`, live `io_threads` and
-`ex_threads`, and effective `read_local:1`. It now also reports:
+`ex_threads`, and effective `read_local:1`. When the effective lane is
+enabled, it also reports:
 
 ```text
 read_local_thread_0:role=ifid,shards=0,active=1,hits_total=123,mget_hits_total=7
@@ -161,10 +164,15 @@
 network waits; it is not inferred from the knob or ownership. The atomic
 flag is written only at loop entry/exit. Shard counts come from published
 owner IDs, not concurrently modified ownership vectors
-(`src/cmd/t_server.cc:1995`). After FLIP settles, every IFID row must have
+(`src/core/rl2s.cc:313`). After FLIP settles, every IFID row must have
 shards=0/active=1, and every EX row active=0. As with other INFO geometry,
 the individual samples are not one transactional view during a move.
 
+With read-local disabled, both new field families are absent, preserving
+the baseline INFO payload. Their caller guard is at
+`src/cmd/t_server.cc:1998`; their scratch arrays and formatting live in the
+non-inlined helper at `src/core/rl2s.cc:310`.
+
 Per-thread `*_total` values are lifetime completions and survive FLIP and
 CONFIG RESETSTAT. They can be nonzero on an EX row after that thread has
 previously served IO. Aggregate `INFO stats` keeps its existing RESETSTAT
@@ -203,6 +211,167 @@
 row on either side of that exit. Updating existing battery internals does
 not change gate row counts.
 
+## OFF-ARM PROOF
+
+Compared against **`78c3e5391`**, for **`--thread-mode 2s --read-local 0`**,
+including its supported overlap 0 and 1 schedules. References below name
+the final working-tree sources unless explicitly marked baseline. This is
+a source and construction proof; no build, server, benchmark or gate was
+run. It does not establish identical generated instructions or measured
+PRE/POST rates.
+
+**Finding and fix.** The submitted diff did not preserve disabled INFO:
+it counted shard owners, visited every thread and formatted
+`read_local_thread_*` and `read_local_active_threads` on off boots too.
+That changed replies and could grow INFO's output allocation. Its
+`owned[kMaxThreads]` array also lived in the common `cmd_info` frame.
+The fix guards the **call** with the effective predicate at
+`src/cmd/t_server.cc:1998`, and moves the entire walk, scratch arrays and
+formatting into `append_read_local_thread_info` in `src/core/rl2s.cc:310`.
+`noinline` keeps those arrays out of the caller, including under LTO.
+The helper receives only references, after the guard; it does not take
+an eagerly computed topology snapshot or allocate before early-returning.
+Disabled INFO emits neither new field family. Enabled INFO keeps both.
+The offset assert at `src/core/thread.h:306` additionally locks the old
+sidecar prefix; it adds no runtime state. The lane implementation is kept.
+
+### 1. Threads, capabilities and roles: unchanged off
+
+The effective predicate is `cfg.overlap == 0 && cfg.read_local != 0`
+(`src/core/server.h:582`). It is false when the knob is zero. The only two
+thread modes are Split and Fused (`src/core/config.h:225`): `main` returns
+through the existing fused path at `src/main.cc:312`, then tests the
+predicate at `:317`. Thus the **new runtime** can be called only for
+Split **and** read-local 1, with overlap 0. Its entry also checks this
+contract before construction (`src/core/rl2s.cc:77`). Mode, overlap and
+read-local remain boot-only CONFIG entries (`src/cmd/t_server.cc:322`).
+
+Off falls through to the old `vector<IoLoop>` and `vector<ExLoop>` at
+`src/main.cc:338`; `ExLoop` still means **`ExLoopT<false>`**
+(`src/core/ex_loop.h:3439`). It does not construct the new
+`vector<FusedExLoop>` at `src/core/rl2s.cc:92`, bind a fused executor,
+call `activate_fused`, or enter `run_fused`.
+
+Placement still builds the ordinary split ratio/explicit placement
+(`src/core/server.h:239`), creates `placement_.total_threads()` contexts
+(`:294`, `:305`), and assigns shard homes to EX tids
+(`src/core/placement.h:178`, `src/core/server.h:367`). Actual OS workers
+are created once per EX tid at `src/main.cc:372` and once per IFID tid at
+`:507`. At gate geometry this is **8 workers: 6 IFID, 2 EX, 16 shards on
+EX**, not an additional reader tier. Main remains the existing monitor.
+
+Baseline already provisioned the opposite dormant loop on every worker
+for FLIP: dormant IO on EX at `src/main.cc:395`, dormant ordinary EX on IO
+at `:528`. Those allocations/capabilities are not new. Off role changes
+still call only `ios[tid].run()` / `exs[tid].run()` (`:426`, `:562`), so
+FLIP cannot later upgrade an off worker to the reader runtime. The entire
+ordinary split construction/role-loop/shutdown suffix starting at
+`src/main.cc:336` was compared byte-for-byte with baseline and is identical.
+
+### 2. Allocations, buffers, sinks and registrations: none added off
+
+The inventory includes existing allocations newly reachable by the armed
+split runtime, not just literal `new` lines in `rl2s.cc`.
+
+| Construction or allocation | Guard and off-arm result |
+| --- | --- |
+| Server read-local sidecar (`new ReadLocalServerState`) | Existing predicate guard at `src/core/server.h:316`, allocation `:317`. False off; the existing owning pointer stays null. |
+| Per-thread sidecars, tick publications, stats and sink slots | Same guard encloses the thread loop at `src/core/server.h:322`; `src/core/thread.h:353` performs `new ReadLocalThreadState`. Never called off, including for future FLIP readers/owners. |
+| Per-shard extended store state and foreign-read safety arrays | Same guard encloses `prepare_read_local` at `src/core/server.h:328`. It reaches `new ReadLocalStoreState` at `src/store/flatstore_atomic.inc:890`. Off stores retain the original ordinary atomic-state allocation path. |
+| `ReadLocalExImpl`, `LaneEntry[kInboxSlots]`, fallback array | `src/core/ex_loop.h:196` first requires compile-time `Fused`, then the effective predicate at `:197`; the three `new` sites are `:198`, `:200`, `:202`. The entire block is discarded for the off `ExLoopT<false>`. |
+| Deferred `Entry[kCapacity]` buffer, block-cache heads and retire sink | Deferred initialization is inside that same block (`src/core/ex_loop.h:204`); buffer allocation is `src/core/read_local.h:260`, sink binding `:262`. Cache heads and retire-ring bookkeeping are members of the optional queue (`:464`), not a baseline per-thread allocation. No off queue exists to grow debug pending sets or retain blocks. |
+| Runtime vectors, default-constructed loop containers and worker closures | `pool`, `ios(nthreads)`, `executors(nthreads)` at `src/core/rl2s.cc:90`, then `pool.emplace_back` at `:122`, are inside the guarded runtime call. This includes executor deque/map allocations during default construction, not merely `init`. The off runtime constructs only the baseline vectors/loops described above. |
+| Boot-gate worker-state vector, loading/error strings and shutdown report buffers | `FusedBootGate boot(nthreads)` at `src/core/rl2s.cc:95` allocates its vector in `src/core/fused_boot_gate.h:23`. The new runtime's local strings (`src/core/rl2s.cc:130`, `:235`, `:263`, `:281`) and report (`:115`) share the same call guard. No static boot gate or report is constructed for off. |
+| Inbox channels, IO/EX rings, network registration and listener resources | All new-runtime calls are below its guard: `src/core/rl2s.cc:127`, `:129`, `:144`, `:196`, `:248`, `:264`, `:270`. Inbox storage comes from `src/core/thread.h:338`; rings from `src/core/ex_loop.h:213` and `src/core/io_loop.h:102`. Off calls the unchanged baseline initializers with the same parameters; it does not additionally run any of these RL2S initializers. |
+| Role/client registration, capacity, completion, demotion and progress hooks | `src/core/rl2s.cc:147`, `:152`, `:160`, `:165`, `:167`, `:172`, `:181` are inside the enabled runtime. Fused sink publication and store arming occur in `src/core/ex_loop.h:241` and `:245`. Off never installs those fused hooks; its pre-existing ordinary IO registration hooks remain. |
+| Connection RYOW sidecar and local-read reply reserves | Parser admission remains behind compile-time `Fused` and the effective predicate (`src/core/io_loop.h:4200`). Its write marking at `:4253` can reach `Rob::prepare_read_local` (`src/net/rob.h:486`, `:709`) only in that armed path. Local-copy reply reservation (`src/core/ex_loop.h:990`) likewise belongs to the existing reader lane. No new accept-time buffer or sidecar allocation was added. |
+| New INFO ownership array, row buffer and output growth | Fixed: caller guard at `src/cmd/t_server.cc:1998` precedes helper entry; `owned` and `row` exist only in `src/core/rl2s.cc:313`, `:317`. Both `body.append` sites (`:336`, `:341`) are inside that helper. Off emits the baseline bytes and takes none of these buffer allocations. |
+
+There is no new `.reserve()`/`.resize()` call in the feature file and no
+file-scope runtime object or thread-local registration. Its project header
+dependencies were already included by main/genthread. All feature-owned
+vectors, strings, arrays and OS-thread creation are local to the two
+guarded entry points. Existing optional sidecars store the hooks; no
+separate always-present registry was introduced.
+
+QSBR registration is the optional per-thread state allocated at boot,
+not mere membership in the baseline `threads_` vector. Its floor scanner
+(`src/core/server.h:606`) is driven by the optional deferred queues.
+Neither the scanner, a tick/park publication, nor a sink binding is added
+to an off thread. Existing store ownership-edge checks remain unchanged
+(`src/core/server.h:1951`) and return for unarmed stores.
+
+### 3. Hot loops and eager arguments: no added off-arm branch
+
+| Changed test or call | Why off does not execute it |
+| --- | --- |
+| Runtime selection and overlap validation | `src/main.cc:317` and `src/core/config.h:1215` are boot-only, before operation processing. The changed startup notice at `src/main.cc:183` is also cold and silent with read-local 0. |
+| IO reader-entry resume/epoch/active publication and exit publication | Both are inside `if constexpr (Fused)` at `src/core/io_loop.h:541` and `:766`. Ordinary `run()` selects `run_split<0/1>` (`:429`); every network/TLS/unix variant passes `Fused=false` (`:407`). The new calls and **all their arguments**, including `read_local_epoch()`, are discarded. Entry/exit are tenure boundaries even on the enabled arm. |
+| EX parked/lane-count check, config invalidation, debug ownership check and retire drain | All additions to `ExLoopT::run` are inside `if constexpr (Fused)` (`src/core/ex_loop.h:708`, `:723`, `:800`). Off instantiates `ExLoopT<false>`; it does not evaluate `read_local_impl()`, `self_->id()` for the new check, or epoch/sink arguments. This also holds with `TOMO_RL_CACHE_DEBUG`. |
+| New mode tests in baseline fused pass/sweep | `src/core/ex_loop.h:397` and `:640` live in `static_assert(Fused)` entry points. Off never calls them. They do add a rotation-boundary test to the existing **1s** schedule; the 1s PRE/POST measurement obligation remains. |
+| Shard-less pass's config, clock, pressure and cap checks | Entirely in the enabled-only `ExLoopT<true>::split_read_local_pass` (`src/core/rl2s.cc:46`). No callback to it is installed off. |
+| `OwnsShards` config-refresh selection | `src/core/ex_loop.h:1913` defaults to **true** on all old calls. `if constexpr (OwnsShards)` adds no runtime test. Only `src/core/rl2s.cc:49` calls `<false>`. Expanding the true block gives the baseline owner-refresh token sequence. |
+| Changed shared `Server::read_local_enabled` predicate | The ordinary parser's `Fused && ...` at `src/core/io_loop.h:3924` short-circuits at compile time. In common IO config refresh, the existing version-change return at `:7135` still precedes the predicate at `:7146`; changing the predicate adds no check to unchanged-version rotations. Other shared uses are boot/config preparation and INFO/RESETSTAT, not the GET/SET dispatch/execution loops. |
+| New INFO caller guard | `src/cmd/t_server.cc:1998` is inside the INFO SERVER command section (`:1975`), not any general operation or worker loop. All new per-shard/per-thread reporting loops are behind it in the cold helper. |
+
+The parser/admission/dispatch function at `src/core/io_loop.h:3918` is
+unchanged. In particular, `if constexpr (Fused)` and the enabled/pressure
+tests at `:3992` still precede `read_local_lane_quota(active_.size())` at
+`:3995`. Off evaluates **neither** the quota call **nor** `active_.size()`;
+there is no new cold-cache-line load disguised as a no-op callee.
+
+Source comparisons discarded the `if constexpr (Fused)` statements in
+both revisions of IO/EX rotation bodies; the remaining token sequences
+match. The initializers, ordinary split dispatch entry points, parser,
+owner task drain and IO config-refresh bodies also match baseline tokens.
+These comparisons check the source specialization, not compiler output.
+
+### 4. Per-operation layouts: unchanged off; optional growth disclosed
+
+| Type | Baseline bytes | Current bytes | Existing layout assert |
+| --- | ---: | ---: | --- |
+| `Op` | 336 | 336 | `src/exec/op.h:463` |
+| `Client` | 1984 | 1984 | `src/net/conn.h:905` |
+| `ThreadCtx` | 1408 | 1408 | `src/core/thread.h:1208` |
+| `Shard` | 1440 | 1440 | `src/core/shard.h:523` |
+| `FlatStore` | 944 | 944 | `src/store/flatstore.h:3912` |
+| `Rob<64>` | 192 | 192 | `src/net/rob.h:1098` |
+| `AtomicEntry` | 144 | 144 | `src/store/atomic_mvcc.h:70` |
+| `Config` | 624 | 624 | `src/core/config.h:429` |
+| Ordinary `ExLoop` | 6112 | 6112 | `src/core/ex_loop.h:3450` |
+
+No data members were added/reordered in `IoLoop`, `Server`, `ThreadCtx`
+or `ExLoopT`: their member declarations match baseline, including the
+existing optional pointer in `ThreadCtx`'s tail (`src/core/thread.h:1205`)
+and the empty `ReadLocalExState<false>` (`src/core/ex_loop.h:103`, `:165`)
+held with `[[no_unique_address]]` (`:3436`). The other locked type files
+are byte-identical; Config differs only in its field's comment. Thus no
+off-arm object has a new allocation stride, field offset or cache line.
+IoLoop and Server have no numeric whole-size assert here; their unchanged
+member/type layouts, rather than an invented size, are the evidence.
+
+**One on-arm sidecar did grow:** production `ReadLocalThreadState`
+**360 -> 368 bytes**. Baseline has an 8-byte tick, a 24-byte sink
+(`src/store/read_local_reclaim.h:61`) and 328-byte stats (38 `uint64_t`
+counters plus three arm counters: `src/core/thread.h:137`,
+`src/net/rob.h:62`). The added `atomic<bool> lane_active` follows them
+(`src/core/thread.h:303`); alignment adds seven padding bytes. The old
+prefix offset is now asserted as 360 at `:306`, total size 368 at `:308`.
+The diagnostics-only selector 3 retains its existing separate alignment
+and is excluded from these production asserts. It too uses only optional
+state. **Off pays zero bytes and no cache line for this growth**, because
+`Server::init` never calls `ThreadCtx::init_read_local_state` off. Existing
+hot fields remain at their old offsets on both arms.
+
+After the INFO fix, all four off-arm source properties above hold. Static
+verification completed: baseline source/token comparisons, Python AST
+parsing of the four changed batteries (without importing/running them),
+and `git diff HEAD --check`. The maintainer still owns build/layout-assert
+execution, both-mode boots, the gate and the PRE/POST rates. No gate row
+was added or retired; quick/full expectations remain **327/344**. The
+quick-tier branch starts at `tests/gate.sh:1258` and exits at `:1260`; the
+standalone RL2S battery has no row on either side of it.
+
 ## MEASUREMENT PLAN
 
 The maintainer performs all steps below, on the quiet box. No performance
@@ -221,7 +390,9 @@
    row automatically.
 2. Boot the four overlap-0 combinations: 2s/0, 2s/1, 1s/0, 1s/1. On eight
    real cores use 6:2 for split; omit ratio for 1s. Require INFO's effective
-   lane bit and active-reader count to match: 0, 6, 0, 8. Also smoke 2s/1
+   lane bit to be 0, 1, 0, 1. Enabled active-reader counts must be 6 and 8;
+   off arms must omit the new active-count/per-thread fields and retain
+   their baseline INFO payload. Also smoke 2s/1
    with one shard and 6:2 (one EX has no shard), explicit placement, and
    the existing TCP/TLS/unix and epoll/uring boot/shutdown cases. Confirm
    split/1/overlap-1 fails with the documented message.
diff --git a/src/cmd/t_server.cc b/src/cmd/t_server.cc
index 2d5c81bfdb1c5ee1dcedddf26af8e1801a2ea4d3..7ad03034a03f1c5a2918a29c761c28fa54bdcb96
--- a/src/cmd/t_server.cc
+++ b/src/cmd/t_server.cc
@@ -14,6 +14,7 @@
 #include "server_tail.h"
 #include "slowlog.h"
 #include "../base/alloc.h"
+#include "../core/genthread.h"
 #include "../core/server.h"
 #include "../core/lbsignals.h"
 #include "../core/pubsub_event.h"
@@ -1992,33 +1993,10 @@
                 static_cast<unsigned>(g_server ? g_server->cfg().port : 0),
                 static_cast<unsigned long long>(uptime),
                 static_cast<unsigned long long>(uptime / 86400));
-        uint32_t active_readers = 0;
-        if (g_server) {
-            uint32_t owned[kMaxThreads] = {};
-            // Read the published ownership directory, never a vector FLIP may be rewriting.
-            for (uint32_t sid = 0; sid < g_server->nshards(); sid++)
-                owned[g_server->worker_of_shard(static_cast<int32_t>(sid))]++;
-            for (uint32_t tid = 0; tid < g_server->nthreads(); tid++) {
-                const ThreadCtx& thread = g_server->thread(tid);
-                const bool active = thread.read_local_lane_active();
-                active_readers += active;
-                const Role role = thread.role();
-                const char* name = role == Role::Idle ? "idle"
-                    : g_server->thread_mode() == ThreadMode::Fused ? "unified"
-                    : role == Role::Ifid ? "ifid" : "ex";
-                const ReadLocalStats* stats = g_server->read_local_enabled()
-                    ? &thread.read_local_stats() : nullptr;
-                // These per-thread totals are lifetime counters; INFO stats retains its existing
-                // RESETSTAT-relative aggregate counters. FLIP does not reset either sidecar.
-                appendf(body,
-                        "read_local_thread_%u:role=%s,shards=%u,active=%u,"
-                        "hits_total=%llu,mget_hits_total=%llu\r\n",
-                        tid, name, owned[tid], active ? 1u : 0u,
-                        static_cast<unsigned long long>(stats ? stats->hits : 0),
-                        static_cast<unsigned long long>(stats ? stats->mget_local_hits : 0));
-            }
-        }
-        appendf(body, "read_local_active_threads:%u\r\n", active_readers);
+        // Guard the CALL, including all argument evaluation. The cold non-inlined helper owns
+        // its scratch buffers so the disabled INFO path keeps its old output without those arrays.
+        if (g_server && g_server->read_local_enabled())
+            append_read_local_thread_info(body, *g_server);
         if (g_server && g_server->thread_mode() == ThreadMode::Fused) {
             appendf(body,
                     "fused_threads:%u\r\nclient_threads:%u\r\nowner_threads:%u\r\n"
diff --git a/src/core/genthread.h b/src/core/genthread.h
index c47b788f303c3a7e7f482d5ddc9f5a580a0a9bbb..e874192a1f6c2412955de097a7cd14587582ee0f
--- a/src/core/genthread.h
+++ b/src/core/genthread.h
@@ -3,6 +3,7 @@
 #pragma once
 
 #include <memory>
+#include <string>
 #include <vector>
 
 namespace tomo {
@@ -27,4 +28,7 @@
                      LateUnixListener& unix_listener,
                      ShutdownReportFinalLine& final_report);
 
+// INFO-only diagnostics. The caller must test read_local_enabled() before this call.
+void append_read_local_thread_info(std::string& body, const Server& server);
+
 }  // namespace tomo
diff --git a/src/core/rl2s.cc b/src/core/rl2s.cc
index d942a2e0f0f948a54a3f9becd0e2e32efb231a98..274b33481ca4d2e1e9e9b06d9d4dc9da575090e8
--- a/src/core/rl2s.cc
+++ b/src/core/rl2s.cc
@@ -305,4 +305,40 @@
     return 0;
 }
 
+// Keep the scratch arrays out of cmd_info's frame, including with LTO. Only an armed INFO SERVER
+// request reaches this helper; the boot/runtime paths install no reporting callback or sidecar.
+__attribute__((noinline, cold))
+void append_read_local_thread_info(std::string& body, const Server& srv) {
+    uint32_t active_readers = 0;
+    uint32_t owned[kMaxThreads] = {};
+    // Read the published ownership directory, never a vector FLIP may be rewriting.
+    for (uint32_t sid = 0; sid < srv.nshards(); sid++)
+        owned[srv.worker_of_shard(static_cast<int32_t>(sid))]++;
+    char row[192];  // fixed grammar, including full-width uint32/uint64 counters
+    for (uint32_t tid = 0; tid < srv.nthreads(); tid++) {
+        const ThreadCtx& thread = srv.thread(tid);
+        const bool active = thread.read_local_lane_active();
+        active_readers += active;
+        const Role role = thread.role();
+        const char* name = role == Role::Idle ? "idle"
+            : srv.thread_mode() == ThreadMode::Fused ? "unified"
+            : role == Role::Ifid ? "ifid" : "ex";
+        const ReadLocalStats& stats = thread.read_local_stats();
+        // Lifetime counters survive FLIP and RESETSTAT; INFO stats retains its existing
+        // RESETSTAT-relative aggregate counters.
+        const int n = std::snprintf(row, sizeof(row),
+            "read_local_thread_%u:role=%s,shards=%u,active=%u,"
+            "hits_total=%llu,mget_hits_total=%llu\r\n",
+            tid, name, owned[tid], active ? 1u : 0u,
+            static_cast<unsigned long long>(stats.hits),
+            static_cast<unsigned long long>(stats.mget_local_hits));
+        if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+        body.append(row, static_cast<size_t>(n));
+    }
+    const int n = std::snprintf(row, sizeof(row), "read_local_active_threads:%u\r\n",
+                              active_readers);
+    if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+    body.append(row, static_cast<size_t>(n));
+}
+
 }  // namespace tomo
diff --git a/src/core/thread.h b/src/core/thread.h
index adea9605d0f0d40c3566e07b90a2663accb6b835..61802252934cbd450279ad7cd2dbaa8d5f03f465
--- a/src/core/thread.h
+++ b/src/core/thread.h
@@ -303,6 +303,8 @@
     std::atomic<bool> lane_active{false};
 };
 #if TOMO_READ_LOCAL_SET_TAX_VARIANT != 3
+static_assert(offsetof(ReadLocalThreadState, lane_active) == 360,
+              "lane activation must follow the entire pre-RL2S sidecar");
 static_assert(sizeof(ReadLocalThreadState) == 368,
               "lane activation adds one alignment word only to the optional sidecar");
 #endif

exec
/bin/bash -lc "nl -ba src/core/rl2s.cc | sed -n '306,346p'
nl -ba src/store/read_local_reclaim.h | tail -12
rg -n 'struct ReadLocalSetTaxStats|alignas|uint64_t|std::|\\[["'^]]*'"\\]' src/store/read_local_settax.h | head -92
nl -ba RL2S.md | sed -n '222,268p'
git diff HEAD --check
git status --short" in /home/user/Projects/cx-rl2s
 succeeded in 0ms:
   306	}
   307	
   308	// Keep the scratch arrays out of cmd_info's frame, including with LTO. Only an armed INFO SERVER
   309	// request reaches this helper; the boot/runtime paths install no reporting callback or sidecar.
   310	__attribute__((noinline, cold))
   311	void append_read_local_thread_info(std::string& body, const Server& srv) {
   312	    uint32_t active_readers = 0;
   313	    uint32_t owned[kMaxThreads] = {};
   314	    // Read the published ownership directory, never a vector FLIP may be rewriting.
   315	    for (uint32_t sid = 0; sid < srv.nshards(); sid++)
   316	        owned[srv.worker_of_shard(static_cast<int32_t>(sid))]++;
   317	    char row[192];  // fixed grammar, including full-width uint32/uint64 counters
   318	    for (uint32_t tid = 0; tid < srv.nthreads(); tid++) {
   319	        const ThreadCtx& thread = srv.thread(tid);
   320	        const bool active = thread.read_local_lane_active();
   321	        active_readers += active;
   322	        const Role role = thread.role();
   323	        const char* name = role == Role::Idle ? "idle"
   324	            : srv.thread_mode() == ThreadMode::Fused ? "unified"
   325	            : role == Role::Ifid ? "ifid" : "ex";
   326	        const ReadLocalStats& stats = thread.read_local_stats();
   327	        // Lifetime counters survive FLIP and RESETSTAT; INFO stats retains its existing
   328	        // RESETSTAT-relative aggregate counters.
   329	        const int n = std::snprintf(row, sizeof(row),
   330	            "read_local_thread_%u:role=%s,shards=%u,active=%u,"
   331	            "hits_total=%llu,mget_hits_total=%llu\r\n",
   332	            tid, name, owned[tid], active ? 1u : 0u,
   333	            static_cast<unsigned long long>(stats.hits),
   334	            static_cast<unsigned long long>(stats.mget_local_hits));
   335	        if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
   336	        body.append(row, static_cast<size_t>(n));
   337	    }
   338	    const int n = std::snprintf(row, sizeof(row), "read_local_active_threads:%u\r\n",
   339	                              active_readers);
   340	    if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
   341	    body.append(row, static_cast<size_t>(n));
   342	}
   343	
   344	}  // namespace tomo
    54	#else
    55	        return nullptr;
    56	#endif
    57	    }
    58	};
    59	
    60	#if TOMO_READ_LOCAL_SET_TAX_VARIANT != 3
    61	static_assert(sizeof(ReadLocalRetireSink) == 3 * sizeof(void*),
    62	              "the shipped retire sink is context + defer + block cache and nothing else");
    63	#endif
    64	
    65	}  // namespace tomo
42:struct ReadLocalSetTaxStats;
48:struct ReadLocalSetTaxStats {
51:    uint64_t overwrite_hits = 0;
52:    uint64_t reject_missing = 0;
53:    uint64_t reject_encoding = 0;
54:    uint64_t reject_ttl = 0;
55:    uint64_t reject_oversize = 0;
56:    uint64_t reject_size_class = 0;
57:    uint64_t reject_borrowed = 0;
58:    uint64_t reject_sequence_saturated = 0;
59:    uint64_t overwrite_maxmemory_oom = 0;
61:    uint64_t init_raw_calls = 0;
62:    uint64_t init_int_calls = 0;
63:    uint64_t init_extern_calls = 0;
64:    uint64_t init_key_bytes = 0;
65:    uint64_t init_value_bytes = 0;
66:    uint64_t init_cell_prepare_calls = 0;
67:    uint64_t fresh_allocation_attempts = 0;
68:    uint64_t accounting_add_calls = 0;
69:    uint64_t accounting_sub_calls = 0;
70:    uint64_t accounting_bytes = 0;
71:    uint64_t slot_replacements = 0;
72:    uint64_t expire_erases = 0;
75:    uint64_t recycle_acquire_attempts = 0;
76:    uint64_t recycle_acquire_hits = 0;
77:    uint64_t recycle_acquire_ineligible = 0;
78:    uint64_t recycle_acquire_empty = 0;
79:    uint64_t recycle_return_attempts = 0;
80:    uint64_t recycle_return_accepted = 0;
81:    uint64_t recycle_return_ineligible = 0;
82:    uint64_t recycle_return_limited = 0;
83:    uint64_t recycle_pool_nodes = 0;
84:    uint64_t recycle_pool_max_owner_nodes = 0;
85:    uint64_t recycle_capacity_evals = 0;
86:    uint64_t recycle_candidate_attempts = 0;
87:    uint64_t recycle_reject_not_string = 0;
88:    uint64_t recycle_reject_encoding = 0;
89:    uint64_t recycle_reject_borrowed = 0;
90:    uint64_t recycle_atomic_pool_accepts = 0;
92:    uint64_t qsbr_deferrals = 0;
93:    uint64_t qsbr_object_deferrals = 0;
94:    uint64_t qsbr_table_deferrals = 0;
95:    uint64_t qsbr_depth = 0;
96:    uint64_t qsbr_max_owner_depth = 0;
98:    uint64_t qsbr_depth_samples = 0;
99:    uint64_t qsbr_depth_sum = 0;
100:    uint64_t qsbr_seals = 0;
101:    uint64_t qsbr_sealed_entries = 0;
102:    uint64_t qsbr_grace_scans = 0;
103:    uint64_t qsbr_participant_loads = 0;
104:    uint64_t qsbr_zero_progress_scans = 0;
105:    uint64_t qsbr_reclaims = 0;
106:    uint64_t qsbr_forced_graces = 0;
107:    uint64_t qsbr_forced_yields = 0;
109:    uint64_t object_sequence_retries = 0;
   222	
   223	**Finding and fix.** The submitted diff did not preserve disabled INFO:
   224	it counted shard owners, visited every thread and formatted
   225	`read_local_thread_*` and `read_local_active_threads` on off boots too.
   226	That changed replies and could grow INFO's output allocation. Its
   227	`owned[kMaxThreads]` array also lived in the common `cmd_info` frame.
   228	The fix guards the **call** with the effective predicate at
   229	`src/cmd/t_server.cc:1998`, and moves the entire walk, scratch arrays and
   230	formatting into `append_read_local_thread_info` in `src/core/rl2s.cc:310`.
   231	`noinline` keeps those arrays out of the caller, including under LTO.
   232	The helper receives only references, after the guard; it does not take
   233	an eagerly computed topology snapshot or allocate before early-returning.
   234	Disabled INFO emits neither new field family. Enabled INFO keeps both.
   235	The offset assert at `src/core/thread.h:306` additionally locks the old
   236	sidecar prefix; it adds no runtime state. The lane implementation is kept.
   237	
   238	### 1. Threads, capabilities and roles: unchanged off
   239	
   240	The effective predicate is `cfg.overlap == 0 && cfg.read_local != 0`
   241	(`src/core/server.h:582`). It is false when the knob is zero. The only two
   242	thread modes are Split and Fused (`src/core/config.h:225`): `main` returns
   243	through the existing fused path at `src/main.cc:312`, then tests the
   244	predicate at `:317`. Thus the **new runtime** can be called only for
   245	Split **and** read-local 1, with overlap 0. Its entry also checks this
   246	contract before construction (`src/core/rl2s.cc:77`). Mode, overlap and
   247	read-local remain boot-only CONFIG entries (`src/cmd/t_server.cc:322`).
   248	
   249	Off falls through to the old `vector<IoLoop>` and `vector<ExLoop>` at
   250	`src/main.cc:338`; `ExLoop` still means **`ExLoopT<false>`**
   251	(`src/core/ex_loop.h:3439`). It does not construct the new
   252	`vector<FusedExLoop>` at `src/core/rl2s.cc:92`, bind a fused executor,
   253	call `activate_fused`, or enter `run_fused`.
   254	
   255	Placement still builds the ordinary split ratio/explicit placement
   256	(`src/core/server.h:239`), creates `placement_.total_threads()` contexts
   257	(`:294`, `:305`), and assigns shard homes to EX tids
   258	(`src/core/placement.h:178`, `src/core/server.h:367`). Actual OS workers
   259	are created once per EX tid at `src/main.cc:372` and once per IFID tid at
   260	`:507`. At gate geometry this is **8 workers: 6 IFID, 2 EX, 16 shards on
   261	EX**, not an additional reader tier. Main remains the existing monitor.
   262	
   263	Baseline already provisioned the opposite dormant loop on every worker
   264	for FLIP: dormant IO on EX at `src/main.cc:395`, dormant ordinary EX on IO
   265	at `:528`. Those allocations/capabilities are not new. Off role changes
   266	still call only `ios[tid].run()` / `exs[tid].run()` (`:426`, `:562`), so
   267	FLIP cannot later upgrade an off worker to the reader runtime. The entire
   268	ordinary split construction/role-loop/shutdown suffix starting at
 M Makefile
 A RL2S.md
 M src/cmd/t_server.cc
 M src/core/config.h
 M src/core/ex_loop.h
 M src/core/genthread.cc
 M src/core/genthread.h
 M src/core/io_loop.h
 A src/core/rl2s.cc
 M src/core/server.h
 M src/core/thread.h
 M src/main.cc
 M tests/bplus.py
 M tests/config_parser_test.cc
 M tests/read_local_lane.py
 A tests/rl2s.py
 M tests/rlcache_churn.py
?? CODEX-OUT.md

exec
/bin/bash -lc "python3 - <<'PY'
from pathlib import Path
import re
s = Path('RL2S.md').read_text()
proof = s.split('## OFF-ARM PROOF', 1)[1].split('## MEASUREMENT PLAN', 1)[0]
refs = sorted(set(re.findall(r'(src/[a-zA-Z0-9_./]+|tests/gate\\.sh):(\\d+)', proof)))
for f, n in refs:
    p = Path(f)
    assert p.exists(), f
    lines = p.read_text().splitlines()
    assert 0 < int(n) <= len(lines), (f, n)
print('OFF-ARM PROOF references:', len(refs), 'file:line targets exist')
assert proof.count('### ') == 4
print('OFF-ARM PROOF contains all four numbered answers')
PY" in /home/user/Projects/cx-rl2s
 succeeded in 0ms:
OFF-ARM PROOF references: 68 file:line targets exist
OFF-ARM PROOF contains all four numbered answers

diff --git a/RL2S.md b/RL2S.md
index 64dea226f85e5b4b296ed8c9f1de21ae463a8cf0..3a58bf02bfafd7ae3a505d79242145d586f17032
--- a/RL2S.md
+++ b/RL2S.md
@@ -76,7 +76,8 @@
 guards evaluation of `active_.size()`; no admission expression moved ahead
 of its old check.
 
-All physical threads provision an `IoLoop` and `FusedExLoop`. They use the
+On an enabled `2s/1` boot, all physical threads provision an `IoLoop` and
+`FusedExLoop`. They use the
 ordinary split task inbox layout and producer masks, including FLIP's
 existing remasking. Initial persistence loading runs before store arming.
 Each executor's `activate_fused` binds its permanent ThreadCtx sink, then
@@ -136,7 +137,8 @@
 1984, ThreadCtx 1408, Shard 1440, FlatStore 944, Rob<64> 192, AtomicEntry 144,
 Config 624; the ordinary ExLoop assert remains 6112. The only data layout
 change is the optional `ReadLocalThreadState`: 360 -> 368 bytes in the
-production build, asserted at `src/core/thread.h:306`. An atomic loop-entry
+production build, asserted at `src/core/thread.h:308`; its old 360-byte
+prefix is also locked at `src/core/thread.h:306`. An atomic loop-entry
 flag plus alignment occupies the extra word, after all existing members.
 The diagnostics-only selector-3 sidecar already has different alignment
 and is excluded from this production-size assert. Read-local 0 allocates
@@ -148,7 +150,8 @@
 ## Observable activation
 
 `INFO server` still reports `thread_mode:2s`, live `io_threads` and
-`ex_threads`, and effective `read_local:1`. It now also reports:
+`ex_threads`, and effective `read_local:1`. When the effective lane is
+enabled, it also reports:
 
 ```text
 read_local_thread_0:role=ifid,shards=0,active=1,hits_total=123,mget_hits_total=7
@@ -161,10 +164,15 @@
 network waits; it is not inferred from the knob or ownership. The atomic
 flag is written only at loop entry/exit. Shard counts come from published
 owner IDs, not concurrently modified ownership vectors
-(`src/cmd/t_server.cc:1995`). After FLIP settles, every IFID row must have
+(`src/core/rl2s.cc:313`). After FLIP settles, every IFID row must have
 shards=0/active=1, and every EX row active=0. As with other INFO geometry,
 the individual samples are not one transactional view during a move.
 
+With read-local disabled, both new field families are absent, preserving
+the baseline INFO payload. Their caller guard is at
+`src/cmd/t_server.cc:1998`; their scratch arrays and formatting live in the
+non-inlined helper at `src/core/rl2s.cc:310`.
+
 Per-thread `*_total` values are lifetime completions and survive FLIP and
 CONFIG RESETSTAT. They can be nonzero on an EX row after that thread has
 previously served IO. Aggregate `INFO stats` keeps its existing RESETSTAT
@@ -203,6 +211,167 @@
 row on either side of that exit. Updating existing battery internals does
 not change gate row counts.
 
+## OFF-ARM PROOF
+
+Compared against **`78c3e5391`**, for **`--thread-mode 2s --read-local 0`**,
+including its supported overlap 0 and 1 schedules. References below name
+the final working-tree sources unless explicitly marked baseline. This is
+a source and construction proof; no build, server, benchmark or gate was
+run. It does not establish identical generated instructions or measured
+PRE/POST rates.
+
+**Finding and fix.** The submitted diff did not preserve disabled INFO:
+it counted shard owners, visited every thread and formatted
+`read_local_thread_*` and `read_local_active_threads` on off boots too.
+That changed replies and could grow INFO's output allocation. Its
+`owned[kMaxThreads]` array also lived in the common `cmd_info` frame.
+The fix guards the **call** with the effective predicate at
+`src/cmd/t_server.cc:1998`, and moves the entire walk, scratch arrays and
+formatting into `append_read_local_thread_info` in `src/core/rl2s.cc:310`.
+`noinline` keeps those arrays out of the caller, including under LTO.
+The helper receives only references, after the guard; it does not take
+an eagerly computed topology snapshot or allocate before early-returning.
+Disabled INFO emits neither new field family. Enabled INFO keeps both.
+The offset assert at `src/core/thread.h:306` additionally locks the old
+sidecar prefix; it adds no runtime state. The lane implementation is kept.
+
+### 1. Threads, capabilities and roles: unchanged off
+
+The effective predicate is `cfg.overlap == 0 && cfg.read_local != 0`
+(`src/core/server.h:582`). It is false when the knob is zero. The only two
+thread modes are Split and Fused (`src/core/config.h:225`): `main` returns
+through the existing fused path at `src/main.cc:312`, then tests the
+predicate at `:317`. Thus the **new runtime** can be called only for
+Split **and** read-local 1, with overlap 0. Its entry also checks this
+contract before construction (`src/core/rl2s.cc:77`). Mode, overlap and
+read-local remain boot-only CONFIG entries (`src/cmd/t_server.cc:322`).
+
+Off falls through to the old `vector<IoLoop>` and `vector<ExLoop>` at
+`src/main.cc:338`; `ExLoop` still means **`ExLoopT<false>`**
+(`src/core/ex_loop.h:3439`). It does not construct the new
+`vector<FusedExLoop>` at `src/core/rl2s.cc:92`, bind a fused executor,
+call `activate_fused`, or enter `run_fused`.
+
+Placement still builds the ordinary split ratio/explicit placement
+(`src/core/server.h:239`), creates `placement_.total_threads()` contexts
+(`:294`, `:305`), and assigns shard homes to EX tids
+(`src/core/placement.h:178`, `src/core/server.h:367`). Actual OS workers
+are created once per EX tid at `src/main.cc:372` and once per IFID tid at
+`:507`. At gate geometry this is **8 workers: 6 IFID, 2 EX, 16 shards on
+EX**, not an additional reader tier. Main remains the existing monitor.
+
+Baseline already provisioned the opposite dormant loop on every worker
+for FLIP: dormant IO on EX at `src/main.cc:395`, dormant ordinary EX on IO
+at `:528`. Those allocations/capabilities are not new. Off role changes
+still call only `ios[tid].run()` / `exs[tid].run()` (`:426`, `:562`), so
+FLIP cannot later upgrade an off worker to the reader runtime. The entire
+ordinary split construction/role-loop/shutdown suffix starting at
+`src/main.cc:336` was compared byte-for-byte with baseline and is identical.
+
+### 2. Allocations, buffers, sinks and registrations: none added off
+
+The inventory includes existing allocations newly reachable by the armed
+split runtime, not just literal `new` lines in `rl2s.cc`.
+
+| Construction or allocation | Guard and off-arm result |
+| --- | --- |
+| Server read-local sidecar (`new ReadLocalServerState`) | Existing predicate guard at `src/core/server.h:316`, allocation `:317`. False off; the existing owning pointer stays null. |
+| Per-thread sidecars, tick publications, stats and sink slots | Same guard encloses the thread loop at `src/core/server.h:322`; `src/core/thread.h:353` performs `new ReadLocalThreadState`. Never called off, including for future FLIP readers/owners. |
+| Per-shard extended store state and foreign-read safety arrays | Same guard encloses `prepare_read_local` at `src/core/server.h:328`. It reaches `new ReadLocalStoreState` at `src/store/flatstore_atomic.inc:890`. Off stores retain the original ordinary atomic-state allocation path. |
+| `ReadLocalExImpl`, `LaneEntry[kInboxSlots]`, fallback array | `src/core/ex_loop.h:196` first requires compile-time `Fused`, then the effective predicate at `:197`; the three `new` sites are `:198`, `:200`, `:202`. The entire block is discarded for the off `ExLoopT<false>`. |
+| Deferred `Entry[kCapacity]` buffer, block-cache heads and retire sink | Deferred initialization is inside that same block (`src/core/ex_loop.h:204`); buffer allocation is `src/core/read_local.h:260`, sink binding `:262`. Cache heads and retire-ring bookkeeping are members of the optional queue (`:464`), not a baseline per-thread allocation. No off queue exists to grow debug pending sets or retain blocks. |
+| Runtime vectors, default-constructed loop containers and worker closures | `pool`, `ios(nthreads)`, `executors(nthreads)` at `src/core/rl2s.cc:90`, then `pool.emplace_back` at `:122`, are inside the guarded runtime call. This includes executor deque/map allocations during default construction, not merely `init`. The off runtime constructs only the baseline vectors/loops described above. |
+| Boot-gate worker-state vector, loading/error strings and shutdown report buffers | `FusedBootGate boot(nthreads)` at `src/core/rl2s.cc:95` allocates its vector in `src/core/fused_boot_gate.h:23`. The new runtime's local strings (`src/core/rl2s.cc:130`, `:235`, `:263`, `:281`) and report (`:115`) share the same call guard. No static boot gate or report is constructed for off. |
+| Inbox channels, IO/EX rings, network registration and listener resources | All new-runtime calls are below its guard: `src/core/rl2s.cc:127`, `:129`, `:144`, `:196`, `:248`, `:264`, `:270`. Inbox storage comes from `src/core/thread.h:338`; rings from `src/core/ex_loop.h:213` and `src/core/io_loop.h:102`. Off calls the unchanged baseline initializers with the same parameters; it does not additionally run any of these RL2S initializers. |
+| Role/client registration, capacity, completion, demotion and progress hooks | `src/core/rl2s.cc:147`, `:152`, `:160`, `:165`, `:167`, `:172`, `:181` are inside the enabled runtime. Fused sink publication and store arming occur in `src/core/ex_loop.h:241` and `:245`. Off never installs those fused hooks; its pre-existing ordinary IO registration hooks remain. |
+| Connection RYOW sidecar and local-read reply reserves | Parser admission remains behind compile-time `Fused` and the effective predicate (`src/core/io_loop.h:4200`). Its write marking at `:4253` can reach `Rob::prepare_read_local` (`src/net/rob.h:486`, `:709`) only in that armed path. Local-copy reply reservation (`src/core/ex_loop.h:990`) likewise belongs to the existing reader lane. No new accept-time buffer or sidecar allocation was added. |
+| New INFO ownership array, row buffer and output growth | Fixed: caller guard at `src/cmd/t_server.cc:1998` precedes helper entry; `owned` and `row` exist only in `src/core/rl2s.cc:313`, `:317`. Both `body.append` sites (`:336`, `:341`) are inside that helper. Off emits the baseline bytes and takes none of these buffer allocations. |
+
+There is no new `.reserve()`/`.resize()` call in the feature file and no
+file-scope runtime object or thread-local registration. Its project header
+dependencies were already included by main/genthread. All feature-owned
+vectors, strings, arrays and OS-thread creation are local to the two
+guarded entry points. Existing optional sidecars store the hooks; no
+separate always-present registry was introduced.
+
+QSBR registration is the optional per-thread state allocated at boot,
+not mere membership in the baseline `threads_` vector. Its floor scanner
+(`src/core/server.h:606`) is driven by the optional deferred queues.
+Neither the scanner, a tick/park publication, nor a sink binding is added
+to an off thread. Existing store ownership-edge checks remain unchanged
+(`src/core/server.h:1951`) and return for unarmed stores.
+
+### 3. Hot loops and eager arguments: no added off-arm branch
+
+| Changed test or call | Why off does not execute it |
+| --- | --- |
+| Runtime selection and overlap validation | `src/main.cc:317` and `src/core/config.h:1215` are boot-only, before operation processing. The changed startup notice at `src/main.cc:183` is also cold and silent with read-local 0. |
+| IO reader-entry resume/epoch/active publication and exit publication | Both are inside `if constexpr (Fused)` at `src/core/io_loop.h:541` and `:766`. Ordinary `run()` selects `run_split<0/1>` (`:429`); every network/TLS/unix variant passes `Fused=false` (`:407`). The new calls and **all their arguments**, including `read_local_epoch()`, are discarded. Entry/exit are tenure boundaries even on the enabled arm. |
+| EX parked/lane-count check, config invalidation, debug ownership check and retire drain | All additions to `ExLoopT::run` are inside `if constexpr (Fused)` (`src/core/ex_loop.h:708`, `:723`, `:800`). Off instantiates `ExLoopT<false>`; it does not evaluate `read_local_impl()`, `self_->id()` for the new check, or epoch/sink arguments. This also holds with `TOMO_RL_CACHE_DEBUG`. |
+| New mode tests in baseline fused pass/sweep | `src/core/ex_loop.h:397` and `:640` live in `static_assert(Fused)` entry points. Off never calls them. They do add a rotation-boundary test to the existing **1s** schedule; the 1s PRE/POST measurement obligation remains. |
+| Shard-less pass's config, clock, pressure and cap checks | Entirely in the enabled-only `ExLoopT<true>::split_read_local_pass` (`src/core/rl2s.cc:46`). No callback to it is installed off. |
+| `OwnsShards` config-refresh selection | `src/core/ex_loop.h:1913` defaults to **true** on all old calls. `if constexpr (OwnsShards)` adds no runtime test. Only `src/core/rl2s.cc:49` calls `<false>`. Expanding the true block gives the baseline owner-refresh token sequence. |
+| Changed shared `Server::read_local_enabled` predicate | The ordinary parser's `Fused && ...` at `src/core/io_loop.h:3924` short-circuits at compile time. In common IO config refresh, the existing version-change return at `:7135` still precedes the predicate at `:7146`; changing the predicate adds no check to unchanged-version rotations. Other shared uses are boot/config preparation and INFO/RESETSTAT, not the GET/SET dispatch/execution loops. |
+| New INFO caller guard | `src/cmd/t_server.cc:1998` is inside the INFO SERVER command section (`:1975`), not any general operation or worker loop. All new per-shard/per-thread reporting loops are behind it in the cold helper. |
+
+The parser/admission/dispatch function at `src/core/io_loop.h:3918` is
+unchanged. In particular, `if constexpr (Fused)` and the enabled/pressure
+tests at `:3992` still precede `read_local_lane_quota(active_.size())` at
+`:3995`. Off evaluates **neither** the quota call **nor** `active_.size()`;
+there is no new cold-cache-line load disguised as a no-op callee.
+
+Source comparisons discarded the `if constexpr (Fused)` statements in
+both revisions of IO/EX rotation bodies; the remaining token sequences
+match. The initializers, ordinary split dispatch entry points, parser,
+owner task drain and IO config-refresh bodies also match baseline tokens.
+These comparisons check the source specialization, not compiler output.
+
+### 4. Per-operation layouts: unchanged off; optional growth disclosed
+
+| Type | Baseline bytes | Current bytes | Existing layout assert |
+| --- | ---: | ---: | --- |
+| `Op` | 336 | 336 | `src/exec/op.h:463` |
+| `Client` | 1984 | 1984 | `src/net/conn.h:905` |
+| `ThreadCtx` | 1408 | 1408 | `src/core/thread.h:1208` |
+| `Shard` | 1440 | 1440 | `src/core/shard.h:523` |
+| `FlatStore` | 944 | 944 | `src/store/flatstore.h:3912` |
+| `Rob<64>` | 192 | 192 | `src/net/rob.h:1098` |
+| `AtomicEntry` | 144 | 144 | `src/store/atomic_mvcc.h:70` |
+| `Config` | 624 | 624 | `src/core/config.h:429` |
+| Ordinary `ExLoop` | 6112 | 6112 | `src/core/ex_loop.h:3450` |
+
+No data members were added/reordered in `IoLoop`, `Server`, `ThreadCtx`
+or `ExLoopT`: their member declarations match baseline, including the
+existing optional pointer in `ThreadCtx`'s tail (`src/core/thread.h:1205`)
+and the empty `ReadLocalExState<false>` (`src/core/ex_loop.h:103`, `:165`)
+held with `[[no_unique_address]]` (`:3436`). The other locked type files
+are byte-identical; Config differs only in its field's comment. Thus no
+off-arm object has a new allocation stride, field offset or cache line.
+IoLoop and Server have no numeric whole-size assert here; their unchanged
+member/type layouts, rather than an invented size, are the evidence.
+
+**One on-arm sidecar did grow:** production `ReadLocalThreadState`
+**360 -> 368 bytes**. Baseline has an 8-byte tick, a 24-byte sink
+(`src/store/read_local_reclaim.h:61`) and 328-byte stats (38 `uint64_t`
+counters plus three arm counters: `src/core/thread.h:137`,
+`src/net/rob.h:62`). The added `atomic<bool> lane_active` follows them
+(`src/core/thread.h:303`); alignment adds seven padding bytes. The old
+prefix offset is now asserted as 360 at `:306`, total size 368 at `:308`.
+The diagnostics-only selector 3 retains its existing separate alignment
+and is excluded from these production asserts. It too uses only optional
+state. **Off pays zero bytes and no cache line for this growth**, because
+`Server::init` never calls `ThreadCtx::init_read_local_state` off. Existing
+hot fields remain at their old offsets on both arms.
+
+After the INFO fix, all four off-arm source properties above hold. Static
+verification completed: baseline source/token comparisons, Python AST
+parsing of the four changed batteries (without importing/running them),
+and `git diff HEAD --check`. The maintainer still owns build/layout-assert
+execution, both-mode boots, the gate and the PRE/POST rates. No gate row
+was added or retired; quick/full expectations remain **327/344**. The
+quick-tier branch starts at `tests/gate.sh:1258` and exits at `:1260`; the
+standalone RL2S battery has no row on either side of it.
+
 ## MEASUREMENT PLAN
 
 The maintainer performs all steps below, on the quiet box. No performance
@@ -221,7 +390,9 @@
    row automatically.
 2. Boot the four overlap-0 combinations: 2s/0, 2s/1, 1s/0, 1s/1. On eight
    real cores use 6:2 for split; omit ratio for 1s. Require INFO's effective
-   lane bit and active-reader count to match: 0, 6, 0, 8. Also smoke 2s/1
+   lane bit to be 0, 1, 0, 1. Enabled active-reader counts must be 6 and 8;
+   off arms must omit the new active-count/per-thread fields and retain
+   their baseline INFO payload. Also smoke 2s/1
    with one shard and 6:2 (one EX has no shard), explicit placement, and
    the existing TCP/TLS/unix and epoll/uring boot/shutdown cases. Confirm
    split/1/overlap-1 fails with the documented message.
diff --git a/src/cmd/t_server.cc b/src/cmd/t_server.cc
index 2d5c81bfdb1c5ee1dcedddf26af8e1801a2ea4d3..7ad03034a03f1c5a2918a29c761c28fa54bdcb96
--- a/src/cmd/t_server.cc
+++ b/src/cmd/t_server.cc
@@ -14,6 +14,7 @@
 #include "server_tail.h"
 #include "slowlog.h"
 #include "../base/alloc.h"
+#include "../core/genthread.h"
 #include "../core/server.h"
 #include "../core/lbsignals.h"
 #include "../core/pubsub_event.h"
@@ -1992,33 +1993,10 @@
                 static_cast<unsigned>(g_server ? g_server->cfg().port : 0),
                 static_cast<unsigned long long>(uptime),
                 static_cast<unsigned long long>(uptime / 86400));
-        uint32_t active_readers = 0;
-        if (g_server) {
-            uint32_t owned[kMaxThreads] = {};
-            // Read the published ownership directory, never a vector FLIP may be rewriting.
-            for (uint32_t sid = 0; sid < g_server->nshards(); sid++)
-                owned[g_server->worker_of_shard(static_cast<int32_t>(sid))]++;
-            for (uint32_t tid = 0; tid < g_server->nthreads(); tid++) {
-                const ThreadCtx& thread = g_server->thread(tid);
-                const bool active = thread.read_local_lane_active();
-                active_readers += active;
-                const Role role = thread.role();
-                const char* name = role == Role::Idle ? "idle"
-                    : g_server->thread_mode() == ThreadMode::Fused ? "unified"
-                    : role == Role::Ifid ? "ifid" : "ex";
-                const ReadLocalStats* stats = g_server->read_local_enabled()
-                    ? &thread.read_local_stats() : nullptr;
-                // These per-thread totals are lifetime counters; INFO stats retains its existing
-                // RESETSTAT-relative aggregate counters. FLIP does not reset either sidecar.
-                appendf(body,
-                        "read_local_thread_%u:role=%s,shards=%u,active=%u,"
-                        "hits_total=%llu,mget_hits_total=%llu\r\n",
-                        tid, name, owned[tid], active ? 1u : 0u,
-                        static_cast<unsigned long long>(stats ? stats->hits : 0),
-                        static_cast<unsigned long long>(stats ? stats->mget_local_hits : 0));
-            }
-        }
-        appendf(body, "read_local_active_threads:%u\r\n", active_readers);
+        // Guard the CALL, including all argument evaluation. The cold non-inlined helper owns
+        // its scratch buffers so the disabled INFO path keeps its old output without those arrays.
+        if (g_server && g_server->read_local_enabled())
+            append_read_local_thread_info(body, *g_server);
         if (g_server && g_server->thread_mode() == ThreadMode::Fused) {
             appendf(body,
                     "fused_threads:%u\r\nclient_threads:%u\r\nowner_threads:%u\r\n"
diff --git a/src/core/genthread.h b/src/core/genthread.h
index c47b788f303c3a7e7f482d5ddc9f5a580a0a9bbb..e874192a1f6c2412955de097a7cd14587582ee0f
--- a/src/core/genthread.h
+++ b/src/core/genthread.h
@@ -3,6 +3,7 @@
 #pragma once
 
 #include <memory>
+#include <string>
 #include <vector>
 
 namespace tomo {
@@ -27,4 +28,7 @@
                      LateUnixListener& unix_listener,
                      ShutdownReportFinalLine& final_report);
 
+// INFO-only diagnostics. The caller must test read_local_enabled() before this call.
+void append_read_local_thread_info(std::string& body, const Server& server);
+
 }  // namespace tomo
diff --git a/src/core/rl2s.cc b/src/core/rl2s.cc
index d942a2e0f0f948a54a3f9becd0e2e32efb231a98..274b33481ca4d2e1e9e9b06d9d4dc9da575090e8
--- a/src/core/rl2s.cc
+++ b/src/core/rl2s.cc
@@ -305,4 +305,40 @@
     return 0;
 }
 
+// Keep the scratch arrays out of cmd_info's frame, including with LTO. Only an armed INFO SERVER
+// request reaches this helper; the boot/runtime paths install no reporting callback or sidecar.
+__attribute__((noinline, cold))
+void append_read_local_thread_info(std::string& body, const Server& srv) {
+    uint32_t active_readers = 0;
+    uint32_t owned[kMaxThreads] = {};
+    // Read the published ownership directory, never a vector FLIP may be rewriting.
+    for (uint32_t sid = 0; sid < srv.nshards(); sid++)
+        owned[srv.worker_of_shard(static_cast<int32_t>(sid))]++;
+    char row[192];  // fixed grammar, including full-width uint32/uint64 counters
+    for (uint32_t tid = 0; tid < srv.nthreads(); tid++) {
+        const ThreadCtx& thread = srv.thread(tid);
+        const bool active = thread.read_local_lane_active();
+        active_readers += active;
+        const Role role = thread.role();
+        const char* name = role == Role::Idle ? "idle"
+            : srv.thread_mode() == ThreadMode::Fused ? "unified"
+            : role == Role::Ifid ? "ifid" : "ex";
+        const ReadLocalStats& stats = thread.read_local_stats();
+        // Lifetime counters survive FLIP and RESETSTAT; INFO stats retains its existing
+        // RESETSTAT-relative aggregate counters.
+        const int n = std::snprintf(row, sizeof(row),
+            "read_local_thread_%u:role=%s,shards=%u,active=%u,"
+            "hits_total=%llu,mget_hits_total=%llu\r\n",
+            tid, name, owned[tid], active ? 1u : 0u,
+            static_cast<unsigned long long>(stats.hits),
+            static_cast<unsigned long long>(stats.mget_local_hits));
+        if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+        body.append(row, static_cast<size_t>(n));
+    }
+    const int n = std::snprintf(row, sizeof(row), "read_local_active_threads:%u\r\n",
+                              active_readers);
+    if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+    body.append(row, static_cast<size_t>(n));
+}
+
 }  // namespace tomo
diff --git a/src/core/thread.h b/src/core/thread.h
index adea9605d0f0d40c3566e07b90a2663accb6b835..61802252934cbd450279ad7cd2dbaa8d5f03f465
--- a/src/core/thread.h
+++ b/src/core/thread.h
@@ -303,6 +303,8 @@
     std::atomic<bool> lane_active{false};
 };
 #if TOMO_READ_LOCAL_SET_TAX_VARIANT != 3
+static_assert(offsetof(ReadLocalThreadState, lane_active) == 360,
+              "lane activation must follow the entire pre-RL2S sidecar");
 static_assert(sizeof(ReadLocalThreadState) == 368,
               "lane activation adds one alignment word only to the optional sidecar");
 #endif

exec
/bin/bash -lc "sed -n '44,122p' src/store/read_local_settax.h
python3 - <<'PY'
import re
from pathlib import Path
s = Path('src/store/read_local_settax.h').read_text().split('struct ReadLocalSetTaxStats {', 1)[1].split('void ', 1)[0]
fields = re.findall(r'"'^'"\\s*uint64_t\\s+\\w+\\s*=\\s*0;', s, re.M)
print('diagnostic uint64 fields:', len(fields))
print('other declarations:', [line for line in s.splitlines() if ';' in line and 'uint64_t' not in line and not line.strip().startswith('//')])
PY" in /home/user/Projects/cx-rl2s
 succeeded in 0ms:
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
// Temporary owner-local attribution for the armed SET bake-off. These are intentionally plain
// counters: the owning fused thread is their sole writer and INFO already takes an exceptional
// cross-thread snapshot of the surrounding read-local telemetry.
struct ReadLocalSetTaxStats {
    // INFO derives overwrite attempts from the one mutually exclusive outcome increment. That
    // keeps a successful selector-3 overwrite to one diagnostic write instead of attempts+hits.
    uint64_t overwrite_hits = 0;
    uint64_t reject_missing = 0;
    uint64_t reject_encoding = 0;
    uint64_t reject_ttl = 0;
    uint64_t reject_oversize = 0;
    uint64_t reject_size_class = 0;
    uint64_t reject_borrowed = 0;
    uint64_t reject_sequence_saturated = 0;
    uint64_t overwrite_maxmemory_oom = 0;

    uint64_t init_raw_calls = 0;
    uint64_t init_int_calls = 0;
    uint64_t init_extern_calls = 0;
    uint64_t init_key_bytes = 0;
    uint64_t init_value_bytes = 0;
    uint64_t init_cell_prepare_calls = 0;
    uint64_t fresh_allocation_attempts = 0;
    uint64_t accounting_add_calls = 0;
    uint64_t accounting_sub_calls = 0;
    uint64_t accounting_bytes = 0;
    uint64_t slot_replacements = 0;
    uint64_t expire_erases = 0;

    // INFO derives recycler hit rate as acquire_hits / acquire_attempts.
    uint64_t recycle_acquire_attempts = 0;
    uint64_t recycle_acquire_hits = 0;
    uint64_t recycle_acquire_ineligible = 0;
    uint64_t recycle_acquire_empty = 0;
    uint64_t recycle_return_attempts = 0;
    uint64_t recycle_return_accepted = 0;
    uint64_t recycle_return_ineligible = 0;
    uint64_t recycle_return_limited = 0;
    uint64_t recycle_pool_nodes = 0;
    uint64_t recycle_pool_max_owner_nodes = 0;
    uint64_t recycle_capacity_evals = 0;
    uint64_t recycle_candidate_attempts = 0;
    uint64_t recycle_reject_not_string = 0;
    uint64_t recycle_reject_encoding = 0;
    uint64_t recycle_reject_borrowed = 0;
    uint64_t recycle_atomic_pool_accepts = 0;

    uint64_t qsbr_deferrals = 0;
    uint64_t qsbr_object_deferrals = 0;
    uint64_t qsbr_table_deferrals = 0;
    uint64_t qsbr_depth = 0;
    uint64_t qsbr_max_owner_depth = 0;
    // Average enqueue depth is depth_sum / depth_samples; depth itself is the live gauge.
    uint64_t qsbr_depth_samples = 0;
    uint64_t qsbr_depth_sum = 0;
    uint64_t qsbr_seals = 0;
    uint64_t qsbr_sealed_entries = 0;
    uint64_t qsbr_grace_scans = 0;
    uint64_t qsbr_participant_loads = 0;
    uint64_t qsbr_zero_progress_scans = 0;
    uint64_t qsbr_reclaims = 0;
    uint64_t qsbr_forced_graces = 0;
    uint64_t qsbr_forced_yields = 0;

    uint64_t object_sequence_retries = 0;

    void add(const ReadLocalSetTaxStats& other) {
#define TOMO_SETTAX_ADD(field) field += other.field
        TOMO_SETTAX_ADD(overwrite_hits);
        TOMO_SETTAX_ADD(reject_missing);
        TOMO_SETTAX_ADD(reject_encoding);
        TOMO_SETTAX_ADD(reject_ttl);
        TOMO_SETTAX_ADD(reject_oversize);
        TOMO_SETTAX_ADD(reject_size_class);
        TOMO_SETTAX_ADD(reject_borrowed);
        TOMO_SETTAX_ADD(reject_sequence_saturated);
        TOMO_SETTAX_ADD(overwrite_maxmemory_oom);
        TOMO_SETTAX_ADD(init_raw_calls);
diagnostic uint64 fields: 53
other declarations: []

diff --git a/RL2S.md b/RL2S.md
index 64dea226f85e5b4b296ed8c9f1de21ae463a8cf0..3a58bf02bfafd7ae3a505d79242145d586f17032
--- a/RL2S.md
+++ b/RL2S.md
@@ -76,7 +76,8 @@
 guards evaluation of `active_.size()`; no admission expression moved ahead
 of its old check.
 
-All physical threads provision an `IoLoop` and `FusedExLoop`. They use the
+On an enabled `2s/1` boot, all physical threads provision an `IoLoop` and
+`FusedExLoop`. They use the
 ordinary split task inbox layout and producer masks, including FLIP's
 existing remasking. Initial persistence loading runs before store arming.
 Each executor's `activate_fused` binds its permanent ThreadCtx sink, then
@@ -136,7 +137,8 @@
 1984, ThreadCtx 1408, Shard 1440, FlatStore 944, Rob<64> 192, AtomicEntry 144,
 Config 624; the ordinary ExLoop assert remains 6112. The only data layout
 change is the optional `ReadLocalThreadState`: 360 -> 368 bytes in the
-production build, asserted at `src/core/thread.h:306`. An atomic loop-entry
+production build, asserted at `src/core/thread.h:308`; its old 360-byte
+prefix is also locked at `src/core/thread.h:306`. An atomic loop-entry
 flag plus alignment occupies the extra word, after all existing members.
 The diagnostics-only selector-3 sidecar already has different alignment
 and is excluded from this production-size assert. Read-local 0 allocates
@@ -148,7 +150,8 @@
 ## Observable activation
 
 `INFO server` still reports `thread_mode:2s`, live `io_threads` and
-`ex_threads`, and effective `read_local:1`. It now also reports:
+`ex_threads`, and effective `read_local:1`. When the effective lane is
+enabled, it also reports:
 
 ```text
 read_local_thread_0:role=ifid,shards=0,active=1,hits_total=123,mget_hits_total=7
@@ -161,10 +164,15 @@
 network waits; it is not inferred from the knob or ownership. The atomic
 flag is written only at loop entry/exit. Shard counts come from published
 owner IDs, not concurrently modified ownership vectors
-(`src/cmd/t_server.cc:1995`). After FLIP settles, every IFID row must have
+(`src/core/rl2s.cc:313`). After FLIP settles, every IFID row must have
 shards=0/active=1, and every EX row active=0. As with other INFO geometry,
 the individual samples are not one transactional view during a move.
 
+With read-local disabled, both new field families are absent, preserving
+the baseline INFO payload. Their caller guard is at
+`src/cmd/t_server.cc:1998`; their scratch arrays and formatting live in the
+non-inlined helper at `src/core/rl2s.cc:310`.
+
 Per-thread `*_total` values are lifetime completions and survive FLIP and
 CONFIG RESETSTAT. They can be nonzero on an EX row after that thread has
 previously served IO. Aggregate `INFO stats` keeps its existing RESETSTAT
@@ -203,6 +211,167 @@
 row on either side of that exit. Updating existing battery internals does
 not change gate row counts.
 
+## OFF-ARM PROOF
+
+Compared against **`78c3e5391`**, for **`--thread-mode 2s --read-local 0`**,
+including its supported overlap 0 and 1 schedules. References below name
+the final working-tree sources unless explicitly marked baseline. This is
+a source and construction proof; no build, server, benchmark or gate was
+run. It does not establish identical generated instructions or measured
+PRE/POST rates.
+
+**Finding and fix.** The submitted diff did not preserve disabled INFO:
+it counted shard owners, visited every thread and formatted
+`read_local_thread_*` and `read_local_active_threads` on off boots too.
+That changed replies and could grow INFO's output allocation. Its
+`owned[kMaxThreads]` array also lived in the common `cmd_info` frame.
+The fix guards the **call** with the effective predicate at
+`src/cmd/t_server.cc:1998`, and moves the entire walk, scratch arrays and
+formatting into `append_read_local_thread_info` in `src/core/rl2s.cc:310`.
+`noinline` keeps those arrays out of the caller, including under LTO.
+The helper receives only references, after the guard; it does not take
+an eagerly computed topology snapshot or allocate before early-returning.
+Disabled INFO emits neither new field family. Enabled INFO keeps both.
+The offset assert at `src/core/thread.h:306` additionally locks the old
+sidecar prefix; it adds no runtime state. The lane implementation is kept.
+
+### 1. Threads, capabilities and roles: unchanged off
+
+The effective predicate is `cfg.overlap == 0 && cfg.read_local != 0`
+(`src/core/server.h:582`). It is false when the knob is zero. The only two
+thread modes are Split and Fused (`src/core/config.h:225`): `main` returns
+through the existing fused path at `src/main.cc:312`, then tests the
+predicate at `:317`. Thus the **new runtime** can be called only for
+Split **and** read-local 1, with overlap 0. Its entry also checks this
+contract before construction (`src/core/rl2s.cc:77`). Mode, overlap and
+read-local remain boot-only CONFIG entries (`src/cmd/t_server.cc:322`).
+
+Off falls through to the old `vector<IoLoop>` and `vector<ExLoop>` at
+`src/main.cc:338`; `ExLoop` still means **`ExLoopT<false>`**
+(`src/core/ex_loop.h:3439`). It does not construct the new
+`vector<FusedExLoop>` at `src/core/rl2s.cc:92`, bind a fused executor,
+call `activate_fused`, or enter `run_fused`.
+
+Placement still builds the ordinary split ratio/explicit placement
+(`src/core/server.h:239`), creates `placement_.total_threads()` contexts
+(`:294`, `:305`), and assigns shard homes to EX tids
+(`src/core/placement.h:178`, `src/core/server.h:367`). Actual OS workers
+are created once per EX tid at `src/main.cc:372` and once per IFID tid at
+`:507`. At gate geometry this is **8 workers: 6 IFID, 2 EX, 16 shards on
+EX**, not an additional reader tier. Main remains the existing monitor.
+
+Baseline already provisioned the opposite dormant loop on every worker
+for FLIP: dormant IO on EX at `src/main.cc:395`, dormant ordinary EX on IO
+at `:528`. Those allocations/capabilities are not new. Off role changes
+still call only `ios[tid].run()` / `exs[tid].run()` (`:426`, `:562`), so
+FLIP cannot later upgrade an off worker to the reader runtime. The entire
+ordinary split construction/role-loop/shutdown suffix starting at
+`src/main.cc:336` was compared byte-for-byte with baseline and is identical.
+
+### 2. Allocations, buffers, sinks and registrations: none added off
+
+The inventory includes existing allocations newly reachable by the armed
+split runtime, not just literal `new` lines in `rl2s.cc`.
+
+| Construction or allocation | Guard and off-arm result |
+| --- | --- |
+| Server read-local sidecar (`new ReadLocalServerState`) | Existing predicate guard at `src/core/server.h:316`, allocation `:317`. False off; the existing owning pointer stays null. |
+| Per-thread sidecars, tick publications, stats and sink slots | Same guard encloses the thread loop at `src/core/server.h:322`; `src/core/thread.h:353` performs `new ReadLocalThreadState`. Never called off, including for future FLIP readers/owners. |
+| Per-shard extended store state and foreign-read safety arrays | Same guard encloses `prepare_read_local` at `src/core/server.h:328`. It reaches `new ReadLocalStoreState` at `src/store/flatstore_atomic.inc:890`. Off stores retain the original ordinary atomic-state allocation path. |
+| `ReadLocalExImpl`, `LaneEntry[kInboxSlots]`, fallback array | `src/core/ex_loop.h:196` first requires compile-time `Fused`, then the effective predicate at `:197`; the three `new` sites are `:198`, `:200`, `:202`. The entire block is discarded for the off `ExLoopT<false>`. |
+| Deferred `Entry[kCapacity]` buffer, block-cache heads and retire sink | Deferred initialization is inside that same block (`src/core/ex_loop.h:204`); buffer allocation is `src/core/read_local.h:260`, sink binding `:262`. Cache heads and retire-ring bookkeeping are members of the optional queue (`:464`), not a baseline per-thread allocation. No off queue exists to grow debug pending sets or retain blocks. |
+| Runtime vectors, default-constructed loop containers and worker closures | `pool`, `ios(nthreads)`, `executors(nthreads)` at `src/core/rl2s.cc:90`, then `pool.emplace_back` at `:122`, are inside the guarded runtime call. This includes executor deque/map allocations during default construction, not merely `init`. The off runtime constructs only the baseline vectors/loops described above. |
+| Boot-gate worker-state vector, loading/error strings and shutdown report buffers | `FusedBootGate boot(nthreads)` at `src/core/rl2s.cc:95` allocates its vector in `src/core/fused_boot_gate.h:23`. The new runtime's local strings (`src/core/rl2s.cc:130`, `:235`, `:263`, `:281`) and report (`:115`) share the same call guard. No static boot gate or report is constructed for off. |
+| Inbox channels, IO/EX rings, network registration and listener resources | All new-runtime calls are below its guard: `src/core/rl2s.cc:127`, `:129`, `:144`, `:196`, `:248`, `:264`, `:270`. Inbox storage comes from `src/core/thread.h:338`; rings from `src/core/ex_loop.h:213` and `src/core/io_loop.h:102`. Off calls the unchanged baseline initializers with the same parameters; it does not additionally run any of these RL2S initializers. |
+| Role/client registration, capacity, completion, demotion and progress hooks | `src/core/rl2s.cc:147`, `:152`, `:160`, `:165`, `:167`, `:172`, `:181` are inside the enabled runtime. Fused sink publication and store arming occur in `src/core/ex_loop.h:241` and `:245`. Off never installs those fused hooks; its pre-existing ordinary IO registration hooks remain. |
+| Connection RYOW sidecar and local-read reply reserves | Parser admission remains behind compile-time `Fused` and the effective predicate (`src/core/io_loop.h:4200`). Its write marking at `:4253` can reach `Rob::prepare_read_local` (`src/net/rob.h:486`, `:709`) only in that armed path. Local-copy reply reservation (`src/core/ex_loop.h:990`) likewise belongs to the existing reader lane. No new accept-time buffer or sidecar allocation was added. |
+| New INFO ownership array, row buffer and output growth | Fixed: caller guard at `src/cmd/t_server.cc:1998` precedes helper entry; `owned` and `row` exist only in `src/core/rl2s.cc:313`, `:317`. Both `body.append` sites (`:336`, `:341`) are inside that helper. Off emits the baseline bytes and takes none of these buffer allocations. |
+
+There is no new `.reserve()`/`.resize()` call in the feature file and no
+file-scope runtime object or thread-local registration. Its project header
+dependencies were already included by main/genthread. All feature-owned
+vectors, strings, arrays and OS-thread creation are local to the two
+guarded entry points. Existing optional sidecars store the hooks; no
+separate always-present registry was introduced.
+
+QSBR registration is the optional per-thread state allocated at boot,
+not mere membership in the baseline `threads_` vector. Its floor scanner
+(`src/core/server.h:606`) is driven by the optional deferred queues.
+Neither the scanner, a tick/park publication, nor a sink binding is added
+to an off thread. Existing store ownership-edge checks remain unchanged
+(`src/core/server.h:1951`) and return for unarmed stores.
+
+### 3. Hot loops and eager arguments: no added off-arm branch
+
+| Changed test or call | Why off does not execute it |
+| --- | --- |
+| Runtime selection and overlap validation | `src/main.cc:317` and `src/core/config.h:1215` are boot-only, before operation processing. The changed startup notice at `src/main.cc:183` is also cold and silent with read-local 0. |
+| IO reader-entry resume/epoch/active publication and exit publication | Both are inside `if constexpr (Fused)` at `src/core/io_loop.h:541` and `:766`. Ordinary `run()` selects `run_split<0/1>` (`:429`); every network/TLS/unix variant passes `Fused=false` (`:407`). The new calls and **all their arguments**, including `read_local_epoch()`, are discarded. Entry/exit are tenure boundaries even on the enabled arm. |
+| EX parked/lane-count check, config invalidation, debug ownership check and retire drain | All additions to `ExLoopT::run` are inside `if constexpr (Fused)` (`src/core/ex_loop.h:708`, `:723`, `:800`). Off instantiates `ExLoopT<false>`; it does not evaluate `read_local_impl()`, `self_->id()` for the new check, or epoch/sink arguments. This also holds with `TOMO_RL_CACHE_DEBUG`. |
+| New mode tests in baseline fused pass/sweep | `src/core/ex_loop.h:397` and `:640` live in `static_assert(Fused)` entry points. Off never calls them. They do add a rotation-boundary test to the existing **1s** schedule; the 1s PRE/POST measurement obligation remains. |
+| Shard-less pass's config, clock, pressure and cap checks | Entirely in the enabled-only `ExLoopT<true>::split_read_local_pass` (`src/core/rl2s.cc:46`). No callback to it is installed off. |
+| `OwnsShards` config-refresh selection | `src/core/ex_loop.h:1913` defaults to **true** on all old calls. `if constexpr (OwnsShards)` adds no runtime test. Only `src/core/rl2s.cc:49` calls `<false>`. Expanding the true block gives the baseline owner-refresh token sequence. |
+| Changed shared `Server::read_local_enabled` predicate | The ordinary parser's `Fused && ...` at `src/core/io_loop.h:3924` short-circuits at compile time. In common IO config refresh, the existing version-change return at `:7135` still precedes the predicate at `:7146`; changing the predicate adds no check to unchanged-version rotations. Other shared uses are boot/config preparation and INFO/RESETSTAT, not the GET/SET dispatch/execution loops. |
+| New INFO caller guard | `src/cmd/t_server.cc:1998` is inside the INFO SERVER command section (`:1975`), not any general operation or worker loop. All new per-shard/per-thread reporting loops are behind it in the cold helper. |
+
+The parser/admission/dispatch function at `src/core/io_loop.h:3918` is
+unchanged. In particular, `if constexpr (Fused)` and the enabled/pressure
+tests at `:3992` still precede `read_local_lane_quota(active_.size())` at
+`:3995`. Off evaluates **neither** the quota call **nor** `active_.size()`;
+there is no new cold-cache-line load disguised as a no-op callee.
+
+Source comparisons discarded the `if constexpr (Fused)` statements in
+both revisions of IO/EX rotation bodies; the remaining token sequences
+match. The initializers, ordinary split dispatch entry points, parser,
+owner task drain and IO config-refresh bodies also match baseline tokens.
+These comparisons check the source specialization, not compiler output.
+
+### 4. Per-operation layouts: unchanged off; optional growth disclosed
+
+| Type | Baseline bytes | Current bytes | Existing layout assert |
+| --- | ---: | ---: | --- |
+| `Op` | 336 | 336 | `src/exec/op.h:463` |
+| `Client` | 1984 | 1984 | `src/net/conn.h:905` |
+| `ThreadCtx` | 1408 | 1408 | `src/core/thread.h:1208` |
+| `Shard` | 1440 | 1440 | `src/core/shard.h:523` |
+| `FlatStore` | 944 | 944 | `src/store/flatstore.h:3912` |
+| `Rob<64>` | 192 | 192 | `src/net/rob.h:1098` |
+| `AtomicEntry` | 144 | 144 | `src/store/atomic_mvcc.h:70` |
+| `Config` | 624 | 624 | `src/core/config.h:429` |
+| Ordinary `ExLoop` | 6112 | 6112 | `src/core/ex_loop.h:3450` |
+
+No data members were added/reordered in `IoLoop`, `Server`, `ThreadCtx`
+or `ExLoopT`: their member declarations match baseline, including the
+existing optional pointer in `ThreadCtx`'s tail (`src/core/thread.h:1205`)
+and the empty `ReadLocalExState<false>` (`src/core/ex_loop.h:103`, `:165`)
+held with `[[no_unique_address]]` (`:3436`). The other locked type files
+are byte-identical; Config differs only in its field's comment. Thus no
+off-arm object has a new allocation stride, field offset or cache line.
+IoLoop and Server have no numeric whole-size assert here; their unchanged
+member/type layouts, rather than an invented size, are the evidence.
+
+**One on-arm sidecar did grow:** production `ReadLocalThreadState`
+**360 -> 368 bytes**. Baseline has an 8-byte tick, a 24-byte sink
+(`src/store/read_local_reclaim.h:61`) and 328-byte stats (38 `uint64_t`
+counters plus three arm counters: `src/core/thread.h:137`,
+`src/net/rob.h:62`). The added `atomic<bool> lane_active` follows them
+(`src/core/thread.h:303`); alignment adds seven padding bytes. The old
+prefix offset is now asserted as 360 at `:306`, total size 368 at `:308`.
+The diagnostics-only selector 3 retains its existing separate alignment
+and is excluded from these production asserts. It too uses only optional
+state. **Off pays zero bytes and no cache line for this growth**, because
+`Server::init` never calls `ThreadCtx::init_read_local_state` off. Existing
+hot fields remain at their old offsets on both arms.
+
+After the INFO fix, all four off-arm source properties above hold. Static
+verification completed: baseline source/token comparisons, Python AST
+parsing of the four changed batteries (without importing/running them),
+and `git diff HEAD --check`. The maintainer still owns build/layout-assert
+execution, both-mode boots, the gate and the PRE/POST rates. No gate row
+was added or retired; quick/full expectations remain **327/344**. The
+quick-tier branch starts at `tests/gate.sh:1258` and exits at `:1260`; the
+standalone RL2S battery has no row on either side of it.
+
 ## MEASUREMENT PLAN
 
 The maintainer performs all steps below, on the quiet box. No performance
@@ -221,7 +390,9 @@
    row automatically.
 2. Boot the four overlap-0 combinations: 2s/0, 2s/1, 1s/0, 1s/1. On eight
    real cores use 6:2 for split; omit ratio for 1s. Require INFO's effective
-   lane bit and active-reader count to match: 0, 6, 0, 8. Also smoke 2s/1
+   lane bit to be 0, 1, 0, 1. Enabled active-reader counts must be 6 and 8;
+   off arms must omit the new active-count/per-thread fields and retain
+   their baseline INFO payload. Also smoke 2s/1
    with one shard and 6:2 (one EX has no shard), explicit placement, and
    the existing TCP/TLS/unix and epoll/uring boot/shutdown cases. Confirm
    split/1/overlap-1 fails with the documented message.
diff --git a/src/cmd/t_server.cc b/src/cmd/t_server.cc
index 2d5c81bfdb1c5ee1dcedddf26af8e1801a2ea4d3..7ad03034a03f1c5a2918a29c761c28fa54bdcb96
--- a/src/cmd/t_server.cc
+++ b/src/cmd/t_server.cc
@@ -14,6 +14,7 @@
 #include "server_tail.h"
 #include "slowlog.h"
 #include "../base/alloc.h"
+#include "../core/genthread.h"
 #include "../core/server.h"
 #include "../core/lbsignals.h"
 #include "../core/pubsub_event.h"
@@ -1992,33 +1993,10 @@
                 static_cast<unsigned>(g_server ? g_server->cfg().port : 0),
                 static_cast<unsigned long long>(uptime),
                 static_cast<unsigned long long>(uptime / 86400));
-        uint32_t active_readers = 0;
-        if (g_server) {
-            uint32_t owned[kMaxThreads] = {};
-            // Read the published ownership directory, never a vector FLIP may be rewriting.
-            for (uint32_t sid = 0; sid < g_server->nshards(); sid++)
-                owned[g_server->worker_of_shard(static_cast<int32_t>(sid))]++;
-            for (uint32_t tid = 0; tid < g_server->nthreads(); tid++) {
-                const ThreadCtx& thread = g_server->thread(tid);
-                const bool active = thread.read_local_lane_active();
-                active_readers += active;
-                const Role role = thread.role();
-                const char* name = role == Role::Idle ? "idle"
-                    : g_server->thread_mode() == ThreadMode::Fused ? "unified"
-                    : role == Role::Ifid ? "ifid" : "ex";
-                const ReadLocalStats* stats = g_server->read_local_enabled()
-                    ? &thread.read_local_stats() : nullptr;
-                // These per-thread totals are lifetime counters; INFO stats retains its existing
-                // RESETSTAT-relative aggregate counters. FLIP does not reset either sidecar.
-                appendf(body,
-                        "read_local_thread_%u:role=%s,shards=%u,active=%u,"
-                        "hits_total=%llu,mget_hits_total=%llu\r\n",
-                        tid, name, owned[tid], active ? 1u : 0u,
-                        static_cast<unsigned long long>(stats ? stats->hits : 0),
-                        static_cast<unsigned long long>(stats ? stats->mget_local_hits : 0));
-            }
-        }
-        appendf(body, "read_local_active_threads:%u\r\n", active_readers);
+        // Guard the CALL, including all argument evaluation. The cold non-inlined helper owns
+        // its scratch buffers so the disabled INFO path keeps its old output without those arrays.
+        if (g_server && g_server->read_local_enabled())
+            append_read_local_thread_info(body, *g_server);
         if (g_server && g_server->thread_mode() == ThreadMode::Fused) {
             appendf(body,
                     "fused_threads:%u\r\nclient_threads:%u\r\nowner_threads:%u\r\n"
diff --git a/src/core/genthread.h b/src/core/genthread.h
index c47b788f303c3a7e7f482d5ddc9f5a580a0a9bbb..e874192a1f6c2412955de097a7cd14587582ee0f
--- a/src/core/genthread.h
+++ b/src/core/genthread.h
@@ -3,6 +3,7 @@
 #pragma once
 
 #include <memory>
+#include <string>
 #include <vector>
 
 namespace tomo {
@@ -27,4 +28,7 @@
                      LateUnixListener& unix_listener,
                      ShutdownReportFinalLine& final_report);
 
+// INFO-only diagnostics. The caller must test read_local_enabled() before this call.
+void append_read_local_thread_info(std::string& body, const Server& server);
+
 }  // namespace tomo
diff --git a/src/core/rl2s.cc b/src/core/rl2s.cc
index d942a2e0f0f948a54a3f9becd0e2e32efb231a98..274b33481ca4d2e1e9e9b06d9d4dc9da575090e8
--- a/src/core/rl2s.cc
+++ b/src/core/rl2s.cc
@@ -305,4 +305,40 @@
     return 0;
 }
 
+// Keep the scratch arrays out of cmd_info's frame, including with LTO. Only an armed INFO SERVER
+// request reaches this helper; the boot/runtime paths install no reporting callback or sidecar.
+__attribute__((noinline, cold))
+void append_read_local_thread_info(std::string& body, const Server& srv) {
+    uint32_t active_readers = 0;
+    uint32_t owned[kMaxThreads] = {};
+    // Read the published ownership directory, never a vector FLIP may be rewriting.
+    for (uint32_t sid = 0; sid < srv.nshards(); sid++)
+        owned[srv.worker_of_shard(static_cast<int32_t>(sid))]++;
+    char row[192];  // fixed grammar, including full-width uint32/uint64 counters
+    for (uint32_t tid = 0; tid < srv.nthreads(); tid++) {
+        const ThreadCtx& thread = srv.thread(tid);
+        const bool active = thread.read_local_lane_active();
+        active_readers += active;
+        const Role role = thread.role();
+        const char* name = role == Role::Idle ? "idle"
+            : srv.thread_mode() == ThreadMode::Fused ? "unified"
+            : role == Role::Ifid ? "ifid" : "ex";
+        const ReadLocalStats& stats = thread.read_local_stats();
+        // Lifetime counters survive FLIP and RESETSTAT; INFO stats retains its existing
+        // RESETSTAT-relative aggregate counters.
+        const int n = std::snprintf(row, sizeof(row),
+            "read_local_thread_%u:role=%s,shards=%u,active=%u,"
+            "hits_total=%llu,mget_hits_total=%llu\r\n",
+            tid, name, owned[tid], active ? 1u : 0u,
+            static_cast<unsigned long long>(stats.hits),
+            static_cast<unsigned long long>(stats.mget_local_hits));
+        if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+        body.append(row, static_cast<size_t>(n));
+    }
+    const int n = std::snprintf(row, sizeof(row), "read_local_active_threads:%u\r\n",
+                              active_readers);
+    if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+    body.append(row, static_cast<size_t>(n));
+}
+
 }  // namespace tomo
diff --git a/src/core/thread.h b/src/core/thread.h
index adea9605d0f0d40c3566e07b90a2663accb6b835..61802252934cbd450279ad7cd2dbaa8d5f03f465
--- a/src/core/thread.h
+++ b/src/core/thread.h
@@ -303,6 +303,8 @@
     std::atomic<bool> lane_active{false};
 };
 #if TOMO_READ_LOCAL_SET_TAX_VARIANT != 3
+static_assert(offsetof(ReadLocalThreadState, lane_active) == 360,
+              "lane activation must follow the entire pre-RL2S sidecar");
 static_assert(sizeof(ReadLocalThreadState) == 368,
               "lane activation adds one alignment word only to the optional sidecar");
 #endif

exec
/bin/bash -lc "rg -n 'parse_and_dispatch<' src/core/io_loop.h
rg -n 'fused_baseline_pass\\(|fused_baseline_sweep\\(|run_fused\\(' src --glob '"'!genthread.cc'"' --glob '"'!rl2s.cc'"'
sed -n '399,407p' src/core/io_loop.h
sed -n '4149,4177p' src/core/io_loop.h" in /home/user/Projects/cx-rl2s
 succeeded in 0ms:
3314:                    parse_and_dispatch<true, Fused ? kGenthreadIfidBatchOps : 0>(c);
3316:                    parse_and_dispatch<false, Fused ? kGenthreadIfidBatchOps : 0>(c);
3318:                parse_and_dispatch<false, Fused ? kGenthreadIfidBatchOps : 0>(c);
3439:                parse_and_dispatch<true, Fused ? kGenthreadIfidBatchOps : 0>(c);
5728:                            dispatch_result = parse_and_dispatch<
5733:                            dispatch_result = parse_and_dispatch<
5738:                        dispatch_result = parse_and_dispatch<
5746:                            dispatch_result = parse_and_dispatch<
5751:                            dispatch_result = parse_and_dispatch<
5756:                        dispatch_result = parse_and_dispatch<
6426:                            dispatch_result = parse_and_dispatch<true, 0, true>(c);
6428:                            dispatch_result = parse_and_dispatch<false, 0, true>(c);
6430:                        dispatch_result = parse_and_dispatch<false, 0, true>(c);
6436:                            dispatch_result = parse_and_dispatch<true, 0, true>(c);
6438:                            dispatch_result = parse_and_dispatch<false, 0, true>(c);
6440:                        dispatch_result = parse_and_dispatch<false, 0, true>(c);
6773:                            dispatch_result = parse_and_dispatch<
6776:                            dispatch_result = parse_and_dispatch<
6779:                        dispatch_result = parse_and_dispatch<
6786:                            dispatch_result = parse_and_dispatch<
6789:                            dispatch_result = parse_and_dispatch<
6792:                        dispatch_result = parse_and_dispatch<
src/core/ex_loop.h:395:    uint32_t fused_baseline_pass() {
src/core/ex_loop.h:638:    uint32_t fused_baseline_sweep() {
src/core/io_loop.h:445:    void run_fused();
src/core/io_loop.h:6854:            work += SweepPass ? fused_executor_->fused_baseline_sweep()
src/core/io_loop.h:6855:                              : fused_executor_->fused_baseline_pass();
    // third kind of runtime state. Everything below is `if constexpr` on kEp, so the uring build
    // contains no epoll code and no test for it, and the epoll build contains no ring code and no
    // test for it. This is the same compile-or-boot-time-variant pattern the registry uses for
    // keyspace notifications (handler_notify in cmd/command.h: two instantiations of the handler,
    // one pointer chosen off a per-batch flag, zero per-operation branching) applied at the loop
    // level instead of the handler level -- the engine is a property of the whole loop, so the
    // choice belongs at its outermost frame.
    template <uint8_t Pipeline>
    void run_split() {
            if ((spec->flags & CmdFlags::SubcmdRoute) &&
                command_reply_container_subcommand_arity(*op, *spec)) {
                // A known child has its own generated arity and Redis checks that before ACL and
                // MULTI. Unknown children deliberately continue: on an unauthenticated connection
                // XGROUP x is parent-arity-valid and must reach NOAUTH.
                conn.advance_parse(consumed);
                finish_prebuilt(c, *op);
                continue;
            }
            if constexpr (Fused) {
                // A consecutive GET can reuse the first member's stable connection gates. Any
                // other frame ends the run but keeps parsing; later writes demote only conflicting
                // unresolved reads instead of turning the run into a connection-wide hold.
                if (read_local_enabled && read_local_batch) {
                    extend_read_local_batch =
                        (spec->flags & CmdFlags::ReadLocalEligible) &&
                        !command_is_read_local_mget(*spec) &&
                        conn.multi_session() == nullptr &&
                        rob.pending_read_local_count() < read_local_quota &&
                        fused_executor_->local_read_lane_has_room();
                    if (!extend_read_local_batch) read_local_batch = false;
                }
            }
            // THE SOLE DISABLED-STATE FEATURE DECISION on an ordinary operation. The executor
            // receives a spec whose handler pointer is already the clean or armed specialization;
            // no notification mask load reaches its execute path.
            //
            // Lane F rides this ONE branch rather than adding its own. notify_armed_ is the union
            // of "keyspace notifications configured" and "some CLIENT/MONITOR/TRACKING feature is

diff --git a/RL2S.md b/RL2S.md
index 64dea226f85e5b4b296ed8c9f1de21ae463a8cf0..3a58bf02bfafd7ae3a505d79242145d586f17032
--- a/RL2S.md
+++ b/RL2S.md
@@ -76,7 +76,8 @@
 guards evaluation of `active_.size()`; no admission expression moved ahead
 of its old check.
 
-All physical threads provision an `IoLoop` and `FusedExLoop`. They use the
+On an enabled `2s/1` boot, all physical threads provision an `IoLoop` and
+`FusedExLoop`. They use the
 ordinary split task inbox layout and producer masks, including FLIP's
 existing remasking. Initial persistence loading runs before store arming.
 Each executor's `activate_fused` binds its permanent ThreadCtx sink, then
@@ -136,7 +137,8 @@
 1984, ThreadCtx 1408, Shard 1440, FlatStore 944, Rob<64> 192, AtomicEntry 144,
 Config 624; the ordinary ExLoop assert remains 6112. The only data layout
 change is the optional `ReadLocalThreadState`: 360 -> 368 bytes in the
-production build, asserted at `src/core/thread.h:306`. An atomic loop-entry
+production build, asserted at `src/core/thread.h:308`; its old 360-byte
+prefix is also locked at `src/core/thread.h:306`. An atomic loop-entry
 flag plus alignment occupies the extra word, after all existing members.
 The diagnostics-only selector-3 sidecar already has different alignment
 and is excluded from this production-size assert. Read-local 0 allocates
@@ -148,7 +150,8 @@
 ## Observable activation
 
 `INFO server` still reports `thread_mode:2s`, live `io_threads` and
-`ex_threads`, and effective `read_local:1`. It now also reports:
+`ex_threads`, and effective `read_local:1`. When the effective lane is
+enabled, it also reports:
 
 ```text
 read_local_thread_0:role=ifid,shards=0,active=1,hits_total=123,mget_hits_total=7
@@ -161,10 +164,15 @@
 network waits; it is not inferred from the knob or ownership. The atomic
 flag is written only at loop entry/exit. Shard counts come from published
 owner IDs, not concurrently modified ownership vectors
-(`src/cmd/t_server.cc:1995`). After FLIP settles, every IFID row must have
+(`src/core/rl2s.cc:313`). After FLIP settles, every IFID row must have
 shards=0/active=1, and every EX row active=0. As with other INFO geometry,
 the individual samples are not one transactional view during a move.
 
+With read-local disabled, both new field families are absent, preserving
+the baseline INFO payload. Their caller guard is at
+`src/cmd/t_server.cc:1998`; their scratch arrays and formatting live in the
+non-inlined helper at `src/core/rl2s.cc:310`.
+
 Per-thread `*_total` values are lifetime completions and survive FLIP and
 CONFIG RESETSTAT. They can be nonzero on an EX row after that thread has
 previously served IO. Aggregate `INFO stats` keeps its existing RESETSTAT
@@ -203,6 +211,167 @@
 row on either side of that exit. Updating existing battery internals does
 not change gate row counts.
 
+## OFF-ARM PROOF
+
+Compared against **`78c3e5391`**, for **`--thread-mode 2s --read-local 0`**,
+including its supported overlap 0 and 1 schedules. References below name
+the final working-tree sources unless explicitly marked baseline. This is
+a source and construction proof; no build, server, benchmark or gate was
+run. It does not establish identical generated instructions or measured
+PRE/POST rates.
+
+**Finding and fix.** The submitted diff did not preserve disabled INFO:
+it counted shard owners, visited every thread and formatted
+`read_local_thread_*` and `read_local_active_threads` on off boots too.
+That changed replies and could grow INFO's output allocation. Its
+`owned[kMaxThreads]` array also lived in the common `cmd_info` frame.
+The fix guards the **call** with the effective predicate at
+`src/cmd/t_server.cc:1998`, and moves the entire walk, scratch arrays and
+formatting into `append_read_local_thread_info` in `src/core/rl2s.cc:310`.
+`noinline` keeps those arrays out of the caller, including under LTO.
+The helper receives only references, after the guard; it does not take
+an eagerly computed topology snapshot or allocate before early-returning.
+Disabled INFO emits neither new field family. Enabled INFO keeps both.
+The offset assert at `src/core/thread.h:306` additionally locks the old
+sidecar prefix; it adds no runtime state. The lane implementation is kept.
+
+### 1. Threads, capabilities and roles: unchanged off
+
+The effective predicate is `cfg.overlap == 0 && cfg.read_local != 0`
+(`src/core/server.h:582`). It is false when the knob is zero. The only two
+thread modes are Split and Fused (`src/core/config.h:225`): `main` returns
+through the existing fused path at `src/main.cc:312`, then tests the
+predicate at `:317`. Thus the **new runtime** can be called only for
+Split **and** read-local 1, with overlap 0. Its entry also checks this
+contract before construction (`src/core/rl2s.cc:77`). Mode, overlap and
+read-local remain boot-only CONFIG entries (`src/cmd/t_server.cc:322`).
+
+Off falls through to the old `vector<IoLoop>` and `vector<ExLoop>` at
+`src/main.cc:338`; `ExLoop` still means **`ExLoopT<false>`**
+(`src/core/ex_loop.h:3439`). It does not construct the new
+`vector<FusedExLoop>` at `src/core/rl2s.cc:92`, bind a fused executor,
+call `activate_fused`, or enter `run_fused`.
+
+Placement still builds the ordinary split ratio/explicit placement
+(`src/core/server.h:239`), creates `placement_.total_threads()` contexts
+(`:294`, `:305`), and assigns shard homes to EX tids
+(`src/core/placement.h:178`, `src/core/server.h:367`). Actual OS workers
+are created once per EX tid at `src/main.cc:372` and once per IFID tid at
+`:507`. At gate geometry this is **8 workers: 6 IFID, 2 EX, 16 shards on
+EX**, not an additional reader tier. Main remains the existing monitor.
+
+Baseline already provisioned the opposite dormant loop on every worker
+for FLIP: dormant IO on EX at `src/main.cc:395`, dormant ordinary EX on IO
+at `:528`. Those allocations/capabilities are not new. Off role changes
+still call only `ios[tid].run()` / `exs[tid].run()` (`:426`, `:562`), so
+FLIP cannot later upgrade an off worker to the reader runtime. The entire
+ordinary split construction/role-loop/shutdown suffix starting at
+`src/main.cc:336` was compared byte-for-byte with baseline and is identical.
+
+### 2. Allocations, buffers, sinks and registrations: none added off
+
+The inventory includes existing allocations newly reachable by the armed
+split runtime, not just literal `new` lines in `rl2s.cc`.
+
+| Construction or allocation | Guard and off-arm result |
+| --- | --- |
+| Server read-local sidecar (`new ReadLocalServerState`) | Existing predicate guard at `src/core/server.h:316`, allocation `:317`. False off; the existing owning pointer stays null. |
+| Per-thread sidecars, tick publications, stats and sink slots | Same guard encloses the thread loop at `src/core/server.h:322`; `src/core/thread.h:353` performs `new ReadLocalThreadState`. Never called off, including for future FLIP readers/owners. |
+| Per-shard extended store state and foreign-read safety arrays | Same guard encloses `prepare_read_local` at `src/core/server.h:328`. It reaches `new ReadLocalStoreState` at `src/store/flatstore_atomic.inc:890`. Off stores retain the original ordinary atomic-state allocation path. |
+| `ReadLocalExImpl`, `LaneEntry[kInboxSlots]`, fallback array | `src/core/ex_loop.h:196` first requires compile-time `Fused`, then the effective predicate at `:197`; the three `new` sites are `:198`, `:200`, `:202`. The entire block is discarded for the off `ExLoopT<false>`. |
+| Deferred `Entry[kCapacity]` buffer, block-cache heads and retire sink | Deferred initialization is inside that same block (`src/core/ex_loop.h:204`); buffer allocation is `src/core/read_local.h:260`, sink binding `:262`. Cache heads and retire-ring bookkeeping are members of the optional queue (`:464`), not a baseline per-thread allocation. No off queue exists to grow debug pending sets or retain blocks. |
+| Runtime vectors, default-constructed loop containers and worker closures | `pool`, `ios(nthreads)`, `executors(nthreads)` at `src/core/rl2s.cc:90`, then `pool.emplace_back` at `:122`, are inside the guarded runtime call. This includes executor deque/map allocations during default construction, not merely `init`. The off runtime constructs only the baseline vectors/loops described above. |
+| Boot-gate worker-state vector, loading/error strings and shutdown report buffers | `FusedBootGate boot(nthreads)` at `src/core/rl2s.cc:95` allocates its vector in `src/core/fused_boot_gate.h:23`. The new runtime's local strings (`src/core/rl2s.cc:130`, `:235`, `:263`, `:281`) and report (`:115`) share the same call guard. No static boot gate or report is constructed for off. |
+| Inbox channels, IO/EX rings, network registration and listener resources | All new-runtime calls are below its guard: `src/core/rl2s.cc:127`, `:129`, `:144`, `:196`, `:248`, `:264`, `:270`. Inbox storage comes from `src/core/thread.h:338`; rings from `src/core/ex_loop.h:213` and `src/core/io_loop.h:102`. Off calls the unchanged baseline initializers with the same parameters; it does not additionally run any of these RL2S initializers. |
+| Role/client registration, capacity, completion, demotion and progress hooks | `src/core/rl2s.cc:147`, `:152`, `:160`, `:165`, `:167`, `:172`, `:181` are inside the enabled runtime. Fused sink publication and store arming occur in `src/core/ex_loop.h:241` and `:245`. Off never installs those fused hooks; its pre-existing ordinary IO registration hooks remain. |
+| Connection RYOW sidecar and local-read reply reserves | Parser admission remains behind compile-time `Fused` and the effective predicate (`src/core/io_loop.h:4200`). Its write marking at `:4253` can reach `Rob::prepare_read_local` (`src/net/rob.h:486`, `:709`) only in that armed path. Local-copy reply reservation (`src/core/ex_loop.h:990`) likewise belongs to the existing reader lane. No new accept-time buffer or sidecar allocation was added. |
+| New INFO ownership array, row buffer and output growth | Fixed: caller guard at `src/cmd/t_server.cc:1998` precedes helper entry; `owned` and `row` exist only in `src/core/rl2s.cc:313`, `:317`. Both `body.append` sites (`:336`, `:341`) are inside that helper. Off emits the baseline bytes and takes none of these buffer allocations. |
+
+There is no new `.reserve()`/`.resize()` call in the feature file and no
+file-scope runtime object or thread-local registration. Its project header
+dependencies were already included by main/genthread. All feature-owned
+vectors, strings, arrays and OS-thread creation are local to the two
+guarded entry points. Existing optional sidecars store the hooks; no
+separate always-present registry was introduced.
+
+QSBR registration is the optional per-thread state allocated at boot,
+not mere membership in the baseline `threads_` vector. Its floor scanner
+(`src/core/server.h:606`) is driven by the optional deferred queues.
+Neither the scanner, a tick/park publication, nor a sink binding is added
+to an off thread. Existing store ownership-edge checks remain unchanged
+(`src/core/server.h:1951`) and return for unarmed stores.
+
+### 3. Hot loops and eager arguments: no added off-arm branch
+
+| Changed test or call | Why off does not execute it |
+| --- | --- |
+| Runtime selection and overlap validation | `src/main.cc:317` and `src/core/config.h:1215` are boot-only, before operation processing. The changed startup notice at `src/main.cc:183` is also cold and silent with read-local 0. |
+| IO reader-entry resume/epoch/active publication and exit publication | Both are inside `if constexpr (Fused)` at `src/core/io_loop.h:541` and `:766`. Ordinary `run()` selects `run_split<0/1>` (`:429`); every network/TLS/unix variant passes `Fused=false` (`:407`). The new calls and **all their arguments**, including `read_local_epoch()`, are discarded. Entry/exit are tenure boundaries even on the enabled arm. |
+| EX parked/lane-count check, config invalidation, debug ownership check and retire drain | All additions to `ExLoopT::run` are inside `if constexpr (Fused)` (`src/core/ex_loop.h:708`, `:723`, `:800`). Off instantiates `ExLoopT<false>`; it does not evaluate `read_local_impl()`, `self_->id()` for the new check, or epoch/sink arguments. This also holds with `TOMO_RL_CACHE_DEBUG`. |
+| New mode tests in baseline fused pass/sweep | `src/core/ex_loop.h:397` and `:640` live in `static_assert(Fused)` entry points. Off never calls them. They do add a rotation-boundary test to the existing **1s** schedule; the 1s PRE/POST measurement obligation remains. |
+| Shard-less pass's config, clock, pressure and cap checks | Entirely in the enabled-only `ExLoopT<true>::split_read_local_pass` (`src/core/rl2s.cc:46`). No callback to it is installed off. |
+| `OwnsShards` config-refresh selection | `src/core/ex_loop.h:1913` defaults to **true** on all old calls. `if constexpr (OwnsShards)` adds no runtime test. Only `src/core/rl2s.cc:49` calls `<false>`. Expanding the true block gives the baseline owner-refresh token sequence. |
+| Changed shared `Server::read_local_enabled` predicate | The ordinary parser's `Fused && ...` at `src/core/io_loop.h:3924` short-circuits at compile time. In common IO config refresh, the existing version-change return at `:7135` still precedes the predicate at `:7146`; changing the predicate adds no check to unchanged-version rotations. Other shared uses are boot/config preparation and INFO/RESETSTAT, not the GET/SET dispatch/execution loops. |
+| New INFO caller guard | `src/cmd/t_server.cc:1998` is inside the INFO SERVER command section (`:1975`), not any general operation or worker loop. All new per-shard/per-thread reporting loops are behind it in the cold helper. |
+
+The parser/admission/dispatch function at `src/core/io_loop.h:3918` is
+unchanged. In particular, `if constexpr (Fused)` and the enabled/pressure
+tests at `:3992` still precede `read_local_lane_quota(active_.size())` at
+`:3995`. Off evaluates **neither** the quota call **nor** `active_.size()`;
+there is no new cold-cache-line load disguised as a no-op callee.
+
+Source comparisons discarded the `if constexpr (Fused)` statements in
+both revisions of IO/EX rotation bodies; the remaining token sequences
+match. The initializers, ordinary split dispatch entry points, parser,
+owner task drain and IO config-refresh bodies also match baseline tokens.
+These comparisons check the source specialization, not compiler output.
+
+### 4. Per-operation layouts: unchanged off; optional growth disclosed
+
+| Type | Baseline bytes | Current bytes | Existing layout assert |
+| --- | ---: | ---: | --- |
+| `Op` | 336 | 336 | `src/exec/op.h:463` |
+| `Client` | 1984 | 1984 | `src/net/conn.h:905` |
+| `ThreadCtx` | 1408 | 1408 | `src/core/thread.h:1208` |
+| `Shard` | 1440 | 1440 | `src/core/shard.h:523` |
+| `FlatStore` | 944 | 944 | `src/store/flatstore.h:3912` |
+| `Rob<64>` | 192 | 192 | `src/net/rob.h:1098` |
+| `AtomicEntry` | 144 | 144 | `src/store/atomic_mvcc.h:70` |
+| `Config` | 624 | 624 | `src/core/config.h:429` |
+| Ordinary `ExLoop` | 6112 | 6112 | `src/core/ex_loop.h:3450` |
+
+No data members were added/reordered in `IoLoop`, `Server`, `ThreadCtx`
+or `ExLoopT`: their member declarations match baseline, including the
+existing optional pointer in `ThreadCtx`'s tail (`src/core/thread.h:1205`)
+and the empty `ReadLocalExState<false>` (`src/core/ex_loop.h:103`, `:165`)
+held with `[[no_unique_address]]` (`:3436`). The other locked type files
+are byte-identical; Config differs only in its field's comment. Thus no
+off-arm object has a new allocation stride, field offset or cache line.
+IoLoop and Server have no numeric whole-size assert here; their unchanged
+member/type layouts, rather than an invented size, are the evidence.
+
+**One on-arm sidecar did grow:** production `ReadLocalThreadState`
+**360 -> 368 bytes**. Baseline has an 8-byte tick, a 24-byte sink
+(`src/store/read_local_reclaim.h:61`) and 328-byte stats (38 `uint64_t`
+counters plus three arm counters: `src/core/thread.h:137`,
+`src/net/rob.h:62`). The added `atomic<bool> lane_active` follows them
+(`src/core/thread.h:303`); alignment adds seven padding bytes. The old
+prefix offset is now asserted as 360 at `:306`, total size 368 at `:308`.
+The diagnostics-only selector 3 retains its existing separate alignment
+and is excluded from these production asserts. It too uses only optional
+state. **Off pays zero bytes and no cache line for this growth**, because
+`Server::init` never calls `ThreadCtx::init_read_local_state` off. Existing
+hot fields remain at their old offsets on both arms.
+
+After the INFO fix, all four off-arm source properties above hold. Static
+verification completed: baseline source/token comparisons, Python AST
+parsing of the four changed batteries (without importing/running them),
+and `git diff HEAD --check`. The maintainer still owns build/layout-assert
+execution, both-mode boots, the gate and the PRE/POST rates. No gate row
+was added or retired; quick/full expectations remain **327/344**. The
+quick-tier branch starts at `tests/gate.sh:1258` and exits at `:1260`; the
+standalone RL2S battery has no row on either side of it.
+
 ## MEASUREMENT PLAN
 
 The maintainer performs all steps below, on the quiet box. No performance
@@ -221,7 +390,9 @@
    row automatically.
 2. Boot the four overlap-0 combinations: 2s/0, 2s/1, 1s/0, 1s/1. On eight
    real cores use 6:2 for split; omit ratio for 1s. Require INFO's effective
-   lane bit and active-reader count to match: 0, 6, 0, 8. Also smoke 2s/1
+   lane bit to be 0, 1, 0, 1. Enabled active-reader counts must be 6 and 8;
+   off arms must omit the new active-count/per-thread fields and retain
+   their baseline INFO payload. Also smoke 2s/1
    with one shard and 6:2 (one EX has no shard), explicit placement, and
    the existing TCP/TLS/unix and epoll/uring boot/shutdown cases. Confirm
    split/1/overlap-1 fails with the documented message.
diff --git a/src/cmd/t_server.cc b/src/cmd/t_server.cc
index 2d5c81bfdb1c5ee1dcedddf26af8e1801a2ea4d3..7ad03034a03f1c5a2918a29c761c28fa54bdcb96
--- a/src/cmd/t_server.cc
+++ b/src/cmd/t_server.cc
@@ -14,6 +14,7 @@
 #include "server_tail.h"
 #include "slowlog.h"
 #include "../base/alloc.h"
+#include "../core/genthread.h"
 #include "../core/server.h"
 #include "../core/lbsignals.h"
 #include "../core/pubsub_event.h"
@@ -1992,33 +1993,10 @@
                 static_cast<unsigned>(g_server ? g_server->cfg().port : 0),
                 static_cast<unsigned long long>(uptime),
                 static_cast<unsigned long long>(uptime / 86400));
-        uint32_t active_readers = 0;
-        if (g_server) {
-            uint32_t owned[kMaxThreads] = {};
-            // Read the published ownership directory, never a vector FLIP may be rewriting.
-            for (uint32_t sid = 0; sid < g_server->nshards(); sid++)
-                owned[g_server->worker_of_shard(static_cast<int32_t>(sid))]++;
-            for (uint32_t tid = 0; tid < g_server->nthreads(); tid++) {
-                const ThreadCtx& thread = g_server->thread(tid);
-                const bool active = thread.read_local_lane_active();
-                active_readers += active;
-                const Role role = thread.role();
-                const char* name = role == Role::Idle ? "idle"
-                    : g_server->thread_mode() == ThreadMode::Fused ? "unified"
-                    : role == Role::Ifid ? "ifid" : "ex";
-                const ReadLocalStats* stats = g_server->read_local_enabled()
-                    ? &thread.read_local_stats() : nullptr;
-                // These per-thread totals are lifetime counters; INFO stats retains its existing
-                // RESETSTAT-relative aggregate counters. FLIP does not reset either sidecar.
-                appendf(body,
-                        "read_local_thread_%u:role=%s,shards=%u,active=%u,"
-                        "hits_total=%llu,mget_hits_total=%llu\r\n",
-                        tid, name, owned[tid], active ? 1u : 0u,
-                        static_cast<unsigned long long>(stats ? stats->hits : 0),
-                        static_cast<unsigned long long>(stats ? stats->mget_local_hits : 0));
-            }
-        }
-        appendf(body, "read_local_active_threads:%u\r\n", active_readers);
+        // Guard the CALL, including all argument evaluation. The cold non-inlined helper owns
+        // its scratch buffers so the disabled INFO path keeps its old output without those arrays.
+        if (g_server && g_server->read_local_enabled())
+            append_read_local_thread_info(body, *g_server);
         if (g_server && g_server->thread_mode() == ThreadMode::Fused) {
             appendf(body,
                     "fused_threads:%u\r\nclient_threads:%u\r\nowner_threads:%u\r\n"
diff --git a/src/core/genthread.h b/src/core/genthread.h
index c47b788f303c3a7e7f482d5ddc9f5a580a0a9bbb..e874192a1f6c2412955de097a7cd14587582ee0f
--- a/src/core/genthread.h
+++ b/src/core/genthread.h
@@ -3,6 +3,7 @@
 #pragma once
 
 #include <memory>
+#include <string>
 #include <vector>
 
 namespace tomo {
@@ -27,4 +28,7 @@
                      LateUnixListener& unix_listener,
                      ShutdownReportFinalLine& final_report);
 
+// INFO-only diagnostics. The caller must test read_local_enabled() before this call.
+void append_read_local_thread_info(std::string& body, const Server& server);
+
 }  // namespace tomo
diff --git a/src/core/rl2s.cc b/src/core/rl2s.cc
index d942a2e0f0f948a54a3f9becd0e2e32efb231a98..274b33481ca4d2e1e9e9b06d9d4dc9da575090e8
--- a/src/core/rl2s.cc
+++ b/src/core/rl2s.cc
@@ -305,4 +305,40 @@
     return 0;
 }
 
+// Keep the scratch arrays out of cmd_info's frame, including with LTO. Only an armed INFO SERVER
+// request reaches this helper; the boot/runtime paths install no reporting callback or sidecar.
+__attribute__((noinline, cold))
+void append_read_local_thread_info(std::string& body, const Server& srv) {
+    uint32_t active_readers = 0;
+    uint32_t owned[kMaxThreads] = {};
+    // Read the published ownership directory, never a vector FLIP may be rewriting.
+    for (uint32_t sid = 0; sid < srv.nshards(); sid++)
+        owned[srv.worker_of_shard(static_cast<int32_t>(sid))]++;
+    char row[192];  // fixed grammar, including full-width uint32/uint64 counters
+    for (uint32_t tid = 0; tid < srv.nthreads(); tid++) {
+        const ThreadCtx& thread = srv.thread(tid);
+        const bool active = thread.read_local_lane_active();
+        active_readers += active;
+        const Role role = thread.role();
+        const char* name = role == Role::Idle ? "idle"
+            : srv.thread_mode() == ThreadMode::Fused ? "unified"
+            : role == Role::Ifid ? "ifid" : "ex";
+        const ReadLocalStats& stats = thread.read_local_stats();
+        // Lifetime counters survive FLIP and RESETSTAT; INFO stats retains its existing
+        // RESETSTAT-relative aggregate counters.
+        const int n = std::snprintf(row, sizeof(row),
+            "read_local_thread_%u:role=%s,shards=%u,active=%u,"
+            "hits_total=%llu,mget_hits_total=%llu\r\n",
+            tid, name, owned[tid], active ? 1u : 0u,
+            static_cast<unsigned long long>(stats.hits),
+            static_cast<unsigned long long>(stats.mget_local_hits));
+        if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+        body.append(row, static_cast<size_t>(n));
+    }
+    const int n = std::snprintf(row, sizeof(row), "read_local_active_threads:%u\r\n",
+                              active_readers);
+    if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+    body.append(row, static_cast<size_t>(n));
+}
+
 }  // namespace tomo
diff --git a/src/core/thread.h b/src/core/thread.h
index adea9605d0f0d40c3566e07b90a2663accb6b835..61802252934cbd450279ad7cd2dbaa8d5f03f465
--- a/src/core/thread.h
+++ b/src/core/thread.h
@@ -303,6 +303,8 @@
     std::atomic<bool> lane_active{false};
 };
 #if TOMO_READ_LOCAL_SET_TAX_VARIANT != 3
+static_assert(offsetof(ReadLocalThreadState, lane_active) == 360,
+              "lane activation must follow the entire pre-RL2S sidecar");
 static_assert(sizeof(ReadLocalThreadState) == 368,
               "lane activation adds one alignment word only to the optional sidecar");
 #endif

exec
/bin/bash -lc "nl -ba src/core/io_loop.h | sed -n '5718,5771p'
nl -ba src/core/io_loop.h | sed -n '6766,6800p'
nl -ba src/core/io_loop.h | sed -n '6848,6859p'
rg -n '"'^ *(template|uint32_t|bool|void).*'"' src/core/io_loop.h | sed -n '/54[89][0-9]:/,/5560:/p' | head -12" in /home/user/Projects/cx-rl2s
 succeeded in 0ms:
  5718	                    if (wb_.take_send_failure()) epoll_request_close(c);
  5719	                }
  5720	            }
  5721	
  5722	            if (!c->closing() && conn.rpos() < conn.rlen() && !c->scatter_barrier() &&
  5723	                !c->parse_backpressure()) {
  5724	                if (__builtin_expect(climon_pause_armed(), false)) {
  5725	                    const uint32_t rpos_before = conn.rpos();
  5726	                    if constexpr (HasTls) {
  5727	                        if (c->is_tls())
  5728	                            dispatch_result = parse_and_dispatch<
  5729	                                true, ParseBatchOps, false, Pipeline == 2, true,
  5730	                                Pipeline == 1, Pipeline == 1>(
  5731	                                    c, pipeline_batch);
  5732	                        else
  5733	                            dispatch_result = parse_and_dispatch<
  5734	                                false, ParseBatchOps, false, Pipeline == 2, true,
  5735	                                Pipeline == 1, Pipeline == 1>(
  5736	                                    c, pipeline_batch);
  5737	                    } else {
  5738	                        dispatch_result = parse_and_dispatch<
  5739	                            false, ParseBatchOps, false, Pipeline == 2, true,
  5740	                            Pipeline == 1, Pipeline == 1>(c, pipeline_batch);
  5741	                    }
  5742	                    if (conn.rpos() != rpos_before) work++;
  5743	                } else {
  5744	                    if constexpr (HasTls) {
  5745	                        if (c->is_tls())
  5746	                            dispatch_result = parse_and_dispatch<
  5747	                                true, ParseBatchOps, false, Pipeline == 2, true,
  5748	                                Pipeline == 1, Pipeline == 1>(
  5749	                                    c, pipeline_batch);
  5750	                        else
  5751	                            dispatch_result = parse_and_dispatch<
  5752	                                false, ParseBatchOps, false, Pipeline == 2, true,
  5753	                                Pipeline == 1, Pipeline == 1>(
  5754	                                    c, pipeline_batch);
  5755	                    } else {
  5756	                        dispatch_result = parse_and_dispatch<
  5757	                            false, ParseBatchOps, false, Pipeline == 2, true,
  5758	                            Pipeline == 1, Pipeline == 1>(c, pipeline_batch);
  5759	                    }
  5760	                    if (__builtin_expect(dispatch_result != DispatchResult::NeedInput, true))
  5761	                        work++;
  5762	                }
  5763	            }
  5764	
  5765	            if constexpr (!kEp) {
  5766	                bool staged_ifid = false;
  5767	                if constexpr (Pipeline == 2)
  5768	                    staged_ifid = pipeline_batch &&
  5769	                        (pipeline_batch->count != pipeline_count_before ||
  5770	                         c->pipeline_prepared());
  5771	                if (!staged_ifid) {
  6766	                // With no pause armed the accounting is byte-for-byte the pre-lane behaviour --
  6767	                // one predicted-false test per active connection per pass. The dispatch variant
  6768	                // stays keyed on c->is_tls() (the kTLS handoff's contract), not the slot pointer.
  6769	                if (__builtin_expect(climon_pause_armed(), false)) {
  6770	                    const uint32_t rpos_before = conn.rpos();
  6771	                    if constexpr (HasTls) {
  6772	                        if (c->is_tls())
  6773	                            dispatch_result = parse_and_dispatch<
  6774	                                true, Fused ? kGenthreadIfidBatchOps : 0>(c);
  6775	                        else
  6776	                            dispatch_result = parse_and_dispatch<
  6777	                                false, Fused ? kGenthreadIfidBatchOps : 0>(c);
  6778	                    } else {
  6779	                        dispatch_result = parse_and_dispatch<
  6780	                            false, Fused ? kGenthreadIfidBatchOps : 0>(c);
  6781	                    }
  6782	                    if (conn.rpos() != rpos_before) work++;
  6783	                } else {
  6784	                    if constexpr (HasTls) {
  6785	                        if (c->is_tls())
  6786	                            dispatch_result = parse_and_dispatch<
  6787	                                true, Fused ? kGenthreadIfidBatchOps : 0>(c);
  6788	                        else
  6789	                            dispatch_result = parse_and_dispatch<
  6790	                                false, Fused ? kGenthreadIfidBatchOps : 0>(c);
  6791	                    } else {
  6792	                        dispatch_result = parse_and_dispatch<
  6793	                            false, Fused ? kGenthreadIfidBatchOps : 0>(c);
  6794	                    }
  6795	                    if (__builtin_expect(
  6796	                            dispatch_result != DispatchResult::NeedInput, true))
  6797	                        work++;
  6798	                }
  6799	            }
  6800	
  6848	            }
  6849	        }
  6850	
  6851	        // The fused loop rotates whole streams: finish the bounded IFID phase above, execute one
  6852	        // batch, collect its local self-lane completions, then enter the existing write-back phase.
  6853	        if constexpr (Fused) {
  6854	            work += SweepPass ? fused_executor_->fused_baseline_sweep()
  6855	                              : fused_executor_->fused_baseline_pass();
  6856	            work += collect_retire_work<HasUnix, kEp>(SweepPass);
  6857	        }
  6858	
  6859	        // PUB/SUB PASS BOUNDARY -- between parsing and serving, on purpose. Everything this pass
5488:    template <bool HasUnix, bool HasTls, bool kEp, bool Fused = false>
5489:    uint32_t sweep() {
5490:        uint32_t work = 0;
5510:    template <bool HasUnix, bool HasTls, bool kEp>
5511:    uint32_t pipeline_sweep(bool natural_order, bool& submitted) {
5512:        uint32_t work = 0;
5535:    void queue_borrow_release(int32_t shard, const char* ptr) {
5540:    uint32_t flush_borrow_releases() {
5541:        uint32_t n = 0;
5552:    template <bool HasUnix, bool kEp, bool TargetedIfid = false>
5553:    uint32_t collect_retire_work(bool unmasked = false) {
5554:        uint32_t pubsub_work = 0;

diff --git a/RL2S.md b/RL2S.md
index 64dea226f85e5b4b296ed8c9f1de21ae463a8cf0..3a58bf02bfafd7ae3a505d79242145d586f17032
--- a/RL2S.md
+++ b/RL2S.md
@@ -76,7 +76,8 @@
 guards evaluation of `active_.size()`; no admission expression moved ahead
 of its old check.
 
-All physical threads provision an `IoLoop` and `FusedExLoop`. They use the
+On an enabled `2s/1` boot, all physical threads provision an `IoLoop` and
+`FusedExLoop`. They use the
 ordinary split task inbox layout and producer masks, including FLIP's
 existing remasking. Initial persistence loading runs before store arming.
 Each executor's `activate_fused` binds its permanent ThreadCtx sink, then
@@ -136,7 +137,8 @@
 1984, ThreadCtx 1408, Shard 1440, FlatStore 944, Rob<64> 192, AtomicEntry 144,
 Config 624; the ordinary ExLoop assert remains 6112. The only data layout
 change is the optional `ReadLocalThreadState`: 360 -> 368 bytes in the
-production build, asserted at `src/core/thread.h:306`. An atomic loop-entry
+production build, asserted at `src/core/thread.h:308`; its old 360-byte
+prefix is also locked at `src/core/thread.h:306`. An atomic loop-entry
 flag plus alignment occupies the extra word, after all existing members.
 The diagnostics-only selector-3 sidecar already has different alignment
 and is excluded from this production-size assert. Read-local 0 allocates
@@ -148,7 +150,8 @@
 ## Observable activation
 
 `INFO server` still reports `thread_mode:2s`, live `io_threads` and
-`ex_threads`, and effective `read_local:1`. It now also reports:
+`ex_threads`, and effective `read_local:1`. When the effective lane is
+enabled, it also reports:
 
 ```text
 read_local_thread_0:role=ifid,shards=0,active=1,hits_total=123,mget_hits_total=7
@@ -161,10 +164,15 @@
 network waits; it is not inferred from the knob or ownership. The atomic
 flag is written only at loop entry/exit. Shard counts come from published
 owner IDs, not concurrently modified ownership vectors
-(`src/cmd/t_server.cc:1995`). After FLIP settles, every IFID row must have
+(`src/core/rl2s.cc:313`). After FLIP settles, every IFID row must have
 shards=0/active=1, and every EX row active=0. As with other INFO geometry,
 the individual samples are not one transactional view during a move.
 
+With read-local disabled, both new field families are absent, preserving
+the baseline INFO payload. Their caller guard is at
+`src/cmd/t_server.cc:1998`; their scratch arrays and formatting live in the
+non-inlined helper at `src/core/rl2s.cc:310`.
+
 Per-thread `*_total` values are lifetime completions and survive FLIP and
 CONFIG RESETSTAT. They can be nonzero on an EX row after that thread has
 previously served IO. Aggregate `INFO stats` keeps its existing RESETSTAT
@@ -203,6 +211,167 @@
 row on either side of that exit. Updating existing battery internals does
 not change gate row counts.
 
+## OFF-ARM PROOF
+
+Compared against **`78c3e5391`**, for **`--thread-mode 2s --read-local 0`**,
+including its supported overlap 0 and 1 schedules. References below name
+the final working-tree sources unless explicitly marked baseline. This is
+a source and construction proof; no build, server, benchmark or gate was
+run. It does not establish identical generated instructions or measured
+PRE/POST rates.
+
+**Finding and fix.** The submitted diff did not preserve disabled INFO:
+it counted shard owners, visited every thread and formatted
+`read_local_thread_*` and `read_local_active_threads` on off boots too.
+That changed replies and could grow INFO's output allocation. Its
+`owned[kMaxThreads]` array also lived in the common `cmd_info` frame.
+The fix guards the **call** with the effective predicate at
+`src/cmd/t_server.cc:1998`, and moves the entire walk, scratch arrays and
+formatting into `append_read_local_thread_info` in `src/core/rl2s.cc:310`.
+`noinline` keeps those arrays out of the caller, including under LTO.
+The helper receives only references, after the guard; it does not take
+an eagerly computed topology snapshot or allocate before early-returning.
+Disabled INFO emits neither new field family. Enabled INFO keeps both.
+The offset assert at `src/core/thread.h:306` additionally locks the old
+sidecar prefix; it adds no runtime state. The lane implementation is kept.
+
+### 1. Threads, capabilities and roles: unchanged off
+
+The effective predicate is `cfg.overlap == 0 && cfg.read_local != 0`
+(`src/core/server.h:582`). It is false when the knob is zero. The only two
+thread modes are Split and Fused (`src/core/config.h:225`): `main` returns
+through the existing fused path at `src/main.cc:312`, then tests the
+predicate at `:317`. Thus the **new runtime** can be called only for
+Split **and** read-local 1, with overlap 0. Its entry also checks this
+contract before construction (`src/core/rl2s.cc:77`). Mode, overlap and
+read-local remain boot-only CONFIG entries (`src/cmd/t_server.cc:322`).
+
+Off falls through to the old `vector<IoLoop>` and `vector<ExLoop>` at
+`src/main.cc:338`; `ExLoop` still means **`ExLoopT<false>`**
+(`src/core/ex_loop.h:3439`). It does not construct the new
+`vector<FusedExLoop>` at `src/core/rl2s.cc:92`, bind a fused executor,
+call `activate_fused`, or enter `run_fused`.
+
+Placement still builds the ordinary split ratio/explicit placement
+(`src/core/server.h:239`), creates `placement_.total_threads()` contexts
+(`:294`, `:305`), and assigns shard homes to EX tids
+(`src/core/placement.h:178`, `src/core/server.h:367`). Actual OS workers
+are created once per EX tid at `src/main.cc:372` and once per IFID tid at
+`:507`. At gate geometry this is **8 workers: 6 IFID, 2 EX, 16 shards on
+EX**, not an additional reader tier. Main remains the existing monitor.
+
+Baseline already provisioned the opposite dormant loop on every worker
+for FLIP: dormant IO on EX at `src/main.cc:395`, dormant ordinary EX on IO
+at `:528`. Those allocations/capabilities are not new. Off role changes
+still call only `ios[tid].run()` / `exs[tid].run()` (`:426`, `:562`), so
+FLIP cannot later upgrade an off worker to the reader runtime. The entire
+ordinary split construction/role-loop/shutdown suffix starting at
+`src/main.cc:336` was compared byte-for-byte with baseline and is identical.
+
+### 2. Allocations, buffers, sinks and registrations: none added off
+
+The inventory includes existing allocations newly reachable by the armed
+split runtime, not just literal `new` lines in `rl2s.cc`.
+
+| Construction or allocation | Guard and off-arm result |
+| --- | --- |
+| Server read-local sidecar (`new ReadLocalServerState`) | Existing predicate guard at `src/core/server.h:316`, allocation `:317`. False off; the existing owning pointer stays null. |
+| Per-thread sidecars, tick publications, stats and sink slots | Same guard encloses the thread loop at `src/core/server.h:322`; `src/core/thread.h:353` performs `new ReadLocalThreadState`. Never called off, including for future FLIP readers/owners. |
+| Per-shard extended store state and foreign-read safety arrays | Same guard encloses `prepare_read_local` at `src/core/server.h:328`. It reaches `new ReadLocalStoreState` at `src/store/flatstore_atomic.inc:890`. Off stores retain the original ordinary atomic-state allocation path. |
+| `ReadLocalExImpl`, `LaneEntry[kInboxSlots]`, fallback array | `src/core/ex_loop.h:196` first requires compile-time `Fused`, then the effective predicate at `:197`; the three `new` sites are `:198`, `:200`, `:202`. The entire block is discarded for the off `ExLoopT<false>`. |
+| Deferred `Entry[kCapacity]` buffer, block-cache heads and retire sink | Deferred initialization is inside that same block (`src/core/ex_loop.h:204`); buffer allocation is `src/core/read_local.h:260`, sink binding `:262`. Cache heads and retire-ring bookkeeping are members of the optional queue (`:464`), not a baseline per-thread allocation. No off queue exists to grow debug pending sets or retain blocks. |
+| Runtime vectors, default-constructed loop containers and worker closures | `pool`, `ios(nthreads)`, `executors(nthreads)` at `src/core/rl2s.cc:90`, then `pool.emplace_back` at `:122`, are inside the guarded runtime call. This includes executor deque/map allocations during default construction, not merely `init`. The off runtime constructs only the baseline vectors/loops described above. |
+| Boot-gate worker-state vector, loading/error strings and shutdown report buffers | `FusedBootGate boot(nthreads)` at `src/core/rl2s.cc:95` allocates its vector in `src/core/fused_boot_gate.h:23`. The new runtime's local strings (`src/core/rl2s.cc:130`, `:235`, `:263`, `:281`) and report (`:115`) share the same call guard. No static boot gate or report is constructed for off. |
+| Inbox channels, IO/EX rings, network registration and listener resources | All new-runtime calls are below its guard: `src/core/rl2s.cc:127`, `:129`, `:144`, `:196`, `:248`, `:264`, `:270`. Inbox storage comes from `src/core/thread.h:338`; rings from `src/core/ex_loop.h:213` and `src/core/io_loop.h:102`. Off calls the unchanged baseline initializers with the same parameters; it does not additionally run any of these RL2S initializers. |
+| Role/client registration, capacity, completion, demotion and progress hooks | `src/core/rl2s.cc:147`, `:152`, `:160`, `:165`, `:167`, `:172`, `:181` are inside the enabled runtime. Fused sink publication and store arming occur in `src/core/ex_loop.h:241` and `:245`. Off never installs those fused hooks; its pre-existing ordinary IO registration hooks remain. |
+| Connection RYOW sidecar and local-read reply reserves | Parser admission remains behind compile-time `Fused` and the effective predicate (`src/core/io_loop.h:4200`). Its write marking at `:4253` can reach `Rob::prepare_read_local` (`src/net/rob.h:486`, `:709`) only in that armed path. Local-copy reply reservation (`src/core/ex_loop.h:990`) likewise belongs to the existing reader lane. No new accept-time buffer or sidecar allocation was added. |
+| New INFO ownership array, row buffer and output growth | Fixed: caller guard at `src/cmd/t_server.cc:1998` precedes helper entry; `owned` and `row` exist only in `src/core/rl2s.cc:313`, `:317`. Both `body.append` sites (`:336`, `:341`) are inside that helper. Off emits the baseline bytes and takes none of these buffer allocations. |
+
+There is no new `.reserve()`/`.resize()` call in the feature file and no
+file-scope runtime object or thread-local registration. Its project header
+dependencies were already included by main/genthread. All feature-owned
+vectors, strings, arrays and OS-thread creation are local to the two
+guarded entry points. Existing optional sidecars store the hooks; no
+separate always-present registry was introduced.
+
+QSBR registration is the optional per-thread state allocated at boot,
+not mere membership in the baseline `threads_` vector. Its floor scanner
+(`src/core/server.h:606`) is driven by the optional deferred queues.
+Neither the scanner, a tick/park publication, nor a sink binding is added
+to an off thread. Existing store ownership-edge checks remain unchanged
+(`src/core/server.h:1951`) and return for unarmed stores.
+
+### 3. Hot loops and eager arguments: no added off-arm branch
+
+| Changed test or call | Why off does not execute it |
+| --- | --- |
+| Runtime selection and overlap validation | `src/main.cc:317` and `src/core/config.h:1215` are boot-only, before operation processing. The changed startup notice at `src/main.cc:183` is also cold and silent with read-local 0. |
+| IO reader-entry resume/epoch/active publication and exit publication | Both are inside `if constexpr (Fused)` at `src/core/io_loop.h:541` and `:766`. Ordinary `run()` selects `run_split<0/1>` (`:429`); every network/TLS/unix variant passes `Fused=false` (`:407`). The new calls and **all their arguments**, including `read_local_epoch()`, are discarded. Entry/exit are tenure boundaries even on the enabled arm. |
+| EX parked/lane-count check, config invalidation, debug ownership check and retire drain | All additions to `ExLoopT::run` are inside `if constexpr (Fused)` (`src/core/ex_loop.h:708`, `:723`, `:800`). Off instantiates `ExLoopT<false>`; it does not evaluate `read_local_impl()`, `self_->id()` for the new check, or epoch/sink arguments. This also holds with `TOMO_RL_CACHE_DEBUG`. |
+| New mode tests in baseline fused pass/sweep | `src/core/ex_loop.h:397` and `:640` live in `static_assert(Fused)` entry points. Off never calls them. They do add a rotation-boundary test to the existing **1s** schedule; the 1s PRE/POST measurement obligation remains. |
+| Shard-less pass's config, clock, pressure and cap checks | Entirely in the enabled-only `ExLoopT<true>::split_read_local_pass` (`src/core/rl2s.cc:46`). No callback to it is installed off. |
+| `OwnsShards` config-refresh selection | `src/core/ex_loop.h:1913` defaults to **true** on all old calls. `if constexpr (OwnsShards)` adds no runtime test. Only `src/core/rl2s.cc:49` calls `<false>`. Expanding the true block gives the baseline owner-refresh token sequence. |
+| Changed shared `Server::read_local_enabled` predicate | The ordinary parser's `Fused && ...` at `src/core/io_loop.h:3924` short-circuits at compile time. In common IO config refresh, the existing version-change return at `:7135` still precedes the predicate at `:7146`; changing the predicate adds no check to unchanged-version rotations. Other shared uses are boot/config preparation and INFO/RESETSTAT, not the GET/SET dispatch/execution loops. |
+| New INFO caller guard | `src/cmd/t_server.cc:1998` is inside the INFO SERVER command section (`:1975`), not any general operation or worker loop. All new per-shard/per-thread reporting loops are behind it in the cold helper. |
+
+The parser/admission/dispatch function at `src/core/io_loop.h:3918` is
+unchanged. In particular, `if constexpr (Fused)` and the enabled/pressure
+tests at `:3992` still precede `read_local_lane_quota(active_.size())` at
+`:3995`. Off evaluates **neither** the quota call **nor** `active_.size()`;
+there is no new cold-cache-line load disguised as a no-op callee.
+
+Source comparisons discarded the `if constexpr (Fused)` statements in
+both revisions of IO/EX rotation bodies; the remaining token sequences
+match. The initializers, ordinary split dispatch entry points, parser,
+owner task drain and IO config-refresh bodies also match baseline tokens.
+These comparisons check the source specialization, not compiler output.
+
+### 4. Per-operation layouts: unchanged off; optional growth disclosed
+
+| Type | Baseline bytes | Current bytes | Existing layout assert |
+| --- | ---: | ---: | --- |
+| `Op` | 336 | 336 | `src/exec/op.h:463` |
+| `Client` | 1984 | 1984 | `src/net/conn.h:905` |
+| `ThreadCtx` | 1408 | 1408 | `src/core/thread.h:1208` |
+| `Shard` | 1440 | 1440 | `src/core/shard.h:523` |
+| `FlatStore` | 944 | 944 | `src/store/flatstore.h:3912` |
+| `Rob<64>` | 192 | 192 | `src/net/rob.h:1098` |
+| `AtomicEntry` | 144 | 144 | `src/store/atomic_mvcc.h:70` |
+| `Config` | 624 | 624 | `src/core/config.h:429` |
+| Ordinary `ExLoop` | 6112 | 6112 | `src/core/ex_loop.h:3450` |
+
+No data members were added/reordered in `IoLoop`, `Server`, `ThreadCtx`
+or `ExLoopT`: their member declarations match baseline, including the
+existing optional pointer in `ThreadCtx`'s tail (`src/core/thread.h:1205`)
+and the empty `ReadLocalExState<false>` (`src/core/ex_loop.h:103`, `:165`)
+held with `[[no_unique_address]]` (`:3436`). The other locked type files
+are byte-identical; Config differs only in its field's comment. Thus no
+off-arm object has a new allocation stride, field offset or cache line.
+IoLoop and Server have no numeric whole-size assert here; their unchanged
+member/type layouts, rather than an invented size, are the evidence.
+
+**One on-arm sidecar did grow:** production `ReadLocalThreadState`
+**360 -> 368 bytes**. Baseline has an 8-byte tick, a 24-byte sink
+(`src/store/read_local_reclaim.h:61`) and 328-byte stats (38 `uint64_t`
+counters plus three arm counters: `src/core/thread.h:137`,
+`src/net/rob.h:62`). The added `atomic<bool> lane_active` follows them
+(`src/core/thread.h:303`); alignment adds seven padding bytes. The old
+prefix offset is now asserted as 360 at `:306`, total size 368 at `:308`.
+The diagnostics-only selector 3 retains its existing separate alignment
+and is excluded from these production asserts. It too uses only optional
+state. **Off pays zero bytes and no cache line for this growth**, because
+`Server::init` never calls `ThreadCtx::init_read_local_state` off. Existing
+hot fields remain at their old offsets on both arms.
+
+After the INFO fix, all four off-arm source properties above hold. Static
+verification completed: baseline source/token comparisons, Python AST
+parsing of the four changed batteries (without importing/running them),
+and `git diff HEAD --check`. The maintainer still owns build/layout-assert
+execution, both-mode boots, the gate and the PRE/POST rates. No gate row
+was added or retired; quick/full expectations remain **327/344**. The
+quick-tier branch starts at `tests/gate.sh:1258` and exits at `:1260`; the
+standalone RL2S battery has no row on either side of it.
+
 ## MEASUREMENT PLAN
 
 The maintainer performs all steps below, on the quiet box. No performance
@@ -221,7 +390,9 @@
    row automatically.
 2. Boot the four overlap-0 combinations: 2s/0, 2s/1, 1s/0, 1s/1. On eight
    real cores use 6:2 for split; omit ratio for 1s. Require INFO's effective
-   lane bit and active-reader count to match: 0, 6, 0, 8. Also smoke 2s/1
+   lane bit to be 0, 1, 0, 1. Enabled active-reader counts must be 6 and 8;
+   off arms must omit the new active-count/per-thread fields and retain
+   their baseline INFO payload. Also smoke 2s/1
    with one shard and 6:2 (one EX has no shard), explicit placement, and
    the existing TCP/TLS/unix and epoll/uring boot/shutdown cases. Confirm
    split/1/overlap-1 fails with the documented message.
diff --git a/src/cmd/t_server.cc b/src/cmd/t_server.cc
index 2d5c81bfdb1c5ee1dcedddf26af8e1801a2ea4d3..7ad03034a03f1c5a2918a29c761c28fa54bdcb96
--- a/src/cmd/t_server.cc
+++ b/src/cmd/t_server.cc
@@ -14,6 +14,7 @@
 #include "server_tail.h"
 #include "slowlog.h"
 #include "../base/alloc.h"
+#include "../core/genthread.h"
 #include "../core/server.h"
 #include "../core/lbsignals.h"
 #include "../core/pubsub_event.h"
@@ -1992,33 +1993,10 @@
                 static_cast<unsigned>(g_server ? g_server->cfg().port : 0),
                 static_cast<unsigned long long>(uptime),
                 static_cast<unsigned long long>(uptime / 86400));
-        uint32_t active_readers = 0;
-        if (g_server) {
-            uint32_t owned[kMaxThreads] = {};
-            // Read the published ownership directory, never a vector FLIP may be rewriting.
-            for (uint32_t sid = 0; sid < g_server->nshards(); sid++)
-                owned[g_server->worker_of_shard(static_cast<int32_t>(sid))]++;
-            for (uint32_t tid = 0; tid < g_server->nthreads(); tid++) {
-                const ThreadCtx& thread = g_server->thread(tid);
-                const bool active = thread.read_local_lane_active();
-                active_readers += active;
-                const Role role = thread.role();
-                const char* name = role == Role::Idle ? "idle"
-                    : g_server->thread_mode() == ThreadMode::Fused ? "unified"
-                    : role == Role::Ifid ? "ifid" : "ex";
-                const ReadLocalStats* stats = g_server->read_local_enabled()
-                    ? &thread.read_local_stats() : nullptr;
-                // These per-thread totals are lifetime counters; INFO stats retains its existing
-                // RESETSTAT-relative aggregate counters. FLIP does not reset either sidecar.
-                appendf(body,
-                        "read_local_thread_%u:role=%s,shards=%u,active=%u,"
-                        "hits_total=%llu,mget_hits_total=%llu\r\n",
-                        tid, name, owned[tid], active ? 1u : 0u,
-                        static_cast<unsigned long long>(stats ? stats->hits : 0),
-                        static_cast<unsigned long long>(stats ? stats->mget_local_hits : 0));
-            }
-        }
-        appendf(body, "read_local_active_threads:%u\r\n", active_readers);
+        // Guard the CALL, including all argument evaluation. The cold non-inlined helper owns
+        // its scratch buffers so the disabled INFO path keeps its old output without those arrays.
+        if (g_server && g_server->read_local_enabled())
+            append_read_local_thread_info(body, *g_server);
         if (g_server && g_server->thread_mode() == ThreadMode::Fused) {
             appendf(body,
                     "fused_threads:%u\r\nclient_threads:%u\r\nowner_threads:%u\r\n"
diff --git a/src/core/genthread.h b/src/core/genthread.h
index c47b788f303c3a7e7f482d5ddc9f5a580a0a9bbb..e874192a1f6c2412955de097a7cd14587582ee0f
--- a/src/core/genthread.h
+++ b/src/core/genthread.h
@@ -3,6 +3,7 @@
 #pragma once
 
 #include <memory>
+#include <string>
 #include <vector>
 
 namespace tomo {
@@ -27,4 +28,7 @@
                      LateUnixListener& unix_listener,
                      ShutdownReportFinalLine& final_report);
 
+// INFO-only diagnostics. The caller must test read_local_enabled() before this call.
+void append_read_local_thread_info(std::string& body, const Server& server);
+
 }  // namespace tomo
diff --git a/src/core/rl2s.cc b/src/core/rl2s.cc
index d942a2e0f0f948a54a3f9becd0e2e32efb231a98..274b33481ca4d2e1e9e9b06d9d4dc9da575090e8
--- a/src/core/rl2s.cc
+++ b/src/core/rl2s.cc
@@ -305,4 +305,40 @@
     return 0;
 }
 
+// Keep the scratch arrays out of cmd_info's frame, including with LTO. Only an armed INFO SERVER
+// request reaches this helper; the boot/runtime paths install no reporting callback or sidecar.
+__attribute__((noinline, cold))
+void append_read_local_thread_info(std::string& body, const Server& srv) {
+    uint32_t active_readers = 0;
+    uint32_t owned[kMaxThreads] = {};
+    // Read the published ownership directory, never a vector FLIP may be rewriting.
+    for (uint32_t sid = 0; sid < srv.nshards(); sid++)
+        owned[srv.worker_of_shard(static_cast<int32_t>(sid))]++;
+    char row[192];  // fixed grammar, including full-width uint32/uint64 counters
+    for (uint32_t tid = 0; tid < srv.nthreads(); tid++) {
+        const ThreadCtx& thread = srv.thread(tid);
+        const bool active = thread.read_local_lane_active();
+        active_readers += active;
+        const Role role = thread.role();
+        const char* name = role == Role::Idle ? "idle"
+            : srv.thread_mode() == ThreadMode::Fused ? "unified"
+            : role == Role::Ifid ? "ifid" : "ex";
+        const ReadLocalStats& stats = thread.read_local_stats();
+        // Lifetime counters survive FLIP and RESETSTAT; INFO stats retains its existing
+        // RESETSTAT-relative aggregate counters.
+        const int n = std::snprintf(row, sizeof(row),
+            "read_local_thread_%u:role=%s,shards=%u,active=%u,"
+            "hits_total=%llu,mget_hits_total=%llu\r\n",
+            tid, name, owned[tid], active ? 1u : 0u,
+            static_cast<unsigned long long>(stats.hits),
+            static_cast<unsigned long long>(stats.mget_local_hits));
+        if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+        body.append(row, static_cast<size_t>(n));
+    }
+    const int n = std::snprintf(row, sizeof(row), "read_local_active_threads:%u\r\n",
+                              active_readers);
+    if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+    body.append(row, static_cast<size_t>(n));
+}
+
 }  // namespace tomo
diff --git a/src/core/thread.h b/src/core/thread.h
index adea9605d0f0d40c3566e07b90a2663accb6b835..61802252934cbd450279ad7cd2dbaa8d5f03f465
--- a/src/core/thread.h
+++ b/src/core/thread.h
@@ -303,6 +303,8 @@
     std::atomic<bool> lane_active{false};
 };
 #if TOMO_READ_LOCAL_SET_TAX_VARIANT != 3
+static_assert(offsetof(ReadLocalThreadState, lane_active) == 360,
+              "lane activation must follow the entire pre-RL2S sidecar");
 static_assert(sizeof(ReadLocalThreadState) == 368,
               "lane activation adds one alignment word only to the optional sidecar");
 #endif

apply patch
patch: completed
/home/user/Projects/cx-rl2s/RL2S.md
diff --git a/RL2S.md b/RL2S.md
index 64dea226f85e5b4b296ed8c9f1de21ae463a8cf0..c8cded74608febc9b1f04ba2c1c1a9c2aff81f37
--- a/RL2S.md
+++ b/RL2S.md
@@ -76,7 +76,8 @@
 guards evaluation of `active_.size()`; no admission expression moved ahead
 of its old check.
 
-All physical threads provision an `IoLoop` and `FusedExLoop`. They use the
+On an enabled `2s/1` boot, all physical threads provision an `IoLoop` and
+`FusedExLoop`. They use the
 ordinary split task inbox layout and producer masks, including FLIP's
 existing remasking. Initial persistence loading runs before store arming.
 Each executor's `activate_fused` binds its permanent ThreadCtx sink, then
@@ -136,7 +137,8 @@
 1984, ThreadCtx 1408, Shard 1440, FlatStore 944, Rob<64> 192, AtomicEntry 144,
 Config 624; the ordinary ExLoop assert remains 6112. The only data layout
 change is the optional `ReadLocalThreadState`: 360 -> 368 bytes in the
-production build, asserted at `src/core/thread.h:306`. An atomic loop-entry
+production build, asserted at `src/core/thread.h:308`; its old 360-byte
+prefix is also locked at `src/core/thread.h:306`. An atomic loop-entry
 flag plus alignment occupies the extra word, after all existing members.
 The diagnostics-only selector-3 sidecar already has different alignment
 and is excluded from this production-size assert. Read-local 0 allocates
@@ -148,7 +150,8 @@
 ## Observable activation
 
 `INFO server` still reports `thread_mode:2s`, live `io_threads` and
-`ex_threads`, and effective `read_local:1`. It now also reports:
+`ex_threads`, and effective `read_local:1`. When the effective lane is
+enabled, it also reports:
 
 ```text
 read_local_thread_0:role=ifid,shards=0,active=1,hits_total=123,mget_hits_total=7
@@ -161,10 +164,15 @@
 network waits; it is not inferred from the knob or ownership. The atomic
 flag is written only at loop entry/exit. Shard counts come from published
 owner IDs, not concurrently modified ownership vectors
-(`src/cmd/t_server.cc:1995`). After FLIP settles, every IFID row must have
+(`src/core/rl2s.cc:313`). After FLIP settles, every IFID row must have
 shards=0/active=1, and every EX row active=0. As with other INFO geometry,
 the individual samples are not one transactional view during a move.
 
+With read-local disabled, both new field families are absent, preserving
+the baseline INFO payload. Their caller guard is at
+`src/cmd/t_server.cc:1998`; their scratch arrays and formatting live in the
+non-inlined helper at `src/core/rl2s.cc:310`.
+
 Per-thread `*_total` values are lifetime completions and survive FLIP and
 CONFIG RESETSTAT. They can be nonzero on an EX row after that thread has
 previously served IO. Aggregate `INFO stats` keeps its existing RESETSTAT
@@ -203,6 +211,167 @@
 row on either side of that exit. Updating existing battery internals does
 not change gate row counts.
 
+## OFF-ARM PROOF
+
+Compared against **`78c3e5391`**, for **`--thread-mode 2s --read-local 0`**,
+including its supported overlap 0 and 1 schedules. References below name
+the final working-tree sources unless explicitly marked baseline. This is
+a source and construction proof; no build, server, benchmark or gate was
+run. It does not establish identical generated instructions or measured
+PRE/POST rates.
+
+**Finding and fix.** The submitted diff did not preserve disabled INFO:
+it counted shard owners, visited every thread and formatted
+`read_local_thread_*` and `read_local_active_threads` on off boots too.
+That changed replies and could grow INFO's output allocation. Its
+`owned[kMaxThreads]` array also lived in the common `cmd_info` frame.
+The fix guards the **call** with the effective predicate at
+`src/cmd/t_server.cc:1998`, and moves the entire walk, scratch arrays and
+formatting into `append_read_local_thread_info` in `src/core/rl2s.cc:310`.
+`noinline` keeps those arrays out of the caller, including under LTO.
+The helper receives only references, after the guard; it does not take
+an eagerly computed topology snapshot or allocate before early-returning.
+Disabled INFO emits neither new field family. Enabled INFO keeps both.
+The offset assert at `src/core/thread.h:306` additionally locks the old
+sidecar prefix; it adds no runtime state. The lane implementation is kept.
+
+### 1. Threads, capabilities and roles: unchanged off
+
+The effective predicate is `cfg.overlap == 0 && cfg.read_local != 0`
+(`src/core/server.h:582`). It is false when the knob is zero. The only two
+thread modes are Split and Fused (`src/core/config.h:225`): `main` returns
+through the existing fused path at `src/main.cc:312`, then tests the
+predicate at `:317`. Thus the **new runtime** can be called only for
+Split **and** read-local 1, with overlap 0. Its entry also checks this
+contract before construction (`src/core/rl2s.cc:77`). Mode, overlap and
+read-local remain boot-only CONFIG entries (`src/cmd/t_server.cc:322`).
+
+Off falls through to the old `vector<IoLoop>` and `vector<ExLoop>` at
+`src/main.cc:338`; `ExLoop` still means **`ExLoopT<false>`**
+(`src/core/ex_loop.h:3439`). It does not construct the new
+`vector<FusedExLoop>` at `src/core/rl2s.cc:92`, bind a fused executor,
+call `activate_fused`, or enter `run_fused`.
+
+Placement still builds the ordinary split ratio/explicit placement
+(`src/core/server.h:239`), creates `placement_.total_threads()` contexts
+(`:294`, `:305`), and assigns shard homes to EX tids
+(`src/core/placement.h:178`, `src/core/server.h:367`). Actual OS workers
+are created once per EX tid at `src/main.cc:372` and once per IFID tid at
+`:507`. At gate geometry this is **8 workers: 6 IFID, 2 EX, 16 shards on
+EX**, not an additional reader tier. Main remains the existing monitor.
+
+Baseline already provisioned the opposite dormant loop on every worker
+for FLIP: dormant IO on EX at `src/main.cc:395`, dormant ordinary EX on IO
+at `:528`. Those allocations/capabilities are not new. Off role changes
+still call only `ios[tid].run()` / `exs[tid].run()` (`:426`, `:562`), so
+FLIP cannot later upgrade an off worker to the reader runtime. The entire
+ordinary split construction/role-loop/shutdown suffix starting at
+`src/main.cc:336` was compared byte-for-byte with baseline and is identical.
+
+### 2. Allocations, buffers, sinks and registrations: none added off
+
+The inventory includes existing allocations newly reachable by the armed
+split runtime, not just literal `new` lines in `rl2s.cc`.
+
+| Construction or allocation | Guard and off-arm result |
+| --- | --- |
+| Server read-local sidecar (`new ReadLocalServerState`) | Existing predicate guard at `src/core/server.h:316`, allocation `:317`. False off; the existing owning pointer stays null. |
+| Per-thread sidecars, tick publications, stats and sink slots | Same guard encloses the thread loop at `src/core/server.h:322`; `src/core/thread.h:353` performs `new ReadLocalThreadState`. Never called off, including for future FLIP readers/owners. |
+| Per-shard extended store state and foreign-read safety arrays | Same guard encloses `prepare_read_local` at `src/core/server.h:328`. It reaches `new ReadLocalStoreState` at `src/store/flatstore_atomic.inc:890`. Off stores retain the original ordinary atomic-state allocation path. |
+| `ReadLocalExImpl`, `LaneEntry[kInboxSlots]`, fallback array | `src/core/ex_loop.h:196` first requires compile-time `Fused`, then the effective predicate at `:197`; the three `new` sites are `:198`, `:200`, `:202`. The entire block is discarded for the off `ExLoopT<false>`. |
+| Deferred `Entry[kCapacity]` buffer, block-cache heads and retire sink | Deferred initialization is inside that same block (`src/core/ex_loop.h:204`); buffer allocation is `src/core/read_local.h:260`, sink binding `:262`. Cache heads and retire-ring bookkeeping are members of the optional queue (`:464`), not a baseline per-thread allocation. No off queue exists to grow debug pending sets or retain blocks. |
+| Runtime vectors, default-constructed loop containers and worker closures | `pool`, `ios(nthreads)`, `executors(nthreads)` at `src/core/rl2s.cc:90`, then `pool.emplace_back` at `:122`, are inside the guarded runtime call. This includes allocations by embedded container constructors, such as executor deques, before `init`. The off runtime constructs only the baseline vectors/loops described above. |
+| Boot-gate worker-state vector, loading/error strings and shutdown report buffers | `FusedBootGate boot(nthreads)` at `src/core/rl2s.cc:95` allocates its vector in `src/core/fused_boot_gate.h:23`. The new runtime's local strings (`src/core/rl2s.cc:130`, `:235`, `:263`, `:281`) and report (`:115`) share the same call guard. No static boot gate or report is constructed for off. |
+| Inbox channels, IO/EX rings, network registration and listener resources | All new-runtime calls are below its guard: `src/core/rl2s.cc:127`, `:129`, `:144`, `:196`, `:248`, `:264`, `:270`. Inbox storage comes from `src/core/thread.h:338`; rings from `src/core/ex_loop.h:213` and `src/core/io_loop.h:102`. Off calls the unchanged baseline initializers with the same parameters; it does not additionally run any of these RL2S initializers. |
+| Role/client registration, capacity, completion, demotion and progress hooks | `src/core/rl2s.cc:147`, `:152`, `:160`, `:165`, `:167`, `:172`, `:181` are inside the enabled runtime. Fused sink publication and store arming occur in `src/core/ex_loop.h:241` and `:245`. Off never installs those fused hooks; its pre-existing ordinary IO registration hooks remain. |
+| Connection RYOW sidecar and local-read reply reserves | Parser admission remains behind compile-time `Fused` and the effective predicate (`src/core/io_loop.h:4200`). Its write marking at `:4253` can reach `Rob::prepare_read_local` (`src/net/rob.h:486`, `:709`) only in that armed path. Local-copy reply reservation (`src/core/ex_loop.h:990`) likewise belongs to the existing reader lane. No new accept-time buffer or sidecar allocation was added. |
+| New INFO ownership array, row buffer and output growth | Fixed: caller guard at `src/cmd/t_server.cc:1998` precedes helper entry; `owned` and `row` exist only in `src/core/rl2s.cc:313`, `:317`. Both `body.append` sites (`:336`, `:341`) are inside that helper. Off emits the baseline bytes and takes none of these buffer allocations. |
+
+There is no new `.reserve()`/`.resize()` call in the feature file and no
+file-scope runtime object or thread-local registration. Its project header
+dependencies were already included by main/genthread. All feature-owned
+vectors, strings, arrays and OS-thread creation are local to the two
+guarded entry points. Existing optional sidecars store the hooks; no
+separate always-present registry was introduced.
+
+QSBR registration is the optional per-thread state allocated at boot,
+not mere membership in the baseline `threads_` vector. Its floor scanner
+(`src/core/server.h:606`) is driven by the optional deferred queues.
+Neither the scanner, a tick/park publication, nor a sink binding is added
+to an off thread. Existing store ownership-edge checks remain unchanged
+(`src/core/server.h:1951`) and return for unarmed stores.
+
+### 3. Hot loops and eager arguments: no added off-arm branch
+
+| Changed test or call | Why off does not execute it |
+| --- | --- |
+| Runtime selection and overlap validation | `src/main.cc:317` and `src/core/config.h:1215` are boot-only, before operation processing. The changed startup notice at `src/main.cc:183` is also cold and silent with read-local 0. |
+| IO reader-entry resume/epoch/active publication and exit publication | Both are inside `if constexpr (Fused)` at `src/core/io_loop.h:541` and `:766`. Ordinary `run()` selects `run_split<0/1>` (`:429`); every network/TLS/unix variant passes `Fused=false` (`:407`). The new calls and **all their arguments**, including `read_local_epoch()`, are discarded. Entry/exit are tenure boundaries even on the enabled arm. |
+| EX parked/lane-count check, config invalidation, debug ownership check and retire drain | All additions to `ExLoopT::run` are inside `if constexpr (Fused)` (`src/core/ex_loop.h:708`, `:723`, `:800`). Off instantiates `ExLoopT<false>`; it does not evaluate `read_local_impl()`, `self_->id()` for the new check, or epoch/sink arguments. This also holds with `TOMO_RL_CACHE_DEBUG`. |
+| New mode tests in baseline fused pass/sweep | `src/core/ex_loop.h:397` and `:640` live in `static_assert(Fused)` entry points. Their IO call sites are themselves under `if constexpr (Fused)` at `src/core/io_loop.h:6853`. Off never calls them. They do add a rotation-boundary test to the existing **1s** schedule; the 1s PRE/POST measurement obligation remains. |
+| Shard-less pass's config, clock, pressure and cap checks | Entirely in the enabled-only `ExLoopT<true>::split_read_local_pass` (`src/core/rl2s.cc:46`). No callback to it is installed off. |
+| `OwnsShards` config-refresh selection | `src/core/ex_loop.h:1913` defaults to **true** on all old calls. `if constexpr (OwnsShards)` adds no runtime test. Only `src/core/rl2s.cc:49` calls `<false>`. Expanding the true block gives the baseline owner-refresh token sequence. |
+| Changed shared `Server::read_local_enabled` predicate | The ordinary parser's `Fused && ...` at `src/core/io_loop.h:3924` short-circuits at compile time. In common IO config refresh, the existing version-change return at `:7135` still precedes the predicate at `:7146`; changing the predicate adds no check to unchanged-version rotations. Other shared uses are boot/config preparation and INFO/RESETSTAT, not the GET/SET dispatch/execution loops. |
+| New INFO caller guard | `src/cmd/t_server.cc:1998` is inside the INFO SERVER command section (`:1975`), not any general operation or worker loop. All new per-shard/per-thread reporting loops are behind it in the cold helper. |
+
+The parser/admission/dispatch function at `src/core/io_loop.h:3918` is
+unchanged. In particular, `if constexpr (Fused)` and the enabled/pressure
+tests at `:3992` still precede `read_local_lane_quota(active_.size())` at
+`:3995`. Off evaluates **neither** the quota call **nor** `active_.size()`;
+there is no new cold-cache-line load disguised as a no-op callee.
+
+Source comparisons discarded the `if constexpr (Fused)` statements in
+both revisions of IO/EX rotation bodies; the remaining token sequences
+match. The initializers, ordinary split dispatch entry points, parser,
+owner task drain and IO config-refresh bodies also match baseline tokens.
+These comparisons check the source specialization, not compiler output.
+
+### 4. Per-operation layouts: unchanged off; optional growth disclosed
+
+| Type | Baseline bytes | Current bytes | Existing layout assert |
+| --- | ---: | ---: | --- |
+| `Op` | 336 | 336 | `src/exec/op.h:463` |
+| `Client` | 1984 | 1984 | `src/net/conn.h:905` |
+| `ThreadCtx` | 1408 | 1408 | `src/core/thread.h:1208` |
+| `Shard` | 1440 | 1440 | `src/core/shard.h:523` |
+| `FlatStore` | 944 | 944 | `src/store/flatstore.h:3912` |
+| `Rob<64>` | 192 | 192 | `src/net/rob.h:1098` |
+| `AtomicEntry` | 144 | 144 | `src/store/atomic_mvcc.h:70` |
+| `Config` | 624 | 624 | `src/core/config.h:429` |
+| Ordinary `ExLoop` | 6112 | 6112 | `src/core/ex_loop.h:3450` |
+
+No data members were added/reordered in `IoLoop`, `Server`, `ThreadCtx`
+or `ExLoopT`: their member declarations match baseline, including the
+existing optional pointer in `ThreadCtx`'s tail (`src/core/thread.h:1205`)
+and the empty `ReadLocalExState<false>` (`src/core/ex_loop.h:103`, `:165`)
+held with `[[no_unique_address]]` (`:3436`). The other locked type files
+are byte-identical; the Config struct's only change is a field comment. Thus no
+off-arm object has a new allocation stride, field offset or cache line.
+IoLoop and Server have no numeric whole-size assert here; their unchanged
+member/type layouts, rather than an invented size, are the evidence.
+
+**One on-arm sidecar did grow:** production `ReadLocalThreadState`
+**360 -> 368 bytes**. Baseline has an 8-byte tick, a 24-byte sink
+(`src/store/read_local_reclaim.h:61`) and 328-byte stats (38 `uint64_t`
+counters plus three arm counters: `src/core/thread.h:137`,
+`src/net/rob.h:62`). The added `atomic<bool> lane_active` follows them
+(`src/core/thread.h:303`); alignment adds seven padding bytes. The old
+prefix offset is now asserted as 360 at `:306`, total size 368 at `:308`.
+The diagnostics-only selector 3 retains its existing separate alignment
+and is excluded from these production asserts. It too uses only optional
+state. **Off pays zero bytes and no cache line for this growth**, because
+`Server::init` never calls `ThreadCtx::init_read_local_state` off. Existing
+hot fields remain at their old offsets on both arms.
+
+After the INFO fix, all four off-arm source properties above hold. Static
+verification completed: baseline source/token comparisons, Python AST
+parsing of the four changed batteries (without importing/running them),
+and `git diff HEAD --check`. The maintainer still owns build/layout-assert
+execution, both-mode boots, the gate and the PRE/POST rates. No gate row
+was added or retired; quick/full expectations remain **327/344**. The
+quick-tier branch starts at `tests/gate.sh:1258` and exits at `:1260`; the
+standalone RL2S battery has no row on either side of it.
+
 ## MEASUREMENT PLAN
 
 The maintainer performs all steps below, on the quiet box. No performance
@@ -221,7 +390,9 @@
    row automatically.
 2. Boot the four overlap-0 combinations: 2s/0, 2s/1, 1s/0, 1s/1. On eight
    real cores use 6:2 for split; omit ratio for 1s. Require INFO's effective
-   lane bit and active-reader count to match: 0, 6, 0, 8. Also smoke 2s/1
+   lane bit to be 0, 1, 0, 1. Enabled active-reader counts must be 6 and 8;
+   off arms must omit the new active-count/per-thread fields and retain
+   their baseline INFO payload. Also smoke 2s/1
    with one shard and 6:2 (one EX has no shard), explicit placement, and
    the existing TCP/TLS/unix and epoll/uring boot/shutdown cases. Confirm
    split/1/overlap-1 fails with the documented message.
diff --git a/src/cmd/t_server.cc b/src/cmd/t_server.cc
index 2d5c81bfdb1c5ee1dcedddf26af8e1801a2ea4d3..7ad03034a03f1c5a2918a29c761c28fa54bdcb96
--- a/src/cmd/t_server.cc
+++ b/src/cmd/t_server.cc
@@ -14,6 +14,7 @@
 #include "server_tail.h"
 #include "slowlog.h"
 #include "../base/alloc.h"
+#include "../core/genthread.h"
 #include "../core/server.h"
 #include "../core/lbsignals.h"
 #include "../core/pubsub_event.h"
@@ -1992,33 +1993,10 @@
                 static_cast<unsigned>(g_server ? g_server->cfg().port : 0),
                 static_cast<unsigned long long>(uptime),
                 static_cast<unsigned long long>(uptime / 86400));
-        uint32_t active_readers = 0;
-        if (g_server) {
-            uint32_t owned[kMaxThreads] = {};
-            // Read the published ownership directory, never a vector FLIP may be rewriting.
-            for (uint32_t sid = 0; sid < g_server->nshards(); sid++)
-                owned[g_server->worker_of_shard(static_cast<int32_t>(sid))]++;
-            for (uint32_t tid = 0; tid < g_server->nthreads(); tid++) {
-                const ThreadCtx& thread = g_server->thread(tid);
-                const bool active = thread.read_local_lane_active();
-                active_readers += active;
-                const Role role = thread.role();
-                const char* name = role == Role::Idle ? "idle"
-                    : g_server->thread_mode() == ThreadMode::Fused ? "unified"
-                    : role == Role::Ifid ? "ifid" : "ex";
-                const ReadLocalStats* stats = g_server->read_local_enabled()
-                    ? &thread.read_local_stats() : nullptr;
-                // These per-thread totals are lifetime counters; INFO stats retains its existing
-                // RESETSTAT-relative aggregate counters. FLIP does not reset either sidecar.
-                appendf(body,
-                        "read_local_thread_%u:role=%s,shards=%u,active=%u,"
-                        "hits_total=%llu,mget_hits_total=%llu\r\n",
-                        tid, name, owned[tid], active ? 1u : 0u,
-                        static_cast<unsigned long long>(stats ? stats->hits : 0),
-                        static_cast<unsigned long long>(stats ? stats->mget_local_hits : 0));
-            }
-        }
-        appendf(body, "read_local_active_threads:%u\r\n", active_readers);
+        // Guard the CALL, including all argument evaluation. The cold non-inlined helper owns
+        // its scratch buffers so the disabled INFO path keeps its old output without those arrays.
+        if (g_server && g_server->read_local_enabled())
+            append_read_local_thread_info(body, *g_server);
         if (g_server && g_server->thread_mode() == ThreadMode::Fused) {
             appendf(body,
                     "fused_threads:%u\r\nclient_threads:%u\r\nowner_threads:%u\r\n"
diff --git a/src/core/genthread.h b/src/core/genthread.h
index c47b788f303c3a7e7f482d5ddc9f5a580a0a9bbb..e874192a1f6c2412955de097a7cd14587582ee0f
--- a/src/core/genthread.h
+++ b/src/core/genthread.h
@@ -3,6 +3,7 @@
 #pragma once
 
 #include <memory>
+#include <string>
 #include <vector>
 
 namespace tomo {
@@ -27,4 +28,7 @@
                      LateUnixListener& unix_listener,
                      ShutdownReportFinalLine& final_report);
 
+// INFO-only diagnostics. The caller must test read_local_enabled() before this call.
+void append_read_local_thread_info(std::string& body, const Server& server);
+
 }  // namespace tomo
diff --git a/src/core/rl2s.cc b/src/core/rl2s.cc
index d942a2e0f0f948a54a3f9becd0e2e32efb231a98..274b33481ca4d2e1e9e9b06d9d4dc9da575090e8
--- a/src/core/rl2s.cc
+++ b/src/core/rl2s.cc
@@ -305,4 +305,40 @@
     return 0;
 }
 
+// Keep the scratch arrays out of cmd_info's frame, including with LTO. Only an armed INFO SERVER
+// request reaches this helper; the boot/runtime paths install no reporting callback or sidecar.
+__attribute__((noinline, cold))
+void append_read_local_thread_info(std::string& body, const Server& srv) {
+    uint32_t active_readers = 0;
+    uint32_t owned[kMaxThreads] = {};
+    // Read the published ownership directory, never a vector FLIP may be rewriting.
+    for (uint32_t sid = 0; sid < srv.nshards(); sid++)
+        owned[srv.worker_of_shard(static_cast<int32_t>(sid))]++;
+    char row[192];  // fixed grammar, including full-width uint32/uint64 counters
+    for (uint32_t tid = 0; tid < srv.nthreads(); tid++) {
+        const ThreadCtx& thread = srv.thread(tid);
+        const bool active = thread.read_local_lane_active();
+        active_readers += active;
+        const Role role = thread.role();
+        const char* name = role == Role::Idle ? "idle"
+            : srv.thread_mode() == ThreadMode::Fused ? "unified"
+            : role == Role::Ifid ? "ifid" : "ex";
+        const ReadLocalStats& stats = thread.read_local_stats();
+        // Lifetime counters survive FLIP and RESETSTAT; INFO stats retains its existing
+        // RESETSTAT-relative aggregate counters.
+        const int n = std::snprintf(row, sizeof(row),
+            "read_local_thread_%u:role=%s,shards=%u,active=%u,"
+            "hits_total=%llu,mget_hits_total=%llu\r\n",
+            tid, name, owned[tid], active ? 1u : 0u,
+            static_cast<unsigned long long>(stats.hits),
+            static_cast<unsigned long long>(stats.mget_local_hits));
+        if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+        body.append(row, static_cast<size_t>(n));
+    }
+    const int n = std::snprintf(row, sizeof(row), "read_local_active_threads:%u\r\n",
+                              active_readers);
+    if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+    body.append(row, static_cast<size_t>(n));
+}
+
 }  // namespace tomo
diff --git a/src/core/thread.h b/src/core/thread.h
index adea9605d0f0d40c3566e07b90a2663accb6b835..61802252934cbd450279ad7cd2dbaa8d5f03f465
--- a/src/core/thread.h
+++ b/src/core/thread.h
@@ -303,6 +303,8 @@
     std::atomic<bool> lane_active{false};
 };
 #if TOMO_READ_LOCAL_SET_TAX_VARIANT != 3
+static_assert(offsetof(ReadLocalThreadState, lane_active) == 360,
+              "lane activation must follow the entire pre-RL2S sidecar");
 static_assert(sizeof(ReadLocalThreadState) == 368,
               "lane activation adds one alignment word only to the optional sidecar");
 #endif

exec
/bin/bash -lc "git diff HEAD --check
python3 - <<'PY'
from pathlib import Path
import re
s = Path('RL2S.md').read_text().split('## OFF-ARM PROOF',1)[1].split('## MEASUREMENT PLAN',1)[0]
refs = sorted(set(re.findall(r'(src/[a-zA-Z0-9_./]+|tests/gate\\.sh):(\\d+)', s)))
for f, n in refs:
    line = Path(f).read_text().splitlines()[int(n)-1]
    print(f'{f}:{n}: {line.strip()[:90]}')
PY" in /home/user/Projects/cx-rl2s
 succeeded in 0ms:
src/cmd/t_server.cc:1998: if (g_server && g_server->read_local_enabled())
src/cmd/t_server.cc:322: g_config.push_back({"thread-mode", ConfigKind::Enum,
src/core/config.h:1215: if (cfg.thread_mode == ThreadMode::Split && cfg.read_local && cfg.overlap != 0) {
src/core/config.h:225: enum class ThreadMode : uint8_t { Split = 0, Fused = 1 };
src/core/config.h:429: static_assert(sizeof(Config) == 624,
src/core/ex_loop.h:103: struct ReadLocalExState<false> {};
src/core/ex_loop.h:1913: template <bool OwnsShards = true>
src/core/ex_loop.h:196: if constexpr (Fused) {
src/core/ex_loop.h:204: !impl->deferred.init(srv, self)) return false;
src/core/ex_loop.h:213: if (!ring_.init(1024)) return false;
src/core/ex_loop.h:241: if (read_local_enabled())
src/core/ex_loop.h:3439: using ExLoop = ExLoopT<false>;
src/core/ex_loop.h:3450: static_assert(sizeof(ExLoop) == 6112);
src/core/ex_loop.h:397: if (srv_->thread_mode() == ThreadMode::Split) return split_read_local_pass();
src/core/ex_loop.h:708: if constexpr (Fused) {
src/core/ex_loop.h:990: char* frame = sink.reserve(24 + static_cast<size_t>(length) + 2);
src/core/fused_boot_gate.h:23: explicit FusedBootGate(uint32_t workers)
src/core/io_loop.h:102: if (!ring_.init(4096)) return false;
src/core/io_loop.h:3918: DispatchResult parse_and_dispatch(
src/core/io_loop.h:3924: [[maybe_unused]] const bool read_local_enabled =
src/core/io_loop.h:4200: if constexpr (Fused) {
src/core/io_loop.h:541: if constexpr (Fused) {
src/core/io_loop.h:6853: if constexpr (Fused) {
src/core/placement.h:178: bool assign_shard_homes(uint32_t nshards, const char* spec) {
src/core/read_local.h:260: entries_.reset(new (std::nothrow) Entry[kCapacity]);
src/core/rl2s.cc:127: bool ok = self.init_task_inbox_local(srv.placement().ifid_threads(),
src/core/rl2s.cc:130: std::string local_error;
src/core/rl2s.cc:147: self.bind_io_role_hooks(
src/core/rl2s.cc:310: __attribute__((noinline, cold))
src/core/rl2s.cc:313: uint32_t owned[kMaxThreads] = {};
src/core/rl2s.cc:46: uint32_t ExLoopT<true>::split_read_local_pass() {
src/core/rl2s.cc:49: refresh_live_config<false>();
src/core/rl2s.cc:77: if (cfg.thread_mode != ThreadMode::Split || !srv.read_local_enabled()) std::abort();
src/core/rl2s.cc:90: std::vector<std::thread> pool;
src/core/rl2s.cc:92: std::vector<FusedExLoop> executors(nthreads);
src/core/rl2s.cc:95: FusedBootGate boot(nthreads);
src/core/server.h:1951: if (!shard.store().read_local_enabled()) return;
src/core/server.h:239: if (cfg.thread_mode == ThreadMode::Fused) {
src/core/server.h:316: if (read_local_enabled()) {
src/core/server.h:322: for (const auto& thread : threads_) {
src/core/server.h:328: for (const auto& shard : shards_) {
src/core/server.h:367: for (uint32_t sid = 0; sid < cfg.shards; sid++) {
src/core/server.h:582: static bool read_local_enabled(const Config& cfg) {
src/core/server.h:606: uint64_t read_local_grace_floor(uint64_t stamp, uint32_t& hint) const {
src/core/shard.h:523: static_assert(sizeof(Shard) == 1440);
src/core/thread.h:1205: std::unique_ptr<ReadLocalThreadState> read_local_state_;
src/core/thread.h:1208: static_assert(sizeof(ThreadCtx) == 1408,
src/core/thread.h:137: struct ReadLocalStats {
src/core/thread.h:303: std::atomic<bool> lane_active{false};
src/core/thread.h:306: static_assert(offsetof(ReadLocalThreadState, lane_active) == 360,
src/core/thread.h:338: }
src/core/thread.h:353: bool init_read_local_state() {
src/exec/op.h:463: static_assert(sizeof(Op) == 336, "Op grew: pay for it inside the struct or re-run the 64c 
src/main.cc:183: if (cfg.read_local && cfg.overlap != 0)
src/main.cc:312: if (cfg.thread_mode == ThreadMode::Fused) {
src/main.cc:317: if (srv.read_local_enabled()) {
src/main.cc:336: // Placement decides every cpu directly. Pinning is relative to the process's ALLOWED set 
src/main.cc:338: const uint32_t nthreads = srv.nthreads();
src/main.cc:372: for (uint32_t tid : srv.placement().ex_threads())
src/main.cc:395: ok = ios[tid].init(&srv, &self, cfg.bind_addr, cfg.port, -1,
src/net/conn.h:905: static_assert(sizeof(Client) == 1984, "Client grew: re-check the io-hot line packing and i
src/net/rob.h:1098: static_assert(sizeof(Rob<64>) == 192, "Rob<64> layout changed");
src/net/rob.h:486: if (__builtin_expect(read_local_state_ == 0, false) && !prepare_read_local()) {
src/net/rob.h:62: struct ReadLocalArmStats {
src/store/atomic_mvcc.h:70: static_assert(sizeof(AtomicEntry) == 144);
src/store/flatstore.h:3912: static_assert(sizeof(FlatStore) == 944);
src/store/flatstore_atomic.inc:890: bool ensure_read_local_store_state() {
src/store/read_local_reclaim.h:61: static_assert(sizeof(ReadLocalRetireSink) == 3 * sizeof(void*),
tests/gate.sh:1258: if [ "$TIER" = quick ]; then

diff --git a/RL2S.md b/RL2S.md
index 64dea226f85e5b4b296ed8c9f1de21ae463a8cf0..c8cded74608febc9b1f04ba2c1c1a9c2aff81f37
--- a/RL2S.md
+++ b/RL2S.md
@@ -76,7 +76,8 @@
 guards evaluation of `active_.size()`; no admission expression moved ahead
 of its old check.
 
-All physical threads provision an `IoLoop` and `FusedExLoop`. They use the
+On an enabled `2s/1` boot, all physical threads provision an `IoLoop` and
+`FusedExLoop`. They use the
 ordinary split task inbox layout and producer masks, including FLIP's
 existing remasking. Initial persistence loading runs before store arming.
 Each executor's `activate_fused` binds its permanent ThreadCtx sink, then
@@ -136,7 +137,8 @@
 1984, ThreadCtx 1408, Shard 1440, FlatStore 944, Rob<64> 192, AtomicEntry 144,
 Config 624; the ordinary ExLoop assert remains 6112. The only data layout
 change is the optional `ReadLocalThreadState`: 360 -> 368 bytes in the
-production build, asserted at `src/core/thread.h:306`. An atomic loop-entry
+production build, asserted at `src/core/thread.h:308`; its old 360-byte
+prefix is also locked at `src/core/thread.h:306`. An atomic loop-entry
 flag plus alignment occupies the extra word, after all existing members.
 The diagnostics-only selector-3 sidecar already has different alignment
 and is excluded from this production-size assert. Read-local 0 allocates
@@ -148,7 +150,8 @@
 ## Observable activation
 
 `INFO server` still reports `thread_mode:2s`, live `io_threads` and
-`ex_threads`, and effective `read_local:1`. It now also reports:
+`ex_threads`, and effective `read_local:1`. When the effective lane is
+enabled, it also reports:
 
 ```text
 read_local_thread_0:role=ifid,shards=0,active=1,hits_total=123,mget_hits_total=7
@@ -161,10 +164,15 @@
 network waits; it is not inferred from the knob or ownership. The atomic
 flag is written only at loop entry/exit. Shard counts come from published
 owner IDs, not concurrently modified ownership vectors
-(`src/cmd/t_server.cc:1995`). After FLIP settles, every IFID row must have
+(`src/core/rl2s.cc:313`). After FLIP settles, every IFID row must have
 shards=0/active=1, and every EX row active=0. As with other INFO geometry,
 the individual samples are not one transactional view during a move.
 
+With read-local disabled, both new field families are absent, preserving
+the baseline INFO payload. Their caller guard is at
+`src/cmd/t_server.cc:1998`; their scratch arrays and formatting live in the
+non-inlined helper at `src/core/rl2s.cc:310`.
+
 Per-thread `*_total` values are lifetime completions and survive FLIP and
 CONFIG RESETSTAT. They can be nonzero on an EX row after that thread has
 previously served IO. Aggregate `INFO stats` keeps its existing RESETSTAT
@@ -203,6 +211,167 @@
 row on either side of that exit. Updating existing battery internals does
 not change gate row counts.
 
+## OFF-ARM PROOF
+
+Compared against **`78c3e5391`**, for **`--thread-mode 2s --read-local 0`**,
+including its supported overlap 0 and 1 schedules. References below name
+the final working-tree sources unless explicitly marked baseline. This is
+a source and construction proof; no build, server, benchmark or gate was
+run. It does not establish identical generated instructions or measured
+PRE/POST rates.
+
+**Finding and fix.** The submitted diff did not preserve disabled INFO:
+it counted shard owners, visited every thread and formatted
+`read_local_thread_*` and `read_local_active_threads` on off boots too.
+That changed replies and could grow INFO's output allocation. Its
+`owned[kMaxThreads]` array also lived in the common `cmd_info` frame.
+The fix guards the **call** with the effective predicate at
+`src/cmd/t_server.cc:1998`, and moves the entire walk, scratch arrays and
+formatting into `append_read_local_thread_info` in `src/core/rl2s.cc:310`.
+`noinline` keeps those arrays out of the caller, including under LTO.
+The helper receives only references, after the guard; it does not take
+an eagerly computed topology snapshot or allocate before early-returning.
+Disabled INFO emits neither new field family. Enabled INFO keeps both.
+The offset assert at `src/core/thread.h:306` additionally locks the old
+sidecar prefix; it adds no runtime state. The lane implementation is kept.
+
+### 1. Threads, capabilities and roles: unchanged off
+
+The effective predicate is `cfg.overlap == 0 && cfg.read_local != 0`
+(`src/core/server.h:582`). It is false when the knob is zero. The only two
+thread modes are Split and Fused (`src/core/config.h:225`): `main` returns
+through the existing fused path at `src/main.cc:312`, then tests the
+predicate at `:317`. Thus the **new runtime** can be called only for
+Split **and** read-local 1, with overlap 0. Its entry also checks this
+contract before construction (`src/core/rl2s.cc:77`). Mode, overlap and
+read-local remain boot-only CONFIG entries (`src/cmd/t_server.cc:322`).
+
+Off falls through to the old `vector<IoLoop>` and `vector<ExLoop>` at
+`src/main.cc:338`; `ExLoop` still means **`ExLoopT<false>`**
+(`src/core/ex_loop.h:3439`). It does not construct the new
+`vector<FusedExLoop>` at `src/core/rl2s.cc:92`, bind a fused executor,
+call `activate_fused`, or enter `run_fused`.
+
+Placement still builds the ordinary split ratio/explicit placement
+(`src/core/server.h:239`), creates `placement_.total_threads()` contexts
+(`:294`, `:305`), and assigns shard homes to EX tids
+(`src/core/placement.h:178`, `src/core/server.h:367`). Actual OS workers
+are created once per EX tid at `src/main.cc:372` and once per IFID tid at
+`:507`. At gate geometry this is **8 workers: 6 IFID, 2 EX, 16 shards on
+EX**, not an additional reader tier. Main remains the existing monitor.
+
+Baseline already provisioned the opposite dormant loop on every worker
+for FLIP: dormant IO on EX at `src/main.cc:395`, dormant ordinary EX on IO
+at `:528`. Those allocations/capabilities are not new. Off role changes
+still call only `ios[tid].run()` / `exs[tid].run()` (`:426`, `:562`), so
+FLIP cannot later upgrade an off worker to the reader runtime. The entire
+ordinary split construction/role-loop/shutdown suffix starting at
+`src/main.cc:336` was compared byte-for-byte with baseline and is identical.
+
+### 2. Allocations, buffers, sinks and registrations: none added off
+
+The inventory includes existing allocations newly reachable by the armed
+split runtime, not just literal `new` lines in `rl2s.cc`.
+
+| Construction or allocation | Guard and off-arm result |
+| --- | --- |
+| Server read-local sidecar (`new ReadLocalServerState`) | Existing predicate guard at `src/core/server.h:316`, allocation `:317`. False off; the existing owning pointer stays null. |
+| Per-thread sidecars, tick publications, stats and sink slots | Same guard encloses the thread loop at `src/core/server.h:322`; `src/core/thread.h:353` performs `new ReadLocalThreadState`. Never called off, including for future FLIP readers/owners. |
+| Per-shard extended store state and foreign-read safety arrays | Same guard encloses `prepare_read_local` at `src/core/server.h:328`. It reaches `new ReadLocalStoreState` at `src/store/flatstore_atomic.inc:890`. Off stores retain the original ordinary atomic-state allocation path. |
+| `ReadLocalExImpl`, `LaneEntry[kInboxSlots]`, fallback array | `src/core/ex_loop.h:196` first requires compile-time `Fused`, then the effective predicate at `:197`; the three `new` sites are `:198`, `:200`, `:202`. The entire block is discarded for the off `ExLoopT<false>`. |
+| Deferred `Entry[kCapacity]` buffer, block-cache heads and retire sink | Deferred initialization is inside that same block (`src/core/ex_loop.h:204`); buffer allocation is `src/core/read_local.h:260`, sink binding `:262`. Cache heads and retire-ring bookkeeping are members of the optional queue (`:464`), not a baseline per-thread allocation. No off queue exists to grow debug pending sets or retain blocks. |
+| Runtime vectors, default-constructed loop containers and worker closures | `pool`, `ios(nthreads)`, `executors(nthreads)` at `src/core/rl2s.cc:90`, then `pool.emplace_back` at `:122`, are inside the guarded runtime call. This includes allocations by embedded container constructors, such as executor deques, before `init`. The off runtime constructs only the baseline vectors/loops described above. |
+| Boot-gate worker-state vector, loading/error strings and shutdown report buffers | `FusedBootGate boot(nthreads)` at `src/core/rl2s.cc:95` allocates its vector in `src/core/fused_boot_gate.h:23`. The new runtime's local strings (`src/core/rl2s.cc:130`, `:235`, `:263`, `:281`) and report (`:115`) share the same call guard. No static boot gate or report is constructed for off. |
+| Inbox channels, IO/EX rings, network registration and listener resources | All new-runtime calls are below its guard: `src/core/rl2s.cc:127`, `:129`, `:144`, `:196`, `:248`, `:264`, `:270`. Inbox storage comes from `src/core/thread.h:338`; rings from `src/core/ex_loop.h:213` and `src/core/io_loop.h:102`. Off calls the unchanged baseline initializers with the same parameters; it does not additionally run any of these RL2S initializers. |
+| Role/client registration, capacity, completion, demotion and progress hooks | `src/core/rl2s.cc:147`, `:152`, `:160`, `:165`, `:167`, `:172`, `:181` are inside the enabled runtime. Fused sink publication and store arming occur in `src/core/ex_loop.h:241` and `:245`. Off never installs those fused hooks; its pre-existing ordinary IO registration hooks remain. |
+| Connection RYOW sidecar and local-read reply reserves | Parser admission remains behind compile-time `Fused` and the effective predicate (`src/core/io_loop.h:4200`). Its write marking at `:4253` can reach `Rob::prepare_read_local` (`src/net/rob.h:486`, `:709`) only in that armed path. Local-copy reply reservation (`src/core/ex_loop.h:990`) likewise belongs to the existing reader lane. No new accept-time buffer or sidecar allocation was added. |
+| New INFO ownership array, row buffer and output growth | Fixed: caller guard at `src/cmd/t_server.cc:1998` precedes helper entry; `owned` and `row` exist only in `src/core/rl2s.cc:313`, `:317`. Both `body.append` sites (`:336`, `:341`) are inside that helper. Off emits the baseline bytes and takes none of these buffer allocations. |
+
+There is no new `.reserve()`/`.resize()` call in the feature file and no
+file-scope runtime object or thread-local registration. Its project header
+dependencies were already included by main/genthread. All feature-owned
+vectors, strings, arrays and OS-thread creation are local to the two
+guarded entry points. Existing optional sidecars store the hooks; no
+separate always-present registry was introduced.
+
+QSBR registration is the optional per-thread state allocated at boot,
+not mere membership in the baseline `threads_` vector. Its floor scanner
+(`src/core/server.h:606`) is driven by the optional deferred queues.
+Neither the scanner, a tick/park publication, nor a sink binding is added
+to an off thread. Existing store ownership-edge checks remain unchanged
+(`src/core/server.h:1951`) and return for unarmed stores.
+
+### 3. Hot loops and eager arguments: no added off-arm branch
+
+| Changed test or call | Why off does not execute it |
+| --- | --- |
+| Runtime selection and overlap validation | `src/main.cc:317` and `src/core/config.h:1215` are boot-only, before operation processing. The changed startup notice at `src/main.cc:183` is also cold and silent with read-local 0. |
+| IO reader-entry resume/epoch/active publication and exit publication | Both are inside `if constexpr (Fused)` at `src/core/io_loop.h:541` and `:766`. Ordinary `run()` selects `run_split<0/1>` (`:429`); every network/TLS/unix variant passes `Fused=false` (`:407`). The new calls and **all their arguments**, including `read_local_epoch()`, are discarded. Entry/exit are tenure boundaries even on the enabled arm. |
+| EX parked/lane-count check, config invalidation, debug ownership check and retire drain | All additions to `ExLoopT::run` are inside `if constexpr (Fused)` (`src/core/ex_loop.h:708`, `:723`, `:800`). Off instantiates `ExLoopT<false>`; it does not evaluate `read_local_impl()`, `self_->id()` for the new check, or epoch/sink arguments. This also holds with `TOMO_RL_CACHE_DEBUG`. |
+| New mode tests in baseline fused pass/sweep | `src/core/ex_loop.h:397` and `:640` live in `static_assert(Fused)` entry points. Their IO call sites are themselves under `if constexpr (Fused)` at `src/core/io_loop.h:6853`. Off never calls them. They do add a rotation-boundary test to the existing **1s** schedule; the 1s PRE/POST measurement obligation remains. |
+| Shard-less pass's config, clock, pressure and cap checks | Entirely in the enabled-only `ExLoopT<true>::split_read_local_pass` (`src/core/rl2s.cc:46`). No callback to it is installed off. |
+| `OwnsShards` config-refresh selection | `src/core/ex_loop.h:1913` defaults to **true** on all old calls. `if constexpr (OwnsShards)` adds no runtime test. Only `src/core/rl2s.cc:49` calls `<false>`. Expanding the true block gives the baseline owner-refresh token sequence. |
+| Changed shared `Server::read_local_enabled` predicate | The ordinary parser's `Fused && ...` at `src/core/io_loop.h:3924` short-circuits at compile time. In common IO config refresh, the existing version-change return at `:7135` still precedes the predicate at `:7146`; changing the predicate adds no check to unchanged-version rotations. Other shared uses are boot/config preparation and INFO/RESETSTAT, not the GET/SET dispatch/execution loops. |
+| New INFO caller guard | `src/cmd/t_server.cc:1998` is inside the INFO SERVER command section (`:1975`), not any general operation or worker loop. All new per-shard/per-thread reporting loops are behind it in the cold helper. |
+
+The parser/admission/dispatch function at `src/core/io_loop.h:3918` is
+unchanged. In particular, `if constexpr (Fused)` and the enabled/pressure
+tests at `:3992` still precede `read_local_lane_quota(active_.size())` at
+`:3995`. Off evaluates **neither** the quota call **nor** `active_.size()`;
+there is no new cold-cache-line load disguised as a no-op callee.
+
+Source comparisons discarded the `if constexpr (Fused)` statements in
+both revisions of IO/EX rotation bodies; the remaining token sequences
+match. The initializers, ordinary split dispatch entry points, parser,
+owner task drain and IO config-refresh bodies also match baseline tokens.
+These comparisons check the source specialization, not compiler output.
+
+### 4. Per-operation layouts: unchanged off; optional growth disclosed
+
+| Type | Baseline bytes | Current bytes | Existing layout assert |
+| --- | ---: | ---: | --- |
+| `Op` | 336 | 336 | `src/exec/op.h:463` |
+| `Client` | 1984 | 1984 | `src/net/conn.h:905` |
+| `ThreadCtx` | 1408 | 1408 | `src/core/thread.h:1208` |
+| `Shard` | 1440 | 1440 | `src/core/shard.h:523` |
+| `FlatStore` | 944 | 944 | `src/store/flatstore.h:3912` |
+| `Rob<64>` | 192 | 192 | `src/net/rob.h:1098` |
+| `AtomicEntry` | 144 | 144 | `src/store/atomic_mvcc.h:70` |
+| `Config` | 624 | 624 | `src/core/config.h:429` |
+| Ordinary `ExLoop` | 6112 | 6112 | `src/core/ex_loop.h:3450` |
+
+No data members were added/reordered in `IoLoop`, `Server`, `ThreadCtx`
+or `ExLoopT`: their member declarations match baseline, including the
+existing optional pointer in `ThreadCtx`'s tail (`src/core/thread.h:1205`)
+and the empty `ReadLocalExState<false>` (`src/core/ex_loop.h:103`, `:165`)
+held with `[[no_unique_address]]` (`:3436`). The other locked type files
+are byte-identical; the Config struct's only change is a field comment. Thus no
+off-arm object has a new allocation stride, field offset or cache line.
+IoLoop and Server have no numeric whole-size assert here; their unchanged
+member/type layouts, rather than an invented size, are the evidence.
+
+**One on-arm sidecar did grow:** production `ReadLocalThreadState`
+**360 -> 368 bytes**. Baseline has an 8-byte tick, a 24-byte sink
+(`src/store/read_local_reclaim.h:61`) and 328-byte stats (38 `uint64_t`
+counters plus three arm counters: `src/core/thread.h:137`,
+`src/net/rob.h:62`). The added `atomic<bool> lane_active` follows them
+(`src/core/thread.h:303`); alignment adds seven padding bytes. The old
+prefix offset is now asserted as 360 at `:306`, total size 368 at `:308`.
+The diagnostics-only selector 3 retains its existing separate alignment
+and is excluded from these production asserts. It too uses only optional
+state. **Off pays zero bytes and no cache line for this growth**, because
+`Server::init` never calls `ThreadCtx::init_read_local_state` off. Existing
+hot fields remain at their old offsets on both arms.
+
+After the INFO fix, all four off-arm source properties above hold. Static
+verification completed: baseline source/token comparisons, Python AST
+parsing of the four changed batteries (without importing/running them),
+and `git diff HEAD --check`. The maintainer still owns build/layout-assert
+execution, both-mode boots, the gate and the PRE/POST rates. No gate row
+was added or retired; quick/full expectations remain **327/344**. The
+quick-tier branch starts at `tests/gate.sh:1258` and exits at `:1260`; the
+standalone RL2S battery has no row on either side of it.
+
 ## MEASUREMENT PLAN
 
 The maintainer performs all steps below, on the quiet box. No performance
@@ -221,7 +390,9 @@
    row automatically.
 2. Boot the four overlap-0 combinations: 2s/0, 2s/1, 1s/0, 1s/1. On eight
    real cores use 6:2 for split; omit ratio for 1s. Require INFO's effective
-   lane bit and active-reader count to match: 0, 6, 0, 8. Also smoke 2s/1
+   lane bit to be 0, 1, 0, 1. Enabled active-reader counts must be 6 and 8;
+   off arms must omit the new active-count/per-thread fields and retain
+   their baseline INFO payload. Also smoke 2s/1
    with one shard and 6:2 (one EX has no shard), explicit placement, and
    the existing TCP/TLS/unix and epoll/uring boot/shutdown cases. Confirm
    split/1/overlap-1 fails with the documented message.
diff --git a/src/cmd/t_server.cc b/src/cmd/t_server.cc
index 2d5c81bfdb1c5ee1dcedddf26af8e1801a2ea4d3..7ad03034a03f1c5a2918a29c761c28fa54bdcb96
--- a/src/cmd/t_server.cc
+++ b/src/cmd/t_server.cc
@@ -14,6 +14,7 @@
 #include "server_tail.h"
 #include "slowlog.h"
 #include "../base/alloc.h"
+#include "../core/genthread.h"
 #include "../core/server.h"
 #include "../core/lbsignals.h"
 #include "../core/pubsub_event.h"
@@ -1992,33 +1993,10 @@
                 static_cast<unsigned>(g_server ? g_server->cfg().port : 0),
                 static_cast<unsigned long long>(uptime),
                 static_cast<unsigned long long>(uptime / 86400));
-        uint32_t active_readers = 0;
-        if (g_server) {
-            uint32_t owned[kMaxThreads] = {};
-            // Read the published ownership directory, never a vector FLIP may be rewriting.
-            for (uint32_t sid = 0; sid < g_server->nshards(); sid++)
-                owned[g_server->worker_of_shard(static_cast<int32_t>(sid))]++;
-            for (uint32_t tid = 0; tid < g_server->nthreads(); tid++) {
-                const ThreadCtx& thread = g_server->thread(tid);
-                const bool active = thread.read_local_lane_active();
-                active_readers += active;
-                const Role role = thread.role();
-                const char* name = role == Role::Idle ? "idle"
-                    : g_server->thread_mode() == ThreadMode::Fused ? "unified"
-                    : role == Role::Ifid ? "ifid" : "ex";
-                const ReadLocalStats* stats = g_server->read_local_enabled()
-                    ? &thread.read_local_stats() : nullptr;
-                // These per-thread totals are lifetime counters; INFO stats retains its existing
-                // RESETSTAT-relative aggregate counters. FLIP does not reset either sidecar.
-                appendf(body,
-                        "read_local_thread_%u:role=%s,shards=%u,active=%u,"
-                        "hits_total=%llu,mget_hits_total=%llu\r\n",
-                        tid, name, owned[tid], active ? 1u : 0u,
-                        static_cast<unsigned long long>(stats ? stats->hits : 0),
-                        static_cast<unsigned long long>(stats ? stats->mget_local_hits : 0));
-            }
-        }
-        appendf(body, "read_local_active_threads:%u\r\n", active_readers);
+        // Guard the CALL, including all argument evaluation. The cold non-inlined helper owns
+        // its scratch buffers so the disabled INFO path keeps its old output without those arrays.
+        if (g_server && g_server->read_local_enabled())
+            append_read_local_thread_info(body, *g_server);
         if (g_server && g_server->thread_mode() == ThreadMode::Fused) {
             appendf(body,
                     "fused_threads:%u\r\nclient_threads:%u\r\nowner_threads:%u\r\n"
diff --git a/src/core/genthread.h b/src/core/genthread.h
index c47b788f303c3a7e7f482d5ddc9f5a580a0a9bbb..e874192a1f6c2412955de097a7cd14587582ee0f
--- a/src/core/genthread.h
+++ b/src/core/genthread.h
@@ -3,6 +3,7 @@
 #pragma once
 
 #include <memory>
+#include <string>
 #include <vector>
 
 namespace tomo {
@@ -27,4 +28,7 @@
                      LateUnixListener& unix_listener,
                      ShutdownReportFinalLine& final_report);
 
+// INFO-only diagnostics. The caller must test read_local_enabled() before this call.
+void append_read_local_thread_info(std::string& body, const Server& server);
+
 }  // namespace tomo
diff --git a/src/core/rl2s.cc b/src/core/rl2s.cc
index d942a2e0f0f948a54a3f9becd0e2e32efb231a98..274b33481ca4d2e1e9e9b06d9d4dc9da575090e8
--- a/src/core/rl2s.cc
+++ b/src/core/rl2s.cc
@@ -305,4 +305,40 @@
     return 0;
 }
 
+// Keep the scratch arrays out of cmd_info's frame, including with LTO. Only an armed INFO SERVER
+// request reaches this helper; the boot/runtime paths install no reporting callback or sidecar.
+__attribute__((noinline, cold))
+void append_read_local_thread_info(std::string& body, const Server& srv) {
+    uint32_t active_readers = 0;
+    uint32_t owned[kMaxThreads] = {};
+    // Read the published ownership directory, never a vector FLIP may be rewriting.
+    for (uint32_t sid = 0; sid < srv.nshards(); sid++)
+        owned[srv.worker_of_shard(static_cast<int32_t>(sid))]++;
+    char row[192];  // fixed grammar, including full-width uint32/uint64 counters
+    for (uint32_t tid = 0; tid < srv.nthreads(); tid++) {
+        const ThreadCtx& thread = srv.thread(tid);
+        const bool active = thread.read_local_lane_active();
+        active_readers += active;
+        const Role role = thread.role();
+        const char* name = role == Role::Idle ? "idle"
+            : srv.thread_mode() == ThreadMode::Fused ? "unified"
+            : role == Role::Ifid ? "ifid" : "ex";
+        const ReadLocalStats& stats = thread.read_local_stats();
+        // Lifetime counters survive FLIP and RESETSTAT; INFO stats retains its existing
+        // RESETSTAT-relative aggregate counters.
+        const int n = std::snprintf(row, sizeof(row),
+            "read_local_thread_%u:role=%s,shards=%u,active=%u,"
+            "hits_total=%llu,mget_hits_total=%llu\r\n",
+            tid, name, owned[tid], active ? 1u : 0u,
+            static_cast<unsigned long long>(stats.hits),
+            static_cast<unsigned long long>(stats.mget_local_hits));
+        if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+        body.append(row, static_cast<size_t>(n));
+    }
+    const int n = std::snprintf(row, sizeof(row), "read_local_active_threads:%u\r\n",
+                              active_readers);
+    if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+    body.append(row, static_cast<size_t>(n));
+}
+
 }  // namespace tomo
diff --git a/src/core/thread.h b/src/core/thread.h
index adea9605d0f0d40c3566e07b90a2663accb6b835..61802252934cbd450279ad7cd2dbaa8d5f03f465
--- a/src/core/thread.h
+++ b/src/core/thread.h
@@ -303,6 +303,8 @@
     std::atomic<bool> lane_active{false};
 };
 #if TOMO_READ_LOCAL_SET_TAX_VARIANT != 3
+static_assert(offsetof(ReadLocalThreadState, lane_active) == 360,
+              "lane activation must follow the entire pre-RL2S sidecar");
 static_assert(sizeof(ReadLocalThreadState) == 368,
               "lane activation adds one alignment word only to the optional sidecar");
 #endif

codex
The proof is written. Source comparisons confirm that the ordinary split runtime is unchanged and that the IO/EX loop bodies match baseline after removing the compile-time fused-only blocks. The allocation inventory, layout checks, and eager-argument review are documented. No build or runtime command was run.
apply patch
patch: completed
/home/user/Projects/cx-rl2s/RL2S.md
diff --git a/RL2S.md b/RL2S.md
index 64dea226f85e5b4b296ed8c9f1de21ae463a8cf0..0101d325da31dbe431bf65c8d205915d53e7847f
--- a/RL2S.md
+++ b/RL2S.md
@@ -76,7 +76,8 @@
 guards evaluation of `active_.size()`; no admission expression moved ahead
 of its old check.
 
-All physical threads provision an `IoLoop` and `FusedExLoop`. They use the
+On an enabled `2s/1` boot, all physical threads provision an `IoLoop` and
+`FusedExLoop`. They use the
 ordinary split task inbox layout and producer masks, including FLIP's
 existing remasking. Initial persistence loading runs before store arming.
 Each executor's `activate_fused` binds its permanent ThreadCtx sink, then
@@ -136,7 +137,8 @@
 1984, ThreadCtx 1408, Shard 1440, FlatStore 944, Rob<64> 192, AtomicEntry 144,
 Config 624; the ordinary ExLoop assert remains 6112. The only data layout
 change is the optional `ReadLocalThreadState`: 360 -> 368 bytes in the
-production build, asserted at `src/core/thread.h:306`. An atomic loop-entry
+production build, asserted at `src/core/thread.h:308`; its old 360-byte
+prefix is also locked at `src/core/thread.h:306`. An atomic loop-entry
 flag plus alignment occupies the extra word, after all existing members.
 The diagnostics-only selector-3 sidecar already has different alignment
 and is excluded from this production-size assert. Read-local 0 allocates
@@ -148,7 +150,8 @@
 ## Observable activation
 
 `INFO server` still reports `thread_mode:2s`, live `io_threads` and
-`ex_threads`, and effective `read_local:1`. It now also reports:
+`ex_threads`, and effective `read_local:1`. When the effective lane is
+enabled, it also reports:
 
 ```text
 read_local_thread_0:role=ifid,shards=0,active=1,hits_total=123,mget_hits_total=7
@@ -161,10 +164,15 @@
 network waits; it is not inferred from the knob or ownership. The atomic
 flag is written only at loop entry/exit. Shard counts come from published
 owner IDs, not concurrently modified ownership vectors
-(`src/cmd/t_server.cc:1995`). After FLIP settles, every IFID row must have
+(`src/core/rl2s.cc:313`). After FLIP settles, every IFID row must have
 shards=0/active=1, and every EX row active=0. As with other INFO geometry,
 the individual samples are not one transactional view during a move.
 
+With read-local disabled, both new field families are absent, preserving
+the baseline INFO payload. Their caller guard is at
+`src/cmd/t_server.cc:1998`; their scratch arrays and formatting live in the
+non-inlined helper at `src/core/rl2s.cc:310`.
+
 Per-thread `*_total` values are lifetime completions and survive FLIP and
 CONFIG RESETSTAT. They can be nonzero on an EX row after that thread has
 previously served IO. Aggregate `INFO stats` keeps its existing RESETSTAT
@@ -203,6 +211,167 @@
 row on either side of that exit. Updating existing battery internals does
 not change gate row counts.
 
+## OFF-ARM PROOF
+
+Compared against **`78c3e5391`**, for **`--thread-mode 2s --read-local 0`**,
+including its supported overlap 0 and 1 schedules. References below name
+the final working-tree sources unless explicitly marked baseline. This is
+a source and construction proof; no build, server, benchmark or gate was
+run. It does not establish identical generated instructions or measured
+PRE/POST rates.
+
+**Finding and fix.** The submitted diff did not preserve disabled INFO:
+it counted shard owners, visited every thread and formatted
+`read_local_thread_*` and `read_local_active_threads` on off boots too.
+That changed replies and could grow INFO's output allocation. Its
+`owned[kMaxThreads]` array also lived in the common `cmd_info` frame.
+The fix guards the **call** with the effective predicate at
+`src/cmd/t_server.cc:1998`, and moves the entire walk, scratch arrays and
+formatting into `append_read_local_thread_info` in `src/core/rl2s.cc:310`.
+`noinline` keeps those arrays out of the caller, including under LTO.
+The helper receives only references, after the guard; it does not take
+an eagerly computed topology snapshot or allocate before early-returning.
+Disabled INFO emits neither new field family. Enabled INFO keeps both.
+The offset assert at `src/core/thread.h:306` additionally locks the old
+sidecar prefix; it adds no runtime state. The lane implementation is kept.
+
+### 1. Threads, capabilities and roles: unchanged off
+
+The effective predicate is `cfg.overlap == 0 && cfg.read_local != 0`
+(`src/core/server.h:582`). It is false when the knob is zero. The only two
+thread modes are Split and Fused (`src/core/config.h:225`): `main` returns
+through the existing fused path at `src/main.cc:312`, then tests the
+predicate at `:317`. Thus the **new runtime** can be called only for
+Split **and** read-local 1, with overlap 0. Its entry also checks this
+contract before construction (`src/core/rl2s.cc:77`). Mode, overlap and
+read-local remain boot-only CONFIG entries (`src/cmd/t_server.cc:322`).
+
+Off falls through to the old `vector<IoLoop>` and `vector<ExLoop>` at
+`src/main.cc:338`; `ExLoop` still means **`ExLoopT<false>`**
+(`src/core/ex_loop.h:3439`). It does not construct the new
+`vector<FusedExLoop>` at `src/core/rl2s.cc:92`, bind a fused executor,
+call `activate_fused`, or enter `run_fused`.
+
+Placement still builds the ordinary split ratio/explicit placement
+(`src/core/server.h:239`), creates `placement_.total_threads()` contexts
+(`:294`, `:305`), and assigns shard homes to EX tids
+(`src/core/placement.h:178`, `src/core/server.h:367`). Actual OS workers
+are created once per EX tid at `src/main.cc:372` and once per IFID tid at
+`:507`. At gate geometry this is **8 workers: 6 IFID, 2 EX, 16 shards on
+EX**, not an additional reader tier. Main remains the existing monitor.
+
+Baseline already provisioned the opposite dormant loop on every worker
+for FLIP: dormant IO on EX at `src/main.cc:395`, dormant ordinary EX on IO
+at `:528`. Those allocations/capabilities are not new. Off role changes
+still call only `ios[tid].run()` / `exs[tid].run()` (`:426`, `:562`), so
+FLIP cannot later upgrade an off worker to the reader runtime. The entire
+ordinary split construction/role-loop/shutdown suffix starting at
+`src/main.cc:336` was compared byte-for-byte with baseline and is identical.
+
+### 2. Allocations, buffers, sinks and registrations: none added off
+
+The inventory includes existing allocations newly reachable by the armed
+split runtime, not just literal `new` lines in `rl2s.cc`.
+
+| Construction or allocation | Guard and off-arm result |
+| --- | --- |
+| Server read-local sidecar (`new ReadLocalServerState`) | Existing predicate guard at `src/core/server.h:316`, allocation `:317`. False off; the existing owning pointer stays null. |
+| Per-thread sidecars, tick publications, stats and sink slots | Same guard encloses the thread loop at `src/core/server.h:322`; `src/core/thread.h:353` performs `new ReadLocalThreadState`. Never called off, including for future FLIP readers/owners. |
+| Per-shard extended store state and foreign-read safety arrays | Same guard encloses `prepare_read_local` at `src/core/server.h:328`. It reaches `new ReadLocalStoreState` at `src/store/flatstore_atomic.inc:890`. Off stores retain the original ordinary atomic-state allocation path. |
+| `ReadLocalExImpl`, `LaneEntry[kInboxSlots]`, fallback array | `src/core/ex_loop.h:196` first requires compile-time `Fused`, then the effective predicate at `:197`; the three `new` sites are `:198`, `:200`, `:202`. The entire block is discarded for the off `ExLoopT<false>`. |
+| Deferred `Entry[kCapacity]` buffer, block-cache heads and retire sink | Deferred initialization is inside that same block (`src/core/ex_loop.h:204`); buffer allocation is `src/core/read_local.h:260`, sink binding `:262`. Cache heads and retire-ring bookkeeping are members of the optional queue (`:464`), not a baseline per-thread allocation. No off queue exists to grow debug pending sets or retain blocks. |
+| Runtime vectors, default-constructed loop containers and worker closures | `pool`, `ios(nthreads)`, `executors(nthreads)` at `src/core/rl2s.cc:90`, then `pool.emplace_back` at `:122`, are inside the guarded runtime call. This includes allocations by embedded container constructors, such as executor deques, before `init`. The off runtime constructs only the baseline vectors/loops described above. |
+| Boot-gate worker-state vector, loading/error strings and shutdown report buffers | `FusedBootGate boot(nthreads)` at `src/core/rl2s.cc:95` allocates its vector in `src/core/fused_boot_gate.h:23`. The new runtime's local strings (`src/core/rl2s.cc:130`, `:235`, `:263`, `:281`) and report (`:115`) share the same call guard. No static boot gate or report is constructed for off. |
+| Inbox channels, IO/EX rings, network registration and listener resources | All new-runtime calls are below its guard: `src/core/rl2s.cc:127`, `:129`, `:144`, `:196`, `:248`, `:264`, `:270`. Inbox storage comes from `src/core/thread.h:342`; rings from `src/core/ex_loop.h:213` and `src/core/io_loop.h:102`. Off calls the unchanged baseline initializers with the same parameters; it does not additionally run any of these RL2S initializers. |
+| Role/client registration, capacity, completion, demotion and progress hooks | `src/core/rl2s.cc:147`, `:152`, `:160`, `:165`, `:167`, `:172`, `:181` are inside the enabled runtime. Fused sink publication and store arming occur in `src/core/ex_loop.h:241` and `:245`. Off never installs those fused hooks; its pre-existing ordinary IO registration hooks remain. |
+| Connection RYOW sidecar and local-read reply reserves | Parser admission remains behind compile-time `Fused` and the effective predicate (`src/core/io_loop.h:4200`). Its write marking at `:4253` can reach `Rob::prepare_read_local` (`src/net/rob.h:486`, `:709`) only in that armed path. Local-copy reply reservation (`src/core/ex_loop.h:990`) likewise belongs to the existing reader lane. No new accept-time buffer or sidecar allocation was added. |
+| New INFO ownership array, row buffer and output growth | Fixed: caller guard at `src/cmd/t_server.cc:1998` precedes helper entry; `owned` and `row` exist only in `src/core/rl2s.cc:313`, `:317`. Both `body.append` sites (`:336`, `:341`) are inside that helper. Off emits the baseline bytes and takes none of these buffer allocations. |
+
+There is no new `.reserve()`/`.resize()` call in the feature file and no
+file-scope runtime object or thread-local registration. Its project header
+dependencies were already included by main/genthread. All feature-owned
+vectors, strings, arrays and OS-thread creation are local to the two
+guarded entry points. Existing optional sidecars store the hooks; no
+separate always-present registry was introduced.
+
+QSBR registration is the optional per-thread state allocated at boot,
+not mere membership in the baseline `threads_` vector. Its floor scanner
+(`src/core/server.h:606`) is driven by the optional deferred queues.
+Neither the scanner, a tick/park publication, nor a sink binding is added
+to an off thread. Existing store ownership-edge checks remain unchanged
+(`src/core/server.h:1951`) and return for unarmed stores.
+
+### 3. Hot loops and eager arguments: no added off-arm branch
+
+| Changed test or call | Why off does not execute it |
+| --- | --- |
+| Runtime selection and overlap validation | `src/main.cc:317` and `src/core/config.h:1215` are boot-only, before operation processing. The changed startup notice at `src/main.cc:183` is also cold and silent with read-local 0. |
+| IO reader-entry resume/epoch/active publication and exit publication | Both are inside `if constexpr (Fused)` at `src/core/io_loop.h:541` and `:766`. Ordinary `run()` selects `run_split<0/1>` (`:429`); every network/TLS/unix variant passes `Fused=false` (`:407`). The new calls and **all their arguments**, including `read_local_epoch()`, are discarded. Entry/exit are tenure boundaries even on the enabled arm. |
+| EX parked/lane-count check, config invalidation, debug ownership check and retire drain | All additions to `ExLoopT::run` are inside `if constexpr (Fused)` (`src/core/ex_loop.h:708`, `:723`, `:800`). Off instantiates `ExLoopT<false>`; it does not evaluate `read_local_impl()`, `self_->id()` for the new check, or epoch/sink arguments. This also holds with `TOMO_RL_CACHE_DEBUG`. |
+| New mode tests in baseline fused pass/sweep | `src/core/ex_loop.h:397` and `:640` live in `static_assert(Fused)` entry points. Their IO call sites are themselves under `if constexpr (Fused)` at `src/core/io_loop.h:6853`. Off never calls them. They do add a rotation-boundary test to the existing **1s** schedule; the 1s PRE/POST measurement obligation remains. |
+| Shard-less pass's config, clock, pressure and cap checks | Entirely in the enabled-only `ExLoopT<true>::split_read_local_pass` (`src/core/rl2s.cc:46`). No callback to it is installed off. |
+| `OwnsShards` config-refresh selection | `src/core/ex_loop.h:1913` defaults to **true** on all old calls. `if constexpr (OwnsShards)` adds no runtime test. Only `src/core/rl2s.cc:49` calls `<false>`. Expanding the true block gives the baseline owner-refresh token sequence. |
+| Changed shared `Server::read_local_enabled` predicate | The ordinary parser's `Fused && ...` at `src/core/io_loop.h:3924` short-circuits at compile time. In common IO config refresh, the existing version-change return at `:7135` still precedes the predicate at `:7146`; changing the predicate adds no check to unchanged-version rotations. Other shared uses are boot/config preparation and INFO/RESETSTAT, not the GET/SET dispatch/execution loops. |
+| New INFO caller guard | `src/cmd/t_server.cc:1998` is inside the INFO SERVER command section (`:1975`), not any general operation or worker loop. All new per-shard/per-thread reporting loops are behind it in the cold helper. |
+
+The parser/admission/dispatch function at `src/core/io_loop.h:3918` is
+unchanged. In particular, `if constexpr (Fused)` and the enabled/pressure
+tests at `:3992` still precede `read_local_lane_quota(active_.size())` at
+`:3995`. Off evaluates **neither** the quota call **nor** `active_.size()`;
+there is no new cold-cache-line load disguised as a no-op callee.
+
+Source comparisons discarded the `if constexpr (Fused)` statements in
+both revisions of IO/EX rotation bodies; the remaining token sequences
+match. The initializers, ordinary split dispatch entry points, parser,
+owner task drain and IO config-refresh bodies also match baseline tokens.
+These comparisons check the source specialization, not compiler output.
+
+### 4. Per-operation layouts: unchanged off; optional growth disclosed
+
+| Type | Baseline bytes | Current bytes | Existing layout assert |
+| --- | ---: | ---: | --- |
+| `Op` | 336 | 336 | `src/exec/op.h:463` |
+| `Client` | 1984 | 1984 | `src/net/conn.h:905` |
+| `ThreadCtx` | 1408 | 1408 | `src/core/thread.h:1208` |
+| `Shard` | 1440 | 1440 | `src/core/shard.h:523` |
+| `FlatStore` | 944 | 944 | `src/store/flatstore.h:3912` |
+| `Rob<64>` | 192 | 192 | `src/net/rob.h:1098` |
+| `AtomicEntry` | 144 | 144 | `src/store/atomic_mvcc.h:70` |
+| `Config` | 624 | 624 | `src/core/config.h:429` |
+| Ordinary `ExLoop` | 6112 | 6112 | `src/core/ex_loop.h:3450` |
+
+No data members were added/reordered in `IoLoop`, `Server`, `ThreadCtx`
+or `ExLoopT`: their member declarations match baseline, including the
+existing optional pointer in `ThreadCtx`'s tail (`src/core/thread.h:1205`)
+and the empty `ReadLocalExState<false>` (`src/core/ex_loop.h:103`, `:165`)
+held with `[[no_unique_address]]` (`:3436`). The other locked type files
+are byte-identical; the Config struct's only change is a field comment. Thus no
+off-arm object has a new allocation stride, field offset or cache line.
+IoLoop and Server have no numeric whole-size assert here; their unchanged
+member/type layouts, rather than an invented size, are the evidence.
+
+**One on-arm sidecar did grow:** production `ReadLocalThreadState`
+**360 -> 368 bytes**. Baseline has an 8-byte tick, a 24-byte sink
+(`src/store/read_local_reclaim.h:61`) and 328-byte stats (38 `uint64_t`
+counters plus three arm counters: `src/core/thread.h:137`,
+`src/net/rob.h:62`). The added `atomic<bool> lane_active` follows them
+(`src/core/thread.h:303`); alignment adds seven padding bytes. The old
+prefix offset is now asserted as 360 at `:306`, total size 368 at `:308`.
+The diagnostics-only selector 3 retains its existing separate alignment
+and is excluded from these production asserts. It too uses only optional
+state. **Off pays zero bytes and no cache line for this growth**, because
+`Server::init` never calls `ThreadCtx::init_read_local_state` off. Existing
+hot fields remain at their old offsets on both arms.
+
+After the INFO fix, all four off-arm source properties above hold. Static
+verification completed: baseline source/token comparisons, Python AST
+parsing of the four changed batteries (without importing/running them),
+and `git diff HEAD --check`. The maintainer still owns build/layout-assert
+execution, both-mode boots, the gate and the PRE/POST rates. No gate row
+was added or retired; quick/full expectations remain **327/344**. The
+quick-tier branch starts at `tests/gate.sh:1258` and exits at `:1260`; the
+standalone RL2S battery has no row on either side of it.
+
 ## MEASUREMENT PLAN
 
 The maintainer performs all steps below, on the quiet box. No performance
@@ -221,7 +390,9 @@
    row automatically.
 2. Boot the four overlap-0 combinations: 2s/0, 2s/1, 1s/0, 1s/1. On eight
    real cores use 6:2 for split; omit ratio for 1s. Require INFO's effective
-   lane bit and active-reader count to match: 0, 6, 0, 8. Also smoke 2s/1
+   lane bit to be 0, 1, 0, 1. Enabled active-reader counts must be 6 and 8;
+   off arms must omit the new active-count/per-thread fields and retain
+   their baseline INFO payload. Also smoke 2s/1
    with one shard and 6:2 (one EX has no shard), explicit placement, and
    the existing TCP/TLS/unix and epoll/uring boot/shutdown cases. Confirm
    split/1/overlap-1 fails with the documented message.
diff --git a/src/cmd/t_server.cc b/src/cmd/t_server.cc
index 2d5c81bfdb1c5ee1dcedddf26af8e1801a2ea4d3..7ad03034a03f1c5a2918a29c761c28fa54bdcb96
--- a/src/cmd/t_server.cc
+++ b/src/cmd/t_server.cc
@@ -14,6 +14,7 @@
 #include "server_tail.h"
 #include "slowlog.h"
 #include "../base/alloc.h"
+#include "../core/genthread.h"
 #include "../core/server.h"
 #include "../core/lbsignals.h"
 #include "../core/pubsub_event.h"
@@ -1992,33 +1993,10 @@
                 static_cast<unsigned>(g_server ? g_server->cfg().port : 0),
                 static_cast<unsigned long long>(uptime),
                 static_cast<unsigned long long>(uptime / 86400));
-        uint32_t active_readers = 0;
-        if (g_server) {
-            uint32_t owned[kMaxThreads] = {};
-            // Read the published ownership directory, never a vector FLIP may be rewriting.
-            for (uint32_t sid = 0; sid < g_server->nshards(); sid++)
-                owned[g_server->worker_of_shard(static_cast<int32_t>(sid))]++;
-            for (uint32_t tid = 0; tid < g_server->nthreads(); tid++) {
-                const ThreadCtx& thread = g_server->thread(tid);
-                const bool active = thread.read_local_lane_active();
-                active_readers += active;
-                const Role role = thread.role();
-                const char* name = role == Role::Idle ? "idle"
-                    : g_server->thread_mode() == ThreadMode::Fused ? "unified"
-                    : role == Role::Ifid ? "ifid" : "ex";
-                const ReadLocalStats* stats = g_server->read_local_enabled()
-                    ? &thread.read_local_stats() : nullptr;
-                // These per-thread totals are lifetime counters; INFO stats retains its existing
-                // RESETSTAT-relative aggregate counters. FLIP does not reset either sidecar.
-                appendf(body,
-                        "read_local_thread_%u:role=%s,shards=%u,active=%u,"
-                        "hits_total=%llu,mget_hits_total=%llu\r\n",
-                        tid, name, owned[tid], active ? 1u : 0u,
-                        static_cast<unsigned long long>(stats ? stats->hits : 0),
-                        static_cast<unsigned long long>(stats ? stats->mget_local_hits : 0));
-            }
-        }
-        appendf(body, "read_local_active_threads:%u\r\n", active_readers);
+        // Guard the CALL, including all argument evaluation. The cold non-inlined helper owns
+        // its scratch buffers so the disabled INFO path keeps its old output without those arrays.
+        if (g_server && g_server->read_local_enabled())
+            append_read_local_thread_info(body, *g_server);
         if (g_server && g_server->thread_mode() == ThreadMode::Fused) {
             appendf(body,
                     "fused_threads:%u\r\nclient_threads:%u\r\nowner_threads:%u\r\n"
diff --git a/src/core/genthread.h b/src/core/genthread.h
index c47b788f303c3a7e7f482d5ddc9f5a580a0a9bbb..e874192a1f6c2412955de097a7cd14587582ee0f
--- a/src/core/genthread.h
+++ b/src/core/genthread.h
@@ -3,6 +3,7 @@
 #pragma once
 
 #include <memory>
+#include <string>
 #include <vector>
 
 namespace tomo {
@@ -27,4 +28,7 @@
                      LateUnixListener& unix_listener,
                      ShutdownReportFinalLine& final_report);
 
+// INFO-only diagnostics. The caller must test read_local_enabled() before this call.
+void append_read_local_thread_info(std::string& body, const Server& server);
+
 }  // namespace tomo
diff --git a/src/core/rl2s.cc b/src/core/rl2s.cc
index d942a2e0f0f948a54a3f9becd0e2e32efb231a98..274b33481ca4d2e1e9e9b06d9d4dc9da575090e8
--- a/src/core/rl2s.cc
+++ b/src/core/rl2s.cc
@@ -305,4 +305,40 @@
     return 0;
 }
 
+// Keep the scratch arrays out of cmd_info's frame, including with LTO. Only an armed INFO SERVER
+// request reaches this helper; the boot/runtime paths install no reporting callback or sidecar.
+__attribute__((noinline, cold))
+void append_read_local_thread_info(std::string& body, const Server& srv) {
+    uint32_t active_readers = 0;
+    uint32_t owned[kMaxThreads] = {};
+    // Read the published ownership directory, never a vector FLIP may be rewriting.
+    for (uint32_t sid = 0; sid < srv.nshards(); sid++)
+        owned[srv.worker_of_shard(static_cast<int32_t>(sid))]++;
+    char row[192];  // fixed grammar, including full-width uint32/uint64 counters
+    for (uint32_t tid = 0; tid < srv.nthreads(); tid++) {
+        const ThreadCtx& thread = srv.thread(tid);
+        const bool active = thread.read_local_lane_active();
+        active_readers += active;
+        const Role role = thread.role();
+        const char* name = role == Role::Idle ? "idle"
+            : srv.thread_mode() == ThreadMode::Fused ? "unified"
+            : role == Role::Ifid ? "ifid" : "ex";
+        const ReadLocalStats& stats = thread.read_local_stats();
+        // Lifetime counters survive FLIP and RESETSTAT; INFO stats retains its existing
+        // RESETSTAT-relative aggregate counters.
+        const int n = std::snprintf(row, sizeof(row),
+            "read_local_thread_%u:role=%s,shards=%u,active=%u,"
+            "hits_total=%llu,mget_hits_total=%llu\r\n",
+            tid, name, owned[tid], active ? 1u : 0u,
+            static_cast<unsigned long long>(stats.hits),
+            static_cast<unsigned long long>(stats.mget_local_hits));
+        if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+        body.append(row, static_cast<size_t>(n));
+    }
+    const int n = std::snprintf(row, sizeof(row), "read_local_active_threads:%u\r\n",
+                              active_readers);
+    if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+    body.append(row, static_cast<size_t>(n));
+}
+
 }  // namespace tomo
diff --git a/src/core/thread.h b/src/core/thread.h
index adea9605d0f0d40c3566e07b90a2663accb6b835..61802252934cbd450279ad7cd2dbaa8d5f03f465
--- a/src/core/thread.h
+++ b/src/core/thread.h
@@ -303,6 +303,8 @@
     std::atomic<bool> lane_active{false};
 };
 #if TOMO_READ_LOCAL_SET_TAX_VARIANT != 3
+static_assert(offsetof(ReadLocalThreadState, lane_active) == 360,
+              "lane activation must follow the entire pre-RL2S sidecar");
 static_assert(sizeof(ReadLocalThreadState) == 368,
               "lane activation adds one alignment word only to the optional sidecar");
 #endif

exec
/bin/bash -lc "git diff HEAD --check
rg -n '"'^## OFF-ARM PROOF|''^### [1-4]'"\\.|init_task_inbox_local|TaskInbox> inbox' RL2S.md src/core/thread.h
python3 - <<'PY'
import subprocess
from pathlib import Path
assert subprocess.check_output(['git','show','78c3e5391:tests/gate.sh']) == Path('tests/gate.sh').read_bytes()
print('Final check: tests/gate.sh is unchanged; EXPECT_QUICK / EXPECT_FULL remain 327 / 344.')
PY" in /home/user/Projects/cx-rl2s
 succeeded in 0ms:
src/core/thread.h:327:    // pinned itself; init_task_inbox_local() then first-touches it from the consumer's local CPU.
src/core/thread.h:340:    bool init_task_inbox_local(const std::vector<uint32_t>& io,
src/core/thread.h:342:        std::unique_ptr<TaskInbox> inbox(new (std::nothrow) TaskInbox);
src/core/thread.h:347:    bool init_task_inbox_local_fused() {
src/core/thread.h:348:        std::unique_ptr<TaskInbox> inbox(new (std::nothrow) TaskInbox);
RL2S.md:214:## OFF-ARM PROOF
RL2S.md:238:### 1. Threads, capabilities and roles: unchanged off
RL2S.md:271:### 2. Allocations, buffers, sinks and registrations: none added off
RL2S.md:304:### 3. Hot loops and eager arguments: no added off-arm branch
RL2S.md:329:### 4. Per-operation layouts: unchanged off; optional growth disclosed
Final check: tests/gate.sh is unchanged; EXPECT_QUICK / EXPECT_FULL remain 327 / 344.

diff --git a/RL2S.md b/RL2S.md
index 64dea226f85e5b4b296ed8c9f1de21ae463a8cf0..0101d325da31dbe431bf65c8d205915d53e7847f
--- a/RL2S.md
+++ b/RL2S.md
@@ -76,7 +76,8 @@
 guards evaluation of `active_.size()`; no admission expression moved ahead
 of its old check.
 
-All physical threads provision an `IoLoop` and `FusedExLoop`. They use the
+On an enabled `2s/1` boot, all physical threads provision an `IoLoop` and
+`FusedExLoop`. They use the
 ordinary split task inbox layout and producer masks, including FLIP's
 existing remasking. Initial persistence loading runs before store arming.
 Each executor's `activate_fused` binds its permanent ThreadCtx sink, then
@@ -136,7 +137,8 @@
 1984, ThreadCtx 1408, Shard 1440, FlatStore 944, Rob<64> 192, AtomicEntry 144,
 Config 624; the ordinary ExLoop assert remains 6112. The only data layout
 change is the optional `ReadLocalThreadState`: 360 -> 368 bytes in the
-production build, asserted at `src/core/thread.h:306`. An atomic loop-entry
+production build, asserted at `src/core/thread.h:308`; its old 360-byte
+prefix is also locked at `src/core/thread.h:306`. An atomic loop-entry
 flag plus alignment occupies the extra word, after all existing members.
 The diagnostics-only selector-3 sidecar already has different alignment
 and is excluded from this production-size assert. Read-local 0 allocates
@@ -148,7 +150,8 @@
 ## Observable activation
 
 `INFO server` still reports `thread_mode:2s`, live `io_threads` and
-`ex_threads`, and effective `read_local:1`. It now also reports:
+`ex_threads`, and effective `read_local:1`. When the effective lane is
+enabled, it also reports:
 
 ```text
 read_local_thread_0:role=ifid,shards=0,active=1,hits_total=123,mget_hits_total=7
@@ -161,10 +164,15 @@
 network waits; it is not inferred from the knob or ownership. The atomic
 flag is written only at loop entry/exit. Shard counts come from published
 owner IDs, not concurrently modified ownership vectors
-(`src/cmd/t_server.cc:1995`). After FLIP settles, every IFID row must have
+(`src/core/rl2s.cc:313`). After FLIP settles, every IFID row must have
 shards=0/active=1, and every EX row active=0. As with other INFO geometry,
 the individual samples are not one transactional view during a move.
 
+With read-local disabled, both new field families are absent, preserving
+the baseline INFO payload. Their caller guard is at
+`src/cmd/t_server.cc:1998`; their scratch arrays and formatting live in the
+non-inlined helper at `src/core/rl2s.cc:310`.
+
 Per-thread `*_total` values are lifetime completions and survive FLIP and
 CONFIG RESETSTAT. They can be nonzero on an EX row after that thread has
 previously served IO. Aggregate `INFO stats` keeps its existing RESETSTAT
@@ -203,6 +211,167 @@
 row on either side of that exit. Updating existing battery internals does
 not change gate row counts.
 
+## OFF-ARM PROOF
+
+Compared against **`78c3e5391`**, for **`--thread-mode 2s --read-local 0`**,
+including its supported overlap 0 and 1 schedules. References below name
+the final working-tree sources unless explicitly marked baseline. This is
+a source and construction proof; no build, server, benchmark or gate was
+run. It does not establish identical generated instructions or measured
+PRE/POST rates.
+
+**Finding and fix.** The submitted diff did not preserve disabled INFO:
+it counted shard owners, visited every thread and formatted
+`read_local_thread_*` and `read_local_active_threads` on off boots too.
+That changed replies and could grow INFO's output allocation. Its
+`owned[kMaxThreads]` array also lived in the common `cmd_info` frame.
+The fix guards the **call** with the effective predicate at
+`src/cmd/t_server.cc:1998`, and moves the entire walk, scratch arrays and
+formatting into `append_read_local_thread_info` in `src/core/rl2s.cc:310`.
+`noinline` keeps those arrays out of the caller, including under LTO.
+The helper receives only references, after the guard; it does not take
+an eagerly computed topology snapshot or allocate before early-returning.
+Disabled INFO emits neither new field family. Enabled INFO keeps both.
+The offset assert at `src/core/thread.h:306` additionally locks the old
+sidecar prefix; it adds no runtime state. The lane implementation is kept.
+
+### 1. Threads, capabilities and roles: unchanged off
+
+The effective predicate is `cfg.overlap == 0 && cfg.read_local != 0`
+(`src/core/server.h:582`). It is false when the knob is zero. The only two
+thread modes are Split and Fused (`src/core/config.h:225`): `main` returns
+through the existing fused path at `src/main.cc:312`, then tests the
+predicate at `:317`. Thus the **new runtime** can be called only for
+Split **and** read-local 1, with overlap 0. Its entry also checks this
+contract before construction (`src/core/rl2s.cc:77`). Mode, overlap and
+read-local remain boot-only CONFIG entries (`src/cmd/t_server.cc:322`).
+
+Off falls through to the old `vector<IoLoop>` and `vector<ExLoop>` at
+`src/main.cc:338`; `ExLoop` still means **`ExLoopT<false>`**
+(`src/core/ex_loop.h:3439`). It does not construct the new
+`vector<FusedExLoop>` at `src/core/rl2s.cc:92`, bind a fused executor,
+call `activate_fused`, or enter `run_fused`.
+
+Placement still builds the ordinary split ratio/explicit placement
+(`src/core/server.h:239`), creates `placement_.total_threads()` contexts
+(`:294`, `:305`), and assigns shard homes to EX tids
+(`src/core/placement.h:178`, `src/core/server.h:367`). Actual OS workers
+are created once per EX tid at `src/main.cc:372` and once per IFID tid at
+`:507`. At gate geometry this is **8 workers: 6 IFID, 2 EX, 16 shards on
+EX**, not an additional reader tier. Main remains the existing monitor.
+
+Baseline already provisioned the opposite dormant loop on every worker
+for FLIP: dormant IO on EX at `src/main.cc:395`, dormant ordinary EX on IO
+at `:528`. Those allocations/capabilities are not new. Off role changes
+still call only `ios[tid].run()` / `exs[tid].run()` (`:426`, `:562`), so
+FLIP cannot later upgrade an off worker to the reader runtime. The entire
+ordinary split construction/role-loop/shutdown suffix starting at
+`src/main.cc:336` was compared byte-for-byte with baseline and is identical.
+
+### 2. Allocations, buffers, sinks and registrations: none added off
+
+The inventory includes existing allocations newly reachable by the armed
+split runtime, not just literal `new` lines in `rl2s.cc`.
+
+| Construction or allocation | Guard and off-arm result |
+| --- | --- |
+| Server read-local sidecar (`new ReadLocalServerState`) | Existing predicate guard at `src/core/server.h:316`, allocation `:317`. False off; the existing owning pointer stays null. |
+| Per-thread sidecars, tick publications, stats and sink slots | Same guard encloses the thread loop at `src/core/server.h:322`; `src/core/thread.h:353` performs `new ReadLocalThreadState`. Never called off, including for future FLIP readers/owners. |
+| Per-shard extended store state and foreign-read safety arrays | Same guard encloses `prepare_read_local` at `src/core/server.h:328`. It reaches `new ReadLocalStoreState` at `src/store/flatstore_atomic.inc:890`. Off stores retain the original ordinary atomic-state allocation path. |
+| `ReadLocalExImpl`, `LaneEntry[kInboxSlots]`, fallback array | `src/core/ex_loop.h:196` first requires compile-time `Fused`, then the effective predicate at `:197`; the three `new` sites are `:198`, `:200`, `:202`. The entire block is discarded for the off `ExLoopT<false>`. |
+| Deferred `Entry[kCapacity]` buffer, block-cache heads and retire sink | Deferred initialization is inside that same block (`src/core/ex_loop.h:204`); buffer allocation is `src/core/read_local.h:260`, sink binding `:262`. Cache heads and retire-ring bookkeeping are members of the optional queue (`:464`), not a baseline per-thread allocation. No off queue exists to grow debug pending sets or retain blocks. |
+| Runtime vectors, default-constructed loop containers and worker closures | `pool`, `ios(nthreads)`, `executors(nthreads)` at `src/core/rl2s.cc:90`, then `pool.emplace_back` at `:122`, are inside the guarded runtime call. This includes allocations by embedded container constructors, such as executor deques, before `init`. The off runtime constructs only the baseline vectors/loops described above. |
+| Boot-gate worker-state vector, loading/error strings and shutdown report buffers | `FusedBootGate boot(nthreads)` at `src/core/rl2s.cc:95` allocates its vector in `src/core/fused_boot_gate.h:23`. The new runtime's local strings (`src/core/rl2s.cc:130`, `:235`, `:263`, `:281`) and report (`:115`) share the same call guard. No static boot gate or report is constructed for off. |
+| Inbox channels, IO/EX rings, network registration and listener resources | All new-runtime calls are below its guard: `src/core/rl2s.cc:127`, `:129`, `:144`, `:196`, `:248`, `:264`, `:270`. Inbox storage comes from `src/core/thread.h:342`; rings from `src/core/ex_loop.h:213` and `src/core/io_loop.h:102`. Off calls the unchanged baseline initializers with the same parameters; it does not additionally run any of these RL2S initializers. |
+| Role/client registration, capacity, completion, demotion and progress hooks | `src/core/rl2s.cc:147`, `:152`, `:160`, `:165`, `:167`, `:172`, `:181` are inside the enabled runtime. Fused sink publication and store arming occur in `src/core/ex_loop.h:241` and `:245`. Off never installs those fused hooks; its pre-existing ordinary IO registration hooks remain. |
+| Connection RYOW sidecar and local-read reply reserves | Parser admission remains behind compile-time `Fused` and the effective predicate (`src/core/io_loop.h:4200`). Its write marking at `:4253` can reach `Rob::prepare_read_local` (`src/net/rob.h:486`, `:709`) only in that armed path. Local-copy reply reservation (`src/core/ex_loop.h:990`) likewise belongs to the existing reader lane. No new accept-time buffer or sidecar allocation was added. |
+| New INFO ownership array, row buffer and output growth | Fixed: caller guard at `src/cmd/t_server.cc:1998` precedes helper entry; `owned` and `row` exist only in `src/core/rl2s.cc:313`, `:317`. Both `body.append` sites (`:336`, `:341`) are inside that helper. Off emits the baseline bytes and takes none of these buffer allocations. |
+
+There is no new `.reserve()`/`.resize()` call in the feature file and no
+file-scope runtime object or thread-local registration. Its project header
+dependencies were already included by main/genthread. All feature-owned
+vectors, strings, arrays and OS-thread creation are local to the two
+guarded entry points. Existing optional sidecars store the hooks; no
+separate always-present registry was introduced.
+
+QSBR registration is the optional per-thread state allocated at boot,
+not mere membership in the baseline `threads_` vector. Its floor scanner
+(`src/core/server.h:606`) is driven by the optional deferred queues.
+Neither the scanner, a tick/park publication, nor a sink binding is added
+to an off thread. Existing store ownership-edge checks remain unchanged
+(`src/core/server.h:1951`) and return for unarmed stores.
+
+### 3. Hot loops and eager arguments: no added off-arm branch
+
+| Changed test or call | Why off does not execute it |
+| --- | --- |
+| Runtime selection and overlap validation | `src/main.cc:317` and `src/core/config.h:1215` are boot-only, before operation processing. The changed startup notice at `src/main.cc:183` is also cold and silent with read-local 0. |
+| IO reader-entry resume/epoch/active publication and exit publication | Both are inside `if constexpr (Fused)` at `src/core/io_loop.h:541` and `:766`. Ordinary `run()` selects `run_split<0/1>` (`:429`); every network/TLS/unix variant passes `Fused=false` (`:407`). The new calls and **all their arguments**, including `read_local_epoch()`, are discarded. Entry/exit are tenure boundaries even on the enabled arm. |
+| EX parked/lane-count check, config invalidation, debug ownership check and retire drain | All additions to `ExLoopT::run` are inside `if constexpr (Fused)` (`src/core/ex_loop.h:708`, `:723`, `:800`). Off instantiates `ExLoopT<false>`; it does not evaluate `read_local_impl()`, `self_->id()` for the new check, or epoch/sink arguments. This also holds with `TOMO_RL_CACHE_DEBUG`. |
+| New mode tests in baseline fused pass/sweep | `src/core/ex_loop.h:397` and `:640` live in `static_assert(Fused)` entry points. Their IO call sites are themselves under `if constexpr (Fused)` at `src/core/io_loop.h:6853`. Off never calls them. They do add a rotation-boundary test to the existing **1s** schedule; the 1s PRE/POST measurement obligation remains. |
+| Shard-less pass's config, clock, pressure and cap checks | Entirely in the enabled-only `ExLoopT<true>::split_read_local_pass` (`src/core/rl2s.cc:46`). No callback to it is installed off. |
+| `OwnsShards` config-refresh selection | `src/core/ex_loop.h:1913` defaults to **true** on all old calls. `if constexpr (OwnsShards)` adds no runtime test. Only `src/core/rl2s.cc:49` calls `<false>`. Expanding the true block gives the baseline owner-refresh token sequence. |
+| Changed shared `Server::read_local_enabled` predicate | The ordinary parser's `Fused && ...` at `src/core/io_loop.h:3924` short-circuits at compile time. In common IO config refresh, the existing version-change return at `:7135` still precedes the predicate at `:7146`; changing the predicate adds no check to unchanged-version rotations. Other shared uses are boot/config preparation and INFO/RESETSTAT, not the GET/SET dispatch/execution loops. |
+| New INFO caller guard | `src/cmd/t_server.cc:1998` is inside the INFO SERVER command section (`:1975`), not any general operation or worker loop. All new per-shard/per-thread reporting loops are behind it in the cold helper. |
+
+The parser/admission/dispatch function at `src/core/io_loop.h:3918` is
+unchanged. In particular, `if constexpr (Fused)` and the enabled/pressure
+tests at `:3992` still precede `read_local_lane_quota(active_.size())` at
+`:3995`. Off evaluates **neither** the quota call **nor** `active_.size()`;
+there is no new cold-cache-line load disguised as a no-op callee.
+
+Source comparisons discarded the `if constexpr (Fused)` statements in
+both revisions of IO/EX rotation bodies; the remaining token sequences
+match. The initializers, ordinary split dispatch entry points, parser,
+owner task drain and IO config-refresh bodies also match baseline tokens.
+These comparisons check the source specialization, not compiler output.
+
+### 4. Per-operation layouts: unchanged off; optional growth disclosed
+
+| Type | Baseline bytes | Current bytes | Existing layout assert |
+| --- | ---: | ---: | --- |
+| `Op` | 336 | 336 | `src/exec/op.h:463` |
+| `Client` | 1984 | 1984 | `src/net/conn.h:905` |
+| `ThreadCtx` | 1408 | 1408 | `src/core/thread.h:1208` |
+| `Shard` | 1440 | 1440 | `src/core/shard.h:523` |
+| `FlatStore` | 944 | 944 | `src/store/flatstore.h:3912` |
+| `Rob<64>` | 192 | 192 | `src/net/rob.h:1098` |
+| `AtomicEntry` | 144 | 144 | `src/store/atomic_mvcc.h:70` |
+| `Config` | 624 | 624 | `src/core/config.h:429` |
+| Ordinary `ExLoop` | 6112 | 6112 | `src/core/ex_loop.h:3450` |
+
+No data members were added/reordered in `IoLoop`, `Server`, `ThreadCtx`
+or `ExLoopT`: their member declarations match baseline, including the
+existing optional pointer in `ThreadCtx`'s tail (`src/core/thread.h:1205`)
+and the empty `ReadLocalExState<false>` (`src/core/ex_loop.h:103`, `:165`)
+held with `[[no_unique_address]]` (`:3436`). The other locked type files
+are byte-identical; the Config struct's only change is a field comment. Thus no
+off-arm object has a new allocation stride, field offset or cache line.
+IoLoop and Server have no numeric whole-size assert here; their unchanged
+member/type layouts, rather than an invented size, are the evidence.
+
+**One on-arm sidecar did grow:** production `ReadLocalThreadState`
+**360 -> 368 bytes**. Baseline has an 8-byte tick, a 24-byte sink
+(`src/store/read_local_reclaim.h:61`) and 328-byte stats (38 `uint64_t`
+counters plus three arm counters: `src/core/thread.h:137`,
+`src/net/rob.h:62`). The added `atomic<bool> lane_active` follows them
+(`src/core/thread.h:303`); alignment adds seven padding bytes. The old
+prefix offset is now asserted as 360 at `:306`, total size 368 at `:308`.
+The diagnostics-only selector 3 retains its existing separate alignment
+and is excluded from these production asserts. It too uses only optional
+state. **Off pays zero bytes and no cache line for this growth**, because
+`Server::init` never calls `ThreadCtx::init_read_local_state` off. Existing
+hot fields remain at their old offsets on both arms.
+
+After the INFO fix, all four off-arm source properties above hold. Static
+verification completed: baseline source/token comparisons, Python AST
+parsing of the four changed batteries (without importing/running them),
+and `git diff HEAD --check`. The maintainer still owns build/layout-assert
+execution, both-mode boots, the gate and the PRE/POST rates. No gate row
+was added or retired; quick/full expectations remain **327/344**. The
+quick-tier branch starts at `tests/gate.sh:1258` and exits at `:1260`; the
+standalone RL2S battery has no row on either side of it.
+
 ## MEASUREMENT PLAN
 
 The maintainer performs all steps below, on the quiet box. No performance
@@ -221,7 +390,9 @@
    row automatically.
 2. Boot the four overlap-0 combinations: 2s/0, 2s/1, 1s/0, 1s/1. On eight
    real cores use 6:2 for split; omit ratio for 1s. Require INFO's effective
-   lane bit and active-reader count to match: 0, 6, 0, 8. Also smoke 2s/1
+   lane bit to be 0, 1, 0, 1. Enabled active-reader counts must be 6 and 8;
+   off arms must omit the new active-count/per-thread fields and retain
+   their baseline INFO payload. Also smoke 2s/1
    with one shard and 6:2 (one EX has no shard), explicit placement, and
    the existing TCP/TLS/unix and epoll/uring boot/shutdown cases. Confirm
    split/1/overlap-1 fails with the documented message.
diff --git a/src/cmd/t_server.cc b/src/cmd/t_server.cc
index 2d5c81bfdb1c5ee1dcedddf26af8e1801a2ea4d3..7ad03034a03f1c5a2918a29c761c28fa54bdcb96
--- a/src/cmd/t_server.cc
+++ b/src/cmd/t_server.cc
@@ -14,6 +14,7 @@
 #include "server_tail.h"
 #include "slowlog.h"
 #include "../base/alloc.h"
+#include "../core/genthread.h"
 #include "../core/server.h"
 #include "../core/lbsignals.h"
 #include "../core/pubsub_event.h"
@@ -1992,33 +1993,10 @@
                 static_cast<unsigned>(g_server ? g_server->cfg().port : 0),
                 static_cast<unsigned long long>(uptime),
                 static_cast<unsigned long long>(uptime / 86400));
-        uint32_t active_readers = 0;
-        if (g_server) {
-            uint32_t owned[kMaxThreads] = {};
-            // Read the published ownership directory, never a vector FLIP may be rewriting.
-            for (uint32_t sid = 0; sid < g_server->nshards(); sid++)
-                owned[g_server->worker_of_shard(static_cast<int32_t>(sid))]++;
-            for (uint32_t tid = 0; tid < g_server->nthreads(); tid++) {
-                const ThreadCtx& thread = g_server->thread(tid);
-                const bool active = thread.read_local_lane_active();
-                active_readers += active;
-                const Role role = thread.role();
-                const char* name = role == Role::Idle ? "idle"
-                    : g_server->thread_mode() == ThreadMode::Fused ? "unified"
-                    : role == Role::Ifid ? "ifid" : "ex";
-                const ReadLocalStats* stats = g_server->read_local_enabled()
-                    ? &thread.read_local_stats() : nullptr;
-                // These per-thread totals are lifetime counters; INFO stats retains its existing
-                // RESETSTAT-relative aggregate counters. FLIP does not reset either sidecar.
-                appendf(body,
-                        "read_local_thread_%u:role=%s,shards=%u,active=%u,"
-                        "hits_total=%llu,mget_hits_total=%llu\r\n",
-                        tid, name, owned[tid], active ? 1u : 0u,
-                        static_cast<unsigned long long>(stats ? stats->hits : 0),
-                        static_cast<unsigned long long>(stats ? stats->mget_local_hits : 0));
-            }
-        }
-        appendf(body, "read_local_active_threads:%u\r\n", active_readers);
+        // Guard the CALL, including all argument evaluation. The cold non-inlined helper owns
+        // its scratch buffers so the disabled INFO path keeps its old output without those arrays.
+        if (g_server && g_server->read_local_enabled())
+            append_read_local_thread_info(body, *g_server);
         if (g_server && g_server->thread_mode() == ThreadMode::Fused) {
             appendf(body,
                     "fused_threads:%u\r\nclient_threads:%u\r\nowner_threads:%u\r\n"
diff --git a/src/core/genthread.h b/src/core/genthread.h
index c47b788f303c3a7e7f482d5ddc9f5a580a0a9bbb..e874192a1f6c2412955de097a7cd14587582ee0f
--- a/src/core/genthread.h
+++ b/src/core/genthread.h
@@ -3,6 +3,7 @@
 #pragma once
 
 #include <memory>
+#include <string>
 #include <vector>
 
 namespace tomo {
@@ -27,4 +28,7 @@
                      LateUnixListener& unix_listener,
                      ShutdownReportFinalLine& final_report);
 
+// INFO-only diagnostics. The caller must test read_local_enabled() before this call.
+void append_read_local_thread_info(std::string& body, const Server& server);
+
 }  // namespace tomo
diff --git a/src/core/rl2s.cc b/src/core/rl2s.cc
index d942a2e0f0f948a54a3f9becd0e2e32efb231a98..274b33481ca4d2e1e9e9b06d9d4dc9da575090e8
--- a/src/core/rl2s.cc
+++ b/src/core/rl2s.cc
@@ -305,4 +305,40 @@
     return 0;
 }
 
+// Keep the scratch arrays out of cmd_info's frame, including with LTO. Only an armed INFO SERVER
+// request reaches this helper; the boot/runtime paths install no reporting callback or sidecar.
+__attribute__((noinline, cold))
+void append_read_local_thread_info(std::string& body, const Server& srv) {
+    uint32_t active_readers = 0;
+    uint32_t owned[kMaxThreads] = {};
+    // Read the published ownership directory, never a vector FLIP may be rewriting.
+    for (uint32_t sid = 0; sid < srv.nshards(); sid++)
+        owned[srv.worker_of_shard(static_cast<int32_t>(sid))]++;
+    char row[192];  // fixed grammar, including full-width uint32/uint64 counters
+    for (uint32_t tid = 0; tid < srv.nthreads(); tid++) {
+        const ThreadCtx& thread = srv.thread(tid);
+        const bool active = thread.read_local_lane_active();
+        active_readers += active;
+        const Role role = thread.role();
+        const char* name = role == Role::Idle ? "idle"
+            : srv.thread_mode() == ThreadMode::Fused ? "unified"
+            : role == Role::Ifid ? "ifid" : "ex";
+        const ReadLocalStats& stats = thread.read_local_stats();
+        // Lifetime counters survive FLIP and RESETSTAT; INFO stats retains its existing
+        // RESETSTAT-relative aggregate counters.
+        const int n = std::snprintf(row, sizeof(row),
+            "read_local_thread_%u:role=%s,shards=%u,active=%u,"
+            "hits_total=%llu,mget_hits_total=%llu\r\n",
+            tid, name, owned[tid], active ? 1u : 0u,
+            static_cast<unsigned long long>(stats.hits),
+            static_cast<unsigned long long>(stats.mget_local_hits));
+        if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+        body.append(row, static_cast<size_t>(n));
+    }
+    const int n = std::snprintf(row, sizeof(row), "read_local_active_threads:%u\r\n",
+                              active_readers);
+    if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+    body.append(row, static_cast<size_t>(n));
+}
+
 }  // namespace tomo
diff --git a/src/core/thread.h b/src/core/thread.h
index adea9605d0f0d40c3566e07b90a2663accb6b835..61802252934cbd450279ad7cd2dbaa8d5f03f465
--- a/src/core/thread.h
+++ b/src/core/thread.h
@@ -303,6 +303,8 @@
     std::atomic<bool> lane_active{false};
 };
 #if TOMO_READ_LOCAL_SET_TAX_VARIANT != 3
+static_assert(offsetof(ReadLocalThreadState, lane_active) == 360,
+              "lane activation must follow the entire pre-RL2S sidecar");
 static_assert(sizeof(ReadLocalThreadState) == 368,
               "lane activation adds one alignment word only to the optional sidecar");
 #endif

codex
Updated [RL2S.md — OFF-ARM PROOF](/home/user/Projects/cx-rl2s/RL2S.md:214) with all four answers and file:line evidence.

Fixed unconditional INFO diagnostics: their buffers, ownership walk and formatting are now enabled-only. All four off-arm properties hold at source level; optional sidecar growth is disclosed.

Source comparisons and `git diff --check` passed. No build, server, benchmark or gate was run.
diff --git a/RL2S.md b/RL2S.md
index 64dea226f85e5b4b296ed8c9f1de21ae463a8cf0..0101d325da31dbe431bf65c8d205915d53e7847f
--- a/RL2S.md
+++ b/RL2S.md
@@ -76,7 +76,8 @@
 guards evaluation of `active_.size()`; no admission expression moved ahead
 of its old check.
 
-All physical threads provision an `IoLoop` and `FusedExLoop`. They use the
+On an enabled `2s/1` boot, all physical threads provision an `IoLoop` and
+`FusedExLoop`. They use the
 ordinary split task inbox layout and producer masks, including FLIP's
 existing remasking. Initial persistence loading runs before store arming.
 Each executor's `activate_fused` binds its permanent ThreadCtx sink, then
@@ -136,7 +137,8 @@
 1984, ThreadCtx 1408, Shard 1440, FlatStore 944, Rob<64> 192, AtomicEntry 144,
 Config 624; the ordinary ExLoop assert remains 6112. The only data layout
 change is the optional `ReadLocalThreadState`: 360 -> 368 bytes in the
-production build, asserted at `src/core/thread.h:306`. An atomic loop-entry
+production build, asserted at `src/core/thread.h:308`; its old 360-byte
+prefix is also locked at `src/core/thread.h:306`. An atomic loop-entry
 flag plus alignment occupies the extra word, after all existing members.
 The diagnostics-only selector-3 sidecar already has different alignment
 and is excluded from this production-size assert. Read-local 0 allocates
@@ -148,7 +150,8 @@
 ## Observable activation
 
 `INFO server` still reports `thread_mode:2s`, live `io_threads` and
-`ex_threads`, and effective `read_local:1`. It now also reports:
+`ex_threads`, and effective `read_local:1`. When the effective lane is
+enabled, it also reports:
 
 ```text
 read_local_thread_0:role=ifid,shards=0,active=1,hits_total=123,mget_hits_total=7
@@ -161,10 +164,15 @@
 network waits; it is not inferred from the knob or ownership. The atomic
 flag is written only at loop entry/exit. Shard counts come from published
 owner IDs, not concurrently modified ownership vectors
-(`src/cmd/t_server.cc:1995`). After FLIP settles, every IFID row must have
+(`src/core/rl2s.cc:313`). After FLIP settles, every IFID row must have
 shards=0/active=1, and every EX row active=0. As with other INFO geometry,
 the individual samples are not one transactional view during a move.
 
+With read-local disabled, both new field families are absent, preserving
+the baseline INFO payload. Their caller guard is at
+`src/cmd/t_server.cc:1998`; their scratch arrays and formatting live in the
+non-inlined helper at `src/core/rl2s.cc:310`.
+
 Per-thread `*_total` values are lifetime completions and survive FLIP and
 CONFIG RESETSTAT. They can be nonzero on an EX row after that thread has
 previously served IO. Aggregate `INFO stats` keeps its existing RESETSTAT
@@ -203,6 +211,167 @@
 row on either side of that exit. Updating existing battery internals does
 not change gate row counts.
 
+## OFF-ARM PROOF
+
+Compared against **`78c3e5391`**, for **`--thread-mode 2s --read-local 0`**,
+including its supported overlap 0 and 1 schedules. References below name
+the final working-tree sources unless explicitly marked baseline. This is
+a source and construction proof; no build, server, benchmark or gate was
+run. It does not establish identical generated instructions or measured
+PRE/POST rates.
+
+**Finding and fix.** The submitted diff did not preserve disabled INFO:
+it counted shard owners, visited every thread and formatted
+`read_local_thread_*` and `read_local_active_threads` on off boots too.
+That changed replies and could grow INFO's output allocation. Its
+`owned[kMaxThreads]` array also lived in the common `cmd_info` frame.
+The fix guards the **call** with the effective predicate at
+`src/cmd/t_server.cc:1998`, and moves the entire walk, scratch arrays and
+formatting into `append_read_local_thread_info` in `src/core/rl2s.cc:310`.
+`noinline` keeps those arrays out of the caller, including under LTO.
+The helper receives only references, after the guard; it does not take
+an eagerly computed topology snapshot or allocate before early-returning.
+Disabled INFO emits neither new field family. Enabled INFO keeps both.
+The offset assert at `src/core/thread.h:306` additionally locks the old
+sidecar prefix; it adds no runtime state. The lane implementation is kept.
+
+### 1. Threads, capabilities and roles: unchanged off
+
+The effective predicate is `cfg.overlap == 0 && cfg.read_local != 0`
+(`src/core/server.h:582`). It is false when the knob is zero. The only two
+thread modes are Split and Fused (`src/core/config.h:225`): `main` returns
+through the existing fused path at `src/main.cc:312`, then tests the
+predicate at `:317`. Thus the **new runtime** can be called only for
+Split **and** read-local 1, with overlap 0. Its entry also checks this
+contract before construction (`src/core/rl2s.cc:77`). Mode, overlap and
+read-local remain boot-only CONFIG entries (`src/cmd/t_server.cc:322`).
+
+Off falls through to the old `vector<IoLoop>` and `vector<ExLoop>` at
+`src/main.cc:338`; `ExLoop` still means **`ExLoopT<false>`**
+(`src/core/ex_loop.h:3439`). It does not construct the new
+`vector<FusedExLoop>` at `src/core/rl2s.cc:92`, bind a fused executor,
+call `activate_fused`, or enter `run_fused`.
+
+Placement still builds the ordinary split ratio/explicit placement
+(`src/core/server.h:239`), creates `placement_.total_threads()` contexts
+(`:294`, `:305`), and assigns shard homes to EX tids
+(`src/core/placement.h:178`, `src/core/server.h:367`). Actual OS workers
+are created once per EX tid at `src/main.cc:372` and once per IFID tid at
+`:507`. At gate geometry this is **8 workers: 6 IFID, 2 EX, 16 shards on
+EX**, not an additional reader tier. Main remains the existing monitor.
+
+Baseline already provisioned the opposite dormant loop on every worker
+for FLIP: dormant IO on EX at `src/main.cc:395`, dormant ordinary EX on IO
+at `:528`. Those allocations/capabilities are not new. Off role changes
+still call only `ios[tid].run()` / `exs[tid].run()` (`:426`, `:562`), so
+FLIP cannot later upgrade an off worker to the reader runtime. The entire
+ordinary split construction/role-loop/shutdown suffix starting at
+`src/main.cc:336` was compared byte-for-byte with baseline and is identical.
+
+### 2. Allocations, buffers, sinks and registrations: none added off
+
+The inventory includes existing allocations newly reachable by the armed
+split runtime, not just literal `new` lines in `rl2s.cc`.
+
+| Construction or allocation | Guard and off-arm result |
+| --- | --- |
+| Server read-local sidecar (`new ReadLocalServerState`) | Existing predicate guard at `src/core/server.h:316`, allocation `:317`. False off; the existing owning pointer stays null. |
+| Per-thread sidecars, tick publications, stats and sink slots | Same guard encloses the thread loop at `src/core/server.h:322`; `src/core/thread.h:353` performs `new ReadLocalThreadState`. Never called off, including for future FLIP readers/owners. |
+| Per-shard extended store state and foreign-read safety arrays | Same guard encloses `prepare_read_local` at `src/core/server.h:328`. It reaches `new ReadLocalStoreState` at `src/store/flatstore_atomic.inc:890`. Off stores retain the original ordinary atomic-state allocation path. |
+| `ReadLocalExImpl`, `LaneEntry[kInboxSlots]`, fallback array | `src/core/ex_loop.h:196` first requires compile-time `Fused`, then the effective predicate at `:197`; the three `new` sites are `:198`, `:200`, `:202`. The entire block is discarded for the off `ExLoopT<false>`. |
+| Deferred `Entry[kCapacity]` buffer, block-cache heads and retire sink | Deferred initialization is inside that same block (`src/core/ex_loop.h:204`); buffer allocation is `src/core/read_local.h:260`, sink binding `:262`. Cache heads and retire-ring bookkeeping are members of the optional queue (`:464`), not a baseline per-thread allocation. No off queue exists to grow debug pending sets or retain blocks. |
+| Runtime vectors, default-constructed loop containers and worker closures | `pool`, `ios(nthreads)`, `executors(nthreads)` at `src/core/rl2s.cc:90`, then `pool.emplace_back` at `:122`, are inside the guarded runtime call. This includes allocations by embedded container constructors, such as executor deques, before `init`. The off runtime constructs only the baseline vectors/loops described above. |
+| Boot-gate worker-state vector, loading/error strings and shutdown report buffers | `FusedBootGate boot(nthreads)` at `src/core/rl2s.cc:95` allocates its vector in `src/core/fused_boot_gate.h:23`. The new runtime's local strings (`src/core/rl2s.cc:130`, `:235`, `:263`, `:281`) and report (`:115`) share the same call guard. No static boot gate or report is constructed for off. |
+| Inbox channels, IO/EX rings, network registration and listener resources | All new-runtime calls are below its guard: `src/core/rl2s.cc:127`, `:129`, `:144`, `:196`, `:248`, `:264`, `:270`. Inbox storage comes from `src/core/thread.h:342`; rings from `src/core/ex_loop.h:213` and `src/core/io_loop.h:102`. Off calls the unchanged baseline initializers with the same parameters; it does not additionally run any of these RL2S initializers. |
+| Role/client registration, capacity, completion, demotion and progress hooks | `src/core/rl2s.cc:147`, `:152`, `:160`, `:165`, `:167`, `:172`, `:181` are inside the enabled runtime. Fused sink publication and store arming occur in `src/core/ex_loop.h:241` and `:245`. Off never installs those fused hooks; its pre-existing ordinary IO registration hooks remain. |
+| Connection RYOW sidecar and local-read reply reserves | Parser admission remains behind compile-time `Fused` and the effective predicate (`src/core/io_loop.h:4200`). Its write marking at `:4253` can reach `Rob::prepare_read_local` (`src/net/rob.h:486`, `:709`) only in that armed path. Local-copy reply reservation (`src/core/ex_loop.h:990`) likewise belongs to the existing reader lane. No new accept-time buffer or sidecar allocation was added. |
+| New INFO ownership array, row buffer and output growth | Fixed: caller guard at `src/cmd/t_server.cc:1998` precedes helper entry; `owned` and `row` exist only in `src/core/rl2s.cc:313`, `:317`. Both `body.append` sites (`:336`, `:341`) are inside that helper. Off emits the baseline bytes and takes none of these buffer allocations. |
+
+There is no new `.reserve()`/`.resize()` call in the feature file and no
+file-scope runtime object or thread-local registration. Its project header
+dependencies were already included by main/genthread. All feature-owned
+vectors, strings, arrays and OS-thread creation are local to the two
+guarded entry points. Existing optional sidecars store the hooks; no
+separate always-present registry was introduced.
+
+QSBR registration is the optional per-thread state allocated at boot,
+not mere membership in the baseline `threads_` vector. Its floor scanner
+(`src/core/server.h:606`) is driven by the optional deferred queues.
+Neither the scanner, a tick/park publication, nor a sink binding is added
+to an off thread. Existing store ownership-edge checks remain unchanged
+(`src/core/server.h:1951`) and return for unarmed stores.
+
+### 3. Hot loops and eager arguments: no added off-arm branch
+
+| Changed test or call | Why off does not execute it |
+| --- | --- |
+| Runtime selection and overlap validation | `src/main.cc:317` and `src/core/config.h:1215` are boot-only, before operation processing. The changed startup notice at `src/main.cc:183` is also cold and silent with read-local 0. |
+| IO reader-entry resume/epoch/active publication and exit publication | Both are inside `if constexpr (Fused)` at `src/core/io_loop.h:541` and `:766`. Ordinary `run()` selects `run_split<0/1>` (`:429`); every network/TLS/unix variant passes `Fused=false` (`:407`). The new calls and **all their arguments**, including `read_local_epoch()`, are discarded. Entry/exit are tenure boundaries even on the enabled arm. |
+| EX parked/lane-count check, config invalidation, debug ownership check and retire drain | All additions to `ExLoopT::run` are inside `if constexpr (Fused)` (`src/core/ex_loop.h:708`, `:723`, `:800`). Off instantiates `ExLoopT<false>`; it does not evaluate `read_local_impl()`, `self_->id()` for the new check, or epoch/sink arguments. This also holds with `TOMO_RL_CACHE_DEBUG`. |
+| New mode tests in baseline fused pass/sweep | `src/core/ex_loop.h:397` and `:640` live in `static_assert(Fused)` entry points. Their IO call sites are themselves under `if constexpr (Fused)` at `src/core/io_loop.h:6853`. Off never calls them. They do add a rotation-boundary test to the existing **1s** schedule; the 1s PRE/POST measurement obligation remains. |
+| Shard-less pass's config, clock, pressure and cap checks | Entirely in the enabled-only `ExLoopT<true>::split_read_local_pass` (`src/core/rl2s.cc:46`). No callback to it is installed off. |
+| `OwnsShards` config-refresh selection | `src/core/ex_loop.h:1913` defaults to **true** on all old calls. `if constexpr (OwnsShards)` adds no runtime test. Only `src/core/rl2s.cc:49` calls `<false>`. Expanding the true block gives the baseline owner-refresh token sequence. |
+| Changed shared `Server::read_local_enabled` predicate | The ordinary parser's `Fused && ...` at `src/core/io_loop.h:3924` short-circuits at compile time. In common IO config refresh, the existing version-change return at `:7135` still precedes the predicate at `:7146`; changing the predicate adds no check to unchanged-version rotations. Other shared uses are boot/config preparation and INFO/RESETSTAT, not the GET/SET dispatch/execution loops. |
+| New INFO caller guard | `src/cmd/t_server.cc:1998` is inside the INFO SERVER command section (`:1975`), not any general operation or worker loop. All new per-shard/per-thread reporting loops are behind it in the cold helper. |
+
+The parser/admission/dispatch function at `src/core/io_loop.h:3918` is
+unchanged. In particular, `if constexpr (Fused)` and the enabled/pressure
+tests at `:3992` still precede `read_local_lane_quota(active_.size())` at
+`:3995`. Off evaluates **neither** the quota call **nor** `active_.size()`;
+there is no new cold-cache-line load disguised as a no-op callee.
+
+Source comparisons discarded the `if constexpr (Fused)` statements in
+both revisions of IO/EX rotation bodies; the remaining token sequences
+match. The initializers, ordinary split dispatch entry points, parser,
+owner task drain and IO config-refresh bodies also match baseline tokens.
+These comparisons check the source specialization, not compiler output.
+
+### 4. Per-operation layouts: unchanged off; optional growth disclosed
+
+| Type | Baseline bytes | Current bytes | Existing layout assert |
+| --- | ---: | ---: | --- |
+| `Op` | 336 | 336 | `src/exec/op.h:463` |
+| `Client` | 1984 | 1984 | `src/net/conn.h:905` |
+| `ThreadCtx` | 1408 | 1408 | `src/core/thread.h:1208` |
+| `Shard` | 1440 | 1440 | `src/core/shard.h:523` |
+| `FlatStore` | 944 | 944 | `src/store/flatstore.h:3912` |
+| `Rob<64>` | 192 | 192 | `src/net/rob.h:1098` |
+| `AtomicEntry` | 144 | 144 | `src/store/atomic_mvcc.h:70` |
+| `Config` | 624 | 624 | `src/core/config.h:429` |
+| Ordinary `ExLoop` | 6112 | 6112 | `src/core/ex_loop.h:3450` |
+
+No data members were added/reordered in `IoLoop`, `Server`, `ThreadCtx`
+or `ExLoopT`: their member declarations match baseline, including the
+existing optional pointer in `ThreadCtx`'s tail (`src/core/thread.h:1205`)
+and the empty `ReadLocalExState<false>` (`src/core/ex_loop.h:103`, `:165`)
+held with `[[no_unique_address]]` (`:3436`). The other locked type files
+are byte-identical; the Config struct's only change is a field comment. Thus no
+off-arm object has a new allocation stride, field offset or cache line.
+IoLoop and Server have no numeric whole-size assert here; their unchanged
+member/type layouts, rather than an invented size, are the evidence.
+
+**One on-arm sidecar did grow:** production `ReadLocalThreadState`
+**360 -> 368 bytes**. Baseline has an 8-byte tick, a 24-byte sink
+(`src/store/read_local_reclaim.h:61`) and 328-byte stats (38 `uint64_t`
+counters plus three arm counters: `src/core/thread.h:137`,
+`src/net/rob.h:62`). The added `atomic<bool> lane_active` follows them
+(`src/core/thread.h:303`); alignment adds seven padding bytes. The old
+prefix offset is now asserted as 360 at `:306`, total size 368 at `:308`.
+The diagnostics-only selector 3 retains its existing separate alignment
+and is excluded from these production asserts. It too uses only optional
+state. **Off pays zero bytes and no cache line for this growth**, because
+`Server::init` never calls `ThreadCtx::init_read_local_state` off. Existing
+hot fields remain at their old offsets on both arms.
+
+After the INFO fix, all four off-arm source properties above hold. Static
+verification completed: baseline source/token comparisons, Python AST
+parsing of the four changed batteries (without importing/running them),
+and `git diff HEAD --check`. The maintainer still owns build/layout-assert
+execution, both-mode boots, the gate and the PRE/POST rates. No gate row
+was added or retired; quick/full expectations remain **327/344**. The
+quick-tier branch starts at `tests/gate.sh:1258` and exits at `:1260`; the
+standalone RL2S battery has no row on either side of it.
+
 ## MEASUREMENT PLAN
 
 The maintainer performs all steps below, on the quiet box. No performance
@@ -221,7 +390,9 @@
    row automatically.
 2. Boot the four overlap-0 combinations: 2s/0, 2s/1, 1s/0, 1s/1. On eight
    real cores use 6:2 for split; omit ratio for 1s. Require INFO's effective
-   lane bit and active-reader count to match: 0, 6, 0, 8. Also smoke 2s/1
+   lane bit to be 0, 1, 0, 1. Enabled active-reader counts must be 6 and 8;
+   off arms must omit the new active-count/per-thread fields and retain
+   their baseline INFO payload. Also smoke 2s/1
    with one shard and 6:2 (one EX has no shard), explicit placement, and
    the existing TCP/TLS/unix and epoll/uring boot/shutdown cases. Confirm
    split/1/overlap-1 fails with the documented message.
diff --git a/src/cmd/t_server.cc b/src/cmd/t_server.cc
index 2d5c81bfdb1c5ee1dcedddf26af8e1801a2ea4d3..7ad03034a03f1c5a2918a29c761c28fa54bdcb96
--- a/src/cmd/t_server.cc
+++ b/src/cmd/t_server.cc
@@ -14,6 +14,7 @@
 #include "server_tail.h"
 #include "slowlog.h"
 #include "../base/alloc.h"
+#include "../core/genthread.h"
 #include "../core/server.h"
 #include "../core/lbsignals.h"
 #include "../core/pubsub_event.h"
@@ -1992,33 +1993,10 @@
                 static_cast<unsigned>(g_server ? g_server->cfg().port : 0),
                 static_cast<unsigned long long>(uptime),
                 static_cast<unsigned long long>(uptime / 86400));
-        uint32_t active_readers = 0;
-        if (g_server) {
-            uint32_t owned[kMaxThreads] = {};
-            // Read the published ownership directory, never a vector FLIP may be rewriting.
-            for (uint32_t sid = 0; sid < g_server->nshards(); sid++)
-                owned[g_server->worker_of_shard(static_cast<int32_t>(sid))]++;
-            for (uint32_t tid = 0; tid < g_server->nthreads(); tid++) {
-                const ThreadCtx& thread = g_server->thread(tid);
-                const bool active = thread.read_local_lane_active();
-                active_readers += active;
-                const Role role = thread.role();
-                const char* name = role == Role::Idle ? "idle"
-                    : g_server->thread_mode() == ThreadMode::Fused ? "unified"
-                    : role == Role::Ifid ? "ifid" : "ex";
-                const ReadLocalStats* stats = g_server->read_local_enabled()
-                    ? &thread.read_local_stats() : nullptr;
-                // These per-thread totals are lifetime counters; INFO stats retains its existing
-                // RESETSTAT-relative aggregate counters. FLIP does not reset either sidecar.
-                appendf(body,
-                        "read_local_thread_%u:role=%s,shards=%u,active=%u,"
-                        "hits_total=%llu,mget_hits_total=%llu\r\n",
-                        tid, name, owned[tid], active ? 1u : 0u,
-                        static_cast<unsigned long long>(stats ? stats->hits : 0),
-                        static_cast<unsigned long long>(stats ? stats->mget_local_hits : 0));
-            }
-        }
-        appendf(body, "read_local_active_threads:%u\r\n", active_readers);
+        // Guard the CALL, including all argument evaluation. The cold non-inlined helper owns
+        // its scratch buffers so the disabled INFO path keeps its old output without those arrays.
+        if (g_server && g_server->read_local_enabled())
+            append_read_local_thread_info(body, *g_server);
         if (g_server && g_server->thread_mode() == ThreadMode::Fused) {
             appendf(body,
                     "fused_threads:%u\r\nclient_threads:%u\r\nowner_threads:%u\r\n"
diff --git a/src/core/genthread.h b/src/core/genthread.h
index c47b788f303c3a7e7f482d5ddc9f5a580a0a9bbb..e874192a1f6c2412955de097a7cd14587582ee0f
--- a/src/core/genthread.h
+++ b/src/core/genthread.h
@@ -3,6 +3,7 @@
 #pragma once
 
 #include <memory>
+#include <string>
 #include <vector>
 
 namespace tomo {
@@ -27,4 +28,7 @@
                      LateUnixListener& unix_listener,
                      ShutdownReportFinalLine& final_report);
 
+// INFO-only diagnostics. The caller must test read_local_enabled() before this call.
+void append_read_local_thread_info(std::string& body, const Server& server);
+
 }  // namespace tomo
diff --git a/src/core/rl2s.cc b/src/core/rl2s.cc
index d942a2e0f0f948a54a3f9becd0e2e32efb231a98..274b33481ca4d2e1e9e9b06d9d4dc9da575090e8
--- a/src/core/rl2s.cc
+++ b/src/core/rl2s.cc
@@ -305,4 +305,40 @@
     return 0;
 }
 
+// Keep the scratch arrays out of cmd_info's frame, including with LTO. Only an armed INFO SERVER
+// request reaches this helper; the boot/runtime paths install no reporting callback or sidecar.
+__attribute__((noinline, cold))
+void append_read_local_thread_info(std::string& body, const Server& srv) {
+    uint32_t active_readers = 0;
+    uint32_t owned[kMaxThreads] = {};
+    // Read the published ownership directory, never a vector FLIP may be rewriting.
+    for (uint32_t sid = 0; sid < srv.nshards(); sid++)
+        owned[srv.worker_of_shard(static_cast<int32_t>(sid))]++;
+    char row[192];  // fixed grammar, including full-width uint32/uint64 counters
+    for (uint32_t tid = 0; tid < srv.nthreads(); tid++) {
+        const ThreadCtx& thread = srv.thread(tid);
+        const bool active = thread.read_local_lane_active();
+        active_readers += active;
+        const Role role = thread.role();
+        const char* name = role == Role::Idle ? "idle"
+            : srv.thread_mode() == ThreadMode::Fused ? "unified"
+            : role == Role::Ifid ? "ifid" : "ex";
+        const ReadLocalStats& stats = thread.read_local_stats();
+        // Lifetime counters survive FLIP and RESETSTAT; INFO stats retains its existing
+        // RESETSTAT-relative aggregate counters.
+        const int n = std::snprintf(row, sizeof(row),
+            "read_local_thread_%u:role=%s,shards=%u,active=%u,"
+            "hits_total=%llu,mget_hits_total=%llu\r\n",
+            tid, name, owned[tid], active ? 1u : 0u,
+            static_cast<unsigned long long>(stats.hits),
+            static_cast<unsigned long long>(stats.mget_local_hits));
+        if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+        body.append(row, static_cast<size_t>(n));
+    }
+    const int n = std::snprintf(row, sizeof(row), "read_local_active_threads:%u\r\n",
+                              active_readers);
+    if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
+    body.append(row, static_cast<size_t>(n));
+}
+
 }  // namespace tomo
diff --git a/src/core/thread.h b/src/core/thread.h
index adea9605d0f0d40c3566e07b90a2663accb6b835..61802252934cbd450279ad7cd2dbaa8d5f03f465
--- a/src/core/thread.h
+++ b/src/core/thread.h
@@ -303,6 +303,8 @@
     std::atomic<bool> lane_active{false};
 };
 #if TOMO_READ_LOCAL_SET_TAX_VARIANT != 3
+static_assert(offsetof(ReadLocalThreadState, lane_active) == 360,
+              "lane activation must follow the entire pre-RL2S sidecar");
 static_assert(sizeof(ReadLocalThreadState) == 368,
               "lane activation adds one alignment word only to the optional sidecar");
 #endif

tokens used
153,975
Updated [RL2S.md — OFF-ARM PROOF](/home/user/Projects/cx-rl2s/RL2S.md:214) with all four answers and file:line evidence.

Fixed unconditional INFO diagnostics: their buffers, ownership walk and formatting are now enabled-only. All four off-arm properties hold at source level; optional sidecar growth is disclosed.

Source comparisons and `git diff --check` passed. No build, server, benchmark or gate was run.

Reading additional input from stdin...
OpenAI Codex v0.153.4
--------
workdir: /home/user/Projects/cx-final
model: gpt-6-astra
provider: openai
approval: never
sandbox: danger-full-access
reasoning effort: max
reasoning summaries: none
session id: 01a085e8-cf0b-7b60-9938-3467c5342b74
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

You are fixing three regressions that this worktree's own new tests report. You MAY build (from clean) and run
those batteries. Do NOT run the full gate; the maintainer runs it. Do not benchmark. Ports 8580-8609, cores
44-55, never a pattern kill, stop only your PIDs.

FAILING, all three added by the netcmd fix work now merged here:
    netcmd notify-retry regression   (see the battery it drives)
    netcmd flush regression
    netcmd output regression

These are YOUR OWN tests failing, which makes them different from an inherited flaky row and means one of two
things is true for each. Decide which, per row, with evidence:
  (a) The fix is incomplete or wrong, and the test is correctly reporting it. Fix the code.
  (b) The test asserts something that is not true of a correct server — a stricter invariant than the system
      guarantees, or a behaviour that changed legitimately elsewhere in this merged tree. Then fix the test,
      and say precisely what it was asserting and why that was wrong.
Do not assume (b) because it is easier. A test written alongside a fix usually encodes what the author believed
the fix did, so a failure often means the fix does less than intended.

CONTEXT THAT MATTERS: this tree is a ten-branch merge. Since these tests were written, the tree also gained a
restored overlap schedule, a renamed latency scheduler, the local-read lane extended to split mode, a large
vestigial-code deletion that removed members and a DEBUG subcommand, and a change to how resizes make progress.
Any of those can legitimately change what a notify, flush or output path does. So (b) is a real possibility —
but it must be demonstrated, not assumed. If another branch's change is responsible, name it.

CONSTRAINTS: do not weaken a row to make it pass, do not widen a tolerance, and do not delete a row. If a row
must change, it must still fail when the property it protects is broken, and you must say how to induce that.

Run each affected battery several times and report pass rates, not single runs.

DELIVERABLE: the fixes, plus NETCMDFIX.md with the (a)/(b) verdict per row, the evidence, what you changed, the
pass rates, and any row whose failure you could not attribute.
codex
I’ll trace all three failures, fix the code or test according to the evidence, and record each verdict in `NETCMDFIX.md`. I’ll use a separate worktree and run only the affected batteries on ports 8580–8609 and cores 44–55.

exec
/bin/bash -lc "pwd && git status --short && git branch --show-current && git rev-parse HEAD && rg --files -g 'AGENTS.md' -g '*netcmd*' -g '*NETCMD*' -g '*notify*' -g 'CMakeLists.txt' -g 'Makefile' -g 'meson.build' -g 'README*' -g 'gate.sh'" in /home/user/Projects/cx-final
 succeeded in 0ms:
/home/user/Projects/cx-final
 M CODEX-OUT.md
cx-final
67f623716ca00b49d7cbdb8d33ab6b643b3e87ad
third_party/lua/README.md
FIXES-NETCMD.md
Makefile
src/cmd/t_string_notify.cc
src/cmd/notify.h
src/cmd/notify.inc
tests/gate.sh
tests/netcmd.py
tests/netcmd_unit.cc
tests/notify.py
tests/netcmd_unit.h
tests/netcmd_stream_unit.cc
tests/netcmd_config_unit.cc
tests/netcmd_zset_unit.cc
scratchpad/robdiet/README.md
scratchpad/ringsize/README.md
scratchpad/rlbatch/README.md

exec
/bin/bash -lc 'for p in /AGENTS.md /home/AGENTS.md /home/user/AGENTS.md /home/user/Projects/AGENTS.md; do if test -f "$p"; then printf '"'%s\\n' \""'$p"; cat "$p"; fi; done' in /home/user/Projects/cx-final
 succeeded in 0ms:
exec
/bin/bash -lc "sed -n '1,280p' tests/netcmd.py && rg -n 'netcmd|notify.retry|flush regression|output regression' tests src && ls -ld /home/user/Projects/cx-final-netcmdfix* 2>/dev/null || true" in /home/user/Projects/cx-final
exec
/bin/bash -lc "cat Makefile && cat FIXES-NETCMD.md && rg -n -C 5 'netcmd|EXPECT_QUICK|EXPECT_FULL|quick.*exit|GATE_CORES|GATE_RATIO' tests/gate.sh" in /home/user/Projects/cx-final
 succeeded in 0ms:
#!/usr/bin/env python3
"""Network and command audit regressions, using an existing server (no launch/skip/rate tests)."""
import sys
from _lib import Conn, RespError, encode


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def run(host, port):
    c = Conn(host, port, timeout=5)
    try:
        c.raw(b"\r\n \t\r\n" + encode("PING", "after-empty"))
        require(c.read() == b"after-empty", "empty inline prefix consumed")
        for command in (("BOGUS",), ("GET",), ("XGROUP", "CREATE")):
            c.raw(encode("CLIENT", "REPLY", "SKIP") + encode(*command) + encode("PING", "visible"))
            require(c.read() == b"visible", "early error consumed SKIP exactly once")
            c.raw(encode("CLIENT", "REPLY", "OFF") + encode(*command) +
                  encode("CLIENT", "REPLY", "ON") + encode("PING", "after-on"))
            require(c.read() == b"OK" and c.read() == b"after-on", "early error obeyed OFF")
        c.raw(encode(b"bogus\r\n:123\r\n") + encode("PING", "framing"))
        require(isinstance(c.read(), RespError), "binary command rejected")
        require(c.read() == b"framing", "binary command cannot inject reply frames")
        for verb in (("XPENDING", b"absent\r\n:123\r\n", "g"),
                     ("XGROUP", "SETID", "absent", b"g\r\n:123\r\n", "0")):
            c.raw(encode(*verb) + encode("PING", "nogroup"))
            require(isinstance(c.read(), RespError), "NOGROUP error returned")
            require(c.read() == b"nogroup", "NOGROUP preserves frame boundaries")
        c.cmd("DEL", "netcmd:stream")
        require(c.cmd("XGROUP", "CREATE", "netcmd:stream", "g", "0", "MKSTREAM") == b"OK", "empty group exists")
        require(c.cmd("XAUTOCLAIM", "netcmd:stream", "g", "c", 0, 0, "COUNT", 9223372036854775807)
                == [b"0-0", [], []], "huge COUNT bounded by empty PEL")
        c.raw(encode("XPENDING", "netcmd:stream", "g", "(18446744073709551615-18446744073709551615", "+", 1) +
              encode("PING", "pending"))
        require(c.read() == [] and c.read() == b"pending", "exclusive max XPENDING has an empty reply")

        for source in ("return {ok='OK\\r\\n:42'}", "return {err='ERR bad\\r\\n:42'}"):
            c.raw(encode("EVAL", source, 0) + encode("PING", "lua-framing"))
            result = c.read()
            require(isinstance(result, (bytes, RespError)), "Lua status/error converted")
            require(c.read() == b"lua-framing", "Lua table cannot inject frames")
        for body in ("error('conversion failed')", "while true do end"):
            source = "return setmetatable({}, {__index=function() " + body + " end})"
            require(c.cmd("EVAL", source, 0) == [], "conversion uses raw table fields without executing __index")
        require(isinstance(c.cmd("EVAL", "local ok=pcall(function() while true do end end); return 42", 0), RespError),
                "catching instruction-limit error cannot turn activation into success")
        print("ok: protocol, suppression, stream bounds, Lua conversion/budget")

        before = c.cmd("CONFIG", "GET", "proto-max-bulk-len")[1]
        try:
            require(c.cmd("CONFIG", "SET", "proto-max-bulk-len", 1048576) == b"OK", "runtime bulk limit set")
            require(c.cmd("CONFIG", "GET", "proto-max-bulk-len")[1] == b"1048576", "runtime bulk limit confirmed")
            fresh = Conn(host, port, timeout=10)
            try:
                value = b"v" * (600 * 1024)
                require(fresh.cmd("MSET", "netcmd:a", value, "netcmd:b", value) == b"OK", "two legal bulks exceed one-bulk receive cap")
                require(fresh.cmd("STRLEN", "netcmd:a") == len(value), "first legal bulk stored")
                require(fresh.cmd("STRLEN", "netcmd:b") == len(value), "second legal bulk stored")
                for request in (("SETRANGE", "netcmd:grow", 1048576, "x"),
                                ("SETBIT", "netcmd:grow", 8388608, 1),
                                ("BITFIELD", "netcmd:grow", "SET", "u8", 8388608, 1),
                                ("APPEND", "netcmd:a", value)):
                    require(isinstance(fresh.cmd(*request), RespError), "runtime growth limit rejects " + request[0])
                require(fresh.cmd("EXISTS", "netcmd:grow") == 0, "rejected growth did not create key")
                require(fresh.cmd("STRLEN", "netcmd:a") == len(value), "rejected append preserved value")
            finally:
                fresh.close()
        finally:
            require(c.cmd("CONFIG", "SET", "proto-max-bulk-len", before) == b"OK", "runtime bulk limit restored")
        print("ok: whole-command reception and runtime string/bitmap limits")

        t = Conn(host, port, timeout=5)
        try:
            require(c.cmd("XADD", "netcmd:tracked", "*", "f", "v") is not None, "tracked stream exists")
            t.cmd("HELLO", 3)
            require(t.cmd("CLIENT", "TRACKING", "ON") == b"OK", "ordinary RESP3 tracking armed")
            t.raw(encode("XREAD", "STREAMS", "netcmd:tracked", "bad-id") + encode("PING", "tracking-parser"))
            require(isinstance(t.read(), RespError) and t.read() == b"tracking-parser",
                    "tracking registration does not emit an extra parser error")
            require(t.cmd("XREAD", "STREAMS", "netcmd:tracked", "0") is not None, "XREAD delivered existing stream")
            c.cmd("XADD", "netcmd:tracked", "*", "f", "next")
            require(t.read() == [b"invalidate", [b"netcmd:tracked"]], "XREAD alone registered its dynamic stream key")
            require(c.cmd("SET", "netcmd:flush", "old") == b"OK", "flush source exists")
            require(t.cmd("GET", "netcmd:flush") == b"old", "flush source is tracked")
            require(c.cmd("FLUSHALL") == b"OK", "owner flush completed")
            require(t.read() == [b"invalidate", None], "completed flush invalidated tracking client")
            require(t.cmd("GET", "netcmd:flush") is None, "read after invalidation observes clear")
        finally:
            t.close()
        print("ok: XREAD tracking without BCAST or a prior static-key read")
    finally:
        c.close()


if __name__ == "__main__":
    run(sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1", int(sys.argv[2]) if len(sys.argv) > 2 else 7899)
tests/netcmd_config_unit.cc:1:#include "netcmd_unit.h"
tests/netcmd.py:31:        c.cmd("DEL", "netcmd:stream")
tests/netcmd.py:32:        require(c.cmd("XGROUP", "CREATE", "netcmd:stream", "g", "0", "MKSTREAM") == b"OK", "empty group exists")
tests/netcmd.py:33:        require(c.cmd("XAUTOCLAIM", "netcmd:stream", "g", "c", 0, 0, "COUNT", 9223372036854775807)
tests/netcmd.py:35:        c.raw(encode("XPENDING", "netcmd:stream", "g", "(18446744073709551615-18446744073709551615", "+", 1) +
tests/netcmd.py:58:                require(fresh.cmd("MSET", "netcmd:a", value, "netcmd:b", value) == b"OK", "two legal bulks exceed one-bulk receive cap")
tests/netcmd.py:59:                require(fresh.cmd("STRLEN", "netcmd:a") == len(value), "first legal bulk stored")
tests/netcmd.py:60:                require(fresh.cmd("STRLEN", "netcmd:b") == len(value), "second legal bulk stored")
tests/netcmd.py:61:                for request in (("SETRANGE", "netcmd:grow", 1048576, "x"),
tests/netcmd.py:62:                                ("SETBIT", "netcmd:grow", 8388608, 1),
tests/netcmd.py:63:                                ("BITFIELD", "netcmd:grow", "SET", "u8", 8388608, 1),
tests/netcmd.py:64:                                ("APPEND", "netcmd:a", value)):
tests/netcmd.py:66:                require(fresh.cmd("EXISTS", "netcmd:grow") == 0, "rejected growth did not create key")
tests/netcmd.py:67:                require(fresh.cmd("STRLEN", "netcmd:a") == len(value), "rejected append preserved value")
tests/netcmd.py:76:            require(c.cmd("XADD", "netcmd:tracked", "*", "f", "v") is not None, "tracked stream exists")
tests/netcmd.py:79:            t.raw(encode("XREAD", "STREAMS", "netcmd:tracked", "bad-id") + encode("PING", "tracking-parser"))
tests/netcmd.py:82:            require(t.cmd("XREAD", "STREAMS", "netcmd:tracked", "0") is not None, "XREAD delivered existing stream")
tests/netcmd.py:83:            c.cmd("XADD", "netcmd:tracked", "*", "f", "next")
tests/netcmd.py:84:            require(t.read() == [b"invalidate", [b"netcmd:tracked"]], "XREAD alone registered its dynamic stream key")
tests/netcmd.py:85:            require(c.cmd("SET", "netcmd:flush", "old") == b"OK", "flush source exists")
tests/netcmd.py:86:            require(t.cmd("GET", "netcmd:flush") == b"old", "flush source is tracked")
tests/netcmd.py:89:            require(t.cmd("GET", "netcmd:flush") is None, "read after invalidation observes clear")
tests/gate.sh:393:pausable make -j2 build/netcmd-unit >/tmp/gate-netcmd-build.txt 2>&1 \
tests/gate.sh:394:    && ok "netcmd regression build" || bad "netcmd regression build" "see /tmp/gate-netcmd-build.txt"
tests/gate.sh:395:for NETCMD_CASE in streams zpop notify-oom notify-retry flush output pubsub receive config; do
tests/gate.sh:396:  timeout 60 ./build/netcmd-unit "$NETCMD_CASE" >/tmp/gate-netcmd-$NETCMD_CASE.txt 2>&1 \
tests/gate.sh:397:      && ok "netcmd $NETCMD_CASE regression" \
tests/gate.sh:398:      || bad "netcmd $NETCMD_CASE regression" "see /tmp/gate-netcmd-$NETCMD_CASE.txt"
tests/gate.sh:567:FEATURE_BATTERIES="s6 multi_exec blocking blockmulti stream streamgroups pubsub lua_scripting scriptsurf limits resp3 bitfield dumprestore zsetops geo climon climon2 tracking hexpire servertail lcs concur edgeproto edgeenc edgetime arity contarity cmdgap aclsel expwide infofix pushtear netcmd"
tests/netcmd_zset_unit.cc:1:#include "netcmd_unit.h"
tests/netcmd_zset_unit.cc:17:        const auto failures = netcmd_failures, allocations = netcmd_allocations;
tests/netcmd_unit.h:12:extern long netcmd_fail_after;
tests/netcmd_unit.h:13:extern size_t netcmd_fail_size;
tests/netcmd_unit.h:14:extern unsigned netcmd_allocations, netcmd_failures;
tests/netcmd_unit.h:19:    netcmd_fail_after = after; netcmd_fail_size = size;
tests/netcmd_unit.h:20:    netcmd_allocations = netcmd_failures = 0;
tests/netcmd_stream_unit.cc:3:#include "netcmd_unit.h"
tests/netcmd_stream_unit.cc:18:    const unsigned failures = netcmd_failures;
tests/netcmd_unit.cc:1:#include "netcmd_unit.h"
tests/netcmd_unit.cc:10:long netcmd_fail_after = -1;
tests/netcmd_unit.cc:11:size_t netcmd_fail_size = 0;
tests/netcmd_unit.cc:12:unsigned netcmd_allocations = 0, netcmd_failures = 0;
tests/netcmd_unit.cc:14:    if (netcmd_fail_after >= 0 && (!netcmd_fail_size || netcmd_fail_size == size)) {
tests/netcmd_unit.cc:15:        ++netcmd_allocations;
tests/netcmd_unit.cc:16:        if (netcmd_fail_after-- == 0) { ++netcmd_failures; throw std::bad_alloc(); }
tests/netcmd_unit.cc:51:        const auto failed = netcmd_failures; fault(-1);
tests/netcmd_unit.cc:61:    static void notify_retry() {
tests/netcmd_unit.cc:222:        const unsigned failures = netcmd_failures; fault(-1);
tests/netcmd_unit.cc:232:        char directory[] = "build/netcmd-config-XXXXXX";
tests/netcmd_unit.cc:252:    else if (mode == "notify-retry") R::notify_retry();
tests/netcmd_unit.cc:260:    std::printf("ok: netcmd %s\n", argv[1]);

exec
/bin/bash -lc "git worktree list && git log -18 --oneline --decorate && git ls-files '*AGENTS.md' && ls -la" in /home/user/Projects/cx-final
 succeeded in 0ms:
CXX      ?= g++
JEDIR    ?= /home/user/Projects/refs/jemalloc/_install
# jemalloc is optional: without it the tree still builds and runs, using the deterministic portable
# size-class table in alloc.h. Set JE=0 to force that path (useful for A/B-ing the allocator).
JE       ?= 1
ifeq ($(JE),1)
  # Prefer the system package; fall back to the copy built from refs/.
  ifneq ($(wildcard /usr/include/jemalloc/jemalloc.h),)
    JEFLAGS := -DTOMO_JEMALLOC
    JELIBS  := -ljemalloc
  else
    JEFLAGS := -DTOMO_JEMALLOC -I$(JEDIR)/include
    JELIBS  := $(JEDIR)/lib/libjemalloc.a -ldl
  endif
else
  JEFLAGS :=
  JELIBS  :=
endif
CXXFLAGS ?= -std=c++20 -O2 -g -Wall -Wextra -march=native -pthread
LDLIBS   ?= -luring -pthread
SRC      := src/main.cc src/net/tls.cc src/cmd/commands.cc src/cmd/glob.cc src/cmd/xshard.cc src/cmd/acl.cc src/cmd/hll.cc src/cmd/t_server.cc src/cmd/t_string.cc src/cmd/t_string_notify.cc src/cmd/t_hash.cc src/cmd/t_hash_ttl.cc \
            src/cmd/t_list.cc src/cmd/t_set.cc src/cmd/t_zset.cc src/cmd/t_zset_ops.cc src/cmd/geo.cc src/cmd/t_stream.cc src/cmd/t_stream_groups.cc src/cmd/scripting.cc src/cmd/functions.cc src/cmd/serialize.cc src/snapshot/snapshot.cc src/persist/aof.cc
SRC      += src/cmd/climon.cc src/cmd/tracking.cc
SRC      += src/cmd/server_tail.cc src/cmd/slowlog.cc src/cmd/lcs.cc src/cmd/info_stats.cc
SRC      += src/cmd/lbsignals.cc
SRC      += src/core/flipctl.cc
SRC      += src/core/genthread.cc
SRC      += src/core/rl2s.cc
SRC      += src/cmd/cmdgap.cc
SRC      += src/cmd/pfdebug.cc
SRC      += src/cmd/cmdmeta.cc
SRC      += src/cmd/t_sort.cc
LDLIBS   += -lssl -lcrypto
BIN      := build/tomokv
OBJ      := $(SRC:%.cc=build/%.o)

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) $(OBJ) -o $@ $(JELIBS) $(LDLIBS) -lm

# The clean string family intentionally excludes the armed instantiations, but its parsed template
# bodies still move GCC just past the default large-unit threshold. 10600 restores the same inlining
# decisions as the base-420b4d492 translation unit; the objdump gate locks cmd_get/cmd_set to base.
build/src/cmd/t_string.o: CXXFLAGS += --param large-unit-insns=10600

build/%.o: %.cc $(wildcard src/*/*.h) $(wildcard src/*/*.inc) $(wildcard third_party/lua/*) Makefile
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(JEFLAGS) -I. -c $< -o $@

asan: CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -O1
asan:
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I. $(SRC) -o build/tomokv-asan $(LDLIBS) -lm

tsan:
	@mkdir -p build
	$(CXX) -std=c++20 -O1 -g -Wall -Wextra -pthread -fsanitize=thread \
	  -I. $(SRC) -o build/tomokv-tsan $(LDLIBS) -lm

# The armed-write block cache's ownership laws as assertions: the cache and the QSBR retire ring
# are owner-private (one thread, no lock), each cached block is resident exactly once, each class
# list matches its counter, and every shard's retire sink names its CURRENT owner. Debug-only: the
# residency set and the sampled list walk cost far more than the path they guard. DESIGN-P0REPLY.md.
rlcachedbg:
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(JEFLAGS) -DTOMO_RL_CACHE_DEBUG -I. $(SRC) \
	  -o build/tomokv-rlcachedbg $(JELIBS) $(LDLIBS) -lm

# NEGATIVE-CONTROL BUILD for the row above: the same assertions with the ownership-edge rebind
# removed, i.e. the pre-fix behaviour. tests/rlcache_churn.py MUST fail against this binary; a
# detector that cannot report failure proves nothing about the runs that pass.
rlcache-nofix:
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(JEFLAGS) -DTOMO_RL_CACHE_DEBUG -DTOMO_RL_CACHE_NO_EAGER_ADOPT -I. $(SRC) \
	  -o build/tomokv-rlcache-nofix $(JELIBS) $(LDLIBS) -lm

# NEGATIVE-CONTROL BUILD for the cross-owner script reservation sub-wave. Identical to the release
# build except that ScriptPhase::Pin arms nothing, so tests/xscript.py counterexample MUST fail
# against it. A detector that cannot report failure proves nothing about the runs that pass.
noreserve:
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(JEFLAGS) -DTOMO_XSCRIPT_NO_RESERVE -I. $(SRC) \
	  -o build/tomokv-noreserve $(JELIBS) $(LDLIBS) -lm

# Server-less unit binaries: the config parser and the flip controller. `make unit` builds and
# runs both (neither boots a server). tests/gate.sh's parser row is the same program.
build/config-parser-test: tests/config_parser_test.cc $(wildcard src/*/*.h) Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I. tests/config_parser_test.cc -o $@
build/flipctl-unit: tests/flipctl_unit.cc src/core/flipctl.cc $(wildcard src/*/*.h) Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I. tests/flipctl_unit.cc src/core/flipctl.cc -o $@
# The fused owner's deferred-reclaim ring (QSBR batches) against a per-entry reference model.
build/read-local-ring-unit: tests/read_local_ring_unit.cc $(wildcard src/*/*.h) Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I. tests/read_local_ring_unit.cc -o $@
build/read-local-write-ring-unit: tests/read_local_write_ring_unit.cc $(wildcard src/*/*.h) Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I. tests/read_local_write_ring_unit.cc -o $@
# The production cross-connection scheduler, with real ROB tasks at both executor capacities.
build/reorder-unit: tests/reorder_unit.cc $(wildcard src/*/*.h) Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I. tests/reorder_unit.cc -o $@
STORE_REGRESSION_SRC := tests/store_regression.cc src/cmd/t_hash.cc src/cmd/t_hash_ttl.cc
build/store-regression: $(STORE_REGRESSION_SRC) $(wildcard src/*/*.h) $(wildcard src/*/*.inc) Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -ffunction-sections -fdata-sections -DTOMO_STORE_REGRESSION_TEST -I. \
	  $(STORE_REGRESSION_SRC) -Wl,--gc-sections -o $@
build/store-regression-sidecar: $(STORE_REGRESSION_SRC) $(wildcard src/*/*.h) $(wildcard src/*/*.inc) Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -ffunction-sections -fdata-sections -DTOMO_STORE_REGRESSION_TEST \
	  -DTOMO_TTL_DEADLINE_SIDECAR=1 -I. $(STORE_REGRESSION_SRC) -Wl,--gc-sections -o $@
build/store-regression-tsan: $(STORE_REGRESSION_SRC) $(wildcard src/*/*.h) $(wildcard src/*/*.inc) Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -O1 -fsanitize=thread -fno-omit-frame-pointer -no-pie \
	  -ffunction-sections -fdata-sections -DTOMO_STORE_REGRESSION_TEST -I. \
	  $(STORE_REGRESSION_SRC) -Wl,--gc-sections -o $@
build/waits-unit: tests/waits_unit.cc $(wildcard src/*/*.h) Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I. tests/waits_unit.cc -o $@
unit: build/config-parser-test build/flipctl-unit build/read-local-ring-unit build/read-local-write-ring-unit build/reorder-unit build/waits-unit
	./build/config-parser-test
	./build/flipctl-unit
	./build/read-local-ring-unit
	./build/read-local-write-ring-unit
	./build/reorder-unit
	./build/waits-unit

# Deterministic core regressions: the test TU instantiates the real executor/IO methods
# with ASAN/UBSAN and test-only interleaving hooks. No server or ring is started.
CORE_TEST_OBJ := $(filter-out build/src/main.o build/src/core/genthread.o,$(OBJ))
build/rehash-waits-unit: tests/rehash_waits_unit.cc $(CORE_TEST_OBJ) $(wildcard src/*/*.h) Makefile
	$(CXX) $(CXXFLAGS) $(JEFLAGS) -I. $< $(CORE_TEST_OBJ) -o $@ $(JELIBS) $(LDLIBS) -lm

build/core-concurrency-unit: tests/core_concurrency_unit.cc $(CORE_TEST_OBJ) $(wildcard src/*/*.h)
	$(CXX) $(CXXFLAGS) $(JEFLAGS) -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -DTOMO_CORE_CONCURRENCY_TEST -I. $< $(CORE_TEST_OBJ) -o $@ \
	  $(JELIBS) $(LDLIBS) -lm

# Directed owner-phase tests. The test includes xshard.cc to drive the real private phases
# without starting worker threads or opening a listener; all other code is the release objects.
build/atomic-survivors-unit: tests/atomic_survivors_unit.cc src/cmd/xshard.cc $(filter-out build/src/main.o build/src/cmd/xshard.o,$(OBJ)) $(wildcard src/*/*.inc) $(wildcard src/*/*.h) Makefile
	$(CXX) $(CXXFLAGS) $(JEFLAGS) -I. tests/atomic_survivors_unit.cc \
	  $(filter-out build/src/main.o build/src/cmd/xshard.o,$(OBJ)) -o $@ $(JELIBS) $(LDLIBS) -lm

# Load drivers: not part of `all`, kept compiling here so they cannot rot unnoticed.
build/benchtxn: tools/benchtxn.cc Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I. tools/benchtxn.cc -o $@
build/broaden-bench: tests/broaden_bench.cc Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I. tests/broaden_bench.cc -o $@
tools: build/benchtxn build/broaden-bench

clean:
	rm -rf build
.PHONY: all asan tsan noreserve clean unit tools

# Deterministic networking/command regressions. These TUs include the real private implementations;
# omit their production objects from this serverless binary. No server threads or gate are started.
NETCMD_TEST_SRC := tests/netcmd_unit.cc tests/netcmd_stream_unit.cc tests/netcmd_zset_unit.cc tests/netcmd_config_unit.cc
NETCMD_TEST_OBJ := $(NETCMD_TEST_SRC:tests/%.cc=build/tests/%.o)
NETCMD_LIB_OBJ := $(filter-out build/src/main.o build/src/cmd/xshard.o build/src/cmd/t_stream_groups.o build/src/cmd/t_zset.o build/src/cmd/server_tail.o,$(OBJ))
build/tests/%.o: tests/%.cc tests/netcmd_unit.h $(wildcard src/*/*.h) $(wildcard src/*/*.cc) $(wildcard src/*/*.inc) Makefile
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(JEFLAGS) -Wno-mismatched-new-delete -I. -c $< -o $@
build/netcmd-unit: $(NETCMD_TEST_OBJ) $(NETCMD_LIB_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(JELIBS) $(LDLIBS) -lm -Wl,--wrap=mkstemp -Wl,--wrap=fopen
# Networking and command fixes

`SURVIVING.md` repeats its list. IDs below refer to its numbered findings, counted once.
This diff fixes 28 findings. Two additional findings remain open, with runnable failing
reproducers. Concurrency, storage, transaction-engine and boot-parser findings are left to
those lanes; the shared boundaries are listed below.

## Fixed findings and negative controls

`live` means `tests/netcmd.py`, added to the shared feature-battery list. `unit NAME` means
`./build/netcmd-unit NAME`. The unit binary includes the actual private command implementations
and omits their duplicate production objects at link time. Its allocation faults and file-open
barrier exist only in that executable. None of the selected regressions has a skip path.

| Finding | Change | Regression and how to make it fail |
|---|---|---|
| **6 — S cmdsem F15, runtime string limit (MISPRICED)** | APPEND, SETRANGE, bit offsets and BITFIELD use the published runtime bulk limit. BITFIELD checks the last accessed bit too. The atomic limit load stays in those existing cold bounds checks; GET/SET acquire no new load. | `live` confirms CONFIG SET/GET of 1 MiB, rejects growing past it, and verifies unchanged values/absent destination. Restore the private 512 MiB constants: these operations succeed and the row fails. |
| **14 — C cmdsem F01, XAUTOCLAIM** | Reserve at most the actual PEL size, not an arbitrary COUNT. | `live` creates an empty group and uses COUNT 9223372036854775807, requiring `[0-0, [], []]`. Restore `reserve(count)`: length_error terminates the server. |
| **15 — C cmdsem F02, notification double destruction** | Remove the catch that repeats the shared_ptr constructor's deleter and reservation release. | `unit notify-oom` fails exactly the control-block allocation after creating a nonempty chain, asserts that failure fired, and checks the reservation is zero. Its success control checks 0→1→0. Restore the catch: double free or reservation-underflow abort. |
| **19 — C deadconf F01 / S atomics 8, Lua conversion** | Inspect result/error-table fields with raw Lua lookups, including nested verbatim fields. Conversion cannot invoke `__index`. | `live` returns metatables whose `__index` either throws or loops forever, requiring an empty array. Restore metamethod-aware lookups: crash or bounded client timeout. |
| **20 — C netproto 2, disabled output limits** | Every untracked Wb specialization stops stale client accounting before staging uncounted bytes, including TLS preparation. | `unit output` starts with tracking armed and a drained zero balance, disables the global selector, stages a seven-byte reply through the real Wb prepare path, and completes its send. Remove the stop: subtraction from zero aborts, independently of cron timing. |
| **27 — S atomics 13 / S deadconf F07, Lua reply injection** | Sanitize CR/LF in Lua `ok` and `err` simple-frame values. | `live` pipelines each injected table and a uniquely labelled PING, requiring exactly the intended two responses. Restore direct append: injected integer 42 replaces the marker response. |
| **30 — S cmdsem F03, XREADGROUP delivery progress** | Allocate all PEL nodes/consumer strings privately, then transfer nodes or swap existing values without allocation. Advance the cursor only after successful preparation. | `unit streams` proves two entries and an existing consumer, fails the second libstdc++ PEL-node allocation, and requires an unchanged cursor and empty PEL. A retry must deliver both entries and leave two pending records. Restore incremental insertion/cursor advance: the state assertions fail. |
| **33 — S cmdsem F06, compact ZPOP** | Pass the already loaded compact index to rank erasure. A successful reply no longer precedes a second fallible load. | `unit zpop` proves compact encoding and exercises MIN/MAX. Failing the first load must return an error without removal. Arming failure for a second allocation must find only the first load, return the member, and remove it. Restore the second load: the allocation/state checks fail. |
| **34 — S cmdsem F07, stream-group observation** | XGROUP's successful CREATE/DESTROY/CREATECONSUMER/DELCONSUMER/SETID branches and XSETID record their stream events. Their armed registry entries install notification context. | `unit streams` arms only the save observer on an existing stream/group, creates a new consumer, and requires exactly one save change. Remove the CREATECONSUMER record or armed wrapper: the consumer exists but the save count stays unchanged. |
| **35 — S cmdsem F09, premature FLUSH invalidation** | Move tracking flush publication from dispatch to the completed scatter retirement hook, after all owner fragments finish. | `unit flush` sends the actual dispatch gate through a two-IO fixture and requires no foreign event before owner work, then requires TrackingFlush at completion. Restore early publication: the first assertion fails. `live` also requires an invalidation after FLUSHALL and an absent key on the following tracked GET; remove the completion call and it times out. |
| **36 — S cmdsem F10, dropped invalidation** | Keep the keyless record on failed allocation/post. Retries re-sample the current IO destinations so migration cannot strand the record or miss a moved tracker; previously delivered invalidations may conservatively repeat. | `unit notify-retry` fills the actual producer marker lane while no pubsub burst is notified, proves the failed post enqueued nothing and retained the record, drains the lane, then requires the exact invalidation and record completion. Restore delete-and-continue: the record is consumed prematurely. |
| **37 — S cmdsem F11, XREAD tracking** | Use XREAD's dynamic STREAMS parser to register its key half. Parse into a scratch Op so tracking cannot append a second error to a malformed command's reply. | `live` arms ordinary RESP3 tracking, performs XREAD without BCAST or a preceding static-key read, appends from another connection, and requires the exact invalidation. Restore the static first_key early return: no invalidation arrives. Malformed XREAD plus PING also checks that registration emits no extra error frame. |
| **39 — S cmdsem F13, empty XPENDING reply** | Parse both bounds/count first and explicitly emit an empty array when incrementing the exclusive maximum start ID overflows. | `live` pipelines that exact boundary and a labelled PING, requiring `[]` before the marker. Restore the silent false return: the PING is mistaken for XPENDING's reply. |
| **40 — S cmdsem F14, NOGROUP framing** | Both NOGROUP builders sanitize binary key/group line delimiters. | `live` exercises missing-key XPENDING and missing-group XGROUP with embedded CR/LF plus a following PING. Restore raw append: an injected/malformed frame consumes the marker position. |
| **45 — S deadconf F02, rewrite loses boot settings** | Preserve original directives absent from the runtime table, including repeatable inline users and `load`, while replacing the runtime-owned directives. | `unit config` starts with inline ACL and a recovery path containing spaces, rewrites, and requires both directives and a loadable token stream. Remove preservation: the directives disappear. |
| **46 — S deadconf F03, rewrite string round trip** | Quote/escape values that need it; preserve simple scalar spelling and the multi-token output-limit/save grammars. | `unit config` round-trips spaces, newline, CR, tab, quotes and backslash in the password through the actual config lexer/loader. Remove escaping: its one-token/value assertions fail. Existing `servertail.py` also retains its unchanged scalar-format and restart checks. |
| **47 — S deadconf F04, rewrite inode collision** | Use mkstemp for a private temporary inode per rewrite; each caller renames only its own file. | `unit config` wraps mkstemp and fopen at link time. Two writers stop after opening until both are present, then must have different inodes, both succeed, and preserve the complete expected file. Restore the shared `.rewrite.tmp` fopen: the inode assertion fails deterministically. |
| **50 — S deadconf F09, catchable instruction limit** | Treat `timed_out` as activation failure even when Lua's outer pcall returns success. | `live` catches the inner runaway-loop error and returns 42; the outer activation must still return an error. Restore status-only completion: 42 is returned. |
| **52 — S netproto 3, suppression bypasses TLS** | The cold suppression serve prepares bytes and selects the TLS pump for userspace TLS, retaining poll/error handling. | `tests/tls.py`'s handshake matrix now runs SKIP;PING;PING and another labelled PING after proving each negotiated version and the fallback counters. The existing fallback gate boot forces TLS1.2 CBC, so this cannot pass solely through kTLS encryption. Restore the plaintext pump: SSL record/framing checks fail. |
| **53 — S netproto 4, early errors escape suppression** | Apply the existing mark to early local completions that precede spec selection/ordinary marking. | `live` exercises unknown command, GET arity and XGROUP subcommand arity under both OFF and SKIP, with following ON/PING markers. Remove early marking: an error escapes or consumes the wrong position. |
| **54 — S netproto 5, PUBSUB inspection loses messages** | Append pending deferred publications for inspection/publish completions as well as subscription controls. | `unit pubsub` seeds an existing subscription and unfinished CHANNELS/NUMSUB/NUMPAT control, delivers a publication, asserts the deferred state is nonempty, and requires that frame after completion. Remove the inspection append: the frame vanishes. |
| **55 — S netproto 6, marked control swallows pushes** | Move a suppressed control's independent deferred publications to Wb's ordered OOB queue before discarding its command reply. | `unit pubsub` repeats the pending-state test with marked operations, including SUBSCRIBE, and requires exactly the independent message after suppressing retirement. Embed it in the marked Op again: retirement discards it. |
| **56 — S netproto 7, aggregate segment narrowing** | Split the two-piece overload using the same segment-length bound as the single-piece overload. | `unit receive` instantiates the real queue with an eight-byte segment bound, appends 11+8 bytes and requires three bounded segments and all 19 ordered bytes. Restore the unsplit overload: segment-count/bound assertions fail. This exercises the mechanism without allocating a >4 GiB list. The production bound remains UINT32_MAX. |
| **57 — S netproto 8, whole-command receive cap** | Distinguish the per-bulk parser bound from the 32-bit whole-command receive bound. Growth still requires ROB quiescence; an incomplete frame at the representation ceiling gets an explicit error. | `live` sets a 1 MiB bulk limit on a fresh connection and successfully sends/stores two legal 600 KiB MSET values. `unit receive` also requires growth beyond one bulk and forbids growth with outstanding slices. Restore a one-bulk whole-buffer cap: growth/response fails. |
| **58 — S netproto 9, epoll's default allowance** | Epoll passes the live limit, and the shared receive-cap correction above removes dependence on a stale per-bulk default altogether. | The directed live battery passed on epoll. `unit receive` reserves a 600 MiB receive allowance with the default argument and the 1 GiB setting without touching that payload. Restore read_space's old default-based 512 MiB ceiling: the default-allowance assertion fails. This tests the shared allowance directly; the live epoll battery does not send a 600 MiB payload. |
| **59 — S netproto 10, empty inline parsing** | Return a distinct Empty result and commit the consumed prefix without publishing an operation or consuming reply suppression. | `live` sends CRLF and whitespace-only lines before a valid RESP command. Restore Incomplete: the labelled reply times out. |
| **60 — S netproto 11, binary command-name framing** | The shared error formatter strips CR/LF from line text, covering unknown-command names and echoed arguments. | `live` uses an unknown bulk command containing an apparent integer frame and checks the following marker. Restore unsanitized reply_err: an extra frame is read. |
| **62 — S netproto 13, deferred output accounting** | Include Wb OOB queues and pending pubsub publications in hard/soft limit input. Deferred storage is inspected only after the chosen client's limit is enabled. | `unit output` holds an ordinary nonblocking ROB entry before Done, proves all nine bytes exist only in OOB deferral, and requires an eight-byte hard limit to close it before completion. Omit deferred bytes from the check: it does not fire. |

## Open, with failing reproducers

- **32 — S cmdsem F05: partial collection mutation on error. NOT FIXED.** Existing
  expanded hash/set/list/zset updates and field-TTL operations do not have one shared
  failure-atomic preparation/publication contract. Correcting only HSET's late TTL clear or
  notification would leave the prefix mutation and the other collection paths. A rollback
  that allocates can fail under the same condition. This diff does not claim to solve that.
  `./build/netcmd-unit collection-oom` is a diagnostic, outside the passing gate loop. It
  proves expanded encoding, arms `a`'s field TTL, fails the second 256-byte replacement, and
  checks the old value. **Run here: it failed at the changed-prefix assertion after proving
  exactly one injected allocation failure.** The same injector can verify a future fix.
- **61 — S netproto 12: kTLS KeyUpdate. NOT FIXED.** The raw receive path needs ancillary
  control-record handling and coordinated receive/transmit rekeying; changing an errno
  branch cannot supply those protocols. TLS1.3 kTLS remains enabled and its existing
  engagement checks are unchanged. The kernel's [control-record and rekey contract](https://docs.kernel.org/networking/tls.html)
  explains why this requires more than ignoring the receive error. Build the diagnostic with
  `g++ -std=c++20 tests/ktls_keyupdate.cc -o build/ktls-keyupdate -lssl -lcrypto`, then run
  `build/ktls-keyupdate 127.0.0.1 TLS_PORT` on a default AES-128 TLS1.3 boot. It requires its
  sole TLS connection to report active kTLS before sending a legal requested KeyUpdate.
  **Run here: kTLS arming passed, then the post-update TLS read failed.** No skip or userspace
  fallback counts as a pass. This open diagnostic is not added to the passing gate loop.

## Other lane boundaries and disputes

No findings were disputed. In particular, these shared command/network-facing findings remain
real and were not silently counted as fixed:

- **3 / cmdsem F08**: WATCH dirtying after unsuccessful writes is rooted in ordinary executor
  completion and transaction observation; the transaction/concurrency lane owns it.
- **4 / deadconf F08**: reading INFO/RESETSTAT counters safely requires changing the plain
  counter writers too; a cmd-only atomic read would retain the race.
- **17 / concurrency 3 + netproto 1**: the Done-to-notify Client lifetime belongs to the
  executor/IO reclamation protocol. This diff does not add or claim an executor-tail reference.
- **31 / cmdsem F04 + storage 10** and **38 / cmdsem F12 + storage 12**: snapshot-safe field
  expiry and both lazy/active hash accounting are storage-lane work. Neither half was patched
  independently here.
- **48–49 / deadconf F05–F06**: boot-password NUL and missing-value validation are in the config
  parser. Rewriter quoting is not a fix for either boot-parser defect.

The remaining MVCC, transaction, script/function-library lifecycle, AOF, storage, scheduler,
migration and read-local retry findings are outside this lane. Core edits in this diff are
restricted to networking integration and a zero-layout-cost test friend declaration.

## Gate accounting

No EXPECT constant was edited. The new rows are all **above** the quick-tier exit:

- One `netcmd regression build` row in the static section.
- Nine rows from `for NETCMD_CASE in streams zpop notify-oom notify-retry flush output pubsub receive config`.
- Four `netcmd` feature rows: two atomic settings in the split loop and two in the fused/armed loop.

Thus **+14 quick and +14 full**. From the documented inherited **327 quick / 344 full**
program, the expected totals become **341 quick / 358 full**, without the optional NIC row.
The inherited literal constants are already 325/342 (two below that program); simply adding
14 to those stale literals would still be wrong. The TLS additions strengthen an existing row
and add no ledger row. The two open diagnostics add no passing row.

## Validation and limits

- Release build and the serverless unit build succeeded with existing footprint assertions.
  No locked size or assertion changed. No Op, Client, ThreadCtx, Shard, FlatStore, Rob,
  AtomicEntry or Config layout changes.
- All nine selected serverless sections passed. Their tests force and assert their relevant
  allocation, queue, completion or file-open states; there are no timing-luck passes.
- The new live battery passed on split/io_uring, split/epoll, fused, and fused with read-local
  armed, with both atomic settings represented. Split boots used cores 0–7, 16 shards, 6:2;
  fused used the same cores/shards and correctly omitted the unsupported ratio option.
- Existing streamgroups, tracking, climon2, limits, lua_scripting, bitfield, servertail,
  zsetops and edgeproto batteries were exercised on split and fused/armed configurations.
  A scalar-format regression found by servertail was corrected in the writer; its test was
  not weakened.
- The TLS1.2/TLS1.3 fallback handshake/suppression matrix passed in both thread modes.
- The two open diagnostics failed as described above. No full gate, benchmark or performance
  comparison was run. Only individually owned test-server PIDs were stopped.

Performance and the maintainer's full gate remain unmeasured here. These are correctness fixes;
there is no PRE/POST performance claim. The user-owned CODEX-OUT.md change and duplicated
SURVIVING.md input are not part of these edits.
24-# testing nothing is worse than no gate.
25-set -u
26-cd "$(dirname "$0")/.."
27-TIER=${1:-quick}
28-PORT=${GATE_PORT:-7899}
29:CORES=${GATE_CORES:-0-7}
30-NCORES=$(taskset -c "$CORES" nproc)
31:if [ "$NCORES" -ge 8 ]; then GATE_RATIO=6:$((NCORES-6))
32:else GATE_RATIO=$(((NCORES+1)/2)):$((NCORES-(NCORES+1)/2)); fi
33-PASS=0; FAIL=0
34-SRV=0; SRVLOG=/dev/null
35-GLOBCASE_ORACLE=0; MMPID=0
36-# The pinned vanilla Redis 7.4 tree (source + built src/redis-server). The ACL-category generator
37-# reads its SOURCE in both tiers; the differential and globcase rows boot its BINARY in the full
--
116-# deltas to the shared base rather than by taking one side: 245+2 / 258+2+3 = 247/263.
117-# Merged: 322 quick + 2 (multirace: the multi_exec order-late row per atomic mode) = 324;
118-# 335 full + 3 = 338. Additions are disjoint from the fused+armed and lane-admission rows.
119-# 258 -> 260 full: reply codes make a blocking timeout's "*-1"/"_" a CODE rather than bytes, so the
120-# ACL retire-recheck's discard has a second representation to drop; one battery per thread mode.
121:# Merged: the aclreply battery adds one row per thread mode and both run BEFORE the quick-tier exit,
122-# so quick is 324 + 2 = 326 and full is 338 + 2 = 340. (Corrected 2026-09-07: the first resolution read
123-# the branch deltas 245/260 over 245/258 as full-only and left quick at 324, so every quick gate failed
124-# its own ledger row by exactly 2 while the full tier was right.)
125-# 340 -> 343 full: the armed-write block cache's ownership laws (DESIGN-P0REPLY.md). A shard that
126-# changed owner kept its read-local retire sink pointing at the OLD owner's QSBR ring and block
127-# cache until the destination's own later pass rebound it, so the new owner wrote through another
128-# thread's unlocked free list. Three rows: the -DTOMO_RL_CACHE_DEBUG build, the churn battery on it
129-# (which carries its own non-vacuity checks, including that the balancer MOVED shards), and the
130-# invariant check over that server's log.
131-# COUNTED BY LINE, NOT BY INFERENCE (which is what produced the 324/326 correction above): the
132:# quick tier exits at the `[ "$TIER" = quick ]` block, and all three rows are emitted after it, so
133-# they are full-only. Quick is unchanged at 326; full is 340 + 3 = 343.
134-# 324 -> 325 quick (340 -> 341 full): the RYOW write-ring unit joins the static rows, carrying the
135-# arm-on-demand transient cases (t-ringdiet, DESIGN-RINGDIET.md). One row in BOTH tiers, because it
136-# is server-less and costs under two seconds; the arithmetic is 324+1 / 340+1 and it is disjoint
137-# from every boot-geometry loop above.
138-# Merged 2026-09-07: the ring unit is one row in BOTH tiers (server-less, under two seconds), so
139-# quick is 326 + 1 = 327 and full is 343 + 1 = 344. Counted by line: the ring row is emitted with
140:# the static rows, far above the quick-tier exit, and the P0 rows stay below it.
141:EXPECT_QUICK=377
142:EXPECT_FULL=394                 # full without the optional NIC row.
143-say(){ printf '  %-52s %s\n' "$1" "$2"; }
144-ledger(){ # verdict label -> one ledger line; the elapsed column is wall time since the last row
145-  local now; now=$(date +%s.%N)
146-  printf '%s\t%s\t%s\n' "$1" "$(awk -v a="$ROW_T" -v b="$now" 'BEGIN{printf "%.1f", b-a}')" "$2" \
147-      >> "$LEDGER"
--
317-g++ -std=c++20 -O2 -march=native -pthread -I. tests/read_local_write_ring_unit.cc \
318-    -o /tmp/tomokv-read-local-write-ring-unit 2>/tmp/gate-ring-unit.txt \
319-    && /tmp/tomokv-read-local-write-ring-unit >>/tmp/gate-ring-unit.txt 2>&1 \
320-    && ok "read-local write ring + arming transient unit" \
321-    || bad "read-local write ring + arming transient unit" "see /tmp/gate-ring-unit.txt"
322:# SURVIVING core concurrency regressions. Eight rows, all ABOVE the quick-tier exit.
323-# Each selection asserts its hazardous state; ASAN/UBSAN and bounded interleaving hooks
324-# make a broken mechanism fail. The fixture starts no server and opens no listener.
325-CORE_UNIT_READY=1
326-pausable make -j2 build/core-concurrency-unit >build/gate-core-concurrency-build.txt 2>&1 \
327-    || CORE_UNIT_READY=0
--
334-    ok "core concurrency $core_row"
335-  else
336-    bad "core concurrency $core_row" "see build/gate-core-$core_row.txt and build/gate-core-concurrency-build.txt"
337-  fi
338-done
339:# REORDER.md: one row in BOTH tiers (before the quick exit). Real published ROB tasks drive the
340-# production scheduler at 32/128 capacity. Exact non-identity permutations prove it fired; ASAN
341-# and UBSAN make undersized scratch and an invalid occupancy shift fail, never skip or time out green.
342-g++ -std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
343-    -fno-omit-frame-pointer -pthread -I. tests/reorder_unit.cc \
344-    -o /tmp/tomokv-reorder-unit 2>/tmp/gate-reorder-unit.txt \
345-    && timeout 60 /tmp/tomokv-reorder-unit >>/tmp/gate-reorder-unit.txt 2>&1 \
346-    && ok "reorder mechanism + 32/128-task geometry battery" \
347-    || bad "reorder mechanism + 32/128-task geometry battery" "see /tmp/gate-reorder-unit.txt"
348:# Twelve storage regressions, all BEFORE the quick-tier exit. The hash reaper is production code;
349-# store-boundary spies make held epochs, capture cursors, eviction and allocation failures exact.
350-# No case skips. Build failure makes every dependent row red. EXPECT constants are maintainer-owned.
351-STORE_REGRESSION_BUILT=0
352-pausable make build/store-regression >/tmp/gate-store-build.txt 2>&1 && STORE_REGRESSION_BUILT=1
353-for STORE_CASE in unlinked randomkey rehash rollback snapshot-eviction flags aof-eviction intents imported-hash field-index-failure hash-bytes; do
--
373-    && timeout --foreground "$GATE_TEST_TIMEOUT" ./build/store-regression-sidecar deadline-sidecar \
374-        >>/tmp/gate-store-sidecar.txt 2>&1 \
375-    && ok "storage deadline-sidecar regression" \
376-    || bad "storage deadline-sidecar regression" "see /tmp/gate-store-sidecar.txt"
377-# SURVIVING.md's atomic lane: one build and fifteen named, deterministic defect rows.
378:# Counted by line: all sixteen are ABOVE the quick-tier exit, so both tiers gain sixteen.
379:# EXPECT_QUICK/EXPECT_FULL are deliberately left to the maintainer (see FIXES-ATOMICS.md).
380-pausable make -j2 build/atomic-survivors-unit >/tmp/gate-atomic-survivors-build.txt 2>&1 \
381-    && ok "atomic survivors unit build" \
382-    || bad "atomic survivors unit build" "see /tmp/gate-atomic-survivors-build.txt"
383-for defect in admission closure script_keys rename_overlay write_latest script_apply \
384-              lua_conversion watch_parent watch_cycle mset_arity watch_oom lua_lines \
--
388-      >"/tmp/gate-atomic-survivors-$defect.txt" 2>&1 \
389-      && ok "atomic survivor: $defect" \
390-      || bad "atomic survivor: $defect" "see /tmp/gate-atomic-survivors-$defect.txt"
391-done
392-# Surviving networking/command audit: deterministic serverless failure states, both tiers.
393:pausable make -j2 build/netcmd-unit >/tmp/gate-netcmd-build.txt 2>&1 \
394:    && ok "netcmd regression build" || bad "netcmd regression build" "see /tmp/gate-netcmd-build.txt"
395-for NETCMD_CASE in streams zpop notify-oom notify-retry flush output pubsub receive config; do
396:  timeout 60 ./build/netcmd-unit "$NETCMD_CASE" >/tmp/gate-netcmd-$NETCMD_CASE.txt 2>&1 \
397:      && ok "netcmd $NETCMD_CASE regression" \
398:      || bad "netcmd $NETCMD_CASE regression" "see /tmp/gate-netcmd-$NETCMD_CASE.txt"
399-done
400-if [ "$ORACLE_OK" = 1 ]; then
401-  python3 tools/gen_acl_categories.py --redis-root "$REDIS74_ROOT" \
402-      --check src/cmd/acl_categories_generated.h \
403-      && ok "generated Redis 7.4 ACL categories" || bad "generated Redis 7.4 ACL categories"
--
451-  boot_dir=$(mktemp -d "/tmp/gate-data-$tag.XXXXXX") || return 1
452-  taskset -c $CORES "$bin" --port $PORT --bind 127.0.0.1 --shards 16 --dir "$boot_dir" "$@" > "$SRVLOG" 2>&1 &
453-  SRV=$!
454-  # 30s, not 10s: the AOF replay boot replays its file BEFORE it listens, and on a box shared
455-  # with other lanes that overran a 10s deadline and turned six AOF rows red with no defect behind
456:  # them. A generous deadline costs nothing when the server is quick — the loop exits on connect.
457-  for _ in $(seq 150); do
458-    if ! kill -0 "$SRV" 2>/dev/null; then
459-      wait "$SRV" 2>/dev/null; boot_log_tail "$tag" "exited before it listened"; return 1
460-    fi
461-    (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && return 0; sleep 0.2; done
--
467-                 # "see $SRVLOG" and the log was rotated by the next row's mktemp before anyone
468-                 # opened it.
469-  printf 'GATE: boot (%s) %s -- last 25 lines of %s:\n' "$1" "$2" "$SRVLOG" >&2
470-  tail -n 25 "$SRVLOG" 2>/dev/null | sed 's/^/GATE|   /' >&2
471-}
472:boot(){ local bin=$1; shift; launch main "$bin" --ratio $GATE_RATIO "$@"; }
473-boot_fused(){ # deliberately omits --ratio, which fused mode rejects
474-  local bin=$1; shift; launch fused "$bin" --thread-mode fused "$@"; }
475-stop(){ kill -TERM $SRV 2>/dev/null; wait $SRV 2>/dev/null; settle; }
476-shutdown_clean(){ python3 tests/shutdown_report.py "$SRVLOG" clean; }
477-shutdown_present(){ python3 tests/shutdown_report.py "$SRVLOG" present; }
--
483-py tests/cmdmeta_coverage.py >/tmp/gate-cmdmeta-coverage.txt 2>&1 \
484-    && ok "cmdmeta covers every registered command" \
485-    || bad "cmdmeta covers every registered command" "see /tmp/gate-cmdmeta-coverage.txt"
486-
487-# ---- 3. correctness: smoke + torture + RYOW on the release build ------------------------------
488:# MERGEWAITS.md: six rows, all BEFORE the quick-tier exit. Build/boot failure belongs to its
489-# dependent row, so it cannot change the count. The retirement row intentionally includes lazy
490-# expiry: cx-waits alone still fails that broader no-quiescence-wait law. Never skip/xfail it.
491-pausable make build/waits-unit >/tmp/gate-waits-unit.txt 2>&1 \
492-    && timeout --foreground 60 taskset -c "$CORES" ./build/waits-unit >>/tmp/gate-waits-unit.txt 2>&1 \
493-    && ok "waits config publication + admission unit" \
--
562-# ---- feature batteries: every shipped feature's directed test, BOTH atomic settings -----------
563-# The gate accumulates a section per landed feature (owner rule). Each test is directed and
564-# asserts its own mechanisms fired; the boot covers multi/blocking/pubsub+sharded/lua/limits.
565-# ONE list, shared with the fused+armed leg below, so the two legs cannot drift apart: a battery
566-# added here runs on the armed lane too, and the ledger arithmetic counts it twice per atomic mode.
567:FEATURE_BATTERIES="s6 multi_exec blocking blockmulti stream streamgroups pubsub lua_scripting scriptsurf limits resp3 bitfield dumprestore zsetops geo climon climon2 tracking hexpire servertail lcs concur edgeproto edgeenc edgetime arity contarity cmdgap aclsel expwide infofix pushtear netcmd"
568-for AT in 0 1; do
569-  boot ./build/tomokv --atomic $AT --enable-debug-command yes \
570-      || bad "feature battery boot (atomic $AT)"
571-  for t in $FEATURE_BATTERIES; do
572-    FEATURE_ARGS=()
--
1211-    && ok "AOF no-sync bypassed sync + reply gate" \
1212-    || bad "AOF no-sync bypass" "see /tmp/gate-aof-no-sync.txt"
1213-stop
1214-
1215-quiet_wait
1216:NET_IO=$NET_IO GATE_PORT=$PORT GATE_CORES=$CORES tests/aof_rewrite_matrix.sh \
1217-    >/tmp/gate-aof-rewrite.txt 2>&1 \
1218-    && ok "AOF rewrite atomic/stage/corruption matrix" \
1219-    || bad "AOF rewrite matrix" "see /tmp/gate-aof-rewrite.txt"
1220-
1221-quiet_wait
1222:NET_IO=$NET_IO GATE_PORT=$PORT GATE_CORES=$CORES tests/aof_rewrite_trigger_matrix.sh \
1223-    >/tmp/gate-aof-rewrite-trigger.txt 2>&1 \
1224-    && ok "AOF rewrite triggers + observability matrix" \
1225-    || bad "AOF rewrite triggers" "see /tmp/gate-aof-rewrite-trigger.txt"
1226-
1227-AOF_OFF_DIR=$(mktemp -d "/tmp/gate-aof-off-${NET_IO}.XXXXXX")
--
1293-  SRV=0; SRVLOG=/dev/null
1294-  guard_port "$PORT"
1295-  guard_port "$TLS_PORT"
1296-  SRVLOG=$(mktemp /tmp/gate-tls-srv.XXXXXX)
1297-  taskset -c $CORES ./build/tomokv --port "$PORT" --tls-port "$TLS_PORT" \
1298:      --bind 127.0.0.1 --shards 16 --ratio "$GATE_RATIO" --protected-mode no \
1299-      --tls-cert-file "$TLS_DIR/server.crt" --tls-key-file "$TLS_DIR/server.key" \
1300-      --tls-ca-cert-file "$TLS_DIR/ca.crt" --tls-auth-clients "$auth" "$@" \
1301-      >"$SRVLOG" 2>&1 &
1302-  SRV=$!
1303-  for _ in $(seq 50); do
--
1366-    || bad "TLS connection-slot cleanup" "$TLS_FREED/$TLS_ACCEPTS"
1367-[ -n "$TLS_ZC" ] && [ "$TLS_ZC" -gt 0 ] \
1368-    && ok "TLS zc borrow gates fired (suppressed=$TLS_ZC)" || bad "TLS zc borrow gates fired"
1369-
1370-if [ "$TIER" = quick ]; then
1371:  program_state "$EXPECT_QUICK"
1372:  echo; echo "GATE(quick): $PASS ok, $FAIL FAIL"; [ $FAIL -eq 0 ] || exit 1; exit 0
1373-fi
1374-
1375-# ---- 4. full tier: torture under ASAN ---------------------------------------------------------
1376-boot $ASAN --atomic 1 --enable-debug-command yes || bad "ASAN boot"
1377-py tests/torture.py 127.0.0.1 $PORT >/tmp/gate-tort-asan.txt 2>&1 \
--
1410-done
1411-
1412-# ---- 4b. full tier: zero-copy borrow lifetime (release+ASAN) ----------------------------------
1413-zcboot(){
1414-  quiet_wait
1415:  tools/quietcheck.sh "${GATE_CORES:-192-199}" "$PORT" 2>/tmp/gate-quiet.err || {
1416-    sleep 3
1417:    tools/quietcheck.sh "${GATE_CORES:-192-199}" "$PORT" 2>>/tmp/gate-quiet.err || {
1418-      echo "boot preflight: cores/port not quiet: $(tail -1 /tmp/gate-quiet.err)"; return 1; }
1419-  }
1420-  SRV=0; SRVLOG=/dev/null
1421-  guard_port "$PORT"
1422-  SRVLOG=$(mktemp /tmp/gate-srv-zc.XXXXXX)
1423:  timeout 900 taskset -c $CORES "$1" --port $PORT --bind 127.0.0.1 --shards 16 --ratio $GATE_RATIO \
1424-      --zc-min 16384 > "$SRVLOG" 2>&1 &
1425-  SRV=$!
1426-  for _ in $(seq 50); do
1427-    if ! kill -0 "$SRV" 2>/dev/null; then wait "$SRV" 2>/dev/null; return 1; fi
1428-    (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && return 0; sleep 0.2; done
--
1486-# The helper discovers the ordinary suites from differ.py's gens registry, adds the two special
1487-# early-exit suites, and runs serially because this gate owns only one target/oracle port pair.
1488-DIFFER_ORACLE_PORT=${GATE_DIFFER_ORACLE_PORT:-$((PORT+1))}
1489-quiet_wait
1490-if GATE_DIFFER_ORACLE_CORES=${GATE_DIFFER_ORACLE_CORES:-$CORES} \
1491:    tests/differ_gate.sh ./build/tomokv "$PORT" "$DIFFER_ORACLE_PORT" "$CORES" "$GATE_RATIO"; then
1492-  ok "Redis 7.4 differential matrix"
1493-else
1494-  bad "Redis 7.4 differential matrix"
1495-fi
1496-
--
1503-# binary the other way and runs the same matrix; differ_gate.sh's own non-vacuity check refuses to
1504-# report a pass unless read_local_hits actually advanced, so an accidentally-disarmed boot is a
1505-# FAIL rather than a quiet green.
1506-if GATE_DIFFER_GEOMETRY=armed-fused \
1507-    GATE_DIFFER_ORACLE_CORES=${GATE_DIFFER_ORACLE_CORES:-$CORES} \
1508:    tests/differ_gate.sh ./build/tomokv "$PORT" "$DIFFER_ORACLE_PORT" "$CORES" "$GATE_RATIO"; then
1509-  ok "Redis 7.4 differential matrix (armed fused + read-local)"
1510-else
1511-  bad "Redis 7.4 differential matrix (armed fused + read-local)"
1512-fi
1513-
--
1595-  esac
1596-else
1597-  say "NIC regression cells" "SKIPPED (no rig/refs)"
1598-fi
1599-
1600:program_state "$((EXPECT_FULL+NIC_CHECKED))"
1601-echo
1602-[ "$NIC_CHECKED" -eq 1 ] \
1603-    && echo "GATE(full): $PASS ok, $FAIL FAIL" \
1604-    || echo "GATE(full, no perf tier): $PASS ok, $FAIL FAIL"
1605-[ $FAIL -eq 0 ] || exit 1

 succeeded in 0ms:
/home/user/Projects/tomokv-cpp                                                           2560dd60b [master]
/home/user/Projects/base-2e794c3bc                                                       2e794c3bc (detached HEAD)
/home/user/Projects/base-b55ec                                                           b55ec084b (detached HEAD)
/home/user/Projects/base-c0d7f27fd                                                       c0d7f27fd (detached HEAD)
/home/user/Projects/base-mask                                                            b3d213071 (detached HEAD)
/home/user/Projects/base-premask                                                         d1a68acb5 (detached HEAD)
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
/home/user/Projects/cx-comments                                                          1ebca1ed5 [cx-comments]
/home/user/Projects/cx-cov                                                               c8e61f646 (detached HEAD)
/home/user/Projects/cx-delvar                                                            a145bb1e3 [cx-delvar]
/home/user/Projects/cx-differ                                                            c8e61f646 (detached HEAD)
/home/user/Projects/cx-divcheck                                                          c8e61f646 (detached HEAD)
/home/user/Projects/cx-docs                                                              8a53ddd6d [cx-docs]
/home/user/Projects/cx-final                                                             67f623716 [cx-final]
/home/user/Projects/cx-final/build/setregress/bad                                        8a7260059 (detached HEAD)
/home/user/Projects/cx-final/build/setregress/good                                       c8e61f646 (detached HEAD)
/home/user/Projects/cx-final/build/setregress/reported-bad                               2587f70e6 (detached HEAD)
/home/user/Projects/cx-fix-atomics                                                       b1c99c165 [cx-fix-atomics]
/home/user/Projects/cx-fix-concurrency                                                   4656510d9 [cx-fix-concurrency]
/home/user/Projects/cx-fix-netcmd                                                        e94b97100 [cx-fix-netcmd]
/home/user/Projects/cx-fix-storage                                                       184b1480b [cx-fix-storage]
/home/user/Projects/cx-harness2                                                          a4ceb61c3 [cx-harness2]
/home/user/Projects/cx-integ                                                             a9a220a49 [cx-integ]
/home/user/Projects/cx-knobs                                                             78c3e5391 [cx-knobs]
/home/user/Projects/cx-merge                                                             c8e61f646 [cx-merge]
/home/user/Projects/cx-mget                                                              78c3e5391 [cx-mget]
/home/user/Projects/cx-mgetaudit                                                         78c3e5391 (detached HEAD)
/home/user/Projects/cx-nowait                                                            78c3e5391 (detached HEAD)
/home/user/Projects/cx-nowaitb                                                           a5b5fd90d [cx-nowaitb]
/home/user/Projects/cx-overlap                                                           a108f2c49 [cx-overlap]
/home/user/Projects/cx-redisgap                                                          c8e61f646 [cx-redisgap]
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
67f623716 (HEAD -> cx-final) merge cx-resizefix
bf41dd331 (cx-resizefix) resize progress without reader retirement waits
818d458ef Merge cx-waits and add resize and retirement liveness batteries
e25e08f57 EXPECT 377/394 for the eight-branch merge
a6f1950ab Record remaining merges, validation rates, and gate arithmetic
8a7260059 Resolve CONFIG REWRITE alias preservation after netcmd merge
821d207b1 Merge cx-fix-netcmd preserving Lua and sender contracts
83e9c8258 Merge cx-fix-atomics with current configuration contracts
878af001a Merge cx-fix-storage with current read-local fixture contract
2587f70e6 EXPECT 335/352 for the merged tree
112ed6679 Merge cx-integ preserving deterministic windows and concurrency guards
6647031dd merge cx-fix-concurrency
d2a7e7fb8 (cx-waits) drop lane transcript
a9a220a49 (cx-integ) drop lane transcript
1d8a25d3c merge cx-vestcut
4656510d9 (cx-fix-concurrency) drop lane transcript
12458e0a5 merge cx-windowfix
daa937b00 (cx-vestcut) drop lane transcript
total 4192
drwxrwxr-x  10 user user    4096 Sep  9 18:08 .
drwxrwxr-x 135 user user    4096 Sep  9 17:44 ..
-rw-rw-r--   1 user user      63 Sep  9 08:16 .git
-rw-rw-r--   1 user user     618 Sep  9 08:16 .gitignore
-rw-rw-r--   1 user user   36183 Sep  9 08:16 AUDIT-ATOMICS.md
-rw-rw-r--   1 user user   50143 Sep  9 08:16 AUDIT-CLEANUP.md
-rw-rw-r--   1 user user    6358 Sep  9 08:16 AUDIT-DEMOTEPLAN.md
-rw-rw-r--   1 user user   15044 Sep  9 08:16 AUDIT-LOCALFUSE.md
-rw-rw-r--   1 user user   27574 Sep  9 08:16 AUDIT-NETWB.md
-rw-rw-r--   1 user user   15253 Sep  9 08:16 AUDIT-PUBLISH.md
-rw-rw-r--   1 user user   30022 Sep  9 08:16 AUDIT-STORAGE.md
-rw-rw-r--   1 user user   36077 Sep  9 08:16 AUDIT-TESTS.md
-rw-rw-r--   1 user user   24753 Sep  9 08:16 AUDIT-resp3push.md
-rw-rw-r--   1 user user   12672 Sep  9 19:23 CODEX-OUT.md
-rw-rw-r--   1 user user   89529 Sep  9 08:16 DESIGN-BPLUS.md
-rw-rw-r--   1 user user    3186 Sep  9 08:16 DESIGN-DEMOTEGATE.md
-rw-rw-r--   1 user user   34654 Sep  9 08:16 DESIGN-GATEHYGIENE.md
-rw-rw-r--   1 user user   48897 Sep  9 08:17 DESIGN-KNOBS.md
-rw-rw-r--   1 user user   24862 Sep  9 08:16 DESIGN-NOTIFY.md
-rw-rw-r--   1 user user   16891 Sep  9 08:16 DESIGN-P0REPLY.md
-rw-rw-r--   1 user user   20101 Sep  9 08:16 DESIGN-RINGDIET.md
-rw-rw-r--   1 user user   16735 Sep  9 08:16 DESIGN-SNAPSHOT.md
-rw-rw-r--   1 user user   12444 Sep  9 08:16 DESIGN-TYPES.md
-rw-rw-r--   1 user user    5352 Sep  9 08:16 DESIGN-W-RLIDEAS.md
-rw-rw-r--   1 user user    9937 Sep  9 08:16 DESIGN-ZC.md
-rw-rw-r--   1 user user   32606 Sep  9 08:16 DESIGN-flipfp.md
-rw-rw-r--   1 user user    5459 Sep  9 08:16 DESIGN-w-atomclean.md
-rw-rw-r--   1 user user    5672 Sep  9 08:16 DESIGN-w-atomlat.md
-rw-rw-r--   1 user user   10317 Sep  9 08:16 DESIGN-w-fused.md
-rw-rw-r--   1 user user   10551 Sep  9 08:16 DESIGN-w-storettl.md
-rw-rw-r--   1 user user   10746 Sep  9 08:17 DESIGN.md
-rw-rw-r--   1 user user    2493 Sep  9 08:16 EVIDENCE-multiryow.txt
-rw-rw-r--   1 user user    2714 Sep  9 08:16 EVIDENCE-remaining.txt
-rw-rw-r--   1 user user   17930 Sep  9 14:02 FIXES-ATOMICS.md
-rw-rw-r--   1 user user   18209 Sep  9 14:07 FIXES-NETCMD.md
-rw-rw-r--   1 user user   12447 Sep  9 13:58 FIXES-STORAGE.md
-rw-rw-r--   1 user user   11252 Sep  9 14:07 FIXES.md
-rw-rw-r--   1 user user   20472 Sep  9 10:50 FIXMERGE.md
-rw-rw-r--   1 user user   12883 Sep  9 08:17 GATEFIX.md
-rw-rw-r--   1 user user  112171 Sep  9 08:17 KNOB-REVIEW.patch
-rw-rw-r--   1 user user   23177 Sep  9 08:16 MERGE.md
-rw-rw-r--   1 user user   28669 Sep  9 08:35 MERGEINTEG.md
-rw-rw-r--   1 user user   23442 Sep  9 14:47 MERGEREST.md
-rw-rw-r--   1 user user   17394 Sep  9 16:58 MERGEWAITS.md
-rw-rw-r--   1 user user    9294 Sep  9 16:31 Makefile
-rw-rw-r--   1 user user   12902 Sep  9 08:16 NOTES-ACLSEL.md
-rw-rw-r--   1 user user    8659 Sep  9 08:16 NOTES-AOF.md
-rw-rw-r--   1 user user   18603 Sep  9 08:16 NOTES-AOFFRAME.md
-rw-rw-r--   1 user user    6201 Sep  9 08:16 NOTES-AOFSCRIPT.md
-rw-rw-r--   1 user user   17196 Sep  9 08:16 NOTES-ARITY.md
-rw-rw-r--   1 user user   13675 Sep  9 08:16 NOTES-ATOMFIX.md
-rw-rw-r--   1 user user   32340 Sep  9 08:16 NOTES-ATOMICS.md
-rw-rw-r--   1 user user   25254 Sep  9 08:16 NOTES-BARRIER.md
-rw-rw-r--   1 user user    9569 Sep  9 08:16 NOTES-BARRIERTEST.md
-rw-rw-r--   1 user user    6056 Sep  9 08:16 NOTES-BITMAP.md
-rw-rw-r--   1 user user    3848 Sep  9 08:16 NOTES-BLOCKING.md
-rw-rw-r--   1 user user    8886 Sep  9 08:16 NOTES-BLOCKMULTI.md
-rw-rw-r--   1 user user    2854 Sep  9 08:16 NOTES-BORROWREG.md
-rw-rw-r--   1 user user    4534 Sep  9 08:16 NOTES-CAPTURE-PREFETCH.md
-rw-rw-r--   1 user user    3827 Sep  9 08:16 NOTES-CGAPS.md
-rw-rw-r--   1 user user   22771 Sep  9 08:16 NOTES-CLIMON2.md
-rw-rw-r--   1 user user   15738 Sep  9 08:16 NOTES-CMDGAP.md
-rw-rw-r--   1 user user   10795 Sep  9 08:16 NOTES-CMDGAP2.md
-rw-rw-r--   1 user user   21350 Sep  9 08:16 NOTES-CMDMETA.md
-rw-rw-r--   1 user user   16341 Sep  9 08:16 NOTES-COMPAT.md
-rw-rw-r--   1 user user   12931 Sep  9 08:16 NOTES-COMPATBLOCK.md
-rw-rw-r--   1 user user   12257 Sep  9 08:16 NOTES-COMPATINTRO.md
-rw-rw-r--   1 user user    4692 Sep  9 08:16 NOTES-CONTARITY.md
-rw-rw-r--   1 user user    6489 Sep  9 08:16 NOTES-DIFFERGATE.md
-rw-rw-r--   1 user user   21493 Sep  9 08:16 NOTES-DOUBLES2.md
-rw-rw-r--   1 user user   21755 Sep  9 08:16 NOTES-EDGEENC.md
-rw-rw-r--   1 user user   23912 Sep  9 08:16 NOTES-EDGEPROTO.md
-rw-rw-r--   1 user user   28361 Sep  9 08:16 NOTES-EDGETIME.md
-rw-rw-r--   1 user user   32393 Sep  9 08:16 NOTES-EPOLL.md
-rw-rw-r--   1 user user   10809 Sep  9 08:16 NOTES-EVICT.md
-rw-rw-r--   1 user user   19739 Sep  9 08:16 NOTES-EXECATOMIC.md
-rw-rw-r--   1 user user   27664 Sep  9 08:16 NOTES-EXECFIX.md
-rw-rw-r--   1 user user   24198 Sep  9 08:16 NOTES-EXECISO.md
-rw-rw-r--   1 user user   22940 Sep  9 08:16 NOTES-EXPWIDE.md
-rw-rw-r--   1 user user   13845 Sep  9 08:16 NOTES-GATEFIX.md
-rw-rw-r--   1 user user   10786 Sep  9 08:16 NOTES-HASH.md
-rw-rw-r--   1 user user   18177 Sep  9 08:16 NOTES-HEXPIRE.md
-rw-rw-r--   1 user user    5861 Sep  9 08:16 NOTES-HLL.md
-rw-rw-r--   1 user user   24917 Sep  9 08:16 NOTES-HYGIENE.md
-rw-rw-r--   1 user user    5247 Sep  9 08:16 NOTES-KNOBS.md
-rw-rw-r--   1 user user    9191 Sep  9 08:16 NOTES-KTLS.md
-rw-rw-r--   1 user user    2650 Sep  9 08:16 NOTES-LIMITS.md
-rw-rw-r--   1 user user    6643 Sep  9 08:16 NOTES-LIST.md
-rw-rw-r--   1 user user    6035 Sep  9 08:16 NOTES-LUA.md
-rw-rw-r--   1 user user    5214 Sep  9 08:16 NOTES-MDIET.md
-rw-rw-r--   1 user user   26987 Sep  9 08:16 NOTES-MIGRATE.md
-rw-rw-r--   1 user user   21428 Sep  9 08:16 NOTES-MONOFIX.md
-rw-rw-r--   1 user user    4169 Sep  9 08:16 NOTES-MULTI.md
-rw-rw-r--   1 user user    4772 Sep  9 08:16 NOTES-MULTILOWER.md
-rw-rw-r--   1 user user   38650 Sep  9 08:16 NOTES-MULTIRACE.md
-rw-rw-r--   1 user user   14956 Sep  9 08:16 NOTES-MULTIRES.md
-rw-rw-r--   1 user user    5149 Sep  9 08:16 NOTES-PERSIST-IO.md
-rw-rw-r--   1 user user    7839 Sep  9 08:16 NOTES-PREFETCH-MGET.md
-rw-rw-r--   1 user user   16292 Sep  9 08:16 NOTES-PSPERF2.md
-rw-rw-r--   1 user user   18603 Sep  9 08:16 NOTES-PUSHTEAR.md
-rw-rw-r--   1 user user   22705 Sep  9 08:16 NOTES-RECYCLE.md
-rw-rw-r--   1 user user    9623 Sep  9 08:16 NOTES-RESP2TRACK.md
-rw-rw-r--   1 user user    9931 Sep  9 08:16 NOTES-S6FIX.md
-rw-rw-r--   1 user user    7909 Sep  9 08:16 NOTES-SAVEFIX.md
-rw-rw-r--   1 user user   29520 Sep  9 08:16 NOTES-SCANCURSOR.md
-rw-rw-r--   1 user user   17551 Sep  9 08:16 NOTES-SCRIPTATOMIC.md
-rw-rw-r--   1 user user   16333 Sep  9 08:16 NOTES-SCRIPTSURF.md
-rw-rw-r--   1 user user   21361 Sep  9 08:16 NOTES-SERVERTAIL.md
-rw-rw-r--   1 user user    8645 Sep  9 08:16 NOTES-SET.md
-rw-rw-r--   1 user user   21874 Sep  9 08:16 NOTES-SNAPCUT.md
-rw-rw-r--   1 user user   27948 Sep  9 08:16 NOTES-SORT.md
-rw-rw-r--   1 user user   20967 Sep  9 08:16 NOTES-SORTXSHARD.md
-rw-rw-r--   1 user user   24457 Sep  9 08:16 NOTES-STOREORDER.md
-rw-rw-r--   1 user user    2929 Sep  9 08:16 NOTES-STREAMS.md
-rw-rw-r--   1 user user    9719 Sep  9 08:16 NOTES-STRING.md
-rw-rw-r--   1 user user   18573 Sep  9 08:16 NOTES-WATCHLIVE.md
-rw-rw-r--   1 user user   10361 Sep  9 08:16 NOTES-WAVED.md
-rw-rw-r--   1 user user    5319 Sep  9 08:16 NOTES-WIREDUMP.md
-rw-rw-r--   1 user user   11135 Sep  9 08:16 NOTES-XACCT.md
-rw-rw-r--   1 user user    7609 Sep  9 08:16 NOTES-XAUTOCLAIM.md
-rw-rw-r--   1 user user    5280 Sep  9 08:16 NOTES-XGROUPS.md
-rw-rw-r--   1 user user    9268 Sep  9 08:16 NOTES-XMOVE.md
-rw-rw-r--   1 user user   20455 Sep  9 08:16 NOTES-XPERF2.md
-rw-rw-r--   1 user user   24775 Sep  9 08:16 NOTES-XSCRIPT.md
-rw-rw-r--   1 user user   11615 Sep  9 08:16 NOTES-XSHARD.md
-rw-rw-r--   1 user user    4211 Sep  9 08:16 NOTES-ZCINFO.md
-rw-rw-r--   1 user user    4814 Sep  9 08:16 NOTES-ZGEO.md
-rw-rw-r--   1 user user    7565 Sep  9 08:16 NOTES-ZSET.md
-rw-rw-r--   1 user user   15276 Sep  9 08:16 NOTES-ZSETFIX.md
-rw-rw-r--   1 user user    8920 Sep  9 08:16 NOTES-pubsub.md
-rw-rw-r--   1 user user    8746 Sep  9 08:16 NOTES-t-puborder.md
-rw-rw-r--   1 user user   21056 Sep  9 08:17 ORTHOG.md
-rw-rw-r--   1 user user   21843 Sep  9 08:17 OVERLAP.md
-rw-rw-r--   1 user user   68574 Sep  9 08:16 P128.md
-rw-rw-r--   1 user user   22529 Sep  9 08:17 REORDER.md
-rw-rw-r--   1 user user   12271 Sep  9 18:08 RESIZEFIX.md
-rw-rw-r--   1 user user   38346 Sep  9 08:17 RL2S.md
-rw-rw-r--   1 user user   18069 Sep  9 15:12 SETREGRESS.md
-rw-rw-r--   1 user user   89647 Sep  9 08:16 SURVIVING.md
-rw-rw-r--   1 user user   22174 Sep  9 08:16 VESTCUT.md
-rw-rw-r--   1 user user 1433497 Sep  9 08:16 VESTIGIAL.md
-rw-rw-r--   1 user user   31220 Sep  9 16:19 WAITS.md
-rw-rw-r--   1 user user   10738 Sep  9 08:16 WINDOWFIX.md
drwxrwxr-x   9 user user    4096 Sep  9 18:08 build
drwxrwxr-x   3 user user    4096 Sep  9 08:17 docs
drwxrwxr-x   3 user user    4096 Sep  9 08:16 scratch
drwxrwxr-x  14 user user    4096 Sep  9 08:16 scratchpad
drwxrwxr-x  10 user user    4096 Sep  9 08:17 src
drwxrwxr-x   3 user user    4096 Sep  9 18:08 tests
drwxrwxr-x   3 user user    4096 Sep  9 08:16 third_party
-rw-rw-r--   1 user user   21407 Sep  9 08:17 tomokv.conf
drwxrwxr-x   3 user user    4096 Sep  9 15:02 tools

exec
/bin/bash -lc 'git worktree add -b codex/netcmdfix-20260909 /home/user/Projects/cx-final-netcmdfix 67f623716ca00b49d7cbdb8d33ab6b643b3e87ad' in /home/user/Projects/cx-final
 succeeded in 0ms:
Preparing worktree (new branch 'codex/netcmdfix-20260909')
HEAD is now at 67f623716 merge cx-resizefix

exec
/bin/bash -lc 'cat tests/netcmd_unit.cc && cat tests/netcmd_unit.h' in /home/user/Projects/cx-final-netcmdfix
 succeeded in 0ms:
#include "netcmd_unit.h"
// Include the actual cold command implementation to exercise its anonymous notification helpers.
// The unit link omits the corresponding production object.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsubobject-linkage"
#include "src/cmd/xshard.cc"
#pragma GCC diagnostic pop
#include <filesystem>

long netcmd_fail_after = -1;
size_t netcmd_fail_size = 0;
unsigned netcmd_allocations = 0, netcmd_failures = 0;
void* operator new(size_t size) {
    if (netcmd_fail_after >= 0 && (!netcmd_fail_size || netcmd_fail_size == size)) {
        ++netcmd_allocations;
        if (netcmd_fail_after-- == 0) { ++netcmd_failures; throw std::bad_alloc(); }
    }
    if (void* ptr = std::malloc(size ? size : 1)) return ptr;
    throw std::bad_alloc();
}
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, size_t) noexcept { std::free(ptr); }
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr, size_t) noexcept { std::free(ptr); }

namespace tomo {
struct NetcmdRegression {
    // Preparation-only fixtures still satisfy the production sender's complete bind contract.
    // The Ring stays unopened; these cases must never submit or acquire a borrowed value.
    struct Sender {
        Ring ring;
        std::atomic<bool> limits{false};
        uint32_t now_s = 0;
        LoopSignals signals;
        void bind(WbEngine& wb) {
            wb.bind(&ring, nullptr,
                [](void*, int32_t, const char*) { check(false, "unexpected fixture borrow release"); },
                nullptr, [](void*, Client&, Op&) { check(false, "unexpected fixture special retire"); },
                &limits, nullptr, [](void*, Client&) { return false; }, &now_s, &signals);
        }
    };
    static void notify_oom() {
        Server server;
        auto chain = std::make_unique<PubSubNotificationChain>();
        chain->items.push_back(PubSubNotificationItem{"channel", "message"});
        fault(0);
        bool threw = false;
        try { (void)notify_reserve_chain(server, std::move(chain)); }
        catch (const std::bad_alloc&) { threw = true; }
        const auto failed = netcmd_failures; fault(-1);
        check(threw && failed == 1, "shared_ptr control block allocation failed");
        check(server.pubsub_inflight() == 0, "reservation released exactly once on failure");
        auto good = std::make_unique<PubSubNotificationChain>();
        good->items.push_back(PubSubNotificationItem{"channel", "message"});
        auto reserved = notify_reserve_chain(server, std::move(good));
        check(reserved && server.pubsub_inflight() == 1, "successful chain owns reservation");
        reserved.reset(); check(server.pubsub_inflight() == 0, "successful chain releases reservation");
    }

    static void notify_retry() {
        Server server;
        Config cfg; cfg.even_ifid = 1; cfg.even_ex = 1; cfg.shards = 16;
        check(server.prepare_boot(cfg), "notification topology");
        server.threads_.resize(server.placement().total_threads());
        for (uint32_t i = 0; i < server.threads_.size(); ++i) {
            server.threads_[i] = std::make_unique<ThreadCtx>();
            server.threads_[i]->init(i, Role::Ifid, server.threads_.size(), 0, 0);
        }
        const uint32_t io = server.placement().ifid_threads().front();
        ThreadCtx& thread = server.thread(io);
        server.climon_tracking_io_mask_.store(uint64_t{1} << io);
        Ring ring; LoopSignals signals;
        uint32_t queued = 0;
        while (thread.post_client(0, nullptr, ring, signals)) ++queued;
        check(queued > 0, "producer marker lane filled");
        Shard shard; shard.init_private(&server, 0, TypeLimits{}, StreamLimits{});
        shard.notify_state_slot() = std::make_unique<NotifyShardState>();
        NotifyOut out; out.routes = NOTIFY_TRACKING; out.key = "expired";
        shard.notify_state_slot()->keyless.push_back(std::move(out));
        check(notify_ex_pass_entry(server, shard, 0, thread, ring, signals) == 0,
              "failed marker post is incomplete work");
        check(shard.notify_state_slot()->keyless.size() == 1, "keyless invalidation retained");
        std::deque<PubSubEvent*> events; thread.take_pubsub_events(events);
        check(events.empty(), "failed post enqueued no payload");
        check(thread.drain_clients([](Client*) {}) == queued, "producer lane drained");
        check(notify_ex_pass_entry(server, shard, 0, thread, ring, signals) == 1, "retry completes record");
        thread.take_pubsub_events(events);
        check(events.size() == 1 && events.front()->kind == PubSubEventKind::TrackingInvalidate &&
              events.front()->items.size() == 1 && events.front()->items.front().value == "expired",
              "exact invalidation delivered after retry");
        server.pubsub_event_retired(); delete events.front();
        thread.drain_clients([](Client*) {});
    }

    static void flush() {
        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 1; cfg.shards = 16;
        check(server.prepare_boot(cfg), "flush topology");
        const auto& ios = server.placement().ifid_threads();
        check(ios.size() == 2, "foreign tracking IO exists");
        server.threads_.resize(server.placement().total_threads());
        for (uint32_t i = 0; i < server.threads_.size(); ++i) {
            server.threads_[i] = std::make_unique<ThreadCtx>();
            server.threads_[i]->init(i, Role::Ifid, server.threads_.size(), 0, 0);
        }
        IoLoop loop; loop.srv_ = &server; loop.self_ = &server.thread(ios[0]);
        loop.climon_armed_cached_ = Server::kClimonTracking;
        server.climon_tracking_io_mask_.store(uint64_t{1} << ios[1]);
        Client writer(-1); writer.set_id(9); Op op; args(op, {"FLUSHALL"});
        check(!loop.climon_armed_gate(&writer, op), "FLUSH passed dispatch gate");
        std::deque<PubSubEvent*> events;
        server.thread(ios[1]).take_pubsub_events(events);
        check(events.empty(), "no flush invalidation before owner work was published");
        op.state.store(OpState::Done);
        loop.climon_flush_completed(op);
        server.thread(ios[1]).take_pubsub_events(events);
        check(events.size() == 1 && events.front()->kind == PubSubEventKind::TrackingFlush,
              "completed flush invalidates foreign tracking IO");
        delete events.front(); server.pubsub_event_retired();
        server.thread(ios[1]).drain_clients([](Client*) {});
    }

    static void output() {
        Sender sender;
        Client client(-1); WbEngine wb;
        sender.bind(wb);
        client.start_obuf_tracking();
        check(client.obuf_bytes() == 0, "tracked balance drained before disable");
        Op* op = client.rob().acquire(); check(op, "reply slot");
        op->reply.append("+PONG\r\n", 7); op->state.store(OpState::Done); client.rob().publish();
        bool allowed; wb.prepare(client, allowed);
        check(allowed && client.fill_buf().size() == 7, "disabled specialization staged reply");
        client.swap_buffers(); client.commit_write(7);
        check(client.write_drained(), "uncounted reply send completion did not underflow");

        Server server; ThreadCtx self; IoLoop loop;
        loop.srv_ = &server; loop.self_ = &self;
        sender.bind(loop.wb_);
        server.live_obuf_normal_hard_.store(8); server.client_obuf_armed_.store(true);
        Client held(-1); held.set_id(1);
        check(held.rob().acquire(), "held ordinary operation"); held.rob().publish();
        check(!held.blocked(), "ordinary operation, not blocking exception");
        check(loop.wb_.defer_oob(held, "012345678", 9), "push entered deferred queue");
        check(held.buffered_output_bytes() == 0 && loop.wb_.deferred_output_bytes(held) == 9,
              "limit bytes exist only in deferred storage");
        check(loop.client_obuf_check(&held, false), "hard limit fired before Done");
        check(held.closing(), "deferred output scheduled connection close");
        loop.wb_.teardown(held);
    }

    static void pubsub() {
        for (auto kind : {IoLoop::PubSubPendingKind::Channels, IoLoop::PubSubPendingKind::Numsub,
                          IoLoop::PubSubPendingKind::Numpat, IoLoop::PubSubPendingKind::Modify}) {
            for (bool suppressed : {false, true}) {
                Sender sender;
                Server server; ThreadCtx self; IoLoop loop; Client client(-1); client.set_id(42);
                loop.srv_ = &server; loop.self_ = &self; sender.bind(loop.wb_);
                Op* op = client.rob().acquire(); check(op, "pending pubsub operation");
                if (suppressed) op->mark_reply_skip();
                client.rob().publish();
                auto& local = loop.pubsub_local_[client.id()]; local.client = &client;
                local.channels.insert("old"); local.pending = 1;
                auto& pending = loop.pubsub_pending_[client.id()];
                pending.kind = kind; pending.op_id = client.rob().flush_id(); pending.subscribe = true;
                if (kind == IoLoop::PubSubPendingKind::Modify) pending.items.push_back("new");
                server.pubsub_pending_started();
                const std::string message = "*3\r\n$7\r\nmessage\r\n$3\r\nold\r\n$4\r\ndata\r\n";
                loop.pubsub_emit(&client, message.data(), message.size(), true);
                check(!pending.deferred.empty(), "publication arrived while control was pending");
                loop.pubsub_finish_pending(client.id());
                check(loop.pubsub_pending_.empty(), "pending control completed");
                bool allowed;
                if (suppressed) loop.wb_.prepare_suppressing(client, allowed);
                else loop.wb_.prepare(client, allowed);
                const std::string reply(client.fill_buf().data(), client.fill_buf().size());
                std::string push = message; push[0] = '>';
                check(reply.find(push) != std::string::npos, "independent publication survives retirement");
                if (suppressed) check(reply == push, "only command reply was suppressed");
            }
        }
    }

    static void receive_segments() {
        SegmentQueue<4, 8> segments;
        segments.append_buf("abcdefghijk", 11, "lmnopqrs", 8);
        check(segments.size() == 3 && segments.byte_size() == 19, "two-piece aggregate split into representable segments");
        iovec iov[4]; bool borrow = false; uint32_t bytes = 0;
        const auto n = segments.build_iov(iov, 4, 64, borrow, bytes);
        std::string actual;
        for (uint32_t i = 0; i < n; ++i) {
            check(iov[i].iov_len <= 8, "segment length cap honored");
            actual.append(static_cast<const char*>(iov[i].iov_base), iov[i].iov_len);
        }
        check(bytes == 19 && actual == "abcdefghijklmnopqrs", "aggregate contents preserved");
        Client input(-1); size_t avail = 0;
        check(input.read_space(2 * 1024 * 1024, avail, true, 1024 * 1024) && avail >= 2 * 1024 * 1024,
              "whole command can exceed individual bulk limit");
        const auto before = input.rcap();
        input.commit_read(before - 1);
        check(!input.read_space(4096, avail, false, 1024 * 1024) && input.rcap() == before,
              "receive growth remains forbidden with outstanding argv slices");
        // Allocation reserves virtual space; no 600 MiB payload or benchmark is needed.
        Client large(-1);
        check(large.read_space(600ull * 1024 * 1024, avail, true) &&
              avail >= 600ull * 1024 * 1024, "default receive allowance has no one-bulk ceiling");
        check(large.read_space(600ull * 1024 * 1024, avail, true, 1024ull * 1024 * 1024) &&
              avail >= 600ull * 1024 * 1024, "raised bulk limit fits epoll receive geometry");
    }

    // Diagnostic for the still-open cross-family partial-mutation finding; deliberately absent
    // from the passing gate rows until a failure-atomic collection update is implemented.
    static void collection_oom() {
        Shard shard; TypeLimits limits; limits.hash.max_entries = 1;
        shard.init_private(nullptr, 0, limits, StreamLimits{}); shard.set_cached_now_ms(1000);
        check(execute(shard, {"HSET", "h", "a", "old-a", "b", "old-b"}) == ":2\r\n", "expanded hash seeded");
        auto* object = shard.store().find(FlatStore::hash_key(Slice("h", 1)), Slice("h", 1));
        check(CollectionRef(object).encoding() == CollectionEncoding::Hashtable, "expanded hash arm entered");
        check(execute(shard, {"HPEXPIRE", "h", "60000", "FIELDS", "1", "a"}) == "*1\r\n:1\r\n", "field TTL armed");
        const std::string replacement(256, 'x'); Op op;
        args(op, {"HSET", "h", "a", replacement.c_str(), "b", replacement.c_str()});
        fault(1, replacement.size() + 1); op.spec->handler(shard, op);
        const unsigned failures = netcmd_failures; fault(-1);
        check(failures == 1, "second replacement allocation failed");
        check(std::string(op.reply.data(), op.reply.size()).starts_with("-ERR"), "HSET reported allocation failure");
        check(execute(shard, {"HGET", "h", "a"}) == "$5\r\nold-a\r\n",
              "OPEN F05: failed multi-field HSET retained a changed prefix (and its old TTL)");
    }

    static void config() {
        Server server;
        const std::string password = "pass with spaces\n\r\t\"'\\tail";
        char directory[] = "build/netcmd-config-XXXXXX";
        check(::mkdtemp(directory), "private config directory");
        const std::string path = std::string(directory) + "/tomo.conf";
        server.cfg_.conf_path = path.c_str(); server.cfg_.requirepass = password.c_str();
        command_bind_server(&server);
        test_config_rewrite();
        command_bind_server(nullptr);
        std::filesystem::remove_all(directory);
    }
};
}

int main(int argc, char** argv) {
    check(argc == 2, "one regression section required");
    check(tomo::command_registry_init(false), "command registry initialized");
    const std::string mode = argv[1];
    using R = tomo::NetcmdRegression;
    if (mode == "streams") test_stream_faults();
    else if (mode == "zpop") test_zpop_faults();
    else if (mode == "notify-oom") R::notify_oom();
    else if (mode == "notify-retry") R::notify_retry();
    else if (mode == "flush") R::flush();
    else if (mode == "output") R::output();
    else if (mode == "pubsub") R::pubsub();
    else if (mode == "receive") R::receive_segments();
    else if (mode == "collection-oom") R::collection_oom();
    else if (mode == "config") R::config();
    else check(false, "unknown regression section");
    std::printf("ok: netcmd %s\n", argv[1]);
}
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <new>
#include <string>
#include "src/cmd/command.h"
#include "src/core/shard.h"
#include "src/exec/op.h"

extern long netcmd_fail_after;
extern size_t netcmd_fail_size;
extern unsigned netcmd_allocations, netcmd_failures;
inline void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
inline void fault(long after, size_t size = 0) {
    netcmd_fail_after = after; netcmd_fail_size = size;
    netcmd_allocations = netcmd_failures = 0;
}
inline void args(tomo::Op& op, std::initializer_list<const char*> values) {
    for (const char* value : values)
        check(op.push_arg(tomo::Slice(value, std::strlen(value))), "argument allocation");
    op.spec = tomo::command_lookup(op.cmd_name());
    check(op.spec != nullptr, "registered command");
    const int first = op.spec->first_key;
    if (first > 0) op.hash = tomo::FlatStore::hash_key(op.arg(first));
    op.shard = 0;
}
inline std::string execute(tomo::Shard& shard, std::initializer_list<const char*> values) {
    tomo::Op op; args(op, values); op.spec->handler(shard, op);
    return std::string(op.reply.data(), op.reply.size());
}
void test_stream_faults();
void test_zpop_faults();
void test_config_rewrite();

exec
/bin/bash -lc "sed -n '1,160p' Makefile && rg -n -C 8 'netcmd' Makefile && sed -n '360,410p' tests/gate.sh && rg -n -C 2 'retry|flush|output|Validation|Repro|test|cmake' FIXES-NETCMD.md" in /home/user/Projects/cx-final-netcmdfix
 succeeded in 0ms:
CXX      ?= g++
JEDIR    ?= /home/user/Projects/refs/jemalloc/_install
# jemalloc is optional: without it the tree still builds and runs, using the deterministic portable
# size-class table in alloc.h. Set JE=0 to force that path (useful for A/B-ing the allocator).
JE       ?= 1
ifeq ($(JE),1)
  # Prefer the system package; fall back to the copy built from refs/.
  ifneq ($(wildcard /usr/include/jemalloc/jemalloc.h),)
    JEFLAGS := -DTOMO_JEMALLOC
    JELIBS  := -ljemalloc
  else
    JEFLAGS := -DTOMO_JEMALLOC -I$(JEDIR)/include
    JELIBS  := $(JEDIR)/lib/libjemalloc.a -ldl
  endif
else
  JEFLAGS :=
  JELIBS  :=
endif
CXXFLAGS ?= -std=c++20 -O2 -g -Wall -Wextra -march=native -pthread
LDLIBS   ?= -luring -pthread
SRC      := src/main.cc src/net/tls.cc src/cmd/commands.cc src/cmd/glob.cc src/cmd/xshard.cc src/cmd/acl.cc src/cmd/hll.cc src/cmd/t_server.cc src/cmd/t_string.cc src/cmd/t_string_notify.cc src/cmd/t_hash.cc src/cmd/t_hash_ttl.cc \
            src/cmd/t_list.cc src/cmd/t_set.cc src/cmd/t_zset.cc src/cmd/t_zset_ops.cc src/cmd/geo.cc src/cmd/t_stream.cc src/cmd/t_stream_groups.cc src/cmd/scripting.cc src/cmd/functions.cc src/cmd/serialize.cc src/snapshot/snapshot.cc src/persist/aof.cc
SRC      += src/cmd/climon.cc src/cmd/tracking.cc
SRC      += src/cmd/server_tail.cc src/cmd/slowlog.cc src/cmd/lcs.cc src/cmd/info_stats.cc
SRC      += src/cmd/lbsignals.cc
SRC      += src/core/flipctl.cc
SRC      += src/core/genthread.cc
SRC      += src/core/rl2s.cc
SRC      += src/cmd/cmdgap.cc
SRC      += src/cmd/pfdebug.cc
SRC      += src/cmd/cmdmeta.cc
SRC      += src/cmd/t_sort.cc
LDLIBS   += -lssl -lcrypto
BIN      := build/tomokv
OBJ      := $(SRC:%.cc=build/%.o)

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) $(OBJ) -o $@ $(JELIBS) $(LDLIBS) -lm

# The clean string family intentionally excludes the armed instantiations, but its parsed template
# bodies still move GCC just past the default large-unit threshold. 10600 restores the same inlining
# decisions as the base-420b4d492 translation unit; the objdump gate locks cmd_get/cmd_set to base.
build/src/cmd/t_string.o: CXXFLAGS += --param large-unit-insns=10600

build/%.o: %.cc $(wildcard src/*/*.h) $(wildcard src/*/*.inc) $(wildcard third_party/lua/*) Makefile
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(JEFLAGS) -I. -c $< -o $@

asan: CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -O1
asan:
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I. $(SRC) -o build/tomokv-asan $(LDLIBS) -lm

tsan:
	@mkdir -p build
	$(CXX) -std=c++20 -O1 -g -Wall -Wextra -pthread -fsanitize=thread \
	  -I. $(SRC) -o build/tomokv-tsan $(LDLIBS) -lm

# The armed-write block cache's ownership laws as assertions: the cache and the QSBR retire ring
# are owner-private (one thread, no lock), each cached block is resident exactly once, each class
# list matches its counter, and every shard's retire sink names its CURRENT owner. Debug-only: the
# residency set and the sampled list walk cost far more than the path they guard. DESIGN-P0REPLY.md.
rlcachedbg:
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(JEFLAGS) -DTOMO_RL_CACHE_DEBUG -I. $(SRC) \
	  -o build/tomokv-rlcachedbg $(JELIBS) $(LDLIBS) -lm

# NEGATIVE-CONTROL BUILD for the row above: the same assertions with the ownership-edge rebind
# removed, i.e. the pre-fix behaviour. tests/rlcache_churn.py MUST fail against this binary; a
# detector that cannot report failure proves nothing about the runs that pass.
rlcache-nofix:
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(JEFLAGS) -DTOMO_RL_CACHE_DEBUG -DTOMO_RL_CACHE_NO_EAGER_ADOPT -I. $(SRC) \
	  -o build/tomokv-rlcache-nofix $(JELIBS) $(LDLIBS) -lm

# NEGATIVE-CONTROL BUILD for the cross-owner script reservation sub-wave. Identical to the release
# build except that ScriptPhase::Pin arms nothing, so tests/xscript.py counterexample MUST fail
# against it. A detector that cannot report failure proves nothing about the runs that pass.
noreserve:
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(JEFLAGS) -DTOMO_XSCRIPT_NO_RESERVE -I. $(SRC) \
	  -o build/tomokv-noreserve $(JELIBS) $(LDLIBS) -lm

# Server-less unit binaries: the config parser and the flip controller. `make unit` builds and
# runs both (neither boots a server). tests/gate.sh's parser row is the same program.
build/config-parser-test: tests/config_parser_test.cc $(wildcard src/*/*.h) Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I. tests/config_parser_test.cc -o $@
build/flipctl-unit: tests/flipctl_unit.cc src/core/flipctl.cc $(wildcard src/*/*.h) Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I. tests/flipctl_unit.cc src/core/flipctl.cc -o $@
# The fused owner's deferred-reclaim ring (QSBR batches) against a per-entry reference model.
build/read-local-ring-unit: tests/read_local_ring_unit.cc $(wildcard src/*/*.h) Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I. tests/read_local_ring_unit.cc -o $@
build/read-local-write-ring-unit: tests/read_local_write_ring_unit.cc $(wildcard src/*/*.h) Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I. tests/read_local_write_ring_unit.cc -o $@
# The production cross-connection scheduler, with real ROB tasks at both executor capacities.
build/reorder-unit: tests/reorder_unit.cc $(wildcard src/*/*.h) Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I. tests/reorder_unit.cc -o $@
STORE_REGRESSION_SRC := tests/store_regression.cc src/cmd/t_hash.cc src/cmd/t_hash_ttl.cc
build/store-regression: $(STORE_REGRESSION_SRC) $(wildcard src/*/*.h) $(wildcard src/*/*.inc) Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -ffunction-sections -fdata-sections -DTOMO_STORE_REGRESSION_TEST -I. \
	  $(STORE_REGRESSION_SRC) -Wl,--gc-sections -o $@
build/store-regression-sidecar: $(STORE_REGRESSION_SRC) $(wildcard src/*/*.h) $(wildcard src/*/*.inc) Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -ffunction-sections -fdata-sections -DTOMO_STORE_REGRESSION_TEST \
	  -DTOMO_TTL_DEADLINE_SIDECAR=1 -I. $(STORE_REGRESSION_SRC) -Wl,--gc-sections -o $@
build/store-regression-tsan: $(STORE_REGRESSION_SRC) $(wildcard src/*/*.h) $(wildcard src/*/*.inc) Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -O1 -fsanitize=thread -fno-omit-frame-pointer -no-pie \
	  -ffunction-sections -fdata-sections -DTOMO_STORE_REGRESSION_TEST -I. \
	  $(STORE_REGRESSION_SRC) -Wl,--gc-sections -o $@
build/waits-unit: tests/waits_unit.cc $(wildcard src/*/*.h) Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I. tests/waits_unit.cc -o $@
unit: build/config-parser-test build/flipctl-unit build/read-local-ring-unit build/read-local-write-ring-unit build/reorder-unit build/waits-unit
	./build/config-parser-test
	./build/flipctl-unit
	./build/read-local-ring-unit
	./build/read-local-write-ring-unit
	./build/reorder-unit
	./build/waits-unit

# Deterministic core regressions: the test TU instantiates the real executor/IO methods
# with ASAN/UBSAN and test-only interleaving hooks. No server or ring is started.
CORE_TEST_OBJ := $(filter-out build/src/main.o build/src/core/genthread.o,$(OBJ))
build/rehash-waits-unit: tests/rehash_waits_unit.cc $(CORE_TEST_OBJ) $(wildcard src/*/*.h) Makefile
	$(CXX) $(CXXFLAGS) $(JEFLAGS) -I. $< $(CORE_TEST_OBJ) -o $@ $(JELIBS) $(LDLIBS) -lm

build/core-concurrency-unit: tests/core_concurrency_unit.cc $(CORE_TEST_OBJ) $(wildcard src/*/*.h)
	$(CXX) $(CXXFLAGS) $(JEFLAGS) -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -DTOMO_CORE_CONCURRENCY_TEST -I. $< $(CORE_TEST_OBJ) -o $@ \
	  $(JELIBS) $(LDLIBS) -lm

# Directed owner-phase tests. The test includes xshard.cc to drive the real private phases
# without starting worker threads or opening a listener; all other code is the release objects.
build/atomic-survivors-unit: tests/atomic_survivors_unit.cc src/cmd/xshard.cc $(filter-out build/src/main.o build/src/cmd/xshard.o,$(OBJ)) $(wildcard src/*/*.inc) $(wildcard src/*/*.h) Makefile
	$(CXX) $(CXXFLAGS) $(JEFLAGS) -I. tests/atomic_survivors_unit.cc \
	  $(filter-out build/src/main.o build/src/cmd/xshard.o,$(OBJ)) -o $@ $(JELIBS) $(LDLIBS) -lm

# Load drivers: not part of `all`, kept compiling here so they cannot rot unnoticed.
build/benchtxn: tools/benchtxn.cc Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I. tools/benchtxn.cc -o $@
build/broaden-bench: tests/broaden_bench.cc Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I. tests/broaden_bench.cc -o $@
tools: build/benchtxn build/broaden-bench

clean:
	rm -rf build
.PHONY: all asan tsan noreserve clean unit tools

# Deterministic networking/command regressions. These TUs include the real private implementations;
154-tools: build/benchtxn build/broaden-bench
155-
156-clean:
157-	rm -rf build
158-.PHONY: all asan tsan noreserve clean unit tools
159-
160-# Deterministic networking/command regressions. These TUs include the real private implementations;
161-# omit their production objects from this serverless binary. No server threads or gate are started.
162:NETCMD_TEST_SRC := tests/netcmd_unit.cc tests/netcmd_stream_unit.cc tests/netcmd_zset_unit.cc tests/netcmd_config_unit.cc
163-NETCMD_TEST_OBJ := $(NETCMD_TEST_SRC:tests/%.cc=build/tests/%.o)
164-NETCMD_LIB_OBJ := $(filter-out build/src/main.o build/src/cmd/xshard.o build/src/cmd/t_stream_groups.o build/src/cmd/t_zset.o build/src/cmd/server_tail.o,$(OBJ))
165:build/tests/%.o: tests/%.cc tests/netcmd_unit.h $(wildcard src/*/*.h) $(wildcard src/*/*.cc) $(wildcard src/*/*.inc) Makefile
166-	@mkdir -p $(dir $@)
167-	$(CXX) $(CXXFLAGS) $(JEFLAGS) -Wno-mismatched-new-delete -I. -c $< -o $@
168:build/netcmd-unit: $(NETCMD_TEST_OBJ) $(NETCMD_LIB_OBJ)
169-	$(CXX) $(CXXFLAGS) $^ -o $@ $(JELIBS) $(LDLIBS) -lm -Wl,--wrap=mkstemp -Wl,--wrap=fopen
    pausable make build/store-regression-tsan >/tmp/gate-store-flags-build.txt 2>&1 && STORE_CASE_BUILT=1
    STORE_RUN=(setarch x86_64 -R ./build/store-regression-tsan flags)
  fi
  quiet_wait
  if [ "$STORE_CASE_BUILT" = 1 ] && \
      timeout --foreground "$GATE_TEST_TIMEOUT" "${STORE_RUN[@]}" \
          >/tmp/gate-store-$STORE_CASE.txt 2>&1; then
    ok "storage $STORE_CASE regression"
  else
    bad "storage $STORE_CASE regression" "see /tmp/gate-store-build.txt, /tmp/gate-store-$STORE_CASE-build.txt and /tmp/gate-store-$STORE_CASE.txt"
  fi
done
pausable make build/store-regression-sidecar >/tmp/gate-store-sidecar.txt 2>&1 \
    && timeout --foreground "$GATE_TEST_TIMEOUT" ./build/store-regression-sidecar deadline-sidecar \
        >>/tmp/gate-store-sidecar.txt 2>&1 \
    && ok "storage deadline-sidecar regression" \
    || bad "storage deadline-sidecar regression" "see /tmp/gate-store-sidecar.txt"
# SURVIVING.md's atomic lane: one build and fifteen named, deterministic defect rows.
# Counted by line: all sixteen are ABOVE the quick-tier exit, so both tiers gain sixteen.
# EXPECT_QUICK/EXPECT_FULL are deliberately left to the maintainer (see FIXES-ATOMICS.md).
pausable make -j2 build/atomic-survivors-unit >/tmp/gate-atomic-survivors-build.txt 2>&1 \
    && ok "atomic survivors unit build" \
    || bad "atomic survivors unit build" "see /tmp/gate-atomic-survivors-build.txt"
for defect in admission closure script_keys rename_overlay write_latest script_apply \
              lua_conversion watch_parent watch_cycle mset_arity watch_oom lua_lines \
              library_limit stage_flag instruction_limit; do
  quiet_wait
  taskset -c "$CORES" timeout --foreground 30 ./build/atomic-survivors-unit "$defect" \
      >"/tmp/gate-atomic-survivors-$defect.txt" 2>&1 \
      && ok "atomic survivor: $defect" \
      || bad "atomic survivor: $defect" "see /tmp/gate-atomic-survivors-$defect.txt"
done
# Surviving networking/command audit: deterministic serverless failure states, both tiers.
pausable make -j2 build/netcmd-unit >/tmp/gate-netcmd-build.txt 2>&1 \
    && ok "netcmd regression build" || bad "netcmd regression build" "see /tmp/gate-netcmd-build.txt"
for NETCMD_CASE in streams zpop notify-oom notify-retry flush output pubsub receive config; do
  timeout 60 ./build/netcmd-unit "$NETCMD_CASE" >/tmp/gate-netcmd-$NETCMD_CASE.txt 2>&1 \
      && ok "netcmd $NETCMD_CASE regression" \
      || bad "netcmd $NETCMD_CASE regression" "see /tmp/gate-netcmd-$NETCMD_CASE.txt"
done
if [ "$ORACLE_OK" = 1 ]; then
  python3 tools/gen_acl_categories.py --redis-root "$REDIS74_ROOT" \
      --check src/cmd/acl_categories_generated.h \
      && ok "generated Redis 7.4 ACL categories" || bad "generated Redis 7.4 ACL categories"
else
  bad "generated Redis 7.4 ACL categories" "oracle source tree missing at $REDIS74_ROOT"
fi

# ---- 2. boot matrix: deleted flags stay dead; live grammar boots ------------------------------
./build/tomokv --mode 3s      2>&1 | grep -q "unknown" && ok "reject --mode (flag deleted)" || bad "reject --mode (flag deleted)"
./build/tomokv --spread 4:4   2>&1 | grep -q "unknown"  && ok "reject --spread"    || bad "reject --spread"
8-## Fixed findings and negative controls
9-
10:`live` means `tests/netcmd.py`, added to the shared feature-battery list. `unit NAME` means
11-`./build/netcmd-unit NAME`. The unit binary includes the actual private command implementations
12-and omits their duplicate production objects at link time. Its allocation faults and file-open
--
19-| **15 — C cmdsem F02, notification double destruction** | Remove the catch that repeats the shared_ptr constructor's deleter and reservation release. | `unit notify-oom` fails exactly the control-block allocation after creating a nonempty chain, asserts that failure fired, and checks the reservation is zero. Its success control checks 0→1→0. Restore the catch: double free or reservation-underflow abort. |
20-| **19 — C deadconf F01 / S atomics 8, Lua conversion** | Inspect result/error-table fields with raw Lua lookups, including nested verbatim fields. Conversion cannot invoke `__index`. | `live` returns metatables whose `__index` either throws or loops forever, requiring an empty array. Restore metamethod-aware lookups: crash or bounded client timeout. |
21:| **20 — C netproto 2, disabled output limits** | Every untracked Wb specialization stops stale client accounting before staging uncounted bytes, including TLS preparation. | `unit output` starts with tracking armed and a drained zero balance, disables the global selector, stages a seven-byte reply through the real Wb prepare path, and completes its send. Remove the stop: subtraction from zero aborts, independently of cron timing. |
22-| **27 — S atomics 13 / S deadconf F07, Lua reply injection** | Sanitize CR/LF in Lua `ok` and `err` simple-frame values. | `live` pipelines each injected table and a uniquely labelled PING, requiring exactly the intended two responses. Restore direct append: injected integer 42 replaces the marker response. |
23:| **30 — S cmdsem F03, XREADGROUP delivery progress** | Allocate all PEL nodes/consumer strings privately, then transfer nodes or swap existing values without allocation. Advance the cursor only after successful preparation. | `unit streams` proves two entries and an existing consumer, fails the second libstdc++ PEL-node allocation, and requires an unchanged cursor and empty PEL. A retry must deliver both entries and leave two pending records. Restore incremental insertion/cursor advance: the state assertions fail. |
24-| **33 — S cmdsem F06, compact ZPOP** | Pass the already loaded compact index to rank erasure. A successful reply no longer precedes a second fallible load. | `unit zpop` proves compact encoding and exercises MIN/MAX. Failing the first load must return an error without removal. Arming failure for a second allocation must find only the first load, return the member, and remove it. Restore the second load: the allocation/state checks fail. |
25-| **34 — S cmdsem F07, stream-group observation** | XGROUP's successful CREATE/DESTROY/CREATECONSUMER/DELCONSUMER/SETID branches and XSETID record their stream events. Their armed registry entries install notification context. | `unit streams` arms only the save observer on an existing stream/group, creates a new consumer, and requires exactly one save change. Remove the CREATECONSUMER record or armed wrapper: the consumer exists but the save count stays unchanged. |
26:| **35 — S cmdsem F09, premature FLUSH invalidation** | Move tracking flush publication from dispatch to the completed scatter retirement hook, after all owner fragments finish. | `unit flush` sends the actual dispatch gate through a two-IO fixture and requires no foreign event before owner work, then requires TrackingFlush at completion. Restore early publication: the first assertion fails. `live` also requires an invalidation after FLUSHALL and an absent key on the following tracked GET; remove the completion call and it times out. |
27:| **36 — S cmdsem F10, dropped invalidation** | Keep the keyless record on failed allocation/post. Retries re-sample the current IO destinations so migration cannot strand the record or miss a moved tracker; previously delivered invalidations may conservatively repeat. | `unit notify-retry` fills the actual producer marker lane while no pubsub burst is notified, proves the failed post enqueued nothing and retained the record, drains the lane, then requires the exact invalidation and record completion. Restore delete-and-continue: the record is consumed prematurely. |
28-| **37 — S cmdsem F11, XREAD tracking** | Use XREAD's dynamic STREAMS parser to register its key half. Parse into a scratch Op so tracking cannot append a second error to a malformed command's reply. | `live` arms ordinary RESP3 tracking, performs XREAD without BCAST or a preceding static-key read, appends from another connection, and requires the exact invalidation. Restore the static first_key early return: no invalidation arrives. Malformed XREAD plus PING also checks that registration emits no extra error frame. |
29-| **39 — S cmdsem F13, empty XPENDING reply** | Parse both bounds/count first and explicitly emit an empty array when incrementing the exclusive maximum start ID overflows. | `live` pipelines that exact boundary and a labelled PING, requiring `[]` before the marker. Restore the silent false return: the PING is mistaken for XPENDING's reply. |
30-| **40 — S cmdsem F14, NOGROUP framing** | Both NOGROUP builders sanitize binary key/group line delimiters. | `live` exercises missing-key XPENDING and missing-group XGROUP with embedded CR/LF plus a following PING. Restore raw append: an injected/malformed frame consumes the marker position. |
31-| **45 — S deadconf F02, rewrite loses boot settings** | Preserve original directives absent from the runtime table, including repeatable inline users and `load`, while replacing the runtime-owned directives. | `unit config` starts with inline ACL and a recovery path containing spaces, rewrites, and requires both directives and a loadable token stream. Remove preservation: the directives disappear. |
32:| **46 — S deadconf F03, rewrite string round trip** | Quote/escape values that need it; preserve simple scalar spelling and the multi-token output-limit/save grammars. | `unit config` round-trips spaces, newline, CR, tab, quotes and backslash in the password through the actual config lexer/loader. Remove escaping: its one-token/value assertions fail. Existing `servertail.py` also retains its unchanged scalar-format and restart checks. |
33-| **47 — S deadconf F04, rewrite inode collision** | Use mkstemp for a private temporary inode per rewrite; each caller renames only its own file. | `unit config` wraps mkstemp and fopen at link time. Two writers stop after opening until both are present, then must have different inodes, both succeed, and preserve the complete expected file. Restore the shared `.rewrite.tmp` fopen: the inode assertion fails deterministically. |
34-| **50 — S deadconf F09, catchable instruction limit** | Treat `timed_out` as activation failure even when Lua's outer pcall returns success. | `live` catches the inner runaway-loop error and returns 42; the outer activation must still return an error. Restore status-only completion: 42 is returned. |
35:| **52 — S netproto 3, suppression bypasses TLS** | The cold suppression serve prepares bytes and selects the TLS pump for userspace TLS, retaining poll/error handling. | `tests/tls.py`'s handshake matrix now runs SKIP;PING;PING and another labelled PING after proving each negotiated version and the fallback counters. The existing fallback gate boot forces TLS1.2 CBC, so this cannot pass solely through kTLS encryption. Restore the plaintext pump: SSL record/framing checks fail. |
36-| **53 — S netproto 4, early errors escape suppression** | Apply the existing mark to early local completions that precede spec selection/ordinary marking. | `live` exercises unknown command, GET arity and XGROUP subcommand arity under both OFF and SKIP, with following ON/PING markers. Remove early marking: an error escapes or consumes the wrong position. |
37-| **54 — S netproto 5, PUBSUB inspection loses messages** | Append pending deferred publications for inspection/publish completions as well as subscription controls. | `unit pubsub` seeds an existing subscription and unfinished CHANNELS/NUMSUB/NUMPAT control, delivers a publication, asserts the deferred state is nonempty, and requires that frame after completion. Remove the inspection append: the frame vanishes. |
38:| **55 — S netproto 6, marked control swallows pushes** | Move a suppressed control's independent deferred publications to Wb's ordered OOB queue before discarding its command reply. | `unit pubsub` repeats the pending-state test with marked operations, including SUBSCRIBE, and requires exactly the independent message after suppressing retirement. Embed it in the marked Op again: retirement discards it. |
39-| **56 — S netproto 7, aggregate segment narrowing** | Split the two-piece overload using the same segment-length bound as the single-piece overload. | `unit receive` instantiates the real queue with an eight-byte segment bound, appends 11+8 bytes and requires three bounded segments and all 19 ordered bytes. Restore the unsplit overload: segment-count/bound assertions fail. This exercises the mechanism without allocating a >4 GiB list. The production bound remains UINT32_MAX. |
40-| **57 — S netproto 8, whole-command receive cap** | Distinguish the per-bulk parser bound from the 32-bit whole-command receive bound. Growth still requires ROB quiescence; an incomplete frame at the representation ceiling gets an explicit error. | `live` sets a 1 MiB bulk limit on a fresh connection and successfully sends/stores two legal 600 KiB MSET values. `unit receive` also requires growth beyond one bulk and forbids growth with outstanding slices. Restore a one-bulk whole-buffer cap: growth/response fails. |
41:| **58 — S netproto 9, epoll's default allowance** | Epoll passes the live limit, and the shared receive-cap correction above removes dependence on a stale per-bulk default altogether. | The directed live battery passed on epoll. `unit receive` reserves a 600 MiB receive allowance with the default argument and the 1 GiB setting without touching that payload. Restore read_space's old default-based 512 MiB ceiling: the default-allowance assertion fails. This tests the shared allowance directly; the live epoll battery does not send a 600 MiB payload. |
42-| **59 — S netproto 10, empty inline parsing** | Return a distinct Empty result and commit the consumed prefix without publishing an operation or consuming reply suppression. | `live` sends CRLF and whitespace-only lines before a valid RESP command. Restore Incomplete: the labelled reply times out. |
43-| **60 — S netproto 11, binary command-name framing** | The shared error formatter strips CR/LF from line text, covering unknown-command names and echoed arguments. | `live` uses an unknown bulk command containing an apparent integer frame and checks the following marker. Restore unsanitized reply_err: an extra frame is read. |
44:| **62 — S netproto 13, deferred output accounting** | Include Wb OOB queues and pending pubsub publications in hard/soft limit input. Deferred storage is inspected only after the chosen client's limit is enabled. | `unit output` holds an ordinary nonblocking ROB entry before Done, proves all nine bytes exist only in OOB deferral, and requires an eight-byte hard limit to close it before completion. Omit deferred bytes from the check: it does not fire. |
45-
46-## Open, with failing reproducers
--
60-  engagement checks are unchanged. The kernel's [control-record and rekey contract](https://docs.kernel.org/networking/tls.html)
61-  explains why this requires more than ignoring the receive error. Build the diagnostic with
62:  `g++ -std=c++20 tests/ktls_keyupdate.cc -o build/ktls-keyupdate -lssl -lcrypto`, then run
63-  `build/ktls-keyupdate 127.0.0.1 TLS_PORT` on a default AES-128 TLS1.3 boot. It requires its
64-  sole TLS connection to report active kTLS before sending a legal requested KeyUpdate.
--
84-
85-The remaining MVCC, transaction, script/function-library lifecycle, AOF, storage, scheduler,
86:migration and read-local retry findings are outside this lane. Core edits in this diff are
87:restricted to networking integration and a zero-layout-cost test friend declaration.
88-
89-## Gate accounting
--
92-
93-- One `netcmd regression build` row in the static section.
94:- Nine rows from `for NETCMD_CASE in streams zpop notify-oom notify-retry flush output pubsub receive config`.
95-- Four `netcmd` feature rows: two atomic settings in the split loop and two in the fused/armed loop.
96-
--
101-and add no ledger row. The two open diagnostics add no passing row.
102-
103:## Validation and limits
104-
105-- Release build and the serverless unit build succeeded with existing footprint assertions.
106-  No locked size or assertion changed. No Op, Client, ThreadCtx, Shard, FlatStore, Rob,
107-  AtomicEntry or Config layout changes.
108:- All nine selected serverless sections passed. Their tests force and assert their relevant
109-  allocation, queue, completion or file-open states; there are no timing-luck passes.
110-- The new live battery passed on split/io_uring, split/epoll, fused, and fused with read-local
--
113-- Existing streamgroups, tracking, climon2, limits, lua_scripting, bitfield, servertail,
114-  zsetops and edgeproto batteries were exercised on split and fused/armed configurations.
115:  A scalar-format regression found by servertail was corrected in the writer; its test was
116-  not weakened.
117-- The TLS1.2/TLS1.3 fallback handshake/suppression matrix passed in both thread modes.
118-- The two open diagnostics failed as described above. No full gate, benchmark or performance
119:  comparison was run. Only individually owned test-server PIDs were stopped.
120-
121-Performance and the maintainer's full gate remain unmeasured here. These are correctness fixes;

exec
/bin/bash -lc "git log --oneline --all --max-count=20 -- tests/netcmd_unit.cc && rg -n 'notify-retry|netcmd flush|netcmd output|netcmd.*FAIL' CODEX-OUT.md MERGEREST.md MERGEWAITS.md && for p in /tmp/gate-netcmd-notify-retry.txt /tmp/gate-netcmd-flush.txt /tmp/gate-netcmd-output.txt; do if test -f \""'$p"; then cat "$p"; fi; done && rg --files -g '"'AGENTS.md' -g '"'!third_party/**'"'" in /home/user/Projects/cx-final-netcmdfix
 exited 1 in 0ms:
821d207b1 Merge cx-fix-netcmd preserving Lua and sender contracts
e94b97100 WIP: codex output, unbuilt and ungated
MERGEREST.md:143:| netcmd streams, zpop, notify-oom, notify-retry, flush, output, pubsub, receive, config | 27/27 |
CODEX-OUT.md:548:| netcmd streams, zpop, notify-oom, notify-retry, flush, output, pubsub, receive, config | 27/27 |
CODEX-OUT.md:22252:+| netcmd streams, zpop, notify-oom, notify-retry, flush, output, pubsub, receive, config | 27/27 |
CODEX-OUT.md:22714:+for case in ['streams', 'zpop', 'notify-oom', 'notify-retry', 'flush', 'output', 'pubsub', 'receive', 'config']:
CODEX-OUT.md:23112:+| netcmd streams, zpop, notify-oom, notify-retry, flush, output, pubsub, receive, config | 27/27 |
CODEX-OUT.md:23574:+for case in ['streams', 'zpop', 'notify-oom', 'notify-retry', 'flush', 'output', 'pubsub', 'receive', 'config']:
CODEX-OUT.md:23939:+| netcmd streams, zpop, notify-oom, notify-retry, flush, output, pubsub, receive, config | 27/27 |
CODEX-OUT.md:24401:+for case in ['streams', 'zpop', 'notify-oom', 'notify-retry', 'flush', 'output', 'pubsub', 'receive', 'config']:
CODEX-OUT.md:24786:+| netcmd streams, zpop, notify-oom, notify-retry, flush, output, pubsub, receive, config | 27/27 |
CODEX-OUT.md:25248:+for case in ['streams', 'zpop', 'notify-oom', 'notify-retry', 'flush', 'output', 'pubsub', 'receive', 'config']:
CODEX-OUT.md:25647:+| netcmd streams, zpop, notify-oom, notify-retry, flush, output, pubsub, receive, config | 27/27 |
CODEX-OUT.md:26109:+for case in ['streams', 'zpop', 'notify-oom', 'notify-retry', 'flush', 'output', 'pubsub', 'receive', 'config']:
CODEX-OUT.md:26506:+| netcmd streams, zpop, notify-oom, notify-retry, flush, output, pubsub, receive, config | 27/27 |
CODEX-OUT.md:26968:+for case in ['streams', 'zpop', 'notify-oom', 'notify-retry', 'flush', 'output', 'pubsub', 'receive', 'config']:
CODEX-OUT.md:27385:+| netcmd streams, zpop, notify-oom, notify-retry, flush, output, pubsub, receive, config | 27/27 |
CODEX-OUT.md:27847:+for case in ['streams', 'zpop', 'notify-oom', 'notify-retry', 'flush', 'output', 'pubsub', 'receive', 'config']:
CODEX-OUT.md:28247:+| netcmd streams, zpop, notify-oom, notify-retry, flush, output, pubsub, receive, config | 27/27 |
CODEX-OUT.md:28709:+for case in ['streams', 'zpop', 'notify-oom', 'notify-retry', 'flush', 'output', 'pubsub', 'receive', 'config']:
CODEX-OUT.md:29097:+| netcmd streams, zpop, notify-oom, notify-retry, flush, output, pubsub, receive, config | 27/27 |
CODEX-OUT.md:29559:+for case in ['streams', 'zpop', 'notify-oom', 'notify-retry', 'flush', 'output', 'pubsub', 'receive', 'config']:
CODEX-OUT.md:29935:+| netcmd streams, zpop, notify-oom, notify-retry, flush, output, pubsub, receive, config | 27/27 |
CODEX-OUT.md:30397:+for case in ['streams', 'zpop', 'notify-oom', 'notify-retry', 'flush', 'output', 'pubsub', 'receive', 'config']:
--ratio: SMT placement requires even logical io and ex counts
FAIL: notification topology
--ratio: SMT placement requires even logical io and ex counts
FAIL: flush topology
timeout: the monitored command dumped core

exec
/bin/bash -lc "rg -n -C 6 'NETCMD|netcmd|clean:' Makefile && rg -n 'SMT placement requires|prepare_boot|even_ifid|affinity|allowed_cpus' src/core/server.h src/core/placement.h src/core/config.h && rg -n 'client_obuf_check|climon_flush_completed|notify_ex_pass_entry|notify_emit_record|stop_obuf_tracking|void prepare|prepare\\(' src/cmd/notify.inc src/net/wb.h src/net/client.h src/net/ioloop.h src/cmd/climon.cc src/core/server.h" in /home/user/Projects/cx-final-netcmdfix
 exited 2 in 0ms:
150-	$(CXX) $(CXXFLAGS) -I. tools/benchtxn.cc -o $@
151-build/broaden-bench: tests/broaden_bench.cc Makefile
152-	@mkdir -p build
153-	$(CXX) $(CXXFLAGS) -I. tests/broaden_bench.cc -o $@
154-tools: build/benchtxn build/broaden-bench
155-
156:clean:
157-	rm -rf build
158-.PHONY: all asan tsan noreserve clean unit tools
159-
160-# Deterministic networking/command regressions. These TUs include the real private implementations;
161-# omit their production objects from this serverless binary. No server threads or gate are started.
162:NETCMD_TEST_SRC := tests/netcmd_unit.cc tests/netcmd_stream_unit.cc tests/netcmd_zset_unit.cc tests/netcmd_config_unit.cc
163:NETCMD_TEST_OBJ := $(NETCMD_TEST_SRC:tests/%.cc=build/tests/%.o)
164:NETCMD_LIB_OBJ := $(filter-out build/src/main.o build/src/cmd/xshard.o build/src/cmd/t_stream_groups.o build/src/cmd/t_zset.o build/src/cmd/server_tail.o,$(OBJ))
165:build/tests/%.o: tests/%.cc tests/netcmd_unit.h $(wildcard src/*/*.h) $(wildcard src/*/*.cc) $(wildcard src/*/*.inc) Makefile
166-	@mkdir -p $(dir $@)
167-	$(CXX) $(CXXFLAGS) $(JEFLAGS) -Wno-mismatched-new-delete -I. -c $< -o $@
168:build/netcmd-unit: $(NETCMD_TEST_OBJ) $(NETCMD_LIB_OBJ)
169-	$(CXX) $(CXXFLAGS) $^ -o $@ $(JELIBS) $(LDLIBS) -lm -Wl,--wrap=mkstemp -Wl,--wrap=fopen
src/core/config.h:279:    uint32_t even_ifid      = 0;
src/core/config.h:737:            cfg.even_ifid = a2; cfg.even_ex = b;
src/core/config.h:949:            if (st.ratio_source) { cfg.even_ifid = cfg.even_ex = 0; st.ratio_source = 0; }
src/core/config.h:1042:    if (cfg.thread_mode == ThreadMode::Fused && (cfg.even_ifid || cfg.even_ex)) {
src/core/placement.h:157:                std::fprintf(stderr, "--place: cpu %ld is outside the affinity mask\n", cpu);
src/core/placement.h:333:                         "--ratio: SMT placement requires even logical io and ex counts\n");
src/core/placement.h:352:                if (peer_domain == kNoDomain) continue; // incomplete affinity unit is unavailable
src/core/server.h:167:    bool prepare_boot(Config& cfg) {
src/core/server.h:189:            uint32_t di = cfg.even_ifid, de = cfg.even_ex;
src/core/server.h:218:            std::fprintf(stderr, "server init requires prepare_boot before persistence recovery\n");
rg: src/net/client.h: No such file or directory (os error 2)
rg: src/net/ioloop.h: No such file or directory (os error 2)
src/core/server.h:2827:    // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
src/cmd/climon.cc:237:bool IoLoop::client_routing_prepare(ClientMigration& migration, std::string& error) {
src/cmd/climon.cc:488:void IoLoop::climon_flush_completed(Op& op) {
src/net/wb.h:215:    bool prepare(Client& c, bool& submit_allowed) {
src/net/wb.h:790:        else conn.stop_obuf_tracking();
src/net/wb.h:883:        else conn.stop_obuf_tracking();
src/cmd/notify.inc:448:uint32_t notify_ex_pass_entry(Server& server, Shard& shard, uint32_t producer,

exec
/bin/bash -lc "sed -n '1,220p' src/core/placement.h && sed -n '1,130p' FIXES-NETCMD.md && git log --oneline -12 -- src/core/server.h src/core/placement.h src/cmd/climon.cc src/net/wb.h && taskset -pc "'$$ && lscpu -e=CPU,CORE,SOCKET,NODE,ONLINE | sed -n '"'1p;46,57p' && command -v gdb" in /home/user/Projects/cx-final-netcmdfix
 succeeded in 0ms:
// placement.h — per-thread role/cpu placement and the shard migration contract.
//
// A LOGICAL THREAD IS THE DEFAULT LOCALITY UNIT. When the allowed CPU set contains sibling pairs,
// Linux's reported pair is the scheduling unit: both dense thread ids have one role and FLIP moves them
// together. There is deliberately no node layer between a thread and its cpu/L3 facts.

#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>
#include "shard.h"
#include "thread.h"
#include "../base/topology.h"

namespace tomo {

inline constexpr uint32_t kNoThread = UINT32_MAX;

struct ThreadPlacement {
    uint32_t id          = 0;
    Role     role        = Role::Idle;
    int      cpu         = -1;
    uint32_t domain      = kNoDomain;
};

class Placement {
public:
    // FUSED PLACEMENT. Every selected CPU owns clients and may own shards. The two dense views
    // intentionally contain the same tids: existing connection fanout consumes ifid_threads(),
    // while shard/snapshot fanout consumes ex_threads(). Explicit role labels remain accepted as
    // CPU selectors, but have no runtime meaning in this mode.
    bool build_fused(const Topology& topo, const char* spec) {
        clear();
        if (spec) {
            if (!build_explicit(topo, spec)) return false;
        } else {
            for (uint32_t domain = 0; domain < topo.ndomains(); domain++)
                for (int cpu : topo.cpus_in(domain)) append(Role::Ifid, cpu, domain);
        }
        if (threads_.empty()) {
            std::fprintf(stderr, "1s placement needs at least one thread\n");
            return false;
        }
        if (threads_.size() > kMaxThreads) {
            std::fprintf(stderr,
                         "1s placement has %zu threads, exceeding the %u-thread channel limit\n",
                         threads_.size(), kMaxThreads);
            return false;
        }
        ifid_.clear();
        ex_.clear();
        for (uint32_t tid = 0; tid < threads_.size(); tid++) {
            threads_[tid].role = Role::Ifid;
            ifid_.push_back(tid);
            ex_.push_back(tid);
        }
        return true;
    }

    // EVEN GLOBAL PLACEMENT. Counts are whole-server, so shapes no per-node grammar can express
    // (15:2) are first-class. IO and EX roles spread across the L3 domains as evenly as
    // integer division allows, with the roles interleaved within each domain.
    //
    // This is also the flip-era invariant in batch form: a runtime controller changing one thread's
    // role keeps these same quotas incrementally -- pick the convert candidate from the domain
    // whose count for the shrinking role sits above quota and whose count for the growing role
    // sits below it. Placement stays a pure function of (counts, topology); no node layer.
    bool build_even(const Topology& topo, uint32_t n_ifid, uint32_t n_ex,
                    bool smt_mode = false) {
        clear();
        if (smt_mode) return build_even_smt(topo, n_ifid, n_ex);
        const uint32_t nd = topo.ndomains() ? topo.ndomains() : 1;
        const uint64_t total = static_cast<uint64_t>(n_ifid) + n_ex;
        uint64_t cap = 0;
        for (uint32_t d = 0; d < nd; d++) cap += topo.cpus_in(d).size();
        if (total > cap) {
            std::fprintf(stderr, "--ratio: %llu threads but only %llu allowed cpus\n",
                         static_cast<unsigned long long>(total),
                         static_cast<unsigned long long>(cap));
            return false;
        }
        if (total > kMaxThreads) {
            std::fprintf(stderr, "--ratio: %llu threads exceeds the %u-thread channel limit\n",
                         static_cast<unsigned long long>(total), kMaxThreads);
            return false;
        }
        std::vector<uint32_t> qi(nd), qe(nd);
        auto spread = [nd](uint32_t count, std::vector<uint32_t>& q) {
            for (uint32_t d = 0; d < nd; d++) q[d] = count / nd + (d < count % nd ? 1u : 0u);
        };
        spread(n_ifid, qi); spread(n_ex, qe);
        // Capacity fix-up for domains of unequal size (declared topologies): shed ex first -- it is
        // the most placement-indifferent role -- into the domain with the most free cpus.
        for (uint32_t d = 0; d < nd; d++) {
            while (qi[d] + qe[d] > topo.cpus_in(d).size()) {
                uint32_t tgt = kNoDomain;
                uint64_t best_room = 0;
                for (uint32_t o = 0; o < nd; o++) {
                    if (o == d) continue;
                    const uint64_t used = qi[o] + qe[o];
                    const uint64_t have = topo.cpus_in(o).size();
                    if (have > used && have - used > best_room) { best_room = have - used; tgt = o; }
                }
                if (tgt == kNoDomain) {
                    std::fprintf(stderr, "--ratio: no domain has room to rebalance\n");
                    return false;
                }
                if (qe[d]) { qe[d]--; qe[tgt]++; }
                else       { qi[d]--; qi[tgt]++; }
            }
        }
        for (uint32_t d = 0; d < nd; d++) {
            const std::vector<int>& cpus = topo.cpus_in(d);
            uint32_t slot = 0;
            for (uint32_t k = 0; k < qi[d]; k++) append(Role::Ifid, cpus[slot++], d);
            for (uint32_t k = 0; k < qe[d]; k++) append(Role::Ex,   cpus[slot++], d);
        }
        return true;
    }

    // --place is intentionally a small, strict language. Boot configuration should fail at the
    // first typo instead of accepting a partial placement and letting unpinned threads float.
    bool build_explicit(const Topology& topo, const char* spec) {
        clear();
        if (!spec || !*spec) {
            std::fprintf(stderr, "--place must contain role@cpu entries\n");
            return false;
        }
        const char* p = spec;
        while (*p) {
            Role role = Role::Idle;
            if (!std::strncmp(p, "ifid@", 5)) {
                role = Role::Ifid; p += 5;
            } else if (!std::strncmp(p, "ex@", 3)) {
                role = Role::Ex; p += 3;
            } else {
                std::fprintf(stderr, "--place: expected ifid@cpu or ex@cpu near '%s'\n", p);
                return false;
            }
            if (*p < '0' || *p > '9') {
                std::fprintf(stderr, "--place: expected a non-negative cpu near '%s'\n", p);
                return false;
            }
            char* end = nullptr;
            const long cpu = std::strtol(p, &end, 10);
            if (cpu < 0 || cpu >= CPU_SETSIZE || (*end != ',' && *end != '\0')) {
                std::fprintf(stderr, "--place: invalid cpu near '%s'\n", p);
                return false;
            }
            const uint32_t domain = topo.domain_of(static_cast<int>(cpu));
            if (domain == kNoDomain) {
                std::fprintf(stderr, "--place: cpu %ld is outside the affinity mask\n", cpu);
                return false;
            }
            if (threads_.size() == kMaxThreads) {
                std::fprintf(stderr, "--place exceeds the %u-thread channel limit\n", kMaxThreads);
                return false;
            }
            append(role, static_cast<int>(cpu), domain);
            if (*end == '\0') break;
            p = end + 1;
            if (!*p) {
                std::fprintf(stderr, "--place: trailing comma\n");
                return false;
            }
        }
        return true;
    }

    // A manual topology map is complete: shard IDs name dense executor thread IDs, not CPUs.
    // Executors omitted from the map remain valid empty fillers. Runtime migration still uses
    // the ordinary ownership transaction; this map selects only initial ownership.
    bool assign_shard_homes(uint32_t nshards, const char* spec = nullptr) {
        if (ex_.empty()) return false;
        shard_home_.assign(nshards, kNoThread);
        if (!spec) {
            for (uint32_t sid = 0; sid < nshards; sid++)
                shard_home_[sid] = ex_[sid % ex_.size()];
            return true;
        }
        if (!*spec) {
            std::fprintf(stderr, "--shard-home must contain shard:thread pairs\n");
            return false;
        }
        std::vector<bool> seen(nshards, false);
        const char* p = spec;
        while (*p) {
            uint32_t sid = 0, tid = 0;
            if (!parse_pair(p, sid, tid)) {
                std::fprintf(stderr, "--shard-home: expected shard:thread pairs near '%s'\n", p);
                return false;
            }
            if (sid >= nshards) {
                std::fprintf(stderr, "--shard-home: shard %u is outside 0..%u\n", sid, nshards - 1);
                return false;
            }
            if (tid >= threads_.size() || !is_executor(tid)) {
                std::fprintf(stderr, "--shard-home: thread %u is not an ex thread\n", tid);
                return false;
            }
            if (seen[sid]) {
                std::fprintf(stderr, "--shard-home: shard %u is assigned more than once\n", sid);
                return false;
            }
            seen[sid] = true;
            shard_home_[sid] = tid;
            if (*p == '\0') break;
            p++;
            if (!*p) {
                std::fprintf(stderr, "--shard-home: trailing comma\n");
                return false;
            }
        }
        for (uint32_t sid = 0; sid < nshards; sid++) {
            if (!seen[sid]) {
# Networking and command fixes

`SURVIVING.md` repeats its list. IDs below refer to its numbered findings, counted once.
This diff fixes 28 findings. Two additional findings remain open, with runnable failing
reproducers. Concurrency, storage, transaction-engine and boot-parser findings are left to
those lanes; the shared boundaries are listed below.

## Fixed findings and negative controls

`live` means `tests/netcmd.py`, added to the shared feature-battery list. `unit NAME` means
`./build/netcmd-unit NAME`. The unit binary includes the actual private command implementations
and omits their duplicate production objects at link time. Its allocation faults and file-open
barrier exist only in that executable. None of the selected regressions has a skip path.

| Finding | Change | Regression and how to make it fail |
|---|---|---|
| **6 — S cmdsem F15, runtime string limit (MISPRICED)** | APPEND, SETRANGE, bit offsets and BITFIELD use the published runtime bulk limit. BITFIELD checks the last accessed bit too. The atomic limit load stays in those existing cold bounds checks; GET/SET acquire no new load. | `live` confirms CONFIG SET/GET of 1 MiB, rejects growing past it, and verifies unchanged values/absent destination. Restore the private 512 MiB constants: these operations succeed and the row fails. |
| **14 — C cmdsem F01, XAUTOCLAIM** | Reserve at most the actual PEL size, not an arbitrary COUNT. | `live` creates an empty group and uses COUNT 9223372036854775807, requiring `[0-0, [], []]`. Restore `reserve(count)`: length_error terminates the server. |
| **15 — C cmdsem F02, notification double destruction** | Remove the catch that repeats the shared_ptr constructor's deleter and reservation release. | `unit notify-oom` fails exactly the control-block allocation after creating a nonempty chain, asserts that failure fired, and checks the reservation is zero. Its success control checks 0→1→0. Restore the catch: double free or reservation-underflow abort. |
| **19 — C deadconf F01 / S atomics 8, Lua conversion** | Inspect result/error-table fields with raw Lua lookups, including nested verbatim fields. Conversion cannot invoke `__index`. | `live` returns metatables whose `__index` either throws or loops forever, requiring an empty array. Restore metamethod-aware lookups: crash or bounded client timeout. |
| **20 — C netproto 2, disabled output limits** | Every untracked Wb specialization stops stale client accounting before staging uncounted bytes, including TLS preparation. | `unit output` starts with tracking armed and a drained zero balance, disables the global selector, stages a seven-byte reply through the real Wb prepare path, and completes its send. Remove the stop: subtraction from zero aborts, independently of cron timing. |
| **27 — S atomics 13 / S deadconf F07, Lua reply injection** | Sanitize CR/LF in Lua `ok` and `err` simple-frame values. | `live` pipelines each injected table and a uniquely labelled PING, requiring exactly the intended two responses. Restore direct append: injected integer 42 replaces the marker response. |
| **30 — S cmdsem F03, XREADGROUP delivery progress** | Allocate all PEL nodes/consumer strings privately, then transfer nodes or swap existing values without allocation. Advance the cursor only after successful preparation. | `unit streams` proves two entries and an existing consumer, fails the second libstdc++ PEL-node allocation, and requires an unchanged cursor and empty PEL. A retry must deliver both entries and leave two pending records. Restore incremental insertion/cursor advance: the state assertions fail. |
| **33 — S cmdsem F06, compact ZPOP** | Pass the already loaded compact index to rank erasure. A successful reply no longer precedes a second fallible load. | `unit zpop` proves compact encoding and exercises MIN/MAX. Failing the first load must return an error without removal. Arming failure for a second allocation must find only the first load, return the member, and remove it. Restore the second load: the allocation/state checks fail. |
| **34 — S cmdsem F07, stream-group observation** | XGROUP's successful CREATE/DESTROY/CREATECONSUMER/DELCONSUMER/SETID branches and XSETID record their stream events. Their armed registry entries install notification context. | `unit streams` arms only the save observer on an existing stream/group, creates a new consumer, and requires exactly one save change. Remove the CREATECONSUMER record or armed wrapper: the consumer exists but the save count stays unchanged. |
| **35 — S cmdsem F09, premature FLUSH invalidation** | Move tracking flush publication from dispatch to the completed scatter retirement hook, after all owner fragments finish. | `unit flush` sends the actual dispatch gate through a two-IO fixture and requires no foreign event before owner work, then requires TrackingFlush at completion. Restore early publication: the first assertion fails. `live` also requires an invalidation after FLUSHALL and an absent key on the following tracked GET; remove the completion call and it times out. |
| **36 — S cmdsem F10, dropped invalidation** | Keep the keyless record on failed allocation/post. Retries re-sample the current IO destinations so migration cannot strand the record or miss a moved tracker; previously delivered invalidations may conservatively repeat. | `unit notify-retry` fills the actual producer marker lane while no pubsub burst is notified, proves the failed post enqueued nothing and retained the record, drains the lane, then requires the exact invalidation and record completion. Restore delete-and-continue: the record is consumed prematurely. |
| **37 — S cmdsem F11, XREAD tracking** | Use XREAD's dynamic STREAMS parser to register its key half. Parse into a scratch Op so tracking cannot append a second error to a malformed command's reply. | `live` arms ordinary RESP3 tracking, performs XREAD without BCAST or a preceding static-key read, appends from another connection, and requires the exact invalidation. Restore the static first_key early return: no invalidation arrives. Malformed XREAD plus PING also checks that registration emits no extra error frame. |
| **39 — S cmdsem F13, empty XPENDING reply** | Parse both bounds/count first and explicitly emit an empty array when incrementing the exclusive maximum start ID overflows. | `live` pipelines that exact boundary and a labelled PING, requiring `[]` before the marker. Restore the silent false return: the PING is mistaken for XPENDING's reply. |
| **40 — S cmdsem F14, NOGROUP framing** | Both NOGROUP builders sanitize binary key/group line delimiters. | `live` exercises missing-key XPENDING and missing-group XGROUP with embedded CR/LF plus a following PING. Restore raw append: an injected/malformed frame consumes the marker position. |
| **45 — S deadconf F02, rewrite loses boot settings** | Preserve original directives absent from the runtime table, including repeatable inline users and `load`, while replacing the runtime-owned directives. | `unit config` starts with inline ACL and a recovery path containing spaces, rewrites, and requires both directives and a loadable token stream. Remove preservation: the directives disappear. |
| **46 — S deadconf F03, rewrite string round trip** | Quote/escape values that need it; preserve simple scalar spelling and the multi-token output-limit/save grammars. | `unit config` round-trips spaces, newline, CR, tab, quotes and backslash in the password through the actual config lexer/loader. Remove escaping: its one-token/value assertions fail. Existing `servertail.py` also retains its unchanged scalar-format and restart checks. |
| **47 — S deadconf F04, rewrite inode collision** | Use mkstemp for a private temporary inode per rewrite; each caller renames only its own file. | `unit config` wraps mkstemp and fopen at link time. Two writers stop after opening until both are present, then must have different inodes, both succeed, and preserve the complete expected file. Restore the shared `.rewrite.tmp` fopen: the inode assertion fails deterministically. |
| **50 — S deadconf F09, catchable instruction limit** | Treat `timed_out` as activation failure even when Lua's outer pcall returns success. | `live` catches the inner runaway-loop error and returns 42; the outer activation must still return an error. Restore status-only completion: 42 is returned. |
| **52 — S netproto 3, suppression bypasses TLS** | The cold suppression serve prepares bytes and selects the TLS pump for userspace TLS, retaining poll/error handling. | `tests/tls.py`'s handshake matrix now runs SKIP;PING;PING and another labelled PING after proving each negotiated version and the fallback counters. The existing fallback gate boot forces TLS1.2 CBC, so this cannot pass solely through kTLS encryption. Restore the plaintext pump: SSL record/framing checks fail. |
| **53 — S netproto 4, early errors escape suppression** | Apply the existing mark to early local completions that precede spec selection/ordinary marking. | `live` exercises unknown command, GET arity and XGROUP subcommand arity under both OFF and SKIP, with following ON/PING markers. Remove early marking: an error escapes or consumes the wrong position. |
| **54 — S netproto 5, PUBSUB inspection loses messages** | Append pending deferred publications for inspection/publish completions as well as subscription controls. | `unit pubsub` seeds an existing subscription and unfinished CHANNELS/NUMSUB/NUMPAT control, delivers a publication, asserts the deferred state is nonempty, and requires that frame after completion. Remove the inspection append: the frame vanishes. |
| **55 — S netproto 6, marked control swallows pushes** | Move a suppressed control's independent deferred publications to Wb's ordered OOB queue before discarding its command reply. | `unit pubsub` repeats the pending-state test with marked operations, including SUBSCRIBE, and requires exactly the independent message after suppressing retirement. Embed it in the marked Op again: retirement discards it. |
| **56 — S netproto 7, aggregate segment narrowing** | Split the two-piece overload using the same segment-length bound as the single-piece overload. | `unit receive` instantiates the real queue with an eight-byte segment bound, appends 11+8 bytes and requires three bounded segments and all 19 ordered bytes. Restore the unsplit overload: segment-count/bound assertions fail. This exercises the mechanism without allocating a >4 GiB list. The production bound remains UINT32_MAX. |
| **57 — S netproto 8, whole-command receive cap** | Distinguish the per-bulk parser bound from the 32-bit whole-command receive bound. Growth still requires ROB quiescence; an incomplete frame at the representation ceiling gets an explicit error. | `live` sets a 1 MiB bulk limit on a fresh connection and successfully sends/stores two legal 600 KiB MSET values. `unit receive` also requires growth beyond one bulk and forbids growth with outstanding slices. Restore a one-bulk whole-buffer cap: growth/response fails. |
| **58 — S netproto 9, epoll's default allowance** | Epoll passes the live limit, and the shared receive-cap correction above removes dependence on a stale per-bulk default altogether. | The directed live battery passed on epoll. `unit receive` reserves a 600 MiB receive allowance with the default argument and the 1 GiB setting without touching that payload. Restore read_space's old default-based 512 MiB ceiling: the default-allowance assertion fails. This tests the shared allowance directly; the live epoll battery does not send a 600 MiB payload. |
| **59 — S netproto 10, empty inline parsing** | Return a distinct Empty result and commit the consumed prefix without publishing an operation or consuming reply suppression. | `live` sends CRLF and whitespace-only lines before a valid RESP command. Restore Incomplete: the labelled reply times out. |
| **60 — S netproto 11, binary command-name framing** | The shared error formatter strips CR/LF from line text, covering unknown-command names and echoed arguments. | `live` uses an unknown bulk command containing an apparent integer frame and checks the following marker. Restore unsanitized reply_err: an extra frame is read. |
| **62 — S netproto 13, deferred output accounting** | Include Wb OOB queues and pending pubsub publications in hard/soft limit input. Deferred storage is inspected only after the chosen client's limit is enabled. | `unit output` holds an ordinary nonblocking ROB entry before Done, proves all nine bytes exist only in OOB deferral, and requires an eight-byte hard limit to close it before completion. Omit deferred bytes from the check: it does not fire. |

## Open, with failing reproducers

- **32 — S cmdsem F05: partial collection mutation on error. NOT FIXED.** Existing
  expanded hash/set/list/zset updates and field-TTL operations do not have one shared
  failure-atomic preparation/publication contract. Correcting only HSET's late TTL clear or
  notification would leave the prefix mutation and the other collection paths. A rollback
  that allocates can fail under the same condition. This diff does not claim to solve that.
  `./build/netcmd-unit collection-oom` is a diagnostic, outside the passing gate loop. It
  proves expanded encoding, arms `a`'s field TTL, fails the second 256-byte replacement, and
  checks the old value. **Run here: it failed at the changed-prefix assertion after proving
  exactly one injected allocation failure.** The same injector can verify a future fix.
- **61 — S netproto 12: kTLS KeyUpdate. NOT FIXED.** The raw receive path needs ancillary
  control-record handling and coordinated receive/transmit rekeying; changing an errno
  branch cannot supply those protocols. TLS1.3 kTLS remains enabled and its existing
  engagement checks are unchanged. The kernel's [control-record and rekey contract](https://docs.kernel.org/networking/tls.html)
  explains why this requires more than ignoring the receive error. Build the diagnostic with
  `g++ -std=c++20 tests/ktls_keyupdate.cc -o build/ktls-keyupdate -lssl -lcrypto`, then run
  `build/ktls-keyupdate 127.0.0.1 TLS_PORT` on a default AES-128 TLS1.3 boot. It requires its
  sole TLS connection to report active kTLS before sending a legal requested KeyUpdate.
  **Run here: kTLS arming passed, then the post-update TLS read failed.** No skip or userspace
  fallback counts as a pass. This open diagnostic is not added to the passing gate loop.

## Other lane boundaries and disputes

No findings were disputed. In particular, these shared command/network-facing findings remain
real and were not silently counted as fixed:

- **3 / cmdsem F08**: WATCH dirtying after unsuccessful writes is rooted in ordinary executor
  completion and transaction observation; the transaction/concurrency lane owns it.
- **4 / deadconf F08**: reading INFO/RESETSTAT counters safely requires changing the plain
  counter writers too; a cmd-only atomic read would retain the race.
- **17 / concurrency 3 + netproto 1**: the Done-to-notify Client lifetime belongs to the
  executor/IO reclamation protocol. This diff does not add or claim an executor-tail reference.
- **31 / cmdsem F04 + storage 10** and **38 / cmdsem F12 + storage 12**: snapshot-safe field
  expiry and both lazy/active hash accounting are storage-lane work. Neither half was patched
  independently here.
- **48–49 / deadconf F05–F06**: boot-password NUL and missing-value validation are in the config
  parser. Rewriter quoting is not a fix for either boot-parser defect.

The remaining MVCC, transaction, script/function-library lifecycle, AOF, storage, scheduler,
migration and read-local retry findings are outside this lane. Core edits in this diff are
restricted to networking integration and a zero-layout-cost test friend declaration.

## Gate accounting

No EXPECT constant was edited. The new rows are all **above** the quick-tier exit:

- One `netcmd regression build` row in the static section.
- Nine rows from `for NETCMD_CASE in streams zpop notify-oom notify-retry flush output pubsub receive config`.
- Four `netcmd` feature rows: two atomic settings in the split loop and two in the fused/armed loop.

Thus **+14 quick and +14 full**. From the documented inherited **327 quick / 344 full**
program, the expected totals become **341 quick / 358 full**, without the optional NIC row.
The inherited literal constants are already 325/342 (two below that program); simply adding
14 to those stale literals would still be wrong. The TLS additions strengthen an existing row
and add no ledger row. The two open diagnostics add no passing row.

## Validation and limits

- Release build and the serverless unit build succeeded with existing footprint assertions.
  No locked size or assertion changed. No Op, Client, ThreadCtx, Shard, FlatStore, Rob,
  AtomicEntry or Config layout changes.
- All nine selected serverless sections passed. Their tests force and assert their relevant
  allocation, queue, completion or file-open states; there are no timing-luck passes.
- The new live battery passed on split/io_uring, split/epoll, fused, and fused with read-local
  armed, with both atomic settings represented. Split boots used cores 0–7, 16 shards, 6:2;
  fused used the same cores/shards and correctly omitted the unsupported ratio option.
- Existing streamgroups, tracking, climon2, limits, lua_scripting, bitfield, servertail,
  zsetops and edgeproto batteries were exercised on split and fused/armed configurations.
  A scalar-format regression found by servertail was corrected in the writer; its test was
  not weakened.
- The TLS1.2/TLS1.3 fallback handshake/suppression matrix passed in both thread modes.
- The two open diagnostics failed as described above. No full gate, benchmark or performance
  comparison was run. Only individually owned test-server PIDs were stopped.

Performance and the maintainer's full gate remain unmeasured here. These are correctness fixes;
there is no PRE/POST performance claim. The user-owned CODEX-OUT.md change and duplicated
SURVIVING.md input are not part of these edits.
818d458ef Merge cx-waits and add resize and retirement liveness batteries
821d207b1 Merge cx-fix-netcmd preserving Lua and sender contracts
112ed6679 Merge cx-integ preserving deterministic windows and concurrency guards
6647031dd merge cx-fix-concurrency
b4013b752 Merge cx-rl2s and connect local reads to both overlap schedules
4938c609c Merge cx-overlap with its working-tree gate fixes
e94b97100 WIP: codex output, unbuilt and ungated
a16665c0c WIP: codex output, unbuilt and ungated
013867fab WIP: codex output, unbuilt and ungated
0d8044b95 WIP cx-overlap: codex output, unbuilt and ungated
c33228203 WIP waits: codex output, unbuilt and unmeasured
0c8cbca76 WIP rl2s: codex output, unbuilt and unmeasured
pid 4077056's current affinity list: 44-55
CPU CORE SOCKET NODE ONLINE
 44   44      0    0    yes
 45   45      0    0    yes
 46   46      0    0    yes
 47   47      0    0    yes
 48   48      0    0    yes
 49   49      0    0    yes
 50   50      0    0    yes
 51   51      0    0    yes
 52   52      0    0    yes
 53   53      0    0    yes
 54   54      0    0    yes
 55   55      0    0    yes
/usr/bin/gdb

codex
The saved logs show `notify-retry` and `flush` failing during topology setup, before their assertions run; `output` crashes. These rows use a serverless unit battery. I’m building it from clean and checking both the fixture contracts and the production paths they exercise.

exec
/bin/bash -lc 'taskset -c 44-55 make clean && mkdir -p build/netcmdfix && taskset -c 44-55 make -j6 build/netcmd-unit build/tomokv > build/netcmdfix/build-pre.log 2>&1' in /home/user/Projects/cx-final-netcmdfix
exec
/bin/bash -lc "sed -n '160,220p' src/core/server.h && sed -n '1,240p' src/base/topology.h && rg -n 'client_obuf_check|close_client\\(' src/io src/core/client.h && sed -n '455,530p' src/cmd/climon.cc && sed -n '430,520p' src/cmd/notify.inc && sed -n '185,245p' src/net/wb.h && sed -n '740,925p' src/net/wb.h" in /home/user/Projects/cx-final-netcmdfix
 exited 2 in 0ms:

    Server() = default;
    friend struct NetcmdRegression;
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Resolve placement and the shard default once, before AOF/snapshot recovery reads geometry.
    bool prepare_boot(Config& cfg) {
        const char* domains = std::getenv("TOMOKV_L3_DOMAINS");
        if (domains && *domains) {
            if (!topo_.declare(domains)) {
                std::fprintf(stderr, "fatal: TOMOKV_L3_DOMAINS '%s' invalid\n", domains);
                return false;
            }
        } else if (!topo_.discover()) {
            std::fprintf(stderr, "fatal: could not discover any allowed cpu\n");
            return false;
        }
        if (!topo_.discover_thread_siblings()) {
            std::fprintf(stderr, "fatal: could not read Linux thread_siblings_list topology\n");
            return false;
        }
        smt_mode_ = topo_.allowed_sibling_pairs() != 0;
        bool placed = false;
        if (cfg.thread_mode == ThreadMode::Fused) {
            placed = placement_.build_fused(topo_, cfg.place);
        } else {
            // Default shape: an even io/ex split of every allowed cpu, io taking the odd one out --
            // the measured 2s center (4:4-class) generalized to any core count.
            uint32_t di = cfg.even_ifid, de = cfg.even_ex;
            if (!cfg.place && !(di | de)) {
                uint32_t n = 0;
                for (uint32_t d = 0; d < topo_.ndomains(); d++)
                    n += static_cast<uint32_t>(topo_.cpus_in(d).size());
                if (smt_mode_) {
                    n = 2 * topo_.allowed_sibling_pairs();
                    de = (n / 4) * 2;
                } else de = n / 2;
                di = n - de;
            }
            placed = cfg.place
                ? placement_.build_explicit(topo_, cfg.place)
                : placement_.build_even(topo_, di, de, smt_mode_ != 0);
        }
        if (!placed) return false;
        if (!placement_.configure_smt_units(topo_, smt_mode_ != 0)) return false;
        if (!placement_.reserve_runtime_roles(placement_.total_threads())) return false;
        if (placement_.ifid_threads().empty() || placement_.ex_threads().empty()) {
            std::fprintf(stderr, "placement needs at least one ifid and one ex thread\n");
            return false;
        }
        if (cfg.shards == Config::kShardsAuto)
            cfg.shards = cfg_default_shards(static_cast<uint32_t>(placement_.ex_threads().size()));
        return true;
    }

    bool init(const Config& cfg, const AofReplayPlan* aof_replay = nullptr) {
        if (placement_.total_threads() == 0) {
            std::fprintf(stderr, "server init requires prepare_boot before persistence recovery\n");
            return false;
        }
// topology.h — what the machine actually looks like, discovered rather than assumed.
//
// DOMAINS ARE SHARED-L3 CPU GROUPS, and this file discovers those boundaries without imposing a
// placement unit on them. Individual threads may be placed anywhere in the allowed affinity mask;
// the domain lookup says which cache each chosen cpu belongs to.
//
// Nothing here DECIDES placement (that is placement.h) and nothing here is consulted on the hot
// path. It records what a later flip/LB controller needs in order to decide:
//
//   - which L3 domain each CPU belongs to
//   - which domain a worker is currently running in
//   - which domain a shard's working set is currently resident in
//   - how many ops were executed from a FOREIGN domain (the actionable signal)
//
// WHY L3 AND NOT NUMA. This box is NPS1: one NUMA node over 128 GB, so there is no NUMA distance to
// exploit and the only locality available is cache locality. A "NUMA-sharded" store here is really
// an L3-sharded store, and calling it that avoids a whole class of wrong reasoning.
//
// WHY THIS MATTERS FOR MIGRATION, which is the part that is easy to miss: an L3 domain is not
// allocated into, it is filled by access. Moving a shard from a worker in domain A to a worker in
// domain B does not move its memory — it invalidates its residency. The new worker must re-pull the
// working set through the fabric, and on this box a CCX-to-fabric link saturates around 51 GB/s
// where a single core can already pull 50. So shard migration is NOT free, and an LB that treats it
// as free will thrash. resident_estimate() exists so the cost can be priced.
#pragma once
#include <sched.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

namespace tomo {

inline constexpr uint32_t kNoDomain = UINT32_MAX;

class Topology {
public:
    // Discovers L3 domains for the CPUs this process is ALLOWED to run on. Respecting the affinity
    // mask matters: under taskset the machine's full topology is not what we get, and assuming
    // otherwise is how threads end up "placed" onto cpus they can never run on.
    // TOMOKV_L3_DOMAINS overrides broken sysfs: comma-separated domains, '+' joins CPU ranges.
    // Every declared CPU must belong to the allowed affinity mask.
    bool declare(const char* spec) {
        cpu_set_t allowed;
        CPU_ZERO(&allowed);
        if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return false;
        domain_of_.assign(CPU_SETSIZE, kNoDomain);
        domains_.clear();

        if (!spec || !*spec) return false;
        const char* p = spec;
        while (*p) {
            std::vector<int> cpus;
            while (*p && *p != ',') {
                char* end = nullptr;
                long a = std::strtol(p, &end, 10);
                if (end == p) return false;
                long b = a;
                if (*end == '-') { p = end + 1; b = std::strtol(p, &end, 10); if (end == p) return false; }
                if (a < 0 || b < a || b >= CPU_SETSIZE ||
                    (*end && *end != '+' && *end != ',')) return false;
                for (long c = a; c <= b; c++) {
                    if (domain_of_[c] != kNoDomain ||
                        std::find(cpus.begin(), cpus.end(), c) != cpus.end()) return false;
                    if (!CPU_ISSET(c, &allowed)) {
                        std::fprintf(stderr, "topology: declared cpu %ld is outside the affinity mask\n", c);
                        return false;
                    }
                    cpus.push_back(static_cast<int>(c));
                }
                p = end;
                if (*p == '+') {
                    p++;
                    if (!*p || *p == ',') return false;
                }
            }
            if (cpus.empty()) return false;
            {
                const uint32_t id = static_cast<uint32_t>(domains_.size());
                for (int c : cpus) domain_of_[c] = id;
                domains_.push_back(std::move(cpus));
            }
            if (*p == ',') {
                p++;
                if (!*p || *p == ',') return false;
            }
        }
        return !domains_.empty();
    }

    bool discover() {
        cpu_set_t allowed;
        CPU_ZERO(&allowed);
        if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return false;

        domain_of_.assign(CPU_SETSIZE, kNoDomain);
        domains_.clear();

        for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
            if (!CPU_ISSET(cpu, &allowed)) continue;
            if (domain_of_[cpu] != kNoDomain) continue;

            std::vector<int> peers;
            if (!read_l3_peers(cpu, allowed, peers)) peers.push_back(cpu);   // no sysfs: own domain

            const uint32_t d = static_cast<uint32_t>(domains_.size());
            for (int p : peers) if (domain_of_[p] == kNoDomain) domain_of_[p] = d;
            domains_.push_back(std::move(peers));
        }
        return !domains_.empty();
    }

    // Read Linux's authoritative SMT relationship for every CPU in the selected topology. Keep
    // the full sysfs list (rather than deriving an offset or intersecting it with affinity): the
    // placement validator must be able to distinguish a complete pair from a taskset which made
    // only one sibling available. This is boot-only and is always read before deriving SMT placement.
    bool discover_thread_siblings() {
        thread_siblings_.assign(CPU_SETSIZE, {});
        for (const std::vector<int>& domain : domains_) {
            for (int cpu : domain) {
                if (!read_thread_siblings(cpu, thread_siblings_[cpu])) return false;
                if (std::find(thread_siblings_[cpu].begin(), thread_siblings_[cpu].end(), cpu) ==
                    thread_siblings_[cpu].end()) return false;
            }
        }
        return true;
    }

    uint32_t allowed_sibling_pairs() const {
        uint32_t pairs = 0;
        for (const auto& domain : domains_) {
            for (int cpu : domain) {
                const auto& siblings = thread_siblings(cpu);
                if (siblings.size() != 2) continue;
                const int peer = siblings[0] == cpu ? siblings[1] : siblings[0];
                if (cpu < peer && domain_of(peer) != kNoDomain) pairs++;
            }
        }
        return pairs;
    }

    uint32_t ndomains() const { return static_cast<uint32_t>(domains_.size()); }
    uint32_t domain_of(int cpu) const {
        return (cpu >= 0 && cpu < static_cast<int>(domain_of_.size())) ? domain_of_[cpu] : kNoDomain;
    }
    const std::vector<int>& cpus_in(uint32_t d) const { return domains_[d]; }
    const std::vector<int>& thread_siblings(int cpu) const {
        static const std::vector<int> empty;
        return cpu >= 0 && cpu < static_cast<int>(thread_siblings_.size())
            ? thread_siblings_[cpu] : empty;
    }

    void dump(FILE* f) const {
        std::fprintf(f, "topology: %u L3 domain(s)\n", ndomains());
        for (uint32_t d = 0; d < ndomains(); d++) {
            std::fprintf(f, "  domain %2u: %zu cpus [", d, domains_[d].size());
            for (size_t i = 0; i < domains_[d].size(); i++)
                std::fprintf(f, "%s%d", i ? "," : "", domains_[d][i]);
            std::fprintf(f, "]\n");
        }
    }

private:
    static bool read_thread_siblings(int cpu, std::vector<int>& out) {
        char path[128];
        std::snprintf(path, sizeof(path),
                      "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);
        FILE* f = std::fopen(path, "r");
        if (!f) return false;
        char buf[512] = {0};
        const bool read = std::fgets(buf, sizeof(buf), f) != nullptr;
        std::fclose(f);
        if (!read) return false;
        parse_cpu_list(buf, out);
        return !out.empty();
    }

    // Reads the CPUs sharing this cpu's level-3 cache, intersected with the allowed set.
    static bool read_l3_peers(int cpu, const cpu_set_t& allowed, std::vector<int>& out) {
        for (int idx = 0; idx < 8; idx++) {
            char path[128];
            std::snprintf(path, sizeof(path),
                          "/sys/devices/system/cpu/cpu%d/cache/index%d/level", cpu, idx);
            FILE* f = std::fopen(path, "r");
            if (!f) continue;
            int level = 0;
            if (std::fscanf(f, "%d", &level) != 1) { std::fclose(f); continue; }
            std::fclose(f);
            if (level != 3) continue;

            std::snprintf(path, sizeof(path),
                          "/sys/devices/system/cpu/cpu%d/cache/index%d/shared_cpu_list", cpu, idx);
            f = std::fopen(path, "r");
            if (!f) continue;
            char buf[512] = {0};
            if (!std::fgets(buf, sizeof(buf), f)) { std::fclose(f); continue; }
            std::fclose(f);
            parse_cpu_list(buf, allowed, out);
            return !out.empty();
        }
        return false;
    }

    // "0-7,128-135" -> the allowed members of that set.
    static void parse_cpu_list(const char* s, const cpu_set_t& allowed, std::vector<int>& out) {
        while (*s) {
            while (*s == ',' || *s == ' ' || *s == '\n') s++;
            if (!*s) break;
            char* end = nullptr;
            long a = std::strtol(s, &end, 10);
            long b = a;
            if (end && *end == '-') b = std::strtol(end + 1, &end, 10);
            for (long c = a; c <= b; c++)
                if (c >= 0 && c < CPU_SETSIZE && CPU_ISSET(static_cast<int>(c), &allowed))
                    out.push_back(static_cast<int>(c));
            s = end ? end : s + 1;
        }
    }

    // Sysfs topology parsing intentionally does not intersect the process affinity mask. A
    // sibling outside that mask is still the sibling, and pair placement must reject an incomplete unit.
    static void parse_cpu_list(const char* s, std::vector<int>& out) {
        while (*s) {
            while (*s == ',' || *s == ' ' || *s == '\n') s++;
            if (!*s) break;
            char* end = nullptr;
            long a = std::strtol(s, &end, 10);
            if (end == s) return;
            long b = a;
            if (*end == '-') {
                const char* range = end + 1;
                b = std::strtol(range, &end, 10);
                if (end == range) return;
            }
            for (long c = a; c <= b; c++)
                if (c >= 0 && c < CPU_SETSIZE) out.push_back(static_cast<int>(c));
            s = end;
        }
rg: src/io: No such file or directory (os error 2)
rg: src/core/client.h: No such file or directory (os error 2)

exec
/bin/bash -lc "rg --files src | rg 'loop|client' && git log --oneline -10 -- src/base/topology.h && git show --stat --oneline 821d207b1 && rg -n 'NetcmdRegression|climon_flush_completed' src" in /home/user/Projects/cx-final-netcmdfix
 succeeded in 0ms:
src/core/ex_loop.h
src/core/io_loop.h
c8e61f646 merge: knob-collapse + test-determinism waves, gate 342/342
53032e634 config/server: stale text after the 3s deletion and the --l3-domains rename; Config::node_cpus -> l3_domains
b7fc528fb config: add boot-only SMT pairing mode
218e15cfc cleanup: remove unused thread topology accessors
605b71a8f no stale code: legacy node-era placement sugar deleted; headers tell the 2s truth
f9ed4e4de docs: describe per-thread placement controls
4d856afb6 operator-declared topology (--node-cpus) + un-shadow the --wb knob + strict args
041c87210 placement: shared-L3 nodes as default policy, not as a fence
821d207b1 Merge cx-fix-netcmd preserving Lua and sender contracts

 FIXES-NETCMD.md             | 123 +++++++++++++++++++++
 Makefile                    |  11 ++
 src/cmd/bitfield.inc        |   3 +-
 src/cmd/climon.cc           |  44 +++++---
 src/cmd/climon.inc          |   2 +
 src/cmd/command.h           |   1 +
 src/cmd/notify.h            |   1 +
 src/cmd/notify.inc          |  57 ++++++----
 src/cmd/scripting.cc        |  36 +++---
 src/cmd/server_tail.cc      |  68 +++++++++---
 src/cmd/t_server.cc         |   6 +
 src/cmd/t_stream_groups.cc  |  65 +++++++----
 src/cmd/t_string.cc         |   7 +-
 src/cmd/t_zset.cc           |  13 ++-
 src/cmd/tracking.cc         |  22 +++-
 src/core/io_loop.h          |  26 ++++-
 src/core/pubsub.inc         |   8 ++
 src/core/server.h           |   1 +
 src/net/conn.h              |  30 +++--
 src/net/resp.h              |  17 ++-
 src/net/wb.h                |  10 ++
 tests/gate.sh               |  12 +-
 tests/ktls_keyupdate.cc     |  58 ++++++++++
 tests/netcmd.py             |  98 +++++++++++++++++
 tests/netcmd_config_unit.cc |  65 +++++++++++
 tests/netcmd_stream_unit.cc |  38 +++++++
 tests/netcmd_unit.cc        | 261 ++++++++++++++++++++++++++++++++++++++++++++
 tests/netcmd_unit.h         |  37 +++++++
 tests/netcmd_zset_unit.cc   |  29 +++++
 tests/tls.py                |   5 +
 30 files changed, 1026 insertions(+), 128 deletions(-)
src/core/io_loop.h:102:                loop->climon_flush_completed(op);
src/core/io_loop.h:445:    friend struct NetcmdRegression;
src/core/server.h:162:    friend struct NetcmdRegression;
src/cmd/climon.inc:93:    void climon_flush_completed(Op& op);
src/cmd/climon.cc:488:void IoLoop::climon_flush_completed(Op& op) {

exec
/bin/bash -lc "rg -n 'client_obuf_check|close_client\\(' src/core/io_loop.h src/net/conn.h src/cmd/climon.cc && sed -n '455,535p' src/cmd/climon.cc && sed -n '390,515p' src/cmd/notify.inc && sed -n '195,240p' src/net/wb.h && sed -n '770,815p' src/net/wb.h && sed -n '860,910p' src/net/wb.h" in /home/user/Projects/cx-final-netcmdfix
 succeeded in 0ms:
src/cmd/climon.cc:569:            if (tls->failed()) close_client(client, tls->output_pending() || client->send_inflight());
src/cmd/climon.cc:1097:                close_client(victim);
src/core/io_loop.h:133:            return static_cast<IoLoop*>(ctx)->client_obuf_check(&client, true);
src/core/io_loop.h:1229:            close_client(c);
src/core/io_loop.h:1240:        if (!tls) { close_client(c); return; }
src/core/io_loop.h:1246:                *c, *tls, cqe->res)) close_client(c);
src/core/io_loop.h:2292:                close_client(c);
src/core/io_loop.h:2368:                close_client(c);
src/core/io_loop.h:2376:        if (res <= 0) { close_client(c); return; }
src/core/io_loop.h:2418:                close_client(c);
src/core/io_loop.h:2453:                close_client(c, tls->output_pending() || c->send_inflight());
src/core/io_loop.h:2490:                close_client(c, tls->output_pending() || c->send_inflight());
src/core/io_loop.h:2498:                close_client(c, tls->output_pending() || c->send_inflight());
src/core/io_loop.h:2513:        if (!tls) { close_client(c); return; }
src/core/io_loop.h:2517:        if (c->closing() || res < 0) { close_client(c); return; }
src/core/io_loop.h:2527:        if (!tls) { close_client(c); return; }
src/core/io_loop.h:2531:            close_client(c);
src/core/io_loop.h:2537:            close_client(c);
src/core/io_loop.h:4480:        close_client(c, drain_tls_output);
src/core/io_loop.h:4788:                    close_client(c);
src/core/io_loop.h:4909:                        close_client(client,
src/core/io_loop.h:5072:                            close_client(client,
src/core/io_loop.h:5148:                        close_client(c, tls->output_pending() || c->send_inflight());
src/core/io_loop.h:5418:                    close_client(c);
src/core/io_loop.h:5460:                    if (tls->failed()) close_client(c, tls->output_pending() || c->send_inflight());
src/core/io_loop.h:5486:                    if (tls->failed()) close_client(c, tls->output_pending() || c->send_inflight());
src/core/io_loop.h:5527:                    if (tls->failed()) close_client(c, tls->output_pending() || c->send_inflight());
src/core/io_loop.h:5687:                // through mark_closing() rather than close_client() (the reply has to go out
src/core/io_loop.h:5785:                else { c->set_in_active(false); active_.erase_at(idx); close_client(c); }
src/core/io_loop.h:5855:                    if (tls->failed()) close_client(c, tls->output_pending() || c->send_inflight());
src/core/io_loop.h:5998:    bool client_obuf_check(Client* c, bool async) {
src/core/io_loop.h:6032:        close_client(c);
src/core/io_loop.h:6063:                close_client(c);
src/core/io_loop.h:6065:            } else if (client_obuf_check(c, false)) {
src/core/io_loop.h:6112:    void close_client(Client* c, bool drain_tls_output = false) {
    climon_conn_release(client->id());
    climon_refresh_armed();
}

// ---- THE armed gate ------------------------------------------------------------------------
// The one per-operation entry point this lane owns. It is reached only from inside the branch
// parse_and_dispatch already takes for keyspace notifications, and only while at least one lane
// feature is armed. Returning true HOLDS the connection: the frame stays unparsed and is retried
// on a later pass (CLIENT PAUSE).

bool IoLoop::climon_armed_gate(Client* client, Op& op) {
    const uint32_t armed = climon_armed_cached_;
    // Keyspace notifications can be configured with the lane entirely idle. That case must not
    // walk any of the machinery below.
    if (__builtin_expect(armed == 0, true)) return false;

    if (armed & Server::kClimonPause) {
        if (climon_pause_holds(op)) {
            srv_->climon_note_pause_hold();
            return true;
        }
    }
    if (armed & Server::kClimonMonitor) climon_monitor_feed(client, op);
    if (armed & Server::kClimonTracking) {
        ClimonConn* state = climon_conn_find(client->id());
        if (state && state->tracking_on) tracking_register_read(client, *state, op);
    }
    climon_mark_reply(client, op);
    return false;
}

// All owner fragments have completed before the retire callback reaches this hook. A foreign
// tracked read that follows the invalidation therefore cannot repopulate a value awaiting FLUSH.
void IoLoop::climon_flush_completed(Op& op) {
    if ((climon_armed_cached_ & Server::kClimonTracking) &&
        (op.cmd_name().eq_icase("flushall") || op.cmd_name().eq_icase("flushdb")))
        tracking_broadcast_flush();
}

void IoLoop::climon_mark_reply(Client* client, Op& op) {
    if (climon_armed_cached_ & Server::kClimonReply) {
        ClimonConn* state = climon_conn_find(client->id());
        if (state) {
            if (state->reply_mode == kClimonReplyOff) {
                op.mark_reply_skip();
            } else if (state->reply_mode == kClimonReplySkipNext) {
                // SKIP covers the command AFTER `CLIENT REPLY SKIP` itself -- which is this one.
                // Marking the Op (not the connection) is what makes a pipelined
                // SKIP;PING;PING drop exactly one reply instead of the whole batch. The
                // connection stays counted as armed until the marked op has actually been
                // served, so the suppressing serve variant is still selected when it retires.
                op.mark_reply_skip();
                state->reply_mode = kClimonReplySkipNow;
            }
        }
    }
}

// Mirrors pubsub_emit's ordering rule, and shares its machinery: an out-of-band frame is a WHOLE
// frame that must land on a frame boundary, never inside another reply's byte range. Client picks
// the output channel (see Client::append_oob); a frame raised from inside this connection's own
// retire drain -- which is where the tracking invalidation and MONITOR hooks fire, with the
// connection's newest reply only partially staged -- or behind an earlier-issued, unretired reply
// parks on the send engine until a drain stages that frontier. Only the connection's own io thread
// ever runs this.
void IoLoop::climon_push_wire(Client* client, const std::string& frame) {
    if (__builtin_expect(wb_.defer_oob(*client, frame.data(), frame.size()), false)) {
        srv_->note_oob_frame_deferred();
    } else if (__builtin_expect(client->append_oob(frame.data(), frame.size()), false)) {
        srv_->note_oob_frame_segmented();
    }
    enqueue_serve(client);
    mark_active(client);
}

bool IoLoop::climon_unblock_local(uint64_t id, bool error_flavor) {
    for (Client* c : self_->clients()) {
        if (!c || c->id() != id) continue;
        if (c->dead() || !c->blocked()) return false;
        if (!blocking_request_unblock(*c, error_flavor)) return false;
        if (blocking_cancel_client(*srv_, *self_, ring_, *c)) enqueue_serve(c);
    if (!slot) {
        slot.reset(new (std::nothrow) NotifyShardState);
        if (!slot) { server->notify_event_dropped(); return false; }
    }
    if (slot->keyless.size() >= kNotifyKeylessCap) {
        server->notify_event_dropped();
        return false;
    }
    try {
        NotifyOut out;
        out.routes = routes;
        if (!server->pubsub_any_subscribers())
            out.routes &= ~(NOTIFY_KEYSPACE | NOTIFY_KEYEVENT);
        out.event = event;
        out.key.assign(key.p, key.n);
        slot->keyless.push_back(std::move(out));
    } catch (const std::bad_alloc&) {
        server->notify_event_dropped();
        return false;
    }
    shard.notify_output_created();
    server->notify_event_fired();
    return true;
}

bool notify_flat_enabled(void* context, uint32_t cls) {
    auto& shard = *static_cast<Shard*>(context);
    const uint32_t mask = shard.notify_mask();
    const bool observed = (mask & NOTIFY_OBSERVERS) && (NOTIFY_ALL & cls);
    if (!(mask & cls) && !observed) return false;
    if (observed && (mask & NOTIFY_SAVE)) return true;
    const uint32_t routes = notify_routes_for_class(mask, cls);
    if (!routes) return false;
    if (cls == NOTIFY_KEY_MISS) {
        const Op* source = shard.notify_source();
        if (!source || !(source->spec->flags & CmdFlags::Readonly) ||
            (source->spec->flags & CmdFlags::Write)) return false;
    }
    Server* server = shard.server();
    if (!server) return false;
    // Tracking invalidation needs the expiry/eviction observers even with nobody subscribed.
    return server->pubsub_any_subscribers() || (routes & NOTIFY_TRACKING) != 0;
}

void notify_flat_emit(void* context, uint32_t cls, NotifyEventId event, Slice key) {
    auto& shard = *static_cast<Shard*>(context);
    // Hexpired/Del raised by the active field-expire cycle have no originating command either, so
    // they take the same keyless route as key expiry rather than being dropped for want of a source.
    if (event == NotifyEventId::Expired || event == NotifyEventId::Evicted ||
        event == NotifyEventId::Hexpired ||
        (event == NotifyEventId::Del && !shard.notify_source())) {
        notify_record_keyless(shard, cls, event, key);
        return;
    }
    Op* source = shard.notify_source();
    if (source) notify_record(shard, *source, cls, event, key);
}

uint32_t notify_ex_pass_entry(Server& server, Shard& shard, uint32_t producer,
                              ThreadCtx& thread, Ring& ring, LoopSignals& signals) {
    (void)thread;
    auto& slot = shard.notify_state_slot();
    if (!slot || slot->keyless.empty()) return 0;
    uint32_t work = 0;
    while (work < kNotifyKeylessPass && !slot->keyless.empty()) {
        NotifyOut& out = slot->keyless.front();
        // Lane F: an expired or evicted key is an invalidation. Post it to every io owner that
        // holds tracking connections; each filters against its own table. The executor never
        // touches a Client here -- only the same io-to-io transport pub/sub already uses.
        if (__builtin_expect((out.routes & NOTIFY_TRACKING) != 0, 0)) {
            // Re-sample destinations on every retry: tracking clients and IO roles can move
            // while this record is parked. Repeating a delivered invalidation is conservative;
            // remembering an old destination as complete can miss a newly migrated tracker.
            const uint64_t io_mask = server.climon_tracking_io_mask();
            for (uint32_t io : server.placement().ifid_threads()) {
                const uint64_t bit = uint64_t{1} << (io & 63);
                if (!(io_mask & bit)) continue;
                std::unique_ptr<PubSubEvent> invalidate;
                try {
                    invalidate = std::make_unique<PubSubEvent>();
                    invalidate->kind = PubSubEventKind::TrackingInvalidate;
                    invalidate->target_io = io;
                    invalidate->caller_id = 0;
                    invalidate->route_mask = io_mask;
                    invalidate->items.push_back(PubSubEventItem{0, false, 0, out.key});
                } catch (const std::bad_alloc&) { return work; }
                server.pubsub_event_created();
                if (!server.thread(io).post_pubsub_event_nonblocking(
                        producer, invalidate.get(), ring, signals)) {
                    server.pubsub_event_retired();
                    return work;  // keep this target and the keyless record for the next pass
                }
                invalidate.release();
            }
            out.routes &= ~NOTIFY_TRACKING;
        }
        std::unique_ptr<PubSubNotificationChain> building;
        try {
            building = std::make_unique<PubSubNotificationChain>();
            building->items.reserve(2);
            for (uint8_t route = 0; route < 2; route++) {
                std::string channel, message;
                if (!notify_make_publication(
                        server, out.routes, route, out.event,
                        Slice(out.key.data(), static_cast<uint32_t>(out.key.size())),
                        channel, message)) continue;
                building->items.push_back(
                    PubSubNotificationItem{std::move(channel), std::move(message)});
            }
        } catch (const std::bad_alloc&) {
            server.notify_event_dropped();
            return work;
        }
        std::shared_ptr<PubSubNotificationChain> chain;
        try {
            chain = notify_reserve_chain(server, std::move(building));
        } catch (const std::bad_alloc&) {
            server.notify_event_dropped();
            return work;
        }
        if (!chain) {
            // A tracking-only record has no pub/sub publication to make; its invalidation was
            // already posted above, so retiring it here is completion, not a drop.
            if (out.routes & (NOTIFY_KEYSPACE | NOTIFY_KEYEVENT)) server.notify_event_dropped();
            slot->keyless.pop_front();
            work++;
    }

    // THE WHOLE REPLY SIDE, in one call: retire completed ops IN ORDER, stage their bytes, and
    // write. All three belong together and all three belong to the sender -- if the io thread
    // retired and merely handed bytes over, only the send syscall would move between modes, and
    // an "ex-wb" measured that way would not be ex-wb. In pure 2s the sender is the connection's
    // io thread and nobody else (the Wb-thread and EX-mode callers this comment once listed were
    // deleted with those postures; see the head of this file), so no lock exists or is needed and
    // the ROB stays SPSC by construction. Returns true if it did anything, so a caller can tell
    // progress from an empty poll.
    template <bool kEp = false, bool ClassifySend = false, bool Coded = false>
    bool serve(Client& c) {
        if (__builtin_expect(limit_armed_->load(std::memory_order_relaxed), false))
            return serve_impl<true, false, kEp, true, ClassifySend, Coded>(c);
        return serve_impl<false, false, kEp, true, ClassifySend, Coded>(c);
    }

    // Micro-pipeline retirement half. It drains exactly the same in-order prefix and stages the
    // same buffers/segments as serve(), but deliberately leaves SQE construction to pump().
    template <bool kEp = false, bool Coded = false>
    bool prepare(Client& c, bool& submit_allowed) {
        submit_allowed = true;
        if (__builtin_expect(limit_armed_->load(std::memory_order_relaxed), false))
            return serve_impl<true, false, kEp, false, false, Coded>(c, &submit_allowed);
        return serve_impl<false, false, kEp, false, false, Coded>(c, &submit_allowed);
    }

    // Unified pipeline batches already guard a nullable Client slot before their submit half. Its
    // bound limit callback tombstones that slot on the rare refusal, avoiding a parallel bool on
    // every ordinary reply while leaving the established split-pipeline prepare API untouched.
    template <bool kEp = false, bool Coded = false>
    bool prepare_pipeline(Client& c) {
        if (__builtin_expect(limit_armed_->load(std::memory_order_relaxed), false))
            return serve_impl<true, false, kEp, false, false, Coded>(c);
        return serve_impl<false, false, kEp, false, false, Coded>(c);
    }

    // kTLS uses the ordinary plaintext staging and send path. This separate instantiation only
    // enforces/counts the pre-existing TLS no-borrow contract; plaintext clients pay no mode test.
    template <bool kEp = false, bool ClassifySend = false, bool Coded = false>
    bool serve_ktls(Client& c) {
        if (__builtin_expect(limit_armed_->load(std::memory_order_relaxed), false))
            return serve_impl<true, true, kEp, true, ClassifySend, Coded>(c);
        return serve_impl<false, true, kEp, true, ClassifySend, Coded>(c);
    }

    // previous version decided it at RUNTIME from a boot-latched bool, which meant serve_impl
    // became a forwarder in front of the real body -- a new function boundary in the io thread's
    // hot path that moved inlining and cost the split cells ~0.9% of real work (measured against a
    // dead-pad placement control: placement alone was -0.11%..-0.30%, the binary was -0.83%..-1.48%).
    //
    // Every caller already knows the mode statically, so nothing had to be threaded:
    //   flush_ready              carries `Fused` as its 3rd template parameter already, and is the
    //                            path BOTH modes take -> passes Fused
    //   wb_serve_natural         split-only: run_loop reaches pipeline_pass under
    //   wb_retire_prepare        constexpr IoPipe = !Fused && Pipeline == 1  -> pass false
    //   genthread_*              fused-only: run_loop reaches run_fused_iofused_loop
    // With Coded=false every coded block below is deleted by `if constexpr`, so a 2s instantiation
    // is the pre-reply-code function, not a variant of it.
    template <bool TrackOutput, bool TlsNoBorrow, bool kEp, bool Submit, bool ClassifySend,
              bool Coded>
    bool serve_impl(Client& c, bool* submit_allowed = nullptr) {
        TOMO_FORENSIC(c.n_serves.fetch_add(1, std::memory_order_relaxed));
        stats_.serves++;
        Client& conn = c;
        if constexpr (TrackOutput) conn.start_obuf_tracking();
        else conn.stop_obuf_tracking();
        draining_ = &c;
        const uint32_t retired = c.rob().drain([&](Op& op) {
            if constexpr (TlsNoBorrow) {
                if (op.no_borrow()) note_zc_suppressed_tls();
            }
            // Cross-shard completion publishes descriptors/state, not bytes.  The connection's IO
            // owner turns those into the final ordered reply here, before the generic staging path
            // inspects the (possibly repurposed) zero-copy fields.
            // Plain commands have no sidecar and take exactly the pre-notify zc_ptr branch. Special
            // command state, borrowed values, and armed notification batches all already use this
            // field, so their retirement hook nests behind that existing test.
            if (op.zc_ptr) retire_fn_(retire_ctx_, conn, op);
            if (op.zc_ptr) {
                // Anything already staged is older than this op. Once sealed, every subsequent
                // reply uses segments until the queue drains, so no fill-buffer append can jump a
                // borrowed value that is only partially written.
                conn.seal_fill_segment();
                if constexpr (Coded) if (op.reply_code_) op_materialise_code(op);
                conn.append_buf_segment(op.direct, op.direct_len,
                                        op.reply.data(), op.reply.size());
                if constexpr (TlsNoBorrow) {
                    conn.append_buf_segment(op.zc_ptr, op.zc_len);
                    release(op.zc_shard, op.zc_ptr);
                    note_zc_suppressed_tls();
                } else {
        if constexpr (TrackOutput) {
            if (limit_fn_(limit_ctx_, c)) {
                if (submit_allowed) *submit_allowed = false;
                stats_.retired += retired;
                if (!retired) stats_.serves_empty++;
                return true;
            }
        }
        if constexpr (Submit)
            if (!conn.nothing_to_write()) did |= pump<kEp, ClassifySend>(c);
        stats_.retired += retired;
        // A serve that retires nothing: the POLLING paths (flush_ready, the backstop) finding
        // nothing, which is expected and cheap.
        if (!retired) stats_.serves_empty++;
        return did;
    }

    template <bool TrackOutput, bool kEp, bool Submit, bool ClassifySend, bool Coded>
    bool serve_tls_impl(Client& c, TlsConn& tls, bool* submit_allowed = nullptr) {
        TOMO_FORENSIC(c.n_serves.fetch_add(1, std::memory_order_relaxed));
        stats_.serves++;
        Client& conn = c;
        if constexpr (TrackOutput) conn.start_obuf_tracking();
        else conn.stop_obuf_tracking();
        draining_ = &c;
        const uint32_t retired = c.rob().drain([&](Op& op) {
            if (op.no_borrow()) note_zc_suppressed_tls();
            if (op.zc_ptr) retire_fn_(retire_ctx_, conn, op);
            if (op.zc_ptr) {
                conn.seal_fill_segment();
                if constexpr (Coded) if (op.reply_code_) op_materialise_code(op);
                conn.append_buf_segment(op.direct, op.direct_len,
                                        op.reply.data(), op.reply.size());
                conn.append_buf_segment(op.zc_ptr, op.zc_len);
                conn.append_static_segment(kCrlf, sizeof(kCrlf));
                release(op.zc_shard, op.zc_ptr);
                note_zc_suppressed_tls();
                if (op.direct_len) stats_.direct++;
                return;
            }
            if (conn.has_pending_segments()) {
                if constexpr (Coded) if (op.reply_code_) op_materialise_code(op);
                conn.append_buf_segment(op.direct, op.direct_len,
                                        op.reply.data(), op.reply.size());
                if (op.direct_len) stats_.direct++;
            } else {
                // Coded reply: render at the fill frontier, one constant-length store by the
                // thread that owns the buffer. The whole block is deleted for Coded=false, and
                // what remains below is the pre-reply-code text, instruction for instruction.
                if constexpr (Coded) {
                    if (op.reply_code_) {

exec
/bin/bash -lc "git show c8e61f646 -- src/base/topology.h src/core/server.h src/core/placement.h | head -180 && rg -n 'client_obuf_check|void IoLoop::close_client|retire_client|latency' src/core/io_loop.h && sed -n '75,125p' src/core/io_loop.h && tail -4 build/netcmdfix/build-pre.log" in /home/user/Projects/cx-final-netcmdfix
 succeeded in 0ms:
commit c8e61f64628655bf99acb91cd986684e5c608fde
Author: henry <henrychen917@protonmail.com>
Date:   Tue Sep 8 07:41:52 2026 +0800

    merge: knob-collapse + test-determinism waves, gate 342/342
    
    Two independently developed lanes merged by codex (gpt-6 max) and gated as one tree.
    
    cx-timing: test determinism. Adds --release-build so timing assertions run only on release
    builds while counter movement, exact values and safety bounds stay mandatory everywhere.
    Gated 344/344 with zero skips before the merge.
    
    cx-knobs: configuration-surface reduction, 37 files. Deletes leftover A/B latches whose winning
    side was already the default, derives what controllers can derive, folds eight load-balancer
    knobs into one --lb, and moves study switches to an x- namespace. sizeof(Config) 624 -> 528.
    
    Conflict resolutions, all recorded in MERGE.md:
    - tests/atomic_torn.py carried BOTH the base's four-attempt re-roll (which does not degrade to a
      skip) and cx-timing's mechanism/timing split.
    - The cross-shard dispatch-scaling row is RETIRED: deleting --shard-home removed the ownership
      instrument it depended on, so cx-timing's rewrite of it could not stand on a different geometry.
      Both xshard_dispatch_scale files are removed rather than left orphaned behind a retired row.
    - xscript's off-control arm retired; its three-arm loop becomes two.
    
    Gate row accounting, verified against the merged tree and confirmed empirically by the ledger:
    327 quick / 344 full, minus the two retirements, both emitting before the quick-tier exit,
    = EXPECT_QUICK 325 / EXPECT_FULL 342. Full gate: 342 ok, 0 FAIL.
    
    One row (atomic owner-local hazard) reddened on the first merged gate with 'window did not open'.
    Not reproduced in 8 standalone runs at gate geometry nor 4 runs in the gate's own boot sequence;
    the re-gate was clean. Treated as a one-off of the arming class already fixed twice this week.
    
    Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
    Claude-Session: https://claude.ai/code/session_0131nXyQ5SvYWSgqqkHefHVb

diff --git a/src/base/topology.h b/src/base/topology.h
index cb3208a11..3b203fa80 100644
--- a/src/base/topology.h
+++ b/src/base/topology.h
@@ -40,10 +40,8 @@ public:
     // Discovers L3 domains for the CPUs this process is ALLOWED to run on. Respecting the affinity
     // mask matters: under taskset the machine's full topology is not what we get, and assuming
     // otherwise is how threads end up "placed" onto cpus they can never run on.
-    // OPERATOR-DECLARED TOPOLOGY (--l3-domains). "0-7,8-15" builds two declared domains and
-    // bypasses discovery, so deliberate off-hardware shapes can still build
-    // shapes discovery would never produce. The declaration is intersected with the affinity mask:
-    // a cpu the process cannot run on is a config error worth failing loudly, not silently pinning.
+    // TOMOKV_L3_DOMAINS overrides broken sysfs: comma-separated domains, '+' joins CPU ranges.
+    // Every declared CPU must belong to the allowed affinity mask.
     bool declare(const char* spec) {
         cpu_set_t allowed;
         CPU_ZERO(&allowed);
@@ -51,6 +49,7 @@ public:
         domain_of_.assign(CPU_SETSIZE, kNoDomain);
         domains_.clear();
 
+        if (!spec || !*spec) return false;
         const char* p = spec;
         while (*p) {
             std::vector<int> cpus;
@@ -60,8 +59,11 @@ public:
                 if (end == p) return false;
                 long b = a;
                 if (*end == '-') { p = end + 1; b = std::strtol(p, &end, 10); if (end == p) return false; }
+                if (a < 0 || b < a || b >= CPU_SETSIZE ||
+                    (*end && *end != '+' && *end != ',')) return false;
                 for (long c = a; c <= b; c++) {
-                    if (c < 0 || c >= CPU_SETSIZE) return false;
+                    if (domain_of_[c] != kNoDomain ||
+                        std::find(cpus.begin(), cpus.end(), c) != cpus.end()) return false;
                     if (!CPU_ISSET(c, &allowed)) {
                         std::fprintf(stderr, "topology: declared cpu %ld is outside the affinity mask\n", c);
                         return false;
@@ -69,14 +71,21 @@ public:
                     cpus.push_back(static_cast<int>(c));
                 }
                 p = end;
-                if (*p == '+') p++;                      // "0-3+8-11" glues ranges into one node
+                if (*p == '+') {
+                    p++;
+                    if (!*p || *p == ',') return false;
+                }
             }
-            if (!cpus.empty()) {
+            if (cpus.empty()) return false;
+            {
                 const uint32_t id = static_cast<uint32_t>(domains_.size());
                 for (int c : cpus) domain_of_[c] = id;
                 domains_.push_back(std::move(cpus));
             }
-            if (*p == ',') p++;
+            if (*p == ',') {
+                p++;
+                if (!*p || *p == ',') return false;
+            }
         }
         return !domains_.empty();
     }
@@ -106,7 +115,7 @@ public:
     // Read Linux's authoritative SMT relationship for every CPU in the selected topology. Keep
     // the full sysfs list (rather than deriving an offset or intersecting it with affinity): the
     // placement validator must be able to distinguish a complete pair from a taskset which made
-    // only one sibling available. This is boot-only and is called only when smt-mode is enabled.
+    // only one sibling available. This is boot-only and is always read before deriving SMT placement.
     bool discover_thread_siblings() {
         thread_siblings_.assign(CPU_SETSIZE, {});
         for (const std::vector<int>& domain : domains_) {
@@ -119,6 +128,19 @@ public:
         return true;
     }
 
+    uint32_t allowed_sibling_pairs() const {
+        uint32_t pairs = 0;
+        for (const auto& domain : domains_) {
+            for (int cpu : domain) {
+                const auto& siblings = thread_siblings(cpu);
+                if (siblings.size() != 2) continue;
+                const int peer = siblings[0] == cpu ? siblings[1] : siblings[0];
+                if (cpu < peer && domain_of(peer) != kNoDomain) pairs++;
+            }
+        }
+        return pairs;
+    }
+
     uint32_t ndomains() const { return static_cast<uint32_t>(domains_.size()); }
     uint32_t domain_of(int cpu) const {
         return (cpu >= 0 && cpu < static_cast<int>(domain_of_.size())) ? domain_of_[cpu] : kNoDomain;
@@ -198,7 +220,7 @@ private:
     }
 
     // Sysfs topology parsing intentionally does not intersect the process affinity mask. A
-    // sibling outside that mask is still the sibling, and smt-mode must reject the incomplete unit.
+    // sibling outside that mask is still the sibling, and pair placement must reject an incomplete unit.
     static void parse_cpu_list(const char* s, std::vector<int>& out) {
         while (*s) {
             while (*s == ',' || *s == ' ' || *s == '\n') s++;
diff --git a/src/core/placement.h b/src/core/placement.h
index a89dee6d5..2ac89cb73 100644
--- a/src/core/placement.h
+++ b/src/core/placement.h
@@ -1,7 +1,7 @@
 // placement.h — per-thread role/cpu placement and the shard migration contract.
 //
-// A LOGICAL THREAD IS THE DEFAULT LOCALITY UNIT. With smt-mode=1, Linux's reported sibling pair is
-// the scheduling unit instead: both dense thread ids always have one role and FLIP moves them
+// A LOGICAL THREAD IS THE DEFAULT LOCALITY UNIT. When the allowed CPU set contains sibling pairs,
+// Linux's reported pair is the scheduling unit: both dense thread ids have one role and FLIP moves them
 // together. There is deliberately no node layer between a thread and its cpu/L3 facts.
 
 #pragma once
@@ -172,56 +172,12 @@ public:
         return true;
     }
 
-    // Shard ownership is a flat shard->EX tid table. A manual map must be complete because mixing
-    // an accidental omission with defaults would make a supposedly controlled placement measure a
-    // different layout. Runtime dispatch still reads Router's bucket owner exactly once.
-    bool assign_shard_homes(uint32_t nshards, const char* spec) {
-        shard_home_.assign(nshards, kNoThread);
-        if (!spec) {
-            for (uint32_t sid = 0; sid < nshards; sid++)
-                shard_home_[sid] = ex_[sid % ex_.size()];
-            return true;
-        }
-        if (!*spec) {
-            std::fprintf(stderr, "--shard-home must contain shard:thread pairs\n");
-            return false;
-        }
-
-        std::vector<bool> seen(nshards, false);
-        const char* p = spec;
-        while (*p) {
-            uint32_t sid = 0, tid = 0;
-            if (!parse_pair(p, sid, tid)) {
-                std::fprintf(stderr, "--shard-home: expected shard:thread pairs near '%s'\n", p);
-                return false;
-            }
-            if (sid >= nshards) {
-                std::fprintf(stderr, "--shard-home: shard %u is outside 0..%u\n", sid, nshards - 1);
-                return false;
72:    // latency problem (uniform ~3.5 ms at p1) and was actually a distribution problem.
133:            return static_cast<IoLoop*>(ctx)->client_obuf_check(&client, true);
5507:    // At depth, completed IFID work already provides the latency-hiding window. Retain the plain
5819:        // the latency -- which is the correct place for overload to live; throughput stays at peak.
5998:    bool client_obuf_check(Client* c, bool async) {
6065:            } else if (client_obuf_check(c, false)) {
6101:        slowlog_arm_.latency_ms = snapshot.latency_monitor_threshold;
    // which spreads them across threads without any userspace handoff. Note this is safe WITHIN one
    // process; two SERVER PROCESSES sharing a port is the failure mode that once faked data loss,
    // so a boot must still verify nothing else holds the port.
    bool init(Server* srv, ThreadCtx* self, const char* addr, uint16_t port,
              int unix_listen_fd = -1, const TlsContext* tls_context = nullptr,
              bool dormant = false) {
        srv_ = srv; self_ = self;
        (void)addr;
        configured_port_ = port;
        tls_context_ = tls_context;
        unix_listen_fd_ = unix_listen_fd;
        epoll_ = srv_->cfg().net_io == NetIoEngine::Epoll;
        targeted_ifid_ = srv_->cfg().thread_mode == ThreadMode::Fused &&
                         srv_->cfg().overlap != 0;
        age_sample_rate_cached_ = srv_->effective_age_sample_rate();
        age_signals_armed_ = age_sample_rate_cached_ != 0;
        client_lb_signal_armed_ = srv_->client_lb_signals_enabled();
        lb_controller_armed_ = srv_->lb_controller_enabled();
        if (!ring_.init(4096)) return false;
        if (epoll_ && !init_epoll()) return false;
        wb_.bind(&ring_, this, [](void* ctx, int32_t shard, const char* ptr) {
            static_cast<IoLoop*>(ctx)->queue_borrow_release(shard, ptr);
        }, this, [](void* ctx, Client& client, Op& op) {
            auto* loop = static_cast<IoLoop*>(ctx);
            const bool was_blocked = client.blocked();
            NotifyBatch* notifications = nullptr;
            if (op.has_scatter_state()) {
                loop->climon_flush_completed(op);
                notifications = notify_take_batch(op);
                xshard_retire(*loop->srv_, *loop->self_, loop->ring_, client, op,
                    loop->scatter_pool_, loop->self_->id(), loop,
                    [](void* release_ctx, int32_t shard, const char* ptr) {
                        static_cast<IoLoop*>(release_ctx)->queue_borrow_release(shard, ptr);
                    }, [](void* release_ctx) {
                        static_cast<IoLoop*>(release_ctx)->wb_.note_zc_suppressed_tls();
                    });
            } else if (op.has_blocking_state()) {
                notifications = notify_take_batch(op);
                blocking_retire(*loop->srv_, client, op, *loop->self_);
            } else if (op.has_multi_state()) {
                notifications = notify_take_batch(op);
                multi_retire_entry(*loop, client, op);
            } else if (__builtin_expect(op.has_notify_state(), false)) {
                notifications = notify_take_batch(op);
            }
            if (__builtin_expect(notifications != nullptr, false))
                notify_retire_batch_entry(*loop, notifications, client.id());
            if (was_blocked && !client.blocked()) {
                // A blocking wait is timeout-exempt, so its parked duration cannot become the
                // idle age the first cron pass sees after retirement. The socket send refresh is
                // too late: cron runs later in this same IO pass, before its CQE can arrive.
g++ -std=c++20 -O2 -g -Wall -Wextra -march=native -pthread -DTOMO_JEMALLOC -I. -c src/cmd/t_hash.cc -o build/src/cmd/t_hash.o
g++ -std=c++20 -O2 -g -Wall -Wextra -march=native -pthread -DTOMO_JEMALLOC -I. -c src/cmd/t_hash_ttl.cc -o build/src/cmd/t_hash_ttl.o
g++ -std=c++20 -O2 -g -Wall -Wextra -march=native -pthread -DTOMO_JEMALLOC -I. -c src/cmd/t_list.cc -o build/src/cmd/t_list.o
g++ -std=c++20 -O2 -g -Wall -Wextra -march=native -pthread -DTOMO_JEMALLOC -I. -c src/cmd/t_set.cc -o build/src/cmd/t_set.o

exec
/bin/bash -lc "sed -n '5988,6050p' src/core/io_loop.h && sed -n '6110,6205p' src/core/io_loop.h && rg -n 'limit|drain|close|cancel' src/core/pipeline.h src/core/overlap.h src/core/io_loop.h | tail -32 && rg -n -C 5 'stop_obuf_tracking|sub_obuf|commit_write' src/net/conn.h" in /home/user/Projects/cx-final-netcmdfix
 succeeded in 0ms:
            op.state.store(OpState::Done, std::memory_order_release);
            srv_->blocking_client_unparked();
            deferred_timers_[i] = deferred_timers_.back();
            deferred_timers_.pop_back();
            cancelled = true;
        }
        if (cancelled) client->set_blocked(false);
        return cancelled;
    }

    bool client_obuf_check(Client* c, bool async) {
        if (c->closing() || c->dead()) return false;
        if (!srv_->client_obuf_armed()) {
            c->stop_obuf_tracking();
            return false;
        }
        c->start_obuf_tracking();
        const ClientLimitsConfigSnapshot limits = srv_->client_limits_snapshot(self_->id());
        const ClientBufferLimit& limit = c->subscriber_mode() ? limits.pubsub : limits.normal;
        if (!limit.hard_bytes && !limit.soft_bytes) {
            c->set_obuf_soft_since_s(0);
            return false;
        }
        uint64_t used = c->obuf_bytes() + wb_.deferred_output_bytes(*c);
        const auto pending = pubsub_pending_.find(c->id());
        if (pending != pubsub_pending_.end()) used += pending->second.deferred.size();
        bool over = limit.hard_bytes && used >= limit.hard_bytes;
        if (!over && limit.soft_bytes && used >= limit.soft_bytes) {
            if (!c->obuf_soft_since_s()) c->set_obuf_soft_since_s(cached_now_s_);
            else if (cached_now_s_ - c->obuf_soft_since_s() > limit.soft_seconds) over = true;
        } else if (!over) {
            c->set_obuf_soft_since_s(0);
        }
        if (!over) return false;

        srv_->note_client_output_buffer_limit_disconnect();
        if (async)
            std::fprintf(stderr,
                "Client id=%llu scheduled to be closed ASAP for overcoming of output buffer limits.\n",
                static_cast<unsigned long long>(c->id()));
        else
            std::fprintf(stderr,
                "Client id=%llu closed for overcoming of output buffer limits.\n",
                static_cast<unsigned long long>(c->id()));
        close_client(c);
        // Unified W1 has already retired/staged this client but must not let W2 submit the bytes
        // which crossed the hard limit. Its batch already has a nullable-client guard, so encode
        // this rare refusal there and keep the ordinary prepare path free of a parallel bool.
        // close_client runs first while the active context still fences the Client lifetime.
        if (active_wb_context_)
            for (uint32_t i = 0; i < active_wb_context_->count; i++)
                if (active_wb_context_->clients[i] == c) {
                    active_wb_context_->clients[i] = nullptr;
                    break;
                }
        return true;
    }

    uint32_t client_cron_pass() {
        auto& clients = self_->clients();
        const size_t initial = clients.size();
        if (!initial) { client_cron_cursor_ = 0; return 0; }
        size_t visits = initial / kClientCronBeatsPerSecond;
    }

    void close_client(Client* c, bool drain_tls_output = false) {
        // IDEMPOTENT, and that is load-bearing: an abrupt disconnect can close a conn twice --
        // once when the recv fails and again when the in-flight reply's send CQE comes back
        // failed. The second call found the client already parked on the deferred-free list and
        // parked it AGAIN: a double delete, one reap later. Caught by the torture battery's RST
        // churn under ASAN; latent since the first teardown path was written.
        if (c->dead()) return;
        if (!c->closing()) {
            c->mark_closing();
            if (deferred_timer_cancel(c)) enqueue_serve(c);
            if (c->blocked() && blocking_cancel_client(*srv_, *self_, ring_, *c))
                enqueue_serve(c);
            TlsConn* slot_tls = tls_slot_conn(c);
            if (slot_tls && slot_tls->ktls() && !c->send_inflight() &&
                !slot_tls->shutdown_started()) {
                // recv(EIO) is how kTLS reports peer close_notify. The first SSL_shutdown call
                // emits our close_notify through TLS_TX without entering the application path.
                (void)slot_tls->shutdown();
            }
            TlsConn* tls = tls_engine(c);
            if (tls && tls->memory_userspace() && !tls->shutdown_started()) {
                (void)tls->shutdown();
                (void)wb_.pump_tls_any(*c, *tls);
            }
            // Under epoll recv_armed_ records an owed EDGE, not a kernel-held buffer pointer, and
            // a torn-down socket owes nothing. Leaving it set would make safe_to_release() refuse
            // forever and leak the whole Client -- the same ~137KB-per-disconnect shape the retry
            // note below describes, reached by a different route.
            if (epoll_) { c->set_recv_armed(false); c->set_send_inflight(false); }
            (void)wb_.take_send_failure();
            // Break any in-flight recv/send NOW: safe_to_release refuses to free while the kernel
            // holds a buffer pointer (recv_armed / send_inflight), and those only clear when their
            // CQEs come back -- which a half-open peer might never trigger on its own.
            const bool pending_tls_output = tls && (tls->output_pending() || c->send_inflight());
            if (!(drain_tls_output && pending_tls_output)) ::shutdown(c->fd(), SHUT_RDWR);
        }
        multi_close_entry(*this, *c);
        if (TlsConn* tls = tls_engine(c)) {
            if (drain_tls_output && (tls->output_pending() || c->send_inflight())) {
                mark_active(c);
                return;
            }
        }
        // Release only at the quiescence fence: a worker may still hold a Task that resolves through
        // this ROB. Anything else is a use-after-free under pipelining. (The retryable wait paths,
        // with their mark_active leak guard, are below.)
        if (!pubsub_disconnect_ready(c)) { mark_active(c); return; }
        c->set_in_active(false);
        active_.erase(c);
        (void)self_->remove_client(c);
        // THE RETRY IS THE LOAD-BEARING PART of this wait: the client left the active set when it
        // went quiet, so nothing revisits it unless mark_active puts it back -- returning without
        // doing so leaked the entire client (~137KB) per disconnect once. Only a quiesced,
        // claim-free conn may release its slot and die.
        if (!c->safe_to_release() || !client_executor_quiesced(c) ||
            !c->safe_to_release()) { mark_active(c); return; }
        client_work_fences_.erase(c);
        climon_untrack_client(c);
        command_client_disconnected(c);
        self_->release_wb_slot(c->wb_slot());
        c->set_wb_slot(Client::kNoWbSlot);
        wb_.teardown(*c);
        ::close(c->fd());
        srv_->client_released();
        srv_->lb_forget_client(c->id());
        // NOT delete. An ex-side claimed post may still sit un-consumed in our inbound channels
        // naming this client. Every such entry was pushed BEFORE this point, and channels are FIFO
        // with their mask bits set -- so ONE full
        // collect_retire_work pass consumes all of them. Park the corpse for one loop iteration and
        // free it at the top of the one after; the drain lambda skips dead clients.
        c->mark_dead();
        dead_next_.push_back(c);
    }

    // Called only after the connection is ROB-quiescent and no longer dispatching. The
    // acquire on Done precedes this snapshot, so every executor that could still hold
    // this Client published its odd scope before we sample. Wait for EACH such scope
    // to end, without requiring all executors to be idle at the same instant.
    bool client_executor_quiesced(Client* client) const {
        try {
            auto [it, inserted] = client_work_fences_.try_emplace(client);
            ClientWorkFence& fence = it->second;
            const uint64_t dispatch = client->rob().dispatch_id();
            if (inserted || fence.dispatch != dispatch) {
                fence.dispatch = dispatch;
                for (uint32_t tid = 0; tid < srv_->nthreads(); tid++)
                    fence.epochs[tid] = srv_->client_work_epoch(tid);
            }
            bool ready = true;
            for (uint32_t tid = 0; tid < srv_->nthreads(); tid++) {
                // This IO's synchronous fused stack cannot run concurrently with teardown;
                // its existing pipeline/serve fences cover retained local handles.
                if (tid == self_->id() || !(fence.epochs[tid] & 1)) continue;
                if (srv_->client_work_epoch(tid) == fence.epochs[tid]) ready = false;
rg: src/core/pipeline.h: No such file or directory (os error 2)
rg: src/core/overlap.h: No such file or directory (os error 2)
src/core/io_loop.h:6030:                "Client id=%llu closed for overcoming of output buffer limits.\n",
src/core/io_loop.h:6032:        close_client(c);
src/core/io_loop.h:6034:        // which crossed the hard limit. Its batch already has a nullable-client guard, so encode
src/core/io_loop.h:6036:        // close_client runs first while the active context still fences the Client lifetime.
src/core/io_loop.h:6060:            bool closed = false;
src/core/io_loop.h:6063:                close_client(c);
src/core/io_loop.h:6064:                closed = true;
src/core/io_loop.h:6066:                closed = true;
src/core/io_loop.h:6069:            // close_client removes by swap-with-back. Revisit this index so the swapped-in client
src/core/io_loop.h:6071:            if (!closed || clients.size() == before) client_cron_cursor_++;
src/core/io_loop.h:6112:    void close_client(Client* c, bool drain_tls_output = false) {
src/core/io_loop.h:6113:        // IDEMPOTENT, and that is load-bearing: an abrupt disconnect can close a conn twice --
src/core/io_loop.h:6121:            if (deferred_timer_cancel(c)) enqueue_serve(c);
src/core/io_loop.h:6122:            if (c->blocked() && blocking_cancel_client(*srv_, *self_, ring_, *c))
src/core/io_loop.h:6127:                // recv(EIO) is how kTLS reports peer close_notify. The first SSL_shutdown call
src/core/io_loop.h:6128:                // emits our close_notify through TLS_TX without entering the application path.
src/core/io_loop.h:6146:            if (!(drain_tls_output && pending_tls_output)) ::shutdown(c->fd(), SHUT_RDWR);
src/core/io_loop.h:6148:        multi_close_entry(*this, *c);
src/core/io_loop.h:6150:            if (drain_tls_output && (tls->output_pending() || c->send_inflight())) {
src/core/io_loop.h:6174:        ::close(c->fd());
src/core/io_loop.h:6181:        // free it at the top of the one after; the drain lambda skips dead clients.
src/core/io_loop.h:6215:    // this pass's drains, so a corpse parked in pass N is freed in pass N+2's prologue -- after the
src/core/io_loop.h:6216:    // whole of pass N+1 (including its channel drains) ran with the corpse still readable.
src/core/io_loop.h:6223:        // returns any BORROW the close path could not (idempotent: release_all empties the queue).
src/core/io_loop.h:6311:    uint64_t   accept_cancel_generation_ = 0;
src/core/io_loop.h:6319:    // (close_client, and adopt_client's failure path). The set itself is never created in uring
src/core/io_loop.h:6323:    std::vector<Client*> epoll_closes_;   // teardowns deferred out of the active-set walk
src/core/io_loop.h:6332:    bool       accept_cancel_submitted_ = false;
src/core/io_loop.h:6333:    bool       tls_accept_cancel_submitted_ = false;
src/core/io_loop.h:6341:    // phase-1 walk below can reach close_client (a TLS handshake failure, a peer that hung up, a
src/core/io_loop.h:6342:    // synchronous send that failed), and close_client's retry paths call mark_active, which
src/core/io_loop.h:6345:    // closed under the epoll engine. An index survives reallocation; an iterator does not.
411-    // belong to the op at the head of this very drain (see the direct-reply note in io_loop).
412-    char* reserve_fill(size_t len) { return fill_buf().reserve(len); }
413-
414-    bool     has_pending_fill() const { return buf_[fill_].size() > 0; }
415-    uint64_t wsent() const { return wsent_; }
416:    void     commit_write(size_t n) {
417-        wsent_ += n;
418-        if (obuf_tracking_) {
419-            if (n > obuf_bytes_) std::abort();
420-            obuf_bytes_ -= n;
421-        }
--
545-        if (obuf_tracking_) return;
546-        obuf_bytes_ = fill_buf().size() + (send_buf().size() - wsent_) +
547-                      segments_.byte_size();
548-        obuf_tracking_ = true;
549-    }
550:    void stop_obuf_tracking() {
551-        if (!obuf_tracking_) return;
552-        obuf_tracking_ = false;
553-        obuf_bytes_ = 0;
554-        obuf_soft_since_s_ = 0;
555-    }

exec
/bin/bash -lc "sed -n '440,500p' src/cmd/notify.inc && sed -n '120,235p' src/cmd/notify.inc && rg -n -C 6 'post_pubsub_event|pubsub_enqueue' src/core/server.h src/core/thread.h && rg -n -A 52 'void IoLoop::tracking_broadcast_flush' src/cmd/tracking.cc && git blame -L 6112,6155 src/core/io_loop.h" in /home/user/Projects/cx-final-netcmdfix
 succeeded in 0ms:
        (event == NotifyEventId::Del && !shard.notify_source())) {
        notify_record_keyless(shard, cls, event, key);
        return;
    }
    Op* source = shard.notify_source();
    if (source) notify_record(shard, *source, cls, event, key);
}

uint32_t notify_ex_pass_entry(Server& server, Shard& shard, uint32_t producer,
                              ThreadCtx& thread, Ring& ring, LoopSignals& signals) {
    (void)thread;
    auto& slot = shard.notify_state_slot();
    if (!slot || slot->keyless.empty()) return 0;
    uint32_t work = 0;
    while (work < kNotifyKeylessPass && !slot->keyless.empty()) {
        NotifyOut& out = slot->keyless.front();
        // Lane F: an expired or evicted key is an invalidation. Post it to every io owner that
        // holds tracking connections; each filters against its own table. The executor never
        // touches a Client here -- only the same io-to-io transport pub/sub already uses.
        if (__builtin_expect((out.routes & NOTIFY_TRACKING) != 0, 0)) {
            // Re-sample destinations on every retry: tracking clients and IO roles can move
            // while this record is parked. Repeating a delivered invalidation is conservative;
            // remembering an old destination as complete can miss a newly migrated tracker.
            const uint64_t io_mask = server.climon_tracking_io_mask();
            for (uint32_t io : server.placement().ifid_threads()) {
                const uint64_t bit = uint64_t{1} << (io & 63);
                if (!(io_mask & bit)) continue;
                std::unique_ptr<PubSubEvent> invalidate;
                try {
                    invalidate = std::make_unique<PubSubEvent>();
                    invalidate->kind = PubSubEventKind::TrackingInvalidate;
                    invalidate->target_io = io;
                    invalidate->caller_id = 0;
                    invalidate->route_mask = io_mask;
                    invalidate->items.push_back(PubSubEventItem{0, false, 0, out.key});
                } catch (const std::bad_alloc&) { return work; }
                server.pubsub_event_created();
                if (!server.thread(io).post_pubsub_event_nonblocking(
                        producer, invalidate.get(), ring, signals)) {
                    server.pubsub_event_retired();
                    return work;  // keep this target and the keyless record for the next pass
                }
                invalidate.release();
            }
            out.routes &= ~NOTIFY_TRACKING;
        }
        std::unique_ptr<PubSubNotificationChain> building;
        try {
            building = std::make_unique<PubSubNotificationChain>();
            building->items.reserve(2);
            for (uint8_t route = 0; route < 2; route++) {
                std::string channel, message;
                if (!notify_make_publication(
                        server, out.routes, route, out.event,
                        Slice(out.key.data(), static_cast<uint32_t>(out.key.size())),
                        channel, message)) continue;
                building->items.push_back(
                    PubSubNotificationItem{std::move(channel), std::move(message)});
            }
        } catch (const std::bad_alloc&) {
            server.notify_event_dropped();
        case NotifyEventId::Sortstore:
        case NotifyEventId::Sadd:
        case NotifyEventId::Srem:
        case NotifyEventId::Spop:
        case NotifyEventId::Sinterstore:
        case NotifyEventId::Sunionstore:
        case NotifyEventId::Sdiffstore:
        case NotifyEventId::Hset:
        case NotifyEventId::Hincrby:
        case NotifyEventId::Hincrbyfloat:
        case NotifyEventId::Hdel:
        case NotifyEventId::Zadd:
        case NotifyEventId::Zincr:
        case NotifyEventId::Zrem:
        case NotifyEventId::Zremrangebyrank:
        case NotifyEventId::Zremrangebyscore:
        case NotifyEventId::Zremrangebylex:
        case NotifyEventId::Zpopmin:
        case NotifyEventId::Zpopmax:
        case NotifyEventId::Zrangestore:
        case NotifyEventId::Zunionstore:
        case NotifyEventId::Zinterstore:
        case NotifyEventId::Zdiffstore:
        case NotifyEventId::Geosearchstore:
        case NotifyEventId::Georadiusstore:
        case NotifyEventId::Xadd:
        case NotifyEventId::Xdel:
        case NotifyEventId::Xtrim:
        case NotifyEventId::XgroupCreate:
        case NotifyEventId::XgroupDestroy:
        case NotifyEventId::XgroupCreateconsumer:
        case NotifyEventId::XgroupDelconsumer:
        case NotifyEventId::XgroupSetid:
        case NotifyEventId::Xsetid:
        case NotifyEventId::RenameFrom:
        case NotifyEventId::RenameTo:
        case NotifyEventId::CopyTo:
        case NotifyEventId::Restore: return 4;
        case NotifyEventId::Hexpired: return 3;   // strictly before the del it may trigger
        case NotifyEventId::Expire:
        case NotifyEventId::Persist:
        case NotifyEventId::Hexpire:
        case NotifyEventId::Hpersist: return 6;
        case NotifyEventId::Del: return 8;
        case NotifyEventId::Expired:
        case NotifyEventId::Evicted:
        case NotifyEventId::Keymiss: return 4;
    }
    return 4;
}

std::atomic<NotifyBatch*>* notify_special_slot(Op& carrier) {
    if (carrier.has_scatter_state()) return &carrier.scatter_state()->notify;
    if (carrier.has_blocking_state()) return &carrier.blocking_state()->notify;
    if (carrier.has_multi_state())
        return &static_cast<MultiExecState*>(carrier.multi_state())->notify;
    return nullptr;
}

NotifyBatch* notify_get_or_create_batch(Op& carrier);

uint32_t notify_key_arg_order(const Shard& shard, const Op& source, Slice key) {
    for (uint32_t i = 1; i < source.argc(); i++)
        if (source.arg(i) == key) return shard.notify_order_base() + i * 16;
    return shard.notify_order_base() + 0x0ff0;
}

std::string notify_keyspace_channel(Slice key) {
    static constexpr char prefix[] = "__keyspace@0__:";
    std::string channel;
    channel.reserve(sizeof(prefix) - 1 + key.n);
    channel.append(prefix, sizeof(prefix) - 1);
    channel.append(key.p, key.n);
    return channel;
}

std::string notify_keyevent_channel(NotifyEventId event) {
    static constexpr char prefix[] = "__keyevent@0__:";
    std::string channel(prefix, sizeof(prefix) - 1);
    channel += notify_event_name(event);
    return channel;
}

bool notify_make_publication(Server& server, uint32_t routes, uint8_t route,
                             NotifyEventId event_id, Slice key,
                             std::string& channel, std::string& message) {
    if (route == 0) {
        if (!(routes & NOTIFY_KEYSPACE)) return false;
        channel = notify_keyspace_channel(key);
        message = notify_event_name(event_id);
        return true;
    }
    if (!(routes & NOTIFY_KEYEVENT)) return false;
    channel = notify_keyevent_channel(event_id);
    message.assign(key.p, key.n);
    (void)server;
    return true;
}

std::shared_ptr<PubSubNotificationChain> notify_reserve_chain(
        Server& server, std::unique_ptr<PubSubNotificationChain> chain) {
    const uint64_t count = chain ? chain->items.size() : 0;
    if (!count || !server.pubsub_notification_reserve(count, kNotifyInflightCap)) return {};
    PubSubNotificationChain* raw = chain.release();
    // The shared_ptr constructor invokes this deleter even if its control block allocation fails.
    return std::shared_ptr<PubSubNotificationChain>(
        raw, [&server, count](PubSubNotificationChain* owned) {
            delete owned;
            server.pubsub_notification_retire(count);
        });
}

}  // namespace

struct NotifyBatch {
    std::mutex mutex;
src/core/thread.h-570-        return transfer_in_[from].producer_free_slots();
src/core/thread.h-571-    }
src/core/thread.h-572-
src/core/thread.h-573-    // Pub/sub payloads use one cold MPSC inbox per owning IO, but wake through the existing
src/core/thread.h-574-    // client_in mesh.  `true` means the caller owns posting the single nullptr marker for this
src/core/thread.h-575-    // non-empty burst.  The mutex is never touched before the first pub/sub command.
src/core/thread.h:576:    bool post_pubsub_event(PubSubEvent* event) {
src/core/thread.h-577-        std::lock_guard<std::mutex> lock(pubsub_mu_);
src/core/thread.h-578-        pubsub_events_.push_back(event);
src/core/thread.h-579-        if (pubsub_notified_) return false;
src/core/thread.h-580-        pubsub_notified_ = true;
src/core/thread.h-581-        return true;
src/core/thread.h-582-    }
src/core/thread.h-583-
src/core/thread.h-584-    // Keyless expiry/eviction events originate on EX.  Reserve the ordinary producer lane marker
src/core/thread.h-585-    // before making the payload visible, and never spin if that SPSC lane is full.  Holding the
src/core/thread.h-586-    // cold inbox mutex closes the marker-before-payload race with the IO consumer.
src/core/thread.h:587:    bool post_pubsub_event_nonblocking(uint32_t from, PubSubEvent* event,
src/core/thread.h-588-                                       Ring& producer_ring, LoopSignals& sig) {
src/core/thread.h-589-        std::lock_guard<std::mutex> lock(pubsub_mu_);
src/core/thread.h-590-        if (!pubsub_notified_) {
src/core/thread.h-591-            if (!post_client(from, nullptr, producer_ring, sig)) return false;
src/core/thread.h-592-            pubsub_notified_ = true;
src/core/thread.h-593-        }
391:void IoLoop::tracking_broadcast_flush() {
392-    track_filter_clear();
393-    const uint64_t mask = srv_->climon_tracking_io_mask();
394-    if (!mask) return;
395-    PubSubEvent local;
396-    local.kind = PubSubEventKind::TrackingFlush;
397-    local.route_mask = mask;
398-    if ((mask >> (self_->id() & 63)) & 1) tracking_handle_event(local);
399-    for (uint32_t io : srv_->placement().ifid_threads()) {
400-        if (io == self_->id()) continue;
401-        if (!((mask >> (io & 63)) & 1)) continue;
402-        PubSubEvent* event = pubsub_new_event(PubSubEventKind::TrackingFlush);
403-        event->target_io = io;
404-        event->origin_io = self_->id();
405-        event->route_mask = mask;
406-        pubsub_post(io, event);
407-    }
408-}
409-
410-void IoLoop::tracking_handle_event(PubSubEvent& event) {
411-    switch (event.kind) {
412-        case PubSubEventKind::TrackingInvalidate:
413-            for (const PubSubEventItem& item : event.items)
414-                tracking_invalidate_local(
415-                    Slice(item.value.data(), static_cast<uint32_t>(item.value.size())),
416-                    event.caller_id);
417-            tracking_forward_stale(event);
418-            break;
419-        case PubSubEventKind::TrackingFlush: {
420-            for (auto& entry : climon_conn_) {
421-                ClimonConn& state = entry.second;
422-                if (!state.tracking_on) continue;
423-                tracking_deliver_frame(state, entry.first, Slice(), true);
424-            }
425-            for (auto& entry : climon_track_keys_) {
426-                srv_->climon_note_tracking_item_delta(
427-                    -static_cast<int64_t>(entry.second.size()));
428-                srv_->climon_note_tracking_key_delta(-1);
429-            }
430-            climon_track_keys_.clear();
431-            tracking_forward_stale(event);
432-            break;
433-        }
434-        case PubSubEventKind::TrackingDeliver: {
435-            uint32_t live_io = 0;
436-            if (command_client_directory_find(event.client_id, live_io) &&
437-                live_io != self_->id()) {
438-                PubSubEvent* forward = pubsub_new_event(PubSubEventKind::TrackingDeliver);
439-                forward->target_io = live_io;
440-                forward->origin_io = self_->id();
441-                forward->client_id = event.client_id;
442-                forward->subscribe = event.subscribe;
443-                forward->count = event.count;
0f920a52d7 (henry  2026-08-27 00:38:00 +0800 6112)     void close_client(Client* c, bool drain_tls_output = false) {
e45e81b915 (henry  2026-08-24 10:44:29 +0800 6113)         // IDEMPOTENT, and that is load-bearing: an abrupt disconnect can close a conn twice --
e45e81b915 (henry  2026-08-24 10:44:29 +0800 6114)         // once when the recv fails and again when the in-flight reply's send CQE comes back
e45e81b915 (henry  2026-08-24 10:44:29 +0800 6115)         // failed. The second call found the client already parked on the deferred-free list and
e45e81b915 (henry  2026-08-24 10:44:29 +0800 6116)         // parked it AGAIN: a double delete, one reap later. Caught by the torture battery's RST
e45e81b915 (henry  2026-08-24 10:44:29 +0800 6117)         // churn under ASAN; latent since the first teardown path was written.
e45e81b915 (henry  2026-08-24 10:44:29 +0800 6118)         if (c->dead()) return;
24bf4663a3 (henry  2026-08-24 11:37:55 +0800 6119)         if (!c->closing()) {
24bf4663a3 (henry  2026-08-24 11:37:55 +0800 6120)             c->mark_closing();
7eb7d9bb98 (henry  2026-09-04 09:41:10 +0800 6121)             if (deferred_timer_cancel(c)) enqueue_serve(c);
b31856f25d (henry  2026-08-26 07:35:16 +0800 6122)             if (c->blocked() && blocking_cancel_client(*srv_, *self_, ring_, *c))
b31856f25d (henry  2026-08-26 07:35:16 +0800 6123)                 enqueue_serve(c);
e937cc8858 (henry  2026-08-27 07:29:11 +0800 6124)             TlsConn* slot_tls = tls_slot_conn(c);
e937cc8858 (henry  2026-08-27 07:29:11 +0800 6125)             if (slot_tls && slot_tls->ktls() && !c->send_inflight() &&
e937cc8858 (henry  2026-08-27 07:29:11 +0800 6126)                 !slot_tls->shutdown_started()) {
e937cc8858 (henry  2026-08-27 07:29:11 +0800 6127)                 // recv(EIO) is how kTLS reports peer close_notify. The first SSL_shutdown call
e937cc8858 (henry  2026-08-27 07:29:11 +0800 6128)                 // emits our close_notify through TLS_TX without entering the application path.
e937cc8858 (henry  2026-08-27 07:29:11 +0800 6129)                 (void)slot_tls->shutdown();
e937cc8858 (henry  2026-08-27 07:29:11 +0800 6130)             }
e937cc8858 (henry  2026-08-27 07:29:11 +0800 6131)             TlsConn* tls = tls_engine(c);
e937cc8858 (henry  2026-08-27 07:29:11 +0800 6132)             if (tls && tls->memory_userspace() && !tls->shutdown_started()) {
0f920a52d7 (henry  2026-08-27 00:38:00 +0800 6133)                 (void)tls->shutdown();
dad0baefa7 (tomo   2026-08-28 11:16:13 +0800 6134)                 (void)wb_.pump_tls_any(*c, *tls);
0f920a52d7 (henry  2026-08-27 00:38:00 +0800 6135)             }
dad0baefa7 (tomo   2026-08-28 11:16:13 +0800 6136)             // Under epoll recv_armed_ records an owed EDGE, not a kernel-held buffer pointer, and
dad0baefa7 (tomo   2026-08-28 11:16:13 +0800 6137)             // a torn-down socket owes nothing. Leaving it set would make safe_to_release() refuse
dad0baefa7 (tomo   2026-08-28 11:16:13 +0800 6138)             // forever and leak the whole Client -- the same ~137KB-per-disconnect shape the retry
dad0baefa7 (tomo   2026-08-28 11:16:13 +0800 6139)             // note below describes, reached by a different route.
dad0baefa7 (tomo   2026-08-28 11:16:13 +0800 6140)             if (epoll_) { c->set_recv_armed(false); c->set_send_inflight(false); }
dad0baefa7 (tomo   2026-08-28 11:16:13 +0800 6141)             (void)wb_.take_send_failure();
24bf4663a3 (henry  2026-08-24 11:37:55 +0800 6142)             // Break any in-flight recv/send NOW: safe_to_release refuses to free while the kernel
24bf4663a3 (henry  2026-08-24 11:37:55 +0800 6143)             // holds a buffer pointer (recv_armed / send_inflight), and those only clear when their
24bf4663a3 (henry  2026-08-24 11:37:55 +0800 6144)             // CQEs come back -- which a half-open peer might never trigger on its own.
0f920a52d7 (henry  2026-08-27 00:38:00 +0800 6145)             const bool pending_tls_output = tls && (tls->output_pending() || c->send_inflight());
0f920a52d7 (henry  2026-08-27 00:38:00 +0800 6146)             if (!(drain_tls_output && pending_tls_output)) ::shutdown(c->fd(), SHUT_RDWR);
24bf4663a3 (henry  2026-08-24 11:37:55 +0800 6147)         }
20ce65a7e8 (henry  2026-08-26 09:50:45 +0800 6148)         multi_close_entry(*this, *c);
e937cc8858 (henry  2026-08-27 07:29:11 +0800 6149)         if (TlsConn* tls = tls_engine(c)) {
0f920a52d7 (henry  2026-08-27 00:38:00 +0800 6150)             if (drain_tls_output && (tls->output_pending() || c->send_inflight())) {
0f920a52d7 (henry  2026-08-27 00:38:00 +0800 6151)                 mark_active(c);
0f920a52d7 (henry  2026-08-27 00:38:00 +0800 6152)                 return;
0f920a52d7 (henry  2026-08-27 00:38:00 +0800 6153)             }
0f920a52d7 (henry  2026-08-27 00:38:00 +0800 6154)         }
cdcf1a80d3 (tomokv 2026-08-22 20:10:38 +0800 6155)         // Release only at the quiescence fence: a worker may still hold a Task that resolves through

exec
/bin/bash -lc "rg -n -C 7 'client_limits_snapshot|config_views_|client_limits.*publish|publish.*client_limits|ClientLimitsConfig' src/core/server.h src/core/waits* tests/waits* && rg -n 'client_obuf_check|live_obuf_normal_hard_|client_limits' tests/*.cc && tail -4 build/netcmdfix/build-pre.log" in /home/user/Projects/cx-final-netcmdfix
 exited 2 in 0ms:
rg: src/core/waits*: No such file or directory (os error 2)
src/core/server.h-2217-    void set_timeout(uint32_t value) {
src/core/server.h-2218-        const uint64_t version = begin_live_config_update();
src/core/server.h-2219-        live_timeout_.store(value, std::memory_order_relaxed);
src/core/server.h-2220-        refresh_client_cron_armed();
src/core/server.h-2221-        end_live_config_update(version);
src/core/server.h-2222-    }
src/core/server.h-2223-
src/core/server.h:2224:    ClientLimitsConfigSnapshot client_limits_snapshot(uint32_t reader_tid) const {
src/core/server.h-2225-        return live_config_mailboxes_[reader_tid].read(live_config_version_).clients;
src/core/server.h-2226-    }
src/core/server.h-2227-    void set_client_output_buffer_limits(const ClientOutputBufferLimits& limits) {
src/core/server.h-2228-        const uint64_t version = begin_live_config_update();
src/core/server.h-2229-        store_client_output_buffer_limits(limits);
src/core/server.h-2230-        refresh_client_cron_armed();
src/core/server.h-2231-        end_live_config_update(version);

exec
/bin/bash -lc "sed -n '500,530p' src/cmd/notify.inc && sed -n '585,620p' src/core/thread.h && rg -n -C 4 'post_client\\(|drain_clients\\(' src/core/thread.h && git log --oneline -6 -- src/core/waits.h && git diff 821d207b1..HEAD -- src/core/io_loop.h | rg -n -C 7 'client_limits_snapshot|ClientLimitsConfig'" in /home/user/Projects/cx-final-netcmdfix
 succeeded in 0ms:
            server.notify_event_dropped();
            return work;
        }
        std::shared_ptr<PubSubNotificationChain> chain;
        try {
            chain = notify_reserve_chain(server, std::move(building));
        } catch (const std::bad_alloc&) {
            server.notify_event_dropped();
            return work;
        }
        if (!chain) {
            // A tracking-only record has no pub/sub publication to make; its invalidation was
            // already posted above, so retiring it here is completion, not a drop.
            if (out.routes & (NOTIFY_KEYSPACE | NOTIFY_KEYEVENT)) server.notify_event_dropped();
            slot->keyless.pop_front();
            work++;
            continue;
        }
        auto* event = new (std::nothrow) PubSubEvent;
        if (!event) { server.notify_event_dropped(); return work; }
        event->kind = PubSubEventKind::NotificationEnqueue;
        event->notification = std::move(chain);
        const auto& ios = server.placement().ifid_threads();
        const uint32_t coordinator = ios[producer % ios.size()];
        server.pubsub_event_created();
        if (!server.thread(coordinator).post_pubsub_event_nonblocking(
                producer, event, ring, signals)) {
            server.pubsub_event_retired();
            delete event;
            return work;
        }
    // before making the payload visible, and never spin if that SPSC lane is full.  Holding the
    // cold inbox mutex closes the marker-before-payload race with the IO consumer.
    bool post_pubsub_event_nonblocking(uint32_t from, PubSubEvent* event,
                                       Ring& producer_ring, LoopSignals& sig) {
        std::lock_guard<std::mutex> lock(pubsub_mu_);
        if (!pubsub_notified_) {
            if (!post_client(from, nullptr, producer_ring, sig)) return false;
            pubsub_notified_ = true;
        }
        pubsub_events_.push_back(event);
        return true;
    }

    void take_pubsub_events(std::deque<PubSubEvent*>& out) {
        std::lock_guard<std::mutex> lock(pubsub_mu_);
        out.swap(pubsub_events_);
        pubsub_notified_ = false;
    }

    // ---- draining (consumer side) ---------------------------------------------------------------
    // Visits only FLAGGED producers, so cost tracks active producers rather than possible ones. The
    // bits are TAKEN before the channels are drained -- see NotifyMask on why the reverse order
    // loses a push that lands mid-drain.
    template <bool IofusedPrivateQueue = false, typename Fn>
    uint32_t drain_tasks(Fn&& fn) {
        uint32_t n = 0;
        for (uint32_t w = 0; w < NotifyMask::kWords; w++) {
            uint64_t bits = task_notify_.take(w);
            while (bits) {
                const uint32_t b = static_cast<uint32_t>(__builtin_ctzll(bits));
                bits &= bits - 1;
                const uint32_t p = w * 64 + b;
                if (p >= nchan_) continue;
                Task t;
                auto recv = [&] {
                    if constexpr (IofusedPrivateQueue)
549-    }
550-    void flush_task_notify(uint32_t from, Ring& my_ring, LoopSignals& sig) {
551-        if (task_notify_.set(from)) task_in_->wake(my_ring, sig, ring());
552-    }
553:    bool post_client(uint32_t from, Client* c, Ring& my_ring, LoopSignals& sig) {
554-        if (!client_in_[from].push(c, sig)) return false;
555-        if (client_notify_.set(from)) client_in_[from].wake(my_ring, sig, ring());
556-        return true;
557-    }
--
587-    bool post_pubsub_event_nonblocking(uint32_t from, PubSubEvent* event,
588-                                       Ring& producer_ring, LoopSignals& sig) {
589-        std::lock_guard<std::mutex> lock(pubsub_mu_);
590-        if (!pubsub_notified_) {
591:            if (!post_client(from, nullptr, producer_ring, sig)) return false;
592-            pubsub_notified_ = true;
593-        }
594-        pubsub_events_.push_back(event);
595-        return true;
--
762-        return depth;
763-    }
764-
765-    template <typename Fn>
766:    uint32_t drain_clients(Fn&& fn) {
767-        uint32_t n = 0;
768-        for (uint32_t w = 0; w < NotifyMask::kWords; w++) {
769-            uint64_t bits = client_notify_.take(w);
770-            while (bits) {
41-             if constexpr (Fused) {
42-                 if (read_local_enabled && read_local_commit_at_ordinary) {
43-                     // prepare() reserved the selected reads and this operation before any stateful
44-@@ -5993,7 +6002,7 @@ ordinary_dispatch:
45-             return false;
46-         }
47-         c->start_obuf_tracking();
48:-        const ClientLimitsConfigSnapshot limits = srv_->client_limits_snapshot();
49:+        const ClientLimitsConfigSnapshot limits = srv_->client_limits_snapshot(self_->id());
50-         const ClientBufferLimit& limit = c->subscriber_mode() ? limits.pubsub : limits.normal;
51-         if (!limit.hard_bytes && !limit.soft_bytes) {
52-             c->set_obuf_soft_since_s(0);
53-@@ -6079,7 +6088,8 @@ ordinary_dispatch:
54- 
55-     void refresh_notify_config() {
56-         LiveConfigSnapshot snapshot;

exec
/bin/bash -lc "rg -n -C 4 'live_config_mailboxes_|LiveConfigMailbox|boot.*config|seed.*config|init.*live_config' src/core/server.h tests/waits_unit.cc && rg -n 'client_obuf_check|live_obuf_normal_hard_|client_limits' tests/*.cc && tail -4 build/netcmdfix/build-pre.log" in /home/user/Projects/cx-final-netcmdfix
 succeeded in 0ms:
tests/waits_unit.cc-42-}
tests/waits_unit.cc-43-
tests/waits_unit.cc-44-static void stopped_writer_and_reader() {
tests/waits_unit.cc-45-    std::atomic<uint64_t> version{2};
tests/waits_unit.cc:46:    LiveConfigMailbox a, b;
tests/waits_unit.cc-47-    a.init(values(2)); b.init(values(2));
tests/waits_unit.cc-48-    version.store(3);                         // writer stopped before staging anything
tests/waits_unit.cc-49-    require(a.read(version).live.version == 2, "odd version must not obstruct reader");
tests/waits_unit.cc-50-    a.publish(values(2), values(4));           // stopped halfway through the fan-out
--
tests/waits_unit.cc-76-}
tests/waits_unit.cc-77-
tests/waits_unit.cc-78-static void concurrent_config() {
tests/waits_unit.cc-79-    std::atomic<uint64_t> version{2};
tests/waits_unit.cc:80:    LiveConfigMailbox boxes[2];
tests/waits_unit.cc-81-    for (auto& box : boxes) box.init(values(2));
tests/waits_unit.cc-82-    std::thread readers[2];
tests/waits_unit.cc-83-    for (unsigned tid = 0; tid < 2; tid++) readers[tid] = std::thread([&, tid] {
tests/waits_unit.cc-84-        uint64_t seen = 2;
--
src/core/server.h-296-            return false;
src/core/server.h-297-        }
src/core/server.h-298-        threads_.resize(nthreads);
src/core/server.h-299-        // One consumer per physical worker, plus the serialized ownership-transfer coordinator.
src/core/server.h:300:        live_config_mailboxes_.reset(new (std::nothrow) LiveConfigMailbox[nthreads + 1]);
src/core/server.h:301:        if (!live_config_mailboxes_) {
src/core/server.h-302-            std::fprintf(stderr, "fatal: could not allocate live configuration mailboxes\n");
src/core/server.h-303-            return false;
src/core/server.h-304-        }
src/core/server.h-305-        live_config_committed_ = capture_live_config(2);
src/core/server.h-306-        for (uint32_t i = 0; i <= nthreads; i++)
src/core/server.h:307:            live_config_mailboxes_[i].init(live_config_committed_);
src/core/server.h-308-        for (uint32_t i = 0; i < nthreads; i++) {
src/core/server.h-309-            threads_[i] = std::make_unique<ThreadCtx>();
src/core/server.h-310-            // The fingerprint writer is armed only when its one reader, the flip controller, is
src/core/server.h-311-            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
--
src/core/server.h-2160-        // Transfers are serialized by the shape transition, but IO-side tracking/ACL publication
src/core/server.h-2161-        // may continue while executors are quiesced. The coordinator has its OWN mailbox: neither
src/core/server.h-2162-        // the writer's mutable committed copy nor the destination's consumer slot is safe here.
src/core/server.h-2163-        const LiveConfigSnapshot config =
src/core/server.h:2164:            live_config_mailboxes_[nthreads()].read(live_config_version_).live;
src/core/server.h-2165-        shard.configure_maxmemory(config.maxmemory != 0, config.maxmemory / nshards(),
src/core/server.h-2166-                                  config.policy, config.samples);
src/core/server.h-2167-        shard.set_notify_mask(config.notify_events |
src/core/server.h-2168-                              (config.tracking_armed ? NOTIFY_TRACKING : 0u) |
--
src/core/server.h-2221-        end_live_config_update(version);
src/core/server.h-2222-    }
src/core/server.h-2223-
src/core/server.h-2224-    ClientLimitsConfigSnapshot client_limits_snapshot(uint32_t reader_tid) const {
src/core/server.h:2225:        return live_config_mailboxes_[reader_tid].read(live_config_version_).clients;
src/core/server.h-2226-    }
src/core/server.h-2227-    void set_client_output_buffer_limits(const ClientOutputBufferLimits& limits) {
src/core/server.h-2228-        const uint64_t version = begin_live_config_update();
src/core/server.h-2229-        store_client_output_buffer_limits(limits);
--
src/core/server.h-3041-
src/core/server.h-3042-    // A mailbox belongs to a physical worker, across fused/split roles and shard migrations.
src/core/server.h-3043-    // Callers copy the result before their next snapshot call; only the consumer-owned slot is read.
src/core/server.h-3044-    LiveConfigSnapshot live_config_snapshot(uint32_t reader_tid) const {
src/core/server.h:3045:        return live_config_mailboxes_[reader_tid].read(live_config_version_).live;
src/core/server.h-3046-    }
src/core/server.h-3047-    bool live_config_snapshot_if_changed(uint32_t reader_tid, uint64_t known_version,
src/core/server.h-3048-                                         LiveConfigSnapshot& snapshot) const {
src/core/server.h-3049-        if (live_config_version_.load(std::memory_order_acquire) == known_version) return false;
--
src/core/server.h-3319-        }
src/core/server.h-3320-    }
src/core/server.h-3321-    void end_live_config_update(uint64_t write_version) {
src/core/server.h-3322-        const LiveConfigValues next = capture_live_config(write_version + 1);
src/core/server.h:3323:        if (live_config_mailboxes_)
src/core/server.h-3324-            for (uint32_t tid = 0; tid <= nthreads(); tid++)
src/core/server.h:3325:                live_config_mailboxes_[tid].publish(live_config_committed_, next);
src/core/server.h-3326-        live_config_committed_ = next;
src/core/server.h-3327-        live_config_version_.store(write_version + 1, std::memory_order_release);
src/core/server.h-3328-    }
src/core/server.h-3329-
--
src/core/server.h-3597-    struct alignas(64) ClientWorkEpoch { std::atomic<uint64_t> epoch{0}; };
src/core/server.h-3598-    ClientWorkEpoch client_work_[kMaxThreads];
src/core/server.h-3599-    // Fixed storage per physical worker plus one transfer consumer, allocated before workers start.
src/core/server.h-3600-    // CONFIG copies into this storage; arbitrarily many updates or a stopped reader cannot grow it.
src/core/server.h:3601:    std::unique_ptr<LiveConfigMailbox[]> live_config_mailboxes_;
src/core/server.h-3602-    LiveConfigValues live_config_committed_{}; // serialized CONFIG writer only
src/core/server.h-3603-};
src/core/server.h-3604-
src/core/server.h-3605-}  // namespace tomo
tests/netcmd_unit.cc:139:        server.live_obuf_normal_hard_.store(8); server.client_obuf_armed_.store(true);
tests/netcmd_unit.cc:146:        check(loop.client_obuf_check(&held, false), "hard limit fired before Done");
g++ -std=c++20 -O2 -g -Wall -Wextra -march=native -pthread -DTOMO_JEMALLOC -I. -c src/cmd/xshard.cc -o build/src/cmd/xshard.o
g++ -std=c++20 -O2 -g -Wall -Wextra -march=native -pthread -DTOMO_JEMALLOC -I. -c src/cmd/t_zset.cc -o build/src/cmd/t_zset.o
g++ -std=c++20 -O2 -g -Wall -Wextra -march=native -pthread -DTOMO_JEMALLOC -I. -c src/cmd/t_stream_groups.cc -o build/src/cmd/t_stream_groups.o
g++ -std=c++20 -O2 -g -Wall -Wextra -march=native -pthread -DTOMO_JEMALLOC -I. -c src/cmd/server_tail.cc -o build/src/cmd/server_tail.o

exec
/bin/bash -lc "git blame -L 2224,2235 src/core/server.h && git log --oneline -4 -- tests/netcmd_unit.cc && sed -n '1,185p' tests/waits_unit.cc && sed -n '1,75p' src/core/thread.h" in /home/user/Projects/cx-final-netcmdfix
 succeeded in 0ms:
c33228203c (henry 2026-09-08 07:42:05 +0800 2224)     ClientLimitsConfigSnapshot client_limits_snapshot(uint32_t reader_tid) const {
c33228203c (henry 2026-09-08 07:42:05 +0800 2225)         return live_config_mailboxes_[reader_tid].read(live_config_version_).clients;
8f3d0900ec (henry 2026-08-26 12:20:21 +0800 2226)     }
8f3d0900ec (henry 2026-08-26 12:20:21 +0800 2227)     void set_client_output_buffer_limits(const ClientOutputBufferLimits& limits) {
8f3d0900ec (henry 2026-08-26 12:20:21 +0800 2228)         const uint64_t version = begin_live_config_update();
8f3d0900ec (henry 2026-08-26 12:20:21 +0800 2229)         store_client_output_buffer_limits(limits);
8f3d0900ec (henry 2026-08-26 12:20:21 +0800 2230)         refresh_client_cron_armed();
8f3d0900ec (henry 2026-08-26 12:20:21 +0800 2231)         end_live_config_update(version);
8f3d0900ec (henry 2026-08-26 12:20:21 +0800 2232)     }
f8a9a6f10d (henry 2026-08-27 08:34:06 +0800 2233)     // ---- Lane F: the single CLIENT/MONITOR/TRACKING armed word ---------------------------------
f8a9a6f10d (henry 2026-08-27 08:34:06 +0800 2234)     // Every feature this lane adds is OFF by default and must cost nothing while off.  Instead of
f8a9a6f10d (henry 2026-08-27 08:34:06 +0800 2235)     // giving each one its own hot-path test, they share ONE word that the IO loop caches per
821d207b1 Merge cx-fix-netcmd preserving Lua and sender contracts
e94b97100 WIP: codex output, unbuilt and ungated
// Server-less checks for configuration publication and atomic admission. Run via `make unit`.
// alarm makes a reader that waits for our deliberately stopped writer FAIL instead of hanging.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unistd.h>
#include "src/core/live_config.h"
#include "src/core/atomic_admission.h"

using namespace tomo;

static void require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "waits unit: %s\n", message); std::abort(); }
}

static LiveConfigValues values(uint64_t version) {
    LiveConfigValues out;
    out.live.version = out.clients.version = version;
    out.live.maxmemory = version * 17;
    out.live.samples = static_cast<uint32_t>(version + 3);
    out.live.proto_max_bulk_len = ~version;
    out.clients.normal.hard_bytes = version * 31;
    out.clients.normal.soft_bytes = version * 7;
    out.clients.normal.soft_seconds = static_cast<uint32_t>(version);
    out.clients.pubsub.hard_bytes = ~version;
    return out;
}

static void coherent(const LiveConfigValues& got) {
    const auto want = values(got.live.version);
    require(!(got.live.version & 1) && got.live.version >= 2, "committed version");
    require(got.clients.version == want.clients.version &&
            got.live.maxmemory == want.live.maxmemory &&
            got.live.samples == want.live.samples &&
            got.live.proto_max_bulk_len == want.live.proto_max_bulk_len &&
            got.clients.normal.hard_bytes == want.clients.normal.hard_bytes &&
            got.clients.normal.soft_bytes == want.clients.normal.soft_bytes &&
            got.clients.normal.soft_seconds == want.clients.normal.soft_seconds &&
            got.clients.pubsub.hard_bytes == want.clients.pubsub.hard_bytes,
            "configuration fields came from different updates");
}

static void stopped_writer_and_reader() {
    std::atomic<uint64_t> version{2};
    LiveConfigMailbox a, b;
    a.init(values(2)); b.init(values(2));
    version.store(3);                         // writer stopped before staging anything
    require(a.read(version).live.version == 2, "odd version must not obstruct reader");
    a.publish(values(2), values(4));           // stopped halfway through the fan-out
    require(a.read(version).live.version == 2, "uncommitted staged value became visible");
    require(b.read(version).live.version == 2, "unstaged reader lost committed config");
    b.publish(values(2), values(4));
    version.store(4);
    // a already consumed its staged slot. Committing it must require no second publication.
    require(a.read(version).live.version == 4, "commit of an already consumed slot was lost");
    require(b.read(version).live.version == 4, "committed update not delivered");
    const LiveConfigValues& pinned = a.read(version);
    for (uint64_t next = 6; next <= 200; next += 2) {
        version.store(next - 1);
        a.publish(values(next - 2), values(next));
        b.publish(values(next - 2), values(next));
        version.store(next);
        coherent(b.read(version));
        require(pinned.live.version == 4, "writer reused a stopped consumer's slot");
        coherent(pinned);
    }
    require(a.read(version).live.version == 200, "coalesced updates did not reach stopped reader");
    // Skip a committed version, then acquire its successor while the writer is still odd.
    version.store(201);
    a.publish(values(200), values(202));
    version.store(202);
    version.store(203);
    a.publish(values(202), values(204));
    require(a.read(version).live.version == 202, "fallback missed the preceding commit");
}

static void concurrent_config() {
    std::atomic<uint64_t> version{2};
    LiveConfigMailbox boxes[2];
    for (auto& box : boxes) box.init(values(2));
    std::thread readers[2];
    for (unsigned tid = 0; tid < 2; tid++) readers[tid] = std::thread([&, tid] {
        uint64_t seen = 2;
        for (unsigned i = 0; i < 20000; i++) {
            const auto copy = boxes[tid].read(version);
            coherent(copy);
            require(copy.live.version >= seen, "reader went backward across updates");
            seen = copy.live.version;
        }
    });
    for (uint64_t next = 4; next <= 20000; next += 2) {
        version.store(next - 1, std::memory_order_release);
        for (auto& box : boxes) box.publish(values(next - 2), values(next));
        version.store(next, std::memory_order_release);
    }
    for (auto& reader : readers) reader.join();
    for (auto& box : boxes)
        require(box.read(version).live.version == 20000, "final committed value lost");
}

static void admission_accounting() {
    AtomicAdmissionCredits credits;
    using Attempt = AtomicAdmissionCredits::Attempt;
    credits.init(3);
    for (unsigned i = 0; i < 3; i++)
        require(credits.try_admit() == Attempt::Admitted, "initial reservation");
    require(credits.try_admit() == Attempt::Full, "full window must refuse");
    credits.set_window(1);
    require(credits.active() == 3 && credits.debt() == 2 && credits.pool() == 0,
            "shrinking must retain existing groups as debt");
    credits.retire(); credits.retire();
    require(credits.debt() == 0 && !credits.can_admit(), "window admitted at its limit");
    credits.set_window(0);
    require(credits.try_admit() == Attempt::Admitted && credits.debt() == 0,
            "unlimited window retained debt");
    credits.set_window(7);
    require(credits.pool() == 5 && credits.active() == 2, "reconfigure lost active groups");
    credits.retire(); credits.retire();
    require(credits.pool() == 7 && credits.active() == 0, "retirement stranded a credit");
}

static void concurrent_admission() {
    AtomicAdmissionCredits credits;
    credits.init(3);
    std::atomic<unsigned> inside{0};
    std::thread workers[4];
    for (auto& worker : workers) worker = std::thread([&] {
        for (unsigned i = 0; i < 10000; i++) {
            if (credits.try_admit() != AtomicAdmissionCredits::Attempt::Admitted) continue;
            require(inside.fetch_add(1) < 3, "concurrent admission exceeded the limit");
            inside.fetch_sub(1);
            credits.retire();
        }
    });
    for (auto& worker : workers) worker.join();
    require(credits.active() == 0 && credits.pool() == 3, "concurrent retirement leaked credits");
    // This race checks conservation through reconfiguration, not a bound on inherited groups.
    for (auto& worker : workers) worker = std::thread([&] {
        for (unsigned i = 0; i < 10000; i++)
            if (credits.try_admit() == AtomicAdmissionCredits::Attempt::Admitted) credits.retire();
    });
    for (unsigned i = 0; i < 10000; i++) credits.set_window(i % 5);
    for (auto& worker : workers) worker.join();
    credits.set_window(3);
    require(credits.active() == 0 && credits.pool() == 3 && credits.debt() == 0,
            "reconfiguration raced a return");
}

int main() {
    alarm(30);
    stopped_writer_and_reader();
    concurrent_config();
    admission_accounting();
    concurrent_admission();
    std::puts("waits unit: PASS");
}
// thread.h — one server thread, the role it is currently playing, and its inbound channels.
//
// ROLE IS STATE, NOT TYPE. A thread is an IO thread or a worker because of a field, not because of
// its class. That is deliberate: the io:ex split is a runtime decision and the optimal split differs
// by workload (measured: ~16:1 io:ex at p1, scaling as 16:1/depth as pipelining deepens). Encoding
// the role in the type system would make the one thing we most need to change at runtime the one
// thing we cannot.
//
// EVERY THREAD HAS THE SAME FOUR INBOUND TRANSPORTS, whatever its role. What arrives on them
// differs; their units and wake semantics do not. That uniformity is what lets a flip/LB controller
// read all four families through one interface instead of special-casing each:
//
//   task_in_.lane(p) from producer p   IO -> EX   a parsed op to execute
//   client_in_[p]   from producer p    EX -> IO   "you have completed ops to retire"
//   release_in_[p]  from producer p    IO -> EX   a FlatStore value borrow is off the wire
//   transfer_in_[p] from producer p    IO -> IO   a fully quiesced connection ownership handoff
//
// A thread's current role determines which families are active. Task slots are the exception to the
// old uniform per-pair mesh: one fixed consumer-local allocation is block-masked into SPSC lanes at
// boot and ExInstall. The other cold/pointer transports retain their per-producer Channel arrays.
//
// One private sub-ring PER PRODUCER is what keeps dispatch genuinely SPSC. The alternative is one
// MPSC queue per consumer, costing an atomic RMW per push from every producer; the fork measured
// this handoff as instruction volume rather than stalls, so removing the RMW is the direct lever.
//
// Runtime FLIP enforces the conversion preconditions here: Ex -> Io drains every inbox and hands
// off every shard first; Io -> Ex migrates every connection only after rob.quiesced(). Each role
// changes by one direct old-to-new store, never an Idle transit which could repeat the fork's P0.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <vector>
#include "shard.h"
#include "signal.h"
#include "../net/rob.h"     // ReadLocalArmStats: the armed lane's cold arm-on-demand counters
#include "flipctl.h"
#include "pubsub_event.h"
#include "../base/topology.h"

namespace tomo {

class Client;
class Ring;
class WbEngine;
class SnapshotManager;

// Heap-side multi-shard state is deliberately opaque here.  Its vectors, result slots and phase
// bookkeeping live in cmd/xshard so neither Task nor the footprint-locked Op grows.
struct ScatterState;

inline constexpr uint32_t kMaxThreads = 128;
inline constexpr uint32_t kInboxSlots = 1024;
// ReadLocalExState<true>::Impl::lane_admit_cap holds the effective lane capacity in a uint16 that
// fits the struct's existing padding; keep the lane inside that range.
static_assert(kInboxSlots <= UINT16_MAX, "lane_admit_cap is a uint16");

enum class Role : uint8_t { Idle = 0, Ifid = 1, Ex = 2 };

// Active groups on this connection-owning thread: one writer, independent of shard ownership.
// The first/last group arms/disarms atomic_activity_. Keep the former lease's cache-line footprint.
struct alignas(64) AtomicAdmissionState {
    uint32_t active = 0;
};
static_assert(sizeof(AtomicAdmissionState) == 64);

// What travels on task_in_. A handle rather than a raw Op*: the worker resolves it through the
// client's ROB, so a recycled slot cannot be reached through a stale pointer. The client itself
// cannot be torn down while any op is in flight — that is the quiescence fence.
struct Task {

codex
Two fixture issues are emerging. The gate launches these units without pinning them, so their `1:1` and `2:1` ratios conflict with automatic SMT pairing on the full machine. Separately, `cx-waits` changed limit reads to use per-worker mailboxes; the `output` fixture leaves that storage uninitialized. I’m verifying these explanations and checking that corrected tests still reject broken notify, flush, and output behavior.

 succeeded in 66087ms:
exec
/bin/bash -lc "rg -n 'topo_|prepare_boot\\(|live_config_mailboxes_|capture_live_config' tests/*unit.cc tests/store_regression* && sed -n '3250,3330p' src/core/server.h && sed -n '1,150p' src/core/live_config.h" in /home/user/Projects/cx-final-netcmdfix
 succeeded in 0ms:
tests/atomic_survivors_unit.cc:444:    require(server.prepare_boot(cfg) && server.init(cfg), "16-shard, 6:2 owner geometry initialized");
tests/netcmd_unit.cc:64:        check(server.prepare_boot(cfg), "notification topology");
tests/netcmd_unit.cc:98:        check(server.prepare_boot(cfg), "flush topology");
tests/core_concurrency_unit.cc:54:            require(server.topo_.declare(domain.c_str()), "declare fixture topology");
tests/core_concurrency_unit.cc:56:                require(server.placement_.build_fused(server.topo_, nullptr), "fused placement");
tests/core_concurrency_unit.cc:58:                require(server.placement_.build_even(server.topo_, 6, 2), "6:2 placement");

    void store_client_output_buffer_limits(const ClientOutputBufferLimits& limits) {
        live_obuf_normal_hard_.store(limits.normal.hard_bytes, std::memory_order_relaxed);
        live_obuf_normal_soft_.store(limits.normal.soft_bytes, std::memory_order_relaxed);
        live_obuf_normal_seconds_.store(limits.normal.soft_seconds, std::memory_order_relaxed);
        live_obuf_pubsub_hard_.store(limits.pubsub.hard_bytes, std::memory_order_relaxed);
        live_obuf_pubsub_soft_.store(limits.pubsub.soft_bytes, std::memory_order_relaxed);
        live_obuf_pubsub_seconds_.store(limits.pubsub.soft_seconds, std::memory_order_relaxed);
    }

    void refresh_climon_armed() {
        uint32_t armed = 0;
        if (climon_monitors_.load(std::memory_order_relaxed)) armed |= kClimonMonitor;
        if (climon_tracking_.load(std::memory_order_relaxed)) armed |= kClimonTracking;
        if (climon_pause_end_ms_.load(std::memory_order_relaxed)) armed |= kClimonPause;
        if (climon_reply_.load(std::memory_order_relaxed)) armed |= kClimonReply;
        climon_armed_.store(armed, std::memory_order_release);
    }

    void refresh_client_cron_armed() {
        const bool normal = live_obuf_normal_hard_.load(std::memory_order_relaxed) != 0 ||
                            live_obuf_normal_soft_.load(std::memory_order_relaxed) != 0;
        const bool pubsub = live_obuf_pubsub_hard_.load(std::memory_order_relaxed) != 0 ||
                            live_obuf_pubsub_soft_.load(std::memory_order_relaxed) != 0;
        client_obuf_armed_.store(normal || pubsub, std::memory_order_release);
        client_cron_armed_.store(live_timeout_.load(std::memory_order_relaxed) != 0 ||
                                 normal || pubsub, std::memory_order_release);
    }

    // Only boot or the serialized CONFIG writer reads these mutable fields as a group. Readers
    // consume immutable, committed mailbox copies and never collect/revalidate these atomics.
    LiveConfigValues capture_live_config(uint64_t version) const {
        LiveConfigValues values;
        auto& snapshot = values.live;
        snapshot.version = version;
        snapshot.maxmemory = live_maxmemory_.load(std::memory_order_relaxed);
        snapshot.policy = static_cast<MaxmemoryPolicy>(
            live_maxmemory_policy_.load(std::memory_order_relaxed));
        snapshot.samples = live_maxmemory_samples_.load(std::memory_order_relaxed);
        snapshot.notify_events = live_notify_events_.load(std::memory_order_relaxed);
        snapshot.tracking_armed =
            (climon_armed_.load(std::memory_order_relaxed) & kClimonTracking) != 0;
        snapshot.slowlog_log_slower_than = live_slowlog_us_.load(std::memory_order_relaxed);
        snapshot.latency_monitor_threshold = live_latency_ms_.load(std::memory_order_relaxed);
        snapshot.save_armed = live_save_armed_.load(std::memory_order_relaxed);
        snapshot.proto_max_bulk_len = live_proto_max_bulk_len_.load(std::memory_order_relaxed);
        snapshot.debug_fanout_defer_us = debug_atomic_fanout_defer_.load(std::memory_order_relaxed);
        auto& clients = values.clients;
        clients.version = version;
        clients.timeout = live_timeout_.load(std::memory_order_relaxed);
        clients.normal.hard_bytes = live_obuf_normal_hard_.load(std::memory_order_relaxed);
        clients.normal.soft_bytes = live_obuf_normal_soft_.load(std::memory_order_relaxed);
        clients.normal.soft_seconds = live_obuf_normal_seconds_.load(std::memory_order_relaxed);
        clients.pubsub.hard_bytes = live_obuf_pubsub_hard_.load(std::memory_order_relaxed);
        clients.pubsub.soft_bytes = live_obuf_pubsub_soft_.load(std::memory_order_relaxed);
        clients.pubsub.soft_seconds = live_obuf_pubsub_seconds_.load(std::memory_order_relaxed);
        return values;
    }

    uint64_t begin_live_config_update() {
        uint64_t version = live_config_version_.load(std::memory_order_acquire);
        for (;;) {
            if (version & 1) {
                version = live_config_version_.load(std::memory_order_acquire);
                continue;
            }
            if (live_config_version_.compare_exchange_weak(
                    version, version + 1, std::memory_order_acq_rel, std::memory_order_acquire))
                return version + 1;
        }
    }
    void end_live_config_update(uint64_t write_version) {
        const LiveConfigValues next = capture_live_config(write_version + 1);
        if (live_config_mailboxes_)
            for (uint32_t tid = 0; tid <= nthreads(); tid++)
                live_config_mailboxes_[tid].publish(live_config_committed_, next);
        live_config_committed_ = next;
        live_config_version_.store(write_version + 1, std::memory_order_release);
    }

    Config    cfg_;
// live_config.h -- coherent configuration publication without reader retries or reclamation.
#pragma once
#include <atomic>
#include <cstdint>
#include "config.h"

namespace tomo {

struct LiveConfigSnapshot {
    uint64_t version;
    uint64_t maxmemory;
    MaxmemoryPolicy policy;
    uint32_t samples;
    uint32_t notify_events;
    bool tracking_armed;
    int64_t slowlog_log_slower_than;
    uint32_t latency_monitor_threshold;
    bool save_armed;
    uint64_t proto_max_bulk_len;
    uint32_t debug_fanout_defer_us;
};

struct ClientLimitsConfigSnapshot {
    uint64_t version = 0;
    uint32_t timeout = 0;
    ClientBufferLimit normal{};
    ClientBufferLimit pubsub{};
};

struct LiveConfigValues {
    LiveConfigSnapshot live{};
    ClientLimitsConfigSnapshot clients{};
};

// Exactly one consumer, identified by physical ThreadCtx id, and one serialized CONFIG producer.
// They each own one slot; an atomic exchange transfers ownership of the third (middle) slot.
// The producer never touches the consumer's slot, even if that worker stops indefinitely.
// No slot is freed, no reader announces a grace period, and no operation retries an exchange.
class LiveConfigMailbox {
public:
    void init(const LiveConfigValues& initial) {
        for (auto& slot : slots_) slot = {initial, initial};
    }

    void publish(const LiveConfigValues& previous, const LiveConfigValues& next) {
        slots_[back_] = {previous, next};
        back_ = middle_.exchange(back_ | kDirty, std::memory_order_acq_rel) & kIndex;
    }

    const LiveConfigValues& read(const std::atomic<uint64_t>& version) {
        if (middle_.load(std::memory_order_acquire) & kDirty)
            front_ = middle_.exchange(front_, std::memory_order_acq_rel) & kIndex;
        // CONFIG stages every mailbox before publishing the even version. During that staging
        // interval choose the preceding committed value, not the partly published new version.
        // Load AFTER acquiring the slot: its publication also orders CONFIG's odd version store,
        // so this load cannot precede the commit of slot.previous. Keep both values even if this
        // consumer acquires an uncommitted slot and the writer then stops before committing it.
        const uint64_t observed = version.load(std::memory_order_acquire);
        const auto& slot = slots_[front_];
        return observed == slot.next.live.version - 1 ? slot.previous : slot.next;
    }

private:
    static constexpr uint32_t kDirty = 4;
    static constexpr uint32_t kIndex = 3;
    static_assert(std::atomic<uint32_t>::is_always_lock_free);
    struct alignas(64) Publication {
        LiveConfigValues previous;
        LiveConfigValues next;
    };
    Publication slots_[3]{};
    alignas(64) uint32_t front_ = 0;                  // consumer only
    alignas(64) uint32_t back_ = 1;                   // serialized producer only
    alignas(64) std::atomic<uint32_t> middle_{2};
};

} // namespace tomo

exec
/bin/bash -lc "git show 821d207b1:src/core/server.h | sed -n '2230,2265p' && git log --format='%h %s' -5 --ancestry-path c8e61f646..HEAD && sed -n '310,385p' src/core/placement.h && rg -n -A 45 'configure_smt_units' src/core/placement.h && ls -l build/netcmd-unit build/tomokv 2>/dev/null" in /home/user/Projects/cx-final-netcmdfix
 succeeded in 0ms:
        return client_obuf_armed_.load(std::memory_order_relaxed);
    }
    void set_timeout(uint32_t value) {
        const uint64_t version = begin_live_config_update();
        live_timeout_.store(value, std::memory_order_relaxed);
        refresh_client_cron_armed();
        end_live_config_update(version);
    }

    ClientLimitsConfigSnapshot client_limits_snapshot() const {
        for (;;) {
            ClientLimitsConfigSnapshot out;
            out.version = live_config_version_.load(std::memory_order_acquire);
            if (out.version & 1) continue;
            out.timeout = live_timeout_.load(std::memory_order_relaxed);
            out.normal.hard_bytes = live_obuf_normal_hard_.load(std::memory_order_relaxed);
            out.normal.soft_bytes = live_obuf_normal_soft_.load(std::memory_order_relaxed);
            out.normal.soft_seconds = live_obuf_normal_seconds_.load(std::memory_order_relaxed);
            out.pubsub.hard_bytes = live_obuf_pubsub_hard_.load(std::memory_order_relaxed);
            out.pubsub.soft_bytes = live_obuf_pubsub_soft_.load(std::memory_order_relaxed);
            out.pubsub.soft_seconds = live_obuf_pubsub_seconds_.load(std::memory_order_relaxed);
            if (live_config_version_.load(std::memory_order_acquire) == out.version) return out;
        }
    }
    void set_client_output_buffer_limits(const ClientOutputBufferLimits& limits) {
        const uint64_t version = begin_live_config_update();
        store_client_output_buffer_limits(limits);
        refresh_client_cron_armed();
        end_live_config_update(version);
    }
    // ---- Lane F: the single CLIENT/MONITOR/TRACKING armed word ---------------------------------
    // Every feature this lane adds is OFF by default and must cost nothing while off.  Instead of
    // giving each one its own hot-path test, they share ONE word that the IO loop caches per
    // batch and folds into the notification-armed decision parse_and_dispatch already makes.  A
    // zero word means the io side takes byte-for-byte the pre-lane path.
    static constexpr uint32_t kClimonMonitor  = 1u << 0;  // >=1 MONITOR client exists
67f623716 merge cx-resizefix
bf41dd331 resize progress without reader retirement waits
818d458ef Merge cx-waits and add resize and retirement liveness batteries
e25e08f57 EXPECT 377/394 for the eight-branch merge
a6f1950ab Record remaining merges, validation rates, and gate arithmetic
    // these dense vectors change, so runtime role publication does not put a mutex on their paths.
    void set_runtime_role(uint32_t tid, Role role) {
        if (tid >= threads_.size() || (role != Role::Ifid && role != Role::Ex)) std::abort();
        auto erase_tid = [&](std::vector<uint32_t>& ids) {
            ids.erase(std::remove(ids.begin(), ids.end(), tid), ids.end());
        };
        erase_tid(ifid_);
        erase_tid(ex_);
        (role == Role::Ifid ? ifid_ : ex_).push_back(tid);
        std::sort(ifid_.begin(), ifid_.end());
        std::sort(ex_.begin(), ex_.end());
        threads_[tid].role = role;
    }

    Role role_of(uint32_t tid) const { return threads_[tid].role; }
    int cpu_of_thread(uint32_t tid) const { return threads_[tid].cpu; }
    uint32_t domain_of_thread(uint32_t tid) const { return threads_[tid].domain; }
    uint32_t shard_home(uint32_t sid) const { return shard_home_[sid]; }

private:
    bool build_even_smt(const Topology& topo, uint32_t n_ifid, uint32_t n_ex) {
        if ((n_ifid & 1u) || (n_ex & 1u)) {
            std::fprintf(stderr,
                         "--ratio: SMT placement requires even logical io and ex counts\n");
            return false;
        }
        const uint32_t nd = topo.ndomains() ? topo.ndomains() : 1;
        using CpuPair = std::pair<int, int>;
        std::vector<std::vector<CpuPair>> units(nd);
        std::vector<bool> used(CPU_SETSIZE, false);
        for (uint32_t d = 0; d < nd; d++) {
            for (int cpu : topo.cpus_in(d)) {
                if (used[cpu]) continue;
                const std::vector<int>& siblings = topo.thread_siblings(cpu);
                if (siblings.size() != 2 ||
                    std::find(siblings.begin(), siblings.end(), cpu) == siblings.end()) {
                    std::fprintf(stderr,
                                 "SMT placement: cpu %d does not have one sysfs sibling\n", cpu);
                    return false;
                }
                const int peer = siblings[0] == cpu ? siblings[1] : siblings[0];
                const uint32_t peer_domain = topo.domain_of(peer);
                if (peer_domain == kNoDomain) continue; // incomplete affinity unit is unavailable
                if (peer_domain != d) {
                    std::fprintf(stderr,
                                 "SMT placement: sibling cpus %d and %d span L3 domains\n",
                                 cpu, peer);
                    return false;
                }
                const std::vector<int>& reverse = topo.thread_siblings(peer);
                if (reverse.size() != 2 ||
                    std::find(reverse.begin(), reverse.end(), cpu) == reverse.end()) {
                    std::fprintf(stderr,
                                 "SMT placement: cpu %d and cpu %d are not a reciprocal sysfs pair\n",
                                 cpu, peer);
                    return false;
                }
                if (used[peer]) {
                    std::fprintf(stderr, "SMT placement: cpu %d belongs to two sibling units\n", peer);
                    return false;
                }
                used[cpu] = used[peer] = true;
                units[d].push_back(CpuPair{cpu, peer});
            }
        }

        const uint64_t total = static_cast<uint64_t>(n_ifid) + n_ex;
        uint64_t capacity_units = 0;
        for (const auto& domain : units) capacity_units += domain.size();
        if (total > capacity_units * 2) {
            std::fprintf(stderr,
                         "--ratio: %llu threads but only %llu complete SMT pairs are allowed\n",
                         static_cast<unsigned long long>(total),
                         static_cast<unsigned long long>(capacity_units));
            return false;
        }
242:    bool configure_smt_units(const Topology& topo, bool enabled) {
243-        smt_peer_.assign(threads_.size(), kNoThread);
244-        if (!enabled) return true;
245-        std::vector<uint32_t> cpu_to_tid(CPU_SETSIZE, kNoThread);
246-        for (uint32_t tid = 0; tid < threads_.size(); tid++) {
247-            const int cpu = threads_[tid].cpu;
248-            if (cpu < 0 || cpu >= CPU_SETSIZE || cpu_to_tid[cpu] != kNoThread) {
249-                std::fprintf(stderr,
250-                             "SMT placement: cpu %d is not a unique provisioned logical cpu\n", cpu);
251-                return false;
252-            }
253-            cpu_to_tid[cpu] = tid;
254-        }
255-        for (uint32_t tid = 0; tid < threads_.size(); tid++) {
256-            const int cpu = threads_[tid].cpu;
257-            const std::vector<int>& siblings = topo.thread_siblings(cpu);
258-            if (siblings.size() != 2 ||
259-                std::find(siblings.begin(), siblings.end(), cpu) == siblings.end()) {
260-                std::fprintf(stderr,
261-                             "SMT placement: cpu %d does not have one sysfs sibling\n", cpu);
262-                return false;
263-            }
264-            const int peer_cpu = siblings[0] == cpu ? siblings[1] : siblings[0];
265-            const uint32_t peer = peer_cpu >= 0 && peer_cpu < CPU_SETSIZE
266-                ? cpu_to_tid[peer_cpu] : kNoThread;
267-            if (peer == kNoThread) {
268-                std::fprintf(stderr,
269-                             "SMT placement: cpu %d requires sibling cpu %d in the placement\n",
270-                             cpu, peer_cpu);
271-                return false;
272-            }
273-            const std::vector<int>& reverse = topo.thread_siblings(peer_cpu);
274-            if (reverse.size() != 2 ||
275-                std::find(reverse.begin(), reverse.end(), cpu) == reverse.end()) {
276-                std::fprintf(stderr,
277-                             "SMT placement: cpu %d and cpu %d are not a reciprocal sysfs pair\n",
278-                             cpu, peer_cpu);
279-                return false;
280-            }
281-            if (threads_[peer].role != threads_[tid].role) {
282-                std::fprintf(stderr,
283-                             "SMT placement: sibling cpus %d and %d have different roles\n",
284-                             cpu, peer_cpu);
285-                return false;
286-            }
287-            smt_peer_[tid] = peer;
-rwxrwxr-x 1 user user 73210552 Sep  9 19:26 build/netcmd-unit
-rwxrwxr-x 1 user user 80283672 Sep  9 19:26 build/tomokv

exec
/bin/bash -lc "ulimit -c 0
python3 - <<'PY'
from pathlib import Path
import json, subprocess
root = Path('build/netcmdfix')
results = []
for case in ('notify-retry', 'flush', 'output'):
    for run in range(1, 6):
        command = ['taskset', '-c', '44-51', 'timeout', '60', './build/netcmd-unit', case]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        (root / f'pre-{case}-{run}.log').write_text(result.stdout)
        results.append({'case':case, 'run':run, 'returncode':result.returncode, 'command':command})
    passed = sum(r['returncode'] == 0 for r in results if r['case'] == case)
    print(f'PRE {case}: {passed}/5; last output: {result.stdout.strip()}')
(root / 'pre-results.json').write_text(json.dumps(results, indent=2) + '\\n')
PY
 taskset -c 44-55 gdb -q -batch -ex run -ex 'bt 12' --args ./build/netcmd-unit output > build/netcmdfix/pre-output-gdb.log 2>&1
cat build/netcmdfix/pre-output-gdb.log" in /home/user/Projects/cx-final-netcmdfix
 succeeded in 6269ms:
PRE notify-retry: 5/5; last output: ok: netcmd notify-retry
PRE flush: 5/5; last output: ok: netcmd flush
PRE output: 0/5; last output: timeout: the monitored command dumped core

This GDB supports auto-downloading debuginfo from the following URLs:
  <https://debuginfod.ubuntu.com>
Enable debuginfod for this session? (y or [n]) [answered N; input not from terminal]
Debuginfod has been disabled.
To make this setting permanent, add 'set debuginfod enabled off' to .gdbinit.
[Thread debugging using libthread_db enabled]
Using host libthread_db library "/lib/x86_64-linux-gnu/libthread_db.so.1".

Program received signal SIGSEGV, Segmentation fault.
tomo::Server::client_limits_snapshot (reader_tid=0, this=0x7ffffffe3c80) at ./src/cmd/../core/server.h:2225
2225	        return live_config_mailboxes_[reader_tid].read(live_config_version_).clients;
#0  tomo::Server::client_limits_snapshot (reader_tid=0, this=0x7ffffffe3c80) at ./src/cmd/../core/server.h:2225
#1  tomo::IoLoop::client_obuf_check (this=this@entry=0x7ffffffe1bc0, c=c@entry=0x7ffffffe1400, async=async@entry=false) at ./src/cmd/../core/io_loop.h:6005
#2  0x000055555561fc20 in tomo::NetcmdRegression::output () at tests/netcmd_unit.cc:146
#3  0x000055555557892a in main (argc=<optimized out>, argv=0x7fffffffd8f8) at tests/netcmd_unit.cc:254

exec
/bin/bash -lc "rg -n 'prepared|note_pubsub|pubsub_inflight|post_fail' src/core/server.h && git show c8e61f646 -- src/core/server.h | rg -n -C 5 'smt_mode_|discover_thread_siblings' && sed -n '1,80p' tests/_lib.py && rg -n -- '--read-local|--atomic|--ratio|read_local' tests/gate.sh | head -28" in /home/user/Projects/cx-final-netcmdfix
 succeeded in 0ms:
3113:    void pubsub_event_created() { pubsub_inflight_.fetch_add(1, std::memory_order_relaxed); }
3115:        if (pubsub_inflight_.fetch_sub(1, std::memory_order_relaxed) == 0) std::abort();
3144:        uint64_t current = pubsub_inflight_.load(std::memory_order_relaxed);
3146:            if (pubsub_inflight_.compare_exchange_weak(
3153:        const uint64_t previous = pubsub_inflight_.fetch_sub(count, std::memory_order_relaxed);
3182:    uint64_t pubsub_inflight() const { return pubsub_inflight_.load(std::memory_order_relaxed); }
3546:    std::atomic<uint64_t> pubsub_inflight_{0};
51-+            }
52-+        } else if (!topo_.discover()) {
53-+            std::fprintf(stderr, "fatal: could not discover any allowed cpu\n");
54-+            return false;
55-+        }
56:+        if (!topo_.discover_thread_siblings()) {
57-+            std::fprintf(stderr, "fatal: could not read Linux thread_siblings_list topology\n");
58-+            return false;
59-+        }
60:+        smt_mode_ = topo_.allowed_sibling_pairs() != 0;
61-+        bool placed = false;
62-+        if (cfg.thread_mode == ThreadMode::Fused) {
63-+            placed = placement_.build_fused(topo_, cfg.place);
64-+        } else {
65-+            // Default shape: an even io/ex split of every allowed cpu, io taking the odd one out --
--
67-+            uint32_t di = cfg.even_ifid, de = cfg.even_ex;
68-+            if (!cfg.place && !(di | de)) {
69-+                uint32_t n = 0;
70-+                for (uint32_t d = 0; d < topo_.ndomains(); d++)
71-+                    n += static_cast<uint32_t>(topo_.cpus_in(d).size());
72:+                if (smt_mode_) {
73-+                    n = 2 * topo_.allowed_sibling_pairs();
74-+                    de = (n / 4) * 2;
75-+                } else de = n / 2;
76-+                di = n - de;
77-+            }
78-+            placed = cfg.place
79-+                ? placement_.build_explicit(topo_, cfg.place)
80:+                : placement_.build_even(topo_, di, de, smt_mode_ != 0);
81-+        }
82-+        if (!placed) return false;
83:+        if (!placement_.configure_smt_units(topo_, smt_mode_ != 0)) return false;
84-+        if (!placement_.reserve_runtime_roles(placement_.total_threads())) return false;
85-+        if (placement_.ifid_threads().empty() || placement_.ex_threads().empty()) {
86-+            std::fprintf(stderr, "placement needs at least one ifid and one ex thread\n");
87-+            return false;
88-+        }
--
142--            if (!topo_.discover()) {
143--                std::fprintf(stderr, "fatal: could not discover any allowed cpu\n");
144--                return false;
145--            }
146--        }
147:-        if (cfg.smt_mode && !topo_.discover_thread_siblings()) {
148--            std::fprintf(stderr,
149--                         "fatal: --smt-mode could not read Linux thread_siblings_list topology\n");
150--            return false;
151--        }
152--        bool placed = false;
--
244-+    uint32_t lb_sample_rate() const {
245-+        return lb_policy_ ? lb_policy_->sample_rate.load(std::memory_order_relaxed) : 0;
246-+    }
247-+    static constexpr uint32_t lb_tick_ms() { return LbAutotune::kTickMs; }
248-+
249:+    bool smt_units_enabled() const { return smt_mode_; }
250-+
251-     bool flipctl_available() const { return cfg_.thread_mode == ThreadMode::Split; }
252-     bool flipctl_enabled() const { return flipctl_.enabled(); }
253-     uint32_t flipctl_tick_ms() const {
254--        return cfg_.lb_tick_ms ? cfg_.lb_tick_ms : std::max<uint32_t>(1, nthreads());
--
290-         report.live_ex = role_count(Role::Ex, true);
291-         report.target_io = flip_target_io_.load(std::memory_order_acquire);
292-         report.target_ex = flip_target_ex_.load(std::memory_order_acquire);
293--        report.smt_mode = cfg_.smt_mode;
294--        report.unit_threads = cfg_.smt_mode ? 2u : 1u;
295:+        report.smt_mode = smt_mode_;
296:+        report.unit_threads = smt_mode_ ? 2u : 1u;
297-         uint32_t buckets[kMaxThreads] = {};
298-         for (uint32_t sid = 0; sid < nshards(); sid++) {
299-             const uint32_t owner = worker_of_shard(static_cast<int32_t>(sid));
300-@@ -1154,11 +1163,17 @@ public:
301-                 return total > 0 && !owners.empty()
--
415-@@ -1498,7 +1524,7 @@ public:
416-             return refuse("ERR FLIP requires at least one io and one ex thread");
417-         if (static_cast<uint64_t>(target_io) + static_cast<uint64_t>(target_ex) != nthreads())
418-             return refuse("ERR FLIP io + ex must equal the existing total thread count");
419--        if (cfg_.smt_mode && ((target_io & 1u) || (target_ex & 1u)))
420:+        if (smt_mode_ && ((target_io & 1u) || (target_ex & 1u)))
421-             return refuse(flip_smt_pairing_error(target_io));
422-         if (coordinator >= nthreads() || thread(coordinator).role() != Role::Ifid)
423-             return refuse("ERR FLIP coordinator is not a live io thread");
424-@@ -1556,7 +1582,7 @@ public:
425-             // without taking its listener with it.
426-             const uint32_t aof_writer = aof_.writer_tid();
427-             for (uint32_t tid : placement_.ifid_threads()) {
428--                if (!cfg_.smt_mode) {
429:+                if (!smt_mode_) {
430-                     if (tid == coordinator || tid == aof_writer || tid == unix_owner_tid_) continue;
431-                     units[unit_count++] = {tid, UINT32_MAX, lb_thread_occupancy(tid)};
432-                     continue;
433-@@ -1571,7 +1597,7 @@ public:
434-             }
435-         } else {
436-             for (uint32_t tid : placement_.ex_threads()) {
437--                if (!cfg_.smt_mode) {
438:+                if (!smt_mode_) {
439-                     units[unit_count++] = {tid, UINT32_MAX, lb_thread_occupancy(tid)};
440-                     continue;
441-                 }
442-@@ -1698,7 +1724,7 @@ public:
443-             placement_.set_runtime_role(id, role);
444-             thread(id).set_role(role);
445-         };
446--        if (!cfg_.smt_mode) {
447:+        if (!smt_mode_) {
448-             change(tid);
449-         } else {
450-             const uint32_t peer = placement_.smt_peer(tid);
451-@@ -1773,7 +1799,7 @@ private:
452-     }
453- 
454-     bool flip_live_pairs_conserved(bool ready) const {
455--        if (!cfg_.smt_mode) return true;
456:+        if (!smt_mode_) return true;
457-         for (uint32_t tid = 0; tid < nthreads(); tid++) {
458-             const uint32_t peer = placement_.smt_peer(tid);
459-             if (peer >= nthreads()) return false;
460-@@ -1785,7 +1811,7 @@ private:
461-     }
462- 
463-     bool flip_candidate_pairs_conserved() const {
464--        if (!cfg_.smt_mode) return true;
465:+        if (!smt_mode_) return true;
466-         for (uint32_t tid = 0; tid < nthreads(); tid++) {
467-             const uint32_t peer = placement_.smt_peer(tid);
468-             if (peer >= nthreads()) return false;
469-@@ -2440,9 +2466,7 @@ public:
470-             atomic_reconfigure_credits(atomic_window());
--
513-@@ -3475,6 +3500,9 @@ private:
514-     bool lb_occupancy_primed_ = false;
515-     std::unordered_map<uint64_t, LbClientSignal> lb_clients_;
516-     std::atomic<uint64_t> lb_client_owner_weight_[kMaxThreads] = {};
517-+    // Boot-derived cold state, separate from the user configuration.
518:+    bool smt_mode_ = false;
519-+    uint64_t script_stage_bytes_ = 4ull * 1024 * 1024;
520-     std::atomic<uint32_t> loading_{0};
521- 
522-     uint8_t executor_slots_[kMaxThreads] = {};
#!/usr/bin/env python3
"""Shared client, geometry and reporting helpers for tests/*.py.

`import _lib` works from any battery because python3 puts the script's own directory (tests/)
first on sys.path. Nothing here opens a server; every helper takes a live connection.

House rules this module encodes (they are rules, not preferences -- see AUDIT-TESTS.md):

  * DETERMINISTIC hooks over timing luck. `armed()` arms a DEBUG window hook and ALWAYS disarms
    it, and refuses to proceed if the arm did not answer OK (an unarmed "deterministic" arm is a
    timing arm in disguise).
  * A geometry claim is PROVED on this boot. `shard_of()` and `topology()` ask the server; nothing
    is inferred from key names or from the default shard-home map (the hash seed is drawn from the
    kernel at every boot, and the balancer can move shards).
  * "Owner" means the thread that owns a shard -- the `shard <sid> <owner_tid>` rows of DEBUG
    LBSIGNALS -- never the `ex` role label. Under --thread-mode 1s every thread is labelled `fused`;
    ownership still comes from shard rows. Every helper here behaves identically in 1s and 2s.
  * A skip is visible and carries a reason. `Report.skip(..., strict=True)` marks a skip that
    means "the mechanism could not be armed" (a DEBUG hook denied, a knob absent); under
    TOMO_GATE_STRICT=1 -- exported by tests/gate.sh -- such a skip fails the battery, so a gate row
    can never turn green by silently skipping the arm it exists for. A probabilistic discovery arm
    that found no window is `strict=False`: it is reported, never a failure.
  * `skip_all(reason)` exits 3 when a whole battery cannot apply to this boot (a 2s-only feature on
    a 1s boot). The gate's `py()` wrapper paints exit 3 red: a row that cannot run is a gate
    defect, not a pass.
"""

import contextlib
import os
import socket
import sys
import time
from collections import namedtuple

# ------------------------------------------------------------------------------------------------
# RESP client
# ------------------------------------------------------------------------------------------------


class RespError(Exception):
    """A `-ERR ...` reply. Returned by Conn.read(), never raised by it, so a reader thread keeps
    the server's exact text instead of a generic exception."""

    def __init__(self, message):
        if isinstance(message, str):
            message = message.encode()
        super().__init__(message)
        self.message = message

    def __eq__(self, other):
        return isinstance(other, RespError) and self.message == other.message

    def __hash__(self):
        return hash(self.message)

    def __repr__(self):
        return "RespError(%r)" % self.message

    def __str__(self):
        return self.message.decode("utf-8", "replace")


def encode(*args):
    """One RESP command frame. str -> utf-8, bytes as-is, everything else via str()."""
    parts = [b"*%d\r\n" % len(args)]
    for arg in args:
        if isinstance(arg, str):
            arg = arg.encode()
        elif not isinstance(arg, (bytes, bytearray)):
            arg = str(arg).encode()
        parts.append(b"$%d\r\n%s\r\n" % (len(arg), arg))
    return b"".join(parts)


class Conn:
    """A blocking RESP2/RESP3 connection with a complete reader.

    read() returns: simple strings and verbatim/bulk strings as bytes, integers as int, nulls as
    None, arrays/sets/pushes as lists, maps as FLAT [k, v, k, v] lists (order preserved, duplicate
    keys kept), doubles/big numbers/booleans as their raw bytes (`,1.5` -> b"1.5", `#t` -> b"t"),
13:#   read-local lane ARMED (--read-local 1, the recommended read-heavy production posture; that leg
96:# read_local:1), the same 32 feature batteries as the split loop, and one shutdown row (clean
110:# --read-local 1). The canonical row boots split with the read-local lane disarmed, so it cannot
111:# reach that lane at all; the armed run carries its own non-vacuity check on read_local_hits.
317:g++ -std=c++20 -O2 -march=native -pthread -I. tests/read_local_write_ring_unit.cc \
412:./build/tomokv --ratio 4:4:2  2>&1 | grep -q "deleted"  && ok "reject 3-part ratio"|| bad "reject 3-part ratio"
472:boot(){ local bin=$1; shift; launch main "$bin" --ratio $GATE_RATIO "$@"; }
473:boot_fused(){ # deliberately omits --ratio, which fused mode rejects
504:      boot ./build/tomokv --atomic 1 --read-local "$WAIT_LOCAL" --overlap 0 \
507:      boot_fused ./build/tomokv --atomic 1 --read-local "$WAIT_LOCAL" --overlap 0 \
550:boot ./build/tomokv --atomic 1 --enable-debug-command yes || bad "atomic release boot"
569:  boot ./build/tomokv --atomic $AT --enable-debug-command yes \
584:  if boot_fused ./build/tomokv --atomic "$AT" --enable-debug-command yes; then
615:# the WIRE says so: INFO server thread_mode:1s AND read_local:1 -- the effective lane state, not the
620:  if boot_fused ./build/tomokv --atomic "$AT" --read-local 1 --enable-debug-command yes; then
623:    ARMED_RL=$(printf '%s\n' "$ARMED_INFO" | sed -n 's/^read_local://p')
626:        || bad "fused+armed boot line (atomic $AT)" "wire mode=$ARMED_MODE read_local=$ARMED_RL"
638:      | awk -F: '/^read_local_keyspace_hits:|^read_local_mget_local_hits:/{s+=$2} END{print s+0}')
659:    boot ./build/tomokv --atomic $AT \
665:    boot_fused ./build/tomokv --atomic $AT --read-local 1 \
678:boot_fused ./build/tomokv --atomic 1 --read-local 1 \
687:py tests/read_local_lane.py 127.0.0.1 "$PORT" >/tmp/gate-read-local-lane.txt 2>&1 \
715:  boot ./build/tomokv --ratio 6:2 --atomic "$AT" --enable-debug-command yes \
725:  boot ./build/tomokv --atomic $AT --enable-debug-command yes \
769:  boot ./build/tomokv --atomic 1 --enable-debug-command yes \
804:boot ./build/tomokv --atomic 1 --dir "$DUMPRESTORE_DIR" --dbfilename dumprestore.tomo \
811:boot ./build/tomokv --atomic 1 --dir "$DUMPRESTORE_DIR" \
841:boot ./build/tomokv --aclfile "$ACL_ATOMIC_FILE" --atomic 1 || bad "ACL atomic purpose boot"

exec
/bin/bash -lc "sed -n '1,95p' tests/netcmd_config_unit.cc && sed -n '25,88p' tests/core_concurrency_unit.cc && rg -n 'nthreads\\(|live_config_version_|ThreadCtx\\(|init\\(' src/core/server.h src/core/thread.h | head -22 && sed -n '1,42p' .gitignore && rg -n -A 60 'pubsub_post\\(' src/core/pubsub.inc | head -70" in /home/user/Projects/cx-final-netcmdfix
 succeeded in 0ms:
#include "netcmd_unit.h"
#include "src/cmd/server_tail.cc"
#include <filesystem>
#include <thread>
#include <barrier>
#include <sys/stat.h>

namespace {
std::atomic<bool> rewrite_arm{false};
std::atomic<unsigned> rewrite_opens{0};
std::barrier rewrite_opened(2);
ino_t rewrite_inodes[2]{};
void observe_open(int fd) {
    if (!rewrite_arm.load()) return;
    const auto index = rewrite_opens.fetch_add(1);
    check(index < 2, "exactly two rewrite opens");
    struct stat info{}; check(::fstat(fd, &info) == 0, "rewrite inode identified");
    rewrite_inodes[index] = info.st_ino;
    rewrite_opened.arrive_and_wait(); // both files are open before either writer may write/rename
}
}
extern "C" int __real_mkstemp(char*);
extern "C" FILE* __real_fopen(const char*, const char*);
extern "C" int __wrap_mkstemp(char* path) {
    const int fd = __real_mkstemp(path);
    if (fd >= 0 && std::strstr(path, ".rewrite.")) observe_open(fd);
    return fd;
}
extern "C" FILE* __wrap_fopen(const char* path, const char* mode) {
    FILE* file = __real_fopen(path, mode);
    if (file && std::strstr(path, ".rewrite.")) observe_open(::fileno(file));
    return file;
}

void test_config_rewrite() {
    check(tomo::config_quote("77") == "77", "simple scalar keeps its original spelling");
    const std::string value = "pass with spaces\n\r\t\"'\\tail";
    std::vector<std::string> words;
    check(tomo::cfg_split_args(tomo::config_quote(value).c_str(), words), "quoted config parses");
    check(words.size() == 1 && words[0] == value, "config string round trip");
    // The harness binds a server whose config path is initialized below by its friend fixture.
    const char* path = tomo::command_server()->cfg().conf_path;
    std::FILE* original = std::fopen(path, "w"); check(original, "create original config");
    std::fputs("user reader on nopass ~* +get\nload \"/tmp/recovery source.tomo\"\nrequirepass old\n"
               "hash-max-ziplist-entries 3\nhash-max-ziplist-value 9\nlist-max-ziplist-size -1\n"
               "zset-max-ziplist-entries 3\nzset-max-ziplist-value 9\n", original);
    std::fclose(original);
    std::string error;
    check(tomo::config_rewrite(error), "first rewrite");
    std::ifstream in(path); std::string body((std::istreambuf_iterator<char>(in)), {});
    check(body.find("user reader on nopass ~* +get\n") != std::string::npos, "inline ACL survived rewrite");
    check(body.find("load \"/tmp/recovery source.tomo\"\n") != std::string::npos, "recovery source survived rewrite");
    check(body.find("max-ziplist-") == std::string::npos, "stale encoding aliases were replaced");
    for (const char* name : {"hash-max-listpack-entries", "hash-max-listpack-value", "list-max-listpack-size",
                             "zset-max-listpack-entries", "zset-max-listpack-value"})
        check(body.find(std::string(name) + " ") != std::string::npos, "canonical encoding directive emitted");
    std::vector<std::string> loaded;
    check(tomo::load_conf_file(path, loaded), "rewritten config loads");
    auto password = std::find(loaded.begin(), loaded.end(), "--requirepass");
    check(password != loaded.end() && ++password != loaded.end() && *password == value, "password preserved as one token");
    rewrite_arm.store(true);
    bool success[2]{};
    std::thread a([&] { std::string err; success[0] = tomo::config_rewrite(err); });
    std::thread b([&] { std::string err; success[1] = tomo::config_rewrite(err); });
    a.join(); b.join(); rewrite_arm.store(false);
    check(rewrite_opens.load() == 2, "two rewrite opens overlapped before either rename");
    check(rewrite_inodes[0] != rewrite_inodes[1], "concurrent writers own distinct temporary inodes");
    check(success[0] && success[1], "both concurrent rewrites completed");
    std::ifstream after(path); std::string final((std::istreambuf_iterator<char>(after)), {});
    check(final == body, "concurrent rewrites preserve exact complete bytes");
}
            // A negative control can fail while the executor is deliberately parked.
            // Do not run static condition-variable destructors with that waiter alive.
            std::_Exit(1);
        }
    }
    static Slice slice(const std::string& text) {
        return {text.data(), static_cast<uint32_t>(text.size())};
    }

    template <bool Fused = false>
    struct Fixture {
        Server server;
        ExLoopT<Fused> loops[8];
        IoLoop io;
        uint32_t source = Fused ? 0 : 6;
        uint32_t destination = Fused ? 1 : 7;
        uint32_t io_id = Fused ? 7 : 0;
        Fixture() {
            cpu_set_t cpus;
            CPU_ZERO(&cpus);
            require(sched_getaffinity(0, sizeof(cpus), &cpus) == 0, "affinity unavailable");
            std::string domain;
            uint32_t count = 0;
            for (int cpu = 0; cpu < CPU_SETSIZE && count < 8; cpu++) {
                if (!CPU_ISSET(cpu, &cpus)) continue;
                if (count++) domain += '+';
                domain += std::to_string(cpu);
            }
            require(count == 8, "regression fixture requires eight allowed CPUs (no skip)");
            require(server.topo_.declare(domain.c_str()), "declare fixture topology");
            if constexpr (Fused)
                require(server.placement_.build_fused(server.topo_, nullptr), "fused placement");
            else
                require(server.placement_.build_even(server.topo_, 6, 2), "6:2 placement");
            require(server.placement_.reserve_runtime_roles(8), "reserve placement roles");
            Config config;
            config.shards = 16;
            config.thread_mode = Fused ? ThreadMode::Fused : ThreadMode::Split;
            config.flip_auto = 0;
            config.save.clear();
            require(server.init(config), "initialize in-memory fixture");
            require(server.nshards() == 16 && server.nthreads() == 8, "fixture geometry");
            for (uint32_t tid = 0; tid < 8; tid++) {
                ThreadCtx& thread = server.thread(tid);
                require(Fused ? thread.init_task_inbox_local_fused()
                              : thread.init_task_inbox_local(server.placement().ifid_threads(),
                                                             server.placement().ex_threads()),
                        "initialize task lanes");
                auto& loop = loops[tid];
                loop.srv_ = &server;
                loop.self_ = &thread;
                loop.fused_handoff_ring_ = &loop.ring_;
                loop.cached_now_ms_ = 1000;
                loop.lb_controller_armed_ = true;
                server.bind_owner_notify_pending(tid, &loop.notify_keyless_pending_);
                loop.refresh_live_config();
            }
            io.srv_ = &server;
            io.self_ = &server.thread(io_id);
            io.lb_controller_armed_ = true;
        }
        int32_t sid() { return server.thread(source).shards().front()->id(); }
        std::string key(int32_t shard) {
            for (uint32_t i = 0; i < 100000; i++) {
src/core/thread.h:318:    ThreadCtx() = default;
src/core/thread.h:319:    ThreadCtx(const ThreadCtx&) = delete;
src/core/thread.h:325:    void init(uint32_t id, Role r, uint32_t nthreads, uint32_t age_sample_rate,
src/core/server.h:216:    bool init(const Config& cfg, const AofReplayPlan* aof_replay = nullptr) {
src/core/server.h:254:        atomic_credits_.init(std::min<uint32_t>(16u * cfg.shards, 1024u));
src/core/server.h:259:        live_config_version_.store(2, std::memory_order_release);  // even versions are stable
src/core/server.h:274:            shards_[i]->init(this, static_cast<int32_t>(i), b0, b1, cfg.zc_min, cfg.encodings.type_limits(),
src/core/server.h:293:        if (!flipctl_.init(cfg.thread_mode == ThreadMode::Split && cfg.flip_auto != 0,
src/core/server.h:307:            live_config_mailboxes_[i].init(live_config_committed_);
src/core/server.h:313:            threads_[i]->init(i, placement_.role_of(i), nthreads,
src/core/server.h:389:        snapshot_.init(nthreads, cfg.shards,
src/core/server.h:392:        aof_.init(*this, cfg, nthreads, cfg.shards,
src/core/server.h:449:               "fused_threads=" + std::to_string(nthreads()) +
src/core/server.h:460:        if (!client_lb_signals_enabled() || owner >= nthreads()) return;
src/core/server.h:513:        for (uint32_t tid = 0; tid < nthreads(); tid++) {
src/core/server.h:556:    uint32_t         nthreads()   const { return static_cast<uint32_t>(threads_.size()); }
src/core/server.h:576:        return tid < nthreads() && serves_clients(thread(tid).role());
src/core/server.h:579:        return tid < nthreads() && owns_shards(thread(tid).role());
src/core/server.h:583:        for (uint32_t tid = 0; tid < nthreads(); tid++) count += serves_clients(tid);
src/core/server.h:593:            if (owner < nthreads() && !seen[owner]) {
src/core/server.h:672:            if (owner < nthreads()) buckets[owner]++;
src/core/server.h:676:        for (uint32_t tid = 0; tid < nthreads(); tid++) {
build/
*.o
compile_commands.json
servertail-testdata/

# compiled test/bench drivers (sources are tracked, binaries are not)
tools/benchtxn

# stray comparison-run snapshot artifacts (dragonfly writes these into cwd)
dump-*.dfs
dump-*.rdb
dump-*.tomo
dump.tomo

# python bytecode from the in-tree test helpers (tests/aof_frame_order.py imports aof_frames)
__pycache__/

# agent lane artifacts -- never commit these
codex*.log
BRIEF*.md
LANE_RULES.md

# replycode lane: measurement drivers are tracked as sources, their binaries are not
scratchpad/replycode/replay
scratchpad/replycode/layout
scratchpad/replycode/*.so
99:    void pubsub_post(uint32_t target_io, PubSubEvent* event) {
100-        if (target_io == self_->id()) {
101-            pubsub_local_events_.push_back(event);
102-            pubsub_pass_pending_ = true;
103-            return;
104-        }
105-        ThreadCtx& target = srv_->thread(target_io);
106-        if (!target.post_pubsub_event(event)) return;
107-
108-        // One nullptr marker wakes one whole inbox burst. This uses the established IO-to-IO
109-        // client channel, including its notify-mask/park protocol. The producer channel is an IO
110-        // channel, so ordinary executor completion traffic cannot fill this SPSC lane.
111-        while (!target.post_client(self_->id(), nullptr, ring_, self_->sig()))
112-            __builtin_ia32_pause();
113-    }
114-
115-    uint32_t pubsub_home_for(Slice channel) const {
116-        const auto& ios = srv_->placement().ifid_threads();
117-        const uint64_t hash = FlatStore::hash_key(channel);
118-        return ios[hash % ios.size()];
119-    }
120-
121-    uint32_t pubsub_home_for(const std::string& channel) const {
122-        return pubsub_home_for(Slice(channel.data(), channel.size()));
123-    }
124-
125-    static bool pubsub_has_ref(const std::vector<PubSubRef>& refs, const PubSubRef& wanted) {
126-        return std::find(refs.begin(), refs.end(), wanted) != refs.end();
127-    }
128-
129-    bool pubsub_home_add_channel(const std::string& channel, const PubSubRef& ref, bool shard) {
130-        auto& index = shard ? pubsub_shard_channels_ : pubsub_channels_;
131-        auto [it, inserted] = index.try_emplace(channel);
132-        std::vector<PubSubRef>& refs = it->second;
133-        if (pubsub_has_ref(refs, ref)) return false;
134-        if (inserted || refs.empty()) srv_->pubsub_active_channel_added(shard);
135-        refs.push_back(ref);
136-        srv_->pubsub_home_entry_added();
137-        return true;
138-    }
139-
140-    bool pubsub_home_remove_channel(const std::string& channel, const PubSubRef& ref, bool shard) {
141-        auto& index = shard ? pubsub_shard_channels_ : pubsub_channels_;
142-        auto it = index.find(channel);
143-        if (it == index.end()) return false;
144-        std::vector<PubSubRef>& refs = it->second;
145-        auto found = std::find(refs.begin(), refs.end(), ref);
146-        if (found == refs.end()) return false;
147-        *found = refs.back();
148-        refs.pop_back();
149-        srv_->pubsub_home_entry_removed();
150-        if (refs.empty()) {
151-            index.erase(it);
152-            srv_->pubsub_active_channel_removed(shard);
153-        }
154-        return true;
155-    }
156-
157-    bool pubsub_home_add_pattern(const std::string& pattern, const PubSubRef& ref) {
158-        for (const PubSubPatternRef& entry : pubsub_patterns_)
159-            if (entry.ref == ref && entry.pattern == pattern) return false;
--
466:            pubsub_post(io, event);
467-        }
468-
469-        // PASS B -- publish replies, strictly after every delivery above is queued.
470-        for (uint32_t io : dirty) {
471-            PubSubOutbox& box = pubsub_outbox_[io];
472-            if (box.results.empty()) continue;
473-            work++;

apply patch
patch: completed
/home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
new file mode 100644
index 0000000000000000000000000000000000000000..bc72f719b1e93668f4c671000b1d5bad75da7ac0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
@@ -0,0 +1,16 @@
+#define _GNU_SOURCE
+#include <dlfcn.h>
+#include <stdio.h>
+#include <string.h>
+
+/* Read-only topology simulation: the process remains pinned to real CPUs 44-51.
+ * These serverless fixtures never launch workers. Only sibling metadata changes. */
+FILE *fopen(const char *path, const char *mode) {
+    static char pairs[2][7] = {"44-45\n", "46-47\n"};
+    int cpu = -1, end = 0;
+    if (sscanf(path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list%n",
+               &cpu, &end) == 1 && end && !path[end] && cpu >= 44 && cpu <= 47)
+        return fmemopen(pairs[(cpu - 44) / 2], 6, "r");
+    FILE *(*real_fopen)(const char *, const char *) = dlsym(RTLD_NEXT, "fopen");
+    return real_fopen(path, mode);
+}

exec
/bin/bash -lc "taskset -c 44-55 gcc -shared -fPIC build/netcmdfix/smt-sysfs.c -o build/netcmdfix/smt-sysfs.so -ldl && cp build/netcmd-unit build/netcmdfix/netcmd-unit-pre && cp tests/netcmd_unit.cc build/netcmdfix/netcmd_unit-pre.cc
python3 - <<'PY'
from pathlib import Path
import json, subprocess
root = Path('build/netcmdfix').resolve()
results = []
for case in ('notify-retry', 'flush'):
    for run in range(1, 6):
        command = ['taskset', '-c', '44-51', 'timeout', '60', 'env', 'TOMOKV_L3_DOMAINS=44-47', f'LD_PRELOAD={root / \"smt-sysfs.so\"}', './build/netcmd-unit', case]
        r = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        (root / f'pre-smt-{case}-{run}.log').write_text(r.stdout)
        results.append({'case':case, 'run':run, 'returncode':r.returncode, 'command':command})
    print(f'PRE simulated SMT {case}: {sum(r[\"returncode\"] == 0 for r in results if r[\"case\"] == case)}/5; last output: {r.stdout.strip()}')
(root / 'pre-smt-results.json').write_text(json.dumps(results, indent=2) + '\\n')
for case in ('notify-retry', 'flush', 'output'):
    saved = Path(f'/tmp/gate-netcmd-{case}.txt')
    if saved.exists():
        (root / f'reported-gate-{case}.log').write_bytes(saved.read_bytes())
PY" in /home/user/Projects/cx-final-netcmdfix
 succeeded in 0ms:
PRE simulated SMT notify-retry: 0/5; last output: --ratio: SMT placement requires even logical io and ex counts
FAIL: notification topology
PRE simulated SMT flush: 0/5; last output: --ratio: SMT placement requires even logical io and ex counts
FAIL: flush topology

diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
new file mode 100644
index 0000000000000000000000000000000000000000..bc72f719b1e93668f4c671000b1d5bad75da7ac0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
@@ -0,0 +1,16 @@
+#define _GNU_SOURCE
+#include <dlfcn.h>
+#include <stdio.h>
+#include <string.h>
+
+/* Read-only topology simulation: the process remains pinned to real CPUs 44-51.
+ * These serverless fixtures never launch workers. Only sibling metadata changes. */
+FILE *fopen(const char *path, const char *mode) {
+    static char pairs[2][7] = {"44-45\n", "46-47\n"};
+    int cpu = -1, end = 0;
+    if (sscanf(path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list%n",
+               &cpu, &end) == 1 && end && !path[end] && cpu >= 44 && cpu <= 47)
+        return fmemopen(pairs[(cpu - 44) / 2], 6, "r");
+    FILE *(*real_fopen)(const char *, const char *) = dlsym(RTLD_NEXT, "fopen");
+    return real_fopen(path, mode);
+}

codex
The evidence now supports (b) for all three. Unchanged `notify-retry` and `flush` each passed 5/5 on eight physical cores, then failed 0/5 with simulated SMT metadata while execution stayed on the assigned cores. `output` failed 0/5; GDB located the crash in its missing configuration mailbox. I’ll correct the fixtures while retaining their behavioral assertions.

apply patch
patch: completed
/home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
new file mode 100644
index 0000000000000000000000000000000000000000..bc72f719b1e93668f4c671000b1d5bad75da7ac0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
@@ -0,0 +1,16 @@
+#define _GNU_SOURCE
+#include <dlfcn.h>
+#include <stdio.h>
+#include <string.h>
+
+/* Read-only topology simulation: the process remains pinned to real CPUs 44-51.
+ * These serverless fixtures never launch workers. Only sibling metadata changes. */
+FILE *fopen(const char *path, const char *mode) {
+    static char pairs[2][7] = {"44-45\n", "46-47\n"};
+    int cpu = -1, end = 0;
+    if (sscanf(path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list%n",
+               &cpu, &end) == 1 && end && !path[end] && cpu >= 44 && cpu <= 47)
+        return fmemopen(pairs[(cpu - 44) / 2], 6, "r");
+    FILE *(*real_fopen)(const char *, const char *) = dlsym(RTLD_NEXT, "fopen");
+    return real_fopen(path, mode);
+}
diff --git a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
index 6d11e9ce5b2552d2f792115b3076459979edbe39..c04a11b7fc4a6c500632d4e2c48b7949c2dc8c6e
--- a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
+++ b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
@@ -60,7 +60,9 @@
 
     static void notify_retry() {
         Server server;
-        Config cfg; cfg.even_ifid = 1; cfg.even_ex = 1; cfg.shards = 16;
+        // The gate runs this serverless row without taskset. Automatic SMT placement requires
+        // whole sibling pairs for BOTH roles; only one IO is a tracking destination below.
+        Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "notification topology");
         server.threads_.resize(server.placement().total_threads());
         for (uint32_t i = 0; i < server.threads_.size(); ++i) {
@@ -94,7 +96,8 @@
     }
 
     static void flush() {
-        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 1; cfg.shards = 16;
+        // Two IOs still prove the foreign delivery; an even EX count also admits SMT placement.
+        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "flush topology");
         const auto& ios = server.placement().ifid_threads();
         check(ios.size() == 2, "foreign tracking IO exists");
@@ -136,7 +139,16 @@
         Server server; ThreadCtx self; IoLoop loop;
         loop.srv_ = &server; loop.self_ = &self;
         sender.bind(loop.wb_);
-        server.live_obuf_normal_hard_.store(8); server.client_obuf_armed_.store(true);
+        // Mirror Server::init's mailbox initialization without starting workers or opening rings.
+        // cx-waits made raw live-field stores writer-only; IO reads the published snapshot.
+        server.live_config_committed_ = server.capture_live_config(server.live_config_version_.load());
+        server.live_config_mailboxes_ = std::make_unique<LiveConfigMailbox[]>(server.nthreads() + 1);
+        server.live_config_mailboxes_[self.id()].init(server.live_config_committed_);
+        ClientOutputBufferLimits limits{}; limits.normal.hard_bytes = 8;
+        server.set_client_output_buffer_limits(limits);
+        check(server.client_obuf_armed() &&
+              server.client_limits_snapshot(self.id()).normal.hard_bytes == 8,
+              "hard limit published to the IO config mailbox");
         Client held(-1); held.set_id(1);
         check(held.rob().acquire(), "held ordinary operation"); held.rob().publish();
         check(!held.blocked(), "ordinary operation, not blocking exception");

exec
/bin/bash -lc 'taskset -c 44-55 make -j6 build/netcmd-unit > build/netcmdfix/build-post.log 2>&1' in /home/user/Projects/cx-final-netcmdfix
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
new file mode 100644
index 0000000000000000000000000000000000000000..bc72f719b1e93668f4c671000b1d5bad75da7ac0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
@@ -0,0 +1,16 @@
+#define _GNU_SOURCE
+#include <dlfcn.h>
+#include <stdio.h>
+#include <string.h>
+
+/* Read-only topology simulation: the process remains pinned to real CPUs 44-51.
+ * These serverless fixtures never launch workers. Only sibling metadata changes. */
+FILE *fopen(const char *path, const char *mode) {
+    static char pairs[2][7] = {"44-45\n", "46-47\n"};
+    int cpu = -1, end = 0;
+    if (sscanf(path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list%n",
+               &cpu, &end) == 1 && end && !path[end] && cpu >= 44 && cpu <= 47)
+        return fmemopen(pairs[(cpu - 44) / 2], 6, "r");
+    FILE *(*real_fopen)(const char *, const char *) = dlsym(RTLD_NEXT, "fopen");
+    return real_fopen(path, mode);
+}
diff --git a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
index 6d11e9ce5b2552d2f792115b3076459979edbe39..c04a11b7fc4a6c500632d4e2c48b7949c2dc8c6e
--- a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
+++ b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
@@ -60,7 +60,9 @@
 
     static void notify_retry() {
         Server server;
-        Config cfg; cfg.even_ifid = 1; cfg.even_ex = 1; cfg.shards = 16;
+        // The gate runs this serverless row without taskset. Automatic SMT placement requires
+        // whole sibling pairs for BOTH roles; only one IO is a tracking destination below.
+        Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "notification topology");
         server.threads_.resize(server.placement().total_threads());
         for (uint32_t i = 0; i < server.threads_.size(); ++i) {
@@ -94,7 +96,8 @@
     }
 
     static void flush() {
-        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 1; cfg.shards = 16;
+        // Two IOs still prove the foreign delivery; an even EX count also admits SMT placement.
+        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "flush topology");
         const auto& ios = server.placement().ifid_threads();
         check(ios.size() == 2, "foreign tracking IO exists");
@@ -136,7 +139,16 @@
         Server server; ThreadCtx self; IoLoop loop;
         loop.srv_ = &server; loop.self_ = &self;
         sender.bind(loop.wb_);
-        server.live_obuf_normal_hard_.store(8); server.client_obuf_armed_.store(true);
+        // Mirror Server::init's mailbox initialization without starting workers or opening rings.
+        // cx-waits made raw live-field stores writer-only; IO reads the published snapshot.
+        server.live_config_committed_ = server.capture_live_config(server.live_config_version_.load());
+        server.live_config_mailboxes_ = std::make_unique<LiveConfigMailbox[]>(server.nthreads() + 1);
+        server.live_config_mailboxes_[self.id()].init(server.live_config_committed_);
+        ClientOutputBufferLimits limits{}; limits.normal.hard_bytes = 8;
+        server.set_client_output_buffer_limits(limits);
+        check(server.client_obuf_armed() &&
+              server.client_limits_snapshot(self.id()).normal.hard_bytes == 8,
+              "hard limit published to the IO config mailbox");
         Client held(-1); held.set_id(1);
         check(held.rob().acquire(), "held ordinary operation"); held.rob().publish();
         check(!held.blocked(), "ordinary operation, not blocking exception");

 succeeded in 32345ms:
exec
/bin/bash -lc "git merge-base c8e61f646 e94b97100 && git show --no-patch --format=fuller c33228203 && rg -n -C 2 'enable-debug-command|--overlap|read_local|bool read|protected_mode|proto_max_bulk_len' src/core/config.h | head -65 && rg -n -C 2 'read_local_keyspace_hits|read_local_hits|read_local_mget_local_hits' src/cmd/info_stats.cc" in /home/user/Projects/cx-final-netcmdfix
 exited 1 in 0ms:
c8e61f64628655bf99acb91cd986684e5c608fde
commit c33228203cd4fb0410f4ab867b767d636f69f643
Author:     henry <henrychen917@protonmail.com>
AuthorDate: Tue Sep 8 07:42:05 2026 +0800
Commit:     henry <henrychen917@protonmail.com>
CommitDate: Tue Sep 8 07:42:05 2026 +0800

    WIP waits: codex output, unbuilt and unmeasured
    
    Preserved so a stray checkout cannot lose it. Needs build, gate and benchmark before any verdict.
    
    Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
313-    // ---- security / test commands ----------------------------------------------------------
314-    const char* requirepass = nullptr;   // empty/unset = off; live via CONFIG SET
315:    uint32_t protected_mode = 1;         // live 0|1; rejects unauthenticated non-loopback accepts
316-    DebugCommandMode enable_debug_command = DebugCommandMode::No; // boot-only: no|yes|local
317-    const char* aclfile = nullptr;        // boot-only; empty/unset disables ACL LOAD/SAVE
--
330-    ThreadMode thread_mode = ThreadMode::Split;
331-    // Boot-only local-read lane; its internal winners are fixed.
332:    uint32_t read_local = 0;            // boot-only 0|1; GET/MGET lane in 1s or shard-less 2s IO
333-    const char* appendfilename = "appendonly.aof";
334-    const char* appenddirname = "appendonlydir";
--
341-    // bulk lengths; the 32-bit Slice ABI sets the supported ceiling checked by the parser below.
342-    uint32_t databases = 1;
343:    uint64_t proto_max_bulk_len = 512ull * 1024 * 1024;
344-
345-    // ---- data path (live via CONFIG SET) ----------------------------------------------------
--
676-            const char* value = next(nullptr);
677-            if (value && (!std::strcmp(value, "1") || !std::strcmp(value, "yes")))
678:                cfg.protected_mode = 1;
679-            else if (value && (!std::strcmp(value, "0") || !std::strcmp(value, "no")))
680:                cfg.protected_mode = 0;
681-            else {
682-                std::fprintf(stderr, "--protected-mode wants 0, 1, yes or no\n");
--
684-            }
685-        }
686:        else if (!std::strcmp(a, "--enable-debug-command")) {
687-            const char* value = next(nullptr);
688-            if (value && !std::strcmp(value, "no")) cfg.enable_debug_command = DebugCommandMode::No;
--
690-            else if (value && !std::strcmp(value, "local")) cfg.enable_debug_command = DebugCommandMode::Local;
691-            else {
692:                std::fprintf(stderr, "--enable-debug-command wants no, yes or local\n");
693-                return kConfigError;
694-            }
--
706-            }
707-        }
708:        else if (!std::strcmp(a, "--overlap")) {
709-            if (!cfg_parse_u32(next(nullptr), cfg.overlap) || cfg.overlap > 1) {
710:                std::fprintf(stderr, "--overlap wants 0 or 1\n");
711-                return kConfigError;
712-            }
713-        }
714-        else if (!std::strcmp(a, "--read-local")) {
715:            if (!cfg_parse_u32(next(nullptr), cfg.read_local) || cfg.read_local > 1) {
716-                std::fprintf(stderr, "--read-local wants 0 or 1\n");
717-                return kConfigError;
--
853-                return kConfigError;
854-            }
855:            cfg.proto_max_bulk_len = value;
856-        }
857-        else if (!std::strcmp(a, "--dir"))        cfg.dir = next(".");
--
957-                        "  without the leading --; `pin no` spells --no-pin. CLI flags override the\n"
958-                        "  file. See tomokv.conf in the repo root for the annotated full set.\n"
959:                        "  threading: --thread-mode 2s|1s --overlap 0|1 --read-local 0|1 (defaults 2s, 0, 0)\n"
960-                        "             (split/fused are mode aliases)\n"
961:                        "    --overlap 1                 2s: IO-overlapped writeback; 1s: all overlap (uring)\n"
962-                        "    --reorder 0|1 (default 0)   cross-connection reordering in an executor batch for latency; per-connection order always preserved\n"
963-                        "  placement (default derived from allowed CPUs):\n"
--

exec
/bin/bash -lc "git diff --check && git diff --stat && rg -n 'static_assert\\(sizeof\\((Op|Client|ThreadCtx|Shard|FlatStore|Rob|AtomicEntry|Config)' src && rg -n '"'^NET_IO=|''^GATE_RATIO=|''^EXPECT_|''^CORES=|''^py'"\\(' tests/gate.sh && sed -n '438,451p' tests/gate.sh && tail -2 build/netcmdfix/build-post.log" in /home/user/Projects/cx-final-netcmdfix
 succeeded in 0ms:
 tests/netcmd_unit.cc | 18 +++++++++++++++---
 1 file changed, 15 insertions(+), 3 deletions(-)
src/core/thread.h:1205:static_assert(sizeof(ThreadCtx) == 1408,
src/core/shard.h:523:static_assert(sizeof(Shard) == 1440);
src/core/config.h:406:static_assert(sizeof(Config) == 624, "Config footprint changed; update the documented accounting");
src/net/rob.h:1068:static_assert(sizeof(Rob<64>) == 192, "Rob<64> layout changed");
src/net/conn.h:900:static_assert(sizeof(Client) == 1984, "Client grew: re-check the io-hot line packing and idle RSS");
src/exec/op.h:463:static_assert(sizeof(Op) == 336, "Op grew: pay for it inside the struct or re-run the 64c A/B");
src/store/atomic_mvcc.h:70:static_assert(sizeof(AtomicEntry) == 144);
src/store/flatstore.h:3973:static_assert(sizeof(FlatStore) == 944);
29:CORES=${GATE_CORES:-0-7}
141:EXPECT_QUICK=377
142:EXPECT_FULL=394                 # full without the optional NIC row.
224:py(){ # python3 battery wrapper: bounded wall time. A timeout or a self-skip (exit 3, see
1216:NET_IO=$NET_IO GATE_PORT=$PORT GATE_CORES=$CORES tests/aof_rewrite_matrix.sh \
1222:NET_IO=$NET_IO GATE_PORT=$PORT GATE_CORES=$CORES tests/aof_rewrite_trigger_matrix.sh \
  for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null || break; sleep 0.1; done
  sleep "${GATE_STOP_SETTLE:-1}"
}
launch(){ # logtag binary args... -> pid in $SRV, log in $SRVLOG; waits up to 30 s for the port
  local tag=$1 bin=$2; shift 2
  quiet_wait
  SRV=0; SRVLOG=/dev/null
  guard_port "$PORT"
  [ -x "$bin" ] || { say "boot ($tag)" "FAIL ($bin is not an executable)"; return 1; }
  SRVLOG=$(mktemp "/tmp/gate-srv-$tag.XXXXXX")
  # Every ordinary boot gets an empty persistence directory. Explicit recovery arms override it
  # through their later --dir argument; a previous battery's SAVE must not become this one's input.
  local boot_dir
  boot_dir=$(mktemp -d "/tmp/gate-data-$tag.XXXXXX") || return 1
g++ -std=c++20 -O2 -g -Wall -Wextra -march=native -pthread -DTOMO_JEMALLOC -Wno-mismatched-new-delete -I. -c tests/netcmd_unit.cc -o build/tests/netcmd_unit.o
g++ -std=c++20 -O2 -g -Wall -Wextra -march=native -pthread build/tests/netcmd_unit.o build/tests/netcmd_stream_unit.o build/tests/netcmd_zset_unit.o build/tests/netcmd_config_unit.o build/src/net/tls.o build/src/cmd/commands.o build/src/cmd/glob.o build/src/cmd/acl.o build/src/cmd/hll.o build/src/cmd/t_server.o build/src/cmd/t_string.o build/src/cmd/t_string_notify.o build/src/cmd/t_hash.o build/src/cmd/t_hash_ttl.o build/src/cmd/t_list.o build/src/cmd/t_set.o build/src/cmd/t_zset_ops.o build/src/cmd/geo.o build/src/cmd/t_stream.o build/src/cmd/scripting.o build/src/cmd/functions.o build/src/cmd/serialize.o build/src/snapshot/snapshot.o build/src/persist/aof.o build/src/cmd/climon.o build/src/cmd/tracking.o build/src/cmd/slowlog.o build/src/cmd/lcs.o build/src/cmd/info_stats.o build/src/cmd/lbsignals.o build/src/core/flipctl.o build/src/core/genthread.o build/src/core/rl2s.o build/src/cmd/cmdgap.o build/src/cmd/pfdebug.o build/src/cmd/cmdmeta.o build/src/cmd/t_sort.o -o build/netcmd-unit -ljemalloc -luring -pthread -lssl -lcrypto -lm -Wl,--wrap=mkstemp -Wl,--wrap=fopen

diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
new file mode 100644
index 0000000000000000000000000000000000000000..bc72f719b1e93668f4c671000b1d5bad75da7ac0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
@@ -0,0 +1,16 @@
+#define _GNU_SOURCE
+#include <dlfcn.h>
+#include <stdio.h>
+#include <string.h>
+
+/* Read-only topology simulation: the process remains pinned to real CPUs 44-51.
+ * These serverless fixtures never launch workers. Only sibling metadata changes. */
+FILE *fopen(const char *path, const char *mode) {
+    static char pairs[2][7] = {"44-45\n", "46-47\n"};
+    int cpu = -1, end = 0;
+    if (sscanf(path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list%n",
+               &cpu, &end) == 1 && end && !path[end] && cpu >= 44 && cpu <= 47)
+        return fmemopen(pairs[(cpu - 44) / 2], 6, "r");
+    FILE *(*real_fopen)(const char *, const char *) = dlsym(RTLD_NEXT, "fopen");
+    return real_fopen(path, mode);
+}
diff --git a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
index 6d11e9ce5b2552d2f792115b3076459979edbe39..c04a11b7fc4a6c500632d4e2c48b7949c2dc8c6e
--- a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
+++ b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
@@ -60,7 +60,9 @@
 
     static void notify_retry() {
         Server server;
-        Config cfg; cfg.even_ifid = 1; cfg.even_ex = 1; cfg.shards = 16;
+        // The gate runs this serverless row without taskset. Automatic SMT placement requires
+        // whole sibling pairs for BOTH roles; only one IO is a tracking destination below.
+        Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "notification topology");
         server.threads_.resize(server.placement().total_threads());
         for (uint32_t i = 0; i < server.threads_.size(); ++i) {
@@ -94,7 +96,8 @@
     }
 
     static void flush() {
-        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 1; cfg.shards = 16;
+        // Two IOs still prove the foreign delivery; an even EX count also admits SMT placement.
+        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "flush topology");
         const auto& ios = server.placement().ifid_threads();
         check(ios.size() == 2, "foreign tracking IO exists");
@@ -136,7 +139,16 @@
         Server server; ThreadCtx self; IoLoop loop;
         loop.srv_ = &server; loop.self_ = &self;
         sender.bind(loop.wb_);
-        server.live_obuf_normal_hard_.store(8); server.client_obuf_armed_.store(true);
+        // Mirror Server::init's mailbox initialization without starting workers or opening rings.
+        // cx-waits made raw live-field stores writer-only; IO reads the published snapshot.
+        server.live_config_committed_ = server.capture_live_config(server.live_config_version_.load());
+        server.live_config_mailboxes_ = std::make_unique<LiveConfigMailbox[]>(server.nthreads() + 1);
+        server.live_config_mailboxes_[self.id()].init(server.live_config_committed_);
+        ClientOutputBufferLimits limits{}; limits.normal.hard_bytes = 8;
+        server.set_client_output_buffer_limits(limits);
+        check(server.client_obuf_armed() &&
+              server.client_limits_snapshot(self.id()).normal.hard_bytes == 8,
+              "hard limit published to the IO config mailbox");
         Client held(-1); held.set_id(1);
         check(held.rob().acquire(), "held ordinary operation"); held.rob().publish();
         check(!held.blocked(), "ordinary operation, not blocking exception");

exec
/bin/bash -lc "ulimit -c 0
python3 - <<'PY'
from pathlib import Path
import json, subprocess
root = Path('build/netcmdfix').resolve()
results = []
for geometry in ('physical', 'smt'):
    for case in ('notify-retry', 'flush', 'output'):
        for run in range(1, 6):
            command = ['taskset', '-c', '44-51', 'timeout', '60']
            if geometry == 'smt':
                command += ['env', 'TOMOKV_L3_DOMAINS=44-47', f'LD_PRELOAD={root / \"smt-sysfs.so\"}']
            command += ['./build/netcmd-unit', case]
            r = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            (root / f'post-{geometry}-{case}-{run}.log').write_text(r.stdout)
            results.append({'geometry':geometry, 'case':case, 'run':run, 'returncode':r.returncode, 'command':command})
        print(f'POST {geometry} {case}: {sum(r[\"returncode\"] == 0 for r in results if r[\"case\"] == case and r[\"geometry\"] == geometry)}/5; {r.stdout.strip()}', flush=True)
(root / 'post-results.json').write_text(json.dumps(results, indent=2) + '\\n')
raise SystemExit(any(r['returncode'] for r in results))
PY" in /home/user/Projects/cx-final-netcmdfix
 succeeded in 0ms:
POST physical notify-retry: 5/5; ok: netcmd notify-retry
POST physical flush: 5/5; ok: netcmd flush
POST physical output: 5/5; Client id=1 closed for overcoming of output buffer limits.
ok: netcmd output
POST smt notify-retry: 5/5; ok: netcmd notify-retry
POST smt flush: 5/5; ok: netcmd flush
POST smt output: 5/5; Client id=1 closed for overcoming of output buffer limits.
ok: netcmd output

diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
new file mode 100644
index 0000000000000000000000000000000000000000..bc72f719b1e93668f4c671000b1d5bad75da7ac0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
@@ -0,0 +1,16 @@
+#define _GNU_SOURCE
+#include <dlfcn.h>
+#include <stdio.h>
+#include <string.h>
+
+/* Read-only topology simulation: the process remains pinned to real CPUs 44-51.
+ * These serverless fixtures never launch workers. Only sibling metadata changes. */
+FILE *fopen(const char *path, const char *mode) {
+    static char pairs[2][7] = {"44-45\n", "46-47\n"};
+    int cpu = -1, end = 0;
+    if (sscanf(path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list%n",
+               &cpu, &end) == 1 && end && !path[end] && cpu >= 44 && cpu <= 47)
+        return fmemopen(pairs[(cpu - 44) / 2], 6, "r");
+    FILE *(*real_fopen)(const char *, const char *) = dlsym(RTLD_NEXT, "fopen");
+    return real_fopen(path, mode);
+}
diff --git a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
index 6d11e9ce5b2552d2f792115b3076459979edbe39..c04a11b7fc4a6c500632d4e2c48b7949c2dc8c6e
--- a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
+++ b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
@@ -60,7 +60,9 @@
 
     static void notify_retry() {
         Server server;
-        Config cfg; cfg.even_ifid = 1; cfg.even_ex = 1; cfg.shards = 16;
+        // The gate runs this serverless row without taskset. Automatic SMT placement requires
+        // whole sibling pairs for BOTH roles; only one IO is a tracking destination below.
+        Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "notification topology");
         server.threads_.resize(server.placement().total_threads());
         for (uint32_t i = 0; i < server.threads_.size(); ++i) {
@@ -94,7 +96,8 @@
     }
 
     static void flush() {
-        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 1; cfg.shards = 16;
+        // Two IOs still prove the foreign delivery; an even EX count also admits SMT placement.
+        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "flush topology");
         const auto& ios = server.placement().ifid_threads();
         check(ios.size() == 2, "foreign tracking IO exists");
@@ -136,7 +139,16 @@
         Server server; ThreadCtx self; IoLoop loop;
         loop.srv_ = &server; loop.self_ = &self;
         sender.bind(loop.wb_);
-        server.live_obuf_normal_hard_.store(8); server.client_obuf_armed_.store(true);
+        // Mirror Server::init's mailbox initialization without starting workers or opening rings.
+        // cx-waits made raw live-field stores writer-only; IO reads the published snapshot.
+        server.live_config_committed_ = server.capture_live_config(server.live_config_version_.load());
+        server.live_config_mailboxes_ = std::make_unique<LiveConfigMailbox[]>(server.nthreads() + 1);
+        server.live_config_mailboxes_[self.id()].init(server.live_config_committed_);
+        ClientOutputBufferLimits limits{}; limits.normal.hard_bytes = 8;
+        server.set_client_output_buffer_limits(limits);
+        check(server.client_obuf_armed() &&
+              server.client_limits_snapshot(self.id()).normal.hard_bytes == 8,
+              "hard limit published to the IO config mailbox");
         Client held(-1); held.set_id(1);
         check(held.rob().acquire(), "held ordinary operation"); held.rob().publish();
         check(!held.blocked(), "ordinary operation, not blocking exception");

codex
All three corrected rows passed 5/5 on physical-core topology and 5/5 with simulated SMT pairing. Next I’ll run them against disposable builds with invalidation retention, flush publication, or output accounting deliberately broken, then check the live netcmd battery in both thread modes.

apply patch
patch: completed
/home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
new file mode 100644
index 0000000000000000000000000000000000000000..b6494711491bb3710f859aa5fb7e295a4e3b093b
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
@@ -0,0 +1,77 @@
+"""Disposable source copies only; never modifies the production sources or objects."""
+import json
+from pathlib import Path
+import resource
+import shlex
+import shutil
+import signal
+import subprocess
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix'
+recipes = subprocess.check_output(['make', '-n', '-B', 'build/netcmd-unit'], text=True).splitlines()
+link = shlex.split(next(line for line in recipes if ' -o build/netcmd-unit ' in line))
+variants = [
+    ('notify-drop', 'notify-retry', 'src/cmd/notify.inc',
+     'return work;  // keep this target and the keyless record for the next pass',
+     'slot->keyless.pop_front(); return work + 1;  // deliberately lose the invalidation',
+     'tests/netcmd_unit.cc', 'failed marker post is incomplete work'),
+    ('flush-early', 'flush', 'src/cmd/climon.cc',
+     'if (armed & Server::kClimonTracking) {\n        ClimonConn* state',
+     'if (armed & Server::kClimonTracking) {\n'
+     '        if (op.cmd_name().eq_icase("flushall") || op.cmd_name().eq_icase("flushdb"))\n'
+     '            tracking_broadcast_flush();\n        ClimonConn* state',
+     'src/cmd/climon.cc', 'no flush invalidation before owner work was published'),
+    ('flush-missing', 'flush', 'src/cmd/climon.cc',
+     'void IoLoop::climon_flush_completed(Op& op) {\n',
+     'void IoLoop::climon_flush_completed(Op& op) {\n    return; // omit completed invalidation\n',
+     'src/cmd/climon.cc', 'completed flush invalidates foreign tracking IO'),
+    ('output-untracked', 'output', 'src/net/wb.h',
+     'else conn.stop_obuf_tracking();',
+     'else {} // deliberately leave accounting armed for uncounted bytes',
+     'tests/netcmd_unit.cc', None),
+    ('output-deferred', 'output', 'src/core/io_loop.h',
+     'uint64_t used = c->obuf_bytes() + wb_.deferred_output_bytes(*c);',
+     'uint64_t used = c->obuf_bytes(); // deliberately omit deferred storage',
+     'tests/netcmd_unit.cc', 'hard limit fired before Done'),
+]
+results = []
+for name, case, source, before, after, tu, diagnostic in variants:
+    tree = out / 'mutants' / name
+    tree.mkdir(parents=True)
+    shutil.copytree(root / 'src', tree / 'src')
+    (tree / 'third_party').symlink_to(root / 'third_party', target_is_directory=True)
+    (tree / 'tests').mkdir()
+    for test in ('netcmd_unit.cc', 'netcmd_unit.h'):
+        shutil.copy2(root / 'tests' / test, tree / 'tests' / test)
+    target = tree / source
+    original = target.read_text()
+    assert original.count(before) == (2 if name == 'output-untracked' else 1)
+    target.write_text(original.replace(before, after))
+    old_object = 'build/tests/netcmd_unit.o' if tu.startswith('tests/') else 'build/src/cmd/climon.o'
+    compile_cmd = shlex.split(next(line for line in recipes if f' -c {tu} -o {old_object}' in line))
+    new_object = str(tree / 'mutant.o')
+    compile_cmd = [str(tree / tu) if arg == tu else new_object if arg == old_object
+                   else f'-I{tree}' if arg == '-I.' else arg for arg in compile_cmd]
+    binary = str(tree / 'netcmd-unit')
+    link_cmd = [new_object if arg == old_object else binary if arg == 'build/netcmd-unit' else arg
+                for arg in link]
+    print(f'building {name}', flush=True)
+    (tree / 'commands.json').write_text(json.dumps([compile_cmd, link_cmd], indent=2) + '\n')
+    with (tree / 'build.log').open('w') as log:
+        subprocess.run(['taskset', '-c', '44-55', *compile_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=300)
+        subprocess.run(['taskset', '-c', '44-55', *link_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=120)
+    for run in range(1, 6):
+        command = ['taskset', '-c', '44-51', 'timeout', '60', binary, case]
+        r = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+        (tree / f'run-{run}.log').write_text(r.stdout)
+        expected = r.returncode == -signal.SIGABRT if diagnostic is None else (
+            r.returncode == 1 and f'FAIL: {diagnostic}' in r.stdout)
+        results.append({'variant':name, 'case':case, 'run':run, 'returncode':r.returncode,
+                        'expected_failure':expected, 'output':r.stdout, 'command':command})
+        assert expected, (name, r.returncode, r.stdout)
+    print(f'{name}: rejected 5/5; {r.stdout.strip()}', flush=True)
+    (out / 'negative-results.json').write_text(json.dumps(results, indent=2) + '\n')
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
new file mode 100644
index 0000000000000000000000000000000000000000..bc72f719b1e93668f4c671000b1d5bad75da7ac0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
@@ -0,0 +1,16 @@
+#define _GNU_SOURCE
+#include <dlfcn.h>
+#include <stdio.h>
+#include <string.h>
+
+/* Read-only topology simulation: the process remains pinned to real CPUs 44-51.
+ * These serverless fixtures never launch workers. Only sibling metadata changes. */
+FILE *fopen(const char *path, const char *mode) {
+    static char pairs[2][7] = {"44-45\n", "46-47\n"};
+    int cpu = -1, end = 0;
+    if (sscanf(path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list%n",
+               &cpu, &end) == 1 && end && !path[end] && cpu >= 44 && cpu <= 47)
+        return fmemopen(pairs[(cpu - 44) / 2], 6, "r");
+    FILE *(*real_fopen)(const char *, const char *) = dlsym(RTLD_NEXT, "fopen");
+    return real_fopen(path, mode);
+}
diff --git a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
index 6d11e9ce5b2552d2f792115b3076459979edbe39..c04a11b7fc4a6c500632d4e2c48b7949c2dc8c6e
--- a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
+++ b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
@@ -60,7 +60,9 @@
 
     static void notify_retry() {
         Server server;
-        Config cfg; cfg.even_ifid = 1; cfg.even_ex = 1; cfg.shards = 16;
+        // The gate runs this serverless row without taskset. Automatic SMT placement requires
+        // whole sibling pairs for BOTH roles; only one IO is a tracking destination below.
+        Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "notification topology");
         server.threads_.resize(server.placement().total_threads());
         for (uint32_t i = 0; i < server.threads_.size(); ++i) {
@@ -94,7 +96,8 @@
     }
 
     static void flush() {
-        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 1; cfg.shards = 16;
+        // Two IOs still prove the foreign delivery; an even EX count also admits SMT placement.
+        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "flush topology");
         const auto& ios = server.placement().ifid_threads();
         check(ios.size() == 2, "foreign tracking IO exists");
@@ -136,7 +139,16 @@
         Server server; ThreadCtx self; IoLoop loop;
         loop.srv_ = &server; loop.self_ = &self;
         sender.bind(loop.wb_);
-        server.live_obuf_normal_hard_.store(8); server.client_obuf_armed_.store(true);
+        // Mirror Server::init's mailbox initialization without starting workers or opening rings.
+        // cx-waits made raw live-field stores writer-only; IO reads the published snapshot.
+        server.live_config_committed_ = server.capture_live_config(server.live_config_version_.load());
+        server.live_config_mailboxes_ = std::make_unique<LiveConfigMailbox[]>(server.nthreads() + 1);
+        server.live_config_mailboxes_[self.id()].init(server.live_config_committed_);
+        ClientOutputBufferLimits limits{}; limits.normal.hard_bytes = 8;
+        server.set_client_output_buffer_limits(limits);
+        check(server.client_obuf_armed() &&
+              server.client_limits_snapshot(self.id()).normal.hard_bytes == 8,
+              "hard limit published to the IO config mailbox");
         Client held(-1); held.set_id(1);
         check(held.rob().acquire(), "held ordinary operation"); held.rob().publish();
         check(!held.blocked(), "ordinary operation, not blocking exception");

exec
/bin/bash -lc 'taskset -c 44-55 python3 -u build/netcmdfix/negative_controls.py > build/netcmdfix/negative-progress.log 2>&1' in /home/user/Projects/cx-final-netcmdfix
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
new file mode 100644
index 0000000000000000000000000000000000000000..b6494711491bb3710f859aa5fb7e295a4e3b093b
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
@@ -0,0 +1,77 @@
+"""Disposable source copies only; never modifies the production sources or objects."""
+import json
+from pathlib import Path
+import resource
+import shlex
+import shutil
+import signal
+import subprocess
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix'
+recipes = subprocess.check_output(['make', '-n', '-B', 'build/netcmd-unit'], text=True).splitlines()
+link = shlex.split(next(line for line in recipes if ' -o build/netcmd-unit ' in line))
+variants = [
+    ('notify-drop', 'notify-retry', 'src/cmd/notify.inc',
+     'return work;  // keep this target and the keyless record for the next pass',
+     'slot->keyless.pop_front(); return work + 1;  // deliberately lose the invalidation',
+     'tests/netcmd_unit.cc', 'failed marker post is incomplete work'),
+    ('flush-early', 'flush', 'src/cmd/climon.cc',
+     'if (armed & Server::kClimonTracking) {\n        ClimonConn* state',
+     'if (armed & Server::kClimonTracking) {\n'
+     '        if (op.cmd_name().eq_icase("flushall") || op.cmd_name().eq_icase("flushdb"))\n'
+     '            tracking_broadcast_flush();\n        ClimonConn* state',
+     'src/cmd/climon.cc', 'no flush invalidation before owner work was published'),
+    ('flush-missing', 'flush', 'src/cmd/climon.cc',
+     'void IoLoop::climon_flush_completed(Op& op) {\n',
+     'void IoLoop::climon_flush_completed(Op& op) {\n    return; // omit completed invalidation\n',
+     'src/cmd/climon.cc', 'completed flush invalidates foreign tracking IO'),
+    ('output-untracked', 'output', 'src/net/wb.h',
+     'else conn.stop_obuf_tracking();',
+     'else {} // deliberately leave accounting armed for uncounted bytes',
+     'tests/netcmd_unit.cc', None),
+    ('output-deferred', 'output', 'src/core/io_loop.h',
+     'uint64_t used = c->obuf_bytes() + wb_.deferred_output_bytes(*c);',
+     'uint64_t used = c->obuf_bytes(); // deliberately omit deferred storage',
+     'tests/netcmd_unit.cc', 'hard limit fired before Done'),
+]
+results = []
+for name, case, source, before, after, tu, diagnostic in variants:
+    tree = out / 'mutants' / name
+    tree.mkdir(parents=True)
+    shutil.copytree(root / 'src', tree / 'src')
+    (tree / 'third_party').symlink_to(root / 'third_party', target_is_directory=True)
+    (tree / 'tests').mkdir()
+    for test in ('netcmd_unit.cc', 'netcmd_unit.h'):
+        shutil.copy2(root / 'tests' / test, tree / 'tests' / test)
+    target = tree / source
+    original = target.read_text()
+    assert original.count(before) == (2 if name == 'output-untracked' else 1)
+    target.write_text(original.replace(before, after))
+    old_object = 'build/tests/netcmd_unit.o' if tu.startswith('tests/') else 'build/src/cmd/climon.o'
+    compile_cmd = shlex.split(next(line for line in recipes if f' -c {tu} -o {old_object}' in line))
+    new_object = str(tree / 'mutant.o')
+    compile_cmd = [str(tree / tu) if arg == tu else new_object if arg == old_object
+                   else f'-I{tree}' if arg == '-I.' else arg for arg in compile_cmd]
+    binary = str(tree / 'netcmd-unit')
+    link_cmd = [new_object if arg == old_object else binary if arg == 'build/netcmd-unit' else arg
+                for arg in link]
+    print(f'building {name}', flush=True)
+    (tree / 'commands.json').write_text(json.dumps([compile_cmd, link_cmd], indent=2) + '\n')
+    with (tree / 'build.log').open('w') as log:
+        subprocess.run(['taskset', '-c', '44-55', *compile_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=300)
+        subprocess.run(['taskset', '-c', '44-55', *link_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=120)
+    for run in range(1, 6):
+        command = ['taskset', '-c', '44-51', 'timeout', '60', binary, case]
+        r = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+        (tree / f'run-{run}.log').write_text(r.stdout)
+        expected = r.returncode == -signal.SIGABRT if diagnostic is None else (
+            r.returncode == 1 and f'FAIL: {diagnostic}' in r.stdout)
+        results.append({'variant':name, 'case':case, 'run':run, 'returncode':r.returncode,
+                        'expected_failure':expected, 'output':r.stdout, 'command':command})
+        assert expected, (name, r.returncode, r.stdout)
+    print(f'{name}: rejected 5/5; {r.stdout.strip()}', flush=True)
+    (out / 'negative-results.json').write_text(json.dumps(results, indent=2) + '\n')
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
new file mode 100644
index 0000000000000000000000000000000000000000..bc72f719b1e93668f4c671000b1d5bad75da7ac0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
@@ -0,0 +1,16 @@
+#define _GNU_SOURCE
+#include <dlfcn.h>
+#include <stdio.h>
+#include <string.h>
+
+/* Read-only topology simulation: the process remains pinned to real CPUs 44-51.
+ * These serverless fixtures never launch workers. Only sibling metadata changes. */
+FILE *fopen(const char *path, const char *mode) {
+    static char pairs[2][7] = {"44-45\n", "46-47\n"};
+    int cpu = -1, end = 0;
+    if (sscanf(path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list%n",
+               &cpu, &end) == 1 && end && !path[end] && cpu >= 44 && cpu <= 47)
+        return fmemopen(pairs[(cpu - 44) / 2], 6, "r");
+    FILE *(*real_fopen)(const char *, const char *) = dlsym(RTLD_NEXT, "fopen");
+    return real_fopen(path, mode);
+}
diff --git a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
index 6d11e9ce5b2552d2f792115b3076459979edbe39..c04a11b7fc4a6c500632d4e2c48b7949c2dc8c6e
--- a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
+++ b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
@@ -60,7 +60,9 @@
 
     static void notify_retry() {
         Server server;
-        Config cfg; cfg.even_ifid = 1; cfg.even_ex = 1; cfg.shards = 16;
+        // The gate runs this serverless row without taskset. Automatic SMT placement requires
+        // whole sibling pairs for BOTH roles; only one IO is a tracking destination below.
+        Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "notification topology");
         server.threads_.resize(server.placement().total_threads());
         for (uint32_t i = 0; i < server.threads_.size(); ++i) {
@@ -94,7 +96,8 @@
     }
 
     static void flush() {
-        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 1; cfg.shards = 16;
+        // Two IOs still prove the foreign delivery; an even EX count also admits SMT placement.
+        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "flush topology");
         const auto& ios = server.placement().ifid_threads();
         check(ios.size() == 2, "foreign tracking IO exists");
@@ -136,7 +139,16 @@
         Server server; ThreadCtx self; IoLoop loop;
         loop.srv_ = &server; loop.self_ = &self;
         sender.bind(loop.wb_);
-        server.live_obuf_normal_hard_.store(8); server.client_obuf_armed_.store(true);
+        // Mirror Server::init's mailbox initialization without starting workers or opening rings.
+        // cx-waits made raw live-field stores writer-only; IO reads the published snapshot.
+        server.live_config_committed_ = server.capture_live_config(server.live_config_version_.load());
+        server.live_config_mailboxes_ = std::make_unique<LiveConfigMailbox[]>(server.nthreads() + 1);
+        server.live_config_mailboxes_[self.id()].init(server.live_config_committed_);
+        ClientOutputBufferLimits limits{}; limits.normal.hard_bytes = 8;
+        server.set_client_output_buffer_limits(limits);
+        check(server.client_obuf_armed() &&
+              server.client_limits_snapshot(self.id()).normal.hard_bytes == 8,
+              "hard limit published to the IO config mailbox");
         Client held(-1); held.set_id(1);
         check(held.rob().acquire(), "held ordinary operation"); held.rob().publish();
         check(!held.blocked(), "ordinary operation, not blocking exception");

exec
/bin/bash -lc "sed -n '449,455p' tests/gate.sh && rg -n 'int .*fd|uint16_t .*port|void run|__main__|sys.argv|CONFIG.*SET' tests/netcmd.py tests/_lib.py && rg -n -C 2 'read_local_keyspace_hits|read_local_mget_local_hits|read_local_hits|io_threads|ex_threads' src/cmd/info*.cc src/cmd/t_server.cc src/cmd/*inc | head -50 && git diff -- tests/netcmd_unit.cc" in /home/user/Projects/cx-final-netcmdfix
 succeeded in 0ms:
  # through their later --dir argument; a previous battery's SAVE must not become this one's input.
  local boot_dir
  boot_dir=$(mktemp -d "/tmp/gate-data-$tag.XXXXXX") || return 1
  taskset -c $CORES "$bin" --port $PORT --bind 127.0.0.1 --shards 16 --dir "$boot_dir" "$@" > "$SRVLOG" 2>&1 &
  SRV=$!
  # 30s, not 10s: the AOF replay boot replays its file BEFORE it listens, and on a box shared
  # with other lanes that overran a 10s deadline and turned six AOF rows red with no defect behind
tests/_lib.py:177:    argv = sys.argv if argv is None else argv
tests/netcmd.py:53:            require(c.cmd("CONFIG", "SET", "proto-max-bulk-len", 1048576) == b"OK", "runtime bulk limit set")
tests/netcmd.py:71:            require(c.cmd("CONFIG", "SET", "proto-max-bulk-len", before) == b"OK", "runtime bulk limit restored")
tests/netcmd.py:97:if __name__ == "__main__":
tests/netcmd.py:98:    run(sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1", int(sys.argv[2]) if len(sys.argv) > 2 else 7899)
src/cmd/t_server.cc-2062-            const FlipReport flip = g_server ? g_server->flip_report() : FlipReport{};
src/cmd/t_server.cc-2063-            appendf(body,
src/cmd/t_server.cc:2064:                    "io_threads:%u\r\nex_threads:%u\r\nflip_target_io:%u\r\n"
src/cmd/t_server.cc-2065-                    "flip_target_ex:%u\r\n"
src/cmd/t_server.cc-2066-                    "flip_unit_threads:%u\r\nflip_bucket_min:%u\r\nflip_bucket_max:%u\r\n"
--
src/cmd/t_server.cc-2521-                static_cast<unsigned long long>(foreign_read_poisoned_shards));
src/cmd/t_server.cc-2522-        appendf(body,
src/cmd/t_server.cc:2523:                "read_local_hits:%llu\r\n"
src/cmd/t_server.cc:2524:                "read_local_keyspace_hits:%llu\r\n"
src/cmd/t_server.cc-2525-                "read_local_keyspace_misses:%llu\r\n"
src/cmd/t_server.cc-2526-                "read_local_fallbacks:%llu\r\n"
--
src/cmd/t_server.cc-2572-                static_cast<unsigned long long>(read_local.defer_quota));
src/cmd/t_server.cc-2573-        appendf(body,
src/cmd/t_server.cc:2574:                "read_local_mget_local_hits:%llu\r\n"
src/cmd/t_server.cc-2575-                "read_local_mget_fallbacks:%llu\r\n"
src/cmd/t_server.cc-2576-                "read_local_mget_fallback_multi:%llu\r\n"
diff --git a/tests/netcmd_unit.cc b/tests/netcmd_unit.cc
index 6d11e9ce5..c04a11b7f 100644
--- a/tests/netcmd_unit.cc
+++ b/tests/netcmd_unit.cc
@@ -60,7 +60,9 @@ struct NetcmdRegression {
 
     static void notify_retry() {
         Server server;
-        Config cfg; cfg.even_ifid = 1; cfg.even_ex = 1; cfg.shards = 16;
+        // The gate runs this serverless row without taskset. Automatic SMT placement requires
+        // whole sibling pairs for BOTH roles; only one IO is a tracking destination below.
+        Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "notification topology");
         server.threads_.resize(server.placement().total_threads());
         for (uint32_t i = 0; i < server.threads_.size(); ++i) {
@@ -94,7 +96,8 @@ struct NetcmdRegression {
     }
 
     static void flush() {
-        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 1; cfg.shards = 16;
+        // Two IOs still prove the foreign delivery; an even EX count also admits SMT placement.
+        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "flush topology");
         const auto& ios = server.placement().ifid_threads();
         check(ios.size() == 2, "foreign tracking IO exists");
@@ -136,7 +139,16 @@ struct NetcmdRegression {
         Server server; ThreadCtx self; IoLoop loop;
         loop.srv_ = &server; loop.self_ = &self;
         sender.bind(loop.wb_);
-        server.live_obuf_normal_hard_.store(8); server.client_obuf_armed_.store(true);
+        // Mirror Server::init's mailbox initialization without starting workers or opening rings.
+        // cx-waits made raw live-field stores writer-only; IO reads the published snapshot.
+        server.live_config_committed_ = server.capture_live_config(server.live_config_version_.load());
+        server.live_config_mailboxes_ = std::make_unique<LiveConfigMailbox[]>(server.nthreads() + 1);
+        server.live_config_mailboxes_[self.id()].init(server.live_config_committed_);
+        ClientOutputBufferLimits limits{}; limits.normal.hard_bytes = 8;
+        server.set_client_output_buffer_limits(limits);
+        check(server.client_obuf_armed() &&
+              server.client_limits_snapshot(self.id()).normal.hard_bytes == 8,
+              "hard limit published to the IO config mailbox");
         Client held(-1); held.set_id(1);
         check(held.rob().acquire(), "held ordinary operation"); held.rob().publish();
         check(!held.blocked(), "ordinary operation, not blocking exception");

exec
/bin/bash -lc "tail -4 build/netcmdfix/negative-progress.log && rg -n '"'^(##|#)|notify-retry|flush|output'"' MERGEWAITS.md | head -30 && git status --short" in /home/user/Projects/cx-final-netcmdfix
 succeeded in 0ms:
building notify-drop
notify-drop: rejected 5/5; FAIL: failed marker post is incomplete work
building flush-early
1:# cx-waits integration and resize/retirement regressions
 M tests/netcmd_unit.cc

diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
new file mode 100644
index 0000000000000000000000000000000000000000..b6494711491bb3710f859aa5fb7e295a4e3b093b
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
@@ -0,0 +1,77 @@
+"""Disposable source copies only; never modifies the production sources or objects."""
+import json
+from pathlib import Path
+import resource
+import shlex
+import shutil
+import signal
+import subprocess
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix'
+recipes = subprocess.check_output(['make', '-n', '-B', 'build/netcmd-unit'], text=True).splitlines()
+link = shlex.split(next(line for line in recipes if ' -o build/netcmd-unit ' in line))
+variants = [
+    ('notify-drop', 'notify-retry', 'src/cmd/notify.inc',
+     'return work;  // keep this target and the keyless record for the next pass',
+     'slot->keyless.pop_front(); return work + 1;  // deliberately lose the invalidation',
+     'tests/netcmd_unit.cc', 'failed marker post is incomplete work'),
+    ('flush-early', 'flush', 'src/cmd/climon.cc',
+     'if (armed & Server::kClimonTracking) {\n        ClimonConn* state',
+     'if (armed & Server::kClimonTracking) {\n'
+     '        if (op.cmd_name().eq_icase("flushall") || op.cmd_name().eq_icase("flushdb"))\n'
+     '            tracking_broadcast_flush();\n        ClimonConn* state',
+     'src/cmd/climon.cc', 'no flush invalidation before owner work was published'),
+    ('flush-missing', 'flush', 'src/cmd/climon.cc',
+     'void IoLoop::climon_flush_completed(Op& op) {\n',
+     'void IoLoop::climon_flush_completed(Op& op) {\n    return; // omit completed invalidation\n',
+     'src/cmd/climon.cc', 'completed flush invalidates foreign tracking IO'),
+    ('output-untracked', 'output', 'src/net/wb.h',
+     'else conn.stop_obuf_tracking();',
+     'else {} // deliberately leave accounting armed for uncounted bytes',
+     'tests/netcmd_unit.cc', None),
+    ('output-deferred', 'output', 'src/core/io_loop.h',
+     'uint64_t used = c->obuf_bytes() + wb_.deferred_output_bytes(*c);',
+     'uint64_t used = c->obuf_bytes(); // deliberately omit deferred storage',
+     'tests/netcmd_unit.cc', 'hard limit fired before Done'),
+]
+results = []
+for name, case, source, before, after, tu, diagnostic in variants:
+    tree = out / 'mutants' / name
+    tree.mkdir(parents=True)
+    shutil.copytree(root / 'src', tree / 'src')
+    (tree / 'third_party').symlink_to(root / 'third_party', target_is_directory=True)
+    (tree / 'tests').mkdir()
+    for test in ('netcmd_unit.cc', 'netcmd_unit.h'):
+        shutil.copy2(root / 'tests' / test, tree / 'tests' / test)
+    target = tree / source
+    original = target.read_text()
+    assert original.count(before) == (2 if name == 'output-untracked' else 1)
+    target.write_text(original.replace(before, after))
+    old_object = 'build/tests/netcmd_unit.o' if tu.startswith('tests/') else 'build/src/cmd/climon.o'
+    compile_cmd = shlex.split(next(line for line in recipes if f' -c {tu} -o {old_object}' in line))
+    new_object = str(tree / 'mutant.o')
+    compile_cmd = [str(tree / tu) if arg == tu else new_object if arg == old_object
+                   else f'-I{tree}' if arg == '-I.' else arg for arg in compile_cmd]
+    binary = str(tree / 'netcmd-unit')
+    link_cmd = [new_object if arg == old_object else binary if arg == 'build/netcmd-unit' else arg
+                for arg in link]
+    print(f'building {name}', flush=True)
+    (tree / 'commands.json').write_text(json.dumps([compile_cmd, link_cmd], indent=2) + '\n')
+    with (tree / 'build.log').open('w') as log:
+        subprocess.run(['taskset', '-c', '44-55', *compile_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=300)
+        subprocess.run(['taskset', '-c', '44-55', *link_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=120)
+    for run in range(1, 6):
+        command = ['taskset', '-c', '44-51', 'timeout', '60', binary, case]
+        r = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+        (tree / f'run-{run}.log').write_text(r.stdout)
+        expected = r.returncode == -signal.SIGABRT if diagnostic is None else (
+            r.returncode == 1 and f'FAIL: {diagnostic}' in r.stdout)
+        results.append({'variant':name, 'case':case, 'run':run, 'returncode':r.returncode,
+                        'expected_failure':expected, 'output':r.stdout, 'command':command})
+        assert expected, (name, r.returncode, r.stdout)
+    print(f'{name}: rejected 5/5; {r.stdout.strip()}', flush=True)
+    (out / 'negative-results.json').write_text(json.dumps(results, indent=2) + '\n')
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
new file mode 100644
index 0000000000000000000000000000000000000000..bc72f719b1e93668f4c671000b1d5bad75da7ac0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
@@ -0,0 +1,16 @@
+#define _GNU_SOURCE
+#include <dlfcn.h>
+#include <stdio.h>
+#include <string.h>
+
+/* Read-only topology simulation: the process remains pinned to real CPUs 44-51.
+ * These serverless fixtures never launch workers. Only sibling metadata changes. */
+FILE *fopen(const char *path, const char *mode) {
+    static char pairs[2][7] = {"44-45\n", "46-47\n"};
+    int cpu = -1, end = 0;
+    if (sscanf(path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list%n",
+               &cpu, &end) == 1 && end && !path[end] && cpu >= 44 && cpu <= 47)
+        return fmemopen(pairs[(cpu - 44) / 2], 6, "r");
+    FILE *(*real_fopen)(const char *, const char *) = dlsym(RTLD_NEXT, "fopen");
+    return real_fopen(path, mode);
+}
diff --git a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
index 6d11e9ce5b2552d2f792115b3076459979edbe39..c04a11b7fc4a6c500632d4e2c48b7949c2dc8c6e
--- a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
+++ b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
@@ -60,7 +60,9 @@
 
     static void notify_retry() {
         Server server;
-        Config cfg; cfg.even_ifid = 1; cfg.even_ex = 1; cfg.shards = 16;
+        // The gate runs this serverless row without taskset. Automatic SMT placement requires
+        // whole sibling pairs for BOTH roles; only one IO is a tracking destination below.
+        Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "notification topology");
         server.threads_.resize(server.placement().total_threads());
         for (uint32_t i = 0; i < server.threads_.size(); ++i) {
@@ -94,7 +96,8 @@
     }
 
     static void flush() {
-        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 1; cfg.shards = 16;
+        // Two IOs still prove the foreign delivery; an even EX count also admits SMT placement.
+        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "flush topology");
         const auto& ios = server.placement().ifid_threads();
         check(ios.size() == 2, "foreign tracking IO exists");
@@ -136,7 +139,16 @@
         Server server; ThreadCtx self; IoLoop loop;
         loop.srv_ = &server; loop.self_ = &self;
         sender.bind(loop.wb_);
-        server.live_obuf_normal_hard_.store(8); server.client_obuf_armed_.store(true);
+        // Mirror Server::init's mailbox initialization without starting workers or opening rings.
+        // cx-waits made raw live-field stores writer-only; IO reads the published snapshot.
+        server.live_config_committed_ = server.capture_live_config(server.live_config_version_.load());
+        server.live_config_mailboxes_ = std::make_unique<LiveConfigMailbox[]>(server.nthreads() + 1);
+        server.live_config_mailboxes_[self.id()].init(server.live_config_committed_);
+        ClientOutputBufferLimits limits{}; limits.normal.hard_bytes = 8;
+        server.set_client_output_buffer_limits(limits);
+        check(server.client_obuf_armed() &&
+              server.client_limits_snapshot(self.id()).normal.hard_bytes == 8,
+              "hard limit published to the IO config mailbox");
         Client held(-1); held.set_id(1);
         check(held.rob().acquire(), "held ordinary operation"); held.rob().publish();
         check(!held.blocked(), "ordinary operation, not blocking exception");

exec
/bin/bash -lc "sed -n '2010,2028p' src/cmd/t_server.cc && sed -n '2049,2078p' src/cmd/t_server.cc && rg -n 'thread_mode:|read_local:|overlap:' src/cmd/t_server.cc && tail -4 build/netcmdfix/negative-progress.log" in /home/user/Projects/cx-final-netcmdfix
 succeeded in 0ms:
                g_server ? g_server->nshards() : 0u,
                g_server ? g_server->cfg().overlap : 0u,
                g_server ? g_server->cfg().reorder : 0u,
                g_server && g_server->read_local_enabled() ? 1u : 0u,
                g_server && g_server->atomic_enabled() ? 1u : 0u,
                sizeof(void*) * 8,
                g_ring_epoll_mode ? "epoll" : "io_uring",
                static_cast<long long>(::getpid()),
                static_cast<unsigned>(g_server ? g_server->cfg().port : 0),
                static_cast<unsigned long long>(uptime),
                static_cast<unsigned long long>(uptime / 86400));
        appendf(body, "key_lb:%u\r\nclient_lb:%u\r\nflip_auto:%u\r\n"
                      "flip_fingerprint_window:%u\r\nnet_io:%s\r\nhash:%s\r\nzc_min:%s\r\n"
                      "pin_threads:%u\r\n",
                g_server && g_server->key_lb_signals_enabled() ? 1u : 0u,
                g_server && g_server->client_lb_signals_enabled() ? 1u : 0u,
                g_server && g_server->flipctl_enabled() ? 1u : 0u,
                flip_fingerprint_window(g_server && g_server->flipctl_enabled()),
                g_ring_epoll_mode ? "epoll" : "uring",
        // Guard the CALL, including all argument evaluation. The cold non-inlined helper owns
        // its scratch buffers so the disabled INFO path keeps its old output without those arrays.
        if (g_server && g_server->read_local_enabled())
            append_read_local_thread_info(body, *g_server);
        if (g_server && g_server->mode_schedule_stats())
            append_mode_schedule_info(body, g_server->mode_schedule_stats(), g_server->nthreads());
        if (g_server && g_server->thread_mode() == ThreadMode::Fused) {
            appendf(body,
                    "fused_threads:%u\r\nclient_threads:%u\r\nowner_threads:%u\r\n"
                    "flip_available:0\r\nflip_unavailable_reason:threads_are_fused\r\n",
                    g_server->nthreads(), g_server->client_serving_thread_count(),
                    g_server->shard_owner_count());
        } else {
            const FlipReport flip = g_server ? g_server->flip_report() : FlipReport{};
            appendf(body,
                    "io_threads:%u\r\nex_threads:%u\r\nflip_target_io:%u\r\n"
                    "flip_target_ex:%u\r\n"
                    "flip_unit_threads:%u\r\nflip_bucket_min:%u\r\nflip_bucket_max:%u\r\n"
                    "flip_client_min:%u\r\nflip_client_max:%u\r\n"
                    "flip_last_transfers:%llu\r\nflip_in_progress:%u\r\n",
                    flip.live_io, flip.live_ex, flip.target_io, flip.target_ex,
                    flip.unit_threads, flip.bucket_min, flip.bucket_max,
                    flip.client_min, flip.client_max,
                    static_cast<unsigned long long>(flip.last_transfers),
                    flip.moving ? 1u : 0u);
        }
    }
    if (info_section(op, "FLIPCTL")) {
        if (g_server && !g_server->flipctl_available()) {
            appendf(body,
2006:                      "thread_mode:%s\r\nshards:%u\r\noverlap:%u\r\nreorder:%u\r\nread_local:%u\r\natomic:%u\r\n"
2080:                    "flipctl_available:0\r\nflipctl_thread_mode:1s\r\n"
flush-missing: rejected 5/5; FAIL: completed flush invalidates foreign tracking IO
building output-untracked
output-untracked: rejected 5/5; timeout: the monitored command dumped core
building output-deferred

apply patch
patch: completed
/home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
new file mode 100644
index 0000000000000000000000000000000000000000..d51dc359bcc502c1649e79d7647bb76ebaf443a0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
@@ -0,0 +1,81 @@
+"""Run only tests/netcmd.py; each attempt owns one server PID and private data directory."""
+import itertools
+import json
+from pathlib import Path
+import resource
+import socket
+import subprocess
+import sys
+import time
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix/live'
+out.mkdir()
+sys.path.insert(0, str(root / 'tests'))
+from _lib import Conn
+
+results = []
+cells = itertools.product(('split', 'fused'), (0, 1), (0, 1), (0, 1))
+for cell, (mode, atomic, read_local, overlap) in enumerate(cells):
+    port = 8580 + cell
+    name = f'{mode}-a{atomic}-rl{read_local}-overlap{overlap}'
+    for attempt in range(1, 4):
+        directory = out / name / str(attempt)
+        directory.mkdir(parents=True)
+        with socket.socket() as guard:
+            guard.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
+            guard.bind(('127.0.0.1', port))
+        command = ['taskset', '-c', '44-51', './build/tomokv', '--bind', '127.0.0.1',
+                   '--port', str(port), '--shards', '16', '--thread-mode', mode,
+                   '--atomic', str(atomic), '--read-local', str(read_local),
+                   '--overlap', str(overlap), '--enable-debug-command', 'yes',
+                   '--dir', str(directory)]
+        if mode == 'split':
+            command += ['--ratio', '6:2']
+        record = dict(cell=name, attempt=attempt, port=port, command=command)
+        with (directory / 'server.log').open('w') as log:
+            server = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
+            record['pid'] = server.pid
+            try:
+                deadline = time.monotonic() + 30
+                while True:
+                    assert server.poll() is None, f'{name}: server exited {server.returncode}'
+                    try:
+                        connection = Conn('127.0.0.1', port, timeout=2)
+                        break
+                    except OSError:
+                        assert time.monotonic() < deadline, f'{name}: boot timeout'
+                        time.sleep(0.05)
+                try:
+                    assert connection.cmd('PING') == b'PONG'
+                    info = connection.cmd('INFO', 'server').decode()
+                    (directory / 'info-server.log').write_text(info)
+                    fields = dict(line.split(':', 1) for line in info.splitlines() if ':' in line)
+                    assert fields['thread_mode'] == ('2s' if mode == 'split' else '1s'), fields
+                    assert fields['read_local'] == str(read_local), fields
+                finally:
+                    connection.close()
+                test = ['taskset', '-c', '52-55', 'timeout', '60', 'python3', '-u',
+                        'tests/netcmd.py', '127.0.0.1', str(port)]
+                result = subprocess.run(test, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+                (directory / 'battery.log').write_text(result.stdout)
+                record.update(returncode=result.returncode, test_command=test)
+            except Exception as exc:
+                record.update(returncode=1, error=repr(exc))
+            finally:
+                if server.poll() is None:
+                    server.terminate()
+                    try:
+                        server.wait(timeout=20)
+                    except subprocess.TimeoutExpired:
+                        server.kill()
+                        server.wait()
+                record['server_exit'] = server.returncode
+        results.append(record)
+        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
+        if record['returncode'] or record['server_exit']:
+            print(f'FAIL {name} attempt {attempt}: {record}', flush=True)
+    cell_results = [r for r in results if r['cell'] == name]
+    print(f'{name}: {sum(r["returncode"] == 0 and r["server_exit"] == 0 for r in cell_results)}/3', flush=True)
+raise SystemExit(any(r['returncode'] or r['server_exit'] for r in results))
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
new file mode 100644
index 0000000000000000000000000000000000000000..b6494711491bb3710f859aa5fb7e295a4e3b093b
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
@@ -0,0 +1,77 @@
+"""Disposable source copies only; never modifies the production sources or objects."""
+import json
+from pathlib import Path
+import resource
+import shlex
+import shutil
+import signal
+import subprocess
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix'
+recipes = subprocess.check_output(['make', '-n', '-B', 'build/netcmd-unit'], text=True).splitlines()
+link = shlex.split(next(line for line in recipes if ' -o build/netcmd-unit ' in line))
+variants = [
+    ('notify-drop', 'notify-retry', 'src/cmd/notify.inc',
+     'return work;  // keep this target and the keyless record for the next pass',
+     'slot->keyless.pop_front(); return work + 1;  // deliberately lose the invalidation',
+     'tests/netcmd_unit.cc', 'failed marker post is incomplete work'),
+    ('flush-early', 'flush', 'src/cmd/climon.cc',
+     'if (armed & Server::kClimonTracking) {\n        ClimonConn* state',
+     'if (armed & Server::kClimonTracking) {\n'
+     '        if (op.cmd_name().eq_icase("flushall") || op.cmd_name().eq_icase("flushdb"))\n'
+     '            tracking_broadcast_flush();\n        ClimonConn* state',
+     'src/cmd/climon.cc', 'no flush invalidation before owner work was published'),
+    ('flush-missing', 'flush', 'src/cmd/climon.cc',
+     'void IoLoop::climon_flush_completed(Op& op) {\n',
+     'void IoLoop::climon_flush_completed(Op& op) {\n    return; // omit completed invalidation\n',
+     'src/cmd/climon.cc', 'completed flush invalidates foreign tracking IO'),
+    ('output-untracked', 'output', 'src/net/wb.h',
+     'else conn.stop_obuf_tracking();',
+     'else {} // deliberately leave accounting armed for uncounted bytes',
+     'tests/netcmd_unit.cc', None),
+    ('output-deferred', 'output', 'src/core/io_loop.h',
+     'uint64_t used = c->obuf_bytes() + wb_.deferred_output_bytes(*c);',
+     'uint64_t used = c->obuf_bytes(); // deliberately omit deferred storage',
+     'tests/netcmd_unit.cc', 'hard limit fired before Done'),
+]
+results = []
+for name, case, source, before, after, tu, diagnostic in variants:
+    tree = out / 'mutants' / name
+    tree.mkdir(parents=True)
+    shutil.copytree(root / 'src', tree / 'src')
+    (tree / 'third_party').symlink_to(root / 'third_party', target_is_directory=True)
+    (tree / 'tests').mkdir()
+    for test in ('netcmd_unit.cc', 'netcmd_unit.h'):
+        shutil.copy2(root / 'tests' / test, tree / 'tests' / test)
+    target = tree / source
+    original = target.read_text()
+    assert original.count(before) == (2 if name == 'output-untracked' else 1)
+    target.write_text(original.replace(before, after))
+    old_object = 'build/tests/netcmd_unit.o' if tu.startswith('tests/') else 'build/src/cmd/climon.o'
+    compile_cmd = shlex.split(next(line for line in recipes if f' -c {tu} -o {old_object}' in line))
+    new_object = str(tree / 'mutant.o')
+    compile_cmd = [str(tree / tu) if arg == tu else new_object if arg == old_object
+                   else f'-I{tree}' if arg == '-I.' else arg for arg in compile_cmd]
+    binary = str(tree / 'netcmd-unit')
+    link_cmd = [new_object if arg == old_object else binary if arg == 'build/netcmd-unit' else arg
+                for arg in link]
+    print(f'building {name}', flush=True)
+    (tree / 'commands.json').write_text(json.dumps([compile_cmd, link_cmd], indent=2) + '\n')
+    with (tree / 'build.log').open('w') as log:
+        subprocess.run(['taskset', '-c', '44-55', *compile_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=300)
+        subprocess.run(['taskset', '-c', '44-55', *link_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=120)
+    for run in range(1, 6):
+        command = ['taskset', '-c', '44-51', 'timeout', '60', binary, case]
+        r = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+        (tree / f'run-{run}.log').write_text(r.stdout)
+        expected = r.returncode == -signal.SIGABRT if diagnostic is None else (
+            r.returncode == 1 and f'FAIL: {diagnostic}' in r.stdout)
+        results.append({'variant':name, 'case':case, 'run':run, 'returncode':r.returncode,
+                        'expected_failure':expected, 'output':r.stdout, 'command':command})
+        assert expected, (name, r.returncode, r.stdout)
+    print(f'{name}: rejected 5/5; {r.stdout.strip()}', flush=True)
+    (out / 'negative-results.json').write_text(json.dumps(results, indent=2) + '\n')
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
new file mode 100644
index 0000000000000000000000000000000000000000..bc72f719b1e93668f4c671000b1d5bad75da7ac0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
@@ -0,0 +1,16 @@
+#define _GNU_SOURCE
+#include <dlfcn.h>
+#include <stdio.h>
+#include <string.h>
+
+/* Read-only topology simulation: the process remains pinned to real CPUs 44-51.
+ * These serverless fixtures never launch workers. Only sibling metadata changes. */
+FILE *fopen(const char *path, const char *mode) {
+    static char pairs[2][7] = {"44-45\n", "46-47\n"};
+    int cpu = -1, end = 0;
+    if (sscanf(path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list%n",
+               &cpu, &end) == 1 && end && !path[end] && cpu >= 44 && cpu <= 47)
+        return fmemopen(pairs[(cpu - 44) / 2], 6, "r");
+    FILE *(*real_fopen)(const char *, const char *) = dlsym(RTLD_NEXT, "fopen");
+    return real_fopen(path, mode);
+}
diff --git a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
index 6d11e9ce5b2552d2f792115b3076459979edbe39..c04a11b7fc4a6c500632d4e2c48b7949c2dc8c6e
--- a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
+++ b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
@@ -60,7 +60,9 @@
 
     static void notify_retry() {
         Server server;
-        Config cfg; cfg.even_ifid = 1; cfg.even_ex = 1; cfg.shards = 16;
+        // The gate runs this serverless row without taskset. Automatic SMT placement requires
+        // whole sibling pairs for BOTH roles; only one IO is a tracking destination below.
+        Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "notification topology");
         server.threads_.resize(server.placement().total_threads());
         for (uint32_t i = 0; i < server.threads_.size(); ++i) {
@@ -94,7 +96,8 @@
     }
 
     static void flush() {
-        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 1; cfg.shards = 16;
+        // Two IOs still prove the foreign delivery; an even EX count also admits SMT placement.
+        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "flush topology");
         const auto& ios = server.placement().ifid_threads();
         check(ios.size() == 2, "foreign tracking IO exists");
@@ -136,7 +139,16 @@
         Server server; ThreadCtx self; IoLoop loop;
         loop.srv_ = &server; loop.self_ = &self;
         sender.bind(loop.wb_);
-        server.live_obuf_normal_hard_.store(8); server.client_obuf_armed_.store(true);
+        // Mirror Server::init's mailbox initialization without starting workers or opening rings.
+        // cx-waits made raw live-field stores writer-only; IO reads the published snapshot.
+        server.live_config_committed_ = server.capture_live_config(server.live_config_version_.load());
+        server.live_config_mailboxes_ = std::make_unique<LiveConfigMailbox[]>(server.nthreads() + 1);
+        server.live_config_mailboxes_[self.id()].init(server.live_config_committed_);
+        ClientOutputBufferLimits limits{}; limits.normal.hard_bytes = 8;
+        server.set_client_output_buffer_limits(limits);
+        check(server.client_obuf_armed() &&
+              server.client_limits_snapshot(self.id()).normal.hard_bytes == 8,
+              "hard limit published to the IO config mailbox");
         Client held(-1); held.set_id(1);
         check(held.rob().acquire(), "held ordinary operation"); held.rob().publish();
         check(!held.blocked(), "ordinary operation, not blocking exception");

diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
new file mode 100644
index 0000000000000000000000000000000000000000..d51dc359bcc502c1649e79d7647bb76ebaf443a0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
@@ -0,0 +1,81 @@
+"""Run only tests/netcmd.py; each attempt owns one server PID and private data directory."""
+import itertools
+import json
+from pathlib import Path
+import resource
+import socket
+import subprocess
+import sys
+import time
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix/live'
+out.mkdir()
+sys.path.insert(0, str(root / 'tests'))
+from _lib import Conn
+
+results = []
+cells = itertools.product(('split', 'fused'), (0, 1), (0, 1), (0, 1))
+for cell, (mode, atomic, read_local, overlap) in enumerate(cells):
+    port = 8580 + cell
+    name = f'{mode}-a{atomic}-rl{read_local}-overlap{overlap}'
+    for attempt in range(1, 4):
+        directory = out / name / str(attempt)
+        directory.mkdir(parents=True)
+        with socket.socket() as guard:
+            guard.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
+            guard.bind(('127.0.0.1', port))
+        command = ['taskset', '-c', '44-51', './build/tomokv', '--bind', '127.0.0.1',
+                   '--port', str(port), '--shards', '16', '--thread-mode', mode,
+                   '--atomic', str(atomic), '--read-local', str(read_local),
+                   '--overlap', str(overlap), '--enable-debug-command', 'yes',
+                   '--dir', str(directory)]
+        if mode == 'split':
+            command += ['--ratio', '6:2']
+        record = dict(cell=name, attempt=attempt, port=port, command=command)
+        with (directory / 'server.log').open('w') as log:
+            server = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
+            record['pid'] = server.pid
+            try:
+                deadline = time.monotonic() + 30
+                while True:
+                    assert server.poll() is None, f'{name}: server exited {server.returncode}'
+                    try:
+                        connection = Conn('127.0.0.1', port, timeout=2)
+                        break
+                    except OSError:
+                        assert time.monotonic() < deadline, f'{name}: boot timeout'
+                        time.sleep(0.05)
+                try:
+                    assert connection.cmd('PING') == b'PONG'
+                    info = connection.cmd('INFO', 'server').decode()
+                    (directory / 'info-server.log').write_text(info)
+                    fields = dict(line.split(':', 1) for line in info.splitlines() if ':' in line)
+                    assert fields['thread_mode'] == ('2s' if mode == 'split' else '1s'), fields
+                    assert fields['read_local'] == str(read_local), fields
+                finally:
+                    connection.close()
+                test = ['taskset', '-c', '52-55', 'timeout', '60', 'python3', '-u',
+                        'tests/netcmd.py', '127.0.0.1', str(port)]
+                result = subprocess.run(test, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+                (directory / 'battery.log').write_text(result.stdout)
+                record.update(returncode=result.returncode, test_command=test)
+            except Exception as exc:
+                record.update(returncode=1, error=repr(exc))
+            finally:
+                if server.poll() is None:
+                    server.terminate()
+                    try:
+                        server.wait(timeout=20)
+                    except subprocess.TimeoutExpired:
+                        server.kill()
+                        server.wait()
+                record['server_exit'] = server.returncode
+        results.append(record)
+        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
+        if record['returncode'] or record['server_exit']:
+            print(f'FAIL {name} attempt {attempt}: {record}', flush=True)
+    cell_results = [r for r in results if r['cell'] == name]
+    print(f'{name}: {sum(r["returncode"] == 0 and r["server_exit"] == 0 for r in cell_results)}/3', flush=True)
+raise SystemExit(any(r['returncode'] or r['server_exit'] for r in results))
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
new file mode 100644
index 0000000000000000000000000000000000000000..b6494711491bb3710f859aa5fb7e295a4e3b093b
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
@@ -0,0 +1,77 @@
+"""Disposable source copies only; never modifies the production sources or objects."""
+import json
+from pathlib import Path
+import resource
+import shlex
+import shutil
+import signal
+import subprocess
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix'
+recipes = subprocess.check_output(['make', '-n', '-B', 'build/netcmd-unit'], text=True).splitlines()
+link = shlex.split(next(line for line in recipes if ' -o build/netcmd-unit ' in line))
+variants = [
+    ('notify-drop', 'notify-retry', 'src/cmd/notify.inc',
+     'return work;  // keep this target and the keyless record for the next pass',
+     'slot->keyless.pop_front(); return work + 1;  // deliberately lose the invalidation',
+     'tests/netcmd_unit.cc', 'failed marker post is incomplete work'),
+    ('flush-early', 'flush', 'src/cmd/climon.cc',
+     'if (armed & Server::kClimonTracking) {\n        ClimonConn* state',
+     'if (armed & Server::kClimonTracking) {\n'
+     '        if (op.cmd_name().eq_icase("flushall") || op.cmd_name().eq_icase("flushdb"))\n'
+     '            tracking_broadcast_flush();\n        ClimonConn* state',
+     'src/cmd/climon.cc', 'no flush invalidation before owner work was published'),
+    ('flush-missing', 'flush', 'src/cmd/climon.cc',
+     'void IoLoop::climon_flush_completed(Op& op) {\n',
+     'void IoLoop::climon_flush_completed(Op& op) {\n    return; // omit completed invalidation\n',
+     'src/cmd/climon.cc', 'completed flush invalidates foreign tracking IO'),
+    ('output-untracked', 'output', 'src/net/wb.h',
+     'else conn.stop_obuf_tracking();',
+     'else {} // deliberately leave accounting armed for uncounted bytes',
+     'tests/netcmd_unit.cc', None),
+    ('output-deferred', 'output', 'src/core/io_loop.h',
+     'uint64_t used = c->obuf_bytes() + wb_.deferred_output_bytes(*c);',
+     'uint64_t used = c->obuf_bytes(); // deliberately omit deferred storage',
+     'tests/netcmd_unit.cc', 'hard limit fired before Done'),
+]
+results = []
+for name, case, source, before, after, tu, diagnostic in variants:
+    tree = out / 'mutants' / name
+    tree.mkdir(parents=True)
+    shutil.copytree(root / 'src', tree / 'src')
+    (tree / 'third_party').symlink_to(root / 'third_party', target_is_directory=True)
+    (tree / 'tests').mkdir()
+    for test in ('netcmd_unit.cc', 'netcmd_unit.h'):
+        shutil.copy2(root / 'tests' / test, tree / 'tests' / test)
+    target = tree / source
+    original = target.read_text()
+    assert original.count(before) == (2 if name == 'output-untracked' else 1)
+    target.write_text(original.replace(before, after))
+    old_object = 'build/tests/netcmd_unit.o' if tu.startswith('tests/') else 'build/src/cmd/climon.o'
+    compile_cmd = shlex.split(next(line for line in recipes if f' -c {tu} -o {old_object}' in line))
+    new_object = str(tree / 'mutant.o')
+    compile_cmd = [str(tree / tu) if arg == tu else new_object if arg == old_object
+                   else f'-I{tree}' if arg == '-I.' else arg for arg in compile_cmd]
+    binary = str(tree / 'netcmd-unit')
+    link_cmd = [new_object if arg == old_object else binary if arg == 'build/netcmd-unit' else arg
+                for arg in link]
+    print(f'building {name}', flush=True)
+    (tree / 'commands.json').write_text(json.dumps([compile_cmd, link_cmd], indent=2) + '\n')
+    with (tree / 'build.log').open('w') as log:
+        subprocess.run(['taskset', '-c', '44-55', *compile_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=300)
+        subprocess.run(['taskset', '-c', '44-55', *link_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=120)
+    for run in range(1, 6):
+        command = ['taskset', '-c', '44-51', 'timeout', '60', binary, case]
+        r = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+        (tree / f'run-{run}.log').write_text(r.stdout)
+        expected = r.returncode == -signal.SIGABRT if diagnostic is None else (
+            r.returncode == 1 and f'FAIL: {diagnostic}' in r.stdout)
+        results.append({'variant':name, 'case':case, 'run':run, 'returncode':r.returncode,
+                        'expected_failure':expected, 'output':r.stdout, 'command':command})
+        assert expected, (name, r.returncode, r.stdout)
+    print(f'{name}: rejected 5/5; {r.stdout.strip()}', flush=True)
+    (out / 'negative-results.json').write_text(json.dumps(results, indent=2) + '\n')
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
new file mode 100644
index 0000000000000000000000000000000000000000..bc72f719b1e93668f4c671000b1d5bad75da7ac0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
@@ -0,0 +1,16 @@
+#define _GNU_SOURCE
+#include <dlfcn.h>
+#include <stdio.h>
+#include <string.h>
+
+/* Read-only topology simulation: the process remains pinned to real CPUs 44-51.
+ * These serverless fixtures never launch workers. Only sibling metadata changes. */
+FILE *fopen(const char *path, const char *mode) {
+    static char pairs[2][7] = {"44-45\n", "46-47\n"};
+    int cpu = -1, end = 0;
+    if (sscanf(path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list%n",
+               &cpu, &end) == 1 && end && !path[end] && cpu >= 44 && cpu <= 47)
+        return fmemopen(pairs[(cpu - 44) / 2], 6, "r");
+    FILE *(*real_fopen)(const char *, const char *) = dlsym(RTLD_NEXT, "fopen");
+    return real_fopen(path, mode);
+}
diff --git a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
index 6d11e9ce5b2552d2f792115b3076459979edbe39..c04a11b7fc4a6c500632d4e2c48b7949c2dc8c6e
--- a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
+++ b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
@@ -60,7 +60,9 @@
 
     static void notify_retry() {
         Server server;
-        Config cfg; cfg.even_ifid = 1; cfg.even_ex = 1; cfg.shards = 16;
+        // The gate runs this serverless row without taskset. Automatic SMT placement requires
+        // whole sibling pairs for BOTH roles; only one IO is a tracking destination below.
+        Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "notification topology");
         server.threads_.resize(server.placement().total_threads());
         for (uint32_t i = 0; i < server.threads_.size(); ++i) {
@@ -94,7 +96,8 @@
     }
 
     static void flush() {
-        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 1; cfg.shards = 16;
+        // Two IOs still prove the foreign delivery; an even EX count also admits SMT placement.
+        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "flush topology");
         const auto& ios = server.placement().ifid_threads();
         check(ios.size() == 2, "foreign tracking IO exists");
@@ -136,7 +139,16 @@
         Server server; ThreadCtx self; IoLoop loop;
         loop.srv_ = &server; loop.self_ = &self;
         sender.bind(loop.wb_);
-        server.live_obuf_normal_hard_.store(8); server.client_obuf_armed_.store(true);
+        // Mirror Server::init's mailbox initialization without starting workers or opening rings.
+        // cx-waits made raw live-field stores writer-only; IO reads the published snapshot.
+        server.live_config_committed_ = server.capture_live_config(server.live_config_version_.load());
+        server.live_config_mailboxes_ = std::make_unique<LiveConfigMailbox[]>(server.nthreads() + 1);
+        server.live_config_mailboxes_[self.id()].init(server.live_config_committed_);
+        ClientOutputBufferLimits limits{}; limits.normal.hard_bytes = 8;
+        server.set_client_output_buffer_limits(limits);
+        check(server.client_obuf_armed() &&
+              server.client_limits_snapshot(self.id()).normal.hard_bytes == 8,
+              "hard limit published to the IO config mailbox");
         Client held(-1); held.set_id(1);
         check(held.rob().acquire(), "held ordinary operation"); held.rob().publish();
         check(!held.blocked(), "ordinary operation, not blocking exception");

exec
/bin/bash -lc 'tail -6 build/netcmdfix/negative-progress.log && git diff --check && sha256sum build/tomokv build/netcmd-unit build/netcmdfix/netcmd-unit-pre > build/netcmdfix/binary-sha256.txt' in /home/user/Projects/cx-final-netcmdfix
 succeeded in 21ms:
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
new file mode 100644
index 0000000000000000000000000000000000000000..d51dc359bcc502c1649e79d7647bb76ebaf443a0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
@@ -0,0 +1,81 @@
+"""Run only tests/netcmd.py; each attempt owns one server PID and private data directory."""
+import itertools
+import json
+from pathlib import Path
+import resource
+import socket
+import subprocess
+import sys
+import time
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix/live'
+out.mkdir()
+sys.path.insert(0, str(root / 'tests'))
+from _lib import Conn
+
+results = []
+cells = itertools.product(('split', 'fused'), (0, 1), (0, 1), (0, 1))
+for cell, (mode, atomic, read_local, overlap) in enumerate(cells):
+    port = 8580 + cell
+    name = f'{mode}-a{atomic}-rl{read_local}-overlap{overlap}'
+    for attempt in range(1, 4):
+        directory = out / name / str(attempt)
+        directory.mkdir(parents=True)
+        with socket.socket() as guard:
+            guard.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
+            guard.bind(('127.0.0.1', port))
+        command = ['taskset', '-c', '44-51', './build/tomokv', '--bind', '127.0.0.1',
+                   '--port', str(port), '--shards', '16', '--thread-mode', mode,
+                   '--atomic', str(atomic), '--read-local', str(read_local),
+                   '--overlap', str(overlap), '--enable-debug-command', 'yes',
+                   '--dir', str(directory)]
+        if mode == 'split':
+            command += ['--ratio', '6:2']
+        record = dict(cell=name, attempt=attempt, port=port, command=command)
+        with (directory / 'server.log').open('w') as log:
+            server = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
+            record['pid'] = server.pid
+            try:
+                deadline = time.monotonic() + 30
+                while True:
+                    assert server.poll() is None, f'{name}: server exited {server.returncode}'
+                    try:
+                        connection = Conn('127.0.0.1', port, timeout=2)
+                        break
+                    except OSError:
+                        assert time.monotonic() < deadline, f'{name}: boot timeout'
+                        time.sleep(0.05)
+                try:
+                    assert connection.cmd('PING') == b'PONG'
+                    info = connection.cmd('INFO', 'server').decode()
+                    (directory / 'info-server.log').write_text(info)
+                    fields = dict(line.split(':', 1) for line in info.splitlines() if ':' in line)
+                    assert fields['thread_mode'] == ('2s' if mode == 'split' else '1s'), fields
+                    assert fields['read_local'] == str(read_local), fields
+                finally:
+                    connection.close()
+                test = ['taskset', '-c', '52-55', 'timeout', '60', 'python3', '-u',
+                        'tests/netcmd.py', '127.0.0.1', str(port)]
+                result = subprocess.run(test, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+                (directory / 'battery.log').write_text(result.stdout)
+                record.update(returncode=result.returncode, test_command=test)
+            except Exception as exc:
+                record.update(returncode=1, error=repr(exc))
+            finally:
+                if server.poll() is None:
+                    server.terminate()
+                    try:
+                        server.wait(timeout=20)
+                    except subprocess.TimeoutExpired:
+                        server.kill()
+                        server.wait()
+                record['server_exit'] = server.returncode
+        results.append(record)
+        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
+        if record['returncode'] or record['server_exit']:
+            print(f'FAIL {name} attempt {attempt}: {record}', flush=True)
+    cell_results = [r for r in results if r['cell'] == name]
+    print(f'{name}: {sum(r["returncode"] == 0 and r["server_exit"] == 0 for r in cell_results)}/3', flush=True)
+raise SystemExit(any(r['returncode'] or r['server_exit'] for r in results))
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
new file mode 100644
index 0000000000000000000000000000000000000000..b6494711491bb3710f859aa5fb7e295a4e3b093b
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
@@ -0,0 +1,77 @@
+"""Disposable source copies only; never modifies the production sources or objects."""
+import json
+from pathlib import Path
+import resource
+import shlex
+import shutil
+import signal
+import subprocess
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix'
+recipes = subprocess.check_output(['make', '-n', '-B', 'build/netcmd-unit'], text=True).splitlines()
+link = shlex.split(next(line for line in recipes if ' -o build/netcmd-unit ' in line))
+variants = [
+    ('notify-drop', 'notify-retry', 'src/cmd/notify.inc',
+     'return work;  // keep this target and the keyless record for the next pass',
+     'slot->keyless.pop_front(); return work + 1;  // deliberately lose the invalidation',
+     'tests/netcmd_unit.cc', 'failed marker post is incomplete work'),
+    ('flush-early', 'flush', 'src/cmd/climon.cc',
+     'if (armed & Server::kClimonTracking) {\n        ClimonConn* state',
+     'if (armed & Server::kClimonTracking) {\n'
+     '        if (op.cmd_name().eq_icase("flushall") || op.cmd_name().eq_icase("flushdb"))\n'
+     '            tracking_broadcast_flush();\n        ClimonConn* state',
+     'src/cmd/climon.cc', 'no flush invalidation before owner work was published'),
+    ('flush-missing', 'flush', 'src/cmd/climon.cc',
+     'void IoLoop::climon_flush_completed(Op& op) {\n',
+     'void IoLoop::climon_flush_completed(Op& op) {\n    return; // omit completed invalidation\n',
+     'src/cmd/climon.cc', 'completed flush invalidates foreign tracking IO'),
+    ('output-untracked', 'output', 'src/net/wb.h',
+     'else conn.stop_obuf_tracking();',
+     'else {} // deliberately leave accounting armed for uncounted bytes',
+     'tests/netcmd_unit.cc', None),
+    ('output-deferred', 'output', 'src/core/io_loop.h',
+     'uint64_t used = c->obuf_bytes() + wb_.deferred_output_bytes(*c);',
+     'uint64_t used = c->obuf_bytes(); // deliberately omit deferred storage',
+     'tests/netcmd_unit.cc', 'hard limit fired before Done'),
+]
+results = []
+for name, case, source, before, after, tu, diagnostic in variants:
+    tree = out / 'mutants' / name
+    tree.mkdir(parents=True)
+    shutil.copytree(root / 'src', tree / 'src')
+    (tree / 'third_party').symlink_to(root / 'third_party', target_is_directory=True)
+    (tree / 'tests').mkdir()
+    for test in ('netcmd_unit.cc', 'netcmd_unit.h'):
+        shutil.copy2(root / 'tests' / test, tree / 'tests' / test)
+    target = tree / source
+    original = target.read_text()
+    assert original.count(before) == (2 if name == 'output-untracked' else 1)
+    target.write_text(original.replace(before, after))
+    old_object = 'build/tests/netcmd_unit.o' if tu.startswith('tests/') else 'build/src/cmd/climon.o'
+    compile_cmd = shlex.split(next(line for line in recipes if f' -c {tu} -o {old_object}' in line))
+    new_object = str(tree / 'mutant.o')
+    compile_cmd = [str(tree / tu) if arg == tu else new_object if arg == old_object
+                   else f'-I{tree}' if arg == '-I.' else arg for arg in compile_cmd]
+    binary = str(tree / 'netcmd-unit')
+    link_cmd = [new_object if arg == old_object else binary if arg == 'build/netcmd-unit' else arg
+                for arg in link]
+    print(f'building {name}', flush=True)
+    (tree / 'commands.json').write_text(json.dumps([compile_cmd, link_cmd], indent=2) + '\n')
+    with (tree / 'build.log').open('w') as log:
+        subprocess.run(['taskset', '-c', '44-55', *compile_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=300)
+        subprocess.run(['taskset', '-c', '44-55', *link_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=120)
+    for run in range(1, 6):
+        command = ['taskset', '-c', '44-51', 'timeout', '60', binary, case]
+        r = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+        (tree / f'run-{run}.log').write_text(r.stdout)
+        expected = r.returncode == -signal.SIGABRT if diagnostic is None else (
+            r.returncode == 1 and f'FAIL: {diagnostic}' in r.stdout)
+        results.append({'variant':name, 'case':case, 'run':run, 'returncode':r.returncode,
+                        'expected_failure':expected, 'output':r.stdout, 'command':command})
+        assert expected, (name, r.returncode, r.stdout)
+    print(f'{name}: rejected 5/5; {r.stdout.strip()}', flush=True)
+    (out / 'negative-results.json').write_text(json.dumps(results, indent=2) + '\n')
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
new file mode 100644
index 0000000000000000000000000000000000000000..bc72f719b1e93668f4c671000b1d5bad75da7ac0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
@@ -0,0 +1,16 @@
+#define _GNU_SOURCE
+#include <dlfcn.h>
+#include <stdio.h>
+#include <string.h>
+
+/* Read-only topology simulation: the process remains pinned to real CPUs 44-51.
+ * These serverless fixtures never launch workers. Only sibling metadata changes. */
+FILE *fopen(const char *path, const char *mode) {
+    static char pairs[2][7] = {"44-45\n", "46-47\n"};
+    int cpu = -1, end = 0;
+    if (sscanf(path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list%n",
+               &cpu, &end) == 1 && end && !path[end] && cpu >= 44 && cpu <= 47)
+        return fmemopen(pairs[(cpu - 44) / 2], 6, "r");
+    FILE *(*real_fopen)(const char *, const char *) = dlsym(RTLD_NEXT, "fopen");
+    return real_fopen(path, mode);
+}
diff --git a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
index 6d11e9ce5b2552d2f792115b3076459979edbe39..c04a11b7fc4a6c500632d4e2c48b7949c2dc8c6e
--- a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
+++ b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
@@ -60,7 +60,9 @@
 
     static void notify_retry() {
         Server server;
-        Config cfg; cfg.even_ifid = 1; cfg.even_ex = 1; cfg.shards = 16;
+        // The gate runs this serverless row without taskset. Automatic SMT placement requires
+        // whole sibling pairs for BOTH roles; only one IO is a tracking destination below.
+        Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "notification topology");
         server.threads_.resize(server.placement().total_threads());
         for (uint32_t i = 0; i < server.threads_.size(); ++i) {
@@ -94,7 +96,8 @@
     }
 
     static void flush() {
-        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 1; cfg.shards = 16;
+        // Two IOs still prove the foreign delivery; an even EX count also admits SMT placement.
+        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "flush topology");
         const auto& ios = server.placement().ifid_threads();
         check(ios.size() == 2, "foreign tracking IO exists");
@@ -136,7 +139,16 @@
         Server server; ThreadCtx self; IoLoop loop;
         loop.srv_ = &server; loop.self_ = &self;
         sender.bind(loop.wb_);
-        server.live_obuf_normal_hard_.store(8); server.client_obuf_armed_.store(true);
+        // Mirror Server::init's mailbox initialization without starting workers or opening rings.
+        // cx-waits made raw live-field stores writer-only; IO reads the published snapshot.
+        server.live_config_committed_ = server.capture_live_config(server.live_config_version_.load());
+        server.live_config_mailboxes_ = std::make_unique<LiveConfigMailbox[]>(server.nthreads() + 1);
+        server.live_config_mailboxes_[self.id()].init(server.live_config_committed_);
+        ClientOutputBufferLimits limits{}; limits.normal.hard_bytes = 8;
+        server.set_client_output_buffer_limits(limits);
+        check(server.client_obuf_armed() &&
+              server.client_limits_snapshot(self.id()).normal.hard_bytes == 8,
+              "hard limit published to the IO config mailbox");
         Client held(-1); held.set_id(1);
         check(held.rob().acquire(), "held ordinary operation"); held.rob().publish();
         check(!held.blocked(), "ordinary operation, not blocking exception");

 succeeded in 121110ms:
codex
Four negative controls have failed as expected: dropped invalidations, premature flush invalidations, missing flush invalidations, and stale output accounting. The fifth checks omitted deferred output bytes. Attribution is precise: the topology assumptions were already invalid when the tests were introduced; `cx-waits` caused the output fixture mismatch.

exec
/bin/bash -lc 'tail -3 build/netcmdfix/negative-progress.log' in /home/user/Projects/cx-final-netcmdfix
 succeeded in 0ms:
output-untracked: rejected 5/5; timeout: the monitored command dumped core
building output-deferred
output-deferred: rejected 5/5; FAIL: hard limit fired before Done

diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
new file mode 100644
index 0000000000000000000000000000000000000000..d51dc359bcc502c1649e79d7647bb76ebaf443a0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
@@ -0,0 +1,81 @@
+"""Run only tests/netcmd.py; each attempt owns one server PID and private data directory."""
+import itertools
+import json
+from pathlib import Path
+import resource
+import socket
+import subprocess
+import sys
+import time
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix/live'
+out.mkdir()
+sys.path.insert(0, str(root / 'tests'))
+from _lib import Conn
+
+results = []
+cells = itertools.product(('split', 'fused'), (0, 1), (0, 1), (0, 1))
+for cell, (mode, atomic, read_local, overlap) in enumerate(cells):
+    port = 8580 + cell
+    name = f'{mode}-a{atomic}-rl{read_local}-overlap{overlap}'
+    for attempt in range(1, 4):
+        directory = out / name / str(attempt)
+        directory.mkdir(parents=True)
+        with socket.socket() as guard:
+            guard.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
+            guard.bind(('127.0.0.1', port))
+        command = ['taskset', '-c', '44-51', './build/tomokv', '--bind', '127.0.0.1',
+                   '--port', str(port), '--shards', '16', '--thread-mode', mode,
+                   '--atomic', str(atomic), '--read-local', str(read_local),
+                   '--overlap', str(overlap), '--enable-debug-command', 'yes',
+                   '--dir', str(directory)]
+        if mode == 'split':
+            command += ['--ratio', '6:2']
+        record = dict(cell=name, attempt=attempt, port=port, command=command)
+        with (directory / 'server.log').open('w') as log:
+            server = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
+            record['pid'] = server.pid
+            try:
+                deadline = time.monotonic() + 30
+                while True:
+                    assert server.poll() is None, f'{name}: server exited {server.returncode}'
+                    try:
+                        connection = Conn('127.0.0.1', port, timeout=2)
+                        break
+                    except OSError:
+                        assert time.monotonic() < deadline, f'{name}: boot timeout'
+                        time.sleep(0.05)
+                try:
+                    assert connection.cmd('PING') == b'PONG'
+                    info = connection.cmd('INFO', 'server').decode()
+                    (directory / 'info-server.log').write_text(info)
+                    fields = dict(line.split(':', 1) for line in info.splitlines() if ':' in line)
+                    assert fields['thread_mode'] == ('2s' if mode == 'split' else '1s'), fields
+                    assert fields['read_local'] == str(read_local), fields
+                finally:
+                    connection.close()
+                test = ['taskset', '-c', '52-55', 'timeout', '60', 'python3', '-u',
+                        'tests/netcmd.py', '127.0.0.1', str(port)]
+                result = subprocess.run(test, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+                (directory / 'battery.log').write_text(result.stdout)
+                record.update(returncode=result.returncode, test_command=test)
+            except Exception as exc:
+                record.update(returncode=1, error=repr(exc))
+            finally:
+                if server.poll() is None:
+                    server.terminate()
+                    try:
+                        server.wait(timeout=20)
+                    except subprocess.TimeoutExpired:
+                        server.kill()
+                        server.wait()
+                record['server_exit'] = server.returncode
+        results.append(record)
+        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
+        if record['returncode'] or record['server_exit']:
+            print(f'FAIL {name} attempt {attempt}: {record}', flush=True)
+    cell_results = [r for r in results if r['cell'] == name]
+    print(f'{name}: {sum(r["returncode"] == 0 and r["server_exit"] == 0 for r in cell_results)}/3', flush=True)
+raise SystemExit(any(r['returncode'] or r['server_exit'] for r in results))
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
new file mode 100644
index 0000000000000000000000000000000000000000..b6494711491bb3710f859aa5fb7e295a4e3b093b
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
@@ -0,0 +1,77 @@
+"""Disposable source copies only; never modifies the production sources or objects."""
+import json
+from pathlib import Path
+import resource
+import shlex
+import shutil
+import signal
+import subprocess
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix'
+recipes = subprocess.check_output(['make', '-n', '-B', 'build/netcmd-unit'], text=True).splitlines()
+link = shlex.split(next(line for line in recipes if ' -o build/netcmd-unit ' in line))
+variants = [
+    ('notify-drop', 'notify-retry', 'src/cmd/notify.inc',
+     'return work;  // keep this target and the keyless record for the next pass',
+     'slot->keyless.pop_front(); return work + 1;  // deliberately lose the invalidation',
+     'tests/netcmd_unit.cc', 'failed marker post is incomplete work'),
+    ('flush-early', 'flush', 'src/cmd/climon.cc',
+     'if (armed & Server::kClimonTracking) {\n        ClimonConn* state',
+     'if (armed & Server::kClimonTracking) {\n'
+     '        if (op.cmd_name().eq_icase("flushall") || op.cmd_name().eq_icase("flushdb"))\n'
+     '            tracking_broadcast_flush();\n        ClimonConn* state',
+     'src/cmd/climon.cc', 'no flush invalidation before owner work was published'),
+    ('flush-missing', 'flush', 'src/cmd/climon.cc',
+     'void IoLoop::climon_flush_completed(Op& op) {\n',
+     'void IoLoop::climon_flush_completed(Op& op) {\n    return; // omit completed invalidation\n',
+     'src/cmd/climon.cc', 'completed flush invalidates foreign tracking IO'),
+    ('output-untracked', 'output', 'src/net/wb.h',
+     'else conn.stop_obuf_tracking();',
+     'else {} // deliberately leave accounting armed for uncounted bytes',
+     'tests/netcmd_unit.cc', None),
+    ('output-deferred', 'output', 'src/core/io_loop.h',
+     'uint64_t used = c->obuf_bytes() + wb_.deferred_output_bytes(*c);',
+     'uint64_t used = c->obuf_bytes(); // deliberately omit deferred storage',
+     'tests/netcmd_unit.cc', 'hard limit fired before Done'),
+]
+results = []
+for name, case, source, before, after, tu, diagnostic in variants:
+    tree = out / 'mutants' / name
+    tree.mkdir(parents=True)
+    shutil.copytree(root / 'src', tree / 'src')
+    (tree / 'third_party').symlink_to(root / 'third_party', target_is_directory=True)
+    (tree / 'tests').mkdir()
+    for test in ('netcmd_unit.cc', 'netcmd_unit.h'):
+        shutil.copy2(root / 'tests' / test, tree / 'tests' / test)
+    target = tree / source
+    original = target.read_text()
+    assert original.count(before) == (2 if name == 'output-untracked' else 1)
+    target.write_text(original.replace(before, after))
+    old_object = 'build/tests/netcmd_unit.o' if tu.startswith('tests/') else 'build/src/cmd/climon.o'
+    compile_cmd = shlex.split(next(line for line in recipes if f' -c {tu} -o {old_object}' in line))
+    new_object = str(tree / 'mutant.o')
+    compile_cmd = [str(tree / tu) if arg == tu else new_object if arg == old_object
+                   else f'-I{tree}' if arg == '-I.' else arg for arg in compile_cmd]
+    binary = str(tree / 'netcmd-unit')
+    link_cmd = [new_object if arg == old_object else binary if arg == 'build/netcmd-unit' else arg
+                for arg in link]
+    print(f'building {name}', flush=True)
+    (tree / 'commands.json').write_text(json.dumps([compile_cmd, link_cmd], indent=2) + '\n')
+    with (tree / 'build.log').open('w') as log:
+        subprocess.run(['taskset', '-c', '44-55', *compile_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=300)
+        subprocess.run(['taskset', '-c', '44-55', *link_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=120)
+    for run in range(1, 6):
+        command = ['taskset', '-c', '44-51', 'timeout', '60', binary, case]
+        r = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+        (tree / f'run-{run}.log').write_text(r.stdout)
+        expected = r.returncode == -signal.SIGABRT if diagnostic is None else (
+            r.returncode == 1 and f'FAIL: {diagnostic}' in r.stdout)
+        results.append({'variant':name, 'case':case, 'run':run, 'returncode':r.returncode,
+                        'expected_failure':expected, 'output':r.stdout, 'command':command})
+        assert expected, (name, r.returncode, r.stdout)
+    print(f'{name}: rejected 5/5; {r.stdout.strip()}', flush=True)
+    (out / 'negative-results.json').write_text(json.dumps(results, indent=2) + '\n')
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
new file mode 100644
index 0000000000000000000000000000000000000000..bc72f719b1e93668f4c671000b1d5bad75da7ac0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
@@ -0,0 +1,16 @@
+#define _GNU_SOURCE
+#include <dlfcn.h>
+#include <stdio.h>
+#include <string.h>
+
+/* Read-only topology simulation: the process remains pinned to real CPUs 44-51.
+ * These serverless fixtures never launch workers. Only sibling metadata changes. */
+FILE *fopen(const char *path, const char *mode) {
+    static char pairs[2][7] = {"44-45\n", "46-47\n"};
+    int cpu = -1, end = 0;
+    if (sscanf(path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list%n",
+               &cpu, &end) == 1 && end && !path[end] && cpu >= 44 && cpu <= 47)
+        return fmemopen(pairs[(cpu - 44) / 2], 6, "r");
+    FILE *(*real_fopen)(const char *, const char *) = dlsym(RTLD_NEXT, "fopen");
+    return real_fopen(path, mode);
+}
diff --git a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
index 6d11e9ce5b2552d2f792115b3076459979edbe39..c04a11b7fc4a6c500632d4e2c48b7949c2dc8c6e
--- a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
+++ b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
@@ -60,7 +60,9 @@
 
     static void notify_retry() {
         Server server;
-        Config cfg; cfg.even_ifid = 1; cfg.even_ex = 1; cfg.shards = 16;
+        // The gate runs this serverless row without taskset. Automatic SMT placement requires
+        // whole sibling pairs for BOTH roles; only one IO is a tracking destination below.
+        Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "notification topology");
         server.threads_.resize(server.placement().total_threads());
         for (uint32_t i = 0; i < server.threads_.size(); ++i) {
@@ -94,7 +96,8 @@
     }
 
     static void flush() {
-        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 1; cfg.shards = 16;
+        // Two IOs still prove the foreign delivery; an even EX count also admits SMT placement.
+        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "flush topology");
         const auto& ios = server.placement().ifid_threads();
         check(ios.size() == 2, "foreign tracking IO exists");
@@ -136,7 +139,16 @@
         Server server; ThreadCtx self; IoLoop loop;
         loop.srv_ = &server; loop.self_ = &self;
         sender.bind(loop.wb_);
-        server.live_obuf_normal_hard_.store(8); server.client_obuf_armed_.store(true);
+        // Mirror Server::init's mailbox initialization without starting workers or opening rings.
+        // cx-waits made raw live-field stores writer-only; IO reads the published snapshot.
+        server.live_config_committed_ = server.capture_live_config(server.live_config_version_.load());
+        server.live_config_mailboxes_ = std::make_unique<LiveConfigMailbox[]>(server.nthreads() + 1);
+        server.live_config_mailboxes_[self.id()].init(server.live_config_committed_);
+        ClientOutputBufferLimits limits{}; limits.normal.hard_bytes = 8;
+        server.set_client_output_buffer_limits(limits);
+        check(server.client_obuf_armed() &&
+              server.client_limits_snapshot(self.id()).normal.hard_bytes == 8,
+              "hard limit published to the IO config mailbox");
         Client held(-1); held.set_id(1);
         check(held.rob().acquire(), "held ordinary operation"); held.rob().publish();
         check(!held.blocked(), "ordinary operation, not blocking exception");

exec
/bin/bash -lc 'taskset -c 52-55 python3 -u build/netcmdfix/live_matrix.py > build/netcmdfix/live-progress.log 2>&1' in /home/user/Projects/cx-final-netcmdfix
exec
/bin/bash -lc "git diff --numstat && git log -1 --oneline && cat build/netcmdfix/binary-sha256.txt && sed -n '47,65p' tests/core_concurrency_unit.cc" in /home/user/Projects/cx-final-netcmdfix
 succeeded in 0ms:
15	3	tests/netcmd_unit.cc
67f623716 merge cx-resizefix
09883d1aaa1d45e072862eed198f3c9ceb678a40ed5411b9ede48eea5426b938  build/tomokv
98eb090ff40d8321794c45531bdee092e2164d8c8b0527241b69677555926414  build/netcmd-unit
b307635e91699497c293279ff22c35125421f340c4d0099069537d46b323e1c6  build/netcmdfix/netcmd-unit-pre
            uint32_t count = 0;
            for (int cpu = 0; cpu < CPU_SETSIZE && count < 8; cpu++) {
                if (!CPU_ISSET(cpu, &cpus)) continue;
                if (count++) domain += '+';
                domain += std::to_string(cpu);
            }
            require(count == 8, "regression fixture requires eight allowed CPUs (no skip)");
            require(server.topo_.declare(domain.c_str()), "declare fixture topology");
            if constexpr (Fused)
                require(server.placement_.build_fused(server.topo_, nullptr), "fused placement");
            else
                require(server.placement_.build_even(server.topo_, 6, 2), "6:2 placement");
            require(server.placement_.reserve_runtime_roles(8), "reserve placement roles");
            Config config;
            config.shards = 16;
            config.thread_mode = Fused ? ThreadMode::Fused : ThreadMode::Split;
            config.flip_auto = 0;
            config.save.clear();
            require(server.init(config), "initialize in-memory fixture");

diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
new file mode 100644
index 0000000000000000000000000000000000000000..d51dc359bcc502c1649e79d7647bb76ebaf443a0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
@@ -0,0 +1,81 @@
+"""Run only tests/netcmd.py; each attempt owns one server PID and private data directory."""
+import itertools
+import json
+from pathlib import Path
+import resource
+import socket
+import subprocess
+import sys
+import time
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix/live'
+out.mkdir()
+sys.path.insert(0, str(root / 'tests'))
+from _lib import Conn
+
+results = []
+cells = itertools.product(('split', 'fused'), (0, 1), (0, 1), (0, 1))
+for cell, (mode, atomic, read_local, overlap) in enumerate(cells):
+    port = 8580 + cell
+    name = f'{mode}-a{atomic}-rl{read_local}-overlap{overlap}'
+    for attempt in range(1, 4):
+        directory = out / name / str(attempt)
+        directory.mkdir(parents=True)
+        with socket.socket() as guard:
+            guard.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
+            guard.bind(('127.0.0.1', port))
+        command = ['taskset', '-c', '44-51', './build/tomokv', '--bind', '127.0.0.1',
+                   '--port', str(port), '--shards', '16', '--thread-mode', mode,
+                   '--atomic', str(atomic), '--read-local', str(read_local),
+                   '--overlap', str(overlap), '--enable-debug-command', 'yes',
+                   '--dir', str(directory)]
+        if mode == 'split':
+            command += ['--ratio', '6:2']
+        record = dict(cell=name, attempt=attempt, port=port, command=command)
+        with (directory / 'server.log').open('w') as log:
+            server = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
+            record['pid'] = server.pid
+            try:
+                deadline = time.monotonic() + 30
+                while True:
+                    assert server.poll() is None, f'{name}: server exited {server.returncode}'
+                    try:
+                        connection = Conn('127.0.0.1', port, timeout=2)
+                        break
+                    except OSError:
+                        assert time.monotonic() < deadline, f'{name}: boot timeout'
+                        time.sleep(0.05)
+                try:
+                    assert connection.cmd('PING') == b'PONG'
+                    info = connection.cmd('INFO', 'server').decode()
+                    (directory / 'info-server.log').write_text(info)
+                    fields = dict(line.split(':', 1) for line in info.splitlines() if ':' in line)
+                    assert fields['thread_mode'] == ('2s' if mode == 'split' else '1s'), fields
+                    assert fields['read_local'] == str(read_local), fields
+                finally:
+                    connection.close()
+                test = ['taskset', '-c', '52-55', 'timeout', '60', 'python3', '-u',
+                        'tests/netcmd.py', '127.0.0.1', str(port)]
+                result = subprocess.run(test, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+                (directory / 'battery.log').write_text(result.stdout)
+                record.update(returncode=result.returncode, test_command=test)
+            except Exception as exc:
+                record.update(returncode=1, error=repr(exc))
+            finally:
+                if server.poll() is None:
+                    server.terminate()
+                    try:
+                        server.wait(timeout=20)
+                    except subprocess.TimeoutExpired:
+                        server.kill()
+                        server.wait()
+                record['server_exit'] = server.returncode
+        results.append(record)
+        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
+        if record['returncode'] or record['server_exit']:
+            print(f'FAIL {name} attempt {attempt}: {record}', flush=True)
+    cell_results = [r for r in results if r['cell'] == name]
+    print(f'{name}: {sum(r["returncode"] == 0 and r["server_exit"] == 0 for r in cell_results)}/3', flush=True)
+raise SystemExit(any(r['returncode'] or r['server_exit'] for r in results))
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
new file mode 100644
index 0000000000000000000000000000000000000000..b6494711491bb3710f859aa5fb7e295a4e3b093b
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
@@ -0,0 +1,77 @@
+"""Disposable source copies only; never modifies the production sources or objects."""
+import json
+from pathlib import Path
+import resource
+import shlex
+import shutil
+import signal
+import subprocess
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix'
+recipes = subprocess.check_output(['make', '-n', '-B', 'build/netcmd-unit'], text=True).splitlines()
+link = shlex.split(next(line for line in recipes if ' -o build/netcmd-unit ' in line))
+variants = [
+    ('notify-drop', 'notify-retry', 'src/cmd/notify.inc',
+     'return work;  // keep this target and the keyless record for the next pass',
+     'slot->keyless.pop_front(); return work + 1;  // deliberately lose the invalidation',
+     'tests/netcmd_unit.cc', 'failed marker post is incomplete work'),
+    ('flush-early', 'flush', 'src/cmd/climon.cc',
+     'if (armed & Server::kClimonTracking) {\n        ClimonConn* state',
+     'if (armed & Server::kClimonTracking) {\n'
+     '        if (op.cmd_name().eq_icase("flushall") || op.cmd_name().eq_icase("flushdb"))\n'
+     '            tracking_broadcast_flush();\n        ClimonConn* state',
+     'src/cmd/climon.cc', 'no flush invalidation before owner work was published'),
+    ('flush-missing', 'flush', 'src/cmd/climon.cc',
+     'void IoLoop::climon_flush_completed(Op& op) {\n',
+     'void IoLoop::climon_flush_completed(Op& op) {\n    return; // omit completed invalidation\n',
+     'src/cmd/climon.cc', 'completed flush invalidates foreign tracking IO'),
+    ('output-untracked', 'output', 'src/net/wb.h',
+     'else conn.stop_obuf_tracking();',
+     'else {} // deliberately leave accounting armed for uncounted bytes',
+     'tests/netcmd_unit.cc', None),
+    ('output-deferred', 'output', 'src/core/io_loop.h',
+     'uint64_t used = c->obuf_bytes() + wb_.deferred_output_bytes(*c);',
+     'uint64_t used = c->obuf_bytes(); // deliberately omit deferred storage',
+     'tests/netcmd_unit.cc', 'hard limit fired before Done'),
+]
+results = []
+for name, case, source, before, after, tu, diagnostic in variants:
+    tree = out / 'mutants' / name
+    tree.mkdir(parents=True)
+    shutil.copytree(root / 'src', tree / 'src')
+    (tree / 'third_party').symlink_to(root / 'third_party', target_is_directory=True)
+    (tree / 'tests').mkdir()
+    for test in ('netcmd_unit.cc', 'netcmd_unit.h'):
+        shutil.copy2(root / 'tests' / test, tree / 'tests' / test)
+    target = tree / source
+    original = target.read_text()
+    assert original.count(before) == (2 if name == 'output-untracked' else 1)
+    target.write_text(original.replace(before, after))
+    old_object = 'build/tests/netcmd_unit.o' if tu.startswith('tests/') else 'build/src/cmd/climon.o'
+    compile_cmd = shlex.split(next(line for line in recipes if f' -c {tu} -o {old_object}' in line))
+    new_object = str(tree / 'mutant.o')
+    compile_cmd = [str(tree / tu) if arg == tu else new_object if arg == old_object
+                   else f'-I{tree}' if arg == '-I.' else arg for arg in compile_cmd]
+    binary = str(tree / 'netcmd-unit')
+    link_cmd = [new_object if arg == old_object else binary if arg == 'build/netcmd-unit' else arg
+                for arg in link]
+    print(f'building {name}', flush=True)
+    (tree / 'commands.json').write_text(json.dumps([compile_cmd, link_cmd], indent=2) + '\n')
+    with (tree / 'build.log').open('w') as log:
+        subprocess.run(['taskset', '-c', '44-55', *compile_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=300)
+        subprocess.run(['taskset', '-c', '44-55', *link_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=120)
+    for run in range(1, 6):
+        command = ['taskset', '-c', '44-51', 'timeout', '60', binary, case]
+        r = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+        (tree / f'run-{run}.log').write_text(r.stdout)
+        expected = r.returncode == -signal.SIGABRT if diagnostic is None else (
+            r.returncode == 1 and f'FAIL: {diagnostic}' in r.stdout)
+        results.append({'variant':name, 'case':case, 'run':run, 'returncode':r.returncode,
+                        'expected_failure':expected, 'output':r.stdout, 'command':command})
+        assert expected, (name, r.returncode, r.stdout)
+    print(f'{name}: rejected 5/5; {r.stdout.strip()}', flush=True)
+    (out / 'negative-results.json').write_text(json.dumps(results, indent=2) + '\n')
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
new file mode 100644
index 0000000000000000000000000000000000000000..bc72f719b1e93668f4c671000b1d5bad75da7ac0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
@@ -0,0 +1,16 @@
+#define _GNU_SOURCE
+#include <dlfcn.h>
+#include <stdio.h>
+#include <string.h>
+
+/* Read-only topology simulation: the process remains pinned to real CPUs 44-51.
+ * These serverless fixtures never launch workers. Only sibling metadata changes. */
+FILE *fopen(const char *path, const char *mode) {
+    static char pairs[2][7] = {"44-45\n", "46-47\n"};
+    int cpu = -1, end = 0;
+    if (sscanf(path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list%n",
+               &cpu, &end) == 1 && end && !path[end] && cpu >= 44 && cpu <= 47)
+        return fmemopen(pairs[(cpu - 44) / 2], 6, "r");
+    FILE *(*real_fopen)(const char *, const char *) = dlsym(RTLD_NEXT, "fopen");
+    return real_fopen(path, mode);
+}
diff --git a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
index 6d11e9ce5b2552d2f792115b3076459979edbe39..c04a11b7fc4a6c500632d4e2c48b7949c2dc8c6e
--- a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
+++ b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
@@ -60,7 +60,9 @@
 
     static void notify_retry() {
         Server server;
-        Config cfg; cfg.even_ifid = 1; cfg.even_ex = 1; cfg.shards = 16;
+        // The gate runs this serverless row without taskset. Automatic SMT placement requires
+        // whole sibling pairs for BOTH roles; only one IO is a tracking destination below.
+        Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "notification topology");
         server.threads_.resize(server.placement().total_threads());
         for (uint32_t i = 0; i < server.threads_.size(); ++i) {
@@ -94,7 +96,8 @@
     }
 
     static void flush() {
-        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 1; cfg.shards = 16;
+        // Two IOs still prove the foreign delivery; an even EX count also admits SMT placement.
+        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "flush topology");
         const auto& ios = server.placement().ifid_threads();
         check(ios.size() == 2, "foreign tracking IO exists");
@@ -136,7 +139,16 @@
         Server server; ThreadCtx self; IoLoop loop;
         loop.srv_ = &server; loop.self_ = &self;
         sender.bind(loop.wb_);
-        server.live_obuf_normal_hard_.store(8); server.client_obuf_armed_.store(true);
+        // Mirror Server::init's mailbox initialization without starting workers or opening rings.
+        // cx-waits made raw live-field stores writer-only; IO reads the published snapshot.
+        server.live_config_committed_ = server.capture_live_config(server.live_config_version_.load());
+        server.live_config_mailboxes_ = std::make_unique<LiveConfigMailbox[]>(server.nthreads() + 1);
+        server.live_config_mailboxes_[self.id()].init(server.live_config_committed_);
+        ClientOutputBufferLimits limits{}; limits.normal.hard_bytes = 8;
+        server.set_client_output_buffer_limits(limits);
+        check(server.client_obuf_armed() &&
+              server.client_limits_snapshot(self.id()).normal.hard_bytes == 8,
+              "hard limit published to the IO config mailbox");
         Client held(-1); held.set_id(1);
         check(held.rob().acquire(), "held ordinary operation"); held.rob().publish();
         check(!held.blocked(), "ordinary operation, not blocking exception");

 exited 1 in 2450ms:
exec
/bin/bash -lc 'tail -5 build/netcmdfix/live-progress.log && git diff 821d207b1..HEAD -- src/cmd/notify.inc src/cmd/climon.cc src/net/wb.h && git status --short' in /home/user/Projects/cx-final-netcmdfix
 succeeded in 0ms:
fused-a0-rl0-overlap1: 3/3
Traceback (most recent call last):
  File "/home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py", line 28, in <module>
    guard.bind(('127.0.0.1', port))
OSError: [Errno 98] Address already in use
 M tests/netcmd_unit.cc

diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
new file mode 100644
index 0000000000000000000000000000000000000000..d51dc359bcc502c1649e79d7647bb76ebaf443a0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
@@ -0,0 +1,81 @@
+"""Run only tests/netcmd.py; each attempt owns one server PID and private data directory."""
+import itertools
+import json
+from pathlib import Path
+import resource
+import socket
+import subprocess
+import sys
+import time
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix/live'
+out.mkdir()
+sys.path.insert(0, str(root / 'tests'))
+from _lib import Conn
+
+results = []
+cells = itertools.product(('split', 'fused'), (0, 1), (0, 1), (0, 1))
+for cell, (mode, atomic, read_local, overlap) in enumerate(cells):
+    port = 8580 + cell
+    name = f'{mode}-a{atomic}-rl{read_local}-overlap{overlap}'
+    for attempt in range(1, 4):
+        directory = out / name / str(attempt)
+        directory.mkdir(parents=True)
+        with socket.socket() as guard:
+            guard.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
+            guard.bind(('127.0.0.1', port))
+        command = ['taskset', '-c', '44-51', './build/tomokv', '--bind', '127.0.0.1',
+                   '--port', str(port), '--shards', '16', '--thread-mode', mode,
+                   '--atomic', str(atomic), '--read-local', str(read_local),
+                   '--overlap', str(overlap), '--enable-debug-command', 'yes',
+                   '--dir', str(directory)]
+        if mode == 'split':
+            command += ['--ratio', '6:2']
+        record = dict(cell=name, attempt=attempt, port=port, command=command)
+        with (directory / 'server.log').open('w') as log:
+            server = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
+            record['pid'] = server.pid
+            try:
+                deadline = time.monotonic() + 30
+                while True:
+                    assert server.poll() is None, f'{name}: server exited {server.returncode}'
+                    try:
+                        connection = Conn('127.0.0.1', port, timeout=2)
+                        break
+                    except OSError:
+                        assert time.monotonic() < deadline, f'{name}: boot timeout'
+                        time.sleep(0.05)
+                try:
+                    assert connection.cmd('PING') == b'PONG'
+                    info = connection.cmd('INFO', 'server').decode()
+                    (directory / 'info-server.log').write_text(info)
+                    fields = dict(line.split(':', 1) for line in info.splitlines() if ':' in line)
+                    assert fields['thread_mode'] == ('2s' if mode == 'split' else '1s'), fields
+                    assert fields['read_local'] == str(read_local), fields
+                finally:
+                    connection.close()
+                test = ['taskset', '-c', '52-55', 'timeout', '60', 'python3', '-u',
+                        'tests/netcmd.py', '127.0.0.1', str(port)]
+                result = subprocess.run(test, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+                (directory / 'battery.log').write_text(result.stdout)
+                record.update(returncode=result.returncode, test_command=test)
+            except Exception as exc:
+                record.update(returncode=1, error=repr(exc))
+            finally:
+                if server.poll() is None:
+                    server.terminate()
+                    try:
+                        server.wait(timeout=20)
+                    except subprocess.TimeoutExpired:
+                        server.kill()
+                        server.wait()
+                record['server_exit'] = server.returncode
+        results.append(record)
+        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
+        if record['returncode'] or record['server_exit']:
+            print(f'FAIL {name} attempt {attempt}: {record}', flush=True)
+    cell_results = [r for r in results if r['cell'] == name]
+    print(f'{name}: {sum(r["returncode"] == 0 and r["server_exit"] == 0 for r in cell_results)}/3', flush=True)
+raise SystemExit(any(r['returncode'] or r['server_exit'] for r in results))
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
new file mode 100644
index 0000000000000000000000000000000000000000..b6494711491bb3710f859aa5fb7e295a4e3b093b
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
@@ -0,0 +1,77 @@
+"""Disposable source copies only; never modifies the production sources or objects."""
+import json
+from pathlib import Path
+import resource
+import shlex
+import shutil
+import signal
+import subprocess
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix'
+recipes = subprocess.check_output(['make', '-n', '-B', 'build/netcmd-unit'], text=True).splitlines()
+link = shlex.split(next(line for line in recipes if ' -o build/netcmd-unit ' in line))
+variants = [
+    ('notify-drop', 'notify-retry', 'src/cmd/notify.inc',
+     'return work;  // keep this target and the keyless record for the next pass',
+     'slot->keyless.pop_front(); return work + 1;  // deliberately lose the invalidation',
+     'tests/netcmd_unit.cc', 'failed marker post is incomplete work'),
+    ('flush-early', 'flush', 'src/cmd/climon.cc',
+     'if (armed & Server::kClimonTracking) {\n        ClimonConn* state',
+     'if (armed & Server::kClimonTracking) {\n'
+     '        if (op.cmd_name().eq_icase("flushall") || op.cmd_name().eq_icase("flushdb"))\n'
+     '            tracking_broadcast_flush();\n        ClimonConn* state',
+     'src/cmd/climon.cc', 'no flush invalidation before owner work was published'),
+    ('flush-missing', 'flush', 'src/cmd/climon.cc',
+     'void IoLoop::climon_flush_completed(Op& op) {\n',
+     'void IoLoop::climon_flush_completed(Op& op) {\n    return; // omit completed invalidation\n',
+     'src/cmd/climon.cc', 'completed flush invalidates foreign tracking IO'),
+    ('output-untracked', 'output', 'src/net/wb.h',
+     'else conn.stop_obuf_tracking();',
+     'else {} // deliberately leave accounting armed for uncounted bytes',
+     'tests/netcmd_unit.cc', None),
+    ('output-deferred', 'output', 'src/core/io_loop.h',
+     'uint64_t used = c->obuf_bytes() + wb_.deferred_output_bytes(*c);',
+     'uint64_t used = c->obuf_bytes(); // deliberately omit deferred storage',
+     'tests/netcmd_unit.cc', 'hard limit fired before Done'),
+]
+results = []
+for name, case, source, before, after, tu, diagnostic in variants:
+    tree = out / 'mutants' / name
+    tree.mkdir(parents=True)
+    shutil.copytree(root / 'src', tree / 'src')
+    (tree / 'third_party').symlink_to(root / 'third_party', target_is_directory=True)
+    (tree / 'tests').mkdir()
+    for test in ('netcmd_unit.cc', 'netcmd_unit.h'):
+        shutil.copy2(root / 'tests' / test, tree / 'tests' / test)
+    target = tree / source
+    original = target.read_text()
+    assert original.count(before) == (2 if name == 'output-untracked' else 1)
+    target.write_text(original.replace(before, after))
+    old_object = 'build/tests/netcmd_unit.o' if tu.startswith('tests/') else 'build/src/cmd/climon.o'
+    compile_cmd = shlex.split(next(line for line in recipes if f' -c {tu} -o {old_object}' in line))
+    new_object = str(tree / 'mutant.o')
+    compile_cmd = [str(tree / tu) if arg == tu else new_object if arg == old_object
+                   else f'-I{tree}' if arg == '-I.' else arg for arg in compile_cmd]
+    binary = str(tree / 'netcmd-unit')
+    link_cmd = [new_object if arg == old_object else binary if arg == 'build/netcmd-unit' else arg
+                for arg in link]
+    print(f'building {name}', flush=True)
+    (tree / 'commands.json').write_text(json.dumps([compile_cmd, link_cmd], indent=2) + '\n')
+    with (tree / 'build.log').open('w') as log:
+        subprocess.run(['taskset', '-c', '44-55', *compile_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=300)
+        subprocess.run(['taskset', '-c', '44-55', *link_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=120)
+    for run in range(1, 6):
+        command = ['taskset', '-c', '44-51', 'timeout', '60', binary, case]
+        r = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+        (tree / f'run-{run}.log').write_text(r.stdout)
+        expected = r.returncode == -signal.SIGABRT if diagnostic is None else (
+            r.returncode == 1 and f'FAIL: {diagnostic}' in r.stdout)
+        results.append({'variant':name, 'case':case, 'run':run, 'returncode':r.returncode,
+                        'expected_failure':expected, 'output':r.stdout, 'command':command})
+        assert expected, (name, r.returncode, r.stdout)
+    print(f'{name}: rejected 5/5; {r.stdout.strip()}', flush=True)
+    (out / 'negative-results.json').write_text(json.dumps(results, indent=2) + '\n')
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
new file mode 100644
index 0000000000000000000000000000000000000000..bc72f719b1e93668f4c671000b1d5bad75da7ac0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
@@ -0,0 +1,16 @@
+#define _GNU_SOURCE
+#include <dlfcn.h>
+#include <stdio.h>
+#include <string.h>
+
+/* Read-only topology simulation: the process remains pinned to real CPUs 44-51.
+ * These serverless fixtures never launch workers. Only sibling metadata changes. */
+FILE *fopen(const char *path, const char *mode) {
+    static char pairs[2][7] = {"44-45\n", "46-47\n"};
+    int cpu = -1, end = 0;
+    if (sscanf(path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list%n",
+               &cpu, &end) == 1 && end && !path[end] && cpu >= 44 && cpu <= 47)
+        return fmemopen(pairs[(cpu - 44) / 2], 6, "r");
+    FILE *(*real_fopen)(const char *, const char *) = dlsym(RTLD_NEXT, "fopen");
+    return real_fopen(path, mode);
+}
diff --git a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
index 6d11e9ce5b2552d2f792115b3076459979edbe39..c04a11b7fc4a6c500632d4e2c48b7949c2dc8c6e
--- a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
+++ b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
@@ -60,7 +60,9 @@
 
     static void notify_retry() {
         Server server;
-        Config cfg; cfg.even_ifid = 1; cfg.even_ex = 1; cfg.shards = 16;
+        // The gate runs this serverless row without taskset. Automatic SMT placement requires
+        // whole sibling pairs for BOTH roles; only one IO is a tracking destination below.
+        Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "notification topology");
         server.threads_.resize(server.placement().total_threads());
         for (uint32_t i = 0; i < server.threads_.size(); ++i) {
@@ -94,7 +96,8 @@
     }
 
     static void flush() {
-        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 1; cfg.shards = 16;
+        // Two IOs still prove the foreign delivery; an even EX count also admits SMT placement.
+        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "flush topology");
         const auto& ios = server.placement().ifid_threads();
         check(ios.size() == 2, "foreign tracking IO exists");
@@ -136,7 +139,16 @@
         Server server; ThreadCtx self; IoLoop loop;
         loop.srv_ = &server; loop.self_ = &self;
         sender.bind(loop.wb_);
-        server.live_obuf_normal_hard_.store(8); server.client_obuf_armed_.store(true);
+        // Mirror Server::init's mailbox initialization without starting workers or opening rings.
+        // cx-waits made raw live-field stores writer-only; IO reads the published snapshot.
+        server.live_config_committed_ = server.capture_live_config(server.live_config_version_.load());
+        server.live_config_mailboxes_ = std::make_unique<LiveConfigMailbox[]>(server.nthreads() + 1);
+        server.live_config_mailboxes_[self.id()].init(server.live_config_committed_);
+        ClientOutputBufferLimits limits{}; limits.normal.hard_bytes = 8;
+        server.set_client_output_buffer_limits(limits);
+        check(server.client_obuf_armed() &&
+              server.client_limits_snapshot(self.id()).normal.hard_bytes == 8,
+              "hard limit published to the IO config mailbox");
         Client held(-1); held.set_id(1);
         check(held.rob().acquire(), "held ordinary operation"); held.rob().publish();
         check(!held.blocked(), "ordinary operation, not blocking exception");

exec
/bin/bash -lc "rg -n 'thread_mode:|shards:|overlap:|atomic:|io_threads:|ex_threads:|read_local:' build/netcmdfix/live/split-a1-rl1-overlap1/3/info-server.log && du -sh build/netcmdfix" in /home/user/Projects/cx-final-netcmdfix
 succeeded in 0ms:
5:thread_mode:2s
6:shards:16
7:overlap:1
9:read_local:1
10:atomic:1
45:io_threads:6
46:ex_threads:2
532M	build/netcmdfix

exec
/bin/bash -lc "cat build/netcmdfix/live-progress.log && python3 - <<'PY'
import json
from collections import defaultdict
from pathlib import Path
rs=json.loads(Path('build/netcmdfix/live/results.json').read_text())
groups=defaultdict(list)
for r in rs: groups[r['cell']].append(r)
for cell, rows in groups.items():
 print(cell, rows[-1]['port'], [(r['returncode'],r['server_exit']) for r in rows])
PY
ss -ltnp 'sport >= :8580 and sport <= :8609'" in /home/user/Projects/cx-final-netcmdfix
 succeeded in 0ms:
split-a0-rl0-overlap0: 3/3
split-a0-rl0-overlap1: 3/3
split-a0-rl1-overlap0: 3/3
split-a0-rl1-overlap1: 3/3
split-a1-rl0-overlap0: 3/3
split-a1-rl0-overlap1: 3/3
split-a1-rl1-overlap0: 3/3
split-a1-rl1-overlap1: 3/3
fused-a0-rl0-overlap0: 3/3
fused-a0-rl0-overlap1: 3/3
Traceback (most recent call last):
  File "/home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py", line 28, in <module>
    guard.bind(('127.0.0.1', port))
OSError: [Errno 98] Address already in use
split-a0-rl0-overlap0 8580 [(0, 0), (0, 0), (0, 0)]
split-a0-rl0-overlap1 8581 [(0, 0), (0, 0), (0, 0)]
split-a0-rl1-overlap0 8582 [(0, 0), (0, 0), (0, 0)]
split-a0-rl1-overlap1 8583 [(0, 0), (0, 0), (0, 0)]
split-a1-rl0-overlap0 8584 [(0, 0), (0, 0), (0, 0)]
split-a1-rl0-overlap1 8585 [(0, 0), (0, 0), (0, 0)]
split-a1-rl1-overlap0 8586 [(0, 0), (0, 0), (0, 0)]
split-a1-rl1-overlap1 8587 [(0, 0), (0, 0), (0, 0)]
fused-a0-rl0-overlap0 8588 [(0, 0), (0, 0), (0, 0)]
fused-a0-rl0-overlap1 8589 [(0, 0), (0, 0), (0, 0)]
State  Recv-Q Send-Q Local Address:Port Peer Address:PortProcess
LISTEN 0      512        127.0.0.1:8590      0.0.0.0:*    users:(("GarnetServer",pid=2762718,fd=132))

diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
new file mode 100644
index 0000000000000000000000000000000000000000..d51dc359bcc502c1649e79d7647bb76ebaf443a0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
@@ -0,0 +1,81 @@
+"""Run only tests/netcmd.py; each attempt owns one server PID and private data directory."""
+import itertools
+import json
+from pathlib import Path
+import resource
+import socket
+import subprocess
+import sys
+import time
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix/live'
+out.mkdir()
+sys.path.insert(0, str(root / 'tests'))
+from _lib import Conn
+
+results = []
+cells = itertools.product(('split', 'fused'), (0, 1), (0, 1), (0, 1))
+for cell, (mode, atomic, read_local, overlap) in enumerate(cells):
+    port = 8580 + cell
+    name = f'{mode}-a{atomic}-rl{read_local}-overlap{overlap}'
+    for attempt in range(1, 4):
+        directory = out / name / str(attempt)
+        directory.mkdir(parents=True)
+        with socket.socket() as guard:
+            guard.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
+            guard.bind(('127.0.0.1', port))
+        command = ['taskset', '-c', '44-51', './build/tomokv', '--bind', '127.0.0.1',
+                   '--port', str(port), '--shards', '16', '--thread-mode', mode,
+                   '--atomic', str(atomic), '--read-local', str(read_local),
+                   '--overlap', str(overlap), '--enable-debug-command', 'yes',
+                   '--dir', str(directory)]
+        if mode == 'split':
+            command += ['--ratio', '6:2']
+        record = dict(cell=name, attempt=attempt, port=port, command=command)
+        with (directory / 'server.log').open('w') as log:
+            server = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
+            record['pid'] = server.pid
+            try:
+                deadline = time.monotonic() + 30
+                while True:
+                    assert server.poll() is None, f'{name}: server exited {server.returncode}'
+                    try:
+                        connection = Conn('127.0.0.1', port, timeout=2)
+                        break
+                    except OSError:
+                        assert time.monotonic() < deadline, f'{name}: boot timeout'
+                        time.sleep(0.05)
+                try:
+                    assert connection.cmd('PING') == b'PONG'
+                    info = connection.cmd('INFO', 'server').decode()
+                    (directory / 'info-server.log').write_text(info)
+                    fields = dict(line.split(':', 1) for line in info.splitlines() if ':' in line)
+                    assert fields['thread_mode'] == ('2s' if mode == 'split' else '1s'), fields
+                    assert fields['read_local'] == str(read_local), fields
+                finally:
+                    connection.close()
+                test = ['taskset', '-c', '52-55', 'timeout', '60', 'python3', '-u',
+                        'tests/netcmd.py', '127.0.0.1', str(port)]
+                result = subprocess.run(test, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+                (directory / 'battery.log').write_text(result.stdout)
+                record.update(returncode=result.returncode, test_command=test)
+            except Exception as exc:
+                record.update(returncode=1, error=repr(exc))
+            finally:
+                if server.poll() is None:
+                    server.terminate()
+                    try:
+                        server.wait(timeout=20)
+                    except subprocess.TimeoutExpired:
+                        server.kill()
+                        server.wait()
+                record['server_exit'] = server.returncode
+        results.append(record)
+        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
+        if record['returncode'] or record['server_exit']:
+            print(f'FAIL {name} attempt {attempt}: {record}', flush=True)
+    cell_results = [r for r in results if r['cell'] == name]
+    print(f'{name}: {sum(r["returncode"] == 0 and r["server_exit"] == 0 for r in cell_results)}/3', flush=True)
+raise SystemExit(any(r['returncode'] or r['server_exit'] for r in results))
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
new file mode 100644
index 0000000000000000000000000000000000000000..b6494711491bb3710f859aa5fb7e295a4e3b093b
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
@@ -0,0 +1,77 @@
+"""Disposable source copies only; never modifies the production sources or objects."""
+import json
+from pathlib import Path
+import resource
+import shlex
+import shutil
+import signal
+import subprocess
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix'
+recipes = subprocess.check_output(['make', '-n', '-B', 'build/netcmd-unit'], text=True).splitlines()
+link = shlex.split(next(line for line in recipes if ' -o build/netcmd-unit ' in line))
+variants = [
+    ('notify-drop', 'notify-retry', 'src/cmd/notify.inc',
+     'return work;  // keep this target and the keyless record for the next pass',
+     'slot->keyless.pop_front(); return work + 1;  // deliberately lose the invalidation',
+     'tests/netcmd_unit.cc', 'failed marker post is incomplete work'),
+    ('flush-early', 'flush', 'src/cmd/climon.cc',
+     'if (armed & Server::kClimonTracking) {\n        ClimonConn* state',
+     'if (armed & Server::kClimonTracking) {\n'
+     '        if (op.cmd_name().eq_icase("flushall") || op.cmd_name().eq_icase("flushdb"))\n'
+     '            tracking_broadcast_flush();\n        ClimonConn* state',
+     'src/cmd/climon.cc', 'no flush invalidation before owner work was published'),
+    ('flush-missing', 'flush', 'src/cmd/climon.cc',
+     'void IoLoop::climon_flush_completed(Op& op) {\n',
+     'void IoLoop::climon_flush_completed(Op& op) {\n    return; // omit completed invalidation\n',
+     'src/cmd/climon.cc', 'completed flush invalidates foreign tracking IO'),
+    ('output-untracked', 'output', 'src/net/wb.h',
+     'else conn.stop_obuf_tracking();',
+     'else {} // deliberately leave accounting armed for uncounted bytes',
+     'tests/netcmd_unit.cc', None),
+    ('output-deferred', 'output', 'src/core/io_loop.h',
+     'uint64_t used = c->obuf_bytes() + wb_.deferred_output_bytes(*c);',
+     'uint64_t used = c->obuf_bytes(); // deliberately omit deferred storage',
+     'tests/netcmd_unit.cc', 'hard limit fired before Done'),
+]
+results = []
+for name, case, source, before, after, tu, diagnostic in variants:
+    tree = out / 'mutants' / name
+    tree.mkdir(parents=True)
+    shutil.copytree(root / 'src', tree / 'src')
+    (tree / 'third_party').symlink_to(root / 'third_party', target_is_directory=True)
+    (tree / 'tests').mkdir()
+    for test in ('netcmd_unit.cc', 'netcmd_unit.h'):
+        shutil.copy2(root / 'tests' / test, tree / 'tests' / test)
+    target = tree / source
+    original = target.read_text()
+    assert original.count(before) == (2 if name == 'output-untracked' else 1)
+    target.write_text(original.replace(before, after))
+    old_object = 'build/tests/netcmd_unit.o' if tu.startswith('tests/') else 'build/src/cmd/climon.o'
+    compile_cmd = shlex.split(next(line for line in recipes if f' -c {tu} -o {old_object}' in line))
+    new_object = str(tree / 'mutant.o')
+    compile_cmd = [str(tree / tu) if arg == tu else new_object if arg == old_object
+                   else f'-I{tree}' if arg == '-I.' else arg for arg in compile_cmd]
+    binary = str(tree / 'netcmd-unit')
+    link_cmd = [new_object if arg == old_object else binary if arg == 'build/netcmd-unit' else arg
+                for arg in link]
+    print(f'building {name}', flush=True)
+    (tree / 'commands.json').write_text(json.dumps([compile_cmd, link_cmd], indent=2) + '\n')
+    with (tree / 'build.log').open('w') as log:
+        subprocess.run(['taskset', '-c', '44-55', *compile_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=300)
+        subprocess.run(['taskset', '-c', '44-55', *link_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=120)
+    for run in range(1, 6):
+        command = ['taskset', '-c', '44-51', 'timeout', '60', binary, case]
+        r = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+        (tree / f'run-{run}.log').write_text(r.stdout)
+        expected = r.returncode == -signal.SIGABRT if diagnostic is None else (
+            r.returncode == 1 and f'FAIL: {diagnostic}' in r.stdout)
+        results.append({'variant':name, 'case':case, 'run':run, 'returncode':r.returncode,
+                        'expected_failure':expected, 'output':r.stdout, 'command':command})
+        assert expected, (name, r.returncode, r.stdout)
+    print(f'{name}: rejected 5/5; {r.stdout.strip()}', flush=True)
+    (out / 'negative-results.json').write_text(json.dumps(results, indent=2) + '\n')
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
new file mode 100644
index 0000000000000000000000000000000000000000..bc72f719b1e93668f4c671000b1d5bad75da7ac0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
@@ -0,0 +1,16 @@
+#define _GNU_SOURCE
+#include <dlfcn.h>
+#include <stdio.h>
+#include <string.h>
+
+/* Read-only topology simulation: the process remains pinned to real CPUs 44-51.
+ * These serverless fixtures never launch workers. Only sibling metadata changes. */
+FILE *fopen(const char *path, const char *mode) {
+    static char pairs[2][7] = {"44-45\n", "46-47\n"};
+    int cpu = -1, end = 0;
+    if (sscanf(path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list%n",
+               &cpu, &end) == 1 && end && !path[end] && cpu >= 44 && cpu <= 47)
+        return fmemopen(pairs[(cpu - 44) / 2], 6, "r");
+    FILE *(*real_fopen)(const char *, const char *) = dlsym(RTLD_NEXT, "fopen");
+    return real_fopen(path, mode);
+}
diff --git a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
index 6d11e9ce5b2552d2f792115b3076459979edbe39..c04a11b7fc4a6c500632d4e2c48b7949c2dc8c6e
--- a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
+++ b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
@@ -60,7 +60,9 @@
 
     static void notify_retry() {
         Server server;
-        Config cfg; cfg.even_ifid = 1; cfg.even_ex = 1; cfg.shards = 16;
+        // The gate runs this serverless row without taskset. Automatic SMT placement requires
+        // whole sibling pairs for BOTH roles; only one IO is a tracking destination below.
+        Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "notification topology");
         server.threads_.resize(server.placement().total_threads());
         for (uint32_t i = 0; i < server.threads_.size(); ++i) {
@@ -94,7 +96,8 @@
     }
 
     static void flush() {
-        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 1; cfg.shards = 16;
+        // Two IOs still prove the foreign delivery; an even EX count also admits SMT placement.
+        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "flush topology");
         const auto& ios = server.placement().ifid_threads();
         check(ios.size() == 2, "foreign tracking IO exists");
@@ -136,7 +139,16 @@
         Server server; ThreadCtx self; IoLoop loop;
         loop.srv_ = &server; loop.self_ = &self;
         sender.bind(loop.wb_);
-        server.live_obuf_normal_hard_.store(8); server.client_obuf_armed_.store(true);
+        // Mirror Server::init's mailbox initialization without starting workers or opening rings.
+        // cx-waits made raw live-field stores writer-only; IO reads the published snapshot.
+        server.live_config_committed_ = server.capture_live_config(server.live_config_version_.load());
+        server.live_config_mailboxes_ = std::make_unique<LiveConfigMailbox[]>(server.nthreads() + 1);
+        server.live_config_mailboxes_[self.id()].init(server.live_config_committed_);
+        ClientOutputBufferLimits limits{}; limits.normal.hard_bytes = 8;
+        server.set_client_output_buffer_limits(limits);
+        check(server.client_obuf_armed() &&
+              server.client_limits_snapshot(self.id()).normal.hard_bytes == 8,
+              "hard limit published to the IO config mailbox");
         Client held(-1); held.set_id(1);
         check(held.rob().acquire(), "held ordinary operation"); held.rob().publish();
         check(!held.blocked(), "ordinary operation, not blocking exception");

apply patch
patch: completed
/home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
new file mode 100644
index 0000000000000000000000000000000000000000..295ae00928add7346879af86ca8bfdb274f46beb
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
@@ -0,0 +1,85 @@
+"""Run only tests/netcmd.py; each attempt owns one server PID and private data directory."""
+import itertools
+import json
+from pathlib import Path
+import resource
+import socket
+import subprocess
+import sys
+import time
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix/live'
+out.mkdir(exist_ok=True)
+sys.path.insert(0, str(root / 'tests'))
+from _lib import Conn
+
+results_file = out / 'results.json'
+results = json.loads(results_file.read_text()) if results_file.exists() else []
+cells = itertools.product(('split', 'fused'), (0, 1), (0, 1), (0, 1))
+for cell, (mode, atomic, read_local, overlap) in enumerate(cells):
+    # 8590 belongs to an existing Garnet PID; resume the remaining cells on unused ports.
+    port = 8580 + cell if cell < 10 else 8596 + cell - 10
+    name = f'{mode}-a{atomic}-rl{read_local}-overlap{overlap}'
+    for attempt in range(1, 4):
+        if any(r['cell'] == name and r['attempt'] == attempt for r in results):
+            continue  # preserve completed attempts, including any failure, when resuming
+        directory = out / name / str(attempt)
+        directory.mkdir(parents=True, exist_ok=True)
+        with socket.socket() as guard:
+            guard.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
+            guard.bind(('127.0.0.1', port))
+        command = ['taskset', '-c', '44-51', './build/tomokv', '--bind', '127.0.0.1',
+                   '--port', str(port), '--shards', '16', '--thread-mode', mode,
+                   '--atomic', str(atomic), '--read-local', str(read_local),
+                   '--overlap', str(overlap), '--enable-debug-command', 'yes',
+                   '--dir', str(directory)]
+        if mode == 'split':
+            command += ['--ratio', '6:2']
+        record = dict(cell=name, attempt=attempt, port=port, command=command)
+        with (directory / 'server.log').open('w') as log:
+            server = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
+            record['pid'] = server.pid
+            try:
+                deadline = time.monotonic() + 30
+                while True:
+                    assert server.poll() is None, f'{name}: server exited {server.returncode}'
+                    try:
+                        connection = Conn('127.0.0.1', port, timeout=2)
+                        break
+                    except OSError:
+                        assert time.monotonic() < deadline, f'{name}: boot timeout'
+                        time.sleep(0.05)
+                try:
+                    assert connection.cmd('PING') == b'PONG'
+                    info = connection.cmd('INFO', 'server').decode()
+                    (directory / 'info-server.log').write_text(info)
+                    fields = dict(line.split(':', 1) for line in info.splitlines() if ':' in line)
+                    assert fields['thread_mode'] == ('2s' if mode == 'split' else '1s'), fields
+                    assert fields['read_local'] == str(read_local), fields
+                finally:
+                    connection.close()
+                test = ['taskset', '-c', '52-55', 'timeout', '60', 'python3', '-u',
+                        'tests/netcmd.py', '127.0.0.1', str(port)]
+                result = subprocess.run(test, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+                (directory / 'battery.log').write_text(result.stdout)
+                record.update(returncode=result.returncode, test_command=test)
+            except Exception as exc:
+                record.update(returncode=1, error=repr(exc))
+            finally:
+                if server.poll() is None:
+                    server.terminate()
+                    try:
+                        server.wait(timeout=20)
+                    except subprocess.TimeoutExpired:
+                        server.kill()
+                        server.wait()
+                record['server_exit'] = server.returncode
+        results.append(record)
+        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
+        if record['returncode'] or record['server_exit']:
+            print(f'FAIL {name} attempt {attempt}: {record}', flush=True)
+    cell_results = [r for r in results if r['cell'] == name]
+    print(f'{name}: {sum(r["returncode"] == 0 and r["server_exit"] == 0 for r in cell_results)}/3', flush=True)
+raise SystemExit(any(r['returncode'] or r['server_exit'] for r in results))
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
new file mode 100644
index 0000000000000000000000000000000000000000..b6494711491bb3710f859aa5fb7e295a4e3b093b
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
@@ -0,0 +1,77 @@
+"""Disposable source copies only; never modifies the production sources or objects."""
+import json
+from pathlib import Path
+import resource
+import shlex
+import shutil
+import signal
+import subprocess
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix'
+recipes = subprocess.check_output(['make', '-n', '-B', 'build/netcmd-unit'], text=True).splitlines()
+link = shlex.split(next(line for line in recipes if ' -o build/netcmd-unit ' in line))
+variants = [
+    ('notify-drop', 'notify-retry', 'src/cmd/notify.inc',
+     'return work;  // keep this target and the keyless record for the next pass',
+     'slot->keyless.pop_front(); return work + 1;  // deliberately lose the invalidation',
+     'tests/netcmd_unit.cc', 'failed marker post is incomplete work'),
+    ('flush-early', 'flush', 'src/cmd/climon.cc',
+     'if (armed & Server::kClimonTracking) {\n        ClimonConn* state',
+     'if (armed & Server::kClimonTracking) {\n'
+     '        if (op.cmd_name().eq_icase("flushall") || op.cmd_name().eq_icase("flushdb"))\n'
+     '            tracking_broadcast_flush();\n        ClimonConn* state',
+     'src/cmd/climon.cc', 'no flush invalidation before owner work was published'),
+    ('flush-missing', 'flush', 'src/cmd/climon.cc',
+     'void IoLoop::climon_flush_completed(Op& op) {\n',
+     'void IoLoop::climon_flush_completed(Op& op) {\n    return; // omit completed invalidation\n',
+     'src/cmd/climon.cc', 'completed flush invalidates foreign tracking IO'),
+    ('output-untracked', 'output', 'src/net/wb.h',
+     'else conn.stop_obuf_tracking();',
+     'else {} // deliberately leave accounting armed for uncounted bytes',
+     'tests/netcmd_unit.cc', None),
+    ('output-deferred', 'output', 'src/core/io_loop.h',
+     'uint64_t used = c->obuf_bytes() + wb_.deferred_output_bytes(*c);',
+     'uint64_t used = c->obuf_bytes(); // deliberately omit deferred storage',
+     'tests/netcmd_unit.cc', 'hard limit fired before Done'),
+]
+results = []
+for name, case, source, before, after, tu, diagnostic in variants:
+    tree = out / 'mutants' / name
+    tree.mkdir(parents=True)
+    shutil.copytree(root / 'src', tree / 'src')
+    (tree / 'third_party').symlink_to(root / 'third_party', target_is_directory=True)
+    (tree / 'tests').mkdir()
+    for test in ('netcmd_unit.cc', 'netcmd_unit.h'):
+        shutil.copy2(root / 'tests' / test, tree / 'tests' / test)
+    target = tree / source
+    original = target.read_text()
+    assert original.count(before) == (2 if name == 'output-untracked' else 1)
+    target.write_text(original.replace(before, after))
+    old_object = 'build/tests/netcmd_unit.o' if tu.startswith('tests/') else 'build/src/cmd/climon.o'
+    compile_cmd = shlex.split(next(line for line in recipes if f' -c {tu} -o {old_object}' in line))
+    new_object = str(tree / 'mutant.o')
+    compile_cmd = [str(tree / tu) if arg == tu else new_object if arg == old_object
+                   else f'-I{tree}' if arg == '-I.' else arg for arg in compile_cmd]
+    binary = str(tree / 'netcmd-unit')
+    link_cmd = [new_object if arg == old_object else binary if arg == 'build/netcmd-unit' else arg
+                for arg in link]
+    print(f'building {name}', flush=True)
+    (tree / 'commands.json').write_text(json.dumps([compile_cmd, link_cmd], indent=2) + '\n')
+    with (tree / 'build.log').open('w') as log:
+        subprocess.run(['taskset', '-c', '44-55', *compile_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=300)
+        subprocess.run(['taskset', '-c', '44-55', *link_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=120)
+    for run in range(1, 6):
+        command = ['taskset', '-c', '44-51', 'timeout', '60', binary, case]
+        r = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+        (tree / f'run-{run}.log').write_text(r.stdout)
+        expected = r.returncode == -signal.SIGABRT if diagnostic is None else (
+            r.returncode == 1 and f'FAIL: {diagnostic}' in r.stdout)
+        results.append({'variant':name, 'case':case, 'run':run, 'returncode':r.returncode,
+                        'expected_failure':expected, 'output':r.stdout, 'command':command})
+        assert expected, (name, r.returncode, r.stdout)
+    print(f'{name}: rejected 5/5; {r.stdout.strip()}', flush=True)
+    (out / 'negative-results.json').write_text(json.dumps(results, indent=2) + '\n')
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
new file mode 100644
index 0000000000000000000000000000000000000000..bc72f719b1e93668f4c671000b1d5bad75da7ac0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
@@ -0,0 +1,16 @@
+#define _GNU_SOURCE
+#include <dlfcn.h>
+#include <stdio.h>
+#include <string.h>
+
+/* Read-only topology simulation: the process remains pinned to real CPUs 44-51.
+ * These serverless fixtures never launch workers. Only sibling metadata changes. */
+FILE *fopen(const char *path, const char *mode) {
+    static char pairs[2][7] = {"44-45\n", "46-47\n"};
+    int cpu = -1, end = 0;
+    if (sscanf(path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list%n",
+               &cpu, &end) == 1 && end && !path[end] && cpu >= 44 && cpu <= 47)
+        return fmemopen(pairs[(cpu - 44) / 2], 6, "r");
+    FILE *(*real_fopen)(const char *, const char *) = dlsym(RTLD_NEXT, "fopen");
+    return real_fopen(path, mode);
+}
diff --git a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
index 6d11e9ce5b2552d2f792115b3076459979edbe39..c04a11b7fc4a6c500632d4e2c48b7949c2dc8c6e
--- a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
+++ b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
@@ -60,7 +60,9 @@
 
     static void notify_retry() {
         Server server;
-        Config cfg; cfg.even_ifid = 1; cfg.even_ex = 1; cfg.shards = 16;
+        // The gate runs this serverless row without taskset. Automatic SMT placement requires
+        // whole sibling pairs for BOTH roles; only one IO is a tracking destination below.
+        Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "notification topology");
         server.threads_.resize(server.placement().total_threads());
         for (uint32_t i = 0; i < server.threads_.size(); ++i) {
@@ -94,7 +96,8 @@
     }
 
     static void flush() {
-        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 1; cfg.shards = 16;
+        // Two IOs still prove the foreign delivery; an even EX count also admits SMT placement.
+        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "flush topology");
         const auto& ios = server.placement().ifid_threads();
         check(ios.size() == 2, "foreign tracking IO exists");
@@ -136,7 +139,16 @@
         Server server; ThreadCtx self; IoLoop loop;
         loop.srv_ = &server; loop.self_ = &self;
         sender.bind(loop.wb_);
-        server.live_obuf_normal_hard_.store(8); server.client_obuf_armed_.store(true);
+        // Mirror Server::init's mailbox initialization without starting workers or opening rings.
+        // cx-waits made raw live-field stores writer-only; IO reads the published snapshot.
+        server.live_config_committed_ = server.capture_live_config(server.live_config_version_.load());
+        server.live_config_mailboxes_ = std::make_unique<LiveConfigMailbox[]>(server.nthreads() + 1);
+        server.live_config_mailboxes_[self.id()].init(server.live_config_committed_);
+        ClientOutputBufferLimits limits{}; limits.normal.hard_bytes = 8;
+        server.set_client_output_buffer_limits(limits);
+        check(server.client_obuf_armed() &&
+              server.client_limits_snapshot(self.id()).normal.hard_bytes == 8,
+              "hard limit published to the IO config mailbox");
         Client held(-1); held.set_id(1);
         check(held.rob().acquire(), "held ordinary operation"); held.rob().publish();
         check(!held.blocked(), "ordinary operation, not blocking exception");

exec
/bin/bash -lc 'taskset -c 52-55 python3 -u build/netcmdfix/live_matrix.py > build/netcmdfix/live-resume-progress.log 2>&1' in /home/user/Projects/cx-final-netcmdfix
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
new file mode 100644
index 0000000000000000000000000000000000000000..295ae00928add7346879af86ca8bfdb274f46beb
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
@@ -0,0 +1,85 @@
+"""Run only tests/netcmd.py; each attempt owns one server PID and private data directory."""
+import itertools
+import json
+from pathlib import Path
+import resource
+import socket
+import subprocess
+import sys
+import time
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix/live'
+out.mkdir(exist_ok=True)
+sys.path.insert(0, str(root / 'tests'))
+from _lib import Conn
+
+results_file = out / 'results.json'
+results = json.loads(results_file.read_text()) if results_file.exists() else []
+cells = itertools.product(('split', 'fused'), (0, 1), (0, 1), (0, 1))
+for cell, (mode, atomic, read_local, overlap) in enumerate(cells):
+    # 8590 belongs to an existing Garnet PID; resume the remaining cells on unused ports.
+    port = 8580 + cell if cell < 10 else 8596 + cell - 10
+    name = f'{mode}-a{atomic}-rl{read_local}-overlap{overlap}'
+    for attempt in range(1, 4):
+        if any(r['cell'] == name and r['attempt'] == attempt for r in results):
+            continue  # preserve completed attempts, including any failure, when resuming
+        directory = out / name / str(attempt)
+        directory.mkdir(parents=True, exist_ok=True)
+        with socket.socket() as guard:
+            guard.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
+            guard.bind(('127.0.0.1', port))
+        command = ['taskset', '-c', '44-51', './build/tomokv', '--bind', '127.0.0.1',
+                   '--port', str(port), '--shards', '16', '--thread-mode', mode,
+                   '--atomic', str(atomic), '--read-local', str(read_local),
+                   '--overlap', str(overlap), '--enable-debug-command', 'yes',
+                   '--dir', str(directory)]
+        if mode == 'split':
+            command += ['--ratio', '6:2']
+        record = dict(cell=name, attempt=attempt, port=port, command=command)
+        with (directory / 'server.log').open('w') as log:
+            server = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
+            record['pid'] = server.pid
+            try:
+                deadline = time.monotonic() + 30
+                while True:
+                    assert server.poll() is None, f'{name}: server exited {server.returncode}'
+                    try:
+                        connection = Conn('127.0.0.1', port, timeout=2)
+                        break
+                    except OSError:
+                        assert time.monotonic() < deadline, f'{name}: boot timeout'
+                        time.sleep(0.05)
+                try:
+                    assert connection.cmd('PING') == b'PONG'
+                    info = connection.cmd('INFO', 'server').decode()
+                    (directory / 'info-server.log').write_text(info)
+                    fields = dict(line.split(':', 1) for line in info.splitlines() if ':' in line)
+                    assert fields['thread_mode'] == ('2s' if mode == 'split' else '1s'), fields
+                    assert fields['read_local'] == str(read_local), fields
+                finally:
+                    connection.close()
+                test = ['taskset', '-c', '52-55', 'timeout', '60', 'python3', '-u',
+                        'tests/netcmd.py', '127.0.0.1', str(port)]
+                result = subprocess.run(test, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+                (directory / 'battery.log').write_text(result.stdout)
+                record.update(returncode=result.returncode, test_command=test)
+            except Exception as exc:
+                record.update(returncode=1, error=repr(exc))
+            finally:
+                if server.poll() is None:
+                    server.terminate()
+                    try:
+                        server.wait(timeout=20)
+                    except subprocess.TimeoutExpired:
+                        server.kill()
+                        server.wait()
+                record['server_exit'] = server.returncode
+        results.append(record)
+        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
+        if record['returncode'] or record['server_exit']:
+            print(f'FAIL {name} attempt {attempt}: {record}', flush=True)
+    cell_results = [r for r in results if r['cell'] == name]
+    print(f'{name}: {sum(r["returncode"] == 0 and r["server_exit"] == 0 for r in cell_results)}/3', flush=True)
+raise SystemExit(any(r['returncode'] or r['server_exit'] for r in results))
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
new file mode 100644
index 0000000000000000000000000000000000000000..b6494711491bb3710f859aa5fb7e295a4e3b093b
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
@@ -0,0 +1,77 @@
+"""Disposable source copies only; never modifies the production sources or objects."""
+import json
+from pathlib import Path
+import resource
+import shlex
+import shutil
+import signal
+import subprocess
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix'
+recipes = subprocess.check_output(['make', '-n', '-B', 'build/netcmd-unit'], text=True).splitlines()
+link = shlex.split(next(line for line in recipes if ' -o build/netcmd-unit ' in line))
+variants = [
+    ('notify-drop', 'notify-retry', 'src/cmd/notify.inc',
+     'return work;  // keep this target and the keyless record for the next pass',
+     'slot->keyless.pop_front(); return work + 1;  // deliberately lose the invalidation',
+     'tests/netcmd_unit.cc', 'failed marker post is incomplete work'),
+    ('flush-early', 'flush', 'src/cmd/climon.cc',
+     'if (armed & Server::kClimonTracking) {\n        ClimonConn* state',
+     'if (armed & Server::kClimonTracking) {\n'
+     '        if (op.cmd_name().eq_icase("flushall") || op.cmd_name().eq_icase("flushdb"))\n'
+     '            tracking_broadcast_flush();\n        ClimonConn* state',
+     'src/cmd/climon.cc', 'no flush invalidation before owner work was published'),
+    ('flush-missing', 'flush', 'src/cmd/climon.cc',
+     'void IoLoop::climon_flush_completed(Op& op) {\n',
+     'void IoLoop::climon_flush_completed(Op& op) {\n    return; // omit completed invalidation\n',
+     'src/cmd/climon.cc', 'completed flush invalidates foreign tracking IO'),
+    ('output-untracked', 'output', 'src/net/wb.h',
+     'else conn.stop_obuf_tracking();',
+     'else {} // deliberately leave accounting armed for uncounted bytes',
+     'tests/netcmd_unit.cc', None),
+    ('output-deferred', 'output', 'src/core/io_loop.h',
+     'uint64_t used = c->obuf_bytes() + wb_.deferred_output_bytes(*c);',
+     'uint64_t used = c->obuf_bytes(); // deliberately omit deferred storage',
+     'tests/netcmd_unit.cc', 'hard limit fired before Done'),
+]
+results = []
+for name, case, source, before, after, tu, diagnostic in variants:
+    tree = out / 'mutants' / name
+    tree.mkdir(parents=True)
+    shutil.copytree(root / 'src', tree / 'src')
+    (tree / 'third_party').symlink_to(root / 'third_party', target_is_directory=True)
+    (tree / 'tests').mkdir()
+    for test in ('netcmd_unit.cc', 'netcmd_unit.h'):
+        shutil.copy2(root / 'tests' / test, tree / 'tests' / test)
+    target = tree / source
+    original = target.read_text()
+    assert original.count(before) == (2 if name == 'output-untracked' else 1)
+    target.write_text(original.replace(before, after))
+    old_object = 'build/tests/netcmd_unit.o' if tu.startswith('tests/') else 'build/src/cmd/climon.o'
+    compile_cmd = shlex.split(next(line for line in recipes if f' -c {tu} -o {old_object}' in line))
+    new_object = str(tree / 'mutant.o')
+    compile_cmd = [str(tree / tu) if arg == tu else new_object if arg == old_object
+                   else f'-I{tree}' if arg == '-I.' else arg for arg in compile_cmd]
+    binary = str(tree / 'netcmd-unit')
+    link_cmd = [new_object if arg == old_object else binary if arg == 'build/netcmd-unit' else arg
+                for arg in link]
+    print(f'building {name}', flush=True)
+    (tree / 'commands.json').write_text(json.dumps([compile_cmd, link_cmd], indent=2) + '\n')
+    with (tree / 'build.log').open('w') as log:
+        subprocess.run(['taskset', '-c', '44-55', *compile_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=300)
+        subprocess.run(['taskset', '-c', '44-55', *link_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=120)
+    for run in range(1, 6):
+        command = ['taskset', '-c', '44-51', 'timeout', '60', binary, case]
+        r = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+        (tree / f'run-{run}.log').write_text(r.stdout)
+        expected = r.returncode == -signal.SIGABRT if diagnostic is None else (
+            r.returncode == 1 and f'FAIL: {diagnostic}' in r.stdout)
+        results.append({'variant':name, 'case':case, 'run':run, 'returncode':r.returncode,
+                        'expected_failure':expected, 'output':r.stdout, 'command':command})
+        assert expected, (name, r.returncode, r.stdout)
+    print(f'{name}: rejected 5/5; {r.stdout.strip()}', flush=True)
+    (out / 'negative-results.json').write_text(json.dumps(results, indent=2) + '\n')
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
new file mode 100644
index 0000000000000000000000000000000000000000..bc72f719b1e93668f4c671000b1d5bad75da7ac0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
@@ -0,0 +1,16 @@
+#define _GNU_SOURCE
+#include <dlfcn.h>
+#include <stdio.h>
+#include <string.h>
+
+/* Read-only topology simulation: the process remains pinned to real CPUs 44-51.
+ * These serverless fixtures never launch workers. Only sibling metadata changes. */
+FILE *fopen(const char *path, const char *mode) {
+    static char pairs[2][7] = {"44-45\n", "46-47\n"};
+    int cpu = -1, end = 0;
+    if (sscanf(path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list%n",
+               &cpu, &end) == 1 && end && !path[end] && cpu >= 44 && cpu <= 47)
+        return fmemopen(pairs[(cpu - 44) / 2], 6, "r");
+    FILE *(*real_fopen)(const char *, const char *) = dlsym(RTLD_NEXT, "fopen");
+    return real_fopen(path, mode);
+}
diff --git a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
index 6d11e9ce5b2552d2f792115b3076459979edbe39..c04a11b7fc4a6c500632d4e2c48b7949c2dc8c6e
--- a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
+++ b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
@@ -60,7 +60,9 @@
 
     static void notify_retry() {
         Server server;
-        Config cfg; cfg.even_ifid = 1; cfg.even_ex = 1; cfg.shards = 16;
+        // The gate runs this serverless row without taskset. Automatic SMT placement requires
+        // whole sibling pairs for BOTH roles; only one IO is a tracking destination below.
+        Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "notification topology");
         server.threads_.resize(server.placement().total_threads());
         for (uint32_t i = 0; i < server.threads_.size(); ++i) {
@@ -94,7 +96,8 @@
     }
 
     static void flush() {
-        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 1; cfg.shards = 16;
+        // Two IOs still prove the foreign delivery; an even EX count also admits SMT placement.
+        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "flush topology");
         const auto& ios = server.placement().ifid_threads();
         check(ios.size() == 2, "foreign tracking IO exists");
@@ -136,7 +139,16 @@
         Server server; ThreadCtx self; IoLoop loop;
         loop.srv_ = &server; loop.self_ = &self;
         sender.bind(loop.wb_);
-        server.live_obuf_normal_hard_.store(8); server.client_obuf_armed_.store(true);
+        // Mirror Server::init's mailbox initialization without starting workers or opening rings.
+        // cx-waits made raw live-field stores writer-only; IO reads the published snapshot.
+        server.live_config_committed_ = server.capture_live_config(server.live_config_version_.load());
+        server.live_config_mailboxes_ = std::make_unique<LiveConfigMailbox[]>(server.nthreads() + 1);
+        server.live_config_mailboxes_[self.id()].init(server.live_config_committed_);
+        ClientOutputBufferLimits limits{}; limits.normal.hard_bytes = 8;
+        server.set_client_output_buffer_limits(limits);
+        check(server.client_obuf_armed() &&
+              server.client_limits_snapshot(self.id()).normal.hard_bytes == 8,
+              "hard limit published to the IO config mailbox");
         Client held(-1); held.set_id(1);
         check(held.rob().acquire(), "held ordinary operation"); held.rob().publish();
         check(!held.blocked(), "ordinary operation, not blocking exception");

 succeeded in 1387ms:
codex
All five broken variants were rejected in all five runs. The live battery has passed 30/30 attempts across split and fused configurations so far. Its port preflight found an existing Garnet process on 8590, so I moved the remaining runs to other ports within the assigned range.

exec
/bin/bash -lc "ulimit -c 0
taskset -c 44-51 gdb -q -batch -ex 'set debuginfod enabled off' -ex run -ex 'bt 10' --args ./build/netcmdfix/mutants/output-untracked/netcmd-unit output > build/netcmdfix/negative-output-untracked-gdb.log 2>&1
cat build/netcmdfix/negative-output-untracked-gdb.log
 tail -6 build/netcmdfix/live-resume-progress.log" in /home/user/Projects/cx-final-netcmdfix
 succeeded in 714ms:
[Thread debugging using libthread_db enabled]
Using host libthread_db library "/lib/x86_64-linux-gnu/libthread_db.so.1".

Program received signal SIGABRT, Aborted.
__pthread_kill_implementation (no_tid=0, signo=6, threadid=<optimized out>) at ./nptl/pthread_kill.c:44
warning: 44	./nptl/pthread_kill.c: No such file or directory
#0  __pthread_kill_implementation (no_tid=0, signo=6, threadid=<optimized out>) at ./nptl/pthread_kill.c:44
#1  __pthread_kill_internal (signo=6, threadid=<optimized out>) at ./nptl/pthread_kill.c:78
#2  __GI___pthread_kill (threadid=<optimized out>, signo=signo@entry=6) at ./nptl/pthread_kill.c:89
#3  0x00007ffff6e4527e in __GI_raise (sig=sig@entry=6) at ../sysdeps/posix/raise.c:26
#4  0x00007ffff6e288ff in __GI_abort () at ./stdlib/abort.c:79
#5  0x00005555556209ae in tomo::Client::commit_write (n=7, this=0x7ffffffe0c00) at /home/user/Projects/cx-final-netcmdfix/build/netcmdfix/mutants/output-untracked/src/cmd/../core/../net/conn.h:419
#6  tomo::Client::commit_write (n=7, this=0x7ffffffe0c00) at /home/user/Projects/cx-final-netcmdfix/build/netcmdfix/mutants/output-untracked/src/cmd/../core/../net/conn.h:416
#7  tomo::NetcmdRegression::output () at /home/user/Projects/cx-final-netcmdfix/build/netcmdfix/mutants/output-untracked/tests/netcmd_unit.cc:136
#8  0x000055555557892a in main (argc=<optimized out>, argv=0x7fffffffd8a8) at /home/user/Projects/cx-final-netcmdfix/build/netcmdfix/mutants/output-untracked/tests/netcmd_unit.cc:266
fused-a0-rl1-overlap0: 3/3
fused-a0-rl1-overlap1: 3/3
fused-a1-rl0-overlap0: 3/3
fused-a1-rl0-overlap1: 3/3
fused-a1-rl1-overlap0: 3/3
fused-a1-rl1-overlap1: 3/3

diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
new file mode 100644
index 0000000000000000000000000000000000000000..295ae00928add7346879af86ca8bfdb274f46beb
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
@@ -0,0 +1,85 @@
+"""Run only tests/netcmd.py; each attempt owns one server PID and private data directory."""
+import itertools
+import json
+from pathlib import Path
+import resource
+import socket
+import subprocess
+import sys
+import time
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix/live'
+out.mkdir(exist_ok=True)
+sys.path.insert(0, str(root / 'tests'))
+from _lib import Conn
+
+results_file = out / 'results.json'
+results = json.loads(results_file.read_text()) if results_file.exists() else []
+cells = itertools.product(('split', 'fused'), (0, 1), (0, 1), (0, 1))
+for cell, (mode, atomic, read_local, overlap) in enumerate(cells):
+    # 8590 belongs to an existing Garnet PID; resume the remaining cells on unused ports.
+    port = 8580 + cell if cell < 10 else 8596 + cell - 10
+    name = f'{mode}-a{atomic}-rl{read_local}-overlap{overlap}'
+    for attempt in range(1, 4):
+        if any(r['cell'] == name and r['attempt'] == attempt for r in results):
+            continue  # preserve completed attempts, including any failure, when resuming
+        directory = out / name / str(attempt)
+        directory.mkdir(parents=True, exist_ok=True)
+        with socket.socket() as guard:
+            guard.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
+            guard.bind(('127.0.0.1', port))
+        command = ['taskset', '-c', '44-51', './build/tomokv', '--bind', '127.0.0.1',
+                   '--port', str(port), '--shards', '16', '--thread-mode', mode,
+                   '--atomic', str(atomic), '--read-local', str(read_local),
+                   '--overlap', str(overlap), '--enable-debug-command', 'yes',
+                   '--dir', str(directory)]
+        if mode == 'split':
+            command += ['--ratio', '6:2']
+        record = dict(cell=name, attempt=attempt, port=port, command=command)
+        with (directory / 'server.log').open('w') as log:
+            server = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
+            record['pid'] = server.pid
+            try:
+                deadline = time.monotonic() + 30
+                while True:
+                    assert server.poll() is None, f'{name}: server exited {server.returncode}'
+                    try:
+                        connection = Conn('127.0.0.1', port, timeout=2)
+                        break
+                    except OSError:
+                        assert time.monotonic() < deadline, f'{name}: boot timeout'
+                        time.sleep(0.05)
+                try:
+                    assert connection.cmd('PING') == b'PONG'
+                    info = connection.cmd('INFO', 'server').decode()
+                    (directory / 'info-server.log').write_text(info)
+                    fields = dict(line.split(':', 1) for line in info.splitlines() if ':' in line)
+                    assert fields['thread_mode'] == ('2s' if mode == 'split' else '1s'), fields
+                    assert fields['read_local'] == str(read_local), fields
+                finally:
+                    connection.close()
+                test = ['taskset', '-c', '52-55', 'timeout', '60', 'python3', '-u',
+                        'tests/netcmd.py', '127.0.0.1', str(port)]
+                result = subprocess.run(test, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+                (directory / 'battery.log').write_text(result.stdout)
+                record.update(returncode=result.returncode, test_command=test)
+            except Exception as exc:
+                record.update(returncode=1, error=repr(exc))
+            finally:
+                if server.poll() is None:
+                    server.terminate()
+                    try:
+                        server.wait(timeout=20)
+                    except subprocess.TimeoutExpired:
+                        server.kill()
+                        server.wait()
+                record['server_exit'] = server.returncode
+        results.append(record)
+        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
+        if record['returncode'] or record['server_exit']:
+            print(f'FAIL {name} attempt {attempt}: {record}', flush=True)
+    cell_results = [r for r in results if r['cell'] == name]
+    print(f'{name}: {sum(r["returncode"] == 0 and r["server_exit"] == 0 for r in cell_results)}/3', flush=True)
+raise SystemExit(any(r['returncode'] or r['server_exit'] for r in results))
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
new file mode 100644
index 0000000000000000000000000000000000000000..b6494711491bb3710f859aa5fb7e295a4e3b093b
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
@@ -0,0 +1,77 @@
+"""Disposable source copies only; never modifies the production sources or objects."""
+import json
+from pathlib import Path
+import resource
+import shlex
+import shutil
+import signal
+import subprocess
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix'
+recipes = subprocess.check_output(['make', '-n', '-B', 'build/netcmd-unit'], text=True).splitlines()
+link = shlex.split(next(line for line in recipes if ' -o build/netcmd-unit ' in line))
+variants = [
+    ('notify-drop', 'notify-retry', 'src/cmd/notify.inc',
+     'return work;  // keep this target and the keyless record for the next pass',
+     'slot->keyless.pop_front(); return work + 1;  // deliberately lose the invalidation',
+     'tests/netcmd_unit.cc', 'failed marker post is incomplete work'),
+    ('flush-early', 'flush', 'src/cmd/climon.cc',
+     'if (armed & Server::kClimonTracking) {\n        ClimonConn* state',
+     'if (armed & Server::kClimonTracking) {\n'
+     '        if (op.cmd_name().eq_icase("flushall") || op.cmd_name().eq_icase("flushdb"))\n'
+     '            tracking_broadcast_flush();\n        ClimonConn* state',
+     'src/cmd/climon.cc', 'no flush invalidation before owner work was published'),
+    ('flush-missing', 'flush', 'src/cmd/climon.cc',
+     'void IoLoop::climon_flush_completed(Op& op) {\n',
+     'void IoLoop::climon_flush_completed(Op& op) {\n    return; // omit completed invalidation\n',
+     'src/cmd/climon.cc', 'completed flush invalidates foreign tracking IO'),
+    ('output-untracked', 'output', 'src/net/wb.h',
+     'else conn.stop_obuf_tracking();',
+     'else {} // deliberately leave accounting armed for uncounted bytes',
+     'tests/netcmd_unit.cc', None),
+    ('output-deferred', 'output', 'src/core/io_loop.h',
+     'uint64_t used = c->obuf_bytes() + wb_.deferred_output_bytes(*c);',
+     'uint64_t used = c->obuf_bytes(); // deliberately omit deferred storage',
+     'tests/netcmd_unit.cc', 'hard limit fired before Done'),
+]
+results = []
+for name, case, source, before, after, tu, diagnostic in variants:
+    tree = out / 'mutants' / name
+    tree.mkdir(parents=True)
+    shutil.copytree(root / 'src', tree / 'src')
+    (tree / 'third_party').symlink_to(root / 'third_party', target_is_directory=True)
+    (tree / 'tests').mkdir()
+    for test in ('netcmd_unit.cc', 'netcmd_unit.h'):
+        shutil.copy2(root / 'tests' / test, tree / 'tests' / test)
+    target = tree / source
+    original = target.read_text()
+    assert original.count(before) == (2 if name == 'output-untracked' else 1)
+    target.write_text(original.replace(before, after))
+    old_object = 'build/tests/netcmd_unit.o' if tu.startswith('tests/') else 'build/src/cmd/climon.o'
+    compile_cmd = shlex.split(next(line for line in recipes if f' -c {tu} -o {old_object}' in line))
+    new_object = str(tree / 'mutant.o')
+    compile_cmd = [str(tree / tu) if arg == tu else new_object if arg == old_object
+                   else f'-I{tree}' if arg == '-I.' else arg for arg in compile_cmd]
+    binary = str(tree / 'netcmd-unit')
+    link_cmd = [new_object if arg == old_object else binary if arg == 'build/netcmd-unit' else arg
+                for arg in link]
+    print(f'building {name}', flush=True)
+    (tree / 'commands.json').write_text(json.dumps([compile_cmd, link_cmd], indent=2) + '\n')
+    with (tree / 'build.log').open('w') as log:
+        subprocess.run(['taskset', '-c', '44-55', *compile_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=300)
+        subprocess.run(['taskset', '-c', '44-55', *link_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=120)
+    for run in range(1, 6):
+        command = ['taskset', '-c', '44-51', 'timeout', '60', binary, case]
+        r = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+        (tree / f'run-{run}.log').write_text(r.stdout)
+        expected = r.returncode == -signal.SIGABRT if diagnostic is None else (
+            r.returncode == 1 and f'FAIL: {diagnostic}' in r.stdout)
+        results.append({'variant':name, 'case':case, 'run':run, 'returncode':r.returncode,
+                        'expected_failure':expected, 'output':r.stdout, 'command':command})
+        assert expected, (name, r.returncode, r.stdout)
+    print(f'{name}: rejected 5/5; {r.stdout.strip()}', flush=True)
+    (out / 'negative-results.json').write_text(json.dumps(results, indent=2) + '\n')
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
new file mode 100644
index 0000000000000000000000000000000000000000..bc72f719b1e93668f4c671000b1d5bad75da7ac0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
@@ -0,0 +1,16 @@
+#define _GNU_SOURCE
+#include <dlfcn.h>
+#include <stdio.h>
+#include <string.h>
+
+/* Read-only topology simulation: the process remains pinned to real CPUs 44-51.
+ * These serverless fixtures never launch workers. Only sibling metadata changes. */
+FILE *fopen(const char *path, const char *mode) {
+    static char pairs[2][7] = {"44-45\n", "46-47\n"};
+    int cpu = -1, end = 0;
+    if (sscanf(path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list%n",
+               &cpu, &end) == 1 && end && !path[end] && cpu >= 44 && cpu <= 47)
+        return fmemopen(pairs[(cpu - 44) / 2], 6, "r");
+    FILE *(*real_fopen)(const char *, const char *) = dlsym(RTLD_NEXT, "fopen");
+    return real_fopen(path, mode);
+}
diff --git a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
index 6d11e9ce5b2552d2f792115b3076459979edbe39..c04a11b7fc4a6c500632d4e2c48b7949c2dc8c6e
--- a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
+++ b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
@@ -60,7 +60,9 @@
 
     static void notify_retry() {
         Server server;
-        Config cfg; cfg.even_ifid = 1; cfg.even_ex = 1; cfg.shards = 16;
+        // The gate runs this serverless row without taskset. Automatic SMT placement requires
+        // whole sibling pairs for BOTH roles; only one IO is a tracking destination below.
+        Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "notification topology");
         server.threads_.resize(server.placement().total_threads());
         for (uint32_t i = 0; i < server.threads_.size(); ++i) {
@@ -94,7 +96,8 @@
     }
 
     static void flush() {
-        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 1; cfg.shards = 16;
+        // Two IOs still prove the foreign delivery; an even EX count also admits SMT placement.
+        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "flush topology");
         const auto& ios = server.placement().ifid_threads();
         check(ios.size() == 2, "foreign tracking IO exists");
@@ -136,7 +139,16 @@
         Server server; ThreadCtx self; IoLoop loop;
         loop.srv_ = &server; loop.self_ = &self;
         sender.bind(loop.wb_);
-        server.live_obuf_normal_hard_.store(8); server.client_obuf_armed_.store(true);
+        // Mirror Server::init's mailbox initialization without starting workers or opening rings.
+        // cx-waits made raw live-field stores writer-only; IO reads the published snapshot.
+        server.live_config_committed_ = server.capture_live_config(server.live_config_version_.load());
+        server.live_config_mailboxes_ = std::make_unique<LiveConfigMailbox[]>(server.nthreads() + 1);
+        server.live_config_mailboxes_[self.id()].init(server.live_config_committed_);
+        ClientOutputBufferLimits limits{}; limits.normal.hard_bytes = 8;
+        server.set_client_output_buffer_limits(limits);
+        check(server.client_obuf_armed() &&
+              server.client_limits_snapshot(self.id()).normal.hard_bytes == 8,
+              "hard limit published to the IO config mailbox");
         Client held(-1); held.set_id(1);
         check(held.rob().acquire(), "held ordinary operation"); held.rob().publish();
         check(!held.blocked(), "ordinary operation, not blocking exception");

apply patch
patch: completed
/home/user/Projects/cx-final-netcmdfix/NETCMDFIX.md
diff --git a//home/user/Projects/cx-final-netcmdfix/NETCMDFIX.md b//home/user/Projects/cx-final-netcmdfix/NETCMDFIX.md
new file mode 100644
index 0000000000000000000000000000000000000000..16724c2e2d3fa8494e027d7d8e12faf9ba097ef4
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/NETCMDFIX.md
@@ -0,0 +1,215 @@
+# Netcmd regression fixes
+
+2026-09-09. Base: `67f623716ca00b49d7cbdb8d33ab6b643b3e87ad` (`cx-final`).
+Worktree: `/home/user/Projects/cx-final-netcmdfix`, branch `codex/netcmdfix-20260909`.
+
+All three verdicts are **(b): invalid test-fixture assumptions**. The notification,
+flush-ordering, and output-limit properties remain required. No existing behavioral
+assertion was removed or relaxed. The patch changes only `tests/netcmd_unit.cc` and
+this report; production code and layouts are unchanged.
+
+## Evidence and changes per row
+
+### `netcmd notify-retry regression` — (b)
+
+The gate drives `build/netcmd-unit notify-retry`, not `tests/notify.py` or
+`tests/netcmd.py`. Unlike the gate's server boots, its serverless unit invocation
+at `tests/gate.sh:396` has **no taskset wrapper**. The saved failure was:
+
+```text
+--ratio: SMT placement requires even logical io and ex counts
+FAIL: notification topology
+```
+
+The erroneous assumption was that `prepare_boot` must accept a `1:1` IO/EX
+topology regardless of the process affinity. `Server::prepare_boot` automatically
+selects SMT placement when discovery finds a complete sibling pair;
+`Placement::build_even_smt` requires an even count for each role. A correct server
+must reject `1:1` in that topology. The failure occurs before the queue is filled
+or the notification code runs.
+
+The unmodified test passed **5/5** on cores 44–51, which contain no complete SMT
+pairs. With simulated sibling metadata it reproduced the exact saved failure
+**5/5**, while execution remained confined to those same cores. This is a topology
+dependency, not a probabilistic queue failure.
+
+Automatic SMT selection came from **cx-knobs**, integrated by `c8e61f646`. That
+commit is already an ancestor of netcmd's test-introduction commit `e94b97100`.
+Thus this assumption was wrong when the test was written; a subsequent overlap,
+read-local, vestigial-code, or resize merge did not introduce it. The original
+netcmd report describes validation on pinned physical cores, which explains why
+those runs did not expose it.
+
+**Change:** use `2:2`, valid with physical cores or complete SMT pairs. The tracking
+mask still selects exactly one IO. The actual producer queue must fill, the failed
+post must report zero completed work, the keyless record must survive, no payload
+may escape the failed post, and the drained retry must deliver exactly the original
+invalidation. Every one of those checks is unchanged.
+
+### `netcmd flush regression` — (b)
+
+This row drives `build/netcmd-unit flush`. Its saved failure was the same SMT
+diagnostic followed by `FAIL: flush topology`. Its erroneous assumption was that
+`prepare_boot` must accept `2:1` under any affinity. The odd EX count violates the
+same existing SMT contract. Attribution is likewise **cx-knobs / an invalid
+netcmd fixture at introduction**, not a new flush-path behavior.
+
+The original test passed **5/5** on cores 44–51 and reproduced the topology
+failure **5/5** under simulated SMT metadata. Neither failure reached dispatch or
+flush completion.
+
+**Change:** use `2:2`. The fixture still asserts exactly two IOs and selects a
+foreign tracking IO. The dispatch gate must produce no invalidation before owner
+completion; after completion, the test must receive exactly one `TrackingFlush`.
+Those assertions and the completion ordering are unchanged. The extra executor
+exists only to satisfy placement; the unit starts no workers.
+
+### `netcmd output regression` — (b)
+
+The original test crashed **5/5**, including at the physical-core geometry. GDB
+identified the failure precisely:
+
+```text
+SIGSEGV
+Server::client_limits_snapshot(reader_tid=0)  src/core/server.h:2225
+IoLoop::client_obuf_check                    src/core/io_loop.h:6005
+NetcmdRegression::output                    tests/netcmd_unit.cc:146 (base)
+```
+
+`live_config_mailboxes_` was null. The erroneous fixture assumption was that a
+default-constructed Server plus direct stores to `live_obuf_normal_hard_` and
+`client_obuf_armed_` supplied all state consumed by `client_obuf_check`.
+
+**cx-waits** changed that contract in `c33228203`, merged here by `818d458ef`.
+Before that merge, `client_limits_snapshot()` collected the mutable live atomics.
+It now reads a committed per-consumer mailbox using the worker ID, without reader
+retries. Production `Server::init` allocates and initializes those mailboxes before
+workers run. The hand-built netcmd fixture bypassed that initialization and wrote
+only the CONFIG producer's private fields. Adding a null fallback or restoring
+the old reader loop in production would conceal the invalid fixture and conflict
+with the intended publication design.
+
+**Change:** initialize the fixture's sole config-consumer mailbox using the same
+committed-value initialization as `Server::init`, then call the real
+`set_client_output_buffer_limits` publisher. A new assertion requires both the
+armed selector and an eight-byte hard limit in the IO's published snapshot before
+the behavioral test proceeds.
+
+The seven-byte uncounted reply must still finish sending without subtraction from
+zero. The separate nine-byte deferred push must still exceed the **unchanged
+eight-byte** limit and close a client with an unfinished ordinary ROB operation.
+All bytes must still exist exclusively in deferred storage, and the operation must
+still be nonblocking. No threshold, timing allowance, or exception was changed.
+
+## Repeated validation
+
+A fresh worktree had no build artifacts. Commands used for the initial clean build:
+
+```sh
+taskset -c 44-55 make clean
+taskset -c 44-55 make -j6 build/netcmd-unit build/tomokv
+```
+
+The changed unit translation unit was then rebuilt. All eight locked layout
+assertions compiled unchanged. Test invocations retained the gate's 60-second
+unit timeout:
+
+```sh
+for row in notify-retry flush output; do
+  for attempt in 1 2 3 4 5; do
+    taskset -c 44-51 timeout 60 ./build/netcmd-unit "$row"
+  done
+done
+```
+
+| Row | PRE physical cores | PRE simulated SMT | POST physical cores | POST simulated SMT |
+|---|---:|---:|---:|---:|
+| notify-retry | 5/5 pass | 0/5 pass | 5/5 pass | 5/5 pass |
+| flush | 5/5 pass | 0/5 pass | 5/5 pass | 5/5 pass |
+| output | 0/5 pass, SIGSEGV | Not run | 5/5 pass | 5/5 pass |
+
+The simulation declares CPUs 44–47 as one L3 domain and uses a disposable
+`fopen` interposer to report sibling pairs 44/45 and 46/47. It changes only sysfs
+read results inside the serverless fixture. Neither actual affinity nor host
+topology is changed, no worker is launched, and no CPU outside 44–55 is used.
+It exercises the real SMT placement implementation. The machine's actual complete
+SMT pairs extend outside the assigned cores, so an actual unpinned rerun was not
+performed. Saved gate diagnostics and the simulated failures agree exactly.
+
+The interposer and exact commands are preserved in
+`build/netcmdfix/smt-sysfs.c` and `pre-smt-results.json` / `post-results.json` under
+that same directory. For example:
+
+```sh
+taskset -c 44-51 timeout 60 env TOMOKV_L3_DOMAINS=44-47 \
+  LD_PRELOAD="$PWD/build/netcmdfix/smt-sysfs.so" ./build/netcmd-unit notify-retry
+```
+
+## Tests reject broken mechanisms
+
+These controls were **built and run**, using disposable copies under
+`build/netcmdfix/mutants/`. They never replaced production sources, objects, or the
+good binaries. Each control was run five times; only the expected assertion or
+identified accounting abort counted as a successful rejection.
+
+| Deliberate break | Required failure observed | Rejections |
+|---|---|---:|
+| In `notify_ex_pass_entry`, pop the keyless record and return `work + 1` when posting its invalidation fails. | `failed marker post is incomplete work` | 5/5 |
+| Restore `tracking_broadcast_flush()` at dispatch in `climon_armed_gate`. | `no flush invalidation before owner work was published` | 5/5 |
+| Return immediately from `climon_flush_completed`, omitting its broadcast. | `completed flush invalidates foreign tracking IO` | 5/5 |
+| Remove the untracked Wb specializations' `stop_obuf_tracking()` calls. | SIGABRT in `Client::commit_write(7)` at `conn.h:419`, confirmed with GDB. | 5/5 |
+| Omit `wb_.deferred_output_bytes(*c)` from `client_obuf_check`'s used-byte total. | `hard limit fired before Done` | 5/5 |
+
+The mutation descriptions above are also the instructions for inducing each
+failure. Exact replacements, compile/link commands, expected diagnostics, and
+per-attempt results are retained in `build/netcmdfix/negative_controls.py`, each
+mutant's `commands.json`, and `build/netcmdfix/negative-results.json`.
+
+## Live integration
+
+The unchanged `tests/netcmd.py` battery also passed **48/48** attempts, with a fresh
+server for each attempt. This includes its real RESP3 tracking invalidation after
+FLUSHALL and the following GET observing the cleared key; this supplements the
+unit's direct completion-hook checks.
+
+| Thread mode | Atomic | Read-local | Overlap 0 | Overlap 1 |
+|---|---:|---:|---:|---:|
+| split | 0 | 0 | 3/3 | 3/3 |
+| split | 0 | 1 | 3/3 | 3/3 |
+| split | 1 | 0 | 3/3 | 3/3 |
+| split | 1 | 1 | 3/3 | 3/3 |
+| fused | 0 | 0 | 3/3 | 3/3 |
+| fused | 0 | 1 | 3/3 | 3/3 |
+| fused | 1 | 0 | 3/3 | 3/3 |
+| fused | 1 | 1 | 3/3 | 3/3 |
+
+Servers used io_uring, cores **44–51**, **16 shards**, and split ratio **6:2**.
+Fused boots omitted `--ratio`. Battery clients used cores **52–55**. Each boot's
+INFO response was saved and checked for its thread mode and effective read-local
+setting. These are netcmd integration passes under those configurations, not a
+measurement of read-local hit coverage or performance.
+
+Ports used were **8580–8589 and 8596–8601**. A preflight found a pre-existing Garnet
+listener on 8590 before starting any test there; the remaining cells resumed on
+unused permitted ports. That listener was neither contacted nor stopped. All 48
+owned server PIDs exited cleanly with status zero. The harness stopped only its
+own PIDs and never used a pattern kill.
+
+`build/netcmdfix/live_matrix.py`, `live/results.json`, and each attempt's
+`server.log`, `info-server.log`, and `battery.log` preserve the commands and evidence.
+Build logs, original failure logs, both debugger traces, and binary SHA-256 values
+are also under `build/netcmdfix/`. These are local, ignored validation artifacts.
+
+## Scope and remaining failures
+
+**No failure among the three requested rows remains unattributed.** The unchanged
+notification, flush hook, and Wb implementations are identical to the netcmd merge
+`821d207b1`; the relevant subsequent production change is the cx-waits configuration
+reader used by the output fixture. The live matrix also covers both restored
+overlap schedules and the read-local option in both thread modes.
+
+No row was added, deleted, or moved. The three rows remain above the quick-tier
+exit; the gate-count delta is **0 quick / 0 full**. `tests/gate.sh`, including
+`EXPECT_QUICK` and `EXPECT_FULL`, was not edited. No full gate or benchmark was run.
+There is no performance claim. The user's changes in the original `cx-final`
+worktree were left untouched.
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
new file mode 100644
index 0000000000000000000000000000000000000000..295ae00928add7346879af86ca8bfdb274f46beb
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
@@ -0,0 +1,85 @@
+"""Run only tests/netcmd.py; each attempt owns one server PID and private data directory."""
+import itertools
+import json
+from pathlib import Path
+import resource
+import socket
+import subprocess
+import sys
+import time
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix/live'
+out.mkdir(exist_ok=True)
+sys.path.insert(0, str(root / 'tests'))
+from _lib import Conn
+
+results_file = out / 'results.json'
+results = json.loads(results_file.read_text()) if results_file.exists() else []
+cells = itertools.product(('split', 'fused'), (0, 1), (0, 1), (0, 1))
+for cell, (mode, atomic, read_local, overlap) in enumerate(cells):
+    # 8590 belongs to an existing Garnet PID; resume the remaining cells on unused ports.
+    port = 8580 + cell if cell < 10 else 8596 + cell - 10
+    name = f'{mode}-a{atomic}-rl{read_local}-overlap{overlap}'
+    for attempt in range(1, 4):
+        if any(r['cell'] == name and r['attempt'] == attempt for r in results):
+            continue  # preserve completed attempts, including any failure, when resuming
+        directory = out / name / str(attempt)
+        directory.mkdir(parents=True, exist_ok=True)
+        with socket.socket() as guard:
+            guard.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
+            guard.bind(('127.0.0.1', port))
+        command = ['taskset', '-c', '44-51', './build/tomokv', '--bind', '127.0.0.1',
+                   '--port', str(port), '--shards', '16', '--thread-mode', mode,
+                   '--atomic', str(atomic), '--read-local', str(read_local),
+                   '--overlap', str(overlap), '--enable-debug-command', 'yes',
+                   '--dir', str(directory)]
+        if mode == 'split':
+            command += ['--ratio', '6:2']
+        record = dict(cell=name, attempt=attempt, port=port, command=command)
+        with (directory / 'server.log').open('w') as log:
+            server = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
+            record['pid'] = server.pid
+            try:
+                deadline = time.monotonic() + 30
+                while True:
+                    assert server.poll() is None, f'{name}: server exited {server.returncode}'
+                    try:
+                        connection = Conn('127.0.0.1', port, timeout=2)
+                        break
+                    except OSError:
+                        assert time.monotonic() < deadline, f'{name}: boot timeout'
+                        time.sleep(0.05)
+                try:
+                    assert connection.cmd('PING') == b'PONG'
+                    info = connection.cmd('INFO', 'server').decode()
+                    (directory / 'info-server.log').write_text(info)
+                    fields = dict(line.split(':', 1) for line in info.splitlines() if ':' in line)
+                    assert fields['thread_mode'] == ('2s' if mode == 'split' else '1s'), fields
+                    assert fields['read_local'] == str(read_local), fields
+                finally:
+                    connection.close()
+                test = ['taskset', '-c', '52-55', 'timeout', '60', 'python3', '-u',
+                        'tests/netcmd.py', '127.0.0.1', str(port)]
+                result = subprocess.run(test, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+                (directory / 'battery.log').write_text(result.stdout)
+                record.update(returncode=result.returncode, test_command=test)
+            except Exception as exc:
+                record.update(returncode=1, error=repr(exc))
+            finally:
+                if server.poll() is None:
+                    server.terminate()
+                    try:
+                        server.wait(timeout=20)
+                    except subprocess.TimeoutExpired:
+                        server.kill()
+                        server.wait()
+                record['server_exit'] = server.returncode
+        results.append(record)
+        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
+        if record['returncode'] or record['server_exit']:
+            print(f'FAIL {name} attempt {attempt}: {record}', flush=True)
+    cell_results = [r for r in results if r['cell'] == name]
+    print(f'{name}: {sum(r["returncode"] == 0 and r["server_exit"] == 0 for r in cell_results)}/3', flush=True)
+raise SystemExit(any(r['returncode'] or r['server_exit'] for r in results))
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
new file mode 100644
index 0000000000000000000000000000000000000000..b6494711491bb3710f859aa5fb7e295a4e3b093b
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
@@ -0,0 +1,77 @@
+"""Disposable source copies only; never modifies the production sources or objects."""
+import json
+from pathlib import Path
+import resource
+import shlex
+import shutil
+import signal
+import subprocess
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix'
+recipes = subprocess.check_output(['make', '-n', '-B', 'build/netcmd-unit'], text=True).splitlines()
+link = shlex.split(next(line for line in recipes if ' -o build/netcmd-unit ' in line))
+variants = [
+    ('notify-drop', 'notify-retry', 'src/cmd/notify.inc',
+     'return work;  // keep this target and the keyless record for the next pass',
+     'slot->keyless.pop_front(); return work + 1;  // deliberately lose the invalidation',
+     'tests/netcmd_unit.cc', 'failed marker post is incomplete work'),
+    ('flush-early', 'flush', 'src/cmd/climon.cc',
+     'if (armed & Server::kClimonTracking) {\n        ClimonConn* state',
+     'if (armed & Server::kClimonTracking) {\n'
+     '        if (op.cmd_name().eq_icase("flushall") || op.cmd_name().eq_icase("flushdb"))\n'
+     '            tracking_broadcast_flush();\n        ClimonConn* state',
+     'src/cmd/climon.cc', 'no flush invalidation before owner work was published'),
+    ('flush-missing', 'flush', 'src/cmd/climon.cc',
+     'void IoLoop::climon_flush_completed(Op& op) {\n',
+     'void IoLoop::climon_flush_completed(Op& op) {\n    return; // omit completed invalidation\n',
+     'src/cmd/climon.cc', 'completed flush invalidates foreign tracking IO'),
+    ('output-untracked', 'output', 'src/net/wb.h',
+     'else conn.stop_obuf_tracking();',
+     'else {} // deliberately leave accounting armed for uncounted bytes',
+     'tests/netcmd_unit.cc', None),
+    ('output-deferred', 'output', 'src/core/io_loop.h',
+     'uint64_t used = c->obuf_bytes() + wb_.deferred_output_bytes(*c);',
+     'uint64_t used = c->obuf_bytes(); // deliberately omit deferred storage',
+     'tests/netcmd_unit.cc', 'hard limit fired before Done'),
+]
+results = []
+for name, case, source, before, after, tu, diagnostic in variants:
+    tree = out / 'mutants' / name
+    tree.mkdir(parents=True)
+    shutil.copytree(root / 'src', tree / 'src')
+    (tree / 'third_party').symlink_to(root / 'third_party', target_is_directory=True)
+    (tree / 'tests').mkdir()
+    for test in ('netcmd_unit.cc', 'netcmd_unit.h'):
+        shutil.copy2(root / 'tests' / test, tree / 'tests' / test)
+    target = tree / source
+    original = target.read_text()
+    assert original.count(before) == (2 if name == 'output-untracked' else 1)
+    target.write_text(original.replace(before, after))
+    old_object = 'build/tests/netcmd_unit.o' if tu.startswith('tests/') else 'build/src/cmd/climon.o'
+    compile_cmd = shlex.split(next(line for line in recipes if f' -c {tu} -o {old_object}' in line))
+    new_object = str(tree / 'mutant.o')
+    compile_cmd = [str(tree / tu) if arg == tu else new_object if arg == old_object
+                   else f'-I{tree}' if arg == '-I.' else arg for arg in compile_cmd]
+    binary = str(tree / 'netcmd-unit')
+    link_cmd = [new_object if arg == old_object else binary if arg == 'build/netcmd-unit' else arg
+                for arg in link]
+    print(f'building {name}', flush=True)
+    (tree / 'commands.json').write_text(json.dumps([compile_cmd, link_cmd], indent=2) + '\n')
+    with (tree / 'build.log').open('w') as log:
+        subprocess.run(['taskset', '-c', '44-55', *compile_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=300)
+        subprocess.run(['taskset', '-c', '44-55', *link_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=120)
+    for run in range(1, 6):
+        command = ['taskset', '-c', '44-51', 'timeout', '60', binary, case]
+        r = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+        (tree / f'run-{run}.log').write_text(r.stdout)
+        expected = r.returncode == -signal.SIGABRT if diagnostic is None else (
+            r.returncode == 1 and f'FAIL: {diagnostic}' in r.stdout)
+        results.append({'variant':name, 'case':case, 'run':run, 'returncode':r.returncode,
+                        'expected_failure':expected, 'output':r.stdout, 'command':command})
+        assert expected, (name, r.returncode, r.stdout)
+    print(f'{name}: rejected 5/5; {r.stdout.strip()}', flush=True)
+    (out / 'negative-results.json').write_text(json.dumps(results, indent=2) + '\n')
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
new file mode 100644
index 0000000000000000000000000000000000000000..bc72f719b1e93668f4c671000b1d5bad75da7ac0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
@@ -0,0 +1,16 @@
+#define _GNU_SOURCE
+#include <dlfcn.h>
+#include <stdio.h>
+#include <string.h>
+
+/* Read-only topology simulation: the process remains pinned to real CPUs 44-51.
+ * These serverless fixtures never launch workers. Only sibling metadata changes. */
+FILE *fopen(const char *path, const char *mode) {
+    static char pairs[2][7] = {"44-45\n", "46-47\n"};
+    int cpu = -1, end = 0;
+    if (sscanf(path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list%n",
+               &cpu, &end) == 1 && end && !path[end] && cpu >= 44 && cpu <= 47)
+        return fmemopen(pairs[(cpu - 44) / 2], 6, "r");
+    FILE *(*real_fopen)(const char *, const char *) = dlsym(RTLD_NEXT, "fopen");
+    return real_fopen(path, mode);
+}
diff --git a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
index 6d11e9ce5b2552d2f792115b3076459979edbe39..c04a11b7fc4a6c500632d4e2c48b7949c2dc8c6e
--- a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
+++ b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
@@ -60,7 +60,9 @@
 
     static void notify_retry() {
         Server server;
-        Config cfg; cfg.even_ifid = 1; cfg.even_ex = 1; cfg.shards = 16;
+        // The gate runs this serverless row without taskset. Automatic SMT placement requires
+        // whole sibling pairs for BOTH roles; only one IO is a tracking destination below.
+        Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "notification topology");
         server.threads_.resize(server.placement().total_threads());
         for (uint32_t i = 0; i < server.threads_.size(); ++i) {
@@ -94,7 +96,8 @@
     }
 
     static void flush() {
-        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 1; cfg.shards = 16;
+        // Two IOs still prove the foreign delivery; an even EX count also admits SMT placement.
+        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "flush topology");
         const auto& ios = server.placement().ifid_threads();
         check(ios.size() == 2, "foreign tracking IO exists");
@@ -136,7 +139,16 @@
         Server server; ThreadCtx self; IoLoop loop;
         loop.srv_ = &server; loop.self_ = &self;
         sender.bind(loop.wb_);
-        server.live_obuf_normal_hard_.store(8); server.client_obuf_armed_.store(true);
+        // Mirror Server::init's mailbox initialization without starting workers or opening rings.
+        // cx-waits made raw live-field stores writer-only; IO reads the published snapshot.
+        server.live_config_committed_ = server.capture_live_config(server.live_config_version_.load());
+        server.live_config_mailboxes_ = std::make_unique<LiveConfigMailbox[]>(server.nthreads() + 1);
+        server.live_config_mailboxes_[self.id()].init(server.live_config_committed_);
+        ClientOutputBufferLimits limits{}; limits.normal.hard_bytes = 8;
+        server.set_client_output_buffer_limits(limits);
+        check(server.client_obuf_armed() &&
+              server.client_limits_snapshot(self.id()).normal.hard_bytes == 8,
+              "hard limit published to the IO config mailbox");
         Client held(-1); held.set_id(1);
         check(held.rob().acquire(), "held ordinary operation"); held.rob().publish();
         check(!held.blocked(), "ordinary operation, not blocking exception");

diff --git a//home/user/Projects/cx-final-netcmdfix/NETCMDFIX.md b//home/user/Projects/cx-final-netcmdfix/NETCMDFIX.md
new file mode 100644
index 0000000000000000000000000000000000000000..16724c2e2d3fa8494e027d7d8e12faf9ba097ef4
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/NETCMDFIX.md
@@ -0,0 +1,215 @@
+# Netcmd regression fixes
+
+2026-09-09. Base: `67f623716ca00b49d7cbdb8d33ab6b643b3e87ad` (`cx-final`).
+Worktree: `/home/user/Projects/cx-final-netcmdfix`, branch `codex/netcmdfix-20260909`.
+
+All three verdicts are **(b): invalid test-fixture assumptions**. The notification,
+flush-ordering, and output-limit properties remain required. No existing behavioral
+assertion was removed or relaxed. The patch changes only `tests/netcmd_unit.cc` and
+this report; production code and layouts are unchanged.
+
+## Evidence and changes per row
+
+### `netcmd notify-retry regression` — (b)
+
+The gate drives `build/netcmd-unit notify-retry`, not `tests/notify.py` or
+`tests/netcmd.py`. Unlike the gate's server boots, its serverless unit invocation
+at `tests/gate.sh:396` has **no taskset wrapper**. The saved failure was:
+
+```text
+--ratio: SMT placement requires even logical io and ex counts
+FAIL: notification topology
+```
+
+The erroneous assumption was that `prepare_boot` must accept a `1:1` IO/EX
+topology regardless of the process affinity. `Server::prepare_boot` automatically
+selects SMT placement when discovery finds a complete sibling pair;
+`Placement::build_even_smt` requires an even count for each role. A correct server
+must reject `1:1` in that topology. The failure occurs before the queue is filled
+or the notification code runs.
+
+The unmodified test passed **5/5** on cores 44–51, which contain no complete SMT
+pairs. With simulated sibling metadata it reproduced the exact saved failure
+**5/5**, while execution remained confined to those same cores. This is a topology
+dependency, not a probabilistic queue failure.
+
+Automatic SMT selection came from **cx-knobs**, integrated by `c8e61f646`. That
+commit is already an ancestor of netcmd's test-introduction commit `e94b97100`.
+Thus this assumption was wrong when the test was written; a subsequent overlap,
+read-local, vestigial-code, or resize merge did not introduce it. The original
+netcmd report describes validation on pinned physical cores, which explains why
+those runs did not expose it.
+
+**Change:** use `2:2`, valid with physical cores or complete SMT pairs. The tracking
+mask still selects exactly one IO. The actual producer queue must fill, the failed
+post must report zero completed work, the keyless record must survive, no payload
+may escape the failed post, and the drained retry must deliver exactly the original
+invalidation. Every one of those checks is unchanged.
+
+### `netcmd flush regression` — (b)
+
+This row drives `build/netcmd-unit flush`. Its saved failure was the same SMT
+diagnostic followed by `FAIL: flush topology`. Its erroneous assumption was that
+`prepare_boot` must accept `2:1` under any affinity. The odd EX count violates the
+same existing SMT contract. Attribution is likewise **cx-knobs / an invalid
+netcmd fixture at introduction**, not a new flush-path behavior.
+
+The original test passed **5/5** on cores 44–51 and reproduced the topology
+failure **5/5** under simulated SMT metadata. Neither failure reached dispatch or
+flush completion.
+
+**Change:** use `2:2`. The fixture still asserts exactly two IOs and selects a
+foreign tracking IO. The dispatch gate must produce no invalidation before owner
+completion; after completion, the test must receive exactly one `TrackingFlush`.
+Those assertions and the completion ordering are unchanged. The extra executor
+exists only to satisfy placement; the unit starts no workers.
+
+### `netcmd output regression` — (b)
+
+The original test crashed **5/5**, including at the physical-core geometry. GDB
+identified the failure precisely:
+
+```text
+SIGSEGV
+Server::client_limits_snapshot(reader_tid=0)  src/core/server.h:2225
+IoLoop::client_obuf_check                    src/core/io_loop.h:6005
+NetcmdRegression::output                    tests/netcmd_unit.cc:146 (base)
+```
+
+`live_config_mailboxes_` was null. The erroneous fixture assumption was that a
+default-constructed Server plus direct stores to `live_obuf_normal_hard_` and
+`client_obuf_armed_` supplied all state consumed by `client_obuf_check`.
+
+**cx-waits** changed that contract in `c33228203`, merged here by `818d458ef`.
+Before that merge, `client_limits_snapshot()` collected the mutable live atomics.
+It now reads a committed per-consumer mailbox using the worker ID, without reader
+retries. Production `Server::init` allocates and initializes those mailboxes before
+workers run. The hand-built netcmd fixture bypassed that initialization and wrote
+only the CONFIG producer's private fields. Adding a null fallback or restoring
+the old reader loop in production would conceal the invalid fixture and conflict
+with the intended publication design.
+
+**Change:** initialize the fixture's sole config-consumer mailbox using the same
+committed-value initialization as `Server::init`, then call the real
+`set_client_output_buffer_limits` publisher. A new assertion requires both the
+armed selector and an eight-byte hard limit in the IO's published snapshot before
+the behavioral test proceeds.
+
+The seven-byte uncounted reply must still finish sending without subtraction from
+zero. The separate nine-byte deferred push must still exceed the **unchanged
+eight-byte** limit and close a client with an unfinished ordinary ROB operation.
+All bytes must still exist exclusively in deferred storage, and the operation must
+still be nonblocking. No threshold, timing allowance, or exception was changed.
+
+## Repeated validation
+
+A fresh worktree had no build artifacts. Commands used for the initial clean build:
+
+```sh
+taskset -c 44-55 make clean
+taskset -c 44-55 make -j6 build/netcmd-unit build/tomokv
+```
+
+The changed unit translation unit was then rebuilt. All eight locked layout
+assertions compiled unchanged. Test invocations retained the gate's 60-second
+unit timeout:
+
+```sh
+for row in notify-retry flush output; do
+  for attempt in 1 2 3 4 5; do
+    taskset -c 44-51 timeout 60 ./build/netcmd-unit "$row"
+  done
+done
+```
+
+| Row | PRE physical cores | PRE simulated SMT | POST physical cores | POST simulated SMT |
+|---|---:|---:|---:|---:|
+| notify-retry | 5/5 pass | 0/5 pass | 5/5 pass | 5/5 pass |
+| flush | 5/5 pass | 0/5 pass | 5/5 pass | 5/5 pass |
+| output | 0/5 pass, SIGSEGV | Not run | 5/5 pass | 5/5 pass |
+
+The simulation declares CPUs 44–47 as one L3 domain and uses a disposable
+`fopen` interposer to report sibling pairs 44/45 and 46/47. It changes only sysfs
+read results inside the serverless fixture. Neither actual affinity nor host
+topology is changed, no worker is launched, and no CPU outside 44–55 is used.
+It exercises the real SMT placement implementation. The machine's actual complete
+SMT pairs extend outside the assigned cores, so an actual unpinned rerun was not
+performed. Saved gate diagnostics and the simulated failures agree exactly.
+
+The interposer and exact commands are preserved in
+`build/netcmdfix/smt-sysfs.c` and `pre-smt-results.json` / `post-results.json` under
+that same directory. For example:
+
+```sh
+taskset -c 44-51 timeout 60 env TOMOKV_L3_DOMAINS=44-47 \
+  LD_PRELOAD="$PWD/build/netcmdfix/smt-sysfs.so" ./build/netcmd-unit notify-retry
+```
+
+## Tests reject broken mechanisms
+
+These controls were **built and run**, using disposable copies under
+`build/netcmdfix/mutants/`. They never replaced production sources, objects, or the
+good binaries. Each control was run five times; only the expected assertion or
+identified accounting abort counted as a successful rejection.
+
+| Deliberate break | Required failure observed | Rejections |
+|---|---|---:|
+| In `notify_ex_pass_entry`, pop the keyless record and return `work + 1` when posting its invalidation fails. | `failed marker post is incomplete work` | 5/5 |
+| Restore `tracking_broadcast_flush()` at dispatch in `climon_armed_gate`. | `no flush invalidation before owner work was published` | 5/5 |
+| Return immediately from `climon_flush_completed`, omitting its broadcast. | `completed flush invalidates foreign tracking IO` | 5/5 |
+| Remove the untracked Wb specializations' `stop_obuf_tracking()` calls. | SIGABRT in `Client::commit_write(7)` at `conn.h:419`, confirmed with GDB. | 5/5 |
+| Omit `wb_.deferred_output_bytes(*c)` from `client_obuf_check`'s used-byte total. | `hard limit fired before Done` | 5/5 |
+
+The mutation descriptions above are also the instructions for inducing each
+failure. Exact replacements, compile/link commands, expected diagnostics, and
+per-attempt results are retained in `build/netcmdfix/negative_controls.py`, each
+mutant's `commands.json`, and `build/netcmdfix/negative-results.json`.
+
+## Live integration
+
+The unchanged `tests/netcmd.py` battery also passed **48/48** attempts, with a fresh
+server for each attempt. This includes its real RESP3 tracking invalidation after
+FLUSHALL and the following GET observing the cleared key; this supplements the
+unit's direct completion-hook checks.
+
+| Thread mode | Atomic | Read-local | Overlap 0 | Overlap 1 |
+|---|---:|---:|---:|---:|
+| split | 0 | 0 | 3/3 | 3/3 |
+| split | 0 | 1 | 3/3 | 3/3 |
+| split | 1 | 0 | 3/3 | 3/3 |
+| split | 1 | 1 | 3/3 | 3/3 |
+| fused | 0 | 0 | 3/3 | 3/3 |
+| fused | 0 | 1 | 3/3 | 3/3 |
+| fused | 1 | 0 | 3/3 | 3/3 |
+| fused | 1 | 1 | 3/3 | 3/3 |
+
+Servers used io_uring, cores **44–51**, **16 shards**, and split ratio **6:2**.
+Fused boots omitted `--ratio`. Battery clients used cores **52–55**. Each boot's
+INFO response was saved and checked for its thread mode and effective read-local
+setting. These are netcmd integration passes under those configurations, not a
+measurement of read-local hit coverage or performance.
+
+Ports used were **8580–8589 and 8596–8601**. A preflight found a pre-existing Garnet
+listener on 8590 before starting any test there; the remaining cells resumed on
+unused permitted ports. That listener was neither contacted nor stopped. All 48
+owned server PIDs exited cleanly with status zero. The harness stopped only its
+own PIDs and never used a pattern kill.
+
+`build/netcmdfix/live_matrix.py`, `live/results.json`, and each attempt's
+`server.log`, `info-server.log`, and `battery.log` preserve the commands and evidence.
+Build logs, original failure logs, both debugger traces, and binary SHA-256 values
+are also under `build/netcmdfix/`. These are local, ignored validation artifacts.
+
+## Scope and remaining failures
+
+**No failure among the three requested rows remains unattributed.** The unchanged
+notification, flush hook, and Wb implementations are identical to the netcmd merge
+`821d207b1`; the relevant subsequent production change is the cx-waits configuration
+reader used by the output fixture. The live matrix also covers both restored
+overlap schedules and the read-local option in both thread modes.
+
+No row was added, deleted, or moved. The three rows remain above the quick-tier
+exit; the gate-count delta is **0 quick / 0 full**. `tests/gate.sh`, including
+`EXPECT_QUICK` and `EXPECT_FULL`, was not edited. No full gate or benchmark was run.
+There is no performance claim. The user's changes in the original `cx-final`
+worktree were left untouched.
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
new file mode 100644
index 0000000000000000000000000000000000000000..295ae00928add7346879af86ca8bfdb274f46beb
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
@@ -0,0 +1,85 @@
+"""Run only tests/netcmd.py; each attempt owns one server PID and private data directory."""
+import itertools
+import json
+from pathlib import Path
+import resource
+import socket
+import subprocess
+import sys
+import time
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix/live'
+out.mkdir(exist_ok=True)
+sys.path.insert(0, str(root / 'tests'))
+from _lib import Conn
+
+results_file = out / 'results.json'
+results = json.loads(results_file.read_text()) if results_file.exists() else []
+cells = itertools.product(('split', 'fused'), (0, 1), (0, 1), (0, 1))
+for cell, (mode, atomic, read_local, overlap) in enumerate(cells):
+    # 8590 belongs to an existing Garnet PID; resume the remaining cells on unused ports.
+    port = 8580 + cell if cell < 10 else 8596 + cell - 10
+    name = f'{mode}-a{atomic}-rl{read_local}-overlap{overlap}'
+    for attempt in range(1, 4):
+        if any(r['cell'] == name and r['attempt'] == attempt for r in results):
+            continue  # preserve completed attempts, including any failure, when resuming
+        directory = out / name / str(attempt)
+        directory.mkdir(parents=True, exist_ok=True)
+        with socket.socket() as guard:
+            guard.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
+            guard.bind(('127.0.0.1', port))
+        command = ['taskset', '-c', '44-51', './build/tomokv', '--bind', '127.0.0.1',
+                   '--port', str(port), '--shards', '16', '--thread-mode', mode,
+                   '--atomic', str(atomic), '--read-local', str(read_local),
+                   '--overlap', str(overlap), '--enable-debug-command', 'yes',
+                   '--dir', str(directory)]
+        if mode == 'split':
+            command += ['--ratio', '6:2']
+        record = dict(cell=name, attempt=attempt, port=port, command=command)
+        with (directory / 'server.log').open('w') as log:
+            server = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
+            record['pid'] = server.pid
+            try:
+                deadline = time.monotonic() + 30
+                while True:
+                    assert server.poll() is None, f'{name}: server exited {server.returncode}'
+                    try:
+                        connection = Conn('127.0.0.1', port, timeout=2)
+                        break
+                    except OSError:
+                        assert time.monotonic() < deadline, f'{name}: boot timeout'
+                        time.sleep(0.05)
+                try:
+                    assert connection.cmd('PING') == b'PONG'
+                    info = connection.cmd('INFO', 'server').decode()
+                    (directory / 'info-server.log').write_text(info)
+                    fields = dict(line.split(':', 1) for line in info.splitlines() if ':' in line)
+                    assert fields['thread_mode'] == ('2s' if mode == 'split' else '1s'), fields
+                    assert fields['read_local'] == str(read_local), fields
+                finally:
+                    connection.close()
+                test = ['taskset', '-c', '52-55', 'timeout', '60', 'python3', '-u',
+                        'tests/netcmd.py', '127.0.0.1', str(port)]
+                result = subprocess.run(test, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+                (directory / 'battery.log').write_text(result.stdout)
+                record.update(returncode=result.returncode, test_command=test)
+            except Exception as exc:
+                record.update(returncode=1, error=repr(exc))
+            finally:
+                if server.poll() is None:
+                    server.terminate()
+                    try:
+                        server.wait(timeout=20)
+                    except subprocess.TimeoutExpired:
+                        server.kill()
+                        server.wait()
+                record['server_exit'] = server.returncode
+        results.append(record)
+        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
+        if record['returncode'] or record['server_exit']:
+            print(f'FAIL {name} attempt {attempt}: {record}', flush=True)
+    cell_results = [r for r in results if r['cell'] == name]
+    print(f'{name}: {sum(r["returncode"] == 0 and r["server_exit"] == 0 for r in cell_results)}/3', flush=True)
+raise SystemExit(any(r['returncode'] or r['server_exit'] for r in results))
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
new file mode 100644
index 0000000000000000000000000000000000000000..b6494711491bb3710f859aa5fb7e295a4e3b093b
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
@@ -0,0 +1,77 @@
+"""Disposable source copies only; never modifies the production sources or objects."""
+import json
+from pathlib import Path
+import resource
+import shlex
+import shutil
+import signal
+import subprocess
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix'
+recipes = subprocess.check_output(['make', '-n', '-B', 'build/netcmd-unit'], text=True).splitlines()
+link = shlex.split(next(line for line in recipes if ' -o build/netcmd-unit ' in line))
+variants = [
+    ('notify-drop', 'notify-retry', 'src/cmd/notify.inc',
+     'return work;  // keep this target and the keyless record for the next pass',
+     'slot->keyless.pop_front(); return work + 1;  // deliberately lose the invalidation',
+     'tests/netcmd_unit.cc', 'failed marker post is incomplete work'),
+    ('flush-early', 'flush', 'src/cmd/climon.cc',
+     'if (armed & Server::kClimonTracking) {\n        ClimonConn* state',
+     'if (armed & Server::kClimonTracking) {\n'
+     '        if (op.cmd_name().eq_icase("flushall") || op.cmd_name().eq_icase("flushdb"))\n'
+     '            tracking_broadcast_flush();\n        ClimonConn* state',
+     'src/cmd/climon.cc', 'no flush invalidation before owner work was published'),
+    ('flush-missing', 'flush', 'src/cmd/climon.cc',
+     'void IoLoop::climon_flush_completed(Op& op) {\n',
+     'void IoLoop::climon_flush_completed(Op& op) {\n    return; // omit completed invalidation\n',
+     'src/cmd/climon.cc', 'completed flush invalidates foreign tracking IO'),
+    ('output-untracked', 'output', 'src/net/wb.h',
+     'else conn.stop_obuf_tracking();',
+     'else {} // deliberately leave accounting armed for uncounted bytes',
+     'tests/netcmd_unit.cc', None),
+    ('output-deferred', 'output', 'src/core/io_loop.h',
+     'uint64_t used = c->obuf_bytes() + wb_.deferred_output_bytes(*c);',
+     'uint64_t used = c->obuf_bytes(); // deliberately omit deferred storage',
+     'tests/netcmd_unit.cc', 'hard limit fired before Done'),
+]
+results = []
+for name, case, source, before, after, tu, diagnostic in variants:
+    tree = out / 'mutants' / name
+    tree.mkdir(parents=True)
+    shutil.copytree(root / 'src', tree / 'src')
+    (tree / 'third_party').symlink_to(root / 'third_party', target_is_directory=True)
+    (tree / 'tests').mkdir()
+    for test in ('netcmd_unit.cc', 'netcmd_unit.h'):
+        shutil.copy2(root / 'tests' / test, tree / 'tests' / test)
+    target = tree / source
+    original = target.read_text()
+    assert original.count(before) == (2 if name == 'output-untracked' else 1)
+    target.write_text(original.replace(before, after))
+    old_object = 'build/tests/netcmd_unit.o' if tu.startswith('tests/') else 'build/src/cmd/climon.o'
+    compile_cmd = shlex.split(next(line for line in recipes if f' -c {tu} -o {old_object}' in line))
+    new_object = str(tree / 'mutant.o')
+    compile_cmd = [str(tree / tu) if arg == tu else new_object if arg == old_object
+                   else f'-I{tree}' if arg == '-I.' else arg for arg in compile_cmd]
+    binary = str(tree / 'netcmd-unit')
+    link_cmd = [new_object if arg == old_object else binary if arg == 'build/netcmd-unit' else arg
+                for arg in link]
+    print(f'building {name}', flush=True)
+    (tree / 'commands.json').write_text(json.dumps([compile_cmd, link_cmd], indent=2) + '\n')
+    with (tree / 'build.log').open('w') as log:
+        subprocess.run(['taskset', '-c', '44-55', *compile_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=300)
+        subprocess.run(['taskset', '-c', '44-55', *link_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=120)
+    for run in range(1, 6):
+        command = ['taskset', '-c', '44-51', 'timeout', '60', binary, case]
+        r = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+        (tree / f'run-{run}.log').write_text(r.stdout)
+        expected = r.returncode == -signal.SIGABRT if diagnostic is None else (
+            r.returncode == 1 and f'FAIL: {diagnostic}' in r.stdout)
+        results.append({'variant':name, 'case':case, 'run':run, 'returncode':r.returncode,
+                        'expected_failure':expected, 'output':r.stdout, 'command':command})
+        assert expected, (name, r.returncode, r.stdout)
+    print(f'{name}: rejected 5/5; {r.stdout.strip()}', flush=True)
+    (out / 'negative-results.json').write_text(json.dumps(results, indent=2) + '\n')
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
new file mode 100644
index 0000000000000000000000000000000000000000..bc72f719b1e93668f4c671000b1d5bad75da7ac0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
@@ -0,0 +1,16 @@
+#define _GNU_SOURCE
+#include <dlfcn.h>
+#include <stdio.h>
+#include <string.h>
+
+/* Read-only topology simulation: the process remains pinned to real CPUs 44-51.
+ * These serverless fixtures never launch workers. Only sibling metadata changes. */
+FILE *fopen(const char *path, const char *mode) {
+    static char pairs[2][7] = {"44-45\n", "46-47\n"};
+    int cpu = -1, end = 0;
+    if (sscanf(path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list%n",
+               &cpu, &end) == 1 && end && !path[end] && cpu >= 44 && cpu <= 47)
+        return fmemopen(pairs[(cpu - 44) / 2], 6, "r");
+    FILE *(*real_fopen)(const char *, const char *) = dlsym(RTLD_NEXT, "fopen");
+    return real_fopen(path, mode);
+}
diff --git a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
index 6d11e9ce5b2552d2f792115b3076459979edbe39..c04a11b7fc4a6c500632d4e2c48b7949c2dc8c6e
--- a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
+++ b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
@@ -60,7 +60,9 @@
 
     static void notify_retry() {
         Server server;
-        Config cfg; cfg.even_ifid = 1; cfg.even_ex = 1; cfg.shards = 16;
+        // The gate runs this serverless row without taskset. Automatic SMT placement requires
+        // whole sibling pairs for BOTH roles; only one IO is a tracking destination below.
+        Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "notification topology");
         server.threads_.resize(server.placement().total_threads());
         for (uint32_t i = 0; i < server.threads_.size(); ++i) {
@@ -94,7 +96,8 @@
     }
 
     static void flush() {
-        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 1; cfg.shards = 16;
+        // Two IOs still prove the foreign delivery; an even EX count also admits SMT placement.
+        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "flush topology");
         const auto& ios = server.placement().ifid_threads();
         check(ios.size() == 2, "foreign tracking IO exists");
@@ -136,7 +139,16 @@
         Server server; ThreadCtx self; IoLoop loop;
         loop.srv_ = &server; loop.self_ = &self;
         sender.bind(loop.wb_);
-        server.live_obuf_normal_hard_.store(8); server.client_obuf_armed_.store(true);
+        // Mirror Server::init's mailbox initialization without starting workers or opening rings.
+        // cx-waits made raw live-field stores writer-only; IO reads the published snapshot.
+        server.live_config_committed_ = server.capture_live_config(server.live_config_version_.load());
+        server.live_config_mailboxes_ = std::make_unique<LiveConfigMailbox[]>(server.nthreads() + 1);
+        server.live_config_mailboxes_[self.id()].init(server.live_config_committed_);
+        ClientOutputBufferLimits limits{}; limits.normal.hard_bytes = 8;
+        server.set_client_output_buffer_limits(limits);
+        check(server.client_obuf_armed() &&
+              server.client_limits_snapshot(self.id()).normal.hard_bytes == 8,
+              "hard limit published to the IO config mailbox");
         Client held(-1); held.set_id(1);
         check(held.rob().acquire(), "held ordinary operation"); held.rob().publish();
         check(!held.blocked(), "ordinary operation, not blocking exception");

codex
The live battery finished at 48/48, covering both thread modes with atomicity, read-local, and overlap each off and on. Each repaired unit passed 10/10 across the two topology setups, and all five broken variants were rejected 5/5. I’m checking the report against the saved evidence and committing the fixture changes.

exec
/bin/bash -lc 'git status --short && git rev-parse HEAD' in /home/user/Projects/cx-final
 succeeded in 0ms:
 M CODEX-OUT.md
67f623716ca00b49d7cbdb8d33ab6b643b3e87ad

exec
/bin/bash -lc "git diff --check && taskset -c 44-55 make -q build/netcmd-unit build/tomokv && git diff --stat && git status --short && git diff --quiet -- src tests/gate.sh Makefile && python3 - <<'PY'
import json
from pathlib import Path
root=Path('build/netcmdfix')
post=json.loads((root/'post-results.json').read_text())
negative=json.loads((root/'negative-results.json').read_text())
live=json.loads((root/'live/results.json').read_text())
assert len(post)==30 and all(r['returncode']==0 for r in post)
assert len(negative)==25 and all(r['expected_failure'] for r in negative)
assert len(live)==48 and all(r['returncode']==0 and r['server_exit']==0 for r in live)
for r in live:
    log=root/'live'/r['cell']/str(r['attempt'])/'info-server.log'
    info=dict(line.split(':',1) for line in log.read_text().splitlines() if ':' in line)
    mode,atomic,local,overlap=r['cell'].split('-')
    assert info['thread_mode']==('2s' if mode=='split' else '1s')
    assert info['atomic']==atomic[1:] and info['read_local']==local[2:]
    assert info['overlap']==overlap.removeprefix('overlap') and info['shards']=='16'
    if mode=='split': assert info['io_threads']=='6' and info['ex_threads']=='2'
    else: assert info['fused_threads']=='8'
    assert 8580<=r['port']<=8609
remaining=[r['pid'] for r in live if Path(f'/proc/{r[\"pid\"]}').exists()]
assert not remaining, remaining
print('Evidence audit: 30/30 positive units; 25/25 expected mutant failures; 48/48 live batteries and clean exits.')
print('All recorded server geometries match. All 48 owned server PIDs have exited.')
PY" in /home/user/Projects/cx-final-netcmdfix
 succeeded in 0ms:
 tests/netcmd_unit.cc | 18 +++++++++++++++---
 1 file changed, 15 insertions(+), 3 deletions(-)
 M tests/netcmd_unit.cc
?? NETCMDFIX.md
Evidence audit: 30/30 positive units; 25/25 expected mutant failures; 48/48 live batteries and clean exits.
All recorded server geometries match. All 48 owned server PIDs have exited.

diff --git a//home/user/Projects/cx-final-netcmdfix/NETCMDFIX.md b//home/user/Projects/cx-final-netcmdfix/NETCMDFIX.md
new file mode 100644
index 0000000000000000000000000000000000000000..16724c2e2d3fa8494e027d7d8e12faf9ba097ef4
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/NETCMDFIX.md
@@ -0,0 +1,215 @@
+# Netcmd regression fixes
+
+2026-09-09. Base: `67f623716ca00b49d7cbdb8d33ab6b643b3e87ad` (`cx-final`).
+Worktree: `/home/user/Projects/cx-final-netcmdfix`, branch `codex/netcmdfix-20260909`.
+
+All three verdicts are **(b): invalid test-fixture assumptions**. The notification,
+flush-ordering, and output-limit properties remain required. No existing behavioral
+assertion was removed or relaxed. The patch changes only `tests/netcmd_unit.cc` and
+this report; production code and layouts are unchanged.
+
+## Evidence and changes per row
+
+### `netcmd notify-retry regression` — (b)
+
+The gate drives `build/netcmd-unit notify-retry`, not `tests/notify.py` or
+`tests/netcmd.py`. Unlike the gate's server boots, its serverless unit invocation
+at `tests/gate.sh:396` has **no taskset wrapper**. The saved failure was:
+
+```text
+--ratio: SMT placement requires even logical io and ex counts
+FAIL: notification topology
+```
+
+The erroneous assumption was that `prepare_boot` must accept a `1:1` IO/EX
+topology regardless of the process affinity. `Server::prepare_boot` automatically
+selects SMT placement when discovery finds a complete sibling pair;
+`Placement::build_even_smt` requires an even count for each role. A correct server
+must reject `1:1` in that topology. The failure occurs before the queue is filled
+or the notification code runs.
+
+The unmodified test passed **5/5** on cores 44–51, which contain no complete SMT
+pairs. With simulated sibling metadata it reproduced the exact saved failure
+**5/5**, while execution remained confined to those same cores. This is a topology
+dependency, not a probabilistic queue failure.
+
+Automatic SMT selection came from **cx-knobs**, integrated by `c8e61f646`. That
+commit is already an ancestor of netcmd's test-introduction commit `e94b97100`.
+Thus this assumption was wrong when the test was written; a subsequent overlap,
+read-local, vestigial-code, or resize merge did not introduce it. The original
+netcmd report describes validation on pinned physical cores, which explains why
+those runs did not expose it.
+
+**Change:** use `2:2`, valid with physical cores or complete SMT pairs. The tracking
+mask still selects exactly one IO. The actual producer queue must fill, the failed
+post must report zero completed work, the keyless record must survive, no payload
+may escape the failed post, and the drained retry must deliver exactly the original
+invalidation. Every one of those checks is unchanged.
+
+### `netcmd flush regression` — (b)
+
+This row drives `build/netcmd-unit flush`. Its saved failure was the same SMT
+diagnostic followed by `FAIL: flush topology`. Its erroneous assumption was that
+`prepare_boot` must accept `2:1` under any affinity. The odd EX count violates the
+same existing SMT contract. Attribution is likewise **cx-knobs / an invalid
+netcmd fixture at introduction**, not a new flush-path behavior.
+
+The original test passed **5/5** on cores 44–51 and reproduced the topology
+failure **5/5** under simulated SMT metadata. Neither failure reached dispatch or
+flush completion.
+
+**Change:** use `2:2`. The fixture still asserts exactly two IOs and selects a
+foreign tracking IO. The dispatch gate must produce no invalidation before owner
+completion; after completion, the test must receive exactly one `TrackingFlush`.
+Those assertions and the completion ordering are unchanged. The extra executor
+exists only to satisfy placement; the unit starts no workers.
+
+### `netcmd output regression` — (b)
+
+The original test crashed **5/5**, including at the physical-core geometry. GDB
+identified the failure precisely:
+
+```text
+SIGSEGV
+Server::client_limits_snapshot(reader_tid=0)  src/core/server.h:2225
+IoLoop::client_obuf_check                    src/core/io_loop.h:6005
+NetcmdRegression::output                    tests/netcmd_unit.cc:146 (base)
+```
+
+`live_config_mailboxes_` was null. The erroneous fixture assumption was that a
+default-constructed Server plus direct stores to `live_obuf_normal_hard_` and
+`client_obuf_armed_` supplied all state consumed by `client_obuf_check`.
+
+**cx-waits** changed that contract in `c33228203`, merged here by `818d458ef`.
+Before that merge, `client_limits_snapshot()` collected the mutable live atomics.
+It now reads a committed per-consumer mailbox using the worker ID, without reader
+retries. Production `Server::init` allocates and initializes those mailboxes before
+workers run. The hand-built netcmd fixture bypassed that initialization and wrote
+only the CONFIG producer's private fields. Adding a null fallback or restoring
+the old reader loop in production would conceal the invalid fixture and conflict
+with the intended publication design.
+
+**Change:** initialize the fixture's sole config-consumer mailbox using the same
+committed-value initialization as `Server::init`, then call the real
+`set_client_output_buffer_limits` publisher. A new assertion requires both the
+armed selector and an eight-byte hard limit in the IO's published snapshot before
+the behavioral test proceeds.
+
+The seven-byte uncounted reply must still finish sending without subtraction from
+zero. The separate nine-byte deferred push must still exceed the **unchanged
+eight-byte** limit and close a client with an unfinished ordinary ROB operation.
+All bytes must still exist exclusively in deferred storage, and the operation must
+still be nonblocking. No threshold, timing allowance, or exception was changed.
+
+## Repeated validation
+
+A fresh worktree had no build artifacts. Commands used for the initial clean build:
+
+```sh
+taskset -c 44-55 make clean
+taskset -c 44-55 make -j6 build/netcmd-unit build/tomokv
+```
+
+The changed unit translation unit was then rebuilt. All eight locked layout
+assertions compiled unchanged. Test invocations retained the gate's 60-second
+unit timeout:
+
+```sh
+for row in notify-retry flush output; do
+  for attempt in 1 2 3 4 5; do
+    taskset -c 44-51 timeout 60 ./build/netcmd-unit "$row"
+  done
+done
+```
+
+| Row | PRE physical cores | PRE simulated SMT | POST physical cores | POST simulated SMT |
+|---|---:|---:|---:|---:|
+| notify-retry | 5/5 pass | 0/5 pass | 5/5 pass | 5/5 pass |
+| flush | 5/5 pass | 0/5 pass | 5/5 pass | 5/5 pass |
+| output | 0/5 pass, SIGSEGV | Not run | 5/5 pass | 5/5 pass |
+
+The simulation declares CPUs 44–47 as one L3 domain and uses a disposable
+`fopen` interposer to report sibling pairs 44/45 and 46/47. It changes only sysfs
+read results inside the serverless fixture. Neither actual affinity nor host
+topology is changed, no worker is launched, and no CPU outside 44–55 is used.
+It exercises the real SMT placement implementation. The machine's actual complete
+SMT pairs extend outside the assigned cores, so an actual unpinned rerun was not
+performed. Saved gate diagnostics and the simulated failures agree exactly.
+
+The interposer and exact commands are preserved in
+`build/netcmdfix/smt-sysfs.c` and `pre-smt-results.json` / `post-results.json` under
+that same directory. For example:
+
+```sh
+taskset -c 44-51 timeout 60 env TOMOKV_L3_DOMAINS=44-47 \
+  LD_PRELOAD="$PWD/build/netcmdfix/smt-sysfs.so" ./build/netcmd-unit notify-retry
+```
+
+## Tests reject broken mechanisms
+
+These controls were **built and run**, using disposable copies under
+`build/netcmdfix/mutants/`. They never replaced production sources, objects, or the
+good binaries. Each control was run five times; only the expected assertion or
+identified accounting abort counted as a successful rejection.
+
+| Deliberate break | Required failure observed | Rejections |
+|---|---|---:|
+| In `notify_ex_pass_entry`, pop the keyless record and return `work + 1` when posting its invalidation fails. | `failed marker post is incomplete work` | 5/5 |
+| Restore `tracking_broadcast_flush()` at dispatch in `climon_armed_gate`. | `no flush invalidation before owner work was published` | 5/5 |
+| Return immediately from `climon_flush_completed`, omitting its broadcast. | `completed flush invalidates foreign tracking IO` | 5/5 |
+| Remove the untracked Wb specializations' `stop_obuf_tracking()` calls. | SIGABRT in `Client::commit_write(7)` at `conn.h:419`, confirmed with GDB. | 5/5 |
+| Omit `wb_.deferred_output_bytes(*c)` from `client_obuf_check`'s used-byte total. | `hard limit fired before Done` | 5/5 |
+
+The mutation descriptions above are also the instructions for inducing each
+failure. Exact replacements, compile/link commands, expected diagnostics, and
+per-attempt results are retained in `build/netcmdfix/negative_controls.py`, each
+mutant's `commands.json`, and `build/netcmdfix/negative-results.json`.
+
+## Live integration
+
+The unchanged `tests/netcmd.py` battery also passed **48/48** attempts, with a fresh
+server for each attempt. This includes its real RESP3 tracking invalidation after
+FLUSHALL and the following GET observing the cleared key; this supplements the
+unit's direct completion-hook checks.
+
+| Thread mode | Atomic | Read-local | Overlap 0 | Overlap 1 |
+|---|---:|---:|---:|---:|
+| split | 0 | 0 | 3/3 | 3/3 |
+| split | 0 | 1 | 3/3 | 3/3 |
+| split | 1 | 0 | 3/3 | 3/3 |
+| split | 1 | 1 | 3/3 | 3/3 |
+| fused | 0 | 0 | 3/3 | 3/3 |
+| fused | 0 | 1 | 3/3 | 3/3 |
+| fused | 1 | 0 | 3/3 | 3/3 |
+| fused | 1 | 1 | 3/3 | 3/3 |
+
+Servers used io_uring, cores **44–51**, **16 shards**, and split ratio **6:2**.
+Fused boots omitted `--ratio`. Battery clients used cores **52–55**. Each boot's
+INFO response was saved and checked for its thread mode and effective read-local
+setting. These are netcmd integration passes under those configurations, not a
+measurement of read-local hit coverage or performance.
+
+Ports used were **8580–8589 and 8596–8601**. A preflight found a pre-existing Garnet
+listener on 8590 before starting any test there; the remaining cells resumed on
+unused permitted ports. That listener was neither contacted nor stopped. All 48
+owned server PIDs exited cleanly with status zero. The harness stopped only its
+own PIDs and never used a pattern kill.
+
+`build/netcmdfix/live_matrix.py`, `live/results.json`, and each attempt's
+`server.log`, `info-server.log`, and `battery.log` preserve the commands and evidence.
+Build logs, original failure logs, both debugger traces, and binary SHA-256 values
+are also under `build/netcmdfix/`. These are local, ignored validation artifacts.
+
+## Scope and remaining failures
+
+**No failure among the three requested rows remains unattributed.** The unchanged
+notification, flush hook, and Wb implementations are identical to the netcmd merge
+`821d207b1`; the relevant subsequent production change is the cx-waits configuration
+reader used by the output fixture. The live matrix also covers both restored
+overlap schedules and the read-local option in both thread modes.
+
+No row was added, deleted, or moved. The three rows remain above the quick-tier
+exit; the gate-count delta is **0 quick / 0 full**. `tests/gate.sh`, including
+`EXPECT_QUICK` and `EXPECT_FULL`, was not edited. No full gate or benchmark was run.
+There is no performance claim. The user's changes in the original `cx-final`
+worktree were left untouched.
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
new file mode 100644
index 0000000000000000000000000000000000000000..295ae00928add7346879af86ca8bfdb274f46beb
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
@@ -0,0 +1,85 @@
+"""Run only tests/netcmd.py; each attempt owns one server PID and private data directory."""
+import itertools
+import json
+from pathlib import Path
+import resource
+import socket
+import subprocess
+import sys
+import time
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix/live'
+out.mkdir(exist_ok=True)
+sys.path.insert(0, str(root / 'tests'))
+from _lib import Conn
+
+results_file = out / 'results.json'
+results = json.loads(results_file.read_text()) if results_file.exists() else []
+cells = itertools.product(('split', 'fused'), (0, 1), (0, 1), (0, 1))
+for cell, (mode, atomic, read_local, overlap) in enumerate(cells):
+    # 8590 belongs to an existing Garnet PID; resume the remaining cells on unused ports.
+    port = 8580 + cell if cell < 10 else 8596 + cell - 10
+    name = f'{mode}-a{atomic}-rl{read_local}-overlap{overlap}'
+    for attempt in range(1, 4):
+        if any(r['cell'] == name and r['attempt'] == attempt for r in results):
+            continue  # preserve completed attempts, including any failure, when resuming
+        directory = out / name / str(attempt)
+        directory.mkdir(parents=True, exist_ok=True)
+        with socket.socket() as guard:
+            guard.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
+            guard.bind(('127.0.0.1', port))
+        command = ['taskset', '-c', '44-51', './build/tomokv', '--bind', '127.0.0.1',
+                   '--port', str(port), '--shards', '16', '--thread-mode', mode,
+                   '--atomic', str(atomic), '--read-local', str(read_local),
+                   '--overlap', str(overlap), '--enable-debug-command', 'yes',
+                   '--dir', str(directory)]
+        if mode == 'split':
+            command += ['--ratio', '6:2']
+        record = dict(cell=name, attempt=attempt, port=port, command=command)
+        with (directory / 'server.log').open('w') as log:
+            server = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
+            record['pid'] = server.pid
+            try:
+                deadline = time.monotonic() + 30
+                while True:
+                    assert server.poll() is None, f'{name}: server exited {server.returncode}'
+                    try:
+                        connection = Conn('127.0.0.1', port, timeout=2)
+                        break
+                    except OSError:
+                        assert time.monotonic() < deadline, f'{name}: boot timeout'
+                        time.sleep(0.05)
+                try:
+                    assert connection.cmd('PING') == b'PONG'
+                    info = connection.cmd('INFO', 'server').decode()
+                    (directory / 'info-server.log').write_text(info)
+                    fields = dict(line.split(':', 1) for line in info.splitlines() if ':' in line)
+                    assert fields['thread_mode'] == ('2s' if mode == 'split' else '1s'), fields
+                    assert fields['read_local'] == str(read_local), fields
+                finally:
+                    connection.close()
+                test = ['taskset', '-c', '52-55', 'timeout', '60', 'python3', '-u',
+                        'tests/netcmd.py', '127.0.0.1', str(port)]
+                result = subprocess.run(test, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+                (directory / 'battery.log').write_text(result.stdout)
+                record.update(returncode=result.returncode, test_command=test)
+            except Exception as exc:
+                record.update(returncode=1, error=repr(exc))
+            finally:
+                if server.poll() is None:
+                    server.terminate()
+                    try:
+                        server.wait(timeout=20)
+                    except subprocess.TimeoutExpired:
+                        server.kill()
+                        server.wait()
+                record['server_exit'] = server.returncode
+        results.append(record)
+        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
+        if record['returncode'] or record['server_exit']:
+            print(f'FAIL {name} attempt {attempt}: {record}', flush=True)
+    cell_results = [r for r in results if r['cell'] == name]
+    print(f'{name}: {sum(r["returncode"] == 0 and r["server_exit"] == 0 for r in cell_results)}/3', flush=True)
+raise SystemExit(any(r['returncode'] or r['server_exit'] for r in results))
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
new file mode 100644
index 0000000000000000000000000000000000000000..b6494711491bb3710f859aa5fb7e295a4e3b093b
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
@@ -0,0 +1,77 @@
+"""Disposable source copies only; never modifies the production sources or objects."""
+import json
+from pathlib import Path
+import resource
+import shlex
+import shutil
+import signal
+import subprocess
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix'
+recipes = subprocess.check_output(['make', '-n', '-B', 'build/netcmd-unit'], text=True).splitlines()
+link = shlex.split(next(line for line in recipes if ' -o build/netcmd-unit ' in line))
+variants = [
+    ('notify-drop', 'notify-retry', 'src/cmd/notify.inc',
+     'return work;  // keep this target and the keyless record for the next pass',
+     'slot->keyless.pop_front(); return work + 1;  // deliberately lose the invalidation',
+     'tests/netcmd_unit.cc', 'failed marker post is incomplete work'),
+    ('flush-early', 'flush', 'src/cmd/climon.cc',
+     'if (armed & Server::kClimonTracking) {\n        ClimonConn* state',
+     'if (armed & Server::kClimonTracking) {\n'
+     '        if (op.cmd_name().eq_icase("flushall") || op.cmd_name().eq_icase("flushdb"))\n'
+     '            tracking_broadcast_flush();\n        ClimonConn* state',
+     'src/cmd/climon.cc', 'no flush invalidation before owner work was published'),
+    ('flush-missing', 'flush', 'src/cmd/climon.cc',
+     'void IoLoop::climon_flush_completed(Op& op) {\n',
+     'void IoLoop::climon_flush_completed(Op& op) {\n    return; // omit completed invalidation\n',
+     'src/cmd/climon.cc', 'completed flush invalidates foreign tracking IO'),
+    ('output-untracked', 'output', 'src/net/wb.h',
+     'else conn.stop_obuf_tracking();',
+     'else {} // deliberately leave accounting armed for uncounted bytes',
+     'tests/netcmd_unit.cc', None),
+    ('output-deferred', 'output', 'src/core/io_loop.h',
+     'uint64_t used = c->obuf_bytes() + wb_.deferred_output_bytes(*c);',
+     'uint64_t used = c->obuf_bytes(); // deliberately omit deferred storage',
+     'tests/netcmd_unit.cc', 'hard limit fired before Done'),
+]
+results = []
+for name, case, source, before, after, tu, diagnostic in variants:
+    tree = out / 'mutants' / name
+    tree.mkdir(parents=True)
+    shutil.copytree(root / 'src', tree / 'src')
+    (tree / 'third_party').symlink_to(root / 'third_party', target_is_directory=True)
+    (tree / 'tests').mkdir()
+    for test in ('netcmd_unit.cc', 'netcmd_unit.h'):
+        shutil.copy2(root / 'tests' / test, tree / 'tests' / test)
+    target = tree / source
+    original = target.read_text()
+    assert original.count(before) == (2 if name == 'output-untracked' else 1)
+    target.write_text(original.replace(before, after))
+    old_object = 'build/tests/netcmd_unit.o' if tu.startswith('tests/') else 'build/src/cmd/climon.o'
+    compile_cmd = shlex.split(next(line for line in recipes if f' -c {tu} -o {old_object}' in line))
+    new_object = str(tree / 'mutant.o')
+    compile_cmd = [str(tree / tu) if arg == tu else new_object if arg == old_object
+                   else f'-I{tree}' if arg == '-I.' else arg for arg in compile_cmd]
+    binary = str(tree / 'netcmd-unit')
+    link_cmd = [new_object if arg == old_object else binary if arg == 'build/netcmd-unit' else arg
+                for arg in link]
+    print(f'building {name}', flush=True)
+    (tree / 'commands.json').write_text(json.dumps([compile_cmd, link_cmd], indent=2) + '\n')
+    with (tree / 'build.log').open('w') as log:
+        subprocess.run(['taskset', '-c', '44-55', *compile_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=300)
+        subprocess.run(['taskset', '-c', '44-55', *link_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=120)
+    for run in range(1, 6):
+        command = ['taskset', '-c', '44-51', 'timeout', '60', binary, case]
+        r = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+        (tree / f'run-{run}.log').write_text(r.stdout)
+        expected = r.returncode == -signal.SIGABRT if diagnostic is None else (
+            r.returncode == 1 and f'FAIL: {diagnostic}' in r.stdout)
+        results.append({'variant':name, 'case':case, 'run':run, 'returncode':r.returncode,
+                        'expected_failure':expected, 'output':r.stdout, 'command':command})
+        assert expected, (name, r.returncode, r.stdout)
+    print(f'{name}: rejected 5/5; {r.stdout.strip()}', flush=True)
+    (out / 'negative-results.json').write_text(json.dumps(results, indent=2) + '\n')
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
new file mode 100644
index 0000000000000000000000000000000000000000..bc72f719b1e93668f4c671000b1d5bad75da7ac0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
@@ -0,0 +1,16 @@
+#define _GNU_SOURCE
+#include <dlfcn.h>
+#include <stdio.h>
+#include <string.h>
+
+/* Read-only topology simulation: the process remains pinned to real CPUs 44-51.
+ * These serverless fixtures never launch workers. Only sibling metadata changes. */
+FILE *fopen(const char *path, const char *mode) {
+    static char pairs[2][7] = {"44-45\n", "46-47\n"};
+    int cpu = -1, end = 0;
+    if (sscanf(path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list%n",
+               &cpu, &end) == 1 && end && !path[end] && cpu >= 44 && cpu <= 47)
+        return fmemopen(pairs[(cpu - 44) / 2], 6, "r");
+    FILE *(*real_fopen)(const char *, const char *) = dlsym(RTLD_NEXT, "fopen");
+    return real_fopen(path, mode);
+}
diff --git a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
index 6d11e9ce5b2552d2f792115b3076459979edbe39..c04a11b7fc4a6c500632d4e2c48b7949c2dc8c6e
--- a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
+++ b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
@@ -60,7 +60,9 @@
 
     static void notify_retry() {
         Server server;
-        Config cfg; cfg.even_ifid = 1; cfg.even_ex = 1; cfg.shards = 16;
+        // The gate runs this serverless row without taskset. Automatic SMT placement requires
+        // whole sibling pairs for BOTH roles; only one IO is a tracking destination below.
+        Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "notification topology");
         server.threads_.resize(server.placement().total_threads());
         for (uint32_t i = 0; i < server.threads_.size(); ++i) {
@@ -94,7 +96,8 @@
     }
 
     static void flush() {
-        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 1; cfg.shards = 16;
+        // Two IOs still prove the foreign delivery; an even EX count also admits SMT placement.
+        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "flush topology");
         const auto& ios = server.placement().ifid_threads();
         check(ios.size() == 2, "foreign tracking IO exists");
@@ -136,7 +139,16 @@
         Server server; ThreadCtx self; IoLoop loop;
         loop.srv_ = &server; loop.self_ = &self;
         sender.bind(loop.wb_);
-        server.live_obuf_normal_hard_.store(8); server.client_obuf_armed_.store(true);
+        // Mirror Server::init's mailbox initialization without starting workers or opening rings.
+        // cx-waits made raw live-field stores writer-only; IO reads the published snapshot.
+        server.live_config_committed_ = server.capture_live_config(server.live_config_version_.load());
+        server.live_config_mailboxes_ = std::make_unique<LiveConfigMailbox[]>(server.nthreads() + 1);
+        server.live_config_mailboxes_[self.id()].init(server.live_config_committed_);
+        ClientOutputBufferLimits limits{}; limits.normal.hard_bytes = 8;
+        server.set_client_output_buffer_limits(limits);
+        check(server.client_obuf_armed() &&
+              server.client_limits_snapshot(self.id()).normal.hard_bytes == 8,
+              "hard limit published to the IO config mailbox");
         Client held(-1); held.set_id(1);
         check(held.rob().acquire(), "held ordinary operation"); held.rob().publish();
         check(!held.blocked(), "ordinary operation, not blocking exception");

exec
/bin/bash -lc "git add tests/netcmd_unit.cc NETCMDFIX.md && git diff --cached --check && git commit -m 'test: repair netcmd topology and configuration fixtures' && git status --short && git log -1 --format='%h %s'" in /home/user/Projects/cx-final-netcmdfix
 succeeded in 0ms:
[codex/netcmdfix-20260909 298bc790c] test: repair netcmd topology and configuration fixtures
 2 files changed, 230 insertions(+), 3 deletions(-)
 create mode 100644 NETCMDFIX.md
298bc790c test: repair netcmd topology and configuration fixtures

diff --git a//home/user/Projects/cx-final-netcmdfix/NETCMDFIX.md b//home/user/Projects/cx-final-netcmdfix/NETCMDFIX.md
new file mode 100644
index 0000000000000000000000000000000000000000..16724c2e2d3fa8494e027d7d8e12faf9ba097ef4
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/NETCMDFIX.md
@@ -0,0 +1,215 @@
+# Netcmd regression fixes
+
+2026-09-09. Base: `67f623716ca00b49d7cbdb8d33ab6b643b3e87ad` (`cx-final`).
+Worktree: `/home/user/Projects/cx-final-netcmdfix`, branch `codex/netcmdfix-20260909`.
+
+All three verdicts are **(b): invalid test-fixture assumptions**. The notification,
+flush-ordering, and output-limit properties remain required. No existing behavioral
+assertion was removed or relaxed. The patch changes only `tests/netcmd_unit.cc` and
+this report; production code and layouts are unchanged.
+
+## Evidence and changes per row
+
+### `netcmd notify-retry regression` — (b)
+
+The gate drives `build/netcmd-unit notify-retry`, not `tests/notify.py` or
+`tests/netcmd.py`. Unlike the gate's server boots, its serverless unit invocation
+at `tests/gate.sh:396` has **no taskset wrapper**. The saved failure was:
+
+```text
+--ratio: SMT placement requires even logical io and ex counts
+FAIL: notification topology
+```
+
+The erroneous assumption was that `prepare_boot` must accept a `1:1` IO/EX
+topology regardless of the process affinity. `Server::prepare_boot` automatically
+selects SMT placement when discovery finds a complete sibling pair;
+`Placement::build_even_smt` requires an even count for each role. A correct server
+must reject `1:1` in that topology. The failure occurs before the queue is filled
+or the notification code runs.
+
+The unmodified test passed **5/5** on cores 44–51, which contain no complete SMT
+pairs. With simulated sibling metadata it reproduced the exact saved failure
+**5/5**, while execution remained confined to those same cores. This is a topology
+dependency, not a probabilistic queue failure.
+
+Automatic SMT selection came from **cx-knobs**, integrated by `c8e61f646`. That
+commit is already an ancestor of netcmd's test-introduction commit `e94b97100`.
+Thus this assumption was wrong when the test was written; a subsequent overlap,
+read-local, vestigial-code, or resize merge did not introduce it. The original
+netcmd report describes validation on pinned physical cores, which explains why
+those runs did not expose it.
+
+**Change:** use `2:2`, valid with physical cores or complete SMT pairs. The tracking
+mask still selects exactly one IO. The actual producer queue must fill, the failed
+post must report zero completed work, the keyless record must survive, no payload
+may escape the failed post, and the drained retry must deliver exactly the original
+invalidation. Every one of those checks is unchanged.
+
+### `netcmd flush regression` — (b)
+
+This row drives `build/netcmd-unit flush`. Its saved failure was the same SMT
+diagnostic followed by `FAIL: flush topology`. Its erroneous assumption was that
+`prepare_boot` must accept `2:1` under any affinity. The odd EX count violates the
+same existing SMT contract. Attribution is likewise **cx-knobs / an invalid
+netcmd fixture at introduction**, not a new flush-path behavior.
+
+The original test passed **5/5** on cores 44–51 and reproduced the topology
+failure **5/5** under simulated SMT metadata. Neither failure reached dispatch or
+flush completion.
+
+**Change:** use `2:2`. The fixture still asserts exactly two IOs and selects a
+foreign tracking IO. The dispatch gate must produce no invalidation before owner
+completion; after completion, the test must receive exactly one `TrackingFlush`.
+Those assertions and the completion ordering are unchanged. The extra executor
+exists only to satisfy placement; the unit starts no workers.
+
+### `netcmd output regression` — (b)
+
+The original test crashed **5/5**, including at the physical-core geometry. GDB
+identified the failure precisely:
+
+```text
+SIGSEGV
+Server::client_limits_snapshot(reader_tid=0)  src/core/server.h:2225
+IoLoop::client_obuf_check                    src/core/io_loop.h:6005
+NetcmdRegression::output                    tests/netcmd_unit.cc:146 (base)
+```
+
+`live_config_mailboxes_` was null. The erroneous fixture assumption was that a
+default-constructed Server plus direct stores to `live_obuf_normal_hard_` and
+`client_obuf_armed_` supplied all state consumed by `client_obuf_check`.
+
+**cx-waits** changed that contract in `c33228203`, merged here by `818d458ef`.
+Before that merge, `client_limits_snapshot()` collected the mutable live atomics.
+It now reads a committed per-consumer mailbox using the worker ID, without reader
+retries. Production `Server::init` allocates and initializes those mailboxes before
+workers run. The hand-built netcmd fixture bypassed that initialization and wrote
+only the CONFIG producer's private fields. Adding a null fallback or restoring
+the old reader loop in production would conceal the invalid fixture and conflict
+with the intended publication design.
+
+**Change:** initialize the fixture's sole config-consumer mailbox using the same
+committed-value initialization as `Server::init`, then call the real
+`set_client_output_buffer_limits` publisher. A new assertion requires both the
+armed selector and an eight-byte hard limit in the IO's published snapshot before
+the behavioral test proceeds.
+
+The seven-byte uncounted reply must still finish sending without subtraction from
+zero. The separate nine-byte deferred push must still exceed the **unchanged
+eight-byte** limit and close a client with an unfinished ordinary ROB operation.
+All bytes must still exist exclusively in deferred storage, and the operation must
+still be nonblocking. No threshold, timing allowance, or exception was changed.
+
+## Repeated validation
+
+A fresh worktree had no build artifacts. Commands used for the initial clean build:
+
+```sh
+taskset -c 44-55 make clean
+taskset -c 44-55 make -j6 build/netcmd-unit build/tomokv
+```
+
+The changed unit translation unit was then rebuilt. All eight locked layout
+assertions compiled unchanged. Test invocations retained the gate's 60-second
+unit timeout:
+
+```sh
+for row in notify-retry flush output; do
+  for attempt in 1 2 3 4 5; do
+    taskset -c 44-51 timeout 60 ./build/netcmd-unit "$row"
+  done
+done
+```
+
+| Row | PRE physical cores | PRE simulated SMT | POST physical cores | POST simulated SMT |
+|---|---:|---:|---:|---:|
+| notify-retry | 5/5 pass | 0/5 pass | 5/5 pass | 5/5 pass |
+| flush | 5/5 pass | 0/5 pass | 5/5 pass | 5/5 pass |
+| output | 0/5 pass, SIGSEGV | Not run | 5/5 pass | 5/5 pass |
+
+The simulation declares CPUs 44–47 as one L3 domain and uses a disposable
+`fopen` interposer to report sibling pairs 44/45 and 46/47. It changes only sysfs
+read results inside the serverless fixture. Neither actual affinity nor host
+topology is changed, no worker is launched, and no CPU outside 44–55 is used.
+It exercises the real SMT placement implementation. The machine's actual complete
+SMT pairs extend outside the assigned cores, so an actual unpinned rerun was not
+performed. Saved gate diagnostics and the simulated failures agree exactly.
+
+The interposer and exact commands are preserved in
+`build/netcmdfix/smt-sysfs.c` and `pre-smt-results.json` / `post-results.json` under
+that same directory. For example:
+
+```sh
+taskset -c 44-51 timeout 60 env TOMOKV_L3_DOMAINS=44-47 \
+  LD_PRELOAD="$PWD/build/netcmdfix/smt-sysfs.so" ./build/netcmd-unit notify-retry
+```
+
+## Tests reject broken mechanisms
+
+These controls were **built and run**, using disposable copies under
+`build/netcmdfix/mutants/`. They never replaced production sources, objects, or the
+good binaries. Each control was run five times; only the expected assertion or
+identified accounting abort counted as a successful rejection.
+
+| Deliberate break | Required failure observed | Rejections |
+|---|---|---:|
+| In `notify_ex_pass_entry`, pop the keyless record and return `work + 1` when posting its invalidation fails. | `failed marker post is incomplete work` | 5/5 |
+| Restore `tracking_broadcast_flush()` at dispatch in `climon_armed_gate`. | `no flush invalidation before owner work was published` | 5/5 |
+| Return immediately from `climon_flush_completed`, omitting its broadcast. | `completed flush invalidates foreign tracking IO` | 5/5 |
+| Remove the untracked Wb specializations' `stop_obuf_tracking()` calls. | SIGABRT in `Client::commit_write(7)` at `conn.h:419`, confirmed with GDB. | 5/5 |
+| Omit `wb_.deferred_output_bytes(*c)` from `client_obuf_check`'s used-byte total. | `hard limit fired before Done` | 5/5 |
+
+The mutation descriptions above are also the instructions for inducing each
+failure. Exact replacements, compile/link commands, expected diagnostics, and
+per-attempt results are retained in `build/netcmdfix/negative_controls.py`, each
+mutant's `commands.json`, and `build/netcmdfix/negative-results.json`.
+
+## Live integration
+
+The unchanged `tests/netcmd.py` battery also passed **48/48** attempts, with a fresh
+server for each attempt. This includes its real RESP3 tracking invalidation after
+FLUSHALL and the following GET observing the cleared key; this supplements the
+unit's direct completion-hook checks.
+
+| Thread mode | Atomic | Read-local | Overlap 0 | Overlap 1 |
+|---|---:|---:|---:|---:|
+| split | 0 | 0 | 3/3 | 3/3 |
+| split | 0 | 1 | 3/3 | 3/3 |
+| split | 1 | 0 | 3/3 | 3/3 |
+| split | 1 | 1 | 3/3 | 3/3 |
+| fused | 0 | 0 | 3/3 | 3/3 |
+| fused | 0 | 1 | 3/3 | 3/3 |
+| fused | 1 | 0 | 3/3 | 3/3 |
+| fused | 1 | 1 | 3/3 | 3/3 |
+
+Servers used io_uring, cores **44–51**, **16 shards**, and split ratio **6:2**.
+Fused boots omitted `--ratio`. Battery clients used cores **52–55**. Each boot's
+INFO response was saved and checked for its thread mode and effective read-local
+setting. These are netcmd integration passes under those configurations, not a
+measurement of read-local hit coverage or performance.
+
+Ports used were **8580–8589 and 8596–8601**. A preflight found a pre-existing Garnet
+listener on 8590 before starting any test there; the remaining cells resumed on
+unused permitted ports. That listener was neither contacted nor stopped. All 48
+owned server PIDs exited cleanly with status zero. The harness stopped only its
+own PIDs and never used a pattern kill.
+
+`build/netcmdfix/live_matrix.py`, `live/results.json`, and each attempt's
+`server.log`, `info-server.log`, and `battery.log` preserve the commands and evidence.
+Build logs, original failure logs, both debugger traces, and binary SHA-256 values
+are also under `build/netcmdfix/`. These are local, ignored validation artifacts.
+
+## Scope and remaining failures
+
+**No failure among the three requested rows remains unattributed.** The unchanged
+notification, flush hook, and Wb implementations are identical to the netcmd merge
+`821d207b1`; the relevant subsequent production change is the cx-waits configuration
+reader used by the output fixture. The live matrix also covers both restored
+overlap schedules and the read-local option in both thread modes.
+
+No row was added, deleted, or moved. The three rows remain above the quick-tier
+exit; the gate-count delta is **0 quick / 0 full**. `tests/gate.sh`, including
+`EXPECT_QUICK` and `EXPECT_FULL`, was not edited. No full gate or benchmark was run.
+There is no performance claim. The user's changes in the original `cx-final`
+worktree were left untouched.
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
new file mode 100644
index 0000000000000000000000000000000000000000..295ae00928add7346879af86ca8bfdb274f46beb
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
@@ -0,0 +1,85 @@
+"""Run only tests/netcmd.py; each attempt owns one server PID and private data directory."""
+import itertools
+import json
+from pathlib import Path
+import resource
+import socket
+import subprocess
+import sys
+import time
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix/live'
+out.mkdir(exist_ok=True)
+sys.path.insert(0, str(root / 'tests'))
+from _lib import Conn
+
+results_file = out / 'results.json'
+results = json.loads(results_file.read_text()) if results_file.exists() else []
+cells = itertools.product(('split', 'fused'), (0, 1), (0, 1), (0, 1))
+for cell, (mode, atomic, read_local, overlap) in enumerate(cells):
+    # 8590 belongs to an existing Garnet PID; resume the remaining cells on unused ports.
+    port = 8580 + cell if cell < 10 else 8596 + cell - 10
+    name = f'{mode}-a{atomic}-rl{read_local}-overlap{overlap}'
+    for attempt in range(1, 4):
+        if any(r['cell'] == name and r['attempt'] == attempt for r in results):
+            continue  # preserve completed attempts, including any failure, when resuming
+        directory = out / name / str(attempt)
+        directory.mkdir(parents=True, exist_ok=True)
+        with socket.socket() as guard:
+            guard.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
+            guard.bind(('127.0.0.1', port))
+        command = ['taskset', '-c', '44-51', './build/tomokv', '--bind', '127.0.0.1',
+                   '--port', str(port), '--shards', '16', '--thread-mode', mode,
+                   '--atomic', str(atomic), '--read-local', str(read_local),
+                   '--overlap', str(overlap), '--enable-debug-command', 'yes',
+                   '--dir', str(directory)]
+        if mode == 'split':
+            command += ['--ratio', '6:2']
+        record = dict(cell=name, attempt=attempt, port=port, command=command)
+        with (directory / 'server.log').open('w') as log:
+            server = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
+            record['pid'] = server.pid
+            try:
+                deadline = time.monotonic() + 30
+                while True:
+                    assert server.poll() is None, f'{name}: server exited {server.returncode}'
+                    try:
+                        connection = Conn('127.0.0.1', port, timeout=2)
+                        break
+                    except OSError:
+                        assert time.monotonic() < deadline, f'{name}: boot timeout'
+                        time.sleep(0.05)
+                try:
+                    assert connection.cmd('PING') == b'PONG'
+                    info = connection.cmd('INFO', 'server').decode()
+                    (directory / 'info-server.log').write_text(info)
+                    fields = dict(line.split(':', 1) for line in info.splitlines() if ':' in line)
+                    assert fields['thread_mode'] == ('2s' if mode == 'split' else '1s'), fields
+                    assert fields['read_local'] == str(read_local), fields
+                finally:
+                    connection.close()
+                test = ['taskset', '-c', '52-55', 'timeout', '60', 'python3', '-u',
+                        'tests/netcmd.py', '127.0.0.1', str(port)]
+                result = subprocess.run(test, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+                (directory / 'battery.log').write_text(result.stdout)
+                record.update(returncode=result.returncode, test_command=test)
+            except Exception as exc:
+                record.update(returncode=1, error=repr(exc))
+            finally:
+                if server.poll() is None:
+                    server.terminate()
+                    try:
+                        server.wait(timeout=20)
+                    except subprocess.TimeoutExpired:
+                        server.kill()
+                        server.wait()
+                record['server_exit'] = server.returncode
+        results.append(record)
+        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
+        if record['returncode'] or record['server_exit']:
+            print(f'FAIL {name} attempt {attempt}: {record}', flush=True)
+    cell_results = [r for r in results if r['cell'] == name]
+    print(f'{name}: {sum(r["returncode"] == 0 and r["server_exit"] == 0 for r in cell_results)}/3', flush=True)
+raise SystemExit(any(r['returncode'] or r['server_exit'] for r in results))
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
new file mode 100644
index 0000000000000000000000000000000000000000..b6494711491bb3710f859aa5fb7e295a4e3b093b
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
@@ -0,0 +1,77 @@
+"""Disposable source copies only; never modifies the production sources or objects."""
+import json
+from pathlib import Path
+import resource
+import shlex
+import shutil
+import signal
+import subprocess
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix'
+recipes = subprocess.check_output(['make', '-n', '-B', 'build/netcmd-unit'], text=True).splitlines()
+link = shlex.split(next(line for line in recipes if ' -o build/netcmd-unit ' in line))
+variants = [
+    ('notify-drop', 'notify-retry', 'src/cmd/notify.inc',
+     'return work;  // keep this target and the keyless record for the next pass',
+     'slot->keyless.pop_front(); return work + 1;  // deliberately lose the invalidation',
+     'tests/netcmd_unit.cc', 'failed marker post is incomplete work'),
+    ('flush-early', 'flush', 'src/cmd/climon.cc',
+     'if (armed & Server::kClimonTracking) {\n        ClimonConn* state',
+     'if (armed & Server::kClimonTracking) {\n'
+     '        if (op.cmd_name().eq_icase("flushall") || op.cmd_name().eq_icase("flushdb"))\n'
+     '            tracking_broadcast_flush();\n        ClimonConn* state',
+     'src/cmd/climon.cc', 'no flush invalidation before owner work was published'),
+    ('flush-missing', 'flush', 'src/cmd/climon.cc',
+     'void IoLoop::climon_flush_completed(Op& op) {\n',
+     'void IoLoop::climon_flush_completed(Op& op) {\n    return; // omit completed invalidation\n',
+     'src/cmd/climon.cc', 'completed flush invalidates foreign tracking IO'),
+    ('output-untracked', 'output', 'src/net/wb.h',
+     'else conn.stop_obuf_tracking();',
+     'else {} // deliberately leave accounting armed for uncounted bytes',
+     'tests/netcmd_unit.cc', None),
+    ('output-deferred', 'output', 'src/core/io_loop.h',
+     'uint64_t used = c->obuf_bytes() + wb_.deferred_output_bytes(*c);',
+     'uint64_t used = c->obuf_bytes(); // deliberately omit deferred storage',
+     'tests/netcmd_unit.cc', 'hard limit fired before Done'),
+]
+results = []
+for name, case, source, before, after, tu, diagnostic in variants:
+    tree = out / 'mutants' / name
+    tree.mkdir(parents=True)
+    shutil.copytree(root / 'src', tree / 'src')
+    (tree / 'third_party').symlink_to(root / 'third_party', target_is_directory=True)
+    (tree / 'tests').mkdir()
+    for test in ('netcmd_unit.cc', 'netcmd_unit.h'):
+        shutil.copy2(root / 'tests' / test, tree / 'tests' / test)
+    target = tree / source
+    original = target.read_text()
+    assert original.count(before) == (2 if name == 'output-untracked' else 1)
+    target.write_text(original.replace(before, after))
+    old_object = 'build/tests/netcmd_unit.o' if tu.startswith('tests/') else 'build/src/cmd/climon.o'
+    compile_cmd = shlex.split(next(line for line in recipes if f' -c {tu} -o {old_object}' in line))
+    new_object = str(tree / 'mutant.o')
+    compile_cmd = [str(tree / tu) if arg == tu else new_object if arg == old_object
+                   else f'-I{tree}' if arg == '-I.' else arg for arg in compile_cmd]
+    binary = str(tree / 'netcmd-unit')
+    link_cmd = [new_object if arg == old_object else binary if arg == 'build/netcmd-unit' else arg
+                for arg in link]
+    print(f'building {name}', flush=True)
+    (tree / 'commands.json').write_text(json.dumps([compile_cmd, link_cmd], indent=2) + '\n')
+    with (tree / 'build.log').open('w') as log:
+        subprocess.run(['taskset', '-c', '44-55', *compile_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=300)
+        subprocess.run(['taskset', '-c', '44-55', *link_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=120)
+    for run in range(1, 6):
+        command = ['taskset', '-c', '44-51', 'timeout', '60', binary, case]
+        r = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+        (tree / f'run-{run}.log').write_text(r.stdout)
+        expected = r.returncode == -signal.SIGABRT if diagnostic is None else (
+            r.returncode == 1 and f'FAIL: {diagnostic}' in r.stdout)
+        results.append({'variant':name, 'case':case, 'run':run, 'returncode':r.returncode,
+                        'expected_failure':expected, 'output':r.stdout, 'command':command})
+        assert expected, (name, r.returncode, r.stdout)
+    print(f'{name}: rejected 5/5; {r.stdout.strip()}', flush=True)
+    (out / 'negative-results.json').write_text(json.dumps(results, indent=2) + '\n')
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
new file mode 100644
index 0000000000000000000000000000000000000000..bc72f719b1e93668f4c671000b1d5bad75da7ac0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
@@ -0,0 +1,16 @@
+#define _GNU_SOURCE
+#include <dlfcn.h>
+#include <stdio.h>
+#include <string.h>
+
+/* Read-only topology simulation: the process remains pinned to real CPUs 44-51.
+ * These serverless fixtures never launch workers. Only sibling metadata changes. */
+FILE *fopen(const char *path, const char *mode) {
+    static char pairs[2][7] = {"44-45\n", "46-47\n"};
+    int cpu = -1, end = 0;
+    if (sscanf(path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list%n",
+               &cpu, &end) == 1 && end && !path[end] && cpu >= 44 && cpu <= 47)
+        return fmemopen(pairs[(cpu - 44) / 2], 6, "r");
+    FILE *(*real_fopen)(const char *, const char *) = dlsym(RTLD_NEXT, "fopen");
+    return real_fopen(path, mode);
+}
diff --git a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
index 6d11e9ce5b2552d2f792115b3076459979edbe39..c04a11b7fc4a6c500632d4e2c48b7949c2dc8c6e
--- a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
+++ b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
@@ -60,7 +60,9 @@
 
     static void notify_retry() {
         Server server;
-        Config cfg; cfg.even_ifid = 1; cfg.even_ex = 1; cfg.shards = 16;
+        // The gate runs this serverless row without taskset. Automatic SMT placement requires
+        // whole sibling pairs for BOTH roles; only one IO is a tracking destination below.
+        Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "notification topology");
         server.threads_.resize(server.placement().total_threads());
         for (uint32_t i = 0; i < server.threads_.size(); ++i) {
@@ -94,7 +96,8 @@
     }
 
     static void flush() {
-        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 1; cfg.shards = 16;
+        // Two IOs still prove the foreign delivery; an even EX count also admits SMT placement.
+        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "flush topology");
         const auto& ios = server.placement().ifid_threads();
         check(ios.size() == 2, "foreign tracking IO exists");
@@ -136,7 +139,16 @@
         Server server; ThreadCtx self; IoLoop loop;
         loop.srv_ = &server; loop.self_ = &self;
         sender.bind(loop.wb_);
-        server.live_obuf_normal_hard_.store(8); server.client_obuf_armed_.store(true);
+        // Mirror Server::init's mailbox initialization without starting workers or opening rings.
+        // cx-waits made raw live-field stores writer-only; IO reads the published snapshot.
+        server.live_config_committed_ = server.capture_live_config(server.live_config_version_.load());
+        server.live_config_mailboxes_ = std::make_unique<LiveConfigMailbox[]>(server.nthreads() + 1);
+        server.live_config_mailboxes_[self.id()].init(server.live_config_committed_);
+        ClientOutputBufferLimits limits{}; limits.normal.hard_bytes = 8;
+        server.set_client_output_buffer_limits(limits);
+        check(server.client_obuf_armed() &&
+              server.client_limits_snapshot(self.id()).normal.hard_bytes == 8,
+              "hard limit published to the IO config mailbox");
         Client held(-1); held.set_id(1);
         check(held.rob().acquire(), "held ordinary operation"); held.rob().publish();
         check(!held.blocked(), "ordinary operation, not blocking exception");

codex
Committed `298bc790c` on `codex/netcmdfix-20260909`.

All three are **(b)**: notify-retry and flush assumed invalid SMT topologies; output omitted the configuration mailbox introduced by `cx-waits`. Behavioral assertions remain intact.

- Each repaired row: **10/10 passed**
- Live netcmd battery: **48/48 passed**
- Five deliberately broken variants: **5/5 rejected each**

Evidence and reproduction details: [NETCMDFIX.md](/home/user/Projects/cx-final-netcmdfix/NETCMDFIX.md).

No full gate or benchmarks ran.
diff --git a//home/user/Projects/cx-final-netcmdfix/NETCMDFIX.md b//home/user/Projects/cx-final-netcmdfix/NETCMDFIX.md
new file mode 100644
index 0000000000000000000000000000000000000000..16724c2e2d3fa8494e027d7d8e12faf9ba097ef4
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/NETCMDFIX.md
@@ -0,0 +1,215 @@
+# Netcmd regression fixes
+
+2026-09-09. Base: `67f623716ca00b49d7cbdb8d33ab6b643b3e87ad` (`cx-final`).
+Worktree: `/home/user/Projects/cx-final-netcmdfix`, branch `codex/netcmdfix-20260909`.
+
+All three verdicts are **(b): invalid test-fixture assumptions**. The notification,
+flush-ordering, and output-limit properties remain required. No existing behavioral
+assertion was removed or relaxed. The patch changes only `tests/netcmd_unit.cc` and
+this report; production code and layouts are unchanged.
+
+## Evidence and changes per row
+
+### `netcmd notify-retry regression` — (b)
+
+The gate drives `build/netcmd-unit notify-retry`, not `tests/notify.py` or
+`tests/netcmd.py`. Unlike the gate's server boots, its serverless unit invocation
+at `tests/gate.sh:396` has **no taskset wrapper**. The saved failure was:
+
+```text
+--ratio: SMT placement requires even logical io and ex counts
+FAIL: notification topology
+```
+
+The erroneous assumption was that `prepare_boot` must accept a `1:1` IO/EX
+topology regardless of the process affinity. `Server::prepare_boot` automatically
+selects SMT placement when discovery finds a complete sibling pair;
+`Placement::build_even_smt` requires an even count for each role. A correct server
+must reject `1:1` in that topology. The failure occurs before the queue is filled
+or the notification code runs.
+
+The unmodified test passed **5/5** on cores 44–51, which contain no complete SMT
+pairs. With simulated sibling metadata it reproduced the exact saved failure
+**5/5**, while execution remained confined to those same cores. This is a topology
+dependency, not a probabilistic queue failure.
+
+Automatic SMT selection came from **cx-knobs**, integrated by `c8e61f646`. That
+commit is already an ancestor of netcmd's test-introduction commit `e94b97100`.
+Thus this assumption was wrong when the test was written; a subsequent overlap,
+read-local, vestigial-code, or resize merge did not introduce it. The original
+netcmd report describes validation on pinned physical cores, which explains why
+those runs did not expose it.
+
+**Change:** use `2:2`, valid with physical cores or complete SMT pairs. The tracking
+mask still selects exactly one IO. The actual producer queue must fill, the failed
+post must report zero completed work, the keyless record must survive, no payload
+may escape the failed post, and the drained retry must deliver exactly the original
+invalidation. Every one of those checks is unchanged.
+
+### `netcmd flush regression` — (b)
+
+This row drives `build/netcmd-unit flush`. Its saved failure was the same SMT
+diagnostic followed by `FAIL: flush topology`. Its erroneous assumption was that
+`prepare_boot` must accept `2:1` under any affinity. The odd EX count violates the
+same existing SMT contract. Attribution is likewise **cx-knobs / an invalid
+netcmd fixture at introduction**, not a new flush-path behavior.
+
+The original test passed **5/5** on cores 44–51 and reproduced the topology
+failure **5/5** under simulated SMT metadata. Neither failure reached dispatch or
+flush completion.
+
+**Change:** use `2:2`. The fixture still asserts exactly two IOs and selects a
+foreign tracking IO. The dispatch gate must produce no invalidation before owner
+completion; after completion, the test must receive exactly one `TrackingFlush`.
+Those assertions and the completion ordering are unchanged. The extra executor
+exists only to satisfy placement; the unit starts no workers.
+
+### `netcmd output regression` — (b)
+
+The original test crashed **5/5**, including at the physical-core geometry. GDB
+identified the failure precisely:
+
+```text
+SIGSEGV
+Server::client_limits_snapshot(reader_tid=0)  src/core/server.h:2225
+IoLoop::client_obuf_check                    src/core/io_loop.h:6005
+NetcmdRegression::output                    tests/netcmd_unit.cc:146 (base)
+```
+
+`live_config_mailboxes_` was null. The erroneous fixture assumption was that a
+default-constructed Server plus direct stores to `live_obuf_normal_hard_` and
+`client_obuf_armed_` supplied all state consumed by `client_obuf_check`.
+
+**cx-waits** changed that contract in `c33228203`, merged here by `818d458ef`.
+Before that merge, `client_limits_snapshot()` collected the mutable live atomics.
+It now reads a committed per-consumer mailbox using the worker ID, without reader
+retries. Production `Server::init` allocates and initializes those mailboxes before
+workers run. The hand-built netcmd fixture bypassed that initialization and wrote
+only the CONFIG producer's private fields. Adding a null fallback or restoring
+the old reader loop in production would conceal the invalid fixture and conflict
+with the intended publication design.
+
+**Change:** initialize the fixture's sole config-consumer mailbox using the same
+committed-value initialization as `Server::init`, then call the real
+`set_client_output_buffer_limits` publisher. A new assertion requires both the
+armed selector and an eight-byte hard limit in the IO's published snapshot before
+the behavioral test proceeds.
+
+The seven-byte uncounted reply must still finish sending without subtraction from
+zero. The separate nine-byte deferred push must still exceed the **unchanged
+eight-byte** limit and close a client with an unfinished ordinary ROB operation.
+All bytes must still exist exclusively in deferred storage, and the operation must
+still be nonblocking. No threshold, timing allowance, or exception was changed.
+
+## Repeated validation
+
+A fresh worktree had no build artifacts. Commands used for the initial clean build:
+
+```sh
+taskset -c 44-55 make clean
+taskset -c 44-55 make -j6 build/netcmd-unit build/tomokv
+```
+
+The changed unit translation unit was then rebuilt. All eight locked layout
+assertions compiled unchanged. Test invocations retained the gate's 60-second
+unit timeout:
+
+```sh
+for row in notify-retry flush output; do
+  for attempt in 1 2 3 4 5; do
+    taskset -c 44-51 timeout 60 ./build/netcmd-unit "$row"
+  done
+done
+```
+
+| Row | PRE physical cores | PRE simulated SMT | POST physical cores | POST simulated SMT |
+|---|---:|---:|---:|---:|
+| notify-retry | 5/5 pass | 0/5 pass | 5/5 pass | 5/5 pass |
+| flush | 5/5 pass | 0/5 pass | 5/5 pass | 5/5 pass |
+| output | 0/5 pass, SIGSEGV | Not run | 5/5 pass | 5/5 pass |
+
+The simulation declares CPUs 44–47 as one L3 domain and uses a disposable
+`fopen` interposer to report sibling pairs 44/45 and 46/47. It changes only sysfs
+read results inside the serverless fixture. Neither actual affinity nor host
+topology is changed, no worker is launched, and no CPU outside 44–55 is used.
+It exercises the real SMT placement implementation. The machine's actual complete
+SMT pairs extend outside the assigned cores, so an actual unpinned rerun was not
+performed. Saved gate diagnostics and the simulated failures agree exactly.
+
+The interposer and exact commands are preserved in
+`build/netcmdfix/smt-sysfs.c` and `pre-smt-results.json` / `post-results.json` under
+that same directory. For example:
+
+```sh
+taskset -c 44-51 timeout 60 env TOMOKV_L3_DOMAINS=44-47 \
+  LD_PRELOAD="$PWD/build/netcmdfix/smt-sysfs.so" ./build/netcmd-unit notify-retry
+```
+
+## Tests reject broken mechanisms
+
+These controls were **built and run**, using disposable copies under
+`build/netcmdfix/mutants/`. They never replaced production sources, objects, or the
+good binaries. Each control was run five times; only the expected assertion or
+identified accounting abort counted as a successful rejection.
+
+| Deliberate break | Required failure observed | Rejections |
+|---|---|---:|
+| In `notify_ex_pass_entry`, pop the keyless record and return `work + 1` when posting its invalidation fails. | `failed marker post is incomplete work` | 5/5 |
+| Restore `tracking_broadcast_flush()` at dispatch in `climon_armed_gate`. | `no flush invalidation before owner work was published` | 5/5 |
+| Return immediately from `climon_flush_completed`, omitting its broadcast. | `completed flush invalidates foreign tracking IO` | 5/5 |
+| Remove the untracked Wb specializations' `stop_obuf_tracking()` calls. | SIGABRT in `Client::commit_write(7)` at `conn.h:419`, confirmed with GDB. | 5/5 |
+| Omit `wb_.deferred_output_bytes(*c)` from `client_obuf_check`'s used-byte total. | `hard limit fired before Done` | 5/5 |
+
+The mutation descriptions above are also the instructions for inducing each
+failure. Exact replacements, compile/link commands, expected diagnostics, and
+per-attempt results are retained in `build/netcmdfix/negative_controls.py`, each
+mutant's `commands.json`, and `build/netcmdfix/negative-results.json`.
+
+## Live integration
+
+The unchanged `tests/netcmd.py` battery also passed **48/48** attempts, with a fresh
+server for each attempt. This includes its real RESP3 tracking invalidation after
+FLUSHALL and the following GET observing the cleared key; this supplements the
+unit's direct completion-hook checks.
+
+| Thread mode | Atomic | Read-local | Overlap 0 | Overlap 1 |
+|---|---:|---:|---:|---:|
+| split | 0 | 0 | 3/3 | 3/3 |
+| split | 0 | 1 | 3/3 | 3/3 |
+| split | 1 | 0 | 3/3 | 3/3 |
+| split | 1 | 1 | 3/3 | 3/3 |
+| fused | 0 | 0 | 3/3 | 3/3 |
+| fused | 0 | 1 | 3/3 | 3/3 |
+| fused | 1 | 0 | 3/3 | 3/3 |
+| fused | 1 | 1 | 3/3 | 3/3 |
+
+Servers used io_uring, cores **44–51**, **16 shards**, and split ratio **6:2**.
+Fused boots omitted `--ratio`. Battery clients used cores **52–55**. Each boot's
+INFO response was saved and checked for its thread mode and effective read-local
+setting. These are netcmd integration passes under those configurations, not a
+measurement of read-local hit coverage or performance.
+
+Ports used were **8580–8589 and 8596–8601**. A preflight found a pre-existing Garnet
+listener on 8590 before starting any test there; the remaining cells resumed on
+unused permitted ports. That listener was neither contacted nor stopped. All 48
+owned server PIDs exited cleanly with status zero. The harness stopped only its
+own PIDs and never used a pattern kill.
+
+`build/netcmdfix/live_matrix.py`, `live/results.json`, and each attempt's
+`server.log`, `info-server.log`, and `battery.log` preserve the commands and evidence.
+Build logs, original failure logs, both debugger traces, and binary SHA-256 values
+are also under `build/netcmdfix/`. These are local, ignored validation artifacts.
+
+## Scope and remaining failures
+
+**No failure among the three requested rows remains unattributed.** The unchanged
+notification, flush hook, and Wb implementations are identical to the netcmd merge
+`821d207b1`; the relevant subsequent production change is the cx-waits configuration
+reader used by the output fixture. The live matrix also covers both restored
+overlap schedules and the read-local option in both thread modes.
+
+No row was added, deleted, or moved. The three rows remain above the quick-tier
+exit; the gate-count delta is **0 quick / 0 full**. `tests/gate.sh`, including
+`EXPECT_QUICK` and `EXPECT_FULL`, was not edited. No full gate or benchmark was run.
+There is no performance claim. The user's changes in the original `cx-final`
+worktree were left untouched.
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
new file mode 100644
index 0000000000000000000000000000000000000000..295ae00928add7346879af86ca8bfdb274f46beb
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/live_matrix.py
@@ -0,0 +1,85 @@
+"""Run only tests/netcmd.py; each attempt owns one server PID and private data directory."""
+import itertools
+import json
+from pathlib import Path
+import resource
+import socket
+import subprocess
+import sys
+import time
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix/live'
+out.mkdir(exist_ok=True)
+sys.path.insert(0, str(root / 'tests'))
+from _lib import Conn
+
+results_file = out / 'results.json'
+results = json.loads(results_file.read_text()) if results_file.exists() else []
+cells = itertools.product(('split', 'fused'), (0, 1), (0, 1), (0, 1))
+for cell, (mode, atomic, read_local, overlap) in enumerate(cells):
+    # 8590 belongs to an existing Garnet PID; resume the remaining cells on unused ports.
+    port = 8580 + cell if cell < 10 else 8596 + cell - 10
+    name = f'{mode}-a{atomic}-rl{read_local}-overlap{overlap}'
+    for attempt in range(1, 4):
+        if any(r['cell'] == name and r['attempt'] == attempt for r in results):
+            continue  # preserve completed attempts, including any failure, when resuming
+        directory = out / name / str(attempt)
+        directory.mkdir(parents=True, exist_ok=True)
+        with socket.socket() as guard:
+            guard.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
+            guard.bind(('127.0.0.1', port))
+        command = ['taskset', '-c', '44-51', './build/tomokv', '--bind', '127.0.0.1',
+                   '--port', str(port), '--shards', '16', '--thread-mode', mode,
+                   '--atomic', str(atomic), '--read-local', str(read_local),
+                   '--overlap', str(overlap), '--enable-debug-command', 'yes',
+                   '--dir', str(directory)]
+        if mode == 'split':
+            command += ['--ratio', '6:2']
+        record = dict(cell=name, attempt=attempt, port=port, command=command)
+        with (directory / 'server.log').open('w') as log:
+            server = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
+            record['pid'] = server.pid
+            try:
+                deadline = time.monotonic() + 30
+                while True:
+                    assert server.poll() is None, f'{name}: server exited {server.returncode}'
+                    try:
+                        connection = Conn('127.0.0.1', port, timeout=2)
+                        break
+                    except OSError:
+                        assert time.monotonic() < deadline, f'{name}: boot timeout'
+                        time.sleep(0.05)
+                try:
+                    assert connection.cmd('PING') == b'PONG'
+                    info = connection.cmd('INFO', 'server').decode()
+                    (directory / 'info-server.log').write_text(info)
+                    fields = dict(line.split(':', 1) for line in info.splitlines() if ':' in line)
+                    assert fields['thread_mode'] == ('2s' if mode == 'split' else '1s'), fields
+                    assert fields['read_local'] == str(read_local), fields
+                finally:
+                    connection.close()
+                test = ['taskset', '-c', '52-55', 'timeout', '60', 'python3', '-u',
+                        'tests/netcmd.py', '127.0.0.1', str(port)]
+                result = subprocess.run(test, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+                (directory / 'battery.log').write_text(result.stdout)
+                record.update(returncode=result.returncode, test_command=test)
+            except Exception as exc:
+                record.update(returncode=1, error=repr(exc))
+            finally:
+                if server.poll() is None:
+                    server.terminate()
+                    try:
+                        server.wait(timeout=20)
+                    except subprocess.TimeoutExpired:
+                        server.kill()
+                        server.wait()
+                record['server_exit'] = server.returncode
+        results.append(record)
+        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
+        if record['returncode'] or record['server_exit']:
+            print(f'FAIL {name} attempt {attempt}: {record}', flush=True)
+    cell_results = [r for r in results if r['cell'] == name]
+    print(f'{name}: {sum(r["returncode"] == 0 and r["server_exit"] == 0 for r in cell_results)}/3', flush=True)
+raise SystemExit(any(r['returncode'] or r['server_exit'] for r in results))
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
new file mode 100644
index 0000000000000000000000000000000000000000..b6494711491bb3710f859aa5fb7e295a4e3b093b
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/negative_controls.py
@@ -0,0 +1,77 @@
+"""Disposable source copies only; never modifies the production sources or objects."""
+import json
+from pathlib import Path
+import resource
+import shlex
+import shutil
+import signal
+import subprocess
+
+resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
+root = Path.cwd()
+out = root / 'build/netcmdfix'
+recipes = subprocess.check_output(['make', '-n', '-B', 'build/netcmd-unit'], text=True).splitlines()
+link = shlex.split(next(line for line in recipes if ' -o build/netcmd-unit ' in line))
+variants = [
+    ('notify-drop', 'notify-retry', 'src/cmd/notify.inc',
+     'return work;  // keep this target and the keyless record for the next pass',
+     'slot->keyless.pop_front(); return work + 1;  // deliberately lose the invalidation',
+     'tests/netcmd_unit.cc', 'failed marker post is incomplete work'),
+    ('flush-early', 'flush', 'src/cmd/climon.cc',
+     'if (armed & Server::kClimonTracking) {\n        ClimonConn* state',
+     'if (armed & Server::kClimonTracking) {\n'
+     '        if (op.cmd_name().eq_icase("flushall") || op.cmd_name().eq_icase("flushdb"))\n'
+     '            tracking_broadcast_flush();\n        ClimonConn* state',
+     'src/cmd/climon.cc', 'no flush invalidation before owner work was published'),
+    ('flush-missing', 'flush', 'src/cmd/climon.cc',
+     'void IoLoop::climon_flush_completed(Op& op) {\n',
+     'void IoLoop::climon_flush_completed(Op& op) {\n    return; // omit completed invalidation\n',
+     'src/cmd/climon.cc', 'completed flush invalidates foreign tracking IO'),
+    ('output-untracked', 'output', 'src/net/wb.h',
+     'else conn.stop_obuf_tracking();',
+     'else {} // deliberately leave accounting armed for uncounted bytes',
+     'tests/netcmd_unit.cc', None),
+    ('output-deferred', 'output', 'src/core/io_loop.h',
+     'uint64_t used = c->obuf_bytes() + wb_.deferred_output_bytes(*c);',
+     'uint64_t used = c->obuf_bytes(); // deliberately omit deferred storage',
+     'tests/netcmd_unit.cc', 'hard limit fired before Done'),
+]
+results = []
+for name, case, source, before, after, tu, diagnostic in variants:
+    tree = out / 'mutants' / name
+    tree.mkdir(parents=True)
+    shutil.copytree(root / 'src', tree / 'src')
+    (tree / 'third_party').symlink_to(root / 'third_party', target_is_directory=True)
+    (tree / 'tests').mkdir()
+    for test in ('netcmd_unit.cc', 'netcmd_unit.h'):
+        shutil.copy2(root / 'tests' / test, tree / 'tests' / test)
+    target = tree / source
+    original = target.read_text()
+    assert original.count(before) == (2 if name == 'output-untracked' else 1)
+    target.write_text(original.replace(before, after))
+    old_object = 'build/tests/netcmd_unit.o' if tu.startswith('tests/') else 'build/src/cmd/climon.o'
+    compile_cmd = shlex.split(next(line for line in recipes if f' -c {tu} -o {old_object}' in line))
+    new_object = str(tree / 'mutant.o')
+    compile_cmd = [str(tree / tu) if arg == tu else new_object if arg == old_object
+                   else f'-I{tree}' if arg == '-I.' else arg for arg in compile_cmd]
+    binary = str(tree / 'netcmd-unit')
+    link_cmd = [new_object if arg == old_object else binary if arg == 'build/netcmd-unit' else arg
+                for arg in link]
+    print(f'building {name}', flush=True)
+    (tree / 'commands.json').write_text(json.dumps([compile_cmd, link_cmd], indent=2) + '\n')
+    with (tree / 'build.log').open('w') as log:
+        subprocess.run(['taskset', '-c', '44-55', *compile_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=300)
+        subprocess.run(['taskset', '-c', '44-55', *link_cmd], stdout=log, stderr=subprocess.STDOUT,
+                       check=True, timeout=120)
+    for run in range(1, 6):
+        command = ['taskset', '-c', '44-51', 'timeout', '60', binary, case]
+        r = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
+        (tree / f'run-{run}.log').write_text(r.stdout)
+        expected = r.returncode == -signal.SIGABRT if diagnostic is None else (
+            r.returncode == 1 and f'FAIL: {diagnostic}' in r.stdout)
+        results.append({'variant':name, 'case':case, 'run':run, 'returncode':r.returncode,
+                        'expected_failure':expected, 'output':r.stdout, 'command':command})
+        assert expected, (name, r.returncode, r.stdout)
+    print(f'{name}: rejected 5/5; {r.stdout.strip()}', flush=True)
+    (out / 'negative-results.json').write_text(json.dumps(results, indent=2) + '\n')
diff --git a//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
new file mode 100644
index 0000000000000000000000000000000000000000..bc72f719b1e93668f4c671000b1d5bad75da7ac0
--- /dev/null
+++ b//home/user/Projects/cx-final-netcmdfix/build/netcmdfix/smt-sysfs.c
@@ -0,0 +1,16 @@
+#define _GNU_SOURCE
+#include <dlfcn.h>
+#include <stdio.h>
+#include <string.h>
+
+/* Read-only topology simulation: the process remains pinned to real CPUs 44-51.
+ * These serverless fixtures never launch workers. Only sibling metadata changes. */
+FILE *fopen(const char *path, const char *mode) {
+    static char pairs[2][7] = {"44-45\n", "46-47\n"};
+    int cpu = -1, end = 0;
+    if (sscanf(path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list%n",
+               &cpu, &end) == 1 && end && !path[end] && cpu >= 44 && cpu <= 47)
+        return fmemopen(pairs[(cpu - 44) / 2], 6, "r");
+    FILE *(*real_fopen)(const char *, const char *) = dlsym(RTLD_NEXT, "fopen");
+    return real_fopen(path, mode);
+}
diff --git a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
index 6d11e9ce5b2552d2f792115b3076459979edbe39..c04a11b7fc4a6c500632d4e2c48b7949c2dc8c6e
--- a//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
+++ b//home/user/Projects/cx-final-netcmdfix/tests/netcmd_unit.cc
@@ -60,7 +60,9 @@
 
     static void notify_retry() {
         Server server;
-        Config cfg; cfg.even_ifid = 1; cfg.even_ex = 1; cfg.shards = 16;
+        // The gate runs this serverless row without taskset. Automatic SMT placement requires
+        // whole sibling pairs for BOTH roles; only one IO is a tracking destination below.
+        Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "notification topology");
         server.threads_.resize(server.placement().total_threads());
         for (uint32_t i = 0; i < server.threads_.size(); ++i) {
@@ -94,7 +96,8 @@
     }
 
     static void flush() {
-        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 1; cfg.shards = 16;
+        // Two IOs still prove the foreign delivery; an even EX count also admits SMT placement.
+        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 2; cfg.shards = 16;
         check(server.prepare_boot(cfg), "flush topology");
         const auto& ios = server.placement().ifid_threads();
         check(ios.size() == 2, "foreign tracking IO exists");
@@ -136,7 +139,16 @@
         Server server; ThreadCtx self; IoLoop loop;
         loop.srv_ = &server; loop.self_ = &self;
         sender.bind(loop.wb_);
-        server.live_obuf_normal_hard_.store(8); server.client_obuf_armed_.store(true);
+        // Mirror Server::init's mailbox initialization without starting workers or opening rings.
+        // cx-waits made raw live-field stores writer-only; IO reads the published snapshot.
+        server.live_config_committed_ = server.capture_live_config(server.live_config_version_.load());
+        server.live_config_mailboxes_ = std::make_unique<LiveConfigMailbox[]>(server.nthreads() + 1);
+        server.live_config_mailboxes_[self.id()].init(server.live_config_committed_);
+        ClientOutputBufferLimits limits{}; limits.normal.hard_bytes = 8;
+        server.set_client_output_buffer_limits(limits);
+        check(server.client_obuf_armed() &&
+              server.client_limits_snapshot(self.id()).normal.hard_bytes == 8,
+              "hard limit published to the IO config mailbox");
         Client held(-1); held.set_id(1);
         check(held.rob().acquire(), "held ordinary operation"); held.rob().publish();
         check(!held.blocked(), "ordinary operation, not blocking exception");

tokens used
136,426
Committed `298bc790c` on `codex/netcmdfix-20260909`.

All three are **(b)**: notify-retry and flush assumed invalid SMT topologies; output omitted the configuration mailbox introduced by `cx-waits`. Behavioral assertions remain intact.

- Each repaired row: **10/10 passed**
- Live netcmd battery: **48/48 passed**
- Five deliberately broken variants: **5/5 rejected each**

Evidence and reproduction details: [NETCMDFIX.md](/home/user/Projects/cx-final-netcmdfix/NETCMDFIX.md).

No full gate or benchmarks ran.

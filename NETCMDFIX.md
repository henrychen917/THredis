# Netcmd regression fixes

2026-09-09. Base: `67f623716ca00b49d7cbdb8d33ab6b643b3e87ad` (`cx-final`).
Worktree: `/home/user/Projects/cx-final-netcmdfix`, branch `codex/netcmdfix-20260909`.

All three verdicts are **(b): invalid test-fixture assumptions**. The notification,
flush-ordering, and output-limit properties remain required. No existing behavioral
assertion was removed or relaxed. The patch changes only `tests/netcmd_unit.cc` and
this report; production code and layouts are unchanged.

## Evidence and changes per row

### `netcmd notify-retry regression` — (b)

The gate drives `build/netcmd-unit notify-retry`, not `tests/notify.py` or
`tests/netcmd.py`. Unlike the gate's server boots, its serverless unit invocation
at `tests/gate.sh:396` has **no taskset wrapper**. The saved failure was:

```text
--ratio: SMT placement requires even logical io and ex counts
FAIL: notification topology
```

The erroneous assumption was that `prepare_boot` must accept a `1:1` IO/EX
topology regardless of the process affinity. `Server::prepare_boot` automatically
selects SMT placement when discovery finds a complete sibling pair;
`Placement::build_even_smt` requires an even count for each role. A correct server
must reject `1:1` in that topology. The failure occurs before the queue is filled
or the notification code runs.

The unmodified test passed **5/5** on cores 44–51, which contain no complete SMT
pairs. With simulated sibling metadata it reproduced the exact saved failure
**5/5**, while execution remained confined to those same cores. This is a topology
dependency, not a probabilistic queue failure.

Automatic SMT selection came from **cx-knobs**, integrated by `c8e61f646`. That
commit is already an ancestor of netcmd's test-introduction commit `e94b97100`.
Thus this assumption was wrong when the test was written; a subsequent overlap,
read-local, vestigial-code, or resize merge did not introduce it. The original
netcmd report describes validation on pinned physical cores, which explains why
those runs did not expose it.

**Change:** use `2:2`, valid with physical cores or complete SMT pairs. The tracking
mask still selects exactly one IO. The actual producer queue must fill, the failed
post must report zero completed work, the keyless record must survive, no payload
may escape the failed post, and the drained retry must deliver exactly the original
invalidation. Every one of those checks is unchanged.

### `netcmd flush regression` — (b)

This row drives `build/netcmd-unit flush`. Its saved failure was the same SMT
diagnostic followed by `FAIL: flush topology`. Its erroneous assumption was that
`prepare_boot` must accept `2:1` under any affinity. The odd EX count violates the
same existing SMT contract. Attribution is likewise **cx-knobs / an invalid
netcmd fixture at introduction**, not a new flush-path behavior.

The original test passed **5/5** on cores 44–51 and reproduced the topology
failure **5/5** under simulated SMT metadata. Neither failure reached dispatch or
flush completion.

**Change:** use `2:2`. The fixture still asserts exactly two IOs and selects a
foreign tracking IO. The dispatch gate must produce no invalidation before owner
completion; after completion, the test must receive exactly one `TrackingFlush`.
Those assertions and the completion ordering are unchanged. The extra executor
exists only to satisfy placement; the unit starts no workers.

### `netcmd output regression` — (b)

The original test crashed **5/5**, including at the physical-core geometry. GDB
identified the failure precisely:

```text
SIGSEGV
Server::client_limits_snapshot(reader_tid=0)  src/core/server.h:2225
IoLoop::client_obuf_check                    src/core/io_loop.h:6005
NetcmdRegression::output                    tests/netcmd_unit.cc:146 (base)
```

`live_config_mailboxes_` was null. The erroneous fixture assumption was that a
default-constructed Server plus direct stores to `live_obuf_normal_hard_` and
`client_obuf_armed_` supplied all state consumed by `client_obuf_check`.

**cx-waits** changed that contract in `c33228203`, merged here by `818d458ef`.
Before that merge, `client_limits_snapshot()` collected the mutable live atomics.
It now reads a committed per-consumer mailbox using the worker ID, without reader
retries. Production `Server::init` allocates and initializes those mailboxes before
workers run. The hand-built netcmd fixture bypassed that initialization and wrote
only the CONFIG producer's private fields. Adding a null fallback or restoring
the old reader loop in production would conceal the invalid fixture and conflict
with the intended publication design.

**Change:** initialize the fixture's sole config-consumer mailbox using the same
committed-value initialization as `Server::init`, then call the real
`set_client_output_buffer_limits` publisher. A new assertion requires both the
armed selector and an eight-byte hard limit in the IO's published snapshot before
the behavioral test proceeds.

The seven-byte uncounted reply must still finish sending without subtraction from
zero. The separate nine-byte deferred push must still exceed the **unchanged
eight-byte** limit and close a client with an unfinished ordinary ROB operation.
All bytes must still exist exclusively in deferred storage, and the operation must
still be nonblocking. No threshold, timing allowance, or exception was changed.

## Repeated validation

A fresh worktree had no build artifacts. Commands used for the initial clean build:

```sh
taskset -c 44-55 make clean
taskset -c 44-55 make -j6 build/netcmd-unit build/tomokv
```

The changed unit translation unit was then rebuilt. All eight locked layout
assertions compiled unchanged. Test invocations retained the gate's 60-second
unit timeout:

```sh
for row in notify-retry flush output; do
  for attempt in 1 2 3 4 5; do
    taskset -c 44-51 timeout 60 ./build/netcmd-unit "$row"
  done
done
```

| Row | PRE physical cores | PRE simulated SMT | POST physical cores | POST simulated SMT |
|---|---:|---:|---:|---:|
| notify-retry | 5/5 pass | 0/5 pass | 5/5 pass | 5/5 pass |
| flush | 5/5 pass | 0/5 pass | 5/5 pass | 5/5 pass |
| output | 0/5 pass, SIGSEGV | Not run | 5/5 pass | 5/5 pass |

The simulation declares CPUs 44–47 as one L3 domain and uses a disposable
`fopen` interposer to report sibling pairs 44/45 and 46/47. It changes only sysfs
read results inside the serverless fixture. Neither actual affinity nor host
topology is changed, no worker is launched, and no CPU outside 44–55 is used.
It exercises the real SMT placement implementation. The machine's actual complete
SMT pairs extend outside the assigned cores, so an actual unpinned rerun was not
performed. Saved gate diagnostics and the simulated failures agree exactly.

The interposer and exact commands are preserved in
`build/netcmdfix/smt-sysfs.c` and `pre-smt-results.json` / `post-results.json` under
that same directory. For example:

```sh
taskset -c 44-51 timeout 60 env TOMOKV_L3_DOMAINS=44-47 \
  LD_PRELOAD="$PWD/build/netcmdfix/smt-sysfs.so" ./build/netcmd-unit notify-retry
```

## Tests reject broken mechanisms

These controls were **built and run**, using disposable copies under
`build/netcmdfix/mutants/`. They never replaced production sources, objects, or the
good binaries. Each control was run five times; only the expected assertion or
identified accounting abort counted as a successful rejection.

| Deliberate break | Required failure observed | Rejections |
|---|---|---:|
| In `notify_ex_pass_entry`, pop the keyless record and return `work + 1` when posting its invalidation fails. | `failed marker post is incomplete work` | 5/5 |
| Restore `tracking_broadcast_flush()` at dispatch in `climon_armed_gate`. | `no flush invalidation before owner work was published` | 5/5 |
| Return immediately from `climon_flush_completed`, omitting its broadcast. | `completed flush invalidates foreign tracking IO` | 5/5 |
| Remove the untracked Wb specializations' `stop_obuf_tracking()` calls. | SIGABRT in `Client::commit_write(7)` at `conn.h:419`, confirmed with GDB. | 5/5 |
| Omit `wb_.deferred_output_bytes(*c)` from `client_obuf_check`'s used-byte total. | `hard limit fired before Done` | 5/5 |

The mutation descriptions above are also the instructions for inducing each
failure. Exact replacements, compile/link commands, expected diagnostics, and
per-attempt results are retained in `build/netcmdfix/negative_controls.py`, each
mutant's `commands.json`, and `build/netcmdfix/negative-results.json`.

## Live integration

The unchanged `tests/netcmd.py` battery also passed **48/48** attempts, with a fresh
server for each attempt. This includes its real RESP3 tracking invalidation after
FLUSHALL and the following GET observing the cleared key; this supplements the
unit's direct completion-hook checks.

| Thread mode | Atomic | Read-local | Overlap 0 | Overlap 1 |
|---|---:|---:|---:|---:|
| split | 0 | 0 | 3/3 | 3/3 |
| split | 0 | 1 | 3/3 | 3/3 |
| split | 1 | 0 | 3/3 | 3/3 |
| split | 1 | 1 | 3/3 | 3/3 |
| fused | 0 | 0 | 3/3 | 3/3 |
| fused | 0 | 1 | 3/3 | 3/3 |
| fused | 1 | 0 | 3/3 | 3/3 |
| fused | 1 | 1 | 3/3 | 3/3 |

Servers used io_uring, cores **44–51**, **16 shards**, and split ratio **6:2**.
Fused boots omitted `--ratio`. Battery clients used cores **52–55**. Each boot's
INFO response was saved and checked for its thread mode and effective read-local
setting. These are netcmd integration passes under those configurations, not a
measurement of read-local hit coverage or performance.

Ports used were **8580–8589 and 8596–8601**. A preflight found a pre-existing Garnet
listener on 8590 before starting any test there; the remaining cells resumed on
unused permitted ports. That listener was neither contacted nor stopped. All 48
owned server PIDs exited cleanly with status zero. The harness stopped only its
own PIDs and never used a pattern kill.

`build/netcmdfix/live_matrix.py`, `live/results.json`, and each attempt's
`server.log`, `info-server.log`, and `battery.log` preserve the commands and evidence.
Build logs, original failure logs, both debugger traces, and binary SHA-256 values
are also under `build/netcmdfix/`. These are local, ignored validation artifacts.

## Scope and remaining failures

**No failure among the three requested rows remains unattributed.** The unchanged
notification, flush hook, and Wb implementations are identical to the netcmd merge
`821d207b1`; the relevant subsequent production change is the cx-waits configuration
reader used by the output fixture. The live matrix also covers both restored
overlap schedules and the read-local option in both thread modes.

No row was added, deleted, or moved. The three rows remain above the quick-tier
exit; the gate-count delta is **0 quick / 0 full**. `tests/gate.sh`, including
`EXPECT_QUICK` and `EXPECT_FULL`, was not edited. No full gate or benchmark was run.
There is no performance claim. The user's changes in the original `cx-final`
worktree were left untouched.

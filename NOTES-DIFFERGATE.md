# DIFFERGATE lane notes

## What was built

- Added `tests/differ_gate.sh`, a serial full-tier matrix driver which boots one pinned vanilla
  Redis oracle and one TomoKV target, runs every leg, prints a one-line verdict per leg, and exits
  nonzero if any leg or owned-process shutdown check fails.
- Added `tests/differ.py --list-generators`. The gate consumes the live `gens` registry rather than
  duplicating its names, then appends the two special early-exit suites `wiredump` and `climon`.
- Added the driver to `tests/gate.sh` after the `quick`-tier exit. No command was added to the quick
  path, so quick-tier runtime is unchanged.
- Corrected the stale `tests/differ.py` oracle comment to vanilla Redis 7.4.2.

The driver refuses occupied target or oracle ports before boot. It records each launched PID,
resolves the listener PID with `ss`, terminates only owned PIDs, and verifies that each listener is
gone after stopping. Before any target boot, `INFO server` must contain `redis_version` and must not
contain either `tomokv_version` or `dragonfly_version`.

No TomoKV runtime knobs, hot-path code, command handlers, or storage code changed. Test-harness
inputs are the target binary/port/cores/ratio arguments plus `REDIS74_ROOT`; optional
`GATE_DIFFER_ORACLE_PORT`, `GATE_DIFFER_ORACLE_CORES`, `GATE_DIFFER_ORACLE_BIN`,
`GATE_DIFFER_REDIS_CLI`, and `GATE_DIFFER_OUT` controls exist for gate integration and directed
negative-control testing. Seeds are deliberately fixed at 7 and 19 so the gate cannot be reduced
below the required two seeds through an environment override.

## Actual discovered and executed legs

`python3 tests/differ.py --list-generators` returned these 17 registry entries, in registry order:

```
string list set zset hash hexpire xshard bitmap hll bitfield cgaps stream script streamgrp zsetops geo servertail
```

The driver appended:

```
wiredump climon
```

All 19 suites ran for atomic 0 and atomic 1, at seeds 7 and 19: 19 x 2 x 2 = 76 unique legs and
303,372 aggregate operations/checks. A parsed Cartesian-product audit found no missing or extra
legs. The driver printed this discovery and matrix proof before boot:

```
DIFFER suites (17 generators + 2 special): string list set zset hash hexpire xshard bitmap hll bitfield cgaps stream script streamgrp zsetops geo servertail wiredump climon
DIFFER matrix: atomic={0,1} seeds={7,19} legs=76 logs=/tmp/differgate-full-release
```

## Runtime and passing evidence

Assigned resources were target port 7082 on cores 8-13 and oracle port 7083 on cores 14-15.
The measured incremental full-tier differ cost on the release binary was 5m07s (306.84 seconds
wall). The exact invocation was:

```
GATE_DIFFER_OUT=/tmp/differgate-full-release \
GATE_DIFFER_ORACLE_CORES=14-15 \
/usr/bin/time -f 'wall_seconds=%e' \
  tests/differ_gate.sh ./build/tomokv 7082 7083 8-13 4:2
```

Release tail:

```
  differ climon (atomic=1 seed=19)                           ok (DIFFER climon: 4318 ops, 0 diffs -> PASS)
  target atomic=1 stop                                       ok (port 7082 free)
  oracle stop                                                ok (port 7083 free)
DIFFER GATE: pass=76 fail=0 runtime=5m07s
wall_seconds=306.84
```

The exact address-only sanitizer compile path documented by `tests/gate.sh` was also exercised:

```
g++ -std=c++20 -O1 -g -fsanitize=address -march=native -pthread -I. \
    src/main.cc src/net/tls.cc src/cmd/*.cc src/snapshot/*.cc src/persist/*.cc \
    -o /tmp/tomokv-differgate-asan -luring -pthread -lssl -lcrypto -lm
ASAN_OPTIONS=detect_leaks=1 \
GATE_DIFFER_OUT=/tmp/differgate-full-address \
GATE_DIFFER_ORACLE_CORES=14-15 \
/usr/bin/time -f 'wall_seconds=%e' \
  tests/differ_gate.sh /tmp/tomokv-differgate-asan 7082 7083 8-13 4:2
```

AddressSanitizer tail and scan:

```
  differ climon (atomic=1 seed=19)                           ok (DIFFER climon: 4318 ops, 0 diffs -> PASS)
  target atomic=1 stop                                       ok (port 7082 free)
  oracle stop                                                ok (port 7083 free)
DIFFER GATE: pass=76 fail=0 runtime=5m08s
wall_seconds=308.51
address_sanitizer_signatures=none
```

`make -j12 asan` also built successfully and its combined ASAN+UBSAN binary completed the same
76 legs with zero diffs. That run emitted one pre-existing UBSAN alignment diagnostic per atomic
boot from vendored `third_party/lua/lstring.c:87`; it emitted no AddressSanitizer or leak report.
The clean address-only run above is the preamble-approved sanitizer gate used for the final result.

Build and static checks:

```
make -j12 clean
make -j12
make -j12 asan
bash -n tests/gate.sh tests/differ_gate.sh
python3 -m py_compile tests/differ.py
git diff --check
```

## Negative controls

### Squatted oracle port

A Python listener was started on core 15/port 7083. Its listener PID was resolved with `ss` and
matched the PID just started. The final driver refused the port with status 1 and did not terminate
the squatter. The squatter PID was then terminated explicitly and the listener was verified gone.

```
guard_rc=1 squatter_pid=436483 survived_guard_pid=436483 guard_cleanup_free=yes
  port 7083 pre-boot guard                                   REFUSE (already listening; pid=436483)
```

### TomoKV substituted for the oracle

The final driver was deliberately pointed at `./build/tomokv` through
`GATE_DIFFER_ORACLE_BIN`. It booted on the oracle port, failed identity with status 1 before any
target boot or differ leg, stopped the exact owned PID, and verified both ports free.

```
identity_rc=1 final_ports_free=yes
  vanilla Redis oracle boot                                  ok (pid=436533 port=7083)
  oracle identity                                            FAIL (redis_version:0.1-cpp,tomokv_version:0.1-cpp)
  oracle stop                                                ok (port 7083 free)
```

## Scope

No behavioral assertions or new differential generators were added; this lane automates only the
existing proven instrument. The monolithic `tests/gate.sh full` was not invoked because the standing
lane preamble explicitly forbids running `tests/gate.sh` and reserves its default port, cores, and
NIC rig for the mainline operator. Its new full-tier helper was run directly on the assigned
resources above, including the exact release and sanitizer matrices. The measured 5m07s is therefore
the incremental full-tier differ runtime, not the total runtime of all pre-existing full sections.

# GATEFIX lane notes

Date: 2026-08-27 (Asia/Taipei)
Branch: `t-gatefix`
Starting commit: `dbef14d43 gate: session_monotonic joins the debug-armed loop`

This lane changes test and gate machinery only. It adds no runtime knob and changes no server
code. All live work used ports 7240-7249 and cores 112-127. I did not run `tests/gate.sh`, the
reserved NIC tier, or any throughput/latency benchmark.

## What changed

- `tests/gate.sh`
  - Arms DEBUG for the feature loop so hexpire's snapshot tail executes.
  - Requires exact `OK` text from the typed-roundtrip `FLUSHALL` and `DEBUG RELOAD` commands.
  - Requires the AOF-off seed `SET` to return `OK` and pre-stop `DBSIZE` to equal 1.
  - Initializes `SRV=0; SRVLOG=/dev/null`, clears them before boots, makes occupied-port guards
    terminate the gate, moves `zcboot`'s log allocation after its guard, and rejects a missing
    idle-CPU sample.
  - Pins 152 quick rows and 161 full/no-NIC rows. A genuinely executed NIC row raises the full
    expectation to 162. The `PROGRAM-STATE` check verifies the total before the summary.
  - Uses `make -j12` rather than an unbounded `make -j`.
  - Sources the checked-in `tests/niclib.sh` and propagates NIC teardown failures.
- `tests/hexpire.py`
  - DEBUG denial is a failure, not a skip.
  - Default mode requires and prints an executed-check floor of 206.
- `tests/snap_typed_roundtrip.py`
  - `build_save` raises unless `SAVE` returns RESP `+OK`.
- `tests/evict_battery.py`
  - Deletes only the nondiscriminating `hot >= cold` LRU comparison. Independent eviction-fired
    and capacity-plateau checks remain.
- `tests/lru_slow.sh`
  - Returns the Python verdict, resolves the server from its listening socket, asserts executable
    identity, confirms port release, and derives its worktree/port/cores from configurable values.
- `tests/niclib.sh`
  - Adds pre-boot port guarding plus the one-listener/answering-executable identity checks used by
    the gate's optional NIC tier.

## A8 — hexpire snapshot checks

### BEFORE

Fresh release servers were booted with the same missing DEBUG setting as the old feature loop and
with DEBUG explicitly armed. The result reproduced in both atomic modes:

```text
atomic=0, DEBUG off: hexpire: 201 checks, 0 failures -> PASS
atomic=0, DEBUG on : hexpire: 206 checks, 0 failures -> PASS
atomic=1, DEBUG off: hexpire: 201 checks, 0 failures -> PASS
atomic=1, DEBUG on : hexpire: 206 checks, 0 failures -> PASS
```

The five omitted checks were all after the DEBUG error at the old `tests/hexpire.py:517`.

### Fix and AFTER

The feature boot now includes `--enable-debug-command yes`, DEBUG denial records a failure, and
default mode enforces a 206-check floor.

Deliberate red control:

```text
value-transport      FAIL
hexpire: 201 checks (floor 206), 2 FAILURES
  DEBUG RELOAD required for snapshot round-trip: ERR DEBUG command not allowed. ...
  executed-check floor: got 201, require >= 206
unarmed repaired battery rc=1
```

Green controls, whose failure count must be zero:

```text
hexpire: 206 checks (floor 206), 0 failures -> PASS
armed atomic=0 rc=0
hexpire: 206 checks (floor 206), 0 failures -> PASS
armed atomic=1 rc=0
```

## A9 — typed snapshot reply assertions

### BEFORE

A server was booted with `--enable-debug-command yes` and a mode-0555 directory. The old exact
`&&` shape passed even though both persistence commands returned errors:

```text
GATE-STYLE CHAIN PASS
SAVE: b'-ERR could not create snapshot temporary file'
state captured: 40 checks
TYPED ROUNDTRIP PASS (40/40)
ERR could not create snapshot temporary file
SAVE redis-cli rc=0
ERR could not create snapshot temporary file
DEBUG RELOAD redis-cli rc=0
directory files=0 bytes=0
```

### Sweep

The only `redis-cli` inside an `&&` chain with discarded output was the typed-roundtrip
`DEBUG RELOAD`. It now goes through an exact-text `OK` assertion. I also repaired the adjacent
discarded `FLUSHALL` even though it was outside the chain, and A10 repairs the discarded AOF seed
`SET`. Every other `redis-cli` in `gate.sh` feeds a parsed variable (`CONFIG`, `INFO`, or `DBSIZE`);
none is an output-discarded member of an `&&` chain.

### Fix and AFTER

`build_save` now requires RESP `b"+OK"`; the shell helper requires CLI text `OK` from both commands.
The read-only control now fails with no file written:

```text
RuntimeError: SAVE failed: b'-ERR could not create snapshot temporary file'
repaired build_save rc=1 debug_text_assert_rc=1
reply=<ERR could not create snapshot temporary file> files=0
```

Writable controls, whose mismatch count must be zero:

```text
SAVE: b'+OK'
state captured: 40 checks
TYPED ROUNDTRIP PASS (40/40)
typed roundtrip atomic=0 rc=0 flush=<OK> reload=<OK> snapshot_bytes=201408

SAVE: b'+OK'
state captured: 40 checks
TYPED ROUNDTRIP PASS (40/40)
typed roundtrip atomic=1 rc=0 flush=<OK> reload=<OK> snapshot_bytes=201408
```

## A10 — AOF-off seed

### BEFORE

With reads working but `maxmemory=1`/`noeviction` rejecting writes, the discarded seed returned CLI
status zero, pre-stop `DBSIZE` was zero, and the old post-recovery row still passed:

```text
PONG
OOM command not allowed when used memory > 'maxmemory'.
visible SET rc=0
discarded SET rc=0
pre-stop DBSIZE=0 appendonlydir=absent
CURRENT AOF-OFF ROW PASS: DBSIZE=0 appendonlydir=absent
```

### Fix and AFTER

The seed row now requires text `OK` and `DBSIZE=1` before the unclean stop.

```text
repaired AOF seed row rc=1 SET=<OOM command not allowed when used memory > 'maxmemory'.> DBSIZE=0

atomic=0 pre-stop SET=<OK> DBSIZE=1
atomic=0 recovery DBSIZE=0 appendonlydir=absent
atomic=1 pre-stop SET=<OK> DBSIZE=1
atomic=1 recovery DBSIZE=0 appendonlydir=absent
```

The rejected-write control must report pre-stop `DBSIZE=0` and a nonzero row status. Valid arms must
report pre-stop `DBSIZE=1`; their recovery control must report `DBSIZE=0` and no appendonly directory.

## A11 — boot state, log ownership, and idle sampling

### BEFORE

Using an occupied assigned port and a known previous log reproduced all three false-green rows.
The overall runs were red because the boot rows failed; these were false-green rows, not
false-green runs.

```text
port 7243 pre-boot guard                             FAIL (already accepting)
release boot                                         FAIL
after guard: SRV=999999 SRVLOG=/tmp/gatefix-a11-prev.6mnkFx
shutdown invariants (nothing stuck)                  ok
guard reproduction totals: 1 ok, 1 FAIL (run is red)

port 7243 pre-boot guard                             FAIL (already accepting)
zc ASAN boot                                         FAIL
zc guard log bytes=0 path=/tmp/gatefix-a11-zc.MJDC6Z
zc ASAN clean                                        ok
zc reproduction totals: 1 ok, 1 FAIL (run is red)

bash: line 1: SRV: unbound variable
bash: line 2: SRV: unbound variable
idle reproduction: C0=<> C1=<> J=0 row=PASS
idle snippet rc=0
```

### Fix and AFTER

State starts and resets at `0`/`/dev/null`; an occupied-port guard uses `exit 1`; `zcboot` creates a
log only after the guard; and the idle row requires nonempty C0, C1, and J.

```text
port 7243 pre-boot guard                             FAIL (already accepting)
repaired boot guard rc=1 stale-row-marker-count=0
repaired idle control: C0=<> C1=<> J=<> row=FAIL

port 7243 pre-boot guard                             FAIL (already accepting)
zc exit state: SRV=0 SRVLOG=/dev/null
repaired zc guard rc=1 ASAN-clean-marker-count=0
```

Both marker counts must remain zero: no shutdown/ASAN-clean row is reached after a guard trip.

## A12 — NIC teardown, guard, and identity

### BEFORE

The NIC rig is reserved and `sudo -n ip netns list` requested a password, so no live NIC run was
performed. I reproduced the control flow with mocked NIC primitives:

```text
nic_boot with foreign_listener=1: rc=0 start_calls=1 ping_calls=1 identity_queries=0
nic_kill_srv GAVE-UP
nic_boot continued
run_cell rc=0 despite teardown GAVE-UP
```

### Fix and AFTER

The checked-in helper rejects an occupied port, requires exactly one listener, and verifies the
answering process executable. Gate teardown failure is propagated before boot and after the cells.

```text
PORT-GUARD-FAIL occupied pids=42 on port 7249
NIC port-guard control: rc=1 start_calls=0
IDENTITY-FAIL wrong-identity answering=123 runs /wrong/binary, not /bin/true
NIC identity control: rc=1
nic_kill_srv GAVE-UP
NIC teardown propagation control: rc=1 boot-marker-count=0
NIC matching identity control: rc=0
```

The occupied-port start count and teardown-failure boot marker must both be zero. The live netns/NIC
tier remains explicitly unexercised in this lane.

## A13 — nondiscriminating LRU comparison

### BEFORE

Two fresh live boots, without the required 256-second LRU-clock dwell, produced opposite verdicts
while the independent mechanism values were identical:

```text
atomic=0: eviction=6512, dbsize=7488, hot=20, cold=22 -> row FAIL
atomic=1: eviction=6512, dbsize=7488, hot=25, cold=21 -> row PASS
```

That is not a stable discriminator for a broken evictor. I deleted the comparison rather than
pretending a threshold adjustment repaired it. The engine was not changed.

### AFTER

The remaining live batteries prove eviction fired and capacity plateaued; both have zero failures:

```text
allkeys-lru: eviction FIRED                          ok evicted=6512
allkeys-lru: plateau near ceiling (<10k of 14k offered) ok 7488
SECTION lru: 3 ok, 0 FAIL
evict lru atomic=0 rc=0

SECTION lru: 3 ok, 0 FAIL
evict lru atomic=1 rc=0
```

## A14 — `lru_slow.sh` status

### BEFORE

The old script was executed with mocked process/Python functions so its forbidden hard-coded cores,
port, and other worktree were not touched:

```text
LRU-SLOW FAIL: hot=0/50 cold=1/50 (forced verdict control)
mocked lru_slow rc=0
```

### Fix and AFTER

Python now exits from the verdict and the shell preserves that status across listener teardown.

```text
LRU-SLOW FAIL: hot=0/50 cold=1/50 (forced verdict control)
repaired mocked lru_slow rc=1
LRU-SLOW PASS: hot=50/50 cold=6/50 (forced pass control)
repaired mocked lru_slow pass rc=0
```

The FAIL arm must return nonzero; the PASS control must return zero. The real 300-second test was not
run because this is a correctness lane and the task explicitly forbids publishing performance
verdicts from it.

## Expected-row ledger

The source-derived count agrees with the constants:

```text
plain=52 feature=2*(19+1) debug=2*6 snapshot=2*6 aof=2*18
static quick rows=152
EXPECT_QUICK=152 match=True
full non-NIC extra=9; static full rows=161; EXPECT_FULL=161
```

The deliberate dropped-row control turns red:

```text
PROGRAM-STATE ledger                                 FAIL (151/152 checks)
ledger drop control: PASS=151 FAIL=1
```

The internal 206-check hexpire floor detects missing checks inside that battery; the 152/161 gate
ledger detects missing gate rows.

## Build, sanitizer, differ, and plain-path evidence

Exact build commands:

```sh
make clean
make -j12
make asan
```

Both builds exited zero. The sanitizer binary is `-fsanitize=address,undefined`. Armed hexpire,
typed snapshot round-trip, and the remaining eviction battery ran under it in both atomic modes:

```text
sanitizer atomic=0 hex=0 flush=<OK> build=0 reload=<OK> verify=0
sanitizer diagnostics atomic=0: 0
sanitizer atomic=1 hex=0 flush=<OK> build=0 reload=<OK> verify=0
sanitizer diagnostics atomic=1: 0
sanitizer evict atomic=0 rc=0
sanitizer eviction diagnostics atomic=0: 0
sanitizer evict atomic=1 rc=0
sanitizer eviction diagnostics atomic=1: 0
```

The zero-diagnostic detector searched for `ERROR: AddressSanitizer`, `ERROR: LeakSanitizer`, and
`runtime error:`. Its clean controls all reported zero.

Relevant differ commands (target 7247, vanilla Redis 7.4.2 oracle 7248):

```sh
taskset -c 124-127 python3 tests/differ.py 127.0.0.1 7247 127.0.0.1 7248 hexpire 11
taskset -c 124-127 python3 tests/differ.py 127.0.0.1 7247 127.0.0.1 7248 hexpire 37
```

They ran once each against target `--atomic 0` and once each against target `--atomic 1`:

```text
DIFFER hexpire: 4288 ops, 0 diffs -> PASS
DIFFER hexpire: 4288 ops, 0 diffs -> PASS
hexpire differ atomic=0 seed11=0 seed37=0
DIFFER hexpire: 4288 ops, 0 diffs -> PASS
DIFFER hexpire: 4288 ops, 0 diffs -> PASS
hexpire differ atomic=1 seed11=0 seed37=0
```

The differ control is zero diffs in every leg.

Plain p32 correctness smoke, with no rate or latency conclusion:

```text
P32 GET/SET correctness smoke: SET=2048/2048 GET=2048/2048 exact
shutdown: dispatched=4096 executed=4096
```

## Exact live-server pattern

Release and sanitizer test servers used this shape, varying only binary, port, atomic value, and
purpose-specific persistence flags:

```sh
taskset -c 112-119 ./build/tomokv --port 7240 --bind 127.0.0.1 \
  --shards 16 --ratio 4:4 --atomic 0 --enable-debug-command yes
taskset -c 112-119 ./build/tomokv-asan --port 7245 --bind 127.0.0.1 \
  --shards 16 --ratio 4:4 --atomic 1 --enable-debug-command yes
taskset -c 120-123 /tmp/claude-1000/redis74/src/redis-server \
  --port 7248 --bind 127.0.0.1 --protected-mode no --save '' --appendonly no
```

Every server stop resolved one PID from `ss -lntpH 'sport = :PORT'`, signalled that exact PID, and
confirmed the listener was released before reuse. Final assigned-port listener count: zero.

## Scope

- No server/C++ feature behavior changed, so there are no new knobs or `tomokv.conf` entries.
- `tests/gate.sh` itself was not run, as required by the lane brief. Static syntax/count validation
  plus the directed live and deliberate-failure arms above cover the changed mechanisms.
- The NIC changes are mock-tested only; the reserved root/netns tier is unexercised.
- The sanitizer grep was not widened in `gate.sh`; the explicitly refuted sanitizer item remains
  out of scope.

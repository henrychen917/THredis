# S6 wrong-answer fixes

## Scope and provenance

This lane owns A4 (RANDOMKEY reachability), A5 (`SCAN ... TYPE stream` and unknown type names),
A6 (pre-save LASTSAVE), and A7's negative-`numreplicas` validation. A2 and the non-decimal SCAN
cursor grammar belong to `t-scancursor` and are not in this lane's gate.

The brief said `tests/s6.py` already existed with 215 comparisons, but it was absent from this
checkout and from repository object history:

```text
$ git log --all --oneline -- tests/s6.py
<no output>
$ git ls-tree -r HEAD tests | grep s6.py
<no output>
```

I therefore landed a socket-level replacement with exactly 215 oracle-derived comparisons: 200
individual A4 reachability rows and 15 firing/control rows for A4-A7. The A7 fsynced-local count is
not a gate row because the lane's explicit alternative was selected after the per-connection
oracle probe below. The new `s6fix` differ suite adds 4,000 randomized byte-compared operations per
seed plus property comparisons for nondeterministic RANDOMKEY, SCAN cursors, and LASTSAVE.
`tests/gate.sh` invokes `s6` first in the existing feature loop under both atomic settings; it was
edited but not run because port 7899 and cores 0-7 are reserved for the mainline operator.

No command, registry row, runtime knob, config documentation, Makefile source, `Op`, or `Client`
layout changed. `sizeof(Op)==336` and `sizeof(Client)==1984` therefore remain under their existing
build assertions. There are no new knobs.

## A4 — RANDOMKEY reachability

### Before

Untouched release binary, 200 keys, 20,000 draws:

```text
target: distinct=184 missing=16 min=0 max=658 unexpected=0 nulls=0 dbsize=200
oracle: distinct=200 missing=0  min=56 max=162 unexpected=0 nulls=0 dbsize=200
```

The actual before-gate run used a fresh process/hash seed and failed 18 individual key rows:

```text
A4 draws=20000 distinct=182 missing=18 unexpected=0 nulls=0 min=0 max=477
s6: 215 comparisons, 23 failures -> FAIL
```

Controls: `DBSIZE=200`, unexpected keys `0`, and null replies `0`. These remained zero before and
after, so the detector measures reachability rather than setup failure or reply corruption.

### Fix

`FlatStore::random_live()` now takes an owner-private xorshift draw rather than reusing the IO-side
draw whose low bits already selected the shard. That removes the power-of-two residue correlation.
The initial fresh draw still chooses the wrapped physical start; reservoir selection across that
one walk makes live keys within the chosen shard uniform. The first fresh-start-only version made
all slots reachable but left a sparse-table adjacent key with only about one expected global hit;
the reservoir step made the mandated 20,000-draw gate stable.

Active cost: RANDOMKEY now performs one bounded walk of the selected shard's two tables and one RNG
step per live candidate. GET/SET dispatch and storage paths are unchanged, so feature-off/plain-path
cost is zero.

### After

```text
atomic=0: distinct=200 missing=0 unexpected=0 nulls=0 min=53 max=233
atomic=1: distinct=200 missing=0 unexpected=0 nulls=0 min=46 max=341
ASAN atomic=0: distinct=200 missing=0 unexpected=0 nulls=0 min=59 max=181
ASAN atomic=1: distinct=200 missing=0 unexpected=0 nulls=0 min=45 max=204
```

## A5 — SCAN TYPE stream and unknown names

### Before

```text
target SCAN TYPE stream          => ERR syntax error
oracle SCAN TYPE stream          => [scan:stream]
target SCAN TYPE not-a-real-type => ERR syntax error
oracle SCAN TYPE not-a-real-type => []
```

### Decision and fix

Redis 7.4 deliberately treats an unknown type name as a valid filter that matches nothing; it does
not return an error. The type parser therefore accepts every following token. Known names match
`object_type()`, including `stream`; unknown names naturally match no object.

The battery exhausts all 16 TomoKV outer cursors. Its controls require the string key count to be
zero and the unknown-type result count to be zero.

### After

```text
atomic=0: stream_calls=16 stream_keys=[b's6:stream'] unknown_calls=16 unknown_keys=0
atomic=1: stream_calls=16 stream_keys=[b's6:stream'] unknown_calls=16 unknown_keys=0
differ:   target stream calls=16, oracle calls=1, complete key sets equal
```

## A6 — LASTSAVE before the first save

### Before

```text
target lastsave=0          now=1787825994 delta=1787825994 future_control=0
oracle lastsave=1787825950 now=1787825995 delta=45         future_control=0
```

### Fix

`SnapshotManager::init()` seeds `last_save_time_` from `CLOCK_REALTIME` in seconds. A later
successful save continues to replace the same atomic value. The control rejects a timestamp more
than one second in the future.

### After

```text
atomic=0: lastsave=1787826765 now=1787826771 delta=6 future_control=0
atomic=1: lastsave=1787826781 now=1787826787 delta=6 future_control=0
```

## A7 — WAITAOF validation and the local-count alternative

### Before: reproduced divergences and controls

With AOF disabled, both servers returned the required zero control:

```text
WAITAOF 0 0 0 => [0,0] on target and oracle
```

With `appendonly=yes appendfsync=always`, after an acknowledged SET:

```text
target WAITAOF 0 0 0 => [0,0]   (INFO reported aof_fsyncs:1)
oracle WAITAOF 0 0 0 => [1,0]
```

The validation defect reproduced independently of AOF mode:

```text
target WAITAOF 0 -1 0 => [0,0]
oracle WAITAOF 0 -1 0 => ERR value is out of range, must be positive
```

Negative timeout was the zero/unchanged control and already matched:

```text
WAITAOF 0 0 -1 => ERR timeout is negative
```

### Why the explicit alternative is necessary

Redis's first result element is tied to the calling connection's write offset, not a process-wide
"everything posted is durable" state. Oracle evidence with `appendfsync=no`:

```text
before writes:            connection A => [1,0]
after A writes:           connection A => [0,0]
same server, no writes:   connection B => [1,0]
```

With `appendfsync=everysec`, the oracle moved `[1,0] -> [0,0] -> [1,0]` across a write and the next
1.5-second fsync interval. TomoKV retains global posted/durable frontiers but no per-connection AOF
offset, and this ConnLocal handler normally runs at parse time before earlier pipelined shard ops.
A global frontier would under-report connection B above; `aof_fsyncs>0` would over-report A after a
new unsynced write. Both would be plausible but false numbers.

Per the brief's allowed alternative, I did not fabricate the local count. `NOTES-SERVERTAIL.md`
now narrows the old "every numlocal==0 reply is exact" claim and records the per-connection gap.
The existing documented `numlocal==1` shelf was not changed.

### Fix and after

Negative `numreplicas` is now rejected before timeout/config handling with Redis's exact error:

```text
WAITAOF 0 -1 0 => ERR value is out of range, must be positive
WAITAOF 0 0 0  => [0,0]  (AOF-off control)
WAITAOF 0 0 -1 => ERR timeout is negative
```

## Cheap SCAN cosmetics

Landed and byte-compared:

- `SCAN 0 COUNT abc` now returns `ERR value is not an integer or out of range`.
- `SCAN 0 NOVALUES` now returns `ERR NOVALUES option can only be used in HSCAN`.

Skipped deliberately as A2/t-scancursor scope: `-0`, `+1`, and `18446744073709551615`. Redis 7.4
accepts all three; TomoKV still returns `ERR invalid cursor`. Changing those is not a text-only
repair because TomoKV reserves the high cursor byte for its outer shard id. No A2 row is in this
lane's gate.

## Exact commands and evidence

All TomoKV servers used cores 96-103, Redis used core 104, and only ports 7230-7231 were used.
Every stop PID was re-read from `ss -lntp 'sport = :PORT'`, signalled exactly, and followed by a
listener-release check. No `tests/gate.sh`, NIC test, or performance measurement was run.

Representative launch/test commands:

```sh
make -j12 clean && make -j12
taskset -c 96-103 ./build/tomokv --port 7230 --bind 127.0.0.1 \
  --ratio 2:2 --shards 16 --protected-mode no --atomic 0 --appendonly no
python3 tests/s6.py 127.0.0.1 7230

taskset -c 96-103 ./build/tomokv --port 7230 --bind 127.0.0.1 \
  --ratio 2:2 --shards 16 --protected-mode no --atomic 1 --appendonly no
python3 tests/s6.py 127.0.0.1 7230

taskset -c 104 /tmp/claude-1000/redis74/src/redis-server \
  --port 7231 --bind 127.0.0.1 --protected-mode no --save "" --appendonly no --daemonize no
python3 tests/differ.py 127.0.0.1 7230 127.0.0.1 7231 s6fix 7
python3 tests/differ.py 127.0.0.1 7230 127.0.0.1 7231 s6fix 43

make asan
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
taskset -c 96-103 ./build/tomokv-asan --port 7230 --bind 127.0.0.1 \
  --ratio 2:2 --shards 16 --protected-mode no --atomic 0 --appendonly no
python3 tests/s6.py 127.0.0.1 7230
# Repeated with --atomic 1.
```

Fail-before tail:

```text
A4 draws=20000 distinct=182 missing=18 unexpected=0 nulls=0 min=0 max=477
A5 stream_calls=1 stream_keys=[] unknown_calls=1 unknown_keys=0
A6 lastsave=0 now=1787826637 delta=1787826637 future_control=0
A7 zero=[0, 0] negative_replicas=[0, 0] negative_timeout=ErrorReply(b'ERR timeout is negative')
s6: 215 comparisons, 23 failures -> FAIL
```

Release pass tails:

```text
atomic=0:
A4 draws=20000 distinct=200 missing=0 unexpected=0 nulls=0 min=53 max=233
s6: 215 comparisons, 0 failures -> PASS

atomic=1:
A4 draws=20000 distinct=200 missing=0 unexpected=0 nulls=0 min=46 max=341
s6: 215 comparisons, 0 failures -> PASS
```

Differ tails, both seeds:

```text
DIFFER s6fix: 48456 logical ops, 0 diffs -> PASS   # seed 7
DIFFER s6fix: 48456 logical ops, 0 diffs -> PASS   # seed 43
```

Sanitizer tails:

```text
ASAN/UBSAN atomic=0: s6: 215 comparisons, 0 failures -> PASS
ASAN/UBSAN atomic=1: s6: 215 comparisons, 0 failures -> PASS
server diagnostic output after each battery: empty
```

Plain path correctness-only smoke after the final clean rebuild:

```text
p32 GET/SET correctness smoke: replies=6400 mismatches=0
```

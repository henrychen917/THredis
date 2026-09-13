# BLOCKMULTI lane notes

## Result

Resolved both handed-on command-surface differences against the vanilla Redis 7.4.2 binary:

- `BLPOP`, `BRPOP`, `BLMPOP`, `BZPOPMIN`, `BZPOPMAX`, and `BZMPOP` execute without blocking inside
  `EXEC`. Missing data is a null transaction element; ready data is popped with the command's exact
  legacy reply shape in RESP2 and RESP3.
- Standalone `WAIT` with an unsatisfied positive replica count now defers its reply until the
  millisecond deadline. Timeout zero waits forever. Disconnect cancels the wait and drains
  `blocked_clients`. `WAIT` inside `MULTI` remains immediate.

No storage, MVCC-resolver, scatter-core ownership, or shard-owner rules were changed. There are no
new runtime knobs and no changes to `tomokv.conf`.

## Live comparison table

All strings below are raw RESP with CR/LF shown as `\r\n`. "Target before" was confirmed on the
pre-change `t-blockmulti` binary; "reference" was confirmed on
`/tmp/claude-1000/redis74/src/redis-server`.

### Missing collection inside `EXEC`

The pre-change target reply was the same for all six commands:
`*1\r\n-ERR command is not supported by MULTI execution\r\n`.

| Command | RESP2 reference | RESP3 reference | Before | After |
|---|---|---|---|---|
| `BLPOP key 0` | `*1\r\n*-1\r\n` | `*1\r\n_\r\n` | DIFFERS | RESOLVED |
| `BRPOP key 0` | `*1\r\n*-1\r\n` | `*1\r\n_\r\n` | DIFFERS | RESOLVED |
| `BLMPOP 0 1 key LEFT` | `*1\r\n*-1\r\n` | `*1\r\n_\r\n` | DIFFERS | RESOLVED |
| `BZPOPMIN key 0` | `*1\r\n*-1\r\n` | `*1\r\n_\r\n` | DIFFERS | RESOLVED |
| `BZPOPMAX key 0` | `*1\r\n*-1\r\n` | `*1\r\n_\r\n` | DIFFERS | RESOLVED |
| `BZMPOP 0 1 key MIN` | `*1\r\n*-1\r\n` | `*1\r\n_\r\n` | DIFFERS | RESOLVED |

### Ready collection inside `EXEC`

The pre-change target again returned the unsupported-command error element above. With `bm:l`
holding `v` and `bm:z` holding member `m` at score 1, the reference replies were:

| Command | RESP2 reference | RESP3 reference | Before | After |
|---|---|---|---|---|
| `BLPOP bm:l 0` | `*1\r\n*2\r\n$4\r\nbm:l\r\n$1\r\nv\r\n` | same | DIFFERS | RESOLVED |
| `BRPOP bm:l 0` | `*1\r\n*2\r\n$4\r\nbm:l\r\n$1\r\nv\r\n` | same | DIFFERS | RESOLVED |
| `BLMPOP 0 1 bm:l LEFT` | `*1\r\n*2\r\n$4\r\nbm:l\r\n*1\r\n$1\r\nv\r\n` | same | DIFFERS | RESOLVED |
| `BZPOPMIN bm:z 0` | `*1\r\n*3\r\n$4\r\nbm:z\r\n$1\r\nm\r\n$1\r\n1\r\n` | `*1\r\n*3\r\n$4\r\nbm:z\r\n$1\r\nm\r\n,1\r\n` | DIFFERS | RESOLVED |
| `BZPOPMAX bm:z 0` | `*1\r\n*3\r\n$4\r\nbm:z\r\n$1\r\nm\r\n$1\r\n1\r\n` | `*1\r\n*3\r\n$4\r\nbm:z\r\n$1\r\nm\r\n,1\r\n` | DIFFERS | RESOLVED |
| `BZMPOP 0 1 bm:z MIN` | `*1\r\n*2\r\n$4\r\nbm:z\r\n*1\r\n*2\r\n$1\r\nm\r\n$1\r\n1\r\n` | `*1\r\n*2\r\n$4\r\nbm:z\r\n*1\r\n*2\r\n$1\r\nm\r\n,1\r\n` | DIFFERS | RESOLVED |

### `WAIT` and controls

| Check | Target before | Redis 7.4 reference | Result after |
|---|---|---|---|
| `WAIT 1 200`, observed at 50 ms | `:0\r\n` | wire silent; `:0\r\n` at about 210 ms | RESOLVED |
| `WAIT 1 0`, observed at 50 ms | `:0\r\n` | wire silent indefinitely | RESOLVED |
| `WAIT 0 500` | immediate `:0\r\n` | immediate `:0\r\n` | CONSISTENT |
| `MULTI; WAIT 1 0; EXEC` | `*1\r\n:0\r\n` | `*1\r\n:0\r\n` | CONSISTENT |
| `MULTI; BLMOVE missing ... 0; EXEC` | null element | null element | CONSISTENT |
| unsatisfied `WAIT 1 0` accounting | `blocked_clients` was not armed | reference moves `0 -> 1 -> 0` across wait/disconnect | RESOLVED |

Malformed transaction children were also byte-probed. Timeout validation remains the blocking
command's `ERR timeout is not a float or out of range`; invalid MPOP `numkeys` is
`ERR numkeys should be greater than 0`; invalid direction is `ERR syntax error`.

## Design

### Blocking collections in `MULTI`

The transaction builder recognizes only the six handed-on blocking pop names. After validating the
blocking timeout, it builds a cold child argv for `LMPOP` or `ZMPOP` and sends that child through the
existing transaction-aware xshard path. The timeout is intentionally omitted from the child, so the
child can never register a waiter. `BLPOP`/`BRPOP` and `BZPOPMIN`/`BZPOPMAX` carry a reply-shape tag
in the scatter side allocation so gather emits the legacy pair/triple rather than MPOP's nested
array. Missing data already uses the protocol-selected null builder.

This lowering is forced through scatter even when all keys share one owner. It therefore stays
inside the transaction's external epoch/abort decision and never calls a blocking handler from an
owner task. The cold transaction objects own all added vectors/fields; `Op` and `Client` did not
grow, and their static footprint assertions passed the clean and ASAN builds.

### Unsatisfied `WAIT`

`WAIT` remains connection-local. A new command flag is inspected only inside the existing
`ConnLocal` branch. For a positive unsatisfied count, the connection-owning IO thread publishes an
unfinished ROB slot, sets the existing connection barrier, and records `{Client*, op_id, deadline}`
in an IO-local vector. No shard is touched. A predicted-false per-IO-batch branch polls the vector;
the vector allocates nothing until the first unsatisfied `WAIT`. Finite expiry writes `:0`, timeout
zero remains pending, and connection teardown cancels the entry. The server's existing
`blocked_clients` counter is incremented/decremented with that lifetime.

GET/SET dispatch does not test the new command flag. With no pending `WAIT`, the runtime cost is one
predicted-false IO-batch branch and zero allocation.

## Tests added/changed

- Added `tests/blockmulti.py`: 141 directed raw-RESP checks in RESP2 and RESP3, ready/missing/error
  controls for all six commands, finite/forever `WAIT`, pipeline ordering, accounting, disconnect,
  and `WAIT`-inside-`EXEC`.
- Changed the existing `blocking` generator in `tests/differ.py`: the former documented gaps are
  now strict comparisons; both missing and ready transaction cases are exercised; finite and
  timeout-zero `WAIT` both require silence at 50 ms. The suite now honors `-3`.

## Exact verification commands

Servers used only the assigned cores and ports:

```text
taskset -c 64-71 ./build/tomokv --port 7480 --bind 127.0.0.1 --shards 16 --ratio 4:4 --atomic 0 --enable-debug-command yes --protected-mode no
taskset -c 64-71 ./build/tomokv --port 7480 --bind 127.0.0.1 --shards 16 --ratio 4:4 --atomic 1 --enable-debug-command yes --protected-mode no
taskset -c 72-79 /tmp/claude-1000/redis74/src/redis-server --port 7481 --bind 127.0.0.1 --save '' --appendonly no --enable-debug-command yes --protected-mode no --daemonize no
```

Build and directed/regression commands:

```text
make clean && make -j8
python3 tests/blockmulti.py 127.0.0.1 7480
python3 tests/blocking.py 127.0.0.1 7480
python3 tests/multi_exec.py 127.0.0.1 7480
python3 tests/edgeproto.py 127.0.0.1 7480
make asan
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 taskset -c 64-71 ./build/tomokv-asan --port 7480 --bind 127.0.0.1 --shards 16 --ratio 4:4 --atomic 1 --enable-debug-command yes --protected-mode no
python3 tests/blockmulti.py 127.0.0.1 7480
make clean && make -j8
```

Differ commands were run under both `--atomic 0` and `--atomic 1` target boots:

```text
python3 tests/differ.py 127.0.0.1 7480 127.0.0.1 7481 blocking 7
python3 tests/differ.py 127.0.0.1 7480 127.0.0.1 7481 blocking 19
python3 tests/differ.py 127.0.0.1 7480 127.0.0.1 7481 blocking 41
python3 tests/differ.py 127.0.0.1 7480 127.0.0.1 7481 blocking 7 -3
python3 tests/differ.py 127.0.0.1 7480 127.0.0.1 7481 blocking 19 -3
```

## Evidence tails

Directed battery, release `--atomic 0`, release `--atomic 1`, final clean release, and ASAN:

```text
  ok   WAIT stays immediate inside EXEC
  ok   WAIT EXEC timing
  ok   final blocked gauge zero control
BLOCKMULTI PASS: 141 checks; collection_fired=12 wait_deadlines_fired=2 wait_disconnect_fired=1
```

Differ seeds 7, 19, and 41 (the same tails were obtained at both atomic settings):

```text
  coverage: multi_ready_cases=608 timeout_zero_arms=8 fifo_waiters=6
DIFFER blocking: 5327 logical ops, 5372 checks, 0 diffs -> PASS
  coverage: multi_ready_cases=590 timeout_zero_arms=8 fifo_waiters=6
DIFFER blocking: 5297 logical ops, 5342 checks, 0 diffs -> PASS
  coverage: multi_ready_cases=583 timeout_zero_arms=8 fifo_waiters=6
DIFFER blocking: 5262 logical ops, 5307 checks, 0 diffs -> PASS
```

RESP3 differ tails were also zero-diff (seed 7 at atomic 0, seed 19 at atomic 1):

```text
DIFFER blocking: 5327 logical ops, 5372 checks, 0 diffs -> PASS
DIFFER blocking: 5297 logical ops, 5342 checks, 0 diffs -> PASS
```

Regression and sanitizer tails:

```text
BLOCKING PASS
MULTI/WATCH directed battery passed
edgeproto: 379 checks, 0 failures -> PASS
ASAN/UBSAN log scan: no AddressSanitizer, LeakSanitizer, or runtime-error diagnostics
```

## Scope and handoff

No scope was cut and no observed difference remains to hand on.

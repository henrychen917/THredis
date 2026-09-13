# Lane t-arity: container subcommand arity and naming

## Result

Implemented Redis 7.4-compatible direct-command arity and unknown-subcommand replies for the
implemented arms of `ACL`, `CONFIG`, `OBJECT`, `MEMORY`, `LATENCY`, and `SLOWLOG`.

The original 51-row boundary matrix contained 42 byte differences and 9 byte-exact controls. All
51 direct probes are now byte-exact. The directed battery adds six outer-container controls and six
valid-form negative controls: 63 checks total, with the valid-form error detector required to stay
at zero. The randomized differ repeats only the 42 differences actually observed before the fix,
varies subcommand case, and runs 4,200 operations per seed.

No runtime knob was added, so `tomokv.conf` is unchanged.

## Design

`SubcommandArity` is cold metadata separate from `CommandSpec`; neither `sizeof(CommandSpec)`,
`sizeof(Op)`, nor `sizeof(Client)` changed. Each owning feature file supplies its own table:

- `acl.inc`: CAT, DELUSER, GENPASS, GETUSER, LIST, LOAD, LOG, SAVE, SETUSER, USERS, WHOAMI, HELP.
- `t_server.cc`: CONFIG GET, SET, REWRITE, RESETSTAT, HELP.
- `server_tail.cc`: OBJECT and MEMORY arms.
- `slowlog.cc`: LATENCY and SLOWLOG arms.

The shared cold formatter distinguishes three observed failure classes:

1. `wrong number of arguments for 'container|subcommand' command`;
2. Redis's handler-level `unknown subcommand or wrong number ...` for variadic-metadata arms
   (`ACL CAT/GENPASS/LOG`, `SLOWLOG GET`);
3. syntax error (`MEMORY USAGE` with a surplus/malformed SAMPLES tail).

`OBJECT`, `MEMORY`, and `SLOWLOG` retain their existing broad registry maxima. Requests beyond
those maxima are still rejected before ACL/MULTI admission; the existing arity-failure branch now
calls the container formatter before retiring that already-invalid operation. This adds no branch,
load, or instruction to a valid GET/SET path. A valid container command pays one short linear scan
of 4–12 cold rows. An invalid container command additionally builds its error string out of line.

## Direct boundary matrix

Replies below are serialized RESP bytes; `\r\n` is shown explicitly. “Before” means the target at
`af0da1912` before this lane. Every `DIFFERS -> RESOLVED` row now equals the Redis column byte for
byte.

### ACL (14 probes)

| Probe | Before target | Redis 7.4 | Result |
|---|---|---|---|
| `ACL CAT keyspace x` | `-ERR Unknown subcommand or wrong number of arguments for 'acl'. Try ACL HELP.\r\n` | `-ERR unknown subcommand or wrong number of arguments for 'CAT'. Try ACL HELP.\r\n` | DIFFERS -> RESOLVED |
| `ACL DELUSER` | `-ERR Unknown subcommand or wrong number of arguments for 'acl'. Try ACL HELP.\r\n` | `-ERR wrong number of arguments for 'acl\|deluser' command\r\n` | DIFFERS -> RESOLVED |
| `ACL GENPASS 8 x` | `-ERR Unknown subcommand or wrong number of arguments for 'acl'. Try ACL HELP.\r\n` | `-ERR unknown subcommand or wrong number of arguments for 'GENPASS'. Try ACL HELP.\r\n` | DIFFERS -> RESOLVED |
| `ACL GETUSER` | `-ERR Unknown subcommand or wrong number of arguments for 'acl'. Try ACL HELP.\r\n` | `-ERR wrong number of arguments for 'acl\|getuser' command\r\n` | DIFFERS -> RESOLVED |
| `ACL GETUSER default x` | `-ERR Unknown subcommand or wrong number of arguments for 'acl'. Try ACL HELP.\r\n` | `-ERR wrong number of arguments for 'acl\|getuser' command\r\n` | DIFFERS -> RESOLVED |
| `ACL LIST x` | `-ERR Unknown subcommand or wrong number of arguments for 'acl'. Try ACL HELP.\r\n` | `-ERR wrong number of arguments for 'acl\|list' command\r\n` | DIFFERS -> RESOLVED |
| `ACL LOAD x` | `-ERR This Redis instance is not configured to use an ACL file. You may want to specify users via the ACL SETUSER command and then issue a CONFIG REWRITE (assuming you have a Redis configuration file set) in order to store users in the Redis configuration.\r\n` | `-ERR wrong number of arguments for 'acl\|load' command\r\n` | DIFFERS -> RESOLVED |
| `ACL SAVE x` | `-ERR This Redis instance is not configured to use an ACL file. You may want to specify users via the ACL SETUSER command and then issue a CONFIG REWRITE (assuming you have a Redis configuration file set) in order to store users in the Redis configuration.\r\n` | `-ERR wrong number of arguments for 'acl\|save' command\r\n` | DIFFERS -> RESOLVED |
| `ACL LOG 1 x` | `-ERR Unknown subcommand or wrong number of arguments for 'acl'. Try ACL HELP.\r\n` | `-ERR unknown subcommand or wrong number of arguments for 'LOG'. Try ACL HELP.\r\n` | DIFFERS -> RESOLVED |
| `ACL SETUSER` | `-ERR Unknown subcommand or wrong number of arguments for 'acl'. Try ACL HELP.\r\n` | `-ERR wrong number of arguments for 'acl\|setuser' command\r\n` | DIFFERS -> RESOLVED |
| `ACL USERS x` | `-ERR Unknown subcommand or wrong number of arguments for 'acl'. Try ACL HELP.\r\n` | `-ERR wrong number of arguments for 'acl\|users' command\r\n` | DIFFERS -> RESOLVED |
| `ACL WHOAMI x` | `-ERR Unknown subcommand or wrong number of arguments for 'acl'. Try ACL HELP.\r\n` | `-ERR wrong number of arguments for 'acl\|whoami' command\r\n` | DIFFERS -> RESOLVED |
| `ACL HELP x` | `-ERR Unknown subcommand or wrong number of arguments for 'acl'. Try ACL HELP.\r\n` | `-ERR wrong number of arguments for 'acl\|help' command\r\n` | DIFFERS -> RESOLVED |
| `ACL BOGUS` | `-ERR Unknown subcommand or wrong number of arguments for 'acl'. Try ACL HELP.\r\n` | `-ERR unknown subcommand 'BOGUS'. Try ACL HELP.\r\n` | DIFFERS -> RESOLVED |

### CONFIG (6 probes)

| Probe | Before target | Redis 7.4 | Result |
|---|---|---|---|
| `CONFIG GET` | `-ERR syntax error\r\n` | `-ERR wrong number of arguments for 'config\|get' command\r\n` | DIFFERS -> RESOLVED |
| `CONFIG SET maxmemory` | `-ERR syntax error\r\n` | `-ERR wrong number of arguments for 'config\|set' command\r\n` | DIFFERS -> RESOLVED |
| `CONFIG REWRITE x` | `-ERR syntax error\r\n` | `-ERR wrong number of arguments for 'config\|rewrite' command\r\n` | DIFFERS -> RESOLVED |
| `CONFIG RESETSTAT x` | `-ERR syntax error\r\n` | `-ERR wrong number of arguments for 'config\|resetstat' command\r\n` | DIFFERS -> RESOLVED |
| `CONFIG HELP x` | `-ERR syntax error\r\n` | `-ERR wrong number of arguments for 'config\|help' command\r\n` | DIFFERS -> RESOLVED |
| `CONFIG BOGUS` | `-ERR syntax error\r\n` | `-ERR unknown subcommand 'BOGUS'. Try CONFIG HELP.\r\n` | DIFFERS -> RESOLVED |

### OBJECT (10 probes)

| Probe | Before target | Redis 7.4 | Result |
|---|---|---|---|
| `OBJECT ENCODING` | `-ERR wrong number of arguments for 'object\|encoding' command\r\n` | same | CONSISTENT |
| `OBJECT ENCODING k x` | `-ERR wrong number of arguments for 'object' command\r\n` | `-ERR wrong number of arguments for 'object\|encoding' command\r\n` | DIFFERS -> RESOLVED |
| `OBJECT REFCOUNT` | `-ERR wrong number of arguments for 'object\|refcount' command\r\n` | same | CONSISTENT |
| `OBJECT REFCOUNT k x` | `-ERR wrong number of arguments for 'object' command\r\n` | `-ERR wrong number of arguments for 'object\|refcount' command\r\n` | DIFFERS -> RESOLVED |
| `OBJECT IDLETIME` | `-ERR wrong number of arguments for 'object\|idletime' command\r\n` | same | CONSISTENT |
| `OBJECT IDLETIME k x` | `-ERR wrong number of arguments for 'object' command\r\n` | `-ERR wrong number of arguments for 'object\|idletime' command\r\n` | DIFFERS -> RESOLVED |
| `OBJECT FREQ` | `-ERR wrong number of arguments for 'object\|freq' command\r\n` | same | CONSISTENT |
| `OBJECT FREQ k x` | `-ERR wrong number of arguments for 'object' command\r\n` | `-ERR wrong number of arguments for 'object\|freq' command\r\n` | DIFFERS -> RESOLVED |
| `OBJECT HELP x` | `-ERR unknown subcommand 'HELP'. Try OBJECT HELP.\r\n` | `-ERR wrong number of arguments for 'object\|help' command\r\n` | DIFFERS -> RESOLVED |
| `OBJECT BOGUS` | `-ERR unknown subcommand 'BOGUS'. Try OBJECT HELP.\r\n` | same | CONSISTENT |

### MEMORY (8 probes)

| Probe | Before target | Redis 7.4 | Result |
|---|---|---|---|
| `MEMORY USAGE` | `-ERR wrong number of arguments for 'memory\|usage' command\r\n` | same | CONSISTENT |
| `MEMORY USAGE k SAMPLES 1 x` | `-ERR wrong number of arguments for 'memory' command\r\n` | `-ERR syntax error\r\n` | DIFFERS -> RESOLVED |
| `MEMORY STATS x` | `-ERR unknown subcommand 'STATS'. Try MEMORY HELP.\r\n` | `-ERR wrong number of arguments for 'memory\|stats' command\r\n` | DIFFERS -> RESOLVED |
| `MEMORY DOCTOR x` | `-ERR unknown subcommand 'DOCTOR'. Try MEMORY HELP.\r\n` | `-ERR wrong number of arguments for 'memory\|doctor' command\r\n` | DIFFERS -> RESOLVED |
| `MEMORY PURGE x` | `-ERR unknown subcommand 'PURGE'. Try MEMORY HELP.\r\n` | `-ERR wrong number of arguments for 'memory\|purge' command\r\n` | DIFFERS -> RESOLVED |
| `MEMORY MALLOC-STATS x` | `-ERR unknown subcommand 'MALLOC-STATS'. Try MEMORY HELP.\r\n` | `-ERR wrong number of arguments for 'memory\|malloc-stats' command\r\n` | DIFFERS -> RESOLVED |
| `MEMORY HELP x` | `-ERR unknown subcommand 'HELP'. Try MEMORY HELP.\r\n` | `-ERR wrong number of arguments for 'memory\|help' command\r\n` | DIFFERS -> RESOLVED |
| `MEMORY BOGUS` | `-ERR unknown subcommand 'BOGUS'. Try MEMORY HELP.\r\n` | same | CONSISTENT |

### LATENCY (8 probes)

| Probe | Before target | Redis 7.4 | Result |
|---|---|---|---|
| `LATENCY HISTORY` | `-ERR unknown subcommand 'HISTORY'. Try LATENCY HELP.\r\n` | `-ERR wrong number of arguments for 'latency\|history' command\r\n` | DIFFERS -> RESOLVED |
| `LATENCY HISTORY command x` | `-ERR unknown subcommand 'HISTORY'. Try LATENCY HELP.\r\n` | `-ERR wrong number of arguments for 'latency\|history' command\r\n` | DIFFERS -> RESOLVED |
| `LATENCY GRAPH` | `-ERR unknown subcommand 'GRAPH'. Try LATENCY HELP.\r\n` | `-ERR wrong number of arguments for 'latency\|graph' command\r\n` | DIFFERS -> RESOLVED |
| `LATENCY GRAPH command x` | `-ERR unknown subcommand 'GRAPH'. Try LATENCY HELP.\r\n` | `-ERR wrong number of arguments for 'latency\|graph' command\r\n` | DIFFERS -> RESOLVED |
| `LATENCY DOCTOR x` | `-ERR unknown subcommand 'DOCTOR'. Try LATENCY HELP.\r\n` | `-ERR wrong number of arguments for 'latency\|doctor' command\r\n` | DIFFERS -> RESOLVED |
| `LATENCY LATEST x` | `-ERR unknown subcommand 'LATEST'. Try LATENCY HELP.\r\n` | `-ERR wrong number of arguments for 'latency\|latest' command\r\n` | DIFFERS -> RESOLVED |
| `LATENCY HELP x` | `-ERR unknown subcommand 'HELP'. Try LATENCY HELP.\r\n` | `-ERR wrong number of arguments for 'latency\|help' command\r\n` | DIFFERS -> RESOLVED |
| `LATENCY BOGUS` | `-ERR unknown subcommand 'BOGUS'. Try LATENCY HELP.\r\n` | same | CONSISTENT |

### SLOWLOG (5 probes)

| Probe | Before target | Redis 7.4 | Result |
|---|---|---|---|
| `SLOWLOG GET 1 x` | `-ERR wrong number of arguments for 'slowlog' command\r\n` | `-ERR unknown subcommand or wrong number of arguments for 'GET'. Try SLOWLOG HELP.\r\n` | DIFFERS -> RESOLVED |
| `SLOWLOG LEN x` | `-ERR unknown subcommand 'LEN'. Try SLOWLOG HELP.\r\n` | `-ERR wrong number of arguments for 'slowlog\|len' command\r\n` | DIFFERS -> RESOLVED |
| `SLOWLOG RESET x` | `-ERR unknown subcommand 'RESET'. Try SLOWLOG HELP.\r\n` | `-ERR wrong number of arguments for 'slowlog\|reset' command\r\n` | DIFFERS -> RESOLVED |
| `SLOWLOG HELP x` | `-ERR unknown subcommand 'HELP'. Try SLOWLOG HELP.\r\n` | `-ERR wrong number of arguments for 'slowlog\|help' command\r\n` | DIFFERS -> RESOLVED |
| `SLOWLOG BOGUS` | `-ERR unknown subcommand 'BOGUS'. Try SLOWLOG HELP.\r\n` | same | CONSISTENT |

### Outer and valid controls

The six one-token forms `ACL`, `CONFIG`, `OBJECT`, `MEMORY`, `LATENCY`, and `SLOWLOG` were and
remain CONSISTENT: each replies exactly
`-ERR wrong number of arguments for '<lowercase-container>' command\r\n`.

The six valid controls (`ACL WHOAMI`, unmatched `CONFIG GET`, missing-key `OBJECT ENCODING`,
missing-key `MEMORY USAGE`, `LATENCY RESET`, `SLOWLOG RESET`) produced zero errors before and after.

## Confirmed handoffs (not changed)

### Missing feature arms

These are command-implementation gaps, not arity-table defects. Adding a fake arity row without
the valid operation would make discovery misleading, so they are handed to their feature lanes.

| Probe | Final target | Redis 7.4 | Handoff |
|---|---|---|---|
| `ACL DRYRUN default` | `-ERR unknown subcommand 'DRYRUN'. Try ACL HELP.\r\n` | `-ERR wrong number of arguments for 'acl\|dryrun' command\r\n` | ACL DRYRUN implementation |
| `ACL DRYRUN default GET k` | `-ERR unknown subcommand 'DRYRUN'. Try ACL HELP.\r\n` | `+OK\r\n` | ACL DRYRUN implementation |
| `LATENCY HISTOGRAM __arity_no_such_command__` | `-ERR unknown subcommand 'HISTOGRAM'. Try LATENCY HELP.\r\n` | `*0\r\n` | LATENCY HISTOGRAM implementation |

### MULTI admission order

Subcommand counts that fit a container's broad registry row are validated when the queued command
executes, while Redis validates fixed-arity subcommands before queuing and dirties the transaction.
Minimal confirmed reproducer:

```text
MULTI
CONFIG GET
EXEC

target: +OK, +QUEUED, *1 then -ERR wrong number of arguments for 'config|get' command
redis:  +OK, -ERR wrong number of arguments for 'config|get' command,
        -EXECABORT Transaction discarded because of previous errors.
```

The same ordering was confirmed for `MEMORY STATS x` and `SLOWLOG LEN x`. Variadic-metadata arms
prove that a blanket pre-queue rejection would also be wrong: Redis queues
`MEMORY USAGE k SAMPLES 1 x` and `SLOWLOG GET 1 x`, then returns their syntax/generic error inside
the EXEC array, while the target's existing outer maxima reject them before queuing. Correcting
both classes needs per-subcommand admission in `multi.inc` (the scatter/transaction core), which
this lane was explicitly told not to touch. The direct-command replies requested by this lane are
byte-exact; this transaction-order work is handed on with the sequences above.

## Exact command list

Representative server boots (all server processes were taskset-pinned, resolved from their
listening socket before termination, and their ports were confirmed released):

```sh
taskset -c 32-39 ./build/tomokv --bind 127.0.0.1 --port 7460 \
  --ratio 4:4 --shards 4 --atomic 0 --enable-debug-command yes
taskset -c 32-39 ./build/tomokv --bind 127.0.0.1 --port 7460 \
  --ratio 4:4 --shards 4 --atomic 1 --enable-debug-command yes
taskset -c 32-39 ./build/tomokv --bind 127.0.0.1 --port 7460 \
  --ratio 4:4 --shards 4 --atomic 0 --enable-debug-command yes \
  --aclfile /home/user/Projects/tomokv-cpp-arity/scratchpad/t-arity-acl.acl
taskset -c 32-39 ./build/tomokv --bind 127.0.0.1 --port 7460 \
  --ratio 4:4 --shards 4 --atomic 1 --enable-debug-command yes \
  --aclfile /home/user/Projects/tomokv-cpp-arity/scratchpad/t-arity-acl.acl
taskset -c 40-47 /tmp/claude-1000/redis74/src/redis-server \
  --bind 127.0.0.1 --port 7461 --save '' --enable-debug-command yes
```

Build and sanitizer:

```sh
make -j8
make -j8 asan
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 taskset -c 32-39 \
  ./build/tomokv-asan --bind 127.0.0.1 --port 7462 \
  --ratio 4:4 --shards 4 --atomic 0 --enable-debug-command yes
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 taskset -c 32-39 \
  ./build/tomokv-asan --bind 127.0.0.1 --port 7462 \
  --ratio 4:4 --shards 4 --atomic 1 --enable-debug-command yes
make clean && make -j8
```

Tests (the differ seed is the sixth positional argument):

```sh
python3 tests/arity.py 127.0.0.1 7460
python3 tests/differ.py 127.0.0.1 7460 127.0.0.1 7461 arity 7
python3 tests/differ.py 127.0.0.1 7460 127.0.0.1 7461 arity 23
python3 tests/differ.py 127.0.0.1 7460 127.0.0.1 7461 arity 91
python3 tests/limits.py 127.0.0.1 7460
python3 tests/acl.py 127.0.0.1 7460 \
  /home/user/Projects/tomokv-cpp-arity/scratchpad/t-arity-acl.acl
python3 tests/arity.py 127.0.0.1 7462
python3 tests/differ.py 127.0.0.1 7462 127.0.0.1 7461 arity 31337
```

## Test evidence

Pre-fix detector (the valid-form negative control was already zero):

```text
arity mechanism: cases=42 fired={'wrong': 6, 'unknown_or_wrong': 0, 'unknown': 16, 'syntax': 6} outer_controls=6 valid_control_errors=0
ARITY: 54 checks, 43 failures -> FAIL
DIFFER arity: 4200 ops, 4101 diffs -> FAIL
```

Optimized target, both `--atomic 0` and `--atomic 1`:

```text
arity mechanism: cases=42 fired={'wrong': 35, 'unknown_or_wrong': 4, 'unknown': 2, 'syntax': 1} outer_controls=6 compat_controls=9 valid_control_errors=0
ARITY: 63 checks, 0 failures -> PASS
DIFFER arity: 4200 ops, 0 diffs -> PASS  # seed 7
DIFFER arity: 4200 ops, 0 diffs -> PASS  # seed 23
DIFFER arity: 4200 ops, 0 diffs -> PASS  # seed 91
```

Load-bearing batteries, both atomic modes:

```text
limits: PASS
acl: PASS (grammar, AUTH, enforcement/closure, revocation, ACL LOG, SAVE/LOAD)
```

ASAN/UBSAN (`make -j8 asan`), no sanitizer diagnostics on either termination:

```text
--atomic 0: ARITY: 63 checks, 0 failures -> PASS
--atomic 0: DIFFER arity: 4200 ops, 0 diffs -> PASS  # seed 31337
--atomic 1: ARITY: 63 checks, 0 failures -> PASS
```

Final clean optimized build and smoke:

```text
make clean && make -j8                                      PASS
ARITY: 63 checks, 0 failures -> PASS
DIFFER arity: 4200 ops, 0 diffs -> PASS
```

No loopback performance measurement was requested or taken.

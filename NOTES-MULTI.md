# MULTI / EXEC / WATCH notes

## Contract and command flow

- `MULTI` is connection-local. Parsing continues and accepted commands are copied into cold
  connection-owned storage; their public operations retire immediately with `+QUEUED`.
- Unknown commands and arity failures mark the transaction dirty at queue time. `EXEC` then returns
  `EXECABORT` without executing any queued command. Errors found by a command while it executes stay
  in that command's EXEC array element.
- Blocking/persistence, all six subscription controls, and nondeterministic random-selection
  commands are rejected in MULTI with `ERR Command not allowed inside a transaction`. Redis permits
  the four regular subscription controls in MULTI for backward compatibility, but our IO-owned
  async home protocol cannot execute as an EXEC child; rejecting all six controls is the explicit
  divergence. Blocking whole-keyspace/persistence operations, CONFIG mutations outside the data
  group, `RANDOMKEY`, and the random-member/removal families are the other exclusions.
- Key-routed commands execute on the owning shard. Same-owner commands reuse their normal handler;
  general multi-owner commands reuse the existing scatter lowering. PING/ECHO and the common
  MGET/MSET/MSETNX/DEL/EXISTS forms have compact transaction paths. Connection-local commands return
  to the connection's IO owner for their handler and reply construction.
- `EXEC` force-admits through the atomic group window even when `--atomic`/`CONFIG SET atomic 0` is
  selected. One shared epoch and abort word cover every queued write. Candidate objects are installed
  privately at epoch zero; the last group participant draws and release-publishes exactly one ticket.
  Foreign readers resolve the predecessor while the ticket is zero, while the originating connection
  can resolve its own private candidate for read-your-writes.

## WATCH ownership and lifetime

- Every shard owns its `key -> watcher` map and only its executor touches the map. An empty registry
  allocates nothing; ordinary writes test `has_watches()` before entering any registry helper.
- EXEC validation installs an owner-local reservation on each watched key. A competing writer retries
  behind an unresolved reservation. This closes the check/write gap and makes the two-client CAS race
  choose exactly one winner.
- A committed write dirties current-generation watchers. An aborted group consumes its reservation
  without dirtying them. Reservations carry a separate state-lifetime pin, including conditional
  groups that install no MVCC record.
- EXEC removes watches before command execution and clears the connection generation/dirty bit at
  retirement. DISCARD and UNWATCH remove owner registry entries; connection close dispatches the same
  cleanup invisibly and keeps the Client alive until all owner references are gone.

## Layout and ownership constraints

- Transaction task pointers use the low bit of the existing `Task::scatter` field; `Task` and `Op`
  did not grow. The Client transaction pointer and watch atomics consume existing tail padding.
- The footprint locks remain `sizeof(Op) == 336` and `sizeof(Client) == 1984`.
- Implementation stays in the command single-TU layout: `multi.inc` is textually included by
  `xshard.cc`, beside the existing scatter and atomic `.inc` files.
- Shard data and WATCH registries are touched only by their assigned executor threads: “db shards
  need to only be touched by their ex threads.”

## Directed validation

Run against a server already bound to the lane port:

```sh
taskset -c 224-231 python3 tests/multi_exec.py 127.0.0.1 7951
```

The battery covers forced atomic behavior with atomic mode off, queue invisibility, queue-time versus
runtime errors, DISCARD/UNWATCH/reset behavior, connection-local and cursor commands, heterogeneous
MSET/MGET, existing-scatter KEYS/RENAME lowering, LMPOP-to-LLEN read-your-writes, a concurrent
torn-read arm, and 100 rounds of two-client WATCH CAS. The queue-error cases use directed
Redis-compatible assertions.

Required repository gate:

```sh
taskset -c 224-231 env GATE_PORT=7951 GATE_CORES=224-231 tests/gate.sh quick
```

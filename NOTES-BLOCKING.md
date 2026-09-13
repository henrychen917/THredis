# Blocking collections

This lane implements `BLPOP`, `BRPOP`, `BLMOVE`, `BRPOPLPUSH`, `BLMPOP`, `BZMPOP`,
`BZPOPMIN`, and `BZPOPMAX`, including finite fractional timeouts and Redis's timeout-zero
meaning "forever."

## Ownership and lifecycle

The governing invariant remains: **db shards need to only be touched by their ex threads.** Each
`Shard` has an opaque, lazily allocated owner-only registry. Its queues are keyed by hash plus the
full key bytes, and each queue is FIFO. IO threads and foreign executors may only update the
registry's atomic waiter/dirty hints; they never traverse or mutate a registry.

A blocking command is lowered by its connection's IO thread into one probe Task per participating
shard. The opaque `BlockingState` lives outside `Op`; the existing zero-copy marker fields identify
it, so `sizeof(Op)==336` remains locked. `Task::scatter` is only an opaque carrier for these Tasks.
No blocking fields were added to the task queue entry. The new `Client::blocked_` flag occupies
existing tail padding, preserving `sizeof(Client)==1984`.

The first probe checks all keys and selects the earliest ready argument. If all are empty, a second
owner pass rechecks each key while registering aliases. A writer that lands during this pass records
the minimum ready argument; it cannot be lost between probe and park. Once parked, a committed
write claims the first live context on the owner and consumes directly. Multi-element writes serve
successive FIFO contexts serially while data remains; they do not wake a herd of connections.

The connection remains receive-capable only so EOF/disconnect is observable, but the parser stops
at the blocking frame. Younger pipelined frames are not parsed until the blocking ROB slot retires.
`BLMOVE` and `BRPOPLPUSH` wake through an IO-owned continuation into the existing two-hop scatter
engine, retaining the connection barrier until the move's ordinary retirement.

## Publication and timeout rules

Immediate wake hooks exist behind `Shard::has_blocking_waiters()` for list pushes and sorted-set
inserts. Local and cross-owner move destinations use the same hook. The common no-waiter arm is a
predicted-false check; registries allocate nothing until a command actually parks.

Atomic scatter/apply mutations never inspect waiters during private epoch-zero installation.
Only the successful promotion/publish path marks touched waiter shards dirty and wakes their owners.
An aborted group therefore exposes no element and wakes nobody. Plain writes that overlap pending
atomic records defer their waiter publication hook until `xshard_plain_finish` makes the mutation
visible.

Executor owners sweep registries on the existing coarse beat. A finite deadline returns a null
array for every blocking collection command, including BLMOVE and BRPOPLPUSH. Cancellation is an
atomic request; the owner removes aliases and completes the ROB slot, so an IO thread never frees a
client beneath an owner callback. `INFO CLIENTS` reports `blocked_clients`, while `INFO STATS`
reports registry alias count as `blocking_waiters`.

Blocking probes also participate in snapshot pre-image preparation before an immediate owner pop.

## Directed test

Run against a server supplied by the caller:

```sh
taskset -c 248-255 python3 tests/blocking.py 127.0.0.1 7953
```

The test covers argument-order priority, left/right list edges, FIFO wake order, multi-element
handoff, timeout bounds, all blocking reply shapes, the connection parse barrier, a
`BLMOVE`/`BRPOPLPUSH` publication chain, disconnect churn with both gauges returning to zero, and a
non-vacuous atomic visibility arm. The atomic arm discovers a cross-owner `LMOVE`, forces phase-two
admission to abandon under maxmemory, proves the waiter remains parked and the source remains
visible, then proves a later committed push wakes it.

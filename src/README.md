# Source directory

[main.cc](main.cc) is the executable entry point: configuration, placement,
recovery, worker startup, and shutdown. The directories below separate ownership
and lifetime responsibilities; the [architecture guide](../docs/ARCHITECTURE.md)
explains how they interact.

| Directory | Responsibility |
| --- | --- |
| [base](base/README.md) | Shared byte/numeric helpers, allocation, and hardware topology discovery. |
| [core](core/README.md) | Server state, routing, worker loops, reclamation coordination, and runtime placement. |
| [net](net/README.md) | Connections, RESP, reply ordering, transports, and socket-buffer lifetimes. |
| [exec](exec/README.md) | Operation storage and inter-thread queue primitives. |
| [store](store/README.md) | Keyspace tables, object representation, expiry, versioning, and memory lifetime. |
| [cmd](cmd/README.md) | Command semantics, type implementations, transactions, scripts, and scatter/gather. |
| [snapshot](snapshot/README.md) | Consistent snapshot cuts, capture, file format, and recovery. |
| [persist](persist/README.md) | Append-only recording, rewrite, durability frontiers, and replay. |

Headers contain specialized implementation as well as declarations. `.inc` files
are textually included into their owning class or translation unit. The root
[Makefile](../Makefile) lists the compiled `.cc` files.

# Thread amortization study surface

`thread-mode` and the study-only `x-overlap` are one boot-latched measurement surface. The mode
names are `2s` (separate IO and executor threads, the default) and `1s` (one generalized thread owns
both roles). `split` and `fused` remain parser aliases so old invocations keep working.

| mode | overlap 0 | overlap 1 | overlap 2 |
| --- | --- | --- | --- |
| `2s` | ordinary split IO/ex loops | exact `t-iopipe` WB/IFID batch schedule | rejected at config validation |
| `1s` | coarse IFID → EX → WB rotation | exact `t-genthread` `iofused` schedule | gated `iofused` three-way schedule |

Use `--x-overlap 0|1|2` for these study schedules and `--x-ex-sched 0|1` for the executor study.
The old public schedule spellings and compatibility aliases are removed; see
[DESIGN-KNOBS.md](DESIGN-KNOBS.md). Below, "overlap" names the internal schedule value.
Overlap 2 emits a boot warning because it is an experimental research schedule.
Unified overlap 1 and 2 require `net-io uring`; the split
iopipe schedule retains its explicit epoll specialization. No schedule constant is runtime-tunable.

Overlap 1 is deliberately the measured implementation, not a family resemblance. In `2s`, its
shallow rotation is WB observe, IFID receive, WB prefetch, IFID parse/hash, WB retire/prepare, IFID
post, and WB submit/reclaim; its existing depth gate selects the measured natural-order path. In
`1s`, `iofused` retains the fork's targeted IFID work, WB dependency prefetch, coarse executor turn,
and SEND-sensitive submission boundary. Overlap 2 builds on those same mechanics. Its deep path
freezes and prefetches a ready-list WB batch, performs the targeted IFID batch, gathers/schedules and
prefetches one whole EX batch, uses WB prepare/pump as the EX load-gap filler, then executes that EX
batch and drains any remaining EX batches normally. Kernel submission remains at the single outer
boundary. A one-bit depth gate returns the next rotation to the unchanged coarse IFID → EX → WB
order after a thin pass. These shapes are fixed so results can be compared against overlap 0's
plain-loop baseline.

The `1s` overlap-1 implementation remains unchanged. Both unified overlap schedules use its pending
IFID/WB ready lists and the fused inbox's lifetime-fixed geometry directly (1,024 slots per
producer), including blocking, transaction, scatter/script follow-up, and stale-owner paths. The
overlap-2 deep path adds only one gate bit and a stack-local whole EX batch to overlap 1's WB batch;
it has no IFID reservations, residual ages, A/D executor contexts, delayed lane retirement, or
unpublished operations. The old streams source is retained for branch comparison but is unreachable
from overlap-2 dispatch. Reservation-capable queue operations remain because overlap-0 read-local
demotion uses them; neither overlap 1 nor overlap 2 enters them.

# Masked-monolith executor inbox

## Scope and invariants

This lane replaces only the `Task` slot topology used for parsed-operation dispatch. Each physical
thread owns one fixed-capacity `Task` slot array for the periods when that thread is an executor.
The allocation is made by that physical thread after CPU pinning, and construction first-touches
every slot there. A thread changing role reuses the same allocation; a FLIP changes only its block
mask.

The existing footprint locks still compile unchanged:

- `sizeof(Op) == 336`
- `sizeof(Client) == 1984`
- `sizeof(Task) == 32`

No atomic was added to `Op` or `Task`. Each sub-ring retains the old queue's producer-owned `tail`,
consumer-owned `head` and `retired` frontier, cached peer index, and release/acquire ordering.
`client_in`, `release_in`, and `transfer_in` are separate transfer paths and retain their existing
containers.

## Allocation and block mask

The total array capacity is fixed at boot to:

```
physical thread count * 1024 Task slots
```

This is the old worst-case per-consumer burst envelope (`1024` slots from every possible physical
producer), consolidated into one allocation. It depends on physical thread count, not the live
io:ex split, and is never resized on the hot path. There is no configuration knob: the existing
envelope is sufficient, so a capacity override would add an operational choice without a current
need.

At boot and at `ExInstall`, a consumer assigns every current producer a contiguous power-of-two
block. A lane descriptor stores `base`, `capacity`, and `mask = capacity - 1`; slot lookup is:

```
slots[base + (monotonic_index & mask)]
```

The allocation is 64-byte aligned. `Task` is 32 bytes and every capacity is at least 256 slots, so
every block begins and ends on cache-line boundaries. Two producers can never write the same cache
line. Head/retired/cache and tail/cache occupy distinct aligned cache lines exactly as in the old
SPSC queue.

IO producers receive the burst capacity after the internal reserve, equally rounded down to a
power of two. The capacity cannot fall below the old 1024 slots per IO producer. The tree also has
pre-existing direct executor producers: scatter phase 2, script apply, and stale-owner forwarding.
Relaying those through IO would change semantics and violate the no-relay rule, so each current EX
producer receives a 256-slot block in the same monolith. `256` is the existing maximum owner-grouped
bundle, not a tunable number. Unused rounding slack remains unassigned.

For the directed 64-thread extremes:

| live split | fixed total | IO block | EX direct block |
| --- | ---: | ---: | ---: |
| 63:1 | 65,536 slots | 1,024 slots each | 256 slots |
| 1:63 | 65,536 slots | 32,768 slots | 256 slots each |

Thus the single IO producer inherits the large unreserved part of the burst envelope while the
63-producer case preserves the old per-producer capacity.

Owner-grouped `push_batch` writes consecutive indices inside exactly one producer block and makes
one release store to that lane's tail. A wrap may split the physical writes at the end of the same
block, as it did in the old ring; it can never cross into another producer's block.

## Consumer scan and wake protocol

Every producer has independent head/tail scan points. `task_notify_` remains the active-producer
summary bitmap: the hot drain takes a word, visits only its set producers, and drains each indicated
sub-ring. Producers still publish slots before setting the bit, and the consumer still takes the
bit before draining. The existing read-first `NotifyMask::set` avoids an RMW while a producer is
already active.

The bitmap is a hint, not a correctness oracle. There are two independent full lookers:

1. The existing 100 microsecond depth-sampling beat already reads every producer scan point. If it
   sees a non-empty task lane without relying on the summary, it republishes that producer hint, so
   the next ordinary drain serves it even while other lanes keep the executor busy.
2. Before parking, `drain_tasks_unmasked` still directly drains every producer lane. The subsequent
   armed-depth recheck prevents sleeping on a push that raced the park protocol.

The task monolith has one blocked flag because the old sleep path armed every task channel together.
This removes sleep-only stores and does not change the producer wake decision or its memory order.

## Stable fast path

`push`, `push_prepared`, `push_batch`, `pop`, and `retire` have the same control-flow and publication
shape as `ExQueue`:

- producer: relaxed tail load, the same cached-full branch/acquire refresh, slot writes, one release
  tail store;
- consumer: relaxed head load, the same cached-empty branch/acquire refresh, slot read, one release
  head store;
- retirement: the same separate release `retired` store;
- notification: the same summary-bit transition and parked-peer check.

There is no role test, remask test, allocation test, growth test, or new branch in post/drain. Block
base/mask values are tenure-stable descriptor fields. Owner-grouped bulk posting remains one capacity
preflight and one tail publication.

## FLIP stage diff

No stage was added. The current stage order remains:

1. `IoDrain`
2. `IoPrepare`
3. `ExDrain`
4. `ClientPrepare`
5. `ClientCommit`
6. `ClientInstall`
7. role commit and `RoleReady`
8. `ShardCommit`
9. `ExInstall`
10. completion to `Idle`

The transport-specific differences are inside the standing steps:

| stage | existing obligation | masked-inbox delta |
| --- | --- | --- |
| `IoDrain` through `IoPrepare` | pause new dispatch; drain IO work; prepare destinations | none |
| `ExDrain` | drain retries/tasks/releases/AOF/notifications and acknowledge only when `flip_quiesced()` | `ex_inbound_quiesced()` now checks every monolith lane's `retired == tail`; converting and surviving producers are therefore parked with no claimed task left in a block |
| `ClientPrepare` through `RoleReady` | move clients, commit roles, activate dormant loops, and wait for ready roles | none; the old mask remains untouched while rollback is still possible |
| `ShardCommit` | publish final shard owners | none |
| `ExInstall` | rebind each final executor's shard notification pointers, acknowledge; coordinator completes after all EX acks | **before rebinding/ack**, each final executor calls `remask_task_inbox_quiesced(final_io, final_ex)` inside its existing allocation |

`ExInstall` is after the irreversible role/shard decision and before dispatch resumes. Remasking is
plain descriptor mutation on the consumer thread. Its acknowledgement is a release publication;
the coordinator acquire-observes all acknowledgements before publishing `Idle`, and producers
acquire-observe that stage before dispatching again. No producer can read a partially installed
base/mask. A pre-commit rollback never remasks and therefore needs no inverse operation.

## Cold growth discipline

The selected fixed envelope has no runtime growth trigger. `MaskedSpscArray::grow_quiesced` exists
for a future explicit capacity increase and refuses unless every lane is quiesced. It allocates and
first-touches a replacement on the consumer thread, installs a fresh mask, then releases the old
array. A caller must place it at the same parked point as the `ExInstall` remask; it is intentionally
unreachable from post, drain, backpressure, or any other hot path.

## Directed functional proof

`tests/spscmask_flip.py` requires a 64-thread 63:1 boot. Its gated `DEBUG IO-THREAD` observation
retains one live connection from every initial IO owner, proving all 63 producer lanes fire. Every
connection continuously pipelines ordered `SET key sequence` / `GET key` pairs while the control
connection flips 63:1 to 1:63 and back. The test requires every stream counter to advance under all
three installed masks, checks every reply position, drains every pipeline, and point-reads the final
acknowledged value for all 63 keys.

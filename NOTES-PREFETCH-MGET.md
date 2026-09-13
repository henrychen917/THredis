# Batched owner-side gather prefetch

## Static investigation

This lane was inspected and changed source-only. Per `LANE_RULES.md`, no build, server, test,
load generator, or benchmark was run.

### Current gather shape

- Scatter preparation hashes every explicit key once and records both the full hash and routed shard
  in `RouteKey` (`src/cmd/scatter_engine.inc:1475-1490`). The arena then carries that hash in
  `KeyRef` (`src/cmd/scatter_engine.inc:1698-1701`), so an owner does not need to reread or rehash key
  bytes to know the bucket address.
- `build_initial_groups` counts keys by routed shard and gives every `ShardGroup` one dense range in
  `state.key_order` (`src/cmd/scatter_engine.inc:801-843`). A task therefore already has exactly the
  hash batch belonging to the shard it is executing.
- The MGET gather walks that owner range in a single loop. For each key it calls the selected lookup
  and immediately tests the object, reads/converts the value, and copies or borrows it before moving
  to the next key (`src/cmd/scatter_engine.inc:2533-2571`). Thus lookup/dereference chains are issued
  serially today.
- Generic multi-key arms have the same shape: MSET mutates one key at a time
  (`src/cmd/scatter_engine.inc:2831-2850`), DEL/UNLINK probes and erases one key at a time
  (`src/cmd/scatter_engine.inc:2851-2872`), and the default image gather looks up and serializes one
  object before advancing (`src/cmd/scatter_engine.inc:2959-2999`). The latter is the source gather
  used by set-operation families such as SINTERSTORE.
- MGET result order is independent of owner execution order. Each owner writes the slot indexed by
  the original key position, and `assemble_mget` emits `state.values[0..key_count)` in original argv
  order (`src/cmd/xshard_commands.inc:1570-1625`). A bucket-only prepass need not and must not change
  either loop.

### Hash and bucket availability

`FlatStore::hash_key` produces the full server hash (`src/store/flatstore.h:1237-1257`). The router
uses its low bits, while `FlatStore::slot_start` remixes it for the open-addressed home slot (the
reason is documented at `src/store/flatstore.h:55-59`). `FlatStore::prefetch` already maps a carried
full hash to the current table's home slot and, during incremental rehash, the old table's home slot
(`src/store/flatstore.h:1230-1235`). The bucket addresses are therefore known before lookup.

### Existing prefetch does not cover scatter

The shipped executor prefetch gathers up to 32 independent tasks, hints their store slots, and then
executes them (`src/core/ex_loop.h:584-611`). Its eligibility test explicitly requires
`!batch[i].scatter` (`src/core/ex_loop.h:591-598`). MGET/MSET and generic multi-key owner tasks enter
`xshard_execute` with a scatter state, so none of their per-key hashes is covered by that mechanism.

Historical context was checked before adding another prefetch path:

At lane start there was no `NOTES-PREFETCH*.md` in this worktree, elsewhere under `/home/user`, or
among reachable Git object names. The available record was the commits and prefetch review below.

- `610d90cbb` records the narrow batched storage-slot prefetch as worth +2-3% when DRAM-bound.
- `4bab6353f` records the broader EX prefetch experiment as a wash despite 180-355M issued hints:
  the measured worker was fixed-work-bound, and a 21x dataset increase cost only 3.5% throughput.
- `96c2009c8` restored only resolved-value lookahead for the old C MGET reassembly after removing the
  broad machinery. That is not present in this C++ gather and is a different cache target.
- Redis PR #15133 was read only for its control-flow shape: make a bucket-hint pass before the normal
  command loop. No Redis source or identifiers are copied here.

The narrow conclusion is to reuse the existing `FlatStore::prefetch(hash)` primitive for the hashes
already grouped at the owner. Do not add candidate-object/value chasing, another state machine, a
cross-owner table read, or a configuration knob.

### Resize and lifetime safety

The storage invariant is one table per shard, executed by exactly one worker at a time
(`src/store/flatstore.h:4-8`); the shard invariant repeats that exactly one thread touches its
`FlatStore` at a time (`src/core/shard.h:1-6`). A gather task runs on that owner and its group contains
only keys routed to that shard.

`FlatStore::prefetch` forms an address only after checking each table pointer. During a complete
owner prepass no other thread can start/advance rehash or free either array. Subsequent real
operations may advance the incremental move on the same thread (`src/store/flatstore.h:702-714,
1960-1983`), including freeing the drained old array, but the prefetch retained and dereferenced
nothing. Every operation reloads `tab_[0]/tab_[1]` and performs the authoritative lookup. A resize
can therefore make an earlier hint stale or useless, but cannot make it a use-after-free or affect
the lookup result.

For blind atomic writes, capacity preparation can change table geometry. Their prepass must be after
`atomic_prepare_capacity`, not before it. Non-atomic/read gathers must issue their prepass before the
per-key MVCC promotion loop, because promotion is itself a serial storage touch.

## Implemented change

The owner-group helper does nothing for fewer than two local keys and otherwise calls
`shard.store().prefetch(hash)` for the complete owner range. It understands both the normal
`state.key_order` range and SORT's derived-key gather range
(`src/cmd/scatter_engine.inc:854-872`). It is called:

1. before MVCC promotion and ordinary non-atomic gather work
   (`src/cmd/scatter_engine.inc:2310-2333`); and
2. after capacity preparation, before any atomic-write existence/install pass
   (`src/cmd/scatter_engine.inc:2356-2388`).

The command loops, result indices, and assembly stay unchanged.

## Measurement surface and acceptance geometry

Commands the change can touch are cross-shard scatter groups with at least two keys on one owner:
MGET, MSET/MSETNX, DEL/UNLINK, EXISTS/TOUCH, set and sorted-set gather/store families (including
SINTERSTORE), and other generic multi-key source gathers. GET and SET never carry `Task::scatter`
and remain byte-path unchanged. Same-owner commands that take local-fast also remain unchanged.

The main session must measure, rather than infer, the win. Required cells are:

- GET and SET controls: ordinary pipelined single-key workload, unchanged throughput and latency.
- MGET and MSET: both a narrow width and a wide width, with keys found by walking candidates and
  `DEBUG SHARD` until every participating owner has at least two keys. Record throughput and tail
  latency; these are both affected and zero-regression cells.
- DEL and one set-store command (SINTERSTORE is the headline): same owner-grouped geometry, with
  populated keys large enough that table buckets are not already resident.
- A one-key-per-owner negative control: no group is eligible, proving the branch does not issue a
  useless local hint.
- Resize overlap: preload close to a growth boundary, find owner-grouped keys with `DEBUG SHARD`, and
  verify the rehash counter advances during wide MGET/MSET/DEL traffic while xshard and multi differs
  remain exact. A geometry that does not move the rehash counter must fail loudly.

Use enough load generators to saturate the server: one memtier thread per load-generator core, not
memtier's default eight. No cell can be called a win in this lane until the main session records a
throughput, latency, or memory improvement without a throughput loss, and confirms zero loss for
GET, SET, MGET, and MSET.

**Winning cells established in this lane: none.** Measurement is forbidden here. The code is a
candidate only; the main session must fill in the cells above and deprecate it if either acceptance
gate fails.

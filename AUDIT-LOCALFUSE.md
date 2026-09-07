# Armed local GET: pass-by-pass redundancy audit

> Historical research record: configuration recipes and measurements below describe earlier
> revisions. Deleted controls are not accepted by the merged server, and the manual-map
> dispatch-scaling instrument is retired. For current controls and merge dispositions, see
> [DESIGN-KNOBS.md](DESIGN-KNOBS.md) and [MERGE.md](MERGE.md).

Baseline: `775aeea48` (mainline). Configuration audited: `--thread-mode 1s --read-local 1`,
defaults `--read-local-interleave 1 --read-local-prefetch-capture 1`, `TOMO_READ_LOCAL_SET_TAX_VARIANT=0`.

Evidence (perf `instructions`, pure GET p32, 32c, ~2590 instr/op):
`enqueue_local_read` 10.3% + `read_local_prefetch_capture` 8.6% + `prepare_local_read<true>` 7.9% +
`read_local_prefetch` 3.9% + `drain_local_reads_bounded_impl<true,*>` 5.1% = 36% (~930 instr/op).

Static instruction counts of the baseline binary (objdump, `build/tomokv`):

| symbol | static instr | note |
|---|---|---|
| `ExLoopT<true>::enqueue_local_read` | 73 | out of line; hot path ~42; 10.3% cannot be its own body (sample skid onto the call site) |
| `FlatStore::read_local_prefetch` | 76 | I0 |
| `FlatStore::read_local_prefetch_capture` | 329 | C0 |
| `ExLoopT<true>::prepare_local_read<true>` (.isra) | 318 | E0 |
| `ExLoopT<true>::prepare_local_read<false>` (.isra) | 296 | E0, `--read-local-prefetch-capture 0` |
| `FlatStore::read_local_probe` | 292 | E0, `--read-local-prefetch-capture 0` |
| `drain_local_reads_bounded_impl<true,true>` | 754 | + lambda#4 (`complete_local_prefix`) 109 |
| `drain_local_reads_bounded_impl<true,false>` | 625 | + lambda#4 109 |
| `Rob<64>::read_local_owner_conflicts_before<drain lambda>` | 278 | out of line, called per op from E0 |

## One local GET, every pass

Notation: `#n` = the n-th time this pass recomputes/reloads a value an earlier pass already had.

### P0 parser admission (`src/core/io_loop.h`)
- 3935 `rob.acquire_read_local()`; 4126-4135 `write_hazard`, `point_route`, **`op->hash`**, **`op->shard`** (the only
  place these are computed; good).
- 4076-4080 run extension: `spec->flags`, `multi_session()`, `fused_executor_->local_read_lane_has_room()` (#1 lane room).
- 4227 `owner_conflict_for_command()` -> `rob.read_local_owner_conflicts_before(dispatch_id)` (fast exit on empty owner map).
- 4250 `rob.read_local_write_conflicts(op->hash)` (fast exit on inactive ring).
- 4262-4324 first-of-run gates only: WATCH, blocked/subscriber, route, keymiss, **`store().read_local_state_acquire()`**
  (per-shard atomic-pending/churn gate), `local_read_lane_has_room(demand)` (#1 for a run head).
- 4963-4966 `op->mark_read_local()`, `rob.mark_current_read_local()`, `rob.publish()`,
  `enqueue_local_read(Task{c, op_id, -1, nullptr})` (32-byte temporary; **Task copy #1**).

State the parser holds in registers at 4966 and then throws away: `Op* op`, `spec`, `read_local_mget_candidate`,
the lane-room verdict, `op_id`.

### P1 `enqueue_local_read` (`src/core/ex_loop.h:233-247`)
| work | already known by | cost |
|---|---|---|
| `read_local_enabled()` + `read_local_impl()` abort check | parser is only reachable armed | 4 |
| `task.client` null abort | parser passed `c` | 2 |
| `client->rob().at(op_id)` -> Op (**Op resolve #1**) | parser has `op` | 7 |
| `op.read_local()` abort (877) | parser set it one line earlier | 3 |
| `read_local_mget(op)` (**mget #1**, 834) -> `spec` load + flag mask | `read_local_mget_candidate` | 5 |
| `lane_count == kInboxSlots` and demand-budget check (**lane room #2**) | 4080/4313 checked the same predicate; between the check and 4966 only `commit_reads()` runs, which can only *release* demand | 8 |
| `lane[tail] = task` (**Task copy #2**, 32 B) + `lane_fallbacks[tail] = None` | — | 4 |
| tail/count/demand updates | — | 6 |
Call/return + prologue: ~6. Total ~45 dynamic; only ~10 do work.
The lane is `Task[1024]` = 32 KB: it alone spans the L1D. Only `{client, op_id}` (16 B) is ever read from it.

### P2 gather (`ex_loop.h:1294-1332`) and mget scan (1341-1350)
| work | already known by | cost |
|---|---|---|
| `discard_tombstone_heads` per chunk: head `pending_read_local` (dispatch + flush acquire loads + bit) | — | 10/chunk |
| per op: `task.client != client`, `pending_read_local(op_id)` (**dispatch/flush reload per op**) | one client per chunk; dispatch/flush cannot move under a fused rotation (parser and sender are this thread; flush cannot pass a not-Done op) | 10 |
| `batch[count++] = task` (**Task copy #3**, 32 B) + fallback copy | — | 5 |
| mget scan: `rob().at()` (**Op resolve #2**) + `read_local_mget` (**mget #2**) per op | spec flags could be read once at gather | 12 |

### P3 I0 prefetch (`ex_loop.h:1356-1359` -> `flatstore.h:722-733`)
| work | already known by | cost |
|---|---|---|
| `rob().at()` (**Op resolve #3**) | P2 | 7 |
| `srv_->shard(op.shard).store()` (**store resolve #1**) | — | 4 |
| `read_local_enabled_` (#1), `read_local_state_acquire()` (**pub word load #1**: `atomic_pending_` ptr + `read_local_extended` + null abort + acquire load) | — | 6 |
| eligibility bits | — | 2 |
| `read_local_snapshot_topology` (**topology #1**: 6 acquire loads of tab/cap/mask + **pub word load #2** + compare) | — | 16 |
| per table: null/cap test, **`mix64(hash)` #1** (2 imul + 3 shr + 3 xor), `& mask`, prefetch | — | 14 (x2 when rehashing) |
Out-of-line call: 6. Total ~55.

### P4 C0 capture (`ex_loop.h:1361-1364` -> `flatstore.h:735-770`)
| work | already known by | cost |
|---|---|---|
| `rob().at()` (**Op resolve #4**), `srv_->shard().store()` (**store #2**), `op.key()` (argc + argv_heap branches) | P2/P3 | 14 |
| `read_local_enabled_` (#2), **pub word load #3**, pending test, mutating test | P3 (same word, same generation on the fast path) | 8 |
| `read_local_snapshot_topology` (**topology #2**, **pub word load #4**) | P3 snapshot is still valid iff the word is unchanged — one compare replaces seven loads | 16 |
| `read_local_capture_in(table0)`: `tag_of(hash)`, **`mix64(hash)` #2**, `& mask`, probe loop [slot acquire load, `ptr_of`, tag compare, `read_local_flags()` (**flags load #1**), `read_local_key(flags)` (**key_ptr/klen derive #1**: KeyExt/HasTtl offset math), `Slice==` (len + memcmp)] | P3 computed `mix64` and the home slot address and threw them away | 30 + memcmp |
| table1 probe (null in steady state) | — | 3 |
| **pub word load #5** + compare | needed (post-probe validation) | 5 |
| `read_local_prefetch_object`: `__builtin_prefetch(object)` (**redundant: the header line was just loaded by the key compare**), `type` load (**type #1**), `read_local_flags()` (**flags #2**), `read_local_key_ptr(flags) + read_local_klen(flags)` (**key_ptr/klen derive #2**), `encoding()` (**enc #1**), value prefetch | key compare had `flags`, `key_ptr`, `klen` in registers | 18 |
| return 32-B `{result, slot, object, state}` -> `captures[i]` | — | 5 |
Out-of-line call: 6. Total ~105 + memcmp.

### P5 E0 (`ex_loop.h:1376-1410` -> `prepare_local_read<true>` 1159-1264)
| work | already known by | cost |
|---|---|---|
| `rob().at()` (**Op resolve #5**) in the E0 loop | — | 7 |
| `read_local_owner_conflicts_before(op_id, lambda)` — out-of-line 278-instr function, per op, fast exit on `owner_slots_ == 0` | the owner map is per client and cannot gain bits during a chunk (parser and demotion do not run inside E0): one test per chunk decides all 32 ops | 10 |
| `prepare_local_read<true>(batch[i], &captures[i])`: `task.client` abort (#2), `rob().at()` (**Op resolve #6**), `op.read_local()` abort (#2), `read_local_mget(op)` (**mget #3**), `op.shard < 0` abort, `srv_->shard().store()` (**store #3**) | P2–P4 | 24 |
| copy result/object/state out of the capture; `AtomicPending`/`Missing`/`Churn` branches; `!object` abort; `!captured->slot` abort | — | 10 |
| `read_local_flags()` (**flags #3**), `type` (**type #2**), TTL test | C0 had them | 6 |
| `read_local_clear_reply(op)` (5 stores) before the first write | the op is pristine from `reset()`; only a retry needs the clear | 6 |
| `encoding()` (**enc #2**) switch; `read_local_reply_string` -> `encoding()` (**enc #3**) -> `read_local_str_value(flags)`: `read_local_key_ptr(flags) + read_local_klen(flags)` (**key_ptr/klen derive #3**), `encoding()` (**enc #4**), `raw_length_relaxed()` | C0 had the value pointer | 14 |
| `reply_bulk(op.sink(), ...)`: reserve (direct/reply select), `$`, `u64_to_dec`, memcpy, `\r\n`, advance | real work | ~35 + memcpy |
| `store.read_local_validate(state)`: `read_local_enabled_` (#3), eligibility (#2), **pub word load #6** + compare | only the load + compare is the validation; the state was eligible or we would not be here | 8 |
| return 12-B `PreparedLocalRead` -> `prepared[i]`; `fallbacks[i] = prepared[i].fallback`; `first_fallback` update | a fallback byte and two chunk counters suffice | 8 |
Call/return: 6. Total ~135 + memcpy.

### P6 completion (`ex_loop.h:1414-1432`, lambda `complete_local_prefix`)
| work | already known by | cost |
|---|---|---|
| `rob().at()` (**Op resolve #7**) | — | 7 |
| `stats.keyspace_hits += prepared[i].keyspace_hits`, `keyspace_misses += ...`, `hits++` (3 memory RMW per op) | chunk sums | 9 |
| `read_local_mget(op)` (**mget #4**) -> `mget_local_hits++` | gather | 5 |
| `note_command(op.spec->id)` | needed per op (per-command counters) | 6 |
| `release_local_read_demotion_demand(op)`: `read_local_impl()` abort, `op.read_local()` abort (#3), `read_local_mget(op)` (**mget #5**), demand > total abort, subtract (memory RMW) | one chunk subtraction | 14 |
| `rob().complete_pending_read_local(op_id)`: bit build, `pending & bit` abort, clear (memory RMW) | one chunk mask clear | 8 |
| `op.state.store(Done, release)` | needed | 2 |
Per chunk: `notify_sender`, lane head/count, `discard_tombstone_heads` (#2), `owner_turn_pending` (`fairlane_owner_debt_pending` = 9 loads + `notified_task_depth_capped`).

## Redundancy totals per GET (steady state, dynamic estimate)

| quantity | times computed/loaded | needed | est. waste (instr) |
|---|---|---|---|
| `Op&` from `(client, op_id)` (`Rob::at`) | 7 | 1 | ~42 |
| `FlatStore&` from `op.shard` | 3 | 1 | ~8 |
| publication word acquire load (+ ptr/extended/null checks) | 6 | 3 (pre-probe, post-probe, post-copy) | ~18 |
| table topology snapshot (7 loads + compare) | 2 | 1 | ~16 |
| `mix64(hash)` + home slot | 2 | 1 | ~12 |
| `read_local_mget(op)` (spec load + mask) | 5 | 0 (one bit at gather) | ~25 |
| `object->flags` / `type` / `enc` loads | 3 / 2 / 4 | 1 each | ~14 |
| `key_ptr + klen` derivation (KeyExt/HasTtl offset math) | 3 | 1 | ~12 |
| `read_local_enabled_` / eligibility re-tests | 3 / 2 | 1 / 1 | ~6 |
| `Task` copies (32 B) | 3 | 0 (16-B lane entry, no batch copy) | ~12 |
| lane-room predicate | 2 | 1 | ~8 |
| `pending_read_local` dispatch/flush reload | per op | per chunk | ~7 |
| abort checks duplicating an invariant already established this rotation | ~12 | ~3 | ~20 |
| `read_local_clear_reply` before first write | 1 | 0 | ~6 |
| `read_local_owner_conflicts_before` out-of-line call | per op | per chunk (owner map empty) | ~10 |
| per-op stats / demand / pending-bit RMW | 6 RMW | 3 RMW per chunk | ~25 |
| out-of-line call/return overhead (enqueue, prefetch, capture, prepare) | 4 | 0–1 | ~24 |
| **total** | | | **~265 instr/op** (of ~930 in the five symbols) |

The remaining ~660 are the parser handoff, the real probe (slot load + tag + key memcmp), the reply bytes
(`$len\r\n<value>\r\n` into direct/reply), the three publication-word validations, the QSBR-safe capture, the
`Done` publish, and the per-chunk scheduling — none of which is redundant.

## Cuts (each its own commit on `t-fable-localfuse`)

1. **Parser-inlined enqueue.** Lane entries shrink from `Task` (32 B) to `{Client*, op_id}` (16 B; lane 32 KB -> 16 KB);
   `enqueue_local_read(Client*, op_id, demand)` is `always_inline`, takes the parser's already-known demand and skips the
   second room check (the parser's check at 4080/4313 is the gate; only demand *releases* can intervene). The demotion
   callback takes `const uint64_t* op_ids` instead of `const Task*` (it only ever read `.op_id`).
2. **One per-op record through the drain.** Gather resolves `Op*`, `FlatStore*`, `is_mget` once (SoA arrays on the chunk
   stack), hoists the ROB dispatch/flush snapshot per chunk, and completion is chunk-aggregated: one pending-mask clear
   (`Rob::complete_pending_read_local_mask`), one demand subtraction, one `hits`/`keyspace_hits`/`keyspace_misses` add.
   The owner-conflict recheck runs per op only when the client's owner map is non-empty at chunk start.
3. **I0 -> C0 -> E0 carry.** I0 (`read_local_prefetch_cursor`) records `{state, slots0, mask0, mix32}` after its
   existing validated topology snapshot; C0 (`read_local_capture_from_cursor`) revalidates with one publication-word
   compare and probes from the carried home slot — no second topology snapshot, no second `mix64`, no redundant object
   prefetch — and the capture record carries `flags/type/enc/value` computed during the key compare (the 7 padding bytes
   and the never-dereferenced `slot` pointer pay for it, so the record stays 32 B). E0 executes from the record: no
   header re-derivation, `read_local_clear_reply` only on retry, validation is one load + compare. Any deviation
   (rehashing, ineligible word, generation moved, store disabled) falls back to the unchanged full
   `read_local_prefetch_capture`, so every result is still "valid under a publication word observed before and after the
   probe".

## Semantics preserved (and how)
- **Demote-not-stall**: the lane, tombstones, `lane_fallbacks`, `preserve_local_read_fallback`, `compact_local_read_tombstones`
  and the demotion plan are unchanged except for the entry type; suffix probing after a fallback still classifies each
  entry's own reason.
- **ROB reply ordering**: completion still publishes `Done` per op in lane order and only for the local prefix.
- **Capture-prefetch**: E0 never touches the slot; the capture record no longer even carries the slot address.
- **Table-generation validation**: three publication-word observations per op (before the topology is used, after the
  probe, after the copy), identical to today; the I0 observation now *is* the pre-probe observation when the word is
  unchanged at C0 — exactly the same reasoning the existing post-probe check already relies on.
- **Per-shard atomic-pending gate**: unchanged (P0 gate plus the pending bit in every observed word).
- **QSBR**: all records are stack locals of one bounded drain inside the rotation boundary.
- **Read purity**: no new retry/spin; the capture path still breaks (never loops) on churn.
- **`--read-local 0`**: the armed code is behind the same `read_local_enabled` constants; the baseline parser and
  `Rob::acquire` are untouched.
- **Knobs**: `--read-local-interleave` (YieldToOwner template) and `--read-local-prefetch-capture 0`
  (`CapturePrefetch=false`: hint prefetch + execute-time `read_local_probe`) keep their own instantiations.

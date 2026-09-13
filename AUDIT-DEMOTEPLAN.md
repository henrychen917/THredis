# AUDIT — `ReadLocalDemotionPlan::prepare` per-write cost (1s + read-local)

Base: 775aeea48. Evidence: perf instructions, 1:1 GET/SET p32 32c armed, ~4055 instr/op.
`prepare` = 5.7% (~230 instr/op), `read_local_command_touches_hash` = 2.6% (~105 instr/op),
`Rob<64>::acquire_read_local` = 3.6%. `touches_hash` is reached from `prepare` on this workload
(its other callers are keyset/MSET or MGET-owner paths), so the plan costs ~335 instr/op, i.e.
~670 instr per WRITE at 1:1. The task's "460/write" is the `prepare` symbol alone.

## 1. What a write does today (no conflict, N local reads still pending)

Call sites, all inside `IoLoop::parse_and_dispatch` (src/core/io_loop.h):

| site | command class | selection mode passed to `prepare` |
|---|---|---|
| io_loop.h:4187 | ordinary point write (SET) | `hash`, `require_hash_match = hash_precise` |
| io_loop.h:4204 | precise MSET (≤16 keys, ring room) | `intersect_command = op` |
| io_loop.h:4211 | MSET not precise | none → demote ALL pending |
| io_loop.h:4330 | MGET that fell back to owner | `intersect_command = op` |
| io_loop.h:4340 | point read that fell back to owner | `hash`, `require_hash_match = true` |
| io_loop.h:4359 | other non-write, non-ConnLocal | `require_hash_match = point_route` |
| io_loop.h:3826 | EX-side batch fallback | `fallback_tasks` (by op id) |

`prepare` (io_loop.h:3504-3665) on the SET path, step by step:

1. Guards (3514-3518), then the only pre-check that exists today, io_loop.h:3519-3522:
   `if (!rob.has_pending_read_local() && !(reserve_current_without_reads && reserve_shard >= 0)) return true;`
   `has_pending_read_local()` is `read_local_pending_slots_ != 0` (rob.h:156). At p32 1:1 the
   parser fills up to `kGenthreadIfidBatchOps = 32` frames per pass (genthread_pipeline.h:11) and
   local reads stay pending until the EX phase drains the lane (ex_loop.h:1283 `drain_local_reads_bounded_impl`
   → `complete_pending_read_local` at ex_loop.h:1423), so at the k-th write of a pass roughly k/2
   GETs are pending. The bit test is almost always TRUE on this workload: the early-out does not fire.
2. **Heap allocation per write**, io_loop.h:3523: `storage_.reset(new (std::nothrow) Storage)`.
   `Storage` (io_loop.h:3488-3496) = `ids[64]` + `scatter[64]` + `scatter_tasks[64]` + `kinds[64]` +
   `reasons[64]` + `owners[128]` + `remaining[128]` = **2304 bytes**. It is freed again at 3586
   (`storage_.reset()`) when nothing was selected. malloc+free ≈ 120-160 instr, before any scan.
3. **ROB bitmap walk**, io_loop.h:3525 → rob.h:158-181 `collect_pending_read_local(0, false, ...)`:
   two frontier loads, then ctz/blsr per set bit, emitting every pending id in ROB order. ~8N.
4. `bool selected[64] = {}` (3528) and a `reasons[i] = reason` fill (3529): ~N + const.
5. **Per pending read key compare**, io_loop.h:3550-3554: for each id, `rob.at(id)` (chunk pointer +
   index, rob.h:391-395) then `read_local_op_touches_hash` (io_loop.h:3473) →
   `read_local_command_touches_hash` (read_local.h:37-45). That function, for a plain GET, does:
   `read_local_command_is_mget` (spec load, flag mask) THEN `read_local_command_is_precise_mset`
   (read_local.h:30-32) = `op.cmd_name().eq_icase("mset")` — an argv[0] Slice load plus a
   case-insensitive compare that fails on length — and only then `op.hash == hash`. It is an
   out-of-line call (it shows as its own perf symbol). ~25-30N.
6. **Transitive-closure loop runs even when nothing was selected**, io_loop.h:3555-3574:
   `if (count_ && selective) { do { for i in 0..N: if (selected[i]) continue; for prior in 0..N:
   if (!selected[prior] || ...) continue; ...` With `selected[]` all false the inner loop still
   iterates N times per outer i → **N² iterations of load/test/branch** (~4N²) to discover that
   nothing changed. This is pure waste on the no-conflict path.
7. Compaction (3575-3582): ~3N. Then `count_ == 0` → `storage_.reset()` (free) and return true.

Cost model: `~10 + ~140 (malloc/free) + 8N + N + 28N + 4N² + 3N`. N=5 → ~500; N=7 → ~650;
N=8 → ~740. The measured ~670/write (prepare + touches_hash) matches N≈7, i.e. an average of
~7 GETs pending when each SET is parsed, which is exactly what a 32-deep 1:1 pipeline produces.

Why the common write pays it: same-key overlap between a write and this connection's still-pending
local reads is 0.0015% of reads at 1M keys, but `prepare` has no cheaper witness than the full walk.

## 2. Related per-op overhead observed on the way

- `read_local_command_touches_hash` (read_local.h:37-45): the string compare for MSET is evaluated
  for every non-MGET op. MSET is the only registry row with `key_step == 2` among precise-write
  candidates (t_string.cc:1473: `{"MSET", 3, -1, Write|MultiShard, cmd_xshard_only, 1, -1, 2}`), so a
  `spec->key_step != 2` test proves "not MSET" without touching argv.
- `Rob::acquire_read_local` (rob.h:112-125): per op it re-derives `read_local_try_deactivate`
  after `read_local_resolve_pending` already established the ring state, and `full()` reloads
  dispatch/flush that resolve just loaded. Small (<10 instr/op). Most of the 3.6% is the
  `Op::reset` that plain `acquire` also pays plus the once-per-write ring append/prune; no
  structural waste found worth a change in this lane.
- Layout: `Rob<64>` is 192 bytes with `flush_` at 128 followed by three 8-byte words
  (rob.h:534-537) → 160 used, **32 bytes of trailing padding** on the flush_ line. Client
  (conn.h:782) holds the Rob by value; no Rob reset path exists (the bitmaps live for the Client).

## 3. Design chosen (see commit 2)

A 256-bit per-connection superset filter of every key hash a still-pending local read may touch,
stored in the Rob's 32 spare bytes. OR-ed when a read is marked pending (`mark_current_read_local`,
the single call site io_loop.h:4964, with the op's own key hash(es)); cleared when
`read_local_pending_slots_` drops to zero; rebuilt exactly whenever the full plan does walk the
pending set. `prepare` consults it before allocating or walking: a miss proves no pending read shares
the write's hash (or any key of a precise MSET / owner MGET), so the common write returns after
~10 instructions; a hit runs today's plan unchanged in its outcome. Also: no allocation until a read
is actually selected, and the closure loop is skipped when nothing was selected.

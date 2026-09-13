# w-atomlat: shorter atomic-group latency

## Goal

The 9:1 MGET/MSET-8 cell is limited by the write group's last-owner path, not by the
read-local hit rate.  This lane removes shared RMWs and redundant owner hops while keeping the
same visibility rule: candidates are immutable and invisible at epoch zero, then the complete
group becomes visible through one ordered 0-to-ticket publication.

## Changes, in landing order

1. **Per-thread apply accounting.**  Replace the server-global apply count with padded monotonic
   opened/closed counters.  Admission writes the admitting IO's slot and the winning completion
   writes its executor's slot.  Cold drains read every `closed` counter before every `opened`
   counter, so a racing scan can conservatively over-count but cannot miss an apply.  The existing
   per-group `apply_open` flag remains the idempotent close claim.

2. **Executor-pass commit batching.**  A pass retains successful last-owner completions, reserves
   their consecutive tickets with one `fetch_add(k)` inside one commit bracket, stores every group
   epoch in completion order, and advances the safe watermark once.  Each group still has its own
   ticket and AOF record.  A pass with one completion follows the same sequence as today; cold or
   retry entries that have no pass collector commit as a batch of one.

3. **Pre-counted record references.**  Each participating owner starts an install wave with one
   sentinel node and contributes one pre-counted `record_refs` reference.  Installed records add
   owner-local nodes; owner completion removes the sentinel, while cleanup removes record nodes.
   Whichever removal reaches zero releases exactly one group reference.  This also initializes
   script APPLY waves and explicitly releases an undispatched wave before teardown.

4. **Separated decision line.**  Put `epoch` and `aborted`, the words read while resolving an
   immutable chain, on their own cache line.  Completion counters and lifetime references occupy
   a different line.  This is arena-only layout; the compile-time MGET-8-in-16-KiB bound remains.

5. **Executor-major posting.**  When several shard groups currently belong to one executor, post
   one task naming that owner's arena chain.  The executor visits each shard serially, preserving
   every per-shard snapshot, notification, AOF, blocking, and placement hook, then contributes one
   owner completion.  One-shard-per-executor placements retain the old shape.  Execution validates
   ownership before dereferencing a store; a placement change forwards/splits work at shard
   boundaries instead of permitting a non-owner access.

6. **Single-owner fallback MGET.**  An owner-path MGET whose distinct shards all map to one
   executor is posted as one ordinary owner batch, with no `ScatterState`, shard groups, value
   slots, pending counter, or gather.  A compact IO-owned token pins the command's already-stamped
   snapshot and expiry cuts until completion; the executor binds that same cut to every touched
   store and emits RESP elements directly in argv order.  If placement changes, the token carries
   the cursor across single-writer task handoffs.  This arm is reached only after the B+ read-local
   attempt falls back; multi-owner MGET continues through scatter/gather.

## Laws and risk boundary

All store mutations remain on the shard's sole current owner; there is no shared-writer index.
Readers neither lock nor retry, and read-local still forces immutable replacement while armed.
No numeric knob is added.  `Op`, `Client`, `ThreadCtx`, `Shard`, `FlatStore`, `Rob<64>`,
`AtomicEntry`, and `Config` remain layout-locked.  The main risks are completion lifetime during
abort/teardown, preserving program-order read cuts, and shard migration between routing and
execution; the designs above make those states explicit rather than relaxing their invariants.

## Measurement

Run the coordinator's quoted mm11/mm91/mm991 atomic-1 cells, the full atomic gauntlet including
`tests/atomic_hazards.py` and `tests/bplus.py`, and the s6 differ.  The intended result is higher
mm11 throughput with mm91/mm991, ordinary commands, protocol replies, persistence, and migration
unchanged.

## Landed scope (rebase onto f40469ea3)

Changes 1-4 are landed. Changes 5 and 6 are NOT:

- **5, executor-major posting** existed only as an unverified WIP commit
  (eb116399c) written against the pre-train `ScatterState` and dropped here. It
  added `ShardGroup::task_next/task_head/task_done`, a `task_count` chain plan
  applied at every publication point, `xshard_task_repartition()` and an
  `execute_executor_major()` fragment loop in `ExLoopT`. It also carried an
  unrelated widening of `for_each_task_key()`'s whole-owner overlap answer.
  Nothing in it was measured.
- **6, single-owner fallback MGET** was never implemented and is rejected: it
  optimizes a shape that is not the target regime.

Two defects were repaired on the way in.

- `script_post_phase()` initialized APPLY's owner completion twice, because
  mainline's 9b5a4be82 had independently added the same call. Change 3 makes
  that call publish the wave's pre-counted sentinels, so the second one aborted.
- Change 3 derived a transaction's per-executor sentinels from
  `MultiExecState::write_keys`. That list is the prepare-time registry key
  range, and a lowered LMPOP/ZMPOP has none, so the key it selects at phase two
  installed against an executor with no pin and aborted the server
  (`tests/multi_exec.py`, heterogeneous_ryow). Sentinels are now derived from
  `state.shards`, the outer task set actually published, which is exactly the
  set of executors that can install.

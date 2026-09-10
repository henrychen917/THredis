# WHAT cx-final ALREADY HAS THAT EVERY UNMERGED BRANCH'S BASE DOES NOT
# Every remaining branch was cut from c8e61f646. cx-final has moved a long way since.
# A merge that resolves toward the branch side in these files SILENTLY REVERTS this work.

## Merges already in cx-final
  bebfde299 test: repair netcmd topology and configuration fixtures
  7c6e0024c EXPECT 383/400 after the resize batteries
  67f623716 merge cx-resizefix
  bf41dd331 resize progress without reader retirement waits
  818d458ef Merge cx-waits and add resize and retirement liveness batteries
  e25e08f57 EXPECT 377/394 for the eight-branch merge
  a6f1950ab Record remaining merges, validation rates, and gate arithmetic
  8a7260059 Resolve CONFIG REWRITE alias preservation after netcmd merge
  821d207b1 Merge cx-fix-netcmd preserving Lua and sender contracts
  83e9c8258 Merge cx-fix-atomics with current configuration contracts
  878af001a Merge cx-fix-storage with current read-local fixture contract
  2587f70e6 EXPECT 335/352 for the merged tree
  112ed6679 Merge cx-integ preserving deterministic windows and concurrency guards
  6647031dd merge cx-fix-concurrency
  1d8a25d3c merge cx-vestcut
  12458e0a5 merge cx-windowfix
  b4013b752 Merge cx-rl2s and connect local reads to both overlap schedules
  4938c609c Merge cx-overlap with its working-tree gate fixes

## The specific changes that must survive any merge
  * tests/atomicwindow.py — a bounded re-arm using the ATOMIC-FANOUT-DEFER hook that made a 20%-flaky
    row deterministic (verified 15/15). Losing this makes every later gate unreadable.
  * tests/atomic_torn.py — the OFF torn-RENAME control re-rolls up to 4 times and does NOT degrade to a
    skip, unlike the three controls beside it.
  * tests/multirace.py — ARM_ATTEMPTS re-arming on fresh connections; must not become a skip.
  * src/store/flatstore.h — reads no longer call rehash_step(); resize progress comes from owner
    maintenance plus non-blocking retirement. Four gate rows assert this. Reverting it restores an
    unbounded read-path wait.
  * src/core/server.h — read_local_enabled() is `thread_mode == Fused && read_local != 0` PLUS the split
    path; the `overlap == 0` clause was deliberately removed so the lane and overlap coexist.
  * src/core/ex_loop.h — the reorder batch-array capacity fix (an out-of-bounds write at n=128 into
    arrays sized 32); the vestigial deletions; the split shard-less reader pass.
  * src/core/config.h — the final knob surface: --overlap and --reorder restored as user knobs,
    --key-lb/--client-lb split back apart, --shard-home restored. 28 deleted knobs must STAY deleted.
  * src/cmd/t_server.cc — per-thread read-local activation reporting in INFO.
  * tests/gate.sh — EXPECT 383/400 and the row set behind it, including the resize batteries.

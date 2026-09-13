# MULTI two-hop lowering notes

## Scope and invariants

- The failing shape is a two-key write whose keys resolve to different shard owners.  The fix must
  stay inside the existing MULTI + scatter lowering: phase-one reads and phase-two installs execute
  only on each key's owner.
- The transaction's MVCC commit ticket, parent abort decision, and `now_cut_ms` expiry cut remain
  transaction-wide.  A child-local abort is only a private cancellation for that command's MVCC
  candidates; runtime command outcomes must not be confused with the parent transaction abort word.
- Validation has to use `--shards 16 --ratio 6:2` in both atomic modes and discover, with
  `DEBUG SHARD`, rather than assume, a pair of names with different owners.  Failure to discover a
  pair is a test failure.

## Redis 7.4.2 `TIME` result

The checkout at `/home/user/Projects/redis` currently describes itself as 7.4.10, but it contains
the `7.4.2` tag.  I inspected that tag directly with `git show 7.4.2:...`; no server was started.

Redis freezes `TIME` across a `MULTI`/`EXEC`, including when a slow queued command sits between two
`TIME` commands:

1. The top-level `call()` for `EXEC` invokes `enterExecutionUnit(1, call_timer)`.
2. `enterExecutionUnit` updates `server.ustime`, `server.mstime`, `server.unixtime`, and
   `server.cmd_time_snapshot` only when `server.execution_nesting++ == 0`.
3. `execCommand` invokes `call()` for each queued command while that nesting level is nonzero, so
   none of the nested calls refreshes the cached clock.
4. `timeCommand` replies from `server.unixtime` and `server.ustime`.

Therefore the repeated values TomoKV returns are reference-compatible and are a consequence of the
same logical-operation clock cut that expiry needs.  No TIME-specific live-clock escape is wanted,
and the pinned expiry cut must remain unchanged.

## Initial lowering diagnosis

`prepare_commands` already treats `xshard_prepare` validation failures as prebuilt per-command
reply elements.  The observed `EXECABORT` is later: two-hop child completion currently promotes any
child `ScatterState::aborted` bit to `MultiExecState::aborted`.

- `RENAME` with a missing source sets the child reply to `NoSuchKey` and uses its child abort bit to
  keep the destination task/install from proceeding.  That is a command outcome, not a reason to
  discard the containing transaction.
- `RENAMENX` and non-REPLACE `COPY` use the child abort bit when the destination existence
  precondition fails.  Their final reply is integer zero.  Promoting that private cancellation to
  the parent is what turns the normal zero into `EXECABORT`.

The child abort bit cannot simply be ignored: for `RENAMENX`, phase-two source and destination work
can run on distinct owners, so a source tombstone installed before the destination rejects NX must
remain invisible.  The implementation therefore keeps child MVCC records on the child's own
epoch/abort words.  The EXEC finalizer publishes the parent epoch and every child epoch with one
identical reserved ticket before advancing the safe read watermark.  A failed child keeps its
private candidates hidden through its own abort word; a genuine transaction abort marks every
child aborted.  The MVCC resolver and single-owner execution path are unchanged.

The MULTI AOF fragment must likewise describe the logical transaction overlay rather than the raw
physical table head.  A rejected `RENAMENX` can leave an aborted source tombstone at that head until
MVCC cleanup.  The fragment now resolves with the transaction connection's read context, so it
skips the failed child's candidate while retaining successful private candidates, then records
that visible post-image under the parent's existing AOF group decision.

## Directed coverage and required run geometry

`tests/expwide.py` now converts both former live notes into assertions.  On TomoKV it walks up to
8,000 destination candidates and buckets each with `DEBUG SHARD`; the selected destination must
have a different result from the source or the battery raises instead of passing vacuously.  The
covered split pair is reused for:

- `RENAMENX` and `COPY` inside MULTI with a live destination (integer 0), plus the existing
  deadline-straddle and elapsed controls;
- `RENAME` inside MULTI with a physically missing source, bracketed by successful SETs, expecting
  `[OK, ERR no such key, OK]` and both SET effects;
- bare `RENAMENX`, `COPY`, and missing-source `RENAME`, including destination postconditions.

Per the lane contract, the main session must run this battery against servers booted exactly with
`--shards 16 --ratio 6:2 --enable-debug-command yes`, once with `--atomic 0` and once with
`--atomic 1`.  This lane did not build, boot a server, or run the battery.

# KNOBS mainline integration notes

## Integration point

- The original lane commit `b0754e02e` was based on `47c694ada`.
- It was rebased onto `00fc7e34c` (`t-merge14`) as `82159ef99`.
- `t-merge14` contains 90 commits after the old base and is an ancestor of `t-knobs`.
- No build, server, test, load generator, or benchmark was run in this integration lane, per
  `LANE_RULES.md`. Validation here was source, diff, range-diff, and ancestry inspection only.

## Cron conflict resolution

`src/core/io_loop.h` was kept in its evolved epoll form. In particular, `run_loop` remains the
three-parameter `run_loop<HasUnix, HasTls, kEp>` specialization at line 257, the retire and ready
passes remain `collect_retire_work<HasUnix, kEp>()` and `flush_ready<HasTls, kEp>()` at lines
327-328, and the park and deferred-close paths retain their `if constexpr (kEp)` branches.

The knobs delta was then ported onto that structure. The old combined `cron_armed` local is now the
separate `client_cron_armed` / `save_cron_armed` pair at lines 278-279. The client-cron transition
still initializes interaction clocks on the rising edge and calls `stop_obuf_tracking()` on the
falling edge at lines 283-293. Inside the existing `{ Span busy(sig.busy_ns); ... }` scope, after
the epoll-templated retire and ready passes, the client beat remains at lines 329-333 and the live
save beat calls `srv_->save_cron_pass(*self_, ring_)` at lines 334-337. Its next deadline is still
set to `cached_now_ms_ + 1000`.

The closing brace after the save block at line 339 closes only the busy-span scope. The function
continues with CPU accounting, the epoll/uring park, shutdown deferred-close draining, and writer
shutdown. This avoids both prior bad resolutions: the save call is live, and no post-cron loop code
is placed at class scope.

## Mainline preservation audit

The full `47c694ada..t-merge14` log was read. Since `t-knobs` is based directly on `t-merge14`, all
90 commits remain in its ancestry. The final code delta versus `t-merge14` is restricted to the
original 14 knobs files (398 insertions, 69 deletions), plus this notes file. `git range-diff`
reports only these integration
adaptations:

- the two epoll template arguments in the cron conflict region;
- evolved context following the cold tail of `Shard`;
- mainline's current boot-only description of `dir` / `dbfilename`.

The named high-risk mainline changes were also checked at their resulting source locations:

- epoll selection and specialization: `src/core/io_loop.h:223-236,257,313,327-328,357-377`;
- epoll deferred connection closes: `src/core/io_loop.h:1771-1778,2040-2046,2392`;
- OOB frame-boundary and prior-reply deferral: `src/net/conn.h:458-474`,
  `src/net/wb.h:110-151,625-637,885-888`, `src/core/pubsub.inc:225-237`, and
  `src/cmd/climon.cc:316-326`;
- cross-owner SORT gather and owner grouping: `src/cmd/scatter_engine.inc:846-899` plus its gather
  execution branches, with the integration audit retained in `NOTES-SORTXSHARD.md`;
- MULTI two-hop child stage membership publication: `src/cmd/multi.inc:1679-1727` and the immutable
  group membership maintained by `src/cmd/scatter_engine.inc`.
- the later connection parse-barrier owner-bit work: `BarrierOwner` at `src/net/conn.h:84-105`,
  owner-scoped acquire/release at `src/net/conn.h:550-571`, and the single counted arm door at
  `src/core/io_loop.h:1740-1750`; `NOTES-BARRIER.md` and all six barrier-lane commits are retained.

The knobs behavior itself remains present: the Redis-compatible, length-aware memory parser is in
`src/core/config.h`; `CONFIG SET` uses it through `parse_bytes` in `src/cmd/t_server.cc`; the flat
save schedule parser/stringifier and default clauses remain in `src/core/config.h`; mutation
counting remains owner-local in `src/core/shard.h`; and successful non-rewrite snapshots publish
their save-change cut in `src/snapshot/snapshot.cc`.

## Measurement surface

Commands and surfaces this lane can touch:

- boot/config-file parsing for `maxmemory`, `save`, `databases`, and `proto-max-bulk-len`;
- `CONFIG GET`, `CONFIG SET`, and `CONFIG REWRITE` for those knobs;
- request parsing and receive-buffer growth when `proto-max-bulk-len` differs from its default;
- successful mutating commands while the save schedule is armed (including `SET`, `MSET`, deletes,
  collection mutations, expiry mutations, and `FLUSHDB` / `FLUSHALL`) through owner-local save
  change accounting;
- periodic snapshot start/completion and the persistence section of `INFO`;
- ordinary client-cron consumers (`timeout`, output-buffer limits, and tracking) only at their
  arm/disarm edge, whose prior behavior must remain unchanged.

For runtime validation by the main session, suffix and `CONFIG GET save` parity need one client and
one server. Save-cron validation needs at least one IO owner and one executor; a multi-shard cell
should confirm that mutations on every shard contribute to the process-wide threshold and that only
the designated first IO owner starts a snapshot. The existing pubsub and EXEC regressions must use
their shipped geometries; cross-owner SORT/MULTI coverage must discover owner-separated keys by
bucketing `DEBUG SHARD` results and fail loudly if that geometry cannot be found.

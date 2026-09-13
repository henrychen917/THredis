# t-zcinfo lane notes

## Premise and measurement surface

The premise is true on the as-found tree. `WbEngine::Stats` contains live, IO-thread-owned
measurements at `src/net/wb.h:569`. The submission paths at `src/net/wb.h:273` and `:316` advance
`zc_sends` only when `build_segment_iov` reports a borrowed iovec, and `WbEngine::release` at
`src/net/wb.h:834-836` advances `zc_releases` when a borrowed segment is returned. Before this lane,
shutdown accounting summed the structs in `src/main.cc:394-410`, but `INFO stats` did not.

The INFO surface added here is:

- `zc_sends`: sendmsg submissions whose iovecs contain at least one borrowed segment.
- `zc_releases`: borrowed segments returned after completion or teardown.
- `sends_submitted`, `short_writes`, `bytes_sent`, `peer_aborts`, and `send_errors`: all five named
  sibling candidates have real increment sites in `src/net/wb.h`, so they are exported too. No
  placeholder or derived-zero rows were added.

Each `IoLoop` publishes its existing `WbEngine` at initialization (`src/core/io_loop.h:84`) through
a cold-tail pointer on its `ThreadCtx` (`src/core/thread.h:283-291,443`). `cmd_info` walks the server's
threads and sums the actual engine structs at `src/cmd/t_server.cc:1511-1520`, beside the existing
`LoopSignals` summation for `net_input_bytes` / `net_output_bytes`. The INFO rows are emitted at
`src/cmd/t_server.cc:1646-1648`. The pointer publication is atomic; the counters remain plain,
single-writer values and receive no new hot-path increments. Executor threads publish no engine
because their retained `WbEngine` never sends.

## Commands and measurement scope

The only server command whose behavior or output changes is `INFO` (`INFO stats`, plus aliases that
include Stats). GET, SET, MGET, and MSET execution and reply paths are unchanged. The gate-only
change at `tests/gate.sh:160` adds the already-landed `tests/pushtear.py` battery under both atomic
feature boots.

For the main-session A/B, the code change can touch only INFO latency/bytes and startup-size cold
state; it adds no send-path branch or counter write. Headline GET/SET/MGET/MSET measurements should
therefore be unchanged, but the owner gate still requires their normal zero-regression cells.

## Gate ledger arithmetic

The committed ledger says 209 quick / 219 full. That is already short by two: the merged feature
loop contains both `contarity` and `infofix`, while the comment and totals account only for the
`207 -> 209` contarity addition. `infofix` is another battery under atomic 0 and atomic 1, so the
correct pre-lane totals are 211 quick / 221 full. Adding `pushtear` to that same loop contributes two
more checks, making the correct totals **213 quick / 223 full**.

## Validation to run in the main session (not run in this lane)

Boot a normal multi-thread server with `--enable-debug-command yes`. Use one plain TCP connection
for the data commands and fully consume every reply; use an admin connection to sample `INFO stats`.
Set `zc-min` low enough that a non-integer string is above it, write that value, sample INFO, GET it,
consume the complete bulk reply, and sample INFO again. `zc_sends` must increase; after completion,
`zc_releases` must also increase. This proves the borrow path fired and its lifetime closed.

Then set `zc-min` above a second non-integer value's length, write that smaller value, bracket a GET
with the same INFO samples, and require `zc_sends` not to increase. That negative control is the
non-vacuity proof: the counter distinguishes borrowed from copied GETs instead of moving for every
reply. For example, `zc-min=64` with a 256-byte value is the positive arm, and `zc-min=16384` with a
64-byte value is the negative arm.

Finally run the feature gate in the main session. Its geometry is the existing 16-shard feature
boot with the configured IO/executor split, once with atomic 0 and once with atomic 1. `pushtear`
uses separate connections for the target and out-of-band producers, finds cross-shard MGET keys via
`DEBUG SHARD`, and fails loudly if its segmented/deferred or borrow counters do not move.

No build, server, test, load generator, or benchmark was run in this lane, per `LANE_RULES.md`.

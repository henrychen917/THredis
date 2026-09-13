# Connection limits

This lane implements `maxclients`, `timeout`, `tcp-keepalive`, `tcp-backlog`, and
`client-output-buffer-limit` with Redis-compatible defaults and configuration grammar.

## Behavioral notes

- `maxclients` is enforced immediately after `accept(2)` and before `Client` allocation. Each
  `SO_REUSEPORT` acceptor uses the shared live count, so a simultaneous burst can overshoot the
  configured value by at most the number of IO threads. Boot reserves 32 descriptors plus two per
  IO thread, attempts to raise `RLIMIT_NOFILE`, and otherwise lowers the effective value.
- `timeout` runs on a 100 ms client-cron beat, visits at least five clients or one tenth of the
  connection table per beat, and closes normal clients only when whole-second idle time is strictly
  greater than the configured value. Blocked and RESP2 pub/sub clients are exempt; MULTI is not.
- `tcp-keepalive` applies to newly accepted TCP clients. A nonzero value enables `SO_KEEPALIVE`,
  sets `TCP_KEEPIDLE` to the knob, `TCP_KEEPINTVL` to `max(1, knob / 3)`, and `TCP_KEEPCNT` to 3.
- `tcp-backlog` is boot-only and is passed to every TCP and unix `listen(2)` call. Linux may cap it
  at `somaxconn`, in which case startup emits the Redis advisory warning.
- Output limits enforce the `normal` and `pubsub` classes. The `slave`/`replica` class parses,
  stores, merges, and serializes as `slave`, but is inert because TomoKV has no replica clients.
  Unlike Redis's fixed reply-buffer accounting, TomoKV counts all retained output in its growable
  fill/send buffers and queued reply segments, including zero-copy `Borrow` segments.

## Validation numbers

- Quick gate: 17 passed, 0 failed; idle CPU 0 jiffies/5 s; direct replies 10,396;
  dispatched/executed 53,684/53,684.
- Pipeline-32 SET disabled-path check, server-thread `instructions:u` divided by the matching server
  command-counter interval: base 2866.81 instructions/op, limits 2868.13, delta +1.32 (bar <= +2).
- Backlog A/B, 2,048 simultaneous connections: 511 = 137,179.50 ops/s and 16,384 = 137,595.85
  ops/s (+0.3%, both accepted 2,048 with zero accept errors), so the Redis default 511 remains.
- `tests/limits.py`: release and ASAN pass, including grammar rejection atomicity, admission/release,
  timeout exemptions, continuous soft windows, pub/sub classing, and borrowed-segment hard limits.
  Its 200-way `maxclients=20` storm admitted 19 clients beside the admin and rejected/counted 181.
- `multi_exec.py`, `blocking.py`, `pubsub.py`, and `lua_scripting.py`: pass from both atomic starting
  modes. `sizeof(Op) == 336` and `sizeof(Client) == 1984` remain locked by the builds.

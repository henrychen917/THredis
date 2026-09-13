# NOTES-BARRIERTEST — the barrier owner mask now has a discriminator

Lane `t-barriertest`. The mechanism in `t-barrier` was already merged; this lane adds only the
directed battery it described. No engine source, gate script, ledger constant, build output or
runtime result is changed here.

**Outcome:** `tests/barrier.py HOST PORT` implements the four-cell ownership battery. It refuses a
one-executor boot, refuses a shard/executor mismatch, discovers the `BLMOVE` pair with `DEBUG
SHARD`, puts each blocking command and its younger probe in one `send()` call, and requires exact
counter deltas. A run that never constructs a held release is a failure named
`geometry never constructed: barrier_releases_held did not move`.

Per `LANE_RULES.md`, I did not build, boot a server, run this battery, run another test, or publish
a validation result. The operator procedures below are instructions, not results.

---

## 1. Geometry and measurement surface

The battery preflights all load-bearing geometry before assigning any cell a pass:

| Requirement | Live proof |
|---|---|
| Debug commands enabled | `DEBUG BARRIER-HOLD 0` must return `OK`; every candidate lookup through `DEBUG SHARD` must return an integer |
| At least two executors | `INFO LB` must report `lb_ex_threads >= 2` |
| One shard per executor | `MEMORY STATS` `shards.count` must equal `INFO LB` `lb_ex_threads` |
| Cross-owner move | Walk up to 4096 boot-unique candidates, bucket by `DEBUG SHARD`, and choose two distinct buckets; no key pair is hard-coded |
| Same-client owners | One blocker connection takes both `BarrierOwner::Blocking` and injected `BarrierOwner::Debug`; the admin connection drives it |
| Same write | The blocking RESP frame and `ECHO` frame are concatenated and passed to one `socket.send()`; a short write is a geometry failure, not retried as a second write |
| Fired mechanism | `barrier_owner_overlaps` and `barrier_releases_held` must each move by exactly two in the armed cell, once for `BLPOP` and once for cross-owner `BLMOVE` |

The shard/executor equality assumes the default round-robin shard placement, exactly as
`NOTES-BARRIER.md` section 5 specifies. Do not supply `--shard-home`: `DEBUG SHARD` reports a shard,
not a dynamically overridden owner, so a custom placement would invalidate that oracle.

The admin connection is the required second connection: it deletes the unique test keys, observes
`blocked_clients`, wakes the blocker with `RPUSH`, reads both INFO counters, clears the latch, and
runs `CLIENT LIST`. At no point can a second client contribute an owner to the blocked client's
barrier; the two bits are acquired on the one blocker `Client`.

The armed cell covers both owner-scoped release sites:

* `BLPOP` retires through `blocking_retire()` (`src/cmd/blocking.inc`).
* A source/destination pair on different executor owners makes awakened `BLMOVE` convert its ROB
  slot into the two-hop scatter and retire through `blocking_scatter_retire()`.

Both arms use a fresh blocker because a correctly held connection cannot parse another command
until the resume cell clears its Debug bit.

---

## 2. The four cells

The script emits exactly one result line for each cell, followed by
`barrier: N ok, M FAIL` and a nonzero exit when `M != 0`.

1. **armed** — Sets `DEBUG BARRIER-HOLD 1` on each blocker, pipelines `BLPOP; ECHO` and cross-owner
   `BLMOVE; ECHO` in one write each, wakes the commands from the admin connection, validates both
   blocking replies, and requires both ECHOs to remain silent for 500 ms. The counter deltas must
   be exactly `barrier_owner_overlaps +2` and `barrier_releases_held +2`. A zero held-release delta
   gets the explicit non-vacuity failure above.
2. **negative control** — Repeats the same two scripts with the latch off. Both ECHOs must answer
   within the same 500 ms observation window, and both counters must remain flat. This proves the
   probe itself is readable and the armed silence was not caused by a dead socket or bad wake.
3. **resume** — Clears the latch from the admin connection, executes one `CLIENT LIST` fan-out, and
   requires both held ECHOs to answer. An ECHO that escaped in cell 1 is recorded and cannot be
   recycled into a false resume pass.
4. **production overlap** — With the latch off, runs the exact ordinary-traffic suite named by
   `NOTES-BARRIER.md` section 5: `blocking.py`, `blockmulti.py`, `multi_exec.py`, `climon.py`,
   `pubsub.py`, and `storeorder.py`. Their output is captured so this battery still emits one line
   for the cell; a nonzero child exit or a 300-second child timeout fails it. Collectively they
   drive all six production owner classes and their own non-vacuity controls. The per-cell deltas
   for both barrier counters must be zero. The overlap counter is cumulative and was deliberately
   moved by cell 1 on the same boot, so the live production assertion is its delta, not an
   impossible absolute zero after the positive-control arm.

The printed order is the order above. Internally, the negative control executes before the armed
cell. `DEBUG BARRIER-HOLD` is process-global: executing the negative cell second would require
clearing the latch and could release cell 1 before cell 3 had a chance to prove the explicit
`CLIENT LIST` wake. Results are retained and printed in the requested conceptual order.

All keys created directly by the armed/control probes carry a PID/time suffix and are deleted in
the final cleanup. Cleanup also clears the global latch and runs `CLIENT LIST`, even after a failing
control build, so the battery does not leave its own clients or debug state stranded. The ordinary
batteries retain their existing cleanup behavior.

---

## 3. Operator run

Use an otherwise idle server. One concrete four-core geometry is two I/O threads, two executors and
two shards:

```sh
taskset -c <four-cpu-range> ./build/tomokv \
  --port 7899 --bind 127.0.0.1 --ratio 2:2 --shards 2 \
  --enable-debug-command yes --protected-mode no
python3 tests/barrier.py 127.0.0.1 7899
```

The expected fixed-binary summary is:

```text
barrier: 4 ok, 0 FAIL
```

The individual lines also report the exact `+2/+2` armed deltas, the discovered `BLMOVE` shard
pair, the negative-control `+0/+0`, both resumed probes, and the production `+0/+0`. Treat a
preflight failure as a failed run; do not downgrade it to a single-owner control or a skip.

### Engine-level positive control

Do **not** use the pre-`t-barrier` parent binary: it lacks `DEBUG BARRIER-HOLD` and the two counters,
so the battery correctly refuses it before reaching the discriminator. Instead make a disposable
control worktree from the battery commit and restore only the old clear semantics while retaining
the injector and telemetry:

1. In `src/net/conn.h`, temporarily replace the body of
   `Client::barrier_release(BarrierOwner who)` with:

   ```cpp
   (void)who;
   barrier_owners_ = 0;
   return true;
   ```

   This is the pre-lane `set_scatter_barrier(false)` behavior expressed in the current mask API:
   the blocking retirement clears **all** bits, including the injected Debug owner, and reports
   that the connection was released.
2. Build that disposable worktree using the main session's normal build procedure.
3. Boot it with the exact geometry above and run `python3 tests/barrier.py 127.0.0.1 7899`.
4. Cell 1 **must FAIL**: one or both probes answer while armed, and
   `barrier_releases_held` stays flat, producing the named `geometry never constructed` failure.
   The script must exit nonzero. If the armed cell passes, the battery has not discriminated the
   hardening and must not be accepted.
5. Restore the real owner-scoped method, rebuild, start a fresh server with the same arguments,
   and require all four cells to pass.

The control edit is deliberately not committed in this lane.

---

## 4. Gate ledger suggestion, not a claim

`tests/gate.sh` is untouched. Its existing feature/debug boots use 16 shards with two executors on
the standard eight-core geometry, so they do **not** meet this battery's `shards == executors`
oracle requirement. The safe future wiring is a dedicated debug-enabled boot with
`--ratio 6:2 --shards 2`, running `tests/barrier.py` once under each atomic mode to retain the
gate's two-mode convention.

That would add two successful battery rows and therefore suggests:

```text
EXPECT_QUICK 218 -> 220
EXPECT_FULL  228 -> 230
```

Those are suggested deltas only. Do not change either constant until the operator actually adds
the two rows; this lane claims zero new gate rows.

---

## 5. Scope and files

| File | Change |
|---|---|
| `tests/barrier.py` | New four-cell raw-RESP barrier ownership battery |
| `NOTES-BARRIERTEST.md` | Geometry, cell semantics, positive control, and unclaimed ledger delta |

No production path can be touched by this diff. The battery, when an operator runs it, exercises
`DEBUG`, `INFO`, `MEMORY STATS`, `BLPOP`, `BLMOVE`, `LMOVE`, `WAIT`, `MULTI`/`EXEC`,
`SUBSCRIBE`/`UNSUBSCRIBE`, and `CLIENT LIST`. No throughput or latency measurement is owed for a
test-and-notes-only change; the relevant measurement surface is the two INFO counters plus exact
reply/silence behavior on the one blocked connection.

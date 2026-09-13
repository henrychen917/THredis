#!/usr/bin/env python3
"""Armed local-read LANE ADMISSION battery (P128.md).

Usage: read_local_lane.py HOST PORT
  boot: --thread-mode 1s --read-local 1 --enable-debug-command yes

The fused thread's local-read lane holds kInboxSlots (1024) entries. A connection may pipeline up
to kRobWindow (64) ops, so a thread that has accepted more than 16 deep-pipelining connections can
be asked for more local reads in one rotation than its lane holds. The shipped rule is: the excess
is DEFERRED (the frame stays at rpos and is re-parsed by a later pass of the same thread, where it
is still served locally) and, while the lane is under pressure, the lane is divided among the
thread's active connections so that no connection can crowd out the ones the rotation parses last.
The former rule demoted the excess to the shard owner as an ordinary task; 7/8 of those tasks were
cross-thread, and they were the seed of the p128 / 2048-connection pure-read collapse.

WHY THIS BATTERY DRIVES A CAP INSTEAD OF LOAD. Oversubscribing a 1024-entry lane by traffic is a
RATE RACE against the drain, and a test client cannot win it: the fused thread empties the lane
every rotation, far faster than one Python process can fill it. Piling on connections does not
help -- the gate measured 256 connections x 64 deep = 2624 pipelined frames producing ZERO lane
events, because the frames were never in flight at once. So the anti-vacuity checks below could
only fire on a saturated rig, and a row that fires only on its author's rig is not a gate row.

Instead, DEBUG READ-LOCAL-LANE-CAP lowers the ADMISSION threshold (the ring keeps its kInboxSlots
entries and its masking; only the parser's "has room" test moves). One connection pipelining more
than the cap in a single write then oversubscribes the lane INSIDE ONE PARSE PASS -- before any
drain can run -- so the mechanism fires deterministically with a handful of connections, on any
box, at any speed. The cap derives to kInboxSlots whenever it is unset, which is what production
always runs; phase 1 below measures the uncapped behaviour precisely so the capped result has a
control to be compared against.

Checks (all counter deltas over this battery's own traffic -- the vacuous-validation rule):
  control    with the cap unset, record the same traffic's lane deltas; the capped phase must
             produce strictly more, which is what proves the CAP is the reason it fires
  lane full  read_local_defer_lane_full  > 0   the lane WAS oversubscribed -- otherwise every
                                                other check here would be vacuous
  no demote  read_local_fallback_lane_full == 0  capacity never creates an owner task
  fair share read_local_defer_quota      > 0   the pressure window armed and divided the lane
  local      read_local_hits delta >= 95% of the shared GETs (the rest may fall back for reasons
             unrelated to capacity, e.g. sequence churn from the RYOW writes below)
  order      every connection receives every reply, in order, with the expected value; the SET
             that follows the deferred GETs on the same connection and the GET behind it prove
             frame order and read-your-own-write survive a deferral
  liveness   every round completes (a deferred frame must be re-parsed without an external wake)
  restore    setting the cap back to 0 restores the derived lane and the traffic still completes
             locally and in order -- the production path is not left altered by the test hook
"""
import time

import _lib

DEPTH = 64          # GETs per connection per round == kRobWindow
CONNS = 16          # small and deterministic: the cap, not the load, creates the pressure
ROUNDS = 6
CAP = 8             # effective lane capacity while the mechanism is under test
ROUND_DEADLINE_S = 20.0
COUNTERS = ("read_local_hits", "read_local_fallbacks", "read_local_fallback_lane_full",
            "read_local_defer_lane_full", "read_local_defer_quota")


def counters(conn):
    table = _lib.info(conn, "stats")
    out = {}
    for key in COUNTERS:
        if key not in table:
            raise AssertionError("INFO stats has no %s; the battery cannot prove its mechanism"
                                 % key)
        out[key] = int(table[key])
    return out


def burst_round(conns, shared, values, r):
    """One round: every connection pipelines DEPTH shared GETs + SET own-key + GET own-key in a
    single write, so a connection's whole window reaches the parser in one pass. Returns
    (ok, detail, elapsed)."""
    want = values + [b"OK", b"r%d" % r]
    t0 = time.monotonic()
    for i, c in enumerate(conns):
        payload = b"".join(_lib.encode("GET", k) for k in shared)
        payload += _lib.encode("SET", "rl:lane:own:%d" % i, "r%d" % r)
        payload += _lib.encode("GET", "rl:lane:own:%d" % i)
        c.sock.sendall(payload)
    for i, c in enumerate(conns):
        try:
            got = [c.read() for _ in range(DEPTH + 2)]
        except Exception as exc:   # a hang here is a deferred frame nobody re-parsed
            return False, "round %d conn %d: %r" % (r, i, exc), time.monotonic() - t0
        if got != want:
            bad = next(j for j in range(len(want)) if got[j] != want[j])
            return False, "round %d conn %d reply %d: got %r want %r" % (
                r, i, bad, got[bad], want[bad]), time.monotonic() - t0
    return True, "", time.monotonic() - t0


def drive(conns, shared, values, first_round, rounds):
    """rounds rounds of burst traffic. Returns (ok, detail, slowest, shared_gets)."""
    slow = 0.0
    for r in range(first_round, first_round + rounds):
        ok, detail, elapsed = burst_round(conns, shared, values, r)
        slow = max(slow, elapsed)
        if not ok:
            return False, detail, slow, 0
    return True, "", slow, DEPTH * len(conns) * rounds


def main():
    host, port = _lib.host_port()
    ctl = _lib.Conn(host, port)
    if _lib.thread_mode(ctl) != "1s":
        _lib.skip_all("lane admission exists only in fused (1s) mode")
    cfg = ctl.cmd("CONFIG", "GET", "read-local")
    if not (isinstance(cfg, list) and len(cfg) == 2 and cfg[1] == b"1"):
        _lib.skip_all("needs --read-local 1 (CONFIG GET read-local -> %r)" % (cfg,))
    probe = ctl.cmd("DEBUG", "READ-LOCAL-LANE-CAP", "0")
    if isinstance(probe, Exception) or probe != b"OK":
        _lib.skip_all("needs DEBUG READ-LOCAL-LANE-CAP (--enable-debug-command yes) -> %r"
                      % (probe,))
    rep = _lib.Report("read_local_lane")

    shared = ["rl:lane:k%d" % i for i in range(DEPTH)]
    values = [b"v%d" % i for i in range(DEPTH)]
    for key, value in zip(shared, values):
        ctl.must("SET", key, value)
    conns = [_lib.Conn(host, port, timeout=ROUND_DEADLINE_S, buffering=1 << 16)
             for _ in range(CONNS)]
    for i, c in enumerate(conns):
        c.must("SET", "rl:lane:own:%d" % i, b"init")

    # PHASE 1 -- CONTROL. Same traffic, cap derived (production geometry). Whatever this produces
    # is the baseline the capped phase must beat; on a gate-sized box it is zero.
    base = counters(ctl)
    ok, detail, slow_ctl, _ = drive(conns, shared, values, 0, 1)
    ctl_d = {k: counters(ctl)[k] - base[k] for k in COUNTERS}
    if not ok:
        rep.check("control round completes with the derived lane", False, detail)

    # PHASE 2 -- CAPPED. The lane admits CAP entries, so one connection's DEPTH-deep write
    # oversubscribes it inside a single parse pass, before any drain can run.
    ctl.must("DEBUG", "READ-LOCAL-LANE-CAP", str(CAP))
    base = counters(ctl)
    ok, detail, slow, shared_gets = drive(conns, shared, values, 1, ROUNDS)
    now = counters(ctl)
    d = {k: now[k] - base[k] for k in COUNTERS}

    rep.check("order + RYOW across deferral: every reply, in order, expected value",
              ok, detail or "%d rounds x %d conns x %d frames at lane cap %d"
              % (ROUNDS, CONNS, DEPTH + 2, CAP))
    rep.check("liveness: slowest round under %.0fs" % ROUND_DEADLINE_S,
              ok and slow < ROUND_DEADLINE_S, "slowest round %.3fs" % slow)
    rep.check("lane oversubscribed: read_local_defer_lane_full > 0",
              d["read_local_defer_lane_full"] > 0,
              "delta=%d at cap %d (%d conns x %d deep in one write)"
              % (d["read_local_defer_lane_full"], CAP, CONNS, DEPTH))
    rep.check("the CAP is why it fired: capped deferrals > uncapped deferrals",
              d["read_local_defer_lane_full"] > ctl_d["read_local_defer_lane_full"],
              "capped=%d vs derived-lane control=%d over the same traffic shape"
              % (d["read_local_defer_lane_full"], ctl_d["read_local_defer_lane_full"]))
    rep.check("no capacity demotion: read_local_fallback_lane_full == 0",
              d["read_local_fallback_lane_full"] == 0,
              "delta=%d" % d["read_local_fallback_lane_full"])
    rep.check("fair share fired: read_local_defer_quota > 0",
              d["read_local_defer_quota"] > 0, "delta=%d" % d["read_local_defer_quota"])
    rep.check("shared GETs served locally (>= 95%%)",
              d["read_local_hits"] >= 0.95 * shared_gets,
              "hits=%d of %d shared GETs (%.2f%%); fallbacks=%d" % (
                  d["read_local_hits"], shared_gets, 100.0 * d["read_local_hits"] / shared_gets,
                  d["read_local_fallbacks"]))

    # PHASE 3 -- RESTORE. The hook must leave nothing behind: back to the derived lane, traffic
    # still completes in order and locally.
    ctl.must("DEBUG", "READ-LOCAL-LANE-CAP", "0")
    base = counters(ctl)
    ok, detail, _, restored_gets = drive(conns, shared, values, ROUNDS + 1, 1)
    d2 = {k: counters(ctl)[k] - base[k] for k in COUNTERS}
    rep.check("cap 0 restores the derived lane: traffic still local and in order",
              ok and d2["read_local_fallback_lane_full"] == 0
              and d2["read_local_hits"] >= 0.95 * restored_gets,
              detail or "hits=%d of %d; fallback_lane_full=%d; deferrals back to %d"
              % (d2["read_local_hits"], restored_gets, d2["read_local_fallback_lane_full"],
                 d2["read_local_defer_lane_full"]))
    for c in conns:
        c.close()
    rep.finish()


if __name__ == "__main__":
    main()

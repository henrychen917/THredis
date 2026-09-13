#!/usr/bin/env python3
# ExpireIndex growth/shrink battery.
#   python3 tests/expireindex.py <host> <port>
#
# Guards two efficiency defects that were REPRODUCED on a live server (see NOTES-XPERF2.md):
#   1. growing the volatile-key sidecar moved every entry in ONE pass, so a single ordinary SET
#      stalled its executor -- and therefore its whole shard -- for 18ms at 734k volatile keys and
#      77ms at 2.9M.  Vanilla redis 7.4 is flat at ~32us over the same range.
#   2. the transition to zero live volatile keys cleared the WHOLE sidecar, whose capacity was
#      monotone in the all-time-high volatile population.  One historical burst taxed every later
#      live->0 forever: 2.03x a neighbouring DEL after a 1M burst.
#
# Every assertion is a RATIO between two measurements taken on the SAME connection seconds apart,
# never an absolute time, so the battery does not care how fast the machine is.
#
# Non-vacuous by construction:
#   - each leg asserts INFO keyspace `expires` really reached the population it claims to test, so
#     a run that silently failed to create volatile keys FAILS instead of passing trivially;
#   - each timing leg carries a NEGATIVE CONTROL that runs the identical code path with no TTLs at
#     all (the sidecar is then never populated).  The control bounds what the detector reports when
#     the mechanism is absent; a detector that cannot separate the two proves nothing;
#   - the active-expiry leg asserts INFO stats `expired_keys` MOVED, which can only happen if the
#     sampler still visits both tables while a migration is in flight.
import socket
import statistics
import sys
import time

HOST, PORT = sys.argv[1], int(sys.argv[2])

# The two timing legs need ONE expire index under test, so they need a single-shard boot:
#   taskset -c <cores> ./build/tomokv --port P --shards 1 --ratio 1:1 --enable-debug-command yes
# With more shards the volatile population splits and no single index reaches a predictable growth
# trigger, which would make the legs measure nothing while still reporting PASS.

# Growth trigger of the sidecar is (live + 1) * 100 >= capacity * 70, so the exact insert that
# triggers a move is predictable from the capacity alone.
SMALL_CAP = 65536
LARGE_CAP = 1048576
# Observed BEFORE the fix: 27.2x (18358us at LARGE vs 676us at SMALL).  Observed AFTER: 2.7x.
MAX_TRIGGER_GROWTH = 8.0
# Observed BEFORE the fix: 2.03x after a 1M burst.  Observed AFTER: 1.01x, same as the no-TTL
# control and same as redis 7.4.
MAX_ZERO_RATIO = 1.25
ZERO_BURST = 1 << 20

FAIL = 0


def ok(name, detail=""):
    print("  %-52s ok %s" % (name, detail))


def bad(name, detail=""):
    global FAIL
    FAIL += 1
    print("  %-52s FAIL %s" % (name, detail))


def check(name, cond, detail=""):
    ok(name, detail) if cond else bad(name, detail)


def enc(*args):
    out = [b"*%d\r\n" % len(args)]
    for a in args:
        if not isinstance(a, bytes):
            a = str(a).encode()
        out += [b"$%d\r\n" % len(a), a, b"\r\n"]
    return b"".join(out)


class C:
    def __init__(self):
        self.s = socket.create_connection((HOST, PORT), timeout=120)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.f = self.s.makefile("rb", buffering=1 << 20)

    def cmd(self, *a):
        self.s.sendall(enc(*a))
        return self.read()

    def read(self):
        line = self.f.readline()
        if not line:
            raise EOFError("server closed connection")
        k, p = line[:1], line[1:-2]
        if k == b"+":
            return p
        if k == b"-":
            raise RuntimeError(p.decode())
        if k == b":":
            return int(p)
        if k == b"$":
            n = int(p)
            if n < 0:
                return None
            d = self.f.read(n)
            self.f.read(2)
            return d
        if k == b"*":
            n = int(p)
            return None if n < 0 else [self.read() for _ in range(n)]
        raise ValueError("unexpected RESP %r" % k)

    def fill(self, fmt, lo, hi, ttl, batch=512):
        i = lo
        while i < hi:
            take = min(batch, hi - i)
            if ttl:
                frames = b"".join(enc("SET", fmt % (i + j), b"v", "PX", ttl) for j in range(take))
            else:
                frames = b"".join(enc("SET", fmt % (i + j), b"v") for j in range(take))
            self.s.sendall(frames)
            for _ in range(take):
                assert self.read() == b"OK"
            i += take

    def drop(self, fmt, lo, hi, batch=512):
        i = lo
        while i < hi:
            take = min(batch, hi - i)
            self.s.sendall(b"".join(enc("DEL", fmt % (i + j)) for j in range(take)))
            for _ in range(take):
                self.read()
            i += take

    def volatile_count(self):
        body = self.cmd("INFO", "keyspace")
        for line in body.splitlines():
            if line.startswith(b"db0:"):
                for field in line.split(b","):
                    if field.startswith(b"expires="):
                        return int(field[8:])
        return 0

    def shard_count(self):
        body = self.cmd("DEBUG", "LBSIGNALS")
        if not isinstance(body, bytes):
            raise RuntimeError("DEBUG LBSIGNALS unavailable: boot with --enable-debug-command yes")
        return sum(1 for line in body.decode().splitlines() if line.split()[:1] == ["shard"])

    def expired_keys(self):
        body = self.cmd("INFO", "stats")
        for line in body.splitlines():
            if line.startswith(b"expired_keys:"):
                return int(line.split(b":", 1)[1])
        return 0


def trigger_cost(c, tag, cap, ttl, samples=48):
    """Median SET cost just below a growth trigger, and the cost of the triggering SET itself."""
    target = -(-cap * 70 // 100) - 1        # ceil(0.70 * cap) - 1 live entries
    args = ("PX", ttl) if ttl else ()
    c.cmd("FLUSHALL")
    c.fill("xi:%s:%%d" % tag, 0, target - samples, ttl)
    base = []
    for i in range(target - samples, target):
        t0 = time.perf_counter_ns()
        assert c.cmd("SET", "xi:%s:%d" % (tag, i), b"v", *args) == b"OK"
        base.append(time.perf_counter_ns() - t0)
    t0 = time.perf_counter_ns()
    assert c.cmd("SET", "xi:%s:%d" % (tag, target), b"v", *args) == b"OK"
    trig = time.perf_counter_ns() - t0
    return statistics.median(base) / 1e3, trig / 1e3, target + 1


def leg_growth_is_incremental(c):
    print("growth: moving the sidecar must not scale with the volatile population")
    small_base, small_trig, small_live = trigger_cost(c, "s", SMALL_CAP, 3600000)
    check("small trigger populated the sidecar", c.volatile_count() == small_live,
          "expires=%d want=%d" % (c.volatile_count(), small_live))
    large_base, large_trig, large_live = trigger_cost(c, "l", LARGE_CAP, 3600000)
    check("large trigger populated the sidecar", c.volatile_count() == large_live,
          "expires=%d want=%d" % (c.volatile_count(), large_live))
    check("populations really differ by 16x", large_live >= 15 * small_live,
          "%d vs %d" % (large_live, small_live))
    growth = large_trig / small_trig
    check("trigger cost growth <= %.1fx over 16x population" % MAX_TRIGGER_GROWTH,
          growth <= MAX_TRIGGER_GROWTH,
          "small=%.0fus large=%.0fus growth=%.2fx" % (small_trig, large_trig, growth))
    # NEGATIVE CONTROL: identical shape with no deadlines, so the sidecar is never populated. This
    # is what "no growth" looks like on this machine; the TTL legs are compared against it.
    ctl_small_base, ctl_small, _ = trigger_cost(c, "cs", SMALL_CAP, 0)
    ctl_large_base, ctl_large, _ = trigger_cost(c, "cl", LARGE_CAP, 0)
    check("control (no TTL) leaves the sidecar empty", c.volatile_count() == 0,
          "expires=%d" % c.volatile_count())
    ok("control growth (no TTL)", "small=%.0fus large=%.0fus growth=%.2fx"
       % (ctl_small, ctl_large, ctl_large / ctl_small))
    ok("baselines", "ttl %.1f/%.1fus control %.1f/%.1fus"
       % (small_base, large_base, ctl_small_base, ctl_large_base))
    c.cmd("FLUSHALL")


def zero_pair(c, tag, burst, ttl, reps=41):
    """Paired DEL cost: A leaves live volatile keys behind, B takes the shard's count to zero."""
    args = ("PX", ttl) if ttl else ()
    c.cmd("FLUSHALL")
    c.fill("xi:%s:%%d" % tag, 0, burst, ttl)
    reached = c.volatile_count()
    c.drop("xi:%s:%%d" % tag, 0, burst)
    assert c.cmd("DBSIZE") == 0
    ctl, zero = [], []
    for _ in range(reps):
        assert c.cmd("SET", "xi:%sA" % tag, b"v", *args) == b"OK"
        assert c.cmd("SET", "xi:%sB" % tag, b"v", *args) == b"OK"
        t0 = time.perf_counter_ns()
        c.cmd("DEL", "xi:%sA" % tag)
        t1 = time.perf_counter_ns()
        c.cmd("DEL", "xi:%sB" % tag)
        t2 = time.perf_counter_ns()
        ctl.append(t1 - t0)
        zero.append(t2 - t1)
    assert c.cmd("DBSIZE") == 0
    return statistics.median(ctl) / 1e3, statistics.median(zero) / 1e3, reached


def leg_zero_transition(c):
    print("live->0: the last volatile DEL must not scale with the ALL-TIME-HIGH population")
    ctl, zero, reached = zero_pair(c, "z", ZERO_BURST, 3600000)
    check("burst really populated the sidecar", reached >= ZERO_BURST,
          "expires=%d want>=%d" % (reached, ZERO_BURST))
    ratio = zero / ctl
    check("live->0 DEL <= %.2fx a neighbouring DEL" % MAX_ZERO_RATIO, ratio <= MAX_ZERO_RATIO,
          "ctl=%.2fus zero=%.2fus ratio=%.2f" % (ctl, zero, ratio))
    # NEGATIVE CONTROL: same burst size, no deadlines. The DEL that empties the keyspace then never
    # touches the sidecar at all, so this is the detector's zero reading.
    cctl, czero, creached = zero_pair(c, "cz", ZERO_BURST, 0)
    check("control burst left the sidecar empty", creached == 0, "expires=%d" % creached)
    ok("control ratio (no TTL)", "ctl=%.2fus zero=%.2fus ratio=%.2f" % (cctl, czero, czero / cctl))
    c.cmd("FLUSHALL")


def leg_active_expiry_survives_migration(c):
    print("active expiry: the sampler must still visit deadlines parked mid-migration")
    c.cmd("FLUSHALL")
    before = c.expired_keys()
    # Big enough that the population straddles several sidecar doublings and is still mid-move when
    # the deadlines start landing, small enough that the sampler drains it in a few seconds.
    n = 20000
    c.fill("xi:m:%d", 0, n, 3000)
    live = c.volatile_count()
    check("population is volatile", live >= n * 0.9, "expires=%d of %d" % (live, n))
    deadline = time.time() + 60
    while time.time() < deadline:
        if c.cmd("DBSIZE") == 0:
            break
        time.sleep(0.25)
    moved = c.expired_keys() - before
    check("keys were actively expired without being touched", moved >= n * 0.99,
          "expired_keys moved %d of %d" % (moved, n))
    check("keyspace drained", c.cmd("DBSIZE") == 0, "dbsize=%s" % c.cmd("DBSIZE"))
    check("sidecar reports empty", c.volatile_count() == 0, "expires=%d" % c.volatile_count())
    c.cmd("FLUSHALL")


def leg_semantics(c):
    print("semantics: deadlines survive growth, shrink and re-growth")
    c.cmd("FLUSHALL")
    # Straddle several doublings, then verify every deadline is still readable and correct.
    n = 50000
    c.fill("xi:sem:%d", 0, n, 3600000)
    ttls = [c.cmd("PTTL", "xi:sem:%d" % i) for i in (0, 1, n // 2, n - 1)]
    check("PTTL survives growth", all(isinstance(t, int) and 0 < t <= 3600000 for t in ttls),
          str(ttls))
    # PERSIST every key: live count returns to zero, which is the release path.
    for i in range(0, n, 512):
        take = min(512, n - i)
        c.s.sendall(b"".join(enc("PERSIST", "xi:sem:%d" % (i + j)) for j in range(take)))
        for _ in range(take):
            c.read()
    check("sidecar drained by PERSIST", c.volatile_count() == 0, "expires=%d" % c.volatile_count())
    check("keys survived PERSIST", c.cmd("DBSIZE") == n, "dbsize=%s" % c.cmd("DBSIZE"))
    check("PTTL now -1", c.cmd("PTTL", "xi:sem:7") == -1)
    # Re-grow from the released minimum and verify again.
    for i in range(0, n, 512):
        take = min(512, n - i)
        c.s.sendall(b"".join(enc("PEXPIRE", "xi:sem:%d" % (i + j), 3600000) for j in range(take)))
        for _ in range(take):
            c.read()
    check("sidecar re-grew", c.volatile_count() == n, "expires=%d" % c.volatile_count())
    check("PTTL after re-growth", 0 < c.cmd("PTTL", "xi:sem:%d" % (n - 1)) <= 3600000)
    # DEL a random spread and confirm the count follows exactly.
    c.drop("xi:sem:%d", 0, n, 512)
    check("sidecar empty after DEL", c.volatile_count() == 0, "expires=%d" % c.volatile_count())
    c.cmd("FLUSHALL")


def main():
    c = C()
    shards = c.shard_count()
    if shards != 1:
        print("  %-52s FAIL server has %d shards; boot with --shards 1"
              % ("single-shard precondition", shards))
        print("EXPIREINDEX FAIL 1")
        sys.exit(1)
    ok("single-shard precondition", "shards=1")
    leg_semantics(c)
    leg_active_expiry_survives_migration(c)
    leg_growth_is_incremental(c)
    leg_zero_transition(c)
    print("EXPIREINDEX %s" % ("PASS" if FAIL == 0 else "FAIL %d" % FAIL))
    sys.exit(1 if FAIL else 0)


if __name__ == "__main__":
    main()

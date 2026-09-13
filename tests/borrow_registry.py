#!/usr/bin/env python3
# Zero-copy borrow registry scaling battery.
#   python3 tests/borrow_registry.py <host> <port>
# Boot: --shards 1 --zc-min 64 --client-output-buffer-limit normal 0 0 0 --enable-debug-command yes
#   one shard  -> every borrow lands in ONE registry, which is the quantity under test
#   zc-min 64  -> a small value still takes the borrow path, so the probe pays registry cost and
#                 almost no wire cost
#   no obuf limit -> the holders below are deliberately slow readers and must not be disconnected
#
# GUARDS a defect that was REPRODUCED on a live server (see NOTES-XPERF2.md): the registry was a
# linear vector scanned once per borrow, per release, per retirement and per in-place-overwrite
# check, so a borrowed GET cost 2903 -> 3185 ns/op (+9.7%) as the live borrow count on its shard
# went 0 -> ~1100, while the identical NON-borrowed GET stayed flat.
#
# Non-vacuous by construction:
#   - the borrow path is PROVEN live, not assumed: DEBUG BORROWCOUNT reads the exact owner-side
#     FlatStore counters and the battery requires zero without holders, non-zero while parked, and
#     zero again after holder teardown;
#   - the plain-GET arm is a NEGATIVE CONTROL measured in the same loop on the same connection. It
#     never enters the registry, so it is what "no growth" reads like on this machine;
#   - the assertion is a RATIO of two per-op costs measured seconds apart, never an absolute time.
import socket
import sys
import time

HOST, PORT = sys.argv[1], int(sys.argv[2])

HOLD_BYTES = 8192
# One unread connection gets ~75 MiB of replies.  Unlike its receive-window setting, that volume
# is deliberately larger than a normal per-socket kernel send queue, so some distinct values must
# remain borrowed by the application.  DEBUG BORROWCOUNT below is the authority on whether it did.
PER_HOLDER = 9600
TARGET_BORROWS = 2000
PROBE_BYTES = 128          # >= zc-min, so it borrows
PLAIN_BYTES = 32           # <  zc-min, so it never borrows: the control
DEPTH = 32
OPS = 64000
ROUNDS = 3
# Observed BEFORE the fix: 1.097 at ~1100 live borrows.  AFTER: 1.020 at ~2100.
MAX_GROWTH = 1.05

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
    def __init__(self, rcvbuf=None):
        self.s = socket.create_connection((HOST, PORT), timeout=120)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        if rcvbuf:
            self.s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
            if hasattr(socket, "TCP_WINDOW_CLAMP"):
                self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_WINDOW_CLAMP, rcvbuf)
        self.f = self.s.makefile("rb", buffering=1 << 20)

    def close(self):
        try:
            self.f.close()
            self.s.close()
        except OSError:
            pass

    def cmd(self, *a):
        self.s.sendall(enc(*a))
        return self.read()

    def read(self):
        line = self.f.readline()
        if not line:
            raise EOFError
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

    def rate(self, frame, reply, ops, depth):
        batch = frame * depth
        want = reply * depth
        t0 = time.perf_counter()
        for _ in range(ops // depth):
            self.s.sendall(batch)
            got = self.f.read(len(want))
            if got != want:
                raise AssertionError("reply mismatch %r" % got[:48])
        return (ops // depth * depth) / (time.perf_counter() - t0)

    def shard_count(self):
        body = self.cmd("DEBUG", "LBSIGNALS")
        return sum(1 for line in body.decode().splitlines() if line.split()[:1] == ["shard"])

    def live_borrows(self):
        return self.cmd("DEBUG", "BORROWCOUNT")


def wait_borrows(admin, predicate, timeout=5.0):
    deadline = time.monotonic() + timeout
    count = admin.live_borrows()
    while not predicate(count) and time.monotonic() < deadline:
        time.sleep(0.05)
        count = admin.live_borrows()
    return count


def main():
    admin = C()
    shards = admin.shard_count()
    if shards != 1:
        bad("single-shard precondition", "shards=%d; boot with --shards 1" % shards)
        print("BORROW-REGISTRY FAIL 1")
        sys.exit(1)
    ok("single-shard precondition", "shards=1")
    zc = admin.cmd("CONFIG", "GET", "zc-min")
    zc_min = int(zc[1]) if isinstance(zc, list) and len(zc) == 2 else -1
    if not (0 < zc_min <= PROBE_BYTES and zc_min > PLAIN_BYTES):
        bad("zc gate straddles the two probes",
            "zc-min=%s needs %d < zc-min <= %d" % (zc_min, PLAIN_BYTES, PROBE_BYTES))
        print("BORROW-REGISTRY FAIL 1")
        sys.exit(1)
    ok("zc gate straddles the two probes", "zc-min=%d plain=%d borrow=%d"
       % (zc_min, PLAIN_BYTES, PROBE_BYTES))

    admin.cmd("FLUSHALL")
    hold = b"h" * HOLD_BYTES
    nkeys = PER_HOLDER
    for i in range(0, nkeys, 32):
        take = min(32, nkeys - i)
        admin.s.sendall(b"".join(enc("SET", "br:%d" % (i + j), hold) for j in range(take)))
        for _ in range(take):
            assert admin.read() == b"OK"
    assert admin.cmd("SET", "br:probe", b"B" * PROBE_BYTES) == b"OK"
    assert admin.cmd("SET", "br:plain", b"P" * PLAIN_BYTES) == b"OK"

    probe = C()
    fb = enc("GET", "br:probe")
    rb = b"$%d\r\n" % PROBE_BYTES + b"B" * PROBE_BYTES + b"\r\n"
    fp = enc("GET", "br:plain")
    rp = b"$%d\r\n" % PLAIN_BYTES + b"P" * PLAIN_BYTES + b"\r\n"
    probe.rate(fb, rb, DEPTH * 40, DEPTH)
    probe.rate(fp, rp, DEPTH * 40, DEPTH)

    def measure():
        b = max(probe.rate(fb, rb, OPS, DEPTH) for _ in range(ROUNDS))
        p = max(probe.rate(fp, rp, OPS, DEPTH) for _ in range(ROUNDS))
        return 1e9 / b, 1e9 / p

    borrow_idle, plain_idle = measure()
    idle_borrows = wait_borrows(admin, lambda count: count == 0)
    check("borrow registry idle without holders", idle_borrows == 0,
          "live-borrows=%d" % idle_borrows)

    # A small client receive window alone does not prove application-side backpressure: the peer's
    # kernel send queue can accept bytes the client has not read.  Put far more than that queue's
    # normal capacity on one unread connection, then ask the shard registry itself what remains.
    holders = []
    issued = 0
    live_borrows = idle_borrows
    while live_borrows < TARGET_BORROWS and issued + PER_HOLDER <= nkeys:
        h = C(rcvbuf=2048)
        h.s.sendall(b"".join(enc("GET", "br:%d" % (issued + j)) for j in range(PER_HOLDER)))
        holders.append(h)
        issued += PER_HOLDER
        live_borrows = wait_borrows(admin, lambda count: count >= TARGET_BORROWS)

    borrow_busy, plain_busy = measure()
    live_borrows_after = admin.live_borrows()
    check("holders really parked borrows on the shard",
          min(live_borrows, live_borrows_after) >= TARGET_BORROWS // 2,
          "live-borrows %d -> %d (after measure %d)"
          % (idle_borrows, live_borrows, live_borrows_after))
    for h in holders:
        h.close()
    probe.close()
    drained_borrows = wait_borrows(admin, lambda count: count == 0)
    check("borrow registry drained after holders", drained_borrows == 0,
          "live-borrows=%d" % drained_borrows)

    growth = borrow_busy / borrow_idle
    control = plain_busy / plain_idle
    check("borrowed GET per-op cost growth <= %.2f" % MAX_GROWTH, growth <= MAX_GROWTH,
          "%.0f -> %.0f ns  ratio=%.3f" % (borrow_idle, borrow_busy, growth))
    check("control (non-borrowed GET) stayed flat", control <= MAX_GROWTH,
          "%.0f -> %.0f ns  ratio=%.3f" % (plain_idle, plain_busy, control))
    check("the two arms really took different paths", borrow_idle > plain_idle * 1.5,
          "borrow %.0f ns vs plain %.0f ns" % (borrow_idle, plain_idle))
    admin.cmd("FLUSHALL")
    admin.close()
    print("BORROW-REGISTRY %s" % ("PASS" if FAIL == 0 else "FAIL %d" % FAIL))
    sys.exit(1 if FAIL else 0)


if __name__ == "__main__":
    main()

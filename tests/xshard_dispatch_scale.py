#!/usr/bin/env python3
# One measurement arm for the cross-shard dispatch scaling guard.
#   python3 tests/xshard_dispatch_scale.py <host> <port> [ops] [depth] [rounds]
# Prints:  ARM threads=<n> cross_ns=<f> same_ns=<f> cross_shards=<a>,<b> same_shard=<c>
#
# Two workloads on ONE connection, alternating so drift hits both equally:
#   cross  MGET of two keys that DEBUG SHARD proves live on DIFFERENT shards -> scatter dispatch
#   same   MGET of two keys on the SAME shard -> localfast, never enters the dispatch arm
# `same` is the control: it is the identical command with the identical key count and reply, so any
# difference between the two arms across thread counts belongs to the dispatch path.
# The geometry is ASSERTED, not assumed: a run whose "cross" pair happened to land on one shard
# would measure the localfast path twice and report a meaningless ratio, so it exits non-zero.
import socket
import statistics
import sys
import time

HOST, PORT = sys.argv[1], int(sys.argv[2])
OPS = int(sys.argv[3]) if len(sys.argv) > 3 else 300000
DEPTH = int(sys.argv[4]) if len(sys.argv) > 4 else 32
ROUNDS = int(sys.argv[5]) if len(sys.argv) > 5 else 3


def enc(*args):
    out = [b"*%d\r\n" % len(args)]
    for a in args:
        if not isinstance(a, bytes):
            a = str(a).encode()
        out += [b"$%d\r\n" % len(a), a, b"\r\n"]
    return b"".join(out)


class C:
    def __init__(self):
        self.s = socket.create_connection((HOST, PORT), timeout=60)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.f = self.s.makefile("rb", buffering=1 << 20)

    def cmd(self, *a):
        self.s.sendall(enc(*a))
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
        raise ValueError("unexpected RESP %r" % k)

    def rate(self, frame, reply, ops, depth):
        batch = frame * depth
        want = reply * depth
        t0 = time.perf_counter()
        for _ in range(ops // depth):
            self.s.sendall(batch)
            got = self.f.read(len(want))
            if got != want:
                raise AssertionError("reply mismatch %r" % got[:64])
        return (ops // depth * depth) / (time.perf_counter() - t0)

    def threads(self):
        body = self.cmd("DEBUG", "LBSIGNALS")
        return sum(1 for line in body.decode().splitlines() if line.split()[:1] == ["thread"])

    def shard_owners(self):
        body = self.cmd("DEBUG", "LBSIGNALS")
        owners = {}
        for line in body.decode().splitlines():
            f = line.split()
            if f[:1] == ["shard"]:
                owners[int(f[1])] = int(f[2])
        return owners


def main():
    c = C()
    c.cmd("FLUSHALL")
    owners = c.shard_owners()
    base = "xds:%d"
    s0 = c.cmd("DEBUG", "SHARD", base % 0)
    cross = same = None
    # The cross pair must span two shards with DIFFERENT owner threads: that is what makes the
    # dispatch fan out to two participants, which is the geometry the guard holds fixed.
    for i in range(1, 40000):
        s = c.cmd("DEBUG", "SHARD", base % i)
        if cross is None and owners[s] != owners[s0]:
            cross = (base % i, s)
        if same is None and s == s0:
            same = (base % i, s)
        if cross and same:
            break
    if not cross or not same:
        print("FAIL: could not find both a two-owner cross-shard pair and a same-shard pair")
        sys.exit(1)
    keys = [base % 0, cross[0], same[0]]
    for k in keys:
        assert c.cmd("SET", k, b"x") == b"OK"
    reply = b"*2\r\n$1\r\nx\r\n$1\r\nx\r\n"
    fcross = enc("MGET", base % 0, cross[0])
    fsame = enc("MGET", base % 0, same[0])
    c.rate(fcross, reply, DEPTH * 50, DEPTH)
    c.rate(fsame, reply, DEPTH * 50, DEPTH)
    # The reported quantity is the DISPATCH EXCESS: cross ns/op minus same ns/op. Both halves are
    # measured back to back inside one round and subtracted THERE, so a drift that moves the whole
    # server cancels instead of being amplified 5x by differencing two independently-chosen bests.
    # The round median is then robust to a single disturbed window.
    excess, crosses, sames = [], [], []
    for _ in range(ROUNDS):
        cn = 1e9 / c.rate(fcross, reply, OPS, DEPTH)
        sn = 1e9 / c.rate(fsame, reply, OPS, DEPTH)
        excess.append(cn - sn)
        crosses.append(cn)
        sames.append(sn)
    print("ARM threads=%d excess_ns=%.1f cross_ns=%.1f same_ns=%.1f cross_owners=%d same_owners=%d"
          % (c.threads(), statistics.median(excess), statistics.median(crosses),
             statistics.median(sames),
             len({owners[s0], owners[cross[1]]}), len({owners[s0], owners[same[1]]})))


if __name__ == "__main__":
    main()

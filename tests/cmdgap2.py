#!/usr/bin/env python3
"""Directed battery for the self-contained command found in the second inventory lane.

Usage: tests/cmdgap2.py HOST PORT

PFDEBUG is useful only when its HLL machinery actually fires, so this battery checks the decoded
sparse opcodes, all 16,384 logical registers, a real sparse-to-dense rewrite, the rewritten byte
image, and TTL preservation.  The eight commands that need absent database/cluster/module/
replication machinery are negative inventory controls rather than partial implementations.
"""

import socket
import sys


HOST, PORT = sys.argv[1], int(sys.argv[2])
failures = []
checks = 0


class RespError:
    def __init__(self, message):
        self.message = message

    def __eq__(self, other):
        return isinstance(other, RespError) and self.message == other.message

    def __repr__(self):
        return "RespError(%r)" % self.message


def frame(*args):
    values = [arg if isinstance(arg, bytes) else str(arg).encode() for arg in args]
    return (b"*%d\r\n" % len(values) +
            b"".join(b"$%d\r\n" % len(value) + value + b"\r\n" for value in values))


class Conn:
    def __init__(self):
        self.sock = socket.create_connection((HOST, PORT), timeout=10)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.file = self.sock.makefile("rb")

    def read(self):
        line = self.file.readline()
        if not line:
            raise EOFError("server closed connection")
        kind, body = line[:1], line[1:-2]
        if kind == b"+":
            return body
        if kind == b"-":
            return RespError(body)
        if kind == b":":
            return int(body)
        if kind == b"$":
            size = int(body)
            if size == -1:
                return None
            value = self.file.read(size)
            if self.file.read(2) != b"\r\n":
                raise AssertionError("bad bulk trailer")
            return value
        if kind == b"*":
            size = int(body)
            return None if size == -1 else [self.read() for _ in range(size)]
        if kind == b"_":
            return None
        raise AssertionError("unsupported RESP reply %r" % line[:32])

    def cmd(self, *args):
        self.sock.sendall(frame(*args))
        return self.read()

    def close(self):
        self.file.close()
        self.sock.close()


def expect(label, got, want):
    global checks
    checks += 1
    if got != want:
        failures.append("%s: got %r, want %r" % (label, got, want))


def run():
    c = Conn()
    try:
        names = c.cmd("COMMAND", "LIST")
        name_set = set(names) if isinstance(names, list) else set()
        expect("PFDEBUG inventory row listed", b"pfdebug" in name_set, True)
        expect("COMMAND COUNT moved by one", c.cmd("COMMAND", "COUNT"), 242)
        info = c.cmd("COMMAND", "INFO", "PFDEBUG")
        routing = ((info[0][0], info[0][1], info[0][3], info[0][4], info[0][5])
                   if isinstance(info, list) and info and isinstance(info[0], list)
                   else None)
        expect("PFDEBUG routing fields", routing, (b"pfdebug", 3, 2, 2, 1))

        handed_on = {b"cluster", b"migrate", b"module", b"move", b"psync", b"replconf",
                     b"swapdb", b"sync"}
        expect("architectural handoff inventory control", name_set.isdisjoint(handed_on), True)

        arity = RespError(b"ERR wrong number of arguments for 'pfdebug' command")
        expect("PFDEBUG arity low", c.cmd("PFDEBUG", "ENCODING"), arity)
        expect("PFDEBUG arity high", c.cmd("PFDEBUG", "ENCODING", "x", "extra"), arity)
        missing = RespError(b"ERR The specified key does not exist")
        expect("PFDEBUG missing", c.cmd("PFDEBUG", "ENCODING", "cmdgap2:missing"), missing)
        expect("missing control remains absent", c.cmd("EXISTS", "cmdgap2:missing"), 0)

        c.cmd("DEL", "cmdgap2:hll", "cmdgap2:getreg", "cmdgap2:wrong", "cmdgap2:bad",
              "cmdgap2:corrupt")
        expect("ordinary PFADD control", c.cmd("PFADD", "cmdgap2:hll", "a", "b", "c"), 1)
        expect("sparse encoding", c.cmd("PFDEBUG", "ENCODING", "cmdgap2:hll"), b"sparse")
        expect("sparse opcode decode", c.cmd("PFDEBUG", "DECODE", "cmdgap2:hll"),
               b"Z:8436 v:1,1 Z:4274 v:2,1 Z:3068 v:1,1 Z:603")

        expect("GETREG fixture", c.cmd("PFADD", "cmdgap2:getreg", "a", "b", "c"), 1)
        expect("GETREG fixture starts sparse",
               c.cmd("PFDEBUG", "ENCODING", "cmdgap2:getreg"), b"sparse")
        registers = c.cmd("PFDEBUG", "GETREG", "cmdgap2:getreg")
        nonzero = ([(index, value) for index, value in enumerate(registers) if value]
                   if isinstance(registers, list) else None)
        expect("GETREG returned every logical register",
               len(registers) if isinstance(registers, list) else None, 16384)
        expect("GETREG decoded sparse values", nonzero,
               [(8436, 1), (12711, 2), (15780, 1)])
        expect("GETREG rewrote sparse storage",
               c.cmd("PFDEBUG", "ENCODING", "cmdgap2:getreg"), b"dense")

        expect("TTL setup", c.cmd("PEXPIRE", "cmdgap2:hll", "600000"), 1)
        ttl_before = c.cmd("PTTL", "cmdgap2:hll")
        expect("TTL armed", isinstance(ttl_before, int) and 590000 <= ttl_before <= 600000, True)
        expect("TODENSE sparse rewrite reply", c.cmd("PFDEBUG", "TODENSE", "cmdgap2:hll"), 1)
        expect("dense encoding", c.cmd("PFDEBUG", "ENCODING", "cmdgap2:hll"), b"dense")
        image = c.cmd("GET", "cmdgap2:hll")
        expect("TODENSE wrote a full dense image",
               isinstance(image, bytes) and len(image) == 12304 and image[:5] == b"HYLL\x00", True)
        ttl_after = c.cmd("PTTL", "cmdgap2:hll")
        expect("TODENSE preserved TTL",
               isinstance(ttl_after, int) and 0 < ttl_after <= ttl_before, True)
        expect("dense DECODE refusal", c.cmd("PFDEBUG", "DECODE", "cmdgap2:hll"),
               RespError(b"ERR HLL encoding is not sparse"))
        expect("TODENSE dense no-op reply", c.cmd("PFDEBUG", "TODENSE", "cmdgap2:hll"), 0)
        dense_registers = c.cmd("PFDEBUG", "GETREG", "cmdgap2:hll")
        dense_nonzero = ([(index, value) for index, value in enumerate(dense_registers) if value]
                         if isinstance(dense_registers, list) else None)
        expect("dense GETREG preserved values", dense_nonzero, nonzero)

        expect("unknown PFDEBUG subcommand",
               c.cmd("PFDEBUG", "NOPE", "cmdgap2:hll"),
               RespError(b"ERR Unknown PFDEBUG subcommand 'NOPE'"))
        expect("SET wrong-type fixture", c.cmd("SADD", "cmdgap2:wrong", "x"), 1)
        expect("collection wrong type", c.cmd("PFDEBUG", "ENCODING", "cmdgap2:wrong"),
               RespError(b"WRONGTYPE Operation against a key holding the wrong kind of value"))
        expect("SET bad-header fixture", c.cmd("SET", "cmdgap2:bad", "not-a-hll"), b"OK")
        expect("bad HLL header", c.cmd("PFDEBUG", "ENCODING", "cmdgap2:bad"),
               RespError(b"WRONGTYPE Key is not a valid HyperLogLog string value."))

        corrupt = b"HYLL\x01\x00\x00\x00" + b"\x00" * 7 + b"\x80" + b"\x00"
        expect("SET corrupt sparse fixture", c.cmd("SET", "cmdgap2:corrupt", corrupt), b"OK")
        expect("corrupt header still identifies sparse",
               c.cmd("PFDEBUG", "ENCODING", "cmdgap2:corrupt"), b"sparse")
        expect("DECODE exposes available corrupt opcode",
               c.cmd("PFDEBUG", "DECODE", "cmdgap2:corrupt"), b"z:1")
        invalid = RespError(b"INVALIDOBJ Corrupted HLL object detected")
        expect("GETREG detects corrupt stream", c.cmd("PFDEBUG", "GETREG", "cmdgap2:corrupt"),
               invalid)
        expect("TODENSE detects corrupt stream", c.cmd("PFDEBUG", "TODENSE", "cmdgap2:corrupt"),
               invalid)

        truncated_xzero = b"HYLL\x01" + b"\x00" * 10 + b"\x80\x40"
        expect("SET truncated XZERO fixture",
               c.cmd("SET", "cmdgap2:truncated-xzero", truncated_xzero), b"OK")
        expect("truncated XZERO header still identifies sparse",
               c.cmd("PFDEBUG", "ENCODING", "cmdgap2:truncated-xzero"), b"sparse")
        expect("DECODE uses the string terminator as the diagnostic low byte",
               c.cmd("PFDEBUG", "DECODE", "cmdgap2:truncated-xzero"), b"Z:1")
        expect("GETREG rejects truncated XZERO",
               c.cmd("PFDEBUG", "GETREG", "cmdgap2:truncated-xzero"), invalid)
        expect("TODENSE rejects truncated XZERO",
               c.cmd("PFDEBUG", "TODENSE", "cmdgap2:truncated-xzero"), invalid)

        # This remains a real unknown command. It is the control leg for inventory tests that
        # would otherwise pass after registering every probed name indiscriminately.
        expect("unknown-command control", c.cmd("CMDGAP2-NOSUCH"),
               RespError(b"ERR unknown command 'CMDGAP2-NOSUCH', with args beginning with: "))
    finally:
        c.close()


run()
if failures:
    for failure in failures:
        print("FAIL", failure)
    print("CMDGAP2 FAIL: %d checks, %d failures" % (checks, len(failures)))
    raise SystemExit(1)
print("CMDGAP2 PASS: %d checks; PFDEBUG sparse decode/getreg and TODENSE rewrite fired" % checks)

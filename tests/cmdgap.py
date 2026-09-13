#!/usr/bin/env python3
"""Directed battery for the small, local COMMAND inventory additions.

Usage: tests/cmdgap.py HOST PORT

The expected bytes were observed against the assigned vanilla Redis 7.4.2 binary. The
RESTORE-ASKING checks use a DUMP produced by the server under test, so the success path must
decode, install, and read back a real value; a handler that merely answers OK cannot pass.
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
        added = {b"asking", b"readonly", b"readwrite", b"restore-asking"}
        expect("all four inventory rows listed", added <= name_set, True)
        # The first lane contributed four rows; the follow-on PFDEBUG lane contributes one more.
        expect("COMMAND COUNT includes both inventory lanes", c.cmd("COMMAND", "COUNT"), 242)
        info = c.cmd("COMMAND", "INFO", "ASKING", "READONLY", "READWRITE",
                     "RESTORE-ASKING")
        routing = [(row[0], row[1], row[3], row[4], row[5]) for row in info]
        expect("COMMAND INFO routing fields", routing,
               [(b"asking", 1, 0, 0, 0),
                (b"readonly", 1, 0, 0, 0),
                (b"readwrite", 1, 0, 0, 0),
                (b"restore-asking", -4, 1, 1, 1)])
        # These are intentionally handed on or out of scope. This control catches a test that
        # accidentally treats every formerly absent command as part of this small implementation.
        shelved = {b"cluster", b"migrate", b"module", b"move", b"psync", b"replconf",
                   b"swapdb", b"sync"}
        expect("shelved inventory control", name_set.isdisjoint(shelved), True)

        disabled = RespError(b"ERR This instance has cluster support disabled")
        for command in ("ASKING", "READONLY", "READWRITE"):
            expect(command + " standalone reply", c.cmd(command), disabled)
            expect(command + " arity",
                   c.cmd(command, "extra"),
                   RespError(b"ERR wrong number of arguments for '%s' command" %
                             command.lower().encode()))

        c.cmd("DEL", "cmdgap:source", "cmdgap:restored", "cmdgap:ttl")
        value = b"cmdgap-value\x00binary"
        expect("SET source", c.cmd("SET", "cmdgap:source", value), b"OK")
        payload = c.cmd("DUMP", "cmdgap:source")
        expect("DUMP produced payload", isinstance(payload, bytes) and len(payload) > len(value),
               True)

        expect("RESTORE-ASKING success",
               c.cmd("RESTORE-ASKING", "cmdgap:restored", "0", payload), b"OK")
        expect("RESTORE-ASKING installed value", c.cmd("GET", "cmdgap:restored"), value)
        expect("RESTORE-ASKING busy-key",
               c.cmd("RESTORE-ASKING", "cmdgap:restored", "0", payload),
               RespError(b"BUSYKEY Target key name already exists."))
        expect("RESTORE-ASKING replace",
               c.cmd("RESTORE-ASKING", "cmdgap:restored", "0", payload, "REPLACE"), b"OK")
        expect("RESTORE-ASKING negative TTL",
               c.cmd("RESTORE-ASKING", "cmdgap:ttl", "-1", payload),
               RespError(b"ERR Invalid TTL value, must be >= 0"))
        expect("RESTORE-ASKING noninteger TTL",
               c.cmd("RESTORE-ASKING", "cmdgap:ttl", "abc", payload),
               RespError(b"ERR value is not an integer or out of range"))
        expect("RESTORE-ASKING option grammar",
               c.cmd("RESTORE-ASKING", "cmdgap:ttl", "0", payload, "BOGUS"),
               RespError(b"ERR syntax error"))

        # Negative controls: the ordinary RESTORE path remains live, and a genuinely unknown
        # command remains unknown after the inventory table grows.
        c.cmd("DEL", "cmdgap:ordinary")
        expect("ordinary RESTORE control",
               c.cmd("RESTORE", "cmdgap:ordinary", "0", payload), b"OK")
        expect("ordinary RESTORE value control", c.cmd("GET", "cmdgap:ordinary"), value)
        expect("unknown-command control", c.cmd("CMDGAP-NOSUCH"),
               RespError(b"ERR unknown command 'CMDGAP-NOSUCH', with args beginning with: "))
    finally:
        c.close()


run()
if failures:
    for failure in failures:
        print("FAIL", failure)
    print("CMDGAP FAIL: %d checks, %d failures" % (checks, len(failures)))
    raise SystemExit(1)
print("CMDGAP PASS: %d checks; 4 inventory rows, 3 cluster-disabled replies, "
      "1 restore alias fired" % checks)

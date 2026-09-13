#!/usr/bin/env python3
"""Directed boot-gated DEBUG and snapshot reload test. Usage: tests/debug.py HOST PORT"""

import os
import socket
import sys
import time


HOST, PORT = sys.argv[1], int(sys.argv[2])


class RespError(Exception):
    pass


def frame(*args):
    values = [arg if isinstance(arg, bytes) else str(arg).encode() for arg in args]
    return (b"*%d\r\n" % len(values) +
            b"".join(b"$%d\r\n" % len(value) + value + b"\r\n" for value in values))


class Conn:
    def __init__(self):
        self.sock = socket.create_connection((HOST, PORT), timeout=30)
        self.file = self.sock.makefile("rb")

    def command(self, *args):
        self.sock.sendall(frame(*args))
        return self.read()

    def read(self):
        kind = self.file.read(1)
        if not kind:
            raise EOFError("server closed")
        line = self.file.readline()
        value = line[:-2]
        if kind == b"+":
            return value
        if kind == b"-":
            return RespError(value.decode("utf-8", "replace"))
        if kind == b":":
            return int(value)
        if kind == b"$":
            size = int(value)
            if size == -1:
                return None
            data = self.file.read(size)
            assert self.file.read(2) == b"\r\n"
            return data
        if kind == b"*":
            count = int(value)
            return None if count == -1 else [self.read() for _ in range(count)]
        raise AssertionError("unknown RESP prefix %r" % kind)


def expect(actual, wanted, label):
    if isinstance(wanted, str) and isinstance(actual, RespError):
        actual = str(actual)
    if actual != wanted:
        raise AssertionError("%s: got %r, wanted %r" % (label, actual, wanted))


def info_value(client, name):
    body = client.command("INFO", "stats")
    marker = (name + ":").encode()
    return int(body.split(marker, 1)[1].split(b"\r\n", 1)[0])


client = Conn()
mode = client.command("CONFIG", "GET", "enable-debug-command")
if mode not in ([b"enable-debug-command", b"yes"],
                [b"enable-debug-command", b"local"]):
    raise AssertionError("DEBUG test was not purpose-booted; toggle reply=%r" % (mode,))
expect(client.command("CONFIG", "SET", "enable-debug-command", "no"),
       "ERR parameter is immutable at runtime", "boot-only config")
expect(client.command("DEBUG", "SLEEP", "0"), b"OK", "DEBUG SLEEP")
expect(client.command("DEBUG", "LOADAOF"),
       "ERR appendonly is disabled", "DEBUG LOADAOF appendonly-off guard")
expect(client.command("DEBUG", "JMAP"),
       "ERR unknown subcommand or wrong number of arguments for 'debug' command",
       "DEBUG JMAP intentionally absent")

# This is deliberately non-vacuous: assert both toggle replies, then prove the expired counter is
# stationary while off and advances after the second toggle.
expect(client.command("DEBUG", "SET-ACTIVE-EXPIRE", "0"), b"OK", "expire toggle off reply")
expired_before = info_value(client, "expired_keys")
key = ("debug:expire:%d" % os.getpid()).encode()
expect(client.command("SET", key, "value", "PX", "100"), b"OK", "expiring SET")
time.sleep(0.35)
expect(info_value(client, "expired_keys"), expired_before, "active expiry stayed off")
expect(client.command("DEBUG", "SET-ACTIVE-EXPIRE", "1"), b"OK", "expire toggle on reply")
deadline = time.monotonic() + 3
while info_value(client, "expired_keys") == expired_before and time.monotonic() < deadline:
    time.sleep(0.02)
if info_value(client, "expired_keys") <= expired_before:
    raise AssertionError("active expiry did not fire after asserted enable reply")

for atomic in (0, 1):
    expect(client.command("CONFIG", "SET", "atomic", str(atomic)), b"OK",
           "atomic=%d" % atomic)
    expect(client.command("FLUSHDB"), b"OK", "flush atomic=%d" % atomic)
    prefix = "debug:reload:%d:" % atomic
    expected = {}
    for index in range(96):
        name = (prefix + "s:%d" % index).encode()
        value = ("value-%d-" % index).encode() + b"x" * (index * 23)
        expect(client.command("SET", name, value), b"OK", "populate string")
        expected[name] = value
    expect(client.command("HSET", prefix + "hash", "a", "1", "b", "2"), 2, "hash")
    expect(client.command("RPUSH", prefix + "list", "x", "y", "z"), 3, "list")
    expect(client.command("SADD", prefix + "set", "m", "n"), 2, "set")
    expect(client.command("ZADD", prefix + "zset", "1", "one", "2", "two"), 2, "zset")
    before = client.command("DBSIZE", "NOW")
    expect(client.command("DEBUG", "RELOAD"), b"OK", "reload atomic=%d" % atomic)
    expect(client.command("DBSIZE", "NOW"), before, "dbsize round trip atomic=%d" % atomic)
    for name, value in expected.items():
        expect(client.command("GET", name), value, "string round trip")
    expect(client.command("HGETALL", prefix + "hash"), [b"a", b"1", b"b", b"2"], "hash round trip")
    expect(client.command("LRANGE", prefix + "list", "0", "-1"), [b"x", b"y", b"z"], "list round trip")
    expect(set(client.command("SMEMBERS", prefix + "set")), {b"m", b"n"}, "set round trip")
    expect(client.command("ZRANGE", prefix + "zset", "0", "-1", "WITHSCORES"),
           [b"one", b"1", b"two", b"2"], "zset round trip")

client.file.close()
client.sock.close()
print("debug: PASS (toggle fired, LOADAOF off guard, mixed snapshot RELOAD atomic=0/1)")

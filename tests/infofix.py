#!/usr/bin/env python3
"""Directed INFO truthfulness battery.

Usage: tests/infofix.py HOST PORT

The byte-counter checks account for the observer's own wire traffic exactly. That gives each
counter a zero-delta control before PING supplies a known positive delta.
"""

import socket
import sys
import time


HOST, PORT = sys.argv[1], int(sys.argv[2])
failures = []
checks = 0


def encode(*args):
    out = [b"*%d\r\n" % len(args)]
    for arg in args:
        if isinstance(arg, str):
            arg = arg.encode()
        out.append(b"$%d\r\n" % len(arg) + arg + b"\r\n")
    return b"".join(out)


class Conn:
    def __init__(self):
        self.sock = socket.create_connection((HOST, PORT), timeout=15)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.file = self.sock.makefile("rb")

    def send(self, *args):
        request = encode(*args)
        self.sock.sendall(request)
        raw, value = self.read()
        return request, raw, value

    def cmd(self, *args):
        return self.send(*args)[2]

    def read(self):
        line = self.file.readline()
        if not line:
            raise EOFError("server closed the connection")
        marker, payload = line[:1], line[1:-2]
        if marker == b"+":
            return line, payload.decode("latin1")
        if marker == b"-":
            return line, RuntimeError(payload.decode("latin1"))
        if marker == b":":
            return line, int(payload)
        if marker in (b"$", b"=", b"!"):
            size = int(payload)
            if size == -1:
                return line, None
            tail = self.file.read(size + 2)
            return line + tail, tail[:-2].decode("latin1")
        if marker in (b"*", b"~", b">"):
            count = int(payload)
            if count == -1:
                return line, None
            raw, values = line, []
            for _ in range(count):
                child_raw, child = self.read()
                raw += child_raw
                values.append(child)
            return raw, values
        if marker == b"%":
            raw, values = line, []
            for _ in range(int(payload) * 2):
                child_raw, child = self.read()
                raw += child_raw
                values.append(child)
            return raw, values
        if marker == b"_":
            return line, None
        raise AssertionError("unexpected RESP marker %r" % line[:16])

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def check(label, got, want):
    global checks
    checks += 1
    ok = want(got) if callable(want) else got == want
    if not ok:
        failures.append("%s: got %r want %r" % (label, got, want))
        print("  FAIL %-46s got=%r" % (label, got))
    return ok


def info(c, section):
    body = c.cmd("INFO", section)
    if not isinstance(body, str):
        raise AssertionError("INFO %s returned %r" % (section, body))
    rows = {}
    for line in body.split("\r\n"):
        if ":" in line:
            name, value = line.split(":", 1)
            rows[name] = value
    return rows


def as_int(rows, name):
    if name not in rows:
        failures.append("missing INFO field %s" % name)
        return -1
    try:
        return int(rows[name])
    except ValueError:
        failures.append("non-integer INFO field %s=%r" % (name, rows[name]))
        return -1


def placeholder_and_commandstats(c):
    print("truthful row inventory + commandstats")
    c.cmd("FLUSHALL")
    c.cmd("SET", "ifx:ttl", "v", "PX", "60000")
    persistence = info(c, "persistence")
    keyspace = info(c, "keyspace")
    check("unsupported delayed fsync row omitted", "aof_delayed_fsync" in persistence, False)
    check("db0 was emitted", "db0" in keyspace, True)
    check("unsupported avg_ttl member omitted", "avg_ttl=" in keyspace.get("db0", ""), False)

    c.cmd("CONFIG", "RESETSTAT")
    for _ in range(4):
        c.cmd("PING")
    commandstats = info(c, "commandstats")
    ping = commandstats.get("cmdstat_ping", "")
    members = dict(item.split("=", 1) for item in ping.split(",") if "=" in item)
    check("cmdstat PING fired", int(members.get("calls", "0")), lambda value: value >= 4)
    check("cmdstat emits only measured members", set(members), {"calls"})


def memory_peak(c):
    print("monotonic object-memory high-water mark")
    c.cmd("FLUSHALL")
    c.cmd("CONFIG", "RESETSTAT")
    base = info(c, "memory")
    c.cmd("SET", "ifx:peak", "x" * 512000)
    high = info(c, "memory")
    c.cmd("DEL", "ifx:peak")
    low = info(c, "memory")
    base_used = as_int(base, "used_memory")
    high_used = as_int(high, "used_memory")
    low_used = as_int(low, "used_memory")
    high_peak = as_int(high, "used_memory_peak")
    low_peak = as_int(low, "used_memory_peak")
    check("allocation changed measured memory", high_used, lambda value: value > base_used)
    check("delete lowered current memory", low_used, lambda value: value < high_used)
    check("peak observed allocation", high_peak, lambda value: value >= high_used)
    check("peak did not fall after delete", low_peak, lambda value: value >= high_peak)

    c.cmd("CONFIG", "RESETSTAT")
    reset = info(c, "memory")
    check("RESETSTAT re-based peak", as_int(reset, "used_memory_peak"),
          as_int(reset, "used_memory"))


def auth_reset(c):
    print("RESETSTAT auth baseline")
    check("arm requirepass", c.cmd("CONFIG", "SET", "requirepass", "ifx-secret"), "OK")
    unauth = Conn()
    denied = unauth.cmd("AUTH", "wrong-secret")
    unauth.close()
    check("wrong AUTH rejected", denied,
          lambda value: isinstance(value, RuntimeError) and
          "invalid username-password" in str(value))
    before = as_int(info(c, "stats"), "auth_failures")
    check("auth failure counter fired", before, lambda value: value > 0)
    c.cmd("CONFIG", "RESETSTAT")
    after = as_int(info(c, "stats"), "auth_failures")
    check("RESETSTAT cleared auth failures", after, 0)
    check("disarm requirepass", c.cmd("CONFIG", "SET", "requirepass", ""), "OK")


def wire_bytes_and_rate():
    print("IO-owned wire bytes + sampled operation rate")
    c = Conn()
    _, reset_raw, reset_reply = c.send("CONFIG", "RESETSTAT")
    check("wire RESETSTAT", reset_reply, "OK")
    info_req, first_raw, first_body = c.send("INFO", "STATS")
    first = {line.split(":", 1)[0]: line.split(":", 1)[1]
             for line in first_body.split("\r\n") if ":" in line}
    # The reset request was consumed before its baseline snapshot. Its +OK was sent afterward.
    check("input zero-delta control", as_int(first, "total_net_input_bytes"), len(info_req))
    check("output zero-delta control", as_int(first, "total_net_output_bytes"), len(reset_raw))

    ping_req, pong_raw, pong = c.send("PING")
    check("wire PING", pong, "PONG")
    _, _, second_body = c.send("INFO", "STATS")
    second = {line.split(":", 1)[0]: line.split(":", 1)[1]
              for line in second_body.split("\r\n") if ":" in line}
    expected_input = len(info_req) + len(ping_req) + len(info_req)
    expected_output = len(reset_raw) + len(first_raw) + len(pong_raw)
    check("input counter exact after PING", as_int(second, "total_net_input_bytes"), expected_input)
    check("output counter exact after PING", as_int(second, "total_net_output_bytes"), expected_output)

    c.cmd("CONFIG", "RESETSTAT")
    time.sleep(0.15)
    control = as_int(info(c, "stats"), "instantaneous_ops_per_sec")
    check("ops/sec idle control", control, 0)
    payload = encode("PING") * 5000
    c.sock.sendall(payload)
    for _ in range(5000):
        _, reply = c.read()
        if reply != "PONG":
            raise AssertionError("pipeline PING returned %r" % reply)
    time.sleep(0.12)
    loaded = as_int(info(c, "stats"), "instantaneous_ops_per_sec")
    check("ops/sec sampler fired", loaded, lambda value: value > 0)
    c.cmd("CONFIG", "RESETSTAT")
    time.sleep(0.15)
    check("ops/sec RESETSTAT control", as_int(info(c, "stats"), "instantaneous_ops_per_sec"), 0)
    c.close()


main = Conn()
try:
    # Recover from an interrupted earlier run before beginning the authenticated portion.
    main.cmd("CONFIG", "SET", "requirepass", "")
    placeholder_and_commandstats(main)
    memory_peak(main)
    auth_reset(main)
finally:
    try:
        main.cmd("CONFIG", "SET", "requirepass", "")
    except Exception:
        pass
    main.close()

wire_bytes_and_rate()

if failures:
    print("INFOFIX: FAIL (%d/%d checks)" % (len(failures), checks))
    for failure in failures:
        print(" - " + failure)
    raise SystemExit(1)
print("INFOFIX: PASS (%d checks)" % checks)

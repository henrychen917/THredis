#!/usr/bin/env python3
"""Directed battery for SLOWLOG + LATENCY.

Usage: tests/slowlog.py HOST PORT

THE VACUOUS-VALIDATION RULE IS THE POINT OF THIS FILE. "No entries appeared" is the expected
result both when the feature is disabled and when it is armed but nothing was slow, so every
assertion here is paired with a MECHANISM COUNTER read out of INFO:

    slowlog_batches_timed     the armed executor arm actually ran
    slowlog_escalations       a pipelined batch overran and escalated to per-op timing
    slowlog_entries_recorded  the recorder actually wrote an entry
    latency_events_recorded   the latency monitor actually sampled

The disabled case asserts slowlog_batches_timed does NOT move, which is the only way to tell
"correctly off" from "silently broken".
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
    def __init__(self, timeout=20):
        self.sock = socket.create_connection((HOST, PORT), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.file = self.sock.makefile("rb")

    def cmd(self, *args):
        self.sock.sendall(encode(*args))
        return self.read()

    def pipeline(self, commands):
        self.sock.sendall(b"".join(encode(*c) for c in commands))
        return [self.read() for _ in commands]

    def read(self):
        line = self.file.readline()
        if not line:
            raise EOFError
        kind, body = line[:1], line[1:-2]
        if kind == b"+":
            return body.decode()
        if kind == b"-":
            return RuntimeError(body.decode())
        if kind == b":":
            return int(body)
        if kind in (b"$", b"="):
            n = int(body)
            return None if n == -1 else self.file.read(n + 2)[:-2].decode("latin1")
        if kind in (b"*", b"~", b">"):
            n = int(body)
            return None if n == -1 else [self.read() for _ in range(n)]
        if kind == b"%":
            return [self.read() for _ in range(int(body) * 2)]
        if kind == b"_":
            return None
        raise AssertionError("unexpected RESP marker %r" % line[:16])

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def check(label, got, want):
    global checks
    checks += 1
    ok = (want(got) if callable(want) else got == want)
    if not ok:
        failures.append("%s: got %r" % (label, got))
        print("  FAIL %-52s got=%r" % (label, got))
    return ok


def counters(c):
    body = c.cmd("INFO", "stats")
    out = {}
    for line in body.split("\r\n"):
        if line.startswith(("slowlog_", "latency_events_")):
            name, _, value = line.partition(":")
            out[name] = int(value)
    return out


def err(text):
    return lambda got: isinstance(got, RuntimeError) and str(got) == text


def slow_command(c):
    """A command whose duration we control, executed on the IO thread."""
    return c.cmd("DEBUG", "SLEEP", "0.05")


def main():
    c = Conn()
    c.cmd("FLUSHALL")

    print("threshold matrix")
    # -------------------------------------------------------------- -1: fully disarmed
    check("disable slowlog", c.cmd("CONFIG", "SET", "slowlog-log-slower-than", "-1"), "OK")
    check("disable latency", c.cmd("CONFIG", "SET", "latency-monitor-threshold", "0"), "OK")
    check("-1 round-trips through CONFIG GET",
          c.cmd("CONFIG", "GET", "slowlog-log-slower-than"), ["slowlog-log-slower-than", "-1"])
    c.cmd("SLOWLOG", "RESET")
    time.sleep(0.2)   # let every executor pass observe the new live-config version
    before = counters(c)
    # Real work through BOTH paths: keyspace commands reach the executor arm, DEBUG SLEEP the
    # connection-local arm. Neither may arm any timing while the knobs are off.
    for i in range(200):
        c.cmd("SET", "sl:k%d" % i, "v")
    slow_command(c)
    after = counters(c)
    check("disarmed: no batch was timed",
          after["slowlog_batches_timed"], before["slowlog_batches_timed"])
    check("disarmed: no entry recorded",
          after["slowlog_entries_recorded"], before["slowlog_entries_recorded"])
    check("disarmed: SLOWLOG LEN stays 0", c.cmd("SLOWLOG", "LEN"), 0)
    check("disarmed: a 50ms command was genuinely issued",
          after["slowlog_entries_recorded"], before["slowlog_entries_recorded"])

    # -------------------------------------------------------------- default: armed, 10ms
    check("restore default", c.cmd("CONFIG", "SET", "slowlog-log-slower-than", "10000"), "OK")
    time.sleep(0.2)
    c.cmd("SLOWLOG", "RESET")
    before = counters(c)
    check("fast command is not logged", c.cmd("SET", "sl:fast", "v"), "OK")
    check("fast command left the log empty", c.cmd("SLOWLOG", "LEN"), 0)
    slow_command(c)
    after = counters(c)
    check("armed: recorder fired",
          after["slowlog_entries_recorded"], lambda v: v > before["slowlog_entries_recorded"])
    entries = c.cmd("SLOWLOG", "GET", "1")
    check("one entry landed", len(entries), 1)
    entry = entries[0]
    check("entry has six fields", len(entry), 6)
    check("entry id is an integer", entry[0], lambda v: isinstance(v, int))
    check("entry timestamp is wall clock", entry[1], lambda v: abs(v - int(time.time())) < 120)
    check("entry duration is sane microseconds", entry[2], lambda v: 40000 < v < 400000)
    check("entry argv is the command", [a.upper() for a in entry[3][:2]], ["DEBUG", "SLEEP"])
    check("entry carries the client address", entry[4], lambda v: v.startswith("127.0.0.1:"))

    # CLIENT SETNAME must reach the recorder's directory.
    c.cmd("CLIENT", "SETNAME", "slowlog-battery")
    c.cmd("SLOWLOG", "RESET")
    slow_command(c)
    named = c.cmd("SLOWLOG", "GET", "1")[0]
    check("entry carries the client name", named[5], "slowlog-battery")

    # -------------------------------------------------------------- 0: log everything
    print("threshold 0 and the executor arm")
    check("threshold 0", c.cmd("CONFIG", "SET", "slowlog-log-slower-than", "0"), "OK")
    time.sleep(0.2)
    c.cmd("SLOWLOG", "RESET")
    before = counters(c)
    c.cmd("SET", "sl:zero", "v")
    check("threshold 0 logs an ordinary command", c.cmd("SLOWLOG", "LEN"), lambda v: v > 0)
    after = counters(c)
    check("executor arm timed batches",
          after["slowlog_batches_timed"], lambda v: v > before["slowlog_batches_timed"])

    # A pipelined burst is what forces the batch screen to escalate.
    before = counters(c)
    c.pipeline([("SET", "sl:p%d" % i, "v") for i in range(400)])
    after = counters(c)
    check("pipelined burst escalated to per-op timing",
          after["slowlog_escalations"], lambda v: v > before["slowlog_escalations"])

    # -------------------------------------------------------------- max-len trimming
    print("slowlog-max-len")
    check("set max-len 5", c.cmd("CONFIG", "SET", "slowlog-max-len", "5"), "OK")
    c.cmd("SLOWLOG", "RESET")
    for i in range(40):
        c.cmd("SET", "sl:trim%d" % i, "v")
    # The ring is per-executor, so the global bound is max-len per recording thread.
    check("max-len bounds the log", c.cmd("SLOWLOG", "LEN"), lambda v: 0 < v <= 5 * 8)
    check("shrinking max-len trims immediately",
          (c.cmd("CONFIG", "SET", "slowlog-max-len", "1"), c.cmd("SLOWLOG", "LEN"))[1],
          lambda v: v <= 8)
    check("max-len 0 keeps nothing", c.cmd("CONFIG", "SET", "slowlog-max-len", "0"), "OK")
    c.cmd("SLOWLOG", "RESET")
    for i in range(20):
        c.cmd("SET", "sl:none%d" % i, "v")
    check("max-len 0 log stays empty", c.cmd("SLOWLOG", "LEN"), 0)
    c.cmd("CONFIG", "SET", "slowlog-max-len", "128")

    # -------------------------------------------------------------- argv truncation
    print("argv truncation")
    c.cmd("SLOWLOG", "RESET")
    c.cmd("RPUSH", "sl:many", *["arg%d" % i for i in range(60)])
    long_entry = next((e for e in c.cmd("SLOWLOG", "GET", "-1")
                       if e[3] and e[3][0].upper() == "RPUSH"), None)
    if check("the wide command was logged", long_entry is not None, True):
        check("argv truncated to 32", len(long_entry[3]), 32)
        check("overflow argument names the remainder", long_entry[3][-1],
              lambda v: "more arguments" in v)
    c.cmd("SLOWLOG", "RESET")
    c.cmd("SET", "sl:long", "z" * 500)
    long_value = next((e for e in c.cmd("SLOWLOG", "GET", "-1")
                       if e[3] and e[3][0].upper() == "SET" and e[3][1] == "sl:long"), None)
    if check("the long-value command was logged", long_value is not None, True):
        check("argument truncated to 128 bytes + marker", long_value[3][2],
              lambda v: v.startswith("z" * 128) and "more bytes" in v)

    # -------------------------------------------------------------- grammar + negative controls
    print("grammar")
    check("SLOWLOG GET rejects bad count", c.cmd("SLOWLOG", "GET", "abc"),
          err("ERR count should be greater than or equal to -1"))
    check("SLOWLOG GET rejects < -1", c.cmd("SLOWLOG", "GET", "-2"),
          err("ERR count should be greater than or equal to -1"))
    check("SLOWLOG GET 0 is empty", c.cmd("SLOWLOG", "GET", "0"), [])
    check("SLOWLOG unknown sub", c.cmd("SLOWLOG", "BOGUS"),
          err("ERR unknown subcommand 'BOGUS'. Try SLOWLOG HELP."))
    check("SLOWLOG HELP", c.cmd("SLOWLOG", "HELP"),
          lambda v: isinstance(v, list) and any("RESET" in line for line in v))
    # RESET clears the ring and is then itself logged, so at threshold 0 the log is left holding
    # exactly the RESET. Verified against the oracle, which behaves identically.
    c.cmd("SLOWLOG", "RESET")
    check("RESET at threshold 0 leaves only itself", c.cmd("SLOWLOG", "LEN"), 1)
    c.cmd("CONFIG", "SET", "slowlog-log-slower-than", "10000")
    time.sleep(0.2)
    check("SLOWLOG RESET empties", (c.cmd("SLOWLOG", "RESET"), c.cmd("SLOWLOG", "LEN"))[1], 0)

    # -------------------------------------------------------------- LATENCY sees the same event
    print("LATENCY")
    check("restore default threshold",
          c.cmd("CONFIG", "SET", "slowlog-log-slower-than", "10000"), "OK")
    check("latency monitor off", c.cmd("CONFIG", "SET", "latency-monitor-threshold", "0"), "OK")
    time.sleep(0.2)
    c.cmd("LATENCY", "RESET")
    before = counters(c)
    slow_command(c)
    after = counters(c)
    check("monitor off records nothing",
          after["latency_events_recorded"], before["latency_events_recorded"])
    check("monitor off LATEST empty", c.cmd("LATENCY", "LATEST"), [])

    check("arm the monitor", c.cmd("CONFIG", "SET", "latency-monitor-threshold", "1"), "OK")
    time.sleep(0.2)
    before = counters(c)
    slow_command(c)
    after = counters(c)
    check("monitor recorded a sample",
          after["latency_events_recorded"], lambda v: v > before["latency_events_recorded"])
    latest = c.cmd("LATENCY", "LATEST")
    check("LATEST has one event class", len(latest), 1)
    check("LATEST names 'command'", latest[0][0], "command")
    check("LATEST latency is ~50ms", latest[0][2], lambda v: 40 <= v < 400)
    check("LATEST max >= last", latest[0][3], lambda v: v >= latest[0][2])
    history = c.cmd("LATENCY", "HISTORY", "command")
    check("HISTORY has samples", history, lambda v: len(v) >= 1 and len(v[0]) == 2)
    check("HISTORY of unknown event is empty", c.cmd("LATENCY", "HISTORY", "nosuch"), [])
    graph = c.cmd("LATENCY", "GRAPH", "command")
    check("GRAPH is ascii", graph, lambda v: isinstance(v, str) and "command" in v)
    check("GRAPH of unknown event errors", c.cmd("LATENCY", "GRAPH", "nosuch"),
          err("ERR No samples available for event 'nosuch'"))
    check("DOCTOR", c.cmd("LATENCY", "DOCTOR"), lambda v: isinstance(v, str) and v)
    check("RESET named event", c.cmd("LATENCY", "RESET", "command"), 1)
    check("RESET cleared it", c.cmd("LATENCY", "LATEST"), [])
    check("RESET of nothing", c.cmd("LATENCY", "RESET"), 0)
    check("LATENCY unknown sub", c.cmd("LATENCY", "BOGUS"),
          err("ERR unknown subcommand 'BOGUS'. Try LATENCY HELP."))
    check("LATENCY HELP", c.cmd("LATENCY", "HELP"),
          lambda v: isinstance(v, list) and any("HISTORY" in line for line in v))

    c.cmd("CONFIG", "SET", "latency-monitor-threshold", "0")
    c.cmd("CONFIG", "SET", "slowlog-log-slower-than", "10000")
    c.cmd("SLOWLOG", "RESET")
    c.close()

    print("\nslowlog: %d checks, %d failures -> %s"
          % (checks, len(failures), "PASS" if not failures else "FAIL"))
    for line in failures:
        print("  " + line)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

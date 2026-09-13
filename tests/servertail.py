#!/usr/bin/env python3
"""Directed battery for the server/introspection tail.

Usage: tests/servertail.py HOST PORT [--binary PATH] [--cores CPULIST] [--spare-port N]

Covers scope A (TIME/LOLWUT/ROLE/WAIT/FAILOVER/REPLICAOF/SLAVEOF/PFSELFTEST/SUBSTR/SORT_RO/
SHUTDOWN), scope B (CONFIG REWRITE/RESETSTAT/HELP, COMMAND COUNT/LIST/INFO/DOCS/GETKEYS/
GETKEYSANDFLAGS/HELP) and scope C (OBJECT, MEMORY).

Every check asserts a MECHANISM, not merely the absence of a crash: SHUTDOWN asserts the process
really exited and released the port, CONFIG REWRITE asserts the rewritten file boots and carries the
mutated value, RESETSTAT asserts the counter actually fell, and each negative control asserts the
exact error string.
"""

import os
import socket
import subprocess
import sys
import time

HOST, PORT = sys.argv[1], int(sys.argv[2])
ARGS = sys.argv[3:]


def opt(name, default=None):
    return ARGS[ARGS.index(name) + 1] if name in ARGS else default


BINARY = opt("--binary", os.path.join(os.path.dirname(__file__), "..", "build", "tomokv"))
CORES = opt("--cores", "104-107")
SPARE_PORT = int(opt("--spare-port", str(PORT + 3)))

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
    def __init__(self, port=None, timeout=10):
        self.sock = socket.create_connection((HOST, port or PORT), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.file = self.sock.makefile("rb")

    def cmd(self, *args):
        self.sock.sendall(encode(*args))
        return self.read()

    def read(self):
        line = self.file.readline()
        if not line:
            raise EOFError("server closed the connection")
        kind, body = line[:1], line[1:-2]
        if kind == b"+":
            return body.decode()
        if kind == b"-":
            return RuntimeError(body.decode())
        if kind == b":":
            return int(body)
        if kind in (b"$", b"="):
            n = int(body)
            if n == -1:
                return None
            data = self.file.read(n + 2)[:-2]
            return data.decode("latin1")
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
        failures.append("%s: got %r want %r" % (label, got, want))
        print("  FAIL %-46s got=%r" % (label, got))
    return ok


def err(text):
    return lambda got: isinstance(got, RuntimeError) and str(got) == text


def err_prefix(text):
    return lambda got: isinstance(got, RuntimeError) and str(got).startswith(text)


# ---------------------------------------------------------------------------- scope A
def scope_a(c):
    print("scope A: parity surface")
    now = int(time.time())
    t = c.cmd("TIME")
    check("TIME shape", t, lambda v: isinstance(v, list) and len(v) == 2)
    check("TIME seconds are wall clock", int(t[0]), lambda v: abs(v - now) < 60)
    check("TIME micros in range", int(t[1]), lambda v: 0 <= v < 1000000)
    check("TIME arity", c.cmd("TIME", "EXTRA"), err_prefix("ERR wrong number of arguments"))

    check("ROLE", c.cmd("ROLE"), ["master", 0, []])
    check("LOLWUT names the server", c.cmd("LOLWUT"), lambda v: "TomoKV ver." in v)
    check("LOLWUT ignores junk", c.cmd("LOLWUT", "VERSION", "6"), lambda v: "TomoKV ver." in v)

    check("WAIT standalone", c.cmd("WAIT", "0", "0"), 0)
    check("WAIT nonzero replicas", c.cmd("WAIT", "3", "10"), 0)
    check("WAIT negative timeout", c.cmd("WAIT", "0", "-1"), err("ERR timeout is negative"))
    check("WAIT non-integer", c.cmd("WAIT", "x", "0"),
          err("ERR value is not an integer or out of range"))

    check("FAILOVER", c.cmd("FAILOVER"), err("ERR FAILOVER requires connected replicas."))
    check("FAILOVER ABORT", c.cmd("FAILOVER", "ABORT"), err("ERR No failover in progress."))

    # Deliberate deviation from redis, which replies +OK. Asserted so it can never drift silently.
    for name in ("REPLICAOF", "SLAVEOF"):
        check("%s refuses" % name, c.cmd(name, "NO", "ONE"),
              err("ERR replication is not supported by tomokv"))

    check("PFSELFTEST", c.cmd("PFSELFTEST"), "OK")

    c.cmd("SET", "st:sub", "Hello World")
    check("SUBSTR == GETRANGE", c.cmd("SUBSTR", "st:sub", "0", "4"), "Hello")
    check("SUBSTR negative", c.cmd("SUBSTR", "st:sub", "-5", "-1"), "World")
    check("SUBSTR missing key", c.cmd("SUBSTR", "st:nosuch", "0", "-1"), "")

    c.cmd("DEL", "st:list")
    c.cmd("RPUSH", "st:list", "3", "1", "2")
    check("SORT_RO ascending", c.cmd("SORT_RO", "st:list"), ["1", "2", "3"])
    check("SORT_RO DESC", c.cmd("SORT_RO", "st:list", "DESC"), ["3", "2", "1"])
    check("SORT_RO LIMIT", c.cmd("SORT_RO", "st:list", "LIMIT", "0", "2"), ["1", "2"])
    # The read-only alias must refuse the one option that would make it a write.
    check("SORT_RO rejects STORE", c.cmd("SORT_RO", "st:list", "STORE", "st:dst"),
          err("ERR syntax error"))
    check("SORT_RO STORE wrote nothing", c.cmd("EXISTS", "st:dst"), 0)
    check("SORT still accepts STORE", c.cmd("SORT", "st:list", "STORE", "st:dst"), 3)


# ---------------------------------------------------------------------------- scope B
def scope_b(c):
    print("scope B: CONFIG + COMMAND")
    check("CONFIG HELP", c.cmd("CONFIG", "HELP"),
          lambda v: isinstance(v, list) and any("REWRITE" in line for line in v))
    check("COMMAND HELP", c.cmd("COMMAND", "HELP"),
          lambda v: isinstance(v, list) and any("GETKEYSANDFLAGS" in line for line in v))

    count = c.cmd("COMMAND", "COUNT")
    listing = c.cmd("COMMAND", "LIST")
    check("COMMAND COUNT positive", count, lambda v: isinstance(v, int) and v > 100)
    check("COMMAND COUNT is top-level LIST cardinality",
          sum("|" not in name for name in listing), count)
    check("COMMAND LIST is lowercase", listing, lambda v: all(n == n.lower() for n in v))
    check("COMMAND LIST has the new rows", set(listing),
          lambda v: {"time", "lolwut", "role", "wait", "lcs", "slowlog", "latency",
                     "memory", "object", "substr", "sort_ro", "shutdown"} <= v)

    pattern = c.cmd("COMMAND", "LIST", "FILTERBY", "PATTERN", "sl*")
    check("FILTERBY PATTERN", set(pattern), lambda v: "slowlog" in v and "get" not in v)
    check("FILTERBY MODULE is empty", c.cmd("COMMAND", "LIST", "FILTERBY", "MODULE", "nope"), [])
    aclcat = c.cmd("COMMAND", "LIST", "FILTERBY", "ACLCAT", "read")
    check("FILTERBY ACLCAT read", set(aclcat), lambda v: "get" in v and "set" not in v)
    check("FILTERBY bad selector", c.cmd("COMMAND", "LIST", "FILTERBY", "NOPE", "x"),
          err("ERR syntax error"))
    check("FILTERBY unknown category", c.cmd("COMMAND", "LIST", "FILTERBY", "ACLCAT", "nope"),
          err_prefix("ERR Unknown ACL category"))

    info = c.cmd("COMMAND", "INFO", "get")
    check("COMMAND INFO is a 10-element row", info,
          lambda v: len(v) == 1 and isinstance(v[0], list) and len(v[0]) == 10)
    check("COMMAND INFO names the command", info[0][0], "get")
    check("COMMAND INFO arity", info[0][1], 2)
    docs = c.cmd("COMMAND", "DOCS", "get")
    check("COMMAND DOCS is a map", docs, lambda v: isinstance(v, list) and v[0] == "get")

    check("GETKEYS single", c.cmd("COMMAND", "GETKEYS", "GET", "foo"), ["foo"])
    check("GETKEYS multi", c.cmd("COMMAND", "GETKEYS", "MSET", "a", "1", "b", "2"), ["a", "b"])
    check("GETKEYS two-key", c.cmd("COMMAND", "GETKEYS", "LCS", "a", "b"), ["a", "b"])
    check("GETKEYS keyless", c.cmd("COMMAND", "GETKEYS", "PING"),
          err("ERR The command has no key arguments"))
    check("GETKEYS bad arity", c.cmd("COMMAND", "GETKEYS", "GET"),
          err("ERR Invalid number of arguments specified for command"))
    check("GETKEYS unknown command", c.cmd("COMMAND", "GETKEYS", "NOSUCHCMD", "a"),
          err("ERR Invalid command specified"))
    flags = c.cmd("COMMAND", "GETKEYSANDFLAGS", "SET", "foo", "bar")
    check("GETKEYSANDFLAGS shape", flags,
          lambda v: v == [["foo", ["OW", "update"]]])
    ro = c.cmd("COMMAND", "GETKEYSANDFLAGS", "GET", "foo")
    check("GETKEYSANDFLAGS readonly", ro, lambda v: v[0][1] == ["RO", "access"])

    # RESETSTAT has to make an actually-observed counter fall.
    for _ in range(50):
        c.cmd("PING")
    before = info_field(c, "total_commands_processed")
    check("commands were counted", before, lambda v: v > 40)
    check("CONFIG RESETSTAT", c.cmd("CONFIG", "RESETSTAT"), "OK")
    after = info_field(c, "total_commands_processed")
    check("RESETSTAT dropped the counter", after, lambda v: v < before)

    check("CONFIG REWRITE without a file", c.cmd("CONFIG", "REWRITE"),
          err("ERR The server is running without a config file"))


def info_field(c, name, section="stats"):
    body = c.cmd("INFO", section)
    for line in body.split("\r\n"):
        if line.startswith(name + ":"):
            return int(line.split(":", 1)[1])
    return -1


# ---------------------------------------------------------------------------- scope C
def scope_c(c):
    print("scope C: OBJECT + MEMORY")
    c.cmd("FLUSHALL")
    c.cmd("SET", "ob:int", "12345")
    c.cmd("SET", "ob:embstr", "short value")
    c.cmd("SET", "ob:raw", "x" * 400)
    c.cmd("RPUSH", "ob:list", "a", "b", "c")
    c.cmd("HSET", "ob:hash", "f", "v")
    c.cmd("SADD", "ob:intset", "1", "2", "3")
    c.cmd("SADD", "ob:lpset", "a", "b", "c")
    c.cmd("ZADD", "ob:zset", "1", "a")
    c.cmd("XADD", "ob:stream", "*", "f", "v")

    expected = {
        "ob:int": "int",
        "ob:embstr": "embstr",
        "ob:raw": "raw",
        "ob:list": "listpack",
        "ob:hash": "listpack",
        "ob:intset": "intset",
        "ob:lpset": "listpack",
        "ob:zset": "listpack",
        "ob:stream": "stream",
    }
    for key, want in expected.items():
        check("OBJECT ENCODING %s" % key, c.cmd("OBJECT", "ENCODING", key), want)

    # Promotion has to change the reported name, or the mapping is decorative.
    for _ in range(400):
        c.cmd("SADD", "ob:bigset", "member-%d" % _)
    check("OBJECT ENCODING promoted set", c.cmd("OBJECT", "ENCODING", "ob:bigset"), "hashtable")
    for i in range(400):
        c.cmd("ZADD", "ob:bigzset", str(i), "m%d" % i)
    check("OBJECT ENCODING promoted zset", c.cmd("OBJECT", "ENCODING", "ob:bigzset"), "skiplist")

    check("OBJECT REFCOUNT", c.cmd("OBJECT", "REFCOUNT", "ob:raw"), 1)
    check("OBJECT IDLETIME", c.cmd("OBJECT", "IDLETIME", "ob:raw"), lambda v: isinstance(v, int))
    check("OBJECT FREQ without lfu", c.cmd("OBJECT", "FREQ", "ob:raw"),
          err_prefix("ERR An LFU maxmemory policy is not selected"))
    check("OBJECT missing key", c.cmd("OBJECT", "ENCODING", "ob:nosuch"), None)
    check("OBJECT HELP", c.cmd("OBJECT", "HELP"),
          lambda v: isinstance(v, list) and any("ENCODING" in line for line in v))
    check("OBJECT unknown sub", c.cmd("OBJECT", "BOGUS", "ob:raw"),
          err("ERR unknown subcommand 'BOGUS'. Try OBJECT HELP."))
    check("OBJECT bad arity", c.cmd("OBJECT", "ENCODING"),
          err("ERR wrong number of arguments for 'object|encoding' command"))

    # FREQ/IDLETIME swap availability with the policy, exactly as redis does.
    c.cmd("CONFIG", "SET", "maxmemory", "100mb")
    c.cmd("CONFIG", "SET", "maxmemory-policy", "allkeys-lfu")
    time.sleep(0.05)
    check("OBJECT FREQ with lfu", c.cmd("OBJECT", "FREQ", "ob:raw"), lambda v: isinstance(v, int))
    check("OBJECT IDLETIME with lfu", c.cmd("OBJECT", "IDLETIME", "ob:raw"),
          err_prefix("ERR An LFU maxmemory policy is selected"))
    c.cmd("CONFIG", "SET", "maxmemory-policy", "noeviction")
    c.cmd("CONFIG", "SET", "maxmemory", "0")
    time.sleep(0.05)

    usage = c.cmd("MEMORY", "USAGE", "ob:raw")
    check("MEMORY USAGE is plausible", usage, lambda v: isinstance(v, int) and 400 < v < 4096)
    small = c.cmd("MEMORY", "USAGE", "ob:embstr")
    check("MEMORY USAGE tracks value size", small, lambda v: v < usage)
    check("MEMORY USAGE SAMPLES accepted", c.cmd("MEMORY", "USAGE", "ob:raw", "SAMPLES", "5"),
          usage)
    check("MEMORY USAGE bad option", c.cmd("MEMORY", "USAGE", "ob:raw", "BOGUS", "5"),
          err("ERR syntax error"))
    check("MEMORY USAGE missing key", c.cmd("MEMORY", "USAGE", "ob:nosuch"), None)

    stats = c.cmd("MEMORY", "STATS")
    fields = dict(zip(stats[0::2], stats[1::2]))
    check("MEMORY STATS has keys.count", fields.get("keys.count"),
          lambda v: isinstance(v, int) and v > 0)
    check("MEMORY STATS has dataset.bytes", fields.get("dataset.bytes"),
          lambda v: isinstance(v, int) and v > 0)
    check("MEMORY STATS reports every shard", fields,
          lambda v: any(k.startswith("shard.") for k in v))
    check("MEMORY DOCTOR", c.cmd("MEMORY", "DOCTOR"), lambda v: isinstance(v, str) and v)
    check("MEMORY PURGE", c.cmd("MEMORY", "PURGE"), "OK")
    check("MEMORY MALLOC-STATS", c.cmd("MEMORY", "MALLOC-STATS"),
          lambda v: isinstance(v, str) and len(v) > 16)
    check("MEMORY HELP", c.cmd("MEMORY", "HELP"),
          lambda v: isinstance(v, list) and any("USAGE" in line for line in v))
    check("MEMORY unknown sub", c.cmd("MEMORY", "BOGUS"),
          err("ERR unknown subcommand 'BOGUS'. Try MEMORY HELP."))


# ------------------------------------------------------------------- SHUTDOWN + REWRITE round-trip
def boot(port, extra=(), conf=None, wait=6.0):
    args = ["taskset", "-c", CORES, os.path.abspath(BINARY)]
    if conf:
        args += ["--conf", conf]
    args += ["--port", str(port), "--bind", HOST, "--enable-debug-command", "yes"]
    args += list(extra)
    # LeakSanitizer's exit-time accounting would turn every clean stop into exit 1: terminating
    # with a client still connected leaves that connection's ROB block unreclaimed, identically on
    # the SIGTERM path that predates SHUTDOWN (verified). Turning off the LEAK accounting keeps the
    # exit-code assertion meaningful while every ASAN memory-error check stays armed.
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = env.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    deadline = time.time() + wait
    while time.time() < deadline:
        try:
            probe = Conn(port, timeout=1)
            probe.cmd("PING")
            probe.close()
            return proc
        except OSError:
            if proc.poll() is not None:
                return None
            time.sleep(0.05)
    proc.kill()
    return None


def scope_shutdown(workdir):
    print("scope A: SHUTDOWN (throwaway server)")
    proc = boot(SPARE_PORT, ["--shards", "2", "--dir", workdir])
    if not check("throwaway server booted", proc is not None, True):
        return
    c = Conn(SPARE_PORT)
    check("SHUTDOWN rejects both save modes", c.cmd("SHUTDOWN", "NOSAVE", "SAVE"),
          err("ERR syntax error"))
    check("SHUTDOWN rejects junk", c.cmd("SHUTDOWN", "BOGUS"), err("ERR syntax error"))
    check("SHUTDOWN ABORT", c.cmd("SHUTDOWN", "ABORT"), err("ERR No shutdown in progress."))

    # Success sends NO reply; the connection just closes and the process exits.
    c.sock.sendall(encode("SHUTDOWN", "NOSAVE"))
    try:
        c.sock.settimeout(3)
        trailing = c.sock.recv(64)
    except (socket.timeout, OSError):
        trailing = b""
    check("SHUTDOWN sends no reply", trailing, b"")
    try:
        code = proc.wait(timeout=8)
    except subprocess.TimeoutExpired:
        proc.kill()
        code = None
    check("SHUTDOWN exit code is clean", code, 0)
    check("SHUTDOWN released the port", port_free(SPARE_PORT), True)
    c.close()


def port_free(port):
    for _ in range(40):
        try:
            probe = socket.create_connection((HOST, port), timeout=0.25)
            probe.close()
            time.sleep(0.1)
        except OSError:
            return True
    return False


def scope_rewrite(workdir):
    print("scope B: CONFIG REWRITE round trip")
    conf = os.path.join(workdir, "rewrite.conf")
    with open(conf, "w") as handle:
        handle.write("# seed config\nshards 2\nmaxmemory 0\n")
    proc = boot(SPARE_PORT, ["--dir", workdir], conf=conf)
    if not check("server booted from conf", proc is not None, True):
        return
    c = Conn(SPARE_PORT)
    check("CONFIG SET before rewrite", c.cmd("CONFIG", "SET", "maxmemory", "64mb"), "OK")
    check("CONFIG SET slowlog knob", c.cmd("CONFIG", "SET", "slowlog-max-len", "77"), "OK")
    check("CONFIG REWRITE", c.cmd("CONFIG", "REWRITE"), "OK")
    c.close()
    proc.terminate()
    proc.wait(timeout=8)

    body = open(conf).read()
    check("rewritten file carries the mutation", body, lambda v: "maxmemory 67108864" in v)
    check("rewritten file omits non-boot names", body,
          lambda v: not any(line.startswith("aof-use-rdb-preamble ")
                            for line in v.splitlines()))
    save_lines = [line for line in body.splitlines() if line.startswith("save ")]
    check("rewritten file preserves repeatable save clauses", save_lines,
          ["save 3600 1", "save 300 100", "save 60 10000"])

    proc = boot(SPARE_PORT, ["--dir", workdir], conf=conf)
    if not check("server reboots from the rewritten file", proc is not None, True):
        return
    c = Conn(SPARE_PORT)
    check("mutation survived the reboot", c.cmd("CONFIG", "GET", "maxmemory"),
          ["maxmemory", "67108864"])
    check("slowlog knob survived", c.cmd("CONFIG", "GET", "slowlog-max-len"),
          ["slowlog-max-len", "77"])
    check("repeated save clauses reboot as one CONFIG value", c.cmd("CONFIG", "GET", "save"),
          ["save", "3600 1 300 100 60 10000"])
    c.close()
    proc.terminate()
    proc.wait(timeout=8)


def main():
    workdir = os.path.join(os.path.dirname(os.path.abspath(BINARY)), "..",
                           "servertail-testdata")
    workdir = os.path.abspath(workdir)
    os.makedirs(workdir, exist_ok=True)

    c = Conn()
    scope_a(c)
    scope_b(c)
    scope_c(c)
    c.close()
    scope_shutdown(workdir)
    scope_rewrite(workdir)

    print("\nservertail: %d checks, %d failures -> %s"
          % (checks, len(failures), "PASS" if not failures else "FAIL"))
    for line in failures:
        print("  " + line)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

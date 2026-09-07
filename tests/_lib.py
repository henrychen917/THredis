#!/usr/bin/env python3
"""Shared client, geometry and reporting helpers for tests/*.py.

`import _lib` works from any battery because python3 puts the script's own directory (tests/)
first on sys.path. Nothing here opens a server; every helper takes a live connection.

House rules this module encodes (they are rules, not preferences -- see AUDIT-TESTS.md):

  * DETERMINISTIC hooks over timing luck. `armed()` arms a DEBUG window hook and ALWAYS disarms
    it, and refuses to proceed if the arm did not answer OK (an unarmed "deterministic" arm is a
    timing arm in disguise).
  * A geometry claim is PROVED on this boot. `shard_of()` and `topology()` ask the server; nothing
    is inferred from key names or from the default shard-home map (the hash seed is drawn from the
    kernel at every boot, and the balancer can move shards).
  * "Owner" means the thread that owns a shard -- the `shard <sid> <owner_tid>` rows of DEBUG
    LBSIGNALS -- never the `ex` role label. Under --thread-mode 1s every thread is labelled `fused`;
    ownership still comes from shard rows. Every helper here behaves identically in 1s and 2s.
  * A skip is visible and carries a reason. `Report.skip(..., strict=True)` marks a skip that
    means "the mechanism could not be armed" (a DEBUG hook denied, a knob absent); under
    TOMO_GATE_STRICT=1 -- exported by tests/gate.sh -- such a skip fails the battery, so a gate row
    can never turn green by silently skipping the arm it exists for. A probabilistic discovery arm
    that found no window is `strict=False`: it is reported, never a failure.
  * `skip_all(reason)` exits 3 when a whole battery cannot apply to this boot (a 2s-only feature on
    a 1s boot). The gate's `py()` wrapper paints exit 3 red: a row that cannot run is a gate
    defect, not a pass.
"""

import contextlib
import os
import socket
import sys
import time
from collections import namedtuple

# ------------------------------------------------------------------------------------------------
# RESP client
# ------------------------------------------------------------------------------------------------


class RespError(Exception):
    """A `-ERR ...` reply. Returned by Conn.read(), never raised by it, so a reader thread keeps
    the server's exact text instead of a generic exception."""

    def __init__(self, message):
        if isinstance(message, str):
            message = message.encode()
        super().__init__(message)
        self.message = message

    def __eq__(self, other):
        return isinstance(other, RespError) and self.message == other.message

    def __hash__(self):
        return hash(self.message)

    def __repr__(self):
        return "RespError(%r)" % self.message

    def __str__(self):
        return self.message.decode("utf-8", "replace")


def encode(*args):
    """One RESP command frame. str -> utf-8, bytes as-is, everything else via str()."""
    parts = [b"*%d\r\n" % len(args)]
    for arg in args:
        if isinstance(arg, str):
            arg = arg.encode()
        elif not isinstance(arg, (bytes, bytearray)):
            arg = str(arg).encode()
        parts.append(b"$%d\r\n%s\r\n" % (len(arg), arg))
    return b"".join(parts)


class Conn:
    """A blocking RESP2/RESP3 connection with a complete reader.

    read() returns: simple strings and verbatim/bulk strings as bytes, integers as int, nulls as
    None, arrays/sets/pushes as lists, maps as FLAT [k, v, k, v] lists (order preserved, duplicate
    keys kept), doubles/big numbers/booleans as their raw bytes (`,1.5` -> b"1.5", `#t` -> b"t"),
    attributes (`|`) are consumed and discarded, and errors as RespError instances.
    """

    def __init__(self, host, port, timeout=30.0, nodelay=True, rcvbuf=None, buffering=1 << 20):
        self.host, self.port = host, int(port)
        self.sock = socket.create_connection((host, int(port)), timeout=timeout)
        self.sock.settimeout(timeout)
        if nodelay:
            self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        if rcvbuf:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
            if hasattr(socket, "TCP_WINDOW_CLAMP"):
                self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_WINDOW_CLAMP, rcvbuf)
        self.file = self.sock.makefile("rb", buffering=buffering)

    # -- transport ----------------------------------------------------------------------------
    def raw(self, payload):
        self.sock.sendall(payload)

    def send(self, *args):
        self.sock.sendall(encode(*args))

    def close(self, reset=False):
        if self.sock is None:
            return
        try:
            if reset:
                import struct
                self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            self.file.close()
            self.sock.close()
        except OSError:
            pass
        self.sock = None

    # -- reader ---------------------------------------------------------------------------------
    def _line(self):
        line = self.file.readline()
        if not line:
            raise EOFError("server closed the connection")
        if not line.endswith(b"\r\n"):
            raise AssertionError("bad RESP line (no CRLF): %r" % line[:40])
        return line[:-2]

    def read(self):
        while True:
            head = self.file.read(1)
            if not head:
                raise EOFError("server closed the connection")
            body = self._line()
            if head == b"+":
                return body
            if head == b"-":
                return RespError(body)
            if head == b":":
                return int(body)
            if head in (b"$", b"="):
                size = int(body)
                if size < 0:
                    return None
                data = self.file.read(size)
                if len(data) != size or self.file.read(2) != b"\r\n":
                    raise AssertionError("bad bulk trailer for %d-byte reply" % size)
                return data
            if head in (b"*", b"~", b">"):
                count = int(body)
                if count < 0:
                    return None
                return [self.read() for _ in range(count)]
            if head == b"%":
                count = int(body)
                if count < 0:
                    return None
                return [self.read() for _ in range(2 * count)]
            if head == b"|":                      # attribute: consume and read the real reply
                for _ in range(2 * int(body)):
                    self.read()
                continue
            if head == b"_":
                return None
            if head in (b",", b"#", b"("):
                return body
            raise AssertionError("unknown RESP marker %r" % (head + body[:20]))

    def cmd(self, *args):
        self.send(*args)
        return self.read()

    def must(self, *args):
        reply = self.cmd(*args)
        if isinstance(reply, RespError):
            raise reply
        return reply


def host_port(argv=None, default_port=6379):
    argv = sys.argv if argv is None else argv
    host = argv[1] if len(argv) > 1 else "127.0.0.1"
    port = int(argv[2]) if len(argv) > 2 else default_port
    return host, port


# ------------------------------------------------------------------------------------------------
# INFO / topology
# ------------------------------------------------------------------------------------------------


def call(conn, *args):
    """Send one command on any test's connection object: _lib.Conn (`cmd`) or the per-battery
    classes that predate it (`cmd` or `command`). Lets the geometry helpers below serve every
    battery before its client is migrated."""
    fn = getattr(conn, "cmd", None) or getattr(conn, "command", None)
    if fn is None:
        raise TypeError("connection object needs a cmd()/command() method: %r" % (conn,))
    return fn(*args)


def info(conn, section=None):
    """INFO [section] as {field: str}. Section headers are dropped."""
    raw = call(conn, "INFO", section) if section else call(conn, "INFO")
    if not isinstance(raw, bytes):
        raise AssertionError("INFO %s returned %r" % (section or "", raw))
    out = {}
    for line in raw.decode("utf-8", "replace").splitlines():
        if not line or line.startswith("#") or ":" not in line:
            continue
        key, value = line.split(":", 1)
        out[key] = value
    return out


def info_int(conn, section, key):
    """A counter the battery rests on. Absent is a FAILURE, never 0 (vacuous-validation rule)."""
    table = info(conn, section)
    if key not in table:
        raise AssertionError("INFO %s has no %s; the battery cannot prove its mechanism fired"
                             % (section, key))
    return int(table[key])


def thread_mode(conn):
    """'1s' (fused) or '2s' (split), from INFO server."""
    mode = info(conn, "server").get("thread_mode")
    if mode not in ("1s", "2s"):
        raise AssertionError("INFO server has no thread_mode field (got %r)" % (mode,))
    return mode


LbThread = namedtuple("LbThread", "tid role domain clients iterations ops busy_ns idle_ns cpu_ns "
                                  "depth_sum depth_samples full_events wakes_sent wakes_recv spins "
                                  "queue_delay_samples queue_delay_ewma_us oldest_age_us "
                                  "oldest_age_samples oldest_age_ewma_us oldest_age_min_us "
                                  "oldest_age_max_us")
LbShard = namedtuple("LbShard", "sid owner domain ops foreign migrations size obj_bytes")
LbSnapshot = namedtuple("LbSnapshot", "threads shards rollups derived raw")
Topology = namedtuple("Topology", "roles shard_owner owners mode")


def lbsignals(conn):
    """Parse DEBUG LBSIGNALS (grammar: src/core/lbsignals.h:154-166, src/cmd/lbsignals.cc:147)."""
    raw = call(conn, "DEBUG", "LBSIGNALS")
    if not isinstance(raw, bytes):
        raise AssertionError(
            "DEBUG LBSIGNALS unavailable; boot with --enable-debug-command yes: %r" % (raw,))
    text = raw.decode("utf-8", "replace")
    if not text.startswith("lbver 1"):
        raise AssertionError("DEBUG LBSIGNALS: missing 'lbver 1' header: %r" % text[:80])
    threads, shards, rollups, derived = [], [], {}, {}
    for line in text.splitlines():
        f = line.split()
        if not f:
            continue
        if f[0] == "thread":
            threads.append(LbThread(
                int(f[1]), f[2], int(f[3]), int(f[4]), int(f[5]), int(f[6]), int(f[7]), int(f[8]),
                int(f[9]), int(f[10]), int(f[11]), int(f[12]), int(f[13]), int(f[14]), int(f[15]),
                int(f[16]), float(f[17]), int(f[18]), int(f[19]), float(f[20]), int(f[21]),
                int(f[22])))
        elif f[0] == "shard":
            shards.append(LbShard(int(f[1]), int(f[2]), int(f[3]), int(f[4]), int(f[5]),
                                  int(f[6]), int(f[7]), int(f[8])))
        elif f[0] == "rollup":
            rollups[f[1]] = {
                "threads": int(f[2]), "ops": int(f[3]), "busy_ns": int(f[4]), "idle_ns": int(f[5]),
                "cpu_ns": int(f[6]), "busy_frac": float(f[7]), "ns_per_op": float(f[8]),
                "avg_depth": float(f[9]), "full_events": int(f[10]),
                "queue_delay_samples": int(f[11]), "queue_delay_ewma_us": float(f[12]),
                "oldest_age_min_us": int(f[13]), "oldest_age_max_us": int(f[14]),
                "oldest_age_ewma_us": float(f[15])}
        elif f[0] == "derived":
            derived = {f[i]: f[i + 1] for i in range(1, len(f) - 1, 2)}
    return LbSnapshot(threads, shards, rollups, derived, text)


def topology(conn):
    """Who owns which shard, from the SHARD rows (true in 1s and 2s alike).

    roles:       {tid: 'io'|'ex'|'fused'} -- informational; never select owners by it
    shard_owner: {sid: owner_tid}
    owners:      sorted distinct owner tids (the concurrency units for cross-owner races)
    mode:        '1s' | '2s'
    """
    snap = lbsignals(conn)
    if not snap.shards:
        raise AssertionError("DEBUG LBSIGNALS reported no shard rows")
    roles = {t.tid: t.role for t in snap.threads}
    shard_owner = {s.sid: s.owner for s in snap.shards}
    mode = snap.derived.get("thread_mode")
    if mode not in ("1s", "2s"):
        raise AssertionError("DEBUG LBSIGNALS has no derived thread_mode (got %r)" % (mode,))
    return Topology(roles, shard_owner, sorted(set(shard_owner.values())), mode)


def shard_of(conn, key):
    reply = call(conn, "DEBUG", "SHARD", key)
    if not isinstance(reply, int):
        raise AssertionError(
            "DEBUG SHARD unavailable; boot with --enable-debug-command yes: %r" % (reply,))
    return reply


def shards_of(conn, keys):
    """[(shard, owner_tid), ...] for keys, preserving input order in one DEBUG reply.

    This is deliberately not a placement transaction: if a FLIP overlaps the command, each row is
    truthful when read but one reply may span old and new placements. Callers needing a coherent
    topology must quiesce FLIP and compare owners with a fresh LBSIGNALS shard-row map.
    """
    keys = list(keys)
    if not keys:
        return []
    reply = call(conn, "DEBUG", "SHARDS", *keys)
    if not isinstance(reply, list) or len(reply) != len(keys):
        raise AssertionError(
            "DEBUG SHARDS unavailable or malformed for %d keys: %r" % (len(keys), reply))
    rows = []
    for index, pair in enumerate(reply):
        if (not isinstance(pair, list) or len(pair) != 2 or
                not isinstance(pair[0], int) or not isinstance(pair[1], int)):
            raise AssertionError("DEBUG SHARDS row %d is not [sid, owner_tid]: %r"
                                 % (index, pair))
        rows.append((pair[0], pair[1]))
    return rows


def probe_keys(conn, prefix, topo, limit=8000, width=4, batch_size=256):
    """Yield (key, shard, owner) for candidates, resolving each batch in one DEBUG reply."""
    if batch_size <= 0:
        raise ValueError("batch_size must be positive")
    fmt = "%s:%0" + str(width) + "d"
    for first in range(0, limit, batch_size):
        keys = [fmt % (prefix, index)
                for index in range(first, min(limit, first + batch_size))]
        for key, (shard, owner) in zip(keys, shards_of(conn, keys)):
            if shard not in topo.shard_owner:
                raise AssertionError(
                    "DEBUG SHARDS returned shard %d that LBSIGNALS did not report" % shard)
            expected_owner = topo.shard_owner[shard]
            if owner != expected_owner:
                raise AssertionError(
                    "DEBUG SHARDS owner %d for shard %d disagrees with LBSIGNALS owner %d; "
                    "placement may have changed during the probes"
                    % (owner, shard, expected_owner))
            yield key, shard, owner


def owner_buckets(conn, prefix, want_owners=2, per_owner=1, limit=8000, topo=None):
    """{owner_tid: [keys]} with at least `want_owners` distinct OWNERS holding >= `per_owner` keys
    each. Owners, not shards: two shards on one executor are one concurrency unit."""
    topo = topo or topology(conn)
    if len(topo.owners) < want_owners:
        raise AssertionError(
            "geometry needs %d distinct shard owners; this boot has %d (%s, thread-mode %s)"
            % (want_owners, len(topo.owners), topo.owners, topo.mode))
    by_owner = {}
    for key, _shard, owner in probe_keys(conn, prefix, topo, limit):
        by_owner.setdefault(owner, []).append(key)
        if sum(1 for keys in by_owner.values() if len(keys) >= per_owner) >= want_owners:
            return by_owner
    counts = {owner: len(keys) for owner, keys in by_owner.items()}
    raise AssertionError("could not find %d owners x %d keys in %d probes: %r"
                         % (want_owners, per_owner, limit, counts))


def cross_owner_pair(conn, prefix, topo=None):
    """(key_a, key_b, owner_a, owner_b) proven to live on two different OWNERS."""
    buckets = owner_buckets(conn, prefix, want_owners=2, per_owner=1, topo=topo)
    owners = sorted(owner for owner, keys in buckets.items() if keys)[:2]
    return buckets[owners[0]][0], buckets[owners[1]][0], owners[0], owners[1]


def same_shard_pair(conn, prefix, limit=8000, topo=None):
    """(key_a, key_b, shard): two keys on ONE physical shard (the localfast geometry)."""
    topo = topo or topology(conn)
    by_shard = {}
    for key, shard, _owner in probe_keys(conn, prefix, topo, limit):
        by_shard.setdefault(shard, []).append(key)
        if len(by_shard[shard]) == 2:
            return by_shard[shard][0], by_shard[shard][1], shard
    raise AssertionError("no same-shard key pair in %d probes" % limit)


# ------------------------------------------------------------------------------------------------
# timing helpers
# ------------------------------------------------------------------------------------------------


def wait_until(predicate, timeout, interval=0.02):
    """Poll `predicate()` until true or `timeout` seconds pass. Returns the last predicate value."""
    deadline = time.monotonic() + timeout
    result = predicate()
    while not result and time.monotonic() < deadline:
        time.sleep(interval)
        result = predicate()
    return result


def pipelined_rate(conn, frame, reply, ops, depth):
    """ops/s for `frame` sent `depth`-deep; every reply must equal `reply` byte-for-byte."""
    batch = frame * depth
    want = reply * depth
    rounds = ops // depth
    t0 = time.perf_counter()
    for _ in range(rounds):
        conn.sock.sendall(batch)
        got = conn.file.read(len(want))
        if got != want:
            raise AssertionError("reply mismatch %r" % got[:64])
    return (rounds * depth) / (time.perf_counter() - t0)


@contextlib.contextmanager
def armed(conn, name, value):
    """Arm `DEBUG <name> <value>`, yield, always disarm with 0. Both replies must be OK."""
    reply = call(conn, "DEBUG", name, str(value))
    if reply != b"OK":
        raise AssertionError("could not arm DEBUG %s %s: %r" % (name, value, reply))
    try:
        yield
    finally:
        reply = call(conn, "DEBUG", name, "0")
        if reply != b"OK":
            raise AssertionError("could not disarm DEBUG %s: %r" % (name, reply))


# ------------------------------------------------------------------------------------------------
# reporting
# ------------------------------------------------------------------------------------------------


def strict_mode():
    return os.environ.get("TOMO_GATE_STRICT", "") == "1"


def skip_all(reason):
    """The whole battery does not apply to this boot. Exit 3 (the gate paints it red)."""
    print("SKIP-ALL: %s" % reason, flush=True)
    sys.exit(3)


class Report:
    """ok/bad/skip lines in the house format plus the final `TAG PASS|FAIL n` line."""

    def __init__(self, tag):
        self.tag = tag
        self.checks = 0
        self.failures = []
        self.skips = []
        self.strict_skips = []

    def ok(self, name, detail=""):
        self.checks += 1
        print("  %-52s ok %s" % (name, detail), flush=True)

    def bad(self, name, detail=""):
        self.checks += 1
        self.failures.append("%s %s" % (name, detail))
        print("  %-52s FAIL %s" % (name, detail), flush=True)

    def check(self, name, cond, detail=""):
        (self.ok if cond else self.bad)(name, detail)
        return bool(cond)

    def skip(self, name, reason, strict=False):
        """strict=True: 'the mechanism could not be armed' (hook denied, knob absent) -- a failure
        under TOMO_GATE_STRICT=1. strict=False: a probabilistic window that did not open."""
        (self.strict_skips if strict else self.skips).append("%s -- %s" % (name, reason))
        print("  %-52s SKIP %s" % (name, reason), flush=True)

    def finish(self):
        if self.strict_skips and strict_mode():
            for entry in self.strict_skips:
                self.failures.append("strict: skipped arm would have been vacuous: " + entry)
                print("  FAIL (TOMO_GATE_STRICT) %s" % entry, flush=True)
        verdict = "PASS" if not self.failures else "FAIL %d" % len(self.failures)
        print("%s %s (checks=%d skips=%d%s)" % (
            self.tag, verdict, self.checks, len(self.skips) + len(self.strict_skips),
            " strict" if strict_mode() else ""), flush=True)
        sys.exit(1 if self.failures else 0)

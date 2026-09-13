#!/usr/bin/env python3
"""Directed CLIENT connection-control + MONITOR battery. Usage: climon2.py HOST PORT

Every check proves its MECHANISM FIRED, not merely that nothing broke:
  * CLIENT REPLY asserts the exact bytes on the wire, including absence.
  * MONITOR asserts the feed line CONTENT (quoting, redaction) and the
    monitor_feed_lines counter moving.
  * CLIENT PAUSE asserts timing in both directions AND the client_pause_holds
    counter moving -- a pause that never held anything proves nothing.
  * CLIENT UNBLOCK asserts the exact reply the blocked client received.
Negative controls are included for every one of them.
"""

import re
import select
import socket
import struct
import sys
import time


class RespError(Exception):
    pass


def encode(*args):
    out = [f"*{len(args)}\r\n".encode()]
    for arg in args:
        if isinstance(arg, str):
            arg = arg.encode()
        out.extend((f"${len(arg)}\r\n".encode(), arg, b"\r\n"))
    return b"".join(out)


class Conn:
    def __init__(self, host, port, timeout=10):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.file = self.sock.makefile("rb", buffering=0)

    def send(self, *args):
        self.sock.sendall(encode(*args))

    def raw(self, data):
        self.sock.sendall(data)

    def command(self, *args):
        self.send(*args)
        return self.read()

    def exact(self, size):
        chunks = []
        while size:
            chunk = self.file.read(size)
            if not chunk:
                raise EOFError("server closed connection")
            chunks.append(chunk)
            size -= len(chunk)
        return b"".join(chunks)

    def read(self):
        prefix = self.exact(1)
        line = self.file.readline()
        if not line.endswith(b"\r\n"):
            raise AssertionError(f"bad RESP line: {prefix + line!r}")
        value = line[:-2]
        if prefix == b"+":
            return value
        if prefix == b"-":
            return RespError(value.decode("utf-8", "replace"))
        if prefix == b":":
            return int(value)
        if prefix == b"$":
            size = int(value)
            if size == -1:
                return None
            payload = self.exact(size)
            assert self.exact(2) == b"\r\n"
            return payload
        if prefix in (b"*", b"~", b">"):
            size = int(value)
            if size == -1:
                return None
            return [self.read() for _ in range(size)]
        if prefix == b"%":
            return [self.read() for _ in range(int(value) * 2)]
        if prefix == b"_":
            return None
        raise AssertionError(f"unknown RESP prefix: {prefix!r}")

    def drain(self, wait=0.35):
        """Every byte currently readable. Used to prove ABSENCE as well as presence."""
        deadline = time.time() + wait
        out = b""
        while True:
            left = deadline - time.time()
            if left <= 0:
                break
            ready, _, _ = select.select([self.sock], [], [], left)
            if not ready:
                break
            chunk = self.sock.recv(65536)
            if not chunk:
                break
            out += chunk
            deadline = time.time() + 0.12
        return out

    def close(self, reset=False):
        if not self.sock:
            return
        if reset:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                 struct.pack("ii", 1, 0))
        try:
            self.file.close()
            self.sock.close()
        except OSError:
            pass
        self.sock = None


def expect(actual, wanted, label):
    if actual != wanted:
        raise AssertionError(f"{label}: got {actual!r}, wanted {wanted!r}")


def expect_err(actual, wanted, label):
    if not isinstance(actual, RespError) or str(actual) != wanted:
        raise AssertionError(f"{label}: got {actual!r}, wanted error {wanted!r}")


HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 6379

admin = Conn(HOST, PORT)
checks = 0


def stats():
    raw = admin.command("INFO", "STATS")
    out = {}
    for line in raw.decode().splitlines():
        if ":" not in line or line.startswith("#"):
            continue
        key, value = line.split(":", 1)
        try:
            out[key] = int(value)
        except ValueError:
            out[key] = value
    return out


def clients_info():
    raw = admin.command("INFO", "CLIENTS")
    out = {}
    for line in raw.decode().splitlines():
        if ":" not in line or line.startswith("#"):
            continue
        key, value = line.split(":", 1)
        try:
            out[key] = int(value)
        except ValueError:
            out[key] = value
    return out


def check(label, fn):
    global checks
    fn()
    checks += 1


opened = [admin]


def new():
    c = Conn(HOST, PORT)
    opened.append(c)
    return c


try:
    # ---- 1. CLIENT HELP ---------------------------------------------------------------------
    help_reply = admin.command("CLIENT", "HELP")
    assert isinstance(help_reply, list) and len(help_reply) >= 50, help_reply
    joined = b"\n".join(help_reply)
    for needed in (b"REPLY (ON|OFF|SKIP)", b"NO-TOUCH (ON|OFF)", b"UNBLOCK <clientid>",
                   b"TRACKING (ON|OFF)", b"TRACKINGINFO", b"PAUSE <timeout>"):
        assert needed in joined, f"CLIENT HELP missing {needed!r}"
    checks += 1

    # ---- 2. CLIENT NO-TOUCH -----------------------------------------------------------------
    expect(admin.command("CLIENT", "NO-TOUCH", "ON"), b"OK", "NO-TOUCH ON")
    info = admin.command("CLIENT", "INFO").decode()
    assert re.search(r"flags=\S*T", info), f"NO-TOUCH did not surface in flags: {info}"
    expect(admin.command("CLIENT", "NO-TOUCH", "OFF"), b"OK", "NO-TOUCH OFF")
    info = admin.command("CLIENT", "INFO").decode()
    assert not re.search(r"flags=\S*T", info), f"NO-TOUCH OFF left the flag set: {info}"
    expect_err(admin.command("CLIENT", "NO-TOUCH", "garbage"), "ERR syntax error",
               "NO-TOUCH garbage")   # negative control
    expect_err(admin.command("CLIENT", "NO-TOUCH"),
               "ERR wrong number of arguments for 'client|no-touch' command", "NO-TOUCH arity")
    # MECHANISM: the flag must actually reach an executor. It is only consulted (and only
    # counted) while maxmemory is enabled, so arm maxmemory, run reads with the flag on, and
    # require the counter to move -- then require it NOT to move with the flag off.
    maxmem_before = admin.command("CONFIG", "GET", "maxmemory")[1]
    expect(admin.command("CONFIG", "SET", "maxmemory", "268435456"), b"OK", "arm maxmemory")
    nt = new()
    nt.command("SET", "cl2:nt", "1")
    base = stats()["client_no_touch_ops"]
    for _ in range(20):
        nt.command("GET", "cl2:nt")
    expect(stats()["client_no_touch_ops"], base, "no-touch counted while OFF")  # negative control
    expect(nt.command("CLIENT", "NO-TOUCH", "ON"), b"OK", "NO-TOUCH ON for the mechanism check")
    for _ in range(20):
        nt.command("GET", "cl2:nt")
    moved = stats()["client_no_touch_ops"] - base
    assert moved >= 20, f"NO-TOUCH never reached an executor (counter moved {moved})"
    expect(nt.command("CLIENT", "NO-TOUCH", "OFF"), b"OK", "NO-TOUCH OFF")
    settled = stats()["client_no_touch_ops"]
    for _ in range(20):
        nt.command("GET", "cl2:nt")
    expect(stats()["client_no_touch_ops"], settled, "flag still set after OFF")  # negative control
    nt.close()
    admin.command("CONFIG", "SET", "maxmemory", maxmem_before.decode())
    checks += 10

    # ---- 3. CLIENT REPLY: exact wire bytes, including absence --------------------------------
    r = new()
    expect(r.command("PING"), b"PONG", "reply baseline")
    r.raw(encode("CLIENT", "REPLY", "OFF") + encode("PING") + encode("SET", "cl2:a", "1"))
    expect(r.drain(), b"", "CLIENT REPLY OFF must produce NO bytes at all")
    r.raw(encode("CLIENT", "REPLY", "ON"))
    expect(r.drain(), b"+OK\r\n", "CLIENT REPLY ON answers even while OFF")
    expect(r.command("GET", "cl2:a"), b"1", "commands still ran while suppressed")
    # SKIP drops exactly ONE reply out of a pipelined batch -- the case a per-connection
    # suppression would get wrong by swallowing both PONGs.
    r.raw(encode("CLIENT", "REPLY", "SKIP") + encode("PING") + encode("PING"))
    expect(r.drain(), b"+PONG\r\n", "CLIENT REPLY SKIP must drop exactly one reply")
    # Two SKIPs pipelined still leave exactly one PONG (oracle-matched).
    r.raw(encode("CLIENT", "REPLY", "SKIP") + encode("CLIENT", "REPLY", "SKIP") +
          encode("PING") + encode("PING"))
    expect(r.drain(), b"+PONG\r\n", "double SKIP")
    expect(r.command("PING"), b"PONG", "connection healthy after SKIP")   # negative control
    expect_err(r.command("CLIENT", "REPLY", "garbage"), "ERR syntax error", "REPLY garbage")
    # RESET clears an OFF mode.
    r.raw(encode("CLIENT", "REPLY", "OFF") + encode("RESET") + encode("PING"))
    expect(r.drain(), b"+RESET\r\n+PONG\r\n", "RESET clears CLIENT REPLY OFF")
    r.close()
    checks += 8

    # ---- 4. MONITOR: feed content, quoting, redaction, counter -------------------------------
    before_lines = stats()["monitor_feed_lines"]
    mon = new()
    expect(mon.command("MONITOR"), b"OK", "MONITOR")
    assert clients_info()["monitor_clients"] == 1, "monitor_clients did not move"
    driver = new()
    driver.command("SET", "cl2:mon", "v")
    driver.command("GET", "cl2:mon")
    driver.command("SET", b"cl2:b\x00in", b'q"t')
    driver.send("AUTH", "hunter2")
    driver.read()
    feed = mon.drain(0.6).decode("latin-1")
    lines = [ln for ln in feed.split("\r\n") if ln.startswith("+")]
    assert len(lines) >= 4, f"monitor feed too short: {feed!r}"
    body = "\n".join(lines)
    assert '"SET" "cl2:mon" "v"' in body, f"missing SET line: {body}"
    assert '"GET" "cl2:mon"' in body, f"missing GET line: {body}"
    assert '"SET" "cl2:b\\x00in" "q\\"t"' in body, f"binary quoting wrong: {body}"
    assert '"AUTH" "(redacted)"' in body, f"AUTH not redacted: {body}"
    assert "hunter2" not in body, "AUTH password leaked into the MONITOR feed"
    for ln in lines:
        assert re.match(r"^\+\d+\.\d{6} \[\d+ [^\]]+\] ", ln), f"bad feed prefix: {ln!r}"
    after_lines = stats()["monitor_feed_lines"]
    assert after_lines > before_lines, "monitor_feed_lines counter never moved"
    # A monitor DOES see its own traffic (oracle-matched).
    mon.send("PING")
    self_feed = mon.drain(0.5).decode("latin-1")
    assert '"PING"' in self_feed, f"monitor did not see its own command: {self_feed!r}"
    # RESET exits monitor mode; the feed must go silent. A monitor sees its own RESET, and this
    # server emits the feed line BEFORE the reply (redis emits it after), so match on content.
    mon.send("RESET")
    reset_bytes = mon.drain(0.5)
    assert b"+RESET\r\n" in reset_bytes, f"RESET reply missing: {reset_bytes!r}"
    driver.command("SET", "cl2:mon", "after")
    expect(mon.drain(0.4), b"", "feed continued after RESET")   # negative control
    assert clients_info()["monitor_clients"] == 0, "monitor_clients did not return to 0"
    checks += 10

    # ---- 5. MONITOR is disarmed when nobody is watching --------------------------------------
    quiet_before = stats()["monitor_feed_lines"]
    driver.command("SET", "cl2:quiet", "1")
    assert stats()["monitor_feed_lines"] == quiet_before, \
        "feed lines were formatted with no monitor attached"
    checks += 1

    # ---- 6. CLIENT UNBLOCK ------------------------------------------------------------------
    expect_err(admin.command("CLIENT", "UNBLOCK", "notanint"),
               "ERR value is not an integer or out of range", "UNBLOCK notanint")
    expect(admin.command("CLIENT", "UNBLOCK", "999999"), 0, "UNBLOCK unknown id")
    expect_err(admin.command("CLIENT", "UNBLOCK", "1", "GARBAGE"),
               "ERR CLIENT UNBLOCK reason should be TIMEOUT or ERROR", "UNBLOCK bad reason")
    blocker = new()
    bid = blocker.command("CLIENT", "ID")
    expect(admin.command("CLIENT", "UNBLOCK", str(bid)), 0,
           "UNBLOCK of a live but NOT blocked client")   # negative control
    blocker.send("BLPOP", "cl2:nolist", "30")
    time.sleep(0.3)
    expect(admin.command("CLIENT", "UNBLOCK", str(bid)), 1, "UNBLOCK returns 1")
    expect(blocker.drain(1.0), b"*-1\r\n", "TIMEOUT flavour delivers the null array")
    blocker.send("BLPOP", "cl2:nolist", "30")
    time.sleep(0.3)
    expect(admin.command("CLIENT", "UNBLOCK", str(bid), "ERROR"), 1, "UNBLOCK ERROR returns 1")
    expect(blocker.drain(1.0),
           b"-UNBLOCKED client unblocked via CLIENT UNBLOCK\r\n", "ERROR flavour")
    expect(blocker.command("PING"), b"PONG", "connection healthy after unblock")
    blocker.close()
    checks += 8

    # ---- 7. CLIENT PAUSE --------------------------------------------------------------------
    expect_err(admin.command("CLIENT", "PAUSE", "abc"),
               "ERR timeout is not an integer or out of range", "PAUSE abc")
    expect_err(admin.command("CLIENT", "PAUSE", "-1"), "ERR timeout is negative", "PAUSE -1")
    expect_err(admin.command("CLIENT", "PAUSE", "100", "GARBAGE"),
               "ERR CLIENT PAUSE mode must be WRITE or ALL", "PAUSE bad mode")
    holds_before = stats()["client_pause_holds"]
    admin.command("SET", "cl2:p", "0")
    expect(admin.command("CLIENT", "PAUSE", "800", "WRITE"), b"OK", "PAUSE WRITE")
    victim = new()
    t0 = time.time()
    expect(victim.command("GET", "cl2:p"), b"0", "reads flow under PAUSE WRITE")
    read_ms = (time.time() - t0) * 1000
    assert read_ms < 250, f"read stalled {read_ms:.0f}ms under PAUSE WRITE"
    t0 = time.time()
    expect(victim.command("SET", "cl2:p", "1"), b"OK", "write eventually completes")
    write_ms = (time.time() - t0) * 1000
    assert write_ms > 300, f"write was NOT held (took {write_ms:.0f}ms) -- pause never fired"
    holds_after = stats()["client_pause_holds"]
    assert holds_after > holds_before, "client_pause_holds counter never moved"
    # ALL mode holds reads too, and UNPAUSE releases immediately.
    expect(admin.command("CLIENT", "PAUSE", "5000", "ALL"), b"OK", "PAUSE ALL")
    waiter = new()
    waiter.send("GET", "cl2:p")
    t0 = time.time()
    time.sleep(0.3)
    expect(waiter.drain(0.3), b"", "read was NOT held under PAUSE ALL")
    expect(admin.command("CLIENT", "UNPAUSE"), b"OK", "UNPAUSE")
    expect(waiter.read(), b"1", "UNPAUSE released the held read")
    released_ms = (time.time() - t0) * 1000
    assert released_ms < 2000, f"UNPAUSE took {released_ms:.0f}ms"
    expect(victim.command("SET", "cl2:p", "2"), b"OK", "writes flow after UNPAUSE")
    victim.close()
    waiter.close()
    checks += 12

    # ---- 8. RESET ---------------------------------------------------------------------------
    rc = new()
    rc.command("CLIENT", "SETNAME", "gonzo")
    rc.command("SELECT", "3")
    expect(rc.command("RESET"), b"RESET", "RESET reply")
    expect(rc.command("CLIENT", "GETNAME"), None, "RESET cleared the name")
    assert "db=0" in rc.command("CLIENT", "INFO").decode(), "RESET did not reset the db"
    rc.close()
    checks += 3

    st = stats()
    print(f"climon2: ok ({checks} checks, monitor_feed_lines={st['monitor_feed_lines']}, "
          f"client_pause_holds={st['client_pause_holds']}, "
          f"client_no_touch_ops={st['client_no_touch_ops']})")
finally:
    try:
        admin.command("CLIENT", "UNPAUSE")
    except Exception:
        pass
    for c in opened:
        c.close()

#!/usr/bin/env python3
"""Directed CLIENT TRACKING (server-assisted client-side caching) battery.

Usage: tracking.py HOST PORT

Raw-socket RESP3: every invalidation assertion compares the EXACT push frame
bytes, and every "should not fire" case is a real negative control that drains
the socket and requires emptiness. The tracking_invalidations counter is asserted
to move, so a battery that silently delivered nothing cannot pass.
"""

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


def push(key):
    return b">2\r\n$10\r\ninvalidate\r\n*1\r\n$%d\r\n%s\r\n" % (len(key), key)


FLUSH_PUSH = b">2\r\n$10\r\ninvalidate\r\n_\r\n"


class Conn:
    def __init__(self, host, port, timeout=10):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.file = self.sock.makefile("rb", buffering=0)

    def send(self, *args):
        self.sock.sendall(encode(*args))

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
        if prefix == b"#":
            return value == b"t"
        raise AssertionError(f"unknown RESP prefix: {prefix!r}")

    def drain(self, wait=0.5):
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
            deadline = time.time() + 0.15
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

opened = []


def new(resp3=False):
    c = Conn(HOST, PORT)
    opened.append(c)
    if resp3:
        c.command("HELLO", "3")
        c.drain(0.05)
    return c


admin = new()
writer = new()
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


try:
    invalidations_before = stats()["tracking_invalidations"]

    # ---- 1. grammar + exact error strings ----------------------------------------------------
    g = new(resp3=True)
    expect(g.command("CLIENT", "TRACKING", "off"), b"OK", "TRACKING off when already off")
    expect(g.command("CLIENT", "GETREDIR"), -1, "GETREDIR when tracking off")
    expect(g.command("CLIENT", "TRACKINGINFO"),
           [b"flags", [b"off"], b"redirect", -1, b"prefixes", []], "TRACKINGINFO off")
    expect_err(g.command("CLIENT", "TRACKING", "garbage"), "ERR syntax error", "TRACKING garbage")
    expect_err(g.command("CLIENT", "TRACKING", "on", "PREFIX", "p"),
               "ERR PREFIX option requires BCAST mode to be enabled", "PREFIX without BCAST")
    expect_err(g.command("CLIENT", "TRACKING", "on", "OPTIN", "OPTOUT"),
               "ERR You can't specify both OPTIN mode and OPTOUT mode", "OPTIN+OPTOUT")
    expect_err(g.command("CLIENT", "TRACKING", "on", "BCAST", "OPTIN"),
               "ERR OPTIN and OPTOUT are not compatible with BCAST", "BCAST+OPTIN")
    expect_err(g.command("CLIENT", "TRACKING", "on", "BCAST",
                         "PREFIX", "foo", "PREFIX", "foobar"),
               "ERR Prefix 'foo' overlaps with another provided prefix 'foobar'. "
               "Prefixes for a single client must not overlap.", "overlapping prefixes")
    expect_err(g.command("CLIENT", "TRACKING", "on", "REDIRECT", "999999"),
               "ERR The client ID you want redirect to does not exist", "REDIRECT to nobody")
    expect_err(g.command("CLIENT", "TRACKING", "on", "REDIRECT", "abc"),
               "ERR value is not an integer or out of range", "REDIRECT abc")
    expect(g.command("CLIENT", "TRACKING", "off", "PREFIX", "p"), b"OK",
           "TRACKING off ignores PREFIX")
    expect_err(g.command("CLIENT", "CACHING", "yes"),
               "ERR CLIENT CACHING can be called only when the client is in tracking mode "
               "with OPTIN or OPTOUT mode enabled", "CACHING while off")
    checks += 12

    # mode switching is refused while on
    expect(g.command("CLIENT", "TRACKING", "on", "BCAST"), b"OK", "TRACKING on BCAST")
    expect_err(g.command("CLIENT", "TRACKING", "on"),
               "ERR You can't switch BCAST mode on/off before disabling tracking for this "
               "client, and then re-enabling it with a different mode.", "BCAST switch refused")
    # BCAST with no PREFIX registers the empty prefix (oracle-matched)
    expect(g.command("CLIENT", "TRACKINGINFO"),
           [b"flags", [b"on", b"bcast"], b"redirect", 0, b"prefixes", [b""]],
           "TRACKINGINFO on BCAST")
    expect(g.command("CLIENT", "TRACKING", "off"), b"OK", "TRACKING off")
    expect(g.command("CLIENT", "TRACKING", "on", "OPTIN"), b"OK", "TRACKING on OPTIN")
    expect_err(g.command("CLIENT", "TRACKING", "on", "OPTOUT"),
               "ERR You can't switch OPTIN/OPTOUT mode before disabling tracking for this "
               "client, and then re-enabling it with a different mode.", "OPTIN/OPTOUT switch")
    expect_err(g.command("CLIENT", "CACHING", "no"),
               "ERR CLIENT CACHING NO is only valid when tracking is enabled in OPTOUT mode.",
               "CACHING no while OPTIN")
    g.command("CLIENT", "TRACKING", "off")
    checks += 7

    # ---- 2. default mode: SET / DEL / expiry / miss-then-write --------------------------------
    t = new(resp3=True)
    expect(t.command("CLIENT", "TRACKING", "on"), b"OK", "TRACKING on")
    expect(t.command("CLIENT", "GETREDIR"), 0, "GETREDIR when on with no redirect")
    writer.command("SET", "trk:a", "1")
    t.drain(0.2)
    expect(t.command("GET", "trk:a"), b"1", "read registers the key")
    writer.command("SET", "trk:a", "2")
    expect(t.drain(), push(b"trk:a"), "plain SET invalidation")

    t.command("GET", "trk:a")
    writer.command("DEL", "trk:a")
    expect(t.drain(), push(b"trk:a"), "DEL invalidation")

    # A read by ANOTHER client must never invalidate -- negative control.
    writer.command("SET", "trk:n", "1")
    t.command("GET", "trk:n")
    writer.command("GET", "trk:n")
    expect(t.drain(0.4), b"", "a read must not invalidate")

    # An untracked key must never invalidate -- negative control.
    writer.command("SET", "trk:never", "1")
    expect(t.drain(0.4), b"", "write to an unread key must not invalidate")

    # Reading a MISSING key still registers it (oracle-matched).
    expect(t.command("GET", "trk:missing"), None, "missing key read")
    writer.command("SET", "trk:missing", "now")
    expect(t.drain(), push(b"trk:missing"), "miss-then-write invalidation")

    # Expiry-driven deletion. The TTL is set BEFORE this client registers, so the only event it
    # can possibly receive for this key is the expiry itself.
    writer.command("SET", "trk:exp", "1")
    writer.command("PEXPIRE", "trk:exp", "400")
    expect(t.drain(0.3), b"", "no invalidation before registering")
    expect(t.command("GET", "trk:exp"), b"1", "register while still alive")
    time.sleep(0.8)
    writer.command("GET", "trk:exp")   # nudge the expiry observer
    expect(t.drain(2.0), push(b"trk:exp"), "expiry invalidation")
    checks += 8

    # ---- 3. NOLOOP ---------------------------------------------------------------------------
    t.command("CLIENT", "TRACKING", "off")
    t.drain(0.2)
    expect(t.command("CLIENT", "TRACKING", "on", "NOLOOP"), b"OK", "TRACKING on NOLOOP")
    t.command("SET", "trk:loop", "1")
    t.command("GET", "trk:loop")
    t.drain(0.2)
    expect(t.command("SET", "trk:loop", "2"), b"OK", "self write under NOLOOP")
    expect(t.drain(0.4), b"", "NOLOOP must suppress a self-write invalidation")
    t.command("GET", "trk:loop")
    writer.command("SET", "trk:loop", "3")
    expect(t.drain(), push(b"trk:loop"), "NOLOOP still delivers other-client writes")
    checks += 3

    # self-write WITHOUT noloop does invalidate (oracle-matched)
    t.command("CLIENT", "TRACKING", "off")
    t.drain(0.2)
    t.command("CLIENT", "TRACKING", "on")
    t.command("SET", "trk:self", "1")
    t.command("GET", "trk:self")
    t.drain(0.2)
    t.send("SET", "trk:self", "2")
    got = t.drain()
    assert push(b"trk:self") in got, f"self-invalidation missing: {got!r}"
    checks += 1

    # ---- 4. BCAST ----------------------------------------------------------------------------
    b = new(resp3=True)
    expect(b.command("CLIENT", "TRACKING", "on", "BCAST", "PREFIX", "bc:"), b"OK", "BCAST on")
    # BCAST needs no prior read.
    writer.command("SET", "bc:1", "x")
    expect(b.drain(), push(b"bc:1"), "BCAST prefix hit with no prior read")
    writer.command("SET", "other:1", "x")
    expect(b.drain(0.4), b"", "BCAST prefix miss")             # negative control
    writer.command("DEL", "bc:1")
    expect(b.drain(), push(b"bc:1"), "BCAST DEL")
    expect(b.command("CLIENT", "TRACKINGINFO"),
           [b"flags", [b"on", b"bcast"], b"redirect", 0, b"prefixes", [b"bc:"]],
           "BCAST TRACKINGINFO")
    b.command("CLIENT", "TRACKING", "off")
    b.drain(0.2)
    writer.command("SET", "bc:2", "x")
    expect(b.drain(0.4), b"", "TRACKING off stops BCAST")       # negative control
    checks += 6

    # ---- 5. OPTIN / OPTOUT -------------------------------------------------------------------
    o = new(resp3=True)
    expect(o.command("CLIENT", "TRACKING", "on", "OPTIN"), b"OK", "OPTIN on")
    writer.command("SET", "oi:1", "1")
    writer.command("SET", "oi:2", "1")
    o.command("GET", "oi:1")                     # no CACHING -> not tracked
    writer.command("SET", "oi:1", "2")
    expect(o.drain(0.4), b"", "OPTIN without CACHING must not track")   # negative control
    expect(o.command("CLIENT", "CACHING", "yes"), b"OK", "CACHING yes")
    o.command("GET", "oi:2")                     # tracked
    writer.command("SET", "oi:2", "2")
    expect(o.drain(), push(b"oi:2"), "OPTIN + CACHING yes tracks")
    # CACHING covers only the NEXT command.
    o.command("CLIENT", "CACHING", "yes")
    writer.command("SET", "oi:3", "1")
    writer.command("SET", "oi:4", "1")
    o.command("GET", "oi:3")                     # covered
    o.command("GET", "oi:4")                     # NOT covered
    o.drain(0.2)
    writer.command("SET", "oi:4", "2")
    expect(o.drain(0.4), b"", "CACHING yes must cover only one command")
    writer.command("SET", "oi:3", "2")
    expect(o.drain(), push(b"oi:3"), "the covered command was tracked")
    o.command("CLIENT", "TRACKING", "off")
    o.drain(0.2)

    expect(o.command("CLIENT", "TRACKING", "on", "OPTOUT"), b"OK", "OPTOUT on")
    writer.command("SET", "oo:1", "1")
    writer.command("SET", "oo:2", "1")
    o.command("GET", "oo:1")                     # tracked by default
    o.drain(0.2)
    writer.command("SET", "oo:1", "2")
    expect(o.drain(), push(b"oo:1"), "OPTOUT tracks by default")
    expect(o.command("CLIENT", "CACHING", "no"), b"OK", "CACHING no")
    o.command("GET", "oo:2")                     # exempted
    writer.command("SET", "oo:2", "2")
    expect(o.drain(0.4), b"", "CACHING no exempts the next command")     # negative control
    o.command("CLIENT", "TRACKING", "off")
    o.drain(0.2)
    checks += 8

    # ---- 6. protocol x no-redirect: RESP2 silence with a RESP3 positive control ---------------
    # Register the SAME key on both connections and mutate it once. The RESP3 push proves the
    # producer fired, so RESP2 silence cannot pass merely because invalidation is broken globally.
    plain2 = new()
    plain3 = new(resp3=True)
    writer.command("SET", "proto:plain", "1")
    expect(plain2.command("CLIENT", "TRACKING", "on"), b"OK", "RESP2 TRACKING on")
    expect(plain3.command("CLIENT", "TRACKING", "on"), b"OK", "RESP3 TRACKING on")
    expect(plain2.command("GET", "proto:plain"), b"1", "RESP2 registers the shared probe")
    expect(plain3.command("GET", "proto:plain"), b"1", "RESP3 registers the shared probe")
    writer.command("SET", "proto:plain", "2")
    expect(plain3.drain(), push(b"proto:plain"), "RESP3 positive control sees invalidation")
    expect(plain2.drain(0.4), b"", "RESP2 without REDIRECT gets no frame")
    expect(plain2.command("PING"), b"PONG", "RESP2 reply stream remains synchronized")
    plain2.command("CLIENT", "TRACKING", "off")
    plain3.command("CLIENT", "TRACKING", "off")
    checks += 7

    # ---- 7. REDIRECT to a RESP2 pubsub connection --------------------------------------------
    target = new()
    tid = target.command("CLIENT", "ID")
    rc = new()
    expect(rc.command("CLIENT", "TRACKING", "on", "REDIRECT", str(tid)), b"OK", "REDIRECT on")
    expect(rc.command("CLIENT", "GETREDIR"), tid, "GETREDIR reports the target")

    # REDIRECT alone does not make RESP2 a push channel: the target must really be subscribed.
    writer.command("SET", "rd:quiet", "1")
    expect(rc.command("GET", "rd:quiet"), b"1", "register before unsubscribed redirect probe")
    writer.command("SET", "rd:quiet", "2")
    expect(target.drain(0.4), b"", "unsubscribed RESP2 redirect target gets no frame")
    expect(target.command("PING"), b"PONG", "unsubscribed redirect reply stream stays synchronized")

    expect(target.command("SUBSCRIBE", "__redis__:invalidate"),
           [b"subscribe", b"__redis__:invalidate", 1], "redirect target subscribes")
    target.drain(0.2)
    writer.command("SET", "rd:1", "1")
    rc.command("GET", "rd:1")
    rc.drain(0.2)
    writer.command("SET", "rd:1", "2")
    expect(target.drain(),
           b"*3\r\n$7\r\nmessage\r\n$20\r\n__redis__:invalidate\r\n*1\r\n$4\r\nrd:1\r\n",
           "RESP2 redirect delivery")
    expect(rc.drain(0.3), b"", "the tracking client itself gets nothing under REDIRECT")
    checks += 8

    # redirect target disappears -> broken_redirect, tracking stays on (oracle-matched)
    rc.command("GET", "rd:1")
    rc.drain(0.2)
    target.close(reset=True)
    time.sleep(0.4)
    writer.command("SET", "rd:1", "3")
    time.sleep(0.4)
    rc.drain(0.4)
    info = rc.command("CLIENT", "TRACKINGINFO")
    assert info[1] == [b"on", b"broken_redirect"], f"broken_redirect not reported: {info!r}"
    expect(rc.command("CLIENT", "TRACKING", "off"), b"OK", "TRACKING off clears broken redirect")
    expect(rc.command("CLIENT", "TRACKINGINFO"),
           [b"flags", [b"off"], b"redirect", -1, b"prefixes", []], "cleared")
    checks += 3

    # A RESP3 redirect target gets a real push whether or not it is subscribed.
    target3 = new(resp3=True)
    tid3 = target3.command("CLIENT", "ID")
    rc3 = new()
    expect(rc3.command("CLIENT", "TRACKING", "on", "REDIRECT", str(tid3)),
           b"OK", "RESP3 REDIRECT on")
    writer.command("SET", "rd3:plain", "1")
    expect(rc3.command("GET", "rd3:plain"), b"1", "register RESP3 redirect probe")
    writer.command("SET", "rd3:plain", "2")
    expect(target3.drain(), push(b"rd3:plain"), "unsubscribed RESP3 redirect gets a push")
    expect(target3.command("SUBSCRIBE", "__redis__:invalidate"),
           [b"subscribe", b"__redis__:invalidate", 1], "RESP3 redirect target subscribes")
    target3.drain(0.2)
    writer.command("SET", "rd3:sub", "1")
    expect(rc3.command("GET", "rd3:sub"), b"1", "register subscribed RESP3 redirect probe")
    writer.command("SET", "rd3:sub", "2")
    expect(target3.drain(), push(b"rd3:sub"), "subscribed RESP3 redirect still gets a push")
    rc3.command("CLIENT", "TRACKING", "off")
    checks += 7

    # ---- 8. FLUSHALL -> null invalidation, with the same protocol control ---------------------
    f2 = new()
    f3 = new(resp3=True)
    f2.command("CLIENT", "TRACKING", "on")
    f3.command("CLIENT", "TRACKING", "on")
    writer.command("FLUSHALL")
    expect(f3.drain(), FLUSH_PUSH, "RESP3 FLUSHALL null invalidation positive control")
    expect(f2.drain(0.4), b"", "RESP2 FLUSHALL without REDIRECT gets no frame")
    expect(f2.command("PING"), b"PONG", "RESP2 stream remains synchronized after FLUSHALL")
    f2.command("CLIENT", "TRACKING", "off")
    f3.command("CLIENT", "TRACKING", "off")
    checks += 3

    # ---- 9. counters + full disarm ------------------------------------------------------------
    invalidations_after = stats()["tracking_invalidations"]
    assert invalidations_after > invalidations_before, \
        "tracking_invalidations never moved -- the battery proved nothing"
    for c in (t,):
        c.command("CLIENT", "TRACKING", "off")
        c.drain(0.2)
    clients_raw = admin.command("INFO", "CLIENTS").decode()
    assert "tracking_clients:0" in clients_raw, \
        f"tracking clients did not return to zero:\n{clients_raw}"
    # With the lane fully disarmed a write must produce nothing anywhere -- the disarm control.
    quiet = stats()["tracking_invalidations"]
    writer.command("SET", "trk:quiet", "1")
    time.sleep(0.2)
    assert stats()["tracking_invalidations"] == quiet, \
        "an invalidation fired with tracking fully off"
    checks += 3

    print(f"tracking: ok ({checks} checks, tracking_invalidations="
          f"{invalidations_after - invalidations_before} fired)")
finally:
    for c in opened:
        c.close()

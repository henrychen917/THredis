#!/usr/bin/env python3
"""Directed connection-limits test. Usage: tests/limits.py HOST PORT"""

import os
import socket
import struct
import sys
import threading
import time


HOST, PORT = sys.argv[1], int(sys.argv[2])


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
    def __init__(self, timeout=10, recvbuf=None):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if recvbuf is not None:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, recvbuf)
        self.sock.settimeout(timeout)
        self.sock.connect((HOST, PORT))
        self.file = self.sock.makefile("rb")

    def send(self, *args):
        self.sock.sendall(encode(*args))

    def command(self, *args):
        self.send(*args)
        return self.read()

    def read(self):
        prefix = self.file.read(1)
        if not prefix:
            raise EOFError("server closed the connection")
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
            payload = self.file.read(size)
            if self.file.read(2) != b"\r\n":
                raise AssertionError("bad bulk trailer")
            return payload
        if prefix == b"*":
            size = int(value)
            if size == -1:
                return None
            return [self.read() for _ in range(size)]
        raise AssertionError(f"unknown RESP prefix: {prefix!r}")

    def close(self, reset=False):
        if self.sock is None:
            return
        if reset:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                 struct.pack("ii", 1, 0))
        self.file.close()
        self.sock.close()
        self.sock = None


def expect(actual, wanted, label):
    if actual != wanted:
        raise AssertionError(f"{label}: got {actual!r}, wanted {wanted!r}")
    print(f"  ok   {label}", flush=True)


def expect_error(actual, contains, label):
    if not isinstance(actual, RespError) or contains not in str(actual):
        raise AssertionError(f"{label}: got {actual!r}")
    print(f"  ok   {label}", flush=True)


def config_get(conn, name):
    value = conn.command("CONFIG", "GET", name)
    if not isinstance(value, list) or len(value) != 2 or value[0] != name.encode():
        raise AssertionError(f"CONFIG GET {name}: {value!r}")
    return value[1].decode()


def stats(conn):
    raw = conn.command("INFO", "STATS")
    if not isinstance(raw, bytes):
        raise AssertionError(f"INFO STATS returned {raw!r}")
    out = {}
    for line in raw.decode().splitlines():
        if ":" in line:
            key, value = line.split(":", 1)
            out[key] = value
    return out


def expect_stat(conn, name, wanted, label, timeout=2.0):
    deadline = time.monotonic() + timeout
    actual = None
    while time.monotonic() < deadline:
        actual = int(stats(conn)[name])
        if actual == wanted:
            print(f"  ok   {label}", flush=True)
            return
        time.sleep(0.02)
    raise AssertionError(f"{label}: got {actual!r}, wanted {wanted!r}")


def wait_for_eof(conn, deadline, heartbeat=None):
    next_heartbeat = time.monotonic()
    while time.monotonic() < deadline:
        conn.sock.settimeout(min(0.2, max(0.05, deadline - time.monotonic())))
        try:
            return conn.sock.recv(1) == b""
        except (ConnectionResetError, BrokenPipeError):
            return True
        except socket.timeout:
            if heartbeat is not None and time.monotonic() >= next_heartbeat:
                if heartbeat.command("PING") != b"PONG":
                    raise AssertionError("heartbeat connection failed")
                next_heartbeat = time.monotonic() + 0.4
    return False


def main():
    admin = Conn()
    original = {
        name: config_get(admin, name)
        for name in ("maxclients", "timeout", "tcp-keepalive",
                     "client-output-buffer-limit", "zc-min")
    }
    token = f"limits:{os.getpid()}:{time.time_ns()}"
    try:
        defaults = ("normal 0 0 0 slave 268435456 67108864 60 "
                    "pubsub 33554432 8388608 60")
        expect(original["client-output-buffer-limit"], defaults, "COBL fresh defaults")

        # Exact multi-clause grammar, Redis memory units, alias serialization, and merge behavior.
        value = "normal 1k 1kb 2 replica 1gb 64mb 60 pubsub 32mb 8mb 60"
        expect(admin.command("CONFIG", "SET", "client-output-buffer-limit", value),
               b"OK", "COBL multi-clause grammar")
        normalized = ("normal 1000 1024 2 slave 1073741824 67108864 60 "
                      "pubsub 33554432 8388608 60")
        expect(config_get(admin, "client-output-buffer-limit"), normalized,
               "COBL units and replica->slave serialization")
        expect(admin.command("CONFIG", "SET", "client-output-buffer-limit",
                             "pubsub 4096 2048 1"), b"OK", "COBL partial merge")
        merged = config_get(admin, "client-output-buffer-limit")
        if not merged.startswith("normal 1000 1024 2 slave 1073741824 67108864 60 "):
            raise AssertionError(f"COBL merge replaced unmentioned classes: {merged!r}")
        print("  ok   COBL unmentioned classes retained", flush=True)

        before = merged
        for bad, label in (("normal 0 0", "wrong group width"),
                           ("master 0 0 0", "master class"),
                           ("unknown 0 0 0", "unknown class"),
                           ("normal 0 0 -1", "negative seconds"),
                           ("normal 0 0 1x", "trailing seconds garbage"),
                           ("normal 184467440737095516160wat 0 0",
                            "overflow with garbage suffix"),
                           ("normal 0 0 0 pubsub nope 0 0", "all-or-nothing")):
            expect_error(admin.command("CONFIG", "SET", "client-output-buffer-limit", bad),
                         "Invalid argument", f"COBL rejects {label}")
            expect(config_get(admin, "client-output-buffer-limit"), before,
                   f"COBL rejection atomic: {label}")

        expect_error(admin.command("CONFIG", "SET", "tcp-backlog", "10"),
                     "immutable", "tcp-backlog is boot-only")
        expect(admin.command("CONFIG", "SET", "tcp-keepalive", "1"), b"OK",
               "tcp-keepalive live knob")
        expect(config_get(admin, "tcp-keepalive"), "1", "tcp-keepalive round-trip")
        expect(admin.command("CONFIG", "SET", "client-output-buffer-limit",
                             "normal 0 0 0 pubsub 0 0 0"), b"OK",
               "disable enforced COBL classes for lifecycle arms")

        # Sequential admission has no cross-accept race: admin + two clients fill maxclients=3.
        expect(admin.command("CONFIG", "SET", "maxclients", "3"), b"OK",
               "maxclients live lower")
        first, second = Conn(), Conn()
        expect(first.command("PING"), b"PONG", "maxclients admitted slot one")
        expect(second.command("PING"), b"PONG", "maxclients admitted slot two")
        rejected_before = int(stats(admin)["rejected_connections"])
        rejected = Conn()
        raw = rejected.sock.recv(128)
        expect(raw, b"-ERR max number of clients reached\r\n", "maxclients exact error")
        expect(rejected.sock.recv(1), b"", "maxclients reject closes synchronously")
        rejected.close()
        expect_stat(admin, "rejected_connections", rejected_before + 1,
                    "rejected_connections advances")
        first.close()
        time.sleep(0.1)
        replacement = Conn()
        expect(replacement.command("PING"), b"PONG", "disconnect returns admission slot")
        replacement.close()
        second.close()
        expect(admin.command("CONFIG", "SET", "maxclients", "10000"), b"OK",
               "maxclients live raise")

        # A simultaneous burst may overshoot only by the number of independent IO acceptors.
        # The process affinity is a conservative upper bound on n_io for this purpose-booted test.
        expect(admin.command("CONFIG", "SET", "maxclients", "20"), b"OK",
               "maxclients storm setup")
        storm_before = int(stats(admin)["rejected_connections"])
        storm_start = threading.Barrier(201)
        storm_admitted = []
        storm_errors = []
        storm_lock = threading.Lock()

        def storm_connect():
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(5)
            try:
                storm_start.wait()
                sock.connect((HOST, PORT))
                sock.sendall(encode("PING"))
                reply = b""
                while not reply.endswith(b"\r\n"):
                    chunk = sock.recv(64)
                    if not chunk:
                        break
                    reply += chunk
                if reply == b"+PONG\r\n":
                    with storm_lock:
                        storm_admitted.append(sock)
                    return
                if reply not in (b"", b"-ERR max number of clients reached\r\n"):
                    raise AssertionError(f"unexpected storm reply {reply!r}")
            except Exception as exc:
                with storm_lock:
                    storm_errors.append(str(exc))
            sock.close()

        storm_threads = [threading.Thread(target=storm_connect) for _ in range(200)]
        for thread in storm_threads:
            thread.start()
        storm_start.wait()
        for thread in storm_threads:
            thread.join(timeout=10)
        alive = sum(thread.is_alive() for thread in storm_threads)
        affinity_width = len(os.sched_getaffinity(0))
        if (alive or storm_errors or not (19 <= len(storm_admitted) <= 19 + affinity_width)):
            raise AssertionError(
                f"maxclients storm bound: admitted={len(storm_admitted)} "
                f"affinity={affinity_width} alive={alive} errors={storm_errors!r}")
        print(f"  ok   maxclients storm slop bound (admitted={len(storm_admitted)})",
              flush=True)
        expect_stat(admin, "rejected_connections",
                    storm_before + 200 - len(storm_admitted),
                    "maxclients storm rejection counter")
        for sock in storm_admitted:
            sock.close()
        time.sleep(0.1)
        expect(admin.command("CONFIG", "SET", "maxclients", "10000"), b"OK",
               "maxclients storm restore")

        # Timeout uses strict whole-second comparison. Subscriber mode remains exempt; MULTI does not.
        idle = Conn(timeout=5)
        expect(admin.command("CONFIG", "SET", "timeout", "1"), b"OK", "timeout live enable")
        started = time.monotonic()
        if not wait_for_eof(idle, started + 3.5, admin):
            raise AssertionError("idle client survived timeout")
        elapsed = time.monotonic() - started
        if elapsed < 1.0 or elapsed > 3.5:
            raise AssertionError(f"idle timeout outside strict-second window: {elapsed:.3f}s")
        print(f"  ok   idle timeout closes ({elapsed:.2f}s)", flush=True)
        idle.close()

        transaction = Conn(timeout=5)
        expect(transaction.command("MULTI"), b"OK", "MULTI opened")
        if not wait_for_eof(transaction, time.monotonic() + 3.5, admin):
            raise AssertionError("idle MULTI client was incorrectly exempt")
        print("  ok   idle MULTI is not timeout-exempt", flush=True)
        transaction.close()

        subscriber = Conn(timeout=5)
        channel = f"{token}:timeout"
        expect(subscriber.command("SUBSCRIBE", channel),
               [b"subscribe", channel.encode(), 1], "subscriber timeout setup")
        for _ in range(6):
            time.sleep(0.4)
            expect(admin.command("PING"), b"PONG", "admin timeout heartbeat")
        expect(subscriber.command("PING"), [b"pong", b""], "subscriber timeout exemption")
        subscriber.close()

        blocked = Conn(timeout=5)
        blocked_key = f"{token}:blocked"
        blocked.send("BLPOP", blocked_key, "0")
        for _ in range(6):
            time.sleep(0.4)
            expect(admin.command("PING"), b"PONG", "blocked timeout heartbeat")
        expect(admin.command("LPUSH", blocked_key, "wake"), 1, "blocked timeout wake")
        expect(blocked.read(), [blocked_key.encode(), b"wake"], "blocked timeout exemption")
        blocked.close()

        active = Conn(timeout=5)
        for _ in range(5):
            time.sleep(0.45)
            expect(active.command("PING"), b"PONG", "recv/send refreshes idle clock")
            expect(admin.command("PING"), b"PONG", "admin timeout heartbeat")
        active.close()
        expect(admin.command("CONFIG", "SET", "timeout", "0"), b"OK", "timeout disable")
        disabled_idle = Conn(timeout=5)
        time.sleep(1.5)
        expect(disabled_idle.command("PING"), b"PONG", "timeout zero keeps idle client")
        disabled_idle.close()

        # Hard limits fire from reply staging before bytes are submitted, for normal and pubsub.
        hard_before = int(stats(admin)["client_output_buffer_limit_disconnections"])
        expect(admin.command("CONFIG", "SET", "client-output-buffer-limit",
                             "normal 1024 0 0 pubsub 4096 0 0"), b"OK",
               "hard-limit setup")
        normal = Conn(timeout=3)
        normal.send("ECHO", b"x" * 2048)
        if not wait_for_eof(normal, time.monotonic() + 2):
            raise AssertionError("normal hard output limit did not close")
        normal.close()
        expect(admin.command("CONFIG", "SET", "client-output-buffer-limit",
                             "normal 0 0 0"), b"OK", "normal hard limit disabled")
        expect_stat(admin, "client_output_buffer_limit_disconnections", hard_before + 1,
                    "normal hard limit counter")

        sub = Conn(timeout=3)
        pub = Conn(timeout=3)
        hard_channel = f"{token}:hard"
        expect(sub.command("SUBSCRIBE", hard_channel),
               [b"subscribe", hard_channel.encode(), 1], "pubsub hard-limit setup")
        pub.command("PUBLISH", hard_channel, b"y" * 8192)
        if not wait_for_eof(sub, time.monotonic() + 2):
            raise AssertionError("pubsub hard output limit did not close")
        sub.close()
        pub.close()
        expect_stat(admin, "client_output_buffer_limit_disconnections", hard_before + 2,
                    "pubsub class hard limit counter")

        # A fully transmitted contiguous reply must leave no stale bytes in the counter.
        expect(admin.command("CONFIG", "SET", "client-output-buffer-limit",
                             "normal 1mb 0 0"), b"OK", "contiguous accounting setup")
        drained = Conn(timeout=5)
        expect(drained.command("ECHO", b"d" * 65536), b"d" * 65536,
               "contiguous reply drains")
        expect(admin.command("CONFIG", "SET", "client-output-buffer-limit",
                             "normal 1024 0 0"), b"OK", "contiguous accounting lower")
        expect(drained.command("PING"), b"PONG", "contiguous accounting returns to zero")
        drained.close()

        # Keep a small receive window unread while continuously producing replies. The staged
        # bytes must remain above soft for strictly more than one second before cron closes it.
        expect(admin.command("CONFIG", "SET", "client-output-buffer-limit",
                             "normal 0 32768 1 pubsub 0 0 0"), b"OK",
               "soft-limit setup")
        soft = Conn(timeout=1, recvbuf=4096)
        stopped = threading.Event()

        def flood_soft():
            frame = encode("ECHO", b"s" * 4096)
            try:
                while not stopped.is_set():
                    soft.sock.sendall(frame * 64)
            except (OSError, socket.timeout):
                pass

        flood = threading.Thread(target=flood_soft)
        flood.start()
        time.sleep(2.8)
        stopped.set()
        flood.join(timeout=2)
        soft.sock.settimeout(2)
        drained = 0
        closed = False
        try:
            while drained < 8 * 1024 * 1024:
                chunk = soft.sock.recv(65536)
                if not chunk:
                    closed = True
                    break
                drained += len(chunk)
        except (ConnectionResetError, BrokenPipeError):
            closed = True
        except socket.timeout:
            pass
        soft.close()
        if not closed:
            raise AssertionError(f"soft output limit did not close (drained={drained})")
        expect_stat(admin, "client_output_buffer_limit_disconnections", hard_before + 3,
                    "soft limit continuous-window counter")

        # A large GET takes the Borrow segment path when zc-min is enabled; it must count too.
        expect(admin.command("CONFIG", "SET", "client-output-buffer-limit",
                             "normal 1mb 0 0"), b"OK", "borrow accounting setup")
        expect(admin.command("CONFIG", "SET", "zc-min", "1"), b"OK", "enable borrow path")
        expect(admin.command("SET", f"{token}:borrow", b"z" * 65536), b"OK",
               "borrow value setup")
        drained_borrower = Conn(timeout=3)
        expect(drained_borrower.command("GET", f"{token}:borrow"), b"z" * 65536,
               "borrow reply drains")
        expect(admin.command("CONFIG", "SET", "client-output-buffer-limit",
                             "normal 32768 0 0"), b"OK", "borrow hard-limit setup")
        expect(drained_borrower.command("PING"), b"PONG", "borrow accounting returns to zero")
        drained_borrower.close()
        borrower = Conn(timeout=3)
        borrower.send("GET", f"{token}:borrow")
        if not wait_for_eof(borrower, time.monotonic() + 2):
            raise AssertionError("borrow bytes were absent from output accounting")
        borrower.close()
        expect_stat(admin, "client_output_buffer_limit_disconnections", hard_before + 4,
                    "borrow hard limit counter")
    finally:
        # Restore the shared purpose-booted server even when an assertion fails.
        try:
            try:
                admin.command("PING")
            except (EOFError, OSError):
                admin.close()
                admin = Conn()
            admin.command("CONFIG", "SET", "maxclients", original["maxclients"])
            admin.command("CONFIG", "SET", "timeout", original["timeout"])
            admin.command("CONFIG", "SET", "tcp-keepalive", original["tcp-keepalive"])
            admin.command("CONFIG", "SET", "zc-min", original["zc-min"])
            admin.command("CONFIG", "SET", "client-output-buffer-limit",
                          original["client-output-buffer-limit"])
            admin.command("DEL", f"{token}:borrow")
        finally:
            admin.close()

    print("limits: PASS", flush=True)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Directed TLS transport battery and ephemeral certificate generator.

Generate:
  tests/tls.py --generate DIR

Run against a purpose-booted server:
  tests/tls.py HOST TLS_PORT CERT_DIR yes|optional|no [--plain-port PORT] [--full]
"""

import argparse
import os
import socket
import ssl
import struct
import subprocess
import sys
import threading
import time


def run(*args):
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def generate(directory):
    os.makedirs(directory, exist_ok=True)
    def path(name):
        return os.path.join(directory, name)

    run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2",
        "-subj", "/CN=TomoKV-Test-CA", "-keyout", path("ca.key"), "-out", path("ca.crt"))
    run("openssl", "req", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=localhost",
        "-addext", "subjectAltName=IP:127.0.0.1,DNS:localhost",
        "-keyout", path("server.key"), "-out", path("server.csr"))
    run("openssl", "x509", "-req", "-days", "2", "-in", path("server.csr"),
        "-CA", path("ca.crt"), "-CAkey", path("ca.key"), "-CAcreateserial",
        "-copy_extensions", "copy", "-out", path("server.crt"))
    run("openssl", "req", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=tomokv-client",
        "-keyout", path("client.key"), "-out", path("client.csr"))
    run("openssl", "x509", "-req", "-days", "2", "-in", path("client.csr"),
        "-CA", path("ca.crt"), "-CAkey", path("ca.key"), "-CAserial", path("ca.srl"),
        "-out", path("client.crt"))

    run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2",
        "-subj", "/CN=TomoKV-Wrong-CA", "-keyout", path("bad-ca.key"),
        "-out", path("bad-ca.crt"))
    run("openssl", "req", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=bad-client",
        "-keyout", path("bad-client.key"), "-out", path("bad-client.csr"))
    run("openssl", "x509", "-req", "-days", "2", "-in", path("bad-client.csr"),
        "-CA", path("bad-ca.crt"), "-CAkey", path("bad-ca.key"), "-CAcreateserial",
        "-out", path("bad-client.crt"))
    print(directory)


def frame(*args):
    values = [value if isinstance(value, bytes) else str(value).encode() for value in args]
    return (b"*%d\r\n" % len(values) +
            b"".join(b"$%d\r\n" % len(value) + value + b"\r\n" for value in values))


class RespReader:
    def __init__(self, stream):
        self.stream = stream

    def read(self):
        kind = self.stream.read(1)
        if not kind:
            raise EOFError("connection closed")
        line = self.stream.readline()
        if not line.endswith(b"\r\n"):
            raise AssertionError("bad RESP line %r" % (kind + line,))
        value = line[:-2]
        if kind == b"+":
            return value
        if kind == b"-":
            return RuntimeError(value.decode("utf-8", "replace"))
        if kind == b":":
            return int(value)
        if kind == b"$":
            size = int(value)
            if size == -1:
                return None
            payload = self.stream.read(size)
            if len(payload) != size or self.stream.read(2) != b"\r\n":
                raise AssertionError("bad bulk payload")
            return payload
        if kind == b"*":
            count = int(value)
            return None if count == -1 else [self.read() for _ in range(count)]
        raise AssertionError("unknown RESP type %r" % kind)


class Conn:
    def __init__(self, host, port, context=None, client_cert=None, timeout=10):
        raw = socket.create_connection((host, port), timeout=timeout)
        if context:
            if client_cert:
                context.load_cert_chain(client_cert[0], client_cert[1])
            self.sock = context.wrap_socket(raw, server_hostname=host)
        else:
            self.sock = raw
        self.sock.settimeout(timeout)
        self.file = self.sock.makefile("rb")
        self.reader = RespReader(self.file)

    def command(self, *args):
        self.sock.sendall(frame(*args))
        return self.reader.read()

    def close(self, graceful=False, reset=False):
        if self.sock is None:
            return
        if reset:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        self.file.close()
        if graceful and isinstance(self.sock, ssl.SSLSocket):
            raw = self.sock.unwrap()
            raw.close()
        else:
            self.sock.close()
        self.sock = None


def context(cert_dir, client="none", tls12=False, tls13=False):
    ctx = ssl.create_default_context(cafile=os.path.join(cert_dir, "ca.crt"))
    ctx.check_hostname = True
    if tls12:
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    elif tls13:
        ctx.minimum_version = ssl.TLSVersion.TLSv1_3
        ctx.maximum_version = ssl.TLSVersion.TLSv1_3
    if client == "good":
        ctx.load_cert_chain(os.path.join(cert_dir, "client.crt"),
                            os.path.join(cert_dir, "client.key"))
    elif client == "bad":
        ctx.load_cert_chain(os.path.join(cert_dir, "bad-client.crt"),
                            os.path.join(cert_dir, "bad-client.key"))
    return ctx


def expect_tls_result(host, port, cert_dir, client, succeeds, label):
    try:
        conn = Conn(host, port, context(cert_dir, client))
        answer = conn.command("PING")
        conn.close()
        if not succeeds or answer != b"PONG":
            raise AssertionError("%s unexpectedly succeeded: %r" % (label, answer))
    except (ssl.SSLError, OSError, EOFError) as error:
        if succeeds:
            raise AssertionError("%s unexpectedly failed: %s" % (label, error)) from error
    print("  ok  " , label, flush=True)


def auth_matrix(host, port, cert_dir, mode):
    if mode == "yes":
        expect_tls_result(host, port, cert_dir, "none", False, "yes rejects missing client cert")
        expect_tls_result(host, port, cert_dir, "bad", False, "yes rejects untrusted client cert")
        expect_tls_result(host, port, cert_dir, "good", True, "yes accepts trusted client cert")
    elif mode == "optional":
        expect_tls_result(host, port, cert_dir, "none", True, "optional accepts missing client cert")
        expect_tls_result(host, port, cert_dir, "bad", False, "optional rejects untrusted client cert")
        expect_tls_result(host, port, cert_dir, "good", True, "optional accepts trusted client cert")
    else:
        expect_tls_result(host, port, cert_dir, "none", True, "no accepts missing client cert")
        expect_tls_result(host, port, cert_dir, "good", True, "no accepts configured client cert")

    wrong = ssl.create_default_context(cafile=os.path.join(cert_dir, "bad-ca.crt"))
    try:
        Conn(host, port, wrong)
    except ssl.SSLError:
        print("  ok   client rejects untrusted server certificate", flush=True)
    else:
        raise AssertionError("client accepted server certificate from the wrong CA")


class BioStream:
    def __init__(self, raw, engine, incoming, outgoing):
        self.raw = raw
        self.engine = engine
        self.incoming = incoming
        self.outgoing = outgoing
        self.buf = bytearray()

    def flush(self):
        while self.outgoing.pending:
            self.raw.sendall(self.outgoing.read())

    def fill(self):
        self.flush()
        encrypted = self.raw.recv(65536)
        if not encrypted:
            self.incoming.write_eof()
            raise EOFError("TLS peer closed")
        self.incoming.write(encrypted)

    def read(self, count):
        while len(self.buf) < count:
            try:
                chunk = self.engine.read(max(16384, count - len(self.buf)))
                if not chunk:
                    raise EOFError("TLS close_notify")
                self.buf.extend(chunk)
            except ssl.SSLWantReadError:
                self.fill()
        out = bytes(self.buf[:count])
        del self.buf[:count]
        return out

    def readline(self):
        while True:
            end = self.buf.find(b"\n")
            if end >= 0:
                out = bytes(self.buf[:end + 1])
                del self.buf[:end + 1]
                return out
            try:
                chunk = self.engine.read(16384)
                if not chunk:
                    raise EOFError("TLS close_notify")
                self.buf.extend(chunk)
            except ssl.SSLWantReadError:
                self.fill()


def torn_record(host, port, cert_dir):
    # SSLObject exposes the exact application ciphertext so TCP can split one TLS record in half.
    # The server must produce no RESP bytes from the first fragment and must parse both this request
    # and the next record correctly after the remainder arrives.
    raw = socket.create_connection((host, port), timeout=5)
    incoming, outgoing = ssl.MemoryBIO(), ssl.MemoryBIO()
    engine = context(cert_dir, tls12=True).wrap_bio(
        incoming, outgoing, server_side=False, server_hostname=host)
    stream = BioStream(raw, engine, incoming, outgoing)
    while True:
        try:
            engine.do_handshake()
            stream.flush()
            break
        except ssl.SSLWantReadError:
            stream.fill()

    value = b"torn-record-value:" + os.urandom(2048)
    engine.write(frame("SET", "tls:torn", value) + frame("GET", "tls:torn"))
    ciphertext = bytearray()
    while outgoing.pending:
        ciphertext.extend(outgoing.read())
    if len(ciphertext) < 12 or ciphertext[0] not in (0x17, 0x16):
        raise AssertionError("application write did not produce a TLS record")
    split = max(6, len(ciphertext) // 2)
    raw.sendall(ciphertext[:split])
    raw.settimeout(0.15)
    try:
        premature = raw.recv(1)
    except socket.timeout:
        premature = b""
    if premature:
        raise AssertionError("server answered before the torn TLS record was complete")
    raw.settimeout(5)
    raw.sendall(ciphertext[split:])
    reader = RespReader(stream)
    if reader.read() != b"OK" or reader.read() != value:
        raise AssertionError("torn TLS record desynchronized SET/GET")
    engine.write(frame("PING"))
    stream.flush()
    if reader.read() != b"PONG":
        raise AssertionError("parser remained desynchronized after torn record")
    raw.close()
    print("  ok   torn TLS record cannot advance plaintext parser cursors", flush=True)


def parse_stats(conn):
    reply = conn.command("INFO", "STATS")
    if not isinstance(reply, bytes):
        raise AssertionError("INFO STATS returned %r" % (reply,))
    result = {}
    for line in reply.decode().splitlines():
        if ":" in line:
            key, value = line.split(":", 1)
            result[key] = value
    return result


def handshake_matrix(host, tls_port, cert_dir, client_kind, admin, expect_ktls):
    # Let the auth-matrix connections cross the IO loop's deferred-free fence so only the admin
    # contributes to the active gauge before each mechanism assertion.
    time.sleep(0.2)
    baseline = parse_stats(admin)
    if expect_ktls and int(baseline.get("tls_ktls_active", "0")) != 1:
        raise AssertionError("kTLS active baseline is not the sole admin connection")
    if not expect_ktls and int(baseline.get("tls_ktls_active", "0")) != 0:
        raise AssertionError("forced fallback unexpectedly has an active kTLS connection")

    for label, kwargs in (("TLSv1.2", {"tls12": True}),
                          ("TLSv1.3", {"tls13": True})):
        fallback_before = int(parse_stats(admin).get("tls_ktls_fallback", "0"))
        probe = Conn(host, tls_port, context(cert_dir, client_kind, **kwargs))
        if probe.sock.version() != label or probe.command("PING") != b"PONG":
            raise AssertionError("%s handshake/RESP mismatch" % label)
        stats = parse_stats(admin)
        active_after = int(stats.get("tls_ktls_active", "0"))
        fallback_after = int(stats.get("tls_ktls_fallback", "0"))
        if expect_ktls:
            if active_after < 2:
                raise AssertionError("%s did not engage bidirectional kTLS" % label)
        elif active_after != 0 or fallback_after <= fallback_before:
            raise AssertionError("%s did not take forced userspace fallback" % label)
        probe.close(graceful=True)
        time.sleep(0.05)
    print("  ok   TLS1.2 + TLS1.3 handshake matrix (%s)" %
          ("kTLS" if expect_ktls else "forced fallback"), flush=True)


def abrupt_midstream(host, tls_port, cert_dir, client_kind):
    # Terminate in the middle of an inbound 1 MiB request body.
    inbound = Conn(host, tls_port, context(cert_dir, client_kind), timeout=10)
    request = frame("SET", "tls:abrupt-in", os.urandom(1024 * 1024))
    inbound.sock.sendall(request[:len(request) // 2])
    inbound.close(reset=True)

    # Terminate while a 1 MiB response is being delivered in the opposite direction.
    setup = Conn(host, tls_port, context(cert_dir, client_kind), timeout=10)
    value = os.urandom(1024 * 1024)
    if setup.command("SET", "tls:abrupt-out", value) != b"OK":
        raise AssertionError("abrupt outbound setup failed")
    setup.sock.sendall(frame("GET", "tls:abrupt-out"))
    if not setup.sock.recv(4096):
        raise AssertionError("abrupt outbound response never started")
    setup.close(reset=True)
    time.sleep(0.1)

    control = Conn(host, tls_port, context(cert_dir, client_kind))
    if control.command("PING") != b"PONG":
        raise AssertionError("server did not recover after abrupt mid-stream disconnects")
    control.close()
    print("  ok   abrupt disconnects during 1MiB input and output", flush=True)


def full_battery(host, tls_port, plain_port, cert_dir, mode, expect_ktls):
    client_kind = "good" if mode == "yes" else "none"
    admin = Conn(host, tls_port, context(cert_dir, client_kind), timeout=60)
    suppressed_before_get = int(parse_stats(admin).get("tls_zc_suppressed", "0"))

    config_reply = admin.command("CONFIG", "GET", "tls-*")
    if not isinstance(config_reply, list) or len(config_reply) != 22:
        raise AssertionError("CONFIG GET tls-* omitted a TLS knob: %r" % (config_reply,))
    config_values = dict(zip(config_reply[0::2], config_reply[1::2]))
    if (config_values.get(b"tls-port") != str(tls_port).encode() or
            config_values.get(b"tls-auth-clients") != mode.encode() or
            config_values.get(b"tls-ktls") != (b"yes" if expect_ktls else b"no")):
        raise AssertionError("CONFIG GET did not preserve TLS listener/auth values")
    immutable = admin.command("CONFIG", "SET", "tls-port", str(tls_port))
    if not isinstance(immutable, RuntimeError) or "immutable" not in str(immutable):
        raise AssertionError("CONFIG SET changed a boot-only TLS knob")
    print("  ok   all TLS knobs are visible and boot-only", flush=True)

    handshake_matrix(host, tls_port, cert_dir, client_kind, admin, expect_ktls)

    values = [os.urandom(size) for size in (1024, 16384, 65536, 1024 * 1024, 8 * 1024 * 1024)]
    request = bytearray()
    for i, value in enumerate(values):
        request.extend(frame("SET", "tls:big:%d" % i, value))
        request.extend(frame("GET", "tls:big:%d" % i))
    admin.sock.sendall(request)
    for value in values:
        if admin.reader.read() != b"OK" or admin.reader.read() != value:
            raise AssertionError("large pipelined TLS round-trip mismatch")
    suppressed_after_get = int(parse_stats(admin).get("tls_zc_suppressed", "0"))
    if suppressed_after_get <= suppressed_before_get:
        raise AssertionError("TLS GET copy-only borrow producer gate did not fire")
    print("  ok   8MiB + boundary values pipeline byte-identically", flush=True)
    print("  ok   TLS GET borrow producer selected the copy-only handler", flush=True)

    mget_values = []
    request = bytearray()
    for i in range(16):
        value = os.urandom(20000 + i)
        mget_values.append(value)
        request.extend(frame("SET", "tls:mget:%d" % i, value))
    request.extend(frame("MGET", *("tls:mget:%d" % i for i in range(16))))
    admin.sock.sendall(request)
    if any(admin.reader.read() != b"OK" for _ in range(16)):
        raise AssertionError("MGET setup failed")
    if admin.reader.read() != mget_values:
        raise AssertionError("TLS MGET borrow-gate copy mismatch")
    suppressed_after_mget = int(parse_stats(admin).get("tls_zc_suppressed", "0"))
    if suppressed_after_mget <= suppressed_after_get:
        raise AssertionError("TLS cross-shard MGET borrow producer gate did not fire")
    print("  ok   cross-shard MGET copies and releases borrowed plaintext", flush=True)

    if admin.command("RESET") != b"RESET" or admin.command("PING") != b"PONG":
        raise AssertionError("RESET broke TLS transport state")
    print("  ok   RESET preserves TLS transport", flush=True)

    torn_record(host, tls_port, cert_dir)
    abrupt_midstream(host, tls_port, cert_dir, client_kind)

    errors = []
    def worker(use_tls, index):
        try:
            conn = Conn(host, tls_port if use_tls else plain_port,
                        context(cert_dir, client_kind) if use_tls else None)
            for n in range(40):
                key = "tls:mixed:%d:%d:%d" % (use_tls, index, n)
                value = os.urandom(256)
                if conn.command("SET", key, value) != b"OK" or conn.command("GET", key) != value:
                    raise AssertionError("mixed transport data mismatch")
            conn.close(graceful=use_tls)
        except Exception as error:  # surfaced after every thread joins
            errors.append(repr(error))

    threads = [threading.Thread(target=worker, args=(kind, i))
               for kind in (False, True) for i in range(4)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(30)
    if errors or any(thread.is_alive() for thread in threads):
        raise AssertionError("mixed plain/TLS clients failed: %r" % errors)
    print("  ok   concurrent plaintext and TLS clients", flush=True)

    # Wrong transport in both directions must terminate cleanly rather than hang or cross-parse.
    raw = socket.create_connection((host, tls_port), timeout=2)
    raw.sendall(frame("PING"))
    raw.settimeout(2)
    try:
        answer = raw.recv(256)
    except (ConnectionResetError, socket.timeout):
        answer = b""
    raw.close()
    if answer.startswith(b"+") or answer.startswith(b"-"):
        raise AssertionError("TLS port emitted plaintext RESP")
    try:
        Conn(host, plain_port, context(cert_dir, client_kind), timeout=2)
    except (ssl.SSLError, OSError, EOFError):
        pass
    else:
        raise AssertionError("TLS client unexpectedly negotiated on plaintext port")
    print("  ok   wrong-transport connections fail cleanly", flush=True)

    # RST during handshake and after an operation exercise deferred BIO cleanup.
    for _ in range(16):
        raw = socket.create_connection((host, tls_port), timeout=2)
        raw.sendall(b"\x16\x03\x03\x00\x80" + os.urandom(9))
        raw.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        raw.close()
    reset = Conn(host, tls_port, context(cert_dir, client_kind))
    if reset.command("PING") != b"PONG":
        raise AssertionError("pre-RST operation failed")
    reset.close(reset=True)
    time.sleep(0.3)

    stats = parse_stats(admin)
    required_positive = ("tls_handshakes_completed", "tls_zc_suppressed",
                         "plain_connections_received")
    missing = [name for name in required_positive if int(stats.get(name, "0")) <= 0]
    if missing:
        raise AssertionError("TLS mechanisms did not fire: %s" % ", ".join(missing))
    if expect_ktls:
        if int(stats.get("tls_ktls_active", "0")) <= 0:
            raise AssertionError("kTLS mechanism counter did not remain active")
    else:
        userspace_counters = ("tls_ciphertext_input_bytes", "tls_plaintext_input_bytes",
                              "tls_ciphertext_output_bytes", "tls_plaintext_output_bytes")
        missing = [name for name in userspace_counters if int(stats.get(name, "0")) <= 0]
        if missing or int(stats.get("tls_ktls_active", "0")) != 0 or \
                int(stats.get("tls_ktls_fallback", "0")) <= 0:
            raise AssertionError("userspace TLS fallback did not fire: %s" % ", ".join(missing))
    print("  ok   TLS counters prove handshake/transport/zc/plain arms fired", flush=True)

    admin.close(graceful=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("host", nargs="?")
    parser.add_argument("tls_port", nargs="?", type=int)
    parser.add_argument("cert_dir", nargs="?")
    parser.add_argument("mode", nargs="?", choices=("yes", "optional", "no"))
    parser.add_argument("--plain-port", type=int)
    parser.add_argument("--full", action="store_true")
    parser.add_argument("--expect-ktls", choices=("yes", "no"), default="yes")
    parser.add_argument("--generate", metavar="DIR")
    args = parser.parse_args()
    if args.generate:
        generate(args.generate)
        return
    if not all((args.host, args.tls_port, args.cert_dir, args.mode)):
        parser.error("HOST TLS_PORT CERT_DIR MODE are required")
    auth_matrix(args.host, args.tls_port, args.cert_dir, args.mode)
    if args.full:
        if not args.plain_port:
            parser.error("--full requires --plain-port")
        full_battery(args.host, args.tls_port, args.plain_port, args.cert_dir, args.mode,
                     args.expect_ktls == "yes")
    print("TLS PASS (%s)" % args.mode)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print("TLS FAIL: %s" % error, file=sys.stderr)
        raise

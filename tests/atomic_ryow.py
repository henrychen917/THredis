#!/usr/bin/env python3
"""Epoch-MVCC per-connection ordering and mixed-write semantics gate."""

import socket
import sys
import threading
import time


HOST, PORT = sys.argv[1], int(sys.argv[2])
FAIL = 0


def note(name, ok, extra=""):
    global FAIL
    print(("  ok   " if ok else "  FAIL ") + name + (" " + extra if extra else ""), flush=True)
    if not ok:
        FAIL += 1


def frame(*args):
    out = bytearray(b"*%d\r\n" % len(args))
    for arg in args:
        if isinstance(arg, str):
            arg = arg.encode()
        out += b"$%d\r\n" % len(arg) + arg + b"\r\n"
    return bytes(out)


class Resp:
    def __init__(self):
        self.sock = socket.create_connection((HOST, PORT), timeout=30)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.file = self.sock.makefile("rb")

    def close(self):
        self.file.close()
        self.sock.close()

    def read(self):
        line = self.file.readline()
        if not line:
            raise EOFError("server closed")
        kind = line[:1]
        if kind == b"+": return line[1:-2]
        if kind == b"-": raise RuntimeError(line[1:-2].decode(errors="replace"))
        if kind == b":": return int(line[1:-2])
        if kind == b"$":
            size = int(line[1:-2])
            if size == -1: return None
            data = self.file.read(size)
            if self.file.read(2) != b"\r\n": raise ValueError("bad bulk")
            return data
        if kind == b"*":
            count = int(line[1:-2])
            return None if count == -1 else [self.read() for _ in range(count)]
        raise ValueError(repr(line[:20]))

    def cmd(self, *args):
        self.sock.sendall(frame(*args))
        return self.read()


def mset_args(keys, value):
    args = ["MSET"]
    for key in keys:
        args.extend((key, value))
    return args


admin = Resp()
note("enable atomic lane", admin.cmd("CONFIG", "SET", "atomic", "1") == b"OK")
keys = ["ary:k%d" % i for i in range(8)]
admin.cmd("DEL", *keys)

# Atomic writes carry no parse barrier. Send all three frames at once to prove that the owner-side
# same-key hazard alone preserves RYOW for both a plain GET and snapshot MGET.
c = Resp()
burst = (frame(*mset_args(keys, "ryow:one")) + frame("GET", keys[0]) +
         frame("MGET", *keys))
c.sock.sendall(burst)
r1, r2, r3 = c.read(), c.read(), c.read()
note("atomic MSET then plain GET/MGET RYOW",
     r1 == b"OK" and r2 == b"ryow:one" and r3 == [b"ryow:one"] * 8,
     "replies=%r/%r/%r" % (r1, r2, r3))
c.close()

# The same precise hazard applies to a younger plain write: it must not physically run before the
# older atomic group decides, even though both frames were parsed and dispatched together.
orderkeys = ["ary:order%d" % i for i in range(8)]
admin.cmd("DEL", *orderkeys)
c = Resp()
c.sock.sendall(frame(*mset_args(orderkeys, "order:atomic")) +
               frame("SET", orderkeys[0], "order:plain") +
               frame("MGET", *orderkeys))
o1, o2, ov = c.read(), c.read(), c.read()
note("atomic MSET then plain SET keeps same-key program order",
     o1 == b"OK" and o2 == b"OK" and
     ov == [b"order:plain"] + [b"order:atomic"] * 7,
     "replies=%r/%r values=%r" % (o1, o2, ov))
c.close()

# MSETNX's decision is made in its one owner pass. A younger same-key command waits at the owner and
# observes the first committed result even when both frames arrive in one recv.
nxkeys = ["ary:nx%d" % i for i in range(8)]
admin.cmd("DEL", *nxkeys)
c = Resp()
first = ["MSETNX"]
second = ["MSETNX"]
for key in nxkeys:
    first.extend((key, "nx:first"))
    second.extend((key, "nx:second"))
c.sock.sendall(frame(*first) + frame(*second) + frame("MGET", *nxkeys))
n1, n2, nv = c.read(), c.read(), c.read()
note("MSETNX single-hop serial semantics",
     n1 == 1 and n2 == 0 and nv == [b"nx:first"] * 8,
     "replies=%r/%r values=%r" % (n1, n2, nv))
c.close()

# Redis permits a duplicate key within one MSETNX and the final occurrence wins. The atomic owner
# must test existence against the pre-command state, not mistake its first private install for an
# external predecessor.
dupkey = "ary:nx-duplicate"
admin.cmd("DEL", dupkey)
dup_result = admin.cmd("MSETNX", dupkey, "nx:early", dupkey, "nx:last")
dup_value = admin.cmd("GET", dupkey)
note("MSETNX duplicate key uses pre-command state",
     dup_result == 1 and dup_value == b"nx:last",
     "reply=%r value=%r" % (dup_result, dup_value))

# An MSETNX group may have installed candidates on other owners before one owner discovers an
# existing key. Epoch zero plus abandonment must hide those candidates from foreign readers and
# from the originating connection's pipelined read alike. Values deliberately exceed the embedded
# string cutover so this also exercises IO-prebuilt KvObj abandonment and owner-side admission.
leakkeys = ["ary:abandon%d" % i for i in range(8)]
admin.cmd("DEL", *leakkeys)
admin.cmd("SET", leakkeys[0], "abandon:guard")
leak_stop = threading.Event()
leak_errors = []
leak_reads = 0


def leak_reader():
    global leak_reads
    reader = Resp()
    try:
        while not leak_stop.is_set():
            values = reader.cmd("MGET", *leakkeys[1:])
            leak_reads += 1
            if any(value is not None for value in values):
                leak_errors.append("foreign observed %r" % (values,))
                break
    except Exception as exc:
        leak_errors.append("foreign:%s" % exc)
    finally:
        reader.close()


thread = threading.Thread(target=leak_reader, daemon=True)
thread.start()
c = Resp()
try:
    for seq in range(256):
        args = ["MSETNX"]
        candidate = ("abandon:leak:%d:" % seq) + "x" * 512
        for key in leakkeys:
            args.extend((key, candidate))
        c.sock.sendall(frame(*args) + frame("MGET", *leakkeys))
        failed, own = c.read(), c.read()
        if failed != 0 or own != [b"abandon:guard"] + [None] * 7:
            leak_errors.append("own observed seq=%d reply=%r values=%r" % (seq, failed, own))
            break
finally:
    c.close()
    leak_stop.set()
    thread.join(timeout=10)
note("abandoned MSETNX candidates are never observable",
     not thread.is_alive() and leak_reads > 0 and not leak_errors,
     "reads=%d errors=%r" % (leak_reads, leak_errors))

# Cross-key atomics on one connection must be admitted concurrently. A tiny window makes overlap
# directly observable (the third frame stalls admission), and the rate comparison guards against a
# future accidental return of the full connection barrier.
admin.cmd("CONFIG", "SET", "atomic-window", "2")
stall_before = int(admin.cmd("INFO", "STATS").split(b"atomic_window_stalls:", 1)[1].split(b"\r\n", 1)[0])
c = Resp()
burst_count = 24
burst = bytearray()
for seq in range(burst_count):
    burst += frame(*mset_args(["ary:overlap:%d:%d" % (seq, i) for i in range(8)], str(seq)))
started = time.perf_counter()
c.sock.sendall(burst)
overlap_ok = all(c.read() == b"OK" for _ in range(burst_count))
pipelined_elapsed = time.perf_counter() - started
c.close()
stall_after = int(admin.cmd("INFO", "STATS").split(b"atomic_window_stalls:", 1)[1].split(b"\r\n", 1)[0])
admin.cmd("CONFIG", "SET", "atomic-window", "256")

c = Resp()
started = time.perf_counter()
for seq in range(burst_count):
    c.cmd(*mset_args(["ary:serial:%d:%d" % (seq, i) for i in range(8)], str(seq)))
serial_elapsed = time.perf_counter() - started
c.close()
pipe_rate = burst_count / max(pipelined_elapsed, 1e-9)
serial_rate = burst_count / max(serial_elapsed, 1e-9)
note("cross-key atomics on one connection overlap",
     overlap_ok and stall_after > stall_before and pipe_rate > serial_rate * 1.10,
     "stalls=%d pipe=%.0f/s serial=%.0f/s" %
     (stall_after - stall_before, pipe_rate, serial_rate))


def consistent_or_absent(values):
    if all(value is None for value in values): return True
    return all(value is not None for value in values) and len(set(values)) == 1


# Atomic DEL and atomic MSET race on the same set. A reader may choose either committed ticket, but
# never a subset of tombstones or a mixture of write signatures.
racekeys = ["ary:race%d" % i for i in range(8)]
admin.cmd(*mset_args(racekeys, "race:init"))
stop = threading.Event()
race_errors = []
torn = 0
reads = 0
race_samples = []
lock = threading.Lock()


def set_racer():
    client = Resp()
    seq = 0
    try:
        while not stop.is_set():
            client.cmd(*mset_args(racekeys, "race:%d" % seq))
            seq += 1
    except Exception as exc:
        race_errors.append("set:%s" % exc)
    finally:
        client.close()


def del_racer():
    client = Resp()
    try:
        while not stop.is_set():
            client.cmd("DEL", *racekeys)
    except Exception as exc:
        race_errors.append("del:%s" % exc)
    finally:
        client.close()


def race_reader():
    global torn, reads
    client = Resp()
    try:
        while not stop.is_set():
            values = client.cmd("MGET", *racekeys)
            with lock:
                reads += 1
                if not consistent_or_absent(values):
                    torn += 1
                    if len(race_samples) < 5: race_samples.append(values)
    except Exception as exc:
        race_errors.append("read:%s" % exc)
    finally:
        client.close()


threads = [threading.Thread(target=set_racer, daemon=True),
           threading.Thread(target=del_racer, daemon=True)] + [
           threading.Thread(target=race_reader, daemon=True) for _ in range(4)]
for thread in threads: thread.start()
time.sleep(2.0)
stop.set()
for thread in threads: thread.join(timeout=10)
note("DEL-vs-MSET is all-or-nothing", torn == 0 and reads > 0 and not race_errors,
     "torn=%d reads=%d errors=%r samples=%r" %
     (torn, reads, race_errors, race_samples))

# A plain SET on a recorded key receives its own ticket. If it wins a read cut, the other seven
# keys must still agree on one complete atomic predecessor; if an atomic group wins, all eight agree.
mixkeys = ["ary:mix%d" % i for i in range(8)]
admin.cmd(*mset_args(mixkeys, "mix:init"))
mix_stop = threading.Event()
mix_errors = []
mix_bad = 0
mix_reads = 0


def atomic_mix_writer():
    client = Resp()
    seq = 0
    try:
        while not mix_stop.is_set():
            client.cmd(*mset_args(mixkeys, "mix:a:%d" % seq))
            seq += 1
    except Exception as exc:
        mix_errors.append("atomic:%s" % exc)
    finally:
        client.close()


def plain_mix_writer():
    client = Resp()
    seq = 0
    try:
        while not mix_stop.is_set():
            client.cmd("SET", mixkeys[0], "mix:p:%d" % seq)
            seq += 1
    except Exception as exc:
        mix_errors.append("plain:%s" % exc)
    finally:
        client.close()


def mix_reader():
    global mix_bad, mix_reads
    client = Resp()
    try:
        while not mix_stop.is_set():
            values = client.cmd("MGET", *mixkeys)
            good = all(value is not None for value in values) and len(set(values[1:])) == 1
            if good and not values[0].startswith(b"mix:p:"):
                good = values[0] == values[1]
            with lock:
                mix_reads += 1
                if not good: mix_bad += 1
    except Exception as exc:
        mix_errors.append("reader:%s" % exc)
    finally:
        client.close()


threads = [threading.Thread(target=atomic_mix_writer, daemon=True),
           threading.Thread(target=plain_mix_writer, daemon=True)] + [
           threading.Thread(target=mix_reader, daemon=True) for _ in range(4)]
for thread in threads: thread.start()
time.sleep(2.0)
mix_stop.set()
for thread in threads: thread.join(timeout=10)
note("plain SET interleaves by ticket on a recorded key",
     mix_bad == 0 and mix_reads > 0 and not mix_errors,
     "bad=%d reads=%d errors=%r" % (mix_bad, mix_reads, mix_errors))

admin.close()
print("ATOMIC_RYOW " + ("PASS" if FAIL == 0 else "FAIL %d" % FAIL), flush=True)
sys.exit(1 if FAIL else 0)

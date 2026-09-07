#!/usr/bin/env python3
"""Epoch-MVCC torn-read, overlapping-writer, window, and live-flip gate."""

import socket
import struct
import sys
import threading
import time

import _lib


# Build identity comes from the caller. Counter movement, exact values, and safety bounds are
# mandatory on every build; only the time spent reaching them is a release-build assertion.
ARGS = sys.argv[1:]
RELEASE_BUILD = "--release-build" in ARGS
ARGS = [arg for arg in ARGS if arg != "--release-build"]
if len(ARGS) != 2:
    raise SystemExit("usage: atomic_torn.py HOST PORT [--release-build]")
HOST, PORT = ARGS[0], int(ARGS[1])
FAIL = 0
BARRIER_TIMEOUT = 10
MECHANISM_TIMEOUT = 30.0  # liveness watchdog, never a successful/skip outcome on exhaustion


def note(name, ok, extra=""):
    global FAIL
    print(("  ok   " if ok else "  FAIL ") + name + (" " + extra if extra else ""), flush=True)
    if not ok:
        FAIL += 1


def skip(name, extra=""):
    print("  SKIP " + name + (" " + extra if extra else ""), flush=True)


def release_note(name, ok, measured):
    if RELEASE_BUILD:
        note(name, ok, measured)
    else:
        skip(name, "requires --release-build; " + measured)


def abort_barriers(*barriers):
    for barrier in barriers:
        try:
            barrier.abort()
        except Exception:
            pass


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
        if kind == b"+":
            return line[1:-2]
        if kind == b"-":
            raise RuntimeError(line[1:-2].decode(errors="replace"))
        if kind == b":":
            return int(line[1:-2])
        if kind == b"$":
            size = int(line[1:-2])
            if size == -1:
                return None
            data = self.file.read(size)
            if self.file.read(2) != b"\r\n":
                raise ValueError("bad bulk trailer")
            return data
        if kind == b"*":
            count = int(line[1:-2])
            if count == -1:
                return None
            return [self.read() for _ in range(count)]
        raise ValueError("bad RESP type %r" % line[:20])

    def cmd(self, *args):
        self.sock.sendall(frame(*args))
        return self.read()


def config(name, value):
    c = Resp()
    try:
        return c.cmd("CONFIG", "SET", name, str(value)) == b"OK"
    finally:
        c.close()


def debug(name, value):
    c = Resp()
    try:
        reply = c.cmd("DEBUG", name, str(value))
        if reply != b"OK":
            raise AssertionError("DEBUG %s returned %r" % (name, reply))
    finally:
        c.close()


def stats():
    c = Resp()
    try:
        body = c.cmd("INFO", "STATS").decode()
    finally:
        c.close()
    result = {}
    for line in body.splitlines():
        if ":" in line:
            key, value = line.split(":", 1)
            if key.startswith("atomic_"):
                result[key] = int(value)
    return result


def required_stat(table, name):
    if name not in table:
        raise AssertionError(
            "INFO STATS has no %s; the battery cannot prove the mechanism fired" % name)
    return table[name]


def gate_geometry():
    """Resolve physical shards and their live OWNER threads on this boot.

    Owners come from the `shard <sid> <owner_tid>` rows of DEBUG LBSIGNALS, never from the `ex`
    role label: under --thread-mode 1s every thread is labelled `io` and still owns shards, so a
    role-based selection found zero executors and aborted with a false reason. The concurrency
    unit for every cross-owner race below is the owning THREAD, whatever its label.
    """
    c = Resp()
    try:
        topo = _lib.topology(c)
        executors = set(topo.owners)
        shard_owner = topo.shard_owner
        if len(executors) < 2:
            raise AssertionError(
                "atomic_torn needs two shard-owning threads for its cross-owner races; this boot "
                "(thread-mode %s) has %d owner(s) %r over %d shard(s)" %
                (topo.mode, len(executors), sorted(executors), len(shard_owner)))

        by_owner = {owner: [] for owner in executors}
        by_shard = {}
        chosen_shard = None
        chosen_other_owner = None
        for index in range(8000):
            key = "at:geometry:%04d" % index
            shard = c.cmd("DEBUG", "SHARD", key)
            if not isinstance(shard, int):
                raise AssertionError(
                    "DEBUG SHARD unavailable; boot with --enable-debug-command yes: %r" % shard)
            if shard not in shard_owner:
                raise AssertionError("DEBUG SHARD returned unreported shard %r" % shard)
            owner = shard_owner[shard]
            by_owner[owner].append(key)
            by_shard.setdefault(shard, []).append(key)
            for same_shard, keys in by_shard.items():
                source_owner = shard_owner[same_shard]
                others = [candidate for candidate in sorted(executors)
                          if candidate != source_owner and len(by_owner[candidate]) >= 8]
                if len(keys) >= 2 and len(by_owner[source_owner]) >= 8 and others:
                    chosen_shard = same_shard
                    chosen_other_owner = others[0]
                    break
            if chosen_shard is not None:
                break
        if chosen_shard is None:
            counts = {owner: len(keys) for owner, keys in by_owner.items()}
            raise AssertionError(
                "could not find same-shard and cross-owner geometry after 8000 probes: %r" % counts)

        source_owner = shard_owner[chosen_shard]
        local_pair = tuple(by_shard[chosen_shard][:2])
        destination = by_owner[chosen_other_owner][0]
        wide_keys = (by_owner[source_owner][:4] + by_owner[chosen_other_owner][:4])
        return {
            "wide_keys": wide_keys,
            "local_pair": local_pair,
            "mover_pair": (local_pair[0], destination),
            "conditional_sources": local_pair,
            "conditional_destination": destination,
            "source_shard": chosen_shard,
            "source_owner": source_owner,
            "destination_owner": chosen_other_owner,
        }
    finally:
        c.close()


def mset(client, keys, signature, value_bytes=0):
    args = ["MSET"]
    pad = b"x" * value_bytes
    encoded = signature.encode() + pad
    for key in keys:
        args.extend((key, encoded))
    return client.cmd(*args)


def signature(values):
    if not values or any(value is None for value in values):
        return None
    heads = [value.split(b"x", 1)[0] for value in values]
    return heads[0] if all(head == heads[0] for head in heads) else b"TORN"


def hammer(prefix, atomic, seconds=2.0, writers=2, readers=4, keys=None):
    if keys is None:
        keys = ["%s:k%d" % (prefix, i) for i in range(8)]
    config("atomic", atomic)
    init = Resp()
    mset(init, keys, "%s:init" % prefix)
    init.close()
    stop = threading.Event()
    start = threading.Barrier(writers + readers + 1, timeout=BARRIER_TIMEOUT)
    lock = threading.Lock()
    torn = 0
    reads = 0
    errors = []
    completed = set()

    def writer(wid):
        nonlocal errors
        client = None
        seq = 0
        try:
            client = Resp()
            start.wait()
            while not stop.is_set():
                sig = "%s:w%d:%d" % (prefix, wid, seq)
                if mset(client, keys, sig) != b"OK":
                    raise AssertionError("MSET did not return OK")
                with lock:
                    completed.add(sig.encode())
                seq += 1
        except Exception as exc:
            abort_barriers(start)
            with lock:
                errors.append("writer%d:%s" % (wid, exc))
        finally:
            if client is not None:
                client.close()

    def reader(rid):
        nonlocal torn, reads, errors
        client = None
        try:
            client = Resp()
            start.wait()
            while not stop.is_set():
                values = client.cmd("MGET", *keys)
                sig = signature(values)
                with lock:
                    reads += 1
                    if sig == b"TORN":
                        torn += 1
        except Exception as exc:
            abort_barriers(start)
            with lock:
                errors.append("reader%d:%s" % (rid, exc))
        finally:
            if client is not None:
                client.close()

    threads = ([threading.Thread(target=writer, args=(i,), daemon=True) for i in range(writers)] +
               [threading.Thread(target=reader, args=(i,), daemon=True) for i in range(readers)])
    for thread in threads:
        thread.start()
    try:
        start.wait()
        time.sleep(seconds)
    except Exception as exc:
        with lock:
            errors.append("controller:%s" % exc)
        abort_barriers(start)
    finally:
        stop.set()
    for thread in threads:
        thread.join(timeout=10)
    threads_still_alive = any(thread.is_alive() for thread in threads)
    with lock:
        if threads_still_alive:
            errors.append("worker threads still alive after join")
        torn_snapshot = torn
        reads_snapshot = reads
        completed_snapshot = set(completed)
        errors_snapshot = list(errors)
    if threads_still_alive:
        final_signature = None
    else:
        final = Resp()
        final_values = final.cmd("MGET", *keys)
        final.close()
        final_signature = signature(final_values)
    return (torn_snapshot, reads_snapshot, errors_snapshot, final_signature, completed_snapshot,
            threads_still_alive)


def rename_hammer(prefix, atomic, keys, seconds=2.0, readers=6):
    config("atomic", atomic)
    left, right = keys
    init = Resp()
    init.cmd("DEL", left, right)
    init.cmd("SET", left, "rename-value")
    init.close()
    stop = threading.Event()
    start = threading.Barrier(readers + 2, timeout=BARRIER_TIMEOUT)
    lock = threading.Lock()
    invalid = 0
    reads = 0
    errors = []

    def writer():
        client = None
        source, destination = left, right
        try:
            client = Resp()
            start.wait()
            while not stop.is_set():
                if client.cmd("RENAME", source, destination) != b"OK":
                    raise AssertionError("RENAME did not return OK")
                source, destination = destination, source
        except Exception as exc:
            abort_barriers(start)
            with lock:
                errors.append("writer:%s" % exc)
        finally:
            if client is not None:
                client.close()

    def reader(rid):
        nonlocal invalid, reads
        client = None
        try:
            client = Resp()
            start.wait()
            while not stop.is_set():
                values = client.cmd("MGET", left, right)
                good = ((values[0] == b"rename-value" and values[1] is None) or
                        (values[0] is None and values[1] == b"rename-value"))
                with lock:
                    reads += 1
                    invalid += not good
        except Exception as exc:
            abort_barriers(start)
            with lock:
                errors.append("reader%d:%s" % (rid, exc))
        finally:
            if client is not None:
                client.close()

    threads = [threading.Thread(target=writer, daemon=True)] + [
        threading.Thread(target=reader, args=(i,), daemon=True) for i in range(readers)]
    for thread in threads:
        thread.start()
    try:
        start.wait()
        time.sleep(seconds)
    except Exception as exc:
        with lock:
            errors.append("controller:%s" % exc)
        abort_barriers(start)
    finally:
        stop.set()
    for thread in threads:
        thread.join(timeout=10)
    threads_still_alive = any(thread.is_alive() for thread in threads)
    with lock:
        if threads_still_alive:
            errors.append("worker threads still alive after join")
        invalid_snapshot = invalid
        reads_snapshot = reads
        errors_snapshot = list(errors)
    if threads_still_alive:
        final_good = False
    else:
        final = Resp()
        final_values = final.cmd("MGET", left, right)
        final.close()
        final_good = ((final_values[0] == b"rename-value" and final_values[1] is None) or
                      (final_values[0] is None and final_values[1] == b"rename-value"))
    return invalid_snapshot, reads_snapshot, errors_snapshot, final_good, threads_still_alive


def sinterstore_hammer(prefix, atomic, sources, seconds=2.0):
    config("atomic", atomic)
    left, right = sources
    destination = prefix + ":dst"
    init = Resp()
    init.cmd("DEL", left, right, destination)
    init.cmd("SADD", left, "base", "moving")
    init.cmd("SADD", right, "base")
    init.cmd("SINTERSTORE", destination, left, right)
    init.close()
    stop = threading.Event()
    start = threading.Barrier(9, timeout=BARRIER_TIMEOUT)
    lock = threading.Lock()
    invalid = 0
    reads = 0
    errors = []

    def mover():
        client = None
        source, target = left, right
        try:
            client = Resp()
            start.wait()
            while not stop.is_set():
                if client.cmd("SMOVE", source, target, "moving") != 1:
                    raise AssertionError("SMOVE lost the moving member")
                source, target = target, source
        except Exception as exc:
            abort_barriers(start)
            with lock:
                errors.append("mover:%s" % exc)
        finally:
            if client is not None:
                client.close()

    def storer(sid):
        client = None
        try:
            client = Resp()
            start.wait()
            while not stop.is_set():
                if client.cmd("SINTERSTORE", destination, left, right) < 1:
                    raise AssertionError("SINTERSTORE lost the stable intersection")
        except Exception as exc:
            abort_barriers(start)
            with lock:
                errors.append("storer%d:%s" % (sid, exc))
        finally:
            if client is not None:
                client.close()

    def reader(rid):
        nonlocal invalid, reads
        client = None
        try:
            client = Resp()
            start.wait()
            while not stop.is_set():
                stored = set(client.cmd("SMEMBERS", destination))
                # Read the sources too: set values cannot be observed with GET/MGET, so
                # SISMEMBER is the type-correct analogue. At every valid cut exactly one source
                # contains "moving", hence no valid SINTERSTORE image contains it.
                client.cmd("SISMEMBER", left, "moving")
                client.cmd("SISMEMBER", right, "moving")
                with lock:
                    reads += 1
                    invalid += stored != {b"base"}
        except Exception as exc:
            abort_barriers(start)
            with lock:
                errors.append("reader%d:%s" % (rid, exc))
        finally:
            if client is not None:
                client.close()

    threads = ([threading.Thread(target=mover, daemon=True)] +
               [threading.Thread(target=storer, args=(i,), daemon=True) for i in range(3)] +
               [threading.Thread(target=reader, args=(i,), daemon=True) for i in range(4)])
    for thread in threads:
        thread.start()
    try:
        start.wait()
        time.sleep(seconds)
    except Exception as exc:
        with lock:
            errors.append("controller:%s" % exc)
        abort_barriers(start)
    finally:
        stop.set()
    for thread in threads:
        thread.join(timeout=10)
    threads_still_alive = any(thread.is_alive() for thread in threads)
    with lock:
        if threads_still_alive:
            errors.append("worker threads still alive after join")
        invalid_snapshot = invalid
        reads_snapshot = reads
        errors_snapshot = list(errors)
    if threads_still_alive:
        final_good = False
    else:
        final = Resp()
        final_members = set(final.cmd("SMEMBERS", destination))
        final.close()
        final_good = final_members == {b"base"}
    return invalid_snapshot, reads_snapshot, errors_snapshot, final_good, threads_still_alive


def lmpop_accounting(prefix, atomic, keys, racers=16, elements=2048):
    config("atomic", atomic)
    empty, source = keys
    expected = [("%s:%05d" % (prefix, i)).encode() for i in range(elements)]
    init = Resp()
    init.cmd("DEL", empty, source)
    for begin in range(0, elements, 256):
        init.cmd("RPUSH", source, *expected[begin:begin + 256])
    init.close()
    start = threading.Barrier(racers + 1, timeout=BARRIER_TIMEOUT)
    lock = threading.Lock()
    popped = []
    errors = []

    def racer(rid):
        local = []
        client = None
        try:
            client = Resp()
            start.wait()
            while True:
                result = client.cmd("LMPOP", "2", empty, source, "LEFT", "COUNT", "7")
                if result is None:
                    break
                if result[0] != source.encode():
                    raise AssertionError("LMPOP selected %r" % (result[0],))
                local.extend(result[1])
        except Exception as exc:
            abort_barriers(start)
            with lock:
                errors.append("racer%d:%s" % (rid, exc))
        finally:
            with lock:
                popped.extend(local)
            if client is not None:
                client.close()

    threads = [threading.Thread(target=racer, args=(i,), daemon=True) for i in range(racers)]
    for thread in threads:
        thread.start()
    try:
        start.wait()
    except Exception as exc:
        with lock:
            errors.append("controller:%s" % exc)
        abort_barriers(start)
    for thread in threads:
        thread.join(timeout=20)
    threads_still_alive = any(thread.is_alive() for thread in threads)
    with lock:
        if threads_still_alive:
            errors.append("worker threads still alive after join")
        popped_snapshot = list(popped)
        errors_snapshot = list(errors)
    if threads_still_alive:
        remaining = -1
    else:
        final = Resp()
        remaining = final.cmd("LLEN", source)
        final.close()
    exact = (not threads_still_alive and not errors_snapshot and remaining == 0 and
             len(popped_snapshot) == elements and len(set(popped_snapshot)) == elements and
             sorted(popped_snapshot) == expected)
    return exact, len(popped_snapshot), errors_snapshot, remaining, threads_still_alive


def conditional_races(prefix, atomic, command, sources, destination, rounds=200,
                      conditional_defer_us=0):
    config("atomic", atomic)
    outputs = [[None, None] for _ in range(rounds)]
    start = threading.Barrier(3, timeout=BARRIER_TIMEOUT)
    finish = threading.Barrier(3, timeout=BARRIER_TIMEOUT)
    lock = threading.Lock()
    errors = []

    def contender(index):
        client = None
        try:
            client = Resp()
            for iteration in range(rounds):
                start.wait()
                outputs[iteration][index] = client.cmd(command, sources[index], destination)
                finish.wait()
        except Exception as exc:
            abort_barriers(start, finish)
            with lock:
                errors.append("contender%d:%s" % (index, exc))
        finally:
            if client is not None:
                client.close()

    threads = [threading.Thread(target=contender, args=(i,), daemon=True) for i in range(2)]
    for thread in threads:
        thread.start()
    admin = None
    hook_armed = False
    anomalies = 0
    try:
        admin = Resp()
        for iteration in range(rounds):
            admin.cmd("MSET", sources[0], "winner-a", sources[1], "winner-b")
            admin.cmd("DEL", destination)
            if iteration == 0 and conditional_defer_us:
                if admin.cmd("DEBUG", "ATOMIC-CONDITIONAL-DEFER",
                             str(conditional_defer_us)) != b"OK":
                    raise AssertionError("could not arm ATOMIC-CONDITIONAL-DEFER")
                hook_armed = True
            start.wait()
            finish.wait()
            values = admin.cmd("MGET", sources[0], sources[1], destination)
            replies = outputs[iteration]
            if sorted(replies) != [0, 1] or values[2] not in (b"winner-a", b"winner-b"):
                anomalies += 1
                continue
            if command == "RENAMENX":
                expected_sources = ([None, b"winner-b"] if values[2] == b"winner-a"
                                    else [b"winner-a", None])
                anomalies += values[:2] != expected_sources
            else:
                anomalies += values[:2] != [b"winner-a", b"winner-b"]
    except Exception as exc:
        abort_barriers(start, finish)
        with lock:
            errors.append("controller:%s" % exc)
    finally:
        if admin is not None:
            if hook_armed:
                try:
                    if admin.cmd("DEBUG", "ATOMIC-CONDITIONAL-DEFER", "0") != b"OK":
                        raise AssertionError("could not disarm ATOMIC-CONDITIONAL-DEFER")
                except Exception as exc:
                    with lock:
                        errors.append("hook-disarm:%s" % exc)
            admin.close()
    for thread in threads:
        thread.join(timeout=10)
    threads_still_alive = any(thread.is_alive() for thread in threads)
    with lock:
        if threads_still_alive:
            errors.append("worker threads still alive after join")
        errors_snapshot = list(errors)
    return anomalies, errors_snapshot, threads_still_alive


# Every routing claim below is proved against this boot's randomized hash seed and current
# shard-to-executor binding. A physical shard pair is required for localfast; actual executor owner
# IDs, not distinct shard numbers, define every concurrent cross-owner arm.
geometry = gate_geometry()
wide_keys = geometry["wide_keys"]
local_pair = geometry["local_pair"]
mover_pair = geometry["mover_pair"]
conditional_sources = geometry["conditional_sources"]
conditional_destination = geometry["conditional_destination"]
note("DEBUG SHARD/LBSIGNALS geometry resolved", True,
     "same_shard=%d source_owner=%d destination_owner=%d" %
     (geometry["source_shard"], geometry["source_owner"], geometry["destination_owner"]))

# Gate-open proof: the OFF alias parks non-lead mutation owners, turning the former kernel-timing
# lottery into one directed publication window. Always disarm because this is the same word used by
# the ON commit-boundary hook below.
debug("ATOMIC-OFF-HOP-DELAY", 100000)
try:
    off_torn, off_reads, off_errors, _, _, off_threads_still_alive = hammer(
        "at:off", 0, seconds=1.0, writers=2, readers=4, keys=wide_keys)
finally:
    debug("ATOMIC-OFF-HOP-DELAY", 0)
note("OFF control exposes torn MSET-8",
     off_torn > 0 and off_reads > 0 and not off_errors and not off_threads_still_alive,
     "torn=%d reads=%d errors=%r threads_still_alive=%r" %
     (off_torn, off_reads, off_errors, off_threads_still_alive))

# Main atomic arm. Hold ticket publication open on demand: this makes both the safe-cut hold and
# predecessor lookup deterministic instead of asking ordinary cleanup timing to expose them.
before = stats()
required_stat(before, "atomic_predecessor_reads")
required_stat(before, "atomic_commit_holds")
required_stat(before, "atomic_promotions")
debug("ATOMIC-COMMIT-DELAY", 2000)
try:
    on_torn, on_reads, on_errors, _, _, on_threads_still_alive = hammer(
        "at:on", 1, seconds=2.5, keys=wide_keys)
finally:
    debug("ATOMIC-COMMIT-DELAY", 0)
after = stats()
pred_delta = (required_stat(after, "atomic_predecessor_reads") -
              required_stat(before, "atomic_predecessor_reads"))
hold_delta = (required_stat(after, "atomic_commit_holds") -
              required_stat(before, "atomic_commit_holds"))
promo_delta = required_stat(after, "atomic_promotions") - required_stat(before, "atomic_promotions")
# V2 batches reclamation on owner passes and the 50ms low-frequency sweep instead of posting a
# cleanup task for every retired group. Poll completed work's cleanup with a watchdog; only the
# original 1.5s completion budget is release-specific. This does not re-arm any race window.
promotion_started = time.monotonic()
deadline = promotion_started + MECHANISM_TIMEOUT
while promo_delta == 0 and time.monotonic() < deadline and not on_errors and not on_torn:
    time.sleep(0.05)
    after = stats()
    promo_delta = required_stat(after, "atomic_promotions") - required_stat(before, "atomic_promotions")
promotion_elapsed = time.monotonic() - promotion_started
note("ON MSET-8/MGET-8 torn-free",
     on_torn == 0 and on_reads > 0 and not on_errors and not on_threads_still_alive,
     "torn=%d reads=%d errors=%r threads_still_alive=%r" %
     (on_torn, on_reads, on_errors, on_threads_still_alive))
# These are window-hit claims, NEVER release-only or skip-on-miss. Induce respectively by
# removing safe-cut holds, bypassing predecessor lookup, or disabling promotion. Each still
# fails on ASAN with a zero delta. Fresh-connection retry bounds for the first two need their own
# measured hit rates; the RENAME control's 5/6 measurement cannot calibrate a different window.
note("ON commit-delay window held a read cut", hold_delta > 0, "delta=%d" % hold_delta)
note("ON exercised predecessor resolution", pred_delta > 0, "delta=%d" % pred_delta)
note("ON exercised promotion", promo_delta > 0, "delta=%d" % promo_delta)
release_note("ON promotion drains within release budget",
             promo_delta > 0 and promotion_elapsed <= 1.5,
             "promotion-drain=%.3fs holds=%d predecessors=%d promotions=%d" %
             (promotion_elapsed, hold_delta, pred_delta, promo_delta))
note("atomic_inflight returns to idle", after.get("atomic_inflight", -1) == 0,
     "value=%d" % after.get("atomic_inflight", -1))

# Same-physical-shard write localfast is a separate atomic arm: no scatter publication window and
# no group entry. Geometry proves the shard; the counter independently proves the path fired.
local_before = stats().get("atomic_localfast", 0)
local_torn, local_reads, local_errors, _, _, local_threads_still_alive = hammer(
    "at:local", 1, seconds=2.0, writers=2, readers=4, keys=local_pair)
local_after = stats().get("atomic_localfast", 0)
note("same-owner atomic MSET-2 localfast is torn-free",
     local_torn == 0 and local_reads > 0 and not local_errors and
     not local_threads_still_alive and local_after > local_before,
     "keys=%r torn=%d reads=%d localfast=%d errors=%r threads_still_alive=%r" %
     (local_pair, local_torn, local_reads, local_after - local_before, local_errors,
      local_threads_still_alive))

# Two overlapping atomic writers on exactly the same key set. The final state after the cleanup
# opportunity supplied by the last MGET must be one complete group, never the physical inversion.
ov_torn, ov_reads, ov_errors, ov_final, ov_completed, ov_threads_still_alive = hammer(
    "at:overlap", 1, seconds=2.5, writers=2, readers=5, keys=wide_keys)
note("overlapping atomic writers never mix",
     ov_torn == 0 and ov_reads > 0 and not ov_errors and not ov_threads_still_alive,
     "torn=%d reads=%d errors=%r threads_still_alive=%r" %
     (ov_torn, ov_reads, ov_errors, ov_threads_still_alive))
note("promotion leaves one exact final group",
     not ov_threads_still_alive and ov_final not in (None, b"TORN") and
     ov_final in ov_completed,
     "final=%r completed=%d threads_still_alive=%r" %
     (ov_final, len(ov_completed), ov_threads_still_alive))

# Broadened movers: RENAME publishes source and destination on distinct owners. Widen exactly that
# OFF mutation wave.
#
# This control is probabilistic in the same way the SINTERSTORE control below is, so it re-rolls the
# same way: only while the run came back CLEAN and every helper stopped, keeping the last real
# result. It does NOT degrade to a skip. This control's entire job is to prove the detector can see
# a tear, so a genuine miss on every roll stays a FAILURE -- that is strictly stronger than the
# skip-on-clean policy used by the SINTERSTORE, COPY and RENAMENX controls.
#
# Measured 2026-09-07 at the gate's geometry (--shards 16 --ratio 6:2, cores 0-7): the outcome is
# almost binary rather than marginal. When the wave lands, ~75,900 of ~76,000 reads are torn; when
# it does not, exactly 0 of ~70,000 are, which is the signature of the hop-delay not taking effect
# for that run rather than of a race narrowly lost. 5 of 6 runs landed it, so four rolls leave a
# residual around 1 in 1,300. To induce the real failure this row protects: make the OFF path
# publish both owners atomically and every roll reports invalid=0.
rename_off = None
for _roll in range(4):
    debug("ATOMIC-OFF-HOP-DELAY", 100000)
    try:
        rename_off = rename_hammer("at:rename-off", 0, mover_pair, seconds=1.0)
    finally:
        debug("ATOMIC-OFF-HOP-DELAY", 0)
    if rename_off[0] > 0 or rename_off[2] or rename_off[4]:
        break
rename_on = None
if not rename_off[2] and not rename_off[4]:
    rename_on = rename_hammer("at:rename-on", 1, mover_pair, seconds=2.0)
note("OFF control exposes torn RENAME",
     rename_off[0] > 0 and rename_off[1] > 0 and not rename_off[2] and not rename_off[4],
     "invalid=%d reads=%d errors=%r threads_still_alive=%r" %
     (rename_off[0], rename_off[1], rename_off[2], rename_off[4]))
if rename_on is None:
    skip("ON RENAME/MGET has exactly one live image", "OFF discovery did not complete cleanly")
else:
    note("ON RENAME/MGET has exactly one live image",
         rename_on[0] == 0 and rename_on[1] > 0 and not rename_on[2] and
         rename_on[3] and not rename_on[4],
         "invalid=%d reads=%d final=%r errors=%r threads_still_alive=%r" %
         (rename_on[0], rename_on[1], rename_on[3], rename_on[2], rename_on[4]))

# Store-family cut consistency. The member moves atomically between two sources, so every valid
# cut has intersection {base}; observing "moving" in the stored result proves a mixed source cut.
store_pair = mover_pair
# The OFF control is probabilistic: preserve the last real result and re-roll only while it is
# clean and every helper stopped. A genuine four-roll miss remains a failure.
store_off = None
for _roll in range(4):
    store_off = sinterstore_hammer("at:store-off", 0, store_pair, seconds=2.5)
    if store_off[0] > 0 or store_off[2] or store_off[4]:
        break
store_on = None
if not store_off[2] and not store_off[4]:
    store_on = sinterstore_hammer("at:store-on", 1, store_pair, seconds=2.0)
if store_off[0] == 0 and store_off[1] > 0 and not store_off[2] and not store_off[4]:
    skip("OFF control exposes impossible SINTERSTORE image",
         "clean run, no mixed image in %d reads (kernel-timing geometry)" % store_off[1])
else:
    note("OFF control exposes impossible SINTERSTORE image",
         store_off[0] > 0 and store_off[1] > 0 and not store_off[2] and not store_off[4],
         "invalid=%d reads=%d errors=%r threads_still_alive=%r" %
         (store_off[0], store_off[1], store_off[2], store_off[4]))
if store_on is None:
    skip("ON SINTERSTORE image matches one source cut",
         "OFF discovery did not complete cleanly")
else:
    note("ON SINTERSTORE image matches one source cut",
         store_on[0] == 0 and store_on[1] > 0 and not store_on[2] and
         store_on[3] and not store_on[4],
         "invalid=%d reads=%d final=%r errors=%r threads_still_alive=%r" %
         (store_on[0], store_on[1], store_on[3], store_on[2], store_on[4]))

# Probe-to-pop races use an empty first key and one shrinking list on a different owner. OFF must
# retain Redis's per-command accounting too; ON additionally exercises fresh-cut owner retries.
pop_pair = mover_pair
pop_off = lmpop_accounting("at:lmpop-off", 0, pop_pair)
pop_on = None
if not pop_off[2] and not pop_off[4]:
    pop_on = lmpop_accounting("at:lmpop-on", 1, pop_pair)
note("OFF LMPOP racers preserve element accounting",
     pop_off[0] and not pop_off[4],
     "popped=%d remaining=%d errors=%r threads_still_alive=%r" %
     (pop_off[1], pop_off[3], pop_off[2], pop_off[4]))
if pop_on is None:
    skip("ON LMPOP racers pop every element exactly once",
         "OFF discovery did not complete cleanly")
else:
    note("ON LMPOP racers pop every element exactly once",
         pop_on[0] and not pop_on[4],
         "popped=%d remaining=%d errors=%r threads_still_alive=%r" %
         (pop_on[1], pop_on[3], pop_on[2], pop_on[4]))

# Conditional movers reserve the destination at owner validation. A losing group may already have
# installed its source image privately; abandonment must keep every such candidate invisible.
# Both sources share one executor and the destination belongs to another. The explicit OFF-only
# park makes the phase-one/phase-two race deterministic without changing atomic-ON scheduling.
renamenx_off = conditional_races(
    "at:renamenx-off", 0, "RENAMENX", conditional_sources,
    conditional_destination, rounds=64, conditional_defer_us=100000)
renamenx_on = None
if not renamenx_off[1] and not renamenx_off[2]:
    renamenx_on = conditional_races(
        "at:renamenx-on", 1, "RENAMENX", conditional_sources,
        conditional_destination, rounds=64)

copy_off = conditional_races(
    "at:copy-off", 0, "COPY", conditional_sources,
    conditional_destination, rounds=64, conditional_defer_us=100000)
copy_on = None
if not copy_off[1] and not copy_off[2]:
    copy_on = conditional_races(
        "at:copy-on", 1, "COPY", conditional_sources,
        conditional_destination, rounds=64)

if renamenx_off[0] == 0 and not renamenx_off[1] and not renamenx_off[2]:
    skip("OFF control exposes RENAMENX losing race",
         "clean run, losing race never manifested (kernel-timing geometry)")
else:
    note("OFF control exposes RENAMENX losing race",
         renamenx_off[0] > 0 and not renamenx_off[1] and not renamenx_off[2],
         "anomalies=%d errors=%r threads_still_alive=%r" % renamenx_off)
if renamenx_on is None:
    skip("ON RENAMENX loser is invisible", "OFF discovery did not complete cleanly")
else:
    note("ON RENAMENX loser is invisible",
         renamenx_on[0] == 0 and not renamenx_on[1] and not renamenx_on[2],
         "anomalies=%d errors=%r threads_still_alive=%r" % renamenx_on)
if copy_off[0] == 0 and not copy_off[1] and not copy_off[2]:
    skip("OFF control exposes COPY losing race",
         "clean run, losing race never manifested (kernel-timing geometry)")
else:
    note("OFF control exposes COPY losing race",
         copy_off[0] > 0 and not copy_off[1] and not copy_off[2],
         "anomalies=%d errors=%r threads_still_alive=%r" % copy_off)
if copy_on is None:
    skip("ON COPY loser is invisible", "OFF discovery did not complete cleanly")
else:
    note("ON COPY loser is invisible",
         copy_on[0] == 0 and not copy_on[1] and not copy_on[2],
         "anomalies=%d errors=%r threads_still_alive=%r" % copy_on)

# Separate admission liveness from live credit reconfiguration/accounting. Both use the
# production bound and must actually enter their window; a clean miss is re-armed, never skipped.
from atomicwindow import held_burst
config("atomic", 1)
try:
    witness = held_burst(HOST, PORT, whole_window=True)
    note("derived atomic window stalls and resumes", True, witness)
except Exception as exc:
    note("derived atomic window stalls and resumes", False, str(exc))

# Resizing was removed with atomic-window. CONFIG SET atomic 1 still rebuilds credit generations;
# require surviving old-generation groups before asserting the fixed bound and reclaimed leases.
try:
    witness = held_burst(HOST, PORT, whole_window=True, reconfigure=True)
    note("atomic reconfiguration preserves derived bound and reclaims leases", True, witness)
except Exception as exc:
    note("atomic reconfiguration preserves derived bound and reclaims leases", False, str(exc))

# Live CONFIG flips under active traffic are a liveness/safety arm. OFF intervals deliberately do
# not promise atomicity; after ending ON, one final group must be read intact.
flip_stop = threading.Event()
flip_errors = []


def flip_writer():
    client = None
    keys = ["at:flip:k%d" % i for i in range(8)]
    seq = 0
    try:
        client = Resp()
        while not flip_stop.is_set():
            mset(client, keys, "flip:%d" % seq)
            seq += 1
    except Exception as exc:
        flip_errors.append(str(exc))
    finally:
        if client is not None:
            client.close()


thread = threading.Thread(target=flip_writer, daemon=True)
thread.start()
for i in range(40):
    if not config("atomic", i & 1):
        flip_errors.append("CONFIG SET failed")
flip_stop.set()
thread.join(timeout=15)
config("atomic", 1)
check = Resp()
flip_keys = ["at:flip:k%d" % i for i in range(8)]
mset(check, flip_keys, "flip:final")
flip_final = signature(check.cmd("MGET", *flip_keys))
check.close()
note("CONFIG SET atomic flip under load", not thread.is_alive() and not flip_errors and
     flip_final == b"flip:final", "errors=%r final=%r" % (flip_errors, flip_final))

# Connection churn is a direct lifetime arm for group entries: the Op and its read-buffer pins may
# retire as soon as the disconnected client's work completes, while owner cleanup can retain the
# ScatterState key span until its last entry is promoted.
churn_before = stats().get("atomic_entries", 0)
for i in range(300):
    try:
        sock = socket.create_connection((HOST, PORT), timeout=10)
        keys = ["at:churn:%d:k%d" % (i, k) for k in range(8)]
        payload = bytearray()
        for seq in range(3):
            args = ["MSET"]
            for key in keys:
                args.extend((key, "churn:%d:%d" % (i, seq)))
            payload += frame(*args)
        sock.sendall(payload)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        sock.close()
    except OSError:
        pass

deadline = time.time() + 5
churn_after = stats()
while (churn_after.get("atomic_inflight", -1) != 0 or
       churn_after.get("atomic_pending_entries", -1) != 0) and time.time() < deadline:
    time.sleep(0.05)
    churn_after = stats()
probe = Resp()
probe_keys = ["at:churn:probe:k%d" % k for k in range(8)]
probe_ok = mset(probe, probe_keys, "churn:probe") == b"OK"
probe_ok = probe_ok and signature(probe.cmd("MGET", *probe_keys)) == b"churn:probe"
probe.close()
note("atomic connection churn drains retained entries",
     probe_ok and churn_after.get("atomic_entries", 0) > churn_before and
     churn_after.get("atomic_inflight", -1) == 0 and
     churn_after.get("atomic_pending_entries", -1) == 0,
     "entries=%d inflight=%d pending=%d" %
     (churn_after.get("atomic_entries", 0) - churn_before,
      churn_after.get("atomic_inflight", -1), churn_after.get("atomic_pending_entries", -1)))

print("ATOMIC_TORN " + ("PASS" if FAIL == 0 else "FAIL %d" % FAIL), flush=True)
sys.exit(1 if FAIL else 0)

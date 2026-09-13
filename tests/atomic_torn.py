#!/usr/bin/env python3
"""Epoch-MVCC torn-read, overlapping-writer, window, and live-flip gate."""

import socket
import struct
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
    start = threading.Barrier(writers + readers)
    lock = threading.Lock()
    torn = 0
    reads = 0
    errors = []
    completed = set()

    def writer(wid):
        nonlocal errors
        client = Resp()
        seq = 0
        try:
            start.wait()
            while not stop.is_set():
                sig = "%s:w%d:%d" % (prefix, wid, seq)
                if mset(client, keys, sig) != b"OK":
                    raise AssertionError("MSET did not return OK")
                with lock:
                    completed.add(sig.encode())
                seq += 1
        except Exception as exc:
            with lock:
                errors.append("writer%d:%s" % (wid, exc))
        finally:
            client.close()

    def reader(rid):
        nonlocal torn, reads, errors
        client = Resp()
        try:
            start.wait()
            while not stop.is_set():
                values = client.cmd("MGET", *keys)
                sig = signature(values)
                with lock:
                    reads += 1
                    if sig == b"TORN":
                        torn += 1
        except Exception as exc:
            with lock:
                errors.append("reader%d:%s" % (rid, exc))
        finally:
            client.close()

    threads = ([threading.Thread(target=writer, args=(i,), daemon=True) for i in range(writers)] +
               [threading.Thread(target=reader, args=(i,), daemon=True) for i in range(readers)])
    for thread in threads:
        thread.start()
    time.sleep(seconds)
    stop.set()
    for thread in threads:
        thread.join(timeout=10)
    final = Resp()
    final_values = final.cmd("MGET", *keys)
    final.close()
    return torn, reads, errors, signature(final_values), completed


def rename_hammer(prefix, atomic, keys, seconds=2.0, readers=6):
    config("atomic", atomic)
    left, right = keys
    init = Resp()
    init.cmd("DEL", left, right)
    init.cmd("SET", left, "rename-value")
    init.close()
    stop = threading.Event()
    start = threading.Barrier(readers + 1)
    lock = threading.Lock()
    invalid = 0
    reads = 0
    errors = []

    def writer():
        client = Resp()
        source, destination = left, right
        try:
            start.wait()
            while not stop.is_set():
                if client.cmd("RENAME", source, destination) != b"OK":
                    raise AssertionError("RENAME did not return OK")
                source, destination = destination, source
        except Exception as exc:
            with lock:
                errors.append("writer:%s" % exc)
        finally:
            client.close()

    def reader(rid):
        nonlocal invalid, reads
        client = Resp()
        try:
            start.wait()
            while not stop.is_set():
                values = client.cmd("MGET", left, right)
                good = ((values[0] == b"rename-value" and values[1] is None) or
                        (values[0] is None and values[1] == b"rename-value"))
                with lock:
                    reads += 1
                    invalid += not good
        except Exception as exc:
            with lock:
                errors.append("reader%d:%s" % (rid, exc))
        finally:
            client.close()

    threads = [threading.Thread(target=writer, daemon=True)] + [
        threading.Thread(target=reader, args=(i,), daemon=True) for i in range(readers)]
    for thread in threads:
        thread.start()
    time.sleep(seconds)
    stop.set()
    for thread in threads:
        thread.join(timeout=10)
    final = Resp()
    final_values = final.cmd("MGET", left, right)
    final.close()
    final_good = ((final_values[0] == b"rename-value" and final_values[1] is None) or
                  (final_values[0] is None and final_values[1] == b"rename-value"))
    return invalid, reads, errors, final_good


def find_torn_rename_pair(prefix):
    """Use the OFF race itself to prove that a pair spans executor owners."""
    last = (-1, 0, ["no pair tried"], False)
    for candidate in range(32):
        pair = ("%s:%d:a" % (prefix, candidate), "%s:%d:b" % (prefix, candidate))
        last = rename_hammer(prefix, 0, pair, seconds=0.35, readers=6)
        if last[0] > 0 and not last[2]:
            return pair, last
    return None, last


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
    start = threading.Barrier(8)
    lock = threading.Lock()
    invalid = 0
    reads = 0
    errors = []

    def mover():
        client = Resp()
        source, target = left, right
        try:
            start.wait()
            while not stop.is_set():
                if client.cmd("SMOVE", source, target, "moving") != 1:
                    raise AssertionError("SMOVE lost the moving member")
                source, target = target, source
        except Exception as exc:
            with lock:
                errors.append("mover:%s" % exc)
        finally:
            client.close()

    def storer(sid):
        client = Resp()
        try:
            start.wait()
            while not stop.is_set():
                if client.cmd("SINTERSTORE", destination, left, right) < 1:
                    raise AssertionError("SINTERSTORE lost the stable intersection")
        except Exception as exc:
            with lock:
                errors.append("storer%d:%s" % (sid, exc))
        finally:
            client.close()

    def reader(rid):
        nonlocal invalid, reads
        client = Resp()
        try:
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
            with lock:
                errors.append("reader%d:%s" % (rid, exc))
        finally:
            client.close()

    threads = ([threading.Thread(target=mover, daemon=True)] +
               [threading.Thread(target=storer, args=(i,), daemon=True) for i in range(3)] +
               [threading.Thread(target=reader, args=(i,), daemon=True) for i in range(4)])
    for thread in threads:
        thread.start()
    time.sleep(seconds)
    stop.set()
    for thread in threads:
        thread.join(timeout=10)
    final = Resp()
    final_members = set(final.cmd("SMEMBERS", destination))
    final.close()
    return invalid, reads, errors, final_members == {b"base"}


def lmpop_accounting(prefix, atomic, keys, racers=16, elements=2048):
    config("atomic", atomic)
    empty, source = keys
    expected = [("%s:%05d" % (prefix, i)).encode() for i in range(elements)]
    init = Resp()
    init.cmd("DEL", empty, source)
    for begin in range(0, elements, 256):
        init.cmd("RPUSH", source, *expected[begin:begin + 256])
    init.close()
    start = threading.Barrier(racers)
    lock = threading.Lock()
    popped = []
    errors = []

    def racer(rid):
        local = []
        client = Resp()
        try:
            start.wait()
            while True:
                result = client.cmd("LMPOP", "2", empty, source, "LEFT", "COUNT", "7")
                if result is None:
                    break
                if result[0] != source.encode():
                    raise AssertionError("LMPOP selected %r" % (result[0],))
                local.extend(result[1])
        except Exception as exc:
            with lock:
                errors.append("racer%d:%s" % (rid, exc))
        finally:
            with lock:
                popped.extend(local)
            client.close()

    threads = [threading.Thread(target=racer, args=(i,), daemon=True) for i in range(racers)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(timeout=20)
    final = Resp()
    remaining = final.cmd("LLEN", source)
    final.close()
    exact = (not any(thread.is_alive() for thread in threads) and not errors and remaining == 0 and
             len(popped) == elements and len(set(popped)) == elements and
             sorted(popped) == expected)
    return exact, len(popped), errors, remaining


def conditional_races(prefix, atomic, command, sources, destination, rounds=200):
    config("atomic", atomic)
    outputs = [[None, None] for _ in range(rounds)]
    start = threading.Barrier(3)
    finish = threading.Barrier(3)
    errors = []

    def contender(index):
        client = Resp()
        try:
            for iteration in range(rounds):
                start.wait()
                outputs[iteration][index] = client.cmd(command, sources[index], destination)
                finish.wait()
        except Exception as exc:
            errors.append("contender%d:%s" % (index, exc))
        finally:
            client.close()

    threads = [threading.Thread(target=contender, args=(i,), daemon=True) for i in range(2)]
    for thread in threads:
        thread.start()
    admin = Resp()
    anomalies = 0
    for iteration in range(rounds):
        admin.cmd("MSET", sources[0], "winner-a", sources[1], "winner-b")
        admin.cmd("DEL", destination)
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
    admin.close()
    for thread in threads:
        thread.join(timeout=10)
    return anomalies, errors, any(thread.is_alive() for thread in threads)


def find_conditional_race_keys(prefix, command, source, destination):
    """Find a second source that makes the OFF two-contender gate observable."""
    last = (-1, ["no candidate tried"], False)
    for candidate in range(32):
        other = "%s:%d" % (prefix, candidate)
        if other in (source, destination):
            continue
        last = conditional_races(
            prefix, 0, command, (source, other), destination, rounds=64)
        if last[0] > 0 and not last[1] and not last[2]:
            return other, last
    return None, last


# Gate-open proof: the test must actually catch the old first-hop physical tear.
off_torn, off_reads, off_errors, _, _ = hammer(
    "at:off", 0, seconds=3.0, writers=4, readers=8)
note("OFF control exposes torn MSET-8", off_torn > 0 and not off_errors,
     "torn=%d reads=%d errors=%r" % (off_torn, off_reads, off_errors))

# Main atomic arm. Both counters are vacuous-validation guards.
before = stats()
on_torn, on_reads, on_errors, _, _ = hammer("at:on", 1, seconds=2.5)
after = stats()
pred_delta = after.get("atomic_predecessor_reads", 0) - before.get("atomic_predecessor_reads", 0)
promo_delta = after.get("atomic_promotions", 0) - before.get("atomic_promotions", 0)
# V2 batches reclamation on owner passes and the 50ms low-frequency sweep instead of posting a
# cleanup task for every retired group. Give that deliberately cold path one bounded tick to fire.
deadline = time.time() + 1.5
while promo_delta == 0 and time.time() < deadline:
    time.sleep(0.05)
    after = stats()
    promo_delta = after.get("atomic_promotions", 0) - before.get("atomic_promotions", 0)
note("ON MSET-8/MGET-8 torn-free", on_torn == 0 and on_reads > 0 and not on_errors,
     "torn=%d reads=%d errors=%r" % (on_torn, on_reads, on_errors))
note("ON exercised predecessor resolution", pred_delta > 0, "delta=%d" % pred_delta)
note("ON exercised promotion", promo_delta > 0, "delta=%d" % promo_delta)
note("atomic_inflight returns to idle", after.get("atomic_inflight", -1) == 0,
     "value=%d" % after.get("atomic_inflight", -1))

# Same-owner write localfast is a separate atomic arm: no cross-shard publication window and no
# group entry. The hash seed is randomized at boot, so discover a distinct-key MSET-2 pair by the
# fired counter instead of copying an implementation hash into the test. With no concurrent load in
# this discovery loop, the first counter transition proves both keys routed to one shard.
local_pair = None
probe = Resp()
local_seen = stats().get("atomic_localfast", 0)
for candidate in range(512):
    pair = ["at:local:%d:a" % candidate, "at:local:%d:b" % candidate]
    mset(probe, pair, "local:probe")
    observed = stats().get("atomic_localfast", 0)
    if observed > local_seen:
        local_pair = pair
        break
    local_seen = observed
probe.close()
local_before = stats().get("atomic_localfast", 0)
if local_pair is not None:
    local_torn, local_reads, local_errors, _, _ = hammer(
        "at:local", 1, seconds=2.0, writers=2, readers=4, keys=local_pair)
else:
    local_torn, local_reads, local_errors = -1, 0, ["no same-shard pair found"]
local_after = stats().get("atomic_localfast", 0)
note("same-owner atomic MSET-2 localfast is torn-free",
     local_pair is not None and local_torn == 0 and local_reads > 0 and
     not local_errors and local_after > local_before,
     "keys=%r torn=%d reads=%d localfast=%d errors=%r" %
     (local_pair, local_torn, local_reads, local_after - local_before, local_errors))

# Two overlapping atomic writers on exactly the same key set. The final state after the cleanup
# opportunity supplied by the last MGET must be one complete group, never the physical inversion.
ov_torn, ov_reads, ov_errors, ov_final, ov_completed = hammer(
    "at:overlap", 1, seconds=2.5, writers=2, readers=5)
note("overlapping atomic writers never mix", ov_torn == 0 and ov_reads > 0 and not ov_errors,
     "torn=%d reads=%d errors=%r" % (ov_torn, ov_reads, ov_errors))
note("promotion leaves one exact final group",
     ov_final not in (None, b"TORN") and ov_final in ov_completed,
     "final=%r completed=%d" % (ov_final, len(ov_completed)))

# Broadened movers: RENAME publishes the source tombstone and destination image with one ticket.
# Discover a cross-owner pair with the OFF tear, then reuse it for the atomic assertion.
mover_pair, rename_off = find_torn_rename_pair("at:rename-route")
if mover_pair is None:
    rename_on = (-1, 0, ["no cross-owner pair found"], False)
else:
    rename_on = rename_hammer("at:rename-on", 1, mover_pair, seconds=2.0)
note("OFF control exposes torn RENAME",
     rename_off[0] > 0 and rename_off[1] > 0 and not rename_off[2],
     "invalid=%d reads=%d errors=%r" % rename_off[:3])
note("ON RENAME/MGET has exactly one live image",
     rename_on[0] == 0 and rename_on[1] > 0 and not rename_on[2] and rename_on[3],
     "invalid=%d reads=%d final=%r errors=%r" %
     (rename_on[0], rename_on[1], rename_on[3], rename_on[2]))

# Store-family cut consistency. The member moves atomically between two sources, so every valid
# cut has intersection {base}; observing "moving" in the stored result proves a mixed source cut.
store_pair = mover_pair
if store_pair is None:
    store_off = store_on = (-1, 0, ["no cross-shard pair found"], False)
else:
    store_off = sinterstore_hammer("at:store-off", 0, store_pair, seconds=2.5)
    store_on = sinterstore_hammer("at:store-on", 1, store_pair, seconds=2.0)
note("OFF control exposes impossible SINTERSTORE image",
     store_off[0] > 0 and store_off[1] > 0 and not store_off[2],
     "invalid=%d reads=%d errors=%r" % store_off[:3])
note("ON SINTERSTORE image matches one source cut",
     store_on[0] == 0 and store_on[1] > 0 and not store_on[2] and store_on[3],
     "invalid=%d reads=%d final=%r errors=%r" %
     (store_on[0], store_on[1], store_on[3], store_on[2]))

# Probe-to-pop races use an empty first key and one shrinking list on a different owner. OFF must
# retain Redis's per-command accounting too; ON additionally exercises fresh-cut owner retries.
pop_pair = mover_pair
if pop_pair is None:
    pop_off = pop_on = (False, 0, ["no cross-shard pair found"], -1)
else:
    pop_off = lmpop_accounting("at:lmpop-off", 0, pop_pair)
    pop_on = lmpop_accounting("at:lmpop-on", 1, pop_pair)
note("OFF LMPOP racers preserve element accounting",
     pop_off[0], "popped=%d remaining=%d errors=%r" %
     (pop_off[1], pop_off[3], pop_off[2]))
note("ON LMPOP racers pop every element exactly once",
     pop_on[0], "popped=%d remaining=%d errors=%r" %
     (pop_on[1], pop_on[3], pop_on[2]))

# Conditional movers reserve the destination at owner validation. A losing group may already have
# installed its source image privately; abandonment must keep every such candidate invisible.
conditional_pair = mover_pair
renamenx_third, renamenx_off = (find_conditional_race_keys(
    "at:renamenx-third", "RENAMENX", conditional_pair[0], conditional_pair[1])
    if conditional_pair else (None, (-1, ["no cross-owner pair found"], False)))
copy_third, copy_off = (find_conditional_race_keys(
    "at:copy-third", "COPY", conditional_pair[0], conditional_pair[1])
    if conditional_pair else (None, (-1, ["no cross-owner pair found"], False)))
if conditional_pair is None or renamenx_third is None or copy_third is None:
    renamenx_off = renamenx_on = copy_off = copy_on = (
        -1, ["no conditional cross-shard keys found"], False)
else:
    conditional_destination = conditional_pair[1]
    renamenx_on = conditional_races(
        "at:renamenx-on", 1, "RENAMENX",
        (conditional_pair[0], renamenx_third), conditional_destination)
    copy_on = conditional_races(
        "at:copy-on", 1, "COPY",
        (conditional_pair[0], copy_third), conditional_destination)
note("OFF control exposes RENAMENX losing race", renamenx_off[0] > 0 and not renamenx_off[1],
     "anomalies=%d errors=%r alive=%r" % renamenx_off)
note("ON RENAMENX loser is invisible", renamenx_on[0] == 0 and not renamenx_on[1] and
     not renamenx_on[2], "anomalies=%d errors=%r alive=%r" % renamenx_on)
note("OFF control exposes COPY losing race", copy_off[0] > 0 and not copy_off[1],
     "anomalies=%d errors=%r alive=%r" % copy_off)
note("ON COPY loser is invisible", copy_on[0] == 0 and not copy_on[1] and not copy_on[2],
     "anomalies=%d errors=%r alive=%r" % copy_on)

# Admission liveness: with a one-group window, the second frame in one received pipeline reaches
# admission before owner notifications for the first are flushed. This makes the fired assertion
# deterministic instead of depending on Python threads winning a scheduling race.
config("atomic", 1)
config("atomic-window", 1)
window_before = stats().get("atomic_window_stalls", 0)
window_run = format(time.time_ns() & 0xfffffff, "x")
window_errors = []
window_client = Resp()
window_frames = []
for sequence in range(64):
    args = ["MSET"]
    for key_index in range(8):
        args.extend(("aw%s:%x:%x" % (window_run, sequence, key_index),
                     "window:%x" % sequence))
    window_frames.append(frame(*args))
try:
    window_client.sock.sendall(b"".join(window_frames))
    for _ in window_frames:
        if window_client.read() != b"OK":
            raise AssertionError("bad window reply")
except Exception as exc:
    window_errors.append(str(exc))
finally:
    window_client.close()
window_after = stats().get("atomic_window_stalls", 0)
note("atomic-window stalls and resumes",
     not window_errors and window_after > window_before,
     "stalls=%d errors=%r" % (window_after - window_before, window_errors))
config("atomic-window", 256)

# Credit leases must preserve the exact configured bound while CONFIG changes it under load. A
# shrink may inherit more already-admitted groups than the new limit; wait for that unavoidable
# debt to retire, then prove no subsequent sample exceeds the bound. Finally, an idle system must
# have returned every leased credit to the pool so a skewed next IO can consume the whole window.
lease_stop = threading.Event()
lease_errors = []


def lease_writer(wid):
    client = Resp()
    keys = ["at:lease:%d:k%d" % (wid, key) for key in range(8)]
    seq = 0
    try:
        while not lease_stop.is_set():
            if mset(client, keys, "lease:%d:%d" % (wid, seq)) != b"OK":
                raise AssertionError("bad lease reply")
            seq += 1
    except Exception as exc:
        lease_errors.append("writer%d:%s" % (wid, exc))
    finally:
        client.close()


lease_threads = [threading.Thread(target=lease_writer, args=(i,), daemon=True) for i in range(8)]
for thread in lease_threads:
    thread.start()
for value in (31, 7, 19, 3):
    if not config("atomic-window", value):
        lease_errors.append("CONFIG atomic-window %d failed" % value)
deadline = time.time() + 3
bounded = False
max_inflight = 0
while time.time() < deadline:
    sample = stats()
    live = sample.get("atomic_inflight", 1000000)
    debt = sample.get("atomic_credit_debt", 1000000)
    if live <= 3 and debt == 0:
        bounded = True
        break
    time.sleep(0.01)
if bounded:
    deadline = time.time() + 0.4
    while time.time() < deadline:
        live = stats().get("atomic_inflight", 1000000)
        max_inflight = max(max_inflight, live)
        if live > 3:
            bounded = False
            break
lease_stop.set()
for thread in lease_threads:
    thread.join(timeout=10)
deadline = time.time() + 3
lease_after = stats()
while lease_after.get("atomic_inflight", -1) != 0 and time.time() < deadline:
    time.sleep(0.01)
    lease_after = stats()
note("atomic-window reconfiguration preserves bound and reclaims leases",
     bounded and not any(thread.is_alive() for thread in lease_threads) and not lease_errors and
     lease_after.get("atomic_inflight", -1) == 0 and
     lease_after.get("atomic_credit_debt", -1) == 0 and
     lease_after.get("atomic_credit_pool", -1) == 3,
     "max=%d pool=%d debt=%d errors=%r" %
     (max_inflight, lease_after.get("atomic_credit_pool", -1),
      lease_after.get("atomic_credit_debt", -1), lease_errors))
config("atomic-window", 256)

# Live CONFIG flips under active traffic are a liveness/safety arm. OFF intervals deliberately do
# not promise atomicity; after ending ON, one final group must be read intact.
flip_stop = threading.Event()
flip_errors = []


def flip_writer():
    client = Resp()
    keys = ["at:flip:k%d" % i for i in range(8)]
    seq = 0
    try:
        while not flip_stop.is_set():
            mset(client, keys, "flip:%d" % seq)
            seq += 1
    except Exception as exc:
        flip_errors.append(str(exc))
    finally:
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

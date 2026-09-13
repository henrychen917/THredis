#!/usr/bin/env python3
"""Directed phase-1 stream semantics, blocking, representation, and memory-floor probes."""

import os
import socket
import sys
import threading
import time


HOST, PORT = sys.argv[1], int(sys.argv[2])
FAIL = 0


def note(name, ok, extra=""):
    global FAIL
    print(("  ok   " if ok else "  FAIL ") + name + (" " + extra if extra else ""),
          flush=True)
    FAIL += not ok


def frame(*args):
    values = [arg if isinstance(arg, bytes) else str(arg).encode() for arg in args]
    return (b"*%d\r\n" % len(values) +
            b"".join(b"$%d\r\n" % len(value) + value + b"\r\n" for value in values))


class Resp:
    def __init__(self):
        self.sock = socket.create_connection((HOST, PORT), timeout=10)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.file = self.sock.makefile("rb")

    def close(self):
        try:
            self.file.close()
        finally:
            self.sock.close()

    def send(self, *args):
        self.sock.sendall(frame(*args))

    def read(self):
        line = self.file.readline()
        if not line:
            raise EOFError("server closed")
        kind, value = line[:1], line[1:-2]
        if kind == b"+": return value
        if kind == b"-": raise RuntimeError(value.decode(errors="replace"))
        if kind == b":": return int(value)
        if kind == b"$":
            size = int(value)
            if size == -1: return None
            data = self.file.read(size)
            if self.file.read(2) != b"\r\n": raise ValueError("bad bulk trailer")
            return data
        if kind == b"*":
            count = int(value)
            return None if count == -1 else [self.read() for _ in range(count)]
        raise ValueError("bad RESP type %r" % line[:20])

    def cmd(self, *args):
        self.send(*args)
        return self.read()

    def pipeline(self, commands):
        self.sock.sendall(b"".join(frame(*command) for command in commands))
        return [self.read() for _ in commands]


def info_value(client, section, name):
    body = client.cmd("INFO", section)
    return int(body.split((name + ":").encode(), 1)[1].split(b"\r\n", 1)[0])


def wait_value(client, section, name, wanted, timeout=3.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if info_value(client, section, name) == wanted: return True
        time.sleep(0.01)
    return info_value(client, section, name) == wanted


def start_wait(*command):
    client = Resp(); result = []; errors = []
    def run():
        try: result.append(client.cmd(*command))
        except Exception as exc: errors.append(str(exc))
    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    return client, thread, result, errors


def xentry(ident, *pairs):
    return [ident.encode(), [item.encode() for item in pairs]]


admin = Resp()
admin.cmd("CONFIG", "SET", "maxmemory", "0")
admin.cmd("FLUSHDB")

# ID grammar, same-ms sequence generation, and stored-last clamping (never wall clock).
note("explicit ID creates stream", admin.cmd("XADD", "stream:ids", "1000-0", "f", "a") == b"1000-0")
note("ms-* increments same-ms sequence",
     admin.cmd("XADD", "stream:ids", "1000-*", "f", "b") == b"1000-1" and
     admin.cmd("XADD", "stream:ids", "1000-*", "f", "c") == b"1000-2")
admin.cmd("XADD", "stream:future", "9999999999999-5", "f", "future")
note("auto ID clamps to stored last, not clock",
     admin.cmd("XADD", "stream:future", "*", "f", "next") == b"9999999999999-6")
created = False
try:
    admin.cmd("XADD", "stream:zero", "0-0", "f", "v")
except RuntimeError:
    created = admin.cmd("EXISTS", "stream:zero") != 0
note("0-0 rejected before key creation", not created)
try:
    admin.cmd("XADD", "stream:ids", "1000-1", "f", "old")
    ordering = False
except RuntimeError as exc:
    ordering = "equal or smaller" in str(exc)
note("duplicate/decreasing IDs rejected", ordering)

# Inclusive/exclusive range bounds, missing sequence grammar, reverse order, and COUNT 0 nil.
forward = admin.cmd("XRANGE", "stream:ids", "1000", "1000")
exclusive = admin.cmd("XRANGE", "stream:ids", "(1000-0", "+")
reverse = admin.cmd("XREVRANGE", "stream:ids", "+", "-")
note("range bounds use start=0/end=UINT64_MAX missing sequences",
     [entry[0] for entry in forward] == [b"1000-0", b"1000-1", b"1000-2"])
note("exclusive '(' range bound", [entry[0] for entry in exclusive] == [b"1000-1", b"1000-2"])
note("reverse range ordering", [entry[0] for entry in reverse] == [b"1000-2", b"1000-1", b"1000-0"])
note("XRANGE COUNT 0 is null array",
     admin.cmd("XRANGE", "stream:ids", "-", "+", "COUNT", "0") is None)

# XDEL parses every ID before mutation and tombstones exactly once.
try:
    admin.cmd("XDEL", "stream:ids", "1000-0", "not-an-id")
    parse_all = False
except RuntimeError:
    parse_all = admin.cmd("XRANGE", "stream:ids", "1000-0", "1000-0") != []
note("XDEL parse-all before apply", parse_all)
note("XDEL tombstone count and idempotence",
     admin.cmd("XDEL", "stream:ids", "1000-0", "1000-9") == 1 and
     admin.cmd("XDEL", "stream:ids", "1000-0") == 0 and
     admin.cmd("XLEN", "stream:ids") == 2)

# Exact MAXLEN/MINID, and '~' accepted with the deliberate exact implementation.
admin.cmd("DEL", "stream:trim")
for ident in range(1, 11): admin.cmd("XADD", "stream:trim", "%d-0" % ident, "f", ident)
note("XTRIM MAXLEN exact boundary",
     admin.cmd("XTRIM", "stream:trim", "MAXLEN", "=", "4") == 6 and
     [e[0] for e in admin.cmd("XRANGE", "stream:trim", "-", "+")] ==
     [b"7-0", b"8-0", b"9-0", b"10-0"])
note("XTRIM MINID exact boundary",
     admin.cmd("XTRIM", "stream:trim", "MINID", "=", "9-0") == 2 and
     [e[0] for e in admin.cmd("XRANGE", "stream:trim", "-", "+")] == [b"9-0", b"10-0"])
admin.cmd("XADD", "stream:trim", "11-0", "f", "11")
note("approximate '~' parses and behaves exact",
     admin.cmd("XTRIM", "stream:trim", "MAXLEN", "~", "1") == 2 and
     admin.cmd("XLEN", "stream:trim") == 1)

# Immediate XREAD is a gather and preserves argument order, including multi-stream replies.
admin.cmd("XADD", "stream:read-a", "1-0", "f", "a")
admin.cmd("XADD", "stream:read-b", "1-0", "f", "b")
note("XREAD immediate single-stream owner route",
     admin.cmd("XREAD", "COUNT", "1", "STREAMS", "stream:read-a", "0-0") ==
     [[b"stream:read-a", [xentry("1-0", "f", "a")]]])
immediate = admin.cmd("XREAD", "COUNT", "1", "STREAMS",
                      "stream:read-b", "stream:read-a", "0-0", "0-0")
note("XREAD immediate multi-stream argument order",
     [item[0] for item in immediate] == [b"stream:read-b", b"stream:read-a"] and
     immediate[0][1] == [xentry("1-0", "f", "b")])
note("XREAD empty reply is null array",
     admin.cmd("XREAD", "STREAMS", "stream:read-a", "1-0") is None)

# Broadcast, not handoff: every plain XREAD waiter observes the same append.
broadcast = [start_wait("XREAD", "COUNT", "1", "BLOCK", "2000", "STREAMS",
                        "stream:broadcast", "$") for _ in range(3)]
wait_value(admin, "CLIENTS", "blocked_clients", 3)
admin.cmd("XADD", "stream:broadcast", "1-0", "f", "wake")
for _, thread, _, _ in broadcast: thread.join(2)
wanted = [[b"stream:broadcast", [xentry("1-0", "f", "wake")]]]
note("blocking XREAD broadcasts one XADD to all waiters",
     all(not thread.is_alive() and not errors and result == [wanted]
         for _, thread, result, errors in broadcast))
for client, _, _, _ in broadcast: client.close()

# '$' is rewritten once when the waiter parks. A second pipelined append may complete before the
# asynchronous gather reruns, but it belongs to the next read, not the wake already claimed by the
# first publication.
dollar_client, dollar_thread, dollar_result, dollar_errors = start_wait(
    "XREAD", "BLOCK", "2000", "STREAMS", "stream:dollar", "$")
wait_value(admin, "CLIENTS", "blocked_clients", 1)
admin.sock.sendall(frame("XADD", "stream:dollar", "1-0", "f", "first") +
                   frame("XADD", "stream:dollar", "2-0", "f", "second"))
append_replies = [admin.read(), admin.read()]
dollar_thread.join(2)
note("blocking '$' freezes the first publication",
     append_replies == [b"1-0", b"2-0"] and not dollar_thread.is_alive() and
     not dollar_errors and dollar_result == [
         [[b"stream:dollar", [xentry("1-0", "f", "first")]]]],
     "reply=%r" % dollar_result)
dollar_client.close()

# Redis freezes the publication before processing a following pipelined delete. The asynchronous
# scatter rerun must therefore not turn a valid wake into nil merely because the entry is gone.
race_client, race_thread, race_result, race_errors = start_wait(
    "XREAD", "BLOCK", "2000", "STREAMS", "stream:delete-race", "$")
wait_value(admin, "CLIENTS", "blocked_clients", 1)
admin.sock.sendall(frame("XADD", "stream:delete-race", "1-0", "f", "published") +
                   frame("XDEL", "stream:delete-race", "1-0"))
race_mutations = [admin.read(), admin.read()]
race_thread.join(2)
note("blocking XREAD freezes publication across following XDEL",
     race_mutations == [b"1-0", 1] and not race_thread.is_alive() and not race_errors and
     race_result == [[[b"stream:delete-race", [xentry("1-0", "f", "published")]]]])
race_client.close()

# COUNT is honored again on the wake/re-entry path, not only by immediate XREAD.
count_client, count_thread, count_result, count_errors = start_wait(
    "XREAD", "COUNT", "1", "BLOCK", "2000", "STREAMS", "stream:count-wake", "$")
wait_value(admin, "CLIENTS", "blocked_clients", 1)
admin.sock.sendall(b"".join(frame("XADD", "stream:count-wake", "%d-0" % i, "f", i)
                            for i in range(1, 6)))
count_appends = [admin.read() for _ in range(5)]
count_thread.join(2)
note("blocking XREAD COUNT applies on wake",
     count_appends == [b"%d-0" % i for i in range(1, 6)] and
     not count_thread.is_alive() and not count_errors and count_result == [
         [[b"stream:count-wake", [xentry("1-0", "f", "1")]]]],
     "reply=%r" % count_result)
count_client.close()

# XREAD is scattered across all named stream owners. Waking the last argument must not make the
# response look like a single-key command or reorder it ahead of an earlier ready argument.
multi_keys = ["stream:block-a", "stream:block-b", "stream:block-c"]
multi_client, multi_thread, multi_result, multi_errors = start_wait(
    "XREAD", "BLOCK", "2000", "STREAMS", *multi_keys, "$", "$", "$")
wait_value(admin, "CLIENTS", "blocked_clients", 1)
admin.cmd("XADD", multi_keys[2], "1-0", "f", "third")
multi_thread.join(2)
note("blocking XREAD wakes from a multi-stream scatter",
     not multi_thread.is_alive() and not multi_errors and multi_result == [[
         [multi_keys[2].encode(), [xentry("1-0", "f", "third")]]]],
     "reply=%r" % multi_result)
multi_client.close()

# Empty-after-delete uses last valid ID, not last-generated ID, so it times out without spinning.
admin.cmd("XADD", "stream:empty", "1-0", "f", "v")
admin.cmd("XDEL", "stream:empty", "1-0")
started = time.monotonic()
empty_reply = admin.cmd("XREAD", "BLOCK", "120", "STREAMS", "stream:empty", "0-0")
elapsed = time.monotonic() - started
note("empty tombstoned stream blocks then times out", empty_reply is None and elapsed >= 0.08,
     "elapsed=%.3f" % elapsed)

# A queued RESET stays behind the blocking parse barrier, then executes in wire order after wake.
reset = Resp()
reset.sock.sendall(frame("XREAD", "BLOCK", "2000", "STREAMS", "stream:reset", "$") +
                   frame("RESET"))
wait_value(admin, "CLIENTS", "blocked_clients", 1)
admin.cmd("XADD", "stream:reset", "1-0", "f", "wake")
reset_first = reset.read(); reset_second = reset.read()
note("RESET pipelined while parked retires after XREAD", reset_first is not None and
     reset_second == b"RESET")
reset.close()

# Disconnect cancellation is pid/socket scoped and must drain both gauges.
disconnect = socket.create_connection((HOST, PORT), timeout=10)
disconnect.sendall(frame("XREAD", "BLOCK", "0", "STREAMS", "stream:disconnect", "$"))
wait_value(admin, "CLIENTS", "blocked_clients", 1)
disconnect.close()
note("parked disconnect drains stream waiter gauges",
     wait_value(admin, "CLIENTS", "blocked_clients", 0) and
     wait_value(admin, "STATS", "blocking_waiters", 0))

# Cross several macro-node boundaries, then leave the head in a partially trimmed node. Forward,
# reverse, and XDEL all have to initialize their cursor from the sorted index plus head dictionary.
admin.cmd("DEL", "stream:indexed")
for base in range(1, 351, 250):
    admin.pipeline([("XADD", "stream:indexed", "%d-0" % ident, "f", ident)
                    for ident in range(base, min(351, base + 250))])
removed_indexed = admin.cmd("XTRIM", "stream:indexed", "MINID", "=", "251-0")
indexed_forward = admin.cmd("XRANGE", "stream:indexed", "251-0", "+", "COUNT", "4")
indexed_reverse = admin.cmd("XREVRANGE", "stream:indexed", "254-0", "-", "COUNT", "4")
deleted_indexed = admin.cmd("XDEL", "stream:indexed", "251-0")
note("macro-node seek and partial gap-head trim",
     removed_indexed == 250 and
     [entry[0] for entry in indexed_forward] == [b"251-0", b"252-0", b"253-0", b"254-0"] and
     [entry[0] for entry in indexed_reverse] == [b"254-0", b"253-0", b"252-0", b"251-0"] and
     deleted_indexed == 1 and admin.cmd("XLEN", "stream:indexed") == 99)

# The default count is intentionally gate-sized; it still crosses hundreds of node rotations. The
# audit's million-op soak can raise STREAM_PLATEAU_OPS without changing the property or harness.
admin.cmd("FLUSHDB")
plateau_ops = int(os.environ.get("STREAM_PLATEAU_OPS", "20000"))
plateau_ops = max(200, plateau_ops)
plateau_mid = plateau_ops // 2
baseline = info_value(admin, "MEMORY", "used_memory_dataset")
for base in range(1, plateau_mid + 1, 250):
    admin.pipeline([("XADD", "stream:plateau", "MAXLEN", "=", "100",
                     "%d-0" % ident, "f", "v")
                    for ident in range(base, min(plateau_mid + 1, base + 250))])
plateau_first = info_value(admin, "MEMORY", "used_memory_dataset")
for base in range(plateau_mid + 1, plateau_ops + 1, 250):
    admin.pipeline([("XADD", "stream:plateau", "MAXLEN", "=", "100",
                     "%d-0" % ident, "f", "v")
                    for ident in range(base, min(plateau_ops + 1, base + 250))])
plateau_second = info_value(admin, "MEMORY", "used_memory_dataset")
plateau_length = admin.cmd("XLEN", "stream:plateau")
admin.cmd("DEL", "stream:plateau")
plateau_after_delete = info_value(admin, "MEMORY", "used_memory_dataset")
note("sustained exact trim reaches a memory plateau",
     plateau_length == 100 and plateau_second <= plateau_first + 8192 and
     plateau_after_delete <= baseline + 4096,
     "first=%d second=%d after-del=%d baseline=%d" %
     (plateau_first, plateau_second, plateau_after_delete, baseline))

# One-allocation floor and one-way embed migration. INFO measures the full resident key/table cost,
# so the assertion is deliberately looser than the audit's ~140-byte KvObj estimate while still
# rejecting Redis's 4.4 KiB first-entry floor.
admin.cmd("FLUSHDB")
baseline = info_value(admin, "MEMORY", "used_memory_dataset")
sample = 1024
for index in range(sample):
    admin.cmd("XADD", "stream:floor:%d" % index, "1-0", "f", "v")
resident = info_value(admin, "MEMORY", "used_memory_dataset")
per_stream = max(0, resident - baseline) / sample
note("one-entry memory floor tracks ~140B design (well below 4.4KiB)", per_stream < 512,
     "measured=%.1fB/key audit-kvobj~=140B" % per_stream)
# The one-big-flip migration assert is RETIRED with the implementation it described: under the
# consumer-groups lane's budget-driven macro nodes (stream-node-max-entries/-bytes, redis
# defaults), the embedded form grows into node-at-a-time structure with no 4.4KiB cliff, and
# OBJECT ENCODING reports the redis name for both forms. What the current design promises —
# and what this protects — is the compact floor (asserted above) plus amortized-LINEAR growth:
# per-entry cost through the embedded->macro boundary must stay bounded, or a structure
# regression re-introduced the old blowup.
admin.cmd("DEL", "stream:migrate")
admin.cmd("CONFIG", "SET", "stream-node-max-entries", "8")
admin.cmd("XADD", "stream:migrate", "1-0", "f", "v")
note("stream reports the redis encoding name",
     admin.cmd("OBJECT", "ENCODING", "stream:migrate") == b"stream")
for ident in range(2, 129):
    admin.cmd("XADD", "stream:migrate", "%d-0" % ident, "f", "v")
final_usage = admin.cmd("MEMORY", "USAGE", "stream:migrate")
admin.cmd("CONFIG", "SET", "stream-node-max-entries", "100")
note("growth across the embedded->macro boundary stays amortized-linear",
     isinstance(final_usage, int) and final_usage < 128 * 200,
     "usage=%r for 128 entries (bound 200B/entry)" % final_usage)

note("stream waiter gauges end at zero",
     info_value(admin, "CLIENTS", "blocked_clients") == 0 and
     info_value(admin, "STATS", "blocking_waiters") == 0)
admin.close()
print("STREAM " + ("PASS" if FAIL == 0 else "FAIL %d" % FAIL), flush=True)
sys.exit(1 if FAIL else 0)

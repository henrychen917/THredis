#!/usr/bin/env python3
"""Directed MULTI/EXEC/WATCH gate. Usage: multi_exec.py HOST PORT."""

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


class RespError:
    def __init__(self, message):
        self.message = message

    def __repr__(self):
        return "RespError(%r)" % self.message


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
            return RespError(line[1:-2].decode(errors="replace"))
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


def set_atomic(value):
    client = Resp()
    try:
        return client.cmd("CONFIG", "SET", "atomic", str(value)) == b"OK"
    finally:
        client.close()


def queue_and_ryow():
    c = Resp()
    observer = Resp()
    key = "multi:ryow"
    try:
        c.cmd("SET", key, "before")
        ok = c.cmd("MULTI") == b"OK"
        ok &= c.cmd("SET", key, "inside") == b"QUEUED"
        ok &= c.cmd("GET", key) == b"QUEUED"
        ok &= observer.cmd("GET", key) == b"before"
        result = c.cmd("EXEC")
        ok &= result == [b"OK", b"inside"]
        ok &= c.cmd("GET", key) == b"inside"
        note("queue invisibility plus inside/after EXEC RYOW", ok, "reply=%r" % (result,))
    finally:
        c.close()
        observer.close()


def queue_errors():
    c = Resp()
    key = "multi:queue-error"
    try:
        c.cmd("DEL", key)
        c.sock.sendall(frame("NO_SUCH_PRE_MULTI_COMMAND") + frame("MULTI") +
                       frame("SET", key, "after-pre-multi-error"))
        before = c.read()
        started = c.read()
        queued = c.read()
        executed = c.cmd("EXEC")
        ok = (isinstance(before, RespError) and started == b"OK" and queued == b"QUEUED" and
              executed == [b"OK"] and c.cmd("GET", key) == b"after-pre-multi-error")
        note("pre-MULTI pipeline error does not dirty the new transaction", ok,
             "before=%r exec=%r" % (before, executed))

        c.cmd("DEL", key)
        c.cmd("MULTI")
        queued = c.cmd("SET", key, "must-not-land")
        unknown = c.cmd("NO_SUCH_MULTI_COMMAND")
        aborted = c.cmd("EXEC")
        absent = c.cmd("GET", key)
        ok = (queued == b"QUEUED" and isinstance(unknown, RespError) and
              isinstance(aborted, RespError) and aborted.message.startswith("EXECABORT") and
              absent is None)
        note("unknown-command queue error aborts EXEC", ok,
             "unknown=%r exec=%r value=%r" % (unknown, aborted, absent))

        c.cmd("MULTI")
        arity = c.cmd("SET", key)
        aborted = c.cmd("EXEC")
        note("wrong-arity queue error aborts EXEC",
             isinstance(arity, RespError) and isinstance(aborted, RespError) and
             aborted.message.startswith("EXECABORT"),
             "arity=%r exec=%r" % (arity, aborted))

        c.cmd("MULTI")
        queued = c.cmd("SET", key, "value", "bad-option")
        executed = c.cmd("EXEC")
        note("runtime syntax error remains an EXEC array element",
             queued == b"QUEUED" and len(executed) == 1 and isinstance(executed[0], RespError),
             "reply=%r" % (executed,))
    finally:
        c.close()


def controls_and_reset():
    c = Resp()
    other = Resp()
    key = "multi:controls"
    try:
        c.cmd("DEL", key)
        ok = c.cmd("MULTI") == b"OK"
        nested = c.cmd("MULTI")
        ok &= isinstance(nested, RespError)
        ok &= c.cmd("SET", key, "discarded") == b"QUEUED"
        ok &= c.cmd("DISCARD") == b"OK"
        ok &= c.cmd("GET", key) is None
        ok &= isinstance(c.cmd("EXEC"), RespError)

        c.cmd("SET", key, "base")
        ok &= c.cmd("WATCH", key) == b"OK"
        ok &= c.cmd("UNWATCH") == b"OK"
        other.cmd("SET", key, "changed-after-unwatch")
        ok &= c.cmd("MULTI") == b"OK"
        ok &= c.cmd("GET", key) == b"QUEUED"
        ok &= c.cmd("EXEC") == [b"changed-after-unwatch"]

        ok &= c.cmd("WATCH", key) == b"OK"
        ok &= c.cmd("MULTI") == b"OK"
        ok &= c.cmd("UNWATCH") == b"QUEUED"
        ok &= c.cmd("GET", key) == b"QUEUED"
        queued_unwatch = c.cmd("EXEC")
        ok &= queued_unwatch == [b"OK", b"changed-after-unwatch"]

        ok &= c.cmd("WATCH", key) == b"OK"
        other.cmd("SET", key, "dirty")
        ok &= c.cmd("MULTI") == b"OK"
        ok &= c.cmd("SET", key, "must-not-land") == b"QUEUED"
        ok &= c.cmd("EXEC") is None
        ok &= c.cmd("MULTI") == b"OK"
        ok &= c.cmd("GET", key) == b"QUEUED"
        ok &= c.cmd("EXEC") == [b"dirty"]
        client_id = c.cmd("CLIENT", "ID")
        ok &= c.cmd("MULTI") == b"OK"
        ok &= c.cmd("CLIENT", "ID") == b"QUEUED"
        ok &= c.cmd("SELECT", "0") == b"QUEUED"
        ok &= c.cmd("SCAN", "0", "COUNT", "1") == b"QUEUED"
        ok &= c.cmd("CONFIG", "GET", "atomic") == b"QUEUED"
        io_result = c.cmd("EXEC")
        ok &= (len(io_result) == 4 and io_result[:2] == [client_id, b"OK"] and
               isinstance(io_result[2], list) and len(io_result[2]) == 2 and
               io_result[3] == [b"atomic", b"0"])
        note("DISCARD/UNWATCH/nested-MULTI/reset-on-EXEC semantics", ok,
             "queued_unwatch=%r" % (queued_unwatch,))
    finally:
        c.close()
        other.close()


def script_watch_declared_keys_only():
    watcher = Resp()
    writer = Resp()
    candidates = ["multi:script-watch:%d" % i for i in range(256)]
    by_owner = {}
    try:
        for key in candidates:
            owner = writer.cmd("DEBUG", "SHARD", key)
            if not isinstance(owner, int):
                note("script WATCH declared-key detector", False,
                     "DEBUG SHARD unavailable: %r" % (owner,))
                return
            by_owner.setdefault(owner, []).append(key)
        pair = next((keys[:2] for keys in by_owner.values() if len(keys) >= 2), None)
        if not pair:
            note("script WATCH declared-key detector", False, "no same-owner key pair")
            return
        declared, argument = pair
        writer.cmd("DEL", declared, argument)

        ok = watcher.cmd("WATCH", argument) == b"OK"
        wrote = writer.cmd(
            "EVAL", "return redis.call('SET',KEYS[1],ARGV[1])",
            "1", declared, "value", argument)
        ok &= wrote == b"OK"
        ok &= watcher.cmd("MULTI") == b"OK"
        ok &= watcher.cmd("GET", argument) == b"QUEUED"
        quiet = watcher.cmd("EXEC")
        ok &= quiet == [None]

        # Positive control: the same detector must fire for the key the script really writes.
        ok &= watcher.cmd("WATCH", declared) == b"OK"
        wrote_again = writer.cmd(
            "EVAL", "return redis.call('SET',KEYS[1],ARGV[1])",
            "1", declared, "changed", argument)
        ok &= wrote_again == b"OK"
        ok &= watcher.cmd("MULTI") == b"OK"
        ok &= watcher.cmd("GET", declared) == b"QUEUED"
        fired = watcher.cmd("EXEC")
        ok &= fired is None
        note("script WATCH declared-key detector", ok,
             "owner=%s quiet=%r fired=%r" %
             (writer.cmd("DEBUG", "SHARD", declared), quiet, fired))
    finally:
        watcher.close()
        writer.close()


def heterogeneous_ryow():
    c = Resp()
    keys = ["multi:hetero:%d" % i for i in range(12)]
    values = ["v%d" % i for i in range(len(keys))]
    mset = []
    for key, value in zip(keys, values):
        mset.extend((key, value))
    try:
        c.cmd("DEL", *keys)
        ok = c.cmd("MULTI") == b"OK"
        ok &= c.cmd("MSET", *mset) == b"QUEUED"
        ok &= c.cmd("MGET", *keys) == b"QUEUED"
        result = c.cmd("EXEC")
        ok &= result == [b"OK", [value.encode() for value in values]]
        ok &= c.cmd("MULTI") == b"OK"
        ok &= c.cmd("KEYS", "multi:hetero:*") == b"QUEUED"
        keys_result = c.cmd("EXEC")
        ok &= (len(keys_result) == 1 and
               set(keys_result[0]) == {key.encode() for key in keys})

        source, destination = "multi:rename:source", "multi:rename:destination"
        c.cmd("SET", source, "payload")
        c.cmd("DEL", destination)
        ok &= c.cmd("MULTI") == b"OK"
        ok &= c.cmd("RENAME", source, destination) == b"QUEUED"
        ok &= c.cmd("GET", destination) == b"QUEUED"
        renamed = c.cmd("EXEC")
        ok &= renamed == [b"OK", b"payload"]

        list_keys = ["multi:lmpop:%d" % i for i in range(8)]
        c.cmd("DEL", *list_keys)
        c.cmd("RPUSH", list_keys[-1], "tail")
        ok &= c.cmd("MULTI") == b"OK"
        ok &= c.cmd("LMPOP", str(len(list_keys)), *list_keys,
                    "LEFT", "COUNT", "1") == b"QUEUED"
        ok &= c.cmd("LLEN", list_keys[-1]) == b"QUEUED"
        popped = c.cmd("EXEC")
        ok &= popped == [[list_keys[-1].encode(), [b"tail"]], 0]
        note("heterogeneous MSET/MGET and lowered RENAME RYOW", ok,
             "mset=%r rename=%r lmpop=%r" % (result, renamed, popped))
    finally:
        c.close()


def info_field(field):
    client = Resp()
    try:
        text = client.cmd("INFO", "stats")
        if not isinstance(text, (bytes, bytearray)):
            return None
        for line in text.decode(errors="replace").split("\r\n"):
            if line.startswith(field + ":"):
                return int(line.split(":", 1)[1])
        return None
    finally:
        client.close()


def torn_arm(seconds=2.0):
    # NOT VACUOUS. This arm is a stress arm: on a quiet box its readers finish their fan-out in
    # microseconds and never straddle anything, which is exactly how a partial EXEC survived here
    # for so long (0/18 quiet, 15/20 with eight spinners). torn == 0 therefore proves nothing on
    # its own, so the arm also demands atomic_fanout_cuts advance: every cross-shard read really
    # did pin a cut while the atomic-activity word read zero -- the guarded path. The deterministic
    # version of this window lives in tests/execatomic.py (DEBUG ATOMIC-FANOUT-DEFER).
    keys = ["multi:torn:%d" % i for i in range(8)]
    before_cuts = info_field("atomic_fanout_cuts")
    init = Resp()
    for key in keys:
        init.cmd("SET", key, "0")
    init.close()

    stop = threading.Event()
    start = threading.Barrier(5)
    lock = threading.Lock()
    errors = []
    torn = 0
    reads = 0
    commits = 0

    def writer():
        nonlocal commits
        c = Resp()
        seq = 1
        try:
            start.wait()
            while not stop.is_set():
                if c.cmd("MULTI") != b"OK":
                    raise AssertionError("MULTI failed")
                for key in keys:
                    if c.cmd("SET", key, str(seq)) != b"QUEUED":
                        raise AssertionError("SET was not queued")
                result = c.cmd("EXEC")
                if result != [b"OK"] * len(keys):
                    raise AssertionError("bad EXEC %r" % (result,))
                commits += 1
                seq += 1
        except Exception as exc:  # pragma: no cover - diagnostic arm
            with lock:
                errors.append("writer:%s" % exc)
        finally:
            c.close()

    def reader(rid):
        nonlocal torn, reads
        c = Resp()
        try:
            start.wait()
            while not stop.is_set():
                values = c.cmd("MGET", *keys)
                with lock:
                    reads += 1
                    if not values or any(value != values[0] for value in values[1:]):
                        torn += 1
        except Exception as exc:  # pragma: no cover - diagnostic arm
            with lock:
                errors.append("reader%d:%s" % (rid, exc))
        finally:
            c.close()

    threads = [threading.Thread(target=writer)] + [
        threading.Thread(target=reader, args=(i,)) for i in range(4)
    ]
    for thread in threads:
        thread.start()
    time.sleep(seconds)
    stop.set()
    for thread in threads:
        thread.join(35)
    alive = [thread.name for thread in threads if thread.is_alive()]
    cuts = (info_field("atomic_fanout_cuts") or 0) - (before_cuts or 0)
    note("EXEC one-ticket torn-read arm",
         not errors and not alive and torn == 0 and reads > 100 and commits > 10 and cuts > 0,
         "reads=%d commits=%d torn=%d fanout_cuts=+%d errors=%r alive=%r" %
         (reads, commits, torn, cuts, errors[:2], alive))


def watch_race(rounds=100):
    admin = Resp()
    errors = []
    wins = 0
    try:
        for round_id in range(rounds):
            key = "multi:watch-cas"
            admin.cmd("SET", key, str(round_id))
            barrier = threading.Barrier(2)
            replies = []
            reply_lock = threading.Lock()

            def contender(value):
                c = Resp()
                try:
                    if c.cmd("WATCH", key) != b"OK":
                        raise AssertionError("WATCH failed")
                    if c.cmd("GET", key) != str(round_id).encode():
                        raise AssertionError("watched read changed early")
                    c.cmd("MULTI")
                    c.cmd("SET", key, value)
                    barrier.wait()
                    result = c.cmd("EXEC")
                    with reply_lock:
                        replies.append(result)
                except Exception as exc:  # pragma: no cover - diagnostic arm
                    with reply_lock:
                        errors.append(str(exc))
                finally:
                    c.close()

            pair = [threading.Thread(target=contender, args=("left",)),
                    threading.Thread(target=contender, args=("right",))]
            for thread in pair:
                thread.start()
            for thread in pair:
                thread.join(35)
            round_wins = sum(reply == [b"OK"] for reply in replies)
            round_aborts = sum(reply is None for reply in replies)
            if round_wins != 1 or round_aborts != 1:
                errors.append("round %d replies=%r" % (round_id, replies))
                break
            wins += round_wins
    finally:
        admin.close()
    note("WATCH CAS race has exactly one EXEC winner per round",
         not errors and wins == rounds, "wins=%d/%d errors=%r" % (wins, rounds, errors[:2]))


note("force atomic contract with --atomic/CONFIG off", set_atomic(0))
queue_and_ryow()
queue_errors()
controls_and_reset()
script_watch_declared_keys_only()
heterogeneous_ryow()
torn_arm()
watch_race()

if FAIL:
    raise SystemExit("%d MULTI/WATCH checks failed" % FAIL)
print("MULTI/WATCH directed battery passed", flush=True)

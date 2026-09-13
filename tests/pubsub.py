#!/usr/bin/env python3
"""Directed RESP2 pub/sub test. Usage: tests/pubsub.py HOST PORT"""

import os
import select
import socket
import struct
import sys
import threading
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
            assert self.file.read(2) == b"\r\n"
            return payload
        if prefix in (b"*", b"~", b">"):
            size = int(value)
            if size == -1:
                return None
            return [self.read() for _ in range(size)]
        if prefix == b"%":                      # RESP3 map, flattened to a key/value list
            return [self.read() for _ in range(int(value) * 2)]
        if prefix == b"_":
            return None
        if prefix in (b",", b"#", b"("):
            return value
        raise AssertionError(f"unknown RESP prefix: {prefix!r}")

    def close(self, reset=False):
        if self.sock is None:
            return
        if reset:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        self.file.close()
        self.sock.close()
        self.sock = None


def expect(actual, wanted, label):
    if actual != wanted:
        raise AssertionError(f"{label}: got {actual!r}, wanted {wanted!r}")


def expect_no_frame(conn, label, timeout=0.15):
    if select.select([conn.sock], [], [], timeout)[0]:
        raise AssertionError(f"{label}: unexpected frame {conn.read()!r}")


def info_stats(conn):
    raw = conn.command("INFO", "STATS")
    if not isinstance(raw, bytes):
        raise AssertionError(f"INFO STATS returned {raw!r}")
    result = {}
    for line in raw.decode().splitlines():
        if ":" in line:
            key, value = line.split(":", 1)
            result[key] = value
    return result


def main():
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} HOST PORT")
    host, port = sys.argv[1], int(sys.argv[2])
    token = f"tomo-pubsub-{os.getpid()}-{time.time_ns()}"
    channel = f"{token}:fanout"
    admin = Conn(host, port)
    publisher = Conn(host, port)
    subscribers = []

    # N-way fanout and per-subscriber ordering.
    nsubs, nmessages = 24, 40
    for index in range(nsubs):
        conn = Conn(host, port)
        expect(conn.command("SUBSCRIBE", channel),
               [b"subscribe", channel.encode(), 1], f"subscribe {index}")
        subscribers.append(conn)
    expect(admin.command("PUBSUB", "NUMSUB", channel, f"{token}:missing"),
           [channel.encode(), nsubs, f"{token}:missing".encode(), 0], "PUBSUB NUMSUB")
    channels = admin.command("PUBSUB", "CHANNELS", f"{token}:*")
    if channel.encode() not in channels:
        raise AssertionError(f"PUBSUB CHANNELS omitted {channel!r}: {channels!r}")
    expect(admin.command("PUBSUB", "CHANNELS", ""), [], "PUBSUB CHANNELS empty pattern")

    for sequence in range(nmessages):
        expect(publisher.command("PUBLISH", channel, str(sequence)), nsubs,
               f"PUBLISH {sequence}")
    for index, conn in enumerate(subscribers):
        for sequence in range(nmessages):
            expect(conn.read(), [b"message", channel.encode(), str(sequence).encode()],
                   f"subscriber {index} message {sequence}")

    # Shard subscription acknowledgements, duplicates, explicit/all unsubscribe, and CLIENT ssub.
    shard_controls = Conn(host, port)
    shard_client_id = shard_controls.command("CLIENT", "ID")
    shard_a, shard_b, shard_c = (
        f"{token}:shard:control:a", f"{token}:shard:control:b", f"{token}:shard:control:c")
    expect(shard_controls.command("SSUBSCRIBE", shard_a),
           [b"ssubscribe", shard_a.encode(), 1], "shard subscribe one")
    expect(shard_controls.command("SSUBSCRIBE", shard_b),
           [b"ssubscribe", shard_b.encode(), 2], "shard subscribe two")
    expect(shard_controls.command("SSUBSCRIBE", shard_a),
           [b"ssubscribe", shard_a.encode(), 2], "duplicate shard subscribe")
    expect(publisher.command("SPUBLISH", shard_a, "hello"), 1, "shard publish receiver count")
    expect(shard_controls.read(), [b"smessage", shard_a.encode(), b"hello"],
           "shard smessage frame")
    client_list = admin.command("CLIENT", "LIST").decode().splitlines()
    client_line = next((line for line in client_list if f"id={shard_client_id} " in line), None)
    if client_line is None or "ssub=2" not in client_line:
        raise AssertionError(f"CLIENT LIST shard count: {client_line!r}")
    missing_shard = f"{token}:shard:missing"
    expect(shard_controls.command("SUNSUBSCRIBE", missing_shard),
           [b"sunsubscribe", missing_shard.encode(), 2], "shard unsubscribe missing")
    expect(shard_controls.command("SUNSUBSCRIBE", shard_a),
           [b"sunsubscribe", shard_a.encode(), 1], "shard unsubscribe named")
    expect(shard_controls.command("SSUBSCRIBE", shard_a, shard_c),
           [b"ssubscribe", shard_a.encode(), 2], "shard resubscribe first")
    expect(shard_controls.read(), [b"ssubscribe", shard_c.encode(), 3],
           "shard resubscribe second")
    unsubscribe_all = [shard_controls.command("SUNSUBSCRIBE"),
                       shard_controls.read(), shard_controls.read()]
    if ({item[1] for item in unsubscribe_all} !=
            {shard_a.encode(), shard_b.encode(), shard_c.encode()} or
            {item[2] for item in unsubscribe_all} != {0, 1, 2} or
            any(item[0] != b"sunsubscribe" for item in unsubscribe_all)):
        raise AssertionError(f"SUNSUBSCRIBE all: {unsubscribe_all!r}")
    expect(shard_controls.command("SUNSUBSCRIBE"), [b"sunsubscribe", None, 0],
           "SUNSUBSCRIBE empty")
    shard_controls.close()

    # Regular exact, shard exact, and regular pattern namespaces never cross in either direction.
    namespace_channel = f"{token}:namespace:exact"
    regular_only = Conn(host, port)
    expect(regular_only.command("SUBSCRIBE", namespace_channel),
           [b"subscribe", namespace_channel.encode(), 1], "regular namespace subscribe")
    expect(publisher.command("SPUBLISH", namespace_channel, "shard"), 0,
           "SPUBLISH ignores regular exact")
    expect_no_frame(regular_only, "regular subscriber got shard publish")
    regular_only.close()

    shard_only = Conn(host, port)
    expect(shard_only.command("SSUBSCRIBE", namespace_channel),
           [b"ssubscribe", namespace_channel.encode(), 1], "shard namespace subscribe")
    expect(publisher.command("PUBLISH", namespace_channel, "regular"), 0,
           "PUBLISH ignores shard exact")
    expect_no_frame(shard_only, "shard subscriber got regular publish")
    shard_only.close()

    pattern_only = Conn(host, port)
    namespace_pattern = f"{token}:namespace:*"
    expect(pattern_only.command("PSUBSCRIBE", namespace_pattern),
           [b"psubscribe", namespace_pattern.encode(), 1], "namespace pattern subscribe")
    expect(publisher.command("SPUBLISH", namespace_channel, "pattern"), 0,
           "SPUBLISH ignores patterns")
    expect_no_frame(pattern_only, "pattern subscriber got shard publish")
    pattern_only.close()

    # Reply counts are namespace-scoped, while subscriber-mode exit uses their three-way total.
    mixed = Conn(host, port)
    mixed_a, mixed_b = f"{token}:mixed:a", f"{token}:mixed:b"
    mixed_pattern = f"{token}:mixed:p:*"
    mixed_s, mixed_t = f"{token}:mixed:s", f"{token}:mixed:t"
    expect(mixed.command("SUBSCRIBE", mixed_a), [b"subscribe", mixed_a.encode(), 1],
           "mixed regular one")
    expect(mixed.command("PSUBSCRIBE", mixed_pattern),
           [b"psubscribe", mixed_pattern.encode(), 2], "mixed pattern two")
    expect(mixed.command("SSUBSCRIBE", mixed_s), [b"ssubscribe", mixed_s.encode(), 1],
           "mixed shard one not three")
    expect(mixed.command("SUBSCRIBE", mixed_b), [b"subscribe", mixed_b.encode(), 3],
           "mixed regular count three")
    expect(mixed.command("SSUBSCRIBE", mixed_t), [b"ssubscribe", mixed_t.encode(), 2],
           "mixed shard count two")
    expect(mixed.command("PING"), [b"pong", b""], "shard subscriber PING")
    restricted_spublish = mixed.command("SPUBLISH", mixed_s, "blocked")
    if not isinstance(restricted_spublish, RespError) or "only (P|S)SUBSCRIBE" not in str(restricted_spublish):
        raise AssertionError(f"subscriber SPUBLISH restriction: {restricted_spublish!r}")
    expect(mixed.command("UNSUBSCRIBE", mixed_a, mixed_b),
           [b"unsubscribe", mixed_a.encode(), 2], "mixed unsubscribe regular first")
    expect(mixed.read(), [b"unsubscribe", mixed_b.encode(), 1],
           "mixed unsubscribe regular second")
    expect(mixed.command("PUNSUBSCRIBE", mixed_pattern),
           [b"punsubscribe", mixed_pattern.encode(), 0], "mixed unsubscribe pattern")
    still_restricted = mixed.command("SET", f"{token}:mixed:key", "value")
    if not isinstance(still_restricted, RespError):
        raise AssertionError(f"shard subscriptions did not retain subscriber mode: {still_restricted!r}")
    expect(mixed.command("SUNSUBSCRIBE", mixed_s, mixed_t),
           [b"sunsubscribe", mixed_s.encode(), 1], "mixed shard unsubscribe first")
    expect(mixed.read(), [b"sunsubscribe", mixed_t.encode(), 0],
           "mixed shard unsubscribe second")
    expect(mixed.command("SET", f"{token}:mixed:key", "value"), b"OK",
           "total subscription count exits mode")
    mixed.close()

    # RESET silently removes shard subscriptions and clears both introspection surfaces.
    reset_conn = Conn(host, port)
    reset_client_id = reset_conn.command("CLIENT", "ID")
    reset_channel = f"{token}:shard:reset"
    expect(reset_conn.command("SSUBSCRIBE", reset_channel),
           [b"ssubscribe", reset_channel.encode(), 1], "shard RESET subscribe")
    expect(reset_conn.command("RESET"), b"RESET", "shard RESET")
    expect_no_frame(reset_conn, "RESET emitted sunsubscribe")
    expect(admin.command("PUBSUB", "SHARDNUMSUB", reset_channel),
           [reset_channel.encode(), 0], "RESET shard cleanup")
    client_info = reset_conn.command("CLIENT", "INFO").decode()
    if f"id={reset_client_id} " not in client_info or "ssub=0" not in client_info:
        raise AssertionError(f"CLIENT INFO shard count: {client_info!r}")
    client_list = admin.command("CLIENT", "LIST").decode().splitlines()
    client_line = next((line for line in client_list if f"id={reset_client_id} " in line), None)
    if client_line is None or "ssub=0" not in client_line:
        raise AssertionError(f"CLIENT LIST reset shard count: {client_line!r}")
    reset_conn.close()

    # Shard introspection is separate, glob-filtered, and has no SHARDNUMPAT arm.
    introspect_regular = Conn(host, port)
    introspect_shard = Conn(host, port)
    regular_intro = f"{token}:introspect:regular"
    shard_intro_a = f"{token}:introspect:shard:a"
    shard_intro_b = f"{token}:introspect:shard:b"
    expect(introspect_regular.command("SUBSCRIBE", regular_intro),
           [b"subscribe", regular_intro.encode(), 1], "introspection regular subscribe")
    expect(introspect_shard.command("SSUBSCRIBE", shard_intro_a, shard_intro_b),
           [b"ssubscribe", shard_intro_a.encode(), 1], "introspection shard first")
    expect(introspect_shard.read(), [b"ssubscribe", shard_intro_b.encode(), 2],
           "introspection shard second")
    all_shard_channels = admin.command("PUBSUB", "SHARDCHANNELS")
    if not {shard_intro_a.encode(), shard_intro_b.encode()}.issubset(all_shard_channels):
        raise AssertionError(f"PUBSUB SHARDCHANNELS omitted active channels: {all_shard_channels!r}")
    shard_channels = admin.command("PUBSUB", "SHARDCHANNELS", f"{token}:introspect:*")
    if set(shard_channels) != {shard_intro_a.encode(), shard_intro_b.encode()}:
        raise AssertionError(f"PUBSUB SHARDCHANNELS namespace: {shard_channels!r}")
    expect(admin.command("PUBSUB", "SHARDCHANNELS", ""), [],
           "PUBSUB SHARDCHANNELS empty pattern")
    regular_channels = admin.command("PUBSUB", "CHANNELS", f"{token}:introspect:*")
    expect(regular_channels, [regular_intro.encode()], "PUBSUB CHANNELS excludes shard")
    expect(admin.command("PUBSUB", "SHARDNUMSUB", shard_intro_a, shard_intro_b, missing_shard),
           [shard_intro_a.encode(), 1, shard_intro_b.encode(), 1, missing_shard.encode(), 0],
           "PUBSUB SHARDNUMSUB")
    expect(admin.command("PUBSUB", "SHARDNUMSUB"), [], "PUBSUB SHARDNUMSUB empty")
    # Redis has no SHARDNUMPAT, and its rejection echoes the subcommand token verbatim.
    shardnumpat = admin.command("PUBSUB", "SHARDNUMPAT")
    expect(str(shardnumpat), "ERR unknown subcommand 'SHARDNUMPAT'. Try PUBSUB HELP.",
           "PUBSUB SHARDNUMPAT rejection")
    expect(str(admin.command("PUBSUB", "NUMPAT", "extra")),
           "ERR wrong number of arguments for 'pubsub|numpat' command", "PUBSUB NUMPAT arity")
    expect(str(admin.command("PUBSUB", "CHANNELS", "a", "b")),
           "ERR unknown subcommand or wrong number of arguments for 'CHANNELS'. Try PUBSUB HELP.",
           "PUBSUB CHANNELS arity")
    help_reply = admin.command("PUBSUB", "HELP")
    if b"SHARDCHANNELS [<pattern>]" not in help_reply or b"SHARDNUMSUB [<shardchannel> ...]" not in help_reply:
        raise AssertionError(f"PUBSUB HELP omitted shard subcommands: {help_reply!r}")
    introspect_regular.close()
    introspect_shard.close()

    # A single command may span channel homes: no slot/CROSSSLOT semantics are imposed.
    multi_home = Conn(host, port)
    multi_channels = [f"{token}:multihome:{index}" for index in range(3)]
    expect(multi_home.command("SSUBSCRIBE", *multi_channels),
           [b"ssubscribe", multi_channels[0].encode(), 1], "multi-home ack one")
    expect(multi_home.read(), [b"ssubscribe", multi_channels[1].encode(), 2],
           "multi-home ack two")
    expect(multi_home.read(), [b"ssubscribe", multi_channels[2].encode(), 3],
           "multi-home ack three")
    for index, item in enumerate(multi_channels):
        expect(publisher.command("SPUBLISH", item, str(index)), 1,
               f"multi-home publish {index}")
        expect(multi_home.read(), [b"smessage", item.encode(), str(index).encode()],
               f"multi-home delivery {index}")
    multi_home.close()

    # N-way shard fanout and per-subscriber ordering mirrors the regular fanout arm.
    shard_fanout_channel = f"{token}:shard:fanout"
    shard_subscribers = []
    for index in range(nsubs):
        conn = Conn(host, port)
        expect(conn.command("SSUBSCRIBE", shard_fanout_channel),
               [b"ssubscribe", shard_fanout_channel.encode(), 1], f"shard subscribe {index}")
        shard_subscribers.append(conn)
    for sequence in range(nmessages):
        expect(publisher.command("SPUBLISH", shard_fanout_channel, str(sequence)), nsubs,
               f"SPUBLISH {sequence}")
    for index, conn in enumerate(shard_subscribers):
        for sequence in range(nmessages):
            expect(conn.read(), [b"smessage", shard_fanout_channel.encode(), str(sequence).encode()],
                   f"shard subscriber {index} message {sequence}")
        conn.close()

    # Subscription controls cannot be EXEC children in the owner-only transaction machinery.
    multi = Conn(host, port)
    expect(multi.command("MULTI"), b"OK", "MULTI before SSUBSCRIBE")
    forbidden_multi = multi.command("SSUBSCRIBE", f"{token}:multi")
    if not isinstance(forbidden_multi, RespError) or "not allowed inside a transaction" not in str(forbidden_multi):
        raise AssertionError(f"SSUBSCRIBE inside MULTI: {forbidden_multi!r}")
    expect(multi.command("DISCARD"), b"OK", "DISCARD after forbidden SSUBSCRIBE")
    multi.close()

    # Exact and both pattern arms, including subscriber-mode framing/restrictions.
    arms = Conn(host, port)
    arm_channel = f"{token}:news:42"
    expect(arms.command("SUBSCRIBE", arm_channel), [b"subscribe", arm_channel.encode(), 1],
           "exact arm")
    expect(arms.command("PSUBSCRIBE", f"{token}:news:*", f"{token}:*:42"),
           [b"psubscribe", f"{token}:news:*".encode(), 2], "pattern arm one")
    expect(arms.read(), [b"psubscribe", f"{token}:*:42".encode(), 3], "pattern arm two")
    expect(admin.command("PUBSUB", "NUMPAT"), 2, "PUBSUB NUMPAT")
    expect(publisher.command("PUBLISH", arm_channel, "payload"), 3, "three delivery arms")
    deliveries = [arms.read(), arms.read(), arms.read()]
    wanted = {
        (b"message", arm_channel.encode(), b"payload"),
        (b"pmessage", f"{token}:news:*".encode(), arm_channel.encode(), b"payload"),
        (b"pmessage", f"{token}:*:42".encode(), arm_channel.encode(), b"payload"),
    }
    if {tuple(item) for item in deliveries} != wanted:
        raise AssertionError(f"pattern deliveries: {deliveries!r}")
    expect(arms.command("PING", "hello"), [b"pong", b"hello"], "subscriber PING")
    restricted = arms.command("SET", f"{token}:key", "value")
    if not isinstance(restricted, RespError) or "only (P|S)SUBSCRIBE" not in str(restricted):
        raise AssertionError(f"subscriber restriction: {restricted!r}")

    # RESET unregisters every exact/pattern arm and leaves subscriber mode.
    expect(arms.command("RESET"), b"RESET", "subscriber RESET")
    expect(arms.command("SET", f"{token}:after-reset", "ok"), b"OK", "command after RESET")
    expect(admin.command("PUBSUB", "NUMSUB", arm_channel), [arm_channel.encode(), 0],
           "RESET exact cleanup")

    # Directed unsubscribe variants, including the no-argument "all" forms and cumulative counts.
    controls = Conn(host, port)
    control_a, control_b = f"{token}:control:a", f"{token}:control:b"
    expect(controls.command("SUBSCRIBE", control_a, control_b),
           [b"subscribe", control_a.encode(), 1], "multi subscribe one")
    expect(controls.read(), [b"subscribe", control_b.encode(), 2], "multi subscribe two")
    expect(controls.command("UNSUBSCRIBE", control_a),
           [b"unsubscribe", control_a.encode(), 1], "named unsubscribe")
    expect(controls.command("UNSUBSCRIBE"),
           [b"unsubscribe", control_b.encode(), 0], "unsubscribe all")
    pattern_a, pattern_b = f"{token}:pa:*", f"{token}:pb:*"
    expect(controls.command("PSUBSCRIBE", pattern_a, pattern_b),
           [b"psubscribe", pattern_a.encode(), 1], "multi psubscribe one")
    expect(controls.read(), [b"psubscribe", pattern_b.encode(), 2], "multi psubscribe two")
    expect(controls.command("PUNSUBSCRIBE", pattern_a),
           [b"punsubscribe", pattern_a.encode(), 1], "named punsubscribe")
    expect(controls.command("PUNSUBSCRIBE"),
           [b"punsubscribe", pattern_b.encode(), 0], "punsubscribe all")
    controls.close()

    for conn in subscribers:
        conn.close()
    subscribers.clear()

    # Shard-publish while connections register and disconnect. Half close before reading the
    # acknowledgement, exercising the pending-command lifetime fence; half close immediately after.
    churn_channel = f"{token}:churn"
    errors = []
    start = threading.Event()
    churn_deferred_before = int(info_stats(admin)["oob_frames_deferred"])

    def publish_loop():
        conn = Conn(host, port)
        try:
            start.wait()
            for sequence in range(500):
                result = conn.command("SPUBLISH", churn_channel, str(sequence))
                if not isinstance(result, int):
                    raise AssertionError(f"churn SPUBLISH returned {result!r}")
        except Exception as exc:  # surfaced in the main thread
            errors.append(exc)
        finally:
            conn.close()

    def churn_loop(worker):
        try:
            start.wait()
            for index in range(80):
                conn = Conn(host, port)
                conn.send("SSUBSCRIBE", churn_channel)
                if (index + worker) % 2:
                    expect(conn.read(), [b"ssubscribe", churn_channel.encode(), 1],
                           f"churn ack {worker}/{index}")
                conn.close(reset=True)
        except Exception as exc:
            errors.append(exc)

    threads = [threading.Thread(target=publish_loop)]
    threads += [threading.Thread(target=churn_loop, args=(worker,)) for worker in range(4)]
    for thread in threads:
        thread.start()
    start.set()
    for thread in threads:
        thread.join()
    if errors:
        raise errors[0]
    churn_deferred_after = int(info_stats(admin)["oob_frames_deferred"])
    if churn_deferred_after <= churn_deferred_before:
        raise AssertionError(
            "churn never parked a delivery behind an earlier SSUBSCRIBE ack: geometry missing")

    # ---- fanout redesign: batching, encode-once framing, ordering, mid-fanout teardown ----------
    # Every arm below asserts its mechanism FIRED, not merely that nothing broke: exact per-frame
    # equality, exact counter deltas, and a batching ratio that a per-delivery design cannot reach.

    # A. Concurrent publishers, one channel. Per PUBLISHER, each subscriber must see a strictly
    #    increasing sequence with no loss, no duplicate and no reordering; publishers may interleave
    #    with each other. Publish order on ONE channel is owned by that channel's single home.
    fan_channel = f"{token}:fan"
    npub, nsub, per_pub = 4, 12, 150
    fan_subs = []
    for index in range(nsub):
        conn = Conn(host, port, timeout=30)
        expect(conn.command("SUBSCRIBE", fan_channel),
               [b"subscribe", fan_channel.encode(), 1], f"fan subscribe {index}")
        fan_subs.append(conn)
    before = info_stats(admin)
    fan_errors = []
    fan_start = threading.Event()

    def fan_publish(pid):
        conn = Conn(host, port, timeout=30)
        try:
            fan_start.wait()
            batch = b"".join(encode("PUBLISH", fan_channel, f"{pid}:{seq}")
                             for seq in range(per_pub))
            conn.sock.sendall(batch)          # pipelined: the batching window this design creates
            for seq in range(per_pub):
                got = conn.read()
                if got != nsub:
                    raise AssertionError(f"publisher {pid} seq {seq} receivers {got!r}")
        except Exception as exc:
            fan_errors.append(exc)
        finally:
            conn.close()

    pubs = [threading.Thread(target=fan_publish, args=(pid,)) for pid in range(npub)]
    for thread in pubs:
        thread.start()
    fan_start.set()
    for thread in pubs:
        thread.join()
    if fan_errors:
        raise fan_errors[0]
    for index, conn in enumerate(fan_subs):
        seen = {pid: -1 for pid in range(npub)}
        for frame in range(npub * per_pub):
            got = conn.read()
            if not (isinstance(got, list) and len(got) == 3 and got[0] == b"message"
                    and got[1] == fan_channel.encode()):
                raise AssertionError(f"fan subscriber {index} frame {frame}: {got!r}")
            pid, seq = (int(part) for part in got[2].split(b":"))
            if seq != seen[pid] + 1:
                raise AssertionError(
                    f"fan subscriber {index}: publisher {pid} jumped {seen[pid]} -> {seq}")
            seen[pid] = seq
        for pid in range(npub):
            expect(seen[pid], per_pub - 1, f"fan subscriber {index} publisher {pid} tail")
    expect_no_frame(fan_subs[0], "fan subscriber 0 extra frame")

    # B. The batching itself. Deliveries must be counted exactly, and the whole point of the design
    #    is that far fewer batch events were posted than publishes -- a per-delivery scatter cannot
    #    produce this ratio. (Deliveries owned by the publishing IO need no event at all, so the
    #    batch count is a ceiling, not an equality.)
    after = info_stats(admin)
    delivered = int(after["pubsub_deliveries"]) - int(before["pubsub_deliveries"])
    batches = int(after["pubsub_delivery_batches"]) - int(before["pubsub_delivery_batches"])
    expect(delivered, npub * per_pub * nsub, "pubsub_deliveries delta")
    if batches >= npub * per_pub:
        raise AssertionError(
            f"fanout did not batch: {batches} posted events for {npub * per_pub} publishes")
    expect(after["pubsub_blobs"], "0", "pubsub_blobs drained after fanout")

    # C. RESP2 and RESP3 subscribers on ONE publish. The encode-once blob serves both protocols by
    #    swapping the leading header byte, so this arm is what proves the swap is correct.
    resp2_sub, resp3_sub = Conn(host, port), Conn(host, port)
    hello = resp3_sub.command("HELLO", "3")
    if isinstance(hello, RespError):
        raise AssertionError(f"HELLO 3: {hello!r}")
    mixed = f"{token}:mixed"
    expect(resp2_sub.command("SUBSCRIBE", mixed), [b"subscribe", mixed.encode(), 1], "mixed resp2")
    expect(resp3_sub.command("SUBSCRIBE", mixed), [b"subscribe", mixed.encode(), 1], "mixed resp3")
    expect(publisher.command("PUBLISH", mixed, "both"), 2, "mixed receivers")
    raw2 = resp2_sub.file.read(1)
    expect(raw2, b"*", "RESP2 subscriber gets an array frame")
    expect(resp2_sub.file.readline(), b"3\r\n", "RESP2 frame arity")
    raw3 = resp3_sub.file.read(1)
    expect(raw3, b">", "RESP3 subscriber gets a push frame")
    expect(resp3_sub.file.readline(), b"3\r\n", "RESP3 frame arity")
    for conn in (resp2_sub, resp3_sub):
        expect([conn.read() for _ in range(3)], [b"message", mixed.encode(), b"both"],
               "mixed frame body")
    # Pattern delivery to a RESP3 client reuses the same blob tail behind a push header.
    expect(resp3_sub.command("PSUBSCRIBE", f"{token}:mix*"),
           [b"psubscribe", f"{token}:mix*".encode(), 2], "mixed resp3 pattern")
    expect(publisher.command("PUBLISH", mixed, "again"), 3, "mixed pattern receivers")
    expect(resp2_sub.read(), [b"message", mixed.encode(), b"again"], "resp2 second frame")
    expect(resp3_sub.file.read(1), b">", "RESP3 exact push")
    expect(resp3_sub.file.readline(), b"3\r\n", "RESP3 exact arity")
    expect([resp3_sub.read() for _ in range(3)], [b"message", mixed.encode(), b"again"],
           "resp3 exact body")
    expect(resp3_sub.file.read(1), b">", "RESP3 pmessage push")
    expect(resp3_sub.file.readline(), b"4\r\n", "RESP3 pmessage arity")
    expect([resp3_sub.read() for _ in range(4)],
           [b"pmessage", f"{token}:mix*".encode(), mixed.encode(), b"again"], "resp3 pmessage body")
    resp2_sub.close()
    resp3_sub.close()

    # D. Subscriber disconnect mid-fanout. Half the subscribers are reset while a pipelined burst
    #    is in flight, so batches already queued name connections that no longer exist. The survivors
    #    must still receive every frame, in order, and the blob gauge must return to zero (ASAN).
    tear_channel = f"{token}:teardown"
    victims, keepers = [], []
    for index in range(16):
        conn = Conn(host, port, timeout=30)
        expect(conn.command("SUBSCRIBE", tear_channel),
               [b"subscribe", tear_channel.encode(), 1], f"teardown subscribe {index}")
        (victims if index % 2 else keepers).append(conn)
    tear_pub = Conn(host, port, timeout=30)
    tear_pub.sock.sendall(b"".join(encode("PUBLISH", tear_channel, f"t{seq}")
                                   for seq in range(400)))
    for conn in victims:
        conn.close(reset=True)               # RST mid-burst, while deliveries are queued for them
    for seq in range(400):
        got = tear_pub.read()
        if not isinstance(got, int):
            raise AssertionError(f"teardown PUBLISH {seq}: {got!r}")
    for index, conn in enumerate(keepers):
        for seq in range(400):
            expect(conn.read(), [b"message", tear_channel.encode(), f"t{seq}".encode()],
                   f"teardown keeper {index} frame {seq}")
        conn.close()
    tear_pub.close()

    # E. Subscriber-side backpressure. Removing the old fixed publish in-flight cap (10 messages)
    #    made `client-output-buffer-limit pubsub` -- Redis's own mechanism, with Redis's grammar --
    #    the only thing standing between a fast publisher and an unbounded subscriber buffer. So it
    #    has to be proven to fire, not assumed. A subscriber that never reads is filled past a
    #    1 MiB hard limit and must be disconnected, with the counter moving.
    saved_limits = admin.command("CONFIG", "GET", "client-output-buffer-limit")[1]
    expect(admin.command("CONFIG", "SET", "client-output-buffer-limit", "pubsub 1048576 0 0"),
           b"OK", "arm pubsub obuf limit")
    slow_channel = f"{token}:slow"
    slow = Conn(host, port, timeout=30)
    expect(slow.command("SUBSCRIBE", slow_channel),
           [b"subscribe", slow_channel.encode(), 1], "slow subscribe")
    disconnects_before = int(info_stats(admin)["client_output_buffer_limit_disconnections"])
    flood = Conn(host, port, timeout=30)
    payload = "x" * 4096
    dropped = False
    for _ in range(40):                          # ~4 MiB of frames, never read by `slow`
        flood.sock.sendall(b"".join(encode("PUBLISH", slow_channel, payload)
                                    for _ in range(100)))
        for _ in range(100):
            flood.read()
        if int(info_stats(admin)["client_output_buffer_limit_disconnections"]) > disconnects_before:
            dropped = True
            break
    if not dropped:
        raise AssertionError(
            "client-output-buffer-limit pubsub never fired: the publish path has no "
            "subscriber-side bound")
    slow.close(reset=True)
    flood.close()
    expect(admin.command("CONFIG", "SET", "client-output-buffer-limit", saved_limits.decode()),
           b"OK", "restore obuf limits")

    # F. Introspection aggregates against a Python-side model of the same population.
    model_exact, model_patterns, model_shard = {}, {}, {}
    model_conns = []
    for index in range(10):
        conn = Conn(host, port)
        channel = f"{token}:model:{index % 4}"
        expect(conn.command("SUBSCRIBE", channel)[0], b"subscribe", f"model subscribe {index}")
        model_exact[channel] = model_exact.get(channel, 0) + 1
        if index % 3 == 0:
            pattern = f"{token}:model:{index % 2}*"
            expect(conn.command("PSUBSCRIBE", pattern)[0], b"psubscribe", f"model psub {index}")
            model_patterns[pattern] = model_patterns.get(pattern, 0) + 1
        if index % 4 == 1:
            shard = f"{token}:model:s{index % 3}"
            expect(conn.command("SSUBSCRIBE", shard)[0], b"ssubscribe", f"model ssub {index}")
            model_shard[shard] = model_shard.get(shard, 0) + 1
        model_conns.append(conn)
    expect(sorted(admin.command("PUBSUB", "CHANNELS", f"{token}:model:*")),
           sorted(name.encode() for name in model_exact), "model PUBSUB CHANNELS")
    expect(sorted(admin.command("PUBSUB", "SHARDCHANNELS", f"{token}:model:*")),
           sorted(name.encode() for name in model_shard), "model PUBSUB SHARDCHANNELS")
    probe = sorted(model_exact) + [f"{token}:model:absent"]
    wanted = []
    for name in probe:
        wanted.extend((name.encode(), model_exact.get(name, 0)))
    expect(admin.command("PUBSUB", "NUMSUB", *probe), wanted, "model PUBSUB NUMSUB")
    shard_probe = sorted(model_shard)
    shard_wanted = []
    for name in shard_probe:
        shard_wanted.extend((name.encode(), model_shard[name]))
    expect(admin.command("PUBSUB", "SHARDNUMSUB", *shard_probe), shard_wanted,
           "model PUBSUB SHARDNUMSUB")
    # NUMPAT counts DISTINCT patterns, not pattern subscriptions -- two clients on one pattern is 1.
    expect(admin.command("PUBSUB", "NUMPAT"), len(model_patterns), "model PUBSUB NUMPAT")
    for conn in model_conns:
        conn.close()
    for conn in fan_subs:
        conn.close()

    arms.close()
    publisher.close()
    zero_keys = (
        "pubsub_channels", "pubsub_subscriptions", "pubsub_patterns",
        "pubsubshard_channels", "pubsubshard_subscriptions",
        "pubsub_home_entries", "pubsub_inflight", "pubsub_pending_commands", "pubsub_blobs",
    )
    deadline = time.monotonic() + 10
    while True:
        stats = info_stats(admin)
        if all(stats.get(key) == "0" for key in zero_keys):
            break
        if time.monotonic() >= deadline:
            raise AssertionError(
                "pub/sub gauges did not drain: " +
                ", ".join(f"{key}={stats.get(key)}" for key in zero_keys))
        time.sleep(0.02)

    expect(admin.command("PUBSUB", "SHARDNUMSUB", churn_channel), [churn_channel.encode(), 0],
           "churn SHARDNUMSUB cleanup")
    admin.close()
    print(f"pubsub: PASS (regular_fanout={nsubs}, shard_fanout={nsubs}, "
          f"concurrent_pub={npub}x{per_pub}x{nsub}, batches={batches} for {npub * per_pub} publishes, "
          f"ordered_messages={nmessages}, shard_churn=320)")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""XGROUP/XINFO outer-arity and cross-executor routing battery.

Usage: tests/contarity.py HOST PORT

Boot requirement: at least two executor threads and `--enable-debug-command yes`. The standard
gate geometry is `--shards 16 --ratio 6:2`. DEBUG SHARD resolves every candidate on this boot's
random hash seed; DEBUG LBSIGNALS supplies the live shard-to-executor map. The battery refuses to
run unless it finds a key owned by an executor different from shard 0's executor, because pinning
every SubcmdRoute child to the same owner as HELP is the routing regression this test must expose.
"""

import socket
import sys


HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 6379


class RespError(Exception):
    def __init__(self, message):
        self.message = message

    def __eq__(self, other):
        return isinstance(other, RespError) and self.message == other.message

    def __repr__(self):
        return "RespError(%r)" % self.message


def frame(*arguments):
    values = [value if isinstance(value, bytes) else str(value).encode()
              for value in arguments]
    return (b"*%d\r\n" % len(values) +
            b"".join(b"$%d\r\n%s\r\n" % (len(value), value) for value in values))


class Connection:
    def __init__(self):
        self.socket = socket.create_connection((HOST, PORT), timeout=10)
        self.file = self.socket.makefile("rb")

    def read(self):
        line = self.file.readline()
        if not line:
            raise EOFError("server closed the connection")
        marker, payload = line[:1], line[1:-2]
        if marker == b"+":
            return payload
        if marker == b"-":
            return RespError(payload)
        if marker == b":":
            return int(payload)
        if marker in (b"$", b"="):
            length = int(payload)
            if length < 0:
                return None
            data = self.file.read(length)
            if self.file.read(2) != b"\r\n":
                raise AssertionError("bad bulk trailer")
            return data[4:] if marker == b"=" and data[3:4] == b":" else data
        if marker in (b"*", b"~"):
            count = int(payload)
            return None if count < 0 else [self.read() for _ in range(count)]
        if marker == b"%":
            return [self.read() for _ in range(int(payload) * 2)]
        raise AssertionError("unsupported RESP marker %r" % marker)

    def command(self, *arguments):
        self.socket.sendall(frame(*arguments))
        return self.read()

    def close(self):
        self.file.close()
        self.socket.close()


XGROUP_HELP = [line.encode() for line in (
    "XGROUP <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
    "CREATE <key> <groupname> <id|$> [option]",
    "    Create a new consumer group. Options are:",
    "    * MKSTREAM",
    "      Create the empty stream if it does not exist.",
    "    * ENTRIESREAD entries_read",
    "      Set the group's entries_read counter (internal use).",
    "CREATECONSUMER <key> <groupname> <consumer>",
    "    Create a new consumer in the specified group.",
    "DELCONSUMER <key> <groupname> <consumer>",
    "    Remove the specified consumer.",
    "DESTROY <key> <groupname>",
    "    Remove the specified group.",
    "SETID <key> <groupname> <id|$> [ENTRIESREAD entries_read]",
    "    Set the current group ID and entries_read counter.",
    "HELP",
    "    Print this help.",
)]

XINFO_HELP = [line.encode() for line in (
    "XINFO <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
    "CONSUMERS <key> <groupname>",
    "    Show consumers of <groupname>.",
    "GROUPS <key>",
    "    Show the stream consumer groups.",
    "STREAM <key> [FULL [COUNT <count>]",
    "    Show information about the stream.",
    "HELP",
    "    Print this help.",
)]


def expect(connection, label, command, wanted):
    got = connection.command(*command)
    if got != wanted:
        raise AssertionError("%s: got %r, wanted %r" % (label, got, wanted))
    print("  ok   " + label, flush=True)


def topology(connection):
    raw = connection.command("DEBUG", "LBSIGNALS")
    if isinstance(raw, RespError) or not isinstance(raw, bytes):
        raise AssertionError(
            "DEBUG LBSIGNALS unavailable; boot with --enable-debug-command yes: %r" % raw)
    executors = set()
    shard_owner = {}
    for line in raw.splitlines():
        fields = line.split()
        if len(fields) >= 3 and fields[0] == b"thread" and fields[2] == b"ex":
            executors.add(int(fields[1]))
        elif len(fields) >= 3 and fields[0] == b"shard":
            shard_owner[int(fields[1])] = int(fields[2])
    if len(executors) < 2:
        raise AssertionError("contarity needs at least two executor threads; found %r" % executors)
    if 0 not in shard_owner:
        raise AssertionError("DEBUG LBSIGNALS did not report shard 0")
    return executors, shard_owner


def cross_owner_key(connection, shard_owner):
    shard_zero_owner = shard_owner[0]
    for index in range(4000):
        key = "contarity:cross:%04d:" % index + "z" * 24
        shard = connection.command("DEBUG", "SHARD", key)
        if isinstance(shard, RespError) or not isinstance(shard, int):
            raise AssertionError(
                "DEBUG SHARD unavailable; boot with --enable-debug-command yes: %r" % shard)
        if shard not in shard_owner:
            raise AssertionError("DEBUG SHARD returned unreported shard %r" % shard)
        if shard_owner[shard] != shard_zero_owner:
            return key, shard, shard_owner[shard], shard_zero_owner
    raise AssertionError(
        "could not find a key whose owner differs from shard 0's executor after 4000 probes")


def command_arity(connection, name):
    reply = connection.command("COMMAND", "INFO", name)
    if (not isinstance(reply, list) or len(reply) != 1 or
            not isinstance(reply[0], list) or len(reply[0]) < 2):
        raise AssertionError("COMMAND INFO %s malformed: %r" % (name, reply))
    return reply[0][1]


def main():
    connection = Connection()
    try:
        expect(connection, "XGROUP HELP exact Redis 7.4.2 array",
               ("XGROUP", "HELP"), XGROUP_HELP)
        expect(connection, "XINFO HELP exact Redis 7.4.2 array",
               ("XINFO", "HELP"), XINFO_HELP)
        expect(connection, "bare XGROUP retains outer arity error", ("XGROUP",),
               RespError(b"ERR wrong number of arguments for 'xgroup' command"))
        expect(connection, "unknown XGROUP arm names the arm", ("XGROUP", "BOGUS"),
               RespError(b"ERR unknown subcommand 'BOGUS'. Try XGROUP HELP."))
        expect(connection, "known XGROUP arm uses generated child arity",
               ("XGROUP", "CREATE", "key", "group"),
               RespError(b"ERR wrong number of arguments for 'xgroup|create' command"))
        expect(connection, "XGROUP HELP surplus names generated child",
               ("XGROUP", "HELP", "extra"),
               RespError(b"ERR wrong number of arguments for 'xgroup|help' command"))
        expect(connection, "XINFO HELP surplus names generated child",
               ("XINFO", "HELP", "extra"),
               RespError(b"ERR wrong number of arguments for 'xinfo|help' command"))

        xgroup_arity = command_arity(connection, "xgroup")
        xinfo_arity = command_arity(connection, "xinfo")
        if (xgroup_arity, xinfo_arity) != (-2, -2):
            raise AssertionError("advertised arities changed: XGROUP=%r XINFO=%r" %
                                 (xgroup_arity, xinfo_arity))
        print("  ok   COMMAND INFO arities remain XGROUP=-2 XINFO=-2", flush=True)

        executors, shard_owner = topology(connection)
        key, shard, owner, shard_zero_owner = cross_owner_key(connection, shard_owner)
        if owner not in executors or shard_zero_owner not in executors or owner == shard_zero_owner:
            raise AssertionError("invalid executor geometry: owner=%r shard0_owner=%r ex=%r" %
                                 (owner, shard_zero_owner, executors))
        print("  ok   geometry key=%s shard=%d owner-ex=%d shard0-owner-ex=%d executors=%s" %
              (key, shard, owner, shard_zero_owner, sorted(executors)), flush=True)

        expect(connection, "clean database", ("FLUSHALL",), b"OK")
        expect(connection, "XGROUP CREATE crosses to key owner",
               ("XGROUP", "CREATE", key, "g", "0-0", "MKSTREAM"), b"OK")
        expect(connection, "ordinary route sees CREATE's stream", ("XLEN", key), 0)
        expect(connection, "XGROUP SETID crosses to key owner",
               ("XGROUP", "SETID", key, "g", "0-0"), b"OK")
        expect(connection, "XGROUP CREATECONSUMER crosses to key owner",
               ("XGROUP", "CREATECONSUMER", key, "g", "c"), 1)
        expect(connection, "XGROUP DELCONSUMER crosses to key owner",
               ("XGROUP", "DELCONSUMER", key, "g", "c"), 0)
        expect(connection, "XGROUP DESTROY crosses to key owner",
               ("XGROUP", "DESTROY", key, "g"), 1)
        expect(connection, "ordinary route still sees stream after DESTROY", ("XLEN", key), 0)

        missing = key + ":missing"
        expect(connection, "XINFO STREAM missing key reaches keyed owner",
               ("XINFO", "STREAM", missing), RespError(b"ERR no such key"))
    finally:
        connection.close()

    print("CONTARITY PASS: help=2 arity=7 keyed_xgroup=5 executors>=2", flush=True)


main()

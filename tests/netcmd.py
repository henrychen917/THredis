#!/usr/bin/env python3
"""Network and command audit regressions, using an existing server (no launch/skip/rate tests)."""
import sys
from _lib import Conn, RespError, encode


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def run(host, port):
    c = Conn(host, port, timeout=5)
    try:
        c.raw(b"\r\n \t\r\n" + encode("PING", "after-empty"))
        require(c.read() == b"after-empty", "empty inline prefix consumed")
        for command in (("BOGUS",), ("GET",), ("XGROUP", "CREATE")):
            c.raw(encode("CLIENT", "REPLY", "SKIP") + encode(*command) + encode("PING", "visible"))
            require(c.read() == b"visible", "early error consumed SKIP exactly once")
            c.raw(encode("CLIENT", "REPLY", "OFF") + encode(*command) +
                  encode("CLIENT", "REPLY", "ON") + encode("PING", "after-on"))
            require(c.read() == b"OK" and c.read() == b"after-on", "early error obeyed OFF")
        c.raw(encode(b"bogus\r\n:123\r\n") + encode("PING", "framing"))
        require(isinstance(c.read(), RespError), "binary command rejected")
        require(c.read() == b"framing", "binary command cannot inject reply frames")
        for verb in (("XPENDING", b"absent\r\n:123\r\n", "g"),
                     ("XGROUP", "SETID", "absent", b"g\r\n:123\r\n", "0")):
            c.raw(encode(*verb) + encode("PING", "nogroup"))
            require(isinstance(c.read(), RespError), "NOGROUP error returned")
            require(c.read() == b"nogroup", "NOGROUP preserves frame boundaries")
        c.cmd("DEL", "netcmd:stream")
        require(c.cmd("XGROUP", "CREATE", "netcmd:stream", "g", "0", "MKSTREAM") == b"OK", "empty group exists")
        require(c.cmd("XAUTOCLAIM", "netcmd:stream", "g", "c", 0, 0, "COUNT", 9223372036854775807)
                == [b"0-0", [], []], "huge COUNT bounded by empty PEL")
        c.raw(encode("XPENDING", "netcmd:stream", "g", "(18446744073709551615-18446744073709551615", "+", 1) +
              encode("PING", "pending"))
        require(c.read() == [] and c.read() == b"pending", "exclusive max XPENDING has an empty reply")

        for source in ("return {ok='OK\\r\\n:42'}", "return {err='ERR bad\\r\\n:42'}"):
            c.raw(encode("EVAL", source, 0) + encode("PING", "lua-framing"))
            result = c.read()
            require(isinstance(result, (bytes, RespError)), "Lua status/error converted")
            require(c.read() == b"lua-framing", "Lua table cannot inject frames")
        for body in ("error('conversion failed')", "while true do end"):
            source = "return setmetatable({}, {__index=function() " + body + " end})"
            require(c.cmd("EVAL", source, 0) == [], "conversion uses raw table fields without executing __index")
        require(isinstance(c.cmd("EVAL", "local ok=pcall(function() while true do end end); return 42", 0), RespError),
                "catching instruction-limit error cannot turn activation into success")
        print("ok: protocol, suppression, stream bounds, Lua conversion/budget")

        before = c.cmd("CONFIG", "GET", "proto-max-bulk-len")[1]
        try:
            require(c.cmd("CONFIG", "SET", "proto-max-bulk-len", 1048576) == b"OK", "runtime bulk limit set")
            require(c.cmd("CONFIG", "GET", "proto-max-bulk-len")[1] == b"1048576", "runtime bulk limit confirmed")
            fresh = Conn(host, port, timeout=10)
            try:
                value = b"v" * (600 * 1024)
                require(fresh.cmd("MSET", "netcmd:a", value, "netcmd:b", value) == b"OK", "two legal bulks exceed one-bulk receive cap")
                require(fresh.cmd("STRLEN", "netcmd:a") == len(value), "first legal bulk stored")
                require(fresh.cmd("STRLEN", "netcmd:b") == len(value), "second legal bulk stored")
                for request in (("SETRANGE", "netcmd:grow", 1048576, "x"),
                                ("SETBIT", "netcmd:grow", 8388608, 1),
                                ("BITFIELD", "netcmd:grow", "SET", "u8", 8388608, 1),
                                ("APPEND", "netcmd:a", value)):
                    require(isinstance(fresh.cmd(*request), RespError), "runtime growth limit rejects " + request[0])
                require(fresh.cmd("EXISTS", "netcmd:grow") == 0, "rejected growth did not create key")
                require(fresh.cmd("STRLEN", "netcmd:a") == len(value), "rejected append preserved value")
            finally:
                fresh.close()
        finally:
            require(c.cmd("CONFIG", "SET", "proto-max-bulk-len", before) == b"OK", "runtime bulk limit restored")
        print("ok: whole-command reception and runtime string/bitmap limits")

        t = Conn(host, port, timeout=5)
        try:
            require(c.cmd("XADD", "netcmd:tracked", "*", "f", "v") is not None, "tracked stream exists")
            t.cmd("HELLO", 3)
            require(t.cmd("CLIENT", "TRACKING", "ON") == b"OK", "ordinary RESP3 tracking armed")
            t.raw(encode("XREAD", "STREAMS", "netcmd:tracked", "bad-id") + encode("PING", "tracking-parser"))
            require(isinstance(t.read(), RespError) and t.read() == b"tracking-parser",
                    "tracking registration does not emit an extra parser error")
            require(t.cmd("XREAD", "STREAMS", "netcmd:tracked", "0") is not None, "XREAD delivered existing stream")
            c.cmd("XADD", "netcmd:tracked", "*", "f", "next")
            require(t.read() == [b"invalidate", [b"netcmd:tracked"]], "XREAD alone registered its dynamic stream key")
            require(c.cmd("SET", "netcmd:flush", "old") == b"OK", "flush source exists")
            require(t.cmd("GET", "netcmd:flush") == b"old", "flush source is tracked")
            require(c.cmd("FLUSHALL") == b"OK", "owner flush completed")
            require(t.read() == [b"invalidate", None], "completed flush invalidated tracking client")
            require(t.cmd("GET", "netcmd:flush") is None, "read after invalidation observes clear")
        finally:
            t.close()
        print("ok: XREAD tracking without BCAST or a prior static-key read")
    finally:
        c.close()


if __name__ == "__main__":
    run(sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1", int(sys.argv[2]) if len(sys.argv) > 2 else 7899)

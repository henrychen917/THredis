// resp.h — RESP2 request parsing and reply formatting.
//
// Patterned on the protocol as documented and as implemented in redis-7.2 / valkey (BSD-3). Nothing
// here is copied from our own Redis 8.6.2 fork, which carries RSALv2/SSPLv1 and would contaminate
// the licence of this tree.
//
// The parser produces Slices INTO the read buffer and never copies. That is the point, and it is
// also why Conn's read buffer is append-only while ops are in flight — see conn.h.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <charconv>
#include <cmath>
#include "../base/numeric.h"
#include "../base/slice.h"
#include "../exec/op.h"

namespace tomo {

enum class ParseResult { Ok, Incomplete, Error };

// Read decimal digits terminated by CRLF, advancing `pos` past the CRLF.
//
// strtol was doing this, and strtol is a general-purpose parser: it skips leading whitespace,
// accepts signs, honours a base argument and consults the locale — none of which a RESP length
// field may contain. It also cannot tell "no digits yet" from "not a number", so it silently
// returned 0 for a truncated header instead of asking for more bytes. This reads exactly what the
// protocol allows and distinguishes Incomplete from Error, which the old code could not.
//
// Returns Ok / Incomplete / Error. Bounded by `maxv` so a hostile length cannot overflow.
inline ParseResult parse_len_crlf(const char* buf, uint32_t len, uint32_t& pos,
                                  uint64_t maxv, uint64_t& out) {
    uint32_t i = pos;
    uint64_t v = 0;
    uint32_t digits = 0;
    while (i < len) {
        const char c = buf[i];
        if (c == '\r') {
            if (i + 1 >= len) return ParseResult::Incomplete;
            if (buf[i + 1] != '\n' || digits == 0) return ParseResult::Error;
            pos = i + 2;
            out = v;
            return ParseResult::Ok;
        }
        if (c < '0' || c > '9') return ParseResult::Error;
        v = v * 10 + static_cast<uint64_t>(c - '0');
        if (v > maxv) return ParseResult::Error;
        digits++;
        i++;
    }
    return ParseResult::Incomplete;
}

// Scans one command from buf[pos, len). On Ok, fills `op` and advances `pos` past the command.
// On Incomplete, leaves pos untouched — more bytes are needed.
//
// The limits are a TEMPLATE variant, not runtime parameters: carrying them as arguments through
// the hottest loop in the server (they were briefly defaulted parameters for the unauthenticated
// pre-AUTH limits) cost +83 instructions/op on loopback and −5.1% on the wire p128 GET cell —
// register-carried limits defeat the immediate-folding the comparisons had always compiled to.
// The unlimited instantiation folds the constants exactly as the original code did; only the
// pre-AUTH path (predicted-false at the call site) pays for real arguments.
template <bool kLimited>
inline ParseResult resp_parse_t(const char* buf, uint32_t len, uint32_t& pos, Op& op,
                                const char** err, uint64_t max_multibulk, uint64_t max_bulk) {
    if constexpr (!kLimited) {
        max_multibulk = 1024 * 1024;
        max_bulk = 512ull * 1024 * 1024;
    }
    const uint32_t start = pos;
    if (pos >= len) return ParseResult::Incomplete;

    auto find_crlf = [&](uint32_t from, uint32_t& out) -> bool {
        for (uint32_t i = from; i + 1 < len; i++)
            if (buf[i] == '\r' && buf[i + 1] == '\n') { out = i; return true; }
        return false;
    };

    if (buf[pos] != '*') {
        // Inline command: whitespace-separated, CRLF-terminated. Kept because every debugging
        // session uses it through nc/telnet and it costs almost nothing.
        uint32_t eol;
        if (!find_crlf(pos, eol)) return ParseResult::Incomplete;
        uint32_t i = pos;
        while (i < eol) {
            while (i < eol && (buf[i] == ' ' || buf[i] == '\t')) i++;
            uint32_t b = i;
            while (i < eol && buf[i] != ' ' && buf[i] != '\t') i++;
            if (i > b && !op.push_arg(Slice(buf + b, i - b))) {
                *err = "ERR out of memory parsing inline command";
                return ParseResult::Error;
            }
        }
        pos = eol + 2;
        return op.argc() ? ParseResult::Ok : ParseResult::Incomplete;  // bare CRLF: ignore
    }

    uint32_t p = pos + 1;                      // past '*'
    uint64_t nargs = 0;
    ParseResult r = parse_len_crlf(buf, len, p, max_multibulk, nargs);
    if (r == ParseResult::Incomplete) return ParseResult::Incomplete;
    if (r == ParseResult::Error || nargs == 0) { *err = "ERR invalid multibulk length"; return ParseResult::Error; }

    for (uint64_t a = 0; a < nargs; a++) {
        if (p >= len) { pos = start; return ParseResult::Incomplete; }
        if (buf[p] != '$') { *err = "ERR expected '$'"; return ParseResult::Error; }
        p++;
        uint64_t blen = 0;
        r = parse_len_crlf(buf, len, p, max_bulk, blen);
        if (r == ParseResult::Incomplete) { pos = start; return ParseResult::Incomplete; }
        if (r == ParseResult::Error) { *err = "ERR invalid bulk length"; return ParseResult::Error; }
        if (p + blen + 2 > len) { pos = start; return ParseResult::Incomplete; }
        if (!op.push_arg(Slice(buf + p, static_cast<uint32_t>(blen)))) {
            *err = "ERR out of memory parsing command";
            return ParseResult::Error;
        }
        p += static_cast<uint32_t>(blen) + 2;
    }
    pos = p;
    return ParseResult::Ok;
}

inline ParseResult resp_parse(const char* buf, uint32_t len, uint32_t& pos, Op& op,
                              const char** err) {
    return resp_parse_t<false>(buf, len, pos, op, err, 0, 0);
}

inline ParseResult resp_parse_limited(const char* buf, uint32_t len, uint32_t& pos, Op& op,
                                      const char** err, uint64_t max_multibulk,
                                      uint64_t max_bulk) {
    return resp_parse_t<true>(buf, len, pos, op, err, max_multibulk, max_bulk);
}

// ---- integer formatting -------------------------------------------------------------------------
// snprintf parses a format string at RUNTIME, every call. On the reply path that is a formatting
// interpreter running millions of times a second to emit at most 20 digits. These write digits
// directly, backwards into a scratch and then copied forward, which needs no division-by-10 chain
// longer than the number actually has.
inline uint32_t u64_to_dec(char* dst, uint64_t v) {
    char tmp[20];
    uint32_t n = 0;
    do { tmp[n++] = static_cast<char>('0' + (v % 10)); v /= 10; } while (v);
    for (uint32_t i = 0; i < n; i++) dst[i] = tmp[n - 1 - i];
    return n;
}
inline uint32_t i64_to_dec(char* dst, int64_t v) {
    if (v < 0) { *dst = '-'; return 1 + u64_to_dec(dst + 1, static_cast<uint64_t>(-(v + 1)) + 1); }
    return u64_to_dec(dst, static_cast<uint64_t>(v));
}

// ---- reply formatting. Handlers call these; they append RESP into the op's own buffer. ----------
template <typename Buf> inline void reply_ok(Buf&& b)   { b.append("+OK\r\n", 5); }
template <typename Buf> inline void reply_nil(Buf&& b)  { b.append("$-1\r\n", 5); }
template <typename Buf> inline void reply_pong(Buf&& b) { b.append("+PONG\r\n", 7); }
template <typename Buf> inline void reply_null_array(Buf&& b) { b.append("*-1\r\n", 5); }
template <typename Buf> inline void reply_emptystr(Buf&& b) { b.append("$0\r\n\r\n", 6); }
template <typename Buf> inline void reply_wrongtype(Buf&& b) {
    b.append("-WRONGTYPE Operation against a key holding the wrong kind of value\r\n", 68);
}
template <typename Buf> inline void reply_syntax(Buf&& b) {
    b.append("-ERR syntax error\r\n", 19);
}
template <typename Buf> inline void reply_outofrange(Buf&& b) {
    // Redis's getRangeLongFromObject names the bounds it enforces; every caller of this helper
    // (SRANDMEMBER / ZRANDMEMBER / HRANDFIELD counts) is one of those sites.
    static constexpr char kMsg[] =
        "-ERR value is out of range, value must between "
        "-9223372036854775807 and 9223372036854775807\r\n";
    b.append(kMsg, sizeof(kMsg) - 1);
}

template <typename Buf> inline void reply_err(Buf&& b, const char* msg) {
    b.push_back('-'); b.append(msg, std::strlen(msg)); b.append("\r\n", 2);
}
template <typename Buf> inline void reply_simple(Buf&& b, const char* msg) {
    b.push_back('+'); b.append(msg, std::strlen(msg)); b.append("\r\n", 2);
}
template <typename Buf> inline void reply_int(Buf&& b, long long v) {
    char* p = b.reserve(24);
    char* q = p;
    *q++ = ':';
    q += i64_to_dec(q, v);
    *q++ = '\r'; *q++ = '\n';
    b.advance(static_cast<size_t>(q - p));
}

template <typename Buf> inline void reply_array_header(Buf&& b, uint64_t n) {
    char* p = b.reserve(24);
    char* q = p;
    *q++ = '*';
    q += u64_to_dec(q, n);
    *q++ = '\r'; *q++ = '\n';
    b.advance(static_cast<size_t>(q - p));
}

template <typename Buf> inline void reply_bulk_header(Buf&& b, uint32_t len) {
    char* p = b.reserve(24);
    char* q = p;
    *q++ = '$';
    q += u64_to_dec(q, len);
    *q++ = '\r'; *q++ = '\n';
    b.advance(static_cast<size_t>(q - p));
}

// ONE reserve, ONE memcpy, no snprintf and no re-entry into the buffer. The previous version did a
// reserve plus three appends, and each append re-checked capacity — for a reply whose total size is
// known exactly before a byte is written.
template <typename Buf> inline void reply_bulk(Buf&& b, Slice s) {
    char* p = b.reserve(24 + static_cast<size_t>(s.n) + 2);
    char* q = p;
    *q++ = '$';
    q += u64_to_dec(q, s.n);
    *q++ = '\r'; *q++ = '\n';
    std::memcpy(q, s.p, s.n); q += s.n;
    *q++ = '\r'; *q++ = '\n';
    b.advance(static_cast<size_t>(q - p));
}

// THE canonical double text lives in src/base/numeric.h: redis renders the RESP2 bulk score and
// the RESP3 "," from one d2string, and the parsers that must accept exactly what it can produce
// belong beside it. This header keeps only the RESP framing.

// RESP2 has no native double: redis sends this same text as a bulk string.
template <typename Buf> inline void reply_double(Buf&& b, double value) {
    char text[kDoubleTextMax];
    const uint32_t length = redis_double_text(text, sizeof(text), value);
    reply_bulk(b, Slice(text, length));
}

}  // namespace tomo

// RESP3 is reply-only. Keeping its protocol-selecting builders in a feature header leaves the
// request parser and the established RESP2 reply_bulk/reply_int implementations untouched.
#include "resp3.h"

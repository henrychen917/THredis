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
#include <cstdlib>
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
// The one-argument append() below is the LITERAL overload (src/exec/op.h, src/base/slice.h): the
// length stays a compile-time constant all the way to the store, so a fixed reply costs a store
// instead of a call to the out-of-line append plus a call to memcpy. Same bytes as before, NUL
// excluded. Only replies that fit in a machine word or two are written this way; the longer error
// texts below keep the explicit length and the out-of-line path.
//
// CODED REPLIES. Where the sink is an Op::Sink (the executor's), a fixed reply records a ReplyCode
// and writes NO bytes; the connection's owner formats it at retire. Where it is a plain SmallBuf
// (pub/sub frames, MONITOR lines, script sub-buffers -- all already owner-side) there is no code
// to set and the literal append below is used unchanged. The `requires` test is what makes one
// helper serve both, so no call site changes and no reply text is written twice.
//
// b.code() returns false on a non-empty sink, which is how a fixed reply nested inside a larger
// one (an EXEC element after its array header) falls back to bytes and stays in order.
#define TOMO_CODED_REPLY(sink, codeexpr)                                          \
    if constexpr (requires { sink.code(codeexpr); }) { if (sink.code(codeexpr)) return; }

// always_inline ON THE FIXED-REPLY HELPERS. Adding the coded attempt grew these bodies just
// enough to cross GCC's inlining threshold, and the loss was not the four instructions of the
// attempt -- it was that cmd_get stopped inlining reply_null and started CALLING it, which
// measured +22 instructions per 2s GET-miss on a deterministic replay. These are all one store
// plus a capacity test; they were inlined before this branch and must stay inlined. reply_int is
// deliberately NOT in this list: it carries the digit loop and was already out of line in base,
// so forcing it would change base's own decision.
template <typename Buf> __attribute__((always_inline)) inline void reply_ok(Buf&& b)   {
    TOMO_CODED_REPLY(b, ReplyCode::Ok)
    b.append("+OK\r\n");
}
template <typename Buf> __attribute__((always_inline)) inline void reply_nil(Buf&& b)  {
    TOMO_CODED_REPLY(b, ReplyCode::Nil)
    b.append("$-1\r\n");
}
template <typename Buf> __attribute__((always_inline)) inline void reply_pong(Buf&& b) {
    TOMO_CODED_REPLY(b, ReplyCode::Pong)
    b.append("+PONG\r\n");
}
template <typename Buf> __attribute__((always_inline)) inline void reply_null_array(Buf&& b) {
    TOMO_CODED_REPLY(b, ReplyCode::NullArray)
    b.append("*-1\r\n");
}
template <typename Buf> __attribute__((always_inline)) inline void reply_emptystr(Buf&& b) {
    TOMO_CODED_REPLY(b, ReplyCode::EmptyStr)
    b.append("$0\r\n\r\n");
}
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
// AN INTEGER IS A VALUE, NOT A FORMAT. DEL's count, EXISTS's count, SETNX's 0/1, INCR's new
// value, LPUSH/SADD/HSET's length -- the executor computes the number, the owner renders the
// digits. Outside int32 the code path is declined (the hole in Op holds four bytes, not eight) and
// the digits are formatted here exactly as before; the wire bytes are identical either way.
template <typename Buf> inline void reply_int(Buf&& b, long long v) {
    if constexpr (requires { b.code(ReplyCode::Int, int32_t{0}); }) {
        if (__builtin_expect(v >= -2147483648LL && v <= 2147483647LL, true) &&
            b.code(ReplyCode::Int, static_cast<int32_t>(v)))
            return;
    }
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

// THE OWNER'S SIDE. Renders a coded reply into dst, which must have kReplyCodeMax writable bytes.
// Runs on the connection's own thread at retire, writing into that thread's own fill buffer -- so
// these bytes are produced and consumed by one core, which is the point of the code.
//
// SHAPE MATTERS HERE, and the obvious shape was wrong. Written as a switch over ReplyCode this
// became an out-of-line call with a jump table (`notrack jmp *%rax`) and a stack canary -- an
// indirect branch and a frame, to replace a five-byte memcpy. That is not a saving. A fixed reply
// is a CONSTANT, so it is a table: one 16-byte load-store at a computed index, no branch, no call,
// and the length comes from the same cache line as the bytes. Writing all 16 bytes is safe and
// deliberate -- every caller reserves kReplyCodeMax -- and it keeps the store width constant.
struct FixedReply {
    char     bytes[16];
    uint32_t len;
};

// Indexed by ReplyCode. Slot 0 (None) is unreachable: every caller tests reply_code_ != 0 first,
// which inlining folds away. Its zero length is a fail-quiet, not a design.
inline constexpr FixedReply kFixedReplies[] = {
    {{0},                 0},   // None -- unreachable
    {"+OK\r\n",           5},
    {"$-1\r\n",           5},
    {"+PONG\r\n",         7},
    {"*-1\r\n",           5},
    {"$0\r\n\r\n",        6},
    {"_\r\n",             3},
    {"#t\r\n",            4},
    {"#f\r\n",            4},
    {{0},                 0},   // Int -- handled below, never read from this table
};

// Digits straight into the destination, no scratch array -- an array here is what pulled a stack
// protector into the hot path. Two passes over at most ten digits, both branch-predictable.
inline uint32_t u32_to_dec_direct(char* dst, uint32_t v) {
    uint32_t n = 1;
    for (uint32_t t = v; t >= 10; t /= 10) n++;
    for (uint32_t i = n; i-- > 0; ) { dst[i] = static_cast<char>('0' + v % 10); v /= 10; }
    return n;
}

// The digits, deliberately OUT OF LINE. Inlined, this loop put 30-odd instructions into the per-op
// retire lambda at every one of its twenty-odd instantiations; the lambda is the io thread's
// hottest code and every op walks past those bytes whether or not it formats an integer.
// ":-2147483648\r\n" is 14 bytes, inside kReplyCodeMax.
__attribute__((noinline))
inline uint32_t format_reply_code_int(char* dst, int32_t ival) {
    char* q = dst;
    *q++ = ':';
    uint32_t mag;
    if (ival < 0) { *q++ = '-'; mag = static_cast<uint32_t>(-static_cast<int64_t>(ival)); }
    else          { mag = static_cast<uint32_t>(ival); }
    q += u32_to_dec_direct(q, mag);
    *q++ = '\r'; *q++ = '\n';
    return static_cast<uint32_t>(q - dst);
}

// always_inline, and measured both ways. Left to its own judgement GCC emitted a CALL at all
// twenty-odd retire sites, so a five-byte reply paid a call to avoid a five-byte memcpy. Inlined
// WITH the digits it bloated the retire lambda instead. This shape is the one that is small: the
// fixed reply -- which is every SET, the reply this change exists for -- becomes a table index, a
// 16-byte load-store and a length load, and an integer reply pays one call, exactly as it paid one
// memcpy call before.
__attribute__((always_inline))
inline uint32_t format_reply_code(char* dst, uint8_t code, int32_t ival) {
    if (code != static_cast<uint8_t>(ReplyCode::Int)) {
        const FixedReply& f = kFixedReplies[code];
        __builtin_memcpy(dst, f.bytes, sizeof(f.bytes));
        return f.len;
    }
    return format_reply_code_int(dst, ival);
}

// For the few paths that SPLICE an op's reply bytes mid-flight instead of retiring them (the
// borrowed-value flush inside assemble_mget). Turns a code back into the bytes it stands for, in
// order, so the splice sees a normal byte buffer. A no-op for the ordinary reply.
// NOINLINE, and callers guard it with op.reply_code_ themselves. Inlined it dragged
// SmallBuf::reserve's grow path -- malloc, memcpy, free -- into WbEngine's per-op retire lambda,
// which is the io thread's hottest code; the lambda went 186 -> 263 instructions and the split GET
// cell paid for all of it while using none of it.
__attribute__((noinline))
inline void op_materialise_code(Op& op) {
    if (op.reply_code_ == 0) return;
    char* p = op.reply.reserve(kReplyCodeMax);
    op.reply.advance(format_reply_code(p, op.reply_code_, op.reply_ival_));
    op.reply_code_ = 0;
}

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

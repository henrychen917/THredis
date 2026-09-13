// resp3.h -- reply-only RESP3 framing selected from Op::kResp3.
//
// Request parsing is deliberately absent: Redis accepts the same RESP2 request grammar after
// HELLO 3. Core commands do not emit attributes; CLIENT TRACKING invalidations use push frames.
#pragma once

namespace tomo {

template <typename Buf> inline void reply_null(Buf&& b, bool resp3) {
    if (resp3) b.append("_\r\n", 3);
    else reply_nil(b);
}

template <typename Buf> inline void reply_null_array(Buf&& b, bool resp3) {
    if (resp3) b.append("_\r\n", 3);
    else reply_null_array(b);
}

template <typename Buf> inline void reply_bool(Buf&& b, bool value, bool resp3) {
    if (!resp3) { reply_int(b, value ? 1 : 0); return; }
    b.append(value ? "#t\r\n" : "#f\r\n", 4);
}

template <typename Buf> inline void reply_map_header(Buf&& b, uint64_t n, bool resp3) {
    if (!resp3) { reply_array_header(b, n * 2); return; }
    char* p = b.reserve(24);
    char* q = p;
    *q++ = '%';
    q += u64_to_dec(q, n);
    *q++ = '\r'; *q++ = '\n';
    b.advance(static_cast<size_t>(q - p));
}

template <typename Buf> inline void reply_set_header(Buf&& b, uint64_t n, bool resp3) {
    if (!resp3) { reply_array_header(b, n); return; }
    char* p = b.reserve(24);
    char* q = p;
    *q++ = '~';
    q += u64_to_dec(q, n);
    *q++ = '\r'; *q++ = '\n';
    b.advance(static_cast<size_t>(q - p));
}

template <typename Buf> inline void reply_push_header(Buf&& b, uint64_t n) {
    char* p = b.reserve(24);
    char* q = p;
    *q++ = '>';
    q += u64_to_dec(q, n);
    *q++ = '\r'; *q++ = '\n';
    b.advance(static_cast<size_t>(q - p));
}

// Redis renders the RESP3 "," double from the SAME d2string as the RESP2 bulk score, so this is
// one formatter, not two (see redis_double_text in src/base/numeric.h).
inline uint32_t resp_double_text(char* text, size_t cap, double value) {
    return redis_double_text(text, cap, value);
}

template <typename Buf> inline void reply_double(Buf&& b, double value, bool resp3) {
    if (!resp3) { reply_double(b, value); return; }
    char text[kDoubleTextMax];
    const uint32_t length = resp_double_text(text, sizeof(text), value);
    char* p = b.reserve(static_cast<size_t>(length) + 3);
    *p = ',';
    std::memcpy(p + 1, text, length);
    p[length + 1] = '\r'; p[length + 2] = '\n';
    b.advance(static_cast<size_t>(length) + 3);
}

template <typename Buf> inline void reply_bignum(Buf&& b, Slice value, bool resp3) {
    if (!resp3) { reply_bulk(b, value); return; }
    b.push_back('(');
    b.append(value.p, value.n);
    b.append("\r\n", 2);
}

template <typename Buf> inline void reply_verbatim(Buf&& b, Slice value, const char ext[3],
                                                   bool resp3) {
    if (!resp3) { reply_bulk(b, value); return; }
    const uint64_t framed = static_cast<uint64_t>(value.n) + 4;
    char* p = b.reserve(24 + static_cast<size_t>(framed) + 2);
    char* q = p;
    *q++ = '=';
    q += u64_to_dec(q, framed);
    *q++ = '\r'; *q++ = '\n';
    std::memcpy(q, ext, 3); q += 3;
    *q++ = ':';
    std::memcpy(q, value.p, value.n); q += value.n;
    *q++ = '\r'; *q++ = '\n';
    b.advance(static_cast<size_t>(q - p));
}

}  // namespace tomo

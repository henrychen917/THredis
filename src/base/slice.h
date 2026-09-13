// slice.h — the two byte types everything else is built from.
//
// Slice  : a non-owning (ptr, len) view. Parsing produces Slices INTO the connection's read buffer,
//          which is what makes command parsing zero-copy. The cost of that choice is a lifetime
//          rule, stated here because it is the one that bites: a Slice is only valid while the
//          buffer it points into is pinned. See Rob/Conn — the read buffer stays pinned until every
//          in-flight Op referencing it has retired.
// Buf    : an owned, growable byte buffer used for replies and for anything that must outlive the
//          read buffer.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string_view>

namespace tomo {

struct Slice {
    const char* p = nullptr;
    uint32_t    n = 0;

    Slice() = default;
    Slice(const char* p_, uint32_t n_) : p(p_), n(n_) {}
    explicit Slice(std::string_view s) : p(s.data()), n(static_cast<uint32_t>(s.size())) {}

    bool empty() const { return n == 0; }
    const char* begin() const { return p; }
    const char* end() const { return p + n; }
    std::string_view sv() const { return {p, n}; }

    bool operator==(const Slice& o) const {
        return n == o.n && (n == 0 || std::memcmp(p, o.p, n) == 0);
    }

    // Case-insensitive compare against a literal — command names arrive in any case.
    bool eq_icase(std::string_view lit) const {
        if (n != lit.size()) return false;
        for (uint32_t i = 0; i < n; i++) {
            char a = p[i], b = lit[i];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (a != b) return false;
        }
        return true;
    }
};

// Owned growable buffer. Deliberately not std::vector<char>: replies are written by appending small
// pieces in a hot loop, and we want an inline capacity so the common small reply never allocates.
template <size_t Inline>
class SmallBuf {
public:
    SmallBuf() : data_(inline_), cap_(Inline) {}
    ~SmallBuf() { if (data_ != inline_) std::free(data_); }
    SmallBuf(const SmallBuf&) = delete;
    SmallBuf& operator=(const SmallBuf&) = delete;

    char*  data()       { return data_; }
    const char* data() const { return data_; }
    size_t size() const { return len_; }
    bool   empty() const { return len_ == 0; }
    void   clear() { len_ = 0; }          // keeps the allocation for reuse
    size_t cap() const { return cap_; }

    // Publish n bytes that a WORKER already wrote past len_ (the direct-reply path). Only the
    // owning sender calls this, at in-order retire; the writer honored cap(), so no grow here.
    void commit_raw(size_t n) { len_ += n; }

    // Drops any heap growth and returns to the inline buffer. Called when a slot is recycled after
    // an unusually large reply, so one big reply does not permanently inflate every ring slot.
    void shrink_to_inline() {
        if (data_ != inline_) { std::free(data_); data_ = inline_; cap_ = Inline; }
        len_ = 0;
    }

    void append(const char* src, size_t n) {
        if (len_ + n > cap_) grow(len_ + n);
        std::memcpy(data_ + len_, src, n);
        len_ += n;
    }
    void append(std::string_view s) { append(s.data(), s.size()); }
    void append(Slice s)            { append(s.p, s.n); }
    void push_back(char c) {
        if (len_ + 1 > cap_) grow(len_ + 1);
        data_[len_++] = c;
    }

    // Reserve n writable bytes and return the cursor; caller commits with advance(). Lets a RESP
    // integer be formatted straight into the buffer instead of via a temporary.
    char* reserve(size_t n) {
        if (len_ + n > cap_) grow(len_ + n);
        return data_ + len_;
    }
    void advance(size_t n) { len_ += n; }

private:
    void grow(size_t need) {
        size_t ncap = cap_ * 2;
        while (ncap < need) ncap *= 2;
        char* nd = static_cast<char*>(std::malloc(ncap));
        std::memcpy(nd, data_, len_);
        if (data_ != inline_) std::free(data_);
        data_ = nd;
        cap_  = ncap;
    }
    char*  data_;
    size_t len_ = 0;
    size_t cap_;
    char   inline_[Inline];
};

}  // namespace tomo

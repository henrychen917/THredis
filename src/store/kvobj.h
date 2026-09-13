// kvobj.h — one key-value pair in ONE allocation.
//
// Layout, contiguous, in this order:
//
//   [ hdr 8B ][ klen_ext u32? ][ expire_at i64? ][ key bytes ][ value: bytes | ptr | i64 | compact ]
//
// Three rules drive this, all of them measured on the fork rather than assumed:
//
//  1. ALLOCATION COUNT is the lever (a bespoke size-class pool was built and deleted; the measured
//     win was fewer allocations, not cleverer ones). So key and value live in the same block as the
//     header, and small values never allocate separately. Embedding was worth +27% on SET.
//  2. NO REFCOUNT. Redis's robj refcount is a cross-thread hazard in a sharded server and its
//     traffic is a shared-line write. A KvObj is owned outright by one shard: copy or move, never
//     share. There is no shared-integer cache either.
//  3. NOTHING STORED THAT THE TABLE ALREADY HAS. FlatStore's slot carries a 15-bit tag, so no hash
//     is kept here. Optional LRU/LFU state steals proven-spare header bits; it adds no field.
//
// BUDGET (this is a test, not an aspiration — see bench/kvobj_footprint):
//   16-byte key + 64-byte value, no TTL  ->  target < 85 B all-in.
//   Today: 8 + 16 + 64 = 88 B before the allocator's size class. Getting under the bar needs the
//   varint header noted at TODO(density); doing it before the data path works would be premature.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <new>
#include "../base/slice.h"
#include "../base/alloc.h"
#include "../net/resp.h"
#include "typeval.h"

namespace tomo {

enum class Type : uint8_t {
    String = 0, Hash = 1, List = 2, Set = 3, Zset = 4, Stream = 5,
};

enum class Enc : uint8_t {
    Raw    = 0,   // value bytes inline in this block
    Int    = 1,   // value IS an int64 held in the block; no bytes, no allocation
    Extern = 2,   // value lives in a separate block; the slot holds a void*
    Compact = 3,  // collection metadata and packed bytes live in the KvObj tail
};

struct KvObjFlags {
    static constexpr uint8_t HasTtl = 1u << 0;
    static constexpr uint8_t KeyExt = 1u << 1;   // klen did not fit in 8 bits; u32 follows the hdr
    // Re-headering a collection moves this ownership bit to the replacement before FlatStore
    // retires the old header. Both headers briefly name the pointer; exactly one may destroy it.
    static constexpr uint8_t OwnsExtern = 1u << 2;
    // Bits 3..7 are the optional five-bit eviction metadata. They are never written while
    // maxmemory is disabled. See KvObj::eviction_meta().
    static constexpr uint8_t LayoutMask = HasTtl | KeyExt | OwnsExtern;
};

// Header is exactly 8 bytes. Fields are ordered so the hot ones (type/enc/flags) share one word.
struct KvObj {
    uint8_t  type;      // Type
    uint8_t  enc;       // Enc
    uint8_t  flags;     // KvObjFlags
    uint8_t  klen8;     // key length when < 255; 255 means "see the u32 after the header"
    uint32_t vlen;      // inline value length, or external length when Enc::Extern

    uint8_t eviction_meta() const { return static_cast<uint8_t>(flags >> 3); }
    void set_eviction_meta(uint8_t meta) {
        flags = static_cast<uint8_t>((flags & KvObjFlags::LayoutMask) | ((meta & 0x1f) << 3));
    }

    // ---- layout arithmetic -------------------------------------------------------------------
    // Everything after the header is optional and positional, so all offsets are computed rather
    // than stored. Kept in one place: get these wrong and every accessor is wrong together.
    uint32_t klen_ext_bytes() const { return (flags & KvObjFlags::KeyExt) ? 4u : 0u; }
    uint32_t ttl_bytes()      const { return (flags & KvObjFlags::HasTtl) ? 8u : 0u; }

    char*       tail()       { return reinterpret_cast<char*>(this) + sizeof(KvObj); }
    const char* tail() const { return reinterpret_cast<const char*>(this) + sizeof(KvObj); }

    uint32_t klen() const {
        if (flags & KvObjFlags::KeyExt) {
            uint32_t k;
            std::memcpy(&k, tail(), 4);
            return k;
        }
        return klen8;
    }

    char*       key_ptr()       { return tail() + klen_ext_bytes() + ttl_bytes(); }
    const char* key_ptr() const { return tail() + klen_ext_bytes() + ttl_bytes(); }
    Slice       key()     const { return Slice(key_ptr(), klen()); }

    char*       val_ptr()       { return key_ptr() + klen(); }
    const char* val_ptr() const { return key_ptr() + klen(); }

    int64_t expire_at_ms() const {
        if (!(flags & KvObjFlags::HasTtl)) return -1;
        int64_t t;
        std::memcpy(&t, tail() + klen_ext_bytes(), 8);
        return t;
    }
    void set_expire_at_ms(int64_t t) {
        // Only valid when the object was built with HasTtl — the slot must already exist.
        std::memcpy(tail() + klen_ext_bytes(), &t, 8);
    }

    // ---- value access ------------------------------------------------------------------------
    Slice str_value() const {
        if (static_cast<Enc>(enc) == Enc::Extern) {
            void* p;
            std::memcpy(&p, val_ptr(), sizeof(void*));
            return Slice(static_cast<const char*>(p), vlen);
        }
        return Slice(val_ptr(), vlen);   // Enc::Raw
    }
    int64_t int_value() const {
        int64_t v;
        std::memcpy(&v, val_ptr(), 8);
        return v;
    }
    void set_int_value(int64_t v) { std::memcpy(val_ptr(), &v, 8); }

    void* external_ptr() const {
        void* p;
        std::memcpy(&p, val_ptr(), sizeof(void*));
        return p;
    }
    void set_external_ptr(void* p) { std::memcpy(val_ptr(), &p, sizeof(void*)); }

    bool is_int() const { return static_cast<Enc>(enc) == Enc::Int; }
    bool is_type(Type t) const { return static_cast<Type>(type) == t; }
};

static_assert(sizeof(KvObj) == 8, "KvObj header must stay 8 bytes");

inline constexpr uint32_t kEmbedThreshold = 192;
inline size_t kvobj_alloc_size(uint32_t klen, uint32_t vlen, bool has_ttl, Enc enc);

// Small collections follow this architecture's string-inline precedent while retaining Compact's
// byte format. Redis/Valkey's one-listpack small form and Dragonfly's packed outer object establish
// the allocation target; the tail embedding is native to KvObj's single-owner block. This fixed
// metadata header is followed immediately by encoded entries. It deliberately contains no C++
// owner, vector, callback, or expanded-representation shell, so hash/list/set/zset all have a
// one-allocation resident form. aux0/aux1 are lane-owned scalar state (hash logical bytes + PRNG,
// or set small-encoding metadata); list and zset leave them zero.
struct EmbeddedCompact {
    // val_ptr() follows a variable-length key and is not naturally aligned. Keep the metadata as
    // bytes and use memcpy loads/stores, just like KvObj's TTL and external pointer accessors.
    // This preserves the dense layout without relying on x86's permissive unaligned accesses.
    uint8_t bytes[32];

    uint32_t entries() const { return load<uint32_t>(0); }
    uint32_t encoded_bytes() const { return load<uint32_t>(4); }
    uint64_t payload_bytes() const { return load<uint64_t>(8); }
    uint64_t aux0() const { return load<uint64_t>(16); }
    uint64_t aux1() const { return load<uint64_t>(24); }

    void set_entries(uint32_t value) { store(0, value); }
    void set_encoded_bytes(uint32_t value) { store(4, value); }
    void set_payload_bytes(uint64_t value) { store(8, value); }
    void set_aux0(uint64_t value) { store(16, value); }
    void set_aux1(uint64_t value) { store(24, value); }

    uint8_t* data() { return bytes + sizeof(bytes); }
    const uint8_t* data() const { return bytes + sizeof(bytes); }

private:
    template <typename T>
    T load(uint32_t offset) const {
        T value;
        std::memcpy(&value, bytes + offset, sizeof(value));
        return value;
    }
    template <typename T>
    void store(uint32_t offset, T value) {
        std::memcpy(bytes + offset, &value, sizeof(value));
    }
};
static_assert(sizeof(EmbeddedCompact) == 32, "embedded collection metadata must stay compact");

// Resident packed bytes above this size move one-way to the ordinary external wrapper. The limit
// matches the proven string embed threshold; allocator-class slack below it is usable in place.
inline constexpr uint32_t kCollectionEmbedMax = kEmbedThreshold;

inline EmbeddedCompact* embedded_compact(KvObj* o) {
    return reinterpret_cast<EmbeddedCompact*>(o->val_ptr());
}
inline const EmbeddedCompact* embedded_compact(const KvObj* o) {
    return reinterpret_cast<const EmbeddedCompact*>(o->val_ptr());
}

inline uint32_t embedded_compact_capacity(const KvObj* o) {
    const size_t prefix = static_cast<size_t>(o->val_ptr() - reinterpret_cast<const char*>(o)) +
                          sizeof(EmbeddedCompact);
    const size_t allocation = good_size(kvobj_alloc_size(
        o->klen(), o->vlen, (o->flags & KvObjFlags::HasTtl) != 0, Enc::Compact));
    const size_t slack = allocation > prefix ? allocation - prefix : 0;
    return static_cast<uint32_t>(std::min<size_t>(slack, kCollectionEmbedMax));
}

// A non-owning facade over either the current external Compact or embedded bytes. Entry.offset
// remains absolute for external gap buffers and zero-based for embedded buffers; logical() is the
// only bridge, exactly as in Compact. Embedded small-N access intentionally rescans linearly and
// never materializes a side index.
class CompactView {
public:
    using Entry = Compact::Entry;

    class Iterator {
    public:
        Iterator() = default;
        Entry operator*() const {
            Entry entry;
            if (external_) external_->at_known_offset(logical_, index_, entry);
            else CompactView(embedded_, capacity_).decode_inline(logical_, index_, entry);
            return entry;
        }
        Iterator& operator++() {
            Entry entry;
            if (external_) external_->at_known_offset(logical_, index_, entry);
            else CompactView(embedded_, capacity_).decode_inline(logical_, index_, entry);
            logical_ += entry.span;
            index_++;
            return *this;
        }
        bool operator==(const Iterator& rhs) const {
            return external_ == rhs.external_ && embedded_ == rhs.embedded_ &&
                   index_ == rhs.index_;
        }
        bool operator!=(const Iterator& rhs) const { return !(*this == rhs); }

    private:
        friend class CompactView;
        Iterator(Compact* external, EmbeddedCompact* embedded, uint32_t capacity, uint32_t index,
                 uint32_t logical)
            : external_(external), embedded_(embedded), capacity_(capacity), index_(index),
              logical_(logical) {}
        Compact* external_ = nullptr;
        EmbeddedCompact* embedded_ = nullptr;
        uint32_t capacity_ = 0;
        uint32_t index_ = 0;
        uint32_t logical_ = 0;
    };

    CompactView() = default;
    explicit CompactView(Compact* external) : external_(external) {}
    CompactView(EmbeddedCompact* embedded, uint32_t capacity)
        : embedded_(embedded), capacity_(capacity) {}

    uint32_t size() const { return external_ ? external_->size() : embedded_->entries(); }
    uint64_t payload_bytes() const {
        return external_ ? external_->payload_bytes() : embedded_->payload_bytes();
    }
    size_t encoded_bytes() const {
        return external_ ? external_->encoded_bytes() : embedded_->encoded_bytes();
    }
    size_t capacity_bytes() const { return external_ ? external_->capacity_bytes() : 0; }
    bool empty() const { return size() == 0; }
    const uint8_t* data() const { return external_ ? external_->data() : embedded_->data(); }
    uint32_t logical(const Entry& entry) const {
        return external_ ? external_->logical(entry) : entry.offset;
    }

    Iterator begin() const { return Iterator(external_, embedded_, capacity_, 0, 0); }
    Iterator end() const {
        return Iterator(external_, embedded_, capacity_, size(),
                        static_cast<uint32_t>(encoded_bytes()));
    }

    bool at(uint32_t index, Entry& out) const {
        if (external_) return external_->at(index, out);
        if (index >= embedded_->entries()) return false;
        uint32_t offset = 0;
        for (uint32_t i = 0; i <= index; i++) {
            if (!decode_inline(offset, i, out)) return false;
            if (i != index) offset += out.span;
        }
        return true;
    }
    bool first(Entry& out) const { return at(0, out); }
    bool last(Entry& out) const { return size() && at(size() - 1, out); }
    bool next(const Entry& current, Entry& out) const {
        if (external_) return external_->next(current, out);
        if (current.index + 1 >= embedded_->entries()) return false;
        Entry checked;
        return decode_inline(current.offset, current.index, checked) &&
               checked.span == current.span &&
               decode_inline(current.offset + current.span, current.index + 1, out);
    }
    bool at_offset(uint32_t logical_offset, Entry& out) const {
        if (external_) return external_->at_offset(logical_offset, out);
        uint32_t offset = 0;
        for (uint32_t index = 0; index < embedded_->entries(); index++) {
            if (offset == logical_offset) return decode_inline(offset, index, out);
            Entry entry;
            if (!decode_inline(offset, index, entry) || offset > logical_offset) return false;
            offset += entry.span;
        }
        return false;
    }

    bool append(Slice value) { return insert(static_cast<uint32_t>(encoded_bytes()), value); }
    bool prepend(Slice value) { return insert(0, value); }

    bool insert(uint32_t logical_offset, Slice value) {
        if (external_) return external_->insert(logical_offset, value);
        const uint32_t old_entries = embedded_->entries();
        const uint32_t old_encoded = embedded_->encoded_bytes();
        if (old_entries == std::numeric_limits<uint32_t>::max()) return false;
        if (logical_offset > old_encoded) return false;
        if (logical_offset != old_encoded) {
            Entry boundary;
            if (!at_offset(logical_offset, boundary)) return false;
        }
        const uint32_t header = varint_size(value.n);
        const uint64_t next = static_cast<uint64_t>(old_encoded) + header + value.n;
        if (next > capacity_) return false;
        const uint32_t span = header + value.n;
        uint8_t* bytes = embedded_->data();
        std::memmove(bytes + logical_offset + span, bytes + logical_offset,
                     old_encoded - logical_offset);
        encode_varint(bytes + logical_offset, value.n);
        if (value.n) std::memcpy(bytes + logical_offset + header, value.p, value.n);
        embedded_->set_encoded_bytes(old_encoded + span);
        embedded_->set_entries(old_entries + 1);
        embedded_->set_payload_bytes(embedded_->payload_bytes() + value.n);
        return true;
    }

    bool replace(const Entry& old_entry, Slice value) {
        if (external_) return external_->replace(old_entry, value);
        Entry checked;
        if (!at(old_entry.index, checked) || checked.offset != old_entry.offset ||
            checked.span != old_entry.span) return false;
        const uint32_t new_span = varint_size(value.n) + value.n;
        const uint32_t old_encoded = embedded_->encoded_bytes();
        const uint64_t old_payload = embedded_->payload_bytes();
        const uint64_t next = static_cast<uint64_t>(old_encoded) - checked.span +
                              new_span;
        if (next > capacity_) return false;
        uint8_t* bytes = embedded_->data();
        const uint32_t tail = old_encoded - checked.offset - checked.span;
        std::memmove(bytes + checked.offset + new_span, bytes + checked.offset + checked.span, tail);
        encode_varint(bytes + checked.offset, value.n);
        const uint32_t header = varint_size(value.n);
        if (value.n) std::memcpy(bytes + checked.offset + header, value.p, value.n);
        embedded_->set_encoded_bytes(static_cast<uint32_t>(next));
        embedded_->set_payload_bytes(old_payload - checked.value.n + value.n);
        return true;
    }

    bool insert_before(const Entry& entry, Slice value) {
        Entry checked;
        if (!at(entry.index, checked) || checked.offset != entry.offset || checked.span != entry.span)
            return false;
        return insert(logical(checked), value);
    }
    bool insert_after(const Entry& entry, Slice value) {
        Entry checked;
        if (!at(entry.index, checked) || checked.offset != entry.offset || checked.span != entry.span)
            return false;
        return insert(logical(checked) + checked.span, value);
    }

    bool erase(const Entry& entry) {
        if (external_) return external_->erase(entry);
        Entry checked;
        if (!at(entry.index, checked) || checked.offset != entry.offset || checked.span != entry.span)
            return false;
        uint8_t* bytes = embedded_->data();
        const uint32_t old_encoded = embedded_->encoded_bytes();
        const uint32_t old_entries = embedded_->entries();
        const uint64_t old_payload = embedded_->payload_bytes();
        const uint32_t tail = old_encoded - checked.offset - checked.span;
        std::memmove(bytes + checked.offset, bytes + checked.offset + checked.span, tail);
        embedded_->set_encoded_bytes(old_encoded - checked.span);
        embedded_->set_entries(old_entries - 1);
        embedded_->set_payload_bytes(old_payload - checked.value.n);
        return true;
    }
    bool pop_front(uint32_t* payload_size = nullptr) {
        if (external_) return external_->pop_front(payload_size);
        Entry entry;
        if (!first(entry)) return false;
        if (payload_size) *payload_size = entry.value.n;
        return erase(entry);
    }
    bool pop_back(uint32_t* payload_size = nullptr) {
        if (external_) return external_->pop_back(payload_size);
        Entry entry;
        if (!last(entry)) return false;
        if (payload_size) *payload_size = entry.value.n;
        return erase(entry);
    }

    bool erase_range(uint32_t first_logical, uint32_t last_logical) {
        if (external_) return external_->erase_range(first_logical, last_logical);
        const uint32_t old_encoded = embedded_->encoded_bytes();
        if (first_logical > last_logical || last_logical > old_encoded) return false;
        if (first_logical == last_logical) return true;
        Entry first;
        if (!at_offset(first_logical, first)) return false;
        uint32_t offset = first_logical;
        uint32_t removed = 0;
        uint64_t payload = 0;
        while (offset < last_logical) {
            Entry entry;
            if (!at_offset(offset, entry) || offset + entry.span > last_logical) return false;
            offset += entry.span;
            removed++;
            payload += entry.value.n;
        }
        if (offset != last_logical) return false;
        std::memmove(embedded_->data() + first_logical, embedded_->data() + last_logical,
                     old_encoded - last_logical);
        embedded_->set_encoded_bytes(old_encoded - (last_logical - first_logical));
        embedded_->set_entries(embedded_->entries() - removed);
        embedded_->set_payload_bytes(embedded_->payload_bytes() - payload);
        return true;
    }

private:
    static uint32_t varint_size(uint32_t value) {
        uint32_t size = 1;
        while (value >= 0x80) { value >>= 7; size++; }
        return size;
    }
    static void encode_varint(uint8_t* destination, uint32_t value) {
        while (value >= 0x80) {
            *destination++ = static_cast<uint8_t>((value & 0x7f) | 0x80);
            value >>= 7;
        }
        *destination = static_cast<uint8_t>(value);
    }
    bool decode_inline(uint32_t offset, uint32_t index, Entry& out) const {
        const uint32_t encoded = embedded_->encoded_bytes();
        if (offset >= encoded) return false;
        uint32_t value = 0;
        uint32_t shift = 0;
        uint32_t position = offset;
        for (uint32_t count = 0; count < 5 && position < encoded;
             count++, position++) {
            const uint8_t byte = embedded_->data()[position];
            if (shift == 28 && (byte & 0xf0)) return false;
            value |= static_cast<uint32_t>(byte & 0x7f) << shift;
            if (!(byte & 0x80)) {
                const uint32_t header = position - offset + 1;
                if (static_cast<uint64_t>(offset) + header + value > encoded)
                    return false;
                out = Entry{Slice(reinterpret_cast<const char*>(embedded_->data() + offset + header),
                                  value), offset, header + value, index};
                return true;
            }
            shift += 7;
        }
        return false;
    }

    Compact* external_ = nullptr;
    EmbeddedCompact* embedded_ = nullptr;
    uint32_t capacity_ = 0;
};

class CollectionRef {
public:
    explicit CollectionRef(KvObj* object) : object_(object) {
        if (static_cast<Enc>(object->enc) == Enc::Compact)
            embedded_ = embedded_compact(object);
        else
            external_ = static_cast<CompactValue*>(object->external_ptr());
    }
    explicit CollectionRef(CompactValue* external) : external_(external) {}

    bool is_embedded() const { return embedded_ != nullptr; }
    CollectionEncoding encoding() const {
        return embedded_ ? CollectionEncoding::Compact : external_->encoding();
    }
    uint32_t entries() const { return embedded_ ? embedded_->entries() : external_->entries(); }
    uint64_t payload_bytes() const {
        return embedded_ ? embedded_->payload_bytes() : external_->payload_bytes();
    }
    CompactView compact() const {
        return embedded_ ? CompactView(embedded_, embedded_compact_capacity(object_))
                         : CompactView(&external_->mutable_compact());
    }
    CompactView::Iterator begin() const { return compact().begin(); }
    CompactView::Iterator end() const { return compact().end(); }

    bool append(Slice value) { return encoding() == CollectionEncoding::Compact && compact().append(value); }
    bool prepend(Slice value) { return encoding() == CollectionEncoding::Compact && compact().prepend(value); }
    bool insert(uint32_t logical, Slice value) {
        return encoding() == CollectionEncoding::Compact && compact().insert(logical, value);
    }
    bool replace(const Compact::Entry& entry, Slice value) {
        return encoding() == CollectionEncoding::Compact && compact().replace(entry, value);
    }
    bool insert_before(const Compact::Entry& entry, Slice value) {
        return encoding() == CollectionEncoding::Compact && compact().insert_before(entry, value);
    }
    bool insert_after(const Compact::Entry& entry, Slice value) {
        return encoding() == CollectionEncoding::Compact && compact().insert_after(entry, value);
    }
    bool erase(const Compact::Entry& entry) {
        return encoding() == CollectionEncoding::Compact && compact().erase(entry);
    }
    bool pop_front(uint32_t* payload = nullptr) {
        return encoding() == CollectionEncoding::Compact && compact().pop_front(payload);
    }
    bool pop_back(uint32_t* payload = nullptr) {
        return encoding() == CollectionEncoding::Compact && compact().pop_back(payload);
    }
    bool erase_range(uint32_t first, uint32_t last) {
        return encoding() == CollectionEncoding::Compact && compact().erase_range(first, last);
    }
    bool replace_compact(Compact&& replacement) {
        if (encoding() != CollectionEncoding::Compact) return false;
        if (!embedded_) return external_->replace_compact(std::move(replacement));
        if (replacement.encoded_bytes() > embedded_compact_capacity(object_)) return false;
        embedded_->set_entries(replacement.size());
        embedded_->set_encoded_bytes(static_cast<uint32_t>(replacement.encoded_bytes()));
        embedded_->set_payload_bytes(replacement.payload_bytes());
        if (embedded_->encoded_bytes())
            std::memcpy(embedded_->data(), replacement.data(), embedded_->encoded_bytes());
        return true;
    }

    bool compact_fits(const CompactLimit& limit, uint32_t resulting_entries,
                      uint32_t incoming_max) const {
        return encoding() == CollectionEncoding::Compact &&
               resulting_entries <= limit.max_entries && incoming_max <= limit.max_value;
    }
    bool list_fits(const CompactLimit& limit, uint32_t resulting_entries,
                   uint64_t resulting_payload) const {
        return encoding() == CollectionEncoding::Compact &&
               resulting_entries <= limit.max_entries && resulting_payload <= limit.max_value;
    }
    bool embedded_bytes_fit(uint64_t resulting_encoded) const {
        return !embedded_ || resulting_encoded <= embedded_compact_capacity(object_);
    }

    template <typename T>
    T* external_as() const { return static_cast<T*>(external_); }
    uint64_t aux0() const { return embedded_->aux0(); }
    uint64_t aux1() const { return embedded_->aux1(); }
    void set_aux0(uint64_t value) { embedded_->set_aux0(value); }
    void set_aux1(uint64_t value) { embedded_->set_aux1(value); }
    KvObj* object() const { return object_; }

private:
    KvObj* object_ = nullptr;
    CompactValue* external_ = nullptr;
    EmbeddedCompact* embedded_ = nullptr;
};

// ---- construction ----------------------------------------------------------------------------
// Values at or below this live in the same block as the key. 192 was validated on the fork, but
// against Redis's allocation shape rather than this one, so it is a starting point to re-measure —
// it trades RSS against SET throughput.
inline size_t kvobj_alloc_size(uint32_t klen, uint32_t vlen, bool has_ttl, Enc enc) {
    size_t n = sizeof(KvObj);
    if (klen >= 255) n += 4;
    if (has_ttl)     n += 8;
    n += klen;
    switch (enc) {
        case Enc::Int:    n += 8; break;
        case Enc::Extern: n += sizeof(void*); break;
        case Enc::Compact:n += vlen; break;
        case Enc::Raw:    n += vlen; break;
    }
    return n;
}

// Builds a String KvObj. `val` is copied when it fits the embed threshold, otherwise a second block
// holds it and this one keeps the pointer. Returns nullptr on OOM rather than throwing: the worker
// loop reports an error reply instead of unwinding.
inline KvObj* kvobj_init_raw_string(void* mem, Slice key, Slice val,
                                    int64_t expire_at_ms = -1) {
    if (!mem || val.n > kEmbedThreshold) return nullptr;
    const bool has_ttl = expire_at_ms >= 0;
    auto* o = static_cast<KvObj*>(mem);
    o->type = static_cast<uint8_t>(Type::String);
    o->enc = static_cast<uint8_t>(Enc::Raw);
    o->flags = static_cast<uint8_t>((has_ttl ? KvObjFlags::HasTtl : 0) |
                                    (key.n >= 255 ? KvObjFlags::KeyExt : 0));
    o->klen8 = static_cast<uint8_t>(key.n >= 255 ? 255 : key.n);
    o->vlen = val.n;
    if (key.n >= 255) { uint32_t k = key.n; std::memcpy(o->tail(), &k, 4); }
    if (has_ttl) o->set_expire_at_ms(expire_at_ms);
    if (key.n) std::memcpy(o->key_ptr(), key.p, key.n);
    if (val.n) std::memcpy(o->val_ptr(), val.p, val.n);
    return o;
}

inline KvObj* kvobj_init_int(void* mem, Slice key, int64_t value,
                             int64_t expire_at_ms = -1) {
    if (!mem) return nullptr;
    const bool has_ttl = expire_at_ms >= 0;
    auto* o = static_cast<KvObj*>(mem);
    o->type = static_cast<uint8_t>(Type::String);
    o->enc = static_cast<uint8_t>(Enc::Int);
    o->flags = static_cast<uint8_t>((has_ttl ? KvObjFlags::HasTtl : 0) |
                                    (key.n >= 255 ? KvObjFlags::KeyExt : 0));
    o->klen8 = static_cast<uint8_t>(key.n >= 255 ? 255 : key.n);
    o->vlen = 0;
    if (key.n >= 255) { uint32_t k = key.n; std::memcpy(o->tail(), &k, 4); }
    if (has_ttl) o->set_expire_at_ms(expire_at_ms);
    if (key.n) std::memcpy(o->key_ptr(), key.p, key.n);
    o->set_int_value(value);
    return o;
}

inline KvObj* kvobj_new_string(Slice key, Slice val, int64_t expire_at_ms = -1) {
    const bool  has_ttl = expire_at_ms >= 0;
    const Enc   enc     = (val.n <= kEmbedThreshold) ? Enc::Raw : Enc::Extern;
    // Request the CLASS-ROUNDED size explicitly. try_overwrite writes up to good_size(request),
    // which is only within the allocation if the allocation asked for it: on jemalloc the class
    // rounds up anyway (zero cost), on an exact allocator (ASAN, glibc) requesting the raw size
    // made that write a heap overflow -- a 3-byte corruption the gate's RYOW-under-ASAN caught.
    const size_t n      = good_size(kvobj_alloc_size(key.n, val.n, has_ttl, enc));

    void* mem = alloc_raw(n);
    if (!mem) return nullptr;

    auto* o = static_cast<KvObj*>(mem);
    if (enc == Enc::Raw) {
        return kvobj_init_raw_string(mem, key, val, expire_at_ms);
    } else {
        o->type  = static_cast<uint8_t>(Type::String);
        o->enc   = static_cast<uint8_t>(enc);
        o->flags = static_cast<uint8_t>((has_ttl ? KvObjFlags::HasTtl : 0) |
                                        (key.n >= 255 ? KvObjFlags::KeyExt : 0) |
                                        KvObjFlags::OwnsExtern);
        o->klen8 = static_cast<uint8_t>(key.n >= 255 ? 255 : key.n);
        o->vlen  = val.n;
        if (key.n >= 255) { uint32_t k = key.n; std::memcpy(o->tail(), &k, 4); }
        if (has_ttl) o->set_expire_at_ms(expire_at_ms);
        if (key.n) std::memcpy(o->key_ptr(), key.p, key.n);
        void* ext = alloc_raw(good_size(val.n));   // same contract as the main block
        if (!ext) { free_sized(mem, n); return nullptr; }
        std::memcpy(ext, val.p, val.n);
        std::memcpy(o->val_ptr(), &ext, sizeof(void*));
    }
    return o;
}

inline KvObj* kvobj_new_int(Slice key, int64_t value, int64_t expire_at_ms = -1) {
    const bool has_ttl = expire_at_ms >= 0;
    const size_t n = good_size(kvobj_alloc_size(key.n, 0, has_ttl, Enc::Int));
    void* mem = alloc_raw(n);
    if (!mem) return nullptr;

    return kvobj_init_int(mem, key, value, expire_at_ms);
}

inline KvObj* kvobj_new_typeval(Slice key, Type type, void* value, uint32_t value_size,
                                int64_t expire_at_ms = -1, bool owns = true) {
    if (type == Type::String || !value) return nullptr;
    const bool has_ttl = expire_at_ms >= 0;
    const size_t n = good_size(kvobj_alloc_size(key.n, value_size, has_ttl, Enc::Extern));
    void* mem = alloc_raw(n);
    if (!mem) return nullptr;

    auto* o = static_cast<KvObj*>(mem);
    o->type = static_cast<uint8_t>(type);
    o->enc = static_cast<uint8_t>(Enc::Extern);
    o->flags = static_cast<uint8_t>((has_ttl ? KvObjFlags::HasTtl : 0) |
                                    (key.n >= 255 ? KvObjFlags::KeyExt : 0) |
                                    (owns ? KvObjFlags::OwnsExtern : 0));
    o->klen8 = static_cast<uint8_t>(key.n >= 255 ? 255 : key.n);
    o->vlen = value_size;
    if (key.n >= 255) { uint32_t k = key.n; std::memcpy(o->tail(), &k, 4); }
    if (has_ttl) o->set_expire_at_ms(expire_at_ms);
    std::memcpy(o->key_ptr(), key.p, key.n);
    o->set_external_ptr(value);
    return o;
}

inline KvObj* kvobj_new_embedded_typeval(Slice key, Type type, const Compact& compact,
                                         uint64_t aux0 = 0, uint64_t aux1 = 0,
                                         int64_t expire_at_ms = -1) {
    if (type == Type::String || compact.encoded_bytes() > kCollectionEmbedMax) return nullptr;
    const bool has_ttl = expire_at_ms >= 0;
    const uint32_t tail_bytes = static_cast<uint32_t>(sizeof(EmbeddedCompact) +
                                                       compact.encoded_bytes());
    const size_t n = good_size(kvobj_alloc_size(key.n, tail_bytes, has_ttl, Enc::Compact));
    void* memory = alloc_raw(n);
    if (!memory) return nullptr;

    auto* object = static_cast<KvObj*>(memory);
    object->type = static_cast<uint8_t>(type);
    object->enc = static_cast<uint8_t>(Enc::Compact);
    object->flags = static_cast<uint8_t>((has_ttl ? KvObjFlags::HasTtl : 0) |
                                         (key.n >= 255 ? KvObjFlags::KeyExt : 0));
    object->klen8 = static_cast<uint8_t>(key.n >= 255 ? 255 : key.n);
    object->vlen = tail_bytes;
    if (key.n >= 255) { uint32_t length = key.n; std::memcpy(object->tail(), &length, 4); }
    if (has_ttl) object->set_expire_at_ms(expire_at_ms);
    if (key.n) std::memcpy(object->key_ptr(), key.p, key.n);

    EmbeddedCompact* embedded = embedded_compact(object);
    embedded->set_entries(compact.size());
    embedded->set_encoded_bytes(static_cast<uint32_t>(compact.encoded_bytes()));
    embedded->set_payload_bytes(compact.payload_bytes());
    embedded->set_aux0(aux0);
    embedded->set_aux1(aux1);
    if (embedded->encoded_bytes())
        std::memcpy(embedded->data(), compact.data(), embedded->encoded_bytes());
    return object;
}

inline KvObj* kvobj_new_hash(Slice key, HashVal* value, int64_t expire_at_ms = -1) {
    return kvobj_new_typeval(key, Type::Hash, value, sizeof(*value), expire_at_ms);
}
inline KvObj* kvobj_new_list(Slice key, ListVal* value, int64_t expire_at_ms = -1) {
    return kvobj_new_typeval(key, Type::List, value, sizeof(*value), expire_at_ms);
}
inline KvObj* kvobj_new_set(Slice key, SetVal* value, int64_t expire_at_ms = -1) {
    return kvobj_new_typeval(key, Type::Set, value, sizeof(*value), expire_at_ms);
}
inline KvObj* kvobj_new_zset(Slice key, ZsetVal* value, int64_t expire_at_ms = -1) {
    return kvobj_new_typeval(key, Type::Zset, value, sizeof(*value), expire_at_ms);
}
inline KvObj* kvobj_new_stream(Slice key, StreamVal* value, int64_t expire_at_ms = -1) {
    return kvobj_new_typeval(key, Type::Stream, value, sizeof(*value), expire_at_ms);
}

// Adopt helpers select the one-allocation compact form. On success ownership is consumed and the
// caller's pointer is nulled; on failure it remains with the caller. This mirrors the old wrapper
// ownership contract while making the representation choice explicit at the only attach point.
inline KvObj* kvobj_adopt_hash(Slice key, HashVal*& value, int64_t expire_at_ms = -1) {
    KvObj* object = value->encoding() == CollectionEncoding::Compact &&
                            value->compact().encoded_bytes() <= kCollectionEmbedMax
        ? kvobj_new_embedded_typeval(key, Type::Hash, value->compact(),
                                    value->compact_payload_bytes, value->random_state, expire_at_ms)
        : kvobj_new_hash(key, value, expire_at_ms);
    if (!object) return nullptr;
    if (static_cast<Enc>(object->enc) == Enc::Compact) delete value;
    value = nullptr;
    return object;
}

inline KvObj* kvobj_adopt_list(Slice key, ListVal*& value, int64_t expire_at_ms = -1) {
    KvObj* object = value->encoding() == CollectionEncoding::Compact &&
                            value->compact().encoded_bytes() <= kCollectionEmbedMax
        ? kvobj_new_embedded_typeval(key, Type::List, value->compact(), 0, 0, expire_at_ms)
        : kvobj_new_list(key, value, expire_at_ms);
    if (!object) return nullptr;
    if (static_cast<Enc>(object->enc) == Enc::Compact) delete value;
    value = nullptr;
    return object;
}

inline uint64_t pack_embedded_set_metadata(const SetVal& value) {
    return static_cast<uint64_t>(value.max_member_bytes) |
           (static_cast<uint64_t>(value.int_width) << 32) |
           (static_cast<uint64_t>(value.small_encoding) << 40);
}

inline KvObj* kvobj_adopt_set(Slice key, SetVal*& value, int64_t expire_at_ms = -1) {
    KvObj* object = value->encoding() == CollectionEncoding::Compact &&
                            value->compact().encoded_bytes() <= kCollectionEmbedMax
        ? kvobj_new_embedded_typeval(key, Type::Set, value->compact(),
                                    pack_embedded_set_metadata(*value), 0, expire_at_ms)
        : kvobj_new_set(key, value, expire_at_ms);
    if (!object) return nullptr;
    if (static_cast<Enc>(object->enc) == Enc::Compact) delete value;
    value = nullptr;
    return object;
}

inline KvObj* kvobj_adopt_zset(Slice key, ZsetVal*& value, int64_t expire_at_ms = -1) {
    KvObj* object = value->encoding() == CollectionEncoding::Compact &&
                            value->compact().encoded_bytes() <= kCollectionEmbedMax
        ? kvobj_new_embedded_typeval(key, Type::Zset, value->compact(), 0, 0, expire_at_ms)
        : kvobj_new_zset(key, value, expire_at_ms);
    if (!object) return nullptr;
    if (static_cast<Enc>(object->enc) == Enc::Compact) delete value;
    value = nullptr;
    return object;
}

inline KvObj* kvobj_adopt_stream(Slice key, StreamVal*& value, int64_t expire_at_ms = -1) {
    KvObj* object = value->encoding() == CollectionEncoding::Compact &&
                            value->compact().encoded_bytes() <= kCollectionEmbedMax
        ? kvobj_new_embedded_typeval(key, Type::Stream, value->compact(),
                                    value->header.last_id.ms, value->header.last_id.seq,
                                    expire_at_ms)
        : kvobj_new_stream(key, value, expire_at_ms);
    if (!object) return nullptr;
    if (static_cast<Enc>(object->enc) == Enc::Compact) delete value;
    value = nullptr;
    return object;
}

// Creates the replacement header needed when HasTtl changes the positional layout. Strings are
// copied because their bytes may be borrowed by a zero-copy send. Collections are single-owner and
// move their external ownership in FlatStore::rewrite_expire().
inline KvObj* kvobj_reheader(KvObj* src, int64_t expire_at_ms) {
    const Type type = static_cast<Type>(src->type);
    KvObj* replacement = nullptr;
    if (type == Type::String) {
        replacement = src->is_int()
            ? kvobj_new_int(src->key(), src->int_value(), expire_at_ms)
            : kvobj_new_string(src->key(), src->str_value(), expire_at_ms);
    } else if (static_cast<Enc>(src->enc) == Enc::Compact) {
        const EmbeddedCompact* embedded = embedded_compact(src);
        const Slice key = src->key();
        const bool has_ttl = expire_at_ms >= 0;
        const uint32_t tail_bytes = static_cast<uint32_t>(sizeof(EmbeddedCompact)) +
                                    embedded->encoded_bytes();
        const size_t bytes = good_size(kvobj_alloc_size(key.n, tail_bytes, has_ttl, Enc::Compact));
        void* memory = alloc_raw(bytes);
        if (!memory) return nullptr;
        replacement = static_cast<KvObj*>(memory);
        replacement->type = src->type;
        replacement->enc = static_cast<uint8_t>(Enc::Compact);
        replacement->flags = static_cast<uint8_t>((has_ttl ? KvObjFlags::HasTtl : 0) |
                                                   (key.n >= 255 ? KvObjFlags::KeyExt : 0));
        replacement->klen8 = static_cast<uint8_t>(key.n >= 255 ? 255 : key.n);
        replacement->vlen = tail_bytes;
        if (key.n >= 255) {
            const uint32_t length = key.n;
            std::memcpy(replacement->tail(), &length, sizeof(length));
        }
        if (has_ttl) replacement->set_expire_at_ms(expire_at_ms);
        if (key.n) std::memcpy(replacement->key_ptr(), key.p, key.n);
        std::memcpy(embedded_compact(replacement), embedded, tail_bytes);
    } else {
        replacement = kvobj_new_typeval(src->key(), type, src->external_ptr(), src->vlen,
                                        expire_at_ms, false);
    }
    return replacement;
}

// What this object ASKED the allocator for. Used to free it (sized free) and as the basis for its
// capacity. Recomputed rather than stored: a size field would cost every key 4 bytes to save a
// multiply, and the computation is a pure function of the header.
inline size_t kvobj_request_size(const KvObj* o) {
    return kvobj_alloc_size(o->klen(), o->vlen, (o->flags & KvObjFlags::HasTtl) != 0,
                            static_cast<Enc>(o->enc));
}

// What the allocator actually handed back. The slack between request and class is already paid for,
// and exposing it is what lets a SET whose value grew by a few bytes still avoid allocating.
inline size_t kvobj_capacity(const KvObj* o) { return good_size(kvobj_request_size(o)); }

// Real footprint, for the store's resident estimate: the size CLASS, not the request, plus any
// external value block.
inline size_t kvobj_size(const KvObj* o) {
    size_t n = kvobj_capacity(o);
    if (static_cast<Enc>(o->enc) != Enc::Extern) return n;
    switch (static_cast<Type>(o->type)) {
        case Type::String: n += good_size(o->vlen); break;
        case Type::Hash:
            n += static_cast<HashVal*>(o->external_ptr())->allocation_bytes() +
                 good_size(sizeof(HashVal)) - sizeof(CompactValue);
            break;
        case Type::List:
            n += static_cast<ListVal*>(o->external_ptr())->allocation_bytes() +
                 good_size(sizeof(ListVal)) - sizeof(CompactValue);
            break;
        case Type::Set:
            n += static_cast<SetVal*>(o->external_ptr())->allocation_bytes() +
                 good_size(sizeof(SetVal)) - sizeof(CompactValue);
            break;
        case Type::Zset:
            n += static_cast<ZsetVal*>(o->external_ptr())->allocation_bytes() +
                 good_size(sizeof(ZsetVal)) - sizeof(CompactValue);
            break;
        case Type::Stream:
            n += static_cast<StreamVal*>(o->external_ptr())->allocation_bytes() +
                 good_size(sizeof(StreamVal)) - sizeof(CompactValue) +
                 stream_groups_allocation_bytes(
                     static_cast<StreamVal*>(o->external_ptr())->groups);
            break;
    }
    return n;
}

inline void kvobj_free(KvObj* o) {
    if (!o) return;
    const size_t n = good_size(kvobj_request_size(o));   // compute BEFORE the value block is released
    if (static_cast<Enc>(o->enc) == Enc::Extern && (o->flags & KvObjFlags::OwnsExtern)) {
        void* ext = o->external_ptr();
        switch (static_cast<Type>(o->type)) {
            case Type::String: free_sized(ext, good_size(o->vlen)); break;
            case Type::Hash: delete static_cast<HashVal*>(ext); break;
            case Type::List: delete static_cast<ListVal*>(ext); break;
            case Type::Set:  delete static_cast<SetVal*>(ext); break;
            case Type::Zset: delete static_cast<ZsetVal*>(ext); break;
            case Type::Stream: delete static_cast<StreamVal*>(ext); break;
        }
    }
    // Sized free: ordinary free() has to look up how big the block was; we already know.
    free_sized(o, n);
}

template <typename Sink>
inline bool obj_type_check(const KvObj* o, Type wanted, Sink&& sink) {
    if (!o || static_cast<Type>(o->type) == wanted) return true;
    reply_wrongtype(sink);
    return false;
}

}  // namespace tomo

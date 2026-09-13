// t_hash.cc — Redis-compatible single-key hash commands.
//
// Compact hashes keep one length-delimited field/value pair per Compact entry. Expanded hashes
// use a shard-owned open-addressed field index and a dense deque of entries. The index resizes
// incrementally; no store-path lock, atomic, or whole-hash resize walk is introduced here.
#include "command.h"
#include "notify.h"
#include "t_hash_ttl.h"
#include "../core/shard.h"
#include "../exec/op.h"
#include "../net/resp.h"
#include "../snapshot/format.h"
#include "../store/kvobj.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tomo {

void reply_maxmemory_oom(Op& op);

class HashFieldMap {
public:
    struct Node {
        uint64_t hash = 0;
        std::string field;
        std::string value;

        Node(uint64_t h, Slice f, Slice v)
            : hash(h), field(f.p ? f.p : "", f.n), value(v.p ? v.p : "", v.n) {}
    };

    enum class SetKind : uint8_t { Inserted, Updated, Oom };
    struct SetResult {
        SetKind kind = SetKind::Oom;
        uint32_t old_field_len = 0;
        uint32_t old_value_len = 0;
    };
    struct EraseResult {
        bool erased = false;
        uint32_t field_len = 0;
        uint32_t value_len = 0;
    };

    explicit HashFieldMap(uint64_t seed) : seed_(seed ? seed : 0x6a09e667f3bcc909ULL) {}
    ~HashFieldMap() {
        std::free(tab_[0]);
        std::free(tab_[1]);
    }
    HashFieldMap(const HashFieldMap&) = delete;
    HashFieldMap& operator=(const HashFieldMap&) = delete;

    bool initialize(uint32_t expected) {
        if (tab_[0]) return true;
        uint32_t cap = kMinCap;
        while (static_cast<uint64_t>(expected) * 100 >= static_cast<uint64_t>(cap) * kLoadPct) {
            if (cap > (uint32_t{1} << 30)) return false;
            cap <<= 1;
        }
        return allocate_initial(cap);
    }

    bool build_insert(Slice field, Slice value) {
        if (!tab_[0] && !initialize(1)) return false;
        const uint64_t hash = field_hash(field);
        try {
            nodes_.emplace_back(hash, field, value);
        } catch (const std::bad_alloc&) {
            return false;
        } catch (const std::length_error&) {
            return false;
        }
        const uint32_t index = static_cast<uint32_t>(nodes_.size() - 1);
        if (!insert_bucket(0, hash, index)) {
            nodes_.pop_back();
            return false;
        }
        node_bytes_ += node_memory(nodes_.back());
        return true;
    }

    const Node* find(Slice field) const {
        const uint64_t hash = field_hash(field);
        if (const Bucket* b = find_bucket_in(0, hash, field)) return &nodes_[b->node_index];
        if (rehashing())
            if (const Bucket* b = find_bucket_in(1, hash, field)) return &nodes_[b->node_index];
        return nullptr;
    }

    SetResult set(Slice field, Slice value) {
        rehash_step();
        const uint64_t hash = field_hash(field);
        Bucket* found = find_bucket(hash, field);
        if (found) {
            Node& node = nodes_[found->node_index];
            std::string replacement;
            try {
                replacement.assign(value.p ? value.p : "", value.n);
            } catch (const std::bad_alloc&) {
                return {SetKind::Oom, 0, 0};
            } catch (const std::length_error&) {
                return {SetKind::Oom, 0, 0};
            }
            const uint32_t old_value_len = static_cast<uint32_t>(node.value.size());
            const uint64_t old_storage = string_storage(node.value);
            node.value.swap(replacement);
            node_bytes_ = node_bytes_ - old_storage + string_storage(node.value);
            return {SetKind::Updated, static_cast<uint32_t>(node.field.size()), old_value_len};
        }

        if (nodes_.size() >= std::numeric_limits<uint32_t>::max() || !prepare_insert())
            return {SetKind::Oom, 0, 0};
        try {
            nodes_.emplace_back(hash, field, value);
        } catch (const std::bad_alloc&) {
            return {SetKind::Oom, 0, 0};
        } catch (const std::length_error&) {
            return {SetKind::Oom, 0, 0};
        }
        const uint32_t index = static_cast<uint32_t>(nodes_.size() - 1);
        if (!insert_bucket(0, hash, index)) {
            nodes_.pop_back();
            return {SetKind::Oom, 0, 0};
        }
        node_bytes_ += node_memory(nodes_.back());
        return {SetKind::Inserted, 0, 0};
    }

    EraseResult erase(Slice field) {
        rehash_step();
        const uint64_t hash = field_hash(field);
        int victim_table = 0;
        Bucket* victim = find_bucket_in(0, hash, field);
        if (!victim && rehashing()) {
            victim = find_bucket_in(1, hash, field);
            victim_table = 1;
        }
        if (!victim) return {};

        const uint32_t victim_pos = static_cast<uint32_t>(victim - tab_[victim_table]);
        const uint32_t index = victim->node_index;
        Node& removed = nodes_[index];
        const EraseResult result{true, static_cast<uint32_t>(removed.field.size()),
                                static_cast<uint32_t>(removed.value.size())};
        const uint64_t removed_memory = node_memory(removed);

        unlink_scan_bucket(victim_table, victim_pos);
        victim->state = kTomb;
        live_[victim_table]--;
        tombs_[victim_table]++;

        const uint32_t last = static_cast<uint32_t>(nodes_.size() - 1);
        if (index != last) {
            Node& moved = nodes_[last];
            Bucket* moved_bucket = find_bucket(moved.hash, as_slice(moved.field));
            std::swap(nodes_[index], nodes_[last]);
            if (moved_bucket) moved_bucket->node_index = index;
        }
        nodes_.pop_back();
        node_bytes_ -= removed_memory;
        maybe_start_shrink();
        return result;
    }

    uint32_t size() const { return static_cast<uint32_t>(nodes_.size()); }
    const Node& at(uint32_t index) const { return nodes_[index]; }
    uint64_t allocation_bytes() const { return table_bytes_ + node_bytes_; }

    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (const Node& node : nodes_) fn(node);
    }

    // One cursor step scans a logical HOME bucket through its intrusive scan chain, not just one
    // physical linear-probe slot. That makes displacement irrelevant and restores the premise
    // used by Redis dictScan's reverse-mask proof. During resize both the smaller home bucket and
    // every expansion of it in the larger table are visited before the cursor advances.
    template <typename Fn>
    uint64_t scan(uint64_t cursor, Fn&& fn) const {
        if (nodes_.empty() || !tab_[0]) return 0;
        if (!rehashing()) {
            scan_home(0, static_cast<uint32_t>(cursor) & mask_[0], fn);
            return advance_cursor(cursor, mask_[0]);
        }

        if (mask_[0] == mask_[1]) {
            const uint32_t home = static_cast<uint32_t>(cursor) & mask_[0];
            scan_home(0, home, fn);
            scan_home(1, home, fn);
            return advance_cursor(cursor, mask_[0]);
        }

        int small = cap_[0] < cap_[1] ? 0 : 1;
        int large = small ^ 1;
        const uint64_t small_mask = mask_[small];
        const uint64_t large_mask = mask_[large];
        scan_home(small, static_cast<uint32_t>(cursor & small_mask), fn);
        do {
            scan_home(large, static_cast<uint32_t>(cursor & large_mask), fn);
            cursor = scan_cursor_next(cursor, large_mask);
        } while (cursor & (small_mask ^ large_mask));
        return cursor;
    }

private:
    struct Bucket {
        uint64_t hash = 0;
        uint32_t node_index = 0;
        // One intrusive list per logical home bucket makes scan work proportional to entries
        // emitted from that bucket even when linear probing forms a long physical cluster. Links
        // are one-based so calloc's zero is the null link.
        uint32_t home_head = 0;
        uint32_t scan_next = 0;
        uint32_t scan_prev = 0;
        uint8_t state = 0;
        uint8_t pad[3] = {};
    };

    static constexpr uint8_t kEmpty = 0;
    static constexpr uint8_t kLive = 1;
    static constexpr uint8_t kTomb = 2;
    static constexpr uint32_t kMinCap = 8;
    static constexpr uint32_t kLoadPct = 70;
    static constexpr uint32_t kRehashBucketsPerWrite = 8;

    static Slice as_slice(const std::string& s) {
        return Slice(s.data(), static_cast<uint32_t>(s.size()));
    }
    static uint64_t string_storage(const std::string& s) {
        return static_cast<uint64_t>(s.capacity()) + 1;
    }
    static uint64_t node_memory(const Node& node) {
        return sizeof(Node) + string_storage(node.field) + string_storage(node.value);
    }
    static uint64_t mix(uint64_t value) {
        value ^= value >> 33;
        value *= 0xff51afd7ed558ccdULL;
        value ^= value >> 33;
        value *= 0xc4ceb9fe1a85ec53ULL;
        value ^= value >> 33;
        return value;
    }

    uint64_t field_hash(Slice field) const {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(field.p);
        uint32_t n = field.n;
        uint64_t hash = seed_ ^ (static_cast<uint64_t>(n) * 0x9e3779b97f4a7c15ULL);
        while (n >= 8) {
            uint64_t word;
            std::memcpy(&word, p, 8);
            hash = mix(hash ^ word);
            p += 8;
            n -= 8;
        }
        uint64_t tail = 0;
        if (n) std::memcpy(&tail, p, n);
        return mix(hash ^ tail ^ (static_cast<uint64_t>(field.n) << 32));
    }

    bool rehashing() const { return tab_[1] != nullptr; }

    bool allocate_initial(uint32_t cap) {
        Bucket* table = static_cast<Bucket*>(std::calloc(cap, sizeof(Bucket)));
        if (!table) return false;
        tab_[0] = table;
        cap_[0] = cap;
        mask_[0] = cap - 1;
        table_bytes_ += static_cast<uint64_t>(cap) * sizeof(Bucket);
        return true;
    }

    bool start_resize(uint32_t cap) {
        if (rehashing()) return true;
        Bucket* table = static_cast<Bucket*>(std::calloc(cap, sizeof(Bucket)));
        if (!table) return false;
        tab_[1] = tab_[0];
        cap_[1] = cap_[0];
        mask_[1] = mask_[0];
        live_[1] = live_[0];
        tombs_[1] = tombs_[0];
        tab_[0] = table;
        cap_[0] = cap;
        mask_[0] = cap - 1;
        live_[0] = tombs_[0] = 0;
        rehash_pos_ = 0;
        table_bytes_ += static_cast<uint64_t>(cap) * sizeof(Bucket);
        return true;
    }

    bool prepare_insert() {
        if (!tab_[0] && !allocate_initial(kMinCap)) return false;
        if (rehashing()) return true;
        const uint64_t occupied = static_cast<uint64_t>(live_[0]) + tombs_[0] + 1;
        if ((static_cast<uint64_t>(live_[0]) + 1) * 100 >=
            static_cast<uint64_t>(cap_[0]) * kLoadPct) {
            if (cap_[0] > (uint32_t{1} << 30)) return false;
            return start_resize(cap_[0] << 1);
        }
        if (occupied * 100 >= static_cast<uint64_t>(cap_[0]) * 85 && tombs_[0] > live_[0] / 2)
            (void)start_resize(cap_[0]);
        return true;
    }

    void maybe_start_shrink() {
        if (rehashing() || cap_[0] <= kMinCap) return;
        if (static_cast<uint64_t>(nodes_.size()) * 100 <=
            static_cast<uint64_t>(cap_[0]) * 15)
            (void)start_resize(cap_[0] >> 1);
    }

    bool insert_bucket(int table, uint64_t hash, uint32_t node_index) {
        uint32_t pos = static_cast<uint32_t>(hash) & mask_[table];
        int32_t first_tomb = -1;
        for (uint32_t probes = 0; probes < cap_[table]; probes++) {
            Bucket& bucket = tab_[table][pos];
            if (bucket.state == kEmpty) {
                if (first_tomb >= 0) {
                    pos = static_cast<uint32_t>(first_tomb);
                    tombs_[table]--;
                }
                link_scan_bucket(table, pos, hash, node_index);
                live_[table]++;
                return true;
            }
            if (bucket.state == kTomb && first_tomb < 0) first_tomb = static_cast<int32_t>(pos);
            pos = (pos + 1) & mask_[table];
        }
        if (first_tomb >= 0) {
            link_scan_bucket(table, static_cast<uint32_t>(first_tomb), hash, node_index);
            live_[table]++;
            tombs_[table]--;
            return true;
        }
        return false;
    }

    const Bucket* find_bucket_in(int table, uint64_t hash, Slice field) const {
        if (!tab_[table]) return nullptr;
        uint32_t pos = static_cast<uint32_t>(hash) & mask_[table];
        for (uint32_t probes = 0; probes < cap_[table]; probes++) {
            const Bucket& bucket = tab_[table][pos];
            if (bucket.state == kEmpty) return nullptr;
            if (bucket.state == kLive && bucket.hash == hash &&
                as_slice(nodes_[bucket.node_index].field) == field)
                return &bucket;
            pos = (pos + 1) & mask_[table];
        }
        return nullptr;
    }

    Bucket* find_bucket_in(int table, uint64_t hash, Slice field) {
        return const_cast<Bucket*>(
            static_cast<const HashFieldMap*>(this)->find_bucket_in(table, hash, field));
    }
    Bucket* find_bucket(uint64_t hash, Slice field) {
        if (Bucket* b = find_bucket_in(0, hash, field)) return b;
        return rehashing() ? find_bucket_in(1, hash, field) : nullptr;
    }

    void link_scan_bucket(int table, uint32_t pos, uint64_t hash, uint32_t node_index) {
        Bucket& bucket = tab_[table][pos];
        const uint32_t saved_home_head = bucket.home_head;
        bucket.hash = hash;
        bucket.node_index = node_index;
        bucket.state = kLive;
        bucket.scan_prev = 0;

        const uint32_t home = static_cast<uint32_t>(hash) & mask_[table];
        const uint32_t old_head = tab_[table][home].home_head;
        bucket.scan_next = old_head;
        if (old_head) tab_[table][old_head - 1].scan_prev = pos + 1;
        tab_[table][home].home_head = pos + 1;

        // When this physical slot is itself a different logical bucket's home, its home-head is
        // independent of the entry now stored in the slot and must survive slot reuse.
        if (pos != home) bucket.home_head = saved_home_head;
    }

    void unlink_scan_bucket(int table, uint32_t pos) {
        Bucket& bucket = tab_[table][pos];
        const uint32_t home = static_cast<uint32_t>(bucket.hash) & mask_[table];
        if (bucket.scan_prev)
            tab_[table][bucket.scan_prev - 1].scan_next = bucket.scan_next;
        else
            tab_[table][home].home_head = bucket.scan_next;
        if (bucket.scan_next)
            tab_[table][bucket.scan_next - 1].scan_prev = bucket.scan_prev;
        bucket.scan_next = bucket.scan_prev = 0;
    }

    void rehash_step() {
        if (!rehashing()) return;
        uint32_t budget = kRehashBucketsPerWrite;
        while (budget && rehash_pos_ < cap_[1]) {
            Bucket& old = tab_[1][rehash_pos_];
            if (old.state == kLive) {
                if (!insert_bucket(0, old.hash, old.node_index)) return;
                unlink_scan_bucket(1, rehash_pos_);
                old.state = kTomb;
                live_[1]--;
                tombs_[1]++;
            }
            rehash_pos_++;
            budget--;
        }
        if (rehash_pos_ == cap_[1]) {
            table_bytes_ -= static_cast<uint64_t>(cap_[1]) * sizeof(Bucket);
            std::free(tab_[1]);
            tab_[1] = nullptr;
            cap_[1] = mask_[1] = live_[1] = tombs_[1] = rehash_pos_ = 0;
        }
    }

    // The reverse-binary counter itself lives beside its derivation in src/store/flatstore.h; the
    // keyspace table and the zset member table walk their cursors with the same two lines.
    static uint64_t advance_cursor(uint64_t cursor, uint64_t mask) {
        return scan_cursor_next(cursor, mask);
    }

    template <typename Fn>
    void scan_home(int table, uint32_t home, Fn& fn) const {
        uint32_t link = tab_[table][home].home_head;
        while (link) {
            const Bucket& bucket = tab_[table][link - 1];
            const uint32_t next = bucket.scan_next;
            if (bucket.state == kLive) fn(nodes_[bucket.node_index]);
            link = next;
        }
    }

    uint64_t seed_ = 0;
    Bucket* tab_[2] = {nullptr, nullptr};
    uint32_t cap_[2] = {0, 0};
    uint32_t mask_[2] = {0, 0};
    uint32_t live_[2] = {0, 0};
    uint32_t tombs_[2] = {0, 0};
    uint32_t rehash_pos_ = 0;
    uint64_t table_bytes_ = 0;
    uint64_t node_bytes_ = 0;
    std::deque<Node> nodes_;
};

HashVal::HashVal(uint64_t seed) {
    random_state = seed ^ 0xd1b54a32d192ed03ULL;
    if (!random_state) random_state = 0x9e3779b97f4a7c15ULL;
}

HashVal::~HashVal() {
    delete fields;
    delete ttls;
}

uint64_t HashVal::random_bounded(uint64_t bound) {
    if (!bound) return 0;
    uint64_t value = random_state;
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    random_state = value;
    value *= 0x2545f4914f6cdd1dULL;
    return static_cast<uint64_t>((static_cast<unsigned __int128>(value) * bound) >> 64);
}

namespace {

struct PairView {
    Compact::Entry entry;
    Slice field;
    Slice value;
};

uint32_t uleb_size(uint32_t value) {
    uint32_t size = 1;
    while (value >= 0x80) {
        value >>= 7;
        size++;
    }
    return size;
}

void encode_uleb(char* dst, uint32_t value) {
    while (value >= 0x80) {
        *dst++ = static_cast<char>((value & 0x7f) | 0x80);
        value >>= 7;
    }
    *dst = static_cast<char>(value);
}

bool encode_pair(Slice field, Slice value, std::string& out) {
    const uint32_t header = uleb_size(field.n);
    const uint64_t total = static_cast<uint64_t>(header) + field.n + value.n;
    if (total > std::numeric_limits<uint32_t>::max()) return false;
    try {
        out.resize(static_cast<size_t>(total));
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }
    encode_uleb(out.data(), field.n);
    if (field.n) std::memcpy(out.data() + header, field.p, field.n);
    if (value.n) std::memcpy(out.data() + header + field.n, value.p, value.n);
    return true;
}

bool decode_pair(const Compact::Entry& entry, PairView& out) {
    uint32_t field_len = 0;
    uint32_t shift = 0;
    uint32_t pos = 0;
    for (uint32_t n = 0; n < 5 && pos < entry.value.n; n++, pos++) {
        const uint8_t byte = static_cast<uint8_t>(entry.value.p[pos]);
        if (shift == 28 && (byte & 0xf0)) return false;
        field_len |= static_cast<uint32_t>(byte & 0x7f) << shift;
        if (!(byte & 0x80)) {
            const uint32_t header = pos + 1;
            if (field_len > entry.value.n - header) return false;
            out.entry = entry;
            out.field = Slice(entry.value.p + header, field_len);
            out.value = Slice(entry.value.p + header + field_len,
                              entry.value.n - header - field_len);
            return true;
        }
        shift += 7;
    }
    return false;
}

bool compact_find(const CollectionRef& hash, Slice wanted, PairView& out) {
    for (auto it = hash.begin(); it != hash.end(); ++it) {
        PairView pair;
        if (decode_pair(*it, pair) && pair.field == wanted) {
            out = pair;
            return true;
        }
    }
    return false;
}

template <typename Fn>
void compact_for_each(const CollectionRef& hash, Fn&& fn) {
    for (auto it = hash.begin(); it != hash.end(); ++it) {
        PairView pair;
        if (decode_pair(*it, pair)) fn(pair);
    }
}

uint64_t hash_compact_payload(const CollectionRef& hash) {
    return hash.is_embedded() ? hash.aux0()
                              : hash.external_as<HashVal>()->compact_payload_bytes;
}

void set_hash_compact_payload(CollectionRef& hash, uint64_t bytes) {
    if (hash.is_embedded()) hash.set_aux0(bytes);
    else hash.external_as<HashVal>()->compact_payload_bytes = bytes;
}

uint64_t hash_random_state(const CollectionRef& hash) {
    return hash.is_embedded() ? hash.aux1() : hash.external_as<HashVal>()->random_state;
}

void set_hash_random_state(CollectionRef& hash, uint64_t state) {
    if (hash.is_embedded()) hash.set_aux1(state);
    else hash.external_as<HashVal>()->random_state = state;
}

HashFieldMap* hash_fields(const CollectionRef& hash) {
    return hash.external_as<HashVal>()->fields;
}

uint64_t hash_random_bounded(CollectionRef& hash, uint64_t bound) {
    if (!bound) return 0;
    uint64_t state = hash_random_state(hash);
    uint64_t value = state;
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    state = value;
    set_hash_random_state(hash, state);
    value *= 0x2545f4914f6cdd1dULL;
    return static_cast<uint64_t>((static_cast<unsigned __int128>(value) * bound) >> 64);
}

uint64_t expanded_allocation(const CollectionRef& hash) {
    HashFieldMap* fields = hash_fields(hash);
    return fields ? sizeof(HashFieldMap) + fields->allocation_bytes() : 0;
}

bool promote_hash(CollectionRef& hash) {
    HashVal* external = hash.external_as<HashVal>();
    auto* map = new (std::nothrow) HashFieldMap(hash_random_state(hash));
    if (!map) return false;
    if (!map->initialize(hash.entries())) {
        delete map;
        return false;
    }
    bool ok = true;
    compact_for_each(hash, [&](const PairView& pair) {
        if (ok && !map->build_insert(pair.field, pair.value)) ok = false;
    });
    if (!ok || map->size() != hash.entries()) {
        delete map;
        return false;
    }

    const uint32_t logical_entries = hash.entries();
    const uint64_t logical_payload = hash_compact_payload(hash);
    external->fields = map;
    external->promote(CollectionEncoding::Hashtable, expanded_allocation(hash),
                      logical_entries, logical_payload);
    external->compact_payload_bytes = 0;
    return true;
}

enum class HashSetKind : uint8_t { Inserted, Updated, Oom };

HashSetKind hash_set(CollectionRef& hash, const CompactLimit& limit, Slice field, Slice value) {
    if (hash.encoding() == CollectionEncoding::Compact) {
        PairView old;
        const bool exists = compact_find(hash, field, old);
        if (!exists && hash.entries() == std::numeric_limits<uint32_t>::max())
            return HashSetKind::Oom;
        const uint32_t resulting_entries = hash.entries() + (exists ? 0u : 1u);
        const uint32_t incoming_max = std::max(field.n, value.n);
        // This is the conversion decision: two maintained/scalar reads and the current write's
        // lengths. It never scans existing fields or bytes to decide whether promotion is needed.
        if (!hash.compact_fits(limit, resulting_entries, incoming_max) && !promote_hash(hash))
            return HashSetKind::Oom;

        if (hash.encoding() == CollectionEncoding::Compact) {
            std::string encoded;
            if (!encode_pair(field, value, encoded)) return HashSetKind::Oom;
            if (exists) {
                if (!hash.replace(old.entry, Slice(encoded.data(), static_cast<uint32_t>(encoded.size()))))
                    return HashSetKind::Oom;
                set_hash_compact_payload(hash,
                    hash_compact_payload(hash) - old.value.n + value.n);
                return HashSetKind::Updated;
            }
            if (!hash.append(Slice(encoded.data(), static_cast<uint32_t>(encoded.size()))))
                return HashSetKind::Oom;
            set_hash_compact_payload(hash,
                hash_compact_payload(hash) + static_cast<uint64_t>(field.n) + value.n);
            return HashSetKind::Inserted;
        }
    }

    HashVal* external = hash.external_as<HashVal>();
    HashFieldMap::SetResult result = external->fields->set(field, value);
    const uint64_t allocation = expanded_allocation(hash);
    if (result.kind == HashFieldMap::SetKind::Inserted) {
        external->note_expanded_insert(field.n + value.n, allocation);
        return HashSetKind::Inserted;
    }
    if (result.kind == HashFieldMap::SetKind::Updated) {
        external->note_expanded_replace(result.old_field_len + result.old_value_len,
                                        field.n + value.n, allocation);
        return HashSetKind::Updated;
    }
    // A bounded rehash step may have completed before a later allocation failed. Refresh the
    // maintained allocation total even though the logical field/value set did not change.
    external->note_expanded_replace(0, 0, allocation);
    return HashSetKind::Oom;
}

bool hash_get(const CollectionRef& hash, Slice field, Slice& value) {
    if (hash.encoding() == CollectionEncoding::Compact) {
        PairView pair;
        if (!compact_find(hash, field, pair)) return false;
        value = pair.value;
        return true;
    }
    const HashFieldMap::Node* node = hash_fields(hash)->find(field);
    if (!node) return false;
    value = Slice(node->value.data(), static_cast<uint32_t>(node->value.size()));
    return true;
}

bool hash_erase(CollectionRef& hash, Slice field) {
    if (hash.encoding() == CollectionEncoding::Compact) {
        PairView pair;
        if (!compact_find(hash, field, pair)) return false;
        const uint64_t payload = static_cast<uint64_t>(pair.field.n) + pair.value.n;
        if (!hash.erase(pair.entry)) return false;
        set_hash_compact_payload(hash, hash_compact_payload(hash) - payload);
        return true;
    }
    HashVal* external = hash.external_as<HashVal>();
    const HashFieldMap::EraseResult result = external->fields->erase(field);
    if (!result.erased) {
        external->note_expanded_replace(0, 0, expanded_allocation(hash));
        return false;
    }
    external->note_expanded_delete(result.field_len + result.value_len, expanded_allocation(hash));
    return true;
}

CollectionRef as_hash(KvObj* object) { return CollectionRef(object); }

template <bool kNotify>
bool attach_new_hash(Shard& shard, Op& op, HashVal* hash) {
    KvObj* object = kvobj_adopt_hash(op.key(), hash);
    if (!object) {
        delete hash;
        reply_err(op.sink(), "ERR out of memory");
        return false;
    }
    const FlatStore::InsertResult inserted_ = shard.store_insert<kNotify>(op.hash, object);
if (inserted_ != FlatStore::InsertResult::Inserted) {
    kvobj_free(object);
    if (inserted_ == FlatStore::InsertResult::MaxmemoryOom) reply_maxmemory_oom(op);
    else reply_err(op.sink(), "ERR keyspace insert failed");
    return false;
    }
    return true;
}

template <bool kNotify>
bool externalize_hash(Shard& shard, Op& op, KvObj*& object) {
    CollectionRef source(object);
    if (!source.is_embedded()) return true;
    auto* value = new (std::nothrow) HashVal;
    if (!value) { reply_err(op.sink(), "ERR out of memory"); return false; }
    for (const Compact::Entry entry : source.compact()) {
        if (!value->append(entry.value)) {
            delete value;
            reply_err(op.sink(), "ERR out of memory");
            return false;
        }
    }
    value->compact_payload_bytes = hash_compact_payload(source);
    value->random_state = hash_random_state(source);
    KvObj* replacement = kvobj_new_hash(object->key(), value, object->expire_at_ms());
    if (!replacement) {
        delete value;
        reply_err(op.sink(), "ERR out of memory");
        return false;
    }
    replacement->set_eviction_meta(object->eviction_meta());
    const FlatStore::InsertResult inserted = shard.store_insert<kNotify>(op.hash, replacement);
    if (inserted != FlatStore::InsertResult::Inserted) {
        kvobj_free(replacement);
        if (inserted == FlatStore::InsertResult::MaxmemoryOom) reply_maxmemory_oom(op);
        else reply_err(op.sink(), "ERR keyspace insert failed");
        return false;
    }
    object = replacement;
    return true;
}

template <bool kNotify>
bool ensure_hash_write_capacity(Shard& shard, Op& op, KvObj*& object,
                                uint32_t additional_entries, uint64_t additional_encoded,
                                uint32_t incoming_max) {
    if (!object) return true;
    CollectionRef hash(object);
    if (!hash.is_embedded()) return true;
    const CompactLimit& limit = shard.type_limits().hash;
    const uint64_t resulting_entries = static_cast<uint64_t>(hash.entries()) + additional_entries;
    if (hash.embedded_bytes_fit(hash.compact().encoded_bytes() + additional_encoded) &&
        resulting_entries <= limit.max_entries && incoming_max <= limit.max_value)
        return true;
    return externalize_hash<kNotify>(shard, op, object);
}

void reply_wrong_arity(Op& op) {
    // Redis names the command in EVERY arity error, including the odd-field-pair one that only
    // the handler can detect. Spelled here the same way the dispatcher spells it (lower case).
    char message[96];
    char command[32];
    const size_t name_len = std::min(std::strlen(op.spec->name), sizeof(command) - 1);
    for (size_t i = 0; i < name_len; i++) {
        const char ch = op.spec->name[i];
        command[i] = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch + ('a' - 'A')) : ch;
    }
    command[name_len] = '\0';
    std::snprintf(message, sizeof(message),
                  "ERR wrong number of arguments for '%s' command", command);
    reply_err(op.sink(), message);
}

const HashFieldTtl* hash_ttls_of(const KvObj* object) {
    // An embedded hash lives in the KvObj tail and has no HashVal to hang a table off; the first
    // HEXPIRE on one externalizes it (see hash_ttl_externalize), so "embedded" implies "no TTLs".
    if (static_cast<Enc>(object->enc) == Enc::Compact) return nullptr;
    return static_cast<const HashVal*>(object->external_ptr())->ttls;
}

// Every hash command's single entry point: find, type-check, and — only when this shard actually
// holds a hash with field deadlines — reap anything already past before the handler reads it.
//
// THE ZERO-COST CLAIM LIVES ON THIS LINE. field_expire_count() is a plain uint32_t in the store; a
// shard that has never seen HEXPIRE pays one load and one predicted-false test per hash command and
// reaches no field-TTL code at all. Reaping (rather than filtering at each read site) is also what
// keeps every handler below unchanged: after this returns, all remaining fields are live.
template <bool kNotify>
inline bool hash_lookup(Shard& shard, Op& op, KvObj*& object) {
    object = shard.store_find<kNotify>(op.hash, op.key());
    if (!obj_type_check(object, Type::Hash, op.sink())) return false;
    if (__builtin_expect(shard.store().field_expire_count() != 0, false) && object)
        object = hash_ttl_on_access(shard, op, object, kNotify);
    return true;
}

bool preconvert_hset(CollectionRef& hash, const CompactLimit& limit, const Op& op) {
    if (hash.encoding() != CollectionEncoding::Compact) return true;
    const uint32_t incoming_fields = (op.argc() - 2) / 2;
    bool promote = incoming_fields > limit.max_entries;
    for (uint32_t i = 2; !promote && i < op.argc(); i++)
        promote = op.arg(i).n > limit.max_value;
    return !promote || promote_hash(hash);
}

bool parse_i64(Slice text, int64_t& value) {
    if (text.n == 0 || text.n >= 21) return false;
    if (text.n == 1 && text.p[0] == '0') {
        value = 0;
        return true;
    }
    uint32_t pos = 0;
    bool negative = false;
    if (text.p[0] == '-') {
        negative = true;
        if (++pos == text.n) return false;
    }
    if (text.p[pos] < '1' || text.p[pos] > '9') return false;
    uint64_t magnitude = static_cast<uint64_t>(text.p[pos++] - '0');
    for (; pos < text.n; pos++) {
        const char ch = text.p[pos];
        if (ch < '0' || ch > '9') return false;
        const uint32_t digit = static_cast<uint32_t>(ch - '0');
        if (magnitude > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
        magnitude = magnitude * 10 + digit;
    }
    const uint64_t limit = negative ? (uint64_t{1} << 63) : static_cast<uint64_t>(INT64_MAX);
    if (magnitude > limit) return false;
    if (negative)
        value = magnitude == (uint64_t{1} << 63) ? INT64_MIN : -static_cast<int64_t>(magnitude);
    else
        value = static_cast<int64_t>(magnitude);
    return true;
}

// HINCRBYFLOAT shares INCRBYFLOAT's string2ld grammar; see src/base/numeric.h.
bool parse_long_double(Slice text, long double& value) {
    return parse_long_double_strict(text, value);
}

uint32_t format_long_double(char* buffer, size_t capacity, long double value) {
    int length = std::snprintf(buffer, capacity, "%.17Lf", value);
    if (length <= 0 || static_cast<size_t>(length) >= capacity) return 0;
    char* dot = static_cast<char*>(std::memchr(buffer, '.', static_cast<size_t>(length)));
    if (dot) {
        while (length > 0 && buffer[length - 1] == '0') length--;
        if (length > 0 && buffer[length - 1] == '.') length--;
    }
    if (length == 2 && buffer[0] == '-' && buffer[1] == '0') {
        buffer[0] = '0';
        length = 1;
    }
    return static_cast<uint32_t>(length);
}

template <bool kNotify>
void cmd_hset(Shard& shard, Op& op) {
    if (op.argc() & 1u) {
        reply_wrong_arity(op);
        return;
    }
    KvObj* object = nullptr;
    if (!hash_lookup<kNotify>(shard, op, object)) return;

    uint64_t additional_encoded = 0;
    uint32_t incoming_max = 0;
    for (uint32_t i = 2; i < op.argc(); i += 2) {
        const uint32_t pair_bytes = uleb_size(op.arg(i).n) + op.arg(i).n + op.arg(i + 1).n;
        additional_encoded += Compact::entry_encoded_size(pair_bytes);
        incoming_max = std::max(incoming_max, std::max(op.arg(i).n, op.arg(i + 1).n));
    }
    if (!ensure_hash_write_capacity<kNotify>(shard, op, object, (op.argc() - 2) / 2,
                                    additional_encoded, incoming_max)) return;
    HashVal* owned = object ? nullptr : new (std::nothrow) HashVal(op.hash);
    if (!object && !owned) {
        reply_err(op.sink(), "ERR out of memory");
        return;
    }
    CollectionRef hash = object ? as_hash(object) : CollectionRef(owned);
    ObjectSizeTracker size_tracker(shard.store(), object);   // existing object: track growth
    // Wide fresh HSET/HMSET must not repeatedly scan an ever-growing Compact. As Redis, Valkey,
    // and the optimized fork do, use only the incoming pair count/lengths to pre-promote when the
    // command itself cannot fit; duplicates may conservatively over-promote, never under-promote.
    if (!preconvert_hset(hash, shard.type_limits().hash, op)) {
        if (!object) delete owned;
        reply_err(op.sink(), "ERR out of memory");
        return;
    }
    uint32_t created = 0;
    for (uint32_t i = 2; i < op.argc(); i += 2) {
        const HashSetKind result = hash_set(hash, shard.type_limits().hash, op.arg(i), op.arg(i + 1));
        if (result == HashSetKind::Oom) {
            if (!object) delete owned;
            reply_err(op.sink(), "ERR out of memory");
            return;
        }
        created += result == HashSetKind::Inserted;
    }
    // Redis 7.4: writing a field's VALUE discards that field's TTL (HINCRBY* deliberately do not).
    if (__builtin_expect(object != nullptr && shard.store().field_expire_count() != 0, false))
        for (uint32_t i = 2; i < op.argc(); i += 2) hash_ttl_clear_field(shard, object, op.arg(i));
    if (!object && !attach_new_hash<kNotify>(shard, op, owned)) return;
    if constexpr (kNotify)
        notify_record(shard, op, NOTIFY_HASH, NotifyEventId::Hset, op.key());
    if (op.cmd_name().eq_icase("hmset")) reply_ok(op.sink());
    else reply_int(op.sink(), created);
}

template <bool kNotify>
void cmd_hsetnx(Shard& shard, Op& op) {
    KvObj* object = nullptr;
    if (!hash_lookup<kNotify>(shard, op, object)) return;
    if (object) {
        Slice ignored;
        if (hash_get(as_hash(object), op.arg(2), ignored)) {
            reply_int(op.sink(), 0);
            return;
        }
    }

    const uint32_t pair_bytes = uleb_size(op.arg(2).n) + op.arg(2).n + op.arg(3).n;
    if (!ensure_hash_write_capacity<kNotify>(shard, op, object, 1,
                                    Compact::entry_encoded_size(pair_bytes),
                                    std::max(op.arg(2).n, op.arg(3).n))) return;
    HashVal* owned = object ? nullptr : new (std::nothrow) HashVal(op.hash);
    if (!object && !owned) {
        reply_err(op.sink(), "ERR out of memory");
        return;
    }
    CollectionRef hash = object ? as_hash(object) : CollectionRef(owned);
    ObjectSizeTracker size_tracker(shard.store(), object);
    if (hash_set(hash, shard.type_limits().hash, op.arg(2), op.arg(3)) == HashSetKind::Oom) {
        if (!object) delete owned;
        reply_err(op.sink(), "ERR out of memory");
        return;
    }
    if (!object && !attach_new_hash<kNotify>(shard, op, owned)) return;
    if constexpr (kNotify)
        notify_record(shard, op, NOTIFY_HASH, NotifyEventId::Hset, op.key());
    reply_int(op.sink(), 1);
}

template <bool kNotify>
void cmd_hget(Shard& shard, Op& op) {
    KvObj* object = nullptr;
    if (!hash_lookup<kNotify>(shard, op, object)) return;
    Slice value;
    if (!object || !hash_get(as_hash(object), op.arg(2), value))
        reply_null(op.sink(), op.resp3());
    else reply_bulk(op.sink(), value);
}

template <bool kNotify>
void cmd_hmget(Shard& shard, Op& op) {
    KvObj* object = nullptr;
    if (!hash_lookup<kNotify>(shard, op, object)) return;
    reply_array_header(op.sink(), op.argc() - 2);
    for (uint32_t i = 2; i < op.argc(); i++) {
        Slice value;
        if (!object || !hash_get(as_hash(object), op.arg(i), value))
            reply_null(op.sink(), op.resp3());
        else reply_bulk(op.sink(), value);
    }
}

template <bool kNotify>
void cmd_hdel(Shard& shard, Op& op) {
    KvObj* object = nullptr;
    if (!hash_lookup<kNotify>(shard, op, object)) return;
    if (!object) {
        reply_int(op.sink(), 0);
        return;
    }
    CollectionRef hash = as_hash(object);
    ObjectSizeTracker size_tracker(shard.store(), object);
    uint32_t deleted = 0;
    bool notified = false;
    for (uint32_t i = 2; i < op.argc(); i++) {
        if (!hash_erase(hash, op.arg(i))) continue;
        if (__builtin_expect(shard.store().field_expire_count() != 0, false))
            hash_ttl_clear_field(shard, object, op.arg(i));
        deleted++;
        if (hash.entries() == 0) {
            if constexpr (kNotify) {
                notify_record(shard, op, NOTIFY_HASH, NotifyEventId::Hdel, op.key());
                notified = true;
            }
            size_tracker.finish();               // account the shrink; erase subtracts the rest
            shard.store_erase<kNotify>(op.hash, op.key());
            break;
        }
    }
    if constexpr (kNotify) if (deleted && !notified)
        notify_record(shard, op, NOTIFY_HASH, NotifyEventId::Hdel, op.key());
    reply_int(op.sink(), deleted);
}

template <bool kNotify>
void cmd_hlen(Shard& shard, Op& op) {
    KvObj* object = nullptr;
    if (!hash_lookup<kNotify>(shard, op, object)) return;
    reply_int(op.sink(), object ? as_hash(object).entries() : 0);
}

template <bool kNotify>
void cmd_hexists(Shard& shard, Op& op) {
    KvObj* object = nullptr;
    if (!hash_lookup<kNotify>(shard, op, object)) return;
    Slice value;
    reply_int(op.sink(), object && hash_get(as_hash(object), op.arg(2), value) ? 1 : 0);
}

template <bool kNotify>
void cmd_hstrlen(Shard& shard, Op& op) {
    KvObj* object = nullptr;
    if (!hash_lookup<kNotify>(shard, op, object)) return;
    Slice value;
    reply_int(op.sink(), object && hash_get(as_hash(object), op.arg(2), value) ? value.n : 0);
}

template <bool kNotify>
void cmd_hincrby(Shard& shard, Op& op) {
    int64_t increment = 0;
    if (!parse_i64(op.arg(3), increment)) {
        reply_err(op.sink(), "ERR value is not an integer or out of range");
        return;
    }
    KvObj* object = nullptr;
    if (!hash_lookup<kNotify>(shard, op, object)) return;

    int64_t old_value = 0;
    Slice old_text;
    if (object && hash_get(as_hash(object), op.arg(2), old_text) &&
        !parse_i64(old_text, old_value)) {
        reply_err(op.sink(), "ERR hash value is not an integer");
        return;
    }
    if ((increment > 0 && old_value > INT64_MAX - increment) ||
        (increment < 0 && old_value < INT64_MIN - increment)) {
        reply_err(op.sink(), "ERR increment or decrement would overflow");
        return;
    }
    const int64_t value = old_value + increment;
    char text[24];
    const uint32_t length = i64_to_dec(text, value);

    const uint32_t encoded = Compact::entry_encoded_size(
        uleb_size(op.arg(2).n) + op.arg(2).n + length);
    if (!ensure_hash_write_capacity<kNotify>(shard, op, object, 1, encoded,
                                    std::max(op.arg(2).n, length))) return;
    HashVal* owned = object ? nullptr : new (std::nothrow) HashVal(op.hash);
    if (!object && !owned) {
        reply_err(op.sink(), "ERR out of memory");
        return;
    }
    CollectionRef hash = object ? as_hash(object) : CollectionRef(owned);
    ObjectSizeTracker size_tracker(shard.store(), object);
    if (hash_set(hash, shard.type_limits().hash, op.arg(2), Slice(text, length)) ==
        HashSetKind::Oom) {
        if (!object) delete owned;
        reply_err(op.sink(), "ERR out of memory");
        return;
    }
    if (!object && !attach_new_hash<kNotify>(shard, op, owned)) return;
    if constexpr (kNotify)
        notify_record(shard, op, NOTIFY_HASH, NotifyEventId::Hincrby, op.key());
    reply_int(op.sink(), value);
}

template <bool kNotify>
void cmd_hincrbyfloat(Shard& shard, Op& op) {
    long double increment = 0;
    if (!parse_long_double(op.arg(3), increment)) {
        reply_err(op.sink(), "ERR value is not a valid float");
        return;
    }
    if (std::isinf(increment)) {
        reply_err(op.sink(), "ERR value is NaN or Infinity");
        return;
    }
    KvObj* object = nullptr;
    if (!hash_lookup<kNotify>(shard, op, object)) return;

    long double old_value = 0;
    Slice old_text;
    if (object && hash_get(as_hash(object), op.arg(2), old_text) &&
        !parse_long_double(old_text, old_value)) {
        reply_err(op.sink(), "ERR hash value is not a float");
        return;
    }
    const long double value = old_value + increment;
    if (std::isnan(value) || std::isinf(value)) {
        reply_err(op.sink(), "ERR increment would produce NaN or Infinity");
        return;
    }
    char text[5 * 1024];
    const uint32_t length = format_long_double(text, sizeof(text), value);
    if (!length) {
        reply_err(op.sink(), "ERR increment would produce NaN or Infinity");
        return;
    }

    const uint32_t encoded = Compact::entry_encoded_size(
        uleb_size(op.arg(2).n) + op.arg(2).n + length);
    if (!ensure_hash_write_capacity<kNotify>(shard, op, object, 1, encoded,
                                    std::max(op.arg(2).n, length))) return;
    HashVal* owned = object ? nullptr : new (std::nothrow) HashVal(op.hash);
    if (!object && !owned) {
        reply_err(op.sink(), "ERR out of memory");
        return;
    }
    CollectionRef hash = object ? as_hash(object) : CollectionRef(owned);
    ObjectSizeTracker size_tracker(shard.store(), object);
    if (hash_set(hash, shard.type_limits().hash, op.arg(2), Slice(text, length)) ==
        HashSetKind::Oom) {
        if (!object) delete owned;
        reply_err(op.sink(), "ERR out of memory");
        return;
    }
    if (!object && !attach_new_hash<kNotify>(shard, op, owned)) return;
    if constexpr (kNotify)
        notify_record(shard, op, NOTIFY_HASH, NotifyEventId::Hincrbyfloat, op.key());
    reply_bulk(op.sink(), Slice(text, length));
}

enum class GetAllMode : uint8_t { Fields, Values, Both };

template <bool kNotify>
void generic_getall(Shard& shard, Op& op, GetAllMode mode) {
    KvObj* object = nullptr;
    if (!hash_lookup<kNotify>(shard, op, object)) return;
    if (!object) {
        if (mode == GetAllMode::Both) reply_map_header(op.sink(), 0, op.resp3());
        else reply_array_header(op.sink(), 0);
        return;
    }
    CollectionRef hash = as_hash(object);
    const uint64_t multiplier = mode == GetAllMode::Both ? 2 : 1;
    if (mode == GetAllMode::Both)
        reply_map_header(op.sink(), hash.entries(), op.resp3());
    else
        reply_array_header(op.sink(), static_cast<uint64_t>(hash.entries()) * multiplier);
    auto emit = [&](Slice field, Slice value) {
        if (mode != GetAllMode::Values) reply_bulk(op.sink(), field);
        if (mode != GetAllMode::Fields) reply_bulk(op.sink(), value);
    };
    if (hash.encoding() == CollectionEncoding::Compact) {
        compact_for_each(hash, [&](const PairView& pair) { emit(pair.field, pair.value); });
    } else {
        hash_fields(hash)->for_each([&](const HashFieldMap::Node& node) {
            emit(Slice(node.field.data(), static_cast<uint32_t>(node.field.size())),
                 Slice(node.value.data(), static_cast<uint32_t>(node.value.size())));
        });
    }
}

template <bool kNotify>
void cmd_hgetall(Shard& shard, Op& op) { generic_getall<kNotify>(shard, op, GetAllMode::Both); }
template <bool kNotify>
void cmd_hkeys(Shard& shard, Op& op) { generic_getall<kNotify>(shard, op, GetAllMode::Fields); }
template <bool kNotify>
void cmd_hvals(Shard& shard, Op& op) { generic_getall<kNotify>(shard, op, GetAllMode::Values); }

struct ScanOptions {
    Slice pattern;
    uint64_t count = 10;
    bool use_pattern = false;
    bool no_values = false;
};

bool parse_scan_options(Op& op, ScanOptions& options) {
    for (uint32_t i = 3; i < op.argc();) {
        const Slice option = op.arg(i);
        if (option.eq_icase("match") && i + 1 < op.argc()) {
            options.pattern = op.arg(i + 1);
            options.use_pattern = !(options.pattern.n == 1 && options.pattern.p[0] == '*');
            i += 2;
        } else if (option.eq_icase("count") && i + 1 < op.argc()) {
            int64_t count = 0;
            if (!parse_i64(op.arg(i + 1), count)) {
                reply_err(op.sink(), "ERR value is not an integer or out of range");
                return false;
            }
            if (count < 1) {
                reply_syntax(op.sink());
                return false;
            }
            options.count = static_cast<uint64_t>(count);
            i += 2;
        } else if (option.eq_icase("novalues")) {
            options.no_values = true;
            i++;
        } else {
            reply_syntax(op.sink());
            return false;
        }
    }
    return true;
}

struct ScanItem {
    Slice field;
    Slice value;
};

void reply_scan(Op& op, uint64_t cursor, const std::vector<ScanItem>& items, bool no_values) {
    char text[24];
    const uint32_t length = u64_to_dec(text, cursor);
    reply_array_header(op.sink(), 2);
    reply_bulk(op.sink(), Slice(text, length));
    reply_array_header(op.sink(), static_cast<uint64_t>(items.size()) * (no_values ? 1 : 2));
    for (const ScanItem& item : items) {
        reply_bulk(op.sink(), item.field);
        if (!no_values) reply_bulk(op.sink(), item.value);
    }
}

template <bool kNotify>
void cmd_hscan(Shard& shard, Op& op) {
    uint64_t cursor = 0;
    if (!command_parse_scan_cursor(op.arg(2), cursor)) {
        reply_err(op.sink(), "ERR invalid cursor");
        return;
    }
    KvObj* object = nullptr;
    if (!hash_lookup<kNotify>(shard, op, object)) return;
    if (!object) {
        // Redis returns the empty scan reply before parsing trailing options for a missing key.
        reply_scan(op, 0, {}, false);
        return;
    }
    ScanOptions options;
    if (!parse_scan_options(op, options)) return;

    CollectionRef hash = as_hash(object);
    std::vector<ScanItem> items;
    try {
        if (hash.encoding() == CollectionEncoding::Compact) {
            const uint64_t start = cursor;
            uint64_t position = 0;
            uint64_t sampled = 0;
            for (auto it = hash.begin(); it != hash.end(); ++it) {
                PairView pair;
                if (!decode_pair(*it, pair)) continue;
                if (position++ < start) continue;
                if (sampled >= options.count) {
                    position--;
                    break;
                }
                sampled++;
                if (!options.use_pattern || command_glob_match(options.pattern, pair.field))
                    items.push_back({pair.field, pair.value});
            }
            cursor = position >= hash.entries() ? 0 : position;
        } else {
            uint64_t sampled = 0;
            uint64_t iterations = 0;
            const uint64_t max_iterations = options.count > UINT64_MAX / 10
                                                ? UINT64_MAX : options.count * 10;
            do {
                cursor = hash_fields(hash)->scan(cursor, [&](const HashFieldMap::Node& node) {
                    sampled++;
                    const Slice field(node.field.data(), static_cast<uint32_t>(node.field.size()));
                    if (!options.use_pattern || command_glob_match(options.pattern, field))
                        items.push_back({field, Slice(node.value.data(),
                                                     static_cast<uint32_t>(node.value.size()))});
                });
                iterations++;
            } while (cursor && sampled < options.count && iterations < max_iterations);
        }
    } catch (const std::bad_alloc&) {
        reply_err(op.sink(), "ERR out of memory");
        return;
    } catch (const std::length_error&) {
        reply_err(op.sink(), "ERR out of memory");
        return;
    }
    reply_scan(op, cursor, items, options.no_values);
}

bool compact_pairs(const CollectionRef& hash, std::vector<PairView>& pairs) {
    try {
        pairs.reserve(hash.entries());
        compact_for_each(hash, [&](const PairView& pair) { pairs.push_back(pair); });
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }
    return pairs.size() == hash.entries();
}

bool select_unique(CollectionRef& hash, uint32_t population, uint32_t count,
                   std::vector<uint32_t>& selected) {
    try {
        selected.reserve(count);
        if (static_cast<uint64_t>(count) * 3 > population) {
            std::vector<uint32_t> all(population);
            for (uint32_t i = 0; i < population; i++) all[i] = i;
            for (uint32_t i = 0; i < count; i++) {
                const uint32_t chosen = i + static_cast<uint32_t>(
                    hash_random_bounded(hash, static_cast<uint64_t>(population - i)));
                std::swap(all[i], all[chosen]);
                selected.push_back(all[i]);
            }
            return true;
        }
        std::unordered_set<uint32_t> seen;
        seen.reserve(static_cast<size_t>(count) * 2);
        while (selected.size() < count) {
            const uint32_t candidate = static_cast<uint32_t>(hash_random_bounded(hash, population));
            if (seen.insert(candidate).second) selected.push_back(candidate);
        }
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }
}

template <bool kNotify>
void cmd_hrandfield(Shard& shard, Op& op) {
    int64_t signed_count = 0;
    bool with_values = false;
    if (op.argc() >= 3) {
        if (!parse_i64(op.arg(2), signed_count)) {
            reply_err(op.sink(), "ERR value is not an integer or out of range");
            return;
        }
        if (signed_count == INT64_MIN) {
            reply_outofrange(op.sink());
            return;
        }
        if (op.argc() > 4) {
            reply_syntax(op.sink());
            return;
        }
        if (op.argc() == 4) {
            if (!op.arg(3).eq_icase("withvalues")) {
                reply_syntax(op.sink());
                return;
            }
            with_values = true;
            if (signed_count < -INT64_MAX / 2 || signed_count > INT64_MAX / 2) {
                reply_outofrange(op.sink());
                return;
            }
        }
    }

    KvObj* object = nullptr;
    if (!hash_lookup<kNotify>(shard, op, object)) return;
    if (!object) {
        if (op.argc() == 2) reply_null(op.sink(), op.resp3());
        else reply_array_header(op.sink(), 0);
        return;
    }
    CollectionRef hash = as_hash(object);
    const uint32_t population = hash.entries();
    if (!population) {
        if (op.argc() == 2) reply_null(op.sink(), op.resp3());
        else reply_array_header(op.sink(), 0);
        return;
    }

    std::vector<PairView> compact;
    if (hash.encoding() == CollectionEncoding::Compact && !compact_pairs(hash, compact)) {
        reply_err(op.sink(), "ERR out of memory");
        return;
    }
    auto pair_at = [&](uint32_t index) -> std::pair<Slice, Slice> {
        if (hash.encoding() == CollectionEncoding::Compact)
            return {compact[index].field, compact[index].value};
        const HashFieldMap::Node& node = hash_fields(hash)->at(index);
        return {Slice(node.field.data(), static_cast<uint32_t>(node.field.size())),
                Slice(node.value.data(), static_cast<uint32_t>(node.value.size()))};
    };

    if (op.argc() == 2) {
        const auto pair = pair_at(static_cast<uint32_t>(hash_random_bounded(hash, population)));
        reply_bulk(op.sink(), pair.first);
        return;
    }
    if (signed_count == 0) {
        reply_array_header(op.sink(), 0);
        return;
    }

    const bool unique = signed_count > 0;
    const uint64_t requested = unique ? static_cast<uint64_t>(signed_count)
                                      : static_cast<uint64_t>(-signed_count);
    if (!unique) {
        const bool pairs = with_values && op.resp3();
        reply_array_header(op.sink(), requested * (with_values && !pairs ? 2 : 1));
        for (uint64_t i = 0; i < requested; i++) {
            const auto pair = pair_at(static_cast<uint32_t>(hash_random_bounded(hash, population)));
            if (pairs) reply_array_header(op.sink(), 2);
            reply_bulk(op.sink(), pair.first);
            if (with_values) reply_bulk(op.sink(), pair.second);
        }
        return;
    }

    const uint32_t count = requested >= population ? population : static_cast<uint32_t>(requested);
    if (count == population) {
        const bool pairs = with_values && op.resp3();
        reply_array_header(op.sink(), static_cast<uint64_t>(count) *
                                      (with_values && !pairs ? 2 : 1));
        for (uint32_t i = 0; i < count; i++) {
            const auto pair = pair_at(i);
            if (pairs) reply_array_header(op.sink(), 2);
            reply_bulk(op.sink(), pair.first);
            if (with_values) reply_bulk(op.sink(), pair.second);
        }
        return;
    }

    std::vector<uint32_t> selected;
    if (!select_unique(hash, population, count, selected)) {
        reply_err(op.sink(), "ERR out of memory");
        return;
    }
    const bool pairs = with_values && op.resp3();
    reply_array_header(op.sink(), static_cast<uint64_t>(selected.size()) *
                                  (with_values && !pairs ? 2 : 1));
    for (uint32_t index : selected) {
        const auto pair = pair_at(index);
        if (pairs) reply_array_header(op.sink(), 2);
        reply_bulk(op.sink(), pair.first);
        if (with_values) reply_bulk(op.sink(), pair.second);
    }
}

#define TOMO_HANDLER_PAIR(fn) fn<false>, 1, 1, 1, notify_handler<fn<true>>

static const CommandSpec kTable[] = {
    // name          min max flags                handler             first last step
    {"HSET",          4, -1, CmdFlags::Write | CmdFlags::DenyOom, TOMO_HANDLER_PAIR(cmd_hset)},
    {"HMSET",         4, -1, CmdFlags::Write | CmdFlags::DenyOom, TOMO_HANDLER_PAIR(cmd_hset)},
    {"HSETNX",        4,  4, CmdFlags::Write | CmdFlags::DenyOom, TOMO_HANDLER_PAIR(cmd_hsetnx)},
    {"HGET",          3,  3, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_hget)},
    {"HMGET",         3, -1, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_hmget)},
    {"HDEL",          3, -1, CmdFlags::Write,    TOMO_HANDLER_PAIR(cmd_hdel)},
    {"HLEN",          2,  2, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_hlen)},
    {"HEXISTS",       3,  3, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_hexists)},
    {"HSTRLEN",       3,  3, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_hstrlen)},
    {"HINCRBY",       4,  4, CmdFlags::Write | CmdFlags::DenyOom, TOMO_HANDLER_PAIR(cmd_hincrby)},
    {"HINCRBYFLOAT",  4,  4, CmdFlags::Write | CmdFlags::DenyOom, TOMO_HANDLER_PAIR(cmd_hincrbyfloat)},
    {"HGETALL",       2,  2, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_hgetall)},
    {"HKEYS",         2,  2, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_hkeys)},
    {"HVALS",         2,  2, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_hvals)},
    {"HRANDFIELD",    2, -1, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_hrandfield)},
    {"HSCAN",         3, -1, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_hscan)},
};

#undef TOMO_HANDLER_PAIR

}  // namespace


namespace {

int64_t snapshot_now_ms() {
    // Only reached for records that actually carry field deadlines (encoding 1); TTL-free hashes
    // never call it, so a bulk load of ordinary hashes reads no clock at all.
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// Logical hash payload: per pair [u32 flen][field][u32 vlen][value], encoding byte 0.  Encoding 1
// appends [i64 expire_ms] (-1 = none) to each pair.  The walk is representation-agnostic; load
// rebuilds through hash_set so encoding follows the CURRENT limits.
template <typename Fn>
void hash_walk(const CollectionRef& hash, Fn&& fn) {
    if (hash.encoding() == CollectionEncoding::Compact) {
        compact_for_each(hash, [&](const PairView& pair) { fn(pair.field, pair.value); });
    } else {
        hash_fields(hash)->for_each([&](const HashFieldMap::Node& node) {
            fn(Slice(node.field.data(), static_cast<uint32_t>(node.field.size())),
               Slice(node.value.data(), static_cast<uint32_t>(node.value.size())));
        });
    }
}

// Field deadlines ride the ordinary value payload, selected by the record's existing per-type
// `encoding` byte: 0 is the untouched TTL-free form, 1 appends an absolute i64 deadline (-1 = none)
// to every pair. That single decision buys snapshot, AOF base, AOF increments and DUMP/RESTORE at
// once, because all four already funnel through these three hooks — no command-rewrite path exists
// in this tree and none had to be invented. Deadlines are absolute, so replay is deterministic and
// anything already past is dropped on load rather than resurrected.
SnapshotHookStatus hash_snapshot_begin(const KvObj& object, SnapshotSaveCursor& cursor,
                                       uint8_t& encoding) {
    if (static_cast<Type>(object.type) != Type::Hash) return SnapshotHookStatus::Corrupt;
    cursor = {};
    cursor.object = &object;
    const bool with_ttl = hash_ttls_of(&object) != nullptr;
    encoding = with_ttl ? 1 : 0;
    cursor.lane[2] = with_ttl ? 1 : 0;
    const uint64_t per_pair = with_ttl ? 16ull : 8ull;
    uint64_t total = 0;
    hash_walk(as_hash(const_cast<KvObj*>(&object)),
              [&](Slice f, Slice v) { total += per_pair + f.n + v.n; });
    cursor.total = total;
    return SnapshotHookStatus::Ok;
}

SnapshotHookStatus hash_snapshot_read(SnapshotSaveCursor& cursor, uint8_t* destination,
                                      size_t capacity, size_t& written) {
    written = 0;
    if (!cursor.object) return SnapshotHookStatus::Corrupt;
    CollectionRef hash = as_hash(const_cast<KvObj*>(cursor.object));
    // begin_save fixed the per-pair width; re-deriving `with_ttl` from the object could disagree
    // with cursor.total if the table were dropped mid-stream, so the decision is carried, not redone.
    const bool with_ttl = cursor.lane[2] != 0;
    const HashFieldTtl* ttls = with_ttl ? hash_ttls_of(cursor.object) : nullptr;
    SnapshotElementEmitter e{destination, capacity};
    uint64_t idx = 0;
    bool stopped = false;
    hash_walk(hash, [&](Slice f, Slice v) {
        if (stopped) { idx++; return; }
        if (idx < cursor.lane[0]) { idx++; return; }
        e.pos = 0;
        e.resume = idx == cursor.lane[0] ? cursor.lane[1] : 0;
        const bool ok = e.put_u32(f.n) && e.put(f.p, f.n) && e.put_u32(v.n) && e.put(v.p, v.n) &&
                        (!with_ttl ||
                         e.put_u64(static_cast<uint64_t>(ttls ? ttls->get(f) : HashFieldTtl::kNone)));
        if (!ok) {
            cursor.lane[0] = idx;
            cursor.lane[1] = e.pos;
            stopped = true;
        }
        idx++;
    });
    if (!stopped) { cursor.lane[0] = idx; cursor.lane[1] = 0; }
    cursor.offset += e.out;
    written = e.out;
    return SnapshotHookStatus::Ok;
}

SnapshotHookStatus hash_snapshot_load(Slice key, uint8_t encoding, int64_t expire_at_ms,
                                      Slice payload, const TypeLimits& limits, KvObj*& result) {
    result = nullptr;
    if (encoding > 1) return SnapshotHookStatus::Corrupt;
    const bool with_ttl = encoding == 1;
    const int64_t now_ms = snapshot_now_ms();
    uint64_t seed = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < key.n; i++)
        seed = (seed ^ static_cast<uint8_t>(key.p[i])) * 0x100000001b3ull;
    HashVal* hash = new (std::nothrow) HashVal(seed);
    if (!hash) return SnapshotHookStatus::Oom;
    CollectionRef hash_ref(hash);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(payload.p);
    uint64_t left = payload.n;
    uint32_t skipped = 0;
    while (left) {
        if (left < 4) { delete hash; return SnapshotHookStatus::Corrupt; }
        const uint32_t flen = snapshot_get_u32(p);
        p += 4; left -= 4;
        if (left < static_cast<uint64_t>(flen) + 4) { delete hash; return SnapshotHookStatus::Corrupt; }
        const Slice field(reinterpret_cast<const char*>(p), flen);
        p += flen; left -= flen;
        const uint32_t vlen = snapshot_get_u32(p);
        p += 4; left -= 4;
        if (left < vlen) { delete hash; return SnapshotHookStatus::Corrupt; }
        const Slice value(reinterpret_cast<const char*>(p), vlen);
        p += vlen; left -= vlen;
        int64_t field_expire = HashFieldTtl::kNone;
        if (with_ttl) {
            if (left < 8) { delete hash; return SnapshotHookStatus::Corrupt; }
            field_expire = static_cast<int64_t>(snapshot_get_u64(p));
            p += 8; left -= 8;
            // Absolute deadlines make this the whole of "recovery honours field TTLs": a field that
            // already lapsed is simply not loaded, exactly as a lapsed key is not.
            if (field_expire >= 0 && field_expire <= now_ms) { skipped++; continue; }
        }
        if (hash_set(hash_ref, limits.hash, field, value) == HashSetKind::Oom) {
            delete hash;
            return SnapshotHookStatus::Oom;
        }
        if (field_expire >= 0) {
            if (!hash->ttls) {
                hash->ttls = new (std::nothrow) HashFieldTtl;
                if (!hash->ttls) { delete hash; return SnapshotHookStatus::Oom; }
            }
            if (!hash->ttls->set(field, field_expire)) { delete hash; return SnapshotHookStatus::Oom; }
        }
    }
    if (hash->ttls) hash->ttl_bytes = hash->ttls->allocation_bytes();
    int64_t key_expire = expire_at_ms;
    if (skipped && !hash->entries()) {
        // Every field lapsed. A zero-field hash is not a value Redis can represent and the loaders
        // require a non-null object, so hand back one whose KEY deadline is already past: find()
        // hides it and the ordinary expire cycle collects it, with no new code path anywhere.
        key_expire = 1;
    }
    // kvobj_adopt_hash prefers the one-allocation embedded form for a small hash -- and that form
    // DELETES the HashVal, taking the field-TTL table with it (a silent TTL loss through COPY,
    // RENAME and RESTORE until it was caught). A hash carrying deadlines is external by definition.
    if (hash->ttls) {
        result = kvobj_new_hash(key, hash, key_expire);
        if (!result) { delete hash; return SnapshotHookStatus::Oom; }
        return SnapshotHookStatus::Ok;
    }
    result = kvobj_adopt_hash(key, hash, key_expire);
    if (!result) { delete hash; return SnapshotHookStatus::Oom; }
    return SnapshotHookStatus::Ok;
}

}  // namespace

// ---- the field-TTL seam (see t_hash_ttl.h) ----------------------------------------------------
// The field-TTL lane needs exactly these five things from the hash lane, and nothing else; keeping
// the surface this small is what lets the two representations stay private to this file.

HashFieldTtl** hash_ttl_slot(KvObj* object) {
    if (static_cast<Enc>(object->enc) == Enc::Compact) return nullptr;   // embedded: no HashVal
    return &static_cast<HashVal*>(object->external_ptr())->ttls;
}

void hash_ttl_note_bytes(KvObj* object) {
    if (static_cast<Enc>(object->enc) == Enc::Compact) return;
    HashVal* value = static_cast<HashVal*>(object->external_ptr());
    value->ttl_bytes = value->ttls ? value->ttls->allocation_bytes() : 0;
}

bool hash_ttl_field_exists(const KvObj* object, Slice field) {
    Slice ignored;
    return hash_get(as_hash(const_cast<KvObj*>(object)), field, ignored);
}

bool hash_ttl_field_erase(KvObj* object, Slice field) {
    CollectionRef hash = as_hash(object);
    return hash_erase(hash, field);
}

uint32_t hash_ttl_field_count(const KvObj* object) {
    return as_hash(const_cast<KvObj*>(object)).entries();
}

bool hash_ttl_externalize(Shard& shard, Op& op, KvObj*& object, bool notify) {
    return notify ? externalize_hash<true>(shard, op, object)
                  : externalize_hash<false>(shard, op, object);
}

SnapshotTypeHooks hash_snapshot_hooks() {
    return {hash_snapshot_begin, hash_snapshot_read, hash_snapshot_load};
}

CommandTable hash_command_table() {
    return {kTable, sizeof(kTable) / sizeof(kTable[0])};
}

// SORT's `BY h_*->f` / `GET h_*->f` read a field on a key the command was NOT routed on, so it may
// not go through hash_lookup(): that path reaps lapsed fields and, when the last one goes, erases
// and records the deletion under op.key() -- which here names the sorted key, not the hash. This
// is the same field read HGET performs with every write-side effect removed; the caller filters
// lapsed fields with hash_ttl_field_lapsed() instead of reaping them.
bool hash_field_value_ro(const KvObj* object, Slice field, Slice& value) {
    if (!object || !object->is_type(Type::Hash)) return false;
    return hash_get(CollectionRef(const_cast<KvObj*>(object)), field, value);
}

}  // namespace tomo

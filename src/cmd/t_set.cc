// t_set.cc — single-key Redis-compatible SET-family commands.
//
// Small integer sets keep sorted 2/4/8-byte signed values in Compact. Small mixed sets keep raw
// members in Compact plus entry offsets for O(1) random access. Both promote one-way to the
// open-addressed SetMemberTable. All objects and PRNG state are worker-local; no synchronization is
// needed on a shard-owned value.
#include "command.h"
#include "notify.h"
#include "xshard.h"
#include "../core/shard.h"
#include "../exec/op.h"
#include "../net/resp.h"
#include "../snapshot/format.h"
#include "../store/kvobj.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tomo {

void reply_maxmemory_oom(Op& op);

// ---- SetMemberTable --------------------------------------------------------------------------

uint32_t SetMemberTable::capacity_for(uint32_t entries) {
    uint32_t cap = 8;
    while (static_cast<uint64_t>(entries) * 10 >= static_cast<uint64_t>(cap) * 7) {
        if (cap > (uint32_t{1} << 30)) return 0;
        cap <<= 1;
    }
    return cap;
}

bool SetMemberTable::rehash(uint32_t capacity) {
    if (capacity < 8 || (capacity & (capacity - 1))) return false;
    std::vector<Slot> next;
    std::vector<uint32_t> next_live;
    try {
        next.resize(capacity);
        next_live.reserve(live_);
    } catch (const std::bad_alloc&) {
        return false;
    }

    const uint32_t mask = capacity - 1;
    for (uint32_t old_slot : live_slots_) {
        Slot& src = slots_[old_slot];
        uint32_t pos = static_cast<uint32_t>(mix64(src.hash)) & mask;
        while (next[pos].state == Live) pos = (pos + 1) & mask;
        Slot& dst = next[pos];
        dst.value = std::move(src.value);
        dst.hash = src.hash;
        dst.dense_pos = static_cast<uint32_t>(next_live.size());
        dst.state = Live;
        next_live.push_back(pos);
    }
    slots_ = std::move(next);
    live_slots_ = std::move(next_live);
    tombs_ = 0;
    if (++generation_ == 0) generation_ = 1;
    return true;
}

bool SetMemberTable::reserve(uint32_t entries) {
    const uint32_t wanted = capacity_for(entries);
    if (wanted == 0) return false;
    if (slots_.size() < wanted && !rehash(wanted)) return false;
    try {
        live_slots_.reserve(entries);
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

uint32_t SetMemberTable::find(Slice value, uint64_t hash) const {
    if (slots_.empty()) return npos;
    const uint32_t mask = static_cast<uint32_t>(slots_.size() - 1);
    uint32_t pos = static_cast<uint32_t>(mix64(hash)) & mask;
    for (uint32_t probes = 0; probes < slots_.size(); probes++) {
        const Slot& slot = slots_[pos];
        if (slot.state == Empty) return npos;
        if (slot.state == Live && slot.hash == hash && slot.value.size() == value.n &&
            (value.n == 0 || std::memcmp(slot.value.data(), value.p, value.n) == 0))
            return pos;
        pos = (pos + 1) & mask;
    }
    return npos;
}

uint32_t SetMemberTable::find_insert_slot(Slice value, uint64_t hash, bool& exists) const {
    exists = false;
    if (slots_.empty()) return npos;
    const uint32_t mask = static_cast<uint32_t>(slots_.size() - 1);
    uint32_t pos = static_cast<uint32_t>(mix64(hash)) & mask;
    uint32_t first_tomb = npos;
    for (uint32_t probes = 0; probes < slots_.size(); probes++) {
        const Slot& slot = slots_[pos];
        if (slot.state == Empty) return first_tomb == npos ? pos : first_tomb;
        if (slot.state == Tomb) {
            if (first_tomb == npos) first_tomb = pos;
        } else if (slot.hash == hash && slot.value.size() == value.n &&
                   (value.n == 0 || std::memcmp(slot.value.data(), value.p, value.n) == 0)) {
            exists = true;
            return pos;
        }
        pos = (pos + 1) & mask;
    }
    return first_tomb;
}

bool SetMemberTable::ensure_insert_capacity() {
    if (slots_.empty()) return rehash(8);
    const uint64_t occupied = static_cast<uint64_t>(live_) + tombs_ + 1;
    if (occupied * 10 < static_cast<uint64_t>(slots_.size()) * 7) return true;
    const bool tomb_heavy = static_cast<uint64_t>(live_ + 1) * 2 < slots_.size();
    if (tomb_heavy) return rehash(static_cast<uint32_t>(slots_.size()));
    if (slots_.size() > (uint32_t{1} << 30)) return false;
    return rehash(static_cast<uint32_t>(slots_.size() * 2));
}

SetMemberTable::InsertResult SetMemberTable::insert(Slice value, uint64_t hash) {
    if (find(value, hash) != npos) return InsertResult::Exists;

    std::string copy;
    try {
        if (value.n) copy.assign(value.p, value.n);
    } catch (const std::bad_alloc&) {
        return InsertResult::Oom;
    }
    if (!ensure_insert_capacity()) return InsertResult::Oom;
    try {
        live_slots_.reserve(static_cast<size_t>(live_) + 1);
    } catch (const std::bad_alloc&) {
        return InsertResult::Oom;
    }

    bool exists = false;
    const uint32_t pos = find_insert_slot(value, hash, exists);
    if (exists) return InsertResult::Exists;
    if (pos == npos) return InsertResult::Oom;
    Slot& slot = slots_[pos];
    if (slot.state == Tomb) tombs_--;
    slot.value = std::move(copy);
    slot.hash = hash;
    slot.dense_pos = static_cast<uint32_t>(live_slots_.size());
    slot.state = Live;
    string_capacity_bytes_ += slot.value.capacity();
    live_slots_.push_back(pos);
    live_++;
    return InsertResult::Inserted;
}

bool SetMemberTable::erase_at(uint32_t slot_index, uint32_t& erased_bytes) {
    if (slot_index >= slots_.size() || slots_[slot_index].state != Live) return false;
    Slot& slot = slots_[slot_index];
    erased_bytes = static_cast<uint32_t>(slot.value.size());
    string_capacity_bytes_ -= slot.value.capacity();
    std::string().swap(slot.value);

    const uint32_t dense_pos = slot.dense_pos;
    const uint32_t moved_slot = live_slots_.back();
    live_slots_[dense_pos] = moved_slot;
    slots_[moved_slot].dense_pos = dense_pos;
    live_slots_.pop_back();
    slot.hash = 0;
    slot.dense_pos = 0;
    slot.state = Tomb;
    live_--;
    tombs_++;
    return true;
}

bool SetMemberTable::erase(Slice value, uint64_t hash, uint32_t& erased_bytes) {
    const uint32_t pos = find(value, hash);
    return pos != npos && erase_at(pos, erased_bytes);
}

uint32_t SetMemberTable::random_slot(uint64_t random) const {
    if (live_slots_.empty()) return npos;
    return live_slots_[static_cast<size_t>(random % live_slots_.size())];
}

uint32_t SetMemberTable::slot_for_dense(uint32_t dense_index) const {
    return dense_index < live_slots_.size() ? live_slots_[dense_index] : npos;
}

bool SetMemberTable::live_at(uint32_t slot) const {
    return slot < slots_.size() && slots_[slot].state == Live;
}

Slice SetMemberTable::value_at(uint32_t slot) const {
    if (!live_at(slot)) return {};
    const std::string& value = slots_[slot].value;
    return Slice(value.data(), static_cast<uint32_t>(value.size()));
}

namespace {

enum class AddResult : uint8_t { Added, Exists, Oom };

bool parse_i64_strict(Slice s, int64_t& out) {
    if (s.n == 0 || s.n >= 32) return false;
    uint32_t pos = 0;
    bool negative = false;
    if (s.p[0] == '-') {
        negative = true;
        if (++pos == s.n) return false;
    }
    if (s.n - pos == 1 && s.p[pos] == '0') {
        out = 0;
        return !negative;
    }
    if (s.p[pos] < '1' || s.p[pos] > '9') return false;
    uint64_t value = 0;
    const uint64_t limit = negative ? (uint64_t{1} << 63) : static_cast<uint64_t>(INT64_MAX);
    for (; pos < s.n; pos++) {
        const char ch = s.p[pos];
        if (ch < '0' || ch > '9') return false;
        const uint32_t digit = static_cast<uint32_t>(ch - '0');
        if (value > (limit - digit) / 10) return false;
        value = value * 10 + digit;
    }
    out = negative ? (value == (uint64_t{1} << 63) ? INT64_MIN
                                                    : -static_cast<int64_t>(value))
                   : static_cast<int64_t>(value);
    return true;
}

void reply_not_integer(Op& op) {
    reply_err(op.sink(), "ERR value is not an integer or out of range");
}

uint64_t random64() {
    static thread_local uint64_t state = 0;
    if (state == 0) {
        const uint64_t ticks = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        state = mix64(ticks ^ static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&state)));
        if (state == 0) state = 0x9e3779b97f4a7c15ULL;
    }
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545f4914f6cdd1dULL;
}

uint64_t random_below(uint64_t bound) {
    if (bound <= 1) return 0;
    const uint64_t threshold = (uint64_t{0} - bound) % bound;
    uint64_t value;
    do { value = random64(); } while (value < threshold);
    return value % bound;
}

uint8_t width_for(int64_t value) {
    if (value >= INT16_MIN && value <= INT16_MAX) return 2;
    if (value >= INT32_MIN && value <= INT32_MAX) return 4;
    return 8;
}

void encode_integer(char* dst, uint8_t width, int64_t value) {
    if (width == 2) {
        const int16_t v = static_cast<int16_t>(value);
        std::memcpy(dst, &v, sizeof(v));
    } else if (width == 4) {
        const int32_t v = static_cast<int32_t>(value);
        std::memcpy(dst, &v, sizeof(v));
    } else {
        std::memcpy(dst, &value, sizeof(value));
    }
}

SetSmallEncoding set_small_encoding(const CollectionRef& set) {
    return set.is_embedded()
        ? static_cast<SetSmallEncoding>((set.aux0() >> 40) & 0xff)
        : set.external_as<SetVal>()->small_encoding;
}

void set_small_encoding(CollectionRef& set, SetSmallEncoding encoding) {
    if (set.is_embedded()) set.set_aux0((set.aux0() & ~(uint64_t{0xff} << 40)) |
                                        (static_cast<uint64_t>(encoding) << 40));
    else set.external_as<SetVal>()->small_encoding = encoding;
}

uint8_t set_int_width(const CollectionRef& set) {
    return set.is_embedded() ? static_cast<uint8_t>((set.aux0() >> 32) & 0xff)
                             : set.external_as<SetVal>()->int_width;
}

void set_int_width(CollectionRef& set, uint8_t width) {
    if (set.is_embedded()) set.set_aux0((set.aux0() & ~(uint64_t{0xff} << 32)) |
                                        (static_cast<uint64_t>(width) << 32));
    else set.external_as<SetVal>()->int_width = width;
}

uint32_t set_max_member_bytes(const CollectionRef& set) {
    return set.is_embedded() ? static_cast<uint32_t>(set.aux0())
                             : set.external_as<SetVal>()->max_member_bytes;
}

void set_max_member_bytes(CollectionRef& set, uint32_t bytes) {
    if (set.is_embedded()) set.set_aux0((set.aux0() & ~uint64_t{UINT32_MAX}) | bytes);
    else set.external_as<SetVal>()->max_member_bytes = bytes;
}

bool integer_at(const CollectionRef& set, uint32_t index, int64_t& out) {
    const uint8_t width = set_int_width(set);
    const uint32_t span = static_cast<uint32_t>(width) + 1;  // one-byte ULEB length
    Compact::Entry entry;
    if (!set.compact().at_offset(index * span, entry) || entry.value.n != width) return false;
    if (width == 2) {
        int16_t value;
        std::memcpy(&value, entry.value.p, sizeof(value));
        out = value;
    } else if (width == 4) {
        int32_t value;
        std::memcpy(&value, entry.value.p, sizeof(value));
        out = value;
    } else {
        std::memcpy(&out, entry.value.p, sizeof(out));
    }
    return true;
}

bool integer_search(const CollectionRef& set, int64_t wanted, uint32_t& position) {
    uint32_t lo = 0, hi = set.entries();
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        int64_t value = 0;
        if (!integer_at(set, mid, value)) return false;
        if (value < wanted) lo = mid + 1;
        else hi = mid;
    }
    position = lo;
    if (lo == set.entries()) return false;
    int64_t value = 0;
    return integer_at(set, lo, value) && value == wanted;
}

uint32_t integer_text_length(int64_t value) {
    char text[24];
    return i64_to_dec(text, value);
}

bool insert_integer_compact(CollectionRef& set, int64_t value, uint32_t position) {
    const uint8_t current_width = set_int_width(set);
    const uint8_t needed_width = std::max(current_width, width_for(value));
    if (needed_width == current_width) {
        char bytes[8];
        encode_integer(bytes, current_width, value);
        return set.insert(position * (static_cast<uint32_t>(current_width) + 1),
                          Slice(bytes, current_width));
    }

    // Width upgrades rebuild once, just like Redis intset's 16->32->64 promotion. The new Compact
    // is complete before it replaces the old one, so OOM leaves the set unchanged.
    Compact replacement;
    const uint32_t old_entries = set.entries();
    for (uint32_t out_index = 0, old_index = 0; out_index <= old_entries; out_index++) {
        int64_t current = value;
        if (out_index != position) {
            if (!integer_at(set, old_index++, current)) return false;
        }
        char bytes[8];
        encode_integer(bytes, needed_width, current);
        if (!replacement.append(Slice(bytes, needed_width))) return false;
    }
    set.replace_compact(std::move(replacement));
    set_int_width(set, needed_width);
    return true;
}

bool generic_find(const CollectionRef& set, Slice member, uint32_t& position,
                  Compact::Entry* found = nullptr) {
    uint32_t i = 0;
    for (const Compact::Entry entry : set.compact()) {
        if (entry.value == member) {
            position = i;
            if (found) *found = entry;
            return true;
        }
        i++;
    }
    return false;
}

bool convert_integer_to_generic_with(CollectionRef& set, Slice incoming) {
    Compact replacement;
    for (uint32_t i = 0; i < set.entries(); i++) {
        int64_t value = 0;
        if (!integer_at(set, i, value)) return false;
        char text[24];
        const uint32_t len = i64_to_dec(text, value);
        if (!replacement.append(Slice(text, len))) return false;
    }
    if (!replacement.append(incoming)) return false;
    if (!set.replace_compact(std::move(replacement))) return false;
    set_small_encoding(set, SetSmallEncoding::Generic);
    return true;
}

bool promote_to_table(CollectionRef& set, uint32_t reserve_entries) {
    SetMemberTable replacement;
    if (!replacement.reserve(reserve_entries)) return false;
    uint64_t logical_payload = 0;

    if (set_small_encoding(set) == SetSmallEncoding::Integer) {
        for (uint32_t i = 0; i < set.entries(); i++) {
            int64_t value = 0;
            if (!integer_at(set, i, value)) return false;
            char text[24];
            const uint32_t len = i64_to_dec(text, value);
            const Slice member(text, len);
            if (replacement.insert(member, FlatStore::hash_key(member)) !=
                SetMemberTable::InsertResult::Inserted)
                return false;
            logical_payload += len;
        }
    } else {
        for (auto entry : set.compact()) {
            if (replacement.insert(entry.value, FlatStore::hash_key(entry.value)) !=
                SetMemberTable::InsertResult::Inserted)
                return false;
            logical_payload += entry.value.n;
        }
    }
    SetVal* external = set.external_as<SetVal>();
    external->table = std::move(replacement);
    external->finish_table_promotion(logical_payload);
    return true;
}

AddResult add_to_table(CollectionRef& set, Slice member) {
    SetVal* external = set.external_as<SetVal>();
    const SetMemberTable::InsertResult result =
        external->table.insert(member, FlatStore::hash_key(member));
    if (result == SetMemberTable::InsertResult::Oom) return AddResult::Oom;
    if (result == SetMemberTable::InsertResult::Exists) return AddResult::Exists;
    external->note_expanded_insert(member.n, external->table.allocation_bytes());
    set_max_member_bytes(set, std::max(set_max_member_bytes(set), member.n));
    return AddResult::Added;
}

AddResult add_member(CollectionRef& set, Slice member, const CompactLimit& limit) {
    if (set.encoding() == CollectionEncoding::Hashtable) return add_to_table(set, member);
    if (set.entries() == std::numeric_limits<uint32_t>::max()) return AddResult::Oom;

    if (set_small_encoding(set) == SetSmallEncoding::Integer) {
        int64_t integer = 0;
        if (parse_i64_strict(member, integer)) {
            uint32_t position = 0;
            if (integer_search(set, integer, position)) return AddResult::Exists;
            const uint32_t resulting = set.entries() + 1;
            if (!set.compact_fits(limit, resulting, integer_text_length(integer))) {
                if (!promote_to_table(set, resulting)) return AddResult::Oom;
                return add_to_table(set, member);
            }
            if (!insert_integer_compact(set, integer, position)) return AddResult::Oom;
            set_max_member_bytes(set,
                std::max(set_max_member_bytes(set), integer_text_length(integer)));
            return AddResult::Added;
        }

        const uint32_t resulting = set.entries() + 1;
        const uint32_t incoming_max = std::max(set_max_member_bytes(set), member.n);
        if (set.compact_fits(limit, resulting, incoming_max)) {
            if (!convert_integer_to_generic_with(set, member)) return AddResult::Oom;
            set_max_member_bytes(set, incoming_max);
            return AddResult::Added;
        }
        if (!promote_to_table(set, resulting)) return AddResult::Oom;
        return add_to_table(set, member);
    }

    uint32_t position = 0;
    if (generic_find(set, member, position)) return AddResult::Exists;
    const uint32_t resulting = set.entries() + 1;
    if (!set.compact_fits(limit, resulting, member.n)) {
        if (!promote_to_table(set, resulting)) return AddResult::Oom;
        return add_to_table(set, member);
    }
    if (!set.append(member)) return AddResult::Oom;
    set_max_member_bytes(set, std::max(set_max_member_bytes(set), member.n));
    return AddResult::Added;
}

bool contains_member(const CollectionRef& set, Slice member) {
    if (set.encoding() == CollectionEncoding::Hashtable)
        return set.external_as<SetVal>()->table.find(member, FlatStore::hash_key(member)) !=
               SetMemberTable::npos;
    if (set_small_encoding(set) == SetSmallEncoding::Generic) {
        uint32_t position = 0;
        return generic_find(set, member, position);
    }
    int64_t integer = 0;
    if (!parse_i64_strict(member, integer)) return false;
    uint32_t position = 0;
    return integer_search(set, integer, position);
}

bool remove_member(CollectionRef& set, Slice member) {
    if (set.encoding() == CollectionEncoding::Hashtable) {
        SetVal* external = set.external_as<SetVal>();
        uint32_t erased_bytes = 0;
        if (!external->table.erase(member, FlatStore::hash_key(member), erased_bytes)) return false;
        external->note_expanded_delete(erased_bytes, external->table.allocation_bytes());
        return true;
    }

    if (set_small_encoding(set) == SetSmallEncoding::Integer) {
        int64_t integer = 0;
        if (!parse_i64_strict(member, integer)) return false;
        uint32_t position = 0;
        if (!integer_search(set, integer, position)) return false;
        Compact::Entry entry;
        const uint32_t offset = position * (static_cast<uint32_t>(set_int_width(set)) + 1);
        return set.compact().at_offset(offset, entry) && set.erase(entry);
    }

    uint32_t position = 0;
    Compact::Entry entry;
    if (!generic_find(set, member, position, &entry)) return false;
    if (!set.erase(entry)) return false;
    return true;
}

template <typename Fn>
bool with_member_at(const CollectionRef& set, uint32_t index, Fn&& fn) {
    if (set.encoding() == CollectionEncoding::Hashtable) {
        const SetMemberTable& table = set.external_as<SetVal>()->table;
        const uint32_t slot = table.slot_for_dense(index);
        if (slot == SetMemberTable::npos) return false;
        fn(table.value_at(slot));
        return true;
    }
    if (set_small_encoding(set) == SetSmallEncoding::Generic) {
        Compact::Entry entry;
        if (!set.compact().at(index, entry)) return false;
        fn(entry.value);
        return true;
    }
    int64_t integer = 0;
    if (!integer_at(set, index, integer)) return false;
    char text[24];
    const uint32_t len = i64_to_dec(text, integer);
    fn(Slice(text, len));
    return true;
}

template <typename Fn>
void for_each_member(const CollectionRef& set, Fn&& fn) {
    if (set.encoding() == CollectionEncoding::Hashtable) {
        const SetMemberTable& table = set.external_as<SetVal>()->table;
        for (uint32_t slot = 0; slot < table.slot_count(); slot++)
            if (table.live_at(slot)) fn(table.value_at(slot));
        return;
    }
    if (set_small_encoding(set) == SetSmallEncoding::Generic) {
        for (auto entry : set.compact()) fn(entry.value);
        return;
    }
    for (uint32_t i = 0; i < set.entries(); i++) {
        int64_t integer = 0;
        if (!integer_at(set, i, integer)) continue;
        char text[24];
        const uint32_t len = i64_to_dec(text, integer);
        fn(Slice(text, len));
    }
}

void reply_scan(Op& op, uint64_t cursor, const std::vector<uint32_t>& slots,
                const SetMemberTable& table) {
    reply_array_header(op.sink(), 2);
    char text[24];
    const uint32_t len = u64_to_dec(text, cursor);
    reply_bulk(op.sink(), Slice(text, len));
    reply_array_header(op.sink(), slots.size());
    for (uint32_t slot : slots) reply_bulk(op.sink(), table.value_at(slot));
}

void reply_empty_scan(Op& op) {
    reply_array_header(op.sink(), 2);
    reply_bulk(op.sink(), Slice("0", 1));
    reply_array_header(op.sink(), 0);
}

CollectionRef as_set(KvObj* object) { return CollectionRef(object); }

template <bool kNotify>
bool externalize_set(Shard& shard, Op& op, KvObj*& object) {
    CollectionRef source(object);
    if (!source.is_embedded()) return true;
    auto* value = new (std::nothrow) SetVal;
    if (!value) { reply_err(op.sink(), "ERR out of memory"); return false; }
    for (const Compact::Entry entry : source.compact()) {
        if (!value->append(entry.value)) {
            delete value;
            reply_err(op.sink(), "ERR out of memory");
            return false;
        }
    }
    value->small_encoding = set_small_encoding(source);
    value->int_width = set_int_width(source);
    value->max_member_bytes = set_max_member_bytes(source);
    KvObj* replacement = kvobj_new_set(object->key(), value,
                                       shard.store().deadline(op.hash, object),
                                       object->has_ttl_slot());
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
bool ensure_set_add_capacity(Shard& shard, Op& op, KvObj*& object) {
    if (!object) return true;
    CollectionRef set(object);
    if (!set.is_embedded()) return true;
    const CompactLimit& limit = shard.type_limits().set;
    const uint32_t hint = op.argc() - 2;
    uint32_t incoming_max = set_max_member_bytes(set);
    bool generic = set_small_encoding(set) == SetSmallEncoding::Generic;
    uint8_t width = set_int_width(set);
    uint64_t transient_integer_encoded = set.compact().encoded_bytes();
    uint32_t integer_prefix = 0;
    bool conversion_seen = generic;
    for (uint32_t i = 2; i < op.argc(); i++) {
        incoming_max = std::max(incoming_max, op.arg(i).n);
        int64_t integer = 0;
        if (!parse_i64_strict(op.arg(i), integer)) {
            generic = true;
            conversion_seen = true;
        } else {
            width = std::max(width, width_for(integer));
            if (!conversion_seen) {
                integer_prefix++;
                transient_integer_encoded = std::max(
                    transient_integer_encoded,
                    static_cast<uint64_t>(set.entries() + integer_prefix) * (width + 1));
            }
        }
    }

    uint64_t projected_encoded = 0;
    if (!generic) {
        projected_encoded = static_cast<uint64_t>(set.entries() + hint) * (width + 1);
    } else {
        if (set_small_encoding(set) == SetSmallEncoding::Generic) {
            projected_encoded = set.compact().encoded_bytes();
        } else {
            for (uint32_t i = 0; i < set.entries(); i++) {
                int64_t integer = 0;
                if (!integer_at(set, i, integer))
                    return externalize_set<kNotify>(shard, op, object);
                projected_encoded += Compact::entry_encoded_size(integer_text_length(integer));
            }
        }
        for (uint32_t i = 2; i < op.argc(); i++)
            projected_encoded += Compact::entry_encoded_size(op.arg(i).n);
    }
    // SADD applies arguments left-to-right.  Before the first non-integer converts an intset-like
    // compact set to text, integer arguments can temporarily need more bytes than the final generic
    // blob.  Account for that peak so the embedded form never reports OOM for an operation that the
    // external form can complete.
    if (set.embedded_bytes_fit(projected_encoded) &&
        set.embedded_bytes_fit(transient_integer_encoded) &&
        static_cast<uint64_t>(set.entries()) + hint <= limit.max_entries &&
        incoming_max <= limit.max_value)
        return true;
    return externalize_set<kNotify>(shard, op, object);
}

template <bool kNotify>
void cmd_sadd(Shard& shard, Op& op) {
    KvObj* object = shard.store_find<kNotify>(op.hash, op.key());
    if (object) {
        auto sink = op.sink();
        if (!obj_type_check(object, Type::Set, sink)) return;
    }

    if (!ensure_set_add_capacity<kNotify>(shard, op, object)) return;
    SetVal* owned = object ? nullptr : new (std::nothrow) SetVal;
    if (!object && !owned) {
        reply_err(op.sink(), "ERR out of memory");
        return;
    }
    CollectionRef set = object ? as_set(object) : CollectionRef(owned);
    ObjectSizeTracker size_tracker(shard.store(), object);

    const CompactLimit& limit = shard.type_limits().set;
    const uint32_t hint = op.argc() - 2;
    if (!object) {
        int64_t first_integer = 0;
        if (!parse_i64_strict(op.arg(2), first_integer)) {
            owned->small_encoding = SetSmallEncoding::Generic;
            if (hint > limit.max_entries || op.arg(2).n > limit.max_value) {
                if (!owned->table.reserve(hint)) {
                    delete owned;
                    reply_err(op.sink(), "ERR out of memory");
                    return;
                }
                owned->finish_table_promotion(0);
            }
        } else if (hint > limit.max_entries) {
            if (!owned->table.reserve(hint)) {
                delete owned;
                reply_err(op.sink(), "ERR out of memory");
                return;
            }
            owned->finish_table_promotion(0);
        }
    } else if (set.encoding() == CollectionEncoding::Compact && hint > limit.max_entries) {
        // Redis/Valkey apply the multi-add size hint before looking for duplicates. Preserve that
        // observable upgrade rule while keeping the decision O(1).
        if (!promote_to_table(set, std::max(set.entries(), hint))) {
            reply_err(op.sink(), "ERR out of memory");
            return;
        }
    }

    uint32_t added = 0;
    for (uint32_t i = 2; i < op.argc(); i++) {
        const AddResult result = add_member(set, op.arg(i), limit);
        if (result == AddResult::Oom) {
            if (!object) delete owned;
            reply_err(op.sink(), "ERR out of memory");
            return;
        }
        added += result == AddResult::Added;
    }

    if (!object) {
        object = kvobj_adopt_set(op.key(), owned);
        if (!object) {
            delete owned;
            reply_err(op.sink(), "ERR out of memory");
            return;
        }
        const FlatStore::InsertResult inserted_ = shard.store_insert<kNotify>(op.hash, object);
if (inserted_ != FlatStore::InsertResult::Inserted) {
    kvobj_free(object);
    if (inserted_ == FlatStore::InsertResult::MaxmemoryOom) reply_maxmemory_oom(op);
    else reply_err(op.sink(), "ERR keyspace insert failed");
    return;
        }
    }
    if constexpr (kNotify) if (added)
        notify_record(shard, op, NOTIFY_SET, NotifyEventId::Sadd, op.key());
    reply_int(op.sink(), added);
}

template <bool kNotify>
void cmd_srem(Shard& shard, Op& op) {
    KvObj* object = shard.store_find<kNotify>(op.hash, op.key());
    if (!object) {
        reply_int(op.sink(), 0);
        return;
    }
    auto sink = op.sink();
    if (!obj_type_check(object, Type::Set, sink)) return;
    CollectionRef set = as_set(object);
    ObjectSizeTracker size_tracker(shard.store(), object);
    uint32_t removed = 0;
    for (uint32_t i = 2; i < op.argc(); i++) removed += remove_member(set, op.arg(i));
    size_tracker.finish();                       // account the shrink before any whole-key erase
    if constexpr (kNotify) if (removed)
        notify_record(shard, op, NOTIFY_SET, NotifyEventId::Srem, op.key());
    if (set.entries() == 0) shard.store_erase<kNotify>(op.hash, op.key());
    reply_int(op.sink(), removed);
}

template <bool kNotify>
void cmd_sismember(Shard& shard, Op& op) {
    KvObj* object = shard.store_find_read<kNotify>(op.hash, op.key());
    if (!object) {
        reply_int(op.sink(), 0);
        return;
    }
    auto sink = op.sink();
    if (!obj_type_check(object, Type::Set, sink)) return;
    reply_int(op.sink(), contains_member(as_set(object), op.arg(2)) ? 1 : 0);
}

template <bool kNotify>
void cmd_smismember(Shard& shard, Op& op) {
    KvObj* object = shard.store_find_read<kNotify>(op.hash, op.key());
    if (object) {
        auto sink = op.sink();
        if (!obj_type_check(object, Type::Set, sink)) return;
    }
    reply_array_header(op.sink(), op.argc() - 2);
    for (uint32_t i = 2; i < op.argc(); i++)
        reply_int(op.sink(), object && contains_member(as_set(object), op.arg(i)) ? 1 : 0);
}

template <bool kNotify>
void cmd_scard(Shard& shard, Op& op) {
    KvObj* object = shard.store_find_read<kNotify>(op.hash, op.key());
    if (!object) {
        reply_int(op.sink(), 0);
        return;
    }
    auto sink = op.sink();
    if (!obj_type_check(object, Type::Set, sink)) return;
    reply_int(op.sink(), as_set(object).entries());
}

template <bool kNotify>
void cmd_smembers(Shard& shard, Op& op) {
    KvObj* object = shard.store_find_read<kNotify>(op.hash, op.key());
    if (!object) {
        reply_set_header(op.sink(), 0, op.resp3());
        return;
    }
    auto sink = op.sink();
    if (!obj_type_check(object, Type::Set, sink)) return;
    CollectionRef set = as_set(object);
    reply_set_header(op.sink(), set.entries(), op.resp3());
    for_each_member(set, [&](Slice member) { reply_bulk(op.sink(), member); });
}

template <bool kNotify>
void cmd_spop(Shard& shard, Op& op) {
    const bool with_count = op.argc() == 3;
    int64_t signed_count = 1;
    if (with_count) {
        // One message for both failures, as redis's getRangeLongFromObject(0, LONG_MAX, msg).
        if (!parse_i64_strict(op.arg(2), signed_count) || signed_count < 0) {
            reply_err(op.sink(), "ERR value is out of range, must be positive");
            return;
        }
    }

    KvObj* object = shard.store_find<kNotify>(op.hash, op.key());
    if (!object) {
        if (with_count) reply_set_header(op.sink(), 0, op.resp3());
        else reply_null(op.sink(), op.resp3());
        return;
    }
    auto sink = op.sink();
    if (!obj_type_check(object, Type::Set, sink)) return;
    CollectionRef initial = as_set(object);
    const uint32_t size = initial.entries();
    const uint64_t count = static_cast<uint64_t>(signed_count);
    if (count == 0) {
        reply_set_header(op.sink(), 0, op.resp3());
        return;
    }

    if (count >= size) {
        if (with_count) reply_set_header(op.sink(), size, op.resp3());
        for_each_member(initial, [&](Slice member) { reply_bulk(op.sink(), member); });
        if constexpr (kNotify)
            notify_record(shard, op, NOTIFY_SET, NotifyEventId::Spop, op.key());
        shard.store_erase<kNotify>(op.hash, op.key());
        return;
    }

    if (initial.is_embedded() && !externalize_set<kNotify>(shard, op, object)) return;
    CollectionRef set = as_set(object);
    ObjectSizeTracker size_tracker(shard.store(), object);   // partial pop: track the shrink
    // Compact deletion would move bytes proportional to the collection. Promote once (charged to
    // the preceding compact writes), then every random pick and tombstone delete is O(1).
    if (set.encoding() != CollectionEncoding::Hashtable &&
        !promote_to_table(set, size)) {
        reply_err(op.sink(), "ERR out of memory");
        return;
    }
    SetVal* expanded = set.external_as<SetVal>();
    if (with_count) reply_set_header(op.sink(), count, op.resp3());
    for (uint64_t i = 0; i < count; i++) {
        const uint32_t slot = expanded->table.random_slot(random64());
        const Slice member = expanded->table.value_at(slot);
        reply_bulk(op.sink(), member);
        uint32_t erased_bytes = 0;
        expanded->table.erase_at(slot, erased_bytes);
        expanded->note_expanded_delete(erased_bytes, expanded->table.allocation_bytes());
    }
    if constexpr (kNotify)
        notify_record(shard, op, NOTIFY_SET, NotifyEventId::Spop, op.key());
}

bool select_unique(uint32_t population, uint32_t count, std::vector<uint32_t>& picks) {
    std::unordered_set<uint32_t> selected;
    try {
        selected.reserve(count);
        picks.reserve(count);
        for (uint64_t j = static_cast<uint64_t>(population) - count; j < population; j++) {
            const uint32_t candidate = static_cast<uint32_t>(random_below(j + 1));
            if (selected.insert(candidate).second) {
                picks.push_back(candidate);
            } else {
                const uint32_t fallback = static_cast<uint32_t>(j);
                selected.insert(fallback);
                picks.push_back(fallback);
            }
        }
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

template <bool kNotify>
void cmd_srandmember(Shard& shard, Op& op) {
    const bool with_count = op.argc() == 3;
    int64_t count = 1;
    if (with_count) {
        if (!parse_i64_strict(op.arg(2), count)) {
            reply_not_integer(op);
            return;
        }
        if (count == INT64_MIN) {
            reply_outofrange(op.sink());
            return;
        }
    }

    KvObj* object = shard.store_find_read<kNotify>(op.hash, op.key());
    if (!object) {
        if (with_count) reply_array_header(op.sink(), 0);
        else reply_null(op.sink(), op.resp3());
        return;
    }
    auto sink = op.sink();
    if (!obj_type_check(object, Type::Set, sink)) return;
    CollectionRef set = as_set(object);
    const uint32_t size = set.entries();

    if (!with_count) {
        const uint32_t pick = static_cast<uint32_t>(random_below(size));
        with_member_at(set, pick, [&](Slice member) { reply_bulk(op.sink(), member); });
        return;
    }
    if (count == 0) {
        reply_array_header(op.sink(), 0);
        return;
    }
    if (count < 0) {
        const uint64_t picks = static_cast<uint64_t>(-count);
        reply_array_header(op.sink(), picks);
        for (uint64_t i = 0; i < picks; i++) {
            const uint32_t pick = static_cast<uint32_t>(random_below(size));
            with_member_at(set, pick, [&](Slice member) { reply_bulk(op.sink(), member); });
        }
        return;
    }

    const uint32_t picks_count = static_cast<uint64_t>(count) >= size
                                     ? size
                                     : static_cast<uint32_t>(count);
    if (picks_count == size) {
        reply_array_header(op.sink(), size);
        for_each_member(set, [&](Slice member) { reply_bulk(op.sink(), member); });
        return;
    }
    std::vector<uint32_t> picks;
    if (!select_unique(size, picks_count, picks)) {
        reply_err(op.sink(), "ERR out of memory");
        return;
    }
    reply_array_header(op.sink(), picks.size());
    for (uint32_t pick : picks)
        with_member_at(set, pick, [&](Slice member) { reply_bulk(op.sink(), member); });
}

struct ScanOptions {
    uint64_t count = 10;
    Slice pattern;
    bool use_pattern = false;
};

bool parse_scan_options(Op& op, ScanOptions& options) {
    for (uint32_t i = 3; i < op.argc();) {
        if (op.arg(i).eq_icase("count") && i + 1 < op.argc()) {
            int64_t count = 0;
            if (!parse_i64_strict(op.arg(i + 1), count)) {
                reply_not_integer(op);
                return false;
            }
            if (count < 1) {
                reply_syntax(op.sink());
                return false;
            }
            options.count = static_cast<uint64_t>(count);
            i += 2;
        } else if (op.arg(i).eq_icase("match") && i + 1 < op.argc()) {
            options.pattern = op.arg(i + 1);
            options.use_pattern = !(options.pattern.n == 1 && options.pattern.p[0] == '*');
            i += 2;
        } else if (op.arg(i).eq_icase("novalues")) {
            // NOVALUES is a real option that this command does not have; redis says so rather
            // than answering the generic syntax error it gives an unknown word.
            reply_err(op.sink(), "ERR NOVALUES option can only be used in HSCAN");
            return false;
        } else {
            reply_syntax(op.sink());
            return false;
        }
    }
    return true;
}

template <bool kNotify>
void cmd_sscan(Shard& shard, Op& op) {
    uint64_t cursor = 0;
    if (!command_parse_scan_cursor(op.arg(2), cursor)) {
        reply_err(op.sink(), "ERR invalid cursor");
        return;
    }
    KvObj* object = shard.store_find_read<kNotify>(op.hash, op.key());
    if (!object) {
        reply_empty_scan(op);
        return;
    }
    auto sink = op.sink();
    if (!obj_type_check(object, Type::Set, sink)) return;
    ScanOptions options;
    if (!parse_scan_options(op, options)) return;
    CollectionRef set = as_set(object);

    if (set.encoding() != CollectionEncoding::Hashtable) {
        uint64_t matches = 0;
        for_each_member(set, [&](Slice member) {
            if (!options.use_pattern || command_glob_match(options.pattern, member)) matches++;
        });
        reply_array_header(op.sink(), 2);
        reply_bulk(op.sink(), Slice("0", 1));
        reply_array_header(op.sink(), matches);
        for_each_member(set, [&](Slice member) {
            if (!options.use_pattern || command_glob_match(options.pattern, member))
                reply_bulk(op.sink(), member);
        });
        return;
    }

    const SetMemberTable& table = set.external_as<SetVal>()->table;
    uint32_t position = 0;
    const uint32_t cursor_generation = static_cast<uint32_t>(cursor >> 32);
    if (cursor != 0 && cursor_generation == table.generation())
        position = static_cast<uint32_t>(cursor);
    if (position >= table.slot_count()) position = 0;

    std::vector<uint32_t> matches;
    try {
        matches.reserve(static_cast<size_t>(std::min<uint64_t>(options.count, table.size())));
    } catch (const std::bad_alloc&) {
        reply_err(op.sink(), "ERR out of memory");
        return;
    }
    const uint64_t max_probes = options.count > std::numeric_limits<uint64_t>::max() / 10
                                    ? std::numeric_limits<uint64_t>::max()
                                    : options.count * 10;
    uint64_t probes = 0, sampled = 0;
    while (position < table.slot_count() && probes < max_probes && sampled < options.count) {
        if (table.live_at(position)) {
            sampled++;
            const Slice member = table.value_at(position);
            if (!options.use_pattern || command_glob_match(options.pattern, member))
                matches.push_back(position);
        }
        position++;
        probes++;
    }
    const uint64_t next_cursor = position == table.slot_count()
                                     ? 0
                                     : (static_cast<uint64_t>(table.generation()) << 32) | position;
    reply_scan(op, next_cursor, matches, table);
}

#define TOMO_HANDLER_PAIR(fn) fn<false>, 1, 1, 1, notify_handler<fn<true>>

static const CommandSpec kTable[] = {
    // name          min max flags                handler          first last step
    {"SADD",          3, -1, CmdFlags::Write | CmdFlags::DenyOom, TOMO_HANDLER_PAIR(cmd_sadd)},
    {"SREM",          3, -1, CmdFlags::Write,     TOMO_HANDLER_PAIR(cmd_srem)},
    {"SISMEMBER",     3,  3, CmdFlags::Readonly,  TOMO_HANDLER_PAIR(cmd_sismember)},
    {"SMISMEMBER",    3, -1, CmdFlags::Readonly,  TOMO_HANDLER_PAIR(cmd_smismember)},
    {"SCARD",         2,  2, CmdFlags::Readonly,  TOMO_HANDLER_PAIR(cmd_scard)},
    {"SMEMBERS",      2,  2, CmdFlags::Readonly,  TOMO_HANDLER_PAIR(cmd_smembers)},
    {"SPOP",          2,  3, CmdFlags::Write,     TOMO_HANDLER_PAIR(cmd_spop)},
    {"SRANDMEMBER",   2,  3, CmdFlags::Readonly,  TOMO_HANDLER_PAIR(cmd_srandmember)},
    {"SSCAN",         3, -1, CmdFlags::Readonly,  TOMO_HANDLER_PAIR(cmd_sscan)},
    {"SMOVE",         4,  4, CmdFlags::Write | CmdFlags::MultiShard,cmd_xshard_only,1,2,1},
    {"SINTER",        2, -1, CmdFlags::Readonly | CmdFlags::MultiShard,cmd_xshard_only,1,-1,1},
    {"SUNION",        2, -1, CmdFlags::Readonly | CmdFlags::MultiShard,cmd_xshard_only,1,-1,1},
    {"SDIFF",         2, -1, CmdFlags::Readonly | CmdFlags::MultiShard,cmd_xshard_only,1,-1,1},
    {"SINTERCARD",    3, -1, CmdFlags::Readonly | CmdFlags::MultiShard,cmd_xshard_only,2,-1,1},
    {"SINTERSTORE",   3, -1, CmdFlags::Write | CmdFlags::DenyOom | CmdFlags::MultiShard,cmd_xshard_only,1,-1,1},
    {"SUNIONSTORE",   3, -1, CmdFlags::Write | CmdFlags::DenyOom | CmdFlags::MultiShard,cmd_xshard_only,1,-1,1},
    {"SDIFFSTORE",    3, -1, CmdFlags::Write | CmdFlags::DenyOom | CmdFlags::MultiShard,cmd_xshard_only,1,-1,1},
};

#undef TOMO_HANDLER_PAIR

}  // namespace

XshardElementResult xshard_set_contains(KvObj* object, Slice member, bool& contains) {
    contains = false;
    if (!object) return XshardElementResult::Missing;
    if (static_cast<Type>(object->type) != Type::Set) return XshardElementResult::WrongType;
    contains = contains_member(as_set(object), member);
    return XshardElementResult::Ok;
}

namespace {

template <bool kNotify>
XshardElementResult xshard_externalize_set(Shard& shard, Slice key, uint64_t hash,
                                           KvObj*& object) {
    CollectionRef source(object);
    if (!source.is_embedded()) return XshardElementResult::Ok;
    auto* value = new (std::nothrow) SetVal;
    if (!value) return XshardElementResult::Oom;
    for (const Compact::Entry entry : source.compact()) {
        if (!value->append(entry.value)) {
            delete value;
            return XshardElementResult::Oom;
        }
    }
    value->small_encoding = set_small_encoding(source);
    value->int_width = set_int_width(source);
    value->max_member_bytes = set_max_member_bytes(source);
    KvObj* replacement = kvobj_new_set(key, value, shard.store().deadline(hash, object),
                                       object->has_ttl_slot());
    if (!replacement) {
        delete value;
        return XshardElementResult::Oom;
    }
    replacement->set_eviction_meta(object->eviction_meta());
    const FlatStore::InsertResult inserted = shard.store_insert<kNotify>(hash, replacement);
    if (inserted != FlatStore::InsertResult::Inserted) {
        kvobj_free(replacement);
        return inserted == FlatStore::InsertResult::MaxmemoryOom
            ? XshardElementResult::Maxmemory : XshardElementResult::InsertFailed;
    }
    object = replacement;
    return XshardElementResult::Ok;
}

template <bool kNotify>
XshardElementResult xshard_remove_set_element_impl(Shard& shard, Slice key, uint64_t hash,
                                                   Slice member) {
    KvObj* object = shard.store_find<kNotify>(hash, key);
    if (!object) return XshardElementResult::Missing;
    if (static_cast<Type>(object->type) != Type::Set) return XshardElementResult::WrongType;
    CollectionRef set = as_set(object);
    ObjectSizeTracker size_tracker(shard.store(), object);
    if (!remove_member(set, member)) return XshardElementResult::Missing;
    size_tracker.finish();
    if (!set.entries()) shard.store_erase<kNotify>(hash, key);
    return XshardElementResult::Ok;
}

template <bool kNotify>
XshardElementResult xshard_insert_set_element_impl(Shard& shard, Slice key, uint64_t hash,
                                                   Slice member) {
    KvObj* object = shard.store_find<kNotify>(hash, key);
    if (object && static_cast<Type>(object->type) != Type::Set)
        return XshardElementResult::WrongType;
    if (object && contains_member(as_set(object), member)) return XshardElementResult::Ok;

    // Embedded sets have fixed backing capacity. Externalize before an insertion so add_member()
    // can retain its ordinary strong OOM guarantee without reconstructing the whole set in the
    // scatter engine. The copy is bounded by the compact threshold; large sets are already O(1)
    // hashtables and never enter this arm.
    if (object && as_set(object).is_embedded()) {
        const XshardElementResult converted =
            xshard_externalize_set<kNotify>(shard, key, hash, object);
        if (converted != XshardElementResult::Ok) return converted;
    }

    SetVal* owned = object ? nullptr : new (std::nothrow) SetVal;
    if (!object && !owned) return XshardElementResult::Oom;
    CollectionRef set = object ? as_set(object) : CollectionRef(owned);
    ObjectSizeTracker size_tracker(shard.store(), object);
    const CompactLimit& limit = shard.type_limits().set;
    if (!object) {
        int64_t integer = 0;
        if (!parse_i64_strict(member, integer)) {
            owned->small_encoding = SetSmallEncoding::Generic;
            if (limit.max_entries == 0 || member.n > limit.max_value) {
                if (!owned->table.reserve(1)) {
                    delete owned;
                    return XshardElementResult::Oom;
                }
                owned->finish_table_promotion(0);
            }
        } else if (limit.max_entries == 0) {
            if (!owned->table.reserve(1)) {
                delete owned;
                return XshardElementResult::Oom;
            }
            owned->finish_table_promotion(0);
        }
    }

    if (add_member(set, member, limit) == AddResult::Oom) {
        if (!object) delete owned;
        return XshardElementResult::Oom;
    }
    if (!object) {
        KvObj* fresh = kvobj_adopt_set(key, owned);
        if (!fresh) {
            delete owned;
            return XshardElementResult::Oom;
        }
        const FlatStore::InsertResult inserted = shard.store_insert<kNotify>(hash, fresh);
        if (inserted != FlatStore::InsertResult::Inserted) {
            kvobj_free(fresh);
            return inserted == FlatStore::InsertResult::MaxmemoryOom
                ? XshardElementResult::Maxmemory : XshardElementResult::InsertFailed;
        }
    }
    return XshardElementResult::Ok;
}

}  // namespace

XshardElementResult xshard_remove_set_element(Shard& shard, Slice key, uint64_t hash,
                                              Slice member) {
    return shard.notify_carrier()
        ? xshard_remove_set_element_impl<true>(shard, key, hash, member)
        : xshard_remove_set_element_impl<false>(shard, key, hash, member);
}

XshardElementResult xshard_insert_set_element(Shard& shard, Slice key, uint64_t hash,
                                              Slice member) {
    return shard.notify_carrier()
        ? xshard_insert_set_element_impl<true>(shard, key, hash, member)
        : xshard_insert_set_element_impl<false>(shard, key, hash, member);
}


namespace {

// Logical set payload: per member [u32 len][bytes], encoding byte 0.  Load re-adds members through
// add_member, so int-compact/generic-compact/hashtable follow the CURRENT limits.
SnapshotHookStatus set_snapshot_begin(const KvObj& object, SnapshotSaveCursor& cursor,
                                      uint8_t& encoding) {
    if (static_cast<Type>(object.type) != Type::Set) return SnapshotHookStatus::Corrupt;
    cursor = {};
    cursor.object = &object;
    encoding = 0;
    uint64_t total = 0;
    for_each_member(as_set(const_cast<KvObj*>(&object)),
                    [&](Slice member) { total += 4ull + member.n; });
    cursor.total = total;
    return SnapshotHookStatus::Ok;
}

SnapshotHookStatus set_snapshot_read(SnapshotSaveCursor& cursor, uint8_t* destination,
                                     size_t capacity, size_t& written) {
    written = 0;
    if (!cursor.object) return SnapshotHookStatus::Corrupt;
    CollectionRef set = as_set(const_cast<KvObj*>(cursor.object));
    SnapshotElementEmitter e{destination, capacity};
    uint64_t idx = 0;
    bool stopped = false;
    for_each_member(set, [&](Slice member) {
        if (stopped) { idx++; return; }
        if (idx < cursor.lane[0]) { idx++; return; }
        e.pos = 0;
        e.resume = idx == cursor.lane[0] ? cursor.lane[1] : 0;
        if (!(e.put_u32(member.n) && e.put(member.p, member.n))) {
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

SnapshotHookStatus set_snapshot_load(Slice key, uint8_t encoding, int64_t expire_at_ms,
                                     Slice payload, const TypeLimits& limits, KvObj*& result) {
    result = nullptr;
    if (encoding != 0) return SnapshotHookStatus::Corrupt;
    auto* set = new (std::nothrow) SetVal;
    if (!set) return SnapshotHookStatus::Oom;
    CollectionRef set_ref(set);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(payload.p);
    uint64_t left = payload.n;
    while (left) {
        if (left < 4) { delete set; return SnapshotHookStatus::Corrupt; }
        const uint32_t len = snapshot_get_u32(p);
        p += 4; left -= 4;
        if (left < len) { delete set; return SnapshotHookStatus::Corrupt; }
        const Slice member(reinterpret_cast<const char*>(p), len);
        p += len; left -= len;
        if (add_member(set_ref, member, limits.set) == AddResult::Oom) {
            delete set;
            return SnapshotHookStatus::Oom;
        }
    }
    result = kvobj_adopt_set(key, set, expire_at_ms);
    if (!result) { delete set; return SnapshotHookStatus::Oom; }
    return SnapshotHookStatus::Ok;
}

}  // namespace

SnapshotTypeHooks set_snapshot_hooks() {
    return {set_snapshot_begin, set_snapshot_read, set_snapshot_load};
}

CommandTable set_command_table() {
    return {kTable, sizeof(kTable) / sizeof(kTable[0])};
}

}  // namespace tomo

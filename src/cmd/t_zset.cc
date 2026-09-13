#include "command.h"
#include "blocking.h"
#include "xshard.h"
#include "notify.h"
#include "t_zset.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../core/shard.h"
#include "../exec/op.h"
#include "../net/resp.h"
#include "../snapshot/format.h"

namespace tomo {

void reply_maxmemory_oom(Op& op);

namespace {

constexpr uint8_t kZslMaxLevel = 32;
constexpr uint32_t kCompactScoreBytes = sizeof(double);

struct ScoreRange {
    double min = 0;
    double max = 0;
    bool min_exclusive = false;
    bool max_exclusive = false;
};

enum class LexInfinity : int8_t { Minus = -1, Finite = 0, Plus = 1 };

struct LexBound {
    LexInfinity infinity = LexInfinity::Finite;
    Slice value;
    bool exclusive = false;
};

struct LexRange {
    LexBound min;
    LexBound max;
};

int bytes_compare(Slice a, Slice b) {
    const uint32_t common = std::min(a.n, b.n);
    const int cmp = common ? std::memcmp(a.p, b.p, common) : 0;
    if (cmp != 0) return cmp < 0 ? -1 : 1;
    if (a.n == b.n) return 0;
    return a.n < b.n ? -1 : 1;
}

int lex_bound_compare(const LexBound& a, const LexBound& b) {
    if (a.infinity != b.infinity)
        return static_cast<int>(a.infinity) < static_cast<int>(b.infinity) ? -1 : 1;
    if (a.infinity != LexInfinity::Finite) return 0;
    return bytes_compare(a.value, b.value);
}

int member_to_bound(Slice member, const LexBound& bound) {
    if (bound.infinity == LexInfinity::Minus) return 1;
    if (bound.infinity == LexInfinity::Plus) return -1;
    return bytes_compare(member, bound.value);
}

bool lex_gte_min(Slice member, const LexRange& range) {
    const int cmp = member_to_bound(member, range.min);
    return range.min.exclusive ? cmp > 0 : cmp >= 0;
}

bool lex_lte_max(Slice member, const LexRange& range) {
    const int cmp = member_to_bound(member, range.max);
    return range.max.exclusive ? cmp < 0 : cmp <= 0;
}

bool lex_range_empty(const LexRange& range) {
    const int cmp = lex_bound_compare(range.min, range.max);
    return cmp > 0 || (cmp == 0 && (range.min.exclusive || range.max.exclusive));
}

bool score_gte_min(double score, const ScoreRange& range) {
    return range.min_exclusive ? score > range.min : score >= range.min;
}

bool score_lte_max(double score, const ScoreRange& range) {
    return range.max_exclusive ? score < range.max : score <= range.max;
}

bool score_range_empty(const ScoreRange& range) {
    return range.min > range.max ||
           (range.min == range.max && (range.min_exclusive || range.max_exclusive));
}

bool parse_i64(Slice s, int64_t& out) {
    if (!s.n) return false;
    if (s.n == 1 && s.p[0] == '0') {
        out = 0;
        return true;
    }
    uint32_t pos = 0;
    bool negative = false;
    if (s.p[pos] == '-') {
        negative = true;
        if (++pos == s.n) return false;
    }
    // Redis integer arguments use canonical decimal: no plus sign and no leading zeroes.
    if (s.p[pos] < '1' || s.p[pos] > '9') return false;
    uint64_t value = 0;
    const uint64_t limit = negative ? (uint64_t{1} << 63)
                                    : static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    for (; pos < s.n; pos++) {
        const char byte = s.p[pos];
        if (byte < '0' || byte > '9') return false;
        const uint32_t digit = static_cast<uint32_t>(byte - '0');
        if (value > (limit - digit) / 10) return false;
        value = value * 10 + digit;
    }
    out = negative ? (value == (uint64_t{1} << 63)
                          ? std::numeric_limits<int64_t>::min()
                          : -static_cast<int64_t>(value))
                   : static_cast<int64_t>(value);
    return true;
}

// A score VALUE and a score RANGE BOUND are not the same grammar on redis, and treating them as
// one was the whole of this tree's numeric-acceptance divergence. A value goes through
// getDoubleFromObject, which refuses "" and " 5" and an out-of-range magnitude; a bound goes
// through zslParseRange, which is a bare strtod and accepts all three. See src/base/numeric.h.
bool parse_score_arg(Slice s, double& out) {
    return parse_double_strict(s, out);
}

bool parse_score_range(Slice min, Slice max, ScoreRange& out) {
    out = {};
    if (min.n && min.p[0] == '(') {
        out.min_exclusive = true;
        min.p++;
        min.n--;
    }
    if (max.n && max.p[0] == '(') {
        out.max_exclusive = true;
        max.p++;
        max.n--;
    }
    return parse_double_lenient(min, out.min) && parse_double_lenient(max, out.max);
}

bool parse_lex_bound(Slice input, bool is_min, LexBound& out) {
    out = {};
    if (input.n == 1 && input.p[0] == '-') {
        out.infinity = LexInfinity::Minus;
        out.exclusive = true;
        return true;
    }
    if (input.n == 1 && input.p[0] == '+') {
        out.infinity = LexInfinity::Plus;
        out.exclusive = true;
        return true;
    }
    if (!input.n || (input.p[0] != '(' && input.p[0] != '[')) return false;
    out.infinity = LexInfinity::Finite;
    out.exclusive = input.p[0] == '(';
    out.value = Slice(input.p + 1, input.n - 1);
    (void)is_min;
    return true;
}

bool parse_lex_range(Slice min, Slice max, LexRange& out) {
    return parse_lex_bound(min, true, out.min) && parse_lex_bound(max, false, out.max);
}

struct ZsetNode {
    struct Level {
        ZsetNode* forward;
        uint64_t span;
    };

    double score;
    ZsetNode* backward;
    uint32_t member_len;
    uint8_t levels;
    uint8_t reserved[3];
    Level level[1];
};

constexpr size_t node_level_offset() { return offsetof(ZsetNode, level); }

size_t node_allocation_size(uint8_t levels, uint32_t member_len) {
    return node_level_offset() + static_cast<size_t>(levels) * sizeof(ZsetNode::Level) + member_len;
}

char* node_member_ptr(ZsetNode* node) {
    return reinterpret_cast<char*>(node) + node_level_offset() +
           static_cast<size_t>(node->levels) * sizeof(ZsetNode::Level);
}

const char* node_member_ptr(const ZsetNode* node) {
    return reinterpret_cast<const char*>(node) + node_level_offset() +
           static_cast<size_t>(node->levels) * sizeof(ZsetNode::Level);
}

Slice node_member(const ZsetNode* node) {
    return Slice(node_member_ptr(node), node->member_len);
}

uint64_t member_hash(Slice member) {
    uint64_t hash = 1469598103934665603ULL;
    for (uint32_t i = 0; i < member.n; i++) {
        hash ^= static_cast<unsigned char>(member.p[i]);
        hash *= 1099511628211ULL;
    }
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33;
    return hash;
}

uint64_t random_bounded(uint64_t limit);

// A shard-local incremental open-addressed index. Resizes move only eight old slots per mutation,
// matching FlatStore's bounded-rehash shape instead of letting std::unordered_map stop one write
// to migrate the entire collection.
class ZsetMemberMap {
public:
    ZsetMemberMap() = default;
    ~ZsetMemberMap() {
        std::free(table_[0].slots);
        std::free(table_[1].slots);
    }
    ZsetMemberMap(const ZsetMemberMap&) = delete;
    ZsetMemberMap& operator=(const ZsetMemberMap&) = delete;

    bool reserve(uint64_t wanted) {
        if (table_[0].slots) return true;
        uint64_t cap = 16;
        while (cap * 7 / 10 < wanted) {
            if (cap > std::numeric_limits<uint32_t>::max() / 2) return false;
            cap *= 2;
        }
        return allocate(table_[0], static_cast<uint32_t>(cap));
    }

    ZsetNode* find(Slice member) const {
        const uint64_t hash = member_hash(member);
        if (ZsetNode* node = find_in(table_[0], hash, member)) return node;
        return find_in(table_[1], hash, member);
    }

    bool insert(ZsetNode* node) {
        rehash_step();
        if (!table_[0].slots && !reserve(1)) return false;
        if (!table_[1].slots &&
            (table_[0].live + table_[0].tombs + 1) * 10 >= table_[0].cap * 7) {
            const bool grow = table_[0].live * 20 >= table_[0].cap * 7;
            if (!start_rehash(grow ? table_[0].cap * 2 : table_[0].cap)) return false;
        }
        const Slice member = node_member(node);
        if (find(member)) return false;
        return insert_into(table_[0], member_hash(member), node);
    }

    bool erase(Slice member) {
        rehash_step();
        const uint64_t hash = member_hash(member);
        return erase_in(table_[0], hash, member) || erase_in(table_[1], hash, member);
    }

    uint64_t size() const { return table_[0].live + table_[1].live; }
    uint64_t allocation_bytes() const {
        return static_cast<uint64_t>(table_[0].cap + table_[1].cap) * sizeof(ZsetNode*);
    }

    ZsetNode* random_node() const {
        const uint64_t total = static_cast<uint64_t>(table_[0].cap) + table_[1].cap;
        if (!total || !size()) return nullptr;
        for (;;) {
            const uint64_t pos = random_bounded(total);
            const Table& table = pos < table_[0].cap ? table_[0] : table_[1];
            const uint64_t base = pos < table_[0].cap ? 0 : table_[0].cap;
            ZsetNode* node = table.slots[pos - base];
            if (node && node != tombstone()) return node;
        }
    }

    // Reverse-binary HOME cursor, the same contract FlatStore::scan and HashFieldMap::scan carry:
    // a member present for the whole iteration is emitted at least once however many times the
    // member table was rebuilt underneath it. A raw physical position cannot promise that, because
    // a rebuild relocates a node from ahead of the position to behind it. See the derivation next
    // to scan_cursor_next() in src/store/flatstore.h.
    template <typename Fn>
    uint64_t scan(uint64_t cursor, uint64_t work, Fn&& fn) const {
        if (!table_[0].slots) return 0;
        const uint64_t slot_budget = work * 10;   // see FlatStore::scan for the two budgets
        uint64_t homes = 0, slots = 0;
        do {
            if (!table_[1].slots) {
                slots += scan_home(table_[0], static_cast<uint32_t>(cursor) & table_[0].mask, fn);
                homes++;
                cursor = scan_cursor_next(cursor, table_[0].mask);
            } else if (table_[0].mask == table_[1].mask) {
                const uint32_t home = static_cast<uint32_t>(cursor) & table_[0].mask;
                slots += scan_home(table_[0], home, fn);   // separate statements: both visits
                slots += scan_home(table_[1], home, fn);   // emit, so their order must be fixed
                homes += 2;
                cursor = scan_cursor_next(cursor, table_[0].mask);
            } else {
                const Table& small = table_[0].cap < table_[1].cap ? table_[0] : table_[1];
                const Table& large = table_[0].cap < table_[1].cap ? table_[1] : table_[0];
                const uint64_t small_mask = small.mask;
                const uint64_t large_mask = large.mask;
                slots += scan_home(small, static_cast<uint32_t>(cursor & small_mask), fn);
                homes++;
                do {
                    slots += scan_home(large, static_cast<uint32_t>(cursor & large_mask), fn);
                    homes++;
                    cursor = scan_cursor_next(cursor, large_mask);
                } while (cursor & (small_mask ^ large_mask));
            }
        } while (cursor && homes < work && slots < slot_budget);
        return cursor;
    }

private:
    struct Table {
        ZsetNode** slots = nullptr;
        uint32_t cap = 0;
        uint32_t mask = 0;
        uint32_t live = 0;
        uint32_t tombs = 0;
    };

    static ZsetNode* tombstone() { return reinterpret_cast<ZsetNode*>(static_cast<uintptr_t>(1)); }

    // Every node whose HOME slot is `home`, wherever linear probing placed it. They all lie in the
    // run of occupied slots starting at `home`, because an insert stops at the first null and
    // nothing writes a null back over an occupied slot (erase leaves a tombstone). Returns slots
    // examined, never zero, so a sparse table cannot walk itself out inside one call.
    template <typename Fn>
    static uint64_t scan_home(const Table& table, uint32_t home, Fn& fn) {
        uint64_t examined = 0;
        uint32_t slot = home;
        for (uint32_t probes = 0; probes <= table.cap; probes++) {
            ZsetNode* node = table.slots[slot];
            if (!node) break;                                 // free slot — end of the run
            examined++;
            if (node != tombstone() &&
                (static_cast<uint32_t>(member_hash(node_member(node))) & table.mask) == home)
                fn(node);
            slot = (slot + 1) & table.mask;
        }
        return examined ? examined : 1;
    }

    static bool allocate(Table& table, uint32_t cap) {
        auto** slots = static_cast<ZsetNode**>(std::calloc(cap, sizeof(ZsetNode*)));
        if (!slots) return false;
        table = {slots, cap, cap - 1, 0, 0};
        return true;
    }

    static ZsetNode* find_in(const Table& table, uint64_t hash, Slice member) {
        if (!table.slots) return nullptr;
        uint32_t slot = static_cast<uint32_t>(hash) & table.mask;
        for (uint32_t probes = 0; probes <= table.cap; probes++) {
            ZsetNode* node = table.slots[slot];
            if (!node) return nullptr;
            if (node != tombstone() && node_member(node) == member) return node;
            slot = (slot + 1) & table.mask;
        }
        return nullptr;
    }

    static bool insert_into(Table& table, uint64_t hash, ZsetNode* node) {
        uint32_t slot = static_cast<uint32_t>(hash) & table.mask;
        int32_t first_tomb = -1;
        for (uint32_t probes = 0; probes <= table.cap; probes++) {
            ZsetNode* current = table.slots[slot];
            if (!current) {
                const uint32_t target = first_tomb >= 0 ? static_cast<uint32_t>(first_tomb) : slot;
                table.slots[target] = node;
                table.live++;
                if (first_tomb >= 0) table.tombs--;
                return true;
            }
            if (current == tombstone() && first_tomb < 0) first_tomb = static_cast<int32_t>(slot);
            slot = (slot + 1) & table.mask;
        }
        return false;
    }

    static bool erase_in(Table& table, uint64_t hash, Slice member) {
        if (!table.slots) return false;
        uint32_t slot = static_cast<uint32_t>(hash) & table.mask;
        for (uint32_t probes = 0; probes <= table.cap; probes++) {
            ZsetNode* node = table.slots[slot];
            if (!node) return false;
            if (node != tombstone() && node_member(node) == member) {
                table.slots[slot] = tombstone();
                table.live--;
                table.tombs++;
                return true;
            }
            slot = (slot + 1) & table.mask;
        }
        return false;
    }

    bool start_rehash(uint32_t cap) {
        Table fresh;
        if (!allocate(fresh, cap)) return false;
        table_[1] = table_[0];
        table_[0] = fresh;
        rehash_position_ = 0;
        return true;
    }

    void rehash_step() {
        if (!table_[1].slots) return;
        uint32_t work = 8;
        while (work && rehash_position_ < table_[1].cap) {
            ZsetNode* node = table_[1].slots[rehash_position_];
            if (node && node != tombstone()) {
                table_[1].slots[rehash_position_] = tombstone();
                table_[1].live--;
                table_[1].tombs++;
                insert_into(table_[0], member_hash(node_member(node)), node);
            }
            rehash_position_++;
            work--;
        }
        if (rehash_position_ == table_[1].cap) {
            std::free(table_[1].slots);
            table_[1] = {};
            rehash_position_ = 0;
        }
    }

    Table table_[2];
    uint32_t rehash_position_ = 0;
};

uint64_t next_random() {
    thread_local uint64_t state = 0x6a09e667f3bcc909ULL;
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

uint64_t random_bounded(uint64_t limit) {
    if (limit <= 1) return 0;
    const uint64_t threshold = static_cast<uint64_t>(-limit) % limit;
    for (;;) {
        const uint64_t value = next_random();
        if (value >= threshold) return value % limit;
    }
}

uint8_t random_level() {
    uint8_t level = 1;
    while (level < kZslMaxLevel && (next_random() & 0xffffu) < 0x4000u) level++;
    return level;
}

int node_key_compare(double score, Slice member, const ZsetNode* node) {
    if (score < node->score) return -1;
    if (score > node->score) return 1;
    return bytes_compare(member, node_member(node));
}

struct RemovalResult {
    uint32_t count = 0;
    uint64_t payload = 0;
};

}  // namespace

// The expanded value mirrors Redis/fork's skiplist+member dictionary. Dictionary keys are views
// into the node's embedded member, so each member is owned exactly once.
struct ZsetData {
    ZsetNode* header = nullptr;
    ZsetNode* tail = nullptr;
    std::array<ZsetNode*, kZslMaxLevel> level_tail{};
    uint64_t length = 0;
    uint64_t node_bytes = 0;
    uint8_t level = 1;
    ZsetMemberMap by_member;

    static ZsetData* create() {
        auto* data = new (std::nothrow) ZsetData;
        if (!data) return nullptr;
        data->header = data->allocate_node(kZslMaxLevel, 0, Slice{});
        if (!data->header) {
            delete data;
            return nullptr;
        }
        return data;
    }

    ~ZsetData() {
        ZsetNode* node = header ? header->level[0].forward : nullptr;
        while (node) {
            ZsetNode* next = node->level[0].forward;
            free_node(node);
            node = next;
        }
        if (header) free_node(header);
    }

    bool reserve(uint64_t count) {
        return by_member.reserve(count);
    }

    uint64_t allocation_bytes() const {
        return sizeof(*this) + node_bytes + by_member.allocation_bytes();
    }

    ZsetNode* find(Slice member) const {
        return by_member.find(member);
    }

    ZsetNode* insert(double score, Slice member) {
        const uint8_t node_level = random_level();
        ZsetNode* node = allocate_node(node_level, score, member);
        if (!node) return nullptr;
        if (!by_member.insert(node)) {
            free_node(node);
            return nullptr;
        }

        if (!tail || node_key_compare(score, member, tail) > 0) insert_at_tail(node);
        else insert_node(node);
        return node;
    }

    void update_score(ZsetNode* node, double score) {
        if (node->score == score) return;
        ZsetNode* update[kZslMaxLevel];
        find_update(node->score, node_member(node), update);
        unlink_node(node, update);
        node->score = score;
        insert_node(node);
    }

    uint32_t erase_node(ZsetNode* node) {
        const uint32_t payload = node->member_len + kCompactScoreBytes;
        ZsetNode* update[kZslMaxLevel];
        find_update(node->score, node_member(node), update);
        by_member.erase(node_member(node));
        unlink_node(node, update);
        free_node(node);
        return payload;
    }

    uint64_t rank_of(const ZsetNode* target) const {
        uint64_t rank = 0;
        ZsetNode* node = header;
        const Slice member = node_member(target);
        for (int i = level - 1; i >= 0; i--) {
            while (node->level[i].forward &&
                   node_key_compare(target->score, member, node->level[i].forward) >= 0) {
                rank += node->level[i].span;
                node = node->level[i].forward;
            }
            if (node == target) return rank;
        }
        return 0;
    }

    ZsetNode* by_rank(uint64_t rank) const {
        if (rank == 0 || rank > length) return nullptr;
        uint64_t traversed = 0;
        ZsetNode* node = header;
        for (int i = level - 1; i >= 0; i--) {
            while (node->level[i].forward && traversed + node->level[i].span <= rank) {
                traversed += node->level[i].span;
                node = node->level[i].forward;
            }
            if (traversed == rank) return node;
        }
        return nullptr;
    }

    ZsetNode* first_by_score(const ScoreRange& range, uint64_t* rank_out = nullptr) const {
        if (score_range_empty(range)) return nullptr;
        uint64_t rank = 0;
        ZsetNode* node = header;
        for (int i = level - 1; i >= 0; i--) {
            while (node->level[i].forward &&
                   !score_gte_min(node->level[i].forward->score, range)) {
                rank += node->level[i].span;
                node = node->level[i].forward;
            }
        }
        node = node->level[0].forward;
        if (!node || !score_lte_max(node->score, range)) return nullptr;
        if (rank_out) *rank_out = rank + 1;
        return node;
    }

    ZsetNode* last_by_score(const ScoreRange& range, uint64_t* rank_out = nullptr) const {
        if (score_range_empty(range)) return nullptr;
        uint64_t rank = 0;
        ZsetNode* node = header;
        for (int i = level - 1; i >= 0; i--) {
            while (node->level[i].forward &&
                   score_lte_max(node->level[i].forward->score, range)) {
                rank += node->level[i].span;
                node = node->level[i].forward;
            }
        }
        if (node == header || !score_gte_min(node->score, range)) return nullptr;
        if (rank_out) *rank_out = rank;
        return node;
    }

    ZsetNode* first_by_lex(const LexRange& range, uint64_t* rank_out = nullptr) const {
        if (lex_range_empty(range)) return nullptr;
        uint64_t rank = 0;
        ZsetNode* node = header;
        for (int i = level - 1; i >= 0; i--) {
            while (node->level[i].forward &&
                   !lex_gte_min(node_member(node->level[i].forward), range)) {
                rank += node->level[i].span;
                node = node->level[i].forward;
            }
        }
        node = node->level[0].forward;
        if (!node || !lex_lte_max(node_member(node), range)) return nullptr;
        if (rank_out) *rank_out = rank + 1;
        return node;
    }

    ZsetNode* last_by_lex(const LexRange& range, uint64_t* rank_out = nullptr) const {
        if (lex_range_empty(range)) return nullptr;
        uint64_t rank = 0;
        ZsetNode* node = header;
        for (int i = level - 1; i >= 0; i--) {
            while (node->level[i].forward &&
                   lex_lte_max(node_member(node->level[i].forward), range)) {
                rank += node->level[i].span;
                node = node->level[i].forward;
            }
        }
        if (node == header || !lex_gte_min(node_member(node), range)) return nullptr;
        if (rank_out) *rank_out = rank;
        return node;
    }

    uint64_t count_by_score(const ScoreRange& range) const {
        uint64_t first_rank = 0, last_rank = 0;
        if (!first_by_score(range, &first_rank) || !last_by_score(range, &last_rank)) return 0;
        return last_rank >= first_rank ? last_rank - first_rank + 1 : 0;
    }

    uint64_t count_by_lex(const LexRange& range) const {
        uint64_t first_rank = 0, last_rank = 0;
        if (!first_by_lex(range, &first_rank) || !last_by_lex(range, &last_rank)) return 0;
        return last_rank >= first_rank ? last_rank - first_rank + 1 : 0;
    }

    RemovalResult erase_rank_range(uint64_t first_rank, uint64_t last_rank) {
        RemovalResult result;
        if (!first_rank || first_rank > last_rank || first_rank > length) return result;
        last_rank = std::min(last_rank, length);

        ZsetNode* update[kZslMaxLevel];
        uint64_t traversed = 0;
        ZsetNode* node = header;
        for (int i = level - 1; i >= 0; i--) {
            while (node->level[i].forward && traversed + node->level[i].span < first_rank) {
                traversed += node->level[i].span;
                node = node->level[i].forward;
            }
            update[i] = node;
        }
        traversed++;
        node = node->level[0].forward;
        while (node && traversed <= last_rank) {
            ZsetNode* next = node->level[0].forward;
            result.count++;
            result.payload += node->member_len + kCompactScoreBytes;
            by_member.erase(node_member(node));
            unlink_node(node, update);
            free_node(node);
            node = next;
            traversed++;
        }
        return result;
    }

    RemovalResult erase_score_range(const ScoreRange& range) {
        return erase_ordered_range(
            [&](const ZsetNode* node) { return score_gte_min(node->score, range); },
            [&](const ZsetNode* node) { return score_lte_max(node->score, range); },
            score_range_empty(range));
    }

    RemovalResult erase_lex_range(const LexRange& range) {
        return erase_ordered_range(
            [&](const ZsetNode* node) { return lex_gte_min(node_member(node), range); },
            [&](const ZsetNode* node) { return lex_lte_max(node_member(node), range); },
            lex_range_empty(range));
    }

private:
    ZsetNode* allocate_node(uint8_t levels, double score, Slice member) {
        const size_t bytes = node_allocation_size(levels, member.n);
        void* allocation = ::operator new(bytes, std::nothrow);
        if (!allocation) return nullptr;
        auto* node = static_cast<ZsetNode*>(allocation);
        node->score = score;
        node->backward = nullptr;
        node->member_len = member.n;
        node->levels = levels;
        node->reserved[0] = node->reserved[1] = node->reserved[2] = 0;
        for (uint8_t i = 0; i < levels; i++) node->level[i] = {nullptr, 0};
        if (member.n) std::memcpy(node_member_ptr(node), member.p, member.n);
        node_bytes += bytes;
        return node;
    }

    void free_node(ZsetNode* node) {
        node_bytes -= node_allocation_size(node->levels, node->member_len);
        ::operator delete(node);
    }

    void find_update(double score, Slice member, ZsetNode** update) const {
        ZsetNode* node = header;
        for (int i = level - 1; i >= 0; i--) {
            while (node->level[i].forward &&
                   node_key_compare(score, member, node->level[i].forward) > 0) {
                node = node->level[i].forward;
            }
            update[i] = node;
        }
    }

    void insert_node(ZsetNode* node) {
        ZsetNode* update[kZslMaxLevel];
        uint64_t rank[kZslMaxLevel];
        ZsetNode* cursor = header;
        const Slice member = node_member(node);
        for (int i = level - 1; i >= 0; i--) {
            rank[i] = (i == level - 1) ? 0 : rank[i + 1];
            while (cursor->level[i].forward &&
                   node_key_compare(node->score, member, cursor->level[i].forward) > 0) {
                rank[i] += cursor->level[i].span;
                cursor = cursor->level[i].forward;
            }
            update[i] = cursor;
        }
        if (node->levels > level) {
            for (uint8_t i = level; i < node->levels; i++) {
                rank[i] = 0;
                update[i] = header;
                header->level[i].span = length;
            }
            level = node->levels;
        }
        for (uint8_t i = 0; i < node->levels; i++) {
            node->level[i].forward = update[i]->level[i].forward;
            update[i]->level[i].forward = node;
            node->level[i].span = update[i]->level[i].span - (rank[0] - rank[i]);
            update[i]->level[i].span = (rank[0] - rank[i]) + 1;
            if (!node->level[i].forward) level_tail[i] = node;
        }
        for (uint8_t i = node->levels; i < level; i++) update[i]->level[i].span++;
        node->backward = update[0] == header ? nullptr : update[0];
        if (node->level[0].forward) node->level[0].forward->backward = node;
        else tail = node;
        length++;
    }

    void insert_at_tail(ZsetNode* node) {
        const uint8_t old_level = level;
        ZsetNode* old_tail = tail;
        for (uint8_t i = 0; i < node->levels; i++) node->level[i] = {nullptr, 0};
        for (uint8_t i = 0; i < old_level; i++) {
            ZsetNode* update = level_tail[i] ? level_tail[i] : header;
            update->level[i].span++;
            if (i < node->levels) {
                update->level[i].forward = node;
                level_tail[i] = node;
            }
        }
        for (uint8_t i = old_level; i < node->levels; i++) {
            header->level[i].forward = node;
            header->level[i].span = length + 1;
            level_tail[i] = node;
        }
        if (node->levels > old_level) level = node->levels;
        node->backward = old_tail;
        tail = node;
        length++;
    }

    void unlink_node(ZsetNode* node, ZsetNode** update) {
        for (uint8_t i = 0; i < level; i++) {
            if (update[i]->level[i].forward == node) {
                update[i]->level[i].span += node->level[i].span - 1;
                update[i]->level[i].forward = node->level[i].forward;
                if (level_tail[i] == node) level_tail[i] = update[i] == header ? nullptr : update[i];
            } else {
                update[i]->level[i].span--;
            }
        }
        if (node->level[0].forward) node->level[0].forward->backward = node->backward;
        else tail = node->backward;
        while (level > 1 && !header->level[level - 1].forward) {
            header->level[level - 1].span = 0;
            level_tail[level - 1] = nullptr;
            level--;
        }
        length--;
    }

    template <typename AtOrAfter, typename AtOrBefore>
    RemovalResult erase_ordered_range(AtOrAfter&& at_or_after, AtOrBefore&& at_or_before,
                                      bool empty_range) {
        RemovalResult result;
        if (empty_range || !length) return result;
        ZsetNode* update[kZslMaxLevel];
        ZsetNode* node = header;
        for (int i = level - 1; i >= 0; i--) {
            while (node->level[i].forward && !at_or_after(node->level[i].forward))
                node = node->level[i].forward;
            update[i] = node;
        }
        node = node->level[0].forward;
        while (node && at_or_before(node)) {
            ZsetNode* next = node->level[0].forward;
            result.count++;
            result.payload += node->member_len + kCompactScoreBytes;
            by_member.erase(node_member(node));
            unlink_node(node, update);
            free_node(node);
            node = next;
        }
        return result;
    }
};

ZsetVal::~ZsetVal() { delete expanded; }

namespace {

bool compact_decode(const Compact::Entry& entry, double& score, Slice& member) {
    if (entry.value.n < kCompactScoreBytes) return false;
    member = Slice(entry.value.p, entry.value.n - kCompactScoreBytes);
    std::memcpy(&score, entry.value.p + member.n, sizeof(score));
    return true;
}

// Redis's listpack zset holds each score as decimal TEXT, and the text it prints for a zero
// carries no sign: a -0 written into a listpack reads back as +0, while the skiplist keeps the
// raw double and preserves the sign bit. Probed on the 7.4.2 oracle:
//     ZADD k -0 m ; ZSCORE k m  ->  "0"  when k is listpack,  "-0"  when k is skiplist
// and the sign survives a later listpack->skiplist promotion only if it was never in a listpack.
// The compact form IS our listpack, so the sign is dropped HERE -- the single point at which a
// score enters compact storage (both the insert and the update call sites) -- and nowhere else.
// It is deliberately not done in reply_double: the oracle prints "-0" for a genuine -0, so the
// sign is data, not formatting. A 4000-value fuzz of the oracle's listpack round-trip found zero
// other lossy doubles, so -0 is the only value this touches.
//
// -0 and +0 compare equal, so the ordered-insert scan and the "score == old_score" no-op test are
// unaffected by normalizing after they run.
bool make_compact_tuple(double score, Slice member, std::string& tuple) {
    if (score == 0) score = 0.0;
    try {
        tuple.resize(kCompactScoreBytes + member.n);
    } catch (const std::bad_alloc&) {
        return false;
    }
    if (member.n) std::memcpy(tuple.data(), member.p, member.n);
    std::memcpy(tuple.data() + member.n, &score, sizeof(score));
    return true;
}

int tuple_compare(double score, Slice member, double other_score, Slice other_member) {
    if (score < other_score) return -1;
    if (score > other_score) return 1;
    return bytes_compare(member, other_member);
}

bool compact_find(const CollectionRef& value, Slice wanted, Compact::Entry* found,
                  double* score_out) {
    for (const Compact::Entry entry : value.compact()) {
        double score;
        Slice member;
        if (!compact_decode(entry, score, member)) continue;
        if (member == wanted) {
            if (found) *found = entry;
            if (score_out) *score_out = score;
            return true;
        }
    }
    return false;
}

uint32_t compact_insert_offset(const CollectionRef& value, double score, Slice member) {
    for (const Compact::Entry entry : value.compact()) {
        double other_score;
        Slice other_member;
        if (compact_decode(entry, other_score, other_member) &&
            tuple_compare(score, member, other_score, other_member) < 0)
            return value.compact().logical(entry);
    }
    return static_cast<uint32_t>(value.compact().encoded_bytes());
}

ZsetData* zset_expanded(const CollectionRef& value) {
    return value.external_as<ZsetVal>()->expanded;
}

bool promote_zset(CollectionRef& value) {
    ZsetData* data = ZsetData::create();
    if (!data || !data->reserve(value.entries())) {
        delete data;
        return false;
    }
    for (const Compact::Entry entry : value.compact()) {
        double score;
        Slice member;
        if (!compact_decode(entry, score, member) || !data->insert(score, member)) {
            delete data;
            return false;
        }
    }
    ZsetVal* external = value.external_as<ZsetVal>();
    external->expanded = data;
    external->promote(CollectionEncoding::Btree, data->allocation_bytes());
    return true;
}

bool zset_get_score(const CollectionRef& value, Slice member, double& score) {
    if (value.encoding() == CollectionEncoding::Compact)
        return compact_find(value, member, nullptr, &score);
    ZsetNode* node = zset_expanded(value)->find(member);
    if (!node) return false;
    score = node->score;
    return true;
}

enum class AddOutcome : uint8_t { Added, Updated, Unchanged, Noop, Oom, Nan };

AddOutcome zset_add_one(CollectionRef& value, const CompactLimit& limit, double input_score,
                        Slice member, bool nx, bool xx, bool gt, bool lt, bool incr,
                        double& resulting_score) {
    if (value.encoding() == CollectionEncoding::Compact) {
        Compact::Entry existing;
        double old_score = 0;
        if (compact_find(value, member, &existing, &old_score)) {
            if (nx) return AddOutcome::Noop;
            double score = incr ? old_score + input_score : input_score;
            if (std::isnan(score)) return AddOutcome::Nan;
            if ((lt && score >= old_score) || (gt && score <= old_score)) return AddOutcome::Noop;
            resulting_score = score;
            if (score == old_score) return AddOutcome::Unchanged;

            std::string tuple;
            if (!make_compact_tuple(score, member, tuple)) return AddOutcome::Oom;
            if (!value.erase(existing)) return AddOutcome::Oom;
            const uint32_t offset = compact_insert_offset(value, score, member);
            // Erasing retained enough vector capacity for this same-sized tuple, so this insertion
            // cannot allocate. Keeping the bool check still protects against malformed offsets.
            if (!value.insert(offset, Slice(tuple.data(), static_cast<uint32_t>(tuple.size()))))
                return AddOutcome::Oom;
            return AddOutcome::Updated;
        }
        if (xx) return AddOutcome::Noop;
        resulting_score = input_score;
        if (!value.compact_fits(limit, value.entries() + 1, member.n)) {
            if (!promote_zset(value)) return AddOutcome::Oom;
        } else {
            std::string tuple;
            if (!make_compact_tuple(input_score, member, tuple)) return AddOutcome::Oom;
            const uint32_t offset = compact_insert_offset(value, input_score, member);
            if (!value.insert(offset, Slice(tuple.data(), static_cast<uint32_t>(tuple.size()))))
                return AddOutcome::Oom;
            return AddOutcome::Added;
        }
    }

    ZsetVal* external = value.external_as<ZsetVal>();
    ZsetNode* existing = external->expanded->find(member);
    if (existing) {
        if (nx) return AddOutcome::Noop;
        const double old_score = existing->score;
        const double score = incr ? old_score + input_score : input_score;
        if (std::isnan(score)) return AddOutcome::Nan;
        if ((lt && score >= old_score) || (gt && score <= old_score)) return AddOutcome::Noop;
        resulting_score = score;
        if (score == old_score) return AddOutcome::Unchanged;
        external->expanded->update_score(existing, score);
        return AddOutcome::Updated;
    }
    if (xx) return AddOutcome::Noop;
    resulting_score = input_score;
    if (!external->expanded->insert(input_score, member)) return AddOutcome::Oom;
    external->note_expanded_insert(member.n + kCompactScoreBytes,
                                   external->expanded->allocation_bytes());
    return AddOutcome::Added;
}

template <bool kNotify>
KvObj* lookup_zset(Shard& shard, Op& op) {
    KvObj* object = shard.store_find<kNotify>(op.hash, op.key());
    if (!obj_type_check(object, Type::Zset, op.sink())) return reinterpret_cast<KvObj*>(-1);
    return object;
}

CollectionRef zset_value(KvObj* object) { return CollectionRef(object); }

void reply_oom(Op& op) { reply_err(op.sink(), "ERR out of memory"); }

void reply_invalid_integer(Op& op) {
    reply_err(op.sink(), "ERR value is not an integer or out of range");
}

void reply_cursor(Op& op, uint64_t cursor) {
    char text[24];
    const uint32_t len = u64_to_dec(text, cursor);
    reply_bulk(op.sink(), Slice(text, len));
}

struct CompactItems {
    std::vector<Compact::Entry> entries;

    bool load(const CollectionRef& value) {
        try {
            entries.reserve(value.entries());
            for (const Compact::Entry entry : value.compact()) entries.push_back(entry);
            return true;
        } catch (const std::bad_alloc&) {
            entries.clear();
            return false;
        }
    }
};

template <bool kNotify>
bool externalize_zset(Shard& shard, Op& op, KvObj*& object) {
    CollectionRef source(object);
    if (!source.is_embedded()) return true;
    auto* value = new (std::nothrow) ZsetVal;
    if (!value) { reply_oom(op); return false; }
    for (const Compact::Entry entry : source.compact()) {
        if (!value->append(entry.value)) {
            delete value;
            reply_oom(op);
            return false;
        }
    }
    KvObj* replacement = kvobj_new_zset(object->key(), value, object->expire_at_ms());
    if (!replacement) {
        delete value;
        reply_oom(op);
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
bool ensure_zset_write_capacity(Shard& shard, Op& op, KvObj*& object,
                                uint32_t additional_entries, uint64_t additional_encoded,
                                uint32_t incoming_max) {
    if (!object) return true;
    CollectionRef value(object);
    if (!value.is_embedded()) return true;
    const CompactLimit& limit = shard.type_limits().zset;
    if (value.embedded_bytes_fit(value.compact().encoded_bytes() + additional_encoded) &&
        static_cast<uint64_t>(value.entries()) + additional_entries <= limit.max_entries &&
        incoming_max <= limit.max_value)
        return true;
    return externalize_zset<kNotify>(shard, op, object);
}

void emit_item(Op& op, Slice member, double score, bool withscores, bool nested = true) {
    if (withscores && nested && op.resp3()) reply_array_header(op.sink(), 2);
    reply_bulk(op.sink(), member);
    if (withscores) reply_double(op.sink(), score, op.resp3());
}

uint64_t scored_array_length(uint64_t count, bool withscores, bool resp3) {
    return count * (withscores && !resp3 ? 2 : 1);
}

struct ParsedScoreMember {
    double score;
    Slice member;
};

template <bool kNotify>
void cmd_zadd_generic(Shard& shard, Op& op, bool force_incr) {
    bool nx = false, xx = false, gt = false, lt = false, ch = false;
    bool incr = force_incr;
    uint32_t score_index = 2;
    if (!force_incr) {
        while (score_index < op.argc()) {
            const Slice option = op.arg(score_index);
            if (option.eq_icase("nx")) nx = true;
            else if (option.eq_icase("xx")) xx = true;
            else if (option.eq_icase("gt")) gt = true;
            else if (option.eq_icase("lt")) lt = true;
            else if (option.eq_icase("ch")) ch = true;
            else if (option.eq_icase("incr")) incr = true;
            else break;
            score_index++;
        }
    }

    const uint32_t remaining = op.argc() - score_index;
    if (!remaining || (remaining & 1)) {
        reply_syntax(op.sink());
        return;
    }
    const uint32_t pair_count = remaining / 2;
    if (nx && xx) {
        reply_err(op.sink(), "ERR XX and NX options at the same time are not compatible");
        return;
    }
    if ((gt && nx) || (lt && nx) || (gt && lt)) {
        reply_err(op.sink(), "ERR GT, LT, and/or NX options at the same time are not compatible");
        return;
    }
    if (incr && pair_count != 1) {
        reply_err(op.sink(), "ERR INCR option supports a single increment-element pair");
        return;
    }

    std::vector<ParsedScoreMember> pairs;
    try {
        pairs.reserve(pair_count);
    } catch (const std::bad_alloc&) {
        reply_oom(op);
        return;
    }
    for (uint32_t i = 0; i < pair_count; i++) {
        double score;
        if (!parse_score_arg(op.arg(score_index + i * 2), score)) {
            reply_err(op.sink(), "ERR value is not a valid float");
            return;
        }
        pairs.push_back({score, op.arg(score_index + i * 2 + 1)});
    }

    KvObj* object = lookup_zset<kNotify>(shard, op);
    if (object == reinterpret_cast<KvObj*>(-1)) return;
    if (!object && xx) {
        if (incr) reply_null(op.sink(), op.resp3());
        else reply_int(op.sink(), 0);
        return;
    }

    const bool new_key = object == nullptr;
    uint64_t additional_encoded = 0;
    uint32_t incoming_max = 0;
    for (const ParsedScoreMember& pair : pairs) {
        additional_encoded += Compact::entry_encoded_size(kCompactScoreBytes + pair.member.n);
        incoming_max = std::max(incoming_max, pair.member.n);
    }
    if (!ensure_zset_write_capacity<kNotify>(shard, op, object, pair_count,
                                    additional_encoded, incoming_max)) return;
    ZsetVal* owned = new_key ? new (std::nothrow) ZsetVal : nullptr;
    if (new_key && !owned) {
        reply_oom(op);
        return;
    }
    CollectionRef value = new_key ? CollectionRef(owned) : zset_value(object);
    ObjectSizeTracker size_tracker(shard.store(), object);

    uint64_t added = 0, updated = 0, processed = 0;
    double last_score = 0;
    for (const ParsedScoreMember& pair : pairs) {
        const AddOutcome outcome = zset_add_one(value, shard.type_limits().zset, pair.score,
                                                pair.member, nx, xx, gt, lt, incr, last_score);
        if (outcome == AddOutcome::Oom || outcome == AddOutcome::Nan) {
            if (new_key) delete owned;
            if (outcome == AddOutcome::Oom) reply_oom(op);
            else reply_err(op.sink(), "ERR resulting score is not a number (NaN)");
            return;
        }
        if (outcome == AddOutcome::Added) added++;
        if (outcome == AddOutcome::Updated) updated++;
        if (outcome != AddOutcome::Noop) processed++;
    }

    if (new_key && value.entries()) {
        KvObj* header = kvobj_adopt_zset(op.key(), owned);
        if (!header) {
            delete owned;
            reply_oom(op);
            return;
        }
        const FlatStore::InsertResult inserted_ = shard.store_insert<kNotify>(op.hash, header);
if (inserted_ != FlatStore::InsertResult::Inserted) {
    kvobj_free(header);
    if (inserted_ == FlatStore::InsertResult::MaxmemoryOom) reply_maxmemory_oom(op);
    else reply_err(op.sink(), "ERR keyspace insert failed");
    return;
        }
    } else if (new_key) {
        delete owned;
    }

    if (incr) {
        if (processed) reply_double(op.sink(), last_score, op.resp3());
        else reply_null(op.sink(), op.resp3());
    } else {
        reply_int(op.sink(), static_cast<long long>(ch ? added + updated : added));
    }
    if constexpr (kNotify) if (processed)
        notify_record(shard, op, NOTIFY_ZSET,
                      incr ? NotifyEventId::Zincr : NotifyEventId::Zadd, op.key());
    if (processed && shard.has_blocking_waiters()) blocking_publish_zset_op(shard, op);
}

template <bool kNotify>
void cmd_zadd(Shard& shard, Op& op) { cmd_zadd_generic<kNotify>(shard, op, false); }
template <bool kNotify>
void cmd_zincrby(Shard& shard, Op& op) { cmd_zadd_generic<kNotify>(shard, op, true); }

template <bool kNotify>
void cmd_zscore(Shard& shard, Op& op) {
    KvObj* object = lookup_zset<kNotify>(shard, op);
    if (object == reinterpret_cast<KvObj*>(-1)) return;
    if (!object) {
        reply_null(op.sink(), op.resp3());
        return;
    }
    double score;
    if (!zset_get_score(zset_value(object), op.arg(2), score))
        reply_null(op.sink(), op.resp3());
    else
        reply_double(op.sink(), score, op.resp3());
}

template <bool kNotify>
void cmd_zmscore(Shard& shard, Op& op) {
    KvObj* object = lookup_zset<kNotify>(shard, op);
    if (object == reinterpret_cast<KvObj*>(-1)) return;
    reply_array_header(op.sink(), op.argc() - 2);
    for (uint32_t i = 2; i < op.argc(); i++) {
        double score;
        if (!object || !zset_get_score(zset_value(object), op.arg(i), score))
            reply_null(op.sink(), op.resp3());
        else
            reply_double(op.sink(), score, op.resp3());
    }
}

template <bool kNotify>
void cmd_zcard(Shard& shard, Op& op) {
    KvObj* object = lookup_zset<kNotify>(shard, op);
    if (object == reinterpret_cast<KvObj*>(-1)) return;
    reply_int(op.sink(), object ? zset_value(object).entries() : 0);
}

template <bool kNotify>
void cmd_zcount(Shard& shard, Op& op) {
    ScoreRange range;
    if (!parse_score_range(op.arg(2), op.arg(3), range)) {
        reply_err(op.sink(), "ERR min or max is not a float");
        return;
    }
    KvObj* object = lookup_zset<kNotify>(shard, op);
    if (object == reinterpret_cast<KvObj*>(-1)) return;
    if (!object || score_range_empty(range)) {
        reply_int(op.sink(), 0);
        return;
    }
    CollectionRef value = zset_value(object);
    uint64_t count = 0;
    if (value.encoding() == CollectionEncoding::Compact) {
        for (const Compact::Entry entry : value.compact()) {
            double score;
            Slice member;
            if (compact_decode(entry, score, member) && score_gte_min(score, range) &&
                score_lte_max(score, range))
                count++;
        }
    } else {
        count = zset_expanded(value)->count_by_score(range);
    }
    reply_int(op.sink(), static_cast<long long>(count));
}

template <bool kNotify>
void cmd_zlexcount(Shard& shard, Op& op) {
    LexRange range;
    if (!parse_lex_range(op.arg(2), op.arg(3), range)) {
        reply_err(op.sink(), "ERR min or max not valid string range item");
        return;
    }
    KvObj* object = lookup_zset<kNotify>(shard, op);
    if (object == reinterpret_cast<KvObj*>(-1)) return;
    if (!object || lex_range_empty(range)) {
        reply_int(op.sink(), 0);
        return;
    }
    CollectionRef value = zset_value(object);
    uint64_t count = 0;
    if (value.encoding() == CollectionEncoding::Compact) {
        bool started = false;
        for (const Compact::Entry entry : value.compact()) {
            double score;
            Slice member;
            if (!compact_decode(entry, score, member)) continue;
            if (!started) {
                if (!lex_gte_min(member, range)) continue;
                started = true;
            }
            if (!lex_lte_max(member, range)) break;
            count++;
        }
    } else {
        count = zset_expanded(value)->count_by_lex(range);
    }
    reply_int(op.sink(), static_cast<long long>(count));
}

template <bool kNotify>
void cmd_zrank_generic(Shard& shard, Op& op, bool reverse) {
    const bool withscore = op.argc() == 4;
    if (withscore && !op.arg(3).eq_icase("withscore")) {
        reply_syntax(op.sink());
        return;
    }
    KvObj* object = lookup_zset<kNotify>(shard, op);
    if (object == reinterpret_cast<KvObj*>(-1)) return;
    if (!object) {
        if (withscore) reply_null_array(op.sink(), op.resp3());
        else reply_null(op.sink(), op.resp3());
        return;
    }
    CollectionRef value = zset_value(object);
    uint64_t rank = 0;
    double score = 0;
    bool found = false;
    if (value.encoding() == CollectionEncoding::Compact) {
        for (const Compact::Entry entry : value.compact()) {
            Slice member;
            if (!compact_decode(entry, score, member)) continue;
            if (member == op.arg(2)) {
                found = true;
                break;
            }
            rank++;
        }
    } else {
        ZsetNode* node = zset_expanded(value)->find(op.arg(2));
        if (node) {
            score = node->score;
            rank = zset_expanded(value)->rank_of(node) - 1;
            found = true;
        }
    }
    if (!found) {
        if (withscore) reply_null_array(op.sink(), op.resp3());
        else reply_null(op.sink(), op.resp3());
        return;
    }
    if (reverse) rank = value.entries() - rank - 1;
    if (withscore) reply_array_header(op.sink(), 2);
    reply_int(op.sink(), static_cast<long long>(rank));
    if (withscore) reply_double(op.sink(), score, op.resp3());
}

template <bool kNotify>
void cmd_zrank(Shard& shard, Op& op) { cmd_zrank_generic<kNotify>(shard, op, false); }
template <bool kNotify>
void cmd_zrevrank(Shard& shard, Op& op) { cmd_zrank_generic<kNotify>(shard, op, true); }

template <bool kNotify>
void cmd_zrem(Shard& shard, Op& op) {
    KvObj* object = lookup_zset<kNotify>(shard, op);
    if (object == reinterpret_cast<KvObj*>(-1)) return;
    if (!object) {
        reply_int(op.sink(), 0);
        return;
    }
    CollectionRef value = zset_value(object);
    ObjectSizeTracker size_tracker(shard.store(), object);
    uint64_t removed = 0;
    if (value.encoding() == CollectionEncoding::Compact) {
        for (uint32_t i = 2; i < op.argc(); i++) {
            Compact::Entry entry;
            if (compact_find(value, op.arg(i), &entry, nullptr) && value.erase(entry)) removed++;
        }
    } else {
        for (uint32_t i = 2; i < op.argc(); i++) {
            ZsetVal* external = value.external_as<ZsetVal>();
            ZsetNode* node = external->expanded->find(op.arg(i));
            if (!node) continue;
            const uint32_t payload = external->expanded->erase_node(node);
            external->note_expanded_delete(payload, external->expanded->allocation_bytes());
            removed++;
        }
    }
    size_tracker.finish();                       // account the shrink before any whole-key erase
    if constexpr (kNotify) if (removed)
        notify_record(shard, op, NOTIFY_ZSET, NotifyEventId::Zrem, op.key());
    if (!value.entries()) shard.store_erase<kNotify>(op.hash, op.key());
    reply_int(op.sink(), static_cast<long long>(removed));
}

RemovalResult compact_erase_rank(CollectionRef& value, int64_t start, int64_t stop) {
    RemovalResult result;
    CompactItems items;
    if (!items.load(value)) return result;
    const int64_t length = static_cast<int64_t>(items.entries.size());
    if (start < 0) start += length;
    if (stop < 0) stop += length;
    if (start < 0) start = 0;
    if (start > stop || start >= length || stop < 0) return result;
    if (stop >= length) stop = length - 1;
    const Compact::Entry& first = items.entries[static_cast<size_t>(start)];
    const Compact::Entry& last = items.entries[static_cast<size_t>(stop)];
    const uint32_t end = value.compact().logical(last) + last.span;
    for (int64_t i = start; i <= stop; i++) result.payload += items.entries[i].value.n;
    result.count = static_cast<uint32_t>(stop - start + 1);
    if (!value.erase_range(value.compact().logical(first), end)) return {};
    return result;
}

RemovalResult compact_erase_score(CollectionRef& value, const ScoreRange& range) {
    RemovalResult result;
    uint32_t first = 0, last = 0;
    bool found = false;
    for (const Compact::Entry entry : value.compact()) {
        double score;
        Slice member;
        if (!compact_decode(entry, score, member)) continue;
        if (!found) {
            if (!score_gte_min(score, range)) continue;
            if (!score_lte_max(score, range)) break;
            first = value.compact().logical(entry);
            found = true;
        }
        if (!score_lte_max(score, range)) break;
        last = value.compact().logical(entry) + entry.span;
        result.count++;
        result.payload += entry.value.n;
    }
    if (found && !value.erase_range(first, last)) return {};
    return result;
}

RemovalResult compact_erase_lex(CollectionRef& value, const LexRange& range) {
    RemovalResult result;
    uint32_t first = 0, last = 0;
    bool found = false;
    for (const Compact::Entry entry : value.compact()) {
        double score;
        Slice member;
        if (!compact_decode(entry, score, member)) continue;
        if (!found) {
            if (!lex_gte_min(member, range)) continue;
            if (!lex_lte_max(member, range)) break;
            first = value.compact().logical(entry);
            found = true;
        }
        if (!lex_lte_max(member, range)) break;
        last = value.compact().logical(entry) + entry.span;
        result.count++;
        result.payload += entry.value.n;
    }
    if (found && !value.erase_range(first, last)) return {};
    return result;
}

template <bool kNotify>
void finish_range_delete(Shard& shard, Op& op, CollectionRef& value, RemovalResult result,
                         bool expanded, ObjectSizeTracker& size_tracker) {
    if (expanded && result.count)
        value.external_as<ZsetVal>()->note_expanded_delete_many(
            result.count, result.payload, zset_expanded(value)->allocation_bytes());
    size_tracker.finish();                       // account the shrink before any whole-key erase
    if constexpr (kNotify) if (result.count) {
        NotifyEventId event = NotifyEventId::Zremrangebyrank;
        if (op.cmd_name().eq_icase("zremrangebyscore"))
            event = NotifyEventId::Zremrangebyscore;
        else if (op.cmd_name().eq_icase("zremrangebylex"))
            event = NotifyEventId::Zremrangebylex;
        notify_record(shard, op, NOTIFY_ZSET, event, op.key());
    }
    if (!value.entries()) shard.store_erase<kNotify>(op.hash, op.key());
    reply_int(op.sink(), result.count);
}

template <bool kNotify>
void cmd_zremrangebyrank(Shard& shard, Op& op) {
    int64_t start, stop;
    if (!parse_i64(op.arg(2), start) || !parse_i64(op.arg(3), stop)) {
        reply_invalid_integer(op);
        return;
    }
    KvObj* object = lookup_zset<kNotify>(shard, op);
    if (object == reinterpret_cast<KvObj*>(-1)) return;
    if (!object) {
        reply_int(op.sink(), 0);
        return;
    }
    CollectionRef value = zset_value(object);
    ObjectSizeTracker size_tracker(shard.store(), object);
    if (value.encoding() == CollectionEncoding::Compact) {
        finish_range_delete<kNotify>(shard, op, value, compact_erase_rank(value, start, stop), false,
                            size_tracker);
        return;
    }
    const int64_t length = static_cast<int64_t>(value.entries());
    if (start < 0) start += length;
    if (stop < 0) stop += length;
    if (start < 0) start = 0;
    RemovalResult result;
    if (start <= stop && start < length && stop >= 0) {
        if (stop >= length) stop = length - 1;
        result = zset_expanded(value)->erase_rank_range(static_cast<uint64_t>(start + 1),
                                                        static_cast<uint64_t>(stop + 1));
    }
    finish_range_delete<kNotify>(shard, op, value, result, true, size_tracker);
}

template <bool kNotify>
void cmd_zremrangebyscore(Shard& shard, Op& op) {
    ScoreRange range;
    if (!parse_score_range(op.arg(2), op.arg(3), range)) {
        reply_err(op.sink(), "ERR min or max is not a float");
        return;
    }
    KvObj* object = lookup_zset<kNotify>(shard, op);
    if (object == reinterpret_cast<KvObj*>(-1)) return;
    if (!object) {
        reply_int(op.sink(), 0);
        return;
    }
    CollectionRef value = zset_value(object);
    ObjectSizeTracker size_tracker(shard.store(), object);
    const bool expanded = value.encoding() != CollectionEncoding::Compact;
    RemovalResult result;
    if (!score_range_empty(range))
        result = expanded ? zset_expanded(value)->erase_score_range(range)
                          : compact_erase_score(value, range);
    finish_range_delete<kNotify>(shard, op, value, result, expanded, size_tracker);
}

template <bool kNotify>
void cmd_zremrangebylex(Shard& shard, Op& op) {
    LexRange range;
    if (!parse_lex_range(op.arg(2), op.arg(3), range)) {
        reply_err(op.sink(), "ERR min or max not valid string range item");
        return;
    }
    KvObj* object = lookup_zset<kNotify>(shard, op);
    if (object == reinterpret_cast<KvObj*>(-1)) return;
    if (!object) {
        reply_int(op.sink(), 0);
        return;
    }
    CollectionRef value = zset_value(object);
    ObjectSizeTracker size_tracker(shard.store(), object);
    const bool expanded = value.encoding() != CollectionEncoding::Compact;
    RemovalResult result;
    if (!lex_range_empty(range))
        result = expanded ? zset_expanded(value)->erase_lex_range(range)
                          : compact_erase_lex(value, range);
    finish_range_delete<kNotify>(shard, op, value, result, expanded, size_tracker);
}

enum class RangeKind : uint8_t { Auto, Rank, Score, Lex };

struct RangeOptions {
    RangeKind kind = RangeKind::Auto;
    bool reverse = false;
    bool direction_is_auto = true;
    bool kind_is_auto = true;
    bool withscores = false;
    bool limit_seen = false;
    int64_t offset = 0;
    int64_t limit = -1;
};

bool parse_range_options(Op& op, RangeOptions& options) {
    for (uint32_t i = 4; i < op.argc(); i++) {
        const Slice arg = op.arg(i);
        if (arg.eq_icase("withscores")) {
            options.withscores = true;
        } else if (arg.eq_icase("limit") && i + 2 < op.argc()) {
            if (!parse_i64(op.arg(i + 1), options.offset) ||
                !parse_i64(op.arg(i + 2), options.limit)) {
                reply_invalid_integer(op);
                return false;
            }
            options.limit_seen = true;
            i += 2;
        } else if (arg.eq_icase("rev") && options.direction_is_auto) {
            options.reverse = true;
            options.direction_is_auto = false;
        } else if (arg.eq_icase("byscore") && options.kind_is_auto) {
            options.kind = RangeKind::Score;
            options.kind_is_auto = false;
        } else if (arg.eq_icase("bylex") && options.kind_is_auto) {
            options.kind = RangeKind::Lex;
            options.kind_is_auto = false;
        } else {
            reply_syntax(op.sink());
            return false;
        }
    }
    if (options.kind == RangeKind::Auto) options.kind = RangeKind::Rank;
    // Redis rejects LIMIT on an index range only when the COUNT actually bounds the answer: its
    // guard tests the parsed count against the -1 default and ignores the offset entirely, so
    // "ZRANGE k 0 -1 LIMIT 0 -1" (and any offset with count -1) is accepted and the LIMIT is a
    // no-op. Client libraries that always emit LIMIT depend on that.
    if (options.limit_seen && options.limit != -1 && options.kind == RangeKind::Rank) {
        reply_err(op.sink(),
                  "ERR syntax error, LIMIT is only supported in combination with either BYSCORE or BYLEX");
        return false;
    }
    if (options.withscores && options.kind == RangeKind::Lex) {
        reply_err(op.sink(),
                  "ERR syntax error, WITHSCORES not supported in combination with BYLEX");
        return false;
    }
    return true;
}

uint64_t limited_count(uint64_t available, int64_t limit) {
    if (limit < 0) return available;
    return std::min<uint64_t>(available, static_cast<uint64_t>(limit));
}

void emit_rank_range(Op& op, const CollectionRef& value, int64_t start, int64_t stop,
                     bool reverse, bool withscores) {
    const int64_t length = static_cast<int64_t>(value.entries());
    if (start < 0) start += length;
    if (stop < 0) stop += length;
    if (start < 0) start = 0;
    if (start > stop || start >= length || stop < 0) {
        reply_array_header(op.sink(), 0);
        return;
    }
    if (stop >= length) stop = length - 1;
    const uint64_t count = static_cast<uint64_t>(stop - start + 1);

    if (value.encoding() == CollectionEncoding::Compact) {
        CompactItems items;
        if (!items.load(value)) {
            reply_oom(op);
            return;
        }
        reply_array_header(op.sink(), scored_array_length(count, withscores, op.resp3()));
        for (int64_t i = start; i <= stop; i++) {
            const size_t index = reverse ? static_cast<size_t>(length - i - 1)
                                         : static_cast<size_t>(i);
            double score;
            Slice member;
            if (compact_decode(items.entries[index], score, member))
                emit_item(op, member, score, withscores);
        }
        return;
    }

    const uint64_t first_rank = reverse ? static_cast<uint64_t>(length - start)
                                        : static_cast<uint64_t>(start + 1);
    ZsetNode* node = zset_expanded(value)->by_rank(first_rank);
    reply_array_header(op.sink(), scored_array_length(count, withscores, op.resp3()));
    for (uint64_t i = 0; i < count && node; i++) {
        emit_item(op, node_member(node), node->score, withscores);
        node = reverse ? node->backward : node->level[0].forward;
    }
}

void emit_score_range(Op& op, const CollectionRef& value, const ScoreRange& range,
                      const RangeOptions& options) {
    // A negative LIMIT offset is NOT rejected here: it counts back from the end of the matched
    // range on the expanded encoding. See zset_resolve_limit_offset in t_zset.h.
    if (score_range_empty(range) || options.limit == 0) {
        reply_array_header(op.sink(), 0);
        return;
    }
    uint64_t first_rank = 0, last_rank = 0;
    if (value.encoding() == CollectionEncoding::Compact) {
        CompactItems items;
        if (!items.load(value)) {
            reply_oom(op);
            return;
        }
        bool found = false;
        for (size_t i = 0; i < items.entries.size(); i++) {
            double score;
            Slice member;
            if (!compact_decode(items.entries[i], score, member)) continue;
            if (score_gte_min(score, range) && score_lte_max(score, range)) {
                if (!found) first_rank = i + 1;
                last_rank = i + 1;
                found = true;
            } else if (found && !score_lte_max(score, range)) {
                break;
            }
        }
        if (!found) {
            reply_array_header(op.sink(), 0);
            return;
        }
        const uint64_t available = last_rank - first_rank + 1;
        uint64_t offset = 0;
        if (!zset_resolve_limit_offset(options.offset, available, false, offset)) {
            reply_array_header(op.sink(), 0);
            return;
        }
        const uint64_t count = limited_count(available - offset, options.limit);
        reply_array_header(op.sink(),
                           scored_array_length(count, options.withscores, op.resp3()));
        int64_t index = options.reverse ? static_cast<int64_t>(last_rank - 1 - offset)
                                        : static_cast<int64_t>(first_rank - 1 + offset);
        const int64_t step = options.reverse ? -1 : 1;
        for (uint64_t i = 0; i < count; i++, index += step) {
            double score;
            Slice member;
            if (compact_decode(items.entries[static_cast<size_t>(index)], score, member))
                emit_item(op, member, score, options.withscores);
        }
        return;
    }

    if (!zset_expanded(value)->first_by_score(range, &first_rank) ||
        !zset_expanded(value)->last_by_score(range, &last_rank)) {
        reply_array_header(op.sink(), 0);
        return;
    }
    const uint64_t available = last_rank - first_rank + 1;
    uint64_t offset = 0;
    if (!zset_resolve_limit_offset(options.offset, available, true, offset)) {
        reply_array_header(op.sink(), 0);
        return;
    }
    const uint64_t count = limited_count(available - offset, options.limit);
    const uint64_t start_rank = options.reverse ? last_rank - offset : first_rank + offset;
    ZsetNode* node = zset_expanded(value)->by_rank(start_rank);
    reply_array_header(op.sink(), scored_array_length(count, options.withscores, op.resp3()));
    for (uint64_t i = 0; i < count && node; i++) {
        emit_item(op, node_member(node), node->score, options.withscores);
        node = options.reverse ? node->backward : node->level[0].forward;
    }
}

void emit_lex_range(Op& op, const CollectionRef& value, const LexRange& range,
                    const RangeOptions& options) {
    // As in emit_score_range: a negative LIMIT offset is resolved per encoding, not rejected.
    if (lex_range_empty(range) || options.limit == 0) {
        reply_array_header(op.sink(), 0);
        return;
    }
    uint64_t first_rank = 0, last_rank = 0;
    if (value.encoding() == CollectionEncoding::Compact) {
        CompactItems items;
        if (!items.load(value)) {
            reply_oom(op);
            return;
        }
        bool found = false;
        for (size_t i = 0; i < items.entries.size(); i++) {
            double score;
            Slice member;
            if (!compact_decode(items.entries[i], score, member)) continue;
            if (!found) {
                if (!lex_gte_min(member, range)) continue;
                if (!lex_lte_max(member, range)) break;
                first_rank = i + 1;
                found = true;
            }
            if (!lex_lte_max(member, range)) break;
            last_rank = i + 1;
        }
        if (!found) {
            reply_array_header(op.sink(), 0);
            return;
        }
        const uint64_t available = last_rank - first_rank + 1;
        uint64_t offset = 0;
        if (!zset_resolve_limit_offset(options.offset, available, false, offset)) {
            reply_array_header(op.sink(), 0);
            return;
        }
        const uint64_t count = limited_count(available - offset, options.limit);
        reply_array_header(op.sink(), count);
        int64_t index = options.reverse ? static_cast<int64_t>(last_rank - 1 - offset)
                                        : static_cast<int64_t>(first_rank - 1 + offset);
        const int64_t step = options.reverse ? -1 : 1;
        for (uint64_t i = 0; i < count; i++, index += step) {
            double score;
            Slice member;
            if (compact_decode(items.entries[static_cast<size_t>(index)], score, member))
                reply_bulk(op.sink(), member);
        }
        return;
    }

    if (!zset_expanded(value)->first_by_lex(range, &first_rank) ||
        !zset_expanded(value)->last_by_lex(range, &last_rank)) {
        reply_array_header(op.sink(), 0);
        return;
    }
    const uint64_t available = last_rank - first_rank + 1;
    uint64_t offset = 0;
    if (!zset_resolve_limit_offset(options.offset, available, true, offset)) {
        reply_array_header(op.sink(), 0);
        return;
    }
    const uint64_t count = limited_count(available - offset, options.limit);
    const uint64_t start_rank = options.reverse ? last_rank - offset : first_rank + offset;
    ZsetNode* node = zset_expanded(value)->by_rank(start_rank);
    reply_array_header(op.sink(), count);
    for (uint64_t i = 0; i < count && node; i++) {
        reply_bulk(op.sink(), node_member(node));
        node = options.reverse ? node->backward : node->level[0].forward;
    }
}

template <bool kNotify>
void cmd_zrange_generic(Shard& shard, Op& op, RangeKind initial_kind,
                        bool initial_reverse, bool unified) {
    RangeOptions options;
    options.kind = initial_kind;
    options.reverse = initial_reverse;
    options.kind_is_auto = initial_kind == RangeKind::Auto;
    options.direction_is_auto = unified;
    if (!parse_range_options(op, options)) return;

    int64_t rank_start = 0, rank_stop = 0;
    ScoreRange score_range;
    LexRange lex_range;
    uint32_t min_index = 2, max_index = 3;
    if (options.reverse && options.kind != RangeKind::Rank) std::swap(min_index, max_index);
    if (options.kind == RangeKind::Rank) {
        if (!parse_i64(op.arg(min_index), rank_start) ||
            !parse_i64(op.arg(max_index), rank_stop)) {
            reply_invalid_integer(op);
            return;
        }
    } else if (options.kind == RangeKind::Score) {
        if (!parse_score_range(op.arg(min_index), op.arg(max_index), score_range)) {
            reply_err(op.sink(), "ERR min or max is not a float");
            return;
        }
    } else {
        if (!parse_lex_range(op.arg(min_index), op.arg(max_index), lex_range)) {
            reply_err(op.sink(), "ERR min or max not valid string range item");
            return;
        }
    }

    KvObj* object = lookup_zset<kNotify>(shard, op);
    if (object == reinterpret_cast<KvObj*>(-1)) return;
    if (!object) {
        reply_array_header(op.sink(), 0);
        return;
    }
    CollectionRef value = zset_value(object);
    if (options.kind == RangeKind::Rank)
        emit_rank_range(op, value, rank_start, rank_stop, options.reverse, options.withscores);
    else if (options.kind == RangeKind::Score)
        emit_score_range(op, value, score_range, options);
    else
        emit_lex_range(op, value, lex_range, options);
}

template <bool kNotify>
void cmd_zrange(Shard& shard, Op& op) {
    cmd_zrange_generic<kNotify>(shard, op, RangeKind::Auto, false, true);
}
template <bool kNotify>
void cmd_zrevrange(Shard& shard, Op& op) {
    cmd_zrange_generic<kNotify>(shard, op, RangeKind::Rank, true, false);
}
template <bool kNotify>
void cmd_zrangebyscore(Shard& shard, Op& op) {
    cmd_zrange_generic<kNotify>(shard, op, RangeKind::Score, false, false);
}
template <bool kNotify>
void cmd_zrevrangebyscore(Shard& shard, Op& op) {
    cmd_zrange_generic<kNotify>(shard, op, RangeKind::Score, true, false);
}
template <bool kNotify>
void cmd_zrangebylex(Shard& shard, Op& op) {
    cmd_zrange_generic<kNotify>(shard, op, RangeKind::Lex, false, false);
}
template <bool kNotify>
void cmd_zrevrangebylex(Shard& shard, Op& op) {
    cmd_zrange_generic<kNotify>(shard, op, RangeKind::Lex, true, false);
}

template <bool kNotify>
void cmd_zpop_generic(Shard& shard, Op& op, bool maximum) {
    int64_t requested = 1;
    if (op.argc() == 3) {
        // One message for both failures, as redis's getRangeLongFromObject(0, LONG_MAX, msg).
        if (!parse_i64(op.arg(2), requested) || requested < 0) {
            reply_err(op.sink(), "ERR value is out of range, must be positive");
            return;
        }
    }
    KvObj* object = lookup_zset<kNotify>(shard, op);
    if (object == reinterpret_cast<KvObj*>(-1)) return;
    if (!object || requested == 0) {
        reply_array_header(op.sink(), 0);
        return;
    }
    CollectionRef value = zset_value(object);
    ObjectSizeTracker size_tracker(shard.store(), object);
    const uint64_t take = std::min<uint64_t>(static_cast<uint64_t>(requested), value.entries());
    const bool nested = op.resp3() && op.argc() == 3;

    RemovalResult removed;
    if (value.encoding() == CollectionEncoding::Compact) {
        CompactItems items;
        if (!items.load(value)) {
            reply_oom(op);
            return;
        }
        reply_array_header(op.sink(), nested ? take : take * 2);
        for (uint64_t i = 0; i < take; i++) {
            const size_t index = maximum ? items.entries.size() - 1 - i : i;
            double score;
            Slice member;
            if (compact_decode(items.entries[index], score, member))
                emit_item(op, member, score, true, nested);
        }
        const int64_t first = maximum ? static_cast<int64_t>(items.entries.size() - take) : 0;
        const int64_t last = maximum ? static_cast<int64_t>(items.entries.size() - 1)
                                     : static_cast<int64_t>(take - 1);
        removed = compact_erase_rank(value, first, last);
    } else {
        const uint64_t start_rank = maximum ? value.entries() : 1;
        ZsetNode* node = zset_expanded(value)->by_rank(start_rank);
        reply_array_header(op.sink(), nested ? take : take * 2);
        for (uint64_t i = 0; i < take && node; i++) {
            emit_item(op, node_member(node), node->score, true, nested);
            node = maximum ? node->backward : node->level[0].forward;
        }
        removed = maximum
                      ? zset_expanded(value)->erase_rank_range(value.entries() - take + 1,
                                                               value.entries())
                      : zset_expanded(value)->erase_rank_range(1, take);
        value.external_as<ZsetVal>()->note_expanded_delete_many(
            removed.count, removed.payload, zset_expanded(value)->allocation_bytes());
    }
    size_tracker.finish();
    if constexpr (kNotify)
        notify_record(shard, op, NOTIFY_ZSET,
                      maximum ? NotifyEventId::Zpopmax : NotifyEventId::Zpopmin, op.key());
    if (!value.entries()) shard.store_erase<kNotify>(op.hash, op.key());
}

template <bool kNotify>
void cmd_zpopmin(Shard& shard, Op& op) { cmd_zpop_generic<kNotify>(shard, op, false); }
template <bool kNotify>
void cmd_zpopmax(Shard& shard, Op& op) { cmd_zpop_generic<kNotify>(shard, op, true); }

bool get_item_by_zero_rank(const CollectionRef& value, const CompactItems* compact, uint64_t rank,
                           Slice& member, double& score) {
    if (value.encoding() == CollectionEncoding::Compact) {
        if (!compact || rank >= compact->entries.size()) return false;
        return compact_decode(compact->entries[static_cast<size_t>(rank)], score, member);
    }
    ZsetNode* node = zset_expanded(value)->by_rank(rank + 1);
    if (!node) return false;
    member = node_member(node);
    score = node->score;
    return true;
}

template <bool kNotify>
void cmd_zrandmember(Shard& shard, Op& op) {
    const bool has_count = op.argc() >= 3;
    const bool withscores = op.argc() == 4;
    int64_t signed_count = 0;
    if (has_count && !parse_i64(op.arg(2), signed_count)) {
        reply_invalid_integer(op);
        return;
    }
    if (withscores && !op.arg(3).eq_icase("withscores")) {
        reply_syntax(op.sink());
        return;
    }
    if (withscores && (signed_count < -std::numeric_limits<int64_t>::max() / 2 ||
                       signed_count > std::numeric_limits<int64_t>::max() / 2)) {
        reply_outofrange(op.sink());
        return;
    }

    KvObj* object = lookup_zset<kNotify>(shard, op);
    if (object == reinterpret_cast<KvObj*>(-1)) return;
    if (!object) {
        if (has_count) reply_array_header(op.sink(), 0);
        else reply_null(op.sink(), op.resp3());
        return;
    }
    CollectionRef value = zset_value(object);
    CompactItems compact;
    CompactItems* compact_ptr = nullptr;
    if (value.encoding() == CollectionEncoding::Compact) {
        if (!compact.load(value)) {
            reply_oom(op);
            return;
        }
        compact_ptr = &compact;
    }

    if (!has_count) {
        Slice member;
        double score;
        if (value.encoding() == CollectionEncoding::Compact) {
            get_item_by_zero_rank(value, compact_ptr, random_bounded(value.entries()), member, score);
        } else {
            ZsetNode* node = zset_expanded(value)->by_member.random_node();
            member = node_member(node);
            score = node->score;
        }
        reply_bulk(op.sink(), member);
        return;
    }

    if (signed_count == std::numeric_limits<int64_t>::min()) {
        reply_outofrange(op.sink());
        return;
    }
    const bool unique = signed_count >= 0;
    uint64_t count = static_cast<uint64_t>(unique ? signed_count : -signed_count);
    if (unique) count = std::min<uint64_t>(count, value.entries());
    if (!count) {
        reply_array_header(op.sink(), 0);
        return;
    }

    if (!unique) {
        reply_array_header(op.sink(), scored_array_length(count, withscores, op.resp3()));
        for (uint64_t i = 0; i < count; i++) {
            Slice member;
            double score;
            if (value.encoding() == CollectionEncoding::Compact) {
                get_item_by_zero_rank(value, compact_ptr, random_bounded(value.entries()), member,
                                      score);
            } else {
                ZsetNode* node = zset_expanded(value)->by_member.random_node();
                member = node_member(node);
                score = node->score;
            }
            emit_item(op, member, score, withscores);
        }
        return;
    }

    if (value.encoding() != CollectionEncoding::Compact) {
        std::vector<ZsetNode*> nodes;
        try {
            nodes.reserve(static_cast<size_t>(count));
            if (count == value.entries() || count > value.entries() / 3) {
                nodes.reserve(value.entries());
                for (ZsetNode* node = zset_expanded(value)->header->level[0].forward; node;
                     node = node->level[0].forward)
                    nodes.push_back(node);
                for (uint64_t i = 0; i < count; i++) {
                    const uint64_t chosen = i + random_bounded(nodes.size() - i);
                    std::swap(nodes[i], nodes[chosen]);
                }
                nodes.resize(static_cast<size_t>(count));
            } else {
                std::unordered_set<ZsetNode*> selected;
                selected.reserve(static_cast<size_t>(count));
                while (nodes.size() < count) {
                    ZsetNode* node = zset_expanded(value)->by_member.random_node();
                    if (selected.insert(node).second) nodes.push_back(node);
                }
            }
        } catch (const std::bad_alloc&) {
            reply_oom(op);
            return;
        }
        reply_array_header(op.sink(), scored_array_length(count, withscores, op.resp3()));
        for (ZsetNode* node : nodes) emit_item(op, node_member(node), node->score, withscores);
        return;
    }

    std::vector<uint64_t> ranks;
    try {
        ranks.reserve(static_cast<size_t>(count));
        if (count == value.entries() || count > value.entries() / 3) {
            ranks.resize(value.entries());
            for (uint64_t i = 0; i < value.entries(); i++) ranks[i] = i;
            for (uint64_t i = 0; i < count; i++) {
                const uint64_t chosen = i + random_bounded(value.entries() - i);
                std::swap(ranks[i], ranks[chosen]);
            }
            ranks.resize(static_cast<size_t>(count));
        } else {
            std::unordered_set<uint64_t> selected;
            selected.reserve(static_cast<size_t>(count));
            while (ranks.size() < count) {
                const uint64_t rank = random_bounded(value.entries());
                if (selected.insert(rank).second) ranks.push_back(rank);
            }
        }
    } catch (const std::bad_alloc&) {
        reply_oom(op);
        return;
    }
    reply_array_header(op.sink(), scored_array_length(count, withscores, op.resp3()));
    for (uint64_t rank : ranks) {
        Slice member;
        double score;
        if (get_item_by_zero_rank(value, compact_ptr, rank, member, score))
            emit_item(op, member, score, withscores);
    }
}

struct ScanItem {
    Slice member;
    double score;
};

void reply_scan(Op& op, uint64_t cursor, const std::vector<ScanItem>& items) {
    reply_array_header(op.sink(), 2);
    reply_cursor(op, cursor);
    reply_array_header(op.sink(), items.size() * 2);
    for (const ScanItem& item : items) {
        reply_bulk(op.sink(), item.member);
        reply_double(op.sink(), item.score);  // ZSCAN scores stay bulk strings under RESP3.
    }
}

template <bool kNotify>
void cmd_zscan(Shard& shard, Op& op) {
    uint64_t cursor;
    if (!command_parse_scan_cursor(op.arg(2), cursor)) {
        reply_err(op.sink(), "ERR invalid cursor");
        return;
    }
    KvObj* object = lookup_zset<kNotify>(shard, op);
    if (object == reinterpret_cast<KvObj*>(-1)) return;
    if (!object) {
        reply_array_header(op.sink(), 2);
        reply_cursor(op, 0);
        reply_array_header(op.sink(), 0);
        return;
    }

    uint64_t count = 10;
    Slice pattern;
    bool use_pattern = false;
    for (uint32_t i = 3; i < op.argc(); i++) {
        if (op.arg(i).eq_icase("count") && i + 1 < op.argc()) {
            int64_t parsed;
            if (!parse_i64(op.arg(++i), parsed)) {
                reply_invalid_integer(op);
                return;
            }
            if (parsed < 1) {
                reply_syntax(op.sink());
                return;
            }
            count = static_cast<uint64_t>(parsed);
        } else if (op.arg(i).eq_icase("match") && i + 1 < op.argc()) {
            pattern = op.arg(++i);
            use_pattern = !(pattern.n == 1 && pattern.p[0] == '*');
        } else {
            reply_syntax(op.sink());
            return;
        }
    }

    CollectionRef value = zset_value(object);
    std::vector<ScanItem> result;
    if (value.encoding() == CollectionEncoding::Compact) {
        try {
            result.reserve(value.entries());
            for (const Compact::Entry entry : value.compact()) {
                ScanItem item;
                if (!compact_decode(entry, item.score, item.member)) continue;
                if (!use_pattern || command_glob_match(pattern, item.member)) result.push_back(item);
            }
        } catch (const std::bad_alloc&) {
            reply_oom(op);
            return;
        }
        reply_scan(op, 0, result);
        return;
    }

    uint64_t next_cursor = 0;
    try {
        next_cursor = zset_expanded(value)->by_member.scan(cursor, count, [&](ZsetNode* node) {
                const Slice member = node_member(node);
                if (!use_pattern || command_glob_match(pattern, member))
                    result.push_back({member, node->score});
            });
    } catch (const std::bad_alloc&) {
        reply_oom(op);
        return;
    }
    reply_scan(op, next_cursor, result);
}

#define TOMO_HANDLER_PAIR(fn) fn<false>, 1, 1, 1, notify_handler<fn<true>>

static const CommandSpec kTable[] = {
    // name                  min max flags               handler                 first last step
    {"ZADD",                 4, -1, CmdFlags::Write | CmdFlags::DenyOom, TOMO_HANDLER_PAIR(cmd_zadd)},
    {"ZSCORE",               3,  3, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_zscore)},
    {"ZMSCORE",              3, -1, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_zmscore)},
    {"ZINCRBY",              4,  4, CmdFlags::Write | CmdFlags::DenyOom, TOMO_HANDLER_PAIR(cmd_zincrby)},
    {"ZCARD",                2,  2, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_zcard)},
    {"ZCOUNT",               4,  4, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_zcount)},
    {"ZRANGE",               4, -1, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_zrange)},
    {"ZRANGEBYSCORE",        4, -1, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_zrangebyscore)},
    {"ZREVRANGEBYSCORE",     4, -1, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_zrevrangebyscore)},
    {"ZRANGEBYLEX",          4, -1, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_zrangebylex)},
    {"ZREVRANGEBYLEX",       4, -1, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_zrevrangebylex)},
    {"ZREVRANGE",            4, -1, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_zrevrange)},
    {"ZRANK",                3,  4, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_zrank)},
    {"ZREVRANK",             3,  4, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_zrevrank)},
    {"ZREM",                 3, -1, CmdFlags::Write,    TOMO_HANDLER_PAIR(cmd_zrem)},
    {"ZREMRANGEBYRANK",      4,  4, CmdFlags::Write,    TOMO_HANDLER_PAIR(cmd_zremrangebyrank)},
    {"ZREMRANGEBYSCORE",     4,  4, CmdFlags::Write,    TOMO_HANDLER_PAIR(cmd_zremrangebyscore)},
    {"ZREMRANGEBYLEX",       4,  4, CmdFlags::Write,    TOMO_HANDLER_PAIR(cmd_zremrangebylex)},
    {"ZLEXCOUNT",            4,  4, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_zlexcount)},
    {"ZPOPMIN",              2,  3, CmdFlags::Write,    TOMO_HANDLER_PAIR(cmd_zpopmin)},
    {"ZPOPMAX",              2,  3, CmdFlags::Write,    TOMO_HANDLER_PAIR(cmd_zpopmax)},
    {"ZRANDMEMBER",          2,  4, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_zrandmember)},
    {"ZSCAN",                3, -1, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_zscan)},
    {"BZPOPMIN",             3, -1, CmdFlags::Write | CmdFlags::Blocking | CmdFlags::MultiShard,cmd_xshard_only,1,-1,1},
    {"BZPOPMAX",             3, -1, CmdFlags::Write | CmdFlags::Blocking | CmdFlags::MultiShard,cmd_xshard_only,1,-1,1},
    {"BZMPOP",               5, -1, CmdFlags::Write | CmdFlags::Blocking | CmdFlags::MultiShard,cmd_xshard_only,3,-1,1},
    {"ZMPOP",                4, -1, CmdFlags::Write | CmdFlags::MultiShard,cmd_xshard_only,2,-1,1},
    {"ZRANGESTORE",          5, -1, CmdFlags::Write | CmdFlags::DenyOom | CmdFlags::MultiShard,cmd_xshard_only,1,2,1},
};

#undef TOMO_HANDLER_PAIR

}  // namespace

XshardPopResult xshard_pop_zset(Shard& shard, Slice key, uint64_t hash, bool maximum,
                                uint64_t count, std::vector<std::string>& members,
                                std::vector<double>& scores) {
    members.clear();
    scores.clear();
    const bool notify = shard.notify_carrier() != nullptr;
    KvObj* object = notify ? shard.store_find<true>(hash, key) : shard.store().find(hash, key);
    if (!object) return XshardPopResult::Missing;
    if (static_cast<Type>(object->type) != Type::Zset) return XshardPopResult::WrongType;
    CollectionRef value = zset_value(object);
    if (!value.entries()) return XshardPopResult::Missing;
    const uint32_t take = static_cast<uint32_t>(
        std::min<uint64_t>(count, static_cast<uint64_t>(value.entries())));
    try {
        members.reserve(take);
        scores.reserve(take);
        if (value.encoding() == CollectionEncoding::Compact) {
            CompactItems items;
            if (!items.load(value)) return XshardPopResult::Oom;
            for (uint32_t i = 0; i < take; i++) {
                const size_t index = maximum ? items.entries.size() - 1 - i : i;
                double score;
                Slice member;
                if (!compact_decode(items.entries[index], score, member))
                    return XshardPopResult::Oom;
                members.emplace_back(member.p, member.n);
                scores.push_back(score);
            }
        } else {
            ZsetNode* node = zset_expanded(value)->by_rank(maximum ? value.entries() : 1);
            for (uint32_t i = 0; i < take && node; i++) {
                const Slice member = node_member(node);
                members.emplace_back(member.p, member.n);
                scores.push_back(node->score);
                node = maximum ? node->backward : node->level[0].forward;
            }
        }
    } catch (const std::bad_alloc&) {
        members.clear();
        scores.clear();
        return XshardPopResult::Oom;
    }
    if (members.size() != take || scores.size() != take) {
        members.clear();
        scores.clear();
        return XshardPopResult::Oom;
    }

    ObjectSizeTracker size_tracker(shard.store(), object);
    if (value.encoding() == CollectionEncoding::Compact) {
        for (uint32_t i = 0; i < take; i++) {
            uint32_t payload = 0;
            if (maximum) value.pop_back(&payload);
            else value.pop_front(&payload);
        }
    } else {
        RemovalResult removed = maximum
            ? zset_expanded(value)->erase_rank_range(value.entries() - take + 1, value.entries())
            : zset_expanded(value)->erase_rank_range(1, take);
        value.external_as<ZsetVal>()->note_expanded_delete_many(
            removed.count, removed.payload, zset_expanded(value)->allocation_bytes());
    }
    if (Op* source = shard.notify_source())
        notify_record(shard, *source, NOTIFY_ZSET,
                      maximum ? NotifyEventId::Zpopmax : NotifyEventId::Zpopmin, key);
    if (!value.entries()) {
        size_tracker.finish();
        if (notify) shard.store_erase<true>(hash, key);
        else shard.store().erase(hash, key);
    }
    return XshardPopResult::Popped;
}


namespace {

// Logical zset payload: per element [u64 score bits][u32 mlen][member] in (score,member) order,
// encoding byte 0.  Load re-adds through zset_add_one, so compact/skiplist follow CURRENT limits
// and inserting in sorted order keeps the compact ordered-insert scan O(1) per element.
template <typename Fn>
bool zset_walk(const CollectionRef& value, Fn&& fn) {
    if (value.encoding() == CollectionEncoding::Compact) {
        for (const Compact::Entry entry : value.compact()) {
            double score;
            Slice member;
            if (!compact_decode(entry, score, member)) return false;
            if (!fn(score, member)) return true;
        }
        return true;
    }
    for (const ZsetNode* node = zset_expanded(value)->header->level[0].forward; node;
         node = node->level[0].forward)
        if (!fn(node->score, node_member(node))) return true;
    return true;
}

SnapshotHookStatus zset_snapshot_begin(const KvObj& object, SnapshotSaveCursor& cursor,
                                       uint8_t& encoding) {
    if (static_cast<Type>(object.type) != Type::Zset) return SnapshotHookStatus::Corrupt;
    cursor = {};
    cursor.object = &object;
    encoding = 0;
    uint64_t total = 0;
    if (!zset_walk(zset_value(const_cast<KvObj*>(&object)),
                   [&](double, Slice member) { total += 12ull + member.n; return true; }))
        return SnapshotHookStatus::Corrupt;
    cursor.total = total;
    return SnapshotHookStatus::Ok;
}

SnapshotHookStatus zset_snapshot_read(SnapshotSaveCursor& cursor, uint8_t* destination,
                                      size_t capacity, size_t& written) {
    written = 0;
    if (!cursor.object) return SnapshotHookStatus::Corrupt;
    CollectionRef value = zset_value(const_cast<KvObj*>(cursor.object));
    SnapshotElementEmitter e{destination, capacity};
    uint64_t idx = 0;
    bool stopped = false;
    const bool walked = zset_walk(value, [&](double score, Slice member) {
        if (idx < cursor.lane[0]) { idx++; return true; }
        e.pos = 0;
        e.resume = idx == cursor.lane[0] ? cursor.lane[1] : 0;
        uint64_t bits;
        std::memcpy(&bits, &score, sizeof(bits));
        if (!(e.put_u64(bits) && e.put_u32(member.n) && e.put(member.p, member.n))) {
            cursor.lane[0] = idx;
            cursor.lane[1] = e.pos;
            stopped = true;
            return false;
        }
        idx++;
        return true;
    });
    if (!walked) return SnapshotHookStatus::Corrupt;
    if (!stopped) { cursor.lane[0] = idx; cursor.lane[1] = 0; }
    cursor.offset += e.out;
    written = e.out;
    return SnapshotHookStatus::Ok;
}

SnapshotHookStatus zset_snapshot_load(Slice key, uint8_t encoding, int64_t expire_at_ms,
                                      Slice payload, const TypeLimits& limits, KvObj*& result) {
    result = nullptr;
    if (encoding != 0) return SnapshotHookStatus::Corrupt;
    auto* value = new (std::nothrow) ZsetVal;
    if (!value) return SnapshotHookStatus::Oom;
    CollectionRef value_ref(value);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(payload.p);
    uint64_t left = payload.n;
    while (left) {
        if (left < 12) { delete value; return SnapshotHookStatus::Corrupt; }
        uint64_t bits = snapshot_get_u64(p);
        double score;
        std::memcpy(&score, &bits, sizeof(score));
        const uint32_t mlen = snapshot_get_u32(p + 8);
        p += 12; left -= 12;
        if (left < mlen || std::isnan(score)) { delete value; return SnapshotHookStatus::Corrupt; }
        const Slice member(reinterpret_cast<const char*>(p), mlen);
        p += mlen; left -= mlen;
        double out;
        const AddOutcome outcome = zset_add_one(value_ref, limits.zset, score, member,
                                                false, false, false, false, false, out);
        if (outcome != AddOutcome::Added) { delete value; return SnapshotHookStatus::Corrupt; }
    }
    result = kvobj_adopt_zset(key, value, expire_at_ms);
    if (!result) { delete value; return SnapshotHookStatus::Oom; }
    return SnapshotHookStatus::Ok;
}

}  // namespace

namespace {

// The expanded zset hangs off an external ZsetVal, so an embedded (in-KvObj) compact zset has to
// be moved out before it can be promoted -- the same two steps ZADD takes when a key outgrows the
// compact limits (externalize_zset + promote_zset), minus the reply plumbing.
template <bool kNotify>
bool zset_sort_promote_one(Shard& shard, uint64_t hash, Slice key) {
    KvObj* object = kNotify ? shard.store_find<true>(hash, key) : shard.store().find(hash, key);
    if (!object || static_cast<Type>(object->type) != Type::Zset) return true;
    if (CollectionRef(object).encoding() != CollectionEncoding::Compact) return true;
    if (CollectionRef(object).is_embedded()) {
        auto* moved = new (std::nothrow) ZsetVal;
        if (!moved) return false;
        for (const Compact::Entry entry : CollectionRef(object).compact())
            if (!moved->append(entry.value)) { delete moved; return false; }
        KvObj* replacement = kvobj_new_zset(object->key(), moved, object->expire_at_ms());
        if (!replacement) { delete moved; return false; }
        replacement->set_eviction_meta(object->eviction_meta());
        if (shard.store_insert<kNotify>(hash, replacement) != FlatStore::InsertResult::Inserted) {
            kvobj_free(replacement);
            return false;
        }
        object = replacement;
    }
    CollectionRef value(object);
    ObjectSizeTracker size_tracker(shard.store(), object);
    return promote_zset(value);
}

}  // namespace

void zset_sort_promote(Shard& shard, uint64_t hash, Slice key, bool notify) {
    if (notify) zset_sort_promote_one<true>(shard, hash, key);
    else zset_sort_promote_one<false>(shard, hash, key);
}

ZsetOwnerResult zset_owner_read(Shard& shard, Slice key, uint64_t hash, bool notify,
                                std::vector<ZsetEntry>& entries, int64_t& expire_at_ms) {
    entries.clear();
    KvObj* object = notify ? shard.store_find<true>(hash, key) : shard.store().find(hash, key);
    if (!object) { expire_at_ms = -1; return ZsetOwnerResult::Missing; }
    if (static_cast<Type>(object->type) != Type::Zset) return ZsetOwnerResult::WrongType;
    expire_at_ms = object->expire_at_ms();
    try {
        entries.reserve(CollectionRef(object).entries());
        if (!zset_walk(zset_value(object), [&](double score, Slice member) {
                entries.push_back({std::string(member.p, member.n), score});
                return true;
            })) return ZsetOwnerResult::Oom;
    } catch (const std::bad_alloc&) {
        entries.clear();
        return ZsetOwnerResult::Oom;
    }
    return ZsetOwnerResult::Ok;
}

ZsetOwnerResult zset_owner_replace(Shard& shard, Slice key, uint64_t hash, bool notify,
                                   const std::vector<ZsetEntry>& entries, int64_t expire_at_ms) {
    if (entries.empty()) {
        if (notify) shard.store_erase<true>(hash, key, FlatStore::EraseEvent::None);
        else shard.store().erase(hash, key);
        return ZsetOwnerResult::Ok;
    }
    auto* value = new (std::nothrow) ZsetVal;
    if (!value) return ZsetOwnerResult::Oom;
    CollectionRef ref(value);
    for (const ZsetEntry& entry : entries) {
        double resulting = 0;
        const AddOutcome added = zset_add_one(ref, shard.type_limits().zset, entry.score,
                                              Slice(entry.member.data(),
                                                    static_cast<uint32_t>(entry.member.size())),
                                              false, false, false, false, false, resulting);
        if (added != AddOutcome::Added) {
            delete value;
            return ZsetOwnerResult::Oom;
        }
    }
    KvObj* object = kvobj_adopt_zset(key, value, expire_at_ms);
    if (!object) { delete value; return ZsetOwnerResult::Oom; }
    const FlatStore::InsertResult inserted = notify
        ? shard.store_insert<true>(hash, object)
        : shard.store_insert<false>(hash, object);
    if (inserted == FlatStore::InsertResult::Inserted) return ZsetOwnerResult::Ok;
    kvobj_free(object);
    return inserted == FlatStore::InsertResult::MaxmemoryOom
        ? ZsetOwnerResult::Maxmemory : ZsetOwnerResult::InsertFailed;
}

SnapshotTypeHooks zset_snapshot_hooks() {
    return {zset_snapshot_begin, zset_snapshot_read, zset_snapshot_load};
}

CommandTable zset_command_table() {
    return {kTable, sizeof(kTable) / sizeof(kTable[0])};
}

}  // namespace tomo

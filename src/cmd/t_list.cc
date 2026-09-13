// t_list.cc — Redis-compatible single-key list commands.
//
// Small lists are one Compact. Expanded lists are a doubly linked list of Compact nodes; all
// ownership remains on the shard executor and no collection storage is borrowed by replies.
#include "command.h"
#include "blocking.h"
#include "notify.h"
#include "xshard.h"
#include "../core/shard.h"
#include "../exec/op.h"
#include "../net/resp.h"
#include "../snapshot/format.h"
#include "../store/kvobj.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace tomo {

void reply_maxmemory_oom(Op& op);
namespace {

// Redis/Valkey/our fork map list-max-listpack-size=-2 to 8 KiB. Dragonfly's QList uses the same
// default and the same 16-bit per-node count bound. An element larger than the byte limit lives in
// a one-element Compact node, matching quicklist's isolated plain-node policy while retaining this
// tree's one Compact node representation.
constexpr uint32_t kNodeMaxBytes = 8 * 1024;
constexpr uint32_t kNodeMaxEntries = std::numeric_limits<uint16_t>::max();

void reply_oom(Op& op) { reply_err(op.sink(), "ERR out of memory"); }
void reply_integer_error(Op& op) {
    reply_err(op.sink(), "ERR value is not an integer or out of range");
}
void reply_non_min_i64(Op& op) {
    reply_err(op.sink(), "ERR value is out of range, value must between -9223372036854775807 and 9223372036854775807");
}

bool parse_i64(Slice s, int64_t& out) {
    if (!s.n) return false;
    uint32_t pos = 0;
    bool negative = false;
    if (s.p[pos] == '-') {
        negative = true;
        if (++pos == s.n) return false;
    }
    // Canonical decimal, as redis's string2ll: the only accepted spelling of a number is the one
    // that formatting it produces again. No leading '+', no leading zeroes, no negative zero.
    // Without this "LPOP key 05" popped five elements where redis rejects the argument outright.
    if (s.p[pos] == '0') {
        if (negative || pos + 1 != s.n) return false;
        out = 0;
        return true;
    }
    if (s.p[pos] < '1' || s.p[pos] > '9') return false;
    uint64_t value = 0;
    const uint64_t limit = negative ? (uint64_t{1} << 63)
                                    : static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    for (; pos < s.n; pos++) {
        const char ch = s.p[pos];
        if (ch < '0' || ch > '9') return false;
        const uint32_t digit = static_cast<uint32_t>(ch - '0');
        if (value > (limit - digit) / 10) return false;
        value = value * 10 + digit;
    }
    out = negative ? (value == (uint64_t{1} << 63)
                          ? std::numeric_limits<int64_t>::min()
                          : -static_cast<int64_t>(value))
                   : static_cast<int64_t>(value);
    return true;
}

CollectionRef list_value(KvObj* object) { return CollectionRef(object); }

// ObjectSizeTracker now lives in flatstore.h (shared by every family).

void clear_nodes(ListVal& list) {
    ListNode* node = list.head;
    while (node) {
        ListNode* next = node->next;
        delete node;
        node = next;
    }
    list.head = list.tail = nullptr;
    list.node_allocation_bytes = 0;
}

void adopt_nodes(ListVal& dst, ListVal& src) {
    clear_nodes(dst);
    dst.head = src.head;
    dst.tail = src.tail;
    dst.node_allocation_bytes = src.node_allocation_bytes;
    src.head = src.tail = nullptr;
    src.node_allocation_bytes = 0;
}

bool node_can_insert(const ListNode* node, Slice value) {
    if (!node || value.n > kNodeMaxBytes || node->values.size() >= kNodeMaxEntries) return false;
    const uint64_t bytes = node->values.encoded_bytes();
    return bytes + Compact::entry_encoded_size(value.n) <= kNodeMaxBytes;
}

bool expanded_push(ListVal& list, Slice value, bool left) {
    ListNode* node = left ? list.head : list.tail;
    if (node_can_insert(node, value)) {
        const size_t old_capacity = node->values.capacity_bytes();
        const bool ok = left ? node->values.prepend(value) : node->values.append(value);
        list.node_allocation_bytes += node->values.capacity_bytes() - old_capacity;
        if (list.encoding() == CollectionEncoding::Deque)
            list.note_expanded_allocation(list.node_allocation_bytes);
        if (!ok) return false;
        return true;
    }

    auto* fresh = new (std::nothrow) ListNode;
    if (!fresh) return false;
    if (!fresh->values.append(value)) {
        delete fresh;
        return false;
    }
    if (!list.head) {
        list.head = list.tail = fresh;
    } else if (left) {
        fresh->next = list.head;
        list.head->prev = fresh;
        list.head = fresh;
    } else {
        fresh->prev = list.tail;
        list.tail->next = fresh;
        list.tail = fresh;
    }
    list.node_allocation_bytes += sizeof(ListNode) + fresh->values.capacity_bytes();
    return true;
}

bool expanded_edge(const ListVal& list, bool left, Compact::Entry& out) {
    const ListNode* node = left ? list.head : list.tail;
    return node && (left ? node->values.first(out) : node->values.last(out));
}

bool expanded_pop(ListVal& list, bool left, uint32_t& payload_size) {
    ListNode* node = left ? list.head : list.tail;
    if (!node) return false;
    const bool ok = left ? node->values.pop_front(&payload_size)
                         : node->values.pop_back(&payload_size);
    if (!ok) return false;
    if (!node->values.empty()) return true;

    if (node->prev) node->prev->next = node->next;
    else list.head = node->next;
    if (node->next) node->next->prev = node->prev;
    else list.tail = node->prev;
    list.node_allocation_bytes -= sizeof(ListNode) + node->values.capacity_bytes();
    delete node;
    return true;
}

class ListCursor {
public:
    static ListCursor seek(const CollectionRef& list, uint32_t logical_index) {
        ListCursor cur;
        if (logical_index >= list.entries()) return cur;
        cur.global_ = logical_index;
        if (list.encoding() == CollectionEncoding::Compact) {
            cur.pack_ = list.compact();
            cur.local_ = logical_index;
            cur.valid_ = true;
            return cur;
        }

        const ListVal* expanded = list.external_as<ListVal>();

        if (logical_index <= list.entries() / 2) {
            uint32_t remaining = logical_index;
            cur.node_ = expanded->head;
            while (cur.node_ && remaining >= cur.node_->values.size()) {
                remaining -= cur.node_->values.size();
                cur.node_ = cur.node_->next;
            }
            if (cur.node_) {
                cur.pack_ = CompactView(const_cast<Compact*>(&cur.node_->values));
                cur.local_ = remaining;
                cur.valid_ = true;
            }
        } else {
            uint32_t remaining = list.entries() - logical_index - 1;
            cur.node_ = expanded->tail;
            while (cur.node_ && remaining >= cur.node_->values.size()) {
                remaining -= cur.node_->values.size();
                cur.node_ = cur.node_->prev;
            }
            if (cur.node_) {
                cur.pack_ = CompactView(const_cast<Compact*>(&cur.node_->values));
                cur.local_ = cur.node_->values.size() - remaining - 1;
                cur.valid_ = true;
            }
        }
        return cur;
    }

    static ListCursor edge(const CollectionRef& list, bool reverse) {
        ListCursor cur = seek(list, reverse ? list.entries() - 1 : 0);
        cur.reverse_ = reverse;
        return cur;
    }

    bool valid() const { return valid_; }
    uint32_t position() const { return global_; }
    bool get(Compact::Entry& out) const { return valid_ && pack_.at(local_, out); }

    void next() {
        if (!valid_) return;
        if (!reverse_) {
            global_++;
            if (local_ + 1 < pack_.size()) {
                local_++;
                return;
            }
            if (!node_) {
                valid_ = false;
                return;
            }
            node_ = node_->next;
            if (!node_) {
                valid_ = false;
                return;
            }
            pack_ = CompactView(const_cast<Compact*>(&node_->values));
            local_ = 0;
        } else {
            if (global_) global_--;
            if (local_) {
                local_--;
                return;
            }
            if (!node_) {
                valid_ = false;
                return;
            }
            node_ = node_->prev;
            if (!node_) {
                valid_ = false;
                return;
            }
            pack_ = CompactView(const_cast<Compact*>(&node_->values));
            local_ = pack_.size() - 1;
        }
    }

private:
    const ListNode* node_ = nullptr;
    CompactView      pack_;
    uint32_t        local_ = 0;
    uint32_t        global_ = 0;
    bool            reverse_ = false;
    bool            valid_ = false;
};

bool append_all_expanded(const CollectionRef& source, ListVal& destination) {
    for (ListCursor cur = ListCursor::edge(source, false); cur.valid(); cur.next()) {
        Compact::Entry entry;
        if (!cur.get(entry) || !expanded_push(destination, entry.value, false)) return false;
    }
    return true;
}

template <bool kNotify>
bool externalize_list(Shard& shard, Op& op, KvObj*& object) {
    CollectionRef source(object);
    if (!source.is_embedded()) return true;
    auto* value = new (std::nothrow) ListVal;
    if (!value) { reply_oom(op); return false; }
    for (const Compact::Entry entry : source.compact()) {
        if (!value->append(entry.value)) {
            delete value;
            reply_oom(op);
            return false;
        }
    }
    KvObj* replacement = kvobj_new_list(object->key(), value,
                                        shard.store().deadline(op.hash, object),
                                        object->has_ttl_slot());
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

bool normalize_index(int64_t index, uint32_t size, uint32_t& normalized) {
    int64_t value = index;
    if (value < 0) value += static_cast<int64_t>(size);
    if (value < 0 || static_cast<uint64_t>(value) >= size) return false;
    normalized = static_cast<uint32_t>(value);
    return true;
}

bool normalize_range(int64_t start, int64_t stop, uint32_t size,
                     uint32_t& first, uint32_t& count) {
    if (!size) return false;
    if (start < 0) start += static_cast<int64_t>(size);
    if (stop < 0) stop += static_cast<int64_t>(size);
    if (start < 0) start = 0;
    if (start > stop || start >= static_cast<int64_t>(size) || stop < 0) return false;
    if (stop >= static_cast<int64_t>(size)) stop = static_cast<int64_t>(size) - 1;
    first = static_cast<uint32_t>(start);
    count = static_cast<uint32_t>(stop - start + 1);
    return true;
}

template <bool kNotify>
void push_generic(Shard& shard, Op& op, bool left, bool only_existing) {
    KvObj* object = shard.store_find<kNotify>(op.hash, op.key());
    if (!obj_type_check(object, Type::List, op.sink())) return;
    if (!object && only_existing) {
        reply_int(op.sink(), 0);
        return;
    }

    const uint32_t added = op.argc() - 2;
    uint64_t incoming_payload = 0;
    for (uint32_t i = 2; i < op.argc(); i++) incoming_payload += op.arg(i).n;

    if (!object) {
        auto* list = new (std::nothrow) ListVal;
        if (!list) { reply_oom(op); return; }
        const CompactLimit& limit = shard.type_limits().list;
        if (list->list_fits(limit, added, incoming_payload)) {
            for (uint32_t i = 2; i < op.argc(); i++) {
                const bool ok = left ? list->prepend(op.arg(i)) : list->append(op.arg(i));
                if (!ok) { delete list; reply_oom(op); return; }
            }
        } else {
            for (uint32_t i = 2; i < op.argc(); i++) {
                if (!expanded_push(*list, op.arg(i), left)) {
                    delete list;
                    reply_oom(op);
                    return;
                }
            }
            const uint64_t allocation = list->node_allocation_bytes;
            list->promote(CollectionEncoding::Deque, allocation);
            for (uint32_t i = 2; i < op.argc(); i++)
                list->note_expanded_insert(op.arg(i).n, allocation);
        }

        const uint32_t final_entries = list->entries();
        KvObj* fresh = kvobj_adopt_list(op.key(), list);
        if (!fresh) { delete list; reply_oom(op); return; }
        const FlatStore::InsertResult inserted_ = shard.store_insert<kNotify>(op.hash, fresh);
if (inserted_ != FlatStore::InsertResult::Inserted) {
    kvobj_free(fresh);
    if (inserted_ == FlatStore::InsertResult::MaxmemoryOom) reply_maxmemory_oom(op);
    else reply_err(op.sink(), "ERR keyspace insert failed");
    return;
        }
        if constexpr (kNotify)
            notify_record(shard, op, NOTIFY_LIST,
                          left ? NotifyEventId::Lpush : NotifyEventId::Rpush, op.key());
        reply_int(op.sink(), final_entries);
        if (shard.has_blocking_waiters()) blocking_publish_list_op(shard, op);
        return;
    }

    CollectionRef before = list_value(object);
    const uint64_t resulting_encoded = before.compact().encoded_bytes() + [&] {
        uint64_t bytes = 0;
        for (uint32_t i = 2; i < op.argc(); i++)
            bytes += Compact::entry_encoded_size(op.arg(i).n);
        return bytes;
    }();
    if (before.is_embedded() &&
        (!before.embedded_bytes_fit(resulting_encoded) ||
         !before.list_fits(shard.type_limits().list, before.entries() + added,
                           before.payload_bytes() + incoming_payload))) {
        if (!externalize_list<kNotify>(shard, op, object)) return;
    }
    CollectionRef list = list_value(object);
    ObjectSizeTracker size_tracker(shard.store(), object);
    if (static_cast<uint64_t>(list.entries()) + added > std::numeric_limits<uint32_t>::max()) {
        reply_err(op.sink(), "ERR list exceeds maximum allowed size");
        return;
    }

    if (list.encoding() == CollectionEncoding::Compact) {
        const uint32_t resulting_entries = list.entries() + added;
        const uint64_t resulting_payload = list.payload_bytes() + incoming_payload;
        if (list.list_fits(shard.type_limits().list, resulting_entries, resulting_payload)) {
            for (uint32_t i = 2; i < op.argc(); i++) {
                const bool ok = left ? list.prepend(op.arg(i)) : list.append(op.arg(i));
                if (!ok) { reply_oom(op); return; }
            }
        } else {
            ListVal staging;
            if (!append_all_expanded(list, staging)) { reply_oom(op); return; }
            for (uint32_t i = 2; i < op.argc(); i++) {
                if (!expanded_push(staging, op.arg(i), left)) { reply_oom(op); return; }
            }
            ListVal* expanded = list.external_as<ListVal>();
            adopt_nodes(*expanded, staging);
            const uint64_t allocation = expanded->node_allocation_bytes;
            expanded->promote(CollectionEncoding::Deque, allocation);
            for (uint32_t i = 2; i < op.argc(); i++)
                expanded->note_expanded_insert(op.arg(i).n, allocation);
        }
    } else {
        ListVal* expanded = list.external_as<ListVal>();
        for (uint32_t i = 2; i < op.argc(); i++) {
            if (!expanded_push(*expanded, op.arg(i), left)) { reply_oom(op); return; }
            expanded->note_expanded_insert(op.arg(i).n, expanded->node_allocation_bytes);
        }
    }
    if constexpr (kNotify)
        notify_record(shard, op, NOTIFY_LIST,
                      left ? NotifyEventId::Lpush : NotifyEventId::Rpush, op.key());
    reply_int(op.sink(), list.entries());
    if (shard.has_blocking_waiters()) blocking_publish_list_op(shard, op);
}

template <bool kNotify>
void cmd_lpush(Shard& shard, Op& op)  { push_generic<kNotify>(shard, op, true, false); }
template <bool kNotify>
void cmd_rpush(Shard& shard, Op& op)  { push_generic<kNotify>(shard, op, false, false); }
template <bool kNotify>
void cmd_lpushx(Shard& shard, Op& op) { push_generic<kNotify>(shard, op, true, true); }
template <bool kNotify>
void cmd_rpushx(Shard& shard, Op& op) { push_generic<kNotify>(shard, op, false, true); }

template <bool kNotify>
void pop_generic(Shard& shard, Op& op, bool left) {
    const bool has_count = op.argc() == 3;
    int64_t requested = 1;
    if (has_count) {
        // getRangeLongFromObject(0, LONG_MAX, msg): redis answers BOTH failures -- an unparseable
        // count and a well-formed negative one -- with the range message, never the generic one.
        if (!parse_i64(op.arg(2), requested) || requested < 0) {
            reply_err(op.sink(), "ERR value is out of range, must be positive");
            return;
        }
    }

    KvObj* object = shard.store_find<kNotify>(op.hash, op.key());
    if (!object) {
        if (has_count) reply_null_array(op.sink(), op.resp3());
        else reply_null(op.sink(), op.resp3());
        return;
    }
    if (!obj_type_check(object, Type::List, op.sink())) return;
    CollectionRef list = list_value(object);
    ObjectSizeTracker size_tracker(shard.store(), object);

    if (!list.entries()) {
        if (has_count) reply_null_array(op.sink(), op.resp3());
        else reply_null(op.sink(), op.resp3());
        size_tracker.finish();
        shard.store_erase<kNotify>(op.hash, op.key());
        return;
    }

    const uint32_t amount = has_count
        ? static_cast<uint32_t>(std::min<uint64_t>(static_cast<uint64_t>(requested), list.entries()))
        : 1;
    if (has_count) reply_array_header(op.sink(), amount);
    if (!amount) return;

    for (uint32_t i = 0; i < amount; i++) {
        Compact::Entry edge;
        const bool found = list.encoding() == CollectionEncoding::Compact
            ? (left ? list.compact().first(edge) : list.compact().last(edge))
            : expanded_edge(*list.external_as<ListVal>(), left, edge);
        if (!found) break;
        reply_bulk(op.sink(), edge.value);

        uint32_t payload = 0;
        if (list.encoding() == CollectionEncoding::Compact) {
            if (left) list.pop_front(&payload);
            else list.pop_back(&payload);
        } else {
            ListVal* expanded = list.external_as<ListVal>();
            expanded_pop(*expanded, left, payload);
            expanded->note_expanded_delete(payload, expanded->node_allocation_bytes);
        }
    }
    if constexpr (kNotify)
        notify_record(shard, op, NOTIFY_LIST,
                      left ? NotifyEventId::Lpop : NotifyEventId::Rpop, op.key());
    if (!list.entries()) {
        size_tracker.finish();
        shard.store_erase<kNotify>(op.hash, op.key());
    }
}

template <bool kNotify>
void cmd_lpop(Shard& shard, Op& op) { pop_generic<kNotify>(shard, op, true); }
template <bool kNotify>
void cmd_rpop(Shard& shard, Op& op) { pop_generic<kNotify>(shard, op, false); }

template <bool kNotify>
void cmd_llen(Shard& shard, Op& op) {
    KvObj* object = shard.store_find_read<kNotify>(op.hash, op.key());
    if (!obj_type_check(object, Type::List, op.sink())) return;
    reply_int(op.sink(), object ? list_value(object).entries() : 0);
}

template <bool kNotify>
void cmd_lindex(Shard& shard, Op& op) {
    KvObj* object = shard.store_find_read<kNotify>(op.hash, op.key());
    if (!object) { reply_null(op.sink(), op.resp3()); return; }
    if (!obj_type_check(object, Type::List, op.sink())) return;
    int64_t index = 0;
    if (!parse_i64(op.arg(2), index)) { reply_integer_error(op); return; }

    CollectionRef list = list_value(object);
    uint32_t normalized = 0;
    if (!normalize_index(index, list.entries(), normalized)) {
        reply_null(op.sink(), op.resp3()); return;
    }
    ListCursor cur = ListCursor::seek(list, normalized);
    Compact::Entry entry;
    if (!cur.get(entry)) { reply_null(op.sink(), op.resp3()); return; }
    reply_bulk(op.sink(), entry.value);
}

template <bool kNotify>
void cmd_lrange(Shard& shard, Op& op) {
    int64_t start = 0, stop = 0;
    if (!parse_i64(op.arg(2), start) || !parse_i64(op.arg(3), stop)) {
        reply_integer_error(op);
        return;
    }
    KvObj* object = shard.store_find_read<kNotify>(op.hash, op.key());
    if (!object) { reply_array_header(op.sink(), 0); return; }
    if (!obj_type_check(object, Type::List, op.sink())) return;

    CollectionRef list = list_value(object);
    uint32_t first = 0, count = 0;
    if (!normalize_range(start, stop, list.entries(), first, count)) {
        reply_array_header(op.sink(), 0);
        return;
    }
    reply_array_header(op.sink(), count);
    ListCursor cur = ListCursor::seek(list, first);
    for (uint32_t i = 0; i < count && cur.valid(); i++, cur.next()) {
        Compact::Entry entry;
        if (cur.get(entry)) reply_bulk(op.sink(), entry.value);
    }
}

bool build_replaced(const CollectionRef& source, uint32_t index, Slice value, ListVal& output) {
    for (ListCursor cur = ListCursor::edge(source, false); cur.valid(); cur.next()) {
        Compact::Entry entry;
        if (!cur.get(entry)) return false;
        if (!expanded_push(output, cur.position() == index ? value : entry.value, false)) return false;
    }
    return true;
}

template <bool kNotify>
void cmd_lset(Shard& shard, Op& op) {
    KvObj* object = shard.store_find<kNotify>(op.hash, op.key());
    if (!object) { reply_err(op.sink(), "ERR no such key"); return; }
    if (!obj_type_check(object, Type::List, op.sink())) return;
    int64_t index = 0;
    if (!parse_i64(op.arg(2), index)) { reply_integer_error(op); return; }

    CollectionRef initial = list_value(object);
    uint32_t normalized = 0;
    if (!normalize_index(index, initial.entries(), normalized)) {
        reply_err(op.sink(), "ERR index out of range");
        return;
    }
    ListCursor old_cursor = ListCursor::seek(initial, normalized);
    Compact::Entry old_entry;
    if (!old_cursor.get(old_entry)) { reply_err(op.sink(), "ERR index out of range"); return; }
    const uint32_t old_payload = old_entry.value.n;
    const Slice replacement = op.arg(3);
    const uint64_t resulting_encoded = initial.compact().encoded_bytes() - old_entry.span +
                                       Compact::entry_encoded_size(replacement.n);
    if (initial.is_embedded() &&
        (!initial.embedded_bytes_fit(resulting_encoded) ||
         !initial.list_fits(shard.type_limits().list, initial.entries(),
                            initial.payload_bytes() - old_payload + replacement.n))) {
        if (!externalize_list<kNotify>(shard, op, object)) return;
    }
    CollectionRef list = list_value(object);
    ObjectSizeTracker size_tracker(shard.store(), object);

    if (list.encoding() == CollectionEncoding::Compact &&
        list.list_fits(shard.type_limits().list, list.entries(),
                       list.payload_bytes() - old_payload + replacement.n)) {
        Compact::Entry direct;
        list.compact().at(normalized, direct);
        if (!list.replace(direct, replacement)) { reply_oom(op); return; }
    } else {
        ListVal staging;
        if (!build_replaced(list, normalized, replacement, staging)) { reply_oom(op); return; }
        ListVal* expanded = list.external_as<ListVal>();
        adopt_nodes(*expanded, staging);
        const uint64_t allocation = expanded->node_allocation_bytes;
        if (list.encoding() == CollectionEncoding::Compact)
            expanded->promote(CollectionEncoding::Deque, allocation);
        expanded->note_expanded_replace(old_payload, replacement.n, allocation);
    }
    if constexpr (kNotify)
        notify_record(shard, op, NOTIFY_LIST, NotifyEventId::Lset, op.key());
    reply_ok(op.sink());
}

template <bool kNotify>
void cmd_linsert(Shard& shard, Op& op) {
    bool before = false;
    if (op.arg(2).eq_icase("before")) before = true;
    else if (!op.arg(2).eq_icase("after")) { reply_syntax(op.sink()); return; }

    KvObj* object = shard.store_find<kNotify>(op.hash, op.key());
    if (!object) { reply_int(op.sink(), 0); return; }
    if (!obj_type_check(object, Type::List, op.sink())) return;
    CollectionRef initial = list_value(object);
    const uint64_t resulting_encoded = initial.compact().encoded_bytes() +
                                       Compact::entry_encoded_size(op.arg(4).n);
    if (initial.is_embedded() &&
        (!initial.embedded_bytes_fit(resulting_encoded) ||
         !initial.list_fits(shard.type_limits().list, initial.entries() + 1,
                            initial.payload_bytes() + op.arg(4).n))) {
        if (!externalize_list<kNotify>(shard, op, object)) return;
    }
    CollectionRef list = list_value(object);
    ObjectSizeTracker size_tracker(shard.store(), object);

    // Redis performs the append-size conversion before seeking the pivot so it never has to keep
    // a compact iterator alive across promotion. Consequently, a missing pivot may still leave a
    // one-way encoding promotion behind; preserve that observable OBJECT ENCODING behavior.
    if (list.encoding() == CollectionEncoding::Compact &&
        (list.entries() == std::numeric_limits<uint32_t>::max() ||
         !list.list_fits(shard.type_limits().list, list.entries() + 1,
                         list.payload_bytes() + op.arg(4).n))) {
        ListVal staging;
        if (!append_all_expanded(list, staging)) { reply_oom(op); return; }
        ListVal* expanded = list.external_as<ListVal>();
        adopt_nodes(*expanded, staging);
        expanded->promote(CollectionEncoding::Deque, expanded->node_allocation_bytes);
    }

    uint32_t pivot_index = 0;
    bool found = false;
    for (ListCursor cur = ListCursor::edge(list, false); cur.valid(); cur.next()) {
        Compact::Entry entry;
        if (cur.get(entry) && entry.value == op.arg(3)) {
            pivot_index = cur.position();
            found = true;
            break;
        }
    }
    if (!found) { reply_int(op.sink(), -1); return; }
    if (list.entries() == std::numeric_limits<uint32_t>::max()) {
        reply_err(op.sink(), "ERR list exceeds maximum allowed size");
        return;
    }

    const Slice inserted = op.arg(4);
    if (list.encoding() == CollectionEncoding::Compact) {
        Compact::Entry pivot;
        list.compact().at(pivot_index, pivot);
        const bool ok = before ? list.insert_before(pivot, inserted)
                               : list.insert_after(pivot, inserted);
        if (!ok) { reply_oom(op); return; }
    } else {
        ListVal staging;
        for (ListCursor cur = ListCursor::edge(list, false); cur.valid(); cur.next()) {
            Compact::Entry entry;
            if (!cur.get(entry)) { reply_oom(op); return; }
            if (cur.position() == pivot_index && before &&
                !expanded_push(staging, inserted, false)) { reply_oom(op); return; }
            if (!expanded_push(staging, entry.value, false)) { reply_oom(op); return; }
            if (cur.position() == pivot_index && !before &&
                !expanded_push(staging, inserted, false)) { reply_oom(op); return; }
        }
        ListVal* expanded = list.external_as<ListVal>();
        adopt_nodes(*expanded, staging);
        const uint64_t allocation = expanded->node_allocation_bytes;
        if (list.encoding() == CollectionEncoding::Compact)
            expanded->promote(CollectionEncoding::Deque, allocation);
        expanded->note_expanded_insert(inserted.n, allocation);
    }
    if constexpr (kNotify)
        notify_record(shard, op, NOTIFY_LIST, NotifyEventId::Linsert, op.key());
    reply_int(op.sink(), list.entries());
}

template <bool kNotify>
void cmd_lrem(Shard& shard, Op& op) {
    int64_t requested = 0;
    if (!parse_i64(op.arg(2), requested)) { reply_integer_error(op); return; }
    if (requested == std::numeric_limits<int64_t>::min()) {
        reply_non_min_i64(op);
        return;
    }

    KvObj* object = shard.store_find<kNotify>(op.hash, op.key());
    if (!object) { reply_int(op.sink(), 0); return; }
    if (!obj_type_check(object, Type::List, op.sink())) return;
    CollectionRef list = list_value(object);
    ObjectSizeTracker size_tracker(shard.store(), object);

    uint32_t matches = 0;
    for (ListCursor cur = ListCursor::edge(list, false); cur.valid(); cur.next()) {
        Compact::Entry entry;
        if (cur.get(entry) && entry.value == op.arg(3)) matches++;
    }
    const uint64_t limit = requested < 0 ? static_cast<uint64_t>(-requested)
                                         : static_cast<uint64_t>(requested);
    const uint32_t remove_count = requested == 0
        ? matches : static_cast<uint32_t>(std::min<uint64_t>(matches, limit));
    if (!remove_count) { reply_int(op.sink(), 0); return; }
    if (remove_count == list.entries()) {
        if constexpr (kNotify)
            notify_record(shard, op, NOTIFY_LIST, NotifyEventId::Lrem, op.key());
        reply_int(op.sink(), remove_count);
        size_tracker.finish();
        shard.store_erase<kNotify>(op.hash, op.key());
        return;
    }

    const bool from_tail = requested < 0;
    const uint32_t first_removed_match = from_tail ? matches - remove_count + 1 : 1;
    uint32_t seen_matches = 0;
    uint64_t removed_payload = 0;
    Compact compact;
    ListVal staging;
    for (ListCursor cur = ListCursor::edge(list, false); cur.valid(); cur.next()) {
        Compact::Entry entry;
        if (!cur.get(entry)) { reply_oom(op); return; }
        bool remove = false;
        if (entry.value == op.arg(3)) {
            seen_matches++;
            remove = from_tail ? seen_matches >= first_removed_match
                               : seen_matches <= remove_count;
        }
        if (remove) {
            removed_payload += entry.value.n;
        } else {
            const bool ok = list.encoding() == CollectionEncoding::Compact
                ? compact.append(entry.value) : expanded_push(staging, entry.value, false);
            if (!ok) { reply_oom(op); return; }
        }
    }

    if (list.encoding() == CollectionEncoding::Compact) {
        list.replace_compact(std::move(compact));
    } else {
        ListVal* expanded = list.external_as<ListVal>();
        adopt_nodes(*expanded, staging);
        expanded->note_expanded_delete_many(remove_count, removed_payload,
                                            expanded->node_allocation_bytes);
    }
    if constexpr (kNotify)
        notify_record(shard, op, NOTIFY_LIST, NotifyEventId::Lrem, op.key());
    reply_int(op.sink(), remove_count);
}

template <bool kNotify>
void cmd_ltrim(Shard& shard, Op& op) {
    int64_t start = 0, stop = 0;
    if (!parse_i64(op.arg(2), start) || !parse_i64(op.arg(3), stop)) {
        reply_integer_error(op);
        return;
    }
    KvObj* object = shard.store_find<kNotify>(op.hash, op.key());
    if (!object) { reply_ok(op.sink()); return; }
    if (!obj_type_check(object, Type::List, op.sink())) return;
    CollectionRef list = list_value(object);
    ObjectSizeTracker size_tracker(shard.store(), object);

    uint32_t first = 0, keep = 0;
    if (!normalize_range(start, stop, list.entries(), first, keep)) {
        if constexpr (kNotify)
            notify_record(shard, op, NOTIFY_LIST, NotifyEventId::Ltrim, op.key());
        size_tracker.finish();
        shard.store_erase<kNotify>(op.hash, op.key());
        reply_ok(op.sink());
        return;
    }
    if (first == 0 && keep == list.entries()) {
        if constexpr (kNotify)
            notify_record(shard, op, NOTIFY_LIST, NotifyEventId::Ltrim, op.key());
        reply_ok(op.sink()); return;
    }

    uint64_t retained_payload = 0;
    Compact compact;
    ListVal staging;
    ListCursor cur = ListCursor::seek(list, first);
    for (uint32_t i = 0; i < keep && cur.valid(); i++, cur.next()) {
        Compact::Entry entry;
        if (!cur.get(entry)) { reply_oom(op); return; }
        retained_payload += entry.value.n;
        const bool ok = list.encoding() == CollectionEncoding::Compact
            ? compact.append(entry.value) : expanded_push(staging, entry.value, false);
        if (!ok) { reply_oom(op); return; }
    }
    const uint32_t removed = list.entries() - keep;
    const uint64_t removed_payload = list.payload_bytes() - retained_payload;
    if (list.encoding() == CollectionEncoding::Compact) {
        list.replace_compact(std::move(compact));
    } else {
        ListVal* expanded = list.external_as<ListVal>();
        adopt_nodes(*expanded, staging);
        expanded->note_expanded_delete_many(removed, removed_payload,
                                            expanded->node_allocation_bytes);
    }
    if constexpr (kNotify)
        notify_record(shard, op, NOTIFY_LIST, NotifyEventId::Ltrim, op.key());
    reply_ok(op.sink());
}

struct LposOptions {
    int64_t rank = 1;
    int64_t count = -1;
    int64_t maxlen = 0;
    bool count_given = false;
};

bool parse_lpos_options(Op& op, LposOptions& options) {
    for (uint32_t i = 3; i < op.argc(); i++) {
        const Slice option = op.arg(i);
        const bool is_rank = option.eq_icase("rank");
        const bool is_count = option.eq_icase("count");
        const bool is_maxlen = option.eq_icase("maxlen");
        if (!is_rank && !is_count && !is_maxlen) {
            reply_syntax(op.sink());
            return false;
        }
        if (i + 1 >= op.argc()) { reply_syntax(op.sink()); return false; }
        int64_t value = 0;
        const bool parsed = parse_i64(op.arg(++i), value);
        if (!parsed) {
            // COUNT and MAXLEN name themselves for every failure (redis passes their message to
            // getRangeLongFromObject); RANK has no message and falls back to the generic one.
            if (is_count) { reply_err(op.sink(), "ERR COUNT can't be negative"); return false; }
            if (is_maxlen) { reply_err(op.sink(), "ERR MAXLEN can't be negative"); return false; }
            reply_integer_error(op);
            return false;
        }
        if (is_rank) {
            if (value == std::numeric_limits<int64_t>::min()) {
                reply_non_min_i64(op);
                return false;
            }
            if (!value) {
                reply_err(op.sink(), "ERR RANK can't be zero: use 1 to start from the first match, 2 from the second ... or use negative to start from the end of the list");
                return false;
            }
            options.rank = value;
        } else if (is_count) {
            if (value < 0) { reply_err(op.sink(), "ERR COUNT can't be negative"); return false; }
            options.count = value;
            options.count_given = true;
        } else if (is_maxlen) {
            if (value < 0) { reply_err(op.sink(), "ERR MAXLEN can't be negative"); return false; }
            options.maxlen = value;
        }
    }
    return true;
}

template <bool kNotify>
void cmd_lpos(Shard& shard, Op& op) {
    LposOptions options;
    if (!parse_lpos_options(op, options)) return;

    KvObj* object = shard.store_find_read<kNotify>(op.hash, op.key());
    if (!object) {
        if (options.count_given) reply_array_header(op.sink(), 0);
        else reply_null(op.sink(), op.resp3());
        return;
    }
    if (!obj_type_check(object, Type::List, op.sink())) return;
    CollectionRef list = list_value(object);
    const bool reverse = options.rank < 0;
    const uint64_t wanted_rank = reverse ? static_cast<uint64_t>(-options.rank)
                                         : static_cast<uint64_t>(options.rank);
    uint64_t scanned = 0;
    uint64_t matches = 0;

    if (!options.count_given) {
        for (ListCursor cur = ListCursor::edge(list, reverse); cur.valid(); cur.next()) {
            if (options.maxlen && scanned >= static_cast<uint64_t>(options.maxlen)) break;
            scanned++;
            Compact::Entry entry;
            if (cur.get(entry) && entry.value == op.arg(2) && ++matches >= wanted_rank) {
                reply_int(op.sink(), cur.position());
                return;
            }
        }
        reply_null(op.sink(), op.resp3());
        return;
    }

    std::vector<uint32_t> positions;
    for (ListCursor cur = ListCursor::edge(list, reverse); cur.valid(); cur.next()) {
        if (options.maxlen && scanned >= static_cast<uint64_t>(options.maxlen)) break;
        scanned++;
        Compact::Entry entry;
        if (!cur.get(entry) || !(entry.value == op.arg(2))) continue;
        matches++;
        if (matches < wanted_rank) continue;
        try {
            positions.push_back(cur.position());
        } catch (const std::bad_alloc&) {
            reply_oom(op);
            return;
        }
        if (options.count && positions.size() >= static_cast<uint64_t>(options.count)) break;
    }
    reply_array_header(op.sink(), positions.size());
    for (uint32_t position : positions) reply_int(op.sink(), position);
}

#define TOMO_HANDLER_PAIR(fn) fn<false>, 1, 1, 1, notify_handler<fn<true>>

static const CommandSpec kTable[] = {
    // name       min max flags               handler       first last step
    {"LPUSH",      3, -1, CmdFlags::Write | CmdFlags::DenyOom, TOMO_HANDLER_PAIR(cmd_lpush)},
    {"RPUSH",      3, -1, CmdFlags::Write | CmdFlags::DenyOom, TOMO_HANDLER_PAIR(cmd_rpush)},
    {"LPUSHX",     3, -1, CmdFlags::Write | CmdFlags::DenyOom, TOMO_HANDLER_PAIR(cmd_lpushx)},
    {"RPUSHX",     3, -1, CmdFlags::Write | CmdFlags::DenyOom, TOMO_HANDLER_PAIR(cmd_rpushx)},
    {"LPOP",       2,  3, CmdFlags::Write,    TOMO_HANDLER_PAIR(cmd_lpop)},
    {"RPOP",       2,  3, CmdFlags::Write,    TOMO_HANDLER_PAIR(cmd_rpop)},
    {"LLEN",       2,  2, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_llen)},
    {"LRANGE",     4,  4, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_lrange)},
    {"LINDEX",     3,  3, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_lindex)},
    {"LSET",       4,  4, CmdFlags::Write | CmdFlags::DenyOom, TOMO_HANDLER_PAIR(cmd_lset)},
    {"LINSERT",    5,  5, CmdFlags::Write | CmdFlags::DenyOom, TOMO_HANDLER_PAIR(cmd_linsert)},
    {"LREM",       4,  4, CmdFlags::Write,    TOMO_HANDLER_PAIR(cmd_lrem)},
    {"LTRIM",      4,  4, CmdFlags::Write,    TOMO_HANDLER_PAIR(cmd_ltrim)},
    {"LPOS",       3, -1, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_lpos)},
    {"BLPOP",      3, -1, CmdFlags::Write | CmdFlags::Blocking | CmdFlags::MultiShard,cmd_xshard_only,1,-1,1},
    {"BRPOP",      3, -1, CmdFlags::Write | CmdFlags::Blocking | CmdFlags::MultiShard,cmd_xshard_only,1,-1,1},
    {"BLMPOP",     5, -1, CmdFlags::Write | CmdFlags::Blocking | CmdFlags::MultiShard,cmd_xshard_only,3,-1,1},
    {"BLMOVE",     6,  6, CmdFlags::Write | CmdFlags::DenyOom | CmdFlags::Blocking | CmdFlags::MultiShard,cmd_xshard_only,1,2,1},
    {"BRPOPLPUSH", 4,  4, CmdFlags::Write | CmdFlags::DenyOom | CmdFlags::Blocking | CmdFlags::MultiShard,cmd_xshard_only,1,2,1},
    {"LMPOP",      4, -1, CmdFlags::Write | CmdFlags::MultiShard,cmd_xshard_only,2,-1,1},
    {"LMOVE",      5,  5, CmdFlags::Write | CmdFlags::DenyOom | CmdFlags::MultiShard,cmd_xshard_only,1,2,1},
    {"RPOPLPUSH",  3,  3, CmdFlags::Write | CmdFlags::DenyOom | CmdFlags::MultiShard,cmd_xshard_only,1,2,1},
};

#undef TOMO_HANDLER_PAIR

}  // namespace

XshardPopResult xshard_pop_list(Shard& shard, Slice key, uint64_t hash, bool left,
                                uint64_t count, std::vector<std::string>& elements) {
    elements.clear();
    const bool notify = shard.notify_carrier() != nullptr;
    KvObj* object = notify ? shard.store_find<true>(hash, key) : shard.store().find(hash, key);
    if (!object) return XshardPopResult::Missing;
    if (static_cast<Type>(object->type) != Type::List) return XshardPopResult::WrongType;
    CollectionRef list = list_value(object);
    if (!list.entries()) return XshardPopResult::Missing;
    const uint32_t take = static_cast<uint32_t>(
        std::min<uint64_t>(count, static_cast<uint64_t>(list.entries())));
    try {
        elements.reserve(take);
        for (ListCursor cur = ListCursor::edge(list, !left);
             cur.valid() && elements.size() < take; cur.next()) {
            Compact::Entry entry;
            if (!cur.get(entry)) return XshardPopResult::Oom;
            elements.emplace_back(entry.value.p, entry.value.n);
        }
    } catch (const std::bad_alloc&) {
        elements.clear();
        return XshardPopResult::Oom;
    }
    if (elements.size() != take) {
        elements.clear();
        return XshardPopResult::Oom;
    }

    ObjectSizeTracker size_tracker(shard.store(), object);
    for (uint32_t i = 0; i < take; i++) {
        uint32_t payload = 0;
        if (list.encoding() == CollectionEncoding::Compact) {
            if (left) list.pop_front(&payload);
            else list.pop_back(&payload);
        } else {
            ListVal* expanded = list.external_as<ListVal>();
            expanded_pop(*expanded, left, payload);
            expanded->note_expanded_delete(payload, expanded->node_allocation_bytes);
        }
    }
    if (Op* source = shard.notify_source())
        notify_record(shard, *source, NOTIFY_LIST,
                      left ? NotifyEventId::Lpop : NotifyEventId::Rpop, key);
    if (!list.entries()) {
        size_tracker.finish();
        if (notify) shard.store_erase<true>(hash, key);
        else shard.store().erase(hash, key);
    }
    return XshardPopResult::Popped;
}

XshardElementResult xshard_peek_list(KvObj* object, bool left, std::string& element) {
    element.clear();
    if (!object) return XshardElementResult::Missing;
    if (static_cast<Type>(object->type) != Type::List) return XshardElementResult::WrongType;
    CollectionRef list = list_value(object);
    if (!list.entries()) return XshardElementResult::Missing;
    Compact::Entry edge;
    const bool found = list.encoding() == CollectionEncoding::Compact
        ? (left ? list.compact().first(edge) : list.compact().last(edge))
        : expanded_edge(*list.external_as<ListVal>(), left, edge);
    if (!found) return XshardElementResult::Missing;
    try {
        element.assign(edge.value.p, edge.value.n);
    } catch (const std::bad_alloc&) {
        return XshardElementResult::Oom;
    }
    return XshardElementResult::Ok;
}

namespace {

template <bool kNotify>
XshardElementResult xshard_externalize_list(Shard& shard, Slice key, uint64_t hash,
                                            KvObj*& object) {
    CollectionRef source(object);
    if (!source.is_embedded()) return XshardElementResult::Ok;
    auto* value = new (std::nothrow) ListVal;
    if (!value) return XshardElementResult::Oom;
    for (const Compact::Entry entry : source.compact()) {
        if (!value->append(entry.value)) {
            delete value;
            return XshardElementResult::Oom;
        }
    }
    KvObj* replacement = kvobj_new_list(key, value, shard.store().deadline(hash, object),
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
XshardElementResult xshard_remove_list_element_impl(Shard& shard, Slice key, uint64_t hash,
                                                    bool left, Slice expected) {
    KvObj* object = shard.store_find<kNotify>(hash, key);
    if (!object) return XshardElementResult::Missing;
    if (static_cast<Type>(object->type) != Type::List) return XshardElementResult::WrongType;
    CollectionRef list = list_value(object);
    if (!list.entries()) return XshardElementResult::Missing;

    Compact::Entry edge;
    const bool found_edge = list.encoding() == CollectionEncoding::Compact
        ? (left ? list.compact().first(edge) : list.compact().last(edge))
        : expanded_edge(*list.external_as<ListVal>(), left, edge);
    if (!found_edge) return XshardElementResult::Missing;

    if (edge.value == expected) {
        ObjectSizeTracker size_tracker(shard.store(), object);
        uint32_t payload = 0;
        if (list.encoding() == CollectionEncoding::Compact) {
            if (left) list.pop_front(&payload);
            else list.pop_back(&payload);
        } else {
            ListVal* expanded = list.external_as<ListVal>();
            expanded_pop(*expanded, left, payload);
            expanded->note_expanded_delete(payload, expanded->node_allocation_bytes);
        }
        if (!list.entries()) {
            size_tracker.finish();
            shard.store_erase<kNotify>(hash, key);
        }
        return XshardElementResult::Ok;
    }

    // A write to the selected edge landed between hops. Preserve it and remove the closest
    // occurrence of the element hop one actually selected. This slow fallback is concurrency-only;
    // the uncontended mover remains one edge read and one edge pop.
    uint32_t matches = 0;
    if (!left) {
        for (ListCursor cur = ListCursor::edge(list, false); cur.valid(); cur.next()) {
            Compact::Entry entry;
            if (!cur.get(entry)) return XshardElementResult::Oom;
            matches += entry.value == expected;
        }
        if (!matches) return XshardElementResult::Missing;
    }
    uint32_t seen = 0;
    uint32_t removed_payload = 0;
    bool removed = false;
    Compact compact;
    ListVal staging;
    for (ListCursor cur = ListCursor::edge(list, false); cur.valid(); cur.next()) {
        Compact::Entry entry;
        if (!cur.get(entry)) return XshardElementResult::Oom;
        bool take = false;
        if (entry.value == expected) {
            seen++;
            take = left ? !removed : seen == matches;
        }
        if (take) {
            removed = true;
            removed_payload = entry.value.n;
            continue;
        }
        const bool ok = list.encoding() == CollectionEncoding::Compact
            ? compact.append(entry.value) : expanded_push(staging, entry.value, false);
        if (!ok) return XshardElementResult::Oom;
    }
    if (!removed) return XshardElementResult::Missing;

    ObjectSizeTracker size_tracker(shard.store(), object);
    if (list.encoding() == CollectionEncoding::Compact) {
        if (!list.replace_compact(std::move(compact))) return XshardElementResult::Oom;
    } else {
        ListVal* expanded = list.external_as<ListVal>();
        adopt_nodes(*expanded, staging);
        expanded->note_expanded_delete(removed_payload, expanded->node_allocation_bytes);
    }
    if (!list.entries()) {
        size_tracker.finish();
        shard.store_erase<kNotify>(hash, key);
    }
    return XshardElementResult::Ok;
}

template <bool kNotify>
XshardElementResult xshard_push_list_element_impl(Shard& shard, Slice key, uint64_t hash,
                                                  bool left, Slice element) {
    KvObj* object = shard.store_find<kNotify>(hash, key);
    if (object && static_cast<Type>(object->type) != Type::List)
        return XshardElementResult::WrongType;

    if (!object) {
        auto* list = new (std::nothrow) ListVal;
        if (!list) return XshardElementResult::Oom;
        if (list->list_fits(shard.type_limits().list, 1, element.n)) {
            if (!list->append(element)) {
                delete list;
                return XshardElementResult::Oom;
            }
        } else {
            if (!expanded_push(*list, element, left)) {
                delete list;
                return XshardElementResult::Oom;
            }
            const uint64_t allocation = list->node_allocation_bytes;
            list->promote(CollectionEncoding::Deque, allocation);
            list->note_expanded_insert(element.n, allocation);
        }
        KvObj* fresh = kvobj_adopt_list(key, list);
        if (!fresh) {
            delete list;
            return XshardElementResult::Oom;
        }
        const FlatStore::InsertResult inserted = shard.store_insert<kNotify>(hash, fresh);
        if (inserted != FlatStore::InsertResult::Inserted) {
            kvobj_free(fresh);
            return inserted == FlatStore::InsertResult::MaxmemoryOom
                ? XshardElementResult::Maxmemory : XshardElementResult::InsertFailed;
        }
        if (shard.has_blocking_waiters()) blocking_publish_key(shard, hash, key.p, key.n);
        return XshardElementResult::Ok;
    }

    CollectionRef before = list_value(object);
    const uint64_t resulting_encoded =
        before.compact().encoded_bytes() + Compact::entry_encoded_size(element.n);
    if (before.is_embedded() &&
        (!before.embedded_bytes_fit(resulting_encoded) ||
         !before.list_fits(shard.type_limits().list, before.entries() + 1,
                           before.payload_bytes() + element.n))) {
        const XshardElementResult converted =
            xshard_externalize_list<kNotify>(shard, key, hash, object);
        if (converted != XshardElementResult::Ok) return converted;
    }

    CollectionRef list = list_value(object);
    if (list.entries() == std::numeric_limits<uint32_t>::max())
        return XshardElementResult::Oom;
    ObjectSizeTracker size_tracker(shard.store(), object);
    if (list.encoding() == CollectionEncoding::Compact) {
        const uint32_t resulting_entries = list.entries() + 1;
        const uint64_t resulting_payload = list.payload_bytes() + element.n;
        if (list.list_fits(shard.type_limits().list, resulting_entries, resulting_payload)) {
            if (!(left ? list.prepend(element) : list.append(element)))
                return XshardElementResult::Oom;
        } else {
            ListVal staging;
            if (!append_all_expanded(list, staging) || !expanded_push(staging, element, left))
                return XshardElementResult::Oom;
            ListVal* expanded = list.external_as<ListVal>();
            adopt_nodes(*expanded, staging);
            const uint64_t allocation = expanded->node_allocation_bytes;
            expanded->promote(CollectionEncoding::Deque, allocation);
            expanded->note_expanded_insert(element.n, allocation);
        }
    } else {
        ListVal* expanded = list.external_as<ListVal>();
        if (!expanded_push(*expanded, element, left)) return XshardElementResult::Oom;
        expanded->note_expanded_insert(element.n, expanded->node_allocation_bytes);
    }
    if (shard.has_blocking_waiters()) blocking_publish_key(shard, hash, key.p, key.n);
    return XshardElementResult::Ok;
}

}  // namespace

XshardElementResult xshard_remove_list_element(Shard& shard, Slice key, uint64_t hash,
                                               bool left, Slice expected) {
    return shard.notify_carrier()
        ? xshard_remove_list_element_impl<true>(shard, key, hash, left, expected)
        : xshard_remove_list_element_impl<false>(shard, key, hash, left, expected);
}

XshardElementResult xshard_push_list_element(Shard& shard, Slice key, uint64_t hash,
                                             bool left, Slice element) {
    return shard.notify_carrier()
        ? xshard_push_list_element_impl<true>(shard, key, hash, left, element)
        : xshard_push_list_element_impl<false>(shard, key, hash, left, element);
}


namespace {

// Logical list payload: per element [u32 len][bytes], front to back, encoding byte 0.  ListCursor
// resumes the walk at lane[0] in O(min(i, n-i)) instead of a front rescan per chunk.
SnapshotHookStatus list_snapshot_begin(const KvObj& object, SnapshotSaveCursor& cursor,
                                       uint8_t& encoding) {
    if (static_cast<Type>(object.type) != Type::List) return SnapshotHookStatus::Corrupt;
    cursor = {};
    cursor.object = &object;
    encoding = 0;
    CollectionRef list = list_value(const_cast<KvObj*>(&object));
    cursor.total = 4ull * list.entries() + list.payload_bytes();
    return SnapshotHookStatus::Ok;
}

SnapshotHookStatus list_snapshot_read(SnapshotSaveCursor& cursor, uint8_t* destination,
                                      size_t capacity, size_t& written) {
    written = 0;
    if (!cursor.object) return SnapshotHookStatus::Corrupt;
    CollectionRef list = list_value(const_cast<KvObj*>(cursor.object));
    SnapshotElementEmitter e{destination, capacity};
    uint64_t idx = cursor.lane[0];
    bool stopped = false;
    for (ListCursor cur = ListCursor::seek(list, static_cast<uint32_t>(idx)); cur.valid();
         cur.next(), idx++) {
        Compact::Entry entry;
        if (!cur.get(entry)) return SnapshotHookStatus::Corrupt;
        e.pos = 0;
        e.resume = idx == cursor.lane[0] ? cursor.lane[1] : 0;
        if (!(e.put_u32(entry.value.n) && e.put(entry.value.p, entry.value.n))) {
            cursor.lane[0] = idx;
            cursor.lane[1] = e.pos;
            stopped = true;
            break;
        }
    }
    if (!stopped) { cursor.lane[0] = idx; cursor.lane[1] = 0; }
    cursor.offset += e.out;
    written = e.out;
    return SnapshotHookStatus::Ok;
}

SnapshotHookStatus list_snapshot_load(Slice key, uint8_t encoding, int64_t expire_at_ms,
                                      Slice payload, const TypeLimits& limits, KvObj*& result) {
    result = nullptr;
    if (encoding != 0) return SnapshotHookStatus::Corrupt;
    auto* list = new (std::nothrow) ListVal;
    if (!list) return SnapshotHookStatus::Oom;
    // First pass: count and validate, so the compact-vs-deque decision mirrors a fresh RPUSH.
    const uint8_t* p = reinterpret_cast<const uint8_t*>(payload.p);
    uint64_t left = payload.n, count = 0, bytes = 0;
    while (left) {
        if (left < 4) { delete list; return SnapshotHookStatus::Corrupt; }
        const uint32_t len = snapshot_get_u32(p);
        if (left - 4 < len) { delete list; return SnapshotHookStatus::Corrupt; }
        count++; bytes += len;
        p += 4ull + len; left -= 4ull + len;
    }
    const bool compact = list->list_fits(limits.list, static_cast<uint32_t>(count), bytes);
    p = reinterpret_cast<const uint8_t*>(payload.p);
    left = payload.n;
    while (left) {
        const uint32_t len = snapshot_get_u32(p);
        const Slice value(reinterpret_cast<const char*>(p) + 4, len);
        p += 4ull + len; left -= 4ull + len;
        const bool ok = compact ? list->append(value) : expanded_push(*list, value, false);
        if (!ok) { delete list; return SnapshotHookStatus::Oom; }
    }
    if (!compact) {
        const uint64_t allocation = list->node_allocation_bytes;
        list->promote(CollectionEncoding::Deque, allocation);
        p = reinterpret_cast<const uint8_t*>(payload.p);
        left = payload.n;
        while (left) {
            const uint32_t len = snapshot_get_u32(p);
            list->note_expanded_insert(len, allocation);
            p += 4ull + len; left -= 4ull + len;
        }
    }
    result = kvobj_adopt_list(key, list, expire_at_ms);
    if (!result) { delete list; return SnapshotHookStatus::Oom; }
    return SnapshotHookStatus::Ok;
}

}  // namespace

SnapshotTypeHooks list_snapshot_hooks() {
    return {list_snapshot_begin, list_snapshot_read, list_snapshot_load};
}

CommandTable list_command_table() {
    return {kTable, sizeof(kTable) / sizeof(kTable[0])};
}

}  // namespace tomo

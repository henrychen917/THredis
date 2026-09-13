// t_stream_groups.cc -- Redis-compatible stream consumer groups and stream metadata.
//
// Every structure below is reached through StreamVal::groups and is created only by XGROUP.
// The stream and this cold state remain owned by one executor shard at all times.
#include "command.h"
#include "t_stream.h"
#include "../core/shard.h"
#include "../exec/op.h"
#include "../net/resp.h"
#include "../snapshot/format.h"
#include "../store/kvobj.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace tomo {
namespace {

constexpr uint32_t kMaxIdText = 128;

int id_compare(const StreamID& a, const StreamID& b) {
    if (a.ms != b.ms) return a.ms < b.ms ? -1 : 1;
    if (a.seq != b.seq) return a.seq < b.seq ? -1 : 1;
    return 0;
}

struct IdLess {
    bool operator()(const StreamID& a, const StreamID& b) const { return id_compare(a, b) < 0; }
};

bool parse_u64_exact(Slice input, uint64_t& value) {
    if (!input.n) return false;
    const char* end = input.p + input.n;
    const auto parsed = std::from_chars(input.p, end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool parse_i64_exact(Slice input, int64_t& value) {
    if (!input.n) return false;
    const char* end = input.p + input.n;
    const auto parsed = std::from_chars(input.p, end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

// Stream IDs keep string2ull semantics (see t_stream.cc), so "0005-1" is 5-1 on both servers.
// Numeric OPTIONS take string2ll's canonical spelling: no '+', no leading zeroes, no "-0".
bool parse_i64_option(Slice input, int64_t& value) {
    if (!input.n || input.n > 20) return false;
    uint32_t i = 0;
    bool negative = false;
    if (input.p[0] == '-') { negative = true; i = 1; }
    if (i >= input.n) return false;
    if (input.p[i] == '0') {
        if (negative || i + 1 != input.n) return false;
        value = 0;
        return true;
    }
    if (input.p[i] < '1' || input.p[i] > '9') return false;
    uint64_t magnitude = 0;
    const uint64_t limit = negative ? (uint64_t{1} << 63) : uint64_t{INT64_MAX};
    for (; i < input.n; i++) {
        const char ch = input.p[i];
        if (ch < '0' || ch > '9') return false;
        const uint32_t digit = static_cast<uint32_t>(ch - '0');
        if (magnitude > (limit - digit) / 10) return false;
        magnitude = magnitude * 10 + digit;
    }
    value = negative ? (magnitude == (uint64_t{1} << 63) ? INT64_MIN
                                                         : -static_cast<int64_t>(magnitude))
                     : static_cast<int64_t>(magnitude);
    return true;
}

bool parse_id(Slice input, StreamID& id, uint64_t missing_seq = 0,
              bool sentinels = false) {
    if (!input.n || input.n > kMaxIdText) return false;
    if (sentinels && input.n == 1 && input.p[0] == '-') { id = {}; return true; }
    if (sentinels && input.n == 1 && input.p[0] == '+') {
        id = {UINT64_MAX, UINT64_MAX}; return true;
    }
    const char* dash = static_cast<const char*>(std::memchr(input.p, '-', input.n));
    if (!dash) {
        if (!parse_u64_exact(input, id.ms)) return false;
        id.seq = missing_seq;
        return true;
    }
    const uint32_t first = static_cast<uint32_t>(dash - input.p);
    if (!first || first + 1 >= input.n ||
        std::memchr(dash + 1, '-', input.n - first - 1)) return false;
    return parse_u64_exact(Slice(input.p, first), id.ms) &&
           parse_u64_exact(Slice(dash + 1, input.n - first - 1), id.seq);
}

bool id_increment(StreamID& id) {
    if (id.seq != UINT64_MAX) { id.seq++; return true; }
    if (id.ms == UINT64_MAX) return false;
    id.ms++; id.seq = 0; return true;
}

void invalid_id(Op& op) {
    reply_err(op.sink(), "ERR Invalid stream ID specified as stream command argument");
}

template <typename Sink>
void reply_id_to(Sink& sink, const StreamID& id) {
    char text[41];
    uint32_t length = u64_to_dec(text, id.ms);
    text[length++] = '-';
    length += u64_to_dec(text + length, id.seq);
    reply_bulk(sink, Slice(text, length));
}

void reply_id(Op& op, const StreamID& id) {
    auto sink = op.sink();
    reply_id_to(sink, id);
}

struct StreamConsumer {
    int64_t seen_time = -1;
    int64_t active_time = -1;
};

struct StreamPending {
    std::string consumer;
    int64_t delivery_time = 0;
    uint64_t delivery_count = 0;
};

using PendingMap = std::map<StreamID, StreamPending, IdLess>;

struct StreamGroup {
    StreamID last_delivered{};
    int64_t entries_read = -1;
    std::map<std::string, StreamConsumer, std::less<>> consumers;
    PendingMap pending;
};

struct StreamGroups {
    std::map<std::string, StreamGroup, std::less<>> groups;
    uint64_t bytes_ = sizeof(StreamGroups);

    void note_insert(uint64_t bytes) { bytes_ += bytes; }
    void note_delete(uint64_t bytes) { bytes_ -= bytes; }
    void note_capacity_change(uint64_t before, uint64_t after) {
        if (after >= before) bytes_ += after - before;
        else bytes_ -= before - after;
    }
};

constexpr uint64_t kStreamGroupMapNodeBytes = 64;

uint64_t consumer_allocation_bytes(const std::string& name) {
    return sizeof(StreamConsumer) + name.capacity() + kStreamGroupMapNodeBytes;
}

uint64_t pending_allocation_bytes(const StreamPending& pending) {
    return sizeof(StreamPending) + pending.consumer.capacity() + kStreamGroupMapNodeBytes;
}

uint64_t group_allocation_bytes(const std::string& name, const StreamGroup& group) {
    uint64_t bytes = sizeof(StreamGroup) + name.capacity() + kStreamGroupMapNodeBytes;
    for (const auto& [consumer_name, consumer] : group.consumers) {
        (void)consumer;
        bytes += consumer_allocation_bytes(consumer_name);
    }
    for (const auto& [id, pending] : group.pending) {
        (void)id;
        bytes += pending_allocation_bytes(pending);
    }
    return bytes;
}

uint64_t recompute_groups_allocation_bytes(const StreamGroups& groups) {
    uint64_t bytes = sizeof(StreamGroups);
    for (const auto& [name, group] : groups.groups)
        bytes += group_allocation_bytes(name, group);
    return bytes;
}

StreamVal* stream_value(KvObj* object) {
    if (!object || static_cast<Type>(object->type) != Type::Stream ||
        static_cast<Enc>(object->enc) != Enc::Extern) return nullptr;
    return static_cast<StreamVal*>(object->external_ptr());
}

StreamGroups* groups_of(KvObj* object) {
    StreamVal* stream = stream_value(object);
    return stream ? static_cast<StreamGroups*>(stream->groups) : nullptr;
}

StreamGroup* find_group(KvObj* object, Slice name) {
    StreamGroups* groups = groups_of(object);
    if (!groups) return nullptr;
    const auto it = groups->groups.find(std::string_view(name.p, name.n));
    return it == groups->groups.end() ? nullptr : &it->second;
}

bool ensure_group_container(StreamVal& stream, StreamGroups*& groups) {
    groups = static_cast<StreamGroups*>(stream.groups);
    if (groups) return true;
    groups = new (std::nothrow) StreamGroups;
    if (!groups) return false;
    stream.groups = groups;
    return true;
}

void maybe_release_groups(StreamVal& stream) {
    auto* groups = static_cast<StreamGroups*>(stream.groups);
    if (groups && groups->groups.empty()) {
        delete groups;
        stream.groups = nullptr;
    }
}

void reply_nogroup(Op& op, Slice key, Slice group, bool xreadgroup = false) {
    auto sink = op.sink();
    sink.push_back('-');
    sink.append("NOGROUP No such key '", sizeof("NOGROUP No such key '") - 1);
    sink.append(key.p, key.n);
    sink.append("' or consumer group '", sizeof("' or consumer group '") - 1);
    sink.append(group.p, group.n);
    if (xreadgroup)
        sink.append("' in XREADGROUP with GROUP option\r\n",
                    sizeof("' in XREADGROUP with GROUP option\r\n") - 1);
    else sink.append("'\r\n", sizeof("'\r\n") - 1);
}

void reply_nogroup_subcommand(Op& op, Slice key, Slice group) {
    auto sink = op.sink();
    sink.push_back('-');
    sink.append("NOGROUP No such consumer group '",
                sizeof("NOGROUP No such consumer group '") - 1);
    sink.append(group.p, group.n);
    sink.append("' for key name '", sizeof("' for key name '") - 1);
    sink.append(key.p, key.n);
    sink.append("'\r\n", sizeof("'\r\n") - 1);
}

void reply_entry(Op& op, const StreamOwnedEntry& entry, bool tombstone_null) {
    reply_array_header(op.sink(), 2);
    reply_id(op, entry.id);
    if (entry.deleted && tombstone_null) {
        reply_null_array(op.sink(), op.resp3());
        return;
    }
    reply_array_header(op.sink(), entry.fields.size() * 2);
    for (size_t i = 0; i < entry.fields.size(); i++) {
        reply_bulk(op.sink(), Slice(entry.fields[i].data(), entry.fields[i].size()));
        reply_bulk(op.sink(), Slice(entry.values[i].data(), entry.values[i].size()));
    }
}

void reply_stream_entries(Op& op, Slice key, const std::vector<StreamOwnedEntry>& entries,
                          bool tombstone_null) {
    if (op.resp3()) reply_map_header(op.sink(), 1, true);
    else reply_array_header(op.sink(), 1);
    if (!op.resp3()) reply_array_header(op.sink(), 2);
    reply_bulk(op.sink(), key);
    reply_array_header(op.sink(), entries.size());
    for (const StreamOwnedEntry& entry : entries) reply_entry(op, entry, tombstone_null);
}

bool create_consumer(StreamGroups& groups, StreamGroup& group, Slice name, int64_t now,
                     StreamConsumer*& consumer, bool& created) {
    created = false;
    try {
        auto [it, inserted] = group.consumers.try_emplace(std::string(name.p, name.n));
        consumer = &it->second;
        if (inserted) {
            consumer->seen_time = now;
            groups.note_insert(consumer_allocation_bytes(it->first));
            created = true;
        }
        return true;
    } catch (const std::bad_alloc&) {
        consumer = nullptr;
        return false;
    }
}

void erase_pending(StreamGroups& groups, StreamGroup& group,
                   PendingMap::iterator pending) {
    groups.note_delete(pending_allocation_bytes(pending->second));
    group.pending.erase(pending);
}

void assign_pending_consumer(StreamGroups& groups, StreamPending& pending, Slice consumer) {
    const uint64_t old_capacity = pending.consumer.capacity();
    pending.consumer.assign(consumer.p, consumer.n);
    groups.note_capacity_change(old_capacity, pending.consumer.capacity());
}

void upsert_pending(StreamGroups& groups, StreamGroup& group, const StreamID& id,
                    StreamPending&& replacement) {
    const auto old = group.pending.find(id);
    const uint64_t old_bytes = old == group.pending.end()
        ? 0 : pending_allocation_bytes(old->second);
    const auto [it, inserted] = group.pending.insert_or_assign(id, std::move(replacement));
    const uint64_t new_bytes = pending_allocation_bytes(it->second);
    if (inserted) groups.note_insert(new_bytes);
    else groups.note_capacity_change(old_bytes, new_bytes);
}

uint64_t pending_for(const StreamGroup& group, const std::string& consumer) {
    uint64_t count = 0;
    for (const auto& [id, pending] : group.pending) {
        (void)id;
        count += pending.consumer == consumer;
    }
    return count;
}

int64_t infer_entries_read(KvObj* object, const StreamID& delivered) {
    StreamHeader header;
    if (!stream_object_header(object, header)) return -1;
    std::vector<StreamOwnedEntry> entries;
    if (!stream_object_collect(object, {}, false, 0, true, entries)) return -1;
    bool exact = delivered.ms == 0 && delivered.seq == 0;
    uint64_t after = 0;
    for (const StreamOwnedEntry& entry : entries) {
        if (id_compare(entry.id, delivered) == 0) exact = true;
        if (id_compare(entry.id, delivered) > 0) after++;
    }
    if (!exact || after > header.entries_added || header.entries_added - after > INT64_MAX)
        return -1;
    return static_cast<int64_t>(header.entries_added - after);
}

bool group_lag(KvObj* object, const StreamHeader& header, const StreamGroup& group,
               int64_t& lag) {
    StreamOwnedEntry cursor; bool cursor_found = false;
    if (!stream_object_find(object, group.last_delivered, cursor, cursor_found)) return false;
    if (cursor_found && cursor.deleted) return false;
    const int64_t inferred = infer_entries_read(object, group.last_delivered);
    if (group.entries_read >= 0 && inferred >= 0 && inferred != group.entries_read) {
        if (header.entries_added > static_cast<uint64_t>(INT64_MAX)) return false;
        lag = static_cast<int64_t>(header.entries_added) - group.entries_read;
        return true;
    }
    std::vector<StreamOwnedEntry> remaining;
    if (!stream_object_collect(object, group.last_delivered, true, 0, false, remaining) ||
        remaining.size() > static_cast<size_t>(INT64_MAX)) return false;
    lag = static_cast<int64_t>(remaining.size());
    return true;
}

bool object_last_physical(KvObj* object, StreamID& id) {
    std::vector<StreamOwnedEntry> entries;
    if (!stream_object_collect(object, {}, false, 0, true, entries)) return false;
    id = entries.empty() ? StreamID{} : entries.back().id;
    return true;
}

const char* const kXgroupHelp[] = {
    "XGROUP <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
    "CREATE <key> <groupname> <id|$> [option]",
    "    Create a new consumer group. Options are:",
    "    * MKSTREAM",
    "      Create the empty stream if it does not exist.",
    "    * ENTRIESREAD entries_read",
    "      Set the group's entries_read counter (internal use).",
    "CREATECONSUMER <key> <groupname> <consumer>",
    "    Create a new consumer in the specified group.",
    "DELCONSUMER <key> <groupname> <consumer>",
    "    Remove the specified consumer.",
    "DESTROY <key> <groupname>",
    "    Remove the specified group.",
    "SETID <key> <groupname> <id|$> [ENTRIESREAD entries_read]",
    "    Set the current group ID and entries_read counter.",
    "HELP",
    "    Print this help.",
};

const char* const kXinfoHelp[] = {
    "XINFO <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
    "CONSUMERS <key> <groupname>",
    "    Show consumers of <groupname>.",
    "GROUPS <key>",
    "    Show the stream consumer groups.",
    "STREAM <key> [FULL [COUNT <count>]",
    "    Show information about the stream.",
    "HELP",
    "    Print this help.",
};

void reply_help(Op& op, const char* const* lines, size_t count) {
    reply_array_header(op.sink(), count);
    for (size_t i = 0; i < count; i++) reply_simple(op.sink(), lines[i]);
}

template <bool kNotify>
void cmd_xgroup(Shard& shard, Op& op) {
    int16_t first_key = 0;
    if (!command_validate_container_subcommand(op, *op.spec, first_key)) return;
    const Slice sub = op.arg(1);
    if (sub.eq_icase("help")) {
        reply_help(op, kXgroupHelp, sizeof(kXgroupHelp) / sizeof(kXgroupHelp[0]));
        return;
    }
    const Slice key = op.arg(2);
    KvObj* object = shard.store_find<kNotify>(op.hash, key);

    if (sub.eq_icase("create")) {
        if (op.argc() < 5) { reply_syntax(op.sink()); return; }
        StreamID id{};
        const bool latest = op.arg(4).n == 1 && op.arg(4).p[0] == '$';
        if (!latest && !parse_id(op.arg(4), id)) { invalid_id(op); return; }
        bool mkstream = false;
        int64_t entries_read = -1;
        bool entries_seen = false;
        for (uint32_t pos = 5; pos < op.argc();) {
            if (op.arg(pos).eq_icase("mkstream") && !mkstream) {
                mkstream = true; pos++; continue;
            }
            if (op.arg(pos).eq_icase("entriesread") && !entries_seen && pos + 1 < op.argc()) {
                if (!parse_i64_exact(op.arg(pos + 1), entries_read) || entries_read < -1) {
                    reply_err(op.sink(), "ERR value is not an integer or out of range"); return;
                }
                entries_seen = true; pos += 2; continue;
            }
            reply_syntax(op.sink()); return;
        }
        if (object && !obj_type_check(object, Type::Stream, op.sink())) return;
        if (object && find_group(object, op.arg(3))) {
            reply_err(op.sink(), "BUSYGROUP Consumer Group name already exists"); return;
        }
        if (!object && !mkstream) {
            reply_err(op.sink(), "ERR The XGROUP subcommand requires the key to exist. Note that for CREATE you may want to use the MKSTREAM option to create an empty stream automatically.");
            return;
        }
        if (!object && !stream_create_empty_external(shard, op, kNotify, object)) return;
        if (!stream_force_external(shard, op, object, kNotify)) return;
        StreamHeader header;
        if (!stream_object_header(object, header)) {
            reply_err(op.sink(), "ERR corrupt stream encoding"); return;
        }
        if (latest) id = header.last_id;
        ObjectSizeTracker tracker(shard.store(), object);
        StreamGroups* groups = nullptr;
        StreamVal* value = stream_value(object);
        if (!value || !ensure_group_container(*value, groups)) {
            reply_err(op.sink(), "ERR out of memory"); return;
        }
        try {
            StreamGroup group;
            group.last_delivered = id;
            group.entries_read = entries_read;
            const auto [it, inserted] = groups->groups.emplace(
                std::string(op.arg(3).p, op.arg(3).n), std::move(group));
            if (!inserted) {
                reply_err(op.sink(), "BUSYGROUP Consumer Group name already exists"); return;
            }
            groups->note_insert(group_allocation_bytes(it->first, it->second));
        } catch (const std::bad_alloc&) {
            maybe_release_groups(*value);
            reply_err(op.sink(), "ERR out of memory"); return;
        }
        reply_ok(op.sink());
        return;
    }

    if (!object) {
        if (sub.eq_icase("destroy")) { reply_int(op.sink(), 0); return; }
        reply_nogroup_subcommand(op, key, op.argc() > 3 ? op.arg(3) : Slice{});
        return;
    }
    if (!obj_type_check(object, Type::Stream, op.sink())) return;
    if (op.argc() < 4) { reply_syntax(op.sink()); return; }
    StreamGroup* group = find_group(object, op.arg(3));

    if (sub.eq_icase("destroy")) {
        if (op.argc() != 4) { reply_syntax(op.sink()); return; }
        StreamGroups* groups = groups_of(object);
        if (!groups || !group) { reply_int(op.sink(), 0); return; }
        ObjectSizeTracker tracker(shard.store(), object);
        const auto found = groups->groups.find(std::string_view(op.arg(3).p, op.arg(3).n));
        groups->note_delete(group_allocation_bytes(found->first, found->second));
        groups->groups.erase(found);
        maybe_release_groups(*stream_value(object));
        reply_int(op.sink(), 1);
        return;
    }
    if (!group) { reply_nogroup_subcommand(op, key, op.arg(3)); return; }

    if (sub.eq_icase("createconsumer")) {
        if (op.argc() != 5) { reply_syntax(op.sink()); return; }
        ObjectSizeTracker tracker(shard.store(), object);
        StreamGroups* groups = groups_of(object);
        StreamConsumer* consumer = nullptr; bool created = false;
        if (!create_consumer(*groups, *group, op.arg(4), shard.now_ms(), consumer, created)) {
            reply_err(op.sink(), "ERR out of memory"); return;
        }
        reply_int(op.sink(), created ? 1 : 0);
        return;
    }
    if (sub.eq_icase("delconsumer")) {
        if (op.argc() != 5) { reply_syntax(op.sink()); return; }
        const std::string name(op.arg(4).p, op.arg(4).n);
        auto consumer = group->consumers.find(name);
        if (consumer == group->consumers.end()) { reply_int(op.sink(), 0); return; }
        ObjectSizeTracker tracker(shard.store(), object);
        StreamGroups* groups = groups_of(object);
        uint64_t removed = 0;
        for (auto it = group->pending.begin(); it != group->pending.end();) {
            if (it->second.consumer == name) {
                auto victim = it++;
                erase_pending(*groups, *group, victim);
                removed++;
            }
            else ++it;
        }
        groups->note_delete(consumer_allocation_bytes(consumer->first));
        group->consumers.erase(consumer);
        reply_int(op.sink(), static_cast<long long>(removed));
        return;
    }
    if (sub.eq_icase("setid")) {
        if (op.argc() != 5 && op.argc() != 7) { reply_syntax(op.sink()); return; }
        StreamHeader header;
        if (!stream_object_header(object, header)) {
            reply_err(op.sink(), "ERR corrupt stream encoding"); return;
        }
        StreamID id{};
        if (op.arg(4).n == 1 && op.arg(4).p[0] == '$') id = header.last_id;
        else if (!parse_id(op.arg(4), id)) { invalid_id(op); return; }
        int64_t entries_read = -1;
        if (op.argc() == 7) {
            if (!op.arg(5).eq_icase("entriesread")) { reply_syntax(op.sink()); return; }
            if (!parse_i64_exact(op.arg(6), entries_read) || entries_read < -1) {
                reply_err(op.sink(), "ERR value is not an integer or out of range"); return;
            }
        }
        ObjectSizeTracker tracker(shard.store(), object);
        group->last_delivered = id;
        group->entries_read = entries_read;
        reply_ok(op.sink());
        return;
    }

    reply_err(op.sink(), "ERR unknown subcommand or wrong number of arguments for 'XGROUP'. Try XGROUP HELP.");
}

template <bool kNotify>
void cmd_xreadgroup(Shard& shard, Op& op) {
    (void)kNotify;
    (void)stream_xreadgroup_execute(shard, op);
}

template <bool kNotify>
void cmd_xack(Shard& shard, Op& op) {
    std::vector<StreamID> ids;
    try { ids.resize(op.argc() - 3); }
    catch (const std::bad_alloc&) { reply_err(op.sink(), "ERR out of memory"); return; }
    for (uint32_t i = 3; i < op.argc(); i++)
        if (!parse_id(op.arg(i), ids[i - 3])) { invalid_id(op); return; }
    KvObj* object = shard.store_find<kNotify>(op.hash, op.arg(1));
    if (!object) { reply_int(op.sink(), 0); return; }
    if (!obj_type_check(object, Type::Stream, op.sink())) return;
    StreamGroups* groups = groups_of(object);
    StreamGroup* group = find_group(object, op.arg(2));
    if (!group) { reply_int(op.sink(), 0); return; }
    ObjectSizeTracker tracker(shard.store(), object);
    uint64_t removed = 0;
    for (const StreamID& id : ids) {
        const auto pending = group->pending.find(id);
        if (pending == group->pending.end()) continue;
        erase_pending(*groups, *group, pending);
        removed++;
    }
    reply_int(op.sink(), static_cast<long long>(removed));
}

bool parse_pending_bound(Op& op, Slice input, bool start, StreamID& id, bool& exclusive) {
    exclusive = input.n && input.p[0] == '(';
    if (exclusive) { input.p++; input.n--; }
    if (!parse_id(input, id, start ? 0 : UINT64_MAX, true)) { invalid_id(op); return false; }
    if (exclusive && start && !id_increment(id)) return false;
    return true;
}

template <bool kNotify>
void cmd_xpending(Shard& shard, Op& op) {
    KvObj* object = shard.store_find<kNotify>(op.hash, op.arg(1));
    if (!object) { reply_nogroup(op, op.arg(1), op.arg(2)); return; }
    if (!obj_type_check(object, Type::Stream, op.sink())) return;
    StreamGroup* group = find_group(object, op.arg(2));
    if (!group) { reply_nogroup(op, op.arg(1), op.arg(2)); return; }
    if (op.argc() == 3) {
        reply_array_header(op.sink(), 4);
        reply_int(op.sink(), static_cast<long long>(group->pending.size()));
        if (group->pending.empty()) {
            reply_null(op.sink(), op.resp3());
            reply_null(op.sink(), op.resp3());
            reply_null_array(op.sink(), op.resp3());
            return;
        }
        reply_id(op, group->pending.begin()->first);
        reply_id(op, group->pending.rbegin()->first);
        std::map<std::string, uint64_t, std::less<>> counts;
        try {
            for (const auto& [id, pending] : group->pending) {
                (void)id; counts[pending.consumer]++;
            }
        } catch (const std::bad_alloc&) { reply_err(op.sink(), "ERR out of memory"); return; }
        reply_array_header(op.sink(), counts.size());
        for (const auto& [name, count] : counts) {
            reply_array_header(op.sink(), 2);
            reply_bulk(op.sink(), Slice(name.data(), name.size()));
            char number[24]; const uint32_t n = u64_to_dec(number, count);
            reply_bulk(op.sink(), Slice(number, n));
        }
        return;
    }

    uint32_t pos = 3;
    uint64_t min_idle = 0;
    if (pos < op.argc() && op.arg(pos).eq_icase("idle")) {
        if (pos + 1 >= op.argc() || !parse_u64_exact(op.arg(pos + 1), min_idle)) {
            reply_err(op.sink(), "ERR value is not an integer or out of range"); return;
        }
        pos += 2;
    }
    if (pos + 3 > op.argc() || op.argc() > pos + 4) { reply_syntax(op.sink()); return; }
    StreamID start{}, end{}; bool start_exclusive = false, end_exclusive = false;
    if (!parse_pending_bound(op, op.arg(pos), true, start, start_exclusive) ||
        !parse_pending_bound(op, op.arg(pos + 1), false, end, end_exclusive)) return;
    int64_t count = 0;
    if (!parse_i64_exact(op.arg(pos + 2), count)) {
        reply_err(op.sink(), "ERR value is not an integer or out of range"); return;
    }
    const Slice consumer = op.argc() == pos + 4 ? op.arg(pos + 3) : Slice{};
    const int64_t now = shard.now_ms();
    std::vector<std::pair<StreamID, const StreamPending*>> matches;
    try {
        if (count > 0) for (auto it = group->pending.lower_bound(start);
                            it != group->pending.end() && matches.size() < static_cast<uint64_t>(count);
                            ++it) {
            const int end_order = id_compare(it->first, end);
            if (end_order > 0 || (end_exclusive && end_order == 0)) break;
            const uint64_t idle = now > it->second.delivery_time
                ? static_cast<uint64_t>(now - it->second.delivery_time) : 0;
            if (idle < min_idle) continue;
            if (consumer.n && (it->second.consumer.size() != consumer.n ||
                std::memcmp(it->second.consumer.data(), consumer.p, consumer.n))) continue;
            matches.emplace_back(it->first, &it->second);
        }
    } catch (const std::bad_alloc&) { reply_err(op.sink(), "ERR out of memory"); return; }
    reply_array_header(op.sink(), matches.size());
    for (const auto& [id, pending] : matches) {
        reply_array_header(op.sink(), 4);
        reply_id(op, id);
        reply_bulk(op.sink(), Slice(pending->consumer.data(), pending->consumer.size()));
        reply_int(op.sink(), now > pending->delivery_time ? now - pending->delivery_time : 0);
        reply_int(op.sink(), static_cast<long long>(pending->delivery_count));
    }
}

struct ClaimOptions {
    uint64_t min_idle = 0;
    bool force = false;
    bool justid = false;
    bool idle_set = false;
    bool time_set = false;
    bool retry_set = false;
    bool lastid_set = false;
    uint64_t idle = 0;
    uint64_t time = 0;
    uint64_t retry = 0;
    StreamID lastid{};
};

bool own_borrowed_entry(const StreamBorrowedEntry& source, StreamOwnedEntry& destination) {
    try {
        destination = StreamOwnedEntry{};
        destination.id = source.id;
        destination.deleted = source.deleted;
        destination.fields.reserve(source.fields->size());
        destination.values.reserve(source.values->size());
        for (Slice field : *source.fields) destination.fields.emplace_back(field.p, field.n);
        for (Slice value : *source.values) destination.values.emplace_back(value.p, value.n);
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

template <bool kNotify>
void cmd_xclaim(Shard& shard, Op& op) {
    ClaimOptions options;
    if (!parse_u64_exact(op.arg(4), options.min_idle)) {
        reply_err(op.sink(), "ERR Invalid min-idle-time argument for XCLAIM"); return;
    }
    uint32_t options_at = 5;
    std::vector<StreamID> ids;
    try {
        while (options_at < op.argc()) {
            if (op.arg(options_at).eq_icase("idle") || op.arg(options_at).eq_icase("time") ||
                op.arg(options_at).eq_icase("retrycount") || op.arg(options_at).eq_icase("force") ||
                op.arg(options_at).eq_icase("justid") || op.arg(options_at).eq_icase("lastid")) break;
            StreamID id;
            if (!parse_id(op.arg(options_at), id)) { invalid_id(op); return; }
            ids.push_back(id); options_at++;
        }
    } catch (const std::bad_alloc&) { reply_err(op.sink(), "ERR out of memory"); return; }
    if (ids.empty()) { reply_syntax(op.sink()); return; }
    for (uint32_t pos = options_at; pos < op.argc();) {
        if (op.arg(pos).eq_icase("force") && !options.force) { options.force = true; pos++; }
        else if (op.arg(pos).eq_icase("justid") && !options.justid) { options.justid = true; pos++; }
        else if (op.arg(pos).eq_icase("idle") && !options.idle_set && pos + 1 < op.argc()) {
            if (!parse_u64_exact(op.arg(pos + 1), options.idle)) {
                reply_err(op.sink(), "ERR Invalid IDLE option argument for XCLAIM"); return;
            }
            options.idle_set = true; pos += 2;
        } else if (op.arg(pos).eq_icase("time") && !options.time_set && pos + 1 < op.argc()) {
            if (!parse_u64_exact(op.arg(pos + 1), options.time)) {
                reply_err(op.sink(), "ERR Invalid TIME option argument for XCLAIM"); return;
            }
            options.time_set = true; pos += 2;
        } else if (op.arg(pos).eq_icase("retrycount") && !options.retry_set && pos + 1 < op.argc()) {
            if (!parse_u64_exact(op.arg(pos + 1), options.retry)) {
                reply_err(op.sink(), "ERR Invalid RETRYCOUNT option argument for XCLAIM"); return;
            }
            options.retry_set = true; pos += 2;
        } else if (op.arg(pos).eq_icase("lastid") && !options.lastid_set && pos + 1 < op.argc()) {
            if (!parse_id(op.arg(pos + 1), options.lastid)) { invalid_id(op); return; }
            options.lastid_set = true; pos += 2;
        } else { reply_syntax(op.sink()); return; }
    }
    if (options.idle_set && options.time_set) { reply_syntax(op.sink()); return; }

    KvObj* object = shard.store_find<kNotify>(op.hash, op.arg(1));
    if (!object) { reply_nogroup(op, op.arg(1), op.arg(2)); return; }
    if (!obj_type_check(object, Type::Stream, op.sink())) return;
    StreamGroups* groups = groups_of(object);
    StreamGroup* group = find_group(object, op.arg(2));
    if (!group) { reply_nogroup(op, op.arg(1), op.arg(2)); return; }
    ObjectSizeTracker tracker(shard.store(), object);
    const int64_t now = shard.now_ms();
    StreamConsumer* consumer = nullptr; bool created = false;
    if (!create_consumer(*groups, *group, op.arg(3), now, consumer, created)) {
        reply_err(op.sink(), "ERR out of memory"); return;
    }
    consumer->seen_time = now;
    std::vector<StreamOwnedEntry> claimed;
    try { claimed.reserve(ids.size()); }
    catch (const std::bad_alloc&) { reply_err(op.sink(), "ERR out of memory"); return; }
    for (const StreamID& id : ids) {
        auto pending = group->pending.find(id);
        StreamOwnedEntry entry; bool found = false;
        if (!stream_object_find(object, id, entry, found)) {
            reply_err(op.sink(), "ERR corrupt stream encoding"); return;
        }
        if (!found || entry.deleted) {
            if (pending != group->pending.end()) erase_pending(*groups, *group, pending);
            continue;
        }
        if (pending == group->pending.end()) {
            if (!options.force) continue;
            try {
                StreamPending fresh;
                fresh.consumer.assign(op.arg(3).p, op.arg(3).n);
                fresh.delivery_time = now;
                fresh.delivery_count = options.retry_set ? options.retry : (options.justid ? 1 : 2);
                const auto [inserted_pending, inserted] =
                    group->pending.emplace(id, std::move(fresh));
                pending = inserted_pending;
                if (inserted)
                    groups->note_insert(pending_allocation_bytes(pending->second));
            } catch (const std::bad_alloc&) {
                reply_err(op.sink(), "ERR out of memory"); return;
            }
        } else {
            const uint64_t idle = now > pending->second.delivery_time
                ? static_cast<uint64_t>(now - pending->second.delivery_time) : 0;
            if (idle < options.min_idle) continue;
            assign_pending_consumer(*groups, pending->second, op.arg(3));
            if (options.retry_set) pending->second.delivery_count = options.retry;
            else if (!options.justid) pending->second.delivery_count++;
        }
        if (options.time_set)
            pending->second.delivery_time = options.time > static_cast<uint64_t>(now)
                ? now : static_cast<int64_t>(options.time);
        else if (options.idle_set)
            pending->second.delivery_time = options.idle > static_cast<uint64_t>(now)
                ? 0 : now - static_cast<int64_t>(options.idle);
        else pending->second.delivery_time = now;
        consumer->active_time = now;
        claimed.push_back(std::move(entry));
    }
    if (options.lastid_set && id_compare(options.lastid, group->last_delivered) > 0)
        group->last_delivered = options.lastid;
    reply_array_header(op.sink(), claimed.size());
    for (const StreamOwnedEntry& entry : claimed) {
        if (options.justid) reply_id(op, entry.id);
        else reply_entry(op, entry, false);
    }
}

template <bool kNotify>
void cmd_xautoclaim(Shard& shard, Op& op) {
    uint64_t min_idle = 0;
    StreamID start{};
    {
        // Redis takes min-idle-time as a signed canonical integer and clamps a negative one to 0,
        // so "XAUTOCLAIM key g c -1 0" reaches the group lookup while "... 05 0" does not.
        int64_t parsed = 0;
        if (!parse_i64_option(op.arg(4), parsed)) {
            reply_err(op.sink(), "ERR Invalid min-idle-time argument for XAUTOCLAIM"); return;
        }
        min_idle = parsed < 0 ? 0 : static_cast<uint64_t>(parsed);
    }
    if (!parse_id(op.arg(5), start)) { invalid_id(op); return; }
    uint64_t count = 100; bool justid = false;
    for (uint32_t pos = 6; pos < op.argc();) {
        if (op.arg(pos).eq_icase("count") && pos + 1 < op.argc()) {
            if (!parse_u64_exact(op.arg(pos + 1), count) || count == 0) {
                reply_err(op.sink(), "ERR COUNT must be > 0"); return;
            }
            pos += 2;
        } else if (op.arg(pos).eq_icase("justid") && !justid) {
            justid = true; pos++;
        } else { reply_syntax(op.sink()); return; }
    }
    KvObj* object = shard.store_find<kNotify>(op.hash, op.arg(1));
    if (!object) { reply_nogroup(op, op.arg(1), op.arg(2)); return; }
    if (!obj_type_check(object, Type::Stream, op.sink())) return;
    StreamGroups* groups = groups_of(object);
    StreamGroup* group = find_group(object, op.arg(2));
    if (!group) { reply_nogroup(op, op.arg(1), op.arg(2)); return; }
    ObjectSizeTracker tracker(shard.store(), object);
    const int64_t now = shard.now_ms();
    StreamConsumer* consumer = nullptr; bool created = false;
    if (!create_consumer(*groups, *group, op.arg(3), now, consumer, created)) {
        reply_err(op.sink(), "ERR out of memory"); return;
    }
    consumer->seen_time = now;
    std::vector<StreamOwnedEntry> claimed;
    std::vector<StreamID> deleted;
    try { claimed.reserve(count); deleted.reserve(count); }
    catch (const std::bad_alloc&) { reply_err(op.sink(), "ERR out of memory"); return; }
    const uint64_t max_scan = count > UINT64_MAX / 10 ? UINT64_MAX : count * 10;
    struct AutoClaimScan {
        StreamGroups* groups;
        StreamGroup* group;
        StreamConsumer* consumer;
        PendingMap::iterator pending;
        Slice consumer_name;
        int64_t now;
        uint64_t min_idle;
        uint64_t count;
        uint64_t max_scan;
        uint64_t scanned = 0;
        bool justid;
        bool copy_failed = false;
        bool assign_failed = false;
        std::vector<StreamOwnedEntry>* claimed;
        std::vector<StreamID>* deleted;
    } scan{groups, group, consumer, group->pending.lower_bound(start), op.arg(3), now,
           min_idle, count, max_scan, 0, justid, false, false, &claimed, &deleted};
    const auto next_pending = [](void* context, StreamID& target) {
        auto& scan = *static_cast<AutoClaimScan*>(context);
        if (scan.copy_failed || scan.assign_failed || scan.pending == scan.group->pending.end() ||
            scan.scanned >= scan.max_scan ||
            scan.claimed->size() + scan.deleted->size() >= scan.count) return false;
        target = scan.pending->first;
        return true;
    };
    const auto visit_pending = [](void* context, const StreamID& target,
                                  const StreamBorrowedEntry* source) {
        auto& scan = *static_cast<AutoClaimScan*>(context);
        auto current = scan.pending++;
        scan.scanned++;
        if (!source || source->deleted) {
            scan.deleted->push_back(target);
            erase_pending(*scan.groups, *scan.group, current);
            return;
        }
        const uint64_t idle = scan.now > current->second.delivery_time
            ? static_cast<uint64_t>(scan.now - current->second.delivery_time) : 0;
        if (idle < scan.min_idle) return;
        StreamOwnedEntry entry;
        if (scan.justid) entry.id = target;
        else if (!own_borrowed_entry(*source, entry)) {
            scan.copy_failed = true;
            return;
        }
        try {
            assign_pending_consumer(*scan.groups, current->second, scan.consumer_name);
        } catch (const std::bad_alloc&) {
            scan.assign_failed = true;
            return;
        }
        current->second.delivery_time = scan.now;
        if (!scan.justid) current->second.delivery_count++;
        scan.consumer->active_time = scan.now;
        scan.claimed->push_back(std::move(entry));
    };
    if (!stream_object_merge_scan(object, &scan, next_pending, visit_pending) || scan.copy_failed) {
        reply_err(op.sink(), "ERR corrupt stream encoding"); return;
    }
    if (scan.assign_failed) { reply_err(op.sink(), "ERR out of memory"); return; }
    const StreamID cursor = scan.pending == group->pending.end() ? StreamID{} : scan.pending->first;
    reply_array_header(op.sink(), 3);
    reply_id(op, cursor);
    reply_array_header(op.sink(), claimed.size());
    for (const StreamOwnedEntry& entry : claimed) {
        if (justid) reply_id(op, entry.id);
        else reply_entry(op, entry, false);
    }
    reply_array_header(op.sink(), deleted.size());
    for (const StreamID& id : deleted) reply_id(op, id);
}

template <bool kNotify>
void cmd_xsetid(Shard& shard, Op& op) {
    StreamID id{};
    if (!parse_id(op.arg(2), id)) { invalid_id(op); return; }
    bool entries_set = false, deleted_set = false;
    uint64_t entries_added = 0;
    StreamID max_deleted{};
    for (uint32_t pos = 3; pos < op.argc();) {
        if (op.arg(pos).eq_icase("entriesadded") && !entries_set && pos + 1 < op.argc()) {
            int64_t parsed = 0;
            if (!parse_i64_option(op.arg(pos + 1), parsed)) {
                reply_err(op.sink(), "ERR value is not an integer or out of range"); return;
            }
            if (parsed < 0) {
                reply_err(op.sink(), "ERR entries_added must be positive"); return;
            }
            entries_added = static_cast<uint64_t>(parsed);
            entries_set = true; pos += 2;
        } else if (op.arg(pos).eq_icase("maxdeletedid") && !deleted_set && pos + 1 < op.argc()) {
            if (!parse_id(op.arg(pos + 1), max_deleted)) { invalid_id(op); return; }
            deleted_set = true; pos += 2;
        } else { reply_syntax(op.sink()); return; }
    }
    KvObj* object = shard.store_find<kNotify>(op.hash, op.arg(1));
    if (!object) { reply_err(op.sink(), "ERR no such key"); return; }
    if (!obj_type_check(object, Type::Stream, op.sink())) return;
    StreamHeader header;
    StreamID top{};
    if (!stream_object_header(object, header) || !object_last_physical(object, top)) {
        reply_err(op.sink(), "ERR corrupt stream encoding"); return;
    }
    if (id_compare(id, top) < 0) {
        reply_err(op.sink(), "ERR The ID specified in XSETID is smaller than the target stream top item"); return;
    }
    const StreamID effective_deleted = deleted_set ? max_deleted : header.max_deleted_entry_id;
    if (id_compare(id, effective_deleted) < 0) {
        reply_err(op.sink(), "ERR The ID specified in XSETID is smaller than the provided max_deleted_entry_id"); return;
    }
    const uint64_t effective_added = entries_set ? entries_added : header.entries_added;
    if (effective_added < stream_object_live_length(object)) {
        reply_err(op.sink(), "ERR The entries_added specified in XSETID is smaller than the target stream length"); return;
    }
    ObjectSizeTracker tracker(shard.store(), object);
    header.last_id = id;
    header.entries_added = effective_added;
    header.max_deleted_entry_id = effective_deleted;
    if (!stream_object_update_header(object, header)) {
        reply_err(op.sink(), "ERR corrupt stream encoding"); return;
    }
    reply_ok(op.sink());
}

void reply_info_header(Op& op, uint32_t pairs) {
    if (op.resp3()) reply_map_header(op.sink(), pairs, true);
    else reply_array_header(op.sink(), pairs * 2);
}

void reply_name(Op& op, const char* name) {
    reply_bulk(op.sink(), Slice(name, static_cast<uint32_t>(std::strlen(name))));
}

void reply_nullable_i64(Op& op, int64_t value) {
    if (value < 0) reply_null(op.sink(), op.resp3());
    else reply_int(op.sink(), value);
}

void reply_group_info(Op& op, KvObj* object, const StreamHeader& header,
                      const std::string& name, const StreamGroup& group, bool full,
                      uint64_t count) {
    int64_t lag = 0;
    const bool has_lag = group_lag(object, header, group, lag);
    reply_info_header(op, full ? 7 : 6);
    reply_name(op, "name"); reply_bulk(op.sink(), Slice(name.data(), name.size()));
    if (!full) {
        reply_name(op, "consumers"); reply_int(op.sink(), group.consumers.size());
        reply_name(op, "pending"); reply_int(op.sink(), group.pending.size());
    }
    reply_name(op, "last-delivered-id"); reply_id(op, group.last_delivered);
    reply_name(op, "entries-read"); reply_nullable_i64(op, group.entries_read);
    reply_name(op, "lag");
    if (has_lag) reply_int(op.sink(), lag); else reply_null(op.sink(), op.resp3());
    if (!full) return;
    reply_name(op, "pel-count"); reply_int(op.sink(), group.pending.size());
    reply_name(op, "pending");
    reply_array_header(op.sink(), std::min<uint64_t>(count, group.pending.size()));
    uint64_t emitted = 0;
    for (const auto& [id, pending] : group.pending) {
        if (emitted++ >= count) break;
        reply_array_header(op.sink(), 4);
        reply_id(op, id);
        reply_bulk(op.sink(), Slice(pending.consumer.data(), pending.consumer.size()));
        reply_int(op.sink(), pending.delivery_time);
        reply_int(op.sink(), pending.delivery_count);
    }
    reply_name(op, "consumers");
    reply_array_header(op.sink(), group.consumers.size());
    for (const auto& [consumer_name, consumer] : group.consumers) {
        reply_info_header(op, 5);
        reply_name(op, "name");
        reply_bulk(op.sink(), Slice(consumer_name.data(), consumer_name.size()));
        reply_name(op, "seen-time"); reply_int(op.sink(), consumer.seen_time);
        reply_name(op, "active-time"); reply_int(op.sink(), consumer.active_time);
        const uint64_t pel_count = pending_for(group, consumer_name);
        reply_name(op, "pel-count"); reply_int(op.sink(), pel_count);
        reply_name(op, "pending");
        reply_array_header(op.sink(), std::min<uint64_t>(count, pel_count));
        uint64_t consumer_emitted = 0;
        for (const auto& [id, pending] : group.pending) {
            if (pending.consumer != consumer_name) continue;
            if (consumer_emitted++ >= count) break;
            reply_array_header(op.sink(), 3);
            reply_id(op, id);
            reply_int(op.sink(), pending.delivery_time);
            reply_int(op.sink(), pending.delivery_count);
        }
    }
}

template <bool kNotify>
void cmd_xinfo(Shard& shard, Op& op) {
    int16_t first_key = 0;
    if (!command_validate_container_subcommand(op, *op.spec, first_key)) return;
    const Slice sub = op.arg(1);
    if (sub.eq_icase("help")) {
        reply_help(op, kXinfoHelp, sizeof(kXinfoHelp) / sizeof(kXinfoHelp[0]));
        return;
    }
    KvObj* object = shard.store_find<kNotify>(op.hash, op.arg(2));
    if (!object) { reply_err(op.sink(), "ERR no such key"); return; }
    if (!obj_type_check(object, Type::Stream, op.sink())) return;
    StreamHeader header;
    if (!stream_object_header(object, header)) {
        reply_err(op.sink(), "ERR corrupt stream encoding"); return;
    }
    const StreamGroups* groups = groups_of(object);
    if (sub.eq_icase("groups")) {
        if (op.argc() != 3) { reply_syntax(op.sink()); return; }
        reply_array_header(op.sink(), groups ? groups->groups.size() : 0);
        if (groups) for (const auto& [name, group] : groups->groups)
            reply_group_info(op, object, header, name, group, false, 0);
        return;
    }
    if (sub.eq_icase("consumers")) {
        if (op.argc() != 4) { reply_syntax(op.sink()); return; }
        const StreamGroup* group = find_group(object, op.arg(3));
        if (!group) { reply_nogroup_subcommand(op, op.arg(2), op.arg(3)); return; }
        reply_array_header(op.sink(), group->consumers.size());
        const int64_t now = shard.now_ms();
        for (const auto& [name, consumer] : group->consumers) {
            reply_info_header(op, 4);
            reply_name(op, "name"); reply_bulk(op.sink(), Slice(name.data(), name.size()));
            reply_name(op, "pending"); reply_int(op.sink(), pending_for(*group, name));
            reply_name(op, "idle");
            reply_int(op.sink(), consumer.seen_time < 0 || now < consumer.seen_time
                ? 0 : now - consumer.seen_time);
            reply_name(op, "inactive");
            reply_int(op.sink(), consumer.active_time < 0 ? -1
                : (now < consumer.active_time ? 0 : now - consumer.active_time));
        }
        return;
    }
    if (!sub.eq_icase("stream")) {
        reply_err(op.sink(), "ERR unknown subcommand or wrong number of arguments for 'XINFO'. Try XINFO HELP.");
        return;
    }
    bool full = false; uint64_t count = 10;
    if (op.argc() > 3) {
        if (!op.arg(3).eq_icase("full")) { reply_syntax(op.sink()); return; }
        full = true;
        if (op.argc() == 6) {
            if (!op.arg(4).eq_icase("count") || !parse_u64_exact(op.arg(5), count)) {
                reply_syntax(op.sink()); return;
            }
            if (count == 0) count = UINT64_MAX;
        } else if (op.argc() != 4) { reply_syntax(op.sink()); return; }
    }
    const uint64_t length = stream_object_live_length(object);
    const uint64_t physical = stream_object_physical_length(object);
    StreamID first{}, last{};
    if (!stream_object_first_live(object, first) || !stream_object_last_live(object, last)) {
        reply_err(op.sink(), "ERR corrupt stream encoding"); return;
    }
    std::vector<StreamOwnedEntry> first_entry, last_entry;
    if (length) {
        if (!stream_object_collect(object, first, false, 1, false, first_entry)) {
            reply_err(op.sink(), "ERR corrupt stream encoding"); return;
        }
        StreamOwnedEntry found; bool present = false;
        if (!stream_object_find(object, last, found, present)) {
            reply_err(op.sink(), "ERR corrupt stream encoding"); return;
        }
        if (present) { last_entry.clear(); last_entry.push_back(std::move(found)); }
    }
    if (!full) {
        reply_info_header(op, 10);
        reply_name(op, "length"); reply_int(op.sink(), length);
        reply_name(op, "radix-tree-keys"); reply_int(op.sink(), physical ? 1 : 0);
        reply_name(op, "radix-tree-nodes"); reply_int(op.sink(), physical ? 2 : 1);
        reply_name(op, "last-generated-id"); reply_id(op, header.last_id);
        reply_name(op, "max-deleted-entry-id"); reply_id(op, header.max_deleted_entry_id);
        reply_name(op, "entries-added"); reply_int(op.sink(), header.entries_added);
        reply_name(op, "recorded-first-entry-id"); reply_id(op, first);
        reply_name(op, "groups"); reply_int(op.sink(), groups ? groups->groups.size() : 0);
        reply_name(op, "first-entry");
        if (first_entry.empty()) reply_null(op.sink(), op.resp3());
        else reply_entry(op, first_entry.front(), false);
        reply_name(op, "last-entry");
        if (last_entry.empty()) reply_null(op.sink(), op.resp3());
        else reply_entry(op, last_entry.front(), false);
        return;
    }
    reply_info_header(op, 9);
    reply_name(op, "length"); reply_int(op.sink(), length);
    reply_name(op, "radix-tree-keys"); reply_int(op.sink(), physical ? 1 : 0);
    reply_name(op, "radix-tree-nodes"); reply_int(op.sink(), physical ? 2 : 1);
    reply_name(op, "last-generated-id"); reply_id(op, header.last_id);
    reply_name(op, "max-deleted-entry-id"); reply_id(op, header.max_deleted_entry_id);
    reply_name(op, "entries-added"); reply_int(op.sink(), header.entries_added);
    reply_name(op, "recorded-first-entry-id"); reply_id(op, first);
    reply_name(op, "entries");
    std::vector<StreamOwnedEntry> entries;
    if (!stream_object_collect(object, {}, false, count, false, entries)) {
        reply_err(op.sink(), "ERR corrupt stream encoding"); return;
    }
    reply_array_header(op.sink(), entries.size());
    for (const StreamOwnedEntry& entry : entries) reply_entry(op, entry, false);
    reply_name(op, "groups");
    reply_array_header(op.sink(), groups ? groups->groups.size() : 0);
    if (groups) for (const auto& [name, group] : groups->groups)
        reply_group_info(op, object, header, name, group, true, count);
}

static const CommandSpec kTable[] = {
    {"XGROUP",      2, -1, CmdFlags::Write | CmdFlags::CursorShard | CmdFlags::SubcmdRoute,
                              cmd_xgroup<false>, 2, 2, 1, cmd_xgroup<true>},
    {"XREADGROUP",  7, -1, CmdFlags::Write | CmdFlags::Blocking | CmdFlags::MultiShard |
                              CmdFlags::CursorShard | CmdFlags::StreamRoute,
                              cmd_xreadgroup<false>, 0, 0, 0, cmd_xreadgroup<true>},
    {"XACK",        4, -1, CmdFlags::Write,
                              cmd_xack<false>, 1, 1, 1, cmd_xack<true>},
    {"XPENDING",    3,  9, CmdFlags::Readonly,
                              cmd_xpending<false>, 1, 1, 1, cmd_xpending<true>},
    {"XCLAIM",      6, -1, CmdFlags::Write,
                              cmd_xclaim<false>, 1, 1, 1, cmd_xclaim<true>},
    {"XAUTOCLAIM",  6,  9, CmdFlags::Write,
                              cmd_xautoclaim<false>, 1, 1, 1, cmd_xautoclaim<true>},
    {"XSETID",      3,  7, CmdFlags::Write,
                              cmd_xsetid<false>, 1, 1, 1, cmd_xsetid<true>},
    {"XINFO",       2,  6, CmdFlags::Readonly | CmdFlags::CursorShard | CmdFlags::SubcmdRoute,
                              cmd_xinfo<false>, 2, 2, 1, cmd_xinfo<true>},
};

}  // namespace

StreamVal::~StreamVal() { stream_groups_destroy(groups); }

void stream_groups_destroy(void* groups) { delete static_cast<StreamGroups*>(groups); }

uint64_t stream_groups_allocation_bytes(const void* opaque) {
    const auto* groups = static_cast<const StreamGroups*>(opaque);
    return groups ? groups->bytes_ : 0;
}

bool stream_parse_xreadgroup(Op& op, StreamXreadGroupArgs& parsed) {
    parsed = {};
    parsed.count = UINT64_MAX;
    if (op.argc() < 7 || !op.arg(1).eq_icase("group")) { reply_syntax(op.sink()); return false; }
    parsed.group_arg = 2;
    parsed.consumer_arg = 3;
    uint32_t pos = 4;
    bool count_seen = false, block_seen = false, noack_seen = false;
    while (pos < op.argc() && !op.arg(pos).eq_icase("streams")) {
        if (op.arg(pos).eq_icase("count") && !count_seen && pos + 1 < op.argc()) {
            if (!parse_u64_exact(op.arg(pos + 1), parsed.count)) {
                reply_err(op.sink(), "ERR value is not an integer or out of range"); return false;
            }
            count_seen = true; pos += 2;
        } else if (op.arg(pos).eq_icase("block") && !block_seen && pos + 1 < op.argc()) {
            if (!parse_u64_exact(op.arg(pos + 1), parsed.block_ms)) {
                reply_err(op.sink(), "ERR timeout is not an integer or out of range"); return false;
            }
            parsed.block = true; block_seen = true; pos += 2;
        } else if (op.arg(pos).eq_icase("noack") && !noack_seen) {
            parsed.noack = true; noack_seen = true; pos++;
        } else { reply_syntax(op.sink()); return false; }
    }
    if (pos >= op.argc() || op.argc() - pos != 3) {
        reply_err(op.sink(), "ERR Unbalanced 'xreadgroup' list of streams: for each stream key "
                             "an ID or '>' must be specified.");
        return false;
    }
    parsed.key_arg = pos + 1;
    parsed.id_arg = pos + 2;
    parsed.new_entries = op.arg(parsed.id_arg).n == 1 && op.arg(parsed.id_arg).p[0] == '>';
    StreamID id;
    if (!parsed.new_entries && !parse_id(op.arg(parsed.id_arg), id)) { invalid_id(op); return false; }
    return true;
}

StreamGroupProbe stream_group_probe(Shard& shard, Slice key, uint64_t hash, Slice group_name) {
    KvObj* object = shard.notify_carrier() ? shard.store_find<true>(hash, key)
                                           : shard.store().find(hash, key);
    if (!object) return StreamGroupProbe::MissingGroup;
    if (static_cast<Type>(object->type) != Type::Stream) return StreamGroupProbe::WrongType;
    const StreamGroup* group = find_group(object, group_name);
    if (!group) return StreamGroupProbe::MissingGroup;
    return stream_object_has_live_after(object, group->last_delivered)
        ? StreamGroupProbe::Ready : StreamGroupProbe::Empty;
}

StreamGroupProbe stream_group_prepare_waiter(Shard& shard, Slice key, uint64_t hash,
                                             Slice group_name, Slice consumer_name) {
    KvObj* object = shard.notify_carrier() ? shard.store_find<true>(hash, key)
                                           : shard.store().find(hash, key);
    if (!object) return StreamGroupProbe::MissingGroup;
    if (static_cast<Type>(object->type) != Type::Stream) return StreamGroupProbe::WrongType;
    StreamGroups* groups = groups_of(object);
    StreamGroup* group = find_group(object, group_name);
    if (!group) return StreamGroupProbe::MissingGroup;
    ObjectSizeTracker tracker(shard.store(), object);
    StreamConsumer* consumer = nullptr; bool created = false;
    if (!create_consumer(*groups, *group, consumer_name, shard.now_ms(), consumer, created))
        return StreamGroupProbe::Oom;
    consumer->seen_time = shard.now_ms();
    return stream_object_has_live_after(object, group->last_delivered)
        ? StreamGroupProbe::Ready : StreamGroupProbe::Empty;
}

void stream_xreadgroup_reply_nogroup(Op& op) {
    StreamXreadGroupArgs parsed;
    if (stream_parse_xreadgroup(op, parsed))
        reply_nogroup(op, op.arg(parsed.key_arg), op.arg(parsed.group_arg), true);
}

bool stream_xreadgroup_execute(Shard& shard, Op& op) {
    StreamXreadGroupArgs parsed;
    if (!stream_parse_xreadgroup(op, parsed)) return true;
    const Slice key = op.arg(parsed.key_arg);
    const uint64_t hash = FlatStore::hash_key(key);
    KvObj* object = (op.spec->flags & CmdFlags::NotifySelected)
        ? shard.store_find<true>(hash, key) : shard.store().find(hash, key);
    if (!object) { reply_nogroup(op, key, op.arg(parsed.group_arg), true); return true; }
    if (!obj_type_check(object, Type::Stream, op.sink())) return true;
    StreamGroups* groups = groups_of(object);
    StreamGroup* group = find_group(object, op.arg(parsed.group_arg));
    if (!group) { reply_nogroup(op, key, op.arg(parsed.group_arg), true); return true; }
    ObjectSizeTracker tracker(shard.store(), object);
    const int64_t now = shard.now_ms();
    StreamConsumer* consumer = nullptr; bool created = false;
    if (!create_consumer(*groups, *group, op.arg(parsed.consumer_arg), now, consumer, created)) {
        reply_err(op.sink(), "ERR out of memory"); return true;
    }
    consumer->seen_time = now;
    const std::string consumer_name(op.arg(parsed.consumer_arg).p,
                                    op.arg(parsed.consumer_arg).n);
    std::vector<StreamOwnedEntry> entries;
    if (parsed.new_entries) {
        if (!stream_object_collect(object, group->last_delivered, true,
                                   parsed.count == UINT64_MAX ? 0 : parsed.count,
                                   false, entries)) {
            reply_err(op.sink(), "ERR out of memory"); return true;
        }
        if (entries.empty()) {
            if (parsed.block) return false;
            reply_null_array(op.sink(), op.resp3());
            return true;
        }
        group->last_delivered = entries.back().id;
        group->entries_read = infer_entries_read(object, group->last_delivered);
        if (!parsed.noack) {
            try {
                for (const StreamOwnedEntry& entry : entries) {
                    StreamPending pending;
                    pending.consumer = consumer_name;
                    pending.delivery_time = now;
                    pending.delivery_count = 1;
                    upsert_pending(*groups, *group, entry.id, std::move(pending));
                }
            } catch (const std::bad_alloc&) {
                reply_err(op.sink(), "ERR out of memory"); return true;
            }
            consumer->active_time = now;
        }
        reply_stream_entries(op, key, entries, false);
        return true;
    }

    StreamID cursor{};
    if (!parse_id(op.arg(parsed.id_arg), cursor)) { invalid_id(op); return true; }
    try {
        for (auto it = group->pending.upper_bound(cursor); it != group->pending.end(); ++it) {
            if (it->second.consumer != consumer_name) continue;
            if (parsed.count != UINT64_MAX && entries.size() >= parsed.count) break;
            StreamOwnedEntry entry; bool found = false;
            if (!stream_object_find(object, it->first, entry, found)) {
                reply_err(op.sink(), "ERR corrupt stream encoding"); return true;
            }
            if (!found) { entry.id = it->first; entry.deleted = true; }
            it->second.delivery_time = now;
            it->second.delivery_count++;
            entries.push_back(std::move(entry));
        }
    } catch (const std::bad_alloc&) { reply_err(op.sink(), "ERR out of memory"); return true; }
    reply_stream_entries(op, key, entries, true);
    return true;
}

namespace {

uint64_t groups_encoded_size(const StreamGroups* groups) {
    uint64_t total = 4;
    if (!groups) return total;
    for (const auto& [name, group] : groups->groups) {
        total += 4 + name.size() + 16 + 8 + 4;
        for (const auto& [consumer_name, consumer] : group.consumers) {
            (void)consumer;
            total += 4 + consumer_name.size() + 16;
        }
        total += 4;
        for (const auto& [id, pending] : group.pending) {
            (void)id;
            total += 16 + 4 + pending.consumer.size() + 16;
        }
    }
    return total;
}

struct ChunkWriter {
    uint64_t skip = 0;
    uint8_t* out = nullptr;
    size_t capacity = 0;
    size_t written = 0;
    uint64_t logical = 0;

    void put(const void* data, size_t size) {
        const uint64_t begin = logical;
        logical += size;
        if (written == capacity || skip >= logical) return;
        const size_t source = skip > begin ? static_cast<size_t>(skip - begin) : 0;
        const size_t take = std::min(size - source, capacity - written);
        if (take) std::memcpy(out + written, static_cast<const uint8_t*>(data) + source, take);
        written += take;
    }
    void u32(uint32_t value) { uint8_t bytes[4]; snapshot_put_u32(bytes, value); put(bytes, 4); }
    void u64(uint64_t value) { uint8_t bytes[8]; snapshot_put_u64(bytes, value); put(bytes, 8); }
    void string(const std::string& value) {
        u32(static_cast<uint32_t>(value.size())); put(value.data(), value.size());
    }
};

bool read_u32(const uint8_t*& p, size_t& left, uint32_t& value) {
    if (left < 4) return false;
    value = snapshot_get_u32(p); p += 4; left -= 4; return true;
}
bool read_u64(const uint8_t*& p, size_t& left, uint64_t& value) {
    if (left < 8) return false;
    value = snapshot_get_u64(p); p += 8; left -= 8; return true;
}
bool read_string(const uint8_t*& p, size_t& left, std::string& value) {
    uint32_t size = 0;
    if (!read_u32(p, left, size) || left < size) return false;
    value.assign(reinterpret_cast<const char*>(p), size); p += size; left -= size; return true;
}

}  // namespace

uint64_t stream_groups_snapshot_size(const void* groups) {
    return groups_encoded_size(static_cast<const StreamGroups*>(groups));
}

bool stream_groups_snapshot_read(const void* opaque, uint64_t offset, uint8_t* destination,
                                 size_t capacity, size_t& written) {
    const auto* groups = static_cast<const StreamGroups*>(opaque);
    ChunkWriter writer{offset, destination, capacity};
    writer.u32(groups ? static_cast<uint32_t>(groups->groups.size()) : 0);
    if (groups) for (const auto& [name, group] : groups->groups) {
        writer.string(name);
        writer.u64(group.last_delivered.ms); writer.u64(group.last_delivered.seq);
        writer.u64(static_cast<uint64_t>(group.entries_read));
        writer.u32(static_cast<uint32_t>(group.consumers.size()));
        for (const auto& [consumer_name, consumer] : group.consumers) {
            writer.string(consumer_name);
            writer.u64(static_cast<uint64_t>(consumer.seen_time));
            writer.u64(static_cast<uint64_t>(consumer.active_time));
        }
        writer.u32(static_cast<uint32_t>(group.pending.size()));
        for (const auto& [id, pending] : group.pending) {
            writer.u64(id.ms); writer.u64(id.seq);
            writer.string(pending.consumer);
            writer.u64(static_cast<uint64_t>(pending.delivery_time));
            writer.u64(pending.delivery_count);
        }
    }
    written = writer.written;
    return offset + written <= writer.logical;
}

bool stream_groups_snapshot_load(StreamVal& stream, Slice payload) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(payload.p);
    size_t left = payload.n;
    uint32_t group_count = 0;
    if (!read_u32(p, left, group_count)) return false;
    if (!group_count) return left == 0;
    auto* groups = new (std::nothrow) StreamGroups;
    if (!groups) return false;
    try {
        for (uint32_t g = 0; g < group_count; g++) {
            std::string name;
            uint64_t ms = 0, seq = 0, entries_read = 0;
            uint32_t consumer_count = 0, pending_count = 0;
            if (!read_string(p, left, name) || !read_u64(p, left, ms) ||
                !read_u64(p, left, seq) || !read_u64(p, left, entries_read) ||
                !read_u32(p, left, consumer_count)) { delete groups; return false; }
            StreamGroup fresh_group;
            fresh_group.last_delivered = {ms, seq};
            fresh_group.entries_read = static_cast<int64_t>(entries_read);
            const auto [group_position, group_inserted] =
                groups->groups.emplace(std::move(name), std::move(fresh_group));
            if (!group_inserted) { delete groups; return false; }
            StreamGroup& group = group_position->second;
            groups->note_insert(group_allocation_bytes(group_position->first, group));
            for (uint32_t c = 0; c < consumer_count; c++) {
                std::string consumer_name; uint64_t seen = 0, active = 0;
                if (!read_string(p, left, consumer_name) || !read_u64(p, left, seen) ||
                    !read_u64(p, left, active)) { delete groups; return false; }
                const auto [consumer_position, consumer_inserted] = group.consumers.emplace(
                    std::move(consumer_name),
                    StreamConsumer{static_cast<int64_t>(seen), static_cast<int64_t>(active)});
                if (!consumer_inserted) { delete groups; return false; }
                groups->note_insert(consumer_allocation_bytes(consumer_position->first));
            }
            if (!read_u32(p, left, pending_count)) { delete groups; return false; }
            for (uint32_t n = 0; n < pending_count; n++) {
                StreamID id; std::string consumer; uint64_t delivery = 0, deliveries = 0;
                if (!read_u64(p, left, id.ms) || !read_u64(p, left, id.seq) ||
                    !read_string(p, left, consumer) || !read_u64(p, left, delivery) ||
                    !read_u64(p, left, deliveries)) { delete groups; return false; }
                const auto [pending_position, pending_inserted] = group.pending.emplace(
                    id, StreamPending{std::move(consumer),
                                      static_cast<int64_t>(delivery), deliveries});
                if (!pending_inserted) { delete groups; return false; }
                groups->note_insert(pending_allocation_bytes(pending_position->second));
            }
        }
    } catch (const std::bad_alloc&) { delete groups; return false; }
    if (left || groups->bytes_ != recompute_groups_allocation_bytes(*groups)) {
        delete groups; return false;
    }
    stream.groups = groups;
    return true;
}

CommandTable stream_group_command_table() {
    return {kTable, sizeof(kTable) / sizeof(kTable[0])};
}

}  // namespace tomo

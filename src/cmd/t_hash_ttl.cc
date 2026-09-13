// t_hash_ttl.cc — HEXPIRE / HPEXPIRE / HEXPIREAT / HPEXPIREAT / HTTL / HPTTL / HEXPIRETIME /
// HPEXPIRETIME / HPERSIST: per-field deadlines on a hash (Redis 7.4).
//
// The design rule for this lane is in t_hash_ttl.h and it is the whole point: a hash that has never
// seen HEXPIRE must not allocate, must not grow, and must not branch more than once. Everything
// below therefore lives OUT OF LINE behind one per-shard counter test in hash_lookup().
//
// Deadlines are absolute milliseconds, stored in a side table hanging off HashVal. Three
// consequences fall straight out of that choice:
//   * persistence is free — the snapshot/AOF/DUMP payload carries the deadline (encoding byte 1)
//     and replay drops what has already lapsed, so no command-rewrite path was needed;
//   * expiry is idempotent — a lazy reap and the active cycle can race to the same field harmlessly;
//   * an embedded (Enc::Compact) hash can be declared TTL-free by construction, because the first
//     HEXPIRE externalizes it, mirroring Redis's listpack -> listpackex promotion.
#include "t_hash_ttl.h"

#include "command.h"
#include "notify.h"
#include "../core/shard.h"
#include "../exec/op.h"
#include "../net/resp.h"
#include "../store/kvobj.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <vector>

namespace tomo {
namespace {

// Redis 7.4 caps a hash-field deadline at the 46-bit expiry range of its field-expiry index; probed
// against the 7.4.2 oracle rather than read out of its source.
constexpr int64_t kFieldExpireMax = 70368744177663LL;   // 2^46 - 1

enum class Cond : uint8_t { None, Nx, Xx, Gt, Lt };
enum class Read : uint8_t { Ttl, Pttl, ExpireTime, PexpireTime, Persist };

bool parse_i64(Slice text, int64_t& value) {
    if (text.n == 0 || text.n >= 21) return false;
    if (text.n == 1 && text.p[0] == '0') { value = 0; return true; }
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
    const uint64_t limit = negative ? (uint64_t{1} << 63)
                                    : static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    if (magnitude > limit) return false;
    if (negative)
        value = magnitude == (uint64_t{1} << 63) ? std::numeric_limits<int64_t>::min()
                                                 : -static_cast<int64_t>(magnitude);
    else
        value = static_cast<int64_t>(magnitude);
    return true;
}

HashFieldTtl* ttl_table(const KvObj* object) {
    HashFieldTtl** slot = hash_ttl_slot(const_cast<KvObj*>(object));
    return slot ? *slot : nullptr;
}

// A field is LIVE when it is present and either has no deadline or has one still in the future.
// The reap normally removes lapsed fields before a handler runs, but it is suppressed while a
// snapshot capture is in flight, so every command in this file filters logically as well.
bool field_live(const KvObj* object, const HashFieldTtl* ttls, Slice field, int64_t now_ms,
                int64_t& expire_ms) {
    expire_ms = HashFieldTtl::kNone;
    if (!hash_ttl_field_exists(object, field)) return false;
    if (ttls) {
        expire_ms = ttls->get(field);
        if (expire_ms >= 0 && expire_ms <= now_ms) return false;
    }
    return true;
}

struct FieldsSpec {
    uint32_t first = 0;
    uint32_t count = 0;
};

// `HEXPIRE key ttl [NX|XX|GT|LT] FIELDS numfields field...` — everything from the FIELDS token on.
// The two numfields error strings differ between the write and read families in Redis 7.4; both are
// reproduced verbatim because the differ compares error bytes.
bool parse_fields_tail(Op& op, uint32_t index, bool write_family, FieldsSpec& out) {
    if (index + 1 >= op.argc() || !op.arg(index).eq_icase("fields")) {
        reply_err(op.sink(),
                  "ERR Mandatory argument FIELDS is missing or not at the right position");
        return false;
    }
    int64_t numfields = 0;
    if (!parse_i64(op.arg(index + 1), numfields) || numfields <= 0) {
        reply_err(op.sink(), write_family
                      ? "ERR Parameter `numFields` should be greater than 0"
                      : "ERR Number of fields must be a positive integer");
        return false;
    }
    const uint32_t supplied = op.argc() - (index + 2);
    if (static_cast<uint64_t>(numfields) != supplied) {
        reply_err(op.sink(), "ERR The `numfields` parameter must match the number of arguments");
        return false;
    }
    out.first = index + 2;
    out.count = supplied;
    return true;
}

template <bool kNotify>
bool hash_ttl_lookup(Shard& shard, Op& op, KvObj*& object) {
    object = shard.store_find<kNotify>(op.hash, op.key());
    if (!obj_type_check(object, Type::Hash, op.sink())) return false;
    if (__builtin_expect(shard.store().field_expire_count() != 0, false) && object)
        object = hash_ttl_on_access(shard, op, object, kNotify);
    return true;
}

void reply_all(Op& op, uint32_t count, int64_t value) {
    reply_array_header(op.sink(), count);
    for (uint32_t i = 0; i < count; i++) reply_int(op.sink(), value);
}

bool condition_allows(Cond cond, int64_t current, int64_t proposed) {
    // "No deadline" is infinity for GT/LT, exactly as key-level EXPIRE GT/LT treat a persistent key.
    switch (cond) {
        case Cond::None: return true;
        case Cond::Nx:   return current < 0;
        case Cond::Xx:   return current >= 0;
        case Cond::Gt:   return current >= 0 && proposed > current;
        case Cond::Lt:   return current < 0 || proposed < current;
    }
    return true;
}

// ---- the write family -------------------------------------------------------------------------

template <bool kNotify>
void hexpire_generic(Shard& shard, Op& op, int64_t unit_ms, bool absolute, const char* cmd_name) {
    KvObj* object = nullptr;
    if (!hash_ttl_lookup<kNotify>(shard, op, object)) return;   // WRONGTYPE outranks every arg error

    int64_t raw = 0;
    if (!parse_i64(op.arg(2), raw)) {
        reply_err(op.sink(), "ERR value is not an integer or out of range");
        return;
    }
    uint32_t index = 3;
    Cond cond = Cond::None;
    if (index < op.argc()) {
        const Slice token = op.arg(index);
        if      (token.eq_icase("nx")) { cond = Cond::Nx; index++; }
        else if (token.eq_icase("xx")) { cond = Cond::Xx; index++; }
        else if (token.eq_icase("gt")) { cond = Cond::Gt; index++; }
        else if (token.eq_icase("lt")) { cond = Cond::Lt; index++; }
    }
    FieldsSpec fields;
    if (!parse_fields_tail(op, index, true, fields)) return;

    if (raw < 0) {
        reply_err(op.sink(), "ERR invalid expire time, must be >= 0");
        return;
    }
    const int64_t now_ms = shard.now_ms();
    const int64_t basetime = absolute ? 0 : now_ms;
    char message[64];
    auto invalid = [&]() {
        std::size_t pos = 0;
        for (const char* p = "ERR invalid expire time in '"; *p; p++) message[pos++] = *p;
        for (const char* p = cmd_name; *p; p++) message[pos++] = *p;
        for (const char* p = "' command"; *p; p++) message[pos++] = *p;
        message[pos] = '\0';
        reply_err(op.sink(), message);
    };
    if (raw > kFieldExpireMax / unit_ms) { invalid(); return; }
    int64_t deadline = raw * unit_ms;
    if (deadline > kFieldExpireMax - basetime) { invalid(); return; }
    deadline += basetime;

    if (!object) { reply_all(op, fields.count, -2); return; }

    // Only a hash that will actually carry a deadline has to leave the embedded representation, so
    // HEXPIRE against absent fields (or a condition nobody meets) keeps the one-allocation form.
    const HashFieldTtl* peek = ttl_table(object);
    if (!peek && deadline > now_ms &&
        (cond == Cond::None || cond == Cond::Nx || cond == Cond::Lt)) {
        bool any = false;
        for (uint32_t i = 0; !any && i < fields.count; i++)
            any = hash_ttl_field_exists(object, op.arg(fields.first + i));
        if (any && !hash_ttl_slot(object) &&
            !hash_ttl_externalize(shard, op, object, kNotify)) return;
    }

    std::vector<int8_t> results;
    try {
        results.assign(fields.count, -2);
    } catch (const std::bad_alloc&) {
        reply_err(op.sink(), "ERR out of memory");
        return;
    }

    ObjectSizeTracker size_tracker(shard.store(), object);
    HashFieldTtl** slot = hash_ttl_slot(object);
    bool set_any = false;
    bool deleted_any = false;
    bool key_gone = false;
    for (uint32_t i = 0; i < fields.count; i++) {
        const Slice field = op.arg(fields.first + i);
        int64_t current = HashFieldTtl::kNone;
        if (!field_live(object, slot ? *slot : nullptr, field, now_ms, current)) continue;
        if (!condition_allows(cond, current, deadline)) { results[i] = 0; continue; }
        if (deadline <= now_ms) {
            // The deadline is already behind us: Redis deletes the field outright and answers 2.
            if (slot && *slot) (*slot)->erase(field);
            hash_ttl_field_erase(object, field);
            results[i] = 2;
            deleted_any = true;
            if (hash_ttl_field_count(object) == 0) { key_gone = true; break; }
            continue;
        }
        if (!slot) { results[i] = 0; continue; }   // embedded and unexternalizable: cannot store
        if (!*slot) {
            *slot = new (std::nothrow) HashFieldTtl;
            if (!*slot) { reply_err(op.sink(), "ERR out of memory"); return; }
        }
        if (!(*slot)->set(field, deadline)) {
            reply_err(op.sink(), "ERR out of memory");
            return;
        }
        results[i] = 1;
        set_any = true;
    }
    if (slot && *slot && (*slot)->empty()) { delete *slot; *slot = nullptr; }
    if (!key_gone) hash_ttl_note_bytes(object);
    if (set_any) shard.store().note_field_ttl(op.hash);

    if constexpr (kNotify) {
        if (deleted_any) notify_record(shard, op, NOTIFY_HASH, NotifyEventId::Hdel, op.key());
        else if (set_any) notify_record(shard, op, NOTIFY_HASH, NotifyEventId::Hexpire, op.key());
    }
    if (key_gone) {
        size_tracker.finish();
        shard.store_erase<kNotify>(op.hash, op.key());
    }
    reply_array_header(op.sink(), fields.count);
    for (uint32_t i = 0; i < fields.count; i++) reply_int(op.sink(), results[i]);
}

template <bool kNotify> void cmd_hexpire(Shard& shard, Op& op) {
    hexpire_generic<kNotify>(shard, op, 1000, false, "hexpire");
}
template <bool kNotify> void cmd_hpexpire(Shard& shard, Op& op) {
    hexpire_generic<kNotify>(shard, op, 1, false, "hpexpire");
}
template <bool kNotify> void cmd_hexpireat(Shard& shard, Op& op) {
    hexpire_generic<kNotify>(shard, op, 1000, true, "hexpireat");
}
template <bool kNotify> void cmd_hpexpireat(Shard& shard, Op& op) {
    hexpire_generic<kNotify>(shard, op, 1, true, "hpexpireat");
}

// ---- the read family, plus HPERSIST -----------------------------------------------------------

template <bool kNotify>
void hread_generic(Shard& shard, Op& op, Read kind) {
    KvObj* object = nullptr;
    if (!hash_ttl_lookup<kNotify>(shard, op, object)) return;
    FieldsSpec fields;
    if (!parse_fields_tail(op, 2, false, fields)) return;
    if (!object) { reply_all(op, fields.count, -2); return; }

    const int64_t now_ms = shard.now_ms();
    HashFieldTtl** slot = hash_ttl_slot(object);
    HashFieldTtl* ttls = slot ? *slot : nullptr;

    if (kind != Read::Persist) {
        reply_array_header(op.sink(), fields.count);
        for (uint32_t i = 0; i < fields.count; i++) {
            int64_t at = HashFieldTtl::kNone;
            if (!field_live(object, ttls, op.arg(fields.first + i), now_ms, at)) {
                reply_int(op.sink(), -2);
                continue;
            }
            if (at < 0) { reply_int(op.sink(), -1); continue; }
            switch (kind) {
                // Redis rounds every second-granularity answer UP (probed, not assumed).
                case Read::Ttl:          reply_int(op.sink(), (at - now_ms + 999) / 1000); break;
                case Read::Pttl:         reply_int(op.sink(), at - now_ms); break;
                case Read::ExpireTime:   reply_int(op.sink(), (at + 999) / 1000); break;
                case Read::PexpireTime:  reply_int(op.sink(), at); break;
                case Read::Persist:      break;
            }
        }
        return;
    }

    std::vector<int8_t> results;
    try {
        results.assign(fields.count, -2);
    } catch (const std::bad_alloc&) {
        reply_err(op.sink(), "ERR out of memory");
        return;
    }
    ObjectSizeTracker size_tracker(shard.store(), object);
    bool removed_any = false;
    for (uint32_t i = 0; i < fields.count; i++) {
        int64_t at = HashFieldTtl::kNone;
        if (!field_live(object, ttls, op.arg(fields.first + i), now_ms, at)) continue;
        if (at < 0) { results[i] = -1; continue; }
        ttls->erase(op.arg(fields.first + i));
        results[i] = 1;
        removed_any = true;
    }
    if (ttls && ttls->empty()) { delete ttls; *slot = nullptr; }
    hash_ttl_note_bytes(object);
    if constexpr (kNotify)
        if (removed_any) notify_record(shard, op, NOTIFY_HASH, NotifyEventId::Hpersist, op.key());
    reply_array_header(op.sink(), fields.count);
    for (uint32_t i = 0; i < fields.count; i++) reply_int(op.sink(), results[i]);
}

template <bool kNotify> void cmd_httl(Shard& shard, Op& op) {
    hread_generic<kNotify>(shard, op, Read::Ttl);
}
template <bool kNotify> void cmd_hpttl(Shard& shard, Op& op) {
    hread_generic<kNotify>(shard, op, Read::Pttl);
}
template <bool kNotify> void cmd_hexpiretime(Shard& shard, Op& op) {
    hread_generic<kNotify>(shard, op, Read::ExpireTime);
}
template <bool kNotify> void cmd_hpexpiretime(Shard& shard, Op& op) {
    hread_generic<kNotify>(shard, op, Read::PexpireTime);
}
template <bool kNotify> void cmd_hpersist(Shard& shard, Op& op) {
    hread_generic<kNotify>(shard, op, Read::Persist);
}

#define TOMO_HANDLER_PAIR(fn) fn<false>, 1, 1, 1, notify_handler<fn<true>>

static const CommandSpec kTable[] = {
    // name          min max flags                                handler
    {"HEXPIRE",       6, -1, CmdFlags::Write | CmdFlags::DenyOom, TOMO_HANDLER_PAIR(cmd_hexpire)},
    {"HPEXPIRE",      6, -1, CmdFlags::Write | CmdFlags::DenyOom, TOMO_HANDLER_PAIR(cmd_hpexpire)},
    {"HEXPIREAT",     6, -1, CmdFlags::Write | CmdFlags::DenyOom, TOMO_HANDLER_PAIR(cmd_hexpireat)},
    {"HPEXPIREAT",    6, -1, CmdFlags::Write | CmdFlags::DenyOom, TOMO_HANDLER_PAIR(cmd_hpexpireat)},
    {"HTTL",          5, -1, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_httl)},
    {"HPTTL",         5, -1, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_hpttl)},
    {"HEXPIRETIME",   5, -1, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_hexpiretime)},
    {"HPEXPIRETIME",  5, -1, CmdFlags::Readonly, TOMO_HANDLER_PAIR(cmd_hpexpiretime)},
    {"HPERSIST",      5, -1, CmdFlags::Write,    TOMO_HANDLER_PAIR(cmd_hpersist)},
};

#undef TOMO_HANDLER_PAIR

}  // namespace

// ---- shared reaping ---------------------------------------------------------------------------

// Removes every field whose deadline has passed. Idempotent and representation-agnostic; the two
// callers differ only in what they do with an emptied hash.
static uint32_t reap_due(KvObj* object, int64_t now_ms) {
    HashFieldTtl** slot = hash_ttl_slot(object);
    if (!slot || !*slot) return 0;
    HashFieldTtl* ttls = *slot;
    if (ttls->min_expire_ms() > now_ms) return 0;
    std::vector<std::string> due;
    try {
        ttls->for_each([&](Slice field, int64_t at) {
            if (at <= now_ms) due.emplace_back(field.p, field.n);
        });
    } catch (const std::bad_alloc&) {
        return 0;
    }
    uint32_t reaped = 0;
    for (const std::string& field : due) {
        const Slice slice(field.data(), static_cast<uint32_t>(field.size()));
        ttls->erase(slice);
        if (hash_ttl_field_erase(object, slice)) reaped++;
    }
    if (ttls->empty()) { delete ttls; *slot = nullptr; }
    hash_ttl_note_bytes(object);
    return reaped;
}

bool hash_ttl_active_reap(FlatStore& store, KvObj* object, int64_t now_ms, uint32_t& reaped) {
    (void)store;
    reaped = reap_due(object, now_ms);
    if (!reaped) return false;
    return hash_ttl_field_count(object) == 0;
}

KvObj* hash_ttl_on_access(Shard& shard, Op& op, KvObj* object, bool notify) {
    // Suppressed during a snapshot capture for the same reason active_expire() is: the frozen table
    // still has to serialize the pre-cut image. Every command in this file filters logically anyway,
    // so the only visible effect is that an ordinary HGET may still see a just-lapsed field until
    // the capture completes.
    if (shard.store().snapshot_active()) return object;
    const int64_t now_ms = shard.now_ms();
    const size_t before = kvobj_size(object);
    const uint32_t reaped = reap_due(object, now_ms);
    if (!reaped) return object;
    shard.store().note_field_expired(reaped);
    if (hash_ttl_field_count(object) != 0) {
        shard.store().note_object_size_change(before, kvobj_size(object));
        if (notify) notify_record(shard, op, NOTIFY_HASH, NotifyEventId::Hexpired, op.key());
        return object;
    }
    if (notify) notify_record(shard, op, NOTIFY_HASH, NotifyEventId::Hexpired, op.key());
    // erase() subtracts the object's whole footprint, so the growth bracket must not also report.
    (void)shard.store().aof().record_delete(op.key());
    if (notify) shard.store_erase<true>(op.hash, op.key());
    else        shard.store_erase<false>(op.hash, op.key());
    return nullptr;
}

void hash_ttl_clear_field(Shard& shard, KvObj* object, Slice field) {
    HashFieldTtl** slot = hash_ttl_slot(object);
    if (!slot || !*slot) return;
    if (!(*slot)->erase(field)) return;
    if ((*slot)->empty()) { delete *slot; *slot = nullptr; }
    hash_ttl_note_bytes(object);
    (void)shard;
}

CommandTable hash_ttl_command_table() {
    return {kTable, sizeof(kTable) / sizeof(kTable[0])};
}

// The read-only half of hash_ttl_on_access(), for SORT's BY/GET dereference: report whether ONE
// field has already lapsed, without reaping it, without touching the object's size accounting, and
// without emitting or erasing under a key name that is not the one the command was routed on.
bool hash_ttl_field_lapsed(const KvObj* object, Slice field, int64_t now_ms) {
    HashFieldTtl** slot = hash_ttl_slot(const_cast<KvObj*>(object));
    if (!slot || !*slot) return false;
    const HashFieldTtl* ttls = *slot;
    if (ttls->min_expire_ms() > now_ms) return false;
    const int64_t at = ttls->get(field);
    return at != HashFieldTtl::kNone && at <= now_ms;
}

}  // namespace tomo

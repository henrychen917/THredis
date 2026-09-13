// t_string.cc — string commands and type-agnostic keyspace commands.
//
// Handlers execute only on the shard owner and emit RESP through Op::Sink. Connection-local server
// commands live in t_server.cc.
#include "command.h"
#include "hll.h"
#include "xshard.h"
#include "notify.h"
#include "serialize.h"
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
#include <string>

namespace tomo {

void reply_maxmemory_oom(Op& op);  // defined below with tomo:: linkage; families share it

#define TOMO_STRING_NOTIFY_HANDLERS(X) \
    X(cmd_get) X(cmd_set) X(cmd_append) X(cmd_strlen) X(cmd_getrange) \
    X(cmd_setrange) X(cmd_setbit) X(cmd_getbit) X(cmd_bitcount) X(cmd_bitpos) \
    X(cmd_getset) X(cmd_setnx) X(cmd_setex) X(cmd_psetex) X(cmd_getex) X(cmd_getdel) \
    X(cmd_del) X(cmd_exists) X(cmd_incr) X(cmd_decr) X(cmd_incrby) X(cmd_decrby) \
    X(cmd_incrbyfloat) X(cmd_pfadd) X(cmd_pfcount) X(cmd_expire) X(cmd_pexpire) \
    X(cmd_expireat) X(cmd_pexpireat) X(cmd_ttl) X(cmd_pttl) X(cmd_persist) \
    X(cmd_expiretime) X(cmd_pexpiretime) X(cmd_type)

#ifndef TOMO_STRING_NOTIFY_TU
#define TOMO_DECLARE_STRING_NOTIFY(fn) void fn##_notify(Shard&, Op&);
TOMO_STRING_NOTIFY_HANDLERS(TOMO_DECLARE_STRING_NOTIFY)
#undef TOMO_DECLARE_STRING_NOTIFY
#endif

namespace {

bool eq_icase(Slice s, const char* lit) {
    const size_t n = std::strlen(lit);
    if (s.n != n) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char a = static_cast<unsigned char>(s.p[i]);
        unsigned char b = static_cast<unsigned char>(lit[i]);
        if (a >= 'a' && a <= 'z') a = static_cast<unsigned char>(a - ('a' - 'A'));
        if (b >= 'a' && b <= 'z') b = static_cast<unsigned char>(b - ('a' - 'A'));
        if (a != b) return false;
    }
    return true;
}

inline constexpr uint64_t kProtoMaxBulkLen = 512ull * 1024 * 1024;

bool parse_i64(Slice s, int64_t& out) {
    // Redis's string2ll accepts only the representation that formatting the resulting integer
    // produces again: no leading '+', leading zeroes, or negative zero.
    if (s.n == 0 || s.n >= 32) return false;
    uint32_t pos = 0;
    bool negative = false;
    if (s.p[pos] == '-') {
        negative = true;
        if (++pos == s.n) return false;
    }
    if (s.p[pos] == '0') {
        if (negative || pos + 1 != s.n) return false;
        out = 0;
        return true;
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
    if (negative) {
        out = value == (uint64_t{1} << 63) ? INT64_MIN : -static_cast<int64_t>(value);
    } else {
        out = static_cast<int64_t>(value);
    }
    return true;
}

// INCRBYFLOAT reads and writes long doubles through redis's string2ld grammar, which lives with
// the other float grammars in src/base/numeric.h. This is the one place a hexadecimal float was
// already accepted before this lane; the score and coordinate parsers now agree with it.
bool parse_long_double(Slice s, long double& out) {
    return parse_long_double_strict(s, out);
}

uint32_t format_long_double(char* text, size_t capacity, long double value) {
    // Redis's LD_STR_HUMAN mode intentionally uses fixed notation with 17 fractional digits,
    // trims the fractional zero suffix, and canonicalizes negative zero.
    int n = std::snprintf(text, capacity, "%.17Lf", value);
    if (n < 0 || static_cast<size_t>(n) >= capacity) return 0;
    if (char* dot = static_cast<char*>(std::memchr(text, '.', static_cast<size_t>(n)))) {
        char* end = text + n;
        while (end > dot + 1 && end[-1] == '0') end--;
        if (end == dot + 1) end = dot;
        n = static_cast<int>(end - text);
    }
    if (n == 2 && text[0] == '-' && text[1] == '0') {
        text[0] = '0';
        n = 1;
    }
    text[n] = '\0';
    return static_cast<uint32_t>(n);
}

Slice string_bytes(const KvObj* o, char (&integer)[24], KvObjRawReadBuffer& raw) {
    if (!o->is_int()) return kvobj_string_value(o, raw);
    return Slice(integer, i64_to_dec(integer, o->int_value()));
}

enum class StoreResult : uint8_t { Stored, Oom, InsertFailed, MaxmemoryOom };

void clear_reply(Op& op);

// FlatStore's insert/try_overwrite return admission enums (i-evict); every string write funnels
// through here so eviction OOM surfaces once, not at each call site.
StoreResult map_insert(FlatStore& store, uint64_t hash, KvObj* replacement) {
    const FlatStore::InsertResult inserted = store.insert(hash, replacement);
    if (inserted == FlatStore::InsertResult::Inserted) return StoreResult::Stored;
    store.discard_set_value(replacement);
    return inserted == FlatStore::InsertResult::MaxmemoryOom ? StoreResult::MaxmemoryOom
                                                             : StoreResult::InsertFailed;
}

StoreResult store_string(Shard& sh, Slice key, uint64_t hash, Slice value, int64_t expire_at_ms,
                         bool integer_encode, bool reserve_ttl_slot) {
    int64_t integer = 0;
    if (integer_encode && parse_i64(value, integer)) {
        KvObj* replacement = sh.store().make_set_int(
            key, integer, expire_at_ms, reserve_ttl_slot);
        if (!replacement) return StoreResult::Oom;
        return map_insert(sh.store(), hash, replacement);
    }

    // try_overwrite is the only in-place raw mutation. It rejects TTL-bearing and borrowed values;
    // the replacement path below then lets FlatStore retain any borrowed old allocation.
    if (expire_at_ms < 0 && !reserve_ttl_slot) {
        const FlatStore::OverwriteResult overwritten = sh.store().try_overwrite(hash, key, value);
        if (overwritten == FlatStore::OverwriteResult::Updated) return StoreResult::Stored;
        if (overwritten == FlatStore::OverwriteResult::MaxmemoryOom) return StoreResult::MaxmemoryOom;
    }

    KvObj* replacement = sh.store().make_set_string(
        key, value, expire_at_ms, reserve_ttl_slot);
    if (!replacement) return StoreResult::Oom;
    return map_insert(sh.store(), hash, replacement);
}

StoreResult store_integer(Shard& sh, Slice key, uint64_t hash, int64_t value,
                          int64_t expire_at_ms, bool reserve_ttl_slot) {
    KvObj* replacement = sh.store().make_set_int(key, value, expire_at_ms, reserve_ttl_slot);
    if (!replacement) return StoreResult::Oom;
    return map_insert(sh.store(), hash, replacement);
}

#ifdef TOMO_STRING_NOTIFY_TU
StoreResult map_insert_notify(Shard& sh, uint64_t hash, KvObj* replacement) {
    const FlatStore::InsertResult inserted = sh.store_insert<true>(hash, replacement);
    if (inserted == FlatStore::InsertResult::Inserted) return StoreResult::Stored;
    sh.store().discard_set_value(replacement);
    return inserted == FlatStore::InsertResult::MaxmemoryOom ? StoreResult::MaxmemoryOom
                                                             : StoreResult::InsertFailed;
}

StoreResult store_string_notify(Shard& sh, Slice key, uint64_t hash, Slice value,
                                int64_t expire_at_ms, bool integer_encode,
                                bool reserve_ttl_slot) {
    int64_t integer = 0;
    if (integer_encode && parse_i64(value, integer)) {
        KvObj* replacement = sh.store().make_set_int(
            key, integer, expire_at_ms, reserve_ttl_slot);
        if (!replacement) return StoreResult::Oom;
        return map_insert_notify(sh, hash, replacement);
    }
    if (expire_at_ms < 0 && !reserve_ttl_slot) {
        const FlatStore::OverwriteResult overwritten =
            sh.store_try_overwrite<true>(hash, key, value);
        if (overwritten == FlatStore::OverwriteResult::Updated) return StoreResult::Stored;
        if (overwritten == FlatStore::OverwriteResult::MaxmemoryOom)
            return StoreResult::MaxmemoryOom;
    }
    KvObj* replacement = sh.store().make_set_string(
        key, value, expire_at_ms, reserve_ttl_slot);
    if (!replacement) return StoreResult::Oom;
    return map_insert_notify(sh, hash, replacement);
}
#endif

template <bool kNotify>
StoreResult store_string_for(Shard& sh, Slice key, uint64_t hash, Slice value,
                             int64_t expire_at_ms, bool integer_encode,
                             bool reserve_ttl_slot) {
    if constexpr (kNotify) {
#ifdef TOMO_STRING_NOTIFY_TU
        return store_string_notify(
            sh, key, hash, value, expire_at_ms, integer_encode, reserve_ttl_slot);
#else
        static_assert(!kNotify, "armed string handler instantiated in the clean translation unit");
#endif
    }
    return store_string(sh, key, hash, value, expire_at_ms, integer_encode, reserve_ttl_slot);
}

template <bool kNotify>
StoreResult store_integer_for(Shard& sh, Slice key, uint64_t hash, int64_t value,
                              int64_t expire_at_ms, bool reserve_ttl_slot) {
    if constexpr (!kNotify)
        return store_integer(sh, key, hash, value, expire_at_ms, reserve_ttl_slot);
#ifdef TOMO_STRING_NOTIFY_TU
    KvObj* replacement = sh.store().make_set_int(key, value, expire_at_ms, reserve_ttl_slot);
    if (!replacement) return StoreResult::Oom;
    return map_insert_notify(sh, hash, replacement);
#else
    static_assert(!kNotify, "armed string handler instantiated in the clean translation unit");
#endif
}

void reply_store_error(Op& op, StoreResult result, bool discard_existing_reply = false) {
    if (discard_existing_reply) clear_reply(op);
    if (result == StoreResult::MaxmemoryOom) reply_maxmemory_oom(op);
    else if (result == StoreResult::Oom) reply_err(op.sink(), "ERR out of memory");
    else reply_err(op.sink(), "ERR keyspace insert failed");
}

void reply_invalid_expire(Op& op, const char* command) {
    char msg[96];
    std::snprintf(msg, sizeof(msg), "ERR invalid expire time in '%s' command", command);
    reply_err(op.sink(), msg);
}

// Redis separates the two ways an expire argument can be refused: a token that is not a number at
// all is "value is not an integer or out of range", and a number whose deadline cannot be
// represented is "invalid expire time in '<cmd>' command". Reporting the latter for both made
// EXPIRE/SET/GETEX/SETEX answer a wrong-but-plausible error for every non-numeric argument.
void reply_not_integer(Op& op) {
    reply_err(op.sink(), "ERR value is not an integer or out of range");
}

enum class ExpireArg : uint8_t { Ok, NotInteger, OutOfRange };

void clear_reply(Op& op) {
    op.clear_reply();       // bytes AND the reply code -- a discarded reply must leave nothing
}

}  // namespace

#ifndef TOMO_STRING_NOTIFY_TU
XshardStringStoreResult xshard_store_string(Shard& shard, Slice key, uint64_t hash, Slice value,
                                             int64_t expire_at_ms, bool integer_encode,
                                             bool reserve_ttl_slot) {
    switch (store_string(
        shard, key, hash, value, expire_at_ms, integer_encode, reserve_ttl_slot)) {
        case StoreResult::Stored: return XshardStringStoreResult::Stored;
        case StoreResult::Oom: return XshardStringStoreResult::Oom;
        case StoreResult::InsertFailed: return XshardStringStoreResult::InsertFailed;
        case StoreResult::MaxmemoryOom: return XshardStringStoreResult::Maxmemory;
    }
    return XshardStringStoreResult::InsertFailed;
}
#endif

#ifdef TOMO_STRING_NOTIFY_TU
XshardStringStoreResult xshard_store_string_notify(Shard& shard, Slice key, uint64_t hash,
                                                   Slice value, int64_t expire_at_ms,
                                                   bool integer_encode,
                                                   bool reserve_ttl_slot) {
    switch (store_string_notify(
        shard, key, hash, value, expire_at_ms, integer_encode, reserve_ttl_slot)) {
        case StoreResult::Stored: return XshardStringStoreResult::Stored;
        case StoreResult::Oom: return XshardStringStoreResult::Oom;
        case StoreResult::InsertFailed: return XshardStringStoreResult::InsertFailed;
        case StoreResult::MaxmemoryOom: return XshardStringStoreResult::Maxmemory;
    }
    return XshardStringStoreResult::InsertFailed;
}
#endif

#ifndef TOMO_STRING_NOTIFY_TU
KvObj* xshard_make_string(Slice key, Slice value, int64_t expire_at_ms, bool integer_encode,
                          bool reserve_ttl_slot) {
    int64_t integer = 0;
    if (integer_encode && parse_i64(value, integer))
        return kvobj_new_int(key, integer, expire_at_ms, reserve_ttl_slot);
    return kvobj_new_string(key, value, expire_at_ms, reserve_ttl_slot);
}

KvObj* xshard_make_atomic_string(Shard& shard, Slice key, Slice value,
                                 int64_t expire_at_ms, bool integer_encode,
                                 bool reserve_ttl_slot) {
    int64_t integer = 0;
    const bool has_ttl_slot = reserve_ttl_slot || expire_at_ms >= 0;
    if (integer_encode && parse_i64(value, integer)) {
        const size_t allocation = good_size(
            kvobj_alloc_size(key.n, 0, has_ttl_slot, Enc::Int));
        void* memory = shard.store().atomic_acquire_value_block(allocation);
        return kvobj_init_int(memory, key, integer, expire_at_ms, reserve_ttl_slot);
    }
    if (value.n > kEmbedThreshold)
        return kvobj_new_string(key, value, expire_at_ms, reserve_ttl_slot);
    const size_t allocation = good_size(
        kvobj_alloc_size(key.n, value.n, has_ttl_slot, Enc::Raw));
    void* memory = shard.store().atomic_acquire_value_block(allocation);
    return kvobj_init_raw_string(memory, key, value, expire_at_ms, reserve_ttl_slot);
}

// tomo:: linkage: every type family replies this exact text on admission failure.
void reply_maxmemory_oom(Op& op) {
    reply_err(op.sink(), "OOM command not allowed when used memory > 'maxmemory'.");
}
#endif

namespace {

void reply_string_bulk(Op& op, const KvObj* o) {
    if (!o) { reply_null(op.sink(), op.resp3()); return; }
    if (o->is_int()) {
        char text[24];
        const uint32_t n = i64_to_dec(text, o->int_value());
        reply_bulk(op.sink(), Slice(text, n));
        return;
    }
    if constexpr (kReadLocalSetTaxAtomicRaw) {
        if (o->encoding() == Enc::Raw) {
            const uint32_t length = kvobj_read_local_raw_length(o);
            auto sink = op.sink();
            char* frame = sink.reserve(24 + static_cast<size_t>(length) + 2);
            char* payload = frame;
            *payload++ = '$';
            payload += u64_to_dec(payload, length);
            *payload++ = '\r';
            *payload++ = '\n';
            kvobj_read_local_copy_raw(o, o->read_local_flags(), length, payload);
            payload += length;
            *payload++ = '\r';
            *payload++ = '\n';
            sink.advance(static_cast<size_t>(payload - frame));
            return;
        }
    }
    KvObjRawReadBuffer raw;
    reply_bulk(op.sink(), kvobj_string_value(o, raw));
}

// GET alone may borrow FlatStore bytes. GETEX/GETDEL/SET GET copy before mutation; extending the
// borrow protocol to mutation replies would turn a string-only fast path into collection policy.
template <bool kNotify, bool kAllowBorrow = true>
void cmd_get(Shard& sh, Op& op) {
    KvObj* o = sh.store_find_read<kNotify>(op.hash, op.key());
    if (!o) { reply_null(op.sink(), op.resp3()); return; }
    auto sink = op.sink();
    if (!obj_type_check(o, Type::String, sink)) return;
    if (o->is_int()) { reply_string_bulk(op, o); return; }
    if constexpr (kReadLocalSetTaxAtomicRaw) {
        if (o->encoding() == Enc::Raw) {
            if constexpr (kReadLocalSetTaxVariant ==
                              ReadLocalSetTaxVariant::ObjectSequenceOverwrite &&
                          kAllowBorrow) {
                const uint32_t length = kvobj_read_local_raw_length(o);
                const uint32_t zc_min = sh.zc_min();
                if (zc_min && length >= zc_min) {
                    reply_bulk_header(op.sink(), length);
                    op.zc_ptr = o->str_data();
                    op.zc_len = length;
                    op.zc_shard = sh.id();
                    // The owner publishes this registry entry before it can run another command;
                    // selector 3's overwrite gate then leaves these exact bytes immutable.
                    sh.store().borrow(op.zc_ptr);
                    return;
                }
            }
            if constexpr (!kAllowBorrow) {
                const uint32_t zc_min = sh.zc_min();
                if (zc_min && kvobj_read_local_raw_length(o) >= zc_min) op.mark_no_borrow();
            }
            reply_string_bulk(op, o);
            return;
        }
    }
    KvObjRawReadBuffer raw;
    const Slice value = kvobj_string_value(o, raw);
    if constexpr (kAllowBorrow) {
        const uint32_t zc_min = sh.zc_min();
        bool may_borrow = true;
        if constexpr (kReadLocalSetTaxVariant == ReadLocalSetTaxVariant::SequenceOverwrite)
            may_borrow = o->encoding() != Enc::Raw;
        if (may_borrow && zc_min && value.n >= zc_min) {
            reply_bulk_header(op.sink(), value.n);
            op.zc_ptr = value.p;
            op.zc_len = value.n;
            op.zc_shard = sh.id();
            sh.store().borrow(value.p);
            return;
        }
    } else {
        // The TLS-only handler variant copies the value. Stamp the operation when the plaintext
        // handler would have borrowed so the IO boundary can prove this producer gate fired.
        const uint32_t zc_min = sh.zc_min();
        if (zc_min && value.n >= zc_min) op.mark_no_borrow();
    }
    reply_bulk(op.sink(), value);
}

enum class ExpireKind : uint8_t { None, Ex, Px, Exat, Pxat };

struct SetOptions {
    bool nx = false;
    bool xx = false;
    bool get = false;
    bool keep_ttl = false;
    ExpireKind expire_kind = ExpireKind::None;
    int64_t expire_at_ms = -1;
};

ExpireArg expiry_at(Shard& sh, Slice arg, ExpireKind kind, int64_t& out) {
    int64_t value = 0;
    if (!parse_i64(arg, value)) return ExpireArg::NotInteger;
    if (value <= 0) return ExpireArg::OutOfRange;
    const bool seconds = kind == ExpireKind::Ex || kind == ExpireKind::Exat;
    if (seconds) {
        if (value > INT64_MAX / 1000) return ExpireArg::OutOfRange;
        value *= 1000;
    }
    if (kind == ExpireKind::Ex || kind == ExpireKind::Px) {
        if (value > INT64_MAX - sh.now_ms()) return ExpireArg::OutOfRange;
        value += sh.now_ms();
    }
    if (value <= 0) return ExpireArg::OutOfRange;
    out = value;
    return ExpireArg::Ok;
}

// Shared tail for every command that takes one EX/PX/EXAT/PXAT argument.
bool apply_expiry_arg(Shard& sh, Op& op, Slice arg, ExpireKind kind, int64_t& out,
                      const char* command) {
    const ExpireArg parsed = expiry_at(sh, arg, kind, out);
    if (parsed == ExpireArg::Ok) return true;
    if (parsed == ExpireArg::NotInteger) reply_not_integer(op);
    else reply_invalid_expire(op, command);
    return false;
}

bool parse_set_options(Shard& sh, Op& op, SetOptions& options) {
    Slice expire_arg;
    for (uint32_t i = 3; i < op.argc(); i++) {
        const Slice arg = op.arg(i);
        if (eq_icase(arg, "NX") && !options.xx) {
            options.nx = true;
        } else if (eq_icase(arg, "XX") && !options.nx) {
            options.xx = true;
        } else if (eq_icase(arg, "GET")) {
            options.get = true;
        } else if (eq_icase(arg, "KEEPTTL") && options.expire_kind == ExpireKind::None) {
            options.keep_ttl = true;
        } else {
            ExpireKind kind = ExpireKind::None;
            if (eq_icase(arg, "EX")) kind = ExpireKind::Ex;
            else if (eq_icase(arg, "PX")) kind = ExpireKind::Px;
            else if (eq_icase(arg, "EXAT")) kind = ExpireKind::Exat;
            else if (eq_icase(arg, "PXAT")) kind = ExpireKind::Pxat;
            if (kind == ExpireKind::None || options.keep_ttl || i + 1 >= op.argc() ||
                (options.expire_kind != ExpireKind::None && options.expire_kind != kind)) {
                reply_syntax(op.sink());
                return false;
            }
            options.expire_kind = kind;
            expire_arg = op.arg(++i);
        }
    }
    if (options.expire_kind != ExpireKind::None &&
        !apply_expiry_arg(sh, op, expire_arg, options.expire_kind, options.expire_at_ms, "set"))
        return false;
    return true;
}

template <bool kNotify>
void cmd_set(Shard& sh, Op& op) {
    // Preserve the allocation-free raw fast path while still applying Redis integer encoding.
    if (op.argc() == 3) {
        const StoreResult result =
            store_string_for<kNotify>(sh, op.key(), op.hash, op.arg(2), -1, true, false);
        if (result != StoreResult::Stored) { reply_store_error(op, result); return; }
        if constexpr (kNotify)
            notify_record(sh, op, NOTIFY_STRING, NotifyEventId::Set, op.key());
        reply_ok(op.sink());
        return;
    }

    SetOptions options;
    if (!parse_set_options(sh, op, options)) return;
    KvObj* old = sh.store_find<kNotify>(op.hash, op.key());
    if (options.get) {
        auto sink = op.sink();
        if (!obj_type_check(old, Type::String, sink)) return;
        reply_string_bulk(op, old);
    }
    if ((options.nx && old) || (options.xx && !old)) {
        if (!options.get) reply_null(op.sink(), op.resp3());
        return;
    }

    int64_t expire = options.expire_at_ms;
    const bool reserve_ttl_slot = options.keep_ttl && old && old->has_ttl_slot();
    if (options.keep_ttl && old) expire = sh.store().deadline(op.hash, old);
    // Redis treats an already elapsed absolute SET deadline as set-then-expire: the old value is
    // removed, no dead replacement is left for DBSIZE/active expiry, and GET (if requested) keeps
    // the reply copied above. Relative EX/PX cannot reach here with a non-future deadline.
    if (expire >= 0 && expire <= sh.now_ms()) {
        if (old) sh.store_erase<kNotify>(op.hash, op.key());
        else if constexpr (kNotify)
            notify_record(sh, op, NOTIFY_GENERIC, NotifyEventId::Del, op.key());
        if (!options.get) reply_ok(op.sink());
        return;
    }
    const StoreResult result =
        store_string_for<kNotify>(sh, op.key(), op.hash, op.arg(2), expire, true,
                                  reserve_ttl_slot);
    if (result != StoreResult::Stored) { reply_store_error(op, result, options.get); return; }
    if constexpr (kNotify) {
        notify_record(sh, op, NOTIFY_STRING, NotifyEventId::Set, op.key());
        if (options.expire_kind != ExpireKind::None)
            notify_record(sh, op, NOTIFY_GENERIC, NotifyEventId::Expire, op.key());
    }
    if (!options.get) reply_ok(op.sink());
}

// Keep the clean specialization source-identical to the pre-notification handler. Besides being
// the construction-time off path, this prevents the armed template's extra control-flow graph from
// perturbing GCC's inlining decisions in the p128 SET cell.
#ifndef TOMO_STRING_NOTIFY_TU
template <>
void cmd_set<false>(Shard& sh, Op& op) {
    if (op.argc() == 3) {
        const StoreResult result = store_string(
            sh, op.key(), op.hash, op.arg(2), -1, true, false);
        if (result != StoreResult::Stored) { reply_store_error(op, result); return; }
        reply_ok(op.sink());
        return;
    }

    SetOptions options;
    if (!parse_set_options(sh, op, options)) return;
    KvObj* old = sh.store().find(op.hash, op.key());
    if (options.get) {
        auto sink = op.sink();
        if (!obj_type_check(old, Type::String, sink)) return;
        reply_string_bulk(op, old);
    }
    if ((options.nx && old) || (options.xx && !old)) {
        if (!options.get) reply_null(op.sink(), op.resp3());
        return;
    }

    int64_t expire = options.expire_at_ms;
    const bool reserve_ttl_slot = options.keep_ttl && old && old->has_ttl_slot();
    if (options.keep_ttl && old) expire = sh.store().deadline(op.hash, old);
    if (expire >= 0 && expire <= sh.now_ms()) {
        if (old) sh.store().erase(op.hash, op.key());
        if (!options.get) reply_ok(op.sink());
        return;
    }
    const StoreResult result = store_string(
        sh, op.key(), op.hash, op.arg(2), expire, true, reserve_ttl_slot);
    if (result != StoreResult::Stored) { reply_store_error(op, result, options.get); return; }
    if (!options.get) reply_ok(op.sink());
}
#endif

// Same grammar shape as parse_set_options: the option list is a loop, repeating ONE form is legal
// and the last value wins, mixing two different forms is a syntax error, and the value itself is
// parsed once after the loop. Redis's GETEX shares SET's extended-argument parser, so `GETEX k EX 1
// EX 2` sets 2 and `GETEX k PERSIST PERSIST` is accepted -- the old fixed-arity form answered
// "wrong number of arguments" for both.
bool parse_getex_options(Op& op, ExpireKind& kind, Slice& expire_arg, bool& persist) {
    for (uint32_t i = 2; i < op.argc(); i++) {
        const Slice arg = op.arg(i);
        if (eq_icase(arg, "PERSIST")) {
            if (kind != ExpireKind::None) { reply_syntax(op.sink()); return false; }
            persist = true;
            continue;
        }
        ExpireKind form = ExpireKind::None;
        if (eq_icase(arg, "EX")) form = ExpireKind::Ex;
        else if (eq_icase(arg, "PX")) form = ExpireKind::Px;
        else if (eq_icase(arg, "EXAT")) form = ExpireKind::Exat;
        else if (eq_icase(arg, "PXAT")) form = ExpireKind::Pxat;
        if (form == ExpireKind::None || persist || i + 1 >= op.argc() ||
            (kind != ExpireKind::None && kind != form)) {
            reply_syntax(op.sink());
            return false;
        }
        kind = form;
        expire_arg = op.arg(++i);
    }
    return true;
}

template <bool kNotify>
void cmd_getex(Shard& sh, Op& op) {
    ExpireKind kind = ExpireKind::None;
    Slice expire_arg;
    bool persist = false;
    if (!parse_getex_options(op, kind, expire_arg, persist)) return;

    KvObj* o = sh.store_find_read<kNotify>(op.hash, op.key());
    if (!o) { reply_null(op.sink(), op.resp3()); return; }
    auto sink = op.sink();
    if (!obj_type_check(o, Type::String, sink)) return;

    int64_t at = -1;
    if (kind != ExpireKind::None && !apply_expiry_arg(sh, op, expire_arg, kind, at, "getex"))
        return;
    reply_string_bulk(op, o);

    FlatStore::TtlResult result = FlatStore::TtlResult::NoChange;
    if (kind != ExpireKind::None && at <= sh.now_ms()) {
        sh.store_erase<kNotify>(op.hash, op.key());
        return;
    } else if (kind != ExpireKind::None) {
        result = sh.store_set_expire<kNotify>(op.hash, op.key(), at);
    } else if (persist) {
        result = sh.store_persist<kNotify>(op.hash, op.key());
    }
    if (result == FlatStore::TtlResult::Oom || result == FlatStore::TtlResult::MaxmemoryOom) {
        clear_reply(op);
        if (result == FlatStore::TtlResult::MaxmemoryOom) reply_maxmemory_oom(op);
        else reply_err(op.sink(), "ERR out of memory");
    } else if constexpr (kNotify) if (result == FlatStore::TtlResult::Updated) {
        notify_record(sh, op, NOTIFY_GENERIC,
                      persist ? NotifyEventId::Persist : NotifyEventId::Expire, op.key());
    }
}

template <bool kNotify>
void cmd_getdel(Shard& sh, Op& op) {
    KvObj* o = sh.store_find_read<kNotify>(op.hash, op.key());
    if (!o) { reply_null(op.sink(), op.resp3()); return; }
    auto sink = op.sink();
    if (!obj_type_check(o, Type::String, sink)) return;
    reply_string_bulk(op, o);
    sh.store_erase<kNotify>(op.hash, op.key());
}

template <bool kNotify>
void cmd_del(Shard& sh, Op& op) {
    uint64_t removed = 0;
    for (uint32_t i = 1; i < op.argc(); i++) {
        const uint64_t hash = i == 1 ? op.hash : FlatStore::hash_key(op.arg(i));
        removed += sh.store_erase<kNotify>(hash, op.arg(i));
    }
    reply_int(op.sink(), static_cast<long long>(removed));
}

template <bool kNotify>
void cmd_exists(Shard& sh, Op& op) {
    uint64_t found = 0;
    for (uint32_t i = 1; i < op.argc(); i++) {
        const uint64_t hash = i == 1 ? op.hash : FlatStore::hash_key(op.arg(i));
        found += sh.store_find_read<kNotify>(hash, op.arg(i)) != nullptr;
    }
    reply_int(op.sink(), static_cast<long long>(found));
}

template <bool kNotify>
void cmd_append(Shard& sh, Op& op) {
    KvObj* o = sh.store_find<kNotify>(op.hash, op.key());
    if (!o) {
        const StoreResult result =
            store_string_for<kNotify>(sh, op.key(), op.hash, op.arg(2), -1, true, false);
        if (result != StoreResult::Stored) { reply_store_error(op, result); return; }
        if constexpr (kNotify)
            notify_record(sh, op, NOTIFY_STRING, NotifyEventId::Append, op.key());
        reply_int(op.sink(), op.arg(2).n);
        return;
    }
    auto sink = op.sink();
    if (!obj_type_check(o, Type::String, sink)) return;

    char integer[24];
    KvObjRawReadBuffer raw;
    const Slice old = string_bytes(o, integer, raw);
    // A raw empty append changes no observable value or encoding. Integer encoding is the one
    // exception: Redis's append path materializes it even when the appended byte count is zero.
    if (op.arg(2).n == 0 && !o->is_int()) {
        if constexpr (kNotify)
            notify_record(sh, op, NOTIFY_STRING, NotifyEventId::Append, op.key());
        reply_int(op.sink(), old.n); return;
    }
    const uint64_t total = static_cast<uint64_t>(old.n) + op.arg(2).n;
    if (total > kProtoMaxBulkLen) {
        reply_err(op.sink(), "ERR string exceeds maximum allowed size (proto-max-bulk-len)");
        return;
    }
    char* merged = static_cast<char*>(std::malloc(total ? static_cast<size_t>(total) : 1));
    if (!merged) { reply_err(op.sink(), "ERR out of memory"); return; }
    std::memcpy(merged, old.p, old.n);
    std::memcpy(merged + old.n, op.arg(2).p, op.arg(2).n);
    const int64_t expire = sh.store().deadline(op.hash, o);
    const StoreResult result = store_string_for<kNotify>(
        sh, op.key(), op.hash, Slice(merged, static_cast<uint32_t>(total)), expire, false,
        o->has_ttl_slot());
    std::free(merged);
    if (result != StoreResult::Stored) { reply_store_error(op, result); return; }
    if constexpr (kNotify)
        notify_record(sh, op, NOTIFY_STRING, NotifyEventId::Append, op.key());
    reply_int(op.sink(), static_cast<long long>(total));
}

template <bool kNotify>
void cmd_strlen(Shard& sh, Op& op) {
    KvObj* o = sh.store_find_read<kNotify>(op.hash, op.key());
    if (!o) { reply_int(op.sink(), 0); return; }
    auto sink = op.sink();
    if (!obj_type_check(o, Type::String, sink)) return;
    if (!o->is_int()) {
        reply_int(op.sink(), kvobj_string_length(o));
        return;
    }
    char integer[24];
    KvObjRawReadBuffer raw;
    reply_int(op.sink(), string_bytes(o, integer, raw).n);
}

template <bool kNotify>
void cmd_getrange(Shard& sh, Op& op) {
    int64_t start = 0, end = 0;
    if (!parse_i64(op.arg(2), start) || !parse_i64(op.arg(3), end)) {
        reply_err(op.sink(), "ERR value is not an integer or out of range");
        return;
    }
    KvObj* o = sh.store_find_read<kNotify>(op.hash, op.key());
    if (!o) { reply_emptystr(op.sink()); return; }
    auto sink = op.sink();
    if (!obj_type_check(o, Type::String, sink)) return;

    char integer[24];
    KvObjRawReadBuffer raw;
    const Slice value = string_bytes(o, integer, raw);
    const int64_t length = value.n;
    if (start < 0 && end < 0 && start > end) { reply_emptystr(op.sink()); return; }
    if (start < 0) start = length + start;
    if (end < 0) end = length + end;
    if (start < 0) start = 0;
    if (end < 0) end = 0;
    if (end >= length) end = length - 1;
    if (start > end || length == 0) { reply_emptystr(op.sink()); return; }
    reply_bulk(op.sink(), Slice(value.p + start, static_cast<uint32_t>(end - start + 1)));
}

template <bool kNotify>
void cmd_setrange(Shard& sh, Op& op) {
    int64_t offset = 0;
    if (!parse_i64(op.arg(2), offset)) {
        reply_err(op.sink(), "ERR value is not an integer or out of range");
        return;
    }
    if (offset < 0) { reply_err(op.sink(), "ERR offset is out of range"); return; }

    KvObj* o = sh.store_find<kNotify>(op.hash, op.key());
    if (o) {
        auto sink = op.sink();
        if (!obj_type_check(o, Type::String, sink)) return;
    }
    if (op.arg(3).n == 0) {
        if (!o) { reply_int(op.sink(), 0); return; }
        char integer[24];
        KvObjRawReadBuffer raw;
        reply_int(op.sink(), string_bytes(o, integer, raw).n);
        return;
    }

    const uint64_t write_end = static_cast<uint64_t>(offset) + op.arg(3).n;
    if (write_end > kProtoMaxBulkLen) {
        reply_err(op.sink(), "ERR string exceeds maximum allowed size (proto-max-bulk-len)");
        return;
    }
    char integer[24];
    KvObjRawReadBuffer raw;
    const Slice old = o ? string_bytes(o, integer, raw) : Slice("", 0);
    const uint32_t new_length = static_cast<uint32_t>(std::max<uint64_t>(old.n, write_end));
    char* changed = static_cast<char*>(std::malloc(new_length));
    if (!changed) { reply_err(op.sink(), "ERR out of memory"); return; }
    std::memcpy(changed, old.p, old.n);
    if (new_length > old.n) std::memset(changed + old.n, 0, new_length - old.n);
    std::memcpy(changed + offset, op.arg(3).p, op.arg(3).n);
    const int64_t expire = o ? sh.store().deadline(op.hash, o) : -1;
    const StoreResult result =
        store_string_for<kNotify>(sh, op.key(), op.hash, Slice(changed, new_length), expire, false,
                                  o && o->has_ttl_slot());
    std::free(changed);
    if (result != StoreResult::Stored) { reply_store_error(op, result); return; }
    if constexpr (kNotify)
        notify_record(sh, op, NOTIFY_STRING, NotifyEventId::Setrange, op.key());
    reply_int(op.sink(), new_length);
}

bool parse_bit_offset(Op& op, Slice argument, uint64_t& offset) {
    int64_t parsed = 0;
    if (!parse_i64(argument, parsed) || parsed < 0 ||
        (static_cast<uint64_t>(parsed) >> 3) >= kProtoMaxBulkLen) {
        reply_err(op.sink(), "ERR bit offset is not an integer or out of range");
        return false;
    }
    offset = static_cast<uint64_t>(parsed);
    return true;
}

template <bool kNotify>
void cmd_setbit(Shard& sh, Op& op) {
    uint64_t offset = 0;
    if (!parse_bit_offset(op, op.arg(2), offset)) return;
    int64_t bit_value = 0;
    if (!parse_i64(op.arg(3), bit_value) || (bit_value != 0 && bit_value != 1)) {
        reply_err(op.sink(), "ERR bit is not an integer or out of range");
        return;
    }

    KvObj* o = sh.store_find<kNotify>(op.hash, op.key());
    if (o) {
        auto sink = op.sink();
        if (!obj_type_check(o, Type::String, sink)) return;
    }
    char integer[24];
    KvObjRawReadBuffer raw;
    const Slice old = o ? string_bytes(o, integer, raw) : Slice("", 0);
    const uint32_t byte = static_cast<uint32_t>(offset >> 3);
    const uint8_t mask = static_cast<uint8_t>(1u << (7 - (offset & 7)));
    const int old_bit = byte < old.n && (static_cast<uint8_t>(old.p[byte]) & mask) ? 1 : 0;
    const uint32_t new_length = std::max<uint32_t>(old.n, byte + 1);

    // Redis materializes integer-encoded strings before a bit write, even when the selected bit
    // already has the requested value. Raw values with no growth and no change remain untouched.
    if (o && !o->is_int() && new_length == old.n && old_bit == bit_value) {
        if constexpr (kNotify)
            notify_record(sh, op, NOTIFY_STRING, NotifyEventId::Setbit, op.key());
        reply_int(op.sink(), old_bit);
        return;
    }

    char* changed = static_cast<char*>(std::malloc(new_length));
    if (!changed) { reply_err(op.sink(), "ERR out of memory"); return; }
    if (old.n) std::memcpy(changed, old.p, old.n);
    if (new_length > old.n) std::memset(changed + old.n, 0, new_length - old.n);
    uint8_t& selected = reinterpret_cast<uint8_t*>(changed)[byte];
    selected = bit_value ? static_cast<uint8_t>(selected | mask)
                         : static_cast<uint8_t>(selected & ~mask);
    const int64_t expire = o ? sh.store().deadline(op.hash, o) : -1;
    const StoreResult result =
        store_string_for<kNotify>(sh, op.key(), op.hash, Slice(changed, new_length), expire, false,
                                  o && o->has_ttl_slot());
    std::free(changed);
    if (result != StoreResult::Stored) { reply_store_error(op, result); return; }
    if constexpr (kNotify)
        notify_record(sh, op, NOTIFY_STRING, NotifyEventId::Setbit, op.key());
    reply_int(op.sink(), old_bit);
}

template <bool kNotify>
void cmd_getbit(Shard& sh, Op& op) {
    uint64_t offset = 0;
    if (!parse_bit_offset(op, op.arg(2), offset)) return;
    KvObj* o = sh.store_find_read<kNotify>(op.hash, op.key());
    if (!o) { reply_int(op.sink(), 0); return; }
    auto sink = op.sink();
    if (!obj_type_check(o, Type::String, sink)) return;
    char integer[24];
    KvObjRawReadBuffer raw;
    const Slice value = string_bytes(o, integer, raw);
    const uint64_t byte = offset >> 3;
    const uint8_t mask = static_cast<uint8_t>(1u << (7 - (offset & 7)));
    const bool set = byte < value.n && (static_cast<uint8_t>(value.p[byte]) & mask);
    reply_int(op.sink(), set ? 1 : 0);
}

uint64_t bitmap_popcount(const uint8_t* bytes, size_t length) {
    uint64_t count = 0;
    while (length >= sizeof(uint64_t)) {
        uint64_t word;
        std::memcpy(&word, bytes, sizeof(word));
        count += static_cast<uint64_t>(__builtin_popcountll(word));
        bytes += sizeof(word);
        length -= sizeof(word);
    }
    while (length--) count += static_cast<uint64_t>(__builtin_popcount(*bytes++));
    return count;
}

bool parse_bitmap_unit(Op& op, Slice unit, bool& bits) {
    if (eq_icase(unit, "bit")) bits = true;
    else if (eq_icase(unit, "byte")) bits = false;
    else { reply_syntax(op.sink()); return false; }
    return true;
}

template <bool kNotify>
void cmd_bitcount(Shard& sh, Op& op) {
    int64_t start = 0, end = 0;
    bool bit_unit = false;
    const bool ranged = op.argc() == 4 || op.argc() == 5;
    if (ranged) {
        if (!parse_i64(op.arg(2), start) || !parse_i64(op.arg(3), end)) {
            reply_err(op.sink(), "ERR value is not an integer or out of range");
            return;
        }
        if (op.argc() == 5 && !parse_bitmap_unit(op, op.arg(4), bit_unit)) return;
    } else if (op.argc() != 2) {
        reply_syntax(op.sink());
        return;
    }

    KvObj* o = sh.store_find_read<kNotify>(op.hash, op.key());
    if (!o) { reply_int(op.sink(), 0); return; }
    auto sink = op.sink();
    if (!obj_type_check(o, Type::String, sink)) return;
    char integer[24];
    KvObjRawReadBuffer raw;
    const Slice value = string_bytes(o, integer, raw);
    if (!ranged) {
        reply_int(op.sink(), static_cast<long long>(bitmap_popcount(
            reinterpret_cast<const uint8_t*>(value.p), value.n)));
        return;
    }

    int64_t total = bit_unit ? static_cast<int64_t>(value.n) * 8 : value.n;
    if (start < 0 && end < 0 && start > end) { reply_int(op.sink(), 0); return; }
    if (start < 0) start = total + start;
    if (end < 0) end = total + end;
    if (start < 0) start = 0;
    if (end < 0) end = 0;
    if (end >= total) end = total - 1;
    if (start > end) { reply_int(op.sink(), 0); return; }

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(value.p);
    if (!bit_unit) {
        reply_int(op.sink(), static_cast<long long>(
            bitmap_popcount(bytes + start, static_cast<size_t>(end - start + 1))));
        return;
    }

    const uint64_t first_byte = static_cast<uint64_t>(start) >> 3;
    const uint64_t last_byte = static_cast<uint64_t>(end) >> 3;
    const uint8_t first_mask = static_cast<uint8_t>(0xffu >> (start & 7));
    const uint8_t last_mask = static_cast<uint8_t>((0xffu << (7 - (end & 7))) & 0xffu);
    uint64_t count = 0;
    if (first_byte == last_byte) {
        count = static_cast<uint64_t>(__builtin_popcount(
            static_cast<unsigned>(bytes[first_byte] & first_mask & last_mask)));
    } else {
        count += static_cast<uint64_t>(__builtin_popcount(
            static_cast<unsigned>(bytes[first_byte] & first_mask)));
        if (last_byte > first_byte + 1)
            count += bitmap_popcount(bytes + first_byte + 1,
                                     static_cast<size_t>(last_byte - first_byte - 1));
        count += static_cast<uint64_t>(__builtin_popcount(
            static_cast<unsigned>(bytes[last_byte] & last_mask)));
    }
    reply_int(op.sink(), static_cast<long long>(count));
}

int64_t bitmap_find_bit(const uint8_t* bytes, uint64_t start, uint64_t end, bool wanted) {
    uint64_t pos = start;
    while (pos <= end && (pos & 7)) {
        if (((bytes[pos >> 3] >> (7 - (pos & 7))) & 1u) == wanted)
            return static_cast<int64_t>(pos);
        pos++;
    }
    while (pos + 7 <= end) {
        const uint8_t byte = bytes[pos >> 3];
        if ((wanted && byte != 0) || (!wanted && byte != 0xff)) {
            for (uint32_t bit = 0; bit < 8; bit++)
                if (((byte >> (7 - bit)) & 1u) == wanted)
                    return static_cast<int64_t>(pos + bit);
        }
        pos += 8;
    }
    while (pos <= end) {
        if (((bytes[pos >> 3] >> (7 - (pos & 7))) & 1u) == wanted)
            return static_cast<int64_t>(pos);
        pos++;
    }
    return -1;
}

template <bool kNotify>
void cmd_bitpos(Shard& sh, Op& op) {
    int64_t bit = 0;
    if (!parse_i64(op.arg(2), bit)) {
        reply_err(op.sink(), "ERR value is not an integer or out of range");
        return;
    }
    if (bit != 0 && bit != 1) {
        reply_err(op.sink(), "ERR The bit argument must be 1 or 0.");
        return;
    }

    const bool ranged = op.argc() >= 4;
    const bool end_given = op.argc() >= 5;
    bool bit_unit = false;
    int64_t start = 0, end = 0;
    if (ranged) {
        if (!parse_i64(op.arg(3), start)) {
            reply_err(op.sink(), "ERR value is not an integer or out of range");
            return;
        }
        // Redis validates the unit before parsing the end when both are supplied.
        if (op.argc() == 6 && !parse_bitmap_unit(op, op.arg(5), bit_unit)) return;
        if (end_given && !parse_i64(op.arg(4), end)) {
            reply_err(op.sink(), "ERR value is not an integer or out of range");
            return;
        }
    }

    KvObj* o = sh.store_find_read<kNotify>(op.hash, op.key());
    if (!o) { reply_int(op.sink(), bit ? -1 : 0); return; }
    auto sink = op.sink();
    if (!obj_type_check(o, Type::String, sink)) return;
    char integer[24];
    KvObjRawReadBuffer raw;
    const Slice value = string_bytes(o, integer, raw);
    int64_t total = bit_unit ? static_cast<int64_t>(value.n) * 8 : value.n;

    if (!ranged) {
        start = 0;
        end = static_cast<int64_t>(value.n) - 1;
    } else {
        if (!end_given)
            end = bit_unit ? static_cast<int64_t>(value.n) * 8 + 7
                           : static_cast<int64_t>(value.n) - 1;
        if (start < 0) start = total + start;
        if (end < 0) end = total + end;
        if (start < 0) start = 0;
        if (end < 0) end = 0;
        if (end >= total) end = total - 1;
    }
    if (start > end) { reply_int(op.sink(), -1); return; }

    const uint64_t first_bit = bit_unit ? static_cast<uint64_t>(start)
                                        : static_cast<uint64_t>(start) * 8;
    const uint64_t last_bit = bit_unit ? static_cast<uint64_t>(end)
                                       : static_cast<uint64_t>(end) * 8 + 7;
    int64_t position = bitmap_find_bit(reinterpret_cast<const uint8_t*>(value.p),
                                       first_bit, last_bit, bit != 0);
    if (position == -1 && bit == 0 && !end_given)
        position = static_cast<int64_t>(value.n) * 8;
    reply_int(op.sink(), position);
}

template <bool kNotify>
void cmd_getset(Shard& sh, Op& op) {
    KvObj* old = sh.store_find_read<kNotify>(op.hash, op.key());
    auto sink = op.sink();
    if (!obj_type_check(old, Type::String, sink)) return;
    reply_string_bulk(op, old);
    const StoreResult result =
        store_string_for<kNotify>(sh, op.key(), op.hash, op.arg(2), -1, true, false);
    if (result != StoreResult::Stored) { reply_store_error(op, result, true); return; }
    if constexpr (kNotify)
        notify_record(sh, op, NOTIFY_STRING, NotifyEventId::Set, op.key());
}

template <bool kNotify>
void cmd_setnx(Shard& sh, Op& op) {
    if (sh.store_find<kNotify>(op.hash, op.key())) { reply_int(op.sink(), 0); return; }
    const StoreResult result =
        store_string_for<kNotify>(sh, op.key(), op.hash, op.arg(2), -1, true, false);
    if (result != StoreResult::Stored) { reply_store_error(op, result); return; }
    if constexpr (kNotify)
        notify_record(sh, op, NOTIFY_STRING, NotifyEventId::Set, op.key());
    reply_int(op.sink(), 1);
}

template <bool kNotify>
void setex_generic(Shard& sh, Op& op, ExpireKind kind, const char* command) {
    int64_t expire = -1;
    if (!apply_expiry_arg(sh, op, op.arg(2), kind, expire, command)) return;
    const StoreResult result =
        store_string_for<kNotify>(sh, op.key(), op.hash, op.arg(3), expire, true, false);
    if (result != StoreResult::Stored) { reply_store_error(op, result); return; }
    if constexpr (kNotify) {
        notify_record(sh, op, NOTIFY_STRING, NotifyEventId::Set, op.key());
        notify_record(sh, op, NOTIFY_GENERIC, NotifyEventId::Expire, op.key());
    }
    reply_ok(op.sink());
}

template <bool kNotify>
void cmd_setex(Shard& sh, Op& op)  { setex_generic<kNotify>(sh, op, ExpireKind::Ex, "setex"); }
template <bool kNotify>
void cmd_psetex(Shard& sh, Op& op) { setex_generic<kNotify>(sh, op, ExpireKind::Px, "psetex"); }

template <bool kNotify>
void incr_decr(Shard& sh, Op& op, int64_t increment) {
    KvObj* o = sh.store_find<kNotify>(op.hash, op.key());
    int64_t old = 0;
    int64_t expire = -1;
    if (o) {
        auto sink = op.sink();
        if (!obj_type_check(o, Type::String, sink)) return;
        if (o->is_int()) old = o->int_value();
        else {
            KvObjRawReadBuffer raw;
            if (!parse_i64(kvobj_string_value(o, raw), old)) {
                reply_err(op.sink(), "ERR value is not an integer or out of range");
                return;
            }
        }
        expire = sh.store().deadline(op.hash, o);
    }
    int64_t value = 0;
    if (__builtin_add_overflow(old, increment, &value)) {
        reply_err(op.sink(), "ERR increment or decrement would overflow");
        return;
    }
    const StoreResult result = store_integer_for<kNotify>(
        sh, op.key(), op.hash, value, expire, o && o->has_ttl_slot());
    if (result != StoreResult::Stored) { reply_store_error(op, result); return; }
    if constexpr (kNotify)
        notify_record(sh, op, NOTIFY_STRING, NotifyEventId::Incrby, op.key());
    reply_int(op.sink(), value);
}

template <bool kNotify>
void cmd_incr(Shard& sh, Op& op) { incr_decr<kNotify>(sh, op, 1); }
template <bool kNotify>
void cmd_decr(Shard& sh, Op& op) { incr_decr<kNotify>(sh, op, -1); }

template <bool kNotify>
void cmd_incrby(Shard& sh, Op& op) {
    int64_t increment = 0;
    if (!parse_i64(op.arg(2), increment)) {
        reply_err(op.sink(), "ERR value is not an integer or out of range");
        return;
    }
    incr_decr<kNotify>(sh, op, increment);
}

template <bool kNotify>
void cmd_decrby(Shard& sh, Op& op) {
    int64_t decrement = 0;
    if (!parse_i64(op.arg(2), decrement)) {
        reply_err(op.sink(), "ERR value is not an integer or out of range");
        return;
    }
    if (decrement == INT64_MIN) {
        reply_err(op.sink(), "ERR decrement would overflow");
        return;
    }
    incr_decr<kNotify>(sh, op, -decrement);
}

template <bool kNotify>
void cmd_incrbyfloat(Shard& sh, Op& op) {
    KvObj* o = sh.store_find<kNotify>(op.hash, op.key());
    if (o) {
        auto sink = op.sink();
        if (!obj_type_check(o, Type::String, sink)) return;
    }

    long double value = 0;
    if (o) {
        if (o->is_int()) value = static_cast<long double>(o->int_value());
        else {
            KvObjRawReadBuffer raw;
            if (!parse_long_double(kvobj_string_value(o, raw), value)) {
                reply_err(op.sink(), "ERR value is not a valid float");
                return;
            }
        }
    }
    long double increment = 0;
    if (!parse_long_double(op.arg(2), increment)) {
        reply_err(op.sink(), "ERR value is not a valid float");
        return;
    }
    value += increment;
    if (std::isnan(value) || std::isinf(value)) {
        reply_err(op.sink(), "ERR increment would produce NaN or Infinity");
        return;
    }

    char text[kLongDoubleChars];
    const uint32_t length = format_long_double(text, sizeof(text), value);
    if (length == 0) { reply_err(op.sink(), "ERR out of memory"); return; }
    const int64_t expire = o ? sh.store().deadline(op.hash, o) : -1;
    const StoreResult result =
        store_string_for<kNotify>(sh, op.key(), op.hash, Slice(text, length), expire, false,
                                  o && o->has_ttl_slot());
    if (result != StoreResult::Stored) { reply_store_error(op, result); return; }
    if constexpr (kNotify)
        notify_record(sh, op, NOTIFY_STRING, NotifyEventId::Incrbyfloat, op.key());
    reply_bulk(op.sink(), Slice(text, length));
}

void reply_hll_bad_header(Op& op) {
    reply_err(op.sink(), "WRONGTYPE Key is not a valid HyperLogLog string value.");
}

void reply_hll_corrupt(Op& op) {
    reply_err(op.sink(), "INVALIDOBJ Corrupted HLL object detected");
}

bool hll_object_image(KvObj* object, Op& op, Slice& image, KvObjRawReadBuffer& raw) {
    if (static_cast<Type>(object->type) != Type::String) {
        reply_wrongtype(op.sink());
        return false;
    }
    if (object->is_int()) {
        reply_hll_bad_header(op);
        return false;
    }
    image = kvobj_string_value(object, raw);
    if (!hll::header_valid(image)) {
        reply_hll_bad_header(op);
        return false;
    }
    return true;
}

template <bool kNotify>
void cmd_pfadd(Shard& sh, Op& op) {
    KvObj* object = sh.store_find<kNotify>(op.hash, op.key());
    Slice current;
    KvObjRawReadBuffer raw;
    if (object && !hll_object_image(object, op, current, raw)) return;

    std::string image;
    try {
        image = object ? std::string(current.p, current.n) : hll::create_sparse();
    } catch (const std::bad_alloc&) {
        reply_err(op.sink(), "ERR out of memory");
        return;
    }

    bool updated = object == nullptr;  // Redis treats creation itself as an update.
    try {
        for (uint32_t i = 2; i < op.argc(); i++) {
            const int result = hll::add(image, op.arg(i));
            if (result < 0) { reply_hll_corrupt(op); return; }
            updated |= result != 0;
        }
    } catch (const std::bad_alloc&) {
        reply_err(op.sink(), "ERR out of memory");
        return;
    }
    if (!updated) { reply_int(op.sink(), 0); return; }

    hll::invalidate_cache(image);
    const int64_t expire_at_ms = object ? sh.store().deadline(op.hash, object) : -1;
    const StoreResult stored = store_string_for<kNotify>(
        sh, op.key(), op.hash, Slice(image.data(), static_cast<uint32_t>(image.size())),
        expire_at_ms, false, object && object->has_ttl_slot());
    if (stored != StoreResult::Stored) { reply_store_error(op, stored); return; }
    if constexpr (kNotify)
        notify_record(sh, op, NOTIFY_STRING, NotifyEventId::Pfadd, op.key());
    reply_int(op.sink(), 1);
}

template <bool kNotify>
void cmd_pfcount(Shard& sh, Op& op) {
    // Multi-key PFCOUNT is intercepted by SCATTER-V2 before dispatch. Keeping this handler strictly
    // single-owner also makes its cached-cardinality byte update safe without synchronization.
    if (op.argc() != 2) {
        reply_err(op.sink(), "ERR internal cross-shard routing error");
        return;
    }
    KvObj* object = sh.store_find_read<kNotify>(op.hash, op.key());
    if (!object) { reply_int(op.sink(), 0); return; }
    Slice current;
    KvObjRawReadBuffer raw;
    if (!hll_object_image(object, op, current, raw)) return;
    if (hll::cache_valid(current)) {
        reply_int(op.sink(), static_cast<long long>(hll::cached_count(current)));
        return;
    }

    bool corrupt = false;
    const uint64_t cardinality = hll::count(current, corrupt);
    if (corrupt) { reply_hll_corrupt(op); return; }
    std::string image;
    try {
        image.assign(current.p, current.n);
    } catch (const std::bad_alloc&) {
        reply_err(op.sink(), "ERR out of memory");
        return;
    }
    hll::set_cached_count(image, cardinality);
    const StoreResult stored = store_string_for<kNotify>(
        sh, op.key(), op.hash, Slice(image.data(), static_cast<uint32_t>(image.size())),
        sh.store().deadline(op.hash, object), false, object->has_ttl_slot());
    if (stored != StoreResult::Stored) { reply_store_error(op, stored); return; }
    reply_int(op.sink(), static_cast<long long>(cardinality));
}

// EXPIRE's option list is variadic in redis: every NX/XX/GT/LT token is folded into a flag set, so
// repeating one is legal and XX may be combined with GT or LT. Only the two documented
// combinations are refused, and only AFTER the whole list scans clean -- an unknown token is
// reported first, before either compatibility rule and before the deadline is parsed.
struct ExpireConditions {
    bool nx = false;
    bool xx = false;
    bool gt = false;
    bool lt = false;
};

// The rejected token is echoed the way redis echoes it: as a C string (so it stops at the first
// NUL) with CR and LF folded to spaces, which is what keeps a hostile option out of the reply
// framing.
void reply_unsupported_expire_option(Op& op, Slice option) {
    std::string message = "ERR Unsupported option ";
    for (uint32_t i = 0; i < option.n; i++) {
        const char ch = option.p[i];
        if (ch == '\0') break;
        message.push_back(ch == '\r' || ch == '\n' ? ' ' : ch);
    }
    reply_err(op.sink(), message.c_str());
}

bool parse_expire_conditions(Op& op, ExpireConditions& conditions) {
    for (uint32_t i = 3; i < op.argc(); i++) {
        if (eq_icase(op.arg(i), "NX")) conditions.nx = true;
        else if (eq_icase(op.arg(i), "XX")) conditions.xx = true;
        else if (eq_icase(op.arg(i), "GT")) conditions.gt = true;
        else if (eq_icase(op.arg(i), "LT")) conditions.lt = true;
        else { reply_unsupported_expire_option(op, op.arg(i)); return false; }
    }
    if (conditions.nx && (conditions.xx || conditions.gt || conditions.lt)) {
        reply_err(op.sink(), "ERR NX and XX, GT or LT options at the same time are not compatible");
        return false;
    }
    if (conditions.gt && conditions.lt) {
        reply_err(op.sink(), "ERR GT and LT options at the same time are not compatible");
        return false;
    }
    return true;
}

template <bool kNotify>
void expire_generic(Shard& sh, Op& op, bool absolute, bool seconds, const char* name) {
    ExpireConditions conditions;
    if (!parse_expire_conditions(op, conditions)) return;
    int64_t when = 0;
    if (!parse_i64(op.arg(2), when)) { reply_not_integer(op); return; }
    if (seconds) {
        if (when > INT64_MAX / 1000 || when < INT64_MIN / 1000) {
            reply_invalid_expire(op, name); return;
        }
        when *= 1000;
    }
    if (!absolute) {
        if (when > INT64_MAX - sh.now_ms()) { reply_invalid_expire(op, name); return; }
        when += sh.now_ms();
    }

    KvObj* o = sh.store_find<kNotify>(op.hash, op.key());
    if (!o) { reply_int(op.sink(), 0); return; }
    const int64_t current = sh.store().deadline(op.hash, o);
    if ((conditions.nx && current >= 0) ||
        (conditions.xx && current < 0) ||
        (conditions.gt && (current < 0 || when <= current)) ||
        (conditions.lt && current >= 0 && when >= current)) {
        reply_int(op.sink(), 0);
        return;
    }
    if (when <= sh.now_ms()) {
        sh.store_erase<kNotify>(op.hash, op.key());
        reply_int(op.sink(), 1);
        return;
    }
    const FlatStore::TtlResult result = sh.store_set_expire<kNotify>(op.hash, op.key(), when);
    if (result == FlatStore::TtlResult::Oom || result == FlatStore::TtlResult::MaxmemoryOom) {
        if (result == FlatStore::TtlResult::MaxmemoryOom) reply_maxmemory_oom(op);
        else reply_err(op.sink(), "ERR out of memory");
        return;
    }
    if constexpr (kNotify) if (result == FlatStore::TtlResult::Updated)
        notify_record(sh, op, NOTIFY_GENERIC, NotifyEventId::Expire, op.key());
    reply_int(op.sink(), result == FlatStore::TtlResult::Updated ? 1 : 0);
}

template <bool kNotify>
void cmd_expire(Shard& sh, Op& op)    { expire_generic<kNotify>(sh, op, false, true,  "expire"); }
template <bool kNotify>
void cmd_pexpire(Shard& sh, Op& op)   { expire_generic<kNotify>(sh, op, false, false, "pexpire"); }
template <bool kNotify>
void cmd_expireat(Shard& sh, Op& op)  { expire_generic<kNotify>(sh, op, true,  true,  "expireat"); }
template <bool kNotify>
void cmd_pexpireat(Shard& sh, Op& op) { expire_generic<kNotify>(sh, op, true,  false, "pexpireat"); }

int64_t rounded_seconds(int64_t ms) {
    return ms / 1000 + ((ms % 1000) >= 500 ? 1 : 0);
}

template <bool kNotify>
void ttl_generic(Shard& sh, Op& op, bool milliseconds, bool absolute) {
    KvObj* o = sh.store_find_read<kNotify>(op.hash, op.key());
    if (!o) { reply_int(op.sink(), -2); return; }
    const int64_t expire = sh.store().deadline(op.hash, o);
    if (expire < 0) { reply_int(op.sink(), -1); return; }
    int64_t value = absolute ? expire : expire - sh.now_ms();
    if (value < 0) value = 0;
    reply_int(op.sink(), milliseconds ? value : rounded_seconds(value));
}

template <bool kNotify>
void cmd_ttl(Shard& sh, Op& op)         { ttl_generic<kNotify>(sh, op, false, false); }
template <bool kNotify>
void cmd_pttl(Shard& sh, Op& op)        { ttl_generic<kNotify>(sh, op, true,  false); }
template <bool kNotify>
void cmd_expiretime(Shard& sh, Op& op)  { ttl_generic<kNotify>(sh, op, false, true); }
template <bool kNotify>
void cmd_pexpiretime(Shard& sh, Op& op) { ttl_generic<kNotify>(sh, op, true,  true); }

template <bool kNotify>
void cmd_persist(Shard& sh, Op& op) {
    const FlatStore::TtlResult result = sh.store_persist<kNotify>(op.hash, op.key());
    if (result == FlatStore::TtlResult::Oom || result == FlatStore::TtlResult::MaxmemoryOom) {
        if (result == FlatStore::TtlResult::MaxmemoryOom) reply_maxmemory_oom(op);
        else reply_err(op.sink(), "ERR out of memory");
        return;
    }
    if constexpr (kNotify) if (result == FlatStore::TtlResult::Updated)
        notify_record(sh, op, NOTIFY_GENERIC, NotifyEventId::Persist, op.key());
    reply_int(op.sink(), result == FlatStore::TtlResult::Updated ? 1 : 0);
}

const char* type_name(const KvObj* o) {
    if (!o) return "none";
    switch (static_cast<Type>(o->type)) {
        case Type::String: return "string";
        case Type::Hash:   return "hash";
        case Type::List:   return "list";
        case Type::Set:    return "set";
        case Type::Zset:   return "zset";
        case Type::Stream: return "stream";
    }
    return "none";
}

template <bool kNotify>
void cmd_type(Shard& sh, Op& op) {
    reply_simple(op.sink(), type_name(sh.store_find_read<kNotify>(op.hash, op.key())));
}


#ifndef TOMO_STRING_NOTIFY_TU
#define TOMO_HANDLER_PAIR(fn, first, last, step) \
    fn<false>, first, last, step, fn##_notify

static const CommandSpec kTable[] = {
    // name          min max flags                                  handler          first last step
    {"GET",           2,  2,  CmdFlags::Readonly | CmdFlags::ReadLocalEligible,
                                                                    TOMO_HANDLER_PAIR(cmd_get, 1, 1, 1)},
    {"SET",           3, -1,  CmdFlags::Write | CmdFlags::DenyOom,  TOMO_HANDLER_PAIR(cmd_set, 1, 1, 1)},
    {"APPEND",        3,  3,  CmdFlags::Write | CmdFlags::DenyOom,  TOMO_HANDLER_PAIR(cmd_append, 1, 1, 1)},
    {"STRLEN",        2,  2,  CmdFlags::Readonly,                    TOMO_HANDLER_PAIR(cmd_strlen, 1, 1, 1)},
    {"GETRANGE",      4,  4,  CmdFlags::Readonly,                    TOMO_HANDLER_PAIR(cmd_getrange, 1, 1, 1)},
    {"SUBSTR",        4,  4,  CmdFlags::Readonly,                    TOMO_HANDLER_PAIR(cmd_getrange, 1, 1, 1)},
    {"SETRANGE",      4,  4,  CmdFlags::Write | CmdFlags::DenyOom,  TOMO_HANDLER_PAIR(cmd_setrange, 1, 1, 1)},
    {"SETBIT",        4,  4,  CmdFlags::Write | CmdFlags::DenyOom,  TOMO_HANDLER_PAIR(cmd_setbit, 1, 1, 1)},
    {"GETBIT",        3,  3,  CmdFlags::Readonly,                    TOMO_HANDLER_PAIR(cmd_getbit, 1, 1, 1)},
    {"BITFIELD",      2, -1,  CmdFlags::Write | CmdFlags::DenyOom,
                                             cmd_bitfield, 1, 1, 1, cmd_bitfield_notify},
    {"BITFIELD_RO",   2, -1,  CmdFlags::Readonly,
                                          cmd_bitfield_ro, 1, 1, 1, cmd_bitfield_ro_notify},
    {"BITCOUNT",      2, -1,  CmdFlags::Readonly,                    TOMO_HANDLER_PAIR(cmd_bitcount, 1, 1, 1)},
    {"BITPOS",        3,  6,  CmdFlags::Readonly,                    TOMO_HANDLER_PAIR(cmd_bitpos, 1, 1, 1)},
    {"BITOP",         4, -1,  CmdFlags::Write | CmdFlags::DenyOom | CmdFlags::MultiShard,
                                                                               cmd_xshard_only, 2, -1, 1},
    {"GETSET",        3,  3,  CmdFlags::Write | CmdFlags::DenyOom,  TOMO_HANDLER_PAIR(cmd_getset, 1, 1, 1)},
    {"SETNX",         3,  3,  CmdFlags::Write | CmdFlags::DenyOom,  TOMO_HANDLER_PAIR(cmd_setnx, 1, 1, 1)},
    {"SETEX",         4,  4,  CmdFlags::Write | CmdFlags::DenyOom,  TOMO_HANDLER_PAIR(cmd_setex, 1, 1, 1)},
    {"PSETEX",        4,  4,  CmdFlags::Write | CmdFlags::DenyOom,  TOMO_HANDLER_PAIR(cmd_psetex, 1, 1, 1)},
    {"GETEX",         2, -1,  CmdFlags::Write,                       TOMO_HANDLER_PAIR(cmd_getex, 1, 1, 1)},
    {"GETDEL",        2,  2,  CmdFlags::Write,                       TOMO_HANDLER_PAIR(cmd_getdel, 1, 1, 1)},
    {"DEL",           2, -1,  CmdFlags::Write | CmdFlags::MultiShard,TOMO_HANDLER_PAIR(cmd_del, 1, -1, 1)},
    {"UNLINK",        2, -1,  CmdFlags::Write | CmdFlags::MultiShard,TOMO_HANDLER_PAIR(cmd_del, 1, -1, 1)},
    {"EXISTS",        2, -1,  CmdFlags::Readonly | CmdFlags::MultiShard,TOMO_HANDLER_PAIR(cmd_exists, 1, -1, 1)},
    {"TOUCH",         2, -1,  CmdFlags::Readonly | CmdFlags::MultiShard,TOMO_HANDLER_PAIR(cmd_exists, 1, -1, 1)},
    {"MGET",          2, -1,  CmdFlags::Readonly | CmdFlags::MultiShard |
                                      CmdFlags::ReadLocalEligible,       cmd_xshard_only,1,-1, 1},
    {"MSET",          3, -1,  CmdFlags::Write | CmdFlags::MultiShard,cmd_xshard_only,  1, -1,  2},
    {"MSETNX",        3, -1,  CmdFlags::Write | CmdFlags::MultiShard,cmd_xshard_only,  1, -1,  2},
    {"RENAME",        3,  3,  CmdFlags::Write | CmdFlags::MultiShard,cmd_xshard_only,  1,  2,  1},
    {"RENAMENX",      3,  3,  CmdFlags::Write | CmdFlags::MultiShard,cmd_xshard_only,  1,  2,  1},
    {"COPY",          3, -1,  CmdFlags::Write | CmdFlags::MultiShard,cmd_xshard_only,  1,  2,  1},
    {"INCR",          2,  2,  CmdFlags::Write | CmdFlags::DenyOom,  TOMO_HANDLER_PAIR(cmd_incr, 1, 1, 1)},
    {"DECR",          2,  2,  CmdFlags::Write | CmdFlags::DenyOom,  TOMO_HANDLER_PAIR(cmd_decr, 1, 1, 1)},
    {"INCRBY",        3,  3,  CmdFlags::Write | CmdFlags::DenyOom,  TOMO_HANDLER_PAIR(cmd_incrby, 1, 1, 1)},
    {"DECRBY",        3,  3,  CmdFlags::Write | CmdFlags::DenyOom,  TOMO_HANDLER_PAIR(cmd_decrby, 1, 1, 1)},
    {"INCRBYFLOAT",   3,  3,  CmdFlags::Write | CmdFlags::DenyOom,  TOMO_HANDLER_PAIR(cmd_incrbyfloat, 1, 1, 1)},
    // Minimum 2, not 3: redis accepts "PFADD key" with no elements and creates the HLL.
    {"PFADD",         2, -1,  CmdFlags::Write | CmdFlags::DenyOom,  TOMO_HANDLER_PAIR(cmd_pfadd, 1, 1, 1)},
    {"PFCOUNT",       2, -1,  CmdFlags::Readonly | CmdFlags::SnapshotWrite | CmdFlags::MultiShard,TOMO_HANDLER_PAIR(cmd_pfcount,1,-1,1)},
    {"PFMERGE",       2, -1,  CmdFlags::Write | CmdFlags::DenyOom | CmdFlags::MultiShard, cmd_xshard_only, 1, -1,  1},
    {"EXPIRE",        3, -1,  CmdFlags::Write,                       TOMO_HANDLER_PAIR(cmd_expire, 1, 1, 1)},
    {"PEXPIRE",       3, -1,  CmdFlags::Write,                       TOMO_HANDLER_PAIR(cmd_pexpire, 1, 1, 1)},
    {"EXPIREAT",      3, -1,  CmdFlags::Write,                       TOMO_HANDLER_PAIR(cmd_expireat, 1, 1, 1)},
    {"PEXPIREAT",     3, -1,  CmdFlags::Write,                       TOMO_HANDLER_PAIR(cmd_pexpireat, 1, 1, 1)},
    {"TTL",           2,  2,  CmdFlags::Readonly,                    TOMO_HANDLER_PAIR(cmd_ttl, 1, 1, 1)},
    {"PTTL",          2,  2,  CmdFlags::Readonly,                    TOMO_HANDLER_PAIR(cmd_pttl, 1, 1, 1)},
    {"PERSIST",       2,  2,  CmdFlags::Write,                       TOMO_HANDLER_PAIR(cmd_persist, 1, 1, 1)},
    {"EXPIRETIME",    2,  2,  CmdFlags::Readonly,                    TOMO_HANDLER_PAIR(cmd_expiretime, 1, 1, 1)},
    {"PEXPIRETIME",   2,  2,  CmdFlags::Readonly,                    TOMO_HANDLER_PAIR(cmd_pexpiretime, 1, 1, 1)},
    {"TYPE",          2,  2,  CmdFlags::Readonly,                    TOMO_HANDLER_PAIR(cmd_type, 1, 1, 1)},
    {"DUMP",          2,  2,  CmdFlags::Readonly,
                                                  cmd_dump, 1, 1, 1, cmd_dump_notify},
    {"RESTORE",       4, -1,  CmdFlags::Write | CmdFlags::DenyOom,
                                            cmd_restore, 1, 1, 1, cmd_restore_notify},
};

#undef TOMO_HANDLER_PAIR
#endif

}  // namespace

#ifdef TOMO_STRING_NOTIFY_TU
void cmd_get_tls_notify(Shard& shard, Op& op) {
    notify_execute_handler(shard, op, cmd_get<true, false>);
}
#define TOMO_DEFINE_STRING_NOTIFY(fn) \
    void fn##_notify(Shard& shard, Op& op) { notify_execute_handler(shard, op, fn<true>); }
TOMO_STRING_NOTIFY_HANDLERS(TOMO_DEFINE_STRING_NOTIFY)
#undef TOMO_DEFINE_STRING_NOTIFY
#else
void cmd_get_tls(Shard& shard, Op& op) { cmd_get<false, false>(shard, op); }
namespace {

SnapshotHookStatus string_snapshot_begin(const KvObj& object, SnapshotSaveCursor& cursor,
                                         uint8_t& encoding) {
    if (static_cast<Type>(object.type) != Type::String) return SnapshotHookStatus::Corrupt;
    cursor = {};
    cursor.object = &object;
    encoding = object.enc;
    cursor.total = object.is_int() ? sizeof(int64_t) : kvobj_string_length(&object);
    return SnapshotHookStatus::Ok;
}

SnapshotHookStatus string_snapshot_read(SnapshotSaveCursor& cursor, uint8_t* destination,
                                        size_t capacity, size_t& written) {
    written = 0;
    if (!cursor.object || cursor.offset > cursor.total) return SnapshotHookStatus::Corrupt;
    const size_t take = static_cast<size_t>(
        std::min<uint64_t>(capacity, cursor.total - cursor.offset));
    if (!take) return SnapshotHookStatus::Ok;
    if (cursor.object->is_int()) {
        uint8_t bytes[8];
        snapshot_put_u64(bytes, static_cast<uint64_t>(cursor.object->int_value()));
        std::memcpy(destination, bytes + cursor.offset, take);
    } else {
        KvObjRawReadBuffer raw;
        const Slice value = kvobj_string_value(cursor.object, raw);
        std::memcpy(destination, value.p + cursor.offset, take);
    }
    cursor.offset += take;
    written = take;
    return SnapshotHookStatus::Ok;
}

SnapshotHookStatus string_snapshot_load(Slice key, uint8_t encoding, int64_t expire_at_ms,
                                        Slice payload, const TypeLimits&, KvObj*& result) {
    result = nullptr;
    const Enc enc = static_cast<Enc>(encoding);
    if (enc == Enc::Int) {
        if (payload.n != sizeof(int64_t)) return SnapshotHookStatus::Corrupt;
        result = kvobj_new_int(key, static_cast<int64_t>(snapshot_get_u64(
                                    reinterpret_cast<const uint8_t*>(payload.p))), expire_at_ms);
    } else if (enc == Enc::Raw || enc == Enc::Extern) {
        result = kvobj_new_string(key, payload, expire_at_ms);
    } else {
        return SnapshotHookStatus::Corrupt;
    }
    return result ? SnapshotHookStatus::Ok : SnapshotHookStatus::Oom;
}

}  // namespace

SnapshotTypeHooks string_snapshot_hooks() {
    return {string_snapshot_begin, string_snapshot_read, string_snapshot_load};
}

CommandTable string_command_table() {
    return {kTable, sizeof(kTable) / sizeof(kTable[0])};
}
#endif

#undef TOMO_STRING_NOTIFY_HANDLERS

}  // namespace tomo

// notify.h -- keyspace-notification integration at the executor/IO retirement seam.
//
// The implementation lives in notify.inc and is textually included by xshard.cc. The command
// registry carries clean and armed handler pointers; IO selects once per operation, so disabled
// handlers contain no notification tests or context gates. Channel construction, batching, and
// publication remain out of line.
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>

#include "../base/slice.h"

namespace tomo {

class Client;
class IoLoop;
class Op;
class FlatStore;
class Server;
class Shard;
class ThreadCtx;
class Ring;
class LoopSignals;
struct ScatterState;

inline constexpr uint32_t NOTIFY_KEYSPACE = 1u << 0;
inline constexpr uint32_t NOTIFY_KEYEVENT = 1u << 1;
inline constexpr uint32_t NOTIFY_GENERIC = 1u << 2;
inline constexpr uint32_t NOTIFY_STRING = 1u << 3;
inline constexpr uint32_t NOTIFY_LIST = 1u << 4;
inline constexpr uint32_t NOTIFY_SET = 1u << 5;
inline constexpr uint32_t NOTIFY_HASH = 1u << 6;
inline constexpr uint32_t NOTIFY_ZSET = 1u << 7;
inline constexpr uint32_t NOTIFY_EXPIRED = 1u << 8;
inline constexpr uint32_t NOTIFY_EVICTED = 1u << 9;
inline constexpr uint32_t NOTIFY_STREAM = 1u << 10;
inline constexpr uint32_t NOTIFY_KEY_MISS = 1u << 11;
// 1<<12 is NOTIFY_LOADED in Redis and deliberately has no flag character.
inline constexpr uint32_t NOTIFY_MODULE = 1u << 13;
inline constexpr uint32_t NOTIFY_NEW = 1u << 14;
inline constexpr uint32_t NOTIFY_ALL = 0x000027fcu;
// Lane F synthetic class. It is NEVER produced by parse_notify_flags and never serialized, so
// CONFIG GET notify-keyspace-events keeps reporting exactly what the operator configured. Its
// only job is to let CLIENT TRACKING arm the same executor-side key observer without adding a
// second armed load to notify_record.
inline constexpr uint32_t NOTIFY_TRACKING = 1u << 20;
// Periodic SAVE reuses the exact successful-mutation fire points of the armed handlers. It never
// becomes a pub/sub route and never allocates a NotifyBatch; it only increments the owner-local
// shard counter before the ordinary route gate below.
inline constexpr uint32_t NOTIFY_SAVE = 1u << 21;
inline constexpr uint32_t NOTIFY_ROUTES = NOTIFY_KEYSPACE | NOTIFY_KEYEVENT | NOTIFY_TRACKING;
inline constexpr uint32_t NOTIFY_OBSERVERS = NOTIFY_TRACKING | NOTIFY_SAVE;

// Synthetic observers need every ordinary mutation class, but those classes must not become
// operator-visible pub/sub classes. Derive the routes for this event separately: K/E require the
// configured class itself, while tracking observes the same A-class mutation surface as before.
inline uint32_t notify_routes_for_class(uint32_t mask, uint32_t cls) {
    uint32_t routes = (mask & cls) ? mask & (NOTIFY_KEYSPACE | NOTIFY_KEYEVENT) : 0u;
    if ((mask & NOTIFY_TRACKING) && (NOTIFY_ALL & cls)) routes |= NOTIFY_TRACKING;
    return routes;
}

inline bool parse_notify_flags(Slice input, uint32_t& flags) {
    uint32_t parsed = 0;
    for (uint32_t i = 0; i < input.n; i++) {
        switch (input.p[i]) {
            case 'K': parsed |= NOTIFY_KEYSPACE; break;
            case 'E': parsed |= NOTIFY_KEYEVENT; break;
            case 'g': parsed |= NOTIFY_GENERIC; break;
            case '$': parsed |= NOTIFY_STRING; break;
            case 'l': parsed |= NOTIFY_LIST; break;
            case 's': parsed |= NOTIFY_SET; break;
            case 'h': parsed |= NOTIFY_HASH; break;
            case 'z': parsed |= NOTIFY_ZSET; break;
            case 'x': parsed |= NOTIFY_EXPIRED; break;
            case 'e': parsed |= NOTIFY_EVICTED; break;
            case 't': parsed |= NOTIFY_STREAM; break;
            case 'm': parsed |= NOTIFY_KEY_MISS; break;
            case 'd': parsed |= NOTIFY_MODULE; break;
            case 'n': parsed |= NOTIFY_NEW; break;
            case 'A': parsed |= NOTIFY_ALL; break;
            default: return false;
        }
    }
    flags = parsed;
    return true;
}

inline std::string serialize_notify_flags(uint32_t flags) {
    std::string out;
    if ((flags & NOTIFY_ALL) == NOTIFY_ALL) {
        out.push_back('A');
    } else {
        static constexpr struct { uint32_t bit; char flag; } classes[] = {
            {NOTIFY_GENERIC, 'g'}, {NOTIFY_STRING, '$'}, {NOTIFY_LIST, 'l'},
            {NOTIFY_SET, 's'}, {NOTIFY_HASH, 'h'}, {NOTIFY_ZSET, 'z'},
            {NOTIFY_EXPIRED, 'x'}, {NOTIFY_EVICTED, 'e'}, {NOTIFY_STREAM, 't'},
            {NOTIFY_MODULE, 'd'},
        };
        for (const auto& entry : classes) if (flags & entry.bit) out.push_back(entry.flag);
    }
    if (flags & NOTIFY_KEYSPACE) out.push_back('K');
    if (flags & NOTIFY_KEYEVENT) out.push_back('E');
    if (flags & NOTIFY_KEY_MISS) out.push_back('m');
    if (flags & NOTIFY_NEW) out.push_back('n');
    return out;
}

enum class NotifyEventId : uint8_t {
    Set, Expire, Del, Persist, RenameFrom, RenameTo, CopyTo, Restore,
    Setrange, Append, Incrby, Incrbyfloat, Setbit, Pfadd,
    Lpush, Rpush, Lpop, Rpop, Linsert, Lset, Lrem, Ltrim, Sortstore,
    Sadd, Srem, Spop, Sinterstore, Sunionstore, Sdiffstore,
    Hset, Hincrby, Hincrbyfloat, Hdel, Hexpire, Hpersist, Hexpired,
    Zadd, Zincr, Zrem, Zremrangebyrank, Zremrangebyscore, Zremrangebylex,
    Zpopmin, Zpopmax, Zrangestore, Zunionstore, Zinterstore, Zdiffstore,
    Geosearchstore, Georadiusstore,
    Xadd, Xdel, Xtrim,
    Expired, Evicted, Keymiss, New,
};

struct FlatNotifySink {
    void* context = nullptr;
    bool (*enabled)(void*, uint32_t) = nullptr;
    void (*emit)(void*, uint32_t, NotifyEventId, Slice) = nullptr;
};

struct NotifyRecord;
struct NotifyBatch;

struct NotifyOut {
    uint32_t routes = 0;
    NotifyEventId event = NotifyEventId::Expired;
    std::string key;
};

struct NotifyShardState {
    std::deque<NotifyOut> keyless;
};

class NotifyExecutionScope {
public:
    // Pointer form: an internal fragment with no client carries no Op at all (the connection-close
    // watch release posts exactly that). Such a fragment is always disarmed, and a disarmed scope
    // never reads its Op, so it must not be forced to invent a reference.
    NotifyExecutionScope(Shard& shard, Op* op, bool active, uint32_t order_base = 0);
    NotifyExecutionScope(Shard& shard, Op& op, bool active, uint32_t order_base = 0)
        : NotifyExecutionScope(shard, &op, active, order_base) {}
    ~NotifyExecutionScope();
    NotifyExecutionScope(const NotifyExecutionScope&) = delete;
    NotifyExecutionScope& operator=(const NotifyExecutionScope&) = delete;
private:
    Shard* shard_ = nullptr;
    Op* carrier_ = nullptr;
    Op* source_ = nullptr;
    uint32_t order_base_ = 0;
};

// EX side.  The current carrier/source context is installed only while notifications are live.
void notify_execute_enter(Shard& shard, Op& carrier, Op& source, uint32_t order_base = 0);
void notify_execute_source(Shard& shard, Op& source, uint32_t order_base = 0);
void notify_execute_leave(Shard& shard);
void notify_execute_handler(Shard& shard, Op& op, void (*handler)(Shard&, Op&));
bool notify_record_slow(Shard& shard, Op& source, uint32_t mask, uint32_t cls,
                        NotifyEventId event, Slice key);
bool notify_record_keyless_slow(Shard& shard, uint32_t mask, uint32_t cls,
                                NotifyEventId event, Slice key);

// Dependent templates let this narrow header keep Shard forward-declared while still inlining the
// sole feature-off gate at every candidate fire point. Subscriber/batch machinery is out of line.
template <typename ShardLike>
inline bool notify_record(ShardLike& shard, Op& source, uint32_t cls,
                          NotifyEventId event, Slice key) {
    const uint32_t mask = shard.notify_mask();
    const bool observed = (mask & NOTIFY_OBSERVERS) && (NOTIFY_ALL & cls);
    if (__builtin_expect((mask & cls) == 0 && !observed, true)) return false;
    const bool save_observed = observed && (mask & NOTIFY_SAVE);
    if (save_observed) shard.note_save_change();
    const uint32_t routes = notify_routes_for_class(mask, cls);
    if (!routes) return save_observed;
    return notify_record_slow(shard, source, routes, cls, event, key);
}

template <typename ShardLike>
inline bool notify_record_keyless(ShardLike& shard, uint32_t cls,
                                  NotifyEventId event, Slice key) {
    const uint32_t mask = shard.notify_mask();
    const bool observed = (mask & NOTIFY_OBSERVERS) && (NOTIFY_ALL & cls);
    if (__builtin_expect((mask & cls) == 0 && !observed, true)) return false;
    const bool save_observed = observed && (mask & NOTIFY_SAVE);
    if (save_observed) shard.note_save_change();
    const uint32_t routes = notify_routes_for_class(mask, cls);
    if (!routes) return save_observed;
    return notify_record_keyless_slow(shard, routes, cls, event, key);
}
bool notify_flat_enabled(void* shard, uint32_t cls);
void notify_flat_emit(void* shard, uint32_t cls, NotifyEventId event, Slice key);
void notify_bind_flat_store(const FlatStore* store, FlatNotifySink* sink);
void notify_flat_store_emit(const FlatStore* store, uint32_t cls,
                            NotifyEventId event, Slice key);

// The registry points at this wrapper only in its armed shadow row. Context setup, mask reads, and
// recording therefore have no representation in the clean handler specialization or ExLoop.
template <auto Handler>
void notify_handler(Shard& shard, Op& op) {
    notify_execute_handler(shard, op, Handler);
}

// Lane B: one bounded executor pass, using a non-blocking marker post to the nearest/home IO.
uint32_t notify_ex_pass_entry(Server& server, Shard& shard, uint32_t producer,
                              ThreadCtx& thread, Ring& ring, LoopSignals& signals);

// IO retirement.  Special states surrender their batch before their existing destructor runs.
NotifyBatch* notify_take_batch(Op& op);
void notify_retire_batch_entry(IoLoop& loop, NotifyBatch* batch, uint64_t writer_id = 0);
void notify_discard_batch(NotifyBatch* batch);
void notify_abort_op(Op& op);
void notify_xshard_finished(Shard& shard, Op& op, ScatterState& state);

}  // namespace tomo

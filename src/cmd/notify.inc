// notify.inc -- out-of-line keyspace-notification recording and publication.
// Textually included by xshard.cc after the scatter, blocking, and MULTI state definitions.

namespace tomo {

struct NotifyRecordImpl {
    uint32_t order = 0;
    uint32_t sequence = 0;
    // Widened from uint8_t for NOTIFY_TRACKING (bit 20). The record is heap batch state, not Op
    // storage; no footprint lock covers it.
    uint32_t routes = 0;
    NotifyEventId event = NotifyEventId::Set;
    Slice key;
};

namespace {

constexpr uint32_t kNotifyBatchCap = 1024;
constexpr uint32_t kNotifyKeylessCap = 4096;
constexpr uint64_t kNotifyInflightCap = 65536;
constexpr uint32_t kNotifyKeylessPass = 32;

struct FlatStoreBinding {
    const FlatStore* store = nullptr;
    FlatNotifySink* sink = nullptr;
};

// Built before worker threads start, then immutable. The indirection is reached only after cron or
// eviction has already selected a real victim; keeping it out of FlatStore preserves both its size
// and every enclosing Shard offset.
std::vector<FlatStoreBinding> g_notify_flat_stores;

const char* notify_event_name(NotifyEventId event) {
    switch (event) {
        case NotifyEventId::Set: return "set";
        case NotifyEventId::Expire: return "expire";
        case NotifyEventId::Del: return "del";
        case NotifyEventId::Persist: return "persist";
        case NotifyEventId::RenameFrom: return "rename_from";
        case NotifyEventId::RenameTo: return "rename_to";
        case NotifyEventId::CopyTo: return "copy_to";
        case NotifyEventId::Restore: return "restore";
        case NotifyEventId::Setrange: return "setrange";
        case NotifyEventId::Append: return "append";
        case NotifyEventId::Incrby: return "incrby";
        case NotifyEventId::Incrbyfloat: return "incrbyfloat";
        case NotifyEventId::Setbit: return "setbit";
        case NotifyEventId::Pfadd: return "pfadd";
        case NotifyEventId::Lpush: return "lpush";
        case NotifyEventId::Rpush: return "rpush";
        case NotifyEventId::Lpop: return "lpop";
        case NotifyEventId::Rpop: return "rpop";
        case NotifyEventId::Linsert: return "linsert";
        case NotifyEventId::Lset: return "lset";
        case NotifyEventId::Lrem: return "lrem";
        case NotifyEventId::Ltrim: return "ltrim";
        case NotifyEventId::Sortstore: return "sortstore";
        case NotifyEventId::Sadd: return "sadd";
        case NotifyEventId::Srem: return "srem";
        case NotifyEventId::Spop: return "spop";
        case NotifyEventId::Sinterstore: return "sinterstore";
        case NotifyEventId::Sunionstore: return "sunionstore";
        case NotifyEventId::Sdiffstore: return "sdiffstore";
        case NotifyEventId::Hset: return "hset";
        case NotifyEventId::Hincrby: return "hincrby";
        case NotifyEventId::Hincrbyfloat: return "hincrbyfloat";
        case NotifyEventId::Hdel: return "hdel";
        case NotifyEventId::Hexpire: return "hexpire";
        case NotifyEventId::Hpersist: return "hpersist";
        case NotifyEventId::Hexpired: return "hexpired";
        case NotifyEventId::Zadd: return "zadd";
        case NotifyEventId::Zincr: return "zincr";
        case NotifyEventId::Zrem: return "zrem";
        case NotifyEventId::Zremrangebyrank: return "zremrangebyrank";
        case NotifyEventId::Zremrangebyscore: return "zremrangebyscore";
        case NotifyEventId::Zremrangebylex: return "zremrangebylex";
        case NotifyEventId::Zpopmin: return "zpopmin";
        case NotifyEventId::Zpopmax: return "zpopmax";
        case NotifyEventId::Zrangestore: return "zrangestore";
        case NotifyEventId::Zunionstore: return "zunionstore";
        case NotifyEventId::Zinterstore: return "zinterstore";
        case NotifyEventId::Zdiffstore: return "zdiffstore";
        case NotifyEventId::Geosearchstore: return "geosearchstore";
        case NotifyEventId::Georadiusstore: return "georadiusstore";
        case NotifyEventId::Xadd: return "xadd";
        case NotifyEventId::Xdel: return "xdel";
        case NotifyEventId::Xtrim: return "xtrim";
        case NotifyEventId::Expired: return "expired";
        case NotifyEventId::Evicted: return "evicted";
        case NotifyEventId::Keymiss: return "keymiss";
        case NotifyEventId::New: return "new";
    }
    return "";
}

uint32_t notify_event_priority(NotifyEventId event) {
    switch (event) {
        case NotifyEventId::New: return 0;
        case NotifyEventId::Set:
        case NotifyEventId::Setrange:
        case NotifyEventId::Append:
        case NotifyEventId::Incrby:
        case NotifyEventId::Incrbyfloat:
        case NotifyEventId::Setbit:
        case NotifyEventId::Pfadd:
        case NotifyEventId::Lpush:
        case NotifyEventId::Rpush:
        case NotifyEventId::Lpop:
        case NotifyEventId::Rpop:
        case NotifyEventId::Linsert:
        case NotifyEventId::Lset:
        case NotifyEventId::Lrem:
        case NotifyEventId::Ltrim:
        case NotifyEventId::Sortstore:
        case NotifyEventId::Sadd:
        case NotifyEventId::Srem:
        case NotifyEventId::Spop:
        case NotifyEventId::Sinterstore:
        case NotifyEventId::Sunionstore:
        case NotifyEventId::Sdiffstore:
        case NotifyEventId::Hset:
        case NotifyEventId::Hincrby:
        case NotifyEventId::Hincrbyfloat:
        case NotifyEventId::Hdel:
        case NotifyEventId::Zadd:
        case NotifyEventId::Zincr:
        case NotifyEventId::Zrem:
        case NotifyEventId::Zremrangebyrank:
        case NotifyEventId::Zremrangebyscore:
        case NotifyEventId::Zremrangebylex:
        case NotifyEventId::Zpopmin:
        case NotifyEventId::Zpopmax:
        case NotifyEventId::Zrangestore:
        case NotifyEventId::Zunionstore:
        case NotifyEventId::Zinterstore:
        case NotifyEventId::Zdiffstore:
        case NotifyEventId::Geosearchstore:
        case NotifyEventId::Georadiusstore:
        case NotifyEventId::Xadd:
        case NotifyEventId::Xdel:
        case NotifyEventId::Xtrim:
        case NotifyEventId::RenameFrom:
        case NotifyEventId::RenameTo:
        case NotifyEventId::CopyTo:
        case NotifyEventId::Restore: return 4;
        case NotifyEventId::Hexpired: return 3;   // strictly before the del it may trigger
        case NotifyEventId::Expire:
        case NotifyEventId::Persist:
        case NotifyEventId::Hexpire:
        case NotifyEventId::Hpersist: return 6;
        case NotifyEventId::Del: return 8;
        case NotifyEventId::Expired:
        case NotifyEventId::Evicted:
        case NotifyEventId::Keymiss: return 4;
    }
    return 4;
}

std::atomic<NotifyBatch*>* notify_special_slot(Op& carrier) {
    if (carrier.has_scatter_state()) return &carrier.scatter_state()->notify;
    if (carrier.has_blocking_state()) return &carrier.blocking_state()->notify;
    if (carrier.has_multi_state())
        return &static_cast<MultiExecState*>(carrier.multi_state())->notify;
    return nullptr;
}

NotifyBatch* notify_get_or_create_batch(Op& carrier);

uint32_t notify_key_arg_order(const Shard& shard, const Op& source, Slice key) {
    for (uint32_t i = 1; i < source.argc(); i++)
        if (source.arg(i) == key) return shard.notify_order_base() + i * 16;
    return shard.notify_order_base() + 0x0ff0;
}

std::string notify_keyspace_channel(Slice key) {
    static constexpr char prefix[] = "__keyspace@0__:";
    std::string channel;
    channel.reserve(sizeof(prefix) - 1 + key.n);
    channel.append(prefix, sizeof(prefix) - 1);
    channel.append(key.p, key.n);
    return channel;
}

std::string notify_keyevent_channel(NotifyEventId event) {
    static constexpr char prefix[] = "__keyevent@0__:";
    std::string channel(prefix, sizeof(prefix) - 1);
    channel += notify_event_name(event);
    return channel;
}

bool notify_make_publication(Server& server, uint32_t routes, uint8_t route,
                             NotifyEventId event_id, Slice key,
                             std::string& channel, std::string& message) {
    if (route == 0) {
        if (!(routes & NOTIFY_KEYSPACE)) return false;
        channel = notify_keyspace_channel(key);
        message = notify_event_name(event_id);
        return true;
    }
    if (!(routes & NOTIFY_KEYEVENT)) return false;
    channel = notify_keyevent_channel(event_id);
    message.assign(key.p, key.n);
    (void)server;
    return true;
}

std::shared_ptr<PubSubNotificationChain> notify_reserve_chain(
        Server& server, std::unique_ptr<PubSubNotificationChain> chain) {
    const uint64_t count = chain ? chain->items.size() : 0;
    if (!count || !server.pubsub_notification_reserve(count, kNotifyInflightCap)) return {};
    PubSubNotificationChain* raw = chain.release();
    try {
        return std::shared_ptr<PubSubNotificationChain>(
            raw, [&server, count](PubSubNotificationChain* owned) {
                delete owned;
                server.pubsub_notification_retire(count);
            });
    } catch (...) {
        delete raw;
        server.pubsub_notification_retire(count);
        throw;
    }
}

}  // namespace

struct NotifyBatch {
    std::mutex mutex;
    NotifyRecordImpl inline_records[8];
    uint32_t inline_size = 0;
    uint32_t total = 0;
    uint32_t next_sequence = 0;
    std::vector<NotifyRecordImpl> spill;
    std::deque<std::string> owned_keys;
};

namespace {

NotifyBatch* notify_get_or_create_batch(Op& carrier) {
    if (auto* slot = notify_special_slot(carrier)) {
        NotifyBatch* batch = slot->load(std::memory_order_acquire);
        if (batch) return batch;
        auto* created = new (std::nothrow) NotifyBatch;
        if (!created) return nullptr;
        if (!slot->compare_exchange_strong(batch, created, std::memory_order_release,
                                           std::memory_order_acquire)) {
            delete created;
            return batch;
        }
        return created;
    }
    if (carrier.has_notify_state()) return static_cast<NotifyBatch*>(carrier.notify_state());
    if (carrier.zc_ptr) std::abort();  // a borrowed GET and an event batch are mutually exclusive
    auto* batch = new (std::nothrow) NotifyBatch;
    if (batch) carrier.attach_notify_state(batch);
    return batch;
}

}  // namespace

void notify_execute_enter(Shard& shard, Op& carrier, Op& source, uint32_t order_base) {
    shard.set_notify_context(&carrier, &source, order_base);
}

void notify_execute_source(Shard& shard, Op& source, uint32_t order_base) {
    shard.set_notify_context(shard.notify_carrier(), &source, order_base);
}

void notify_execute_leave(Shard& shard) {
    shard.set_notify_context(nullptr, nullptr, 0);
}

NotifyExecutionScope::NotifyExecutionScope(Shard& shard, Op* op, bool active,
                                           uint32_t order_base) {
    if (!active || !op) return;
    shard_ = &shard;
    carrier_ = shard.notify_carrier();
    source_ = shard.notify_source();
    order_base_ = shard.notify_order_base();
    if (carrier_) notify_execute_source(shard, *op, order_base);
    else notify_execute_enter(shard, *op, *op, order_base);
}

NotifyExecutionScope::~NotifyExecutionScope() {
    if (!shard_) return;
    if (carrier_) shard_->set_notify_context(carrier_, source_, order_base_);
    else notify_execute_leave(*shard_);
}

void notify_execute_handler(Shard& shard, Op& op, void (*handler)(Shard&, Op&)) {
    Op* carrier = shard.notify_carrier();
    Op* prior_source = shard.notify_source();
    const uint32_t order_base = shard.notify_order_base();
    if (carrier) notify_execute_source(shard, op, order_base);
    else notify_execute_enter(shard, op, op);
    handler(shard, op);
    if (carrier) shard.set_notify_context(carrier, prior_source, order_base);
    else notify_execute_leave(shard);
}

void notify_bind_flat_store(const FlatStore* store, FlatNotifySink* sink) {
    try { g_notify_flat_stores.push_back(FlatStoreBinding{store, sink}); }
    catch (const std::bad_alloc&) {}
}

void notify_flat_store_emit(const FlatStore* store, uint32_t cls,
                            NotifyEventId event, Slice key) {
    for (const FlatStoreBinding& binding : g_notify_flat_stores) {
        if (binding.store != store) continue;
        FlatNotifySink* sink = binding.sink;
        if (sink && sink->enabled && sink->enabled(sink->context, cls) && sink->emit)
            sink->emit(sink->context, cls, event, key);
        return;
    }
}

bool notify_record_slow(Shard& shard, Op& source, uint32_t routes, uint32_t cls,
                        NotifyEventId event, Slice key) {
    (void)cls;
    Server* server = shard.server();
    if (!server) return false;
    // Tracking invalidation has no pub/sub subscriber to require. When only the pub/sub routes
    // are armed the original early-out is unchanged.
    if (!server->pubsub_any_subscribers() && !(routes & NOTIFY_TRACKING)) return false;
    Op* carrier = shard.notify_carrier();
    if (!carrier) carrier = &source;
    NotifyBatch* batch = notify_get_or_create_batch(*carrier);
    if (!batch) { server->notify_event_dropped(); return false; }

    std::lock_guard<std::mutex> lock(batch->mutex);
    if (batch->total >= kNotifyBatchCap) {
        server->notify_event_dropped();
        return false;
    }
    Slice stable = key;
    bool pinned = false;
    for (uint32_t i = 1; i < carrier->argc(); i++) {
        if (carrier->arg(i) == key) { stable = carrier->arg(i); pinned = true; break; }
    }
    if (!pinned && carrier == &source) {
        for (uint32_t i = 1; i < source.argc(); i++) {
            if (source.arg(i) == key) { stable = source.arg(i); pinned = true; break; }
        }
    }
    if (!pinned) {
        try {
            batch->owned_keys.emplace_back(key.p, key.n);
            const std::string& owned = batch->owned_keys.back();
            stable = Slice(owned.data(), static_cast<uint32_t>(owned.size()));
        } catch (const std::bad_alloc&) {
            server->notify_event_dropped();
            return false;
        }
    }

    NotifyRecordImpl record;
    record.order = notify_key_arg_order(shard, source, key) + notify_event_priority(event);
    record.sequence = batch->next_sequence++;
    record.routes = routes;
    if (!server->pubsub_any_subscribers())
        record.routes &= ~(NOTIFY_KEYSPACE | NOTIFY_KEYEVENT);
    record.event = event;
    record.key = stable;
    try {
        if (batch->inline_size < 8) batch->inline_records[batch->inline_size++] = record;
        else batch->spill.push_back(record);
    } catch (const std::bad_alloc&) {
        server->notify_event_dropped();
        return false;
    }
    batch->total++;
    server->notify_event_fired();
    return true;
}

bool notify_record_keyless_slow(Shard& shard, uint32_t routes, uint32_t cls,
                                NotifyEventId event, Slice key) {
    (void)cls;
    Server* server = shard.server();
    if (!server) return false;
    if (!server->pubsub_any_subscribers() && !(routes & NOTIFY_TRACKING)) return false;
    auto& slot = shard.notify_state_slot();
    if (!slot) {
        slot.reset(new (std::nothrow) NotifyShardState);
        if (!slot) { server->notify_event_dropped(); return false; }
    }
    if (slot->keyless.size() >= kNotifyKeylessCap) {
        server->notify_event_dropped();
        return false;
    }
    try {
        NotifyOut out;
        out.routes = routes;
        if (!server->pubsub_any_subscribers())
            out.routes &= ~(NOTIFY_KEYSPACE | NOTIFY_KEYEVENT);
        out.event = event;
        out.key.assign(key.p, key.n);
        slot->keyless.push_back(std::move(out));
    } catch (const std::bad_alloc&) {
        server->notify_event_dropped();
        return false;
    }
    shard.notify_output_created();
    server->notify_event_fired();
    return true;
}

bool notify_flat_enabled(void* context, uint32_t cls) {
    auto& shard = *static_cast<Shard*>(context);
    const uint32_t mask = shard.notify_mask();
    const bool observed = (mask & NOTIFY_OBSERVERS) && (NOTIFY_ALL & cls);
    if (!(mask & cls) && !observed) return false;
    if (observed && (mask & NOTIFY_SAVE)) return true;
    const uint32_t routes = notify_routes_for_class(mask, cls);
    if (!routes) return false;
    if (cls == NOTIFY_KEY_MISS) {
        const Op* source = shard.notify_source();
        if (!source || !(source->spec->flags & CmdFlags::Readonly) ||
            (source->spec->flags & CmdFlags::Write)) return false;
    }
    Server* server = shard.server();
    if (!server) return false;
    // Tracking invalidation needs the expiry/eviction observers even with nobody subscribed.
    return server->pubsub_any_subscribers() || (routes & NOTIFY_TRACKING) != 0;
}

void notify_flat_emit(void* context, uint32_t cls, NotifyEventId event, Slice key) {
    auto& shard = *static_cast<Shard*>(context);
    // Hexpired/Del raised by the active field-expire cycle have no originating command either, so
    // they take the same keyless route as key expiry rather than being dropped for want of a source.
    if (event == NotifyEventId::Expired || event == NotifyEventId::Evicted ||
        event == NotifyEventId::Hexpired ||
        (event == NotifyEventId::Del && !shard.notify_source())) {
        notify_record_keyless(shard, cls, event, key);
        return;
    }
    Op* source = shard.notify_source();
    if (source) notify_record(shard, *source, cls, event, key);
}

uint32_t notify_ex_pass_entry(Server& server, Shard& shard, uint32_t producer,
                              ThreadCtx& thread, Ring& ring, LoopSignals& signals) {
    (void)thread;
    auto& slot = shard.notify_state_slot();
    if (!slot || slot->keyless.empty()) return 0;
    uint32_t work = 0;
    while (work < kNotifyKeylessPass && !slot->keyless.empty()) {
        NotifyOut& out = slot->keyless.front();
        // Lane F: an expired or evicted key is an invalidation. Post it to every io owner that
        // holds tracking connections; each filters against its own table. The executor never
        // touches a Client here -- only the same io-to-io transport pub/sub already uses.
        if (__builtin_expect((out.routes & NOTIFY_TRACKING) != 0, 0)) {
            const uint64_t io_mask = server.climon_tracking_io_mask();
            for (uint32_t io : server.placement().ifid_threads()) {
                if (!((io_mask >> (io & 63)) & 1)) continue;
                auto* invalidate = new (std::nothrow) PubSubEvent;
                if (!invalidate) break;
                invalidate->kind = PubSubEventKind::TrackingInvalidate;
                invalidate->target_io = io;
                invalidate->caller_id = 0;
                invalidate->items.push_back(PubSubEventItem{0, false, 0, out.key});
                server.pubsub_event_created();
                if (!server.thread(io).post_pubsub_event_nonblocking(
                        producer, invalidate, ring, signals)) {
                    server.pubsub_event_retired();
                    delete invalidate;
                }
            }
        }
        std::unique_ptr<PubSubNotificationChain> building;
        try {
            building = std::make_unique<PubSubNotificationChain>();
            building->items.reserve(2);
            for (uint8_t route = 0; route < 2; route++) {
                std::string channel, message;
                if (!notify_make_publication(
                        server, out.routes, route, out.event,
                        Slice(out.key.data(), static_cast<uint32_t>(out.key.size())),
                        channel, message)) continue;
                building->items.push_back(
                    PubSubNotificationItem{std::move(channel), std::move(message)});
            }
        } catch (const std::bad_alloc&) {
            server.notify_event_dropped();
            return work;
        }
        std::shared_ptr<PubSubNotificationChain> chain;
        try {
            chain = notify_reserve_chain(server, std::move(building));
        } catch (const std::bad_alloc&) {
            server.notify_event_dropped();
            return work;
        }
        if (!chain) {
            // A tracking-only record has no pub/sub publication to make; its invalidation was
            // already posted above, so retiring it here is completion, not a drop.
            if (out.routes & (NOTIFY_KEYSPACE | NOTIFY_KEYEVENT)) server.notify_event_dropped();
            slot->keyless.pop_front();
            work++;
            continue;
        }
        auto* event = new (std::nothrow) PubSubEvent;
        if (!event) { server.notify_event_dropped(); return work; }
        event->kind = PubSubEventKind::NotificationEnqueue;
        event->notification = std::move(chain);
        const auto& ios = server.placement().ifid_threads();
        const uint32_t coordinator = ios[producer % ios.size()];
        server.pubsub_event_created();
        if (!server.thread(coordinator).post_pubsub_event_nonblocking(
                producer, event, ring, signals)) {
            server.pubsub_event_retired();
            delete event;
            return work;
        }
        slot->keyless.pop_front();
        work++;
    }
    return work;
}

NotifyBatch* notify_take_batch(Op& op) {
    if (op.has_multi_state()) {
        auto* multi = static_cast<MultiExecState*>(op.multi_state());
        if (multi->aborted.load(std::memory_order_acquire) ||
            multi->watch_failed.load(std::memory_order_acquire)) {
            NotifyBatch* discarded = multi->notify.exchange(nullptr, std::memory_order_acq_rel);
            notify_discard_batch(discarded);
            return nullptr;
        }
    }
    if (op.has_scatter_state()) {
        ScatterState* scatter = op.scatter_state();
        if ((scatter->atomic_write || scatter->atomic_apply || scatter->atomic_pop) &&
            scatter->aborted.load(std::memory_order_acquire)) {
            NotifyBatch* discarded = scatter->notify.exchange(nullptr, std::memory_order_acq_rel);
            notify_discard_batch(discarded);
            return nullptr;
        }
    }
    if (auto* slot = notify_special_slot(op)) {
        NotifyBatch* batch = slot->load(std::memory_order_acquire);
        return batch ? slot->exchange(nullptr, std::memory_order_acq_rel) : nullptr;
    }
    if (!op.has_notify_state()) return nullptr;
    auto* batch = static_cast<NotifyBatch*>(op.notify_state());
    op.zc_ptr = nullptr;
    op.zc_len = 0;
    op.zc_shard = -1;
    return batch;
}

void notify_retire_batch_entry(IoLoop& loop, NotifyBatch* batch, uint64_t writer_id) {
    if (!batch) return;
    std::vector<NotifyRecordImpl> records;
    try {
        records.reserve(batch->total);
        for (uint32_t i = 0; i < batch->inline_size; i++)
            records.push_back(batch->inline_records[i]);
        records.insert(records.end(), batch->spill.begin(), batch->spill.end());
        std::stable_sort(records.begin(), records.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.order < rhs.order || (lhs.order == rhs.order && lhs.sequence < rhs.sequence);
        });
        // Lane F: CLIENT TRACKING rides the SAME observation. A record only carries the tracking
        // route while some connection has tracking on, so with the feature off this loop does not
        // exist at runtime -- records.empty() is the ordinary case and the routes test fails.
        if (__builtin_expect(loop.srv_->climon_armed() & Server::kClimonTracking, 0)) {
            std::vector<std::string> changed;
            for (const NotifyRecordImpl& record : records) {
                if (!(record.routes & NOTIFY_TRACKING)) continue;
                // A key MISS is not a value change; everything else that reaches a notification
                // fire point mutated or removed the key.
                if (record.event == NotifyEventId::Keymiss) continue;
                changed.emplace_back(record.key.p, record.key.n);
            }
            loop.tracking_broadcast_keys(changed, writer_id);
        }
        auto building = std::make_unique<PubSubNotificationChain>();
        building->items.reserve(records.size() * 2);
        for (const NotifyRecordImpl& record : records) {
            for (uint8_t route = 0; route < 2; route++) {
                std::string channel, message;
                if (!notify_make_publication(*loop.srv_, record.routes, route, record.event,
                                             record.key, channel, message)) continue;
                building->items.push_back(
                    PubSubNotificationItem{std::move(channel), std::move(message)});
            }
        }
        auto chain = notify_reserve_chain(*loop.srv_, std::move(building));
        if (!chain) {
            loop.srv_->notify_event_dropped(batch->total);
            delete batch;
            return;
        }
        loop.pubsub_notification_enqueue(std::move(chain));
        loop.pubsub_drain_events();
    } catch (const std::bad_alloc&) {
        loop.srv_->notify_event_dropped(records.size() < batch->total
                                            ? batch->total - records.size() : 1);
    }
    delete batch;
}

void notify_discard_batch(NotifyBatch* batch) { delete batch; }

void notify_abort_op(Op& op) {
    if (!op.has_notify_state()) return;
    auto* batch = static_cast<NotifyBatch*>(op.notify_state());
    op.zc_ptr = nullptr;
    op.zc_len = 0;
    op.zc_shard = -1;
    notify_discard_batch(batch);
}

void notify_xshard_finished(Shard& shard, Op& op, ScatterState& state) {
    WorkError error = WorkError::None;
    if (first_error(state, error) || state.aborted.load(std::memory_order_acquire) ||
        state.final_reply == FinalReply::Work || state.final_reply == FinalReply::Internal ||
        state.final_reply == FinalReply::QueueFull ||
        state.final_reply == FinalReply::SortConversion) return;

    auto emit = [&](uint32_t cls, NotifyEventId event, uint32_t arg) {
        notify_record(shard, op, cls, event, op.arg(arg));
    };
    auto emit_atomic_new = [&](uint32_t image_pos, uint32_t arg, bool creates = true) {
        if (creates && state.atomic_apply && state.images && !state.images[image_pos].present)
            emit(NOTIFY_NEW, NotifyEventId::New, arg);
    };
    switch (state.kind) {
        case Kind::Del:
        case Kind::Unlink:
            // Atomic deletes install tombstone records instead of calling FlatStore::erase(), so
            // their owner-side status vector is the only committed fire point. Plain scatter
            // already emitted through erase() and must not be duplicated here.
            if (state.atomic_write)
                for (uint32_t pos = 0; pos < state.key_count; pos++)
                    if (state.status[pos])
                        emit(NOTIFY_GENERIC, NotifyEventId::Del, state.keys[pos].arg);
            break;
        case Kind::Mset:
            for (uint32_t arg = 1; arg < op.argc(); arg += 2)
                emit(NOTIFY_STRING, NotifyEventId::Set, arg);
            break;
        case Kind::Msetnx:
            if (state.final_integer == 1)
                for (uint32_t arg = 1; arg < op.argc(); arg += 2)
                    emit(NOTIFY_STRING, NotifyEventId::Set, arg);
            break;
        case Kind::Rename:
            if (state.final_reply == FinalReply::Ok) {
                emit(NOTIFY_GENERIC, NotifyEventId::RenameFrom, 1);
                emit_atomic_new(1, 2);
                emit(NOTIFY_GENERIC, NotifyEventId::RenameTo, 2);
            }
            break;
        case Kind::Renamenx:
            if (state.final_integer == 1) {
                emit(NOTIFY_GENERIC, NotifyEventId::RenameFrom, 1);
                emit_atomic_new(1, 2);
                emit(NOTIFY_GENERIC, NotifyEventId::RenameTo, 2);
            }
            break;
        case Kind::Copy:
            if (state.final_integer == 1) {
                emit_atomic_new(1, 2);
                emit(NOTIFY_GENERIC, NotifyEventId::CopyTo, 2);
            }
            break;
        case Kind::Smove:
            if (state.final_integer == 1 && !same_key(op)) {
                emit(NOTIFY_SET, NotifyEventId::Srem, 1);
                if (state.atomic_apply && state.apply && !state.apply[0].present)
                    emit(NOTIFY_GENERIC, NotifyEventId::Del, 1);
                emit_atomic_new(1, 2);
                emit(NOTIFY_SET, NotifyEventId::Sadd, 2);
            }
            break;
        case Kind::Lmove:
        case Kind::Rpoplpush:
            if (state.final_reply == FinalReply::Bulk) {
                emit(NOTIFY_LIST, state.from_left ? NotifyEventId::Lpop : NotifyEventId::Rpop, 1);
                if (state.atomic_apply && state.apply && !state.apply[0].present && !same_key(op))
                    emit(NOTIFY_GENERIC, NotifyEventId::Del, 1);
                emit_atomic_new(1, 2, !same_key(op));
                emit(NOTIFY_LIST, state.to_left ? NotifyEventId::Lpush : NotifyEventId::Rpush, 2);
            }
            break;
        case Kind::Bitop:
            if (state.final_reply == FinalReply::Integer) {
                if (state.apply && state.apply[0].present) {
                    emit_atomic_new(0, 2);
                    emit(NOTIFY_STRING, NotifyEventId::Set, 2);
                } else
                    emit(NOTIFY_GENERIC, NotifyEventId::Del, 2);
            }
            break;
        case Kind::Pfmerge:
            if (state.final_reply == FinalReply::Ok) {
                emit_atomic_new(0, 1);
                emit(NOTIFY_STRING, NotifyEventId::Pfadd, 1);
            }
            break;
        case Kind::Sinterstore:
        case Kind::Sunionstore:
        case Kind::Sdiffstore:
            if (state.final_reply == FinalReply::Integer) {
                if (state.apply && state.apply[0].present) {
                    emit_atomic_new(0, 1);
                    const NotifyEventId event = state.kind == Kind::Sinterstore
                        ? NotifyEventId::Sinterstore : state.kind == Kind::Sunionstore
                        ? NotifyEventId::Sunionstore : NotifyEventId::Sdiffstore;
                    emit(NOTIFY_SET, event, 1);
                } else {
                    emit(NOTIFY_GENERIC, NotifyEventId::Del, 1);
                }
            }
            break;
        case Kind::Zunionstore:
        case Kind::Zinterstore:
        case Kind::Zdiffstore:
            if (state.final_reply == FinalReply::Integer) {
                if (state.apply && state.apply[0].present) {
                    emit_atomic_new(0, 1);
                    const NotifyEventId event = state.kind == Kind::Zunionstore
                        ? NotifyEventId::Zunionstore : state.kind == Kind::Zinterstore
                        ? NotifyEventId::Zinterstore : NotifyEventId::Zdiffstore;
                    emit(NOTIFY_ZSET, event, 1);
                } else {
                    emit(NOTIFY_GENERIC, NotifyEventId::Del, 1);
                }
            }
            break;
        case Kind::Zrangestore:
            if (state.final_reply == FinalReply::Integer) {
                if (state.apply && state.apply[0].present) {
                    emit_atomic_new(0, 1);
                    emit(NOTIFY_ZSET, NotifyEventId::Zrangestore, 1);
                } else {
                    emit(NOTIFY_GENERIC, NotifyEventId::Del, 1);
                }
            }
            break;
        case Kind::Geosearchstore:
        case Kind::Georadius:
        case Kind::Georadiusbymember:
            if (state.geo_store && state.final_reply == FinalReply::Integer) {
                if (state.apply && state.apply[0].present) {
                    emit_atomic_new(0, state.geo_destination_arg);
                    const NotifyEventId event = state.kind == Kind::Geosearchstore
                        ? NotifyEventId::Geosearchstore : NotifyEventId::Georadiusstore;
                    emit(NOTIFY_ZSET, event,
                         state.geo_destination_arg);
                } else {
                    emit(NOTIFY_GENERIC, NotifyEventId::Del, state.geo_destination_arg);
                }
            }
            break;
        case Kind::Sort:
            if (state.sort_options.store && state.final_reply == FinalReply::Integer) {
                if (state.apply && state.apply[0].present) {
                    emit_atomic_new(0, state.sort_options.store_arg);
                    emit(NOTIFY_LIST, NotifyEventId::Sortstore, state.sort_options.store_arg);
                } else {
                    emit(NOTIFY_GENERIC, NotifyEventId::Del, state.sort_options.store_arg);
                }
            }
            break;
        default: break;
    }
}

}  // namespace tomo

// tracking.cc -- CLIENT TRACKING (RESP3 server-assisted client-side caching).
//
// OWNERSHIP, which is the whole design.
//
//   * The key -> interested-connections table is PER IO OWNER and holds only that owner's own
//     connections (IoLoop::climon_track_keys_). A connection is owned by exactly one io thread
//     at a time; migration extracts and installs its entries at the ownership commit edge. Thus
//     each table still has exactly one reader and one writer and NO LOCK EXISTS ON ANY ARMED PATH.
//     A single global table would have put a lock on the tracking-armed write path, which is
//     precisely where writes live.
//
//   * Read registration is io-side, inside the one armed gate parse_and_dispatch already reaches
//     (climon_armed_gate). The io thread already holds the connection, the command spec and the
//     parsed key range at that point, so registration is a hash insert with no cross-thread hop.
//     A read from a NON-tracking connection never reaches this file.
//
//   * The invalidation SOURCE is the executor-side keyspace-notification chassis: the armed write
//     handler variants already observe every mutation with its key, including the expiry and
//     eviction paths that no io thread can see. CLIENT TRACKING arms them through the synthetic
//     NOTIFY_TRACKING class, so a write is observed exactly once for both features and, when only
//     one of them is armed, the other produces nothing.
//
//   * Delivery is a broadcast of the changed KEY (never of client state) to the io owners that
//     actually have tracking connections, taken from Server::climon_tracking_io_mask(). Each
//     owner filters against its own table. A global monotonic key filter keeps writes to
//     un-tracked keys from posting anything at all.
//
// REDIRECT sends the invalidation to another connection, which may be owned by a different io
// thread; that is one extra hop through the same transport (TrackingDeliver).

#include "../core/io_loop.h"

namespace tomo {

namespace {

// Monotonic membership filter over every key any owner has registered. Set-only; cleared when a
// tracking table is flushed. A false positive costs one broadcast that every owner then filters
// out, so it can only ever cost work, never correctness.
constexpr uint32_t kTrackFilterWords = 1024;   // 64 Kbit
std::atomic<uint64_t> g_track_filter[kTrackFilterWords];
// Number of registered BCAST prefixes across the process. Non-zero forces every changed key to
// broadcast, because prefix membership cannot be tested by the key filter.
std::atomic<uint64_t> g_track_bcast_prefixes{0};

inline void track_filter_add(uint64_t hash) {
    const uint32_t word = static_cast<uint32_t>((hash >> 6) % kTrackFilterWords);
    g_track_filter[word].fetch_or(1ull << (hash & 63), std::memory_order_relaxed);
}

inline bool track_filter_may_contain(uint64_t hash) {
    const uint32_t word = static_cast<uint32_t>((hash >> 6) % kTrackFilterWords);
    return (g_track_filter[word].load(std::memory_order_relaxed) >> (hash & 63)) & 1;
}

void track_filter_clear() {
    for (uint32_t i = 0; i < kTrackFilterWords; i++)
        g_track_filter[i].store(0, std::memory_order_relaxed);
}

bool track_prefix_matches(const std::vector<std::string>& prefixes, Slice key) {
    if (prefixes.empty()) return true;   // BCAST with no PREFIX tracks the whole keyspace
    for (const std::string& prefix : prefixes) {
        if (prefix.size() > key.n) continue;
        if (std::memcmp(prefix.data(), key.p, prefix.size()) == 0) return true;
    }
    return false;
}

// Redis refuses overlapping BCAST prefixes so a key can never be reported twice.
bool track_prefix_overlaps(const std::string& a, const std::string& b) {
    const size_t shortest = std::min(a.size(), b.size());
    return std::memcmp(a.data(), b.data(), shortest) == 0;
}

}  // namespace

// ---- registration --------------------------------------------------------------------------

void IoLoop::tracking_note_prefix_registered(const std::string& prefix, bool added) {
    (void)prefix;
    if (added) {
        g_track_bcast_prefixes.fetch_add(1, std::memory_order_relaxed);
        srv_->climon_note_tracking_prefix_delta(1);
    } else {
        g_track_bcast_prefixes.fetch_sub(1, std::memory_order_relaxed);
        srv_->climon_note_tracking_prefix_delta(-1);
    }
}

// Called from climon_armed_gate for a connection whose tracking is ON. BCAST mode keeps no table
// at all -- its prefix registry is the whole subscription -- so only default (per-key
// remembering) mode registers here.
void IoLoop::tracking_register_read(Client* client, ClimonConn& state, Op& op) {
    const bool caching = state.caching_armed;
    state.caching_armed = false;   // CLIENT CACHING covers exactly the next command
    if (state.bcast) return;
    const CommandSpec* spec = op.spec;
    if (!spec) return;
    // Only genuine reads register. A write from the tracking connection is not a subscription.
    if (!(spec->flags & CmdFlags::Readonly) || (spec->flags & CmdFlags::Write)) return;
    if (spec->first_key <= 0) return;
    // OPTIN: register only when the previous command was CLIENT CACHING yes.
    // OPTOUT: register unless the previous command was CLIENT CACHING no.
    if (state.optin && !caching) return;
    if (state.optout && caching) return;

    const uint64_t id = client->id();
    const int32_t last = spec->last_key;
    const int32_t step = spec->key_step > 0 ? spec->key_step : 1;
    const uint32_t end = last < 0 ? op.argc()
                                  : std::min<uint32_t>(op.argc(),
                                                       static_cast<uint32_t>(last) + 1);
    for (uint32_t i = static_cast<uint32_t>(spec->first_key); i < end;
         i += static_cast<uint32_t>(step)) {
        const Slice key = op.arg(i);
        if (srv_->cfg().tracking_table_max_keys &&
            climon_track_keys_.size() >= srv_->cfg().tracking_table_max_keys &&
            climon_track_keys_.find(std::string(key.p, key.n)) == climon_track_keys_.end()) {
            // Bounded table: redis evicts a random entry and tells its owner the cached value is
            // no longer tracked. We do exactly that -- an eviction is an invalidation.
            auto victim = climon_track_keys_.begin();
            if (victim != climon_track_keys_.end()) {
                const Slice vkey(victim->first.data(),
                                 static_cast<uint32_t>(victim->first.size()));
                for (uint64_t owner : victim->second) {
                    ClimonConn* target = climon_conn_find(owner);
                    if (target) tracking_deliver_frame(*target, owner, vkey, false);
                }
                srv_->climon_note_tracking_item_delta(
                    -static_cast<int64_t>(victim->second.size()));
                srv_->climon_note_tracking_key_delta(-1);
                climon_track_keys_.erase(victim);
            }
        }
        std::vector<uint64_t>& owners = climon_track_keys_[std::string(key.p, key.n)];
        if (owners.empty()) srv_->climon_note_tracking_key_delta(1);
        if (std::find(owners.begin(), owners.end(), id) == owners.end()) {
            owners.push_back(id);
            srv_->climon_note_tracking_item_delta(1);
        }
        track_filter_add(FlatStore::hash_key(key));
    }
}

void IoLoop::tracking_forget_client(uint64_t id, ClimonConn& state) {
    for (auto it = climon_track_keys_.begin(); it != climon_track_keys_.end();) {
        auto& owners = it->second;
        auto found = std::find(owners.begin(), owners.end(), id);
        if (found != owners.end()) {
            owners.erase(found);
            srv_->climon_note_tracking_item_delta(-1);
        }
        if (owners.empty()) {
            srv_->climon_note_tracking_key_delta(-1);
            it = climon_track_keys_.erase(it);
        } else {
            ++it;
        }
    }
    for (const std::string& prefix : state.prefixes) tracking_note_prefix_registered(prefix, false);
    state.prefixes.clear();
    if (state.tracking_on) {
        state.tracking_on = false;
        state.bcast = state.optin = state.optout = state.noloop = false;
        state.caching_armed = false;
        state.redirect = 0;
        if (climon_local_trackers_) climon_local_trackers_--;
        if (!climon_local_trackers_) srv_->climon_set_tracking_io(self_->id(), false);
        srv_->climon_tracking_removed();
        if (state.client) command_client_set_tracking_view(state.client, false, -1);
    }
}

void IoLoop::tracking_migration_snapshot(uint64_t id, std::vector<std::string>& keys) const {
    for (const auto& entry : climon_track_keys_)
        if (std::find(entry.second.begin(), entry.second.end(), id) != entry.second.end())
            keys.push_back(entry.first);
}

void IoLoop::tracking_migration_extract(uint64_t id, std::vector<std::string>& keys) {
    size_t out = 0;
    for (size_t i = 0; i < keys.size(); i++) {
        std::string& key = keys[i];
        auto entry = climon_track_keys_.find(key);
        if (entry == climon_track_keys_.end()) continue;  // invalidated since reversible prepare
        auto owner = std::find(entry->second.begin(), entry->second.end(), id);
        if (owner == entry->second.end()) continue;
        *owner = entry->second.back();
        entry->second.pop_back();
        srv_->climon_note_tracking_item_delta(-1);
        if (entry->second.empty()) {
            srv_->climon_note_tracking_key_delta(-1);
            climon_track_keys_.erase(entry);
        }
        if (out != i) keys[out] = std::move(key);
        out++;
    }
    keys.resize(out);
}

bool IoLoop::tracking_migration_install(uint64_t id, const std::vector<std::string>& keys) {
    try {
        climon_track_keys_.reserve(climon_track_keys_.size() + keys.size());
        for (const std::string& key : keys) {
            std::vector<uint64_t>& owners = climon_track_keys_[key];
            if (owners.empty()) srv_->climon_note_tracking_key_delta(1);
            if (std::find(owners.begin(), owners.end(), id) == owners.end()) {
                owners.push_back(id);
                srv_->climon_note_tracking_item_delta(1);
            }
            track_filter_add(FlatStore::hash_key(Slice(
                key.data(), static_cast<uint32_t>(key.size()))));
        }
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

// ---- delivery ------------------------------------------------------------------------------

void IoLoop::tracking_emit_invalidation(Client* target, bool redirected, Slice key, bool flush) {
    const uint32_t live_io = target->ifid_thread();
    if (live_io != self_->id()) {
        PubSubEvent* event = pubsub_new_event(PubSubEventKind::TrackingDeliver);
        event->target_io = live_io;
        event->origin_io = self_->id();
        event->client_id = target->id();
        event->subscribe = redirected;
        event->count = flush ? 1 : 0;
        event->channel.assign(key.p, key.n);
        pubsub_post(live_io, event);
        srv_->tracking_forwarded_stale_added();
        return;
    }
    // Redis decides after resolving REDIRECT. RESP3 always has a push channel; RESP2 only has a
    // valid carriage when another connection is the target and that connection is in pub/sub mode.
    const bool resp3_push = target->resp3();
    if (!resp3_push && (!redirected || !target->subscriber_mode())) return;

    SmallBuf<128> frame;
    if (resp3_push) {
        reply_push_header(frame, 2);
        reply_bulk(frame, Slice("invalidate", 10));
    } else {
        // RESP2 legacy carriage: the invalidation arrives as an ordinary pub/sub message on the
        // reserved __redis__:invalidate channel.
        reply_array_header(frame, 3);
        reply_bulk(frame, Slice("message", 7));
        reply_bulk(frame, Slice("__redis__:invalidate", 20));
    }
    if (flush) {
        // Oracle-confirmed: RESP3 null (`_`), RESP2 redirect path null BULK (`$-1`).
        if (resp3_push) frame.append("_\r\n", 3);
        else frame.append("$-1\r\n", 5);
    } else {
        reply_array_header(frame, 1);
        reply_bulk(frame, key);
    }
    climon_push_wire(target, std::string(frame.data(), frame.size()));
    srv_->climon_note_invalidation();
}

// owner_id names the TRACKING connection. The frame goes to it, or to its REDIRECT target.
void IoLoop::tracking_deliver_frame(ClimonConn& state, uint64_t owner_id, Slice key, bool flush) {
    (void)owner_id;   // retained for call-site clarity and future per-owner accounting
    if (!state.redirect) {
        Client* target = state.client;
        if (!target || target->dead() || target->closing()) return;
        tracking_emit_invalidation(target, false, key, flush);
        return;
    }
    // REDIRECT: the target may be owned by another io thread.
    uint32_t owner_io = 0;
    if (!command_client_directory_find(state.redirect, owner_io)) {
        // Oracle-confirmed: detection is LAZY (nothing happens at the target's disconnect) and
        // tracking stays ON. A RESP3 tracking client is told with a `tracking-redir-broken` push
        // on EVERY subsequent invalidation attempt; a RESP2 one sees nothing on the wire and only
        // the TRACKINGINFO flag changes.
        state.broken_redirect = true;
        Client* self_client = state.client;
        if (self_client && !self_client->dead() && !self_client->closing() &&
            self_client->resp3()) {
            SmallBuf<64> frame;
            reply_push_header(frame, 2);
            reply_bulk(frame, Slice("tracking-redir-broken", 21));
            reply_int(frame, static_cast<long long>(state.redirect));
            climon_push_wire(self_client, std::string(frame.data(), frame.size()));
        }
        return;
    }
    if (owner_io == self_->id()) {
        ClimonConn* target_state = climon_conn_find(state.redirect);
        Client* target = target_state ? target_state->client : nullptr;
        if (!target) {
            for (Client* c : self_->clients())
                if (c && c->id() == state.redirect) { target = c; break; }
        }
        if (!target || target->dead() || target->closing()) return;
        tracking_emit_invalidation(target, true, key, flush);
        return;
    }
    PubSubEvent* event = pubsub_new_event(PubSubEventKind::TrackingDeliver);
    event->target_io = owner_io;
    event->origin_io = self_->id();
    event->client_id = state.redirect;
    event->subscribe = true;
    event->count = flush ? 1 : 0;
    event->channel.assign(key.p, key.n);
    pubsub_post(owner_io, event);
}

// One owner's local pass over a changed key.
void IoLoop::tracking_invalidate_local(Slice key, uint64_t writer_id) {
    // 1. BCAST subscribers, matched by prefix. No table lookup: the subscription IS the prefix.
    for (auto& entry : climon_conn_) {
        ClimonConn& state = entry.second;
        if (!state.tracking_on || !state.bcast) continue;
        if (state.noloop && entry.first == writer_id) continue;
        if (!track_prefix_matches(state.prefixes, key)) continue;
        tracking_deliver_frame(state, entry.first, key, false);
    }
    // 2. Per-key remembering. Redis forgets the key once it has reported it.
    if (climon_track_keys_.empty()) return;
    auto found = climon_track_keys_.find(std::string(key.p, key.n));
    if (found == climon_track_keys_.end()) return;
    std::vector<uint64_t> owners;
    owners.swap(found->second);
    srv_->climon_note_tracking_item_delta(-static_cast<int64_t>(owners.size()));
    srv_->climon_note_tracking_key_delta(-1);
    climon_track_keys_.erase(found);
    for (uint64_t owner : owners) {
        if (owner == writer_id) {
            ClimonConn* state = climon_conn_find(owner);
            if (state && state->noloop) continue;
        }
        ClimonConn* state = climon_conn_find(owner);
        if (state && state->tracking_on) tracking_deliver_frame(*state, owner, key, false);
    }
}

void IoLoop::tracking_broadcast_keys(const std::vector<std::string>& keys, uint64_t writer_id) {
    if (keys.empty()) return;
    const uint64_t mask = srv_->climon_tracking_io_mask();
    if (!mask) return;
    const bool bcast_active = g_track_bcast_prefixes.load(std::memory_order_relaxed) != 0;
    std::vector<std::string> interesting;
    interesting.reserve(keys.size());
    for (const std::string& key : keys) {
        const Slice slice(key.data(), static_cast<uint32_t>(key.size()));
        if (!bcast_active && !track_filter_may_contain(FlatStore::hash_key(slice))) continue;
        // One command can fire more than one notification for the SAME key (a type change plus
        // a value change, a MULTI body touching it twice). A client must see one invalidation.
        if (std::find(interesting.begin(), interesting.end(), key) != interesting.end()) continue;
        interesting.push_back(key);
    }
    if (interesting.empty()) return;

    PubSubEvent local;
    local.kind = PubSubEventKind::TrackingInvalidate;
    local.route_mask = mask;
    local.caller_id = writer_id;
    local.items.reserve(interesting.size());
    for (const std::string& key : interesting)
        local.items.push_back(PubSubEventItem{0, false, 0, key});
    if ((mask >> (self_->id() & 63)) & 1) tracking_handle_event(local);
    for (uint32_t io : srv_->placement().ifid_threads()) {
        if (io == self_->id()) continue;
        if (!((mask >> (io & 63)) & 1)) continue;
        PubSubEvent* event = pubsub_new_event(PubSubEventKind::TrackingInvalidate);
        event->target_io = io;
        event->origin_io = self_->id();
        event->caller_id = writer_id;
        event->route_mask = mask;
        event->items = local.items;
        pubsub_post(io, event);
    }
}

void IoLoop::tracking_broadcast_flush() {
    track_filter_clear();
    const uint64_t mask = srv_->climon_tracking_io_mask();
    if (!mask) return;
    PubSubEvent local;
    local.kind = PubSubEventKind::TrackingFlush;
    local.route_mask = mask;
    if ((mask >> (self_->id() & 63)) & 1) tracking_handle_event(local);
    for (uint32_t io : srv_->placement().ifid_threads()) {
        if (io == self_->id()) continue;
        if (!((mask >> (io & 63)) & 1)) continue;
        PubSubEvent* event = pubsub_new_event(PubSubEventKind::TrackingFlush);
        event->target_io = io;
        event->origin_io = self_->id();
        event->route_mask = mask;
        pubsub_post(io, event);
    }
}

void IoLoop::tracking_handle_event(PubSubEvent& event) {
    switch (event.kind) {
        case PubSubEventKind::TrackingInvalidate:
            for (const PubSubEventItem& item : event.items)
                tracking_invalidate_local(
                    Slice(item.value.data(), static_cast<uint32_t>(item.value.size())),
                    event.caller_id);
            tracking_forward_stale(event);
            break;
        case PubSubEventKind::TrackingFlush: {
            for (auto& entry : climon_conn_) {
                ClimonConn& state = entry.second;
                if (!state.tracking_on) continue;
                tracking_deliver_frame(state, entry.first, Slice(), true);
            }
            for (auto& entry : climon_track_keys_) {
                srv_->climon_note_tracking_item_delta(
                    -static_cast<int64_t>(entry.second.size()));
                srv_->climon_note_tracking_key_delta(-1);
            }
            climon_track_keys_.clear();
            tracking_forward_stale(event);
            break;
        }
        case PubSubEventKind::TrackingDeliver: {
            uint32_t live_io = 0;
            if (command_client_directory_find(event.client_id, live_io) &&
                live_io != self_->id()) {
                PubSubEvent* forward = pubsub_new_event(PubSubEventKind::TrackingDeliver);
                forward->target_io = live_io;
                forward->origin_io = self_->id();
                forward->client_id = event.client_id;
                forward->subscribe = event.subscribe;
                forward->count = event.count;
                forward->channel = event.channel;
                pubsub_post(live_io, forward);
                srv_->tracking_forwarded_stale_added();
                break;
            }
            Client* target = nullptr;
            for (Client* c : self_->clients())
                if (c && c->id() == event.client_id) { target = c; break; }
            if (!target || target->dead() || target->closing()) break;
            tracking_emit_invalidation(
                target, event.subscribe,
                Slice(event.channel.data(), static_cast<uint32_t>(event.channel.size())),
                event.count != 0);
            break;
        }
        default:
            break;
    }
}

void IoLoop::tracking_forward_stale(const PubSubEvent& event) {
    if (routing_forward_.empty()) return;
    uint64_t posted = event.route_mask;
    for (const auto& entry : routing_forward_) {
        if (!entry.second.tracking) continue;
        uint32_t live_io = 0;
        if (!command_client_directory_find(entry.first, live_io) || live_io == self_->id())
            continue;
        const uint64_t bit = 1ull << (live_io & 63);
        if (posted & bit) continue;
        PubSubEvent* forward = pubsub_new_event(event.kind);
        forward->target_io = live_io;
        forward->origin_io = self_->id();
        forward->caller_id = event.caller_id;
        forward->route_mask = posted | bit;
        forward->items = event.items;
        pubsub_post(live_io, forward);
        posted |= bit;
        srv_->tracking_forwarded_stale_added(
            event.kind == PubSubEventKind::TrackingFlush ? 1 : event.items.size());
    }
}

// ---- CLIENT TRACKING / CACHING / GETREDIR / TRACKINGINFO ------------------------------------

void IoLoop::tracking_reply_trackinginfo(Client* client, Op& op) {
    ClimonConn* state = climon_conn_find(client->id());
    auto sink = op.sink();
    // Redis 7.4 reports exactly three fields here; the table counters live in INFO instead.
    reply_map_header(sink, 3, op.resp3());

    reply_bulk(sink, Slice("flags", 5));
    std::vector<const char*> flags;
    if (!state || !state->tracking_on) {
        flags.push_back("off");
    } else {
        flags.push_back("on");
        if (state->bcast) flags.push_back("bcast");
        if (state->optin) flags.push_back("optin");
        if (state->optout) flags.push_back("optout");
        if (state->optin && state->caching_armed) flags.push_back("caching-yes");
        if (state->optout && state->caching_armed) flags.push_back("caching-no");
        if (state->noloop) flags.push_back("noloop");
    }
    if (state && state->broken_redirect) flags.push_back("broken_redirect");
    reply_set_header(sink, flags.size(), op.resp3());
    for (const char* flag : flags)
        reply_bulk(sink, Slice(flag, static_cast<uint32_t>(std::strlen(flag))));

    reply_bulk(sink, Slice("redirect", 8));
    // Tracking off reports -1; on reports the redirect id (0 when none was requested), and a
    // broken redirect keeps reporting the id of the connection that went away.
    if (!state || !state->tracking_on) reply_int(sink, -1);
    else reply_int(sink, static_cast<long long>(state->redirect));

    reply_bulk(sink, Slice("prefixes", 8));
    // Oracle-confirmed: `flags` is a SET in RESP3 but `prefixes` stays a plain array.
    const size_t nprefix = state ? state->prefixes.size() : 0;
    reply_array_header(sink, nprefix);
    if (state)
        for (const std::string& prefix : state->prefixes)
            reply_bulk(sink, Slice(prefix.data(), static_cast<uint32_t>(prefix.size())));
}

IoLoop::ClimonStartResult IoLoop::tracking_client_subcommand(Client* client, Op& op) {
    const Slice sub = op.arg(1);

    if (sub.eq_icase("getredir")) {
        if (op.argc() != 2) {
            climon_wrong_args(op, "getredir");
            return ClimonStartResult::Sync;
        }
        ClimonConn* state = climon_conn_find(client->id());
        reply_int(op.sink(), state && state->tracking_on
                                 ? static_cast<long long>(state->redirect) : -1);
        return ClimonStartResult::Sync;
    }

    if (sub.eq_icase("trackinginfo")) {
        if (op.argc() != 2) {
            climon_wrong_args(op, "trackinginfo");
            return ClimonStartResult::Sync;
        }
        tracking_reply_trackinginfo(client, op);
        return ClimonStartResult::Sync;
    }

    if (sub.eq_icase("caching")) {
        if (op.argc() != 3) {
            climon_wrong_args(op, "caching");
            return ClimonStartResult::Sync;
        }
        ClimonConn* state = climon_conn_find(client->id());
        // Oracle order: the tracking-state check comes FIRST, so `CLIENT CACHING maybe` on a
        // non-tracking connection reports the mode error, not a syntax error.
        if (!state || !state->tracking_on) {
            reply_err(op.sink(),
                "ERR CLIENT CACHING can be called only when the client is in tracking mode with "
                "OPTIN or OPTOUT mode enabled");
            return ClimonStartResult::Sync;
        }
        const bool yes = op.arg(2).eq_icase("yes");
        if (!yes && !op.arg(2).eq_icase("no")) {
            reply_err(op.sink(), "ERR syntax error");
            return ClimonStartResult::Sync;
        }
        if (yes && !state->optin) {
            reply_err(op.sink(),
                "ERR CLIENT CACHING YES is only valid when tracking is enabled in OPTIN mode.");
            return ClimonStartResult::Sync;
        }
        if (!yes && !state->optout) {
            reply_err(op.sink(),
                "ERR CLIENT CACHING NO is only valid when tracking is enabled in OPTOUT mode.");
            return ClimonStartResult::Sync;
        }
        state->caching_armed = true;
        reply_ok(op.sink());
        return ClimonStartResult::Sync;
    }

    if (!sub.eq_icase("tracking")) return ClimonStartResult::NotHandled;
    if (op.argc() < 3) {
        reply_err(op.sink(), "ERR wrong number of arguments for 'client|tracking' command");
        return ClimonStartResult::Sync;
    }

    bool enable = false;
    if (op.arg(2).eq_icase("on")) enable = true;
    else if (op.arg(2).eq_icase("off")) enable = false;
    else { reply_syntax(op.sink()); return ClimonStartResult::Sync; }

    bool bcast = false, optin = false, optout = false, noloop = false;
    uint64_t redirect = 0;
    std::vector<std::string> prefixes;
    for (uint32_t i = 3; i < op.argc(); i++) {
        const Slice token = op.arg(i);
        if (token.eq_icase("redirect")) {
            if (++i >= op.argc()) { reply_syntax(op.sink()); return ClimonStartResult::Sync; }
            int64_t id = 0;
            if (!climon_parse_i64(op.arg(i), id)) {
                reply_err(op.sink(), "ERR value is not an integer or out of range");
                return ClimonStartResult::Sync;
            }
            // A negative or zero id parses fine and then simply names no connection.
            uint32_t owner_io = 0;
            if (id <= 0 ||
                !command_client_directory_find(static_cast<uint64_t>(id), owner_io)) {
                reply_err(op.sink(),
                    "ERR The client ID you want redirect to does not exist");
                return ClimonStartResult::Sync;
            }
            redirect = static_cast<uint64_t>(id);
        } else if (token.eq_icase("bcast")) {
            bcast = true;
        } else if (token.eq_icase("prefix")) {
            if (++i >= op.argc()) { reply_syntax(op.sink()); return ClimonStartResult::Sync; }
            prefixes.emplace_back(op.arg(i).p, op.arg(i).n);
        } else if (token.eq_icase("optin")) {
            optin = true;
        } else if (token.eq_icase("optout")) {
            optout = true;
        } else if (token.eq_icase("noloop")) {
            noloop = true;
        } else {
            reply_syntax(op.sink());
            return ClimonStartResult::Sync;
        }
    }

    // ORACLE-DERIVED CHECK ORDER (each step is pinned by a differ case):
    //   1 OPTIN+OPTOUT together        4 OPTIN/OPTOUT switch on an already-on client
    //   2 PREFIX without BCAST         5 BCAST combined with OPTIN/OPTOUT
    //   3 BCAST switch on an           6 overlap against prefixes this client already holds
    //     already-on client            7 overlap among the prefixes this command provides
    // `CLIENT TRACKING off <anything>` is accepted by redis, so every rule is enable-gated.
    ClimonConn* existing = climon_conn_find(client->id());
    const bool live = existing != nullptr && existing->tracking_on && enable;

    if (enable && optin && optout) {
        reply_err(op.sink(),
            "ERR You can't specify both OPTIN mode and OPTOUT mode");
        return ClimonStartResult::Sync;
    }
    if (enable && !prefixes.empty() && !bcast) {
        reply_err(op.sink(),
            "ERR PREFIX option requires BCAST mode to be enabled");
        return ClimonStartResult::Sync;
    }
    if (live && existing->bcast != bcast) {
        reply_err(op.sink(),
            "ERR You can't switch BCAST mode on/off before disabling tracking for this "
            "client, and then re-enabling it with a different mode.");
        return ClimonStartResult::Sync;
    }
    // Adding a mode the connection did not have is legal; CONTRADICTING the held one is not.
    if (live && ((existing->optin && optout) || (existing->optout && optin))) {
        reply_err(op.sink(),
            "ERR You can't switch OPTIN/OPTOUT mode before disabling tracking for this "
            "client, and then re-enabling it with a different mode.");
        return ClimonStartResult::Sync;
    }
    if (enable && bcast && (optin || optout)) {
        reply_err(op.sink(),
            "ERR OPTIN and OPTOUT are not compatible with BCAST");
        return ClimonStartResult::Sync;
    }
    if (live)
        for (const std::string& fresh : prefixes)
            for (const std::string& held : existing->prefixes)
                if (track_prefix_overlaps(fresh, held)) {
                    std::string error = "ERR Prefix '";
                    error += fresh;
                    error += "' overlaps with an existing prefix '";
                    error += held;
                    error += "'. Prefixes for a single client must not overlap.";
                    reply_err(op.sink(), error.c_str());
                    return ClimonStartResult::Sync;
                }
    for (size_t a = 0; enable && a < prefixes.size(); a++)
        for (size_t b = a + 1; b < prefixes.size(); b++)
            if (track_prefix_overlaps(prefixes[a], prefixes[b])) {
                // Oracle wording: the FIRST-listed prefix is named first.
                std::string error = "ERR Prefix '";
                error += prefixes[a];
                error += "' overlaps with another provided prefix '";
                error += prefixes[b];
                error += "'. Prefixes for a single client must not overlap.";
                reply_err(op.sink(), error.c_str());
                return ClimonStartResult::Sync;
            }

    if (!enable) {
        if (existing) {
            tracking_forget_client(client->id(), *existing);
            existing->broken_redirect = false;
            climon_conn_release(client->id());
            climon_refresh_armed();
        }
        reply_ok(op.sink());
        return ClimonStartResult::Sync;
    }

    ClimonConn& state = climon_conn_get(client);
    if (!state.tracking_on) {
        state.tracking_on = true;
        climon_local_trackers_++;
        srv_->climon_set_tracking_io(self_->id(), true);
        srv_->climon_tracking_added();
    }
    // Redis REPLACES the per-call modifiers rather than accumulating them: a plain
    // `CLIENT TRACKING on` after `on OPTIN`/`on NOLOOP` clears optin/noloop (differ-pinned).
    // Only an explicit contradiction (OPTIN while OPTOUT is held) is refused, above.
    state.bcast = bcast;
    // Oracle-confirmed: `CLIENT TRACKING on BCAST` with no PREFIX registers the EMPTY prefix,
    // which TRACKINGINFO reports as one zero-length entry and which matches every key.
    if (bcast && prefixes.empty() && state.prefixes.empty()) prefixes.emplace_back();
    state.optin = optin;
    state.optout = optout;
    state.noloop = noloop;
    state.redirect = redirect;
    state.broken_redirect = false;
    for (std::string& prefix : prefixes) {
        tracking_note_prefix_registered(prefix, true);
        state.prefixes.push_back(std::move(prefix));
    }
    std::sort(state.prefixes.begin(), state.prefixes.end());   // redis reports them sorted
    command_client_set_tracking_view(client, true, static_cast<int64_t>(redirect));
    climon_refresh_armed();
    reply_ok(op.sink());
    return ClimonStartResult::Sync;
}

}  // namespace tomo

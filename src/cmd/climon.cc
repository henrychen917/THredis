// climon.cc -- cold implementation of the IO-owned CLIENT connection-control surface and MONITOR.
//
// EVERYTHING IN THIS FILE IS COLD. parse_and_dispatch reaches it through exactly one call --
// climon_armed_gate -- placed inside the notification-armed branch it already takes, so with the
// lane idle the emitted hot path is byte-for-byte the pre-lane sequence. The previous attempt at
// this surface sprinkled a feed hook at every dispatch site and measured +64.9 instructions/op on
// an idle server; that patch is preserved out of tree as the cautionary reference and none of its
// hot-path shape is reused here.
//
// CLIENT TRACKING lives in tracking.cc. This file owns:
//   CLIENT HELP / NO-TOUCH / REPLY / PAUSE / UNPAUSE / UNBLOCK, MONITOR, and the shared
//   per-connection lane state (ClimonConn) plus its arming counters.

#include "../core/io_loop.h"

#include <cinttypes>

namespace tomo {

namespace {

// Redis quotes MONITOR arguments with the same escaping sdscatrepr uses for DEBUG output.
void climon_quote_arg(Slice value, std::string& out) {
    out.push_back('"');
    for (uint32_t i = 0; i < value.n; i++) {
        const unsigned char ch = static_cast<unsigned char>(value.p[i]);
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\a': out += "\\a"; break;
            case '\b': out += "\\b"; break;
            default:
                if (ch >= ' ' && ch <= '~') {
                    out.push_back(static_cast<char>(ch));
                } else {
                    char hex[8];
                    std::snprintf(hex, sizeof(hex), "\\x%02x", ch);
                    out += hex;
                }
        }
    }
    out.push_back('"');
}

bool climon_eq_on_off(Slice value, bool& enabled) {
    if (value.eq_icase("on")) { enabled = true; return true; }
    if (value.eq_icase("off")) { enabled = false; return true; }
    return false;
}

constexpr const char* kClimonHelp[] = {
    "CLIENT <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
    "CACHING (YES|NO)",
    "    Enable/disable tracking of the keys for next command in OPTIN/OPTOUT modes.",
    "GETREDIR",
    "    Return the client ID we are redirecting to when tracking is enabled.",
    "GETNAME",
    "    Return the name of the current connection.",
    "ID",
    "    Return the ID of the current connection.",
    "INFO",
    "    Return information about the current client connection.",
    "KILL <ip:port>",
    "    Kill connection made from <ip:port>.",
    "KILL <option> <value> [<option> <value> [...]]",
    "    Kill connections. Options are:",
    "    * ADDR (<ip:port>|<unixsocket>:0)",
    "      Kill connections made from the specified address",
    "    * LADDR (<ip:port>|<unixsocket>:0)",
    "      Kill connections made to specified local address",
    "    * TYPE (NORMAL|MASTER|REPLICA|PUBSUB)",
    "      Kill connections by type.",
    "    * USER <username>",
    "      Kill connections authenticated by <username>.",
    "    * SKIPME (YES|NO)",
    "      Skip killing current connection (default: yes).",
    "    * ID <client-id>",
    "      Kill connections by client id.",
    "    * MAXAGE <maxage>",
    "      Kill connections older than the specified age.",
    "LIST [options ...]",
    "    Return information about client connections. Options:",
    "    * TYPE (NORMAL|MASTER|REPLICA|PUBSUB)",
    "      Return clients of specified type.",
    "UNPAUSE",
    "    Stop the current client pause, resuming traffic.",
    "PAUSE <timeout> [WRITE|ALL]",
    "    Suspend all, or just write, clients for <timeout> milliseconds.",
    "REPLY (ON|OFF|SKIP)",
    "    Control the replies sent to the current connection.",
    "SETNAME <name>",
    "    Assign the name <name> to the current connection.",
    "SETINFO <option> <value>",
    "    Set client meta attr. Options are:",
    "    * LIB-NAME: the client lib name.",
    "    * LIB-VER: the client lib version.",
    "UNBLOCK <clientid> [TIMEOUT|ERROR]",
    "    Unblock the specified blocked client.",
    "TRACKING (ON|OFF) [REDIRECT <id>] [BCAST] [PREFIX <prefix> [...]]",
    "         [OPTIN] [OPTOUT] [NOLOOP]",
    "    Control server assisted client side caching.",
    "TRACKINGINFO",
    "    Report tracking status for the current connection.",
    "NO-EVICT (ON|OFF)",
    "    Protect current client connection from eviction.",
    "NO-TOUCH (ON|OFF)",
    "    Will not touch LRU/LFU stats when this mode is on.",
    "HELP",
    "    Print this help.",
};

}  // namespace

// Redis reports a per-subcommand arity error, not a generic syntax error. Shared with
// tracking.cc, which owns the TRACKING/CACHING/GETREDIR/TRACKINGINFO half of the surface.
void climon_wrong_args(Op& op, const char* subcommand) {
    std::string error = "ERR wrong number of arguments for 'client|";
    error += subcommand;
    error += "' command";
    reply_err(op.sink(), error.c_str());
}

bool IoLoop::climon_parse_i64(Slice value, int64_t& out) {
    if (!value.n) return false;
    uint32_t pos = 0;
    bool negative = false;
    if (value.p[pos] == '-') { negative = true; if (++pos == value.n) return false; }
    // Canonical decimal, as redis's string2ll: no leading '+', no leading zeroes, no negative
    // zero. "CLIENT UNBLOCK 05" and "CLIENT KILL ID 05" were accepted before this.
    if (value.p[pos] == '0') {
        if (negative || pos + 1 != value.n) return false;
        out = 0;
        return true;
    }
    if (value.p[pos] < '1' || value.p[pos] > '9') return false;
    uint64_t magnitude = 0;
    const uint64_t limit = negative ? uint64_t{INT64_MAX} + 1 : uint64_t{INT64_MAX};
    for (; pos < value.n; pos++) {
        const char ch = value.p[pos];
        if (ch < '0' || ch > '9') return false;
        const uint32_t digit = static_cast<uint32_t>(ch - '0');
        if (magnitude > (limit - digit) / 10) return false;
        magnitude = magnitude * 10 + digit;
    }
    if (negative) {
        out = magnitude == uint64_t{INT64_MAX} + 1
            ? INT64_MIN : -static_cast<int64_t>(magnitude);
    } else {
        out = static_cast<int64_t>(magnitude);
    }
    return true;
}

std::string IoLoop::climon_string(Slice value) {
    return std::string(value.p, value.n);
}

bool IoLoop::climon_parse_client_type(Slice value, ClientTypeFilter& type) {
    if (value.eq_icase("normal")) type = ClientTypeFilter::Normal;
    else if (value.eq_icase("master")) type = ClientTypeFilter::Master;
    else if (value.eq_icase("replica") || value.eq_icase("slave"))
        type = ClientTypeFilter::Replica;
    else if (value.eq_icase("pubsub")) type = ClientTypeFilter::Pubsub;
    else return false;
    return true;
}

void IoLoop::climon_copy_client_filter(PubSubEvent& target, const PubSubEvent& source) {
    target.client_filter_mask = source.client_filter_mask;
    target.client_type = source.client_type;
    target.client_id = source.client_id;
    target.client_max_age = source.client_max_age;
    target.caller_id = source.caller_id;
    target.client_skipme = source.client_skipme;
    target.client_old_form = source.client_old_form;
    target.client_addr = source.client_addr;
    target.client_laddr = source.client_laddr;
    target.client_user = source.client_user;
    target.items = source.items;
}

// ---- lane state ----------------------------------------------------------------------------

void IoLoop::climon_refresh_armed() {
    const uint32_t before = climon_armed_cached_;
    climon_armed_cached_ = srv_->climon_armed();
    // parse_and_dispatch caches the armed word ONCE per pass (that is what makes it free when
    // off). A CLIENT subcommand that arms or disarms the lane therefore invalidates the cache of
    // the very pass it is running in -- a pipelined `CLIENT REPLY OFF; PING` would otherwise
    // answer the PING. Flag it; the cold dispatch branches end the pass so the next one re-reads.
    if (climon_armed_cached_ != before) climon_armed_dirty_ = true;
    notify_armed_ = notify_config_armed_ || save_config_armed_ ||
                    climon_armed_cached_ != 0;
    climon_pause_deadline_ms_ = srv_->climon_pause_end_ms();
    // A held connection parked with unparsed bytes, nothing to write and a quiesced ROB -- which
    // is exactly the shape flush_ready drops from the active set. Whoever ends the pause (the
    // UNPAUSE caller, or any owner whose clock passed the deadline) clears the global bit; EVERY
    // owner observes that here on its next batch and puts its own connections back.
    if ((before & Server::kClimonPause) && !(climon_armed_cached_ & Server::kClimonPause)) {
        for (Client* c : self_->clients())
            if (c && !c->dead()) { enqueue_serve(c); mark_active(c); }
    }
}

IoLoop::ClimonConn* IoLoop::climon_conn_find(uint64_t id) {
    auto found = climon_conn_.find(id);
    return found == climon_conn_.end() ? nullptr : &found->second;
}

IoLoop::ClimonConn& IoLoop::climon_conn_get(Client* client) {
    ClimonConn& state = climon_conn_[client->id()];
    state.client = client;
    return state;
}

// Drop the map entry once a connection is back to defaults, so a server that briefly used one of
// these features returns to holding nothing per connection.
void IoLoop::climon_conn_release(uint64_t id) {
    auto found = climon_conn_.find(id);
    if (found != climon_conn_.end() && found->second.idle()) climon_conn_.erase(found);
}

void IoLoop::climon_track_client(Client* client) {
    command_client_directory_add(client->id(), self_->id());
}

void IoLoop::climon_untrack_client(Client* client) {
    const uint64_t id = client->id();
    command_client_directory_remove(id);
    auto found = climon_conn_.find(id);
    if (found == climon_conn_.end()) return;
    ClimonConn& state = found->second;
    if (state.monitor) climon_monitor_stop(client, id);
    if (state.tracking_on || state.bcast) tracking_forget_client(id, state);
    if (state.reply_mode != kClimonReplyOn) {
        if (climon_local_reply_) climon_local_reply_--;
        srv_->climon_reply_removed();
    }
    climon_conn_.erase(id);
    climon_refresh_armed();
}

void IoLoop::climon_reset_client(Client* client) {
    auto found = climon_conn_.find(client->id());
    if (found == climon_conn_.end()) return;
    ClimonConn& state = found->second;
    if (state.monitor) climon_monitor_stop(client, client->id());
    if (state.tracking_on || state.bcast) tracking_forget_client(client->id(), state);
    if (state.reply_mode != kClimonReplyOn) {
        state.reply_mode = kClimonReplyOn;
        if (climon_local_reply_) climon_local_reply_--;
        srv_->climon_reply_removed();
    }
    state.broken_redirect = false;
    client->set_no_touch(false);
    climon_conn_release(client->id());
    climon_refresh_armed();
}

// ---- THE armed gate ------------------------------------------------------------------------
// The one per-operation entry point this lane owns. It is reached only from inside the branch
// parse_and_dispatch already takes for keyspace notifications, and only while at least one lane
// feature is armed. Returning true HOLDS the connection: the frame stays unparsed and is retried
// on a later pass (CLIENT PAUSE).

bool IoLoop::climon_armed_gate(Client* client, Op& op) {
    const uint32_t armed = climon_armed_cached_;
    // Keyspace notifications can be configured with the lane entirely idle. That case must not
    // walk any of the machinery below.
    if (__builtin_expect(armed == 0, true)) return false;

    if (armed & Server::kClimonPause) {
        if (climon_pause_holds(op)) {
            srv_->climon_note_pause_hold();
            return true;
        }
    }
    if (armed & Server::kClimonMonitor) climon_monitor_feed(client, op);
    if (armed & Server::kClimonTracking) {
        // A whole-keyspace flush is the one mutation with no per-key notification to ride, so it
        // is observed here, on the command that requests it. FLUSHALL is a scatter barrier: the
        // connection stalls behind it either way, so firing at dispatch cannot reorder anything
        // a client can see.
        if (__builtin_expect(op.cmd_name().eq_icase("flushall") ||
                             op.cmd_name().eq_icase("flushdb"), false)) {
            tracking_broadcast_flush();
        } else {
            ClimonConn* state = climon_conn_find(client->id());
            if (state && state->tracking_on) tracking_register_read(client, *state, op);
        }
    }
    if (armed & Server::kClimonReply) {
        ClimonConn* state = climon_conn_find(client->id());
        if (state) {
            if (state->reply_mode == kClimonReplyOff) {
                op.mark_reply_skip();
            } else if (state->reply_mode == kClimonReplySkipNext) {
                // SKIP covers the command AFTER `CLIENT REPLY SKIP` itself -- which is this one.
                // Marking the Op (not the connection) is what makes a pipelined
                // SKIP;PING;PING drop exactly one reply instead of the whole batch. The
                // connection stays counted as armed until the marked op has actually been
                // served, so the suppressing serve variant is still selected when it retires.
                op.mark_reply_skip();
                state->reply_mode = kClimonReplySkipNow;
            }
        }
    }
    return false;
}

// Mirrors pubsub_emit's ordering rule, and shares its machinery: an out-of-band frame is a WHOLE
// frame that must land on a frame boundary, never inside another reply's byte range. Client picks
// the output channel (see Client::append_oob); a frame raised from inside this connection's own
// retire drain -- which is where the tracking invalidation and MONITOR hooks fire, with the
// connection's newest reply only partially staged -- or behind an earlier-issued, unretired reply
// parks on the send engine until a drain stages that frontier. Only the connection's own io thread
// ever runs this.
void IoLoop::climon_push_wire(Client* client, const std::string& frame) {
    if (__builtin_expect(wb_.defer_oob(*client, frame.data(), frame.size()), false)) {
        srv_->note_oob_frame_deferred();
    } else if (__builtin_expect(client->append_oob(frame.data(), frame.size()), false)) {
        srv_->note_oob_frame_segmented();
    }
    enqueue_serve(client);
    mark_active(client);
}

bool IoLoop::climon_unblock_local(uint64_t id, bool error_flavor) {
    for (Client* c : self_->clients()) {
        if (!c || c->id() != id) continue;
        if (c->dead() || !c->blocked()) return false;
        if (!blocking_request_unblock(*c, error_flavor)) return false;
        if (blocking_cancel_client(*srv_, *self_, ring_, *c)) enqueue_serve(c);
        mark_active(c);
        return true;
    }
    return false;
}

// ---- CLIENT REPLY --------------------------------------------------------------------------

// Cheap owner-local gate: the suppressing serve is only worth entering while THIS connection is
// in a non-default CLIENT REPLY mode. One map probe on a connection whose owner already knows a
// reply mode exists somewhere.
bool IoLoop::climon_reply_suppressed(Client* client) {
    if (!climon_local_reply_) return false;
    const ClimonConn* state = climon_conn_find(client->id());
    return state && state->reply_mode != kClimonReplyOn;
}

uint32_t IoLoop::climon_serve_suppressed(Client* client) {
    const bool did = wb_.serve_suppressing(*client);
    ClimonConn* state = climon_conn_find(client->id());
    // A one-shot SKIP disarms once its marked op has actually retired -- not when it was marked,
    // or the suppressing variant would stop being selected before the reply reached the drain.
    if (state && state->reply_mode == kClimonReplySkipNow && client->rob().quiesced()) {
        state->reply_mode = kClimonReplyOn;
        if (climon_local_reply_) climon_local_reply_--;
        srv_->climon_reply_removed();
        climon_conn_release(client->id());
        climon_refresh_armed();
    }
    return did ? 1u : 0u;
}

// ---- CLIENT PAUSE --------------------------------------------------------------------------

bool IoLoop::climon_pause_holds(Op& op) {
    if (!climon_pause_deadline_ms_) return false;
    // Cheap monotonic re-check: the per-batch clock in run_loop already refreshed cached_now_ms_
    // while the pause bit is armed.
    if (cached_now_ms_ >= climon_pause_deadline_ms_) return false;
    const CommandSpec* spec = op.spec;
    if (!spec) return false;
    // DELIBERATE DIVERGENCE FROM REDIS, documented in NOTES-CLIMON2.md: redis postpones CLIENT
    // UNPAUSE itself under PAUSE ... ALL, so an ALL pause can only end by expiring. We exempt the
    // connection-control class (CLIENT/RESET/MONITOR, the CmdFlags::Climon rows) so UNPAUSE
    // always works. Everything else -- including PING and reads -- is held under ALL, matching
    // redis.
    if (spec->flags & CmdFlags::Climon) return false;
    if (srv_->climon_pause_mode() == Server::kPauseWrite)
        return (spec->flags & CmdFlags::Write) != 0;
    return true;
}

void IoLoop::climon_release_pause() {
    srv_->climon_expire_pause(cached_now_ms_);
    climon_refresh_armed();   // the 1->0 transition re-marks this owner's held connections
}

// ---- MONITOR -------------------------------------------------------------------------------

void IoLoop::climon_monitor_format(Client* client, Op& op, std::string& out) {
    // Redis format: "<sec>.<usec> [<db> <addr>] "CMD" "arg" ...", delivered as a simple status.
    struct timespec ts;
    ::clock_gettime(CLOCK_REALTIME, &ts);
    char head[96];
    const std::string addr = command_client_addr(client);
    std::snprintf(head, sizeof(head), "+%lld.%06lld [%u %s]",
                  static_cast<long long>(ts.tv_sec),
                  static_cast<long long>(ts.tv_nsec / 1000),
                  client->session().db_index, addr.c_str());
    out.assign(head);
    const bool redact = op.cmd_name().eq_icase("auth") ||
                        (op.cmd_name().eq_icase("hello") && op.argc() > 1);
    for (uint32_t i = 0; i < op.argc(); i++) {
        out.push_back(' ');
        // AUTH arguments and HELLO's AUTH tail carry credentials; redis prints them as "(redacted)".
        if (redact && i > 0 &&
            (op.cmd_name().eq_icase("auth") || op.arg(i - 1).eq_icase("auth") ||
             (i >= 2 && op.arg(i - 2).eq_icase("auth")))) {
            out += "\"(redacted)\"";
            continue;
        }
        climon_quote_arg(op.arg(i), out);
    }
    out += "\r\n";
}

void IoLoop::climon_monitor_feed(Client* client, Op& op) {
    // MONITOR itself is never fed (oracle-confirmed), but a monitor DOES see its own ordinary
    // traffic -- verified against redis 7.4, which emits the reply first and the feed line after.
    if (op.cmd_name().eq_icase("monitor")) return;
    std::string line;
    climon_monitor_format(client, op, line);
    srv_->climon_note_monitor_line();

    // Encode once, share the blob with every owner that actually has a monitor.
    auto blob = std::make_shared<const std::string>(std::move(line));
    if (climon_local_monitors_) climon_monitor_deliver(*blob);
    const uint64_t mask = srv_->climon_monitor_io_mask();
    for (uint32_t io : srv_->placement().ifid_threads()) {
        if (io == self_->id()) continue;
        if (!((mask >> (io & 63)) & 1)) continue;
        PubSubEvent* event = pubsub_new_event(PubSubEventKind::MonitorFeed);
        event->target_io = io;
        event->origin_io = self_->id();
        event->blob = blob;
        pubsub_post(io, event);
    }
}

void IoLoop::climon_monitor_deliver(const std::string& line) {
    for (auto& entry : climon_conn_) {
        ClimonConn& state = entry.second;
        if (!state.monitor || !state.client) continue;
        Client* target = state.client;
        if (target->dead() || target->closing()) continue;
        climon_push_wire(target, line);
    }
}

IoLoop::ClimonStartResult IoLoop::climon_start_monitor(Client* client, Op& op) {
    ClimonConn& state = climon_conn_get(client);
    if (state.monitor) {
        // A second MONITOR on a monitoring connection is silently ignored by redis -- no reply
        // at all, not an error. The op still retires; it simply carries no bytes.
        climon_conn_release(client->id());
        return ClimonStartResult::Sync;
    }
    state.monitor = true;
    climon_local_monitors_++;
    srv_->climon_set_monitor_io(self_->id(), true);
    srv_->climon_monitor_added();
    climon_refresh_armed();
    reply_ok(op.sink());
    return ClimonStartResult::Sync;
}

void IoLoop::climon_monitor_stop(Client* client, uint64_t id) {
    ClimonConn* state = climon_conn_find(id);
    if (!state || !state->monitor) return;
    (void)client;
    state->monitor = false;
    if (climon_local_monitors_) climon_local_monitors_--;
    if (!climon_local_monitors_) srv_->climon_set_monitor_io(self_->id(), false);
    srv_->climon_monitor_removed();
}

// ---- CLIENT subcommands owned by this file --------------------------------------------------

IoLoop::ClimonStartResult IoLoop::climon_start_client_unblock(Client* client, Op& op) {
    if (op.argc() != 3 && op.argc() != 4) {
        climon_wrong_args(op, "unblock");
        return ClimonStartResult::Sync;
    }
    int64_t id = 0;
    if (!climon_parse_i64(op.arg(2), id)) {
        reply_err(op.sink(), "ERR value is not an integer or out of range");
        return ClimonStartResult::Sync;
    }
    bool error_flavor = false;
    if (op.argc() == 4) {
        if (op.arg(3).eq_icase("timeout")) error_flavor = false;
        else if (op.arg(3).eq_icase("error")) error_flavor = true;
        else {
            reply_err(op.sink(), "ERR CLIENT UNBLOCK reason should be TIMEOUT or ERROR");
            return ClimonStartResult::Sync;
        }
    }
    uint32_t owner = 0;
    if (id <= 0 || !command_client_directory_find(static_cast<uint64_t>(id), owner)) {
        reply_int(op.sink(), 0);
        return ClimonStartResult::Sync;
    }
    // Same-owner is answered inline; a remote owner gets one request and replies once. The
    // blocked client is only ever touched by the thread that owns it.
    if (owner == self_->id()) {
        const bool unblocked = climon_unblock_local(static_cast<uint64_t>(id), error_flavor);
        reply_int(op.sink(), unblocked ? 1 : 0);
        return ClimonStartResult::Sync;
    }

    ClimonPending pending;
    pending.kind = ClimonPendingKind::ClientUnblock;
    pending.client = client;
    pending.op_id = client->rob().dispatch_id();
    pending.remaining = 1;
    if (!climon_pending_.emplace(client->id(), std::move(pending)).second) std::abort();

    srv_->client_scatter_started();
    client->rob().publish();
    // Released by the quiescence backstop in flush_ready when the remote owner's reply retires
    // this slot. Note this is CLIENT UNBLOCK, not CLIENT PAUSE -- PAUSE parks a connection through
    // climon_pause_holds() and never touches the barrier at all.
    barrier_arm(client, BarrierOwner::Climon);
    PubSubEvent* event = pubsub_new_event(PubSubEventKind::ClientUnblockRequest);
    event->target_io = owner;
    event->origin_io = self_->id();
    event->conn_id = client->id();
    event->op_id = client->rob().dispatch_id() - 1;
    event->client_id = static_cast<uint64_t>(id);
    event->subscribe = error_flavor;
    pubsub_post(owner, event);
    pubsub_drain_events();
    return ClimonStartResult::Async;
}

IoLoop::ClimonStartResult IoLoop::climon_start_client_list(Client* client, Op& op) {
    PubSubEvent filter;
    filter.caller_id = client->id();
    if (op.argc() == 2) {
        // No filter.
    } else if (op.argc() == 4 && op.arg(2).eq_icase("type")) {
        if (!climon_parse_client_type(op.arg(3), filter.client_type)) {
            std::string error = "ERR Unknown client type '";
            error.append(op.arg(3).p, op.arg(3).n);
            error.push_back('\'');
            reply_err(op.sink(), error.c_str());
            return ClimonStartResult::Sync;
        }
        filter.client_filter_mask |= ClientFilterType;
    } else if (op.argc() > 3 && op.arg(2).eq_icase("id")) {
        filter.client_filter_mask |= ClientFilterIdList;
        for (uint32_t arg = 3; arg < op.argc(); arg++) {
            int64_t id = 0;
            if (!climon_parse_i64(op.arg(arg), id)) {
                reply_err(op.sink(), "ERR Invalid client ID");
                return ClimonStartResult::Sync;
            }
            if (id >= 0)
                filter.items.push_back(PubSubEventItem{
                    0, false, static_cast<uint64_t>(id), {}});
        }
    } else {
        reply_syntax(op.sink());
        return ClimonStartResult::Sync;
    }
    return climon_begin_client_scatter(client, filter, ClimonPendingKind::ClientList);
}

IoLoop::ClimonStartResult IoLoop::climon_start_client_kill(Client* client, Op& op) {
    if (op.argc() == 2) {
        reply_err(op.sink(), "ERR wrong number of arguments for 'client|kill' command");
        return ClimonStartResult::Sync;
    }
    PubSubEvent filter;
    filter.caller_id = client->id();
    filter.client_skipme = true;
    if (op.argc() == 3) {
        filter.client_old_form = true;
        filter.client_skipme = false;
        filter.client_filter_mask = ClientFilterAddr;
        filter.client_addr = climon_string(op.arg(2));
    } else {
        for (uint32_t arg = 2; arg < op.argc();) {
            if (arg + 1 >= op.argc()) {
                reply_syntax(op.sink());
                return ClimonStartResult::Sync;
            }
            const Slice option = op.arg(arg);
            const Slice value = op.arg(arg + 1);
            if (option.eq_icase("id")) {
                int64_t id = 0;
                if (!climon_parse_i64(value, id) || id <= 0) {
                    reply_err(op.sink(), "ERR client-id should be greater than 0");
                    return ClimonStartResult::Sync;
                }
                filter.client_filter_mask |= ClientFilterId;
                filter.client_id = static_cast<uint64_t>(id);
            } else if (option.eq_icase("maxage")) {
                int64_t age = 0;
                if (!climon_parse_i64(value, age)) {
                    reply_err(op.sink(), "ERR maxage is not an integer or out of range");
                    return ClimonStartResult::Sync;
                }
                if (age <= 0) {
                    reply_err(op.sink(), "ERR maxage should be greater than 0");
                    return ClimonStartResult::Sync;
                }
                filter.client_filter_mask |= ClientFilterMaxAge;
                filter.client_max_age = static_cast<uint64_t>(age);
            } else if (option.eq_icase("type")) {
                if (!climon_parse_client_type(value, filter.client_type)) {
                    std::string error = "ERR Unknown client type '";
                    error.append(value.p, value.n);
                    error.push_back('\'');
                    reply_err(op.sink(), error.c_str());
                    return ClimonStartResult::Sync;
                }
                filter.client_filter_mask |= ClientFilterType;
            } else if (option.eq_icase("addr")) {
                filter.client_filter_mask |= ClientFilterAddr;
                filter.client_addr = climon_string(value);
            } else if (option.eq_icase("laddr")) {
                filter.client_filter_mask |= ClientFilterLaddr;
                filter.client_laddr = climon_string(value);
            } else if (option.eq_icase("user")) {
                uint32_t user_index = 0;
                if (!acl_find_user(value, user_index)) {
                    std::string error = "ERR No such user '";
                    error.append(value.p, value.n);
                    error.push_back('\'');
                    reply_err(op.sink(), error.c_str());
                    return ClimonStartResult::Sync;
                }
                (void)user_index;
                filter.client_filter_mask |= ClientFilterUser;
                filter.client_user = climon_string(value);
            } else if (option.eq_icase("skipme")) {
                if (value.eq_icase("yes")) filter.client_skipme = true;
                else if (value.eq_icase("no")) filter.client_skipme = false;
                else {
                    reply_syntax(op.sink());
                    return ClimonStartResult::Sync;
                }
            } else {
                reply_syntax(op.sink());
                return ClimonStartResult::Sync;
            }
            arg += 2;
        }
    }
    return climon_begin_client_scatter(client, filter, ClimonPendingKind::ClientKill);
}

IoLoop::ClimonStartResult IoLoop::climon_begin_client_scatter(
        Client* client, const PubSubEvent& filter, ClimonPendingKind kind) {
    const auto& ios = srv_->placement().ifid_threads();
    std::vector<PubSubEvent*> events;
    events.reserve(ios.size());
    for (uint32_t io : ios) {
        PubSubEvent* event = pubsub_new_event(
            kind == ClimonPendingKind::ClientList
                ? PubSubEventKind::ClientListRequest
                : PubSubEventKind::ClientKillRequest);
        event->target_io = io;
        climon_copy_client_filter(*event, filter);
        events.push_back(event);
    }

    ClimonPending pending;
    pending.kind = kind;
    pending.client = client;
    pending.op_id = client->rob().dispatch_id();
    pending.remaining = static_cast<uint32_t>(events.size());
    pending.old_form = filter.client_old_form;
    if (!climon_pending_.emplace(client->id(), std::move(pending)).second) std::abort();

    srv_->client_scatter_started();
    client->rob().publish();
    // CLIENT LIST / CLIENT KILL fan-out. Released by the quiescence backstop in flush_ready once
    // every io thread has answered and this slot retires.
    barrier_arm(client, BarrierOwner::Climon);
    for (PubSubEvent* event : events) {
        event->origin_io = self_->id();
        event->conn_id = client->id();
        event->op_id = client->rob().dispatch_id() - 1;
        pubsub_post(event->target_io, event);
    }
    pubsub_drain_events();
    return ClimonStartResult::Async;
}

IoLoop::ClimonStartResult IoLoop::climon_start_client_command(Client* client, Op& op) {
    if (op.cmd_name().eq_icase("monitor")) {
        if (op.argc() != 1) {
            reply_err(op.sink(), "ERR wrong number of arguments for 'monitor' command");
            return ClimonStartResult::Sync;
        }
        return climon_start_monitor(client, op);
    }

    const Slice sub = op.arg(1);
    if (sub.eq_icase("list")) return climon_start_client_list(client, op);
    if (sub.eq_icase("kill")) return climon_start_client_kill(client, op);
    if (sub.eq_icase("unblock")) return climon_start_client_unblock(client, op);

    if (sub.eq_icase("help")) {
        if (op.argc() != 2) { climon_wrong_args(op, "help"); return ClimonStartResult::Sync; }
        auto sink = op.sink();
        reply_array_header(sink, sizeof(kClimonHelp) / sizeof(kClimonHelp[0]));
        for (const char* line : kClimonHelp) reply_simple(sink, line);
        return ClimonStartResult::Sync;
    }

    if (sub.eq_icase("no-touch")) {
        if (op.argc() != 3) { climon_wrong_args(op, "no-touch"); return ClimonStartResult::Sync; }
        bool enabled = false;
        if (!climon_eq_on_off(op.arg(2), enabled)) {
            reply_syntax(op.sink());
            return ClimonStartResult::Sync;
        }
        // Two writes, both cold: the connection flag byte (which every Op captures for free, and
        // which the executor reads only when maxmemory is enabled) and the CLIENT INFO catalog.
        client->set_no_touch(enabled);
        command_set_local_context(client, self_);
        command_client_set_no_touch(client, enabled);
        command_set_local_context(nullptr, nullptr);
        reply_ok(op.sink());
        return ClimonStartResult::Sync;
    }

    if (sub.eq_icase("reply")) {
        if (op.argc() != 3) { climon_wrong_args(op, "reply"); return ClimonStartResult::Sync; }
        ClimonConn& state = climon_conn_get(client);
        const uint8_t before = state.reply_mode;
        if (op.arg(2).eq_icase("on")) {
            state.reply_mode = kClimonReplyOn;
            // CLIENT REPLY ON always answers, including the call that lifts OFF -- so undo the
            // mark the armed gate just made for this very op.
            op.clear_reply_skip();
            reply_ok(op.sink());
        } else if (op.arg(2).eq_icase("off")) {
            state.reply_mode = kClimonReplyOff;
        } else if (op.arg(2).eq_icase("skip")) {
            // SKIP does not suppress its own (absent) reply, and re-arming while OFF is a no-op.
            if (state.reply_mode != kClimonReplyOff) state.reply_mode = kClimonReplySkipNext;
        } else {
            reply_syntax(op.sink());
            return ClimonStartResult::Sync;
        }
        const bool was_armed = before != kClimonReplyOn;
        const bool now_armed = state.reply_mode != kClimonReplyOn;
        if (!was_armed && now_armed) { climon_local_reply_++; srv_->climon_reply_added(); }
        else if (was_armed && !now_armed) {
            if (climon_local_reply_) climon_local_reply_--;
            srv_->climon_reply_removed();
        }
        climon_conn_release(client->id());
        climon_refresh_armed();
        return ClimonStartResult::Sync;
    }

    if (sub.eq_icase("unpause")) {
        if (op.argc() != 2) { climon_wrong_args(op, "unpause"); return ClimonStartResult::Sync; }
        srv_->climon_clear_pause();
        climon_refresh_armed();
        climon_release_pause();
        reply_ok(op.sink());
        return ClimonStartResult::Sync;
    }

    if (sub.eq_icase("pause")) {
        if (op.argc() < 3) { climon_wrong_args(op, "pause"); return ClimonStartResult::Sync; }
        if (op.argc() > 4) {
            reply_err(op.sink(), "ERR unknown subcommand or wrong number of arguments for "
                                 "'PAUSE'. Try CLIENT HELP.");
            return ClimonStartResult::Sync;
        }
        int64_t timeout = 0;
        if (!climon_parse_i64(op.arg(2), timeout)) {
            reply_err(op.sink(), "ERR timeout is not an integer or out of range");
            return ClimonStartResult::Sync;
        }
        if (timeout < 0) {
            reply_err(op.sink(), "ERR timeout is negative");
            return ClimonStartResult::Sync;
        }
        uint8_t mode = Server::kPauseAll;
        if (op.argc() == 4) {
            if (op.arg(3).eq_icase("write")) mode = Server::kPauseWrite;
            else if (op.arg(3).eq_icase("all")) mode = Server::kPauseAll;
            else {
                reply_err(op.sink(), "ERR CLIENT PAUSE mode must be WRITE or ALL");
                return ClimonStartResult::Sync;
            }
        }
        const uint64_t now_ms = now_ns() / 1000000ull;
        srv_->climon_set_pause(now_ms + static_cast<uint64_t>(timeout), mode);
        climon_refresh_armed();
        reply_ok(op.sink());
        return ClimonStartResult::Sync;
    }

    const ClimonStartResult tracked = tracking_client_subcommand(client, op);
    if (tracked != ClimonStartResult::NotHandled) return tracked;

    // Ordinary CLIENT subcommands stay owner-local. Keeping this fallback in the cold object
    // prevents LIST/KILL support from changing parse_and_dispatch's code placement.
    command_set_local_context(client, self_);
    snapshot_bind_io(self_, &ring_);
    op.spec->handler(srv_->shard(0), op);
    snapshot_bind_io(nullptr, nullptr);
    command_set_local_context(nullptr, nullptr);
    return ClimonStartResult::Sync;
}

// ---- IO-to-IO transport ----------------------------------------------------------------------

void IoLoop::climon_handle_client_request(PubSubEvent& event) {
    srv_->client_scatter_io_replied();
    // CLIENT traffic is cold and needs a current age/MAXAGE snapshot even when timeout and
    // output-buffer cron are disabled. Each owner samples once for its entire local pass.
    cached_now_ms_ = now_ns() / 1000000ull;
    cached_now_s_ = static_cast<uint32_t>(cached_now_ms_ / 1000);
    const bool list = event.kind == PubSubEventKind::ClientListRequest;
    PubSubEvent* result = pubsub_new_event(
        list ? PubSubEventKind::ClientListResult : PubSubEventKind::ClientKillResult);
    result->conn_id = event.conn_id;
    result->op_id = event.op_id;
    std::vector<Client*> victims;
    for (Client* candidate : self_->clients()) {
        if (!candidate || !command_client_filter_match(*candidate, event, cached_now_ms_))
            continue;
        if (list) result->message += command_client_info_line(*candidate, cached_now_ms_);
        else victims.push_back(candidate);
    }
    if (!list) {
        result->count = victims.size();
        for (Client* victim : victims) {
            if (victim->id() == event.caller_id) {
                victim->mark_closing();
                mark_active(victim);
            } else {
                close_client(victim);
            }
        }
    }
    pubsub_post(event.origin_io, result);
}

void IoLoop::climon_finish_client_pending(uint64_t conn_id) {
    auto found = climon_pending_.find(conn_id);
    if (found == climon_pending_.end()) return;
    ClimonPending& pending = found->second;
    Client* client = pending.client;
    if (!client || client->dead()) { climon_pending_.erase(found); return; }
    Op& op = client->rob().at(pending.op_id);
    if (pending.kind == ClimonPendingKind::ClientList) {
        reply_verbatim(op.sink(), Slice(pending.body.data(), pending.body.size()),
                       "txt", op.resp3());
    } else if (pending.kind == ClimonPendingKind::ClientKill) {
        if (pending.old_form) {
            if (pending.count) reply_ok(op.sink());
            else reply_err(op.sink(), "ERR No such client");
        } else {
            reply_int(op.sink(), static_cast<long long>(pending.count));
        }
    } else {
        reply_int(op.sink(), static_cast<long long>(pending.count));
    }
    op.state.store(OpState::Done, std::memory_order_release);
    enqueue_serve(client);
    mark_active(client);
    climon_pending_.erase(found);
}

void IoLoop::climon_handle_client_result(PubSubEvent& event) {
    auto found = climon_pending_.find(event.conn_id);
    if (found == climon_pending_.end() || found->second.op_id != event.op_id) return;
    ClimonPending& pending = found->second;
    if (event.kind == PubSubEventKind::ClientListResult) pending.body += event.message;
    else pending.count += event.count;
    if (!pending.remaining) std::abort();
    if (--pending.remaining == 0) climon_finish_client_pending(event.conn_id);
}

bool IoLoop::climon_handle_event(PubSubEvent& event) {
    switch (event.kind) {
        case PubSubEventKind::ClientListRequest:
        case PubSubEventKind::ClientKillRequest:
            climon_handle_client_request(event);
            return true;
        case PubSubEventKind::ClientListResult:
        case PubSubEventKind::ClientKillResult:
        case PubSubEventKind::ClientUnblockResult:
            climon_handle_client_result(event);
            return true;
        case PubSubEventKind::ClientUnblockRequest: {
            PubSubEvent* result = pubsub_new_event(PubSubEventKind::ClientUnblockResult);
            result->conn_id = event.conn_id;
            result->op_id = event.op_id;
            result->count = climon_unblock_local(event.client_id, event.subscribe) ? 1 : 0;
            pubsub_post(event.origin_io, result);
            return true;
        }
        case PubSubEventKind::MonitorFeed:
            if (event.blob) climon_monitor_deliver(*event.blob);
            return true;
        case PubSubEventKind::TrackingInvalidate:
        case PubSubEventKind::TrackingFlush:
        case PubSubEventKind::TrackingDeliver:
            tracking_handle_event(event);
            return true;
        default:
            return false;
    }
}

}  // namespace tomo

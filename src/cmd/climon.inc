// climon.inc -- declarations for the IO-owned CLIENT/MONITOR/TRACKING transport.
//
// Included inside IoLoop. Implementations live in climon.cc (connection control, MONITOR) and
// tracking.cc (client-side caching), so adding cold CLIENT catalog work cannot move or perturb
// the hot parse/dispatch code emitted from main.cc.
//
// ZERO-COST CONTRACT. Nothing declared here is read on an unarmed path. parse_and_dispatch makes
// exactly ONE feature decision -- the pre-existing notification-armed test -- and this lane rides
// it: srv_->climon_armed() folds into notify_armed_ once per io batch, and every per-operation
// hook below sits behind that same already-taken branch, out of line in a cold translation unit.

    enum class ClimonStartResult : uint8_t { NotHandled, Sync, Async };
    enum class ClimonPendingKind : uint8_t { ClientList, ClientKill, ClientUnblock };

    struct ClimonPending {
        ClimonPendingKind kind = ClimonPendingKind::ClientList;
        Client* client = nullptr;
        uint64_t op_id = 0;
        uint32_t remaining = 0;
        uint64_t count = 0;
        bool old_form = false;
        std::string body;
    };

    // CLIENT REPLY modes, matching redis semantics: ON restores replies, OFF suppresses until ON,
    // SKIP suppresses exactly the NEXT command's reply (and not its own).
    enum : uint8_t { kClimonReplyOn = 0, kClimonReplyOff = 1, kClimonReplySkipNext = 2,
                     kClimonReplySkipNow = 3 };

    // Per-connection lane state. Deliberately NOT in Client: Client is footprint-locked at 1984
    // bytes and its io-hot head is layout-tuned, and an entry is created only for a connection
    // that actually uses one of these features. An idle server holds an empty map.
    struct ClimonConn {
        Client* client = nullptr;
        uint8_t reply_mode = kClimonReplyOn;
        bool monitor = false;
        bool tracking_on = false;
        bool bcast = false;
        bool optin = false;
        bool optout = false;
        bool noloop = false;
        bool caching_armed = false;   // CLIENT CACHING yes|no consumed by the next command
        bool broken_redirect = false;
        uint64_t redirect = 0;
        std::vector<std::string> prefixes;

        bool idle() const {
            return reply_mode == kClimonReplyOn && !monitor && !tracking_on && !broken_redirect;
        }
    };

    static bool climon_parse_i64(Slice value, int64_t& out);
    static std::string climon_string(Slice value);
    static bool climon_parse_client_type(Slice value, ClientTypeFilter& type);
    static void climon_copy_client_filter(PubSubEvent& target, const PubSubEvent& source);

    ClimonStartResult climon_begin_client_scatter(
        Client* client, const PubSubEvent& filter, ClimonPendingKind kind);
    ClimonStartResult climon_start_client_list(Client* client, Op& op);
    ClimonStartResult climon_start_client_kill(Client* client, Op& op);
    ClimonStartResult climon_start_client_unblock(Client* client, Op& op);
    ClimonStartResult climon_start_client_command(Client* client, Op& op);
    void climon_handle_client_request(PubSubEvent& event);
    void climon_finish_client_pending(uint64_t conn_id);
    void climon_handle_client_result(PubSubEvent& event);
    bool climon_handle_event(PubSubEvent& event);

    // ---- the single armed hook -----------------------------------------------------------------
    // Called from parse_and_dispatch ONLY from inside the pre-existing notify-armed branch.
    // Returns true to HOLD the connection (CLIENT PAUSE): the frame stays unparsed and is retried.
    bool climon_armed_gate(Client* client, Op& op);
    // Per-io-batch refresh of the cached armed word + pause deadline. Per batch is free.
    void climon_refresh_armed();

    ClimonConn* climon_conn_find(uint64_t id);
    ClimonConn& climon_conn_get(Client* client);
    void climon_conn_release(uint64_t id);
    void climon_track_client(Client* client);
    void climon_untrack_client(Client* client);
    void climon_reset_client(Client* client, Op& op);
    bool climon_migration_ready(const Client* client) const;
    bool client_routing_prepare(ClientMigration& migration, std::string& error);
    void client_routing_commit_extract(ClientMigration& migration,
                                       std::vector<PubSubEvent*>& rebind_events);
    bool client_routing_install(void* opaque, Client* client, uint32_t source);
    void client_routing_discard(void* opaque);
    void client_routing_installed(uint64_t client_id);
    void client_routing_cleanup_pass();
    void client_routing_refresh_masks();
    void client_routing_forward_monitor(const PubSubEvent& event);

    // ---- CLIENT REPLY ---------------------------------------------------------------------------
    bool climon_reply_suppressed(Client* client);
    uint32_t climon_serve_suppressed(Client* client);
    uint32_t climon_prepare_suppressed(Client* client, bool& submit_allowed);

    // Append an out-of-band frame to a connection this owner owns, using the same ROB-tail rule
    // pub/sub delivery uses so a push can never overtake a command already parsed ahead of it.
    void climon_push_wire(Client* client, const std::string& frame);

    // ---- CLIENT UNBLOCK -------------------------------------------------------------------------
    bool climon_unblock_local(uint64_t id, bool error_flavor);

    // ---- CLIENT PAUSE ---------------------------------------------------------------------------
    bool climon_pause_holds(Op& op);
    bool climon_pause_armed() const { return (climon_armed_cached_ & Server::kClimonPause) != 0; }
    void climon_release_pause();

    // ---- MONITOR --------------------------------------------------------------------------------
    void climon_monitor_feed(Client* client, Op& op);
    void climon_monitor_format(Client* client, Op& op, std::string& out);
    void climon_monitor_deliver(const std::string& line);
    ClimonStartResult climon_start_monitor(Client* client, Op& op);
    void climon_monitor_stop(Client* client, uint64_t id);

    // ---- CLIENT TRACKING (tracking.cc) ----------------------------------------------------------
    void tracking_reply_trackinginfo(Client* client, Op& op);
    ClimonStartResult tracking_client_subcommand(Client* client, Op& op);
    void tracking_register_read(Client* client, ClimonConn& state, Op& op);
    void tracking_forget_client(uint64_t id, ClimonConn& state);
    void tracking_invalidate_local(Slice key, uint64_t writer_id);
    void tracking_handle_event(PubSubEvent& event);
    void tracking_broadcast_keys(const std::vector<std::string>& keys, uint64_t writer_id);
    void tracking_broadcast_flush();
    void tracking_deliver_frame(ClimonConn& state, uint64_t owner_id, Slice key, bool flush);
    void tracking_emit_invalidation(Client* target, bool redirected, Slice key, bool flush);
    void tracking_note_prefix_registered(const std::string& prefix, bool added);
    void tracking_migration_snapshot(uint64_t id, std::vector<std::string>& keys) const;
    void tracking_migration_extract(uint64_t id, std::vector<std::string>& keys);
    bool tracking_migration_install(uint64_t id, const std::vector<std::string>& keys);
    void tracking_forward_stale(const PubSubEvent& event);

    // ---- lane state (true cold tail of IoLoop) --------------------------------------------------
    std::unordered_map<uint64_t, ClimonPending> climon_pending_;
    std::unordered_map<uint64_t, ClimonConn> climon_conn_;
    // key -> ids of THIS owner's tracking clients. Single-owner: only this io thread reads or
    // writes it, so no lock sits on any armed path.
    std::unordered_map<std::string, std::vector<uint64_t>> climon_track_keys_;
    uint32_t climon_armed_cached_ = 0;
    bool climon_armed_dirty_ = false;   // a cold command changed the armed word mid-pass
    uint64_t climon_pause_deadline_ms_ = 0;
    uint32_t climon_local_monitors_ = 0;
    uint32_t climon_local_trackers_ = 0;
    uint32_t climon_local_reply_ = 0;
    uint64_t climon_monitor_clock_ms_ = 0;

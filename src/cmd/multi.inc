// multi.inc -- MULTI/EXEC/DISCARD/WATCH/UNWATCH on the epoch-MVCC seam.
//
// This file is textually included by xshard.cc.  It shares the command-image helpers in the same
// anonymous namespace and deliberately creates no extra translation unit.

namespace tomo {

struct MultiQueuedCommand {
    const CommandSpec* spec = nullptr;
    std::vector<std::string> argv;
};

struct MultiWatchedKey {
    std::string key;
    uint64_t hash = 0;
    int32_t shard = -1;
};

enum class MultiCommandKind : uint8_t {
    Local,
    Mget,
    Mset,
    Msetnx,
    Del,
    Exists,
    Ping,
    Echo,
    UnwatchReply,
    IoLocal,
    Scan,
    Xshard,
    Prebuilt,
    Unsupported,
};

enum class MultiBlockingPop : uint8_t {
    None, Blpop, Brpop, Blmpop, Bzpopmin, Bzpopmax, Bzmpop
};

struct MultiValue {
    bool present = false;
    std::string value;
};

struct MultiCommand {
    static constexpr uint32_t kXshardStages = 4;
    static constexpr uint32_t kShardMaskWords = 4;

    MultiQueuedCommand queued;
    std::unique_ptr<Op> op;
    MultiCommandKind kind = MultiCommandKind::Unsupported;
    std::vector<uint32_t> key_args;
    std::vector<int32_t> key_shards;
    std::vector<int32_t> participants;
    std::vector<uint8_t> shard_done;
    std::vector<MultiValue> values;
    // Blocking collection commands are executed inside EXEC through a nonblocking LMPOP/ZMPOP
    // child. The normalized argv owns the slices installed in `op`; it allocates only for these
    // transaction-only slow-path commands.
    std::vector<std::string> exec_argv;
    MultiBlockingPop blocking_pop = MultiBlockingPop::None;
    uint8_t pop_reply_shape = static_cast<uint8_t>(PopReplyShape::Mpop);
    std::atomic<uint32_t> pending{0};
    std::atomic<uint32_t> stage{0};
    std::atomic<long long> integer{0};
    std::atomic<bool> condition_failed{false};
    std::atomic<bool> finished{false};
    ScatterState* scatter = nullptr;
    // ScatterState::groups is rebuilt between an xshard child's waves.  A barrier-only shard must
    // decide whether it belongs to the PUBLISHED wave without consulting that mutable table: the
    // last task from the prior wave is allowed to rewrite it before publishing command.stage.
    // Four 64-bit words cover the server's 256-shard maximum.  Four stages cover the longest child
    // today (SORT source -> dynamic gather -> STORE apply) with one spare stage for a future retry.
    std::array<std::array<uint64_t, kShardMaskWords>, kXshardStages> xshard_stage_groups{};
    uint32_t notify_order_base = 0;
};

enum class MultiStateKind : uint8_t { Execute, WatchAdd, Unwatch, Close };
enum class MultiFinalReply : uint8_t { Exec, Ok, ExecAbort };

struct MultiWriteKey {
    std::string key;
    uint64_t hash = 0;
    int32_t shard = -1;
};

struct MultiWriteKeyHash {
    size_t operator()(const MultiWriteKey& key) const {
        return static_cast<size_t>(key.hash ^
            (static_cast<uint64_t>(static_cast<uint32_t>(key.shard)) *
             0x9e3779b97f4a7c15ULL));
    }
};

struct MultiWriteKeyEqual {
    bool operator()(const MultiWriteKey& left, const MultiWriteKey& right) const {
        return left.shard == right.shard && left.key == right.key;
    }
};

struct MultiSession {
    bool active = false;
    bool queue_error = false;
    uint64_t next_queue_op_id = 0;
    std::vector<MultiQueuedCommand> queue;
    std::vector<MultiWatchedKey> watched;
    MultiExecState* pending = nullptr;       // prepared, but not yet published to owner queues
};

struct MultiOwnerRecordRefs {
    // `nodes` is also the pointer retained by every AtomicEntry installed on this executor. Writer
    // owners begin at one so cleanup cannot release the state's pre-count before the owner has run
    // all of its outer shard tasks through the transaction's final decision.
    uint32_t nodes = 0;
    uint32_t remaining = 0;
    bool completion_sentinel = false;
};

// EXEC MUST NEVER BE WEAKER THAN THE SAME COMMANDS RUN BARE.  A bare cross-shard read pins a read
// cut, so a transaction that carries one has to pin a cut too; SnapshotCut is the shared header
// that lets it do so through the connection-owning IO thread's ScatterArenaPool, which is the one
// object allowed to write that thread's published cleanup floor (see xshard.h).
struct alignas(8) MultiExecState : SnapshotCut {
    ~MultiExecState() { notify_discard_batch(notify.exchange(nullptr)); }
    MultiStateKind kind = MultiStateKind::Execute;
    MultiFinalReply final_reply = MultiFinalReply::Exec;
    Server* server = nullptr;
    Client* client = nullptr;
    uint32_t owner_io = 0;
    uint64_t origin_conn_id = 0;
    uint64_t watch_generation = 0;
    uint64_t admission_generation = 0;
    bool admitted = false;
    bool reset_watch = false;
    bool published = false;
    bool retired = false;
    // Set at prepare time when the transaction carries at least one READ whose fragments land on
    // more than one owner -- the only shape that can straddle a foreign ticket, and therefore the
    // only shape that pays for a cut.  A write-only or single-owner transaction registers nothing.
    bool exec_read_fanout = false;
    // THE TRANSACTION'S EXPIRY CUT, the time-axis twin of the read cut above.  Absolute
    // CLOCK_REALTIME milliseconds, stamped once before any fragment is posted and installed on
    // every owner that runs part of this transaction.  Redis reaches the same place from the other
    // direction: EXEC is the top-level call, the queued commands run nested inside it, and
    // server.mstime is only refreshed at nesting depth zero -- so a key cannot expire part-way
    // through a transaction.  Unconditional: a single-owner transaction spans several tasks too,
    // and its clock moved between them just as readily.
    int64_t now_cut_ms = 0;
    // TEST HOOK (DEBUG ATOMIC-FANOUT-DEFER), the MULTI half.  Deadline and lead shard are written
    // once on the owning IO thread before the tasks are posted and read by the owners through the
    // task handoff's release/acquire pair.  Zero in production.
    uint64_t debug_fanout_deadline = 0;
    int32_t debug_fanout_lead = -1;
    std::vector<std::unique_ptr<MultiCommand>> commands;
    std::vector<MultiWatchedKey> watched;
    std::vector<MultiWriteKey> write_keys;
    std::unordered_set<MultiWriteKey, MultiWriteKeyHash, MultiWriteKeyEqual> write_key_set;
    std::vector<int32_t> shards;
    // IO builds these in transaction order. Each cursor has exactly one shard-owner writer.
    std::vector<std::vector<uint32_t>> shard_commands;
    std::vector<uint32_t> shard_cursors;
    std::vector<uint8_t> watch_shard_done;
    std::vector<uint8_t> execution_joined;
    std::vector<uint8_t> final_shard_done;
    std::vector<uint8_t> aof_shard_done;
    std::vector<uint8_t> execution_noted;
    std::vector<MultiOwnerRecordRefs> owner_record_refs;
    ScatterArenaPool scatter_pool;
    std::atomic<uint64_t> epoch{0};
    std::atomic<uint32_t> record_refs{0};
    bool record_refs_precounted = false;
    std::atomic<uint32_t> watch_refs{0};
    std::atomic<bool> aborted{false};
    std::atomic<bool> watch_failed{false};
    std::atomic<bool> watch_phase_done{false};
    std::atomic<uint32_t> watch_pending{0};
    std::atomic<uint32_t> lists_pending{0};
    std::atomic<bool> finalizer_claimed{false};
    std::atomic<bool> finalized{false};
    // Open between admission and the finalizer that decides this transaction on every owner. EXEC
    // force-admits a group at BOTH atomic settings, so this is the arm that keeps a plain-default
    // server's transactions out of a half-applied snapshot.
    std::atomic<bool> apply_open{false};
    std::atomic<uint32_t> tasks_pending{0};
    std::atomic<NotifyBatch*> notify{nullptr};
    std::shared_ptr<AofGroupDecision> aof_group;
};

namespace {

constexpr uintptr_t kMultiTaskTag = 1;

bool multi_name(const Op& op, const char* lower) { return op.cmd_name().eq_icase(lower); }

MultiSession* ensure_multi_session(Client& client) {
    if (client.multi_session()) return client.multi_session();
    auto* session = new (std::nothrow) MultiSession;
    if (session) client.set_multi_session(session);
    return session;
}

void release_admission(MultiExecState& state) {
    if (state.server)
        state.server->atomic_apply_close(state.owner_io, state.apply_open);
    if (!state.admitted) return;
    state.server->atomic_retire_group(state.owner_io, state.admission_generation);
    state.admitted = false;
}

// Does `group_epoch` name a word THIS transaction publishes through? A transaction is one logical
// unit spread over several epoch words: the plain per-key installs publish through `state.epoch`,
// and every cross-shard command lowered inside the same EXEC keeps its own ScatterState and
// publishes through that one. Anything asking "is there an undecided record here that is not mine"
// has to accept all of them, or the transaction answers yes against its own RENAME/LMPOP child.
// `commands` and their scatter pointers are written by IO in prepare_commands() before the first
// fragment is posted, and cleared only by destroy_multi_children() at retirement, which cannot run
// while a fragment is still executing -- so this walk is stable on an owner thread.
bool multi_state_owns_epoch(const MultiExecState& state, const void* group_epoch) {
    if (!group_epoch) return false;
    if (group_epoch == static_cast<const void*>(&state.epoch)) return true;
    for (const auto& command : state.commands)
        if (command && command->scatter &&
            group_epoch == static_cast<const void*>(&command->scatter->epoch))
            return true;
    return false;
}

bool multi_records_done(const MultiExecState& state) {
    if (state.record_refs.load(std::memory_order_acquire) != 0 ||
        state.watch_refs.load(std::memory_order_acquire) != 0) return false;
    for (const auto& command : state.commands)
        if (command->scatter && !xshard_multi_child_records_done(*command->scatter)) return false;
    return true;
}

void destroy_multi_children(MultiExecState& state) {
    for (const auto& command : state.commands) {
        if (!command->scatter) continue;
        xshard_destroy(command->scatter, state.scatter_pool, state.owner_io);
        command->scatter = nullptr;
    }
    state.scatter_pool.reap_deferred();
}

void destroy_multi_state(MultiExecState* state) {
    if (!state) return;
    if (__builtin_expect(atomic_tripwire_enabled(), false))
        atomic_tripwire_forget_scope(state);
    if (!multi_records_done(*state)) std::abort();
    // The cut bracket is register-at-dispatch / unregister-at-retire, both on the connection's own
    // IO thread. Reaching destruction still holding it would mean this thread's published cleanup
    // floor is pinned to a dead transaction, so cleanup would stop advancing server-wide. That is
    // an engine invariant, not an input condition; make it loud rather than silently degrading.
    if (state->snapshot_registered) std::abort();
    destroy_multi_children(*state);
    release_admission(*state);
    delete state;
}

bool command_forbidden_in_multi(const Op& op) {
    // Subscription controls need the IO-owned async home protocol, which cannot run from an EXEC
    // child. Reject all six controls uniformly; this is intentionally stricter than Redis for the
    // four regular controls and matches it for SSUBSCRIBE/SUNSUBSCRIBE.
    const bool config_mutation = multi_name(op, "config") && op.argc() > 1 &&
                                 !op.arg(1).eq_icase("get");
    return (op.spec->flags & CmdFlags::NoMulti) ||
           multi_name(op, "save") || multi_name(op, "flushall") ||
           multi_name(op, "flushdb") || config_mutation || multi_name(op, "randomkey") ||
           multi_name(op, "spop") || multi_name(op, "srandmember") ||
           multi_name(op, "zrandmember") || multi_name(op, "hrandfield") ||
           multi_name(op, "subscribe") || multi_name(op, "unsubscribe") ||
           multi_name(op, "psubscribe") || multi_name(op, "punsubscribe") ||
           multi_name(op, "ssubscribe") || multi_name(op, "sunsubscribe") ||
           multi_name(op, "acl");
}

bool queued_copy(const Op& op, MultiQueuedCommand& out) {
    out.spec = op.spec;
    try {
        out.argv.reserve(op.argc());
        for (uint32_t i = 0; i < op.argc(); i++)
            out.argv.emplace_back(op.arg(i).p, op.arg(i).n);
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

bool has_name(const MultiQueuedCommand& queued, const char* lower) {
    return Slice(queued.argv[0].data(), static_cast<uint32_t>(queued.argv[0].size()))
        .eq_icase(lower);
}

MultiBlockingPop multi_blocking_pop(const MultiQueuedCommand& queued) {
    if (has_name(queued, "blpop")) return MultiBlockingPop::Blpop;
    if (has_name(queued, "brpop")) return MultiBlockingPop::Brpop;
    if (has_name(queued, "blmpop")) return MultiBlockingPop::Blmpop;
    if (has_name(queued, "bzpopmin")) return MultiBlockingPop::Bzpopmin;
    if (has_name(queued, "bzpopmax")) return MultiBlockingPop::Bzpopmax;
    if (has_name(queued, "bzmpop")) return MultiBlockingPop::Bzmpop;
    return MultiBlockingPop::None;
}

void command_key_args(const MultiQueuedCommand& queued, std::vector<uint32_t>& args) {
    const CommandSpec& spec = *queued.spec;
    if (spec.flags & CmdFlags::StreamRoute) {
        uint32_t streams = 1;
        while (streams < queued.argv.size() &&
               !Slice(queued.argv[streams].data(), queued.argv[streams].size()).eq_icase("streams"))
            streams++;
        if (streams == queued.argv.size() || ((queued.argv.size() - streams - 1) & 1u)) return;
        const uint32_t count = static_cast<uint32_t>((queued.argv.size() - streams - 1) / 2);
        for (uint32_t arg = streams + 1; arg < streams + 1 + count; arg++) args.push_back(arg);
        return;
    }
    const MultiBlockingPop blocking = multi_blocking_pop(queued);
    if (blocking == MultiBlockingPop::Blpop || blocking == MultiBlockingPop::Brpop ||
        blocking == MultiBlockingPop::Bzpopmin || blocking == MultiBlockingPop::Bzpopmax) {
        for (uint32_t arg = 1; arg + 1 < queued.argv.size(); arg++) args.push_back(arg);
        return;
    }
    if (blocking == MultiBlockingPop::Blmpop || blocking == MultiBlockingPop::Bzmpop) {
        int64_t parsed = 0;
        const Slice count(queued.argv[2].data(), static_cast<uint32_t>(queued.argv[2].size()));
        if (!parse_i64(count, parsed) || parsed < 1 ||
            static_cast<uint64_t>(parsed) > UINT32_MAX ||
            3 + static_cast<uint64_t>(parsed) >= queued.argv.size()) return;
        for (uint32_t arg = 3; arg < 3 + static_cast<uint32_t>(parsed); arg++)
            args.push_back(arg);
        return;
    }
    if (spec.first_key <= 0 || spec.key_step <= 0) return;
    const uint32_t last = spec.last_key < 0
        ? static_cast<uint32_t>(queued.argv.size() - 1)
        : std::min<uint32_t>(static_cast<uint32_t>(spec.last_key),
                             static_cast<uint32_t>(queued.argv.size() - 1));
    for (uint32_t arg = static_cast<uint32_t>(spec.first_key); arg <= last;
         arg += static_cast<uint32_t>(spec.key_step))
        args.push_back(arg);
    // SORT'S STORE DESTINATION IS AN OPTION WORD, NOT PART OF ITS REGISTRY KEY RANGE (1..1), so the
    // loop above misses it. A queued `SORT src STORE dst` therefore looked like a ONE-key command:
    // classify_multi_command() saw a single shard, called it Local, and EXEC ran the same-owner arm
    // on the SOURCE's shard -- which installed `dst` in the source's table. The key was then
    // invisible to every routed lookup (EXISTS/TYPE/LRANGE answer on dst's real owner) while KEYS
    // and DBSIZE still counted it, and a snapshot would have persisted it under the wrong shard.
    // Naming the destination here restores the ordinary lowering: same shard stays Local and is
    // correct; different shards fall through to xshard_prepare's two-hop apply. It also puts the
    // destination into add_write_key(), so WATCH on it now sees the write.
    // SORT_RO is excluded by name: STORE is a syntax error there, so it names no destination.
    if (has_name(queued, "sort")) {
        uint32_t store_arg = 0;
        for (uint32_t i = 2; i < queued.argv.size(); i++) {
            const Slice word(queued.argv[i].data(),
                             static_cast<uint32_t>(queued.argv[i].size()));
            // Mirror parse_sort_options' grammar exactly, because a BY or GET pattern may itself
            // be the word STORE and must not be mistaken for the option.
            if (word.eq_icase("limit") && i + 2 < queued.argv.size()) i += 2;
            else if ((word.eq_icase("by") || word.eq_icase("get")) && i + 1 < queued.argv.size()) i++;
            else if (word.eq_icase("store") && i + 1 < queued.argv.size()) store_arg = ++i;
        }
        if (store_arg) args.push_back(store_arg);   // last STORE wins, as the parser records it
    }
}

// True when a queued SORT will READ keys derived from its elements. Such a command touches shards
// that no key of the transaction names, so it cannot be executed as an ordinary one-key Local child:
// see the participation note in prepare_commands().
bool multi_sort_dereferences(const MultiQueuedCommand& queued) {
    if (!has_name(queued, "sort") && !has_name(queued, "sort_ro")) return false;
    const uint32_t argc = static_cast<uint32_t>(queued.argv.size());
    for (uint32_t i = 2; i < argc; i++) {
        const Slice word(queued.argv[i].data(), static_cast<uint32_t>(queued.argv[i].size()));
        // Mirror parse_sort_options' grammar so an option word appearing INSIDE a pattern is not
        // mistaken for the option itself.
        if (word.eq_icase("limit") && i + 2 < argc) { i += 2; continue; }
        if (!(word.eq_icase("by") || word.eq_icase("get")) || i + 1 >= argc) continue;
        const std::string& pattern = queued.argv[++i];
        // `#` is the element and a pattern with no '*' is a constant nil: neither reads a key.
        const bool self = word.eq_icase("get") && pattern.size() == 1 && pattern[0] == '#';
        if (!self && pattern.find('*') != std::string::npos) return true;
    }
    return false;
}

bool contains_shard(const std::vector<int32_t>& shards, int32_t shard) {
    return std::find(shards.begin(), shards.end(), shard) != shards.end();
}

void add_shard(std::vector<int32_t>& shards, int32_t shard) {
    if (!contains_shard(shards, shard)) shards.push_back(shard);
}

bool multi_xshard_stage_has_group(const MultiCommand& command, uint32_t stage, int32_t shard) {
    if (stage >= MultiCommand::kXshardStages || shard < 0 || shard >= 256) std::abort();
    const uint32_t sid = static_cast<uint32_t>(shard);
    return command.xshard_stage_groups[stage][sid >> 6] & (1ull << (sid & 63));
}

bool make_child_op(MultiCommand& command) {
    command.op = std::make_unique<Op>();
    command.op->reset();
    command.op->spec = command.queued.spec;
    for (const std::string& arg : command.queued.argv)
        if (!command.op->push_arg(Slice(arg.data(), static_cast<uint32_t>(arg.size()))))
            return false;
    return true;
}

bool validate_multi_blocking_timeout(Op& op, MultiBlockingPop kind) {
    const uint32_t timeout_arg = (kind == MultiBlockingPop::Blmpop ||
                                  kind == MultiBlockingPop::Bzmpop)
        ? 1 : op.argc() - 1;
    int64_t timeout_ms = 0;
    if (blocking_reply_timeout_error(op, parse_timeout_ms(op.arg(timeout_arg), timeout_ms)))
        return false;
    if (timeout_ms != 0) {
        timespec now{};
        ::clock_gettime(CLOCK_REALTIME, &now);
        const int64_t now_ms = static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
        if (timeout_ms > INT64_MAX - now_ms) {
            reply_err(op.sink(), "ERR timeout is out of range");
            return false;
        }
    }
    return true;
}

bool normalize_multi_blocking_pop(MultiCommand& command) {
    const MultiBlockingPop kind = command.blocking_pop;
    const bool list = kind == MultiBlockingPop::Blpop || kind == MultiBlockingPop::Brpop ||
                      kind == MultiBlockingPop::Blmpop;
    const bool single = kind == MultiBlockingPop::Blpop || kind == MultiBlockingPop::Brpop ||
                        kind == MultiBlockingPop::Bzpopmin || kind == MultiBlockingPop::Bzpopmax;
    const char* alias = list ? "LMPOP" : "ZMPOP";
    const CommandSpec* spec = command_lookup(Slice(alias, 5));
    if (!spec) return false;
    if (command.queued.spec->flags & CmdFlags::NotifySelected)
        spec = command_notify_variant(spec);

    command.exec_argv.clear();
    if (single) {
        command.exec_argv.reserve(command.queued.argv.size() + 3);
        command.exec_argv.emplace_back(alias);
        command.exec_argv.push_back(std::to_string(command.queued.argv.size() - 2));
        for (uint32_t i = 1; i + 1 < command.queued.argv.size(); i++)
            command.exec_argv.push_back(command.queued.argv[i]);
        const bool right_or_max = kind == MultiBlockingPop::Brpop ||
                                  kind == MultiBlockingPop::Bzpopmax;
        command.exec_argv.emplace_back(list ? (right_or_max ? "RIGHT" : "LEFT")
                                            : (right_or_max ? "MAX" : "MIN"));
        command.exec_argv.emplace_back("COUNT");
        command.exec_argv.emplace_back("1");
        command.pop_reply_shape = static_cast<uint8_t>(
            list ? PopReplyShape::ListPair : PopReplyShape::ZsetTriple);
    } else {
        command.exec_argv.reserve(command.queued.argv.size() - 1);
        command.exec_argv.emplace_back(alias);
        for (uint32_t i = 2; i < command.queued.argv.size(); i++)
            command.exec_argv.push_back(command.queued.argv[i]);
        command.pop_reply_shape = static_cast<uint8_t>(PopReplyShape::Mpop);
    }

    command.op->reset();
    command.op->spec = spec;
    for (const std::string& arg : command.exec_argv)
        if (!command.op->push_arg(Slice(arg.data(), static_cast<uint32_t>(arg.size()))))
            return false;
    return true;
}

MultiCommandKind classify_multi_command(MultiCommand& command) {
    if (has_name(command.queued, "ping")) return MultiCommandKind::Ping;
    if (has_name(command.queued, "echo")) return MultiCommandKind::Echo;
    if (has_name(command.queued, "unwatch")) return MultiCommandKind::UnwatchReply;
    if (command.queued.spec->flags & CmdFlags::ConnLocal) return MultiCommandKind::IoLocal;
    if (has_name(command.queued, "dbsize")) return MultiCommandKind::IoLocal;
    if (has_name(command.queued, "config") && command.queued.argv.size() > 1 &&
        Slice(command.queued.argv[1].data(),
              static_cast<uint32_t>(command.queued.argv[1].size())).eq_icase("get"))
        return MultiCommandKind::IoLocal;
    // SubcmdRoute containers ride CursorShard only to reach the IO special-route hook; they are
    // not cursor commands. Their keyed forms have a truthful key range and fall through to Local;
    // their keyless subcommands answer on the IO thread like DBSIZE and CONFIG GET.
    if (command.queued.spec->flags & CmdFlags::SubcmdRoute)
        return command.key_args.empty() ? MultiCommandKind::IoLocal : MultiCommandKind::Local;
    if ((command.queued.spec->flags & CmdFlags::CursorShard) &&
        !(command.queued.spec->flags & CmdFlags::StreamRoute)) return MultiCommandKind::Scan;
    // These six execute through a normalized nonblocking xshard child even when all keys happen
    // to share one owner. Calling their ordinary Blocking handler from EXEC would park the whole
    // transaction, which Redis never does.
    if (command.blocking_pop != MultiBlockingPop::None) return MultiCommandKind::Unsupported;
    if (command.key_args.empty()) return MultiCommandKind::Unsupported;
    // A DEREFERENCING SORT IS NEVER LOCAL, however few keys it names. Its BY/GET patterns read keys
    // derived from the source's ELEMENTS, on shards this transaction never declared, and a Local
    // child is ordered only against the commands that share its one shard -- so an earlier MSET
    // still pending on a weight key's shard would be invisible to it. As an Xshard child it can
    // declare every shard as a participant and take the transaction's ordinary stage barrier.
    if (multi_sort_dereferences(command.queued)) return MultiCommandKind::Unsupported;
    bool same = true;
    for (size_t i = 1; i < command.key_shards.size(); i++)
        same &= command.key_shards[i] == command.key_shards[0];
    if (same) return MultiCommandKind::Local;
    if (has_name(command.queued, "mget")) return MultiCommandKind::Mget;
    if (has_name(command.queued, "mset")) return MultiCommandKind::Mset;
    if (has_name(command.queued, "msetnx")) return MultiCommandKind::Msetnx;
    if (has_name(command.queued, "del") || has_name(command.queued, "unlink"))
        return MultiCommandKind::Del;
    if (has_name(command.queued, "exists") || has_name(command.queued, "touch"))
        return MultiCommandKind::Exists;
    return MultiCommandKind::Unsupported;
}

bool state_writes_key(const MultiExecState& state, const MultiWatchedKey& watched) {
    for (const MultiWriteKey& key : state.write_keys)
        if (key.shard == watched.shard && key.key == watched.key) return true;
    return false;
}

void add_write_key(MultiExecState& state, const MultiCommand& command, uint32_t index) {
    if (!(command.queued.spec->flags & (CmdFlags::Write | CmdFlags::SnapshotWrite))) return;
    if (command.kind == MultiCommandKind::Unsupported) return;
    const uint32_t arg = command.key_args[index];
    const std::string& key = command.queued.argv[arg];
    const int32_t shard = command.key_shards[index];
    MultiWriteKey candidate{
        key, FlatStore::hash_key(Slice(key.data(), static_cast<uint32_t>(key.size()))), shard};
    if (!state.write_key_set.emplace(candidate).second) return;
    state.write_keys.push_back(std::move(candidate));
}

bool command_has_owner_work(const MultiCommand& command);

bool prepare_commands(Server& server, MultiExecState& state,
                      std::vector<MultiQueuedCommand>& queue) {
    try {
        state.commands.reserve(queue.size());
        state.write_key_set.reserve(queue.size());
        for (MultiQueuedCommand& queued : queue) {
            auto command = std::make_unique<MultiCommand>();
            command->notify_order_base =
                static_cast<uint32_t>(state.commands.size()) * 0x10000u;
            command->queued = std::move(queued);
            command->blocking_pop = multi_blocking_pop(command->queued);
            command_key_args(command->queued, command->key_args);
            command->key_shards.reserve(command->key_args.size());
            for (uint32_t arg : command->key_args) {
                const std::string& key = command->queued.argv[arg];
                const uint64_t hash = FlatStore::hash_key(
                    Slice(key.data(), static_cast<uint32_t>(key.size())));
                command->key_shards.push_back(server.router().shard_of(hash));
            }
            command->kind = classify_multi_command(*command);
            if (!make_child_op(*command)) return false;
            uint32_t acl_denied_arg = UINT32_MAX;
            const AclDeniedReason acl_denied = acl_check_queued(
                state.client->acl_user_idx(), *command->queued.spec,
                command->queued.argv, &acl_denied_arg);
            if (acl_denied != AclDeniedReason::None) {
                const char* reason = acl_denied == AclDeniedReason::Command
                    ? "no permission to execute the command or subcommand"
                    : acl_denied == AclDeniedReason::Key
                        ? "no permission to touch the specified keys"
                        : acl_denied == AclDeniedReason::Channel
                            ? "no permission to access one of the channels used as arguments"
                            : "no permission";
                std::string message =
                    "NOPERM ACLs rules changed between the moment the transaction was accumulated and the EXEC call. This command is no longer allowed for the following reason: ";
                message += reason;
                reply_err(command->op->sink(), message.c_str());
                command->kind = MultiCommandKind::Prebuilt;
                command->key_args.clear();
                command->key_shards.clear();
                LoopSignals& signals = server.thread(state.owner_io).sig();
                if (acl_denied == AclDeniedReason::Command) signals.acl_access_denied_cmd++;
                else if (acl_denied == AclDeniedReason::Key) signals.acl_access_denied_key++;
                else if (acl_denied == AclDeniedReason::Channel)
                    signals.acl_access_denied_channel++;
                const uint32_t object_arg = acl_denied_arg < command->queued.argv.size()
                    ? acl_denied_arg : 0;
                const std::string& object = command->queued.argv[object_arg];
                const char* username = acl_username(state.client->acl_user_idx());
                acl_log_denial(server.thread(state.owner_io), *state.client, acl_denied,
                    AclLogContext::Multi,
                    Slice(object.data(), static_cast<uint32_t>(object.size())),
                    Slice(username, static_cast<uint32_t>(std::strlen(username))));
            }
            if (command->kind != MultiCommandKind::Prebuilt &&
                command->blocking_pop != MultiBlockingPop::None) {
                if (!validate_multi_blocking_timeout(*command->op, command->blocking_pop)) {
                    command->kind = MultiCommandKind::Prebuilt;
                    command->key_args.clear();
                    command->key_shards.clear();
                } else if (!normalize_multi_blocking_pop(*command)) {
                    return false;
                }
            }
            if (state.client->resp3()) command->op->mark_resp3();
            if (command->kind == MultiCommandKind::Scan &&
                !command_prepare_scan_route(server, *command->op))
                command->kind = MultiCommandKind::Prebuilt;
            if (command->kind == MultiCommandKind::Unsupported &&
                (command->queued.spec->flags & CmdFlags::MultiShard) &&
                !(command->queued.spec->flags & (CmdFlags::AllShards | CmdFlags::ConfigRoute))) {
                ScatterDispatch child_dispatch;
                const ScatterPrepare prepared = xshard_prepare(
                    server, *command->op, state.scatter_pool, state.owner_io,
                    state.origin_conn_id, child_dispatch, true);
                if (prepared == ScatterPrepare::Ready) {
                    command->kind = MultiCommandKind::Xshard;
                    command->scatter = child_dispatch.state;
                    xshard_multi_child_bind(*command->scatter);
                    xshard_multi_child_capture_groups(
                        *command->scatter, command->xshard_stage_groups[0]);
                    for (uint32_t i = 0; i < child_dispatch.nshards; i++)
                        add_shard(command->participants,
                                  xshard_dispatch_shard(child_dispatch, i));
                    // Phase two can route to a STORE/destination owner not present in the initial
                    // dispatch.  Use the lowering's exact key set: registry metadata for LMPOP,
                    // ZMPOP and option-bearing commands intentionally spans non-key arguments.
                    for (uint32_t i = 0; i < command->scatter->key_count; i++)
                        add_shard(command->participants, command->scatter->keys[i].shard);
                    // A dereferencing SORT reads shards no key of this transaction names, and which
                    // are not knowable until the source has been read. It therefore participates in
                    // EVERY shard: shards outside its scatter group hold the barrier only (the
                    // Xshard arm below waits on `finished` for a shard with no group of its own),
                    // which is what orders the dereference after every earlier command everywhere
                    // and before every later one.
                    if (multi_sort_dereferences(command->queued))
                        for (uint32_t sid = 0; sid < server.nshards(); sid++)
                            add_shard(command->participants, static_cast<int32_t>(sid));
                } else if (prepared == ScatterPrepare::Error) {
                    // Validation errors are execution-time array elements, just as they are when
                    // the same syntactically valid command is run outside MULTI.
                    command->kind = MultiCommandKind::Prebuilt;
                    command->key_args.clear();
                    command->key_shards.clear();
                }
            }
            if (command->kind == MultiCommandKind::Local) {
                command->op->shard = command->key_shards[0];
                const uint32_t first = command->key_args[0];
                command->op->hash = FlatStore::hash_key(command->op->arg(first));
                if (command->queued.spec->flags & CmdFlags::MultiShard)
                    command->op->mark_local_xshard();
                add_shard(command->participants, command->key_shards[0]);
            } else if (command->kind == MultiCommandKind::Scan) {
                add_shard(command->participants, command->op->shard);
            } else if (command->kind == MultiCommandKind::Xshard) {
                // xshard_prepare supplied the exact phase-one participant set above.
                command->scatter->pop_reply_shape =
                    static_cast<PopReplyShape>(command->pop_reply_shape);
            } else if (command->kind == MultiCommandKind::Ping ||
                       command->kind == MultiCommandKind::Echo ||
                       command->kind == MultiCommandKind::UnwatchReply ||
                       command->kind == MultiCommandKind::IoLocal ||
                       command->kind == MultiCommandKind::Prebuilt) {
                add_shard(command->participants, 0);
            } else {
                for (int32_t shard : command->key_shards)
                    add_shard(command->participants, shard);
                if (command->participants.empty()) add_shard(command->participants, 0);
            }
            command->shard_done.assign(server.nshards(), 0);
            // SCOPE: multi-owner READ fragments, nothing else.  Mget/Exists are reached only when
            // classify_multi_command already found the key set spanning more than one shard, and an
            // Xshard child carries its own participant count.  A single-owner read resolves inside
            // one owner task and cannot straddle anything (the same-owner control reads zero torn),
            // and a write must keep cloning from the newest committed value, so widening this test
            // would buy no isolation and cost the transaction a floor registration.
            const bool sort_deref_read = command->kind == MultiCommandKind::Xshard &&
                command->scatter && command->scatter->kind == Kind::Sort &&
                command->scatter->sort_options.deref;
            if (sort_deref_read) {
                // STORE makes SORT a write command, but its dynamic BY/GET wave is still a
                // multi-owner read and must borrow the transaction's one registered cut.
                state.exec_read_fanout = true;
            } else if (!(command->queued.spec->flags &
                         (CmdFlags::Write | CmdFlags::SnapshotWrite))) {
                if (command->kind == MultiCommandKind::Mget ||
                    command->kind == MultiCommandKind::Exists)
                    state.exec_read_fanout = true;
                else if (command->kind == MultiCommandKind::Xshard && command->scatter &&
                         command->scatter->nsub > 1)
                    state.exec_read_fanout = true;
            }
            if (command->kind == MultiCommandKind::Mget)
                command->values.resize(command->key_args.size());
            command->pending.store(static_cast<uint32_t>(command->participants.size()),
                                   std::memory_order_relaxed);
            for (size_t i = 0; i < command->key_args.size(); i++) add_write_key(state, *command, i);
            for (int32_t shard : command->participants) add_shard(state.shards, shard);
            const uint32_t command_index = static_cast<uint32_t>(state.commands.size());
            if (command_has_owner_work(*command)) {
                for (int32_t shard : command->participants)
                    state.shard_commands[static_cast<uint32_t>(shard)].push_back(command_index);
            }
            state.commands.push_back(std::move(command));
        }
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

MultiExecState* allocate_state(Server& server, Client& client, uint32_t owner_io,
                               MultiStateKind kind) {
    auto* state = new (std::nothrow) MultiExecState;
    if (!state) return nullptr;
    state->server = &server;
    state->client = &client;
    state->owner_io = owner_io;
    state->origin_conn_id = client.id();
    state->watch_generation = client.watch_generation();
    state->kind = kind;
    try {
        state->owner_record_refs.assign(server.nthreads(), MultiOwnerRecordRefs{});
        state->shard_commands.resize(server.nshards());
        state->shard_cursors.assign(server.nshards(), 0);
        state->watch_shard_done.assign(server.nshards(), 0);
        state->execution_joined.assign(server.nshards(), 0);
        state->final_shard_done.assign(server.nshards(), 0);
        state->aof_shard_done.assign(server.nshards(), 0);
        state->execution_noted.assign(server.nshards(), 0);
    } catch (const std::bad_alloc&) {
        delete state;
        return nullptr;
    }
    return state;
}

// WRITE_KEYS IS NOT THE SET OF EXECUTORS THAT INSTALL. It is the prepare-time registry key list,
// and a lowered LMPOP/ZMPOP has none: command_key_args() returns nothing for their numkeys form, so
// classify_multi_command() sends them down the Xshard arm and multi_execute_task() calls
// prepare_write_key() at phase two for a key SELECTED THEN, on whichever shard held a non-empty
// list. Deriving sentinels from write_keys therefore left that executor -- and, for a transaction
// whose only write is such a command, EVERY executor -- with no pin, which the install then
// aborted on. The outer task set is the exact and cheaper predicate: every executor that installs
// anything for this transaction must first have been given one of its shard tasks.
bool multi_state_may_install(const MultiExecState& state) {
    for (const auto& command : state.commands) {
        if (!command_has_owner_work(*command)) continue;
        if (command->queued.spec->flags & (CmdFlags::Write | CmdFlags::SnapshotWrite))
            return true;
    }
    return false;
}

void initialize_multi_owner_record_refs(MultiExecState& state) {
    if (state.kind != MultiStateKind::Execute || !multi_state_may_install(state)) return;
    if (state.record_refs.load(std::memory_order_relaxed) != 0 ||
        state.record_refs_precounted) std::abort();

    // Every outer shard task reaches the post-decision completion boundary even when WATCH or
    // EXECABORT skips its commands. Count all such tasks per executor so only its last one releases
    // the sentinel protecting whatever records earlier tasks may have installed.
    uint32_t owners = 0;
    for (int32_t shard : state.shards) {
        const uint32_t tid = state.server->worker_of_shard(shard);
        const uint32_t slot = state.server->executor_slot(tid);
        if (slot >= state.owner_record_refs.size()) std::abort();
        MultiOwnerRecordRefs& progress = state.owner_record_refs[slot];
        if (progress.remaining++) continue;
        if (progress.nodes || progress.completion_sentinel) std::abort();
        progress.nodes = 1;
        progress.completion_sentinel = true;
        owners++;
    }
    if (!owners) std::abort();
    state.record_refs_precounted = true;
    state.record_refs.store(owners, std::memory_order_release);
}

void complete_multi_owner_record_refs(MultiExecState& state, uint32_t owner_thread_id) {
    if (!state.record_refs_precounted) return;
    const uint32_t slot = state.server->executor_slot(owner_thread_id);
    if (slot >= state.owner_record_refs.size()) std::abort();
    MultiOwnerRecordRefs& progress = state.owner_record_refs[slot];
    if (!progress.remaining) std::abort();
    if (--progress.remaining != 0 || !progress.completion_sentinel) return;

    progress.completion_sentinel = false;
    if (!progress.nodes) std::abort();
    if (--progress.nodes == 0 &&
        state.record_refs.fetch_sub(1, std::memory_order_release) == 0) std::abort();
}

void clear_unpublished_multi_record_refs(MultiExecState& state) {
    if (!state.record_refs_precounted) return;
    uint32_t owners = 0;
    for (MultiOwnerRecordRefs& progress : state.owner_record_refs) {
        if (!progress.completion_sentinel) continue;
        // The caller proved no task was published, so only the prepare-time sentinel can be present.
        if (progress.nodes != 1) std::abort();
        progress.nodes = 0;
        progress.completion_sentinel = false;
        owners++;
    }
    if (state.record_refs.load(std::memory_order_relaxed) != owners) std::abort();
    state.record_refs.store(0, std::memory_order_relaxed);
    state.record_refs_precounted = false;
}

MultiExecState* prepare_unwatch_state(Server& server, Client& client, uint32_t owner_io,
                                      MultiSession& session, MultiStateKind kind,
                                      MultiFinalReply reply) {
    MultiExecState* state = allocate_state(server, client, owner_io, kind);
    if (!state) return nullptr;
    state->final_reply = reply;
    state->reset_watch = true;
    state->watched = std::move(session.watched);
    for (const MultiWatchedKey& key : state->watched) add_shard(state->shards, key.shard);
    state->watch_pending.store(static_cast<uint32_t>(state->shards.size()),
                               std::memory_order_relaxed);
    state->lists_pending.store(static_cast<uint32_t>(state->shards.size()),
                               std::memory_order_relaxed);
    state->tasks_pending.store(static_cast<uint32_t>(state->shards.size()),
                               std::memory_order_relaxed);
    return state;
}

void set_state_reply(MultiExecState& state, Op& public_op) {
    if (state.final_reply == MultiFinalReply::Ok) {
        reply_ok(public_op.sink());
        return;
    }
    if (state.final_reply == MultiFinalReply::ExecAbort) {
        reply_err(public_op.sink(),
                  "EXECABORT Transaction discarded because of previous errors.");
        return;
    }
    if (state.watch_failed.load(std::memory_order_acquire)) {
        reply_null_array(public_op.sink(), public_op.resp3());
        return;
    }
    if (state.aborted.load(std::memory_order_acquire)) {
        reply_err(public_op.sink(), "EXECABORT Transaction discarded because of an execution error.");
        return;
    }
    reply_array_header(public_op.sink(), state.commands.size());
    for (const auto& command : state.commands)
        public_op.reply.append(command->op->reply.data(), command->op->reply.size());
}

void complete_child_zc(Shard& shard, Op& op) {
    if (!op.zc_ptr || op.zc_shard < 0) return;
    op.reply.append(op.zc_ptr, op.zc_len);
    op.reply.append("\r\n", 2);
    shard.store().unborrow(op.zc_ptr);
    op.zc_ptr = nullptr;
    op.zc_len = 0;
    op.zc_shard = -1;
}

bool load_clone(Shard& shard, Slice key, KvObj* visible, KvObj*& clone) {
    clone = nullptr;
    if (!visible) return true;
    ObjectImage image;
    if (!serialize_object(visible, image)) return false;
    return load_object_image(key, image, shard.type_limits(), clone) ==
               SnapshotHookStatus::Ok && clone;
}

// Violations of the install-time ordering invariant asserted in prepare_write_key() below. It is a
// bug counter, not a workload counter: every battery that exercises a transaction behind an
// undecided same-connection unit asserts it is EXACTLY zero, in both atomic modes. Namespace scope
// rather than Shard::Stats because Shard's layout is locked (sizeof(Shard) == 1440) and because a
// counter that is never incremented needs no per-shard line of its own.
// Internal linkage is right for the counter itself -- prepare_write_key() below is its only
// writer, and it lives in this file's anonymous namespace with the rest of the transaction
// helpers. The READER that INFO calls cannot: it is declared in multi.h and defined out beyond
// this anonymous namespace, next to multi_task_owns_epoch().
std::atomic<uint64_t> g_exec_order_late{0};

enum class MultiPrepareWrite : uint8_t { Ready, Retry, Error };

MultiPrepareWrite prepare_write_key(Server& server, MultiExecState& state, Shard& shard,
                                    Slice key, uint64_t hash, uint32_t owner_thread_id,
                                    const void* watch_token = nullptr,
                                    std::atomic<uint32_t>* watch_refs = nullptr) {
    if (!watch_token) watch_token = &state;
    if (!watch_refs) watch_refs = &state.watch_refs;
    // THE FALSIFIER, and it must read ZERO. It asks EXACTLY the question the dispatch-time park in
    // ExLoop::execute() asks -- does this owner still hold an undecided record belonging to a
    // DIFFERENT logical unit of this connection? -- but asks it one step later, at install time.
    // The park is supposed to foreclose it completely, because only this owner thread may publish
    // into this owner's pending list, so nothing can appear between the dispatch check and these
    // installs. A non-zero reading is therefore not a statistic: it is proof that the park's
    // safety argument (NOTES-MULTIRACE.md §5) is false on this build, and that a transaction could
    // again clone a withdrawable candidate into an acknowledged write.
    //
    // It counts into its OWN counter, deliberately. Sharing atomic_exec_order_holds with the park
    // would make this arm unreadable: the park is non-zero BY DESIGN in exactly the runs that open
    // the window, so a sum can never be checked against zero and the falsifier would be inert --
    // the safety argument would break with no test going red. Split, each counter answers one
    // question: holds > 0 proves the hold fired, late == 0 proves it fired EARLY ENOUGH.
    // Not a Shard::Stats field because Shard's layout is locked at 1440 bytes; a namespace-scope
    // atomic costs nothing here, since the increment is on a path that is never taken.
    //
    // Holding the transaction HERE instead of counting was tried and is WRONG -- it deadlocks,
    // NOTES-MULTIRES.md §5(a); the park is safe only because it happens before the fragment has
    // installed anything on this owner.
    // The exclusion is BY EPOCH WORD and must cover the transaction's lowered children as well as
    // its own: a cross-shard child installs through the scatter path with its own ScatterState,
    // and counting that would report a violation every time a transaction merely met its own
    // RENAME/LMPOP child.
    if (__builtin_expect(shard.store().atomic_has_records(), false) &&
        shard.store().atomic_has_foreign_unit_undecided(
            state.origin_conn_id,
            [&state](const void* group_epoch) {
                return multi_state_owns_epoch(state, group_epoch);
            }))
        g_exec_order_late.fetch_add(1, std::memory_order_relaxed);
    if (!shard.watch_write_ready(key, watch_token)) return MultiPrepareWrite::Retry;
    shard.watch_reserve_write(key, state.client, state.watch_generation, watch_token,
                              &state.epoch, &state.aborted, watch_refs);
    const auto snapshot = shard.store().snapshot_prepare_write(hash, key);
    if (snapshot == FlatStore::SnapshotWriteResult::Pending) return MultiPrepareWrite::Retry;
    if (snapshot == FlatStore::SnapshotWriteResult::Error) return MultiPrepareWrite::Error;
    if (!shard.store().atomic_prepare_capacity(1)) return MultiPrepareWrite::Error;

    shard.store().atomic_set_read_context(UINT64_MAX, state.origin_conn_id);
    KvObj* visible = shard.store().find(hash, key);
    KvObj* clone = nullptr;
    if (!load_clone(shard, key, visible, clone)) {
        shard.store().atomic_clear_read_epoch();
        return MultiPrepareWrite::Error;
    }
    if (!shard.store().atomic_admit(key, clone)) {
        if (clone) kvobj_free(clone);
        shard.store().atomic_clear_read_epoch();
        return MultiPrepareWrite::Error;
    }
    const uint32_t slot = server.executor_slot(owner_thread_id);
    if (slot >= state.owner_record_refs.size()) std::abort();
    MultiOwnerRecordRefs& owner_progress = state.owner_record_refs[slot];
    uint32_t* owner_refs = &owner_progress.nodes;
    void* entry = nullptr;
    if (!shard.store().atomic_prepare_transaction(
            key, hash, state.origin_conn_id, &state.epoch, &state.record_refs,
            &state.aborted, owner_refs, entry)) {
        shard.store().atomic_unadmit(clone);
        if (clone) kvobj_free(clone);
        shard.store().atomic_clear_read_epoch();
        return MultiPrepareWrite::Error;
    }
    KvObj* installed = clone;
    shard.store().atomic_install_transaction(hash, key, entry, installed);
    // INVARIANT, not a hope: initialize_multi_owner_record_refs() gave every executor holding an
    // outer task of an installing transaction its sentinel before the first task became visible,
    // and this executor is running one of those tasks. A missing pin here would mean cleanup could
    // release the state's last reference between two of this owner's own installs.
    if (!owner_progress.completion_sentinel || !*owner_refs) std::abort();
    (*owner_refs)++;
    // Keep the transaction overlay selected for the handler and later keys in this command.
    shard.store().atomic_set_read_context(UINT64_MAX, state.origin_conn_id);
    return MultiPrepareWrite::Ready;
}

bool command_has_owner_work(const MultiCommand& command) {
    return command.kind != MultiCommandKind::Ping &&
           command.kind != MultiCommandKind::Echo &&
           command.kind != MultiCommandKind::UnwatchReply &&
           command.kind != MultiCommandKind::IoLocal &&
           command.kind != MultiCommandKind::Prebuilt &&
           command.kind != MultiCommandKind::Unsupported;
}

bool prepare_local_write_keys(Server& server, MultiExecState& state, MultiCommand& command,
                              Shard& shard, uint32_t owner_thread_id) {
    if (!(command.queued.spec->flags & (CmdFlags::Write | CmdFlags::SnapshotWrite))) return true;
    std::vector<uint32_t> unique;
    try { unique.reserve(command.key_args.size()); }
    catch (const std::bad_alloc&) { state.aborted.store(true, std::memory_order_release); return false; }
    for (size_t i = 0; i < command.key_args.size(); i++) {
        if (command.key_shards[i] != shard.id()) continue;
        const uint32_t arg = command.key_args[i];
        bool duplicate = false;
        for (uint32_t prior : unique)
            duplicate |= command.queued.argv[prior] == command.queued.argv[arg];
        if (!duplicate) unique.push_back(arg);
    }
    // Reservation/snapshot checks precede every physical install, so Retry never leaves half of a
    // command installed merely because another EXEC still owns a watched key.
    for (uint32_t arg : unique) {
        const std::string& owned = command.queued.argv[arg];
        const Slice key(owned.data(), static_cast<uint32_t>(owned.size()));
        if (!shard.watch_write_ready(key, &state)) return false;
        const auto snapshot = shard.store().snapshot_prepare_write(FlatStore::hash_key(key), key);
        if (snapshot == FlatStore::SnapshotWriteResult::Pending) return false;
        if (snapshot == FlatStore::SnapshotWriteResult::Error) {
            state.aborted.store(true, std::memory_order_release);
            return false;
        }
    }
    for (uint32_t arg : unique) {
        const std::string& owned = command.queued.argv[arg];
        const Slice key(owned.data(), static_cast<uint32_t>(owned.size()));
        const MultiPrepareWrite result = prepare_write_key(
            server, state, shard, key, FlatStore::hash_key(key), owner_thread_id);
        if (result == MultiPrepareWrite::Retry) return false;
        if (result == MultiPrepareWrite::Error) {
            state.aborted.store(true, std::memory_order_release);
            return false;
        }
    }
    return true;
}

// The cut a multi-owner READ fragment of this transaction resolves at.  UINT64_MAX means "newest
// committed at the instant this fragment runs", which is what every MULTI child used before this
// lane; that is exactly the behaviour that let one MGET's eight fragments straddle a foreign
// transaction's single ticket.  A transaction that registered no cut (write-only, single-owner, or
// the cut was never drawn) keeps the old value, so nothing else in the file changes shape.
uint64_t multi_read_cut(const MultiExecState& state) {
    return state.snapshot_registered ? state.snapshot : UINT64_MAX;
}

uint64_t multi_tripwire_slot(const Shard& shard, size_t key_index) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(shard.id())) << 32) |
           static_cast<uint32_t>(key_index);
}

void multi_tripwire_fragment_begin(MultiExecState& state, Shard& shard) {
    if (!atomic_tripwire_enabled()) return;
    const uint32_t shard_id = static_cast<uint32_t>(shard.id());
    const uint64_t cut = multi_read_cut(state);
    for (uint32_t command_index : state.shard_commands[shard_id]) {
        MultiCommand& command = *state.commands[command_index];
        if (command.kind != MultiCommandKind::Mget &&
            command.kind != MultiCommandKind::Exists)
            continue;
        for (size_t i = 0; i < command.key_args.size(); i++) {
            if (command.key_shards[i] != shard.id()) continue;
            const Slice key = command.op->arg(command.key_args[i]);
            const uint64_t hash = FlatStore::hash_key(key);
            atomic_tripwire_plain_begin(
                &state, &command, multi_tripwire_slot(shard, i), hash, cut,
                shard.store().atomic_tripwire_ticket(hash, key),
                shard.store().atomic_needs_version(hash, key));
        }
    }
}

void multi_tripwire_read_execution(MultiExecState& state, MultiCommand& command,
                                   Shard& shard, size_t key_index, Slice key) {
    if (!atomic_tripwire_enabled()) return;
    const uint64_t hash = FlatStore::hash_key(key);
    atomic_tripwire_plain_execution(
        &state, &command, multi_tripwire_slot(shard, key_index), hash, multi_read_cut(state),
        shard.store().atomic_tripwire_ticket(hash, key),
        shard.store().atomic_needs_version(hash, key),
        shard.store().atomic_tripwire_plain_path());
}

bool execute_local_command(Server& server, MultiExecState& state, MultiCommand& command,
                           Shard& shard, uint32_t owner_thread_id) {
    if (!prepare_local_write_keys(server, state, command, shard, owner_thread_id)) return false;
    if (command.op->spec->flags & CmdFlags::NotifySelected)
        notify_execute_source(shard, *command.op, command.notify_order_base);
    shard.store().atomic_set_read_context(UINT64_MAX, state.origin_conn_id);
    command.op->spec->handler(shard, *command.op);
    complete_child_zc(shard, *command.op);
    shard.store().atomic_clear_read_epoch();
    return true;
}

bool execute_cross_command(Server& server, MultiExecState& state, MultiCommand& command,
                           Shard& shard, uint32_t owner_thread_id, uint32_t stage) {
    Op& op = *command.op;
    const bool notify = (op.spec->flags & CmdFlags::NotifySelected) != 0;
    if (notify)
        notify_execute_source(shard, op, command.notify_order_base);
    // MGET/EXISTS/TOUCH are the read kinds here and reach this function only with a key set that
    // spans more than one shard, i.e. exactly the fan-out that could straddle. They resolve at the
    // transaction's cut. MSET/MSETNX/DEL keep UINT64_MAX on purpose: their clone base must stay
    // the newest committed value, or a read-modify-write inside EXEC would overwrite a concurrent
    // commit from a stale image. Write visibility is deliberately untouched by this lane.
    const bool read_kind = command.kind == MultiCommandKind::Mget ||
                           command.kind == MultiCommandKind::Exists;
    shard.store().atomic_set_read_context(
        read_kind ? multi_read_cut(state) : UINT64_MAX, state.origin_conn_id);
    if (command.kind == MultiCommandKind::Mget) {
        for (size_t i = 0; i < command.key_args.size(); i++) {
            if (command.key_shards[i] != shard.id()) continue;
            const uint32_t arg = command.key_args[i];
            const Slice key = op.arg(arg);
            if (__builtin_expect(atomic_tripwire_enabled(), false))
                multi_tripwire_read_execution(state, command, shard, i, key);
            KvObj* object = notify
                ? shard.store_find_read<true>(FlatStore::hash_key(key), key)
                : shard.store_find_read<false>(FlatStore::hash_key(key), key);
            if (!object || static_cast<Type>(object->type) != Type::String) continue;
            MultiValue& value = command.values[i];
            if (object->is_int()) {
                char text[24];
                const uint32_t n = i64_to_dec(text, object->int_value());
                value.value.assign(text, n);
            } else {
                KvObjRawReadBuffer raw;
                const Slice bytes = kvobj_string_value(object, raw);
                value.value.assign(bytes.p, bytes.n);
            }
            value.present = true;
        }
    } else if (command.kind == MultiCommandKind::Mset ||
               (command.kind == MultiCommandKind::Msetnx && stage == 1)) {
        for (size_t i = 0; i < command.key_args.size(); i++) {
            if (command.key_shards[i] != shard.id()) continue;
            const uint32_t arg = command.key_args[i];
            const Slice key = op.arg(arg);
            const MultiPrepareWrite prepared = prepare_write_key(
                server, state, shard, key, FlatStore::hash_key(key), owner_thread_id);
            if (prepared == MultiPrepareWrite::Retry) {
                shard.store().atomic_clear_read_epoch();
                return false;
            }
            if (prepared == MultiPrepareWrite::Error) {
                state.aborted.store(true, std::memory_order_release);
                break;
            }
            const WorkError stored = store_xstring_selected(
                notify, shard, key, FlatStore::hash_key(key), op.arg(arg + 1));
            if (stored != WorkError::None) {
                state.aborted.store(true, std::memory_order_release);
                break;
            }
            if (notify) notify_record(shard, op, NOTIFY_STRING, NotifyEventId::Set, key);
        }
    } else if (command.kind == MultiCommandKind::Msetnx) {
        for (size_t i = 0; i < command.key_args.size(); i++) {
            if (command.key_shards[i] != shard.id()) continue;
            const uint32_t arg = command.key_args[i];
            const Slice key = op.arg(arg);
            if (notify ? shard.store_find<true>(FlatStore::hash_key(key), key)
                       : shard.store().find(FlatStore::hash_key(key), key))
                command.condition_failed.store(true, std::memory_order_release);
        }
    } else if (command.kind == MultiCommandKind::Del) {
        long long removed = 0;
        for (size_t i = 0; i < command.key_args.size(); i++) {
            if (command.key_shards[i] != shard.id()) continue;
            const uint32_t arg = command.key_args[i];
            const Slice key = op.arg(arg);
            const MultiPrepareWrite prepared = prepare_write_key(
                server, state, shard, key, FlatStore::hash_key(key), owner_thread_id);
            if (prepared == MultiPrepareWrite::Retry) {
                shard.store().atomic_clear_read_epoch();
                return false;
            }
            if (prepared == MultiPrepareWrite::Error) {
                state.aborted.store(true, std::memory_order_release);
                break;
            }
            removed += notify ? shard.store_erase<true>(FlatStore::hash_key(key), key)
                              : shard.store().erase(FlatStore::hash_key(key), key);
        }
        command.integer.fetch_add(removed, std::memory_order_relaxed);
    } else if (command.kind == MultiCommandKind::Exists) {
        long long found = 0;
        for (size_t i = 0; i < command.key_args.size(); i++) {
            if (command.key_shards[i] != shard.id()) continue;
            const uint32_t arg = command.key_args[i];
            const Slice key = op.arg(arg);
            if (__builtin_expect(atomic_tripwire_enabled(), false))
                multi_tripwire_read_execution(state, command, shard, i, key);
            found += (notify ? shard.store_find_read<true>(FlatStore::hash_key(key), key)
                             : shard.store_find_read<false>(FlatStore::hash_key(key), key)) != nullptr;
        }
        command.integer.fetch_add(found, std::memory_order_relaxed);
    }
    shard.store().atomic_clear_read_epoch();
    return true;
}

void assemble_cross_reply(MultiCommand& command) {
    Op& op = *command.op;
    if (command.kind == MultiCommandKind::Mget) {
        reply_array_header(op.sink(), command.values.size());
        for (const MultiValue& value : command.values) {
            if (!value.present) reply_null(op.sink(), op.resp3());
            else reply_bulk(op.sink(), Slice(value.value.data(),
                                              static_cast<uint32_t>(value.value.size())));
        }
    } else if (command.kind == MultiCommandKind::Mset) {
        reply_ok(op.sink());
    } else if (command.kind == MultiCommandKind::Msetnx) {
        reply_int(op.sink(), command.condition_failed.load(std::memory_order_acquire) ? 0 : 1);
    } else if (command.kind == MultiCommandKind::Del ||
               command.kind == MultiCommandKind::Exists) {
        reply_int(op.sink(), command.integer.load(std::memory_order_relaxed));
    } else if (command.kind == MultiCommandKind::Ping) {
        if (op.argc() == 1) reply_pong(op.sink());
        else reply_bulk(op.sink(), op.arg(1));
    } else if (command.kind == MultiCommandKind::Echo) {
        reply_bulk(op.sink(), op.arg(1));
    } else if (command.kind == MultiCommandKind::UnwatchReply) {
        reply_ok(op.sink());
    } else {
        reply_err(op.sink(), "ERR command is not supported by MULTI execution");
    }
}

bool execute_watch_phase(MultiExecState& state, Shard& shard) {
    if (state.watch_shard_done[static_cast<uint32_t>(shard.id())]) return true;
    if (state.kind == MultiStateKind::WatchAdd) {
        for (const MultiWatchedKey& watched : state.watched) {
            if (watched.shard != shard.id()) continue;
            const Slice key(watched.key.data(), static_cast<uint32_t>(watched.key.size()));
            if (!shard.watch_add(key, state.client, state.watch_generation)) return false;
        }
    } else if (state.kind == MultiStateKind::Execute) {
        for (const MultiWatchedKey& watched : state.watched) {
            if (watched.shard != shard.id()) continue;
            const Slice key(watched.key.data(), static_cast<uint32_t>(watched.key.size()));
            if (!shard.watch_validate_and_reserve(
                    key, state.client, state.watch_generation, &state, &state.epoch,
                    &state.aborted, &state.watch_refs,
                    state_writes_key(state, watched))) return false;
        }
        if (state.client->watch_dirty()) {
            state.watch_failed.store(true, std::memory_order_release);
            state.aborted.store(true, std::memory_order_release);
        }
        for (const MultiWatchedKey& watched : state.watched) {
            if (watched.shard != shard.id()) continue;
            shard.watch_remove(Slice(watched.key.data(), static_cast<uint32_t>(watched.key.size())),
                               state.client, state.watch_generation);
        }
    } else {
        for (const MultiWatchedKey& watched : state.watched) {
            if (watched.shard != shard.id()) continue;
            // A prior atomic writer may have completed its owner pass while its global decision is
            // still unpublished.  Keep UNWATCH/DISCARD/close behind that decision so removing the
            // last watcher also releases the reservation's state-lifetime pin.
            if (!shard.watch_write_ready(
                    Slice(watched.key.data(), static_cast<uint32_t>(watched.key.size()))))
                return false;
            shard.watch_remove(Slice(watched.key.data(), static_cast<uint32_t>(watched.key.size())),
                               state.client, state.watch_generation);
        }
    }
    state.watch_shard_done[static_cast<uint32_t>(shard.id())] = 1;
    if (state.watch_pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
        state.watch_phase_done.store(true, std::memory_order_release);
    return true;
}

void finalize_reservations(MultiExecState& state, Shard& shard) {
    if (state.final_shard_done[static_cast<uint32_t>(shard.id())]) return;
    for (const MultiWatchedKey& watched : state.watched) {
        if (watched.shard != shard.id()) continue;
        shard.watch_write_ready(
            Slice(watched.key.data(), static_cast<uint32_t>(watched.key.size())), nullptr);
    }
    for (const MultiWriteKey& write : state.write_keys) {
        if (write.shard != shard.id()) continue;
        shard.watch_write_ready(
            Slice(write.key.data(), static_cast<uint32_t>(write.key.size())), nullptr);
    }
    state.final_shard_done[static_cast<uint32_t>(shard.id())] = 1;
}

void finish_command(MultiCommand& command, uint32_t stage) {
    if (command.pending.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    if (command.kind == MultiCommandKind::Msetnx && stage == 0 &&
        !command.condition_failed.load(std::memory_order_acquire)) {
        command.pending.store(static_cast<uint32_t>(command.participants.size()),
                              std::memory_order_relaxed);
        command.stage.store(1, std::memory_order_release);
        return;
    }
    if (command.kind != MultiCommandKind::Local &&
        command.kind != MultiCommandKind::Scan &&
        command.kind != MultiCommandKind::IoLocal &&
        command.kind != MultiCommandKind::Prebuilt) assemble_cross_reply(command);
    command.finished.store(true, std::memory_order_release);
}

void assemble_ownerless_replies(MultiExecState& state) {
    for (const auto& command : state.commands) {
        if (command_has_owner_work(*command) ||
            command->kind == MultiCommandKind::IoLocal ||
            command->kind == MultiCommandKind::Prebuilt)
            continue;
        assemble_cross_reply(*command);
    }
}

// A transaction is one commit ticket but not one runtime-error bit. Each xshard child keeps its
// own abort word so a normal command failure can hide that child's partially installed candidates
// while the other queued commands still commit, as Redis EXEC semantics require. The Server helper
// calls the commit arm before its safe watermark publication, so no reader can observe only a
// subset of these epoch stores.
void publish_multi_child_epochs(MultiExecState& state, uint64_t ticket) {
    for (const auto& command : state.commands)
        if (command->scatter)
            command->scatter->epoch.store(ticket, std::memory_order_release);
}

void abort_multi_children(MultiExecState& state) {
    for (const auto& command : state.commands)
        if (command->scatter)
            command->scatter->aborted.store(true, std::memory_order_release);
}

}  // namespace

bool multi_queueing(const Client& client) {
    return client.multi_session() && client.multi_session()->active;
}

void multi_mark_queue_error(Client& client) {
    MultiSession* session = client.multi_session();
    if (session && session->active) session->queue_error = true;
}

MultiIoAction multi_handle_io(Server& server, Client& client, Op& op, uint32_t owner_io,
                              MultiExecState*& dispatch) {
    dispatch = nullptr;
    MultiSession* session = client.multi_session();
    if (session && session->pending) {
        dispatch = session->pending;
        return MultiIoAction::Dispatch;
    }

    if (multi_name(op, "multi")) {
        session = ensure_multi_session(client);
        if (!session) {
            reply_err(op.sink(), "ERR out of memory");
        } else if (session->active) {
            reply_err(op.sink(), "ERR MULTI calls can not be nested");
        } else {
            session->active = true;
            session->queue_error = false;
            session->next_queue_op_id = client.rob().dispatch_id() + 1;
            session->queue.clear();
            reply_ok(op.sink());
        }
        return MultiIoAction::LocalDone;
    }

    if (multi_name(op, "watch")) {
        session = ensure_multi_session(client);
        if (!session) { reply_err(op.sink(), "ERR out of memory"); return MultiIoAction::LocalDone; }
        if (session->active) {
            reply_err(op.sink(), "ERR WATCH inside MULTI is not allowed");
            return MultiIoAction::LocalDone;
        }
        if (session->watched.empty()) {
            client.next_watch_generation();
            client.clear_watch_dirty();
        }
        MultiExecState* state = allocate_state(server, client, owner_io, MultiStateKind::WatchAdd);
        if (!state) { reply_err(op.sink(), "ERR out of memory"); return MultiIoAction::LocalDone; }
        state->final_reply = MultiFinalReply::Ok;
        try {
            for (uint32_t i = 1; i < op.argc(); i++) {
                const std::string key(op.arg(i).p, op.arg(i).n);
                bool duplicate = false;
                for (const MultiWatchedKey& prior : session->watched)
                    duplicate |= prior.key == key;
                if (duplicate) continue;
                const uint64_t hash = FlatStore::hash_key(op.arg(i));
                MultiWatchedKey watched{key, hash, server.router().shard_of(hash)};
                session->watched.push_back(watched);
                state->watched.push_back(std::move(watched));
            }
            for (const MultiWatchedKey& key : state->watched) add_shard(state->shards, key.shard);
        } catch (const std::bad_alloc&) {
            destroy_multi_state(state);
            reply_err(op.sink(), "ERR out of memory");
            return MultiIoAction::LocalDone;
        }
        if (state->shards.empty()) {
            delete state;
            reply_ok(op.sink());
            return MultiIoAction::LocalDone;
        }
        state->watch_pending.store(static_cast<uint32_t>(state->shards.size()),
                                   std::memory_order_relaxed);
        state->lists_pending.store(static_cast<uint32_t>(state->shards.size()),
                                   std::memory_order_relaxed);
        state->tasks_pending.store(static_cast<uint32_t>(state->shards.size()),
                                   std::memory_order_relaxed);
        session->pending = state;
        dispatch = state;
        return MultiIoAction::Dispatch;
    }

    if (multi_name(op, "unwatch") && (!session || !session->active)) {
        if (!session || session->watched.empty()) {
            client.next_watch_generation();
            client.clear_watch_dirty();
            reply_ok(op.sink());
            return MultiIoAction::LocalDone;
        }
        MultiExecState* state = prepare_unwatch_state(
            server, client, owner_io, *session, MultiStateKind::Unwatch, MultiFinalReply::Ok);
        if (!state) { reply_err(op.sink(), "ERR out of memory"); return MultiIoAction::LocalDone; }
        session->pending = state;
        dispatch = state;
        return MultiIoAction::Dispatch;
    }

    if (multi_name(op, "discard")) {
        if (!session || !session->active) {
            reply_err(op.sink(), "ERR DISCARD without MULTI");
            return MultiIoAction::LocalDone;
        }
        session->active = false;
        session->queue_error = false;
        session->queue.clear();
        if (session->watched.empty()) {
            client.next_watch_generation();
            client.clear_watch_dirty();
            reply_ok(op.sink());
            return MultiIoAction::LocalDone;
        }
        MultiExecState* state = prepare_unwatch_state(
            server, client, owner_io, *session, MultiStateKind::Unwatch, MultiFinalReply::Ok);
        if (!state) { reply_err(op.sink(), "ERR out of memory"); return MultiIoAction::LocalDone; }
        session->pending = state;
        dispatch = state;
        return MultiIoAction::Dispatch;
    }

    if (multi_name(op, "exec")) {
        if (!session || !session->active) {
            reply_err(op.sink(), "ERR EXEC without MULTI");
            return MultiIoAction::LocalDone;
        }
        if (!server.atomic_can_admit(owner_io, true)) return MultiIoAction::Backpressure;
        uint64_t generation = 0;
        if (!server.atomic_try_admit(owner_io, generation, true))
            return MultiIoAction::Backpressure;
        MultiExecState* state = allocate_state(server, client, owner_io, MultiStateKind::Execute);
        if (!state) {
            server.atomic_retire_group(owner_io, generation);
            reply_err(op.sink(), "ERR out of memory");
            session->active = false;
            session->queue.clear();
            return MultiIoAction::LocalDone;
        }
        state->admitted = true;
        server.atomic_apply_open(owner_io, state->apply_open);
        state->admission_generation = generation;
        state->reset_watch = true;
        state->watched = std::move(session->watched);
        state->final_reply = session->queue_error ? MultiFinalReply::ExecAbort : MultiFinalReply::Exec;
        if (!session->queue_error &&
            !prepare_commands(server, *state, session->queue)) {
            destroy_multi_state(state);
            reply_err(op.sink(), "ERR out of memory");
            session->active = false;
            session->queue.clear();
            return MultiIoAction::LocalDone;
        }
        if (!state->write_keys.empty() && server.aof().recording()) {
            std::vector<uint32_t> participants;
            try {
                participants.reserve(state->write_keys.size());
                for (const MultiWriteKey& write : state->write_keys)
                    participants.push_back(static_cast<uint32_t>(write.shard));
            } catch (const std::bad_alloc&) {
                destroy_multi_state(state);
                reply_err(op.sink(), "ERR out of memory");
                session->active = false;
                session->queue.clear();
                return MultiIoAction::LocalDone;
            }
            state->aof_group = aof_create_group(server.aof(), participants);
            if (!state->aof_group) {
                destroy_multi_state(state);
                reply_err(op.sink(), "ERR out of memory");
                session->active = false;
                session->queue.clear();
                return MultiIoAction::LocalDone;
            }
        }
        session->active = false;
        session->queue_error = false;
        session->queue.clear();
        for (const MultiWatchedKey& key : state->watched) add_shard(state->shards, key.shard);
        if (state->shards.empty()) {
            if (state->final_reply == MultiFinalReply::ExecAbort)
                reply_err(op.sink(), "EXECABORT Transaction discarded because of previous errors.");
            else reply_array_header(op.sink(), 0);
            release_admission(*state);
            client.next_watch_generation();
            client.clear_watch_dirty();
            delete state;
            return MultiIoAction::LocalDone;
        }
        state->watch_pending.store(static_cast<uint32_t>(state->shards.size()),
                                   std::memory_order_relaxed);
        state->lists_pending.store(static_cast<uint32_t>(state->shards.size()),
                                   std::memory_order_relaxed);
        state->tasks_pending.store(static_cast<uint32_t>(state->shards.size()),
                                   std::memory_order_relaxed);
        session->pending = state;
        dispatch = state;
        return MultiIoAction::Dispatch;
    }

    if (!session || !session->active) return MultiIoAction::NotHandled;
    if (command_forbidden_in_multi(op)) {
        session->queue_error = true;
        reply_err(op.sink(), "ERR Command not allowed inside a transaction");
        return MultiIoAction::LocalDone;
    }
    MultiQueuedCommand queued;
    if (!queued_copy(op, queued)) {
        session->queue_error = true;
        reply_err(op.sink(), "ERR out of memory");
        return MultiIoAction::LocalDone;
    }
    try { session->queue.push_back(std::move(queued)); }
    catch (const std::bad_alloc&) {
        session->queue_error = true;
        reply_err(op.sink(), "ERR out of memory");
        return MultiIoAction::LocalDone;
    }
    reply_simple(op.sink(), "QUEUED");
    return MultiIoAction::LocalDone;
}

template <bool IofusedPrivateQueue>
bool multi_dispatch_entry_impl(IoLoop& loop, Client& client, Op& op, uint32_t consumed) {
    MultiSession* queue_session = client.multi_session();
    const bool was_queueing = queue_session && queue_session->active;
    const uint64_t op_id = client.rob().dispatch_id();
    if (was_queueing && op_id != queue_session->next_queue_op_id)
        queue_session->queue_error = true;
    MultiExecState* dispatch = nullptr;
    const MultiIoAction action =
        multi_handle_io(*loop.srv_, client, op, loop.self_->id(), dispatch);
    if (action == MultiIoAction::NotHandled) return false;
    if (action == MultiIoAction::Backpressure) {
        client.set_atomic_backpressure(true);
        return true;
    }
    if (was_queueing) queue_session->next_queue_op_id = op_id + 1;
    if (action == MultiIoAction::LocalDone) {
        client.advance_parse(consumed);
        loop.self_->note_command(op.spec->id);
        loop.flip_fingerprint_note(*op.spec, op);
        loop.finish_prebuilt(&client, op);
        return true;
    }

    uint32_t needed[kMaxThreads] = {};
    const uint32_t count = multi_dispatch_count(dispatch);
    for (uint32_t i = 0; i < count; i++) {
        const int32_t sid = multi_dispatch_shard(dispatch, i);
        needed[loop.srv_->worker_of_shard(sid)]++;
    }
    for (uint32_t tid = 0; tid < loop.srv_->nthreads(); tid++) {
        if (!needed[tid]) continue;
        ThreadCtx& owner = loop.srv_->thread(tid);
        const uint32_t free_slots = [&] {
            if constexpr (IofusedPrivateQueue)
                return owner.iofused_task_free_slots(loop.self_->id());
            else
                return owner.task_free_slots(loop.self_->id());
        }();
        if (free_slots < needed[tid]) {
            // Returning through the one parse-loop continuation reaches the existing top-of-loop
            // backpressure check, which has the same effect as the old in-line break.
            client.set_atomic_backpressure(true);
            return true;
        }
    }

    // PIN THE TRANSACTION'S READ CUT, BEFORE ANY FRAGMENT IS POSTED.
    // Drawn here rather than at execution for the same reason a bare cross-shard read draws it at
    // prepare: this is the connection's program order, and register_snapshot publishes the cleanup
    // floor synchronously, so no owner can resolve under a cut whose predecessors cleanup has
    // already been free to collapse. `op` reuses the parse pass's cut when IO stamped one, which
    // keeps a plain read and a transaction prepared back to back on one connection from disagreeing
    // about which of them is older. Untaken -- and therefore free -- for any transaction that
    // carries no multi-owner read.
    if (__builtin_expect(dispatch->exec_read_fanout, false)) {
        if (!loop.scatter_pool_.can_register_snapshot()) {
            // The unresolved-cut window is full on this IO thread. Falling through without a cut
            // would silently restore the defect, so this is backpressure: the state stays in
            // session->pending and the same EXEC is re-dispatched on a later pass.
            client.set_atomic_backpressure(true);
            return true;
        }
        dispatch->snapshot = op.has_read_cut() ? op.read_cut(loop.srv_->atomic_snapshot())
                                               : loop.srv_->atomic_snapshot();
        if (!loop.scatter_pool_.register_snapshot(*loop.srv_, dispatch, loop.self_->id())) {
            dispatch->snapshot = UINT64_MAX;
            client.set_atomic_backpressure(true);
            return true;
        }
        // Vacuous-validation guard: one per transaction that really entered the machinery. A gate
        // row for this defect that never moves this counter never armed the window it claims to
        // close. register_snapshot may have adopted a confirmed newer cut, so the children are
        // stamped from dispatch->snapshot afterwards, never from the value asked for.
        loop.srv_->note_atomic_exec_read_cut();
        for (const auto& command : dispatch->commands) {
            if (command->kind != MultiCommandKind::Xshard || !command->scatter) continue;
            const bool sort_deref_read = command->scatter->kind == Kind::Sort &&
                                         command->scatter->sort_options.deref;
            if (!sort_deref_read &&
                (command->queued.spec->flags & (CmdFlags::Write | CmdFlags::SnapshotWrite)))
                continue;
            if (!sort_deref_read && command->scatter->nsub <= 1) continue;
            // The child never registers on its own: it lives in the transaction's private
            // ScatterArenaPool, which does not own this thread's floor slot. It only borrows the
            // cut the transaction published, and xshard_destroy skips unregistering it because
            // snapshot_registered stays false.
            command->scatter->snapshot = dispatch->snapshot;
        }
        // TEST HOOK arming, after the participant set exists so a lead shard can be named. Only a
        // transaction carrying a multi-owner read arms it -- the writer transaction the battery
        // races must stay free to run and publish its ticket inside the window.
        const uint32_t fanout_defer = loop.srv_->debug_atomic_fanout_defer();
        if (__builtin_expect(fanout_defer != 0, false) && dispatch->shards.size() > 1) {
            dispatch->debug_fanout_lead = dispatch->shards[0];
            dispatch->debug_fanout_deadline =
                now_ns() + static_cast<uint64_t>(fanout_defer) * 1000ull;
        }
    }

    // PIN THE TRANSACTION'S TIME CUT, BEFORE ANY FRAGMENT IS POSTED.
    // Same argument as the read cut above, for the other axis a fan-out can straddle. Unlike the
    // read cut this one is unconditional and free of any registry: a deadline is not a ticket
    // anybody has to wait on, so pinning one costs a single clock read per EXEC and nothing else.
    // The cross-shard children inherit it rather than stamping their own, so a transaction is one
    // instant end to end and not one instant per lowered command.
    dispatch->now_cut_ms = now_realtime_ms();
    for (const auto& command : dispatch->commands)
        if (command->scatter) command->scatter->now_cut_ms = dispatch->now_cut_ms;

    // A pending EXEC may survive queue or snapshot-window backpressure while shard ownership
    // migrates. Derive owner sentinels only on the final, no-failure path immediately before the
    // first task can become visible, so their slots match the routes used by this publication.
    initialize_multi_owner_record_refs(*dispatch);

    Rob<kRobWindow>& rob = client.rob();
    op.attach_multi_state(dispatch);
    client.atomic_group_started();
    rob.publish();
    multi_dispatch_started(client, dispatch);
    LoopSignals& sig = loop.self_->sig();
    for (uint32_t i = 0; i < count; i++) {
        const int32_t sid = multi_dispatch_shard(dispatch, i);
        const uint32_t tid = loop.srv_->worker_of_shard(sid);
        ThreadCtx& owner = loop.srv_->thread(tid);
        const Task task = multi_make_task(&client, op_id, sid, dispatch);
        const bool posted = [&] {
            if constexpr (IofusedPrivateQueue)
                return owner.post_iofused_task_quiet(loop.self_->id(), task, sig);
            else
                return owner.post_task_quiet(loop.self_->id(), task, sig);
        }();
        if (!posted) std::abort();
        if (!loop.touched_[tid]) {
            loop.touched_[tid] = true;
            loop.touched_list_[loop.ntouched_++] = tid;
        }
    }
    loop.self_->note_command(op.spec->id);
    loop.flip_fingerprint_note(*op.spec, op);
    client.advance_parse(consumed);
    sig.ops++;
    // Released by the quiescence backstop in flush_ready: EXEC's parked lifetime IS its ROB slot,
    // so "nothing in flight" and "this transaction is done" are the same instant. Unlike a blocking
    // command, EXEC takes the barrier WITHOUT requiring an empty ROB -- older ops of this
    // connection may still be in flight when it does.
    loop.barrier_arm(&client, BarrierOwner::Exec);
    loop.mark_active(&client);
    return true;
}

bool multi_dispatch_entry(IoLoop& loop, Client& client, Op& op, uint32_t consumed) {
    return multi_dispatch_entry_impl<false>(loop, client, op, consumed);
}

bool multi_dispatch_entry_iofused(IoLoop& loop, Client& client, Op& op, uint32_t consumed) {
    return multi_dispatch_entry_impl<true>(loop, client, op, consumed);
}

void multi_retire_entry(IoLoop& loop, Client& client, Op& op) {
    command_set_local_context(&client, loop.self_);
    snapshot_bind_io(loop.self_, &loop.ring_);
    if (client.is_tls()) {
        auto* state = static_cast<MultiExecState*>(op.multi_state());
        if (state)
            for (const auto& command : state->commands) {
                ScatterState* scatter = command->scatter;
                if (!scatter || scatter->kind != Kind::Mget || !scatter->values) continue;
                for (uint32_t i = 0; i < scatter->key_count; i++) {
                    ValueSlot& slot = scatter->values[i];
                    if (slot.kind != ValueKind::Borrow) continue;
                    loop.queue_borrow_release(slot.shard, slot.ptr);
                    loop.wb_.note_zc_suppressed_tls();
                    slot.kind = ValueKind::Nil;
                }
            }
    }
    // RELEASE THE CUT ONLY AFTER THE LAST FRAGMENT HAS ANSWERED. Retirement is that moment: the
    // final reply is assembled once every participating owner has reported, and this is the one
    // retire path a published transaction takes (io_loop.h routes op.has_multi_state() here). The
    // state object is still alive -- multi_retire below is what releases or defers it.
    if (auto* cut = static_cast<MultiExecState*>(op.multi_state())) {
        if (__builtin_expect(cut->snapshot_registered, false))
            loop.scatter_pool_.unregister_snapshot(*loop.srv_, cut, loop.self_->id());
    }
    multi_retire(client, op, loop.multi_deferred_);
    snapshot_bind_io(nullptr, nullptr);
    command_set_local_context(nullptr, nullptr);
}

uint32_t multi_owner_reap_entry(IoLoop& loop) {
    return multi_reap_deferred(loop.multi_deferred_);
}

template <bool IofusedPrivateQueue>
uint32_t multi_owner_pass_entry_impl(IoLoop& loop) {
    uint32_t work = multi_owner_reap_entry(loop);
    while (!loop.pending_multi_cleanups_.empty()) {
        MultiExecState* state = loop.pending_multi_cleanups_.front();
        uint32_t needed[kMaxThreads] = {};
        const uint32_t count = multi_dispatch_count(state);
        for (uint32_t i = 0; i < count; i++)
            needed[loop.srv_->worker_of_shard(multi_dispatch_shard(state, i))]++;
        bool room = true;
        for (uint32_t tid = 0; tid < loop.srv_->nthreads(); tid++) {
            if (!needed[tid]) continue;
            ThreadCtx& owner = loop.srv_->thread(tid);
            const uint32_t free_slots = [&] {
                if constexpr (IofusedPrivateQueue)
                    return owner.iofused_task_free_slots(loop.self_->id());
                else
                    return owner.task_free_slots(loop.self_->id());
            }();
            if (free_slots < needed[tid]) {
                room = false;
                break;
            }
        }
        if (!room) break;
        for (uint32_t i = 0; i < count; i++) {
            const int32_t sid = multi_dispatch_shard(state, i);
            ThreadCtx& owner = loop.srv_->thread(loop.srv_->worker_of_shard(sid));
            const Task task = multi_make_task(nullptr, UINT64_MAX, sid, state);
            const bool posted = [&] {
                if constexpr (IofusedPrivateQueue)
                    return owner.post_iofused_task(
                        loop.self_->id(), task, loop.ring_, loop.self_->sig());
                else
                    return owner.post_task(
                        loop.self_->id(), task, loop.ring_, loop.self_->sig());
            }();
            if (!posted)
                std::abort();
        }
        multi_internal_dispatch_started(state);
        loop.pending_multi_cleanups_.pop_front();
        work++;
    }
    return work;
}

uint32_t multi_owner_pass_entry(IoLoop& loop) {
    return multi_owner_pass_entry_impl<false>(loop);
}

uint32_t multi_owner_pass_entry_iofused(IoLoop& loop) {
    return multi_owner_pass_entry_impl<true>(loop);
}

void multi_close_entry(IoLoop& loop, Client& client) {
    if (MultiExecState* cleanup = multi_prepare_close(*loop.srv_, client, loop.self_->id()))
        loop.pending_multi_cleanups_.push_back(cleanup);
}

void multi_shutdown_entry(IoLoop& loop) {
    for (MultiExecState* state : loop.pending_multi_cleanups_)
        multi_abandon_unpublished(state);
    loop.pending_multi_cleanups_.clear();
}

uint32_t multi_dispatch_count(const MultiExecState* state) {
    return state ? static_cast<uint32_t>(state->shards.size()) : 0;
}

int32_t multi_dispatch_shard(const MultiExecState* state, uint32_t index) {
    return state && index < state->shards.size() ? state->shards[index] : -1;
}

void multi_dispatch_started(Client& client, MultiExecState* state) {
    if (!state) return;
    state->published = true;
    MultiSession* session = client.multi_session();
    if (session && session->pending == state) session->pending = nullptr;
}

void multi_internal_dispatch_started(MultiExecState* state) {
    if (state) state->published = true;
}

void multi_abandon_unpublished(MultiExecState* state) {
    if (!state || state->published) return;
    clear_unpublished_multi_record_refs(*state);
    destroy_multi_state(state);
}

bool multi_task_tagged(const Task& task) {
    return (reinterpret_cast<uintptr_t>(task.scatter) & kMultiTaskTag) != 0;
}

bool multi_task_owns_epoch(const Task& task, const void* group_epoch) {
    const uintptr_t tagged = reinterpret_cast<uintptr_t>(task.scatter);
    const auto* state = reinterpret_cast<const MultiExecState*>(tagged & ~kMultiTaskTag);
    return state && multi_state_owns_epoch(*state, group_epoch);
}

// INFO atomic_exec_order_late. Defined HERE, outside this file's anonymous namespace, because
// t_server.cc calls it through multi.h: inside, it would be internal-linkage and the reference
// from the other translation unit would not resolve.
uint64_t multi_exec_order_late() { return g_exec_order_late.load(std::memory_order_relaxed); }

Task multi_make_task(Client* client, uint64_t op_id, int32_t shard, MultiExecState* state) {
    const uintptr_t tagged = reinterpret_cast<uintptr_t>(state) | kMultiTaskTag;
    return Task{client, op_id, shard, reinterpret_cast<ScatterState*>(tagged)};
}

bool multi_aof_emit_fragment(MultiExecState& state, Shard& shard) {
    if (!state.aof_group) return true;
    uint8_t& done = state.aof_shard_done[static_cast<uint32_t>(shard.id())];
    if (done) return true;
    uint32_t count = 0;
    for (const MultiWriteKey& write : state.write_keys) count += write.shard == shard.id();
    if (!count) { done = 1; return true; }
    AofProducer& producer = shard.store().aof();
    if (!producer.begin_group(state.aof_group)) return false;
    // Runtime-error children keep any already-installed candidates private under their own abort
    // word. The physical table head may therefore be a rejected RENAMENX source tombstone. Resolve
    // the transaction's own overlay and record its LOGICAL final image, which skips that child but
    // still sees successful private candidates from the same connection.
    shard.store().atomic_set_read_context(UINT64_MAX, state.origin_conn_id);
    bool ok = true;
    for (const MultiWriteKey& write : state.write_keys) {
        if (write.shard != shard.id()) continue;
        const Slice key(write.key.data(), static_cast<uint32_t>(write.key.size()));
        ok &= producer.record_group_visible_post_image(shard.store(), write.hash, key);
    }
    shard.store().atomic_clear_read_epoch();
    ok &= producer.finish_group();
    done = 1;
    return ok;
}

MultiTaskResult multi_execute_task(Server& server, const Task& task, Shard& shard,
                                   uint32_t owner_thread_id, uint32_t owner_domain,
                                   AofOwnerContext* aof_context) {
    // A carrier-less fragment is not an error: multi_close_entry posts the watch-release fragment
    // of a vanished connection with client=nullptr and op_id=UINT64_MAX, and this function's own
    // tail already destroys the state on that path. Only the notify scope wanted an Op, and a
    // fragment with no client raises no notifications, so it takes the pointer form.
    Op* carrier = task.client && task.op_id != UINT64_MAX
        ? &task.client->rob().at(task.op_id) : nullptr;
    const bool notify = carrier && (carrier->spec->flags & CmdFlags::NotifySelected);
    NotifyExecutionScope notify_scope(shard, carrier, notify);
    const uintptr_t tagged = reinterpret_cast<uintptr_t>(task.scatter);
    auto& state = *reinterpret_cast<MultiExecState*>(tagged & ~kMultiTaskTag);
    // ONE EXPIRY CUT FOR THE WHOLE TRANSACTION, on every owner and for every fragment including the
    // WATCH phase below -- redis validates watches inside EXEC's own call, under EXEC's snapshot.
    // Without this, two identical reads in one EXEC could disagree about the same key.
    PinnedNowMs now_cut(shard, state.now_cut_ms);
    uint8_t& noted = state.execution_noted[static_cast<uint32_t>(shard.id())];
    if (!(noted & 1u)) {
        shard.note_execution(owner_domain);
        noted |= 1u;
    }
    if (__builtin_expect(atomic_tripwire_enabled(), false) && !(noted & 2u)) {
        multi_tripwire_fragment_begin(state, shard);
        noted |= 2u;
    }
    if (!state.watch_phase_done.load(std::memory_order_acquire)) {
        if (!execute_watch_phase(state, shard)) return MultiTaskResult::Retry;
        if (!state.watch_phase_done.load(std::memory_order_acquire)) return MultiTaskResult::Retry;
    }

    // TEST HOOK (DEBUG ATOMIC-FANOUT-DEFER), the MULTI half. Park every fragment of this
    // transaction except the one on its lead shard until the deadline, so the foreign transaction
    // the battery races really does commit BETWEEN two fragments of one in-EXEC read. It sits
    // AFTER the watch phase deliberately: that phase is an all-participants barrier, so parking
    // ahead of it would hold the lead back too and no fragment would run early. A park, not a
    // stall -- Retry re-queues the task on multi_retries_ and the owner returns to its queue, so
    // the writer is free to execute and publish its ticket inside the window. Off, this is one
    // load of a word the task has already touched.
    if (__builtin_expect(state.debug_fanout_deadline != 0, false) &&
        shard.id() != state.debug_fanout_lead &&
        now_ns() < state.debug_fanout_deadline)
        return MultiTaskResult::Retry;

    const uint32_t shard_id = static_cast<uint32_t>(shard.id());
    std::vector<uint32_t>& command_list = state.shard_commands[shard_id];
    uint32_t& cursor = state.shard_cursors[shard_id];
    if (state.kind != MultiStateKind::Execute ||
        state.final_reply == MultiFinalReply::ExecAbort ||
        state.watch_failed.load(std::memory_order_acquire)) {
        cursor = static_cast<uint32_t>(command_list.size());
    }

    while (cursor < command_list.size()) {
        MultiCommand& command = *state.commands[command_list[cursor]];
        if (command.finished.load(std::memory_order_acquire)) {
            cursor++;
            continue;
        }
        const uint32_t stage = command.stage.load(std::memory_order_acquire);
        uint8_t& done = command.shard_done[shard_id];
        if (command.kind == MultiCommandKind::Xshard) {
            // Membership is published with `stage`, not discovered from ScatterState::groups.
            // The latter is a reusable table: source/gather/apply transitions rewrite it on the
            // last owner of the old stage.  Reading it from a barrier-only task before the release
            // store below races that rewrite and can execute a partially built SORT gather group.
            if (!multi_xshard_stage_has_group(command, stage, shard.id())) {
                if (command.finished.load(std::memory_order_acquire)) continue;
                return MultiTaskResult::Retry;
            }
            if (done == stage + 1) {
                if (command.finished.load(std::memory_order_acquire)) continue;
                return MultiTaskResult::Retry;
            }
            if (notify)
                notify_execute_source(shard, *command.op, command.notify_order_base);
            const Task child_task{state.client, task.op_id, shard.id(), command.scatter};
            bool execute_child = true;
            if (command.scatter->atomic_pop && command.scatter->phase == 2) {
                ShardGroup* group = group_for(*command.scatter, shard.id());
                if (!group || group->count != 1) std::abort();
                const uint32_t pos = command.scatter->key_order[group->begin];
                const KeyRef& selected = command.scatter->keys[pos];
                const MultiPrepareWrite prepared = prepare_write_key(
                    server, state, shard, command.op->arg(selected.arg), selected.hash,
                    owner_thread_id, command.scatter, &command.scatter->watch_refs);
                if (prepared == MultiPrepareWrite::Retry) return MultiTaskResult::Retry;
                if (prepared == MultiPrepareWrite::Error) {
                    state.aborted.store(true, std::memory_order_release);
                    command.scatter->aborted.store(true, std::memory_order_release);
                    execute_child = false;
                }
            }
            const ScatterTaskResult executed = execute_child
                ? xshard_execute(child_task, shard, *command.op, owner_thread_id)
                : ScatterTaskResult::Complete;
            xshard_watch_finish(child_task, shard, *command.op, executed);
            if (executed == ScatterTaskResult::Retry) return MultiTaskResult::Retry;
            done = static_cast<uint8_t>(stage + 1);
            const MultiChildFinish finished = xshard_multi_child_complete(
                *command.scatter, *command.op, owner_thread_id);
            if (finished == MultiChildFinish::PhaseAdvanced) {
                const uint32_t next_stage = stage + 1;
                if (next_stage >= MultiCommand::kXshardStages) std::abort();
                // xshard_multi_child_complete has finished rebuilding groups.  Snapshot their
                // membership first; the release store then publishes both the group contents and
                // this immutable routing decision to every retrying transaction fragment.
                xshard_multi_child_capture_groups(
                    *command.scatter, command.xshard_stage_groups[next_stage]);
                command.stage.store(next_stage, std::memory_order_release);
            } else if (finished == MultiChildFinish::Final) {
                if (notify)
                    notify_xshard_finished(shard, *command.op, *command.scatter);
                assemble_final(*state.client, *command.op, *command.scatter,
                               nullptr, nullptr, nullptr);
                command.finished.store(true, std::memory_order_release);
            }
            continue;
        }
        if (done == stage + 1) {
            if (command.finished.load(std::memory_order_acquire)) continue;
            return MultiTaskResult::Retry;
        }
        bool executed = true;
        if (state.aborted.load(std::memory_order_acquire)) {
            // Still join the command barrier; another owner may already have installed a
            // private candidate which the shared abort flag will hide.
        } else if (command.kind == MultiCommandKind::Local ||
                   command.kind == MultiCommandKind::Scan) {
            executed = execute_local_command(server, state, command, shard, owner_thread_id);
        } else {
            executed = execute_cross_command(
                server, state, command, shard, owner_thread_id, stage);
        }
        if (!executed) return MultiTaskResult::Retry;
        done = static_cast<uint8_t>(stage + 1);
        finish_command(command, stage);
    }

    uint8_t& joined = state.execution_joined[shard_id];
    if (!joined) {
        joined = 1;
        // Each shard emits its AOF fragment exactly once, under the same join guard that
        // publishes its list's owner work; a failed emit aborts the whole group (GCMT).
        if (!multi_aof_emit_fragment(state, shard)) aof_abort_group(state.aof_group);
        // This first barrier publishes every list's owner work before the one commit decision.
        // tasks_pending remains the post-decision lifetime barrier for state and reservations.
        if (state.lists_pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            bool expected = false;
            if (state.finalizer_claimed.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel)) {
                if (state.watch_failed.load(std::memory_order_acquire))
                    state.aborted.store(true, std::memory_order_release);
                if (!state.aborted.load(std::memory_order_acquire) &&
                    state.final_reply == MultiFinalReply::Exec) {
                    assemble_ownerless_replies(state);
                    const uint64_t ticket = server.atomic_commit_group(
                        state.epoch,
                        [&](uint64_t shared) { publish_multi_child_epochs(state, shared); });
                    if (aof_context)
                        (void)aof_commit_group(server.aof(), state.aof_group, ticket,
                                               *aof_context);
                } else {
                    // Child entries no longer point at the parent's abort word, because doing so
                    // made a normal child error abort every other command. A genuine transaction
                    // abort still has to decide all of them before reservations are finalized.
                    abort_multi_children(state);
                    aof_abort_group(state.aof_group);
                }
                state.finalized.store(true, std::memory_order_release);
                // Every owner has joined and the one commit decision is made: nothing this
                // transaction writes can land on a shard after this point, so it can no longer
                // straddle a snapshot cut. Closed here, on an OWNER, not at reply retirement.
                server.atomic_apply_close(owner_thread_id, state.apply_open);
            }
        }
    }
    if (!state.finalized.load(std::memory_order_acquire)) return MultiTaskResult::Retry;
    finalize_reservations(state, shard);
    complete_multi_owner_record_refs(state, owner_thread_id);
    if (state.tasks_pending.fetch_sub(1, std::memory_order_acq_rel) != 1)
        return MultiTaskResult::Complete;

    if (task.op_id == UINT64_MAX) {
        destroy_multi_state(&state);
        return MultiTaskResult::Complete;
    }
    return MultiTaskResult::Final;
}

void multi_retire(Client& client, Op& op, std::vector<MultiExecState*>& deferred) {
    auto* state = static_cast<MultiExecState*>(op.multi_state());
    if (!state) return;
    if (state->kind == MultiStateKind::Execute &&
        state->final_reply == MultiFinalReply::Exec &&
        !state->watch_failed.load(std::memory_order_acquire) &&
        !state->aborted.load(std::memory_order_acquire)) {
        // Connection-local handlers deliberately return to their owning IO thread.  This preserves
        // their thread-local client/snapshot context and keeps every shard-owning handler on EX.
        for (const auto& command : state->commands)
            if (command->kind == MultiCommandKind::IoLocal)
                command->op->spec->handler(state->server->shard(0), *command->op);
    }
    set_state_reply(*state, op);
    op.detach_scatter_state();
    client.atomic_group_finished();
    if (state->reset_watch) {
        client.next_watch_generation();
        client.clear_watch_dirty();
    }
    release_admission(*state);
    state->retired = true;
    if (multi_records_done(*state)) {
        destroy_multi_state(state);
        return;
    }
    try { deferred.push_back(state); }
    catch (const std::bad_alloc&) {
        // The record refs keep the state valid.  An allocation failure leaks one transaction state
        // rather than freeing epoch/abort words still named by shard-local MVCC entries.
    }
}

uint32_t multi_reap_deferred(std::vector<MultiExecState*>& deferred) {
    uint32_t reaped = 0;
    for (size_t i = 0; i < deferred.size();) {
        MultiExecState* state = deferred[i];
        if (!multi_records_done(*state)) { i++; continue; }
        destroy_multi_state(state);
        deferred[i] = deferred.back();
        deferred.pop_back();
        reaped++;
    }
    return reaped;
}

MultiExecState* multi_prepare_close(Server& server, Client& client, uint32_t owner_io) {
    MultiSession* session = client.multi_session();
    if (!session) return nullptr;
    if (session->pending && !session->pending->published) {
        MultiExecState* pending = session->pending;
        session->pending = nullptr;
        multi_abandon_unpublished(pending);
    }
    session->active = false;
    session->queue_error = false;
    session->queue.clear();
    if (session->watched.empty()) return nullptr;
    MultiExecState* state = prepare_unwatch_state(
        server, client, owner_io, *session, MultiStateKind::Close, MultiFinalReply::Ok);
    return state;
}

void multi_session_destroy(MultiSession* session) {
    if (!session) return;
    if (session->pending && !session->pending->published)
        multi_abandon_unpublished(session->pending);
    delete session;
}

bool multi_session_active(const Client& client) {
    const MultiSession* session = client.multi_session();
    return session && session->active;
}

uint32_t multi_session_queue_size(const Client& client) {
    const MultiSession* session = client.multi_session();
    return session && session->active ? static_cast<uint32_t>(session->queue.size()) : 0;
}

uint32_t multi_session_watch_size(const Client& client) {
    const MultiSession* session = client.multi_session();
    return session ? static_cast<uint32_t>(session->watched.size()) : 0;
}

uint64_t multi_session_memory(const Client& client) {
    const MultiSession* session = client.multi_session();
    if (!session) return 0;
    uint64_t bytes = 0;
    for (const MultiQueuedCommand& command : session->queue)
        for (const std::string& arg : command.argv) bytes += arg.size();
    return bytes;
}

namespace {

std::string watch_key_string(Slice key) { return std::string(key.p, key.n); }

template <typename Fn>
void for_each_spec_key(Op& op, Fn&& fn) {
    // EVAL/FCALL's legacy registry range extends through ARGV so generic command metadata can
    // express a variable key count.  The numkeys field is the authoritative touched range: treating
    // trailing arguments as keys spuriously dirties an unrelated WATCH when a script command runs
    // between WATCH and EXEC.
    if (op.spec && (op.spec->flags & CmdFlags::ScriptRoute)) {
        uint32_t first = 0, count = 0;
        if (!command_script_key_range(op, first, count)) return;
        for (uint32_t i = 0; i < count; i++) fn(op.arg(first + i));
        return;
    }
    if (!op.spec || op.spec->first_key <= 0 || op.spec->key_step <= 0) return;
    const uint32_t last = op.spec->last_key < 0
        ? op.argc() - 1
        : std::min<uint32_t>(static_cast<uint32_t>(op.spec->last_key), op.argc() - 1);
    for (uint32_t arg = static_cast<uint32_t>(op.spec->first_key); arg <= last;
         arg += static_cast<uint32_t>(op.spec->key_step))
        fn(op.arg(arg));
}

}  // namespace

// Retires every reservation on `key` whose unit has DECIDED, applying its effect, and answers
// whether anything still blocks a foreign unit on this key.
//
// "Decided" is epoch != 0 (the unit published a commit ticket) or aborted. An undecided
// reservation is kept. Only an undecided BLOCKING reservation -- the EXEC validate claim --
// makes this return false; an undecided atomic-group write reservation is left in place and
// answers "ready", because nothing may wait on it (Shard::WatchReservation::blocking).
bool Shard::watch_finalize_reservation(const std::string& key) {
    auto found = watch_reservations_.find(key);
    if (found == watch_reservations_.end()) return true;
    std::vector<WatchReservation>& list = found->second;
    bool blocked = false;
    bool dirty = false;
    size_t keep = 0;
    for (size_t i = 0; i < list.size(); i++) {
        WatchReservation& reservation = list[i];
        const bool aborted = reservation.aborted &&
            reservation.aborted->load(std::memory_order_acquire);
        const uint64_t epoch = reservation.epoch
            ? reservation.epoch->load(std::memory_order_acquire) : 1;
        if (!aborted && epoch == 0) {
            if (reservation.blocking) {
                stats_.watch_reservation_waits++;
                blocked = true;
            }
            if (keep != i) list[keep] = reservation;
            keep++;
            continue;
        }
        if (!aborted && reservation.mutates) dirty = true;
        if (reservation.refs) reservation.refs->fetch_sub(1, std::memory_order_release);
    }
    list.resize(keep);
    if (dirty) {
        auto watchers = watchers_.find(key);
        if (watchers != watchers_.end()) {
            for (const WatchEntry& entry : watchers->second)
                if (entry.client && entry.client->watch_generation() == entry.generation)
                    entry.client->set_watch_dirty();
        }
    }
    if (list.empty()) watch_reservations_.erase(found);
    return !blocked;
}

void Shard::watch_prune_stale(const std::string& key) {
    auto found = watchers_.find(key);
    if (found == watchers_.end()) return;
    auto& entries = found->second;
    size_t keep = 0;
    for (const Shard::WatchEntry& entry : entries) {
        if (entry.client && entry.client->watch_generation() == entry.generation) {
            entries[keep++] = entry;
        } else if (entry.client) {
            entry.client->watch_unref();
        }
    }
    entries.resize(keep);
    if (entries.empty()) watchers_.erase(found);
}

bool Shard::watch_add(Slice key, Client* client, uint64_t generation) {
    const std::string owned = watch_key_string(key);
    if (!watch_finalize_reservation(owned)) return false;
    watch_prune_stale(owned);
    auto& entries = watchers_[owned];
    for (const WatchEntry& entry : entries)
        if (entry.client == client && entry.generation == generation) return true;
    const int64_t deadline = store_.watch_deadline(FlatStore::hash_key(key), key);
    try { entries.push_back(WatchEntry{client, generation, deadline}); }
    catch (const std::bad_alloc&) {
        if (entries.empty()) watchers_.erase(owned);
        return false;
    }
    client->watch_ref();
    return true;
}

void Shard::watch_remove(Slice key, Client* client, uint64_t generation) {
    const std::string owned = watch_key_string(key);
    auto found = watchers_.find(owned);
    if (found == watchers_.end()) return;
    auto& entries = found->second;
    size_t keep = 0;
    for (const WatchEntry& entry : entries) {
        if (entry.client == client && entry.generation == generation) client->watch_unref();
        else entries[keep++] = entry;
    }
    entries.resize(keep);
    if (entries.empty()) watchers_.erase(found);
}

bool Shard::watch_validate_and_reserve(Slice key, Client* client, uint64_t generation,
                                       const void* token, std::atomic<uint64_t>* epoch,
                                       std::atomic<bool>* aborted,
                                       std::atomic<uint32_t>* refs, bool mutates) {
    const std::string owned = watch_key_string(key);
    if (auto existing = watch_reservations_.find(owned); existing != watch_reservations_.end())
        for (WatchReservation& reservation : existing->second)
            if (reservation.token == token) {
                reservation.mutates |= mutates;
                return true;
            }
    if (!watch_finalize_reservation(owned)) return false;
    // Everything still on this key is now an UNDECIDED atomic-group write, and this transaction no
    // longer waits for it. Waiting was one half of the wait-for cycle, and it buys nothing that is
    // not already available here: a group holding a reservation on this key is a write to a key
    // this client watched, and "the group lands first" is a realizable serialization. Take it, and
    // this transaction must abort -- which is exactly what WATCH promises. The alternative reading
    // (the group lands after) is only reachable by waiting for a decision that may itself be
    // waiting on this transaction.
    if (auto pending = watch_reservations_.find(owned); pending != watch_reservations_.end())
        for (const WatchReservation& reservation : pending->second)
            if (reservation.token != token && reservation.mutates) {
                client->set_watch_dirty();
                stats_.watch_reservation_precommit_aborts++;
                break;
            }
    // A key that was alive when this client armed the WATCH and whose deadline has since elapsed is
    // a change, even when nothing has reaped it yet: redis aborts EXEC in that case whether the
    // removal came from its active cycle or is still pending. No command wrote the key, so the
    // reservation path never hears about it -- the armed deadline is the whole record.
    if (const auto watched = watchers_.find(owned); watched != watchers_.end()) {
        const int64_t now = now_ms_;
        for (const WatchEntry& entry : watched->second)
            if (entry.client == client && entry.generation == generation &&
                entry.expire_at_ms >= 0 && entry.expire_at_ms <= now) {
                client->set_watch_dirty();
                break;
            }
    }
    if (client->watch_generation() != generation || client->watch_dirty()) return true;
    if (!watch_append_reservation(owned, WatchReservation{
            token, epoch, aborted, refs, client, generation, mutates, /*blocking=*/true}))
        aborted->store(true, std::memory_order_release);
    return true;
}

bool Shard::watch_write_ready(Slice key, const void* token) {
    if (!has_watches()) return true;
    const std::string owned = watch_key_string(key);
    if (auto existing = watch_reservations_.find(owned); existing != watch_reservations_.end())
        for (const WatchReservation& reservation : existing->second)
            if (reservation.token == token) return true;
    return watch_finalize_reservation(owned);
}

void Shard::watch_reserve_write(Slice key, Client* writer, uint64_t writer_generation,
                                const void* token, std::atomic<uint64_t>* epoch,
                                std::atomic<bool>* aborted,
                                std::atomic<uint32_t>* refs) {
    if (!has_watches()) return;
    const std::string owned = watch_key_string(key);
    if (auto existing = watch_reservations_.find(owned); existing != watch_reservations_.end())
        for (WatchReservation& reservation : existing->second)
            if (reservation.token == token) {
                reservation.mutates = true;
                return;
            }
    if (watchers_.find(owned) == watchers_.end()) return;
    // Never exclusive, never a wait: this group's reservation sits alongside whatever else is
    // undecided on the key, and each one dirties the key's watchers at its OWN decision.
    if (auto found = watch_reservations_.find(owned);
        found != watch_reservations_.end() && !found->second.empty())
        stats_.watch_reservation_coexist++;
    if (!watch_append_reservation(owned, WatchReservation{
            token, epoch, aborted, refs, writer, writer_generation, true, /*blocking=*/false}))
        aborted->store(true, std::memory_order_release);
}

// Appends one reservation, leaving the map unchanged on allocation failure. Returns false when the
// caller must abort its unit rather than proceed with an unrecorded reservation.
bool Shard::watch_append_reservation(const std::string& key, const WatchReservation& reservation) {
    try {
        std::vector<WatchReservation>& list = watch_reservations_[key];
        list.push_back(reservation);
        if (reservation.refs) reservation.refs->fetch_add(1, std::memory_order_release);
    } catch (const std::bad_alloc&) {
        auto found = watch_reservations_.find(key);
        if (found != watch_reservations_.end() && found->second.empty())
            watch_reservations_.erase(found);
        return false;
    }
    return true;
}

void Shard::watch_write_committed(Slice key, Client*, uint64_t) {
    if (!has_watches()) return;
    const std::string owned = watch_key_string(key);
    if (!watch_finalize_reservation(owned)) return;
    watch_prune_stale(owned);
    auto found = watchers_.find(owned);
    if (found == watchers_.end()) return;
    for (const WatchEntry& entry : found->second)
        if (entry.client && entry.client->watch_generation() == entry.generation)
            entry.client->set_watch_dirty();
}

bool Shard::watch_all_write_ready() {
    if (!has_watches()) return true;
    std::vector<std::string> keys;
    try {
        keys.reserve(watch_reservations_.size());
        for (const auto& item : watch_reservations_) keys.push_back(item.first);
    } catch (const std::bad_alloc&) {
        return false;
    }
    for (const std::string& key : keys)
        if (!watch_finalize_reservation(key)) return false;
    return true;
}

void Shard::watch_all_write_committed() {
    if (!has_watches()) return;
    if (!watch_all_write_ready()) return;
    for (auto& item : watchers_)
        for (const WatchEntry& entry : item.second)
            if (entry.client && entry.client->watch_generation() == entry.generation)
                entry.client->set_watch_dirty();
}

void xshard_watch_finish(const Task& task, Shard& shard, Op& op,
                         ScatterTaskResult result) {
    if (result != ScatterTaskResult::Complete || !task.scatter || !shard.has_watches()) return;
    ScatterState& state = *task.scatter;
    if (!xshard_owner_mutation_phase(state, op) || state.atomic_group) return;
    ShardGroup* group = group_for(state, shard.id());
    if (!group || group->count == 0) {
        shard.watch_all_write_committed();
        return;
    }
    for (uint32_t i = 0; i < group->count; i++) {
        const uint32_t pos = state.key_order[group->begin + i];
        shard.watch_write_committed(op.arg(state.keys[pos].arg));
    }
}

bool multi_plain_write_ready(Shard& shard, Op& op) {
    if (!shard.has_watches()) return true;
    bool ready = true;
    for_each_spec_key(op, [&](Slice key) { ready &= shard.watch_write_ready(key); });
    return ready;
}

void multi_plain_write_committed(Shard& shard, Op& op) {
    if (!shard.has_watches()) return;
    for_each_spec_key(op, [&](Slice key) { shard.watch_write_committed(key); });
}

}  // namespace tomo

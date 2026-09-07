// server.h — process-wide shared state. Everything here is either immutable after boot or
// explicitly synchronised; nothing in it is written on the hot path.
//
// THAT IS THE RULE, and it is worth stating because it is easy to violate by accident. A counter
// bumped per command from a struct every thread shares is a shared-line write on the hot path, and
// at this thread count it shows up immediately. Per-command counters live in Shard::Stats or
// LoopSignals, both single-writer; INFO sums them on request.
//
// THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
// dispatch. Router's packed bucket array remains the bucket-granularity authority; shard_owner_ is
// its derived shard-granularity fast path, published only at a quiesced commit. See
// NOTES-MIGRATE.md for the handoff and stale-route forwarding contract.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <memory>
#include <mutex>
#include <new>
#include <sys/resource.h>
#include <unordered_map>
#include <vector>
#include "shard.h"
#include "thread.h"
#include "flipctl.h"
#include "weighted_lb.h"
#include "placement.h"
#include "config.h"        // struct Config: every runtime knob, one home
#include "live_config.h"
#include "atomic_admission.h"
#include "../base/topology.h"
#include "../net/conn.h"   // kRobWindow: one source of truth for the window size
#include "../net/wb.h"
#include "../cmd/command.h"
#include "../snapshot/snapshot.h"
#include "../persist/aof.h"

#ifdef TOMO_RL_CACHE_DEBUG
#include <cstdio>
#endif

namespace tomo {

struct LbClientObservation {
    uint64_t id = 0;
    uint64_t dispatched_ops = 0;
    uint32_t pipeline_depth = 0;
};

struct LbClientSignal {
    uint64_t last_ops = 0;
    double weight = 0;
    uint64_t last_move_ms = 0;
    uint32_t owner = UINT32_MAX;
};

enum class FlipStage : uint8_t {
    Idle = 0,
    Planning,
    IoDrain,
    IoPrepare,
    ExDrain,
    ClientPrepare,
    ClientCommit,
    ClientInstall,
    RoleReady,
    ShardCommit,
    ExInstall,
    Rollback,
};

enum class LbStage : uint8_t {
    Idle = 0,
    ExDrain,
    ClientDrain,
    ClientMoving,
};

struct LbShardMove {
    uint32_t sid = UINT32_MAX;
    uint32_t source = UINT32_MAX;
    uint32_t destination = UINT32_MAX;
    double weight = 0;
    uint64_t bytes = 0;
};

struct LbClientMove {
    uint64_t id = 0;
    uint32_t source = UINT32_MAX;
    uint32_t destination = UINT32_MAX;
    double weight = 0;
};

struct FlipReport {
    uint32_t live_io = 0;
    uint32_t live_ex = 0;
    uint32_t target_io = 0;
    uint32_t target_ex = 0;
    uint32_t smt_mode = 0;
    uint32_t unit_threads = 1;
    uint32_t bucket_min = 0;
    uint32_t bucket_max = 0;
    uint32_t client_min = 0;
    uint32_t client_max = 0;
    uint64_t last_transfers = 0;
    bool moving = false;
};

// Allocated only for the boot-armed fused read-local lane. The Server keeps only one pointer at its
// true tail, so baseline member offsets and cache-line sharing remain unchanged.
struct ReadLocalServerState {
    std::atomic<uint64_t> epoch{1};
};

// A snapshot/placement drain is the only reader, while opens and closes happen on unrelated
// physical threads.  Monotonic halves avoid the false-zero a signed per-thread delta can expose
// when a scan observes an executor's close before the admitting IO's open.  Cache-line spacing
// keeps those writers independent.
struct alignas(64) AtomicApplySlot {
    std::atomic<uint64_t> opened{0};
    std::atomic<uint64_t> closed{0};
};
static_assert(sizeof(AtomicApplySlot) == 64);
static_assert(alignof(AtomicApplySlot) == 64);

class Server {
public:
    static constexpr uint64_t kAtomicEnabledBit = uint64_t{1} << 63;

    Server() = default;
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool init(const Config& cfg, const AofReplayPlan* aof_replay = nullptr) {
        cfg_ = cfg;
        if (cfg.shards == 0 || cfg.shards > 256) {
            std::fprintf(stderr, "shards must be between 1 and 256\n");
            return false;
        }
        if (cfg.maxmemory_samples == 0 || cfg.maxmemory_samples > 64) {
            std::fprintf(stderr, "maxmemory-samples must be between 1 and 64\n");
            return false;
        }
        // #77 diagnostics ship dark with DEBUG itself. The registry is process-cold heap state,
        // initialized before any worker exists; no Server/Shard/MVCC object gains a field.
        atomic_tripwire_configure(cfg.enable_debug_command != DebugCommandMode::No);
        live_maxmemory_.store(cfg.maxmemory, std::memory_order_relaxed);
        live_maxmemory_policy_.store(static_cast<uint8_t>(cfg.maxmemory_policy),
                                     std::memory_order_relaxed);
        live_maxmemory_samples_.store(cfg.maxmemory_samples, std::memory_order_relaxed);
        live_maxclients_.store(cfg.maxclients, std::memory_order_relaxed);
        live_timeout_.store(cfg.timeout, std::memory_order_relaxed);
        live_tcp_keepalive_.store(cfg.tcp_keepalive, std::memory_order_relaxed);
        store_client_output_buffer_limits(cfg.client_output_buffer_limits);
        refresh_client_cron_armed();
        security_flags_.store(cfg.requirepass && *cfg.requirepass ? kSecurityAuth : 0,
                              std::memory_order_relaxed);
        protected_mode_.store(cfg.protected_mode != 0, std::memory_order_relaxed);
        live_notify_events_.store(cfg.notify_events, std::memory_order_relaxed);
        live_slowlog_us_.store(cfg.slowlog_log_slower_than, std::memory_order_relaxed);
        live_latency_ms_.store(cfg.latency_monitor_threshold, std::memory_order_relaxed);
        live_save_armed_.store(!cfg.save.empty(), std::memory_order_relaxed);
        live_proto_max_bulk_len_.store(cfg.proto_max_bulk_len, std::memory_order_relaxed);
        save_clauses_ = cfg.save;
        atomic_activity_.store(cfg.atomic ? kAtomicEnabledBit : 0,
                               std::memory_order_relaxed);
        // AUTO resolves against the shard count: the measured three-point optimum (see config.h).
        const uint32_t resolved_window = cfg.atomic_window == Config::kAtomicWindowAuto
            ? std::min<uint32_t>(16u * cfg.shards, 1024u)
            : cfg.atomic_window;
        cfg_.atomic_window = resolved_window;
        atomic_credits_.init(resolved_window);
        const uint64_t auto_stage = cfg.maxmemory
            ? std::max<uint64_t>(4ull * 1024 * 1024,
                  std::min<uint64_t>(cfg.maxmemory / cfg.shards / 16, 64ull * 1024 * 1024))
            : 4ull * 1024 * 1024;
        if (cfg_.script_crossshard_max_bytes == -1)
            cfg_.script_crossshard_max_bytes = static_cast<int64_t>(auto_stage);
        if (cfg_.script_crossshard_workbench_bytes == -1)
            cfg_.script_crossshard_workbench_bytes = static_cast<int64_t>(auto_stage * 2);
        if (cfg_.script_crossshard_conflict_retries == -1)
            cfg_.script_crossshard_conflict_retries = 8;
        if (cfg_.script_crossshard_cut_slots == -1)
            cfg_.script_crossshard_cut_slots = 4;
        live_config_version_.store(2, std::memory_order_release);  // even versions are stable
        // Declared topology is a legacy lowering input and therefore cannot accompany --place.
        // A declaration that fails to parse or names cpus outside the affinity mask fails the BOOT,
        // loudly: silently falling back to discovery would measure a different layout.
        if (cfg.place && cfg.l3_domains && *cfg.l3_domains) {
            std::fprintf(stderr, "fatal: --place and --l3-domains are mutually exclusive\n");
            return false;
        }
        if (cfg.l3_domains && *cfg.l3_domains) {
            if (!topo_.declare(cfg.l3_domains)) {
                std::fprintf(stderr, "fatal: --l3-domains '%s' invalid\n", cfg.l3_domains);
                return false;
            }
        } else {
            if (!topo_.discover()) {
                std::fprintf(stderr, "fatal: could not discover any allowed cpu\n");
                return false;
            }
        }
        if (cfg.smt_mode && !topo_.discover_thread_siblings()) {
            std::fprintf(stderr,
                         "fatal: --smt-mode could not read Linux thread_siblings_list topology\n");
            return false;
        }
        bool placed = false;
        if (cfg.thread_mode == ThreadMode::Fused) {
            placed = placement_.build_fused(topo_, cfg.place);
        } else {
            // Default shape: an even io/ex split of every allowed cpu, io taking the odd one out --
            // the measured 2s center (4:4-class) generalized to any core count.
            uint32_t di = cfg.even_ifid, de = cfg.even_ex;
            if (!cfg.place && !(di | de)) {
                uint32_t n = 0;
                for (uint32_t d = 0; d < topo_.ndomains(); d++)
                    n += static_cast<uint32_t>(topo_.cpus_in(d).size());
                di = n - n / 2; de = n / 2;
            }
            placed = cfg.place
                ? placement_.build_explicit(topo_, cfg.place)
                : placement_.build_even(topo_, di, de, cfg.smt_mode != 0);
        }
        if (!placed) return false;
        if (!placement_.configure_smt_units(topo_, cfg.smt_mode != 0)) return false;
        if (!placement_.reserve_runtime_roles(placement_.total_threads())) return false;
        if (placement_.ifid_threads().empty() || placement_.ex_threads().empty()) {
            std::fprintf(stderr, "placement needs at least one ifid and one ex thread\n");
            return false;
        }
        unix_owner_tid_ = cfg.unixsocket && *cfg.unixsocket
            ? placement_.ifid_threads().front() : UINT32_MAX;
        if (!adjust_open_files_limit()) return false;
        check_tcp_backlog_settings();
        // Shard maps are resolved exactly once at boot; parsing never leaks onto a request path.
        if (!placement_.assign_shard_homes(cfg.shards, cfg.shard_home)) return false;

        // ---- shards: bucket ranges, fixed for the life of the process ----------------------------
        shards_.resize(cfg.shards);
        const uint32_t per = kNumBuckets / cfg.shards;
        for (uint32_t i = 0; i < cfg.shards; i++) {
            const uint32_t b0 = i * per;
            const uint32_t b1 = (i + 1 == cfg.shards) ? kNumBuckets : (i + 1) * per;
            shards_[i] = std::make_unique<Shard>();
            shards_[i]->init(this, static_cast<int32_t>(i), b0, b1, cfg.zc_min, cfg.type_limits,
                             cfg.stream_limits);
            if (key_lb_signals_enabled() && !shards_[i]->enable_lb_signals()) {
                std::fprintf(stderr, "fatal: could not allocate weighted-placement signals\n");
                return false;
            }
            shards_[i]->bind_atomic_state(
                [](void* ctx) { return static_cast<Server*>(ctx)->atomic_commit(); }, this,
                &atomic_activity_, &script_intent_owners_);
        }
        router_.build_uniform(static_cast<int32_t>(cfg.shards));

        // ---- threads ---------------------------------------------------------------------------
        // Every front-end has already lowered to dense per-thread entries. Every thread still gets
        // a producer lane from every other, because a role change must not require re-wiring
        // the mesh; the TASK lanes are a role-partitioned masked monolith whose block sizes are
        // re-derived at each role change (ThreadCtx::remask_quiesced), while the client, release
        // and transfer channels stay uniform per-thread arrays.
        const uint32_t nthreads = placement_.total_threads();
        if (!flipctl_.init(cfg.thread_mode == ThreadMode::Split && cfg.flip_auto != 0,
                           cfg.flip_auto_band, nthreads)) {
            std::fprintf(stderr, "fatal: could not allocate flip controller state\n");
            return false;
        }
        for (uint32_t i = 0; i < kMaxThreads; i++) executor_slots_[i] = UINT8_MAX;
        // Role changes may make any physical thread an executor. Stable tid-indexed slots avoid
        // renumbering live atomic-group arrays at each flip.
        for (uint32_t tid = 0; tid < nthreads; tid++)
            executor_slots_[tid] = static_cast<uint8_t>(tid);
        threads_.resize(nthreads);
        live_config_mailboxes_.reset(new (std::nothrow) LiveConfigMailbox[nthreads]);
        if (!live_config_mailboxes_) {
            std::fprintf(stderr, "fatal: could not allocate live configuration mailboxes\n");
            return false;
        }
        live_config_committed_ = capture_live_config(2);
        for (uint32_t i = 0; i < nthreads; i++)
            live_config_mailboxes_[i].init(live_config_committed_);
        for (uint32_t i = 0; i < nthreads; i++) {
            threads_[i] = std::make_unique<ThreadCtx>();
            // The fingerprint writer is armed only when its one reader, the flip controller, is
            // enabled (DESIGN-flipfp.md): with --flip-auto 0 and in 1s mode it is dark and costs
            // one predicted branch per op. flip_work_window keeps its CONFIG value either way.
            threads_[i]->init(i, placement_.role_of(i), nthreads,
                              cfg.flip_auto ? 0 : cfg.lb_age_sample_rate,
                              flipctl_.enabled() ? cfg.flip_work_window : 0);
            threads_[i]->init_command_counts(command_registry_size());
        }
        if (read_local_enabled()) {
            read_local_state_.reset(new (std::nothrow) ReadLocalServerState);
            if (!read_local_state_) {
                std::fprintf(stderr, "fatal: could not allocate read-local server state\n");
                return false;
            }
            for (const auto& thread : threads_) {
                if (!thread->init_read_local_state()) {
                    std::fprintf(stderr, "fatal: could not allocate read-local thread state\n");
                    return false;
                }
            }
            for (const auto& shard : shards_) {
                if (!shard->store().prepare_read_local()) {
                    std::fprintf(stderr, "fatal: could not allocate read-local store state\n");
                    return false;
                }
            }
        }
        if (key_lb_signals_enabled()) {
            try {
                lb_bucket_last_samples_.assign(kNumBuckets, 0);
                lb_bucket_weight_.assign(kNumBuckets, 0.0);
                lb_bucket_last_move_ms_.assign(kNumBuckets, 0);
            } catch (const std::bad_alloc&) {
                std::fprintf(stderr, "fatal: could not allocate weighted key-placement windows\n");
                return false;
            }
        }
        if (lb_controller_enabled()) {
            try {
                lb_thread_last_busy_.assign(nthreads, 0);
                lb_thread_last_idle_.assign(nthreads, 0);
                lb_thread_occupancy_.assign(nthreads, 0.0);
            } catch (const std::bad_alloc&) {
                std::fprintf(stderr, "fatal: could not allocate shared LB windows\n");
                return false;
            }
        }
        for (uint32_t i = 0; i < nthreads; i++)
            atomic_read_floors_[i].store(UINT64_MAX, std::memory_order_relaxed);
        for (uint32_t i = 0; i < nthreads; i++)
            atomic_snapshot_completions_[i].store(0, std::memory_order_relaxed);
        flip_target_io_.store(static_cast<uint32_t>(placement_.ifid_threads().size()),
                              std::memory_order_relaxed);
        flip_target_ex_.store(static_cast<uint32_t>(placement_.ex_threads().size()),
                              std::memory_order_relaxed);

        // ---- shards directly onto workers ------------------------------------------------------
        // Locality resolution stops at the individual EX thread. There is no contiguous node range:
        // the default is one flat round-robin across every executor in thread-id order.
        for (uint32_t sid = 0; sid < cfg.shards; sid++) {
            const uint32_t tid = placement_.shard_home(sid);
            threads_[tid]->shards().push_back(shards_[sid].get());
            // Populate Router's authoritative bucket entries and its derived dispatch fast path.
            set_worker_of_shard(static_cast<int32_t>(sid), tid);
            // Seed residency from this exact thread's cpu domain, so note_execution compares at
            // thread resolution even when adjacent tids live in different L3 domains.
            shards_[sid]->note_migration(placement_.domain_of_thread(tid));
        }
        snapshot_.init(nthreads, cfg.shards,
                       static_cast<uint32_t>(placement_.ex_threads().size()),
                       cfg.dir, cfg.dbfilename, cfg.persist_io);
        aof_.init(*this, cfg, nthreads, cfg.shards,
                  placement_.ifid_threads().back(), aof_replay);
        for (uint32_t sid = 0; sid < cfg.shards; sid++) {
            const uint32_t sequence = aof_replay && aof_replay->next_sequence.size() == cfg.shards
                ? aof_replay->next_sequence[sid] : 0;
            shards_[sid]->store().bind_aof(cfg.appendonly ? &aof_ : nullptr,
                                           static_cast<int32_t>(sid), sequence);
        }
        return true;
    }

    const Config&    cfg()        const { return cfg_; }
    ThreadMode thread_mode() const { return cfg_.thread_mode; }
    const char* thread_mode_name() const {
        return cfg_.thread_mode == ThreadMode::Fused ? "1s" : "2s";
    }

    bool lb_machinery_enabled() const {
        return cfg_.lb_sample_rate && cfg_.lb_tick_ms && cfg_.lb_imbalance_pct &&
               cfg_.lb_move_cap && cfg_.lb_cooldown_ms;
    }
    bool key_lb_signals_enabled() const {
        return cfg_.key_lb && lb_machinery_enabled();
    }
    bool client_lb_signals_enabled() const {
        return cfg_.client_lb && lb_machinery_enabled();
    }
    bool lb_controller_enabled() const {
        return key_lb_signals_enabled() || client_lb_signals_enabled();
    }

    bool flipctl_available() const { return cfg_.thread_mode == ThreadMode::Split; }
    bool flipctl_enabled() const { return flipctl_.enabled(); }
    uint32_t flipctl_tick_ms() const {
        return cfg_.lb_tick_ms ? cfg_.lb_tick_ms : std::max<uint32_t>(1, nthreads());
    }
    bool flipctl_tick(uint64_t now_ms) { return flipctl_.tick(*this, now_ms); }
    uint32_t flipctl_signal_sample_rate() const { return flipctl_.signal_sample_rate(); }
    uint32_t effective_age_sample_rate() const {
        return cfg_.flip_auto ? flipctl_signal_sample_rate() : cfg_.lb_age_sample_rate;
    }
    void flipctl_force_trigger() { flipctl_.request_forced_trigger(); }
    uint32_t flipctl_wait_ms() const { return flipctl_.wait_ms(flipctl_tick_ms()); }
    bool flipctl_debug_seek(uint32_t target_io, bool force, std::string& error) {
        return flipctl_.debug_seek(target_io, force, error);
    }
    void flipctl_debug_cost(double commands_per_client) {
        flipctl_.debug_cost(commands_per_client);
    }
    FlipctlReport flipctl_report() const { return flipctl_.report(); }
    std::string flipctl_debug_dump() const {
        if (flipctl_available()) return flipctl_.debug_dump();
        return "state=unavailable\nphase=fused\navailable=0\n"
               "thread_mode=1s\nreason=threads_are_fused\n"
               "fused_threads=" + std::to_string(nthreads()) +
               "\nclient_threads=" + std::to_string(client_serving_thread_count()) +
               "\nowner_threads=" + std::to_string(shard_owner_count()) + "\n";
    }

    // Client observations are gathered by the connection owner once per second. ROB dispatch
    // deltas are the operation-rate signal; live in-flight depth is a floor so a deep connection
    // that is temporarily backpressured does not look idle. The state is keyed by connection id,
    // hence survives an IO ownership move without turning that move into a signal discontinuity.
    void lb_publish_client_observations(uint32_t owner,
                                        const std::vector<LbClientObservation>& observations) {
        if (!client_lb_signals_enabled() || owner >= nthreads()) return;
        std::lock_guard<std::mutex> lock(lb_signal_mu_);
        double total = 0;
        for (const LbClientObservation& observation : observations) {
            LbClientSignal& signal = lb_clients_[observation.id];
            const uint64_t delta = observation.dispatched_ops - signal.last_ops;
            signal.last_ops = observation.dispatched_ops;
            const double sample = std::max<double>(delta, observation.pipeline_depth);
            signal.weight = signal.owner == UINT32_MAX
                ? sample : 0.25 * sample + 0.75 * signal.weight;
            signal.owner = owner;
            total += signal.weight;
        }
        lb_client_owner_weight_[owner].store(
            static_cast<uint64_t>(total * 1024.0 + 0.5), std::memory_order_release);
    }
    void lb_forget_client(uint64_t id) {
        if (!client_lb_signals_enabled()) return;
        std::lock_guard<std::mutex> lock(lb_signal_mu_);
        lb_clients_.erase(id);
    }
    double lb_client_weight(uint64_t id) const {
        if (!client_lb_signals_enabled()) return 0.0;
        std::lock_guard<std::mutex> lock(lb_signal_mu_);
        const auto found = lb_clients_.find(id);
        return found == lb_clients_.end() ? 0.0 : found->second.weight;
    }

    // The controller owns this fold. Every bucket keeps a monotonic sampled counter on its
    // physical shard; EWMA history is indexed by immutable bucket id, never by executor owner.
    void lb_fold_signals() {
        if (!lb_controller_enabled()) return;
        std::lock_guard<std::mutex> lock(lb_signal_mu_);
        if (key_lb_signals_enabled()) {
            for (uint32_t sid = 0; sid < nshards(); sid++) {
                Shard& physical = shard(static_cast<int32_t>(sid));
                for (uint32_t bucket = physical.bucket_begin();
                     bucket < physical.bucket_end(); bucket++) {
                    const uint32_t current = physical.lb_bucket_samples(bucket);
                    const uint32_t delta = current - lb_bucket_last_samples_[bucket];
                    lb_bucket_last_samples_[bucket] = current;
                    const double sample = static_cast<double>(delta) * cfg_.lb_sample_rate;
                    lb_bucket_weight_[bucket] = lb_bucket_primed_
                        ? 0.25 * sample + 0.75 * lb_bucket_weight_[bucket] : sample;
                }
            }
            lb_bucket_primed_ = true;
        }
        // Occupancy is 1 - measured idle over the same window. cpu_ns deliberately does not enter:
        // polling/spinning is scheduled CPU but does not mean the role has useful work available.
        for (uint32_t tid = 0; tid < nthreads(); tid++) {
            const LoopSignals& signal = thread(tid).sig();
            const uint64_t busy = __atomic_load_n(&signal.busy_ns, __ATOMIC_RELAXED);
            const uint64_t idle = __atomic_load_n(&signal.idle_ns, __ATOMIC_RELAXED);
            const uint64_t db = busy - lb_thread_last_busy_[tid];
            const uint64_t di = idle - lb_thread_last_idle_[tid];
            lb_thread_last_busy_[tid] = busy;
            lb_thread_last_idle_[tid] = idle;
            const double occupancy = db + di
                ? static_cast<double>(db) / static_cast<double>(db + di) : 0.0;
            lb_thread_occupancy_[tid] = lb_occupancy_primed_
                ? 0.25 * occupancy + 0.75 * lb_thread_occupancy_[tid] : occupancy;
        }
        lb_occupancy_primed_ = true;
    }
    double lb_shard_weight(uint32_t sid) const {
        if (sid >= nshards() || !key_lb_signals_enabled()) return 0.0;
        std::lock_guard<std::mutex> lock(lb_signal_mu_);
        const Shard& physical = shard(static_cast<int32_t>(sid));
        double weight = 0;
        for (uint32_t bucket = physical.bucket_begin(); bucket < physical.bucket_end(); bucket++)
            weight += lb_bucket_weight_[bucket];
        return weight;
    }
    uint64_t lb_shard_bytes(uint32_t sid) const {
        if (sid >= nshards() || !key_lb_signals_enabled()) return 0;
        const Shard& physical = shard(static_cast<int32_t>(sid));
        uint64_t bytes = 0;
        for (uint32_t bucket = physical.bucket_begin(); bucket < physical.bucket_end(); bucket++)
            bytes += physical.lb_bucket_bytes(bucket);
        return bytes;
    }
    double lb_thread_occupancy(uint32_t tid) const {
        if (tid >= lb_thread_occupancy_.size()) return 0.0;
        std::lock_guard<std::mutex> lock(lb_signal_mu_);
        return lb_thread_occupancy_[tid];
    }
    const Topology&  topo()       const { return topo_; }
    Placement&       placement()        { return placement_; }
    Router&          router()           { return router_; }
    Shard&           shard(int32_t i)   { return *shards_[i]; }
    const Shard&     shard(int32_t i) const { return *shards_[i]; }
    ThreadCtx&       thread(uint32_t i) { return *threads_[i]; }
    uint32_t         nthreads()   const { return static_cast<uint32_t>(threads_.size()); }
    uint32_t         nshards()    const { return static_cast<uint32_t>(shards_.size()); }
    SnapshotManager& snapshot()         { return snapshot_; }
    const SnapshotManager& snapshot() const { return snapshot_; }
    AofManager& aof() { return aof_; }
    const AofManager& aof() const { return aof_; }
    const ThreadCtx& thread(uint32_t i) const { return *threads_[i]; }

    // Role is the split-mode loop state and deliberately remains Ifid/Ex. Capabilities are the
    // mode-independent truth used by cold control/reporting surfaces: a fused thread serves
    // clients AND owns shards even though its operational loop role is encoded as Ifid.
    bool serves_clients(Role role) const {
        return role != Role::Idle &&
               (cfg_.thread_mode == ThreadMode::Fused || role == Role::Ifid);
    }
    bool owns_shards(Role role) const {
        return role != Role::Idle &&
               (cfg_.thread_mode == ThreadMode::Fused || role == Role::Ex);
    }
    bool serves_clients(uint32_t tid) const {
        return tid < nthreads() && serves_clients(thread(tid).role());
    }
    bool owns_shards(uint32_t tid) const {
        return tid < nthreads() && owns_shards(thread(tid).role());
    }
    uint32_t client_serving_thread_count() const {
        uint32_t count = 0;
        for (uint32_t tid = 0; tid < nthreads(); tid++) count += serves_clients(tid);
        return count;
    }
    // "Owner" is the observable topology term: distinct tids currently named by shard rows. It
    // can be smaller than the number of owner-capable threads when there are fewer shards.
    uint32_t shard_owner_count() const {
        bool seen[kMaxThreads] = {};
        uint32_t count = 0;
        for (uint32_t sid = 0; sid < nshards(); sid++) {
            const uint32_t owner = worker_of_shard(static_cast<int32_t>(sid));
            if (owner < nthreads() && !seen[owner]) {
                seen[owner] = true;
                count++;
            }
        }
        return count;
    }

    static bool read_local_enabled(const Config& cfg) {
        return cfg.thread_mode == ThreadMode::Fused && cfg.overlap == 0 && cfg.read_local != 0;
    }
    bool read_local_enabled() const { return read_local_enabled(cfg_); }
    uint64_t read_local_epoch() const {
        if (!read_local_state_) std::abort();
        return read_local_state_->epoch.load(std::memory_order_seq_cst);
    }
    // Retirement uses the returned OLD value as its stamp. The increment makes a subsequent
    // rotation publication strictly newer, which is the grace test below.
    uint64_t advance_read_local_epoch() {
        if (!read_local_state_) std::abort();
        return read_local_state_->epoch.fetch_add(1, std::memory_order_seq_cst);
    }
    // Grace floor for a QSBR drain, bounded by what the caller needs. `stamp` is the oldest sealed
    // retirement stamp; the drain releases an entry only when the floor is STRICTLY above its stamp.
    // Returns the exact floor (minimum tick over active participants) when that floor is above
    // `stamp`; otherwise returns a value <= `stamp` as soon as one active participant with
    // tick <= `stamp` is found. Both answers produce exactly the decision a full scan would: with a
    // blocking participant the true floor is <= its tick <= stamp, so nothing is releasable and the
    // caller stops at its first batch either way. What changes is the cost of the not-ready case:
    // one or two foreign tick lines instead of one per thread. `hint` remembers the blocking
    // participant so the next pass tests it first; it is caller-owned scratch and never read for
    // anything else. Parked participants contribute infinity because they hold no foreign pointer.
    uint64_t read_local_grace_floor(uint64_t stamp, uint32_t& hint) const {
        const uint32_t participants = static_cast<uint32_t>(threads_.size());
        if (hint < participants) {
            const uint64_t publication = threads_[hint]->read_local_publication();
            if (!ThreadCtx::read_local_publication_parked(publication)) {
                const uint64_t tick = ThreadCtx::read_local_publication_tick(publication);
                if (tick <= stamp) return tick;
            }
        }
        uint64_t floor = UINT64_MAX;
        for (uint32_t i = 0; i < participants; i++) {
            const uint64_t publication = threads_[i]->read_local_publication();
            if (ThreadCtx::read_local_publication_parked(publication)) continue;
            const uint64_t tick = ThreadCtx::read_local_publication_tick(publication);
            if (tick <= stamp) {
                hint = i;
                return tick;
            }
            floor = std::min(floor, tick);
        }
        return floor;
    }

    uint32_t role_count(Role role, bool ready = false) const {
        uint32_t count = 0;
        for (const auto& thread : threads_)
            if ((ready ? thread->ready_role() : thread->role()) == role) count++;
        return count;
    }

    FlipReport flip_report() const {
        FlipReport report;
        report.live_io = role_count(Role::Ifid, true);
        report.live_ex = role_count(Role::Ex, true);
        report.target_io = flip_target_io_.load(std::memory_order_acquire);
        report.target_ex = flip_target_ex_.load(std::memory_order_acquire);
        report.smt_mode = cfg_.smt_mode;
        report.unit_threads = cfg_.smt_mode ? 2u : 1u;
        uint32_t buckets[kMaxThreads] = {};
        for (uint32_t sid = 0; sid < nshards(); sid++) {
            const uint32_t owner = worker_of_shard(static_cast<int32_t>(sid));
            if (owner < nthreads()) buckets[owner]++;
        }
        report.bucket_min = UINT32_MAX;
        report.client_min = UINT32_MAX;
        for (uint32_t tid = 0; tid < nthreads(); tid++) {
            if (thread(tid).ready_role() == Role::Ex) {
                report.bucket_min = std::min(report.bucket_min, buckets[tid]);
                report.bucket_max = std::max(report.bucket_max, buckets[tid]);
            }
            if (thread(tid).ready_role() == Role::Ifid) {
                const uint32_t clients = thread(tid).client_count();
                report.client_min = std::min(report.client_min, clients);
                report.client_max = std::max(report.client_max, clients);
            }
        }
        if (report.bucket_min == UINT32_MAX) report.bucket_min = 0;
        if (report.client_min == UINT32_MAX) report.client_min = 0;
        report.last_transfers = flip_last_transfers_.load(std::memory_order_relaxed);
        report.moving = flip_stage_.load(std::memory_order_acquire) != FlipStage::Idle;
        return report;
    }

    FlipStage flip_stage() const { return flip_stage_.load(std::memory_order_acquire); }
    uint64_t flip_epoch() const { return flip_epoch_.load(std::memory_order_acquire); }
    bool flip_dispatch_paused() const { return flip_stage() != FlipStage::Idle; }
    LbStage lb_stage() const { return lb_stage_.load(std::memory_order_acquire); }
    uint64_t lb_epoch() const { return lb_epoch_.load(std::memory_order_acquire); }
    bool lb_dispatch_paused() const { return lb_stage() == LbStage::ExDrain; }
    bool placement_transition_active() const {
        return flip_dispatch_paused() || lb_stage() != LbStage::Idle;
    }
    bool lb_cron_writer(uint32_t tid) const {
        if (!lb_controller_enabled() || flip_dispatch_paused()) return false;
        for (uint32_t candidate = 0; candidate < nthreads(); candidate++)
            if (thread(candidate).role() == Role::Ifid) return candidate == tid;
        return false;
    }
    uint32_t lb_coordinator() const { return lb_coordinator_; }
    // The io thread that owns the unix listener (UINT32_MAX without --unixsocket). Latched once
    // in init(); both boot paths and the FLIP candidate filter read this one value.
    uint32_t unix_owner_tid() const { return unix_owner_tid_; }
    LbClientMove lb_client_move() const { return lb_client_move_; }
    bool lb_should_pause(uint32_t owner, uint64_t id) const {
        const LbStage stage = lb_stage();
        return stage == LbStage::ExDrain ||
               (stage == LbStage::ClientDrain && lb_client_move_.source == owner &&
                lb_client_move_.id == id);
    }
    uint64_t lb_deadline_ns() const { return lb_deadline_ns_.load(std::memory_order_acquire); }
    void lb_ack(uint32_t tid) {
        lb_ack_[tid].store((lb_epoch() << 8) | static_cast<uint8_t>(lb_stage()),
                           std::memory_order_release);
    }
    bool lb_acked(uint32_t tid) const {
        return lb_ack_[tid].load(std::memory_order_acquire) ==
               ((lb_epoch() << 8) | static_cast<uint8_t>(lb_stage()));
    }
    bool lb_all_ex_acked() const {
        for (uint32_t tid = 0; tid < nthreads(); tid++)
            if (live_executor(tid) && !lb_acked(tid)) return false;
        return true;
    }
    uint64_t lb_ticks() const { return lb_ticks_.load(std::memory_order_relaxed); }
    uint64_t lb_bucket_moves() const {
        return lb_bucket_moves_.load(std::memory_order_relaxed);
    }
    uint64_t lb_client_moves() const {
        return lb_client_moves_.load(std::memory_order_relaxed);
    }
    uint64_t lb_bucket_cross_domain_moves() const {
        return lb_bucket_cross_domain_moves_.load(std::memory_order_relaxed);
    }
    uint64_t lb_client_cross_domain_moves() const {
        return lb_client_cross_domain_moves_.load(std::memory_order_relaxed);
    }
    uint64_t lb_no_candidate() const {
        return lb_no_candidate_.load(std::memory_order_relaxed);
    }
    uint64_t lb_hysteresis_refused() const {
        return lb_hysteresis_refused_.load(std::memory_order_relaxed);
    }
    uint64_t lb_cooldown_refused() const {
        return lb_cooldown_refused_.load(std::memory_order_relaxed);
    }
    uint64_t lb_transition_refused() const {
        return lb_transition_refused_.load(std::memory_order_relaxed);
    }
    uint64_t lb_capacity_refused() const {
        return lb_capacity_refused_.load(std::memory_order_relaxed);
    }
    uint64_t lb_client_refused() const {
        return lb_client_refused_.load(std::memory_order_relaxed);
    }
    uint64_t lb_hot_bucket_refused() const {
        return lb_hot_bucket_refused_.load(std::memory_order_relaxed);
    }
    double lb_bucket_weight_spread_current() const {
        return lb_bucket_weight_spread_current_.load(std::memory_order_relaxed) / 1024.0;
    }
    double lb_bucket_weight_spread_before() const {
        return lb_bucket_weight_spread_before_.load(std::memory_order_relaxed) / 1024.0;
    }
    double lb_bucket_weight_spread_after() const {
        return lb_bucket_weight_spread_after_.load(std::memory_order_relaxed) / 1024.0;
    }
    double lb_client_weight_spread_current() const {
        return lb_client_weight_spread_current_.load(std::memory_order_relaxed) / 1024.0;
    }
    double lb_client_weight_spread_before() const {
        return lb_client_weight_spread_before_.load(std::memory_order_relaxed) / 1024.0;
    }
    double lb_client_weight_spread_after() const {
        return lb_client_weight_spread_after_.load(std::memory_order_relaxed) / 1024.0;
    }
    uint64_t lb_bucket_bytes_spread_current() const {
        return lb_bucket_bytes_spread_current_.load(std::memory_order_relaxed);
    }
    uint64_t lb_bucket_bytes_spread_before() const {
        return lb_bucket_bytes_spread_before_.load(std::memory_order_relaxed);
    }
    uint64_t lb_bucket_bytes_spread_after() const {
        return lb_bucket_bytes_spread_after_.load(std::memory_order_relaxed);
    }
    // Serializes only the two publication edges which begin a snapshot or a FLIP. The long-running
    // operations never hold this mutex: after either publishes Preparing/Planning, the other's
    // atomic state check is sufficient to refuse it.
    std::mutex& shape_transition_mutex() { return shape_transition_mu_; }
    uint32_t flip_coordinator() const { return flip_coordinator_; }
    uint32_t flip_target_io() const { return flip_target_io_.load(std::memory_order_acquire); }
    uint32_t flip_target_ex() const { return flip_target_ex_.load(std::memory_order_acquire); }
    uint64_t flip_completed() const { return flip_completed_.load(std::memory_order_relaxed); }
    uint64_t flip_refused() const { return flip_refused_.load(std::memory_order_relaxed); }
    uint64_t flip_last_transfers() const {
        return flip_last_transfers_.load(std::memory_order_relaxed);
    }
    uint64_t flip_clients_transferred() const {
        return flip_clients_transferred_.load(std::memory_order_relaxed);
    }
    double flip_bucket_weight_spread_before() const {
        return flip_bucket_weight_spread_before_.load(std::memory_order_relaxed) / 1024.0;
    }
    double flip_bucket_weight_spread_after() const {
        return flip_bucket_weight_spread_after_.load(std::memory_order_relaxed) / 1024.0;
    }
    double flip_client_weight_spread_before() const {
        return flip_client_weight_spread_before_.load(std::memory_order_relaxed) / 1024.0;
    }
    double flip_client_weight_spread_after() const {
        return flip_client_weight_spread_after_.load(std::memory_order_relaxed) / 1024.0;
    }
    uint64_t flip_bucket_bytes_spread_before() const {
        return flip_bucket_bytes_spread_before_.load(std::memory_order_relaxed);
    }
    uint64_t flip_bucket_bytes_spread_after() const {
        return flip_bucket_bytes_spread_after_.load(std::memory_order_relaxed);
    }
    void flip_note_client_transferred(uint64_t id, uint32_t destination) {
        flip_clients_transferred_.fetch_add(1, std::memory_order_relaxed);
        if (flip_stage() != FlipStage::Idle)
            flip_active_transfers_.fetch_add(1, std::memory_order_relaxed);
        if (client_lb_signals_enabled()) {
            std::lock_guard<std::mutex> lock(lb_signal_mu_);
            const auto found = lb_clients_.find(id);
            if (found != lb_clients_.end()) found->second.owner = destination;
        }
    }
    void flip_note_refused() { flip_refused_.fetch_add(1, std::memory_order_relaxed); }
    uint64_t flip_conservation_checks() const {
        return flip_conservation_checks_.load(std::memory_order_relaxed);
    }
    uint64_t flip_conservation_violations() const {
        return flip_conservation_violations_.load(std::memory_order_relaxed);
    }
    void set_loading(bool loading) { loading_.store(loading ? 1u : 0u, std::memory_order_release); }
    bool loading_begin() {
        // Runtime DEBUG loads and placement transitions use the same short admission edge as
        // snapshots. This makes "refuse while loading" exact even when two IO owners parse the
        // commands concurrently.
        std::lock_guard<std::mutex> transition_lock(shape_transition_mu_);
        if (placement_transition_active()) return false;
        loading_.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }
    void loading_end() {
        if (loading_.fetch_sub(1, std::memory_order_acq_rel) == 0) std::abort();
    }
    bool loading() const { return loading_.load(std::memory_order_acquire) != 0; }

    bool flip_is_candidate(uint32_t tid) const {
        return tid < nthreads() && flip_convert_[tid] != Role::Idle;
    }
    Role flip_candidate_target(uint32_t tid) const {
        return tid < nthreads() ? flip_convert_[tid] : Role::Idle;
    }
    Role flip_final_role(uint32_t tid) const {
        if (tid >= nthreads()) return Role::Idle;
        return flip_convert_[tid] == Role::Idle ? thread(tid).role() : flip_convert_[tid];
    }
    uint32_t flip_incoming_clients(uint32_t tid) const {
        return tid < nthreads()
            ? flip_incoming_clients_[tid].load(std::memory_order_acquire) : 0;
    }
    uint32_t flip_client_destination(uint32_t source, uint32_t ordinal) const {
        if (source >= nthreads() || ordinal >= flip_client_destinations_[source].size())
            return UINT32_MAX;
        return flip_client_destinations_[source][ordinal];
    }
    Client* flip_client_at(uint32_t source, uint32_t ordinal) const {
        if (source >= nthreads() || ordinal >= flip_client_plan_[source].size()) return nullptr;
        return flip_client_plan_[source][ordinal];
    }
    uint32_t flip_client_quota(uint32_t tid) const {
        return tid < nthreads() ? flip_client_quota_[tid] : 0;
    }
    uint32_t flip_shard_destination(uint32_t sid) const {
        return sid < nshards() ? flip_shard_destination_[sid] : UINT32_MAX;
    }
    void flip_note_bucket_transferred() {
        if (flip_stage() == FlipStage::Idle) std::abort();
        flip_active_transfers_.fetch_add(1, std::memory_order_relaxed);
    }
    bool flip_build_client_plan(Client* coordinator_client, std::string& error) {
        if (!flip_surviving_io_count_) {
            error = "ERR FLIP would leave no connection owner";
            return false;
        }

        uint64_t total64 = 0;
        for (uint32_t tid = 0; tid < nthreads(); tid++) {
            if (thread(tid).role() != Role::Ifid) continue;
            total64 += thread(tid).client_count();
        }
        if (total64 > UINT32_MAX || total64 != live_clients()) {
            error = "ERR FLIP connection ownership count is not conserved";
            return false;
        }
        const uint32_t total = static_cast<uint32_t>(total64);
        try {
            for (uint32_t tid = 0; tid < nthreads(); tid++) {
                flip_client_destinations_[tid].clear();
                flip_client_plan_[tid].clear();
                flip_client_quota_[tid] = 0;
                flip_incoming_clients_[tid].store(0, std::memory_order_relaxed);
                flip_source_clients_[tid].store(0, std::memory_order_relaxed);
            }
            std::vector<uint32_t> targets(flip_surviving_io_,
                                          flip_surviving_io_ + flip_surviving_io_count_);
            std::vector<WeightedLbItem> items;
            std::vector<Client*> clients;
            items.reserve(total);
            clients.reserve(total);
            const bool weighted_client = client_lb_signals_enabled();
            bool coordinator_seen = coordinator_client == nullptr;
            for (uint32_t owner = 0; owner < nthreads(); owner++) {
                if (thread(owner).role() != Role::Ifid) continue;
                for (Client* client : thread(owner).clients()) {
                    const bool pinned = coordinator_client && client == coordinator_client;
                    coordinator_seen |= pinned;
                    items.push_back({client->id(), owner,
                                     weighted_client ? lb_client_weight(client->id()) : 0.0,
                                     pinned});
                    clients.push_back(client);
                }
            }
            if (items.size() != total || !coordinator_seen) {
                error = "ERR FLIP coordinator connection left the ownership set";
                return false;
            }
            std::vector<WeightedLbAssignment> assignments;
            if (!weighted_lb_partition(items, targets, assignments) ||
                assignments.size() != items.size()) {
                error = "ERR FLIP cannot satisfy weighted client and coordinator constraints";
                return false;
            }

            uint32_t lanes[kMaxThreads][kMaxThreads] = {};
            double before[kMaxThreads] = {};
            double after[kMaxThreads] = {};
            uint32_t planned = 0;
            for (uint32_t i = 0; i < assignments.size(); i++) {
                const WeightedLbAssignment& assignment = assignments[i];
                if (assignment.id != clients[i]->id() || assignment.source >= nthreads() ||
                    assignment.destination >= nthreads()) std::abort();
                before[assignment.source] += assignment.weight;
                after[assignment.destination] += assignment.weight;
                flip_client_quota_[assignment.destination]++;
                if (assignment.source == assignment.destination) continue;
                flip_client_plan_[assignment.source].push_back(clients[i]);
                flip_client_destinations_[assignment.source].push_back(assignment.destination);
                lanes[assignment.source][assignment.destination]++;
                flip_incoming_clients_[assignment.destination].fetch_add(
                    1, std::memory_order_relaxed);
                planned++;
            }
            const uint32_t low = total / flip_surviving_io_count_;
            const uint32_t high = low + (total % flip_surviving_io_count_ != 0);
            double before_min = 0, before_max = 0, after_min = 0, after_max = 0;
            bool before_first = true, after_first = true;
            for (uint32_t tid = 0; tid < nthreads(); tid++) {
                if (thread(tid).role() == Role::Ifid) {
                    if (before_first) before_min = before_max = before[tid];
                    else { before_min = std::min(before_min, before[tid]);
                           before_max = std::max(before_max, before[tid]); }
                    before_first = false;
                }
            }
            for (uint32_t target : targets) {
                if (flip_client_quota_[target] < low || flip_client_quota_[target] > high) {
                    error = "ERR FLIP weighted client plan violates count balance";
                    return false;
                }
                if (after_first) after_min = after_max = after[target];
                else { after_min = std::min(after_min, after[target]);
                       after_max = std::max(after_max, after[target]); }
                after_first = false;
            }
            flip_client_weight_spread_before_.store(
                static_cast<uint64_t>((before_max - before_min) * 1024.0 + 0.5),
                std::memory_order_relaxed);
            flip_client_weight_spread_after_.store(
                static_cast<uint64_t>((after_max - after_min) * 1024.0 + 0.5),
                std::memory_order_relaxed);
            for (uint32_t source = 0; source < nthreads(); source++) {
                if (flip_client_plan_[source].size() !=
                    flip_client_destinations_[source].size()) std::abort();
                for (uint32_t destination : targets) {
                    if (lanes[source][destination] >
                        thread(destination).client_transfer_free_slots(source)) {
                        error = "ERR FLIP connection-transfer inbox capacity is insufficient";
                        return false;
                    }
                }
                flip_source_clients_[source].store(
                    static_cast<uint32_t>(flip_client_plan_[source].size()),
                    std::memory_order_relaxed);
            }
            flip_planned_client_transfers_ = planned;
            return true;
        } catch (const std::bad_alloc&) {
            error = "ERR FLIP could not allocate client balance plan";
            return false;
        }
    }
    uint32_t flip_source_clients(uint32_t source) const {
        return source < nthreads()
            ? flip_source_clients_[source].load(std::memory_order_acquire) : 0;
    }
    bool flip_reserve_shard_plan(std::string& error) {

        if (!flip_shard_ownership_conserved()) {
            error = "ERR FLIP shard ownership is not a complete one-owner partition";
            return false;
        }
        try {
            std::vector<uint32_t> executors;
            for (uint32_t tid = 0; tid < nthreads(); tid++)
                if (flip_final_role(tid) == Role::Ex) executors.push_back(tid);
            if (executors.empty()) {
                error = "ERR FLIP would leave no bucket owner";
                return false;
            }
            std::vector<WeightedLbItem> items;
            items.reserve(nshards());
            const bool weighted_key = key_lb_signals_enabled();
            for (uint32_t sid = 0; sid < nshards(); sid++) {
                const uint32_t owner = worker_of_shard(static_cast<int32_t>(sid));
                if (owner >= nthreads()) {
                    error = "ERR FLIP shard ownership names an invalid thread";
                    return false;
                }
                items.push_back({sid, owner,
                                 weighted_key ? lb_shard_weight(sid) : 0.0, false,
                                 weighted_key ? static_cast<double>(lb_shard_bytes(sid)) : 0.0});
            }
            std::sort(executors.begin(), executors.end());
            std::vector<WeightedLbAssignment> assignments;
            if (!weighted_lb_partition(items, executors, assignments) ||
                assignments.size() != items.size()) {
                error = "ERR FLIP cannot satisfy weighted bucket and count constraints";
                return false;
            }

            uint32_t incoming[kMaxThreads] = {};
            double before_weight[kMaxThreads] = {};
            double after_weight[kMaxThreads] = {};
            double before_bytes[kMaxThreads] = {};
            double after_bytes[kMaxThreads] = {};
            for (uint32_t sid = 0; sid < nshards(); sid++)
                flip_shard_destination_[sid] = UINT32_MAX;
            uint32_t moves = 0;
            for (const WeightedLbAssignment& assignment : assignments) {
                if (assignment.id >= nshards() || assignment.destination >= nthreads())
                    std::abort();
                const uint32_t sid = static_cast<uint32_t>(assignment.id);
                flip_shard_destination_[sid] = assignment.destination;
                flip_bucket_quota_[assignment.destination]++;
                before_weight[assignment.source] += assignment.weight;
                after_weight[assignment.destination] += assignment.weight;
                before_bytes[assignment.source] += assignment.secondary;
                after_bytes[assignment.destination] += assignment.secondary;
                if (assignment.source != assignment.destination) {
                    incoming[assignment.destination]++;
                    moves++;
                }
            }
            const uint32_t low = nshards() / static_cast<uint32_t>(executors.size());
            const uint32_t high = low + (nshards() % executors.size() != 0);
            double bw_min = 0, bw_max = 0, aw_min = 0, aw_max = 0;
            double bb_min = 0, bb_max = 0, ab_min = 0, ab_max = 0;
            bool before_first = true, after_first = true;
            for (uint32_t tid = 0; tid < nthreads(); tid++) {
                if (thread(tid).role() != Role::Ex) continue;
                if (before_first) {
                    bw_min = bw_max = before_weight[tid];
                    bb_min = bb_max = before_bytes[tid];
                } else {
                    bw_min = std::min(bw_min, before_weight[tid]);
                    bw_max = std::max(bw_max, before_weight[tid]);
                    bb_min = std::min(bb_min, before_bytes[tid]);
                    bb_max = std::max(bb_max, before_bytes[tid]);
                }
                before_first = false;
            }
            for (uint32_t tid : executors) {
                if (flip_bucket_quota_[tid] < low || flip_bucket_quota_[tid] > high) {
                    error = "ERR FLIP weighted bucket plan violates count balance";
                    return false;
                }
                if (after_first) {
                    aw_min = aw_max = after_weight[tid];
                    ab_min = ab_max = after_bytes[tid];
                } else {
                    aw_min = std::min(aw_min, after_weight[tid]);
                    aw_max = std::max(aw_max, after_weight[tid]);
                    ab_min = std::min(ab_min, after_bytes[tid]);
                    ab_max = std::max(ab_max, after_bytes[tid]);
                }
                after_first = false;
                if (!reserve_shard_capacity(tid, incoming[tid])) {
                    error = "ERR FLIP could not reserve executor shard ownership";
                    return false;
                }
            }
            flip_bucket_weight_spread_before_.store(
                static_cast<uint64_t>((bw_max - bw_min) * 1024.0 + 0.5),
                std::memory_order_relaxed);
            flip_bucket_weight_spread_after_.store(
                static_cast<uint64_t>((aw_max - aw_min) * 1024.0 + 0.5),
                std::memory_order_relaxed);
            flip_bucket_bytes_spread_before_.store(
                static_cast<uint64_t>(bb_max - bb_min + 0.5), std::memory_order_relaxed);
            flip_bucket_bytes_spread_after_.store(
                static_cast<uint64_t>(ab_max - ab_min + 0.5), std::memory_order_relaxed);
            flip_planned_shard_transfers_ = moves;
            return true;
        } catch (const std::bad_alloc&) {
            error = "ERR FLIP could not allocate bucket balance plan";
            return false;
        }
    }

    // One cron-owned controller beat. It computes candidates from the same immutable-id signal
    // windows consumed by FLIP, then publishes either a short EX quiescence transaction or one
    // connection drain request. Nothing here runs on an operation path.
    bool lb_controller_tick(uint32_t coordinator, uint64_t now_ms) {
        if (!lb_controller_enabled() || coordinator >= nthreads()) return false;
        const bool key_enabled = key_lb_signals_enabled();
        const bool client_enabled = client_lb_signals_enabled();
        lb_ticks_.fetch_add(1, std::memory_order_relaxed);
        try {
            {
                std::lock_guard<std::mutex> transition_lock(shape_transition_mu_);
                if (lb_stage() != LbStage::Idle || flip_dispatch_paused() ||
                    snapshot_.in_progress() || loading()) {
                    lb_transition_refused_.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                // FLIP folds the same windows while holding this admission mutex. Serialising the
                // fold prevents a losing concurrent planner from consuming and decaying its tick.
                lb_fold_signals();
            }
            std::vector<uint32_t> executors;
            std::vector<uint32_t> ios;
            if (cfg_.thread_mode == ThreadMode::Fused) {
                if (key_enabled) executors = placement_.ex_threads();
                if (client_enabled) ios = placement_.ifid_threads();
            } else {
                for (uint32_t tid = 0; tid < nthreads(); tid++) {
                    const Role role = thread(tid).role();
                    if (key_enabled && role == Role::Ex) executors.push_back(tid);
                    else if (client_enabled && role == Role::Ifid) ios.push_back(tid);
                }
            }

            auto spread = [](const double* loads, const std::vector<uint32_t>& owners) {
                if (owners.empty()) return 0.0;
                double lo = loads[owners.front()], hi = lo;
                for (uint32_t tid : owners) {
                    lo = std::min(lo, loads[tid]);
                    hi = std::max(hi, loads[tid]);
                }
                return hi - lo;
            };
            auto ratio_pct = [&](double span, const double* loads,
                                 const std::vector<uint32_t>& owners) {
                double total = 0;
                for (uint32_t tid : owners) total += loads[tid];
                return total > 0 && !owners.empty()
                    ? span * 100.0 * owners.size() / total : 0.0;
            };
            auto update_streak = [&](double ratio, uint32_t& streak) {
                const double fire = cfg_.lb_imbalance_pct;
                const double release = fire * 0.8; // Schmitt release band
                if (ratio > fire) streak = std::min<uint32_t>(streak + 1, 3);
                else if (ratio < release) streak = 0;
                if (streak < 3) {
                    lb_hysteresis_refused_.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                return true;
            };

            std::vector<LbShardMove> shard_plan;
            double shard_before = 0, shard_after = 0;
            double bytes_before = 0, bytes_after = 0;
            if (key_enabled && executors.size() >= 2) {
                double loads[kMaxThreads] = {};
                double byte_loads[kMaxThreads] = {};
                std::vector<WeightedLbItem> shard_items;
                shard_items.reserve(nshards());
                std::vector<uint64_t> last_move;
                bool dominant_bucket = false;
                {
                    std::lock_guard<std::mutex> lock(lb_signal_mu_);
                    last_move = lb_bucket_last_move_ms_;
                    double total = 0, hottest = 0;
                    for (double weight : lb_bucket_weight_) {
                        total += weight;
                        hottest = std::max(hottest, weight);
                    }
                    dominant_bucket = total > 0 && hottest * 2.0 > total;
                }
                uint32_t cooldown_seen = 0;
                for (uint32_t sid = 0; sid < nshards(); sid++) {
                    const uint32_t owner = worker_of_shard(static_cast<int32_t>(sid));
                    const double weight = lb_shard_weight(sid);
                    const uint64_t bytes = lb_shard_bytes(sid);
                    const Shard& physical = shard(static_cast<int32_t>(sid));
                    bool cooling = false;
                    for (uint32_t bucket = physical.bucket_begin();
                         bucket < physical.bucket_end(); bucket++) {
                        const uint64_t moved = last_move[bucket];
                        if (moved && now_ms - moved < cfg_.lb_cooldown_ms) {
                            cooling = true;
                            break;
                        }
                    }
                    if (cooling) cooldown_seen++;
                    shard_items.push_back(
                        {sid, owner, weight, cooling, static_cast<double>(bytes)});
                    loads[owner] += weight;
                    byte_loads[owner] += static_cast<double>(bytes);
                }
                shard_before = spread(loads, executors);
                bytes_before = spread(byte_loads, executors);
                const double weight_ratio = ratio_pct(shard_before, loads, executors);
                const double byte_ratio = ratio_pct(bytes_before, byte_loads, executors);
                lb_bucket_weight_spread_current_.store(
                    static_cast<uint64_t>(shard_before * 1024.0 + 0.5),
                    std::memory_order_relaxed);
                lb_bucket_bytes_spread_current_.store(
                    static_cast<uint64_t>(bytes_before + 0.5), std::memory_order_relaxed);
                if (dominant_bucket && weight_ratio > cfg_.lb_imbalance_pct) {
                    // A bucket carrying at least half of all observed demand cannot be decomposed
                    // by the single-owner actuator. It may contain a hot key; record and stop
                    // instead of merely relocating the bottleneck.
                    lb_hot_bucket_refused_.fetch_add(1, std::memory_order_relaxed);
                    lb_no_candidate_.fetch_add(1, std::memory_order_relaxed);
                    lb_bucket_hot_streak_ = 0;
                } else if (update_streak(
                               std::max(weight_ratio, byte_ratio), lb_bucket_hot_streak_)) {
                    for (uint32_t step = 0; step < cfg_.lb_move_cap; step++) {
                        const double old_weight_span = spread(loads, executors);
                        const double old_byte_span = spread(byte_loads, executors);
                        const bool demand_hot = ratio_pct(old_weight_span, loads, executors) >
                                                cfg_.lb_imbalance_pct;
                        const bool memory_hot = ratio_pct(old_byte_span, byte_loads, executors) >
                                                cfg_.lb_imbalance_pct;
                        if (!demand_hot && !memory_hot) break;
                        WeightedLbMoveChoice choice;
                        if (!weighted_lb_best_incremental_move(
                                shard_items, executors, demand_hot, memory_hot, choice)) {
                            if (cooldown_seen)
                                lb_cooldown_refused_.fetch_add(1, std::memory_order_relaxed);
                            else
                                lb_no_candidate_.fetch_add(1, std::memory_order_relaxed);
                            break;
                        }
                        WeightedLbItem& item = shard_items[choice.item_index];
                        shard_plan.push_back({static_cast<uint32_t>(item.id), choice.source,
                                             choice.destination, item.weight,
                                             static_cast<uint64_t>(item.secondary)});
                        loads[choice.source] -= item.weight;
                        loads[choice.destination] += item.weight;
                        byte_loads[choice.source] -= item.secondary;
                        byte_loads[choice.destination] += item.secondary;
                        item.owner = choice.destination;
                        item.pinned = true;
                    }
                    shard_after = spread(loads, executors);
                    bytes_after = spread(byte_loads, executors);
                }
            } else if (key_enabled) {
                lb_no_candidate_.fetch_add(1, std::memory_order_relaxed);
                lb_bucket_hot_streak_ = 0;
            }

            LbClientMove client_plan;
            double client_before = 0, client_after = 0;
            if (client_enabled && ios.size() >= 2) {
                double loads[kMaxThreads] = {};
                std::vector<WeightedLbItem> clients;
                uint32_t cooldown_seen = 0;
                {
                    std::lock_guard<std::mutex> lock(lb_signal_mu_);
                    clients.reserve(lb_clients_.size());
                    for (const auto& entry : lb_clients_) {
                        const LbClientSignal& signal = entry.second;
                        if (signal.owner >= nthreads() ||
                            thread(signal.owner).role() != Role::Ifid) continue;
                        const bool cooling = signal.last_move_ms &&
                            now_ms - signal.last_move_ms < cfg_.lb_cooldown_ms;
                        if (cooling) cooldown_seen++;
                        clients.push_back(
                            {entry.first, signal.owner, signal.weight, cooling, 0.0});
                        loads[signal.owner] += signal.weight;
                    }
                }
                client_before = spread(loads, ios);
                lb_client_weight_spread_current_.store(
                    static_cast<uint64_t>(client_before * 1024.0 + 0.5),
                    std::memory_order_relaxed);
                const double client_ratio = ratio_pct(client_before, loads, ios);
                if (update_streak(client_ratio, lb_client_hot_streak_)) {
                    WeightedLbMoveChoice choice;
                    if (!weighted_lb_best_incremental_move(
                            clients, ios, true, false, choice)) {
                        if (cooldown_seen)
                            lb_cooldown_refused_.fetch_add(1, std::memory_order_relaxed);
                        else
                            lb_no_candidate_.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        const WeightedLbItem& client = clients[choice.item_index];
                        client_plan = {client.id, choice.source, choice.destination, client.weight};
                        client_after = choice.after_weight_spread;
                    }
                }
            } else if (client_enabled) {
                lb_no_candidate_.fetch_add(1, std::memory_order_relaxed);
                lb_client_hot_streak_ = 0;
            }

            const bool have_bucket = !shard_plan.empty();
            const bool have_client = client_plan.id != 0;
            if (!have_bucket && !have_client) {
                lb_no_candidate_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            const bool choose_client = have_client && (!have_bucket || lb_prefer_client_);

            std::lock_guard<std::mutex> transition_lock(shape_transition_mu_);
            if (lb_stage() != LbStage::Idle || flip_dispatch_paused() ||
                snapshot_.in_progress() || loading()) {
                lb_transition_refused_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            for (uint32_t tid = 0; tid < nthreads(); tid++)
                lb_ack_[tid].store(0, std::memory_order_relaxed);
            lb_coordinator_ = coordinator;
            lb_epoch_.fetch_add(1, std::memory_order_acq_rel);
            lb_deadline_ns_.store(now_ns() + 5ull * 1000 * 1000 * 1000,
                                  std::memory_order_release);
            if (choose_client) {
                lb_client_move_ = client_plan;
                lb_client_weight_spread_before_.store(
                    static_cast<uint64_t>(client_before * 1024.0 + 0.5),
                    std::memory_order_relaxed);
                lb_client_weight_spread_after_.store(
                    static_cast<uint64_t>(client_after * 1024.0 + 0.5),
                    std::memory_order_relaxed);
                lb_client_hot_streak_ = 0; // consume sustain before touching a candidate
                lb_stage_.store(LbStage::ClientDrain, std::memory_order_release);
            } else {
                lb_shard_moves_ = std::move(shard_plan);
                lb_bucket_weight_spread_before_.store(
                    static_cast<uint64_t>(shard_before * 1024.0 + 0.5),
                    std::memory_order_relaxed);
                lb_bucket_weight_spread_after_.store(
                    static_cast<uint64_t>(shard_after * 1024.0 + 0.5),
                    std::memory_order_relaxed);
                lb_bucket_bytes_spread_before_.store(
                    static_cast<uint64_t>(bytes_before + 0.5), std::memory_order_relaxed);
                lb_bucket_bytes_spread_after_.store(
                    static_cast<uint64_t>(bytes_after + 0.5), std::memory_order_relaxed);
                lb_bucket_hot_streak_ = 0;
                lb_stage_.store(LbStage::ExDrain, std::memory_order_release);
            }
            lb_prefer_client_ = !choose_client;
            return true;
        } catch (const std::bad_alloc&) {
            lb_capacity_refused_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    bool lb_commit_shard_plan(uint64_t now_ms) {
        if (lb_stage() != LbStage::ExDrain || !lb_all_ex_acked()) return false;
        uint32_t incoming[kMaxThreads] = {};
        for (const LbShardMove& move : lb_shard_moves_) incoming[move.destination]++;
        for (uint32_t tid = 0; tid < nthreads(); tid++) {
            if (!incoming[tid]) continue;
            if (!reserve_shard_capacity(tid, incoming[tid])) {
                lb_capacity_refused_.fetch_add(1, std::memory_order_relaxed);
                lb_stage_.store(LbStage::Idle, std::memory_order_release);
                lb_deadline_ns_.store(0, std::memory_order_release);
                return false;
            }
        }
        for (const LbShardMove& move : lb_shard_moves_) {
            if (!transfer_shard_quiesced(static_cast<int32_t>(move.sid), move.source,
                                          move.destination)) std::abort();
            Shard& physical = shard(static_cast<int32_t>(move.sid));
            {
                std::lock_guard<std::mutex> lock(lb_signal_mu_);
                for (uint32_t bucket = physical.bucket_begin();
                     bucket < physical.bucket_end(); bucket++)
                    lb_bucket_last_move_ms_[bucket] = now_ms;
            }
            lb_bucket_moves_.fetch_add(1, std::memory_order_relaxed);
            if (placement_.domain_of_thread(move.source) !=
                placement_.domain_of_thread(move.destination))
                lb_bucket_cross_domain_moves_.fetch_add(1, std::memory_order_relaxed);
        }
        lb_stage_.store(LbStage::Idle, std::memory_order_release);
        lb_deadline_ns_.store(0, std::memory_order_release);
        return true;
    }

    bool lb_client_move_started(uint64_t id, uint64_t now_ms) {
        if (lb_client_move_.id != id) return false;
        LbStage expected = LbStage::ClientDrain;
        if (!lb_stage_.compare_exchange_strong(expected, LbStage::ClientMoving,
                                               std::memory_order_acq_rel)) return false;
        {
            std::lock_guard<std::mutex> lock(lb_signal_mu_);
            const auto found = lb_clients_.find(id);
            if (found != lb_clients_.end()) found->second.last_move_ms = now_ms;
        }
        lb_client_inflight_id_.store(id, std::memory_order_release);
        return true;
    }
    void lb_refuse_client_request() {
        if (lb_stage() != LbStage::ClientDrain) return;
        lb_client_refused_.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(lb_signal_mu_);
        const auto found = lb_clients_.find(lb_client_move_.id);
        if (found != lb_clients_.end())
            found->second.last_move_ms = now_ns() / 1000000ull;
        lb_stage_.store(LbStage::Idle, std::memory_order_release);
        lb_deadline_ns_.store(0, std::memory_order_release);
    }
    void lb_client_move_committed(uint64_t id, uint32_t source, uint32_t destination) {
        uint64_t expected = id;
        if (!lb_client_inflight_id_.compare_exchange_strong(
                expected, 0, std::memory_order_acq_rel)) return;
        lb_client_moves_.fetch_add(1, std::memory_order_relaxed);
        if (placement_.domain_of_thread(source) != placement_.domain_of_thread(destination))
            lb_client_cross_domain_moves_.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(lb_signal_mu_);
        const auto found = lb_clients_.find(id);
        if (found != lb_clients_.end()) found->second.owner = destination;
        lb_stage_.store(LbStage::Idle, std::memory_order_release);
        lb_deadline_ns_.store(0, std::memory_order_release);
    }
    void lb_client_move_cancelled(uint64_t id) {
        uint64_t expected = id;
        if (lb_client_inflight_id_.compare_exchange_strong(
                expected, 0, std::memory_order_acq_rel)) {
            lb_client_refused_.fetch_add(1, std::memory_order_relaxed);
            lb_stage_.store(LbStage::Idle, std::memory_order_release);
            lb_deadline_ns_.store(0, std::memory_order_release);
        }
    }
    bool lb_timed_out() const {
        const uint64_t deadline = lb_deadline_ns();
        return deadline && now_ns() >= deadline;
    }
    void lb_stage_timed_out() {
        const LbStage stage = lb_stage();
        if (stage == LbStage::Idle || stage == LbStage::ClientMoving) return;
        if (stage == LbStage::ClientDrain) {
            lb_client_refused_.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(lb_signal_mu_);
            const auto found = lb_clients_.find(lb_client_move_.id);
            if (found != lb_clients_.end())
                found->second.last_move_ms = now_ns() / 1000000ull;
        }
        else
            lb_transition_refused_.fetch_add(1, std::memory_order_relaxed);
        lb_stage_.store(LbStage::Idle, std::memory_order_release);
        lb_deadline_ns_.store(0, std::memory_order_release);
    }
    bool flip_timed_out() const {
        return flip_deadline_ns_.load(std::memory_order_acquire) != 0 &&
               now_ns() >= flip_deadline_ns_.load(std::memory_order_acquire);
    }
    bool flip_begin(uint32_t target_io, uint32_t target_ex, uint32_t coordinator,
                    std::string& error) {
        if (cfg_.thread_mode == ThreadMode::Fused) {
            error = "ERR FLIP is unavailable with --thread-mode 1s: threads are fused";
            return false;
        }
        std::lock_guard<std::mutex> transition_lock(shape_transition_mu_);
        const LbStage live_lb_stage = lb_stage();
        if (live_lb_stage == LbStage::ExDrain || live_lb_stage == LbStage::ClientDrain) {
            // An explicit shape change wins over an uncommitted cron candidate. No ownership edge
            // exists in either stage, so withdrawing it is exact. A ClientMoving request has
            // already crossed its reversible preflight; FLIP admits it and IoDrain waits for that
            // existing transfer to settle before it counts or moves any connection.
            lb_transition_refused_.fetch_add(1, std::memory_order_relaxed);
            lb_stage_.store(LbStage::Idle, std::memory_order_release);
            lb_deadline_ns_.store(0, std::memory_order_release);
        }
        FlipStage expected = FlipStage::Idle;
        if (!flip_stage_.compare_exchange_strong(expected, FlipStage::Planning,
                                                 std::memory_order_acq_rel)) {
            error = "ERR a FLIP is already in progress";
            flip_refused_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        // Publish intent before validation. A refused request remains observable as live != target
        // through the report form; erasing it was the fork's most damaging control-plane blind spot.
        flip_target_io_.store(target_io, std::memory_order_release);
        flip_target_ex_.store(target_ex, std::memory_order_release);
        auto refuse = [&](const std::string& message) {
            error = message;
            flip_refused_.fetch_add(1, std::memory_order_relaxed);
            flip_stage_.store(FlipStage::Idle, std::memory_order_release);
            return false;
        };
        if (!target_io || !target_ex)
            return refuse("ERR FLIP requires at least one io and one ex thread");
        if (static_cast<uint64_t>(target_io) + static_cast<uint64_t>(target_ex) != nthreads())
            return refuse("ERR FLIP io + ex must equal the existing total thread count");
        if (cfg_.smt_mode && ((target_io & 1u) || (target_ex & 1u)))
            return refuse(flip_smt_pairing_error(target_io));
        if (coordinator >= nthreads() || thread(coordinator).role() != Role::Ifid)
            return refuse("ERR FLIP coordinator is not a live io thread");
        if (loading()) return refuse("ERR FLIP is not allowed while loading");
        if (snapshot_.in_progress()) return refuse("ERR FLIP is not allowed during a snapshot");
        if (role_count(Role::Ifid) + role_count(Role::Ex) != nthreads())
            return refuse("ERR FLIP thread conservation is already violated");
        flip_conservation_check();
        if (lb_controller_enabled()) lb_fold_signals();

        const uint32_t live_io = role_count(Role::Ifid);
        for (uint32_t tid = 0; tid < kMaxThreads; tid++) {
            flip_convert_[tid] = Role::Idle;
            flip_client_plan_[tid].clear();
            flip_client_destinations_[tid].clear();
            flip_client_quota_[tid] = 0;
            flip_bucket_quota_[tid] = 0;
            flip_incoming_clients_[tid].store(0, std::memory_order_relaxed);
            flip_source_clients_[tid].store(0, std::memory_order_relaxed);
            flip_ack_[tid].store(0, std::memory_order_relaxed);
        }
        for (uint32_t sid = 0; sid < 256; sid++) flip_shard_destination_[sid] = UINT32_MAX;
        flip_planned_client_transfers_ = 0;
        flip_planned_shard_transfers_ = 0;
        flip_active_transfers_.store(0, std::memory_order_relaxed);
        const uint32_t conversions = live_io > target_io ? live_io - target_io
                                                          : target_io - live_io;
        if (conversions == 0) {
            // A same-split request is a completed no-op, not a rebalance trigger: weight-driven
            // movement belongs to the cron mover with its hysteresis and cooldown. Entering the
            // stage machine with an empty plan is not an option either -- every stage predicate
            // (client quotas, RoleReady acks, the completion balance audit) judges distributions
            // a no-op deliberately leaves alone. Complete here; the caller replies OK on seeing
            // Idle. (The reshuffle this replaces moved a hundred buckets on a balanced no-op.)
            flip_last_transfers_.store(0, std::memory_order_relaxed);
            flip_completed_.fetch_add(1, std::memory_order_relaxed);
            flip_target_io_.store(target_io, std::memory_order_release);
            flip_target_ex_.store(target_ex, std::memory_order_release);
            flip_stage_.store(FlipStage::Idle, std::memory_order_release);
            return true;
        }
        struct RoleUnit {
            uint32_t first = UINT32_MAX;
            uint32_t second = UINT32_MAX;
            double occupancy = 0;
        };
        RoleUnit units[kMaxThreads];
        uint32_t unit_count = 0;
        uint32_t selected = 0;
        if (target_io < live_io) {
            // LOAD-BEARING exclusions, not tie-breakers: the AOF writer (and, in smt mode, its
            // sibling) must never be converted io->ex. writer_shutdown() runs only on the io loop's
            // exit paths; an executor-state ~AofManager closes without an fsync and discard_chunks()
            // drops buffered records. The unix-listener owner likewise cannot leave the io role
            // without taking its listener with it.
            const uint32_t aof_writer = aof_.writer_tid();
            for (uint32_t tid : placement_.ifid_threads()) {
                if (!cfg_.smt_mode) {
                    if (tid == coordinator || tid == aof_writer || tid == unix_owner_tid_) continue;
                    units[unit_count++] = {tid, UINT32_MAX, lb_thread_occupancy(tid)};
                    continue;
                }
                const uint32_t peer = placement_.smt_peer(tid);
                if (peer >= nthreads() || tid < peer) continue;
                if (tid == coordinator || peer == coordinator ||
                    tid == aof_writer || peer == aof_writer ||
                    tid == unix_owner_tid_ || peer == unix_owner_tid_) continue;
                units[unit_count++] = {
                    tid, peer, (lb_thread_occupancy(tid) + lb_thread_occupancy(peer)) * 0.5};
            }
        } else {
            for (uint32_t tid : placement_.ex_threads()) {
                if (!cfg_.smt_mode) {
                    units[unit_count++] = {tid, UINT32_MAX, lb_thread_occupancy(tid)};
                    continue;
                }
                const uint32_t peer = placement_.smt_peer(tid);
                if (peer >= nthreads() || tid < peer) continue;
                units[unit_count++] = {
                    tid, peer, (lb_thread_occupancy(tid) + lb_thread_occupancy(peer)) * 0.5};
            }
        }
        std::sort(units, units + unit_count, [](const RoleUnit& a, const RoleUnit& b) {
            if (a.occupancy != b.occupancy) return a.occupancy < b.occupancy;
            return a.first > b.first; // disabled/all-zero signal preserves the old reverse-id tie
        });
        // Domain-even selection. Boot placement spreads both roles evenly across L3 domains
        // (build_even); a picker that ignores topology un-evens it -- occupancy ties broke by
        // reverse id, so with cold signals every conversion clustered into the highest domain and
        // one L3 went role-lopsided after a single flip. Conversions round-robin across domains
        // (always the domain with the fewest taken so far, most remaining candidates on ties) and
        // occupancy keeps deciding WITHIN a domain via the sort above.
        const Role target_role = target_io < live_io ? Role::Ex : Role::Ifid;
        uint32_t unit_domain[kMaxThreads];
        uint32_t domain_taken[kMaxThreads] = {};
        uint32_t domain_left[kMaxThreads] = {};
        bool     unit_used[kMaxThreads] = {};
        uint32_t max_domain = 0;
        for (uint32_t i = 0; i < unit_count; i++) {
            unit_domain[i] = placement_.domain_of_thread(units[i].first);
            if (unit_domain[i] >= kMaxThreads) unit_domain[i] = 0;
            domain_left[unit_domain[i]]++;
            max_domain = std::max(max_domain, unit_domain[i]);
        }
        while (selected < conversions) {
            uint32_t pick_domain = UINT32_MAX;
            for (uint32_t d = 0; d <= max_domain; d++) {
                if (!domain_left[d]) continue;
                if (pick_domain == UINT32_MAX ||
                    domain_taken[d] < domain_taken[pick_domain] ||
                    (domain_taken[d] == domain_taken[pick_domain] &&
                     domain_left[d] > domain_left[pick_domain]))
                    pick_domain = d;
            }
            if (pick_domain == UINT32_MAX) break;  // no candidates anywhere: caught below
            for (uint32_t i = 0; i < unit_count; i++) {
                if (unit_used[i] || unit_domain[i] != pick_domain) continue;
                unit_used[i] = true;
                domain_taken[pick_domain]++;
                domain_left[pick_domain]--;
                flip_convert_[units[i].first] = target_role;
                selected++;
                if (units[i].second != UINT32_MAX) {
                    flip_convert_[units[i].second] = target_role;
                    selected++;
                }
                break;
            }
        }
        if (selected != conversions)
            return refuse("ERR FLIP cannot select enough movable threads (coordinator/AOF/UNIX owner pinned)");
        if (!flip_candidate_pairs_conserved())
            return refuse("ERR FLIP candidate plan would split an SMT sibling pair");

        flip_surviving_io_count_ = 0;
        for (uint32_t tid = 0; tid < nthreads(); tid++)
            if (flip_final_role(tid) == Role::Ifid)
                flip_surviving_io_[flip_surviving_io_count_++] = tid;
        if (!flip_surviving_io_count_)
            return refuse("ERR FLIP would leave no connection owner");

        flip_coordinator_ = coordinator;
        flip_target_io_.store(target_io, std::memory_order_release);
        flip_target_ex_.store(target_ex, std::memory_order_release);
        flip_failed_.store(false, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(flip_error_mu_);
            flip_error_.clear();
        }
        flip_epoch_.fetch_add(1, std::memory_order_acq_rel);
        flip_deadline_ns_.store(now_ns() + 5ull * 1000 * 1000 * 1000,
                                std::memory_order_release);
        flip_stage_.store(FlipStage::IoDrain, std::memory_order_release);
        return true;
    }

    void flip_set_stage(FlipStage stage) {
        if (stage == FlipStage::Idle) std::abort();
        flip_stage_.store(stage, std::memory_order_release);
    }
    void flip_ack(uint32_t tid, FlipStage stage) {
        const uint64_t token = (flip_epoch() << 8) | static_cast<uint8_t>(stage);
        flip_ack_[tid].store(token, std::memory_order_release);
    }
    bool flip_acked(uint32_t tid, FlipStage stage) const {
        const uint64_t token = (flip_epoch() << 8) | static_cast<uint8_t>(stage);
        return flip_ack_[tid].load(std::memory_order_acquire) == token;
    }
    bool flip_all_role_acked(Role role, FlipStage stage) const {
        for (uint32_t tid = 0; tid < nthreads(); tid++)
            if (thread(tid).role() == role && !flip_acked(tid, stage)) return false;
        return true;
    }
    void flip_note_failure(const std::string& error) {
        bool expected = false;
        if (!flip_failed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            return;
        std::lock_guard<std::mutex> lock(flip_error_mu_);
        flip_error_ = error;
    }
    bool flip_failed(std::string& error) const {
        if (!flip_failed_.load(std::memory_order_acquire)) return false;
        std::lock_guard<std::mutex> lock(flip_error_mu_);
        error = flip_error_;
        return true;
    }
    void flip_conservation_check(bool ready = false) {
        flip_conservation_checks_.fetch_add(1, std::memory_order_relaxed);
        if (role_count(Role::Ifid, ready) + role_count(Role::Ex, ready) == nthreads() &&
            flip_live_pairs_conserved(ready)) return;
        flip_conservation_violations_.fetch_add(1, std::memory_order_relaxed);
        std::abort();
    }
    void flip_change_role(uint32_t tid, Role role) {
        flip_conservation_check();
        auto change = [&](uint32_t id) {
            placement_.set_runtime_role(id, role);
            thread(id).set_role(role);
        };
        if (!cfg_.smt_mode) {
            change(tid);
        } else {
            const uint32_t peer = placement_.smt_peer(tid);
            if (peer >= nthreads() || flip_candidate_target(peer) != role) std::abort();
            change(tid);
            change(peer);
        }
        flip_conservation_check();
    }
    void flip_refuse_active() {
        flip_refused_.fetch_add(1, std::memory_order_relaxed);
        // Retain the requested target. The no-argument report then exposes live != target after a
        // refusal instead of erasing the only witness that the requested shape was not reached.
        flip_stage_.store(FlipStage::Idle, std::memory_order_release);
        flip_deadline_ns_.store(0, std::memory_order_release);
    }
    void flip_complete_active() {
        flip_conservation_check();
        flip_conservation_check(true);
        if (!flip_shard_ownership_conserved()) std::abort();
        if (role_count(Role::Ifid, true) != flip_target_io() ||
            role_count(Role::Ex, true) != flip_target_ex()) {
            flip_conservation_violations_.fetch_add(1, std::memory_order_relaxed);
            std::abort();
        }
        uint32_t buckets[kMaxThreads] = {};
        for (uint32_t sid = 0; sid < nshards(); sid++) {
            const uint32_t owner = worker_of_shard(static_cast<int32_t>(sid));
            if (owner >= nthreads()) std::abort();
            buckets[owner]++;
        }
        uint32_t bucket_min = UINT32_MAX, bucket_max = 0;
        uint32_t client_min = UINT32_MAX, client_max = 0;
        for (uint32_t tid = 0; tid < nthreads(); tid++) {
            if (thread(tid).role() == Role::Ex) {
                if (buckets[tid] != flip_bucket_quota_[tid]) std::abort();
                bucket_min = std::min(bucket_min, buckets[tid]);
                bucket_max = std::max(bucket_max, buckets[tid]);
            } else if (thread(tid).role() == Role::Ifid) {
                const uint32_t clients = thread(tid).client_count();
                if (clients != flip_client_quota_[tid]) std::abort();
                client_min = std::min(client_min, clients);
                client_max = std::max(client_max, clients);
            }
        }
        if (bucket_min == UINT32_MAX || client_min == UINT32_MAX ||
            bucket_max - bucket_min > 1 || client_max - client_min > 1)
            std::abort();
        const uint64_t transfers = flip_active_transfers_.load(std::memory_order_relaxed);
        if (transfers != static_cast<uint64_t>(flip_planned_shard_transfers_) +
                         flip_planned_client_transfers_)
            std::abort();

        flip_last_transfers_.store(transfers, std::memory_order_relaxed);
        flip_completed_.fetch_add(1, std::memory_order_relaxed);
        flip_stage_.store(FlipStage::Idle, std::memory_order_release);
        flip_deadline_ns_.store(0, std::memory_order_release);
    }

private:
    std::string flip_smt_pairing_error(uint32_t requested_io) const {
        std::string nearest;
        auto add = [&](uint32_t io) {
            if (io < 2 || io + 2 > nthreads()) return;
            if (!nearest.empty()) nearest += " and ";
            nearest += std::to_string(io) + ":" + std::to_string(nthreads() - io);
        };
        add(requested_io & ~1u);
        add((requested_io & ~1u) + 2);
        return "ERR FLIP SMT sibling pairs require even io/ex counts; nearest achievable splits are " +
               nearest;
    }

    bool flip_live_pairs_conserved(bool ready) const {
        if (!cfg_.smt_mode) return true;
        for (uint32_t tid = 0; tid < nthreads(); tid++) {
            const uint32_t peer = placement_.smt_peer(tid);
            if (peer >= nthreads()) return false;
            const Role role = ready ? thread(tid).ready_role() : thread(tid).role();
            const Role peer_role = ready ? thread(peer).ready_role() : thread(peer).role();
            if (role != peer_role) return false;
        }
        return true;
    }

    bool flip_candidate_pairs_conserved() const {
        if (!cfg_.smt_mode) return true;
        for (uint32_t tid = 0; tid < nthreads(); tid++) {
            const uint32_t peer = placement_.smt_peer(tid);
            if (peer >= nthreads()) return false;
            const Role role = flip_convert_[tid] == Role::Idle
                ? thread(tid).role() : flip_convert_[tid];
            const Role peer_role = flip_convert_[peer] == Role::Idle
                ? thread(peer).role() : flip_convert_[peer];
            if (role != peer_role) return false;
        }
        return true;
    }

    bool flip_shard_ownership_conserved() const {
        if (nshards() > 256 || router_.transfer_phase() != Router::TransferPhase::Idle)
            return false;
        uint8_t seen[256] = {};
        for (uint32_t tid = 0; tid < nthreads(); tid++) {
            const auto& owned = thread(tid).shards();
            if (thread(tid).role() != Role::Ex && !owned.empty()) return false;
            for (Shard* shard : owned) {
                if (!shard || shard->id() < 0 ||
                    static_cast<uint32_t>(shard->id()) >= nshards() ||
                    shard != shards_[static_cast<uint32_t>(shard->id())].get() ||
                    seen[static_cast<uint32_t>(shard->id())]++ ||
                    worker_of_shard(shard->id()) != tid) return false;
                for (uint32_t bucket = shard->bucket_begin(); bucket < shard->bucket_end(); bucket++)
                    if (router_.shard_of_bucket(bucket) != shard->id() ||
                        router_.owner_of_bucket(bucket) != tid) return false;
            }
        }
        for (uint32_t sid = 0; sid < nshards(); sid++) if (seen[sid] != 1) return false;
        return true;
    }

public:

    bool save_schedule_armed() const {
        return live_save_armed_.load(std::memory_order_relaxed);
    }
    bool save_cron_writer(uint32_t tid) const {
        return save_schedule_armed() && !placement_.ifid_threads().empty() &&
               placement_.ifid_threads().front() == tid;
    }
    uint64_t save_change_total() const {
        uint64_t total = 0;
        for (const auto& shard : shards_) total += shard->save_changes();
        return total;
    }
    uint64_t save_changes_since_last_save() const {
        const uint64_t total = save_change_total();
        const uint64_t baseline = save_change_baseline_.load(std::memory_order_relaxed);
        return total >= baseline ? total - baseline : 0;
    }
    void snapshot_save_succeeded(uint64_t change_cut) {
        save_change_baseline_.store(change_cut, std::memory_order_relaxed);
    }
    uint64_t scheduled_save_triggers() const {
        return scheduled_save_triggers_.load(std::memory_order_relaxed);
    }
    uint64_t save_cron_checks() const {
        return save_cron_checks_.load(std::memory_order_relaxed);
    }
    void set_save_schedule(const std::vector<SaveClause>& clauses) {
        const bool was_armed = save_schedule_armed();
        {
            std::lock_guard<std::mutex> lock(save_mu_);
            save_clauses_ = clauses;
        }
        if (!was_armed && !clauses.empty())
            save_change_baseline_.store(save_change_total(), std::memory_order_relaxed);
        const uint64_t version = begin_live_config_update();
        live_save_armed_.store(!clauses.empty(), std::memory_order_relaxed);
        end_live_config_update(version);
    }
    void set_proto_max_bulk_len(uint64_t value) {
        const uint64_t version = begin_live_config_update();
        live_proto_max_bulk_len_.store(value, std::memory_order_relaxed);
        end_live_config_update(version);
    }
    uint32_t save_cron_pass(ThreadCtx& writer, Ring& ring) {
        if (!save_cron_writer(writer.id()) || snapshot_.in_progress()) return 0;
        save_cron_checks_.fetch_add(1, std::memory_order_relaxed);
        const uint64_t changes = save_changes_since_last_save();
        const std::time_t now_time = std::time(nullptr);
        const uint64_t now_s = now_time > 0 ? static_cast<uint64_t>(now_time) : 0;
        const int64_t last_signed = snapshot_.last_save_time();
        const uint64_t last_s = last_signed > 0 ? static_cast<uint64_t>(last_signed) : 0;
        bool eligible = false;
        {
            std::lock_guard<std::mutex> lock(save_mu_);
            for (const SaveClause& clause : save_clauses_) {
                if (changes >= clause.changes && now_s >= last_s &&
                    now_s - last_s > clause.seconds) {
                    eligible = true;
                    break;
                }
            }
        }
        if (!eligible) return 0;
        std::string error;
        const SnapshotManager::StartResult result =
            snapshot_.start(*this, writer, ring, false, error);
        if (result != SnapshotManager::StartResult::Started) return 0;
        scheduled_save_triggers_.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }

    // Exactly one indexed acquire load on dispatch.  The normal optimized build has no bounds
    // branch; an unoptimized assertion build retains the diagnostic check.
    uint32_t worker_of_shard(int32_t shard_id) const {
#if !defined(NDEBUG) && !defined(__OPTIMIZE__)
        if (shard_id < 0 || static_cast<uint32_t>(shard_id) >= shards_.size()) std::abort();
#endif
        return shard_owner_[shard_id].load(std::memory_order_acquire);
    }
    void set_worker_of_shard(int32_t shard_id, uint32_t thread_id) {
        if (shard_id < 0 || static_cast<uint32_t>(shard_id) >= shards_.size()) std::abort();
        Shard& sh = *shards_[static_cast<uint32_t>(shard_id)];
        router_.set_initial_owner(sh.bucket_begin(), sh.bucket_end(), thread_id);
        shard_owner_[shard_id].store(thread_id, std::memory_order_release);
    }

    // The caller must hold both executor loops at the safe point described in NOTES-MIGRATE.md:
    // no executing/retry/group/snapshot work may touch this shard.  Queue entries which arrive via
    // a stale pre-commit route are harmless because ExLoop rechecks and forwards before access.
    // Vector capacity and membership are settled while PREPARING still names the source.  The
    // phase store in commit_transfer() is the sole ownership edge; bucket entries and the derived
    // shard array publish destination only after that edge.
    // THE OWNERSHIP EDGE OWNS THE SINK.
    //
    // A shard's read-local retire sink names TWO structures that belong to one thread and are
    // protected by nothing else: the owner's QSBR retire ring (`defer`, a single-producer ring) and
    // the owner's private block cache (`block_cache`, an unlocked free list). Every armed write on
    // the shard reaches both through the shard's store, so the sink must name the thread that is
    // executing that shard's writes -- at every instant, not eventually.
    //
    // Moving the shard without moving the sink leaves the DESTINATION executing writes through the
    // SOURCE's ring and free list. Two threads then splice one unlocked list: the loser's update is
    // lost, a block ends up linked twice, and the damage surfaces later as either
    // KvBlockCache::put's `heads[cls] == memory` abort or a take() of a still-linked block whose
    // KvObj header overwrites the list `next`, so that the NEXT take dereferences a wild pointer.
    // Both were observed (see DESIGN-P0REPLY.md).
    //
    // The rebind used to be deferred to the destination's own next executor pass
    // (`lb_rebind_pending_` -> `read_local_rebind_owned_shards_after_lb`). Nothing ordered that
    // pass before the destination's first write to the shard it had just been given, and the
    // reproduction shows exactly that gap: transfer sid 1 to thread 1, then thread 1 executing
    // cmd_set on it against thread 7's cache, with no rebind in between.
    //
    // Here there is no gap. Both callers hold every executor at the quiesced safe point (the LB
    // commits only once every EX thread has acked ExDrain, which `flip_quiesced()` grants only with
    // an EMPTY retire ring), so the source has nothing outstanding for this shard and the
    // destination has not started. The sink moves with ownership, in the same critical section.
    void adopt_read_local_retire_sink(Shard& shard, uint32_t destination) {
#ifdef TOMO_RL_CACHE_NO_EAGER_ADOPT
        // NEGATIVE CONTROL (Makefile target `rlcache-nofix`): restores the pre-fix behaviour, where
        // the sink was left for the destination's own later pass to rebind. Every invariant added
        // for this defect MUST fail against this build; a detector that cannot report failure
        // proves nothing about the runs that pass.
        (void)shard; (void)destination;
        return;
#else
        if (!shard.store().read_local_enabled()) return;
        const ReadLocalRetireSink* sink = threads_[destination]->read_local_retire_sink_or_null();
        if (!sink) std::abort();
        shard.store().rebind_read_local_retire_sink(*sink);
#endif
    }

#ifdef TOMO_RL_CACHE_DEBUG
    // THE INVARIANT THAT WOULD HAVE CAUGHT THIS, stated where it can be checked cheaply and
    // continuously rather than only where it is violated.
    //
    // Every shard's read-local retire sink must name its CURRENT owner, at every instant. Checking
    // it only at the point of USE (KvBlockCache's owner assertion) makes detection depend on the
    // new owner happening to write to the moved shard inside the window, which is a coin flip per
    // move. Checking the mapping itself makes ANY move with a stale sink fire, deterministically,
    // whether or not a write lands in the window -- which is what lets a gate row rest on it.
    //
    // 16-256 pointer compares, debug builds only, once per executor pass.
    void debug_assert_read_local_sinks_follow_ownership(uint32_t checker) const {
        // Only outside a migration stage. The transfer functions publish the sink and the new owner
        // as two separate stores inside one quiesced critical section, so a thread parked at the
        // ExDrain ack can observe the instant between them; that transient is not the defect. The
        // defect outlives the stage -- the pre-fix rebind did not happen until the destination's
        // own later pass -- so checking at Idle still catches it on the very next pass.
        if (lb_stage() != LbStage::Idle || flip_stage() != FlipStage::Idle) return;
        for (uint32_t sid = 0; sid < nshards(); sid++) {
            const Shard& shard = *shards_[sid];
            if (!shard.store().read_local_enabled()) continue;
            const uint32_t owner = shard_owner_[sid].load(std::memory_order_acquire);
            if (owner >= threads_.size()) continue;
            const ReadLocalRetireSink* want = threads_[owner]->read_local_retire_sink_or_null();
            if (!want) continue;
            const ReadLocalRetireSink& have = shard.store().read_local_retire_sink_debug();
            if (have.block_cache == want->block_cache && have.context == want->context) continue;
            std::fprintf(stderr,
                "\nRLSINK-VIOLATION shard %u owner thread %u expects cache %p/queue %p, store %p "
                "still names cache %p/queue %p (observed by thread %u)\n",
                sid, owner, static_cast<void*>(want->block_cache), want->context,
                static_cast<const void*>(&shard.store()),
                static_cast<void*>(have.block_cache), have.context, checker);
            std::fflush(stderr);
            std::abort();
        }
    }
#endif

    bool transfer_bucket_range_quiesced(uint32_t begin, uint32_t end, uint32_t source,
                                         uint32_t destination) {
        if (begin >= end || end > kNumBuckets || source >= threads_.size() ||
            destination >= threads_.size() || source == destination) return false;
        if (!live_executor(source) || !live_executor(destination))
            return false;

        // FlatStore is the lock-free physical ownership unit. Accept a range of one or more whole
        // stores, never a partial store which would give two threads access to the same table.
        std::vector<Shard*> moving;
        try {
            moving.reserve(shards_.size());
        } catch (...) {
            return false;
        }
        uint32_t cursor = begin;
        for (const auto& owned : shards_) {
            Shard* const shard = owned.get();
            if (shard->bucket_end() <= begin || shard->bucket_begin() >= end) continue;
            if (shard->bucket_begin() != cursor || shard->bucket_end() > end ||
                worker_of_shard(shard->id()) != source) return false;
            moving.push_back(shard);
            cursor = shard->bucket_end();
        }
        if (moving.empty() || cursor != end) return false;

        auto& from = threads_[source]->shards();
        auto& to = threads_[destination]->shards();
        for (Shard* shard : moving) {
            if (std::find(from.begin(), from.end(), shard) == from.end() ||
                std::find(to.begin(), to.end(), shard) != to.end()) return false;
        }

        // Allocation is the sole ordinary failure after validation, so force it before publishing
        // PREPARING. No recoverable operation remains after begin_transfer succeeds.
        try {
            to.reserve(to.size() + moving.size());
        } catch (...) {
            return false;
        }
        if (!router_.begin_transfer(begin, end, source, destination))
            return false;

        for (Shard* shard : moving) to.push_back(shard);
        from.erase(std::remove_if(from.begin(), from.end(), [&](Shard* shard) {
            return std::find(moving.begin(), moving.end(), shard) != moving.end();
        }), from.end());
        for (Shard* shard : moving) adopt_read_local_retire_sink(*shard, destination);
        router_.commit_transfer();
        for (Shard* shard : moving)
            shard_owner_[shard->id()].store(destination, std::memory_order_release);
        router_.finish_transfer();
        for (Shard* shard : moving)
            shard->note_migration(placement_.domain_of_thread(destination));
        return true;
    }

    bool transfer_shard_quiesced(int32_t shard_id, uint32_t source, uint32_t destination) {
        if (shard_id < 0 || static_cast<uint32_t>(shard_id) >= shards_.size()) return false;
        Shard& shard = *shards_[static_cast<uint32_t>(shard_id)];
        if (source >= threads_.size() || destination >= threads_.size() || source == destination ||
            !live_executor(source) ||
            (!live_executor(destination) &&
             !(flip_stage() != FlipStage::Idle && flip_final_role(destination) == Role::Ex)) ||
            worker_of_shard(shard_id) != source) return false;
        auto& from = threads_[source]->shards();
        auto& to = threads_[destination]->shards();
        auto found = std::find(from.begin(), from.end(), &shard);
        if (found == from.end() || std::find(to.begin(), to.end(), &shard) != to.end() ||
            to.size() == to.capacity()) return false;
        if (!router_.begin_transfer(shard.bucket_begin(), shard.bucket_end(), source, destination))
            return false;
        to.push_back(&shard);                       // capacity was reserved before PREPARING
        *found = from.back();
        from.pop_back();
        adopt_read_local_retire_sink(shard, destination);
        router_.commit_transfer();                 // THE single bucket ownership edge
        shard_owner_[shard_id].store(destination, std::memory_order_release);
        router_.finish_transfer();
        shard.note_migration(placement_.domain_of_thread(destination));
        return true;
    }
    bool reserve_shard_capacity(uint32_t tid, uint32_t incoming) {
        if (tid >= nthreads()) return false;
        auto& owned = thread(tid).shards();
        try {
            owned.reserve(owned.size() + incoming);
            return true;
        } catch (...) {
            return false;
        }
    }

    bool live_executor(uint32_t tid) const {
        if (tid >= nthreads()) return false;
        return cfg_.thread_mode == ThreadMode::Fused
            ? placement_.is_executor(tid) : thread(tid).role() == Role::Ex;
    }
    uint32_t executor_slot(uint32_t thread_id) const {
        return thread_id < kMaxThreads ? executor_slots_[thread_id] : UINT8_MAX;
    }

    std::atomic<uint64_t>& next_client_id() { return next_client_id_; }
    std::atomic<bool>&     shutting_down()  { return shutting_down_; }

    uint32_t maxclients() const { return live_maxclients_.load(std::memory_order_relaxed); }
    void set_maxclients(uint32_t value) {
        live_maxclients_.store(value, std::memory_order_relaxed);
    }
    uint64_t live_clients() const { return live_clients_.load(std::memory_order_relaxed); }
    void client_accepted() { live_clients_.fetch_add(1, std::memory_order_relaxed); }
    void client_released() {
        if (live_clients_.fetch_sub(1, std::memory_order_relaxed) == 0) std::abort();
    }
    void note_rejected_conn() { rejected_conns_.fetch_add(1, std::memory_order_relaxed); }
    uint64_t rejected_conns() const { return rejected_conns_.load(std::memory_order_relaxed); }

    uint32_t tcp_keepalive() const {
        return live_tcp_keepalive_.load(std::memory_order_relaxed);
    }
    void set_tcp_keepalive(uint32_t value) {
        live_tcp_keepalive_.store(value, std::memory_order_relaxed);
    }
    uint32_t timeout() const { return live_timeout_.load(std::memory_order_relaxed); }
    bool client_cron_armed() const {
        return client_cron_armed_.load(std::memory_order_relaxed);
    }
    const std::atomic<bool>* client_obuf_armed_ptr() const {
        return &client_obuf_armed_;
    }
    bool client_obuf_armed() const {
        return client_obuf_armed_.load(std::memory_order_relaxed);
    }
    void set_timeout(uint32_t value) {
        const uint64_t version = begin_live_config_update();
        live_timeout_.store(value, std::memory_order_relaxed);
        refresh_client_cron_armed();
        end_live_config_update(version);
    }

    ClientLimitsConfigSnapshot client_limits_snapshot(uint32_t reader_tid) const {
        return live_config_mailboxes_[reader_tid].read(live_config_version_).clients;
    }
    void set_client_output_buffer_limits(const ClientOutputBufferLimits& limits) {
        const uint64_t version = begin_live_config_update();
        store_client_output_buffer_limits(limits);
        refresh_client_cron_armed();
        end_live_config_update(version);
    }
    // ---- Lane F: the single CLIENT/MONITOR/TRACKING armed word ---------------------------------
    // Every feature this lane adds is OFF by default and must cost nothing while off.  Instead of
    // giving each one its own hot-path test, they share ONE word that the IO loop caches per
    // batch and folds into the notification-armed decision parse_and_dispatch already makes.  A
    // zero word means the io side takes byte-for-byte the pre-lane path.
    static constexpr uint32_t kClimonMonitor  = 1u << 0;  // >=1 MONITOR client exists
    static constexpr uint32_t kClimonTracking = 1u << 1;  // >=1 CLIENT TRACKING client exists
    static constexpr uint32_t kClimonPause    = 1u << 2;  // a CLIENT PAUSE deadline is live
    static constexpr uint32_t kClimonReply    = 1u << 3;  // >=1 CLIENT REPLY OFF/SKIP client

    uint32_t climon_armed() const { return climon_armed_.load(std::memory_order_relaxed); }

    void climon_monitor_added() {
        climon_monitors_.fetch_add(1, std::memory_order_relaxed);
        refresh_climon_armed();
    }
    void climon_monitor_removed() {
        if (climon_monitors_.fetch_sub(1, std::memory_order_relaxed) == 0) std::abort();
        refresh_climon_armed();
    }
    uint64_t climon_monitors() const { return climon_monitors_.load(std::memory_order_relaxed); }

    // Tracking arms shard-side write observation, so it publishes through the same mailboxes
    // executors already poll (one version compare per pass when nothing changed).
    void climon_tracking_added() {
        const uint64_t version = begin_live_config_update();
        climon_tracking_.fetch_add(1, std::memory_order_relaxed);
        refresh_climon_armed();
        end_live_config_update(version);
    }
    void climon_tracking_removed() {
        const uint64_t version = begin_live_config_update();
        if (climon_tracking_.fetch_sub(1, std::memory_order_relaxed) == 0) std::abort();
        refresh_climon_armed();
        end_live_config_update(version);
    }
    uint64_t climon_tracking_clients() const {
        return climon_tracking_.load(std::memory_order_relaxed);
    }

    void climon_reply_added() {
        climon_reply_.fetch_add(1, std::memory_order_relaxed);
        refresh_climon_armed();
    }
    void climon_reply_removed() {
        if (climon_reply_.fetch_sub(1, std::memory_order_relaxed) == 0) std::abort();
        refresh_climon_armed();
    }

    // CLIENT PAUSE: one global deadline, checked per batch by every io owner.
    static constexpr uint8_t kPauseAll = 0;
    static constexpr uint8_t kPauseWrite = 1;
    void climon_set_pause(uint64_t end_ms, uint8_t mode) {
        climon_pause_mode_.store(mode, std::memory_order_relaxed);
        climon_pause_end_ms_.store(end_ms, std::memory_order_relaxed);
        refresh_climon_armed();
    }
    void climon_clear_pause() {
        climon_pause_end_ms_.store(0, std::memory_order_relaxed);
        refresh_climon_armed();
    }
    uint64_t climon_pause_end_ms() const {
        return climon_pause_end_ms_.load(std::memory_order_relaxed);
    }
    uint8_t climon_pause_mode() const {
        return climon_pause_mode_.load(std::memory_order_relaxed);
    }
    // Called by an io owner that observed the deadline pass; disarming is idempotent and racy-safe
    // because every consumer also compares the deadline against its own clock.
    void climon_expire_pause(uint64_t now_ms) {
        uint64_t end = climon_pause_end_ms_.load(std::memory_order_relaxed);
        if (!end || now_ms < end) return;
        if (climon_pause_end_ms_.compare_exchange_strong(end, 0, std::memory_order_relaxed))
            refresh_climon_armed();
    }

    // Which io threads own at least one MONITOR client, and which own at least one tracking
    // client.  Feeds and invalidations post ONLY to the owners in the mask, so a single monitor
    // does not cost one cross-thread message per io thread per command.
    uint64_t climon_monitor_io_mask() const {
        return climon_monitor_io_mask_.load(std::memory_order_relaxed);
    }
    void climon_set_monitor_io(uint32_t io, bool present) {
        const uint64_t bit = 1ull << (io & 63);
        if (present) climon_monitor_io_mask_.fetch_or(bit, std::memory_order_relaxed);
        else climon_monitor_io_mask_.fetch_and(~bit, std::memory_order_relaxed);
    }
    uint64_t climon_tracking_io_mask() const {
        return climon_tracking_io_mask_.load(std::memory_order_relaxed);
    }
    void climon_set_tracking_io(uint32_t io, bool present) {
        const uint64_t bit = 1ull << (io & 63);
        if (present) climon_tracking_io_mask_.fetch_or(bit, std::memory_order_relaxed);
        else climon_tracking_io_mask_.fetch_and(~bit, std::memory_order_relaxed);
    }

    void climon_note_monitor_line() {
        climon_monitor_lines_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t climon_monitor_lines() const {
        return climon_monitor_lines_.load(std::memory_order_relaxed);
    }
    void climon_note_invalidation(uint64_t n = 1) {
        climon_invalidations_.fetch_add(n, std::memory_order_relaxed);
    }
    uint64_t climon_invalidations() const {
        return climon_invalidations_.load(std::memory_order_relaxed);
    }
    void tracking_forwarded_stale_added(uint64_t n = 1) {
        tracking_forwarded_stale_.fetch_add(n, std::memory_order_relaxed);
    }
    uint64_t tracking_forwarded_stale() const {
        return tracking_forwarded_stale_.load(std::memory_order_relaxed);
    }
    void monitor_forwarded_stale_added(uint64_t n = 1) {
        monitor_forwarded_stale_.fetch_add(n, std::memory_order_relaxed);
    }
    uint64_t monitor_forwarded_stale() const {
        return monitor_forwarded_stale_.load(std::memory_order_relaxed);
    }
    // PROOF-OF-MECHANISM for the out-of-band frame channel, and the reason it exists is the
    // vacuous-validation trap: the geometry that used to splice a push into a borrowed reply is
    // "an out-of-band frame arrives while ops are still in flight", and a battery that never
    // reaches that state passes without testing anything. These two counters name the two output
    // routes, so a check can assert the gate OPENED before it believes its own clean result.
    //
    //   segmented -- frame took the segment channel immediately. The important busy-ROB case is a
    //                genuinely parked blocking head, which must not hold a delivery for its whole
    //                timeout; an already-existing segment queue can also select this route.
    //   deferred  -- frame parked behind the replies of commands issued before it, including both
    //                Done-but-unretired replies and the partial staging window inside a drain.
    void note_oob_frame_segmented() {
        oob_frames_segmented_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t oob_frames_segmented() const {
        return oob_frames_segmented_.load(std::memory_order_relaxed);
    }
    void note_oob_frame_deferred() {
        oob_frames_deferred_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t oob_frames_deferred() const {
        return oob_frames_deferred_.load(std::memory_order_relaxed);
    }
    // Proof-of-mechanism counter for CLIENT NO-TOUCH: operations that reached an executor with
    // the suppression bit set while maxmemory was enabled. Incremented only inside the
    // maxmemory-enabled arm, so it costs nothing in the default configuration.
    void climon_note_no_touch() {
        climon_no_touch_ops_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t climon_no_touch_ops() const {
        return climon_no_touch_ops_.load(std::memory_order_relaxed);
    }
    void climon_note_pause_hold() {
        climon_pause_holds_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t climon_pause_holds() const {
        return climon_pause_holds_.load(std::memory_order_relaxed);
    }
    void climon_note_tracking_key_delta(int64_t delta) {
        climon_tracking_keys_.fetch_add(static_cast<uint64_t>(delta), std::memory_order_relaxed);
    }
    uint64_t climon_tracking_keys() const {
        return climon_tracking_keys_.load(std::memory_order_relaxed);
    }
    void climon_note_tracking_item_delta(int64_t delta) {
        climon_tracking_items_.fetch_add(static_cast<uint64_t>(delta), std::memory_order_relaxed);
    }
    uint64_t climon_tracking_items() const {
        return climon_tracking_items_.load(std::memory_order_relaxed);
    }
    void climon_note_tracking_prefix_delta(int64_t delta) {
        climon_tracking_prefixes_.fetch_add(static_cast<uint64_t>(delta),
                                            std::memory_order_relaxed);
    }
    uint64_t climon_tracking_prefixes() const {
        return climon_tracking_prefixes_.load(std::memory_order_relaxed);
    }

    void note_client_output_buffer_limit_disconnect() {
        client_output_buffer_limit_disconnections_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t client_output_buffer_limit_disconnections() const {
        return client_output_buffer_limit_disconnections_.load(std::memory_order_relaxed);
    }

    void blocking_client_parked() {
        blocked_clients_.fetch_add(1, std::memory_order_relaxed);
    }
    void blocking_client_unparked() {
        blocked_clients_.fetch_sub(1, std::memory_order_relaxed);
    }
    uint64_t blocked_clients() const {
        return blocked_clients_.load(std::memory_order_relaxed);
    }
    void blocking_waiter_added() {
        blocking_waiters_.fetch_add(1, std::memory_order_relaxed);
    }
    void blocking_waiter_removed() {
        blocking_waiters_.fetch_sub(1, std::memory_order_relaxed);
    }
    uint64_t blocking_waiters() const {
        return blocking_waiters_.load(std::memory_order_relaxed);
    }

    // The disabled AUTH fast path is one acquire load plus one predicted-not-taken branch in the
    // IO parser. Password bytes remain in the cold CONFIG table; only AUTH and live CONFIG changes
    // ever take its lock.
    static constexpr uint8_t kSecurityAuth = 1u << 0;
    static constexpr uint8_t kSecurityAcl = 1u << 1;
    uint8_t security_flags() const { return security_flags_.load(std::memory_order_acquire); }
    bool requirepass_enabled() const { return (security_flags() & kSecurityAuth) != 0; }
    bool acl_active() const { return (security_flags() & kSecurityAcl) != 0; }
    void set_auth_config(bool required) {
        const uint64_t write_version = begin_live_config_update();
        uint8_t flags = security_flags_.load(std::memory_order_relaxed);
        flags = required ? static_cast<uint8_t>(flags | kSecurityAuth)
                         : static_cast<uint8_t>(flags & ~kSecurityAuth);
        security_flags_.store(flags, std::memory_order_relaxed);
        end_live_config_update(write_version);
    }
    void set_acl_active(bool active) {
        acl_active_desired_.store(active, std::memory_order_release);
        if (!active && acl_kill_broadcasts_.load(std::memory_order_acquire) != 0) return;
        const uint64_t write_version = begin_live_config_update();
        uint8_t flags = security_flags_.load(std::memory_order_relaxed);
        flags = active ? static_cast<uint8_t>(flags | kSecurityAcl)
                       : static_cast<uint8_t>(flags & ~kSecurityAcl);
        security_flags_.store(flags, std::memory_order_relaxed);
        end_live_config_update(write_version);
    }
    void acl_kill_broadcast_started(uint32_t targets) {
        acl_kill_broadcasts_.fetch_add(targets, std::memory_order_acq_rel);
    }
    void acl_kill_broadcast_finished() {
        if (acl_kill_broadcasts_.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
        if (!acl_active_desired_.load(std::memory_order_acquire)) set_acl_active(false);
    }
    bool protected_mode() const {
        return protected_mode_.load(std::memory_order_acquire);
    }
    void set_protected_mode(bool enabled) {
        protected_mode_.store(enabled, std::memory_order_release);
    }
    bool active_expire_enabled() const {
        return active_expire_enabled_.load(std::memory_order_relaxed);
    }
    void set_active_expire_enabled(bool enabled) {
        active_expire_enabled_.store(enabled, std::memory_order_relaxed);
    }
    void note_auth_failure() { auth_failures_.fetch_add(1, std::memory_order_relaxed); }
    uint64_t auth_failures() const { return auth_failures_.load(std::memory_order_relaxed); }
    void acl_perm_retired() { acl_perm_retired_.fetch_add(1, std::memory_order_relaxed); }
    uint64_t acl_perm_retired_count() const {
        return acl_perm_retired_.load(std::memory_order_relaxed);
    }
    void acl_pubsub_client_killed() {
        acl_pubsub_clients_killed_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t acl_pubsub_clients_killed() const {
        return acl_pubsub_clients_killed_.load(std::memory_order_relaxed);
    }
    void note_rejected_connection() {
        rejected_connections_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t rejected_connections() const {
        return rejected_connections_.load(std::memory_order_relaxed);
    }

    uint64_t atomic_mode_state() const {
        return atomic_activity_.load(std::memory_order_acquire);
    }
    bool atomic_enabled() const { return atomic_mode_state() & kAtomicEnabledBit; }
    bool atomic_work_active() const { return (atomic_mode_state() & ~kAtomicEnabledBit) != 0; }
    uint32_t atomic_window() const {
        return atomic_credits_.window();
    }
    void set_atomic_enabled(bool enabled) {
        if (enabled) {
            atomic_activity_.fetch_or(kAtomicEnabledBit, std::memory_order_release);
        } else {
            atomic_activity_.fetch_and(~kAtomicEnabledBit, std::memory_order_release);
        }
    }
    void set_atomic_window(uint32_t window) {
        // CONFIG publishes process-global settings from shard 0 only (cmd_config). Its owner is
        // the sole window writer, including across migration; admission/retirement only count.
        atomic_credits_.set_window(window);
    }
    bool atomic_tracking_active() const {
        return atomic_mode_state() != 0;
    }
    bool atomic_can_admit(uint32_t /*owner_io*/, bool force = false) const {
        if (snapshot_atomic_barrier_.load(std::memory_order_acquire)) return false;
        if (!force && !(atomic_mode_state() & kAtomicEnabledBit)) return true;
        return atomic_credits_.can_admit();
    }
    bool atomic_try_admit(uint32_t owner_io, uint64_t& admission_token,
                          bool force = false) {
        admission_token = 0;
        if (snapshot_atomic_barrier_.load(std::memory_order_acquire)) return false;
        if (!force && !(atomic_mode_state() & kAtomicEnabledBit)) return false;
        const auto attempt = atomic_credits_.try_admit();
        if (attempt != AtomicAdmissionCredits::Attempt::Admitted) {
            if (attempt == AtomicAdmissionCredits::Attempt::Full)
                atomic_window_stalls_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        AtomicAdmissionState& local = thread(owner_io).atomic_admission_state();
        const bool first = local.active++ == 0;
        if (first) atomic_activity_.fetch_add(1, std::memory_order_release);
        if (snapshot_atomic_barrier_.load(std::memory_order_acquire) ||
            (!force && !(atomic_mode_state() & kAtomicEnabledBit))) {
            local.active--;
            if (first) atomic_activity_.fetch_sub(1, std::memory_order_release);
            atomic_credits_.retire();
            return false;
        }
        thread(owner_io).note_atomic_group();
        // Existing EXEC/scatter state has a receipt word named admission_generation. Keep its
        // representation (and layouts) while removing generation-dependent credit accounting.
        admission_token = 1;
        return true;
    }
    void set_snapshot_atomic_barrier(bool enabled) {
        snapshot_atomic_barrier_.store(enabled, std::memory_order_release);
    }
    void atomic_retire_group(uint32_t owner_io, uint64_t admission_token) {
        AtomicAdmissionState& local = thread(owner_io).atomic_admission_state();
        if (admission_token != 1 || !local.active) std::abort();
        local.active--;
        atomic_credits_.retire();
        if (!local.active) atomic_activity_.fetch_sub(1, std::memory_order_release);
    }
    // GROUP COMMIT IS TWO STEPS AND A READER MUST NEVER SEE THE FIRST WITHOUT THE SECOND.
    // A cross-shard group installs its versions on every owner while the shared epoch word still
    // reads zero (undecided ⇒ invisible), and only the last owner turns it into a ticket. That
    // ticket used to be drawn straight out of commit_seq_, so between the draw and the
    // `epoch.store(ticket)` two instructions later the sequence already named a commit whose
    // records still answered "undecided". A reader whose cut landed in that hole saw the group
    // for the fragments it read AFTER the store and missed it for the fragments it read BEFORE --
    // one MGET, two generations. The hole is nanoseconds wide and a preemption between the two
    // instructions makes it milliseconds wide; it produced exactly one torn MGET per ~1.1M
    // pipelined batches on the session-monotonicity hammer.
    //
    // So the visible read watermark is no longer the drawn sequence. Committers bracket
    // [draw, publish] in atomic_commit_inflight_, and whoever takes the count to zero republishes
    // the drawn sequence it read BEFORE its own decrement into atomic_commit_safe_: at that
    // instant every ticket at or below that value has already stored its epoch, because a
    // committer that had not would still be holding the count up. Readers load one word exactly
    // as before -- the cost is on the commit side, and it is two RMWs on a line commits already
    // own.
    uint64_t atomic_commit_reserve(uint64_t tickets = 1) {
        if (!tickets) std::abort();
        atomic_commit_inflight_.fetch_add(1, std::memory_order_seq_cst);
        return commit_seq_.fetch_add(tickets, std::memory_order_seq_cst) + 1;
    }
    void atomic_commit_publish() {
        // Loaded BEFORE the decrement on purpose: it is the value whose publication the count
        // still proves. Reading it after would let a ticket drawn by a newcomer ride in.
        const uint64_t drawn = commit_seq_.load(std::memory_order_seq_cst);
        if (atomic_commit_inflight_.fetch_sub(1, std::memory_order_acq_rel) != 1) {
            // Another committer is still between its draw and its store, so the watermark stays
            // where it is and every reader's cut stays below that undecided ticket. Counting it is
            // how the regression battery proves the guarded window actually opened.
            atomic_commit_windows_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        uint64_t safe = atomic_commit_safe_.load(std::memory_order_relaxed);
        while (safe < drawn &&
               !atomic_commit_safe_.compare_exchange_weak(
                   safe, drawn, std::memory_order_release, std::memory_order_relaxed)) {}
    }
    // Reserve a consecutive run under ONE commit bracket, then let the caller release-store every
    // decision word in its completion order before the safe watermark moves. The stall before the
    // first store is the atomic-ON use of DEBUG's shared hop delay: it widens the closed window on
    // demand, is zero in production, and the load is on an already-cold group/batch path. For a
    // one-ticket batch this is exactly the historical reserve/delay/store/publish sequence, while a
    // larger batch widens one common closed window.
    template <typename StoreTickets>
    uint64_t atomic_commit_batch(uint64_t tickets, StoreTickets&& store_tickets) {
        const uint64_t first = atomic_commit_reserve(tickets);
        const uint32_t stall = debug_hop_delay_.load(std::memory_order_relaxed);
        if (__builtin_expect(stall != 0, false)) debug_stall_us(stall);
        store_tickets(first);
        atomic_commit_publish();
        return first;
    }
    // The whole two-step for a group whose epoch word is `epoch`. `publish_members` lets a
    // composite group install the SAME ticket into additional decision words before the safe
    // watermark moves. Readers therefore still see either all of the ticket or none of it.
    template <typename PublishMembers>
    uint64_t atomic_commit_group(std::atomic<uint64_t>& epoch,
                                 PublishMembers&& publish_members) {
        return atomic_commit_batch(1, [&](uint64_t ticket) {
            epoch.store(ticket, std::memory_order_release);
            publish_members(ticket);
        });
    }
    uint64_t atomic_commit_group(std::atomic<uint64_t>& epoch) {
        return atomic_commit_group(epoch, [](uint64_t) {});
    }
    // Same-owner tickets (a localfast plain version, FLUSH's logical clear) have no window: the
    // shard that draws them installs their records before it yields, and no other task may touch
    // that shard in between. They still travel through the bracket so the watermark can never
    // regress behind them.
    uint64_t atomic_commit() {
        const uint64_t ticket = atomic_commit_reserve();
        atomic_commit_publish();
        return ticket;
    }
    uint64_t atomic_snapshot() const {
        return atomic_commit_safe_.load(std::memory_order_seq_cst);
    }
    uint64_t script_crossshard_max_bytes() const {
        return static_cast<uint64_t>(cfg_.script_crossshard_max_bytes);
    }
    uint64_t script_crossshard_workbench_bytes() const {
        return static_cast<uint64_t>(cfg_.script_crossshard_workbench_bytes);
    }
    uint32_t script_crossshard_conflict_retries() const {
        return static_cast<uint32_t>(cfg_.script_crossshard_conflict_retries);
    }
    uint32_t script_crossshard_cut_slots() const {
        return static_cast<uint32_t>(cfg_.script_crossshard_cut_slots);
    }
    void note_script_stage_owner(uint64_t bytes) {
        script_stage_owner_tasks_.fetch_add(1, std::memory_order_relaxed);
        script_staged_bytes_total_.fetch_add(bytes, std::memory_order_relaxed);
    }
    void note_script_run() { script_run_attempts_.fetch_add(1, std::memory_order_relaxed); }
    void note_script_validate_owner() {
        script_validate_owner_tasks_.fetch_add(1, std::memory_order_relaxed);
    }
    void note_script_apply_owner() {
        script_apply_owner_tasks_.fetch_add(1, std::memory_order_relaxed);
    }
    void note_script_activation() {
        script_crossshard_activations_.fetch_add(1, std::memory_order_relaxed);
    }
    void note_script_group_commit() {
        script_group_commits_.fetch_add(1, std::memory_order_relaxed);
    }
    void note_script_retry() { script_group_occ_retries_.fetch_add(1, std::memory_order_relaxed); }
    void note_script_giveup() { script_group_occ_giveups_.fetch_add(1, std::memory_order_relaxed); }
    // THE TWO COUNTERS THAT MAKE SORT's BY/GET DEREFERENCE FALSIFIABLE.  A battery that only
    // compares replies cannot tell a dereference that RAN from one whose patterns happened to
    // resolve to nothing, and cannot tell a refusal that FIRED from a command that failed earlier
    // for an unrelated reason.  lookups counts one per derived key actually read; refusals counts
    // one per BY/GET rejected by the single-owner admission rule.  Each is the other's control.
    void note_sort_deref_lookup() { sort_deref_lookups_.fetch_add(1, std::memory_order_relaxed); }
    void note_sort_deref_refusal() { sort_deref_refusals_.fetch_add(1, std::memory_order_relaxed); }
    // One per BY/GET SORT finished inside a phase-one owner task, i.e. one per general SORT that
    // took the cross-shard engine rather than localfast. Without it a green battery cannot tell
    // which of the two arms it actually exercised.
    void note_sort_scatter_general() {
        sort_scatter_general_.fetch_add(1, std::memory_order_relaxed);
    }
    // MUST STAY ZERO. One per dereference that would have read a shard this executor does not own
    // -- structurally unreachable, because the parse-time admission rule refuses those patterns.
    // A non-zero reading means the admission rule and the executor map disagree.
    void note_sort_deref_escape() { sort_deref_escapes_.fetch_add(1, std::memory_order_relaxed); }
    void note_script_window_refusal() {
        script_crossshard_window_refusals_.fetch_add(1, std::memory_order_relaxed);
    }
    void note_script_abort_oom() {
        script_group_aborts_oom_.fetch_add(1, std::memory_order_relaxed);
    }
    // THE THREE COUNTERS THAT MAKE THE RESERVATION SUB-WAVE FALSIFIABLE.
    //
    // AMENDMENT 1 requires proof that reservation ARMED, not proof that the phases ran: an
    // activation whose PIN wave silently armed nothing looks identical from the outside -- same
    // replies, same phase counters, same commit -- right up until a competing write is missed.
    //   script_keys_armed          one per declared key, per owner PIN task
    //   script_keys_released       one per UNPIN; armed - released must be 0 at rest
    //   script_write_tickets_forced a plain write that would have taken the untracked physical
    //                              path and instead materialized an MVCC version BECAUSE the key
    //                              was reserved. This is the counter that proves the arming is
    //                              load-bearing rather than merely present.
    void note_script_key_armed() { script_keys_armed_.fetch_add(1, std::memory_order_relaxed); }
    void note_script_key_released() {
        script_keys_released_.fetch_add(1, std::memory_order_relaxed);
    }
    void note_script_forced_write() {
        script_write_tickets_forced_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t script_keys_armed() const { return script_keys_armed_.load(); }
    uint64_t script_keys_released() const { return script_keys_released_.load(); }
    uint64_t script_intents_live() const {
        const uint64_t armed = script_keys_armed_.load(std::memory_order_acquire);
        return armed - script_keys_released_.load(std::memory_order_acquire);
    }
    uint64_t script_write_tickets_forced() const { return script_write_tickets_forced_.load(); }
    uint64_t script_stage_owner_tasks() const { return script_stage_owner_tasks_.load(); }
    uint64_t script_run_attempts() const { return script_run_attempts_.load(); }
    uint64_t script_validate_owner_tasks() const { return script_validate_owner_tasks_.load(); }
    uint64_t script_apply_owner_tasks() const { return script_apply_owner_tasks_.load(); }
    uint64_t script_crossshard_activations() const { return script_crossshard_activations_.load(); }
    uint64_t script_group_commits() const { return script_group_commits_.load(); }
    uint64_t script_group_occ_retries() const { return script_group_occ_retries_.load(); }
    uint64_t script_group_occ_giveups() const { return script_group_occ_giveups_.load(); }
    uint64_t script_staged_bytes_total() const { return script_staged_bytes_total_.load(); }
    uint64_t script_crossshard_window_refusals() const {
        return script_crossshard_window_refusals_.load();
    }
    uint64_t script_group_aborts_oom() const { return script_group_aborts_oom_.load(); }
    uint64_t sort_deref_lookups() const { return sort_deref_lookups_.load(); }
    uint64_t sort_deref_refusals() const { return sort_deref_refusals_.load(); }
    uint64_t sort_scatter_general() const { return sort_scatter_general_.load(); }
    uint64_t sort_deref_escapes() const { return sort_deref_escapes_.load(); }
    bool script_intents_active() const {
        return script_intent_owners_.load(std::memory_order_acquire) != 0;
    }
    bool script_try_certification() {
        bool expected = false;
        return script_certification_active_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_relaxed);
    }
    void script_finish_certification() {
        if (!script_certification_active_.exchange(false, std::memory_order_release))
            std::abort();
    }
    // The raw drawn sequence. NOT a read cut: it may already name a group whose epoch word still
    // reads zero. Only the instrumentation compares the two.
    uint64_t atomic_commit_drawn() const {
        return commit_seq_.load(std::memory_order_seq_cst);
    }
    void publish_atomic_read_floor(uint32_t thread, uint64_t floor) {
        atomic_read_floors_[thread].store(floor, std::memory_order_seq_cst);
    }
    void note_atomic_snapshot_complete(uint32_t thread) {
        atomic_snapshot_completions_[thread].fetch_add(1, std::memory_order_release);
    }
    uint64_t atomic_snapshot_completions(uint32_t thread) const {
        return atomic_snapshot_completions_[thread].load(std::memory_order_acquire);
    }
    uint64_t atomic_read_floor() const {
        // Lifetime invariant: promotion frees only versions whose tickets are strictly below this
        // minimum and no newer than its pre-floor commit cutoff. Readers publish the successor of
        // their inclusive cut, so the winner at that cut is eligible to become the sole physical
        // representation. Snapshot registration publishes then confirms commit_seq; these
        // seq_cst operations ensure a cleanup that missed the
        // publication has an older cutoff. Thus no active IO snapshot can later dereference a
        // freed loser. UINT64_MAX means no registered reader constrains the floor.
        uint64_t floor = UINT64_MAX;
        // Only IO threads register read cuts. Executor slots are initialized to MAX and never
        // publish, so scanning them merely repeats work in every owner cleanup pass.
        for (uint32_t i : placement_.ifid_threads())
            floor = std::min(floor,
                             atomic_read_floors_[i].load(std::memory_order_seq_cst));
        return floor;
    }
    uint64_t atomic_groups() const {
        uint64_t groups = 0;
        for (uint32_t io : placement_.ifid_threads()) groups += thread(io).atomic_groups();
        return groups;
    }
    uint64_t atomic_inflight() const { return atomic_credits_.active(); }
    // GROUPS WHOSE RECORDS ARE ONLY PARTLY INSTALLED -- the quantity a snapshot cut must wait on.
    // It is NOT atomic_inflight(). A group leaves atomic_inflight() only when its REPLY retires on
    // the IO thread that admitted it, and that retire runs in the very loop a blocking SAVE is
    // sitting inside: waiting for atomic_inflight()==0 from a SAVE wedges the server (measured --
    // rdb_bgsave_in_progress stuck at 1 with 13 leases held). Records, by contrast, are dispatched
    // to every participating owner in ONE indivisible IO step (io_loop.h checks free slots on all
    // participants and only then posts, aborting on a failed post), and they finish on the OWNER
    // threads, which keep running while a SAVE blocks. So this window opens at admission and closes
    // either at the owner-side completion or at the synchronous pre-dispatch teardown -- never on a
    // thread a snapshot can be occupying.
    uint64_t atomic_apply_inflight() const {
        uint64_t closed = 0;
        uint64_t opened = 0;
        // CLOSES FIRST, in a distinct whole pass.  If this scan observes a close, that close
        // acquired the group's published open flag, so the later load of its opening counter
        // cannot miss the matching open.  A close racing after this first pass only makes the
        // answer conservatively high.
        for (uint32_t tid = 0; tid < nthreads(); tid++)
            closed += atomic_apply_slots_[tid].closed.load(std::memory_order_acquire);
        for (uint32_t tid = 0; tid < nthreads(); tid++)
            opened += atomic_apply_slots_[tid].opened.load(std::memory_order_acquire);
        if (closed > opened) std::abort();
        return opened - closed;
    }
    void atomic_apply_open(uint32_t tid, std::atomic<bool>& flag) {
        if (tid >= nthreads()) std::abort();
        // Publish the counter before making the close claim available to another thread.
        atomic_apply_slots_[tid].opened.fetch_add(1, std::memory_order_release);
        flag.store(true, std::memory_order_release);
    }
    // Idempotent by construction: whichever of the two ends reaches the group first closes it.
    void atomic_apply_close(uint32_t tid, std::atomic<bool>& flag) {
        if (tid >= nthreads()) std::abort();
        if (__builtin_expect(!flag.load(std::memory_order_acquire), true)) return;
        if (flag.exchange(false, std::memory_order_acq_rel))
            atomic_apply_slots_[tid].closed.fetch_add(1, std::memory_order_release);
    }
    uint64_t atomic_window_stalls() const {
        return atomic_window_stalls_.load(std::memory_order_relaxed);
    }
    // TEST HOOK (DEBUG ATOMIC-DIRECT-DEFER). Number of extra owner passes a cross-shard RENAME's
    // destination task is held after its source hop is ready. Zero in production; its only reader
    // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
    // other command. It widens -- deterministically -- the window in which a younger whole-owner
    // walker could overtake an older same-connection group on the destination shard.
    // TEST HOOK (P128.md section 8): effective local-read LANE CAPACITY for admission. 0 means
    // derive, which is kInboxSlots and is what production always runs. A non-zero value only makes
    // the armed parser stop admitting sooner; the physical ring keeps its kInboxSlots entries and
    // its masking, so this can never overrun anything. It exists because lane oversubscription is
    // otherwise a RATE RACE the gate cannot win -- a test client cannot push 1024 frames into one
    // fused thread faster than that thread drains them, so the anti-vacuity row of the lane battery
    // could only fire on a saturated rig. With a cap, a single connection pipelining more than the
    // cap in ONE parse pass fills the lane inside that pass, before any drain can run, which makes
    // the row deterministic at gate scale. Read once per rotation by each fused thread, never per
    // op. Reachable only through the already-gated DEBUG command.
    uint32_t debug_read_local_lane_cap() const {
        return debug_read_local_lane_cap_.load(std::memory_order_relaxed);
    }
    void set_debug_read_local_lane_cap(uint32_t cap) {
        debug_read_local_lane_cap_.store(cap, std::memory_order_relaxed);
    }
    uint32_t debug_atomic_direct_defer() const {
        return debug_atomic_direct_defer_.load(std::memory_order_relaxed);
    }
    void set_debug_atomic_direct_defer(uint32_t passes) {
        debug_atomic_direct_defer_.store(passes, std::memory_order_relaxed);
    }
    // TEST HOOK shared by DEBUG ATOMIC-COMMIT-DELAY and ATOMIC-OFF-HOP-DELAY. Atomic ON stalls a
    // commit batch between its ticket reservation and the decision stores (read by
    // atomic_commit_batch(), a cold group/batch path); atomic OFF parks non-lead owners at the
    // first cross-owner mutation wave. Both aliases write this one word, so last writer wins and
    // zero disarms both. It turns the reserve/publish hole into a window wide enough for a reader
    // to straddle, which is how the torn MGET is reproduced on demand instead of once per 1.1M
    // batches. Reads occur only on the cold group/scatter paths, never GET/SET.
    uint32_t debug_hop_delay() const {
        return debug_hop_delay_.load(std::memory_order_relaxed);
    }
    void set_debug_hop_delay(uint32_t microseconds) {
        debug_hop_delay_.store(microseconds, std::memory_order_relaxed);
    }
    // TEST HOOK (DEBUG ATOMIC-FANOUT-DEFER). Microseconds every fragment of a cross-shard READ
    // except the one on its lead shard is PARKED -- re-queued, not spun -- after the command is
    // dispatched. That park is the fan-out window: the lead fragment answers from the world before
    // a transaction, the parked ones answer after its one ticket lands, and a reader with no pinned
    // cut then returns two generations in one reply. Parking rather than stalling is deliberate:
    // the executor stays free, so the transaction the test is racing can actually run and commit
    // inside the window. Zero in production; read once per cross-shard read at prepare time on the
    // already-cold scatter path, never on GET/SET.
    // The fused read-local lane serves a clean MGET without the scatter engine, so it has its own
    // half of this hook (ExLoopT::debug_fanout_stall_local). That path never loads this atomic --
    // it shares a line with the commit sequence -- so arm/disarm publishes through the live-config
    // version and every fused executor latches the value on its next pass, like a CONFIG SET.
    void set_debug_atomic_fanout_defer(uint32_t microseconds) {
        const uint64_t version = begin_live_config_update();
        debug_atomic_fanout_defer_.store(microseconds, std::memory_order_relaxed);
        end_live_config_update(version);
    }
    uint32_t debug_atomic_fanout_defer() const {
        return debug_atomic_fanout_defer_.load(std::memory_order_relaxed);
    }
    // TEST HOOK (DEBUG ATOMIC-CONDITIONAL-DEFER). Microseconds the destination-validation task of
    // an atomic-OFF RENAMENX/COPY is PARKED during phase one. Two contenders can therefore both
    // observe the empty destination before either phase-two install runs. Parking keeps the owner
    // free to execute the competing task; zero is the production default.
    void set_debug_atomic_conditional_defer(uint32_t microseconds) {
        const uint64_t deadline = microseconds
            ? now_ns() + static_cast<uint64_t>(microseconds) * 1000ull : 0;
        debug_atomic_conditional_deadline_.store(deadline, std::memory_order_relaxed);
    }
    uint64_t debug_atomic_conditional_deadline() const {
        return debug_atomic_conditional_deadline_.load(std::memory_order_relaxed);
    }
    // TEST HOOK (DEBUG SCRIPT-STAGE-DEFER). Microseconds every cross-owner script GATHER task
    // except the one on the coordinator's own shard is PARKED -- re-queued, not spun -- after the
    // activation has reserved all of its declared keys and pinned its cut. That park IS the window
    // AMENDMENT 1 is about: the coordinator's own key is read from the world before a competing
    // plain write, the parked ones read after it, and an activation whose reservation did not
    // actually arm those keys therefore composes two generations into one reply and never notices.
    // Parking rather than stalling keeps the executor free so the racing write can really run.
    // Zero in production; read once per activation at prepare time on the cold scatter path.
    void set_debug_script_stage_defer(uint32_t microseconds) {
        debug_script_stage_defer_.store(microseconds, std::memory_order_relaxed);
    }
    uint32_t debug_script_stage_defer() const {
        return debug_script_stage_defer_.load(std::memory_order_relaxed);
    }
    // TEST HOOK (DEBUG ATOMIC-READ-DELAY). Microseconds a plain read is held on its owner before
    // it resolves, widening the gap between the IO-side dispatch of a pipelined read and its
    // execution. That gap is the session-monotonicity window: foreign commits landing inside it
    // used to make the earlier command answer with a newer world than the later one.
    void set_debug_atomic_read_delay(uint32_t microseconds) {
        debug_atomic_read_delay_.store(microseconds, std::memory_order_relaxed);
    }
    uint32_t debug_atomic_read_delay() const {
        return debug_atomic_read_delay_.load(std::memory_order_relaxed);
    }
    // TEST HOOK (DEBUG BARRIER-HOLD). While armed, every blocking dispatch pins a SECOND owner
    // (BarrierOwner::Debug) on its connection's parse barrier; flush_ready drops that bit again as
    // soon as the latch is cleared. Zero in production, and read at exactly two already-cold
    // places: the blocking dispatch arm, and the arm of flush_ready that only runs when a barrier
    // is already set. Nothing on GET/SET touches it.
    //
    // Why a hook is needed at all: the barrier's six owners cannot overlap on any reachable
    // sequence (NOTES-BARRIER.md section 2), so the state the owner-scoped release exists to
    // survive -- a release that must NOT drop the barrier -- has to be injected. It is held past
    // ROB quiescence on purpose; barrier_release_quiesced() exempts this one bit for that reason.
    //
    // A LATCH, NOT A DEADLINE, and that is not a style choice. A barred connection's io thread has
    // nothing left to do and parks on submit_and_wait(1) -- unbounded under io_uring. A hold that
    // expired on a clock would therefore never be noticed, and the connection would hang instead
    // of resuming. The releasing edge has to be an EVENT. The test clears this latch from a second
    // connection and then wakes every io thread through an existing fan-out (CLIENT LIST), which
    // is what makes the resume observable rather than theoretical.
    void set_debug_barrier_hold(uint32_t armed) {
        debug_barrier_hold_.store(armed, std::memory_order_relaxed);
    }
    bool debug_barrier_hold_armed() const {
        return debug_barrier_hold_.load(std::memory_order_relaxed) != 0;
    }
    // TEST HOOK (DEBUG BLOCKING-TIMEOUT-REAP). One blocking retirement consumes the arm and makes
    // its owner's client cron due in that same IO pass. This makes the otherwise probabilistic
    // ordering between reply retirement and the 100 ms timeout beat deterministic.
    void set_debug_blocking_timeout_reap(bool armed) {
        debug_blocking_timeout_reap_.store(armed, std::memory_order_release);
    }
    bool debug_blocking_timeout_reap_take() {
        if (!debug_blocking_timeout_reap_.load(std::memory_order_acquire) ||
            !debug_blocking_timeout_reap_.exchange(false, std::memory_order_acq_rel))
            return false;
        debug_blocking_timeout_reaps_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    uint64_t debug_blocking_timeout_reaps() const {
        return debug_blocking_timeout_reaps_.load(std::memory_order_relaxed);
    }
    // An overlap: some owner took the parse barrier while another owner already held it. With
    // DEBUG BARRIER-HOLD off this must read ZERO -- it is the live form of the reachability verdict
    // in NOTES-BARRIER.md, and if it ever moves on production traffic the latent case just went
    // live and the owner-scoped release in blocking_retire() became load-bearing rather than
    // defensive. With the latch ON it advances once per blocking dispatch, which is the counter's
    // own positive control: a "must be zero" reading proves nothing until something has been shown
    // able to make it non-zero.
    void note_barrier_overlap() {
        barrier_owner_overlaps_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t barrier_owner_overlaps() const {
        return barrier_owner_overlaps_.load(std::memory_order_relaxed);
    }
    // A blocking retirement released its own claim and the barrier STAYED UP because another owner
    // still held it. This is the instruction the old unconditional clear got wrong, so a validation
    // run that never moves this counter never reached the geometry it claims to cover -- it must
    // FAIL LOUDLY rather than report a pass.
    void note_barrier_release_held() {
        barrier_releases_held_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t barrier_releases_held() const {
        return barrier_releases_held_.load(std::memory_order_relaxed);
    }
    // Counters that keep the regression battery from passing vacuously: they prove the guarded
    // windows actually opened during the run rather than merely that nothing broke.
    void note_atomic_commit_hold() {
        atomic_commit_holds_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t atomic_commit_holds() const {
        return atomic_commit_holds_.load(std::memory_order_relaxed);
    }
    uint64_t atomic_commit_windows() const {
        return atomic_commit_windows_.load(std::memory_order_relaxed);
    }
    // The guarded path of the EXEC fan-out fix: a cross-shard READ that pinned a cut although the
    // tracking word read zero at prepare time. Before the fix that exact sample sent the read out
    // with no cut at all, so every fragment answered "newest committed right now" and a transaction
    // committing inside the fan-out was seen by some fragments and missed by others. A regression
    // that never moves this counter never entered the window it claims to close.
    void note_atomic_fanout_cut() {
        atomic_fanout_cuts_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t atomic_fanout_cuts() const {
        return atomic_fanout_cuts_.load(std::memory_order_relaxed);
    }
    // The EXEC half of the same story, counted separately so a gate row cannot be satisfied by
    // ordinary bare traffic moving atomic_fanout_cuts underneath it. One per transaction that
    // published a read cut because it carries a read whose fragments span more than one owner.
    // Before this lane a MULTI child ran with `!force_atomic`, never reached the read-cut
    // machinery, and its fragments each answered "newest committed right now": a transaction
    // committing inside the fan-out was seen by some and missed by others, so MULTI/EXEC gave a
    // cross-shard read LESS isolation than the same command run bare.
    void note_atomic_exec_read_cut() {
        atomic_exec_read_cuts_.fetch_add(1, std::memory_order_relaxed);
        atomic_fanout_cuts_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t atomic_exec_read_cuts() const {
        return atomic_exec_read_cuts_.load(std::memory_order_relaxed);
    }
    void note_atomic_read_cut_held() {
        atomic_read_cuts_held_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t atomic_read_cuts_held() const {
        return atomic_read_cuts_held_.load(std::memory_order_relaxed);
    }
    __attribute__((noinline, cold)) static void debug_stall_us(uint32_t microseconds) {
        const uint64_t deadline = now_ns() + static_cast<uint64_t>(microseconds) * 1000ull;
        while (now_ns() < deadline) __builtin_ia32_pause();
    }
    uint32_t atomic_credit_pool() const { return atomic_credits_.pool(); }
    uint32_t atomic_credit_debt() const { return atomic_credits_.debt(); }

    // A mailbox belongs to a physical worker, across fused/split roles and shard migrations.
    // Callers copy the result before their next snapshot call; only the consumer-owned slot is read.
    LiveConfigSnapshot live_config_snapshot(uint32_t reader_tid) const {
        return live_config_mailboxes_[reader_tid].read(live_config_version_).live;
    }
    bool live_config_snapshot_if_changed(uint32_t reader_tid, uint64_t known_version,
                                         LiveConfigSnapshot& snapshot) const {
        if (live_config_version_.load(std::memory_order_acquire) == known_version) return false;
        snapshot = live_config_snapshot(reader_tid);
        return snapshot.version != known_version;
    }
    void set_maxmemory_config(uint64_t memory, MaxmemoryPolicy policy, uint32_t samples,
                              bool set_memory = true, bool set_policy = true,
                              bool set_samples = true) {
        const uint64_t write_version = begin_live_config_update();
        if (set_memory) live_maxmemory_.store(memory, std::memory_order_relaxed);
        if (set_policy)
            live_maxmemory_policy_.store(static_cast<uint8_t>(policy), std::memory_order_relaxed);
        if (set_samples) live_maxmemory_samples_.store(samples, std::memory_order_relaxed);
        end_live_config_update(write_version);
    }
    void set_notify_events(uint32_t value) {
        const uint64_t write_version = begin_live_config_update();
        live_notify_events_.store(value, std::memory_order_relaxed);
        end_live_config_update(write_version);
    }
    void set_slowlog_config(int64_t slowlog_us, uint32_t latency_ms) {
        const uint64_t write_version = begin_live_config_update();
        live_slowlog_us_.store(slowlog_us, std::memory_order_relaxed);
        live_latency_ms_.store(latency_ms, std::memory_order_relaxed);
        end_live_config_update(write_version);
    }
    int64_t slowlog_log_slower_than() const {
        return live_slowlog_us_.load(std::memory_order_relaxed);
    }
    uint32_t latency_monitor_threshold() const {
        return live_latency_ms_.load(std::memory_order_relaxed);
    }

    bool pubsub_any_subscribers() const {
        return pubsub_active_channels_.load(std::memory_order_relaxed) != 0 ||
               pubsub_pattern_subscriptions_.load(std::memory_order_relaxed) != 0;
    }
    bool pubsub_any_shard_subscribers() const {
        return pubsub_shard_channels_.load(std::memory_order_relaxed) != 0;
    }
    void notify_event_fired() { notify_events_fired_.fetch_add(1, std::memory_order_relaxed); }
    void notify_event_dropped(uint64_t count = 1) {
        notify_events_dropped_.fetch_add(count, std::memory_order_relaxed);
    }
    uint64_t notify_events_fired() const {
        return notify_events_fired_.load(std::memory_order_relaxed);
    }
    uint64_t notify_events_dropped() const {
        return notify_events_dropped_.load(std::memory_order_relaxed);
    }
    void client_scatter_started() {
        client_scatter_requests_.fetch_add(1, std::memory_order_relaxed);
    }
    void client_scatter_io_replied() {
        client_scatter_io_responses_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t client_scatter_requests() const {
        return client_scatter_requests_.load(std::memory_order_relaxed);
    }
    uint64_t client_scatter_io_responses() const {
        return client_scatter_io_responses_.load(std::memory_order_relaxed);
    }

    // Pub/sub is IO-owned. These atomics are reporting/lifetime gauges only and are never read or
    // written by the plain key-command path.
    void pubsub_event_created() { pubsub_inflight_.fetch_add(1, std::memory_order_relaxed); }
    void pubsub_event_retired() {
        if (pubsub_inflight_.fetch_sub(1, std::memory_order_relaxed) == 0) std::abort();
    }
    // Fanout reporting. `blobs` is a lifetime gauge that must drain to zero (the refcount proof);
    // `deliveries / delivery_batches` is the batching proof -- a ratio of 1.0 means the scatter
    // machinery is posting one event per delivery again, which is the thing this design removed.
    void pubsub_blob_created() { pubsub_blobs_.fetch_add(1, std::memory_order_relaxed); }
    void pubsub_blob_retired() {
        if (pubsub_blobs_.fetch_sub(1, std::memory_order_relaxed) == 0) std::abort();
    }
    void pubsub_deliveries_added(uint64_t n) {
        pubsub_deliveries_.fetch_add(n, std::memory_order_relaxed);
    }
    void pubsub_delivery_batch_posted() {
        pubsub_delivery_batches_.fetch_add(1, std::memory_order_relaxed);
    }
    void pubsub_forwarded_stale_added(uint64_t n = 1) {
        pubsub_forwarded_stale_.fetch_add(n, std::memory_order_relaxed);
    }
    uint64_t pubsub_blobs() const { return pubsub_blobs_.load(std::memory_order_relaxed); }
    uint64_t pubsub_deliveries() const {
        return pubsub_deliveries_.load(std::memory_order_relaxed);
    }
    uint64_t pubsub_delivery_batches() const {
        return pubsub_delivery_batches_.load(std::memory_order_relaxed);
    }
    uint64_t pubsub_forwarded_stale() const {
        return pubsub_forwarded_stale_.load(std::memory_order_relaxed);
    }
    bool pubsub_notification_reserve(uint64_t count, uint64_t limit) {
        uint64_t current = pubsub_inflight_.load(std::memory_order_relaxed);
        while (count <= limit && current <= limit - count) {
            if (pubsub_inflight_.compare_exchange_weak(
                    current, current + count, std::memory_order_relaxed,
                    std::memory_order_relaxed)) return true;
        }
        return false;
    }
    void pubsub_notification_retire(uint64_t count) {
        const uint64_t previous = pubsub_inflight_.fetch_sub(count, std::memory_order_relaxed);
        if (previous < count) std::abort();
    }
    void pubsub_pending_started() { pubsub_pending_.fetch_add(1, std::memory_order_relaxed); }
    void pubsub_pending_finished() {
        if (pubsub_pending_.fetch_sub(1, std::memory_order_relaxed) == 0) std::abort();
    }
    void pubsub_home_entry_added() { pubsub_home_entries_.fetch_add(1, std::memory_order_relaxed); }
    void pubsub_home_entry_removed() {
        if (pubsub_home_entries_.fetch_sub(1, std::memory_order_relaxed) == 0) std::abort();
    }
    void pubsub_active_channel_added(bool shard = false) {
        (shard ? pubsub_shard_channels_ : pubsub_active_channels_)
            .fetch_add(1, std::memory_order_relaxed);
    }
    void pubsub_active_channel_removed(bool shard = false) {
        auto& value = shard ? pubsub_shard_channels_ : pubsub_active_channels_;
        if (value.fetch_sub(1, std::memory_order_relaxed) == 0) std::abort();
    }
    void pubsub_subscription_added(bool pattern, bool shard = false) {
        (shard ? pubsub_shard_subscriptions_
               : (pattern ? pubsub_pattern_subscriptions_ : pubsub_subscriptions_))
            .fetch_add(1, std::memory_order_relaxed);
    }
    void pubsub_subscription_removed(bool pattern, bool shard = false) {
        auto& value = shard ? pubsub_shard_subscriptions_
                            : (pattern ? pubsub_pattern_subscriptions_ : pubsub_subscriptions_);
        if (value.fetch_sub(1, std::memory_order_relaxed) == 0) std::abort();
    }
    uint64_t pubsub_inflight() const { return pubsub_inflight_.load(std::memory_order_relaxed); }
    uint64_t pubsub_pending() const { return pubsub_pending_.load(std::memory_order_relaxed); }
    uint64_t pubsub_home_entries() const {
        return pubsub_home_entries_.load(std::memory_order_relaxed);
    }
    uint64_t pubsub_active_channels() const {
        return pubsub_active_channels_.load(std::memory_order_relaxed);
    }
    uint64_t pubsub_subscriptions() const {
        return pubsub_subscriptions_.load(std::memory_order_relaxed);
    }
    uint64_t pubsub_pattern_subscriptions() const {
        return pubsub_pattern_subscriptions_.load(std::memory_order_relaxed);
    }
    uint64_t pubsub_shard_channels() const {
        return pubsub_shard_channels_.load(std::memory_order_relaxed);
    }
    uint64_t pubsub_shard_subscriptions() const {
        return pubsub_shard_subscriptions_.load(std::memory_order_relaxed);
    }

private:
    bool adjust_open_files_limit() {
        rlimit limit{};
        if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) {
            std::perror("getrlimit(RLIMIT_NOFILE)");
            return false;
        }
        const uint64_t reserve = 32 + placement_.ifid_threads().size() * 2;
        const uint64_t wanted = static_cast<uint64_t>(cfg_.maxclients) + reserve;
        if (limit.rlim_cur < wanted) {
            rlimit raised = limit;
            raised.rlim_cur = static_cast<rlim_t>(std::min<uint64_t>(wanted, limit.rlim_max));
            if (::setrlimit(RLIMIT_NOFILE, &raised) == 0)
                (void)::getrlimit(RLIMIT_NOFILE, &limit);
        }
        if (limit.rlim_cur >= wanted) return true;
        if (limit.rlim_cur <= reserve) {
            std::fprintf(stderr,
                "Your current 'ulimit -n' of %llu is not enough for the server to start. "
                "Please increase your open file limit to at least %llu. Exiting.\n",
                static_cast<unsigned long long>(limit.rlim_cur),
                static_cast<unsigned long long>(wanted));
            return false;
        }
        const uint32_t reduced = static_cast<uint32_t>(std::min<uint64_t>(
            limit.rlim_cur - reserve, UINT32_MAX));
        cfg_.maxclients = reduced;
        live_maxclients_.store(reduced, std::memory_order_relaxed);
        std::fprintf(stderr,
            "Current maximum open files is %llu. maxclients has been reduced to %u to compensate "
            "for low ulimit. If you need higher maxclients increase 'ulimit -n'.\n",
            static_cast<unsigned long long>(limit.rlim_cur), reduced);
        return true;
    }

    void check_tcp_backlog_settings() const {
        std::FILE* file = std::fopen("/proc/sys/net/core/somaxconn", "r");
        if (!file) return;
        int somaxconn = 0;
        const bool read = std::fscanf(file, "%d", &somaxconn) == 1;
        std::fclose(file);
        if (read && somaxconn < static_cast<int>(cfg_.tcp_backlog))
            std::fprintf(stderr,
                "WARNING: The TCP backlog setting of %u cannot be enforced because "
                "/proc/sys/net/core/somaxconn is set to the lower value of %d.\n",
                cfg_.tcp_backlog, somaxconn);
    }

    void store_client_output_buffer_limits(const ClientOutputBufferLimits& limits) {
        live_obuf_normal_hard_.store(limits.normal.hard_bytes, std::memory_order_relaxed);
        live_obuf_normal_soft_.store(limits.normal.soft_bytes, std::memory_order_relaxed);
        live_obuf_normal_seconds_.store(limits.normal.soft_seconds, std::memory_order_relaxed);
        live_obuf_pubsub_hard_.store(limits.pubsub.hard_bytes, std::memory_order_relaxed);
        live_obuf_pubsub_soft_.store(limits.pubsub.soft_bytes, std::memory_order_relaxed);
        live_obuf_pubsub_seconds_.store(limits.pubsub.soft_seconds, std::memory_order_relaxed);
    }

    void refresh_climon_armed() {
        uint32_t armed = 0;
        if (climon_monitors_.load(std::memory_order_relaxed)) armed |= kClimonMonitor;
        if (climon_tracking_.load(std::memory_order_relaxed)) armed |= kClimonTracking;
        if (climon_pause_end_ms_.load(std::memory_order_relaxed)) armed |= kClimonPause;
        if (climon_reply_.load(std::memory_order_relaxed)) armed |= kClimonReply;
        climon_armed_.store(armed, std::memory_order_release);
    }

    void refresh_client_cron_armed() {
        const bool normal = live_obuf_normal_hard_.load(std::memory_order_relaxed) != 0 ||
                            live_obuf_normal_soft_.load(std::memory_order_relaxed) != 0;
        const bool pubsub = live_obuf_pubsub_hard_.load(std::memory_order_relaxed) != 0 ||
                            live_obuf_pubsub_soft_.load(std::memory_order_relaxed) != 0;
        client_obuf_armed_.store(normal || pubsub, std::memory_order_release);
        client_cron_armed_.store(live_timeout_.load(std::memory_order_relaxed) != 0 ||
                                 normal || pubsub, std::memory_order_release);
    }

    // Only boot or the serialized CONFIG writer reads these mutable fields as a group. Readers
    // consume immutable, committed mailbox copies and never collect/revalidate these atomics.
    LiveConfigValues capture_live_config(uint64_t version) const {
        LiveConfigValues values;
        auto& snapshot = values.live;
        snapshot.version = version;
        snapshot.maxmemory = live_maxmemory_.load(std::memory_order_relaxed);
        snapshot.policy = static_cast<MaxmemoryPolicy>(
            live_maxmemory_policy_.load(std::memory_order_relaxed));
        snapshot.samples = live_maxmemory_samples_.load(std::memory_order_relaxed);
        snapshot.notify_events = live_notify_events_.load(std::memory_order_relaxed);
        snapshot.tracking_armed =
            (climon_armed_.load(std::memory_order_relaxed) & kClimonTracking) != 0;
        snapshot.slowlog_log_slower_than = live_slowlog_us_.load(std::memory_order_relaxed);
        snapshot.latency_monitor_threshold = live_latency_ms_.load(std::memory_order_relaxed);
        snapshot.save_armed = live_save_armed_.load(std::memory_order_relaxed);
        snapshot.proto_max_bulk_len = live_proto_max_bulk_len_.load(std::memory_order_relaxed);
        snapshot.debug_fanout_defer_us = debug_atomic_fanout_defer_.load(std::memory_order_relaxed);
        auto& clients = values.clients;
        clients.version = version;
        clients.timeout = live_timeout_.load(std::memory_order_relaxed);
        clients.normal.hard_bytes = live_obuf_normal_hard_.load(std::memory_order_relaxed);
        clients.normal.soft_bytes = live_obuf_normal_soft_.load(std::memory_order_relaxed);
        clients.normal.soft_seconds = live_obuf_normal_seconds_.load(std::memory_order_relaxed);
        clients.pubsub.hard_bytes = live_obuf_pubsub_hard_.load(std::memory_order_relaxed);
        clients.pubsub.soft_bytes = live_obuf_pubsub_soft_.load(std::memory_order_relaxed);
        clients.pubsub.soft_seconds = live_obuf_pubsub_seconds_.load(std::memory_order_relaxed);
        return values;
    }

    uint64_t begin_live_config_update() {
        uint64_t version = live_config_version_.load(std::memory_order_acquire);
        for (;;) {
            if (version & 1) {
                version = live_config_version_.load(std::memory_order_acquire);
                continue;
            }
            if (live_config_version_.compare_exchange_weak(
                    version, version + 1, std::memory_order_acq_rel, std::memory_order_acquire))
                return version + 1;
        }
    }
    void end_live_config_update(uint64_t write_version) {
        const LiveConfigValues next = capture_live_config(write_version + 1);
        for (uint32_t tid = 0; tid < nthreads(); tid++)
            live_config_mailboxes_[tid].publish(live_config_committed_, next);
        live_config_committed_ = next;
        live_config_version_.store(write_version + 1, std::memory_order_release);
    }

    Config    cfg_;
    Topology  topo_;
    Placement placement_;
    Router    router_;
    std::vector<std::unique_ptr<Shard>>     shards_;
    std::vector<std::unique_ptr<ThreadCtx>> threads_;
    AofManager aof_;
    // Declared after AOF so its destructor runs first and can detach an active rewrite callback.
    SnapshotManager snapshot_;
    FlipController flipctl_;

    // Router is authoritative at bucket granularity. This commit-only derivative exists solely so
    // shard-granularity dispatch remains one flat-array load.
    std::atomic<uint32_t> shard_owner_[256] = {};

    // FLIP is a cold, manually driven control-plane transaction.  The stage store is the global
    // dispatch barrier; acknowledgements are tagged with the transaction epoch so a late wakeup
    // from an earlier attempt cannot satisfy a later stage.
    std::mutex shape_transition_mu_;
    std::atomic<FlipStage> flip_stage_{FlipStage::Idle};
    std::atomic<uint64_t> flip_epoch_{0};
    std::atomic<uint64_t> flip_deadline_ns_{0};
    std::atomic<uint32_t> flip_target_io_{0};
    std::atomic<uint32_t> flip_target_ex_{0};
    std::atomic<uint64_t> flip_ack_[kMaxThreads] = {};
    Role flip_convert_[kMaxThreads] = {};
    uint32_t flip_surviving_io_[kMaxThreads] = {};
    uint32_t flip_surviving_io_count_ = 0;
    std::vector<Client*> flip_client_plan_[kMaxThreads];
    std::vector<uint32_t> flip_client_destinations_[kMaxThreads];
    uint32_t flip_client_quota_[kMaxThreads] = {};
    uint32_t flip_bucket_quota_[kMaxThreads] = {};
    uint32_t flip_shard_destination_[256] = {};
    std::atomic<uint32_t> flip_incoming_clients_[kMaxThreads] = {};
    std::atomic<uint32_t> flip_source_clients_[kMaxThreads] = {};
    uint32_t flip_planned_client_transfers_ = 0;
    uint32_t flip_planned_shard_transfers_ = 0;
    uint32_t flip_coordinator_ = UINT32_MAX;
    uint32_t unix_owner_tid_ = UINT32_MAX;
    std::atomic<bool> flip_failed_{false};
    mutable std::mutex flip_error_mu_;
    std::string flip_error_;
    std::atomic<uint64_t> flip_completed_{0};
    std::atomic<uint64_t> flip_refused_{0};
    std::atomic<uint64_t> flip_clients_transferred_{0};
    std::atomic<uint64_t> flip_active_transfers_{0};
    std::atomic<uint64_t> flip_last_transfers_{0};
    std::atomic<uint64_t> flip_bucket_weight_spread_before_{0}; // fixed point, /1024
    std::atomic<uint64_t> flip_bucket_weight_spread_after_{0};
    std::atomic<uint64_t> flip_client_weight_spread_before_{0};
    std::atomic<uint64_t> flip_client_weight_spread_after_{0};
    std::atomic<uint64_t> flip_bucket_bytes_spread_before_{0};
    std::atomic<uint64_t> flip_bucket_bytes_spread_after_{0};
    std::atomic<uint64_t> flip_conservation_checks_{0};
    std::atomic<uint64_t> flip_conservation_violations_{0};

    // The continuous controller has one cron writer and publishes only these two cold stages.
    // EX movement reuses FLIP's quiescence fence; client movement pauses one selected connection
    // until the existing asynchronous transfer primitive takes ownership.
    std::atomic<LbStage> lb_stage_{LbStage::Idle};
    std::atomic<uint64_t> lb_epoch_{0};
    std::atomic<uint64_t> lb_deadline_ns_{0};
    std::atomic<uint64_t> lb_ack_[kMaxThreads] = {};
    uint32_t lb_coordinator_ = UINT32_MAX;
    std::vector<LbShardMove> lb_shard_moves_;
    LbClientMove lb_client_move_;
    std::atomic<uint64_t> lb_client_inflight_id_{0};
    uint32_t lb_bucket_hot_streak_ = 0;
    uint32_t lb_client_hot_streak_ = 0;
    bool lb_prefer_client_ = false;
    std::atomic<uint64_t> lb_ticks_{0};
    std::atomic<uint64_t> lb_bucket_moves_{0};
    std::atomic<uint64_t> lb_client_moves_{0};
    std::atomic<uint64_t> lb_bucket_cross_domain_moves_{0};
    std::atomic<uint64_t> lb_client_cross_domain_moves_{0};
    std::atomic<uint64_t> lb_no_candidate_{0};
    std::atomic<uint64_t> lb_hysteresis_refused_{0};
    std::atomic<uint64_t> lb_cooldown_refused_{0};
    std::atomic<uint64_t> lb_transition_refused_{0};
    std::atomic<uint64_t> lb_capacity_refused_{0};
    std::atomic<uint64_t> lb_client_refused_{0};
    std::atomic<uint64_t> lb_hot_bucket_refused_{0};
    std::atomic<uint64_t> lb_bucket_weight_spread_current_{0};
    std::atomic<uint64_t> lb_bucket_weight_spread_before_{0};
    std::atomic<uint64_t> lb_bucket_weight_spread_after_{0};
    std::atomic<uint64_t> lb_client_weight_spread_current_{0};
    std::atomic<uint64_t> lb_client_weight_spread_before_{0};
    std::atomic<uint64_t> lb_client_weight_spread_after_{0};
    std::atomic<uint64_t> lb_bucket_bytes_spread_current_{0};
    std::atomic<uint64_t> lb_bucket_bytes_spread_before_{0};
    std::atomic<uint64_t> lb_bucket_bytes_spread_after_{0};

    // Weighted-placement state is absent when lb-sample-rate=0. Bucket arrays are indexed by the
    // immutable routing id; client state is keyed by the immutable connection id. The mutex is a
    // once-per-controller-beat/read-side lock and is never acquired on an operation path.
    mutable std::mutex lb_signal_mu_;
    std::vector<uint32_t> lb_bucket_last_samples_;
    std::vector<double> lb_bucket_weight_;
    std::vector<uint64_t> lb_bucket_last_move_ms_;
    std::vector<uint64_t> lb_thread_last_busy_;
    std::vector<uint64_t> lb_thread_last_idle_;
    std::vector<double> lb_thread_occupancy_;
    bool lb_bucket_primed_ = false;
    bool lb_occupancy_primed_ = false;
    std::unordered_map<uint64_t, LbClientSignal> lb_clients_;
    std::atomic<uint64_t> lb_client_owner_weight_[kMaxThreads] = {};
    std::atomic<uint32_t> loading_{0};

    uint8_t executor_slots_[kMaxThreads] = {};
    std::atomic<uint64_t> next_client_id_{1};
    std::atomic<bool>     shutting_down_{false};
    std::atomic<uint64_t> live_clients_{0};
    std::atomic<uint64_t> rejected_conns_{0};
    std::atomic<uint64_t> client_output_buffer_limit_disconnections_{0};
    std::atomic<uint64_t> blocked_clients_{0};
    std::atomic<uint64_t> blocking_waiters_{0};
    std::atomic<uint64_t> live_config_version_{0};
    std::atomic<uint64_t> live_maxmemory_{0};
    std::atomic<uint8_t>  live_maxmemory_policy_{
        static_cast<uint8_t>(MaxmemoryPolicy::NoEviction)};
    std::atomic<uint32_t> live_maxmemory_samples_{5};
    std::atomic<uint32_t> live_maxclients_{10000};
    std::atomic<uint32_t> live_timeout_{0};
    std::atomic<uint32_t> live_tcp_keepalive_{300};
    std::atomic<bool> client_cron_armed_{true};
    std::atomic<bool> client_obuf_armed_{true};
    std::atomic<uint64_t> live_obuf_normal_hard_{0};
    std::atomic<uint64_t> live_obuf_normal_soft_{0};
    std::atomic<uint32_t> live_obuf_normal_seconds_{0};
    std::atomic<uint64_t> live_obuf_pubsub_hard_{32ull * 1024 * 1024};
    std::atomic<uint64_t> live_obuf_pubsub_soft_{8ull * 1024 * 1024};
    std::atomic<uint32_t> live_obuf_pubsub_seconds_{60};
    std::atomic<uint32_t> live_notify_events_{0};
    std::atomic<bool> live_save_armed_{true};
    std::atomic<uint64_t> live_proto_max_bulk_len_{512ull * 1024 * 1024};
    // Lane F cold tail. Every field here is read once per io batch (or never, while the armed
    // word is zero); none of them is on a per-operation path.
    std::atomic<uint32_t> climon_armed_{0};
    std::atomic<uint64_t> climon_monitors_{0};
    std::atomic<uint64_t> climon_tracking_{0};
    std::atomic<uint64_t> climon_reply_{0};
    std::atomic<uint64_t> climon_pause_end_ms_{0};
    std::atomic<uint8_t>  climon_pause_mode_{kPauseAll};
    std::atomic<uint64_t> climon_monitor_io_mask_{0};
    std::atomic<uint64_t> climon_tracking_io_mask_{0};
    std::atomic<uint64_t> climon_monitor_lines_{0};
    std::atomic<uint64_t> climon_invalidations_{0};
    std::atomic<uint64_t> oob_frames_segmented_{0};
    std::atomic<uint64_t> oob_frames_deferred_{0};
    std::atomic<uint64_t> climon_pause_holds_{0};
    std::atomic<uint64_t> climon_no_touch_ops_{0};
    std::atomic<uint64_t> climon_tracking_keys_{0};
    std::atomic<uint64_t> climon_tracking_items_{0};
    std::atomic<uint64_t> climon_tracking_prefixes_{0};
    // Preserve the commit-watermark block's placement after folding window into atomic_credits_.
    uint32_t atomic_window_layout_padding_ = 0;
    // The drawn sequence and the visible read watermark share a line on purpose: readers used to
    // load commit_seq_ itself, so keeping the watermark beside it leaves reader traffic exactly
    // where it was, and a committer touches all three in one go.
    std::atomic<uint64_t> commit_seq_{0};
    std::atomic<uint64_t> atomic_commit_inflight_{0};
    std::atomic<uint64_t> atomic_commit_safe_{0};
    std::atomic<bool> snapshot_atomic_barrier_{false};
    AtomicApplySlot atomic_apply_slots_[kMaxThreads] = {};
    std::atomic<uint64_t> atomic_window_stalls_{0};
    std::atomic<uint32_t> debug_atomic_direct_defer_{0};
    std::atomic<uint32_t> debug_read_local_lane_cap_{0};   // 0 = derive (kInboxSlots)
    std::atomic<uint32_t> debug_hop_delay_{0};
    std::atomic<uint32_t> debug_atomic_read_delay_{0};
    std::atomic<uint32_t> debug_atomic_fanout_defer_{0};
    std::atomic<uint32_t> debug_script_stage_defer_{0};
    std::atomic<uint32_t> debug_barrier_hold_{0};
    std::atomic<bool> debug_blocking_timeout_reap_{false};
    std::atomic<uint64_t> debug_blocking_timeout_reaps_{0};
    std::atomic<uint64_t> barrier_owner_overlaps_{0};
    std::atomic<uint64_t> barrier_releases_held_{0};
    std::atomic<uint64_t> atomic_commit_windows_{0};
    std::atomic<uint64_t> atomic_commit_holds_{0};
    std::atomic<uint64_t> atomic_read_cuts_held_{0};
    std::atomic<uint64_t> atomic_fanout_cuts_{0};
    std::atomic<uint64_t> atomic_exec_read_cuts_{0};
    AtomicAdmissionCredits atomic_credits_;
    // Preserve every following cache-line offset formerly occupied by pool/debt/credit_ops.
    uint32_t atomic_credit_layout_padding_[3] = {};
    // Enabled is the high bit; every IO with admitted groups and every live record contributes
    // one low-bit unit. Exact group counts belong to atomic_credits_.
    // Dispatch therefore decides OFF/ON/draining with one acquire load and one predictable test.
    std::atomic<uint64_t> atomic_activity_{0};
    std::atomic<uint64_t> atomic_read_floors_[kMaxThreads] = {};
    std::atomic<uint64_t> atomic_snapshot_completions_[kMaxThreads] = {};
    // Cross-script instrumentation is a cold Server tail. It is touched only by the staged engine;
    // ordinary command dispatch reads none of these cache lines.
    std::atomic<uint64_t> script_stage_owner_tasks_{0};
    std::atomic<uint64_t> script_run_attempts_{0};
    std::atomic<uint64_t> script_validate_owner_tasks_{0};
    std::atomic<uint64_t> script_apply_owner_tasks_{0};
    std::atomic<uint64_t> script_crossshard_activations_{0};
    std::atomic<uint64_t> script_group_commits_{0};
    std::atomic<uint64_t> script_group_occ_retries_{0};
    std::atomic<uint64_t> script_group_occ_giveups_{0};
    std::atomic<uint64_t> script_staged_bytes_total_{0};
    std::atomic<uint64_t> script_crossshard_window_refusals_{0};
    std::atomic<uint64_t> sort_deref_lookups_{0};
    std::atomic<uint64_t> sort_deref_refusals_{0};
    std::atomic<uint64_t> sort_scatter_general_{0};
    std::atomic<uint64_t> sort_deref_escapes_{0};
    std::atomic<uint64_t> script_group_aborts_oom_{0};
    std::atomic<uint64_t> script_keys_armed_{0};
    std::atomic<uint64_t> script_keys_released_{0};
    std::atomic<uint64_t> script_write_tickets_forced_{0};
    std::atomic<uint64_t> script_intent_owners_{0};
    std::atomic<bool> script_certification_active_{false};
    std::atomic<uint64_t> pubsub_inflight_{0};
    std::atomic<uint64_t> pubsub_blobs_{0};
    std::atomic<uint64_t> pubsub_deliveries_{0};
    std::atomic<uint64_t> pubsub_delivery_batches_{0};
    std::atomic<uint64_t> pubsub_forwarded_stale_{0};
    std::atomic<uint64_t> pubsub_pending_{0};
    std::atomic<uint64_t> pubsub_home_entries_{0};
    std::atomic<uint64_t> pubsub_active_channels_{0};
    std::atomic<uint64_t> pubsub_subscriptions_{0};
    std::atomic<uint64_t> pubsub_pattern_subscriptions_{0};
    std::atomic<uint64_t> pubsub_shard_channels_{0};
    std::atomic<uint64_t> pubsub_shard_subscriptions_{0};
    std::atomic<uint64_t> tracking_forwarded_stale_{0};
    std::atomic<uint64_t> monitor_forwarded_stale_{0};
    // Security and DEBUG state is cold and appended after every pre-existing hot atomic so this
    // feature cannot reshuffle cache-line sharing in dispatch, atomic admission, or pub/sub.
    std::atomic<uint8_t> security_flags_{0};
    std::atomic<uint32_t> acl_kill_broadcasts_{0};
    std::atomic<bool> acl_active_desired_{false};
    std::atomic<bool> protected_mode_{true};
    std::atomic<bool> active_expire_enabled_{true};
    std::atomic<uint64_t> auth_failures_{0};
    std::atomic<uint64_t> acl_perm_retired_{0};
    std::atomic<uint64_t> acl_pubsub_clients_killed_{0};
    std::atomic<uint64_t> rejected_connections_{0};
    std::atomic<uint64_t> notify_events_fired_{0};
    std::atomic<uint64_t> notify_events_dropped_{0};
    // Cold observability for proving CLIENT catalog work reached every IO owner.
    std::atomic<uint64_t> client_scatter_requests_{0};
    std::atomic<uint64_t> client_scatter_io_responses_{0};
    // Slow-log arming, published through the same mailboxes as the eviction/notify knobs and read
    // once per executor pass. Appended at the true tail so no pre-existing offset moves.
    std::atomic<int64_t>  live_slowlog_us_{10000};
    std::atomic<uint32_t> live_latency_ms_{0};
    // Periodic snapshot policy is cold: one designated IO owner locks it once per second. Mutation
    // accounting remains per shard, so armed writes never contend on a process-global cache line.
    mutable std::mutex save_mu_;
    std::vector<SaveClause> save_clauses_;
    std::atomic<uint64_t> save_change_baseline_{0};
    std::atomic<uint64_t> scheduled_save_triggers_{0};
    std::atomic<uint64_t> save_cron_checks_{0};
    // Appended cold state: disabled servers allocate no epoch state and no established offset
    // moves. Later test-only knobs stay behind this pointer for the same reason.
    std::unique_ptr<ReadLocalServerState> read_local_state_;
    // Appended at the true tail: this test-only knob must not move any production member.
    std::atomic<uint64_t> debug_atomic_conditional_deadline_{0};
    // Fixed storage per physical worker, allocated before any worker starts in both modes. CONFIG
    // copies into this storage; arbitrarily many updates or a stopped reader cannot grow it.
    std::unique_ptr<LiveConfigMailbox[]> live_config_mailboxes_;
    LiveConfigValues live_config_committed_{}; // serialized CONFIG writer only
};

}  // namespace tomo

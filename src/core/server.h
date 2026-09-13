// server.h — process-wide shared state. Everything here is either immutable after boot or
// explicitly synchronised; nothing in it is written on the hot path.
//
// THAT IS THE RULE, and it is worth stating because it is easy to violate by accident. A counter
// bumped per command from a struct every thread shares is a shared-line write on the hot path, and
// at this thread count it shows up immediately. Per-command counters live in Shard::Stats or
// LoopSignals, both single-writer; INFO sums them on request.
//
// THE ONE MUTABLE HOT-PATH STRUCTURE is shard_owner_: shard id -> thread id, one atomic load per
// dispatch. That indirection is deliberate and it is what makes pointer-handoff load balancing
// possible later — an LB moves a shard by storing a different thread id, with no data movement and
// no change to routing. See placement.h for the ordering contract that makes such a move safe.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <climits>
#include <ctime>
#include <memory>
#include <mutex>
#include <sys/resource.h>
#include <vector>
#include "shard.h"
#include "thread.h"
#include "placement.h"
#include "config.h"        // struct Config: every runtime knob, one home
#include "../base/topology.h"
#include "../net/conn.h"   // kRobWindow: one source of truth for the window size
#include "../net/wb.h"
#include "../cmd/command.h"
#include "../snapshot/snapshot.h"
#include "../persist/aof.h"

namespace tomo {

struct LiveConfigSnapshot {
    uint64_t version;
    uint64_t maxmemory;
    MaxmemoryPolicy policy;
    uint32_t samples;
    uint32_t notify_events;
    // Lane F: CLIENT TRACKING arms the same executor-side write observer keyspace notifications
    // use.  It rides the existing live-config snapshot so ExLoop's per-pass shard mask refresh
    // stays one load, and so a shard never reads a second armed word per operation.
    bool     tracking_armed;
    // Appended at the tail: executors latch these once per pass through the same seqlock, so the
    // slow-log arming decision costs the pass nothing beyond the version compare it already made.
    int64_t  slowlog_log_slower_than;
    uint32_t latency_monitor_threshold;
    bool     save_armed;
    uint64_t proto_max_bulk_len;
};

struct ClientLimitsConfigSnapshot {
    uint64_t version = 0;
    uint32_t timeout = 0;
    ClientBufferLimit normal{};
    ClientBufferLimit pubsub{};
};

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
        live_atomic_window_.store(resolved_window, std::memory_order_relaxed);
        atomic_credit_pool_.store(resolved_window, std::memory_order_relaxed);
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
        if (cfg.place && cfg.node_cpus && *cfg.node_cpus) {
            std::fprintf(stderr, "fatal: --place and --node-cpus are mutually exclusive\n");
            return false;
        }
        if (cfg.node_cpus && *cfg.node_cpus) {
            if (!topo_.declare(cfg.node_cpus)) {
                std::fprintf(stderr, "fatal: --l3-domains '%s' invalid\n", cfg.node_cpus);
                return false;
            }
        } else {
            if (!topo_.discover()) {
                std::fprintf(stderr, "fatal: could not discover any allowed cpu\n");
                return false;
            }
        }
        // Default shape: an even io/ex split of every allowed cpu, io taking the odd one out --
        // the measured 2s center (4:4-class) generalized to any core count.
        uint32_t di = cfg.even_ifid, de = cfg.even_ex;
        if (!cfg.place && !(di | de)) {
            uint32_t n = 0;
            for (uint32_t d = 0; d < topo_.ndomains(); d++)
                n += static_cast<uint32_t>(topo_.cpus_in(d).size());
            di = n - n / 2; de = n / 2;
        }
        const bool placed = cfg.place
            ? placement_.build_explicit(topo_, cfg.place)
            : placement_.build_even(topo_, di, de);
        if (!placed) return false;
        if (placement_.ifid_threads().empty() || placement_.ex_threads().empty()) {
            std::fprintf(stderr, "placement needs at least one ifid and one ex thread\n");
            return false;
        }
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
            shards_[i]->bind_atomic_state(
                [](void* ctx) { return static_cast<Server*>(ctx)->atomic_commit(); }, this,
                &atomic_activity_, &script_intent_owners_);
        }
        router_.build_uniform(static_cast<int32_t>(cfg.shards));

        // ---- threads ---------------------------------------------------------------------------
        // Every front-end has already lowered to dense per-thread entries. Every thread still gets
        // a channel from every other regardless of role, because a role change must not require
        // re-wiring the mesh.
        const uint32_t nthreads = placement_.total_threads();
        for (uint32_t i = 0; i < kMaxThreads; i++) executor_slots_[i] = UINT8_MAX;
        for (uint32_t slot = 0; slot < placement_.ex_threads().size(); slot++)
            executor_slots_[placement_.ex_threads()[slot]] = static_cast<uint8_t>(slot);
        threads_.resize(nthreads);
        for (uint32_t i = 0; i < nthreads; i++) {
            threads_[i] = std::make_unique<ThreadCtx>();
            threads_[i]->init(i, placement_.role_of(i), nthreads);
            threads_[i]->init_command_counts(command_registry_size());
        }
        for (uint32_t i = 0; i < nthreads; i++)
            atomic_read_floors_[i].store(UINT64_MAX, std::memory_order_relaxed);
        for (uint32_t i = 0; i < nthreads; i++)
            atomic_snapshot_completions_[i].store(0, std::memory_order_relaxed);

        // ---- shards directly onto workers ------------------------------------------------------
        // Locality resolution stops at the individual EX thread. There is no contiguous node range:
        // the default is one flat round-robin across every executor in thread-id order.
        for (uint32_t sid = 0; sid < cfg.shards; sid++) {
            const uint32_t tid = placement_.shard_home(sid);
            threads_[tid]->shards().push_back(shards_[sid].get());
            // The reverse mapping is the ONE load on dispatch and the ONE release store used by a
            // future migration. Do not mirror ownership in another synchronised structure.
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
    const Topology&  topo()       const { return topo_; }
    Placement&       placement()        { return placement_; }
    Router&          router()           { return router_; }
    Shard&           shard(int32_t i)   { return *shards_[i]; }
    ThreadCtx&       thread(uint32_t i) { return *threads_[i]; }
    uint32_t         nthreads()   const { return static_cast<uint32_t>(threads_.size()); }
    uint32_t         nshards()    const { return static_cast<uint32_t>(shards_.size()); }
    SnapshotManager& snapshot()         { return snapshot_; }
    const SnapshotManager& snapshot() const { return snapshot_; }
    AofManager& aof() { return aof_; }
    const AofManager& aof() const { return aof_; }
    const ThreadCtx& thread(uint32_t i) const { return *threads_[i]; }

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

    // One atomic load on the dispatch path; one atomic store is how an LB moves work.
    uint32_t worker_of_shard(int32_t shard_id) const {
        return shard_owner_[shard_id].load(std::memory_order_acquire);
    }
    void set_worker_of_shard(int32_t shard_id, uint32_t thread_id) {
        shard_owner_[shard_id].store(thread_id, std::memory_order_release);
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

    ClientLimitsConfigSnapshot client_limits_snapshot() const {
        for (;;) {
            ClientLimitsConfigSnapshot out;
            out.version = live_config_version_.load(std::memory_order_acquire);
            if (out.version & 1) continue;
            out.timeout = live_timeout_.load(std::memory_order_relaxed);
            out.normal.hard_bytes = live_obuf_normal_hard_.load(std::memory_order_relaxed);
            out.normal.soft_bytes = live_obuf_normal_soft_.load(std::memory_order_relaxed);
            out.normal.soft_seconds = live_obuf_normal_seconds_.load(std::memory_order_relaxed);
            out.pubsub.hard_bytes = live_obuf_pubsub_hard_.load(std::memory_order_relaxed);
            out.pubsub.soft_bytes = live_obuf_pubsub_soft_.load(std::memory_order_relaxed);
            out.pubsub.soft_seconds = live_obuf_pubsub_seconds_.load(std::memory_order_relaxed);
            if (live_config_version_.load(std::memory_order_acquire) == out.version) return out;
        }
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

    // Tracking arms shard-side write observation, so it must publish through the live-config
    // seqlock the executors already poll (one version compare per pass when nothing changed).
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
        return live_atomic_window_.load(std::memory_order_acquire);
    }
    void set_atomic_enabled(bool enabled) {
        if (enabled) {
            atomic_reconfigure_credits(atomic_window());
            atomic_activity_.fetch_or(kAtomicEnabledBit, std::memory_order_release);
        } else {
            atomic_activity_.fetch_and(~kAtomicEnabledBit, std::memory_order_release);
            atomic_reconfigure_credits(atomic_window());
        }
    }
    void set_atomic_window(uint32_t window) {
        atomic_reconfigure_credits(window);
    }
    bool atomic_tracking_active() const {
        return atomic_mode_state() != 0;
    }
    bool atomic_can_admit(uint32_t owner_io, bool force = false) const {
        if (snapshot_atomic_barrier_.load(std::memory_order_acquire)) return false;
        if (!force && !(atomic_mode_state() & kAtomicEnabledBit)) return true;
        const uint64_t generation = atomic_credit_generation_.load(std::memory_order_acquire);
        if (generation & 1) return false;
        const uint32_t window = atomic_window();
        if (!window) return true;
        const AtomicAdmissionLease& lease = thread(owner_io).atomic_admission_lease();
        return (lease.generation == generation && lease.available != 0) ||
               atomic_credit_pool_.load(std::memory_order_acquire) != 0;
    }
    bool atomic_try_admit(uint32_t owner_io, uint64_t& admitted_generation,
                          bool force = false) {
        admitted_generation = 0;
        if (snapshot_atomic_barrier_.load(std::memory_order_acquire)) return false;
        if (!force && !(atomic_mode_state() & kAtomicEnabledBit)) return false;
        AtomicAdmissionLease& lease = thread(owner_io).atomic_admission_lease();
        const uint64_t generation = atomic_credit_generation_.load(std::memory_order_acquire);
        if (generation & 1) return false;
        const uint32_t window = atomic_window();
        if (lease.generation != generation) {
            lease.generation = generation;
            lease.available = 0; // the reconfiguration reset the global pool without old leases
        }
        if (window && lease.available == 0) {
            atomic_credit_ops_.fetch_add(1, std::memory_order_acq_rel);
            if (atomic_credit_generation_.load(std::memory_order_acquire) != generation) {
                atomic_credit_ops_.fetch_sub(1, std::memory_order_release);
                return false;
            }
            uint32_t available = atomic_credit_pool_.load(std::memory_order_relaxed);
            bool borrowed = false;
            while (available) {
                const uint32_t take = std::min<uint32_t>(available, kAtomicLeaseBatch);
                if (atomic_credit_pool_.compare_exchange_weak(
                        available, available - take, std::memory_order_acq_rel,
                        std::memory_order_relaxed)) {
                    lease.available = take;
                    borrowed = true;
                    break;
                }
            }
            atomic_credit_ops_.fetch_sub(1, std::memory_order_release);
            if (!borrowed) {
                atomic_window_stalls_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
        if (window) lease.available--;
        const bool first = lease.active++ == 0;
        lease.published_active.store(lease.active, std::memory_order_release);
        if (first) atomic_activity_.fetch_add(1, std::memory_order_release);
        if (snapshot_atomic_barrier_.load(std::memory_order_acquire) ||
            atomic_credit_generation_.load(std::memory_order_acquire) != generation ||
            (!force && !(atomic_mode_state() & kAtomicEnabledBit))) {
            lease.active--;
            lease.published_active.store(lease.active, std::memory_order_release);
            if (first) atomic_activity_.fetch_sub(1, std::memory_order_release);
            atomic_release_admission_credit(lease, generation);
            return false;
        }
        thread(owner_io).note_atomic_group();
        admitted_generation = generation;
        return true;
    }
    void set_snapshot_atomic_barrier(bool enabled) {
        snapshot_atomic_barrier_.store(enabled, std::memory_order_release);
    }
    void atomic_retire_group(uint32_t owner_io, uint64_t admitted_generation) {
        AtomicAdmissionLease& lease = thread(owner_io).atomic_admission_lease();
        if (!lease.active) std::abort();
        lease.active--;
        lease.published_active.store(lease.active, std::memory_order_release);

        atomic_release_admission_credit(lease, admitted_generation);

        if (lease.active == 0) {
            atomic_activity_.fetch_sub(1, std::memory_order_release);
            // An idle IO must not strand its lease while a hot peer is window-stalled. The config
            // generation plus credit_ops handshake makes returning this batch race-free.
            const uint64_t generation = atomic_credit_generation_.load(std::memory_order_acquire);
            if (atomic_window() && !(generation & 1) && lease.generation == generation &&
                lease.available) {
                const uint32_t returned = lease.available;
                atomic_credit_ops_.fetch_add(1, std::memory_order_acq_rel);
                if (atomic_credit_generation_.load(std::memory_order_acquire) == generation) {
                    atomic_credit_pool_.fetch_add(returned, std::memory_order_release);
                    lease.available = 0;
                }
                atomic_credit_ops_.fetch_sub(1, std::memory_order_release);
            }
        }
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
    uint64_t atomic_commit_reserve() {
        atomic_commit_inflight_.fetch_add(1, std::memory_order_seq_cst);
        return commit_seq_.fetch_add(1, std::memory_order_seq_cst) + 1;
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
    // The whole two-step for a group whose epoch word is `epoch`. The stall between the two is a
    // TEST HOOK (DEBUG ATOMIC-COMMIT-DELAY) that widens the closed window on demand; it is zero
    // in production and the load is on an already-cold once-per-group path. `publish_members`
    // lets a composite group install the SAME ticket into additional decision words before the
    // safe watermark moves. Readers therefore still see either all of the ticket or none of it.
    template <typename PublishMembers>
    uint64_t atomic_commit_group(std::atomic<uint64_t>& epoch,
                                 PublishMembers&& publish_members) {
        const uint64_t ticket = atomic_commit_reserve();
        const uint32_t stall = debug_atomic_commit_delay_.load(std::memory_order_relaxed);
        if (__builtin_expect(stall != 0, false)) debug_stall_us(stall);
        epoch.store(ticket, std::memory_order_release);
        publish_members(ticket);
        atomic_commit_publish();
        return ticket;
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
    uint64_t atomic_inflight() const {
        uint64_t inflight = 0;
        for (uint32_t io : placement_.ifid_threads())
            inflight += thread(io).atomic_admission_lease().published_active.load(
                std::memory_order_acquire);
        return inflight;
    }
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
        return atomic_apply_inflight_.load(std::memory_order_acquire);
    }
    void atomic_apply_open(std::atomic<bool>& flag) {
        flag.store(true, std::memory_order_release);
        atomic_apply_inflight_.fetch_add(1, std::memory_order_acq_rel);
    }
    // Idempotent by construction: whichever of the two ends reaches the group first closes it.
    void atomic_apply_close(std::atomic<bool>& flag) {
        if (__builtin_expect(!flag.load(std::memory_order_acquire), true)) return;
        if (flag.exchange(false, std::memory_order_acq_rel))
            atomic_apply_inflight_.fetch_sub(1, std::memory_order_acq_rel);
    }
    uint64_t atomic_window_stalls() const {
        return atomic_window_stalls_.load(std::memory_order_relaxed);
    }
    // TEST HOOK (DEBUG ATOMIC-DIRECT-DEFER). Number of extra owner passes a cross-shard RENAME's
    // destination task is held after its source hop is ready. Zero in production; its only reader
    // is xshard_prepare()'s already-cold direct-RENAME arm, so the disabled cost is nothing on any
    // other command. It widens -- deterministically -- the window in which a younger whole-owner
    // walker could overtake an older same-connection group on the destination shard.
    uint32_t debug_atomic_direct_defer() const {
        return debug_atomic_direct_defer_.load(std::memory_order_relaxed);
    }
    void set_debug_atomic_direct_defer(uint32_t passes) {
        debug_atomic_direct_defer_.store(passes, std::memory_order_relaxed);
    }
    // TEST HOOK (DEBUG ATOMIC-COMMIT-DELAY). Microseconds a group commit is held between drawing
    // its ticket and storing that ticket into the shared epoch word. Zero in production; read only
    // by atomic_commit_group(), a once-per-group cold path. It turns the reserve/publish hole into
    // a window wide enough for a reader to straddle, which is how the torn MGET is reproduced on
    // demand instead of once per 1.1M batches.
    void set_debug_atomic_commit_delay(uint32_t microseconds) {
        debug_atomic_commit_delay_.store(microseconds, std::memory_order_relaxed);
    }
    // TEST HOOK (DEBUG ATOMIC-FANOUT-DEFER). Microseconds every fragment of a cross-shard READ
    // except the one on its lead shard is PARKED -- re-queued, not spun -- after the command is
    // dispatched. That park is the fan-out window: the lead fragment answers from the world before
    // a transaction, the parked ones answer after its one ticket lands, and a reader with no pinned
    // cut then returns two generations in one reply. Parking rather than stalling is deliberate:
    // the executor stays free, so the transaction the test is racing can actually run and commit
    // inside the window. Zero in production; read once per cross-shard read at prepare time on the
    // already-cold scatter path, never on GET/SET.
    void set_debug_atomic_fanout_defer(uint32_t microseconds) {
        debug_atomic_fanout_defer_.store(microseconds, std::memory_order_relaxed);
    }
    uint32_t debug_atomic_fanout_defer() const {
        return debug_atomic_fanout_defer_.load(std::memory_order_relaxed);
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
    uint32_t debug_barrier_hold() const {
        return debug_barrier_hold_.load(std::memory_order_relaxed);
    }
    bool debug_barrier_hold_armed() const {
        return debug_barrier_hold_.load(std::memory_order_relaxed) != 0;
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
    uint32_t atomic_credit_pool() const {
        return atomic_credit_pool_.load(std::memory_order_acquire);
    }
    uint32_t atomic_credit_debt() const {
        return atomic_credit_debt_.load(std::memory_order_acquire);
    }

    LiveConfigSnapshot live_config_snapshot() const {
        // CONFIG writers make version odd around a change. Executors take this coherent snapshot
        // once per pass; no atomic reaches an individual operation or store lookup.
        for (;;) {
            LiveConfigSnapshot snapshot;
            snapshot.version = live_config_version_.load(std::memory_order_acquire);
            if (snapshot.version & 1) continue;
            snapshot.maxmemory = live_maxmemory_.load(std::memory_order_relaxed);
            snapshot.policy = static_cast<MaxmemoryPolicy>(
                live_maxmemory_policy_.load(std::memory_order_relaxed));
            snapshot.samples = live_maxmemory_samples_.load(std::memory_order_relaxed);
            snapshot.notify_events = live_notify_events_.load(std::memory_order_relaxed);
            snapshot.tracking_armed =
                (climon_armed_.load(std::memory_order_relaxed) & kClimonTracking) != 0;
            snapshot.slowlog_log_slower_than =
                live_slowlog_us_.load(std::memory_order_relaxed);
            snapshot.latency_monitor_threshold =
                live_latency_ms_.load(std::memory_order_relaxed);
            snapshot.save_armed = live_save_armed_.load(std::memory_order_relaxed);
            snapshot.proto_max_bulk_len =
                live_proto_max_bulk_len_.load(std::memory_order_relaxed);
            if (live_config_version_.load(std::memory_order_acquire) == snapshot.version)
                return snapshot;
        }
    }
    bool live_config_snapshot_if_changed(uint64_t known_version,
                                         LiveConfigSnapshot& snapshot) const {
        // The unchanged per-pass case is one acquire load. Field loads and the retry loop exist
        // only after CONFIG has published a different stable version.
        const uint64_t version = live_config_version_.load(std::memory_order_acquire);
        if (version == known_version) return false;
        snapshot = live_config_snapshot();
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
    uint64_t pubsub_blobs() const { return pubsub_blobs_.load(std::memory_order_relaxed); }
    uint64_t pubsub_deliveries() const {
        return pubsub_deliveries_.load(std::memory_order_relaxed);
    }
    uint64_t pubsub_delivery_batches() const {
        return pubsub_delivery_batches_.load(std::memory_order_relaxed);
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
    static constexpr uint32_t kAtomicLeaseBatch = 8;

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

    void atomic_release_admission_credit(AtomicAdmissionLease& lease,
                                         uint64_t admitted_generation) {
        uint64_t generation = atomic_credit_generation_.load(std::memory_order_acquire);
        while (generation & 1) {
            __builtin_ia32_pause();
            generation = atomic_credit_generation_.load(std::memory_order_acquire);
        }
        if (!atomic_window()) return;
        if (admitted_generation == generation && lease.generation == generation) {
            lease.available++;
            return;
        }
        if (admitted_generation == generation) return;
        uint32_t carry = lease.reconfig_carry.load(std::memory_order_relaxed);
        while (carry && !lease.reconfig_carry.compare_exchange_weak(
                              carry, carry - 1, std::memory_order_acq_rel,
                              std::memory_order_relaxed)) {}
        if (carry) atomic_return_reconfigured_credit(generation);
    }

    void atomic_return_reconfigured_credit(uint64_t generation) {
        atomic_credit_ops_.fetch_add(1, std::memory_order_acq_rel);
        if (atomic_credit_generation_.load(std::memory_order_acquire) == generation) {
            uint32_t debt = atomic_credit_debt_.load(std::memory_order_relaxed);
            while (debt && !atomic_credit_debt_.compare_exchange_weak(
                               debt, debt - 1, std::memory_order_acq_rel,
                               std::memory_order_relaxed)) {}
            if (!debt) atomic_credit_pool_.fetch_add(1, std::memory_order_release);
        }
        atomic_credit_ops_.fetch_sub(1, std::memory_order_release);
    }

    void atomic_reconfigure_credits(uint32_t window) {
        // Odd generations close admission while CONFIG takes an exact active-group snapshot.
        // Borrow/return operations announce themselves so the rebuilt pool cannot race a credit
        // mutation. IO-local available batches are intentionally discarded by the generation
        // change; only published active groups survive into the new accounting epoch.
        uint64_t generation = atomic_credit_generation_.load(std::memory_order_acquire);
        for (;;) {
            if (generation & 1) {
                generation = atomic_credit_generation_.load(std::memory_order_acquire);
                continue;
            }
            if (atomic_credit_generation_.compare_exchange_weak(
                    generation, generation + 1, std::memory_order_acq_rel,
                    std::memory_order_acquire))
                break;
        }
        while (atomic_credit_ops_.load(std::memory_order_acquire) != 0)
            __builtin_ia32_pause();

        uint64_t active = 0;
        for (uint32_t io : placement_.ifid_threads()) {
            AtomicAdmissionLease& lease = thread(io).atomic_admission_lease();
            const uint32_t live = lease.published_active.load(std::memory_order_acquire);
            lease.reconfig_carry.store(live, std::memory_order_release);
            active += live;
        }
        live_atomic_window_.store(window, std::memory_order_release);
        const uint64_t bounded = std::min<uint64_t>(active, UINT32_MAX);
        atomic_credit_pool_.store(
            window && bounded < window ? window - static_cast<uint32_t>(bounded) : 0,
            std::memory_order_release);
        atomic_credit_debt_.store(
            window && bounded > window ? static_cast<uint32_t>(bounded - window) : 0,
            std::memory_order_release);
        atomic_credit_generation_.store(generation + 2, std::memory_order_release);
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

    std::atomic<uint32_t> shard_owner_[256] = {};
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
    std::atomic<uint32_t> live_atomic_window_{256};
    // The drawn sequence and the visible read watermark share a line on purpose: readers used to
    // load commit_seq_ itself, so keeping the watermark beside it leaves reader traffic exactly
    // where it was, and a committer touches all three in one go.
    std::atomic<uint64_t> commit_seq_{0};
    std::atomic<uint64_t> atomic_commit_inflight_{0};
    std::atomic<uint64_t> atomic_commit_safe_{0};
    std::atomic<bool> snapshot_atomic_barrier_{false};
    std::atomic<uint64_t> atomic_apply_inflight_{0};
    std::atomic<uint64_t> atomic_window_stalls_{0};
    std::atomic<uint32_t> debug_atomic_direct_defer_{0};
    std::atomic<uint32_t> debug_atomic_commit_delay_{0};
    std::atomic<uint32_t> debug_atomic_read_delay_{0};
    std::atomic<uint32_t> debug_atomic_fanout_defer_{0};
    std::atomic<uint32_t> debug_script_stage_defer_{0};
    std::atomic<uint32_t> debug_barrier_hold_{0};
    std::atomic<uint64_t> barrier_owner_overlaps_{0};
    std::atomic<uint64_t> barrier_releases_held_{0};
    std::atomic<uint64_t> atomic_commit_windows_{0};
    std::atomic<uint64_t> atomic_commit_holds_{0};
    std::atomic<uint64_t> atomic_read_cuts_held_{0};
    std::atomic<uint64_t> atomic_fanout_cuts_{0};
    std::atomic<uint64_t> atomic_exec_read_cuts_{0};
    std::atomic<uint64_t> atomic_credit_generation_{2};
    std::atomic<uint32_t> atomic_credit_pool_{0};
    std::atomic<uint32_t> atomic_credit_debt_{0};
    std::atomic<uint32_t> atomic_credit_ops_{0};
    // Enabled is the high bit; every admitted group and live record contributes one low-bit unit.
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
    std::atomic<uint64_t> pubsub_pending_{0};
    std::atomic<uint64_t> pubsub_home_entries_{0};
    std::atomic<uint64_t> pubsub_active_channels_{0};
    std::atomic<uint64_t> pubsub_subscriptions_{0};
    std::atomic<uint64_t> pubsub_pattern_subscriptions_{0};
    std::atomic<uint64_t> pubsub_shard_channels_{0};
    std::atomic<uint64_t> pubsub_shard_subscriptions_{0};
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
    // Slow-log arming, published through the same seqlock as the eviction/notify knobs and read
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
};

}  // namespace tomo

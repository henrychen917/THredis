// lbsignals.cc — capture + derive + render for the LB signal read side. See src/core/lbsignals.h
// for the contract; this file is the one place cross-thread counter reads happen.
#include "../core/lbsignals.h"

#include <cinttypes>
#include <cstdarg>
#include <cstdio>

#include "../core/server.h"
#include "../core/shard.h"

namespace tomo {

// Every cross-thread counter read goes through this. The owners write these fields with plain
// stores (single-writer law); a relaxed atomic load is the same MOV on x86 and keeps the race
// defined. Monotonic uint64 counters make torn semantic values impossible to act on: any read is
// some value the owner actually published.
static inline uint64_t rd(const uint64_t& f) {
    return __atomic_load_n(&f, __ATOMIC_RELAXED);
}
static inline uint64_t clamp_age(uint64_t value) {
    return std::min(value, kLbAgeSaneMaxUs);
}
static inline double clamp_age(double value) {
    return std::min(value, static_cast<double>(kLbAgeSaneMaxUs));
}
LbSnapshot lbsignals_capture(Server& srv) {
    LbSnapshot snap;
    snap.stamp_ns = now_ns();
    snap.fused_mode = srv.thread_mode() == ThreadMode::Fused;
    const uint32_t nt = srv.nthreads();
    snap.threads.reserve(nt);
    for (uint32_t tid = 0; tid < nt; tid++) {
        ThreadCtx& t = srv.thread(tid);
        const Role role = t.role();
        if (role == Role::Idle) continue;
        const LoopSignals& s = t.sig();
        LbThreadRow r;
        r.tid = tid;
        r.role = role;
        r.domain = t.domain();
        // The owner maintains an atomic publication beside its private client vector. Capture must
        // never inspect the vector cross-thread: even a size-only read would race reallocation.
        const bool serves_clients = srv.serves_clients(role);
        const bool owns_shards = srv.owns_shards(role);
        r.clients = serves_clients ? t.client_count() : 0;
        snap.client_threads += serves_clients;
        r.iterations = rd(s.iterations);
        r.ops = rd(s.ops);
        r.busy_ns = rd(s.busy_ns);
        r.idle_ns = rd(s.idle_ns);
        r.cpu_ns = rd(s.cpu_ns);
        r.depth_sum = rd(s.depth_sum);
        r.depth_samples = rd(s.depth_samples);
        r.queue_delay_samples = rd(s.queue_delay_samples);
        r.queue_delay_ewma_us = clamp_age(
            static_cast<double>(rd(s.queue_delay_ewma_x256)) / 256.0);
        r.oldest_age_us = clamp_age(rd(s.oldest_age_us));
        r.oldest_age_samples = rd(s.oldest_age_samples);
        r.oldest_age_ewma_us = clamp_age(
            static_cast<double>(rd(s.oldest_age_ewma_x256)) / 256.0);
        r.oldest_age_min_us = clamp_age(rd(s.oldest_age_min_us));
        r.oldest_age_max_us = clamp_age(rd(s.oldest_age_max_us));
        r.full_events = rd(s.full_events);
        const MaskedQueueDiagnostics masked = t.task_inbox_diagnostics();
        r.masked_lane_high_water = masked.lane_high_water;
        r.masked_lane_full_events = masked.lane_full_events;
        r.masked_arena_occupancy_at_lane_full_sum =
            masked.arena_occupancy_at_lane_full_sum;
        r.masked_arena_capacity_at_lane_full_sum =
            masked.arena_capacity_at_lane_full_sum;
        r.wakes_sent = rd(s.wakes_sent);
        r.wakes_recv = rd(s.wakes_recv);
        r.spins = rd(s.spins);
        // In fused mode both capabilities are true and the one physical loop owns both bodies of
        // work. Never manufacture separate io/ex service costs from that combined counter set.
        LbRoleRollup& roll = serves_clients && owns_shards
            ? snap.fused : role == Role::Ifid ? snap.io : snap.ex;
        roll.threads++;
        roll.ops += r.ops;
        roll.busy_ns += r.busy_ns;
        roll.idle_ns += r.idle_ns;
        roll.cpu_ns += r.cpu_ns;
        roll.depth_sum += r.depth_sum;
        roll.depth_samples += r.depth_samples;
        roll.queue_delay_samples += r.queue_delay_samples;
        roll.queue_delay_ewma_weighted +=
            r.queue_delay_ewma_us * static_cast<double>(r.queue_delay_samples);
        roll.oldest_age_samples += r.oldest_age_samples;
        roll.oldest_age_ewma_weighted +=
            r.oldest_age_ewma_us * static_cast<double>(r.oldest_age_samples);
        if (r.oldest_age_samples) {
            if (roll.oldest_age_first) {
                roll.oldest_age_min_us = r.oldest_age_min_us;
                roll.oldest_age_max_us = r.oldest_age_max_us;
                roll.oldest_age_first = false;
            } else {
                roll.oldest_age_min_us = std::min(roll.oldest_age_min_us, r.oldest_age_min_us);
                roll.oldest_age_max_us = std::max(roll.oldest_age_max_us, r.oldest_age_max_us);
            }
        }
        roll.full_events += r.full_events;
        roll.masked_lane_high_water =
            std::max(roll.masked_lane_high_water, r.masked_lane_high_water);
        roll.masked_lane_full_events += r.masked_lane_full_events;
        roll.masked_arena_occupancy_at_lane_full_sum +=
            r.masked_arena_occupancy_at_lane_full_sum;
        roll.masked_arena_capacity_at_lane_full_sum +=
            r.masked_arena_capacity_at_lane_full_sum;
        snap.threads.push_back(r);
    }
    const uint32_t ns = srv.nshards();
    snap.shards.reserve(ns);
    bool owner_seen[kMaxThreads] = {};
    for (uint32_t sid = 0; sid < ns; sid++) {
        Shard& sh = srv.shard(static_cast<int32_t>(sid));
        const Shard::Stats& st = sh.stats();
        LbShardRow r;
        r.sid = sid;
        r.owner_tid = srv.worker_of_shard(static_cast<int32_t>(sid));
        if (r.owner_tid < nt && !owner_seen[r.owner_tid]) {
            owner_seen[r.owner_tid] = true;
            snap.owner_threads++;
        }
        // Thread domains are boot-latched. Derive the current owner's domain from the same atomic
        // owner row instead of racing the shard owner's mutable residency gauge.
        r.owner_domain = r.owner_tid < nt ? srv.thread(r.owner_tid).domain() : kNoDomain;
        r.ops = rd(st.ops);
        r.foreign_ops = rd(st.foreign_ops);
        r.migrations = rd(st.migrations);
        r.size = sh.published_size();
        r.obj_bytes = sh.published_obj_bytes();
        snap.shards.push_back(r);
    }
    return snap;
}

static void appendf_lb(std::string& out, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) out.append(buf, static_cast<size_t>(n) < sizeof(buf) ? static_cast<size_t>(n)
                                                                    : sizeof(buf) - 1);
}

static void format_rollup(std::string& out, const char* name, const LbRoleRollup& r) {
    appendf_lb(out,
               "rollup %s %u %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %.6f %.1f %.3f %" PRIu64
               " %" PRIu64 " %.3f %" PRIu64 " %" PRIu64 " %.3f"
               " masked_lane_high_water %u masked_lane_full_events %" PRIu64
               " masked_arena_occupancy_at_lane_full %.6f\n",
               name, r.threads, r.ops, r.busy_ns, r.idle_ns, r.cpu_ns, r.busy_frac(), r.ns_per_op(),
               r.avg_depth(), r.full_events, r.queue_delay_samples, r.queue_delay_ewma_us(),
               r.oldest_age_min_us, r.oldest_age_max_us, r.oldest_age_ewma_us(),
               r.masked_lane_high_water, r.masked_lane_full_events,
               r.masked_arena_occupancy_at_lane_full());
}

void lbsignals_format(const LbSnapshot& snap, std::string& out) {
    appendf_lb(out, "lbver 1 stamp_ns %" PRIu64 "\n", snap.stamp_ns);
    for (const auto& r : snap.threads) {
        const char* role = snap.fused_mode ? "fused" : r.role == Role::Ifid ? "io" : "ex";
        appendf_lb(out,
                   "thread %u %s %u %u %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
                   " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
                   " %" PRIu64 " %.3f %" PRIu64 " %" PRIu64 " %.3f %" PRIu64 " %" PRIu64
                   " masked_lane_high_water %u masked_lane_full_events %" PRIu64
                   " masked_arena_occupancy_at_lane_full %.6f\n",
                   r.tid, role, r.domain, r.clients, r.iterations,
                   r.ops, r.busy_ns, r.idle_ns, r.cpu_ns, r.depth_sum, r.depth_samples,
                   r.full_events, r.wakes_sent, r.wakes_recv, r.spins, r.queue_delay_samples,
                   r.queue_delay_ewma_us, r.oldest_age_us, r.oldest_age_samples,
                   r.oldest_age_ewma_us, r.oldest_age_min_us, r.oldest_age_max_us,
                   r.masked_lane_high_water, r.masked_lane_full_events,
                   r.masked_arena_occupancy_at_lane_full());
    }
    for (const auto& r : snap.shards)
        appendf_lb(out,
                   "shard %u %u %u %" PRIu64 " %" PRIu64 " %" PRIu64 " %u %" PRIu64 "\n",
                   r.sid, r.owner_tid, r.owner_domain, r.ops, r.foreign_ops, r.migrations, r.size,
                   r.obj_bytes);
    if (snap.fused_mode) {
        format_rollup(out, "fused", snap.fused);
    } else {
        format_rollup(out, "io", snap.io);
        format_rollup(out, "ex", snap.ex);
    }
    uint64_t sh_ops = 0, sh_foreign = 0;
    for (const auto& r : snap.shards) { sh_ops += r.ops; sh_foreign += r.foreign_ops; }
    const double foreign_frac = sh_ops
        ? static_cast<double>(sh_foreign) / static_cast<double>(sh_ops) : 0.0;
    if (snap.fused_mode) {
        appendf_lb(out,
                   "derived thread_mode 1s fused_threads %u client_threads %u "
                   "owner_threads %u foreign_frac %.6f\n",
                   snap.fused.threads, snap.client_threads, snap.owner_threads, foreign_frac);
        return;
    }
    const double fstar = snap.ratio_star_io_frac();
    const uint32_t total = snap.io.threads + snap.ex.threads;
    uint32_t nio = total ? static_cast<uint32_t>(fstar * total + 0.5) : 0;
    if (total) { if (nio == 0) nio = 1; if (nio >= total) nio = total - 1; }
    appendf_lb(out, "derived ratio_star_io_frac %.6f ratio_star_io %u ratio_star_ex %u "
                    "foreign_frac %.6f thread_mode 2s client_threads %u owner_threads %u\n",
               fstar, nio, total - nio, foreign_frac, snap.client_threads, snap.owner_threads);
}

void lbsignals_info_section(Server& srv, std::string& out) {
    const LbSnapshot snap = lbsignals_capture(srv);
    uint64_t sh_ops = 0, sh_foreign = 0;
    for (const auto& r : snap.shards) { sh_ops += r.ops; sh_foreign += r.foreign_ops; }
    const double foreign_frac = sh_ops
        ? static_cast<double>(sh_foreign) / static_cast<double>(sh_ops) : 0.0;
    if (snap.fused_mode) {
        double occupancy_min = 0, occupancy_max = 0;
        bool occupancy_first = true;
        for (const LbThreadRow& row : snap.threads) {
            const double occupancy = srv.lb_thread_occupancy(row.tid);
            if (occupancy_first) occupancy_min = occupancy_max = occupancy;
            else {
                occupancy_min = std::min(occupancy_min, occupancy);
                occupancy_max = std::max(occupancy_max, occupancy);
            }
            occupancy_first = false;
        }
        appendf_lb(out,
                   "# LB\r\n"
                   "lb_thread_mode:1s\r\n"
                   "lb_fused_threads:%u\r\nlb_client_threads:%u\r\nlb_owner_threads:%u\r\n"
                   "lb_fused_busy_frac:%.4f\r\nlb_fused_ns_per_op:%.1f\r\n"
                   "lb_fused_avg_depth:%.3f\r\nlb_fused_full_events:%" PRIu64 "\r\n"
                   "lb_total_threads:%u\r\nlb_foreign_op_frac:%.4f\r\n",
                   snap.fused.threads, snap.client_threads, snap.owner_threads,
                   snap.fused.busy_frac(), snap.fused.ns_per_op(), snap.fused.avg_depth(),
                   snap.fused.full_events, snap.fused.threads, foreign_frac);
        appendf_lb(out,
                   "lb_fused_masked_lane_high_water:%u\r\n"
                   "lb_fused_masked_lane_full_events:%" PRIu64 "\r\n"
                   "lb_fused_masked_arena_occupancy_at_lane_full:%.6f\r\n",
                   snap.fused.masked_lane_high_water, snap.fused.masked_lane_full_events,
                   snap.fused.masked_arena_occupancy_at_lane_full());
        appendf_lb(out,
                   "lb_fused_queue_delay_samples:%" PRIu64 "\r\n"
                   "lb_fused_queue_delay_ewma_us:%.3f\r\n"
                   "lb_fused_oldest_age_samples:%" PRIu64 "\r\n"
                   "lb_fused_oldest_age_min_us:%" PRIu64 "\r\n"
                   "lb_fused_oldest_age_max_us:%" PRIu64 "\r\n"
                   "lb_fused_oldest_age_ewma_us:%.3f\r\n",
                   snap.fused.queue_delay_samples, snap.fused.queue_delay_ewma_us(),
                   snap.fused.oldest_age_samples, snap.fused.oldest_age_min_us,
                   snap.fused.oldest_age_max_us, snap.fused.oldest_age_ewma_us());
        appendf_lb(out,
                   "lb_fused_occupancy_min:%.4f\r\nlb_fused_occupancy_max:%.4f\r\n",
                   occupancy_min, occupancy_max);
    } else {
        const uint32_t total = snap.io.threads + snap.ex.threads;
        double io_occ_min = 0, io_occ_max = 0, ex_occ_min = 0, ex_occ_max = 0;
        bool io_occ_first = true, ex_occ_first = true;
        for (const LbThreadRow& row : snap.threads) {
            const double occupancy = srv.lb_thread_occupancy(row.tid);
            double& lo = row.role == Role::Ifid ? io_occ_min : ex_occ_min;
            double& hi = row.role == Role::Ifid ? io_occ_max : ex_occ_max;
            bool& first = row.role == Role::Ifid ? io_occ_first : ex_occ_first;
            if (first) lo = hi = occupancy;
            else { lo = std::min(lo, occupancy); hi = std::max(hi, occupancy); }
            first = false;
        }
        appendf_lb(out,
                   "# LB\r\n"
                   "lb_io_threads:%u\r\nlb_ex_threads:%u\r\n"
                   "lb_io_busy_frac:%.4f\r\nlb_ex_busy_frac:%.4f\r\n"
                   "lb_io_ns_per_op:%.1f\r\nlb_ex_ns_per_op:%.1f\r\n"
                   "lb_io_avg_depth:%.3f\r\nlb_ex_avg_depth:%.3f\r\n"
                   "lb_io_full_events:%" PRIu64 "\r\nlb_ex_full_events:%" PRIu64 "\r\n"
                   "lb_ratio_star_io_frac:%.4f\r\nlb_total_threads:%u\r\n"
                   "lb_foreign_op_frac:%.4f\r\n",
                   snap.io.threads, snap.ex.threads, snap.io.busy_frac(), snap.ex.busy_frac(),
                   snap.io.ns_per_op(), snap.ex.ns_per_op(), snap.io.avg_depth(), snap.ex.avg_depth(),
                   snap.io.full_events, snap.ex.full_events, snap.ratio_star_io_frac(), total,
                   foreign_frac);
        appendf_lb(out,
                   "lb_io_masked_lane_high_water:%u\r\n"
                   "lb_ex_masked_lane_high_water:%u\r\n"
                   "lb_io_masked_lane_full_events:%" PRIu64 "\r\n"
                   "lb_ex_masked_lane_full_events:%" PRIu64 "\r\n"
                   "lb_io_masked_arena_occupancy_at_lane_full:%.6f\r\n"
                   "lb_ex_masked_arena_occupancy_at_lane_full:%.6f\r\n",
                   snap.io.masked_lane_high_water, snap.ex.masked_lane_high_water,
                   snap.io.masked_lane_full_events, snap.ex.masked_lane_full_events,
                   snap.io.masked_arena_occupancy_at_lane_full(),
                   snap.ex.masked_arena_occupancy_at_lane_full());
        appendf_lb(out,
                   "lb_io_queue_delay_samples:%" PRIu64 "\r\n"
                   "lb_ex_queue_delay_samples:%" PRIu64 "\r\n"
                   "lb_io_queue_delay_ewma_us:%.3f\r\nlb_ex_queue_delay_ewma_us:%.3f\r\n"
                   "lb_io_oldest_age_samples:%" PRIu64 "\r\n"
                   "lb_ex_oldest_age_samples:%" PRIu64 "\r\n"
                   "lb_io_oldest_age_min_us:%" PRIu64 "\r\n"
                   "lb_io_oldest_age_max_us:%" PRIu64 "\r\n"
                   "lb_io_oldest_age_ewma_us:%.3f\r\n"
                   "lb_ex_oldest_age_min_us:%" PRIu64 "\r\n"
                   "lb_ex_oldest_age_max_us:%" PRIu64 "\r\n"
                   "lb_ex_oldest_age_ewma_us:%.3f\r\n",
                   snap.io.queue_delay_samples, snap.ex.queue_delay_samples,
                   snap.io.queue_delay_ewma_us(), snap.ex.queue_delay_ewma_us(),
                   snap.io.oldest_age_samples, snap.ex.oldest_age_samples,
                   snap.io.oldest_age_min_us, snap.io.oldest_age_max_us,
                   snap.io.oldest_age_ewma_us(), snap.ex.oldest_age_min_us,
                   snap.ex.oldest_age_max_us, snap.ex.oldest_age_ewma_us());
        appendf_lb(out,
                   "lb_io_occupancy_min:%.4f\r\nlb_io_occupancy_max:%.4f\r\n"
                   "lb_ex_occupancy_min:%.4f\r\nlb_ex_occupancy_max:%.4f\r\n",
                   io_occ_min, io_occ_max, ex_occ_min, ex_occ_max);
    }
    appendf_lb(out,
               "lb_flip_bucket_weight_spread_before:%.3f\r\n"
               "lb_flip_bucket_weight_spread_after:%.3f\r\n"
               "lb_flip_client_weight_spread_before:%.3f\r\n"
               "lb_flip_client_weight_spread_after:%.3f\r\n"
               "lb_flip_bucket_bytes_spread_before:%" PRIu64 "\r\n"
               "lb_flip_bucket_bytes_spread_after:%" PRIu64 "\r\n",
               srv.flip_bucket_weight_spread_before(), srv.flip_bucket_weight_spread_after(),
               srv.flip_client_weight_spread_before(), srv.flip_client_weight_spread_after(),
               srv.flip_bucket_bytes_spread_before(), srv.flip_bucket_bytes_spread_after());
    appendf_lb(out,
               "tomokv_keylb_enabled:%u\r\n"
               "tomokv_clientlb_enabled:%u\r\n"
               "tomokv_keylb_stage:%u\r\n"
               "tomokv_keylb_ticks:%" PRIu64 "\r\n"
               "tomokv_keylb_bucket_moves:%" PRIu64 "\r\n"
               "tomokv_keylb_client_moves:%" PRIu64 "\r\n"
               "tomokv_keylb_bucket_cross_domain_moves:%" PRIu64 "\r\n"
               "tomokv_keylb_client_cross_domain_moves:%" PRIu64 "\r\n"
               "tomokv_keylb_no_candidate:%" PRIu64 "\r\n",
               srv.key_lb_signals_enabled() ? 1u : 0u,
               srv.client_lb_signals_enabled() ? 1u : 0u,
               static_cast<unsigned>(srv.lb_stage()),
               srv.lb_ticks(), srv.lb_bucket_moves(), srv.lb_client_moves(),
               srv.lb_bucket_cross_domain_moves(), srv.lb_client_cross_domain_moves(),
               srv.lb_no_candidate());
    appendf_lb(out,
               "tomokv_keylb_hysteresis_refused:%" PRIu64 "\r\n"
               "tomokv_keylb_cooldown_refused:%" PRIu64 "\r\n"
               "tomokv_keylb_transition_refused:%" PRIu64 "\r\n"
               "tomokv_keylb_capacity_refused:%" PRIu64 "\r\n"
               "tomokv_keylb_client_refused:%" PRIu64 "\r\n"
               "tomokv_keylb_hot_bucket_refused:%" PRIu64 "\r\n",
               srv.lb_hysteresis_refused(), srv.lb_cooldown_refused(),
               srv.lb_transition_refused(), srv.lb_capacity_refused(), srv.lb_client_refused(),
               srv.lb_hot_bucket_refused());
    appendf_lb(out,
               "tomokv_keylb_bucket_weight_spread_current:%.3f\r\n"
               "tomokv_keylb_bucket_weight_spread_before:%.3f\r\n"
               "tomokv_keylb_bucket_weight_spread_after:%.3f\r\n"
               "tomokv_keylb_client_weight_spread_current:%.3f\r\n"
               "tomokv_keylb_client_weight_spread_before:%.3f\r\n"
               "tomokv_keylb_client_weight_spread_after:%.3f\r\n",
               srv.lb_bucket_weight_spread_current(),
               srv.lb_bucket_weight_spread_before(), srv.lb_bucket_weight_spread_after(),
               srv.lb_client_weight_spread_current(), srv.lb_client_weight_spread_before(),
               srv.lb_client_weight_spread_after());
    appendf_lb(out,
               "tomokv_keylb_bucket_bytes_spread_current:%" PRIu64 "\r\n"
               "tomokv_keylb_bucket_bytes_spread_before:%" PRIu64 "\r\n"
               "tomokv_keylb_bucket_bytes_spread_after:%" PRIu64 "\r\n\r\n",
               srv.lb_bucket_bytes_spread_current(),
               srv.lb_bucket_bytes_spread_before(), srv.lb_bucket_bytes_spread_after());
}

}  // namespace tomo

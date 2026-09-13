// lbsignals.h — the READ side of the signal system: capture, derive, export.
//
// signal.h already makes every loop report LoopSignals in one set of units (ops, nanoseconds,
// entries) and every shard report locality (foreign_ops). What was missing is the consumer: a
// coherent capture of all of it, and the derived quantities a balancer actually steers on. This
// file is that consumer, and it is the ONLY one — the future LB controller, INFO's # LB section
// and DEBUG LBSIGNALS all read through the same capture/derive path, so the number the operator
// sees is the number the controller acts on. The fork's balancer defects all started as private
// derivations that drifted from each other.
//
// COST: zero on the hot path. Everything here runs on the cold command path (DEBUG/INFO) or in a
// controller beat. Writers keep their plain single-owner stores; capture reads each counter with
// a relaxed __atomic_load_n, which compiles to the same MOV on x86 but keeps the cross-thread
// read defined. Counters are MONOTONIC — rates are the reader's job (two captures, subtract);
// the server keeps no window state, so any number of readers can watch at their own cadence
// without stepping on each other. That statelessness is itself a signal-quality feature: the
// fork's sampled-cputime hack had one global window that every consumer fought over.
//
// RATIO_STAR — the headline derivation. Because busy time is time-weighted per role and ops flow
// through both roles, the per-op service cost of each role is directly measurable:
//     c_io = io_busy_ns / io_ops        c_ex = ex_busy_ns / ex_ops
// Work conservation then gives the throughput-optimal split of N threads:
//     n_io* = N * c_io / (c_io + c_ex)
// This is the "best ratio" a work-bound regime can justify from first principles. It is exact
// for the saturated pipelined regime; the p1 regime is width/latency-bound (p1 = send_threads x
// rate law), so ratio_star deliberately reports the work-bound optimum and the signal-quality
// study quantifies each estimator's distance from the empirically best ratio per regime.
// Queue delay and oldest-age may TRIGGER a controller investigation, but they do not judge a
// placement: only measured throughput after the move can do that. Latency signals show pressure,
// not whether a different owner split will process more work.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "signal.h"
#include "thread.h"

namespace tomo {

class Server;

struct LbThreadRow {
    uint32_t tid = 0;
    Role     role = Role::Idle;
    uint32_t domain = 0;
    uint32_t clients = 0;          // Ifid only: connections owned
    uint64_t iterations = 0;
    uint64_t ops = 0;
    uint64_t busy_ns = 0;
    uint64_t idle_ns = 0;
    uint64_t cpu_ns = 0;           // CLOCK_THREAD_CPUTIME_ID — the DEFER_TASKRUN-proof reading
    uint64_t depth_sum = 0;
    uint64_t depth_samples = 0;
    uint64_t queue_delay_samples = 0;
    double queue_delay_ewma_us = 0.0;
    uint64_t oldest_age_us = 0;
    uint64_t oldest_age_samples = 0;
    double oldest_age_ewma_us = 0.0;
    uint64_t oldest_age_min_us = 0;
    uint64_t oldest_age_max_us = 0;
    uint64_t full_events = 0;
    uint32_t masked_lane_high_water = 0;
    uint64_t masked_lane_full_events = 0;
    uint64_t masked_arena_occupancy_at_lane_full_sum = 0;
    uint64_t masked_arena_capacity_at_lane_full_sum = 0;
    uint64_t wakes_sent = 0;
    uint64_t wakes_recv = 0;
    uint64_t spins = 0;
    double masked_arena_occupancy_at_lane_full() const {
        return masked_arena_capacity_at_lane_full_sum
            ? static_cast<double>(masked_arena_occupancy_at_lane_full_sum) /
                  static_cast<double>(masked_arena_capacity_at_lane_full_sum)
            : 0.0;
    }
};

struct LbShardRow {
    uint32_t sid = 0;
    uint32_t owner_tid = 0;
    uint32_t owner_domain = 0;
    uint64_t ops = 0;
    uint64_t foreign_ops = 0;      // executed by a worker outside the shard's home L3 domain
    uint64_t migrations = 0;
    uint32_t size = 0;
    uint64_t obj_bytes = 0;
};

struct LbRoleRollup {
    uint32_t threads = 0;
    uint64_t ops = 0;
    uint64_t busy_ns = 0;
    uint64_t idle_ns = 0;
    uint64_t cpu_ns = 0;
    uint64_t depth_sum = 0;
    uint64_t depth_samples = 0;
    uint64_t full_events = 0;
    uint32_t masked_lane_high_water = 0;
    uint64_t masked_lane_full_events = 0;
    uint64_t masked_arena_occupancy_at_lane_full_sum = 0;
    uint64_t masked_arena_capacity_at_lane_full_sum = 0;
    uint64_t queue_delay_samples = 0;
    double queue_delay_ewma_weighted = 0.0;
    uint64_t oldest_age_samples = 0;
    double oldest_age_ewma_weighted = 0.0;
    uint64_t oldest_age_min_us = 0;
    uint64_t oldest_age_max_us = 0;
    bool oldest_age_first = true;
    double busy_frac() const {
        const uint64_t t = busy_ns + idle_ns;
        return t ? static_cast<double>(busy_ns) / static_cast<double>(t) : 0.0;
    }
    double ns_per_op() const {
        return ops ? static_cast<double>(busy_ns) / static_cast<double>(ops) : 0.0;
    }
    double avg_depth() const {
        return depth_samples ? static_cast<double>(depth_sum) / static_cast<double>(depth_samples)
                             : 0.0;
    }
    double queue_delay_ewma_us() const {
        return queue_delay_samples ? queue_delay_ewma_weighted / queue_delay_samples : 0.0;
    }
    double oldest_age_ewma_us() const {
        return oldest_age_samples ? oldest_age_ewma_weighted / oldest_age_samples : 0.0;
    }
    double masked_arena_occupancy_at_lane_full() const {
        return masked_arena_capacity_at_lane_full_sum
            ? static_cast<double>(masked_arena_occupancy_at_lane_full_sum) /
                  static_cast<double>(masked_arena_capacity_at_lane_full_sum)
            : 0.0;
    }
};

struct LbSnapshot {
    uint64_t stamp_ns = 0;                 // CLOCK_MONOTONIC at capture
    bool fused_mode = false;
    uint32_t client_threads = 0;            // threads capable of owning connections
    uint32_t owner_threads = 0;             // distinct tids named by shard rows
    std::vector<LbThreadRow> threads;
    std::vector<LbShardRow>  shards;
    LbRoleRollup io;                       // Role::Ifid rollup
    LbRoleRollup ex;                       // Role::Ex rollup
    LbRoleRollup fused;                    // combined 1s loop work; never a synthetic split

    // Work-conservation optimum for the current total thread count; 0 threads / 0 ops degrade to
    // an even split rather than a division fault so an idle server still answers.
    double ratio_star_io_frac() const {
        const double cio = io.ns_per_op(), cex = ex.ns_per_op();
        const double s = cio + cex;
        return s > 0.0 ? cio / s : 0.5;
    }
};

// Capture is declared here and defined in src/cmd/lbsignals.cc (it needs the full Server type).
LbSnapshot lbsignals_capture(Server& srv);

// Text renderers. Format is line-oriented, space-separated columns after a row tag — built for
// the study harness and future tooling to parse without a JSON dependency:
//   lbver 1 stamp_ns <ns>
//   thread <tid> <io|ex|fused> <domain> <clients> <iterations> <ops> <busy_ns> <idle_ns> <cpu_ns>
//          <depth_sum> <depth_samples> <full_events> <wakes_sent> <wakes_recv> <spins>
//          <queue_delay_samples> <queue_delay_ewma_us> <oldest_age_us>
//          <oldest_age_samples> <oldest_age_ewma_us> <oldest_age_min_us> <oldest_age_max_us>
//          masked_lane_high_water <n> masked_lane_full_events <n>
//          masked_arena_occupancy_at_lane_full <fraction>
//   shard <sid> <owner_tid> <owner_domain> <ops> <foreign_ops> <migrations> <size> <obj_bytes>
//   rollup <io|ex|fused> <threads> <ops> <busy_ns> <idle_ns> <cpu_ns> <busy_frac> <ns_per_op>
//          <avg_depth> <full_events> <queue_delay_samples> <queue_delay_ewma_us>
//          <oldest_age_min_us> <oldest_age_max_us> <oldest_age_ewma_us>
//          masked_lane_high_water <n> masked_lane_full_events <n>
//          masked_arena_occupancy_at_lane_full <fraction>
// In 2s the derived row retains ratio_star_io_frac/ratio_star_io/ratio_star_ex. In 1s there is no
// role split to optimize, so it reports thread_mode/fused_threads instead. Both forms include the
// actual distinct shard-owner and client-serving thread counts.
void lbsignals_format(const LbSnapshot& snap, std::string& out);
// The short derived block for INFO's # LB section.
void lbsignals_info_section(Server& srv, std::string& out);

}  // namespace tomo

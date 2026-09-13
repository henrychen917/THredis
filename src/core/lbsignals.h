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
    uint64_t full_events = 0;
    uint64_t wakes_sent = 0;
    uint64_t wakes_recv = 0;
    uint64_t spins = 0;
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
};

struct LbSnapshot {
    uint64_t stamp_ns = 0;                 // CLOCK_MONOTONIC at capture
    std::vector<LbThreadRow> threads;
    std::vector<LbShardRow>  shards;
    LbRoleRollup io;                       // Role::Ifid rollup
    LbRoleRollup ex;                       // Role::Ex rollup

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
//   thread <tid> <io|ex> <domain> <clients> <iterations> <ops> <busy_ns> <idle_ns> <cpu_ns>
//          <depth_sum> <depth_samples> <full_events> <wakes_sent> <wakes_recv> <spins>
//   shard <sid> <owner_tid> <owner_domain> <ops> <foreign_ops> <migrations> <size> <obj_bytes>
//   rollup <io|ex> <threads> <ops> <busy_ns> <idle_ns> <cpu_ns> <busy_frac> <ns_per_op> <avg_depth> <full_events>
//   derived ratio_star_io_frac <f> ratio_star_io <n> ratio_star_ex <n> foreign_frac <f>
void lbsignals_format(const LbSnapshot& snap, std::string& out);
// The short derived block for INFO's # LB section.
void lbsignals_info_section(Server& srv, std::string& out);

}  // namespace tomo

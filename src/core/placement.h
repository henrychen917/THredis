// placement.h — per-thread role/cpu placement and the shard migration contract.
//
// A LOGICAL THREAD IS THE DEFAULT LOCALITY UNIT. When the allowed CPU set contains sibling pairs,
// Linux's reported pair is the scheduling unit: both dense thread ids have one role and FLIP moves them
// together. There is deliberately no node layer between a thread and its cpu/L3 facts.

#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>
#include "shard.h"
#include "thread.h"
#include "../base/topology.h"

namespace tomo {

inline constexpr uint32_t kNoThread = UINT32_MAX;

struct ThreadPlacement {
    uint32_t id          = 0;
    Role     role        = Role::Idle;
    int      cpu         = -1;
    uint32_t domain      = kNoDomain;
};

class Placement {
public:
    // FUSED PLACEMENT. Every selected CPU owns clients and may own shards. The two dense views
    // intentionally contain the same tids: existing connection fanout consumes ifid_threads(),
    // while shard/snapshot fanout consumes ex_threads(). Explicit role labels remain accepted as
    // CPU selectors, but have no runtime meaning in this mode.
    bool build_fused(const Topology& topo, const char* spec) {
        clear();
        if (spec) {
            if (!build_explicit(topo, spec)) return false;
        } else {
            for (uint32_t domain = 0; domain < topo.ndomains(); domain++)
                for (int cpu : topo.cpus_in(domain)) append(Role::Ifid, cpu, domain);
        }
        if (threads_.empty()) {
            std::fprintf(stderr, "1s placement needs at least one thread\n");
            return false;
        }
        if (threads_.size() > kMaxThreads) {
            std::fprintf(stderr,
                         "1s placement has %zu threads, exceeding the %u-thread channel limit\n",
                         threads_.size(), kMaxThreads);
            return false;
        }
        ifid_.clear();
        ex_.clear();
        for (uint32_t tid = 0; tid < threads_.size(); tid++) {
            threads_[tid].role = Role::Ifid;
            ifid_.push_back(tid);
            ex_.push_back(tid);
        }
        return true;
    }

    // EVEN GLOBAL PLACEMENT. Counts are whole-server, so shapes no per-node grammar can express
    // (15:2:15) are first-class. Each role is spread across the L3 domains as evenly as integer
    // division allows and roles are interleaved within a domain, so sender pairing stays local.
    //
    // This is also the flip-era invariant in batch form: a runtime controller changing one thread's
    // role keeps these same quotas incrementally -- pick the convert candidate from the domain
    // whose count for the shrinking role sits above quota and whose count for the growing role
    // sits below it. Placement stays a pure function of (counts, topology); no node layer.
    bool build_even(const Topology& topo, uint32_t n_ifid, uint32_t n_ex,
                    bool smt_mode = false) {
        clear();
        if (smt_mode) return build_even_smt(topo, n_ifid, n_ex);
        const uint32_t nd = topo.ndomains() ? topo.ndomains() : 1;
        const uint64_t total = static_cast<uint64_t>(n_ifid) + n_ex;
        uint64_t cap = 0;
        for (uint32_t d = 0; d < nd; d++) cap += topo.cpus_in(d).size();
        if (total > cap) {
            std::fprintf(stderr, "--ratio: %llu threads but only %llu allowed cpus\n",
                         static_cast<unsigned long long>(total),
                         static_cast<unsigned long long>(cap));
            return false;
        }
        if (total > kMaxThreads) {
            std::fprintf(stderr, "--ratio: %llu threads exceeds the %u-thread channel limit\n",
                         static_cast<unsigned long long>(total), kMaxThreads);
            return false;
        }
        std::vector<uint32_t> qi(nd), qe(nd);
        auto spread = [nd](uint32_t count, std::vector<uint32_t>& q) {
            for (uint32_t d = 0; d < nd; d++) q[d] = count / nd + (d < count % nd ? 1u : 0u);
        };
        spread(n_ifid, qi); spread(n_ex, qe);
        // Capacity fix-up for domains of unequal size (declared topologies): shed ex first -- it is
        // the most placement-indifferent role -- into the domain with the most free cpus.
        for (uint32_t d = 0; d < nd; d++) {
            while (qi[d] + qe[d] > topo.cpus_in(d).size()) {
                uint32_t tgt = kNoDomain;
                uint64_t best_room = 0;
                for (uint32_t o = 0; o < nd; o++) {
                    if (o == d) continue;
                    const uint64_t used = qi[o] + qe[o];
                    const uint64_t have = topo.cpus_in(o).size();
                    if (have > used && have - used > best_room) { best_room = have - used; tgt = o; }
                }
                if (tgt == kNoDomain) {
                    std::fprintf(stderr, "--ratio: no domain has room to rebalance\n");
                    return false;
                }
                if (qe[d]) { qe[d]--; qe[tgt]++; }
                else       { qi[d]--; qi[tgt]++; }
            }
        }
        for (uint32_t d = 0; d < nd; d++) {
            const std::vector<int>& cpus = topo.cpus_in(d);
            uint32_t slot = 0;
            for (uint32_t k = 0; k < qi[d]; k++) append(Role::Ifid, cpus[slot++], d);
            for (uint32_t k = 0; k < qe[d]; k++) append(Role::Ex,   cpus[slot++], d);
        }
        return true;
    }

    // --place is intentionally a small, strict language. Boot configuration should fail at the
    // first typo instead of accepting a partial placement and letting unpinned threads float.
    bool build_explicit(const Topology& topo, const char* spec) {
        clear();
        if (!spec || !*spec) {
            std::fprintf(stderr, "--place must contain role@cpu entries\n");
            return false;
        }
        const char* p = spec;
        while (*p) {
            Role role = Role::Idle;
            if (!std::strncmp(p, "ifid@", 5)) {
                role = Role::Ifid; p += 5;
            } else if (!std::strncmp(p, "ex@", 3)) {
                role = Role::Ex; p += 3;
            } else {
                std::fprintf(stderr, "--place: expected ifid@cpu or ex@cpu near '%s'\n", p);
                return false;
            }
            if (*p < '0' || *p > '9') {
                std::fprintf(stderr, "--place: expected a non-negative cpu near '%s'\n", p);
                return false;
            }
            char* end = nullptr;
            const long cpu = std::strtol(p, &end, 10);
            if (cpu < 0 || cpu >= CPU_SETSIZE || (*end != ',' && *end != '\0')) {
                std::fprintf(stderr, "--place: invalid cpu near '%s'\n", p);
                return false;
            }
            const uint32_t domain = topo.domain_of(static_cast<int>(cpu));
            if (domain == kNoDomain) {
                std::fprintf(stderr, "--place: cpu %ld is outside the affinity mask\n", cpu);
                return false;
            }
            if (threads_.size() == kMaxThreads) {
                std::fprintf(stderr, "--place exceeds the %u-thread channel limit\n", kMaxThreads);
                return false;
            }
            append(role, static_cast<int>(cpu), domain);
            if (*end == '\0') break;
            p = end + 1;
            if (!*p) {
                std::fprintf(stderr, "--place: trailing comma\n");
                return false;
            }
        }
        return true;
    }

    // A manual topology map is complete: shard IDs name dense executor thread IDs, not CPUs.
    // Executors omitted from the map remain valid empty fillers. Runtime migration still uses
    // the ordinary ownership transaction; this map selects only initial ownership.
    bool assign_shard_homes(uint32_t nshards, const char* spec = nullptr) {
        if (ex_.empty()) return false;
        shard_home_.assign(nshards, kNoThread);
        if (!spec) {
            for (uint32_t sid = 0; sid < nshards; sid++)
                shard_home_[sid] = ex_[sid % ex_.size()];
            return true;
        }
        if (!*spec) {
            std::fprintf(stderr, "--shard-home must contain shard:thread pairs\n");
            return false;
        }
        std::vector<bool> seen(nshards, false);
        const char* p = spec;
        while (*p) {
            uint32_t sid = 0, tid = 0;
            if (!parse_pair(p, sid, tid)) {
                std::fprintf(stderr, "--shard-home: expected shard:thread pairs near '%s'\n", p);
                return false;
            }
            if (sid >= nshards) {
                std::fprintf(stderr, "--shard-home: shard %u is outside 0..%u\n", sid, nshards - 1);
                return false;
            }
            if (tid >= threads_.size() || !is_executor(tid)) {
                std::fprintf(stderr, "--shard-home: thread %u is not an ex thread\n", tid);
                return false;
            }
            if (seen[sid]) {
                std::fprintf(stderr, "--shard-home: shard %u is assigned more than once\n", sid);
                return false;
            }
            seen[sid] = true;
            shard_home_[sid] = tid;
            if (*p == '\0') break;
            p++;
            if (!*p) {
                std::fprintf(stderr, "--shard-home: trailing comma\n");
                return false;
            }
        }
        for (uint32_t sid = 0; sid < nshards; sid++) {
            if (!seen[sid]) {
                std::fprintf(stderr, "--shard-home: shard %u has no owner (manual maps must be complete)\n", sid);
                return false;
            }
        }
        return true;
    }

    uint32_t total_threads() const { return static_cast<uint32_t>(threads_.size()); }

    ThreadPlacement&       thread(uint32_t tid)       { return threads_[tid]; }
    const ThreadPlacement& thread(uint32_t tid) const { return threads_[tid]; }
    const std::vector<ThreadPlacement>& threads() const { return threads_; }
    const std::vector<uint32_t>& ifid_threads() const { return ifid_; }
    const std::vector<uint32_t>& ex_threads()   const { return ex_; }
    bool is_executor(uint32_t tid) const {
        return std::find(ex_.begin(), ex_.end(), tid) != ex_.end();
    }

    // Resolve cpu siblings to dense thread ids after either placement frontend has run. Pair mode
    // is a boot contract, not a best effort: a selected cpu with an absent sibling, a non-pair
    // sysfs topology, or two sibling threads assigned different roles rejects the shape.
    bool configure_smt_units(const Topology& topo, bool enabled) {
        smt_peer_.assign(threads_.size(), kNoThread);
        if (!enabled) return true;
        std::vector<uint32_t> cpu_to_tid(CPU_SETSIZE, kNoThread);
        for (uint32_t tid = 0; tid < threads_.size(); tid++) {
            const int cpu = threads_[tid].cpu;
            if (cpu < 0 || cpu >= CPU_SETSIZE || cpu_to_tid[cpu] != kNoThread) {
                std::fprintf(stderr,
                             "SMT placement: cpu %d is not a unique provisioned logical cpu\n", cpu);
                return false;
            }
            cpu_to_tid[cpu] = tid;
        }
        for (uint32_t tid = 0; tid < threads_.size(); tid++) {
            const int cpu = threads_[tid].cpu;
            const std::vector<int>& siblings = topo.thread_siblings(cpu);
            if (siblings.size() != 2 ||
                std::find(siblings.begin(), siblings.end(), cpu) == siblings.end()) {
                std::fprintf(stderr,
                             "SMT placement: cpu %d does not have one sysfs sibling\n", cpu);
                return false;
            }
            const int peer_cpu = siblings[0] == cpu ? siblings[1] : siblings[0];
            const uint32_t peer = peer_cpu >= 0 && peer_cpu < CPU_SETSIZE
                ? cpu_to_tid[peer_cpu] : kNoThread;
            if (peer == kNoThread) {
                std::fprintf(stderr,
                             "SMT placement: cpu %d requires sibling cpu %d in the placement\n",
                             cpu, peer_cpu);
                return false;
            }
            const std::vector<int>& reverse = topo.thread_siblings(peer_cpu);
            if (reverse.size() != 2 ||
                std::find(reverse.begin(), reverse.end(), cpu) == reverse.end()) {
                std::fprintf(stderr,
                             "SMT placement: cpu %d and cpu %d are not a reciprocal sysfs pair\n",
                             cpu, peer_cpu);
                return false;
            }
            if (threads_[peer].role != threads_[tid].role) {
                std::fprintf(stderr,
                             "SMT placement: sibling cpus %d and %d have different roles\n",
                             cpu, peer_cpu);
                return false;
            }
            smt_peer_[tid] = peer;
        }
        for (uint32_t tid = 0; tid < threads_.size(); tid++)
            if (smt_peer_[tid] == tid || smt_peer_[tid] >= threads_.size() ||
                smt_peer_[smt_peer_[tid]] != tid) return false;
        return true;
    }

    uint32_t smt_peer(uint32_t tid) const {
        return tid < smt_peer_.size() ? smt_peer_[tid] : kNoThread;
    }

    bool reserve_runtime_roles(uint32_t total) {
        try {
            ifid_.reserve(total);
            ex_.reserve(total);
            return true;
        } catch (...) {
            return false;
        }
    }

    // Invoked only under FLIP's global quiescence barrier. Hot fan-out readers are paused while
    // these dense vectors change, so runtime role publication does not put a mutex on their paths.
    void set_runtime_role(uint32_t tid, Role role) {
        if (tid >= threads_.size() || (role != Role::Ifid && role != Role::Ex)) std::abort();
        auto erase_tid = [&](std::vector<uint32_t>& ids) {
            ids.erase(std::remove(ids.begin(), ids.end(), tid), ids.end());
        };
        erase_tid(ifid_);
        erase_tid(ex_);
        (role == Role::Ifid ? ifid_ : ex_).push_back(tid);
        std::sort(ifid_.begin(), ifid_.end());
        std::sort(ex_.begin(), ex_.end());
        threads_[tid].role = role;
    }

    Role role_of(uint32_t tid) const { return threads_[tid].role; }
    int cpu_of_thread(uint32_t tid) const { return threads_[tid].cpu; }
    uint32_t domain_of_thread(uint32_t tid) const { return threads_[tid].domain; }
    uint32_t shard_home(uint32_t sid) const { return shard_home_[sid]; }

private:
    bool build_even_smt(const Topology& topo, uint32_t n_ifid, uint32_t n_ex) {
        if ((n_ifid & 1u) || (n_ex & 1u)) {
            std::fprintf(stderr,
                         "--ratio: SMT placement requires even logical io and ex counts\n");
            return false;
        }
        const uint32_t nd = topo.ndomains() ? topo.ndomains() : 1;
        using CpuPair = std::pair<int, int>;
        std::vector<std::vector<CpuPair>> units(nd);
        std::vector<bool> used(CPU_SETSIZE, false);
        for (uint32_t d = 0; d < nd; d++) {
            for (int cpu : topo.cpus_in(d)) {
                if (used[cpu]) continue;
                const std::vector<int>& siblings = topo.thread_siblings(cpu);
                if (siblings.size() != 2 ||
                    std::find(siblings.begin(), siblings.end(), cpu) == siblings.end()) {
                    std::fprintf(stderr,
                                 "SMT placement: cpu %d does not have one sysfs sibling\n", cpu);
                    return false;
                }
                const int peer = siblings[0] == cpu ? siblings[1] : siblings[0];
                const uint32_t peer_domain = topo.domain_of(peer);
                if (peer_domain == kNoDomain) continue; // incomplete affinity unit is unavailable
                if (peer_domain != d) {
                    std::fprintf(stderr,
                                 "SMT placement: sibling cpus %d and %d span L3 domains\n",
                                 cpu, peer);
                    return false;
                }
                const std::vector<int>& reverse = topo.thread_siblings(peer);
                if (reverse.size() != 2 ||
                    std::find(reverse.begin(), reverse.end(), cpu) == reverse.end()) {
                    std::fprintf(stderr,
                                 "SMT placement: cpu %d and cpu %d are not a reciprocal sysfs pair\n",
                                 cpu, peer);
                    return false;
                }
                if (used[peer]) {
                    std::fprintf(stderr, "SMT placement: cpu %d belongs to two sibling units\n", peer);
                    return false;
                }
                used[cpu] = used[peer] = true;
                units[d].push_back(CpuPair{cpu, peer});
            }
        }

        const uint64_t total = static_cast<uint64_t>(n_ifid) + n_ex;
        uint64_t capacity_units = 0;
        for (const auto& domain : units) capacity_units += domain.size();
        if (total > capacity_units * 2) {
            std::fprintf(stderr,
                         "--ratio: %llu threads but only %llu complete SMT pairs are allowed\n",
                         static_cast<unsigned long long>(total),
                         static_cast<unsigned long long>(capacity_units));
            return false;
        }
        if (total > kMaxThreads) {
            std::fprintf(stderr, "--ratio: %llu threads exceeds the %u-thread channel limit\n",
                         static_cast<unsigned long long>(total), kMaxThreads);
            return false;
        }

        std::vector<uint32_t> qi(nd), qe(nd);
        auto spread = [nd](uint32_t count, std::vector<uint32_t>& quotas) {
            for (uint32_t d = 0; d < nd; d++)
                quotas[d] = count / nd + (d < count % nd ? 1u : 0u);
        };
        spread(n_ifid / 2, qi);
        spread(n_ex / 2, qe);
        for (uint32_t d = 0; d < nd; d++) {
            while (qi[d] + qe[d] > units[d].size()) {
                uint32_t target = kNoDomain;
                uint64_t best_room = 0;
                for (uint32_t other = 0; other < nd; other++) {
                    if (other == d) continue;
                    const uint64_t assigned = qi[other] + qe[other];
                    if (units[other].size() > assigned &&
                        units[other].size() - assigned > best_room) {
                        best_room = units[other].size() - assigned;
                        target = other;
                    }
                }
                if (target == kNoDomain) {
                    std::fprintf(stderr, "--ratio: no domain has an SMT pair to rebalance\n");
                    return false;
                }
                if (qe[d]) { qe[d]--; qe[target]++; }
                else       { qi[d]--; qi[target]++; }
            }
        }
        for (uint32_t d = 0; d < nd; d++) {
            uint32_t slot = 0;
            auto append_pair = [&](Role role) {
                const CpuPair& pair = units[d][slot++];
                append(role, pair.first, d);
                append(role, pair.second, d);
            };
            for (uint32_t count = 0; count < qi[d]; count++) append_pair(Role::Ifid);
            for (uint32_t count = 0; count < qe[d]; count++) append_pair(Role::Ex);
        }
        return true;
    }

    void clear() {
        threads_.clear();
        ifid_.clear();
        ex_.clear();
        shard_home_.clear();
        smt_peer_.clear();
    }

    void append(Role role, int cpu, uint32_t domain) {
        const uint32_t tid = static_cast<uint32_t>(threads_.size());
        threads_.push_back(ThreadPlacement{tid, role, cpu, domain});
        (role == Role::Ifid ? ifid_ : ex_).push_back(tid);
    }


    static bool parse_u32(const char*& p, uint32_t& out) {
        if (*p < '0' || *p > '9') return false;
        char* end = nullptr;
        const unsigned long value = std::strtoul(p, &end, 10);
        if (value > UINT32_MAX) return false;
        out = static_cast<uint32_t>(value);
        p = end;
        return true;
    }

    static bool parse_pair(const char*& p, uint32_t& left, uint32_t& right) {
        if (!parse_u32(p, left) || *p != ':') return false;
        p++;
        return parse_u32(p, right) && (*p == ',' || *p == '\0');
    }

    std::vector<ThreadPlacement> threads_;
    std::vector<uint32_t> ifid_;
    std::vector<uint32_t> ex_;
    std::vector<uint32_t> shard_home_;
    std::vector<uint32_t> smt_peer_;
};

// ---------------------------------------------------------------------------------------------
// THE MIGRATION CONTRACT. Moving a physical shard rewrites the EX-owner bits of its bucket range
// behind Router's transfer descriptor. What makes it correct is the ordering around it:
//
//   1. Bring the old owner to an execution safe point. task_in.quiesced() tests the RETIRED
//      frontier; head == tail alone is insufficient because a popped task may still be executing.
//   2. Publish PREPARING, rewrite all entries while routing still resolves to the old owner, and
//      settle owner-local bookkeeping while both loops remain quiescent.
//   3. Publish COMMITTED once. Only then may the new owner touch the shard's store. A producer that
//      had already read the old route is forwarded by the old owner before shard access.
//
// Skipping step 2 puts two threads in one FlatStore, which has no locks precisely because that is
// supposed to be impossible — it would corrupt silently rather than crash.
//
// Written as a contract rather than an implementation because there is no LB yet and a half-built
// migration path is worse than none.
// ---------------------------------------------------------------------------------------------
struct MigrationPlan {
    int32_t  shard_id    = -1;
    uint32_t from_thread = 0;
    uint32_t to_thread   = 0;
    size_t   cost_bytes  = 0;    // what the new domain must re-pull; Shard::migration_cost_bytes()
};

}  // namespace tomo

// placement.h — per-thread role/cpu placement and the shard migration contract.
//
// THREAD IS THE LOCALITY UNIT. Each dense thread id has one role, one cpu and therefore one L3
// domain. There is deliberately no node layer between a thread and those facts: SMT siblings,
// cross-CCX layouts and deliberately asymmetric experiments all fit the same representation.

#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    // EVEN GLOBAL PLACEMENT. Counts are whole-server, so shapes no per-node grammar can express
    // (15:2:15) are first-class. Each role is spread across the L3 domains as evenly as integer
    // division allows and roles are interleaved within a domain, so sender pairing stays local.
    //
    // This is also the flip-era invariant in batch form: a runtime controller changing one thread's
    // role keeps these same quotas incrementally -- pick the convert candidate from the domain
    // whose count for the shrinking role sits above quota and whose count for the growing role
    // sits below it. Placement stays a pure function of (counts, topology); no node layer.
    bool build_even(const Topology& topo, uint32_t n_ifid, uint32_t n_ex) {
        clear();
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

    // Shard ownership is a flat shard->EX tid table. A manual map must be complete because mixing
    // an accidental omission with defaults would make a supposedly controlled placement measure a
    // different layout. Runtime dispatch still reads Server::shard_owner_ exactly once.
    bool assign_shard_homes(uint32_t nshards, const char* spec) {
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
            if (tid >= threads_.size() || threads_[tid].role != Role::Ex) {
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

    Role role_of(uint32_t tid) const { return threads_[tid].role; }
    int cpu_of_thread(uint32_t tid) const { return threads_[tid].cpu; }
    uint32_t domain_of_thread(uint32_t tid) const { return threads_[tid].domain; }
    uint32_t shard_home(uint32_t sid) const { return shard_home_[sid]; }

private:
    void clear() {
        threads_.clear();
        ifid_.clear();
        ex_.clear();
        shard_home_.clear();
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
};

// ---------------------------------------------------------------------------------------------
// THE MIGRATION CONTRACT. Moving a shard is a single store into worker_of_shard[]. What makes it
// correct is the ordering around it:
//
//   1. Stop routing new ops to the old owner (the store does this; it is a release).
//   2. WAIT for the old owner to finish what it already has — task_in.quiesced(), which tests the
//      RETIRED frontier. head == tail only means "nothing left to pop"; the worker may still be
//      executing what it popped.
//   3. Only then may the new owner touch the shard's store.
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

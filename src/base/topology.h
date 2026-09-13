// topology.h — what the machine actually looks like, discovered rather than assumed.
//
// DOMAINS ARE SHARED-L3 CPU GROUPS, and this file discovers those boundaries without imposing a
// placement unit on them. Individual threads may be placed anywhere in the allowed affinity mask;
// the domain lookup says which cache each chosen cpu belongs to.
//
// Nothing here DECIDES placement (that is placement.h) and nothing here is consulted on the hot
// path. It records what a later flip/LB controller needs in order to decide:
//
//   - which L3 domain each CPU belongs to
//   - which domain a worker is currently running in
//   - which domain a shard's working set is currently resident in
//   - how many ops were executed from a FOREIGN domain (the actionable signal)
//
// WHY L3 AND NOT NUMA. This box is NPS1: one NUMA node over 128 GB, so there is no NUMA distance to
// exploit and the only locality available is cache locality. A "NUMA-sharded" store here is really
// an L3-sharded store, and calling it that avoids a whole class of wrong reasoning.
//
// WHY THIS MATTERS FOR MIGRATION, which is the part that is easy to miss: an L3 domain is not
// allocated into, it is filled by access. Moving a shard from a worker in domain A to a worker in
// domain B does not move its memory — it invalidates its residency. The new worker must re-pull the
// working set through the fabric, and on this box a CCX-to-fabric link saturates around 51 GB/s
// where a single core can already pull 50. So shard migration is NOT free, and an LB that treats it
// as free will thrash. resident_estimate() exists so the cost can be priced.
#pragma once
#include <sched.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

namespace tomo {

inline constexpr uint32_t kNoDomain = UINT32_MAX;

class Topology {
public:
    // Discovers L3 domains for the CPUs this process is ALLOWED to run on. Respecting the affinity
    // mask matters: under taskset the machine's full topology is not what we get, and assuming
    // otherwise is how threads end up "placed" onto cpus they can never run on.
    // OPERATOR-DECLARED TOPOLOGY (--node-cpus). "0-7,8-15" builds two declared domains and
    // bypasses discovery, so deliberate off-hardware shapes can still build
    // shapes discovery would never produce. The declaration is intersected with the affinity mask:
    // a cpu the process cannot run on is a config error worth failing loudly, not silently pinning.
    bool declare(const char* spec) {
        cpu_set_t allowed;
        CPU_ZERO(&allowed);
        if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return false;
        domain_of_.assign(CPU_SETSIZE, kNoDomain);
        domains_.clear();

        const char* p = spec;
        while (*p) {
            std::vector<int> cpus;
            while (*p && *p != ',') {
                char* end = nullptr;
                long a = std::strtol(p, &end, 10);
                if (end == p) return false;
                long b = a;
                if (*end == '-') { p = end + 1; b = std::strtol(p, &end, 10); if (end == p) return false; }
                for (long c = a; c <= b; c++) {
                    if (c < 0 || c >= CPU_SETSIZE) return false;
                    if (!CPU_ISSET(c, &allowed)) {
                        std::fprintf(stderr, "topology: declared cpu %ld is outside the affinity mask\n", c);
                        return false;
                    }
                    cpus.push_back(static_cast<int>(c));
                }
                p = end;
                if (*p == '+') p++;                      // "0-3+8-11" glues ranges into one node
            }
            if (!cpus.empty()) {
                const uint32_t id = static_cast<uint32_t>(domains_.size());
                for (int c : cpus) domain_of_[c] = id;
                domains_.push_back(std::move(cpus));
            }
            if (*p == ',') p++;
        }
        return !domains_.empty();
    }

    bool discover() {
        cpu_set_t allowed;
        CPU_ZERO(&allowed);
        if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return false;

        domain_of_.assign(CPU_SETSIZE, kNoDomain);
        domains_.clear();

        for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
            if (!CPU_ISSET(cpu, &allowed)) continue;
            if (domain_of_[cpu] != kNoDomain) continue;

            std::vector<int> peers;
            if (!read_l3_peers(cpu, allowed, peers)) peers.push_back(cpu);   // no sysfs: own domain

            const uint32_t d = static_cast<uint32_t>(domains_.size());
            for (int p : peers) if (domain_of_[p] == kNoDomain) domain_of_[p] = d;
            domains_.push_back(std::move(peers));
        }
        return !domains_.empty();
    }

    uint32_t ndomains() const { return static_cast<uint32_t>(domains_.size()); }
    uint32_t domain_of(int cpu) const {
        return (cpu >= 0 && cpu < static_cast<int>(domain_of_.size())) ? domain_of_[cpu] : kNoDomain;
    }
    const std::vector<int>& cpus_in(uint32_t d) const { return domains_[d]; }

    void dump(FILE* f) const {
        std::fprintf(f, "topology: %u L3 domain(s)\n", ndomains());
        for (uint32_t d = 0; d < ndomains(); d++) {
            std::fprintf(f, "  domain %2u: %zu cpus [", d, domains_[d].size());
            for (size_t i = 0; i < domains_[d].size(); i++)
                std::fprintf(f, "%s%d", i ? "," : "", domains_[d][i]);
            std::fprintf(f, "]\n");
        }
    }

private:
    // Reads the CPUs sharing this cpu's level-3 cache, intersected with the allowed set.
    static bool read_l3_peers(int cpu, const cpu_set_t& allowed, std::vector<int>& out) {
        for (int idx = 0; idx < 8; idx++) {
            char path[128];
            std::snprintf(path, sizeof(path),
                          "/sys/devices/system/cpu/cpu%d/cache/index%d/level", cpu, idx);
            FILE* f = std::fopen(path, "r");
            if (!f) continue;
            int level = 0;
            if (std::fscanf(f, "%d", &level) != 1) { std::fclose(f); continue; }
            std::fclose(f);
            if (level != 3) continue;

            std::snprintf(path, sizeof(path),
                          "/sys/devices/system/cpu/cpu%d/cache/index%d/shared_cpu_list", cpu, idx);
            f = std::fopen(path, "r");
            if (!f) continue;
            char buf[512] = {0};
            if (!std::fgets(buf, sizeof(buf), f)) { std::fclose(f); continue; }
            std::fclose(f);
            parse_cpu_list(buf, allowed, out);
            return !out.empty();
        }
        return false;
    }

    // "0-7,128-135" -> the allowed members of that set.
    static void parse_cpu_list(const char* s, const cpu_set_t& allowed, std::vector<int>& out) {
        while (*s) {
            while (*s == ',' || *s == ' ' || *s == '\n') s++;
            if (!*s) break;
            char* end = nullptr;
            long a = std::strtol(s, &end, 10);
            long b = a;
            if (end && *end == '-') b = std::strtol(end + 1, &end, 10);
            for (long c = a; c <= b; c++)
                if (c >= 0 && c < CPU_SETSIZE && CPU_ISSET(static_cast<int>(c), &allowed))
                    out.push_back(static_cast<int>(c));
            s = end ? end : s + 1;
        }
    }

    std::vector<uint32_t>          domain_of_;
    std::vector<std::vector<int>>  domains_;
};

}  // namespace tomo

// read_local_observe.h -- observation of the lane without a per-command logging arm.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <string>

namespace tomo {

// SLOWLOG would capture argv, take clocks and write a shared log on the path that avoids that
// work. Keep exact existing hit/fallback counters; sample ONE reply preparation per physical
// thread per distinct cached millisecond instead. A longer observation WINDOW improves coverage;
// there is no sample-rate knob and this signal never feeds LB or decides read eligibility.
//
// The sampling test is at a nonempty drain boundary. The ordinary drain is instantiated with
// Observe=false, so its per-command loop has no sampling branch, clock, counter or pointer chase.
// The sampled instantiation chooses one entry in its first chunk, preserving the same gather,
// prefetch, execution and commit order. A sample only enters the histogram if it commits locally.
// Bytes are the RESP bytes of those sampled replies, NOT an extrapolated total. Service time is
// prepare_local_read(): pure GET chunks capture/prefetch BEFORE that call, whereas MGET and mixed
// chunks capture inside it. This is neither end-to-end nor whole-lane latency; it excludes lane
// residence, chunk gathering, completion and sending. Equal time sampling is not a uniform sample
// of commands, and a pooled histogram across threads/GET/MGET must not imply command quantiles.
struct alignas(64) ReadLocalObservation {
    static constexpr std::array<uint64_t, 9> upper_ns = {
        128, 256, 512, 1024, 2048, 4096, 16384, 65536, UINT64_MAX};
    static constexpr const char* bounds = "128,256,512,1024,2048,4096,16384,65536,inf";
    static constexpr const char* latency_scope = "sampled_prepare_local_read_ns";
    struct Snapshot {
        uint64_t sampled_hits = 0, sampled_reply_bytes = 0, sampled_service_ns = 0;
        uint64_t sampled_mget_hits = 0, uncommitted_samples = 0;
        std::array<uint64_t, upper_ns.size()> histogram{};
        void add(const Snapshot& other) {
            sampled_hits += other.sampled_hits;
            sampled_reply_bytes += other.sampled_reply_bytes;
            sampled_service_ns += other.sampled_service_ns;
            sampled_mget_hits += other.sampled_mget_hits;
            uncommitted_samples += other.uncommitted_samples;
            for (size_t i = 0; i < histogram.size(); ++i) histogram[i] += other.histogram[i];
        }
    };
    struct Sample {
        uint32_t index = UINT32_MAX;
        uint64_t elapsed_ns = 0, bytes = 0;
        bool prepared = false, mget = false;
    };

    // Owner-private; never scanned by INFO. Keep the publication on different cache lines so an
    // observer cannot make the drain's sampling test contend with publication stores.
    int64_t last_sample_ms = std::numeric_limits<int64_t>::min();
    uint64_t random = 0;

    uint32_t choose(uint32_t count, uint32_t tid) {
        // Independent per-thread phase, advanced only on a sampled drain. Vary the index to avoid
        // always timing the first GET or the same member of a periodic GET/MGET pipeline.
        uint64_t x = random ? random : (uint64_t{tid} + 1) * 0x9e3779b97f4a7c15ull;
        x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
        random = x;
        return (x * 2685821657736338717ull) % count;
    }

    static size_t bucket(uint64_t ns) {
        size_t i = 0;
        while (ns > upper_ns[i]) ++i;
        return i;
    }

    void record(const Sample& sample, uint32_t completed) {
        if (!sample.prepared) return;
        if (sample.index >= completed) {
            add(uncommitted_samples_, 1);
            return;
        }
        add(sampled_hits_, 1);
        add(sampled_reply_bytes_, sample.bytes);
        add(sampled_service_ns_, sample.elapsed_ns);
        add(sampled_mget_hits_, sample.mget);
        add(histogram_[bucket(sample.elapsed_ns)], 1);
    }

    Snapshot snapshot() const {
        Snapshot result;
        result.sampled_hits = sampled_hits_.load(std::memory_order_relaxed);
        result.sampled_reply_bytes = sampled_reply_bytes_.load(std::memory_order_relaxed);
        result.sampled_service_ns = sampled_service_ns_.load(std::memory_order_relaxed);
        result.sampled_mget_hits = sampled_mget_hits_.load(std::memory_order_relaxed);
        result.uncommitted_samples = uncommitted_samples_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < upper_ns.size(); ++i)
            result.histogram[i] = histogram_[i].load(std::memory_order_relaxed);
        return result;
    }

    // One writer, so relaxed load/store is sufficient and emits no locked RMW. INFO takes one
    // independent load per counter, without retries or a seqlock. An in-flight sample can straddle
    // an INFO snapshot: this is explicitly not a transactional snapshot of the counter set.
    static void add(std::atomic<uint64_t>& counter, uint64_t value) {
        counter.store(counter.load(std::memory_order_relaxed) + value, std::memory_order_relaxed);
    }
    alignas(64) std::atomic<uint64_t> sampled_hits_{0};
    std::atomic<uint64_t> sampled_reply_bytes_{0}, sampled_service_ns_{0};
    std::atomic<uint64_t> sampled_mget_hits_{0}, uncommitted_samples_{0};
    std::array<std::atomic<uint64_t>, upper_ns.size()> histogram_{};
};
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(sizeof(ReadLocalObservation) == 192);

inline void append_read_local_histogram(std::string& body,
                                      const ReadLocalObservation::Snapshot& sample) {
    for (size_t i = 0; i < sample.histogram.size(); ++i) {
        if (i) body += ',';
        body += std::to_string(sample.histogram[i]);
    }
    body += "\r\n";
}

// Dependent Server keeps this entire feature in one header without a thread/server include cycle.
// Only INFO instantiates this cold walk. No new per-client state or accept-time accounting: both
// current placement and accept history already exist. Lifetime counters survive RESETSTAT/FLIP;
// clients/active/shards/role are endpoint gauges, never subtractable event counters.
template <class Server>
__attribute__((noinline, cold))
void append_read_local_observation_info(std::string& body, const Server& server) {
    const bool enabled = server.read_local_enabled();
    body += "# Read_local\r\nread_local_schema:1\r\nread_local_enabled:";
    body += enabled ? "1\r\n" : "0\r\n";
    body += "read_local_counter_scope:lifetime\r\n"
            "read_local_thread_roles:0=idle,1=io,2=ex_or_fused\r\n"
            "read_local_snapshot_scope:independent_counters\r\n"
            "read_local_misses_scope:command_fallbacks_not_keyspace_misses\r\n"
            "read_local_bytes_scope:sampled_resp_replies\r\n"
            "read_local_sample_period_ms:1\r\nread_local_latency_scope:";
    body += ReadLocalObservation::latency_scope;
    body += "\r\nread_local_latency_bucket_upper_ns:";
    body += ReadLocalObservation::bounds;
    body += "\r\n";
    std::array<uint32_t, 128> owned{};
    for (uint32_t sid = 0; sid < server.nshards(); ++sid)
        ++owned[server.worker_of_shard(static_cast<int32_t>(sid))];
    uint64_t hits = 0, misses = 0, arms = 0, connections = 0;
    ReadLocalObservation::Snapshot total;
    for (uint32_t tid = 0; tid < server.nthreads(); ++tid) {
        const auto& thread = server.thread(tid);
        uint64_t local_hits = 0, local_misses = 0, local_arms = 0, sidecars = 0;
        uint64_t defer_quota = 0, defer_full = 0, mget_retries = 0;
        ReadLocalObservation::Snapshot sample;
        if (enabled) {
            const auto& stats = thread.read_local_stats();
            local_hits = stats.hits;
            local_misses = stats.fallbacks();
            local_arms = stats.arm.arms;
            sidecars = stats.arm.sidecars;
            defer_quota = stats.defer_quota;
            defer_full = stats.defer_lane_full;
            mget_retries = stats.mget_generation_retries;
            sample = thread.read_local_observation().snapshot();
        }
        const uint64_t clients = thread.client_count();
        const std::string prefix = "read_local_thread_" + std::to_string(tid);
        body += prefix + "_observation:role=" + std::to_string(static_cast<unsigned>(thread.role()));
        auto field = [&](const char* key, uint64_t value) {
            body += ','; body += key; body += '='; body += std::to_string(value);
        };
        field("active", thread.read_local_lane_active());
        field("shards", owned[tid]);
        field("connections", clients);
        field("accepts", thread.sig().accepts);
        field("hits", local_hits); field("misses", local_misses);
        field("arms", local_arms); field("write_ring_sidecars", sidecars);
        field("defer_quota", defer_quota); field("defer_lane_full", defer_full);
        field("mget_generation_retries", mget_retries);
        field("sampled_hits", sample.sampled_hits);
        field("sampled_reply_bytes", sample.sampled_reply_bytes);
        field("sampled_service_ns", sample.sampled_service_ns);
        field("sampled_mget_hits", sample.sampled_mget_hits);
        field("uncommitted_samples", sample.uncommitted_samples);
        body += "\r\n" + prefix + "_latency_histogram:";
        append_read_local_histogram(body, sample);
        hits += local_hits; misses += local_misses; arms += local_arms; connections += clients;
        total.add(sample);
    }
    auto line = [&](const char* name, uint64_t value) {
        body += "read_local_"; body += name; body += ':'; body += std::to_string(value);
        body += "\r\n";
    };
    line("threads", server.nthreads());
    line("observation_bytes", enabled ? server.nthreads() * sizeof(ReadLocalObservation) : 0);
    line("hits_total", hits); line("misses_total", misses); line("arms_total", arms);
    line("connections", connections);
    line("sampled_hits", total.sampled_hits);
    line("sampled_reply_bytes", total.sampled_reply_bytes);
    line("sampled_service_ns", total.sampled_service_ns);
    line("sampled_mget_hits", total.sampled_mget_hits);
    line("uncommitted_samples", total.uncommitted_samples);
    body += "read_local_latency_histogram:";
    append_read_local_histogram(body, total);
}

} // namespace tomo

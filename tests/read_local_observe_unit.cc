// Serverless contracts for sampled lane telemetry. In particular, a reply prepared after a
// fallback is NOT a locally committed hit and must never contribute bytes or latency buckets.
#include "src/core/read_local_observe.h"
#include <cassert>
#include <cstdio>

namespace {
// Exercise the production formatter without a server. Disabled accessors abort: an INFO request
// with read-local=0 must render zero evidence without dereferencing or creating an armed sidecar.
struct Thread {
    bool enabled = false;
    uint32_t clients = 0, current_role = 0;
    struct Stats {
        uint64_t hits = 0, misses = 0, defer_quota = 0, defer_lane_full = 0;
        uint64_t mget_generation_retries = 0;
        struct { uint64_t arms = 0, sidecars = 0; } arm;
        uint64_t fallbacks() const { return misses; }
    } stats;
    struct { uint64_t accepts = 0; } signals;
    tomo::ReadLocalObservation observation;
    const Stats& read_local_stats() const { assert(enabled); return stats; }
    const tomo::ReadLocalObservation& read_local_observation() const {
        assert(enabled); return observation;
    }
    uint32_t client_count() const { return clients; }
    uint32_t role() const { return current_role; }
    bool read_local_lane_active() const { return enabled && current_role != 0; }
    const auto& sig() const { return signals; }
};
struct Server {
    bool enabled = false;
    std::array<Thread, 2> threads;
    bool read_local_enabled() const { return enabled; }
    uint32_t nshards() const { return 16; }
    uint32_t nthreads() const { return threads.size(); }
    uint32_t worker_of_shard(int32_t sid) const { return sid % 2; }
    const Thread& thread(uint32_t tid) const { return threads[tid]; }
};
void expect_line(const std::string& text, const std::string& line) {
    assert(text.find(line + "\r\n") != std::string::npos);
}
} // namespace

int main() {
    using Observation = tomo::ReadLocalObservation;
    for (size_t i = 0; i < Observation::upper_ns.size(); ++i) {
        assert(Observation::bucket(Observation::upper_ns[i]) == i);
        if (i + 1 < Observation::upper_ns.size())
            assert(Observation::bucket(Observation::upper_ns[i] + 1) == i + 1);
    }
    Observation observation;
    Observation::Sample sample;
    observation.record(sample, 1);
    assert(observation.snapshot().sampled_hits == 0);

    sample.index = 2;
    sample.elapsed_ns = 256;
    sample.bytes = 71;
    sample.prepared = true;
    observation.record(sample, 2); // Only entries 0 and 1 commit; entry 2 is discarded.
    auto counts = observation.snapshot();
    assert(counts.sampled_hits == 0 && counts.sampled_reply_bytes == 0);
    assert(counts.sampled_service_ns == 0 && counts.uncommitted_samples == 1);
    for (auto bucket : counts.histogram) assert(bucket == 0);

    observation.record(sample, 3);
    sample.index = 0;
    sample.elapsed_ns = 100000;
    sample.bytes = 142;
    sample.mget = true;
    observation.record(sample, 1);
    counts = observation.snapshot();
    assert(counts.sampled_hits == 2 && counts.sampled_reply_bytes == 213);
    assert(counts.sampled_service_ns == 100256 && counts.sampled_mget_hits == 1);
    assert(counts.histogram[1] == 1 && counts.histogram[8] == 1);
    std::string text;
    tomo::append_read_local_histogram(text, counts);
    assert(text == "0,1,0,0,0,0,0,0,1\r\n");
    Observation::Snapshot aggregate;
    aggregate.add(counts);
    aggregate.add(counts);
    assert(aggregate.sampled_hits == 4 && aggregate.histogram[8] == 2);

    // Every slot can be observed; periodic workloads must not always sample their first member.
    uint32_t visited = 0;
    for (unsigned i = 0; i < 4096; ++i) visited |= 1u << observation.choose(32, 0);
    assert(visited == UINT32_MAX);
    assert(observation.choose(1, 0) == 0);

    Server server;
    text.clear();
    tomo::append_read_local_observation_info(text, server);
    expect_line(text, "read_local_enabled:0");
    expect_line(text, "read_local_observation_bytes:0");
    expect_line(text, "read_local_hits_total:0");
    expect_line(text, "read_local_latency_histogram:0,0,0,0,0,0,0,0,0");

    server.enabled = true;
    for (auto& thread : server.threads) thread.enabled = true;
    auto& thread = server.threads[1];
    thread.clients = 17;
    thread.current_role = 1;
    thread.signals.accepts = 21;
    thread.stats.hits = 900;
    thread.stats.misses = 3;
    thread.stats.arm.arms = 20;
    thread.observation.record(sample, 1);
    server.threads[0].stats.hits = 100;
    text.clear();
    tomo::append_read_local_observation_info(text, server);
    expect_line(text, "read_local_enabled:1");
    expect_line(text, "read_local_observation_bytes:384");
    expect_line(text, "read_local_hits_total:1000");
    expect_line(text, "read_local_misses_total:3");
    expect_line(text, "read_local_arms_total:20");
    expect_line(text, "read_local_connections:17");
    expect_line(text, "read_local_sampled_reply_bytes:142");
    expect_line(text, "read_local_latency_histogram:0,0,0,0,0,0,0,0,1");
    expect_line(text, "read_local_thread_1_latency_histogram:0,0,0,0,0,0,0,0,1");
    assert(text.find("read_local_thread_1_observation:role=1,active=1,shards=8,"
                     "connections=17,accepts=21,hits=900,misses=3,arms=20,") != std::string::npos);
    std::puts("read-local observation contracts passed");
}

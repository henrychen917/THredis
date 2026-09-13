#include "info_stats.h"

#include "../core/signal.h"

#include <algorithm>
#include <array>
#include <mutex>

namespace tomo {
namespace {

constexpr uint64_t kSampleMinNs = 100000000ull;
constexpr size_t kSampleWindow = 8;

struct InfoStatsState {
    std::mutex mu;
    uint64_t last_sample_ns = 0;
    uint64_t last_operations = 0;
    std::array<uint64_t, kSampleWindow> rates{};
    uint64_t rate_sum = 0;
    uint64_t published_rate = 0;
    uint64_t memory_peak = 0;
    size_t rate_cursor = 0;
    size_t rate_count = 0;
};

InfoStatsState g_info_stats;

}  // namespace

uint64_t info_stats_sample_ops(uint64_t operations) {
    const uint64_t sampled_ns = now_ns();
    std::lock_guard<std::mutex> lock(g_info_stats.mu);
    if (!g_info_stats.last_sample_ns) {
        g_info_stats.last_sample_ns = sampled_ns;
        g_info_stats.last_operations = operations;
        return 0;
    }
    const uint64_t elapsed = sampled_ns - g_info_stats.last_sample_ns;
    if (elapsed < kSampleMinNs) return g_info_stats.published_rate;

    const uint64_t delta = operations >= g_info_stats.last_operations
        ? operations - g_info_stats.last_operations : 0;
    const uint64_t rate = static_cast<uint64_t>(
        (static_cast<unsigned __int128>(delta) * 1000000000ull) / elapsed);
    if (g_info_stats.rate_count == kSampleWindow)
        g_info_stats.rate_sum -= g_info_stats.rates[g_info_stats.rate_cursor];
    else
        g_info_stats.rate_count++;
    g_info_stats.rates[g_info_stats.rate_cursor] = rate;
    g_info_stats.rate_sum += rate;
    g_info_stats.rate_cursor = (g_info_stats.rate_cursor + 1) % kSampleWindow;
    g_info_stats.published_rate = g_info_stats.rate_sum / g_info_stats.rate_count;
    g_info_stats.last_sample_ns = sampled_ns;
    g_info_stats.last_operations = operations;
    return g_info_stats.published_rate;
}

uint64_t info_stats_observe_memory(uint64_t object_bytes) {
    std::lock_guard<std::mutex> lock(g_info_stats.mu);
    g_info_stats.memory_peak = std::max(g_info_stats.memory_peak, object_bytes);
    return g_info_stats.memory_peak;
}

void info_stats_reset(uint64_t operations, uint64_t object_bytes) {
    std::lock_guard<std::mutex> lock(g_info_stats.mu);
    g_info_stats.last_sample_ns = now_ns();
    g_info_stats.last_operations = operations;
    g_info_stats.rates.fill(0);
    g_info_stats.rate_sum = 0;
    g_info_stats.published_rate = 0;
    g_info_stats.memory_peak = object_bytes;
    g_info_stats.rate_cursor = 0;
    g_info_stats.rate_count = 0;
}

}  // namespace tomo

#pragma once

#include <cstdint>

namespace tomo {

// INFO-only process state. The request paths supply already-existing per-IO counters; sampling,
// division, locking and the memory high-water mark all stay on the cold introspection path.
uint64_t info_stats_sample_ops(uint64_t operations);
uint64_t info_stats_observe_memory(uint64_t object_bytes);
void info_stats_reset(uint64_t operations, uint64_t object_bytes);

}  // namespace tomo

// hll.h -- Redis-compatible HyperLogLog string-image operations.
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "../base/slice.h"

namespace tomo::hll {

inline constexpr uint32_t kRegisters = 1u << 14;
inline constexpr uint32_t kDenseBytes = kRegisters * 6 / 8;
inline constexpr uint32_t kHeaderBytes = 16;
inline constexpr uint32_t kDenseSize = kHeaderBytes + kDenseBytes;

// Set once before worker creation; all HLL mutation/replay/merge callers share this boot policy.
void configure_sparse_max_bytes(uint32_t value);

// Header validation is deliberately separate from sparse-stream validation. Redis makes the same
// distinction: a bad HLL string gets WRONGTYPE, while a valid HYLL header whose opcode stream is
// corrupt gets INVALIDOBJ when an operation actually decodes it.
bool header_valid(Slice image);
bool is_dense(Slice image);

// PFDEBUG's owner-local views. Conversion validates that a sparse opcode stream covers exactly all
// registers; DECODE deliberately formats the opcodes that are physically present, matching the
// diagnostic command even when a later GETREG/TODENSE rejects an incomplete stream.
bool make_dense(std::string& image);
bool decode_sparse(Slice image, std::string& decoded);

std::string create_sparse();
void invalidate_cache(std::string& image);
bool cache_valid(Slice image);
uint64_t cached_count(Slice image);
void set_cached_count(std::string& image, uint64_t count);

// Returns 1 when a register changed, 0 for a duplicate/no-op, and -1 for a corrupt sparse image.
int add(std::string& image, Slice element);

// Count one validated image. `corrupt` is set only for an invalid sparse opcode stream.
uint64_t count(Slice image, bool& corrupt);

// Coordinator operations use one raw byte per register. Missing keys are skipped by the caller.
bool merge_registers(Slice image, std::array<uint8_t, kRegisters>& maximum);
uint64_t count_registers(const std::array<uint8_t, kRegisters>& registers);

// Shape PFMERGE exactly like Redis: begin with the destination's current image (or a fresh sparse
// image), force dense if any input was dense, otherwise update registers in ascending order through
// the sparse mutation algorithm. The caller must already have merged and validated every input.
bool merge_result(Slice destination, bool destination_present,
                  const std::array<uint8_t, kRegisters>& maximum,
                  bool any_dense, std::string& result);

}  // namespace tomo::hll

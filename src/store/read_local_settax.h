// read_local_settax.h -- internal bake-off selector for armed plain-SET allocation tax.
//
// This is deliberately compile-time rather than a CONFIG/command-line knob: production and every
// unqualified build retain the immutable read-local path (0), while an owner bake-off can select
// exactly one experimental implementation with
//   -DTOMO_READ_LOCAL_SET_TAX_VARIANT=1   (shard-sequence in-place overwrite)
//   -DTOMO_READ_LOCAL_SET_TAX_VARIANT=3   (per-object-sequence in-place overwrite)
//
// Selector 2 (post-QSBR same-class recycling) is RETIRED: it won its bake-off and was hardcoded.
// Armed writes now draw their block from, and return it to, the shard's own post-grace block cache
// unconditionally (FlatStore::read_local_cache_take/put, NOTES-RECYCLE.md), so there is no longer a
// build that recycles and a build that does not. Selecting 2 is a compile error rather than a
// silent second recycler competing with the shipped one.
#pragma once
#include <cstdint>

#ifndef TOMO_READ_LOCAL_SET_TAX_VARIANT
#define TOMO_READ_LOCAL_SET_TAX_VARIANT 0
#endif

namespace tomo {

enum class ReadLocalSetTaxVariant : uint8_t {
    Off = 0,
    SequenceOverwrite = 1,
    ObjectSequenceOverwrite = 3,
};

static_assert(TOMO_READ_LOCAL_SET_TAX_VARIANT == 0 ||
              TOMO_READ_LOCAL_SET_TAX_VARIANT == 1 ||
              TOMO_READ_LOCAL_SET_TAX_VARIANT == 3,
              "TOMO_READ_LOCAL_SET_TAX_VARIANT must be 0 (off), 1 (shard sequence), or "
              "3 (object sequence); selector 2 shipped as the unconditional block cache");

inline constexpr ReadLocalSetTaxVariant kReadLocalSetTaxVariant =
    static_cast<ReadLocalSetTaxVariant>(TOMO_READ_LOCAL_SET_TAX_VARIANT);

inline constexpr bool kReadLocalSetTaxAtomicRaw =
    kReadLocalSetTaxVariant == ReadLocalSetTaxVariant::SequenceOverwrite ||
    kReadLocalSetTaxVariant == ReadLocalSetTaxVariant::ObjectSequenceOverwrite;

struct ReadLocalSetTaxStats;

#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
// Temporary owner-local attribution for the armed SET bake-off. These are intentionally plain
// counters: the owning fused thread is their sole writer and INFO already takes an exceptional
// cross-thread snapshot of the surrounding read-local telemetry.
struct ReadLocalSetTaxStats {
    // INFO derives overwrite attempts from the one mutually exclusive outcome increment. That
    // keeps a successful selector-3 overwrite to one diagnostic write instead of attempts+hits.
    uint64_t overwrite_hits = 0;
    uint64_t reject_missing = 0;
    uint64_t reject_encoding = 0;
    uint64_t reject_ttl = 0;
    uint64_t reject_oversize = 0;
    uint64_t reject_size_class = 0;
    uint64_t reject_borrowed = 0;
    uint64_t reject_sequence_saturated = 0;
    uint64_t overwrite_maxmemory_oom = 0;

    uint64_t init_raw_calls = 0;
    uint64_t init_int_calls = 0;
    uint64_t init_extern_calls = 0;
    uint64_t init_key_bytes = 0;
    uint64_t init_value_bytes = 0;
    uint64_t init_cell_prepare_calls = 0;
    uint64_t fresh_allocation_attempts = 0;
    uint64_t accounting_add_calls = 0;
    uint64_t accounting_sub_calls = 0;
    uint64_t accounting_bytes = 0;
    uint64_t slot_replacements = 0;
    uint64_t expire_erases = 0;

    // INFO derives recycler hit rate as acquire_hits / acquire_attempts.
    uint64_t recycle_acquire_attempts = 0;
    uint64_t recycle_acquire_hits = 0;
    uint64_t recycle_acquire_ineligible = 0;
    uint64_t recycle_acquire_empty = 0;
    uint64_t recycle_return_attempts = 0;
    uint64_t recycle_return_accepted = 0;
    uint64_t recycle_return_ineligible = 0;
    uint64_t recycle_return_limited = 0;
    uint64_t recycle_pool_nodes = 0;
    uint64_t recycle_pool_max_owner_nodes = 0;
    uint64_t recycle_capacity_evals = 0;
    uint64_t recycle_candidate_attempts = 0;
    uint64_t recycle_reject_not_string = 0;
    uint64_t recycle_reject_encoding = 0;
    uint64_t recycle_reject_borrowed = 0;
    uint64_t recycle_atomic_pool_accepts = 0;

    uint64_t qsbr_deferrals = 0;
    uint64_t qsbr_object_deferrals = 0;
    uint64_t qsbr_table_deferrals = 0;
    uint64_t qsbr_depth = 0;
    uint64_t qsbr_max_owner_depth = 0;
    // Average enqueue depth is depth_sum / depth_samples; depth itself is the live gauge.
    uint64_t qsbr_depth_samples = 0;
    uint64_t qsbr_depth_sum = 0;
    uint64_t qsbr_seals = 0;
    uint64_t qsbr_sealed_entries = 0;
    uint64_t qsbr_grace_scans = 0;
    uint64_t qsbr_participant_loads = 0;
    uint64_t qsbr_zero_progress_scans = 0;
    uint64_t qsbr_reclaims = 0;
    uint64_t qsbr_forced_graces = 0;
    uint64_t qsbr_forced_yields = 0;

    uint64_t object_sequence_retries = 0;

    void add(const ReadLocalSetTaxStats& other) {
#define TOMO_SETTAX_ADD(field) field += other.field
        TOMO_SETTAX_ADD(overwrite_hits);
        TOMO_SETTAX_ADD(reject_missing);
        TOMO_SETTAX_ADD(reject_encoding);
        TOMO_SETTAX_ADD(reject_ttl);
        TOMO_SETTAX_ADD(reject_oversize);
        TOMO_SETTAX_ADD(reject_size_class);
        TOMO_SETTAX_ADD(reject_borrowed);
        TOMO_SETTAX_ADD(reject_sequence_saturated);
        TOMO_SETTAX_ADD(overwrite_maxmemory_oom);
        TOMO_SETTAX_ADD(init_raw_calls);
        TOMO_SETTAX_ADD(init_int_calls);
        TOMO_SETTAX_ADD(init_extern_calls);
        TOMO_SETTAX_ADD(init_key_bytes);
        TOMO_SETTAX_ADD(init_value_bytes);
        TOMO_SETTAX_ADD(init_cell_prepare_calls);
        TOMO_SETTAX_ADD(fresh_allocation_attempts);
        TOMO_SETTAX_ADD(accounting_add_calls);
        TOMO_SETTAX_ADD(accounting_sub_calls);
        TOMO_SETTAX_ADD(accounting_bytes);
        TOMO_SETTAX_ADD(slot_replacements);
        TOMO_SETTAX_ADD(expire_erases);
        TOMO_SETTAX_ADD(recycle_acquire_attempts);
        TOMO_SETTAX_ADD(recycle_acquire_hits);
        TOMO_SETTAX_ADD(recycle_acquire_ineligible);
        TOMO_SETTAX_ADD(recycle_acquire_empty);
        TOMO_SETTAX_ADD(recycle_return_attempts);
        TOMO_SETTAX_ADD(recycle_return_accepted);
        TOMO_SETTAX_ADD(recycle_return_ineligible);
        TOMO_SETTAX_ADD(recycle_return_limited);
        TOMO_SETTAX_ADD(recycle_pool_nodes);
        if (recycle_pool_max_owner_nodes < other.recycle_pool_max_owner_nodes)
            recycle_pool_max_owner_nodes = other.recycle_pool_max_owner_nodes;
        TOMO_SETTAX_ADD(recycle_capacity_evals);
        TOMO_SETTAX_ADD(recycle_candidate_attempts);
        TOMO_SETTAX_ADD(recycle_reject_not_string);
        TOMO_SETTAX_ADD(recycle_reject_encoding);
        TOMO_SETTAX_ADD(recycle_reject_borrowed);
        TOMO_SETTAX_ADD(recycle_atomic_pool_accepts);
        TOMO_SETTAX_ADD(qsbr_deferrals);
        TOMO_SETTAX_ADD(qsbr_object_deferrals);
        TOMO_SETTAX_ADD(qsbr_table_deferrals);
        TOMO_SETTAX_ADD(qsbr_depth);
        if (qsbr_max_owner_depth < other.qsbr_max_owner_depth)
            qsbr_max_owner_depth = other.qsbr_max_owner_depth;
        TOMO_SETTAX_ADD(qsbr_depth_samples);
        TOMO_SETTAX_ADD(qsbr_depth_sum);
        TOMO_SETTAX_ADD(qsbr_seals);
        TOMO_SETTAX_ADD(qsbr_sealed_entries);
        TOMO_SETTAX_ADD(qsbr_grace_scans);
        TOMO_SETTAX_ADD(qsbr_participant_loads);
        TOMO_SETTAX_ADD(qsbr_zero_progress_scans);
        TOMO_SETTAX_ADD(qsbr_reclaims);
        TOMO_SETTAX_ADD(qsbr_forced_graces);
        TOMO_SETTAX_ADD(qsbr_forced_yields);
        TOMO_SETTAX_ADD(object_sequence_retries);
#undef TOMO_SETTAX_ADD
    }
};
#endif

}  // namespace tomo

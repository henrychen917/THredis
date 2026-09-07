// atomic_admission.h -- exact atomic-window accounting with bounded admission and retirement.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace tomo {

class AtomicAdmissionCredits {
public:
    enum class Attempt { Admitted, Full, Contended };

    void init(uint32_t window) { state_.store(pack(window, 0), std::memory_order_relaxed); }
    uint32_t window() const { return limit(state_.load(std::memory_order_acquire)); }
    uint32_t active() const { return used(state_.load(std::memory_order_acquire)); }

    bool can_admit() const { return has_room(state_.load(std::memory_order_acquire)); }

    Attempt try_admit() {
        uint64_t state = state_.load(std::memory_order_acquire);
        if (!has_room(state)) return Attempt::Full;
        // A failed reservation returns to the existing dispatch backpressure path. Strong CAS
        // avoids spurious refusal; a competing admission, retirement or CONFIG can win only this
        // attempt, never make the connection-owning thread spin on its behalf.
        return state_.compare_exchange_strong(state, state + 1, std::memory_order_acq_rel,
                                              std::memory_order_relaxed)
            ? Attempt::Admitted : Attempt::Contended;
    }

    void retire() {
        if (!used(state_.fetch_sub(1, std::memory_order_acq_rel))) std::abort();
    }

    void set_window(uint32_t window) {
        // One configuration writer: Server calls this only from shard 0's CONFIG task. Concurrent
        // admissions/retirements change only the low half, so an unsigned high-half delta installs
        // the new limit in one RMW while preserving their exact count. No intermediate unlimited
        // window, active-group snapshot, odd generation, writer retry or reader acknowledgement.
        const uint32_t old = limit(state_.load(std::memory_order_relaxed));
        const uint32_t delta = window - old;
        state_.fetch_add(uint64_t{delta} << 32, std::memory_order_acq_rel);
    }

    uint32_t pool() const {
        const uint64_t state = state_.load(std::memory_order_acquire);
        return limit(state) > used(state) ? limit(state) - used(state) : 0;
    }
    uint32_t debt() const {
        const uint64_t state = state_.load(std::memory_order_acquire);
        return limit(state) && used(state) > limit(state) ? used(state) - limit(state) : 0;
    }

private:
    static uint64_t pack(uint32_t window, uint32_t active) {
        return (uint64_t{window} << 32) | active;
    }
    static uint32_t used(uint64_t state) { return static_cast<uint32_t>(state); }
    static uint32_t limit(uint64_t state) { return static_cast<uint32_t>(state >> 32); }
    static bool has_room(uint64_t state) {
        // Unlimited still cannot overflow the representation. Exhaustion uses backpressure.
        return used(state) != UINT32_MAX && (!limit(state) || used(state) < limit(state));
    }
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    std::atomic<uint64_t> state_{pack(256, 0)};
};
static_assert(sizeof(AtomicAdmissionCredits) == sizeof(uint64_t));

} // namespace tomo

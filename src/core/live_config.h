// live_config.h -- coherent configuration publication without reader retries or reclamation.
#pragma once
#include <atomic>
#include <cstdint>
#include "config.h"

namespace tomo {

struct LiveConfigSnapshot {
    uint64_t version;
    uint64_t maxmemory;
    MaxmemoryPolicy policy;
    uint32_t samples;
    uint32_t notify_events;
    bool tracking_armed;
    int64_t slowlog_log_slower_than;
    uint32_t latency_monitor_threshold;
    bool save_armed;
    uint64_t proto_max_bulk_len;
    uint32_t debug_fanout_defer_us;
};

struct ClientLimitsConfigSnapshot {
    uint64_t version = 0;
    uint32_t timeout = 0;
    ClientBufferLimit normal{};
    ClientBufferLimit pubsub{};
};

struct LiveConfigValues {
    LiveConfigSnapshot live{};
    ClientLimitsConfigSnapshot clients{};
};

// Exactly one consumer, identified by physical ThreadCtx id, and one serialized CONFIG producer.
// They each own one slot; an atomic exchange transfers ownership of the third (middle) slot.
// The producer never touches the consumer's slot, even if that worker stops indefinitely.
// No slot is freed, no reader announces a grace period, and no operation retries an exchange.
class LiveConfigMailbox {
public:
    void init(const LiveConfigValues& initial) {
        for (auto& slot : slots_) slot = {initial, initial};
    }

    void publish(const LiveConfigValues& previous, const LiveConfigValues& next) {
        slots_[back_] = {previous, next};
        back_ = middle_.exchange(back_ | kDirty, std::memory_order_acq_rel) & kIndex;
    }

    const LiveConfigValues& read(const std::atomic<uint64_t>& version) {
        if (middle_.load(std::memory_order_acquire) & kDirty)
            front_ = middle_.exchange(front_, std::memory_order_acq_rel) & kIndex;
        // CONFIG stages every mailbox before publishing the even version. During that staging
        // interval choose the preceding committed value, not the partly published new version.
        // Load AFTER acquiring the slot: its publication also orders CONFIG's odd version store,
        // so this load cannot precede the commit of slot.previous. Keep both values even if this
        // consumer acquires an uncommitted slot and the writer then stops before committing it.
        const uint64_t observed = version.load(std::memory_order_acquire);
        const auto& slot = slots_[front_];
        return observed == slot.next.live.version - 1 ? slot.previous : slot.next;
    }

private:
    static constexpr uint32_t kDirty = 4;
    static constexpr uint32_t kIndex = 3;
    static_assert(std::atomic<uint32_t>::is_always_lock_free);
    struct alignas(64) Publication {
        LiveConfigValues previous;
        LiveConfigValues next;
    };
    Publication slots_[3]{};
    alignas(64) uint32_t front_ = 0;                  // consumer only
    alignas(64) uint32_t back_ = 1;                   // serialized producer only
    alignas(64) std::atomic<uint32_t> middle_{2};
};

} // namespace tomo

// weighted_lb.h -- the one placement policy shared by FLIP and the continuous controller.
//
// Demand is primary. An optional second load vector (bytes for buckets) is compared
// lexicographically, never blended into demand. Count remains a hard invariant: every target
// finishes with floor/ceil item count, including the all-zero boot window. Items are considered
// largest-first and placed on the least loaded eligible target; current ownership and target id
// only break exact ties. This is deterministic, allocation-only code and never runs on a request
// path.
#pragma once
#include <atomic>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace tomo {

// Internal LB policy, allocated only by --lb 1. One decision spans the three sustained
// controller ticks; sampling targets a fixed number of key visits over that whole window.
// Executors latch the rate on their existing census beat and weight each sample by that rate,
// so a rate change or owner migration cannot reinterpret counters collected at another rate.
struct LbAutotune {
    static constexpr uint32_t kTickMs = 1000;
    static constexpr uint32_t kDecisionTicks = 3;
    static constexpr uint32_t kWindowMs = kTickMs * kDecisionTicks;
    static constexpr uint32_t kSamplesPerDecision = 4096;
    static constexpr uint64_t kMoveTimeoutNs = 5ull * 1000 * 1000 * 1000;

    std::atomic<uint32_t> sample_rate{1}; // bootstrap until the first completed traffic window
    uint64_t last_fold_ns = 0;           // protected by Server::lb_signal_mu_

    static uint32_t sample_every(double visits, double elapsed_ms, uint32_t window_ms) {
        if (!(elapsed_ms > 0)) return 1;
        const double rate = std::ceil(visits * window_ms / elapsed_ms / kSamplesPerDecision);
        return static_cast<uint32_t>(std::clamp(rate, 1.0, double(UINT32_MAX)));
    }

    void observe_visits(uint64_t visits, uint64_t now) {
        if (last_fold_ns && now > last_fold_ns) {
            sample_rate.store(sample_every(visits, double(now - last_fold_ns) / 1000000.0,
                                           kWindowMs),
                              std::memory_order_relaxed);
        }
        last_fold_ns = now;
    }

    struct QuietJitter {
        double previous = 0;
        double jitter = 0;
        uint32_t windows = 0;

        bool observe(double value) {
            const double delta = std::abs(value - previous);
            // Learn adjacent-window jitter before making a decision. Once learned, excursions
            // cannot widen their own admission band: only quiet, in-band changes update it.
            if (windows && (windows <= kDecisionTicks || delta <= 2 * jitter))
                jitter = windows == 1 ? delta : 0.25 * delta + 0.75 * jitter;
            previous = value;
            windows = std::min(windows + 1, kDecisionTicks + 1);
            return windows > kDecisionTicks;
        }
        double band() const { return 2 * jitter; }
    };
    QuietJitter key_jitter, client_jitter; // controller writer only

    // Completion records include drain and install, not just the pointer exchange. These two
    // atomics are cold: one writer per completed movement, never an operation-path timestamp.
    std::atomic<uint64_t> transfer_ns{0};
    std::atomic<uint64_t> transfers{0};
    void note_transfer(uint64_t elapsed_ns, uint32_t count) {
        if (!count) return;
        transfer_ns.fetch_add(std::max<uint64_t>(elapsed_ns, 1), std::memory_order_relaxed);
        transfers.fetch_add(count, std::memory_order_release);
    }
    uint64_t move_cost_ns() const {
        const uint64_t count = transfers.load(std::memory_order_acquire);
        return count ? std::max<uint64_t>(1, transfer_ns.load(std::memory_order_relaxed) / count)
                     : 0;
    }
    uint32_t move_cap(uint32_t candidates) const {
        const uint64_t cost = move_cost_ns();
        // Bootstrap with one move. Thereafter admit at most one decision tick's share of the
        // measured transfer capacity, amortized across the sustained decision window.
        const uint64_t cap = cost ? (uint64_t{kTickMs} * 1000000 / cost) / kDecisionTicks : 1;
        return static_cast<uint32_t>(std::min<uint64_t>(candidates, std::max<uint64_t>(1, cap)));
    }
    uint64_t cooldown_ms() const {
        // Round up to the observation cadence; zero measured cost never disables movement.
        const uint64_t cost = move_cost_ns();
        const uint64_t ticks = (cost * kDecisionTicks + uint64_t{kTickMs} * 1000000 - 1) /
                               (uint64_t{kTickMs} * 1000000);
        return std::max<uint64_t>(1, ticks) * kTickMs;
    }
};

struct WeightedLbItem {
    uint64_t id = 0;
    uint32_t owner = 0;
    double weight = 0;
    bool pinned = false;
    // A lexicographic second objective, never blended into demand. Bucket placement uses bytes;
    // client placement leaves it zero.
    double secondary = 0;
};

struct WeightedLbAssignment {
    uint64_t id = 0;
    uint32_t source = 0;
    uint32_t destination = 0;
    double weight = 0;
    double secondary = 0;
};

struct WeightedLbMoveChoice {
    uint32_t item_index = UINT32_MAX;
    uint32_t source = UINT32_MAX;
    uint32_t destination = UINT32_MAX;
    double before_weight_spread = 0;
    double after_weight_spread = 0;
    double before_secondary_spread = 0;
    double after_secondary_spread = 0;
};

// The incremental face of the same placement policy. Pinned items contribute load but cannot be
// selected (the controller uses that for cooldown and already-selected candidates). A demand move
// must strictly improve demand spread. A secondary-only move must not worsen demand and must
// strictly improve the independent secondary spread. Count spread and stable ids break exact ties.
inline bool weighted_lb_best_incremental_move(const std::vector<WeightedLbItem>& items,
                                              const std::vector<uint32_t>& targets,
                                              bool demand_hot, bool secondary_hot,
                                              WeightedLbMoveChoice& choice) {
    choice = {};
    if (targets.size() < 2 || (!demand_hot && !secondary_hot)) return false;
    std::vector<double> load(targets.size(), 0);
    std::vector<double> secondary_load(targets.size(), 0);
    std::vector<uint32_t> count(targets.size(), 0);
    auto target_index = [&](uint32_t tid) {
        for (uint32_t i = 0; i < targets.size(); i++) if (targets[i] == tid) return i;
        return UINT32_MAX;
    };
    for (const WeightedLbItem& item : items) {
        const uint32_t owner = target_index(item.owner);
        if (owner == UINT32_MAX) return false;
        load[owner] += std::max(0.0, item.weight);
        secondary_load[owner] += std::max(0.0, item.secondary);
        count[owner]++;
    }
    auto spread = [](const auto& values) {
        const auto [lo, hi] = std::minmax_element(values.begin(), values.end());
        return values.empty() ? 0.0 : static_cast<double>(*hi - *lo);
    };
    const double old_weight = spread(load);
    const double old_secondary = spread(secondary_load);
    choice.before_weight_spread = old_weight;
    choice.before_secondary_spread = old_secondary;
    double best_weight = old_weight, best_secondary = old_secondary;
    uint32_t best_count = UINT32_MAX;
    for (uint32_t item_index = 0; item_index < items.size(); item_index++) {
        const WeightedLbItem& item = items[item_index];
        if (item.pinned) continue;
        const uint32_t source = target_index(item.owner);
        for (uint32_t destination = 0; destination < targets.size(); destination++) {
            if (destination == source) continue;
            load[source] -= std::max(0.0, item.weight);
            load[destination] += std::max(0.0, item.weight);
            secondary_load[source] -= std::max(0.0, item.secondary);
            secondary_load[destination] += std::max(0.0, item.secondary);
            count[source]--;
            count[destination]++;
            const double next_weight = spread(load);
            const double next_secondary = spread(secondary_load);
            const auto [count_lo, count_hi] = std::minmax_element(count.begin(), count.end());
            const uint32_t next_count = *count_hi - *count_lo;
            count[destination]--;
            count[source]++;
            secondary_load[destination] -= std::max(0.0, item.secondary);
            secondary_load[source] += std::max(0.0, item.secondary);
            load[destination] -= std::max(0.0, item.weight);
            load[source] += std::max(0.0, item.weight);

            const bool improves = demand_hot
                ? next_weight + 1e-9 < old_weight
                : next_weight <= old_weight + 1e-9 &&
                  next_secondary + 1e-9 < old_secondary;
            if (!improves) continue;
            if (choice.item_index == UINT32_MAX || next_weight < best_weight ||
                (next_weight == best_weight && next_secondary < best_secondary) ||
                (next_weight == best_weight && next_secondary == best_secondary &&
                 next_count < best_count) ||
                (next_weight == best_weight && next_secondary == best_secondary &&
                 next_count == best_count && item.id < items[choice.item_index].id) ||
                (next_weight == best_weight && next_secondary == best_secondary &&
                 next_count == best_count && item.id == items[choice.item_index].id &&
                 targets[destination] < choice.destination)) {
                choice.item_index = item_index;
                choice.source = item.owner;
                choice.destination = targets[destination];
                choice.after_weight_spread = next_weight;
                choice.after_secondary_spread = next_secondary;
                best_weight = next_weight;
                best_secondary = next_secondary;
                best_count = next_count;
            }
        }
    }
    return choice.item_index != UINT32_MAX;
}

inline bool weighted_lb_partition(const std::vector<WeightedLbItem>& items,
                                  const std::vector<uint32_t>& targets,
                                  std::vector<WeightedLbAssignment>& out) {
    out.clear();
    if (targets.empty()) return items.empty();
    const uint32_t low = static_cast<uint32_t>(items.size() / targets.size());
    const uint32_t high = low + (items.size() % targets.size() != 0);
    std::vector<uint32_t> count(targets.size(), 0);
    std::vector<double> load(targets.size(), 0);
    std::vector<double> secondary_load(targets.size(), 0);
    std::vector<uint32_t> destination(items.size(), UINT32_MAX);
    auto target_index = [&](uint32_t tid) {
        for (uint32_t i = 0; i < targets.size(); i++) if (targets[i] == tid) return i;
        return UINT32_MAX;
    };

    for (uint32_t i = 0; i < items.size(); i++) {
        if (!items[i].pinned) continue;
        const uint32_t at = target_index(items[i].owner);
        if (at == UINT32_MAX || count[at] == high) return false;
        destination[i] = at;
        count[at]++;
        load[at] += std::max(0.0, items[i].weight);
        secondary_load[at] += std::max(0.0, items[i].secondary);
    }

    std::vector<uint32_t> order;
    order.reserve(items.size());
    for (uint32_t i = 0; i < items.size(); i++) if (!items[i].pinned) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        if (items[a].weight != items[b].weight) return items[a].weight > items[b].weight;
        if (items[a].secondary != items[b].secondary)
            return items[a].secondary > items[b].secondary;
        return items[a].id < items[b].id;
    });

    for (uint32_t ordinal = 0; ordinal < order.size(); ordinal++) {
        uint32_t deficit = 0;
        for (uint32_t c : count) if (c < low) deficit += low - c;
        const uint32_t remaining = static_cast<uint32_t>(order.size() - ordinal);
        const bool must_fill_low = remaining == deficit;
        const uint32_t item = order[ordinal];
        uint32_t best = UINT32_MAX;
        for (uint32_t t = 0; t < targets.size(); t++) {
            if (count[t] >= high || (must_fill_low && count[t] >= low)) continue;
            if (best == UINT32_MAX || load[t] < load[best] ||
                (load[t] == load[best] && secondary_load[t] < secondary_load[best]) ||
                (load[t] == load[best] && secondary_load[t] == secondary_load[best] &&
                 count[t] < count[best]) ||
                (load[t] == load[best] && secondary_load[t] == secondary_load[best] &&
                 count[t] == count[best] &&
                 targets[t] == items[item].owner && targets[best] != items[item].owner) ||
                (load[t] == load[best] && secondary_load[t] == secondary_load[best] &&
                 count[t] == count[best] &&
                 (targets[t] == items[item].owner) == (targets[best] == items[item].owner) &&
                 targets[t] < targets[best])) best = t;
        }
        if (best == UINT32_MAX) return false;
        destination[item] = best;
        count[best]++;
        load[best] += std::max(0.0, items[item].weight);
        secondary_load[best] += std::max(0.0, items[item].secondary);
    }

    for (uint32_t c : count) if (c < low || c > high) return false;

    // Ties keep the incumbent -- globally, not just per greedy step. The greedy fill above cannot
    // see that a zero-move assignment with the same spread exists (once an item's incumbent has
    // absorbed earlier items it looks "fuller" and the item is shipped elsewhere), so a balanced
    // no-op FLIP reshuffled a hundred buckets for nothing. If assigning every item to its current
    // owner is feasible under the same count bounds and no worse in weight or secondary spread
    // than the greedy plan, prefer it wholesale: locality is free when balance ties.
    {
        std::vector<uint32_t> inc_count(targets.size(), 0);
        std::vector<double> inc_load(targets.size(), 0);
        std::vector<double> inc_secondary(targets.size(), 0);
        bool feasible = true;
        for (uint32_t i = 0; i < items.size() && feasible; i++) {
            const uint32_t at = target_index(items[i].owner);
            if (at == UINT32_MAX) { feasible = false; break; }
            inc_count[at]++;
            inc_load[at] += std::max(0.0, items[i].weight);
            inc_secondary[at] += std::max(0.0, items[i].secondary);
        }
        if (feasible)
            for (uint32_t c : inc_count) if (c < low || c > high) { feasible = false; break; }
        if (feasible) {
            auto spread = [](const std::vector<double>& v) {
                const auto [lo, hi] = std::minmax_element(v.begin(), v.end());
                return v.empty() ? 0.0 : *hi - *lo;
            };
            if (spread(inc_load) <= spread(load) + 1e-9 &&
                spread(inc_secondary) <= spread(secondary_load) + 1e-9) {
                for (uint32_t i = 0; i < items.size(); i++)
                    destination[i] = target_index(items[i].owner);
            }
        }
    }
    out.reserve(items.size());
    for (uint32_t i = 0; i < items.size(); i++) {
        if (destination[i] == UINT32_MAX) return false;
        out.push_back(WeightedLbAssignment{
            items[i].id, items[i].owner, targets[destination[i]],
            std::max(0.0, items[i].weight), std::max(0.0, items[i].secondary)});
    }
    return true;
}

}  // namespace tomo

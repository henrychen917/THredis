// thread.h — one server thread, the role it is currently playing, and its inbound channels.
//
// ROLE IS STATE, NOT TYPE. A thread is an IO thread or a worker because of a field, not because of
// its class. That is deliberate: the io:ex split is a runtime decision and the optimal split differs
// by workload (measured: ~16:1 io:ex at p1, scaling as 16:1/depth as pipelining deepens). Encoding
// the role in the type system would make the one thing we most need to change at runtime the one
// thing we cannot.
//
// EVERY THREAD HAS THE SAME THREE INBOUND CHANNELS, whatever its role. What arrives on them differs;
// their shape, units and wake semantics do not. That uniformity is what lets a flip/LB controller
// read all three loops through one interface instead of special-casing each:
//
//   task_in_[p]     from producer p    IO -> EX   a parsed op to execute
//   client_in_[p]   from producer p    EX -> IO   "you have completed ops to retire"
//   release_in_[p]  from producer p    IO -> EX   a FlatStore value borrow is off the wire
//
// A thread's current role determines which channel families are active; keeping the arrays uniform
// means a future role change does not rewire the producer mesh.
//
// One channel PER PRODUCER, which is what keeps each ring genuinely SPSC. The alternative is one
// MPSC inbox per consumer, costing an atomic RMW per push from every producer; the fork measured
// this handoff as instruction volume rather than stalls, so removing the RMW is the direct lever.
//
//   TODO(flip): converting Ex -> Io must first drain every inbox and hand its shards to another
//   worker; converting Io -> Ex must first migrate its connections and reach rob.quiesced() on each.
//   A conversion that loses or duplicates a thread breaks pool accounting — a real P0 in the fork.
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>
#include "shard.h"
#include "signal.h"
#include "pubsub_event.h"
#include "../base/topology.h"

namespace tomo {

class Client;
class Ring;
class WbEngine;

// Heap-side multi-shard state is deliberately opaque here.  Its vectors, result slots and phase
// bookkeeping live in cmd/xshard so neither Task nor the footprint-locked Op grows.
struct ScatterState;

inline constexpr uint32_t kMaxThreads = 128;
inline constexpr uint32_t kInboxSlots = 1024;

enum class Role : uint8_t { Idle = 0, Ifid = 1, Ex = 2 };

// Admission credits are leased to the connection-owning IO. Plain fields have exactly one writer;
// CONFIG/INFO consult only the published mirrors. No field names a shard or shard-side structure.
struct alignas(64) AtomicAdmissionLease {
    uint64_t generation = 0;
    uint32_t available = 0;
    uint32_t active = 0;
    std::atomic<uint32_t> published_active{0};
    std::atomic<uint32_t> reconfig_carry{0};
};

// What travels on task_in_. A handle rather than a raw Op*: the worker resolves it through the
// client's ROB, so a recycled slot cannot be reached through a stale pointer. The client itself
// cannot be torn down while any op is in flight — that is the quiescence fence.
struct Task {
    Client*  client = nullptr;
    uint64_t op_id  = 0;
    int32_t  shard  = -1;       // -1 means use Op::shard (the ordinary single-shard path)
    ScatterState* scatter = nullptr;
};

struct BorrowRelease {
    int32_t     shard = -1;
    const char* ptr   = nullptr;
};

using TaskChan   = Channel<Task, kInboxSlots>;
using ClientChan = Channel<Client*, kInboxSlots>;
using ReleaseChan = Channel<BorrowRelease, kInboxSlots>;

class ThreadCtx {
public:
    ThreadCtx() = default;
    ThreadCtx(const ThreadCtx&) = delete;
    ThreadCtx& operator=(const ThreadCtx&) = delete;

    // `nthreads` is the TOTAL thread count, not the io count: any thread can become a producer
    // through a role change. Sized to the live count rather than kMaxThreads — a fixed 128-wide
    // array would be megabytes per thread, nearly all of it untouched.
    void init(uint32_t id, Role r, uint32_t nthreads) {
        id_ = id;
        role_.store(r, std::memory_order_relaxed);
        nchan_     = nthreads;
        task_in_   = std::make_unique<TaskChan[]>(nthreads);
        client_in_ = std::make_unique<ClientChan[]>(nthreads);
        release_in_ = std::make_unique<ReleaseChan[]>(nthreads);
    }

    void init_command_counts(uint32_t count) {
        command_count_size_ = count;
        command_counts_ = count ? std::make_unique<uint64_t[]>(count) : nullptr;
    }

    uint32_t id()   const { return id_; }
    Role     role() const { return role_.load(std::memory_order_acquire); }

    // Where this thread actually runs. Latched once the thread is pinned and running, because
    // sched_getcpu() before that answers about the wrong cpu. A worker passes domain() to
    // Shard::note_execution so the shard can tell local work from foreign work; that ratio is the
    // signal a later flip/LB controller acts on.
    void latch_placement(const Topology& topo) {
        cpu_    = sched_getcpu();
        domain_ = topo.domain_of(cpu_);
    }
    uint32_t domain() const { return domain_; }

    // The producer is the only writer. INFO's exceptional aggregation reads these cold arrays;
    // ordinary command execution performs one non-atomic increment in thread-private memory.
    void note_command(uint16_t id) {
        if (id < command_count_size_) command_counts_[id]++;
    }
    uint64_t command_calls(uint32_t id) const {
        return id < command_count_size_ ? command_counts_[id] : 0;
    }
    void note_atomic_group() { atomic_groups_++; }
    uint64_t atomic_groups() const { return atomic_groups_; }
    void note_atomic_localfast() { atomic_localfast_++; }
    uint64_t atomic_localfast() const { return atomic_localfast_; }
    // Owner-local: counts how often a whole-owner walker (KEYS / exact DBSIZE / FLUSH) was held
    // behind an older same-connection task parked on this shard.  It is the fired-mechanism proof
    // for the scan-ordering fix, so it must be observable rather than merely believed.
    void note_atomic_scan_hold() { atomic_scan_holds_++; }
    uint64_t atomic_scan_holds() const { return atomic_scan_holds_; }
    AtomicAdmissionLease& atomic_admission_lease() { return atomic_admission_lease_; }
    const AtomicAdmissionLease& atomic_admission_lease() const { return atomic_admission_lease_; }

    // ---- posting (producer side) ---------------------------------------------------------------
    // Push AND flag, in that order. Flagging before the push would let the consumer take the bit,
    // find an empty queue, and clear it while the item is still in flight.
    bool post_task(uint32_t from, const Task& t, Ring& my_ring, LoopSignals& sig) {
        if (!task_in_[from].push(t, sig)) return false;
        // Publish the bit FIRST, wake only if we are the producer that raised it. Order matters more
        // than it looks -- see Channel::push/wake.
        if (task_notify_.set(from)) task_in_[from].wake(my_ring, sig, ring());
        return true;
    }

    // Item 2 (dispatch batching): push without notifying, then one flush per (producer, consumer)
    // pair per parse pass. A 32-deep pipeline used to pay 32 bit-publishes and 32 wake decisions; it
    // now pays 32 ring pushes and ONE of each. Safe against the park race the split reintroduces:
    // a consumer deciding to sleep between our push and our flush re-checks QUEUE DEPTHS in
    // any_inbound(), not just masks -- the depth check exists precisely so no notification scheme
    // has to be perfect.
    bool post_task_quiet(uint32_t from, const Task& t, LoopSignals& sig) {
        return task_in_[from].push(t, sig);
    }
    bool post_tasks_quiet(uint32_t from, const Task* tasks, uint32_t count, LoopSignals& sig) {
        return task_in_[from].push_batch(tasks, count, sig);
    }
    uint32_t task_free_slots(uint32_t from) const {
        return task_in_[from].producer_free_slots();
    }
    void flush_task_notify(uint32_t from, Ring& my_ring, LoopSignals& sig) {
        if (task_notify_.set(from)) task_in_[from].wake(my_ring, sig, ring());
    }
    bool post_client(uint32_t from, Client* c, Ring& my_ring, LoopSignals& sig) {
        if (!client_in_[from].push(c, sig)) return false;
        if (client_notify_.set(from)) client_in_[from].wake(my_ring, sig, ring());
        return true;
    }
    bool post_release(uint32_t from, const BorrowRelease& r, Ring& my_ring, LoopSignals& sig) {
        if (!release_in_[from].push(r, sig)) return false;
        if (release_notify_.set(from)) release_in_[from].wake(my_ring, sig, ring());
        return true;
    }

    // Pub/sub payloads use one cold MPSC inbox per owning IO, but wake through the existing
    // client_in mesh.  `true` means the caller owns posting the single nullptr marker for this
    // non-empty burst.  The mutex is never touched before the first pub/sub command.
    bool post_pubsub_event(PubSubEvent* event) {
        std::lock_guard<std::mutex> lock(pubsub_mu_);
        pubsub_events_.push_back(event);
        if (pubsub_notified_) return false;
        pubsub_notified_ = true;
        return true;
    }

    // Keyless expiry/eviction events originate on EX.  Reserve the ordinary producer lane marker
    // before making the payload visible, and never spin if that SPSC lane is full.  Holding the
    // cold inbox mutex closes the marker-before-payload race with the IO consumer.
    bool post_pubsub_event_nonblocking(uint32_t from, PubSubEvent* event,
                                       Ring& producer_ring, LoopSignals& sig) {
        std::lock_guard<std::mutex> lock(pubsub_mu_);
        if (!pubsub_notified_) {
            if (!post_client(from, nullptr, producer_ring, sig)) return false;
            pubsub_notified_ = true;
        }
        pubsub_events_.push_back(event);
        return true;
    }

    void take_pubsub_events(std::deque<PubSubEvent*>& out) {
        std::lock_guard<std::mutex> lock(pubsub_mu_);
        out.swap(pubsub_events_);
        pubsub_notified_ = false;
    }

    // ---- draining (consumer side) ---------------------------------------------------------------
    // Visits only FLAGGED producers, so cost tracks active producers rather than possible ones. The
    // bits are TAKEN before the channels are drained -- see NotifyMask on why the reverse order
    // loses a push that lands mid-drain.
    template <typename Fn>
    uint32_t drain_tasks(Fn&& fn) {
        uint32_t n = 0;
        for (uint32_t w = 0; w < NotifyMask::kWords; w++) {
            uint64_t bits = task_notify_.take(w);
            while (bits) {
                const uint32_t b = static_cast<uint32_t>(__builtin_ctzll(bits));
                bits &= bits - 1;
                const uint32_t p = w * 64 + b;
                if (p >= nchan_) continue;
                Task t;
                while (task_in_[p].recv(t)) { fn(t); task_in_[p].retire(); n++; }
            }
        }
        return n;
    }

    template <typename Fn>
    uint32_t drain_clients(Fn&& fn) {
        uint32_t n = 0;
        for (uint32_t w = 0; w < NotifyMask::kWords; w++) {
            uint64_t bits = client_notify_.take(w);
            while (bits) {
                const uint32_t b = static_cast<uint32_t>(__builtin_ctzll(bits));
                bits &= bits - 1;
                const uint32_t p = w * 64 + b;
                if (p >= nchan_) continue;
                Client* c = nullptr;
                while (client_in_[p].recv(c)) { fn(c); client_in_[p].retire(); n++; }
            }
        }
        return n;
    }

    template <typename Fn>
    uint32_t drain_releases(Fn&& fn) {
        uint32_t n = 0;
        for (uint32_t w = 0; w < NotifyMask::kWords; w++) {
            uint64_t bits = release_notify_.take(w);
            while (bits) {
                const uint32_t b = static_cast<uint32_t>(__builtin_ctzll(bits));
                bits &= bits - 1;
                const uint32_t p = w * 64 + b;
                if (p >= nchan_) continue;
                BorrowRelease r;
                while (release_in_[p].recv(r)) { fn(r); release_in_[p].retire(); n++; }
            }
        }
        return n;
    }

    // The ring peers poke to wake this thread. Published once at startup, read by producers.
    void  set_ring(Ring* r) { ring_.store(r, std::memory_order_release); }
    Ring* ring() const      { return ring_.load(std::memory_order_acquire); }

    std::vector<Shard*>&  shards()  { return shards_; }    // Ex role
    std::vector<Client*>& clients() { return clients_; }   // Ifid role

    // The single reporting surface. Every loop fills the same fields in the same units, so a
    // controller compares like with like — the failure mode behind every balancer defect in the
    // fork was comparing two quantities that were not the same kind of thing.
    LoopSignals& sig() { return sig_; }

    // IO-only INFO surface. Published once when the owning IoLoop binds its send engine; INFO then
    // sums the engine's single-writer counters with the same exceptional cross-thread read shape
    // used for sig(). Executor threads leave this null because their WbEngine never sends.
    void set_wb_engine(WbEngine* engine) {
        wb_engine_.store(engine, std::memory_order_release);
    }
    WbEngine* wb_engine() const {
        return wb_engine_.load(std::memory_order_acquire);
    }

    // Sample inbound pressure. Called once per loop iteration so depth_sum/depth_samples form a
    // time-average rather than a spot reading, which is too noisy to control on.
    void sample_depth() {
        uint64_t d = 0;
        for (uint32_t i = 0; i < nchan_; i++)
            d += task_in_[i].depth() + client_in_[i].depth() + release_in_[i].depth();
        sig_.depth_sum += d;
        sig_.depth_samples++;
    }

    // Arm/disarm every inbound channel around a block. Producers only pay a wake syscall while
    // these are armed. ALWAYS re-check the channels after arming and before actually blocking:
    // a producer that pushed just before the flag was set would not have woken us.
    void arm_blocked() {
        parked_.store(true, std::memory_order_release);
        for (uint32_t i = 0; i < nchan_; i++) {
            task_in_[i].arm_blocked(); client_in_[i].arm_blocked(); release_in_[i].arm_blocked();
        }
        // The other half of the Dekker pair. Declaring intent to block is a store; the role-specific
        // inbound check that follows is a load of a different location. Without this fence the CPU
        // may hoist that load above the store, and a producer that ran in between sees blocked_ ==
        // false while we see an empty mask. Sleep path only -- it costs nothing where it runs.
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }
    void clear_blocked() {
        parked_.store(false, std::memory_order_release);
        for (uint32_t i = 0; i < nchan_; i++) {
            task_in_[i].clear_blocked(); client_in_[i].clear_blocked(); release_in_[i].clear_blocked();
        }
    }
    // Two loads instead of a scan of every channel. Used to re-check after arming the blocked flag.
    // Asked ONLY on the way to sleep, which is why it can afford to be thorough. The mask is the fast
    // "where do I look" hint for the drain path; here we also look at the queues themselves, because
    // the queue push is the release store the blocked_ protocol was designed to pair with. Checking
    // both means no reasoning about mask staleness can cost us a wakeup -- and a scan of a handful of
    // channels is free for a thread that by definition has nothing else to do.
    // MASK-INDEPENDENT DRAIN. The fast path asks the notify mask "who has work" and visits only
    // those channels, which is what keeps discovery O(active producers) instead of O(threads). The
    // cost of that is a hard dependency on the mask being perfect: a bit that goes missing for any
    // reason leaves a queued item that NOTHING will ever look at again, and the connection waiting on
    // that reply wedges permanently. We measured exactly that -- 209 connections holding a posted,
    // never-served claim while the sender sat idle.
    //
    // So the mask stays the hot path and stops being load-bearing for correctness. A thread about to
    // park scans every channel directly; if the mask told the truth, the scan finds nothing and costs
    // a handful of loads, and it only runs when the thread has already decided it has no work.
    template <typename Fn> uint32_t drain_tasks_unmasked(Fn&& fn) {
        uint32_t n = 0; Task t;
        for (uint32_t p = 0; p < nchan_; p++)
            while (task_in_[p].recv(t)) { fn(t); task_in_[p].retire(); n++; }
        return n;
    }
    template <typename Fn> uint32_t drain_clients_unmasked(Fn&& fn) {
        uint32_t n = 0; Client* c = nullptr;
        for (uint32_t p = 0; p < nchan_; p++)
            while (client_in_[p].recv(c)) { fn(c); client_in_[p].retire(); n++; }
        return n;
    }
    template <typename Fn> uint32_t drain_releases_unmasked(Fn&& fn) {
        uint32_t n = 0; BorrowRelease r;
        for (uint32_t p = 0; p < nchan_; p++)
            while (release_in_[p].recv(r)) { fn(r); release_in_[p].retire(); n++; }
        return n;
    }

    // ---- the wb slot table: this thread AS A SENDER --------------------------------------------
    // Maps ready-mask bit -> the client it names. Owned and mutated ONLY by this thread; producers
    // never touch it -- they read the slot index off the Client and set a bit.
    static constexpr uint32_t kNoWbSlot = UINT32_MAX;

    uint32_t assign_wb_slot(Client* c) {
        uint32_t s;
        if (!free_slots_.empty()) { s = free_slots_.back(); free_slots_.pop_back(); }
        else if (slots_.size() < ReadyMask::kSlots) { s = static_cast<uint32_t>(slots_.size()); slots_.push_back(nullptr); }
        else return kNoWbSlot;                    // table full: caller stays on the channel path
        slots_[s] = c;
        return s;
    }
    void release_wb_slot(uint32_t s) {
        if (s == kNoWbSlot || s >= slots_.size()) return;
        slots_[s] = nullptr;
        free_slots_.push_back(s);
    }
    Client* wb_slot_client(uint32_t s) { return s < slots_.size() ? slots_[s] : nullptr; }

    ReadyMask& ready() { return ready_; }

    // Producer side of the park protocol: called by whoever performed the empty->flagged transition
    // on this thread's ready mask (that RMW is the fence the load below leans on).
    void wake_if_parked(Ring& my_ring, LoopSignals& sig) {
        if (ring_ && parked_.load(std::memory_order_acquire)) {
            my_ring.msg_to(*ring_, ur_tag(UrKind::Wake, nullptr));
            sig.wakes_sent++;
        }
    }

    // Park predicates must match what the current loop can actually drain. A generic union of all
    // channel families turns one misrouted/stale entry into a permanent busy-spin: the loop sees
    // work forever, but none of its drain functions can consume that family.
    bool any_io_inbound() const {
        if (ready_.any()) return true;
        if (client_notify_.any()) return true;
        for (uint32_t i = 0; i < nchan_; i++)
            if (client_in_[i].depth()) return true;
        return false;
    }
    bool any_ex_inbound() const {
        if (task_notify_.any() || release_notify_.any()) return true;
        for (uint32_t i = 0; i < nchan_; i++)
            if (task_in_[i].depth() || release_in_[i].depth()) return true;
        return false;
    }

    std::atomic<bool>& stop_flag() { return stop_; }

private:
    uint32_t          id_ = 0;
    std::atomic<Role> role_{Role::Idle};
    std::atomic<bool> stop_{false};
    std::atomic<Ring*> ring_{nullptr};

    int      cpu_    = -1;
    uint32_t domain_ = kNoDomain;

    std::unique_ptr<TaskChan[]>   task_in_;
    std::unique_ptr<ClientChan[]> client_in_;
    std::unique_ptr<ReleaseChan[]> release_in_;
    std::unique_ptr<uint64_t[]> command_counts_;
    uint32_t command_count_size_ = 0;
    uint64_t atomic_groups_ = 0;
    uint64_t atomic_localfast_ = 0;
    uint64_t atomic_scan_holds_ = 0;
    AtomicAdmissionLease atomic_admission_lease_;
    ReadyMask  ready_;                     // as a sender: which of my clients completed work
    std::vector<Client*>  slots_;          // slot -> client, sender-owned
    std::vector<uint32_t> free_slots_;
    std::atomic<bool>     parked_{false};
    NotifyMask task_notify_;      // "which producers have ops for me"
    NotifyMask client_notify_;    // "which producers have clients for me"
    NotifyMask release_notify_;   // "which producers returned store borrows to me"
    uint32_t nchan_ = 0;

    // IO-only cold path. A nullptr in client_in is the notification token; payload ownership
    // moves through this queue and is retired by the destination IoLoop.
    std::mutex pubsub_mu_;
    std::deque<PubSubEvent*> pubsub_events_;
    bool pubsub_notified_ = false;

    std::vector<Shard*>  shards_;
    std::vector<Client*> clients_;
    LoopSignals          sig_;
    // Cold publication only: keep every pre-existing ThreadCtx member at its current offset.
    std::atomic<WbEngine*> wb_engine_{nullptr};
};

}  // namespace tomo

// uring.h — one io_uring per thread. The native network backend, and the default.
//
// SECOND ENGINE (--net-io epoll, see net/epoll.h). When epoll is selected this class becomes a
// pure CROSS-THREAD DOORBELL and issues no io_uring syscall at all -- not even io_uring_setup. That
// is the whole point of the mode: the server has to boot where io_uring is unavailable.
//
// Why the doorbell lives HERE rather than in a new abstraction. Every cross-thread wake in the tree
// is spelled `my_ring.msg_to(peer_ring, tag)` -- Channel::wake, ThreadCtx::wake_if_parked, the
// snapshot epoch barrier. Those call sites belong to the EX side and to the channel mesh, which the
// engine change is not allowed to touch. So the ENGINE-DEPENDENT PART IS HIDDEN BEHIND THE SAME
// FOUR METHODS instead: in epoll mode msg_to() posts the tag into the target's mailbox and rings its
// eventfd, and for_each_cqe() hands those tags back shaped exactly like CQEs. Both loops' on_cqe
// switches therefore see the identical tag stream in either engine, and ex_loop.h, thread.h and
// signal.h are unmodified.
//
// THE TAG MUST SURVIVE, not just the wake. The snapshot epoch barrier posts UrKind::SnapshotStart
// carrying a SnapshotManager*, and the executor that receives it calls begin_snapshot() on that
// pointer. A doorbell that only said "wake up" would silently break BGSAVE under epoll -- the
// executors would never enter the barrier and the snapshot would hang. Hence a mailbox of tags, not
// a bare eventfd counter.
//
// Cost when NOT selected: one predicted-not-taken test on wake_fd_ per outer-loop step (submit,
// wait, drain) and per park-wake. Nothing per operation, nothing per event.
//
// RING PER THREAD, NEVER SHARED. A shared ring needs locking around the submission queue and
// serialises every thread through one completion path. Per-thread rings with SINGLE_ISSUER let the
// kernel skip its own internal locking, and it is the configuration where io_uring actually wins:
// naively swapped in, io_uring is roughly net-neutral, and the large gains come from exploiting it
// deliberately (ring per thread + DEFER_TASKRUN).
//
// DEFER_TASKRUN is on, with one consequence recorded here because it already caused a wrong reading
// once: it defers completion processing to the point where the thread waits on the ring, which means
// the thread's CPU time no longer reflects how busy it is. Any controller that reads thread busyness
// to make decisions will be BLIND under this flag and must sample CPUTIME explicitly instead of
// inferring it from loop behaviour.
//
// USER DATA encodes what a completion was for. x86-64 user pointers are canonical 48-bit, so the
// top 16 bits carry the kind — same trick as the FlatStore slot.
#pragma once
#include <liburing.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace tomo {

// BOOT-LATCHED, WRITTEN ONCE, before any thread that reads it exists (main.cc, before the pool is
// spawned). Not a knob that can be flipped: every Ring in the process must agree, because a uring
// ring cannot receive an eventfd doorbell and an eventfd doorbell cannot receive a msg_ring.
inline bool g_ring_epoll_mode = false;

enum class UrKind : uint8_t {
    Accept = 1,
    Recv   = 2,
    Send   = 3,
    Close  = 4,
    Wake   = 5,   // cross-ring notification via msg_ring
    UnixAccept = 6,
    SnapshotStart = 7,  // epoch barrier request; pointer is SnapshotManager
    TlsAccept = 9,
    TlsRecv = 10,
    TlsSend = 11,
    AofIo = 12,         // persistence-engine request; pointer is writer-private request state
    SnapshotIo = 13,    // persistence-engine request; pointer is writer-private request state
    TlsReadPoll = 14,
    TlsWritePoll = 15,
};

inline uint64_t ur_tag(UrKind k, void* p) {
    return (static_cast<uint64_t>(k) << 56) | (reinterpret_cast<uint64_t>(p) & ((1ULL << 48) - 1));
}
inline UrKind ur_kind(uint64_t tag) { return static_cast<UrKind>(tag >> 56); }
template <typename T>
inline T* ur_ptr(uint64_t tag) { return reinterpret_cast<T*>(tag & ((1ULL << 48) - 1)); }

class Ring {
public:
    Ring() = default;
    ~Ring() {
        if (inited_) io_uring_queue_exit(&r_);
        if (wake_fd_ >= 0) ::close(wake_fd_);
    }
    Ring(const Ring&) = delete;
    Ring& operator=(const Ring&) = delete;

    bool init(unsigned entries) {
        if (g_ring_epoll_mode) {
            // EFD_NONBLOCK so a spurious drain of an already-empty counter cannot park the loop.
            wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            if (wake_fd_ < 0) { std::perror("eventfd"); return false; }
            return true;
        }
        io_uring_params p{};
        // SINGLE_ISSUER: promises only one thread submits, letting the kernel drop internal locking.
        // DEFER_TASKRUN: completions are processed when we wait, not via IPI on the submitting cpu.
        // SUBMIT_ALL: keep submitting after an SQE fails validation instead of stopping at it. We
        // batch many SQEs per enter by design, so the default behaviour would silently strand every
        // later SQE in the batch -- including recv re-arms belonging to unrelated connections -- and
        // present as one connection's fault stalling several others.
        p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SUBMIT_ALL;
        int rc = io_uring_queue_init_params(entries, &r_, &p);
        if (rc < 0) {
            // Fall back rather than refuse to boot: an older kernel should still run, just slower.
            // The fork had the opposite failure — a build without uring support hung on the first
            // connection instead of degrading, which is far worse than being slow.
            std::memset(&p, 0, sizeof(p));
            rc = io_uring_queue_init_params(entries, &r_, &p);
            if (rc < 0) { std::fprintf(stderr, "io_uring init failed: %d\n", rc); return false; }
            deferred_ = false;
        }
        inited_ = true;
        return true;
    }

    io_uring* raw() { return &r_; }
    bool deferred() const { return deferred_; }

    // >= 0 exactly in epoll mode. The io loop registers it in its epoll set; that is what turns a
    // peer's msg_to() into a return from epoll_wait.
    int wake_fd() const { return wake_fd_; }

    // Drain the doorbell counter. Level-triggered registration plus this read is deliberate: an
    // edge-triggered eventfd that we forgot to read would go quiet forever, and the price of
    // getting it wrong is a permanently parked io thread.
    void drain_wake_fd() {
        uint64_t counter = 0;
        while (::read(wake_fd_, &counter, sizeof(counter)) == static_cast<ssize_t>(sizeof(counter)))
            ;
    }

    // Never returns nullptr in uring mode: a full SQ ring is flushed rather than reported, because
    // every caller would otherwise have to invent the same retry and one of them would get it
    // wrong. In epoll mode there is no submission queue and this returns nullptr -- every caller
    // already handles that (it is the sqe-starved path), and no caller reaches it: network
    // submission is engine-selected in IoLoop, and persistence is forced to --persist-io normal.
    io_uring_sqe* sqe() {
        if (__builtin_expect(wake_fd_ >= 0, false)) return nullptr;
        io_uring_sqe* s = io_uring_get_sqe(&r_);
        if (!s) { submit(); s = io_uring_get_sqe(&r_); }
        return s;
    }

    // A linked pair must not be split by sqe()'s full-ring flush between its two entries.
    void ensure_sq_space(unsigned needed) {
        if (__builtin_expect(wake_fd_ >= 0, false)) return;
        if (io_uring_sq_space_left(&r_) < needed) submit();
    }

    int submit() {
        pending_ = 0;
        if (__builtin_expect(wake_fd_ >= 0, false)) return 0;
        return io_uring_submit(&r_);
    }

    // Submit AND run pending completion work.
    //
    // THIS IS NOT AN OPTIMISATION, IT IS REQUIRED UNDER DEFER_TASKRUN. That flag defers completion
    // processing until the thread enters the kernel asking for events; a plain io_uring_submit()
    // does not ask. So a busy loop that submits and then reads the CQ sees it EMPTY -- not because
    // nothing finished, but because nothing has been allowed to finish yet. Completions only surface
    // when the loop eventually parks, which turns every request's latency into "however long until
    // this thread next runs out of work".
    //
    // Measured cost of getting this wrong: a uniform ~3.9 ms per operation at p1, matching p99
    // exactly, i.e. paid by every request rather than a tail.
    int submit_and_reap() {
        pending_ = 0;
        if (__builtin_expect(wake_fd_ >= 0, false)) return 0;
        return io_uring_submit_and_get_events(&r_);
    }

    // Submit whatever is queued and block until a completion arrives OR the timeout expires.
    // The timeout is not decoration: without it a thread parked here never re-reads its stop flag,
    // so shutdown hangs and the process has to be SIGKILLed. It also bounds the damage from any
    // missed wake — the loop recovers on the next tick instead of sleeping forever.
    int submit_and_wait(unsigned want = 1, unsigned timeout_ms = 50) {
        pending_ = 0;
        if (__builtin_expect(wake_fd_ >= 0, false)) {
            // The ex loop's park. Same contract as the uring path: block until a peer rings the
            // doorbell OR the timeout expires, so the stop flag is re-read on every tick.
            pollfd p{wake_fd_, POLLIN, 0};
            const int n = ::poll(&p, 1, static_cast<int>(timeout_ms));
            if (n > 0) drain_wake_fd();
            return n < 0 ? 0 : n;
        }
        __kernel_timespec ts{};
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = static_cast<long long>(timeout_ms % 1000) * 1000000LL;
        // cqe_ptr is written through UNCONDITIONALLY by liburing — passing nullptr segfaults inside
        // the library rather than returning an error. The value is then ignored here on purpose:
        // completions are drained by for_each_cqe, which advances the ring once for the whole batch.
        io_uring_cqe* cqe = nullptr;
        return io_uring_submit_and_wait_timeout(&r_, &cqe, want, &ts, nullptr);
    }

    // Batch-drain completions. `fn(cqe)` per completion; the ring advances once at the end, which
    // is a single store rather than one per completion.
    template <typename Fn>
    unsigned for_each_cqe(Fn&& fn) {
        unsigned n = 0;
        if (__builtin_expect(wake_fd_ >= 0, false)) return for_each_mail(fn);
        if (!deferred_cqes_.empty()) {
            std::vector<io_uring_cqe> deferred;
            deferred.swap(deferred_cqes_);
            for (io_uring_cqe& cqe : deferred) { fn(&cqe); n++; }
        }
        io_uring_cqe* cqe;
        unsigned head, raw = 0;
        raw_callback_active_ = true;
        raw_callback_taken_ = false;
        io_uring_for_each_cqe(&r_, head, cqe) {
            raw++;
            raw_callback_index_ = raw;
            fn(cqe);
            if (raw_callback_taken_) break;
        }
        raw_callback_active_ = false;
        if (raw && !raw_callback_taken_) io_uring_cq_advance(&r_, raw);
        raw_callback_taken_ = false;
        raw_callback_index_ = 0;
        n += raw;
        return n;
    }

    // A persistence command can deliberately occupy its IO owner (SAVE and the AOF rewrite mark).
    // It still has to reap its own CQEs under DEFER_TASKRUN, but must not consume unrelated recv/send
    // completions whose callbacks live in IoLoop. The filter handles matching CQEs and retains the
    // rest for the loop's next ordinary for_each_cqe pass.
    template <typename Fn>
    unsigned for_each_cqe_filtered(Fn&& fn) {
        // Nothing to filter in epoll mode: persistence is forced onto the syscall engine there, so
        // no AofIo/SnapshotIo completion can exist. Returning zero is not a stub -- the callers
        // (SAVE, the AOF rewrite mark) all loop on their own pending counters, which stay zero.
        if (__builtin_expect(wake_fd_ >= 0, false)) return 0;
        // A blocking SAVE can enter here from inside the recv CQE callback that invoked it. Retire
        // that already-processed prefix before looking at the CQ and tell the outer batch to stop;
        // otherwise the current recv CQE is copied into deferred_cqes_ and delivered a second time.
        if (raw_callback_active_ && !raw_callback_taken_) {
            io_uring_cq_advance(&r_, raw_callback_index_);
            raw_callback_taken_ = true;
        }
        unsigned consumed = 0;
        std::vector<io_uring_cqe> keep;
        keep.reserve(deferred_cqes_.size());
        for (io_uring_cqe& cqe : deferred_cqes_) {
            if (fn(&cqe)) consumed++;
            else keep.push_back(cqe);
        }
        deferred_cqes_.swap(keep);

        io_uring_cqe* cqe;
        unsigned head, raw = 0;
        io_uring_for_each_cqe(&r_, head, cqe) {
            if (fn(cqe)) consumed++;
            else deferred_cqes_.push_back(*cqe);
            raw++;
        }
        if (raw) io_uring_cq_advance(&r_, raw);
        return consumed;
    }

    void note_pending() { pending_++; }
    unsigned pending() const { return pending_; }

    // Post a completion into ANOTHER thread's ring. This is how an IO thread tells a WB thread that
    // a client has replies to send, without a shared queue or an eventfd round trip.
    bool msg_to(Ring& target, uint64_t tag) {
        if (__builtin_expect(target.wake_fd_ >= 0, false)) return target.post_mail(tag);
        io_uring_sqe* s = sqe();
        if (!s) return false;
        io_uring_prep_msg_ring(s, target.r_.ring_fd, 0, tag, 0);
        s->user_data = ur_tag(UrKind::Wake, nullptr);
        note_pending();
        return true;
    }

private:
    // The epoll-mode doorbell. PAYLOAD FIRST, THEN THE BELL -- the same push-then-flag order the
    // channel mesh uses, and for the same reason: a consumer woken by the bell must be able to see
    // everything the bell was rung for. The mutex is only ever contended by a peer that found this
    // thread parked, which is by construction a thread that had nothing to do.
    bool post_mail(uint64_t tag) {
        {
            std::lock_guard<std::mutex> lock(mail_mu_);
            mail_.push_back(tag);
        }
        const uint64_t one = 1;
        return ::write(wake_fd_, &one, sizeof(one)) == static_cast<ssize_t>(sizeof(one));
    }

    // Hand the mailbox back shaped as CQEs so both loops' on_cqe switches are engine-blind. res and
    // flags are zero, which is what a msg_ring completion carries anyway.
    template <typename Fn>
    unsigned for_each_mail(Fn&& fn) {
        std::vector<uint64_t> taken;
        {
            std::lock_guard<std::mutex> lock(mail_mu_);
            if (mail_.empty()) return 0;
            taken.swap(mail_);
        }
        for (uint64_t tag : taken) {
            io_uring_cqe cqe{};
            cqe.user_data = tag;
            fn(&cqe);
        }
        return static_cast<unsigned>(taken.size());
    }

    io_uring r_{};
    bool     inited_   = false;
    bool     deferred_ = true;
    unsigned pending_  = 0;
    std::vector<io_uring_cqe> deferred_cqes_;
    bool raw_callback_active_ = false;
    bool raw_callback_taken_ = false;
    unsigned raw_callback_index_ = 0;
    // Epoll-mode doorbell state. -1 in uring mode, which is the single test every method above
    // branches on; the vector and mutex are never constructed into use there.
    int wake_fd_ = -1;
    std::mutex mail_mu_;
    std::vector<uint64_t> mail_;
};

}  // namespace tomo

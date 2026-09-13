// epoll.h — the second network event engine, selected at boot by --net-io epoll.
//
// WHY A SECOND ENGINE AT ALL. io_uring is the native path and stays the default: ring per thread +
// DEFER_TASKRUN is where the measured wins live. But io_uring is not always available (older or
// hardened kernels, containers with io_uring_setup seccomp-blocked, some hypervisors) and it is not
// always wanted. This engine exists so the same server binary still runs there, correctly, at
// whatever speed epoll gives.
//
// WHAT AN ENGINE IS ALLOWED TO CHANGE. Only how an io thread WAITS FOR and COMPLETES network
// readiness. Routing, shard ownership, the ROB, retirement order and the reply STRUCTURE are engine
// independent; both engines feed exactly the same parse -> route -> retire -> stage -> write
// pipeline. The choice is resolved ONCE, at boot, into a template parameter (see IoLoop::run), so no
// per-event branch on "which engine" exists in either instantiation.
//
// THE THREE SHAPE DIFFERENCES, stated once here because every subtlety below follows from them:
//   1. io_uring COMPLETES operations; epoll only REPORTS READINESS. So under epoll the io thread
//      issues recv/send/accept itself, synchronously, and the "completion" is the syscall's return
//      value fed to the same accounting the CQE result feeds.
//   2. io_uring holds a pointer into our buffers while a recv/send is in flight. epoll never does.
//      That makes the read buffer resettable at every quiescence point rather than only when no
//      recv is armed, and it makes a closing connection releasable immediately.
//   3. epoll has no cross-thread doorbell of its own. io_uring has msg_ring. See uring.h for the
//      eventfd mailbox that stands in for it, so the ex side and every channel wake stay unchanged.
//
// EDGE TRIGGERED, ARMED ONCE, NEVER RE-ARMED. Each connection is registered once at accept with
// EPOLLIN|EPOLLOUT|EPOLLRDHUP|EPOLLET and is never epoll_ctl'd again; ::close() removes it. That is
// deliberate: EPOLLONESHOT would cost one epoll_ctl syscall per readiness event, which is a syscall
// per recv on the hot path, and level-triggered EPOLLIN spins the loop at 100% whenever a
// connection has buffered bytes the ROB window will not let us parse yet. Edge triggering costs
// neither, at the price of the standard obligation: read/write until EAGAIN, and if we stop early
// (no read space, ROB full) remember that ourselves rather than expecting another edge. Client's
// recv_armed_ carries exactly that memory under this engine -- true means "we reached EAGAIN, an
// edge is owed", false means "there may be more, retry without waiting".
#pragma once
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>

namespace tomo {

// Per-thread epoll set. One per io thread, never shared: sharing it would reintroduce exactly the
// thundering-herd/one-thread-takes-everything distribution failure that SO_REUSEPORT exists to
// avoid on the accept side.
class EpollSet {
public:
    static constexpr int kMaxEvents = 256;

    EpollSet() = default;
    ~EpollSet() { if (fd_ >= 0) ::close(fd_); }
    EpollSet(const EpollSet&) = delete;
    EpollSet& operator=(const EpollSet&) = delete;

    bool init() {
        fd_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (fd_ < 0) { std::perror("epoll_create1"); return false; }
        return true;
    }
    bool add(int target, uint32_t events, uint64_t tag) {
        epoll_event ev{};
        ev.events = events;
        ev.data.u64 = tag;
        return ::epoll_ctl(fd_, EPOLL_CTL_ADD, target, &ev) == 0;
    }
    // Blocking (or, with timeout_ms == 0, polling) wait. EINTR is reported as zero events rather
    // than as an error: the loop's next pass re-reads its stop flag, which is how shutdown lands.
    int wait(int timeout_ms) {
        const int n = ::epoll_wait(fd_, events_, kMaxEvents, timeout_ms);
        n_ = n > 0 ? n : 0;
        return n_;
    }
    const epoll_event& event(int i) const { return events_[i]; }

private:
    int fd_ = -1;
    int n_ = 0;
    epoll_event events_[kMaxEvents];
};

// O_NONBLOCK is mandatory under this engine and only under it: every recv/send/accept is issued by
// the io thread itself, and a blocking one would park the whole thread (and every other connection
// it owns) inside one socket.
inline bool set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

}  // namespace tomo

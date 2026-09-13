#include "snapshot.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

#include "../core/server.h"
#include "../core/thread.h"
#include "../net/uring.h"
#include "../persist/aof.h"
#include "../store/flatstore.h"

namespace tomo {
namespace {

constexpr uint8_t kFileMagic[8] = {'T','O','M','O','S','N','P','\0'};
constexpr uint32_t kFileHeaderBytes = 80;
constexpr uint32_t kFrameHeaderBytes = 32;
constexpr uint32_t kFooterBytes = 32;
constexpr uint32_t kFrameTag = 0x4d415246;   // "FRAM"
constexpr uint32_t kFooterTag = 0x454e4f44;  // "DONE"
constexpr uint32_t kRecordTag = 0x44434552;  // "RECD"
constexpr uint32_t kRecordHeaderBytes = 32;
constexpr uint32_t kWriterFramesPerPass = 8;
constexpr uint32_t kSnapshotMaxInflight = 64;

enum SnapshotIoRole : uint8_t {
    SnapshotIoHeader = 1,
    SnapshotIoFrame = 2,
    SnapshotIoFooter = 3,
    SnapshotIoFileSync = 4,
    SnapshotIoDirectorySync = 5,
};

struct SnapshotIoRequest {
    SnapshotIoRole role = SnapshotIoFrame;
    uint64_t epoch = 0;
    int fd = -1;
    uint64_t offset = 0;
    size_t remaining = 0;
    std::array<uint8_t, kFileHeaderBytes> header{};
    std::unique_ptr<SnapshotChunk> chunk;
    iovec vectors[2]{};
    uint32_t vector_count = 0;
};

thread_local SnapshotIoContext tls_io_context;

int64_t realtime_ms() {
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

bool write_all(int fd, const uint8_t* p, size_t n) {
    while (n) {
        const ssize_t written = ::write(fd, p, n);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) return false;
        p += written;
        n -= static_cast<size_t>(written);
    }
    return true;
}

void consume_iovecs(SnapshotIoRequest& request, size_t bytes) {
    if (request.vector_count == 2 && bytes >= request.vectors[0].iov_len) {
        bytes -= request.vectors[0].iov_len;
        request.vectors[0] = request.vectors[1];
        request.vector_count = 1;
    }
    if (request.vector_count == 1 && bytes >= request.vectors[0].iov_len) {
        request.vector_count = 0;
        return;
    }
    if (bytes && request.vector_count) {
        request.vectors[0].iov_base =
            static_cast<uint8_t*>(request.vectors[0].iov_base) + bytes;
        request.vectors[0].iov_len -= bytes;
    }
}

bool queue_snapshot_write(Ring& ring, SnapshotIoRequest& request) {
    io_uring_sqe* sqe = ring.sqe();
    if (!sqe || request.vector_count == 0) return false;
    io_uring_prep_writev(sqe, request.fd, request.vectors,
                         static_cast<unsigned>(request.vector_count), request.offset);
    sqe->user_data = ur_tag(UrKind::SnapshotIo, &request);
    ring.note_pending();
    return true;
}

bool read_all_fd(int fd, std::vector<uint8_t>& out, std::string& error) {
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size < 0) {
        error = "could not stat snapshot";
        return false;
    }
    try {
        out.resize(static_cast<size_t>(st.st_size));
    } catch (const std::bad_alloc&) {
        error = "out of memory reading snapshot";
        return false;
    }
    size_t off = 0;
    while (off < out.size()) {
        const ssize_t n = ::read(fd, out.data() + off, out.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { error = "short snapshot read"; return false; }
        off += static_cast<size_t>(n);
    }
    return true;
}

const char* hook_error(SnapshotHookStatus status) {
    switch (status) {
        case SnapshotHookStatus::Unsupported: return "snapshot contains an unsupported value type";
        case SnapshotHookStatus::Corrupt: return "corrupt snapshot type payload";
        case SnapshotHookStatus::Oom: return "out of memory loading snapshot";
        case SnapshotHookStatus::Ok: break;
    }
    return "snapshot type hook failed";
}

}  // namespace

const SnapshotTypeHooks& snapshot_type_hooks(Type type) {
    static const SnapshotTypeHooks hooks[] = {
        string_snapshot_hooks(), hash_snapshot_hooks(), list_snapshot_hooks(),
        set_snapshot_hooks(), zset_snapshot_hooks(), stream_snapshot_hooks(),
    };
    const uint32_t index = static_cast<uint32_t>(type);
    return hooks[index < 6 ? index : 0];
}

void snapshot_bind_io(ThreadCtx* thread, Ring* ring) { tls_io_context = {thread, ring}; }
SnapshotIoContext snapshot_io_context() { return tls_io_context; }

SnapshotManager::~SnapshotManager() {
    abort_file();
    if (chunk_in_) {
        for (uint32_t p = 0; p < nthreads_; p++) {
            SnapshotChunk* chunk = nullptr;
            while (chunk_in_[p].recv(chunk)) { delete chunk; chunk_in_[p].retire(); }
        }
    }
}

void SnapshotManager::init(uint32_t nthreads, uint32_t nshards, uint32_t executor_count,
                           const char* dir, const char* dbfilename,
                           PersistIoEngine engine) {
    // Redis defines LASTSAVE before the first successful save as the server start time.
    last_save_time_.store(realtime_ms() / 1000, std::memory_order_relaxed);
    nthreads_ = nthreads;
    nshards_ = nshards;
    executor_count_ = executor_count;
    dir_ = (dir && *dir) ? dir : ".";
    dbfilename_ = (dbfilename && *dbfilename) ? dbfilename : "dump.tomo";
    engine_ = engine;
    final_path_ = dir_ + "/" + dbfilename_;
    chunk_in_ = std::make_unique<ChunkChan[]>(nthreads_);
    next_sequence_.assign(nshards_, 0);
    saw_begin_.assign(nshards_, 0);
    saw_end_.assign(nshards_, 0);
}

void SnapshotManager::set_error(const char* text) {
    std::lock_guard<std::mutex> lock(error_mu_);
    error_ = text ? text : "snapshot failed";
}

SnapshotManager::StartResult SnapshotManager::start(Server& server, ThreadCtx& writer,
                                                     Ring& writer_ring, bool is_blocking,
                                                     std::string& error, AofManager* rewrite,
                                                     const char* target_dir,
                                                     const char* target_filename) {
    Phase expected = Phase::Idle;
    if (!phase_.compare_exchange_strong(expected, Phase::Preparing,
                                        std::memory_order_acq_rel)) {
        error = "Background save already in progress";
        return StartResult::Busy;
    }
    // EVERY snapshot path arms the barrier, not just the AOF-rewrite one. The cut is taken per
    // owner between Freeze and Mark; a cross-shard atomic group whose records are installed on some
    // owners and still queued on others straddles it and lands in the file half applied. Arming
    // stops new groups from being admitted, and the drain below waits out the ones already
    // dispatched. Measured on the unfixed tree: 36-51 of 100 generation-tagged cross-shard MSET
    // groups torn per SAVE at --atomic 1, and 8-11 per SAVE for MULTI/EXEC at the default
    // --atomic 0, while a live MGET reader on the same server never saw a single torn group.
    server.set_snapshot_atomic_barrier(true);

    const uint64_t next_epoch = epoch_.fetch_add(1, std::memory_order_relaxed) + 1;
    epoch_.store(next_epoch, std::memory_order_release);
    ready_owners_.store(0, std::memory_order_relaxed);
    frozen_owners_.store(0, std::memory_order_relaxed);
    marked_owners_.store(0, std::memory_order_relaxed);
    cancelled_owners_.store(0, std::memory_order_relaxed);
    finished_owners_.store(0, std::memory_order_relaxed);
    blocking_.store(is_blocking, std::memory_order_relaxed);
    save_current_shard_.store(0, std::memory_order_relaxed);
    writer_tid_.store(writer.id(), std::memory_order_relaxed);
    writer_ring_.store(&writer_ring, std::memory_order_release);
    server_ = &server;
    rewrite_ = rewrite;
    save_change_cut_ = rewrite ? 0 : server.save_change_total();
    writer_failed_.store(false, std::memory_order_relaxed);
    frame_count_ = 0;
    ended_shards_ = 0;
    file_offset_ = 0;
    io_inflight_ = 0;
    header_complete_ = false;
    footer_submitted_ = false;
    directory_fd_ = -1;
    std::fill(next_sequence_.begin(), next_sequence_.end(), 0);
    std::fill(saw_begin_.begin(), saw_begin_.end(), 0);
    std::fill(saw_end_.begin(), saw_end_.end(), 0);
    {
        std::lock_guard<std::mutex> lock(error_mu_);
        error_.clear();
    }

    active_dir_ = target_dir && *target_dir ? target_dir : dir_;
    const char* active_name = target_filename && *target_filename
        ? target_filename : dbfilename_.c_str();
    final_path_ = active_dir_ + "/" + active_name;
    char suffix[96];
    std::snprintf(suffix, sizeof(suffix), ".tmp.%ld.%llu", static_cast<long>(::getpid()),
                  static_cast<unsigned long long>(next_epoch));
    temp_path_ = final_path_ + suffix;
    fd_ = ::open(temp_path_.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (fd_ < 0) {
        error = "could not create snapshot temporary file";
        server.set_snapshot_atomic_barrier(false);
        phase_.store(Phase::Idle, std::memory_order_release);
        writer_tid_.store(UINT32_MAX, std::memory_order_relaxed);
        return StartResult::Failed;
    }

    // The target CQE is the epoch broadcast.  Every executor observes it between operation batches.
    for (uint32_t tid : server.placement().ex_threads()) {
        Ring* target = server.thread(tid).ring();
        while (!target && !server.shutting_down().load(std::memory_order_relaxed)) {
            std::this_thread::yield();
            target = server.thread(tid).ring();
        }
        if (!target || !writer_ring.msg_to(*target, ur_tag(UrKind::SnapshotStart, this))) {
            set_error("could not broadcast snapshot epoch");
            phase_.store(Phase::Failed, std::memory_order_release);
            break;
        }
    }
    writer_ring.submit_and_reap();

    while (phase() == Phase::Preparing &&
           ready_owners_.load(std::memory_order_acquire) != executor_count_)
        std::this_thread::yield();
    if (phase() == Phase::Preparing) {
        drain_atomic_groups(server);
        phase_.store(Phase::Freeze, std::memory_order_release);
        for (uint32_t tid : server.placement().ex_threads())
            if (Ring* target = server.thread(tid).ring())
                writer_ring.msg_to(*target, ur_tag(UrKind::Wake, nullptr));
        writer_ring.submit_and_reap();
    }
    while (phase() == Phase::Freeze &&
           frozen_owners_.load(std::memory_order_acquire) != executor_count_)
        std::this_thread::yield();
    if (phase() == Phase::Freeze) {
        cut_ms_.store(realtime_ms(), std::memory_order_release);
        phase_.store(Phase::Mark, std::memory_order_release);
        for (uint32_t tid : server.placement().ex_threads())
            if (Ring* target = server.thread(tid).ring())
                writer_ring.msg_to(*target, ur_tag(UrKind::Wake, nullptr));
        writer_ring.submit_and_reap();
    }
    while (phase() == Phase::Mark &&
           marked_owners_.load(std::memory_order_acquire) != executor_count_)
        std::this_thread::yield();

    if (phase() == Phase::Mark && rewrite_ &&
        !rewrite_->rewrite_mark(writer, writer_ring, next_epoch, cut_ms(), error)) {
        fail(next_epoch, error.empty() ? "could not rotate AOF at snapshot mark" : error.c_str());
    }

    if (phase() != Phase::Mark) {
        while (cancelled_owners_.load(std::memory_order_acquire) +
                   finished_owners_.load(std::memory_order_acquire) != executor_count_)
            std::this_thread::yield();
        abort_file();
        server.set_snapshot_atomic_barrier(false);
        phase_.store(Phase::Idle, std::memory_order_release);
        std::lock_guard<std::mutex> lock(error_mu_);
        error = error_.empty() ? "snapshot start failed" : error_;
        return StartResult::Failed;
    }

    // Release the epoch barrier before touching the file.  Executors may fill their bounded
    // channels, but this command's IO thread is still the sole writer and writes the header before
    // its loop can drain a frame.
    phase_.store(Phase::Capture, std::memory_order_release);
    server.set_snapshot_atomic_barrier(false);
    for (uint32_t tid : server.placement().ex_threads())
        if (Ring* target = server.thread(tid).ring())
            writer_ring.msg_to(*target, ur_tag(UrKind::Wake, nullptr));
    writer_ring.submit_and_reap();
    const bool header_started = engine_ == PersistIoEngine::Normal
        ? write_header_normal() : submit_header_uring(writer_ring);
    if (!header_started) {
        fail(next_epoch, "could not write snapshot header");
        while (cancelled_owners_.load(std::memory_order_acquire) +
                   finished_owners_.load(std::memory_order_acquire) != executor_count_)
            std::this_thread::yield();
        discard_chunks();
        abort_file();
        server.set_snapshot_atomic_barrier(false);
        phase_.store(Phase::Idle, std::memory_order_release);
        std::lock_guard<std::mutex> lock(error_mu_);
        error = error_.empty() ? "snapshot start failed" : error_;
        return StartResult::Failed;
    }
    if (engine_ == PersistIoEngine::Uring) writer_ring.submit_and_reap();
    if (!is_blocking) return StartResult::Started;

    while (phase() == Phase::Capture) {
        writer_pass(writer, writer_ring, true);
        writer_ring.submit_and_reap();
        if (engine_ == PersistIoEngine::Uring)
            pump_io_completions(writer, writer_ring);
        std::this_thread::yield();
    }
    if (phase() == Phase::Failed) {
        while (cancelled_owners_.load(std::memory_order_acquire) +
                   finished_owners_.load(std::memory_order_acquire) != executor_count_ ||
               io_inflight_ != 0) {
            writer_pass(writer, writer_ring, true);
            writer_ring.submit_and_reap();
            if (engine_ == PersistIoEngine::Uring)
                pump_io_completions(writer, writer_ring);
            std::this_thread::yield();
        }
        writer_pass(writer, writer_ring, true);
        std::lock_guard<std::mutex> lock(error_mu_);
        error = error_.empty() ? "snapshot failed" : error_;
        return StartResult::Failed;
    }
    if (writer_failed_.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(error_mu_);
        error = error_.empty() ? "snapshot finalization failed" : error_;
        return StartResult::Failed;
    }
    return StartResult::Started;
}

// Waits out every cross-shard atomic group whose records are only partly installed, so the cut that
// follows cannot land inside one. The barrier is already armed, so the count is monotonically
// non-increasing here and this terminates.
//
// It waits on atomic_apply_inflight(), NOT on atomic_inflight(). The latter also counts groups
// whose records are all installed but whose reply has not yet retired, and that retirement runs on
// the admitting IO thread -- the thread a blocking SAVE is sitting inside. Waiting on it deadlocks:
// probed on this tree, a SAVE under a 24-connection MSET storm hung with rdb_bgsave_in_progress:1
// and atomic_inflight:13 forever. Owner-side completion has no such dependency.
//
// cuts_waited_ is the non-vacuous part: a drain that never blocks is indistinguishable from a
// missing one, so the battery asserts this counter advanced.
void SnapshotManager::drain_atomic_groups(Server& server) {
    cuts_armed_.fetch_add(1, std::memory_order_relaxed);
    const uint64_t queued = server.atomic_apply_inflight();
    if (!queued) return;
    cuts_waited_.fetch_add(1, std::memory_order_relaxed);
    drained_groups_.fetch_add(queued, std::memory_order_relaxed);
    while (server.atomic_apply_inflight() != 0 &&
           !server.shutting_down().load(std::memory_order_relaxed))
        std::this_thread::yield();
}

void SnapshotManager::owner_ready(uint64_t value) {
    if (value == epoch()) ready_owners_.fetch_add(1, std::memory_order_acq_rel);
}
void SnapshotManager::owner_frozen(uint64_t value) {
    if (value == epoch()) frozen_owners_.fetch_add(1, std::memory_order_acq_rel);
}
void SnapshotManager::owner_marked(uint64_t value) {
    if (value == epoch()) marked_owners_.fetch_add(1, std::memory_order_acq_rel);
}
void SnapshotManager::owner_cancelled(uint64_t value) {
    if (value == epoch()) cancelled_owners_.fetch_add(1, std::memory_order_acq_rel);
}
void SnapshotManager::owner_finished(uint64_t value) {
    if (value == epoch()) finished_owners_.fetch_add(1, std::memory_order_acq_rel);
}
void SnapshotManager::fail(uint64_t value, const char* reason) {
    if (value != epoch()) return;
    set_error(reason);
    Phase current = phase();
    while (current != Phase::Idle && current != Phase::Failed &&
           !phase_.compare_exchange_weak(current, Phase::Failed, std::memory_order_acq_rel)) {}
}

bool SnapshotManager::post_chunk(uint32_t producer, std::unique_ptr<SnapshotChunk>& chunk,
                                 Ring& producer_ring, LoopSignals& signals) {
    if (!chunk || producer >= nthreads_) return false;
    SnapshotChunk* raw = chunk.get();
    if (!chunk_in_[producer].push(raw, signals)) return false;
    chunk.release();
    if (chunk_notify_.set(producer) && !blocking()) {
        Ring* target = writer_ring_.load(std::memory_order_acquire);
        chunk_in_[producer].wake(producer_ring, signals, target);
    }
    return true;
}

bool SnapshotManager::write_header_normal() {
    uint8_t h[kFileHeaderBytes] = {};
    std::memcpy(h, kFileMagic, sizeof(kFileMagic));
    snapshot_put_u32(h + 8, kSnapshotFormatVersion);
    snapshot_put_u32(h + 12, kFileHeaderBytes);
    snapshot_put_u32(h + 16, nshards_);
    snapshot_put_u32(h + 20, static_cast<uint32_t>(g_hash_kind));
    snapshot_put_u64(h + 24, epoch());
    snapshot_put_u64(h + 32, static_cast<uint64_t>(cut_ms()));
    snapshot_put_u64(h + 40, g_hash_seed);
    snapshot_put_u64(h + 48, g_sip_k0);
    snapshot_put_u64(h + 56, g_sip_k1);
    snapshot_put_u64(h + 64, snapshot_checksum(h, 64));
    if (!write_all(fd_, h, sizeof(h))) return false;
    file_offset_ = sizeof(h);
    header_complete_ = true;
    return true;
}

bool SnapshotManager::validate_frame(const SnapshotChunk& chunk, uint8_t* h) {
    if (chunk.sid < 0 || static_cast<uint32_t>(chunk.sid) >= nshards_) return false;
    const uint32_t sid = static_cast<uint32_t>(chunk.sid);
    if (chunk.sequence != next_sequence_[sid]++ || saw_end_[sid]) return false;
    if (chunk.flags & SnapshotFrameBegin) {
        if (saw_begin_[sid]) return false;
        saw_begin_[sid] = 1;
    } else if (!saw_begin_[sid]) {
        return false;
    }
    snapshot_put_u32(h + 0, kFrameTag);
    snapshot_put_u32(h + 4, sid);
    snapshot_put_u32(h + 8, chunk.sequence);
    snapshot_put_u32(h + 12, chunk.flags);
    snapshot_put_u32(h + 16, static_cast<uint32_t>(chunk.bytes.size()));
    snapshot_put_u64(h + 20, snapshot_checksum(chunk.bytes.data(), chunk.bytes.size()));
    frame_count_++;
    if (chunk.flags & SnapshotFrameEnd) {
        saw_end_[sid] = 1;
        ended_shards_++;
        if (blocking()) {
            const uint32_t next = sid + 1;
            save_current_shard_.store(next, std::memory_order_release);
            if (server_ && next < nshards_) {
                ThreadCtx& owner = server_->thread(server_->worker_of_shard(static_cast<int32_t>(next)));
                Ring* source = writer_ring_.load(std::memory_order_acquire);
                if (source && owner.ring()) source->msg_to(*owner.ring(), ur_tag(UrKind::Wake, nullptr));
            }
        }
    }
    return true;
}

bool SnapshotManager::write_frame_normal(const SnapshotChunk& chunk) {
    uint8_t h[kFrameHeaderBytes] = {};
    if (!validate_frame(chunk, h)) return false;
    if (!write_all(fd_, h, sizeof(h)) ||
        !write_all(fd_, chunk.bytes.data(), chunk.bytes.size())) return false;
    file_offset_ += sizeof(h) + chunk.bytes.size();
    return true;
}

bool SnapshotManager::submit_header_uring(Ring& ring) {
    auto* request = new (std::nothrow) SnapshotIoRequest();
    if (!request) return false;
    request->role = SnapshotIoHeader;
    request->epoch = epoch();
    request->fd = fd_;
    std::memcpy(request->header.data(), kFileMagic, sizeof(kFileMagic));
    snapshot_put_u32(request->header.data() + 8, kSnapshotFormatVersion);
    snapshot_put_u32(request->header.data() + 12, kFileHeaderBytes);
    snapshot_put_u32(request->header.data() + 16, nshards_);
    snapshot_put_u32(request->header.data() + 20, static_cast<uint32_t>(g_hash_kind));
    snapshot_put_u64(request->header.data() + 24, epoch());
    snapshot_put_u64(request->header.data() + 32, static_cast<uint64_t>(cut_ms()));
    snapshot_put_u64(request->header.data() + 40, g_hash_seed);
    snapshot_put_u64(request->header.data() + 48, g_sip_k0);
    snapshot_put_u64(request->header.data() + 56, g_sip_k1);
    snapshot_put_u64(request->header.data() + 64,
                     snapshot_checksum(request->header.data(), 64));
    request->offset = 0;
    request->remaining = kFileHeaderBytes;
    request->vectors[0] = {request->header.data(), kFileHeaderBytes};
    request->vector_count = 1;
    if (!queue_snapshot_write(ring, *request)) { delete request; return false; }
    file_offset_ = kFileHeaderBytes;
    io_inflight_++;
    return true;
}

bool SnapshotManager::submit_frame_uring(std::unique_ptr<SnapshotChunk> chunk, Ring& ring) {
    auto* request = new (std::nothrow) SnapshotIoRequest();
    if (!request) return false;
    request->role = SnapshotIoFrame;
    request->epoch = epoch();
    request->fd = fd_;
    if (!validate_frame(*chunk, request->header.data())) { delete request; return false; }
    request->chunk = std::move(chunk);
    request->offset = file_offset_;
    request->remaining = kFrameHeaderBytes + request->chunk->bytes.size();
    request->vectors[0] = {request->header.data(), kFrameHeaderBytes};
    request->vectors[1] = {request->chunk->bytes.data(), request->chunk->bytes.size()};
    request->vector_count = 2;
    if (!queue_snapshot_write(ring, *request)) { delete request; return false; }
    file_offset_ += request->remaining;
    io_inflight_++;
    return true;
}

bool SnapshotManager::submit_footer_uring(Ring& ring) {
    auto* request = new (std::nothrow) SnapshotIoRequest();
    if (!request) return false;
    request->role = SnapshotIoFooter;
    request->epoch = epoch();
    request->fd = fd_;
    snapshot_put_u32(request->header.data() + 0, kFooterTag);
    snapshot_put_u32(request->header.data() + 4, nshards_);
    snapshot_put_u64(request->header.data() + 8, epoch());
    snapshot_put_u64(request->header.data() + 16, frame_count_);
    snapshot_put_u64(request->header.data() + 24,
                     snapshot_checksum(request->header.data(), 24));
    request->offset = file_offset_;
    request->remaining = kFooterBytes;
    request->vectors[0] = {request->header.data(), kFooterBytes};
    request->vector_count = 1;
    if (!queue_snapshot_write(ring, *request)) { delete request; return false; }
    file_offset_ += kFooterBytes;
    footer_submitted_ = true;
    io_inflight_++;
    return true;
}

bool SnapshotManager::submit_sync_uring(Ring& ring, int fd, uint8_t role) {
    auto* request = new (std::nothrow) SnapshotIoRequest();
    if (!request) return false;
    request->role = static_cast<SnapshotIoRole>(role);
    request->epoch = epoch();
    request->fd = fd;
    io_uring_sqe* sqe = ring.sqe();
    if (!sqe) { delete request; return false; }
    io_uring_prep_fsync(sqe, fd,
                        role == SnapshotIoDirectorySync ? 0 : IORING_FSYNC_DATASYNC);
    sqe->user_data = ur_tag(UrKind::SnapshotIo, request);
    ring.note_pending();
    io_inflight_++;
    return true;
}

bool SnapshotManager::finish_file_normal() {
    uint8_t footer[kFooterBytes] = {};
    snapshot_put_u32(footer + 0, kFooterTag);
    snapshot_put_u32(footer + 4, nshards_);
    snapshot_put_u64(footer + 8, epoch());
    snapshot_put_u64(footer + 16, frame_count_);
    snapshot_put_u64(footer + 24, snapshot_checksum(footer, 24));
    if (!write_all(fd_, footer, sizeof(footer)) || ::fdatasync(fd_) != 0) return false;
    file_offset_ += sizeof(footer);
    return finish_file_metadata(nullptr);
}

bool SnapshotManager::finish_file_metadata(Ring* ring) {
    if (::close(fd_) != 0) { fd_ = -1; return false; }
    fd_ = -1;
    if (::rename(temp_path_.c_str(), final_path_.c_str()) != 0) return false;
    const int dfd = ::open(active_dir_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) return false;
    if (ring) {
        directory_fd_ = dfd;
        if (!submit_sync_uring(*ring, directory_fd_, SnapshotIoDirectorySync)) {
            ::close(directory_fd_);
            directory_fd_ = -1;
            return false;
        }
        return true;
    }
    const bool dir_synced = ::fsync(dfd) == 0;
    const bool dir_closed = ::close(dfd) == 0;
    if (!dir_synced || !dir_closed) return false;
    return complete_file_success();
}

bool SnapshotManager::complete_file_success() {
    if (rewrite_ && !rewrite_->rewrite_complete(final_path_, epoch())) return false;
    if (!rewrite_ && server_) server_->snapshot_save_succeeded(save_change_cut_);
    last_save_time_.store(realtime_ms() / 1000, std::memory_order_relaxed);
    writer_tid_.store(UINT32_MAX, std::memory_order_relaxed);
    writer_ring_.store(nullptr, std::memory_order_release);
    server_ = nullptr;
    rewrite_ = nullptr;
    phase_.store(Phase::Idle, std::memory_order_release);
    return true;
}

void SnapshotManager::on_io_complete(ThreadCtx&, Ring& ring, void* opaque, int result) {
    std::unique_ptr<SnapshotIoRequest> request(static_cast<SnapshotIoRequest*>(opaque));
    if (!request) return;
    if (request->epoch != epoch()) return;

    auto finalization_failed = [&]() {
        set_error("could not finalize snapshot file");
        writer_failed_.store(true, std::memory_order_relaxed);
        abort_file();
        phase_.store(Phase::Idle, std::memory_order_release);
    };

    if (request->role == SnapshotIoHeader || request->role == SnapshotIoFrame ||
        request->role == SnapshotIoFooter) {
        if (result <= 0 || static_cast<size_t>(result) > request->remaining) {
            if (io_inflight_) io_inflight_--;
            if (request->role == SnapshotIoHeader)
                fail(request->epoch, "could not write snapshot header");
            else if (request->role == SnapshotIoFrame)
                fail(request->epoch, "snapshot file write failed");
            else
                finalization_failed();
            return;
        }
        request->remaining -= static_cast<size_t>(result);
        request->offset += static_cast<uint64_t>(result);
        consume_iovecs(*request, static_cast<size_t>(result));
        if (request->remaining) {
            SnapshotIoRequest* retry = request.release();
            if (!queue_snapshot_write(ring, *retry)) {
                request.reset(retry);
                if (io_inflight_) io_inflight_--;
                if (retry->role == SnapshotIoFrame)
                    fail(retry->epoch, "snapshot file write failed");
                else if (retry->role == SnapshotIoHeader)
                    fail(retry->epoch, "could not write snapshot header");
                else
                    finalization_failed();
            }
            return;
        }

        const SnapshotIoRole role = request->role;
        if (io_inflight_) io_inflight_--;
        if (role == SnapshotIoHeader) {
            header_complete_ = true;
        } else if (role == SnapshotIoFooter &&
                   !submit_sync_uring(ring, fd_, SnapshotIoFileSync)) {
            finalization_failed();
        }
        return;
    }

    if (io_inflight_) io_inflight_--;
    if (result < 0) {
        finalization_failed();
        return;
    }
    if (request->role == SnapshotIoFileSync) {
        if (!finish_file_metadata(&ring)) finalization_failed();
        return;
    }
    if (request->role == SnapshotIoDirectorySync) {
        const bool closed = directory_fd_ >= 0 && ::close(directory_fd_) == 0;
        directory_fd_ = -1;
        if (!closed || !complete_file_success()) finalization_failed();
    }
}

uint32_t SnapshotManager::pump_io_completions(ThreadCtx& writer, Ring& ring) {
    return ring.for_each_cqe_filtered([&](io_uring_cqe* cqe) {
        if (ur_kind(cqe->user_data) != UrKind::SnapshotIo) return false;
        on_io_complete(writer, ring, ur_ptr<void>(cqe->user_data), cqe->res);
        return true;
    });
}

void SnapshotManager::abort_file() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    if (directory_fd_ >= 0) { ::close(directory_fd_); directory_fd_ = -1; }
    if (!temp_path_.empty()) (void)::unlink(temp_path_.c_str());
    if (server_) server_->set_snapshot_atomic_barrier(false);
    writer_tid_.store(UINT32_MAX, std::memory_order_relaxed);
    writer_ring_.store(nullptr, std::memory_order_release);
    if (rewrite_) rewrite_->rewrite_abort();
    rewrite_ = nullptr;
    server_ = nullptr;
}

void SnapshotManager::discard_chunks() {
    for (uint32_t word = 0; word < NotifyMask::kWords; word++) (void)chunk_notify_.take(word);
    for (uint32_t producer = 0; producer < nthreads_; producer++) {
        SnapshotChunk* chunk = nullptr;
        while (chunk_in_[producer].recv(chunk)) {
            delete chunk;
            chunk_in_[producer].retire();
        }
    }
}

uint32_t SnapshotManager::writer_pass(ThreadCtx& writer, Ring& ring, bool drain_all) {
    if (writer.id() != writer_tid_.load(std::memory_order_relaxed)) return 0;
    if (phase() == Phase::Failed) {
        if (cancelled_owners_.load(std::memory_order_acquire) +
                finished_owners_.load(std::memory_order_acquire) == executor_count_ &&
            io_inflight_ == 0) {
            discard_chunks();
            abort_file();
            phase_.store(Phase::Idle, std::memory_order_release);
        }
        return 0;
    }
    if (phase() != Phase::Capture) return 0;
    if (!header_complete_) return 0;
    if (engine_ == PersistIoEngine::Uring && io_inflight_ >= kSnapshotMaxInflight) return 0;

    uint32_t budget = drain_all ? 64 : kWriterFramesPerPass;
    if (engine_ == PersistIoEngine::Uring)
        budget = std::min(budget, kSnapshotMaxInflight - io_inflight_);
    uint32_t consumed = 0;
    auto consume_chunk = [&](uint32_t producer, SnapshotChunk* chunk) {
        chunk_in_[producer].retire();
        bool ok = false;
        if (engine_ == PersistIoEngine::Normal) {
            ok = write_frame_normal(*chunk);
            delete chunk;
        } else {
            ok = submit_frame_uring(std::unique_ptr<SnapshotChunk>(chunk), ring);
        }
        budget--;
        consumed++;
        if (!ok) fail(epoch(), "snapshot file write failed");
        return ok;
    };
    for (uint32_t word = 0; word < NotifyMask::kWords && budget; word++) {
        uint64_t bits = chunk_notify_.take(word);
        while (bits && budget) {
            const uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(bits));
            bits &= bits - 1;
            const uint32_t producer = word * 64 + bit;
            if (producer >= nthreads_) continue;
            SnapshotChunk* chunk = nullptr;
            while (budget && chunk_in_[producer].recv(chunk)) {
                if (!consume_chunk(producer, chunk)) return consumed;
            }
            if (chunk_in_[producer].depth()) chunk_notify_.set(producer);
        }
        // Preserve producers not visited because this pass hit its work budget.
        while (bits) {
            const uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(bits));
            bits &= bits - 1;
            chunk_notify_.set(word * 64 + bit);
        }
    }
    // Mask-independent backstop on the writer's sleep/SAVE path, matching the request channels:
    // notify bits are a discovery optimization, never the sole correctness mechanism.
    if (drain_all && consumed == 0) {
        for (uint32_t producer = 0; producer < nthreads_ && budget; producer++) {
            SnapshotChunk* chunk = nullptr;
            while (budget && chunk_in_[producer].recv(chunk)) {
                if (!consume_chunk(producer, chunk)) return consumed;
            }
        }
    }
    if (ended_shards_ == nshards_ &&
        finished_owners_.load(std::memory_order_acquire) == executor_count_ &&
        io_inflight_ == 0 && !footer_submitted_) {
        const bool finishing = engine_ == PersistIoEngine::Normal
            ? finish_file_normal() : submit_footer_uring(ring);
        if (!finishing) {
            set_error("could not finalize snapshot file");
            writer_failed_.store(true, std::memory_order_relaxed);
            abort_file();
            phase_.store(Phase::Idle, std::memory_order_release);
        }
    }
    return consumed;
}

std::unique_ptr<SnapshotLoadPlan> snapshot_read_plan(const char* path, uint32_t expected_shards,
                                                     std::string& error) {
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { error = "could not open snapshot"; return nullptr; }
    std::vector<uint8_t> file;
    const bool read_ok = read_all_fd(fd, file, error);
    ::close(fd);
    if (!read_ok) return nullptr;
    if (file.size() < kFileHeaderBytes + kFooterBytes ||
        std::memcmp(file.data(), kFileMagic, sizeof(kFileMagic)) != 0 ||
        snapshot_get_u32(file.data() + 8) != kSnapshotFormatVersion ||
        snapshot_get_u32(file.data() + 12) != kFileHeaderBytes ||
        snapshot_get_u64(file.data() + 64) != snapshot_checksum(file.data(), 64)) {
        error = "invalid snapshot header";
        return nullptr;
    }
    auto plan = std::make_unique<SnapshotLoadPlan>();
    plan->shard_count = snapshot_get_u32(file.data() + 16);
    if (plan->shard_count != expected_shards) {
        error = "snapshot shard count does not match --shards";
        return nullptr;
    }
    plan->hash_kind = snapshot_get_u32(file.data() + 20);
    if (plan->hash_kind > static_cast<uint32_t>(HashKind::SipHash12)) {
        error = "snapshot uses an unknown hash kind";
        return nullptr;
    }
    plan->epoch = snapshot_get_u64(file.data() + 24);
    plan->cut_ms = static_cast<int64_t>(snapshot_get_u64(file.data() + 32));
    plan->hash_seed = snapshot_get_u64(file.data() + 40);
    plan->sip_k0 = snapshot_get_u64(file.data() + 48);
    plan->sip_k1 = snapshot_get_u64(file.data() + 56);
    plan->sections.resize(plan->shard_count);
    std::vector<uint32_t> sequence(plan->shard_count, 0);
    std::vector<uint8_t> began(plan->shard_count, 0), ended(plan->shard_count, 0);

    size_t pos = kFileHeaderBytes;
    uint64_t frames = 0;
    while (pos + kFooterBytes <= file.size() && snapshot_get_u32(file.data() + pos) == kFrameTag) {
        if (pos + kFrameHeaderBytes > file.size()) { error = "truncated snapshot frame"; return nullptr; }
        const uint8_t* h = file.data() + pos;
        const uint32_t sid = snapshot_get_u32(h + 4);
        const uint32_t seq = snapshot_get_u32(h + 8);
        const uint32_t flags = snapshot_get_u32(h + 12);
        const uint32_t len = snapshot_get_u32(h + 16);
        const uint64_t checksum = snapshot_get_u64(h + 20);
        pos += kFrameHeaderBytes;
        if (sid >= plan->shard_count || seq != sequence[sid]++ || ended[sid] ||
            pos + len > file.size() || checksum != snapshot_checksum(file.data() + pos, len)) {
            error = "invalid snapshot frame";
            return nullptr;
        }
        if (flags & SnapshotFrameBegin) {
            if (began[sid]) { error = "duplicate shard section"; return nullptr; }
            began[sid] = 1;
        } else if (!began[sid]) { error = "shard section has no begin frame"; return nullptr; }
        try {
            plan->sections[sid].insert(plan->sections[sid].end(), file.data() + pos,
                                       file.data() + pos + len);
        } catch (const std::bad_alloc&) {
            error = "out of memory assembling shard sections";
            return nullptr;
        }
        pos += len;
        if (flags & SnapshotFrameEnd) ended[sid] = 1;
        frames++;
    }
    if (pos + kFooterBytes != file.size() || snapshot_get_u32(file.data() + pos) != kFooterTag ||
        snapshot_get_u32(file.data() + pos + 4) != plan->shard_count ||
        snapshot_get_u64(file.data() + pos + 8) != plan->epoch ||
        snapshot_get_u64(file.data() + pos + 16) != frames ||
        snapshot_get_u64(file.data() + pos + 24) != snapshot_checksum(file.data() + pos, 24)) {
        error = "snapshot has no valid completion footer";
        return nullptr;
    }
    for (uint32_t sid = 0; sid < plan->shard_count; sid++) {
        if (!began[sid] || !ended[sid]) { error = "incomplete shard section"; return nullptr; }
    }
    return plan;
}

bool snapshot_load_shard(const SnapshotLoadPlan& plan, Server& server, Shard& shard,
                         std::string& error) {
    const int64_t now = realtime_ms();
    const uint32_t sid = static_cast<uint32_t>(shard.id());
    const std::vector<uint8_t>& section = plan.sections[sid];
    size_t pos = 0;
    shard.set_cached_now_ms(now);
    while (pos < section.size()) {
        if (section.size() - pos < kRecordHeaderBytes) { error = "truncated record"; return false; }
        const uint8_t* h = section.data() + pos;
        if (snapshot_get_u32(h) != kRecordTag) { error = "invalid record tag"; return false; }
        const uint8_t type_raw = h[4];
        const uint8_t encoding = h[5];
        const uint32_t key_len = snapshot_get_u32(h + 8);
        const uint64_t payload_len = snapshot_get_u64(h + 16);
        const int64_t expire = static_cast<int64_t>(snapshot_get_u64(h + 24));
        pos += kRecordHeaderBytes;
        if (type_raw > static_cast<uint8_t>(Type::Stream) || payload_len > UINT32_MAX ||
            static_cast<uint64_t>(section.size() - pos) <
                static_cast<uint64_t>(key_len) + payload_len) {
            error = "invalid record lengths";
            return false;
        }
        const Slice key(reinterpret_cast<const char*>(section.data() + pos), key_len);
        pos += key_len;
        const Slice payload(reinterpret_cast<const char*>(section.data() + pos),
                            static_cast<uint32_t>(payload_len));
        pos += static_cast<size_t>(payload_len);
        if (expire >= 0 && expire <= now) continue;
        const uint64_t hash = FlatStore::hash_key(key);
        if (server.router().shard_of(hash) != shard.id()) {
            error = "snapshot key is in the wrong shard section";
            return false;
        }
        if (shard.store().find(hash, key)) { error = "duplicate snapshot key"; return false; }
        KvObj* object = nullptr;
        const SnapshotHookStatus status = snapshot_type_hooks(static_cast<Type>(type_raw)).load(
            key, encoding, expire, payload, shard.type_limits(), object);
        if (status != SnapshotHookStatus::Ok || !object) {
            error = hook_error(status);
            return false;
        }
        if (shard.store().insert(hash, object) != FlatStore::InsertResult::Inserted) {
            kvobj_free(object);
            error = "could not insert loaded key";
            return false;
        }
        shard.store().note_loaded_object(hash, object);
    }
    shard.publish_size();
    return true;
}

bool snapshot_load_owned(const SnapshotLoadPlan& plan, Server& server, ThreadCtx& owner,
                         std::string& error) {
    for (Shard* shard : owner.shards())
        if (!snapshot_load_shard(plan, server, *shard, error)) return false;
    return true;
}

}  // namespace tomo

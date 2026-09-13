// aof.h -- single-file AOF with owner-private logical shard streams.
//
// Executors serialize objects they own and post chunks; they never open or write files. One fixed
// IO thread drains all producer channels into one physical stream. appendonly=no leaves no channel,
// chunk, or producer allocation behind.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../core/signal.h"
#include "../snapshot/format.h"

struct io_uring_sqe;

namespace tomo {

class Config;
enum class AppendFsyncPolicy : uint8_t;
enum class PersistIoEngine : uint8_t;
class FlatStore;
class Op;
class Ring;
class Server;
class Shard;
class SnapshotLoadPlan;
class ThreadCtx;

inline constexpr uint32_t kAofChunkBytes = 64 * 1024;

enum AofFrameFlags : uint32_t {
    AofFrameLargeBegin = 1u << 0,
    AofFrameLargeEnd = 1u << 1,
};

enum class AofRecordKind : uint8_t {
    Put = 1,
    Del = 2,
    Flush = 3,
    Timestamp = 4,
    GroupPut = 5,
    GroupDel = 6,
    GroupCommit = 7,
};

enum class AofRewriteDebugStage : uint8_t {
    None = 0,
    BeforeMark = 1,
    BeforeManifest = 2,
    AfterManifest = 3,
};

struct AofGroupDependency {
    uint32_t sid = 0;
    uint32_t sequence = 0;
};

// Allocated only for a live atomic group while AOF is enabled. Owners retain this decision while
// serializing private candidates; the writer retains it until every named shard fragment precedes
// the GCMT record. The dependency fields below are writer-private after construction.
struct AofGroupDecision {
    std::atomic<uint64_t> ticket{0};
    std::atomic<bool> aborted{false};
    std::atomic<bool> commit_enqueued{false};
    std::vector<uint32_t> participants;
    std::vector<AofGroupDependency> dependencies;
    std::vector<uint8_t> dependency_seen;
};

struct AofChunk {
    int32_t sid = -1;
    uint32_t sequence = 0;
    uint64_t post_sequence = 0;
    uint32_t flags = 0;
    uint32_t records = 0;
    std::vector<uint8_t> bytes;
    std::shared_ptr<AofGroupDecision> group;
    std::vector<uint32_t> group_ticket_offsets;
    bool group_fragment_last = false;
    bool group_commit = false;
};

class AofReplayPlan {
public:
    uint32_t shard_count = 0;
    uint32_t hash_kind = 0;
    uint64_t hash_seed = 0;
    uint64_t sip_k0 = 0;
    uint64_t sip_k1 = 0;
    uint64_t replayed_records = 0;
    uint64_t groups_skipped = 0;
    std::vector<uint32_t> next_sequence;
    std::vector<std::vector<uint8_t>> sections;
    std::vector<uint8_t> control_section;
    std::unordered_set<uint64_t> committed_groups;
};

struct AofOwnerContext {
    uint32_t producer = 0;
    Ring* ring = nullptr;
    LoopSignals* signals = nullptr;
};

class AofManager;

class AofProducer {
public:
    void init(AofManager* manager, int32_t sid, uint32_t next_sequence);
    bool enabled() const;
    int32_t sid() const { return sid_; }

    bool record_post_image(FlatStore& store, uint64_t hash, Slice key,
                           AofOwnerContext& context, uint64_t group = 0);
    bool record_post_image_buffered(FlatStore& store, uint64_t hash, Slice key,
                                    uint64_t group = 0);
    bool record_delete(Slice key, uint64_t group = 0);
    bool record_flush();
    bool begin_group(const std::shared_ptr<AofGroupDecision>& group);
    bool record_group_post_image(FlatStore& store, uint64_t hash, Slice key);
    bool record_group_visible_post_image(FlatStore& store, uint64_t hash, Slice key);
    bool finish_group();
    bool flush(AofOwnerContext& context);
    bool has_pending() const { return build_ || !ready_.empty() || !staged_.empty(); }

private:
    struct PendingGroup {
        std::shared_ptr<AofGroupDecision> decision;
        std::unique_ptr<AofChunk> build;
        std::vector<std::unique_ptr<AofChunk>> chunks;
    };
    struct StagedItem {
        std::unique_ptr<AofChunk> plain;
        PendingGroup group;
    };
    bool record_post_image_impl(FlatStore& store, uint64_t hash, Slice key,
                                AofOwnerContext* context, uint64_t group);
    bool record_bytes(AofRecordKind kind, uint8_t type, uint8_t encoding, Slice key,
                      int64_t expire_at_ms, uint64_t group, const uint8_t* payload,
                      uint64_t payload_len, const SnapshotTypeHooks* hooks,
                      SnapshotSaveCursor* cursor, AofOwnerContext* context);
    bool emit(const uint8_t* bytes, uint64_t length, bool large, AofOwnerContext* context);
    bool seal(uint32_t flags, AofOwnerContext* context);
    bool post_ready(AofOwnerContext& context);
    bool maybe_timestamp(int64_t now_ms, AofOwnerContext* context);
    std::unique_ptr<AofChunk> make_chunk(uint32_t flags = 0);
    std::unique_ptr<AofChunk> make_group_chunk(uint32_t flags = 0);
    bool group_emit(PendingGroup& group, const uint8_t* bytes, uint64_t length, bool large);
    bool group_seal(PendingGroup& group, uint32_t flags);
    bool record_group_bytes(PendingGroup& group, AofRecordKind kind, uint8_t type,
                            uint8_t encoding, Slice key, int64_t expire_at_ms,
                            const SnapshotTypeHooks* hooks,
                            SnapshotSaveCursor* cursor);
    bool record_group_post_image_impl(FlatStore& store, uint64_t hash, Slice key,
                                      bool physical);
    bool make_ready(std::unique_ptr<AofChunk> chunk);
    bool resolve_groups();

    AofManager* manager_ = nullptr;
    int32_t sid_ = -1;
    uint32_t sequence_ = 0;
    int64_t timestamp_second_ = -1;
    std::unique_ptr<AofChunk> build_;
    std::vector<std::unique_ptr<AofChunk>> ready_;
    std::deque<StagedItem> staged_;
    PendingGroup* active_group_ = nullptr;
};

class AofManager {
public:
    AofManager() = default;
    ~AofManager();
    AofManager(const AofManager&) = delete;
    AofManager& operator=(const AofManager&) = delete;

    void init(Server& server, const Config& config, uint32_t nthreads, uint32_t nshards,
              uint32_t writer_tid, const AofReplayPlan* replay);
    bool bind_writer(ThreadCtx& writer, Ring& ring, std::string& error);
    void writer_shutdown(ThreadCtx& writer, Ring& ring);
    uint32_t writer_pass(ThreadCtx& writer, Ring& ring, bool drain_all = false);
    void on_io_complete(ThreadCtx& writer, Ring& ring, void* request, int result);
    bool post_chunk(uint32_t producer, std::unique_ptr<AofChunk>& chunk,
                    Ring& producer_ring, LoopSignals& signals);
    bool request_rewrite();
    bool rewrite_mark(ThreadCtx& writer, Ring& ring, uint64_t snapshot_epoch,
                      int64_t cut_ms, std::string& error);
    bool rewrite_complete(const std::string& base_path, uint64_t snapshot_epoch);
    void rewrite_abort();

    bool configured() const { return configured_; }
    bool recording() const { return recording_.load(std::memory_order_acquire); }
    bool writer_is(uint32_t tid) const { return configured_ && tid == writer_tid_; }
    AppendFsyncPolicy fsync_policy() const {
        return fsync_policy_.load(std::memory_order_acquire);
    }
    void set_fsync_policy(AppendFsyncPolicy policy) {
        fsync_policy_.store(policy, std::memory_order_release);
    }
    bool timestamp_enabled() const { return timestamp_enabled_.load(std::memory_order_relaxed); }
    void set_timestamp_enabled(bool enabled) {
        timestamp_enabled_.store(enabled, std::memory_order_relaxed);
    }
    void set_auto_rewrite_config(uint32_t percentage, uint64_t min_size) {
        auto_rewrite_min_size_.store(min_size, std::memory_order_relaxed);
        auto_rewrite_percentage_.store(percentage, std::memory_order_release);
    }
    uint32_t auto_rewrite_percentage() const {
        return auto_rewrite_percentage_.load(std::memory_order_acquire);
    }
    uint64_t auto_rewrite_min_size() const {
        return auto_rewrite_min_size_.load(std::memory_order_relaxed);
    }
    bool failed() const { return failed_.load(std::memory_order_acquire); }
    uint64_t records_written() const { return records_written_.load(std::memory_order_relaxed); }
    uint64_t replayed_records() const { return replayed_records_.load(std::memory_order_relaxed); }
    uint64_t groups_committed() const { return groups_committed_.load(std::memory_order_relaxed); }
    uint64_t groups_skipped() const { return groups_skipped_.load(std::memory_order_relaxed); }
    uint64_t current_size() const { return current_size_.load(std::memory_order_relaxed); }
    uint64_t pending_chunks() const { return pending_chunks_.load(std::memory_order_acquire); }
    uint64_t posted_sequence() const {
        return posted_sequence_.load(std::memory_order_acquire);
    }
    bool reply_gate_ready(uint64_t target) const;
    void register_send_gate_wait(uint32_t tid);
    uint64_t fsyncs() const { return fsyncs_.load(std::memory_order_relaxed); }
    uint64_t send_gate_waits() const {
        return send_gate_waits_.load(std::memory_order_relaxed);
    }
    // Writer passes that had a ready GCMT in hand and held it back because a large record was open
    // on the physical stream. Non-zero proves the interleave window was entered and closed.
    uint64_t control_defers() const {
        return control_defers_.load(std::memory_order_relaxed);
    }
    bool rewrite_in_progress() const {
        return rewrite_in_progress_.load(std::memory_order_acquire);
    }
    bool rewrite_scheduled() const {
        return rewrite_requested_.load(std::memory_order_acquire);
    }
    bool last_rewrite_ok() const { return last_rewrite_ok_.load(std::memory_order_relaxed); }
    uint64_t base_size() const { return base_size_.load(std::memory_order_relaxed); }
    uint64_t rewrite_base_size() const {
        return rewrite_base_size_.load(std::memory_order_relaxed);
    }
    uint64_t rewrite_requests() const {
        return rewrite_requests_.load(std::memory_order_relaxed);
    }
    uint64_t rewrite_completions() const {
        return rewrite_completions_.load(std::memory_order_relaxed);
    }
    uint64_t history_unlinks() const {
        return history_unlinks_.load(std::memory_order_relaxed);
    }
    uint64_t auto_rewrite_triggers() const {
        return auto_rewrite_triggers_.load(std::memory_order_relaxed);
    }
    uint64_t rewrite_failures() const {
        return rewrite_failures_.load(std::memory_order_relaxed);
    }
    uint32_t consecutive_rewrite_failures() const {
        return consecutive_rewrite_failures_.load(std::memory_order_relaxed);
    }
    uint64_t auto_rewrite_backoff_skips() const {
        return auto_rewrite_backoff_skips_.load(std::memory_order_relaxed);
    }
    void debug_stop_after_group_fragments(uint64_t count) {
        debug_stop_after_fragments_.store(count, std::memory_order_release);
    }
    void debug_rewrite_pause(AofRewriteDebugStage stage) {
        debug_rewrite_pause_.store(stage, std::memory_order_release);
    }

    void fail(const char* message);
    bool wait_until_drained(uint32_t timeout_ms);

private:
    using ChunkChan = Channel<AofChunk*, 64>;
    bool write_header_normal();
    bool write_header_uring(ThreadCtx& writer, Ring& ring, int fd, uint64_t& offset);
    bool write_frame_normal(const AofChunk& chunk);
    bool write_group_commit_normal(AofChunk& chunk);
    bool prepare_group_commit(AofChunk& chunk, uint8_t* header);
    bool submit_frame_uring(std::unique_ptr<AofChunk> chunk, bool group_commit,
                            Ring& ring, io_uring_sqe*& last_write);
    bool mark_post_written(uint64_t sequence);
    bool mark_post_submitted(uint64_t sequence);
    bool mark_post_durable(uint64_t sequence);
    uint32_t maybe_sync(ThreadCtx& writer, Ring& ring, io_uring_sqe* last_write);
    void wake_gate_waiters(ThreadCtx& writer, Ring& ring);
    bool group_dependencies_ready(const AofGroupDecision& group) const;
    void note_group_fragment(const AofChunk& chunk);
    uint32_t drain_pending_commits(uint32_t& budget, Ring& ring, io_uring_sqe*& last_write);
    bool drain_producer(uint32_t producer, uint32_t& budget, uint32_t& consumed,
                        Ring& ring, io_uring_sqe*& last_write);
    uint32_t pump_io_completions(ThreadCtx& writer, Ring& ring);
    bool wait_control_write(ThreadCtx& writer, Ring& ring, int fd, const uint8_t* bytes,
                            size_t length, uint64_t& offset);
    bool wait_control_sync(ThreadCtx& writer, Ring& ring, int fd);
    void discard_chunks();
    bool persist_manifest(const std::string& base_name, uint64_t base_sequence,
                          uint64_t base_epoch, uint64_t base_commit,
                          uint64_t persisted_base_size, uint64_t persisted_rewrite_base_size,
                          const std::vector<std::pair<uint64_t, std::string>>& increments,
                          const std::vector<std::vector<uint32_t>>& increment_starts,
                          std::string& error);
    void maybe_start_rewrite(ThreadCtx& writer, Ring& ring);
    void maybe_schedule_auto_rewrite();
    void maybe_pause_rewrite(AofRewriteDebugStage stage);
    void cleanup_unreferenced_files();
    bool schedule_rewrite(bool automatic);

    bool configured_ = false;
    PersistIoEngine engine_;
    Server* server_ = nullptr;
    uint32_t nthreads_ = 0;
    uint32_t nshards_ = 0;
    uint32_t writer_tid_ = UINT32_MAX;
    std::unique_ptr<ChunkChan[]> chunk_in_;
    NotifyMask chunk_notify_;
    std::atomic<Ring*> writer_ring_{nullptr};
    std::atomic<bool> recording_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> writer_ready_{false};
    std::atomic<uint64_t> pending_chunks_{0};
    std::atomic<uint64_t> posted_sequence_{0};
    std::atomic<uint64_t> written_sequence_{0};
    std::atomic<uint64_t> submitted_sequence_{0};
    std::atomic<uint64_t> durable_sequence_{0};
    std::atomic<uint64_t> records_written_{0};
    std::atomic<uint64_t> replayed_records_{0};
    std::atomic<uint64_t> groups_committed_{0};
    std::atomic<uint64_t> groups_skipped_{0};
    std::atomic<uint64_t> current_size_{0};
    std::atomic<uint64_t> fsyncs_{0};
    std::atomic<uint64_t> send_gate_waits_{0};
    std::atomic<uint64_t> control_defers_{0};
    std::atomic<bool> rewrite_requested_{false};
    std::atomic<bool> rewrite_in_progress_{false};
    std::atomic<bool> last_rewrite_ok_{true};
    std::atomic<uint64_t> base_size_{0};
    std::atomic<uint64_t> rewrite_base_size_{0};
    std::atomic<uint64_t> rewrite_requests_{0};
    std::atomic<uint64_t> rewrite_completions_{0};
    std::atomic<uint64_t> history_unlinks_{0};
    std::atomic<uint64_t> auto_rewrite_triggers_{0};
    std::atomic<uint64_t> rewrite_failures_{0};
    std::atomic<uint32_t> consecutive_rewrite_failures_{0};
    std::atomic<uint64_t> auto_rewrite_backoff_skips_{0};
    std::atomic<int64_t> next_rewrite_retry_ms_{0};
    std::atomic<uint32_t> auto_rewrite_percentage_{100};
    std::atomic<uint64_t> auto_rewrite_min_size_{64ull * 1024 * 1024};
    std::atomic<bool> timestamp_enabled_{false};
    std::atomic<AppendFsyncPolicy> fsync_policy_;
    std::atomic<uint64_t> debug_stop_after_fragments_{0};
    std::atomic<AofRewriteDebugStage> debug_rewrite_pause_{AofRewriteDebugStage::None};
    std::string directory_path_;
    std::string appendfilename_;
    std::string manifest_path_;
    std::string file_path_;
    std::string last_error_;
    int fd_ = -1;
    uint64_t file_offset_ = 0;
    uint64_t last_good_offset_ = 0;
    uint64_t large_record_offset_ = 0;
    uint64_t failed_write_offset_ = UINT64_MAX;
    uint32_t locked_producer_ = UINT32_MAX;
    uint32_t writer_cursor_ = 0;
    uint32_t io_inflight_ = 0;
    uint32_t fsync_inflight_ = 0;
    bool everysec_fsync_inflight_ = false;
    int64_t last_fsync_ms_ = 0;
    std::vector<uint32_t> next_sequence_;
    std::unordered_set<uint64_t> written_out_of_order_;
    std::unordered_set<uint64_t> submitted_out_of_order_;
    std::unordered_set<uint64_t> durable_out_of_order_;
    void* current_uring_write_ = nullptr;
    bool short_sync_needed_ = false;
    NotifyMask gate_waiters_;
    std::vector<std::unique_ptr<AofChunk>> pending_commits_;
    std::string base_name_;
    uint64_t base_sequence_ = 0;
    uint64_t base_epoch_ = 0;
    uint64_t base_commit_ = 0;
    uint64_t active_incr_sequence_ = 1;
    uint64_t rewrite_target_sequence_ = 0;
    std::string rewrite_base_name_;
    std::vector<std::pair<uint64_t, std::string>> increments_;
    std::vector<std::vector<uint32_t>> increment_starts_;
    std::vector<std::string> rewrite_history_;
    bool backoff_reported_ = false;
};

std::string aof_directory_path(const Config& config);
std::string aof_file_path(const Config& config);
std::unique_ptr<AofReplayPlan> aof_read_plan(const char* path, uint32_t expected_shards,
                                             bool truncate_tail, bool& exists,
                                             std::string& warning, std::string& error,
                                             const std::vector<uint32_t>* initial_sequences = nullptr);
bool aof_load_owned(const AofReplayPlan& plan, Server& server, ThreadCtx& owner,
                    std::string& error);
bool aof_load_shard(const AofReplayPlan& plan, Server& server, Shard& shard,
                    std::string& error);
bool aof_read_recovery(const Config& config, uint32_t expected_shards,
                       std::unique_ptr<SnapshotLoadPlan>& base,
                       std::vector<std::unique_ptr<AofReplayPlan>>& increments,
                       std::string& warning, std::string& error);

bool aof_record_local_op(Shard& shard, Op& op, AofOwnerContext& context);

std::shared_ptr<AofGroupDecision> aof_create_group(AofManager& manager,
                                                   const std::vector<uint32_t>& participants);
void aof_abort_group(const std::shared_ptr<AofGroupDecision>& group);
bool aof_commit_group(AofManager& manager, const std::shared_ptr<AofGroupDecision>& group,
                      uint64_t ticket, AofOwnerContext& context);

}  // namespace tomo

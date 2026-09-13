// xshard.h -- arena-backed scatter/gather lowering for multi-key commands.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>
#include "../store/flatstore.h"

namespace tomo {

class Client;
class LoopSignals;
class Op;
class Ring;
class Server;
class Shard;
class ThreadCtx;
struct AofOwnerContext;
struct ScatterState;
struct Task;
struct ScatterDispatch;

enum class XshardStringStoreResult : uint8_t { Stored, Oom, InsertFailed, Maxmemory };
XshardStringStoreResult xshard_store_string(Shard& shard, Slice key, uint64_t hash, Slice value,
                                            int64_t expire_at_ms = -1,
                                            bool integer_encode = true);
XshardStringStoreResult xshard_store_string_notify(Shard& shard, Slice key, uint64_t hash,
                                                   Slice value, int64_t expire_at_ms = -1,
                                                   bool integer_encode = true);
KvObj* xshard_make_string(Slice key, Slice value, int64_t expire_at_ms = -1,
                          bool integer_encode = true);
KvObj* xshard_make_atomic_string(Shard& shard, Slice key, Slice value,
                                 int64_t expire_at_ms = -1,
                                 bool integer_encode = true);

// Multi-key pops choose a key after their first-hop probes, then ask that key's live owner to do
// the mutation.  Keeping these helpers in the type lanes preserves their cursor/encoding rules and
// the ObjectSizeTracker finish-before-erase contract instead of replaying a stale whole-object
// image on hop two.
enum class XshardPopResult : uint8_t { Popped, Missing, WrongType, Oom };
XshardPopResult xshard_pop_list(Shard& shard, Slice key, uint64_t hash, bool left,
                                uint64_t count, std::vector<std::string>& elements);
XshardPopResult xshard_pop_zset(Shard& shard, Slice key, uint64_t hash, bool maximum,
                                uint64_t count, std::vector<std::string>& members,
                                std::vector<double>& scores);

// Element movers probe only the selected source element in hop one, then ask each live owner to
// perform its half of the mutation in hop two.  Unlike ObjectImage replay, these helpers preserve
// unrelated writes that landed between the hops.  The list removal's expected value makes the
// ordinary edge case O(1); if a concurrent same-edge push displaced it, the owner removes the
// closest matching value from that edge instead of deleting the concurrent element.
enum class XshardElementResult : uint8_t {
    Ok, Missing, WrongType, Oom, Maxmemory, InsertFailed
};
XshardElementResult xshard_peek_list(KvObj* object, bool left, std::string& element);
XshardElementResult xshard_remove_list_element(Shard& shard, Slice key, uint64_t hash,
                                               bool left, Slice expected);
XshardElementResult xshard_push_list_element(Shard& shard, Slice key, uint64_t hash,
                                             bool left, Slice element);
XshardElementResult xshard_set_contains(KvObj* object, Slice member, bool& contains);
XshardElementResult xshard_remove_set_element(Shard& shard, Slice key, uint64_t hash,
                                              Slice member);
XshardElementResult xshard_insert_set_element(Shard& shard, Slice key, uint64_t hash,
                                              Slice member);

enum class ScatterPrepare : uint8_t { NotScatter, Ready, Backpressure, Error };
enum class ScatterTaskResult : uint8_t { Complete, Retry };
enum class ScatterFinish : uint8_t { Waiting, Retry, Final };

// ONE PUBLISHED READ CUT, whoever holds it.  A read that resolves its fragments on several owners
// needs the versions its cut names to survive until the last fragment has answered, and the only
// thing that keeps cleanup off them is the floor its owning IO thread publishes.  The floor slot is
// per IO THREAD (Server::atomic_read_floors_), so exactly one object per thread may write it: that
// thread's ScatterArenaPool.  Factoring the four bookkeeping fields out of ScatterState lets a
// transaction (MultiExecState) hold a cut through the same pool and the same exact-minimum
// arithmetic, instead of being locked out of the machinery because it carries a pool of its own.
struct SnapshotCut {
    uint64_t snapshot = UINT64_MAX;
    uint32_t snapshot_slot = std::numeric_limits<uint32_t>::max();  // owning IO's unresolved set
    bool snapshot_registered = false;
    std::atomic<bool> snapshot_complete{false};
};

// The common arena size is deliberately a size class, not a maximum command size.  The owning IO
// thread recycles these blocks after ROB retirement; larger shapes use exact-size heap blocks.
// Keeping the freelist here (rather than in ScatterState) makes cross-thread frees impossible by
// construction.
class ScatterArenaPool {
public:
    ScatterArenaPool() = default;
    ~ScatterArenaPool();
    ScatterArenaPool(const ScatterArenaPool&) = delete;
    ScatterArenaPool& operator=(const ScatterArenaPool&) = delete;

private:
    friend ScatterPrepare xshard_prepare(Server&, Op&, ScatterArenaPool&, uint32_t,
                                          uint64_t, ScatterDispatch&, bool, Client*);
    friend void xshard_destroy(ScatterState*, ScatterArenaPool&, uint32_t);
    friend void xshard_retire(Server&, ThreadCtx&, Ring&, Client&, Op&, ScatterArenaPool&,
                              uint32_t, void*, void (*)(void*, int32_t, const char*),
                              void (*)(void*));
    static constexpr uint32_t kCached = 64;
    static constexpr size_t kCommonBytes = 16384;  // holds MGET-8 layouts at the 1KB inline slot
    void* acquire(size_t bytes, bool& pooled);
    void release(void* ptr, bool pooled);
    void defer_destroy(ScatterState* state);
public:
    uint32_t reap_deferred();
    uint32_t refresh_snapshot_floor(Server& server, uint32_t owner_io);
    // `limit` is the per-IO-thread snapshot window; the script engine passes its own.
    bool can_register_snapshot(uint32_t limit = 8) const;
    // CALLER CONTRACT: both run on the IO thread that owns this pool, `owner_io` is that thread's
    // id, and the bracket must enclose every fragment that resolves under the cut -- register
    // before the tasks are posted, unregister only after the last one has answered.  register may
    // advance cut->snapshot to a confirmed newer sequence (the hazard-pointer handshake), so read
    // it back rather than reusing the value you asked for.
    bool register_snapshot(Server& server, SnapshotCut* cut, uint32_t owner_io);
    void unregister_snapshot(Server& server, SnapshotCut* cut, uint32_t owner_io);
private:
    void* cached_[kCached] = {};
    uint32_t count_ = 0;
    // Only unresolved cuts participate in the floor. Atomic work bounds this set at eight; in a
    // read-only interval it is ROB-bounded instead, so the cached minimum/count below make insert
    // and arbitrary removal O(1) without weakening the exact published floor.
    std::vector<SnapshotCut*> unresolved_snapshots_;
    std::vector<ScatterState*> deferred_destroy_;
    uint64_t observed_snapshot_completions_ = 0;
    uint64_t unresolved_snapshot_floor_ = UINT64_MAX;
    uint32_t unresolved_floor_refs_ = 0;
    uint64_t published_snapshot_floor_ = UINT64_MAX;
};

struct ScatterDispatch {
    ScatterState* state = nullptr;
    uint32_t nshards = 0;
    bool barrier = false;
    bool atomic_write = false;
};

// Parses command-specific options and coalesces request key positions by shard.  Error means a
// complete RESP error is already in op; Ready owns `state` until destroy or final completion.
ScatterPrepare xshard_prepare(Server& server, Op& op, ScatterArenaPool& pool,
                              uint32_t owner_io, uint64_t origin_conn_id,
                              ScatterDispatch& dispatch, bool force_atomic = false,
                              Client* origin_client = nullptr);
int32_t xshard_dispatch_shard(const ScatterDispatch& dispatch, uint32_t index);
void xshard_destroy(ScatterState* state, ScatterArenaPool& pool, uint32_t owner_io);

// Called by the connection-owning IO thread immediately before the ROB slot is staged.  It builds
// final RESP bytes/segments, transfers every gathered borrow to the connection segment queue, and
// then returns the arena to this IO thread's pool.
void xshard_retire(Server& server, ThreadCtx& self, Ring& ring, Client& client, Op& op,
                   ScatterArenaPool& pool, uint32_t owner_io,
                   void* release_ctx, void (*release_fn)(void*, int32_t, const char*),
                   void (*suppressed_fn)(void*));

// Same-owner commands are ordinary tasks (Task::scatter == nullptr).  The marker and gate cursor
// reuse otherwise-idle zero-copy descriptor fields, preserving sizeof(Op).
bool xshard_is_local(const Op& op);
void xshard_aof_emit(const Task& task, Shard& shard, Op& op, AofOwnerContext& context);
void xshard_aof_emit_local(Shard& shard, Op& op, AofOwnerContext& context);
FlatStore::SnapshotWriteResult xshard_local_snapshot_prepare(Op& op, Shard& shard);

// Snapshot write gate.  A scatter task may name several mutation keys; progress is remembered in
// the heap group so Pending resumes at exactly the same key on the next owner pass.
FlatStore::SnapshotWriteResult xshard_snapshot_prepare(const Task& task, Shard& shard);

// Executes one bounded owner pass.  KEYS may return Retry; all other tasks complete in one pass.
ScatterTaskResult xshard_execute(const Task& task, Shard& shard, Op& op,
                                 uint32_t owner_thread_id);
void xshard_watch_finish(const Task& task, Shard& shard, Op& op,
                         ScatterTaskResult result);
bool xshard_task_should_defer(Server& server, Shard& shard, const Task& task, Op& op);
bool xshard_tasks_share_key(const Task& older, Op& older_op,
                            const Task& younger, Op& younger_op, int32_t shard_id);
// True for a scatter task whose group names no keys (KEYS, exact DBSIZE, FLUSHDB/FLUSHALL): it
// reads or clears the owner's whole keyspace, so its program-order dependency is per-owner rather
// than per-key.  Used only to attribute the ordering hold that keeps such a walker behind an
// older same-connection task on the same shard.
bool xshard_task_is_whole_owner(const Task& task);

// Counts a completed owner and, for the last owner, either serializes the final reply or publishes
// a fully-preflighted second hop.  Final means the caller must publish OpState::Done and notify IO.
ScatterFinish xshard_complete(Server& server, ThreadCtx& self, Ring& ring,
                              const Task& task, Op& op);

// Ordinary one-key operations only enter this path when their key already has an MVCC record.
// Reads bind the current committed cut; writes first install a deep-cloned, freshly-ticketed
// version so collection handlers may continue mutating in place without touching a predecessor.
bool xshard_plain_prepare(Server& server, Shard& shard, Op& op, uint64_t origin_conn_id);
void xshard_plain_finish(Shard& shard);
uint32_t xshard_cleanup_shard(Server& server, Shard& shard, uint32_t budget = 8);
uint32_t xshard_cleanup_shard_at(Shard& shard, uint64_t floor, uint64_t cleanup_cutoff,
                                 uint32_t budget = 8);

}  // namespace tomo

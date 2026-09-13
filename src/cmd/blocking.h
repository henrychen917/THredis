// blocking.h -- owner-only blocking list/zset waiters.
//
// The implementation is textually included by xshard.cc (blocking.inc), beside the scatter and
// atomic machinery whose publish path it extends.  Keep the public surface opaque: neither Op nor
// Task pays for blocking-only state, and shard owners remain the only threads that touch registries.
#pragma once

#include <cstdint>
#include <vector>
#include "../store/typeval.h"

namespace tomo {

class Client;
class Op;
class Ring;
class Server;
class Shard;
class ThreadCtx;
class ScatterArenaPool;
struct ScatterState;
struct Task;
struct LoopSignals;

struct BlockingState;

enum class BlockingPrepare : uint8_t { Ready, Error };
enum class BlockingSnapshotPrepare : uint8_t { Ready, Pending, Error };

struct BlockingDispatch {
    BlockingState* state = nullptr;
    uint32_t nshards = 0;
};

void blocking_bind_executor(Server* server, ThreadCtx* self, Ring* ring);

BlockingPrepare blocking_prepare(Server& server, Client& client, Op& op, uint64_t op_id,
                                 BlockingDispatch& dispatch);
int32_t blocking_dispatch_shard(const BlockingDispatch& dispatch, uint32_t index);
void blocking_destroy_unpublished(BlockingState* state);
void blocking_start(BlockingState* state, uint32_t tasks);

// Blocking tasks use Task::scatter as an opaque carrier without growing the queue entry.  Op's
// marker distinguishes them from real ScatterState tasks before either pointer is dereferenced.
bool blocking_execute(Server& server, ThreadCtx& self, Ring& ring, const Task& task,
                      Shard& shard, Op& op);
BlockingSnapshotPrepare blocking_snapshot_prepare(const Task& task, Shard& shard, Op& op);

// Called only behind Shard::has_blocking_waiters(), from mutations that can make a waited
// collection non-empty.
void blocking_publish_key(Shard& shard, uint64_t hash, const char* key, uint32_t key_len);
void blocking_publish_list_op(Shard& shard, Op& op);
void blocking_publish_zset_op(Shard& shard, Op& op);
void blocking_defer_plain_publication(bool defer);
void blocking_plain_mutation_published(Shard& shard, Op& op);

// Executor heartbeat and atomic publish integration.
uint32_t blocking_owner_cycle(Server& server, ThreadCtx& self, Ring& ring, int64_t now_ms,
                              bool timeout_beat);
void blocking_atomic_published(Server& server, ThreadCtx& self, Ring& ring,
                               ScatterState& state);
void blocking_scatter_mutation_published(const Task& task, Shard& shard, Op& op);

// IO-side connection barrier, move resume, cancellation and ordered retirement.
bool blocking_resume_move(Server& server, ThreadCtx& self, Ring& ring, Client& client,
                          ScatterArenaPool& pool);
bool blocking_cancel_client(Server& server, ThreadCtx& self, Ring& ring, Client& client);
// CLIENT UNBLOCK: arm the reply shape the cancel is about to need. Owner-io only.
bool blocking_request_unblock(Client& client, bool error_flavor);
void blocking_retire(Server& server, Client& client, Op& op, ThreadCtx& thread);
void blocking_scatter_retire(Server& server, Client& client, ScatterState& state);
bool blocking_stream_bounds(const BlockingState* state, uint32_t key_index, StreamID& cursor,
                            StreamID& upper, bool& bounded);
const std::vector<uint8_t>* blocking_stream_captured_payload(
    const BlockingState* state, uint32_t key_index);
bool blocking_wants_dispatch(const Op& op);

}  // namespace tomo

// multi.h -- MULTI/EXEC and WATCH integration at the atomic snapshot seam.
//
// The implementation is textually included by xshard.cc (multi.inc).  This header intentionally
// exposes only opaque state and the narrow IO/executor hooks needed by the single-TU core.
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace tomo {

class Client;
class IoLoop;
class Op;
class Server;
class Shard;
struct MultiExecState;
struct MultiSession;
struct Task;
struct AofOwnerContext;

enum class MultiIoAction : uint8_t {
    NotHandled,
    LocalDone,
    Dispatch,
    Backpressure,
};

enum class MultiTaskResult : uint8_t {
    Retry,
    Complete,
    Final,
};

// Called after lookup/arity validation.  Transaction controls are handled here; while MULTI is
// active every other accepted command is copied into connection-lived storage and answered QUEUED.
MultiIoAction multi_handle_io(Server& server, Client& client, Op& op, uint32_t owner_io,
                              MultiExecState*& dispatch);
bool multi_queueing(const Client& client);
void multi_mark_queue_error(Client& client);

// The IO loop deliberately contains only predicted-cold calls into these entries.  Keeping the
// transaction dispatch and lifetime machinery in multi.inc preserves the layout of the ordinary
// parse/dispatch and retirement paths.
bool multi_dispatch_entry(IoLoop& loop, Client& client, Op& op, uint32_t consumed);
bool multi_dispatch_entry_iofused(IoLoop& loop, Client& client, Op& op, uint32_t consumed);
void multi_retire_entry(IoLoop& loop, Client& client, Op& op);
uint32_t multi_owner_pass_entry(IoLoop& loop);
uint32_t multi_owner_pass_entry_iofused(IoLoop& loop);
uint32_t multi_owner_reap_entry(IoLoop& loop);
void multi_close_entry(IoLoop& loop, Client& client);
void multi_shutdown_entry(IoLoop& loop);

uint32_t multi_dispatch_count(const MultiExecState* state);
int32_t multi_dispatch_shard(const MultiExecState* state, uint32_t index);
void multi_dispatch_started(Client& client, MultiExecState* state);
void multi_internal_dispatch_started(MultiExecState* state);
void multi_abandon_unpublished(MultiExecState* state);

// Low-bit tagging reuses Task::scatter without changing the 32-byte queue item.
bool multi_task_tagged(const Task& task);
// Does `group_epoch` belong to this fragment's OWN logical unit? The transaction's plain per-key
// installs publish through its own epoch word; every cross-shard command lowered inside the same
// EXEC keeps its own ScatterState and publishes through that one. A caller asking "is there an
// undecided record here that is not mine" has to accept both, or a transaction holds against its
// own child (NOTES-MULTIRES.md §5(a)).
bool multi_task_owns_epoch(const Task& task, const void* group_epoch);
// INFO atomic_exec_order_late: times an EXEC write reached its install with an undecided record
// from another logical unit of the same connection still on this owner -- the ordering violation
// the dispatch-time park in ExLoop::execute() exists to make impossible. It is a BUG counter and
// must read zero; it is separate from atomic_exec_order_holds precisely so that "the hold fired"
// and "the hold fired too late" cannot be confused for one another (NOTES-MULTIRACE.md §5).
uint64_t multi_exec_order_late();
Task multi_make_task(Client* client, uint64_t op_id, int32_t shard, MultiExecState* state);
MultiTaskResult multi_execute_task(Server& server, const Task& task, Shard& shard,
                                   uint32_t owner_thread_id, uint32_t owner_domain,
                                   AofOwnerContext* aof_context);

// IO-side retirement owns state destruction.  Pending MVCC records may retain the shared epoch
// after the EXEC reply, so those states are reaped when their owner-local record refs reach zero.
void multi_retire(Client& client, Op& op, std::vector<MultiExecState*>& deferred);
uint32_t multi_reap_deferred(std::vector<MultiExecState*>& deferred);

// Connection teardown.  A watched connection stays alive until owner tasks remove every registry
// entry; the returned state is dispatched exactly like an internal MULTI task (op_id == UINT64_MAX).
MultiExecState* multi_prepare_close(Server& server, Client& client, uint32_t owner_io);
void multi_session_destroy(MultiSession* session);
bool multi_session_active(const Client& client);
uint32_t multi_session_queue_size(const Client& client);
uint32_t multi_session_watch_size(const Client& client);
uint64_t multi_session_memory(const Client& client);

// Ordinary owner-local write hook.  The false result means an EXEC still holds a WATCH validation
// reservation on one of this command's keys and the task must be retried, not executed early.
bool multi_plain_write_ready(Shard& shard, Op& op);
void multi_plain_write_committed(Shard& shard, Op& op);

}  // namespace tomo

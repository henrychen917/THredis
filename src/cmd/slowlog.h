// slowlog.h — the SLOWLOG + LATENCY recording hook.
//
// ZERO COST WHEN OFF IS THE WHOLE DESIGN. With slowlog-log-slower-than = -1 and
// latency-monitor-threshold = 0 the executor pays exactly one predicted-false branch PER BATCH --
// not per op -- and reads no clock at all. Everything below that gate is out of line.
//
// WHEN ARMED the executor times at BATCH granularity (two clock reads per <=32 ops) and escalates
// to per-op timing only after a batch overruns. See NOTES-SERVERTAIL.md for the attribution
// semantics, which differ from redis's and are documented rather than papered over.
#pragma once
#include <cstdint>

namespace tomo {

class Op;
class Shard;
struct Config;

// Latched once per executor pass from the live-config seqlock; passed by value into the hook so
// the armed test is a register compare, not a memory load, inside the batch loop.
struct SlowlogArm {
    // Microseconds. Negative means the slow log is disabled. 0 means "log everything".
    int64_t  slowlog_us = -1;
    // Milliseconds. 0 means the latency monitor is disabled.
    uint32_t latency_ms = 0;
    bool armed() const { return slowlog_us >= 0 || latency_ms != 0; }
};

// Redis truncation rules for a logged command: at most 32 arguments, at most 128 bytes each.
inline constexpr uint32_t kSlowlogMaxArgs = 32;
inline constexpr uint32_t kSlowlogMaxArgBytes = 128;

// A snapshot of one command's arguments, taken BEFORE it executes.
//
// THIS IS A LIFETIME FIX, NOT AN OPTIMISATION. Op::argv are Slices into the connection's read
// buffer, which is pinned only until the op retires. execute() publishes OpState::Done, and the
// owning IO thread may then retire the op and call reset_rbuf_at_quiescence(), which memmoves and
// can even realloc that buffer. Reading argv from the executor after execute() returns is
// therefore a use-after-retire race -- so the bytes are copied while they are provably alive, into
// a per-executor scratch that is reused for every op and never allocates.
struct SlowlogCapture {
    uint32_t total_argc = 0;                 // argc before truncation
    uint32_t captured = 0;                   // arguments actually stored
    uint32_t used = 0;                       // bytes consumed in `bytes`
    uint32_t off[kSlowlogMaxArgs] = {};
    uint32_t len[kSlowlogMaxArgs] = {};      // stored length, <= kSlowlogMaxArgBytes
    uint32_t full[kSlowlogMaxArgs] = {};     // original length, for the truncation marker
    char     bytes[kSlowlogMaxArgs * kSlowlogMaxArgBytes];
};

// One executor's recorder state. Lives at the cold tail of ExLoop.
struct SlowlogExState {
    // Batches remaining in per-op (exact attribution) mode. Set when a multi-op batch overruns.
    uint32_t escalate_batches = 0;
    SlowlogCapture capture;
};

// Copies the argument bytes out of the op while they are still pinned. Cheap: bounded by 32
// arguments of 128 bytes, and for an ordinary GET/SET it is two short memcpys.
void slowlog_capture(const Op& op, SlowlogCapture& out);

// The executor-side recorder, fed from a capture rather than from the (possibly retired) op.
void slowlog_record_captured(uint32_t thread_id, uint64_t client_id, const SlowlogCapture& capture,
                             uint64_t duration_ns, int64_t now_ms, const SlowlogArm& arm);

// Records one completed operation. `duration_ns` is the measured span; `exact` says whether it is
// this op's own time (batch of one, or escalated per-op timing) or the enclosing batch's time.
// Called only from the armed arm of the executor batch loop.
void slowlog_record(uint32_t thread_id, uint64_t client_id, const Op& op, uint64_t duration_ns,
                    int64_t now_ms, const SlowlogArm& arm, bool exact);

// How many batches stay in per-op timing after an overrun.
inline constexpr uint32_t kSlowlogEscalateBatches = 64;

// Boot seeding from the parsed config, and the live CONFIG SET path.
void slowlog_configure(const Config& config);
void slowlog_set_max_len(uint64_t max_len);

// Mechanism counters. The vacuous-validation rule is load-bearing: a test that cannot see the
// recorder FIRE proves nothing, so these are published in INFO and asserted by tests/slowlog.py.
void slowlog_note_escalation();
void slowlog_note_batch_timed();
uint64_t slowlog_entries_recorded();
uint64_t slowlog_escalations();
uint64_t slowlog_batches_timed();
uint64_t latency_events_recorded();

// Addr/name directory. Connection lifecycle is cold; commands are not, so the slowlog entry
// carries a client id and the address is resolved from this sharded directory at GET time.
void slowlog_note_client(uint64_t client_id, const char* addr);
void slowlog_note_client_name(uint64_t client_id, const char* name, uint32_t name_len);
void slowlog_forget_client(uint64_t client_id);

// Shared by the cold outer-arity error path; writes the complete SLOWLOG/LATENCY reply.
bool slowlog_validate_container_subcommand(Op& op, bool slowlog);

}  // namespace tomo

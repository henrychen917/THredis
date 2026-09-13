// slowlog.cc — SLOWLOG and LATENCY.
//
// THE COST MODEL, which is the point of this file:
//
//   disarmed  (slowlog-log-slower-than -1 AND latency-monitor-threshold 0)
//       one predicted-false branch per EXECUTED BATCH (<=32 ops). No clock read. No allocation.
//       The recorder is never entered, and its ring is never constructed.
//
//   armed, normal mode
//       two now_ns() reads per batch. A batch of ONE op yields that op's exact duration, which is
//       every unpipelined command -- so a genuinely slow command on an ordinary connection is
//       attributed exactly. A batch of many that overruns cannot be attributed after the fact
//       (nothing can re-run it), so it escalates instead.
//
//   armed, escalated mode
//       two now_ns() reads per OP for the next kSlowlogEscalateBatches batches. Exact attribution.
//       A slow command that recurs -- which is what an operator is hunting -- is caught here.
//
// The honest consequence, stated plainly: a command that is slow EXACTLY ONCE while sharing a
// pipelined batch with other commands is not logged; it arms escalation instead, and its next
// occurrence is logged exactly. Redis times every command individually and has no such window.
// The trade buys the default-on hot path back. See NOTES-SERVERTAIL.md.
#include "slowlog.h"

#include "command.h"
#include "../base/slice.h"
#include "../core/config.h"
#include "../core/thread.h"
#include "../exec/op.h"
#include "../net/resp.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tomo {
namespace {

struct SlowlogEntry {
    uint64_t id = 0;
    int64_t  unix_seconds = 0;
    uint64_t duration_us = 0;
    uint64_t client_id = 0;
    bool     exact = true;
    std::vector<std::string> argv;
};

struct SlowlogShard {
    std::mutex mutex;
    std::deque<SlowlogEntry> entries;
};

std::array<SlowlogShard, kMaxThreads> g_slowlog;
std::atomic<uint64_t> g_slowlog_max_len{128};
std::atomic<uint64_t> g_slowlog_next_id{0};

std::atomic<uint64_t> g_entries_recorded{0};
std::atomic<uint64_t> g_escalations{0};
std::atomic<uint64_t> g_batches_timed{0};
std::atomic<uint64_t> g_latency_events{0};

// ---- LATENCY -----------------------------------------------------------------------------------
// Redis keeps <=160 (timestamp, latency) samples per event with same-second coalescing. One event
// class is produced here: 'command'. Everything is behind the same cold mutex as the slow log.
constexpr size_t kLatencyMaxSamples = 160;

struct LatencySample {
    int64_t  unix_seconds = 0;
    uint32_t latency_ms = 0;
};

struct LatencyEvent {
    std::deque<LatencySample> samples;
    uint32_t max_latency_ms = 0;
};

std::mutex g_latency_mu;
std::unordered_map<std::string, LatencyEvent> g_latency;

void latency_add(const char* event, int64_t now_ms, uint32_t latency_ms) {
    const int64_t seconds = now_ms / 1000;
    std::lock_guard<std::mutex> lock(g_latency_mu);
    LatencyEvent& slot = g_latency[event];
    // Same-second samples coalesce to the maximum, exactly as redis's latency monitor does.
    if (!slot.samples.empty() && slot.samples.back().unix_seconds == seconds) {
        slot.samples.back().latency_ms = std::max(slot.samples.back().latency_ms, latency_ms);
    } else {
        slot.samples.push_back(LatencySample{seconds, latency_ms});
        while (slot.samples.size() > kLatencyMaxSamples) slot.samples.pop_front();
    }
    slot.max_latency_ms = std::max(slot.max_latency_ms, latency_ms);
    g_latency_events.fetch_add(1, std::memory_order_relaxed);
}

// ---- client address directory ------------------------------------------------------------------
// SLOWLOG entries carry addr and name. Those live in a thread_local catalog owned by the
// connection's IO thread, which the recording executor cannot read, and the IO thread that later
// answers SLOWLOG GET may be a different one. A tiny sharded directory keyed by the process-unique
// client id closes that gap. It is written only on connect/disconnect/CLIENT SETNAME -- connection
// lifecycle, never the command path.
constexpr size_t kDirectoryShards = 16;

struct ClientDirectoryShard {
    std::mutex mutex;
    std::unordered_map<uint64_t, std::pair<std::string, std::string>> entries;  // addr, name
};

std::array<ClientDirectoryShard, kDirectoryShards> g_directory;

ClientDirectoryShard& directory_shard(uint64_t client_id) {
    return g_directory[client_id % kDirectoryShards];
}

bool directory_lookup(uint64_t client_id, std::string& addr, std::string& name) {
    ClientDirectoryShard& shard = directory_shard(client_id);
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto found = shard.entries.find(client_id);
    if (found == shard.entries.end()) return false;
    addr = found->second.first;
    name = found->second.second;
    return true;
}

bool eq_icase(Slice s, const char* lit) {
    const size_t n = std::strlen(lit);
    if (s.n != n) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char a = static_cast<unsigned char>(s.p[i]);
        unsigned char b = static_cast<unsigned char>(lit[i]);
        if (a >= 'a' && a <= 'z') a -= 'a' - 'A';
        if (b >= 'a' && b <= 'z') b -= 'a' - 'A';
        if (a != b) return false;
    }
    return true;
}

bool parse_i64(Slice s, int64_t& out) {
    if (!s.n || s.n > 20) return false;
    uint32_t i = 0;
    bool negative = false;
    if (s.p[0] == '-') { negative = true; i = 1; }
    if (i >= s.n) return false;
    // Canonical decimal, as redis's string2ll: no leading '+', no leading zeroes, no negative
    // zero. "SLOWLOG GET 05" was accepted before this.
    if (s.p[i] == '0') {
        if (negative || i + 1 != s.n) return false;
        out = 0;
        return true;
    }
    if (s.p[i] < '1' || s.p[i] > '9') return false;
    uint64_t value = 0;
    for (; i < s.n; i++) {
        const char ch = s.p[i];
        if (ch < '0' || ch > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(ch - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    const uint64_t limit = negative ? (uint64_t{1} << 63) : (uint64_t{1} << 63) - 1;
    if (value > limit) return false;
    // INT64_MIN has no positive counterpart, so negating the cast of 2^63 is undefined (UBSAN
    // catches it on "WAIT -9223372036854775808 0"). Name the value instead of computing it.
    out = negative ? (value == (uint64_t{1} << 63) ? std::numeric_limits<int64_t>::min()
                                                   : -static_cast<int64_t>(value))
                   : static_cast<int64_t>(value);
    return true;
}

void reply_help_lines(Op& op, const char* const* lines, size_t count) {
    auto sink = op.sink();
    reply_array_header(sink, count);
    for (size_t i = 0; i < count; i++) reply_simple(sink, lines[i]);
}

// Materializes the redis-shaped argv from a capture: at most 32 arguments, each at most 128 bytes,
// with the overflow named by a synthetic trailing argument.
bool build_argv(const SlowlogCapture& capture, std::vector<std::string>& out) {
    try {
        out.reserve(capture.captured + (capture.total_argc > capture.captured ? 1 : 0));
        for (uint32_t i = 0; i < capture.captured; i++) {
            std::string value(capture.bytes + capture.off[i], capture.len[i]);
            if (capture.full[i] > capture.len[i])
                value += "... (" + std::to_string(capture.full[i] - capture.len[i]) +
                         " more bytes)";
            out.push_back(std::move(value));
        }
        if (capture.total_argc > capture.captured)
            out.push_back("... (" + std::to_string(capture.total_argc - capture.captured) +
                          " more arguments)");
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

// The shared tail of both recorders: latency sampling, threshold test, ring insert.
void slowlog_finish(uint32_t thread_id, SlowlogEntry& entry, uint64_t duration_ns,
                    int64_t now_ms, const SlowlogArm& arm) {
    if (arm.latency_ms) {
        const uint64_t duration_ms = duration_ns / 1000000;
        if (duration_ms >= arm.latency_ms)
            latency_add("command", now_ms, static_cast<uint32_t>(duration_ms));
    }
    if (arm.slowlog_us < 0 || entry.duration_us < static_cast<uint64_t>(arm.slowlog_us)) return;
    if (!g_slowlog_max_len.load(std::memory_order_acquire) || thread_id >= g_slowlog.size()) return;

    entry.id = g_slowlog_next_id.fetch_add(1, std::memory_order_relaxed);
    SlowlogShard& shard = g_slowlog[thread_id];
    {
        std::lock_guard<std::mutex> lock(shard.mutex);
        // Re-read under the lock so a concurrent CONFIG SET slowlog-max-len 0 cannot be raced.
        const uint64_t live_max = g_slowlog_max_len.load(std::memory_order_acquire);
        if (!live_max) return;
        try { shard.entries.push_front(std::move(entry)); }
        catch (const std::bad_alloc&) { return; }
        while (shard.entries.size() > live_max) shard.entries.pop_back();
    }
    g_entries_recorded.fetch_add(1, std::memory_order_relaxed);
}

std::vector<SlowlogEntry> collect_slowlog(size_t limit) {
    std::vector<SlowlogEntry> all;
    for (SlowlogShard& shard : g_slowlog) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        all.insert(all.end(), shard.entries.begin(), shard.entries.end());
    }
    // The id is a single global fetch_add, so it is a total order across executors: newest first.
    std::sort(all.begin(), all.end(),
              [](const SlowlogEntry& a, const SlowlogEntry& b) { return a.id > b.id; });
    if (limit < all.size()) all.resize(limit);
    return all;
}

const char* const kSlowlogHelp[] = {
    "SLOWLOG <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
    "GET [<count>]",
    "    Return top <count> entries from the slowlog (default: 10, -1 mean all).",
    "    Entries are made of:",
    "    id, timestamp, time in microseconds, arguments array, client IP and port,",
    "    client name",
    "LEN",
    "    Return the length of the slowlog.",
    "RESET",
    "    Reset the slowlog.",
    "HELP",
    "    Print this help.",
};

constexpr SubcommandArity kSlowlogSubcommands[] = {
    {"get",   2, 3, SubcommandArityError::UnknownOrWrong},
    {"len",   2, 2},
    {"reset", 2, 2},
    {"help",  2, 2},
};

void cmd_slowlog(Shard&, Op& op) {
    if (!command_validate_subcommand(op, "SLOWLOG", kSlowlogSubcommands,
                                     sizeof(kSlowlogSubcommands) /
                                         sizeof(kSlowlogSubcommands[0]))) return;
    const Slice sub = op.arg(1);
    if (eq_icase(sub, "LEN") && op.argc() == 2) {
        size_t total = 0;
        for (SlowlogShard& shard : g_slowlog) {
            std::lock_guard<std::mutex> lock(shard.mutex);
            total += shard.entries.size();
        }
        reply_int(op.sink(), static_cast<long long>(total));
        return;
    }
    if (eq_icase(sub, "RESET") && op.argc() == 2) {
        for (SlowlogShard& shard : g_slowlog) {
            std::lock_guard<std::mutex> lock(shard.mutex);
            shard.entries.clear();
        }
        reply_ok(op.sink());
        return;
    }
    if (eq_icase(sub, "HELP") && op.argc() == 2) {
        reply_help_lines(op, kSlowlogHelp, sizeof(kSlowlogHelp) / sizeof(kSlowlogHelp[0]));
        return;
    }
    if (eq_icase(sub, "GET") && op.argc() <= 3) {
        int64_t count = 10;
        if (op.argc() == 3 && (!parse_i64(op.arg(2), count) || count < -1)) {
            reply_err(op.sink(), "ERR count should be greater than or equal to -1");
            return;
        }
        const size_t limit = count < 0 ? SIZE_MAX : static_cast<size_t>(count);
        const std::vector<SlowlogEntry> entries = collect_slowlog(limit);
        auto sink = op.sink();
        reply_array_header(sink, entries.size());
        for (const SlowlogEntry& entry : entries) {
            std::string addr, name;
            if (!directory_lookup(entry.client_id, addr, name)) addr = "unknown:0";
            reply_array_header(sink, 6);
            reply_int(sink, static_cast<long long>(entry.id));
            reply_int(sink, static_cast<long long>(entry.unix_seconds));
            reply_int(sink, static_cast<long long>(entry.duration_us));
            reply_array_header(sink, entry.argv.size());
            for (const std::string& arg : entry.argv)
                reply_bulk(sink, Slice(arg.data(), static_cast<uint32_t>(arg.size())));
            reply_bulk(sink, Slice(addr.data(), static_cast<uint32_t>(addr.size())));
            reply_bulk(sink, Slice(name.data(), static_cast<uint32_t>(name.size())));
        }
        return;
    }
}

const char* const kLatencyHelp[] = {
    "LATENCY <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
    "DOCTOR",
    "    Return a human readable latency analysis report.",
    "GRAPH <event>",
    "    Return an ASCII latency graph for the <event> class.",
    "HISTORY <event>",
    "    Return time-latency samples for the <event> class.",
    "LATEST",
    "    Return the latest latency samples for all events.",
    "RESET [<event> ...]",
    "    Reset latency data of one or more <event> classes.",
    "    (default: reset all data for all event classes)",
    "HELP",
    "    Print this help.",
};

constexpr SubcommandArity kLatencySubcommands[] = {
    {"doctor",  2,  2},
    {"graph",   3,  3},
    {"history", 3,  3},
    {"latest",  2,  2},
    {"reset",   2, -1},
    {"help",    2,  2},
};

void cmd_latency(Shard&, Op& op) {
    if (!command_validate_subcommand(op, "LATENCY", kLatencySubcommands,
                                     sizeof(kLatencySubcommands) /
                                         sizeof(kLatencySubcommands[0]))) return;
    const Slice sub = op.arg(1);
    if (eq_icase(sub, "HELP") && op.argc() == 2) {
        reply_help_lines(op, kLatencyHelp, sizeof(kLatencyHelp) / sizeof(kLatencyHelp[0]));
        return;
    }
    if (eq_icase(sub, "LATEST") && op.argc() == 2) {
        std::lock_guard<std::mutex> lock(g_latency_mu);
        auto sink = op.sink();
        size_t live = 0;
        for (const auto& entry : g_latency) live += entry.second.samples.empty() ? 0 : 1;
        reply_array_header(sink, live);
        for (const auto& entry : g_latency) {
            if (entry.second.samples.empty()) continue;
            const LatencySample& last = entry.second.samples.back();
            reply_array_header(sink, 4);
            reply_bulk(sink, Slice(entry.first.data(),
                                   static_cast<uint32_t>(entry.first.size())));
            reply_int(sink, static_cast<long long>(last.unix_seconds));
            reply_int(sink, last.latency_ms);
            reply_int(sink, entry.second.max_latency_ms);
        }
        return;
    }
    if (eq_icase(sub, "HISTORY") && op.argc() == 3) {
        std::lock_guard<std::mutex> lock(g_latency_mu);
        auto sink = op.sink();
        auto found = g_latency.find(std::string(op.arg(2).p, op.arg(2).n));
        if (found == g_latency.end()) { reply_array_header(sink, 0); return; }
        reply_array_header(sink, found->second.samples.size());
        for (const LatencySample& sample : found->second.samples) {
            reply_array_header(sink, 2);
            reply_int(sink, static_cast<long long>(sample.unix_seconds));
            reply_int(sink, sample.latency_ms);
        }
        return;
    }
    if (eq_icase(sub, "RESET")) {
        std::lock_guard<std::mutex> lock(g_latency_mu);
        long long removed = 0;
        if (op.argc() == 2) {
            removed = static_cast<long long>(g_latency.size());
            g_latency.clear();
        } else {
            for (uint32_t i = 2; i < op.argc(); i++)
                removed += g_latency.erase(std::string(op.arg(i).p, op.arg(i).n)) ? 1 : 0;
        }
        reply_int(op.sink(), removed);
        return;
    }
    if (eq_icase(sub, "DOCTOR") && op.argc() == 2) {
        std::string text;
        {
            std::lock_guard<std::mutex> lock(g_latency_mu);
            uint32_t worst = 0;
            const char* worst_event = nullptr;
            for (const auto& entry : g_latency)
                if (entry.second.max_latency_ms > worst) {
                    worst = entry.second.max_latency_ms;
                    worst_event = entry.first.c_str();
                }
            if (!worst_event) {
                text = "No latency spike was observed during the lifetime of this tomokv "
                       "instance. Either the latency monitor is disabled "
                       "(latency-monitor-threshold 0) or nothing has crossed the threshold yet.";
            } else {
                char line[256];
                std::snprintf(line, sizeof(line),
                              "The worst event class is '%s', peaking at %u ms. Use LATENCY "
                              "HISTORY %s to see when, and SLOWLOG GET to see which commands.",
                              worst_event, worst, worst_event);
                text = line;
            }
        }
        reply_verbatim(op.sink(), Slice(text.data(), static_cast<uint32_t>(text.size())), "txt",
                       op.resp3());
        return;
    }
    if (eq_icase(sub, "GRAPH") && op.argc() == 3) {
        std::string text;
        {
            std::lock_guard<std::mutex> lock(g_latency_mu);
            auto found = g_latency.find(std::string(op.arg(2).p, op.arg(2).n));
            if (found == g_latency.end() || found->second.samples.empty()) {
                std::string message = "ERR No samples available for event '";
                message.append(op.arg(2).p, op.arg(2).n);
                message += "'";
                reply_err(op.sink(), message.c_str());
                return;
            }
            const LatencyEvent& event = found->second;
            text = std::string(op.arg(2).p, op.arg(2).n) + " - high " +
                   std::to_string(event.max_latency_ms) + " ms, low " +
                   std::to_string(std::min_element(event.samples.begin(), event.samples.end(),
                       [](const LatencySample& a, const LatencySample& b) {
                           return a.latency_ms < b.latency_ms;
                       })->latency_ms) + " ms (all time high " +
                   std::to_string(event.max_latency_ms) + " ms)\n";
            // Eight rows of vertical bars, oldest sample on the left, exactly like redis's shape.
            const uint32_t peak = event.max_latency_ms ? event.max_latency_ms : 1;
            for (int row = 8; row >= 1; row--) {
                for (const LatencySample& sample : event.samples)
                    text.push_back(sample.latency_ms * 8 / peak >= static_cast<uint32_t>(row)
                                   ? '#' : ' ');
                text.push_back('\n');
            }
        }
        reply_verbatim(op.sink(), Slice(text.data(), static_cast<uint32_t>(text.size())), "txt",
                       op.resp3());
        return;
    }
}

static const CommandSpec kTable[] = {
    // Both are cold introspection commands answered entirely on the connection's IO thread.
    {"SLOWLOG", 2, 3, CmdFlags::ConnLocal | CmdFlags::Admin, cmd_slowlog, 0, 0, 0},
    {"LATENCY", 2, -1, CmdFlags::ConnLocal | CmdFlags::Admin, cmd_latency, 0, 0, 0},
};

}  // namespace

bool slowlog_validate_container_subcommand(Op& op, bool slowlog) {
    if (slowlog)
        return command_validate_subcommand(op, "SLOWLOG", kSlowlogSubcommands,
                                           sizeof(kSlowlogSubcommands) /
                                               sizeof(kSlowlogSubcommands[0]));
    return command_validate_subcommand(op, "LATENCY", kLatencySubcommands,
                                       sizeof(kLatencySubcommands) /
                                           sizeof(kLatencySubcommands[0]));
}

// -------------------------------------------------------------------------------------------
// The two recorders. Both funnel into slowlog_finish() so their semantics cannot drift.
// -------------------------------------------------------------------------------------------

// Copies the arguments out while the connection's read buffer is still pinned. See the header:
// after execute() publishes Done the owning IO thread may retire the op and compact that buffer,
// so the executor must never read argv afterwards.
void slowlog_capture(const Op& op, SlowlogCapture& out) {
    const uint32_t argc = op.argc();
    out.total_argc = argc;
    out.captured = 0;
    out.used = 0;
    // Reserve the last visible slot for the "N more arguments" marker when there is an overflow.
    const uint32_t limit = argc > kSlowlogMaxArgs ? kSlowlogMaxArgs - 1 : argc;
    for (uint32_t i = 0; i < limit; i++) {
        const Slice arg = op.arg(i);
        const uint32_t take = arg.n < kSlowlogMaxArgBytes ? arg.n : kSlowlogMaxArgBytes;
        if (out.used + take > sizeof(out.bytes)) break;
        if (take) std::memcpy(out.bytes + out.used, arg.p, take);
        out.off[out.captured] = out.used;
        out.len[out.captured] = take;
        out.full[out.captured] = arg.n;
        out.captured++;
        out.used += take;
    }
}

// The connection-local path. Safe to read argv directly: the IO loop stores Done only AFTER this
// returns, so the op cannot have retired.
void slowlog_record(uint32_t thread_id, uint64_t client_id, const Op& op, uint64_t duration_ns,
                    int64_t now_ms, const SlowlogArm& arm, bool exact) {
    const uint64_t duration_us = duration_ns / 1000;
    // Skip the capture entirely unless this op will actually be logged.
    const bool wants_entry = arm.slowlog_us >= 0 &&
                             duration_us >= static_cast<uint64_t>(arm.slowlog_us);
    SlowlogEntry entry;
    entry.unix_seconds = now_ms / 1000;
    entry.client_id = client_id;
    entry.duration_us = duration_us;
    entry.exact = exact;
    if (wants_entry) {
        SlowlogCapture capture;
        slowlog_capture(op, capture);
        if (!build_argv(capture, entry.argv)) return;
    }
    slowlog_finish(thread_id, entry, duration_ns, now_ms, arm);
}

// The executor path, fed from a capture taken before execute().
void slowlog_record_captured(uint32_t thread_id, uint64_t client_id, const SlowlogCapture& capture,
                             uint64_t duration_ns, int64_t now_ms, const SlowlogArm& arm) {
    const uint64_t duration_us = duration_ns / 1000;
    SlowlogEntry entry;
    entry.unix_seconds = now_ms / 1000;
    entry.client_id = client_id;
    entry.duration_us = duration_us;
    entry.exact = true;
    if (arm.slowlog_us >= 0 && duration_us >= static_cast<uint64_t>(arm.slowlog_us) &&
        !build_argv(capture, entry.argv)) return;
    slowlog_finish(thread_id, entry, duration_ns, now_ms, arm);
}

void slowlog_note_escalation() { g_escalations.fetch_add(1, std::memory_order_relaxed); }
void slowlog_note_batch_timed() { g_batches_timed.fetch_add(1, std::memory_order_relaxed); }

void slowlog_configure(const Config& config) {
    g_slowlog_max_len.store(config.slowlog_max_len, std::memory_order_release);
}

void slowlog_set_max_len(uint64_t max_len) {
    g_slowlog_max_len.store(max_len, std::memory_order_release);
    for (SlowlogShard& shard : g_slowlog) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        while (shard.entries.size() > max_len) shard.entries.pop_back();
    }
}

uint64_t slowlog_entries_recorded() { return g_entries_recorded.load(std::memory_order_relaxed); }
uint64_t slowlog_escalations() { return g_escalations.load(std::memory_order_relaxed); }
uint64_t slowlog_batches_timed() { return g_batches_timed.load(std::memory_order_relaxed); }
uint64_t latency_events_recorded() { return g_latency_events.load(std::memory_order_relaxed); }

void slowlog_note_client(uint64_t client_id, const char* addr) {
    ClientDirectoryShard& shard = directory_shard(client_id);
    std::lock_guard<std::mutex> lock(shard.mutex);
    try { shard.entries[client_id] = {addr ? addr : "unknown:0", std::string()}; }
    catch (const std::bad_alloc&) {}
}

void slowlog_note_client_name(uint64_t client_id, const char* name, uint32_t name_len) {
    ClientDirectoryShard& shard = directory_shard(client_id);
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto found = shard.entries.find(client_id);
    if (found == shard.entries.end()) return;
    try { found->second.second.assign(name, name_len); }
    catch (const std::bad_alloc&) {}
}

void slowlog_forget_client(uint64_t client_id) {
    ClientDirectoryShard& shard = directory_shard(client_id);
    std::lock_guard<std::mutex> lock(shard.mutex);
    shard.entries.erase(client_id);
}

CommandTable slowlog_command_table() {
    return {kTable, sizeof(kTable) / sizeof(kTable[0])};
}

}  // namespace tomo

// shutdown_report.h -- one immutable end-of-run snapshot for both thread modes.
//
// Collection runs after every worker has joined. Human and machine renderers consume only the
// captured value, so adding cleanup after collection cannot make the two views disagree. The
// final-line guard is declared before every other automatic in main(); its destructor therefore
// emits the machine record after the server, loops, listeners, signal handlers and TLS state have
// all been destroyed.
//
// ONE HOME, so the split (main.cc) and fused (genthread.cc) paths cannot drift again: the fused
// path used to print no per-thread, wb, tls or epoll evidence at all, and the two copies of the
// `stuck:` walk had already diverged in spelling.
//
// Every counter here is read after every loop thread has been joined: LoopSignals, WbEngine::Stats
// and the per-thread client lists have no writer alive, and every Client still present in
// clients() is a live object (reap_dead() runs on each loop's exit path before it returns).
//
// The `stuck:` human line is asserted verbatim by tests/gate.sh
// ("stuck: live_conns=0 rob_not_quiesced=0 unsent_bytes_pending=0"); do not reword it. The gate
// parses only the final `shutdown_report {...}` JSON record, whose names are equally stable.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <utility>
#include <vector>

#include "../net/conn.h"
#include "server.h"

namespace tomo {

class ShutdownReport final {
public:
    enum class Mode : uint8_t { Split, Fused };
    enum class ThreadRole : uint8_t { Io, Ex, Fused, Idle };

    ShutdownReport(const ShutdownReport&) = delete;
    ShutdownReport& operator=(const ShutdownReport&) = delete;
    ShutdownReport(ShutdownReport&&) noexcept = default;
    ShutdownReport& operator=(ShutdownReport&&) = delete;
    ~ShutdownReport() = default;

private:
    struct ThreadRow {
        uint64_t ops = 0;
        uint64_t iterations = 0;
        uint64_t busy_ns = 0;
        uint64_t idle_ns = 0;
        uint64_t cpu_ns = 0;
        uint64_t wakes_sent = 0;
        uint64_t wakes_recv = 0;
        uint32_t tid = 0;
        ThreadRole role = ThreadRole::Idle;
    };

    struct Work {
        uint64_t dispatched = 0;
        uint64_t executed = 0;
        uint64_t unified = 0;
    };

    struct Wb {
        uint64_t sends_submitted = 0;
        uint64_t sends_completed = 0;
        uint64_t short_writes = 0;
        uint64_t send_errors = 0;
        uint64_t peer_aborts = 0;
        uint64_t serves = 0;
        uint64_t serves_empty = 0;
        uint64_t bytes_sent = 0;
        uint64_t retired = 0;
        uint64_t direct = 0;
        uint64_t zc_sends = 0;
        uint64_t zc_bytes = 0;
        uint64_t zc_releases = 0;
    };

    struct Tls {
        uint64_t accepts = 0;
        uint64_t handshakes_started = 0;
        uint64_t handshakes_completed = 0;
        uint64_t handshakes_failed = 0;
        uint64_t connections_freed = 0;
        uint64_t want_read = 0;
        uint64_t want_write = 0;
        uint64_t ciphertext_input_bytes = 0;
        uint64_t plaintext_input_bytes = 0;
        uint64_t ciphertext_output_bytes = 0;
        uint64_t plaintext_output_bytes = 0;
        uint64_t zc_suppressed = 0;
        uint64_t ktls_active = 0;
        uint64_t ktls_fallback = 0;
    };

    struct Stuck {
        uint64_t live_connections = 0;
        uint64_t rob_not_quiesced = 0;
        uint64_t unsent_bytes_pending = 0;
        uint64_t slots_done = 0;
        uint64_t slots_issued = 0;
        uint64_t slots_free = 0;
        uint64_t flag_set = 0;
    };

    struct Epoll {
        bool enabled = false;
        uint64_t events = 0;
        uint64_t recvs = 0;
    };

    struct Accept {
        uint64_t accepts = 0;
        uint64_t errors = 0;
        uint64_t rearms = 0;
        uint64_t sqe_starved = 0;
        uint64_t notify_drop = 0;
    };

#ifdef TOMO_WEDGE_FORENSICS
    struct StrandedClient {
        uint32_t claims = 0;
        uint32_t defers = 0;
        uint32_t serves = 0;
        uint32_t inflight = 0;
        bool flag = false;
    };
    std::vector<StrandedClient> stranded_clients_;
#endif

    ShutdownReport() = default;

    Mode mode_ = Mode::Split;
    uint32_t thread_count_ = 0;
    std::array<ThreadRow, kMaxThreads> threads_{};
    Work work_{};
    Wb wb_{};
    Tls tls_{};
    Stuck stuck_{};
    Epoll epoll_{};
    Accept accept_{};

    template <class IoLoops, class ExecutionLoops>
    friend ShutdownReport collect_shutdown_report(Server&, IoLoops&, ExecutionLoops&);
    friend void print_shutdown_report_human(const ShutdownReport&);
    friend void print_shutdown_report_json(const ShutdownReport&);
};

template <class IoLoops, class ExecutionLoops>
ShutdownReport collect_shutdown_report(Server& server, IoLoops& io_loops,
                                        ExecutionLoops& execution_loops) {
    ShutdownReport report;
    report.mode_ = server.thread_mode() == ThreadMode::Fused
        ? ShutdownReport::Mode::Fused : ShutdownReport::Mode::Split;
    report.thread_count_ = server.nthreads();
    report.epoll_.enabled = server.cfg().net_io == NetIoEngine::Epoll;

    for (uint32_t tid = 0; tid < server.nthreads(); tid++) {
        const LoopSignals& signals = server.thread(tid).sig();
        ShutdownReport::ThreadRow& row = report.threads_[tid];
        row.tid = tid;
        row.ops = signals.ops;
        row.iterations = signals.iterations;
        row.busy_ns = signals.busy_ns;
        row.idle_ns = signals.idle_ns;
        row.cpu_ns = signals.cpu_ns;
        row.wakes_sent = signals.wakes_sent;
        row.wakes_recv = signals.wakes_recv;
        if (report.mode_ == ShutdownReport::Mode::Fused) {
            row.role = ShutdownReport::ThreadRole::Fused;
            report.work_.unified += signals.ops;
        } else {
            const Role role = server.thread(tid).role();
            row.role = role == Role::Ifid ? ShutdownReport::ThreadRole::Io
                     : role == Role::Ex ? ShutdownReport::ThreadRole::Ex
                                        : ShutdownReport::ThreadRole::Idle;
            // Preserve the established 2s rollup: LoopSignals::ops is lifetime-cumulative and is
            // charged to the thread's final role. Exact per-tenure attribution would require new
            // increments on the operation path, which shutdown reporting must not introduce.
            (role == Role::Ifid ? report.work_.dispatched : report.work_.executed) += signals.ops;
        }

        report.accept_.accepts += signals.accepts;
        report.accept_.errors += signals.accept_err;
        report.accept_.rearms += signals.accept_rearm;
        report.accept_.sqe_starved += signals.sqe_starved;
        report.accept_.notify_drop += signals.notify_drop;

        report.tls_.accepts += signals.tls_accepts;
        report.tls_.handshakes_started += signals.tls_handshakes_started;
        report.tls_.handshakes_completed += signals.tls_handshakes_completed;
        report.tls_.handshakes_failed += signals.tls_handshakes_failed;
        report.tls_.connections_freed += signals.tls_connections_freed;
        report.tls_.want_read += signals.tls_want_read;
        report.tls_.want_write += signals.tls_want_write;
        report.tls_.ciphertext_input_bytes += signals.tls_ciphertext_input_bytes;
        report.tls_.plaintext_input_bytes += signals.tls_plaintext_input_bytes;
        report.tls_.ciphertext_output_bytes += signals.tls_ciphertext_output_bytes;
        report.tls_.plaintext_output_bytes += signals.tls_plaintext_output_bytes;
        report.tls_.zc_suppressed += signals.tls_zc_suppressed;
        report.tls_.ktls_active += signals.tls_ktls_active;
        report.tls_.ktls_fallback += signals.tls_ktls_fallback;

        report.epoll_.events += signals.epoll_events;
        report.epoll_.recvs += signals.epoll_recvs;
    }

    auto accumulate_wb = [&](auto& loops) {
        for (auto& loop : loops) {
            const auto& stats = loop.engine().stats();
            report.wb_.sends_submitted += stats.sends_submitted;
            report.wb_.sends_completed += stats.sends_completed;
            report.wb_.short_writes += stats.short_writes;
            report.wb_.send_errors += stats.send_errors;
            report.wb_.peer_aborts += stats.peer_aborts;
            report.wb_.serves += stats.serves;
            report.wb_.serves_empty += stats.serves_empty;
            report.wb_.bytes_sent += stats.bytes_sent;
            report.wb_.retired += stats.retired;
            report.wb_.direct += stats.direct;
            report.wb_.zc_sends += stats.zc_sends;
            report.wb_.zc_bytes += stats.zc_bytes;
            report.wb_.zc_releases += stats.zc_releases;
        }
    };
    accumulate_wb(io_loops);
    accumulate_wb(execution_loops);

    for (uint32_t tid = 0; tid < server.nthreads(); tid++) {
        for (Client* client : server.thread(tid).clients()) {
            if (!client) continue;
            report.stuck_.live_connections++;
            if (!client->rob().quiesced()) {
                report.stuck_.rob_not_quiesced++;
                const bool flag = client->retire_queued().load(std::memory_order_acquire);
                if (flag) report.stuck_.flag_set++;
#ifdef TOMO_WEDGE_FORENSICS
                report.stranded_clients_.push_back({
                    client->n_claims.load(std::memory_order_relaxed),
                    client->n_defers.load(std::memory_order_relaxed),
                    client->n_serves.load(std::memory_order_relaxed),
                    client->rob().in_flight(), flag});
#endif
                for (uint64_t id = client->rob().flush_id(), end = client->rob().dispatch_id();
                     id != end; id++) {
                    switch (client->rob().at(id).state.load(std::memory_order_acquire)) {
                        case OpState::Done: report.stuck_.slots_done++; break;
                        case OpState::Issued: report.stuck_.slots_issued++; break;
                        default: report.stuck_.slots_free++; break;
                    }
                }
            }
            if (!client->nothing_to_write()) report.stuck_.unsent_bytes_pending++;
        }
    }
    return report;
}

inline const char* shutdown_thread_role_human(ShutdownReport::ThreadRole role) {
    switch (role) {
        case ShutdownReport::ThreadRole::Io: return "io";
        case ShutdownReport::ThreadRole::Ex: return "ex";
        case ShutdownReport::ThreadRole::Fused: return "uni";
        case ShutdownReport::ThreadRole::Idle: return "idle";
    }
    return "idle";
}

inline const char* shutdown_thread_role_json(ShutdownReport::ThreadRole role) {
    switch (role) {
        case ShutdownReport::ThreadRole::Io: return "io";
        case ShutdownReport::ThreadRole::Ex: return "ex";
        case ShutdownReport::ThreadRole::Fused: return "fused";
        case ShutdownReport::ThreadRole::Idle: return "idle";
    }
    return "idle";
}

inline void print_shutdown_report_human(const ShutdownReport& report) {
    std::printf("\n%-6s %-4s %12s %10s %9s %9s %9s %9s %8s\n",
                "thread", "role", "ops", "iters", "busy_ms", "idle_ms", "cpu_ms",
                "wake_tx", "wake_rx");
    for (uint32_t index = 0; index < report.thread_count_; index++) {
        const ShutdownReport::ThreadRow& row = report.threads_[index];
        std::printf("t%-5u %-4s %12llu %10llu %9.1f %9.1f %9.1f %9llu %8llu\n",
                    row.tid, shutdown_thread_role_human(row.role),
                    static_cast<unsigned long long>(row.ops),
                    static_cast<unsigned long long>(row.iterations),
                    row.busy_ns / 1e6, row.idle_ns / 1e6, row.cpu_ns / 1e6,
                    static_cast<unsigned long long>(row.wakes_sent),
                    static_cast<unsigned long long>(row.wakes_recv));
    }

#ifdef TOMO_WEDGE_FORENSICS
    for (const ShutdownReport::StrandedClient& client : report.stranded_clients_) {
        std::printf("  stranded conn: claims=%u defers=%u serves=%u inflight=%u flag=%d\n",
                    client.claims, client.defers, client.serves, client.inflight,
                    static_cast<int>(client.flag));
    }
#endif

    const ShutdownReport::Wb& wb = report.wb_;
    std::printf("wb: retired=%llu direct=%llu sends=%llu/%llu short=%llu err=%llu"
                " peer_aborts=%llu bytes=%llu"
                " zc_sends=%llu zc_bytes=%llu zc_releases=%llu serves=%llu empty=%llu\n",
                static_cast<unsigned long long>(wb.retired),
                static_cast<unsigned long long>(wb.direct),
                static_cast<unsigned long long>(wb.sends_completed),
                static_cast<unsigned long long>(wb.sends_submitted),
                static_cast<unsigned long long>(wb.short_writes),
                static_cast<unsigned long long>(wb.send_errors),
                static_cast<unsigned long long>(wb.peer_aborts),
                static_cast<unsigned long long>(wb.bytes_sent),
                static_cast<unsigned long long>(wb.zc_sends),
                static_cast<unsigned long long>(wb.zc_bytes),
                static_cast<unsigned long long>(wb.zc_releases),
                static_cast<unsigned long long>(wb.serves),
                static_cast<unsigned long long>(wb.serves_empty));

    const ShutdownReport::Tls& tls = report.tls_;
    std::printf("tls: accepts=%llu handshakes=%llu/%llu failed=%llu freed=%llu"
                " want_read=%llu want_write=%llu cipher_in=%llu plain_in=%llu"
                " cipher_out=%llu plain_out=%llu zc_suppressed=%llu"
                " ktls_active=%llu ktls_fallback=%llu\n",
                static_cast<unsigned long long>(tls.accepts),
                static_cast<unsigned long long>(tls.handshakes_completed),
                static_cast<unsigned long long>(tls.handshakes_started),
                static_cast<unsigned long long>(tls.handshakes_failed),
                static_cast<unsigned long long>(tls.connections_freed),
                static_cast<unsigned long long>(tls.want_read),
                static_cast<unsigned long long>(tls.want_write),
                static_cast<unsigned long long>(tls.ciphertext_input_bytes),
                static_cast<unsigned long long>(tls.plaintext_input_bytes),
                static_cast<unsigned long long>(tls.ciphertext_output_bytes),
                static_cast<unsigned long long>(tls.plaintext_output_bytes),
                static_cast<unsigned long long>(tls.zc_suppressed),
                static_cast<unsigned long long>(tls.ktls_active),
                static_cast<unsigned long long>(tls.ktls_fallback));

    const ShutdownReport::Stuck& stuck = report.stuck_;
    std::printf("stuck: live_conns=%llu rob_not_quiesced=%llu unsent_bytes_pending=%llu"
                " | slots done=%llu issued=%llu free=%llu flag_set=%llu\n",
                static_cast<unsigned long long>(stuck.live_connections),
                static_cast<unsigned long long>(stuck.rob_not_quiesced),
                static_cast<unsigned long long>(stuck.unsent_bytes_pending),
                static_cast<unsigned long long>(stuck.slots_done),
                static_cast<unsigned long long>(stuck.slots_issued),
                static_cast<unsigned long long>(stuck.slots_free),
                static_cast<unsigned long long>(stuck.flag_set));

    if (report.epoll_.enabled) {
        std::printf("epoll: events=%llu recvs=%llu\n",
                    static_cast<unsigned long long>(report.epoll_.events),
                    static_cast<unsigned long long>(report.epoll_.recvs));
    }

    const ShutdownReport::Accept& accept = report.accept_;
    if (report.mode_ == ShutdownReport::Mode::Split) {
        std::printf("shutdown: dispatched=%llu executed=%llu accepts=%llu accept_err=%llu "
                    "rearm=%llu sqe_starved=%llu notify_drop=%llu\n",
                    static_cast<unsigned long long>(report.work_.dispatched),
                    static_cast<unsigned long long>(report.work_.executed),
                    static_cast<unsigned long long>(accept.accepts),
                    static_cast<unsigned long long>(accept.errors),
                    static_cast<unsigned long long>(accept.rearms),
                    static_cast<unsigned long long>(accept.sqe_starved),
                    static_cast<unsigned long long>(accept.notify_drop));
    } else {
        std::printf("shutdown: unified_work=%llu accepts=%llu accept_err=%llu rearm=%llu"
                    " sqe_starved=%llu notify_drop=%llu\n",
                    static_cast<unsigned long long>(report.work_.unified),
                    static_cast<unsigned long long>(accept.accepts),
                    static_cast<unsigned long long>(accept.errors),
                    static_cast<unsigned long long>(accept.rearms),
                    static_cast<unsigned long long>(accept.sqe_starved),
                    static_cast<unsigned long long>(accept.notify_drop));
    }
}

inline void print_shutdown_report_json(const ShutdownReport& report) {
    std::printf("shutdown_report {\"schema\":1,\"thread_mode\":\"%s\",\"work\":{",
                report.mode_ == ShutdownReport::Mode::Fused ? "1s" : "2s");
    if (report.mode_ == ShutdownReport::Mode::Fused) {
        std::printf("\"kind\":\"fused\",\"unified\":%llu",
                    static_cast<unsigned long long>(report.work_.unified));
    } else {
        std::printf("\"kind\":\"split\",\"dispatched\":%llu,\"executed\":%llu",
                    static_cast<unsigned long long>(report.work_.dispatched),
                    static_cast<unsigned long long>(report.work_.executed));
    }
    std::printf("},\"threads\":[");
    for (uint32_t index = 0; index < report.thread_count_; index++) {
        const ShutdownReport::ThreadRow& row = report.threads_[index];
        if (index) std::putchar(',');
        std::printf("{\"tid\":%u,\"role\":\"%s\",\"ops\":%llu,\"iterations\":%llu,"
                    "\"busy_ns\":%llu,\"idle_ns\":%llu,\"cpu_ns\":%llu,"
                    "\"wake_tx\":%llu,\"wake_rx\":%llu}",
                    row.tid, shutdown_thread_role_json(row.role),
                    static_cast<unsigned long long>(row.ops),
                    static_cast<unsigned long long>(row.iterations),
                    static_cast<unsigned long long>(row.busy_ns),
                    static_cast<unsigned long long>(row.idle_ns),
                    static_cast<unsigned long long>(row.cpu_ns),
                    static_cast<unsigned long long>(row.wakes_sent),
                    static_cast<unsigned long long>(row.wakes_recv));
    }
    const ShutdownReport::Wb& wb = report.wb_;
    std::printf("],\"wb\":{\"retired\":%llu,\"direct\":%llu,"
                "\"sends_completed\":%llu,\"sends_submitted\":%llu,"
                "\"short_writes\":%llu,\"send_errors\":%llu,\"peer_aborts\":%llu,"
                "\"bytes_sent\":%llu,\"zc_sends\":%llu,\"zc_bytes\":%llu,"
                "\"zc_releases\":%llu,\"serves\":%llu,\"serves_empty\":%llu}",
                static_cast<unsigned long long>(wb.retired),
                static_cast<unsigned long long>(wb.direct),
                static_cast<unsigned long long>(wb.sends_completed),
                static_cast<unsigned long long>(wb.sends_submitted),
                static_cast<unsigned long long>(wb.short_writes),
                static_cast<unsigned long long>(wb.send_errors),
                static_cast<unsigned long long>(wb.peer_aborts),
                static_cast<unsigned long long>(wb.bytes_sent),
                static_cast<unsigned long long>(wb.zc_sends),
                static_cast<unsigned long long>(wb.zc_bytes),
                static_cast<unsigned long long>(wb.zc_releases),
                static_cast<unsigned long long>(wb.serves),
                static_cast<unsigned long long>(wb.serves_empty));

    const ShutdownReport::Tls& tls = report.tls_;
    std::printf(",\"tls\":{\"accepts\":%llu,\"handshakes_completed\":%llu,"
                "\"handshakes_started\":%llu,\"handshakes_failed\":%llu,"
                "\"connections_freed\":%llu,\"want_read\":%llu,\"want_write\":%llu,"
                "\"ciphertext_input_bytes\":%llu,\"plaintext_input_bytes\":%llu,"
                "\"ciphertext_output_bytes\":%llu,\"plaintext_output_bytes\":%llu,"
                "\"zc_suppressed\":%llu,\"ktls_active\":%llu,\"ktls_fallback\":%llu}",
                static_cast<unsigned long long>(tls.accepts),
                static_cast<unsigned long long>(tls.handshakes_completed),
                static_cast<unsigned long long>(tls.handshakes_started),
                static_cast<unsigned long long>(tls.handshakes_failed),
                static_cast<unsigned long long>(tls.connections_freed),
                static_cast<unsigned long long>(tls.want_read),
                static_cast<unsigned long long>(tls.want_write),
                static_cast<unsigned long long>(tls.ciphertext_input_bytes),
                static_cast<unsigned long long>(tls.plaintext_input_bytes),
                static_cast<unsigned long long>(tls.ciphertext_output_bytes),
                static_cast<unsigned long long>(tls.plaintext_output_bytes),
                static_cast<unsigned long long>(tls.zc_suppressed),
                static_cast<unsigned long long>(tls.ktls_active),
                static_cast<unsigned long long>(tls.ktls_fallback));

    const ShutdownReport::Stuck& stuck = report.stuck_;
    std::printf(",\"stuck\":{\"live_conns\":%llu,\"rob_not_quiesced\":%llu,"
                "\"unsent_bytes_pending\":%llu,\"slots_done\":%llu,"
                "\"slots_issued\":%llu,\"slots_free\":%llu,\"flag_set\":%llu}",
                static_cast<unsigned long long>(stuck.live_connections),
                static_cast<unsigned long long>(stuck.rob_not_quiesced),
                static_cast<unsigned long long>(stuck.unsent_bytes_pending),
                static_cast<unsigned long long>(stuck.slots_done),
                static_cast<unsigned long long>(stuck.slots_issued),
                static_cast<unsigned long long>(stuck.slots_free),
                static_cast<unsigned long long>(stuck.flag_set));

    std::printf(",\"epoll\":{\"enabled\":%s,\"events\":%llu,\"recvs\":%llu}",
                report.epoll_.enabled ? "true" : "false",
                static_cast<unsigned long long>(report.epoll_.events),
                static_cast<unsigned long long>(report.epoll_.recvs));
    const ShutdownReport::Accept& accept = report.accept_;
    std::printf(",\"accept\":{\"accepts\":%llu,\"accept_err\":%llu,\"rearm\":%llu,"
                "\"sqe_starved\":%llu,\"notify_drop\":%llu}}\n",
                static_cast<unsigned long long>(accept.accepts),
                static_cast<unsigned long long>(accept.errors),
                static_cast<unsigned long long>(accept.rearms),
                static_cast<unsigned long long>(accept.sqe_starved),
                static_cast<unsigned long long>(accept.notify_drop));
}

// Declare this before every other automatic in main(). Once armed, its destructor is the final
// output-producing lifetime edge in the process's normal shutdown path.
class ShutdownReportFinalLine final {
public:
    ShutdownReportFinalLine() = default;
    ShutdownReportFinalLine(const ShutdownReportFinalLine&) = delete;
    ShutdownReportFinalLine& operator=(const ShutdownReportFinalLine&) = delete;

    ~ShutdownReportFinalLine() {
        if (!report_) return;
        std::fflush(stderr);
        print_shutdown_report_json(*report_);
        std::fflush(stdout);
    }

    void arm(ShutdownReport&& report) { report_.emplace(std::move(report)); }

private:
    std::optional<ShutdownReport> report_;
};

}  // namespace tomo

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <string>
#include <unistd.h>
#include <vector>

#include "src/core/config.h"

namespace {

[[noreturn]] void fail(const char* message) {
    std::fprintf(stderr, "config parser test: %s\n", message);
    std::exit(1);
}

// Negative probes deliberately trip the parser's own error messages; those belong to the parser,
// not to the gate log, so the probes run with stderr parked on /dev/null.
struct StderrSilencer {
    int saved = -1;
    StderrSilencer() {
        std::fflush(stderr);
        saved = ::dup(STDERR_FILENO);
        const int null_fd = ::open("/dev/null", O_WRONLY);
        if (saved < 0 || null_fd < 0 || ::dup2(null_fd, STDERR_FILENO) < 0)
            fail("silencing stderr failed");
        ::close(null_fd);
    }
    ~StderrSilencer() {
        std::fflush(stderr);
        ::dup2(saved, STDERR_FILENO);
        ::close(saved);
    }
};

std::string rejection_text(std::initializer_list<const char*> values,
                           bool validate = false) {
    std::FILE* capture = std::tmpfile();
    if (!capture) fail("tmpfile for stderr capture failed");
    const int saved_stderr = ::dup(STDERR_FILENO);
    if (saved_stderr < 0) fail("dup stderr failed");
    std::fflush(stderr);
    if (::dup2(::fileno(capture), STDERR_FILENO) < 0) fail("redirect stderr failed");

    tomo::Config cfg;
    tomo::ConfigParseState state;
    const std::vector<const char*> args(values);
    int result = tomo::parse_config_args(args, cfg, state, 2, "test");
    if (validate && result == tomo::kConfigParsed) result = tomo::validate_config(cfg);

    std::fflush(stderr);
    if (::dup2(saved_stderr, STDERR_FILENO) < 0) fail("restore stderr failed");
    ::close(saved_stderr);
    if (result != tomo::kConfigError) fail("rejection-text probe was not rejected");

    std::rewind(capture);
    std::string output;
    char block[256];
    while (std::fgets(block, sizeof(block), capture)) output += block;
    std::fclose(capture);
    return output;
}

}  // namespace

int main() {
    char path[] = "/tmp/tomokv-config-parser.XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0) fail("mkstemp failed");
    std::FILE* file = ::fdopen(fd, "w");
    if (!file) fail("fdopen failed");

    std::fputs("  # leading comments are skipped after trimming\n", file);
    std::fputs("user alice on #0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef >\"pass phrase\" ~*\n",
               file);
    std::fputs("requirepass 'single quoted value'\n", file);
    if (std::fclose(file) != 0) fail("fclose failed");

    std::vector<std::string> tokens;
    const bool loaded = tomo::load_conf_file(path, tokens);
    ::unlink(path);
    if (!loaded) fail("load_conf_file rejected valid Redis quoting");

    const std::vector<std::string> expected = {
        "--user",
        "alice",
        "on",
        "#0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        ">pass phrase",
        "~*",
        "--requirepass",
        "single quoted value",
    };
    if (tokens != expected) fail("mid-value '#' or quoted token did not survive exactly");

    std::vector<std::string> inline_hash;
    if (!tomo::cfg_split_args("port 7953 #not-an-inline-comment", inline_hash) ||
        inline_hash.size() != 3 || inline_hash[2] != "#not-an-inline-comment")
        fail("inline '#' was treated as a comment");

    std::vector<std::string> escaped;
    if (!tomo::cfg_split_args("requirepass \"a\\n\\x23b\"", escaped) ||
        escaped.size() != 2 || escaped[1] != "a\n#b")
        fail("double-quoted Redis escapes were not decoded");

    std::vector<std::string> malformed;
    if (tomo::cfg_split_args("requirepass \"unterminated", malformed) ||
        tomo::cfg_split_args("requirepass \"closed\"suffix", malformed))
        fail("malformed Redis quoting was accepted");

    tomo::Config tls;
    tomo::ConfigParseState tls_state;
    const std::vector<const char*> tls_args = {
        "--port", "0", "--tls-port", "7953",
        "--tls-cert-file", "/cert.pem", "--tls-key-file", "/key.pem",
        "--tls-ca-cert-file", "/ca.pem", "--tls-ca-cert-dir", "/ca-dir",
        "--tls-auth-clients", "OpTiOnAl",
        "--tls-protocols", "TLSv1.2 TLSv1.3",
        "--tls-ciphers", "DEFAULT", "--tls-ciphersuites", "TLS_AES_256_GCM_SHA384",
        "--tls-prefer-server-ciphers", "YeS",
    };
    if (tomo::parse_config_args(tls_args, tls, tls_state, 2, "test") != tomo::kConfigParsed ||
        tomo::validate_config(tls) != tomo::kConfigParsed)
        fail("valid TLS Redis grammar was rejected");
    if (tls.port != 0 || tls.tls_port != 7953 ||
        tls.tls_auth_clients != tomo::TlsAuthClients::Optional ||
        !tls.tls_prefer_server_ciphers ||
        std::strcmp(tls.tls_protocols, "TLSv1.2 TLSv1.3") ||
        std::strcmp(tls.tls_ciphers, "DEFAULT") ||
        std::strcmp(tls.tls_ciphersuites, "TLS_AES_256_GCM_SHA384"))
        fail("TLS knob values were not preserved byte-exactly");

    auto rejects = [](std::initializer_list<const char*> values) {
        StderrSilencer quiet;
        tomo::Config cfg;
        tomo::ConfigParseState state;
        const std::vector<const char*> args(values);
        return tomo::parse_config_args(args, cfg, state, 2, "test") == tomo::kConfigError;
    };
    if (!rejects({"--tls-port", "65536"}) ||
        !rejects({"--tls-port", "-1"}) ||
        !rejects({"--tls-auth-clients", "true"}) ||
        !rejects({"--tls-prefer-server-ciphers", "1"}))
        fail("invalid TLS grammar was accepted");

    // --shards follows the same numeric grammar as every other knob: the range is enforced at
    // parse time and garbage is rejected rather than atoi'd into a misleading range message.
    tomo::Config shards;
    tomo::ConfigParseState shards_state;
    const std::vector<const char*> shards_args = {"--shards", "256"};
    if (tomo::parse_config_args(shards_args, shards, shards_state, 2, "test") !=
            tomo::kConfigParsed ||
        shards.shards != 256 ||
        !rejects({"--shards", "0"}) ||
        !rejects({"--shards", "257"}) ||
        !rejects({"--shards", "abc"}) ||
        !rejects({"--shards", "-5"}) ||
        !rejects({"--shards", "16x"}) ||
        !rejects({"--shards", ""}))
        fail("shards boot grammar differs");
    if (rejection_text({"--shards", "16x"}) != "--shards must be between 1 and 256\n")
        fail("shards rejection text is not canonical");
    tomo::Config shards_default;
    if (shards_default.shards != 16) fail("shards default is not 16");

    tomo::Config persistence;
    tomo::ConfigParseState persistence_state;
    const std::vector<const char*> persistence_args = {"--persist-io", "NoRmAl"};
    if (tomo::parse_config_args(persistence_args, persistence, persistence_state, 2, "test") !=
            tomo::kConfigParsed ||
        persistence.persist_io != tomo::PersistIoEngine::Normal ||
        !rejects({"--persist-io", "hybrid"}))
        fail("persist-io boot grammar differs");

    // --net-io mirrors --persist-io exactly: named enum, case insensitive, boot-only, and a
    // NEGATIVE CONTROL so "it parsed" cannot pass for a parser that accepts anything.
    tomo::Config network;
    tomo::ConfigParseState network_state;
    const std::vector<const char*> network_args = {"--net-io", "EpOlL"};
    if (tomo::parse_config_args(network_args, network, network_state, 2, "test") !=
            tomo::kConfigParsed ||
        network.net_io != tomo::NetIoEngine::Epoll ||
        !rejects({"--net-io", "kqueue"}) ||
        !rejects({"--net-io", ""}))
        fail("net-io boot grammar differs");
    tomo::Config network_default;
    if (network_default.net_io != tomo::NetIoEngine::Uring)
        fail("net-io default is not uring");

    tomo::Config threads;
    tomo::ConfigParseState threads_state;
    const std::vector<const char*> threads_args = {
        "--thread-mode", "1s", "--overlap", "2",
    };
    if (tomo::parse_config_args(threads_args, threads, threads_state, 2, "test") !=
            tomo::kConfigParsed ||
        tomo::validate_config(threads) != tomo::kConfigParsed ||
        threads.thread_mode != tomo::ThreadMode::Fused || threads.overlap != 2)
        fail("primary thread-mode/overlap grammar differs");
    tomo::Config thread_default;
    if (thread_default.thread_mode != tomo::ThreadMode::Split ||
        thread_default.overlap != 0)
        fail("thread study defaults are not 2s overlap 0");

    auto parses_threads = [](std::initializer_list<const char*> values,
                             tomo::ThreadMode mode, uint32_t pipeline) {
        tomo::Config cfg;
        tomo::ConfigParseState state;
        const std::vector<const char*> args(values);
        return tomo::parse_config_args(args, cfg, state, 2, "test") == tomo::kConfigParsed &&
               tomo::validate_config(cfg) == tomo::kConfigParsed &&
               cfg.thread_mode == mode && cfg.overlap == pipeline;
    };
    if (!parses_threads({"--thread-mode", "2s", "--overlap", "1"},
                        tomo::ThreadMode::Split, 1) ||
        !parses_threads({"--thread-mode", "1s", "--thread-pipeline", "1"},
                        tomo::ThreadMode::Fused, 1) ||
        !parses_threads({"--thread-mode", "split"}, tomo::ThreadMode::Split, 0) ||
        !parses_threads({"--thread-mode", "fused"}, tomo::ThreadMode::Fused, 0) ||
        !parses_threads({"--genthread-schedule", "coarse"},
                        tomo::ThreadMode::Fused, 0) ||
        !parses_threads({"--genthread-schedule", "IoFuSeD"},
                        tomo::ThreadMode::Fused, 1) ||
        !parses_threads({"--genthread-schedule", "streams"},
                        tomo::ThreadMode::Fused, 2))
        fail("thread-mode or genthread compatibility aliases differ");
    if (!rejects({"--thread-mode", "two-stage"}) ||
        !rejects({"--overlap", "3"}) ||
        !rejects({"--overlap", "-1"}) ||
        !rejects({"--thread-pipeline", "3"}) ||
        !rejects({"--thread-pipeline", "-1"}) ||
        !rejects({"--genthread-schedule", "streams0"}))
        fail("invalid thread study grammar was accepted");
    if (rejection_text({"--overlap", "3"}) != "--overlap wants 0, 1 or 2\n" ||
        rejection_text({"--thread-pipeline", "-1"}) !=
            "--overlap wants 0, 1 or 2\n" ||
        rejection_text({"--genthread-schedule", "streams0"}) !=
            "--genthread-schedule wants coarse, iofused or streams\n")
        fail("thread-study parser rejection text is not canonical");
    tomo::Config invalid_split_deep;
    tomo::ConfigParseState invalid_split_deep_state;
    const std::vector<const char*> invalid_split_deep_args = {
        "--overlap", "2", "--thread-mode", "2s",
    };
    {
        StderrSilencer quiet;
        if (tomo::parse_config_args(invalid_split_deep_args, invalid_split_deep,
                                    invalid_split_deep_state, 2, "test") != tomo::kConfigParsed ||
            tomo::validate_config(invalid_split_deep) != tomo::kConfigError)
            fail("2s plus overlap 2 was not rejected after order-independent parsing");
    }
    if (rejection_text({"--overlap", "2", "--thread-mode", "2s"}, true) !=
            "--overlap 2 is only available with --thread-mode 1s; "
            "2s has no deep unified-stream schedule\n" ||
        rejection_text({"--thread-mode", "1s", "--overlap", "1",
                        "--net-io", "epoll"}, true) !=
            "--thread-mode 1s with --overlap 1 requires --net-io uring "
            "for its single submit boundary\n")
        fail("thread-study validation rejection text is not canonical");
    tomo::Config read_local;
    tomo::ConfigParseState read_local_state;
    const std::vector<const char*> read_local_args = {
        "--thread-mode", "1s", "--overlap", "0", "--read-local", "1",
    };
    if (tomo::parse_config_args(read_local_args, read_local, read_local_state, 2, "test") !=
            tomo::kConfigParsed ||
        tomo::validate_config(read_local) != tomo::kConfigParsed ||
        read_local.thread_mode != tomo::ThreadMode::Fused || read_local.read_local != 1 ||
        !rejects({"--read-local", "2"}) ||
        !rejects({"--read-local", "yes"}) ||
        !rejects({"--read-local", "-1"}) ||
        !rejects({"--read-local", ""}))
        fail("read-local boot grammar differs");
    tomo::Config read_local_default;
    if (read_local_default.read_local != 0 ||
        read_local_default.read_local_interleave != 1 ||
        read_local_default.read_local_atomic_filter != 1)
        fail("read-local defaults differ");
    auto parses_read_local_interleave = [](const char* value, uint32_t expected) {
        tomo::Config cfg;
        tomo::ConfigParseState state;
        const std::vector<const char*> args = {"--read-local-interleave", value};
        return tomo::parse_config_args(args, cfg, state, 2, "test") ==
                   tomo::kConfigParsed &&
               cfg.read_local_interleave == expected;
    };
    if (!parses_read_local_interleave("0", 0) ||
        !parses_read_local_interleave("1", 1) ||
        !rejects({"--read-local-interleave", "2"}) ||
        !rejects({"--read-local-interleave", "yes"}) ||
        !rejects({"--read-local-interleave", "-1"}) ||
        !rejects({"--read-local-interleave", ""}))
        fail("read-local-interleave boot grammar differs");
    tomo::Config prefetch_capture_off;
    tomo::ConfigParseState prefetch_capture_off_state;
    const std::vector<const char*> prefetch_capture_off_args = {
        "--read-local-prefetch-capture", "0",
    };
    if (tomo::parse_config_args(prefetch_capture_off_args, prefetch_capture_off,
                                prefetch_capture_off_state, 2, "test") !=
            tomo::kConfigParsed ||
        prefetch_capture_off.read_local_prefetch_capture != 0 ||
        !rejects({"--read-local-prefetch-capture", "2"}) ||
        !rejects({"--read-local-prefetch-capture", "yes"}) ||
        !rejects({"--read-local-prefetch-capture", "-1"}) ||
        !rejects({"--read-local-prefetch-capture", ""}))
        fail("read-local-prefetch-capture boot grammar differs");
    tomo::Config prefetch_capture_on;
    tomo::ConfigParseState prefetch_capture_on_state;
    const std::vector<const char*> prefetch_capture_on_args = {
        "--read-local-prefetch-capture", "1",
    };
    if (tomo::parse_config_args(prefetch_capture_on_args, prefetch_capture_on,
                                prefetch_capture_on_state, 2, "test") !=
            tomo::kConfigParsed ||
        tomo::validate_config(prefetch_capture_on) != tomo::kConfigParsed ||
        prefetch_capture_on.read_local != 0 ||
        prefetch_capture_on.read_local_prefetch_capture != 1)
        fail("read-local-prefetch-capture inert on setting was rejected");
    tomo::Config prefetch_capture_default;
    if (prefetch_capture_default.read_local_prefetch_capture != 1)
        fail("read-local-prefetch-capture default is not capture-at-prefetch");
    auto parses_read_local_atomic_filter = [](const char* value, uint32_t expected) {
        tomo::Config cfg;
        tomo::ConfigParseState state;
        const std::vector<const char*> args = {"--read-local-atomic-filter", value};
        return tomo::parse_config_args(args, cfg, state, 2, "test") ==
                   tomo::kConfigParsed &&
               cfg.read_local == 0 && cfg.read_local_atomic_filter == expected;
    };
    if (!parses_read_local_atomic_filter("0", 0) ||
        !parses_read_local_atomic_filter("1", 1) ||
        !rejects({"--read-local-atomic-filter", "2"}) ||
        !rejects({"--read-local-atomic-filter", "yes"}) ||
        !rejects({"--read-local-atomic-filter", "-1"}) ||
        !rejects({"--read-local-atomic-filter", ""}))
        fail("read-local-atomic-filter boot grammar differs");
    tomo::Config read_local_split;
    tomo::ConfigParseState read_local_split_state;
    const std::vector<const char*> read_local_split_args = {"--read-local", "1"};
    if (tomo::parse_config_args(read_local_split_args, read_local_split,
                                read_local_split_state, 2, "test") != tomo::kConfigParsed ||
        tomo::validate_config(read_local_split) != tomo::kConfigParsed ||
        read_local_split.thread_mode != tomo::ThreadMode::Split ||
        read_local_split.read_local != 1)
        fail("read-local split-mode lane was rejected");
    if (rejection_text({"--thread-mode", "2s", "--read-local", "1", "--overlap", "1"}, true) !=
        "--thread-mode 2s --read-local 1 requires --overlap 0\n")
        fail("split read-local silently accepted an unreachable overlap lane");
    auto parses_read_local_fallback_cell = [](const char* overlap, uint32_t expected) {
        tomo::Config cfg;
        tomo::ConfigParseState state;
        const std::vector<const char*> args = {
            "--thread-mode", "1s", "--overlap", overlap, "--read-local", "1",
        };
        return tomo::parse_config_args(args, cfg, state, 2, "test") ==
                   tomo::kConfigParsed &&
               tomo::validate_config(cfg) == tomo::kConfigParsed &&
               cfg.thread_mode == tomo::ThreadMode::Fused &&
               cfg.overlap == expected && cfg.read_local == 1;
    };
    if (!parses_read_local_fallback_cell("1", 1) ||
        !parses_read_local_fallback_cell("2", 2))
        fail("read-local overlap fallback cells were rejected");

    tomo::Config smt;
    tomo::ConfigParseState smt_state;
    const std::vector<const char*> smt_args = {"--smt-mode", "1"};
    if (tomo::parse_config_args(smt_args, smt, smt_state, 2, "test") !=
            tomo::kConfigParsed ||
        smt.smt_mode != 1 ||
        !rejects({"--smt-mode", "2"}) ||
        !rejects({"--smt-mode", "yes"}) ||
        !rejects({"--smt-mode", "-1"}))
        fail("smt-mode boot grammar differs");
    tomo::Config smt_default;
    if (smt_default.smt_mode != 0)
        fail("smt-mode default is not logical-CPU independent");

    tomo::Config ex_sched;
    tomo::ConfigParseState ex_sched_state;
    const std::vector<const char*> ex_sched_args = {"--ex-sched", "1"};
    if (tomo::parse_config_args(ex_sched_args, ex_sched, ex_sched_state, 2, "test") !=
            tomo::kConfigParsed ||
        ex_sched.ex_sched != 1 ||
        !rejects({"--ex-sched", "2"}) ||
        !rejects({"--ex-sched", "yes"}) ||
        !rejects({"--ex-sched", "-1"}))
        fail("ex-sched boot grammar differs");
    tomo::Config ex_sched_default;
    if (ex_sched_default.ex_sched != 0)
        fail("ex-sched default is not FIFO");

    tomo::Config lb;
    tomo::ConfigParseState lb_state;
    const std::vector<const char*> lb_args = {
        "--key-lb", "0", "--client-lb", "0",
        "--lb-sample-rate", "0", "--lb-age-sample-rate", "0", "--lb-tick-ms", "250",
        "--lb-imbalance-pct", "17", "--lb-move-cap", "3",
        "--lb-cooldown-ms", "9000",
    };
    if (tomo::parse_config_args(lb_args, lb, lb_state, 2, "test") != tomo::kConfigParsed ||
        lb.key_lb != 0 || lb.client_lb != 0 || lb.lb_sample_rate != 0 ||
        lb.lb_age_sample_rate != 0 ||
        lb.lb_tick_ms != 250 || lb.lb_imbalance_pct != 17 || lb.lb_move_cap != 3 ||
        lb.lb_cooldown_ms != 9000)
        fail("weighted-LB knob grammar or zero off posture differs");
    tomo::Config lb_default;
    if (lb_default.key_lb != 1 || lb_default.client_lb != 1 ||
        lb_default.lb_sample_rate != 64 || lb_default.lb_age_sample_rate != 0 ||
        lb_default.lb_tick_ms != 1000 ||
        lb_default.lb_imbalance_pct != 25 || lb_default.lb_move_cap != 1 ||
        lb_default.lb_cooldown_ms != 5000)
        fail("weighted-LB defaults differ");
    if (!rejects({"--key-lb", "2"}) ||
        !rejects({"--key-lb", "-1"}) ||
        !rejects({"--client-lb", "yes"}) ||
        !rejects({"--client-lb", "2"}) ||
        !rejects({"--lb-sample-rate", "-1"}) ||
        !rejects({"--lb-age-sample-rate", "-1"}) ||
        !rejects({"--lb-tick-ms", "later"}) ||
        !rejects({"--lb-imbalance-pct", "-1"}) ||
        !rejects({"--lb-move-cap", "-1"}) ||
        !rejects({"--lb-cooldown-ms", "-1"}))
        fail("invalid weighted-LB knob grammar was accepted");

    tomo::Config flipctl;
    tomo::ConfigParseState flipctl_state;
    const std::vector<const char*> flipctl_args = {
        "--flip-auto", "1", "--flip-auto-band", "7", "--flip-work-window", "0",
    };
    if (tomo::parse_config_args(flipctl_args, flipctl, flipctl_state, 2, "test") !=
            tomo::kConfigParsed ||
        flipctl.flip_auto != 1 || flipctl.flip_auto_band != 7 ||
        flipctl.flip_work_window != 0)
        fail("flip controller knob grammar or zero off posture differs");
    tomo::Config flipctl_default;
    if (flipctl_default.flip_auto != 0 || flipctl_default.flip_auto_band != -1 ||
        flipctl_default.flip_work_window != 100)
        fail("flip controller defaults differ");
    if (!rejects({"--flip-auto", "2"}) ||
        !rejects({"--flip-auto", "yes"}) ||
        !rejects({"--flip-auto-band", "-2"}) ||
        !rejects({"--flip-auto-band", "auto"}) ||
        !rejects({"--flip-work-window", "-1"}))
        fail("invalid flip controller knob grammar was accepted");

    tomo::Config xscript;
    tomo::ConfigParseState xscript_state;
    const std::vector<const char*> xscript_args = {
        "--script-crossshard-max-bytes", "0",
        "--script-crossshard-workbench-bytes", "1048576",
        "--script-crossshard-conflict-retries", "-1",
        "--script-crossshard-cut-slots", "7",
    };
    if (tomo::parse_config_args(xscript_args, xscript, xscript_state, 2, "test") !=
            tomo::kConfigParsed ||
        xscript.script_crossshard_max_bytes != 0 ||
        xscript.script_crossshard_workbench_bytes != 1048576 ||
        xscript.script_crossshard_conflict_retries != -1 ||
        xscript.script_crossshard_cut_slots != 7)
        fail("cross-script knob grammar or values differ");
    if (!rejects({"--script-crossshard-max-bytes", "-2"}) ||
        !rejects({"--script-crossshard-max-bytes", "-9223372036854775808"}) ||
        !rejects({"--script-crossshard-workbench-bytes", "wat"}) ||
        !rejects({"--script-crossshard-conflict-retries", "9223372036854775808"}) ||
        !rejects({"--script-crossshard-cut-slots", "-9"}))
        fail("invalid cross-script knob grammar was accepted");

    tomo::Config missing_ca;
    tomo::ConfigParseState missing_ca_state;
    const std::vector<const char*> missing_ca_args = {
        "--tls-port", "7953", "--tls-cert-file", "/cert.pem",
        "--tls-key-file", "/key.pem", "--tls-auth-clients", "yes",
    };
    {
        StderrSilencer quiet;
        if (tomo::parse_config_args(missing_ca_args, missing_ca, missing_ca_state, 2, "test") !=
                tomo::kConfigParsed ||
            tomo::validate_config(missing_ca) != tomo::kConfigError)
            fail("client-auth TLS boot without a CA was accepted");
    }

    return 0;
}

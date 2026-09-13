#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <string>
#include <unistd.h>
#include <vector>

#include "src/core/config.h"
#include "src/core/weighted_lb.h"

static_assert(tomo::cfg_default_shards(1) == 8);
static_assert(tomo::cfg_default_shards(2) == 16);
static_assert(tomo::cfg_default_shards(16) == 128);
static_assert(tomo::cfg_default_shards(32) == 256);
static_assert(tomo::cfg_default_shards(UINT32_MAX) == 256);

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
    if (rejection_text({"--shards", "16x"}) != "--shards wants -1 (auto) or 1..256\n")
        fail("shards rejection text is not canonical");
    tomo::Config shards_default;
    if (shards_default.shards != tomo::Config::kShardsAuto)
        fail("shards default is not auto");
    if (tomo::parse_config_args({"--shards", "-1"}, shards, shards_state, 2, "test") !=
            tomo::kConfigParsed || shards.shards != tomo::Config::kShardsAuto)
        fail("explicit auto shards rejected");

    // The retained network engine also determines persistence; there is no separate selector.
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
        "--thread-mode", "1s", "--x-overlap", "2",
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
    if (!parses_threads({"--thread-mode", "2s", "--x-overlap", "1"},
                        tomo::ThreadMode::Split, 1) ||
        !parses_threads({"--thread-mode", "1s", "--x-overlap", "1"},
                        tomo::ThreadMode::Fused, 1) ||
        !parses_threads({"--thread-mode", "split"}, tomo::ThreadMode::Split, 0) ||
        !parses_threads({"--thread-mode", "fused"}, tomo::ThreadMode::Fused, 0))
        fail("thread-mode compatibility aliases differ");
    if (!rejects({"--thread-mode", "two-stage"}) ||
        !rejects({"--x-overlap", "3"}) ||
        !rejects({"--x-overlap", "-1"}) ||
        !rejects({"--genthread-schedule", "streams0"}))
        fail("invalid thread study grammar was accepted");
    if (rejection_text({"--x-overlap", "3"}) != "--x-overlap wants 0, 1 or 2\n")
        fail("thread-study parser rejection text is not canonical");
    tomo::Config invalid_split_deep;
    tomo::ConfigParseState invalid_split_deep_state;
    const std::vector<const char*> invalid_split_deep_args = {
        "--x-overlap", "2", "--thread-mode", "2s",
    };
    {
        StderrSilencer quiet;
        if (tomo::parse_config_args(invalid_split_deep_args, invalid_split_deep,
                                    invalid_split_deep_state, 2, "test") != tomo::kConfigParsed ||
            tomo::validate_config(invalid_split_deep) != tomo::kConfigError)
            fail("2s plus overlap 2 was not rejected after order-independent parsing");
    }
    if (rejection_text({"--x-overlap", "2", "--thread-mode", "2s"}, true) !=
            "--x-overlap 2 is only available with --thread-mode 1s; "
            "2s has no deep unified-stream schedule\n" ||
        rejection_text({"--thread-mode", "1s", "--x-overlap", "1",
                        "--net-io", "epoll"}, true) !=
            "--thread-mode 1s with --x-overlap 1 requires --net-io uring "
            "for its single submit boundary\n")
        fail("thread-study validation rejection text is not canonical");
    tomo::Config read_local;
    tomo::ConfigParseState read_local_state;
    const std::vector<const char*> read_local_args = {
        "--thread-mode", "1s", "--x-overlap", "0", "--read-local", "1",
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
    if (read_local_default.read_local != 0) fail("read-local default differs");
    tomo::Config read_local_split;
    tomo::ConfigParseState read_local_split_state;
    const std::vector<const char*> read_local_split_args = {"--read-local", "1"};
    if (tomo::parse_config_args(read_local_split_args, read_local_split,
                                read_local_split_state, 2, "test") != tomo::kConfigParsed ||
        tomo::validate_config(read_local_split) != tomo::kConfigParsed ||
        read_local_split.thread_mode != tomo::ThreadMode::Split ||
        read_local_split.read_local != 1)
        fail("read-local split-mode inert setting was rejected");
    auto parses_read_local_fallback_cell = [](const char* overlap, uint32_t expected) {
        tomo::Config cfg;
        tomo::ConfigParseState state;
        const std::vector<const char*> args = {
            "--thread-mode", "1s", "--x-overlap", overlap, "--read-local", "1",
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

    tomo::Config ex_sched;
    tomo::ConfigParseState ex_sched_state;
    const std::vector<const char*> ex_sched_args = {"--x-ex-sched", "1"};
    if (tomo::parse_config_args(ex_sched_args, ex_sched, ex_sched_state, 2, "test") !=
            tomo::kConfigParsed ||
        ex_sched.ex_sched != 1 ||
        !rejects({"--x-ex-sched", "2"}) ||
        !rejects({"--x-ex-sched", "yes"}) ||
        !rejects({"--x-ex-sched", "-1"}))
        fail("ex-sched boot grammar differs");
    tomo::Config ex_sched_default;
    if (ex_sched_default.ex_sched != 0)
        fail("ex-sched default is not FIFO");

    // A longer observation interval with proportionally more traffic must preserve the
    // samples-per-decision target; transfer pacing must respond to measured cost.
    tomo::LbAutotune sampled_lb;
    sampled_lb.last_fold_ns = 1;
    sampled_lb.observe_visits(4096, 1000000001);
    const uint32_t sampled_rate = sampled_lb.sample_rate.load();
    sampled_lb.observe_visits(8192, 3000000001);
    if (sampled_rate != 3 || sampled_lb.sample_rate.load() != sampled_rate)
        fail("LB samples per decision depend on observation interval");
    tomo::LbAutotune slow_lb, fast_lb;
    if (slow_lb.move_cap(16) != 1 || slow_lb.cooldown_ms() == 0)
        fail("LB bootstrap cannot move or has no observation cooldown");
    slow_lb.note_transfer(600000000, 1);
    fast_lb.note_transfer(1000000, 1);
    if (slow_lb.move_cap(16) >= fast_lb.move_cap(16) ||
        slow_lb.cooldown_ms() <= fast_lb.cooldown_ms())
        fail("LB pacing does not track completed transfer cost");
    tomo::LbAutotune::QuietJitter noise;
    for (double sample : {10.0, 11.0, 10.0, 11.0}) noise.observe(sample);
    if (noise.band() != 2.0) fail("LB band is not twice measured quiet jitter");
    noise.observe(40.0);
    if (noise.band() != 2.0) fail("an excursion widened its own LB band");

    tomo::Config lb;
    tomo::ConfigParseState lb_state;
    if (lb.lb != 1) fail("LB default is not enabled");
    for (const char* value : {"0", "1"}) {
        if (tomo::parse_config_args({"--lb", value}, lb, lb_state, 2, "test") !=
                tomo::kConfigParsed || lb.lb != static_cast<uint32_t>(*value - '0'))
            fail("LB boot grammar differs");
    }
    if (!rejects({"--lb", "2"}) || !rejects({"--lb", "-1"}) ||
        !rejects({"--lb", "yes"}) || !rejects({"--lb", ""}))
        fail("invalid LB grammar accepted");

    // Each retired spelling must fail even with its formerly valid default. The same parser
    // consumes conf-file tokens, so this also prevents CONFIG REWRITE from reviving old knobs.
    const std::pair<const char*, const char*> retired[] = {
        {"--read-local-prefetch-capture", "1"}, {"--read-local-atomic-filter", "1"},
        {"--read-local-interleave", "1"}, {"--flip-auto-band", "-1"},
        {"--shard-home", "0:1"}, {"--l3-domains", "0-7"}, {"--smt-mode", "0"},
        {"--genthread-schedule", "coarse"}, {"--atomic-window", "-1"},
        {"--persist-io", "uring"}, {"--lru-clock-shift", "8"},
        {"--script-crossshard-max-bytes", "-1"},
        {"--script-crossshard-workbench-bytes", "-1"},
        {"--script-crossshard-conflict-retries", "-1"},
        {"--script-crossshard-cut-slots", "-1"}, {"--tls-ktls", "yes"},
        {"--key-lb", "1"}, {"--client-lb", "1"}, {"--lb-sample-rate", "64"},
        {"--lb-age-sample-rate", "0"}, {"--lb-tick-ms", "1000"},
        {"--lb-imbalance-pct", "25"}, {"--lb-move-cap", "1"},
        {"--lb-cooldown-ms", "5000"}, {"--ex-sched", "0"},
        {"--overlap", "0"}, {"--thread-pipeline", "0"},
    };
    for (const auto& [flag, value] : retired) {
        if (!rejects({flag, value})) fail("retired knob was accepted");
    }
    if (tomo::persistence_engine(network_default) != tomo::PersistIoEngine::Uring ||
        tomo::persistence_engine(network) != tomo::PersistIoEngine::Normal)
        fail("persistence does not follow the network engine");

    tomo::Config flipctl;
    tomo::ConfigParseState flipctl_state;
    const std::vector<const char*> flipctl_args = {
        "--flip-auto", "1", "--flip-work-window", "0",
    };
    if (tomo::parse_config_args(flipctl_args, flipctl, flipctl_state, 2, "test") !=
            tomo::kConfigParsed ||
        flipctl.flip_auto != 1 ||
        flipctl.flip_work_window != 0)
        fail("flip controller knob grammar or zero off posture differs");
    tomo::Config flipctl_default;
    if (flipctl_default.flip_auto != 0 ||
        flipctl_default.flip_work_window != 100)
        fail("flip controller defaults differ");
    if (!rejects({"--flip-auto", "2"}) ||
        !rejects({"--flip-auto", "yes"}) ||
        !rejects({"--flip-work-window", "-1"}))
        fail("invalid flip controller knob grammar was accepted");

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

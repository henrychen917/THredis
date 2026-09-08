#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <string>
#include <unistd.h>
#include <vector>

#include "src/core/config.h"
#include "src/core/placement.h"
#include "src/core/weighted_lb.h"

static_assert(tomo::cfg_default_shards(1) == 8);
static_assert(tomo::cfg_default_shards(2) == 16);
static_assert(tomo::cfg_default_shards(16) == 128);
static_assert(tomo::cfg_default_shards(32) == 256);
static_assert(tomo::cfg_default_shards(UINT32_MAX) == 256);
static_assert(tomo::flip_fingerprint_window(false) == 0);
static_assert(tomo::flip_fingerprint_window(true) == 100);

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
    std::fputs("reorder 1\n", file);
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
        "--reorder",
        "1",
    };
    if (tokens != expected) fail("mid-value '#' or quoted token did not survive exactly");
    tomo::Config reorder_file;
    tomo::ConfigParseState reorder_file_state;
    if (tomo::parse_config_args({tokens[tokens.size() - 2].c_str(), tokens.back().c_str()},
                               reorder_file, reorder_file_state, 1, "conf") !=
            tomo::kConfigParsed || reorder_file.reorder != 1 ||
        tomo::parse_config_args({"--reorder", "0"}, reorder_file, reorder_file_state,
                                2, "cli") != tomo::kConfigParsed || reorder_file.reorder != 0)
        fail("reorder conf-file grammar or CLI override differs");

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

    // Restored reference controls: default translation, every alias, signed list modes,
    // full reference ranges, and the surprising INTEGER vs MEMORY distinction for set values.
    tomo::Config encodings;
    tomo::ConfigParseState encoding_state;
    const tomo::TypeLimits fixed;
    const auto defaults = encodings.encodings.type_limits();
    if (std::memcmp(&fixed, &defaults, sizeof(fixed)))
        fail("restored encoding defaults changed the fixed behavior");
    if (tomo::parse_config_args({"--hash-max-ziplist-entries", "4",
                                "--hash-max-ziplist-value", "1kb",
                                "--list-max-ziplist-size", "-3",
                                "--set-max-listpack-entries", "6",
                                "--set-max-listpack-value", "20",
                                "--zset-max-ziplist-entries", "7",
                                "--zset-max-ziplist-value", "2k"},
                               encodings, encoding_state, 1, "conf") != tomo::kConfigParsed)
        fail("reference encoding aliases or memory grammar rejected");
    auto limits = encodings.encodings.type_limits();
    if (limits.hash.max_entries != 4 || limits.hash.max_value != 1024 ||
        limits.list.max_entries != UINT32_MAX || limits.list.max_value != 16384 ||
        limits.set.max_entries != 6 || limits.set.max_value != 20 ||
        limits.zset.max_entries != 7 || limits.zset.max_value != 2000)
        fail("reference encoding limits were not applied");
    if (tomo::parse_config_args({"--hash-max-listpack-entries", "512",
                                "--list-max-listpack-size", "4"},
                               encodings, encoding_state, 2, "cli") != tomo::kConfigParsed ||
        encodings.encodings.values[tomo::EncodingConfig::HashEntries] != 512 ||
        encodings.encodings.type_limits().list.max_entries != 4)
        fail("canonical CLI encoding settings did not override file aliases");
    for (const auto& setting : tomo::EncodingConfig::settings) {
        const std::string flag = std::string("--") + setting.name;
        tomo::Config cfg;
        tomo::ConfigParseState state;
        if (tomo::parse_config_args({flag.c_str(), "0"}, cfg, state, 2, "test") !=
                tomo::kConfigParsed || !rejects({flag.c_str(), "1x"}) ||
            !rejects({flag.c_str(), "+1"}) || !rejects({flag.c_str(), "-0"}) ||
            !rejects({flag.c_str(), "9223372036854775808"}))
            fail("encoding zero/range/decimal grammar differs");
    }
    for (const auto& probe : {std::pair{"-1", 4096u}, {"-2", 8192u},
                              {"-3", 16384u}, {"-4", 32768u}, {"-5", 65536u},
                              {"-6", 65536u}, {"-2147483648", 65536u}}) {
        if (tomo::parse_config_args({"--list-max-listpack-size", probe.first},
                                   encodings, encoding_state, 2, "test") != tomo::kConfigParsed ||
            encodings.encodings.type_limits().list.max_value != probe.second)
            fail("negative list mode did not select/clamp its byte budget");
    }
    if (tomo::parse_config_args({"--list-max-listpack-size", "0",
                                "--hash-max-listpack-entries", "9223372036854775807",
                                "--hash-max-listpack-value", "4294967296"},
                               encodings, encoding_state, 2, "test") != tomo::kConfigParsed ||
        encodings.encodings.type_limits().list.max_entries != 1 ||
        encodings.encodings.type_limits().hash.max_entries != UINT32_MAX ||
        encodings.encodings.type_limits().hash.max_value != UINT32_MAX ||
        encodings.encodings.values[tomo::EncodingConfig::HashEntries] != INT64_MAX ||
        !rejects({"--list-max-listpack-size", "-2147483649"}) ||
        !rejects({"--list-max-listpack-size", "2147483648"}) ||
        !rejects({"--list-max-listpack-size", "1kb"}) ||
        !rejects({"--set-max-listpack-value", "1kb"}) ||
        !rejects({"--set-max-listpack-value", "01"}))
        fail("reference range, integer grammar or list zero semantics differ");

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
        "--thread-mode", "1s", "--overlap", "1",
    };
    if (tomo::parse_config_args(threads_args, threads, threads_state, 2, "test") !=
            tomo::kConfigParsed ||
        tomo::validate_config(threads) != tomo::kConfigParsed ||
        threads.thread_mode != tomo::ThreadMode::Fused || threads.overlap != 1)
        fail("primary thread-mode/overlap grammar differs");
    tomo::Config thread_default;
    if (thread_default.thread_mode != tomo::ThreadMode::Split ||
        thread_default.overlap != 0)
        fail("thread defaults are not 2s overlap 0");

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
        !parses_threads({"--thread-mode", "1s", "--overlap", "1"},
                        tomo::ThreadMode::Fused, 1) ||
        !parses_threads({"--thread-mode", "split"}, tomo::ThreadMode::Split, 0) ||
        !parses_threads({"--thread-mode", "fused"}, tomo::ThreadMode::Fused, 0))
        fail("thread-mode compatibility aliases differ");
    if (!rejects({"--thread-mode", "two-stage"}) ||
        !rejects({"--overlap", "2"}) ||
        !rejects({"--overlap", "3"}) ||
        !rejects({"--overlap", "-1"}) ||
        !rejects({"--overlap", "yes"}) ||
        !rejects({"--overlap", ""}) ||
        !rejects({"--overlap"}) ||
        !rejects({"--genthread-schedule", "streams0"}))
        fail("invalid thread grammar was accepted");
    if (rejection_text({"--overlap", "3"}) != "--overlap wants 0 or 1\n")
        fail("overlap parser rejection text is not canonical");
    for (const char* mode : {"1s", "2s"}) {
        if (!rejects({"--thread-mode", mode, "--overlap", "2"}) ||
            !rejects({"--overlap", "2", "--thread-mode", mode}))
            fail("overlap 2 was accepted in a mode or argument order");
    }
    tomo::Config invalid_overlap;
    invalid_overlap.overlap = 2;
    {
        StderrSilencer quiet;
        if (tomo::validate_config(invalid_overlap) != tomo::kConfigError)
            fail("programmatic overlap 2 was accepted");
    }
    if (rejection_text({"--thread-mode", "1s", "--overlap", "1",
                        "--net-io", "epoll"}, true) !=
            "--thread-mode 1s with --overlap 1 requires --net-io uring "
            "for its single submit boundary\n")
        fail("overlap engine validation rejection text is not canonical");
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
    auto parses_read_local_cell = [](const char* mode, const char* overlap,
                                     const char* lane, const char* reorder, const char* engine) {
        tomo::Config cfg;
        tomo::ConfigParseState state;
        const std::vector<const char*> args = {
            "--thread-mode", mode, "--overlap", overlap, "--read-local", lane,
            "--reorder", reorder, "--net-io", engine,
        };
        return tomo::parse_config_args(args, cfg, state, 2, "test") ==
                   tomo::kConfigParsed &&
               tomo::validate_config(cfg) == tomo::kConfigParsed &&
               cfg.thread_mode == ((!std::strcmp(mode, "1s") || !std::strcmp(mode, "fused"))
                   ? tomo::ThreadMode::Fused : tomo::ThreadMode::Split) &&
               cfg.overlap == static_cast<uint32_t>(*overlap - '0') &&
               cfg.reorder == static_cast<uint32_t>(*reorder - '0') &&
               cfg.read_local == static_cast<uint32_t>(*lane - '0');
    };
    for (const char* mode : {"1s", "2s", "fused", "split"})
        for (const char* overlap : {"0", "1"})
            for (const char* lane : {"0", "1"})
                for (const char* reorder : {"0", "1"}) {
                    if (!parses_read_local_cell(mode, overlap, lane, reorder, "uring"))
                        fail("overlap/read-local/reorder boot cell was rejected");
                    const bool fused = !std::strcmp(mode, "1s") || !std::strcmp(mode, "fused");
                    const bool epoll_supported = !fused || *overlap == '0';
                    StderrSilencer quiet;
                    if (parses_read_local_cell(mode, overlap, lane, reorder, "epoll") !=
                        epoll_supported)
                        fail("epoll overlap/read-local/reorder validation differs");
                }

    tomo::Config reorder;
    tomo::ConfigParseState reorder_state;
    const std::vector<const char*> reorder_args = {"--reorder", "1"};
    if (tomo::parse_config_args(reorder_args, reorder, reorder_state, 2, "test") !=
            tomo::kConfigParsed ||
        reorder.reorder != 1 ||
        !rejects({"--reorder", "2"}) || !rejects({"--reorder", "yes"}) ||
        !rejects({"--reorder", "-1"}) || !rejects({"--reorder", "1x"}) ||
        !rejects({"--reorder", ""}) || !rejects({"--reorder"}))
        fail("reorder boot grammar differs");
    if (rejection_text({"--reorder", "3"}) != "--reorder wants 0 or 1\n")
        fail("reorder parser rejection text is not canonical");
    tomo::Config reorder_default;
    if (reorder_default.reorder != 0) fail("reorder default is not FIFO");
    reorder.reorder = 2;
    {
        StderrSilencer quiet;
        if (tomo::validate_config(reorder) != tomo::kConfigError)
            fail("programmatic reorder 2 was accepted");
    }

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

    tomo::Config lb_defaults;
    if (lb_defaults.key_lb != 1 || lb_defaults.client_lb != 1)
        fail("independent LB defaults are not both enabled");
    for (const char* mode : {"1s", "2s"})
        for (const char* key : {"0", "1"})
            for (const char* client : {"0", "1"}) {
                tomo::Config lb;
                tomo::ConfigParseState state;
                if (tomo::parse_config_args({"--thread-mode", mode, "--key-lb", key,
                                             "--client-lb", client}, lb, state, 1, "conf") !=
                        tomo::kConfigParsed || tomo::validate_config(lb) != tomo::kConfigParsed ||
                    lb.key_lb != static_cast<uint32_t>(*key - '0') ||
                    lb.client_lb != static_cast<uint32_t>(*client - '0'))
                    fail("independent LB configuration cell differs");
                // CLI overrides one feature without changing the other feature from the file.
                if (tomo::parse_config_args({"--key-lb", "1"}, lb, state, 2, "cli") !=
                        tomo::kConfigParsed || lb.key_lb != 1 ||
                    lb.client_lb != static_cast<uint32_t>(*client - '0'))
                    fail("key-lb override changed client-lb");
                if (tomo::parse_config_args({"--client-lb", "0"}, lb, state, 2, "cli") !=
                        tomo::kConfigParsed || lb.key_lb != 1 || lb.client_lb != 0)
                    fail("client-lb override changed key-lb");
            }
    for (const char* flag : {"--key-lb", "--client-lb"}) {
        for (const char* value : {"2", "-1", "yes", "1x", "", "4294967296"})
            if (!rejects({flag, value})) fail("invalid independent LB grammar accepted");
        if (!rejects({flag})) fail("missing independent LB argument accepted");
    }

    // No server or worker threads: exercise the real topology mapper against four allowed CPUs.
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (::sched_getaffinity(0, sizeof(allowed), &allowed) != 0)
        fail("could not read allowed CPUs for placement fixture");
    std::vector<int> cpus;
    for (int cpu = 0; cpu < CPU_SETSIZE && cpus.size() < 4; cpu++)
        if (CPU_ISSET(cpu, &allowed)) cpus.push_back(cpu);
    if (cpus.size() != 4) fail("placement fixture needs four allowed CPUs");
    std::string domain;
    for (int cpu : cpus) domain += (domain.empty() ? "" : "+") + std::to_string(cpu);
    tomo::Topology topology;
    if (!topology.declare(domain.c_str())) fail("could not declare placement fixture");
    tomo::Placement placement;
    if (!placement.build_even(topology, 1, 3) || !placement.assign_shard_homes(4))
        fail("default shard homes rejected");
    for (uint32_t sid = 0; sid < 4; sid++)
        if (placement.shard_home(sid) != 1 + sid % 3)
            fail("default shard homes are not round-robin over executors");
    if (!placement.assign_shard_homes(4, "0:1,1:1,2:2,3:2") ||
        placement.ex_threads().size() != 3)
        fail("manual shard homes rejected an empty filler executor");
    for (uint32_t sid = 0; sid < 4; sid++)
        if (placement.shard_home(sid) != 1 + sid / 2)
            fail("manual shard homes were silently replaced by default ownership");
    {
        StderrSilencer quiet;
        for (const char* map : {"", "0:1", "0:1,1:1,2:2,2:2", "0:0,1:1,2:2,3:2",
                                "0:4,1:1,2:2,3:2", "0:1,1:1,2:2,4:2",
                                "0:1,1:1,2:2,3:2,", "0:1,1:1,2:2,3:2x",
                                "0:1,1:1,2:2,3:4294967296"})
            if (placement.assign_shard_homes(4, map)) fail("invalid shard-home map accepted");
    }
    if (!placement.build_fused(topology, nullptr) ||
        !placement.assign_shard_homes(4, "0:0,1:0,2:1,3:1"))
        fail("fused shard homes did not accept fused executor IDs");
    tomo::Config homes;
    tomo::ConfigParseState homes_state;
    if (homes.shard_home ||
        tomo::parse_config_args({"--shard-home", "0:1,1:1"}, homes, homes_state, 1, "conf") !=
            tomo::kConfigParsed || std::strcmp(homes.shard_home, "0:1,1:1") ||
        tomo::parse_config_args({"--shard-home", "0:2,1:2"}, homes, homes_state, 2, "cli") !=
            tomo::kConfigParsed || std::strcmp(homes.shard_home, "0:2,1:2"))
        fail("shard-home conf grammar or CLI precedence differs");
    if (rejection_text({"--shard-home", ""}, true) !=
            "--shard-home must contain shard:thread pairs\n")
        fail("empty shard-home accepted at validation");

    // Each retired spelling must fail even with its formerly valid default. The same parser
    // consumes conf-file tokens, so this also prevents CONFIG REWRITE from reviving old knobs.
    const std::pair<const char*, const char*> retired[] = {
        {"--read-local-prefetch-capture", "1"}, {"--read-local-atomic-filter", "1"},
        {"--read-local-interleave", "1"}, {"--flip-auto-band", "-1"},
        {"--l3-domains", "0-7"}, {"--smt-mode", "0"},
        {"--genthread-schedule", "coarse"}, {"--atomic-window", "-1"},
        {"--persist-io", "uring"}, {"--lru-clock-shift", "8"},
        {"--script-crossshard-max-bytes", "-1"},
        {"--script-crossshard-workbench-bytes", "-1"},
        {"--script-crossshard-conflict-retries", "-1"},
        {"--script-crossshard-cut-slots", "-1"}, {"--tls-ktls", "yes"},
        {"--lb", "1"}, {"--flip-work-window", "100"},
        {"--script-instruction-limit", "100000"}, {"--lb-sample-rate", "64"},
        {"--load", "dump.tomo"}, {"--conf", "tomokv.conf"},
        {"--hash-max-compact-entries", "512"}, {"--hash-max-compact-value", "64"},
        {"--list-max-compact-entries", "4294967295"}, {"--list-max-compact-value", "8192"},
        {"--set-max-compact-entries", "128"}, {"--set-max-compact-value", "64"},
        {"--zset-max-compact-entries", "128"}, {"--zset-max-compact-value", "64"},
        {"--lb-age-sample-rate", "0"}, {"--lb-tick-ms", "1000"},
        {"--lb-imbalance-pct", "25"}, {"--lb-move-cap", "1"},
        {"--lb-cooldown-ms", "5000"}, {"--ex-sched", "0"}, {"--x-ex-sched", "0"},
        {"--x-overlap", "0"}, {"--thread-pipeline", "0"},
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
        "--flip-auto", "1",
    };
    if (tomo::parse_config_args(flipctl_args, flipctl, flipctl_state, 2, "test") !=
            tomo::kConfigParsed ||
        flipctl.flip_auto != 1)
        fail("flip controller knob grammar or zero off posture differs");
    tomo::Config flipctl_default;
    if (flipctl_default.flip_auto != 0)
        fail("flip controller defaults differ");
    if (!rejects({"--flip-auto", "2"}) ||
        !rejects({"--flip-auto", "yes"}))
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

// config.h — every runtime knob in one place: the Config struct, the CLI grammar that fills it,
// and the conf-file loader. One parser serves both the command line and tomokv.conf (a conf line
// is translated to the equivalent --flag tokens), so a knob's spelling, validation, and default
// exist exactly once. t_server.cc's CONFIG SET/GET table (init_config) is built FROM this struct,
// which makes it the third consumer of the same source of truth.
//
// House rules for knobs (owner): numeric where possible; 0 = off and off allocates nothing;
// -1 = auto; thresholds self-derive. A field in Config that nothing reads is a lie — delete it.
//
// COMPILE-TIME tunables live elsewhere on purpose (changing them is a rebuild + re-validation,
// not an operational act). The complete list, so nothing hides:
//   kRobWindow        64      net/rob.h       max in-flight ops per connection (ROB size);
//                                             the read-local write ring is sized FROM it
//   kEmbedThreshold   192     store/kvobj.h   value bytes embedded in the key's block
//   ValueSlot::kInline 1024   cmd/scatter_engine.inc  gather slot capacity; pairs with zc-min as
//                                             the unified copy-vs-borrow cutover (min of the two)
//   kCommonBytes      16KiB   cmd/xshard.h    pooled scatter arena block size
//   sizeof(Op)==336, sizeof(Client)==1984     footprint locks (static_assert, do not move)

#pragma once

#include <cstdint>
#include <climits>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "../base/slice.h"       // Slice (notify flag parsing)
#include "../cmd/notify.h"       // parse_notify_flags
#include "../store/eviction.h"   // MaxmemoryPolicy + parse_maxmemory_policy
#include "../store/typeval.h"    // TypeLimits (compact-encoding limits)
#include "../store/flatstore.h"  // HashKind + g_hash_kind (the only symbols needed from it)

namespace tomo {

inline bool cfg_parse_u32(const char* s, uint32_t& out);
inline bool cfg_parse_u64(const char* s, uint64_t& out);

struct ClientBufferLimit {
    uint64_t hard_bytes = 0;
    uint64_t soft_bytes = 0;
    uint32_t soft_seconds = 0;
};

struct ClientOutputBufferLimits {
    ClientBufferLimit normal{};
    ClientBufferLimit replica{256ull * 1024 * 1024, 64ull * 1024 * 1024, 60};
    ClientBufferLimit pubsub{32ull * 1024 * 1024, 8ull * 1024 * 1024, 60};
};

inline bool cfg_eq_icase(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        unsigned char ac = static_cast<unsigned char>(*a++);
        unsigned char bc = static_cast<unsigned char>(*b++);
        if (ac >= 'A' && ac <= 'Z') ac = static_cast<unsigned char>(ac + ('a' - 'A'));
        if (bc >= 'A' && bc <= 'Z') bc = static_cast<unsigned char>(bc + ('a' - 'A'));
        if (ac != bc) return false;
    }
    return *a == *b;
}

// Redis memtoull grammar: bare bytes (or B), decimal K/M/G, binary KB/MB/GB.  Both boot
// parsing and CONFIG SET call this length-aware implementation, so a RESP bulk does not need a
// temporary NUL-terminated copy and the two surfaces cannot drift again.
inline bool cfg_memory_suffix(const char* input, size_t length, const char* expected) {
    const size_t expected_length = std::strlen(expected);
    if (length != expected_length) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char actual = static_cast<unsigned char>(input[i]);
        unsigned char wanted = static_cast<unsigned char>(expected[i]);
        if (actual >= 'A' && actual <= 'Z') actual += static_cast<unsigned char>('a' - 'A');
        if (wanted >= 'A' && wanted <= 'Z') wanted += static_cast<unsigned char>('a' - 'A');
        if (actual != wanted) return false;
    }
    return true;
}

inline bool cfg_parse_memory(const char* input, size_t length, uint64_t& out) {
    if (!input || !length || *input == '-') return false;
    size_t digits = 0;
    while (digits < length && input[digits] >= '0' && input[digits] <= '9') digits++;
    if (!digits || digits >= 128) return false; // Redis memtoull numeric buffer bound
    uint64_t value = 0;
    bool saturated = false;
    for (size_t i = 0; i < digits; i++) {
        const uint32_t digit = static_cast<uint32_t>(input[i] - '0');
        if (!saturated) {
            if (value > (UINT64_MAX - digit) / 10) {
                value = UINT64_MAX;
                saturated = true;
            } else {
                value = value * 10 + digit;
            }
        }
    }
    const char* suffix = input + digits;
    const size_t suffix_length = length - digits;
    uint64_t mul = 1;
    if (!suffix_length || cfg_memory_suffix(suffix, suffix_length, "b")) mul = 1;
    else if (cfg_memory_suffix(suffix, suffix_length, "k"))  mul = 1000;
    else if (cfg_memory_suffix(suffix, suffix_length, "kb")) mul = 1024;
    else if (cfg_memory_suffix(suffix, suffix_length, "m"))  mul = 1000ull * 1000;
    else if (cfg_memory_suffix(suffix, suffix_length, "mb")) mul = 1024ull * 1024;
    else if (cfg_memory_suffix(suffix, suffix_length, "g"))  mul = 1000ull * 1000 * 1000;
    else if (cfg_memory_suffix(suffix, suffix_length, "gb")) mul = 1024ull * 1024 * 1024;
    else return false;
    // Redis preserves strtoull's ULLONG_MAX saturation and then performs the unsigned multiply.
    // The wrap for an overflowing suffixed value is odd but observable CONFIG behavior.
    out = value * mul;
    return true;
}

inline bool cfg_parse_memory(const char* input, uint64_t& out) {
    return input && cfg_parse_memory(input, std::strlen(input), out);
}

struct SaveClause {
    uint64_t seconds = 0;
    uint64_t changes = 0;
};

inline constexpr uint64_t kProtoMinBulkLen = 1024ull * 1024;
// Op argv Slices and the connection receive cursor are uint32_t. Reserve framing slack so a
// maximum-size bulk plus its RESP envelope remains representable without growing either hot type.
inline constexpr uint64_t kProtoMaxBulkLenSupported = UINT32_MAX - 64ull * 1024;

inline bool cfg_parse_save_schedule(const char* input, size_t length,
                                    std::vector<SaveClause>& out) {
    out.clear();
    if (!length) return true;
    if (!input || std::isspace(static_cast<unsigned char>(input[0])) ||
        std::isspace(static_cast<unsigned char>(input[length - 1]))) return false;
    std::vector<uint64_t> values;
    size_t pos = 0;
    while (pos < length) {
        const size_t begin = pos;
        while (pos < length && !std::isspace(static_cast<unsigned char>(input[pos]))) pos++;
        uint64_t value = 0;
        if (begin == pos || !cfg_parse_u64(std::string(input + begin, pos - begin).c_str(), value))
            return false;
        values.push_back(value);
        while (pos < length && std::isspace(static_cast<unsigned char>(input[pos]))) pos++;
    }
    if (values.empty() || (values.size() & 1u)) return false;
    out.reserve(values.size() / 2);
    for (size_t i = 0; i < values.size(); i += 2) {
        if (values[i] == 0) return false;
        out.push_back(SaveClause{values[i], values[i + 1]});
    }
    return true;
}

inline std::string cfg_save_schedule_string(const std::vector<SaveClause>& clauses) {
    std::string out;
    for (const SaveClause& clause : clauses) {
        if (!out.empty()) out.push_back(' ');
        out += std::to_string(clause.seconds);
        out.push_back(' ');
        out += std::to_string(clause.changes);
    }
    return out;
}

inline bool cfg_parse_client_output_buffer_limit(const char* const* args, size_t count,
                                                  ClientOutputBufferLimits& limits,
                                                  const char*& error) {
    if (!count || count % 4) {
        error = "Wrong number of arguments in buffer limit configuration.";
        return false;
    }
    ClientOutputBufferLimits scratch = limits;
    for (size_t i = 0; i < count; i += 4) {
        ClientBufferLimit* target = nullptr;
        if (cfg_eq_icase(args[i], "normal")) target = &scratch.normal;
        else if (cfg_eq_icase(args[i], "slave") || cfg_eq_icase(args[i], "replica"))
            target = &scratch.replica;
        else if (cfg_eq_icase(args[i], "pubsub")) target = &scratch.pubsub;
        if (!target || cfg_eq_icase(args[i], "master")) {
            error = "Invalid client class specified in buffer limit configuration.";
            return false;
        }
        uint64_t hard = 0, soft = 0;
        uint32_t seconds = 0;
        if (!cfg_parse_memory(args[i + 1], hard) || !cfg_parse_memory(args[i + 2], soft) ||
            !cfg_parse_u32(args[i + 3], seconds) || seconds > INT_MAX) {
            error = "Error in hard, soft or soft_seconds setting in buffer limit configuration.";
            return false;
        }
        *target = ClientBufferLimit{hard, soft, seconds};
    }
    limits = scratch;
    error = nullptr;
    return true;
}

inline std::string cfg_client_output_buffer_limit_string(const ClientOutputBufferLimits& limits) {
    return "normal " + std::to_string(limits.normal.hard_bytes) + " " +
           std::to_string(limits.normal.soft_bytes) + " " +
           std::to_string(limits.normal.soft_seconds) + " slave " +
           std::to_string(limits.replica.hard_bytes) + " " +
           std::to_string(limits.replica.soft_bytes) + " " +
           std::to_string(limits.replica.soft_seconds) + " pubsub " +
           std::to_string(limits.pubsub.hard_bytes) + " " +
           std::to_string(limits.pubsub.soft_bytes) + " " +
           std::to_string(limits.pubsub.soft_seconds);
}

enum class DebugCommandMode : uint8_t { No = 0, Yes = 1, Local = 2 };
enum class AppendFsyncPolicy : uint8_t { Always = 0, Everysec = 1, No = 2 };
// One boot-latched persistence engine governs both AOF and snapshot file data/sync operations.
// Uring is native; epoll networking derives the syscall persistence engine (there is no ring).
enum class PersistIoEngine : uint8_t { Normal = 0, Uring = 1 };
// One boot-latched NETWORK event engine for every io thread. Uring is the native default and the
// only path with measured numbers behind it; epoll exists so the same binary runs where io_uring is
// unavailable or unwanted. This boot-only choice also selects the persistence engine.
enum class NetIoEngine : uint8_t { Uring = 0, Epoll = 1 };
// Boot-latched loop architecture. 2s retains dedicated network and executor threads; 1s gives
// every selected physical thread both loop objects. Split/fused remain compatibility spellings.
enum class ThreadMode : uint8_t { Split = 0, Fused = 1 };
enum class TlsAuthClients : uint8_t { Yes = 0, No = 1, Optional = 2 };

struct Config {
    // ---- placement (boot-only) -------------------------------------------------------------
    const char* place       = nullptr;   // complete role@cpu list; null = --ratio / default
    // Whole-server role counts for even placement (--ratio); zero = unset, split mode only.
    uint32_t even_ifid      = 0;
    uint32_t even_ex        = 0;
    // Resolve before persistence recovery and shard allocation: eight migration units per EX.
    static constexpr uint32_t kShardsAuto = UINT32_MAX;
    uint32_t shards         = kShardsAuto;
    bool     pin_threads    = true;     // relative to the process's allowed CPU set

    // One boot latch owns all key/client signals, census, windows and autonomous movement.
    // Off allocates none of that state; controller policy is derived in weighted_lb.h.
    uint32_t lb = 1;

    // ---- automatic role split (boot-latched) -----------------------------------------------
    // Ships dark. The workload fingerprint the controller reads is owner-local and SAMPLED by
    // parse pass (DESIGN-flipfp.md): flip_work_window is commands per fingerprinted command, one
    // pass in W is fingerprinted whole, and the writer is armed only while the controller -- its
    // one reader -- is enabled, so with flip_auto 0 (and in 1s mode) dispatch pays one predicted
    // branch per op and no store. 1 = every pass (exhaustive), 0 = off. The trigger band
    // always learns twice the anchor's own quiet jitter.
    uint32_t flip_auto = 0;
    uint32_t flip_work_window = 100;

    // ---- network (boot-only) ---------------------------------------------------------------
    uint16_t port           = 6379;
    const char* bind_addr   = "127.0.0.1";
    const char* unixsocket  = nullptr;
    uint32_t maxclients     = 10000;     // live; accept-path pre-count safety valve
    uint32_t timeout        = 0;         // live; idle seconds, 0 = disabled
    uint32_t tcp_keepalive  = 300;       // live for newly accepted TCP clients, 0 = off
    uint32_t tcp_backlog    = 511;       // boot-only, passed directly to listen(2)
    NetIoEngine net_io      = NetIoEngine::Uring;  // boot-only: which network event engine io runs
    // Boot-only amortization study: 0=plain, 1=interwoven, 2=gated unified three-way. This occupies
    // an internal study namespace; it is omitted from the user help and reference configuration.
    uint32_t overlap = 0;
    ClientOutputBufferLimits client_output_buffer_limits;

    // ---- security / test commands ----------------------------------------------------------
    const char* requirepass = nullptr;   // empty/unset = off; live via CONFIG SET
    uint32_t protected_mode = 1;         // live 0|1; rejects unauthenticated non-loopback accepts
    DebugCommandMode enable_debug_command = DebugCommandMode::No; // boot-only: no|yes|local
    const char* aclfile = nullptr;        // boot-only; empty/unset disables ACL LOAD/SAVE
    uint32_t acl_pubsub_allchannels = 0;  // acl-pubsub-default: 0=resetchannels, 1=allchannels
    uint64_t acllog_max_len = 128;        // live in L4; per-io-thread bound
    std::vector<std::vector<std::string>> acl_users; // repeatable `user name rules...` lines

    // ---- persistence (dir/dbfilename are boot-only) ----------------------------------------
    const char* dir         = ".";
    const char* dbfilename  = "dump.tomo";
    const char* load_path   = nullptr;   // boot-only: load a dump before serving
    // Redis's default periodic snapshot policy. An empty vector is `save ""` and arms no
    // mutation observers or cron work.
    std::vector<SaveClause> save{{3600, 1}, {300, 100}, {60, 10000}};
    bool appendonly = false;
    AppendFsyncPolicy appendfsync = AppendFsyncPolicy::Everysec;
    ThreadMode thread_mode = ThreadMode::Split;
    // Boot-only local-read lane; its internal winners are fixed.
    uint32_t read_local = 0;            // boot-only 0|1; 1s overlap-0 GET/MGET local-read lane
    const char* appendfilename = "appendonly.aof";
    const char* appenddirname = "appendonlydir";
    uint32_t auto_aof_rewrite_percentage = 100;
    uint64_t auto_aof_rewrite_min_size = 64ull * 1024 * 1024;
    bool aof_timestamp_enabled = false;

    // TomoKV intentionally owns one keyspace. The compatibility knob is still parsed and exposed,
    // but only the honest value 1 is accepted. The protocol bound is live and applies to request
    // bulk lengths; the 32-bit Slice ABI sets the supported ceiling checked by the parser below.
    uint32_t databases = 1;
    uint64_t proto_max_bulk_len = 512ull * 1024 * 1024;

    // ---- data path (live via CONFIG SET) ----------------------------------------------------
    uint32_t zc_min         = 16384;     // zero-copy GET replies at >= this value length.
                                         // DEFAULT ON (owner: hardcode a consistent gain): -4.1%
                                         // server cycles at d16K on the wire-walled NIC, +20-24%
                                         // class on unwalled wires per the fork's history. 0 = off.
                                         // Also the multi-key gather cutover: min(zc-min, kInline).
    uint64_t maxmemory      = 0;         // bytes; zero removes all eviction-path work.
    MaxmemoryPolicy maxmemory_policy = MaxmemoryPolicy::NoEviction;
    uint32_t maxmemory_samples = 5;
    // ---- atomics (live via CONFIG SET) -----------------------------------------------------
    uint32_t atomic = 0;               // 0 = fully off, no allocation; epoch-MVCC multi-key lane
    // The in-flight credit window is always min(16 * resolved shards, 1024), derived at boot.

    // ---- scripting -----------------------------------------------------------------------------
    // Lua VM instructions an EVAL/FCALL activation may retire before it is aborted with BUSY.
    // Scripts run inside one shard-owner task, so an unbounded script would park that owner's
    // whole queue; this is the bound that makes SCRIPT KILL structurally unnecessary. Rounded up
    // to the 1000-instruction hook interval. 0 = unlimited (opt out; an owner can then stall).
    // Boot-only, and deliberately tomo-named: Redis's lua-time-limit/busy-reply-threshold is a
    // wall-clock BUSY-reply threshold for a script that keeps running, which is a different
    // mechanism, so borrowing the name would borrow the wrong semantics.
    uint64_t script_instruction_limit = 100000;
    TypeLimits type_limits;              // 8 compact-encoding limits, all live via CONFIG SET
    StreamLimits stream_limits;          // macro-node roll-over budgets, live via CONFIG SET

    // Empty flag string = notifications off.
    uint32_t notify_events = 0;
    // Owner EX batch scheduler. Boot-only: 0 preserves the FIFO drain, 1 enables head-rank /
    // static-length buckets. This consumes the existing alignment hole before the uint64_t below.
    uint32_t ex_sched = 0;

    // CLIENT TRACKING's bounded per-key remembering table (redis knob name and semantics:
    // tracking-table-max-keys, default 1000000, 0 = unlimited). The bound is applied per io
    // owner, because that is where this server keeps the table -- see tracking.cc. Nothing is
    // allocated until a connection actually enables tracking.
    uint64_t tracking_table_max_keys = 1000000;

    // TLS is a separate listener.  All fields are boot-only in v1; tls-port=0 constructs no
    // SSL_CTX, no BIO registry, and selects the compile-time-clean plaintext IO loop.
    uint16_t tls_port = 0;
    const char* tls_cert_file = nullptr;
    const char* tls_key_file = nullptr;
    const char* tls_ca_cert_file = nullptr;
    const char* tls_ca_cert_dir = nullptr;
    TlsAuthClients tls_auth_clients = TlsAuthClients::Yes;
    const char* tls_protocols = nullptr;
    const char* tls_ciphers = nullptr;
    const char* tls_ciphersuites = nullptr;
    bool tls_prefer_server_ciphers = false;

    // SLOWLOG + LATENCY. Redis knob names, grammar and semantics exactly:
    //   slowlog-log-slower-than  microseconds; -1 disables the log entirely, 0 logs everything
    //   slowlog-max-len          ring capacity; 0 keeps no entries
    //   latency-monitor-threshold milliseconds; 0 disables the latency monitor
    // -1 is a real negative here rather than an unsigned sentinel because redis's grammar accepts
    // and reports -1, and the knob-compat rule says shared features adopt the reference server's
    // grammar exactly.
    int64_t  slowlog_log_slower_than = 10000;
    uint64_t slowlog_max_len = 128;
    uint32_t latency_monitor_threshold = 0;
    // The conf file this process booted from, retained purely so CONFIG REWRITE has a destination.
    // It is set by the argv pre-scan in main, NOT by parse_config_args -- `--conf` is consumed
    // before the parser runs. Null means "started without a config file", which is exactly the
    // condition CONFIG REWRITE reports as an error.
    const char* conf_path = nullptr;
};
// DESIGN-KNOBS.md: 624 - 104 deleted field bytes + 4 for lb + 4 net alignment = 528.
// This is the explicitly accounted configuration-surface reduction; other layout locks stay put.
static_assert(sizeof(Config) == 528, "Config footprint changed; update the documented accounting");

inline constexpr uint32_t cfg_default_shards(uint32_t executors) {
    return executors >= 32 ? 256 : 8 * executors;
}

// epoll owns no io_uring ring; preserve its existing syscall persistence dependency.
inline PersistIoEngine persistence_engine(const Config& cfg) {
    return cfg.net_io == NetIoEngine::Epoll ? PersistIoEngine::Normal : PersistIoEngine::Uring;
}

// ---- tiny local parsers (const char* flavors; the Slice flavors in the .cc files are separate) --

inline bool cfg_parse_u32(const char* s, uint32_t& out) {
    if (!s || !*s) return false;
    uint64_t v = 0;
    for (const char* p = s; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + static_cast<uint64_t>(*p - '0');
        if (v > UINT32_MAX) return false;
    }
    out = static_cast<uint32_t>(v);
    return true;
}

inline bool cfg_parse_u64(const char* s, uint64_t& out) {
    if (!s || !*s) return false;
    uint64_t v = 0;
    for (const char* p = s; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        const uint32_t digit = static_cast<uint32_t>(*p - '0');
        if (v > (UINT64_MAX - digit) / 10) return false;
        v = v * 10 + digit;
    }
    out = v;
    return true;
}

// Signed flavor. The tree had no signed scalar parser until slowlog-log-slower-than, whose redis
// grammar accepts a real -1.
inline bool cfg_parse_i64(const char* s, int64_t& out) {
    if (!s || !*s) return false;
    const bool negative = *s == '-';
    const char* p = (negative || *s == '+') ? s + 1 : s;
    if (!*p) return false;
    uint64_t v = 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        const uint32_t digit = static_cast<uint32_t>(*p - '0');
        if (v > (UINT64_MAX - digit) / 10) return false;
        v = v * 10 + digit;
    }
    const uint64_t limit = negative ? (uint64_t{1} << 63) : (uint64_t{1} << 63) - 1;
    if (v > limit) return false;
    // INT64_MIN has no positive counterpart. Avoid negating it: even rejected values such as
    // INT64_MIN must not execute undefined behavior.
    out = negative ? (v == (uint64_t{1} << 63) ? std::numeric_limits<int64_t>::min()
                                                : -static_cast<int64_t>(v))
                   : static_cast<int64_t>(v);
    return true;
}

// Cross-source state: --ratio and --place are mutually exclusive WITHIN a source; across sources
// the later one (CLI over conf) silently replaces the earlier, which is what "base conf, per-run
// shape override" bench scripts want.
struct ConfigParseState {
    int ratio_source = 0;   // 0 = unset, 1 = conf, 2 = cli
    int place_source = 0;
    int save_source = 0;
};

enum : int { kConfigParsed = 0, kConfigError = 1, kConfigHelp = 2 };

// Parses one token stream (no argv[0]) into cfg. `source` is 1 for the conf file, 2 for the CLI.
// Prints its own error messages; the caller adds file context on conf failures.
inline int parse_config_args(const std::vector<const char*>& args, Config& cfg,
                             ConfigParseState& st, int source, const char* prog) {
    const int argc = static_cast<int>(args.size());
    for (int i = 0; i < argc; i++) {
        auto next = [&](const char* d) { return (i + 1 < argc) ? args[++i] : d; };
        const char* a = args[i];
        if      (!std::strcmp(a, "--port")) {
            uint32_t value = 0;
            if (!cfg_parse_u32(next(nullptr), value) || value > UINT16_MAX) {
                std::fprintf(stderr, "--port must be between 0 and %u\n", UINT16_MAX);
                return kConfigError;
            }
            cfg.port = static_cast<uint16_t>(value);
        }
        else if (!std::strcmp(a, "--tls-port")) {
            uint32_t value = 0;
            if (!cfg_parse_u32(next(nullptr), value) || value > UINT16_MAX) {
                std::fprintf(stderr, "--tls-port must be between 0 and %u\n", UINT16_MAX);
                return kConfigError;
            }
            cfg.tls_port = static_cast<uint16_t>(value);
        }
        else if (!std::strcmp(a, "--tls-cert-file")) cfg.tls_cert_file = next("");
        else if (!std::strcmp(a, "--tls-key-file")) cfg.tls_key_file = next("");
        else if (!std::strcmp(a, "--tls-ca-cert-file")) cfg.tls_ca_cert_file = next("");
        else if (!std::strcmp(a, "--tls-ca-cert-dir")) cfg.tls_ca_cert_dir = next("");
        else if (!std::strcmp(a, "--tls-auth-clients")) {
            const char* value = next(nullptr);
            if (cfg_eq_icase(value, "yes")) cfg.tls_auth_clients = TlsAuthClients::Yes;
            else if (cfg_eq_icase(value, "no")) cfg.tls_auth_clients = TlsAuthClients::No;
            else if (cfg_eq_icase(value, "optional"))
                cfg.tls_auth_clients = TlsAuthClients::Optional;
            else {
                std::fprintf(stderr, "--tls-auth-clients wants yes, no or optional\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--tls-protocols")) cfg.tls_protocols = next("");
        else if (!std::strcmp(a, "--tls-ciphers")) cfg.tls_ciphers = next("");
        else if (!std::strcmp(a, "--tls-ciphersuites")) cfg.tls_ciphersuites = next("");
        else if (!std::strcmp(a, "--tls-prefer-server-ciphers")) {
            const char* value = next(nullptr);
            if (cfg_eq_icase(value, "yes")) cfg.tls_prefer_server_ciphers = true;
            else if (cfg_eq_icase(value, "no")) cfg.tls_prefer_server_ciphers = false;
            else {
                std::fprintf(stderr, "--tls-prefer-server-ciphers wants yes or no\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--bind"))       cfg.bind_addr = next("127.0.0.1");
        else if (!std::strcmp(a, "--unixsocket")) cfg.unixsocket = next("");
        else if (!std::strcmp(a, "--maxclients")) {
            if (!cfg_parse_u32(next(nullptr), cfg.maxclients) || cfg.maxclients == 0) {
                std::fprintf(stderr, "--maxclients must be between 1 and %u\n", UINT32_MAX);
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--timeout")) {
            if (!cfg_parse_u32(next(nullptr), cfg.timeout) || cfg.timeout > INT_MAX) {
                std::fprintf(stderr, "--timeout must be between 0 and %d\n", INT_MAX);
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--tcp-keepalive")) {
            if (!cfg_parse_u32(next(nullptr), cfg.tcp_keepalive) || cfg.tcp_keepalive > INT_MAX) {
                std::fprintf(stderr, "--tcp-keepalive must be between 0 and %d\n", INT_MAX);
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--tcp-backlog")) {
            if (!cfg_parse_u32(next(nullptr), cfg.tcp_backlog) || cfg.tcp_backlog > INT_MAX) {
                std::fprintf(stderr, "--tcp-backlog must be between 0 and %d\n", INT_MAX);
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--client-output-buffer-limit")) {
            const int begin = i + 1;
            int end = begin;
            while (end < argc && std::strncmp(args[end], "--", 2) != 0) end++;
            std::vector<std::string> words;
            for (int arg = begin; arg < end; arg++) {
                const char* p = args[arg];
                while (*p) {
                    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
                    const char* start = p;
                    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
                    if (p != start) words.emplace_back(start, static_cast<size_t>(p - start));
                }
            }
            std::vector<const char*> values;
            values.reserve(words.size());
            for (const std::string& word : words) values.push_back(word.c_str());
            // The parser stages internally and commits only on success; no outer copy needed.
            const char* error = nullptr;
            if (!cfg_parse_client_output_buffer_limit(values.data(), values.size(),
                                                       cfg.client_output_buffer_limits, error)) {
                std::fprintf(stderr, "--client-output-buffer-limit: %s\n", error);
                return kConfigError;
            }
            i = end - 1;
        }
        else if (!std::strcmp(a, "--requirepass")) cfg.requirepass = next("");
        else if (!std::strcmp(a, "--aclfile")) cfg.aclfile = next("");
        else if (!std::strcmp(a, "--user")) {
            const int begin = i + 1;
            int end = begin;
            while (end < argc && std::strncmp(args[end], "--", 2) != 0) end++;
            if (end - begin < 1) {
                std::fprintf(stderr, "--user requires a username\n");
                return kConfigError;
            }
            std::vector<std::string> definition;
            try {
                for (int arg = begin; arg < end; arg++) definition.emplace_back(args[arg]);
                cfg.acl_users.push_back(std::move(definition));
            } catch (const std::bad_alloc&) {
                std::fprintf(stderr, "out of memory parsing --user\n");
                return kConfigError;
            }
            i = end - 1;
        }
        else if (!std::strcmp(a, "--acl-pubsub-default")) {
            const char* value = next(nullptr);
            if (value && cfg_eq_icase(value, "allchannels")) cfg.acl_pubsub_allchannels = 1;
            else if (value && cfg_eq_icase(value, "resetchannels")) cfg.acl_pubsub_allchannels = 0;
            else {
                std::fprintf(stderr, "--acl-pubsub-default wants allchannels or resetchannels\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--acllog-max-len")) {
            if (!cfg_parse_u64(next(nullptr), cfg.acllog_max_len)) {
                std::fprintf(stderr, "--acllog-max-len wants an unsigned integer\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--slowlog-log-slower-than")) {
            if (!cfg_parse_i64(next(nullptr), cfg.slowlog_log_slower_than) ||
                cfg.slowlog_log_slower_than < -1) {
                std::fprintf(stderr,
                             "--slowlog-log-slower-than wants microseconds, -1 to disable\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--slowlog-max-len")) {
            if (!cfg_parse_u64(next(nullptr), cfg.slowlog_max_len)) {
                std::fprintf(stderr, "--slowlog-max-len wants an unsigned integer\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--latency-monitor-threshold")) {
            if (!cfg_parse_u32(next(nullptr), cfg.latency_monitor_threshold)) {
                std::fprintf(stderr,
                             "--latency-monitor-threshold wants milliseconds, 0 to disable\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--protected-mode")) {
            const char* value = next(nullptr);
            if (value && (!std::strcmp(value, "1") || !std::strcmp(value, "yes")))
                cfg.protected_mode = 1;
            else if (value && (!std::strcmp(value, "0") || !std::strcmp(value, "no")))
                cfg.protected_mode = 0;
            else {
                std::fprintf(stderr, "--protected-mode wants 0, 1, yes or no\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--enable-debug-command")) {
            const char* value = next(nullptr);
            if (value && !std::strcmp(value, "no")) cfg.enable_debug_command = DebugCommandMode::No;
            else if (value && !std::strcmp(value, "yes")) cfg.enable_debug_command = DebugCommandMode::Yes;
            else if (value && !std::strcmp(value, "local")) cfg.enable_debug_command = DebugCommandMode::Local;
            else {
                std::fprintf(stderr, "--enable-debug-command wants no, yes or local\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--thread-mode")) {
            const char* value = next(nullptr);
            if (value && (!std::strcmp(value, "2s") || !std::strcmp(value, "split")))
                cfg.thread_mode = ThreadMode::Split;
            else if (value && (!std::strcmp(value, "1s") || !std::strcmp(value, "fused")))
                cfg.thread_mode = ThreadMode::Fused;
            else {
                std::fprintf(stderr,
                             "--thread-mode wants 2s or 1s (split/fused are aliases)\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--x-overlap")) {
            if (!cfg_parse_u32(next(nullptr), cfg.overlap) || cfg.overlap > 2) {
                std::fprintf(stderr, "--x-overlap wants 0, 1 or 2\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--read-local")) {
            if (!cfg_parse_u32(next(nullptr), cfg.read_local) || cfg.read_local > 1) {
                std::fprintf(stderr, "--read-local wants 0 or 1\n");
                return kConfigError;
            }
        }
        // WHOLE-SERVER role counts, evenly spread across L3 domains by the server itself.
        // This is the runtime replacement for authoring --place strings offline, and the knob a
        // flip controller will drive: counts in, placement out, no per-node arithmetic.
        else if (!std::strcmp(a, "--ratio")) {
            if (st.place_source == source) {
                std::fprintf(stderr, "--ratio and --place are mutually exclusive\n");
                return kConfigError;
            }
            if (st.place_source) { cfg.place = nullptr; st.place_source = 0; }
            st.ratio_source = source;
            const char* v = next("");
            unsigned a2 = 0, b = 0, c = 0;
            const int got = std::sscanf(v, "%u:%u:%u", &a2, &b, &c);
            if (got != 2 || a2 == 0 || b == 0) {
                std::fprintf(stderr, "--ratio wants global ifid:ex (e.g. 30:34); 3s was deleted 2026-08-24\n");
                return kConfigError;
            }
            cfg.even_ifid = a2; cfg.even_ex = b;
        }
        else if (!std::strcmp(a, "--shards")) {
            // Same grammar as every other numeric knob (a bare atoi accepted "16x" as 16 and
            // turned "abc"/"-5" into a misleading range message from validate_config).
            const char* value = next(nullptr);
            if (value && !std::strcmp(value, "-1")) cfg.shards = Config::kShardsAuto;
            else if (!cfg_parse_u32(value, cfg.shards) || cfg.shards == 0 || cfg.shards > 256) {
                std::fprintf(stderr, "--shards wants -1 (auto) or 1..256\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--x-ex-sched")) {
            if (!cfg_parse_u32(next(nullptr), cfg.ex_sched) || cfg.ex_sched > 1) {
                std::fprintf(stderr, "--x-ex-sched wants 0 or 1\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--lb")) {
            if (!cfg_parse_u32(next(nullptr), cfg.lb) || cfg.lb > 1) {
                std::fprintf(stderr, "--lb wants 0 or 1\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--flip-auto")) {
            if (!cfg_parse_u32(next(nullptr), cfg.flip_auto) || cfg.flip_auto > 1) {
                std::fprintf(stderr, "--flip-auto wants 0 or 1\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--flip-work-window")) {
            if (!cfg_parse_u32(next(nullptr), cfg.flip_work_window)) {
                std::fprintf(stderr,
                             "--flip-work-window wants an unsigned command count (0 disables)\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--maxmemory")) {
            if (!cfg_parse_memory(next(nullptr), cfg.maxmemory)) {
                std::fprintf(stderr, "--maxmemory wants a Redis memory value (0 disables)\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--maxmemory-policy")) {
            const char* value = next(nullptr);
            if (!value || !parse_maxmemory_policy(value, cfg.maxmemory_policy)) {
                std::fprintf(stderr, "--maxmemory-policy wants a Redis maxmemory policy\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--maxmemory-samples")) {
            if (!cfg_parse_u32(next(nullptr), cfg.maxmemory_samples) ||
                cfg.maxmemory_samples == 0 || cfg.maxmemory_samples > 64) {
                std::fprintf(stderr, "--maxmemory-samples must be between 1 and 64\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--notify-keyspace-events")) {
            const char* value = next(nullptr);
            const Slice input(value ? value : "", value ? std::strlen(value) : 0);
            uint32_t parsed = 0;
            if (!value || !parse_notify_flags(input, parsed)) {
                std::fprintf(stderr, "--notify-keyspace-events contains an invalid flag\n");
                return kConfigError;
            }
            cfg.notify_events = parsed;
        }
        else if (!std::strcmp(a, "--tracking-table-max-keys")) {
            if (!cfg_parse_u64(next(nullptr), cfg.tracking_table_max_keys)) {
                std::fprintf(stderr, "--tracking-table-max-keys wants an unsigned key count\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--save")) {
            const int begin = i + 1;
            int end = begin;
            while (end < argc && std::strncmp(args[end], "--", 2) != 0) end++;
            if (begin == end) {
                std::fprintf(stderr, "--save wants an empty value or seconds/changes pairs\n");
                return kConfigError;
            }
            std::string flat;
            for (int arg = begin; arg < end; arg++) {
                if (!flat.empty()) flat.push_back(' ');
                flat += args[arg];
            }
            std::vector<SaveClause> parsed;
            if (!cfg_parse_save_schedule(flat.data(), flat.size(), parsed)) {
                std::fprintf(stderr, "--save wants positive seconds and unsigned changes pairs\n");
                return kConfigError;
            }
            if (st.save_source != source) {
                cfg.save.clear();
                st.save_source = source;
            }
            // An explicit empty value resets the accumulated schedule. Non-empty repeated clauses
            // append within one source, matching repeated redis.conf `save` directives.
            if (flat.empty()) cfg.save.clear();
            else cfg.save.insert(cfg.save.end(), parsed.begin(), parsed.end());
            i = end - 1;
        }
        else if (!std::strcmp(a, "--databases")) {
            uint32_t value = 0;
            if (!cfg_parse_u32(next(nullptr), value) || value != 1) {
                std::fprintf(stderr, "--databases must be 1: this server owns one keyspace\n");
                return kConfigError;
            }
            cfg.databases = value;
        }
        else if (!std::strcmp(a, "--proto-max-bulk-len")) {
            uint64_t value = 0;
            if (!cfg_parse_memory(next(nullptr), value) || value < kProtoMinBulkLen ||
                value > kProtoMaxBulkLenSupported) {
                std::fprintf(stderr,
                    "--proto-max-bulk-len must be between %llu and %llu bytes\n",
                    static_cast<unsigned long long>(kProtoMinBulkLen),
                    static_cast<unsigned long long>(kProtoMaxBulkLenSupported));
                return kConfigError;
            }
            cfg.proto_max_bulk_len = value;
        }
        else if (!std::strcmp(a, "--dir"))        cfg.dir = next(".");
        else if (!std::strcmp(a, "--dbfilename")) cfg.dbfilename = next("dump.tomo");
        else if (!std::strcmp(a, "--load"))       cfg.load_path = next("");
        else if (!std::strcmp(a, "--appendonly")) {
            const char* value = next(nullptr);
            if (cfg_eq_icase(value, "yes")) cfg.appendonly = true;
            else if (cfg_eq_icase(value, "no")) cfg.appendonly = false;
            else {
                std::fprintf(stderr, "--appendonly wants yes or no\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--appendfsync")) {
            const char* value = next(nullptr);
            if (cfg_eq_icase(value, "always")) cfg.appendfsync = AppendFsyncPolicy::Always;
            else if (cfg_eq_icase(value, "everysec")) cfg.appendfsync = AppendFsyncPolicy::Everysec;
            else if (cfg_eq_icase(value, "no")) cfg.appendfsync = AppendFsyncPolicy::No;
            else {
                std::fprintf(stderr, "--appendfsync wants always, everysec or no\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--net-io")) {
            const char* value = next(nullptr);
            if (cfg_eq_icase(value, "uring")) cfg.net_io = NetIoEngine::Uring;
            else if (cfg_eq_icase(value, "epoll")) cfg.net_io = NetIoEngine::Epoll;
            else {
                std::fprintf(stderr, "--net-io wants uring or epoll\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--appendfilename")) cfg.appendfilename = next("appendonly.aof");
        else if (!std::strcmp(a, "--appenddirname")) cfg.appenddirname = next("appendonlydir");
        else if (!std::strcmp(a, "--auto-aof-rewrite-percentage")) {
            if (!cfg_parse_u32(next(nullptr), cfg.auto_aof_rewrite_percentage)) {
                std::fprintf(stderr, "--auto-aof-rewrite-percentage wants an unsigned integer\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--auto-aof-rewrite-min-size")) {
            if (!cfg_parse_memory(next(nullptr), cfg.auto_aof_rewrite_min_size)) {
                std::fprintf(stderr, "--auto-aof-rewrite-min-size wants a byte count\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--aof-use-rdb-preamble")) {
            const char* value = next(nullptr);
            if (!cfg_eq_icase(value, "yes")) {
                std::fprintf(stderr, "aof-use-rdb-preamble no is unsupported: the AOF base file is a TomoKV snapshot\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--aof-timestamp-enabled")) {
            const char* value = next(nullptr);
            if (cfg_eq_icase(value, "yes")) cfg.aof_timestamp_enabled = true;
            else if (cfg_eq_icase(value, "no")) cfg.aof_timestamp_enabled = false;
            else {
                std::fprintf(stderr, "--aof-timestamp-enabled wants yes or no\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--hash-max-compact-entries")) {
            if (!cfg_parse_u32(next(nullptr), cfg.type_limits.hash.max_entries)) return kConfigError;
        }
        else if (!std::strcmp(a, "--hash-max-compact-value")) {
            if (!cfg_parse_u32(next(nullptr), cfg.type_limits.hash.max_value)) return kConfigError;
        }
        else if (!std::strcmp(a, "--list-max-compact-entries")) {
            if (!cfg_parse_u32(next(nullptr), cfg.type_limits.list.max_entries)) return kConfigError;
        }
        else if (!std::strcmp(a, "--list-max-compact-value")) {
            if (!cfg_parse_u32(next(nullptr), cfg.type_limits.list.max_value)) return kConfigError;
        }
        else if (!std::strcmp(a, "--set-max-compact-entries")) {
            if (!cfg_parse_u32(next(nullptr), cfg.type_limits.set.max_entries)) return kConfigError;
        }
        else if (!std::strcmp(a, "--set-max-compact-value")) {
            if (!cfg_parse_u32(next(nullptr), cfg.type_limits.set.max_value)) return kConfigError;
        }
        else if (!std::strcmp(a, "--zset-max-compact-entries")) {
            if (!cfg_parse_u32(next(nullptr), cfg.type_limits.zset.max_entries)) return kConfigError;
        }
        else if (!std::strcmp(a, "--zset-max-compact-value")) {
            if (!cfg_parse_u32(next(nullptr), cfg.type_limits.zset.max_value)) return kConfigError;
        }
        else if (!std::strcmp(a, "--stream-node-max-bytes")) {
            if (!cfg_parse_u32(next(nullptr), cfg.stream_limits.node_max_bytes)) return kConfigError;
        }
        else if (!std::strcmp(a, "--stream-node-max-entries")) {
            if (!cfg_parse_u32(next(nullptr), cfg.stream_limits.node_max_entries)) return kConfigError;
        }
        else if (!std::strcmp(a, "--zc-min")) {
            const char* v = next(nullptr);
            if (!cfg_parse_u32(v, cfg.zc_min)) {
                std::fprintf(stderr, "--zc-min wants a uint32 byte count (0 disables; 16384 suggested when enabled)\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--atomic")) {
            if (!cfg_parse_u32(next(nullptr), cfg.atomic) || cfg.atomic > 1) {
                std::fprintf(stderr, "--atomic wants 0 or 1\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--script-instruction-limit")) {
            if (!cfg_parse_u64(next(nullptr), cfg.script_instruction_limit)) {
                std::fprintf(stderr, "--script-instruction-limit wants a uint64, 0 = unlimited\n");
                return kConfigError;
            }
        }
        else if (!std::strcmp(a, "--no-pin"))     cfg.pin_threads = false;
        else if (!std::strcmp(a, "--hash")) {
            const char* h = next("mix64");
            if      (!std::strcmp(h, "mix64"))   g_hash_kind = HashKind::Mix64Seeded;
            else if (!std::strcmp(h, "siphash")) g_hash_kind = HashKind::SipHash12;
            else { std::fprintf(stderr, "--hash must be mix64 | siphash\n"); return kConfigError; }
        }
        else if (!std::strcmp(a, "--place")) {
            if (st.ratio_source == source) {
                std::fprintf(stderr, "--ratio and --place are mutually exclusive\n");
                return kConfigError;
            }
            if (st.ratio_source) { cfg.even_ifid = cfg.even_ex = 0; st.ratio_source = 0; }
            st.place_source = source;
            cfg.place = next("");
        }
        else if (!std::strcmp(a, "--help")) {
            std::printf("usage: %s [conf-file] [--conf FILE] [--port N] [--bind A] [--unixsocket PATH]\n"
                        "       [--shards N] [--zc-min N] [--no-pin]\n"
                        "  conf file: `name value` per line, # comments; same names as the flags\n"
                        "  without the leading --; `pin no` spells --no-pin. CLI flags override the\n"
                        "  file. See tomokv.conf in the repo root for the annotated full set.\n"
                        "  threading: --thread-mode 2s|1s --read-local 0|1 (defaults 2s, 0)\n"
                        "             (split/fused are mode aliases)\n"
                        "  placement (default derived from allowed CPUs):\n"
                        "    --ratio io:ex               global counts, split mode only\n"
                        "    --place role@cpu,...        explicit CPUs; roles are ifid, ex\n"
                        "    --shards -1|N               default auto: min(8*executors, 256)\n"
                        "  load balancing: --lb 0|1 (default 1)\n"
                        "  flip controller: --flip-auto 0|1\n"
                        "    --flip-work-window N       commands per fingerprint sample (default 100)\n"
                        "    --zc-min N                  zero-copy replies at >= N bytes (0=off)\n"
                        "  cache: --maxmemory BYTES --maxmemory-policy POLICY\n"
                        "         --maxmemory-samples N (1..64, default 5)\n"
                        "  limits: --maxclients N --timeout SECONDS --tcp-keepalive SECONDS\n"
                        "          --tcp-backlog N --client-output-buffer-limit CLASS HARD SOFT SECONDS ...\n"
                        "  network engine: --net-io uring|epoll (boot-only; default uring)\n"
                        "  TLS: --tls-port N --tls-cert-file PATH --tls-key-file PATH\n"
                        "       --tls-ca-cert-file PATH --tls-ca-cert-dir PATH\n"
                        "       --tls-auth-clients yes|no|optional --tls-protocols LIST\n"
                        "       --tls-ciphers LIST --tls-ciphersuites LIST\n"
                        "       --tls-prefer-server-ciphers yes|no\n"
                        "  notifications: --notify-keyspace-events FLAGS (default empty/off)\n"
                        "  client-side caching: --tracking-table-max-keys N "
                        "(default 1000000, 0=unlimited)\n"
                        "  persistence: --dir PATH --dbfilename NAME --load PATH\n"
                        "    --save SECONDS CHANGES (repeatable; --save \"\" disables)\n"
                        "    --appendonly yes|no --appendfsync always|everysec|no\n"
                        "    --appendfilename NAME --appenddirname NAME\n"
                        "    --auto-aof-rewrite-percentage N --auto-aof-rewrite-min-size BYTES\n"
                        "    --aof-use-rdb-preamble yes --aof-timestamp-enabled yes|no\n"
                        "  compatibility: --databases 1 --proto-max-bulk-len BYTES\n"
                        "  security: --requirepass PASSWORD --protected-mode 0|1|yes|no\n"
                        "            --enable-debug-command no|yes|local --aclfile PATH\n"
                        "            --user NAME RULE... --acl-pubsub-default allchannels|resetchannels\n"
                        "            --acllog-max-len N\n"
                        "  observability: --slowlog-log-slower-than US (default 10000; -1 off)\n"
                        "            --slowlog-max-len N (default 128)\n"
                        "            --latency-monitor-threshold MS (default 0 = off)\n"
                        "  atomics: --atomic 0|1 (default 0)\n"
                        "  scripting: --script-instruction-limit N (default 100000; 0=unlimited)\n"
                        "  compact encodings: --{hash,list,set,zset}-max-compact-{entries,value} N\n"
                        "  streams: --stream-node-max-bytes N --stream-node-max-entries N\n"
                        "  misc: --hash mix64|siphash\n"
                        "  (--mode/--wb/--nodes died with 3s, 2026-08)\n",
                        prog);
            return kConfigHelp;
        }
        else {
            std::fprintf(stderr, "unknown argument '%s' (see --help)\n", a);
            return kConfigError;
        }
    }
    return kConfigParsed;
}

// Post-parse validation shared by every source combination. Call once, after all token streams.
inline int validate_config(const Config& cfg) {
    if (cfg.thread_mode == ThreadMode::Split && cfg.overlap == 2) {
        std::fprintf(stderr,
                     "--x-overlap 2 is only available with --thread-mode 1s; 2s has no deep unified-stream schedule\n");
        return kConfigError;
    }
    if (cfg.thread_mode == ThreadMode::Fused && cfg.overlap != 0 &&
        cfg.net_io != NetIoEngine::Uring) {
        std::fprintf(stderr,
                     "--thread-mode 1s with --x-overlap %u requires --net-io uring for its single submit boundary\n",
                     cfg.overlap);
        return kConfigError;
    }
    if (cfg.thread_mode == ThreadMode::Fused && (cfg.even_ifid || cfg.even_ex)) {
        std::fprintf(stderr,
                     "--ratio is unavailable with --thread-mode 1s: every thread handles networking and execution\n");
        return kConfigError;
    }
    if (cfg.thread_mode == ThreadMode::Fused && cfg.flip_auto) {
        std::fprintf(stderr,
                     "--flip-auto is unavailable with --thread-mode 1s\n");
        return kConfigError;
    }
    if (cfg.databases != 1) {
        std::fprintf(stderr, "databases must be 1: this server owns one keyspace\n");
        return kConfigError;
    }
    if (cfg.proto_max_bulk_len < kProtoMinBulkLen ||
        cfg.proto_max_bulk_len > kProtoMaxBulkLenSupported) {
        std::fprintf(stderr, "proto-max-bulk-len is outside the supported range\n");
        return kConfigError;
    }
    if (cfg.port && cfg.tls_port && cfg.port == cfg.tls_port) {
        std::fprintf(stderr, "port and tls-port must be different listeners\n");
        return kConfigError;
    }
    if (cfg.tls_port) {
        if (!cfg.tls_cert_file || !*cfg.tls_cert_file) {
            std::fprintf(stderr, "tls-port requires tls-cert-file\n");
            return kConfigError;
        }
        if (!cfg.tls_key_file || !*cfg.tls_key_file) {
            std::fprintf(stderr, "tls-port requires tls-key-file\n");
            return kConfigError;
        }
        if (cfg.tls_auth_clients != TlsAuthClients::No &&
            (!cfg.tls_ca_cert_file || !*cfg.tls_ca_cert_file) &&
            (!cfg.tls_ca_cert_dir || !*cfg.tls_ca_cert_dir)) {
            std::fprintf(stderr,
                         "tls-auth-clients yes or optional requires tls-ca-cert-file or tls-ca-cert-dir\n");
            return kConfigError;
        }
    }
    if (cfg.aclfile && *cfg.aclfile && !cfg.acl_users.empty()) {
        std::fprintf(stderr, "Configuring Redis with users defined in redis.conf and at the same setting an ACL file path is invalid. This setup is very likely to lead to configuration errors and security holes, please define either an ACL file or declare users directly in your redis.conf, but not both.\n");
        return kConfigError;
    }
    if (cfg.shards != Config::kShardsAuto && (cfg.shards == 0 || cfg.shards > 256)) {
        std::fprintf(stderr, "shards must be between 1 and 256\n");
        return kConfigError;
    }
    if (!cfg.dir || !*cfg.dir || !cfg.dbfilename || !*cfg.dbfilename ||
        std::strchr(cfg.dbfilename, '/')) {
        std::fprintf(stderr, "--dir must be non-empty and --dbfilename must be a plain filename\n");
        return kConfigError;
    }
    if (!cfg.appendfilename || !*cfg.appendfilename || std::strchr(cfg.appendfilename, '/') ||
        !cfg.appenddirname || !*cfg.appenddirname || std::strchr(cfg.appenddirname, '/')) {
        std::fprintf(stderr, "--appendfilename and --appenddirname must be plain names\n");
        return kConfigError;
    }
    return kConfigParsed;
}

inline bool cfg_is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

inline uint8_t cfg_hex_digit(char c) {
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
    return static_cast<uint8_t>(c - 'A' + 10);
}

// Redis sdssplitargs grammar, used by config.c: double quotes recognize C-style escapes and
// \xHH, single quotes recognize only \', and a closing quote must end the token. Quotes may start
// after an unquoted prefix (`>"pass phrase"` is one ACL rule). A '#' has no special meaning here:
// config.c treats it as a comment only when it is the first character of the trimmed line.
inline bool cfg_split_args(const char* line, std::vector<std::string>& out) {
    const char* p = line;
    while (true) {
        while (*p && std::isspace(static_cast<unsigned char>(*p))) p++;
        if (!*p) return true;

        std::string current;
        bool in_double = false;
        bool in_single = false;
        bool done = false;
        while (!done) {
            if (in_double) {
                if (*p == '\\' && p[1] == 'x' && cfg_is_hex_digit(p[2]) &&
                    cfg_is_hex_digit(p[3])) {
                    current.push_back(static_cast<char>((cfg_hex_digit(p[2]) << 4) |
                                                        cfg_hex_digit(p[3])));
                    p += 3;
                } else if (*p == '\\' && p[1]) {
                    p++;
                    char c = *p;
                    if (*p == 'n') c = '\n';
                    else if (*p == 'r') c = '\r';
                    else if (*p == 't') c = '\t';
                    else if (*p == 'b') c = '\b';
                    else if (*p == 'a') c = '\a';
                    current.push_back(c);
                } else if (*p == '"') {
                    if (p[1] && !std::isspace(static_cast<unsigned char>(p[1]))) return false;
                    done = true;
                } else if (!*p) {
                    return false;
                } else {
                    current.push_back(*p);
                }
            } else if (in_single) {
                if (*p == '\\' && p[1] == '\'') {
                    p++;
                    current.push_back('\'');
                } else if (*p == '\'') {
                    if (p[1] && !std::isspace(static_cast<unsigned char>(p[1]))) return false;
                    done = true;
                } else if (!*p) {
                    return false;
                } else {
                    current.push_back(*p);
                }
            } else {
                switch (*p) {
                case ' ':
                case '\n':
                case '\r':
                case '\t':
                case '\0':
                    done = true;
                    break;
                case '"':
                    in_double = true;
                    break;
                case '\'':
                    in_single = true;
                    break;
                default:
                    current.push_back(*p);
                    break;
                }
            }
            if (*p) p++;
        }
        out.push_back(std::move(current));
    }
}

// Conf-file loader: translates `name value...` lines into the exact --flag token stream the CLI
// parser consumes, so the file cannot drift from the flag grammar. `pin yes|no` maps to the
// valueless --no-pin. Token storage lives in `store` (must outlive parsing — Config keeps
// const char* views into it).
inline bool load_conf_file(const char* path, std::vector<std::string>& store) {
    std::FILE* f = std::fopen(path, "r");
    if (!f) {
        std::fprintf(stderr, "cannot open conf file '%s'\n", path);
        return false;
    }
    char line[4096];
    int lineno = 0;
    bool ok = true;
    while (ok && std::fgets(line, sizeof(line), f)) {
        lineno++;
        const char* first = line;
        while (*first && std::isspace(static_cast<unsigned char>(*first))) first++;
        if (*first == '#' || !*first) continue;
        std::vector<std::string> words;
        if (!cfg_split_args(first, words)) {
            std::fprintf(stderr, "%s:%d: Unbalanced quotes in configuration line\n", path,
                         lineno);
            ok = false;
            continue;
        }
        if (words.empty()) continue;
        // The Redis reference spelling for an empty notification mask is two quotes. This loader
        // otherwise deliberately has no shell quoting grammar, so normalize that one exact token
        // to the empty argv value consumed by the shared flag parser.
        if (words[0] == "notify-keyspace-events" && words.size() == 2 && words[1] == "\"\"")
            words[1].clear();
        if (words[0] == "pin") {
            if (words.size() != 2 || (words[1] != "yes" && words[1] != "no")) {
                std::fprintf(stderr, "%s:%d: pin wants yes|no\n", path, lineno);
                ok = false;
            } else if (words[1] == "no") {
                store.emplace_back("--no-pin");
            }
            continue;
        }
        store.emplace_back("--" + words[0]);
        for (size_t w = 1; w < words.size(); w++) store.emplace_back(words[w]);
    }
    std::fclose(f);
    return ok;
}

}  // namespace tomo

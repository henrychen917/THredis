// t_server.cc — Redis client/tool compatibility, introspection, and cross-shard keyspace commands.
//
// Connection-local handlers use a thread-local context bound for the duration of the synchronous
// IO-thread call. Client metadata lives in a cold, locked catalog here rather than enlarging the
// 1984-byte Client. Store handlers still receive only (Shard&, Op&) and never touch a socket.
#include "command.h"
#include "acl.h"
#include "auth.h"
#include "cmdmeta.h"
#include "debug.h"
#include "info_stats.h"
#include "scripting.h"
#include "server_tail.h"
#include "slowlog.h"
#include "../base/alloc.h"
#include "../core/server.h"
#include "../core/lbsignals.h"
#include "../core/pubsub_event.h"
#include "../core/thread.h"
#include "../exec/op.h"
#include "../net/conn.h"
#include "../net/resp.h"
#include "../store/kvobj.h"
#include "../store/eviction.h"
#include "../snapshot/snapshot.h"
#include "multi.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/socket.h>

namespace tomo {
namespace {

constexpr const char* kVersion = "0.1-cpp";
constexpr uint64_t kScanInnerMask = (uint64_t{1} << 56) - 1;
constexpr uint64_t kMaxInnerCursor = (uint64_t{1} << 33) - 1;

Server* g_server = nullptr;
thread_local Client* g_client = nullptr;
thread_local ThreadCtx* g_thread = nullptr;
uint64_t g_started_monotonic_ns = 0;

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

bool parse_u64(Slice s, uint64_t& out) {
    if (!s.n) return false;
    uint64_t value = 0;
    for (uint32_t i = 0; i < s.n; i++) {
        const char ch = s.p[i];
        if (ch < '0' || ch > '9') return false;
        const uint32_t digit = static_cast<uint32_t>(ch - '0');
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

bool parse_i64_slice(Slice s, int64_t& out) {
    if (!s.n || s.n > 20) return false;
    uint32_t i = 0;
    bool negative = false;
    if (s.p[0] == '-' || s.p[0] == '+') { negative = s.p[0] == '-'; i = 1; }
    if (i >= s.n) return false;
    uint64_t value = 0;
    for (; i < s.n; i++) {
        const char ch = s.p[i];
        if (ch < '0' || ch > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(ch - '0');
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
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

// Canonical decimal, as redis's string2ll: no leading '+', no leading zeroes, no negative zero.
// This is the COMMAND-argument spelling; the lax parse_i64_slice above stays for config values,
// where a conf file writing "+10" or "010" has always been accepted.
bool parse_i64_canonical(Slice s, int64_t& out) {
    if (!s.n || s.n > 20) return false;
    uint32_t i = 0;
    bool negative = false;
    if (s.p[0] == '-') { negative = true; i = 1; }
    if (i >= s.n) return false;
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
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
        value = value * 10 + digit;
    }
    const uint64_t limit = negative ? (uint64_t{1} << 63) : (uint64_t{1} << 63) - 1;
    if (value > limit) return false;
    out = negative ? (value == (uint64_t{1} << 63) ? std::numeric_limits<int64_t>::min()
                                                   : -static_cast<int64_t>(value))
                   : static_cast<int64_t>(value);
    return true;
}

std::string lower_name(const char* name) {
    std::string out(name);
    for (char& ch : out)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    return out;
}

void appendf(std::string& out, const char* fmt, ...) {
    char stack[512];
    va_list ap;
    va_start(ap, fmt);
    va_list copy;
    va_copy(copy, ap);
    int n = std::vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(copy); return; }
    if (static_cast<size_t>(n) < sizeof(stack)) out.append(stack, static_cast<size_t>(n));
    else {
        std::vector<char> buf(static_cast<size_t>(n) + 1);
        std::vsnprintf(buf.data(), buf.size(), fmt, copy);
        out.append(buf.data(), static_cast<size_t>(n));
    }
    va_end(copy);
}

struct ClientMeta {
    std::string addr;
    std::string laddr;
    std::string name;
    std::string lib_name;
    std::string lib_ver;
    uint64_t created_ms = 0;
    uint32_t subscriptions = 0;
    uint32_t pattern_subscriptions = 0;
    uint32_t shard_subscriptions = 0;
    bool unix_socket = false;
    bool no_evict = false;
    bool no_touch = false;
    // Mirrored from the io loop's tracking state purely so CLIENT INFO/LIST can report the
    // redis field names. Nothing reads these except the info-line formatter.
    bool tracking = false;
    int64_t tracking_redirect = -1;
};

// One catalog per IO thread. It contains no cross-thread Client pointer and is keyed by the
// process-unique connection id used on the IO-to-IO transport.
thread_local std::unordered_map<uint64_t, ClientMeta> g_client_meta;

bool valid_client_text(Slice value) {
    for (uint32_t i = 0; i < value.n; i++) {
        const unsigned char ch = static_cast<unsigned char>(value.p[i]);
        // Redis strings are length-delimited, so an embedded NUL is data rather than a line/text
        // delimiter. Other ASCII controls, space, and DEL are rejected by CLIENT SETNAME/SETINFO.
        if ((ch != 0 && ch <= ' ') || ch == 127) return false;
    }
    return true;
}

ClientMeta* client_meta(Client* client) {
    if (!client) return nullptr;
    auto found = g_client_meta.find(client->id());
    return found == g_client_meta.end() ? nullptr : &found->second;
}

const ClientMeta* client_meta(const Client* client) {
    return client_meta(const_cast<Client*>(client));
}

std::string client_info_line_impl(const Client& client, const ClientMeta& meta, uint64_t now_ms) {
    char flags[16];
    char* flag = flags;
    if (client.subscriber_mode()) *flag++ = 'P';
    if (multi_session_active(client)) *flag++ = 'x';
    if (client.blocked()) *flag++ = 'b';
    if (client.closing()) *flag++ = 'c';
    if (meta.unix_socket) *flag++ = 'U';
    if (meta.no_evict) *flag++ = 'e';
    if (meta.no_touch) *flag++ = 'T';
    if (flag == flags) *flag++ = 'N';
    *flag = '\0';

    char events[3];
    char* event = events;
    if (!client.closing()) *event++ = 'r';
    if (client.send_inflight() || client.buffered_output_bytes()) *event++ = 'w';
    *event = '\0';

    const uint64_t age = now_ms >= meta.created_ms ? (now_ms - meta.created_ms) / 1000 : 0;
    const uint64_t now_s = now_ms / 1000;
    const uint64_t idle = now_s >= client.last_interaction_s()
        ? now_s - client.last_interaction_s() : 0;
    const uint64_t qbuf = client.rlen() - client.rpos();
    const uint64_t qbuf_free = client.rcap() >= client.rlen()
        ? client.rcap() - client.rlen() : 0;
    const uint64_t omem = client.buffered_output_bytes();
    const uint64_t total_mem = sizeof(Client) + client.rcap() + omem +
                               multi_session_memory(client);
    const int64_t multi = multi_session_active(client)
        ? static_cast<int64_t>(multi_session_queue_size(client)) : -1;
    const char* username = acl_username(client.acl_user_idx());
    std::string last_cmd = "NULL";
    const uint64_t dispatched = client.rob().dispatch_id();
    if (dispatched) {
        const Op& last = client.rob().at(dispatched - 1);
        if (last.spec) {
            last_cmd.assign(last.spec->name);
            for (char& ch : last_cmd)
                if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
        }
    }

    std::string out;
    appendf(out,
        "id=%llu addr=%s laddr=%s fd=%d name=%s age=%llu idle=%llu flags=%s db=%u "
        "sub=%u psub=%u ssub=%u multi=%lld watch=%u qbuf=%llu qbuf-free=%llu "
        "argv-mem=0 multi-mem=%llu rbs=%llu rbp=%llu obl=0 oll=%u omem=%llu "
        "tot-mem=%llu events=%s cmd=%s user=%s redir=%lld resp=%u lib-name=%s lib-ver=%s\n",
        static_cast<unsigned long long>(client.id()), meta.addr.c_str(), meta.laddr.c_str(),
        client.fd(), meta.name.c_str(), static_cast<unsigned long long>(age),
        static_cast<unsigned long long>(idle), flags, client.session().db_index,
        meta.subscriptions, meta.pattern_subscriptions, meta.shard_subscriptions,
        static_cast<long long>(multi), multi_session_watch_size(client),
        static_cast<unsigned long long>(qbuf), static_cast<unsigned long long>(qbuf_free),
        static_cast<unsigned long long>(multi_session_memory(client)),
        static_cast<unsigned long long>(client.rcap()),
        static_cast<unsigned long long>(client.rcap()), client.output_list_length(),
        static_cast<unsigned long long>(omem), static_cast<unsigned long long>(total_mem),
        events, last_cmd.c_str(), username,
        static_cast<long long>(meta.tracking ? meta.tracking_redirect : -1),
        client.resp3() ? 3u : 2u,
        meta.lib_name.c_str(), meta.lib_ver.c_str());
    return out;
}

enum class ConfigKind : uint8_t {
    String, Bool, Unsigned, Bytes, Enum, Policy, ClientOutputBufferLimit, NotifyFlags, Save,
    // slowlog-log-slower-than is the tree's first genuinely signed knob: redis's grammar accepts
    // and reports -1, so the unsigned sentinel that --atomic-window uses would not round-trip.
    Signed
};
struct ConfigValue {
    const char* name;
    ConfigKind kind;
    std::string value;
    bool immutable = false;
};

std::mutex g_config_mu;
std::vector<ConfigValue> g_config;
ClientOutputBufferLimits g_client_obuf_limits;

void add_config(const char* name, ConfigKind kind, uint64_t value) {
    g_config.push_back(ConfigValue{name, kind, std::to_string(value)});
}

void init_config(const Config& cfg) {
    std::lock_guard<std::mutex> lock(g_config_mu);
    g_config.clear();
    g_config.push_back({"save", ConfigKind::Save, cfg_save_schedule_string(cfg.save)});
    g_config.push_back({"dir", ConfigKind::String, (cfg.dir && *cfg.dir) ? cfg.dir : "."});
    g_config.push_back({"dbfilename", ConfigKind::String,
                        (cfg.dbfilename && *cfg.dbfilename) ? cfg.dbfilename : "dump.tomo"});
    g_config.push_back({"appendonly", ConfigKind::Bool, cfg.appendonly ? "yes" : "no", true});
    const char* appendfsync = cfg.appendfsync == AppendFsyncPolicy::Always ? "always" :
                              cfg.appendfsync == AppendFsyncPolicy::No ? "no" : "everysec";
    g_config.push_back({"appendfsync", ConfigKind::Enum, appendfsync});
    g_config.push_back({"persist-io", ConfigKind::Enum,
                        cfg.persist_io == PersistIoEngine::Normal ? "normal" : "uring", true});
    // Boot-only, like persist-io: reported so an operator can confirm which engine a running
    // server actually chose, and refused by CONFIG SET rather than silently accepted.
    g_config.push_back({"net-io", ConfigKind::Enum,
                        cfg.net_io == NetIoEngine::Epoll ? "epoll" : "uring", true});
    g_config.push_back({"appendfilename", ConfigKind::String, cfg.appendfilename, true});
    g_config.push_back({"appenddirname", ConfigKind::String, cfg.appenddirname, true});
    add_config("auto-aof-rewrite-percentage", ConfigKind::Unsigned,
               cfg.auto_aof_rewrite_percentage);
    add_config("auto-aof-rewrite-min-size", ConfigKind::Bytes,
               cfg.auto_aof_rewrite_min_size);
    g_config.push_back({"aof-use-rdb-preamble", ConfigKind::String, "yes"});
    g_config.push_back({"aof-timestamp-enabled", ConfigKind::Bool,
                        cfg.aof_timestamp_enabled ? "yes" : "no"});
    add_config("maxmemory", ConfigKind::Bytes, cfg.maxmemory);
    g_config.push_back({"maxmemory-policy", ConfigKind::Policy,
                        maxmemory_policy_name(cfg.maxmemory_policy)});
    add_config("maxmemory-samples", ConfigKind::Unsigned, cfg.maxmemory_samples);
    g_config.push_back({"script-instruction-limit", ConfigKind::Unsigned,
                        std::to_string(cfg.script_instruction_limit), true});
    g_config.push_back({"script-crossshard-max-bytes", ConfigKind::Signed,
                        std::to_string(cfg.script_crossshard_max_bytes), true});
    g_config.push_back({"script-crossshard-workbench-bytes", ConfigKind::Signed,
                        std::to_string(cfg.script_crossshard_workbench_bytes), true});
    g_config.push_back({"script-crossshard-conflict-retries", ConfigKind::Signed,
                        std::to_string(cfg.script_crossshard_conflict_retries), true});
    g_config.push_back({"script-crossshard-cut-slots", ConfigKind::Signed,
                        std::to_string(cfg.script_crossshard_cut_slots), true});
    add_config("maxclients", ConfigKind::Unsigned, cfg.maxclients);
    add_config("timeout", ConfigKind::Unsigned, cfg.timeout);
    add_config("tcp-keepalive", ConfigKind::Unsigned, cfg.tcp_keepalive);
    add_config("tcp-backlog", ConfigKind::Unsigned, cfg.tcp_backlog);
    g_config.push_back({"tls-port", ConfigKind::Unsigned, std::to_string(cfg.tls_port), true});
    g_config.push_back({"tls-cert-file", ConfigKind::String,
                        cfg.tls_cert_file ? cfg.tls_cert_file : "", true});
    g_config.push_back({"tls-key-file", ConfigKind::String,
                        cfg.tls_key_file ? cfg.tls_key_file : "", true});
    g_config.push_back({"tls-ca-cert-file", ConfigKind::String,
                        cfg.tls_ca_cert_file ? cfg.tls_ca_cert_file : "", true});
    g_config.push_back({"tls-ca-cert-dir", ConfigKind::String,
                        cfg.tls_ca_cert_dir ? cfg.tls_ca_cert_dir : "", true});
    const char* tls_auth = cfg.tls_auth_clients == TlsAuthClients::No ? "no" :
                           cfg.tls_auth_clients == TlsAuthClients::Optional ? "optional" : "yes";
    g_config.push_back({"tls-auth-clients", ConfigKind::Enum, tls_auth, true});
    g_config.push_back({"tls-protocols", ConfigKind::String,
                        cfg.tls_protocols ? cfg.tls_protocols : "", true});
    g_config.push_back({"tls-ciphers", ConfigKind::String,
                        cfg.tls_ciphers ? cfg.tls_ciphers : "", true});
    g_config.push_back({"tls-ciphersuites", ConfigKind::String,
                        cfg.tls_ciphersuites ? cfg.tls_ciphersuites : "", true});
    g_config.push_back({"tls-prefer-server-ciphers", ConfigKind::Bool,
                        cfg.tls_prefer_server_ciphers ? "yes" : "no", true});
    g_config.push_back({"tls-ktls", ConfigKind::Bool, cfg.tls_ktls ? "yes" : "no", true});
    g_client_obuf_limits = cfg.client_output_buffer_limits;
    g_config.push_back({"client-output-buffer-limit", ConfigKind::ClientOutputBufferLimit,
                        cfg_client_output_buffer_limit_string(g_client_obuf_limits)});
    g_config.push_back({"notify-keyspace-events", ConfigKind::NotifyFlags,
                        serialize_notify_flags(cfg.notify_events)});
    // Boot-latched: the io owners read the bound directly out of Config, so it is reported but
    // not live-settable (redis allows CONFIG SET; see NOTES-CLIMON2.md).
    g_config.push_back({"tracking-table-max-keys", ConfigKind::Unsigned,
                        std::to_string(cfg.tracking_table_max_keys), true});
    add_config("databases", ConfigKind::Unsigned, cfg.databases);
    add_config("proto-max-bulk-len", ConfigKind::Bytes, cfg.proto_max_bulk_len);
    add_config("zc-min", ConfigKind::Unsigned, cfg.zc_min);
    add_config("atomic", ConfigKind::Unsigned, cfg.atomic);
    add_config("atomic-window", ConfigKind::Unsigned, cfg.atomic_window);
    add_config("hash-max-compact-entries", ConfigKind::Unsigned, cfg.type_limits.hash.max_entries);
    add_config("hash-max-compact-value", ConfigKind::Unsigned, cfg.type_limits.hash.max_value);
    add_config("list-max-compact-entries", ConfigKind::Unsigned, cfg.type_limits.list.max_entries);
    add_config("list-max-compact-value", ConfigKind::Unsigned, cfg.type_limits.list.max_value);
    add_config("set-max-compact-entries", ConfigKind::Unsigned, cfg.type_limits.set.max_entries);
    add_config("set-max-compact-value", ConfigKind::Unsigned, cfg.type_limits.set.max_value);
    add_config("zset-max-compact-entries", ConfigKind::Unsigned, cfg.type_limits.zset.max_entries);
    add_config("zset-max-compact-value", ConfigKind::Unsigned, cfg.type_limits.zset.max_value);
    add_config("stream-node-max-bytes", ConfigKind::Unsigned,
               cfg.stream_limits.node_max_bytes);
    add_config("stream-node-max-entries", ConfigKind::Unsigned,
               cfg.stream_limits.node_max_entries);
    g_config.push_back({"requirepass", ConfigKind::String,
                        cfg.requirepass ? cfg.requirepass : ""});
    g_config.push_back({"protected-mode", ConfigKind::Bool,
                        cfg.protected_mode != 0 ? "yes" : "no"});
    g_config.push_back({"aclfile", ConfigKind::String, cfg.aclfile ? cfg.aclfile : "", true});
    g_config.push_back({"acl-pubsub-default", ConfigKind::String,
                        cfg.acl_pubsub_allchannels ? "allchannels" : "resetchannels"});
    add_config("acllog-max-len", ConfigKind::Unsigned, cfg.acllog_max_len);
    g_config.push_back({"slowlog-log-slower-than", ConfigKind::Signed,
                        std::to_string(cfg.slowlog_log_slower_than)});
    add_config("slowlog-max-len", ConfigKind::Unsigned, cfg.slowlog_max_len);
    add_config("latency-monitor-threshold", ConfigKind::Unsigned,
               cfg.latency_monitor_threshold);
    const char* debug_mode = cfg.enable_debug_command == DebugCommandMode::Yes ? "yes" :
                             cfg.enable_debug_command == DebugCommandMode::Local ? "local" : "no";
    g_config.push_back({"enable-debug-command", ConfigKind::String, debug_mode, true});
    if (g_server) {
        const char* password = cfg.requirepass ? cfg.requirepass : "";
        auth_publish_requirepass(*g_server, Slice(password, std::strlen(password)));
    }
}

ConfigValue* find_config(Slice name) {
    for (ConfigValue& item : g_config)
        if (eq_icase(name, item.name)) return &item;
    return nullptr;
}

bool authenticate_acl_user(Slice username, Slice password) {
    uint32_t index = kAclDefaultUser;
    if (!acl_authenticate(username, password, index)) return false;
    if (g_client) g_client->set_acl_user_idx(index);
    return true;
}

void note_acl_auth_denial(Op& op, Slice username) {
    if (!g_thread || !g_client) return;
    g_thread->sig().acl_access_denied_auth++;
    acl_log_denial(*g_thread, *g_client, AclDeniedReason::Auth,
                   AclLogContext::Toplevel, op.cmd_name(), username);
}

bool parse_bytes(Slice input, uint64_t& value) {
    return cfg_parse_memory(input.p, input.n, value);
}

bool parse_client_output_buffer_limit_slice(Slice input,
                                            const ClientOutputBufferLimits& base,
                                            ClientOutputBufferLimits& out,
                                            const char*& error) {
    std::vector<std::string> words;
    size_t pos = 0;
    while (pos < input.n) {
        while (pos < input.n && (input.p[pos] == ' ' || input.p[pos] == '\t' ||
                                 input.p[pos] == '\r' || input.p[pos] == '\n')) pos++;
        const size_t begin = pos;
        while (pos < input.n && input.p[pos] != ' ' && input.p[pos] != '\t' &&
               input.p[pos] != '\r' && input.p[pos] != '\n') pos++;
        if (pos > begin) words.emplace_back(input.p + begin, pos - begin);
    }
    std::vector<const char*> argv;
    argv.reserve(words.size());
    for (const std::string& word : words) argv.push_back(word.c_str());
    out = base;
    return cfg_parse_client_output_buffer_limit(argv.data(), argv.size(), out, error);
}

bool normalize_config(const ConfigValue& entry, Slice input, std::string& out) {
    switch (entry.kind) {
        case ConfigKind::String:
            if (!std::strcmp(entry.name, "acl-pubsub-default")) {
                if (eq_icase(input, "allchannels")) out = "allchannels";
                else if (eq_icase(input, "resetchannels")) out = "resetchannels";
                else return false;
                return true;
            }
            out.assign(input.p, input.n);
            return true;
        case ConfigKind::Bool:
            if (eq_icase(input, "yes")) { out = "yes"; return true; }
            if (eq_icase(input, "no")) { out = "no"; return true; }
            if (input == Slice("1", 1)) { out = "yes"; return true; }
            if (input == Slice("0", 1)) { out = "no"; return true; }
            return false;
        case ConfigKind::Unsigned: {
            uint64_t value = 0;
            if (!parse_u64(input, value)) return false;
            if ((std::strstr(entry.name, "compact") || !std::strcmp(entry.name, "zc-min")) &&
                value > UINT32_MAX) return false;
            if (!std::strcmp(entry.name, "maxmemory-samples") && (value == 0 || value > 64))
                return false;
            if (!std::strcmp(entry.name, "atomic") && value > 1) return false;
            if (!std::strcmp(entry.name, "atomic-window") && value > UINT32_MAX) return false;
            if (!std::strcmp(entry.name, "databases") && value != 1) return false;
            if (!std::strcmp(entry.name, "auto-aof-rewrite-percentage") &&
                value > UINT32_MAX) return false;
            if (!std::strcmp(entry.name, "maxclients") && (value == 0 || value > UINT32_MAX))
                return false;
            if ((!std::strcmp(entry.name, "timeout") ||
                 !std::strcmp(entry.name, "tcp-keepalive") ||
                 !std::strcmp(entry.name, "tcp-backlog")) && value > INT_MAX)
                return false;
            out = std::to_string(value);
            return true;
        }
        case ConfigKind::Bytes: {
            uint64_t value = 0;
            if (!parse_bytes(input, value)) return false;
            if (!std::strcmp(entry.name, "proto-max-bulk-len") &&
                (value < kProtoMinBulkLen || value > kProtoMaxBulkLenSupported)) return false;
            out = std::to_string(value);
            return true;
        }
        case ConfigKind::Enum:
            if (eq_icase(input, "always")) { out = "always"; return true; }
            if (eq_icase(input, "everysec")) { out = "everysec"; return true; }
            if (eq_icase(input, "no")) { out = "no"; return true; }
            return false;
        case ConfigKind::Policy: {
            static const char* policies[] = {"noeviction", "allkeys-lru", "allkeys-lfu",
                "allkeys-random", "volatile-lru", "volatile-lfu", "volatile-random", "volatile-ttl"};
            for (const char* policy : policies)
                if (eq_icase(input, policy)) { out = policy; return true; }
            return false;
        }
        case ConfigKind::ClientOutputBufferLimit: {
            ClientOutputBufferLimits parsed;
            const char* error = nullptr;
            if (!parse_client_output_buffer_limit_slice(input, g_client_obuf_limits,
                                                        parsed, error)) return false;
            out = cfg_client_output_buffer_limit_string(parsed);
            return true;
        }
        case ConfigKind::NotifyFlags: {
            uint32_t flags = 0;
            if (!parse_notify_flags(input, flags)) return false;
            out = serialize_notify_flags(flags);
            return true;
        }
        case ConfigKind::Signed: {
            int64_t value = 0;
            if (!parse_i64_slice(input, value)) return false;
            if (!std::strcmp(entry.name, "slowlog-log-slower-than") && value < -1) return false;
            out = std::to_string(value);
            return true;
        }
        case ConfigKind::Save: {
            std::vector<SaveClause> clauses;
            if (!cfg_parse_save_schedule(input.p, input.n, clauses)) return false;
            out = cfg_save_schedule_string(clauses);
            return true;
        }
    }
    return false;
}

bool collect_config_updates(Op& op,
                            std::vector<std::pair<ConfigValue*, std::string>>& updates) {
    if (op.argc() < 4) {
        command_reply_subcommand_wrong_args(op, "CONFIG", "set");
        return false;
    }
    if ((op.argc() & 1u) != 0) { reply_syntax(op.sink()); return false; }
    ClientOutputBufferLimits obuf_scratch = g_client_obuf_limits;
    for (uint32_t i = 2; i < op.argc(); i += 2) {
        ConfigValue* item = find_config(op.arg(i));
        if (!item) {
            std::string msg = "ERR Unknown option or number of arguments for CONFIG SET - '";
            msg.append(op.arg(i).p, op.arg(i).n); msg.push_back('\'');
            reply_err(op.sink(), msg.c_str()); return false;
        }
        if (!std::strcmp(item->name, "aof-use-rdb-preamble")) {
            if (!eq_icase(op.arg(i + 1), "yes")) {
                reply_err(op.sink(), "ERR aof-use-rdb-preamble no is unsupported: the AOF base file is a TomoKV snapshot");
                return false;
            }
        }
        if (item->immutable || !std::strcmp(item->name, "dir") ||
            !std::strcmp(item->name, "dbfilename") || !std::strcmp(item->name, "tcp-backlog")) {
            reply_err(op.sink(), "ERR parameter is immutable at runtime"); return false;
        }
        std::string value;
        bool normalized = false;
        if (item->kind == ConfigKind::ClientOutputBufferLimit) {
            ClientOutputBufferLimits parsed;
            const char* error = nullptr;
            normalized = parse_client_output_buffer_limit_slice(
                op.arg(i + 1), obuf_scratch, parsed, error);
            if (normalized) {
                obuf_scratch = parsed;
                value = cfg_client_output_buffer_limit_string(parsed);
            }
        } else {
            normalized = normalize_config(*item, op.arg(i + 1), value);
        }
        if (!normalized) {
            std::string msg = "ERR Invalid argument '";
            msg.append(op.arg(i + 1).p, op.arg(i + 1).n);
            msg += "' for CONFIG SET '"; msg += item->name; msg.push_back('\'');
            reply_err(op.sink(), msg.c_str()); return false;
        }
        updates.emplace_back(item, std::move(value));
    }
    return true;
}

void snapshot_command(Op& op, bool blocking) {
    if (!g_server) { reply_err(op.sink(), "ERR snapshot subsystem is unavailable"); return; }
    const SnapshotIoContext context = snapshot_io_context();
    if (!context.thread || !context.ring) {
        reply_err(op.sink(), "ERR snapshot command has no IO owner");
        return;
    }
    std::string error;
    const SnapshotManager::StartResult result = g_server->snapshot().start(
        *g_server, *context.thread, *context.ring, blocking, error);
    if (result == SnapshotManager::StartResult::Busy) {
        reply_err(op.sink(), "ERR Background save already in progress");
    } else if (result == SnapshotManager::StartResult::Failed) {
        std::string message = "ERR ";
        message += error.empty() ? "snapshot failed" : error;
        reply_err(op.sink(), message.c_str());
    } else if (blocking) {
        reply_ok(op.sink());
    } else {
        reply_simple(op.sink(), "Background saving started");
    }
}

void cmd_save(Shard&, Op& op) { snapshot_command(op, true); }
void cmd_bgsave(Shard&, Op& op) { snapshot_command(op, false); }
void cmd_bgrewriteaof(Shard&, Op& op) {
    if (!g_server || !g_server->aof().recording()) {
        reply_err(op.sink(), "ERR Background append only file rewriting not scheduled");
        return;
    }
    if (!g_server->aof().request_rewrite()) {
        reply_err(op.sink(), "ERR Background append only file rewriting already in progress");
        return;
    }
    reply_simple(op.sink(), "Background append only file rewriting started");
}
void cmd_lastsave(Shard&, Op& op) {
    reply_int(op.sink(), g_server ? static_cast<long long>(g_server->snapshot().last_save_time()) : 0);
}

void cmd_ping(Shard&, Op& op) {
    if (op.argc() == 2) reply_bulk(op.sink(), op.arg(1));
    else reply_pong(op.sink());
}

void cmd_echo(Shard&, Op& op) { reply_bulk(op.sink(), op.arg(1)); }

void cmd_auth(Shard&, Op& op) {
    const Slice password = op.argc() == 2 ? op.arg(1) : op.arg(2);
    if (op.argc() == 2 && acl_default_nopass()) {
        reply_err(op.sink(), "ERR AUTH <password> called without any password configured for the default user. Are you sure your configuration is correct?");
        return;
    }
    const Slice username = op.argc() == 2 ? Slice("default", 7) : op.arg(1);
    if (!authenticate_acl_user(username, password)) {
        if (g_server) g_server->note_auth_failure();
        note_acl_auth_denial(op, username);
        reply_err(op.sink(), "WRONGPASS invalid username-password pair or user is disabled.");
        return;
    }
    if (g_client) g_client->set_authenticated(true);
    reply_ok(op.sink());
}

void cmd_hello(Shard&, Op& op) {
    uint32_t next = 1;
    const bool has_version = op.argc() >= 2;
    uint64_t version = g_client && g_client->resp3() ? 3 : 2;
    if (has_version) {
        if (!parse_u64(op.arg(next++), version)) {
            reply_err(op.sink(), "ERR Protocol version is not an integer or out of range");
            return;
        }
        if (version < 2 || version > 3) {
            reply_err(op.sink(), "NOPROTO unsupported protocol version");
            return;
        }
    }

    Slice username, password, client_name;
    bool has_auth = false, has_name = false;
    while (next < op.argc()) {
        const Slice option = op.arg(next);
        if (eq_icase(option, "auth") && next + 2 < op.argc()) {
            username = op.arg(next + 1);
            password = op.arg(next + 2);
            has_auth = true;
            next += 3;
        } else if (eq_icase(option, "setname") && next + 1 < op.argc()) {
            client_name = op.arg(next + 1);
            if (!valid_client_text(client_name)) {
                reply_err(op.sink(), "ERR Client names cannot contain spaces, newlines or special characters.");
                return;
            }
            has_name = true;
            next += 2;
        } else {
            std::string message = "ERR Syntax error in HELLO option '";
            message.append(option.p, option.n);
            message.push_back('\'');
            reply_err(op.sink(), message.c_str());
            return;
        }
    }
    if (has_auth) {
        if (!authenticate_acl_user(username, password)) {
            if (g_server) g_server->note_auth_failure();
            note_acl_auth_denial(op, username);
            reply_err(op.sink(), "WRONGPASS invalid username-password pair or user is disabled.");
            return;
        }
        if (g_client) g_client->set_authenticated(true);
    }
    if (g_server && g_server->requirepass_enabled() &&
        (!g_client || !g_client->authenticated())) {
        reply_err(op.sink(),
                  "NOAUTH HELLO must be called with the client already authenticated, otherwise the HELLO <proto> AUTH <user> <pass> option can be used to authenticate the client and select the RESP protocol version at the same time");
        return;
    }
    if (has_name) command_client_set_name(g_client, client_name);
    if (has_version && g_client) g_client->set_resp3(version == 3);
    const bool resp3 = version == 3;
    auto sink = op.sink();
    reply_map_header(sink, 7, resp3);
    reply_bulk(sink, Slice("server", 6)); reply_bulk(sink, Slice("redis", 5));
    reply_bulk(sink, Slice("version", 7)); reply_bulk(sink, Slice(kVersion, std::strlen(kVersion)));
    reply_bulk(sink, Slice("proto", 5)); reply_int(sink, static_cast<long long>(version));
    reply_bulk(sink, Slice("id", 2)); reply_int(sink, g_client ? static_cast<long long>(g_client->id()) : 0);
    reply_bulk(sink, Slice("mode", 4)); reply_bulk(sink, Slice("standalone", 10));
    reply_bulk(sink, Slice("role", 4)); reply_bulk(sink, Slice("master", 6));
    reply_bulk(sink, Slice("modules", 7)); reply_array_header(sink, 0);
}

void cmd_select(Shard&, Op& op) {
    uint64_t db = 0;
    if (!parse_u64(op.arg(1), db) || db != 0) {
        reply_err(op.sink(), "ERR this server supports a single keyspace; only SELECT 0 is valid");
        return;
    }
    if (g_client) g_client->session().db_index = 0;
    reply_ok(op.sink());
}

void cmd_reset(Shard&, Op& op) {
    if (g_client) g_client->session().db_index = 0;
    if (g_client) {
        g_client->set_resp3(false);
        g_client->set_acl_user_idx(kAclDefaultUser);
        g_client->set_authenticated(!g_server || !g_server->requirepass_enabled());
    }
    command_client_reset_meta(g_client);
    reply_simple(op.sink(), "RESET");
}

void cmd_debug_impl(Shard&, Op& op) {
    const Slice subcommand = op.arg(1);
    if (eq_icase(subcommand, "lbsignals") && op.argc() == 2) {
        // Raw monotonic dump; windowing is the reader's job (two calls, subtract). See lbsignals.h.
        if (!g_server) { reply_err(op.sink(), "ERR no server context"); return; }
        std::string out;
        lbsignals_format(lbsignals_capture(*g_server), out);
        reply_verbatim(op.sink(), Slice(out.data(), out.size()), "txt", op.resp3());
        return;
    }
    if (eq_icase(subcommand, "sleep") && op.argc() == 3) {
        // DEBUG SLEEP is redis's one unguarded strtod: it validates nothing, so a word sleeps
        // for zero seconds and answers OK. The upper bound is ours -- redis will happily sleep a
        // year, and this server is not going to.
        double seconds = 0;
        if (!parse_double_lenient(op.arg(2), seconds)) seconds = 0;
        if (!std::isfinite(seconds) || seconds < 0.0) seconds = 0;
        if (seconds > 86400.0) {
            reply_err(op.sink(), "ERR value is not a valid float");
            return;
        }
        const int64_t nanoseconds = static_cast<int64_t>(seconds * 1000000000.0);
        timespec remaining{nanoseconds / 1000000000ll, nanoseconds % 1000000000ll};
        while (::nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {}
        reply_ok(op.sink());
        return;
    }
    // Window widener for the cross-shard scan-ordering regression test. Holds a direct RENAME's
    // destination task for N extra owner passes AFTER its source hop is ready, which is exactly
    // the park a younger whole-owner walker used to run past. Production default is 0.
    if (eq_icase(subcommand, "atomic-direct-defer") && op.argc() == 3) {
        uint64_t passes = 0;
        if (!parse_u64(op.arg(2), passes) || passes > 1000000) {
            reply_err(op.sink(), "ERR value is not an integer or out of range");
            return;
        }
        if (!g_server) { reply_err(op.sink(), "ERR no server context"); return; }
        g_server->set_debug_atomic_direct_defer(static_cast<uint32_t>(passes));
        reply_ok(op.sink());
        return;
    }
    // GEOMETRY INJECTOR for the parse-barrier ownership regression. While armed, every blocking
    // dispatch pins a SECOND owner on its connection's parse barrier, so the blocking command's
    // retirement releases a barrier it does not solely own. That two-owner state is unreachable on
    // any production sequence (NOTES-BARRIER.md section 2) -- which is why it must be injected
    // rather than provoked, and why a battery that only replays real command sequences proves
    // nothing about this code. Production default is 0.
    //
    // Observable while armed: a frame pipelined BEHIND a blocking command stays unparsed after the
    // blocking reply retires, instead of being answered in the same flush pass. Clearing the latch
    // resumes it. Assert on barrier_releases_held in INFO STATS too -- if that did not move, the
    // hold never overlapped a retirement and the timing arm measured nothing.
    //
    // A LATCH, NOT A DURATION: a barred connection's io thread parks unbounded under io_uring, so
    // nothing would ever notice a clock expiring. Clear it from ANOTHER connection (the barred one
    // cannot be parsed, by construction) and then wake the io threads -- CLIENT LIST fans out to
    // every one of them and is the cheapest existing way to do it.
    if (eq_icase(subcommand, "barrier-hold") && op.argc() == 3) {
        uint64_t armed = 0;
        if (!parse_u64(op.arg(2), armed) || armed > 1) {
            reply_err(op.sink(), "ERR value is not an integer or out of range");
            return;
        }
        if (!g_server) { reply_err(op.sink(), "ERR no server context"); return; }
        g_server->set_debug_barrier_hold(static_cast<uint32_t>(armed));
        reply_ok(op.sink());
        return;
    }
    // Which owner a key routes to. The hash seed is drawn from the kernel at every boot, so a test
    // cannot know from the key name alone whether a two-key command is one owner's work or a real
    // cross-shard group -- and a cross-shard battery that silently ran same-owner proves nothing.
    // This is the geometry oracle those batteries gate on.
    if (eq_icase(subcommand, "shard") && op.argc() == 3) {
        if (!g_server) { reply_err(op.sink(), "ERR no server context"); return; }
        reply_int(op.sink(), g_server->router().shard_of(FlatStore::hash_key(op.arg(2))));
        return;
    }
    // Window widener for the torn-read regression. Holds a cross-shard group between drawing its
    // commit ticket and storing that ticket into its shared epoch word -- the hole in which the
    // sequence already named a commit whose records still answered "undecided". Production 0.
    if (eq_icase(subcommand, "atomic-commit-delay") && op.argc() == 3) {
        uint64_t microseconds = 0;
        if (!parse_u64(op.arg(2), microseconds) || microseconds > 1000000) {
            reply_err(op.sink(), "ERR value is not an integer or out of range");
            return;
        }
        if (!g_server) { reply_err(op.sink(), "ERR no server context"); return; }
        g_server->set_debug_atomic_commit_delay(static_cast<uint32_t>(microseconds));
        reply_ok(op.sink());
        return;
    }
    // Window widener for the session-monotonicity regression. Holds a plain read on its owner
    // before it resolves, so foreign commits have time to land between the IO-side dispatch of a
    // pipelined read and its execution. Production 0.
    if (eq_icase(subcommand, "atomic-read-delay") && op.argc() == 3) {
        uint64_t microseconds = 0;
        if (!parse_u64(op.arg(2), microseconds) || microseconds > 1000000) {
            reply_err(op.sink(), "ERR value is not an integer or out of range");
            return;
        }
        if (!g_server) { reply_err(op.sink(), "ERR no server context"); return; }
        g_server->set_debug_atomic_read_delay(static_cast<uint32_t>(microseconds));
        reply_ok(op.sink());
        return;
    }
    // Window widener for the EXEC fan-out regression. Parks every fragment of a cross-shard READ
    // except the one on its lead shard for N microseconds, so a transaction can commit strictly
    // between the lead fragment's answer and the rest. That straddle is the shape of the defect:
    // a reader with no pinned cut reports one key from after the transaction and the others from
    // before it. Production 0.
    if (eq_icase(subcommand, "atomic-fanout-defer") && op.argc() == 3) {
        uint64_t microseconds = 0;
        if (!parse_u64(op.arg(2), microseconds) || microseconds > 10000000) {
            reply_err(op.sink(), "ERR value is not an integer or out of range");
            return;
        }
        if (!g_server) { reply_err(op.sink(), "ERR no server context"); return; }
        g_server->set_debug_atomic_fanout_defer(static_cast<uint32_t>(microseconds));
        reply_ok(op.sink());
        return;
    }
    // Window widener for the cross-owner script reservation regression. Parks every declared key's
    // GATHER task except the coordinator's own for N microseconds AFTER the reservation sub-wave
    // has armed every key and the cut has been chosen. A plain write landing in that park must be
    // forced through MVCC by the reservation; if it is not, the activation reads one key from
    // before the write and another from after it and never notices. Production 0.
    if (eq_icase(subcommand, "script-stage-defer") && op.argc() == 3) {
        uint64_t microseconds = 0;
        if (!parse_u64(op.arg(2), microseconds) || microseconds > 10000000) {
            reply_err(op.sink(), "ERR value is not an integer or out of range");
            return;
        }
        if (!g_server) { reply_err(op.sink(), "ERR no server context"); return; }
        g_server->set_debug_script_stage_defer(static_cast<uint32_t>(microseconds));
        reply_ok(op.sink());
        return;
    }
    if (eq_icase(subcommand, "set-active-expire") && op.argc() == 3) {
        if (!(op.arg(2) == Slice("0", 1) || op.arg(2) == Slice("1", 1))) {
            reply_err(op.sink(), "ERR value is not an integer or out of range");
            return;
        }
        if (g_server) g_server->set_active_expire_enabled(op.arg(2).p[0] == '1');
        reply_ok(op.sink());
        return;
    }
    if (eq_icase(subcommand, "aof-stop-after-group-fragments") && op.argc() == 3) {
        uint64_t count = 0;
        if (!parse_u64(op.arg(2), count) || count == 0) {
            reply_err(op.sink(), "ERR value is not an integer or out of range");
            return;
        }
        if (!g_server || !g_server->aof().recording()) {
            reply_err(op.sink(), "ERR appendonly is disabled");
            return;
        }
        g_server->aof().debug_stop_after_group_fragments(count);
        reply_ok(op.sink());
        return;
    }
    if (eq_icase(subcommand, "aof-rewrite-pause") && op.argc() == 3) {
        AofRewriteDebugStage stage = AofRewriteDebugStage::None;
        if (eq_icase(op.arg(2), "before-mark")) stage = AofRewriteDebugStage::BeforeMark;
        else if (eq_icase(op.arg(2), "before-manifest"))
            stage = AofRewriteDebugStage::BeforeManifest;
        else if (eq_icase(op.arg(2), "after-manifest"))
            stage = AofRewriteDebugStage::AfterManifest;
        else if (!eq_icase(op.arg(2), "off")) {
            reply_syntax(op.sink());
            return;
        }
        if (!g_server || !g_server->aof().recording()) {
            reply_err(op.sink(), "ERR appendonly is disabled");
            return;
        }
        g_server->aof().debug_rewrite_pause(stage);
        reply_ok(op.sink());
        return;
    }
    if (eq_icase(subcommand, "loadaof") && op.argc() == 2) {
        reply_err(op.sink(), "ERR internal DEBUG LOADAOF routing error");
        return;
    }
    if (eq_icase(subcommand, "borrowcount") && op.argc() == 2) {
        reply_err(op.sink(), "ERR internal DEBUG BORROWCOUNT routing error");
        return;
    }
    if (eq_icase(subcommand, "reload") && op.argc() == 2) {
        reply_err(op.sink(), "ERR internal DEBUG RELOAD routing error");
        return;
    }
    reply_err(op.sink(), "ERR unknown subcommand or wrong number of arguments for 'debug' command");
}

void cmd_quit(Shard&, Op& op) {
    reply_ok(op.sink());
    if (g_client) g_client->mark_closing();
}

void cmd_pubsub_only(Shard&, Op& op) {
    reply_err(op.sink(), "ERR internal pubsub routing error");
}

// MONITOR never reaches a shard: the io dispatcher routes it into climon_start_client_command.
// This row exists so registry validation, ACL categories and COMMAND DOCS see a real handler.
void cmd_monitor(Shard&, Op& op) {
    reply_err(op.sink(), "ERR MONITOR is unavailable in this execution context");
}

void cmd_client(Shard&, Op& op) {
    const Slice sub = op.arg(1);
    // Redis answers a wrong-arity subcommand with a per-subcommand message, and an unrecognised
    // one with the CLIENT HELP pointer -- never with a bare syntax error.
    static constexpr struct { const char* name; const char* lower; int32_t min; int32_t max; }
        kArity[] = {
            {"ID", "id", 2, 2}, {"GETNAME", "getname", 2, 2}, {"SETNAME", "setname", 3, 3},
            {"SETINFO", "setinfo", 4, 4}, {"INFO", "info", 2, 2},
            {"NO-EVICT", "no-evict", 3, 3},
        };
    for (const auto& entry : kArity) {
        if (!eq_icase(sub, entry.name)) continue;
        if (static_cast<int32_t>(op.argc()) < entry.min ||
            static_cast<int32_t>(op.argc()) > entry.max) {
            climon_wrong_args(op, entry.lower);
            return;
        }
        break;
    }
    if (eq_icase(sub, "ID") && op.argc() == 2) {
        reply_int(op.sink(), g_client ? static_cast<long long>(g_client->id()) : 0);
    } else if (eq_icase(sub, "SETNAME") && op.argc() == 3) {
        if (!valid_client_text(op.arg(2))) {
            reply_err(op.sink(), "ERR Client names cannot contain spaces, newlines or special characters.");
            return;
        }
        command_client_set_name(g_client, op.arg(2));
        reply_ok(op.sink());
    } else if (eq_icase(sub, "GETNAME") && op.argc() == 2) {
        const std::string name = command_client_name(g_client);
        if (name.empty()) reply_null(op.sink(), op.resp3());
        else reply_bulk(op.sink(), Slice(name.data(), name.size()));
    } else if (eq_icase(sub, "SETINFO") && op.argc() == 4) {
        if (!eq_icase(op.arg(2), "LIB-NAME") && !eq_icase(op.arg(2), "LIB-VER")) {
            std::string error = "ERR Unrecognized option '";
            error.append(op.arg(2).p, op.arg(2).n);
            error.push_back('\'');
            reply_err(op.sink(), error.c_str());
            return;
        }
        if (!valid_client_text(op.arg(3)) && op.arg(3).n != 0) {
            std::string error = "ERR ";
            error.append(op.arg(2).p, op.arg(2).n);
            error += " cannot contain spaces, newlines or special characters.";
            reply_err(op.sink(), error.c_str());
            return;
        }
        if (!command_client_set_info(g_client, op.arg(2), op.arg(3))) {
            reply_err(op.sink(), "ERR client metadata unavailable");
            return;
        }
        reply_ok(op.sink());
    } else if (eq_icase(sub, "INFO") && op.argc() == 2) {
        const std::string body = command_client_info_line(
            *g_client, now_ns() / 1000000ull);
        reply_verbatim(op.sink(), Slice(body.data(), body.size()), "txt", op.resp3());
    } else if (eq_icase(sub, "LIST") || eq_icase(sub, "KILL")) {
        reply_err(op.sink(), "ERR CLIENT scatter is unavailable in this execution context");
    } else if (eq_icase(sub, "NO-EVICT") && op.argc() == 3 &&
               (eq_icase(op.arg(2), "ON") || eq_icase(op.arg(2), "OFF"))) {
        command_client_set_no_evict(g_client, eq_icase(op.arg(2), "ON"));
        reply_ok(op.sink());
    } else if (eq_icase(sub, "NO-EVICT")) {
        reply_syntax(op.sink());
    } else {
        std::string error = "ERR Unknown subcommand or wrong number of arguments for '";
        error.append(op.arg(1).p, op.arg(1).n);
        error += "'. Try CLIENT HELP.";
        reply_err(op.sink(), error.c_str());
    }
}

const char* command_group(const CommandMetadata& metadata) {
    const uint64_t categories = command_metadata_categories(metadata);
    if (categories & (uint64_t{1} << 13)) return "server";
    if (categories & (uint64_t{1} << 18)) return "connection";
    return "generic";
}

void reply_command_docs(Op::Sink& sink, const CommandMetadata& metadata, bool resp3) {
    // A RESP2 map is a flat array. These four fields are enough for redis-cli's live help parser.
    reply_map_header(sink, 4, resp3);
    reply_bulk(sink, Slice("summary", 7));
    const Slice name = command_metadata_name(metadata);
    std::string summary = "tomokv compatible ";
    summary.append(name.p, name.n);
    summary += " command";
    reply_bulk(sink, Slice(summary.data(), summary.size()));
    reply_bulk(sink, Slice("since", 5)); reply_bulk(sink, Slice("0.1.0", 5));
    reply_bulk(sink, Slice("group", 5));
    const char* group = command_group(metadata);
    reply_bulk(sink, Slice(group, std::strlen(group)));
    reply_bulk(sink, Slice("complexity", 10));
    const char* complexity = "O(1) or proportional to returned work";
    reply_bulk(sink, Slice(complexity, std::strlen(complexity)));
}

void cmd_command(Shard&, Op& op) {
    auto sink = op.sink();
    if (op.argc() == 1) {
        const uint32_t count = command_registry_size();
        reply_array_header(sink, count);
        for (uint32_t i = 0; i < count; i++)
            command_metadata_reply_info(op, command_metadata_for(*command_registry_at(i)));
        return;
    }
    if (eq_icase(op.arg(1), "COUNT") && op.argc() == 2) {
        reply_int(sink, command_registry_size());
        return;
    }
    if (eq_icase(op.arg(1), "INFO")) {
        if (op.argc() == 2) {
            const uint32_t count = command_registry_size();
            reply_array_header(sink, count);
            for (uint32_t i = 0; i < count; i++)
                command_metadata_reply_info(op, command_metadata_for(*command_registry_at(i)));
            return;
        }
        reply_array_header(sink, op.argc() - 2);
        for (uint32_t i = 2; i < op.argc(); i++)
            command_metadata_reply_info(op, command_metadata_lookup(op.arg(i)));
        return;
    }
    if (eq_icase(op.arg(1), "DOCS")) {
        std::vector<const CommandMetadata*> specs;
        if (op.argc() == 2) {
            for (uint32_t i = 0; i < command_registry_size(); i++)
                specs.push_back(command_metadata_for(*command_registry_at(i)));
        } else {
            for (uint32_t i = 2; i < op.argc(); i++)
                if (const CommandMetadata* spec = command_metadata_lookup(op.arg(i)))
                    specs.push_back(spec);
        }
        reply_map_header(sink, specs.size(), op.resp3());
        for (const CommandMetadata* spec : specs) {
            reply_bulk(sink, command_metadata_name(*spec));
            reply_command_docs(sink, *spec, op.resp3());
        }
        return;
    }
    // LIST / GETKEYS / GETKEYSANDFLAGS / HELP live in the server-tail feature file.
    if (server_tail_command_subcommand(op)) return;
    reply_err(sink, "ERR unknown subcommand or wrong number of arguments for 'command'. Try COMMAND HELP.");
}

static constexpr SubcommandArity kConfigSubcommands[] = {
    {"get",       3, -1},
    {"set",       4, -1},
    {"rewrite",   2,  2},
    {"resetstat", 2,  2},
    {"help",      2,  2},
};

void cmd_config(Shard& sh, Op& op) {
    if (!command_validate_subcommand(op, "CONFIG", kConfigSubcommands,
                                     sizeof(kConfigSubcommands) /
                                         sizeof(kConfigSubcommands[0]))) return;
    if (eq_icase(op.arg(1), "GET") && op.argc() >= 3) {
        std::vector<std::pair<std::string, std::string>> matches;
        {
            std::lock_guard<std::mutex> lock(g_config_mu);
            for (const ConfigValue& item : g_config) {
                Slice name(item.name, std::strlen(item.name));
                bool matched = false;
                for (uint32_t i = 2; i < op.argc() && !matched; i++)
                    matched = command_glob_match(op.arg(i), name, true);
                if (matched) matches.emplace_back(item.name, item.value);
            }
        }
        auto sink = op.sink();
        reply_map_header(sink, matches.size(), op.resp3());
        for (const auto& item : matches) {
            reply_bulk(sink, Slice(item.first.data(), item.first.size()));
            reply_bulk(sink, Slice(item.second.data(), item.second.size()));
        }
        return;
    }
    if (eq_icase(op.arg(1), "SET")) {
        std::vector<std::pair<ConfigValue*, std::string>> updates;
        {
            std::lock_guard<std::mutex> lock(g_config_mu);
            // IO validated before fan-out, so failure here can only mean internal table corruption.
            if (!collect_config_updates(op, updates)) return;
            if (sh.id() == 0) {
                for (auto& update : updates) {
                    update.first->value = update.second;
                    if (!std::strcmp(update.first->name, "client-output-buffer-limit")) {
                        ClientOutputBufferLimits parsed;
                        const ClientOutputBufferLimits defaults;
                        const char* error = nullptr;
                        const Slice text(update.second.data(), update.second.size());
                        if (parse_client_output_buffer_limit_slice(text, defaults, parsed, error))
                            g_client_obuf_limits = parsed;
                    }
                }
            }
        }

        // CONFIG is already a barrier over every shard owner. Apply the notification sink on
        // each owner task before that barrier replies, then publish the same mask through the
        // live seqlock for subsequent per-pass snapshots. This closes the SET-then-command seam.
        for (const auto& update : updates) {
            if (std::strcmp(update.first->name, "notify-keyspace-events")) continue;
            uint32_t flags = 0;
            if (!parse_notify_flags(
                    Slice(update.second.data(), static_cast<uint32_t>(update.second.size())),
                    flags)) std::abort();
            // Preserve the tracking arm across a notify-config barrier: the CONFIG task publishes
            // the configured classes, the tracking bit is owned by CLIENT TRACKING.
            const bool tracking =
                g_server && (g_server->climon_armed() & Server::kClimonTracking) != 0;
            const bool save_armed = g_server && g_server->save_schedule_armed();
            sh.set_notify_mask(flags |
                               (tracking ? NOTIFY_TRACKING : 0u) |
                               (save_armed ? NOTIFY_SAVE : 0u));
        }

        // Eviction config is process-global (odd/even snapshot read by owners each pass); publish
        // it once from shard 0's task rather than per shard.
        if (sh.id() == 0 && g_server) {
            for (const auto& update : updates) {
                if (!std::strcmp(update.first->name, "save")) {
                    std::vector<SaveClause> clauses;
                    if (!cfg_parse_save_schedule(update.second.data(), update.second.size(),
                                                 clauses)) std::abort();
                    g_server->set_save_schedule(clauses);
                } else if (!std::strcmp(update.first->name, "proto-max-bulk-len")) {
                    uint64_t value = 0;
                    if (!parse_u64(Slice(update.second.data(), update.second.size()), value))
                        std::abort();
                    g_server->set_proto_max_bulk_len(value);
                }
            }
            LiveConfigSnapshot desired = g_server->live_config_snapshot();
            bool set_memory = false, set_policy = false, set_samples = false;
            for (const auto& update : updates) {
                const Slice text(update.second.data(),
                                 static_cast<uint32_t>(update.second.size()));
                if (!std::strcmp(update.first->name, "maxmemory")) {
                    parse_u64(text, desired.maxmemory); set_memory = true;
                } else if (!std::strcmp(update.first->name, "maxmemory-policy")) {
                    parse_maxmemory_policy(text.sv(), desired.policy); set_policy = true;
                } else if (!std::strcmp(update.first->name, "maxmemory-samples")) {
                    uint64_t samples = 0;
                    parse_u64(text, samples);
                    desired.samples = static_cast<uint32_t>(samples); set_samples = true;
                }
            }
            if (set_memory || set_policy || set_samples)
                g_server->set_maxmemory_config(desired.maxmemory, desired.policy, desired.samples,
                                               set_memory, set_policy, set_samples);
            uint32_t auto_percentage = g_server->aof().auto_rewrite_percentage();
            uint64_t auto_min_size = g_server->aof().auto_rewrite_min_size();
            bool set_auto_rewrite = false;
            for (const auto& update : updates) {
                if (std::strcmp(update.first->name, "notify-keyspace-events")) continue;
                uint32_t flags = 0;
                if (!parse_notify_flags(
                        Slice(update.second.data(), static_cast<uint32_t>(update.second.size())),
                        flags)) std::abort();
                g_server->set_notify_events(flags);
            }
            for (const auto& update : updates) {
                uint64_t value = 0;
                if (!std::strcmp(update.first->name, "requirepass")) {
                    auth_publish_requirepass(
                        *g_server, Slice(update.second.data(), update.second.size()));
                    continue;
                }
                if (!std::strcmp(update.first->name, "protected-mode")) {
                    g_server->set_protected_mode(update.second == "yes");
                    continue;
                }
                if (!std::strcmp(update.first->name, "acl-pubsub-default")) {
                    acl_set_pubsub_default(update.second == "allchannels");
                }
                if (!std::strcmp(update.first->name, "aof-timestamp-enabled")) {
                    g_server->aof().set_timestamp_enabled(update.second == "yes");
                    continue;
                }
                if (!std::strcmp(update.first->name, "appendfsync")) {
                    const AppendFsyncPolicy policy = update.second == "always"
                        ? AppendFsyncPolicy::Always
                        : update.second == "no" ? AppendFsyncPolicy::No
                                                 : AppendFsyncPolicy::Everysec;
                    g_server->aof().set_fsync_policy(policy);
                    continue;
                }
                if (!parse_u64(Slice(update.second.data(), update.second.size()), value)) continue;
                if (!std::strcmp(update.first->name, "auto-aof-rewrite-percentage")) {
                    auto_percentage = static_cast<uint32_t>(value);
                    set_auto_rewrite = true;
                } else if (!std::strcmp(update.first->name, "auto-aof-rewrite-min-size")) {
                    auto_min_size = value;
                    set_auto_rewrite = true;
                } else if (!std::strcmp(update.first->name, "atomic"))
                    g_server->set_atomic_enabled(value != 0);
                else if (!std::strcmp(update.first->name, "atomic-window"))
                    g_server->set_atomic_window(static_cast<uint32_t>(value));
                else if (!std::strcmp(update.first->name, "maxclients"))
                    g_server->set_maxclients(static_cast<uint32_t>(value));
                else if (!std::strcmp(update.first->name, "timeout"))
                    g_server->set_timeout(static_cast<uint32_t>(value));
                else if (!std::strcmp(update.first->name, "tcp-keepalive"))
                    g_server->set_tcp_keepalive(static_cast<uint32_t>(value));
                else if (!std::strcmp(update.first->name, "acllog-max-len"))
                    acl_set_log_max_len(value);
                else if (!std::strcmp(update.first->name, "slowlog-max-len"))
                    slowlog_set_max_len(value);
            }
            // Slow-log arming is published through the live seqlock so executors pick it up on
            // their next pass without any per-op load. Both knobs travel together because the
            // recorder is armed by either one.
            {
                int64_t slowlog_us = g_server->slowlog_log_slower_than();
                uint32_t latency_ms = g_server->latency_monitor_threshold();
                bool changed = false;
                for (const auto& update : updates) {
                    if (!std::strcmp(update.first->name, "slowlog-log-slower-than")) {
                        parse_i64_slice(Slice(update.second.data(), update.second.size()),
                                        slowlog_us);
                        changed = true;
                    } else if (!std::strcmp(update.first->name, "latency-monitor-threshold")) {
                        uint64_t parsed = 0;
                        if (parse_u64(Slice(update.second.data(), update.second.size()), parsed))
                            latency_ms = static_cast<uint32_t>(parsed);
                        changed = true;
                    }
                }
                if (changed) g_server->set_slowlog_config(slowlog_us, latency_ms);
            }
            if (set_auto_rewrite)
                g_server->aof().set_auto_rewrite_config(auto_percentage, auto_min_size);
            for (const auto& update : updates) {
                if (std::strcmp(update.first->name, "client-output-buffer-limit")) continue;
                ClientOutputBufferLimits parsed;
                const char* error = nullptr;
                const Slice text(update.second.data(), update.second.size());
                const ClientOutputBufferLimits defaults;
                if (parse_client_output_buffer_limit_slice(text, defaults, parsed, error)) {
                    g_server->set_client_output_buffer_limits(parsed);
                }
            }
        }

        TypeLimits limits = sh.type_limits();
        StreamLimits stream_limits = sh.stream_limits();
        for (const auto& update : updates) {
            uint64_t value = 0;
            if (!parse_u64(Slice(update.second.data(), update.second.size()), value)) continue;
            const uint32_t v = static_cast<uint32_t>(value);
            if (!std::strcmp(update.first->name, "zc-min")) sh.set_zc_min(v);
            else if (!std::strcmp(update.first->name, "hash-max-compact-entries")) limits.hash.max_entries = v;
            else if (!std::strcmp(update.first->name, "hash-max-compact-value")) limits.hash.max_value = v;
            else if (!std::strcmp(update.first->name, "list-max-compact-entries")) limits.list.max_entries = v;
            else if (!std::strcmp(update.first->name, "list-max-compact-value")) limits.list.max_value = v;
            else if (!std::strcmp(update.first->name, "set-max-compact-entries")) limits.set.max_entries = v;
            else if (!std::strcmp(update.first->name, "set-max-compact-value")) limits.set.max_value = v;
            else if (!std::strcmp(update.first->name, "zset-max-compact-entries")) limits.zset.max_entries = v;
            else if (!std::strcmp(update.first->name, "zset-max-compact-value")) limits.zset.max_value = v;
            else if (!std::strcmp(update.first->name, "stream-node-max-bytes")) stream_limits.node_max_bytes = v;
            else if (!std::strcmp(update.first->name, "stream-node-max-entries")) stream_limits.node_max_entries = v;
        }
        sh.set_type_limits(limits);
        sh.set_stream_limits(stream_limits);
        return;
    }
    // REWRITE / RESETSTAT / HELP live in the server-tail feature file. They are IO-local, like GET.
    if (server_tail_config_subcommand(op)) return;
}

// CONFIG RESETSTAT baseline. Every counter INFO reports here is a single-writer value owned by a
// shard owner or an IO loop; zeroing them from the calling thread would be a write race on the hot
// path. Instead RESETSTAT snapshots the aggregate and INFO subtracts it -- the reads are exactly
// the cross-thread reads INFO already performs, so nothing new is shared.
struct StatBaseline {
    uint64_t total_ops = 0;
    uint64_t sampled_ops = 0;
    uint64_t connections = 0;
    uint64_t hits = 0, misses = 0, expired = 0, evicted = 0;
    uint64_t rejected = 0, auth_failures = 0;
    uint64_t net_input_bytes = 0, net_output_bytes = 0;
    uint64_t object_bytes = 0;
    uint64_t acl_denied_cmd = 0, acl_denied_key = 0, acl_denied_channel = 0, acl_denied_auth = 0;
    std::vector<uint64_t> command_calls;
};

std::mutex g_stat_baseline_mu;
StatBaseline g_stat_baseline;

void collect_stat_totals(StatBaseline& out) {
    out = StatBaseline{};
    if (!g_server) return;
    for (uint32_t i = 0; i < g_server->nshards(); i++) {
        const Shard& sh = g_server->shard(static_cast<int32_t>(i));
        out.hits += sh.stats().hits;
        out.misses += sh.stats().misses;
        out.expired += sh.stats().expired;
        out.evicted += sh.published_evicted();
        out.object_bytes += sh.published_obj_bytes();
    }
    out.command_calls.assign(command_registry_size(), 0);
    for (uint32_t t = 0; t < g_server->nthreads(); t++) {
        ThreadCtx& thread = g_server->thread(t);
        for (uint32_t id = 0; id < command_registry_size(); id++) {
            const uint64_t calls = thread.command_calls(id);
            out.command_calls[id] += calls;
            out.total_ops += calls;
            if (std::strcmp(command_registry_at(id)->name, "INFO")) out.sampled_ops += calls;
        }
        const LoopSignals& sig = thread.sig();
        out.connections += sig.accepts;
        out.acl_denied_cmd += sig.acl_access_denied_cmd;
        out.acl_denied_key += sig.acl_access_denied_key;
        out.acl_denied_channel += sig.acl_access_denied_channel;
        out.acl_denied_auth += sig.acl_access_denied_auth;
        out.net_input_bytes += sig.net_input_bytes;
        out.net_output_bytes += sig.net_output_bytes;
    }
    out.rejected = g_server->rejected_conns() + g_server->rejected_connections();
    out.auth_failures = g_server->auth_failures();
}

// Saturating: a baseline can only ever be <= the live counter, but a counter that wrapped or a
// shard that was added after the reset must not underflow into a nonsense INFO value.
inline uint64_t minus_baseline(uint64_t live, uint64_t base) {
    return live >= base ? live - base : 0;
}

bool info_section(Op& op, const char* wanted) {
    // EVERYTHING is the reference's alias for ALL plus module-generated sections. We load no
    // modules, so the two are identical here -- but omitting it made `INFO everything` match no
    // section at all and return an EMPTY reply, where the reference returns every section.
    return op.argc() == 1 || eq_icase(op.arg(1), "ALL") || eq_icase(op.arg(1), "DEFAULT") ||
           eq_icase(op.arg(1), "EVERYTHING") || eq_icase(op.arg(1), wanted);
}

void cmd_info(Shard&, Op& op) {
    std::string body;
    uint64_t keys = 0, expires = 0, obj_bytes = 0, hits = 0, misses = 0, expired = 0,
             evicted = 0, keyspace_rehashes = 0;
    uint64_t total_ops = 0, sampled_ops = 0, connections = 0, rejected = 0;
    uint64_t net_input_bytes = 0, net_output_bytes = 0, auth_failures = 0;
    uint64_t sends_submitted = 0, short_writes = 0, bytes_sent = 0,
             peer_aborts = 0, send_errors = 0, zc_sends = 0, zc_releases = 0;
    uint64_t acl_denied_cmd = 0, acl_denied_key = 0, acl_denied_channel = 0,
             acl_denied_auth = 0;
    uint64_t atomic_predecessor_reads = 0, atomic_chain_max = 0,
             atomic_promotions = 0, atomic_records_freed = 0,
             atomic_entries = 0, atomic_pending_entries = 0,
             atomic_cleanup_fast = 0, atomic_cleanup_slow = 0,
             atomic_localfast = 0, atomic_scan_holds = 0, blocking_waiters = 0,
             atomic_gauge_underflows = 0, atomic_exec_order_holds = 0,
             watch_reservation_waits = 0, watch_reservation_coexist = 0,
             watch_reservation_precommit_aborts = 0;
    uint64_t hash_field_expires = 0, expired_hash_fields = 0;
    uint64_t plain_accepts = 0, tls_accepts = 0, tls_handshakes_started = 0,
             tls_handshakes_completed = 0, tls_handshakes_failed = 0,
             tls_connections_freed = 0, tls_want_read = 0, tls_want_write = 0,
             tls_ciphertext_input = 0, tls_plaintext_input = 0,
             tls_ciphertext_output = 0, tls_plaintext_output = 0,
             tls_zc_suppressed = 0, tls_ktls_active = 0, tls_ktls_fallback = 0;
    // --net-io epoll only. Both stay zero on the io_uring engine, which is what makes them usable
    // as a live FIRED-MECHANISM proof: a test that claims to be running on epoll and sees
    // net_io_epoll_events at 0 is not running on epoll, and its other assertions prove nothing.
    uint64_t epoll_events = 0, epoll_recvs = 0;
    if (g_server) {
        for (uint32_t i = 0; i < g_server->nshards(); i++) {
            const Shard& sh = g_server->shard(static_cast<int32_t>(i));
            keys += sh.published_size(); expires += sh.published_expires();
            hash_field_expires += sh.store().field_expire_count();
            expired_hash_fields += sh.store().field_expired();
            obj_bytes += sh.published_obj_bytes();
            hits += sh.stats().hits; misses += sh.stats().misses; expired += sh.stats().expired;
            evicted += sh.published_evicted();
            keyspace_rehashes += sh.stats().rehashes;
            atomic_predecessor_reads += sh.stats().atomic_predecessor_reads;
            atomic_chain_max = std::max(atomic_chain_max, sh.stats().atomic_chain_max);
            atomic_promotions += sh.stats().atomic_promotions;
            atomic_records_freed += sh.stats().atomic_records_freed;
            atomic_entries += sh.stats().atomic_entries;
            atomic_gauge_underflows += sh.stats().atomic_gauge_underflows;
            atomic_exec_order_holds += sh.stats().atomic_exec_order_holds;
            watch_reservation_waits += sh.stats().watch_reservation_waits;
            watch_reservation_coexist += sh.stats().watch_reservation_coexist;
            watch_reservation_precommit_aborts +=
                sh.stats().watch_reservation_precommit_aborts;
            atomic_pending_entries += sh.store().atomic_pending_entries();
            atomic_cleanup_fast += sh.store().atomic_cleanup_fast();
            atomic_cleanup_slow += sh.store().atomic_cleanup_slow();
            blocking_waiters += sh.blocking_waiters();
        }
        for (uint32_t t = 0; t < g_server->nthreads(); t++) {
            for (uint32_t id = 0; id < command_registry_size(); id++) {
                const uint64_t calls = g_server->thread(t).command_calls(id);
                total_ops += calls;
                if (!op.spec || id != op.spec->id) sampled_ops += calls;
            }
            connections += g_server->thread(t).sig().accepts;
            atomic_localfast += g_server->thread(t).atomic_localfast();
            atomic_scan_holds += g_server->thread(t).atomic_scan_holds();
            acl_denied_cmd += g_server->thread(t).sig().acl_access_denied_cmd;
            acl_denied_key += g_server->thread(t).sig().acl_access_denied_key;
            acl_denied_channel += g_server->thread(t).sig().acl_access_denied_channel;
            acl_denied_auth += g_server->thread(t).sig().acl_access_denied_auth;
            const LoopSignals& sig = g_server->thread(t).sig();
            plain_accepts += sig.plain_accepts;
            tls_accepts += sig.tls_accepts;
            tls_handshakes_started += sig.tls_handshakes_started;
            tls_handshakes_completed += sig.tls_handshakes_completed;
            tls_handshakes_failed += sig.tls_handshakes_failed;
            tls_connections_freed += sig.tls_connections_freed;
            tls_want_read += sig.tls_want_read;
            tls_want_write += sig.tls_want_write;
            tls_ciphertext_input += sig.tls_ciphertext_input_bytes;
            tls_plaintext_input += sig.tls_plaintext_input_bytes;
            tls_ciphertext_output += sig.tls_ciphertext_output_bytes;
            tls_plaintext_output += sig.tls_plaintext_output_bytes;
            tls_zc_suppressed += sig.tls_zc_suppressed;
            tls_ktls_active += sig.tls_ktls_active;
            tls_ktls_fallback += sig.tls_ktls_fallback;
            epoll_events += sig.epoll_events;
            epoll_recvs += sig.epoll_recvs;
            net_input_bytes += sig.net_input_bytes;
            net_output_bytes += sig.net_output_bytes;
            if (const WbEngine* wb = g_server->thread(t).wb_engine()) {
                const WbEngine::Stats& stats = wb->stats();
                sends_submitted += stats.sends_submitted;
                short_writes += stats.short_writes;
                bytes_sent += stats.bytes_sent;
                peer_aborts += stats.peer_aborts;
                send_errors += stats.send_errors;
                zc_sends += stats.zc_sends;
                zc_releases += stats.zc_releases;
            }
        }
        // Redis counts BOTH accept-time reject classes in rejected_connections: maxclients
        // (networking.c:1355) and protected-mode denials (networking.c:1306).
        rejected = g_server->rejected_conns() + g_server->rejected_connections();
        auth_failures = g_server->auth_failures();
    }
    // Apply the CONFIG RESETSTAT baseline to exactly the counters redis's RESETSTAT zeroes.
    StatBaseline baseline;
    {
        std::lock_guard<std::mutex> lock(g_stat_baseline_mu);
        baseline = g_stat_baseline;
    }
    total_ops = minus_baseline(total_ops, baseline.total_ops);
    connections = minus_baseline(connections, baseline.connections);
    hits = minus_baseline(hits, baseline.hits);
    misses = minus_baseline(misses, baseline.misses);
    expired = minus_baseline(expired, baseline.expired);
    evicted = minus_baseline(evicted, baseline.evicted);
    rejected = minus_baseline(rejected, baseline.rejected);
    net_input_bytes = minus_baseline(net_input_bytes, baseline.net_input_bytes);
    net_output_bytes = minus_baseline(net_output_bytes, baseline.net_output_bytes);
    auth_failures = minus_baseline(auth_failures, baseline.auth_failures);
    acl_denied_cmd = minus_baseline(acl_denied_cmd, baseline.acl_denied_cmd);
    acl_denied_key = minus_baseline(acl_denied_key, baseline.acl_denied_key);
    acl_denied_channel = minus_baseline(acl_denied_channel, baseline.acl_denied_channel);
    acl_denied_auth = minus_baseline(acl_denied_auth, baseline.acl_denied_auth);
    const uint64_t connected = g_server ? g_server->live_clients() : 0;

    if (info_section(op, "SERVER")) {
        const uint64_t uptime = g_started_monotonic_ns ? (now_ns() - g_started_monotonic_ns) / 1000000000ull : 0;
        // process_id and tcp_port are plain facts about this process, not telemetry that could be
        // stale -- and tooling depends on them. The NIC bench harness identifies the server it just
        // booted by reading process_id out of INFO, so its absence made every NIC cell fail with an
        // opaque "boot/cell FAIL" long before any measurement was taken.
        appendf(body, "# Server\r\nredis_version:%s\r\ntomokv_version:%s\r\nredis_mode:standalone\r\n"
                      "arch_bits:%zu\r\nmultiplexing_api:io_uring\r\nprocess_id:%lld\r\n"
                      "tcp_port:%u\r\nuptime_in_seconds:%llu\r\nuptime_in_days:%llu\r\n",
                kVersion, kVersion, sizeof(void*) * 8,
                static_cast<long long>(::getpid()),
                static_cast<unsigned>(g_server ? g_server->cfg().port : 0),
                static_cast<unsigned long long>(uptime),
                static_cast<unsigned long long>(uptime / 86400));
    }
    if (info_section(op, "CLIENTS")) {
        appendf(body, "# Clients\r\nconnected_clients:%llu\r\nblocked_clients:%llu\r\n"
                      "tracking_clients:%llu\r\nmonitor_clients:%llu\r\n",
                static_cast<unsigned long long>(connected),
                static_cast<unsigned long long>(g_server ? g_server->blocked_clients() : 0),
                static_cast<unsigned long long>(
                    g_server ? g_server->climon_tracking_clients() : 0),
                static_cast<unsigned long long>(g_server ? g_server->climon_monitors() : 0));
    }
    if (info_section(op, "MEMORY")) {
        const uint64_t object_peak = info_stats_observe_memory(obj_bytes);
        size_t allocated = 0, resident = 0;
#if defined(TOMO_JEMALLOC)
        uint64_t epoch = 1; size_t epoch_size = sizeof(epoch);
        mallctl("epoch", &epoch, &epoch_size, &epoch, sizeof(epoch));
        size_t sz = sizeof(allocated); mallctl("stats.allocated", &allocated, &sz, nullptr, 0);
        sz = sizeof(resident); mallctl("stats.resident", &resident, &sz, nullptr, 0);
#endif
        appendf(body, "# Memory\r\nused_memory:%llu\r\nused_memory_dataset:%llu\r\n"
                      "used_memory_rss:%llu\r\nused_memory_peak:%llu\r\n"
                      "mem_allocator:%s\r\nallocator_allocated:%llu\r\nallocator_resident:%llu\r\n",
                static_cast<unsigned long long>(obj_bytes), static_cast<unsigned long long>(obj_bytes),
                static_cast<unsigned long long>(resident), static_cast<unsigned long long>(object_peak),
                alloc_backend(), static_cast<unsigned long long>(allocated),
                static_cast<unsigned long long>(resident));
    }
    if (info_section(op, "PERSISTENCE")) {
        uint64_t preimages = 0;
        if (g_server)
            for (uint32_t i = 0; i < g_server->nshards(); i++)
                preimages += g_server->shard(static_cast<int32_t>(i)).store().snapshot_preimages();
        appendf(body,
                "# Persistence\r\nrdb_bgsave_in_progress:%u\r\nrdb_last_save_time:%lld\r\n"
                "rdb_changes_since_last_save:%llu\r\nrdb_scheduled_saves:%llu\r\n"
                "rdb_save_cron_checks:%llu\r\n"
                "snapshot_preimages:%llu\r\n"
                "snapshot_cuts_armed:%llu\r\nsnapshot_cuts_waited:%llu\r\n"
                "snapshot_groups_drained:%llu\r\n"
                "aof_enabled:%u\r\naof_rewrite_in_progress:%u\r\n"
                "aof_rewrite_scheduled:%u\r\naof_last_bgrewrite_status:%s\r\n"
                "aof_last_write_status:%s\r\naof_base_size:%llu\r\n"
                "aof_current_size:%llu\r\naof_pending_rewrite:%u\r\n"
                "aof_records_written:%llu\r\n"
                "aof_replayed_records:%llu\r\naof_groups_committed:%llu\r\n"
                "aof_groups_skipped_on_replay:%llu\r\naof_fsyncs:%llu\r\n"
                "aof_send_gate_waits:%llu\r\naof_control_frames_deferred:%llu\r\n"
                "aof_rewrite_base_size:%llu\r\n"
                "aof_rewrite_requests:%llu\r\naof_rewrite_completions:%llu\r\n"
                "aof_auto_rewrite_triggers:%llu\r\naof_history_unlinks:%llu\r\n"
                "aof_rewrite_failures:%llu\r\naof_rewrite_consecutive_failures:%u\r\n"
                "aof_auto_rewrite_backoff_skips:%llu\r\n",
                g_server && g_server->snapshot().in_progress() ? 1u : 0u,
                static_cast<long long>(g_server ? g_server->snapshot().last_save_time() : 0),
                static_cast<unsigned long long>(
                    g_server ? g_server->save_changes_since_last_save() : 0),
                static_cast<unsigned long long>(
                    g_server ? g_server->scheduled_save_triggers() : 0),
                static_cast<unsigned long long>(g_server ? g_server->save_cron_checks() : 0),
                static_cast<unsigned long long>(preimages),
                static_cast<unsigned long long>(g_server ? g_server->snapshot().cuts_armed() : 0),
                static_cast<unsigned long long>(g_server ? g_server->snapshot().cuts_waited() : 0),
                static_cast<unsigned long long>(
                    g_server ? g_server->snapshot().drained_groups() : 0),
                g_server && g_server->aof().configured() ? 1u : 0u,
                g_server && g_server->aof().rewrite_in_progress() ? 1u : 0u,
                g_server && g_server->aof().rewrite_scheduled() ? 1u : 0u,
                g_server && g_server->aof().last_rewrite_ok() ? "ok" : "err",
                g_server && g_server->aof().failed() ? "err" : "ok",
                static_cast<unsigned long long>(g_server ? g_server->aof().base_size() : 0),
                static_cast<unsigned long long>(g_server ? g_server->aof().current_size() : 0),
                g_server && g_server->aof().rewrite_scheduled() ? 1u : 0u,
                static_cast<unsigned long long>(g_server ? g_server->aof().records_written() : 0),
                static_cast<unsigned long long>(g_server ? g_server->aof().replayed_records() : 0),
                static_cast<unsigned long long>(g_server ? g_server->aof().groups_committed() : 0),
                static_cast<unsigned long long>(g_server ? g_server->aof().groups_skipped() : 0),
                static_cast<unsigned long long>(g_server ? g_server->aof().fsyncs() : 0),
                static_cast<unsigned long long>(g_server ? g_server->aof().send_gate_waits() : 0),
                static_cast<unsigned long long>(g_server ? g_server->aof().control_defers() : 0),
                static_cast<unsigned long long>(g_server ? g_server->aof().rewrite_base_size() : 0),
                static_cast<unsigned long long>(g_server ? g_server->aof().rewrite_requests() : 0),
                static_cast<unsigned long long>(g_server ? g_server->aof().rewrite_completions() : 0),
                static_cast<unsigned long long>(g_server ? g_server->aof().auto_rewrite_triggers() : 0),
                static_cast<unsigned long long>(g_server ? g_server->aof().history_unlinks() : 0),
                static_cast<unsigned long long>(g_server ? g_server->aof().rewrite_failures() : 0),
                g_server ? g_server->aof().consecutive_rewrite_failures() : 0,
                static_cast<unsigned long long>(g_server ? g_server->aof().auto_rewrite_backoff_skips() : 0));
    }
    if (info_section(op, "STATS")) {
        const ScriptStats scripting = script_stats();
        const FunctionStats functions = function_stats();
        const uint64_t sampled_rate = info_stats_sample_ops(sampled_ops);
        appendf(body, "# Stats\r\ntotal_connections_received:%llu\r\nrejected_connections:%llu\r\n"
                      "total_commands_processed:%llu\r\nkeyspace_hits:%llu\r\nkeyspace_misses:%llu\r\n"
                      "expired_keys:%llu\r\nevicted_keys:%llu\r\ninstantaneous_ops_per_sec:%llu\r\n"
                      "expired_hash_fields:%llu\r\nhash_field_expires:%llu\r\n"
                      "keyspace_rehashes:%llu\r\n"
                      "total_net_input_bytes:%llu\r\ntotal_net_output_bytes:%llu\r\n"
                      "sends_submitted:%llu\r\nshort_writes:%llu\r\nbytes_sent:%llu\r\n"
                      "peer_aborts:%llu\r\nsend_errors:%llu\r\n"
                      "zc_sends:%llu\r\nzc_releases:%llu\r\n"
                      "auth_failures:%llu\r\n"
                      "acl_access_denied_auth:%llu\r\nacl_access_denied_cmd:%llu\r\n"
                      "acl_access_denied_key:%llu\r\nacl_access_denied_channel:%llu\r\n"
                      "acl_pubsub_clients_killed:%llu\r\nacl_perm_retired:%llu\r\n"
                      "atomic_groups:%llu\r\natomic_inflight:%llu\r\n"
                      "atomic_predecessor_reads:%llu\r\natomic_chain_max:%llu\r\n"
                      "atomic_cleanup_fast:%llu\r\natomic_cleanup_slow:%llu\r\n"
                      "atomic_promotions:%llu\r\natomic_window_stalls:%llu\r\n"
                      "atomic_records_freed:%llu\r\natomic_entries:%llu\r\n"
                      "atomic_gauge_underflows:%llu\r\n"
                      "atomic_pending_entries:%llu\r\natomic_localfast:%llu\r\n"
                      "atomic_scan_order_holds:%llu\r\natomic_exec_order_holds:%llu\r\n"
                      "watch_reservation_waits:%llu\r\n"
                      "watch_reservation_coexist:%llu\r\n"
                      "watch_reservation_precommit_aborts:%llu\r\n"
                      "atomic_commit_windows:%llu\r\natomic_commit_holds:%llu\r\n"
                      "atomic_read_cuts_held:%llu\r\natomic_fanout_cuts:%llu\r\n"
                      "atomic_exec_read_cuts:%llu\r\n"
                      "atomic_credit_pool:%u\r\natomic_credit_debt:%u\r\n"
                      "script_stage_owner_tasks:%llu\r\nscript_run_attempts:%llu\r\n"
                      "script_validate_owner_tasks:%llu\r\nscript_apply_owner_tasks:%llu\r\n"
                      "script_crossshard_activations:%llu\r\nscript_group_commits:%llu\r\n"
                      "script_group_occ_retries:%llu\r\nscript_group_occ_giveups:%llu\r\n"
                      "script_staged_bytes_total:%llu\r\nscript_crossshard_window_refusals:%llu\r\n"
                      "script_group_aborts_oom:%llu\r\n"
                      "script_keys_armed:%llu\r\nscript_keys_released:%llu\r\n"
                      "script_intents_live:%llu\r\nscript_write_tickets_forced:%llu\r\n"
                      "sort_deref_lookups:%llu\r\nsort_deref_refusals:%llu\r\n"
                      "sort_scatter_general:%llu\r\nsort_deref_escapes:%llu\r\n"
                      "pubsub_channels:%llu\r\npubsub_subscriptions:%llu\r\n"
                      "pubsubshard_channels:%llu\r\npubsubshard_subscriptions:%llu\r\n"
                      "pubsub_patterns:%llu\r\npubsub_home_entries:%llu\r\n"
                      "pubsub_inflight:%llu\r\npubsub_pending_commands:%llu\r\n"
                      "pubsub_blobs:%llu\r\npubsub_deliveries:%llu\r\n"
                      "pubsub_delivery_batches:%llu\r\n"
                      "client_output_buffer_limit_disconnections:%llu\r\n"
                      "notify_events_fired:%llu\r\nnotify_events_dropped:%llu\r\n"
                      "client_scatter_requests:%llu\r\nclient_scatter_io_responses:%llu\r\n"
                      "plain_connections_received:%llu\r\ntls_connections_received:%llu\r\n"
                      "tls_current_connections:%llu\r\ntls_handshakes_started:%llu\r\n"
                      "tls_handshakes_completed:%llu\r\ntls_handshakes_failed:%llu\r\n"
                      "tls_want_read:%llu\r\ntls_want_write:%llu\r\n"
                      "tls_ciphertext_input_bytes:%llu\r\ntls_plaintext_input_bytes:%llu\r\n"
                      "tls_ciphertext_output_bytes:%llu\r\ntls_plaintext_output_bytes:%llu\r\n"
                      "tls_zc_suppressed:%llu\r\n"
                      "tls_ktls_active:%llu\r\ntls_ktls_fallback:%llu\r\n"
                      "blocking_waiters:%llu\r\n"
                      "number_of_cached_scripts:%llu\r\nnumber_of_libraries:%llu\r\n"
                      "number_of_functions:%llu\r\n"
                      "script_flush_generation:%llu\r\nscript_interpreter_builds:%llu\r\n"
                      "script_chunk_cache_hits:%llu\r\nscript_chunk_cache_misses:%llu\r\n"
                      "script_readonly_rejections:%llu\r\n"
                      "script_effect_writes:%llu\r\nscript_failed_after_effects:%llu\r\n"
                      "function_generation:%llu\r\nfunction_calls:%llu\r\n"
                      "function_thread_rebuilds:%llu\r\nfunction_readonly_rejections:%llu\r\n"
                      "monitor_feed_lines:%llu\r\nclient_pause_holds:%llu\r\n"
                      "client_no_touch_ops:%llu\r\n"
                      "tracking_total_keys:%llu\r\ntracking_total_items:%llu\r\n"
                      "tracking_total_prefixes:%llu\r\ntracking_invalidations:%llu\r\n"
                      "slowlog_batches_timed:%llu\r\nslowlog_escalations:%llu\r\n"
                      "slowlog_entries_recorded:%llu\r\nlatency_events_recorded:%llu\r\n"
                      "net_io_epoll_events:%llu\r\nnet_io_epoll_recvs:%llu\r\n"
                      "oob_frames_segmented:%llu\r\noob_frames_deferred:%llu\r\n"
                      "barrier_owner_overlaps:%llu\r\nbarrier_releases_held:%llu\r\n",
                static_cast<unsigned long long>(connections), static_cast<unsigned long long>(rejected),
                static_cast<unsigned long long>(total_ops), static_cast<unsigned long long>(hits),
                static_cast<unsigned long long>(misses), static_cast<unsigned long long>(expired),
                static_cast<unsigned long long>(evicted),
                static_cast<unsigned long long>(sampled_rate),
                static_cast<unsigned long long>(expired_hash_fields),
                static_cast<unsigned long long>(hash_field_expires),
                static_cast<unsigned long long>(keyspace_rehashes),
                static_cast<unsigned long long>(net_input_bytes),
                static_cast<unsigned long long>(net_output_bytes),
                static_cast<unsigned long long>(sends_submitted),
                static_cast<unsigned long long>(short_writes),
                static_cast<unsigned long long>(bytes_sent),
                static_cast<unsigned long long>(peer_aborts),
                static_cast<unsigned long long>(send_errors),
                static_cast<unsigned long long>(zc_sends),
                static_cast<unsigned long long>(zc_releases),
                static_cast<unsigned long long>(auth_failures),
                static_cast<unsigned long long>(acl_denied_auth),
                static_cast<unsigned long long>(acl_denied_cmd),
                static_cast<unsigned long long>(acl_denied_key),
                static_cast<unsigned long long>(acl_denied_channel),
                static_cast<unsigned long long>(
                    g_server ? g_server->acl_pubsub_clients_killed() : 0),
                static_cast<unsigned long long>(
                    g_server ? g_server->acl_perm_retired_count() : 0),
                static_cast<unsigned long long>(g_server ? g_server->atomic_groups() : 0),
                static_cast<unsigned long long>(g_server ? g_server->atomic_inflight() : 0),
                static_cast<unsigned long long>(atomic_predecessor_reads),
                static_cast<unsigned long long>(atomic_chain_max),
                static_cast<unsigned long long>(atomic_cleanup_fast),
                static_cast<unsigned long long>(atomic_cleanup_slow),
                static_cast<unsigned long long>(atomic_promotions),
                static_cast<unsigned long long>(g_server ? g_server->atomic_window_stalls() : 0),
                static_cast<unsigned long long>(atomic_records_freed),
                static_cast<unsigned long long>(atomic_entries),
                static_cast<unsigned long long>(atomic_gauge_underflows),
                static_cast<unsigned long long>(atomic_pending_entries),
                static_cast<unsigned long long>(atomic_localfast),
                static_cast<unsigned long long>(atomic_scan_holds),
                static_cast<unsigned long long>(atomic_exec_order_holds),
                static_cast<unsigned long long>(watch_reservation_waits),
                static_cast<unsigned long long>(watch_reservation_coexist),
                static_cast<unsigned long long>(watch_reservation_precommit_aborts),
                static_cast<unsigned long long>(
                    g_server ? g_server->atomic_commit_windows() : 0),
                static_cast<unsigned long long>(
                    g_server ? g_server->atomic_commit_holds() : 0),
                static_cast<unsigned long long>(
                    g_server ? g_server->atomic_read_cuts_held() : 0),
                static_cast<unsigned long long>(
                    g_server ? g_server->atomic_fanout_cuts() : 0),
                static_cast<unsigned long long>(
                    g_server ? g_server->atomic_exec_read_cuts() : 0),
                g_server ? g_server->atomic_credit_pool() : 0,
                g_server ? g_server->atomic_credit_debt() : 0,
                static_cast<unsigned long long>(g_server ? g_server->script_stage_owner_tasks() : 0),
                static_cast<unsigned long long>(g_server ? g_server->script_run_attempts() : 0),
                static_cast<unsigned long long>(g_server ? g_server->script_validate_owner_tasks() : 0),
                static_cast<unsigned long long>(g_server ? g_server->script_apply_owner_tasks() : 0),
                static_cast<unsigned long long>(g_server ? g_server->script_crossshard_activations() : 0),
                static_cast<unsigned long long>(g_server ? g_server->script_group_commits() : 0),
                static_cast<unsigned long long>(g_server ? g_server->script_group_occ_retries() : 0),
                static_cast<unsigned long long>(g_server ? g_server->script_group_occ_giveups() : 0),
                static_cast<unsigned long long>(g_server ? g_server->script_staged_bytes_total() : 0),
                static_cast<unsigned long long>(g_server ? g_server->script_crossshard_window_refusals() : 0),
                static_cast<unsigned long long>(g_server ? g_server->script_group_aborts_oom() : 0),
                static_cast<unsigned long long>(g_server ? g_server->script_keys_armed() : 0),
                static_cast<unsigned long long>(g_server ? g_server->script_keys_released() : 0),
                static_cast<unsigned long long>(g_server ? g_server->script_intents_live() : 0),
                static_cast<unsigned long long>(
                    g_server ? g_server->script_write_tickets_forced() : 0),
                static_cast<unsigned long long>(g_server ? g_server->sort_deref_lookups() : 0),
                static_cast<unsigned long long>(g_server ? g_server->sort_deref_refusals() : 0),
                static_cast<unsigned long long>(g_server ? g_server->sort_scatter_general() : 0),
                static_cast<unsigned long long>(g_server ? g_server->sort_deref_escapes() : 0),
                static_cast<unsigned long long>(g_server ? g_server->pubsub_active_channels() : 0),
                static_cast<unsigned long long>(g_server ? g_server->pubsub_subscriptions() : 0),
                static_cast<unsigned long long>(g_server ? g_server->pubsub_shard_channels() : 0),
                static_cast<unsigned long long>(g_server ? g_server->pubsub_shard_subscriptions() : 0),
                static_cast<unsigned long long>(g_server ? g_server->pubsub_pattern_subscriptions() : 0),
                static_cast<unsigned long long>(g_server ? g_server->pubsub_home_entries() : 0),
                static_cast<unsigned long long>(g_server ? g_server->pubsub_inflight() : 0),
                static_cast<unsigned long long>(g_server ? g_server->pubsub_pending() : 0),
                static_cast<unsigned long long>(g_server ? g_server->pubsub_blobs() : 0),
                static_cast<unsigned long long>(g_server ? g_server->pubsub_deliveries() : 0),
                static_cast<unsigned long long>(g_server ? g_server->pubsub_delivery_batches() : 0),
                static_cast<unsigned long long>(
                    g_server ? g_server->client_output_buffer_limit_disconnections() : 0),
                static_cast<unsigned long long>(g_server ? g_server->notify_events_fired() : 0),
                static_cast<unsigned long long>(g_server ? g_server->notify_events_dropped() : 0),
                static_cast<unsigned long long>(g_server ? g_server->client_scatter_requests() : 0),
                static_cast<unsigned long long>(
                    g_server ? g_server->client_scatter_io_responses() : 0),
                static_cast<unsigned long long>(plain_accepts),
                static_cast<unsigned long long>(tls_accepts),
                static_cast<unsigned long long>(tls_accepts - tls_connections_freed),
                static_cast<unsigned long long>(tls_handshakes_started),
                static_cast<unsigned long long>(tls_handshakes_completed),
                static_cast<unsigned long long>(tls_handshakes_failed),
                static_cast<unsigned long long>(tls_want_read),
                static_cast<unsigned long long>(tls_want_write),
                static_cast<unsigned long long>(tls_ciphertext_input),
                static_cast<unsigned long long>(tls_plaintext_input),
                static_cast<unsigned long long>(tls_ciphertext_output),
                static_cast<unsigned long long>(tls_plaintext_output),
                static_cast<unsigned long long>(tls_zc_suppressed),
                static_cast<unsigned long long>(tls_ktls_active),
                static_cast<unsigned long long>(tls_ktls_fallback),
                static_cast<unsigned long long>(blocking_waiters),
                static_cast<unsigned long long>(scripting.cached_scripts),
                static_cast<unsigned long long>(functions.libraries),
                static_cast<unsigned long long>(functions.functions),
                static_cast<unsigned long long>(scripting.flush_generation),
                static_cast<unsigned long long>(scripting.state_rebuilds),
                static_cast<unsigned long long>(scripting.compile_hits),
                static_cast<unsigned long long>(scripting.compile_misses),
                static_cast<unsigned long long>(scripting.ro_rejections),
                static_cast<unsigned long long>(scripting.effect_writes),
                static_cast<unsigned long long>(scripting.failed_after_effects),
                static_cast<unsigned long long>(functions.generation),
                static_cast<unsigned long long>(functions.calls),
                static_cast<unsigned long long>(functions.thread_rebuilds),
                static_cast<unsigned long long>(functions.ro_rejections),
                static_cast<unsigned long long>(g_server ? g_server->climon_monitor_lines() : 0),
                static_cast<unsigned long long>(g_server ? g_server->climon_pause_holds() : 0),
                static_cast<unsigned long long>(g_server ? g_server->climon_no_touch_ops() : 0),
                static_cast<unsigned long long>(g_server ? g_server->climon_tracking_keys() : 0),
                static_cast<unsigned long long>(g_server ? g_server->climon_tracking_items() : 0),
                static_cast<unsigned long long>(g_server ? g_server->climon_tracking_prefixes() : 0),
                static_cast<unsigned long long>(g_server ? g_server->climon_invalidations() : 0),
                // Mechanism counters: a slowlog test that cannot see these move proves nothing.
                static_cast<unsigned long long>(slowlog_batches_timed()),
                static_cast<unsigned long long>(slowlog_escalations()),
                static_cast<unsigned long long>(slowlog_entries_recorded()),
                static_cast<unsigned long long>(latency_events_recorded()),
                static_cast<unsigned long long>(epoll_events),
                static_cast<unsigned long long>(epoll_recvs),
                // Out-of-band frame channel: a push battery that cannot see these move never
                // reached the non-quiesced / mid-drain geometry it is there to cover.
                static_cast<unsigned long long>(g_server ? g_server->oob_frames_segmented() : 0),
                static_cast<unsigned long long>(g_server ? g_server->oob_frames_deferred() : 0),
                // barrier_owner_overlaps must read 0 on any production run: it is the live
                // assertion behind NOTES-BARRIER.md's reachability verdict. barrier_releases_held
                // is the fired-mechanism proof for DEBUG BARRIER-HOLD -- a barrier-ownership test
                // that leaves it at 0 never reached its geometry and must fail, not pass.
                static_cast<unsigned long long>(
                    g_server ? g_server->barrier_owner_overlaps() : 0),
                static_cast<unsigned long long>(
                    g_server ? g_server->barrier_releases_held() : 0));
    }
    if (info_section(op, "COMMANDSTATS")) {
        body += "# Commandstats\r\n";
        for (uint32_t id = 0; id < command_registry_size(); id++) {
            uint64_t calls = 0;
            for (uint32_t t = 0; g_server && t < g_server->nthreads(); t++)
                calls += g_server->thread(t).command_calls(id);
            if (id < baseline.command_calls.size())
                calls = minus_baseline(calls, baseline.command_calls[id]);
            if (!calls) continue;
            const std::string name = lower_name(command_registry_at(id)->name);
            appendf(body, "cmdstat_%s:calls=%llu\r\n",
                    name.c_str(), static_cast<unsigned long long>(calls));
        }
    }
    if (info_section(op, "KEYSPACE")) {
        body += "# Keyspace\r\n";
        appendf(body, "db0:keys=%llu,expires=%llu\r\n",
                static_cast<unsigned long long>(keys), static_cast<unsigned long long>(expires));
    }
    if (g_server && info_section(op, "LB")) lbsignals_info_section(*g_server, body);
    reply_verbatim(op.sink(), Slice(body.data(), body.size()), "txt", op.resp3());
}

void cmd_dbsize(Shard&, Op& op) {
    if (op.argc() == 2 && !eq_icase(op.arg(1), "NOW")) {
        reply_err(op.sink(), "ERR unknown DBSIZE option");
        return;
    }
    uint64_t keys = 0;
    if (g_server) for (uint32_t i = 0; i < g_server->nshards(); i++)
        keys += g_server->shard(static_cast<int32_t>(i)).published_size();
    reply_int(op.sink(), static_cast<long long>(keys));
}

// publish_size after clear: publications otherwise happen only at executor batch boundaries, so an
// idle shard would advertise its pre-flush count forever (DBSIZE stuck at stale totals).
void cmd_flush(Shard& sh, Op&) {
    const bool changed = sh.store().size() != 0;
    if (sh.store().snapshot_active()) {
        // The scatter snapshot gate has serialized every frozen pre-image before this handler is
        // reached.  Keep the frozen table allocation/cursor alive for the capture walker: clear()
        // frees both tables, which was the pre-existing FLUSH-under-capture bug.  Logical erases
        // retain the table geometry and leave ordinary tombstones for traversal to cross safely.
        sh.store().clear_during_snapshot();
    } else {
        sh.store().clear();
    }
    if (changed && (sh.notify_mask() & NOTIFY_SAVE)) sh.note_save_change();
    sh.publish_size();
}

void cmd_randomkey(Shard& sh, Op& op) {
    KvObj* obj = sh.store().random_live();
    if (!obj) reply_null(op.sink(), op.resp3());
    else reply_bulk(op.sink(), obj->key());
}

const char* object_type(const KvObj* obj) {
    switch (static_cast<Type>(obj->type)) {
        case Type::String: return "string";
        case Type::Hash: return "hash";
        case Type::List: return "list";
        case Type::Set: return "set";
        case Type::Zset: return "zset";
        case Type::Stream: return "stream";
    }
    return "none";
}

void cmd_scan(Shard& sh, Op& op) {
    uint64_t outer = 0;
    if (!command_parse_scan_cursor(op.arg(1), outer)) {
        reply_err(op.sink(), "ERR invalid cursor"); return;
    }
    const uint32_t shard_id = static_cast<uint32_t>(outer >> 56);
    uint64_t inner = outer & kScanInnerMask;
    if (!g_server || shard_id >= g_server->nshards() || shard_id != static_cast<uint32_t>(sh.id()) ||
        inner > kMaxInnerCursor) {
        reply_err(op.sink(), "ERR invalid cursor"); return;
    }
    Slice match("*", 1), type;
    uint32_t count = 10;
    for (uint32_t i = 2; i < op.argc();) {
        if (eq_icase(op.arg(i), "MATCH") && i + 1 < op.argc()) {
            match = op.arg(i + 1); i += 2;
        } else if (eq_icase(op.arg(i), "COUNT") && i + 1 < op.argc()) {
            // Two different errors, as Redis and as HSCAN/SSCAN/ZSCAN here already spell them:
            // an unparseable COUNT is an integer error, a parseable one below 1 is a syntax
            // error. SCAN was the one member of the family answering "syntax error" to both.
            // Verified against the 7.4 oracle: COUNT -1 and COUNT 0 are BOTH syntax errors (so the
            // parse must be signed), and COUNT 4294967296 is ACCEPTED, so a large value clamps
            // rather than being rejected. The sibling lane's unsigned parse got both of those
            // backwards; this side is the one that matches.
            int64_t parsed = 0;
            if (!parse_i64_canonical(op.arg(i + 1), parsed)) {
                reply_err(op.sink(), "ERR value is not an integer or out of range"); return;
            }
            if (parsed < 1) { reply_syntax(op.sink()); return; }
            // A COUNT past the slot-work hint's range is clamped, not rejected: it is a hint, and
            // one call is bounded by the table anyway.
            count = parsed > static_cast<int64_t>(UINT32_MAX) ? UINT32_MAX
                                                              : static_cast<uint32_t>(parsed);
            i += 2;
        } else if (eq_icase(op.arg(i), "TYPE") && i + 1 < op.argc()) {
            type = op.arg(i + 1);
            i += 2;
        } else if (eq_icase(op.arg(i), "NOVALUES")) {
            reply_err(op.sink(), "ERR NOVALUES option can only be used in HSCAN"); return;
        } else { reply_syntax(op.sink()); return; }
    }

    std::vector<Slice> keys;
    keys.reserve(std::min<uint32_t>(count, 1024));
    inner = sh.store().scan(inner, count, [&](KvObj* obj) {
        if (type.n && !eq_icase(type, object_type(obj))) return;
        if (command_glob_match(match, obj->key())) keys.push_back(obj->key());
    });
    uint64_t next = 0;
    if (inner) next = (static_cast<uint64_t>(shard_id) << 56) | inner;
    else if (shard_id + 1 < g_server->nshards()) next = static_cast<uint64_t>(shard_id + 1) << 56;

    char cursor[24];
    const uint32_t cursor_len = u64_to_dec(cursor, next);
    auto sink = op.sink();
    reply_array_header(sink, 2);
    reply_bulk(sink, Slice(cursor, cursor_len));
    reply_array_header(sink, keys.size());
    for (Slice key : keys) reply_bulk(sink, key);
}

void cmd_transaction_control(Shard&, Op& op) {
    reply_err(op.sink(), "ERR internal transaction routing error");
}

static const CommandSpec kTable[] = {
    // name       min max flags                                                    handler        first last step
    {"PING",       1,  2, CmdFlags::ConnLocal,                                    cmd_ping,       0,  0, 0},
    {"ACL",        2, -1, CmdFlags::ConnLocal,                                    cmd_acl,        0,  0, 0},
    {"SAVE",       1,  1, CmdFlags::ConnLocal | CmdFlags::Admin,                  cmd_save,       0,  0, 0},
    {"BGSAVE",     1,  2, CmdFlags::ConnLocal | CmdFlags::Admin,                  cmd_bgsave,     0,  0, 0},
    {"BGREWRITEAOF",1, 1, CmdFlags::ConnLocal | CmdFlags::Admin,                  cmd_bgrewriteaof,0, 0, 0},
    {"LASTSAVE",   1,  1, CmdFlags::ConnLocal | CmdFlags::Admin,                  cmd_lastsave,   0,  0, 0},
    {"ECHO",       2,  2, CmdFlags::ConnLocal,                                    cmd_echo,       0,  0, 0},
    {"AUTH",       2,  3, CmdFlags::ConnLocal | CmdFlags::AclExempt,              cmd_auth,       0,  0, 0},
    {"HELLO",       1, -1, CmdFlags::ConnLocal | CmdFlags::AclExempt,              cmd_hello,      0,  0, 0},
    {"RESET",      1,  1, CmdFlags::ConnLocal | CmdFlags::AclExempt |
                          CmdFlags::Climon,                                       cmd_reset,      0,  0, 0},
    {"QUIT",       1,  1, CmdFlags::ConnLocal | CmdFlags::AclExempt,              cmd_quit,       0,  0, 0},
    {"SUBSCRIBE",   2, -1, CmdFlags::ConnLocal | CmdFlags::PubSub,                cmd_pubsub_only,0,  0, 0},
    {"UNSUBSCRIBE", 1, -1, CmdFlags::ConnLocal | CmdFlags::PubSub,                cmd_pubsub_only,0,  0, 0},
    {"PSUBSCRIBE",  2, -1, CmdFlags::ConnLocal | CmdFlags::PubSub,                cmd_pubsub_only,0,  0, 0},
    {"PUNSUBSCRIBE",1, -1, CmdFlags::ConnLocal | CmdFlags::PubSub,                cmd_pubsub_only,0,  0, 0},
    {"SSUBSCRIBE",   2, -1, CmdFlags::ConnLocal | CmdFlags::PubSub,                cmd_pubsub_only,0,  0, 0},
    {"SUNSUBSCRIBE", 1, -1, CmdFlags::ConnLocal | CmdFlags::PubSub,                cmd_pubsub_only,0,  0, 0},
    {"PUBLISH",     3,  3, CmdFlags::ConnLocal | CmdFlags::PubSub,                cmd_pubsub_only,0,  0, 0},
    {"SPUBLISH",    3,  3, CmdFlags::ConnLocal | CmdFlags::PubSub,                cmd_pubsub_only,0,  0, 0},
    {"PUBSUB",      2, -1, CmdFlags::ConnLocal | CmdFlags::PubSub | CmdFlags::Admin,cmd_pubsub_only,0,0,0},
    {"CLIENT",     2, -1, CmdFlags::ConnLocal | CmdFlags::PubSub | CmdFlags::Admin |
                          CmdFlags::Climon,                                       cmd_client,    0,  0, 0},
    // MONITOR is answered entirely on the io side by the climon object; the PubSub flag is the
    // established "io-owned async command" routing, not a pub/sub semantic.
    {"MONITOR",    1,  1, CmdFlags::ConnLocal | CmdFlags::PubSub | CmdFlags::Admin |
                          CmdFlags::Climon,                                       cmd_monitor,    0,  0, 0},
    {"COMMAND",    1, -1, CmdFlags::ConnLocal | CmdFlags::Admin,                  cmd_command,    0,  0, 0},
    {"CONFIG",     2, -1, CmdFlags::Admin | CmdFlags::ConfigRoute,                cmd_config,     0,  0, 0},
    {"DEBUG",      2, -1, CmdFlags::Admin | CmdFlags::ConfigRoute,                cmd_debug,      0,  0, 0},
    {"INFO",       1,  2, CmdFlags::ConnLocal | CmdFlags::Admin,                  cmd_info,       0,  0, 0},
    {"SELECT",     2,  2, CmdFlags::ConnLocal,                                    cmd_select,     0,  0, 0},
        {"DBSIZE",     1,  2, CmdFlags::Admin | CmdFlags::ConfigRoute,                cmd_dbsize,     0,  0, 0},
    {"FLUSHALL",   1,  2, CmdFlags::Write | CmdFlags::Admin | CmdFlags::AllShards,cmd_flush,      0,  0, 0},
    {"FLUSHDB",    1,  2, CmdFlags::Write | CmdFlags::Admin | CmdFlags::AllShards,cmd_flush,      0,  0, 0},
    {"RANDOMKEY",  1,  1, CmdFlags::Readonly | CmdFlags::RandomShard,             cmd_randomkey,  0,  0, 0},
    {"SCAN",       2, -1, CmdFlags::Readonly | CmdFlags::CursorShard,             cmd_scan,       0,  0, 0},
    {"KEYS",       2,  2, CmdFlags::Readonly | CmdFlags::Admin | CmdFlags::MultiShard,cmd_xshard_only,0,0,0},
    {"SORT",       2, -1, CmdFlags::Write | CmdFlags::DenyOom | CmdFlags::MultiShard,cmd_xshard_only,1,1,1},
    {"MULTI",      1,  1, CmdFlags::ConnLocal | CmdFlags::Transaction,              cmd_transaction_control,0,0,0},
    {"EXEC",       1,  1, CmdFlags::ConnLocal | CmdFlags::Transaction,              cmd_transaction_control,0,0,0},
    {"DISCARD",    1,  1, CmdFlags::ConnLocal | CmdFlags::Transaction,              cmd_transaction_control,0,0,0},
    {"WATCH",      2, -1, CmdFlags::ConnLocal | CmdFlags::Transaction,              cmd_transaction_control,1,-1,1},
    {"UNWATCH",    1,  1, CmdFlags::ConnLocal | CmdFlags::Transaction,              cmd_transaction_control,0,0,0},
};

}  // namespace

bool debug_command_allowed(const Server& server, const Client* client) {
    const DebugCommandMode mode = server.cfg().enable_debug_command;
    if (mode == DebugCommandMode::Yes) return true;
    if (mode != DebugCommandMode::Local || !client) return false;

    sockaddr_storage peer{};
    socklen_t length = sizeof(peer);
    if (::getpeername(client->fd(), reinterpret_cast<sockaddr*>(&peer), &length) != 0)
        return false;
    if (peer.ss_family == AF_UNIX) return true;
    if (peer.ss_family == AF_INET) {
        const auto* address = reinterpret_cast<const sockaddr_in*>(&peer);
        return (ntohl(address->sin_addr.s_addr) & 0xff000000u) == 0x7f000000u;
    }
    if (peer.ss_family == AF_INET6) {
        const auto* address = reinterpret_cast<const sockaddr_in6*>(&peer);
        return IN6_IS_ADDR_LOOPBACK(&address->sin6_addr);
    }
    return false;
}

void reply_debug_command_denied(Op& op) {
    reply_err(op.sink(),
              "ERR DEBUG command not allowed. If the enable-debug-command option is set to \"local\", you can run it from a local connection, otherwise you need to set this option in the configuration file, and then restart the server.");
}

void cmd_debug(Shard& shard, Op& op) {
    if (!g_server || !debug_command_allowed(*g_server, g_client)) {
        reply_debug_command_denied(op);
        return;
    }
    cmd_debug_impl(shard, op);
}

bool command_parse_scan_cursor(Slice text, uint64_t& cursor) {
    // Redis's string2ull first tries canonical string2ll, then falls back to strtoull. Its input
    // is a C string, so an embedded NUL terminates the cursor text as well.
    uint32_t length = 0;
    while (length < text.n && text.p[length] != '\0') length++;
    const Slice input(text.p, length);

    int64_t signed_value = 0;
    if (parse_i64_canonical(input, signed_value)) {
        if (signed_value < 0) return false;
        cursor = static_cast<uint64_t>(signed_value);
        return true;
    }

    uint32_t position = 0;
    while (position < input.n &&
           (input.p[position] == ' ' ||
            (input.p[position] >= '\t' && input.p[position] <= '\r')))
        position++;
    bool negative = false;
    if (position < input.n && (input.p[position] == '+' || input.p[position] == '-')) {
        negative = input.p[position] == '-';
        position++;
    }
    if (position == input.n) return false;

    uint64_t magnitude = 0;
    for (; position < input.n; position++) {
        const char byte = input.p[position];
        if (byte < '0' || byte > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(byte - '0');
        if (magnitude > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
        magnitude = magnitude * 10 + digit;
    }
    cursor = negative ? uint64_t{0} - magnitude : magnitude;
    return true;
}

Server* command_server() { return g_server; }
ThreadCtx* command_local_thread() { return g_thread; }

void command_config_snapshot(std::vector<std::pair<std::string, std::string>>& out) {
    std::lock_guard<std::mutex> lock(g_config_mu);
    out.clear();
    out.reserve(g_config.size());
    for (const ConfigValue& item : g_config) out.emplace_back(item.name, item.value);
}

void command_config_resetstat() {
    // Capture the CURRENT aggregate rather than writing zeros into per-shard/per-thread counters
    // that only their owner may write. INFO subtracts this baseline. The reads here are the same
    // cross-thread reads INFO already performs on every call, so no new sharing is introduced.
    StatBaseline baseline;
    collect_stat_totals(baseline);
    info_stats_reset(baseline.sampled_ops, baseline.object_bytes);
    std::lock_guard<std::mutex> lock(g_stat_baseline_mu);
    g_stat_baseline = baseline;
}

void command_bind_server(Server* server) {
    g_server = server;
    scripting_bind_server(server);
    g_started_monotonic_ns = now_ns();
    info_stats_reset(0, 0);
    if (server) { init_config(server->cfg()); slowlog_configure(server->cfg()); }
}

void command_set_local_context(Client* client, ThreadCtx* thread) {
    g_client = client;
    g_thread = thread;
}

void command_client_connected(Client* client, const char* addr, const char* laddr,
                              bool unix_socket, uint64_t now_ms) {
    if (!client) return;
    ClientMeta meta;
    meta.addr = addr ? addr : "unknown:0";
    meta.laddr = laddr ? laddr : "unknown:0";
    meta.unix_socket = unix_socket;
    meta.created_ms = now_ms;
    // SLOWLOG entries are built by a shard owner, which cannot read this thread_local catalog, and
    // are read back by whichever IO thread runs SLOWLOG GET. The recorder keeps its own sharded
    // id -> (addr, name) directory; connection lifecycle is the only writer.
    slowlog_note_client(client->id(), meta.addr.c_str());
    g_client_meta[client->id()] = std::move(meta);
}

void command_client_disconnected(Client* client) {
    if (!client) return;
    slowlog_forget_client(client->id());
    g_client_meta.erase(client->id());
}

// Cold process-wide directory. CLIENT UNBLOCK and CLIENT TRACKING REDIRECT need to name a
// connection owned by ANOTHER io thread; the per-owner catalog above deliberately cannot. The
// mutex is taken at accept, at close, and by those two cold commands -- never on a reply path.
std::mutex g_client_dir_mu;
std::unordered_map<uint64_t, uint32_t> g_client_dir;

void command_client_directory_add(uint64_t id, uint32_t io) {
    std::lock_guard<std::mutex> lock(g_client_dir_mu);
    g_client_dir[id] = io;
}

void command_client_directory_remove(uint64_t id) {
    std::lock_guard<std::mutex> lock(g_client_dir_mu);
    g_client_dir.erase(id);
}

bool command_client_directory_find(uint64_t id, uint32_t& io) {
    std::lock_guard<std::mutex> lock(g_client_dir_mu);
    auto found = g_client_dir.find(id);
    if (found == g_client_dir.end()) return false;
    io = found->second;
    return true;
}

void command_client_set_subscriptions(Client* client, uint32_t channels, uint32_t patterns,
                                      uint32_t shard_channels) {
    ClientMeta* meta = client_meta(client);
    if (!meta) return;
    meta->subscriptions = channels;
    meta->pattern_subscriptions = patterns;
    meta->shard_subscriptions = shard_channels;
}

std::string command_client_info_line(const Client& client, uint64_t now_ms) {
    const ClientMeta* meta = client_meta(&client);
    if (meta) return client_info_line_impl(client, *meta, now_ms);
    ClientMeta fallback;
    fallback.addr = fallback.laddr = "unknown:0";
    fallback.created_ms = now_ms;
    return client_info_line_impl(client, fallback, now_ms);
}

bool command_client_filter_match(const Client& client, const PubSubEvent& event,
                                 uint64_t now_ms) {
    const ClientMeta* meta = client_meta(&client);
    if (!meta || client.dead() || client.closing()) return false;
    if (event.client_skipme && client.id() == event.caller_id) return false;
    if ((event.client_filter_mask & ClientFilterId) && client.id() != event.client_id)
        return false;
    if (event.client_filter_mask & ClientFilterIdList) {
        bool found = false;
        for (const PubSubEventItem& item : event.items) found |= item.count == client.id();
        if (!found) return false;
    }
    if ((event.client_filter_mask & ClientFilterAddr) && meta->addr != event.client_addr)
        return false;
    if ((event.client_filter_mask & ClientFilterLaddr) && meta->laddr != event.client_laddr)
        return false;
    if ((event.client_filter_mask & ClientFilterUser) &&
        event.client_user != acl_username(client.acl_user_idx())) return false;
    if (event.client_filter_mask & ClientFilterMaxAge) {
        const uint64_t age = now_ms >= meta->created_ms ? (now_ms - meta->created_ms) / 1000 : 0;
        if (age < event.client_max_age) return false;
    }
    if (event.client_filter_mask & ClientFilterType) {
        const bool pubsub = client.subscriber_mode();
        switch (event.client_type) {
            case ClientTypeFilter::Normal: if (pubsub) return false; break;
            case ClientTypeFilter::Pubsub: if (!pubsub) return false; break;
            case ClientTypeFilter::Master:
            case ClientTypeFilter::Replica: return false;
            case ClientTypeFilter::Any: break;
        }
    }
    return true;
}

bool command_client_set_name(Client* client, Slice name) {
    ClientMeta* meta = client_meta(client);
    if (!meta) return false;
    meta->name.assign(name.p, name.n);
    slowlog_note_client_name(client->id(), name.p, name.n);
    return true;
}

std::string command_client_name(const Client* client) {
    const ClientMeta* meta = client_meta(client);
    return meta ? meta->name : std::string();
}

bool command_client_set_info(Client* client, Slice option, Slice value) {
    ClientMeta* meta = client_meta(client);
    if (!meta) return false;
    if (eq_icase(option, "LIB-NAME")) meta->lib_name.assign(value.p, value.n);
    else if (eq_icase(option, "LIB-VER")) meta->lib_ver.assign(value.p, value.n);
    else return false;
    return true;
}

void command_client_set_no_evict(Client* client, bool enabled) {
    if (ClientMeta* meta = client_meta(client)) meta->no_evict = enabled;
}

void command_client_set_no_touch(Client* client, bool enabled) {
    if (ClientMeta* meta = client_meta(client)) meta->no_touch = enabled;
}

std::string command_client_addr(const Client* client) {
    const ClientMeta* meta = client_meta(client);
    return meta ? meta->addr : std::string("unknown:0");
}

void command_client_set_tracking_view(Client* client, bool on, int64_t redirect) {
    ClientMeta* meta = client_meta(client);
    if (!meta) return;
    meta->tracking = on;
    meta->tracking_redirect = redirect;
}

void command_client_reset_meta(Client* client) {
    ClientMeta* meta = client_meta(client);
    if (!meta) return;
    meta->name.clear();
    meta->lib_name.clear();
    meta->lib_ver.clear();
    meta->no_evict = false;
    meta->no_touch = false;
    meta->tracking = false;
    meta->tracking_redirect = -1;
    slowlog_note_client_name(client->id(), "", 0);
}

bool command_prepare_scan_route(Server& server, Op& op) {
    if (op.spec->flags & CmdFlags::SubcmdRoute)
        return command_prepare_subcmd_route(server, op);
    if (op.spec->flags & CmdFlags::ScriptRoute)
        return command_prepare_script_route(server, op);
    if (op.spec->flags & CmdFlags::StreamRoute) {
        if (op.local_xshard()) return true;
        reply_err(op.sink(), "ERR internal XREAD local routing error");
        return false;
    }
    uint64_t cursor = 0;
    if (!command_parse_scan_cursor(op.arg(1), cursor) ||
        (cursor & kScanInnerMask) > kMaxInnerCursor) {
        reply_err(op.sink(), "ERR invalid cursor"); return false;
    }
    const uint32_t shard = static_cast<uint32_t>(cursor >> 56);
    if (shard >= server.nshards()) { reply_err(op.sink(), "ERR invalid cursor"); return false; }
    op.hash = cursor;
    op.shard = static_cast<int32_t>(shard);
    return true;
}

bool command_validate_all_shards(Op& op) {
    if (op.argc() == 1) return true;
    if (op.argc() == 2 && (eq_icase(op.arg(1), "ASYNC") || eq_icase(op.arg(1), "SYNC"))) return true;
    reply_syntax(op.sink());
    return false;
}

bool command_config_routes_all_shards(Op& op) {
    // The conditional-scatter route: CONFIG SET fans out; DBSIZE NOW (owner request 2026-08-25)
    // is the exact-on-demand variant -- each owner counts its own store at execution time, so the
    // reply reflects everything already dispatched ahead of it on every shard, with none of the
    // batch-boundary publication lag the plain DBSIZE reads.
    if (op.cmd_name().eq_icase("dbsize")) return op.argc() == 2 && eq_icase(op.arg(1), "NOW");
    if (op.cmd_name().eq_icase("debug"))
        return op.argc() == 2 &&
               (eq_icase(op.arg(1), "reload") || eq_icase(op.arg(1), "loadaof") ||
                eq_icase(op.arg(1), "borrowcount"));
    return op.argc() >= 2 && eq_icase(op.arg(1), "SET");
}

bool command_validate_config_set(Op& op) {
    if (op.cmd_name().eq_icase("dbsize")) return true;   // DBSIZE NOW needs no further validation
    std::lock_guard<std::mutex> lock(g_config_mu);
    std::vector<std::pair<ConfigValue*, std::string>> updates;
    return collect_config_updates(op, updates);
}

CommandTable server_command_table() {
    return {kTable, sizeof(kTable) / sizeof(kTable[0])};
}

}  // namespace tomo

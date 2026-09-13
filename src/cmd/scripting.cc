// scripting.cc — EVAL/EVALSHA and the process-wide SHA1 script cache.
//
// Routing validates the declared KEYS range on IO. Execution is one ordinary task on that shard's
// owner, so Lua and every redis.call handler obey the ownership law without locks around a shard.
// The cache is the only cross-thread scripting object and is protected independently of shard data.
#include "command.h"
#include "notify.h"
#include "scripting.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../core/server.h"
#include "../core/shard.h"
#include "../exec/op.h"
#include "../net/resp.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
extern "C" {
#include "../../third_party/lua/lua_amalgamation.c"
}
#pragma GCC diagnostic pop

// Private Lua source macros must not escape into the surrounding C++ translation unit.
#undef getline
#undef next

namespace tomo {
namespace {

constexpr uint32_t kScriptMaxBytes = 1024 * 1024;
constexpr uint64_t kDefaultInstructionLimit = 100000;
constexpr int kHookInterval = 1000;
constexpr uint32_t kReplyMaxDepth = 32;
constexpr uint32_t kReplyMaxElements = 100000;

Server* g_script_server = nullptr;
char g_hook_context_key;
// Latched at bind time from the config so the hook — which runs every kHookInterval VM
// instructions — reads a plain global instead of walking Server. 0 = unlimited.
uint64_t g_instruction_limit = kDefaultInstructionLimit;

std::atomic<uint64_t> g_compile_hits{0};
std::atomic<uint64_t> g_compile_misses{0};
std::atomic<uint64_t> g_state_rebuilds{0};
std::atomic<uint64_t> g_ro_rejections{0};
// The two counters that make the no-undo contract testable rather than assumed. `g_effect_writes`
// counts nested Write-flagged commands that answered without an error, i.e. keyspace effects a
// script has already applied. `g_failed_after_effects` counts activations that then FAILED with at
// least one such effect standing — precisely the interleave the deleted undo log used to reverse.
// A regression that never advances the second counter has not exercised the fix at all.
std::atomic<uint64_t> g_effect_writes{0};
std::atomic<uint64_t> g_failed_after_effects{0};

uint32_t rotl32(uint32_t value, uint32_t bits) {
    return (value << bits) | (value >> (32 - bits));
}

std::string sha1_hex(Slice input) {
    uint32_t h0 = 0x67452301u, h1 = 0xefcdab89u, h2 = 0x98badcfeu,
             h3 = 0x10325476u, h4 = 0xc3d2e1f0u;
    const uint64_t bit_length = static_cast<uint64_t>(input.n) * 8;
    const size_t padded = (static_cast<size_t>(input.n) + 9 + 63) & ~size_t{63};
    std::vector<uint8_t> bytes(padded, 0);
    if (input.n) std::memcpy(bytes.data(), input.p, input.n);
    bytes[input.n] = 0x80;
    for (uint32_t i = 0; i < 8; i++)
        bytes[padded - 1 - i] = static_cast<uint8_t>(bit_length >> (i * 8));

    for (size_t block = 0; block < padded; block += 64) {
        uint32_t words[80];
        for (uint32_t i = 0; i < 16; i++) {
            const uint8_t* p = bytes.data() + block + i * 4;
            words[i] = (static_cast<uint32_t>(p[0]) << 24) |
                       (static_cast<uint32_t>(p[1]) << 16) |
                       (static_cast<uint32_t>(p[2]) << 8) | p[3];
        }
        for (uint32_t i = 16; i < 80; i++)
            words[i] = rotl32(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (uint32_t i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5a827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdcu; }
            else { f = b ^ c ^ d; k = 0xca62c1d6u; }
            const uint32_t next = rotl32(a, 5) + f + e + k + words[i];
            e = d; d = c; c = rotl32(b, 30); b = a; a = next;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    const uint32_t digest[5] = {h0, h1, h2, h3, h4};
    static constexpr char hex[] = "0123456789abcdef";
    std::string result(40, '0');
    for (uint32_t i = 0; i < 20; i++) {
        const uint8_t byte = static_cast<uint8_t>(digest[i / 4] >> (24 - (i % 4) * 8));
        result[i * 2] = hex[byte >> 4];
        result[i * 2 + 1] = hex[byte & 15];
    }
    return result;
}

class ScriptCache {
public:
    bool store(Slice body, std::string& sha) {
        try {
            sha = sha1_hex(body);
            std::string source(body.p, body.n);
            std::lock_guard<std::mutex> lock(mu_);
            scripts_.insert_or_assign(sha, std::move(source));
            return true;
        } catch (const std::bad_alloc&) {
            return false;
        }
    }

    bool find(Slice sha, std::string& source) const {
        try {
            std::lock_guard<std::mutex> lock(mu_);
            const auto it = scripts_.find(std::string(sha.p, sha.n));
            if (it == scripts_.end()) return false;
            source = it->second;
            return true;
        } catch (const std::bad_alloc&) {
            return false;
        }
    }

    bool exists(Slice sha) const {
        try {
            std::lock_guard<std::mutex> lock(mu_);
            return scripts_.find(std::string(sha.p, sha.n)) != scripts_.end();
        } catch (const std::bad_alloc&) {
            return false;
        }
    }

    void flush() {
        std::lock_guard<std::mutex> lock(mu_);
        scripts_.clear();
    }

    uint64_t size() const {
        std::lock_guard<std::mutex> lock(mu_);
        return scripts_.size();
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::string> scripts_;
};

ScriptCache g_scripts;

bool parse_nonnegative(Slice value, uint32_t& result, bool& negative) {
    negative = false;
    if (!value.n) return false;
    uint32_t pos = 0;
    if (value.p[0] == '-') { negative = true; pos = 1; }
    if (pos == value.n) return false;
    uint64_t parsed = 0;
    for (; pos < value.n; pos++) {
        const uint8_t c = static_cast<uint8_t>(value.p[pos]);
        if (c < '0' || c > '9') return false;
        parsed = parsed * 10 + (c - '0');
        if (parsed > UINT32_MAX) return false;
    }
    result = static_cast<uint32_t>(parsed);
    return true;
}

void reply_text_error(Op& op, std::string_view kind, std::string_view detail) {
    auto sink = op.sink();
    sink.push_back('-');
    sink.append(kind.data(), kind.size());
    if (!detail.empty()) { sink.push_back(' '); sink.append(detail.data(), detail.size()); }
    sink.append("\r\n", 2);
}

// A script error already carries its own code word ("ERR ...", "WRONGTYPE ...", "My Error").
void reply_raw_error(Op& op, std::string_view message) {
    auto sink = op.sink();
    sink.push_back('-');
    sink.append(message.data(), message.size());
    sink.append("\r\n", 2);
}

std::string clean_error(const char* text, size_t length) {
    const size_t take = std::min<size_t>(length, 2048);
    std::string result;
    result.reserve(take);
    for (size_t i = 0; i < take; i++) {
        const char ch = text[i];
        result.push_back(ch == '\r' || ch == '\n' || ch == '\0' ? ' ' : ch);
    }
    return result;
}

// Redis names the EVAL chunk "@user_script": the '@' tells Lua the name is a source path, so its
// position prefix reads `user_script:1:` rather than `[string "user_script"]:1:`. Byte-for-byte
// compatible error text starts here.
constexpr const char* kEvalChunkName = "@user_script";

bool validate_source(Slice source, std::string& error) {
    if (source.n > kScriptMaxBytes) {
        error = "script exceeds the 1 MiB limit";
        return false;
    }
    if (source.n && static_cast<uint8_t>(source.p[0]) == 0x1b) {
        error = "binary chunks are not allowed";
        return false;
    }
    lua_State* state = luaL_newstate();
    if (!state) { error = "out of memory"; return false; }
    const int status = luaL_loadbuffer(state, source.p, source.n, kEvalChunkName);
    if (status != 0) {
        size_t length = 0;
        const char* detail = lua_tolstring(state, -1, &length);
        error = clean_error(detail ? detail : "compile error", detail ? length : 13);
    }
    lua_close(state);
    return status == 0;
}

// THERE IS NO UNDO LOG, IN EITHER ATOMIC MODE — AND THAT IS THE SEMANTICS, NOT A GAP.
//
// v1 kept a deep pre-image of every declared key while `--atomic 1` was on and restored it when the
// activation failed (see NOTES-SCRIPTATOMIC.md). Redis has never undone a script's partial effects,
// so an activation that wrote and then raised diverged from the oracle the moment atomics were
// enabled, and the divergence was a LOST WRITE: the restore republished a superseded value over a
// committed one, or erased the key outright when the script had just created it. It also could not
// be made correct in place -- `capture()` read through FlatStore::find(), which resolves at the
// current MVCC read cut, while `rollback()` wrote through the raw erase/insert pair, which does not.
// A read-only activation reads at its pinned program-order cut, so the "original" it captured could
// already be a superseded version — restoring it published a stale value over a newer committed one
// and left any live pending record naming an object the restore had freed.
//
// A script is still externally indivisible: it is one task on one owner, and the single-owner law
// keeps every other task out for its whole duration. Isolation never needed the undo log; only
// failure-atomicity did, and Redis does not offer failure-atomicity.
class ScriptEvictionGuard {
public:
    // Unconditional, so the two atomic modes cannot disagree about what a script observes. Redis
    // does not evict inside an activation either: the OOM decision is taken once, when the script
    // starts. Two plain stores on a cold path; the GET/SET path never reaches here.
    explicit ScriptEvictionGuard(Shard& shard) : store_(&shard.store()) {
        previous_ = store_->script_suspend_eviction();
    }
    ~ScriptEvictionGuard() { store_->script_restore_eviction(previous_); }
    ScriptEvictionGuard(const ScriptEvictionGuard&) = delete;
    ScriptEvictionGuard& operator=(const ScriptEvictionGuard&) = delete;
private:
    FlatStore* store_ = nullptr;
    MaxmemoryPolicy previous_ = MaxmemoryPolicy::NoEviction;
};

struct ScriptContext {
    Shard* shard = nullptr;
    Op* parent = nullptr;
    uint32_t key_first = 0;
    uint32_t key_count = 0;
    uint64_t instructions = 0;
    uint32_t call_index = 0;
    bool timed_out = false;
    uint8_t script_resp = 2;
    // Set per call from the handler variant that entered (clean vs notify registry row). The
    // sandbox closures are built once per persistent state, so notify-variance is a runtime flag
    // here rather than a template parameter — one predictable test per nested call.
    bool notify = false;
    // EVAL_RO / EVALSHA_RO / FCALL_RO, and any function carrying the no-writes flag. Checked at
    // the redis.call dispatch site against the command row's Write bit.
    bool readonly = false;
    // Nested Write-flagged commands that answered without an error: the keyspace effects this
    // activation has already applied and which a failure must NOT reverse.
    uint32_t effect_writes = 0;
    // Filled by the pcall message handler: the first Lua frame's line at the point of the error.
    // Redis reports it in the " ... on @user_script:<line>." tail.
    int error_line = 0;
    uint8_t* cross_access = nullptr;
    uint32_t cross_access_count = 0;
};

// Persistent per-thread engine. Building luaL_newstate + the sandbox + recompiling the source on
// EVERY call measured 53.8k EVALSHA/s at p50 18.9 ms against vanilla redis's 550k/s (~18 µs of
// per-call construction). Scripts execute on shard-owner executors, so one state per thread needs
// no locks: sandbox and compiled chunks are built once; a call rebinds only the context slot and
// the KEYS/ARGV tables. The sandbox closures capture a pointer to `current` (stable for the life
// of the state), never a per-call context address.
struct LuaEngine {
    lua_State* state = nullptr;
    ScriptContext* current = nullptr;   // per-call slot; closures and the hook read through this
    uint64_t flush_generation = 0;      // mirrors g_script_flushes when the state was (re)built
    ~LuaEngine() {
        if (state) lua_close(state);
    }
};
thread_local LuaEngine t_lua_engine;
struct ScriptCrossBinding {
    Op* op = nullptr;
    uint8_t* access = nullptr;
    uint32_t count = 0;
};
thread_local ScriptCrossBinding t_script_cross;
std::atomic<uint64_t> g_script_flushes{0};

ScriptContext* lua_context(lua_State* state) {
    lua_pushlightuserdata(state, &g_hook_context_key);
    lua_gettable(state, LUA_REGISTRYINDEX);
    auto* slot = static_cast<ScriptContext**>(lua_touserdata(state, -1));
    lua_pop(state, 1);
    return slot ? *slot : nullptr;
}

void instruction_hook(lua_State* state, lua_Debug*) {
    ScriptContext* context = lua_context(state);
    if (!context) return;
    context->instructions += kHookInterval;
    if (context->instructions > g_instruction_limit) {
        context->timed_out = true;
        luaL_error(state, "script exceeded the %llu instruction limit",
                   static_cast<unsigned long long>(g_instruction_limit));
    }
}

// pcall message handler. It must not change the error VALUE (Redis distinguishes a raised table
// with an `err` field from a raised string), so it only records where the throw happened: the
// first frame with a real source line, skipping the C frames of redis.call.
int script_error_handler(lua_State* state) {
    ScriptContext* context = lua_context(state);
    if (context) {
        lua_Debug frame;
        for (int level = 1; level <= 24; level++) {
            if (!lua_getstack(state, level, &frame)) break;
            if (!lua_getinfo(state, "Sl", &frame)) break;
            if (frame.currentline > 0) { context->error_line = frame.currentline; break; }
        }
    }
    return 1;
}

bool command_name_is(const CommandSpec& spec, const char* expected) {
    return std::strcmp(spec.name, expected) == 0;
}

bool script_name_whitelisted(const CommandSpec& spec) {
    // Deliberately explicit: command-table shape alone does not prove determinism (SPOP and
    // HRANDFIELD are both single-key). New commands remain unavailable until this list is audited.
    static constexpr const char* allowed[] = {
        "APPEND", "BITCOUNT", "BITPOS", "DECR", "DECRBY", "DEL", "EXISTS",
        "GET", "GETBIT", "GETRANGE", "GETSET",
        "HDEL", "HEXISTS", "HGET", "HGETALL", "HINCRBY", "HINCRBYFLOAT", "HKEYS",
        "HLEN", "HMGET", "HSET", "HSETNX", "HSTRLEN", "HVALS",
        "INCR", "INCRBY", "INCRBYFLOAT",
        "LINDEX", "LINSERT", "LLEN", "LPOP", "LPOS", "LPUSH", "LPUSHX", "LRANGE",
        "LREM", "LSET", "LTRIM",
        "PFADD", "PERSIST", "RPOP", "RPUSH", "RPUSHX",
        "SADD", "SCARD", "SISMEMBER", "SMEMBERS", "SMISMEMBER", "SREM",
        "SET", "SETBIT", "SETNX", "SETRANGE", "STRLEN", "TOUCH", "TYPE", "UNLINK",
        "ZADD", "ZCARD", "ZCOUNT", "ZINCRBY", "ZLEXCOUNT", "ZPOPMAX", "ZPOPMIN",
        "ZRANGE", "ZRANGEBYLEX", "ZRANGEBYSCORE", "ZRANK", "ZREM", "ZREMRANGEBYLEX",
        "ZREMRANGEBYRANK", "ZREMRANGEBYSCORE", "ZREVRANGE", "ZREVRANGEBYLEX",
        "ZREVRANGEBYSCORE", "ZREVRANK", "ZSCORE",
    };
    for (const char* name : allowed) if (command_name_is(spec, name)) return true;
    return false;
}

bool script_command_allowed(const CommandSpec& spec, uint32_t argc) {
    const uint32_t forbidden = CmdFlags::Admin | CmdFlags::ConnLocal | CmdFlags::AllShards |
        CmdFlags::RandomShard | CmdFlags::CursorShard | CmdFlags::ConfigRoute |
        CmdFlags::ScriptRoute;
    if ((spec.flags & forbidden) || !script_name_whitelisted(spec)) return false;
    if (spec.flags & CmdFlags::MultiShard) {
        return argc == 2 && (command_name_is(spec, "DEL") || command_name_is(spec, "UNLINK") ||
                            command_name_is(spec, "EXISTS") || command_name_is(spec, "TOUCH"));
    }
    if (spec.first_key <= 0 || spec.last_key != spec.first_key || spec.key_step != 1) return false;
    return true;
}

bool mark_declared(ScriptContext& context, Slice key, uint8_t bits) {
    bool declared = false;
    for (uint32_t i = 0; i < context.key_count; i++) {
        if (context.parent->arg(context.key_first + i) != key) continue;
        declared = true;
        if (context.cross_access && i < context.cross_access_count)
            context.cross_access[i] |= bits;
    }
    return declared;
}

bool parse_decimal_line(const char* data, size_t size, size_t& pos, int64_t& value) {
    const size_t begin = pos;
    while (pos + 1 < size && !(data[pos] == '\r' && data[pos + 1] == '\n')) pos++;
    if (pos + 1 >= size || pos == begin) return false;
    const auto converted = std::from_chars(data + begin, data + pos, value);
    if (converted.ec != std::errc{} || converted.ptr != data + pos) return false;
    pos += 2;
    return true;
}

bool push_resp_value(lua_State* state, const char* data, size_t size, size_t& pos,
                     uint32_t depth, bool top, bool& server_error, std::string& error) {
    if (pos >= size || depth > kReplyMaxDepth) { error = "invalid nested command reply"; return false; }
    const char marker = data[pos++];
    if (marker == '+' || marker == '-') {
        const size_t begin = pos;
        while (pos + 1 < size && !(data[pos] == '\r' && data[pos + 1] == '\n')) pos++;
        if (pos + 1 >= size) { error = "invalid nested command reply"; return false; }
        const size_t length = pos - begin;
        pos += 2;
        if (marker == '-' && top) {
            server_error = true;
            error.assign(data + begin, length);
            return true;
        }
        lua_createtable(state, 0, 1);
        lua_pushlstring(state, data + begin, length);
        lua_setfield(state, -2, marker == '+' ? "ok" : "err");
        return true;
    }
    if (marker == ':') {
        int64_t value = 0;
        if (!parse_decimal_line(data, size, pos, value)) {
            error = "invalid integer command reply"; return false;
        }
        lua_pushnumber(state, static_cast<lua_Number>(value));
        return true;
    }
    if (marker == '_') {
        if (pos + 1 >= size || data[pos] != '\r' || data[pos + 1] != '\n') {
            error = "invalid null command reply"; return false;
        }
        pos += 2;
        lua_pushboolean(state, 0);
        return true;
    }
    if (marker == '#') {
        if (pos + 2 >= size || (data[pos] != 't' && data[pos] != 'f') ||
            data[pos + 1] != '\r' || data[pos + 2] != '\n') {
            error = "invalid boolean command reply"; return false;
        }
        lua_pushboolean(state, data[pos] == 't');
        pos += 3;
        return true;
    }
    if (marker == ',' || marker == '(') {
        const size_t begin = pos;
        while (pos + 1 < size && !(data[pos] == '\r' && data[pos + 1] == '\n')) pos++;
        if (pos + 1 >= size) { error = "invalid scalar command reply"; return false; }
        const size_t length = pos - begin;
        pos += 2;
        lua_createtable(state, 0, 1);
        if (marker == ',') {
            std::string text(data + begin, length);
            char* end = nullptr;
            const double value = std::strtod(text.c_str(), &end);
            if (end != text.c_str() + text.size()) {
                lua_pop(state, 1); error = "invalid double command reply"; return false;
            }
            lua_pushnumber(state, static_cast<lua_Number>(value));
            lua_setfield(state, -2, "double");
        } else {
            lua_pushlstring(state, data + begin, length);
            lua_setfield(state, -2, "big_number");
        }
        return true;
    }
    if (marker == '$') {
        int64_t length = 0;
        if (!parse_decimal_line(data, size, pos, length) || length < -1) {
            error = "invalid bulk command reply"; return false;
        }
        if (length == -1) { lua_pushboolean(state, 0); return true; }
        if (static_cast<uint64_t>(length) > size - pos ||
            static_cast<uint64_t>(length) + 2 > size - pos ||
            data[pos + length] != '\r' || data[pos + length + 1] != '\n') {
            error = "truncated bulk command reply"; return false;
        }
        lua_pushlstring(state, data + pos, static_cast<size_t>(length));
        pos += static_cast<size_t>(length) + 2;
        return true;
    }
    if (marker == '=') {
        int64_t length = 0;
        if (!parse_decimal_line(data, size, pos, length) || length < 4 ||
            static_cast<uint64_t>(length) + 2 > size - pos || data[pos + 3] != ':' ||
            data[pos + length] != '\r' || data[pos + length + 1] != '\n') {
            error = "invalid verbatim command reply"; return false;
        }
        lua_createtable(state, 0, 1);
        lua_createtable(state, 0, 2);
        lua_pushlstring(state, data + pos, 3);
        lua_setfield(state, -2, "format");
        lua_pushlstring(state, data + pos + 4, static_cast<size_t>(length - 4));
        lua_setfield(state, -2, "string");
        lua_setfield(state, -2, "verbatim_string");
        pos += static_cast<size_t>(length) + 2;
        return true;
    }
    if (marker == '*') {
        int64_t count = 0;
        if (!parse_decimal_line(data, size, pos, count) || count < -1 ||
            count > static_cast<int64_t>(kReplyMaxElements)) {
            error = "invalid array command reply"; return false;
        }
        if (count == -1) { lua_pushboolean(state, 0); return true; }
        lua_createtable(state, static_cast<int>(count), 0);
        for (int64_t i = 0; i < count; i++) {
            bool child_error = false;
            if (!push_resp_value(state, data, size, pos, depth + 1, false, child_error, error))
                return false;
            lua_rawseti(state, -2, static_cast<int>(i + 1));
        }
        return true;
    }
    if (marker == '%' || marker == '~') {
        int64_t count = 0;
        if (!parse_decimal_line(data, size, pos, count) || count < 0 ||
            count > static_cast<int64_t>(kReplyMaxElements)) {
            error = "invalid aggregate command reply"; return false;
        }
        lua_createtable(state, 0, 1);
        lua_createtable(state, 0, static_cast<int>(count));
        for (int64_t i = 0; i < count; i++) {
            bool child_error = false;
            if (!push_resp_value(state, data, size, pos, depth + 1, false,
                                 child_error, error)) return false;
            if (marker == '%') {
                if (!push_resp_value(state, data, size, pos, depth + 1, false,
                                     child_error, error)) return false;
            } else {
                lua_pushboolean(state, 1);
            }
            lua_rawset(state, -3);
        }
        lua_setfield(state, -2, marker == '%' ? "map" : "set");
        return true;
    }
    error = "unsupported nested command reply";
    return false;
}

int redis_dispatch(lua_State* state, bool protected_call) {
    auto* slot = static_cast<ScriptContext**>(lua_touserdata(state, lua_upvalueindex(1)));
    ScriptContext* context = slot ? *slot : nullptr;
    char deferred_error[2100] = {};
    bool failed = false;
    {
        const int argc = lua_gettop(state);
        Op nested;
        if (!context || argc < 1) {
            std::snprintf(deferred_error, sizeof(deferred_error), "ERR redis.call requires a command name");
            failed = true;
        } else {
            for (int i = 1; i <= argc && !failed; i++) {
                const int type = lua_type(state, i);
                if (type != LUA_TSTRING && type != LUA_TNUMBER) {
                    std::snprintf(deferred_error, sizeof(deferred_error),
                                  "ERR Lua redis() command arguments must be strings or integers");
                    failed = true;
                    break;
                }
                size_t length = 0;
                const char* value = lua_tolstring(state, i, &length);
                if (length > UINT32_MAX || !nested.push_arg(
                        Slice(value, static_cast<uint32_t>(length)))) {
                    std::snprintf(deferred_error, sizeof(deferred_error), "ERR out of memory");
                    failed = true;
                }
            }
        }

        const CommandSpec* spec = failed ? nullptr : command_lookup(nested.cmd_name());
        if (!failed && (!spec || !command_arity_ok(*spec, nested.argc()))) {
            std::snprintf(deferred_error, sizeof(deferred_error),
                          spec ? "ERR wrong number of arguments for command from script"
                               : "ERR Unknown Redis command called from script");
            failed = true;
        }
        if (!failed && !script_command_allowed(*spec, nested.argc())) {
            std::snprintf(deferred_error, sizeof(deferred_error),
                          "ERR command '%s' is not allowed from scripts", spec->name);
            failed = true;
        }
        // The read-only gate. CommandSpec::flags carries Write on every keyspace mutation
        // (verified against the registry rows, e.g. PERSIST/DEL/SETBIT are Write, TOUCH/EXISTS
        // are Readonly), so this needs no second whitelist.
        if (!failed && context->readonly && (spec->flags & CmdFlags::Write)) {
            std::snprintf(deferred_error, sizeof(deferred_error),
                          "ERR Write commands are not allowed from read-only scripts.");
            g_ro_rejections.fetch_add(1, std::memory_order_relaxed);
            failed = true;
        }
        if (!failed) {
            const uint32_t key_arg = static_cast<uint32_t>(spec->first_key);
            const Slice key = nested.arg(key_arg);
            if (!mark_declared(*context, key, 1)) {
                std::snprintf(deferred_error, sizeof(deferred_error),
                              "ERR Script attempted to access an undeclared key");
                failed = true;
            } else {
                if (context->notify) spec = command_notify_variant(spec);
                nested.spec = spec;
                nested.hash = FlatStore::hash_key(key);
                nested.shard = context->shard->id();
                if (context->script_resp == 3) nested.mark_resp3();
                if (context->notify)
                    notify_execute_source(*context->shard, nested,
                                          context->call_index++ * 0x10000u);
                if ((spec->flags & CmdFlags::DenyOom) &&
                    !context->shard->store().budget_admit(key)) {
                    reply_maxmemory_oom(nested);
                } else {
                    spec->handler(*context->shard, nested);
                }
                if (context->notify)
                    notify_execute_source(*context->shard, *context->parent, 0);
                // A Write row that answered without an error has already changed the keyspace.
                // Nothing later in this activation may reverse it, so record it here: this is the
                // quantity a regression asserts on, not "no exception was thrown".
                if (spec->flags & CmdFlags::Write) {
                    const bool errored = !nested.zc_ptr && nested.reply.size() &&
                                         nested.reply.data()[0] == '-';
                    if (!errored) {
                        mark_declared(*context, key, 2);
                        context->effect_writes++;
                        g_effect_writes.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                if (nested.zc_ptr) {
                    try {
                        std::string borrowed(nested.zc_ptr, nested.zc_len);
                        context->shard->store().unborrow(nested.zc_ptr);
                        nested.zc_ptr = nullptr;
                        lua_pushlstring(state, borrowed.data(), borrowed.size());
                    } catch (const std::bad_alloc&) {
                        context->shard->store().unborrow(nested.zc_ptr);
                        nested.zc_ptr = nullptr;
                        std::snprintf(deferred_error, sizeof(deferred_error), "ERR out of memory");
                        failed = true;
                    }
                } else {
                    size_t pos = 0;
                    bool server_error = false;
                    std::string parse_error;
                    if (!push_resp_value(state, nested.reply.data(), nested.reply.size(), pos, 0,
                                         true, server_error, parse_error) ||
                        (!server_error && pos != nested.reply.size())) {
                        std::snprintf(deferred_error, sizeof(deferred_error), "ERR %s",
                                      parse_error.empty() ? "invalid nested command reply"
                                                          : parse_error.c_str());
                        failed = true;
                    } else if (server_error) {
                        const std::string safe = clean_error(parse_error.data(), parse_error.size());
                        std::snprintf(deferred_error, sizeof(deferred_error), "%s", safe.c_str());
                        failed = true;
                    }
                }
            }
        }
    }

    if (!failed) return 1;
    lua_createtable(state, 0, 1);
    lua_pushstring(state, deferred_error);
    lua_setfield(state, -2, "err");
    if (protected_call) return 1;
    // RAISE A TABLE, not a string. Lua prepends `user_script:N: ` to a raised string; Redis's
    // wire error for a failed redis.call has no position prefix (the position appears only in the
    // trailing " ... on @user_script:N." clause), and a table carries none.
    return lua_error(state);
}

int redis_call(lua_State* state) { return redis_dispatch(state, false); }
int redis_pcall(lua_State* state) { return redis_dispatch(state, true); }

int redis_setresp(lua_State* state) {
    auto* slot = static_cast<ScriptContext**>(lua_touserdata(state, lua_upvalueindex(1)));
    ScriptContext* context = slot ? *slot : nullptr;
    if (!context) return luaL_error(state, "redis.setresp() called outside a script");
    if (lua_gettop(state) != 1)
        return luaL_error(state, "redis.setresp() requires one argument.");
    const int version = static_cast<int>(lua_tonumber(state, 1));
    if (!lua_isnumber(state, 1) || (version != 2 && version != 3))
        return luaL_error(state, "RESP version must be 2 or 3.");
    context->script_resp = static_cast<uint8_t>(version);
    return 0;
}

void remove_global(lua_State* state, const char* name) {
    lua_pushnil(state);
    lua_setglobal(state, name);
}

// State reuse must not let one script's globals leak into the next call, so undeclared global
// writes and reads are errors (redis semantics; redis rejects both on its own reused
// interpreter). rawset can still bypass this, exactly as it can in redis — scripts are an
// admin-trust surface.
int protected_global_newindex(lua_State* state) {
    return luaL_error(state, "Script attempted to create global variable '%s'",
                      lua_tostring(state, 2));
}

int protected_global_index(lua_State* state) {
    return luaL_error(state, "Script attempted to access nonexistent global variable '%s'",
                      lua_tostring(state, 2));
}

// Installs `name` into the globals table bypassing the protection metatable; the value to
// install must be on top of the stack.
void set_global_raw(lua_State* state, const char* name) {
    lua_pushvalue(state, LUA_GLOBALSINDEX);
    lua_insert(state, -2);
    lua_pushstring(state, name);
    lua_insert(state, -2);
    lua_rawset(state, -3);
    lua_pop(state, 1);
}

void open_library(lua_State* state, lua_CFunction open, const char* name) {
    lua_pushcfunction(state, open);
    lua_pushstring(state, name);
    lua_call(state, 1, 0);
}

// Built ONCE per thread state: libraries, strips, the redis table (closures over the stable
// per-state context slot), the hook registry entry, and — last — the globals protection.
void create_sandbox_static(lua_State* state, ScriptContext** slot) {
    open_library(state, luaopen_base, "");
    open_library(state, luaopen_table, LUA_TABLIBNAME);
    open_library(state, luaopen_string, LUA_STRLIBNAME);
    open_library(state, luaopen_math, LUA_MATHLIBNAME);
    remove_global(state, "dofile");
    remove_global(state, "load");
    remove_global(state, "loadfile");
    remove_global(state, "loadstring");
    remove_global(state, "print");
    remove_global(state, "newproxy");
    remove_global(state, "collectgarbage");
    remove_global(state, "coroutine");
    lua_getglobal(state, "string");
    lua_pushnil(state); lua_setfield(state, -2, "dump");
    lua_pop(state, 1);
    lua_getglobal(state, "math");
    lua_pushnil(state); lua_setfield(state, -2, "random");
    lua_pushnil(state); lua_setfield(state, -2, "randomseed");
    lua_pop(state, 1);

    lua_createtable(state, 0, 3);
    lua_pushlightuserdata(state, slot);
    lua_pushcclosure(state, redis_call, 1);
    lua_setfield(state, -2, "call");
    lua_pushlightuserdata(state, slot);
    lua_pushcclosure(state, redis_pcall, 1);
    lua_setfield(state, -2, "pcall");
    lua_pushlightuserdata(state, slot);
    lua_pushcclosure(state, redis_setresp, 1);
    lua_setfield(state, -2, "setresp");
    lua_setglobal(state, "redis");

    lua_pushlightuserdata(state, &g_hook_context_key);
    lua_pushlightuserdata(state, slot);
    lua_settable(state, LUA_REGISTRYINDEX);

    // Globals protection LAST, so the setup writes above bypass nothing. KEYS/ARGV are installed
    // per call via set_global_raw.
    lua_pushvalue(state, LUA_GLOBALSINDEX);
    lua_createtable(state, 0, 2);
    lua_pushcfunction(state, protected_global_newindex);
    lua_setfield(state, -2, "__newindex");
    lua_pushcfunction(state, protected_global_index);
    lua_setfield(state, -2, "__index");
    lua_setmetatable(state, -2);
    lua_pop(state, 1);
}

// Rebuilt per call: only the tables whose contents depend on the op. Leaves the two tables on the
// stack (keys first, then args) for the FCALL parameter style; the EVAL style pops them into the
// KEYS/ARGV globals.
void push_call_tables(lua_State* state, ScriptContext& context) {
    lua_createtable(state, static_cast<int>(context.key_count), 0);
    for (uint32_t i = 0; i < context.key_count; i++) {
        const Slice key = context.parent->arg(context.key_first + i);
        lua_pushlstring(state, key.p, key.n);
        lua_rawseti(state, -2, static_cast<int>(i + 1));
    }

    const uint32_t argv_first = context.key_first + context.key_count;
    const uint32_t argv_count = context.parent->argc() - argv_first;
    lua_createtable(state, static_cast<int>(argv_count), 0);
    for (uint32_t i = 0; i < argv_count; i++) {
        const Slice value = context.parent->arg(argv_first + i);
        lua_pushlstring(state, value.p, value.n);
        lua_rawseti(state, -2, static_cast<int>(i + 1));
    }
}

void bind_call_globals(lua_State* state, ScriptContext& context) {
    push_call_tables(state, context);
    set_global_raw(state, "ARGV");
    set_global_raw(state, "KEYS");
}

bool append_lua_result(lua_State* state, int index, SmallBuf<kInlineReply>& output,
                       uint32_t depth, uint32_t& elements, std::string& error,
                       bool outer_resp3, uint8_t script_resp) {
    if (depth > kReplyMaxDepth) { error = "script reply nesting is too deep"; return false; }
    index = index < 0 ? lua_gettop(state) + index + 1 : index;
    const int type = lua_type(state, index);
    if (type == LUA_TNIL) {
        reply_null(output, outer_resp3);
        return true;
    }
    if (type == LUA_TBOOLEAN) {
        const bool value = lua_toboolean(state, index);
        if (script_resp == 3) reply_bool(output, value, outer_resp3);
        else if (value) reply_int(output, 1);
        else reply_null(output, outer_resp3);
        return true;
    }
    if (type == LUA_TNUMBER) {
        const lua_Number number = lua_tonumber(state, index);
        if (!std::isfinite(number) || number < static_cast<lua_Number>(LLONG_MIN) ||
            number > static_cast<lua_Number>(LLONG_MAX)) {
            error = "script returned a non-integer number"; return false;
        }
        reply_int(output, static_cast<long long>(number));
        return true;
    }
    if (type == LUA_TSTRING) {
        size_t length = 0;
        const char* value = lua_tolstring(state, index, &length);
        if (length > UINT32_MAX) { error = "script reply is too large"; return false; }
        reply_bulk(output, Slice(value, static_cast<uint32_t>(length)));
        return true;
    }
    if (type != LUA_TTABLE) { reply_null(output, outer_resp3); return true; }

    lua_getfield(state, index, "err");
    if (!lua_isnil(state, -1)) {
        size_t length = 0;
        const char* value = lua_tolstring(state, -1, &length);
        if (!value) { lua_pop(state, 1); error = "invalid redis error table"; return false; }
        output.push_back('-'); output.append(value, length); output.append("\r\n", 2);
        lua_pop(state, 1);
        return true;
    }
    lua_pop(state, 1);
    lua_getfield(state, index, "ok");
    if (!lua_isnil(state, -1)) {
        size_t length = 0;
        const char* value = lua_tolstring(state, -1, &length);
        if (!value) { lua_pop(state, 1); error = "invalid redis status table"; return false; }
        output.push_back('+'); output.append(value, length); output.append("\r\n", 2);
        lua_pop(state, 1);
        return true;
    }
    lua_pop(state, 1);

    lua_getfield(state, index, "double");
    if (lua_isnumber(state, -1)) {
        const double value = static_cast<double>(lua_tonumber(state, -1));
        lua_pop(state, 1);
        reply_double(output, value, outer_resp3);
        return true;
    }
    lua_pop(state, 1);

    lua_getfield(state, index, "big_number");
    if (lua_isstring(state, -1)) {
        size_t length = 0;
        const char* value = lua_tolstring(state, -1, &length);
        std::string safe(value, length);
        for (char& ch : safe) if (ch == '\r' || ch == '\n') ch = ' ';
        lua_pop(state, 1);
        reply_bignum(output, Slice(safe.data(), static_cast<uint32_t>(safe.size())), outer_resp3);
        return true;
    }
    lua_pop(state, 1);

    lua_getfield(state, index, "verbatim_string");
    if (lua_istable(state, -1)) {
        const int verbatim = lua_gettop(state);
        lua_getfield(state, verbatim, "format");
        size_t format_length = 0;
        const char* format = lua_tolstring(state, -1, &format_length);
        lua_getfield(state, verbatim, "string");
        size_t value_length = 0;
        const char* value = lua_tolstring(state, -1, &value_length);
        if (format && format_length >= 3 && value && value_length <= UINT32_MAX) {
            char ext[3] = {format[0], format[1], format[2]};
            reply_verbatim(output, Slice(value, static_cast<uint32_t>(value_length)), ext,
                           outer_resp3);
            lua_pop(state, 3);
            return true;
        }
        lua_pop(state, 2);
    }
    lua_pop(state, 1);

    lua_getfield(state, index, "map");
    if (lua_istable(state, -1)) {
        const int map = lua_gettop(state);
        uint32_t count = 0;
        lua_pushnil(state);
        while (lua_next(state, map)) {
            lua_pop(state, 1);
            if (++count > kReplyMaxElements) {
                error = "script reply has too many elements"; return false;
            }
        }
        reply_map_header(output, count, outer_resp3);
        lua_pushnil(state);
        while (lua_next(state, map)) {
            lua_pushvalue(state, -2);
            if (!append_lua_result(state, -1, output, depth + 1, elements, error,
                                   outer_resp3, script_resp)) return false;
            lua_pop(state, 1);
            lua_pushvalue(state, -1);
            if (!append_lua_result(state, -1, output, depth + 1, elements, error,
                                   outer_resp3, script_resp)) return false;
            lua_pop(state, 2);  // value copy, then original value; retain the iteration key
        }
        lua_pop(state, 1);
        return true;
    }
    lua_pop(state, 1);

    lua_getfield(state, index, "set");
    if (lua_istable(state, -1)) {
        const int set = lua_gettop(state);
        uint32_t count = 0;
        lua_pushnil(state);
        while (lua_next(state, set)) {
            lua_pop(state, 1);
            if (++count > kReplyMaxElements) {
                error = "script reply has too many elements"; return false;
            }
        }
        reply_set_header(output, count, outer_resp3);
        lua_pushnil(state);
        while (lua_next(state, set)) {
            lua_pushvalue(state, -2);
            if (!append_lua_result(state, -1, output, depth + 1, elements, error,
                                   outer_resp3, script_resp)) return false;
            lua_pop(state, 2);  // key copy, then original value; retain the iteration key
        }
        lua_pop(state, 1);
        return true;
    }
    lua_pop(state, 1);

    uint32_t count = 0;
    for (;;) {
        if (++elements > kReplyMaxElements) { error = "script reply has too many elements"; return false; }
        lua_rawgeti(state, index, static_cast<int>(count + 1));
        const bool stop = lua_isnil(state, -1);
        lua_pop(state, 1);
        if (stop) break;
        count++;
    }
    reply_array_header(output, count);
    for (uint32_t i = 0; i < count; i++) {
        lua_rawgeti(state, index, static_cast<int>(i + 1));
        const bool ok = append_lua_result(state, -1, output, depth + 1, elements, error,
                                          outer_resp3, script_resp);
        lua_pop(state, 1);
        if (!ok) return false;
    }
    return true;
}

void script_note_failure(uint32_t effect_writes) {
    if (effect_writes) g_failed_after_effects.fetch_add(1, std::memory_order_relaxed);
}

// Builds the wire error for a failed activation, matching Redis 7 byte for byte:
//   <message> script: <tag>, on @<chunk>:<line>.
// A raised table with an `err` field contributes its string unchanged (that is how redis.call and
// error({err=...}) travel); any other raised value is a Lua string that already carries its own
// `user_script:N: ` position prefix and is presented as an ERR.
std::string script_runtime_error(lua_State* state, const ScriptContext& context,
                                 const ScriptInvocation& call) {
    std::string message;
    bool from_table = false;
    if (lua_istable(state, -1)) {
        lua_getfield(state, -1, "err");
        if (lua_isstring(state, -1)) {
            size_t length = 0;
            const char* text = lua_tolstring(state, -1, &length);
            message = clean_error(text, length);
            from_table = true;
        }
        lua_pop(state, 1);
    }
    if (!from_table) {
        size_t length = 0;
        const char* text = lua_tolstring(state, -1, &length);
        message = "ERR " + clean_error(text ? text : "runtime error", text ? length : 13);
    }
    char tail[1024];
    std::snprintf(tail, sizeof(tail), " script: %.*s, on @%s:%d.",
                  static_cast<int>(std::min<uint32_t>(call.tag.n, 512)), call.tag.p, call.chunk,
                  context.error_line > 0 ? context.error_line : 1);
    message += tail;
    return message;
}

}  // namespace

lua_State* script_thread_state() {
    LuaEngine& engine = t_lua_engine;
    if (engine.state &&
        engine.flush_generation != g_script_flushes.load(std::memory_order_acquire)) {
        // SCRIPT FLUSH invalidates every thread's compiled-chunk cache; rebuilding the whole
        // state is the simple correct form and FLUSH is admin-cold. It also drops the FUNCTION
        // library materialization, whose generation stamp lives in the same registry.
        lua_close(engine.state);
        engine.state = nullptr;
    }
    if (!engine.state) {
        engine.state = luaL_newstate();
        if (!engine.state) return nullptr;
        engine.flush_generation = g_script_flushes.load(std::memory_order_acquire);
        create_sandbox_static(engine.state, &engine.current);
        lua_newtable(engine.state);
        lua_setfield(engine.state, LUA_REGISTRYINDEX, "tomo_chunks");
        g_state_rebuilds.fetch_add(1, std::memory_order_relaxed);
    }
    return engine.state;
}

lua_State* script_new_sandbox_state() {
    static ScriptContext* null_slot = nullptr;
    lua_State* state = luaL_newstate();
    if (!state) return nullptr;
    create_sandbox_static(state, &null_slot);
    return state;
}

std::string script_clean_error(const char* text, size_t length) {
    return clean_error(text, length);
}

void script_execute(Shard& shard, Op& op, const ScriptInvocation& call) {
    LuaEngine& engine = t_lua_engine;
    lua_State* state = engine.state;
    ScriptContext context{&shard, &op, call.key_first, call.key_count};
    if (t_script_cross.op == &op) {
        context.cross_access = t_script_cross.access;
        context.cross_access_count = t_script_cross.count;
    }
    context.notify = call.notify;
    context.readonly = call.readonly;
    engine.current = &context;

    ScriptEvictionGuard eviction_guard(shard);

    // The message handler must sit BELOW the callable for lua_pcall; the callable arrived on top.
    lua_pushcfunction(state, script_error_handler);
    lua_insert(state, -2);
    const int handler_index = lua_gettop(state) - 1;
    int arguments = 0;
    if (call.style == ScriptArgStyle::Params) {
        push_call_tables(state, context);
        arguments = 2;
    } else {
        bind_call_globals(state, context);
    }

    lua_sethook(state, instruction_hook, LUA_MASKCOUNT, kHookInterval);
    const int status = lua_pcall(state, arguments, 1, handler_index);
    lua_sethook(state, nullptr, 0, 0);
    if (status != 0) {
        const std::string error = script_runtime_error(state, context, call);
        lua_settop(state, 0);
        engine.current = nullptr;
        // Effects already applied STAND, and the notifications they fired stand with them. Both
        // atomic modes take this same arm, which is what makes the two modes byte-identical to the
        // oracle for a failing activation.
        script_note_failure(context.effect_writes);
        if (context.timed_out) {
            char busy[96];
            std::snprintf(busy, sizeof(busy), "script exceeded the %llu instruction limit",
                          static_cast<unsigned long long>(g_instruction_limit));
            reply_text_error(op, "BUSY", busy);
        } else reply_raw_error(op, error);
        return;
    }

    SmallBuf<kInlineReply> result;
    std::string conversion_error;
    uint32_t elements = 0;
    const bool converted = append_lua_result(
        state, -1, result, 0, elements, conversion_error, op.resp3(), context.script_resp);
    lua_settop(state, 0);
    engine.current = nullptr;
    if (!converted) {
        script_note_failure(context.effect_writes);
        reply_text_error(op, "ERR", std::string("Error running script: ") + conversion_error);
        return;
    }
    op.sink().append(result.data(), result.size());
}

void script_cross_access_begin(Op& op, uint8_t* access, uint32_t count) {
    if (t_script_cross.op) std::abort();
    t_script_cross = ScriptCrossBinding{&op, access, count};
}

void script_cross_access_end() {
    t_script_cross = ScriptCrossBinding{};
}

namespace {

void run_eval(Shard& shard, Op& op, Slice source, bool cache_source, Slice sha_hint,
              bool notify, bool readonly) {
    if (source.n > kScriptMaxBytes) {
        reply_text_error(op, "ERR", "script exceeds the 1 MiB limit");
        return;
    }
    if (source.n && static_cast<uint8_t>(source.p[0]) == 0x1b) {
        reply_text_error(op, "ERR",
                         "Error compiling script (new function): binary chunks are not allowed");
        return;
    }
    uint32_t key_first = 0, key_count = 0;
    if (!command_script_key_range(op, key_first, key_count)) {
        reply_text_error(op, "ERR", "invalid script key declaration");
        return;
    }

    lua_State* state = script_thread_state();
    if (!state) { reply_text_error(op, "ERR", "out of memory"); return; }

    // Fetch the compiled chunk by sha, compiling at most once per thread per script.
    std::string sha_storage;
    Slice sha = sha_hint;
    if (!sha.n) {
        sha_storage = sha1_hex(source);
        sha = Slice(sha_storage.data(), static_cast<uint32_t>(sha_storage.size()));
    }
    lua_getfield(state, LUA_REGISTRYINDEX, "tomo_chunks");
    lua_pushlstring(state, sha.p, sha.n);
    lua_rawget(state, -2);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        g_compile_misses.fetch_add(1, std::memory_order_relaxed);
        const int compiled = luaL_loadbuffer(state, source.p, source.n, kEvalChunkName);
        if (compiled != 0) {
            size_t length = 0;
            const char* text = lua_tolstring(state, -1, &length);
            const std::string error =
                clean_error(text ? text : "compile error", text ? length : 13);
            lua_settop(state, 0);
            reply_text_error(op, "ERR",
                             std::string("Error compiling script (new function): ") + error);
            return;
        }
        lua_pushlstring(state, sha.p, sha.n);
        lua_pushvalue(state, -2);
        lua_rawset(state, -4);
    } else {
        g_compile_hits.fetch_add(1, std::memory_order_relaxed);
    }
    lua_remove(state, -2);   // drop the chunk table, leaving the function on top
    if (cache_source) {
        std::string ignored;
        if (!g_scripts.store(source, ignored)) {
            lua_settop(state, 0);
            reply_text_error(op, "ERR", "out of memory caching script");
            return;
        }
    }

    ScriptInvocation call;
    call.key_first = key_first;
    call.key_count = key_count;
    call.style = ScriptArgStyle::Globals;
    call.readonly = readonly;
    call.notify = notify;
    call.chunk = "user_script";
    call.tag = sha;
    script_execute(shard, op, call);
}

template <bool kNotify, bool kSha, bool kReadonly>
void cmd_eval(Shard& shard, Op& op) {
    if constexpr (!kSha) {
        // cache_source = false: routing already stored this source, in program order.
        run_eval(shard, op, op.arg(1), false, Slice(), kNotify, kReadonly);
    } else {
        std::string source;
        if (!g_scripts.find(op.arg(1), source)) {
            reply_text_error(op, "NOSCRIPT", "No matching script. Please use EVAL.");
            return;
        }
        run_eval(shard, op, Slice(source.data(), static_cast<uint32_t>(source.size())), false,
                 op.arg(1), kNotify, kReadonly);
    }
}

void reply_script_wrong_args(Op& op, const char* sub) {
    char text[96];
    std::snprintf(text, sizeof(text), "wrong number of arguments for 'script|%s' command", sub);
    reply_text_error(op, "ERR", text);
}

// Deliberately our own wording rather than Redis's help block: the subcommand set differs (no
// interactive debugger) and the Redis text is source we do not copy.
void reply_script_help(Op& op) {
    static constexpr const char* lines[] = {
        "SCRIPT <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
        "DEBUG (YES|SYNC|NO)",
        "    Only NO is supported; this server has no interactive script debugger.",
        "EXISTS <sha1> [<sha1> ...]",
        "    Return 1/0 per sha1 for scripts present in the process-wide script store.",
        "FLUSH [ASYNC|SYNC]",
        "    Drop every cached script. Both modes are synchronous here.",
        "KILL",
        "    Terminate the running script. Scripts are bounded by an instruction limit and",
        "    always terminate on their own, so this reports NOTBUSY.",
        "LOAD <script>",
        "    Compile a script and add it to the script store, returning its sha1.",
        "HELP",
        "    Print this help.",
    };
    reply_array_header(op.sink(), sizeof(lines) / sizeof(lines[0]));
    for (const char* line : lines) reply_simple(op.sink(), line);
}

void cmd_script(Shard&, Op& op) {
    const Slice subcommand = op.arg(1);
    if (subcommand.eq_icase("load")) {
        if (op.argc() != 3) { reply_script_wrong_args(op, "load"); return; }
        std::string compile_error;
        if (!validate_source(op.arg(2), compile_error)) {
            reply_text_error(op, "ERR",
                             std::string("Error compiling script (new function): ") + compile_error);
            return;
        }
        std::string sha;
        if (!g_scripts.store(op.arg(2), sha)) {
            reply_text_error(op, "ERR", "out of memory caching script");
            return;
        }
        reply_bulk(op.sink(), Slice(sha.data(), static_cast<uint32_t>(sha.size())));
        return;
    }
    if (subcommand.eq_icase("exists")) {
        if (op.argc() < 3) { reply_script_wrong_args(op, "exists"); return; }
        reply_array_header(op.sink(), op.argc() - 2);
        for (uint32_t i = 2; i < op.argc(); i++) reply_int(op.sink(), g_scripts.exists(op.arg(i)));
        return;
    }
    if (subcommand.eq_icase("flush")) {
        if (op.argc() > 3 || (op.argc() == 3 &&
            !(op.arg(2).eq_icase("sync") || op.arg(2).eq_icase("async")))) {
            reply_text_error(op, "ERR", "SCRIPT FLUSH only support SYNC|ASYNC option");
            return;
        }
        g_scripts.flush();
        g_script_flushes.fetch_add(1, std::memory_order_release);
        reply_ok(op.sink());
        return;
    }
    if (subcommand.eq_icase("kill")) {
        if (op.argc() != 2) { reply_script_wrong_args(op, "kill"); return; }
        // Scripts run to completion inside one owner task under a hard instruction limit, so no
        // activation is ever observable from another connection. NOTBUSY is therefore not a
        // placeholder — it is the only reachable answer.
        reply_text_error(op, "NOTBUSY", "No scripts in execution right now.");
        return;
    }
    if (subcommand.eq_icase("debug")) {
        if (op.argc() != 3) { reply_script_wrong_args(op, "debug"); return; }
        if (op.arg(2).eq_icase("no")) { reply_ok(op.sink()); return; }
        if (op.arg(2).eq_icase("yes") || op.arg(2).eq_icase("sync")) {
            reply_text_error(op, "ERR",
                             "SCRIPT DEBUG YES|SYNC is not supported: this server has no "
                             "interactive script debugger");
            return;
        }
        reply_text_error(op, "ERR", "Use SCRIPT DEBUG YES/SYNC/NO");
        return;
    }
    if (subcommand.eq_icase("help")) {
        reply_script_help(op);
        return;
    }
    char text[128];
    std::snprintf(text, sizeof(text), "unknown subcommand '%.*s'. Try SCRIPT HELP.",
                  static_cast<int>(std::min<uint32_t>(subcommand.n, 64)), subcommand.p);
    reply_text_error(op, "ERR", text);
}

static const CommandSpec kTable[] = {
    // name       min max flags                                      handler     first last step
    {"EVAL",       3, -1, CmdFlags::Write | CmdFlags::CursorShard | CmdFlags::ScriptRoute |
                              CmdFlags::MultiShard,
                cmd_eval<false, false, false>, 3, -1, 1,
                notify_handler<cmd_eval<true, false, false>>},
    {"EVALSHA",    3, -1, CmdFlags::Write | CmdFlags::CursorShard | CmdFlags::ScriptRoute |
                              CmdFlags::MultiShard,
                cmd_eval<false, true, false>, 3, -1, 1,
                notify_handler<cmd_eval<true, true, false>>},
    // The _RO forms are Readonly, exactly as Redis flags them: no AOF post-image pass and no
    // WATCH invalidation for an activation that cannot mutate. The atomic MVCC promotion still
    // runs — it is keyed on the declared range, not on write-ness, which is what RYOW needs.
    {"EVAL_RO",    3, -1, CmdFlags::Readonly | CmdFlags::CursorShard | CmdFlags::ScriptRoute |
                              CmdFlags::MultiShard,
                cmd_eval<false, false, true>, 3, -1, 1,
                notify_handler<cmd_eval<true, false, true>>},
    {"EVALSHA_RO", 3, -1, CmdFlags::Readonly | CmdFlags::CursorShard | CmdFlags::ScriptRoute |
                              CmdFlags::MultiShard,
                cmd_eval<false, true, true>, 3, -1, 1,
                notify_handler<cmd_eval<true, true, true>>},
    {"SCRIPT",     2, -1, CmdFlags::ConnLocal | CmdFlags::OrderedLocal | CmdFlags::Admin,
                cmd_script,    0,  0, 0},
};

}  // namespace

void scripting_bind_server(Server* server) {
    g_script_server = server;
    if (server) g_instruction_limit = server->cfg().script_instruction_limit;
    if (!g_instruction_limit) g_instruction_limit = UINT64_MAX;   // 0 = unlimited
}

ScriptStats script_stats() {
    ScriptStats stats;
    stats.cached_scripts = g_scripts.size();
    stats.compile_hits = g_compile_hits.load(std::memory_order_relaxed);
    stats.compile_misses = g_compile_misses.load(std::memory_order_relaxed);
    stats.flush_generation = g_script_flushes.load(std::memory_order_relaxed);
    stats.state_rebuilds = g_state_rebuilds.load(std::memory_order_relaxed);
    stats.ro_rejections = g_ro_rejections.load(std::memory_order_relaxed);
    stats.effect_writes = g_effect_writes.load(std::memory_order_relaxed);
    stats.failed_after_effects = g_failed_after_effects.load(std::memory_order_relaxed);
    return stats;
}

bool command_script_key_range(const Op& op, uint32_t& first, uint32_t& count) {
    first = 3;
    count = 0;
    if (op.argc() < 3) return false;
    bool negative = false;
    if (!parse_nonnegative(op.arg(2), count, negative) || negative) return false;
    return count <= op.argc() - first;
}

// THE SCRIPT STORE IS MAINTAINED ON THE IO THREAD, in connection parse order.
//
// It has to be. The store is a process-wide side effect that one command produces (EVAL) and
// another consumes (EVALSHA, SCRIPT EXISTS). Executors are per-shard and run concurrently, so two
// ops of one pipelined batch whose declared keys land on different owners execute in an order the
// ROB only restores for their REPLIES. Producing the store entry on the executor therefore let a
// LATER EVAL arm an EARLIER EVALSHA — a real reordering, caught by the differ at 4236 ops.
// Routing is the one stage that sees a connection's commands in order, so both halves live here:
// EVAL validates and stores, EVALSHA resolves. (SCRIPT/FUNCTION close the same loop from the
// other side with CmdFlags::OrderedLocal.)
//
// Compilation on the IO thread costs one throwaway lua_State per DISTINCT script, process-wide:
// the second EVAL of the same source finds its sha already stored and skips it, exactly as
// SCRIPT LOAD has always done.
static bool script_route_store(Op& op) {
    const char* name = op.spec ? op.spec->name : "";
    if (!std::strncmp(name, "EVALSHA", 7)) {
        if (g_scripts.exists(op.arg(1))) return true;
        reply_text_error(op, "NOSCRIPT", "No matching script. Please use EVAL.");
        return false;
    }
    if (std::strncmp(name, "EVAL", 4)) return true;      // FCALL / FCALL_RO own no source
    const Slice source = op.arg(1);
    if (source.n > kScriptMaxBytes) {
        reply_text_error(op, "ERR", "script exceeds the 1 MiB limit");
        return false;
    }
    const std::string sha = sha1_hex(source);
    if (g_scripts.exists(Slice(sha.data(), static_cast<uint32_t>(sha.size())))) return true;
    std::string compile_error;
    if (!validate_source(source, compile_error)) {
        reply_text_error(op, "ERR",
                         std::string("Error compiling script (new function): ") + compile_error);
        return false;
    }
    std::string ignored;
    if (!g_scripts.store(source, ignored)) {
        reply_text_error(op, "ERR", "out of memory caching script");
        return false;
    }
    return true;
}

bool command_validate_script_route(Op& op, uint32_t& first, uint32_t& count) {
    first = 0;
    count = 0;
    bool negative = false;
    if (!parse_nonnegative(op.arg(2), count, negative)) {
        // Same grammar, two Redis messages: EVAL reports the generic integer error, FCALL its own.
        reply_text_error(op, "ERR", function_is_fcall(op)
            ? "Bad number of keys provided" : "value is not an integer or out of range");
        return false;
    }
    if (negative) {
        reply_text_error(op, "ERR", "Number of keys can't be negative");
        return false;
    }
    first = 3;
    if (count > op.argc() - first) {
        reply_text_error(op, "ERR", "Number of keys can't be greater than number of args");
        return false;
    }
    // After the numkeys grammar, before routing: Redis reports a bad key count ahead of a compile
    // error, and this keeps that precedence.
    return script_route_store(op);
}

bool command_prepare_script_route(Server& server, Op& op) {
    uint32_t first = 0, count = 0;
    if (!command_validate_script_route(op, first, count)) return false;
    if (!count) {
        op.hash = 0;
        op.shard = 0;
        op.mark_local_xshard();
        return true;
    }
    op.hash = FlatStore::hash_key(op.arg(first));
    op.shard = server.router().shard_of(op.hash);
    for (uint32_t i = 1; i < count; i++) {
        const uint64_t hash = FlatStore::hash_key(op.arg(first + i));
        if (server.router().shard_of(hash) != op.shard) {
            reply_text_error(op, "CROSSSLOT", "Keys in request don't hash to the same slot");
            return false;
        }
    }
    op.mark_local_xshard();
    return true;
}

CommandTable scripting_command_table() {
    return {kTable, sizeof(kTable) / sizeof(kTable[0])};
}

}  // namespace tomo

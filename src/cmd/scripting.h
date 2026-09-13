// scripting.h — the seam between the Lua interpreter (scripting.cc) and the FUNCTION library
// (functions.cc).
//
// The interpreter is a per-thread singleton owned by scripting.cc: one persistent lua_State per
// executor thread, its sandbox built once, chunks cached by sha in the Lua registry. functions.cc
// owns the process-wide library registry (control-plane state, cold-path mutex) and borrows the
// interpreter through the two calls below. Nothing else crosses: the ScriptContext, the undo log,
// the eviction guard and the reply converter all stay private to scripting.cc.
//
// The Lua amalgamation is compiled exactly once, inside scripting.cc. Other translation units
// include the vendored PUBLIC headers only and link against those definitions.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "../base/slice.h"

struct lua_State;

namespace tomo {

class Shard;
class Op;

enum class ScriptArgStyle : uint8_t {
    Globals,   // EVAL: KEYS/ARGV are installed as globals; the chunk is called with no arguments.
    Params,    // FCALL: the callback is invoked as callback(keys, args).
};

// One script/function activation. `tag` is the identity Redis prints in its error tail
// ("... script: <tag>, on @<chunk>:<line>."): the sha1 for EVAL, the function name for FCALL.
struct ScriptInvocation {
    uint32_t       key_first = 0;
    uint32_t       key_count = 0;
    ScriptArgStyle style     = ScriptArgStyle::Globals;
    bool           readonly  = false;   // reject Write commands from redis.call/pcall
    bool           notify    = false;   // armed keyspace-notification variant entered
    const char*    chunk     = "user_script";
    Slice          tag;
};

// The calling thread's persistent interpreter, rebuilt when SCRIPT FLUSH moved the generation.
// nullptr on allocation failure. Must be called BEFORE anything is pushed on the returned state:
// a rebuild closes the previous one.
lua_State* script_thread_state();

// A fresh sandboxed state with no context bound — redis.call is present but inert. Used by
// FUNCTION LOAD to validate and to harvest registration metadata off the executor threads. The
// caller owns it and must lua_close() it.
lua_State* script_new_sandbox_state();

// Runs the callable on top of `state` (consumed) under the shard-owner contract: instruction hook,
// atomic undo/rollback, eviction suspension, notification source hooks and RESP conversion. Writes
// the reply — or the Redis-shaped error — into `op`. The stack is left empty either way.
void script_execute(Shard& shard, Op& op, const ScriptInvocation& call);

// Flattens a Lua error string into one RESP-safe line (CR/LF/NUL to spaces, 2 KiB cap).
std::string script_clean_error(const char* text, size_t length);

struct ScriptStats {
    uint64_t cached_scripts    = 0;
    uint64_t compile_hits      = 0;   // per-thread compiled-chunk cache hits
    uint64_t compile_misses    = 0;   // sha compiled into a thread state for the first time
    uint64_t flush_generation  = 0;   // SCRIPT FLUSH count; per-thread states rebuild on change
    uint64_t state_rebuilds    = 0;   // interpreter states constructed (boot + post-FLUSH)
    uint64_t ro_rejections     = 0;   // write commands refused inside a read-only activation
    // Keyspace effects applied from inside activations (nested Write rows that answered without an
    // error), and activations that FAILED with at least one such effect standing. The second is the
    // interleave the removed atomic undo log used to reverse; a test that leaves it at zero has not
    // touched the guarded path.
    uint64_t effect_writes        = 0;
    uint64_t failed_after_effects = 0;
};
ScriptStats script_stats();

struct FunctionStats {
    uint64_t libraries       = 0;
    uint64_t functions       = 0;
    uint64_t generation      = 0;   // LOAD/DELETE/FLUSH/RESTORE count
    uint64_t calls           = 0;   // FCALL/FCALL_RO activations that reached a callback
    uint64_t thread_rebuilds = 0;   // per-thread library re-materializations
    uint64_t ro_rejections   = 0;   // FCALL_RO refused for a function without no-writes
};
FunctionStats function_stats();

// Whether argv[0] of this op is one of the FCALL forms. Routing shares EVAL's numkeys grammar but
// Redis reports a different message for a malformed count.
bool function_is_fcall(const Op& op);

// Cold bridge used by the cross-owner scatter coordinator. Each declared-key byte receives bit 0
// when a nested command reads it and bit 1 when a nested write completes without an error. The
// binding is thread-local and scoped to one outer Op, so ordinary single-owner activations do not
// test or allocate cross-script state.
void script_cross_access_begin(Op& op, uint8_t* access, uint32_t count);
void script_cross_access_end();

}  // namespace tomo

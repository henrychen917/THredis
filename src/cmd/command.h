// command.h — the command table.
//
// A command is data, not a class. The dispatch path does one lookup and one indirect call; adding a
// type later means adding rows, not changing the machinery. Handlers write RESP straight into
// op.reply, so nothing here knows what a value is.
//
// KEY POSITION IS METADATA, not a parse-time discovery. The IO thread must know which arguments are
// keys BEFORE it can route, because routing happens before execution. v1 covers only single-key
// commands (first_key == 1); the multi-key forms need the key range so a command can be split
// across shards and reassembled, which is why the fields exist now rather than being added later.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include "../base/slice.h"

namespace tomo {

class Shard;
class Op;

// Static executor scheduling cost. This is deliberately coarse: the policy needs a cheap verb
// class, not a runtime estimate from argv or clocks. Point includes GET and unarmed SET; the armed
// registry promotes writes by one saturated class. SmallMulti is the simple fanout/vector family;
// Long covers combining, range, scan, and other whole-value work.
enum class CommandLengthClass : uint8_t {
    Point = 0,
    SmallMulti = 1,
    Long = 2,
    Count = 3,
};

struct CmdFlags {
    static constexpr uint32_t Write     = 1u << 0;   // mutates the keyspace
    static constexpr uint32_t Readonly  = 1u << 1;
    static constexpr uint32_t Admin     = 1u << 2;
    static constexpr uint32_t ConnLocal = 1u << 3;   // answered on the IO thread; never dispatched
    static constexpr uint32_t AllShards = 1u << 4;   // one public op, one owner task per shard
    static constexpr uint32_t RandomShard = 1u << 5; // keyless op routed by the IO thread's PRNG
    static constexpr uint32_t CursorShard = 1u << 6; // shard id is encoded in argv[1]'s cursor
    static constexpr uint32_t ConfigRoute = 1u << 7; // GET is IO-local; SET fans out as control work
    // Growth commands, gated by the pre-execution maxmemory check (redis DENYOOM): if the shard is
    // over budget and eviction cannot bring it under, the command is refused with the exact OOM
    // reply BEFORE its handler runs. DEL/expiry/read commands stay ungated so an over-budget shard
    // can always be drained.
    static constexpr uint32_t DenyOom = 1u << 8;
    static constexpr uint32_t MultiShard = 1u << 9;  // command-specific scatter/gather lowering
    // Logically read-only command with a physical value update that needs snapshot pre-image
    // capture. PFCOUNT uses this for Redis's cached-cardinality bytes; it remains reported as
    // readonly and is not maxmemory admission gated.
    static constexpr uint32_t SnapshotWrite = 1u << 10;
    // EVAL/EVALSHA discover their declared KEYS range from argv[2]. Validation and routing happen
    // on IO; the resulting ordinary task executes the interpreter on exactly one shard owner.
    static constexpr uint32_t ScriptRoute = 1u << 11;
    static constexpr uint32_t PubSub = 1u << 12;       // IO-owned async command; never touches a shard
    // A blocking collection command is lowered before ordinary scatter routing.  Its issued ROB
    // slot is also a connection parse barrier until an owner supplies data or a timeout reply.
    static constexpr uint32_t Blocking = 1u << 13;
    static constexpr uint32_t Transaction = 1u << 14; // MULTI/EXEC/WATCH controls, IO-owned
    static constexpr uint32_t AclExempt = 1u << 15;   // AUTH/HELLO/RESET/QUIT bypass ACL command bits
    // Set only on the registry's armed shadow rows. It consumes no Op storage and lets the
    // inherently-slower scatter/blocking/MULTI engines select their recording path without
    // putting a notification branch in ExLoop::execute. (Bit 15 went to AclExempt in the ACL
    // merge; the shadow-row flag moved to bit 16 — it is registry-internal, nothing serializes it.)
    static constexpr uint32_t NotifySelected = 1u << 16;
    // XREAD discovers its key half after the STREAMS token. The validated range is consumed by
    // ACL/MULTI/scatter without pretending the trailing ID half contains keys.
    static constexpr uint32_t StreamRoute = 1u << 17;
    // A connection-local command that must still observe same-connection PROGRAM order against
    // ops already dispatched to executors. ConnLocal handlers normally answer at parse time, so a
    // pipelined `EVAL x` + `SCRIPT EXISTS sha(x)` would ask the script store before the EVAL that
    // populates it had run. Commands carrying this bit wait for the ROB to drain first — the same
    // barrier the blocking lowering uses, and one predicted-false test inside a branch the
    // GET/SET path never enters. Set on SCRIPT and FUNCTION: both read or mutate state that
    // in-flight EVAL/EVALSHA/FCALL activations on the same connection produce or consume.
    static constexpr uint32_t OrderedLocal = 1u << 18;
    // CLIENT/MONITOR/RESET: connection-control commands whose implementation lives in the cold
    // climon translation unit. Marking them in the registry replaces a name comparison on the
    // ConnLocal path with a flag test on a word the dispatcher already holds. (Landed alongside
    // OrderedLocal, which claimed bit 18 first — merge trains assign flag bits, not lanes.)
    static constexpr uint32_t Climon = 1u << 19;
    // Container commands whose FIRST ARGUMENT decides whether a key is present at all: OBJECT
    // ENCODING <key> and XGROUP CREATE <key> ... route by argv[2], while their HELP arms have no
    // key. The row keeps the truthful first_key so ACL/MULTI still gate the keyed forms (both
    // bound their walk by argc), and rides CursorShard so the existing IO special-route hook
    // resolves the shard before dispatch. No new branch reaches the GET/SET path: the hook is the
    // one SCAN/EVAL/XREAD already pay for.
    // (Third lane to claim bit 18 this wave — reassigned to 20 at the train.)
    static constexpr uint32_t SubcmdRoute = 1u << 20;
    // WAIT on an unsatisfied replica count owns a reply deadline on the connection's IO thread.
    // It remains ConnLocal -- no shard is involved -- but the IO dispatcher publishes its ROB
    // slot unfinished and completes it from a cold deadline list.  Commands without this bit do
    // not enter that machinery; the test lives inside the already-cold ConnLocal branch.
    static constexpr uint32_t DeferredLocal = 1u << 21;
    // Administrative execution constraints inherited from the shipped fork's FLIP row. They are
    // named flags (rather than command-name checks) so scripts/MULTI/loading keep one policy gate.
    static constexpr uint32_t NoScript = 1u << 22;
    static constexpr uint32_t NoMulti = 1u << 23;
    static constexpr uint32_t NoAsyncLoading = 1u << 24;
    // FLIP publishes an unfinished connection-local ROB slot and is completed by IoLoop's staged
    // coordinator. No other command enters that control path.
    static constexpr uint32_t FlipAsync = 1u << 25;
    // Exact hot-decode class for the fused parsing-thread read lane. The flag is deliberately
    // narrower than Readonly: it admits only plain GET and MGET, then applies the remaining
    // connection/store gates to every routed key before either command enters the lane.
    static constexpr uint32_t ReadLocalEligible = 1u << 26;
    // DEBUG SLEEP alone publishes a connection-local unfinished ROB slot and completes it from
    // IoLoop's cold timer list. This bit does not make DEBUG generally deferred: all other DEBUG
    // subcommands retain their synchronous control-plane path.
    static constexpr uint32_t DebugSleep = 1u << 27;
};

using CmdHandler = void (*)(Shard&, Op&);

// Registry placeholder pair for commands implemented by scatter/gather lowering.
void cmd_xshard_only(Shard& shard, Op& op);
void cmd_xshard_only_notify(Shard& shard, Op& op);

struct CommandSpec {
    const char* name;
    // Inclusive counts, including the command itself. max_arity == -1 means unbounded. Keeping both
    // bounds in the row avoids type handlers growing their own subtly different arity parsers.
    int32_t     min_arity;
    int32_t     max_arity;
    uint32_t    flags;
    // Boot-stamped scheduler metadata in the existing alignment hole before handler.
    uint8_t     length_class = 0;
    CmdHandler  handler;

    // Key range within argv: [first_key, last_key] stepping by key_step.
    // last_key == -1 means "to the end of argv" (MGET, DEL, ...).
    int16_t     first_key;
    int16_t     last_key;
    int16_t     key_step;

    // Dense boot-assigned id. Family tables leave this zero; commands.cc copies their rows into
    // registry-owned storage and assigns the final value before any server thread starts.
    uint16_t    id = 0;

    // Cold registry tail. `handler` is always the clean specialization. The IO thread selects a
    // shadow row whose handler is `handler_notify` when its pass-local armed cache is true.
    CmdHandler  handler_notify = nullptr;

    constexpr CommandSpec(const char* name_, int32_t min_arity_, int32_t max_arity_,
                          uint32_t flags_, CmdHandler handler_, int16_t first_key_,
                          int16_t last_key_, int16_t key_step_,
                          CmdHandler handler_notify_ = nullptr)
        : name(name_), min_arity(min_arity_), max_arity(max_arity_), flags(flags_),
          length_class(0), handler(handler_), first_key(first_key_), last_key(last_key_),
          key_step(key_step_),
          handler_notify(handler_notify_ ? handler_notify_ :
                         (handler_ == cmd_xshard_only ? cmd_xshard_only_notify : handler_)) {}
};

// 48 = the ACL audit's measured 40 plus the notify v2 handler_notify tail pointer. Registry rows
// are cold read-only data; the lock exists to catch accidental growth, not to forbid deliberate.
static_assert(sizeof(CommandSpec) == 48);

inline CommandLengthClass command_length_class(const CommandSpec& spec) {
    return static_cast<CommandLengthClass>(spec.length_class);
}

inline bool command_is_read_local_mget(const CommandSpec& spec) {
    constexpr uint32_t kMgetClass = CmdFlags::ReadLocalEligible | CmdFlags::MultiShard;
    return (spec.flags & kMgetClass) == kMgetClass;
}

struct CommandTable {
    const CommandSpec* specs;
    size_t             size;
};

// Redis registers container subcommands as commands in their own right. TomoKV keeps one registry
// row per public container, so cold handlers use these tables to recover the same per-subcommand
// arity names without growing CommandSpec or adding work to ordinary GET/SET dispatch.
enum class SubcommandArityError : uint8_t {
    WrongArgs,
    UnknownOrWrong,
    Syntax,
};

struct SubcommandArity {
    const char*          name;
    int16_t              min_arity;
    int16_t              max_arity;
    SubcommandArityError error = SubcommandArityError::WrongArgs;
};

bool command_validate_subcommand(Op& op, const char* container,
                                 const SubcommandArity* table, size_t count);
// Resolve argv[0]|argv[1] in the generated Redis metadata, validate that child's advertised
// arity, and return its first-key argument (zero for a keyless arm). Returns false after writing
// the complete unknown-subcommand or child-arity reply.
bool command_validate_container_subcommand(Op& op, const CommandSpec& spec,
                                           int16_t& first_key);
// The pre-ACL arity gate rejects a known child with a bad generated arity, but deliberately lets
// an unknown child continue: Redis resolves that as an unknown command after arity lookup, so an
// unauthenticated `XGROUP x` reaches NOAUTH.
bool command_reply_container_subcommand_arity(Op& op, const CommandSpec& spec);
void command_reply_subcommand_wrong_args(Op& op, const char* container,
                                         const char* subcommand);
// The outer registry still owns each container's broad bound so malformed requests fail before
// ACL/MULTI handling. This rare-path hook replaces only that already-taken generic error.
bool command_reply_container_outer_arity(Op& op, const CommandSpec& spec);

// Every family owns its table; the registry calls every type table plus the server/admin table.
CommandTable string_command_table();
CommandTable hash_command_table();
CommandTable hash_ttl_command_table();
CommandTable list_command_table();
CommandTable set_command_table();
CommandTable zset_command_table();
CommandTable zset_ops_command_table();
CommandTable geo_command_table();
CommandTable stream_command_table();
CommandTable stream_group_command_table();
CommandTable server_command_table();
CommandTable scripting_command_table();
CommandTable functions_command_table();
CommandTable server_tail_command_table();
CommandTable slowlog_command_table();
CommandTable lcs_command_table();
CommandTable cmdgap_command_table();
CommandTable pfdebug_command_table();

// Built once before threads start. Lookup hashes the uppercase-normalized bytes into an open-
// addressed table; the load factor is capped at 1/2 so ordinary command names land in one probe.
bool command_registry_init(bool tls_enabled, bool fused_mode = false,
                           bool read_local_armed = false);

// Clean registry rows for the verbs command_lookup resolves inline. command_registry_init stamps
// them from the same rows the hash table indexes and re-checks the two against each other; they
// stay null before that, so a pre-init lookup is still "unknown", exactly as the hash probe says.
// Read on every dispatch and written only at boot, so it owns its cache line: no .bss neighbour
// that is written at runtime can false-share the GET pointer load.
struct alignas(64) HotCommandSpecs {
    const CommandSpec* get  = nullptr;
    const CommandSpec* set  = nullptr;
    const CommandSpec* del  = nullptr;
    const CommandSpec* mget = nullptr;
    const CommandSpec* mset = nullptr;
    const CommandSpec* incr = nullptr;
};
extern HotCommandSpecs g_hot_command_specs;

// The cold entry: open-addressed probe of the uppercase-normalized name. Callers use
// command_lookup below, which reaches this only for a name the inline resolve did not claim.
const CommandSpec* command_lookup_registry(Slice name);

// Little-endian packed verb key with every byte OR 0x20. For an ASCII letter, OR 0x20 has exactly
// its two spellings as preimages ('G' 0x47 and 'g' 0x67 both give 0x67) and nothing else, and the
// length is matched first, so key equality against a canonical verb is byte-exact case-insensitive
// equality — the relation command_lookup_registry's ascii_upper compare implements — and no other
// byte string, binary or otherwise, can alias a hot verb. A 3-byte key carries 0x20 in its top byte.
inline constexpr uint32_t command_verb_key(char a, char b, char c, char d = ' ') {
    return (static_cast<uint32_t>(static_cast<uint8_t>(a)) |
            (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
            (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
            (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24)) | 0x20202020u;
}

// Command lookup. GET/SET/DEL and MGET/MSET/INCR resolve here, in registers, from the length the
// parser already established: no call, no frame, no canary, no stack round-trip. Every other verb,
// every other length, and the pre-init state take the out-of-line probe. The 3-byte key is built
// from an in-bounds 16-bit load plus one byte rather than a 3-byte memcpy: GCC lowers that memcpy
// to two narrow stack stores and a wider reload, which forces a frame and a store-forwarding stall
// on every GET.
// Always inlined so the parse loop never depends on the unit-growth heuristics (see the t_string.o
// inliner note in the Makefile).
__attribute__((always_inline)) inline const CommandSpec* command_lookup(Slice name) {
    if (name.n == 3) {
        uint16_t lo;
        std::memcpy(&lo, name.p, 2);    // one 16-bit load plus one byte: both inside the name
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        lo = __builtin_bswap16(lo);
#endif
        const uint32_t key =
            (static_cast<uint32_t>(lo) |
             (static_cast<uint32_t>(static_cast<uint8_t>(name.p[2])) << 16)) | 0x20202020u;
        if (key == command_verb_key('G', 'E', 'T')) return g_hot_command_specs.get;
        if (key == command_verb_key('S', 'E', 'T')) return g_hot_command_specs.set;
        if (key == command_verb_key('D', 'E', 'L')) return g_hot_command_specs.del;
    } else if (name.n == 4) {
        uint32_t key;
        std::memcpy(&key, name.p, 4);   // one 32-bit load, never widened past the name
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        key = __builtin_bswap32(key);
#endif
        key |= 0x20202020u;
        if (key == command_verb_key('M', 'G', 'E', 'T')) return g_hot_command_specs.mget;
        if (key == command_verb_key('M', 'S', 'E', 'T')) return g_hot_command_specs.mset;
        if (key == command_verb_key('I', 'N', 'C', 'R')) return g_hot_command_specs.incr;
    }
    return command_lookup_registry(name);
}
inline bool command_arity_ok(const CommandSpec& spec, uint32_t argc) {
    return argc >= static_cast<uint32_t>(spec.min_arity) &&
           (spec.max_arity < 0 || argc <= static_cast<uint32_t>(spec.max_arity));
}
uint32_t command_registry_size();
const CommandSpec* command_registry_at(uint32_t id);
uint64_t command_acl_category_mask(const CommandSpec& spec);
const CommandSpec* command_notify_variant(const CommandSpec* spec);
const CommandSpec* command_tls_variant(const CommandSpec* spec);

// GET is the only ordinary executor handler that can create a FlatStore borrow. TLS selects these
// copy-only variants on the IO parse specialization, so the plaintext handler has no transport
// load or branch at all.
void cmd_get_tls(Shard&, Op&);
void cmd_get_tls_notify(Shard&, Op&);

// Larger single-key string/keyspace commands live with the shared owner helpers instead of the
// latency-sensitive string translation unit. Their registry rows remain in the string family.
void cmd_bitfield(Shard&, Op&);
void cmd_bitfield_notify(Shard&, Op&);
void cmd_bitfield_ro(Shard&, Op&);
void cmd_bitfield_ro_notify(Shard&, Op&);
class Server;
void cmd_flip_unavailable(Shard&, Op&);
class Client;
class ThreadCtx;
struct Op;
struct PubSubEvent;
void reply_maxmemory_oom(Op& op);  // defined in t_string.cc, tomo:: linkage
// Lets the connection-local admin commands read published per-shard counters.
void command_bind_server(Server* s);

// ConnLocal handlers retain the small, uniform CmdHandler ABI. IO binds this thread-local context
// only around the synchronous handler call; executors never see a Client or a socket.
void command_set_local_context(Client* client, ThreadCtx* thread);

// CLIENT metadata is owned by the connection's IO thread and keyed by process-unique id.  Scatter
// messages never carry Client pointers; they ask each IO owner to inspect only its own clients.
void command_client_connected(Client* client, const char* addr, const char* laddr,
                              bool unix_socket, uint64_t now_ms);
void command_client_disconnected(Client* client);
// Runtime IO-owner handoff moves the thread-local CLIENT catalog as an extracted map node. The
// opaque handle owns that node between source and destination; no metadata field is reconstructed.
void* command_client_migration_extract(Client* client);
bool command_client_migration_install(void* catalog);
void command_client_migration_discard(void* catalog);
bool command_client_migration_reserve(uint32_t extra);
void command_client_set_subscriptions(Client* client, uint32_t channels, uint32_t patterns,
                                      uint32_t shard_channels);
std::string command_client_info_line(const Client& client, uint64_t now_ms);
bool command_client_filter_match(const Client& client, const PubSubEvent& event,
                                 uint64_t now_ms);
bool command_client_set_name(Client* client, Slice name);
std::string command_client_name(const Client* client);
bool command_client_set_info(Client* client, Slice option, Slice value);
// Compatibility metadata only: reported by CLIENT INFO/LIST, not enforced. Redis applies this to
// client output-buffer eviction, a mechanism TomoKV does not implement.
void command_client_set_no_evict(Client* client, bool enabled);
void command_client_set_no_touch(Client* client, bool enabled);
// MONITOR feed lines and CLIENT INFO share the owner-catalog peer address.
std::string command_client_addr(const Client* client);
// CLIENT INFO's redir= field is owned by the tracking lane, which lives in the io loop.
void command_client_set_tracking_view(Client* client, bool on, int64_t redirect);
void command_client_reset_meta(Client* client);
// CLIENT subcommand arity error, shared by climon.cc and tracking.cc.
void climon_wrong_args(Op& op, const char* subcommand);
// Process-wide id -> live Client directory. The pointer is read only under the directory mutex and
// only to acquire-load Client::ifid_thread(), the connection ownership edge. Besides CLIENT
// UNBLOCK/TRACKING REDIRECT, stale notification deliveries use it after a migration has already
// moved the old owner's local catalog node.
void command_client_directory_add(Client* client, uint32_t io);
void command_client_directory_move(uint64_t id, uint32_t io);
void command_client_directory_remove(uint64_t id);
bool command_client_directory_find(uint64_t id, uint32_t& io);

// Redis glob matching is case-sensitive except for the explicitly insensitive command-name
// filters. ACL and pub/sub callers must keep the default: broadening either match is observable.
bool command_glob_match(Slice pattern, Slice text, bool nocase = false);
// SCAN-family cursors use Redis's string2ull grammar, not canonical command integers.
bool command_parse_scan_cursor(Slice text, uint64_t& cursor);

// Special routing helpers. Validation writes a complete error reply into op on failure.
bool command_prepare_scan_route(Server& server, Op& op);
// SubcmdRoute resolution for containers whose generated child row decides whether a key exists.
// Returns false after writing a complete reply into op — the IO caller then retires it without
// dispatching.
bool command_prepare_subcmd_route(Server& server, Op& op);
// Lets the server-tail translation unit reach the bound Server without duplicating the binding.
Server* command_server();
// The IO thread currently running a ConnLocal handler, for the few commands that must talk to
// the loop itself (SHUTDOWN's ring pokes). Null outside a ConnLocal call.
ThreadCtx* command_local_thread();
// Live CONFIG table as (name, value) pairs, for CONFIG REWRITE.
void command_config_snapshot(std::vector<std::pair<std::string, std::string>>& out);
// CONFIG RESETSTAT. The resettable INFO counters are per-shard and per-thread single-writer
// values, so they are not zeroed in place: INFO subtracts a baseline this call captures. That
// keeps every counter's single-writer property intact and adds no cross-thread write.
void command_config_resetstat();
bool command_validate_all_shards(Op& op);
bool command_config_routes_all_shards(Op& op);
bool command_validate_config_set(Op& op);
bool command_prepare_script_route(Server& server, Op& op);
bool command_validate_script_route(Op& op, uint32_t& first, uint32_t& count);
bool command_script_key_range(const Op& op, uint32_t& first, uint32_t& count);
void scripting_bind_server(Server* server);

// Registry placeholder for commands whose implementation is the multi-shard lowering itself.
// Reaching it as an ordinary single-shard handler is an internal routing error.
}  // namespace tomo

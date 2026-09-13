// Server-less battery for the exact scheduler used by both executor call sites. Real Clients,
// Tasks and published ROB slots; no alternate scheduling implementation or timing window.
// The gate runs this under ASAN + UBSAN. A FIFO/no-op mutant must fail the permutation oracle.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>
#include "src/core/config.h"
#include "src/core/reorder.h"

// This fixture never creates a MULTI session or executes a command. Keep the only out-of-line
// Client destructor dependency explicit, and fail if the fixture ever strays into that subsystem.
namespace tomo {
void multi_session_destroy(MultiSession* session) { if (session) std::abort(); }
}

namespace {
using namespace tomo;

[[noreturn]] void fail(const char* what) {
    std::fprintf(stderr, "reorder battery: FAIL: %s\n", what);
    std::exit(1);
}
void require(bool condition, const char* what) { if (!condition) fail(what); }
void unused_handler(Shard&, Op&) { fail("fixture executed a command"); }

constexpr CommandSpec spec(const char* name, CommandLengthClass length, uint32_t flags) {
    CommandSpec out(name, 2, 2, flags, unused_handler, 1, 1, 1, unused_handler);
    out.set_length_class(length);
    return out;
}
constexpr auto point = spec("GET", CommandLengthClass::Point, CmdFlags::Readonly);
constexpr auto small = spec("MGET", CommandLengthClass::SmallMulti,
                            CmdFlags::Readonly | CmdFlags::MultiShard);
constexpr auto long_op = spec("BITCOUNT", CommandLengthClass::Long, CmdFlags::Readonly);
constexpr auto admin = spec("DEBUG", CommandLengthClass::Point, CmdFlags::Admin);
constexpr auto other_point = spec("STRLEN", CommandLengthClass::Point, CmdFlags::Readonly);

static_assert(sizeof(Op) == 336 && sizeof(Client) == 1984 && sizeof(ThreadCtx) == 1408);
static_assert(sizeof(Shard) == 1440 && sizeof(FlatStore) == 944 && sizeof(Rob<64>) == 192);
static_assert(sizeof(AtomicEntry) == 144 && sizeof(Config) == 624);
static_assert(sizeof(CommandSpec) == 48 && sizeof(Task) == 32);
static_assert((point.flags & CmdFlags::ReorderClasses) == CmdFlags::ReorderPoint);
static_assert((small.flags & CmdFlags::ReorderClasses) == CmdFlags::ReorderPoint << 1);
static_assert((long_op.flags & CmdFlags::ReorderClasses) == CmdFlags::ReorderPoint << 2);
static_assert((admin.flags & CmdFlags::ReorderClasses) == 0);

Task publish(Client& client, const CommandSpec& command, bool hazard = false) {
    auto& rob = client.rob();
    const uint64_t id = rob.dispatch_id();
    Op* op = rob.acquire();
    require(op != nullptr, "fixture exceeded the real ROB window");
    op->spec = &command;
    op->shard = 7;
    if (hazard) op->mark_atomic_hazard();
    op->state.store(OpState::Issued, std::memory_order_release);
    rob.publish();
    require(id >= rob.flush_id() && id - rob.flush_id() < kRobWindow,
            "fixture did not publish a live ROB task");
    require(rob.at(id).state.load(std::memory_order_acquire) == OpState::Issued,
            "fixture did not issue the task");
    Task task(&client, id, 7, nullptr);
    task.enqueue_us_low = static_cast<uint32_t>(id + 101);
    return task;
}

void retire_all(Client& client) {
    auto& rob = client.rob();
    while (!rob.quiesced()) {
        rob.at(rob.flush_id()).state.store(OpState::Done, std::memory_order_release);
        require(rob.drain([](Op&) {}) == 1, "fixture did not retire exactly one ROB slot");
    }
}

bool same(const Task& a, const Task& b) {
    return a.client == b.client && a.op_id == b.op_id && a.shard == b.shard &&
           a.scatter == b.scatter && a.enqueue_us_low == b.enqueue_us_low;
}

uint32_t permutations = 0;
template <size_t Capacity>
void verify(Task (&tasks)[Capacity], const std::vector<Task>& expected, bool must_move) {
    require(expected.size() <= Capacity, "oracle exceeds its input capacity");
    const std::vector<Task> before(tasks, tasks + Capacity);
    const ReorderResult witness = ex_schedule_batch(tasks, static_cast<uint32_t>(expected.size()));
    bool moved = false;
    for (size_t i = 0; i < expected.size(); i++) {
        require(same(tasks[i], expected[i]), "actual permutation differs from exact oracle");
        moved |= !same(tasks[i], before[i]);
        // Independently check membership, uniqueness and order for each connection, even when
        // the exact oracle changes in the future. Task stamps/shards/scatter pointers move too.
        uint32_t matches = 0;
        for (size_t j = 0; j < expected.size(); j++) matches += same(tasks[i], before[j]);
        require(matches == 1, "a task was lost, duplicated or partially copied");
        for (size_t j = 0; j < i; j++)
            if (tasks[i].client && tasks[i].client == tasks[j].client)
                require(tasks[j].op_id < tasks[i].op_id, "per-connection order inverted");
    }
    for (size_t i = expected.size(); i < Capacity; i++)
        require(same(tasks[i], before[i]), "scheduler touched the unused batch suffix");
    require(moved == must_move, "required reordering did not fire (or FIFO control moved)");
    require((witness.permuted_runs != 0) == moved, "permutation telemetry differs from actual tasks");
    require(witness.permuted_runs <= witness.multi_client_runs, "permutation without a multi-client run");
    permutations += moved;
}

// Every connection contributes its live head: a long request is gathered first, short requests
// behind it must pass it. Sweep every n through each real capacity, including 31/32/33/127/128.
// At n=128 all clients are distinct, so the half-full connection table necessarily occupies
// more than one 64-bit word, independent of allocator addresses or hash collisions.
template <size_t Capacity>
void heads() {
    for (uint32_t n = 2; n <= Capacity; n++) {
        Task tasks[Capacity];
        std::vector<std::unique_ptr<Client>> clients;
        std::vector<Task> expected;
        for (uint32_t i = 0; i < n; i++) {
            clients.push_back(std::make_unique<Client>(-1));
            tasks[i] = publish(*clients.back(), i ? point : long_op);
            require(clients.back()->rob().in_flight() == 1 &&
                    tasks[i].op_id == clients.back()->rob().flush_id(),
                    "mixed-client head-rank state was not entered");
            uint8_t length = 255;
            require(ex_sched_candidate(tasks[i], length) &&
                    length == static_cast<uint8_t>(i ? CommandLengthClass::Point :
                                                      CommandLengthClass::Long),
                    "mixed-length eligible run was not entered");
        }
        for (uint32_t i = 1; i < n; i++) expected.push_back(tasks[i]);
        expected.push_back(tasks[0]);
        verify(tasks, expected, true);
        require(tasks[0].client == clients[1].get(), "short head did not pass the long head");
        for (auto& client : clients) retire_all(*client);
    }
    std::printf("  heads: n=2..%zu, exact short-before-long permutations\n", Capacity);
}

// Two connections fill the whole batch (64 live operations each at capacity 128). The first
// connection's long head widens its later short operations; no younger own task may overtake it.
// Four complete fills/retirements also cross ROB wraps without fixing flush_id at zero.
template <size_t Capacity>
void pipelines() {
    static_assert(Capacity / 2 <= kRobWindow);
    Client a(-1), b(-1);
    for (uint32_t round = 0; round < 4; round++) {
        Task tasks[Capacity];
        std::vector<Task> expected;
        for (uint32_t i = 0; i < Capacity / 2; i++)
            tasks[i] = publish(a, i ? point : long_op);
        for (uint32_t i = 0; i < Capacity / 2; i++)
            tasks[Capacity / 2 + i] = publish(b, point);
        require(a.rob().in_flight() + b.rob().in_flight() == Capacity,
                "maximum pipeline batch was not entered");
        for (uint32_t i = 0; i < Capacity / 2; i++) {
            expected.push_back(tasks[Capacity / 2 + i]);
            expected.push_back(tasks[i]);
        }
        verify(tasks, expected, true);
        retire_all(a);
        retire_all(b);
    }
    require(a.rob().flush_id() >= kRobWindow && b.rob().flush_id() >= kRobWindow,
            "ROB wrap state was not entered");
    std::printf("  pipelines: %zu tasks, prefix cost and connection order across ROB wraps\n", Capacity);
}

// Put an ineligible task inside the large batch. The complete eligible suffix must still be
// scheduled, even when begin is already greater than 32. A 32-task clamp fails the exact oracle.
template <size_t Capacity>
void barriers() {
    constexpr uint32_t barrier_at = Capacity == 128 ? 32 : 16;
    for (uint32_t kind = 0; kind < 3; kind++) {
        Task tasks[Capacity];
        std::vector<std::unique_ptr<Client>> clients;
        for (uint32_t i = 0; i < Capacity; i++) {
            clients.push_back(std::make_unique<Client>(-1));
            tasks[i] = publish(*clients.back(), i == barrier_at ? admin :
                               i % 2 ? point : long_op);
        }
        if (kind == 1) tasks[barrier_at].client = nullptr;
        if (kind == 2) {
            // Opaque tag is inspected only for non-nullness; never a ScatterState dereference.
            tasks[barrier_at].scatter = reinterpret_cast<ScatterState*>(clients.back().get());
            clients[barrier_at]->rob().at(tasks[barrier_at].op_id).spec = &point;
        }
        uint8_t length = 255;
        require(!ex_sched_candidate(tasks[barrier_at], length), "barrier state was not entered");
        std::vector<Task> expected;
        auto append_run = [&](uint32_t begin, uint32_t end) {
            for (uint32_t i = begin; i < end; i++) if (i % 2) expected.push_back(tasks[i]);
            for (uint32_t i = begin; i < end; i++) if (!(i % 2)) expected.push_back(tasks[i]);
        };
        append_run(0, barrier_at);
        expected.push_back(tasks[barrier_at]);
        append_run(barrier_at + 1, Capacity);
        verify(tasks, expected, true);
        for (auto& client : clients) retire_all(*client);
    }
    std::printf("  barriers: capacity %zu, both complete runs preserved and reordered\n", Capacity);
}

void fifo_controls() {
    Client a(-1), b(-1);
    Task tasks[kGenthreadPipelineExBatchOps];
    verify(tasks, {}, false);
    tasks[0] = publish(a, point);
    verify(tasks, {tasks[0]}, false);
    for (uint32_t i = 1; i < kRobWindow; i++) tasks[i] = publish(a, i % 2 ? long_op : point);
    require(a.rob().full(), "single-client full-window control was not entered");
    verify(tasks, std::vector<Task>(tasks, tasks + kRobWindow), false);
    retire_all(a);
    tasks[0] = publish(a, point);
    tasks[1] = publish(b, point);
    verify(tasks, {tasks[0], tasks[1]}, false); // homogeneous equal-rank degeneration
    retire_all(a);
    retire_all(b);
    tasks[0] = publish(a, point);
    tasks[1] = publish(b, long_op);
    verify(tasks, {tasks[0], tasks[1]}, false); // already selected bucket order
    retire_all(a);
    retire_all(b);
    std::puts("  controls: empty/single task, one full client, homogeneous and already ordered FIFO");
}

void hidden_predecessors() {
    {
        Client a(-1), b(-1);
        Task tasks[kGenthreadExBatchOps];
        (void)publish(a, point); // live but absent from this gathered run
        tasks[0] = publish(a, point);
        tasks[1] = publish(b, small);
        tasks[2] = publish(b, small);
        require(tasks[0].op_id - a.rob().flush_id() == 1, "hidden-head state was not entered");
        verify(tasks, {tasks[1], tasks[2], tasks[0]}, true);
        retire_all(a);
        retire_all(b);
    }
    {
        Client a(-1), b(-1);
        Task tasks[kGenthreadExBatchOps];
        tasks[0] = publish(a, point);
        (void)publish(a, point); // gap within the represented connection
        tasks[1] = publish(a, point);
        tasks[2] = publish(b, small);
        tasks[3] = publish(b, small);
        tasks[4] = publish(b, small);
        require(tasks[1].op_id == tasks[0].op_id + 2, "in-run gap state was not entered");
        verify(tasks, {tasks[0], tasks[2], tasks[3], tasks[4], tasks[1]}, true);
        retire_all(a);
        retire_all(b);
    }
    {
        Client a(-1), b(-1);
        Task tasks[kGenthreadExBatchOps];
        tasks[0] = publish(a, point, true);
        tasks[1] = publish(b, small);
        require(a.rob().at(tasks[0].op_id).atomic_hazard(), "atomic hazard state was not entered");
        verify(tasks, {tasks[1], tasks[0]}, true);
        retire_all(a);
        retire_all(b);
    }
    std::puts("  predecessor controls: missing head, in-run gap and atomic hazard widen to Long");
}

// R2's trigger is STATIC class, never rank spread, command identity, or an atomic-hazard bit.
// These controls have legal cross-client rank permutations in the old scheduler. Checking exact
// FIFO alone is insufficient: a rank/dependency scan returning identity must also be rejected.
template <size_t Capacity>
void uniform_controls() {
    for (const CommandSpec* cost : {&point, &small, &long_op}) {
        Client a(-1), b(-1);
        Task tasks[Capacity];
        constexpr uint32_t per_client = Capacity / 2;
        for (uint32_t i = 0; i < per_client; i++) tasks[i] = publish(a, *cost, i == 0);
        for (uint32_t i = 0; i < per_client; i++)
            tasks[per_client + i] = publish(b, cost == &point && i % 2 ? other_point : *cost);
        require(tasks[per_client - 1].op_id - a.rob().flush_id() > 1,
                "uniform wide-rank control never armed");
        ExReorderScreen screen(tasks, Capacity);
        for (const Task& task : tasks) {
            const uint32_t flags = task.client->rob().at(task.op_id).spec->flags;
            require(screen.prefetch(flags), "uniform ordinary prefetch was suppressed");
        }
        require(!screen.mixed(), "uniform batch entered scheduler branch");
        const std::vector<Task> expected(tasks, tasks + Capacity);
        const ReorderResult result = ex_schedule_batch(tasks, Capacity);
        require(result.multi_client_runs == 0 && result.permuted_runs == 0,
                "uniform batch sampled ranks/dependencies");
        verify(tasks, expected, false);
        retire_all(a);
        retire_all(b);

        // An absent live predecessor can widen cost after admission, but cannot admit a batch.
        (void)publish(a, *cost);
        (void)publish(a, *cost);
        tasks[0] = publish(a, *cost, true);
        tasks[1] = publish(b, *cost);
        ExReorderScreen hidden(tasks, 2);
        require(hidden.prefetch(cost->flags) && hidden.prefetch(cost->flags) && !hidden.mixed(),
                "hidden predecessor manufactured a static class mix");
        verify(tasks, {tasks[0], tasks[1]}, false);
        retire_all(a);
        retire_all(b);
    }
    std::printf("  R2 uniform: capacity %zu, all classes, wide ranks, hidden heads and hazards\n", Capacity);
}

void screen_controls() {
    Client a(-1), b(-1), c(-1);
    Task tasks[kGenthreadExBatchOps];
    tasks[0] = publish(a, long_op);
    tasks[1] = publish(b, point);
    ExReorderScreen mixed(tasks, 2);
    require(mixed.prefetch(long_op.flags) && !mixed.mixed(), "first class triggered scheduling");
    require(mixed.prefetch(point.flags) && mixed.mixed(), "mixed classes never triggered scheduling");
    require(!mixed.prefetch(CmdFlags::CursorShard | point.flags), "cursor prefetch lost its barrier");
    require(!mixed.prefetch(CmdFlags::RandomShard | point.flags), "random prefetch lost its barrier");
    require(mixed.prefetch(long_op.flags), "screen stopped prefetching after the trigger");
    retire_all(a);
    retire_all(b);

    // A special first task must not be mistaken for a Point command or dereferenced as an Op.
    tasks[0] = Task(nullptr, UINT64_MAX, -1, nullptr);
    tasks[1] = publish(a, small);
    tasks[2] = publish(b, small);
    ExReorderScreen special(tasks, 3);
    require(special.prefetch(small.flags) && special.prefetch(small.flags) && !special.mixed(),
            "null-client barrier manufactured a mix");
    tasks[0] = Task(&c, UINT64_MAX, -1, reinterpret_cast<ScatterState*>(&c));
    ExReorderScreen scatter(tasks, 3);
    require(scatter.prefetch(small.flags) && !scatter.mixed(), "scatter was used as a class seed");
    retire_all(a);
    retire_all(b);

    // The batch as a whole mixes classes; its separate eligible runs do not. A wide rank
    // spread on the uniform prefix would be enough to sort if the inner-run guard were lost.
    tasks[0] = publish(a, point);
    tasks[1] = publish(a, point);
    tasks[2] = publish(a, point);
    tasks[3] = publish(b, point);
    tasks[4] = publish(c, admin);
    tasks[5] = publish(c, long_op);
    const ReorderResult split = ex_schedule_batch(tasks, 6);
    require(split.multi_client_runs == 0 && split.permuted_runs == 0,
            "class mix across a barrier armed a uniform run");
    retire_all(a);
    retire_all(b);
    retire_all(c);
    std::puts("  R2 screen: mixed witness, special/scatter seeds, prefetch barriers and uniform runs");
}
}  // namespace

int main() {
    static_assert(kGenthreadExBatchOps == 32 && kGenthreadPipelineExBatchOps == 128,
                  "update the regression cases when executor geometry changes");
    heads<kGenthreadExBatchOps>();
    heads<kGenthreadPipelineExBatchOps>();
    pipelines<kGenthreadExBatchOps>();
    pipelines<kGenthreadPipelineExBatchOps>();
    barriers<kGenthreadExBatchOps>();
    barriers<kGenthreadPipelineExBatchOps>();
    fifo_controls();
    hidden_predecessors();
    uniform_controls<kGenthreadExBatchOps>();
    uniform_controls<kGenthreadPipelineExBatchOps>();
    screen_controls();
    require(permutations == 175, "a required positive case silently disappeared");
    std::printf("reorder battery: PASS, %u witnessed permutations, max batch 128\n", permutations);
}

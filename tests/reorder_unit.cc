// Server-less battery for the exact scheduler used by both executor call sites. Real Clients,
// Tasks and published ROB slots; no alternate scheduling implementation or timing window.
// The gate runs this under ASAN + UBSAN. A FIFO/no-op mutant must fail the permutation oracle.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>
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
    out.length_class = static_cast<uint8_t>(length);
    return out;
}
constexpr auto point = spec("GET", CommandLengthClass::Point, CmdFlags::Readonly);
constexpr auto small = spec("MGET", CommandLengthClass::SmallMulti,
                            CmdFlags::Readonly | CmdFlags::MultiShard);
constexpr auto long_op = spec("BITCOUNT", CommandLengthClass::Long, CmdFlags::Readonly);
constexpr auto admin = spec("DEBUG", CommandLengthClass::Point, CmdFlags::Admin);

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

// Independent heads must exercise the weighted turn, not merely any legal permutation.
// Sweep BOTH real capacities, including 31/32/33/127/128; all Task fields must travel intact.
template <size_t Capacity>
void heads() {
    for (uint32_t n = 2; n <= Capacity; n++) {
        Task tasks[Capacity];
        std::vector<std::unique_ptr<Client>> clients;
        for (uint32_t i = 0; i < n; i++) {
            clients.push_back(std::make_unique<Client>(-1));
            tasks[i] = publish(*clients.back(), i ? point : long_op);
            require(tasks[i].op_id == clients.back()->rob().flush_id(),
                    "mixed-client live-head state was not entered");
        }
        const uint32_t quick = std::min<uint32_t>(4, n - 1);
        std::vector<Task> expected(tasks + 1, tasks + quick + 1);
        expected.push_back(tasks[0]);
        expected.insert(expected.end(), tasks + quick + 1, tasks + n);
        verify(tasks, expected, true);
        for (auto& client : clients) retire_all(*client);
    }
    std::printf("  heads: n=2..%zu, exact 4:1 short/long turns\n", Capacity);
}

// Only the short client's head can be promoted: unrepresented and queued predecessors are
// conservatively long. Repeated full live windows cross ROB wraps without pinning flush_id=0.
template <size_t Capacity>
void pipelines() {
    Client a(-1), b(-1);
    for (uint32_t round = 0; round < 4; round++) {
        Task tasks[Capacity];
        for (uint32_t i = 0; i < Capacity / 2; i++)
            tasks[i] = publish(a, i ? point : long_op);
        for (uint32_t i = 0; i < Capacity / 2; i++)
            tasks[Capacity / 2 + i] = publish(b, point);
        std::vector<Task> expected;
        expected.reserve(Capacity);
        expected.push_back(tasks[Capacity / 2]);
        expected.insert(expected.end(), tasks, tasks + Capacity / 2);
        expected.insert(expected.end(), tasks + Capacity / 2 + 1, tasks + Capacity);
        verify(tasks, expected, true);
        retire_all(a);
        retire_all(b);
    }
    require(a.rob().flush_id() >= kRobWindow && b.rob().flush_id() >= kRobWindow,
            "ROB wrap state was not entered");
    std::printf("  pipelines: %zu tasks, head promotion and FIFO across ROB wraps\n", Capacity);
}

// Every special mechanism is a barrier, including a suffix beginning above the old 32 limit.
template <size_t Capacity>
void barriers() {
    constexpr uint32_t barrier_at = Capacity == 128 ? 32 : 16;
    constexpr uint32_t flags[] = {CmdFlags::Admin, CmdFlags::ConnLocal, CmdFlags::AllShards,
        CmdFlags::RandomShard, CmdFlags::CursorShard, CmdFlags::ConfigRoute, CmdFlags::ScriptRoute,
        CmdFlags::PubSub, CmdFlags::Blocking, CmdFlags::Transaction, CmdFlags::StreamRoute,
        CmdFlags::SubcmdRoute, CmdFlags::FlipAsync};
    for (uint32_t kind = 0; kind < std::size(flags) + 5; kind++) {
        Task tasks[Capacity];
        std::vector<std::unique_ptr<Client>> clients;
        const auto special = spec("BARRIER", CommandLengthClass::Point,
                                  kind < std::size(flags) ? flags[kind] : 0);
        const auto invalid = spec("INVALID", CommandLengthClass::Count, 0);
        for (uint32_t i = 0; i < Capacity; i++) {
            clients.push_back(std::make_unique<Client>(-1));
            tasks[i] = publish(*clients.back(), i == barrier_at ? special :
                               i == 0 || i == barrier_at + 1 ? long_op : point);
        }
        if (kind == std::size(flags)) tasks[barrier_at].client = nullptr;
        if (kind == std::size(flags) + 1)
            tasks[barrier_at].scatter = reinterpret_cast<ScatterState*>(clients.back().get());
        if (kind == std::size(flags) + 2)
            clients[barrier_at]->rob().at(tasks[barrier_at].op_id).spec = nullptr;
        if (kind == std::size(flags) + 3)
            clients[barrier_at]->rob().at(tasks[barrier_at].op_id).attach_blocking_state(
                reinterpret_cast<BlockingState*>(clients.back().get()));
        if (kind == std::size(flags) + 4)
            clients[barrier_at]->rob().at(tasks[barrier_at].op_id).spec = &invalid;
        uint8_t length = 255;
        require(!ex_sched_candidate(tasks[barrier_at], length), "barrier state was not entered");
        std::vector<Task> expected;
        auto append_run = [&](uint32_t begin, uint32_t end) {
            for (uint32_t i = begin + 1; i < begin + 5; i++) expected.push_back(tasks[i]);
            expected.push_back(tasks[begin]);
            for (uint32_t i = begin + 5; i < end; i++) expected.push_back(tasks[i]);
        };
        append_run(0, barrier_at);
        expected.push_back(tasks[barrier_at]);
        append_run(barrier_at + 1, Capacity);
        verify(tasks, expected, true);
        if (kind == std::size(flags) + 3)
            clients[barrier_at]->rob().at(tasks[barrier_at].op_id).detach_blocking_state();
        for (auto& client : clients) retire_all(*client);
    }
    std::printf("  barriers: capacity %zu, all special flags and complete suffix\n", Capacity);
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
        Client a(-1), b(-1), c(-1);
        Task tasks[kGenthreadExBatchOps];
        (void)publish(a, point);
        tasks[0] = publish(a, point);
        tasks[1] = publish(c, long_op); // opens a real coarse-class mix
        tasks[2] = publish(b, small);
        require(tasks[0].op_id - a.rob().flush_id() == 1, "hidden-head state was not entered");
        verify(tasks, {tasks[2], tasks[0], tasks[1]}, true);
        retire_all(a); retire_all(b); retire_all(c);
    }
    {
        Client a(-1), b(-1), c(-1);
        Task tasks[kGenthreadExBatchOps];
        tasks[0] = publish(a, point);
        (void)publish(a, point);
        tasks[1] = publish(a, point);
        tasks[2] = publish(c, long_op);
        tasks[3] = publish(b, small);
        require(tasks[1].op_id == tasks[0].op_id + 2, "in-run gap state was not entered");
        verify(tasks, {tasks[0], tasks[3], tasks[1], tasks[2]}, true);
        retire_all(a); retire_all(b); retire_all(c);
    }
    {
        Client a(-1), b(-1);
        Task tasks[kGenthreadExBatchOps];
        tasks[0] = publish(a, point, true);
        tasks[1] = publish(b, small);
        require(a.rob().at(tasks[0].op_id).atomic_hazard(), "atomic hazard state was not entered");
        verify(tasks, {tasks[1], tasks[0]}, true);
        retire_all(a); retire_all(b);
    }
    std::puts("  predecessor controls: missing head, in-run gap and atomic hazard");
}

// A due long turn cannot jump its own short predecessor, even if that predecessor is the FIFTH
// short in the queue. Removing the frontier guard deterministically inverts that connection.
void dependency_over_quota() {
    Task tasks[kGenthreadExBatchOps];
    std::vector<std::unique_ptr<Client>> clients;
    for (uint32_t i = 0; i < 5; i++) {
        clients.push_back(std::make_unique<Client>(-1));
        tasks[i] = publish(*clients.back(), point);
    }
    tasks[5] = publish(*clients.back(), long_op);
    clients.push_back(std::make_unique<Client>(-1));
    tasks[6] = publish(*clients.back(), point);
    verify(tasks, std::vector<Task>(tasks, tasks + 7), false);
    for (auto& client : clients) retire_all(*client);
    std::puts("  dependency: due ratio turn waits for the fifth short predecessor");
}

// A younger short from gather B must pass an older long retained from gather A. Replacing this
// with a per-batch sorter/no-op fails, even if every single-batch permutation above still passes.
// The callback immediately retires and destroys each Client, catching post-Done dereferences.
template <size_t Capacity>
void continuous() {
    ExReorderQueues<Capacity> queues;
    std::vector<std::unique_ptr<Client>> clients(3 * Capacity);
    std::vector<uint64_t> output;
    Task first[Capacity], second[Capacity], barrier[Capacity];
    auto add = [&](Task& task, uint32_t id, const CommandSpec& command) {
        clients[id] = std::make_unique<Client>(-1);
        task = publish(*clients[id], command);
        task.enqueue_us_low = id + 1;
    };
    for (uint32_t i = 0; i < Capacity; i++) {
        add(first[i], i, i == 1 ? point : long_op);
        add(second[i], Capacity + i, point);
    }
    add(barrier[0], 2 * Capacity, admin);
    add(barrier[1], 2 * Capacity + 1, long_op);
    add(barrier[2], 2 * Capacity + 2, point);
    uint32_t witnessed = 0;
    auto emit = [&](const Task* tasks, uint32_t n, ReorderResult witness) {
        witnessed += witness.permuted_runs;
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t id = tasks[i].enqueue_us_low - 1;
            require(id < clients.size() && clients[id].get() == tasks[i].client,
                    "continuous drain lost/duplicated a task");
            output.push_back(id);
            retire_all(*clients[id]);
            clients[id].reset();
        }
    };
    queues.submit(first, Capacity, emit);
    require(output.empty() && queues.size() == Capacity, "first gather was not retained");
    queues.submit(second, Capacity, emit);
    require(output.size() == Capacity && queues.size() == Capacity,
            "second gather did not emit one quantum and retain one");
    require(output[0] == 1 && output[1] == Capacity && output[4] == 0,
            "cross-gather promotion or 4:1 service did not fire");
    queues.submit(barrier, 3, emit);
    require(output.size() == 2 * Capacity + 1 && output.back() == 2 * Capacity,
            "barrier executed before both queues drained (or suffix crossed barrier)");
    require(queues.size() == 2, "mixed post-barrier suffix was not retained");
    queues.finish(emit);
    require(!queues.size() && output.size() == 2 * Capacity + 3 && witnessed > 0,
            "continuous drain failed to finish or never reordered");
    require(output[2 * Capacity + 1] == 2 * Capacity + 2 &&
            output.back() == 2 * Capacity + 1, "post-barrier ratio phase was not reset");
    std::puts("  continuous: cross-gather promotion, bounded debt, barrier and immediate destruction");
}

// Check long service while both FIFOs remain backlogged through repeated admissions/ring wraps.
// This is a service-order assertion, not a timing tolerance or a throughput benchmark.
template <size_t Capacity>
void sustained_service() {
    ExReorderQueues<Capacity> queues;
    std::vector<std::unique_ptr<Client>> clients;
    uint32_t since_long = 0, longs = 0, seen = 0;
    auto emit = [&](const Task* tasks, uint32_t n, ReorderResult) {
        for (uint32_t i = 0; i < n; i++) {
            const bool heavy = tasks[i].client->rob().at(tasks[i].op_id).spec == &long_op;
            if (heavy) {
                require(since_long <= 4, "long FIFO starved while both classes were backlogged");
                since_long = 0;
                longs++;
            } else since_long++;
            seen++;
            retire_all(*tasks[i].client);
        }
    };
    for (uint32_t round = 0; round < 12; round++) {
        Task batch[Capacity];
        for (uint32_t i = 0; i < Capacity; i++) {
            clients.push_back(std::make_unique<Client>(-1));
            // Extra long work keeps the long FIFO backlogged; each long has no short dependency.
            batch[i] = publish(*clients.back(), i % 2 ? point : long_op);
        }
        queues.submit(batch, Capacity, emit);
        require(queues.size() <= Capacity, "retained debt exceeded one gather");
    }
    queues.finish(emit);
    require(seen == 12 * Capacity && longs == 6 * Capacity,
            "ring-wrap service lost work or failed to exercise both queues");
    std::puts("  sustained: bounded long service across 12 gathers and ring wraps");
}

// A deterministic mixed stream checks semantic invariants independently of the service policy.
// IO retires each emitted op immediately, so subsequent admissions also see advancing ROB heads.
template <size_t Capacity>
void connection_stream() {
    constexpr uint32_t total = 1024, client_count = 32;
    std::vector<std::unique_ptr<Client>> clients;
    for (uint32_t i = 0; i < client_count; i++) clients.push_back(std::make_unique<Client>(-1));
    std::vector<Task> input;
    std::vector<uint32_t> segments;
    uint32_t segment = 0;
    for (uint32_t i = 0; i < total; i++) {
        // This permutation changes the connection adjacency at every gather while keeping each
        // producer's own command ids increasing and its live window within the actual ROB.
        const uint32_t client = (i * 13 + (i / client_count) * 7) % client_count;
        const auto& command = i % 113 == 112 ? admin :
                              i % 7 == 0 ? long_op : i % 3 == 0 ? small : point;
        input.push_back(publish(*clients[client], command, i % 53 == 0));
        input.back().enqueue_us_low = i + 1;
        segments.push_back(segment);
        if (&command == &admin) segment++;
    }
    std::vector<bool> seen(total);
    uint32_t count = 0;
    auto emit = [&](const Task* tasks, uint32_t n, ReorderResult) {
        for (uint32_t i = 0; i < n; i++) {
            const Task& task = tasks[i];
            const uint32_t original = task.enqueue_us_low - 1;
            require(original < total && !seen[original] && same(task, input[original]),
                    "connection stream lost, duplicated or damaged a Task");
            require(segments[original] == segments[count], "connection stream crossed a barrier");
            auto& rob = task.client->rob();
            require(task.op_id == rob.flush_id(), "connection stream violated execution/RYOW order");
            if (rob.at(task.op_id).spec == &admin)
                require(original == count, "connection stream moved a special-task barrier");
            seen[original] = true;
            count++;
            rob.at(task.op_id).state.store(OpState::Done, std::memory_order_release);
            require(rob.drain([](Op&) {}) == 1, "connection stream did not retire exactly one op");
        }
    };
    ExReorderQueues<Capacity> queues;
    for (uint32_t begin = 0; begin < total; begin += Capacity)
        queues.submit(input.data() + begin, Capacity, emit);
    queues.finish(emit);
    require(count == total, "connection stream left unfinished work");
    for (auto& client : clients) require(client->rob().quiesced(), "connection remained in flight");
    std::puts("  connection stream: 1024 tasks, advancing heads, hazards and fixed barrier scopes");
}
}  // namespace

int main() {
    static_assert(kGenthreadExBatchOps == 32 && kGenthreadPipelineExBatchOps == 128);
    static_assert(kExReorderShortQuota == 4, "rejudge ratio oracles if the candidate changes");
    heads<kGenthreadExBatchOps>();
    heads<kGenthreadPipelineExBatchOps>();
    pipelines<kGenthreadExBatchOps>();
    pipelines<kGenthreadPipelineExBatchOps>();
    barriers<kGenthreadExBatchOps>();
    barriers<kGenthreadPipelineExBatchOps>();
    fifo_controls();
    hidden_predecessors();
    dependency_over_quota();
    continuous<kGenthreadExBatchOps>();
    continuous<kGenthreadPipelineExBatchOps>();
    sustained_service<kGenthreadExBatchOps>();
    sustained_service<kGenthreadPipelineExBatchOps>();
    connection_stream<kGenthreadExBatchOps>();
    connection_stream<kGenthreadPipelineExBatchOps>();
    require(permutations == 205, "a required positive batch case silently disappeared");
    std::printf("reorder battery: PASS, %u batch permutations plus continuous service, max batch 128\n",
                permutations);
}

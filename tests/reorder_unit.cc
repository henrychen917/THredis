// Server-less battery for the exact scheduler used before IO admission. Real Clients,
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

// Exercise the actual private frontier and release publication, including wrap, credit
// reservation and a partial final chunk. A publish-before-sort mutant fails before the oracle.
void admission_transport() {
    MaskedSpscArray<Task, 2> queue;
    require(queue.init_local_fused<512>(2), "admission fixture queue initialization");
    for (uint32_t round = 0; round < 3; round++) {
        std::vector<std::unique_ptr<Client>> clients;
        std::vector<Task> original;
        require(queue.reserve(0, 2), "reserve capacity before admission");
        queue.begin_admission(0);
        for (uint32_t i = 0; i < 510; i++) {
            clients.push_back(std::make_unique<Client>(-1));
            original.push_back(publish(*clients.back(), i % 128 ? point : long_op));
            require(queue.push_admitted(0, original.back(), [](Task&) {}),
                    "admission refused before reserved capacity boundary");
        }
        require(!queue.push_admitted(0, original.back(), [](Task&) {}),
                "admission ignored reserved credits");
        Task task;
        require(queue.depth(0) == 0 && !queue.pop(0, task),
                "consumer observed an unpublished admission prefix");
        uint32_t callbacks = 0;
        queue.finish_admission<128>(0, [&](auto& tasks, uint32_t n) {
            callbacks++;
            return ex_schedule_batch(tasks, n).permuted_runs != 0;
        });
        require(callbacks == 4 && queue.depth(0) == 510,
                "admission lost the partial chunk or published the wrong frontier");
        std::vector<Task> expected;
        for (uint32_t base = 0; base < 510; base += 128) {
            for (uint32_t i = base + 1; i < std::min(base + 128, 510u); i++)
                expected.push_back(original[i]);
            expected.push_back(original[base]);
        }
        for (const Task& want : expected) {
            require(queue.pop(0, task) && same(task, want), "published admission order differs");
            queue.retire(0);
        }
        require(!queue.pop(0, task), "admission duplicated a task");
        // The same lane can now publish a reserved special directly. A new admission scope
        // must start AFTER it, including when the underlying ring is about to wrap.
        queue.push_reserved(0, original[0]);
        queue.cancel_reservation(0, 1);
        queue.begin_admission(0);
        require(queue.push_admitted(0, original[1], [](Task&) {}), "resume admission after direct post");
        require(queue.pop(0, task) && same(task, original[0]), "admission passed a direct barrier");
        queue.retire(0);
        require(!queue.pop(0, task), "resumed admission published early");
        queue.finish_admission<128>(0, [](auto&, uint32_t n) {
            require(n == 1, "resumed private frontier is stale");
            return false;
        });
        require(queue.pop(0, task) && same(task, original[1]), "resumed admission lost task");
        queue.retire(0);
        require(queue.quiesced(0), "admission did not fully retire");
        for (auto& client : clients) retire_all(*client);
    }
    std::puts("  admission: private publication, exact order, credit refusal, wrap, direct-post fence");
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
    admission_transport();
    fifo_controls();
    hidden_predecessors();
    require(permutations == 175, "a required positive case silently disappeared");
    std::printf("reorder battery: PASS, %u witnessed permutations, max batch 128\n", permutations);
}

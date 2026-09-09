// Server-less checks for configuration publication and atomic admission. Run via `make unit`.
// alarm makes a reader that waits for our deliberately stopped writer FAIL instead of hanging.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unistd.h>
#include "src/core/live_config.h"
#include "src/core/atomic_admission.h"

using namespace tomo;

static void require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "waits unit: %s\n", message); std::abort(); }
}

static LiveConfigValues values(uint64_t version) {
    LiveConfigValues out;
    out.live.version = out.clients.version = version;
    out.live.maxmemory = version * 17;
    out.live.samples = static_cast<uint32_t>(version + 3);
    out.live.proto_max_bulk_len = ~version;
    out.clients.normal.hard_bytes = version * 31;
    out.clients.normal.soft_bytes = version * 7;
    out.clients.normal.soft_seconds = static_cast<uint32_t>(version);
    out.clients.pubsub.hard_bytes = ~version;
    return out;
}

static void coherent(const LiveConfigValues& got) {
    const auto want = values(got.live.version);
    require(!(got.live.version & 1) && got.live.version >= 2, "committed version");
    require(got.clients.version == want.clients.version &&
            got.live.maxmemory == want.live.maxmemory &&
            got.live.samples == want.live.samples &&
            got.live.proto_max_bulk_len == want.live.proto_max_bulk_len &&
            got.clients.normal.hard_bytes == want.clients.normal.hard_bytes &&
            got.clients.normal.soft_bytes == want.clients.normal.soft_bytes &&
            got.clients.normal.soft_seconds == want.clients.normal.soft_seconds &&
            got.clients.pubsub.hard_bytes == want.clients.pubsub.hard_bytes,
            "configuration fields came from different updates");
}

static void stopped_writer_and_reader() {
    std::atomic<uint64_t> version{2};
    LiveConfigMailbox a, b;
    a.init(values(2)); b.init(values(2));
    version.store(3);                         // writer stopped before staging anything
    require(a.read(version).live.version == 2, "odd version must not obstruct reader");
    a.publish(values(2), values(4));           // stopped halfway through the fan-out
    require(a.read(version).live.version == 2, "uncommitted staged value became visible");
    require(b.read(version).live.version == 2, "unstaged reader lost committed config");
    b.publish(values(2), values(4));
    version.store(4);
    // a already consumed its staged slot. Committing it must require no second publication.
    require(a.read(version).live.version == 4, "commit of an already consumed slot was lost");
    require(b.read(version).live.version == 4, "committed update not delivered");
    const LiveConfigValues& pinned = a.read(version);
    for (uint64_t next = 6; next <= 200; next += 2) {
        version.store(next - 1);
        a.publish(values(next - 2), values(next));
        b.publish(values(next - 2), values(next));
        version.store(next);
        coherent(b.read(version));
        require(pinned.live.version == 4, "writer reused a stopped consumer's slot");
        coherent(pinned);
    }
    require(a.read(version).live.version == 200, "coalesced updates did not reach stopped reader");
    // Skip a committed version, then acquire its successor while the writer is still odd.
    version.store(201);
    a.publish(values(200), values(202));
    version.store(202);
    version.store(203);
    a.publish(values(202), values(204));
    require(a.read(version).live.version == 202, "fallback missed the preceding commit");
}

static void concurrent_config() {
    std::atomic<uint64_t> version{2};
    LiveConfigMailbox boxes[2];
    for (auto& box : boxes) box.init(values(2));
    std::thread readers[2];
    for (unsigned tid = 0; tid < 2; tid++) readers[tid] = std::thread([&, tid] {
        uint64_t seen = 2;
        for (unsigned i = 0; i < 20000; i++) {
            const auto copy = boxes[tid].read(version);
            coherent(copy);
            require(copy.live.version >= seen, "reader went backward across updates");
            seen = copy.live.version;
        }
    });
    for (uint64_t next = 4; next <= 20000; next += 2) {
        version.store(next - 1, std::memory_order_release);
        for (auto& box : boxes) box.publish(values(next - 2), values(next));
        version.store(next, std::memory_order_release);
    }
    for (auto& reader : readers) reader.join();
    for (auto& box : boxes)
        require(box.read(version).live.version == 20000, "final committed value lost");
}

static void admission_accounting() {
    AtomicAdmissionCredits credits;
    using Attempt = AtomicAdmissionCredits::Attempt;
    credits.init(3);
    for (unsigned i = 0; i < 3; i++)
        require(credits.try_admit() == Attempt::Admitted, "initial reservation");
    require(credits.try_admit() == Attempt::Full, "full window must refuse");
    credits.set_window(1);
    require(credits.active() == 3 && credits.debt() == 2 && credits.pool() == 0,
            "shrinking must retain existing groups as debt");
    credits.retire(); credits.retire();
    require(credits.debt() == 0 && !credits.can_admit(), "window admitted at its limit");
    credits.set_window(0);
    require(credits.try_admit() == Attempt::Admitted && credits.debt() == 0,
            "unlimited window retained debt");
    credits.set_window(7);
    require(credits.pool() == 5 && credits.active() == 2, "reconfigure lost active groups");
    credits.retire(); credits.retire();
    require(credits.pool() == 7 && credits.active() == 0, "retirement stranded a credit");
}

static void concurrent_admission() {
    AtomicAdmissionCredits credits;
    credits.init(3);
    std::atomic<unsigned> inside{0};
    std::thread workers[4];
    for (auto& worker : workers) worker = std::thread([&] {
        for (unsigned i = 0; i < 10000; i++) {
            if (credits.try_admit() != AtomicAdmissionCredits::Attempt::Admitted) continue;
            require(inside.fetch_add(1) < 3, "concurrent admission exceeded the limit");
            inside.fetch_sub(1);
            credits.retire();
        }
    });
    for (auto& worker : workers) worker.join();
    require(credits.active() == 0 && credits.pool() == 3, "concurrent retirement leaked credits");
    // This race checks conservation through reconfiguration, not a bound on inherited groups.
    for (auto& worker : workers) worker = std::thread([&] {
        for (unsigned i = 0; i < 10000; i++)
            if (credits.try_admit() == AtomicAdmissionCredits::Attempt::Admitted) credits.retire();
    });
    for (unsigned i = 0; i < 10000; i++) credits.set_window(i % 5);
    for (auto& worker : workers) worker.join();
    credits.set_window(3);
    require(credits.active() == 0 && credits.pool() == 3 && credits.debt() == 0,
            "reconfiguration raced a return");
}

int main() {
    alarm(30);
    stopped_writer_and_reader();
    concurrent_config();
    admission_accounting();
    concurrent_admission();
    std::puts("waits unit: PASS");
}

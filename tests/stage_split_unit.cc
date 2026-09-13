// Serverless lifetime checks for F11. No socket, ring, worker loop or load generator is started.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include "src/net/conn.h"
#include "src/net/resp.h"

// The allocator-count build wraps calls from this TU, leaving the production allocator and
// sanitizer interceptors intact. Counting pointer/capacity changes alone would miss an allocation
// that was made and immediately freed. These counters are never linked into the server.
#ifdef TOMO_STAGE_SPLIT_ALLOCATIONS
static size_t allocations = 0;
static size_t allocated_bytes = 0;
extern "C" void* __real_malloc(size_t);
extern "C" void* __real_realloc(void*, size_t);
extern "C" void* __real__Znwm(size_t);
extern "C" void* __wrap_malloc(size_t n) {
    ++allocations; allocated_bytes += n;
    return __real_malloc(n);
}
extern "C" void* __wrap_realloc(void* p, size_t n) {
    ++allocations; allocated_bytes += n;
    return __real_realloc(p, n);
}
extern "C" void* __wrap__Znwm(size_t n) {
    ++allocations; allocated_bytes += n;
    return __real__Znwm(n);
}
#endif

namespace tomo {
void multi_session_destroy(MultiSession* session) {
    if (session) std::abort(); // this fixture never creates a transaction session
}
}

static void check(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL stage split: %s\n", why); std::abort(); }
}

static void op_storage() {
    tomo::OpChunk<8> chunk;
    for (size_t i = 0; i < 8; i++) {
        auto& op = chunk.ops[i];
        check(op.reply.data() == chunk.reply_bytes[i], "each slot owns its matching body");
        check(op.reply.cap() == tomo::kInlineReply, "bound inline capacity");
        std::memset(chunk.reply_bytes[i], int('a' + i), tomo::kInlineReply);
        op.reset();
        check(chunk.reply_bytes[i][0] == char('a' + i), "reset does not write unused body bytes");
        op.reply.append("+OK\r\n");
    }
    for (size_t i = 0; i < 8; i++)
        check(std::string_view(chunk.ops[i].reply.data(), chunk.ops[i].reply.size()) == "+OK\r\n",
              "body writes do not alias another slot");

    auto& op = chunk.ops[0];
    char* home = op.reply.data();
    op.clear_reply();
    char direct[8];
    op.direct = direct;
    op.direct_cap = sizeof(direct);
    auto sink = op.sink();
    sink.append("abcd");
    sink.append("0123456789");
    check(op.direct_len == 4 && std::string_view(direct, 4) == "abcd", "direct prefix survives spill");
    check(std::string_view(op.reply.data(), op.reply.size()) == "0123456789", "spill suffix ordered");
    check(op.reply.data() == home, "short spill uses chunk storage");

    op.clear_reply();
    op.direct = nullptr;
    char big[9000];
    std::memset(big, 'z', sizeof(big));
    op.reply.append(big, sizeof(big));
    check(op.oversized() && op.reply.data() != home, "large reply actually grows");
    check(op.reply.size() == sizeof(big) && !std::memcmp(op.reply.data(), big, sizeof(big)),
          "large reply preserves every byte");
    for (unsigned i = 0; i < 40; i++) check(op.push_arg(tomo::Slice("key", 3)), "argv growth");
    check(op.arg(39).sv() == "key", "heap argv accessible after header move");
    op.shrink();
    check(op.reply.data() == home && op.reply.cap() == tomo::kInlineReply && !op.oversized(),
          "retirement returns to the original chunk body");
    op.reset();
    check(op.argc() == 0 && op.push_arg(tomo::Slice("GET", 3)) && op.cmd_name().sv() == "GET",
          "argv recycles after shrink");

    op.clear_reply();
    op.reply_code_ok_ = 1;
    check(op.sink().code(tomo::ReplyCode::Int, -123), "coded reply is armed");
    check(op.reply.empty(), "coded reply does not write body");
    op.sink().append("tail");
    check(std::string_view(op.reply.data(), op.reply.size()) == "tail", "coded suffix uses byte sink");
    op.clear_reply();
    check(!op.replied() && op.reply_code_ == 0 && op.reply_ival_ == 0, "discard also clears code");

    // The marker pointer is independent of the reply home's ownership and must survive body use.
    int marker = 0;
    op.attach_multi_state(&marker);
    op.reply.append("reply");
    check(op.multi_state() == &marker, "transaction marker survives byte reply");
    op.reset_read_local(0xff);
    check(!op.has_multi_state() && !op.read_local() && !op.read_local_precise_write(),
          "recycle clears marker and masks connection-only route bits");
}

template <bool Codes> static void rob_recycle() {
    tomo::Rob<64> rob;
    char* homes[64]{};
    for (unsigned round = 0; round < 3; round++) {
        for (unsigned i = 0; i < 64; i++) {
            auto* op = rob.acquire<Codes>();
            check(op != nullptr, "ROB window admits exactly 64 slots");
            if (round == 0) homes[i] = op->reply.data();
            check(op->reply.data() == homes[i], "reply body remains stable across ROB wrap");
            op->hash = i;
            op->state.store(tomo::OpState::Issued, std::memory_order_release);
            rob.publish();
        }
        check(rob.acquire<Codes>() == nullptr, "full ROB refuses another slot");
        for (unsigned i = 64; i-- > 1;) {
            auto& op = rob.at(uint64_t(round) * 64 + i);
            op.sink().append("+OK\r\n");
            op.state.store(tomo::OpState::Done, std::memory_order_release);
        }
        check(rob.drain([](tomo::Op&) { check(false, "cannot retire past an issued head"); }) == 0,
              "later completed bodies remain pinned");
        auto& head = rob.at(uint64_t(round) * 64);
        head.sink().append("+OK\r\n");
        head.state.store(tomo::OpState::Done, std::memory_order_release);
        unsigned next = 0;
        check(rob.drain([&](tomo::Op& op) {
            check(op.hash == next++, "body replies retire in connection order");
            check(std::string_view(op.reply.data(), op.reply.size()) == "+OK\r\n", "published bytes");
        }) == 64 && rob.quiesced(), "complete drain before reuse");
    }
}

static void standalone() {
    tomo::Op op;
    char* empty = op.reply.data();
    check(empty && op.reply.cap() == 0, "standalone Op starts without reply allocation");
    op.reset();
    check(op.reply.data() == empty && op.reply.empty(), "unarmed reset needs no body");
    op.reply.append("standalone");
    check(op.reply.data() != empty && op.reply.cap() >= 10, "standalone byte reply allocates on demand");
    op.shrink();
    check(op.reply.data() == empty && op.reply.cap() == 0, "standalone shrink releases owned storage");
    op.reply.append("again");
    check(std::string_view(op.reply.data(), op.reply.size()) == "again", "standalone reuse after shrink");
}

static void send_storage() {
    tomo::Client client(-1);
    check(client.send_msg() == nullptr && client.send_state_bytes() == 0,
          "an idle connection allocates no send body");
    bool borrowed = true;
    uint32_t bytes = 1;
    check(client.build_segment_iov(borrowed, bytes) == 0 && !borrowed && bytes == 0 &&
          client.send_state_bytes() == 0, "empty send work allocates nothing");
    client.append_fill("+OK\r\n", 5);
    check(client.send_state_bytes() == 0, "ordinary byte reply needs no descriptor body");
    client.seal_fill_segment();
    static const char value[] = "borrowed";
    client.append_borrow_segment(value, 8, 3);
    for (unsigned i = 0; i < 18; i++) client.append_static_segment("x", 1);
    const auto count = client.build_segment_iov(borrowed, bytes);
    check(count == 16 && borrowed && bytes == 27, "bounded iovec window includes borrow");
    auto* message = client.send_msg();
    check(message && message->msg_iovlen == count && client.send_state_bytes() == 312,
          "first segmented send materializes exactly one body");
    auto* iov = message->msg_iov;
    iovec submitted[16];
    std::memcpy(submitted, iov, sizeof(submitted));
    client.set_send_inflight(true);
    check(!client.safe_to_release() && !client.migration_protocol_idle(),
          "kernel-held body prevents connection release and migration");
    for (unsigned i = 0; i < 40; i++) client.append_static_segment("y", 1);
    check(client.send_msg() == message && message->msg_iov == iov &&
          !std::memcmp(submitted, iov, sizeof(submitted)),
          "queue growth cannot move or rewrite in-flight kernel descriptors");
    unsigned released = 0;
    auto release = [&](int32_t shard, const char* ptr) {
        check(shard == 3 && ptr == value, "borrow release returns to its original owner");
        ++released;
    };
    client.set_send_inflight(false); // simulated short-send CQE; no syscall
    check(client.consume_segments(2, release) == 0 && released == 0, "short prefix keeps borrow pinned");
    check(client.build_segment_iov(borrowed, bytes) == 16 &&
          client.send_msg() == message && message->msg_iov == iov && iov[0].iov_len == 3 &&
          std::string_view(static_cast<const char*>(iov[0].iov_base), 3) == "K\r\n",
          "resubmission keeps both addresses and advances the byte frontier");
    check(client.consume_segments(11, release) == 8 && released == 1, "borrow released once at its end");
    client.release_all_segments(release);
    check(released == 1 && !client.has_pending_segments(), "remaining static segments add no release");
    check(client.send_msg() == message && client.send_state_bytes() == 312,
          "drained body remains stable for later sends and quiescent migration");
    check(client.safe_to_release() && client.migration_protocol_idle(),
          "retaining an idle body does not prevent release or migration");
}

#ifdef TOMO_STAGE_SPLIT_ALLOCATIONS
static void allocation_contract() {
    const size_t start = allocations;
    tomo::Op standalone;
    tomo::Rob<64> rob;
    check(allocations == start, "empty Op and ROB allocate nothing");
    auto* op = rob.acquire<false>();
    check(op && allocations == start + 1, "first acquisition allocates one chunk, no sidecar");
    const size_t warm = allocations;
    for (unsigned round = 0; round < 8; ++round) {
        op->sink().append("+OK\r\n");
        rob.publish();
        op->state.store(tomo::OpState::Done, std::memory_order_release);
        check(rob.drain([](tomo::Op&) {}) == 1, "inline byte reply retires");
        if (round != 7) op = rob.acquire<false>();
    }
    check(allocations == warm, "eight short byte replies reuse the one chunk allocation");
    standalone.sink().append("+OK\r\n");
    check(allocations == warm + 1, "standalone byte reply allocation is observed");
    standalone.clear_reply();
    standalone.sink().append("+OK\r\n");
    check(allocations == warm + 1, "standalone reply reuses its allocation");

    const size_t before_client = allocations;
    tomo::Client client(-1);
    check(allocations == before_client + 1, "idle Client allocates only its existing receive buffer");
    bool borrow;
    uint32_t bytes;
    const size_t idle = allocations;
    client.build_segment_iov(borrow, bytes);
    client.append_fill("+OK\r\n", 5);
    check(allocations == idle, "empty window and ordinary byte reply allocate nothing");
    // Static descriptors exercise only SendState: sealing the fill would also allocate a queue
    // payload, obscuring the mechanism being priced here.
    client.append_static_segment("x", 1);
    const size_t before_body = allocations, before_bytes = allocated_bytes;
    check(client.build_segment_iov(borrow, bytes) == 1, "first send window contains real work");
    check(allocations == before_body + 1 && allocated_bytes == before_bytes + 312,
          "first segmented send allocates exactly the 312-byte body");
    const size_t with_body = allocations;
    check(client.build_segment_iov(borrow, bytes) == 1 && allocations == with_body,
          "subsequent window reuses the body");
    std::puts("stage-split allocations: idle Op/ROB=0; ROB chunk=1; 8 inline replies=0 extra; "
              "idle Client=1 rbuf; byte send=0 extra; segmented body=1 x 312 bytes; rebuild=0");
}
#endif

int main() {
    op_storage();
    rob_recycle<false>();
    rob_recycle<true>();
    standalone();
    send_storage();
#ifdef TOMO_STAGE_SPLIT_ALLOCATIONS
    allocation_contract();
#endif
    std::puts("stage-split unit: Op/Client lifetimes, both ROB code modes, wrap, short sends and borrow release passed");
}

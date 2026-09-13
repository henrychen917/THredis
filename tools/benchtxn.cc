// benchtxn.cc — native feature-cell load driver.
//
// WHY THIS EXISTS. The feature cells that pythons cannot saturate: MULTI/EXEC transactions, BLPOP
// producer/consumer wakes, and pub/sub fanout all measured "at driver ceiling" under the python
// harness — the server, the redis oracle and dragonfly all pinned at the same number because the
// DRIVER was the bottleneck. This driver exists to raise that ceiling until the SERVER is the
// bottleneck again: pipelined writers, bulk readers, and a frame-accurate RESP skip-parser that
// counts complete replies without materializing anything.
//
//   ./benchtxn exec   HOST PORT SECS CONNS THREADS PIPE NCMDS   — PIPE txns in flight/conn,
//                                                                 NCMDS SETs per MULTI..EXEC
//   ./benchtxn blpop  HOST PORT SECS PAIRS THREADS              — PAIRS (pusher,popper) pairs,
//                                                                 poppers block on private keys
//   ./benchtxn fanout HOST PORT SECS SUBS PUBS THREADS PIPE [s] — SUBS subscribers on one channel,
//                                                                 PUBS pipelined publishers;
//                                                                 trailing 's' = sharded variant
//   ./benchtxn mcmd  HOST PORT SECS CONNS THREADS PIPE MODE NKEYS KEYSPACE [rate_cap]
//                    MODE = mset | mget | mix.  Multi-key cells: redis-benchmark ceilings near
//                    600k multi-key cmd/s, so the comparison matrix drives them from here.
//                    rate_cap (ops/s, 0 = uncapped) exists because instructions/op counts idle
//                    spin on this architecture and is only comparable at a MATCHED achieved rate.
//
// Output: one machine-parseable line,  <mode> ops <n> secs <s> rate <ops/s> [deliv <n> drate <d/s>]
// where ops = EXECs completed / pops completed / publishes acked, and deliv = message frames
// counted at the subscribers (fanout only). Rates are the steady window (after a warm fraction).
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static int dial(const char* host, int port) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0) return -1;
    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) != 0) { close(fd); fd = -1; }
    freeaddrinfo(res);
    if (fd >= 0) { int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); }
    return fd;
}

static bool send_all(int fd, const char* p, size_t n) {
    while (n) {
        ssize_t w = send(fd, p, n, 0);
        if (w <= 0) return false;
        p += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

// Frame-accurate RESP reply counter. Consumes a byte stream incrementally; count() returns the
// number of COMPLETE top-level replies seen. Handles +,-,:,$,* (recursively via a pending-frames
// stack held as a counter, since arrays only nest as counts here).
struct RespCounter {
    uint64_t done = 0;
    // parser state
    int stage = 0;           // 0 = at type byte, 1 = reading line, 2 = reading bulk body
    long need = 0;           // bulk bytes (+2 crlf) still to consume in stage 2
    long open = 0;           // outstanding frames of the current top-level reply
    char type = 0;
    std::string line;

    void feed(const char* p, size_t n) {
        size_t i = 0;
        while (i < n) {
            if (stage == 0) {
                type = p[i++];
                line.clear();
                stage = 1;
                if (open == 0) open = 1;   // starting a new top-level reply
                continue;
            }
            if (stage == 1) {
                // accumulate until \n; line holds everything since the type byte (minus \r\n)
                const void* nl = memchr(p + i, '\n', n - i);
                size_t take = nl ? static_cast<size_t>(static_cast<const char*>(nl) - (p + i)) + 1
                                 : n - i;
                line.append(p + i, take);
                i += take;
                if (!nl) return;
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                    line.pop_back();
                if (type == '$') {
                    long len = atol(line.c_str());
                    if (len < 0) { close_frame(); }
                    else { need = len + 2; stage = 2; continue; }
                } else if (type == '*') {
                    long len = atol(line.c_str());
                    open--;                       // the array header frame itself is resolved...
                    open += len < 0 ? 0 : len;    // ...and its children are now outstanding
                    if (open == 0) done++;
                    stage = 0;
                    continue;
                } else {
                    close_frame();
                }
                stage = 0;
                continue;
            }
            // stage 2: bulk body
            long take = static_cast<long>(n - i) < need ? static_cast<long>(n - i) : need;
            i += static_cast<size_t>(take);
            need -= take;
            if (need == 0) { close_frame(); stage = 0; }
        }
    }
    void close_frame() {
        if (--open == 0) done++;
    }
};

static uint64_t now_us() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

static std::string bulk(const std::string& s) {
    return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n";
}
static std::string cmd(std::initializer_list<std::string> parts) {
    std::string out = "*" + std::to_string(parts.size()) + "\r\n";
    for (const auto& p : parts) out += bulk(p);
    return out;
}

// ---- exec mode ----------------------------------------------------------------------------------
// Each connection keeps PIPE transactions in flight; a transaction is MULTI + NCMDS SETs + EXEC =
// (NCMDS + 2) replies. We count EXEC completions = total replies / (NCMDS + 2).
static void run_exec(const char* host, int port, int secs, int conns, int nthreads, int pipe_depth,
                     int ncmds) {
    std::atomic<uint64_t> total{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    const int per = conns / nthreads > 0 ? conns / nthreads : 1;
    for (int t = 0; t < nthreads; t++)
        ts.emplace_back([&, t] {
            std::vector<int> fds;
            std::vector<RespCounter> cnt(static_cast<size_t>(per));
            std::vector<std::string> blob(static_cast<size_t>(per));
            for (int c = 0; c < per; c++) {
                int fd = dial(host, port);
                if (fd < 0) { fprintf(stderr, "dial failed\n"); exit(2); }
                fds.push_back(fd);
                std::string txn;
                for (int p = 0; p < pipe_depth; p++) {
                    txn += cmd({"MULTI"});
                    for (int k = 0; k < ncmds; k++)
                        txn += cmd({"SET", "tx" + std::to_string(t) + "_" + std::to_string(c) +
                                               "_" + std::to_string(k),
                                    "v0123456789abcdef"});
                    txn += cmd({"EXEC"});
                }
                blob[static_cast<size_t>(c)] = txn;
                send_all(fd, txn.data(), txn.size());
            }
            const long replies_per_txn = ncmds + 2;
            char buf[1 << 16];
            std::vector<uint64_t> credits(static_cast<size_t>(per), 0);
            while (!stop.load(std::memory_order_relaxed)) {
                for (int c = 0; c < per; c++) {
                    ssize_t r = recv(fds[static_cast<size_t>(c)], buf, sizeof buf, MSG_DONTWAIT);
                    if (r <= 0) continue;
                    RespCounter& rc = cnt[static_cast<size_t>(c)];
                    const uint64_t before = rc.done;
                    rc.feed(buf, static_cast<size_t>(r));
                    const uint64_t txns =
                        rc.done / static_cast<uint64_t>(replies_per_txn) -
                        before / static_cast<uint64_t>(replies_per_txn);
                    if (txns) {
                        total.fetch_add(txns, std::memory_order_relaxed);
                        credits[static_cast<size_t>(c)] += txns;
                        // refill the window one whole pipeline at a time to keep writes chunky
                        if (credits[static_cast<size_t>(c)] >=
                            static_cast<uint64_t>(pipe_depth)) {
                            credits[static_cast<size_t>(c)] -= static_cast<uint64_t>(pipe_depth);
                            send_all(fds[static_cast<size_t>(c)], blob[static_cast<size_t>(c)].data(),
                                     blob[static_cast<size_t>(c)].size());
                        }
                    }
                }
            }
            for (int fd : fds) close(fd);
        });
    const uint64_t t0 = now_us();
    // warm 20%, then measure
    std::this_thread::sleep_for(std::chrono::milliseconds(secs * 200));
    const uint64_t warm_ops = total.load();
    const uint64_t t1 = now_us();
    std::this_thread::sleep_for(std::chrono::milliseconds(secs * 800));
    const uint64_t t2 = now_us();
    const uint64_t ops = total.load() - warm_ops;
    stop.store(true);
    for (auto& th : ts) th.join();
    (void)t0;
    printf("exec ops %llu secs %.3f rate %.0f\n", static_cast<unsigned long long>(ops),
           (t2 - t1) / 1e6, ops / ((t2 - t1) / 1e6));
}

// ---- blpop mode ---------------------------------------------------------------------------------
// PAIRS private keys; popper c blocks on key c, pusher c LPUSHes key c whenever its popper's pop
// completed — a ping-pong that measures the block/wake/serve round trip capacity, not list ops.
static void run_blpop(const char* host, int port, int secs, int pairs, int nthreads) {
    std::atomic<uint64_t> total{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    const int per = pairs / nthreads > 0 ? pairs / nthreads : 1;
    for (int t = 0; t < nthreads; t++)
        ts.emplace_back([&, t] {
            struct Pair { int push_fd; int pop_fd; RespCounter pop_cnt; RespCounter push_cnt;
                          std::string key; uint64_t pops = 0; };
            std::vector<Pair> ps(static_cast<size_t>(per));
            for (int c = 0; c < per; c++) {
                Pair& p = ps[static_cast<size_t>(c)];
                p.key = "blq" + std::to_string(t) + "_" + std::to_string(c);
                p.push_fd = dial(host, port);
                p.pop_fd = dial(host, port);
                if (p.push_fd < 0 || p.pop_fd < 0) { fprintf(stderr, "dial failed\n"); exit(2); }
                const std::string pop = cmd({"BLPOP", p.key, "0"});
                send_all(p.pop_fd, pop.data(), pop.size());
                const std::string push = cmd({"LPUSH", p.key, "x"});
                send_all(p.push_fd, push.data(), push.size());
            }
            char buf[1 << 14];
            while (!stop.load(std::memory_order_relaxed)) {
                for (auto& p : ps) {
                    ssize_t r = recv(p.pop_fd, buf, sizeof buf, MSG_DONTWAIT);
                    if (r > 0) {
                        const uint64_t before = p.pop_cnt.done;
                        p.pop_cnt.feed(buf, static_cast<size_t>(r));
                        uint64_t got = p.pop_cnt.done - before;
                        if (got) {
                            total.fetch_add(got, std::memory_order_relaxed);
                            // re-arm both sides of the ping-pong
                            std::string next = cmd({"BLPOP", p.key, "0"});
                            send_all(p.pop_fd, next.data(), next.size());
                            std::string push = cmd({"LPUSH", p.key, "x"});
                            while (got--) send_all(p.push_fd, push.data(), push.size());
                        }
                    }
                    r = recv(p.push_fd, buf, sizeof buf, MSG_DONTWAIT);
                    if (r > 0) p.push_cnt.feed(buf, static_cast<size_t>(r));
                }
            }
            for (auto& p : ps) { close(p.push_fd); close(p.pop_fd); }
        });
    std::this_thread::sleep_for(std::chrono::milliseconds(secs * 200));
    const uint64_t warm = total.load();
    const uint64_t t1 = now_us();
    std::this_thread::sleep_for(std::chrono::milliseconds(secs * 800));
    const uint64_t t2 = now_us();
    const uint64_t ops = total.load() - warm;
    stop.store(true);
    for (auto& th : ts) th.join();
    printf("blpop ops %llu secs %.3f rate %.0f\n", static_cast<unsigned long long>(ops),
           (t2 - t1) / 1e6, ops / ((t2 - t1) / 1e6));
}

// ---- fanout mode --------------------------------------------------------------------------------
// SUBS subscriber conns on one channel; PUBS publisher conns each keep PIPE publishes in flight.
// ops = publish acks, deliv = message frames counted across subscribers. Every subscriber runs its
// own counter so delivery is frame-accurate, not byte-guessed.
static void run_fanout(const char* host, int port, int secs, int subs, int pubs, int nthreads,
                       int pipe_depth, bool sharded) {
    std::atomic<uint64_t> pub_total{0}, deliv_total{0};
    std::atomic<bool> stop{false};
    std::atomic<int> ready{0};
    const char* chan = "bench:chan";
    std::vector<std::thread> ts;
    // subscriber threads
    const int sub_threads = nthreads / 2 > 0 ? nthreads / 2 : 1;
    const int per_sub = subs / sub_threads > 0 ? subs / sub_threads : 1;
    for (int t = 0; t < sub_threads; t++)
        ts.emplace_back([&, t] {
            std::vector<int> fds;
            std::vector<RespCounter> cnt(static_cast<size_t>(per_sub));
            for (int c = 0; c < per_sub; c++) {
                int fd = dial(host, port);
                if (fd < 0) { fprintf(stderr, "dial failed\n"); exit(2); }
                const std::string sub = cmd({sharded ? "SSUBSCRIBE" : "SUBSCRIBE", chan});
                send_all(fd, sub.data(), sub.size());
                fds.push_back(fd);
            }
            // swallow the subscribe confirmations before counting
            char buf[1 << 16];
            for (int c = 0; c < per_sub; c++) {
                ssize_t r = recv(fds[static_cast<size_t>(c)], buf, sizeof buf, 0);
                if (r > 0) cnt[static_cast<size_t>(c)].feed(buf, static_cast<size_t>(r));
                cnt[static_cast<size_t>(c)].done = 0;
            }
            ready.fetch_add(per_sub);
            std::vector<uint64_t> seen(static_cast<size_t>(per_sub), 0);
            while (!stop.load(std::memory_order_relaxed)) {
                for (int c = 0; c < per_sub; c++) {
                    ssize_t r = recv(fds[static_cast<size_t>(c)], buf, sizeof buf, MSG_DONTWAIT);
                    if (r <= 0) continue;
                    RespCounter& rc = cnt[static_cast<size_t>(c)];
                    rc.feed(buf, static_cast<size_t>(r));
                    const uint64_t d = rc.done - seen[static_cast<size_t>(c)];
                    seen[static_cast<size_t>(c)] = rc.done;
                    if (d) deliv_total.fetch_add(d, std::memory_order_relaxed);
                }
            }
            for (int fd : fds) close(fd);
        });
    while (ready.load() < per_sub * sub_threads) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    // publisher threads
    const int pub_threads = nthreads - sub_threads > 0 ? nthreads - sub_threads : 1;
    const int per_pub = pubs / pub_threads > 0 ? pubs / pub_threads : 1;
    for (int t = 0; t < pub_threads; t++)
        ts.emplace_back([&, t] {
            std::vector<int> fds;
            std::vector<RespCounter> cnt(static_cast<size_t>(per_pub));
            std::string one = cmd({sharded ? "SPUBLISH" : "PUBLISH", chan, "m0123456789abcdef"});
            std::string blob;
            for (int p = 0; p < pipe_depth; p++) blob += one;
            for (int c = 0; c < per_pub; c++) {
                int fd = dial(host, port);
                if (fd < 0) { fprintf(stderr, "dial failed\n"); exit(2); }
                send_all(fd, blob.data(), blob.size());
                fds.push_back(fd);
            }
            char buf[1 << 16];
            std::vector<uint64_t> credits(static_cast<size_t>(per_pub), 0);
            std::vector<uint64_t> seen(static_cast<size_t>(per_pub), 0);
            while (!stop.load(std::memory_order_relaxed)) {
                for (int c = 0; c < per_pub; c++) {
                    ssize_t r = recv(fds[static_cast<size_t>(c)], buf, sizeof buf, MSG_DONTWAIT);
                    if (r <= 0) continue;
                    RespCounter& rc = cnt[static_cast<size_t>(c)];
                    rc.feed(buf, static_cast<size_t>(r));
                    const uint64_t acks = rc.done - seen[static_cast<size_t>(c)];
                    seen[static_cast<size_t>(c)] = rc.done;
                    if (acks) {
                        pub_total.fetch_add(acks, std::memory_order_relaxed);
                        credits[static_cast<size_t>(c)] += acks;
                        if (credits[static_cast<size_t>(c)] >= static_cast<uint64_t>(pipe_depth)) {
                            credits[static_cast<size_t>(c)] -= static_cast<uint64_t>(pipe_depth);
                            send_all(fds[static_cast<size_t>(c)], blob.data(), blob.size());
                        }
                    }
                }
            }
            for (int fd : fds) close(fd);
        });
    std::this_thread::sleep_for(std::chrono::milliseconds(secs * 200));
    const uint64_t warm_p = pub_total.load(), warm_d = deliv_total.load();
    const uint64_t t1 = now_us();
    std::this_thread::sleep_for(std::chrono::milliseconds(secs * 800));
    const uint64_t t2 = now_us();
    const uint64_t p = pub_total.load() - warm_p, d = deliv_total.load() - warm_d;
    stop.store(true);
    for (auto& th : ts) th.join();
    printf("fanout ops %llu secs %.3f rate %.0f deliv %llu drate %.0f\n",
           static_cast<unsigned long long>(p), (t2 - t1) / 1e6, p / ((t2 - t1) / 1e6),
           static_cast<unsigned long long>(d), d / ((t2 - t1) / 1e6));
}

// ---- mcmd mode ----------------------------------------------------------------------------------
// Pipelined multi-key GET/SET cells (MGET5/MSET5). redis-benchmark tops out near 600k cmd/s
// generating these (per-request argv substitution), which reads as "MGET == MSET" — a driver
// ceiling, not a server measurement. Here each connection pre-renders a pool of distinct
// command blobs over a random keyspace at startup and rotates them, so steady-state send cost
// is a memcpy and the server is the thing being measured again.
//   ./benchtxn mcmd HOST PORT SECS CONNS THREADS PIPE mget|mset [NKEYS] [KEYSPACE]
static void run_mcmd(const char* host, int port, int secs, int conns, int nthreads,
                     int pipe_depth, bool is_set, int nkeys, int keyspace, bool mix = false,
                     double rate_cap = 0.0) {
    std::atomic<uint64_t> total{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    const int per = conns / nthreads > 0 ? conns / nthreads : 1;
    for (int t = 0; t < nthreads; t++)
        ts.emplace_back([&, t] {
            unsigned seed = 0x9e3779b9u * static_cast<unsigned>(t + 1);
            auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) ^ (seed >> 20); };
            // mix mode alternates MSET/MGET inside the pipeline so both classes contend in one
            // run (the "interleaved" cell); solo modes keep a single class per connection.
            int mix_counter = 0;
            auto one_cmd = [&] {
                const bool this_set = mix ? ((mix_counter++ & 1) == 0) : is_set;
                std::vector<std::string> parts;
                parts.push_back(this_set ? "MSET" : "MGET");
                for (int k = 0; k < nkeys; k++) {
                    parts.push_back("m" + std::to_string(k) + ":" +
                                    std::to_string(rnd() % static_cast<unsigned>(keyspace)));
                    if (this_set) parts.push_back("v0123456789abcdef");
                }
                std::string out = "*" + std::to_string(parts.size()) + "\r\n";
                for (const auto& p : parts) out += bulk(p);
                return out;
            };
            // 64 distinct pre-rendered pipeline blobs per connection; rotation keeps the key
            // stream varied without any per-request formatting on the hot loop.
            const int kPool = 256;
            std::vector<int> fds;
            std::vector<RespCounter> cnt(static_cast<size_t>(per));
            std::vector<std::vector<std::string>> pools(static_cast<size_t>(per));
            std::vector<uint32_t> next(static_cast<size_t>(per), 0);
            for (int c = 0; c < per; c++) {
                int fd = dial(host, port);
                if (fd < 0) { fprintf(stderr, "dial failed\n"); exit(2); }
                fds.push_back(fd);
                auto& pool = pools[static_cast<size_t>(c)];
                pool.reserve(kPool);
                for (int b = 0; b < kPool; b++) {
                    std::string blob;
                    for (int p = 0; p < pipe_depth; p++) blob += one_cmd();
                    pool.push_back(std::move(blob));
                }
                send_all(fd, pool[0].data(), pool[0].size());
                next[static_cast<size_t>(c)] = 1;
            }
            char buf[1 << 16];
            std::vector<uint64_t> credits(static_cast<size_t>(per), 0);
            std::vector<uint64_t> seen(static_cast<size_t>(per), 0);
            const double per_thread_cap = rate_cap > 0 ? rate_cap / nthreads : 0.0;
            const uint64_t t_start = now_us();
            uint64_t issued = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                if (per_thread_cap > 0) {
                    const double elapsed = (now_us() - t_start) / 1e6;
                    if (issued > per_thread_cap * elapsed) {
                        std::this_thread::sleep_for(std::chrono::microseconds(50));
                        continue;
                    }
                }
                for (int c = 0; c < per; c++) {
                    ssize_t r = recv(fds[static_cast<size_t>(c)], buf, sizeof buf, MSG_DONTWAIT);
                    if (r <= 0) continue;
                    RespCounter& rc = cnt[static_cast<size_t>(c)];
                    rc.feed(buf, static_cast<size_t>(r));
                    const uint64_t done = rc.done - seen[static_cast<size_t>(c)];
                    seen[static_cast<size_t>(c)] = rc.done;
                    if (done) {
                        total.fetch_add(done, std::memory_order_relaxed);
                        issued += done;
                        credits[static_cast<size_t>(c)] += done;
                        if (credits[static_cast<size_t>(c)] >= static_cast<uint64_t>(pipe_depth)) {
                            credits[static_cast<size_t>(c)] -= static_cast<uint64_t>(pipe_depth);
                            auto& pool = pools[static_cast<size_t>(c)];
                            const std::string& blob =
                                pool[next[static_cast<size_t>(c)]++ % pool.size()];
                            send_all(fds[static_cast<size_t>(c)], blob.data(), blob.size());
                        }
                    }
                }
            }
            for (int fd : fds) close(fd);
        });
    std::this_thread::sleep_for(std::chrono::milliseconds(secs * 200));
    const uint64_t warm = total.load();
    const uint64_t t1 = now_us();
    std::this_thread::sleep_for(std::chrono::milliseconds(secs * 800));
    const uint64_t t2 = now_us();
    const uint64_t ops = total.load() - warm;
    stop.store(true);
    for (auto& th : ts) th.join();
    printf("mcmd %s ops %llu secs %.3f rate %.0f keyrate %.0f\n",
           mix ? "mix" : (is_set ? "mset" : "mget"),
           static_cast<unsigned long long>(ops), (t2 - t1) / 1e6, ops / ((t2 - t1) / 1e6),
           ops * static_cast<double>(nkeys) / ((t2 - t1) / 1e6));
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: benchtxn exec|blpop|fanout|mcmd HOST PORT ... (see header)\n");
        return 1;
    }
    const std::string mode = argv[1];
    const char* host = argv[2];
    const int port = atoi(argv[3]);
    if (mode == "exec" && argc >= 9)
        run_exec(host, port, atoi(argv[4]), atoi(argv[5]), atoi(argv[6]), atoi(argv[7]),
                 atoi(argv[8]));
    else if (mode == "blpop" && argc >= 6)
        run_blpop(host, port, atoi(argv[4]), atoi(argv[5]), atoi(argv[6]));
    else if (mode == "fanout" && argc >= 8)
        run_fanout(host, port, atoi(argv[4]), atoi(argv[5]), atoi(argv[6]), atoi(argv[7]),
                   argc >= 9 ? atoi(argv[8]) : 16, argc >= 10 && argv[9][0] == 's');
    else if (mode == "mcmd" && argc >= 9)
        run_mcmd(host, port, atoi(argv[4]), atoi(argv[5]), atoi(argv[6]), atoi(argv[7]),
                 std::string(argv[8]) == "mset", argc >= 10 ? atoi(argv[9]) : 5,
                 argc >= 11 ? atoi(argv[10]) : 2000000,
                 std::string(argv[8]) == "mix",
                 argc >= 12 ? atof(argv[11]) : 0.0);
    else {
        fprintf(stderr, "bad args for mode %s\n", mode.c_str());
        return 1;
    }
    return 0;
}

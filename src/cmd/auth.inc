// auth.inc -- out-of-line requirepass dispatch gate; textually included by xshard.cc.

#include <linux/sockios.h>
#include <sys/ioctl.h>

namespace tomo {
namespace {

constexpr uint32_t kSha256Round[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

uint32_t sha_rotr(uint32_t value, uint32_t count) {
    return (value >> count) | (value << (32 - count));
}

void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
    uint32_t words[64];
    for (uint32_t i = 0; i < 16; i++) {
        const uint32_t p = i * 4;
        words[i] = (static_cast<uint32_t>(block[p]) << 24) |
                   (static_cast<uint32_t>(block[p + 1]) << 16) |
                   (static_cast<uint32_t>(block[p + 2]) << 8) |
                   static_cast<uint32_t>(block[p + 3]);
    }
    for (uint32_t i = 16; i < 64; i++) {
        const uint32_t s0 = sha_rotr(words[i - 15], 7) ^ sha_rotr(words[i - 15], 18) ^
                            (words[i - 15] >> 3);
        const uint32_t s1 = sha_rotr(words[i - 2], 17) ^ sha_rotr(words[i - 2], 19) ^
                            (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (uint32_t i = 0; i < 64; i++) {
        const uint32_t sum1 = sha_rotr(e, 6) ^ sha_rotr(e, 11) ^ sha_rotr(e, 25);
        const uint32_t choose = (e & f) ^ (~e & g);
        const uint32_t temp1 = h + sum1 + choose + kSha256Round[i] + words[i];
        const uint32_t sum0 = sha_rotr(a, 2) ^ sha_rotr(a, 13) ^ sha_rotr(a, 22);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = sum0 + majority;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

std::array<uint64_t, 4> sha256_password(Slice input) {
    uint32_t state[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                         0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    uint32_t consumed = 0;
    while (input.n - consumed >= 64) {
        sha256_transform(state, reinterpret_cast<const uint8_t*>(input.p + consumed));
        consumed += 64;
    }

    uint8_t tail[128] = {};
    const uint32_t remainder = input.n - consumed;
    if (remainder) std::memcpy(tail, input.p + consumed, remainder);
    tail[remainder] = 0x80;
    const uint32_t padded = remainder < 56 ? 64 : 128;
    const uint64_t bits = static_cast<uint64_t>(input.n) * 8;
    for (uint32_t i = 0; i < 8; i++)
        tail[padded - 1 - i] = static_cast<uint8_t>(bits >> (i * 8));
    sha256_transform(state, tail);
    if (padded == 128) sha256_transform(state, tail + 64);

    uint8_t bytes[32];
    for (uint32_t i = 0; i < 8; i++) {
        bytes[i * 4] = static_cast<uint8_t>(state[i] >> 24);
        bytes[i * 4 + 1] = static_cast<uint8_t>(state[i] >> 16);
        bytes[i * 4 + 2] = static_cast<uint8_t>(state[i] >> 8);
        bytes[i * 4 + 3] = static_cast<uint8_t>(state[i]);
    }
    std::array<uint64_t, 4> digest;
    std::memcpy(digest.data(), bytes, sizeof(bytes));
    return digest;
}

bool auth_allowed_without_credentials(const Op& op) {
    const Slice name = op.cmd_name();
    return name.eq_icase("auth") || name.eq_icase("hello") ||
           name.eq_icase("reset") || name.eq_icase("quit");
}

bool unauthenticated_output_limit_exceeded(const Client& client) {
    int kernel_bytes = 0;
    if (::ioctl(client.fd(), SIOCOUTQ, &kernel_bytes) != 0 || kernel_bytes < 0) kernel_bytes = 0;
    return client.buffered_output_bytes() + static_cast<uint32_t>(kernel_bytes) > 1024;
}

}  // namespace

std::array<uint64_t, 4> auth_password_digest(Slice password) {
    return sha256_password(password);
}

bool auth_dispatch_entry(IoLoop& loop, Client& client, Op& op, uint32_t consumed) {
    if (unauthenticated_output_limit_exceeded(client)) {
        client.advance_parse(consumed);
        loop.finish_prebuilt(&client, op);
        loop.close_client(&client);
        return true;
    }
    if (auth_allowed_without_credentials(op)) return false;
    client.advance_parse(consumed);
    loop.self_->note_command(op.spec->id);
    reply_err(op.sink(), "NOAUTH Authentication required.");
    loop.finish_prebuilt(&client, op);
    return true;
}

void auth_publish_requirepass(Server& server, Slice password) {
    static const bool kSha256Verified = [] {
        static constexpr uint8_t kAbcDigest[32] = {
            0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
            0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
            0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
            0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
        };
        const std::array<uint64_t, 4> digest = sha256_password(Slice("abc", 3));
        return std::memcmp(digest.data(), kAbcDigest, sizeof(kAbcDigest)) == 0;
    }();
    if (!kSha256Verified) std::abort();
    const bool required = !password.empty();
    server.set_auth_config(required);
    acl_update_default_requirepass(server, password);
}

}  // namespace tomo

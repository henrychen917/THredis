// hll.cc -- byte-faithful Redis HyperLogLog encoding.
//
// An HLL is a normal string whose bytes are an external interoperability contract: GET, STRLEN,
// snapshots, and Redis tooling all observe the HYLL header and its dense/sparse payload. This is
// intentionally a representation-faithful port of Redis src/hyperloglog.c rather than a native C++
// sketch. Here foreign-format fidelity beats native invention; changing opcode normalization,
// promotion order, MurmurHash64A, or cache bytes would be a compatibility bug.
#include "hll.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>

namespace tomo::hll {
namespace {

constexpr uint8_t kDense = 0;
constexpr uint8_t kSparse = 1;
constexpr uint32_t kP = 14;
constexpr uint32_t kQ = 64 - kP;
constexpr uint32_t kRegisterMask = kRegisters - 1;
constexpr uint8_t kRegisterMax = 63;
uint32_t g_sparse_max_bytes = 3000;  // Boot-latched before any worker can use the HLL helpers.
constexpr double kAlphaInf = 0.721347520444481703680;

inline const uint8_t* bytes(Slice image) {
    return reinterpret_cast<const uint8_t*>(image.p);
}

inline uint8_t* bytes(std::string& image) {
    return reinterpret_cast<uint8_t*>(image.data());
}

inline bool sparse_zero(uint8_t opcode) { return (opcode & 0xc0) == 0; }
inline bool sparse_xzero(uint8_t opcode) { return (opcode & 0xc0) == 0x40; }
inline bool sparse_val(uint8_t opcode) { return (opcode & 0x80) != 0; }
inline uint32_t zero_len(uint8_t opcode) { return (opcode & 0x3f) + 1; }
inline uint32_t xzero_len(const uint8_t* opcode) {
    return (((opcode[0] & 0x3f) << 8) | opcode[1]) + 1;
}
inline uint8_t val_value(uint8_t opcode) { return ((opcode >> 2) & 0x1f) + 1; }
inline uint32_t val_len(uint8_t opcode) { return (opcode & 0x03) + 1; }
inline uint8_t make_val(uint8_t value, uint32_t length) {
    return static_cast<uint8_t>(((value - 1) << 2) | (length - 1) | 0x80);
}
inline uint8_t make_zero(uint32_t length) { return static_cast<uint8_t>(length - 1); }
inline void make_xzero(uint8_t* out, uint32_t length) {
    const uint32_t encoded = length - 1;
    out[0] = static_cast<uint8_t>((encoded >> 8) | 0x40);
    out[1] = static_cast<uint8_t>(encoded);
}

uint8_t dense_get(const uint8_t* registers, uint32_t index) {
    const uint32_t bit = index * 6;
    const uint32_t byte = bit / 8;
    const uint32_t first_bit = bit & 7;
    const uint32_t b0 = registers[byte];
    const uint32_t b1 = byte + 1 < kDenseBytes ? registers[byte + 1] : 0;
    return static_cast<uint8_t>(((b0 >> first_bit) | (b1 << (8 - first_bit))) &
                                kRegisterMax);
}

void dense_put(uint8_t* registers, uint32_t index, uint8_t value) {
    const uint32_t bit = index * 6;
    const uint32_t byte = bit / 8;
    const uint32_t first_bit = bit & 7;
    const uint32_t next_bits = 8 - first_bit;
    registers[byte] &= static_cast<uint8_t>(~(kRegisterMax << first_bit));
    registers[byte] |= static_cast<uint8_t>(value << first_bit);
    // Redis relies on SDS's trailing NUL for the last register. No significant bits spill there.
    if (byte + 1 < kDenseBytes) {
        registers[byte + 1] &= static_cast<uint8_t>(~(kRegisterMax >> next_bits));
        registers[byte + 1] |= static_cast<uint8_t>(value >> next_bits);
    }
}

int dense_set(uint8_t* registers, uint32_t index, uint8_t value) {
    if (value <= dense_get(registers, index)) return 0;
    dense_put(registers, index, value);
    return 1;
}

uint64_t murmur_hash64a(Slice element) {
    constexpr uint64_t multiplier = 0xc6a4a7935bd1e995ULL;
    constexpr uint32_t shift = 47;
    uint64_t hash = 0xadc83b19ULL ^ (static_cast<uint64_t>(element.n) * multiplier);
    const uint8_t* data = bytes(element);
    const uint8_t* end = data + (element.n - (element.n & 7));
    while (data != end) {
        uint64_t value = static_cast<uint64_t>(data[0]) |
                         (static_cast<uint64_t>(data[1]) << 8) |
                         (static_cast<uint64_t>(data[2]) << 16) |
                         (static_cast<uint64_t>(data[3]) << 24) |
                         (static_cast<uint64_t>(data[4]) << 32) |
                         (static_cast<uint64_t>(data[5]) << 40) |
                         (static_cast<uint64_t>(data[6]) << 48) |
                         (static_cast<uint64_t>(data[7]) << 56);
        value *= multiplier;
        value ^= value >> shift;
        value *= multiplier;
        hash ^= value;
        hash *= multiplier;
        data += 8;
    }
    switch (element.n & 7) {
        case 7: hash ^= static_cast<uint64_t>(data[6]) << 48; [[fallthrough]];
        case 6: hash ^= static_cast<uint64_t>(data[5]) << 40; [[fallthrough]];
        case 5: hash ^= static_cast<uint64_t>(data[4]) << 32; [[fallthrough]];
        case 4: hash ^= static_cast<uint64_t>(data[3]) << 24; [[fallthrough]];
        case 3: hash ^= static_cast<uint64_t>(data[2]) << 16; [[fallthrough]];
        case 2: hash ^= static_cast<uint64_t>(data[1]) << 8; [[fallthrough]];
        case 1: hash ^= static_cast<uint64_t>(data[0]); hash *= multiplier; [[fallthrough]];
        case 0: break;
    }
    hash ^= hash >> shift;
    hash *= multiplier;
    hash ^= hash >> shift;
    return hash;
}

uint8_t pattern_length(Slice element, uint32_t& index) {
    uint64_t hash = murmur_hash64a(element);
    index = static_cast<uint32_t>(hash & kRegisterMask);
    hash >>= kP;
    hash |= uint64_t{1} << kQ;
    return static_cast<uint8_t>(__builtin_ctzll(hash) + 1);
}

bool sparse_to_dense(std::string& image) {
    if (image.size() < kHeaderBytes) return false;
    if (static_cast<uint8_t>(image[4]) == kDense) return image.size() == kDenseSize;
    if (static_cast<uint8_t>(image[4]) != kSparse) return false;

    std::string dense(kDenseSize, '\0');
    std::memcpy(dense.data(), image.data(), kHeaderBytes);
    dense[4] = static_cast<char>(kDense);
    uint8_t* registers = bytes(dense) + kHeaderBytes;
    const uint8_t* source = bytes(Slice(image.data(), static_cast<uint32_t>(image.size())));
    size_t pos = kHeaderBytes;
    uint32_t index = 0;
    while (pos < image.size()) {
        const uint8_t opcode = source[pos];
        uint32_t run = 0;
        if (sparse_zero(opcode)) {
            run = zero_len(opcode);
            pos++;
        } else if (sparse_xzero(opcode)) {
            if (pos + 1 >= image.size()) return false;
            run = xzero_len(source + pos);
            pos += 2;
        } else {
            run = val_len(opcode);
            const uint8_t value = val_value(opcode);
            pos++;
            if (run > kRegisters - index) return false;
            for (uint32_t i = 0; i < run; i++) dense_put(registers, index++, value);
            continue;
        }
        if (run > kRegisters - index) return false;
        index += run;
    }
    if (index != kRegisters) return false;
    image.swap(dense);
    return true;
}

int promote_and_set(std::string& image, uint32_t index, uint8_t value) {
    if (!sparse_to_dense(image)) return -1;
    return dense_set(bytes(image) + kHeaderBytes, index, value);
}

bool coalesce_sparse_values(std::string& image, size_t previous) {
    size_t pos = previous == std::string::npos ? kHeaderBytes : previous;
    int scan = 5;
    while (pos < image.size() && scan--) {
        const uint8_t current = static_cast<uint8_t>(image[pos]);
        if (sparse_xzero(current)) {
            if (pos + 1 >= image.size()) return false;
            pos += 2;
            continue;
        }
        if (sparse_zero(current)) {
            pos++;
            continue;
        }
        if (pos + 1 < image.size() && sparse_val(static_cast<uint8_t>(image[pos + 1]))) {
            const uint8_t next = static_cast<uint8_t>(image[pos + 1]);
            if (val_value(current) == val_value(next)) {
                const uint32_t length = val_len(current) + val_len(next);
                if (length <= 4) {
                    image[pos + 1] = static_cast<char>(make_val(val_value(current), length));
                    image.erase(pos, 1);
                    continue;
                }
            }
        }
        pos++;
    }
    return true;
}

int sparse_set(std::string& image, uint32_t index, uint8_t value) {
    if (value > 32) return promote_and_set(image, index, value);

    size_t pos = kHeaderBytes;
    size_t previous = std::string::npos;
    uint32_t first = 0;
    uint32_t span = 0;
    size_t opcode_len = 0;
    while (pos < image.size()) {
        const uint8_t opcode = static_cast<uint8_t>(image[pos]);
        opcode_len = 1;
        if (sparse_zero(opcode)) {
            span = zero_len(opcode);
        } else if (sparse_val(opcode)) {
            span = val_len(opcode);
        } else {
            if (pos + 1 >= image.size()) return -1;
            span = xzero_len(bytes(image) + pos);
            opcode_len = 2;
        }
        if (index <= first + span - 1) break;
        previous = pos;
        pos += opcode_len;
        first += span;
    }
    if (!span || pos >= image.size()) return -1;

    const uint8_t opcode = static_cast<uint8_t>(image[pos]);
    const bool is_zero = sparse_zero(opcode);
    const bool is_xzero = sparse_xzero(opcode);
    const bool is_value = sparse_val(opcode);
    const uint32_t run = is_zero ? zero_len(opcode) :
                         is_xzero ? xzero_len(bytes(image) + pos) : val_len(opcode);

    if (is_value) {
        const uint8_t old = val_value(opcode);
        if (old >= value) return 0;
        if (run == 1) {
            image[pos] = static_cast<char>(make_val(value, 1));
            if (!coalesce_sparse_values(image, previous)) return -1;
            invalidate_cache(image);
            return 1;
        }
    }
    if (is_zero && run == 1) {
        image[pos] = static_cast<char>(make_val(value, 1));
        if (!coalesce_sparse_values(image, previous)) return -1;
        invalidate_cache(image);
        return 1;
    }

    uint8_t sequence[5];
    size_t sequence_len = 0;
    const uint32_t last = first + span - 1;
    if (is_zero || is_xzero) {
        if (index != first) {
            const uint32_t length = index - first;
            if (length > 64) {
                make_xzero(sequence + sequence_len, length);
                sequence_len += 2;
            } else {
                sequence[sequence_len++] = make_zero(length);
            }
        }
        sequence[sequence_len++] = make_val(value, 1);
        if (index != last) {
            const uint32_t length = last - index;
            if (length > 64) {
                make_xzero(sequence + sequence_len, length);
                sequence_len += 2;
            } else {
                sequence[sequence_len++] = make_zero(length);
            }
        }
    } else {
        const uint8_t current = val_value(opcode);
        if (index != first) sequence[sequence_len++] = make_val(current, index - first);
        sequence[sequence_len++] = make_val(value, 1);
        if (index != last) sequence[sequence_len++] = make_val(current, last - index);
    }

    const size_t old_len = is_xzero ? 2 : 1;
    if (sequence_len > old_len && image.size() + sequence_len - old_len > g_sparse_max_bytes)
        return promote_and_set(image, index, value);
    image.replace(pos, old_len, reinterpret_cast<const char*>(sequence), sequence_len);

    // Redis scans at most five opcodes from the predecessor and greedily coalesces adjacent VALs.
    if (!coalesce_sparse_values(image, previous)) return -1;
    invalidate_cache(image);
    return 1;
}

bool sparse_histogram(Slice image, int (&histogram)[64]) {
    const uint8_t* source = bytes(image);
    size_t pos = kHeaderBytes;
    uint32_t index = 0;
    while (pos < image.n) {
        const uint8_t opcode = source[pos];
        uint32_t run = 0;
        uint8_t value = 0;
        if (sparse_zero(opcode)) {
            run = zero_len(opcode);
            pos++;
        } else if (sparse_xzero(opcode)) {
            if (pos + 1 >= image.n) return false;
            run = xzero_len(source + pos);
            pos += 2;
        } else {
            run = val_len(opcode);
            value = val_value(opcode);
            pos++;
        }
        if (run > kRegisters - index) return false;
        index += run;
        histogram[value] += static_cast<int>(run);
    }
    return index == kRegisters;
}

double sigma(double x) {
    if (x == 1.0) return std::numeric_limits<double>::infinity();
    double previous;
    double y = 1;
    double z = x;
    do {
        x *= x;
        previous = z;
        z += x * y;
        y += y;
    } while (previous != z);
    return z;
}

double tau(double x) {
    if (x == 0.0 || x == 1.0) return 0.0;
    double previous;
    double y = 1.0;
    double z = 1 - x;
    do {
        x = std::sqrt(x);
        previous = z;
        y *= 0.5;
        z -= std::pow(1 - x, 2) * y;
    } while (previous != z);
    return z / 3;
}

uint64_t estimate(const int (&histogram)[64]) {
    const double registers = kRegisters;
    double z = registers * tau((registers - histogram[kQ + 1]) / registers);
    for (int j = kQ; j >= 1; --j) {
        z += histogram[j];
        z *= 0.5;
    }
    z += registers * sigma(histogram[0] / registers);
    const double result = std::llround(kAlphaInf * registers * registers / z);
    return static_cast<uint64_t>(result);
}

}  // namespace

void configure_sparse_max_bytes(uint32_t value) { g_sparse_max_bytes = value; }

bool header_valid(Slice image) {
    if (image.n < kHeaderBytes) return false;
    const uint8_t* data = bytes(image);
    if (std::memcmp(data, "HYLL", 4) != 0 || data[4] > kSparse) return false;
    return data[4] != kDense || image.n == kDenseSize;
}

bool is_dense(Slice image) { return header_valid(image) && bytes(image)[4] == kDense; }

bool make_dense(std::string& image) { return sparse_to_dense(image); }

bool decode_sparse(Slice image, std::string& decoded) {
    if (!header_valid(image) || bytes(image)[4] != kSparse) return false;
    decoded.clear();
    const uint8_t* source = bytes(image);
    size_t pos = kHeaderBytes;
    auto append_number = [&](uint32_t value) {
        char text[16];
        const auto result = std::to_chars(text, text + sizeof(text), value);
        decoded.append(text, static_cast<size_t>(result.ptr - text));
    };
    while (pos < image.n) {
        if (!decoded.empty()) decoded.push_back(' ');
        const uint8_t opcode = source[pos];
        if (sparse_zero(opcode)) {
            decoded += "z:";
            append_number(zero_len(opcode));
            pos++;
        } else if (sparse_xzero(opcode)) {
            // The diagnostic decoder reports physical opcodes rather than validating the stream.
            // A running Redis reads the string image's guaranteed trailing NUL as the missing low
            // XZERO byte, so reproduce that observed result without reading beyond the Slice.
            const uint8_t encoded[2] = {opcode, pos + 1 < image.n ? source[pos + 1] : uint8_t{0}};
            decoded += "Z:";
            append_number(xzero_len(encoded));
            pos += 2;
        } else {
            decoded += "v:";
            append_number(val_value(opcode));
            decoded.push_back(',');
            append_number(val_len(opcode));
            pos++;
        }
    }
    return true;
}

std::string create_sparse() {
    std::string image(kHeaderBytes + 2, '\0');
    std::memcpy(image.data(), "HYLL", 4);
    image[4] = static_cast<char>(kSparse);
    make_xzero(bytes(image) + kHeaderBytes, kRegisters);
    return image;
}

void invalidate_cache(std::string& image) {
    if (image.size() >= kHeaderBytes) image[15] = static_cast<char>(image[15] | 0x80);
}

bool cache_valid(Slice image) {
    return image.n >= kHeaderBytes && (bytes(image)[15] & 0x80) == 0;
}

uint64_t cached_count(Slice image) {
    const uint8_t* card = bytes(image) + 8;
    uint64_t result = 0;
    for (uint32_t i = 0; i < 8; i++) result |= static_cast<uint64_t>(card[i]) << (i * 8);
    return result;
}

void set_cached_count(std::string& image, uint64_t count_value) {
    for (uint32_t i = 0; i < 8; i++) image[8 + i] = static_cast<char>(count_value >> (i * 8));
}

int add(std::string& image, Slice element) {
    uint32_t index = 0;
    const uint8_t length = pattern_length(element, index);
    const uint8_t encoding = static_cast<uint8_t>(image[4]);
    if (encoding == kDense)
        return dense_set(bytes(image) + kHeaderBytes, index, length);
    if (encoding == kSparse) return sparse_set(image, index, length);
    return -1;
}

uint64_t count(Slice image, bool& corrupt) {
    int histogram[64] = {};
    if (bytes(image)[4] == kDense) {
        const uint8_t* registers = bytes(image) + kHeaderBytes;
        for (uint32_t i = 0; i < kRegisters; i++) histogram[dense_get(registers, i)]++;
    } else if (!sparse_histogram(image, histogram)) {
        corrupt = true;
    }
    return estimate(histogram);
}

bool merge_registers(Slice image, std::array<uint8_t, kRegisters>& maximum) {
    const uint8_t* source = bytes(image);
    if (source[4] == kDense) {
        source += kHeaderBytes;
        for (uint32_t i = 0; i < kRegisters; i++)
            maximum[i] = std::max(maximum[i], dense_get(source, i));
        return true;
    }

    size_t pos = kHeaderBytes;
    uint32_t index = 0;
    while (pos < image.n) {
        const uint8_t opcode = source[pos];
        uint32_t run = 0;
        if (sparse_zero(opcode)) {
            run = zero_len(opcode);
            pos++;
        } else if (sparse_xzero(opcode)) {
            if (pos + 1 >= image.n) return false;
            run = xzero_len(source + pos);
            pos += 2;
        } else {
            run = val_len(opcode);
            const uint8_t value = val_value(opcode);
            pos++;
            if (run > kRegisters - index) return false;
            for (uint32_t i = 0; i < run; i++, index++)
                maximum[index] = std::max(maximum[index], value);
            continue;
        }
        if (run > kRegisters - index) return false;
        index += run;
    }
    return index == kRegisters;
}

uint64_t count_registers(const std::array<uint8_t, kRegisters>& registers) {
    int histogram[64] = {};
    for (uint8_t value : registers) histogram[value]++;
    return estimate(histogram);
}

bool merge_result(Slice destination, bool destination_present,
                  const std::array<uint8_t, kRegisters>& maximum,
                  bool any_dense, std::string& result) {
    if (destination_present) result.assign(destination.p, destination.n);
    else result = create_sparse();

    if (any_dense) {
        if (!sparse_to_dense(result)) return false;
        uint8_t* registers = bytes(result) + kHeaderBytes;
        for (uint32_t i = 0; i < kRegisters; i++) dense_put(registers, i, maximum[i]);
    } else {
        for (uint32_t i = 0; i < kRegisters; i++) {
            if (!maximum[i]) continue;
            const int changed = static_cast<uint8_t>(result[4]) == kDense
                ? dense_set(bytes(result) + kHeaderBytes, i, maximum[i])
                : sparse_set(result, i, maximum[i]);
            if (changed < 0) return false;
        }
    }
    invalidate_cache(result);
    return true;
}

}  // namespace tomo::hll

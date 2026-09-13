// eviction.h — maxmemory policy names shared by boot/config parsing and the owner-local store.
#pragma once

#include <cstdint>
#include <string_view>

namespace tomo {

enum class MaxmemoryPolicy : uint8_t {
    NoEviction,
    AllKeysRandom,
    AllKeysLru,
    AllKeysLfu,
    VolatileRandom,
    VolatileLru,
    VolatileLfu,
    VolatileTtl,
};

inline constexpr const char* maxmemory_policy_name(MaxmemoryPolicy policy) {
    switch (policy) {
        case MaxmemoryPolicy::NoEviction:     return "noeviction";
        case MaxmemoryPolicy::AllKeysRandom:  return "allkeys-random";
        case MaxmemoryPolicy::AllKeysLru:     return "allkeys-lru";
        case MaxmemoryPolicy::AllKeysLfu:     return "allkeys-lfu";
        case MaxmemoryPolicy::VolatileRandom: return "volatile-random";
        case MaxmemoryPolicy::VolatileLru:    return "volatile-lru";
        case MaxmemoryPolicy::VolatileLfu:    return "volatile-lfu";
        case MaxmemoryPolicy::VolatileTtl:    return "volatile-ttl";
    }
    return "noeviction";
}

inline bool parse_maxmemory_policy(std::string_view input, MaxmemoryPolicy& out) {
    char normalized[32];
    if (input.empty() || input.size() >= sizeof(normalized)) return false;
    for (size_t i = 0; i < input.size(); i++) {
        char ch = input[i];
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        normalized[i] = ch;
    }
    const std::string_view value(normalized, input.size());
    if      (value == "noeviction")      out = MaxmemoryPolicy::NoEviction;
    else if (value == "allkeys-random")  out = MaxmemoryPolicy::AllKeysRandom;
    else if (value == "allkeys-lru")     out = MaxmemoryPolicy::AllKeysLru;
    else if (value == "allkeys-lfu")     out = MaxmemoryPolicy::AllKeysLfu;
    else if (value == "volatile-random") out = MaxmemoryPolicy::VolatileRandom;
    else if (value == "volatile-lru")    out = MaxmemoryPolicy::VolatileLru;
    else if (value == "volatile-lfu")    out = MaxmemoryPolicy::VolatileLfu;
    else if (value == "volatile-ttl")    out = MaxmemoryPolicy::VolatileTtl;
    else return false;
    return true;
}

inline constexpr bool maxmemory_policy_is_lru(MaxmemoryPolicy policy) {
    return policy == MaxmemoryPolicy::AllKeysLru || policy == MaxmemoryPolicy::VolatileLru;
}

inline constexpr bool maxmemory_policy_is_lfu(MaxmemoryPolicy policy) {
    return policy == MaxmemoryPolicy::AllKeysLfu || policy == MaxmemoryPolicy::VolatileLfu;
}

inline constexpr bool maxmemory_policy_is_volatile(MaxmemoryPolicy policy) {
    return policy == MaxmemoryPolicy::VolatileRandom || policy == MaxmemoryPolicy::VolatileLru ||
           policy == MaxmemoryPolicy::VolatileLfu || policy == MaxmemoryPolicy::VolatileTtl;
}

}  // namespace tomo

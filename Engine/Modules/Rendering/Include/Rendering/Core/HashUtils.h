// HashUtils.h — small header-only FNV-1a helpers shared by pipeline desc /
// format key hashing.

#pragma once

#include "Types/Fnv1a.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace GameEngine::Rendering::HashUtils
{

using ::GameEngine::Hashing::Fnv1a64;

// FNV-1a "hash combine": fold a trivially-copyable value into a running hash.
template <typename T>
inline uint64_t HashValue(uint64_t h, const T& v) noexcept
{
    return Fnv1a64(&v, sizeof(T), h);
}

// boost-style hash_combine. Stronger entropy mixing than naive XOR — use
// when combining two precomputed hashes (e.g. logical-pipeline hash with
// format-key hash) rather than when ingesting raw bytes.
inline uint64_t HashCombine(uint64_t seed, uint64_t v) noexcept
{
    seed ^= v + 0x9e3779b97f4a7c15ULL + (seed << 12) + (seed >> 4);
    return seed;
}

// Hash the size + bytes of a uint8 vector. Common ingredient for SPIR-V
// payloads and other shader byte hashes.
inline uint64_t HashBytes(uint64_t h, const std::vector<uint8_t>& v) noexcept
{
    const uint64_t sz = static_cast<uint64_t>(v.size());
    h = HashValue(h, sz);
    if (!v.empty())
        h = Fnv1a64(v.data(), v.size(), h);
    return h;
}

// Hash a uint32 vector with size-prefix.
inline uint64_t HashVecU32(uint64_t h, const std::vector<uint32_t>& v) noexcept
{
    const uint64_t sz = static_cast<uint64_t>(v.size());
    h = HashValue(h, sz);
    if (!v.empty())
        h = Fnv1a64(v.data(), v.size() * sizeof(uint32_t), h);
    return h;
}

} // namespace GameEngine::Rendering::HashUtils

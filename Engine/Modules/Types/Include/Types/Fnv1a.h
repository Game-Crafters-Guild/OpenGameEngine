#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace GameEngine::Hashing
{

inline constexpr std::uint64_t kFnv1a64OffsetBasis = 14695981039346656037ull;
inline constexpr std::uint64_t kFnv1a64Prime = 1099511628211ull;

inline std::uint64_t Fnv1a64(const void* data,
                             std::size_t size,
                             std::uint64_t seed = kFnv1a64OffsetBasis) noexcept
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint64_t hash = seed;
    for (std::size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= kFnv1a64Prime;
    }
    return hash;
}

constexpr std::uint64_t Fnv1a64(std::string_view text,
                                std::uint64_t seed = kFnv1a64OffsetBasis) noexcept
{
    std::uint64_t hash = seed;
    for (const char character : text)
    {
        hash ^= static_cast<unsigned char>(character);
        hash *= kFnv1a64Prime;
    }
    return hash;
}

template <typename T>
inline std::uint64_t Fnv1a64Value(std::uint64_t hash, const T& value) noexcept
{
    static_assert(std::is_trivially_copyable_v<T>);
    return Fnv1a64(&value, sizeof(T), hash);
}

} // namespace GameEngine::Hashing

#pragma once

#include "Types/Fnv1a.h"

#include <cstdint>
#include <string_view>

namespace GameEngine
{
// StringId: deterministic 64-bit id derived from a string.
// Use HashStringId("Name") or the "_sid" literal for compile-time ids.
using StringId = std::uint64_t;

// Simple constexpr FNV-1a 64-bit hash for string ids.
constexpr StringId HashStringId(std::string_view sv)
{
    return Hashing::Fnv1a64(sv);
}

// User-defined literal for compile-time string ids, e.g.:
//   constexpr StringId kMyId = "My.Id"_sid;
constexpr StringId operator"" _sid(const char* s, size_t n)
{
    return HashStringId(std::string_view{s, n});
}

} // namespace GameEngine

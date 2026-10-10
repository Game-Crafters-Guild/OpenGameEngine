#pragma once

#include "AssetCore/Types.h"
#include "Types/StringId.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace GameEngine
{
// AssetTypeId: a stable 64-bit id derived from a canonical string identifier.
//
// - In code: use HashAssetTypeId("Model") or the "_assetTypeId" literal.
// - On disk (AssetDatabase.assetdb): persist the string id (merge-friendly), then hash at runtime.
//
// NOTE: 0 is reserved to mean "invalid/unknown".
using AssetTypeId = StringId;

// Simple constexpr FNV-1a 64-bit hash for asset type ids.
// Matches the convention used by UI::EventId and Input::InputId.
constexpr AssetTypeId HashAssetTypeId(std::string_view sv)
{
    AssetTypeId hash = HashStringId(sv);
    // Reserve 0 for invalid/unknown even if a string ever hashes to 0 (extremely unlikely).
    return (hash == 0) ? 1ull : hash;
}

// User-defined literal for compile-time type ids, e.g.:
//   constexpr AssetTypeId kMyType = "MyPlugin.MyType"_assetTypeId;
constexpr AssetTypeId operator"" _assetTypeId(const char* s, size_t n)
{
    return HashAssetTypeId(std::string_view{s, n});
}

} // namespace GameEngine

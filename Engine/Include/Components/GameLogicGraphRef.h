#pragma once

#include "AssetCore/AssetTypes.h"
#include "Components/AssetRef.h"
#include "Types/StringUtils.h"
#include "Types/Types.h"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace GameEngine::Components
{

struct GameLogicGraphRef
{
    static constexpr uint32 kEntryFunctionCapacity = 64;

    AssetRef<AssetType::Graph> graphGuid;
    bool runOnStart = true;
    bool runOnUpdate = true;
    char entryFunction[kEntryFunctionCapacity] = "ExecuteGameLogicGraph";

    template <size_t Capacity>
    static void CopyFixedString(char (&target)[Capacity], std::string_view value)
    {
        std::memset(target, 0, sizeof(target));
        const size_t count = std::min(value.size(), sizeof(target) - 1);
        if (count > 0)
            std::memcpy(target, value.data(), count);
    }

    std::string_view EntryFunction() const { return FixedStringView(entryFunction); }

    void SetEntryFunction(std::string_view value)
    {
        CopyFixedString(entryFunction, value.empty() ? std::string_view("ExecuteGameLogicGraph") : value);
    }
};

} // namespace GameEngine::Components

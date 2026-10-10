#pragma once

#include <span>
#include <string_view>

namespace GameEngine {

/** One row of a kind-owned TypeId/category → icon-stem table. The tables live
    with each kind's registration site; this header is mechanism only. */
struct NodeIconStemEntry
{
    std::string_view TypeId;
    std::string_view Stem;
};

inline std::string_view FindIconStem(std::span<const NodeIconStemEntry> table,
                                     std::string_view typeId)
{
    for (const NodeIconStemEntry& entry : table)
    {
        if (entry.TypeId == typeId)
            return entry.Stem;
    }
    return {};
}

} // namespace GameEngine

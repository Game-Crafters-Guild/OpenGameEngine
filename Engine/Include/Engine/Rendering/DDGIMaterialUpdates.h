#pragma once

#include "SceneBvh/UberMaterial.h"

#include <array>
#include <cstring>
#include <span>
#include <vector>

namespace GameEngine::Engine::Renderer
{

inline std::array<float, 6> DDGIMaterialBindings(const SceneBvh::UberMaterial& material)
{
    return {material.AlbedoMapLayer, material.NormalMapLayer, material.RoughnessMapLayer,
            material.MetalnessMapLayer, material.EmissiveMapLayer, material.AlphaMapLayer};
}

struct DDGIMaterialUpdateRange
{
    size_t First = 0;
    size_t Count = 0;
};

// A stable material-table layout can be patched without touching instances or
// geometry. A deduplication split/merge or a map-binding change requires the
// structural path; callers must also verify per-instance slot assignments.
inline bool PlanDDGIMaterialUpdates(std::span<const SceneBvh::UberMaterial> previous,
                                    std::span<const SceneBvh::UberMaterial> current,
                                    std::vector<DDGIMaterialUpdateRange>& ranges)
{
    ranges.clear();
    if (previous.size() != current.size())
        return false;
    for (size_t i = 0; i < current.size(); ++i)
    {
        if (DDGIMaterialBindings(previous[i]) != DDGIMaterialBindings(current[i]))
        {
            ranges.clear();
            return false;
        }
        if (std::memcmp(&previous[i], &current[i], sizeof(SceneBvh::UberMaterial)) == 0)
            continue;
        if (!ranges.empty() && ranges.back().First + ranges.back().Count == i)
            ++ranges.back().Count;
        else
            ranges.push_back({i, 1});
    }
    return true;
}

}  // namespace GameEngine::Engine::Renderer

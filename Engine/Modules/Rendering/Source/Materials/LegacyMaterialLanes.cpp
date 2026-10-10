#include "Rendering/Materials/LegacyMaterialLanes.h"

#include <algorithm>

namespace GameEngine::Rendering
{

const LegacyMaterialLane* FindLegacyMaterialLane(std::string_view name)
{
    const auto it = std::find_if(kLegacyMaterialLanes.begin(), kLegacyMaterialLanes.end(),
                                 [name](const LegacyMaterialLane& lane) { return lane.Name == name; });
    return it == kLegacyMaterialLanes.end() ? nullptr : &*it;
}

std::string LegacyMaterialLaneGlsl(const LegacyMaterialLane& lane)
{
    static constexpr char kComponents[] = "xyzw";
    std::string expr = "uParams[" + std::to_string(lane.Lane) + "].";
    for (uint32_t i = 0; i < lane.Components; ++i)
        expr += kComponents[lane.Component + i];
    return expr;
}

} // namespace GameEngine::Rendering

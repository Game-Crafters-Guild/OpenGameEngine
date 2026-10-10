#include "DebugServer/TerrainSplitThresholdReport.h"

#include "CBTTerrain/CBTScreenTarget.h"

#include <cstdio>
#include <string>

namespace GameEngine::Editor
{

nlohmann::json DescribeTerrainSplitThreshold(float targetPixelError, float appliedSplitThresholdPx,
                                             std::uint32_t renderHeightPx)
{
    nlohmann::json out;
    out["targetPixelError"] = targetPixelError;
    out["targetReferenceHeightPx"] = CBTTerrain::kTargetReferenceHeightPx;
    char summary[192];
    if (renderHeightPx == 0u)
    {
        out["splitThresholdPx"] = nullptr;
        out["renderHeightPx"] = nullptr;
        std::snprintf(summary, sizeof(summary),
                      "Split threshold: no frame built yet (target %g px at %g rows).",
                      targetPixelError, CBTTerrain::kTargetReferenceHeightPx);
    }
    else
    {
        out["splitThresholdPx"] = appliedSplitThresholdPx;
        out["renderHeightPx"] = renderHeightPx;
        std::snprintf(summary, sizeof(summary),
                      "Split threshold: %.2f px at %u render rows (target %g px at %g rows).",
                      appliedSplitThresholdPx, renderHeightPx, targetPixelError,
                      CBTTerrain::kTargetReferenceHeightPx);
    }
    out["summary"] = std::string(summary);
    return out;
}

} // namespace GameEngine::Editor

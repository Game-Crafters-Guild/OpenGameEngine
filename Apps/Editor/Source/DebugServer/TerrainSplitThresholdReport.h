#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>

namespace GameEngine::Editor
{

// The split-threshold part of the `get_terrain_stats` debug response.
//
// Terrain.TargetPixelError is authored in pixels at 1080 rows; the terrain kernels test projected
// split edges against that value scaled to the view's render height (CBTTerrain::SplitThresholdPixels).
// The authored number alone does not say what a view refines to, so the response carries the
// threshold BuildFrameParams applied on its last frame and the render height it applied it for, read
// back from the feature rather than recomputed, plus one sentence for the diagnosis text.
// `renderHeightPx` 0 means no frame has been built yet: the applied fields are null.
nlohmann::json DescribeTerrainSplitThreshold(float targetPixelError, float appliedSplitThresholdPx,
                                             std::uint32_t renderHeightPx);

} // namespace GameEngine::Editor

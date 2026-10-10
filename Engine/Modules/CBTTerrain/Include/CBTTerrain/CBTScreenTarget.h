#pragma once

// CBTScreenTarget.h — the CPU-side mapping from the authored target pixel error to the split
// threshold Classify tests projected split edges against (CBTFrameParams::Screen.z, merge at half of
// it in Screen.w). A CPU-only policy, like CBTNearBias.h: only the resolved threshold crosses to the
// GPU.
//
// The target is resolution-relative: it is a pixel length at kTargetReferenceHeightPx rows, and the
// threshold scales with the render height. A projected edge scales with the render height at a fixed
// field of view, so at a given aspect ratio the triangle count a view draws does not depend on its
// resolution or render scale: a 3840 x 2160 view draws what a 1920 x 1080 view does, with
// triangles twice as long in pixels. That keeps the pool demand of a terrain independent of the display.

#include <cstdint>

namespace GameEngine::CBTTerrain
{

// The render height (rows) at which the authored target pixel error is the split threshold.
inline constexpr float kTargetReferenceHeightPx = 1080.0f;

// The split threshold in render pixels for `targetPixelError` (pixels at 1080 rows) in a view
// `renderHeightPx` rows tall. A view with no height yet keeps the target unscaled.
inline float SplitThresholdPixels(float targetPixelError, uint32_t renderHeightPx)
{
    if (renderHeightPx == 0u)
        return targetPixelError;
    return targetPixelError * (static_cast<float>(renderHeightPx) / kTargetReferenceHeightPx);
}

} // namespace GameEngine::CBTTerrain

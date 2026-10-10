#include "Assets/AlphaCutoffThreshold.h"

#include <algorithm>
#include <cmath>

namespace GameEngine {

std::optional<uint32> AlphaCutoffOpaqueThreshold(float cutoff, uint32 marginSteps)
{
    // Before any comparison or cast: NaN survives every ordered guard and casts
    // to an unspecified value.
    if (!std::isfinite(cutoff))
        return std::nullopt;

    // The shader discards when sampledAlpha < cutoff, with sampledAlpha =
    // byte/255, so the smallest non-discarding byte is ceil(cutoff * 255).
    const float clamped = std::clamp(cutoff, 0.0f, 1.0f);
    const uint32 exact = static_cast<uint32>(std::ceil(clamped * 255.0f));
    const uint32 threshold = exact + marginSteps;
    if (threshold > 255u)
        return std::nullopt;
    return threshold;
}

} // namespace GameEngine

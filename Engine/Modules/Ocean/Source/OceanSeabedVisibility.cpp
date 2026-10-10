#include "Ocean/OceanSeabedVisibility.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Ocean
{

float32 OceanSeabedVisibleDepthM(const OceanParamsGPU& params)
{
    constexpr float32 kMinDepthFogDensity = 1e-3f; // guards the division; the max clamp bounds the result
    const float32 minDensity =
        std::max(std::min({params.DepthFogDensity[0], params.DepthFogDensity[1],
                           params.DepthFogDensity[2]}),
                 kMinDepthFogDensity);
    const float32 byDensity = -std::log(kOceanSeabedVisibleTransmittance) / minDensity;
    const float32 byRange = std::max(params.DepthFogEndDistance, 0.0f);
    const float32 byWindow = params.ShallowClarityFloor < 1.0f ? params.ShallowClarityDistance : 0.0f;
    return std::clamp(std::max({byDensity, byRange, byWindow}), 0.0f, kOceanSeabedVisibleDepthMaxM);
}

} // namespace GameEngine::Ocean

#pragma once

#include "Types/Types.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace GameEngine::Ocean
{

// Lightweight CPU-side RG texture used by ocean query mirrors for author maps.
// Values are stored in the same normalized/linear space the GPU sampler returns.
struct OceanCpuTextureRG
{
    uint32 Width = 0;
    uint32 Height = 0;
    std::vector<float32> RG;

    bool IsValid() const
    {
        return Width > 0u && Height > 0u &&
               RG.size() >= static_cast<size_t>(Width) * Height * 2u;
    }

    bool SampleLinear(float32 u, float32 v, float32& outR, float32& outG) const
    {
        if (!IsValid())
            return false;

        u = std::clamp(u, 0.0f, 1.0f);
        v = std::clamp(v, 0.0f, 1.0f);

        const float32 x = u * static_cast<float32>(Width - 1u);
        const float32 y = v * static_cast<float32>(Height - 1u);
        const uint32 x0 = static_cast<uint32>(std::floor(x));
        const uint32 y0 = static_cast<uint32>(std::floor(y));
        const uint32 x1 = std::min(x0 + 1u, Width - 1u);
        const uint32 y1 = std::min(y0 + 1u, Height - 1u);
        const float32 tx = x - static_cast<float32>(x0);
        const float32 ty = y - static_cast<float32>(y0);

        auto texel = [&](uint32 px, uint32 py, uint32 lane) -> float32 {
            const size_t i = (static_cast<size_t>(py) * Width + px) * 2u + lane;
            return RG[i];
        };

        const float32 r0 = texel(x0, y0, 0u) * (1.0f - tx) + texel(x1, y0, 0u) * tx;
        const float32 r1 = texel(x0, y1, 0u) * (1.0f - tx) + texel(x1, y1, 0u) * tx;
        const float32 g0 = texel(x0, y0, 1u) * (1.0f - tx) + texel(x1, y0, 1u) * tx;
        const float32 g1 = texel(x0, y1, 1u) * (1.0f - tx) + texel(x1, y1, 1u) * tx;

        outR = r0 * (1.0f - ty) + r1 * ty;
        outG = g0 * (1.0f - ty) + g1 * ty;
        return true;
    }
};

} // namespace GameEngine::Ocean

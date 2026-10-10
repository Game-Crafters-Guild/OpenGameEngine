#pragma once

#include "Types/Types.h"

#include <algorithm>

namespace GameEngine::TerrainECS
{

// Write one material's weight into an RGBA8 splat texel.
//
// The two arms are the shipped paint semantics, so surface rules and paint cannot drift:
// a rule row IS a paint write whose strength was computed from its conditions.
//
// It sits in a header rather than in the bake TU for the same reason the rule evaluator
// does — it is the part the GPU splat kernel has to reproduce expression for expression
// (terrain_surface_rules.glsl GE_CompositeSplatTexel), and a mirror is far easier to hold
// against a function than against a lambda nested in a texel walk.
//
// THE TEXEL IS THE STATE. Every call reads the bytes the previous call wrote, so a stack
// of rows composites through the uint8 quantization rather than in float — which is what
// makes the GPU mirror byte-comparable rather than approximately equal.
inline void CompositeSplatTexel(uint8* pixel, uint32 layerIdx, float32 weight, bool replace)
{
    if (replace)
    {
        // The ramp weight is the replacement FRACTION: every channel
        // lerps from what the mix already held toward its pure-layer
        // value, which preserves the weight sum and makes the edge
        // exactly as soft as the falloff. The target channel must
        // lerp like the rest — writing the ramp into it absolutely
        // drops the weight that layer already had, stepping at the
        // ramp's zero crossing wherever the surrounding splat
        // already contains the painted layer.
        for (uint32 i = 0; i < 4; ++i)
        {
            const float32 target = (i == layerIdx) ? 255.0f : 0.0f;
            pixel[i] = static_cast<uint8>(std::clamp(
                pixel[i] * (1.0f - weight) + target * weight, 0.0f, 255.0f));
        }
        return;
    }

    float32 weights[4];
    for (uint32 i = 0; i < 4; ++i)
        weights[i] = pixel[i] / 255.0f;

    weights[layerIdx] += weight;

    float32 total = 0.0f;
    for (float32 wt : weights)
        total += wt;
    if (total > 0.0f)
    {
        for (uint32 i = 0; i < 4; ++i)
            pixel[i] = static_cast<uint8>(std::clamp(weights[i] / total * 255.0f, 0.0f, 255.0f));
    }
}

} // namespace GameEngine::TerrainECS

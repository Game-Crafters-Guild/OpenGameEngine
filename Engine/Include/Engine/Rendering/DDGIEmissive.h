#pragma once

#include "Components/Rendering/LightPhotometry.h"  // kReferenceWhiteNits

namespace GameEngine::Engine::Renderer
{

// Scene-linear emissive for the DDGI software lane, composed the way the raster
// surface (Surfaces/standard_pbr.glsl) and the shared trace shading
// (ddgi_hit_shade.glsl) compose it: authored emission is map * colour * nits,
// folded to scene-linear by the reference-white anchor of 203 nits.
//
// This returns the map-less term (colour * nits / 203) in Color; a bound
// emissive map multiplies it (GE_DDGIApplyEmissiveMap in
// Includes/ddgi_hit_shade.glsl). The divide lives here, and in the hardware
// kernel, rather than in that shared shader step so the software lane's value is
// pinnable in a unit test; dropping it makes DDGI emit 203x hot.
struct DDGIEmissive
{
    float Color[3] = {0.0f, 0.0f, 0.0f};
};

inline DDGIEmissive ComputeDDGIEmissive(float tintR, float tintG, float tintB, float nits)
{
    const float luminance = nits / Components::kReferenceWhiteNits;
    return DDGIEmissive{{tintR * luminance, tintG * luminance, tintB * luminance}};
}

}  // namespace GameEngine::Engine::Renderer

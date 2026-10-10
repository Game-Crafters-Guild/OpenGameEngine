#pragma once

#include "Rendering/Core/Handle.h"

#include <cstdint>

namespace GameEngine::Rendering
{
class IDevice;
} // namespace GameEngine::Rendering

namespace GameEngine::Ocean
{

// Procedural tileable underwater caustics texture. Generated once at startup on
// the CPU (no external image), uploaded with a full mip chain, and sampled by the
// surface shader to modulate the refracted scene colour beneath the water.
//
// The pattern is a Voronoi-edge web: caustics are the bright, thin, curved lines
// where refracted sunlight focuses, which is exactly the ridge between Worley
// cells. Two channels carry distinct data so one small texture serves both the
// caustic intensity sample (two tiled scrolling lookups in the surface shader)
// and the UV-distortion sample:
//   R, G = a tangent-space distortion normal (xy, [0,1]-encoded) from a smooth
//          low-frequency field — bends the caustic lookup UVs so the web ripples.
//   B    = the caustic intensity (Voronoi-edge web), the value the surface reads
//          twice at two scrolling scales and multiplies into the scene colour.
//   A    = 1 (unused; keeps the format RGBA8).
//
// The texture is tileable (the noise wraps at the texture period) so it can be
// sampled with Repeat wrap at arbitrary world tiling without visible seams. A
// failed init leaves the handle invalid; the surface then skips caustics (the
// CausticsAvailable gate stays off), so the effect degrades cleanly.
class OceanCaustics
{
public:
    bool Initialize(::GameEngine::Rendering::IDevice* device);
    bool IsReady() const { return m_Ready; }

    ::GameEngine::Rendering::TextureHandle GetTexture() const { return m_Texture; }
    ::GameEngine::Rendering::SamplerHandle GetSampler() const { return m_Sampler; }

    void Destroy(::GameEngine::Rendering::IDevice* device);

private:
    // Power-of-two so the mip chain and the wrap-tileable noise stay exact.
    static constexpr uint32_t kResolution = 256;

    ::GameEngine::Rendering::TextureHandle m_Texture;
    ::GameEngine::Rendering::SamplerHandle m_Sampler; // linear / repeat / mipmapped
    bool m_Ready = false;
};

} // namespace GameEngine::Ocean

#pragma once

#include <cstdint>

namespace GameEngine::Engine::Renderer
{

// Interior octahedral resolution and tile size of the SHARP (power-64)
// reflection lobe. MUST match GE_DDGI_GLOSSY_OCT_RES / GE_DDGI_GLOSSY_TILE in
// Includes/ddgi_common.glsl — the kernels index their state buffers with the
// GLSL values while this side sizes the allocation, so a divergence is an
// out-of-bounds write, not a visual artifact.
inline constexpr int32_t kDDGIGlossyOctRes = 16;
inline constexpr int32_t kDDGIGlossyBorder = 1;
inline constexpr int32_t kDDGIGlossyTile = kDDGIGlossyOctRes + 2 * kDDGIGlossyBorder;  // 18

// Where probe `probeIdx`'s 18x18 tile lives in the glossy atlas.
//
// NEAR-SQUARE rather than the probe-XYZ z-major packing the irradiance/depth/
// rough atlases share, because that layout does not survive an 18x18 tile: its
// height is probeY * probeZ * tile, which at 32^3 probes is 32*32*18 = 18432
// texels, past the 8192 maximum 2D texture dimension every target guarantees.
// Near-square packs the same 32768 probes into 182*18 = 3276 square. The
// layout is therefore independent of the grid's shape, which is also why it
// needs its own addressing helper (GE_DDGIGlossyTexelUV) rather than reusing
// GE_DDGIProbeTileOrigin.
struct DDGIGlossyAtlasLayout
{
    int32_t TilesX = 0;
    int32_t TilesY = 0;
    int32_t WidthTexels = 0;
    int32_t HeightTexels = 0;
};

// Smallest near-square tile grid holding `probeTotal` tiles: TilesX =
// ceil(sqrt(probeTotal)), TilesY = ceil(probeTotal / TilesX).
//
// Integer arithmetic throughout — a sqrt() here would be rounded differently
// on different targets at the perfect squares, and the CPU allocation and the
// GPU's tile addressing must agree exactly or probes read each other's tiles.
// A non-positive probe count yields a 1x1 layout rather than a zero-sized
// texture, so a degenerate volume still allocates something bindable.
constexpr DDGIGlossyAtlasLayout ComputeDDGIGlossyAtlasLayout(int32_t probeTotal)
{
    DDGIGlossyAtlasLayout layout;
    const int32_t total = probeTotal > 0 ? probeTotal : 1;

    int32_t tilesX = 1;
    while (tilesX * tilesX < total)
        ++tilesX;

    layout.TilesX = tilesX;
    layout.TilesY = (total + tilesX - 1) / tilesX;
    layout.WidthTexels = layout.TilesX * kDDGIGlossyTile;
    layout.HeightTexels = layout.TilesY * kDDGIGlossyTile;
    return layout;
}

}  // namespace GameEngine::Engine::Renderer

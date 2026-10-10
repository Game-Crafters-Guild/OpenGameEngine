#pragma once

#include "Types/Types.h"

namespace GameEngine {

/**
 * @brief Single-channel alpha view over decoded texel data.
 *
 * Alpha points at the first texel's alpha byte; PixelStride is the byte
 * distance between consecutive texels (4 to view the A channel of an
 * interleaved RGBA8 image in place). Rows are Width texels, tightly packed.
 */
struct AlphaPlaneView {
    const uint8* Alpha = nullptr;
    uint32 Width = 0;
    uint32 Height = 0;
    uint32 PixelStride = 1;
};

/// Minimum alpha over every texel of the plane; 0 for a null/empty plane.
/// One O(W*H) scan answers every whole-plane opacity question: the plane is
/// opaque at threshold T exactly when PlaneMinAlpha(plane) >= T.
uint8 PlaneMinAlpha(const AlphaPlaneView& plane);

/**
 * @brief Conservative UV-footprint opacity test for one triangle.
 *
 * True when every texel the triangle can influence through filtered sampling
 * — its conservatively rasterized texel coverage dilated by dilationTexels —
 * satisfies alpha >= opaqueThreshold. UVs are in normalized texture space
 * (after any texture transform). Repeat addressing is assumed on both axes,
 * matching the bindless material sampler, so tiling triangles that span
 * multiple wraps test every texel they can reach. Degenerate triangles are
 * covered as segments/points.
 *
 * planeMinAlpha must be PlaneMinAlpha(plane), computed once per plane by the
 * caller: footprints too large (or too far) to rasterize, and non-finite
 * UVs, resolve against the whole plane through it in O(1) — which can only
 * keep more texels in the test.
 */
bool TriangleFootprintIsOpaque(const AlphaPlaneView& plane,
                               const float uvA[2],
                               const float uvB[2],
                               const float uvC[2],
                               uint8 opaqueThreshold,
                               float dilationTexels,
                               uint8 planeMinAlpha);

} // namespace GameEngine

#pragma once

#include "AssetCore/NineSlice.h" // NineSlice

#include <functional>

namespace GameEngine
{
namespace UI
{

// One sub-quad of a 9-slice background: a destination rect (physical px) paired
// with its source UV sub-rect (normalized).
struct NineSliceRegion
{
    float X = 0.0f, Y = 0.0f, W = 0.0f, H = 0.0f;
    float U0 = 0.0f, V0 = 0.0f, U1 = 1.0f, V1 = 1.0f;
};

// Sink for emitted regions. Called 0..many times (tiling multiplies quads), so
// callers stream regions rather than receive a fixed-size array.
using NineSliceEmit = std::function<void(const NineSliceRegion&)>;

// Emit the destination + UV rects for a 9-slice background.
//
//   (imgW,imgH)        source texture size, texels
//   (dx,dy,dw,dh)      destination rect, physical px (the element's content box)
//   contentScale       physical px per source texel for the fixed corners + tile size
//
// Corners are drawn 1:1 (never distorted); edges and the center stretch (one quad)
// or tile/round (many quads) per FillX (horizontal axis) / FillY (vertical axis).
// Source cuts use the up-to-four-per-axis model: the [X[0],X[1]] / [X[2],X[3]]
// bands are gaps and are not sampled. Zero-area regions (degenerate borders, gaps,
// clamped corners) are omitted, so corners stay seamless and no pixel is lost to a
// cut. Tiled bands get a half-texel UV inset to avoid bleeding the neighbouring
// (corner) texel at each tile seam.
void BuildNineSliceRegions(const NineSlice& slice, float imgW, float imgH,
                           float dx, float dy, float dw, float dh,
                           float contentScale, const NineSliceEmit& emit);

} // namespace UI
} // namespace GameEngine

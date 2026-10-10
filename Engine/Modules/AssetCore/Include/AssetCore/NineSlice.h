#pragma once

#include "AssetCore/Types.h" // uint8 / uint16

namespace GameEngine {

// How an edge / the center region fills its destination span when 9-slicing:
//   Stretch  scale the source band to fill
//   Tile     repeat the source band at its natural size (last repeat clipped)
//   Round    like Tile, but the tile size is snapped so a whole number fits
//   Scale    (center only) scale the center source to fit preserving aspect, centered
enum class NineSliceFill : uint8 { Stretch, Tile, Round, Scale };

// 9-slice (nine-patch) border layout authored on a texture and applied when the
// texture is used as a UI background. The image is drawn as a 3x3 grid: corners
// keep their source size, edges stretch/tile along one axis, the center fills the
// middle. Cuts are stored as absolute SOURCE-TEXEL positions, up to four per axis:
//   corner = [0, X[0]] · center = [X[1], X[2]] · corner = [X[3], width]
// The [X[0],X[1]] and [X[2],X[3]] bands are GAPS (dropped, never sampled). The
// canonical "shared lines" 9-slice keeps X[0]==X[1] and X[2]==X[3] (no gaps);
// independent edges split each pair to expose a gap. Same scheme for Y vs height.
//
// This is a small dependency-free POD on purpose: it rides on the texture cache
// and the UI primitive path, neither of which should pull in the full texture
// asset / GPU format headers. It lives in AssetCore (not the Engine asset layer)
// so UI public headers can include it without crossing into the Engine monolith.
struct NineSlice
{
    bool          Enabled = false;
    bool          FillCenter = true;            // draw the center region (CSS `fill`)
    uint16        X[4] = {0, 0, 0, 0};          // 0 <= X[0] <= X[1] <= X[2] <= X[3] <= width
    uint16        Y[4] = {0, 0, 0, 0};          // 0 <= Y[0] <= Y[1] <= Y[2] <= Y[3] <= height
    NineSliceFill FillX = NineSliceFill::Stretch;     // top/bottom EDGES, horizontally
    NineSliceFill FillY = NineSliceFill::Stretch;     // left/right EDGES, vertically
    NineSliceFill CenterFill = NineSliceFill::Stretch; // the center region (2D)

    // Only textures the user explicitly enabled take the 9-quad path; everything
    // else stays on the single-quad fast path. Degenerate (zero-border) sliced
    // textures still render correctly — the emitter skips zero-area regions.
    bool IsSliced() const { return Enabled; }
};

} // namespace GameEngine

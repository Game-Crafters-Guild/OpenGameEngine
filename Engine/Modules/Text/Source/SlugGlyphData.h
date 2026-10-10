#pragma once

// Internal data structures for Slug font rendering.
// Slug evaluates quadratic Bezier curves per-pixel in the fragment shader.
// Each font produces two GPU textures:
//   - Curve texture (RGBA16F, 4096 wide): packed control points
//   - Band texture (RG16UI, 4096 wide): spatial acceleration structure
//
// This header is internal to the Text module — not a public API.
// SlugGlyphInfo is defined in FontAtlas.h to keep the unordered_map in the header.

#include <cstdint>
#include <vector>

namespace GameEngine::Rendering::Text
{

// Raw curve segment (quadratic Bezier) in em-space coordinates.
struct SlugCurve
{
    float X1, Y1; // start point
    float X2, Y2; // control point
    float X3, Y3; // end point
};

// Texture width from FontAtlas::kSlugTextureWidth (matches shader's kLogBandTextureWidth = 12).

// Intermediate per-glyph result before packing into shared textures.
struct SlugGlyphBuildResult
{
    FontAtlas::SlugGlyphInfo Info;
    std::vector<SlugCurve> Curves;

    // Band data: for each horizontal band, the indices of curves that intersect it.
    // For each vertical band, same. Sorted by descending max coordinate.
    std::vector<uint16_t> HBandCurveCounts; // [hBandCount] curve count per H band
    std::vector<uint16_t> VBandCurveCounts; // [vBandCount] curve count per V band
    std::vector<uint16_t> HBandCurveIndices; // flattened curve indices for all H bands
    std::vector<uint16_t> VBandCurveIndices; // flattened curve indices for all V bands
};

} // namespace GameEngine::Rendering::Text

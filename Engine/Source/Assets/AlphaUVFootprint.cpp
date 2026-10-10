#include "Assets/AlphaUVFootprint.h"

#include <algorithm>
#include <cmath>

namespace GameEngine {

namespace {

// Covers the farthest point of a texel cell from its center (half diagonal of
// a unit cell), rounded up so cell-corner contact still counts as coverage.
constexpr float kCellHalfDiagonal = 0.7072f;

// A footprint whose candidate texel count exceeds this multiple of the plane
// falls back to the whole-plane test instead of rasterizing (conservative:
// the whole-plane test can only demand more texels be opaque).
constexpr uint64 kMaxRasterTexelFactor = 4;

float SegmentDistanceSquared(float px, float py,
                             float ax, float ay,
                             float bx, float by)
{
    const float abx = bx - ax;
    const float aby = by - ay;
    const float apx = px - ax;
    const float apy = py - ay;
    const float lenSq = abx * abx + aby * aby;
    float t = 0.0f;
    if (lenSq > 0.0f)
        t = std::clamp((apx * abx + apy * aby) / lenSq, 0.0f, 1.0f);
    const float dx = apx - t * abx;
    const float dy = apy - t * aby;
    return dx * dx + dy * dy;
}

// Squared distance from a point to a triangle (0 inside). Degenerate
// triangles report 0 everywhere the edge-sign test cannot separate, which
// over-covers — safe, since coverage only adds texels to the opacity test.
float TriangleDistanceSquared(float px, float py,
                              const float a[2], const float b[2], const float c[2])
{
    const float d1 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
    const float d2 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0]);
    const float d3 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0]);
    const bool hasNeg = (d1 < 0.0f) || (d2 < 0.0f) || (d3 < 0.0f);
    const bool hasPos = (d1 > 0.0f) || (d2 > 0.0f) || (d3 > 0.0f);
    if (!(hasNeg && hasPos))
        return 0.0f;

    return std::min({SegmentDistanceSquared(px, py, a[0], a[1], b[0], b[1]),
                     SegmentDistanceSquared(px, py, b[0], b[1], c[0], c[1]),
                     SegmentDistanceSquared(px, py, c[0], c[1], a[0], a[1])});
}

inline uint32 WrapIndex(int64 i, uint32 extent)
{
    const int64 e = static_cast<int64>(extent);
    const int64 m = i % e;
    return static_cast<uint32>(m < 0 ? m + e : m);
}

inline uint8 TexelAlpha(const AlphaPlaneView& plane, uint32 x, uint32 y)
{
    const uint64 index = (static_cast<uint64>(y) * plane.Width + x) * plane.PixelStride;
    return plane.Alpha[index];
}

} // namespace

uint8 PlaneMinAlpha(const AlphaPlaneView& plane)
{
    if (!plane.Alpha || plane.Width == 0 || plane.Height == 0)
        return 0;
    uint8 minAlpha = 255;
    for (uint32 y = 0; y < plane.Height; ++y)
    {
        for (uint32 x = 0; x < plane.Width; ++x)
        {
            const uint8 alpha = TexelAlpha(plane, x, y);
            if (alpha < minAlpha)
            {
                minAlpha = alpha;
                if (minAlpha == 0)
                    return 0;
            }
        }
    }
    return minAlpha;
}

bool TriangleFootprintIsOpaque(const AlphaPlaneView& plane,
                               const float uvA[2],
                               const float uvB[2],
                               const float uvC[2],
                               uint8 opaqueThreshold,
                               float dilationTexels,
                               uint8 planeMinAlpha)
{
    if (!plane.Alpha || plane.Width == 0 || plane.Height == 0)
        return false;

    const float w = static_cast<float>(plane.Width);
    const float h = static_cast<float>(plane.Height);
    const float a[2] = {uvA[0] * w, uvA[1] * h};
    const float b[2] = {uvB[0] * w, uvB[1] * h};
    const float c[2] = {uvC[0] * w, uvC[1] * h};

    // Non-finite UVs cannot be located; require the whole plane (conservative).
    for (const float v : {a[0], a[1], b[0], b[1], c[0], c[1]})
        if (!std::isfinite(v))
            return planeMinAlpha >= opaqueThreshold;

    const float radius = std::max(dilationTexels, 0.0f) + kCellHalfDiagonal;
    const float radiusSq = radius * radius;

    const float minX = std::min({a[0], b[0], c[0]}) - radius;
    const float maxX = std::max({a[0], b[0], c[0]}) + radius;
    const float minY = std::min({a[1], b[1], c[1]}) - radius;
    const float maxY = std::max({a[1], b[1], c[1]}) + radius;

    // Bbox coordinates beyond this bound cannot be walked safely: the
    // float->int64 casts below are UB once the value leaves int64 range, and
    // far before that the spanX*spanY product can wrap uint64, turning the
    // raster cap into an unbounded loop. Such footprints (absurd UV tiling)
    // take the whole-plane fallback, which only errs toward keeping Mask.
    constexpr float kMaxPixelCoordMagnitude = 1.0e9f;
    if (std::max({std::fabs(minX), std::fabs(maxX), std::fabs(minY), std::fabs(maxY)}) >
        kMaxPixelCoordMagnitude)
        return planeMinAlpha >= opaqueThreshold;

    // Texel centers sit at integer + 0.5; candidates are the texels whose
    // center can be within `radius` of the triangle.
    const int64 x0 = static_cast<int64>(std::floor(minX - 0.5f));
    const int64 x1 = static_cast<int64>(std::ceil(maxX + 0.5f));
    const int64 y0 = static_cast<int64>(std::floor(minY - 0.5f));
    const int64 y1 = static_cast<int64>(std::ceil(maxY + 0.5f));
    const uint64 spanX = static_cast<uint64>(x1 - x0 + 1);
    const uint64 spanY = static_cast<uint64>(y1 - y0 + 1);

    const uint64 planeTexels = static_cast<uint64>(plane.Width) * plane.Height;
    if ((spanX >= plane.Width && spanY >= plane.Height) ||
        spanX * spanY > kMaxRasterTexelFactor * planeTexels)
        return planeMinAlpha >= opaqueThreshold;

    for (int64 iy = y0; iy <= y1; ++iy)
    {
        const uint32 wy = WrapIndex(iy, plane.Height);
        const float cy = static_cast<float>(iy) + 0.5f;
        for (int64 ix = x0; ix <= x1; ++ix)
        {
            const float cx = static_cast<float>(ix) + 0.5f;
            if (TriangleDistanceSquared(cx, cy, a, b, c) > radiusSq)
                continue;
            if (TexelAlpha(plane, WrapIndex(ix, plane.Width), wy) < opaqueThreshold)
                return false;
        }
    }
    return true;
}

} // namespace GameEngine

#include "UI/NineSliceLayout.h"

#include <algorithm>
#include <cmath>

namespace GameEngine
{
namespace UI
{
namespace
{
// Safety bound on tiles per axis so a tiny tile across a huge band can't explode
// the draw count. Generous for real UI borders; a band needing more is clamped.
constexpr int kMaxTilesPerAxis = 128;
// The center cell subdivides on BOTH axes (worst case nx*ny), so it gets a tighter
// per-axis cap: 32*32 = 1024 quads instead of an unbounded 128*128 = 16384.
constexpr int kMaxCenterTilesPerAxis = 32;
// A natural tile <= half a dest pixel would emit many sub-pixel slivers; collapse
// to one stretched span instead.
constexpr float kMinTileDestPx = 0.5f;
// Stop tiling within a sub-pixel of the band end so the last step isn't a sliver.
constexpr float kTileEndEpsilonPx = 0.01f;

// One dest+UV span along a single axis.
struct Span
{
    float d0, d1, uv0, uv1;
};

// Subdivide a band along one axis into dest+UV spans, appending to `out` (up to
// `cap`). Stretch -> one span. Tile -> repeats of `tileDest` from d0, last clipped.
// Round -> tile size snapped so a whole number fits. Tiled spans inset the source
// UV by `halfTexelUV` at both ends to avoid bleeding the neighbour texel at seams.
int FillAxis(float d0, float d1, float u0, float u1, NineSliceFill fill,
             float tileDest, float halfTexelUV, Span* out, int cap)
{
    const float destLen = d1 - d0;
    if (destLen <= 0.0f || cap <= 0)
        return 0;

    // Scale is a center-only mode handled by the caller; for an axis span it (and
    // Stretch) collapse to a single full span.
    if (fill == NineSliceFill::Stretch || fill == NineSliceFill::Scale || tileDest <= kMinTileDestPx)
    {
        out[0] = {d0, d1, u0, u1};
        return 1;
    }

    float tile = tileDest;
    if (fill == NineSliceFill::Round)
    {
        const int nWhole = std::max(1, static_cast<int>(std::lround(destLen / tile)));
        tile = destLen / static_cast<float>(nWhole);
    }

    // Inset the band UV by half a texel to avoid bleeding the neighbour texel at
    // seams, but never past the band's own half-width (a 1-texel band must keep a
    // non-zero source span).
    const float inset = std::min(halfTexelUV, 0.5f * (u1 - u0));
    const float us0 = u0 + inset;
    const float us1 = u1 - inset;
    const float uSpan = us1 - us0;

    int n = 0;
    float cur = d0;
    while (cur < d1 - kTileEndEpsilonPx && n < cap)
    {
        const float seg = std::min(tile, d1 - cur);
        const float frac = (tile > 0.0f) ? (seg / tile) : 1.0f; // partial last tile
        out[n] = {cur, cur + seg, us0, us0 + uSpan * frac};
        ++n;
        cur += seg;
    }
    return n;
}
} // namespace

void BuildNineSliceRegions(const NineSlice& slice, float imgW, float imgH,
                           float dx, float dy, float dw, float dh,
                           float contentScale, const NineSliceEmit& emit)
{
    if (!emit)
        return;

    const float sw = (imgW > 0.0f) ? imgW : 1.0f;
    const float sh = (imgH > 0.0f) ? imgH : 1.0f;
    const float scale = (contentScale > 0.0f) ? contentScale : 1.0f;

    // Clamp cuts into [0,size] and enforce non-decreasing order (defensive).
    float xc[4];
    float yc[4];
    for (int i = 0; i < 4; ++i)
    {
        xc[i] = std::clamp(static_cast<float>(slice.X[i]), 0.0f, sw);
        yc[i] = std::clamp(static_cast<float>(slice.Y[i]), 0.0f, sh);
    }
    for (int i = 1; i < 4; ++i)
    {
        xc[i] = std::max(xc[i], xc[i - 1]);
        yc[i] = std::max(yc[i], yc[i - 1]);
    }

    // Destination corner thicknesses (source-texel border widths scaled to px),
    // with the CSS overflow rule scaling an opposing pair down to fit the box.
    float dL = xc[0] * scale;
    float dR = (sw - xc[3]) * scale;
    float dT = yc[0] * scale;
    float dB = (sh - yc[3]) * scale;
    if (dL + dR > dw && (dL + dR) > 0.0f) { const float k = dw / (dL + dR); dL *= k; dR *= k; }
    if (dT + dB > dh && (dT + dB) > 0.0f) { const float k = dh / (dT + dB); dT *= k; dB *= k; }

    const float X[4] = {dx, dx + dL, dx + dw - dR, dx + dw};
    const float Y[4] = {dy, dy + dT, dy + dh - dB, dy + dh};

    // Per-band source UVs; gaps [xc0,xc1]/[xc2,xc3] excluded (center samples xc1..xc2).
    const float Us[3][2] = {{0.0f, xc[0] / sw}, {xc[1] / sw, xc[2] / sw}, {xc[3] / sw, 1.0f}};
    const float Vs[3][2] = {{0.0f, yc[0] / sh}, {yc[1] / sh, yc[2] / sh}, {yc[3] / sh, 1.0f}};
    const float Xd[3][2] = {{X[0], X[1]}, {X[1], X[2]}, {X[2], X[3]}};
    const float Yd[3][2] = {{Y[0], Y[1]}, {Y[1], Y[2]}, {Y[2], Y[3]}};

    // Natural tile size (dest px) for the center band, used when an axis tiles.
    const float centerTileW = (xc[2] - xc[1]) * scale;
    const float centerTileH = (yc[2] - yc[1]) * scale;
    const float halfTexelU = 0.5f / sw;
    const float halfTexelV = 0.5f / sh;

    Span xs[kMaxTilesPerAxis];
    Span ys[kMaxTilesPerAxis];

    for (int col = 0; col < 3; ++col)
    {
        for (int row = 0; row < 3; ++row)
        {
            const bool isCenter = (col == 1 && row == 1);
            if (isCenter && !slice.FillCenter)
                continue;
            const float cw = Xd[col][1] - Xd[col][0];
            const float ch = Yd[row][1] - Yd[row][0];
            if (cw <= 0.0f || ch <= 0.0f)
                continue; // degenerate border / dropped gap / clamped corner

            // Center with Scale: a single quad fitted into the cell preserving the
            // center band's aspect ratio, centered (the rest of the cell is empty).
            if (isCenter && slice.CenterFill == NineSliceFill::Scale)
            {
                const float srcW = xc[2] - xc[1];
                const float srcH = yc[2] - yc[1];
                if (srcW > 0.0f && srcH > 0.0f)
                {
                    const float srcAspect = srcW / srcH;
                    float drawW = cw;
                    float drawH = ch;
                    if (cw / ch > srcAspect) { drawH = ch; drawW = ch * srcAspect; }
                    else                     { drawW = cw; drawH = cw / srcAspect; }
                    NineSliceRegion r;
                    r.X = Xd[1][0] + (cw - drawW) * 0.5f;
                    r.Y = Yd[1][0] + (ch - drawH) * 0.5f;
                    r.W = drawW;
                    r.H = drawH;
                    r.U0 = Us[1][0]; r.V0 = Vs[1][0];
                    r.U1 = Us[1][1]; r.V1 = Vs[1][1];
                    emit(r);
                }
                continue;
            }

            // Edges fill per FillX/FillY; the center fills per CenterFill. Only the
            // center column/row subdivide; corner-side columns/rows are one 1:1 span.
            const NineSliceFill xMode = isCenter ? slice.CenterFill : slice.FillX;
            const NineSliceFill yMode = isCenter ? slice.CenterFill : slice.FillY;
            // The center tiles on both axes, so cap it tighter; 1D edges keep the full cap.
            const int cap = isCenter ? kMaxCenterTilesPerAxis : kMaxTilesPerAxis;

            int nx, ny;
            if (col == 1)
                nx = FillAxis(Xd[1][0], Xd[1][1], Us[1][0], Us[1][1], xMode, centerTileW, halfTexelU, xs, cap);
            else
            {
                xs[0] = {Xd[col][0], Xd[col][1], Us[col][0], Us[col][1]};
                nx = 1;
            }
            if (row == 1)
                ny = FillAxis(Yd[1][0], Yd[1][1], Vs[1][0], Vs[1][1], yMode, centerTileH, halfTexelV, ys, cap);
            else
            {
                ys[0] = {Yd[row][0], Yd[row][1], Vs[row][0], Vs[row][1]};
                ny = 1;
            }

            for (int i = 0; i < nx; ++i)
            {
                for (int j = 0; j < ny; ++j)
                {
                    NineSliceRegion r;
                    r.X = xs[i].d0;
                    r.Y = ys[j].d0;
                    r.W = xs[i].d1 - xs[i].d0;
                    r.H = ys[j].d1 - ys[j].d0;
                    r.U0 = xs[i].uv0;
                    r.V0 = ys[j].uv0;
                    r.U1 = xs[i].uv1;
                    r.V1 = ys[j].uv1;
                    emit(r);
                }
            }
        }
    }
}

} // namespace UI
} // namespace GameEngine

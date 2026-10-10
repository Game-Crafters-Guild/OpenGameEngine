#include "FontAtlas_Internal.h"
#include "SlugGlyphData.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(GE_HAVE_FREETYPE)
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#endif

namespace GameEngine::Rendering::Text
{

#if defined(GE_HAVE_FREETYPE)

// --- FreeType outline decomposition into quadratic Bezier segments ---

struct OutlineDecompContext
{
    std::vector<SlugCurve>* curves;
    float scaleToEm;    // 1.0 / units_per_em
    float lastX, lastY; // current pen position in em-space
};

static int SlugMoveTo(const FT_Vector* to, void* user)
{
    auto* ctx = static_cast<OutlineDecompContext*>(user);
    ctx->lastX = static_cast<float>(to->x) * ctx->scaleToEm;
    ctx->lastY = static_cast<float>(to->y) * ctx->scaleToEm;
    return 0;
}

static int SlugLineTo(const FT_Vector* to, void* user)
{
    auto* ctx = static_cast<OutlineDecompContext*>(user);
    float ex = static_cast<float>(to->x) * ctx->scaleToEm;
    float ey = static_cast<float>(to->y) * ctx->scaleToEm;

    // Represent a line as a degenerate quadratic: control point at midpoint.
    SlugCurve c;
    c.X1 = ctx->lastX;
    c.Y1 = ctx->lastY;
    c.X2 = (ctx->lastX + ex) * 0.5f;
    c.Y2 = (ctx->lastY + ey) * 0.5f;
    c.X3 = ex;
    c.Y3 = ey;
    ctx->curves->push_back(c);

    ctx->lastX = ex;
    ctx->lastY = ey;
    return 0;
}

static int SlugConicTo(const FT_Vector* ctrl, const FT_Vector* to, void* user)
{
    auto* ctx = static_cast<OutlineDecompContext*>(user);
    float cx = static_cast<float>(ctrl->x) * ctx->scaleToEm;
    float cy = static_cast<float>(ctrl->y) * ctx->scaleToEm;
    float ex = static_cast<float>(to->x) * ctx->scaleToEm;
    float ey = static_cast<float>(to->y) * ctx->scaleToEm;

    SlugCurve c;
    c.X1 = ctx->lastX;
    c.Y1 = ctx->lastY;
    c.X2 = cx;
    c.Y2 = cy;
    c.X3 = ex;
    c.Y3 = ey;
    ctx->curves->push_back(c);

    ctx->lastX = ex;
    ctx->lastY = ey;
    return 0;
}

static int SlugCubicTo(const FT_Vector* ctrl1, const FT_Vector* ctrl2, const FT_Vector* to, void* user)
{
    // Degree reduction: approximate a cubic Bezier with two quadratic segments
    // via midpoint subdivision. This is standard practice for CFF fonts.
    auto* ctx = static_cast<OutlineDecompContext*>(user);
    float c1x = static_cast<float>(ctrl1->x) * ctx->scaleToEm;
    float c1y = static_cast<float>(ctrl1->y) * ctx->scaleToEm;
    float c2x = static_cast<float>(ctrl2->x) * ctx->scaleToEm;
    float c2y = static_cast<float>(ctrl2->y) * ctx->scaleToEm;
    float ex = static_cast<float>(to->x) * ctx->scaleToEm;
    float ey = static_cast<float>(to->y) * ctx->scaleToEm;

    float p0x = ctx->lastX, p0y = ctx->lastY;

    // Split the cubic at t=0.5 with De Casteljau, giving the two half-cubics
    // (p0, m01, m012, mid) and (mid, m123, m23, e).
    float m01x = (p0x + c1x) * 0.5f, m01y = (p0y + c1y) * 0.5f;
    float m12x = (c1x + c2x) * 0.5f, m12y = (c1y + c2y) * 0.5f;
    float m23x = (c2x + ex) * 0.5f, m23y = (c2y + ey) * 0.5f;
    float m012x = (m01x + m12x) * 0.5f, m012y = (m01y + m12y) * 0.5f;
    float m123x = (m12x + m23x) * 0.5f, m123y = (m12y + m23y) * 0.5f;
    float midx = (m012x + m123x) * 0.5f, midy = (m012y + m123y) * 0.5f;

    // Reduce each half-cubic to a quadratic that keeps its endpoints. The
    // control point is the degree-reduction average of the two endpoint-tangent
    // extrapolations, q = (3*(P1 + P2) - (P0 + P3)) / 4.
    //
    // Taking a De Casteljau point (m012 / m123) as the control instead costs a
    // whole degree of accuracy: it lands on the half-cubic's P2 (resp. P1), so
    // the quadratic leaves its start point along the wrong tangent and the
    // curve is pulled toward the chord. On a circular arc that is 0.031 em of
    // deviation against 0.0017 em here — an inward pinch of 0.32 px on a 30 px
    // 'O' bowl. Only CFF/PostScript outlines reach this path; TrueType 'glyf'
    // fonts are quadratic already and go through SlugConicTo.
    auto reduceHalf = [&](float startX, float startY, float ctrlAX, float ctrlAY,
                          float ctrlBX, float ctrlBY, float endX, float endY)
    {
        SlugCurve q;
        q.X1 = startX;
        q.Y1 = startY;
        q.X2 = (3.0f * (ctrlAX + ctrlBX) - (startX + endX)) * 0.25f;
        q.Y2 = (3.0f * (ctrlAY + ctrlBY) - (startY + endY)) * 0.25f;
        q.X3 = endX;
        q.Y3 = endY;
        ctx->curves->push_back(q);
    };

    reduceHalf(p0x, p0y, m01x, m01y, m012x, m012y, midx, midy);
    reduceHalf(midx, midy, m123x, m123y, m23x, m23y, ex, ey);

    ctx->lastX = ex;
    ctx->lastY = ey;
    return 0;
}

// --- Band subdivision ---

// Compute how many bands to use for a given em-space extent.
// Heuristic: ~8 bands per em unit, clamped to [1, 255] (band count fits in uint8).
static uint16_t ComputeBandCount(float extent)
{
    if (extent <= 0.0f)
        return 1;
    int n = static_cast<int>(std::ceil(extent * 8.0f));
    return static_cast<uint16_t>(std::max(1, std::min(255, n)));
}

// Check if a quadratic curve's Y range intersects the band [bandMin, bandMax].
static bool CurveIntersectsBandY(const SlugCurve& c, float bandMin, float bandMax)
{
    float yMin = std::min({c.Y1, c.Y2, c.Y3});
    float yMax = std::max({c.Y1, c.Y2, c.Y3});
    // Slight epsilon overlap matching the Slug reference (1/1024 em)
    static constexpr float kBandEpsilon = 1.0f / 1024.0f;
    return yMax >= (bandMin - kBandEpsilon) && yMin <= (bandMax + kBandEpsilon);
}

// Check if a quadratic curve's X range intersects the band [bandMin, bandMax].
static bool CurveIntersectsBandX(const SlugCurve& c, float bandMin, float bandMax)
{
    float xMin = std::min({c.X1, c.X2, c.X3});
    float xMax = std::max({c.X1, c.X2, c.X3});
    static constexpr float kBandEpsilon = 1.0f / 1024.0f;
    return xMax >= (bandMin - kBandEpsilon) && xMin <= (bandMax + kBandEpsilon);
}

static bool BuildSlugGlyphData(FT_Face face, uint32_t glyphId, SlugGlyphBuildResult& out)
{
    if (FT_Load_Glyph(face, glyphId, FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING | FT_LOAD_NO_SCALE) != 0)
        return false;

    FT_GlyphSlot slot = face->glyph;
    FT_Outline& outline = slot->outline;

    if (outline.n_points == 0 || outline.n_contours == 0)
        return false;

    float scaleToEm = 1.0f / static_cast<float>(face->units_per_EM);

    // Decompose outline into quadratic Bezier segments
    out.Curves.clear();
    OutlineDecompContext ctx;
    ctx.curves = &out.Curves;
    ctx.scaleToEm = scaleToEm;
    ctx.lastX = ctx.lastY = 0;

    FT_Outline_Funcs funcs{};
    funcs.move_to = SlugMoveTo;
    funcs.line_to = SlugLineTo;
    funcs.conic_to = SlugConicTo;
    funcs.cubic_to = SlugCubicTo;

    if (FT_Outline_Decompose(&outline, &funcs, &ctx) != 0)
        return false;

    if (out.Curves.empty())
        return false;

    // Compute em-space bounds
    float emXMin = 1e9f, emYMin = 1e9f, emXMax = -1e9f, emYMax = -1e9f;
    for (const auto& c : out.Curves)
    {
        emXMin = std::min({emXMin, c.X1, c.X2, c.X3});
        emYMin = std::min({emYMin, c.Y1, c.Y2, c.Y3});
        emXMax = std::max({emXMax, c.X1, c.X2, c.X3});
        emYMax = std::max({emYMax, c.Y1, c.Y2, c.Y3});
    }

    out.Info.EmXMin = emXMin;
    out.Info.EmYMin = emYMin;
    out.Info.EmXMax = emXMax;
    out.Info.EmYMax = emYMax;
    // Subdivide into bands
    float emWidth = emXMax - emXMin;
    float emHeight = emYMax - emYMin;
    uint16_t hBandCount = ComputeBandCount(emHeight);
    uint16_t vBandCount = ComputeBandCount(emWidth);
    out.Info.HBandCount = hBandCount;
    out.Info.VBandCount = vBandCount;

    // Build horizontal bands (indexed by Y, used for horizontal ray casting)
    out.HBandCurveCounts.resize(hBandCount);
    out.HBandCurveIndices.clear();
    float hBandSize = emHeight / hBandCount;

    // Reuse a single indices vector across all band iterations to avoid per-band allocation.
    std::vector<uint16_t> indices;

    for (uint16_t b = 0; b < hBandCount; ++b)
    {
        float bandMin = emYMin + b * hBandSize;
        float bandMax = emYMin + (b + 1) * hBandSize;

        indices.clear();
        for (uint16_t ci = 0; ci < static_cast<uint16_t>(out.Curves.size()); ++ci)
        {
            if (CurveIntersectsBandY(out.Curves[ci], bandMin, bandMax))
                indices.push_back(ci);
        }

        // Sort by descending max X (for early exit in shader)
        std::sort(indices.begin(), indices.end(), [&](uint16_t lhs, uint16_t rhs)
        {
            float maxL = std::max({out.Curves[lhs].X1, out.Curves[lhs].X2, out.Curves[lhs].X3});
            float maxR = std::max({out.Curves[rhs].X1, out.Curves[rhs].X2, out.Curves[rhs].X3});
            return maxL > maxR;
        });

        out.HBandCurveCounts[b] = static_cast<uint16_t>(indices.size());
        out.HBandCurveIndices.insert(out.HBandCurveIndices.end(), indices.begin(), indices.end());
    }

    // Build vertical bands (indexed by X, used for vertical ray casting)
    out.VBandCurveCounts.resize(vBandCount);
    out.VBandCurveIndices.clear();
    float vBandSize = emWidth / vBandCount;

    for (uint16_t b = 0; b < vBandCount; ++b)
    {
        float bandMin = emXMin + b * vBandSize;
        float bandMax = emXMin + (b + 1) * vBandSize;

        indices.clear();
        for (uint16_t ci = 0; ci < static_cast<uint16_t>(out.Curves.size()); ++ci)
        {
            if (CurveIntersectsBandX(out.Curves[ci], bandMin, bandMax))
                indices.push_back(ci);
        }

        // Sort by descending max Y (for early exit in shader)
        std::sort(indices.begin(), indices.end(), [&](uint16_t lhs, uint16_t rhs)
        {
            float maxL = std::max({out.Curves[lhs].Y1, out.Curves[lhs].Y2, out.Curves[lhs].Y3});
            float maxR = std::max({out.Curves[rhs].Y1, out.Curves[rhs].Y2, out.Curves[rhs].Y3});
            return maxL > maxR;
        });

        out.VBandCurveCounts[b] = static_cast<uint16_t>(indices.size());
        out.VBandCurveIndices.insert(out.VBandCurveIndices.end(), indices.begin(), indices.end());
    }

    return true;
}

// --- Texture packing ---

static constexpr int kSlugTextureWidth = FontAtlas::kSlugTextureWidth;
static constexpr int kSlugPageRows     = FontAtlas::kSlugPageRows;

// Pre-allocate a page's backing buffers to their final (GPU texture) size.
static void InitSlugPage(FontAtlas::SlugPage& page)
{
    page.CurveData.assign(static_cast<size_t>(kSlugTextureWidth) * kSlugPageRows * 4, 0.0f);
    page.BandData.assign(static_cast<size_t>(kSlugTextureWidth) * kSlugPageRows * 2, 0);
    page.CurvePenX = 0;
    page.CurvePenY = 0;
    page.BandPenX = 0;
    page.BandPenY = 0;
    page.CurveRowsUsed = 0;
    page.BandRowsUsed = 0;
    page.Generation = 0;
}

// Number of curve texels a glyph consumes (2 per curve). Not row-aligned —
// curves are packed densely and may straddle row boundaries.
static int CurveTexelsFor(const SlugGlyphBuildResult& build)
{
    return static_cast<int>(build.Curves.size()) * 2;
}

// How many band texels a glyph needs (counts + offset entries + curve indices).
static int BandTexelsFor(const SlugGlyphBuildResult& build)
{
    int total = build.Info.HBandCount + build.Info.VBandCount;
    total += static_cast<int>(build.HBandCurveIndices.size() + build.VBandCurveIndices.size());
    return total;
}

// Does this glyph fit in the given page's remaining capacity?
// Mirrors the exact addressing PackGlyphIntoPage performs so we never write
// past the pre-allocated backing buffers.
static bool FitsInPage(const FontAtlas::SlugPage& page, const SlugGlyphBuildResult& build)
{
    // --- Curves: packed densely starting at (CurvePenX, CurvePenY) ---
    int curveTexels = CurveTexelsFor(build);
    if (curveTexels > 0)
    {
        // Packing advances by 2 per curve, wrapping at row end. Last curve is
        // written at offset (CurvePenX + curveTexels - 2..-1).
        int lastTexelAbs = page.CurvePenY * kSlugTextureWidth + page.CurvePenX + curveTexels - 1;
        int lastRow = lastTexelAbs / kSlugTextureWidth;
        if (lastRow >= kSlugPageRows) return false;
    }

    // --- Bands: may wrap to a new row, then span multiple rows ---
    int bandTexels = BandTexelsFor(build);
    if (bandTexels > 0)
    {
        int startX = page.BandPenX;
        int startY = page.BandPenY;
        if (startX + bandTexels > kSlugTextureWidth)
        {
            startX = 0;
            startY++;
        }
        int maxRow = startY + (startX + bandTexels - 1) / kSlugTextureWidth;
        if (maxRow >= kSlugPageRows) return false;
    }

    return true;
}

// Pack a built glyph into a specific page. Caller must have verified FitsInPage.
static void PackGlyphIntoPage(
    FontAtlas::SlugPage& page,
    const SlugGlyphBuildResult& build,
    FontAtlas::SlugGlyphInfo& outInfo)
{
    // Copy build.Info fields selectively — DO NOT clobber outInfo.PageIndex,
    // which was already set by the caller to the correct destination page.
    outInfo.EmXMin = build.Info.EmXMin;
    outInfo.EmYMin = build.Info.EmYMin;
    outInfo.EmXMax = build.Info.EmXMax;
    outInfo.EmYMax = build.Info.EmYMax;
    outInfo.HBandCount = build.Info.HBandCount;
    outInfo.VBandCount = build.Info.VBandCount;
    // (GlyphLocX/Y are written below once we know the band start.)

    // --- Curves ---
    // Pack densely starting at (CurvePenX, CurvePenY). The per-curve texel
    // positions wrap across row boundaries naturally — each curve occupies
    // two consecutive texels within a single row (we wrap BEFORE writing
    // if needed so a curve never straddles a row boundary).
    static thread_local std::vector<int> curveTexelX;
    static thread_local std::vector<int> curveTexelY;
    curveTexelX.resize(build.Curves.size());
    curveTexelY.resize(build.Curves.size());

    int penX = page.CurvePenX;
    int penY = page.CurvePenY;
    for (size_t i = 0; i < build.Curves.size(); ++i)
    {
        if (penX + 2 > kSlugTextureWidth)
        {
            penX = 0;
            penY++;
        }
        curveTexelX[i] = penX;
        curveTexelY[i] = penY;

        const SlugCurve& c = build.Curves[i];
        size_t idx0 = (static_cast<size_t>(penY) * kSlugTextureWidth + penX) * 4;
        page.CurveData[idx0 + 0] = c.X1;
        page.CurveData[idx0 + 1] = c.Y1;
        page.CurveData[idx0 + 2] = c.X2;
        page.CurveData[idx0 + 3] = c.Y2;

        size_t idx1 = (static_cast<size_t>(penY) * kSlugTextureWidth + penX + 1) * 4;
        page.CurveData[idx1 + 0] = c.X3;
        page.CurveData[idx1 + 1] = c.Y3;

        penX += 2;
    }
    page.CurvePenX = penX;
    page.CurvePenY = penY;
    page.CurveRowsUsed = penY + 1;

    // --- Bands ---
    uint16_t hBandCount = build.Info.HBandCount;
    uint16_t vBandCount = build.Info.VBandCount;
    int totalBandEntries = hBandCount + vBandCount;
    int bandTexelsNeeded = BandTexelsFor(build);

    // Wrap to a new band row if the current row doesn't have enough contiguous space.
    if (page.BandPenX + bandTexelsNeeded > kSlugTextureWidth)
    {
        page.BandPenX = 0;
        page.BandPenY++;
    }

    outInfo.GlyphLocX = page.BandPenX;
    outInfo.GlyphLocY = page.BandPenY;

    int curveIndexStart = totalBandEntries;

    auto writeBandEntry = [&](int listOffset, uint16_t curveCount, uint16_t offset)
    {
        int tx = page.BandPenX + listOffset;
        int ty = page.BandPenY;
        ty += tx / kSlugTextureWidth;
        tx = tx % kSlugTextureWidth;
        size_t idx = (static_cast<size_t>(ty) * kSlugTextureWidth + tx) * 2;
        page.BandData[idx + 0] = curveCount;
        page.BandData[idx + 1] = offset;
    };

    int hOffset = 0;
    for (uint16_t b = 0; b < hBandCount; ++b)
    {
        writeBandEntry(b, build.HBandCurveCounts[b],
                       static_cast<uint16_t>(curveIndexStart + hOffset));
        hOffset += build.HBandCurveCounts[b];
    }

    int vOffset = 0;
    for (uint16_t b = 0; b < vBandCount; ++b)
    {
        writeBandEntry(hBandCount + b, build.VBandCurveCounts[b],
                       static_cast<uint16_t>(curveIndexStart
                                             + static_cast<int>(build.HBandCurveIndices.size()) + vOffset));
        vOffset += build.VBandCurveCounts[b];
    }

    auto writeCurveIndex = [&](int listOffset, uint16_t curveIdx)
    {
        int tx = page.BandPenX + curveIndexStart + listOffset;
        int ty = page.BandPenY;
        ty += tx / kSlugTextureWidth;
        tx = tx % kSlugTextureWidth;
        size_t idx = (static_cast<size_t>(ty) * kSlugTextureWidth + tx) * 2;
        page.BandData[idx + 0] = static_cast<uint16_t>(curveTexelX[curveIdx]);
        page.BandData[idx + 1] = static_cast<uint16_t>(curveTexelY[curveIdx]);
    };

    int listOffset = 0;
    for (uint16_t ci : build.HBandCurveIndices)
        writeCurveIndex(listOffset++, ci);
    for (uint16_t ci : build.VBandCurveIndices)
        writeCurveIndex(listOffset++, ci);

    // Advance pen past the written data.
    page.BandPenX += bandTexelsNeeded;
    if (page.BandPenX >= kSlugTextureWidth)
    {
        page.BandPenY += page.BandPenX / kSlugTextureWidth;
        page.BandPenX = page.BandPenX % kSlugTextureWidth;
    }
    page.BandRowsUsed = page.BandPenY + 1;

    ++page.Generation;
}

// --- FontAtlas integration ---

bool FontAtlas::EnsureSlugGlyphData(const std::vector<uint32_t>& glyphIds)
{
    if (glyphIds.empty())
        return true;

    FT_Face face = m_FtFace.Face();
    if (!face)
        return false;

    // Reuse build result across glyphs to retain vector capacity.
    SlugGlyphBuildResult build;

    // Ensure at least one page exists before we try to pack into it.
    if (m_SlugPages.empty())
    {
        m_SlugPages.emplace_back();
        InitSlugPage(m_SlugPages.back());
    }

    for (uint32_t gid : glyphIds)
    {
        if (gid == 0)
            continue;

        // try_emplace avoids the double lookup of count() + emplace().
        auto [it, inserted] = m_SlugGlyphs.try_emplace(gid, SlugGlyphInfo{});
        if (!inserted)
            continue;

        if (!BuildSlugGlyphData(face, gid, build))
            continue; // whitespace/empty — already inserted as default SlugGlyphInfo

        // Find a page that fits. Pages are append-only; new glyphs always go
        // into the newest page, never into older ones (which may still have
        // gaps but whose layout is frozen because other glyphs reference them).
        SlugPage* page = &m_SlugPages.back();
        if (!FitsInPage(*page, build))
        {
            m_SlugPages.emplace_back();
            InitSlugPage(m_SlugPages.back());
            page = &m_SlugPages.back();

            // Safety: if the glyph is so large that even an empty page can't
            // hold it, drop it rather than endlessly creating pages. In practice
            // this should never trigger for a real font (a single page fits
            // ~130k curves / 260k band texels).
            if (!FitsInPage(*page, build))
                continue;
        }

        it->second.PageIndex = static_cast<uint16_t>(m_SlugPages.size() - 1);
        PackGlyphIntoPage(*page, build, it->second);
    }

    return true;
}

#endif // GE_HAVE_FREETYPE

} // namespace GameEngine::Rendering::Text

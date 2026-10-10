#include "FontAtlas_Internal.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace GameEngine::Rendering::Text
{

#if defined(GE_HAVE_FREETYPE)
namespace
{
// A face + hb_font pair, already sized for shaping. Wraps the atlas's primary
// face so the measurement cores below share one implementation across the
// public measure entry points.
struct MeasureFaceRef
{
    FT_Face Face = nullptr;
#if defined(GE_HAVE_HARFBUZZ)
    hb_font_t* Hb = nullptr;
#endif
};

// cache (optional): shaped-width LRU keyed on (utf8, atlasPx, ligature arm) — a
// hit skips shaping entirely; only the advance width and the cluster count are
// cached (heights come from the design-unit metrics below, which need no
// shaping, and the spacing itself is applied per request on top).

// The three design-unit parts of a CSS `normal` line box.
struct NormalLineUnits
{
    float Ascent = 0.0f;
    float Descent = 0.0f;
    float LineGap = 0.0f;
};

// Which of the face's three competing metric sets a `normal` line box is built
// from. Blink asks the platform font API, which on Windows is DirectWrite:
// DWRITE_FONT_METRICS reports the OS/2 usWin pair plus only whatever hhea
// leading those do not already account for, unless the face opts into its
// typographic metrics via fsSelection bit 7 (USE_TYPO_METRICS).
//
// The distinction is invisible on faces whose three sets agree (Roboto,
// Roboto Mono) and decides the whole line box on faces where they do not:
// Cascadia Code sets the bit and would otherwise be 14% too tall, and Consolas
// and Calibri keep their totals but move the baseline by 2px at 16px because
// hhea splits the same height differently.
static NormalLineUnits NormalLineUnitsForFace(FT_Face face)
{
    NormalLineUnits u{(float)face->ascender, (float)(-face->descender),
                      (float)(face->height - (face->ascender - face->descender))};

    const auto* hhea = (const TT_HoriHeader*)FT_Get_Sfnt_Table(face, FT_SFNT_HHEA);
    const auto* os2 = (const TT_OS2*)FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
    // 0xFFFF is FreeType's "this face has no OS/2 table"; a non-SFNT face has
    // neither table. Both leave hhea as the only metric set there is.
    if (!hhea || !os2 || os2->version == 0xFFFFu)
        return u;

    constexpr FT_UShort kFsSelectionUseTypoMetrics = 1u << 7;
    if (os2->fsSelection & kFsSelectionUseTypoMetrics)
    {
        u.Ascent = (float)os2->sTypoAscender;
        u.Descent = (float)(-os2->sTypoDescender);
        u.LineGap = (float)os2->sTypoLineGap;
        return u;
    }

    const float winAscent = (float)os2->usWinAscent;
    const float winDescent = (float)os2->usWinDescent;
    const float hheaExtent = (float)(hhea->Ascender - hhea->Descender);
    u.Ascent = winAscent;
    u.Descent = winDescent;
    // The usWin pair usually already covers part of hhea's gap; only the
    // remainder survives as external leading, and never a negative amount.
    u.LineGap = std::max(0.0f, (float)hhea->Line_Gap - ((winAscent + winDescent) - hheaExtent));
    return u;
}

// A CSS `normal` line box, in the pixel space the face is rasterized in.
//
// Design units scaled to the render size, not FreeType's size->metrics: those
// are rounded to whole pixels at the active ppem, so reading them bakes the
// atlas ppem's rounding into every render size and makes the reported height
// move when the atlas size changes.
//
// Blink rounds ascent, descent and line gap to whole pixels independently and
// sums them (SimpleFontData::PlatformInit), then places the baseline at
// floor(ascent + halfLeading) (FontHeight::AddLeading). Both roundings happen
// at the size the font was instantiated at, so renderPixelSize being a DEVICE
// pixel size is what keeps this matching Chrome on a scaled display: Chrome
// folds the device scale factor into the used font size, and its line box in
// CSS px is correspondingly fractional there.
static LineMetrics ExactLineMetricsWithFace(FT_Face face, float renderPixelSize)
{
    const float upem = (face->units_per_EM > 0) ? (float)face->units_per_EM : 1000.0f;
    const float unitScale = renderPixelSize / upem;
    const NormalLineUnits u = NormalLineUnitsForFace(face);

    const float ascPx = std::round(u.Ascent * unitScale);
    const float descPx = std::round(u.Descent * unitScale);
    const float gapPx = std::round(u.LineGap * unitScale);

    LineMetrics lm{};
    lm.height = ascPx + descPx + gapPx;
    lm.ascender = std::floor(ascPx + gapPx * 0.5f);
    lm.descender = lm.height - lm.ascender;
    return lm;
}

// No default for the spacing on these internal seams: the bug the argument
// exists to fix was a call site that silently measured without it.
TextMetrics MeasureUtf8WithFace(const MeasureFaceRef& f, std::string_view utf8,
                                float geometryScale, float renderPixelSize,
                                ShapedWidthCache* cache, unsigned atlasPx,
                                float letterSpacingPx)
{
    TextMetrics m{};
    float wAtlas = 0.0f;
    uint32_t clusterCount = 0;
    uint64_t cacheKey = 0;
    bool haveWidth = false;
    const bool tracked = (letterSpacingPx != 0.0f);
    if (cache)
    {
        cacheKey = ShapedWidthKey(utf8, atlasPx, tracked);
        haveWidth = ShapedWidthLookup(*cache, cacheKey, wAtlas, clusterCount);
    }
#if defined(GE_HAVE_HARFBUZZ)
    if (!haveWidth && f.Hb)
    {
        hb_buffer_t* s_HbBuf = Detail::GetThreadLocalHBBuffer();
        hb_buffer_add_utf8(s_HbBuf, utf8.data(), (int)utf8.size(), 0, (int)utf8.size());
        Detail::HBDirScript r = Detail::DetectDirScriptUtf8(utf8);
        if (r.script != HB_SCRIPT_INVALID)
        {
            hb_buffer_set_direction(s_HbBuf, r.dir);
            hb_buffer_set_script(s_HbBuf, r.script);
            if (r.lang)
                hb_buffer_set_language(s_HbBuf, hb_language_from_string(r.lang, -1));
        }
        else
        {
            hb_buffer_guess_segment_properties(s_HbBuf);
        }
        hb_feature_t features[2];
        const unsigned featureCount = Detail::LetterSpacingFeatures(letterSpacingPx, features);
        hb_shape(f.Hb, s_HbBuf, featureCount ? features : nullptr, featureCount);
        unsigned count = 0;
        const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(s_HbBuf, &count);
        const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(s_HbBuf, &count);
        for (unsigned i = 0; i < count; ++i)
        {
            wAtlas += pos[i].x_advance / 64.0f;
            if (i == 0 || infos[i].cluster != infos[i - 1].cluster)
                ++clusterCount;
        }
        if (cache)
            ShapedWidthInsert(*cache, cacheKey, wAtlas, clusterCount);
        haveWidth = true;
    }
#endif
    if (!haveWidth)
    {
        // Degraded byte-wise fallback (no HarfBuzz font): never published to
        // the shared cache — a persisted approximation would be served to
        // hb-capable consumers too, converting a self-healing per-call
        // fallback into sticky cross-producer poison.
        size_t codepoints = 0;
        for (unsigned char c : utf8)
        {
            if (FT_Load_Char(f.Face, c, FT_LOAD_DEFAULT) == 0)
                wAtlas += f.Face->glyph->advance.x / 64.0f;
            if ((c & 0xC0u) != 0x80u) // UTF-8 codepoint start = pseudo-cluster
                ++codepoints;
        }
        clusterCount = (uint32_t)codepoints;
    }
    // Tracking is applied in render px on top of the shaped width: exactly
    // clusterCount x spacing, immune to the atlas-ppem scaling.
    m.width = wAtlas * geometryScale + (float)clusterCount * letterSpacingPx;

    // Exact design-unit line metrics at the render size — must agree with
    // GetFontLineMetrics so single-line and multi-line heights share one
    // definition. The width above comes from the atlas ppem scaled by
    // geometryScale; the line box comes from the caller's fractional device
    // size, which is where CSS rounds it.
    const LineMetrics lm = ExactLineMetricsWithFace(f.Face, renderPixelSize);
    m.ascender = lm.ascender;
    m.descender = lm.descender;
    m.height = lm.height;
    m.baseline = m.ascender;
    return m;
}

bool BuildCaretMapWithFace(const MeasureFaceRef& f, std::string_view utf8,
                           float geometryScale, std::vector<float>& outXByByte,
                           float letterSpacingPx)
{
    outXByByte.clear();
    outXByByte.resize(utf8.size() + 1, 0.0f);
    if (!f.Face)
        return false;

    // Tracking accumulates along the pen in atlas space so the final scale to
    // render pixels converts every position in one pass. The round-trip
    // (ls / gs) * gs is spacing-exact to a ulp; ls == 0 leaves the math
    // byte-identical to the untracked path.
    const float lsAtlas = (geometryScale != 0.0f) ? letterSpacingPx / geometryScale : 0.0f;

#if defined(GE_HAVE_HARFBUZZ)
    if (f.Hb)
    {
        hb_buffer_t* s_HbBuf = Detail::GetThreadLocalHBBuffer();
        hb_buffer_add_utf8(s_HbBuf, utf8.data(), (int)utf8.size(), 0, (int)utf8.size());
        Detail::HBDirScript r = Detail::DetectDirScriptUtf8(utf8);
        if (r.script != HB_SCRIPT_INVALID)
        {
            hb_buffer_set_direction(s_HbBuf, r.dir);
            hb_buffer_set_script(s_HbBuf, r.script);
            if (r.lang)
                hb_buffer_set_language(s_HbBuf, hb_language_from_string(r.lang, -1));
        }
        else
        {
            hb_buffer_guess_segment_properties(s_HbBuf);
        }
        hb_feature_t features[2];
        const unsigned featureCount = Detail::LetterSpacingFeatures(letterSpacingPx, features);
        hb_shape(f.Hb, s_HbBuf, featureCount ? features : nullptr, featureCount);
        unsigned count = 0;
        const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(s_HbBuf, &count);
        const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(s_HbBuf, &count);
        // Map from byte index -> x position in pixels (ink advance space)
        float xAtlas = 0.0f;
        // Initialize all undefined to -1 so we can fill gaps
        std::fill(outXByByte.begin(), outXByByte.end(), -1.0f);
        for (unsigned i = 0; i < count; ++i)
        {
            unsigned cluster = infos[i].cluster; // byte index in original UTF-8 string
            if (cluster <= utf8.size())
            {
                if (outXByByte[cluster] < 0.0f)
                    outXByByte[cluster] = xAtlas;
            }
            xAtlas += pos[i].x_advance / 64.0f;
            // Spacing goes after each cluster's last glyph, including the run's
            // final one — the caret at end-of-text carries the trailing gap and
            // so equals the measured width.
            if (i + 1 == count || infos[i + 1].cluster != cluster)
                xAtlas += lsAtlas;
        }
        // Ensure the last insertion point is set to total width
        outXByByte[utf8.size()] = xAtlas;
        // Backfill any gaps (bytes within a cluster share the same x)
        float last = 0.0f;
        for (size_t i = 0; i < outXByByte.size(); ++i)
        {
            if (outXByByte[i] >= 0.0f)
                last = outXByByte[i];
            else
                outXByByte[i] = last;
        }
        // Convert atlas-space X positions to render pixels
        if (geometryScale != 1.0f)
        {
            for (float& v : outXByByte)
                v *= geometryScale;
        }
        return true;
    }
#endif
    // Fallback without HarfBuzz: approximate using glyph advances for ASCII
    float xAtlas = 0.0f;
    outXByByte[0] = 0.0f;
    size_t bi = 0;
    while (bi < utf8.size())
    {
        unsigned char c = (unsigned char)utf8[bi];
        unsigned codepoint = c;
        size_t step = 1;
        if (c >= 0x80)
        { // naive UTF-8 step
            if ((c >> 5) == 0x6 && bi + 1 < utf8.size())
            {
                codepoint = ((c & 0x1F) << 6) | ((unsigned char)utf8[bi + 1] & 0x3F);
                step = 2;
            }
            else if ((c >> 4) == 0xE && bi + 2 < utf8.size())
            {
                codepoint = ((c & 0x0F) << 12) | (((unsigned char)utf8[bi + 1] & 0x3F) << 6) | ((unsigned char)utf8[bi + 2] & 0x3F);
                step = 3;
            }
            else if ((c >> 3) == 0x1E && bi + 3 < utf8.size())
            {
                codepoint = ((c & 0x07) << 18) | (((unsigned char)utf8[bi + 1] & 0x3F) << 12) | (((unsigned char)utf8[bi + 2] & 0x3F) << 6) | ((unsigned char)utf8[bi + 3] & 0x3F);
                step = 4;
            }
        }
        // Use FreeType advance (atlas size)
        FT_Load_Char(f.Face, codepoint, FT_LOAD_DEFAULT);
        xAtlas += f.Face->glyph->advance.x / 64.0f;
        bi += step;
        xAtlas += lsAtlas; // per-codepoint pseudo-clusters, one gap each
        outXByByte[bi] = xAtlas;
    }
    // Fill any gaps (should not be necessary but safe)
    for (size_t i = 1; i < outXByByte.size(); ++i)
        if (outXByByte[i] == 0.0f && i > 0)
            outXByByte[i] = outXByByte[i - 1];
    // Convert atlas-space X positions to render pixels
    if (geometryScale != 1.0f)
    {
        for (float& v : outXByByte)
            v *= geometryScale;
    }
    return true;
}
} // namespace
#endif // GE_HAVE_FREETYPE

bool FontAtlas::GetTypographicHeights(unsigned pixelSize,
                                      float& outCapHeight, bool& outCapValid,
                                      float& outXHeight, bool& outXValid,
                                      float& outWinAscent, float& outWinDescent)
{
#if defined(GE_HAVE_FREETYPE)
    outCapHeight = 0.0f;
    outXHeight = 0.0f;
    outWinAscent = 0.0f;
    outWinDescent = 0.0f;
    outCapValid = false;
    outXValid = false;
    FT_Face face = m_FtFace.Face();
    if (!face)
        return false;
    // Temporarily set to requested render size; restore to atlas size (or pixelSize if 0) afterwards.
    unsigned prevPx = (m_AtlasPixelSize > 0) ? m_AtlasPixelSize : pixelSize;
    m_FtFace.SetPixelSizes(pixelSize);
#if defined(GE_HAVE_HARFBUZZ)
    if (m_HbFont.Get())
    {
        hb_ft_font_changed(m_HbFont.Get());
    }
#endif
    double upm = (face->units_per_EM ? (double)face->units_per_EM : 2048.0);
    double yppem = (face->size && face->size->metrics.y_ppem ? (double)face->size->metrics.y_ppem : (double)pixelSize);
    double scale = (upm > 0.0) ? (yppem / upm) : 1.0;
    TT_OS2* os2 = (TT_OS2*)FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
    if (os2)
    {
        outWinAscent = (float)(os2->usWinAscent * scale);
        outWinDescent = (float)(os2->usWinDescent * scale);
        if (os2->version >= 2)
        {
            if (os2->sCapHeight != 0)
            {
                outCapHeight = (float)(os2->sCapHeight * scale);
                outCapValid = true;
            }
            if (os2->sxHeight != 0)
            {
                outXHeight = (float)(os2->sxHeight * scale);
                outXValid = true;
            }
        }
    }
    else
    {
        // Fallback to FT size metrics (hhea asc/desc) if OS/2 absent and size metrics available.
        if (face->size)
        {
            const FT_Size_Metrics& mt = face->size->metrics;
            outWinAscent = (float)(mt.ascender / 64.0);
            outWinDescent = (float)(-mt.descender / 64.0);
        }
    }
    // Restore atlas pixel size
    m_FtFace.SetPixelSizes(prevPx);
#if defined(GE_HAVE_HARFBUZZ)
    if (m_HbFont.Get())
    {
        hb_ft_font_changed(m_HbFont.Get());
    }
#endif
    return true;
#else
    (void)pixelSize;
    (void)outCapHeight;
    (void)outCapValid;
    (void)outXHeight;
    (void)outXValid;
    (void)outWinAscent;
    (void)outWinDescent;
    return false;
#endif
}

bool FontAtlas::HasGlyph(uint32_t codepoint) const
{
#if !defined(GE_HAVE_FREETYPE)
    (void)codepoint;
    return false;
#else
    if (!m_FtFace.IsValid())
        return false;
    FT_UInt gid = FT_Get_Char_Index(m_FtFace.Face(), (FT_ULong)codepoint);
    return gid != 0;
#endif
}

TextMetrics FontAtlas::MeasureUtf8(std::string_view utf8, float pixelSize,
                                   float letterSpacingPx)
{
#if !defined(GE_HAVE_FREETYPE)
    (void)utf8;
    (void)pixelSize;
    (void)letterSpacingPx;
    return {};
#else
    if (!m_FtFace.IsValid())
        return {};

    // Always shape at the atlas generation size and scale to the requested render size.
    // This keeps measurement, caret mapping and ShapeText rendering in lock-step.
    const unsigned rasterPx = RasterPixelSize(pixelSize);
    const unsigned atlasSize = (m_AtlasPixelSize > 0) ? m_AtlasPixelSize : rasterPx;
    const float geometryScale = std::max(1.0f, pixelSize) / static_cast<float>(atlasSize);

    const bool sized = m_FtFace.SetPixelSizes(atlasSize);
#if defined(GE_HAVE_HARFBUZZ)
    // CRITICAL: Notify HarfBuzz that the FreeType face size has changed!
    if (m_HbFont.Get())
    {
        hb_ft_font_changed(m_HbFont.Get());
    }
#endif

    MeasureFaceRef f{m_FtFace.Face()
#if defined(GE_HAVE_HARFBUZZ)
                     , m_HbFont.Get()
#endif
    };
    // A width shaped at an unverified pixel size must not enter the shared
    // cache under the atlasSize key.
    return MeasureUtf8WithFace(f, utf8, geometryScale, pixelSize,
                               sized ? m_ShapedWidthCache.get() : nullptr, atlasSize,
                               letterSpacingPx);
#endif
}

FontAtlas::MeasureResult FontAtlas::MeasureText(std::string_view utf8, float pixelSize,
                                                float letterSpacingPx)
{
    MeasureResult r{};
    r.metrics = MeasureUtf8(utf8, pixelSize, letterSpacingPx);
    BuildCaretMapUtf8(utf8, pixelSize, r.caretXByByte, letterSpacingPx);
    return r;
}

LineMetrics FontAtlas::GetLineMetrics(float pixelSize) const
{
#if !defined(GE_HAVE_FREETYPE)
    (void)pixelSize;
    return {};
#else
    if (!m_FtFace.IsValid())
        return {};
    return ExactLineMetricsWithFace(m_FtFace.Face(), pixelSize);
#endif
}

LineMetrics FontAtlas::GetFontLineMetrics(float pixelSize) const
{
    return GetLineMetrics(pixelSize);
}

bool FontAtlas::BuildCaretMapUtf8(std::string_view utf8, float pixelSize,
                                  std::vector<float>& outXByByte, float letterSpacingPx)
{
#if !defined(GE_HAVE_FREETYPE)
    (void)utf8;
    (void)pixelSize;
    (void)outXByByte;
    (void)letterSpacingPx;
    return false;
#else
    if (!m_FtFace.IsValid())
    {
        outXByByte.clear();
        outXByByte.resize(utf8.size() + 1, 0.0f);
        return false;
    }

    // Shape at atlas generation size, but report caret X in render pixels.
    // Caret X retains the requested fractional render size.
    const unsigned rasterPx = RasterPixelSize(pixelSize);
    const unsigned atlasSize = (m_AtlasPixelSize > 0) ? m_AtlasPixelSize : rasterPx;
    const float geometryScale = std::max(1.0f, pixelSize) / static_cast<float>(atlasSize);

    m_FtFace.SetPixelSizes(atlasSize);

#if defined(GE_HAVE_HARFBUZZ)
    if (m_HbFont.Get())
    {
        hb_ft_font_changed(m_HbFont.Get());
    }
#endif

    MeasureFaceRef f{m_FtFace.Face()
#if defined(GE_HAVE_HARFBUZZ)
                     , m_HbFont.Get()
#endif
    };
    return BuildCaretMapWithFace(f, utf8, geometryScale, outXByByte, letterSpacingPx);
#endif
}

} // namespace GameEngine::Rendering::Text

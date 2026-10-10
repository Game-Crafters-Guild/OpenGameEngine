#include "FontAtlas_Internal.h"

#include <algorithm>
#include <cassert>
#include <climits>
#include <cmath>
#include <cstring>

std::optional<GameEngine::Rendering::Text::FaceDebugInfo>
GameEngine::Rendering::Text::FontAtlas::GetFaceDebugInfo() const
{
#if defined(GE_HAVE_FREETYPE)
    if (!m_FtFace.IsValid())
        return std::nullopt;
    FT_Face face = m_FtFace.Face();
    if (!face)
        return std::nullopt;
    FaceDebugInfo info{};
    if (face->family_name)
        info.family = face->family_name;
    if (face->style_name)
        info.style = face->style_name;
    if (const char* ps = FT_Get_Postscript_Name(face))
        info.postscript = ps;
    info.numGlyphs = (int)face->num_glyphs;
    info.unitsPerEm = (int)face->units_per_EM;
    info.ascender = (int)face->ascender;
    info.descender = (int)face->descender;
    info.height = (int)face->height;
    return info;
#else
    return std::nullopt;
#endif
}

using namespace GameEngine::Rendering::Text;

namespace GameEngine
{
namespace Rendering
{
namespace Text
{

// Out-of-line so the ShapedWidthCachePtr member destroys where ShapedWidthCache
// is a complete type (defined in FontAtlas_Internal.h; the public header only
// forward-declares it).
FontAtlas::~FontAtlas() = default;

void ShapedWidthCacheDeleter::operator()(ShapedWidthCache* cache) const
{
    delete cache;
}

static uint32_t fnv1a_32(const void* data, size_t len)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i)
    {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

uint32_t FontAtlas::ComputeAtlasIdFromBytes(const void* data, size_t size, unsigned pixelSize) const
{
    uint32_t h = fnv1a_32(data, size);
    h ^= pixelSize + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
}

bool FontAtlas::LoadFontBytes(const unsigned char* bytes, size_t size, unsigned pixelSize)
{
#if !defined(GE_HAVE_FREETYPE)
    (void)bytes;
    (void)size;
    (void)pixelSize;
    return false;
#else
    m_AtlasPixelSize = pixelSize;

    // Cached widths were shaped with the old bytes/pixel size.
    if (!m_ShapedWidthCache)
        m_ShapedWidthCache = ShapedWidthCachePtr(new ShapedWidthCache());
    else
    {
        std::lock_guard<std::mutex> lock(m_ShapedWidthCache->Mutex);
        m_ShapedWidthCache->Map.clear();
    }

    if (!m_FtLib.Init())
        return false;
    if (!m_FtFace.NewMemoryFace(m_FtLib.Lib(), bytes, size))
        return false;
    if (!m_FtFace.SetPixelSizes(pixelSize))
        return false;
    // Cache face properties for perf-oriented shaping decisions.
    m_IsFixedWidth = false;
    if (FT_Face face = m_FtFace.Face())
    {
        m_IsFixedWidth = (face->face_flags & FT_FACE_FLAG_FIXED_WIDTH) != 0;
    }
    // Invalidate ASCII fast caches (face/size changed).
    m_AsciiCacheValid = false;
    m_AsciiKerningValid = false;
    m_AsciiCachePixelSize = 0;
#if defined(GE_HAVE_HARFBUZZ)
    m_HbFont.CreateFromFT(m_FtFace.Face());
    {
        int flags = FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING | FT_LOAD_NO_AUTOHINT;
        m_HbFont.SetLoadFlags(flags);
    }
    // ppem gates GPOS device-table / trak adjustments and hb_ft_font_changed
    // never touches it, so whatever is set here persists until ShapeGlyphs
    // re-sets it (to this same atlas size). Setting it at load means measures
    // BEFORE the first shape agree with the shaped steady state from frame 0.
    if (m_HbFont.Get())
        hb_font_set_ppem(m_HbFont.Get(), pixelSize, pixelSize);
#endif
    m_BasePixelSize = pixelSize;
    m_AtlasId = ComputeAtlasIdFromBytes(bytes, size, pixelSize);
    {
        const uint32_t h = m_AtlasId;
        m_ColorAtlasId = h ^ (0xC010CAFEu + 0x9e3779b9u + (h << 6) + (h >> 2));
    }
    m_ColorAtlasGeneration = 0;
    m_ColorContentGeneration = 0;
    m_SlugPages.clear();
    m_SlugGlyphs.clear();
    return true;
#endif
}

// Shared shaping helper: sets atlas size, syncs HarfBuzz, runs shaping (HarfBuzz / ASCII fast path / FT fallback).
// letterSpacingPx (render px) is folded into xAdv at every cluster end, the
// last one included, so callers consuming the advances see tracked pen
// positions without further handling and their sum is the measured width.
// Non-zero spacing also suppresses optional ligatures, so the tracked and
// untracked glyph runs are not the same run.
// Returns the geometry scale factor (pixelSize / atlasPixelSize).
float FontAtlas::ShapeGlyphs(std::string_view utf8, float pixelSize, bool preferFastAscii,
                             ShapingOutput& out, float letterSpacingPx)
{
    out.Clear();

#if !defined(GE_HAVE_FREETYPE)
    (void)utf8;
    (void)pixelSize;
    (void)preferFastAscii;
    (void)letterSpacingPx;
    return 1.0f;
#else
    if (!m_FtFace.IsValid())
        return 1.0f;

    const unsigned atlasSize = (m_AtlasPixelSize > 0) ? m_AtlasPixelSize : RasterPixelSize(pixelSize);
    m_FtFace.SetPixelSizes(atlasSize);

#if defined(GE_HAVE_HARFBUZZ)
    if (m_HbFont.Get())
    {
        hb_ft_font_changed(m_HbFont.Get());
        hb_font_set_scale(m_HbFont.Get(), atlasSize * 64, atlasSize * 64);
        hb_font_set_ppem(m_HbFont.Get(), atlasSize, atlasSize);
    }
#endif

    const float geometryScale = std::max(1.0f, pixelSize) / static_cast<float>(atlasSize);
    // Advances are atlas-space; tracking converts down and scales back up with
    // them, exact to a ulp. ls == 0 keeps the untracked math byte-identical.
    const float lsAtlas = (geometryScale != 0.0f) ? letterSpacingPx / geometryScale : 0.0f;

    auto isSimpleAscii = [](std::string_view s) -> bool
    {
        for (unsigned char c : s)
        {
            if (c >= 0x80)
                return false;
            if (c < 0x20 && c != '\n')
                return false;
        }
        return true;
    };

#if defined(GE_HAVE_HARFBUZZ)
    const bool allowFastAscii = preferFastAscii && isSimpleAscii(utf8);
    if (m_HbFont.Get() && !allowFastAscii)
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
            out.isRTL = (r.dir == HB_DIRECTION_RTL);
        }
        else
        {
            hb_buffer_guess_segment_properties(s_HbBuf);
            out.isRTL = (hb_buffer_get_direction(s_HbBuf) == HB_DIRECTION_RTL);
        }
        hb_feature_t features[2];
        const unsigned featureCount = Detail::LetterSpacingFeatures(letterSpacingPx, features);
        hb_shape(m_HbFont.Get(), s_HbBuf, featureCount ? features : nullptr, featureCount);
        unsigned count = 0;
        const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(s_HbBuf, &count);
        const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(s_HbBuf, &count);
        out.Reserve(count);
        for (unsigned i = 0; i < count; ++i)
        {
            out.glyphIds.push_back(infos[i].codepoint);
            float adv = pos[i].x_advance / 64.0f;
            // Tracking on the last glyph of each cluster, the run's final
            // cluster included: the advance the caller lays out with must be
            // the same advance the measure path reports.
            if (i + 1 == count || infos[i + 1].cluster != infos[i].cluster)
                adv += lsAtlas;
            out.xAdv.push_back(adv);
            out.xOff.push_back(pos[i].x_offset / 64.0f);
            out.yOff.push_back(pos[i].y_offset / 64.0f);
        }
    }
#endif
    // ASCII fast path
    if (out.glyphIds.empty() && preferFastAscii && isSimpleAscii(utf8))
    {
        FT_Face face = m_FtFace.Face();
        if (face)
        {
            if (!m_AsciiCacheValid || m_AsciiCachePixelSize != atlasSize)
            {
                m_AsciiCachePixelSize = atlasSize;
                for (uint32_t c = 0; c < 128; ++c)
                {
                    const uint32_t gid = (uint32_t)FT_Get_Char_Index(face, (FT_ULong)c);
                    m_AsciiGid[c] = gid;
                    float adv = 0.0f;
                    if (gid != 0 && FT_Load_Glyph(face, gid, FT_LOAD_DEFAULT) == 0)
                        adv = face->glyph->advance.x / 64.0f;
                    m_AsciiAdvancePx[c] = adv;
                }
                m_AsciiCacheValid = true;
                m_AsciiKerningValid = false;
            }
            if (!m_AsciiKerningValid)
            {
                m_AsciiKernPx.fill(0.0f);
                if (FT_HAS_KERNING(face))
                {
                    for (uint32_t a = 0; a < 128; ++a)
                    {
                        const uint32_t ga = m_AsciiGid[a];
                        if (ga == 0)
                            continue;
                        for (uint32_t b = 0; b < 128; ++b)
                        {
                            const uint32_t gb = m_AsciiGid[b];
                            if (gb == 0)
                                continue;
                            FT_Vector kv{};
                            if (FT_Get_Kerning(face, ga, gb, FT_KERNING_DEFAULT, &kv) == 0)
                                m_AsciiKernPx[a * 128 + b] = kv.x / 64.0f;
                        }
                    }
                }
                m_AsciiKerningValid = true;
            }
            out.Reserve(utf8.size());
            out.xOff.assign(utf8.size(), 0.0f);
            out.yOff.assign(utf8.size(), 0.0f);
            unsigned char prev = 0;
            bool havePrev = false;
            for (unsigned char c : utf8)
            {
                if (c >= 0x80)
                    break;
                const uint32_t gid = m_AsciiGid[c];
                out.glyphIds.push_back(gid);
                // One ASCII char = one cluster, so every advance carries a gap.
                float adv = m_AsciiAdvancePx[c] + lsAtlas;
                if (havePrev)
                    adv += m_AsciiKernPx[(uint32_t)prev * 128 + (uint32_t)c];
                out.xAdv.push_back(adv);
                prev = c;
                havePrev = true;
            }
        }
    }
    // FT fallback: no HarfBuzz, no ASCII cache
    if (out.glyphIds.empty())
    {
        out.Reserve(utf8.size());
        out.xOff.assign(utf8.size(), 0.0f);
        out.yOff.assign(utf8.size(), 0.0f);
        for (unsigned char c : utf8)
        {
            uint32_t gid = FT_Get_Char_Index(m_FtFace.Face(), c);
            out.glyphIds.push_back(gid);
            float adv = 0.0f;
            if (FT_Load_Glyph(m_FtFace.Face(), gid, FT_LOAD_DEFAULT) == 0)
                adv = m_FtFace.Face()->glyph->advance.x / 64.0f;
            out.xAdv.push_back(adv);
        }
        // This path emits one glyph per BYTE, but the pseudo-cluster the
        // measure path counts is a CODEPOINT, so the gap lands on the last byte
        // of each one. Anything else makes a no-HarfBuzz build track multibyte
        // text wider in shaping than it measured it.
        for (size_t i = 0; i < out.xAdv.size(); ++i)
        {
            const bool lastByteOfCodepoint =
                (i + 1 == utf8.size()) || (((unsigned char)utf8[i + 1] & 0xC0u) != 0x80u);
            if (lastByteOfCodepoint)
                out.xAdv[i] += lsAtlas;
        }
    }
    return geometryScale;
#endif
}

void FontAtlas::ShapeText(std::string_view utf8, float pixelSize, ShapeResult& out,
                          uint32_t color, float letterSpacingPx)
{
    out.Clear();
#if !defined(GE_HAVE_FREETYPE)
    (void)utf8;
    (void)pixelSize;
    (void)color;
    (void)letterSpacingPx;
    return;
#else
    if (!m_FtFace.IsValid())
        return;

    const unsigned rasterPx = RasterPixelSize(pixelSize);
    static thread_local ShapingOutput shaped;
    const float geometryScale = ShapeGlyphs(utf8, pixelSize, false, shaped, letterSpacingPx);
    const unsigned atlasSize = (m_AtlasPixelSize > 0) ? m_AtlasPixelSize : rasterPx;

    // Line metrics (in render pixels) — the exact design-unit metrics, not
    // FreeType's ppem-rounded size->metrics: the baseline placement here must
    // agree with the line-box math in TextLayout, which reads
    // GetFontLineMetrics.
    {
        const LineMetrics lm = GetLineMetrics(pixelSize);
        out.metrics.ascender = lm.ascender;
        out.metrics.descender = lm.descender;
        out.metrics.height = lm.height;
        out.metrics.baseline = out.metrics.ascender;
    }

    const auto& glyphIds = shaped.glyphIds;
    const auto& xAdv = shaped.xAdv;
    const auto& xOff = shaped.xOff;
    const auto& yOff = shaped.yOff;
    const bool isRTL = shaped.isRTL;

    // Partition glyphs into field vs color and ensure atlas contains them.
    // Thread-local vectors avoid per-call heap allocation.
    static thread_local std::vector<uint32_t> fieldIds;
    static thread_local std::vector<uint32_t> colorIds;
    fieldIds.clear();
    colorIds.clear();
    bool faceHasColor = FT_HAS_COLOR(m_FtFace.Face());
    if (faceHasColor)
    {
        fieldIds.reserve(glyphIds.size());
        for (uint32_t gid : glyphIds)
        {
            if (gid == 0) { fieldIds.push_back(gid); continue; }
            if (FT_Load_Glyph(m_FtFace.Face(), gid, FT_LOAD_COLOR | FT_LOAD_RENDER) == 0)
            {
                FT_GlyphSlot g = m_FtFace.Face()->glyph;
                if (g->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA && g->bitmap.width > 0 && g->bitmap.rows > 0)
                { colorIds.push_back(gid); continue; }
            }
            fieldIds.push_back(gid);
        }
    }

    const uint32_t beforeColorGen = m_ColorAtlasGeneration;
    const size_t beforePageCount = m_SlugPages.size();
    uint32_t beforeLastPageGen = m_SlugPages.empty() ? 0u : m_SlugPages.back().Generation;

    // For non-emoji fonts, pass glyphIds directly (avoids copy).
    const auto& slugGlyphIds = faceHasColor ? fieldIds : glyphIds;
    EnsureSlugGlyphData(slugGlyphIds);
    if (!colorIds.empty())
        MultiPageAddColorGlyphs(colorIds, atlasSize);

    const bool slugChanged = (m_SlugPages.size() != beforePageCount)
        || (!m_SlugPages.empty() && m_SlugPages.back().Generation != beforeLastPageGen);
    out.atlasChanged = (m_ColorAtlasGeneration != beforeColorGen) || slugChanged;

    // Em-to-screen conversion uses the requested fractional render size.
    const float emScale = std::max(1.0f, pixelSize);

    // Compute RTL shift (only needed for RTL text)
    float runMinX = 1e30f, runMaxX = -1e30f;
    if (isRTL)
    {
        float pen = 0.0f;
        for (size_t i = 0; i < glyphIds.size(); ++i)
        {
            uint32_t gid = glyphIds[i];
            const SlugGlyphInfo* slugInfo = nullptr;
            { auto it = m_SlugGlyphs.find(gid); if (it != m_SlugGlyphs.end()) slugInfo = &it->second; }
            const GlyphInfo* colorInfo = nullptr;
            { auto it = m_ColorGlyphsById.find(gid); if (it != m_ColorGlyphsById.end()) colorInfo = &it->second; }
            if (slugInfo && slugInfo->HBandCount > 0)
            {
                float gx0Local = pen + xOff[i] + slugInfo->EmXMin * emScale / geometryScale;
                float gx1Local = pen + xOff[i] + slugInfo->EmXMax * emScale / geometryScale;
                runMinX = std::min(runMinX, gx0Local);
                runMaxX = std::max(runMaxX, gx1Local);
            }
            else if (colorInfo)
            {
                float gx0Local = pen + xOff[i] + colorInfo->bearingX;
                float gx1Local = gx0Local + colorInfo->width;
                runMinX = std::min(runMinX, gx0Local);
                runMaxX = std::max(runMaxX, gx1Local);
            }
            pen += xAdv[i];
        }
        if (!(runMinX < runMaxX)) { runMinX = 0.0f; runMaxX = 0.0f; }
    }
    // RTL extent is in atlas-size pixels; scale to render pixels.
    const float rtlShift = isRTL ? (-runMaxX * geometryScale) : 0.0f;

    // Y-down: baselineY is ascender below the top-left origin
    const float baselineY = out.metrics.ascender;

    out.glyphs.reserve(glyphIds.size());
    float penX = 0.0f;

    for (size_t i = 0; i < glyphIds.size(); ++i)
    {
        uint32_t gid = glyphIds[i];
        const SlugGlyphInfo* slugInfo = nullptr;
        { auto it = m_SlugGlyphs.find(gid); if (it != m_SlugGlyphs.end()) slugInfo = &it->second; }
        const GlyphInfo* colorInfo = nullptr;
        { auto it = m_ColorGlyphsById.find(gid); if (it != m_ColorGlyphsById.end()) colorInfo = &it->second; }

        const bool glyphIsColor = (colorInfo && !slugInfo);
        const bool hasSlugCurves = slugInfo && slugInfo->HBandCount > 0;

        // Skip whitespace glyphs (no curves and no color bitmap)
        if (!hasSlugCurves && !glyphIsColor)
        {
            penX += xAdv[i] * geometryScale;
            continue;
        }

        float baseX = rtlShift + penX + xOff[i] * geometryScale;
        float baseY_glyph = baselineY + yOff[i] * geometryScale;

        GlyphPlacement gp{};
        gp.color = color;
        gp.isColor = glyphIsColor;

        if (glyphIsColor)
        {
            // Color emoji path (unchanged)
            const GlyphInfo& gi = *colorInfo;
            gp.x = baseX + gi.bearingX * geometryScale;
            gp.y = baseY_glyph - gi.bearingY * geometryScale;
            gp.width = gi.bitmapWidth * geometryScale;
            gp.height = gi.bitmapHeight * geometryScale;
            gp.u0 = gi.u0;
            gp.v0 = gi.v0;
            gp.u1 = gi.u1;
            gp.v1 = gi.v1;
            gp.atlasPageIndex = static_cast<uint32_t>(gi.pageIndex);
        }
        else
        {
            // Slug glyph: undilated screen-space rect from em-space bounds.
            // The vertex shader adds half-pixel dilation dynamically so it
            // stays correct under zoom/DPI changes.
            float gx = baseX + slugInfo->EmXMin * emScale;
            float gy = baseY_glyph - slugInfo->EmYMax * emScale;
            float gw = (slugInfo->EmXMax - slugInfo->EmXMin) * emScale;
            float gh = (slugInfo->EmYMax - slugInfo->EmYMin) * emScale;

            gp.x = gx;
            gp.y = gy;
            gp.width = gw;
            gp.height = gh;

            // Undilated em-space bounds for the vertex shader.
            // Y is flipped: screen-top (p01.y=0) maps to em-top (EmYMax),
            // screen-bottom (p01.y=1) maps to em-bottom (EmYMin).
            gp.u0 = slugInfo->EmXMin;
            gp.v0 = slugInfo->EmYMax;   // screen top = em-space top
            gp.u1 = slugInfo->EmXMax;
            gp.v1 = slugInfo->EmYMin;   // screen bottom = em-space bottom
            gp.atlasPageIndex = 0;
            gp.glyphLocX = slugInfo->GlyphLocX;
            gp.glyphLocY = slugInfo->GlyphLocY;
            gp.hBandCount = slugInfo->HBandCount;
            gp.vBandCount = slugInfo->VBandCount;
            gp.pageIndex = slugInfo->PageIndex;

            // Precompute band transform: em-space coordinate → band index.
            // bandTransform.x scales renderCoord.x → vertical band index (vBandCount bands)
            // bandTransform.y scales renderCoord.y → horizontal band index (hBandCount bands)
            float emWidth = slugInfo->EmXMax - slugInfo->EmXMin;
            float emHeight = slugInfo->EmYMax - slugInfo->EmYMin;
            gp.bandScaleX = (slugInfo->VBandCount > 0 && emWidth > 0.0f)
                ? static_cast<float>(slugInfo->VBandCount) / emWidth : 0.0f;
            gp.bandScaleY = (slugInfo->HBandCount > 0 && emHeight > 0.0f)
                ? static_cast<float>(slugInfo->HBandCount) / emHeight : 0.0f;
            gp.bandOffX = -slugInfo->EmXMin * gp.bandScaleX;
            gp.bandOffY = -slugInfo->EmYMin * gp.bandScaleY;
        }

        out.glyphs.push_back(gp);
        penX += xAdv[i] * geometryScale;
    }

    out.metrics.width = penX;
#endif
}

// EnsureSlugGlyphData is defined in FontAtlas_Slug.cpp

bool FontAtlas::MultiPageAddColorGlyphs(const std::vector<uint32_t>& glyphIds, unsigned pixelSize)
{
#if !defined(GE_HAVE_FREETYPE)
    (void)glyphIds;
    (void)pixelSize;
    return false;
#else
    if (!m_FtFace.IsValid())
        return false;
    m_FtFace.SetPixelSizes(pixelSize);
    bool anyPixelsChanged = false;
    // Ensure at least one color page exists
    if (m_ColorPages.empty())
    {
        AtlasPage p{};
        p.width = m_InitW;
        p.height = m_InitH;
        p.pixels.resize(p.width * p.height * 4, 0);
        p.penX = p.penY = p.rowH = 0;
        m_ColorPages.push_back(std::move(p));
        ++m_ColorAtlasGeneration;
        anyPixelsChanged = true;
    }

    for (uint32_t gid : glyphIds)
    {
        if (gid == 0)
            continue;
        if (m_ColorGlyphsById.find(gid) != m_ColorGlyphsById.end())
            continue;
        // Prefer COLR/CPAL vector color if available; otherwise fall back to bitmap BGRA
        // COLR/CPAL path: composite layers into a single RGBA tile at the requested pixel size
        {
            FT_LayerIterator it;
            std::memset(&it, 0, sizeof(it));
            FT_UInt layerGlyph = 0;
            FT_UInt colorIndex = 0;
            bool hasLayers = FT_Get_Color_Glyph_Layer(m_FtFace.Face(), gid, &layerGlyph, &colorIndex, &it) != 0;
            if (hasLayers)
            {
                // Select default palette (0); ignore index out parameter
                FT_Color* palette = nullptr;
                (void)FT_Palette_Select(m_FtFace.Face(), 0, &palette);
                // First pass: extents
                int minX = INT_MAX, maxX = INT_MIN, maxTop = 0, maxBottom = 0;
                std::memset(&it, 0, sizeof(it));
                while (FT_Get_Color_Glyph_Layer(m_FtFace.Face(), gid, &layerGlyph, &colorIndex, &it))
                {
                    if (FT_Load_Glyph(m_FtFace.Face(), layerGlyph, FT_LOAD_DEFAULT) != 0)
                        continue;
                    if (FT_Render_Glyph(m_FtFace.Face()->glyph, FT_RENDER_MODE_NORMAL) != 0)
                        continue;
                    FT_GlyphSlot lg = m_FtFace.Face()->glyph;
                    int left = (int)lg->bitmap_left;
                    int top = (int)lg->bitmap_top;
                    int w = (int)lg->bitmap.width;
                    int h = (int)lg->bitmap.rows;
                    if (w <= 0 || h <= 0)
                        continue;
                    minX = std::min(minX, left);
                    maxX = std::max(maxX, left + w);
                    maxTop = std::max(maxTop, top);
                    maxBottom = std::max(maxBottom, std::max(0, h - top));
                }
                if (minX == INT_MAX || maxX <= minX || (maxTop + maxBottom) <= 0)
                { /* no visible layers */
                    goto BITMAP_FALLBACK;
                }
                const int gw = maxX - minX;
                const int gh = maxTop + maxBottom;
                std::vector<uint8_t> rgba((size_t)gw * (size_t)gh * 4, 0);
                // Second pass: composite
                std::memset(&it, 0, sizeof(it));
                while (FT_Get_Color_Glyph_Layer(m_FtFace.Face(), gid, &layerGlyph, &colorIndex, &it))
                {
                    if (FT_Load_Glyph(m_FtFace.Face(), layerGlyph, FT_LOAD_DEFAULT) != 0)
                        continue;
                    if (FT_Render_Glyph(m_FtFace.Face()->glyph, FT_RENDER_MODE_NORMAL) != 0)
                        continue;
                    FT_GlyphSlot lg = m_FtFace.Face()->glyph;
                    int left = (int)lg->bitmap_left;
                    int top = (int)lg->bitmap_top;
                    int w = (int)lg->bitmap.width;
                    int h = (int)lg->bitmap.rows;
                    if (w <= 0 || h <= 0)
                        continue;
                    uint8_t cr = 255, cg = 255, cb = 255, ca = 255;
                    if (palette)
                    {
                        FT_Color c = palette[colorIndex];
                        cb = c.blue;
                        cg = c.green;
                        cr = c.red;
                        ca = c.alpha;
                    }
                    const uint8_t* src = lg->bitmap.buffer;
                    const int sp = lg->bitmap.pitch;
                    for (int y = 0; y < h; ++y)
                    {
                        const uint8_t* srow = src + y * sp;
                        int dy = (maxTop - top) + y;
                        if (dy < 0 || dy >= gh)
                            continue;
                        for (int x = 0; x < w; ++x)
                        {
                            int dx = (left - minX) + x;
                            if (dx < 0 || dx >= gw)
                                continue;
                            uint8_t cov = srow[x]; // FT_RENDER_MODE_NORMAL gives 8bpp coverage
                            if (!cov)
                                continue;
                            uint32_t srcA = (uint32_t)((uint32_t)cov * (uint32_t)ca + 127) / 255u; // 0..255
                            uint8_t* dpx = &rgba[(size_t)(dy * gw + dx) * 4];
                            // Alpha over (non-premultiplied dest)
                            uint32_t invA = 255u - srcA;
                            dpx[0] = (uint8_t)(((uint32_t)cr * srcA + (uint32_t)dpx[0] * invA + 127) / 255u);
                            dpx[1] = (uint8_t)(((uint32_t)cg * srcA + (uint32_t)dpx[1] * invA + 127) / 255u);
                            dpx[2] = (uint8_t)(((uint32_t)cb * srcA + (uint32_t)dpx[2] * invA + 127) / 255u);
                            dpx[3] = (uint8_t)(std::min(255u, srcA + ((uint32_t)dpx[3] * invA + 127) / 255u));
                        }
                    }
                }
                // Pack into atlas
                const int pad = 1;
                int pageIndex = -1, dstX = 0, dstY = 0;
                for (int pi = 0; pi < (int)m_ColorPages.size(); ++pi)
                {
                    auto& p = m_ColorPages[pi];
                    if (p.penX + gw + pad > p.width)
                    {
                        p.penX = 0;
                        p.penY += p.rowH;
                        p.rowH = 0;
                    }
                    if (p.penY + gh + pad > p.height)
                    {
                        continue;
                    }
                    dstX = p.penX;
                    dstY = p.penY;
                    p.penX += gw + pad;
                    p.rowH = std::max(p.rowH, gh + pad);
                    pageIndex = pi;
                    break;
                }
                if (pageIndex < 0)
                {
                    AtlasPage p{};
                    p.width = m_InitW;
                    p.height = m_InitH;
                    p.pixels.resize(p.width * p.height * 4, 0);
                    p.penX = p.penY = p.rowH = 0;
                    m_ColorPages.push_back(std::move(p));
                    pageIndex = (int)m_ColorPages.size() - 1;
                    anyPixelsChanged = true;
                }
                auto& page = m_ColorPages[pageIndex];
                for (int y = 0; y < gh; ++y)
                {
                    const uint8_t* srow = rgba.data() + (size_t)y * (size_t)gw * 4;
                    uint8_t* drow = page.pixels.data() + ((dstY + y) * page.width + dstX) * 4;
                    std::memcpy(drow, srow, (size_t)gw * 4);
                }
                anyPixelsChanged = true;
                // Base glyph advance and bearings
                (void)FT_Load_Glyph(m_FtFace.Face(), gid, FT_LOAD_DEFAULT);
                FT_GlyphSlot gbase = m_FtFace.Face()->glyph;
                GlyphInfo gi{};
                gi.width = (float)gw;
                gi.height = (float)gh;
                gi.bitmapWidth = (float)gw;
                gi.bitmapHeight = (float)gh;
                gi.bearingX = (float)minX;
                gi.bearingY = (float)maxTop;
                gi.advance = (int)gbase->advance.x;
                gi.atlasX = dstX;
                gi.atlasY = dstY;
                gi.pageIndex = pageIndex;
                gi.u0 = (float)dstX / (float)page.width;
                gi.v0 = (float)dstY / (float)page.height;
                gi.u1 = (float)(dstX + gw) / (float)page.width;
                gi.v1 = (float)(dstY + gh) / (float)page.height;
                m_ColorGlyphsById[gid] = gi;
                continue; // next glyph
            }
        }
    BITMAP_FALLBACK:
        if (FT_Load_Glyph(m_FtFace.Face(), gid, FT_LOAD_COLOR | FT_LOAD_RENDER) != 0)
            continue;
        FT_GlyphSlot g = m_FtFace.Face()->glyph;
        if (g->bitmap.pixel_mode != FT_PIXEL_MODE_BGRA || g->bitmap.width == 0 || g->bitmap.rows == 0)
            continue;
        {
            const int gw = (int)g->bitmap.width;
            const int gh = (int)g->bitmap.rows;
            const int pad = 1; // tiny pad to avoid bleeding
            // Try to pack on existing pages
            int pageIndex = -1, dstX = 0, dstY = 0;
            for (int pi = 0; pi < (int)m_ColorPages.size(); ++pi)
            {
                auto& page = m_ColorPages[pi];
                if (page.penX + gw + pad > page.width)
                {
                    page.penX = 0;
                    page.penY += page.rowH;
                    page.rowH = 0;
                }
                if (page.penY + gh + pad > page.height)
                {
                    continue;
                }
                dstX = page.penX;
                dstY = page.penY;
                page.penX += gw + pad;
                page.rowH = std::max(page.rowH, gh + pad);
                pageIndex = pi;
                break;
            }
            if (pageIndex < 0)
            {
                // allocate new page
                AtlasPage p{};
                p.width = m_InitW;
                p.height = m_InitH;
                p.pixels.resize(p.width * p.height * 4, 0);
                p.penX = p.penY = p.rowH = 0;
                m_ColorPages.push_back(std::move(p));
                pageIndex = (int)m_ColorPages.size() - 1;
                anyPixelsChanged = true;
            }
            auto& page = m_ColorPages[pageIndex];
            // Copy BGRA -> RGBA
            const uint8_t* src = g->bitmap.buffer;
            const int srcPitch = g->bitmap.pitch;
            for (int y = 0; y < gh; ++y)
            {
                const uint8_t* srow = src + y * srcPitch;
                uint8_t* drow = page.pixels.data() + ((dstY + y) * page.width + dstX) * 4;
                for (int x = 0; x < gw; ++x)
                {
                    uint8_t b = srow[x * 4 + 0];
                    uint8_t gch = srow[x * 4 + 1];
                    uint8_t r = srow[x * 4 + 2];
                    uint8_t a = srow[x * 4 + 3];
                    drow[x * 4 + 0] = r;
                    drow[x * 4 + 1] = gch;
                    drow[x * 4 + 2] = b;
                    drow[x * 4 + 3] = a;
                }
            }
            anyPixelsChanged = true;
            // Store glyph info
            GlyphInfo gi{};
            gi.width = (float)gw;
            gi.height = (float)gh;
            gi.bitmapWidth = (float)gw;
            gi.bitmapHeight = (float)gh;
            gi.bearingX = (float)g->bitmap_left;
            gi.bearingY = (float)g->bitmap_top;
            gi.advance = (int)(g->advance.x);
            gi.atlasX = dstX;
            gi.atlasY = dstY;
            gi.pageIndex = pageIndex;
            gi.u0 = (float)dstX / (float)page.width;
            gi.v0 = (float)dstY / (float)page.height;
            gi.u1 = (float)(dstX + gw) / (float)page.width;
            gi.v1 = (float)(dstY + gh) / (float)page.height;
            m_ColorGlyphsById[gid] = gi;
        }
    }
    if (anyPixelsChanged)
    {
        ++m_ColorContentGeneration;
    }
    return true;
#endif
}

int FontAtlas::GetColorPageCount() const
{
    return (int)m_ColorPages.size();
}

GlyphAtlasRef FontAtlas::GetColorAtlasRef(int pageIndex) const
{
    GlyphAtlasRef r{};
    r.atlasId = m_ColorAtlasId;
    r.generation = m_ColorAtlasGeneration;
    if (pageIndex >= 0 && pageIndex < (int)m_ColorPages.size())
    {
        r.width = m_ColorPages[pageIndex].width;
        r.height = m_ColorPages[pageIndex].height;
    }
    return r;
}

std::optional<AtlasPixels> FontAtlas::GetColorAtlasPixels(int pageIndex) const
{
    if (pageIndex < 0 || pageIndex >= (int)m_ColorPages.size())
        return std::nullopt;
    const auto& p = m_ColorPages[pageIndex];
    AtlasPixels ap{};
    ap.data = p.pixels.data();
    ap.rowPitch = (size_t)p.width * 4;
    return ap;
}

} // namespace Text
} // namespace Rendering
} // namespace GameEngine

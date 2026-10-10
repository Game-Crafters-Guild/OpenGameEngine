#pragma once

#include <cstdint>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Rendering/Text/FTUtils.h"

namespace GameEngine
{
namespace Rendering
{
namespace Text
{

struct TextMetrics
{
    float width = 0;
    float height = 0;
    float ascender = 0;
    float descender = 0;
    float baseline = 0; // equals ascender for typical top-origin layout
};
// A CSS `normal` line box. The face's line gap is included in height and split
// as half-leading, so ascender is the baseline position within the box and
// ascender + descender == height.
struct LineMetrics
{
    float ascender = 0.0f;
    float descender = 0.0f;
    float height = 0.0f;
};

struct FaceDebugInfo
{
    std::string family;
    std::string style;
    std::string postscript;
    int numGlyphs = 0;
    int unitsPerEm = 0;
    int ascender = 0;
    int descender = 0;
    int height = 0;
};

struct GlyphAtlasRef
{
    uint32_t atlasId = 0;    // stable per font bytes + pixel size
    uint32_t generation = 0; // increments on rebuild
    int width = 0;
    int height = 0;
};
struct AtlasPixels
{
    const uint8_t* data = nullptr; // R8 pixels
    size_t rowPitch = 0;           // bytes per row
};

// Forward declarations — defined in FontAtlas_Internal.h.
struct ShapingOutput;
struct ShapedWidthCache;

// Out-of-line deleter so the unique_ptr<ShapedWidthCache> member never requires
// the internal ShapedWidthCache definition in including TUs (MSVC instantiates
// default_delete eagerly with the class).
struct ShapedWidthCacheDeleter
{
    void operator()(ShapedWidthCache* cache) const;
};
using ShapedWidthCachePtr = std::unique_ptr<ShapedWidthCache, ShapedWidthCacheDeleter>;

class FontAtlas
{
  public:
    FontAtlas() = default;
    // Out-of-line: the ShapedWidthCachePtr member references ShapedWidthCache,
    // which is internal-only.
    ~FontAtlas();

    // Per-glyph Slug metadata (em-space bounds and band texture coordinates).
    struct SlugGlyphInfo
    {
        float EmXMin = 0, EmYMin = 0, EmXMax = 0, EmYMax = 0;
        int GlyphLocX = 0, GlyphLocY = 0;
        uint16_t HBandCount = 0, VBandCount = 0;
        uint16_t PageIndex = 0; // which SlugPage this glyph's data lives on
    };

    // A single GPU page of Slug data (curves + bands). Pre-allocated at fixed
    // dimensions and append-only. When a page fills up, a new page is created.
    // This keeps GPU textures stable — never resized or reallocated — which
    // eliminates flicker from descriptor index churn.
    struct SlugPage
    {
        // Pre-allocated to kSlugTextureWidth × kSlugPageRows at page creation.
        std::vector<float> CurveData;     // RGBA16F (4 floats per texel)
        std::vector<uint16_t> BandData;   // RG16UI  (2 uint16 per texel)
        // Allocation pens (next free texel).
        int CurvePenX = 0, CurvePenY = 0;
        int BandPenX = 0, BandPenY = 0;
        // Highest row that has been written to (used to skip uploading blank rows).
        int CurveRowsUsed = 0;
        int BandRowsUsed = 0;
        // Per-page generation — increments when new glyphs are packed into it.
        uint32_t Generation = 0;
    };

  public:
    // Initialize from a memory buffer and set base pixel size used for atlas generation
    bool LoadFontBytes(const unsigned char* bytes, size_t size, unsigned pixelSize);

    // Shaping-aware text measurement using HarfBuzz when available.
    //
    // letterSpacingPx (CSS letter-spacing, device px) is added once per
    // typographic cluster — after each cluster, never inside one (combining
    // marks stay fused to their base), the run's last cluster included. A run of
    // N clusters is therefore exactly N x spacing wider than its untracked self,
    // and the trailing gap is part of the advance: that is what Chrome measures
    // (letter-spacing:5px on "A" is +5px, and the end-of-text caret carries the
    // last gap), and it is what makes sub-runs compose — measuring a prefix and
    // the rest separately sums to measuring the whole.
    //
    // Non-zero spacing also suppresses optional ligatures (CSS Text 3 8.2), so a
    // tracked run is shaped from its de-ligated glyphs; otherwise a word's
    // tracking would depend on which letter pairs happen to ligate. Every
    // measuring/shaping entry point below shares this definition.
    TextMetrics MeasureUtf8(std::string_view utf8, float pixelSize,
                            float letterSpacingPx = 0.0f);

    // Unified measurement API: returns both metrics and caret map so callers can
    // use a single source of truth for layout, caret positioning and selection.
    // Prefer this for new code and for UI components that need caret/selection.
    struct MeasureResult
    {
        TextMetrics metrics;
        std::vector<float> caretXByByte; // UTF-8 byte insertion points [0..size()], in render pixels
    };

    // Preferred high-level entry point for measuring UTF-8 text plus caret map.
    MeasureResult MeasureText(std::string_view utf8, float pixelSize,
                              float letterSpacingPx = 0.0f);

    // True when the loaded face is fixed-width (monospace). Used for perf-oriented
    // fast paths (e.g., UI perf overlays) where HarfBuzz shaping is unnecessary.
    bool IsFixedWidth() const { return m_IsFixedWidth; }

    // The CSS `normal` line box at this pixel size, computed the way Blink does:
    // ascent, descent and line gap each rounded to a whole pixel and summed,
    // with the baseline at floor(ascent + half the gap).
    //
    // pixelSize is the DEVICE size the text is set at (font size x content
    // scale) and is deliberately fractional: Blink instantiates the font at the
    // fractional device size and rounds there, so rounding at a truncated whole
    // ppem instead loses up to two device pixels of line box per line at
    // fractional display scales. See ShapeText for the raster size, which is
    // still whole.
    LineMetrics GetLineMetrics(float pixelSize) const;

    // Preferred name for user-facing font line metrics; forwards to GetLineMetrics
    // so there is a single typographic definition used throughout the engine.
    LineMetrics GetFontLineMetrics(float pixelSize) const;

    // Optional debug info about the loaded FreeType face.
    std::optional<FaceDebugInfo> GetFaceDebugInfo() const;

    // Build a caret map: x positions (in render pixels) at each UTF-8 byte insertion point [0..utf8.size()].
    // Uses HarfBuzz shaping when available to ensure positions match rendered geometry.
    bool BuildCaretMapUtf8(std::string_view utf8, float pixelSize, std::vector<float>& outXByByte,
                           float letterSpacingPx = 0.0f);

    // Query: does this font contain a glyph for the given Unicode codepoint?
    bool HasGlyph(uint32_t codepoint) const;

    // Per-glyph placement output for the UI renderer.
    struct GlyphPlacement
    {
        float x, y, width, height;   // screen-space quad (Y-down, top-left origin, render pixels)
        // Color emoji: atlas UV coordinates. Slug text: undilated em-space bounds.
        float u0, v0, u1, v1;
        uint32_t atlasPageIndex;      // color emoji: atlas page index. slug: unused
        uint32_t color;               // per-glyph ARGB color (stamped from caller)
        bool isColor;                 // true = RGBA color emoji page, false = Slug curve glyph
        // Slug-specific data (ignored for color glyphs)
        int glyphLocX = 0, glyphLocY = 0;     // position in band texture
        uint16_t hBandCount = 0, vBandCount = 0;
        uint16_t pageIndex = 0;               // which SlugPage carries this glyph's data
        // Precomputed band transform: maps em-space coord → band index
        // bandIndex = renderCoord * bandScale + bandOff, clamped to [0, bandMax]
        float bandScaleX = 0, bandScaleY = 0, bandOffX = 0, bandOffY = 0;
    };
    struct ShapeResult
    {
        std::vector<GlyphPlacement> glyphs;
        TextMetrics metrics;
        bool atlasChanged = false;

        void Clear()
        {
            glyphs.clear();
            metrics = {};
            atlasChanged = false;
        }
    };

    // Shape text into glyph placements for the SDF UI renderer.
    // Output parameter pattern: caller provides ShapeResult& for vector reuse (zero alloc after warmup).
    // Coordinates are Y-down with (originX, originY) at top-left of the line box.
    // The atlasPageIndex in each GlyphPlacement refers to field or color atlas pages
    // (check isColor to distinguish). Caller is responsible for mapping page indices
    // to GPU texture handles via UITextureRegistry.
    // Glyph advances, quad extents and carets retain the fractional pixelSize;
    // atlas storage uses its fixed raster size.
    void ShapeText(std::string_view utf8, float pixelSize, ShapeResult& out,
                   uint32_t color = 0xFF000000u, float letterSpacingPx = 0.0f);

    // Atlas info and pixels for uploading by a renderer
    // Color (RGBA) multi-page helpers (emoji/bitmaps)
    int GetColorPageCount() const;
    GlyphAtlasRef GetColorAtlasRef(int pageIndex) const;
    std::optional<AtlasPixels> GetColorAtlasPixels(int pageIndex) const;
    uint32_t GetColorContentGeneration() const { return m_ColorContentGeneration; }

    // Slug texture dimensions (matches shader's kLogBandTextureWidth = 12).
    static constexpr int kSlugTextureWidth = 4096;
    static constexpr int kSlugPageRows    = 64;   // pre-allocated rows per page

    // Slug per-page data accessors. Each page is a fixed-size GPU texture;
    // data is append-only within a page. Pages are created on demand when the
    // current one fills up. Pages never resize, so GPU textures and their
    // bindless slot indices stay stable across the font's lifetime.
    int GetSlugPageCount() const { return static_cast<int>(m_SlugPages.size()); }
    const float* GetSlugPageCurveData(int pageIndex) const
    {
        return m_SlugPages[pageIndex].CurveData.data();
    }
    const uint16_t* GetSlugPageBandData(int pageIndex) const
    {
        return m_SlugPages[pageIndex].BandData.data();
    }
    // Number of rows in the page that contain meaningful data. The GPU texture
    // is allocated to full kSlugPageRows, but uploads can skip trailing rows.
    int GetSlugPageCurveRowsUsed(int pageIndex) const
    {
        return m_SlugPages[pageIndex].CurveRowsUsed;
    }
    int GetSlugPageBandRowsUsed(int pageIndex) const
    {
        return m_SlugPages[pageIndex].BandRowsUsed;
    }
    // Per-page generation. Increments when new glyphs are packed into this page.
    // Callers compare against a cached value to decide whether to re-upload.
    uint32_t GetSlugPageGeneration(int pageIndex) const
    {
        return m_SlugPages[pageIndex].Generation;
    }

    // Stable ID for this font (available after LoadFontBytes).
    uint32_t GetAtlasId() const { return m_AtlasId; }

    // Query typographic heights at a render pixel size (cap-height/x-height if available, plus Win asc/desc)
    // Returns false if FreeType is not available
    bool GetTypographicHeights(unsigned pixelSize,
                               float& outCapHeight, bool& outCapValid,
                               float& outXHeight, bool& outXValid,
                               float& outWinAscent, float& outWinDescent);

  private:
    struct AtlasPage
    {
        std::vector<uint8_t> pixels;
        int width = 0, height = 0;
        int penX = 0, penY = 0, rowH = 0;
    };

#if defined(GE_HAVE_FREETYPE)
    struct GlyphInfo
    {
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
        float width = 0, height = 0;
        float bitmapWidth = 0, bitmapHeight = 0;
        float bearingX = 0, bearingY = 0;
        int advance = 0; // 26.6 fixed px*64
        int atlasX = 0, atlasY = 0;
        int pageIndex = 0;
    };
    bool MultiPageAddColorGlyphs(const std::vector<uint32_t>& glyphIds, unsigned pixelSize);
#endif

    // Internal helpers
    uint32_t ComputeAtlasIdFromBytes(const void* data, size_t size, unsigned pixelSize) const;
    std::vector<AtlasPage> m_ColorPages;                       // Color RGBA pages (emoji)
    std::unordered_map<uint32_t, GlyphInfo> m_ColorGlyphsById;
    uint32_t m_ColorAtlasGeneration = 0;
    uint32_t m_ColorContentGeneration = 0;

    // Atlas generation pixel size
    unsigned m_AtlasPixelSize = 0;

    // Slug data is organized into append-only pages. Each page pre-allocates
    // fixed-size GPU-shaped buffers (curve + band). When the current page can't
    // fit a new glyph, a fresh page is appended.
    std::vector<SlugPage> m_SlugPages;
    std::unordered_map<uint32_t, SlugGlyphInfo> m_SlugGlyphs;

    bool EnsureSlugGlyphData(const std::vector<uint32_t>& glyphIds);

    // Shared shaping helper: runs HarfBuzz (or ASCII fast path / FT fallback) to produce
    // glyph IDs and per-glyph positioning. Output is in atlas-size pixels.
    // letterSpacingPx (render px, see MeasureUtf8) is folded into the advance
    // of each cluster's last glyph, the run's final cluster included, so every
    // consumer of the advances sees tracked pen positions and their sum is the
    // measured width.
    // Returns the computed geometry scale factor (pixelSize / atlasPixelSize).
    // No default for the spacing: the bug this argument exists to fix was a
    // call site that silently measured without it.
    float ShapeGlyphs(std::string_view utf8, float pixelSize, bool preferFastAscii,
                      ShapingOutput& out, float letterSpacingPx);

  private:
    unsigned m_BasePixelSize = 0;
    bool m_IsFixedWidth = false;
    // Fast ASCII caches (used for digit-heavy overlays).
    // Values are at atlas generation size (m_AtlasPixelSize); geometryScale is applied later.
    bool m_AsciiCacheValid = false;
    bool m_AsciiKerningValid = false;
    unsigned m_AsciiCachePixelSize = 0;
    std::array<uint32_t, 128> m_AsciiGid{};        // ASCII codepoint -> glyph id (0 if missing)
    std::array<float, 128> m_AsciiAdvancePx{};     // ASCII codepoint -> x advance (pixels, atlas size)
    std::array<float, 128 * 128> m_AsciiKernPx{};  // pair kerning (pixels, atlas size), indexed [a*128+b]

#if defined(GE_HAVE_FREETYPE)
    FreeTypeLib m_FtLib;
    FreeTypeFace m_FtFace;
#endif

#if defined(GE_HAVE_HARFBUZZ)
    HBFont m_HbFont;
#endif

    uint32_t m_AtlasId = 0;
    // Color atlas pages must not share the same atlasId as the field atlas.
    uint32_t m_ColorAtlasId = 0;

    // Packing config (initial page size for color atlas pages)
    int m_InitW = 1024;
    int m_InitH = 1024;

    // Shaped-width LRU (internally locked): (utf8 hash, atlas px) → atlas-space
    // advance width, so repeated measures of the same text skip shaping. Unlike
    // the UI's per-node cache it survives element destruction, and it serves
    // wrap line-breaking (per-word measures) cheaply. Created by LoadFontBytes;
    // cleared on font reload. Declared last: destroyed after the members its
    // entries were shaped from.
    ShapedWidthCachePtr m_ShapedWidthCache;
};

} // namespace Text
} // namespace Rendering
} // namespace GameEngine

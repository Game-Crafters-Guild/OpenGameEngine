#pragma once

// Private implementation helpers for FontAtlas.
// This header must only be included by Text module .cpp files (not public headers).

#include "Rendering/Text/FontAtlas.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

#if defined(GE_HAVE_FREETYPE)
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_OUTLINE_H
#include FT_COLOR_H
#include FT_TRUETYPE_TABLES_H
#endif

#if defined(GE_HAVE_HARFBUZZ)
#include <hb.h>
#include <hb-ft.h>
#endif

namespace GameEngine::Rendering::Text
{

#if defined(GE_HAVE_HARFBUZZ)
namespace Detail
{
struct HBDirScript
{
    hb_direction_t dir;
    hb_script_t script;
    const char* lang;
};

// Heuristic: detect text direction and script from the first strong character in a UTF-8 string.
// Shared between measurement, caret mapping, and mesh building so all HarfBuzz calls see identical properties.
inline HBDirScript DetectDirScriptUtf8(std::string_view s)
{
    HBDirScript r{HB_DIRECTION_LTR, HB_SCRIPT_INVALID, nullptr};
    auto nextCodepoint = [](const char* p, const char* e, uint32_t& out) -> const char*
    {
        if (p >= e)
        {
            out = 0;
            return e;
        }
        unsigned char c = (unsigned char)*p;
        if (c < 0x80)
        {
            out = c;
            return p + 1;
        }
        if ((c >> 5) == 0x6 && p + 1 < e)
        {
            out = ((c & 0x1F) << 6) | ((unsigned char)p[1] & 0x3F);
            return p + 2;
        }
        if ((c >> 4) == 0xE && p + 2 < e)
        {
            out = ((c & 0x0F) << 12) |
                  (((unsigned char)p[1] & 0x3F) << 6) |
                  ((unsigned char)p[2] & 0x3F);
            return p + 3;
        }
        if ((c >> 3) == 0x1E && p + 3 < e)
        {
            out = ((c & 0x07) << 18) |
                  (((unsigned char)p[1] & 0x3F) << 12) |
                  (((unsigned char)p[2] & 0x3F) << 6) |
                  ((unsigned char)p[3] & 0x3F);
            return p + 4;
        }
        out = 0;
        return e;
    };

    const char* p = s.data();
    const char* e = s.data() + s.size();
    uint32_t cp = 0;
    while (p < e)
    {
        p = nextCodepoint(p, e, cp);
        if (!cp)
            break;
        // Hebrew
        if (cp >= 0x0590 && cp <= 0x05FF)
        {
            r.dir = HB_DIRECTION_RTL;
            r.script = HB_SCRIPT_HEBREW;
            r.lang = "he";
            break;
        }
        // Arabic
        if ((cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F) || (cp >= 0x08A0 && cp <= 0x08FF))
        {
            r.dir = HB_DIRECTION_RTL;
            r.script = HB_SCRIPT_ARABIC;
            r.lang = "ar";
            break;
        }
        // Basic Latin: treat as strong LTR
        if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z'))
        {
            r.dir = HB_DIRECTION_LTR;
            r.script = HB_SCRIPT_LATIN;
            r.lang = "en";
            break;
        }
        // Digits: weak; keep scanning
        if (cp >= '0' && cp <= '9')
            continue;
    }
    return r;
}
// Reusable thread-local HarfBuzz buffer — avoids per-call heap churn.
// Used by MeasureUtf8, BuildCaretMapUtf8, and ShapeText.
inline hb_buffer_t* GetThreadLocalHBBuffer()
{
    static thread_local hb_buffer_t* s_HbBuf = nullptr;
    if (!s_HbBuf)
        s_HbBuf = hb_buffer_create();
    hb_buffer_clear_contents(s_HbBuf);
    return s_HbBuf;
}

// Optional ligatures must not survive non-zero letter-spacing (CSS Text 3
// 8.2): tracking a ligated pair would space the fused glyph as one cluster and
// make the tracking of a word depend on which letter pairs happen to ligate.
// Blink drops the common and contextual ligature features; Chrome's tracked
// "fi" at 16px/2px is then exactly the DE-LIGATED width plus 2 x spacing
// (9.4375 + 4 = 13.4375, measured), never the ligated width plus spacing.
// Discretionary and historical ligatures are off by default in HarfBuzz, so
// liga + clig is the whole optional set this engine can turn on.
inline unsigned LetterSpacingFeatures(float letterSpacingPx, hb_feature_t (&out)[2])
{
    if (letterSpacingPx == 0.0f)
        return 0;
    out[0] = {HB_TAG('l', 'i', 'g', 'a'), 0, HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END};
    out[1] = {HB_TAG('c', 'l', 'i', 'g'), 0, HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END};
    return 2;
}
} // namespace Detail
#endif

// Shaped-width LRU: (utf8 hash, atlas px, ligature arm) → atlas-space advance
// width plus the run's cluster count. Widths are cached WITHOUT the spacing
// itself: letter-spacing is a per-request scalar applied as clusterCount x
// spacing on top of the cached width, so one entry serves every spacing of the
// same arm.
// Two arms, not one: non-zero spacing suppresses optional ligatures
// (Detail::LetterSpacingFeatures), which changes the glyph run and therefore
// both the cached width and the cluster count. The key carries that bit so a
// tracked measure can never be served the ligated width, or the reverse.
// Populated by the primary face's measure entry points; a repeat measure of the
// same (text, atlas px, arm) skips shaping. Internally locked (the cache
// outlives element destruction and serves per-word wrap measures); the critical
// section is a hash probe, orders of magnitude below the shaping it replaces.
struct ShapedWidthCache
{
    struct Entry
    {
        float WidthAtlas = 0.0f;
        uint32_t ClusterCount = 0; // typographic clusters in the shaped run
        uint64_t LastUse = 0;
    };
    std::mutex Mutex;
    std::unordered_map<uint64_t, Entry> Map;
    uint64_t UseCounter = 0;
};

// FNV-1a 64 over the text bytes, mixed with the atlas pixel size the width was
// shaped at and the ligature arm it was shaped in.
inline uint64_t ShapedWidthKey(std::string_view utf8, unsigned atlasPx, bool ligaturesSuppressed)
{
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : utf8)
    {
        h ^= c;
        h *= 1099511628211ull;
    }
    h ^= (uint64_t)atlasPx * 0x9E3779B97F4A7C15ull;
    if (ligaturesSuppressed)
        h ^= 0xD6E8FEB86659FD93ull;
    return h;
}

inline bool ShapedWidthLookup(ShapedWidthCache& cache, uint64_t key, float& outWidthAtlas,
                              uint32_t& outClusterCount)
{
    std::lock_guard<std::mutex> lock(cache.Mutex);
    auto it = cache.Map.find(key);
    if (it == cache.Map.end())
        return false;
    it->second.LastUse = ++cache.UseCounter;
    outWidthAtlas = it->second.WidthAtlas;
    outClusterCount = it->second.ClusterCount;
    return true;
}

inline void ShapedWidthInsert(ShapedWidthCache& cache, uint64_t key, float widthAtlas,
                              uint32_t clusterCount)
{
    constexpr size_t kCapacity = 4096;
    std::lock_guard<std::mutex> lock(cache.Mutex);
    cache.Map[key] = {widthAtlas, clusterCount, ++cache.UseCounter};
    if (cache.Map.size() <= kCapacity)
        return;
    // Evict the least-recently-used quarter in one sweep — amortizes the
    // O(n) scan instead of paying an ordering structure on every hit.
    std::vector<uint64_t> uses;
    uses.reserve(cache.Map.size());
    for (const auto& [k, e] : cache.Map)
        uses.push_back(e.LastUse);
    auto nth = uses.begin() + uses.size() / 4;
    std::nth_element(uses.begin(), nth, uses.end());
    const uint64_t cutoff = *nth;
    for (auto it = cache.Map.begin(); it != cache.Map.end();)
    {
        if (it->second.LastUse <= cutoff)
            it = cache.Map.erase(it);
        else
            ++it;
    }
}

// Whole-pixel size used only when allocating an atlas without an existing
// raster size. Layout geometry, advances and carets retain fractional pixels.
inline unsigned RasterPixelSize(float pixelSize)
{
    return static_cast<unsigned>(std::max(1.0f, pixelSize));
}

// Output of text shaping: glyph IDs and per-glyph positioning.
// Used internally by ShapeGlyphs(), consumed by ShapeText().
struct ShapingOutput
{
    std::vector<uint32_t> glyphIds;
    std::vector<float> xAdv;  // x advance per glyph (atlas-size pixels)
    std::vector<float> xOff;  // x offset per glyph (atlas-size pixels)
    std::vector<float> yOff;  // y offset per glyph (atlas-size pixels)
    bool isRTL = false;

    void Clear()
    {
        glyphIds.clear();
        xAdv.clear();
        xOff.clear();
        yOff.clear();
        isRTL = false;
    }

    void Reserve(size_t n)
    {
        glyphIds.reserve(n);
        xAdv.reserve(n);
        xOff.reserve(n);
        yOff.reserve(n);
    }
};

} // namespace GameEngine::Rendering::Text



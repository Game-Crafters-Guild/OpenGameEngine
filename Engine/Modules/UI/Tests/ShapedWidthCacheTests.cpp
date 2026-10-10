// Shaped-width LRU + font-reload identity — regression guard for the serial
// text measurement cache. FontAtlas's shaped-width cache serves repeated
// measures of the same (text, atlas px) without re-shaping, and LoadFontBytes
// must clear it so a reload cannot serve a stale width. Cached widths must be
// indistinguishable from freshly shaped ones, across repeats and across a
// reload.

#include <gtest/gtest.h>

#include "Rendering/Text/FontAtlas.h"
#include "RobotoTestFont.h"

#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering::Text;

namespace
{

constexpr unsigned kAtlasPx = UITesting::kUiAtlasPixelSize;

std::vector<uint8_t> LoadRobotoBytes()
{
    return UITesting::LoadStagedFontBytes("Roboto-Regular.ttf");
}

// Corpus spanning the shaping paths: plain ASCII, digits, RTL (Hebrew and
// Arabic take the HarfBuzz script/direction branch), combining and multi-byte
// codepoints, and whitespace-heavy strings.
const std::vector<std::string>& Corpus()
{
    static const std::vector<std::string> kCorpus = {
        "Hello, world!",
        "0123456789 3.14159",
        "The quick brown fox jumps over the lazy dog",
        "שלום עולם",              // Hebrew (RTL)
        "مرحبا بالعالم", // Arabic (RTL)
        "Mixed שלום and ASCII 42",
        "café naïve résumé",
        "中文測試",                                       // CJK
        "   leading and   internal   spaces ",
        "a",
        " ",
    };
    return kCorpus;
}

const std::vector<float>& PixelSizes()
{
    // Atlas size (steady state), plus off-size renders that exercise the
    // geometryScale path (the cache stores atlas-space widths; scale applies
    // after the lookup).
    static const std::vector<float> kSizes = {static_cast<float>(kAtlasPx), 14.0f, 33.0f};
    return kSizes;
}

} // namespace

// The shaped-width LRU serves repeated measures (second-and-later measures of
// the same text hit the cache) and is cleared by LoadFontBytes. Cached widths
// must be indistinguishable from freshly shaped ones — across repeats, at
// off-atlas render sizes, and across a reload.
TEST(ShapedWidthCache, StableAcrossRepeatsAndReload)
{
    const std::vector<uint8_t> fontBytes = LoadRobotoBytes();
    ASSERT_FALSE(fontBytes.empty())
        << "staged Roboto-Regular.ttf not found at " << UITesting::StagedFontPath("Roboto-Regular.ttf");
    FontAtlas atlas;
    ASSERT_TRUE(atlas.LoadFontBytes(fontBytes.data(), fontBytes.size(), kAtlasPx));

    for (const std::string& text : Corpus())
    {
        for (float px : PixelSizes())
        {
            SCOPED_TRACE("text=\"" + text + "\" px=" + std::to_string(px));
            const TextMetrics first = atlas.MeasureUtf8(text, px);
            // Repeat measure must hit the shaped-width cache and match exactly.
            const TextMetrics repeat = atlas.MeasureUtf8(text, px);
            EXPECT_EQ(first.width, repeat.width);
        }
    }

    // Reload clears the cache; re-measures recompute against the (identical)
    // new face and must land on the same widths.
    const std::string probe = Corpus().front();
    const float before = atlas.MeasureUtf8(probe, kAtlasPx).width;
    ASSERT_TRUE(atlas.LoadFontBytes(fontBytes.data(), fontBytes.size(), kAtlasPx));
    EXPECT_EQ(before, atlas.MeasureUtf8(probe, kAtlasPx).width);
}

// A same-bytes reload can't distinguish "cache cleared" from "stale entry
// served" (identical widths either way). Reload with a DIFFERENT font — the
// staged RobotoMono — and compare against a fresh atlas of that font as ground
// truth: a stale width from the old font fails deterministically.
TEST(ShapedWidthCache, ClearedByDifferentFontReload)
{
    const std::vector<uint8_t> fontA = LoadRobotoBytes();
    ASSERT_FALSE(fontA.empty())
        << "staged Roboto-Regular.ttf not found at " << UITesting::StagedFontPath("Roboto-Regular.ttf");
    const std::vector<uint8_t> fontB = UITesting::LoadStagedFontBytes("RobotoMono-Regular.ttf");
    ASSERT_FALSE(fontB.empty())
        << "staged RobotoMono-Regular.ttf not found at " << UITesting::StagedFontPath("RobotoMono-Regular.ttf");
    ASSERT_NE(fontA, fontB);

    FontAtlas atlas;
    ASSERT_TRUE(atlas.LoadFontBytes(fontA.data(), fontA.size(), kAtlasPx));
    const std::string probe = "The quick brown fox jumps over the lazy dog";
    (void)atlas.MeasureUtf8(probe, kAtlasPx); // populate the cache with font A's width

    ASSERT_TRUE(atlas.LoadFontBytes(fontB.data(), fontB.size(), kAtlasPx));
    FontAtlas groundTruth;
    ASSERT_TRUE(groundTruth.LoadFontBytes(fontB.data(), fontB.size(), kAtlasPx));
    EXPECT_EQ(groundTruth.MeasureUtf8(probe, kAtlasPx).width,
              atlas.MeasureUtf8(probe, kAtlasPx).width);
}

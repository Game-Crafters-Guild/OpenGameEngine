#include <gtest/gtest.h>

#include "Assets/TextureAsset.h"
#include "UI/NineSliceLayout.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::UI;

namespace
{
// Collect the emitted regions into a vector for assertions.
std::vector<NineSliceRegion> CollectRegions(const NineSlice& s, float imgW, float imgH,
                                            float dx, float dy, float dw, float dh, float scale)
{
    std::vector<NineSliceRegion> out;
    BuildNineSliceRegions(s, imgW, imgH, dx, dy, dw, dh, scale,
                          [&](const NineSliceRegion& r) { out.push_back(r); });
    return out;
}

// Find the first region whose top-left corner matches (x,y) within eps; -1 if none.
int FindRegionAt(const std::vector<NineSliceRegion>& r, float x, float y, float eps = 0.01f)
{
    for (size_t i = 0; i < r.size(); ++i)
        if (std::abs(r[i].X - x) < eps && std::abs(r[i].Y - y) < eps)
            return static_cast<int>(i);
    return -1;
}

NineSlice MakeShared(uint16 sw, uint16 sh, uint16 l, uint16 t, uint16 r, uint16 b)
{
    NineSlice s;
    s.Enabled = true;
    s.X[0] = l;              s.X[1] = l;
    s.X[2] = uint16(sw - r); s.X[3] = uint16(sw - r);
    s.Y[0] = t;              s.Y[1] = t;
    s.Y[2] = uint16(sh - b); s.Y[3] = uint16(sh - b);
    return s;
}
} // namespace

// ----- Metadata round-trip -------------------------------------------------

TEST(NineSliceMetaTests, RoundTripsThroughFormatAndParse)
{
    NineSlice in;
    in.Enabled = true;
    in.FillCenter = false;
    in.X[0] = 12; in.X[1] = 20; in.X[2] = 80; in.X[3] = 90;
    in.Y[0] = 4;  in.Y[1] = 4;  in.Y[2] = 60; in.Y[3] = 60;
    in.FillX = NineSliceFill::Tile;
    in.FillY = NineSliceFill::Round;
    in.CenterFill = NineSliceFill::Scale; // distinct from FillX/FillY so a token-slot swap is caught

    const std::string text = TextureNineSliceMetaValue(in);

    NineSlice out;
    ASSERT_TRUE(ParseTextureNineSliceMeta(text, out));
    EXPECT_TRUE(out.Enabled);
    EXPECT_FALSE(out.FillCenter);
    for (int i = 0; i < 4; ++i) { EXPECT_EQ(out.X[i], in.X[i]); EXPECT_EQ(out.Y[i], in.Y[i]); }
    EXPECT_EQ(out.FillX, NineSliceFill::Tile);
    EXPECT_EQ(out.FillY, NineSliceFill::Round);
    EXPECT_EQ(out.CenterFill, NineSliceFill::Scale);
}

TEST(NineSliceMetaTests, BackCompatParses9s1WithoutCenterFill)
{
    // A 9s1 string (13 tokens, no centerFill) must still parse, defaulting to Stretch.
    NineSlice out;
    ASSERT_TRUE(ParseTextureNineSliceMeta("9s1,1,10,10,90,90,10,10,90,90,tile,round,1", out));
    EXPECT_TRUE(out.Enabled);
    EXPECT_EQ(out.FillX, NineSliceFill::Tile);
    EXPECT_EQ(out.FillY, NineSliceFill::Round);
    EXPECT_EQ(out.CenterFill, NineSliceFill::Stretch);
}

TEST(NineSliceMetaTests, RejectsEmptyAndMalformed)
{
    NineSlice out;
    EXPECT_FALSE(ParseTextureNineSliceMeta("", out));
    EXPECT_FALSE(ParseTextureNineSliceMeta("garbage", out));
    EXPECT_FALSE(ParseTextureNineSliceMeta("9s1,1,10,10,90,90", out));        // too few tokens
    EXPECT_FALSE(ParseTextureNineSliceMeta("9s9,1,1,1,1,1,1,1,1,1,a,b,1", out)); // wrong schema id
}

TEST(NineSliceMetaTests, DisabledParsesButReportsFalse)
{
    NineSlice s = MakeShared(100, 100, 10, 10, 10, 10);
    s.Enabled = false;
    NineSlice out;
    EXPECT_FALSE(ParseTextureNineSliceMeta(TextureNineSliceMetaValue(s), out));
    EXPECT_FALSE(out.IsSliced());
    EXPECT_EQ(out.X[0], 10);
}

TEST(NineSliceMetaTests, ParseRepairsDecreasingCuts)
{
    NineSlice out;
    ASSERT_TRUE(ParseTextureNineSliceMeta("9s1,1,50,10,90,20,10,10,90,90,stretch,stretch,1", out));
    EXPECT_LE(out.X[0], out.X[1]);
    EXPECT_LE(out.X[1], out.X[2]);
    EXPECT_LE(out.X[2], out.X[3]);
}

// ----- Layout math (stretch) ----------------------------------------------

TEST(NineSliceLayoutTests, SharedBorders_NineRegions_CornersOneToOne)
{
    const NineSlice s = MakeShared(100, 100, 10, 10, 10, 10);
    const auto r = CollectRegions(s, 100, 100, 0, 0, 200, 200, 1.0f);

    EXPECT_EQ(r.size(), 9u);

    const int tl = FindRegionAt(r, 0, 0);
    ASSERT_GE(tl, 0);
    EXPECT_FLOAT_EQ(r[tl].W, 10.0f);
    EXPECT_FLOAT_EQ(r[tl].H, 10.0f);
    EXPECT_NEAR(r[tl].U0, 0.0f, 1e-5f);
    EXPECT_NEAR(r[tl].U1, 0.10f, 1e-5f);

    const int br = FindRegionAt(r, 190, 190);
    ASSERT_GE(br, 0);
    EXPECT_FLOAT_EQ(r[br].W, 10.0f);
    EXPECT_NEAR(r[br].U1, 1.0f, 1e-5f);

    const int c = FindRegionAt(r, 10, 10);
    ASSERT_GE(c, 0);
    EXPECT_FLOAT_EQ(r[c].W, 180.0f);
    EXPECT_FLOAT_EQ(r[c].H, 180.0f);
    EXPECT_NEAR(r[c].U0, 0.10f, 1e-5f);
    EXPECT_NEAR(r[c].U1, 0.90f, 1e-5f);
}

TEST(NineSliceLayoutTests, SharedBorders_NoPixelsLostAtCuts)
{
    const NineSlice s = MakeShared(100, 100, 10, 10, 10, 10);
    const auto r = CollectRegions(s, 100, 100, 0, 0, 200, 200, 1.0f);

    const int tl = FindRegionAt(r, 0, 0);
    const int c = FindRegionAt(r, 10, 10);
    const int br = FindRegionAt(r, 190, 190);
    ASSERT_GE(tl, 0); ASSERT_GE(c, 0); ASSERT_GE(br, 0);

    EXPECT_NEAR(r[tl].U1, r[c].U0, 1e-6f);
    EXPECT_NEAR(r[c].U1, r[br].U0, 1e-6f);
    EXPECT_NEAR(r[tl].V1, r[c].V0, 1e-6f);
    EXPECT_NEAR(r[c].V1, r[br].V0, 1e-6f);

    EXPECT_FLOAT_EQ(r[tl].W + r[c].W + r[br].W, 200.0f);
    EXPECT_FLOAT_EQ(r[tl].H + r[c].H + r[br].H, 200.0f);
}

TEST(NineSliceLayoutTests, ContentScaleScalesCornersNotCenter)
{
    const NineSlice s = MakeShared(100, 100, 10, 10, 10, 10);
    const auto r = CollectRegions(s, 100, 100, 0, 0, 200, 200, 2.0f);

    const int tl = FindRegionAt(r, 0, 0);
    ASSERT_GE(tl, 0);
    EXPECT_FLOAT_EQ(r[tl].W, 20.0f); // 10 texels * 2.0 scale
    EXPECT_FLOAT_EQ(r[tl].H, 20.0f);

    const int c = FindRegionAt(r, 20, 20);
    ASSERT_GE(c, 0);
    EXPECT_FLOAT_EQ(r[c].W, 160.0f); // 200 - 20 - 20
}

TEST(NineSliceLayoutTests, FillCenterFalse_OmitsCenter)
{
    NineSlice s = MakeShared(100, 100, 10, 10, 10, 10);
    s.FillCenter = false;
    const auto r = CollectRegions(s, 100, 100, 0, 0, 200, 200, 1.0f);

    EXPECT_EQ(r.size(), 8u);
    EXPECT_LT(FindRegionAt(r, 10, 10), 0);
}

TEST(NineSliceLayoutTests, IndependentCuts_DropGapBands)
{
    NineSlice s;
    s.Enabled = true;
    s.X[0] = 10; s.X[1] = 20; s.X[2] = 80; s.X[3] = 90;
    s.Y[0] = 10; s.Y[1] = 10; s.Y[2] = 90; s.Y[3] = 90; // Y shared
    const auto r = CollectRegions(s, 100, 100, 0, 0, 200, 200, 1.0f);

    EXPECT_EQ(r.size(), 9u);
    const int tl = FindRegionAt(r, 0, 0);
    const int c = FindRegionAt(r, 10, 10);
    ASSERT_GE(tl, 0); ASSERT_GE(c, 0);

    EXPECT_NEAR(r[tl].U1, 0.10f, 1e-5f);
    EXPECT_NEAR(r[c].U0, 0.20f, 1e-5f);
    EXPECT_NEAR(r[c].U1, 0.80f, 1e-5f);
}

TEST(NineSliceLayoutTests, OversizedBorders_ClampToBox_NoNegativeRegions)
{
    const NineSlice s = MakeShared(100, 100, 40, 40, 40, 40);
    const auto r = CollectRegions(s, 100, 100, 0, 0, 50, 50, 1.0f);

    EXPECT_EQ(r.size(), 4u); // only the four corners survive
    for (const auto& reg : r)
    {
        EXPECT_GT(reg.W, 0.0f);
        EXPECT_GT(reg.H, 0.0f);
        EXPECT_LE(reg.X + reg.W, 50.0f + 1e-3f);
        EXPECT_LE(reg.Y + reg.H, 50.0f + 1e-3f);
    }
    EXPECT_GE(FindRegionAt(r, 0, 0), 0);
    EXPECT_GE(FindRegionAt(r, 25, 25), 0);
}

// ----- Tile / round fill ---------------------------------------------------

TEST(NineSliceLayoutTests, FillX_TilesEdgesNotCenter)
{
    // FillX tiles the top/bottom EDGES; the center follows CenterFill (default
    // Stretch). 20px borders, 60-texel center band, wide box (300) -> edges tile
    // 5x horizontally (4 full 60px tiles + a 20px partial); center stays one span.
    NineSlice s = MakeShared(100, 100, 20, 20, 20, 20);
    s.FillX = NineSliceFill::Tile;
    s.FillY = NineSliceFill::Stretch;
    const auto r = CollectRegions(s, 100, 100, 0, 0, 300, 100, 1.0f);

    // 4 corners + top(5) + bottom(5) + center(1) + left(1) + right(1).
    EXPECT_EQ(r.size(), 17u);

    const int tl = FindRegionAt(r, 0, 0);
    ASSERT_GE(tl, 0);
    EXPECT_FLOAT_EQ(r[tl].W, 20.0f);

    // First top-edge tile: one tile wide (60), half-texel-inset UV.
    const int t0 = FindRegionAt(r, 20, 0);
    ASSERT_GE(t0, 0);
    EXPECT_FLOAT_EQ(r[t0].W, 60.0f);
    EXPECT_NEAR(r[t0].U0, 0.20f + 0.5f / 100.0f, 1e-5f);

    // Center is a single stretched span (not tiled).
    const int c = FindRegionAt(r, 20, 20);
    ASSERT_GE(c, 0);
    EXPECT_FLOAT_EQ(r[c].W, 260.0f);
}

TEST(NineSliceLayoutTests, RoundX_SnapsEdgeToWholeTiles)
{
    NineSlice s = MakeShared(100, 100, 20, 20, 20, 20);
    s.FillX = NineSliceFill::Round;
    s.FillY = NineSliceFill::Stretch;
    const auto r = CollectRegions(s, 100, 100, 0, 0, 300, 100, 1.0f);

    // 4 corners + top(4) + bottom(4) + center(1) + left(1) + right(1).
    EXPECT_EQ(r.size(), 15u);

    std::vector<float> topWidths;
    for (const auto& reg : r)
        if (std::abs(reg.Y - 0.0f) < 0.01f && reg.X >= 20.0f - 0.01f && reg.X + reg.W <= 280.0f + 0.01f)
            topWidths.push_back(reg.W);
    ASSERT_EQ(topWidths.size(), 4u);
    for (float wdt : topWidths)
        EXPECT_NEAR(wdt, 65.0f, 1e-3f);
}

TEST(NineSliceLayoutTests, CenterFill_Tile_TilesCenterBothAxes)
{
    // CenterFill tiles the center independently of the (stretched) edges.
    NineSlice s = MakeShared(100, 100, 20, 20, 20, 20);
    s.CenterFill = NineSliceFill::Tile; // FillX/FillY default Stretch
    const auto r = CollectRegions(s, 100, 100, 0, 0, 300, 300, 1.0f);

    // Center 260x260 / 60px tiles -> 5x5 = 25; edges 1 each; corners 4. Total 33.
    EXPECT_EQ(r.size(), 33u);

    int centerCount = 0;
    for (const auto& reg : r)
        if (reg.X >= 20.0f - 0.01f && reg.X + reg.W <= 280.0f + 0.01f &&
            reg.Y >= 20.0f - 0.01f && reg.Y + reg.H <= 280.0f + 0.01f)
            ++centerCount;
    EXPECT_EQ(centerCount, 25);
}

TEST(NineSliceLayoutTests, CenterFill_Scale_FitsAndCenters)
{
    NineSlice s = MakeShared(100, 100, 20, 20, 20, 20);
    s.CenterFill = NineSliceFill::Scale;
    const auto r = CollectRegions(s, 100, 100, 0, 0, 300, 100, 1.0f);

    // 4 corners + 4 edges + 1 scaled center.
    EXPECT_EQ(r.size(), 9u);

    // Center band is 60x60 (aspect 1) fit into the 260x60 center box by height,
    // centered -> a 60x60 quad at x = 20 + (260-60)/2 = 120, y = 20.
    const int c = FindRegionAt(r, 120, 20);
    ASSERT_GE(c, 0);
    EXPECT_NEAR(r[c].W, 60.0f, 1e-3f);
    EXPECT_NEAR(r[c].H, 60.0f, 1e-3f);
}

TEST(NineSliceLayoutTests, EdgeScaleDegradesToStretch)
{
    // Scale is a center-only mode; on the edges it collapses to a single stretched
    // span, identical to Stretch.
    const NineSlice base = MakeShared(100, 100, 20, 20, 20, 20);
    NineSlice scaled = base;
    scaled.FillX = NineSliceFill::Scale;
    scaled.FillY = NineSliceFill::Scale;

    const auto a = CollectRegions(base, 100, 100, 0, 0, 300, 300, 1.0f);
    const auto b = CollectRegions(scaled, 100, 100, 0, 0, 300, 300, 1.0f);
    EXPECT_EQ(a.size(), 9u);
    EXPECT_EQ(b.size(), a.size());
}

TEST(NineSliceLayoutTests, CenterTilingIsBoundedOnTinyBandLargeBox)
{
    // A 2-texel center band tiled across a 4000px box would naively emit 128x128;
    // the center per-axis cap keeps it bounded (32x32 = 1024 + corners/edges).
    NineSlice s = MakeShared(100, 100, 49, 49, 49, 49); // center band = 100-49-49 = 2 texels
    s.CenterFill = NineSliceFill::Tile;
    s.FillX = NineSliceFill::Stretch;
    s.FillY = NineSliceFill::Stretch;
    const auto r = CollectRegions(s, 100, 100, 0, 0, 4000, 4000, 1.0f);
    EXPECT_LE(r.size(), 1024u + 9u);
    EXPECT_GT(r.size(), 100u); // it did tile, just bounded
}

TEST(NineSliceLayoutTests, TileX_PartialLastTile_ContiguousWithClippedUV)
{
    NineSlice s = MakeShared(100, 100, 20, 20, 20, 20);
    s.FillX = NineSliceFill::Tile;
    s.FillY = NineSliceFill::Stretch;
    const auto r = CollectRegions(s, 100, 100, 0, 0, 300, 100, 1.0f);

    // Top-edge tiles span the center column [20,280] at y=0; 260/60 -> 4 full + 1 partial.
    std::vector<NineSliceRegion> top;
    for (const auto& reg : r)
        if (std::abs(reg.Y) < 0.01f && reg.X >= 20.0f - 0.01f && reg.X + reg.W <= 280.0f + 0.01f)
            top.push_back(reg);
    std::sort(top.begin(), top.end(), [](const NineSliceRegion& l, const NineSliceRegion& rr) { return l.X < rr.X; });
    ASSERT_EQ(top.size(), 5u);

    // Contiguous in dest: no gap or overlap between consecutive tiles.
    for (size_t i = 0; i + 1 < top.size(); ++i)
        EXPECT_NEAR(top[i].X + top[i].W, top[i + 1].X, 1e-3f);

    // Last tile is the 20px partial; its source UV span is clipped to 20/60 of a full tile.
    EXPECT_NEAR(top.back().W, 20.0f, 1e-3f);
    const float fullSpan = top.front().U1 - top.front().U0;
    EXPECT_NEAR(top.back().U1 - top.back().U0, fullSpan * (20.0f / 60.0f), 1e-4f);
}

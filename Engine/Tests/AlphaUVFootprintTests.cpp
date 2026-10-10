// Tests for AlphaUVFootprint: the conservative UV-footprint opacity classifier
// behind FBX inferred-Mask demotion. The safety property under test: a
// footprint is only reported opaque when every texel it can influence
// (dilated, wrap-aware) is opaque — real cutouts must stay Mask.

#include <gtest/gtest.h>

#include "Assets/AlphaUVFootprint.h"

#include <cmath>
#include <limits>
#include <vector>

using namespace GameEngine;

namespace {

constexpr uint32 kSize = 64;
constexpr uint8 kThreshold = 136;      // ceil(0.5 * 255) + 8: importer derivation for the default cutoff
constexpr float kDilation = 4.0f;

struct PlaneData {
    std::vector<uint8> Texels;
    AlphaPlaneView View;
};

PlaneData MakePlane(uint8 fill = 255)
{
    PlaneData plane;
    plane.Texels.assign(static_cast<size_t>(kSize) * kSize, fill);
    plane.View = AlphaPlaneView{plane.Texels.data(), kSize, kSize, 1};
    return plane;
}

void CarveHole(PlaneData& plane, uint32 x0, uint32 y0, uint32 x1, uint32 y1, uint8 value = 0)
{
    for (uint32 y = y0; y < y1; ++y)
        for (uint32 x = x0; x < x1; ++x)
            plane.Texels[static_cast<size_t>(y) * kSize + x] = value;
}

// Triangle over the texel-space rectangle [x0,x1]x[y0,y1] expressed in UVs.
struct Tri {
    float A[2], B[2], C[2];
};

Tri TexelRectTri(float x0, float y0, float x1, float y1)
{
    const float s = static_cast<float>(kSize);
    return Tri{{x0 / s, y0 / s}, {x1 / s, y0 / s}, {x0 / s, y1 / s}};
}

bool Opaque(const AlphaPlaneView& view, const Tri& t,
            uint8 threshold = kThreshold, float dilation = kDilation)
{
    return TriangleFootprintIsOpaque(view, t.A, t.B, t.C, threshold, dilation,
                                     PlaneMinAlpha(view));
}

} // namespace

// --- PlaneMinAlpha ---

TEST(AlphaUVFootprintTest, PlaneMinAlpha_AllOpaque)
{
    PlaneData plane = MakePlane();
    EXPECT_EQ(PlaneMinAlpha(plane.View), 255);
}

TEST(AlphaUVFootprintTest, PlaneMinAlpha_SingleCutoutTexelWins)
{
    PlaneData plane = MakePlane();
    CarveHole(plane, 63, 63, 64, 64, 17);
    EXPECT_EQ(PlaneMinAlpha(plane.View), 17);
}

TEST(AlphaUVFootprintTest, PlaneMinAlpha_EmptyPlaneIsZero)
{
    // 0 keeps the whole-plane comparison conservative for every threshold > 0.
    EXPECT_EQ(PlaneMinAlpha(AlphaPlaneView{}), 0);
}

TEST(AlphaUVFootprintTest, PlaneMinAlpha_ThresholdComparisonIsInclusive)
{
    PlaneData plane = MakePlane(250);
    EXPECT_EQ(PlaneMinAlpha(plane.View), 250);
    // The whole-plane predicate is minAlpha >= threshold.
    EXPECT_TRUE(Opaque(plane.View, TexelRectTri(10, 10, 50, 50), 250));
    EXPECT_FALSE(Opaque(plane.View, TexelRectTri(10, 10, 50, 50), 251));
}

// --- Core classification ---

TEST(AlphaUVFootprintTest, OpaqueRegionClassifiesOpaque)
{
    PlaneData plane = MakePlane();
    EXPECT_TRUE(Opaque(plane.View, TexelRectTri(10, 10, 50, 50)));
}

TEST(AlphaUVFootprintTest, TriangleOverCutoutStaysMask)
{
    PlaneData plane = MakePlane();
    CarveHole(plane, 8, 8, 16, 16);
    EXPECT_FALSE(Opaque(plane.View, TexelRectTri(6, 6, 20, 20)));
}

TEST(AlphaUVFootprintTest, BoundaryStraddlingTriangleStaysMask)
{
    PlaneData plane = MakePlane();
    CarveHole(plane, 8, 8, 16, 16);
    // Overlaps the hole's right edge only.
    EXPECT_FALSE(Opaque(plane.View, TexelRectTri(14, 9, 30, 15)));
}

TEST(AlphaUVFootprintTest, TriangleInsideDilationMarginStaysMask)
{
    PlaneData plane = MakePlane();
    CarveHole(plane, 8, 8, 16, 16);
    // Nearest hole texel center x=15.5; triangle starts at x=18 → 2.5 texels,
    // within the 4-texel dilation margin.
    EXPECT_FALSE(Opaque(plane.View, TexelRectTri(18, 8, 26, 16)));
}

TEST(AlphaUVFootprintTest, TriangleBeyondDilationMarginIsOpaque)
{
    PlaneData plane = MakePlane();
    CarveHole(plane, 8, 8, 16, 16);
    // Nearest hole texel center x=15.5; triangle starts at x=24 → 8.5 texels,
    // beyond dilation (4) + cell half-diagonal.
    EXPECT_TRUE(Opaque(plane.View, TexelRectTri(24, 8, 40, 16)));
}

TEST(AlphaUVFootprintTest, PartialAlphaBelowThresholdStaysMask)
{
    PlaneData plane = MakePlane();
    CarveHole(plane, 30, 30, 34, 34, 120); // below threshold 137, not a 0/255 cutout
    EXPECT_FALSE(Opaque(plane.View, TexelRectTri(28, 28, 40, 40)));
}

// --- Repeat addressing ---

TEST(AlphaUVFootprintTest, WrappedTriangleReachesCutoutAcrossSeam)
{
    PlaneData plane = MakePlane();
    CarveHole(plane, 0, 28, 4, 36);
    // u spans [0.95, 1.05]: the wrap reaches columns 0..3 where the hole is.
    Tri t{{0.95f, 0.45f}, {1.05f, 0.45f}, {0.95f, 0.55f}};
    EXPECT_FALSE(Opaque(plane.View, t));
}

TEST(AlphaUVFootprintTest, WrappedTriangleOverOpaqueSeamIsOpaque)
{
    PlaneData plane = MakePlane();
    CarveHole(plane, 28, 28, 36, 36);
    // Same seam-spanning shape, but the wrapped columns are opaque.
    Tri t{{0.95f, 0.05f}, {1.05f, 0.05f}, {0.95f, 0.15f}};
    EXPECT_TRUE(Opaque(plane.View, t));
}

TEST(AlphaUVFootprintTest, NegativeUVsWrap)
{
    PlaneData plane = MakePlane();
    CarveHole(plane, 56, 8, 64, 16);
    // u in [-0.1, -0.05] wraps to columns ~57..60 — inside the hole.
    Tri t{{-0.1f, 0.13f}, {-0.05f, 0.13f}, {-0.1f, 0.2f}};
    EXPECT_FALSE(Opaque(plane.View, t));
}

// --- Tiling fallback ---

TEST(AlphaUVFootprintTest, TilingTriangleTestsWholePlane)
{
    PlaneData holed = MakePlane();
    CarveHole(holed, 40, 40, 44, 44);
    Tri tiling{{0.0f, 0.0f}, {3.2f, 0.0f}, {0.0f, 3.2f}};
    EXPECT_FALSE(Opaque(holed.View, tiling));

    PlaneData clean = MakePlane();
    EXPECT_TRUE(Opaque(clean.View, tiling));
}

// --- Degenerate geometry ---

TEST(AlphaUVFootprintTest, DegeneratePointOverCutoutStaysMask)
{
    PlaneData plane = MakePlane();
    CarveHole(plane, 8, 8, 16, 16);
    Tri t = TexelRectTri(12, 12, 12, 12); // all three corners coincide
    t.B[0] = t.A[0];
    t.B[1] = t.A[1];
    t.C[0] = t.A[0];
    t.C[1] = t.A[1];
    EXPECT_FALSE(Opaque(plane.View, t));
}

TEST(AlphaUVFootprintTest, DegeneratePointInOpaqueRegionIsOpaque)
{
    PlaneData plane = MakePlane();
    CarveHole(plane, 8, 8, 16, 16);
    Tri t{{0.7f, 0.7f}, {0.7f, 0.7f}, {0.7f, 0.7f}};
    EXPECT_TRUE(Opaque(plane.View, t));
}

// --- Robustness ---

TEST(AlphaUVFootprintTest, NonFiniteUVFallsBackToWholePlane)
{
    PlaneData holed = MakePlane();
    CarveHole(holed, 0, 0, 1, 1);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    Tri t{{nan, 0.5f}, {0.6f, 0.5f}, {0.5f, 0.6f}};
    EXPECT_FALSE(Opaque(holed.View, t));

    PlaneData clean = MakePlane();
    EXPECT_TRUE(Opaque(clean.View, t));
}

TEST(AlphaUVFootprintTest, HugePixelSpaceUVFallsBackToWholePlane)
{
    // A pixel-space bbox beyond the int64-safe walking range previously hit
    // UB float->int64 casts (and for spans past ~2^40 texels the uint64 span
    // product wrapped, defeating the raster cap). Such UVs must take the
    // whole-plane fallback and complete immediately.
    PlaneData holed = MakePlane();
    CarveHole(holed, 0, 0, 1, 1);
    Tri t{{-1.0e30f, -1.0e30f}, {1.0e30f, 0.5f}, {0.5f, 1.0e30f}};
    EXPECT_FALSE(Opaque(holed.View, t));

    PlaneData clean = MakePlane();
    EXPECT_TRUE(Opaque(clean.View, t));
}

TEST(AlphaUVFootprintTest, SmallTriangleAtHugeUVOffsetFallsBackToWholePlane)
{
    // A tiny triangle tiled absurdly far away exceeds the coordinate clamp
    // even though its span is small; it too resolves via the whole plane.
    PlaneData holed = MakePlane();
    CarveHole(holed, 40, 40, 41, 41);
    Tri t{{1.0e12f, 1.0e12f}, {1.0e12f, 1.0e12f}, {1.0e12f, 1.0e12f}};
    EXPECT_FALSE(Opaque(holed.View, t));

    PlaneData clean = MakePlane();
    EXPECT_TRUE(Opaque(clean.View, t));
}

TEST(AlphaUVFootprintTest, InterleavedStrideReadsAlphaChannel)
{
    // RGBA8-style interleaved data: RGB bytes are garbage (0), alpha is 255
    // except one cutout texel.
    std::vector<uint8> rgba(static_cast<size_t>(kSize) * kSize * 4, 0);
    for (size_t i = 0; i < static_cast<size_t>(kSize) * kSize; ++i)
        rgba[i * 4 + 3] = 255;
    const size_t holeTexel = 20 * kSize + 20;
    rgba[holeTexel * 4 + 3] = 0;

    AlphaPlaneView view{rgba.data() + 3, kSize, kSize, 4};
    EXPECT_FALSE(Opaque(view, TexelRectTri(18, 18, 24, 24)));
    EXPECT_TRUE(Opaque(view, TexelRectTri(40, 40, 50, 50)));
}

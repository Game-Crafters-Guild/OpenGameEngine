// The end taper: how an open run stops.
//
// Two contracts carry the weight here and they pull opposite ways. Off must be
// EXACTLY off — a run that authors no taper has to sweep the same vertices it
// always did, down to the bit, or every existing scene silently re-textures.
// On must actually reach nothing — a pinch that stops at 20% of the width is a
// smaller square cut, not a point.

#include "SplineGeometry/SplineEndTaper.h"
#include "SplineGeometry/SplineProfile.h"
#include "SplineGeometry/SplineStripBuilder.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::SplineGeometry;

namespace
{

using V3 = Mathematics::Vector3;

// A straight run along +Z, one station every `step` metres, both sides at
// `halfWidth`. Distance is the walked length, which is what the taper measures
// its span on.
std::vector<SplineStripStation> StraightStations(uint32 count, float32 step, float32 halfWidth)
{
    std::vector<SplineStripStation> stations(count);
    for (uint32 i = 0; i < count; ++i)
    {
        const float32 z = step * static_cast<float32>(i);
        SplineStripStation& s = stations[i];
        s.Position = V3{0.0f, 5.0f, z};
        s.Forward = V3{0.0f, 0.0f, 1.0f};
        s.Right = V3{1.0f, 0.0f, 0.0f};
        s.Up = V3{0.0f, 1.0f, 0.0f};
        s.HalfWidthLeft = halfWidth;
        s.HalfWidthRight = halfWidth;
        s.Distance = z;
    }
    return stations;
}

SplineProfile FlatRibbon(float32 width)
{
    SplineProfileParams params;
    params.Shape = SplineProfileShape::Bevel;
    params.Width = width;
    params.EdgeDrop = 0.0f;
    params.EdgeInset = 0.0f;
    return BuildProfile(params);
}

bool BitsEqual(float32 a, float32 b)
{
    return std::memcmp(&a, &b, sizeof(float32)) == 0;
}

// Every float a station carries, compared by bits. A tolerance compare would
// pass on a taper that moved the last mantissa bit of every vertex, which is
// exactly the change "off" must not make.
void ExpectStationsBitIdentical(std::span<const SplineStripStation> actual,
                                std::span<const SplineStripStation> expected,
                                const char* what)
{
    ASSERT_EQ(actual.size(), expected.size()) << what;
    for (size_t i = 0; i < actual.size(); ++i)
    {
        EXPECT_TRUE(BitsEqual(actual[i].HalfWidthLeft, expected[i].HalfWidthLeft))
            << what << ": station " << i << " left half-width moved";
        EXPECT_TRUE(BitsEqual(actual[i].HalfWidthRight, expected[i].HalfWidthRight))
            << what << ": station " << i << " right half-width moved";
        EXPECT_TRUE(BitsEqual(actual[i].Position.x, expected[i].Position.x))
            << what << ": station " << i << " x moved";
        EXPECT_TRUE(BitsEqual(actual[i].Position.y, expected[i].Position.y))
            << what << ": station " << i << " y moved";
        EXPECT_TRUE(BitsEqual(actual[i].Position.z, expected[i].Position.z))
            << what << ": station " << i << " z moved";
        EXPECT_TRUE(BitsEqual(actual[i].Distance, expected[i].Distance))
            << what << ": station " << i << " distance moved";
    }
}

} // namespace

// The invariant that protects every scene authored before the taper existed,
// stated as bits rather than as a tolerance: an off taper is not a scale of one
// applied cheaply, it is no arithmetic at all.
TEST(SplineEndTaper, AZeroTaperLeavesEveryStationBitIdentical)
{
    // Widths and a height that are not powers of two, so a multiply-by-one or a
    // subtract-of-zero that rounds would have somewhere to show up.
    const std::vector<SplineStripStation> reference = StraightStations(12u, 2.3f, 3.7f);
    std::vector<SplineStripStation> stations = reference;

    SplineEndTaperParams params;
    params.TaperMetres = 0.0f;
    params.HeightDrop = 0.45f;
    ApplyEndTaper(stations, params);

    ExpectStationsBitIdentical(stations, reference, "zero taper");
}

// The same invariant one level down, where it is actually spent: the swept
// vertices themselves. This is the compare the Channel-mode bit test makes for
// per-side widths, applied to the taper.
TEST(SplineEndTaper, AZeroTaperSweepsTheSameVerticesBitForBit)
{
    const SplineProfile profile = FlatRibbon(4.5f);
    ASSERT_TRUE(profile.IsValid());

    const std::vector<SplineStripStation> untouched = StraightStations(12u, 2.3f, 3.7f);
    std::vector<SplineStripStation> tapered = untouched;

    SplineEndTaperParams params;
    params.TaperMetres = 0.0f;
    params.HeightDrop = 0.45f;
    ApplyEndTaper(tapered, params);

    SplineStripParams stripParams;
    stripParams.WidthScale = SplineProfileScale::LateralOnly;
    stripParams.UOriginMetres = 0.0f;
    const SplineStripMesh expected = BuildSplineStrip(profile, untouched, stripParams);
    const SplineStripMesh actual = BuildSplineStrip(profile, tapered, stripParams);

    ASSERT_TRUE(expected.IsValid());
    ASSERT_EQ(actual.Vertices.size(), expected.Vertices.size());
    ASSERT_EQ(actual.Indices, expected.Indices);
    for (size_t i = 0; i < expected.Vertices.size(); ++i)
    {
        EXPECT_TRUE(BitsEqual(actual.Vertices[i].Position.x, expected.Vertices[i].Position.x))
            << "vertex " << i << " x";
        EXPECT_TRUE(BitsEqual(actual.Vertices[i].Position.y, expected.Vertices[i].Position.y))
            << "vertex " << i << " y";
        EXPECT_TRUE(BitsEqual(actual.Vertices[i].Position.z, expected.Vertices[i].Position.z))
            << "vertex " << i << " z";
        EXPECT_TRUE(BitsEqual(actual.Vertices[i].UV.x, expected.Vertices[i].UV.x))
            << "vertex " << i << " u";
        EXPECT_TRUE(BitsEqual(actual.Vertices[i].UV.y, expected.Vertices[i].UV.y))
            << "vertex " << i << " v";
    }
}

// The point the whole feature exists for: the run must actually reach nothing.
// A pinch that stops short is a narrower square cut.
TEST(SplineEndTaper, BothOpenEndsPinchToNothing)
{
    constexpr float32 kHalfWidth = 3.0f;
    std::vector<SplineStripStation> stations = StraightStations(41u, 1.0f, kHalfWidth);

    SplineEndTaperParams params;
    params.TaperMetres = 8.0f;
    ApplyEndTaper(stations, params);

    EXPECT_FLOAT_EQ(stations.front().HalfWidthLeft, 0.0f);
    EXPECT_FLOAT_EQ(stations.front().HalfWidthRight, 0.0f);
    EXPECT_FLOAT_EQ(stations.back().HalfWidthLeft, 0.0f);
    EXPECT_FLOAT_EQ(stations.back().HalfWidthRight, 0.0f);

    // The middle of a 40 m run is nowhere near either 8 m taper.
    EXPECT_FLOAT_EQ(stations[20].HalfWidthLeft, kHalfWidth);
    EXPECT_FLOAT_EQ(stations[20].HalfWidthRight, kHalfWidth);
}

// Monotone in, monotone out. A pinch that widened anywhere along its span would
// read as a bulge at the river mouth, and a smoothstep evaluated on the wrong
// variable is exactly the bug that produces one.
TEST(SplineEndTaper, TheTaperNarrowsMonotonicallyIntoEachEnd)
{
    constexpr float32 kHalfWidth = 3.0f;
    constexpr float32 kTaper = 8.0f;
    std::vector<SplineStripStation> stations = StraightStations(41u, 1.0f, kHalfWidth);

    SplineEndTaperParams params;
    params.TaperMetres = kTaper;
    ApplyEndTaper(stations, params);

    for (size_t i = 0; i + 1u < stations.size(); ++i)
    {
        const float32 distance = stations[i].Distance;
        if (distance < kTaper)
        {
            EXPECT_LE(stations[i].HalfWidthLeft, stations[i + 1u].HalfWidthLeft)
                << "head taper widens backwards at station " << i;
        }
        const float32 fromBack = stations.back().Distance - stations[i + 1u].Distance;
        if (fromBack < kTaper)
        {
            EXPECT_GE(stations[i].HalfWidthLeft, stations[i + 1u].HalfWidthLeft)
                << "tail taper widens forwards at station " << i;
        }
    }

    // Every width stays inside the band it started in: no overshoot, no
    // negative side.
    for (const SplineStripStation& s : stations)
    {
        EXPECT_GE(s.HalfWidthLeft, 0.0f);
        EXPECT_LE(s.HalfWidthLeft, kHalfWidth);
    }
}

// Smooth, not linear: the join where the taper meets the untapered run must not
// be a crease. A linear ramp arrives at the boundary with the full slope of the
// taper; a smoothstep arrives with none of it.
TEST(SplineEndTaper, ThePinchMeetsTheRunWithoutACrease)
{
    constexpr float32 kHalfWidth = 3.0f;
    constexpr float32 kTaper = 8.0f;
    std::vector<SplineStripStation> stations = StraightStations(41u, 1.0f, kHalfWidth);

    SplineEndTaperParams params;
    params.TaperMetres = kTaper;
    ApplyEndTaper(stations, params);

    // The station one metre inside the boundary. A LINEAR taper would put it at
    // 7/8 of the width; a smoothstep leaves it far closer to full width, which
    // is the whole reason for the curve.
    const float32 linearWidth = kHalfWidth * (7.0f / 8.0f);
    EXPECT_GT(stations[7].HalfWidthLeft, linearWidth)
        << "the join is as steep as a linear ramp — smoothstep is what removes the crease";
    EXPECT_LT(stations[7].HalfWidthLeft, kHalfWidth);

    // And the first step off zero must be shallower than a linear ramp too, or
    // the point is a spike rather than a taper.
    EXPECT_LT(stations[1].HalfWidthLeft, kHalfWidth * (1.0f / 8.0f));
}

// The pinch tucks under the surface it came from, so the point is buried rather
// than left sitting on the waterline.
TEST(SplineEndTaper, ThePinchedRingSinksByTheHeightDrop)
{
    constexpr float32 kDrop = 0.35f;
    constexpr float32 kTaper = 8.0f;
    const std::vector<SplineStripStation> reference = StraightStations(41u, 1.0f, 3.0f);
    std::vector<SplineStripStation> stations = reference;

    SplineEndTaperParams params;
    params.TaperMetres = kTaper;
    params.HeightDrop = kDrop;
    ApplyEndTaper(stations, params);

    EXPECT_FLOAT_EQ(stations.front().Position.y, reference.front().Position.y - kDrop);
    EXPECT_FLOAT_EQ(stations.back().Position.y, reference.back().Position.y - kDrop);
    // Untapered stations keep their draped height exactly: the drop is a
    // property of the pinch, not a lift applied to the run.
    EXPECT_FLOAT_EQ(stations[20].Position.y, reference[20].Position.y);
    // The sink follows the pinch, so a half-pinched ring is half-sunk at most.
    EXPECT_GT(stations[4].Position.y, reference[4].Position.y - kDrop);
    EXPECT_LT(stations[4].Position.y, reference[4].Position.y);
}

// A closed loop has no end to taper: its first and last stations are the same
// place, and pinching them would cut the loop open at its seam.
TEST(SplineEndTaper, AClosedLoopIsLeftAlone)
{
    const std::vector<SplineStripStation> reference = StraightStations(12u, 2.3f, 3.7f);
    std::vector<SplineStripStation> stations = reference;

    SplineEndTaperParams params;
    params.TaperMetres = 8.0f;
    params.HeightDrop = 0.45f;
    params.ClosedLoop = true;
    ApplyEndTaper(stations, params);

    ExpectStationsBitIdentical(stations, reference, "closed loop");
}

// A run shorter than two tapers is not a special case in the code and must not
// become one in the geometry: each station takes the nearer end, so the two
// pinches meet instead of one of them winning.
TEST(SplineEndTaper, AShortRunLetsItsTwoPinchesMeet)
{
    constexpr float32 kHalfWidth = 3.0f;
    // 10 m of run against an 8 m taper per end.
    std::vector<SplineStripStation> stations = StraightStations(11u, 1.0f, kHalfWidth);

    SplineEndTaperParams params;
    params.TaperMetres = 8.0f;
    ApplyEndTaper(stations, params);

    EXPECT_FLOAT_EQ(stations.front().HalfWidthLeft, 0.0f);
    EXPECT_FLOAT_EQ(stations.back().HalfWidthLeft, 0.0f);
    for (const SplineStripStation& s : stations)
    {
        EXPECT_GE(s.HalfWidthLeft, 0.0f);
        EXPECT_LE(s.HalfWidthLeft, kHalfWidth);
    }
    // The widest ring is the middle one, and it never reaches full width
    // because neither pinch finished.
    EXPECT_GT(stations[5].HalfWidthLeft, stations[4].HalfWidthLeft);
    EXPECT_GT(stations[5].HalfWidthLeft, stations[6].HalfWidthLeft);
    EXPECT_LT(stations[5].HalfWidthLeft, kHalfWidth);
}

// An asymmetric channel is the shape the bank fit produces, and the taper must
// carry that asymmetry into the point rather than averaging the two sides on
// the way there.
TEST(SplineEndTaper, AnAsymmetricRunKeepsItsRatioThroughTheTaper)
{
    constexpr float32 kLeft = 2.0f;
    constexpr float32 kRight = 6.0f;
    std::vector<SplineStripStation> stations = StraightStations(41u, 1.0f, 1.0f);
    for (SplineStripStation& s : stations)
    {
        s.HalfWidthLeft = kLeft;
        s.HalfWidthRight = kRight;
    }

    SplineEndTaperParams params;
    params.TaperMetres = 8.0f;
    ApplyEndTaper(stations, params);

    for (size_t i = 1; i + 1u < stations.size(); ++i)
    {
        if (stations[i].HalfWidthLeft <= 0.0f)
            continue;
        EXPECT_NEAR(stations[i].HalfWidthRight / stations[i].HalfWidthLeft, kRight / kLeft, 1.0e-4f)
            << "station " << i << " lost the channel's asymmetry";
    }
}

// The taper reads Distance and nothing else, so a run whose stations are not
// laid out in metres of arc — or that carries a non-finite one — must not have
// its widths poisoned.
TEST(SplineEndTaper, ANonFiniteStationIsSkippedRatherThanScaledByANaN)
{
    std::vector<SplineStripStation> stations = StraightStations(41u, 1.0f, 3.0f);
    stations[3].Distance = std::numeric_limits<float32>::quiet_NaN();

    SplineEndTaperParams params;
    params.TaperMetres = 8.0f;
    ApplyEndTaper(stations, params);

    EXPECT_FLOAT_EQ(stations[3].HalfWidthLeft, 3.0f)
        << "an unplaceable station must keep its width, not take a NaN one";
    EXPECT_TRUE(std::isfinite(stations[3].HalfWidthLeft));
    // Its neighbours still taper: one bad station is not a bad run.
    EXPECT_LT(stations[2].HalfWidthLeft, 3.0f);
    EXPECT_LT(stations[4].HalfWidthLeft, 3.0f);
}

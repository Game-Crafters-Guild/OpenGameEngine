#include "Components/Spline/SplineStationJitter.h"
#include "SplineLayout/TileLayout.h"

#include "GoldenUlps.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace GameEngine;
using GameEngine::SplineLayout::BuildTilePoses;
using GameEngine::SplineLayout::CenterSample;
using GameEngine::SplineLayout::NormalizedOrFallback;
using GameEngine::SplineLayout::TileLayoutParams;
using GameEngine::SplineLayout::TilePose;
using GameEngine::SplineLayout::Tests::FloatBits;
using GameEngine::SplineLayout::Tests::kMaxGoldenUlps;
using GameEngine::SplineLayout::Tests::UlpDistance;
using V3 = GameEngine::Mathematics::Vector3;

namespace
{

// The scenario the pre-change golden below was captured from: curving in XZ,
// climbing in Y, with a tilted surface normal, so a golden over it is sensitive
// to the station walk, the chord yaw, the slope blend and the footprint
// centering alike.
std::vector<CenterSample> GoldenScenario()
{
    std::vector<CenterSample> center;
    constexpr int kSamples = 41;
    for (int i = 0; i < kSamples; ++i)
    {
        const float32 t = static_cast<float32>(i) / static_cast<float32>(kSamples - 1);
        CenterSample s;
        s.Pos = V3(4.0f * t * t, 1.5f * t, 20.0f * t);
        s.Normal = NormalizedOrFallback(V3(0.12f, 1.0f, -0.20f * t), V3(0.0f, 1.0f, 0.0f));
        center.push_back(s);
    }
    return center;
}

// The corner-pivoted, roughly square footprint of the measured kit tile.
TileLayoutParams GoldenParams()
{
    TileLayoutParams p;
    p.Spacing = 2.0f;
    p.Fit = Components::SplinePlacementFit::FitToLength;
    p.AlignToSurfaceNormal = true;
    p.SlopeBlend = 1.0f;
    p.MaxTiltDegrees = 35.0f;
    p.MeshBoundsCenter = V3(1.2f, 0.05f, 1.1f);
    p.MeshBoundsHalfExtents = V3(1.19f, 0.05f, 1.31f);
    p.PlantMode = Components::SplinePlantMode::BoundsMin;
    p.MaxTiles = 2048;
    return p;
}

// A flat straight run along +Z, for the tests that reason about distance.
std::vector<CenterSample> FlatLineZ(float32 length, uint32 sampleCount)
{
    std::vector<CenterSample> center;
    for (uint32 i = 0; i < sampleCount; ++i)
    {
        CenterSample s;
        s.Pos = V3(0.0f, 0.0f,
                   length * static_cast<float32>(i) / static_cast<float32>(sampleCount - 1));
        s.Normal = V3(0.0f, 1.0f, 0.0f);
        center.push_back(s);
    }
    return center;
}

TileLayoutParams FlatParams(float32 spacing)
{
    TileLayoutParams p;
    p.Spacing = spacing;
    p.Fit = Components::SplinePlacementFit::FitToLength;
    p.AlignToSurfaceNormal = false;
    p.MaxTiles = 2048;
    return p;
}

bool SameBits(const V3& a, const V3& b)
{
    return FloatBits(a.x) == FloatBits(b.x) && FloatBits(a.y) == FloatBits(b.y) &&
           FloatBits(a.z) == FloatBits(b.z);
}

bool PoseBitsEqual(const TilePose& a, const TilePose& b)
{
    return SameBits(a.Position, b.Position) && SameBits(a.Right, b.Right) &&
           SameBits(a.Up, b.Up) && SameBits(a.Forward, b.Forward) && SameBits(a.Base, b.Base) &&
           a.StationIndex == b.StationIndex;
}

} // namespace

// ---- The zero-behaviour-change contract ----

// The load-bearing test of this whole change: with every variation knob at its
// default, the layout must be what it was BEFORE the knobs existed — not
// "close", identical to the bit.
//
// These are not values this suite computed; they were captured from
// Apps/Editor/Source/Placement/TileLayout.cpp at f865d7f0f (the commit this
// branch forked from), compiled standalone against the same scenario. That is
// what makes the test non-circular: nothing in the new code produced them.
TEST(SplineStationJitter, DefaultKnobsReproduceThePreChangeLayoutExactly)
{
    constexpr uint32 kGoldenPositionBits[][3] = {
        {0xBF9A5E38, 0x3E14317D, 0xBF8AB9EA}, {0xBF98F922, 0x3E8FC41B, 0x3F8025EE},
        {0xBF8E65CD, 0x3ED726F6, 0x40467374}, {0xBF71B795, 0x3F0FA401, 0x40A63EF9},
        {0xBF310384, 0x3F3407AF, 0x40E8FBA1}, {0xBEB63DFE, 0x3F58B121, 0x4115A9DA},
        {0x3D767770, 0x3F7D91C9, 0x413696A7}, {0x3F0DE6A8, 0x3F914D2B, 0x4157387F},
        {0x3F8FCF9A, 0x3FA3DD88, 0x4177850D}, {0x3FE1E037, 0x3FB67220, 0x418BB9C1},
        {0x401EBB92, 0x3FC8827E, 0x419B5D40},
    };
    constexpr size_t kGoldenCount = sizeof(kGoldenPositionBits) / sizeof(kGoldenPositionBits[0]);

    const std::vector<TilePose> poses = BuildTilePoses(GoldenScenario(), GoldenParams());
    ASSERT_EQ(poses.size(), kGoldenCount)
        << "the default station count moved; the knobs are supposed to be inert by default";
    // Compared in ULPs, not bits (GoldenUlps.h): on arm64 four of these 33 values
    // land 1-8 ULP away with arithmetic that is textually identical to the
    // capture commit. The contract this test exists for -- "the knobs are inert
    // at their defaults" -- survives intact: a knob that leaked would move a
    // station by metres, which is billions of ULPs, not eight.
    auto expectNear = [&](uint32 actual, uint32 golden, size_t station, const char* axis) {
        EXPECT_LE(UlpDistance(actual, golden), kMaxGoldenUlps)
            << "station " << station << " " << axis << ": " << actual << " vs golden " << golden
            << " is beyond cross-architecture rounding, so the default layout really moved";
    };
    for (size_t i = 0; i < kGoldenCount; ++i)
    {
        expectNear(FloatBits(poses[i].Position.x), kGoldenPositionBits[i][0], i, "x");
        expectNear(FloatBits(poses[i].Position.y), kGoldenPositionBits[i][1], i, "y");
        expectNear(FloatBits(poses[i].Position.z), kGoldenPositionBits[i][2], i, "z");
    }
}

// The positive control for the golden above: the same comparison MUST be able
// to fail. Without this, a golden that silently stopped being evaluated (or a
// jitter path that never ran) would read as a pass forever.
TEST(SplineStationJitter, TheGoldenComparisonCanActuallyFail)
{
    TileLayoutParams jittered = GoldenParams();
    jittered.Seed = 1234u;
    jittered.SpacingJitterMetres = 0.6f;

    const std::vector<TilePose> base = BuildTilePoses(GoldenScenario(), GoldenParams());
    const std::vector<TilePose> moved = BuildTilePoses(GoldenScenario(), jittered);
    ASSERT_EQ(base.size(), moved.size()) << "spacing jitter must not change the station COUNT";

    const bool anyMoved =
        std::any_of(base.begin(), base.end(), [&](const TilePose& b)
                    { return !PoseBitsEqual(b, moved[static_cast<size_t>(&b - base.data())]); });
    EXPECT_TRUE(anyMoved) << "jitter changed nothing, so the golden test proves nothing";
}

// Every knob explicitly zeroed must be the same as every knob left default —
// i.e. zero is genuinely the identity, not merely the default.
TEST(SplineStationJitter, ExplicitZeroKnobsMatchTheDefaults)
{
    TileLayoutParams zeroed = GoldenParams();
    zeroed.Seed = 4242u; // a seed with nothing to salt must not matter either
    zeroed.SpacingJitterMetres = 0.0f;
    zeroed.YawJitterDegrees = 0.0f;
    zeroed.LateralJitterMetres = 0.0f;
    zeroed.DropoutChance = 0.0f;
    zeroed.EndTaperMetres = 0.0f;

    const std::vector<TilePose> defaults = BuildTilePoses(GoldenScenario(), GoldenParams());
    const std::vector<TilePose> explicitZeros = BuildTilePoses(GoldenScenario(), zeroed);
    ASSERT_EQ(defaults.size(), explicitZeros.size());
    for (size_t i = 0; i < defaults.size(); ++i)
        EXPECT_TRUE(PoseBitsEqual(defaults[i], explicitZeros[i])) << "station " << i;
}

// ---- Determinism ----

TEST(SplineStationJitter, SameSeedReproducesTheLayoutExactly)
{
    TileLayoutParams p = GoldenParams();
    p.Seed = 777u;
    p.SpacingJitterMetres = 0.5f;
    p.YawJitterDegrees = 15.0f;
    p.LateralJitterMetres = 0.3f;
    p.DropoutChance = 0.2f;
    p.EndTaperMetres = 4.0f;

    const std::vector<TilePose> first = BuildTilePoses(GoldenScenario(), p);
    const std::vector<TilePose> second = BuildTilePoses(GoldenScenario(), p);
    ASSERT_EQ(first.size(), second.size());
    ASSERT_FALSE(first.empty());
    for (size_t i = 0; i < first.size(); ++i)
        EXPECT_TRUE(PoseBitsEqual(first[i], second[i])) << "pose " << i;
}

TEST(SplineStationJitter, ADifferentSeedGivesADifferentLayout)
{
    TileLayoutParams a = GoldenParams();
    a.Seed = 1u;
    a.SpacingJitterMetres = 0.5f;
    a.LateralJitterMetres = 0.3f;
    TileLayoutParams b = a;
    b.Seed = 2u;

    const std::vector<TilePose> first = BuildTilePoses(GoldenScenario(), a);
    const std::vector<TilePose> second = BuildTilePoses(GoldenScenario(), b);
    ASSERT_EQ(first.size(), second.size());
    bool anyDiffer = false;
    for (size_t i = 0; i < first.size(); ++i)
        anyDiffer |= !PoseBitsEqual(first[i], second[i]);
    EXPECT_TRUE(anyDiffer) << "Seed does not reach the jitter";
}

// ---- Spacing jitter ----

// The ordering guarantee the half-spacing clamp exists to provide: a station
// may slide, but never past a neighbour. Reordering would hand tiles the wrong
// chord and flip their yaw.
TEST(SplineStationJitter, SpacingJitterNeverReordersStations)
{
    for (float32 asked : {0.5f, 1.0f, 5.0f, 1000.0f})
    {
        TileLayoutParams p = FlatParams(2.0f);
        p.Seed = 31u;
        p.SpacingJitterMetres = asked; // deliberately past the clamp
        const std::vector<TilePose> poses = BuildTilePoses(FlatLineZ(40.0f, 81u), p);
        ASSERT_GE(poses.size(), 2u);
        for (size_t i = 1; i < poses.size(); ++i)
        {
            EXPECT_GE(poses[i].Base.z, poses[i - 1].Base.z)
                << "asked " << asked << ": station " << i << " slid past its neighbour";
        }
    }
}

// The clamp itself: however much jitter is asked for, no station moves further
// than half the spacing from where the grid put it.
TEST(SplineStationJitter, SpacingJitterIsClampedToHalfTheSpacing)
{
    constexpr float32 kSpacing = 2.0f;
    constexpr float32 kLength = 40.0f;
    TileLayoutParams p = FlatParams(kSpacing);
    p.Seed = 5u;
    p.SpacingJitterMetres = 100.0f;

    const std::vector<TilePose> jittered = BuildTilePoses(FlatLineZ(kLength, 81u), p);
    const std::vector<TilePose> nominal = BuildTilePoses(FlatLineZ(kLength, 81u), FlatParams(kSpacing));
    ASSERT_EQ(jittered.size(), nominal.size());

    bool anyActuallyMoved = false;
    for (size_t i = 0; i < jittered.size(); ++i)
    {
        const float32 moved = std::abs(jittered[i].Base.z - nominal[i].Base.z);
        EXPECT_LE(moved, kSpacing * 0.5f + 1.0e-3f) << "station " << i;
        anyActuallyMoved |= moved > 1.0e-3f;
    }
    EXPECT_TRUE(anyActuallyMoved) << "the clamp swallowed the whole effect";
}

// ---- Dropout ----

// The contract that makes dropout usable: removing stations must not disturb
// the ones that remain. A survivor keeps its ordinal, and its pose is
// bit-identical to the pose it had when nothing was dropped — so the mesh the
// controller picks for it (keyed on that ordinal) is unchanged too.
TEST(SplineStationJitter, DropoutLeavesSurvivorsBitIdenticalAndKeepsTheirOrdinals)
{
    TileLayoutParams full = FlatParams(2.0f);
    full.Seed = 91u;
    full.SpacingJitterMetres = 0.4f;
    full.LateralJitterMetres = 0.25f;
    full.YawJitterDegrees = 10.0f;

    TileLayoutParams thinned = full;
    thinned.DropoutChance = 0.35f;

    const std::vector<TilePose> all = BuildTilePoses(FlatLineZ(60.0f, 121u), full);
    const std::vector<TilePose> some = BuildTilePoses(FlatLineZ(60.0f, 121u), thinned);
    ASSERT_FALSE(all.empty());
    ASSERT_LT(some.size(), all.size()) << "dropout removed nothing";
    ASSERT_FALSE(some.empty()) << "dropout removed everything";

    // Ordinals stay strictly ascending, and each survivor matches the full run's
    // pose at that same ordinal, bit for bit.
    for (size_t i = 0; i < some.size(); ++i)
    {
        if (i > 0)
            EXPECT_GT(some[i].StationIndex, some[i - 1].StationIndex);
        ASSERT_LT(some[i].StationIndex, all.size());
        EXPECT_TRUE(PoseBitsEqual(some[i], all[some[i].StationIndex]))
            << "survivor at ordinal " << some[i].StationIndex << " moved when its neighbours left";
    }
}

// StationIndex is the ordinal along the spline, NOT the index in the returned
// vector — that distinction is what the pool pick depends on.
TEST(SplineStationJitter, StationIndexIsTheOrdinalNotTheArrayIndex)
{
    TileLayoutParams p = FlatParams(2.0f);
    p.Seed = 3u;
    p.DropoutChance = 0.5f;
    const std::vector<TilePose> poses = BuildTilePoses(FlatLineZ(60.0f, 121u), p);
    ASSERT_GE(poses.size(), 2u);

    bool anyDiverges = false;
    for (size_t i = 0; i < poses.size(); ++i)
        anyDiverges |= poses[i].StationIndex != static_cast<uint32>(i);
    EXPECT_TRUE(anyDiverges)
        << "with half the stations dropped, ordinals must have outrun array indices";
}

// With no dropout the two DO coincide, which is what keeps the default layout's
// picks identical to what they were before this field existed.
TEST(SplineStationJitter, WithoutDropoutTheOrdinalIsTheArrayIndex)
{
    const std::vector<TilePose> poses = BuildTilePoses(GoldenScenario(), GoldenParams());
    ASSERT_FALSE(poses.empty());
    for (size_t i = 0; i < poses.size(); ++i)
        EXPECT_EQ(poses[i].StationIndex, static_cast<uint32>(i));
}

TEST(SplineStationJitter, FullDropoutPlacesNothingAndNoDropoutPlacesEverything)
{
    TileLayoutParams none = FlatParams(2.0f);
    none.Seed = 8u;
    none.DropoutChance = 0.0f;
    EXPECT_EQ(BuildTilePoses(FlatLineZ(40.0f, 81u), none).size(), 21u);

    TileLayoutParams all = none;
    all.DropoutChance = 1.0f;
    EXPECT_TRUE(BuildTilePoses(FlatLineZ(40.0f, 81u), all).empty());
}

// ---- End taper ----

// The taper thins toward the ends and leaves the middle alone: that asymmetry
// is the whole point, so measure both halves rather than just the total.
TEST(SplineStationJitter, EndTaperThinsTheEndsAndLeavesTheMiddleIntact)
{
    constexpr float32 kLength = 60.0f;
    constexpr float32 kTaper = 15.0f;
    TileLayoutParams p = FlatParams(2.0f);
    p.Seed = 17u;
    p.EndTaperMetres = kTaper;

    const std::vector<TilePose> tapered = BuildTilePoses(FlatLineZ(kLength, 121u), p);
    const std::vector<TilePose> square = BuildTilePoses(FlatLineZ(kLength, 121u), FlatParams(2.0f));
    ASSERT_LT(tapered.size(), square.size()) << "the taper removed nothing";

    const auto inZone = [](const std::vector<TilePose>& poses, bool nearEnds)
    {
        return std::count_if(poses.begin(), poses.end(), [&](const TilePose& t)
        {
            const float32 fromEnd = std::min(t.Base.z, kLength - t.Base.z);
            return nearEnds ? (fromEnd < kTaper) : (fromEnd >= kTaper);
        });
    };

    // Nothing outside the taper zone may be touched.
    EXPECT_EQ(inZone(tapered, false), inZone(square, false))
        << "the taper reached past its own zone into the middle of the run";
    // Inside it, stations must actually have gone.
    EXPECT_LT(inZone(tapered, true), inZone(square, true));
}

// A taper reaching the whole run leaves the extreme ends empty: keep
// probability hits zero AT each end, which is what "peters out" means.
TEST(SplineStationJitter, TaperedRunsDoNotStartOrEndOnAStation)
{
    constexpr float32 kLength = 40.0f;
    TileLayoutParams p = FlatParams(2.0f);
    p.Seed = 23u;
    p.EndTaperMetres = kLength * 0.5f;

    const std::vector<TilePose> poses = BuildTilePoses(FlatLineZ(kLength, 81u), p);
    ASSERT_FALSE(poses.empty());
    EXPECT_GT(poses.front().Base.z, 0.0f);
    EXPECT_LT(poses.back().Base.z, kLength);
}

// Dropout and taper compose on the KEEP probability, so asking for both must
// leave no more than either alone would.
TEST(SplineStationJitter, DropoutAndTaperCompose)
{
    TileLayoutParams taperOnly = FlatParams(2.0f);
    taperOnly.Seed = 44u;
    taperOnly.EndTaperMetres = 12.0f;

    TileLayoutParams dropOnly = FlatParams(2.0f);
    dropOnly.Seed = 44u;
    dropOnly.DropoutChance = 0.3f;

    TileLayoutParams both = taperOnly;
    both.DropoutChance = 0.3f;

    const auto count = [](const TileLayoutParams& p)
    { return BuildTilePoses(FlatLineZ(60.0f, 121u), p).size(); };

    const size_t taperCount = count(taperOnly);
    const size_t dropCount = count(dropOnly);
    const size_t bothCount = count(both);
    EXPECT_LE(bothCount, taperCount);
    EXPECT_LE(bothCount, dropCount);
}

// ---- Yaw and lateral jitter ----

// Yaw jitter must turn tiles without corrupting the frame they are turned in.
TEST(SplineStationJitter, YawJitterTurnsTilesAndKeepsTheBasisOrthonormal)
{
    TileLayoutParams p = FlatParams(2.0f);
    p.Seed = 61u;
    p.YawJitterDegrees = 20.0f;

    const std::vector<TilePose> turned = BuildTilePoses(FlatLineZ(40.0f, 81u), p);
    const std::vector<TilePose> straight = BuildTilePoses(FlatLineZ(40.0f, 81u), FlatParams(2.0f));
    ASSERT_EQ(turned.size(), straight.size());

    bool anyTurned = false;
    for (size_t i = 0; i < turned.size(); ++i)
    {
        const TilePose& t = turned[i];
        EXPECT_NEAR(V3::Dot(t.Right, t.Right), 1.0f, 1.0e-4f) << "pose " << i;
        EXPECT_NEAR(V3::Dot(t.Up, t.Up), 1.0f, 1.0e-4f) << "pose " << i;
        EXPECT_NEAR(V3::Dot(t.Forward, t.Forward), 1.0f, 1.0e-4f) << "pose " << i;
        EXPECT_NEAR(V3::Dot(t.Right, t.Forward), 0.0f, 1.0e-4f) << "pose " << i;
        EXPECT_NEAR(V3::Dot(t.Right, t.Up), 0.0f, 1.0e-4f) << "pose " << i;

        const float32 turn = std::acos(std::clamp(V3::Dot(t.Forward, straight[i].Forward), -1.0f,
                                                  1.0f)) *
                             180.0f / Mathematics::Pi;
        EXPECT_LE(turn, 20.0f + 1.0e-2f) << "pose " << i << " turned past the cap";
        anyTurned |= turn > 1.0f;
    }
    EXPECT_TRUE(anyTurned) << "yaw jitter turned nothing";
}

// Lateral jitter moves ACROSS travel. On a straight +Z run that means X moves
// and the along-path coordinate does not.
TEST(SplineStationJitter, LateralJitterMovesAcrossTravelNotAlongIt)
{
    TileLayoutParams p = FlatParams(2.0f);
    p.Seed = 71u;
    p.LateralJitterMetres = 0.75f;

    const std::vector<TilePose> slid = BuildTilePoses(FlatLineZ(40.0f, 81u), p);
    const std::vector<TilePose> centred = BuildTilePoses(FlatLineZ(40.0f, 81u), FlatParams(2.0f));
    ASSERT_EQ(slid.size(), centred.size());

    bool anySlid = false;
    for (size_t i = 0; i < slid.size(); ++i)
    {
        EXPECT_NEAR(slid[i].Base.z, centred[i].Base.z, 1.0e-4f)
            << "pose " << i << " moved along the path, not across it";
        const float32 across = std::abs(slid[i].Base.x - centred[i].Base.x);
        EXPECT_LE(across, 0.75f + 1.0e-3f) << "pose " << i;
        anySlid |= across > 1.0e-3f;
    }
    EXPECT_TRUE(anySlid) << "lateral jitter moved nothing";
}

// ---- Hostile input ----

// Authored floats arrive unsanitized and NaN passes std::clamp straight through.
// A poisoned knob must mean "no variation", never a NaN pose — a single NaN
// here would travel into transforms, bounds and culling.
TEST(SplineStationJitter, NonFiniteKnobsAreIgnoredRatherThanPropagated)
{
    const float32 nan = std::numeric_limits<float32>::quiet_NaN();
    const float32 inf = std::numeric_limits<float32>::infinity();

    for (float32 poison : {nan, inf, -inf})
    {
        TileLayoutParams p = GoldenParams();
        p.Seed = 12u;
        p.SpacingJitterMetres = poison;
        p.YawJitterDegrees = poison;
        p.LateralJitterMetres = poison;
        p.DropoutChance = poison;
        p.EndTaperMetres = poison;

        const std::vector<TilePose> poses = BuildTilePoses(GoldenScenario(), p);
        const std::vector<TilePose> clean = BuildTilePoses(GoldenScenario(), GoldenParams());
        ASSERT_EQ(poses.size(), clean.size()) << "a poisoned knob changed the station count";
        for (size_t i = 0; i < poses.size(); ++i)
        {
            ASSERT_TRUE(std::isfinite(poses[i].Position.x) && std::isfinite(poses[i].Position.y) &&
                        std::isfinite(poses[i].Position.z))
                << "pose " << i << " is non-finite";
            EXPECT_TRUE(PoseBitsEqual(poses[i], clean[i])) << "pose " << i;
        }
    }
}

// A negative knob is not an invitation to jitter backwards by its magnitude.
TEST(SplineStationJitter, NegativeKnobsAreTreatedAsZero)
{
    TileLayoutParams p = GoldenParams();
    p.Seed = 13u;
    p.SpacingJitterMetres = -3.0f;
    p.YawJitterDegrees = -30.0f;
    p.LateralJitterMetres = -1.0f;
    p.DropoutChance = -0.5f;
    p.EndTaperMetres = -10.0f;

    const std::vector<TilePose> poses = BuildTilePoses(GoldenScenario(), p);
    const std::vector<TilePose> clean = BuildTilePoses(GoldenScenario(), GoldenParams());
    ASSERT_EQ(poses.size(), clean.size());
    for (size_t i = 0; i < poses.size(); ++i)
        EXPECT_TRUE(PoseBitsEqual(poses[i], clean[i])) << "pose " << i;
}

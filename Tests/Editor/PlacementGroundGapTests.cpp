// The composed-ground query and the signed ground-to-underside gap, on a
// synthetic terrain whose every expected number is computable by hand.
//
// The base heightfield is FLAT, so a modifier is the only thing that can move a
// reading. That is what makes these tests falsifiable rather than decorative: an
// implementation that reads the base heightmap instead of the composed one
// returns kBaseGroundY where a modifier covers the sample, and the assertions
// below name both numbers. That difference is the whole reason the query exists —
// an offline model of the shipped heightmap cannot see a scene-authored modifier
// at all.
//
// Placed pieces are ROTATED here, not just translated. A level piece is the one
// pose where a box around it and the piece itself agree, so a suite built only
// from level pieces cannot see the gap metric's real question: which surface, and
// over which ground, is being measured.

#include <gtest/gtest.h>

#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Transform.h"
#include "ECS/World.h"
#include "Placement/PlacementGroundGap.h"
#include "TerrainECS/PlanarHeightQuery.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainService.h"

#include <cmath>

using namespace GameEngine;
using GameEngine::Editor::GroundGapProbe;
using GameEngine::Editor::GroundGapStatus;
using GameEngine::Editor::kGroundGapProbeCount;
using GameEngine::Editor::MeasurePieceGroundGap;
using GameEngine::Editor::MeasureRouteGroundGaps;
using GameEngine::Editor::PieceGroundGap;
using GameEngine::TerrainECS::PlanarHeightQuery;
using GameEngine::TerrainECS::ResolvePlanarHeightQuery;
using GameEngine::Mathematics::Vector3;
using GameEngine::TerrainECS::TerrainService;

namespace
{

constexpr float32 kWorldSize = 256.0f;
constexpr float32 kHeightScale = 64.0f;
constexpr uint32 kHeightmapDim = 129u;

// TerrainBaseSource::Flat fills a constant ZERO normalized height, so the base
// world Y is the terrain entity's own altitude. The bake refills the base from
// BaseSource before applying modifiers, so writing the heightfield by hand here
// would be discarded — the base source IS the control, not the samples.
//
// That altitude is deliberately non-zero: a base of 0 would be indistinguishable
// from the "no data" answer, and a control that cannot fail is not a control.
constexpr float32 kTerrainOriginY = 7.0f;
constexpr float32 kBaseGroundY = kTerrainOriginY;

// The flatten volume: centred on the origin, hard-edged (falloff 0, so the shape
// weight is a binary in/out mask and the composed height is a whole number
// rather than the product of two ramps), levelling to an absolute world Y.
constexpr float32 kFlattenTargetY = 30.0f;
constexpr float32 kFlattenRadius = 40.0f;

// Probe far enough inside the circle that no edge behaviour can reach it, and
// far enough outside that none can reach the control either.
constexpr float32 kInsideX = 0.0f;
constexpr float32 kInsideZ = 0.0f;
constexpr float32 kOutsideX = 100.0f;
constexpr float32 kOutsideZ = 100.0f;

// Millimetre, in world metres — the tolerance the sibling terrain suites use.
constexpr float32 kHeightTolerance = 1.0e-3f;

// The synthetic piece: a 2 m square slab 0.2 m thick, pivot at its centre, so
// its underside sits 0.1 m below whatever Y its transform carries.
constexpr float32 kPieceHalfXZ = 1.0f;
constexpr float32 kPieceHalfY = 0.1f;

// The tilt the route pieces this instrument was built for actually carry: the
// review measured 19 of 20 pieces between 13 and 50 degrees.
constexpr float32 kPitchDeg = 30.0f;

constexpr float32 kDegToRad = 3.14159265358979323846f / 180.0f;

const Vector3 kUnitScale{1.0f, 1.0f, 1.0f};
const Vector3 kSlabHalf{kPieceHalfXZ, kPieceHalfY, kPieceHalfXZ};

void EnsureTerrainService()
{
    if (!TerrainService::IsInitialized())
        TerrainService::Initialize();
}

// Scale, then pitch about X, then yaw about Y, then translate — stored
// column-major, the layout WorldTransform carries and the gap metric reads its
// half-axes from.
Components::WorldTransform PoseAt(const Vector3& position, float32 pitchDeg, float32 yawDeg,
                                  const Vector3& scale)
{
    const float32 cp = std::cos(pitchDeg * kDegToRad);
    const float32 sp = std::sin(pitchDeg * kDegToRad);
    const float32 cy = std::cos(yawDeg * kDegToRad);
    const float32 sy = std::sin(yawDeg * kDegToRad);

    Components::WorldTransform xf{};
    xf.matrix[0] = cy * scale.x;
    xf.matrix[1] = 0.0f;
    xf.matrix[2] = -sy * scale.x;

    xf.matrix[4] = sy * sp * scale.y;
    xf.matrix[5] = cp * scale.y;
    xf.matrix[6] = cy * sp * scale.y;

    xf.matrix[8] = sy * cp * scale.z;
    xf.matrix[9] = -sp * scale.z;
    xf.matrix[10] = cy * cp * scale.z;

    xf.matrix[12] = position.x;
    xf.matrix[13] = position.y;
    xf.matrix[14] = position.z;
    return xf;
}

Components::WorldTransform IdentityAt(float32 x, float32 y, float32 z)
{
    return PoseAt(Vector3{x, y, z}, 0.0f, 0.0f, kUnitScale);
}

// A negative-determinant transform: one basis column flipped, which mirrors the
// piece without changing the box it occupies.
Components::WorldTransform MirrorColumn(Components::WorldTransform xf, uint32 column)
{
    xf.matrix[column * 4u] *= -1.0f;
    xf.matrix[column * 4u + 1u] *= -1.0f;
    xf.matrix[column * 4u + 2u] *= -1.0f;
    return xf;
}

Components::LocalBounds BoundsOf(const Vector3& halfExtents)
{
    Components::LocalBounds bounds{};
    bounds.Box.center = {0.0f, 0.0f, 0.0f};
    bounds.Box.halfExtents = halfExtents;
    return bounds;
}

// Where a slab pitched about X actually presents its underside, derived from the
// pose rather than from the code under test: the underside quad's centre sits
// cos(pitch) * halfY below the pivot, and its two edges sit sin(pitch) * halfZ
// above and below that centre.
struct PitchedUnderside
{
    float32 CenterY = 0.0f;
    float32 MinY = 0.0f;
    float32 MaxY = 0.0f;
};

PitchedUnderside PitchedSlabUnderside(float32 pivotY, float32 pitchDeg, const Vector3& half)
{
    const float32 centerY = pivotY - half.y * std::cos(pitchDeg * kDegToRad);
    const float32 edge = half.z * std::sin(pitchDeg * kDegToRad);
    return PitchedUnderside{centerY, centerY - edge, centerY + edge};
}

// The pivot Y that lands a pitched slab's underside CENTRE exactly on `groundY`.
float32 PitchedPivotForUndersideCenter(float32 groundY, float32 pitchDeg, const Vector3& half)
{
    return groundY + half.y * std::cos(pitchDeg * kDegToRad);
}

// A terrain whose base bakes to a constant, so every expected number below is
// arithmetic rather than a sample of noise.
ECS::EntityHandle CreateFlatTerrain(ECS::World& world)
{
    TerrainService& service = TerrainService::Get();

    Terrain::TerrainConfig config{};
    config.HeightmapWidth = kHeightmapDim;
    config.HeightmapHeight = kHeightmapDim;
    config.WorldSizeX = kWorldSize;
    config.WorldSizeZ = kWorldSize;
    config.HeightScale = kHeightScale;
    config.LODLevels = 4;

    const TerrainECS::TerrainHandle handle = service.CreateTerrain(config);
    auto* data = service.GetTerrainData(handle);
    EXPECT_NE(data, nullptr);

    data->MarkFullDirty();
    service.RebuildQuadtree(handle);
    data->ResetSplatmapAndCommitRange();

    auto entity = world.CreateEntity();
    Components::Terrain terrain{};
    terrain.SizeX = kWorldSize;
    terrain.SizeZ = kWorldSize;
    terrain.HeightScale = kHeightScale;
    terrain.TerrainDataHandle = handle.Index;
    terrain.TerrainDataGeneration = handle.Generation;
    terrain.BaseSource = Components::TerrainBaseSource::Flat;
    world.AddComponentImmediate<Components::Terrain>(entity, terrain);
    world.AddComponentImmediate<Components::WorldTransform>(
        entity, IdentityAt(0.0f, kTerrainOriginY, 0.0f));
    return entity;
}

// A hard-edged circular flatten to an absolute world Y.
void AddFlattenVolume(ECS::World& world, float32 centerX, float32 centerZ, float32 radius,
                      float32 targetY)
{
    auto entity = world.CreateEntity();
    Components::TerrainModifierVolume volume{};
    volume.Shape = Components::TerrainVolumeShape::Circle;
    volume.Radius = radius;
    volume.Falloff = 0.0f;
    volume.Priority = 0.0f;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(entity, volume);

    Components::TerrainFlattenEffect flatten{};
    flatten.UseVolumeHeight = false;
    flatten.TargetHeight = targetY;
    flatten.Blend = Components::TerrainModifierBlend::Set;
    world.AddComponentImmediate<Components::TerrainFlattenEffect>(entity, flatten);

    world.AddComponentImmediate<Components::WorldTransform>(entity,
                                                           IdentityAt(centerX, 0.0f, centerZ));
}

void BakeModifiers(ECS::World& world)
{
    TerrainECS::TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);
}

// A placed piece: LocalBounds + WorldTransform, parented to `route`. This is the
// shape the spline placement controllers create, built directly because none of
// them is compiled into this target.
ECS::EntityHandle CreatePiece(ECS::World& world, ECS::EntityHandle route, float32 x, float32 y,
                              float32 z)
{
    auto entity = world.CreateEntity();
    world.AddComponentImmediate<Components::LocalBounds>(entity, BoundsOf(kSlabHalf));
    world.AddComponentImmediate<Components::WorldTransform>(entity, IdentityAt(x, y, z));
    world.AddComponentImmediate<Components::Parent>(entity, Components::Parent{route});
    return entity;
}

// The transform Y that puts a level piece's underside exactly on `undersideY`.
constexpr float32 PivotForUnderside(float32 undersideY)
{
    return undersideY + kPieceHalfY;
}

PieceGroundGap Measure(const TerrainECS::PlanarHeightQuery& query, const Vector3& half,
                       const Components::WorldTransform& xf)
{
    PieceGroundGap gap{};
    MeasurePieceGroundGap(query, BoundsOf(half).Box, xf.matrix, gap);
    return gap;
}

} // namespace

TEST(ComposedGroundQuery, ReadsTheModifierAndNotTheBaseHeightmap)
{
    EnsureTerrainService();
    ECS::World world;
    CreateFlatTerrain(world);
    AddFlattenVolume(world, 0.0f, 0.0f, kFlattenRadius, kFlattenTargetY);
    BakeModifiers(world);

    const TerrainECS::PlanarHeightQuery query = TerrainECS::ResolvePlanarHeightQuery(world);
    ASSERT_TRUE(query.IsValid());

    float32 inside = 0.0f;
    ASSERT_TRUE(query.SampleHeight(kInsideX, kInsideZ, inside));
    EXPECT_NEAR(inside, kFlattenTargetY, kHeightTolerance)
        << "inside the flatten the query must read the composed height " << kFlattenTargetY
        << " m; reading the base " << kBaseGroundY
        << " m means it is sampling the base heightmap and cannot see a modifier at all";

    // The control: the same query, same terrain, outside the volume. Without it
    // an implementation that returned kFlattenTargetY everywhere would pass.
    float32 outside = 0.0f;
    ASSERT_TRUE(query.SampleHeight(kOutsideX, kOutsideZ, outside));
    EXPECT_NEAR(outside, kBaseGroundY, kHeightTolerance)
        << "outside the flatten the query must read the untouched base";
}

TEST(ComposedGroundQuery, OutsideTheFootprintRefusesRatherThanReportingZero)
{
    EnsureTerrainService();
    ECS::World world;
    CreateFlatTerrain(world);
    BakeModifiers(world);

    const TerrainECS::PlanarHeightQuery query = TerrainECS::ResolvePlanarHeightQuery(world);
    ASSERT_TRUE(query.IsValid());

    constexpr float32 kBeyondEdge = kWorldSize;
    EXPECT_FALSE(query.ContainsXZ(kBeyondEdge, kBeyondEdge));
    float32 y = -1.0f;
    EXPECT_FALSE(query.SampleHeight(kBeyondEdge, kBeyondEdge, y))
        << "a point off the terrain must be refused, never answered as ground level";
}

TEST(PlacementGroundGap, SignsFloatingPositiveAndBuriedNegative)
{
    EnsureTerrainService();
    ECS::World world;
    CreateFlatTerrain(world);
    AddFlattenVolume(world, 0.0f, 0.0f, kFlattenRadius, kFlattenTargetY);
    BakeModifiers(world);

    const TerrainECS::PlanarHeightQuery query = TerrainECS::ResolvePlanarHeightQuery(world);
    ASSERT_TRUE(query.IsValid());

    // Ground here is the flattened kFlattenTargetY, so the arithmetic is
    // underside - 30.0 in every arm.
    constexpr float32 kFloat = 0.5f;
    constexpr float32 kBury = 0.3f;

    const auto measure = [&](float32 undersideY) {
        const PieceGroundGap gap = Measure(
            query, kSlabHalf,
            IdentityAt(kInsideX, PivotForUnderside(undersideY), kInsideZ));
        EXPECT_EQ(gap.Status, GroundGapStatus::Measured);
        return gap;
    };

    const PieceGroundGap bedded = measure(kFlattenTargetY);
    EXPECT_NEAR(bedded.CenterGap, 0.0f, kHeightTolerance);
    EXPECT_NEAR(bedded.MinGap, 0.0f, kHeightTolerance);
    EXPECT_NEAR(bedded.MaxGap, 0.0f, kHeightTolerance);
    EXPECT_TRUE(bedded.FullySampled());

    const PieceGroundGap floating = measure(kFlattenTargetY + kFloat);
    EXPECT_NEAR(floating.CenterGap, kFloat, kHeightTolerance)
        << "a piece above the ground must read POSITIVE";
    EXPECT_NEAR(floating.MaxGap, kFloat, kHeightTolerance);

    const PieceGroundGap buried = measure(kFlattenTargetY - kBury);
    EXPECT_NEAR(buried.CenterGap, -kBury, kHeightTolerance)
        << "a piece below the ground must read NEGATIVE";
    EXPECT_NEAR(buried.MinGap, -kBury, kHeightTolerance);
}

TEST(PlacementGroundGap, MeasuresAgainstComposedGroundNotTheBase)
{
    EnsureTerrainService();
    ECS::World world;
    CreateFlatTerrain(world);
    AddFlattenVolume(world, 0.0f, 0.0f, kFlattenRadius, kFlattenTargetY);
    BakeModifiers(world);

    const TerrainECS::PlanarHeightQuery query = TerrainECS::ResolvePlanarHeightQuery(world);
    ASSERT_TRUE(query.IsValid());

    // A piece bedded on the FLATTENED ground. Measured against the base it would
    // read +14 m of float; against the composed ground it reads zero. This is the
    // gap instrument inheriting the query's modifier-awareness, and it is the
    // reading an offline model of the shipped heightmap gets wrong.
    const PieceGroundGap gap = Measure(
        query, kSlabHalf,
        IdentityAt(kInsideX, PivotForUnderside(kFlattenTargetY), kInsideZ));
    ASSERT_EQ(gap.Status, GroundGapStatus::Measured);
    EXPECT_NEAR(gap.CenterGap, 0.0f, kHeightTolerance);

    constexpr float32 kGapAgainstBase = kFlattenTargetY - kBaseGroundY;
    EXPECT_GT(std::abs(gap.CenterGap - kGapAgainstBase), 1.0f)
        << "the gap matched the BASE-relative " << kGapAgainstBase
        << " m, so the instrument is not reading composed ground";
}

TEST(PlacementGroundGap, ReportsEveryPieceParentedToTheRoute)
{
    EnsureTerrainService();
    ECS::World world;
    CreateFlatTerrain(world);
    AddFlattenVolume(world, 0.0f, 0.0f, kFlattenRadius, kFlattenTargetY);
    BakeModifiers(world);

    const TerrainECS::PlanarHeightQuery query = TerrainECS::ResolvePlanarHeightQuery(world);
    ASSERT_TRUE(query.IsValid());

    auto route = world.CreateEntity();
    world.AddComponentImmediate<Components::WorldTransform>(route, IdentityAt(0.0f, 0.0f, 0.0f));

    constexpr float32 kFloat = 0.25f;
    CreatePiece(world, route, kInsideX, PivotForUnderside(kFlattenTargetY), kInsideZ);
    CreatePiece(world, route, kInsideX + 4.0f, PivotForUnderside(kFlattenTargetY + kFloat),
                kInsideZ);

    // A piece under a DIFFERENT parent must not be counted: the route link is the
    // only thing that scopes the report, and an over-broad scan would silently
    // fold another route's pieces into this one's worst-case summary.
    auto otherRoute = world.CreateEntity();
    world.AddComponentImmediate<Components::WorldTransform>(otherRoute,
                                                            IdentityAt(0.0f, 0.0f, 0.0f));
    CreatePiece(world, otherRoute, kInsideX, PivotForUnderside(kFlattenTargetY + 9.0f), kInsideZ);

    std::vector<PieceGroundGap> gaps;
    MeasureRouteGroundGaps(world, route, query, gaps);

    ASSERT_EQ(gaps.size(), 2u);
    float32 worstFloating = -1.0e9f;
    for (const PieceGroundGap& gap : gaps)
    {
        EXPECT_TRUE(gap.FullySampled());
        worstFloating = std::max(worstFloating, gap.MaxGap);
    }
    EXPECT_NEAR(worstFloating, kFloat, kHeightTolerance)
        << "the route summary must come from this route's pieces only";
}

// A pitched slab bedded at the middle of its underside, on ground that is flat
// over its whole neighbourhood. Everything a box around the piece could tell you
// here is wrong: its floor is sin(pitch) * halfZ below the underside centre, and
// it reports that one number for all five probes, so the piece reads uniformly
// buried when in truth one edge is buried and the opposite edge stands proud by
// the same amount.
TEST(PlacementGroundGap, PitchedPieceMeasuresItsOwnUndersideNotTheBoxUnderIt)
{
    EnsureTerrainService();
    ECS::World world;
    CreateFlatTerrain(world);
    AddFlattenVolume(world, 0.0f, 0.0f, kFlattenRadius, kFlattenTargetY);
    BakeModifiers(world);

    const TerrainECS::PlanarHeightQuery query = TerrainECS::ResolvePlanarHeightQuery(world);
    ASSERT_TRUE(query.IsValid());

    // Control: the ground under this piece is the flatten, everywhere it probes.
    float32 groundUnderPiece = 0.0f;
    ASSERT_TRUE(query.SampleHeight(kInsideX, kInsideZ, groundUnderPiece));
    ASSERT_NEAR(groundUnderPiece, kFlattenTargetY, kHeightTolerance);

    const float32 pivotY =
        PitchedPivotForUndersideCenter(kFlattenTargetY, kPitchDeg, kSlabHalf);
    const Components::WorldTransform xf =
        PoseAt(Vector3{kInsideX, pivotY, kInsideZ}, kPitchDeg, 0.0f, kUnitScale);
    const PitchedUnderside expected = PitchedSlabUnderside(pivotY, kPitchDeg, kSlabHalf);

    const PieceGroundGap gap = Measure(query, kSlabHalf, xf);
    ASSERT_EQ(gap.Status, GroundGapStatus::Measured);
    EXPECT_TRUE(gap.FullySampled());

    EXPECT_NEAR(gap.UndersideCenterY, expected.CenterY, kHeightTolerance)
        << "the underside centre is cos(pitch) * halfY below the pivot";
    EXPECT_NEAR(gap.CenterGap, 0.0f, kHeightTolerance)
        << "the underside centre was bedded ON the ground, so its gap is zero";
    EXPECT_NEAR(gap.MinGap, expected.MinY - kFlattenTargetY, kHeightTolerance)
        << "the buried edge is sin(pitch) * halfZ below the underside centre";
    EXPECT_NEAR(gap.MaxGap, expected.MaxY - kFlattenTargetY, kHeightTolerance)
        << "the opposite edge stands the same distance PROUD — a single number "
           "cannot describe a tilted piece, and the box's floor reports the buried "
           "edge for all five probes";

    // The bias a world AABB carries, stated as a number rather than left implicit:
    // its floor is the lowest underside CORNER, not the underside under any
    // probe.
    const Mathematics::AABB worldBox = BoundsOf(kSlabHalf).Box.TransformToAABB(xf.matrix);
    EXPECT_NEAR(gap.UndersideCenterY - worldBox.min.y,
                kSlabHalf.z * std::sin(kPitchDeg * kDegToRad), kHeightTolerance);
}

// Non-uniform scale reaches the metric through the same half-axes, so the closed
// form holds with the scaled extents — no inverse transpose, no special case.
TEST(PlacementGroundGap, NonUniformScaleScalesTheUndersideWithThePiece)
{
    EnsureTerrainService();
    ECS::World world;
    CreateFlatTerrain(world);
    AddFlattenVolume(world, 0.0f, 0.0f, kFlattenRadius, kFlattenTargetY);
    BakeModifiers(world);

    const TerrainECS::PlanarHeightQuery query = TerrainECS::ResolvePlanarHeightQuery(world);
    ASSERT_TRUE(query.IsValid());

    const Vector3 scale{3.0f, 2.0f, 1.5f};
    const Vector3 scaledHalf{kSlabHalf.x * scale.x, kSlabHalf.y * scale.y,
                             kSlabHalf.z * scale.z};
    const float32 pivotY =
        PitchedPivotForUndersideCenter(kFlattenTargetY, kPitchDeg, scaledHalf);
    const PieceGroundGap gap =
        Measure(query, kSlabHalf,
                PoseAt(Vector3{kInsideX, pivotY, kInsideZ}, kPitchDeg, 0.0f, scale));
    const PitchedUnderside expected = PitchedSlabUnderside(pivotY, kPitchDeg, scaledHalf);

    ASSERT_EQ(gap.Status, GroundGapStatus::Measured);
    EXPECT_NEAR(gap.UndersideCenterY, expected.CenterY, kHeightTolerance);
    EXPECT_NEAR(gap.CenterGap, 0.0f, kHeightTolerance);
    EXPECT_NEAR(gap.MinGap, expected.MinY - kFlattenTargetY, kHeightTolerance);
    EXPECT_NEAR(gap.MaxGap, expected.MaxY - kFlattenTargetY, kHeightTolerance);
}

// A yawed plank, level, bedded on the base ground — with a spike of raised ground
// placed exactly under one corner of the box that CONTAINS it, eight and a half
// metres off the plank itself. A metric that probes the box reports the plank
// thirteen metres buried in ground it does not stand on.
TEST(PlacementGroundGap, YawedPieceProbesOnlyGroundItActuallyCovers)
{
    EnsureTerrainService();
    ECS::World world;
    CreateFlatTerrain(world);

    const Vector3 plankHalf{0.5f, 0.15f, 8.0f};
    constexpr float32 kYawDeg = 45.0f;
    constexpr float32 kSpikeTargetY = 20.0f;
    constexpr float32 kSpikeRadius = 3.0f;

    // Level, with its underside exactly on the untouched base ground.
    const Components::WorldTransform xf =
        PoseAt(Vector3{0.0f, kBaseGroundY + plankHalf.y, 0.0f}, 0.0f, kYawDeg, kUnitScale);
    const Mathematics::AABB worldBox = BoundsOf(plankHalf).Box.TransformToAABB(xf.matrix);

    // The spike sits under the box's (max.x, min.z) corner — a corner of the
    // inflated box, and nowhere near the plank.
    const float32 spikeX = worldBox.max.x;
    const float32 spikeZ = worldBox.min.z;
    AddFlattenVolume(world, spikeX, spikeZ, kSpikeRadius, kSpikeTargetY);
    BakeModifiers(world);

    const TerrainECS::PlanarHeightQuery query = TerrainECS::ResolvePlanarHeightQuery(world);
    ASSERT_TRUE(query.IsValid());

    // Controls, both directions: the spike really is there (a fixture whose
    // modifier silently failed to bake would pass this test vacuously), and the
    // ground under the plank really is the untouched base.
    float32 spikeGround = 0.0f;
    ASSERT_TRUE(query.SampleHeight(spikeX, spikeZ, spikeGround));
    ASSERT_NEAR(spikeGround, kSpikeTargetY, kHeightTolerance)
        << "the spike did not bake; this fixture cannot fail without it";
    float32 plankGround = 0.0f;
    ASSERT_TRUE(query.SampleHeight(0.0f, 0.0f, plankGround));
    ASSERT_NEAR(plankGround, kBaseGroundY, kHeightTolerance)
        << "the spike reached the plank's own ground; move it further out";

    const PieceGroundGap gap = Measure(query, plankHalf, xf);
    ASSERT_EQ(gap.Status, GroundGapStatus::Measured);
    EXPECT_TRUE(gap.FullySampled());

    EXPECT_NEAR(gap.CenterGap, 0.0f, kHeightTolerance);
    EXPECT_NEAR(gap.MinGap, 0.0f, kHeightTolerance)
        << "the plank is bedded on flat base ground; a burial here is ground the "
           "piece does not cover — the spike under the containing box's corner";
    EXPECT_NEAR(gap.MaxGap, 0.0f, kHeightTolerance);

    // Structural: every probe lies on the plank. Its long axis runs along the
    // yawed local Z, so a probe's distance across that axis cannot exceed the
    // plank's own half width.
    const float32 axisX = std::sin(kYawDeg * kDegToRad);
    const float32 axisZ = std::cos(kYawDeg * kDegToRad);
    for (uint32 i = 0u; i < kGroundGapProbeCount; ++i)
    {
        const GroundGapProbe& probe = gap.Probes[i];
        const float32 across = std::abs(probe.X * axisZ - probe.Z * axisX);
        const float32 along = std::abs(probe.X * axisX + probe.Z * axisZ);
        EXPECT_LE(across, plankHalf.x + kHeightTolerance)
            << "probe " << i << " sits " << across << " m off the plank's axis";
        EXPECT_LE(along, plankHalf.z + kHeightTolerance)
            << "probe " << i << " sits " << along << " m along a plank " << plankHalf.z
            << " m half-long";
    }
}

// The property the review asked to preserve: whatever the orientation, lifting a
// piece lifts every gap by exactly the lift, and moves no probe.
TEST(PlacementGroundGap, LiftingAPieceLiftsEveryGapByTheSameAmount)
{
    EnsureTerrainService();
    ECS::World world;
    CreateFlatTerrain(world);
    AddFlattenVolume(world, 0.0f, 0.0f, kFlattenRadius, kFlattenTargetY);
    BakeModifiers(world);

    const TerrainECS::PlanarHeightQuery query = TerrainECS::ResolvePlanarHeightQuery(world);
    ASSERT_TRUE(query.IsValid());

    constexpr float32 kLift = 0.37f;
    const float32 pivotY =
        PitchedPivotForUndersideCenter(kFlattenTargetY, kPitchDeg, kSlabHalf);
    const PieceGroundGap low = Measure(
        query, kSlabHalf, PoseAt(Vector3{kInsideX, pivotY, kInsideZ}, kPitchDeg, 25.0f, kUnitScale));
    const PieceGroundGap high =
        Measure(query, kSlabHalf,
                PoseAt(Vector3{kInsideX, pivotY + kLift, kInsideZ}, kPitchDeg, 25.0f, kUnitScale));

    ASSERT_EQ(low.Status, GroundGapStatus::Measured);
    ASSERT_EQ(high.Status, GroundGapStatus::Measured);
    EXPECT_NEAR(high.MinGap - low.MinGap, kLift, kHeightTolerance);
    EXPECT_NEAR(high.MaxGap - low.MaxGap, kLift, kHeightTolerance);
    EXPECT_NEAR(high.CenterGap - low.CenterGap, kLift, kHeightTolerance);
    for (uint32 i = 0u; i < kGroundGapProbeCount; ++i)
    {
        EXPECT_NEAR(high.Probes[i].X, low.Probes[i].X, kHeightTolerance) << "probe " << i;
        EXPECT_NEAR(high.Probes[i].Z, low.Probes[i].Z, kHeightTolerance) << "probe " << i;
        EXPECT_NEAR(high.Probes[i].Gap - low.Probes[i].Gap, kLift, kHeightTolerance)
            << "probe " << i << " did not rise with the piece";
    }
}

// A mirrored transform turns the authored bottom face into the upper one. The
// underside is whichever face lies below the other, so the numbers must not move
// — and a metric that always took "centre minus the local-Y half-axis" would
// measure the top of the piece.
TEST(PlacementGroundGap, MirroredTransformMeasuresThePhysicallyLowerFace)
{
    EnsureTerrainService();
    ECS::World world;
    CreateFlatTerrain(world);
    AddFlattenVolume(world, 0.0f, 0.0f, kFlattenRadius, kFlattenTargetY);
    BakeModifiers(world);

    const TerrainECS::PlanarHeightQuery query = TerrainECS::ResolvePlanarHeightQuery(world);
    ASSERT_TRUE(query.IsValid());

    const float32 pivotY =
        PitchedPivotForUndersideCenter(kFlattenTargetY, kPitchDeg, kSlabHalf);
    const Components::WorldTransform xf =
        PoseAt(Vector3{kInsideX, pivotY, kInsideZ}, kPitchDeg, 0.0f, kUnitScale);
    const PieceGroundGap reference = Measure(query, kSlabHalf, xf);
    ASSERT_EQ(reference.Status, GroundGapStatus::Measured);

    // Column 1 is the local Y axis: flipping it stands the piece on its authored
    // top, which occupies the identical box.
    const PieceGroundGap mirroredY = Measure(query, kSlabHalf, MirrorColumn(xf, 1u));
    ASSERT_EQ(mirroredY.Status, GroundGapStatus::Measured);
    EXPECT_NEAR(mirroredY.UndersideCenterY, reference.UndersideCenterY, kHeightTolerance)
        << "a Y-mirrored piece was measured at its upper face";
    EXPECT_NEAR(mirroredY.MinGap, reference.MinGap, kHeightTolerance);
    EXPECT_NEAR(mirroredY.MaxGap, reference.MaxGap, kHeightTolerance);

    // Column 0 flips handedness without touching which face is down.
    const PieceGroundGap mirroredX = Measure(query, kSlabHalf, MirrorColumn(xf, 0u));
    ASSERT_EQ(mirroredX.Status, GroundGapStatus::Measured);
    EXPECT_NEAR(mirroredX.UndersideCenterY, reference.UndersideCenterY, kHeightTolerance);
    EXPECT_NEAR(mirroredX.MinGap, reference.MinGap, kHeightTolerance);
    EXPECT_NEAR(mirroredX.MaxGap, reference.MaxGap, kHeightTolerance);
}

// A wall has no underside above a world XZ, so there is no gap to report — and a
// steep ramp still has one. The pair pins the refusal to the degenerate case
// instead of letting it swallow ordinary tilt.
TEST(PlacementGroundGap, WallLikePieceRefusesAndASteepRampStillMeasures)
{
    EnsureTerrainService();
    ECS::World world;
    CreateFlatTerrain(world);
    AddFlattenVolume(world, 0.0f, 0.0f, kFlattenRadius, kFlattenTargetY);
    BakeModifiers(world);

    const TerrainECS::PlanarHeightQuery query = TerrainECS::ResolvePlanarHeightQuery(world);
    ASSERT_TRUE(query.IsValid());

    const float32 pivotY = kFlattenTargetY + 1.0f;
    const auto at = [&](float32 pitchDeg) {
        return Measure(query, kSlabHalf,
                       PoseAt(Vector3{kInsideX, pivotY, kInsideZ}, pitchDeg, 0.0f, kUnitScale));
    };

    EXPECT_EQ(at(90.0f).Status, GroundGapStatus::UndersideNearVertical)
        << "a piece stood on end has no underside height to compare with the ground";
    EXPECT_EQ(at(89.0f).Status, GroundGapStatus::UndersideNearVertical);
    EXPECT_EQ(at(60.0f).Status, GroundGapStatus::Measured)
        << "a 60-degree ramp is an ordinary placed piece; refusing it would blind "
           "the instrument to the poses it exists to measure";

    // No underside area at all: a piece flat in Z.
    const PieceGroundGap flat =
        Measure(query, Vector3{kPieceHalfXZ, kPieceHalfY, 0.0f},
                PoseAt(Vector3{kInsideX, pivotY, kInsideZ}, 0.0f, 0.0f, kUnitScale));
    EXPECT_EQ(flat.Status, GroundGapStatus::DegenerateFootprint);
}

// Off the terrain the gap is refused rather than answered against ground level,
// and the refusal names the cause the caller can act on.
TEST(PlacementGroundGap, PieceOffTheTerrainReportsNoGroundRatherThanZero)
{
    EnsureTerrainService();
    ECS::World world;
    CreateFlatTerrain(world);
    BakeModifiers(world);

    const TerrainECS::PlanarHeightQuery query = TerrainECS::ResolvePlanarHeightQuery(world);
    ASSERT_TRUE(query.IsValid());

    const PieceGroundGap gap =
        Measure(query, kSlabHalf,
                PoseAt(Vector3{kWorldSize, kBaseGroundY, kWorldSize}, kPitchDeg, 0.0f, kUnitScale));
    EXPECT_EQ(gap.Status, GroundGapStatus::NoGroundSampled);
    EXPECT_EQ(gap.ProbesSampled, 0u);
    EXPECT_FALSE(gap.FullySampled());
    EXPECT_NEAR(gap.MinGap, 0.0f, kHeightTolerance)
        << "a refused reading leaves zero, and says so through its status";
}

// ---------------------------------------------------------------------------
// Tiled residency
//
// SampleHeight branches on the tiled residency FIRST, and a 4 km island at 1 m
// spacing sits exactly on the boundary that decides which residency a terrain gets (its 1025 samples
// per axis are the per-tile maximum). Everything above runs on the single
// heightfield, so without this the gap metric's live behaviour on a tiled
// terrain would be untested — including the footprint the query reports, which
// on a tiled terrain comes from the residency and NOT from the terrain
// component's own Size fields.
// ---------------------------------------------------------------------------

namespace
{

// A 4 km island's shape: 1 sample per metre, so the per-tile heightmap lands on the
// 1025-sample maximum and the terrain tiles rather than staying single.
constexpr float32 kTiledWorldSize = 4096.0f;
constexpr float32 kTiledHeightScale = 200.0f;
constexpr float32 kTiledNormalizedHeight = 0.25f;
constexpr float32 kTiledGroundY = kTerrainOriginY + kTiledNormalizedHeight * kTiledHeightScale;

// A component footprint that DISAGREES with the residency, so a reader that takes
// the footprint from the component instead of the tiles is caught.
constexpr float32 kStaleComponentSize = 256.0f;

struct ResidentTiledTerrain
{
    TerrainECS::TiledTerrainHandle Handle{};
    float32 TileWorldSize = 0.0f;
    float32 WorldOriginX = 0.0f;
    float32 WorldOriginZ = 0.0f;
};

// One resident tile, filled with a constant so the expected ground is arithmetic.
// LoadTile leaves a tile Empty and fills it with procedural noise; production
// streams tiles to Full, and the sampler refuses an Empty tile, so both are set
// the way the sibling tiled suite sets them.
ResidentTiledTerrain CreateTiledTerrainWithOneResidentTile(ECS::World& world)
{
    TerrainService& service = TerrainService::Get();

    TerrainECS::TiledTerrainConfig config{};
    config.WorldSizeX = kTiledWorldSize;
    config.WorldSizeZ = kTiledWorldSize;
    config.HeightScale = kTiledHeightScale;
    config.SamplesPerMeter = 1.0f;
    config.PatchGridSize = 32;

    ResidentTiledTerrain resident{};
    resident.Handle = service.CreateTiledTerrain(config);
    auto* data = service.GetTiledTerrainData(resident.Handle);
    EXPECT_NE(data, nullptr);
    if (!data)
        return resident;
    resident.TileWorldSize = data->Config.TileWorldSize;
    resident.WorldOriginX = data->WorldOriginX;
    resident.WorldOriginZ = data->WorldOriginZ;
    EXPECT_EQ(data->Config.TileConfig.HeightmapWidth, 1025u)
        << "this fixture is meant to sit on the per-tile sample maximum";

    auto* tile = service.LoadTile(resident.Handle, TerrainECS::TileCoord{0, 0});
    EXPECT_NE(tile, nullptr);
    if (tile)
    {
        const uint32 width = tile->Heightfield.GetWidth();
        const uint32 height = tile->Heightfield.GetHeight();
        for (uint32 z = 0u; z < height; ++z)
            for (uint32 x = 0u; x < width; ++x)
                tile->Heightfield.SetSample(x, z, kTiledNormalizedHeight);
        tile->LodState = TerrainECS::TileLodState::Full;
    }

    auto entity = world.CreateEntity();
    Components::Terrain terrain{};
    terrain.SizeX = kStaleComponentSize;
    terrain.SizeZ = kStaleComponentSize;
    terrain.HeightScale = kTiledHeightScale;
    terrain.TiledTerrainHandle = resident.Handle.Index;
    terrain.TiledTerrainGeneration = resident.Handle.Generation;
    // No single residency: an index this far out of range can never match a slot,
    // so the query must resolve through the tiles.
    terrain.TerrainDataHandle = 0xFFFFFFFFu;
    terrain.TerrainDataGeneration = 0xFFFFFFFFu;
    terrain.BaseSource = Components::TerrainBaseSource::Flat;
    world.AddComponentImmediate<Components::Terrain>(entity, terrain);
    world.AddComponentImmediate<Components::WorldTransform>(
        entity, IdentityAt(0.0f, kTerrainOriginY, 0.0f));
    return resident;
}

} // namespace

TEST(PlacementGroundGap, MeasuresAPitchedPieceOnATiledResidency)
{
    EnsureTerrainService();
    ECS::World world;
    const ResidentTiledTerrain resident = CreateTiledTerrainWithOneResidentTile(world);

    const TerrainECS::PlanarHeightQuery query = TerrainECS::ResolvePlanarHeightQuery(world);
    ASSERT_TRUE(query.IsValid());
    ASSERT_NE(query.Tiled, nullptr) << "the fixture must resolve through the tiled residency";
    ASSERT_EQ(query.Single, nullptr) << "a single heightfield would answer instead of the tiles";

    // The footprint the query answers over is the residency's, not the component's
    // stale Size: a point 1000 m out is inside the tiles and outside the
    // component footprint.
    EXPECT_NEAR(query.FootprintSizeX(), kTiledWorldSize, kHeightTolerance);
    EXPECT_NEAR(query.FootprintOriginX(), resident.WorldOriginX, kHeightTolerance);
    EXPECT_NEAR(query.LatticeSpacingX(),
                resident.TileWorldSize / static_cast<float32>(1025u - 1u), kHeightTolerance);

    const float32 insideTileX = resident.WorldOriginX + resident.TileWorldSize * 0.5f;
    const float32 insideTileZ = resident.WorldOriginZ + resident.TileWorldSize * 0.5f;
    EXPECT_TRUE(query.ContainsXZ(insideTileX, insideTileZ));

    // Control: the resident tile reads its constant, composed through the
    // terrain's altitude and height scale.
    float32 ground = 0.0f;
    ASSERT_TRUE(query.SampleHeight(insideTileX, insideTileZ, ground));
    ASSERT_NEAR(ground, kTiledGroundY, kHeightTolerance);

    const float32 pivotY = PitchedPivotForUndersideCenter(kTiledGroundY, kPitchDeg, kSlabHalf);
    const PieceGroundGap gap =
        Measure(query, kSlabHalf,
                PoseAt(Vector3{insideTileX, pivotY, insideTileZ}, kPitchDeg, 0.0f, kUnitScale));
    const PitchedUnderside expected = PitchedSlabUnderside(pivotY, kPitchDeg, kSlabHalf);

    ASSERT_EQ(gap.Status, GroundGapStatus::Measured);
    EXPECT_TRUE(gap.FullySampled());
    EXPECT_NEAR(gap.CenterGap, 0.0f, kHeightTolerance);
    EXPECT_NEAR(gap.MinGap, expected.MinY - kTiledGroundY, kHeightTolerance);
    EXPECT_NEAR(gap.MaxGap, expected.MaxY - kTiledGroundY, kHeightTolerance);

    // A piece over a tile that never streamed in is refused, not answered: the
    // neighbouring tile coordinate is inside the footprint and has no data.
    const PieceGroundGap unstreamed = Measure(
        query, kSlabHalf,
        PoseAt(Vector3{insideTileX + resident.TileWorldSize, pivotY, insideTileZ}, kPitchDeg,
               0.0f, kUnitScale));
    EXPECT_TRUE(query.ContainsXZ(insideTileX + resident.TileWorldSize, insideTileZ));
    EXPECT_EQ(unstreamed.Status, GroundGapStatus::NoGroundSampled);

    TerrainService::Get().DestroyTiledTerrain(resident.Handle);
}

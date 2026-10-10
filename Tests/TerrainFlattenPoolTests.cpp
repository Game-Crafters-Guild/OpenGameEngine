// Pooled flattens: the two operators that let terrain volumes say something the
// priority stack cannot.
//
// A POOL averages: flattens whose Blend is Average blend their targets by weight
// and apply once, where the plain stack has the later one overwrite the earlier
// and leave a step. Membership is the PoolGroup name — blank is the shared pool
// every unnamed member joins, a name is the opt-OUT into a pool of its own. A
// CLAIM owns: a volume marks ground it owns, and each pool member that respects
// claims grades up to it and stops while its pool-mates carry on.
//
// The oracles here are of three kinds:
//
//  1. GEOMETRY — the station polyline, on its own. A SplinePath volume's geometry
//     is the arc-length resample, not the curve behind it, and the offline
//     instrument that audits an island scene's ground reproduces THIS object. Its
//     resampling rule, its segment-nearest query and its bounds are pinned
//     directly, because a divergence here reads downstream as a small plausible
//     offset rather than as a failure.
//
//  2. THE OPERATORS, RED AND GREEN — a synthetic crossing whose composed height
//     is computable by hand. Two straight routes at constant, DIFFERENT grades
//     cross a claimed road. Every assertion names the number it expects and the
//     number the operator-off arm produces instead, so a test that passes
//     because nothing happened is not available: the two arms differ by 5 m.
//
//  3. THE CONTRACTS AROUND THEM — that a claim writes no height, that claims
//     combine by max rather than sum, that a lone pooled flatten reduces exactly
//     to the unpooled one, that pooling is defined for every shape rather than
//     refused off a route, and that editing either effect re-bakes.
//
// Sample geometry, used by every hand-computed number below: the terrain is
// 129 samples over 256 m centred on the origin, so sample (sx, sz) sits at world
// (-128 + 2*sx, -128 + 2*sz) and world (0, 0) is sample (64, 64).

#include <gtest/gtest.h>

#include "Components/Spline/SplineComponent.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"
#include "SplineECS/SplineService.h"
#include "Terrain/Heightfield.h"
#include "TerrainECS/DirtyRegionLog.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainRoutePolyline.h"
#include "TerrainECS/TerrainService.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace
{
using namespace GameEngine;
using namespace GameEngine::TerrainECS;

struct ScopedTerrainService
{
    ScopedTerrainService()
    {
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
        TerrainService::Initialize();
    }
    ~ScopedTerrainService()
    {
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
    }
};

struct ScopedSplineService
{
    ScopedSplineService()
    {
        if (SplineECS::SplineService::IsInitialized())
            SplineECS::SplineService::Shutdown();
        SplineECS::SplineService::Initialize();
    }
    ~ScopedSplineService()
    {
        if (SplineECS::SplineService::IsInitialized())
            SplineECS::SplineService::Shutdown();
    }
};

constexpr float32 kWorldSize = 256.0f;
constexpr float32 kHeightScale = 64.0f;
constexpr uint32 kHeightmapDim = 129;
constexpr float32 kSampleSpacing = kWorldSize / static_cast<float32>(kHeightmapDim - 1); // 2 m

// World XZ -> sample index, for the hand-computed probes.
int32 SampleAt(float32 world)
{
    return static_cast<int32>(std::lround((world + kWorldSize * 0.5f) / kSampleSpacing));
}

Terrain::TerrainConfig MakeTestConfig()
{
    Terrain::TerrainConfig cfg{};
    cfg.HeightmapWidth = kHeightmapDim;
    cfg.HeightmapHeight = kHeightmapDim;
    cfg.WorldSizeX = kWorldSize;
    cfg.WorldSizeZ = kWorldSize;
    cfg.HeightScale = kHeightScale;
    cfg.LODLevels = 4;
    return cfg;
}

// Mirrors TerrainExtractionSystem's creation path, as the sibling suites do.
TerrainHandle CreateBakedTerrain(TerrainService& svc)
{
    const TerrainHandle handle = svc.CreateTerrain(MakeTestConfig());
    auto* data = svc.GetTerrainData(handle);
    FillHeightfieldBaseRegion(data->Heightfield, Components::TerrainBaseSource::ProceduralNoise,
                              nullptr, 0, 0,
                              static_cast<int32>(kHeightmapDim) - 1,
                              static_cast<int32>(kHeightmapDim) - 1);
    data->MarkFullDirty();
    svc.RebuildQuadtree(handle);
    data->ResetSplatmapAndCommitRange();
    return handle;
}

Components::WorldTransform IdentityAt(float32 x, float32 y, float32 z)
{
    Components::WorldTransform xf{};
    xf.matrix[12] = x;
    xf.matrix[13] = y;
    xf.matrix[14] = z;
    return xf;
}

ECS::EntityHandle CreateTerrainEntity(ECS::World& world, TerrainHandle handle)
{
    auto e = world.CreateEntity();
    Components::Terrain terrain{};
    terrain.SizeX = kWorldSize;
    terrain.SizeZ = kWorldSize;
    terrain.HeightScale = kHeightScale;
    terrain.TerrainDataHandle = handle.Index;
    terrain.TerrainDataGeneration = handle.Generation;
    terrain.BaseSource = Components::TerrainBaseSource::ProceduralNoise;
    world.AddComponentImmediate<Components::Terrain>(e, terrain);
    world.AddComponentImmediate<Components::WorldTransform>(e, IdentityAt(0.0f, 0.0f, 0.0f));
    return e;
}

ECS::EntityHandle CreateSplineEntity(ECS::World& world, SplineECS::SplineService& svc,
                                     const std::vector<Mathematics::Vector3>& points,
                                     float32 radius)
{
    const SplineECS::SplineHandle handle =
        svc.CreateSpline(Spline::SplineType::CatmullRom, false);
    auto* data = svc.GetSplineData(handle);
    for (const auto& p : points)
        data->AddPoint(p, radius);
    svc.RebuildCache(handle);

    auto e = world.CreateEntity();
    Components::SplineComponent comp{};
    comp.SplineDataIndex = handle.Index();
    comp.SplineDataGeneration = handle.Generation();
    world.AddComponentImmediate<Components::SplineComponent>(e, comp);
    world.AddComponentImmediate<Components::WorldTransform>(e, IdentityAt(0.0f, 0.0f, 0.0f));
    return e;
}

// The volume's own default arc-length resample. Read off the component rather
// than restated, because several tests below rebuild the polyline the gather
// built and must ask for exactly the geometry it did.
constexpr float32 kDefaultStationSpacing = Components::TerrainModifierVolume{}.StationSpacing;

// A spline volume with a hard edge (falloff 0), so the shape weight is a binary
// in/out mask and every composed height below is a whole number rather than a
// product of two ramps.
//
// StationSpacing lives HERE, on the volume, because it is geometry: the station
// polyline is a SplinePath volume's shape, feeding both the weight ramp and the
// reference height every flatten on it reads.
void AddSplineVolume(ECS::World& world, ECS::EntityHandle entity, float32 priority,
                     float32 falloff = 0.0f,
                     float32 stationSpacing = kDefaultStationSpacing)
{
    Components::TerrainModifierVolume volume{};
    volume.Shape = Components::TerrainVolumeShape::SplinePath;
    volume.Falloff = falloff;
    volume.Priority = priority;
    volume.StationSpacing = stationSpacing;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(entity, volume);
}

// The flatten a route is now authored as: Blend = Average puts it in a pool, and
// the pool group is the opt-OUT (blank joins the shared pool). UseVolumeHeight is
// what "follows its own heights" became — on a SplinePath volume the reference is
// the station polyline's own grade at the nearest station.
Components::TerrainFlattenEffect MakePooledFlatten(const char* poolGroup, bool respectClaims)
{
    Components::TerrainFlattenEffect fx{};
    std::memset(fx.PoolGroup, 0, sizeof(fx.PoolGroup));
    if (poolGroup)
        std::memcpy(fx.PoolGroup, poolGroup,
                    std::min(sizeof(fx.PoolGroup) - 1, std::strlen(poolGroup)));
    fx.Blend = Components::TerrainModifierBlend::Average;
    fx.UseVolumeHeight = true;
    fx.TargetHeight = 0.0f;
    fx.RespectClaims = respectClaims;
    return fx;
}

// One complete bake, returning heights in WORLD Y — the quantity every hand
// computation below is written in. The terrain entity sits at Y = 0, so this is
// `normalized * kHeightScale`.
struct BakeResult
{
    std::vector<float32> Base;
    std::vector<float32> Baked;

    float32 BakedAt(float32 worldX, float32 worldZ) const
    {
        return Baked[static_cast<std::size_t>(SampleAt(worldZ)) * kHeightmapDim + SampleAt(worldX)];
    }
    float32 BaseAt(float32 worldX, float32 worldZ) const
    {
        return Base[static_cast<std::size_t>(SampleAt(worldZ)) * kHeightmapDim + SampleAt(worldX)];
    }
};

BakeResult Bake(const std::function<void(ECS::World&, SplineECS::SplineService&)>& author)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    auto& terrainSvc = TerrainService::Get();
    auto& splineSvc = SplineECS::SplineService::Get();

    BakeResult result{};
    const TerrainHandle handle = CreateBakedTerrain(terrainSvc);
    auto* data = terrainSvc.GetTerrainData(handle);
    if (!data)
        return result;

    const float32* base = data->Heightfield.GetRawSamples();
    result.Base.assign(base, base + data->Heightfield.GetSampleCount());
    for (float32& v : result.Base)
        v *= kHeightScale;

    ECS::World world;
    CreateTerrainEntity(world, handle);
    author(world, splineSvc);

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    const float32* baked = data->Heightfield.GetRawSamples();
    result.Baked.assign(baked, baked + data->Heightfield.GetSampleCount());
    for (float32& v : result.Baked)
        v *= kHeightScale;
    return result;
}

// ---- The synthetic scene ---------------------------------------------------
//
// Two straight routes running along X, six metres either side of z = 0, at
// constant but DIFFERENT grades. Each has a swept half-width of 8 m and a hard
// edge, so the band z in [-14, +2] belongs to the low route and [-2, +14] to the
// high one, and the strip between z = -2 and z = +2 belongs to BOTH at full
// weight. That strip is where the two operators are visible.
//
// Crossing it at right angles is a road: a third spline along Z at x = 0 with
// the design's measured claim shape (radius 2.4, falloff 1.5), carrying a ground
// claim rather than any height.
constexpr float32 kLowRouteZ = -6.0f;
constexpr float32 kHighRouteZ = 6.0f;
constexpr float32 kLowGrade = 10.0f;
constexpr float32 kHighGrade = 20.0f;
// 7 m, deliberately not a whole number of sample spacings from either route:
// the volumes below carry a hard edge (falloff 0), so the shape weight is a step
// function, and a probe sitting EXACTLY on that step has its weight decided by
// float rounding rather than by the operator under test. At 7 m the edges fall
// at odd world coordinates, and every sample is a clear metre inside or outside.
constexpr float32 kRouteHalfWidth = 7.0f;
constexpr float32 kClaimRadius = 2.4f;  // LANE_HALF_WIDTH - 1.0, the sculpt's own mask
constexpr float32 kClaimFalloff = 1.5f; // (LANE_HALF_WIDTH + 0.5) - claim radius

// Where both routes reach full weight AND the claimed road passes: world (0, 0).
constexpr float32 kOverlapOnRoadX = 0.0f;
constexpr float32 kOverlapOnRoadZ = 0.0f;
// Where both routes reach full weight and the road is far away (20 m off, well
// past the claim's 3.9 m outer reach).
constexpr float32 kOverlapOffRoadX = 20.0f;
constexpr float32 kOverlapOffRoadZ = 0.0f;

// The average of the two grades — what the POOL operator must produce where the
// routes overlap, against the 20 m an overwriting stack produces instead.
constexpr float32 kAveragedGrade = 15.0f; // (10 + 20) / 2

void AuthorRoute(ECS::World& world, SplineECS::SplineService& svc, float32 z, float32 grade,
                 float32 priority, const char* poolGroup, bool respectClaims)
{
    const ECS::EntityHandle e = CreateSplineEntity(
        world, svc,
        {{-40.0f, grade, z}, {0.0f, grade, z}, {40.0f, grade, z}}, kRouteHalfWidth);
    AddSplineVolume(world, e, priority);
    world.AddComponentImmediate<Components::TerrainFlattenEffect>(
        e, MakePooledFlatten(poolGroup, respectClaims));
}

void AuthorClaimedRoad(ECS::World& world, SplineECS::SplineService& svc, float32 strength)
{
    const ECS::EntityHandle e = CreateSplineEntity(
        world, svc,
        {{0.0f, 0.0f, -40.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 40.0f}}, kClaimRadius);
    // Priority 0: the claim is in place before either route is applied.
    AddSplineVolume(world, e, 0.0f, kClaimFalloff);
    Components::TerrainGroundClaimEffect claim{};
    claim.Strength = strength;
    world.AddComponentImmediate<Components::TerrainGroundClaimEffect>(e, claim);
}

// The same route with an ORDINARY blend operator instead of Average: it applies
// in place at its own stack slot rather than joining a pool. RespectClaims is
// read here too — ownership is about who holds the ground, and the operator a
// region composes with says nothing about that.
Components::TerrainFlattenEffect MakeUnpooledFlatten(Components::TerrainModifierBlend blend,
                                                     bool respectClaims)
{
    Components::TerrainFlattenEffect fx{};
    std::memset(fx.PoolGroup, 0, sizeof(fx.PoolGroup));
    fx.Blend = blend;
    fx.UseVolumeHeight = true;
    fx.TargetHeight = 0.0f;
    fx.RespectClaims = respectClaims;
    return fx;
}

void AuthorUnpooledRoute(ECS::World& world, SplineECS::SplineService& svc, float32 z,
                         float32 grade, float32 priority,
                         Components::TerrainModifierBlend blend, bool respectClaims)
{
    const ECS::EntityHandle e = CreateSplineEntity(
        world, svc,
        {{-40.0f, grade, z}, {0.0f, grade, z}, {40.0f, grade, z}}, kRouteHalfWidth);
    AddSplineVolume(world, e, priority);
    world.AddComponentImmediate<Components::TerrainFlattenEffect>(
        e, MakeUnpooledFlatten(blend, respectClaims));
}

// The two routes — blending by default, or pushed apart into pools of their own
// by naming them differently — with or without the claimed road.
//
// `separated` is the OPT-OUT the pool group name is. Both arms author the same
// geometry at the same priorities; the only difference is whether the routes are
// allowed to share ground.
BakeResult BakeScene(bool separated, bool withClaim, bool respectClaims = true,
                     float32 claimStrength = 1.0f)
{
    return Bake([&](ECS::World& world, SplineECS::SplineService& svc) {
        if (withClaim)
            AuthorClaimedRoad(world, svc, claimStrength);
        AuthorRoute(world, svc, kLowRouteZ, kLowGrade, 10.0f,
                    separated ? "LowRoute" : nullptr, respectClaims);
        AuthorRoute(world, svc, kHighRouteZ, kHighGrade, 20.0f,
                    separated ? "HighRoute" : nullptr, respectClaims);
    });
}

constexpr float32 kHeightTolerance = 1e-3f; // millimetre, in world metres

// A float32 written so it reads back as the SAME float32. std::to_string gives
// six decimals, which silently rounds a station's coordinate — and the offline
// twin would then evaluate a polyline the engine never used, turning the parity
// gate into a comparison of two different geometries.
std::string ExactFloat(float32 value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
    return buffer;
}

} // namespace

// ---------------------------------------------------------------- geometry --

TEST(TerrainRouteGeometry, ResampleDividesTheRouteIntoEqualIntervals)
{
    ScopedSplineService splineScope;
    auto& svc = SplineECS::SplineService::Get();
    const SplineECS::SplineHandle handle =
        svc.CreateSpline(Spline::SplineType::CatmullRom, false);
    auto* data = svc.GetSplineData(handle);
    data->AddPoint({0.0f, 0.0f, 0.0f}, 3.0f);
    data->AddPoint({5.0f, 0.0f, 0.0f}, 3.0f);
    data->AddPoint({10.0f, 0.0f, 0.0f}, 3.0f);
    svc.RebuildCache(handle);

    RoutePolyline polyline;
    BuildRoutePolyline(*data, 1.0f, polyline);

    ASSERT_TRUE(polyline.IsValid());
    // ceil(10 / 1) = 10 intervals, so 11 stations.
    EXPECT_EQ(polyline.Stations.size(), 11u);
    EXPECT_NEAR(polyline.Stations.front().X, 0.0f, 1e-3f);
    EXPECT_NEAR(polyline.Stations.back().X, 10.0f, 1e-3f)
        << "the last station must be the route END, not the last whole multiple of the spacing";

    // Equal intervals, which is the rule the offline twin reproduces.
    for (std::size_t i = 1; i < polyline.Stations.size(); ++i)
    {
        const float32 step = polyline.Stations[i].X - polyline.Stations[i - 1].X;
        EXPECT_NEAR(step, 1.0f, 1e-3f) << "interval " << i;
    }
    EXPECT_NEAR(polyline.Stations[5].HalfWidth, 3.0f, 1e-4f)
        << "the swept radius must survive the resample";
}

TEST(TerrainRouteGeometry, SpacingIsAnUpperBoundAndIsClampedFromBelow)
{
    ScopedSplineService splineScope;
    auto& svc = SplineECS::SplineService::Get();
    const SplineECS::SplineHandle handle =
        svc.CreateSpline(Spline::SplineType::CatmullRom, false);
    auto* data = svc.GetSplineData(handle);
    data->AddPoint({0.0f, 0.0f, 0.0f}, 1.0f);
    data->AddPoint({3.0f, 0.0f, 0.0f}, 1.0f);
    svc.RebuildCache(handle);

    // A route that is not a whole number of steps long still ends on its end.
    RoutePolyline uneven;
    BuildRoutePolyline(*data, 2.0f, uneven);
    ASSERT_TRUE(uneven.IsValid());
    EXPECT_EQ(uneven.Stations.size(), 3u) << "ceil(3 / 2) = 2 intervals";
    EXPECT_NEAR(uneven.Stations.back().X, 3.0f, 1e-3f);
    EXPECT_NEAR(uneven.Stations[1].X, 1.5f, 1e-3f) << "equal intervals, not a 2 m step and a 1 m tail";

    // Scene text is untrusted: a zero or negative spacing is a station count of
    // infinity, and must clamp rather than hang.
    RoutePolyline degenerate;
    BuildRoutePolyline(*data, 0.0f, degenerate);
    ASSERT_TRUE(degenerate.IsValid());
    EXPECT_LE(degenerate.Stations.size(),
              static_cast<std::size_t>(3.0f / kMinRouteStationSpacing) + 2u);

    RoutePolyline negative;
    BuildRoutePolyline(*data, -5.0f, negative);
    EXPECT_EQ(negative.Stations.size(), degenerate.Stations.size())
        << "a negative spacing must land on the same floor a zero one does";
}

TEST(TerrainRouteGeometry, ClosestStationIsSegmentNearestAndInterpolatesTheGrade)
{
    RoutePolyline polyline;
    polyline.Stations = {{0.0f, 0.0f, 10.0f, 2.0f},   // (x, z, y, halfWidth)
                         {10.0f, 0.0f, 20.0f, 4.0f}};

    // Straight out from the middle of the only segment.
    const RouteSample middle = ClosestStationXZ(polyline, 5.0f, 3.0f);
    EXPECT_NEAR(middle.Distance, 3.0f, 1e-5f);
    EXPECT_NEAR(middle.Height, 15.0f, 1e-5f) << "grade interpolates linearly along the segment";
    EXPECT_NEAR(middle.HalfWidth, 3.0f, 1e-5f) << "so does the half-width";

    // Past the end: the projection CLAMPS to the segment rather than running off
    // the line, which is what makes a route stop at its end instead of grading
    // the ground beyond it.
    const RouteSample past = ClosestStationXZ(polyline, 14.0f, 0.0f);
    EXPECT_NEAR(past.Distance, 4.0f, 1e-5f);
    EXPECT_NEAR(past.Height, 20.0f, 1e-5f);

    // An unevaluable polyline contributes nothing rather than reading as "on the
    // route": +inf distance is what every weight ramp turns into a zero.
    RoutePolyline empty;
    EXPECT_TRUE(std::isinf(ClosestStationXZ(empty, 0.0f, 0.0f).Distance));
}

TEST(TerrainRouteGeometry, BoundsCoverEveryStationsSweptWidthPlusFalloff)
{
    RoutePolyline polyline;
    polyline.Stations = {{0.0f, 0.0f, 0.0f, 2.0f}, {10.0f, 4.0f, 0.0f, 3.0f}};

    float32 minX = 0.0f, minZ = 0.0f, maxX = 0.0f, maxZ = 0.0f;
    RoutePolylineBoundsXZ(polyline, 1.0f, minX, minZ, maxX, maxZ);
    EXPECT_NEAR(minX, -3.0f, 1e-5f);  // 0 - (2 + 1)
    EXPECT_NEAR(maxX, 14.0f, 1e-5f);  // 10 + (3 + 1)
    EXPECT_NEAR(minZ, -3.0f, 1e-5f);
    EXPECT_NEAR(maxZ, 8.0f, 1e-5f);   // 4 + (3 + 1)

    // An invalid polyline leaves an INVERTED box, which every bake loop reads as
    // "no samples" — the same answer an unresolved spline gives.
    RoutePolyline empty;
    RoutePolylineBoundsXZ(empty, 1.0f, minX, minZ, maxX, maxZ);
    EXPECT_GT(minX, maxX);
    EXPECT_GT(minZ, maxZ);
}

// --------------------------------------------------- operator 1: the pool --
//
// Pooled flattens BLEND BY DEFAULT. An author who wants a junction to average
// leaves the pool group blank; an author who wants two systems kept apart names
// them. These pin that direction, because it is the one an implementation can
// silently invert.

TEST(TerrainFlattenPool, UnnamedRoutesBlendWhereTheyOverlapWithoutBeingTold)
{
    const BakeResult byDefault = BakeScene(/*separated=*/false, /*withClaim=*/false);
    const BakeResult separated = BakeScene(/*separated=*/true, /*withClaim=*/false);

    // The point of the slice: neither route names a pool, and where both reach
    // full weight the ground lands on the average of their grades.
    EXPECT_NEAR(byDefault.BakedAt(kOverlapOffRoadX, kOverlapOffRoadZ), kAveragedGrade,
                kHeightTolerance)
        << "pooled flattens that were told nothing must blend; a blank pool group is the SHARED "
           "pool, not a pool of one";

    // The opt-out still exists and still overwrites: two pools of one, applied in
    // priority order, so the higher-priority route wins outright.
    EXPECT_NEAR(separated.BakedAt(kOverlapOffRoadX, kOverlapOffRoadZ), kHighGrade,
                kHeightTolerance);

    // And the two arms genuinely disagree, so neither number came from a bake
    // that did nothing.
    EXPECT_GT(std::abs(byDefault.BakedAt(kOverlapOffRoadX, kOverlapOffRoadZ)
                       - separated.BakedAt(kOverlapOffRoadX, kOverlapOffRoadZ)),
              1.0f);
}

TEST(TerrainFlattenPool, PoolingDoesNotChangeGroundOnlyOneRouteReaches)
{
    const BakeResult byDefault = BakeScene(/*separated=*/false, /*withClaim=*/false);
    const BakeResult separated = BakeScene(/*separated=*/true, /*withClaim=*/false);

    // z = -12 is inside the low route's band (|z - (-6)| = 6 < 7) and outside the
    // high route's (|z - 6| = 18). One member, so the average is that member —
    // whichever pool it sits in.
    EXPECT_NEAR(byDefault.BakedAt(kOverlapOffRoadX, -12.0f), kLowGrade, kHeightTolerance);
    EXPECT_NEAR(separated.BakedAt(kOverlapOffRoadX, -12.0f), kLowGrade, kHeightTolerance);

    // Beyond both bands the routes must not have touched the ground at all.
    EXPECT_NEAR(byDefault.BakedAt(kOverlapOffRoadX, 40.0f),
                byDefault.BaseAt(kOverlapOffRoadX, 40.0f), kHeightTolerance);
}

TEST(TerrainFlattenPool, DifferentPoolNamesDoNotAverageTogether)
{
    const BakeResult separate = Bake([](ECS::World& world, SplineECS::SplineService& svc) {
        AuthorRoute(world, svc, kLowRouteZ, kLowGrade, 10.0f, "RoutesA", true);
        AuthorRoute(world, svc, kHighRouteZ, kHighGrade, 20.0f, "RoutesB", true);
    });

    // Two pools of one, applied in priority order: the later route overwrites the
    // earlier. A name that resolved onto the shared pool would read 15 here.
    EXPECT_NEAR(separate.BakedAt(kOverlapOffRoadX, kOverlapOffRoadZ), kHighGrade,
                kHeightTolerance);
}

// Naming ONE of two routes is the asymmetric case: the named one leaves the
// shared pool, and what is left behind must still be a pool — of one — rather
// than reverting to an overwrite.
TEST(TerrainFlattenPool, NamingOneRouteSeparatesOnlyThatRoute)
{
    // The named route is the HIGH one at the higher priority, so its pool of one
    // applies last and overwrites the shared pool beneath it.
    const BakeResult namedHigh = Bake([](ECS::World& world, SplineECS::SplineService& svc) {
        AuthorRoute(world, svc, kLowRouteZ, kLowGrade, 10.0f, nullptr, true);
        AuthorRoute(world, svc, kHighRouteZ, kHighGrade, 20.0f, "Aqueduct", true);
    });
    EXPECT_NEAR(namedHigh.BakedAt(kOverlapOffRoadX, kOverlapOffRoadZ), kHighGrade,
                kHeightTolerance);

    // Naming the LOW one instead leaves the high route alone in the shared pool,
    // and the shared pool is the higher-priority slot, so it wins.
    const BakeResult namedLow = Bake([](ECS::World& world, SplineECS::SplineService& svc) {
        AuthorRoute(world, svc, kLowRouteZ, kLowGrade, 10.0f, "Aqueduct", true);
        AuthorRoute(world, svc, kHighRouteZ, kHighGrade, 20.0f, nullptr, true);
    });
    EXPECT_NEAR(namedLow.BakedAt(kOverlapOffRoadX, kOverlapOffRoadZ), kHighGrade,
                kHeightTolerance);

    // Where only the named route reaches, it still grades — a pool of one is a
    // route, not a disabled one.
    EXPECT_NEAR(namedHigh.BakedAt(kOverlapOffRoadX, 12.0f), kHighGrade, kHeightTolerance);
}

// ------------------------------------------------------- operator 2: claim --

TEST(TerrainPoolClaim, PooledFlattensStopAtGroundAnotherVolumeOwns)
{
    const BakeResult claimed = BakeScene(/*separated=*/false, /*withClaim=*/true);

    // GREEN: on the road, the claim is full and BOTH members respect it, so every
    // term of the masked weight sum is zero, the blend is zero, and the ground is
    // exactly what the base heightfield had.
    EXPECT_NEAR(claimed.BakedAt(kOverlapOnRoadX, kOverlapOnRoadZ),
                claimed.BaseAt(kOverlapOnRoadX, kOverlapOnRoadZ), kHeightTolerance);

    // Off the road the same routes grade normally, so the claim is a local
    // mask rather than a switch that turned the routes off.
    EXPECT_NEAR(claimed.BakedAt(kOverlapOffRoadX, kOverlapOffRoadZ), kAveragedGrade,
                kHeightTolerance);
}

TEST(TerrainPoolClaim, ClaimOffLetsThePoolGradeOwnedGround)
{
    const BakeResult respecting = BakeScene(/*separated=*/false, /*withClaim=*/true,
                                            /*respectClaims=*/true);
    const BakeResult ignoring = BakeScene(/*separated=*/false, /*withClaim=*/true,
                                          /*respectClaims=*/false);

    // RED: with the claim ignored by every member, the pool cuts straight through
    // the road at the averaged grade — the artifact the operator exists to
    // prevent.
    EXPECT_NEAR(ignoring.BakedAt(kOverlapOnRoadX, kOverlapOnRoadZ), kAveragedGrade,
                kHeightTolerance);

    // The two arms differ by metres on the claimed ground, and agree off it.
    EXPECT_GT(std::abs(ignoring.BakedAt(kOverlapOnRoadX, kOverlapOnRoadZ)
                       - respecting.BakedAt(kOverlapOnRoadX, kOverlapOnRoadZ)),
              1.0f);
    EXPECT_NEAR(ignoring.BakedAt(kOverlapOffRoadX, kOverlapOffRoadZ),
                respecting.BakedAt(kOverlapOffRoadX, kOverlapOffRoadZ), kHeightTolerance);
}

TEST(TerrainPoolClaim, AClaimWritesNoHeightOfItsOwn)
{
    // A claim volume alone, over ground nothing else touches. A claim that wrote
    // height — the round-4 failure this effect is shaped to avoid — would level
    // the road to its own reference and show up here.
    const BakeResult claimOnly = Bake([](ECS::World& world, SplineECS::SplineService& svc) {
        AuthorClaimedRoad(world, svc, 1.0f);
    });

    ASSERT_EQ(claimOnly.Base.size(), claimOnly.Baked.size());
    ASSERT_FALSE(claimOnly.Base.empty());
    EXPECT_EQ(std::memcmp(claimOnly.Base.data(), claimOnly.Baked.data(),
                          claimOnly.Base.size() * sizeof(float32)),
              0)
        << "a ground claim must leave the heightfield byte-identical";
}

TEST(TerrainPoolClaim, ClaimsCombineByMaxNotBySum)
{
    // Two claimants at 0.6 over the same road. MAX leaves 0.6 owned, so the
    // pool still moves the ground 40% of the way to the averaged grade. SUM
    // would leave 1.2, a negative blend, and the ground untouched — so the two
    // rules are told apart by whether this sample moved at all.
    const BakeResult twoClaims = Bake([](ECS::World& world, SplineECS::SplineService& svc) {
        AuthorClaimedRoad(world, svc, 0.6f);
        AuthorClaimedRoad(world, svc, 0.6f);
        AuthorRoute(world, svc, kLowRouteZ, kLowGrade, 10.0f, "Approaches", true);
        AuthorRoute(world, svc, kHighRouteZ, kHighGrade, 20.0f, "Approaches", true);
    });

    const float32 base = twoClaims.BaseAt(kOverlapOnRoadX, kOverlapOnRoadZ);
    const float32 expected = base + (kAveragedGrade - base) * 0.4f; // blend = 1 - max(0.6, 0.6)
    EXPECT_NEAR(twoClaims.BakedAt(kOverlapOnRoadX, kOverlapOnRoadZ), expected, kHeightTolerance);
    EXPECT_GT(std::abs(twoClaims.BakedAt(kOverlapOnRoadX, kOverlapOnRoadZ) - base), 0.5f)
        << "summed claims would have left this sample untouched";
}

// ---------------------------------------------------- surrounding contracts --

TEST(TerrainFlattenPoolContracts, APoolOfOneReducesToTheUnpooledFlattenOnAStraightRoute)
{
    // On a straight route the station polyline and the curve behind it are the
    // same line, so the only thing under test is the COMPOSITION: one pooled
    // flatten alone in the shared pool, with no claims present, must be the plain
    // Set flatten it claims to reduce to.
    //
    // This is the reduction that lets the pool accumulator be the ONLY path.
    // (sum w*t) / (sum w) with one term is t to within one ulp of the divide,
    // which is four orders below the millimetre tolerance below.
    const auto authorWith = [](bool pooled) {
        return [pooled](ECS::World& world, SplineECS::SplineService& svc) {
            const ECS::EntityHandle e = CreateSplineEntity(
                world, svc,
                {{-40.0f, kLowGrade, 0.0f}, {0.0f, kLowGrade, 0.0f}, {40.0f, kLowGrade, 0.0f}},
                7.0f);
            AddSplineVolume(world, e, 10.0f);
            if (pooled)
            {
                world.AddComponentImmediate<Components::TerrainFlattenEffect>(
                    e, MakePooledFlatten(nullptr, true));
            }
            else
            {
                Components::TerrainFlattenEffect flatten{};
                flatten.UseVolumeHeight = true;
                flatten.TargetHeight = 0.0f;
                flatten.Blend = Components::TerrainModifierBlend::Set;
                world.AddComponentImmediate<Components::TerrainFlattenEffect>(e, flatten);
            }
        };
    };

    const BakeResult unpooled = Bake(authorWith(false));
    const BakeResult poolOfOne = Bake(authorWith(true));

    ASSERT_EQ(unpooled.Baked.size(), poolOfOne.Baked.size());
    float32 worst = 0.0f;
    for (std::size_t i = 0; i < unpooled.Baked.size(); ++i)
        worst = std::max(worst, std::abs(unpooled.Baked[i] - poolOfOne.Baked[i]));
    EXPECT_LT(worst, kHeightTolerance)
        << "a pool of one on a straight route must be the plain flatten; worst delta " << worst;

    // Both actually did something, so the agreement is not two no-ops matching.
    EXPECT_NEAR(poolOfOne.BakedAt(0.0f, 0.0f), kLowGrade, kHeightTolerance);
}

// The CAPABILITY GAIN the dissolution brings (design row 8). The corridor was
// refused outright on a non-spline volume, because "follow route heights" had no
// route to read. A flatten's UseVolumeHeight is defined for EVERY shape — the
// entity's own Y off a spline — so a pooled flatten on a circle is meaningful,
// and two overlapping pads average instead of the later one overwriting the
// earlier and leaving a step at its rim.
//
// This is what the deleted refusal oracle becomes: it asserts the arrangement the
// refusal made unexpressible.
TEST(TerrainFlattenPoolContracts, PooledPadsOnCircleVolumesAverageWhereTheyOverlap)
{
    // Two circular pads on the ground plane, 20 m apart, radius 30 with a hard
    // edge. Their entity Y IS the pad height (UseVolumeHeight off a spline reads
    // the entity's translation), so pad A grades to 10 and pad B to 20.
    const auto authorPads = [](const char* poolA, const char* poolB) {
        return [poolA, poolB](ECS::World& world, SplineECS::SplineService&) {
            const auto addPad = [&world](float32 x, float32 y, float32 priority,
                                         const char* pool) {
                auto e = world.CreateEntity();
                world.AddComponentImmediate<Components::WorldTransform>(e, IdentityAt(x, y, 0.0f));
                Components::TerrainModifierVolume volume{};
                volume.Shape = Components::TerrainVolumeShape::Circle;
                volume.Radius = 30.0f;
                volume.Falloff = 0.0f;
                volume.Priority = priority;
                world.AddComponentImmediate<Components::TerrainModifierVolume>(e, volume);
                world.AddComponentImmediate<Components::TerrainFlattenEffect>(
                    e, MakePooledFlatten(pool, /*respectClaims=*/false));
            };
            addPad(0.0f, kLowGrade, 10.0f, poolA);
            addPad(20.0f, kHighGrade, 20.0f, poolB);
        };
    };

    // Probe (10, 0): 10 m from pad A's centre and 10 m from pad B's, so both
    // reach full weight. Probe (-20, 0): inside A (20 < 30), outside B (40 > 30).
    const BakeResult shared = Bake(authorPads(nullptr, nullptr));
    EXPECT_NEAR(shared.BakedAt(10.0f, 0.0f), kAveragedGrade, kHeightTolerance)
        << "two pooled pads must average where they overlap — the arrangement the corridor's "
           "non-spline refusal could not express at all";
    EXPECT_NEAR(shared.BakedAt(-20.0f, 0.0f), kLowGrade, kHeightTolerance)
        << "where only one pad reaches, the pool of that one member is that member";

    // The opt-out reads the same on a circle as on a route: named apart, the
    // higher-priority pad overwrites rather than averaging.
    const BakeResult separated = Bake(authorPads("PadA", "PadB"));
    EXPECT_NEAR(separated.BakedAt(10.0f, 0.0f), kHighGrade, kHeightTolerance);
    EXPECT_GT(std::abs(shared.BakedAt(10.0f, 0.0f) - separated.BakedAt(10.0f, 0.0f)), 1.0f)
        << "the two arms must disagree, or neither number came from a bake that pooled";
}

TEST(TerrainFlattenPoolContracts, EditingAPooledFlattenReBakesAndBumpsTheSurfaceVersion)
{
    // The conform chain that re-beds placement on an edited route starts at
    // HeightfieldVersion (SplineSurfaceConform::ConformSurfaceRevision folds it).
    // A flatten parameter that moves the bake but not the geometry hash would
    // leave the version still, and the paving would settle on stale ground.
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    auto& terrainSvc = TerrainService::Get();
    auto& splineSvc = SplineECS::SplineService::Get();

    const TerrainHandle handle = CreateBakedTerrain(terrainSvc);
    auto* data = terrainSvc.GetTerrainData(handle);
    ASSERT_NE(data, nullptr);

    ECS::World world;
    CreateTerrainEntity(world, handle);

    const ECS::EntityHandle route = CreateSplineEntity(
        world, splineSvc,
        {{-40.0f, kLowGrade, 0.0f}, {0.0f, kLowGrade, 0.0f}, {40.0f, kLowGrade, 0.0f}},
        kRouteHalfWidth);
    AddSplineVolume(world, route, 10.0f);
    world.AddComponentImmediate<Components::TerrainFlattenEffect>(
        route, MakePooledFlatten(nullptr, true));

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    const uint64 versionAfterFirstBake = data->HeightfieldVersion;
    const float32 heightAfterFirstBake =
        data->Heightfield.GetRawSamples()[static_cast<std::size_t>(SampleAt(0.0f)) * kHeightmapDim
                                          + SampleAt(0.0f)] * kHeightScale;
    EXPECT_NEAR(heightAfterFirstBake, kLowGrade, kHeightTolerance);

    // Edit the flatten's own parameter — not the volume, not the spline.
    auto* effect = world.GetComponentForWrite<Components::TerrainFlattenEffect>(route);
    ASSERT_NE(effect, nullptr);
    effect->UseVolumeHeight = false;
    effect->TargetHeight = kHighGrade;

    system.Update(world, 1.0f / 60.0f);

    EXPECT_GT(data->HeightfieldVersion, versionAfterFirstBake)
        << "a flatten edit must bump the surface version the conform digest folds";
    const float32 heightAfterEdit =
        data->Heightfield.GetRawSamples()[static_cast<std::size_t>(SampleAt(0.0f)) * kHeightmapDim
                                          + SampleAt(0.0f)] * kHeightScale;
    EXPECT_NEAR(heightAfterEdit, kHighGrade, kHeightTolerance)
        << "the edit must reach the bake, not just the version";
}

TEST(TerrainFlattenPoolContracts, RegionBakeOfAMovedRouteMatchesAFullBake)
{
    // Both operators need scratch the modifier-major bake loop does not
    // otherwise have — a claim outlives the modifier that wrote it, and a
    // pool's members accumulate before any of them applies. Scratch scoped to
    // the BAKE REGION is where that goes wrong: a region re-bake must still be
    // bit-identical to a fresh full bake of the same final state, or an edit
    // leaves a seam at the region boundary that only shows up on content.
    //
    // Both members reach this region and nothing sits between them in priority,
    // so this says nothing about WHERE a pool's single blend lands. The slot is
    // pinned separately, by RegionBakeWithAMemberOutsideTheRegionMatchesFullBake
    // and the three pool-order tests below it.
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    auto& terrainSvc = TerrainService::Get();
    auto& splineSvc = SplineECS::SplineService::Get();

    constexpr float32 kMovedZ = -14.0f; // how far the low route's entity slides

    const auto authorArm = [&](ECS::World& world, bool moveAfterAuthoring) {
        AuthorClaimedRoad(world, splineSvc, 1.0f);
        const ECS::EntityHandle lowRoute = CreateSplineEntity(
            world, splineSvc,
            {{-40.0f, kLowGrade, kLowRouteZ}, {0.0f, kLowGrade, kLowRouteZ},
             {40.0f, kLowGrade, kLowRouteZ}}, kRouteHalfWidth);
        AddSplineVolume(world, lowRoute, 10.0f);
        world.AddComponentImmediate<Components::TerrainFlattenEffect>(
            lowRoute, MakePooledFlatten("Approaches", true));
        AuthorRoute(world, splineSvc, kHighRouteZ, kHighGrade, 20.0f, "Approaches", true);
        if (moveAfterAuthoring)
        {
            auto* xf = world.GetComponentForWrite<Components::WorldTransform>(lowRoute);
            xf->matrix[14] = kMovedZ;
        }
        return lowRoute;
    };

    // Arm A: bake, then slide one route and re-bake — the region path.
    const TerrainHandle handleA = CreateBakedTerrain(terrainSvc);
    auto* dataA = terrainSvc.GetTerrainData(handleA);
    ASSERT_NE(dataA, nullptr);

    ECS::World worldA;
    CreateTerrainEntity(worldA, handleA);
    const ECS::EntityHandle lowRoute = authorArm(worldA, /*moveAfterAuthoring=*/false);

    TerrainModifierSystem systemA;
    systemA.Update(worldA, 1.0f / 60.0f); // first bake: full, no baseline yet

    dataA->SplatmapDirty = false;
    const uint64 cursor = dataA->HeightfieldVersion;

    auto* xf = worldA.GetComponentForWrite<Components::WorldTransform>(lowRoute);
    ASSERT_NE(xf, nullptr);
    xf->matrix[14] = kMovedZ;

    systemA.Update(worldA, 1.0f / 60.0f);

    // The second bake must actually be region-scoped, or this proves nothing
    // about scratch that is scoped to a region.
    DirtyRegionLog::Region dirty;
    ASSERT_TRUE(dataA->HeightfieldDirtyLog.CollectSince(cursor, dirty));
    EXPECT_GT(dirty.MinZ, 0);
    EXPECT_LT(dirty.MaxZ, static_cast<int32>(kHeightmapDim));

    // Arm B: a fresh terrain baked once, with the route already where A slid it.
    const TerrainHandle handleB = CreateBakedTerrain(terrainSvc);
    auto* dataB = terrainSvc.GetTerrainData(handleB);
    ASSERT_NE(dataB, nullptr);

    ECS::World worldB;
    CreateTerrainEntity(worldB, handleB);
    authorArm(worldB, /*moveAfterAuthoring=*/true);

    TerrainModifierSystem systemB;
    systemB.Update(worldB, 1.0f / 60.0f);

    ASSERT_EQ(dataA->Heightfield.GetSampleCount(), dataB->Heightfield.GetSampleCount());
    EXPECT_EQ(0, std::memcmp(dataA->Heightfield.GetRawSamples(),
                             dataB->Heightfield.GetRawSamples(),
                             dataA->Heightfield.GetSampleCount() * sizeof(float32)))
        << "a region re-bake of a moved route must be bit-identical to a full bake";
}

// ------------------------------------------------------------------ timing --

// What pooling costs the CPU bake against the plain flatten-on-a-spline it
// composes with. Disabled by default and run explicitly, like the suite's other
// timing oracles: it is a measurement, not a gate, and a threshold here would
// only encode whichever machine ran it last.
//
//   TerrainRegionBakeTests.exe --gtest_also_run_disabled_tests \
//       --gtest_filter=TerrainFlattenPoolTiming.DISABLED_PooledVsPlainFlattenFullBake
//
// Release only for a number worth quoting: at /Od the per-texel station scan and
// the spline SDF are both unoptimised, and not by the same factor.
TEST(TerrainFlattenPoolTiming, DISABLED_PooledVsPlainFlattenFullBake)
{
    constexpr int kRoutes = 12;   // a large island scene's approach count
    constexpr int kRepeats = 20;

    // Three arms. Every pooled flatten pays an accumulator — there is no in-place
    // arm beside it. "shared" is what content actually produces (one pool, twelve
    // members); "separated" is the worst case that shape can reach — twelve pools
    // of ONE, each allocating and scanning its own scratch rect to carry a single
    // term.
    enum class Arm { PlainFlatten, SharedPool, SeparatePools };
    const auto timeBake = [&](Arm arm) {
        ScopedTerrainService terrainScope;
        ScopedSplineService splineScope;
        auto& terrainSvc = TerrainService::Get();
        auto& splineSvc = SplineECS::SplineService::Get();

        double best = 1e30;
        for (int repeat = 0; repeat < kRepeats; ++repeat)
        {
            const TerrainHandle handle = CreateBakedTerrain(terrainSvc);
            ECS::World world;
            CreateTerrainEntity(world, handle);
            for (int r = 0; r < kRoutes; ++r)
            {
                // Routes fanned across the terrain, each resampled at 0.5 m like
                // an authored scene's, so the station count per route is realistic.
                const float32 z = -60.0f + static_cast<float32>(r) * 10.0f;
                const ECS::EntityHandle e = CreateSplineEntity(
                    world, splineSvc,
                    {{-50.0f, kLowGrade, z}, {0.0f, kLowGrade, z + 4.0f},
                     {50.0f, kLowGrade, z}}, kRouteHalfWidth);
                AddSplineVolume(world, e, 10.0f + static_cast<float32>(r), 7.0f);
                if (arm != Arm::PlainFlatten)
                {
                    char separateName[16] = {};
                    std::snprintf(separateName, sizeof(separateName), "Route%d", r);
                    world.AddComponentImmediate<Components::TerrainFlattenEffect>(
                        e, MakePooledFlatten(arm == Arm::SharedPool ? nullptr : separateName, true));
                }
                else
                {
                    Components::TerrainFlattenEffect flatten{};
                    flatten.UseVolumeHeight = true;
                    flatten.Blend = Components::TerrainModifierBlend::Set;
                    world.AddComponentImmediate<Components::TerrainFlattenEffect>(e, flatten);
                }
            }

            TerrainModifierSystem system;
            const auto start = std::chrono::steady_clock::now();
            system.Update(world, 1.0f / 60.0f);
            const auto end = std::chrono::steady_clock::now();
            // BEST of the repeats, not the mean: the minimum is the run least
            // disturbed by whatever else the machine was doing, and this box is
            // not guaranteed quiet.
            best = std::min(best, std::chrono::duration<double, std::milli>(end - start).count());
        }
        return best;
    };

    const double flattenMs = timeBake(Arm::PlainFlatten);
    const double sharedMs = timeBake(Arm::SharedPool);
    const double separateMs = timeBake(Arm::SeparatePools);
    std::printf("[ timing ] %d routes over %u^2 samples, best of %d full bakes\n",
                kRoutes, kHeightmapDim, kRepeats);
    std::printf("[ timing ]   plain flatten (Set)    %8.3f ms\n", flattenMs);
    std::printf("[ timing ]   pooled shared pool     %8.3f ms   delta %+8.3f ms (%+.1f%%)\n",
                sharedMs, sharedMs - flattenMs, 100.0 * (sharedMs - flattenMs) / flattenMs);
    std::printf("[ timing ]   pooled 12 pools of 1   %8.3f ms   delta %+8.3f ms (%+.1f%%)\n",
                separateMs, separateMs - flattenMs,
                100.0 * (separateMs - flattenMs) / flattenMs);
    SUCCEED();
}

// --------------------------------------------------- the offline-twin gate --

TEST(TerrainPoolParity, EmitsTheFixtureTheOfflineTwinIsHeldTo)
{
    // The arc's recurring failure class is the offline instrument and the engine
    // drifting apart while both look right — rounds 1 to 4 held them together by
    // reading source and transcribing, which is how a 1.1 cm offset survived four
    // of them. This writes what the engine ACTUALLY composed on the synthetic
    // scene: the station polylines its bake evaluated, and the base and baked
    // height at a line of probes across the crossing. The content
    // project's Tools/pool_parity.py recomputes the baked column from the stations and the
    // base column and must reproduce every row; its committed copy of this file
    // is Tools/fixtures/pool_parity.json.
    //
    // MIXED POOL MEMBERSHIP IS THE POINT, and it is why the two routes below
    // disagree about claims. Per-member masking only differs from the pool-wide
    // OR it replaced where ONE pool holds members that answer differently: with
    // every member respecting claims, maskedWsum is (1 - claim) * wsum and the
    // blend collapses to the old formula BIT-IDENTICALLY. A uniform fixture
    // therefore scores zero delta against BOTH operators and its green says
    // nothing at all about the change — which is what the twin lane measured on
    // the pre-dissolution fixture (max delta 0.000e+00 over 651 probes).
    // pool_parity.py reports the count as "pools with mixed claim"; this test
    // asserts it rather than leaving it to be read.
    //
    // The stations are rebuilt here with the same BuildRoutePolyline call and the
    // same volume StationSpacing the gather makes, which is deterministic — the
    // one link in the chain that is argued rather than read out of the bake.
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    auto& terrainSvc = TerrainService::Get();
    auto& splineSvc = SplineECS::SplineService::Get();

    const TerrainHandle handle = CreateBakedTerrain(terrainSvc);
    auto* data = terrainSvc.GetTerrainData(handle);
    ASSERT_NE(data, nullptr);

    std::vector<float32> baseWorld(data->Heightfield.GetSampleCount());
    for (std::size_t i = 0; i < baseWorld.size(); ++i)
        baseWorld[i] = data->Heightfield.GetRawSamples()[i] * kHeightScale;

    ECS::World world;
    CreateTerrainEntity(world, handle);
    AuthorClaimedRoad(world, splineSvc, 1.0f);

    // A SECOND claimed road, whose only job is to put probes inside a claim's
    // FALLOFF RAMP. The road above is centred on x = 0 with a 2.4 m radius and a
    // 1.5 m feather, so its ramp lives at |x| in (2.4, 3.9) — and the sample grid
    // is 2 m, on even world coordinates, so not one probe of the fixture lands in
    // it. Every probe reads a claim of exactly 0 or exactly 1, and a mask that
    // drifted anywhere in between would pass the parity gate untouched.
    //
    // Centred off the grid (x = 9.5) with a wider feather, so five sample columns
    // fall in its ramp at five different weights, all of them clear of the first
    // road's reach. It stays below both routes in priority, and stops well short
    // of both probes the hand-computed numbers below are read at.
    //
    // CONTROL POINTS EVERY 2 m, not three across 80 m. The claim volume is an
    // ordinary SplinePath volume, so its shape ramp reads the SAME station
    // polyline a route does, and the content tool emits these roads resampled at
    // 0.5 m — a road with three control points is the unfaithful thing here.
    {
        constexpr float32 kRampClaimX = 9.5f;
        constexpr float32 kRampClaimFalloff = 5.0f;
        constexpr float32 kRampClaimSpacing = 2.0f;
        std::vector<Mathematics::Vector3> rampPoints;
        for (float32 z = -40.0f; z <= 40.0f; z += kRampClaimSpacing)
            rampPoints.push_back({kRampClaimX, 0.0f, z});
        const ECS::EntityHandle rampClaim =
            CreateSplineEntity(world, splineSvc, rampPoints, kClaimRadius);
        AddSplineVolume(world, rampClaim, 0.0f, kRampClaimFalloff);
        Components::TerrainGroundClaimEffect claim{};
        claim.Strength = 1.0f;
        world.AddComponentImmediate<Components::TerrainGroundClaimEffect>(rampClaim, claim);
    }

    // BOTH UNNAMED, so they share the default pool — what content authors now
    // write, and the pool the twin will meet in the field. An offline model that
    // still keyed pools by "has a name" would put these two routes in pools of one
    // and overwrite, which the averaged-grade assertion below catches from the
    // engine side and the twin's own run catches from the other.
    //
    // They DISAGREE about claims: the low route owns its ground and grades
    // through, the high route defers. That is the mixed membership above, and it
    // is what makes the on-road probe a half blend rather than the base.
    AuthorRoute(world, splineSvc, kLowRouteZ, kLowGrade, 10.0f, nullptr,
                /*respectClaims=*/false);
    AuthorRoute(world, splineSvc, kHighRouteZ, kHighGrade, 20.0f, nullptr,
                /*respectClaims=*/true);

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    // The four volumes, in the order they were authored above.
    struct FixtureSpline
    {
        std::string Kind;
        std::string PoolGroup;
        float32 Falloff = 0.0f;
        float32 Priority = 0.0f;
        float32 VolumeWeight = 1.0f;
        float32 Strength = 0.0f;     // claims only
        float32 TargetHeight = 0.0f; // flattens only
        bool UseVolumeHeight = true;
        bool RespectClaims = true;
        RoutePolyline Polyline;
    };
    std::vector<FixtureSpline> splines;
    world.Query<ECS::Read<Components::SplineComponent>,
                ECS::Read<Components::TerrainModifierVolume>>()
        .Each([&](ECS::EntityHandle e, const Components::SplineComponent& sc,
                  const Components::TerrainModifierVolume& vol) {
            const SplineECS::SplineHandle h{sc.SplineDataIndex, sc.SplineDataGeneration};
            const Spline::SplineData* spline = splineSvc.GetSplineData(h);
            if (!spline)
                return;
            FixtureSpline entry{};
            entry.Falloff = vol.Falloff;
            entry.Priority = vol.Priority;
            entry.VolumeWeight = vol.Weight;
            // ONE geometry for both kinds. The station polyline is every
            // SplinePath volume's shape now, so a claim's ramp and a flatten's
            // weight read the same object at the same spacing, and the
            // curve-vs-polyline residual that used to land in the mask is zero by
            // construction. Resampling a claim finer than its volume asked for is
            // what left the twin evaluating a curve the engine never did.
            BuildRoutePolyline(*spline, vol.StationSpacing, entry.Polyline);
            if (const auto* claim = world.GetComponent<Components::TerrainGroundClaimEffect>(e))
            {
                entry.Kind = "claim";
                entry.Strength = claim->Strength;
            }
            else if (const auto* flatten = world.GetComponent<Components::TerrainFlattenEffect>(e))
            {
                entry.Kind = "flatten";
                entry.PoolGroup = std::string(Components::EffectPoolName(*flatten));
                entry.TargetHeight = flatten->TargetHeight;
                entry.UseVolumeHeight = flatten->UseVolumeHeight;
                entry.RespectClaims = flatten->RespectClaims;
            }
            else
            {
                return;
            }
            splines.push_back(std::move(entry));
        });
    ASSERT_EQ(splines.size(), 4u);

    // The emitted fixture must exercise the operator it gates. One pool holding
    // members that disagree about claims is the only arrangement where per-member
    // masking and the pool-wide OR it replaced compute different numbers.
    int mixedPools = 0;
    for (std::size_t g = 0; g < splines.size(); ++g)
    {
        if (splines[g].Kind != "flatten")
            continue;
        bool anyRespect = false;
        bool anyIgnore = false;
        bool firstOfPool = true;
        for (std::size_t m = 0; m < splines.size(); ++m)
        {
            if (splines[m].Kind != "flatten" || splines[m].PoolGroup != splines[g].PoolGroup)
                continue;
            if (m < g)
            {
                firstOfPool = false; // already counted when its first member was reached
                break;
            }
            (splines[m].RespectClaims ? anyRespect : anyIgnore) = true;
        }
        if (firstOfPool && anyRespect && anyIgnore)
            ++mixedPools;
    }
    EXPECT_GE(mixedPools, 1)
        << "the emitted fixture has no pool whose members disagree about claims, so the twin's "
           "run scores identically under the pool-wide OR this replaced and its green says "
           "nothing about per-member masking";

    // A probe line straight across the crossing, from deep inside the claimed
    // road out to ground no route reaches.
    //
    // "schema" is the emitter contract pool_parity.py refuses to run without: a
    // fixture missing it predates per-member masking, and comparing against one
    // would score the new model against the old engine.
    std::string json = "{\n";
    json += "  \"schema\": \"pooled-flatten-1\",\n";
    json += "  \"heightScale\": " + ExactFloat(kHeightScale) + ",\n";
    json += "  \"lowGrade\": " + ExactFloat(kLowGrade) + ",\n";
    json += "  \"highGrade\": " + ExactFloat(kHighGrade) + ",\n";
    json += "  \"splines\": [\n";
    for (std::size_t s = 0; s < splines.size(); ++s)
    {
        json += "    {\"kind\": \"" + splines[s].Kind + "\""
              + ", \"poolGroup\": \"" + splines[s].PoolGroup + "\""
              + ", \"falloff\": " + ExactFloat(splines[s].Falloff)
              + ", \"priority\": " + ExactFloat(splines[s].Priority)
              + ", \"volumeWeight\": " + ExactFloat(splines[s].VolumeWeight)
              + ", \"strength\": " + ExactFloat(splines[s].Strength)
              + ", \"targetHeight\": " + ExactFloat(splines[s].TargetHeight)
              + ", \"useVolumeHeight\": " + (splines[s].UseVolumeHeight ? "true" : "false")
              + ", \"respectClaims\": " + (splines[s].RespectClaims ? "true" : "false")
              + ", \"stations\": [";
        for (std::size_t i = 0; i < splines[s].Polyline.Stations.size(); ++i)
        {
            const RouteStation& st = splines[s].Polyline.Stations[i];
            json += (i ? ", [" : "[") + ExactFloat(st.X) + ", " + ExactFloat(st.Z) + ", "
                  + ExactFloat(st.Y) + ", " + ExactFloat(st.HalfWidth) + "]";
        }
        json += "]}";
        json += (s + 1 < splines.size()) ? ",\n" : "\n";
    }
    json += "  ],\n  \"probes\": [\n";

    bool firstProbe = true;
    for (int32 sx = SampleAt(-30.0f); sx <= SampleAt(30.0f); ++sx)
    {
        for (int32 sz = SampleAt(-20.0f); sz <= SampleAt(20.0f); ++sz)
        {
            const std::size_t idx = static_cast<std::size_t>(sz) * kHeightmapDim + sx;
            const float32 worldX = -kWorldSize * 0.5f + static_cast<float32>(sx) * kSampleSpacing;
            const float32 worldZ = -kWorldSize * 0.5f + static_cast<float32>(sz) * kSampleSpacing;
            const float32 baked = data->Heightfield.GetRawSamples()[idx] * kHeightScale;
            json += firstProbe ? "    " : ",\n    ";
            firstProbe = false;
            json += "{\"x\": " + ExactFloat(worldX) + ", \"z\": " + ExactFloat(worldZ)
                  + ", \"base\": " + ExactFloat(baseWorld[idx])
                  + ", \"baked\": " + ExactFloat(baked) + "}";
        }
    }
    json += "\n  ]\n}\n";

    // Written into the test working directory (CMAKE_BINARY_DIR); the reviewed
    // copy lives beside the twin that asserts on it, at Tools/fixtures/.
    FILE* file = std::fopen("pool_parity.json", "wb");
    ASSERT_NE(file, nullptr);
    std::fwrite(json.data(), 1, json.size(), file);
    std::fclose(file);

    // The fixture is only as good as the bake behind it, so it carries the
    // hand-computed numbers the operators are defined by.
    const auto worldAt = [&](float32 x, float32 z) {
        return data->Heightfield.GetRawSamples()[static_cast<std::size_t>(SampleAt(z))
                                                     * kHeightmapDim + SampleAt(x)]
             * kHeightScale;
    };
    const auto baseAt = [&](float32 x, float32 z) {
        return baseWorld[static_cast<std::size_t>(SampleAt(z)) * kHeightmapDim + SampleAt(x)];
    };

    // Off the road, claim = 0, so both members contribute their full weight:
    // wsum = 2, maskedWsum = 2, blend = min(2,1) * 1 = 1, target = 15.
    EXPECT_NEAR(worldAt(kOverlapOffRoadX, kOverlapOffRoadZ), kAveragedGrade, kHeightTolerance);

    // On the road, claim = 1, and the pool is MIXED:
    //   wsum       = 1 + 1              = 2
    //   maskedWsum = 1*1 + 1*(1 - 1)    = 1    (low ignores the claim, high defers)
    //   blend      = min(2,1) * (1 / 2) = 0.5
    //   target     = (10 + 20) / 2      = 15
    // The pool-wide OR this replaced would read respectPool = true and produce
    // blend = min(2,1) * (1 - 1) = 0, leaving the base height untouched.
    {
        const float32 base = baseAt(kOverlapOnRoadX, kOverlapOnRoadZ);
        const float32 perMember = base + (kAveragedGrade - base) * 0.5f;
        EXPECT_NEAR(worldAt(kOverlapOnRoadX, kOverlapOnRoadZ), perMember, kHeightTolerance)
            << "the mixed pool must half-blend on fully claimed ground";
        EXPECT_GT(std::abs(perMember - base), 1.0f)
            << "base and the per-member answer must differ, or this probe cannot tell the two "
               "operators apart";
    }

    // And that the ramp is actually SAMPLED, which is the whole point of the
    // second claimed road. On the centre row both routes reach full weight, so the
    // blend there is exactly 1 - claim/2 and the claim at a probe is recoverable:
    //   baked = base + (15 - base) * (1 - claim/2)  =>  claim = 2 * (1 - blend)
    // A probe whose recovered claim is strictly between 0 and 1 is one whose mask
    // came off the claim's ramp. Without this count the fixture can quietly go
    // back to sampling nothing but 0 and 1.
    int rampProbes = 0;
    for (int32 sx = SampleAt(-30.0f); sx <= SampleAt(30.0f); ++sx)
    {
        const float32 worldX = -kWorldSize * 0.5f + static_cast<float32>(sx) * kSampleSpacing;
        const float32 base = baseAt(worldX, 0.0f);
        const float32 span = kAveragedGrade - base;
        if (std::abs(span) < 1.0f)
            continue; // no leverage to recover a mask from at this probe
        const float32 blend = (worldAt(worldX, 0.0f) - base) / span;
        const float32 claim = 2.0f * (1.0f - blend);
        if (claim > 1e-3f && claim < 1.0f - 1e-3f)
            ++rampProbes;
    }
    EXPECT_GE(rampProbes, 5)
        << "the fixture must probe the claim's falloff ramp, not just its interior and its "
           "exterior — a mask that drifted between 0 and 1 is invisible to a gate that only "
           "ever reads 0 and 1";
}

TEST(TerrainFlattenPoolContracts, EditingAClaimReBakesThePoolsItMasks)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    auto& terrainSvc = TerrainService::Get();
    auto& splineSvc = SplineECS::SplineService::Get();

    const TerrainHandle handle = CreateBakedTerrain(terrainSvc);
    auto* data = terrainSvc.GetTerrainData(handle);
    ASSERT_NE(data, nullptr);

    ECS::World world;
    CreateTerrainEntity(world, handle);

    const ECS::EntityHandle road = CreateSplineEntity(
        world, splineSvc,
        {{0.0f, 0.0f, -40.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 40.0f}}, kClaimRadius);
    AddSplineVolume(world, road, 0.0f, kClaimFalloff);
    Components::TerrainGroundClaimEffect claim{};
    claim.Strength = 1.0f;
    world.AddComponentImmediate<Components::TerrainGroundClaimEffect>(road, claim);

    const ECS::EntityHandle route = CreateSplineEntity(
        world, splineSvc,
        {{-40.0f, kLowGrade, 0.0f}, {0.0f, kLowGrade, 0.0f}, {40.0f, kLowGrade, 0.0f}},
        kRouteHalfWidth);
    AddSplineVolume(world, route, 10.0f);
    world.AddComponentImmediate<Components::TerrainFlattenEffect>(
        route, MakePooledFlatten(nullptr, true));

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    const auto heightOnRoad = [&] {
        return data->Heightfield.GetRawSamples()[static_cast<std::size_t>(SampleAt(0.0f))
                                                     * kHeightmapDim + SampleAt(0.0f)]
             * kHeightScale;
    };
    const float32 masked = heightOnRoad();

    // Release the claim: the route should now own that ground.
    auto* claimEffect = world.GetComponentForWrite<Components::TerrainGroundClaimEffect>(road);
    ASSERT_NE(claimEffect, nullptr);
    claimEffect->Strength = 0.0f;

    system.Update(world, 1.0f / 60.0f);

    EXPECT_NEAR(heightOnRoad(), kLowGrade, kHeightTolerance)
        << "releasing the claim must re-bake the route over the ground it had ceded";
    EXPECT_GT(std::abs(heightOnRoad() - masked), 1.0f);
}

// ------------------------------------------------ where a pool's slot falls --
//
// A pool occupies ONE slot in the priority order and the whole of its blend
// lands there, so exactly which slot it is decides what a modifier between its
// members sees. The tests above only ever pooled ADJACENT routes, where every
// candidate slot gives the same answer. These straddle a non-member.

namespace
{

// A spline volume carrying a plain SET flatten — a NON-member modifier, used to
// probe where a pool's single slot actually falls in the priority order.
void AddFlattenVolume(ECS::World& world, SplineECS::SplineService& svc, float32 z,
                      float32 target, float32 priority)
{
    const ECS::EntityHandle e = CreateSplineEntity(
        world, svc, {{-40.0f, 0.0f, z}, {0.0f, 0.0f, z}, {40.0f, 0.0f, z}}, kRouteHalfWidth);
    AddSplineVolume(world, e, priority);
    Components::TerrainFlattenEffect fx{};
    fx.UseVolumeHeight = false;
    fx.TargetHeight = target;
    fx.Blend = Components::TerrainModifierBlend::Set;
    world.AddComponentImmediate<Components::TerrainFlattenEffect>(e, fx);
}

constexpr float32 kFlattenTarget = 50.0f;

} // namespace

// A pool's members straddle a non-member in the priority order. The pool owns
// ONE slot, at its highest-priority member, so the non-member between them must
// apply BEFORE the pool's blend, not after it.
//
// Run over the DEFAULT pool, because that is now where nearly every route lives:
// with blending on by default, "a modifier interleaved between routes" stops
// being an exotic arrangement and becomes the normal way a flatten shares a scene
// with a road network. The named-pool arm is the same rule and is covered by
// TwoInterleavedPoolsComposeInPriorityOrder below.
TEST(TerrainFlattenPool, ThePoolSlotIsItsHighestPriorityMemberNotItsFirst)
{
    const BakeResult r = Bake([](ECS::World& world, SplineECS::SplineService& svc) {
        AuthorRoute(world, svc, kLowRouteZ, kLowGrade, 10.0f, nullptr, false);
        AddFlattenVolume(world, svc, 0.0f, kFlattenTarget, 20.0f);
        AuthorRoute(world, svc, kHighRouteZ, kHighGrade, 30.0f, nullptr, false);
    });
    EXPECT_NEAR(r.BakedAt(kOverlapOffRoadX, kOverlapOffRoadZ), kAveragedGrade, kHeightTolerance)
        << "the pool must land AFTER the priority-20 flatten it straddles; a flush pinned to "
           "the FIRST member would leave the flatten's target here";
}

// The counterfactual: a non-member ABOVE every member must win outright. If this
// and the test above both pass, the pool's slot is genuinely at its last member
// rather than at either end of the stack.
TEST(TerrainFlattenPool, ANonMemberAboveEveryMemberStillOverwritesThePool)
{
    const BakeResult r = Bake([](ECS::World& world, SplineECS::SplineService& svc) {
        AuthorRoute(world, svc, kLowRouteZ, kLowGrade, 10.0f, nullptr, false);
        AuthorRoute(world, svc, kHighRouteZ, kHighGrade, 30.0f, nullptr, false);
        AddFlattenVolume(world, svc, 0.0f, kFlattenTarget, 40.0f);
    });
    EXPECT_NEAR(r.BakedAt(kOverlapOffRoadX, kOverlapOffRoadZ), kFlattenTarget, kHeightTolerance);
}

// A LONE pooled flatten is a pool of one, and a pool of one still owns its slot:
// a non-member above it overwrites it, one below it is overwritten. Without this
// an in-place arm could come back as "one member composes inline" without any
// test noticing that its slot moved.
TEST(TerrainFlattenPool, APoolOfOneStillOccupiesItsOwnSlot)
{
    const BakeResult flattenAbove = Bake([](ECS::World& world, SplineECS::SplineService& svc) {
        AuthorRoute(world, svc, 0.0f, kLowGrade, 10.0f, nullptr, false);
        AddFlattenVolume(world, svc, 0.0f, kFlattenTarget, 20.0f);
    });
    EXPECT_NEAR(flattenAbove.BakedAt(kOverlapOffRoadX, kOverlapOffRoadZ), kFlattenTarget,
                kHeightTolerance);

    const BakeResult flattenBelow = Bake([](ECS::World& world, SplineECS::SplineService& svc) {
        AddFlattenVolume(world, svc, 0.0f, kFlattenTarget, 10.0f);
        AuthorRoute(world, svc, 0.0f, kLowGrade, 20.0f, nullptr, false);
    });
    EXPECT_NEAR(flattenBelow.BakedAt(kOverlapOffRoadX, kOverlapOffRoadZ), kLowGrade,
                kHeightTolerance);
}

// A pooled flatten's contribution lands at its POOL's slot, not at its own
// position in its volume's effect stack. So another height effect on the SAME
// volume applies first, whatever StackOrder says — the route composes with the
// other routes, not with the stack around it.
//
// This is the one behaviour a reader would expect StackOrder to govern and it
// does not, which is why the flatten's own header states it. Pinned here so it is
// a contract rather than an accident.
TEST(TerrainFlattenPool, APooledFlattenLandsAtItsPoolsSlotNotItsStackPosition)
{
    constexpr float32 kOffset = 5.0f;
    const BakeResult r = Bake([](ECS::World& world, SplineECS::SplineService& svc) {
        const ECS::EntityHandle e = CreateSplineEntity(
            world, svc,
            {{-40.0f, kLowGrade, 0.0f}, {0.0f, kLowGrade, 0.0f}, {40.0f, kLowGrade, 0.0f}},
            kRouteHalfWidth);
        AddSplineVolume(world, e, 10.0f);
        // Pooled flatten FIRST in the stack, height offset second: read as a
        // stack, the ground would be graded to kLowGrade and then raised by
        // kOffset.
        Components::TerrainFlattenEffect pooled = MakePooledFlatten(nullptr, false);
        pooled.StackOrder = 0;
        world.AddComponentImmediate<Components::TerrainFlattenEffect>(e, pooled);
        Components::TerrainHeightOffsetEffect offset{};
        offset.StackOrder = 1;
        offset.Offset = kOffset;
        offset.Blend = Components::TerrainModifierBlend::Add;
        world.AddComponentImmediate<Components::TerrainHeightOffsetEffect>(e, offset);
    });

    // The offset applies at the volume's own slot, over the BASE; the pool's
    // blend then lands on top at the pool's slot and levels the result to the
    // route's grade. So the offset is overwritten rather than added to it.
    EXPECT_NEAR(r.BakedAt(kOverlapOffRoadX, kOverlapOffRoadZ), kLowGrade, kHeightTolerance)
        << "a pooled flatten applies at its POOL's slot; an effect sharing its volume composes "
           "BEFORE it regardless of StackOrder";
    EXPECT_GT(std::abs(r.BakedAt(kOverlapOffRoadX, kOverlapOffRoadZ) - (kLowGrade + kOffset)),
              1.0f)
        << "and specifically not the stack reading, which would land here";
}

// Two named pools whose members interleave in the priority order. Each pool
// applies at its own last member, so the pool whose last member is highest wins.
TEST(TerrainFlattenPool, TwoInterleavedPoolsComposeInPriorityOrder)
{
    const auto bakeWith = [](float32 aFirst, float32 aLast, float32 bFirst, float32 bLast) {
        return Bake([&](ECS::World& world, SplineECS::SplineService& svc) {
            AuthorRoute(world, svc, -6.0f, 10.0f, aFirst, "GroupA", false);
            AuthorRoute(world, svc, -3.0f, 30.0f, bFirst, "GroupB", false);
            AuthorRoute(world, svc, 3.0f, 20.0f, aLast, "GroupA", false);
            AuthorRoute(world, svc, 6.0f, 40.0f, bLast, "GroupB", false);
        });
    };

    // A's last member at priority 30, B's at 40 -> B applies last.
    const BakeResult bWins = bakeWith(10.0f, 30.0f, 20.0f, 40.0f);
    EXPECT_NEAR(bWins.BakedAt(kOverlapOffRoadX, kOverlapOffRoadZ), 35.0f, kHeightTolerance)
        << "GroupB's last member has the highest priority, so its average must win";

    // Swap: A's last member at 40 -> A applies last.
    const BakeResult aWins = bakeWith(20.0f, 40.0f, 10.0f, 30.0f);
    EXPECT_NEAR(aWins.BakedAt(kOverlapOffRoadX, kOverlapOffRoadZ), 15.0f, kHeightTolerance)
        << "GroupA's last member now has the highest priority, so its average must win";
}

// A pool member whose footprint misses the bake region entirely, with a
// NON-member between the two members in priority order. This is the shape that
// breaks if the flush position is derived from which members CONTRIBUTED A RECT
// rather than from the members' positions in the stack.
// RegionBakeOfAMovedRouteMatchesAFullBake above does not reach it: there both
// members contribute to the region, and nothing sits between them.
TEST(TerrainFlattenPoolContracts, RegionBakeWithAMemberOutsideTheRegionMatchesFullBake)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    auto& terrainSvc = TerrainService::Get();
    auto& splineSvc = SplineECS::SplineService::Get();

    constexpr float32 kNearZ = -40.0f;  // the member the region bake touches
    constexpr float32 kFarZ = 90.0f;    // the member far outside it
    constexpr float32 kSlide = -6.0f;

    const auto authorArm = [&](ECS::World& world, bool moveAfterAuthoring) {
        const ECS::EntityHandle nearRoute = CreateSplineEntity(
            world, splineSvc,
            {{-40.0f, kLowGrade, kNearZ}, {0.0f, kLowGrade, kNearZ}, {40.0f, kLowGrade, kNearZ}},
            kRouteHalfWidth);
        AddSplineVolume(world, nearRoute, 10.0f);
        world.AddComponentImmediate<Components::TerrainFlattenEffect>(
            nearRoute, MakePooledFlatten("Approaches", false));

        // Non-member, priority BETWEEN the two members.
        AddFlattenVolume(world, splineSvc, kNearZ, kFlattenTarget, 20.0f);

        // Member far away: contributes no rect to a region bake around kNearZ,
        // but still occupies its slot in the stack.
        AuthorRoute(world, splineSvc, kFarZ, kHighGrade, 30.0f, "Approaches", false);

        if (moveAfterAuthoring)
        {
            auto* xf = world.GetComponentForWrite<Components::WorldTransform>(nearRoute);
            xf->matrix[14] = kSlide;
        }
        return nearRoute;
    };

    const TerrainHandle handleA = CreateBakedTerrain(terrainSvc);
    auto* dataA = terrainSvc.GetTerrainData(handleA);
    ASSERT_NE(dataA, nullptr);
    ECS::World worldA;
    CreateTerrainEntity(worldA, handleA);
    const ECS::EntityHandle nearRoute = authorArm(worldA, false);

    TerrainModifierSystem systemA;
    systemA.Update(worldA, 1.0f / 60.0f);

    dataA->SplatmapDirty = false;
    const uint64 cursor = dataA->HeightfieldVersion;
    auto* xf = worldA.GetComponentForWrite<Components::WorldTransform>(nearRoute);
    ASSERT_NE(xf, nullptr);
    xf->matrix[14] = kSlide;
    systemA.Update(worldA, 1.0f / 60.0f);

    DirtyRegionLog::Region dirty;
    ASSERT_TRUE(dataA->HeightfieldDirtyLog.CollectSince(cursor, dirty));
    EXPECT_LT(dirty.MaxZ, static_cast<int32>(kHeightmapDim))
        << "the re-bake must be region-scoped for this test to mean anything";

    const TerrainHandle handleB = CreateBakedTerrain(terrainSvc);
    auto* dataB = terrainSvc.GetTerrainData(handleB);
    ASSERT_NE(dataB, nullptr);
    ECS::World worldB;
    CreateTerrainEntity(worldB, handleB);
    authorArm(worldB, true);
    TerrainModifierSystem systemB;
    systemB.Update(worldB, 1.0f / 60.0f);

    ASSERT_EQ(dataA->Heightfield.GetSampleCount(), dataB->Heightfield.GetSampleCount());
    EXPECT_EQ(0, std::memcmp(dataA->Heightfield.GetRawSamples(), dataB->Heightfield.GetRawSamples(),
                             dataA->Heightfield.GetSampleCount() * sizeof(float32)))
        << "a region re-bake whose pool has a member outside the region must still be "
           "bit-identical to a full bake";
}

// ------------------------------------------ the two fields the hash must fold --
//
// The pool group and the volume's StationSpacing are gather-time inputs: both are
// spent before the texel loop runs, so neither shows up in the per-texel maths
// the other fields drive. That is the shape that keeps hashing correctly by habit
// and stops the day the fold is pruned as dead. Both are red without their fold.

TEST(TerrainFlattenPoolContracts, EditingThePoolGroupReBakes)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    auto& terrainSvc = TerrainService::Get();
    auto& splineSvc = SplineECS::SplineService::Get();

    const TerrainHandle handle = CreateBakedTerrain(terrainSvc);
    auto* data = terrainSvc.GetTerrainData(handle);
    ASSERT_NE(data, nullptr);

    ECS::World world;
    CreateTerrainEntity(world, handle);
    // Both UNNAMED, so both are in the shared pool — the state content arrives in.
    AuthorRoute(world, splineSvc, kLowRouteZ, kLowGrade, 10.0f, nullptr, false);

    const ECS::EntityHandle high = CreateSplineEntity(
        world, splineSvc,
        {{-40.0f, kHighGrade, kHighRouteZ}, {0.0f, kHighGrade, kHighRouteZ},
         {40.0f, kHighGrade, kHighRouteZ}}, kRouteHalfWidth);
    AddSplineVolume(world, high, 20.0f);
    world.AddComponentImmediate<Components::TerrainFlattenEffect>(
        high, MakePooledFlatten(nullptr, false));

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    const auto heightAt = [&]() {
        return data->Heightfield.GetRawSamples()[static_cast<std::size_t>(SampleAt(kOverlapOffRoadZ))
                                                     * kHeightmapDim
                                                 + SampleAt(kOverlapOffRoadX)] * kHeightScale;
    };
    ASSERT_NEAR(heightAt(), kAveragedGrade, kHeightTolerance)
        << "precondition: the shared pool's average";
    const uint64 versionBefore = data->HeightfieldVersion;

    // NAME one flatten's pool — the opt-out edit. Nothing else changes: not the
    // volume, not the spline, not any other effect field. A hash that dropped the
    // pool-group fold would leave the ground on the average it already has.
    auto* fx = world.GetComponentForWrite<Components::TerrainFlattenEffect>(high);
    ASSERT_NE(fx, nullptr);
    std::memset(fx->PoolGroup, 0, sizeof(fx->PoolGroup));
    std::memcpy(fx->PoolGroup, "Other", 5);

    system.Update(world, 1.0f / 60.0f);

    EXPECT_GT(data->HeightfieldVersion, versionBefore)
        << "naming a pool group re-partitions which routes average; it must re-bake";
    EXPECT_NEAR(heightAt(), kHighGrade, kHeightTolerance)
        << "split into two pools the higher-priority route overwrites instead of averaging";
}

TEST(TerrainFlattenPoolContracts, EditingTheVolumesStationSpacingReBakes)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    auto& terrainSvc = TerrainService::Get();
    auto& splineSvc = SplineECS::SplineService::Get();

    const TerrainHandle handle = CreateBakedTerrain(terrainSvc);
    auto* data = terrainSvc.GetTerrainData(handle);
    ASSERT_NE(data, nullptr);

    ECS::World world;
    CreateTerrainEntity(world, handle);

    // A route that BENDS: on a straight route every spacing resamples to the same
    // geometry, so the edit would be a no-op and the test vacuous.
    const ECS::EntityHandle route = CreateSplineEntity(
        world, splineSvc,
        {{-40.0f, kLowGrade, -30.0f}, {0.0f, kHighGrade, 0.0f}, {40.0f, kLowGrade, -30.0f}},
        kRouteHalfWidth);
    AddSplineVolume(world, route, 10.0f, 0.0f, /*stationSpacing=*/0.5f);
    world.AddComponentImmediate<Components::TerrainFlattenEffect>(
        route, MakePooledFlatten(nullptr, false));

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    std::vector<float32> fine(data->Heightfield.GetRawSamples(),
                              data->Heightfield.GetRawSamples()
                                  + data->Heightfield.GetSampleCount());
    const uint64 versionBefore = data->HeightfieldVersion;

    // The spacing lives on the VOLUME — geometry belongs to the region, not to an
    // effect inside it — so this is the edit the hash must fold.
    auto* vol = world.GetComponentForWrite<Components::TerrainModifierVolume>(route);
    ASSERT_NE(vol, nullptr);
    vol->StationSpacing = 40.0f; // a handful of long chords instead of a dense resample

    system.Update(world, 1.0f / 60.0f);

    EXPECT_GT(data->HeightfieldVersion, versionBefore)
        << "StationSpacing decides the polyline the bake evaluates; editing it must re-bake";
    EXPECT_NE(0, std::memcmp(fine.data(), data->Heightfield.GetRawSamples(),
                             fine.size() * sizeof(float32)))
        << "the coarser resample must actually change the baked ground, or this proves nothing";
}

// --------------------------------------------- the conform chain's first link --

// Dragging a route's CONTROL POINT — not one of the effect's fields — must move
// the geometry hash, re-bake, and bump HeightfieldVersion, which is the term
// ConformSurfaceRevision folds and what schedules a conforming recipe to
// re-settle. Both halves are asserted: the version moved AND the ground moved.
TEST(TerrainFlattenPoolContracts, DraggingARouteControlPointReBakesAndBumpsTheVersion)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    auto& terrainSvc = TerrainService::Get();
    auto& splineSvc = SplineECS::SplineService::Get();

    const TerrainHandle handle = CreateBakedTerrain(terrainSvc);
    auto* data = terrainSvc.GetTerrainData(handle);
    ASSERT_NE(data, nullptr);

    ECS::World world;
    CreateTerrainEntity(world, handle);

    const SplineECS::SplineHandle splineHandle =
        splineSvc.CreateSpline(Spline::SplineType::CatmullRom, false);
    auto* splineData = splineSvc.GetSplineData(splineHandle);
    ASSERT_NE(splineData, nullptr);
    splineData->AddPoint({-40.0f, kLowGrade, 0.0f}, kRouteHalfWidth);
    splineData->AddPoint({0.0f, kLowGrade, 0.0f}, kRouteHalfWidth);
    splineData->AddPoint({40.0f, kLowGrade, 0.0f}, kRouteHalfWidth);
    splineSvc.RebuildCache(splineHandle);

    auto route = world.CreateEntity();
    Components::SplineComponent comp{};
    comp.SplineDataIndex = splineHandle.Index();
    comp.SplineDataGeneration = splineHandle.Generation();
    world.AddComponentImmediate<Components::SplineComponent>(route, comp);
    world.AddComponentImmediate<Components::WorldTransform>(route, IdentityAt(0.0f, 0.0f, 0.0f));
    AddSplineVolume(world, route, 10.0f);
    world.AddComponentImmediate<Components::TerrainFlattenEffect>(
        route, MakePooledFlatten(nullptr, false));

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    const auto heightAt = [&](float32 x, float32 z) {
        return data->Heightfield.GetRawSamples()[static_cast<std::size_t>(SampleAt(z))
                                                     * kHeightmapDim + SampleAt(x)] * kHeightScale;
    };
    const uint64 versionBefore = data->HeightfieldVersion;
    const float32 before = heightAt(0.0f, 0.0f);
    ASSERT_NEAR(before, kLowGrade, kHeightTolerance) << "precondition: the route graded here";

    // Drag the middle control point UP — the same edit the spline gizmo makes.
    auto* liveSpline = splineSvc.GetSplineData(splineHandle);
    ASSERT_NE(liveSpline, nullptr);
    liveSpline->Points[1].Position.y = kHighGrade;
    splineSvc.RebuildCache(splineHandle);

    system.Update(world, 1.0f / 60.0f);

    EXPECT_GT(data->HeightfieldVersion, versionBefore)
        << "a control-point drag must bump the version ConformSurfaceRevision folds, or a "
           "conformed piece keeps its pre-drag altitude with nothing to schedule a re-place";
    EXPECT_GT(heightAt(0.0f, 0.0f), before + 1.0f)
        << "the drag must reach the bake, not just the version";
}

// ------------------------------------------- where a claim sits in the stack --

// A pooled flatten reads the claim buffer at the slot it ACCUMULATES in — its
// OWN, not its pool's flush — so a claim authored above that slot is not yet in
// place and does NOT mask it. Routes above every claim is a real authoring
// constraint rather than a coincidence of the scenes written so far, and the
// offline twin depends on it, so the rule is pinned here instead of living in a
// docstring.
//
// RE-DERIVED for per-member masking: a pool of ONE is the case where the member's
// own slot and the pool's flush position coincide, so both arms below read what
// they did when ownership was resolved once at the flush. The case where the two
// rules disagree needs a claim BETWEEN two members of one pool, which is
// AClaimBetweenTwoPoolMembersMasksOnlyTheMembersBelowIt.
TEST(TerrainPoolClaim, AClaimAboveAPooledFlattenInPriorityDoesNotMaskIt)
{
    const auto bake = [](float32 claimPriority) {
        return Bake([&](ECS::World& world, SplineECS::SplineService& svc) {
            const ECS::EntityHandle claimEntity = CreateSplineEntity(
                world, svc, {{0.0f, 0.0f, -40.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 40.0f}},
                kClaimRadius);
            AddSplineVolume(world, claimEntity, claimPriority, kClaimFalloff);
            Components::TerrainGroundClaimEffect claim{};
            claim.Strength = 1.0f;
            world.AddComponentImmediate<Components::TerrainGroundClaimEffect>(claimEntity, claim);

            AuthorRoute(world, svc, kLowRouteZ, kLowGrade, 10.0f, nullptr, true);
        });
    };

    // Claim BELOW the route: in place first, so the member is masked off the road
    // and the ground there keeps its base height.
    const BakeResult below = bake(0.0f);
    EXPECT_NEAR(below.BakedAt(kOverlapOnRoadX, kOverlapOnRoadZ),
                below.BaseAt(kOverlapOnRoadX, kOverlapOnRoadZ), kHeightTolerance)
        << "a claim below the route must hold the road";

    // Claim ABOVE the route: the member accumulates before the claim exists, so
    // it grades straight through.
    const BakeResult above = bake(50.0f);
    EXPECT_NEAR(above.BakedAt(kOverlapOnRoadX, kOverlapOnRoadZ), kLowGrade, kHeightTolerance)
        << "a claim ABOVE the route in priority is not yet accumulated when the member "
           "accumulates, so the route grades through it";
}

// ------------------------------------------ RespectClaims is read PER MEMBER --
//
// THE RED ARM for the dissolution's composition change, and it is a red arm by
// construction: the scene below is the one the deleted
// RespectClaimsIsOredAcrossThePoolNotReadPerMember pinned, and the two operators
// compute different ground on it.
//
// One pool, two members at full weight over ground a claim owns outright
// (claim = 1). The low route does NOT respect claims; the high route does.
//
//   PER MEMBER (this slice)           POOL-WIDE OR (what it replaced)
//   wsum       = 1 + 1          = 2   respectPool = false OR true = true
//   maskedWsum = 1*1 + 1*0      = 1   blend = min(2,1) * (1 - claim) = 0
//   blend      = min(2,1) * 1/2 = 0.5 ground = base, untouched
//   target     = (10 + 20) / 2  = 15
//   ground     = base + (15 - base) * 0.5
//
// The base heightfield is procedural noise, so the two predictions are computed
// from the measured base here rather than written as literals — and the test
// asserts they are at least a metre apart, so it cannot go vacuous if the base
// ever lands on 15. Both are printed on every run. On the fixture's own base of
// 33.935490 m at world (0, 0) they are 24.467745 m (per member) against
// 33.935490 m (pool-wide OR): 9.4677 m apart, and the bake reads 24.467743 m.
TEST(TerrainPoolClaim, RespectClaimsIsReadPerMemberNotOredAcrossThePool)
{
    const BakeResult mixed = Bake([](ECS::World& world, SplineECS::SplineService& svc) {
        AuthorClaimedRoad(world, svc, 1.0f); // priority 0, below both routes
        AuthorRoute(world, svc, kLowRouteZ, kLowGrade, 10.0f, nullptr, /*respectClaims=*/false);
        AuthorRoute(world, svc, kHighRouteZ, kHighGrade, 20.0f, nullptr, /*respectClaims=*/true);
    });

    const float32 base = mixed.BaseAt(kOverlapOnRoadX, kOverlapOnRoadZ);
    const float32 perMember = base + (kAveragedGrade - base) * 0.5f;
    const float32 pooledOr = base;

    std::printf("[POOL-MASK] base=%.6f  per-member=%.6f  pool-ORed=%.6f  baked=%.6f\n",
                static_cast<double>(base), static_cast<double>(perMember),
                static_cast<double>(pooledOr),
                static_cast<double>(mixed.BakedAt(kOverlapOnRoadX, kOverlapOnRoadZ)));
    std::fflush(stdout);

    ASSERT_GT(std::abs(perMember - pooledOr), 1.0f)
        << "the two operators must predict different ground here, or this is not a red arm";
    EXPECT_NEAR(mixed.BakedAt(kOverlapOnRoadX, kOverlapOnRoadZ), perMember, kHeightTolerance)
        << "the member that owns its ground must grade its share while its pool-mate stops; a "
           "pool-wide OR would leave the base height here";

    // The CAPABILITY this buys, measured rather than described. Alone, a route
    // with the flag off grades through the claim — what its author asked for.
    // Under the OR, putting an ordinary unnamed sibling beside it re-enabled
    // masking on it; per member, the sibling's own answer no longer speaks for it.
    const BakeResult alone = Bake([](ECS::World& world, SplineECS::SplineService& svc) {
        AuthorClaimedRoad(world, svc, 1.0f);
        AuthorRoute(world, svc, kLowRouteZ, kLowGrade, 10.0f, nullptr, /*respectClaims=*/false);
    });
    EXPECT_NEAR(alone.BakedAt(kOverlapOnRoadX, kOverlapOnRoadZ), kLowGrade, kHeightTolerance)
        << "alone, a route told not to stop at claimed ground grades through it";

    // Off the claim the mixed pool still averages, so nothing above is a member
    // that simply stopped contributing.
    EXPECT_NEAR(mixed.BakedAt(kOverlapOffRoadX, kOverlapOffRoadZ), kAveragedGrade,
                kHeightTolerance);
}

// A blend can never leave the interval between the height that was there and the
// target — that is what makes it a blend. Ownership above 1 is the one input that
// can violate it: the mask is 1 - claim, so an unsaturated claim turns a member's
// masked weight negative and pushes the ground AWAY from the pool's target.
//
// The arms vary the POOL SHAPE — alone in the shared pool, sharing it, and named
// out of it — which is the axis that could still reach the mask with a different
// weight sum.
TEST(TerrainPoolClaim, AClaimStrongerThanOneNeverInvertsTheBlend)
{
    struct Arm { const char* Label; const char* PoolGroup; bool SecondRoute; };
    constexpr Arm kArms[] = {
        {"shared pool, alone",  nullptr,      false},
        {"shared pool, shared", nullptr,      true},
        {"named pool",          "Aqueduct",   false},
    };

    for (const Arm& arm : kArms)
    {
        const BakeResult r = Bake([&](ECS::World& world, SplineECS::SplineService& svc) {
            AuthorClaimedRoad(world, svc, 2.0f);
            AuthorRoute(world, svc, kLowRouteZ, kLowGrade, 10.0f, arm.PoolGroup, true);
            if (arm.SecondRoute)
                AuthorRoute(world, svc, kHighRouteZ, kHighGrade, 20.0f, nullptr, true);
        });
        const float32 base = r.BaseAt(kOverlapOnRoadX, kOverlapOnRoadZ);
        const float32 baked = r.BakedAt(kOverlapOnRoadX, kOverlapOnRoadZ);
        // The reachable target at the probe: the pool's weighted average, which is
        // the low grade alone unless the second route is there to average with.
        const float32 target = arm.SecondRoute ? kAveragedGrade : kLowGrade;
        const float32 lo = std::min(base, target);
        const float32 hi = std::max(base, target);
        std::printf("[POOL-STRENGTH] %-21s strength=2.0  base=%.4f target=%.4f baked=%.4f\n",
                    arm.Label, static_cast<double>(base), static_cast<double>(target),
                    static_cast<double>(baked));
        std::fflush(stdout);
        EXPECT_GE(baked, lo - kHeightTolerance)
            << arm.Label << ": a blend must stay between the ground that was there and the target";
        EXPECT_LE(baked, hi + kHeightTolerance)
            << arm.Label << ": a blend must stay between the ground that was there and the target";
    }
}

// ------------------------------------- a run that owns the ground it grades --
//
// Blend-by-default makes this arrangement the normal shape of a road rather than
// an exotic one. Each member answers for itself now, so a road that claims its
// ground and grades it says so with its OWN RespectClaims flag, and the path
// beside it still stops at the claim.

TEST(TerrainPoolClaim, ARunThatOwnsItsGroundGradesItAndHoldsOtherRoutesOff)
{
    // The owning run: a pooled flatten in a pool of its own that does NOT defer to
    // claims, plus the claim, on one volume. A crossing footpath is in the shared
    // pool, above it in priority, and does defer.
    //
    // The private pool is what keeps the road's grade out of the footpath's
    // average, not what keeps the claim off the road — per-member masking means a
    // shared-pool road would grade its share too, which is what
    // RespectClaimsIsReadPerMemberNotOredAcrossThePool measures.
    const BakeResult r = Bake([](ECS::World& world, SplineECS::SplineService& svc) {
        const ECS::EntityHandle road = CreateSplineEntity(
            world, svc, {{0.0f, kHighGrade, -40.0f}, {0.0f, kHighGrade, 0.0f},
                         {0.0f, kHighGrade, 40.0f}}, kClaimRadius);
        AddSplineVolume(world, road, 10.0f, kClaimFalloff);
        world.AddComponentImmediate<Components::TerrainFlattenEffect>(
            road, MakePooledFlatten("Roads", /*respectClaims=*/false));
        Components::TerrainGroundClaimEffect claim{};
        claim.Strength = 1.0f;
        world.AddComponentImmediate<Components::TerrainGroundClaimEffect>(road, claim);

        AuthorRoute(world, svc, 0.0f, kLowGrade, 20.0f, nullptr, /*respectClaims=*/true);
    });

    // On the road's own centreline the road's grade stands, not the footpath's and
    // not the base: the run graded the ground it owns.
    EXPECT_NEAR(r.BakedAt(0.0f, 0.0f), kHighGrade, kHeightTolerance)
        << "a run that owns its ground must still grade it — a member that deferred to its own "
           "claim would leave the base height here";

    // Well clear of the road, the footpath grades normally, so the claim is local.
    EXPECT_NEAR(r.BakedAt(30.0f, 0.0f), kLowGrade, kHeightTolerance);
}

// The same-entity claim, both ways round. This is what design row 9 NARROWED: a
// claim on the volume a pooled flatten sits on is LEGAL, and only the member's
// own RespectClaims plus the effects' stack order decide whether it holds itself
// back.
//
// Effects apply in STACK ORDER within one volume, and the claim is read where the
// member accumulates. So a same-entity claim silences the grade only when the
// claim runs FIRST — precisely the condition
// TerrainModifierSystem::WarnOnClaimsThatCannotMask reports (a ground claim below
// the flatten in stack order, and only when the flatten respects claims at all).
TEST(TerrainPoolClaim, ASelfClaimOnlyHoldsAMemberBackWhenItRunsFirst)
{
    const auto bakeSelfClaiming = [](int32 flattenStackOrder, int32 claimStackOrder,
                                     bool respectClaims) {
        return Bake([&](ECS::World& world, SplineECS::SplineService& svc) {
            const ECS::EntityHandle road = CreateSplineEntity(
                world, svc, {{0.0f, kHighGrade, -40.0f}, {0.0f, kHighGrade, 0.0f},
                             {0.0f, kHighGrade, 40.0f}}, kClaimRadius);
            AddSplineVolume(world, road, 10.0f, kClaimFalloff);
            Components::TerrainFlattenEffect flatten = MakePooledFlatten("Roads", respectClaims);
            flatten.StackOrder = flattenStackOrder;
            world.AddComponentImmediate<Components::TerrainFlattenEffect>(road, flatten);
            Components::TerrainGroundClaimEffect claim{};
            claim.StackOrder = claimStackOrder;
            claim.Strength = 1.0f;
            world.AddComponentImmediate<Components::TerrainGroundClaimEffect>(road, claim);
        });
    };

    // The trap the gather warns about: the claim is BELOW the flatten, so it is
    // already in the ownership buffer when the flatten accumulates, and a flatten
    // that defers to claims defers to its own. It grades nothing.
    const BakeResult claimFirst = bakeSelfClaiming(/*flattenStackOrder=*/1, /*claimStackOrder=*/0,
                                                   /*respectClaims=*/true);
    EXPECT_NEAR(claimFirst.BakedAt(0.0f, 0.0f), claimFirst.BaseAt(0.0f, 0.0f), kHeightTolerance)
        << "this is the documented trap, not a wish: if it ever grades, the warning that names it "
           "is now lying and both must move together";

    // The NARROWING: the same volume with the claim ABOVE the flatten. The member
    // accumulates before its own claim reaches the buffer, so it grades its route
    // — and the claim still holds back every route that accumulates after it. A
    // same-entity claim is an ordinary arrangement now, not a refusal.
    const BakeResult flattenFirst = bakeSelfClaiming(/*flattenStackOrder=*/0, /*claimStackOrder=*/1,
                                                     /*respectClaims=*/true);
    EXPECT_NEAR(flattenFirst.BakedAt(0.0f, 0.0f), kHighGrade, kHeightTolerance)
        << "a claim stacked ABOVE the flatten is not yet in the buffer when the member "
           "accumulates, so the run grades its own ground";

    // And with the flag off the stack order stops mattering: a member that does
    // not defer to claims never reads the ownership buffer at all.
    const BakeResult ignoring = bakeSelfClaiming(/*flattenStackOrder=*/1, /*claimStackOrder=*/0,
                                                 /*respectClaims=*/false);
    EXPECT_NEAR(ignoring.BakedAt(0.0f, 0.0f), kHighGrade, kHeightTolerance);
}

// A claim whose slot falls BETWEEN two members of one pool.
//
// RE-DERIVED for per-member masking, and this is where the two operators part
// company. Ownership is read at each MEMBER's own accumulate slot, so a claim
// between two members is in place for the member above it and absent for the one
// below:
//
//   low member  (priority 10, before the claim): owned = 0 -> contributes w * 1
//   claim       (priority 15)                  : ownership buffer <- 1
//   high member (priority 20, after the claim) : owned = 1 -> contributes w * 0
//
//   wsum = 2, maskedWsum = 1, blend = min(2,1) * (1/2) = 0.5, target = 15
//
// The pool-wide read at the FLUSH — what this replaced — saw the claim in place
// for every member and masked the whole pool down to the base. Half the pool
// grades now: the same "each member answers for itself" rule the RespectClaims
// red arm measures, reached through ORDERING instead of through the flag.
TEST(TerrainPoolClaim, AClaimBetweenTwoPoolMembersMasksOnlyTheMembersBelowIt)
{
    // Control: the ordinary arrangement, claim below BOTH members. Every member
    // accumulates after it, so the whole pool is masked and the ground is base.
    const BakeResult r = Bake([](ECS::World& world, SplineECS::SplineService& svc) {
        AuthorRoute(world, svc, kLowRouteZ, kLowGrade, 10.0f, nullptr, true);
        AuthorClaimedRoad(world, svc, 1.0f); // priority 0 inside AuthorClaimedRoad
        AuthorRoute(world, svc, kHighRouteZ, kHighGrade, 20.0f, nullptr, true);
    });
    EXPECT_NEAR(r.BakedAt(kOverlapOnRoadX, kOverlapOnRoadZ),
                r.BaseAt(kOverlapOnRoadX, kOverlapOnRoadZ), kHeightTolerance);

    // The interleaved one: the claim sits between the two members.
    const BakeResult between = Bake([](ECS::World& world, SplineECS::SplineService& svc) {
        AuthorRoute(world, svc, kLowRouteZ, kLowGrade, 10.0f, nullptr, true);
        const ECS::EntityHandle claimEntity = CreateSplineEntity(
            world, svc, {{0.0f, 0.0f, -40.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 40.0f}},
            kClaimRadius);
        AddSplineVolume(world, claimEntity, 15.0f, kClaimFalloff);
        Components::TerrainGroundClaimEffect claim{};
        claim.Strength = 1.0f;
        world.AddComponentImmediate<Components::TerrainGroundClaimEffect>(claimEntity, claim);
        AuthorRoute(world, svc, kHighRouteZ, kHighGrade, 20.0f, nullptr, true);
    });

    const float32 base = between.BaseAt(kOverlapOnRoadX, kOverlapOnRoadZ);
    const float32 expected = base + (kAveragedGrade - base) * 0.5f;
    ASSERT_GT(std::abs(expected - base), 1.0f)
        << "the half blend and the fully-masked answer must differ, or this arm proves nothing";
    EXPECT_NEAR(between.BakedAt(kOverlapOnRoadX, kOverlapOnRoadZ), expected, kHeightTolerance)
        << "each member reads ownership at its OWN slot, so a claim between two members holds "
           "back only the member above it — a read at the pool's flush would mask both and leave "
           "the base height here";
}

// ------------------------------------- claims mask in EVERY blend mode --------
//
// Ownership is not a pooling concept. The claim mask used to be read only inside
// the pooled branch, so a region authored with (RespectClaims = true, Blend !=
// Average) carried a flag that did nothing; it is read at the slot every effect
// applies in now, whatever operator it composes with.
//
// THE RED ARM, and it is red by construction: each arm below bakes the SAME scene
// twice, once with the claim respected and once without, and asserts the two
// differ by more than a metre before asserting which one the bake produced. An
// arm whose operator happens not to move the ground at the probe FAILS on that
// guard rather than passing vacuously.
//
// Every operator is covered, because the masking is applied to the WEIGHT and
// BlendHeight's arms are not one expression: Add/Subtract multiply the value,
// Set/Min/Max/SmoothMin/SmoothMax lerp toward a union. Weight 0 must return the
// ground unchanged in all of them.
TEST(TerrainClaimMask, EveryBlendModeStopsAtClaimedGround)
{
    using Blend = Components::TerrainModifierBlend;

    // The grade is chosen per operator so the UNMASKED arm actually moves the
    // ground at the probe: the fixture's base there is ~33.9 m, so Max and
    // SmoothMax need a target above it and the rest need one below.
    struct Arm { const char* Label; Blend Mode; float32 Grade; };
    constexpr Arm kArms[] = {
        {"Set",       Blend::Set,       kLowGrade},
        {"Add",       Blend::Add,       kLowGrade},
        {"Subtract",  Blend::Subtract,  kLowGrade},
        {"Min (cut)", Blend::Min,       kLowGrade},
        {"Max",       Blend::Max,       50.0f},
        {"SmoothMin", Blend::SmoothMin, kLowGrade},
        {"SmoothMax", Blend::SmoothMax, 50.0f},
    };

    for (const Arm& arm : kArms)
    {
        const auto bake = [&](bool respectClaims) {
            return Bake([&](ECS::World& world, SplineECS::SplineService& svc) {
                AuthorClaimedRoad(world, svc, 1.0f); // priority 0, below the route
                AuthorUnpooledRoute(world, svc, kLowRouteZ, arm.Grade, 10.0f, arm.Mode,
                                    respectClaims);
            });
        };

        const BakeResult respecting = bake(true);
        const BakeResult ignoring = bake(false);

        const float32 base = respecting.BaseAt(kOverlapOnRoadX, kOverlapOnRoadZ);
        const float32 masked = respecting.BakedAt(kOverlapOnRoadX, kOverlapOnRoadZ);
        const float32 unmasked = ignoring.BakedAt(kOverlapOnRoadX, kOverlapOnRoadZ);

        std::printf("[CLAIM-BLEND] %-10s base=%.6f  masked=%.6f  unmasked=%.6f\n",
                    arm.Label, static_cast<double>(base), static_cast<double>(masked),
                    static_cast<double>(unmasked));
        std::fflush(stdout);

        ASSERT_GT(std::abs(unmasked - base), 1.0f)
            << arm.Label << ": the unmasked arm must move the ground here, or this arm proves "
                            "nothing about masking";
        EXPECT_NEAR(masked, base, kHeightTolerance)
            << arm.Label << ": a fully claimed texel must keep the ground that was there, "
                            "whatever operator the effect blends with";

        // Off the claim the same effect shapes normally, so nothing above is an
        // effect that simply stopped contributing everywhere.
        EXPECT_NEAR(respecting.BakedAt(kOverlapOffRoadX, kOverlapOffRoadZ),
                    ignoring.BakedAt(kOverlapOffRoadX, kOverlapOffRoadZ), kHeightTolerance)
            << arm.Label << ": the claim is local to the ground it owns";
    }
}

// The other half of the red arm: an effect that does NOT respect claims composes
// straight through one, and an effect that does is untouched where no claim
// reaches it. Both are the "masking absent" control the arm above needs.
TEST(TerrainClaimMask, AnUnpooledEffectIsUnmaskedWithoutAClaimOrWithTheFlagOff)
{
    // No claim authored at all: the flag is on and there is nothing to defer to.
    const BakeResult unclaimed = Bake([](ECS::World& world, SplineECS::SplineService& svc) {
        AuthorUnpooledRoute(world, svc, kLowRouteZ, kLowGrade, 10.0f,
                            Components::TerrainModifierBlend::Set, /*respectClaims=*/true);
    });
    EXPECT_NEAR(unclaimed.BakedAt(kOverlapOnRoadX, kOverlapOnRoadZ), kLowGrade, kHeightTolerance)
        << "with no claim in the scene a claim-respecting effect grades normally";

    // A claim IS authored, and the effect is told to ignore it.
    const BakeResult ignoring = Bake([](ECS::World& world, SplineECS::SplineService& svc) {
        AuthorClaimedRoad(world, svc, 1.0f);
        AuthorUnpooledRoute(world, svc, kLowRouteZ, kLowGrade, 10.0f,
                            Components::TerrainModifierBlend::Set, /*respectClaims=*/false);
    });
    EXPECT_NEAR(ignoring.BakedAt(kOverlapOnRoadX, kOverlapOnRoadZ), kLowGrade, kHeightTolerance)
        << "a region that owns its ground grades through the claim over it";
}

// THE BYTE-IDENTITY SPINE for everything that does not author the newly-live
// combination. The mask is applied as `weight * (1 - claim)`, and `1 - 0` is
// exactly 1.0f, so an effect with the flag OFF multiplies its weight by a value
// that changes no bits — even with a full-strength claim sitting in the buffer
// underneath it.
//
// EQ on the raw samples, not NEAR: "close enough" here would let a rounding
// change hide, and the migration-parity bakes this suite guards compare bytes.
TEST(TerrainClaimMask, TheMaskIsBitInertWhereTheEffectDoesNotRespectClaims)
{
    const BakeResult withClaim = Bake([](ECS::World& world, SplineECS::SplineService& svc) {
        AuthorClaimedRoad(world, svc, 1.0f);
        AuthorUnpooledRoute(world, svc, kLowRouteZ, kLowGrade, 10.0f,
                            Components::TerrainModifierBlend::Set, /*respectClaims=*/false);
    });
    const BakeResult withoutClaim = Bake([](ECS::World& world, SplineECS::SplineService& svc) {
        AuthorUnpooledRoute(world, svc, kLowRouteZ, kLowGrade, 10.0f,
                            Components::TerrainModifierBlend::Set, /*respectClaims=*/false);
    });

    ASSERT_EQ(withClaim.Baked.size(), withoutClaim.Baked.size());
    EXPECT_EQ(0, std::memcmp(withClaim.Baked.data(), withoutClaim.Baked.data(),
                             withClaim.Baked.size() * sizeof(float32)))
        << "a claim the effect ignores must leave the heightfield byte-identical: the mask "
           "multiplies by exactly 1.0f, which is not a rounding step";
}

// The same-entity trap, at an ordinary blend. This is the condition
// TerrainModifierSystem::WarnOnClaimsThatCannotMask reports, and the warning no
// longer says "blends by Average" — so the behaviour it names has to hold here
// too, or the message is lying about a case it now covers.
TEST(TerrainClaimMask, ASelfClaimBelowAnUnpooledEffectHoldsItBack)
{
    const auto bake = [](int32 flattenStackOrder, int32 claimStackOrder) {
        return Bake([&](ECS::World& world, SplineECS::SplineService& svc) {
            const ECS::EntityHandle road = CreateSplineEntity(
                world, svc, {{0.0f, kHighGrade, -40.0f}, {0.0f, kHighGrade, 0.0f},
                             {0.0f, kHighGrade, 40.0f}}, kClaimRadius);
            AddSplineVolume(world, road, 10.0f, kClaimFalloff);
            Components::TerrainFlattenEffect flatten =
                MakeUnpooledFlatten(Components::TerrainModifierBlend::Set,
                                    /*respectClaims=*/true);
            flatten.StackOrder = flattenStackOrder;
            world.AddComponentImmediate<Components::TerrainFlattenEffect>(road, flatten);
            Components::TerrainGroundClaimEffect claim{};
            claim.StackOrder = claimStackOrder;
            claim.Strength = 1.0f;
            world.AddComponentImmediate<Components::TerrainGroundClaimEffect>(road, claim);
        });
    };

    const BakeResult claimFirst = bake(/*flattenStackOrder=*/1, /*claimStackOrder=*/0);
    EXPECT_NEAR(claimFirst.BakedAt(0.0f, 0.0f), claimFirst.BaseAt(0.0f, 0.0f), kHeightTolerance)
        << "the documented trap: a claim below a claim-respecting effect is already in the buffer "
           "when that effect applies, so it holds its own ground back";

    const BakeResult flattenFirst = bake(/*flattenStackOrder=*/0, /*claimStackOrder=*/1);
    EXPECT_NEAR(flattenFirst.BakedAt(0.0f, 0.0f), kHighGrade, kHeightTolerance)
        << "a claim stacked ABOVE the effect is not yet in the buffer when it applies, so the run "
           "shapes its own ground";
}

// -------------------------------------------------------- curvature residual --

// How far the analytic Catmull-Rom parts from the station polyline through it, at
// a large island scene's lane geometry.
//
// WHAT THIS NUMBER NOW MEANS. It used to be the residual that reached a route's
// blend: a corridor evaluated the polyline while the ground claim beside it
// evaluated the curve, and the disagreement landed in the mask that MULTIPLIES
// the blend. Canonicalisation removed that path — the station polyline is EVERY
// SplinePath volume's geometry now, claims included, so a flatten and a claim
// read the same object and the residual reaching a pool's blend is 0 by
// construction on every arm, not merely small.
//
// What is still measured here is the gap between the two curves themselves, which
// still has a live consumer: TerrainVolumeShape::SplineArea resolves its weight
// through SignedDistanceToSplineXZ on the analytic curve (ComputeWeight's
// non-Route branch). This is the record of how
// far a shape that took the polyline instead would move.
//
// Disabled by default, as before and for the same reason: it is a measurement,
// not a gate. There is no threshold that would not encode whichever machine and
// spacing ran it last, and the sweep costs seconds an arm.
//
//   TerrainRegionBakeTests.exe --gtest_also_run_disabled_tests \
//       --gtest_filter=TerrainRouteCurvature.DISABLED_CurveVsPolylineAtLaneGeometry
TEST(TerrainRouteCurvature, DISABLED_CurveVsPolylineAtLaneGeometry)
{
    ScopedSplineService splineScope;
    auto& svc = SplineECS::SplineService::Get();

    constexpr float32 kSpacing = 0.5f; // APPROACH_POINT_SPACING

    // The STRAIGHT arm is the CONTROL. Curve and polyline coincide exactly on a
    // straight route, so whatever it reports is the instrument's own floor (the
    // band SDF's closest-point search plus float32 rounding). A bent arm measures
    // CURVATURE only to the extent it exceeds this.
    struct Arm { const char* Name; float32 BendRadius; bool Straight; };
    const Arm arms[] = {{"STRAIGHT control", 20.0f, true},
                        {"R=30m gentle", 30.0f, false}, {"R=10m design case", 10.0f, false},
                        {"R=5m tight", 5.0f, false}, {"R=3m hairpin", 3.0f, false}};

    for (const Arm& arm : arms)
    {
        const float32 R = arm.BendRadius;
        const SplineECS::SplineHandle handle =
            svc.CreateSpline(Spline::SplineType::CatmullRom, false);
        auto* spline = svc.GetSplineData(handle);
        for (int i = 0; i <= 8; ++i)
        {
            if (arm.Straight)
            {
                spline->AddPoint({R * (static_cast<float32>(i) / 8.0f), 0.0f, 0.0f}, kClaimRadius);
                continue;
            }
            const float32 a = static_cast<float32>(i) / 8.0f * 1.5707963f;
            spline->AddPoint({R * std::cos(a), 0.0f, R * std::sin(a)}, kClaimRadius);
        }
        svc.RebuildCache(handle);

        RoutePolyline polyline;
        BuildRoutePolyline(*spline, kSpacing, polyline);
        ASSERT_TRUE(polyline.IsValid()) << arm.Name;

        float32 maxDistanceDelta = 0.0f;
        float32 maxWeightDelta = 0.0f;
        double sumWeightDelta = 0.0;
        int samples = 0;
        for (float32 x = -8.0f; x <= R + 8.0f; x += 0.05f)
        {
            for (float32 z = -8.0f; z <= R + 8.0f; z += 0.05f)
            {
                const float32 dPoly = ClosestStationXZ(polyline, x, z).Distance;
                if (dPoly > kClaimRadius + kClaimFalloff + 1.0f)
                    continue; // outside the shape ramp on both evaluators
                const float32 dCurve =
                    Spline::SignedDistanceToSplineBandXZ(*spline, x, z) + kClaimRadius;
                const float32 weightPoly =
                    ShapeFalloffWeight(kClaimRadius - dPoly, kClaimFalloff, 0.0f);
                const float32 weightCurve =
                    ShapeFalloffWeight(kClaimRadius - dCurve, kClaimFalloff, 0.0f);
                maxDistanceDelta = std::max(maxDistanceDelta, std::abs(dPoly - dCurve));
                const float32 weightDelta = std::abs(weightPoly - weightCurve);
                maxWeightDelta = std::max(maxWeightDelta, weightDelta);
                sumWeightDelta += weightDelta;
                ++samples;
            }
        }

        std::printf("[ROUTE-CURVATURE] %-18s samples=%d  max|dPoly-dCurve|=%.6f m  "
                    "max|weightDelta|=%.6f  mean|weightDelta|=%.6f  "
                    "height residual at 1m grade=%.4f m  at 5m=%.4f m  "
                    "(residual reaching a POOL blend: 0 by construction)\n",
                    arm.Name, samples, static_cast<double>(maxDistanceDelta),
                    static_cast<double>(maxWeightDelta),
                    samples > 0 ? sumWeightDelta / samples : 0.0,
                    static_cast<double>(maxWeightDelta * 1.0f),
                    static_cast<double>(maxWeightDelta * 5.0f));
        std::fflush(stdout);
        EXPECT_GT(samples, 0) << arm.Name;
    }
}

// ---- Pooling generalized to the other height effects ------------------------
//
// The flatten's pooled value is an absolute candidate HEIGHT, so its pool lerps
// the ground toward the weighted average. A height offset's, a noise's and a
// stamp's are DISPLACEMENTS, so their pools ADD the weighted average — two
// overlapping members contribute their mean, not their sum.
//
// Every arm below names the number the OTHER operator produces, so a test that
// passes because nothing happened is not available.

namespace
{

// A circle volume with a hard edge, so the weight inside it is exactly 1 and
// every number below is hand-computable.
ECS::EntityHandle AddPadVolume(ECS::World& world, float32 x, float32 y, float32 radius,
                               float32 priority)
{
    auto e = world.CreateEntity();
    world.AddComponentImmediate<Components::WorldTransform>(e, IdentityAt(x, y, 0.0f));
    Components::TerrainModifierVolume volume{};
    volume.Shape = Components::TerrainVolumeShape::Circle;
    volume.Radius = radius;
    volume.Falloff = 0.0f;
    volume.Priority = priority;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(e, volume);
    return e;
}

// The arms every pooled-effect test switches on, so two arms of one test differ
// in exactly the blend and nothing else.
enum class PoolArm { Pooled, PooledNamedApart, Unpooled };

template <typename TEffect>
void ApplyArm(TEffect& fx, PoolArm arm, const char* name, bool respectClaims)
{
    std::memset(fx.PoolGroup, 0, sizeof(fx.PoolGroup));
    fx.RespectClaims = respectClaims;
    switch (arm)
    {
    case PoolArm::Pooled:
        fx.Blend = Components::TerrainModifierBlend::Average;
        break;
    case PoolArm::PooledNamedApart:
        fx.Blend = Components::TerrainModifierBlend::Average;
        std::memcpy(fx.PoolGroup, name, std::min(sizeof(fx.PoolGroup) - 1, std::strlen(name)));
        break;
    case PoolArm::Unpooled:
        fx.Blend = Components::TerrainModifierBlend::Add;
        break;
    }
}

constexpr float32 kPadRadius = 30.0f;
// Pad A sits at the origin, pad B 20 m along +X. Probe (10, 0) is 10 m from each
// centre, so BOTH reach full weight; probe (-20, 0) is inside A (20 < 30) and
// outside B (40 > 30), so only one member reaches it.
constexpr float32 kPadBX = 20.0f;
constexpr float32 kBothPadsX = 10.0f;
constexpr float32 kOnlyPadAX = -20.0f;

constexpr float32 kOffsetA = 6.0f;
constexpr float32 kOffsetB = 18.0f;
constexpr float32 kOffsetMean = 12.0f; // (6 + 18) / 2
constexpr float32 kOffsetSum = 24.0f;  // what an unpooled Add pair produces

} // namespace

TEST(TerrainEffectPool, PooledHeightOffsetsAverageWhereTheyOverlapInsteadOfSumming)
{
    const auto authorPads = [](PoolArm arm) {
        return [arm](ECS::World& world, SplineECS::SplineService&) {
            const auto addPad = [&world, arm](float32 x, float32 offset, float32 priority,
                                              const char* name) {
                const auto e = AddPadVolume(world, x, 0.0f, kPadRadius, priority);
                Components::TerrainHeightOffsetEffect fx{};
                fx.Offset = offset;
                ApplyArm(fx, arm, name, /*respectClaims=*/false);
                world.AddComponentImmediate<Components::TerrainHeightOffsetEffect>(e, fx);
            };
            addPad(0.0f, kOffsetA, 10.0f, "PadA");
            addPad(kPadBX, kOffsetB, 20.0f, "PadB");
        };
    };

    const BakeResult pooled = Bake(authorPads(PoolArm::Pooled));
    const BakeResult unpooled = Bake(authorPads(PoolArm::Unpooled));

    const float32 pooledRise = pooled.BakedAt(kBothPadsX, 0.0f) - pooled.BaseAt(kBothPadsX, 0.0f);
    const float32 unpooledRise =
        unpooled.BakedAt(kBothPadsX, 0.0f) - unpooled.BaseAt(kBothPadsX, 0.0f);

    // The point of the operator: where both pads reach, the pool contributes the
    // MEAN displacement once.
    EXPECT_NEAR(pooledRise, kOffsetMean, kHeightTolerance)
        << "two pooled height offsets must raise the ground by their average";
    // RED: the same pair stacked additively piles up instead.
    EXPECT_NEAR(unpooledRise, kOffsetSum, kHeightTolerance)
        << "the unpooled arm must sum, or this test has no red side";
    EXPECT_GT(std::abs(pooledRise - unpooledRise), 1.0f);

    // Where only one member reaches, a pool of one is that member: pooled and
    // unpooled agree, which is the reduction the single implementation promises.
    EXPECT_NEAR(pooled.BakedAt(kOnlyPadAX, 0.0f) - pooled.BaseAt(kOnlyPadAX, 0.0f),
                kOffsetA, kHeightTolerance);
    EXPECT_NEAR(pooled.BakedAt(kOnlyPadAX, 0.0f), unpooled.BakedAt(kOnlyPadAX, 0.0f),
                kHeightTolerance)
        << "a lone pooled member must reduce exactly to the unpooled operator";

    // Named apart, the two pads are two pools of one, so they stack again.
    const BakeResult separated = Bake(authorPads(PoolArm::PooledNamedApart));
    EXPECT_NEAR(separated.BakedAt(kBothPadsX, 0.0f) - separated.BaseAt(kBothPadsX, 0.0f),
                kOffsetSum, kHeightTolerance)
        << "naming a pool is the opt-OUT: two pools of one compose as two effects";
}

TEST(TerrainEffectPool, PooledNoiseCrossfadesInsteadOfDoublingTheRoughness)
{
    // Two pads carrying IDENTICAL noise. fBM is evaluated in world space, so both
    // produce the same displacement at any texel — which makes the arithmetic
    // exact without knowing the field: pooled = N, unpooled Add = 2N.
    const auto authorPads = [](PoolArm arm, int padCount) {
        return [arm, padCount](ECS::World& world, SplineECS::SplineService&) {
            const auto addPad = [&world, arm](float32 x, float32 priority, const char* name) {
                const auto e = AddPadVolume(world, x, 0.0f, kPadRadius, priority);
                Components::TerrainNoiseEffect fx{};
                fx.Frequency = 8.0f;
                fx.Amplitude = 5.0f;
                fx.Octaves = 3;
                fx.Seed = 4242u;
                ApplyArm(fx, arm, name, /*respectClaims=*/false);
                world.AddComponentImmediate<Components::TerrainNoiseEffect>(e, fx);
            };
            addPad(0.0f, 10.0f, "PadA");
            if (padCount > 1)
                addPad(kPadBX, 20.0f, "PadB");
        };
    };

    const BakeResult one = Bake(authorPads(PoolArm::Pooled, 1));
    const BakeResult pooledTwo = Bake(authorPads(PoolArm::Pooled, 2));
    const BakeResult unpooledTwo = Bake(authorPads(PoolArm::Unpooled, 2));

    const float32 single = one.BakedAt(kBothPadsX, 0.0f) - one.BaseAt(kBothPadsX, 0.0f);
    const float32 pooled = pooledTwo.BakedAt(kBothPadsX, 0.0f) - pooledTwo.BaseAt(kBothPadsX, 0.0f);
    const float32 unpooled =
        unpooledTwo.BakedAt(kBothPadsX, 0.0f) - unpooledTwo.BaseAt(kBothPadsX, 0.0f);

    // The probe must sit on non-zero noise, or every arm below is 0 == 0.
    ASSERT_GT(std::abs(single), 0.1f)
        << "the probe landed on a noise null; this arm would prove nothing";

    // Averaging two identical fields is exactly one of them — the crossfade.
    EXPECT_NEAR(pooled, single, kHeightTolerance)
        << "two pooled noise regions must crossfade, not accumulate";
    // RED: stacked additively, the same pair doubles the roughness.
    EXPECT_NEAR(unpooled, 2.0f * single, kHeightTolerance)
        << "the unpooled arm must double, or this test has no red side";
    EXPECT_GT(std::abs(pooled - unpooled), 0.1f);
}

TEST(TerrainEffectPool, PooledStampsAverageInsteadOfStacking)
{
    // No mask, so each stamp is the documented flat raise of HeightScale metres.
    constexpr float32 kStampA = 4.0f;
    constexpr float32 kStampB = 16.0f;
    constexpr float32 kStampMean = 10.0f;
    constexpr float32 kStampSum = 20.0f;

    const auto authorPads = [](PoolArm arm) {
        return [arm](ECS::World& world, SplineECS::SplineService&) {
            const auto addPad = [&world, arm](float32 x, float32 height, float32 priority,
                                              const char* name) {
                const auto e = AddPadVolume(world, x, 0.0f, kPadRadius, priority);
                Components::TerrainStampEffect fx{};
                fx.HeightScale = height;
                ApplyArm(fx, arm, name, /*respectClaims=*/false);
                world.AddComponentImmediate<Components::TerrainStampEffect>(e, fx);
            };
            addPad(0.0f, kStampA, 10.0f, "PadA");
            addPad(kPadBX, kStampB, 20.0f, "PadB");
        };
    };

    const BakeResult pooled = Bake(authorPads(PoolArm::Pooled));
    const BakeResult unpooled = Bake(authorPads(PoolArm::Unpooled));

    const float32 pooledRise = pooled.BakedAt(kBothPadsX, 0.0f) - pooled.BaseAt(kBothPadsX, 0.0f);
    const float32 unpooledRise =
        unpooled.BakedAt(kBothPadsX, 0.0f) - unpooled.BaseAt(kBothPadsX, 0.0f);

    EXPECT_NEAR(pooledRise, kStampMean, kHeightTolerance)
        << "two pooled stamps must raise by their average, not stack";
    EXPECT_NEAR(unpooledRise, kStampSum, kHeightTolerance)
        << "the unpooled arm must stack, or this test has no red side";
    EXPECT_GT(std::abs(pooledRise - unpooledRise), 1.0f);
}

// ---- Claims, both directions, on each newly poolable kind -------------------
//
// The claimed road runs along +Z at x = 0 with priority 0, so its ownership is
// already in the buffer when a pad at a higher priority accumulates. A pad that
// respects the claim contributes NOTHING over the road (masked weight 0), and
// one that does not contributes in full. The two arms differ by the whole
// displacement, and agree off the road.

namespace
{
// Both probes sit at z = 10 rather than on the origin: gradient noise is exactly
// 0 at a lattice point and world (0, 0) is one, so an origin probe reads 0 for
// every noise arm and would prove nothing. On-road x = 0 is inside the claim
// (radius 2.4 + falloff 1.5); off-road x = 20 is well outside the claim and
// still inside the pad (sqrt(20^2 + 10^2) = 22.4 < 30).
constexpr float32 kOnRoadX = 0.0f;
constexpr float32 kOnRoadZ = 10.0f;
constexpr float32 kOffRoadX = 20.0f;
constexpr float32 kOffRoadZ = 10.0f;

// One pad over the claimed road, carrying the effect under test.
template <typename TEffect, typename TInit>
BakeResult BakeClaimedPad(bool respectClaims, TInit init)
{
    return Bake([respectClaims, init](ECS::World& world, SplineECS::SplineService& svc) {
        AuthorClaimedRoad(world, svc, 1.0f); // priority 0: in place before the pad
        const auto e = AddPadVolume(world, 0.0f, 0.0f, kPadRadius, 10.0f);
        TEffect fx{};
        init(fx);
        ApplyArm(fx, PoolArm::Pooled, "", respectClaims);
        world.AddComponentImmediate<TEffect>(e, fx);
    });
}
} // namespace

TEST(TerrainEffectPoolClaim, APooledHeightOffsetStopsAtClaimedGroundOnlyWhenItRespectsClaims)
{
    const auto init = [](Components::TerrainHeightOffsetEffect& fx) { fx.Offset = kOffsetB; };
    const BakeResult respecting =
        BakeClaimedPad<Components::TerrainHeightOffsetEffect>(/*respectClaims=*/true, init);
    const BakeResult ignoring =
        BakeClaimedPad<Components::TerrainHeightOffsetEffect>(/*respectClaims=*/false, init);

    // Respecting: the claim masks the member's whole weight, so the road is left
    // exactly as the base heightfield had it.
    EXPECT_NEAR(respecting.BakedAt(kOnRoadX, kOnRoadZ), respecting.BaseAt(kOnRoadX, kOnRoadZ),
                kHeightTolerance)
        << "a pooled height offset that stops at claimed ground must write nothing over it";
    // RED: with the claim ignored, the same pad raises the road by its offset.
    EXPECT_NEAR(ignoring.BakedAt(kOnRoadX, kOnRoadZ) - ignoring.BaseAt(kOnRoadX, kOnRoadZ),
                kOffsetB, kHeightTolerance)
        << "the claim-ignoring arm must raise the road, or this test has no red side";
    EXPECT_GT(std::abs(ignoring.BakedAt(kOnRoadX, kOnRoadZ)
                       - respecting.BakedAt(kOnRoadX, kOnRoadZ)),
              1.0f);

    // Off the claim the two arms agree, so nothing above came from an effect that
    // simply stopped contributing everywhere.
    EXPECT_NEAR(respecting.BakedAt(kOffRoadX, kOffRoadZ), ignoring.BakedAt(kOffRoadX, kOffRoadZ),
                kHeightTolerance);
    EXPECT_NEAR(respecting.BakedAt(kOffRoadX, kOffRoadZ) - respecting.BaseAt(kOffRoadX, kOffRoadZ),
                kOffsetB, kHeightTolerance);
}

TEST(TerrainEffectPoolClaim, APooledNoiseStopsAtClaimedGroundOnlyWhenItRespectsClaims)
{
    const auto init = [](Components::TerrainNoiseEffect& fx) {
        fx.Frequency = 8.0f;
        fx.Amplitude = 5.0f;
        fx.Octaves = 3;
        fx.Seed = 4242u;
    };
    const BakeResult respecting =
        BakeClaimedPad<Components::TerrainNoiseEffect>(/*respectClaims=*/true, init);
    const BakeResult ignoring =
        BakeClaimedPad<Components::TerrainNoiseEffect>(/*respectClaims=*/false, init);

    const float32 ignoredRise =
        ignoring.BakedAt(kOnRoadX, kOnRoadZ) - ignoring.BaseAt(kOnRoadX, kOnRoadZ);
    ASSERT_GT(std::abs(ignoredRise), 0.1f)
        << "the probe landed on a noise null; this arm would prove nothing";

    EXPECT_NEAR(respecting.BakedAt(kOnRoadX, kOnRoadZ), respecting.BaseAt(kOnRoadX, kOnRoadZ),
                kHeightTolerance)
        << "a pooled noise that stops at claimed ground must leave the road smooth";
    EXPECT_GT(std::abs(ignoring.BakedAt(kOnRoadX, kOnRoadZ)
                       - respecting.BakedAt(kOnRoadX, kOnRoadZ)),
              0.1f);

    EXPECT_NEAR(respecting.BakedAt(kOffRoadX, kOffRoadZ), ignoring.BakedAt(kOffRoadX, kOffRoadZ),
                kHeightTolerance);
}

TEST(TerrainEffectPoolClaim, APooledStampStopsAtClaimedGroundOnlyWhenItRespectsClaims)
{
    constexpr float32 kStampHeight = 12.0f;
    const auto init = [](Components::TerrainStampEffect& fx) { fx.HeightScale = kStampHeight; };
    const BakeResult respecting =
        BakeClaimedPad<Components::TerrainStampEffect>(/*respectClaims=*/true, init);
    const BakeResult ignoring =
        BakeClaimedPad<Components::TerrainStampEffect>(/*respectClaims=*/false, init);

    EXPECT_NEAR(respecting.BakedAt(kOnRoadX, kOnRoadZ), respecting.BaseAt(kOnRoadX, kOnRoadZ),
                kHeightTolerance)
        << "a pooled stamp that stops at claimed ground must write nothing over it";
    EXPECT_NEAR(ignoring.BakedAt(kOnRoadX, kOnRoadZ) - ignoring.BaseAt(kOnRoadX, kOnRoadZ),
                kStampHeight, kHeightTolerance)
        << "the claim-ignoring arm must stamp the road, or this test has no red side";
    EXPECT_GT(std::abs(ignoring.BakedAt(kOnRoadX, kOnRoadZ)
                       - respecting.BakedAt(kOnRoadX, kOnRoadZ)),
              1.0f);

    EXPECT_NEAR(respecting.BakedAt(kOffRoadX, kOffRoadZ), ignoring.BakedAt(kOffRoadX, kOffRoadZ),
                kHeightTolerance);
}

// ---- Pool identity is (kind, group), not the group name alone ---------------

TEST(TerrainEffectPool, TwoKindsSharingAPoolNameDoNotAverageTogether)
{
    // A flatten pad and a noise pad over the same ground. If pools were keyed by
    // NAME alone, giving them the same name would merge a candidate height with a
    // displacement into one average; keyed by (kind, name), the name makes no
    // difference at all — which is what this pins.
    const auto authorPads = [](const char* flattenPool, const char* noisePool) {
        return [flattenPool, noisePool](ECS::World& world, SplineECS::SplineService&) {
            const auto flattenEntity = AddPadVolume(world, 0.0f, 0.0f, kPadRadius, 10.0f);
            Components::TerrainFlattenEffect flatten{};
            flatten.UseVolumeHeight = false;
            flatten.TargetHeight = 20.0f;
            ApplyArm(flatten, PoolArm::PooledNamedApart, flattenPool, /*respectClaims=*/false);
            world.AddComponentImmediate<Components::TerrainFlattenEffect>(flattenEntity, flatten);

            const auto noiseEntity = AddPadVolume(world, 0.0f, 0.0f, kPadRadius, 20.0f);
            Components::TerrainNoiseEffect noise{};
            noise.Frequency = 8.0f;
            noise.Amplitude = 5.0f;
            noise.Octaves = 3;
            noise.Seed = 4242u;
            ApplyArm(noise, PoolArm::PooledNamedApart, noisePool, /*respectClaims=*/false);
            world.AddComponentImmediate<Components::TerrainNoiseEffect>(noiseEntity, noise);
        };
    };

    const BakeResult sameName = Bake(authorPads("Shared", "Shared"));
    const BakeResult differentNames = Bake(authorPads("Flattens", "Noises"));

    ASSERT_EQ(sameName.Baked.size(), differentNames.Baked.size());
    ASSERT_FALSE(sameName.Baked.empty());
    EXPECT_EQ(0, std::memcmp(sameName.Baked.data(), differentNames.Baked.data(),
                             sameName.Baked.size() * sizeof(float32)))
        << "a pool is keyed by (kind, group): sharing a name across two kinds must change nothing, "
           "because averaging a candidate height against a displacement is not a quantity";

    // Non-vacuity: the bake did something, and the flatten's target is visible
    // under the noise it composes with.
    EXPECT_GT(std::abs(sameName.BakedAt(0.0f, 0.0f) - sameName.BaseAt(0.0f, 0.0f)), 1.0f);
}

// ---- Two pools whose last member is the same volume ------------------------

TEST(TerrainEffectPool, TwoPoolsOnOneVolumeApplyInTheStackOrderOfTheEffectsThatOwnThem)
{
    // One volume carrying a pooled flatten AND a pooled height offset, so the
    // volume is the last member of both pools and the tie-break decides.
    constexpr float32 kTarget = 20.0f;
    constexpr float32 kOffset = 8.0f;

    const auto author = [](int32 flattenOrder, int32 offsetOrder) {
        return [flattenOrder, offsetOrder](ECS::World& world, SplineECS::SplineService&) {
            const auto e = AddPadVolume(world, 0.0f, 0.0f, kPadRadius, 10.0f);

            Components::TerrainFlattenEffect flatten{};
            flatten.StackOrder = flattenOrder;
            flatten.UseVolumeHeight = false;
            flatten.TargetHeight = kTarget;
            ApplyArm(flatten, PoolArm::Pooled, "", /*respectClaims=*/false);
            world.AddComponentImmediate<Components::TerrainFlattenEffect>(e, flatten);

            Components::TerrainHeightOffsetEffect offset{};
            offset.StackOrder = offsetOrder;
            offset.Offset = kOffset;
            ApplyArm(offset, PoolArm::Pooled, "", /*respectClaims=*/false);
            world.AddComponentImmediate<Components::TerrainHeightOffsetEffect>(e, offset);
        };
    };

    // Flatten first, then the offset raises off the levelled grade.
    const BakeResult flattenFirst = Bake(author(/*flattenOrder=*/0, /*offsetOrder=*/1));
    EXPECT_NEAR(flattenFirst.BakedAt(0.0f, 0.0f), kTarget + kOffset, kHeightTolerance)
        << "the offset pool must apply after the flatten pool when its effect sits above it";

    // Offset first, then the flatten levels to its target and wipes the raise.
    const BakeResult offsetFirst = Bake(author(/*flattenOrder=*/1, /*offsetOrder=*/0));
    EXPECT_NEAR(offsetFirst.BakedAt(0.0f, 0.0f), kTarget, kHeightTolerance)
        << "the flatten pool must apply after the offset pool when its effect sits above it";

    EXPECT_GT(std::abs(flattenFirst.BakedAt(0.0f, 0.0f) - offsetFirst.BakedAt(0.0f, 0.0f)), 1.0f);
}

// ---- The new fields are folded into the change hash ------------------------
//
// A field the hash does not carry edits silently and never re-bakes, which is
// the one failure that looks like nothing happening.

TEST(TerrainEffectPoolContracts, EditingANoisePoolGroupReBakes)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    auto& terrainSvc = TerrainService::Get();

    const TerrainHandle handle = CreateBakedTerrain(terrainSvc);
    auto* data = terrainSvc.GetTerrainData(handle);
    ASSERT_NE(data, nullptr);

    ECS::World world;
    CreateTerrainEntity(world, handle);

    // Two pooled noise pads in the SHARED pool, so moving one into a pool of its
    // own changes the ground where they overlap.
    const auto addPad = [&world](float32 x, float32 priority) {
        const auto e = AddPadVolume(world, x, 0.0f, kPadRadius, priority);
        Components::TerrainNoiseEffect fx{};
        fx.Frequency = 8.0f;
        fx.Amplitude = 5.0f;
        fx.Octaves = 3;
        fx.Seed = 4242u;
        ApplyArm(fx, PoolArm::Pooled, "", /*respectClaims=*/false);
        world.AddComponentImmediate<Components::TerrainNoiseEffect>(e, fx);
        return e;
    };
    addPad(0.0f, 10.0f);
    const ECS::EntityHandle second = addPad(kPadBX, 20.0f);

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    const uint64 versionBefore = data->HeightfieldVersion;
    std::vector<float32> before(data->Heightfield.GetRawSamples(),
                                data->Heightfield.GetRawSamples()
                                    + data->Heightfield.GetSampleCount());

    // Move the second pad into a pool of its own. ONLY the pool group changes.
    {
        auto* fx = world.GetComponentForWrite<Components::TerrainNoiseEffect>(second);
        ASSERT_NE(fx, nullptr);
        std::memset(fx->PoolGroup, 0, sizeof(fx->PoolGroup));
        std::memcpy(fx->PoolGroup, "Apart", 5);
    }

    system.Update(world, 1.0f / 60.0f);

    EXPECT_GT(data->HeightfieldVersion, versionBefore)
        << "renaming a pool group must re-bake; a field the change hash does not carry edits "
           "silently and never bakes";
    EXPECT_NE(0, std::memcmp(before.data(), data->Heightfield.GetRawSamples(),
                             before.size() * sizeof(float32)))
        << "the re-bake must actually move the ground, or the version bump proves nothing";
}

TEST(TerrainEffectPoolContracts, EditingAStampRespectClaimsReBakes)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    auto& terrainSvc = TerrainService::Get();
    auto& splineSvc = SplineECS::SplineService::Get();

    const TerrainHandle handle = CreateBakedTerrain(terrainSvc);
    auto* data = terrainSvc.GetTerrainData(handle);
    ASSERT_NE(data, nullptr);

    ECS::World world;
    CreateTerrainEntity(world, handle);
    AuthorClaimedRoad(world, splineSvc, 1.0f);

    const auto e = AddPadVolume(world, 0.0f, 0.0f, kPadRadius, 10.0f);
    Components::TerrainStampEffect fx{};
    fx.HeightScale = 12.0f;
    ApplyArm(fx, PoolArm::Pooled, "", /*respectClaims=*/false);
    world.AddComponentImmediate<Components::TerrainStampEffect>(e, fx);

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    const uint64 versionBefore = data->HeightfieldVersion;
    std::vector<float32> before(data->Heightfield.GetRawSamples(),
                                data->Heightfield.GetRawSamples()
                                    + data->Heightfield.GetSampleCount());

    {
        auto* stamp = world.GetComponentForWrite<Components::TerrainStampEffect>(e);
        ASSERT_NE(stamp, nullptr);
        stamp->RespectClaims = true;
    }

    system.Update(world, 1.0f / 60.0f);

    EXPECT_GT(data->HeightfieldVersion, versionBefore)
        << "toggling RespectClaims must re-bake";
    EXPECT_NE(0, std::memcmp(before.data(), data->Heightfield.GetRawSamples(),
                             before.size() * sizeof(float32)))
        << "the re-bake must actually move the ground over the claim";
}

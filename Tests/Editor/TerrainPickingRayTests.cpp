// Oracle for the terrain brush pick-ray march (ledger cleanup item 2). The legacy marcher
// stepped a fixed 2 m x 500 = 1000 m reach, so strokes on the far half of a large terrain
// silently missed. MarchTerrainSurface clips the ray to the terrain AABB and marches the
// traversed span with a bounded coarse-step + bisection budget, so a far-side hit on a 4 km
// terrain lands (discriminating: the > 1000 m case fails the legacy reach) while cost stays
// bounded regardless of terrain size.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "Picking/TerrainPicking.h"

#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTPlanetShading.h"
#include "CBTTerrainECS/CBTRenderFeature.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "TerrainECS/TerrainService.h"

using GameEngine::Mathematics::Ray3D;
using GameEngine::Mathematics::Vector3;
using GameEngine::Editor::Picking::MarchTerrainSurface;
using GameEngine::Editor::Picking::RaycastTerrain;
using GameEngine::Editor::Picking::SolvePlanetSurfaceHit;
using GameEngine::Editor::Picking::TerrainPickHit;

namespace
{
// A 4 km square terrain footprint centred at the origin, flat at y = 0 (a 1 m AABB Y margin
// mirrors the caller). The flat plane isolates the reach behaviour from height detail.
constexpr float kHalfExtent = 2000.0f;
const Vector3 kAabbMin(-kHalfExtent, -1.0f, -kHalfExtent);
const Vector3 kAabbMax(kHalfExtent, 1.0f, kHalfExtent);

bool FlatSurface(float wx, float wz, float& outY)
{
    if (wx < -kHalfExtent || wx > kHalfExtent || wz < -kHalfExtent || wz > kHalfExtent)
        return false;
    outY = 0.0f;
    return true;
}

Ray3D MakeRay(const Vector3& origin, Vector3 dir)
{
    const float len = dir.Length();
    if (len > 0.0f)
        dir = Vector3(dir.x / len, dir.y / len, dir.z / len);
    Ray3D r;
    r.origin = origin;
    r.direction = dir;
    return r;
}
} // namespace

// The far-side hit: a shallow descending ray reaches the flat surface ~2 km out — beyond the
// legacy 1000 m reach. The whole point of the fix.
TEST(TerrainPickingRay, FarSideHitBeyondLegacyReach)
{
    // From y = 100 descending at dir.y = -0.05 the ray reaches y = 0 at t = 2000 (unit dir).
    const Ray3D ray = MakeRay(Vector3(0.0f, 100.0f, -1950.0f), Vector3(0.0f, -0.05f, 1.0f));

    float t = 0.0f;
    Vector3 pos{};
    ASSERT_TRUE(MarchTerrainSurface(ray, kAabbMin, kAabbMax, 100000.0f, FlatSurface, t, pos));

    // Descending from y = 100 the ray reaches y = 0 at t = 100 / |dir.y| (direction is unit).
    const float expectedT = 100.0f / (-ray.direction.y);
    EXPECT_GT(t, 1000.0f);              // discriminating: the legacy 1000 m march never gets here
    EXPECT_NEAR(t, expectedT, 0.2f);
    EXPECT_NEAR(pos.y, 0.0f, 0.05f);   // lands ON the surface
    EXPECT_GE(pos.x, -kHalfExtent);
    EXPECT_LE(pos.z, kHalfExtent);
}

// A ray whose slab intersection lies entirely behind the origin (pointing up and away) never
// enters the AABB in +t, so the march rejects it for free.
TEST(TerrainPickingRay, RayMissingBoxReturnsFalse)
{
    const Ray3D ray = MakeRay(Vector3(0.0f, 100.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f));

    float t = 0.0f;
    Vector3 pos{};
    EXPECT_FALSE(MarchTerrainSurface(ray, kAabbMin, kAabbMax, 100000.0f, FlatSurface, t, pos));
}

// Straight-down from directly above the centre: hits the flat surface at t = origin.y.
TEST(TerrainPickingRay, VerticalHitOnFlatSurface)
{
    const Ray3D ray = MakeRay(Vector3(0.0f, 250.0f, 0.0f), Vector3(0.0f, -1.0f, 0.0f));

    float t = 0.0f;
    Vector3 pos{};
    ASSERT_TRUE(MarchTerrainSurface(ray, kAabbMin, kAabbMax, 100000.0f, FlatSurface, t, pos));
    EXPECT_NEAR(t, 250.0f, 0.1f);
    EXPECT_NEAR(pos.y, 0.0f, 0.05f);
}

// tMax caps the search: a hit that lies beyond the cap is not reported (a nearer terrain's
// hit already claimed the range).
TEST(TerrainPickingRay, RespectsTMaxCap)
{
    const Ray3D ray = MakeRay(Vector3(0.0f, 100.0f, -1950.0f), Vector3(0.0f, -0.05f, 1.0f));

    float t = 0.0f;
    Vector3 pos{};
    // The real hit is at ~2000; cap well below it.
    EXPECT_FALSE(MarchTerrainSurface(ray, kAabbMin, kAabbMax, 500.0f, FlatSurface, t, pos));
}

// ---- RaycastTerrain (world-level) domain filtering ----
//
// A spherical (planet) terrain can retain a residual heightfield (e.g. after a live
// planar -> spherical domain switch). The planar march must skip it — otherwise the
// planet is click-pickable as a phantom flat footprint at the entity origin.

namespace
{
using GameEngine::ECS::World;
using GameEngine::TerrainECS::TerrainService;
namespace Components = GameEngine::Components;

// Scoped TerrainService lifetime, mirroring the TerrainZoneStrokeCommandTests fixture
// (re-initialize defensively; always shut down so no state leaks across suites).
struct TerrainServiceScope
{
    TerrainServiceScope()
    {
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
        TerrainService::Initialize();
    }
    ~TerrainServiceScope()
    {
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
    }
};

constexpr float kTestTerrainSize = 64.0f;
constexpr float kTestHeightScale = 16.0f;

// Entity with a service-backed terrain (real, non-empty heightfield — CreateTerrain
// zero-fills it, i.e. a flat surface at the entity origin) centred at the world origin.
GameEngine::ECS::EntityHandle MakeHeightfieldTerrainEntity(World& world, TerrainService& svc,
                                                           Components::TerrainDomain domain)
{
    const auto config = GameEngine::Terrain::TerrainConfig::FromSamplesPerMeter(
        kTestTerrainSize, kTestTerrainSize, kTestHeightScale, /*samplesPerMeter*/ 1.0f);
    const GameEngine::TerrainECS::TerrainHandle handle = svc.CreateTerrain(config);

    Components::Terrain terrain{};
    terrain.SizeX = kTestTerrainSize;
    terrain.SizeZ = kTestTerrainSize;
    terrain.HeightScale = kTestHeightScale;
    terrain.Domain = domain;
    terrain.TerrainDataHandle = handle.Index;
    terrain.TerrainDataGeneration = handle.Generation;

    const GameEngine::ECS::EntityHandle e = world.CreateEntity();
    world.AddComponentImmediate(e, terrain);
    world.AddComponentImmediate(e, Components::WorldTransform{}); // identity: centred at origin
    return e;
}
} // namespace

// Harness sanity twin: the fabricated state IS planar-pickable when Domain == Planar.
// Without this, the spherical test below could pass vacuously on a broken setup.
TEST(TerrainPickingRay, PlanarTerrainWithHeightfieldIsPickable)
{
    TerrainServiceScope scope;
    World world;
    const auto entity = MakeHeightfieldTerrainEntity(world, TerrainService::Get(),
                                                     Components::TerrainDomain::Planar);

    const Ray3D ray = MakeRay(Vector3(0.0f, 100.0f, 0.0f), Vector3(0.0f, -1.0f, 0.0f));
    TerrainPickHit hit{};
    ASSERT_TRUE(RaycastTerrain(ray, world, 1000.0f, hit));
    EXPECT_EQ(hit.Entity.id, entity.id);
    EXPECT_NEAR(hit.WorldPosition.y, 0.0f, 0.1f); // flat zero-filled heightfield
}

// ---- Unclamped baked heights: the march slab must follow the real surface ----
//
// TerrainModifierSystem accumulates into a raw heightfield sample reference and never clamps
// (ApplyHeightEffectSample / ApplyHeightModifiers), and TerrainRenderFeature uploads the field
// verbatim as R32_FLOAT — so a HeightOffset volume draws ground BELOW the entity origin and a
// raise draws it ABOVE originY + HeightScale. A Y slab derived from the nominal [0,1] band
// excludes exactly that ground, and the two exclusions fail differently:
//   * below the floor every coarse sample sits above the surface, so the cast reports a silent
//     MISS — the spline conform then keeps the authored altitude and the piece floats;
//   * above the lid the first sample is already under the surface, so the cast brackets at once
//     and reports the LID, short of the ground — the piece is buried.
// Neither is visible downstream: PickResult carries no "clipped" signal.

namespace
{
using GameEngine::TerrainECS::TerrainHandle;

// Fill a service-backed terrain's heightfield with one normalized height and refresh the
// quadtree, mirroring what a modifier bake leaves behind — verbatim, unclamped. Returns the
// world-space Y the filled field describes.
float FillTerrainHeight(World& world, TerrainService& svc,
                        GameEngine::ECS::EntityHandle entity, float normalizedHeight)
{
    const auto* terrain = world.GetComponent<Components::Terrain>(entity);
    const TerrainHandle handle{terrain->TerrainDataHandle, terrain->TerrainDataGeneration};
    auto* data = svc.GetTerrainData(handle);
    float* samples = data->Heightfield.GetMutableSamples();
    const std::size_t count = data->Heightfield.GetSampleCount();
    for (std::size_t i = 0; i < count; ++i)
        samples[i] = normalizedHeight;
    svc.RebuildQuadtree(handle);
    return normalizedHeight * kTestHeightScale;
}
} // namespace

// A lowering volume: the drawn surface sits at -0.25 * 16 = -4 m, three metres under the floor
// the [0,1] assumption allowed (originY - 1). This is the floating-fence case — pre-fix the
// cast returned false and the conform silently kept the authored altitude.
TEST(TerrainPickingRay, PlanarPickFindsGroundLoweredBelowTheEntityOrigin)
{
    TerrainServiceScope scope;
    World world;
    auto& svc = TerrainService::Get();
    const auto entity = MakeHeightfieldTerrainEntity(world, svc, Components::TerrainDomain::Planar);
    const float surfaceY = FillTerrainHeight(world, svc, entity, -0.25f);
    ASSERT_LT(surfaceY, -1.0f) << "fixture must place the surface below the legacy slab floor";

    const Ray3D ray = MakeRay(Vector3(0.0f, 100.0f, 0.0f), Vector3(0.0f, -1.0f, 0.0f));
    TerrainPickHit hit{};
    ASSERT_TRUE(RaycastTerrain(ray, world, 1000.0f, hit))
        << "ground lowered below the entity origin reported a silent miss";
    EXPECT_EQ(hit.Entity.id, entity.id);
    EXPECT_NEAR(hit.WorldPosition.y, surfaceY, 0.1f);
}

// The mirror: a raise past HeightScale (1.5 * 16 = 24 m, lid was 17 m). This one never missed
// — it answered with the lid — so the assertion that discriminates is the POSITION, not the
// hit flag. That is the buried-path case.
TEST(TerrainPickingRay, PlanarPickFindsGroundRaisedAboveTheHeightScaleCeiling)
{
    TerrainServiceScope scope;
    World world;
    auto& svc = TerrainService::Get();
    const auto entity = MakeHeightfieldTerrainEntity(world, svc, Components::TerrainDomain::Planar);
    const float surfaceY = FillTerrainHeight(world, svc, entity, 1.5f);
    ASSERT_GT(surfaceY, kTestHeightScale + 1.0f)
        << "fixture must place the surface above the legacy slab lid";

    const Ray3D ray = MakeRay(Vector3(0.0f, 100.0f, 0.0f), Vector3(0.0f, -1.0f, 0.0f));
    TerrainPickHit hit{};
    ASSERT_TRUE(RaycastTerrain(ray, world, 1000.0f, hit));
    EXPECT_EQ(hit.Entity.id, entity.id);
    EXPECT_NEAR(hit.WorldPosition.y, surfaceY, 0.1f)
        << "the cast answered with the slab lid, not the ground the renderer draws";
}

// The scene that reported it: a 200 m terrain, HeightScale 30, entity at y = 0, carrying a
// volume whose HeightOffset is -6 m (-0.2 normalized). Wherever the base noise sits under
// 0.2 the baked sample goes negative and the ground leaves the nominal band. Fixed at the
// reported geometry rather than the tidy constants above, so the regression is pinned to the
// shape of the actual defect.
TEST(TerrainPickingRay, PlanarPickFindsGroundUnderAHeightOffsetVolumeAtSceneScale)
{
    TerrainServiceScope scope;
    World world;
    auto& svc = TerrainService::Get();

    constexpr float kSceneSize = 200.0f;
    constexpr float kSceneHeightScale = 30.0f;
    const auto config = GameEngine::Terrain::TerrainConfig::FromSamplesPerMeter(
        kSceneSize, kSceneSize, kSceneHeightScale, /*samplesPerMeter*/ 2.0f);
    const TerrainHandle handle = svc.CreateTerrain(config);

    Components::Terrain terrain{};
    terrain.SizeX = kSceneSize;
    terrain.SizeZ = kSceneSize;
    terrain.HeightScale = kSceneHeightScale;
    terrain.Domain = Components::TerrainDomain::Planar;
    terrain.TerrainDataHandle = handle.Index;
    terrain.TerrainDataGeneration = handle.Generation;
    const GameEngine::ECS::EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate(entity, terrain);
    world.AddComponentImmediate(entity, Components::WorldTransform{});

    // Base ground at 3 m (0.1 normalized), then the volume's -6 m: -0.1 normalized, y = -3.
    const float bakedNorm = 0.1f + (-6.0f / kSceneHeightScale);
    auto* data = svc.GetTerrainData(handle);
    float* samples = data->Heightfield.GetMutableSamples();
    const std::size_t count = data->Heightfield.GetSampleCount();
    for (std::size_t i = 0; i < count; ++i)
        samples[i] = bakedNorm;
    svc.RebuildQuadtree(handle);
    const float surfaceY = bakedNorm * kSceneHeightScale;

    // The conform ray's own geometry: it starts 100 m above an authored sample at y = 6 and
    // probes 1000 m down (SplineSurfaceConform's kConformRayLift / kConformRayMaxDistance).
    const Ray3D ray = MakeRay(Vector3(10.0f, 6.0f + 100.0f, -20.0f), Vector3(0.0f, -1.0f, 0.0f));
    TerrainPickHit hit{};
    ASSERT_TRUE(RaycastTerrain(ray, world, 1000.0f, hit))
        << "the conform ray found no ground under a lowered volume — the reported miss";
    EXPECT_EQ(hit.Entity.id, entity.id);
    EXPECT_NEAR(hit.WorldPosition.y, surfaceY, 0.1f);
}

// The discriminating case: identical state except Domain == Spherical must NOT return a
// planar pick hit. Pre-guard, the residual heightfield produced a phantom hit at t = 100.
TEST(TerrainPickingRay, SphericalTerrainWithResidualHeightfieldIsNotPlanarPicked)
{
    TerrainServiceScope scope;
    World world;
    MakeHeightfieldTerrainEntity(world, TerrainService::Get(),
                                 Components::TerrainDomain::Spherical);

    const Ray3D ray = MakeRay(Vector3(0.0f, 100.0f, 0.0f), Vector3(0.0f, -1.0f, 0.0f));
    TerrainPickHit hit{};
    EXPECT_FALSE(RaycastTerrain(ray, world, 1000.0f, hit))
        << "spherical terrain's residual heightfield must not be planar-pickable";
}

// The anti-phantom guard, made positive: the residual footprint is placed 5 km up so the
// planar march and the planet sphere answer at unmistakably different places. Only the
// footprint answer is forbidden — whatever the sphere path returns is fine.
TEST(TerrainPickingRay, SphericalTerrainNeverReportsItsResidualFootprintPosition)
{
    TerrainServiceScope scope;
    World world;
    constexpr float kFootprintY = 5000.0f;

    const auto config = GameEngine::Terrain::TerrainConfig::FromSamplesPerMeter(
        kTestTerrainSize, kTestTerrainSize, kTestHeightScale, /*samplesPerMeter*/ 1.0f);
    const GameEngine::TerrainECS::TerrainHandle handle =
        TerrainService::Get().CreateTerrain(config);

    Components::Terrain terrain{};
    terrain.SizeX = kTestTerrainSize;
    terrain.SizeZ = kTestTerrainSize;
    terrain.HeightScale = kTestHeightScale;
    terrain.Domain = Components::TerrainDomain::Spherical;
    terrain.TerrainDataHandle = handle.Index;
    terrain.TerrainDataGeneration = handle.Generation;

    Components::WorldTransform xf{}; // identity, then lifted on Y
    xf.matrix[13] = kFootprintY;

    const GameEngine::ECS::EntityHandle e = world.CreateEntity();
    world.AddComponentImmediate(e, terrain);
    world.AddComponentImmediate(e, xf);

    const Ray3D ray = MakeRay(Vector3(0.0f, kFootprintY + 100.0f, 0.0f), Vector3(0.0f, -1.0f, 0.0f));
    TerrainPickHit hit{};
    const bool anyHit = RaycastTerrain(ray, world, 100000.0f, hit);
    EXPECT_FALSE(anyHit && std::abs(hit.WorldPosition.y - kFootprintY) < 1.0f)
        << "the flat footprint of a spherical terrain is not drawn, so it is not a pick target";
}

// ---- SolvePlanetSurfaceHit: the pick lands on the DISPLACED surface ----
//
// The analytic ray/sphere root sits on the reference sphere, which ignores relief and
// sculpt. These lock the refinement onto the surface the renderer actually displaces to,
// using the production evaluator (CBTRenderFeature::SampleSphereSurfaceHeight) rather than a
// copy of the height math. The feature is constructed device-free: SampleSphereSurfaceHeight
// reads only the domain config SetActive pushes plus TerrainService's sculpt layer, and an
// uninitialized feature's teardown is guarded on a null device.

namespace
{
using GameEngine::CBTTerrainECS::CBTRenderFeature;

constexpr float kTestPlanetRadius = 2000.0f;

// Tune a feature to a spherical planet exactly as CBTUpdateSystem tunes it from the Terrain
// component. Relief defaults (60 m amplitude, 4 octaves) are the shipped ones.
void TuneSphericalPlanet(CBTRenderFeature& feature)
{
    CBTRenderFeature::DomainConfig domain{};
    domain.DomainMode = GameEngine::CBTTerrain::kDomainSpherical;
    domain.PlanetRadius = kTestPlanetRadius;
    feature.SetActive(true, GameEngine::CBTTerrain::CBTClassifyDesc{}, 8.0f,
                      GameEngine::Components::kNoTerrainSeaLevel, domain, 0u);
}

// A direction whose relief is far from zero, so "landed on the base sphere" and "landed on
// the displaced surface" are separated by tens of metres rather than by rounding.
Vector3 DirectionWithStrongRelief(const CBTRenderFeature& feature, float& outHeight)
{
    Vector3 best(0.0f, 1.0f, 0.0f);
    outHeight = 0.0f;
    constexpr int kSteps = 24;
    for (int i = 0; i < kSteps; ++i)
    {
        for (int j = 0; j < kSteps; ++j)
        {
            const float theta = 3.14159265f * (static_cast<float>(i) + 0.5f) / kSteps;
            const float phi = 6.2831853f * static_cast<float>(j) / kSteps;
            const Vector3 dir(std::sin(theta) * std::cos(phi), std::cos(theta),
                              std::sin(theta) * std::sin(phi));
            const float h = feature.SampleSphereSurfaceHeight(dir.x, dir.y, dir.z);
            if (std::abs(h) > std::abs(outHeight))
            {
                outHeight = h;
                best = dir;
            }
        }
    }
    return best;
}
} // namespace

// The discriminating test for the requirement "picks hit the displaced surface": a ray fired
// straight at the planet centre along a direction with strong relief must land at radius +
// relief, not at radius.
TEST(TerrainPickingRay, PlanetHitLandsOnDisplacedSurfaceNotTheReferenceSphere)
{
    TerrainServiceScope scope; // SampleSphereSurfaceHeight composes the service's sculpt layer
    CBTRenderFeature feature;
    TuneSphericalPlanet(feature);

    float relief = 0.0f;
    const Vector3 dir = DirectionWithStrongRelief(feature, relief);
    ASSERT_GT(std::abs(relief), 10.0f) << "sampling found no relief to discriminate against";

    // Fire inward along -dir from well outside the planet.
    const Vector3 origin(dir.x * kTestPlanetRadius * 3.0f, dir.y * kTestPlanetRadius * 3.0f,
                         dir.z * kTestPlanetRadius * 3.0f);
    const Ray3D ray = MakeRay(origin, Vector3(-dir.x, -dir.y, -dir.z));

    float t = 0.0f;
    Vector3 pos{}, normal{};
    ASSERT_TRUE(SolvePlanetSurfaceHit(ray, kTestPlanetRadius, 100000.0f,
                                      [&feature](float dx, float dy, float dz) {
                                          return feature.SampleSphereSurfaceHeight(dx, dy, dz);
                                      },
                                      t, pos, normal));

    const float hitRadius = pos.Length();
    EXPECT_NEAR(hitRadius, kTestPlanetRadius + relief, 0.5f);
    // Discriminating: the base-sphere answer is wrong by the whole relief.
    EXPECT_GT(std::abs(hitRadius - kTestPlanetRadius), 10.0f);
    // The outward normal is radial.
    EXPECT_NEAR(Vector3::Dot(normal, dir), 1.0f, 1e-3f);
    // t is in the ray's own units (the ray is unit-length here).
    EXPECT_NEAR(t, kTestPlanetRadius * 3.0f - hitRadius, 0.5f);
}

// A ray aimed past the planet reports no hit rather than a point in empty space.
TEST(TerrainPickingRay, PlanetSolveMissesWhenTheRayClearsTheSphere)
{
    TerrainServiceScope scope;
    CBTRenderFeature feature;
    TuneSphericalPlanet(feature);

    // Offset the origin far to the side and fire parallel to the planet's "vertical".
    const Ray3D ray = MakeRay(Vector3(kTestPlanetRadius * 4.0f, kTestPlanetRadius * 4.0f, 0.0f),
                              Vector3(0.0f, -1.0f, 0.0f));
    float t = 0.0f;
    Vector3 pos{}, normal{};
    EXPECT_FALSE(SolvePlanetSurfaceHit(ray, kTestPlanetRadius, 100000.0f,
                                       [&feature](float dx, float dy, float dz) {
                                           return feature.SampleSphereSurfaceHeight(dx, dy, dz);
                                       },
                                       t, pos, normal));
}

// tMax caps the planet solve the same way it caps the planar march, so a mesh in front of
// the planet still wins the pick.
TEST(TerrainPickingRay, PlanetSolveRespectsTMaxCap)
{
    TerrainServiceScope scope;
    CBTRenderFeature feature;
    TuneSphericalPlanet(feature);
    auto heightFn = [&feature](float dx, float dy, float dz) {
        return feature.SampleSphereSurfaceHeight(dx, dy, dz);
    };

    const Ray3D ray = MakeRay(Vector3(0.0f, kTestPlanetRadius * 3.0f, 0.0f),
                              Vector3(0.0f, -1.0f, 0.0f));
    float t = 0.0f;
    Vector3 pos{}, normal{};
    ASSERT_TRUE(SolvePlanetSurfaceHit(ray, kTestPlanetRadius, 100000.0f, heightFn, t, pos, normal));
    ASSERT_GT(t, 1000.0f);

    float cappedT = 0.0f;
    EXPECT_FALSE(SolvePlanetSurfaceHit(ray, kTestPlanetRadius, t * 0.5f, heightFn, cappedT, pos,
                                       normal));
}

// t is reported in the caller's ray parameter units, not in unit-direction units — the same
// contract MarchTerrainSurface documents, so RaycastTerrain can compare the two domains'
// hits against one `bestT`.
TEST(TerrainPickingRay, PlanetSolveReportsTInTheRaysOwnUnits)
{
    TerrainServiceScope scope;
    CBTRenderFeature feature;
    TuneSphericalPlanet(feature);
    auto heightFn = [&feature](float dx, float dy, float dz) {
        return feature.SampleSphereSurfaceHeight(dx, dy, dz);
    };

    Ray3D unitRay;
    unitRay.origin = Vector3(0.0f, kTestPlanetRadius * 3.0f, 0.0f);
    unitRay.direction = Vector3(0.0f, -1.0f, 0.0f);

    Ray3D scaledRay = unitRay;
    scaledRay.direction = Vector3(0.0f, -4.0f, 0.0f); // same ray, direction scaled 4x

    float tUnit = 0.0f, tScaled = 0.0f;
    Vector3 posUnit{}, posScaled{}, normal{};
    ASSERT_TRUE(SolvePlanetSurfaceHit(unitRay, kTestPlanetRadius, 100000.0f, heightFn, tUnit,
                                      posUnit, normal));
    ASSERT_TRUE(SolvePlanetSurfaceHit(scaledRay, kTestPlanetRadius, 100000.0f, heightFn, tScaled,
                                      posScaled, normal));

    EXPECT_NEAR(tScaled, tUnit * 0.25f, 1e-3f);
    EXPECT_NEAR(posScaled.y, posUnit.y, 1e-2f);
}

// ---- SolvePlanetSurfaceHit with the ray origin INSIDE the reference sphere ----
//
// PlanetRelief is signed — its base noise spans [-1.5, 1.5] of the amplitude — so the ground
// the renderer draws dips below the reference sphere across every valley. A camera flying low
// over one is therefore inside that sphere, and the analytic ray/sphere solve has no near root
// there at all: its only non-negative root is where the ray leaves the sphere on the FAR side
// of the planet. That is the ordinary pose for a camera near the surface, not an edge case.

namespace
{
// The direction of the deepest valley on the tuned planet, sampled through the production
// height evaluator so the test cannot disagree with the renderer about where the ground is.
Vector3 DeepestValleyDirection(const CBTRenderFeature& feature, float& outHeight)
{
    Vector3 best(0.0f, 1.0f, 0.0f);
    outHeight = 0.0f;
    constexpr int kSteps = 96;
    for (int i = 0; i < kSteps; ++i)
    {
        for (int j = 0; j < kSteps; ++j)
        {
            const float theta = 3.14159265f * (static_cast<float>(i) + 0.5f) / kSteps;
            const float phi = 6.2831853f * static_cast<float>(j) / kSteps;
            const Vector3 dir(std::sin(theta) * std::cos(phi), std::cos(theta),
                              std::sin(theta) * std::sin(phi));
            const float h = feature.SampleSphereSurfaceHeight(dir.x, dir.y, dir.z);
            if (h < outHeight)
            {
                outHeight = h;
                best = dir;
            }
        }
    }
    return best;
}

// The direction of the highest peak — the sibling pose that sits OUTSIDE the reference sphere.
Vector3 HighestPeakDirection(const CBTRenderFeature& feature, float& outHeight)
{
    Vector3 best(0.0f, 1.0f, 0.0f);
    outHeight = 0.0f;
    constexpr int kSteps = 96;
    for (int i = 0; i < kSteps; ++i)
    {
        for (int j = 0; j < kSteps; ++j)
        {
            const float theta = 3.14159265f * (static_cast<float>(i) + 0.5f) / kSteps;
            const float phi = 6.2831853f * static_cast<float>(j) / kSteps;
            const Vector3 dir(std::sin(theta) * std::cos(phi), std::cos(theta),
                              std::sin(theta) * std::sin(phi));
            const float h = feature.SampleSphereSurfaceHeight(dir.x, dir.y, dir.z);
            if (h > outHeight)
            {
                outHeight = h;
                best = dir;
            }
        }
    }
    return best;
}

// A unit vector tangent to `up` — the local "east" a camera looks along toward the horizon.
Vector3 TangentTo(const Vector3& up)
{
    const Vector3 seed =
        (std::abs(up.z) < 0.9f) ? Vector3(0.0f, 0.0f, 1.0f) : Vector3(1.0f, 0.0f, 0.0f);
    return Vector3(up.y * seed.z - up.z * seed.y, up.z * seed.x - up.x * seed.z,
                   up.x * seed.y - up.y * seed.x)
        .Normalize();
}
} // namespace

// The discriminating case: a camera 25 m over a valley floor, looking 45 degrees down at the
// ground just ahead. The hit must be that ground. The far-root answer is ~2.8 km away on the
// opposite hemisphere, so both the distance and the hemisphere checks separate them.
TEST(TerrainPickingRay, PlanetPickFromInsideTheReferenceSphereHitsTheGroundAhead)
{
    TerrainServiceScope scope;
    CBTRenderFeature feature;
    TuneSphericalPlanet(feature);
    auto heightFn = [&feature](float dx, float dy, float dz) {
        return feature.SampleSphereSurfaceHeight(dx, dy, dz);
    };

    float valleyH = 0.0f;
    const Vector3 up = DeepestValleyDirection(feature, valleyH);
    ASSERT_LT(valleyH, -30.0f) << "no valley below the reference sphere to fly into";

    const float camRadius = kTestPlanetRadius + valleyH + 25.0f;
    ASSERT_LT(camRadius, kTestPlanetRadius) << "camera is not inside the reference sphere";
    const Vector3 origin(up.x * camRadius, up.y * camRadius, up.z * camRadius);

    const Vector3 east = TangentTo(up);
    const Ray3D ray = MakeRay(origin, Vector3(east.x - up.x, east.y - up.y, east.z - up.z));

    float t = 0.0f;
    Vector3 pos{}, normal{};
    ASSERT_TRUE(SolvePlanetSurfaceHit(ray, kTestPlanetRadius, 100000.0f, heightFn, t, pos, normal));

    EXPECT_GT(Vector3::Dot(pos.Normalize(), up), 0.99f)
        << "pick landed on the far hemisphere instead of on the ground in front of the camera";
    EXPECT_LT(t, 500.0f) << "the ground 25 m below a 45-degree look is ~35 m out, not " << t;
    // Surface membership. True of the far-root answer as well, so it is the other half of the
    // requirement rather than the discriminator: the hit must be ON the drawn ground.
    EXPECT_NEAR(pos.Length(), kTestPlanetRadius + heightFn(pos.x, pos.y, pos.z), 0.5f);
}

// The same pose looking straight down, where the answer is exact: the surface direction never
// changes along the ray, so the hit is at the camera's altitude above the valley floor. This
// is a precision lock, NOT a discriminator — a radial ray is the one case the far-root seed
// recovers from on its own, because RefinePlanetHit re-solves each step and its own near-root
// preference walks the answer back through the planet centre onto the near side.
TEST(TerrainPickingRay, PlanetPickFromInsideTheReferenceSphereLandsAtTheCameraAltitude)
{
    TerrainServiceScope scope;
    CBTRenderFeature feature;
    TuneSphericalPlanet(feature);
    auto heightFn = [&feature](float dx, float dy, float dz) {
        return feature.SampleSphereSurfaceHeight(dx, dy, dz);
    };

    float valleyH = 0.0f;
    const Vector3 up = DeepestValleyDirection(feature, valleyH);
    ASSERT_LT(valleyH, -30.0f);

    constexpr float kAltitude = 40.0f;
    const float camRadius = kTestPlanetRadius + valleyH + kAltitude;
    ASSERT_LT(camRadius, kTestPlanetRadius);
    const Vector3 origin(up.x * camRadius, up.y * camRadius, up.z * camRadius);
    const Ray3D ray = MakeRay(origin, Vector3(-up.x, -up.y, -up.z));

    float t = 0.0f;
    Vector3 pos{}, normal{};
    ASSERT_TRUE(SolvePlanetSurfaceHit(ray, kTestPlanetRadius, 100000.0f, heightFn, t, pos, normal));

    EXPECT_NEAR(t, kAltitude, 0.05f);
    EXPECT_NEAR(pos.Length(), kTestPlanetRadius + valleyH, 0.05f);
    EXPECT_GT(Vector3::Dot(normal, up), 0.9999f);
}

// A camera under the ground has no surface in front of it. Answering with the reference
// sphere's far root would put the pick on the opposite hemisphere.
TEST(TerrainPickingRay, PlanetSolveReportsNothingFromUnderTheSurface)
{
    TerrainServiceScope scope;
    CBTRenderFeature feature;
    TuneSphericalPlanet(feature);
    auto heightFn = [&feature](float dx, float dy, float dz) {
        return feature.SampleSphereSurfaceHeight(dx, dy, dz);
    };

    float valleyH = 0.0f;
    const Vector3 up = DeepestValleyDirection(feature, valleyH);
    ASSERT_LT(valleyH, -30.0f);

    const float camRadius = kTestPlanetRadius + valleyH - 20.0f;
    const Vector3 origin(up.x * camRadius, up.y * camRadius, up.z * camRadius);
    const Ray3D ray = MakeRay(origin, Vector3(-up.x, -up.y, -up.z));

    float t = 0.0f;
    Vector3 pos{}, normal{};
    EXPECT_FALSE(SolvePlanetSurfaceHit(ray, kTestPlanetRadius, 100000.0f, heightFn, t, pos, normal));
}

// Control for the split: the mirror-image pose OUTSIDE the reference sphere — a camera the
// same 25 m over the highest peak, looking 45 degrees down — keeps answering with the ground
// in front of it. This is the analytic-seed path and must be untouched by the chord march.
TEST(TerrainPickingRay, PlanetPickFromOverAPeakStillHitsTheGroundAhead)
{
    TerrainServiceScope scope;
    CBTRenderFeature feature;
    TuneSphericalPlanet(feature);
    auto heightFn = [&feature](float dx, float dy, float dz) {
        return feature.SampleSphereSurfaceHeight(dx, dy, dz);
    };

    float peakH = 0.0f;
    const Vector3 up = HighestPeakDirection(feature, peakH);
    ASSERT_GT(peakH, 30.0f) << "no peak above the reference sphere to fly over";

    const float camRadius = kTestPlanetRadius + peakH + 25.0f;
    ASSERT_GT(camRadius, kTestPlanetRadius) << "camera is not outside the reference sphere";
    const Vector3 origin(up.x * camRadius, up.y * camRadius, up.z * camRadius);

    const Vector3 east = TangentTo(up);
    const Ray3D ray = MakeRay(origin, Vector3(east.x - up.x, east.y - up.y, east.z - up.z));

    float t = 0.0f;
    Vector3 pos{}, normal{};
    ASSERT_TRUE(SolvePlanetSurfaceHit(ray, kTestPlanetRadius, 100000.0f, heightFn, t, pos, normal));
    EXPECT_GT(Vector3::Dot(pos.Normalize(), up), 0.99f);
    EXPECT_LT(t, 1000.0f);
}

// ---- The OUTSIDE branch: one analytic re-solve seeded on the reference sphere ----
//
// Outside the reference sphere the solve seeds RefinePlanetHit with the analytic entry point on
// that sphere and evaluates the height there. Each refinement iteration takes the near root of
// the sphere of radius + h(current point), so it follows the drawn ground only while that
// corrected sphere is still reachable. Over ground that dips below the reference radius the
// corrected sphere is the SMALLER one, and a shallow ray can miss it entirely: the discriminant
// goes negative on the first iteration, the loop breaks, and the seed is returned unchanged. The
// pick is then a hit reported ON the reference sphere, short of the drawn ground — a wrong
// answer, not a miss.
//
// These two pin the measured magnitude against a dense march of the drawn surface, so the size
// of the error is a fact the next lane inherits rather than a claim it has to re-derive. Both go
// red when the outside branch is fixed: replace the pins with the corrected behaviour, never
// loosen them.

namespace
{
// The relief envelope in metres. PlanetRelief normalizes its octave sum, so |height| <=
// kReliefEnvelope * amplitude for any octave count, and radius + this bounds every point the
// renderer can draw.
float ReliefEnvelope(const CBTRenderFeature& feature)
{
    return GameEngine::CBTTerrain::kReliefEnvelope * feature.GetDomainConfig().ReliefAmplitude;
}

// Independent first-crossing oracle for the DISPLACED surface. The search is clipped to the
// envelope shell (radius + ReliefEnvelope) that bounds all drawn ground, then coarse-stepped and
// bisected on the signed gap |p| - (radius + h(p)). The shell clip is what makes it an oracle
// for near-limb rays: the coarse step shrinks with the traversed chord, so a grazing ray is
// sampled finely exactly where the production solve is coarsest.
bool OracleFirstGroundHit(const CBTRenderFeature& feature, const Vector3& o, const Vector3& d,
                          float radius, float& outT)
{
    const float shell = radius + ReliefEnvelope(feature);
    const float b = Vector3::Dot(o, d);
    const float c = Vector3::Dot(o, o) - shell * shell;
    const float disc = b * b - c;
    if (disc < 0.0f)
        return false; // never reaches anything the renderer can draw
    const float sq = std::sqrt(disc);
    const float tEnter = std::max(-b - sq, 0.0f);
    const float tExit = -b + sq;
    if (!(tExit > tEnter))
        return false;

    auto gap = [&](float t) {
        const Vector3 p(o.x + d.x * t, o.y + d.y * t, o.z + d.z * t);
        return p.Length() - (radius + feature.SampleSphereSurfaceHeight(p.x, p.y, p.z));
    };
    if (gap(tEnter) <= 0.0f)
        return false; // already at or under the ground: no surface ahead

    constexpr int kOracleSteps = 2048;
    constexpr int kOracleBisections = 30;
    const float step = (tExit - tEnter) / static_cast<float>(kOracleSteps);
    float loT = tEnter;
    for (int i = 1; i <= kOracleSteps; ++i)
    {
        const float t = tEnter + step * static_cast<float>(i);
        if (gap(t) > 0.0f)
        {
            loT = t;
            continue;
        }
        float hiT = t;
        for (int r = 0; r < kOracleBisections; ++r)
        {
            const float mid = (loT + hiT) * 0.5f;
            if (gap(mid) > 0.0f)
                loT = mid;
            else
                hiT = mid;
        }
        outT = (loT + hiT) * 0.5f;
        return true;
    }
    return false;
}
} // namespace

// A camera half a metre above the reference sphere, over a valley floor 56.11 m below it,
// looking 10 degrees down. The drawn ground is 339.60 m ahead; the pick answers 2.89 m — the
// entry point on the reference sphere, 336.71 m short — and reports it as a hit.
TEST(TerrainPickingRay, PlanetPickJustOutsideTheReferenceSphereLandsOnTheReferenceSphere)
{
    TerrainServiceScope scope;
    CBTRenderFeature feature;
    TuneSphericalPlanet(feature);
    auto heightFn = [&feature](float dx, float dy, float dz) {
        return feature.SampleSphereSurfaceHeight(dx, dy, dz);
    };

    float valleyH = 0.0f;
    const Vector3 up = DeepestValleyDirection(feature, valleyH);
    ASSERT_LT(valleyH, -30.0f) << "no valley below the reference sphere to look across";

    constexpr float kAltitudeAboveReference = 0.5f;
    const float camRadius = kTestPlanetRadius + kAltitudeAboveReference;
    ASSERT_GT(camRadius, kTestPlanetRadius) << "this must take the analytic outside branch";
    const Vector3 origin = up * camRadius;

    constexpr float kDepressionRadians = 10.0f * 3.14159265f / 180.0f;
    const Vector3 east = TangentTo(up);
    const Vector3 dir = east * std::cos(kDepressionRadians) - up * std::sin(kDepressionRadians);

    float oracleT = 0.0f;
    ASSERT_TRUE(OracleFirstGroundHit(feature, origin, dir, kTestPlanetRadius, oracleT));

    Ray3D ray;
    ray.origin = origin;
    ray.direction = dir;
    float t = 0.0f;
    Vector3 pos{}, normal{};
    ASSERT_TRUE(SolvePlanetSurfaceHit(ray, kTestPlanetRadius, 100000.0f, heightFn, t, pos, normal));

    EXPECT_NEAR(valleyH, -56.11f, 0.05f);
    EXPECT_NEAR(oracleT, 339.60f, 0.5f);
    EXPECT_NEAR(t, 2.89f, 0.05f);
    EXPECT_NEAR(oracleT - t, 336.71f, 0.5f)
        << "the outside branch answers short of the drawn ground by this much";
    // The reported hit sits on the REFERENCE sphere: RefinePlanetHit's corrected sphere (radius
    // + a negative height) is unreachable, so the seed survives untouched.
    EXPECT_NEAR(pos.Length(), kTestPlanetRadius, 0.5f);
    EXPECT_GT(std::abs(pos.Length() - (kTestPlanetRadius + heightFn(pos.x, pos.y, pos.z))), 50.0f)
        << "a hit that far off the drawn surface is a wrong hit, not a miss";
}

// The same failure swept across the whole visible disc from a 1.5 R orbit: the impact parameter
// runs from the sub-camera point out to the envelope shell at each of 32 azimuths, and every ray
// whose drawn ground the oracle finds is compared with the pick. The worst positional error is
// 175.81 m — nearly twice the +/- 90 m relief envelope — at 95% of the silhouette, and 33 of the
// 3937 drawn rays are not reported at all: they clear the reference sphere, so the analytic
// solve has no root to seed even though the renderer draws ground along them.
TEST(TerrainPickingRay, PlanetPickNearTheLimbLandsShortOfTheDrawnGround)
{
    TerrainServiceScope scope;
    CBTRenderFeature feature;
    TuneSphericalPlanet(feature);
    auto heightFn = [&feature](float dx, float dy, float dz) {
        return feature.SampleSphereSurfaceHeight(dx, dy, dz);
    };

    // The whole visible disc from one orbit pose, not one great circle: the limb error is a
    // function of the terrain the ray grazes, so a single azimuth samples one path across the
    // relief and can miss the worst case entirely.
    const Vector3 up(0.0f, 1.0f, 0.0f);
    const Vector3 axisA(1.0f, 0.0f, 0.0f);
    const Vector3 axisB(0.0f, 0.0f, 1.0f);
    const Vector3 origin = up * (kTestPlanetRadius * 1.5f);
    const float shell = kTestPlanetRadius + ReliefEnvelope(feature);
    const float originLength = origin.Length();

    constexpr int kAzimuths = 32;
    constexpr int kImpactSteps = 128;
    int   drawnRays = 0;
    int   unreportedRays = 0;
    int   worstAzimuth = -1;
    int   worstImpact = -1;
    float worstError = 0.0f;
    for (int a = 0; a < kAzimuths; ++a)
    {
        const float phi =
            6.2831853f * (static_cast<float>(a) + 0.5f) / static_cast<float>(kAzimuths);
        const Vector3 east = axisA * std::cos(phi) + axisB * std::sin(phi);
        for (int i = 0; i < kImpactSteps; ++i)
        {
            // Impact parameter as a fraction of the silhouette: 0 straight down at the
            // sub-camera point, 1 grazing the outermost ground the renderer can draw.
            const float fraction =
                (static_cast<float>(i) + 0.5f) / static_cast<float>(kImpactSteps);
            const float alpha = std::asin(fraction * shell / originLength);
            const Vector3 dir = east * std::sin(alpha) - up * std::cos(alpha);

            float oracleT = 0.0f;
            if (!OracleFirstGroundHit(feature, origin, dir, kTestPlanetRadius, oracleT))
                continue; // the renderer draws no ground along this ray
            ++drawnRays;

            Ray3D ray;
            ray.origin = origin;
            ray.direction = dir;
            float t = 0.0f;
            Vector3 pos{}, normal{};
            if (!SolvePlanetSurfaceHit(ray, kTestPlanetRadius, 100000.0f, heightFn, t, pos,
                                       normal))
            {
                ++unreportedRays;
                continue;
            }

            const float error = (origin + dir * oracleT - pos).Length();
            if (error > worstError)
            {
                worstError = error;
                worstAzimuth = a;
                worstImpact = i;
            }
        }
    }

    const float worstFraction =
        (static_cast<float>(worstImpact) + 0.5f) / static_cast<float>(kImpactSteps);

    EXPECT_EQ(drawnRays, 3937);
    EXPECT_EQ(unreportedRays, 33)
        << "a ray that clears the reference sphere but grazes a relief peak has no analytic seed";
    EXPECT_NEAR(worstError, 175.81f, 0.5f) << "worst at azimuth index " << worstAzimuth;
    EXPECT_NEAR(worstFraction, 0.949f, 0.01f);
    EXPECT_GT(worstError, ReliefEnvelope(feature))
        << "the error is not bounded by the relief envelope";
}

// ---- The sculpt brush keeps its own planet cast ----
//
// The brush hands a cast's normal to ApplySphereDab as the surface DIRECTION to sculpt, so on
// a planet it needs a cast that answers only about the planet. Picking::RaycastTerrain is the
// combined scene-level cast: it prefers a planar terrain's hit (whose normal is the planar
// convention, world +Y — the north pole as a sculpt direction), it is gated on the CBT render
// feature's own domain state, and it is capped by the caller's pick reach. Routing the brush
// through it moved sculpt dabs. Unifying the two is a change that has to prove equivalence
// first.
//
// Both of the tool's cast entry points must honour that: RaycastSurface (the pointer path's
// camera ray) and ProbeSurfaceAt (the automation path's probe ray). Each dispatches on
// IsSphericalPlanet, so the lock is positional — the planet branch has to come first and cast
// through RaycastPlanet, leaving the combined cast reachable only once the planet case is
// ruled out.
//
// TerrainBrushTool.cpp is in no test target (its dependency cone pulls in the undo service,
// change notifications and zone authoring) and Picking::RaycastActivePlanet reaches
// RenderServices through the Engine singleton, so this contract has no reachable behavioural
// seam. It is locked at the source level, the way PanelDefaultTabIconTests locks the panel
// tab-icon defaults.
TEST(TerrainPickingRay, SphereBrushCursorCastsThroughTheBrushsOwnPlanetRaycast)
{
    const std::filesystem::path brushSource = std::filesystem::path(GE_EDITOR_SOURCE_DIR) /
                                              "Source" / "SceneView" / "TerrainBrushTool.cpp";
    std::ifstream in(brushSource, std::ios::binary);
    ASSERT_TRUE(in.good()) << "cannot read " << brushSource.string();
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string source = buffer.str();

    // Each entry point's body, delimited by the next function definition after it.
    struct CastEntryPoint
    {
        const char* Signature;
        const char* NextSignature;
    };
    const CastEntryPoint entryPoints[] = {
        {"bool TerrainBrushTool::RaycastSurface", "\nbool TerrainBrushTool::ProbeSurfaceAt"},
        {"bool TerrainBrushTool::ProbeSurfaceAt", "\nbool TerrainBrushTool::ApplyStrokePhase"},
    };

    for (const CastEntryPoint& entry : entryPoints)
    {
        const std::size_t bodyBegin = source.find(entry.Signature);
        ASSERT_NE(bodyBegin, std::string::npos)
            << entry.Signature << " is gone; re-point this lock at whatever replaced it";
        const std::size_t bodyEnd = source.find(entry.NextSignature, bodyBegin);
        ASSERT_NE(bodyEnd, std::string::npos)
            << entry.NextSignature << " is gone; re-point this lock";
        const std::string body = source.substr(bodyBegin, bodyEnd - bodyBegin);

        const std::size_t domainGate = body.find("IsSphericalPlanet(");
        const std::size_t planetCast = body.find("RaycastPlanet(");
        const std::size_t terrainCast = body.find("RaycastTerrain(");

        EXPECT_NE(domainGate, std::string::npos)
            << entry.Signature << " must dispatch on the active terrain's domain";
        EXPECT_NE(planetCast, std::string::npos)
            << entry.Signature << " must cast through TerrainBrushTool::RaycastPlanet on a planet";
        EXPECT_LT(domainGate, planetCast)
            << entry.Signature << " must rule the domain BEFORE casting";
        EXPECT_LT(planetCast, terrainCast)
            << entry.Signature << " reaches the combined scene terrain cast before the planet "
               "cast: that cast returns a planar terrain's up-normal as the sculpt direction, is "
               "gated on the CBT feature's domain state, and is capped by the pick reach";
    }
}

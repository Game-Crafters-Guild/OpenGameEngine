// Oracles for the terrain usability slice: the auto-derived MaxDepth formula (incl.
// the decode-precision ceiling clamp), the internal MaxDepthOverride, and the creation
// presets yielding a rendering-ready configuration — plus the live-edit handle
// contract of TerrainSchema::ApplyProperty (the path IPC set_component drives).

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "CBTTerrain/CBTDeepDecode.h"
#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTSphereRoots.h"
#include "CBTTerrainECS/TerrainProvisioning.h"
#include "Components/Terrain/Terrain.h"
#include "Core/EngineLoggerBridge.h"
#include "Logger/LogSink.h"
#include "Logger/Logger.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Terrain/TerrainModifiers.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Scene/SceneIOContext.h"
#include "Scene/SceneSchemaRegistry.h"
#include "TerrainECS/Scene/TerrainSceneSchemas.h"
#include "TerrainECS/TerrainService.h"
#include "Components/Name.h"
#include "Components/Terrain/TerrainGrass.h"
#include "PhysicsECS/Components/HeightFieldColliderShape.h"
#include "TerrainECS/TerrainDefaultSurfaceRules.h"
#include "TerrainECS/TerrainEntityProvisioning.h"
#include "TerrainECS/TerrainSizingPlan.h"

using GameEngine::ECS::World;
using GameEngine::Components::Terrain;
using GameEngine::Components::TerrainDebugView;
using GameEngine::Components::TerrainDomain;
using GameEngine::Components::TerrainPlanetRelief;
using GameEngine::CBTTerrain::CBTSurfaceParams;
using GameEngine::CBTTerrainECS::DeriveTerrainMaxDepth;
using GameEngine::CBTTerrainECS::FindActiveTerrain;
using GameEngine::CBTTerrainECS::MakePlanetReliefPreset;
using GameEngine::CBTTerrainECS::MakeTerrainPreset;
using GameEngine::CBTTerrainECS::ResolveActivePlanetRelief;
using GameEngine::CBTTerrainECS::ImportedHeightSource;
using GameEngine::CBTTerrainECS::ResolveTerrainMaxDepth;
using GameEngine::CBTTerrainECS::SubdivCapFor;
using GameEngine::CBTTerrainECS::TerrainPreset;

namespace
{
// Planar base depth 1, spherical 5; the decode-precision ceiling is baseDepth + the cap the
// ACTIVE gVertex representation carries: kMaxDecodeSubdiv (40 — the fp32-world-store cap,
// raised from the old fp32-exact rung of 23 in slice 1) with the flag off, and
// DeepDecode::kDeepDecodeSubdiv (50 — the (sector, local) df64 store, decode-precision arc
// S2b) for a SPHERICAL terrain with the flag on. Every deepDecode=false expectation below is
// pinned UNCHANGED from before the S2b lift (the #617 rule: the flag off must be bywise the
// shipped behavior).
constexpr uint32_t kPlanarCeiling = 1u + GameEngine::CBTTerrain::kMaxDecodeSubdiv;  // 41
constexpr uint32_t kSphereCeiling = 5u + GameEngine::CBTTerrain::kMaxDecodeSubdiv;  // 45
constexpr uint32_t kSphereDeepCeiling =
    5u + GameEngine::CBTTerrain::DeepDecode::kDeepDecodeSubdiv; // 55
} // namespace

// ---- Planar: derived cap tracks the provisioned heightfield lattice ----

TEST(TerrainMaxDepth, PlanarTracksSampleResolution)
{
    // subdiv = round(2*log2(lattice intervals)); depth = 1 + subdiv. These sizes provision
    // exactly size*spm intervals.
    EXPECT_EQ(DeriveTerrainMaxDepth(TerrainDomain::Planar, 256.0f, 256.0f, 2.0f, 0.0f, false, false), 19u);  // 2*log2(512)=18
    EXPECT_EQ(DeriveTerrainMaxDepth(TerrainDomain::Planar, 512.0f, 512.0f, 2.0f, 0.0f, false, false), 21u);  // 2*log2(1024)=20
    EXPECT_EQ(DeriveTerrainMaxDepth(TerrainDomain::Planar, 1024.0f, 1024.0f, 2.0f, 0.0f, false, false), 23u); // 2*log2(2048)=22
}

TEST(TerrainMaxDepth, PlanarClampsToDecodeCeiling)
{
    // subdiv = round(2*log2(intervals)); it pins at kMaxDecodeSubdiv (40) once intervals >= 2^20.
    // 2^20 @ 1spm -> 2*log2(2^20)=40 subdiv -> depth 41 (the ceiling exactly).
    EXPECT_EQ(DeriveTerrainMaxDepth(TerrainDomain::Planar, 1048576.0f, 1048576.0f, 1.0f, 0.0f, false, false), kPlanarCeiling);
    // 2^22 @ 2spm -> deeper than the cap, still pinned at the ceiling.
    EXPECT_EQ(DeriveTerrainMaxDepth(TerrainDomain::Planar, 4194304.0f, 4194304.0f, 2.0f, 0.0f, false, false), kPlanarCeiling);
    // A 4 km @ 1 spm terrain (2*log2(4096)=24) now derives BELOW the ceiling — the old cap (23)
    // clamped it, the raised cap lets the 1 m sample-spacing target through at depth 25.
    EXPECT_EQ(DeriveTerrainMaxDepth(TerrainDomain::Planar, 4096.0f, 4096.0f, 1.0f, 0.0f, false, false), 25u);
}

TEST(TerrainMaxDepth, PlanarNeverBelowBaseDepth)
{
    // A zero-size terrain provisions no heightfield and floors at the base depth, never below.
    EXPECT_EQ(DeriveTerrainMaxDepth(TerrainDomain::Planar, 0.0f, 0.0f, 0.0f, 0.0f, false, false), 1u);
    // The smallest heightfield provisioned is 65 x 65 samples, so even a 1 m terrain at 1 spm
    // holds 64 intervals: 2*log2(64) = 12, depth 13.
    EXPECT_EQ(DeriveTerrainMaxDepth(TerrainDomain::Planar, 1.0f, 1.0f, 1.0f, 0.0f, false, false), 13u);
}

// ---- Spherical: derived cap scales with radius, pins at the ceiling for big planets ----

TEST(TerrainMaxDepth, SphericalScalesWithRadius)
{
    // subdiv = round(2*log2(pi*R/2 / 0.25)); depth = 5 + subdiv (target 0.25 m facet, #603).
    EXPECT_EQ(DeriveTerrainMaxDepth(TerrainDomain::Spherical, 0.0f, 0.0f, 0.0f, 250.0f, false, false), 26u);
    EXPECT_EQ(DeriveTerrainMaxDepth(TerrainDomain::Spherical, 0.0f, 0.0f, 0.0f, 500.0f, false, false), 28u);
    EXPECT_EQ(DeriveTerrainMaxDepth(TerrainDomain::Spherical, 0.0f, 0.0f, 0.0f, 1000.0f, false, false), 30u);
}

TEST(TerrainMaxDepth, SphericalClampsToDecodeCeiling)
{
    // subdiv = round(2*log2(pi*R/2 / 0.25)); it pins at kMaxDecodeSubdiv (40) once pi*R/2 >= 0.25*2^20,
    // i.e. R >= ~167 km. Mid-size planets (2-50 km) derive BELOW the ceiling — the 0.25 m facet
    // target (#603) lands R=50 km at depth 42.
    EXPECT_EQ(DeriveTerrainMaxDepth(TerrainDomain::Spherical, 0.0f, 0.0f, 0.0f, 50000.0f, false, false), 42u);
    EXPECT_EQ(DeriveTerrainMaxDepth(TerrainDomain::Spherical, 0.0f, 0.0f, 0.0f, 700000.0f, false, false), kSphereCeiling);
    EXPECT_EQ(DeriveTerrainMaxDepth(TerrainDomain::Spherical, 0.0f, 0.0f, 0.0f, 6371000.0f, false, false), kSphereCeiling); // Earth
}

TEST(TerrainMaxDepth, SphericalDeeperThanPlanarCeiling)
{
    // The spherical path must be allowed past the planar ceiling (41): the sphere base depth (5)
    // is higher, so an Earth-scale planet reaches heap depth 45, four levels past the planar cap.
    EXPECT_GT(DeriveTerrainMaxDepth(TerrainDomain::Spherical, 0.0f, 0.0f, 0.0f, 6371000.0f, false, false), kPlanarCeiling);
}

// ---- Deep-decode cap lift (decode-precision arc S2b): flag ON -> the SPHERICAL ceiling
// lifts 40 -> 50, because the (sector, local) df64 gVertex store carries those depths
// (measured: decode error <= 2e-5 m at Earth vs the fp32 world store's ~0.25 m). ----

TEST(TerrainMaxDepth, DeepDecodeLiftsSphericalCeilingOnly)
{
    // Earth: the unclamped 0.25 m facet derivation wants round(2*log2(pi*6.371e6/2 / 0.25))
    // = 51, so it clamps at the deep cap 50 -> depth 55. Facet floor (pi*R/2)*2^-25 =
    // 0.298 m — under the 0.5 m walking bar (the arc's product moment: was 9.54 m at 45).
    EXPECT_EQ(DeriveTerrainMaxDepth(TerrainDomain::Spherical, 0.0f, 0.0f, 0.0f, 6371000.0f, true, false),
              kSphereDeepCeiling);
    // 700 km: wants round(2*log2(pi*3.5e5 / 0.25)) = 44 — ceiling-clamped to 45 under the
    // fp32 store (44 > 40), derives FREE at depth 49 under the deep cap (44 < 50).
    EXPECT_EQ(DeriveTerrainMaxDepth(TerrainDomain::Spherical, 0.0f, 0.0f, 0.0f, 700000.0f, true, false), 49u);
    // 50 km: derives 37 < 40 in BOTH modes -> depth 42 either way. The no-regression guard:
    // scenes the fp32 cap already served must not re-tessellate when the flag flips.
    EXPECT_EQ(DeriveTerrainMaxDepth(TerrainDomain::Spherical, 0.0f, 0.0f, 0.0f, 50000.0f, true, false), 42u);
    EXPECT_EQ(DeriveTerrainMaxDepth(TerrainDomain::Spherical, 0.0f, 0.0f, 0.0f, 50000.0f, false, false), 42u);
    // Planar IGNORES the flag: the deep (sector, local) store is spherical-only (planar
    // VertexEval never takes the df64 path), so its fp32-world cap must hold at 41.
    EXPECT_EQ(DeriveTerrainMaxDepth(TerrainDomain::Planar, 1048576.0f, 1048576.0f, 1.0f, 0.0f, true, false),
              kPlanarCeiling);
    EXPECT_EQ(DeriveTerrainMaxDepth(TerrainDomain::Planar, 4194304.0f, 4194304.0f, 2.0f, 0.0f, true, false),
              kPlanarCeiling);
}

// ---- Narrow-heap arm: a device without 64-bit shader integers runs the u32-heap kernels,
// where the ceiling is the heap ID's own range rather than a precision preference. This is
// the value get_terrain_stats reports as subdivCeiling, so it is pinned directly. ----

TEST(TerrainMaxDepth, NarrowHeapCeilingIsTheHeapIdRangeInEveryDomain)
{
    constexpr uint32_t kNarrow = GameEngine::CBTTerrain::kHeap32DecodeSubdiv;

    // Domain and the deep-decode flag are both moot on the narrow arm: the deep store is
    // int64 by construction, so it does not exist there.
    EXPECT_EQ(SubdivCapFor(TerrainDomain::Planar, false, true), kNarrow);
    EXPECT_EQ(SubdivCapFor(TerrainDomain::Spherical, false, true), kNarrow);
    EXPECT_EQ(SubdivCapFor(TerrainDomain::Spherical, true, true), kNarrow);

    // The wide arm is what the narrow one gives up, and it must stay where it was.
    EXPECT_EQ(SubdivCapFor(TerrainDomain::Planar, false, false),
              GameEngine::CBTTerrain::kMaxDecodeSubdiv);
    EXPECT_EQ(SubdivCapFor(TerrainDomain::Spherical, true, false),
              GameEngine::CBTTerrain::DeepDecode::kDeepDecodeSubdiv);
    EXPECT_LT(kNarrow, GameEngine::CBTTerrain::kMaxDecodeSubdiv);

    // And it reaches the derived cap: a planet big enough to want every subdivision the wide
    // arm allows lands on the narrow ceiling instead.
    EXPECT_EQ(DeriveTerrainMaxDepth(TerrainDomain::Spherical, 0.0f, 0.0f, 0.0f, 6371000.0f, false, true),
              5u + kNarrow);
    EXPECT_EQ(DeriveTerrainMaxDepth(TerrainDomain::Planar, 1048576.0f, 1048576.0f, 1.0f, 0.0f, false, true),
              1u + kNarrow);
}

TEST(TerrainMaxDepth, DeepDecodeOverrideClampFollowsRepresentation)
{
    // The MaxDepthOverride escape hatch clamps to the representation ceiling too: the deep
    // flag lifts the spherical clamp to 55 and leaves planar at 41; flag off is the
    // unchanged 45/41.
    Terrain sphere{};
    sphere.Domain = TerrainDomain::Spherical;
    sphere.MaxDepthOverride = 99u;
    EXPECT_EQ(ResolveTerrainMaxDepth(sphere, {}, true, false), kSphereDeepCeiling);
    EXPECT_EQ(ResolveTerrainMaxDepth(sphere, {}, false, false), kSphereCeiling);

    Terrain planar{};
    planar.Domain = TerrainDomain::Planar;
    planar.MaxDepthOverride = 99u;
    EXPECT_EQ(ResolveTerrainMaxDepth(planar, {}, true, false), kPlanarCeiling);
    EXPECT_EQ(ResolveTerrainMaxDepth(planar, {}, false, false), kPlanarCeiling);
}

// ---- The flag->cap coupling oracle (S2b, can-fail): the cap each mode hands out must be a
// depth whose STORAGE REPRESENTATION carries the facets — measured here, not assumed. ----

namespace
{
// Compact twin of the CBTDeepDecodeTests S1 probe machinery (same sample set, same
// methodology) so this suite can measure the representation error at the derived cap.
uint64_t MakeSphereHeapID(uint32_t rootIndex, uint64_t pathBits, uint32_t subdiv)
{
    const uint64_t rootHeap =
        (uint64_t(1) << GameEngine::CBTTerrain::kSphereBaseDepth) + rootIndex;
    const uint64_t mask = (subdiv >= 64u) ? ~uint64_t(0) : ((uint64_t(1) << subdiv) - 1u);
    return (rootHeap << subdiv) | (pathBits & mask);
}

double Dist3(const double a[3], const double b[3])
{
    const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

constexpr uint32_t kSampleRoots[3] = {0u, 9u, 17u};
constexpr uint64_t kSamplePaths[3] = {0xAAAAAAAAAAAAAAAAull, 0x5555555555555555ull,
                                      0x6DB6DB6DB6DB6DB6ull};
} // namespace

TEST(TerrainMaxDepth, DecodeCapMatchesWhatTheRepresentationCanCarry)
{
    // The invariant that makes the flag->cap coupling load-bearing: at the cap either mode
    // derives at Earth radius, the mode's OWN storage representation must keep its decode/
    // store error under 25% of the facet short edge (the S1 degeneracy criterion; err/short
    // was 8.6% at the fp32 cap 40 and 275% at 50 — design §1's measured table). CAN-FAIL:
    // force the flag-off spherical cap to 50 (e.g. by lifting kMaxDecodeSubdiv or breaking
    // SubdivCapFor's coupling) and the fp32 leg below measures ~2.7x the facet -> red.
    namespace DD = GameEngine::CBTTerrain::DeepDecode;
    using GameEngine::CBTTerrain::BuildSphereRoots;
    using GameEngine::CBTTerrain::kSphereBaseDepth;
    constexpr double kEarthR = 6.371e6;
    const auto roots = BuildSphereRoots();

    const uint32_t offCap =
        DeriveTerrainMaxDepth(TerrainDomain::Spherical, 0.0f, 0.0f, 0.0f, 6371000.0f, false, false) -
        kSphereBaseDepth;
    const uint32_t onCap =
        DeriveTerrainMaxDepth(TerrainDomain::Spherical, 0.0f, 0.0f, 0.0f, 6371000.0f, true, false) -
        kSphereBaseDepth;
    ASSERT_EQ(offCap, GameEngine::CBTTerrain::kMaxDecodeSubdiv);
    ASSERT_EQ(onCap, DD::kDeepDecodeSubdiv);
    // The int64 LEB walk is exact only to numSubdiv ~52 (scale = 2^52 < 2^63) — no cap may
    // ever exceed it, in any mode.
    ASSERT_LE(onCap, 52u);

    for (const bool deep : {false, true})
    {
        const uint32_t cap = deep ? onCap : offCap;
        double maxErr = 0.0;
        double minShort = 1e30;
        for (uint32_t root : kSampleRoots)
        {
            for (uint64_t path : kSamplePaths)
            {
                const uint64_t h = MakeSphereHeapID(root, path, cap);
                const DD::Bary bary = DD::WalkBary(h, kSphereBaseDepth);
                double c[3][3];
                for (int corner = 0; corner < 3; ++corner)
                {
                    const DD::CubeNum num = DD::CornerNumerator(bary, corner, roots[root]);
                    DD::DecodeExact(num, kEarthR, 0.0, c[corner]);
                    if (!deep)
                    {
                        // fp32 world store: one float cast per component (what legacy
                        // Kernel_VertexEval persists into gVertex).
                        const double f[3] = {double(float(c[corner][0])),
                                             double(float(c[corner][1])),
                                             double(float(c[corner][2]))};
                        maxErr = std::max(maxErr, Dist3(c[corner], f));
                    }
                    else
                    {
                        // Deep (sector, local) store: df64 decode, then reconstruct the
                        // sector-local remainder against the exact geometry (origin = the
                        // corner's own sector -> the reconstruction is just Local).
                        const DD::DeepVertex v = DD::DecodeCorner(
                            num, static_cast<float>(kEarthR), 0.0f);
                        double err2 = 0.0;
                        for (int k = 0; k < 3; ++k)
                        {
                            const double exactLocal =
                                c[corner][k] - 1024.0 * static_cast<double>(v.Sector[k]);
                            const double d = exactLocal - static_cast<double>(v.Local[k]);
                            err2 += d * d;
                        }
                        maxErr = std::max(maxErr, std::sqrt(err2));
                    }
                }
                const double e01 = Dist3(c[0], c[1]);
                const double e12 = Dist3(c[1], c[2]);
                const double e20 = Dist3(c[2], c[0]);
                minShort = std::min({minShort, e01, e12, e20});
            }
        }
        const double ratio = maxErr / minShort;
        std::printf("[cap-coupling] deep=%d capSubdiv=%u shortEdge=%.4f m reprErr(max)=%.3g m "
                    "err/short=%.2f%%\n",
                    deep ? 1 : 0, cap, minShort, maxErr, 100.0 * ratio);
        EXPECT_LT(ratio, 0.25) << (deep ? "deep (sector, local) store" : "fp32 world store")
                               << " cannot carry the facets at its own derived cap ("
                               << cap << ") — the flag->cap coupling is broken";
    }
}

// ---- MaxDepthOverride: the internal debug/test knob ----

TEST(TerrainMaxDepth, OverrideWinsWhenSet)
{
    Terrain t{};
    t.Domain = TerrainDomain::Spherical;
    t.PlanetRadius = 50000.0f; // would auto-derive to 42
    t.MaxDepthOverride = 12u;
    EXPECT_EQ(ResolveTerrainMaxDepth(t, {}, false, false), 12u);
}

TEST(TerrainMaxDepth, OverrideClampsToDomainCeilingAndFloor)
{
    Terrain sphere{};
    sphere.Domain = TerrainDomain::Spherical;
    sphere.MaxDepthOverride = 99u;
    EXPECT_EQ(ResolveTerrainMaxDepth(sphere, {}, false, false), kSphereCeiling); // clamp high
    sphere.MaxDepthOverride = 2u;
    EXPECT_EQ(ResolveTerrainMaxDepth(sphere, {}, false, false), 5u);             // clamp to sphere base depth

    Terrain planar{};
    planar.Domain = TerrainDomain::Planar;
    planar.MaxDepthOverride = 99u;
    EXPECT_EQ(ResolveTerrainMaxDepth(planar, {}, false, false), kPlanarCeiling);
}

TEST(TerrainMaxDepth, ZeroOverrideFallsBackToAuto)
{
    Terrain t{};
    t.Domain = TerrainDomain::Planar;
    t.SizeX = 512.0f;
    t.SizeZ = 512.0f;
    t.SamplesPerMeter = 2.0f;
    t.MaxDepthOverride = 0u; // default: auto
    EXPECT_EQ(ResolveTerrainMaxDepth(t, {}, false, false),
              DeriveTerrainMaxDepth(TerrainDomain::Planar, 512.0f, 512.0f, 2.0f, 0.0f, false, false));
}

TEST(TerrainMaxDepth, PlanarAutoUsesLongerAxis)
{
    // A rectangular terrain derives from max(SizeX, SizeZ) — extraction sizes the
    // heightfield from the longer axis, so the cap must track it, not SizeX alone.
    Terrain t{};
    t.Domain = TerrainDomain::Planar;
    t.SizeX = 256.0f;
    t.SizeZ = 1024.0f;
    t.SamplesPerMeter = 2.0f;
    EXPECT_EQ(ResolveTerrainMaxDepth(t, {}, false, false),
              DeriveTerrainMaxDepth(TerrainDomain::Planar, 1024.0f, 1024.0f, 2.0f, 0.0f, false, false));
    EXPECT_NE(ResolveTerrainMaxDepth(t, {}, false, false),
              DeriveTerrainMaxDepth(TerrainDomain::Planar, 256.0f, 256.0f, 2.0f, 0.0f, false, false));
}

namespace
{
// The planar leg length at a depth: two LEB levels halve a leg, depth 1 is the two twin roots
// whose legs span the longer axis.
float PlanarCapLegMetres(float longerAxisMetres, uint32_t depth)
{
    return longerAxisMetres * std::pow(2.0f, -0.5f * static_cast<float>(depth - 1u));
}

Terrain MakePlanar(float sizeX, float sizeZ, float samplesPerMeter)
{
    Terrain t{};
    t.Domain = TerrainDomain::Planar;
    t.SizeX = sizeX;
    t.SizeZ = sizeZ;
    t.SamplesPerMeter = samplesPerMeter;
    return t;
}
} // namespace

TEST(TerrainMaxDepth, PlanarCapReachesTheProvisionedLattice)
{
    // 576 m at 1 sample per metre: the heightfield provisions 1025 x 1025 samples (577 needed,
    // rounded up to 64 x 2^k + 1), a 0.5625 m lattice. The cap's legs must reach that spacing
    // (2 x log2(1024) = 20 subdivisions, depth 21); deriving from the authored density
    // (2 x log2(576) = 18.3, depth 19) stops at 1.125 m legs, every other sample.
    const Terrain lakeside = MakePlanar(576.0f, 576.0f, 1.0f);
    EXPECT_EQ(ResolveTerrainMaxDepth(lakeside, {}, false, false), 21u);

    // Every single-heightfield terrain: the cap's leg equals the lattice spacing the sizing plan
    // provisions, whatever the authored size and density.
    const float sizes[] = {100.0f, 300.0f, 576.0f, 700.0f, 1000.0f};
    const float densities[] = {0.5f, 1.0f};
    for (const float size : sizes)
        for (const float density : densities)
        {
            const GameEngine::TerrainECS::TerrainSizingPlan plan =
                GameEngine::TerrainECS::DeriveTerrainSizingPlan(size, size, density);
            ASSERT_EQ(plan.Source, GameEngine::TerrainECS::TerrainHeightSource::Single)
                << size << " m at " << density;
            const uint32_t cap = ResolveTerrainMaxDepth(MakePlanar(size, size, density), {}, false, false);
            EXPECT_NEAR(PlanarCapLegMetres(size, cap), plan.MetresPerTexelNear,
                        plan.MetresPerTexelNear * 1e-4f)
                << size << " m at " << density << " spm: cap " << cap;
        }
}

TEST(TerrainMaxDepth, PlanarTiledCapFollowsTheTileLattice)
{
    // Tiles are capped at 1024 interior samples, so a terrain whose shorter axis is below one
    // tile's world size gets tiles of that shorter axis, re-rounded to 64 x 2^k intervals:
    // 3000 x 400 m at 1 sample per metre provisions 400 m tiles at 512 intervals (0.78125 m), so
    // the longer axis holds 3840 intervals: round(2 x log2(3840)) = 24, depth 25. The CBT grid
    // does not align with the tile lattice, so the cap is the nearest level, not an exact match:
    // the finest leg is 3000 / 2^12 = 0.73 m on the 0.78 m lattice.
    const GameEngine::TerrainECS::TerrainSizingPlan plan =
        GameEngine::TerrainECS::DeriveTerrainSizingPlan(3000.0f, 400.0f, 1.0f);
    ASSERT_NE(plan.Source, GameEngine::TerrainECS::TerrainHeightSource::Single);
    ASSERT_FLOAT_EQ(plan.MetresPerTexelNear, 0.78125f);
    EXPECT_EQ(ResolveTerrainMaxDepth(MakePlanar(3000.0f, 400.0f, 1.0f), {}, false, false), 25u);
}

TEST(TerrainMaxDepth, PlanarImportedHeightmapCapsAtTheImportsSpacing)
{
    // An imported 1 m heightmap (577 x 577 samples) stretched over 576 m at 1 sample per metre:
    // the lattice is 0.5625 m (cap 21), the data 1 m. Refining past the source adds triangles and
    // no detail, so the cap follows the import: 576 intervals, 2 x log2(576) = 18.3 rounded up to
    // 19 subdivisions, depth 20 (0.795 m legs, the first level at or below 1 m; depth 19's
    // 1.125 m legs would skip source samples).
    const Terrain lakeside = MakePlanar(576.0f, 576.0f, 1.0f);
    EXPECT_EQ(ResolveTerrainMaxDepth(lakeside, ImportedHeightSource{577u, 577u}, false, false), 20u);

    // Heights from modifiers, sculpting, noise or a flat base have the lattice as their source.
    EXPECT_EQ(ResolveTerrainMaxDepth(lakeside, {}, false, false), 21u);

    // An import finer than the lattice changes nothing: the lattice already caps it.
    EXPECT_EQ(ResolveTerrainMaxDepth(lakeside, ImportedHeightSource{4097u, 4097u}, false, false), 21u);

    // A rectangular terrain reads the import along its longer axis, as the root legs do.
    Terrain wide = MakePlanar(576.0f, 288.0f, 1.0f);
    EXPECT_EQ(ResolveTerrainMaxDepth(wide, ImportedHeightSource{577u, 2049u}, false, false), 20u);

    // An import whose interval count is a power of two caps exactly at its level: 513 samples on
    // 512 m at 2 samples per metre (lattice 1024 intervals, cap 21) gives 2 x log2(512) = 18,
    // depth 19.
    EXPECT_EQ(ResolveTerrainMaxDepth(MakePlanar(512.0f, 512.0f, 2.0f), ImportedHeightSource{513u, 513u},
                                     false, false),
              19u);

    // Tiled terrains keep their lattice cap, with or without an import (3000 x 400 m at 1 spm, the
    // 0.78 m tile lattice): their tiles read the import, but their CBT spans the tile grid, which
    // can overhang the footprint the import is measured on
    // (PlanarTiledCapKeepsEveryImportSampleOnTheOverhangingGrid).
    EXPECT_EQ(ResolveTerrainMaxDepth(MakePlanar(3000.0f, 400.0f, 1.0f), {}, false, false), 25u);
    EXPECT_EQ(ResolveTerrainMaxDepth(MakePlanar(3000.0f, 400.0f, 1.0f), ImportedHeightSource{577u, 577u},
                                     false, false),
              25u);
}

TEST(TerrainMaxDepth, PlanarTiledCapKeepsEveryImportSampleOnTheOverhangingGrid)
{
    // A tiled terrain's tiles read its import, but its CBT spans the tile grid, and the grid
    // rounds up past the footprint (#2609): 576 m at 2 samples per metre is 2 x 2 tiles of 512 m,
    // 1024 m a side. A 577 x 577 import (1 m samples) measured on the footprint caps at depth 20,
    // whose legs on the 1024 m grid are 1.41 m and skip import samples. The tiled terrain keeps its
    // lattice cap, 21, whose 1 m legs show every one.
    const GameEngine::TerrainECS::TerrainSizingPlan plan =
        GameEngine::TerrainECS::DeriveTerrainSizingPlan(576.0f, 576.0f, 2.0f);
    ASSERT_NE(plan.Source, GameEngine::TerrainECS::TerrainHeightSource::Single);
    const float gridMetres =
        static_cast<float>(std::max(plan.TilesPerAxisX, plan.TilesPerAxisZ)) * plan.TileWorldSize;
    ASSERT_FLOAT_EQ(gridMetres, 1024.0f);

    const float importSpacing = 576.0f / 576.0f;
    const uint32_t cap = ResolveTerrainMaxDepth(MakePlanar(576.0f, 576.0f, 2.0f),
                                                ImportedHeightSource{577u, 577u}, false, false);
    EXPECT_LE(PlanarCapLegMetres(gridMetres, cap), importSpacing * (1.0f + 1e-4f))
        << "cap " << cap << " skips import samples on the tile grid";
    EXPECT_EQ(cap, 21u);
}

// ---- Creation presets: each yields a rendering-ready configuration ----

TEST(TerrainPresets, SmallPlanarIsDefaultScale)
{
    const Terrain t = MakeTerrainPreset(TerrainPreset::SmallPlanar);
    EXPECT_EQ(t.Domain, TerrainDomain::Planar);
    EXPECT_FLOAT_EQ(t.SizeX, 512.0f);
    EXPECT_FLOAT_EQ(t.SizeZ, 512.0f);
    EXPECT_FLOAT_EQ(t.SamplesPerMeter, 2.0f);
    EXPECT_EQ(t.MaxDepthOverride, 0u);     // auto
    EXPECT_EQ(t.TerrainDataHandle, 0u);    // not provisioned yet
    EXPECT_EQ(ResolveTerrainMaxDepth(t, {}, false, false), 21u);
}

TEST(TerrainPresets, EveryPresetStartsAtTheDefaultTarget)
{
    // 11 px at 1080 rows: the reference implementation's 60 px^2 split area as an edge length.
    for (const TerrainPreset preset : {TerrainPreset::SmallPlanar, TerrainPreset::LargePlanar,
                                       TerrainPreset::Planet5km, TerrainPreset::Planet50km})
        EXPECT_FLOAT_EQ(MakeTerrainPreset(preset).TargetPixelError, 11.0f);
}

TEST(TerrainPresets, LargePlanarTilesAndHoldsTexelDensity)
{
    const Terrain t = MakeTerrainPreset(TerrainPreset::LargePlanar);
    EXPECT_EQ(t.Domain, TerrainDomain::Planar);
    EXPECT_FLOAT_EQ(t.SizeX, 4096.0f);
    EXPECT_FLOAT_EQ(t.SamplesPerMeter, 1.0f); // 1 spm for 4 km+
    EXPECT_FLOAT_EQ(t.MaterialTiling, 80.0f); // ~10 per 512 m
    // 4096 @ 1spm -> 2*log2(4096)=24 subdiv -> depth 25 (below the raised ceiling; the old 23 cap
    // pinned it at 24).
    EXPECT_EQ(ResolveTerrainMaxDepth(t, {}, false, false), 25u);
}

TEST(TerrainPresets, Planet5kmIsSphericalWithReliefAndAutoDepth)
{
    const Terrain t = MakeTerrainPreset(TerrainPreset::Planet5km);
    EXPECT_EQ(t.Domain, TerrainDomain::Spherical);
    EXPECT_FLOAT_EQ(t.PlanetRadius, 5000.0f);
    EXPECT_EQ(t.MaxDepthOverride, 0u);
    // R=5 km -> round(2*log2(pi*2500/0.25))=30 subdiv -> depth 35 (0.25 m facet target, #603).
    EXPECT_EQ(ResolveTerrainMaxDepth(t, {}, false, false), 35u);

    // The base relief is a companion component now (MakePlanetReliefPreset), not a
    // Terrain field — same authored values, just relocated.
    const TerrainPlanetRelief r = MakePlanetReliefPreset(TerrainPreset::Planet5km);
    EXPECT_FLOAT_EQ(r.Amplitude, 125.0f); // ~= R/40
    EXPECT_EQ(r.Octaves, 4u); // readable-detail default (was 3; 5+ lattices the sin noise)
}

TEST(TerrainPresets, Planet50kmIsSphericalWithScaledRelief)
{
    const Terrain t = MakeTerrainPreset(TerrainPreset::Planet50km);
    EXPECT_EQ(t.Domain, TerrainDomain::Spherical);
    EXPECT_FLOAT_EQ(t.PlanetRadius, 50000.0f);
    // R=50 km -> round(2*log2(pi*25000/0.25))=37 subdiv -> depth 42 (0.25 m facet target, #603).
    EXPECT_EQ(ResolveTerrainMaxDepth(t, {}, false, false), 42u);

    const TerrainPlanetRelief r = MakePlanetReliefPreset(TerrainPreset::Planet50km);
    EXPECT_FLOAT_EQ(r.Amplitude, 1250.0f); // ~= R/40
    EXPECT_EQ(r.Octaves, 4u); // readable-detail default (was 3; 5+ lattices the sin noise)
}

// A planar preset carries no relief — MakePlanetReliefPreset returns the inert default.
TEST(TerrainPresets, PlanarPresetReliefIsDefault)
{
    const TerrainPlanetRelief r = MakePlanetReliefPreset(TerrainPreset::SmallPlanar);
    EXPECT_FLOAT_EQ(r.Amplitude, TerrainPlanetRelief{}.Amplitude);
    EXPECT_FLOAT_EQ(r.Frequency, TerrainPlanetRelief{}.Frequency);
    EXPECT_EQ(r.Octaves, TerrainPlanetRelief{}.Octaves);
}

// ---- FindActiveTerrain: the ONE resolver the renderer + brush share ----

namespace
{
GameEngine::ECS::EntityHandle AddTerrain(World& world, bool enabled, TerrainDomain domain,
                                         uint32_t singleHandle, uint32_t tiledHandle)
{
    const auto e = world.CreateEntity();
    Terrain t{};
    t.Domain = domain;
    t.PlanetRadius = 7000.0f;
    t.TerrainDataHandle = singleHandle;
    t.TiledTerrainHandle = tiledHandle;
    world.AddComponentImmediate(e, t);
    GameEngine::ECS::Entity(&world, e).SetEnabled<Terrain>(enabled);
    return e;
}
} // namespace

TEST(FindActiveTerrain, EmptyWorldReturnsFalse)
{
    World w;
    Terrain out{};
    EXPECT_FALSE(FindActiveTerrain(w, out));
}

TEST(FindActiveTerrain, EnabledLiveTerrainIsReturnedWithFields)
{
    World w;
    AddTerrain(w, /*enabled*/ true, TerrainDomain::Spherical, /*single*/ 1u, /*tiled*/ 0u);
    Terrain out{};
    ASSERT_TRUE(FindActiveTerrain(w, out));
    EXPECT_EQ(out.Domain, TerrainDomain::Spherical);
    EXPECT_FLOAT_EQ(out.PlanetRadius, 7000.0f);
}

// The divergence this unification fixes: the old brush predicate matched ANY enabled
// spherical terrain (no liveness), so an un-provisioned planet could route the brush
// to sphere-sculpt while the renderer tuned a different (live) terrain. The shared
// resolver requires liveness, so a handle-less terrain is not "active".
TEST(FindActiveTerrain, EnabledButNotLiveIsSkipped)
{
    World w;
    AddTerrain(w, /*enabled*/ true, TerrainDomain::Spherical, /*single*/ 0u, /*tiled*/ 0u);
    Terrain out{};
    EXPECT_FALSE(FindActiveTerrain(w, out));
}

TEST(FindActiveTerrain, DisabledLiveIsSkipped)
{
    World w;
    AddTerrain(w, /*enabled*/ false, TerrainDomain::Planar, /*single*/ 1u, /*tiled*/ 0u);
    Terrain out{};
    EXPECT_FALSE(FindActiveTerrain(w, out));
}

TEST(FindActiveTerrain, TiledHandleCountsAsLive)
{
    World w;
    AddTerrain(w, /*enabled*/ true, TerrainDomain::Planar, /*single*/ 0u, /*tiled*/ 3u);
    Terrain out{};
    EXPECT_TRUE(FindActiveTerrain(w, out));
}

// ---- ResolveActivePlanetRelief: the single relief source CBTUpdateSystem feeds into
// the CBT DomainConfig (and the collider / far-clip framing read the same). ----

// The relief the renderer draws with is the active terrain's TerrainPlanetRelief
// component — this is the DomainConfig source (CBTUpdateSystem reads it here).
TEST(ResolveActivePlanetRelief, ReturnsActiveTerrainReliefComponent)
{
    World w;
    const auto e = AddTerrain(w, /*enabled*/ true, TerrainDomain::Spherical, /*single*/ 1u, /*tiled*/ 0u);
    TerrainPlanetRelief relief{};
    relief.Amplitude = 321.0f;
    relief.Frequency = 9.0f;
    relief.Octaves = 5u;
    w.AddComponentImmediate(e, relief);

    const TerrainPlanetRelief resolved = ResolveActivePlanetRelief(w);
    EXPECT_FLOAT_EQ(resolved.Amplitude, 321.0f);
    EXPECT_FLOAT_EQ(resolved.Frequency, 9.0f);
    EXPECT_EQ(resolved.Octaves, 5u);
}

// A spherical terrain without the component (or none live) resolves to the default —
// so a planet missing its relief renders with the old default amplitude, not garbage.
TEST(ResolveActivePlanetRelief, DefaultsWhenComponentAbsent)
{
    World w;
    AddTerrain(w, /*enabled*/ true, TerrainDomain::Spherical, /*single*/ 1u, /*tiled*/ 0u);
    const TerrainPlanetRelief resolved = ResolveActivePlanetRelief(w);
    EXPECT_FLOAT_EQ(resolved.Amplitude, TerrainPlanetRelief{}.Amplitude);
    EXPECT_FLOAT_EQ(resolved.Frequency, TerrainPlanetRelief{}.Frequency);
    EXPECT_EQ(resolved.Octaves, TerrainPlanetRelief{}.Octaves);
}

TEST(ResolveActivePlanetRelief, EmptyWorldReturnsDefault)
{
    World w;
    const TerrainPlanetRelief resolved = ResolveActivePlanetRelief(w);
    EXPECT_FLOAT_EQ(resolved.Amplitude, TerrainPlanetRelief{}.Amplitude);
}

// The relief resolves off the SAME entity FindActiveTerrain picks: a disabled terrain's
// relief must not leak to the active one.
TEST(ResolveActivePlanetRelief, ResolvesReliefOfTheActiveTerrainOnly)
{
    World w;
    // A disabled terrain carrying a distinctive relief must be skipped.
    const auto disabled = AddTerrain(w, /*enabled*/ false, TerrainDomain::Spherical, /*single*/ 5u, /*tiled*/ 0u);
    TerrainPlanetRelief disabledRelief{};
    disabledRelief.Amplitude = 999.0f;
    w.AddComponentImmediate(disabled, disabledRelief);

    const auto active = AddTerrain(w, /*enabled*/ true, TerrainDomain::Spherical, /*single*/ 1u, /*tiled*/ 0u);
    TerrainPlanetRelief activeRelief{};
    activeRelief.Amplitude = 42.0f;
    w.AddComponentImmediate(active, activeRelief);

    EXPECT_FLOAT_EQ(ResolveActivePlanetRelief(w).Amplitude, 42.0f);
}

// ---- CBT debug view (ledger cleanup): the toggle plumbs through the surface params without
// disturbing normal rendering (default off), and the enum <-> surface-param contract is
// lockstep with cbt_surface.glsl. ----

TEST(TerrainDebugView, DefaultsOffAndValuesAreLockstep)
{
    // Default is Off -> the surface debug branch is skipped -> planar/planet render unchanged.
    EXPECT_EQ(static_cast<uint32_t>(Terrain{}.DebugView), 0u);
    EXPECT_EQ(static_cast<uint32_t>(TerrainDebugView::Off), 0u);
    // Facets == 1 mirrors CBT_DEBUG_FACETS in cbt_surface.glsl and the DebugMode the feature
    // uploads (CBTUpdateSystem casts DebugView -> uint32 -> CBTSurfaceParams.DebugMode).
    EXPECT_EQ(static_cast<uint32_t>(TerrainDebugView::Facets), 1u);
    // AtlasSlots == 2 mirrors CBT_DEBUG_ATLAS_SLOTS. TerrainInspector lists one option per value
    // and resolves the dropdown's selected index by matching the option value, so the list order
    // never has to track the enum numbering.
    EXPECT_EQ(static_cast<uint32_t>(TerrainDebugView::AtlasSlots), 2u);
}

TEST(TerrainDebugView, SurfaceParamsLayoutLockstep)
{
    // The std430 mirror is 864 bytes — 88 (22 scalars) plus the appended analytic sphere-modifier
    // tail (count + pad + 16 x 3 vec4) — enforced by the static_assert in CBTLayout.h and pinned
    // field-by-field in CBTLayoutTests. Lockstep with cbt_surface.glsl CBTSurfaceParamsData. Every
    // new field defaults to 0, so a non-atlas / non-debug / analytic-free terrain takes none of
    // those branches — golden-image safe. Materials are not in here: both terrain domains read the
    // terrain material table (TerrainRenderFeature).
    EXPECT_EQ(sizeof(CBTSurfaceParams), 864u);
    EXPECT_EQ(CBTSurfaceParams{}.SphereAnalyticCount, 0u);
    EXPECT_EQ(CBTSurfaceParams{}.DebugMode, 0u);
    EXPECT_EQ(CBTSurfaceParams{}.AtlasBacked, 0u);
    EXPECT_EQ(CBTSurfaceParams{}.AtlasDim, 0u);
    EXPECT_EQ(CBTSurfaceParams{}.AtlasSplatBindless, 0u);
    EXPECT_EQ(CBTSurfaceParams{}.AtlasNormalCoarseBindless, 0u);
}

// ---- TerrainSchema live-edit handle contract (the IPC set_component path) ----
//
// The debug-server's set_component applies Terrain fields through
// TerrainSchema::ApplyProperty (ApplyComponentViaSchema — Terrain has no reflected
// field table). The inspector's commit path preserves the component's runtime
// handles, so a config edit walks the extraction system's IN-PLACE debounced
// re-provision (release old resources, then recreate). ApplyProperty used to zero
// the handles unconditionally ("clear runtime state on load"), which on a LIVE
// provisioned terrain orphaned the old TerrainData / TiledTerrainData slot — its
// GPU resource sets were never released (leak), and the recreate walked a path the
// user's inspector workflow never exercises (IPC repros diverged from reality).
// These oracles pin the unified contract effect-side, against a real
// TerrainService: a handle that still resolves survives repeated applies with a
// stable service population; a handle that does not resolve is zeroed exactly as
// before (fresh scene load / service reset), so the extraction system's
// stale-handle guard can never be stranded.

namespace
{

struct ScopedTerrainService
{
    ScopedTerrainService()
    {
        if (GameEngine::TerrainECS::TerrainService::IsInitialized())
            GameEngine::TerrainECS::TerrainService::Shutdown();
        GameEngine::TerrainECS::TerrainService::Initialize();
    }
    ~ScopedTerrainService()
    {
        if (GameEngine::TerrainECS::TerrainService::IsInitialized())
            GameEngine::TerrainECS::TerrainService::Shutdown();
    }
};

const GameEngine::Scene::ISceneComponentSchema* FindTerrainSchema()
{
    GameEngine::Scene::EnsureTerrainSceneSchemasRegistered();
    return GameEngine::Scene::SceneSchemaRegistry::Find("Terrain");
}

// Applies one property through the exact call the debug-server set_component
// handler makes (ApplyComponentViaSchema -> schema->ApplyProperty).
void ApplyTerrainProperty(GameEngine::ECS::World& world, GameEngine::ECS::EntityHandle entity,
                          std::string_view property, std::string_view value)
{
    const auto* schema = FindTerrainSchema();
    ASSERT_NE(schema, nullptr);
    GameEngine::Scene::SceneLoadContext loadCtx{};
    std::string err;
    ASSERT_TRUE(schema->ApplyProperty(world, entity, loadCtx, property, value, &err)) << err;
}

GameEngine::TerrainECS::TerrainHandle CreateProvisionedTerrain(
    GameEngine::TerrainECS::TerrainService& svc)
{
    GameEngine::Terrain::TerrainConfig cfg{};
    cfg.HeightmapWidth = 129;
    cfg.HeightmapHeight = 129;
    cfg.WorldSizeX = 256.0f;
    cfg.WorldSizeZ = 256.0f;
    cfg.HeightScale = 64.0f;
    cfg.LODLevels = 4;
    return svc.CreateTerrain(cfg);
}

} // namespace

TEST(TerrainSchemaLiveEdit, LiveSingleHandleSurvivesRepeatedApplies)
{
    ScopedTerrainService scoped;
    auto& svc = GameEngine::TerrainECS::TerrainService::Get();

    const auto handle = CreateProvisionedTerrain(svc);
    ASSERT_NE(svc.GetTerrainData(handle), nullptr);

    World w;
    const auto e = w.CreateEntity();
    Terrain t{};
    t.SizeX = 256.0f;
    t.SizeZ = 256.0f;
    t.TerrainDataHandle = handle.Index;
    t.TerrainDataGeneration = handle.Generation;
    w.AddComponentImmediate(e, t);

    // Two IPC-style edits of a provisioning-relevant field on the LIVE terrain.
    ApplyTerrainProperty(w, e, "samplespermeter", "3");
    ApplyTerrainProperty(w, e, "samplespermeter", "2.5");

    const auto* c = w.GetComponent<Terrain>(e);
    ASSERT_NE(c, nullptr);
    EXPECT_FLOAT_EQ(c->SamplesPerMeter, 2.5f); // the edits applied

    // The runtime handle survives — the extraction system re-provisions IN PLACE
    // (same as an inspector commit) instead of recreating against a zeroed handle.
    EXPECT_EQ(c->TerrainDataHandle, handle.Index);
    EXPECT_EQ(c->TerrainDataGeneration, handle.Generation);

    // No orphan: the service still holds exactly the one terrain, and the
    // component still references it (a zeroed handle here = leaked slot, since
    // nothing else would ever release it or its GPU resource sets).
    EXPECT_EQ(svc.GetActiveTerrainCount(), 1u);
    EXPECT_NE(svc.GetTerrainData(handle), nullptr);
}

TEST(TerrainSchemaLiveEdit, LiveTiledHandleSurvivesRepeatedApplies)
{
    ScopedTerrainService scoped;
    auto& svc = GameEngine::TerrainECS::TerrainService::Get();

    GameEngine::TerrainECS::TiledTerrainConfig cfg{};
    cfg.WorldSizeX = 4096.0f;
    cfg.WorldSizeZ = 4096.0f;
    cfg.HeightScale = 256.0f;
    cfg.SamplesPerMeter = 2.0f;
    const auto tiled = svc.CreateTiledTerrain(cfg);
    ASSERT_NE(svc.GetTiledTerrainData(tiled), nullptr);

    World w;
    const auto e = w.CreateEntity();
    Terrain t{};
    t.SizeX = 4096.0f;
    t.SizeZ = 4096.0f;
    t.SamplesPerMeter = 2.0f;
    t.TiledTerrainHandle = tiled.Index;
    t.TiledTerrainGeneration = tiled.Generation;
    w.AddComponentImmediate(e, t);

    ApplyTerrainProperty(w, e, "samplespermeter", "1");
    ApplyTerrainProperty(w, e, "heightscale", "128");

    const auto* c = w.GetComponent<Terrain>(e);
    ASSERT_NE(c, nullptr);
    EXPECT_FLOAT_EQ(c->SamplesPerMeter, 1.0f);
    EXPECT_FLOAT_EQ(c->HeightScale, 128.0f);

    // The tiled handle survives, so the extraction system's settle-debounced
    // re-provision (which releases the old unified/atlas set BEFORE recreating)
    // is the path taken — not an orphaning recreate.
    EXPECT_EQ(c->TiledTerrainHandle, tiled.Index);
    EXPECT_EQ(c->TiledTerrainGeneration, tiled.Generation);
    EXPECT_NE(svc.GetTiledTerrainData(tiled), nullptr);
}

// Load semantics are unchanged: a handle that does NOT resolve (destroyed data,
// reset service — what a fresh scene load into a new world looks like) is
// zeroed, exactly as the unconditional clear did. Preserving a dead handle
// would strand the terrain on the extraction system's stale-handle guard.
TEST(TerrainSchemaLiveEdit, StaleHandleIsZeroed)
{
    ScopedTerrainService scoped;
    auto& svc = GameEngine::TerrainECS::TerrainService::Get();

    const auto handle = CreateProvisionedTerrain(svc);
    svc.DestroyTerrain(handle); // the component's handle is now stale

    World w;
    const auto e = w.CreateEntity();
    Terrain t{};
    t.TerrainDataHandle = handle.Index;
    t.TerrainDataGeneration = handle.Generation;
    t.TiledTerrainHandle = 7u; // never existed
    t.TiledTerrainGeneration = 3u;
    w.AddComponentImmediate(e, t);

    ApplyTerrainProperty(w, e, "heightscale", "32");

    const auto* c = w.GetComponent<Terrain>(e);
    ASSERT_NE(c, nullptr);
    EXPECT_FLOAT_EQ(c->HeightScale, 32.0f);
    EXPECT_EQ(c->TerrainDataHandle, 0u);
    EXPECT_EQ(c->TerrainDataGeneration, 0u);
    EXPECT_EQ(c->TiledTerrainHandle, 0u);
    EXPECT_EQ(c->TiledTerrainGeneration, 0u);
}

TEST(TerrainSchemaLiveEdit, HandlesZeroWithoutService)
{
    if (GameEngine::TerrainECS::TerrainService::IsInitialized())
        GameEngine::TerrainECS::TerrainService::Shutdown();

    World w;
    const auto e = w.CreateEntity();
    Terrain t{};
    t.TerrainDataHandle = 2u;
    t.TerrainDataGeneration = 5u;
    w.AddComponentImmediate(e, t);

    ApplyTerrainProperty(w, e, "sizex", "512");

    const auto* c = w.GetComponent<Terrain>(e);
    ASSERT_NE(c, nullptr);
    EXPECT_FLOAT_EQ(c->SizeX, 512.0f);
    EXPECT_EQ(c->TerrainDataHandle, 0u);
    EXPECT_EQ(c->TerrainDataGeneration, 0u);
}

// ---------------------------------------------------------------------------
// Pre-volume modifier -> modifier volume + effect, at load
//
// The scene loader hands a whole component block to ApplyProperties, so the
// migration sees every authored field at once. These oracles drive that exact
// call and assert the resulting components — the field mapping the bake-side
// byte-identity oracles (TerrainModifierVolumeTests) assume.
// ---------------------------------------------------------------------------

namespace
{
using GameEngine::Components::TerrainFlattenEffect;
using GameEngine::Components::TerrainHeightOffsetEffect;
using GameEngine::Components::TerrainModifierBlend;
using GameEngine::Components::TerrainModifierVolume;
using GameEngine::Components::TerrainNoiseEffect;
using GameEngine::Components::TerrainPaintLayerEffect;
using GameEngine::Components::TerrainVolumeShape;

// Load one authored component block exactly as SceneIO does: property keys
// lower-cased (SceneIO.cpp:884), then ONE grouped ApplyProperties call rather
// than a sequence of single-property applies.
// The load result rather than an assertion, so a test can pin what a REJECTED block
// reports. Returns false with *outError set exactly as SceneIO would surface it.
bool TryLoadComponentBlock(World& world, GameEngine::ECS::EntityHandle entity,
                           std::string_view componentName,
                           const std::vector<std::pair<std::string_view, std::string_view>>& props,
                           std::string* outError)
{
    GameEngine::Scene::EnsureTerrainSceneSchemasRegistered();
    const auto* schema = GameEngine::Scene::SceneSchemaRegistry::Find(componentName);
    if (!schema)
    {
        if (outError)
            *outError = std::string("no schema registered for ") + std::string(componentName);
        return false;
    }

    std::vector<std::string> keys;
    keys.reserve(props.size());
    for (const auto& kv : props)
    {
        std::string k(kv.first);
        for (char& ch : k)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        keys.push_back(std::move(k));
    }
    std::vector<std::pair<std::string_view, std::string_view>> lowered;
    lowered.reserve(props.size());
    for (std::size_t i = 0; i < props.size(); ++i)
        lowered.emplace_back(keys[i], props[i].second);

    GameEngine::Scene::SceneLoadContext ctx{};
    std::size_t failed = 0;
    return schema->ApplyProperties(world, entity, ctx, lowered, outError, &failed);
}

void LoadComponentBlock(World& world, GameEngine::ECS::EntityHandle entity,
                        std::string_view componentName,
                        const std::vector<std::pair<std::string_view, std::string_view>>& props)
{
    std::string err;
    ASSERT_TRUE(TryLoadComponentBlock(world, entity, componentName, props, &err)) << err;
}

// The property pairs a schema's OWN Serialize emits for `entity`, keyed the way
// SceneIO keys them (lower-cased). Spelling a block out this way keeps a test's
// "authored at its defaults" arm from drifting when a default value changes. A
// line that does not parse becomes a property no schema accepts, so the load
// that consumes these pairs fails loudly instead of thinning the comparison.
std::vector<std::pair<std::string, std::string>> SerializedProps(
    const GameEngine::Scene::ISceneComponentSchema& schema, const World& world,
    GameEngine::ECS::EntityHandle entity)
{
    std::vector<std::string> lines;
    GameEngine::Scene::SceneSaveContext saveCtx{};
    schema.Serialize(world, entity, saveCtx, lines);

    std::vector<std::pair<std::string, std::string>> props;
    props.reserve(lines.size());
    for (const std::string& line : lines)
    {
        const std::size_t dot = line.find('.');
        const std::size_t eq = line.find(" = ");
        if (dot == std::string::npos || eq == std::string::npos || eq < dot)
        {
            props.emplace_back("unparsed-serialized-line", line);
            continue;
        }
        std::string prop = line.substr(dot + 1, eq - dot - 1);
        for (char& ch : prop)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        props.emplace_back(std::move(prop), line.substr(eq + 3));
    }
    return props;
}
} // namespace

TEST(TerrainModifierMigration, FlattenModifierBlockLoadsAsVolumePlusEffect)
{
    World w;
    const auto e = w.CreateEntity();
    // targetHeight deliberately precedes useEntityHeight: a per-property
    // migration could not know the flag was coming.
    LoadComponentBlock(w, e, "TerrainFlattenModifier",
                       {{"shape", "1"}, {"radius", "30"}, {"rectHalfX", "12"}, {"rectHalfZ", "18"},
                        {"falloff", "6"}, {"priority", "3"}, {"targetHeight", "17"},
                        {"useEntityHeight", "false"}});

    EXPECT_FALSE(GameEngine::Scene::SceneSchemaRegistry::Find("TerrainFlattenModifier")->IsPresent(w, e))
        << "the pre-volume component must not survive the load";

    const auto* vol = w.GetComponent<TerrainModifierVolume>(e);
    ASSERT_NE(vol, nullptr);
    EXPECT_EQ(vol->Shape, TerrainVolumeShape::Rectangle);
    EXPECT_FLOAT_EQ(vol->Radius, 30.0f);
    EXPECT_FLOAT_EQ(vol->RectHalfX, 12.0f);
    EXPECT_FLOAT_EQ(vol->RectHalfZ, 18.0f);
    EXPECT_FLOAT_EQ(vol->Falloff, 6.0f);
    EXPECT_FLOAT_EQ(vol->Priority, 3.0f);
    EXPECT_FLOAT_EQ(vol->FalloffInward, 0.0f);
    EXPECT_FLOAT_EQ(vol->Weight, 1.0f);

    const auto* fx = w.GetComponent<TerrainFlattenEffect>(e);
    ASSERT_NE(fx, nullptr);
    EXPECT_FALSE(fx->UseVolumeHeight);
    EXPECT_FLOAT_EQ(fx->TargetHeight, 17.0f);
    EXPECT_TRUE(fx->Enabled);
}

// useEntityHeight made the pre-volume component IGNORE targetHeight, so the
// migrated effect must carry a zero offset from the volume reference — not the
// stale authored value, which would move the terrain.
TEST(TerrainModifierMigration, FlattenUseEntityHeightDropsTheIgnoredTarget)
{
    World w;
    const auto e = w.CreateEntity();
    LoadComponentBlock(w, e, "TerrainFlattenModifier",
                       {{"targetHeight", "17"}, {"useEntityHeight", "true"}});

    const auto* fx = w.GetComponent<TerrainFlattenEffect>(e);
    ASSERT_NE(fx, nullptr);
    EXPECT_TRUE(fx->UseVolumeHeight);
    EXPECT_FLOAT_EQ(fx->TargetHeight, 0.0f);
}

TEST(TerrainModifierMigration, NoiseModifierBlockLoadsAsVolumePlusEffect)
{
    World w;
    const auto e = w.CreateEntity();
    LoadComponentBlock(w, e, "TerrainNoiseModifier",
                       {{"shape", "0"}, {"radius", "25"}, {"falloff", "8"}, {"blend", "2"},
                        {"frequency", "6.5"}, {"amplitude", "12"}, {"octaves", "5"},
                        {"seed", "77"}, {"lacunarity", "2.5"}, {"persistence", "0.4"}});

    EXPECT_FALSE(GameEngine::Scene::SceneSchemaRegistry::Find("TerrainNoiseModifier")->IsPresent(w, e));
    const auto* vol = w.GetComponent<TerrainModifierVolume>(e);
    ASSERT_NE(vol, nullptr);
    EXPECT_EQ(vol->Shape, TerrainVolumeShape::Circle);

    const auto* fx = w.GetComponent<TerrainNoiseEffect>(e);
    ASSERT_NE(fx, nullptr);
    EXPECT_EQ(fx->Blend, TerrainModifierBlend::Subtract);
    EXPECT_FLOAT_EQ(fx->Frequency, 6.5f);
    EXPECT_FLOAT_EQ(fx->Amplitude, 12.0f);
    EXPECT_EQ(fx->Octaves, 5u);
    EXPECT_EQ(fx->Seed, 77u);
    EXPECT_FLOAT_EQ(fx->Lacunarity, 2.5f);
    EXPECT_FLOAT_EQ(fx->Persistence, 0.4f);
}

// Shape::Spline filled a closed loop interior, so the faithful migration is
// SplineArea. SplinePath would silently hollow out every migrated closed region.
TEST(TerrainModifierMigration, SplineShapeMigratesToSplineArea)
{
    World w;
    const auto e = w.CreateEntity();
    LoadComponentBlock(w, e, "TerrainNoiseModifier", {{"shape", "2"}, {"falloff", "5"}});

    const auto* vol = w.GetComponent<TerrainModifierVolume>(e);
    ASSERT_NE(vol, nullptr);
    EXPECT_EQ(vol->Shape, TerrainVolumeShape::SplineArea);
}

TEST(TerrainModifierMigration, SplineModifierFlattenModeLoadsAsFlattenEffect)
{
    World w;
    const auto e = w.CreateEntity();
    LoadComponentBlock(w, e, "TerrainSplineModifier",
                       {{"falloff", "5"}, {"priority", "2"}, {"heightOffset", "-4"},
                        {"flatten", "true"}, {"paintLayer", "true"},
                        {"paintLayerIndex", "3"}, {"paintStrength", "0.7"}});

    EXPECT_FALSE(GameEngine::Scene::SceneSchemaRegistry::Find("TerrainSplineModifier")->IsPresent(w, e));
    const auto* vol = w.GetComponent<TerrainModifierVolume>(e);
    ASSERT_NE(vol, nullptr);
    EXPECT_EQ(vol->Shape, TerrainVolumeShape::SplineArea);
    EXPECT_FLOAT_EQ(vol->Falloff, 5.0f);
    EXPECT_FLOAT_EQ(vol->Priority, 2.0f);

    const auto* flatten = w.GetComponent<TerrainFlattenEffect>(e);
    ASSERT_NE(flatten, nullptr);
    EXPECT_TRUE(flatten->UseVolumeHeight);
    EXPECT_FLOAT_EQ(flatten->TargetHeight, -4.0f);
    EXPECT_EQ(w.GetComponent<TerrainHeightOffsetEffect>(e), nullptr);

    const auto* paint = w.GetComponent<TerrainPaintLayerEffect>(e);
    ASSERT_NE(paint, nullptr);
    EXPECT_EQ(paint->LayerIndex, 3u);
    EXPECT_FLOAT_EQ(paint->Strength, 0.7f);
    EXPECT_TRUE(paint->Replace) << "spline painting replaced the layer, so the effect must too";
    // The paint effect runs after the height effect it was authored alongside.
    EXPECT_GT(paint->StackOrder, flatten->StackOrder);
}

TEST(TerrainModifierMigration, SplineModifierOffsetModeLoadsAsHeightOffsetEffect)
{
    World w;
    const auto e = w.CreateEntity();
    // Offset mode always accumulated, whatever Blend said — blend 0 (Set) here
    // must NOT become the effect blend.
    LoadComponentBlock(w, e, "TerrainSplineModifier",
                       {{"blend", "0"}, {"falloff", "5"}, {"heightOffset", "9"}, {"flatten", "false"}});

    EXPECT_EQ(w.GetComponent<TerrainFlattenEffect>(e), nullptr);
    const auto* fx = w.GetComponent<TerrainHeightOffsetEffect>(e);
    ASSERT_NE(fx, nullptr);
    EXPECT_FLOAT_EQ(fx->Offset, 9.0f);
    EXPECT_EQ(fx->Blend, TerrainModifierBlend::Add);
}

// A disabled pre-volume modifier migrates to a disabled effect, which the gather
// drops — the volume then has nothing to do.
TEST(TerrainModifierMigration, DisabledModifierMigratesToDisabledEffect)
{
    World w;
    const auto e = w.CreateEntity();
    LoadComponentBlock(w, e, "TerrainNoiseModifier", {{"enabled", "false"}, {"amplitude", "5"}});

    const auto* fx = w.GetComponent<TerrainNoiseEffect>(e);
    ASSERT_NE(fx, nullptr);
    EXPECT_FALSE(fx->Enabled);
}

// Two pre-volume modifiers on ONE entity migrate onto one volume as two stacked
// effects, in the order the blocks were loaded.
TEST(TerrainModifierMigration, TwoModifiersOnOneEntityStackInLoadOrder)
{
    World w;
    const auto e = w.CreateEntity();
    LoadComponentBlock(w, e, "TerrainFlattenModifier",
                       {{"radius", "30"}, {"falloff", "6"}, {"targetHeight", "5"},
                        {"useEntityHeight", "false"}});
    LoadComponentBlock(w, e, "TerrainNoiseModifier",
                       {{"radius", "30"}, {"falloff", "6"}, {"amplitude", "4"}});

    const auto* flatten = w.GetComponent<TerrainFlattenEffect>(e);
    const auto* noise = w.GetComponent<TerrainNoiseEffect>(e);
    ASSERT_NE(flatten, nullptr);
    ASSERT_NE(noise, nullptr);
    EXPECT_LT(flatten->StackOrder, noise->StackOrder);
    // One region for the entity, not two.
    const auto* vol = w.GetComponent<TerrainModifierVolume>(e);
    ASSERT_NE(vol, nullptr);
    EXPECT_FLOAT_EQ(vol->Radius, 30.0f);
}

// SceneIO reads a component block with NO properties as "add defaults"
// (schema->AddDefault) and one with properties through ApplyProperties. Both
// spell the same authored concept, so they have to land the same region — with
// two independent constructions they were free to disagree, and did: a bare
// block produced SplinePath while an authored one produced SplineArea, so the
// same closed spline meant a swept band in one scene and a filled area in the
// next. AddDefault, the IPC's create_component and blueprint overrides all
// arrive here.
TEST(TerrainModifierMigration, BareSplineModifierBlockLoadsTheSameRegionAsADefaultOne)
{
    GameEngine::Scene::EnsureTerrainSceneSchemasRegistered();
    const auto* schema = GameEngine::Scene::SceneSchemaRegistry::Find("TerrainSplineModifier");
    ASSERT_NE(schema, nullptr);

    // Arm A — the block spelled out at exactly the pre-volume spline block's
    // declared defaults (TerrainSceneSchemas' LegacyModifierBlocks).
    // Keys are lower-cased, as SceneIO hands them to a schema.
    const std::vector<std::pair<std::string_view, std::string_view>> pairs = {
        {"blend", "1"},  // TerrainModifierBlend::Add
        {"falloff", "5"}, {"priority", "0"}, {"heightoffset", "0"},
        {"flatten", "true"}, {"enabled", "true"},
        {"paintlayer", "false"}, {"paintlayerindex", "2"}, {"paintstrength", "0.8"}};

    World spelled;
    const auto a = spelled.CreateEntity();
    GameEngine::Scene::SceneLoadContext loadCtx{};
    std::string err;
    std::size_t failed = 0;
    ASSERT_TRUE(schema->ApplyProperties(spelled, a, loadCtx, pairs, &err, &failed)) << err;

    // Arm B — the same block with no properties at all.
    World bare;
    const auto b = bare.CreateEntity();
    std::string addErr;
    ASSERT_TRUE(schema->AddDefault(bare, b, &addErr)) << addErr;

    const auto* spelledVol = spelled.GetComponent<TerrainModifierVolume>(a);
    const auto* bareVol = bare.GetComponent<TerrainModifierVolume>(b);
    ASSERT_NE(spelledVol, nullptr);
    ASSERT_NE(bareVol, nullptr) << "a bare block must still provision the region";

    EXPECT_EQ(bareVol->Shape, spelledVol->Shape)
        << "a bare TerrainSplineModifier block and a default authored one disagree "
           "about the region's shape";
    EXPECT_FLOAT_EQ(bareVol->Falloff, spelledVol->Falloff);
    EXPECT_FLOAT_EQ(bareVol->Priority, spelledVol->Priority);

    // Which shape, on its own terms — agreeing on SplinePath would be agreeing on
    // the wrong answer. TerrainVolumeShape::SplineArea is what Shape::Spline has
    // always baked as, pinned against a real bake by
    // TerrainVolumeMigration.ClosedSplineShapeMigratesToSplineAreaNotPath.
    EXPECT_EQ(bareVol->Shape, TerrainVolumeShape::SplineArea);

    // The effect half lands as well, and identically: a default modifier flattens
    // to the spline's own height.
    const auto* spelledFx = spelled.GetComponent<TerrainFlattenEffect>(a);
    const auto* bareFx = bare.GetComponent<TerrainFlattenEffect>(b);
    ASSERT_NE(spelledFx, nullptr);
    ASSERT_NE(bareFx, nullptr);
    EXPECT_EQ(bareFx->UseVolumeHeight, spelledFx->UseVolumeHeight);
    EXPECT_FLOAT_EQ(bareFx->TargetHeight, spelledFx->TargetHeight);
    EXPECT_EQ(bareFx->Enabled, spelledFx->Enabled);
    EXPECT_EQ(bareFx->StackOrder, spelledFx->StackOrder);
}

// The volume model round-trips through its own schemas: save what load produced,
// reload it, get the same components.
TEST(TerrainModifierMigration, VolumeAndEffectsRoundTripThroughTheirOwnSchemas)
{
    GameEngine::Scene::EnsureTerrainSceneSchemasRegistered();

    World src;
    const auto a = src.CreateEntity();
    TerrainModifierVolume vol{};
    vol.Shape = TerrainVolumeShape::SplinePath;
    vol.Radius = 21.0f;
    vol.RectHalfX = 4.0f;
    vol.RectHalfZ = 5.0f;
    vol.Falloff = 3.5f;
    vol.FalloffInward = 2.25f;
    vol.Weight = 0.75f;
    vol.Priority = -2.0f;
    src.AddComponentImmediate(a, vol);
    TerrainNoiseEffect noise{};
    noise.StackOrder = 4;
    noise.Amplitude = 13.5f;
    noise.Seed = 909u;
    src.AddComponentImmediate(a, noise);

    // Serialize both blocks, then feed the lines back through ApplyProperty.
    World dst;
    const auto b = dst.CreateEntity();
    for (const char* name : {"TerrainModifierVolume", "TerrainNoiseEffect"})
    {
        const auto* schema = GameEngine::Scene::SceneSchemaRegistry::Find(name);
        ASSERT_NE(schema, nullptr) << name;
        std::vector<std::string> lines;
        GameEngine::Scene::SceneSaveContext saveCtx{};
        schema->Serialize(src, a, saveCtx, lines);
        ASSERT_FALSE(lines.empty()) << name;

        GameEngine::Scene::SceneLoadContext loadCtx{};
        for (const std::string& line : lines)
        {
            const std::size_t dot = line.find('.');
            const std::size_t eq = line.find(" = ");
            ASSERT_NE(dot, std::string::npos);
            ASSERT_NE(eq, std::string::npos);
            std::string prop = line.substr(dot + 1, eq - dot - 1);
            for (char& ch : prop)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            const std::string value = line.substr(eq + 3);
            std::string err;
            ASSERT_TRUE(schema->ApplyProperty(dst, b, loadCtx, prop, value, &err)) << line << ": " << err;
        }
    }

    const auto* rtVol = dst.GetComponent<TerrainModifierVolume>(b);
    ASSERT_NE(rtVol, nullptr);
    EXPECT_EQ(rtVol->Shape, TerrainVolumeShape::SplinePath);
    EXPECT_FLOAT_EQ(rtVol->Radius, 21.0f);
    EXPECT_FLOAT_EQ(rtVol->RectHalfX, 4.0f);
    EXPECT_FLOAT_EQ(rtVol->RectHalfZ, 5.0f);
    EXPECT_FLOAT_EQ(rtVol->Falloff, 3.5f);
    EXPECT_FLOAT_EQ(rtVol->FalloffInward, 2.25f);
    EXPECT_FLOAT_EQ(rtVol->Weight, 0.75f);
    EXPECT_FLOAT_EQ(rtVol->Priority, -2.0f);

    const auto* rtNoise = dst.GetComponent<TerrainNoiseEffect>(b);
    ASSERT_NE(rtNoise, nullptr);
    EXPECT_EQ(rtNoise->StackOrder, 4);
    EXPECT_FLOAT_EQ(rtNoise->Amplitude, 13.5f);
    EXPECT_EQ(rtNoise->Seed, 909u);
}

// ---------------------------------------------------------------------------
// Bare block vs authored block — the two paths must agree on the region
// ---------------------------------------------------------------------------
//
// SceneIO treats a component block with no properties as "add defaults" and
// calls AddDefault; a block WITH properties goes through ApplyProperties. Both
// have to produce the same volume for a default-valued component, and the only
// thing that guarantees it is that both derive the region from the pre-volume
// component's own defaults. A hand-written TerrainModifierVolume{} in AddDefault
// agrees only while two independent sets of default literals happen to match,
// and nothing fails when they stop.

namespace
{
// Drive the schema's no-properties path exactly as SceneIO does.
void AddDefaultBlock(World& world, GameEngine::ECS::EntityHandle entity,
                     std::string_view componentName)
{
    GameEngine::Scene::EnsureTerrainSceneSchemasRegistered();
    const auto* schema = GameEngine::Scene::SceneSchemaRegistry::Find(componentName);
    ASSERT_NE(schema, nullptr) << componentName;
    std::string err;
    ASSERT_TRUE(schema->AddDefault(world, entity, &err)) << err;
}

// The region half of a volume — what MigrateRegion writes and AddDefault must
// reproduce. Weight/FalloffInward are excluded: they belong to the volume alone,
// and no pre-volume component can express them.
void ExpectSameRegion(const TerrainModifierVolume& bare, const TerrainModifierVolume& authored)
{
    EXPECT_EQ(bare.Shape, authored.Shape);
    EXPECT_FLOAT_EQ(bare.Radius, authored.Radius);
    EXPECT_FLOAT_EQ(bare.RectHalfX, authored.RectHalfX);
    EXPECT_FLOAT_EQ(bare.RectHalfZ, authored.RectHalfZ);
    EXPECT_FLOAT_EQ(bare.Falloff, authored.Falloff);
    EXPECT_FLOAT_EQ(bare.Priority, authored.Priority);
}

// One schema's two arms: a bare block, and a block spelling out the same
// component's declared defaults, so the authored arm is default-valued too.
void ExpectBareMatchesAuthored(
    std::string_view componentName,
    const std::vector<std::pair<std::string_view, std::string_view>>& defaults)
{
    World bareWorld;
    const auto bareEntity = bareWorld.CreateEntity();
    AddDefaultBlock(bareWorld, bareEntity, componentName);
    const auto* bare = bareWorld.GetComponent<TerrainModifierVolume>(bareEntity);
    ASSERT_NE(bare, nullptr) << componentName << " (bare)";

    World authoredWorld;
    const auto authoredEntity = authoredWorld.CreateEntity();
    LoadComponentBlock(authoredWorld, authoredEntity, componentName, defaults);
    const auto* authored = authoredWorld.GetComponent<TerrainModifierVolume>(authoredEntity);
    ASSERT_NE(authored, nullptr) << componentName << " (authored)";

    ExpectSameRegion(*bare, *authored);
}

// The pre-volume common shape fields at their declared defaults
// (GE_TERRAIN_MODIFIER_COMMON_FIELDS), spelled as scene properties.
const std::vector<std::pair<std::string_view, std::string_view>>& CommonShapeDefaults()
{
    static const std::vector<std::pair<std::string_view, std::string_view>> props = {
        {"shape", "0"}, {"radius", "50"}, {"rectHalfX", "50"}, {"rectHalfZ", "50"},
        {"falloff", "10"}, {"priority", "0"}};
    return props;
}
} // namespace

TEST(TerrainModifierBareBlock, FlattenBareBlockMatchesAuthoredDefaults)
{
    ExpectBareMatchesAuthored("TerrainFlattenModifier", CommonShapeDefaults());
}

TEST(TerrainModifierBareBlock, NoiseBareBlockMatchesAuthoredDefaults)
{
    ExpectBareMatchesAuthored("TerrainNoiseModifier", CommonShapeDefaults());
}

TEST(TerrainModifierBareBlock, StampBareBlockMatchesAuthoredDefaults)
{
    ExpectBareMatchesAuthored("TerrainStampModifier", CommonShapeDefaults());
}

TEST(TerrainModifierBareBlock, PaintLayerBareBlockMatchesAuthoredDefaults)
{
    ExpectBareMatchesAuthored("TerrainPaintLayerModifier", CommonShapeDefaults());
}

// ---------------------------------------------------------------------------
// Losing a region to a second modifier is reported, never silent
// ---------------------------------------------------------------------------

namespace
{
class CapturingLogSink final : public Logger::LogSink
{
  public:
    explicit CapturingLogSink(std::vector<std::string>* out) : m_Out(out) {}

    void Write(const Logger::LogMessage& message) override
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        m_Out->push_back(message.Message);
    }

    void Flush() override {}
    bool ShouldLog(Logger::LogLevel) const override { return true; }
    Logger::String GetName() const override { return "CapturingLogSink"; }

  private:
    std::vector<std::string>* m_Out;
    std::mutex m_Mutex;
};

// Engine is a SHARED library: the schemas log through Engine.dll's Logger
// state, not this exe's copy. Adopt the engine's state or the sink sees nothing.
//
// RAII: the sink holds a raw pointer to the test's stack vector, and a fatal
// assertion would skip a trailing ClearSinks() — the next engine log line then
// writes through a dangling pointer and the whole suite dies mid-run. The
// destructor removes the sink on every exit path, including ASSERT aborts.
class ScopedEngineLogCapture
{
  public:
    explicit ScopedEngineLogCapture(std::vector<std::string>* out)
    {
        Logger::Log::RedirectToSharedState(GameEngine::GetEngineLoggerState());
        Logger::Log::Initialize({});
        Logger::Log::ClearSinks();
        Logger::Log::AddSink(std::make_unique<CapturingLogSink>(out));
    }
    ~ScopedEngineLogCapture() { Logger::Log::ClearSinks(); }
    ScopedEngineLogCapture(const ScopedEngineLogCapture&) = delete;
    ScopedEngineLogCapture& operator=(const ScopedEngineLogCapture&) = delete;
};

bool AnyLineMentionsSeveralModifiers(const std::vector<std::string>& lines)
{
    return std::any_of(lines.begin(), lines.end(), [](const std::string& l) {
        return l.find("several terrain modifiers") != std::string::npos;
    });
}

bool AnyLineMentionsMigratedModifier(const std::vector<std::string>& lines,
                                     std::string_view blockName)
{
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) {
        return l.find("retired pre-volume terrain modifier") != std::string::npos
            && l.find(std::string(blockName)) != std::string::npos;
    });
}
} // namespace

// A legacy block converts on load and rewrites the entity into a volume plus an
// effect. Saying nothing about that leaves a scene re-converting on every load
// with nobody told that a re-save would settle it, so the conversion reports —
// naming the block, which is what points at the entity to fix.
//
// Each of the five is checked, because the warning sits at five separate
// conversion points and a missed one is silent by construction. A load with no
// scene file always reports (there is no file key to dedupe on), which is what
// makes this deterministic per test.
TEST(TerrainModifierMigration, EveryMigratedPreVolumeModifierReportsItself)
{
    const std::vector<std::pair<std::string, std::vector<std::pair<std::string_view, std::string_view>>>>
        blocks = {
            {"TerrainFlattenModifier", {{"radius", "30"}}},
            {"TerrainNoiseModifier", {{"radius", "30"}}},
            {"TerrainStampModifier", {{"radius", "30"}}},
            {"TerrainPaintLayerModifier", {{"radius", "30"}}},
            {"TerrainSplineModifier", {{"falloff", "5"}}},
        };

    for (const auto& [name, props] : blocks)
    {
        std::vector<std::string> logLines;
        ScopedEngineLogCapture capture(&logLines);

        World w;
        const auto e = w.CreateEntity();
        LoadComponentBlock(w, e, name, props);
        Logger::Log::Flush();

        EXPECT_TRUE(AnyLineMentionsMigratedModifier(logLines, name))
            << name << " migrated without reporting it";
    }
}

// The counterpart: a scene that carries no legacy block must stay quiet, or the
// warning is noise that trains people to ignore it.
TEST(TerrainModifierMigration, AVolumeSceneMigratesNothingAndSaysNothing)
{
    std::vector<std::string> logLines;
    ScopedEngineLogCapture capture(&logLines);

    World w;
    const auto e = w.CreateEntity();
    LoadComponentBlock(w, e, "TerrainModifierVolume", {{"radius", "30"}});
    LoadComponentBlock(w, e, "TerrainNoiseEffect", {{"amplitude", "12"}});
    Logger::Log::Flush();

    EXPECT_FALSE(AnyLineMentionsMigratedModifier(logLines, "Terrain"))
        << "a volume scene reported a migration it did not perform";
}

// A volume owns ONE region per entity, so a second modifier with a different
// region silently costs the first one its shape. Both migration paths must say
// so — the shaped path always did; the spline path used to overwrite in silence.
TEST(TerrainModifierMigration, SplineRegionOverwriteWarnsLikeTheShapedPath)
{
    std::vector<std::string> logLines;
    ScopedEngineLogCapture capture(&logLines);

    World w;
    const auto e = w.CreateEntity();

    // A shaped modifier lands a Circle region first.
    LoadComponentBlock(w, e, "TerrainNoiseModifier", {{"shape", "0"}, {"falloff", "9"}});
    Logger::Log::Flush();
    logLines.clear();

    // The spline modifier then replaces it with SplineArea. Last one wins by
    // design; what must not happen is it going unreported.
    LoadComponentBlock(w, e, "TerrainSplineModifier", {{"falloff", "4"}});
    Logger::Log::Flush();

    const auto* vol = w.GetComponent<TerrainModifierVolume>(e);
    ASSERT_NE(vol, nullptr);
    EXPECT_EQ(vol->Shape, TerrainVolumeShape::SplineArea);
    EXPECT_FLOAT_EQ(vol->Falloff, 4.0f);
    EXPECT_TRUE(AnyLineMentionsSeveralModifiers(logLines))
        << "a spline modifier overwrote an existing region without reporting it";

}

// The same conflict through the shaped path, so the two arms are pinned by one
// oracle and cannot drift apart again.
TEST(TerrainModifierMigration, ShapedRegionOverwriteWarns)
{
    std::vector<std::string> logLines;
    ScopedEngineLogCapture capture(&logLines);

    World w;
    const auto e = w.CreateEntity();
    LoadComponentBlock(w, e, "TerrainNoiseModifier", {{"shape", "0"}, {"radius", "30"}});
    Logger::Log::Flush();
    logLines.clear();

    LoadComponentBlock(w, e, "TerrainFlattenModifier", {{"shape", "1"}, {"radius", "77"}});
    Logger::Log::Flush();

    EXPECT_TRUE(AnyLineMentionsSeveralModifiers(logLines))
        << "a shaped modifier overwrote an existing region without reporting it";

}

// A single modifier on a fresh entity is the ordinary case and must stay quiet —
// without this, both oracles above would pass on a schema that warned always.
TEST(TerrainModifierMigration, SingleModifierMigrationDoesNotWarn)
{
    std::vector<std::string> logLines;
    ScopedEngineLogCapture capture(&logLines);

    World w;
    const auto e = w.CreateEntity();
    LoadComponentBlock(w, e, "TerrainNoiseModifier", {{"shape", "0"}, {"radius", "30"}});
    Logger::Log::Flush();

    EXPECT_FALSE(AnyLineMentionsSeveralModifiers(logLines))
        << "migrating a lone modifier reported a region conflict that does not exist";

}

// ---------------------------------------------------------------------------
// The retired Smooth blend (3) migrates rather than failing the load
// ---------------------------------------------------------------------------

namespace
{
bool AnyLineMentionsRetiredSmooth(const std::vector<std::string>& lines)
{
    return std::any_of(lines.begin(), lines.end(), [](const std::string& l) {
        return l.find("retired Smooth mode") != std::string::npos;
    });
}
} // namespace

// Smooth never had its own dispatch — every blend switch let it fall through to Add — so
// a scene carrying 3 must keep loading, and must land on the value it always baked as.
// Rejecting it would make old scenes unopenable for a mode that never did anything.
TEST(TerrainBlendMigration, LegacySmoothOnModifierLoadsAsAdd)
{
    std::vector<std::string> logLines;
    ScopedEngineLogCapture capture(&logLines);

    World w;
    const auto e = w.CreateEntity();
    LoadComponentBlock(w, e, "TerrainNoiseModifier",
                       {{"shape", "0"}, {"radius", "30"}, {"blend", "3"}, {"amplitude", "12"}});
    Logger::Log::Flush();

    const auto* fx = w.GetComponent<TerrainNoiseEffect>(e);
    ASSERT_NE(fx, nullptr);
    EXPECT_EQ(fx->Blend, TerrainModifierBlend::Add);
    EXPECT_TRUE(AnyLineMentionsRetiredSmooth(logLines))
        << "a legacy blend value was rewritten without reporting it";

}

// The sculpt zone keeps its component (no pre-volume migration), so the stored value is
// observable directly — and it is the other of the two guard shapes in the schema file.
TEST(TerrainBlendMigration, LegacySmoothOnSculptZoneLoadsAsAdd)
{
    World w;
    const auto e = w.CreateEntity();
    LoadComponentBlock(w, e, "TerrainSculptZone", {{"extentX", "16"}, {"blend", "3"}});

    const auto* z = w.GetComponent<GameEngine::Components::TerrainSculptZone>(e);
    ASSERT_NE(z, nullptr);
    EXPECT_FLOAT_EQ(z->ExtentX, 16.0f);
    EXPECT_EQ(z->Blend, TerrainModifierBlend::Add);
}

// Without this, the oracle above would pass on a schema that mapped EVERY blend to Add,
// and the "reported it" arm would pass on one that warned unconditionally.
TEST(TerrainBlendMigration, SupportedBlendValuesLoadUnchangedAndQuietly)
{
    std::vector<std::string> logLines;
    ScopedEngineLogCapture capture(&logLines);

    const std::pair<std::string_view, TerrainModifierBlend> cases[] = {
        {"0", TerrainModifierBlend::Set},
        {"1", TerrainModifierBlend::Add},
        {"2", TerrainModifierBlend::Subtract},
        {"4", TerrainModifierBlend::Min},
        {"5", TerrainModifierBlend::Max},
        {"6", TerrainModifierBlend::SmoothMin},
        {"7", TerrainModifierBlend::SmoothMax},
    };
    for (const auto& [authored, expected] : cases)
    {
        World w;
        const auto e = w.CreateEntity();
        LoadComponentBlock(w, e, "TerrainNoiseEffect", {{"blend", authored}});

        const auto* fx = w.GetComponent<TerrainNoiseEffect>(e);
        ASSERT_NE(fx, nullptr) << authored;
        EXPECT_EQ(fx->Blend, expected) << authored;
    }
    Logger::Log::Flush();

    EXPECT_FALSE(AnyLineMentionsRetiredSmooth(logLines))
        << "a supported blend value was reported as the retired mode";

}

// 3 migrates and 8 (Average) is a live mode, but only where a pool accumulator exists
// (the four HEIGHT effects). 8 on a pool-less component gets the pool refusal naming where
// Average is legal; past 8 is corruption, and the message must name the full range.
TEST(TerrainBlendMigration, BlendAboveTheEnumIsRejectedNamingTheNewRange)
{
    World w;
    const auto e = w.CreateEntity();

    // A pre-volume modifier has no pool of its own — its bake is a per-texel blend
    // switch with a fall-through arm, so an Average reaching it would bake as Add.
    std::string err;
    EXPECT_FALSE(TryLoadComponentBlock(w, e, "TerrainNoiseModifier", {{"blend", "8"}}, &err));
    EXPECT_NE(err.find("only the height effects"), std::string::npos) << err;
    EXPECT_NE(err.find("Use 0-2 or 4-7 here"), std::string::npos) << err;

    err.clear();
    EXPECT_FALSE(TryLoadComponentBlock(w, e, "TerrainSplineModifier", {{"blend", "9"}}, &err));
    EXPECT_NE(err.find("blend must be 0-2, 4-7 or 8"), std::string::npos) << err;
}

// The tombstone, pinned against the value the union operators were assigned around it.
// If a future mode ever takes 3, an old scene's 3 (Smooth, which baked as Add) and a new
// scene's 3 become indistinguishable on disk — this is the test that stops that.
TEST(TerrainBlendMigration, ValueThreeStaysTheSmoothTombstoneAfterTheUnionOperatorsLanded)
{
    const auto tombstone = static_cast<TerrainModifierBlend>(3);
    EXPECT_NE(TerrainModifierBlend::Min, tombstone);
    EXPECT_NE(TerrainModifierBlend::Max, tombstone);
    EXPECT_NE(TerrainModifierBlend::SmoothMin, tombstone);
    EXPECT_NE(TerrainModifierBlend::SmoothMax, tombstone);

    std::vector<std::string> logLines;
    ScopedEngineLogCapture capture(&logLines);

    World w;
    const auto e = w.CreateEntity();
    LoadComponentBlock(w, e, "TerrainFlattenEffect", {{"targetHeight", "12"}, {"blend", "3"}});
    Logger::Log::Flush();

    const auto* fx = w.GetComponent<TerrainFlattenEffect>(e);
    ASSERT_NE(fx, nullptr);
    EXPECT_FLOAT_EQ(fx->TargetHeight, 12.0f);
    EXPECT_EQ(fx->Blend, TerrainModifierBlend::Add)
        << "a scene carrying the retired 3 no longer loads as Add";
    EXPECT_TRUE(AnyLineMentionsRetiredSmooth(logLines))
        << "the legacy blend value was rewritten without reporting it";
}

// Flatten reaching BlendHeight is what makes the operators reach their motivating cases
// (composition design §3.2). Pin that the field round-trips, and that its DEFAULT is
// Set — the semantics flatten hardcoded before it carried a blend at all.
TEST(TerrainBlendMigration, FlattenEffectBlendAndSmoothingRoundTripWithSetAsTheDefault)
{
    EXPECT_EQ(TerrainFlattenEffect{}.Blend, TerrainModifierBlend::Set);

    World w;
    const auto e = w.CreateEntity();
    LoadComponentBlock(w, e, "TerrainFlattenEffect",
                       {{"targetHeight", "8"}, {"blend", "4"}, {"blendSmoothing", "3.5"}});

    const auto* fx = w.GetComponent<TerrainFlattenEffect>(e);
    ASSERT_NE(fx, nullptr);
    EXPECT_EQ(fx->Blend, TerrainModifierBlend::Min);
    EXPECT_FLOAT_EQ(fx->BlendSmoothing, 3.5f);
}

// ---------------------------------------------------------------------------
// Surface rules serialization (F10 S2)
//
// The rule block is the only terrain effect whose scene form is a nested indexed
// key (rule0.condition1.min), so the round-trip is worth pinning field by field:
// a parser that dropped a segment would still load a plausible-looking rule set.
// ---------------------------------------------------------------------------

using GameEngine::Components::kMaxTerrainRuleConditions;
using GameEngine::Components::kMaxTerrainSurfaceRules;
using GameEngine::Components::TerrainRuleConditionKind;
using GameEngine::Components::TerrainRuleFalloffCurve;
using GameEngine::Components::TerrainSurfaceRulesEffect;

namespace
{
const GameEngine::Scene::ISceneComponentSchema* SurfaceRulesSchema()
{
    GameEngine::Scene::EnsureTerrainSceneSchemasRegistered();
    return GameEngine::Scene::SceneSchemaRegistry::Find("TerrainSurfaceRulesEffect");
}

// A rule set whose every field differs from its default, so a round-trip that
// silently substituted a default anywhere fails rather than coincidentally
// matching. Two rows, each with a different condition count and kind mix.
TerrainSurfaceRulesEffect AuthoredRuleSet()
{
    TerrainSurfaceRulesEffect fx{};
    fx.StackOrder = 3;
    fx.RuleCount = 2;

    fx.Rules[0].MaterialSlot = 2;
    fx.Rules[0].Strength = 0.625f;
    fx.Rules[0].Replace = true;
    fx.Rules[0].ConditionCount = 2;
    fx.Rules[0].Conditions[0].Kind = TerrainRuleConditionKind::SlopeDegrees;
    fx.Rules[0].Conditions[0].FalloffCurve = TerrainRuleFalloffCurve::Smoothstep;
    fx.Rules[0].Conditions[0].Min = 34.5f;
    fx.Rules[0].Conditions[0].Max = 72.25f;
    fx.Rules[0].Conditions[0].Feather = 6.5f;
    fx.Rules[0].Conditions[1].Kind = TerrainRuleConditionKind::HeightNormalized;
    fx.Rules[0].Conditions[1].Min = 0.125f;
    fx.Rules[0].Conditions[1].Max = 0.875f;
    fx.Rules[0].Conditions[1].Feather = 0.0625f;

    fx.Rules[1].MaterialSlot = 3;
    fx.Rules[1].Strength = 0.25f;
    fx.Rules[1].Replace = false;
    fx.Rules[1].ConditionCount = 1;
    fx.Rules[1].Conditions[0].Kind = TerrainRuleConditionKind::Noise;
    fx.Rules[1].Conditions[0].Min = 0.5f;
    fx.Rules[1].Conditions[0].Max = 1.0f;
    fx.Rules[1].Conditions[0].Feather = 0.125f;
    fx.Rules[1].Conditions[0].NoiseFrequency = 0.03125f;
    fx.Rules[1].Conditions[0].NoiseSeed = 4242u;
    return fx;
}

// Load a spelled-out property list, returning the schema's own error text.
bool TryLoadRuleProps(World& world, GameEngine::ECS::EntityHandle entity,
                      const std::vector<std::pair<std::string, std::string>>& props,
                      std::string* outError)
{
    const auto* schema = SurfaceRulesSchema();
    if (!schema)
        return false;
    std::vector<std::pair<std::string_view, std::string_view>> pairs;
    pairs.reserve(props.size());
    for (const auto& kv : props)
        pairs.emplace_back(kv.first, kv.second);
    GameEngine::Scene::SceneLoadContext ctx{};
    std::size_t failed = 0;
    return schema->ApplyProperties(world, entity, ctx, pairs, outError, &failed);
}
} // namespace

TEST(TerrainSurfaceRulesSchema, AuthoredRuleSetRoundTripsFieldForField)
{
    const auto* schema = SurfaceRulesSchema();
    ASSERT_NE(schema, nullptr) << "the rules effect has no registered scene schema";

    World src;
    const auto s = src.CreateEntity();
    src.AddComponentImmediate(s, AuthoredRuleSet());

    const auto props = SerializedProps(*schema, src, s);
    ASSERT_FALSE(props.empty());

    World dst;
    const auto d = dst.CreateEntity();
    std::string err;
    ASSERT_TRUE(TryLoadRuleProps(dst, d, props, &err)) << err;

    const auto* loaded = dst.GetComponent<TerrainSurfaceRulesEffect>(d);
    ASSERT_NE(loaded, nullptr);
    const TerrainSurfaceRulesEffect authored = AuthoredRuleSet();

    EXPECT_EQ(loaded->StackOrder, authored.StackOrder);
    EXPECT_EQ(loaded->Enabled, authored.Enabled);
    ASSERT_EQ(loaded->RuleCount, authored.RuleCount);

    for (GameEngine::uint32 r = 0; r < authored.RuleCount; ++r)
    {
        const auto& a = authored.Rules[r];
        const auto& b = loaded->Rules[r];
        EXPECT_EQ(b.MaterialSlot, a.MaterialSlot) << "rule " << r;
        EXPECT_FLOAT_EQ(b.Strength, a.Strength) << "rule " << r;
        EXPECT_EQ(b.Replace, a.Replace) << "rule " << r;
        ASSERT_EQ(b.ConditionCount, a.ConditionCount) << "rule " << r;
        for (GameEngine::uint32 c = 0; c < a.ConditionCount; ++c)
        {
            const auto& ac = a.Conditions[c];
            const auto& bc = b.Conditions[c];
            EXPECT_EQ(bc.Kind, ac.Kind) << "rule " << r << " condition " << c;
            EXPECT_EQ(bc.FalloffCurve, ac.FalloffCurve) << "rule " << r << " condition " << c;
            EXPECT_FLOAT_EQ(bc.Min, ac.Min) << "rule " << r << " condition " << c;
            EXPECT_FLOAT_EQ(bc.Max, ac.Max) << "rule " << r << " condition " << c;
            EXPECT_FLOAT_EQ(bc.Feather, ac.Feather) << "rule " << r << " condition " << c;
            if (ac.Kind == TerrainRuleConditionKind::Noise)
            {
                EXPECT_FLOAT_EQ(bc.NoiseFrequency, ac.NoiseFrequency);
                EXPECT_EQ(bc.NoiseSeed, ac.NoiseSeed);
            }
        }
    }
}

TEST(TerrainSurfaceRulesSchema, ARoundTrippedRuleSetIsBitIdentical)
{
    // Field-by-field above catches a dropped field; this catches a field nobody
    // thought to compare, including the storage tail beyond the live rows.
    const auto* schema = SurfaceRulesSchema();
    ASSERT_NE(schema, nullptr);

    World src;
    const auto s = src.CreateEntity();
    src.AddComponentImmediate(s, AuthoredRuleSet());

    World dst;
    const auto d = dst.CreateEntity();
    std::string err;
    ASSERT_TRUE(TryLoadRuleProps(dst, d, SerializedProps(*schema, src, s), &err)) << err;

    const auto* loaded = dst.GetComponent<TerrainSurfaceRulesEffect>(d);
    ASSERT_NE(loaded, nullptr);
    const TerrainSurfaceRulesEffect authored = AuthoredRuleSet();
    EXPECT_EQ(std::memcmp(&authored, loaded, sizeof(TerrainSurfaceRulesEffect)), 0);
}

TEST(TerrainSurfaceRulesSchema, OnlyLiveRowsAreSerialized)
{
    // The fixed-capacity tail is storage, not content: a two-row set must not put
    // eight rules into the scene file, nor into every diff that touches one.
    const auto* schema = SurfaceRulesSchema();
    ASSERT_NE(schema, nullptr);

    World src;
    const auto s = src.CreateEntity();
    src.AddComponentImmediate(s, AuthoredRuleSet());

    const auto props = SerializedProps(*schema, src, s);
    for (const auto& kv : props)
    {
        EXPECT_EQ(kv.first.find("rule2."), std::string::npos)
            << "a row beyond ruleCount was serialized: " << kv.first;
        EXPECT_EQ(kv.first.find("condition2."), std::string::npos)
            << "a condition beyond conditionCount was serialized: " << kv.first;
    }
    // The noise-only fields ride on the one Noise condition and nowhere else.
    std::size_t noiseFields = 0;
    for (const auto& kv : props)
        if (kv.first.find("noisefrequency") != std::string::npos)
            ++noiseFields;
    EXPECT_EQ(noiseFields, 1u);
}

TEST(TerrainSurfaceRulesSchema, AnOverCapRuleCountIsRejectedLoudly)
{
    // Her decision was a loud overflow error, never a silent drop. A clamp here
    // would load a rule set that bakes a terrain looking nearly right.
    World w;
    const auto e = w.CreateEntity();
    std::string err;
    EXPECT_FALSE(TryLoadRuleProps(
        w, e, {{"rulecount", std::to_string(kMaxTerrainSurfaceRules + 1)}}, &err));
    EXPECT_NE(err.find(std::to_string(kMaxTerrainSurfaceRules)), std::string::npos)
        << "the error must name the cap; got: " << err;
}

TEST(TerrainSurfaceRulesSchema, AnOverCapConditionCountIsRejectedLoudly)
{
    World w;
    const auto e = w.CreateEntity();
    std::string err;
    EXPECT_FALSE(TryLoadRuleProps(
        w, e,
        {{"rulecount", "1"},
         {"rule0.conditioncount", std::to_string(kMaxTerrainRuleConditions + 1)}},
        &err));
    EXPECT_NE(err.find(std::to_string(kMaxTerrainRuleConditions)), std::string::npos)
        << "the error must name the cap; got: " << err;
}

TEST(TerrainSurfaceRulesSchema, AnOutOfRangeRowOrConditionIndexIsRejected)
{
    // The index in the KEY is the other way past the array, and it does not go
    // through ruleCount at all.
    World w;
    const auto e = w.CreateEntity();
    std::string err;
    EXPECT_FALSE(TryLoadRuleProps(
        w, e, {{"rule" + std::to_string(kMaxTerrainSurfaceRules) + ".material", "1"}}, &err));

    World w2;
    const auto e2 = w2.CreateEntity();
    std::string err2;
    EXPECT_FALSE(TryLoadRuleProps(
        w2, e2,
        {{"rule0.condition" + std::to_string(kMaxTerrainRuleConditions) + ".min", "1"}}, &err2));
}

TEST(TerrainSurfaceRulesSchema, ARowIndexTooLargeForUint32IsRejectedRatherThanWrapped)
{
    // 2^32 is rule0 modulo a uint32. Wrapping does not fail the load — it
    // silently writes the authored value into a DIFFERENT row than the file
    // names, which is the one outcome worse than refusing the key.
    World w;
    const auto e = w.CreateEntity();
    std::string err;
    EXPECT_FALSE(TryLoadRuleProps(w, e, {{"rule4294967296.material", "3"}}, &err));

    // Nothing was written to the row the wrap would have selected.
    const auto* loaded = w.GetComponent<TerrainSurfaceRulesEffect>(e);
    if (loaded)
        EXPECT_EQ(loaded->Rules[0].MaterialSlot, 0u) << "the wrapped index reached row 0";

    World w2;
    const auto e2 = w2.CreateEntity();
    std::string err2;
    EXPECT_FALSE(TryLoadRuleProps(w2, e2, {{"rule0.condition4294967296.min", "1"}}, &err2));
}

TEST(TerrainSurfaceRulesSchema, AMaterialSlotPastTheChannelCountIsClampedLikeThePaintSchemas)
{
    // The sibling paint schemas clamp LayerIndex rather than refusing the file,
    // and the bake clamps the same way; an out-of-range slot is a bad value, not
    // a bad file. Loading it must not leave a slot the bake would have to police.
    World w;
    const auto e = w.CreateEntity();
    std::string err;
    ASSERT_TRUE(TryLoadRuleProps(w, e, {{"rulecount", "1"}, {"rule0.material", "99"}}, &err))
        << err;

    const auto* loaded = w.GetComponent<TerrainSurfaceRulesEffect>(e);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->Rules[0].MaterialSlot, GameEngine::Terrain::kMaxTerrainMaterialLayers - 1u);
}

TEST(TerrainSurfaceRulesSchema, AnUnknownEnumeratorIsRejectedRatherThanCastBlindly)
{
    World w;
    const auto e = w.CreateEntity();
    std::string err;
    EXPECT_FALSE(TryLoadRuleProps(w, e, {{"rule0.condition0.kind", "99"}}, &err));

    World w2;
    const auto e2 = w2.CreateEntity();
    std::string err2;
    EXPECT_FALSE(TryLoadRuleProps(w2, e2, {{"rule0.condition0.curve", "7"}}, &err2));
}

TEST(TerrainSurfaceRulesSchema, AddDefaultProvisionsAnEmptyRuleList)
{
    const auto* schema = SurfaceRulesSchema();
    ASSERT_NE(schema, nullptr);

    World w;
    const auto e = w.CreateEntity();
    std::string err;
    ASSERT_TRUE(schema->AddDefault(w, e, &err)) << err;

    const auto* fx = w.GetComponent<TerrainSurfaceRulesEffect>(e);
    ASSERT_NE(fx, nullptr);
    EXPECT_EQ(fx->RuleCount, 0u);
    EXPECT_TRUE(fx->Enabled);
}

using GameEngine::Components::TerrainStampEffect;
using GameEngine::Components::EffectPoolName;
using GameEngine::Components::kTerrainPoolGroupCapacity;

// ---------------------------------------------------------------------------
// Pooling fields on the other height effects
// ---------------------------------------------------------------------------
//
// Average is POOLING, and the loader refuses it wherever no pool accumulator
// exists — a fail-close that matters, because every other blend switch in the
// engine has a fall-through arm that would bake an Average as Add. The four
// HEIGHT effects have a pool; the pre-volume modifiers and the sculpt zone do
// not, and must still be refused.

namespace
{
// Serialize one component block and feed every line back through ApplyProperty,
// exactly as VolumeAndEffectsRoundTripThroughTheirOwnSchemas does.
void RoundTripBlock(const char* name, World& src, GameEngine::ECS::EntityHandle a,
                    World& dst, GameEngine::ECS::EntityHandle b)
{
    const auto* schema = GameEngine::Scene::SceneSchemaRegistry::Find(name);
    ASSERT_NE(schema, nullptr) << name;
    std::vector<std::string> lines;
    GameEngine::Scene::SceneSaveContext saveCtx{};
    schema->Serialize(src, a, saveCtx, lines);
    ASSERT_FALSE(lines.empty()) << name;

    GameEngine::Scene::SceneLoadContext loadCtx{};
    for (const std::string& line : lines)
    {
        const std::size_t dot = line.find('.');
        const std::size_t eq = line.find(" = ");
        ASSERT_NE(dot, std::string::npos);
        ASSERT_NE(eq, std::string::npos);
        std::string prop = line.substr(dot + 1, eq - dot - 1);
        for (char& ch : prop)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const std::string value = line.substr(eq + 3);
        std::string err;
        ASSERT_TRUE(schema->ApplyProperty(dst, b, loadCtx, prop, value, &err)) << line << ": " << err;
    }
}

// Does this component's schema accept blend = 8 (Average)?
bool SchemaAcceptsAverage(const char* name)
{
    const auto* schema = GameEngine::Scene::SceneSchemaRegistry::Find(name);
    EXPECT_NE(schema, nullptr) << name;
    if (!schema)
        return false;
    World w;
    const auto e = w.CreateEntity();
    GameEngine::Scene::SceneLoadContext ctx{};
    std::string err;
    return schema->ApplyProperty(w, e, ctx, "blend", "8", &err);
}
} // namespace

TEST(TerrainEffectPoolSchema, PoolingFieldsRoundTripOnEveryPoolableHeightEffect)
{
    GameEngine::Scene::EnsureTerrainSceneSchemasRegistered();

    // Every field set away from its default, so a property dropped by either side
    // of the round trip shows up as a mismatch rather than as an agreeing default.
    World src;
    const auto a = src.CreateEntity();

    TerrainHeightOffsetEffect offset{};
    offset.StackOrder = 2;
    offset.Offset = -7.5f;
    offset.Blend = TerrainModifierBlend::Average;
    std::memcpy(offset.PoolGroup, "Embankments", 11);
    offset.RespectClaims = true;
    src.AddComponentImmediate(a, offset);

    TerrainNoiseEffect noise{};
    noise.StackOrder = 3;
    noise.Amplitude = 11.25f;
    noise.Blend = TerrainModifierBlend::Average;
    std::memcpy(noise.PoolGroup, "Dunes", 5);
    noise.RespectClaims = true;
    src.AddComponentImmediate(a, noise);

    TerrainStampEffect stamp{};
    stamp.StackOrder = 4;
    stamp.HeightScale = 3.25f;
    stamp.Blend = TerrainModifierBlend::Average;
    std::memcpy(stamp.PoolGroup, "Craters", 7);
    stamp.RespectClaims = true;
    src.AddComponentImmediate(a, stamp);

    World dst;
    const auto b = dst.CreateEntity();
    for (const char* name : {"TerrainHeightOffsetEffect", "TerrainNoiseEffect", "TerrainStampEffect"})
        RoundTripBlock(name, src, a, dst, b);

    const auto* rtOffset = dst.GetComponent<TerrainHeightOffsetEffect>(b);
    ASSERT_NE(rtOffset, nullptr);
    EXPECT_EQ(rtOffset->Blend, TerrainModifierBlend::Average);
    EXPECT_EQ(EffectPoolName(*rtOffset), std::string_view("Embankments"));
    EXPECT_TRUE(rtOffset->RespectClaims);
    EXPECT_FLOAT_EQ(rtOffset->Offset, -7.5f);

    const auto* rtNoise = dst.GetComponent<TerrainNoiseEffect>(b);
    ASSERT_NE(rtNoise, nullptr);
    EXPECT_EQ(rtNoise->Blend, TerrainModifierBlend::Average);
    EXPECT_EQ(EffectPoolName(*rtNoise), std::string_view("Dunes"));
    EXPECT_TRUE(rtNoise->RespectClaims);
    EXPECT_FLOAT_EQ(rtNoise->Amplitude, 11.25f);

    const auto* rtStamp = dst.GetComponent<TerrainStampEffect>(b);
    ASSERT_NE(rtStamp, nullptr);
    EXPECT_EQ(rtStamp->Blend, TerrainModifierBlend::Average);
    EXPECT_EQ(EffectPoolName(*rtStamp), std::string_view("Craters"));
    EXPECT_TRUE(rtStamp->RespectClaims);
    EXPECT_FLOAT_EQ(rtStamp->HeightScale, 3.25f);
}

TEST(TerrainEffectPoolSchema, AverageIsAcceptedByTheHeightEffectsAndRefusedEverywhereElse)
{
    GameEngine::Scene::EnsureTerrainSceneSchemasRegistered();

    // The four effects that reach a pool accumulator.
    EXPECT_TRUE(SchemaAcceptsAverage("TerrainFlattenEffect"));
    EXPECT_TRUE(SchemaAcceptsAverage("TerrainHeightOffsetEffect"));
    EXPECT_TRUE(SchemaAcceptsAverage("TerrainNoiseEffect"));
    EXPECT_TRUE(SchemaAcceptsAverage("TerrainStampEffect"));

    // The pre-volume modifiers and the sculpt zone have no pool, and their blend
    // switches fall through to Add — so an Average reaching one would bake a
    // silent wrong answer. The loader must keep refusing it.
    EXPECT_FALSE(SchemaAcceptsAverage("TerrainNoiseModifier"))
        << "a pre-volume modifier has no pool; Average there would bake as Add";
    EXPECT_FALSE(SchemaAcceptsAverage("TerrainStampModifier"))
        << "a pre-volume modifier has no pool; Average there would bake as Add";
    EXPECT_FALSE(SchemaAcceptsAverage("TerrainSplineModifier"))
        << "a pre-volume modifier has no pool; Average there would bake as Add";
    EXPECT_FALSE(SchemaAcceptsAverage("TerrainSculptZone"))
        << "the sculpt zone bakes its payload on its own Add/Set path; Average would bake as Add";
}

TEST(TerrainEffectPoolSchema, AnOverLongPoolGroupIsRefusedRatherThanTruncated)
{
    GameEngine::Scene::EnsureTerrainSceneSchemasRegistered();

    // Truncation would silently MERGE two pools whose first 31 characters agree,
    // so the loader refuses instead. Same answer at the same bound on every
    // poolable effect.
    const std::string tooLong(kTerrainPoolGroupCapacity, 'x');
    for (const char* name : {"TerrainFlattenEffect", "TerrainHeightOffsetEffect",
                             "TerrainNoiseEffect", "TerrainStampEffect"})
    {
        const auto* schema = GameEngine::Scene::SceneSchemaRegistry::Find(name);
        ASSERT_NE(schema, nullptr) << name;
        World w;
        const auto e = w.CreateEntity();
        GameEngine::Scene::SceneLoadContext ctx{};
        std::string err;
        EXPECT_FALSE(schema->ApplyProperty(w, e, ctx, "poolgroup",
                                           "\"" + tooLong + "\"", &err))
            << name << " accepted a pool group of " << tooLong.size() << " characters";

        // And one character shorter is accepted, so the bound is the field's, not
        // an accidental off-by-one.
        const std::string atLimit(kTerrainPoolGroupCapacity - 1, 'x');
        EXPECT_TRUE(schema->ApplyProperty(w, e, ctx, "poolgroup",
                                          "\"" + atLimit + "\"", &err))
            << name << ": " << err;
    }
}

// ---- TerrainECS::ProvisionTerrainEntity: the one terrain creation every path shares ----------

namespace
{

// The scene's global surface-rules volumes: those TerrainECS::ProvisionTerrainEntity spawns.
std::vector<GameEngine::ECS::EntityHandle> GlobalSurfaceRuleVolumes(GameEngine::ECS::World& world)
{
    std::vector<GameEngine::ECS::EntityHandle> volumes;
    world.Query<GameEngine::ECS::Read<GameEngine::Components::TerrainModifierVolume>,
                GameEngine::ECS::Read<GameEngine::Components::TerrainSurfaceRulesEffect>>()
        .Each([&](GameEngine::ECS::EntityHandle entity,
                            const GameEngine::Components::TerrainModifierVolume& volume,
                            const GameEngine::Components::TerrainSurfaceRulesEffect&) {
            if (volume.Shape == GameEngine::Components::TerrainVolumeShape::Global)
                volumes.push_back(entity);
        });
    return volumes;
}

} // namespace

TEST(TerrainEntityProvisioning, APlanarTerrainGetsGrassAndTheScenesDefaultSurfaceRulesOnce)
{
    namespace C = GameEngine::Components;
    World world;
    const auto preset = GameEngine::CBTTerrainECS::MakeTerrainPreset(GameEngine::CBTTerrainECS::TerrainPreset::LargePlanar);

    const auto first = world.CreateEntity();
    const auto provisioned = GameEngine::TerrainECS::ProvisionTerrainEntity(world, first, preset, C::TerrainPlanetRelief{});
    ASSERT_EQ(provisioned.Error, "");
    EXPECT_TRUE(world.GetComponent<C::Terrain>(first) != nullptr);
    EXPECT_TRUE(world.GetComponent<C::TerrainGrass>(first) != nullptr);
    const auto volumes = GlobalSurfaceRuleVolumes(world);
    ASSERT_EQ(volumes.size(), 1u);
    EXPECT_EQ(volumes[0], provisioned.SurfaceRulesVolume);
    const auto* rules = world.GetComponent<C::TerrainSurfaceRulesEffect>(volumes[0]);
    ASSERT_NE(rules, nullptr);
    const auto defaults = GameEngine::TerrainECS::MakeDefaultTerrainSurfaceRules();
    ASSERT_GT(defaults.RuleCount, 0u);
    EXPECT_EQ(rules->RuleCount, defaults.RuleCount);
    EXPECT_EQ(std::memcmp(rules->Rules, defaults.Rules, sizeof(defaults.Rules)), 0);
    const auto* name = world.GetComponent<C::Name>(volumes[0]);
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name->View(), "Terrain Surface Rules");
    // The tiled preset is provisioned by extraction: no eager heightfield collider here.
    EXPECT_EQ(world.GetComponent<GameEngine::Components::HeightFieldColliderShape>(first), nullptr);

    // The rules belong to the scene: a second terrain finds them and spawns none.
    const auto second = world.CreateEntity();
    const auto again = GameEngine::TerrainECS::ProvisionTerrainEntity(world, second, preset, C::TerrainPlanetRelief{});
    ASSERT_EQ(again.Error, "");
    EXPECT_FALSE(again.SurfaceRulesVolume.IsValid());
    EXPECT_EQ(GlobalSurfaceRuleVolumes(world).size(), 1u);
}

TEST(TerrainEntityProvisioning, ASingleTileTerrainCollidesAndAPlanetGetsItsReliefAndNoRules)
{
    namespace C = GameEngine::Components;
    ScopedTerrainService scoped;
    World world;

    const auto small = world.CreateEntity();
    const auto smallPreset = GameEngine::CBTTerrainECS::MakeTerrainPreset(GameEngine::CBTTerrainECS::TerrainPreset::SmallPlanar);
    ASSERT_EQ(GameEngine::TerrainECS::ProvisionTerrainEntity(world, small, smallPreset, C::TerrainPlanetRelief{}).Error, "");
    const auto* shape = world.GetComponent<C::HeightFieldColliderShape>(small);
    ASSERT_NE(shape, nullptr);
    const auto* smallTerrain = world.GetComponent<C::Terrain>(small);
    EXPECT_NE(GameEngine::TerrainECS::TerrainService::Get().GetTerrainData(GameEngine::TerrainECS::TerrainHandle{
                  smallTerrain->TerrainDataHandle, smallTerrain->TerrainDataGeneration}),
              nullptr)
        << "the single tile's heightfield is provisioned now";
    EXPECT_EQ(shape->sizeX, smallPreset.SizeX);

    World planetWorld;
    const auto planet = planetWorld.CreateEntity();
    const auto relief = GameEngine::CBTTerrainECS::MakePlanetReliefPreset(GameEngine::CBTTerrainECS::TerrainPreset::Planet5km);
    const auto planetResult = GameEngine::TerrainECS::ProvisionTerrainEntity(
        planetWorld, planet, GameEngine::CBTTerrainECS::MakeTerrainPreset(GameEngine::CBTTerrainECS::TerrainPreset::Planet5km), relief);
    ASSERT_EQ(planetResult.Error, "");
    const auto* attached = planetWorld.GetComponent<C::TerrainPlanetRelief>(planet);
    ASSERT_NE(attached, nullptr);
    EXPECT_EQ(attached->Amplitude, relief.Amplitude);
    EXPECT_FALSE(planetResult.SurfaceRulesVolume.IsValid());
    EXPECT_TRUE(GlobalSurfaceRuleVolumes(planetWorld).empty());
    EXPECT_EQ(planetWorld.GetComponent<C::HeightFieldColliderShape>(planet), nullptr);
}

TEST(TerrainEntityProvisioning, APlanarSideBeyondWhatTheDensityCanTileIsRefusedWithTheLimit)
{
    namespace C = GameEngine::Components;
    World world;
    auto config = GameEngine::CBTTerrainECS::MakeTerrainPreset(GameEngine::CBTTerrainECS::TerrainPreset::LargePlanar);
    const float maxExtent = GameEngine::TerrainECS::MaxTerrainExtentMetres(config.SamplesPerMeter);
    EXPECT_EQ(maxExtent, 131072.0f) << "128 tiles of 1024 samples at 1 sample per metre";

    config.SizeX = maxExtent;
    config.SizeZ = maxExtent;
    const auto atLimit = world.CreateEntity();
    EXPECT_EQ(GameEngine::TerrainECS::ProvisionTerrainEntity(world, atLimit, config, C::TerrainPlanetRelief{}).Error, "");
    const auto plan = GameEngine::TerrainECS::DeriveTerrainSizingPlan(config.SizeX, config.SizeZ, config.SamplesPerMeter);
    EXPECT_EQ(plan.TilesPerAxisX, GameEngine::TerrainECS::kMaxTerrainTilesPerAxis);

    for (const float side : {std::nextafter(maxExtent, 2.0f * maxExtent), 1e30f, 0.0f, -5.0f})
    {
        config.SizeX = side;
        const auto refused = world.CreateEntity();
        const auto result = GameEngine::TerrainECS::ProvisionTerrainEntity(world, refused, config, C::TerrainPlanetRelief{});
        EXPECT_NE(result.Error.find("up to 131072 m on each side"), std::string::npos) << result.Error;
        EXPECT_EQ(world.GetComponent<C::Terrain>(refused), nullptr) << "a refused terrain leaves the entity untouched";
    }
    config.SizeX = 200000.0f;
    const auto refused = world.CreateEntity();
    EXPECT_EQ(GameEngine::TerrainECS::ProvisionTerrainEntity(world, refused, config, C::TerrainPlanetRelief{}).Error,
              "A terrain at 1 samples per meter is up to 131072 m on each side; this one is 200000 x 131072 m. "
              "Make it smaller or lower its samples per meter.");
}

TEST(TerrainEntityProvisioning, TheDensityLimitAndTheExtentLimitAreOneRule)
{
    using GameEngine::TerrainECS::MaxTerrainExtentMetres;
    using GameEngine::TerrainECS::MaxTerrainSamplesPerMeter;
    // At the limit the two functions map onto each other; one step past it is over.
    EXPECT_EQ(MaxTerrainSamplesPerMeter(131072.0f, 4096.0f), 1.0f);
    EXPECT_EQ(MaxTerrainExtentMetres(MaxTerrainSamplesPerMeter(131072.0f, 256.0f)), 131072.0f);
    EXPECT_EQ(MaxTerrainSamplesPerMeter(32768.0f, 32768.0f), 4.0f);
    const float over = std::nextafter(1.0f, 2.0f);
    EXPECT_LT(MaxTerrainExtentMetres(over), 131072.0f) << "a denser terrain of the same side is past the limit";
}

TEST(TerrainEntityProvisioning, TheSizingPlanCountsTilesWithinRangeForAnySize)
{
    // A side no provisioning accepts still reaches the derivation from a scene file or a
    // component write: its tile counts saturate instead of overflowing.
    const auto plan = GameEngine::TerrainECS::DeriveTerrainSizingPlan(1e30f, 256.0f, 1.0f);
    EXPECT_EQ(plan.TilesPerAxisX, GameEngine::TerrainECS::kMaxDerivedTilesPerAxis);
    EXPECT_EQ(plan.TilesPerAxisZ, 1u);
    EXPECT_EQ(plan.TotalTiles, GameEngine::TerrainECS::kMaxDerivedTilesPerAxis);
    // Tiles are square on the shorter side (256 m here, 256 samples inside each).
    EXPECT_EQ(plan.UnifiedWidth, GameEngine::TerrainECS::kMaxDerivedTilesPerAxis * 256u + 1u);
    EXPECT_EQ(plan.Source, GameEngine::TerrainECS::TerrainHeightSource::Atlas);
    EXPECT_TRUE(GameEngine::Terrain::TerrainNeedsTiling(1e30f, 256.0f, 1.0f)) << "a side past the cast range still tiles";
    const auto nan = GameEngine::TerrainECS::DeriveTerrainSizingPlan(std::nanf(""), 256.0f, 1.0f);
    EXPECT_LE(nan.TilesPerAxisX, GameEngine::TerrainECS::kMaxDerivedTilesPerAxis);
}

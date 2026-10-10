// CBTUpdateSystem::ConsumeDirtyRegion integration coverage (adversarial-review C5).
// Drives the real ECS -> TerrainService -> classify path that C4/C5 rely on, with a
// real slot-0 terrain, and asserts:
//   * C1: the FIRST terrain (TerrainService slot 0 == handle index 0, generation 1)
//     yields a non-empty dirty rect. The pre-fix guard treated handle-index 0 as
//     "unset" and early-returned, so CBT's default single-terrain scene was inert.
//   * m3: the log cursor only COMMITS once the render side confirms it recorded the
//     pushed rect (the update-record count advanced). A frame whose update never ran
//     re-arms the rect instead of silently dropping the edit.
// Engine-linked (TerrainService lives in TerrainECS -> Engine.dll); modelled on
// TerrainRegionBakeTests.

#include <gtest/gtest.h>

#include "CBTTerrain/CBTInstance.h"          // CBTClassifyDesc
#include "CBTTerrain/CBTSphereFaceMap.h"     // SphereEditRegions (cross-face drain)
#include "CBTTerrain/SphereSculptPaging.h"   // DeriveSculptVirtualDim (radius scaling)
#include "CBTTerrainECS/Systems/CBTUpdateSystem.h"
#include "CBTTerrainECS/Systems/RegisterCBTSystems.h"
#include "Components/Terrain/Terrain.h"
#include "ECS/ECSTemplates.h"
#include "ECS/SystemScheduling.h"
#include "ECS/World.h"
#include "Rendering/Core/Device.h"
#include "Scene/SceneSchemaRegistry.h"
#include "SplineECS/Systems/RegisterSplineSystems.h"
#include "TerrainECS/TerrainRenderFeature.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/Systems/RegisterTerrainSystems.h"
#include "TerrainECS/Systems/TerrainWorldHooks.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <set>
#include <string_view>
#include <vector>

namespace
{
using namespace GameEngine;
using namespace GameEngine::TerrainECS;
using GameEngine::CBTTerrainECS::CBTUpdateSystem;

// Standalone headless Vulkan device for the TerrainRenderFeature extraction-math oracle
// (mirrors CBTTestHarness::MakeHeadlessDevice; that helper lives in the CBTTerrain test
// tree, not linkable here). Returns null when no device is available -> the oracle skips.
std::unique_ptr<Rendering::IDevice> MakeHeadlessDeviceOrNull()
{
#if defined(_WIN32)
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif
    Rendering::DeviceDesc dd{};
    dd.preferredAPI = Rendering::GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    auto device = Rendering::DeviceFactory::CreateDevice(dd);
    if (!device || !device->Initialize(dd))
        return nullptr;
    return device;
}

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

constexpr uint32 kHeightmapDim = 129;

Terrain::TerrainConfig MakeConfig()
{
    Terrain::TerrainConfig cfg{};
    cfg.HeightmapWidth = kHeightmapDim;
    cfg.HeightmapHeight = kHeightmapDim;
    cfg.WorldSizeX = 256.0f;
    cfg.WorldSizeZ = 256.0f;
    cfg.HeightScale = 64.0f;
    cfg.LODLevels = 4;
    return cfg;
}

// One terrain entity carrying the service handle exactly as TerrainExtractionSystem
// wires it (index + generation).
void AddTerrainEntity(ECS::World& world, TerrainHandle handle)
{
    auto e = world.CreateEntity();
    Components::Terrain terrain{};
    terrain.TerrainDataHandle = handle.Index;
    terrain.TerrainDataGeneration = handle.Generation;
    world.AddComponentImmediate<Components::Terrain>(e, terrain);
}

// A TILED terrain entity exactly as TerrainExtractionSystem wires it once a terrain
// auto-tiles: the single-terrain handle is ZEROED and the tiled handle carries the
// live slot. `enabled` lets a test cover the disabled case.
ECS::EntityHandle AddTiledTerrainEntity(ECS::World& world, uint32 tiledIndex,
                                        uint32 tiledGeneration, bool enabled = true)
{
    auto e = world.CreateEntity();
    Components::Terrain terrain{};
    terrain.TerrainDataHandle = 0;
    terrain.TerrainDataGeneration = 0;
    terrain.TiledTerrainHandle = tiledIndex;
    terrain.TiledTerrainGeneration = tiledGeneration;
    world.AddComponentImmediate<Components::Terrain>(e, terrain);
    ECS::Entity(&world, e).SetEnabled<Components::Terrain>(enabled);
    return e;
}

bool RectIsEmpty(const CBTTerrain::CBTClassifyDesc& c)
{
    return c.DirtyMaxU <= c.DirtyMinU || c.DirtyMaxV <= c.DirtyMinV;
}

// One touched (face, face-local UV rect) region — the single-face-edit case.
CBTTerrain::SphereEditRegions OneFace(uint32 face, float minU, float minV, float maxU, float maxV)
{
    CBTTerrain::SphereEditRegions r{};
    r.Count = 1u;
    r.Rects[0] = CBTTerrain::SphereFaceUVRect{face, minU, minV, maxU, maxV};
    return r;
}
} // namespace

// C1 regression guard: slot 0 (handle index 0, generation 1) must reclassify. This
// FAILS if ConsumeDirtyRegion's terrain guard reverts to the `handle == 0` sentinel.
TEST(CBTRegionConsume, FirstTerrainSlotYieldsNonEmptyRect)
{
    ScopedTerrainService svc;
    const TerrainHandle handle = TerrainService::Get().CreateTerrain(MakeConfig());
    ASSERT_EQ(handle.Index, 0u) << "first terrain must occupy slot 0 (the overloaded-handle case)";
    ASSERT_NE(handle.Generation, 0u);

    ECS::World world;
    AddTerrainEntity(world, handle);

    // CreateTerrain seeds a full-terrain dirty entry (MarkFullDirty at version 1);
    // a fresh CBT cursor (version 0) must collect it -> the whole UV square.
    CBTUpdateSystem system(nullptr);
    CBTTerrain::CBTClassifyDesc classify{};
    system.ConsumeDirtyRegion(world, /*updateRecordCount=*/0u, classify);

    EXPECT_FALSE(RectIsEmpty(classify))
        << "the default single-terrain scene produced an empty dirty rect — C5 is inert";
    EXPECT_NEAR(classify.DirtyMinU, 0.0f, 1e-3f);
    EXPECT_NEAR(classify.DirtyMinV, 0.0f, 1e-3f);
    EXPECT_NEAR(classify.DirtyMaxU, 1.0f, 1e-3f);
    EXPECT_NEAR(classify.DirtyMaxV, 1.0f, 1e-3f);
}

// m3: with no confirmation (the update-record count never advances), the pushed rect
// must be RE-ARMED every frame, not committed-then-dropped. FAILS if the cursor
// advances unconditionally at consume time.
TEST(CBTRegionConsume, UnconfirmedEditIsReArmedNotDropped)
{
    ScopedTerrainService svc;
    const TerrainHandle handle = TerrainService::Get().CreateTerrain(MakeConfig());
    ECS::World world;
    AddTerrainEntity(world, handle);

    CBTUpdateSystem system(nullptr);

    // Frame A: push the full-dirty rect at record count 0.
    CBTTerrain::CBTClassifyDesc a{};
    system.ConsumeDirtyRegion(world, 0u, a);
    ASSERT_FALSE(RectIsEmpty(a));

    // Frame B: same record count (0) => no update recorded the rect => re-arm.
    CBTTerrain::CBTClassifyDesc b{};
    system.ConsumeDirtyRegion(world, 0u, b);
    EXPECT_FALSE(RectIsEmpty(b)) << "an unconfirmed edit was dropped instead of re-armed";
}

// m3: once the render side confirms (record count advanced), the cursor commits and a
// quiescent terrain produces an empty rect; a subsequent NEW edit re-arms again.
TEST(CBTRegionConsume, ConfirmedEditBecomesQuiescentThenReArmsOnNewEdit)
{
    ScopedTerrainService svc;
    const TerrainHandle handle = TerrainService::Get().CreateTerrain(MakeConfig());
    ECS::World world;
    AddTerrainEntity(world, handle);
    TerrainData* data = TerrainService::Get().GetTerrainData(handle);
    ASSERT_NE(data, nullptr);

    CBTUpdateSystem system(nullptr);

    // Push at record count 0.
    CBTTerrain::CBTClassifyDesc push{};
    system.ConsumeDirtyRegion(world, 0u, push);
    ASSERT_FALSE(RectIsEmpty(push));

    // Record count advanced to 1 => the pushed rect was recorded => commit. Nothing
    // newer in the log => quiescent (empty rect).
    CBTTerrain::CBTClassifyDesc quiescent{};
    system.ConsumeDirtyRegion(world, 1u, quiescent);
    EXPECT_TRUE(RectIsEmpty(quiescent)) << "confirmed edit still re-armed — the cursor never committed";

    // A new region edit must reach the classify again (still record count 1: the new
    // edit is a fresh in-flight rect, not yet confirmed).
    data->MarkRegionDirty(16, 16, 48, 48);
    CBTTerrain::CBTClassifyDesc reedit{};
    system.ConsumeDirtyRegion(world, 1u, reedit);
    EXPECT_FALSE(RectIsEmpty(reedit)) << "a new edit after a committed one was not reclassified";
}

// ---------------------------------------------------------------------------
// Planet editing (plan §planet-editing): ConsumeSphereSculpt — the (face,rect) analogue
// of ConsumeDirtyRegion. `regions` is the clear-on-read drain from TerrainService, so the
// tests pass a NON-EMPTY drain on an edit frame and an EMPTY drain on subsequent frames
// (the service already cleared it). SphereSculptEnabled turns on for good once any dab
// lands; each touched face is drained to the GPU one per recorded update and re-armed from
// the pending set (not a fresh ingest) until the render side confirms it recorded.
// ---------------------------------------------------------------------------

// No edits -> the sculpt sample stays disabled and the rect is empty (quiescence: the
// C7 default planet is byte-identical + pays nothing).
TEST(CBTRegionConsume, SphereSculptQuiescentUntilEdit)
{
    CBTUpdateSystem system(nullptr);
    CBTTerrain::CBTClassifyDesc c{};
    system.ConsumeSphereSculpt(/*version=*/0u, /*recordCount=*/0u, CBTTerrain::SphereEditRegions{}, c);
    EXPECT_EQ(c.SphereSculptEnabled, 0u);
    EXPECT_TRUE(RectIsEmpty(c));
}

// The first dab (version 1) turns the sculpt sample on for good and pushes the edited
// face's (face-local) rect into the classify.
TEST(CBTRegionConsume, SphereSculptEditPushesFaceRect)
{
    CBTUpdateSystem system(nullptr);
    CBTTerrain::CBTClassifyDesc c{};
    system.ConsumeSphereSculpt(1u, 0u, OneFace(2u, 0.25f, 0.30f, 0.55f, 0.60f), c);
    EXPECT_EQ(c.SphereSculptEnabled, 1u);
    EXPECT_EQ(c.DirtyFace, 2u);
    EXPECT_FALSE(RectIsEmpty(c));
    EXPECT_NEAR(c.DirtyMinU, 0.25f, 1e-4f);
    EXPECT_NEAR(c.DirtyMaxV, 0.60f, 1e-4f);
}

// An unconfirmed edit (record count never advances) is RE-ARMED from the pending set, not
// dropped — the same no-scene-view guard the planar path carries. The second frame drains EMPTY
// (the service already cleared it), so a passing re-arm proves the pending set held the face.
TEST(CBTRegionConsume, SphereSculptUnconfirmedReArmed)
{
    CBTUpdateSystem system(nullptr);
    CBTTerrain::CBTClassifyDesc a{};
    system.ConsumeSphereSculpt(1u, 0u, OneFace(0u, 0.1f, 0.1f, 0.4f, 0.4f), a);
    ASSERT_FALSE(RectIsEmpty(a));
    CBTTerrain::CBTClassifyDesc b{};
    system.ConsumeSphereSculpt(1u, 0u, CBTTerrain::SphereEditRegions{}, b);
    EXPECT_FALSE(RectIsEmpty(b)) << "an unconfirmed sphere edit was dropped instead of re-armed";
}

// Once confirmed (record count advanced), a held version goes quiescent (empty rect) but
// the sculpt sample STAYS enabled (relief persists); a new dab (version 2) re-arms.
TEST(CBTRegionConsume, SphereSculptConfirmedQuiescentThenReArms)
{
    CBTUpdateSystem system(nullptr);
    CBTTerrain::CBTClassifyDesc push{};
    system.ConsumeSphereSculpt(1u, 0u, OneFace(3u, 0.2f, 0.2f, 0.5f, 0.5f), push);
    ASSERT_FALSE(RectIsEmpty(push));

    // Record count advanced -> the pushed rect was recorded -> commit; drain is now EMPTY.
    CBTTerrain::CBTClassifyDesc quiescent{};
    system.ConsumeSphereSculpt(1u, 1u, CBTTerrain::SphereEditRegions{}, quiescent);
    EXPECT_TRUE(RectIsEmpty(quiescent)) << "confirmed sphere edit still re-armed";
    EXPECT_EQ(quiescent.SphereSculptEnabled, 1u) << "relief must persist after the edit frame";

    // A new dab (version bumped) reclassifies again.
    CBTTerrain::CBTClassifyDesc reedit{};
    system.ConsumeSphereSculpt(2u, 1u, OneFace(4u, 0.6f, 0.6f, 0.8f, 0.8f), reedit);
    EXPECT_FALSE(RectIsEmpty(reedit)) << "a new sphere dab after a committed one was not reclassified";
    EXPECT_EQ(reedit.DirtyFace, 4u);
}

// CROSS-FACE SEAM ORACLE (Finding 1): a dab straddling a cube edge touches TWO faces. The GPU
// Classify push carries one (face,rect), so pre-fix only the primary face reclassified and the
// neighbour face kept its pre-edit heights until a later dab centred on it — a seam that healed
// only when the exact edge was brushed again. The per-face drain must reclassify BOTH faces (count +
// content) over successive recorded updates, then go quiescent.
TEST(CBTRegionConsume, SphereSculptCrossFaceDrainsBothFaces)
{
    CBTUpdateSystem system(nullptr);
    CBTTerrain::SphereEditRegions regions{};
    regions.Count = 2u;
    regions.Rects[0] = CBTTerrain::SphereFaceUVRect{0u, 0.80f, 0.40f, 1.00f, 0.60f}; // +X edge band
    regions.Rects[1] = CBTTerrain::SphereFaceUVRect{2u, 0.00f, 0.40f, 0.20f, 0.60f}; // +Y edge band

    auto faceRect = [&](uint32 face) -> const CBTTerrain::SphereFaceUVRect& {
        return regions.Rects[0].Face == face ? regions.Rects[0] : regions.Rects[1];
    };
    auto expectMatches = [](const CBTTerrain::CBTClassifyDesc& c, const CBTTerrain::SphereFaceUVRect& r) {
        EXPECT_NEAR(c.DirtyMinU, r.MinU, 1e-4f);
        EXPECT_NEAR(c.DirtyMinV, r.MinV, 1e-4f);
        EXPECT_NEAR(c.DirtyMaxU, r.MaxU, 1e-4f);
        EXPECT_NEAR(c.DirtyMaxV, r.MaxV, 1e-4f);
    };

    // Update 1 (record 0): one face pushed with its exact written extent.
    CBTTerrain::CBTClassifyDesc c1{};
    system.ConsumeSphereSculpt(1u, 0u, regions, c1);
    ASSERT_FALSE(RectIsEmpty(c1));
    const uint32 firstFace = c1.DirtyFace;
    ASSERT_TRUE(firstFace == 0u || firstFace == 2u);
    expectMatches(c1, faceRect(firstFace));

    // Update 2 (record advanced -> first confirmed; drain now EMPTY): the OTHER face reclassifies
    // from the pending set (the fix).
    CBTTerrain::CBTClassifyDesc c2{};
    system.ConsumeSphereSculpt(1u, 1u, CBTTerrain::SphereEditRegions{}, c2);
    ASSERT_FALSE(RectIsEmpty(c2));
    const uint32 secondFace = c2.DirtyFace;
    EXPECT_NE(secondFace, firstFace) << "the neighbour face never reclassified — the cross-face seam";
    EXPECT_TRUE(secondFace == 0u || secondFace == 2u);
    expectMatches(c2, faceRect(secondFace));

    // Update 3 (record advanced -> both confirmed): quiescent (no residual work).
    CBTTerrain::CBTClassifyDesc c3{};
    system.ConsumeSphereSculpt(1u, 2u, CBTTerrain::SphereEditRegions{}, c3);
    EXPECT_TRUE(RectIsEmpty(c3)) << "both faces drained but Classify still dirty — not quiescent";
}

// A corner edit touches THREE faces; all three must reclassify (over three recorded updates),
// then quiescent. The modifier bake path shares this drain, so a multi-face modifier re-evaluates
// every touched face instead of just the first.
TEST(CBTRegionConsume, SphereSculptCornerDrainsThreeFaces)
{
    CBTUpdateSystem system(nullptr);
    CBTTerrain::SphereEditRegions regions{};
    regions.Count = 3u;
    regions.Rects[0] = CBTTerrain::SphereFaceUVRect{0u, 0.90f, 0.90f, 1.00f, 1.00f};
    regions.Rects[1] = CBTTerrain::SphereFaceUVRect{2u, 0.00f, 0.90f, 0.10f, 1.00f};
    regions.Rects[2] = CBTTerrain::SphereFaceUVRect{4u, 0.90f, 0.00f, 1.00f, 0.10f};

    std::set<uint32> drained;
    for (uint32 rc = 0; rc < 3u; ++rc)
    {
        // Only the first frame drains the (three-face) union; the rest drain EMPTY and reclassify
        // the remaining faces from the pending set.
        const CBTTerrain::SphereEditRegions in = rc == 0u ? regions : CBTTerrain::SphereEditRegions{};
        CBTTerrain::CBTClassifyDesc c{};
        system.ConsumeSphereSculpt(1u, rc, in, c);
        ASSERT_FALSE(RectIsEmpty(c)) << "corner face " << rc << " did not reclassify";
        drained.insert(c.DirtyFace);
    }
    EXPECT_EQ(drained.size(), 3u) << "a corner edit did not reclassify all three faces";
    EXPECT_TRUE(drained.count(0u) && drained.count(2u) && drained.count(4u));

    CBTTerrain::CBTClassifyDesc quiescent{};
    system.ConsumeSphereSculpt(1u, 3u, CBTTerrain::SphereEditRegions{}, quiescent);
    EXPECT_TRUE(RectIsEmpty(quiescent)) << "all corner faces drained but Classify still dirty";
}

// Sub-texel gate + radius scaling (Finding 2): the footprint-in-texels helper now takes the runtime
// virtual dim. At the pre-paging fixed 256 a small angular brush is sub-texel; with the radius-scaled
// paging dim a FIXED world brush stays supra-texel at every radius — the invisible-brush fix.
TEST(CBTRegionConsume, SphereSculptDabFootprintTexelsScalesWithVirtualDim)
{
    using GameEngine::TerrainECS::TerrainService;
    // At a fixed 256/face: a modest angular brush is several texels; a tiny one is sub-texel.
    EXPECT_GT(TerrainService::SphereSculptDabFootprintTexels(0.02f, 256u), 1.0f);
    EXPECT_LT(TerrainService::SphereSculptDabFootprintTexels(0.001f, 256u), 1.0f);
    // A fixed 128 m world brush stays supra-texel at every radius because the virtual dim scales
    // with the radius (the whole point of the page table).
    for (float radius : {2000.0f, 20000.0f, 50000.0f})
    {
        const uint32_t dim = GameEngine::CBTTerrain::DeriveSculptVirtualDim(radius);
        const float ar = 128.0f / radius; // angularRadius for a 128 m brush
        EXPECT_GT(TerrainService::SphereSculptDabFootprintTexels(ar, dim), 1.0f)
            << "a 128 m brush went sub-texel at R=" << radius;
    }
}

// FAST-STROKE RESIDUE ORACLE: TWO disjoint dabs on the SAME face landing between two drains must
// BOTH reach the render dirty union. The layer's LastRegions holds only the last dab, so draining a
// per-face UNION accumulator (not LastRegions) is what stops the earlier dab's footprint from
// staying stale until a later edit overlaps it — the "gaps until you go over the exact area"
// residue. Discriminating: reverting ConsumeSphereSculptDirtyRegions to LastRegions covers only the
// second dab, so the union-contains assertions fail.
TEST(CBTRegionConsume, SphereSculptRenderDirtyUnionsMultipleDabsBetweenDrains)
{
    ScopedTerrainService svc;
    auto& s = TerrainService::Get();

    // Two dabs, both dominant-+X (face 0) at disjoint interior spots -> disjoint face-0 rects.
    const auto r1 = s.ApplySphereSculptDab(1.0f, -0.6f, -0.6f, 0.03f, 10.0f, false);
    const auto r2 = s.ApplySphereSculptDab(1.0f, 0.6f, 0.6f, 0.03f, 10.0f, false);

    auto faceZeroRect = [](const CBTTerrain::SphereEditRegions& reg, bool& found) {
        for (uint32 i = 0; i < reg.Count; ++i)
            if (reg.Rects[i].Face == 0u)
            {
                found = true;
                return reg.Rects[i];
            }
        found = false;
        return CBTTerrain::SphereFaceUVRect{};
    };
    bool f1 = false, f2 = false;
    const auto rect1 = faceZeroRect(r1, f1);
    const auto rect2 = faceZeroRect(r2, f2);
    ASSERT_TRUE(f1 && f2) << "both dabs should land on the +X face (0)";
    ASSERT_TRUE(rect1.MaxU < rect2.MinU || rect2.MaxU < rect1.MinU || rect1.MaxV < rect2.MinV ||
                rect2.MaxV < rect1.MinV)
        << "test dabs overlap — pick more separated directions";

    CBTTerrain::SphereEditRegions drained{};
    ASSERT_TRUE(s.ConsumeSphereSculptDirtyRegions(drained));
    bool fd = false;
    const auto rectD = faceZeroRect(drained, fd);
    ASSERT_TRUE(fd);
    // The drained union must CONTAIN both dabs' footprints (LastRegions would drop the first).
    EXPECT_LE(rectD.MinU, std::min(rect1.MinU, rect2.MinU) + 1e-4f);
    EXPECT_LE(rectD.MinV, std::min(rect1.MinV, rect2.MinV) + 1e-4f);
    EXPECT_GE(rectD.MaxU, std::max(rect1.MaxU, rect2.MaxU) - 1e-4f);
    EXPECT_GE(rectD.MaxV, std::max(rect1.MaxV, rect2.MaxV) - 1e-4f);

    // Clear-on-read: a second drain with no new edit returns nothing.
    CBTTerrain::SphereEditRegions again{};
    EXPECT_FALSE(s.ConsumeSphereSculptDirtyRegions(again)) << "drain must clear on read";
}

// PLANET-RESIZE REMAP ORACLE (sculpt-content remap slice): a live radius edit re-derives the
// sculpt grid over authored content; ConfigurePlanetSculpt must treat the resulting remap as an
// EDIT — advancing the sculpt version (the forced-VertexEval + GPU upload keys) and feeding the
// remapped regions into BOTH dirty unions (render re-tess drain + per-face physics) exactly like
// a dab. DISCRIMINATING: dropping the AccumulateSphereSculptDirtyFaces call on the remap regions
// leaves the drains empty and the classify/physics never refresh the resized content.
TEST(CBTRegionConsume, SphereSculptResizeRemapFeedsDirtyUnionsAndVersion)
{
    ScopedTerrainService svc;
    auto& s = TerrainService::Get();

    s.ConfigurePlanetSculpt(2000.0f);
    const auto dabRegions = s.ApplySphereSculptDab(1.0f, 0.1f, 0.1f, 0.03f, 10.0f, false);
    ASSERT_GT(dabRegions.Count, 0u);
    // Drain both consumers so anything present AFTER the resize is the remap's contribution.
    CBTTerrain::SphereEditRegions drained{};
    ASSERT_TRUE(s.ConsumeSphereSculptDirtyRegions(drained));
    for (uint32 face = 0; face < 6u; ++face)
        s.ClearPlanetSculptDirtyFace(face);
    const uint64 versionBefore = s.SphereSculptVersion();

    // The resize: a live radius edit re-deriving a different grid triggers the remap.
    s.ConfigurePlanetSculpt(20000.0f);
    EXPECT_EQ(s.GetPlanetSculptGeometry().VirtualDim,
              GameEngine::CBTTerrain::DeriveSculptVirtualDim(20000.0f));
    EXPECT_EQ(s.SphereSculptVersion(), versionBefore + 1u) << "a remap IS an edit";

    CBTTerrain::SphereEditRegions remapDrain{};
    ASSERT_TRUE(s.ConsumeSphereSculptDirtyRegions(remapDrain))
        << "the remap must feed the render dirty union";
    bool coversDabFace = false;
    for (uint32 i = 0; i < remapDrain.Count; ++i)
        coversDabFace = coversDabFace || remapDrain.Rects[i].Face == 0u;
    EXPECT_TRUE(coversDabFace) << "the remapped dab's face must be marked dirty";
    EXPECT_TRUE(s.GetPlanetSculptMirror().Faces[0].Touched)
        << "the remap must also feed the physics per-face union (collider refresh)";

    // Idempotent per-frame call pattern: the SAME radius again is a no-op (no version churn).
    s.ConfigurePlanetSculpt(20000.0f);
    EXPECT_EQ(s.SphereSculptVersion(), versionBefore + 1u);
    CBTTerrain::SphereEditRegions idle{};
    EXPECT_FALSE(s.ConsumeSphereSculptDirtyRegions(idle)) << "an unchanged radius must stay quiescent";
}

// C8 draw-gate oracle: CBT is the only terrain renderer and CBTRenderNode emits its
// indirect draw ONLY when the feature is active; the feature is active iff
// ComputeTerrainActive is true. A TILED terrain zeroes its single-terrain handle and
// carries only TiledTerrainHandle, so the pre-C8 single-handle-only gate returned FALSE
// for it — CBT never activated, no draw was emitted, and the terrain vanished at the
// tiling flip. This asserts the gate opens for a tiled-only terrain (DISCRIMINATING:
// reverting ComputeTerrainActive to the single-handle-only test fails this).
TEST(CBTRegionConsume, TiledTerrainActivatesCBTDrawGate)
{
    ECS::World world;
    AddTiledTerrainEntity(world, /*tiledIndex=*/0u, /*tiledGeneration=*/1u);
    EXPECT_TRUE(CBTUpdateSystem::ComputeTerrainActive(world))
        << "a tiled terrain (single handle zeroed) left CBT inactive — the vanish bug";
}

TEST(CBTRegionConsume, SingleTerrainActivatesCBTDrawGate)
{
    ScopedTerrainService svc;
    const TerrainHandle handle = TerrainService::Get().CreateTerrain(MakeConfig());
    ECS::World world;
    AddTerrainEntity(world, handle);
    EXPECT_TRUE(CBTUpdateSystem::ComputeTerrainActive(world));
}

TEST(CBTRegionConsume, NoHandleOrDisabledTerrainLeavesCBTInactive)
{
    ECS::World empty;
    EXPECT_FALSE(CBTUpdateSystem::ComputeTerrainActive(empty))
        << "no terrain entity should leave CBT quiescent";

    ECS::World disabled;
    AddTiledTerrainEntity(disabled, 0u, 1u, /*enabled=*/false);
    EXPECT_FALSE(CBTUpdateSystem::ComputeTerrainActive(disabled))
        << "a disabled tiled terrain must not activate CBT";
}

// C8 delete-leak oracle: removing a Terrain component must free a TILED terrain's data
// (and its unified GPU textures). Extraction ZEROES TerrainDataHandle for a tiled
// terrain and carries only the tiled pair, so a release that checks the single handle
// alone strands the tiled store plus up to ~805 MiB of unified textures until app exit.
// DISCRIMINATING: drop the tiled half of ReleaseTerrainOwnedResources and the store is
// still non-null here.
//
// Release is the world's OnRemove<Terrain> hook, so the world needs the registrar the
// engine installs on its primary world — a world without it holds Terrain components
// that free nothing. That is the contract, and it is why this registers explicitly
// rather than relying on the schema to release by itself.
TEST(CBTRegionConsume, RemoveTiledTerrainComponentReleasesTiledStore)
{
    ScopedTerrainService svc;

    TiledTerrainConfig cfg{};
    cfg.WorldSizeX = 4096.0f;
    cfg.WorldSizeZ = 4096.0f;
    cfg.HeightScale = 200.0f;
    cfg.SamplesPerMeter = 1.0f;
    cfg.PatchGridSize = 32;
    const TiledTerrainHandle tiled = TerrainService::Get().CreateTiledTerrain(cfg);
    TerrainService::Get().LoadTile(tiled, {0, 0});
    ASSERT_NE(TerrainService::Get().GetTiledTerrainData(tiled), nullptr);

    // Entity carrying ONLY the tiled handle (single zeroed), as extraction wires it.
    ECS::World world;
    GameEngine::TerrainECS::RegisterTerrainWorldHooks(world); // as EnsurePrimaryWorld does
    const ECS::EntityHandle e = AddTiledTerrainEntity(world, tiled.Index, tiled.Generation);

    const auto* schema = GameEngine::Scene::SceneSchemaRegistry::Find("Terrain");
    ASSERT_NE(schema, nullptr) << "Terrain scene schema not registered";
    EXPECT_TRUE(schema->Remove(world, e));

    EXPECT_EQ(TerrainService::Get().GetTiledTerrainData(tiled), nullptr)
        << "tiled terrain data leaked after component removal";
    EXPECT_EQ(world.GetComponent<Components::Terrain>(e), nullptr);
}

// ---------------------------------------------------------------------------
// Tiled streaming (E6): ConsumeUnifiedDirtyRect — the tiled analogue of
// ConsumeDirtyRegion. A tiled terrain's edits (tile streaming + modifier re-bakes) land
// in the unified GPU texture, which TerrainRenderFeature accumulates into a UV dirty rect;
// CBTUpdateSystem drains it here with the SAME confirm / re-arm discipline as the planar
// log path so a rect drained on a non-recording frame is re-armed, not dropped. Drives the
// discipline directly (no device / RenderServices), like the ConsumeSphereSculpt tests.
// ---------------------------------------------------------------------------

// No upload since the last drain -> the rect stays empty (streaming quiescence: a parked
// camera over settled tiles must publish nothing so Classify stays idle-zero-work).
TEST(CBTRegionConsume, UnifiedDirtyQuiescentUntilUpload)
{
    CBTUpdateSystem system(nullptr);
    CBTTerrain::CBTClassifyDesc c{};
    system.ConsumeUnifiedDirtyRect(/*takenHasRect=*/false, 0, 0, 0, 0, /*recordCount=*/0u, c);
    EXPECT_TRUE(RectIsEmpty(c));
}

// A drained upload rect reaches the classify.
TEST(CBTRegionConsume, UnifiedDirtyUploadPushesRect)
{
    CBTUpdateSystem system(nullptr);
    CBTTerrain::CBTClassifyDesc c{};
    system.ConsumeUnifiedDirtyRect(true, 0.25f, 0.30f, 0.55f, 0.60f, 0u, c);
    EXPECT_FALSE(RectIsEmpty(c));
    EXPECT_NEAR(c.DirtyMinU, 0.25f, 1e-4f);
    EXPECT_NEAR(c.DirtyMinV, 0.30f, 1e-4f);
    EXPECT_NEAR(c.DirtyMaxU, 0.55f, 1e-4f);
    EXPECT_NEAR(c.DirtyMaxV, 0.60f, 1e-4f);
}

// An unconfirmed streamed rect (record count never advances) is RE-ARMED, not dropped —
// the same no-scene-view guard the planar + sphere paths carry.
TEST(CBTRegionConsume, UnifiedDirtyUnconfirmedReArmed)
{
    CBTUpdateSystem system(nullptr);
    CBTTerrain::CBTClassifyDesc a{};
    system.ConsumeUnifiedDirtyRect(true, 0.1f, 0.1f, 0.4f, 0.4f, 0u, a);
    ASSERT_FALSE(RectIsEmpty(a));
    // Next frame: no NEW upload (takenHasRect=false) + same record count => re-arm the union.
    CBTTerrain::CBTClassifyDesc b{};
    system.ConsumeUnifiedDirtyRect(false, 0, 0, 0, 0, 0u, b);
    EXPECT_FALSE(RectIsEmpty(b)) << "an unconfirmed streamed rect was dropped instead of re-armed";
    EXPECT_NEAR(b.DirtyMinU, 0.1f, 1e-4f);
    EXPECT_NEAR(b.DirtyMaxU, 0.4f, 1e-4f);
}

// Once confirmed (record count advanced), a held rect goes quiescent (empty); a NEW upload
// re-arms again.
TEST(CBTRegionConsume, UnifiedDirtyConfirmedQuiescentThenReArms)
{
    CBTUpdateSystem system(nullptr);
    CBTTerrain::CBTClassifyDesc push{};
    system.ConsumeUnifiedDirtyRect(true, 0.2f, 0.2f, 0.5f, 0.5f, 0u, push);
    ASSERT_FALSE(RectIsEmpty(push));

    // Record count advanced -> the pushed rect was recorded -> commit; no new upload.
    CBTTerrain::CBTClassifyDesc quiescent{};
    system.ConsumeUnifiedDirtyRect(false, 0, 0, 0, 0, 1u, quiescent);
    EXPECT_TRUE(RectIsEmpty(quiescent)) << "confirmed streamed rect still re-armed";

    // A new upload (still record count 1) reclassifies again.
    CBTTerrain::CBTClassifyDesc reupload{};
    system.ConsumeUnifiedDirtyRect(true, 0.6f, 0.6f, 0.8f, 0.8f, 1u, reupload);
    EXPECT_FALSE(RectIsEmpty(reupload)) << "a new upload after a committed one was not reclassified";
    EXPECT_NEAR(reupload.DirtyMinU, 0.6f, 1e-4f);
}

// Multiple uploads before a confirm union into their bounding rect (a multi-tile streaming
// frame, or several unconfirmed frames, must reclassify the whole touched footprint).
TEST(CBTRegionConsume, UnifiedDirtyUnionsAcrossUnconfirmedFrames)
{
    CBTUpdateSystem system(nullptr);
    CBTTerrain::CBTClassifyDesc a{};
    system.ConsumeUnifiedDirtyRect(true, 0.1f, 0.1f, 0.3f, 0.3f, 0u, a);
    CBTTerrain::CBTClassifyDesc b{};
    system.ConsumeUnifiedDirtyRect(true, 0.5f, 0.5f, 0.7f, 0.7f, 0u, b); // still unconfirmed
    EXPECT_NEAR(b.DirtyMinU, 0.1f, 1e-4f);
    EXPECT_NEAR(b.DirtyMinV, 0.1f, 1e-4f);
    EXPECT_NEAR(b.DirtyMaxU, 0.7f, 1e-4f);
    EXPECT_NEAR(b.DirtyMaxV, 0.7f, 1e-4f);
}

// ---------------------------------------------------------------------------
// Extraction math (E6): TerrainRenderFeature's unified-height dirty accumulator maps a
// patched texel sub-rect to the correct UV AABB and unions across uploads. Device-backed
// (needs the unified textures); skips when no headless Vulkan device is available.
// ---------------------------------------------------------------------------
TEST(CBTRegionConsume, UnifiedHeightDirtyRectExtractionMath)
{
    auto device = MakeHeadlessDeviceOrNull();
    if (!device)
        GTEST_SKIP() << "no headless Vulkan device";

    {
        TerrainRenderFeature feature;
        ASSERT_TRUE(feature.Initialize(device.get()));

        const TerrainHandle handle{0u, 1u};
        constexpr uint32 kW = 128u;
        constexpr uint32 kH = 128u;
        feature.EnsureUnifiedTiledTextures(handle, kW, kH);

        constexpr float32 kTexel = 1.0f / 128.0f;
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
        uint64 ver = 0;
        // Nothing uploaded yet -> empty.
        EXPECT_FALSE(feature.PeekUnifiedHeightDirtyRect(handle, u0, v0, u1, v1, ver));

        // Interior 32x32 block at texel origin (32,64). The raw texel rect is [32,64..64,96];
        // the accumulator pads ONE texel each side (bilinear footprint), so the UV AABB is
        // exactly [31,63 .. 65,97] texels wide.
        const std::vector<float32> block(32u * 32u, 0.5f);
        feature.UploadHeightmapRegion(handle, block.data(), 32u, 32u, 32u, 64u);
        ASSERT_TRUE(feature.PeekUnifiedHeightDirtyRect(handle, u0, v0, u1, v1, ver));
        EXPECT_NEAR(u0, 31.0f * kTexel, 1e-5f) << "min U not padded one texel";
        EXPECT_NEAR(v0, 63.0f * kTexel, 1e-5f) << "min V not padded one texel";
        EXPECT_NEAR(u1, 65.0f * kTexel, 1e-5f) << "max U not padded one texel";
        EXPECT_NEAR(v1, 97.0f * kTexel, 1e-5f) << "max V not padded one texel";
        EXPECT_EQ(ver, 1u);

        // A second upload at texel origin (0,0): its padded min would go below 0 and must
        // CLAMP to 0 (not wrap negative); the union preserves the interior upload's max.
        feature.UploadHeightmapRegion(handle, block.data(), 32u, 32u, 0u, 0u);
        ASSERT_TRUE(feature.PeekUnifiedHeightDirtyRect(handle, u0, v0, u1, v1, ver));
        EXPECT_NEAR(u0, 0.0f, 1e-5f) << "edge-touching min U did not clamp to 0";
        EXPECT_NEAR(v0, 0.0f, 1e-5f) << "edge-touching min V did not clamp to 0";
        EXPECT_NEAR(u1, 65.0f * kTexel, 1e-5f);
        EXPECT_NEAR(v1, 97.0f * kTexel, 1e-5f);
        EXPECT_EQ(ver, 2u);

        // Take drains it -> a subsequent peek is empty; version stays monotonic (always
        // written, per the Take/Peek contract — even on the false return below).
        float t0 = 0, t1 = 0, t2 = 0, t3 = 0;
        uint64 tver = 0;
        ASSERT_TRUE(feature.TakeUnifiedHeightDirtyRect(handle, t0, t1, t2, t3, tver));
        EXPECT_EQ(tver, 2u);
        float d0 = 0, d1 = 0, d2 = 0, d3 = 0;
        uint64 dver = 0;
        EXPECT_FALSE(feature.PeekUnifiedHeightDirtyRect(handle, d0, d1, d2, d3, dver));
        EXPECT_EQ(dver, 2u) << "version must stay monotonic across a drain";

        // A block flush against the far edge (dstX+srcW == W): the padded max must CLAMP to
        // 1.0, and the accumulator is empty post-drain so this rect stands alone.
        feature.UploadHeightmapRegion(handle, block.data(), 32u, 32u, 96u, 96u);
        ASSERT_TRUE(feature.PeekUnifiedHeightDirtyRect(handle, u0, v0, u1, v1, ver));
        EXPECT_NEAR(u0, 95.0f * kTexel, 1e-5f);
        EXPECT_NEAR(v0, 95.0f * kTexel, 1e-5f);
        EXPECT_NEAR(u1, 1.0f, 1e-5f) << "edge-touching max U did not clamp to 1";
        EXPECT_NEAR(v1, 1.0f, 1e-5f) << "edge-touching max V did not clamp to 1";
        EXPECT_EQ(ver, 3u);

        feature.ReleaseTerrainResources(handle);
    } // feature destructs (destroys its textures) while the device is still alive

    device->Shutdown();
}

// Routing (E6, C1-mirage guard): ConsumeUnifiedTiledDirtyRegion must resolve the tiled
// terrain's TiledTerrainHandle -> GlobalGpuHandle (via TerrainService) and drain THAT
// handle's accumulator into the classify. The global handle is deliberately a DISTINCT slot
// from the tiled handle, so a resolution that used the tiled handle directly (the exact
// handle-disambiguation class that left a feature inert for months) would drain an empty
// slot and leave the rect empty -> this fails. Device-backed (needs the unified textures).
TEST(CBTRegionConsume, UnifiedTiledDirtyRoutingResolvesGlobalHandleAndLandsRect)
{
    auto device = MakeHeadlessDeviceOrNull();
    if (!device)
        GTEST_SKIP() << "no headless Vulkan device";

    ScopedTerrainService svc;
    {
        TerrainRenderFeature feature;
        ASSERT_TRUE(feature.Initialize(device.get()));

        TiledTerrainConfig cfg{};
        cfg.WorldSizeX = 4096.0f;
        cfg.WorldSizeZ = 4096.0f;
        cfg.HeightScale = 200.0f;
        cfg.SamplesPerMeter = 1.0f;
        cfg.PatchGridSize = 32;
        const TiledTerrainHandle tiled = TerrainService::Get().CreateTiledTerrain(cfg);
        TiledTerrainData* tiledData = TerrainService::Get().GetTiledTerrainData(tiled);
        ASSERT_NE(tiledData, nullptr);

        // Global GPU handle on a DISTINCT slot from the tiled handle (as extraction assigns
        // one via CreateTerrain). Seed the accumulator ONLY for this global handle.
        const TerrainHandle globalHandle{tiled.Index + 7u, 1u};
        tiledData->GlobalGpuHandleIndex = globalHandle.Index;
        tiledData->GlobalGpuHandleGeneration = globalHandle.Generation;

        constexpr uint32 kW = 128u;
        constexpr uint32 kH = 128u;
        feature.EnsureUnifiedTiledTextures(globalHandle, kW, kH);
        const std::vector<float32> block(32u * 32u, 0.5f);
        feature.UploadHeightmapRegion(globalHandle, block.data(), 32u, 32u, 32u, 64u);

        Components::Terrain tuning{};
        tuning.TiledTerrainHandle = tiled.Index;
        tuning.TiledTerrainGeneration = tiled.Generation;

        CBTUpdateSystem system(nullptr);
        CBTTerrain::CBTClassifyDesc classify{};
        system.ConsumeUnifiedTiledDirtyRegion(&feature, /*updateRecordCount=*/0u, tuning, classify);
        EXPECT_FALSE(RectIsEmpty(classify))
            << "tiled routing did not resolve the global handle / drain the rect";
        constexpr float32 kTexel = 1.0f / 128.0f;
        EXPECT_NEAR(classify.DirtyMinU, 31.0f * kTexel, 1e-4f);
        EXPECT_NEAR(classify.DirtyMaxV, 97.0f * kTexel, 1e-4f);

        feature.ReleaseTerrainResources(globalHandle);
    }
    device->Shutdown();
}

// Defensive routing: a null feature or a tiled handle with no live TiledTerrainData drains
// nothing and leaves the rect empty (no crash) — the discipline still runs so a prior
// pending rect could re-arm / commit.
TEST(CBTRegionConsume, UnifiedTiledDirtyNullFeatureOrMissingDataIsEmptyRect)
{
    ScopedTerrainService svc;
    CBTUpdateSystem system(nullptr);

    Components::Terrain tuning{};
    tuning.TiledTerrainHandle = 99u; // no such tiled data
    tuning.TiledTerrainGeneration = 1u;

    CBTTerrain::CBTClassifyDesc nullFeature{};
    system.ConsumeUnifiedTiledDirtyRegion(nullptr, 0u, tuning, nullFeature);
    EXPECT_TRUE(RectIsEmpty(nullFeature));
}

// ---------------------------------------------------------------------------
// Schedule ordering: CBTUpdate must run STRICTLY BEFORE TerrainModifiers.
// ---------------------------------------------------------------------------

// CBTUpdateSystem owns the sphere sculpt store's IDENTITY transitions —
// TerrainService::ResetPlanetSculpt when the active planet entity changes, and
// EnsureSphereSculptLoaded for a planet carrying a saved .tsculpt
// (CBTUpdateSystem.cpp, the currentPlanet block). TerrainModifierSystem owns the
// store's CONTENT — ConfigurePlanetSculpt + BakePlanetModifierRegions
// (TerrainModifierSystem::BakeSphereModifiers).
//
// Identity must settle before content is baked into it. Reversed, a planet swap
// bakes the new planet's modifiers into the old planet's grid and the reset then
// drops the whole store (SphereSculptLayer::Reset drops content AND geometry) —
// and the modifier change gate keys on a hash that did not move, so the lost bake
// is never re-run. Same-wave is equally wrong: waves dispatch onto the JobSystem,
// so reset and bake would interleave nondeterministically.
//
// This case covers the end-to-end order across all three modules, so it also
// catches an inversion introduced through some other module's edges. It cannot
// tell a declared edge from an accidental one — DeclaredEdgeOrdersTerrainModifiers
// below is the one that isolates the edge itself.
TEST(CBTUpdateSchedule, CBTUpdateIsScheduledBeforeTerrainModifiers)
{
    ECS::SystemScheduleBuilder builder;
    SplineECS::AddSplineSystemsToSchedule(builder);
    TerrainECS::AddTerrainSystemsToSchedule(builder, nullptr);
    CBTTerrainECS::AddCBTSystemsToSchedule(builder, nullptr);

    ECS::SystemManager manager;
    // Partial: TransformHierarchy / Camera / PhysicsWorldBootstrap belong to
    // modules this fixture does not build, so their edges are legitimately
    // unresolved here.
    builder.BuildAndRegisterWithWaves(manager, ECS::ScheduleCompleteness::Partial);

    const auto& plan = manager.GetExecutionPlan();
    ASSERT_TRUE(plan.IsValid());

    int cbtWave = -1;
    int modifierWave = -1;
    for (std::size_t w = 0; w < plan.Waves.size(); ++w)
    {
        for (std::size_t idx : plan.Waves[w].SystemIndices)
        {
            const char* name = manager.GetSequentialSystemName(idx);
            if (!name)
                continue;
            if (std::string_view(name) == "CBTUpdate")
                cbtWave = static_cast<int>(w);
            else if (std::string_view(name) == "TerrainModifiers")
                modifierWave = static_cast<int>(w);
        }
    }

    ASSERT_GE(cbtWave, 0) << "CBTUpdate is not in the execution plan";
    ASSERT_GE(modifierWave, 0) << "TerrainModifiers is not in the execution plan";
    // Reported so a failure shows the actual waves, not just the comparison.
    EXPECT_LT(cbtWave, modifierWave)
        << "CBTUpdate wave " << cbtWave << ", TerrainModifiers wave " << modifierWave
        << " — the sculpt-store identity reset/load must complete before the modifier "
           "bake, and same-wave systems execute in parallel";
}

// The declared edge in isolation. Only TerrainECS and CBTTerrainECS contribute, so
// TerrainModifiers' other dependencies (TransformHierarchy, SplineExtraction) are
// deliberately unregistered and their edges inactive — the ONLY thing that can keep
// TerrainModifiers out of CBTUpdate's wave is the declared "CBTUpdate" edge itself.
// Strip that edge and both land in wave 0, where they would execute in parallel and
// race the sculpt-store reset against the modifier bake; this test fails there,
// where the end-to-end case above would still pass on the strength of the spline
// and transform edges.
TEST(CBTUpdateSchedule, DeclaredEdgeOrdersTerrainModifiersAfterCBTUpdate)
{
    ECS::SystemScheduleBuilder builder;
    TerrainECS::AddTerrainSystemsToSchedule(builder, nullptr);
    CBTTerrainECS::AddCBTSystemsToSchedule(builder, nullptr);

    ECS::SystemManager manager;
    builder.BuildAndRegisterWithWaves(manager, ECS::ScheduleCompleteness::Partial);

    const auto& plan = manager.GetExecutionPlan();
    ASSERT_TRUE(plan.IsValid());

    int cbtWave = -1;
    int modifierWave = -1;
    for (std::size_t w = 0; w < plan.Waves.size(); ++w)
    {
        for (std::size_t idx : plan.Waves[w].SystemIndices)
        {
            const char* name = manager.GetSequentialSystemName(idx);
            if (!name)
                continue;
            if (std::string_view(name) == "CBTUpdate")
                cbtWave = static_cast<int>(w);
            else if (std::string_view(name) == "TerrainModifiers")
                modifierWave = static_cast<int>(w);
        }
    }

    ASSERT_GE(cbtWave, 0) << "CBTUpdate is not in the execution plan";
    ASSERT_GE(modifierWave, 0) << "TerrainModifiers is not in the execution plan";
    EXPECT_LT(cbtWave, modifierWave)
        << "CBTUpdate wave " << cbtWave << ", TerrainModifiers wave " << modifierWave
        << " — TerrainModifiers does not declare a dependency on CBTUpdate, so the "
           "sculpt-store identity reset/load is no longer ordered before the bake";
}

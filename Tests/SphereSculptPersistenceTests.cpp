// Sphere sculpt persistence at the SERVICE level (save/load slice): the .tsculpt blob
// round-trips through TerrainService — restore is an EDIT (version + both dirty unions), the
// saved-grid restore composes with the #632 radius-remap path (save at R=2000, load into a
// R=20000 planet), the #630 grid-keyed undo entries survive a load, and the needs-save
// lifecycle drives the editor's dirty prompt. Engine-linked like CBTRegionConsumeTests.

#include <gtest/gtest.h>

#include "CBTTerrain/SphereSculptPaging.h" // DeriveSculptVirtualDim
#include "TerrainECS/TerrainService.h"

#include <cstring>
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

// The dab used across these oracles: +X-dominant direction, 10 m peak.
constexpr float kDabDirX = 1.0f, kDabDirY = 0.1f, kDabDirZ = 0.1f;
constexpr float kDabAngularRadius = 0.03f;
constexpr float kDabStrength = 10.0f;

float SampleAtDab(const TerrainService& svc)
{
    return svc.SampleSphereSculptHeight(kDabDirX, kDabDirY, kDabDirZ, 0.0f);
}
} // namespace

// Save -> reset -> load at the SAME radius: the published heights come back byte-identical,
// the load advances the version and feeds BOTH dirty unions (render re-tess + per-face
// physics) — a load IS an edit, so the restored planet re-tessellates without camera motion.
TEST(SphereSculptPersistence, SaveLoadRoundTripSameRadius)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    svc.ConfigurePlanetSculpt(2000.0f);
    svc.ApplySphereSculptDab(kDabDirX, kDabDirY, kDabDirZ, kDabAngularRadius, kDabStrength, false);
    const float authored = SampleAtDab(svc);
    ASSERT_GT(authored, 0.0f);

    uint64 savedVersion = 0;
    const std::vector<uint8> blob = svc.EncodeSphereSculptBlob(savedVersion);
    ASSERT_FALSE(blob.empty());
    EXPECT_EQ(savedVersion, svc.SphereSculptVersion());

    // The scene-swap sequence: identity reset drops the store, then the load restores it.
    svc.ResetPlanetSculpt();
    ASSERT_EQ(SampleAtDab(svc), 0.0f);
    ASSERT_FALSE(svc.HasSphereSculptEdits());

    ASSERT_TRUE(svc.RestoreSphereSculptFromBlob(blob.data(), blob.size(), GUID{}));
    EXPECT_TRUE(svc.HasSphereSculptEdits()) << "a restore must re-arm the GPU sample gate";
    EXPECT_EQ(svc.GetPlanetSculptGeometry().VirtualDim,
              CBTTerrain::DeriveSculptVirtualDim(2000.0f))
        << "the restore must adopt the SAVED grid";

    const float restored = SampleAtDab(svc);
    EXPECT_EQ(0, std::memcmp(&authored, &restored, sizeof(float)))
        << "same-radius load must republish byte-identical heights (authored " << authored
        << " vs restored " << restored << ")";

    // Load is an edit: both consumers see the restored footprint without any dab.
    CBTTerrain::SphereEditRegions drained{};
    EXPECT_TRUE(svc.ConsumeSphereSculptDirtyRegions(drained))
        << "the restore must feed the render dirty union (re-tess without camera motion)";
    EXPECT_TRUE(svc.GetPlanetSculptMirror().Faces[0].Touched)
        << "the restore must feed the physics per-face union (collider refresh)";

    // A fresh load carries no unsaved work; the next stroke re-arms the save prompt.
    EXPECT_FALSE(svc.SphereSculptNeedsSave());
    svc.ApplySphereSculptDab(kDabDirX, kDabDirY, kDabDirZ, kDabAngularRadius, kDabStrength, false);
    EXPECT_TRUE(svc.SphereSculptNeedsSave());
}

// Save at R=2000, load into a planet whose live radius derives a DIFFERENT grid (R=20000):
// the restore adopts the saved grid, then the normal per-frame ConfigurePlanetSculpt remaps
// the content via the #632 path — the content keeps its ANGULAR position and its metre
// amplitudes, and the remap feeds the dirty unions. No third restore path exists.
TEST(SphereSculptPersistence, RadiusChangedLoadRemapsViaExistingConfigurePath)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    svc.ConfigurePlanetSculpt(2000.0f);
    svc.ApplySphereSculptDab(kDabDirX, kDabDirY, kDabDirZ, kDabAngularRadius, kDabStrength, false);
    const float authored = SampleAtDab(svc);
    ASSERT_GT(authored, kDabStrength * 0.7f);

    uint64 savedVersion = 0;
    const std::vector<uint8> blob = svc.EncodeSphereSculptBlob(savedVersion);
    svc.ResetPlanetSculpt();
    ASSERT_TRUE(svc.RestoreSphereSculptFromBlob(blob.data(), blob.size(), GUID{}));
    EXPECT_EQ(svc.GetPlanetSculptGeometry().VirtualDim, CBTTerrain::DeriveSculptVirtualDim(2000.0f));
    const uint64 versionAfterLoad = svc.SphereSculptVersion();

    // The live component's radius derive (CBTRenderFeature / modifier bake per-frame call).
    svc.ConfigurePlanetSculpt(20000.0f);
    EXPECT_EQ(svc.GetPlanetSculptGeometry().VirtualDim,
              CBTTerrain::DeriveSculptVirtualDim(20000.0f))
        << "the live radius must win after the saved-grid restore";
    EXPECT_EQ(svc.SphereSculptVersion(), versionAfterLoad + 1u) << "the remap IS an edit";

    // Angular-position oracle (#632 pattern): the mound is still at the dab direction with its
    // metre amplitude (bilinear resample onto the finer grid — small smoothing tolerated), and
    // an antipodal direction stays untouched.
    const float remapped = SampleAtDab(svc);
    EXPECT_GT(remapped, authored * 0.8f) << "content lost amplitude across the radius-changed load";
    EXPECT_LT(remapped, authored * 1.2f) << "content gained amplitude across the radius-changed load";
    EXPECT_EQ(svc.SampleSphereSculptHeight(-1.0f, -0.1f, -0.1f, 0.0f), 0.0f)
        << "content leaked to the antipode — angular position not preserved";

    // The remap feeds the render union so the resized content re-tessellates.
    CBTTerrain::SphereEditRegions drained{};
    EXPECT_TRUE(svc.ConsumeSphereSculptDirtyRegions(drained));
}

// #630 composition: undo entries captured BEFORE a save survive a save -> reset -> load cycle
// on the same grid (the restored store is the same page grid, so the grid-keyed restore
// applies), and are refused per page after a radius-changed load remapped the grid — stale
// bytes are never misplaced onto a different angular rect.
TEST(SphereSculptPersistence, UndoEntriesComposeAcrossLoadAndRemap)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    svc.ConfigurePlanetSculpt(2000.0f);
    svc.BeginSphereSculptStrokeCapture();
    svc.ApplySphereSculptDab(kDabDirX, kDabDirY, kDabDirZ, kDabAngularRadius, kDabStrength, false);
    const std::vector<CBTTerrain::SphereSculptPageState> preImages =
        svc.TakeSphereSculptStrokeCapture();
    ASSERT_FALSE(preImages.empty());
    const std::vector<CBTTerrain::SphereSculptPageState> postImages =
        svc.SnapshotSphereSculptPages(preImages);
    const float authored = SampleAtDab(svc);
    ASSERT_GT(authored, 0.0f);

    uint64 savedVersion = 0;
    const std::vector<uint8> blob = svc.EncodeSphereSculptBlob(savedVersion);

    // Same-grid load: the pre-images apply — undoing the stroke on the restored store works.
    svc.ResetPlanetSculpt();
    ASSERT_TRUE(svc.RestoreSphereSculptFromBlob(blob.data(), blob.size(), GUID{}));
    svc.RestoreSphereSculptPages(preImages);
    EXPECT_EQ(SampleAtDab(svc), 0.0f) << "undo after a same-grid load must revert the stroke";
    svc.RestoreSphereSculptPages(postImages);
    const float redone = SampleAtDab(svc);
    EXPECT_EQ(0, std::memcmp(&authored, &redone, sizeof(float)))
        << "redo after a same-grid load must reproduce the stroke exactly";

    // Radius-changed load: the remap re-grids the store; the old-grid entries are refused per
    // page (heights unchanged), never misplaced.
    svc.ResetPlanetSculpt();
    ASSERT_TRUE(svc.RestoreSphereSculptFromBlob(blob.data(), blob.size(), GUID{}));
    svc.ConfigurePlanetSculpt(20000.0f);
    const float remapped = SampleAtDab(svc);
    ASSERT_GT(remapped, 0.0f);
    svc.RestoreSphereSculptPages(preImages);
    EXPECT_EQ(SampleAtDab(svc), remapped)
        << "old-grid undo entries must be refused after a radius-changed load";
}

// The needs-save lifecycle behind the editor's dirty prompt: quiescent -> false, stroke ->
// true, saved -> false, stroke again -> true, planet reset -> false (content gone, nothing
// to save). Keyed on DAB mutations only: modifier bakes (every scene provision runs a full
// one) and radius remaps advance the sculpt version but re-derive/remap on the next load, so
// they must NOT arm the prompt — the phantom-dirty regression an untouched loaded planet hit
// when needs-save compared versions.
TEST(SphereSculptPersistence, NeedsSaveLifecycle)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    svc.ConfigurePlanetSculpt(2000.0f);
    EXPECT_FALSE(svc.SphereSculptNeedsSave());

    // The provision-time modifier FULL bake advances the version even with no modifiers;
    // an untouched planet must still read clean.
    const uint64 preBake = svc.SphereSculptVersion();
    svc.BakePlanetModifierRegions(
        CBTTerrain::SphereEditRegions{}, [](float32, float32, float32) { return 0.0f; },
        /*fullBake=*/true);
    EXPECT_GT(svc.SphereSculptVersion(), preBake) << "full bake should advance the version";
    EXPECT_FALSE(svc.SphereSculptNeedsSave()) << "a re-derivable bake is not unsaved stroke work";

    svc.ApplySphereSculptDab(kDabDirX, kDabDirY, kDabDirZ, kDabAngularRadius, kDabStrength, false);
    EXPECT_TRUE(svc.SphereSculptNeedsSave());

    uint64 savedVersion = 0;
    const std::vector<uint8> blob = svc.EncodeSphereSculptBlob(savedVersion);
    ASSERT_FALSE(blob.empty());
    svc.MarkSphereSculptSaved(GUID{}, savedVersion);
    EXPECT_FALSE(svc.SphereSculptNeedsSave());

    // A radius remap moves content between grids (version advances) but loses nothing a
    // load could not reconstruct from the file + live radius — still clean.
    svc.ConfigurePlanetSculpt(20000.0f);
    EXPECT_FALSE(svc.SphereSculptNeedsSave()) << "a radius remap is not unsaved stroke work";

    svc.ApplySphereSculptDab(kDabDirX, kDabDirY, kDabDirZ, kDabAngularRadius, kDabStrength, true);
    EXPECT_TRUE(svc.SphereSculptNeedsSave());

    svc.ResetPlanetSculpt();
    EXPECT_FALSE(svc.SphereSculptNeedsSave());
}

// A corrupt blob is refused wholesale: the store keeps its current content and geometry, no
// partial import, and the call reports failure so the loader can warn.
TEST(SphereSculptPersistence, RestoreRejectsCorruptBlob)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    svc.ConfigurePlanetSculpt(2000.0f);
    svc.ApplySphereSculptDab(kDabDirX, kDabDirY, kDabDirZ, kDabAngularRadius, kDabStrength, false);
    const float authored = SampleAtDab(svc);
    const uint64 version = svc.SphereSculptVersion();

    const std::vector<uint8> garbage(64, 0xAB);
    EXPECT_FALSE(svc.RestoreSphereSculptFromBlob(garbage.data(), garbage.size(), GUID{}));
    EXPECT_EQ(svc.SphereSculptVersion(), version) << "a refused restore must not be an edit";
    EXPECT_EQ(SampleAtDab(svc), authored) << "a refused restore must not touch the content";
}

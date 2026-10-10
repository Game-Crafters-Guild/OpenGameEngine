#include <gtest/gtest.h>

#include "Scene/SceneBackupRecovery.h"
#include "Terrain/SphereSculptSaver.h"
#include "Terrain/TerrainZonePayloadSaver.h"

#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "CBTTerrain/SphereSculptSerialization.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "TerrainECS/TerrainService.h"

#include "TestTempDir.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace
{
using namespace GameEngine;
using GameEngine::ECS::World;
using GameEngine::TerrainECS::TerrainService;

// FlushDirtySphereSculpt mirrors the zone flush contract on the planet's .tsculpt sidecar:
// explicit Save mints `<sceneStem>_Zones/<guid>.tsculpt` + swaps the Terrain component's
// sphereSculpt ref to the file-backed GUID and clears the needs-save state; the autosave
// backup stages an EXISTING file's `.backup` sibling only (no mint, no ref-swap, no live
// write, needs-save preserved); writes are atomic so an interrupted save cannot corrupt the
// last good file; the recovery scan surfaces staged .tsculpt.backup siblings.
class SceneBackupSphereSculptFlushTests : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
    }

    void SetUp() override
    {
        // A fresh service per test: the sphere sculpt store is service-global state.
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
        TerrainService::Initialize();
        m_Dir = TestUtils::MakeUniqueTempDirectory("GameEngine_SceneBackupSphereSculptTests");
        std::error_code ec;
        std::filesystem::create_directories(m_Dir, ec);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_Dir, ec);
    }

    // An enabled spherical planet entity + one authored sculpt dab (needs-save set).
    ECS::EntityHandle MakeSculptedPlanet(World& world)
    {
        ECS::EntityHandle e = world.CreateEntity();
        Components::Transform xf{};
        Components::Terrain terrain{};
        terrain.Domain = Components::TerrainDomain::Spherical;
        terrain.PlanetRadius = 2000.0f;
        // FindActiveTerrainEntity (the saver's resolver) requires a provisioned handle;
        // fake the generation exactly like CBTRegionConsumeTests' AddTerrainEntity does.
        terrain.TerrainDataGeneration = 1;
        world.AddComponentImmediate(e, xf);
        world.AddComponentImmediate(e, terrain);

        auto& svc = TerrainService::Get();
        svc.ConfigurePlanetSculpt(2000.0f);
        svc.ApplySphereSculptDab(1.0f, 0.1f, 0.1f, 0.03f, 10.0f, false);
        EXPECT_TRUE(svc.SphereSculptNeedsSave());
        return e;
    }

    std::filesystem::path ZonesDir(const std::filesystem::path& scenePath) const
    {
        return Editor::SceneZonesFolderFor(scenePath);
    }

    std::filesystem::path m_Dir;
};

// Explicit Save mints the .tsculpt, swaps the component ref to the file-backed GUID, clears
// needs-save, and quiesces (a re-run writes nothing). The written blob decodes.
TEST_F(SceneBackupSphereSculptFlushTests, SaveMintsSwapsAndQuiesces)
{
    auto& svc = TerrainService::Get();
    auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
    World world;
    const ECS::EntityHandle planet = MakeSculptedPlanet(world);
    const std::filesystem::path scenePath = m_Dir / "mint.scene";

    EXPECT_EQ(Editor::FlushDirtySphereSculpt(world, scenePath, registry, svc,
                                             Editor::ZonePayloadFlushMode::SaveMintAndSwap),
              1u);
    EXPECT_FALSE(svc.SphereSculptNeedsSave()) << "an explicit save owns the needs-save clear";

    const auto* c = world.GetComponent<Components::Terrain>(planet);
    ASSERT_NE(c, nullptr);
    const GUID fileGuid = c->SphereSculptGuid.ToGuid();
    EXPECT_FALSE(fileGuid.IsNull()) << "save must swap the component ref to the file-backed GUID";

    AssetMetadata meta{};
    ASSERT_TRUE(registry.TryGetAssetMetadata(fileGuid, meta));
    ASSERT_TRUE(std::filesystem::exists(meta.Path));
    EXPECT_EQ(meta.Path.extension().string(), ".tsculpt");
    // The registry canonicalizes paths (lowercase, forward slashes) — compare identity, not text.
    std::error_code eqEc;
    EXPECT_TRUE(std::filesystem::equivalent(meta.Path.parent_path(), ZonesDir(scenePath), eqEc))
        << "sidecar lives in the zones folder (" << meta.Path.parent_path() << ")";

    std::ifstream in(meta.Path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    CBTTerrain::SphereSculptGeometry geom{};
    std::vector<CBTTerrain::SphereSculptPageContent> pages;
    ASSERT_TRUE(CBTTerrain::DecodeSphereSculpt(bytes.data(), bytes.size(), geom, pages));
    EXPECT_EQ(geom.VirtualDim, CBTTerrain::DeriveSculptVirtualDim(2000.0f));
    EXPECT_GT(pages.size(), 0u);

    // Quiescence: nothing new to save -> a re-run writes nothing.
    EXPECT_EQ(Editor::FlushDirtySphereSculpt(world, scenePath, registry, svc,
                                             Editor::ZonePayloadFlushMode::SaveMintAndSwap),
              0u);
}

// The autosave timer stages backup-side ONLY for an already-minted file: an unsaved sculpt is
// skipped (no mint, no ref-swap from the timer), a minted one gets a `.backup` sibling while
// the live file keeps its bytes and needs-save stays set. The ledger dedupes idle intervals.
TEST_F(SceneBackupSphereSculptFlushTests, BackupStagesExistingOnlyAndDedupes)
{
    auto& svc = TerrainService::Get();
    auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
    World world;
    const ECS::EntityHandle planet = MakeSculptedPlanet(world);
    const std::filesystem::path scenePath = m_Dir / "backup.scene";
    Editor::ZonePayloadStagingLedger ledger;

    // Unsaved sculpt: the timer must not mint.
    EXPECT_EQ(Editor::FlushDirtySphereSculpt(world, scenePath, registry, svc,
                                             Editor::ZonePayloadFlushMode::BackupExistingOnly,
                                             &ledger),
              0u);
    EXPECT_TRUE(svc.SphereSculptNeedsSave());
    EXPECT_TRUE(world.GetComponent<Components::Terrain>(planet)->SphereSculptGuid.IsNull())
        << "the timer must never ref-swap a live component";

    // Mint via explicit Save, stroke again, then stage.
    ASSERT_EQ(Editor::FlushDirtySphereSculpt(world, scenePath, registry, svc,
                                             Editor::ZonePayloadFlushMode::SaveMintAndSwap),
              1u);
    svc.ApplySphereSculptDab(1.0f, -0.1f, -0.1f, 0.03f, 6.0f, false);
    ASSERT_TRUE(svc.SphereSculptNeedsSave());

    EXPECT_EQ(Editor::FlushDirtySphereSculpt(world, scenePath, registry, svc,
                                             Editor::ZonePayloadFlushMode::BackupExistingOnly,
                                             &ledger),
              1u);
    AssetMetadata meta{};
    ASSERT_TRUE(registry.TryGetAssetMetadata(
        world.GetComponent<Components::Terrain>(planet)->SphereSculptGuid.ToGuid(), meta));
    const std::filesystem::path staged = Editor::StagedZonePayloadPathFor(meta.Path);
    EXPECT_TRUE(std::filesystem::exists(staged)) << "backup stages a .backup sibling";
    EXPECT_TRUE(svc.SphereSculptNeedsSave())
        << "staging must not clear needs-save: the next explicit Save owns the live write";

    // Idle interval: same sculpt version -> the ledger dedupes, nothing re-staged.
    EXPECT_EQ(Editor::FlushDirtySphereSculpt(world, scenePath, registry, svc,
                                             Editor::ZonePayloadFlushMode::BackupExistingOnly,
                                             &ledger),
              0u);

    // The recovery scan surfaces the staged sculpt exactly like staged zones. The registry
    // canonicalizes paths, so match by filesystem identity rather than textual equality.
    const Editor::SceneRecoveryScan scan = Editor::ScanForSceneRecovery(scenePath);
    const bool found =
        std::any_of(scan.StagedPayloads.begin(), scan.StagedPayloads.end(),
                    [&](const std::filesystem::path& p) {
                        std::error_code ec;
                        return std::filesystem::equivalent(p, staged, ec);
                    });
    EXPECT_TRUE(found) << "ScanForSceneRecovery must pick up .tsculpt.backup siblings";
}

// Gate 5 (atomic save): an interrupted write — temp bytes written, rename never ran — leaves
// the committed .tsculpt intact; and a write whose temp path cannot even open fails cleanly
// without touching the target.
TEST_F(SceneBackupSphereSculptFlushTests, InterruptedSaveLeavesPriorFileIntact)
{
    auto& svc = TerrainService::Get();
    auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
    World world;
    MakeSculptedPlanet(world);
    const std::filesystem::path scenePath = m_Dir / "interrupt.scene";

    ASSERT_EQ(Editor::FlushDirtySphereSculpt(world, scenePath, registry, svc,
                                             Editor::ZonePayloadFlushMode::SaveMintAndSwap),
              1u);
    // Locate the minted file.
    std::filesystem::path live;
    for (const auto& entry : std::filesystem::directory_iterator(ZonesDir(scenePath)))
        if (entry.path().extension() == ".tsculpt")
            live = entry.path();
    ASSERT_FALSE(live.empty());
    std::ifstream goodIn(live, std::ios::binary);
    const std::vector<uint8_t> goodBytes((std::istreambuf_iterator<char>(goodIn)),
                                         std::istreambuf_iterator<char>());
    goodIn.close();
    ASSERT_FALSE(goodBytes.empty());

    // Crash simulation: a garbage temp exists but the rename never happened.
    {
        std::ofstream tmp(live.string() + ".tmp", std::ios::binary | std::ios::trunc);
        const char garbage[] = {0x00, 0x01, 0x02, 0x03};
        tmp.write(garbage, sizeof(garbage));
    }
    std::ifstream stillIn(live, std::ios::binary);
    const std::vector<uint8_t> stillBytes((std::istreambuf_iterator<char>(stillIn)),
                                          std::istreambuf_iterator<char>());
    stillIn.close();
    EXPECT_EQ(stillBytes, goodBytes) << "an interrupted write corrupted the committed file";

    // A write whose temp cannot open (the .tmp path is a DIRECTORY) fails without
    // touching the target — the failure path of the atomic contract.
    std::error_code ec;
    std::filesystem::remove(live.string() + ".tmp", ec);
    std::filesystem::create_directories(live.string() + ".tmp", ec);
    EXPECT_FALSE(Editor::WriteBlobAtomic(live, {0x11, 0x22}));
    std::ifstream afterIn(live, std::ios::binary);
    const std::vector<uint8_t> afterBytes((std::istreambuf_iterator<char>(afterIn)),
                                          std::istreambuf_iterator<char>());
    EXPECT_EQ(afterBytes, goodBytes) << "a failed write touched the committed file";
}

// No planet / planar-only world / clean store: the flush is a no-op in every mode.
TEST_F(SceneBackupSphereSculptFlushTests, NoOpWithoutSphericalPlanetOrEdits)
{
    auto& svc = TerrainService::Get();
    auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
    const std::filesystem::path scenePath = m_Dir / "noop.scene";

    // Clean store, planet present.
    World world;
    ECS::EntityHandle e = world.CreateEntity();
    Components::Transform xf{};
    Components::Terrain planar{};
    world.AddComponentImmediate(e, xf);
    world.AddComponentImmediate(e, planar);
    EXPECT_EQ(Editor::FlushDirtySphereSculpt(world, scenePath, registry, svc,
                                             Editor::ZonePayloadFlushMode::SaveMintAndSwap),
              0u);

    // Dirty store but no spherical planet in the world (orphaned edits): no owner, no write.
    svc.ConfigurePlanetSculpt(2000.0f);
    svc.ApplySphereSculptDab(1.0f, 0.0f, 0.0f, 0.03f, 5.0f, false);
    ASSERT_TRUE(svc.SphereSculptNeedsSave());
    EXPECT_EQ(Editor::FlushDirtySphereSculpt(world, scenePath, registry, svc,
                                             Editor::ZonePayloadFlushMode::SaveMintAndSwap),
              0u);
    EXPECT_TRUE(std::filesystem::exists(ZonesDir(scenePath)) == false ||
                std::filesystem::is_empty(ZonesDir(scenePath)));
}
} // namespace

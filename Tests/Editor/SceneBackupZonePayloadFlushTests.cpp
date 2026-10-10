#include <gtest/gtest.h>

#include "Scene/SceneBackupRecovery.h"
#include "Scene/SceneDocumentManager.h"
#include "Terrain/TerrainZonePayloadSaver.h"

#include "AssetCore/Asset.h"
#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Components/Terrain/TerrainModifiers.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/TerrainZonePayload.h"

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
using GameEngine::TerrainECS::TerrainZonePayload;
using GameEngine::TerrainECS::ZonePayloadFormat;

// The autosave backup path (SceneDocumentManager::SaveBackupCopy) stages dirty
// zone payloads BACKUP-SIDE (`<live>.tzone.backup` siblings) — never the live
// .tzone, which only explicit Save writes. These tests lock that contract plus
// the #510 invariants that carry over: atomic temp+rename writes, and no
// mint/ref-swap of an unsaved zone from the timer (F3).
class SceneBackupZonePayloadFlushTests : public ::testing::Test
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
        if (!TerrainService::IsInitialized())
            TerrainService::Initialize();
        m_Dir = TestUtils::MakeUniqueTempDirectory("GameEngine_SceneBackupZonePayloadFlushTests");
        std::error_code ec;
        std::filesystem::create_directories(m_Dir, ec);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_Dir, ec);
    }

    // A sculpt-zone entity + a resident payload filled to `fill`, marked dirty.
    ECS::EntityHandle MakeDirtyZone(World& world, const GUID& guid, float32 fill)
    {
        ECS::EntityHandle e = world.CreateEntity();
        Components::Transform xf{};
        Components::TerrainSculptZone z{};
        z.ExtentX = 16.0f;
        z.ExtentZ = 16.0f;
        z.PayloadRef.Set(guid);
        world.AddComponentImmediate(e, xf);
        world.AddComponentImmediate(e, z);

        TerrainZonePayload p;
        p.Allocate(ZonePayloadFormat::SculptOffsetR32F, 8, 8);
        std::fill(p.Offsets.begin(), p.Offsets.end(), fill);
        TerrainService::Get().SetZonePayload(guid, std::move(p)); // sets NeedsSave
        return e;
    }

    // Make `guid` resolve to an existing .tzone under the scene's companion folder,
    // without going through Save (so the backup path sees it as file-backed).
    std::filesystem::path RegisterFileBackedZone(const GUID& guid, const std::string& sceneStem)
    {
        const std::filesystem::path zonesDir = m_Dir / (sceneStem + "_Zones");
        std::error_code ec;
        std::filesystem::create_directories(zonesDir, ec);
        const std::filesystem::path tzone = zonesDir / (guid.ToString() + ".tzone");
        AssetMetadata meta{};
        meta.Guid = guid;
        meta.Path = tzone;
        EngineCore::GetInstance().GetAssetManager().GetRegistry().RegisterAssetMetadata(meta);
        return tzone;
    }

    std::filesystem::path m_Dir;
};

// Option B core: a backup STAGES a dirty file-backed payload to a `.backup`
// sibling — the live .tzone path is untouched — and leaves an unsaved zone
// (no .tzone yet) alone: no mint, no ref-swap, no staged file (F3). NeedsSave
// stays set on the staged zone: the live bytes are still unwritten and the
// next explicit Save must persist them.
TEST_F(SceneBackupZonePayloadFlushTests, BackupStagesFileBackedButSkipsUnsavedZone)
{
    auto& svc = TerrainService::Get();
    World world;

    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);
    const std::filesystem::path scenePath = m_Dir / "backup.scene";
    ASSERT_TRUE(doc.SaveAs(scenePath)); // establishes the scene path (empty world)

    // Zone A is already file-backed; zone B was created since the last Save.
    const GUID gA = GUID::Generate();
    const GUID gB = GUID::Generate();
    const std::filesystem::path aTzone = RegisterFileBackedZone(gA, scenePath.stem().string());
    MakeDirtyZone(world, gA, 1.0f);
    MakeDirtyZone(world, gB, 2.0f);
    ASSERT_TRUE(svc.GetZonePayload(gA)->NeedsSave);
    ASSERT_TRUE(svc.GetZonePayload(gB)->NeedsSave);

    ASSERT_TRUE(doc.SaveBackupCopy());

    const std::filesystem::path aStaged = Editor::StagedZonePayloadPathFor(aTzone);
    EXPECT_TRUE(std::filesystem::exists(aStaged)) << "backup stages the file-backed zone backup-side";
    EXPECT_FALSE(std::filesystem::exists(aTzone))
        << "the live .tzone is written ONLY by explicit Save — the timer must not touch it";
    EXPECT_TRUE(svc.GetZonePayload(gA)->NeedsSave)
        << "staging must not clear NeedsSave: the next explicit Save still owns the live write";
    EXPECT_TRUE(svc.GetZonePayload(gB)->NeedsSave) << "backup must NOT mint an unsaved zone (F3)";
    const std::filesystem::path bTzone =
        m_Dir / (scenePath.stem().string() + "_Zones") / (gB.ToString() + ".tzone");
    EXPECT_FALSE(std::filesystem::exists(bTzone)) << "no orphan .tzone minted from the timer";
    EXPECT_FALSE(std::filesystem::exists(Editor::StagedZonePayloadPathFor(bTzone)))
        << "no staged sibling for an unsaved zone either";
}

// F3 at the saver: BackupExistingOnly skips an unsaved zone; SaveMintAndSwap mints it.
// Quiescence: once flushed (clean), a re-run writes nothing.
TEST_F(SceneBackupZonePayloadFlushTests, SaveMintsUnsavedZoneBackupSkipsAndBothQuiesce)
{
    auto& svc = TerrainService::Get();
    auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
    World world;
    const GUID guid = GUID::Generate();
    MakeDirtyZone(world, guid, 3.0f);

    const std::filesystem::path scenePath = m_Dir / "mint.scene";
    Editor::ZonePayloadStagingLedger ledger;

    // The backup mode never mints an unsaved zone.
    EXPECT_EQ(Editor::FlushDirtyZonePayloads(world, scenePath, registry, svc,
                                             Editor::ZonePayloadFlushMode::BackupExistingOnly,
                                             &ledger),
              0u)
        << "backup skips a zone with no file-backed ref";
    EXPECT_TRUE(svc.GetZonePayload(guid)->NeedsSave);

    // Save mints it and clears NeedsSave (F6: on this path the ref may swap to the
    // file-derived GUID — the quiescence below re-queries the world either way).
    EXPECT_EQ(Editor::FlushDirtyZonePayloads(world, scenePath, registry, svc,
                                             Editor::ZonePayloadFlushMode::SaveMintAndSwap),
              1u)
        << "save mints the .tzone";

    // No strokes since -> a re-run writes nothing.
    EXPECT_EQ(Editor::FlushDirtyZonePayloads(world, scenePath, registry, svc,
                                             Editor::ZonePayloadFlushMode::SaveMintAndSwap),
              0u)
        << "quiescent: no stroke -> no write";
}

// F1: an interrupted write (temp written, rename never happened) leaves the last good
// .tzone intact. The atomic temp+rename guarantees the committed file is never the one
// being truncated, so a hard-kill mid-write cannot zero the last save.
TEST_F(SceneBackupZonePayloadFlushTests, InterruptedWritePreservesLastGoodBytes)
{
    auto& svc = TerrainService::Get();
    auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
    World world;
    const GUID guid = GUID::Generate();
    const std::filesystem::path tzone = RegisterFileBackedZone(guid, "interrupt");

    // Write the known-good payload (fill 5.0) through the atomic live-write path
    // (file-backed, so Save overwrites in place without minting).
    MakeDirtyZone(world, guid, 5.0f);
    ASSERT_EQ(Editor::FlushDirtyZonePayloads(world, m_Dir / "interrupt.scene", registry, svc,
                                             Editor::ZonePayloadFlushMode::SaveMintAndSwap),
              1u);
    ASSERT_TRUE(std::filesystem::exists(tzone));

    // Simulate a crash mid-write: a temp exists with garbage, but the rename never ran.
    {
        std::ofstream tmp(tzone.string() + ".tmp", std::ios::binary | std::ios::trunc);
        const char garbage[] = {0x00, 0x01, 0x02, 0x03};
        tmp.write(garbage, sizeof(garbage));
    }

    // The committed file still decodes to the last good payload.
    std::ifstream in(tzone, std::ios::binary);
    std::vector<uint8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    TerrainZonePayload decoded;
    ASSERT_TRUE(TerrainECS::DecodeZonePayload(bytes.data(), bytes.size(), decoded));
    ASSERT_FALSE(decoded.Offsets.empty());
    EXPECT_FLOAT_EQ(decoded.Offsets[0], 5.0f) << "interrupted write did not corrupt the last save";
}
} // namespace

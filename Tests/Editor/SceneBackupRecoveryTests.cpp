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
#include "TerrainECS/Scene/TerrainSceneSchemas.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/TerrainZonePayload.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace
{
using namespace GameEngine;
using GameEngine::ECS::World;
using GameEngine::TerrainECS::TerrainService;
using GameEngine::TerrainECS::TerrainZonePayload;
using GameEngine::TerrainECS::ZonePayloadFormat;

std::vector<char> ReadAllBytes(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A scratch root under %TEMP% that belongs to THIS PROCESS. A fixed name is
// shared by every EditorTests process on the machine, and SetUp's remove_all
// then deletes a concurrent run's scene out from under it: the loser fails
// once and passes on every retry, which is what this suite was caught doing.
//
// The process id still repeats — it is recycled, and a crashed run leaves a
// directory no TearDown removed — so SetUp keeps clearing the root before it
// uses it.
std::filesystem::path ProcessScratchRoot()
{
#if defined(_WIN32)
    const auto pid = static_cast<unsigned long>(_getpid());
#else
    const auto pid = static_cast<unsigned long>(getpid());
#endif
    return std::filesystem::temp_directory_path() /
           ("GameEngine_SceneBackupRecoveryTests_" + std::to_string(pid));
}

// Option B crash-recovery flow (terrain arc): the autosave timer stages dirty
// zone payloads to `.tzone.backup` siblings and writes `<stem>.backup.scene`
// only when there is unsaved work; explicit Save alone writes live files. A
// crash leaves the staging behind; ScanForSceneRecovery detects it and
// ResolveRecoveryDecision promotes (restore) or deletes (discard) it. These
// tests drive the whole decision path headless — no UI, no editor process.
class SceneBackupRecoveryTests : public ::testing::Test
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
        Scene::EnsureTerrainSceneSchemasRegistered();
    }

    void SetUp() override
    {
        if (!TerrainService::IsInitialized())
            TerrainService::Initialize();
        m_Dir = ProcessScratchRoot();
        std::error_code ec;
        std::filesystem::remove_all(m_Dir, ec);
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

    static GUID CurrentPayloadGuid(World& world, ECS::EntityHandle e)
    {
        if (const auto* z = world.GetComponent<Components::TerrainSculptZone>(e))
            return z->PayloadRef.ToGuid();
        return GUID::Null();
    }

    // Simulate a brush stroke on a resident payload: new offsets + the edit
    // notification that bumps DataVersion and re-flags NeedsSave.
    static void Stroke(const GUID& guid, float32 fill)
    {
        TerrainZonePayload* p = TerrainService::Get().GetZonePayload(guid);
        ASSERT_NE(p, nullptr);
        std::fill(p->Offsets.begin(), p->Offsets.end(), fill);
        TerrainService::Get().NotifyZonePayloadEdited(
            guid, 0, 0, static_cast<int32>(p->Width), static_cast<int32>(p->Height));
    }

    // Editing session that crashed mid-flight:
    //   Save (fill 1.0, live .tzone minted) -> stroke (fill 2.0) -> autosave
    //   tick (stages backup-side) -> hard kill (nothing cleaned up).
    // The scene file is back-dated so the backup-vs-scene mtime comparison is
    // deterministic regardless of filesystem timestamp granularity.
    struct CrashState
    {
        World world; // must outlive doc
        Editor::SceneDocumentManager doc;
        std::filesystem::path scenePath;
        std::filesystem::path liveTzone;
        std::filesystem::path stagedTzone;
        GUID payloadGuid;
        std::vector<char> liveBytesAtSave;   // fill 1.0 — what Save persisted
        std::vector<char> stagedBytesAtCrash; // fill 2.0 — what the autosave staged
    };

    void MakeCrashState(CrashState& s, const std::string& sceneName)
    {
        s.doc.SetWorld(&s.world);
        const GUID authored = GUID::Generate();
        const ECS::EntityHandle zone = MakeDirtyZone(s.world, authored, 1.0f);

        s.scenePath = m_Dir / (sceneName + ".scene");
        ASSERT_TRUE(s.doc.SaveAs(s.scenePath)); // mints the live .tzone, clears NeedsSave

        s.payloadGuid = CurrentPayloadGuid(s.world, zone); // may have swapped to the file GUID
        ASSERT_FALSE(s.payloadGuid.IsNull());

        // The minted live file is the single .tzone in the scene's zones folder.
        const std::filesystem::path zonesDir = Editor::SceneZonesFolderFor(s.scenePath);
        for (const auto& entry : std::filesystem::directory_iterator(zonesDir))
        {
            if (entry.path().extension() == ".tzone")
                s.liveTzone = entry.path();
        }
        ASSERT_FALSE(s.liveTzone.empty());
        ASSERT_TRUE(std::filesystem::exists(s.liveTzone));

        // The staging path resolves the zone through registry metadata; make the
        // GUID -> live-file mapping explicit so the fixture doesn't depend on
        // what the mint path registered in this shared-engine test process.
        auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
        AssetMetadata meta{};
        if (!registry.TryGetAssetMetadata(s.payloadGuid, meta) || meta.Path.empty())
        {
            meta.Guid = s.payloadGuid;
            meta.Path = s.liveTzone;
            registry.RegisterAssetMetadata(meta);
        }
        s.liveBytesAtSave = ReadAllBytes(s.liveTzone);
        ASSERT_FALSE(s.liveBytesAtSave.empty());

        std::filesystem::last_write_time(
            s.scenePath, std::filesystem::file_time_type::clock::now() - std::chrono::hours(1));

        Stroke(s.payloadGuid, 2.0f);
        ASSERT_TRUE(s.doc.SaveBackupCopy()); // the autosave tick

        s.stagedTzone = Editor::StagedZonePayloadPathFor(s.liveTzone);
        ASSERT_TRUE(std::filesystem::exists(s.stagedTzone));
        s.stagedBytesAtCrash = ReadAllBytes(s.stagedTzone);
        ASSERT_FALSE(s.stagedBytesAtCrash.empty());
        // ...and the process dies here: no clean-close cleanup runs.
    }

    std::filesystem::path m_Dir;
};

// Staging never touches the live .tzone: byte-identical before/after the
// autosave tick. (Pre-Option-B this fails — the timer overwrote the live file.)
TEST_F(SceneBackupRecoveryTests, StagingLeavesLiveTzoneBytesUntouched)
{
    CrashState s;
    MakeCrashState(s, "staging_live_untouched");

    EXPECT_EQ(ReadAllBytes(s.liveTzone), s.liveBytesAtSave)
        << "autosave staging must not rewrite the live .tzone";
    EXPECT_NE(s.stagedBytesAtCrash, s.liveBytesAtSave)
        << "sanity: the staged bytes actually differ (the stroke changed the payload)";
    EXPECT_TRUE(TerrainService::Get().GetZonePayload(s.payloadGuid)->NeedsSave)
        << "staging must not clear NeedsSave — explicit Save still owns the live write";
}

// The staging ledger quiesces the timer: identical payload -> no re-stage; a
// new stroke -> re-stage.
TEST_F(SceneBackupRecoveryTests, StagingQuiescesUntilNextStroke)
{
    CrashState s;
    MakeCrashState(s, "staging_quiesce");
    auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
    auto& svc = TerrainService::Get();

    Editor::ZonePayloadStagingLedger ledger;
    EXPECT_EQ(Editor::FlushDirtyZonePayloads(s.world, s.scenePath, registry, svc,
                                             Editor::ZonePayloadFlushMode::BackupExistingOnly,
                                             &ledger),
              1u)
        << "fresh ledger: the dirty zone stages once";
    EXPECT_EQ(Editor::FlushDirtyZonePayloads(s.world, s.scenePath, registry, svc,
                                             Editor::ZonePayloadFlushMode::BackupExistingOnly,
                                             &ledger),
              0u)
        << "no stroke since -> idle tick re-stages nothing";

    Stroke(s.payloadGuid, 3.0f);
    EXPECT_EQ(Editor::FlushDirtyZonePayloads(s.world, s.scenePath, registry, svc,
                                             Editor::ZonePayloadFlushMode::BackupExistingOnly,
                                             &ledger),
              1u)
        << "a new stroke bumps DataVersion -> re-stages";
}

// Explicit Save writes the live file and deletes the staged sibling: a stale
// staged file left behind would be offered as "newer" recovery after a crash
// that happened AFTER the save.
TEST_F(SceneBackupRecoveryTests, ExplicitSaveWritesLiveAndRemovesStagedSibling)
{
    CrashState s;
    MakeCrashState(s, "save_supersedes_staging");

    ASSERT_TRUE(s.doc.Save());

    EXPECT_FALSE(std::filesystem::exists(s.stagedTzone))
        << "Save must delete the staged sibling it supersedes";
    EXPECT_EQ(ReadAllBytes(s.liveTzone), s.stagedBytesAtCrash)
        << "the live file now holds the stroked payload (same bytes staging captured)";
    EXPECT_FALSE(TerrainService::Get().GetZonePayload(s.payloadGuid)->NeedsSave);
}

// After a crash, the scan sees recovery pending: the staged payload is listed
// and the backup scene (newer than the scene) is usable.
TEST_F(SceneBackupRecoveryTests, ScanDetectsRecoveryPendingAfterCrash)
{
    CrashState s;
    MakeCrashState(s, "scan_detects");

    const Editor::SceneRecoveryScan scan = Editor::ScanForSceneRecovery(s.scenePath);
    EXPECT_TRUE(scan.RecoveryPending());
    EXPECT_TRUE(scan.BackupSceneUsable) << "the backup scene is newer than the scene";
    ASSERT_EQ(scan.StagedPayloads.size(), 1u);
    EXPECT_EQ(scan.StagedPayloads[0], s.stagedTzone);
}

// A backup scene OLDER than the scene (pre-dating the last explicit Save) is
// stale and never offered.
TEST_F(SceneBackupRecoveryTests, ScanIgnoresBackupSceneOlderThanScene)
{
    CrashState s;
    MakeCrashState(s, "scan_stale");

    // Save: supersedes the staging and makes the scene newer than the backup.
    ASSERT_TRUE(s.doc.Save());

    const Editor::SceneRecoveryScan scan = Editor::ScanForSceneRecovery(s.scenePath);
    EXPECT_FALSE(scan.BackupSceneUsable) << "backup older than the scene is stale";
    EXPECT_TRUE(scan.StagedPayloads.empty());
    EXPECT_FALSE(scan.RecoveryPending());
}

// RESTORE: promotion is an atomic rename — the live .tzone becomes byte-identical
// to what was staged, the staged file is consumed, and the open is pointed at the
// backup scene content under the real scene's identity.
TEST_F(SceneBackupRecoveryTests, RestorePromotesStagedBytesAndOpensBackup)
{
    CrashState s;
    MakeCrashState(s, "restore");

    const Editor::SceneRecoveryScan scan = Editor::ScanForSceneRecovery(s.scenePath);
    ASSERT_TRUE(scan.RecoveryPending());

    const Editor::SceneRecoveryResolution r = Editor::ResolveRecoveryDecision(scan, /*restore*/ true);

    EXPECT_EQ(ReadAllBytes(s.liveTzone), s.stagedBytesAtCrash)
        << "promotion must land the exact staged bytes on the live path";
    EXPECT_FALSE(std::filesystem::exists(s.stagedTzone)) << "the staged file was consumed";
    EXPECT_TRUE(r.OpenedFromBackup);
    EXPECT_EQ(r.ContentPathToOpen, scan.BackupScenePath);
    EXPECT_EQ(r.DocumentPath, s.scenePath);
    EXPECT_TRUE(std::filesystem::exists(scan.BackupScenePath))
        << "the backup scene stays until the recovered session closes cleanly";
}

// DECLINE: all staging is deleted, the live .tzone keeps the last-saved bytes,
// and the open is pointed at the last saved scene.
TEST_F(SceneBackupRecoveryTests, DiscardDeletesStagingAndKeepsLastSavedBytes)
{
    CrashState s;
    MakeCrashState(s, "discard");

    const Editor::SceneRecoveryScan scan = Editor::ScanForSceneRecovery(s.scenePath);
    ASSERT_TRUE(scan.RecoveryPending());

    const Editor::SceneRecoveryResolution r = Editor::ResolveRecoveryDecision(scan, /*restore*/ false);

    EXPECT_EQ(ReadAllBytes(s.liveTzone), s.liveBytesAtSave)
        << "decline must leave the last explicit Save untouched";
    EXPECT_FALSE(std::filesystem::exists(s.stagedTzone));
    EXPECT_FALSE(std::filesystem::exists(scan.BackupScenePath));
    EXPECT_FALSE(r.OpenedFromBackup);
    EXPECT_EQ(r.ContentPathToOpen, s.scenePath);
    EXPECT_FALSE(Editor::ScanForSceneRecovery(s.scenePath).RecoveryPending())
        << "no prompt on the next open";
}

// Clean close (what SceneEditorController::CleanupBackupsForCleanClose runs on
// scene switch and teardown): no backup litter survives — no staged payloads,
// no leftover atomic-write temps, no backup scene — so the next open of this
// scene sees no recovery prompt.
TEST_F(SceneBackupRecoveryTests, CleanCloseLeavesNoBackupLitter)
{
    CrashState s;
    MakeCrashState(s, "clean_close");

    // A crashed staging write can also leave an orphan temp beside the staged file.
    {
        std::ofstream tmp(s.stagedTzone.string() + ".tmp", std::ios::binary | std::ios::trunc);
        tmp << "garbage";
    }

    Editor::DiscardSceneBackups(Editor::ScanForSceneRecovery(s.scenePath));

    EXPECT_FALSE(std::filesystem::exists(s.stagedTzone));
    EXPECT_FALSE(std::filesystem::exists(s.stagedTzone.string() + ".tmp"));
    EXPECT_FALSE(std::filesystem::exists(Editor::SceneBackupPathFor(s.scenePath)));
    EXPECT_TRUE(std::filesystem::exists(s.liveTzone)) << "the live payload is never touched";
    EXPECT_FALSE(Editor::ScanForSceneRecovery(s.scenePath).RecoveryPending());
}

// "Don't Save" then reopen must actually discard: a replace-open clears the
// resident zone payload store. A leaked resident payload would keep NeedsSave
// forever (phantom-dirty document) and the next explicit Save would silently
// persist the strokes the user chose to discard.
TEST_F(SceneBackupRecoveryTests, ReplaceOpenDropsResidentPayloadEdits)
{
    World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);
    const std::filesystem::path scenePath = m_Dir / "reopen.scene";
    ASSERT_TRUE(doc.SaveAs(scenePath));

    const GUID guid = GUID::Generate();
    MakeDirtyZone(world, guid, 4.0f); // the "discarded" stroke: resident + NeedsSave
    ASSERT_TRUE(TerrainService::Get().AnyZonePayloadNeedsSave());

    ASSERT_TRUE(doc.OpenSceneReplace(scenePath));

    EXPECT_FALSE(TerrainService::Get().AnyZonePayloadNeedsSave())
        << "a replace-open must not leak the previous session's payload edits";
    EXPECT_EQ(TerrainService::Get().GetZonePayload(guid), nullptr)
        << "the discarded payload must not stay resident";
}

// After an explicit Save, an idle autosave tick writes NOTHING — no backup
// scene, no staged payloads. Without this gate every clean-session crash would
// leave a "newer" backup scene behind and prompt spurious recovery.
TEST_F(SceneBackupRecoveryTests, IdleAutosaveAfterSaveWritesNoBackup)
{
    World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);
    const GUID authored = GUID::Generate();
    MakeDirtyZone(world, authored, 1.0f);
    const std::filesystem::path scenePath = m_Dir / "idle.scene";
    ASSERT_TRUE(doc.SaveAs(scenePath));

    EXPECT_FALSE(doc.SaveBackupCopy()) << "nothing unsaved -> the tick is a no-op";
    EXPECT_FALSE(std::filesystem::exists(Editor::SceneBackupPathFor(scenePath)));
    EXPECT_FALSE(Editor::ScanForSceneRecovery(scenePath).RecoveryPending());
}
} // namespace

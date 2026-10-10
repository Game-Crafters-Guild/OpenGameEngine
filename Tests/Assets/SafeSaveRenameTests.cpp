#include <gtest/gtest.h>

#include "AssetCore/SharedFileRead.h"
#include "Assets/AssetManager.h"
#include "FileWatcher/FileWatcher.h"
#include "Assets/FileWatchingService.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestTempDir.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef PLATFORM_WINDOWS
// Declared directly instead of via <windows.h>: that header's global GUID
// typedef is ambiguous against GameEngine::GUID under the using-directive
// below. Signatures match kernel32 exactly (BOOL=int, DWORD=unsigned long).
extern "C" __declspec(dllimport) int __stdcall ReplaceFileW(const wchar_t* replacedFileName,
                                                            const wchar_t* replacementFileName,
                                                            const wchar_t* backupFileName,
                                                            unsigned long replaceFlags,
                                                            void* exclude,
                                                            void* reserved);
extern "C" __declspec(dllimport) unsigned long __stdcall GetLastError(void);
#endif

using namespace GameEngine;

namespace
{
void WriteBinaryFile(const std::filesystem::path& p, const std::string& content)
{
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(f.is_open()) << "Failed to open: " << p.string();
    f << content;
}

// The default deadline is generous relative to the mechanism, not tuned to it:
// a watcher-driven registration owes FileWatcher::DEBOUNCE_DELAY (100ms), plus
// kDeletedEventHold (200ms) on the delete leg, plus service dispatch, the scan
// handoff and the registry writer lock — all of which stretch under machine
// load. 10s is ~30x those holds and matches the deadlines the raw-watcher test
// below already uses.
bool WaitFor(const std::function<bool()>& pred,
             std::chrono::milliseconds timeout = std::chrono::seconds(10),
             std::chrono::milliseconds step = std::chrono::milliseconds(25))
{
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout)
    {
        if (pred())
            return true;
        std::this_thread::sleep_for(step);
    }
    return pred();
}
} // namespace

// Registry-level fixture: no file watching, events are simulated by calling the
// same registry APIs the watcher path (AssetManager::HandleFileChange) calls.
class SafeSaveRenameRegistryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        root = TestUtils::MakeUniqueTempDirectory("safe_save_rename_registry_test");
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);

        jobSystem = std::make_unique<JobSystem::WorkStealingThreadPool>(2);
        assetManager = std::make_unique<AssetManager>();
        ASSERT_TRUE(assetManager->Initialize(root, jobSystem.get()));
    }

    void TearDown() override
    {
        if (assetManager)
        {
            assetManager->Shutdown();
            assetManager.reset();
        }
        jobSystem.reset();
        std::filesystem::remove_all(root);
    }

    std::filesystem::path root;
    std::unique_ptr<JobSystem::WorkStealingThreadPool> jobSystem;
    std::unique_ptr<AssetManager> assetManager;
};

// Safe-save (write temp, rename temp over target) repeated across cycles.
// Regression: the rename used to rebind the temp entry — whose GUID derives
// from the temp path — onto the target path, erasing the target's identity.
// The next cycle's temp registration then derived the same GUID, hit the
// derive-collision guard, and (in Debug) aborted on the watcher thread while
// m_RegistryMutex was held, freezing the editor.
TEST_F(SafeSaveRenameRegistryTest, RenameOverRegisteredTargetKeepsTargetIdentity)
{
    auto& reg = assetManager->GetRegistry();

    const auto target = root / "textures" / "swap.png";
    const auto temp = root / "textures" / "swap_new.png";

    WriteBinaryFile(target, "v0");
    ASSERT_TRUE(reg.RegisterAsset(target));
    const GUID targetGuid = reg.GetAssetGUID(target);
    ASSERT_FALSE(targetGuid.IsNull());

    GUID tempGuid = GUID::Null();
    for (int cycle = 0; cycle < 3; ++cycle)
    {
        WriteBinaryFile(temp, "v" + std::to_string(cycle + 1));

        // Cycle 2+ used to refuse this registration (derive collision) and
        // abort in Debug builds.
        ASSERT_TRUE(reg.RegisterAsset(temp)) << "temp registration refused on cycle " << cycle;
        const GUID g = reg.GetAssetGUID(temp);
        ASSERT_FALSE(g.IsNull()) << "cycle " << cycle;
        if (cycle == 0)
            tempGuid = g;
        else
            EXPECT_EQ(g, tempGuid) << "temp path must derive the same GUID every cycle";

        std::error_code ec;
        std::filesystem::rename(temp, target, ec);
        ASSERT_FALSE(ec) << "disk rename failed on cycle " << cycle << ": " << ec.message();

        // What the watcher's Renamed event handling calls.
        ASSERT_TRUE(reg.TryRenameAssetPath(temp, target)) << "cycle " << cycle;

        EXPECT_EQ(reg.GetAssetGUID(target), targetGuid)
            << "target identity must be stable across safe-save cycle " << cycle;
        EXPECT_TRUE(reg.GetAssetGUID(temp).IsNull())
            << "temp entry must be unregistered after rename-away, cycle " << cycle;

        AssetMetadata targetMd{};
        ASSERT_TRUE(reg.TryGetAssetMetadata(targetGuid, targetMd)) << "cycle " << cycle;
        EXPECT_EQ(targetMd.Path.filename(), target.filename()) << "cycle " << cycle;

        AssetMetadata tempMd{};
        EXPECT_FALSE(reg.TryGetAssetMetadata(tempGuid, tempMd))
            << "temp metadata must not survive the rename-away, cycle " << cycle;
    }
}

// The derive-collision guard's rename-kept shape, at the store-less in-memory
// seam: after a live rename the riding GUID still derives from the OLD path,
// so re-registering that path collides. The guard heals — the renamed asset
// migrates onto the GUID its current path derives, the registrant takes the
// identity its own path derives — and must never abort: it runs on the
// file-watcher thread under the registry write lock (this fixture runs it in
// every build config, Debug included).
TEST_F(SafeSaveRenameRegistryTest, RegisteringARenamedAwayPathHealsTheKeptIdentity)
{
    auto& reg = assetManager->GetRegistry();

    const auto oldPath = root / "collide_a.png";
    const auto newPath = root / "collide_b.png";

    WriteBinaryFile(oldPath, "one");
    ASSERT_TRUE(reg.RegisterAsset(oldPath));
    const GUID ridingGuid = reg.GetAssetGUID(oldPath);
    ASSERT_FALSE(ridingGuid.IsNull());

    // Plain rename (destination unregistered): the GUID rides along by design,
    // leaving an entry whose GUID derives from a path it no longer occupies.
    std::error_code ec;
    std::filesystem::rename(oldPath, newPath, ec);
    ASSERT_FALSE(ec) << ec.message();
    ASSERT_TRUE(reg.TryRenameAssetPath(oldPath, newPath));
    ASSERT_EQ(reg.GetAssetGUID(newPath), ridingGuid);

    // Re-create a file at the old path: it derives ridingGuid again, which the
    // renamed asset is only keeping for session continuity — the registrant
    // reclaims it, the renamed asset moves to its own path-derived identity.
    WriteBinaryFile(oldPath, "two");
    EXPECT_TRUE(reg.RegisterAsset(oldPath));
    EXPECT_EQ(reg.GetAssetGUID(oldPath), ridingGuid);

    const GUID renamedGuid = reg.GetAssetGUID(newPath);
    ASSERT_FALSE(renamedGuid.IsNull());
    EXPECT_NE(renamedGuid, ridingGuid);

    // Both identities resolve to their own paths.
    AssetMetadata md{};
    ASSERT_TRUE(reg.TryGetAssetMetadata(ridingGuid, md));
    EXPECT_EQ(md.Path.filename(), oldPath.filename());
    ASSERT_TRUE(reg.TryGetAssetMetadata(renamedGuid, md));
    EXPECT_EQ(md.Path.filename(), newPath.filename());
}

// Store-backed registry fixture (authoritative DB + SQLite derived cache) —
// the editor's real shape. The 13b invariant reads the store's tombstones, so
// it needs this fixture; the store-less one above exercises pure in-memory
// registry behavior.
class SafeSaveRenameStoreBackedTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        root = TestUtils::MakeUniqueTempDirectory("safe_save_rename_store_test");
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);

        jobSystem = std::make_unique<JobSystem::WorkStealingThreadPool>(2);
        assetManager = std::make_unique<AssetManager>();
        ASSERT_TRUE(assetManager->Initialize(root, jobSystem.get(),
                                             root / "AssetDatabase.assetdb", root / ".Cache"));
    }

    void TearDown() override
    {
        if (assetManager)
        {
            assetManager->Shutdown();
            assetManager.reset();
        }
        jobSystem.reset();
        std::filesystem::remove_all(root);
    }

    std::filesystem::path root;
    std::unique_ptr<JobSystem::WorkStealingThreadPool> jobSystem;
    std::unique_ptr<AssetManager> assetManager;
};

// 13b pathological-lingering edge: the external app's replace fails, its temp
// file lives on, and the REAL file's Deleted event outlives the watcher hold —
// consuming the target's registration before the rename finally lands. The
// plain-rename branch used to let the temp-derived GUID ride onto the real
// path (fresh identity for the file, original GUID dangling). The store still
// owns the destination path's derived GUID, so the rename must restore it.
TEST_F(SafeSaveRenameStoreBackedTest, LingeringTempRenameAfterConsumedDeleteRestoresTargetIdentity)
{
    auto& reg = assetManager->GetRegistry();

    // The referrer uses an extension no parser owns so dependency extraction
    // takes the syntactic GUID-scanner path (parser-owned formats reject
    // content that isn't structurally valid for them).
    const auto target = root / "textures" / "hero.png";
    const auto temp = root / "textures" / "hero_new.png";
    const auto referrer = root / "notes" / "uses_hero.txt";

    WriteBinaryFile(target, "v0");
    ASSERT_TRUE(reg.RegisterAsset(target));
    const GUID targetGuid = reg.GetAssetGUID(target);
    ASSERT_FALSE(targetGuid.IsNull());

    // A live referrer keeps the target's store record as a tombstone across
    // the Deleted (aggressive cleanup removes unreferenced records outright,
    // and an unreferenced asset has nothing to dangle).
    WriteBinaryFile(referrer, "{ \"texture\": \"" + targetGuid.ToString() + "\" }");
    ASSERT_TRUE(reg.RegisterAsset(referrer));
    const GUID referrerGuid = reg.GetAssetGUID(referrer);
    ASSERT_FALSE(referrerGuid.IsNull());
    const Vector<GUID> deps = reg.RefreshDependencies(referrerGuid);
    ASSERT_NE(std::find(deps.begin(), deps.end(), targetGuid), deps.end())
        << "referrer must depend on the target for the tombstone to survive";

    GUID tempGuid = GUID::Null();
    for (int cycle = 0; cycle < 3; ++cycle)
    {
        // The app's failed replace leaves the temp behind; it registers like
        // any other created file (its GUID derives from the temp path).
        WriteBinaryFile(temp, "v" + std::to_string(cycle + 1));
        ASSERT_TRUE(reg.RegisterAsset(temp)) << "cycle " << cycle;
        const GUID g = reg.GetAssetGUID(temp);
        ASSERT_FALSE(g.IsNull()) << "cycle " << cycle;
        if (cycle == 0)
            tempGuid = g;
        else
            EXPECT_EQ(g, tempGuid) << "temp path must derive the same GUID every cycle";
        ASSERT_NE(tempGuid, targetGuid);

        // The real file's Deleted event fired past the hold window — what
        // AssetManager::HandleFileChange(Deleted) calls.
        std::error_code ec;
        std::filesystem::remove(target, ec);
        ASSERT_FALSE(ec) << ec.message();
        ASSERT_TRUE(reg.TryUnregisterAssetByPath(target)) << "cycle " << cycle;
        ASSERT_TRUE(reg.GetAssetGUID(target).IsNull());

        // Minutes later the replace finally lands.
        std::filesystem::rename(temp, target, ec);
        ASSERT_FALSE(ec) << "disk rename failed on cycle " << cycle << ": " << ec.message();
        ASSERT_TRUE(reg.TryRenameAssetPath(temp, target)) << "cycle " << cycle;

        EXPECT_EQ(reg.GetAssetGUID(target), targetGuid)
            << "destination identity must be restored, not migrated to the temp GUID, cycle "
            << cycle;
        EXPECT_TRUE(reg.GetAssetGUID(temp).IsNull())
            << "temp entry must be unregistered after rename-away, cycle " << cycle;

        AssetMetadata md{};
        ASSERT_TRUE(reg.TryGetAssetMetadata(targetGuid, md)) << "cycle " << cycle;
        EXPECT_EQ(md.Path.filename(), target.filename()) << "cycle " << cycle;
    }
}

// The restoring re-registration keeps the caller's spelling of the destination,
// so the restored asset is named as its file is, not from the case-folded key.
TEST_F(SafeSaveRenameStoreBackedTest, LingeringTempRenameRestoresTheTargetWithItsNameCase)
{
    auto& reg = assetManager->GetRegistry();
    const auto target = root / "Textures" / "Hero Texture.png";
    const auto temp = root / "Textures" / "Hero Texture_new.png";
    const auto referrer = root / "notes" / "uses_hero.txt";

    WriteBinaryFile(target, "v0");
    ASSERT_TRUE(reg.RegisterAsset(target));
    const GUID targetGuid = reg.GetAssetGUID(target);
    ASSERT_FALSE(targetGuid.IsNull());
    WriteBinaryFile(referrer, "{ \"texture\": \"" + targetGuid.ToString() + "\" }");
    ASSERT_TRUE(reg.RegisterAsset(referrer));
    const Vector<GUID> deps = reg.RefreshDependencies(reg.GetAssetGUID(referrer));
    ASSERT_NE(std::find(deps.begin(), deps.end(), targetGuid), deps.end());

    WriteBinaryFile(temp, "v1");
    ASSERT_TRUE(reg.RegisterAsset(temp));
    std::error_code ec;
    std::filesystem::remove(target, ec);
    ASSERT_FALSE(ec) << ec.message();
    ASSERT_TRUE(reg.TryUnregisterAssetByPath(target));
    std::filesystem::rename(temp, target, ec);
    ASSERT_FALSE(ec) << ec.message();
    ASSERT_TRUE(reg.TryRenameAssetPath(temp, target));

    ASSERT_EQ(reg.GetAssetGUID(target), targetGuid);
    AssetMetadata md{};
    ASSERT_TRUE(reg.TryGetAssetMetadata(targetGuid, md));
    EXPECT_EQ(md.Name, "Hero Texture");
}

// Windows reports a replace-rename (MoveFileEx REPLACE_EXISTING) as REMOVED
// for the clobbered destination followed by the RENAMED_OLD/NEW pair. The
// watcher must suppress that Deleted — the destination is getting new content,
// not disappearing — so the registry sees the rename while the destination is
// still registered and can keep its identity.
TEST(FileWatcherReplaceRenameTest, ReplaceRenameSuppressesTargetDeleted)
{
#ifdef PLATFORM_WINDOWS
    const auto dir = TestUtils::MakeUniqueTempDirectory("replace_rename_events_test");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    std::mutex m;
    std::vector<FileChangeEvent> events;
    FileWatcher watcher;
    watcher.SetCallback([&](const FileChangeEvent& ev)
                        {
                            std::lock_guard<std::mutex> lk(m);
                            events.push_back(ev);
                        });
    const auto sawEventFor = [&m, &events](const char* filename)
    {
        std::lock_guard<std::mutex> lk(m);
        return std::any_of(events.begin(), events.end(), [filename](const FileChangeEvent& ev)
                           { return ev.Path.filename() == filename; });
    };
    const auto sawTargetDeleted = [&m, &events]
    {
        std::lock_guard<std::mutex> lk(m);
        return std::any_of(events.begin(), events.end(), [](const FileChangeEvent& ev) {
            return ev.Type == FileChangeType::Deleted && ev.Path.filename() == "swap.png";
        });
    };
    const auto sawReplaceRename = [&m, &events]
    {
        std::lock_guard<std::mutex> lk(m);
        return std::any_of(events.begin(), events.end(), [](const FileChangeEvent& ev) {
            return ev.Type == FileChangeType::Renamed && ev.Path.filename() == "swap.png" &&
                   ev.OldPath.filename() == "swap_new.png";
        });
    };

    ASSERT_TRUE(watcher.StartWatching(dir, true));

    // StartWatching only spawns the watch thread; anything written before it
    // registers the directory handle is never reported. Rewrite a sentinel
    // until the watcher answers — a single write is itself lost by the race it
    // is probing, and nothing would ever re-trigger it.
    bool armed = false;
    const auto armDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!armed && std::chrono::steady_clock::now() < armDeadline)
    {
        WriteBinaryFile(dir / "armed_probe.bin", "probe");
        armed = WaitFor([&] { return sawEventFor("armed_probe.bin"); }, std::chrono::milliseconds(250));
    }
    ASSERT_TRUE(armed) << "watcher never reported the arming sentinel";

    // Each write is confirmed delivered before the next step, so the replace
    // is not racing an unreported create.
    WriteBinaryFile(dir / "swap.png", "v0");
    ASSERT_TRUE(WaitFor([&] { return sawEventFor("swap.png"); }, std::chrono::seconds(10)))
        << "watcher never reported the destination's creation";
    WriteBinaryFile(dir / "swap_new.png", "v1");
    ASSERT_TRUE(WaitFor([&] { return sawEventFor("swap_new.png"); }, std::chrono::seconds(10)))
        << "watcher never reported the replacement's creation";

    // A second event on swap.png within FileWatcher::DEBOUNCE_DELAY (100ms of
    // its last one) is coalesced and re-fired as a bare Modified — which would
    // drop the OldPath this test exists to check. Timed from the observation
    // above, not from the write, so a late-delivered create cannot shrink it.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    std::error_code ec;
    std::filesystem::rename(dir / "swap_new.png", dir / "swap.png", ec);
    ASSERT_FALSE(ec) << ec.message();

    EXPECT_TRUE(WaitFor([&] { return sawReplaceRename(); }, std::chrono::seconds(10)))
        << "expected a paired Renamed event for the replace";

    // Absence has no predicate to wait on: an unsuppressed Deleted would fire
    // FileWatcher::kDeletedEventHold (200ms) after the rename, so settle well
    // past that hold before concluding it never fires.
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    EXPECT_FALSE(sawTargetDeleted()) << "replaced destination must not surface as Deleted";

    watcher.StopWatching();
    std::filesystem::remove_all(dir);
#else
    GTEST_SKIP() << "Windows-specific event pairing.";
#endif
}

// End-to-end fixture: real file watcher drives the registry, replicating the
// editor's safe-save flow exactly.
class SafeSaveRenameWatcherTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        root = TestUtils::MakeUniqueTempDirectory("safe_save_rename_watcher_test");
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);

        jobSystem = std::make_unique<JobSystem::WorkStealingThreadPool>(2);
        assetManager = std::make_unique<AssetManager>();
        ASSERT_TRUE(assetManager->Initialize(root, jobSystem.get()));

        FileWatchingService::GetInstance().StartWatching();
        ArmWatcher();
    }

    // StartWatching only spawns the watch thread; anything written before it
    // registers the directory handle is never reported, so a fixed sleep here
    // decides by luck whether a test's first write is observable at all — and a
    // lost write has nothing to re-trigger it. Rewrite a sentinel until the
    // registry answers, then take it back off disk and see that leave too: at
    // return the create AND delete legs are both proven live for this root.
    // The service is a process-wide singleton every watcher fixture in this
    // binary starts and stops, so how long it takes to come back up is a
    // property of the whole suite run, not of this test — hence a re-armed
    // probe rather than a fixed wait, and an outer bound that only a genuinely
    // dead watcher can reach.
    void ArmWatcher()
    {
        auto& reg = assetManager->GetRegistry();
        // .png for the same reason the tests below use it with non-image bytes:
        // registration keys off the extension, so the probe is registrable
        // without pretending to be a real texture.
        const auto probe = root / "watcher_arm_probe.png";

        bool armed = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!armed && std::chrono::steady_clock::now() < deadline)
        {
            WriteBinaryFile(probe, "probe");
            armed = WaitFor([&]() { return !reg.GetAssetGUID(probe).IsNull(); },
                            std::chrono::milliseconds(250));
        }
        ASSERT_TRUE(armed) << "watcher never registered the arming sentinel";

        std::error_code ec;
        std::filesystem::remove(probe, ec);
        ASSERT_FALSE(ec) << "could not remove the arming sentinel: " << ec.message();
        ASSERT_TRUE(WaitFor([&]() { return reg.GetAssetGUID(probe).IsNull(); }))
            << "arming sentinel never left the registry";
    }

    void TearDown() override
    {
        if (assetManager)
        {
            assetManager->Shutdown();
            assetManager.reset();
        }
        FileWatchingService::GetInstance().StopWatching();
        jobSystem.reset();
        std::filesystem::remove_all(root);
    }

    std::filesystem::path root;
    std::unique_ptr<JobSystem::WorkStealingThreadPool> jobSystem;
    std::unique_ptr<AssetManager> assetManager;
};

// 13a: an external app's safe-save is a SINGLE replace attempt — apps without
// retry loops fail the save outright when the editor's read handle blocks it.
// Windows' legacy MoveFileEx(REPLACE_EXISTING) rename refuses ANY open target
// handle regardless of sharing (only the writer can opt into POSIX rename
// semantics), so the flows FILE_SHARE_DELETE unblocks are the ReplaceFile
// family (Photoshop, Office) and delete-then-rename fallbacks — both need
// nothing more than delete-compatible sharing from our handles. CRT streams
// can never grant that; SharedFileReader must.
TEST_F(SafeSaveRenameWatcherTest, ExternalSingleAttemptReplaceFileSucceedsWhileReaderHoldsTarget)
{
#ifdef PLATFORM_WINDOWS
    auto& reg = assetManager->GetRegistry();

    const auto target = root / "swap.png";
    const auto temp = root / "swap_new.png";

    WriteBinaryFile(target, "v0");
    ASSERT_TRUE(WaitFor([&]() { return !reg.GetAssetGUID(target).IsNull(); }))
        << "target was never registered by the watcher";
    const GUID targetGuid = reg.GetAssetGUID(target);
    ASSERT_FALSE(targetGuid.IsNull());

    WriteBinaryFile(temp, "v1");
    ASSERT_TRUE(WaitFor([&]() { return !reg.GetAssetGUID(temp).IsNull(); }));
    const GUID tempGuid = reg.GetAssetGUID(temp);

    // Sanity: the CRT sharing mode really does bounce the replace — this is
    // the failure the shared-open path exists to prevent. If this ever starts
    // succeeding, the main assertion below can no longer prove anything.
    {
        std::ifstream legacyReader(target, std::ios::binary);
        ASSERT_TRUE(legacyReader.is_open());
        ASSERT_FALSE(ReplaceFileW(target.wstring().c_str(), temp.wstring().c_str(),
                                  nullptr, 0, nullptr, nullptr))
            << "CRT read handle unexpectedly allowed ReplaceFile";
    }
    ASSERT_TRUE(std::filesystem::exists(target));
    ASSERT_TRUE(std::filesystem::exists(temp))
        << "failed ReplaceFile must leave the replacement file in place";

    // The engine's reader holds the target open across the app's one attempt.
    SharedFileReader reader(target);
    ASSERT_TRUE(reader.IsOpen());

    ASSERT_TRUE(ReplaceFileW(target.wstring().c_str(), temp.wstring().c_str(),
                             nullptr, 0, nullptr, nullptr))
        << "single-attempt external ReplaceFile bounced on the engine's read handle, error "
        << GetLastError();

    // The held handle still reads the displaced content as a consistent
    // snapshot; the new bytes arrive through the watcher as their own event.
    char held[8] = {};
    ASSERT_TRUE(reader.SeekTo(0));
    EXPECT_EQ(reader.Read(held, sizeof(held)), 2);
    EXPECT_EQ(std::string(held, 2), "v0");
    reader.Close();

    // The watcher-driven registry keeps the destination's identity.
    ASSERT_TRUE(WaitFor([&]() { return reg.GetAssetGUID(temp).IsNull(); }))
        << "temp entry still registered after the replace";
    ASSERT_TRUE(WaitFor([&]() { return reg.GetAssetGUID(target) == targetGuid; }))
        << "target GUID changed across the replace: now " << reg.GetAssetGUID(target).ToString()
        << " expected " << targetGuid.ToString() << " (temp GUID was " << tempGuid.ToString()
        << ")";

    std::string onDisk;
    ASSERT_TRUE(ReadFileTextShared(target, onDisk));
    EXPECT_EQ(onDisk, "v1");
#else
    GTEST_SKIP() << "Windows-specific sharing semantics.";
#endif
}

TEST_F(SafeSaveRenameWatcherTest, SafeSaveCyclesKeepTargetGuidStable)
{
    auto& reg = assetManager->GetRegistry();

    const auto target = root / "swap.png";
    const auto temp = root / "swap_new.png";

    WriteBinaryFile(target, "v0");
    ASSERT_TRUE(WaitFor([&]() { return !reg.GetAssetGUID(target).IsNull(); }))
        << "target was never registered by the watcher";
    const GUID targetGuid = reg.GetAssetGUID(target);
    ASSERT_FALSE(targetGuid.IsNull());

    for (int cycle = 0; cycle < 3; ++cycle)
    {
        WriteBinaryFile(temp, "v" + std::to_string(cycle + 1));
        ASSERT_TRUE(WaitFor([&]() { return !reg.GetAssetGUID(temp).IsNull(); }))
            << "temp file was not (re-)registrable on cycle " << cycle;

        std::error_code ec;
        std::filesystem::rename(temp, target, ec);
        ASSERT_FALSE(ec) << "disk rename failed on cycle " << cycle << ": " << ec.message();

        ASSERT_TRUE(WaitFor([&]() { return reg.GetAssetGUID(temp).IsNull(); }))
            << "temp entry still registered after rename-away on cycle " << cycle;
        ASSERT_TRUE(WaitFor([&]() { return reg.GetAssetGUID(target) == targetGuid; }))
            << "target GUID changed on cycle " << cycle;
    }

    EXPECT_EQ(reg.GetAssetGUID(target), targetGuid);
}

// The same cycle performed by this process and announced as its own write, which is how
// every writer in the editor saves. The watcher still reports the replace-rename, and
// that report is now consumed as this write arriving — so the identity this suite exists
// to protect has to survive on the strength of the writer's own report alone.
//
// A regression guard for the consumed-rename path, not a proof that the rename IS
// consumed: it passes with consumption disabled too, because then the watcher's report
// arrives and keeps the identity itself. What consumption does is pinned by
// ExpectedAssetWrite.ASaveOnAWatchedHostDrivesOnce.
TEST_F(SafeSaveRenameWatcherTest, AnAnnouncedSafeSaveKeepsTargetGuidStable)
{
    auto& reg = assetManager->GetRegistry();

    const auto target = root / "announced.png";
    const auto temp = std::filesystem::path(target.string() + ".tmp");

    WriteBinaryFile(target, "v0");
    ASSERT_TRUE(WaitFor([&]() { return !reg.GetAssetGUID(target).IsNull(); }))
        << "target was never registered by the watcher";
    const GUID targetGuid = reg.GetAssetGUID(target);
    ASSERT_FALSE(targetGuid.IsNull());

    for (int cycle = 0; cycle < 3; ++cycle)
    {
        auto write = assetManager->ExpectWrite(target);
        WriteBinaryFile(temp, "v" + std::to_string(cycle + 1));

        std::error_code ec;
        std::filesystem::rename(temp, target, ec);
        ASSERT_FALSE(ec) << "disk rename failed on cycle " << cycle << ": " << ec.message();

        write.Report(target);

        EXPECT_EQ(reg.GetAssetGUID(target), targetGuid)
            << "the writer's own report re-minted the target's identity on cycle " << cycle;
    }

    // Past the watcher's report of the last cycle: consuming it must not have left the
    // registry pointing anywhere else.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(reg.GetAssetGUID(target), targetGuid);
    EXPECT_TRUE(reg.GetAssetGUID(temp).IsNull()) << "the safe-save temp was registered";
}

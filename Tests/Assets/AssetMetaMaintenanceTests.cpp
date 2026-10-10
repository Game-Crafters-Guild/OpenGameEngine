#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Assets/FileWatchingService.h"
#include "FileWatcher/FileWatcher.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestTempDir.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace GameEngine;

namespace
{
static void WriteTextFile(const std::filesystem::path& p, const std::string& content)
{
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(f.is_open()) << "Failed to open: " << p.string();
    f << content;
}

static bool WaitUntil(const std::function<bool()>& pred,
                      std::chrono::milliseconds timeout = std::chrono::seconds(5),
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

class AssetMetaMaintenanceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        root = TestUtils::MakeUniqueTempDirectory("asset_meta_maintenance_test");
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);

        jobSystem = std::make_unique<JobSystem::WorkStealingThreadPool>(2);
        assetManager = std::make_unique<AssetManager>();
        ASSERT_TRUE(assetManager->Initialize(root, jobSystem.get()));

        // Start centralized file watching (AssetManager is subscribed).
        FileWatchingService::GetInstance().StartWatching();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
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

TEST_F(AssetMetaMaintenanceTest, RenamingAssetPreservesGuidWithoutMeta)
{
    const auto oldAsset = root / "rename_me.txt";
    const auto newAsset = root / "renamed.txt";

    WriteTextFile(oldAsset, "hello");
    ASSERT_TRUE(WaitUntil([&]() { return !assetManager->GetRegistry().GetAssetGUID(oldAsset).IsNull(); }))
        << "Expected asset to be registered: " << oldAsset.string();

    const GUID guidBefore = assetManager->GetRegistry().GetAssetGUID(oldAsset);
    ASSERT_FALSE(guidBefore.IsNull());

    const auto oldMeta = std::filesystem::path(oldAsset.string() + ".meta");
    const auto newMeta = std::filesystem::path(newAsset.string() + ".meta");
    EXPECT_FALSE(std::filesystem::exists(oldMeta));

    std::error_code ec;
    std::filesystem::rename(oldAsset, newAsset, ec);
    ASSERT_FALSE(ec) << "Failed to rename asset: " << ec.message();

    ASSERT_TRUE(WaitUntil([&]() { return assetManager->GetRegistry().GetAssetGUID(newAsset) == guidBefore; }))
        << "Expected GUID to be preserved across rename";

    EXPECT_FALSE(std::filesystem::exists(oldMeta));
    EXPECT_FALSE(std::filesystem::exists(newMeta));
}

// Drives a raw FileWatcher through a rename and requires it to report ONE
// Renamed event carrying both spellings. `pollingIntervalMs` only matters to
// the polling backend; the Windows backend reports as the OS delivers.
static void ExpectRenameReportedWithOldPath(const char* tempPrefix, uint32 pollingIntervalMs)
{
    const auto dir = TestUtils::MakeUniqueTempDirectory(tempPrefix);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    std::mutex m;
    std::condition_variable cv;
    bool gotRename = false;
    FileChangeEvent renameEv{};
    std::vector<std::filesystem::path> reported;
    std::vector<FileChangeType> reportedTypes;

    FileWatcher watcher;
    watcher.SetPollingInterval(pollingIntervalMs);
    watcher.SetCallback([&](const FileChangeEvent& ev)
                        {
                            std::lock_guard<std::mutex> lk(m);
                            reported.push_back(ev.Path.filename());
                            reportedTypes.push_back(ev.Type);
                            if (ev.Type != FileChangeType::Renamed)
                                return;
                            renameEv = ev;
                            gotRename = true;
                            cv.notify_one();
                        });

    const auto sawEventFor = [&m, &reported](const char* filename)
    {
        std::lock_guard<std::mutex> lk(m);
        return std::find(reported.begin(), reported.end(), filename) != reported.end();
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
        WriteTextFile(dir / "armed_probe.txt", "probe");
        armed = WaitUntil([&]() { return sawEventFor("armed_probe.txt"); },
                          std::chrono::milliseconds(250));
    }
    ASSERT_TRUE(armed) << "watcher never reported the arming sentinel";

    const auto oldPath = dir / "old_name.txt";
    const auto newPath = dir / "new_name.txt";
    WriteTextFile(oldPath, "hello");
    ASSERT_TRUE(WaitUntil([&]() { return sawEventFor("old_name.txt"); }, std::chrono::seconds(10)))
        << "watcher never reported the file being renamed";

    std::error_code ec;
    std::filesystem::rename(oldPath, newPath, ec);
    ASSERT_FALSE(ec) << "Rename failed: " << ec.message();

    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::seconds(5), [&]() { return gotRename; });
    }

    watcher.StopWatching();
    std::filesystem::remove_all(dir);

    ASSERT_TRUE(gotRename) << "Did not observe a Renamed event";
    EXPECT_EQ(renameEv.Path, newPath);
    EXPECT_EQ(renameEv.OldPath, oldPath);

    // The rename must not also surface as the pair it replaces: a Deleted for
    // the old spelling would tear down the asset's registration, a Created for
    // the new one would mint it a second identity.
    std::lock_guard<std::mutex> lk(m);
    for (size_t i = 0; i < reported.size(); ++i)
    {
        EXPECT_FALSE(reportedTypes[i] == FileChangeType::Deleted && reported[i] == "old_name.txt")
            << "rename also reported as Deleted(old)";
        EXPECT_FALSE(reportedTypes[i] == FileChangeType::Created && reported[i] == "new_name.txt")
            << "rename also reported as Created(new)";
    }
}

// The Windows backend pairs FILE_ACTION_RENAMED_OLD_NAME / NEW_NAME as the OS
// delivers them.
TEST(FileWatcherRenamePairingTest, WindowsRenameEventHasOldPath)
{
#ifdef PLATFORM_WINDOWS
    ExpectRenameReportedWithOldPath("filewatcher_rename_pairing_test", 1000);
#else
    GTEST_SKIP() << "Native rename events are the Windows backend; the polling backend is "
                    "covered by PollingRenameEventHasOldPath.";
#endif
}

// The polling backend sees a rename as one path vanishing and another
// appearing in the same scan; it pairs the two by file identity (volume +
// inode), which a rename keeps, so the registry can carry the GUID across.
TEST(FileWatcherRenamePairingTest, PollingRenameEventHasOldPath)
{
#ifdef PLATFORM_WINDOWS
    GTEST_SKIP() << "The polling backend does not run on Windows.";
#else
    ExpectRenameReportedWithOldPath("filewatcher_polling_rename_pairing_test", 100);
#endif
}

// An inode is not unique over time: ext4 and xfs give a deleted file's inode
// to the next file created, so "delete A, create B" inside one poll presents
// B under A's identity. The pairing also requires A's timestamp to ride
// along (rename() preserves it, a new file has a new one), and this test
// fabricates the recycled-inode shape with a hard link: B is created
// carrying A's identity, A is removed, and B is given a different
// timestamp. The watcher must report Deleted(A) + Created(B), never a
// Renamed that would rebind A's GUID onto B's bytes.
TEST(FileWatcherRenamePairingTest, PollingDoesNotPairARecycledIdentityWithANewTimestamp)
{
#ifdef PLATFORM_WINDOWS
    GTEST_SKIP() << "The polling backend does not run on Windows.";
#else
    const auto dir = TestUtils::MakeUniqueTempDirectory("filewatcher_polling_recycled_identity");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    std::mutex m;
    std::vector<FileChangeEvent> reported;
    FileWatcher watcher;
    // A wide poll so the three steps below land inside one scan window.
    watcher.SetPollingInterval(1000);
    watcher.SetCallback([&](const FileChangeEvent& ev)
                        {
                            std::lock_guard<std::mutex> lk(m);
                            reported.push_back(ev);
                        });
    const auto sawEvent = [&](FileChangeType type, const std::filesystem::path& path)
    {
        std::lock_guard<std::mutex> lk(m);
        return std::any_of(reported.begin(), reported.end(), [&](const FileChangeEvent& ev)
                           { return ev.Type == type && ev.Path.filename() == path.filename(); });
    };

    ASSERT_TRUE(watcher.StartWatching(dir, true));
    bool armed = false;
    const auto armDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!armed && std::chrono::steady_clock::now() < armDeadline)
    {
        WriteTextFile(dir / "armed_probe.txt", "probe");
        armed = WaitUntil([&]() { return sawEvent(FileChangeType::Created, "armed_probe.txt") ||
                                         sawEvent(FileChangeType::Modified, "armed_probe.txt"); },
                          std::chrono::milliseconds(250));
    }
    ASSERT_TRUE(armed) << "watcher never reported the arming sentinel";

    const auto a = dir / "a.txt";
    const auto b = dir / "b.txt";
    WriteTextFile(a, "a");
    ASSERT_TRUE(WaitUntil([&]() { return sawEvent(FileChangeType::Created, a); }, std::chrono::seconds(10)));

    // Same identity, new path, new timestamp, A gone: the recycled-inode shape.
    std::error_code ec;
    std::filesystem::create_hard_link(a, b, ec);
    ASSERT_FALSE(ec) << ec.message();
    std::filesystem::remove(a, ec);
    ASSERT_FALSE(ec) << ec.message();
    std::filesystem::last_write_time(b, std::filesystem::last_write_time(b) + std::chrono::seconds(5), ec);
    ASSERT_FALSE(ec) << ec.message();

    EXPECT_TRUE(WaitUntil([&]() { return sawEvent(FileChangeType::Created, b); }, std::chrono::seconds(10)))
        << "B is a new file and must be reported as Created";
    EXPECT_TRUE(WaitUntil([&]() { return sawEvent(FileChangeType::Deleted, a); }, std::chrono::seconds(10)))
        << "A is gone and must be reported as Deleted";
    watcher.StopWatching();
    std::filesystem::remove_all(dir);

    std::lock_guard<std::mutex> lk(m);
    for (const FileChangeEvent& ev : reported)
    {
        EXPECT_NE(ev.Type, FileChangeType::Renamed)
            << "recycled identity paired as a rename: " << ev.OldPath.string() << " -> " << ev.Path.string();
    }
#endif
}

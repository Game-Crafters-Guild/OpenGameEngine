// One save, one drive — on a host that watches its files and on one that cannot.
//
// A writer announces a write, publishes the bytes and reports it; the report is what
// drives the change pipeline. Where a file watcher runs it reports the same write a
// moment later, and that report has to be recognised as this write arriving rather than
// as a second change. Where no watcher runs (the browser build has no filesystem
// watching at all) the report is the only delivery there will ever be.
//
// The watched arm below performs a real safe-save through a real FileWatcher and counts
// what the watching service dispatched as well as what the asset pipeline drove, so a
// run in which the watcher happened to deliver nothing fails as an untrustworthy
// instrument instead of passing for the wrong reason. The fixture waits for the watcher
// to prove it is delivering before any test body runs — watching arms on its own thread
// after StartWatching returns, so a save made immediately can land before the first
// change request and be reported by nobody.

#include <gtest/gtest.h>

#include "AssetCore/AssetEvents.h"
#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h" // AssetSourceDesc
#include "Assets/FileWatchingService.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "WatchedFileEvents.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace GameEngine;

constexpr size_t kPoolWorkers = 2;
constexpr const char* kSourceAlias = "expected";

// Generous against the watcher's own holds (100 ms debounce, 200 ms delete hold, one
// poll interval on a polling backend), all of which stretch under load.
constexpr std::chrono::milliseconds kWatcherDeadline{8000};

std::filesystem::path MakeUniqueTempDir()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = std::filesystem::temp_directory_path() / ("ge_expected_write_" + std::to_string(stamp));
    std::filesystem::create_directories(dir);
    return dir;
}

void WriteFile(const std::filesystem::path& path, std::string_view payload)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
}

// What every writer in the engine does: build the bytes beside the destination, then
// put them in place with a rename. The watcher reports the rename, not the temp.
void SafeSave(const std::filesystem::path& target, std::string_view payload)
{
    const std::filesystem::path temp = target.string() + ".tmp";
    WriteFile(temp, payload);
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    ASSERT_FALSE(ec) << "safe-save rename failed: " << ec.message();
}

bool WaitFor(const std::function<bool()>& pred,
             std::chrono::milliseconds timeout = kWatcherDeadline,
             std::chrono::milliseconds step = std::chrono::milliseconds(20))
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

// BinaryAsset (.bin) is the simplest device-free registered asset type, so this
// exercises the real registry path without a renderer or an EngineCore.
struct WriteFixture
{
    explicit WriteFixture(bool watched)
        : Watched(watched),
          Directory(MakeUniqueTempDir()),
          Pool(std::make_unique<JobSystem::WorkStealingThreadPool>(kPoolWorkers)),
          Assets(std::make_unique<AssetManager>())
    {
        EXPECT_TRUE(Assets->Initialize(Pool.get()));

        AssetSourceDesc source;
        source.Alias = kSourceAlias;
        source.Root = Directory;
        source.DerivedIdentity = true; // GUID = hash(alias + rel path); no DB required
        source.RequiresScan = true;
        source.RegisterFileWatcher = watched;
        EXPECT_TRUE(Assets->RegisterSource(source));
        Assets->WaitForStartupScan(kSourceAlias);

        if (Watched)
        {
            FileWatchingService::GetInstance().StartWatching();
            ArmedForWatching = TestUtils::WaitUntilWatchingIsArmed(Directory);
        }
    }

    ~WriteFixture()
    {
        if (Watched)
            FileWatchingService::GetInstance().StopWatching();
        Assets.reset();
        Pool.reset();
        std::error_code ec;
        std::filesystem::remove_all(Directory, ec);
    }

    bool Watched;
    // False when the watcher never delivered anything for this directory: the watched
    // tests are then measuring nothing and say so rather than passing.
    bool ArmedForWatching = false;
    std::filesystem::path Directory;
    std::unique_ptr<JobSystem::WorkStealingThreadPool> Pool;
    std::unique_ptr<AssetManager> Assets;
};

// Counts the asset events the pipeline dispatched for one path, for the life of the scope.
class DriveCounter
{
  public:
    DriveCounter(AssetManager& assets, std::filesystem::path path)
        : m_Assets(assets), m_Path(std::move(path))
    {
        m_Handle = m_Assets.GetEventDispatcher().AddCallback(
            [this](const AssetEvent& event)
            {
                if (std::filesystem::path(event.AssetPath) == m_Path)
                {
                    m_Types.push_back(event.EventType);
                    m_Count.fetch_add(1, std::memory_order_release);
                }
            });
    }

    ~DriveCounter() { m_Assets.GetEventDispatcher().RemoveCallback(m_Handle); }

    DriveCounter(const DriveCounter&) = delete;
    DriveCounter& operator=(const DriveCounter&) = delete;

    int Count() const { return m_Count.load(std::memory_order_acquire); }
    const std::vector<AssetEventType>& Types() const { return m_Types; }

  private:
    AssetManager& m_Assets;
    std::filesystem::path m_Path;
    uint32 m_Handle = 0;
    std::atomic<int> m_Count{0};
    std::vector<AssetEventType> m_Types;
};

} // namespace

// The browser's host: nothing outside the process can change these files, so no watcher
// runs and the writer's report is the only delivery there will ever be. Without it the
// asset just authored has no registry identity at all.
TEST(ExpectedAssetWrite, ASaveOnAHostWithoutAWatcherDrivesOnce)
{
    WriteFixture fixture(/*watched=*/false);
    const std::filesystem::path assetPath = fixture.Directory / "unwatched.bin";

    const size_t countBefore = fixture.Assets->GetRegistry().GetAssetCount();
    DriveCounter drives(*fixture.Assets, assetPath);

    {
        auto write = fixture.Assets->ExpectWrite(assetPath);
        SafeSave(assetPath, "authored on a host that cannot watch");
        write.Report(assetPath);
    }

    ASSERT_EQ(drives.Count(), 1) << "the only report of the write did not drive exactly once";
    EXPECT_EQ(drives.Types()[0], AssetEventType::AssetCreated);
    EXPECT_FALSE(fixture.Assets->GetRegistry().GetAssetGUID(assetPath).IsNull());
    EXPECT_EQ(fixture.Assets->GetRegistry().GetAssetCount(), countBefore + 1);

    // Nothing else is coming, but a late second drive would be just as wrong.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(drives.Count(), 1);
}

// The desktop host: a real watcher reports the same safe-save the writer just reported.
// One save must still drive the pipeline once.
TEST(ExpectedAssetWrite, ASaveOnAWatchedHostDrivesOnce)
{
    WriteFixture fixture(/*watched=*/true);
    ASSERT_TRUE(fixture.ArmedForWatching) << "the watcher never came up for this directory";
    const std::filesystem::path assetPath = fixture.Directory / "watched.bin";

    TestUtils::FileEventWitness witness(fixture.Directory, assetPath);
    DriveCounter drives(*fixture.Assets, assetPath);

    {
        auto write = fixture.Assets->ExpectWrite(assetPath);
        SafeSave(assetPath, "authored where a watcher is running");
        write.Report(assetPath);
    }

    ASSERT_EQ(drives.Count(), 1) << "the writer's report did not drive";

    ASSERT_TRUE(WaitFor([&witness] { return witness.Count() > 0; }))
        << "the watcher never reported this save, so this run cannot say whether its "
           "report would have driven the pipeline a second time";

    // Past the watcher's own report, and past the debounce behind it.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(drives.Count(), 1)
        << "the watcher's report of a save already delivered drove the pipeline again";
    EXPECT_FALSE(fixture.Assets->GetRegistry().GetAssetGUID(assetPath).IsNull());
}

// The other half. A file this process wrote is not exempt from being edited by something
// else a moment later, and inside the echo window is exactly when that is easiest to get
// wrong: the path is announced, so only the bytes distinguish the two.
TEST(ExpectedAssetWrite, AnExternalEditInsideTheEchoWindowStillDrives)
{
    WriteFixture fixture(/*watched=*/true);
    ASSERT_TRUE(fixture.ArmedForWatching) << "the watcher never came up for this directory";
    const std::filesystem::path assetPath = fixture.Directory / "then_edited.bin";

    DriveCounter drives(*fixture.Assets, assetPath);

    {
        auto write = fixture.Assets->ExpectWrite(assetPath);
        SafeSave(assetPath, "ours");
        write.Report(assetPath);
    }
    ASSERT_EQ(drives.Count(), 1);

    // Not a safe save: an outside editor writing the file in place, immediately.
    WriteFile(assetPath, "somebody else's, and a different length");

    EXPECT_TRUE(WaitFor([&drives] { return drives.Count() >= 2; }))
        << "an external edit made inside the echo window was swallowed as our own write";
    EXPECT_EQ(drives.Count(), 2);
}

// An announced write that never happened must not swallow the next change to the path it
// did not produce. Dropping the handle without reporting is how a failed write says so.
TEST(ExpectedAssetWrite, AWriteThatNeverLandedDoesNotSwallowTheNextChange)
{
    WriteFixture fixture(/*watched=*/true);
    ASSERT_TRUE(fixture.ArmedForWatching) << "the watcher never came up for this directory";
    const std::filesystem::path assetPath = fixture.Directory / "abandoned.bin";

    DriveCounter drives(*fixture.Assets, assetPath);

    {
        auto write = fixture.Assets->ExpectWrite(assetPath);
        // The write fails: nothing is produced and nothing is reported.
    }

    WriteFile(assetPath, "written by something else entirely");

    EXPECT_TRUE(WaitFor([&drives] { return drives.Count() >= 1; }))
        << "an abandoned announcement swallowed a change it never made";
}

// A write the caller reports but never made must not invent an asset: the registry only
// takes paths that are regular files on disk right now.
TEST(ExpectedAssetWrite, ReportingAMissingFileRegistersNothing)
{
    WriteFixture fixture(/*watched=*/false);
    const std::filesystem::path assetPath = fixture.Directory / "never_written.bin";

    const size_t countBefore = fixture.Assets->GetRegistry().GetAssetCount();
    {
        auto write = fixture.Assets->ExpectWrite(assetPath);
        write.Report(assetPath);
    }

    EXPECT_EQ(fixture.Assets->GetRegistry().GetAssetCount(), countBefore);
    EXPECT_TRUE(fixture.Assets->GetRegistry().GetAssetGUID(assetPath).IsNull());
}

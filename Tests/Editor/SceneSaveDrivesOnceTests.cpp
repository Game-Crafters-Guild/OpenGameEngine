// A scene save drives the asset pipeline once, with a real file watcher running.
//
// The document announces the file it is about to write and reports it afterwards; the
// watcher's report of the same write is recognised as that write arriving. What makes
// Save As worth its own test is that it lands on the target TWICE — it copies the
// previous scene into place first so comments and includes carry across, then writes the
// document over it — and the watcher reports the copy the moment it appears. An
// announcement that only covers the second touch leaves the first one driving a change
// of its own, which is a second import of a file the user saved once.
//
// Both tests carry a witness on the watcher's own dispatch, because a drive count alone
// cannot tell "the echo was consumed" from "no watcher ever reported anything" — and the
// second is reachable, since watching arms on its own thread after StartWatching returns.

#include <gtest/gtest.h>

#include "Scene/SceneDocumentManager.h"

#include "AssetCore/AssetEvents.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/FileWatchingService.h"
#include "Core/Engine.h"
#include "ECS/World.h"

#include "TestTempDir.h"
#include "WatchedFileEvents.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace GameEngine;

namespace
{
// A fresh alias per suite setup, mounted on a root unique to the process and the setup.
// Unmounting a source is asynchronous (BeginUnregisterSource), so a run that mounts the same
// alias twice — which is what --gtest_repeat does, and what anyone chasing a flake here will
// reach for — would either be refused or race the teardown. Mounting somewhere new instead is
// hermetic and cheap.
int g_MountGeneration = 0;

// Generous against the watcher's own holds (100 ms debounce, 200 ms delete hold) and
// against machine load; the tests below only ever wait for a report that does arrive.
constexpr std::chrono::milliseconds kWatcherDeadline{8000};

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
} // namespace

class SceneSaveDrivesOnceTests : public ::testing::Test
{
protected:
    // The watched source is mounted once for the whole suite: unmounting is asynchronous,
    // and a per-test mount/unmount cycle would race the watcher these tests depend on.
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

        s_Alias = "save_drives_once_" + std::to_string(++g_MountGeneration);
        s_Dir = GameEngine::TestUtils::MakeUniqueTempDirectory("GameEngine_SceneSaveDrivesOnceTests");
        std::error_code ec;
        std::filesystem::create_directories(s_Dir, ec);

        AssetManager& assets = engine.GetAssetManager();
        AssetSourceDesc source;
        source.Alias = s_Alias;
        source.Root = s_Dir;
        source.DerivedIdentity = true;
        source.RequiresScan = true;
        source.RegisterFileWatcher = true;
        ASSERT_TRUE(assets.RegisterSource(source));
        assets.WaitForStartupScan(s_Alias);
        FileWatchingService::GetInstance().StartWatching();
        s_Armed = ::TestUtils::WaitUntilWatchingIsArmed(s_Dir);
    }

    static void TearDownTestSuite()
    {
        FileWatchingService::GetInstance().StopWatching();
        std::error_code ec;
        std::filesystem::remove_all(s_Dir, ec);
    }

    // Asset events dispatched for one path, for the life of the scope. The event types are
    // kept, not just the count: "two events" is a different finding depending on whether the
    // second one carried content or was a deletion, and a bare count cannot say which.
    class DriveCounter
    {
      public:
        explicit DriveCounter(std::filesystem::path path) : m_Path(std::move(path))
        {
            m_Handle = EngineCore::GetInstance().GetAssetManager().GetEventDispatcher().AddCallback(
                [this](const AssetEvent& event)
                {
                    if (std::filesystem::path(event.AssetPath) != m_Path)
                        return;
                    std::lock_guard<std::mutex> lock(m_Mutex);
                    m_Types.push_back(event.EventType);
                });
        }

        ~DriveCounter()
        {
            EngineCore::GetInstance().GetAssetManager().GetEventDispatcher().RemoveCallback(m_Handle);
        }

        DriveCounter(const DriveCounter&) = delete;
        DriveCounter& operator=(const DriveCounter&) = delete;

        int Count() const
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            return static_cast<int>(m_Types.size());
        }

        std::string Describe() const
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            std::string out;
            for (const AssetEventType t : m_Types)
            {
                if (!out.empty())
                    out += ", ";
                switch (t)
                {
                case AssetEventType::AssetCreated: out += "Created"; break;
                case AssetEventType::AssetModified: out += "Modified"; break;
                case AssetEventType::AssetDestroyed: out += "Destroyed"; break;
                default: out += "other(" + std::to_string(static_cast<int>(t)) + ")"; break;
                }
            }
            return out.empty() ? std::string("<none>") : out;
        }

      private:
        std::filesystem::path m_Path;
        uint32 m_Handle = 0;
        mutable std::mutex m_Mutex;
        std::vector<AssetEventType> m_Types;
    };

    static std::filesystem::path s_Dir;
    static std::string s_Alias;
    // False when the watcher never delivered anything for s_Dir: these tests are then
    // measuring nothing and say so rather than passing.
    static bool s_Armed;
};

std::filesystem::path SceneSaveDrivesOnceTests::s_Dir;
std::string SceneSaveDrivesOnceTests::s_Alias;
bool SceneSaveDrivesOnceTests::s_Armed = false;

// Save As onto a fresh path: the preservation copy and the document write are one save.
TEST_F(SceneSaveDrivesOnceTests, SaveAsFromAnExistingSceneDrivesOnce)
{
    ASSERT_TRUE(s_Armed) << "the watcher never came up for this directory";

    ECS::World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);

    const std::filesystem::path first = s_Dir / "first.scene";
    ASSERT_TRUE(doc.SaveAs(first)) << "could not establish the document's scene path";

    const std::filesystem::path second = s_Dir / "second.scene";
    ::TestUtils::FileEventWitness witness(s_Dir, second);
    DriveCounter drives(second);

    ASSERT_TRUE(doc.SaveAs(second));
    ASSERT_TRUE(std::filesystem::exists(second));

    ASSERT_TRUE(WaitFor([&drives] { return drives.Count() >= 1; }))
        << "the save itself never drove the pipeline";

    // The instrument. Without a watcher report of this save there is nothing for the
    // announcement to have absorbed, and the count below would be one for the wrong
    // reason.
    ASSERT_TRUE(WaitFor([&witness] { return witness.Count() > 0; }))
        << "the watcher never reported this save, so this run cannot say whether its "
           "report would have driven the pipeline a second time";

    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    EXPECT_EQ(drives.Count(), 1)
        << "one Save As drove the pipeline " << drives.Count()
        << " times — a touch of the target outside the announcement. Events: "
        << drives.Describe();
}

// Plain Save over the document's own path: one safe-save, one drive.
TEST_F(SceneSaveDrivesOnceTests, SaveOverTheSamePathDrivesOnce)
{
    ASSERT_TRUE(s_Armed) << "the watcher never came up for this directory";

    ECS::World world;
    Editor::SceneDocumentManager doc;
    doc.SetWorld(&world);

    const std::filesystem::path scene = s_Dir / "resaved.scene";
    ASSERT_TRUE(doc.SaveAs(scene));
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    ::TestUtils::FileEventWitness witness(s_Dir, scene);
    DriveCounter drives(scene);
    ASSERT_TRUE(doc.Save());

    ASSERT_TRUE(WaitFor([&drives] { return drives.Count() >= 1; }))
        << "the save itself never drove the pipeline";

    ASSERT_TRUE(WaitFor([&witness] { return witness.Count() > 0; }))
        << "the watcher never reported this save, so this run cannot say whether its "
           "report would have driven the pipeline a second time";

    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    EXPECT_EQ(drives.Count(), 1)
        << "one Save drove the pipeline " << drives.Count()
        << " times. Events: " << drives.Describe();
}

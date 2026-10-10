// Pins that cancelling a hot-reload run ends the swap stage's wait for the main
// thread at once. The swap stage hands the swap to the main thread and waits
// for it; a Shutdown on the main thread cannot pump that queue, so a wait that
// ignored the run's cancellation stalled Shutdown for the full timeout.
//
// The engine is initialized headless with scripting fully disabled, so the
// swap stage takes its main-thread path against a ScriptManager whose queue
// this test owns and never pumps.

#include <gtest/gtest.h>

#include "Core/Application.h"
#include "Core/Engine.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Jobs/HotReloadCancellation.h"
#include "Jobs/HotReloadTasks.h"
#include "Scripting/ScriptManager.h"
#include "Scripting/ScriptsConfig.h"
#include "TestTempDir.h" // Tests/

#include <chrono>
#include <filesystem>
#include <memory>
#include <system_error>
#include <thread>

using namespace GameEngine;
using namespace std::chrono_literals;

namespace
{

constexpr auto kDeadline = 10s;
// Well under the swap stage's 5 s main-thread timeout.
constexpr auto kPromptCancel = 1s;

class MainThreadSwapCancel : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_OriginalWorkingDirectory = std::filesystem::current_path();
        m_Workspace = TestUtils::MakeUniqueTempDirectory("MainThreadSwapCancel");

        ApplicationConfig config{};
        config.WorkspaceDirectory = m_Workspace.string();
        config.AssetDirectory = "Assets";
        ScriptsConfig scriptsConfig{};
        scriptsConfig.disableClr = true;
        scriptsConfig.enableHotReload = false;
        scriptsConfig.enableAsyncHotReload = false;
        scriptsConfig.enableAutoProjectGeneration = false;
        EngineCore::GetInstance().SetScriptsConfig(scriptsConfig);
        ASSERT_TRUE(EngineCore::GetInstance().Initialize(config));
    }

    void TearDown() override
    {
        EngineCore::GetInstance().Shutdown();
        std::error_code ec;
        std::filesystem::current_path(m_OriginalWorkingDirectory, ec);
        std::filesystem::remove_all(m_Workspace, ec);
    }

    std::filesystem::path m_OriginalWorkingDirectory;
    std::filesystem::path m_Workspace;
};

} // namespace

TEST_F(MainThreadSwapCancel, CancelEndsTheWaitForTheMainThread)
{
    // This thread plays the editor's main thread: marked, so the swap stage
    // does not pump the queue inline, and busy, so it never pumps it either.
    auto& scriptManager = EngineCore::GetInstance().GetScriptManager();
    scriptManager.MarkMainThread();
    ASSERT_EQ(scriptManager.GetMainThreadTaskCount(), 0u);

    JobSystem::WorkStealingThreadPool pool(1);
    auto cancellation = std::make_shared<HotReloadCancellation>();
    auto swap = pool.Submit(std::make_unique<MainThreadSwapTask>(&pool, "GameEngine.Scripts.dll", nullptr,
                                                                 std::make_shared<PreloadSync>(), nullptr, cancellation));
    ASSERT_TRUE(swap.IsValid());

    // The swap is queued for the main thread just before the stage starts waiting.
    const auto queueDeadline = std::chrono::steady_clock::now() + kDeadline;
    while (scriptManager.GetMainThreadTaskCount() == 0 && std::chrono::steady_clock::now() < queueDeadline)
    {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_EQ(scriptManager.GetMainThreadTaskCount(), 1u) << "the swap stage never queued its main-thread swap";

    const auto cancelled = std::chrono::steady_clock::now();
    cancellation->Cancel();
    swap.Wait();
    const auto waited = std::chrono::steady_clock::now() - cancelled;

    EXPECT_LT(waited, kPromptCancel) << "the swap stage waited "
                                     << std::chrono::duration_cast<std::chrono::milliseconds>(waited).count()
                                     << " ms for the main thread after its run was cancelled";
    std::chrono::milliseconds swapDuration{};
    EXPECT_FALSE(swap.TryGetResult(swapDuration)) << "a cancelled swap must not publish a swap duration";
}

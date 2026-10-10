// Tick/PollEventsAndTick driver contract. External frame drivers call them
// directly instead of Run(): tests call Tick(), the browser
// requestAnimationFrame runner calls PollEventsAndTick(). These tests pin the
// properties they rely on: a requested exit makes both return false without
// running a frame, each manual call advances exactly one frame until exit
// (PollEventsAndTick included, when its pump ran no frame), a frame reports
// nothing unless asked to, and a frame waits on the CPU only when GE_FRAME_CAP
// asks for a limiter.

#include "Core/Application.h"
#include "Core/Engine.h"
#include "Scripting/ScriptsConfig.h"

#include "EngineLogCapture.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace
{

namespace fs = std::filesystem;

class CountingApp : public GameEngine::Application
{
public:
    using GameEngine::Application::Application;

    int UpdateCalls = 0;
    int RenderCalls = 0;
    int ExitAfterUpdates = -1;
    bool RequestNestedTick = false;
    bool NestedTickResult = false;

protected:
    void Update(GameEngine::float64) override
    {
        ++UpdateCalls;
        if (RequestNestedTick)
        {
            RequestNestedTick = false;
            NestedTickResult = Tick();
        }
        if (ExitAfterUpdates >= 0 && UpdateCalls >= ExitAfterUpdates)
            RequestExit();
    }

    void Render() override { ++RenderCalls; }
};

constexpr const char* kFrameCapEnvVar = "GE_FRAME_CAP";

void SetFrameCapEnv(const char* value)
{
#ifdef _WIN32
    // An empty value removes the variable on Windows.
    _putenv_s(kFrameCapEnvVar, value != nullptr ? value : "");
#else
    if (value != nullptr)
        setenv(kFrameCapEnvVar, value, 1);
    else
        unsetenv(kFrameCapEnvVar);
#endif
}

// Sets GE_FRAME_CAP for one test (nullptr unsets it) and puts back whatever the
// environment held before.
class ScopedFrameCapEnv
{
public:
    explicit ScopedFrameCapEnv(const char* value)
    {
        if (const char* previous = std::getenv(kFrameCapEnvVar))
        {
            m_Previous = previous;
            m_HadPrevious = true;
        }
        SetFrameCapEnv(value);
    }

    ~ScopedFrameCapEnv() { SetFrameCapEnv(m_HadPrevious ? m_Previous.c_str() : nullptr); }

private:
    std::string m_Previous;
    bool m_HadPrevious = false;
};

// Ticks `frames` frames of a non-editor application, as the Player runs, and
// counts the frames the frame-rate limiter held with a CPU wait. The frames do
// no work, so a limiter at any ordinary rate holds them.
int CountFramesHeldByTheLimiter(const char* name, int frames)
{
    const fs::path tempRoot = fs::temp_directory_path() / "ge_application_frame_cap_test";
    fs::create_directories(tempRoot / "Assets");

    GameEngine::ScriptsConfig scriptsConfig{};
    scriptsConfig.disableClr = true;
    scriptsConfig.enableHotReload = false;
    scriptsConfig.enableAsyncHotReload = false;
    scriptsConfig.enableAutoProjectGeneration = false;
    GameEngine::EngineCore::GetInstance().SetScriptsConfig(scriptsConfig);

    GameEngine::ApplicationConfig cfg{};
    cfg.Name = name;
    cfg.WorkspaceDirectory = tempRoot.string();
    cfg.AssetDirectory = "Assets";
    cfg.EnableEditor = false;

    int held = 0;
    {
        CountingApp app(cfg);
        EXPECT_TRUE(app.Initialize());
        for (int frame = 0; frame < frames; ++frame)
        {
            app.Tick();
            if (app.GetLastFramePhaseTimings().SleepMs > 0.0)
                ++held;
        }
        app.Shutdown();
    }

    std::error_code ec;
    fs::remove_all(tempRoot, ec);
    return held;
}

} // namespace

TEST(ApplicationTick, ExitRequestedMeansNoFrameRuns)
{
    GameEngine::ApplicationConfig cfg{};
    cfg.Name = "TickContract.NoFrame";
    CountingApp app(cfg);

    app.RequestExit();
    EXPECT_FALSE(app.Tick());
    EXPECT_FALSE(app.PollEventsAndTick());
    EXPECT_EQ(app.UpdateCalls, 0);
    EXPECT_EQ(app.RenderCalls, 0);
    EXPECT_EQ(app.GetFrameCount(), 0u);
}

TEST(ApplicationTick, ManualTicksAdvanceOneFrameEachAndStopOnExit)
{
    const fs::path tempRoot =
        fs::temp_directory_path() / "ge_application_tick_test";
    fs::create_directories(tempRoot / "Assets");

    // Scripting-free EngineCore so this Engine-only target boots without a
    // .NET runtime (same pattern as RenderPipelineDeclareTests).
    GameEngine::ScriptsConfig scriptsConfig{};
    scriptsConfig.disableClr = true;
    scriptsConfig.enableHotReload = false;
    scriptsConfig.enableAsyncHotReload = false;
    scriptsConfig.enableAutoProjectGeneration = false;
    GameEngine::EngineCore::GetInstance().SetScriptsConfig(scriptsConfig);

    GameEngine::ApplicationConfig cfg{};
    cfg.Name = "TickContract.ManualFrames";
    cfg.WorkspaceDirectory = tempRoot.string();
    cfg.AssetDirectory = "Assets";

    {
        CountingApp app(cfg);
        app.ExitAfterUpdates = 3;
        // A native resize notification can arrive during the frame itself.
        // Its refresh must not run a second update/render inside this frame.
        app.RequestNestedTick = true;
        ASSERT_TRUE(app.Initialize());

        EXPECT_TRUE(app.Tick());
        EXPECT_TRUE(app.NestedTickResult);
        EXPECT_EQ(app.UpdateCalls, 1);
        EXPECT_EQ(app.RenderCalls, 1);
        // No window exists, so the pump runs no frame and the step ticks once.
        EXPECT_TRUE(app.PollEventsAndTick());
        EXPECT_EQ(app.UpdateCalls, 2);
        EXPECT_EQ(app.RenderCalls, 2);
        // Third Update() requests exit mid-frame; the frame completes and
        // Tick() reports "don't continue".
        EXPECT_FALSE(app.Tick());

        EXPECT_EQ(app.UpdateCalls, 3);
        EXPECT_EQ(app.RenderCalls, 3);
        EXPECT_EQ(app.GetFrameCount(), 3u);

        // Once exited, further ticks are inert.
        EXPECT_FALSE(app.Tick());
        EXPECT_FALSE(app.PollEventsAndTick());
        EXPECT_EQ(app.UpdateCalls, 3);

        app.Shutdown();
    }

    std::error_code ec;
    fs::remove_all(tempRoot, ec);
}

// The per-frame [FrameStartup] phase timings belong to the GE_FRAME_TRACE
// instrument, so a run that did not ask for the trace reports none of them.
// IsFrameTraceEnabled latches its env read on first use, so only the unset arm
// is testable in-process; the armed arm is a live check.
TEST(ApplicationTick, StartupFrameTimingsNeedTheFrameTraceSwitch)
{
    const char* frameTrace = std::getenv("GE_FRAME_TRACE");
    if (frameTrace != nullptr && *frameTrace != '\0' && std::string(frameTrace) != "0")
        GTEST_SKIP() << "GE_FRAME_TRACE is armed in this environment; this test asserts the "
                        "unset arm";

    const fs::path tempRoot = fs::temp_directory_path() / "ge_application_frame_trace_test";
    fs::create_directories(tempRoot / "Assets");

    GameEngine::ScriptsConfig scriptsConfig{};
    scriptsConfig.disableClr = true;
    scriptsConfig.enableHotReload = false;
    scriptsConfig.enableAsyncHotReload = false;
    scriptsConfig.enableAutoProjectGeneration = false;
    GameEngine::EngineCore::GetInstance().SetScriptsConfig(scriptsConfig);

    GameEngine::ApplicationConfig cfg{};
    cfg.Name = "TickContract.FrameTrace";
    cfg.WorkspaceDirectory = tempRoot.string();
    cfg.AssetDirectory = "Assets";

    std::vector<std::string> lines;
    {
        GameEngine::TestLog::ScopedEngineLogCapture capture(&lines);
        // A capture that sees nothing is indistinguishable from a line that was
        // never emitted: prove the sink live before reading a zero out of it.
        Logger::Log::Info("ApplicationTickTests: capture sink live");

        CountingApp app(cfg);
        app.ExitAfterUpdates = 3;
        // A native resize notification can arrive during the frame itself.
        // Its refresh must not run a second update/render inside this frame.
        app.RequestNestedTick = true;
        ASSERT_TRUE(app.Initialize());
        app.Tick();
        app.Tick();
        app.Tick();
        app.Shutdown();
        Logger::Log::Flush();
    }

    ASSERT_EQ(GameEngine::TestLog::CountLinesContaining(lines, "capture sink live"), 1u)
        << "log capture saw nothing at all — the [FrameStartup] zero below would be meaningless";
    EXPECT_EQ(GameEngine::TestLog::CountLinesContaining(lines, "[FrameStartup]"), 0u)
        << "frame phase timings must stay behind GE_FRAME_TRACE";

    std::error_code ec;
    fs::remove_all(tempRoot, ec);
}

// There is no default frame limiter. VSync paces through present (a Player's
// game.config window.vsync goes to the swapchain), and a game that turns VSync
// off asked for frames as fast as they come, so without GE_FRAME_CAP no frame of
// a non-editor application waits on the CPU.
TEST(ApplicationTick, NoFrameWaitsWithoutTheFrameCapSwitch)
{
    ScopedFrameCapEnv frameCap(nullptr);
    EXPECT_EQ(CountFramesHeldByTheLimiter("TickContract.NoFrameCap", 5), 0)
        << "a non-editor application with GE_FRAME_CAP unset must not throttle";
}

// GE_FRAME_CAP is the one way to ask for a limiter, and it still holds the
// frame. This is also the live control for the zero above: a frame the limiter
// held reports its wait in SleepMs.
TEST(ApplicationTick, FrameCapSwitchHoldsTheFrame)
{
    ScopedFrameCapEnv frameCap("30");
    EXPECT_GT(CountFramesHeldByTheLimiter("TickContract.FrameCapSwitch", 5), 0)
        << "GE_FRAME_CAP=30 must hold frames that do no work";
}

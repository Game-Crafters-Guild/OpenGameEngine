// Editor teardown is a no-heartbeat window: nothing calls NotifyAlive between
// the last frame and process exit. These tests lock the three parts of
// watching it — the watchdog's shutdown clock, the capture policy that keeps
// the diagnostic from suspending a main thread mid-teardown, and main.cpp
// keeping the watchdog alive across Application::Shutdown — and the job
// system's occupancy in a frame stall's report, which teardown reports leave
// out because teardown destroys the pool.

#include <gtest/gtest.h>

#include "Diagnostics/MainThreadHangWatchdog.h"
#include "JobSystem/JobChannel.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{

using GameEngine::Editor::MainThreadHangWatchdog;

void SetEnvVar(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

// Stop() has to run even when an assertion returns early. A live watchdog
// thread outlives its test, and Start() is a no-op while one is already
// running — so the next test silently inherits this one's thresholds and
// receives its stall reports, failing for a reason that is not its own.
struct ScopedWatchdog
{
    explicit ScopedWatchdog(const JobSystem::WorkStealingThreadPool* jobSystem = nullptr)
    {
        MainThreadHangWatchdog::Start(jobSystem);
    }
    ~ScopedWatchdog() { MainThreadHangWatchdog::Stop(); }
    ScopedWatchdog(const ScopedWatchdog&) = delete;
    ScopedWatchdog& operator=(const ScopedWatchdog&) = delete;
};

// Collects every line the watchdog logs. The sink outlives the test (the
// logger owns it); the callback is unregistered so nothing dangles.
class WatchdogLogCapture
{
  public:
    WatchdogLogCapture()
    {
        // Without this the logger's effective level is Off and nothing the
        // watchdog reports would reach any sink.
        Logger::Log::Initialize({Logger::LogLevel::Debug, false});
        auto sink = Logger::MakeUnique<Logger::CallbackSink>();
        m_Sink = sink.get();
        Logger::Log::AddSink(std::move(sink));
        m_CallbackId = m_Sink->RegisterCallback(
            [this](const Logger::LogMessage& message)
            {
                if (message.Message.find("[HangWatchdog]") == std::string::npos)
                    return;
                std::lock_guard<std::mutex> lock(m_Mutex);
                m_Lines.push_back(message.Message);
            });
    }

    ~WatchdogLogCapture() { m_Sink->UnregisterCallback(m_CallbackId); }

    std::vector<std::string> Lines() const
    {
        Logger::Log::Flush();
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Lines;
    }

  private:
    Logger::CallbackSink* m_Sink = nullptr;
    Logger::uint64 m_CallbackId = 0;
    mutable std::mutex m_Mutex;
    std::vector<std::string> m_Lines;
};

std::string ReadEditorMainCpp()
{
    const std::filesystem::path path =
        std::filesystem::path(GE_EDITOR_SOURCE_DIR) / "Source" / "main.cpp";
    std::ifstream file(path);
    EXPECT_TRUE(file.is_open()) << "cannot read " << path.string();
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// A latch the test opens; jobs park on it until then.
class Gate
{
  public:
    void Open()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Open = true;
        m_Changed.notify_all();
    }
    void Wait()
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        m_Changed.wait(lock, [this] { return m_Open; });
    }

  private:
    std::mutex m_Mutex;
    std::condition_variable m_Changed;
    bool m_Open = false;
};

// Opens the gate on every exit path, so a failed assertion never leaves a
// parked job holding a thread the pool's destructor joins.
struct GateOpener
{
    Gate& Target;
    ~GateOpener() { Target.Open(); }
};

// The stall report, wherever it sits among banner lines.
std::string FindStallLine(const std::vector<std::string>& lines)
{
    for (const std::string& line : lines)
        if (line.find("unresponsive") != std::string::npos)
            return line;
    return {};
}

} // namespace

// The teardown clock: no heartbeat arrives after BeginShutdownWatch, so the
// whole shutdown is one stall and only the shutdown threshold decides when it
// is reported. Before it is called, the frame threshold governs — a long
// pre-shutdown stall under that threshold must stay silent.
TEST(MainThreadHangWatchdogTest, ShutdownWatchReportsAStalledTeardown)
{
    SetEnvVar("GE_HANG_WATCHDOG", "1");
    SetEnvVar("GE_HANG_WATCHDOG_MS", "100000");
    SetEnvVar("GE_HANG_WATCHDOG_SHUTDOWN_MS", "400");
    SetEnvVar("GE_HANG_WATCHDOG_SHUTDOWN_STACKS", "");

    WatchdogLogCapture capture;
    ScopedWatchdog watchdog;
    MainThreadHangWatchdog::NotifyAlive();

    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    const std::vector<std::string> beforeShutdown = capture.Lines();
    EXPECT_TRUE(beforeShutdown.empty())
        << "reported below the frame threshold: " << beforeShutdown.front();

    MainThreadHangWatchdog::BeginShutdownWatch();
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    MainThreadHangWatchdog::Stop();

    const std::vector<std::string> lines = capture.Lines();
    ASSERT_FALSE(lines.empty()) << "teardown stall went unreported";
    EXPECT_NE(lines.front().find("unresponsive"), std::string::npos) << lines.front();
    EXPECT_NE(lines.front().find("phase: shutdown"), std::string::npos) << lines.front();
}

// A teardown report must not suspend the main thread by default. The capture
// runs dbghelp inside the suspend window, and dbghelp allocates and touches the
// loader: freezing a main thread that holds the CRT heap or loader lock — which
// teardown does constantly — deadlocks the diagnostic against the shutdown it
// is watching. So the default report carries no stack, and says how to get one.
TEST(MainThreadHangWatchdogTest, TeardownStackCaptureIsOffUnlessArmed)
{
#ifndef _WIN32
    GTEST_SKIP() << "in-process stack capture is implemented only under _WIN32 "
                    "(MainThreadHangWatchdog.cpp); off Windows the stall report "
                    "carries no stack and has no capture to arm";
#endif
    SetEnvVar("GE_HANG_WATCHDOG", "1");
    SetEnvVar("GE_HANG_WATCHDOG_MS", "100000");
    SetEnvVar("GE_HANG_WATCHDOG_SHUTDOWN_MS", "400");
    SetEnvVar("GE_HANG_WATCHDOG_SHUTDOWN_STACKS", "");

    WatchdogLogCapture capture;
    ScopedWatchdog watchdog;
    MainThreadHangWatchdog::NotifyAlive();
    MainThreadHangWatchdog::BeginShutdownWatch();

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    MainThreadHangWatchdog::Stop();

    const std::string stall = FindStallLine(capture.Lines());
    ASSERT_FALSE(stall.empty()) << "teardown stall went unreported";
    EXPECT_EQ(stall.find("main-thread stack:"), std::string::npos)
        << "teardown suspended the main thread by default: " << stall;
    EXPECT_NE(stall.find("GE_HANG_WATCHDOG_SHUTDOWN_STACKS=1"), std::string::npos)
        << "the report does not say how to get a stack: " << stall;
}

// Armed, the capture is back — and says so, naming the deadlock it risks. An
// opt-in hazard that arms silently is the same defect as one that is on by
// default: nobody reading the log knows the diagnostic can be the cause.
TEST(MainThreadHangWatchdogTest, ArmedTeardownCaptureNamesItsHazard)
{
#ifndef _WIN32
    GTEST_SKIP() << "in-process stack capture is implemented only under _WIN32 "
                    "(MainThreadHangWatchdog.cpp); off Windows the stall report "
                    "carries no stack and has no capture to arm";
#endif
    SetEnvVar("GE_HANG_WATCHDOG", "1");
    SetEnvVar("GE_HANG_WATCHDOG_MS", "100000");
    SetEnvVar("GE_HANG_WATCHDOG_SHUTDOWN_MS", "400");
    SetEnvVar("GE_HANG_WATCHDOG_SHUTDOWN_STACKS", "1");

    WatchdogLogCapture capture;
    ScopedWatchdog watchdog;
    MainThreadHangWatchdog::NotifyAlive();
    MainThreadHangWatchdog::BeginShutdownWatch();

    const std::vector<std::string> armLines = capture.Lines();
    ASSERT_FALSE(armLines.empty()) << "arming the teardown capture logged nothing";
    EXPECT_NE(armLines.front().find("GE_HANG_WATCHDOG_SHUTDOWN_STACKS=1"),
              std::string::npos)
        << armLines.front();
    EXPECT_NE(armLines.front().find("deadlock"), std::string::npos)
        << "the arming line does not name the hazard: " << armLines.front();

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    MainThreadHangWatchdog::Stop();

    const std::string stall = FindStallLine(capture.Lines());
    ASSERT_FALSE(stall.empty()) << "teardown stall went unreported";
    EXPECT_NE(stall.find("main-thread stack:"), std::string::npos)
        << "armed, but no stack captured: " << stall;

    SetEnvVar("GE_HANG_WATCHDOG_SHUTDOWN_STACKS", "");
}

// The other side of the same switch: a healthy shutdown is silent. Teardown is
// seconds of heartbeat-free work, so the frame threshold would report every
// quit as a hang — BeginShutdownWatch has to move off it, not just relabel the
// phase.
TEST(MainThreadHangWatchdogTest, HealthyTeardownIsSilentUnderTheDefaultThreshold)
{
    SetEnvVar("GE_HANG_WATCHDOG", "1");
    SetEnvVar("GE_HANG_WATCHDOG_MS", "300");
    SetEnvVar("GE_HANG_WATCHDOG_SHUTDOWN_MS", "");
    SetEnvVar("GE_HANG_WATCHDOG_SHUTDOWN_STACKS", "");

    WatchdogLogCapture capture;
    ScopedWatchdog watchdog;
    MainThreadHangWatchdog::NotifyAlive();
    MainThreadHangWatchdog::BeginShutdownWatch();

    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    MainThreadHangWatchdog::Stop();

    const std::vector<std::string> lines = capture.Lines();
    EXPECT_TRUE(lines.empty()) << "healthy teardown reported as a hang: " << lines.front();
}

// The watchdog only covers teardown if main tears down before stopping it.
// Leaving Shutdown to ~EditorApplication (which runs after EditorMainImpl
// returns) puts the entire shutdown outside the window, which is how a
// shutdown wedge came to be reported by nothing at all — just a log that
// stops mid-line.
TEST(MainThreadHangWatchdogTest, EditorMainShutsDownBeforeStoppingTheWatchdog)
{
    const std::string source = ReadEditorMainCpp();

    const size_t shutdown = source.find("app.Shutdown()");
    const size_t stop = source.find("MainThreadHangWatchdog::Stop()");
    ASSERT_NE(shutdown, std::string::npos)
        << "main.cpp no longer shuts the application down explicitly";
    ASSERT_NE(stop, std::string::npos) << "main.cpp no longer stops the watchdog";
    EXPECT_LT(shutdown, stop) << "teardown runs outside the watchdog window";

    EXPECT_NE(source.find("MainThreadHangWatchdog::BeginShutdownWatch()"), std::string::npos)
        << "teardown is watched on the frame threshold, not the shutdown one";
}

// A frame stall's report names what every thread of the job system runs: a
// Background job parked on one worker and a channel job parked on a blocking
// thread show with how long they have run, the other worker as idle. Read
// lock-free from the pool while the main thread is stalled.
TEST(MainThreadHangWatchdogTest, StallReportNamesWhatEachJobSystemThreadRuns)
{
    SetEnvVar("GE_HANG_WATCHDOG", "1");
    SetEnvVar("GE_HANG_WATCHDOG_MS", "300");
    SetEnvVar("GE_HANG_WATCHDOG_SHUTDOWN_MS", "");
    SetEnvVar("GE_HANG_WATCHDOG_SHUTDOWN_STACKS", "");

    JobSystem::WorkStealingThreadPool pool(2);
    JobSystem::JobChannel channel(pool, {.Name = "Watchdog test builds", .MaxRunning = 1});
    Gate gate;
    GateOpener opener{gate};
    std::atomic<int> parked{0};
    pool.EnqueueWork(
        [&]
        {
            parked.fetch_add(1);
            gate.Wait();
        },
        JobSystem::JobPriority::Background);
    channel.Enqueue(
        [&]
        {
            parked.fetch_add(1);
            gate.Wait();
        });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (parked.load() < 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(parked.load(), 2);

    WatchdogLogCapture capture;
    {
        ScopedWatchdog watchdog(&pool);
        MainThreadHangWatchdog::NotifyAlive();
        std::this_thread::sleep_for(std::chrono::milliseconds(900));
    }

    const std::string stall = FindStallLine(capture.Lines());
    ASSERT_FALSE(stall.empty()) << "the stall went unreported";
    EXPECT_NE(stall.find("job system threads:"), std::string::npos) << stall;
    EXPECT_NE(stall.find(": Background for "), std::string::npos) << stall;
    EXPECT_NE(stall.find("Job Blocking #0: Watchdog test builds for "), std::string::npos) << stall;
    EXPECT_NE(stall.find(": idle"), std::string::npos) << stall;
    gate.Open();
}

// Teardown destroys the job system, so a teardown report must not read it:
// BeginShutdownWatch detaches the pool and the report carries no occupancy.
TEST(MainThreadHangWatchdogTest, TeardownReportLeavesTheJobSystemOut)
{
    SetEnvVar("GE_HANG_WATCHDOG", "1");
    SetEnvVar("GE_HANG_WATCHDOG_MS", "100000");
    SetEnvVar("GE_HANG_WATCHDOG_SHUTDOWN_MS", "400");
    SetEnvVar("GE_HANG_WATCHDOG_SHUTDOWN_STACKS", "");

    JobSystem::WorkStealingThreadPool pool(1);
    WatchdogLogCapture capture;
    {
        ScopedWatchdog watchdog(&pool);
        MainThreadHangWatchdog::NotifyAlive();
        MainThreadHangWatchdog::BeginShutdownWatch();
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    }

    const std::string stall = FindStallLine(capture.Lines());
    ASSERT_FALSE(stall.empty()) << "teardown stall went unreported";
    EXPECT_EQ(stall.find("job system threads:"), std::string::npos)
        << "a teardown report read the job system: " << stall;
}

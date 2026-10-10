#include "Core/Application.h"
#include "Core/Engine.h"
#include "Logger/Logger.h"
#include "Logger/FileSink.h"
#include "Input/GamepadEdgeTracker.h"
#include "Input/GamepadFrame.h"
#include "Input/GamepadPolling.h"
#include "Input/InputSystem.h"
#include "Platform/Window.h"
#include "Core/CpuProfiler.h"
#include "Core/DebugMetrics.h"
#include "Core/NvtxRange.h"
#include "Core/Time.h"
#include <chrono>
#include <optional>
#include <thread>
#include <utility>
#include <cstdlib>

namespace
{
    // Number of leading frames whose phase timings the trace reports, enough to
    // cover engine init settling into a steady-state frame.
    constexpr std::uint64_t kFrameStartupTraceFrames = 20;

    // Debug-only helper controlled by GE_FRAME_TRACE env var. When enabled,
    // we emit a small, structured trace of the high-level frame loop order
    // (input -> app update -> engine update -> render) plus the leading frames'
    // phase timings, to help diagnose gizmo/ECS/render latency issues without
    // spamming logs in normal runs.
    inline bool IsFrameTraceEnabled()
    {
        static bool s_Enabled = []() {
            const char* env = std::getenv("GE_FRAME_TRACE");
            if (!env || !*env)
            {
                return false;
            }

            // Treat "0" as explicit off; anything else enables tracing.
            return !(env[0] == '0' && env[1] == '\0');
        }();
        return s_Enabled;
    }
}

namespace GameEngine {

Application* Application::s_Instance = nullptr;

Application* Application::Get()
{
    return s_Instance;
}

Application::Application(const ApplicationConfig& config)
    : m_Config(config)
    , m_Initialized(false)
    , m_ShouldExit(false)
    , m_DeltaTime(0.0)
    , m_TotalTime(0.0)
    , m_FrameCount(0)
{
    // First-wins: typical hosts (Editor, Player) construct exactly one
    // Application. Tests / multi-instance hosts can still construct
    // additional ones; they just won't be exposed via Get().
    if (!s_Instance)
        s_Instance = this;

    m_ApplicationInputChain.getInput = [this]() { return m_InputSystem.get(); };
    m_GamepadEdgeTracker = std::make_unique<Input::GamepadEdgeTracker>();
}

Application::~Application() {
    if (s_Instance == this)
        s_Instance = nullptr;
    if (m_Initialized) {
        Shutdown();
    }
}

bool Application::Initialize() {
    // Auto-initialize logger at Debug level in debug builds if not already configured
#ifdef _DEBUG
    if (Logger::Log::GetSinkCount() == 0) {
	        Logger::Log::Config logCfg;
	        logCfg.GlobalMinLevel = Logger::LogLevel::Debug;
	        Logger::Log::Initialize(logCfg);
	        Logger::Log::Info("Logger auto-initialized (Debug)");

	    #if LOGGER_ENABLE_FILE_LOGGING
	        // Optional file logging controlled via GE_LOGFILE environment variable.
	        // This is primarily used by the Editor to capture IoL Console.WriteLine output.
	        const char* logFileEnv = std::getenv("GE_LOGFILE");
	        if (logFileEnv && *logFileEnv) {
	            Logger::FileSink::Config fileCfg;
	            fileCfg.filename = logFileEnv;
	            fileCfg.append = true;
	            fileCfg.minLevel = logCfg.GlobalMinLevel;
	            fileCfg.includeTimestamp = true;
	            fileCfg.includeSource = logCfg.EnableSourceLocation;
	            Logger::Log::AddSink(Logger::MakeUnique<Logger::FileSink>(fileCfg));
	            Logger::Log::Info("File logging enabled: {}", fileCfg.filename);
	        }
	    #endif // LOGGER_ENABLE_FILE_LOGGING
    }
#endif

    Logger::Log::Info("Initializing application: {}", m_Config.Name);

    // Resolve the NVTX gate before the engine spins up any worker threads: the
    // flag it writes is read unsynchronized by ranges on every thread.
    GameEngine::Profiling::InitializeNvtx();

	    // Initialize engine
	    if (!EngineCore::GetInstance().Initialize(m_Config)) {
        Logger::Log::Error("Failed to initialize engine");
        return false;
    }

	    // Create a shared InputSystem instance for the application. Derived
	    // applications (Editor, games) can configure actions/contexts via
	    // GetInputSystem(), while the main loop handles per-frame pumping.
	    m_InputSystem = std::make_unique<Input::InputSystem>();
	    // Expose input to Engine-level services (e.g. ScriptingABI) via a non-owning pointer.
	    EngineCore::GetInstance().SetInputSystem(m_InputSystem.get());

    // Record start time
    m_StartTime = std::chrono::high_resolution_clock::now();
    m_LastFrameTime = m_StartTime;

    m_Initialized = true;
    Logger::Log::Info("Application initialized successfully");
    return true;
}

int Application::Run() {
    if (!m_Initialized) {
        Logger::Log::Error("Application not initialized");
        return -1;
    }

    Logger::Log::Info("Starting main loop");
    RunPlatformLoop();

    Logger::Log::Info("Main loop ended");
    return m_ExitCode;
}

void Application::RunPlatformLoop() {
    while (PollEventsAndTick()) {
    }
}

bool Application::PollEventsAndTick() {
    if (m_ShouldExit) return false;
    // Native refresh callbacks run whole frames inside the pump: an uncovered
    // window, and every step of a live resize while the OS holds the pump. A
    // frame that completed there is this step's frame; ticking again would
    // render twice for one pump.
    const uint64 framesBeforePump = m_FrameCount;
    Platform::Window::PollEvents();
    if (m_FrameCount != framesBeforePump) return !m_ShouldExit;
    return Tick();
}

void Application::Shutdown() {
    if (!m_Initialized) {
        return;
    }

    Logger::Log::Info("Shutting down application");

    // Clear the Engine-level non-owning pointer to application-owned input.
    EngineCore::GetInstance().SetInputSystem(nullptr);
    
    // Call derived class shutdown
    OnShutdown();
    
    // Shutdown engine
    EngineCore::GetInstance().Shutdown();
    
    m_Initialized = false;
    Logger::Log::Info("Application shutdown complete");
}

void Application::RouteGamepadPoll()
{
    // A gamepad is read here, beside the window event pump, because the platform
    // exposes it as state rather than as callbacks: one read per frame is the
    // whole device's event source. What its buttons did since the previous read
    // enters the same chain a key does, so the game consumes what it claims and
    // the application's own InputSystem sees only what the game left; axes and
    // connection go to every stage, consumable by none.
    Input::GamepadFrame frame;
    Input::ReadGamepadFrame(frame);

    // Named on the first thing there is to route rather than up front: a poll
    // with no pad in any slot, now or a frame ago, reports nothing, and naming
    // the chain costs the editor a walk of its window list every frame for a
    // device nobody has plugged in. One answer serves the whole poll, since
    // nothing a stage does can change the window list inside it.
    const WindowInputRouterConfig* chain = nullptr;
    auto resolveChain = [this, &chain]() -> const WindowInputRouterConfig&
    {
        if (!chain)
            chain = &GamepadInputChain();
        return *chain;
    };

    m_GamepadEdgeTracker->Report(
        frame,
        [&resolveChain](int gamepadIndex, const float* axes, int axisCount, bool connected)
        { WindowInputRouter::RouteGamepadState(resolveChain(), gamepadIndex, axes, axisCount, connected); },
        [&resolveChain](int gamepadIndex, Input::GamepadButton button, bool down)
        { WindowInputRouter::RouteGamepadButton(resolveChain(), gamepadIndex, static_cast<int>(button), down); });
}

bool Application::Tick() {
    if (m_ShouldExit) return false;
    // A frame may itself trigger native refresh notifications. Never nest an
    // update or GPU submission inside an in-progress frame.
    if (m_Ticking) return true;
    struct TickScope
    {
        bool& active;
        explicit TickScope(bool& value) : active(value) { active = true; }
        ~TickScope() { active = false; }
    } scope(m_Ticking);
    {
        UpdateTiming();
        // Publish the frame delta to the global clock so any subsystem or user
        // script can read Time::GetDeltaTime() without threading it through calls.
        Time::Detail::SetFrameDeltaTime(static_cast<float>(m_DeltaTime));

        // Top-level NVTX range every engine scope nests under. Held in an
        // optional rather than a `{}` block because the frame body declares
        // locals (phases, trace, frameIndex) that outlive it, and it must be
        // closed explicitly before the frame-rate limiter below so the throttle
        // sleep shows in Nsight as the gap between Frame ranges instead of
        // inflating the frame itself.
        std::optional<GameEngine::Profiling::ScopedNvtxRange> frameRange{std::in_place, "Frame"};

        // Optional CPU profiler: closes the frame that just finished — that is
        // the frame its readers see — and opens the one starting here.
        GameEngine::Profiling::CpuProfiler::Get().BeginFrame();

        // Capture coarse CPU timings for the main frame phases (stored for the next frame's UI).
        auto MsBetween = [](const auto& a, const auto& b) -> double
        {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        FramePhaseTimings phases{};

	        const bool trace = IsFrameTraceEnabled();
	        const std::uint64_t frameIndex = static_cast<std::uint64_t>(m_FrameCount);

	        if (trace)
	        {
	            Logger::Log::Debug(
	                "[FrameTrace] frame={} phase=BeginFrame dt={} totalTime={}",
	                frameIndex,
	                m_DeltaTime,
	                m_TotalTime);
	        }

	        // 1) Route polled gamepad state and update high-level input.
	        //    This ensures that input for the current frame is visible to
	        //    both application logic and engine systems in the same frame.
	        const auto tPoll0 = std::chrono::high_resolution_clock::now();
	        RouteGamepadPoll();
	        const auto tPoll1 = std::chrono::high_resolution_clock::now();
	        phases.PollEventsMs = MsBetween(tPoll0, tPoll1);
	        if (m_InputSystem)
	        {
	            const auto tIn0 = std::chrono::high_resolution_clock::now();
	            m_InputSystem->Update(static_cast<float>(m_DeltaTime));
	            const auto tIn1 = std::chrono::high_resolution_clock::now();
	            phases.InputMs = MsBetween(tIn0, tIn1);
	        }

	        if (trace)
	        {
	            Logger::Log::Debug(
	                "[FrameTrace] frame={} phase=InputUpdated",
	                frameIndex);
	        }

	        // 2) Let the application process input-driven game/editor logic.
	        //    Any user-driven changes to the ECS world (gizmos, gameplay
	        //    code, editor tools) are applied here before engine systems run.
	        const auto tApp0 = std::chrono::high_resolution_clock::now();
	        Update(m_DeltaTime);
	        const auto tApp1 = std::chrono::high_resolution_clock::now();
	        phases.AppUpdateMs = MsBetween(tApp0, tApp1);

	        if (trace)
	        {
	            Logger::Log::Debug(
	                "[FrameTrace] frame={} phase=Application.UpdateComplete",
	                frameIndex);
	        }

	        // 3) Update engine systems (including the optional engine-managed
	        //    rendering loop when enabled). This now consumes the latest
	        //    world state produced by the application, so rendering and
	        //    other engine systems operate on up-to-date data in the same
	        //    frame as the input/logic that produced it.
	        //    Ordering contract with step 4: the synchronous job waves engine
	        //    systems dispatch here (ECS extraction, mesh/material upload
	        //    chains) join before Update returns, so the device-recovery tick
	        //    inside Render() below never tears the device down under one of
	        //    those workers mid-create. Genuinely asynchronous workers
	        //    (streaming texture uploads) are not covered by this ordering and
	        //    rely on the device's rebuild lock instead; VulkanDevice also
	        //    enforces the same exclusion for every allocator-touching entry
	        //    (m_DeviceRebuildMutex), so this frame structure orders work for
	        //    coherence, not for device-lifetime safety.
	        const auto tEng0 = std::chrono::high_resolution_clock::now();
	        EngineCore::GetInstance().Update(m_DeltaTime);
	        const auto tEng1 = std::chrono::high_resolution_clock::now();
	        phases.EngineUpdateMs = MsBetween(tEng0, tEng1);

	        if (trace)
	        {
	            Logger::Log::Debug(
	                "[FrameTrace] frame={} phase=Engine.UpdateComplete",
	                frameIndex);
	        }

	        // 4) Render the frame.
	        const auto tRen0 = std::chrono::high_resolution_clock::now();
	        Render();
	        const auto tRen1 = std::chrono::high_resolution_clock::now();
	        phases.RenderMs = MsBetween(tRen0, tRen1);

	        // Early frame timings, for diagnosing a post-Initialize white-screen
	        // delay. Behind GE_FRAME_TRACE with the phase trace above: same
	        // frame-loop diagnostic, same audience.
	        if (trace && frameIndex < kFrameStartupTraceFrames)
	        {
	            Logger::Log::Info("[FrameStartup] frame={} Poll={:.1f}ms Input={:.1f}ms AppUpdate={:.1f}ms EngineUpdate={:.1f}ms Render={:.1f}ms TOTAL={:.1f}ms",
	                             frameIndex,
	                             phases.PollEventsMs,
	                             phases.InputMs,
	                             phases.AppUpdateMs,
	                             phases.EngineUpdateMs,
	                             phases.RenderMs,
	                             MsBetween(tPoll0, tRen1));
	        }

	        // Optional CPU profiler: dump on request (hotkey-controlled by editor).
	        // We dump here (before sleep) so the report reflects actual work, not throttling.
	        GameEngine::Profiling::CpuProfiler::Get().EndFrameAndTryDumpToLog(
	            [](std::string_view line)
	            {
	                Logger::Log::Info("{}", line);
	            });

	        if (trace)
	        {
	            Logger::Log::Debug(
	                "[FrameTrace] frame={} phase=Application.RenderComplete",
	                frameIndex);
	        }

	        m_FrameCount++;

	        // Close the frame's NVTX range before any throttling below.
	        frameRange.reset();

        // Frame rate limiting, only on request: GE_FRAME_CAP set to a positive
        // rate (e.g. 60, 120) holds each frame to it with a CPU wait. Unset,
        // empty, 0 or unparsable means no limiter, in every application: with
        // VSync on, present paces the frame, and an application that turns VSync
        // off asked for uncapped frames. A fixed frame rate (SetFixedFrameRate)
        // ignores GE_FRAME_CAP.
        //
        // A plain sleep under a coarse timer (common on Windows) quantizes to
        // ~15.6 ms and turns a 60 fps cap into 30 to 40 fps, so the wait below
        // sleeps most of the remainder and yields for the tail.
        double targetFps = 0.0;
        if (!IsFixedFrameRateEnabled())
        {
            if (const char* capEnv = std::getenv("GE_FRAME_CAP"))
            {
                if (capEnv[0] != '\0')
                {
                    char* end = nullptr;
                    const double v = std::strtod(capEnv, &end);
                    if (end != capEnv)
                    {
                        targetFps = (v > 0.0) ? v : 0.0;
                    }
                }
            }
        }

        if (targetFps > 0.0)
        {
            const double targetFrameTime = 1.0 / targetFps;
            // Measure the *current* frame time since UpdateTiming() (start of this loop iteration).
            const auto now = std::chrono::high_resolution_clock::now();
            const double frameTime = std::chrono::duration<double>(now - m_LastFrameTime).count();
            if (frameTime < targetFrameTime)
            {
                const double remaining = targetFrameTime - frameTime;
                const auto tSleep0 = std::chrono::high_resolution_clock::now();
                // Sleep most of the remaining time, then spin/yield for the tail to reduce oversleep.
                if (remaining > 0.002)
                {
                    std::this_thread::sleep_for(std::chrono::duration<double>(remaining - 0.001));
                }
                while (std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - m_LastFrameTime).count() < targetFrameTime)
                {
                    std::this_thread::yield();
                }
                const auto tSleep1 = std::chrono::high_resolution_clock::now();
                phases.SleepMs = MsBetween(tSleep0, tSleep1);
            }
        }

        // Publish last-frame phases for consumers (Editor overlay, diagnostics).
        m_LastFramePhases = phases;

        auto& metrics = Debug::DebugMetrics::Get();
        metrics.PushSample("Time/Process",  static_cast<float>(phases.AppUpdateMs + phases.EngineUpdateMs));
        metrics.PushSample("Time/Render",   static_cast<float>(phases.RenderMs));
        metrics.PushSample("Time/Input",    static_cast<float>(phases.InputMs));
        metrics.PushSample("Time/PollEvents", static_cast<float>(phases.PollEventsMs));
        metrics.PushSample("Time/Sleep",    static_cast<float>(phases.SleepMs));
    }
    return !m_ShouldExit;
}

void Application::UpdateTiming() {
    auto currentTime = std::chrono::high_resolution_clock::now();

    if (m_FixedDeltaTime > 0.0)
    {
        m_DeltaTime = m_FixedDeltaTime;
        m_TotalTime = static_cast<float64>(m_FrameCount) * m_FixedDeltaTime;
        m_LastFrameTime = currentTime;
        return;
    }

    auto deltaTimeDuration = std::chrono::duration<float64>(currentTime - m_LastFrameTime);
    m_DeltaTime = deltaTimeDuration.count();

    auto totalTimeDuration = std::chrono::duration<float64>(currentTime - m_StartTime);
    m_TotalTime = totalTimeDuration.count();

    m_LastFrameTime = currentTime;

    // Sliding-window FPS: average completed-frame intervals over the last
    // 250 ms so the display reacts quickly without becoming raw 1/dt noise.
    // Spike-resistant — a 4-second stall just means the window has 1 entry
    // for that period; FPS recovers as soon as new fast frames roll in,
    // unlike an avg-of-dt approach where one bad sample sticky-drags the
    // displayed value through the whole next averaging window.
    //
    // Fixed-capacity ring buffer instead of std::deque to avoid Debug-build
    // bookkeeping and any heap touches on the per-frame hot path. The head
    // pointer wraps modulo kFrameWindowCapacity; we count entries valid
    // for the [head - count, head) range and age out the tail by stepping
    // count down while the oldest timestamp is outside the display window.
    constexpr auto kWindow = std::chrono::milliseconds(250);
    const float frameMs = static_cast<float>(m_DeltaTime * 1000.0);

    // Insert at head.
    m_FrameTimes[m_FrameRingHead] = currentTime;
    m_FrameMs[m_FrameRingHead]    = frameMs;
    m_FrameRingHead = (m_FrameRingHead + 1u) % kFrameWindowCapacity;
    if (m_FrameRingCount < kFrameWindowCapacity)
        ++m_FrameRingCount;

    // Age out tail entries older than the window. Tail = (head - count) mod cap.
    while (m_FrameRingCount > 0u)
    {
        const std::size_t tailIdx = (m_FrameRingHead + kFrameWindowCapacity - m_FrameRingCount)
                                     % kFrameWindowCapacity;
        if ((currentTime - m_FrameTimes[tailIdx]) <= kWindow)
            break;
        --m_FrameRingCount;
    }

    if (m_FrameRingCount >= 2u)
    {
        const std::size_t oldestIdx = (m_FrameRingHead + kFrameWindowCapacity - m_FrameRingCount)
                                      % kFrameWindowCapacity;
        const float sampleSpanSeconds =
            std::chrono::duration<float>(currentTime - m_FrameTimes[oldestIdx]).count();
        m_CurrentFps = sampleSpanSeconds > 1e-6f
            ? static_cast<float>(m_FrameRingCount - 1u) / sampleSpanSeconds
            : 0.0f;
    }
    else
    {
        m_CurrentFps = m_DeltaTime > 1e-6 ? static_cast<float>(1.0 / m_DeltaTime) : 0.0f;
    }

    if (m_FrameRingCount == 0u)
    {
        m_FrameMsWorst = frameMs;
        m_FrameMsMean  = frameMs;
    }
    else
    {
        // Walk the live entries by index — cheaper than range-for since
        // we'd need to wrap. Sum + max in one pass.
        float worst = 0.0f;
        float sum   = 0.0f;
        for (std::size_t i = 0; i < m_FrameRingCount; ++i)
        {
            const std::size_t idx = (m_FrameRingHead + kFrameWindowCapacity - m_FrameRingCount + i)
                                    % kFrameWindowCapacity;
            const float v = m_FrameMs[idx];
            if (v > worst) worst = v;
            sum += v;
        }
        m_FrameMsWorst = worst;
        m_FrameMsMean  = sum / static_cast<float>(m_FrameRingCount);
    }

    // Chart-facing series. FPS is a rate — only meaningful over a window —
    // so we push the sliding 250 ms estimate (same value as GetFps()). FrameMs
    // is per-frame work cost, so we push raw frameMs to keep individual
    // frame events visible.
    //
    // Pushing 1000/frameMs per frame would create a noisy "instantaneous FPS"
    // chart that spikes to >300 on a single fast frame and dips to 150 on a
    // single slow one, even though the sustained throughput is mid-200s. The
    // sliding estimate avoids that misreading.
    auto& metrics = Debug::DebugMetrics::Get();
    metrics.PushSample("Time/FPS", m_CurrentFps);
    metrics.PushSample("Time/FrameMs", frameMs);
}

void Application::SetFixedFrameRate(double fps)
{
    m_FixedDeltaTime = (fps > 0.0) ? (1.0 / fps) : 0.0;
}

void Application::ClearFixedFrameRate()
{
    m_FixedDeltaTime = 0.0;
}

} // namespace GameEngine

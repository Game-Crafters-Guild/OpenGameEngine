#include "Diagnostics/MainThreadHangWatchdog.h"

#include "JobSystem/JobSystemStatistics.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Backtrace.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <DbgHelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace GameEngine::Editor
{
namespace
{

using Clock = std::chrono::steady_clock;

constexpr int kPollIntervalMs = 200;
constexpr int kRestallLogIntervalMs = 5000;
constexpr int kDefaultThresholdMs = 2000;
// Teardown has no heartbeat to pace it, so the whole shutdown is one stall as
// far as the watchdog is concerned: the threshold has to clear a healthy
// shutdown end to end, not a frame.
constexpr int kDefaultShutdownThresholdMs = 10000;
constexpr int kMaxStackFrames = 48;

std::atomic<long long> g_LastBeatNs{0}; // 0 = not armed yet
std::atomic<const char*> g_Phase{nullptr};
std::atomic<bool> g_Running{false};
// Read by the loop every poll so BeginShutdownWatch can widen it in flight.
std::atomic<int> g_ThresholdMs{kDefaultThresholdMs};
std::atomic<int> g_ShutdownThresholdMs{kDefaultShutdownThresholdMs};
std::thread g_Thread;

const char* const kShutdownPhase = "shutdown";

// The job system whose occupancy a stall report carries, and the snapshot
// buffer sized for it at Start, so a report never allocates to read the pool.
// The loop holds the mutex across the read; BeginShutdownWatch and Stop take
// it to detach, so the pool is never read after either returns.
std::mutex g_JobSystemMutex;
const JobSystem::WorkStealingThreadPool* g_JobSystem = nullptr; // guarded by g_JobSystemMutex
// Sized by Start under g_JobSystemMutex before the loop thread exists, and
// afterwards filled and read only by the loop thread, which formats the
// entries after releasing the mutex.
std::vector<JobSystem::JobSystemStatistics::Occupancy> g_Occupancy;

void DetachJobSystem()
{
    std::lock_guard<std::mutex> lock(g_JobSystemMutex);
    g_JobSystem = nullptr;
}

// One line per pool thread, named as the thread is ("Job Worker #3"), with
// what it runs and, for Background and channel jobs, for how long. Empty when
// no job system is attached. A job that started after nowNs was read reports
// 0.0s, never a negative time.
std::string DescribeJobSystem(long long nowNs)
{
    size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(g_JobSystemMutex);
        if (g_JobSystem == nullptr)
            return {};
        count = g_JobSystem->SnapshotOccupancy(g_Occupancy);
    }
    // The entries are plain values once read; formatting them allocates,
    // which is fine here because the main thread is not suspended.
    std::string out = "\n  job system threads:";
    for (size_t i = 0; i < count; ++i)
    {
        const JobSystem::JobSystemStatistics::Occupancy& thread = g_Occupancy[i];
        const bool worker = thread.Kind == JobSystem::JobSystemStatistics::ThreadKind::Worker;
        out += "\n    ";
        out += worker ? "Job Worker #" : "Job Blocking #";
        out += std::to_string(thread.Index);
        out += ": ";
        if (thread.Running == nullptr)
        {
            out += "idle";
            continue;
        }
        out += thread.Running;
        if (thread.SinceNs != 0)
        {
            char seconds[48];
            const long long elapsedNs = std::max(0LL, nowNs - static_cast<long long>(thread.SinceNs));
            std::snprintf(seconds, sizeof(seconds), " for %.1fs", static_cast<double>(elapsedNs) / 1e9);
            out += seconds;
        }
    }
    return out;
}

// Env overrides share one shape: a positive integer, anything else ignored.
int ThresholdFromEnv(const char* name, int fallback)
{
    if (const char* env = std::getenv(name); env && env[0] != '\0')
    {
        const int v = std::atoi(env);
        if (v > 0)
            return v;
    }
    return fallback;
}

#ifdef _WIN32
// Opt-in flags share one shape: exactly "1" arms, anything else leaves the
// flag disarmed.
bool FlagFromEnv(const char* name)
{
    const char* env = std::getenv(name);
    return env && env[0] == '1' && env[1] == '\0';
}

// Whether a stall report may suspend the main thread and walk it. True for the
// frame window; teardown decides separately — see BeginShutdownWatch.
//
// Guarded by g_CaptureModeMutex rather than atomic, because reading it and
// acting on it must be one step: an atomic read would let the loop decide to
// capture, then block on the publish and suspend a main thread that has since
// entered teardown. The loop holds the mutex across decide+capture; the main
// thread holds it to publish. Deliberately NOT Logger::DbgHelpMutex(), which
// the main thread also takes (Error-log backtraces) — a main thread wedged
// while holding that one would then silence the report that names the wedge.
std::mutex g_CaptureModeMutex;
bool g_StackCaptureArmed = true; // guarded by g_CaptureModeMutex
// GE_HANG_WATCHDOG_SHUTDOWN_STACKS, resolved at Start().
std::atomic<bool> g_ShutdownStacksArmed{false};

HANDLE g_MainThread = nullptr; // duplicated handle, closed on Stop()

// Capture raw return addresses of the (suspended) main thread. StackWalk64
// runs against pre-warmed DbgHelp module tables inside the suspend window;
// symbolization happens after ResumeThread. The CALLER holds
// Logger::DbgHelpMutex() across suspend+walk+symbolize: dbghelp is
// process-global and single-threaded, and acquiring the mutex BEFORE
// SuspendThread means we can never freeze the main thread mid-CaptureBacktrace
// (Error-log backtraces) and then walk dbghelp's half-mutated state.
//
// Residual hazard: dbghelp's own callbacks can allocate and can touch the
// loader, so a main thread suspended while it holds the CRT heap or loader
// lock deadlocks this walk against it — permanently, because the only thread
// that can resume it is the one now blocked. The capture then converts a main
// thread that was merely slow into a process that never exits. The proper fix
// is a copy-context + RtlVirtualUnwind walk that keeps dbghelp entirely out of
// the suspend window; until then g_StackCaptureArmed bounds where the hazard
// is accepted, and teardown is outside that bound by default.
int CaptureMainThreadStack(void** frames, int maxFrames)
{
    if (SuspendThread(g_MainThread) == static_cast<DWORD>(-1))
        return 0;

    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    int count = 0;
    if (GetThreadContext(g_MainThread, &ctx))
    {
        STACKFRAME64 sf{};
        sf.AddrPC.Mode = AddrModeFlat;
        sf.AddrFrame.Mode = AddrModeFlat;
        sf.AddrStack.Mode = AddrModeFlat;
#if defined(_M_X64)
        const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
        sf.AddrPC.Offset = ctx.Rip;
        sf.AddrFrame.Offset = ctx.Rbp;
        sf.AddrStack.Offset = ctx.Rsp;
#elif defined(_M_ARM64)
        const DWORD machine = IMAGE_FILE_MACHINE_ARM64;
        sf.AddrPC.Offset = ctx.Pc;
        sf.AddrFrame.Offset = ctx.Fp;
        sf.AddrStack.Offset = ctx.Sp;
#else
#error Unsupported architecture for MainThreadHangWatchdog stack capture
#endif
        const HANDLE process = GetCurrentProcess();
        while (count < maxFrames)
        {
            if (!StackWalk64(machine, process, g_MainThread, &sf, &ctx, nullptr,
                             SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
                break;
            if (sf.AddrPC.Offset == 0)
                break;
            frames[count++] = reinterpret_cast<void*>(sf.AddrPC.Offset);
        }
    }

    ResumeThread(g_MainThread);
    return count;
}

std::string SymbolizeFrames(void** frames, int count)
{
    const HANDLE process = GetCurrentProcess();
    std::string out;
    alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 512];
    for (int i = 0; i < count; ++i)
    {
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;

        out += "\n    ";
        const DWORD64 addr = reinterpret_cast<DWORD64>(frames[i]);
        DWORD64 disp = 0;
        if (SymFromAddr(process, addr, &disp, sym))
        {
            out += sym->Name;
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisp = 0;
            if (SymGetLineFromAddr64(process, addr, &lineDisp, &line) && line.FileName)
            {
                out += "  [";
                out += line.FileName;
                out += ":";
                out += std::to_string(line.LineNumber);
                out += "]";
            }
        }
        else
        {
            char hex[24];
            std::snprintf(hex, sizeof(hex), "0x%016llx",
                          static_cast<unsigned long long>(addr));
            out += hex;
        }
    }
    return out;
}
#endif // _WIN32

long long NowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               Clock::now().time_since_epoch())
        .count();
}

void WatchdogLoop()
{
#ifdef _WIN32
    // Pre-warm DbgHelp so the suspend-window StackWalk64 hits loaded module
    // tables instead of allocating during first-touch module loads. Serialized
    // with every other dbghelp user; SymInitialize fails harmlessly when the
    // logger's backtrace path initialized the session first.
    {
        std::lock_guard<std::mutex> dbghelpLock(Logger::DbgHelpMutex());
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        SymRefreshModuleList(GetCurrentProcess());
    }
#endif

    bool inStall = false;
    long long stallStartNs = 0;
    long long lastReportNs = 0;
    const char* stallPhase = nullptr;

    while (g_Running.load(std::memory_order_relaxed))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));

        const long long beat = g_LastBeatNs.load(std::memory_order_relaxed);
        if (beat == 0)
            continue; // not armed until the first main-loop heartbeat

        const long long now = NowNs();
        const long long silentMs = (now - beat) / 1'000'000;

        if (silentMs < g_ThresholdMs.load(std::memory_order_relaxed))
        {
            if (inStall)
            {
                const double totalS =
                    static_cast<double>(now - stallStartNs) / 1e9;
                Logger::Log::Warning(
                    "[HangWatchdog] main thread recovered after {:.1f}s "
                    "(phase at stall: {})",
                    totalS, stallPhase ? stallPhase : "unknown");
                inStall = false;
            }
            continue;
        }

        if (!inStall)
        {
            inStall = true;
            stallStartNs = beat;
            lastReportNs = 0;
            stallPhase = g_Phase.load(std::memory_order_relaxed);
        }

        if (lastReportNs != 0 &&
            (now - lastReportNs) / 1'000'000 < kRestallLogIntervalMs)
            continue;
        lastReportNs = now;

        const char* phase = g_Phase.load(std::memory_order_relaxed);
        const double stalledS = static_cast<double>(silentMs) / 1000.0;
        // Read after any stack capture below has resumed the main thread.
        std::string jobSystem;
#ifdef _WIN32
        bool captured = false;
        std::string stack;
        {
            std::lock_guard<std::mutex> modeLock(g_CaptureModeMutex);
            if (g_StackCaptureArmed)
            {
                void* frames[kMaxStackFrames];
                // Held across suspend+walk+symbolize — see CaptureMainThreadStack.
                std::lock_guard<std::mutex> dbghelpLock(Logger::DbgHelpMutex());
                const int n = CaptureMainThreadStack(frames, kMaxStackFrames);
                stack = SymbolizeFrames(frames, n);
                captured = true;
            }
        }
        jobSystem = DescribeJobSystem(now);

        if (captured)
        {
            Logger::Log::Warning(
                "[HangWatchdog] main thread unresponsive for {:.1f}s (phase: {}); "
                "main-thread stack:{}{}",
                stalledS, phase ? phase : "unknown",
                stack.empty() ? "\n    <no frames captured>" : stack.c_str(), jobSystem);
        }
        else
        {
            Logger::Log::Warning(
                "[HangWatchdog] main thread unresponsive for {:.1f}s (phase: {}); "
                "no stack: in-process capture suspends the main thread and runs "
                "dbghelp inside that window, which deadlocks if the suspended "
                "thread holds the CRT heap or loader lock. Attach out of process "
                "instead: cdb -pv -p {} -c \"~*k; qd\". "
                "GE_HANG_WATCHDOG_SHUTDOWN_STACKS=1 arms in-process capture and "
                "accepts that risk.{}",
                stalledS, phase ? phase : "unknown", GetCurrentProcessId(), jobSystem);
        }
#else
        jobSystem = DescribeJobSystem(now);
        Logger::Log::Warning(
            "[HangWatchdog] main thread unresponsive for {:.1f}s (phase: {}){}",
            stalledS, phase ? phase : "unknown", jobSystem);
#endif
    }

    // No SymCleanup: the dbghelp session is process-shared with the logger's
    // backtrace path (Backtrace.cpp s_symInitialized), which must stay live
    // after the watchdog stops.
}

} // namespace

void MainThreadHangWatchdog::Start(const JobSystem::WorkStealingThreadPool* jobSystem)
{
    if (g_Running.load(std::memory_order_relaxed))
        return;

    if (const char* env = std::getenv("GE_HANG_WATCHDOG");
        env && env[0] == '0' && env[1] == '\0')
        return;

    const int thresholdMs = ThresholdFromEnv("GE_HANG_WATCHDOG_MS", kDefaultThresholdMs);
    g_ShutdownThresholdMs.store(
        ThresholdFromEnv("GE_HANG_WATCHDOG_SHUTDOWN_MS", kDefaultShutdownThresholdMs),
        std::memory_order_relaxed);

#ifdef _WIN32
    {
        std::lock_guard<std::mutex> modeLock(g_CaptureModeMutex);
        g_StackCaptureArmed = true;
    }
    g_ShutdownStacksArmed.store(FlagFromEnv("GE_HANG_WATCHDOG_SHUTDOWN_STACKS"),
                                std::memory_order_relaxed);

    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &g_MainThread, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
    {
        Logger::Log::Warning(
            "[HangWatchdog] DuplicateHandle failed ({}); watchdog disabled",
            GetLastError());
        return;
    }
#endif

    {
        std::lock_guard<std::mutex> lock(g_JobSystemMutex);
        g_JobSystem = jobSystem;
        // Every compute worker plus the most blocking threads any pool has.
        g_Occupancy.resize(jobSystem ? jobSystem->GetWorkerCount() + JobSystem::kMaxBlockingThreadBudget : 0);
    }

    g_LastBeatNs.store(0, std::memory_order_relaxed);
    g_Phase.store(nullptr, std::memory_order_relaxed);
    g_ThresholdMs.store(thresholdMs, std::memory_order_relaxed);
    g_Running.store(true, std::memory_order_relaxed);
    g_Thread = std::thread(WatchdogLoop);
}

void MainThreadHangWatchdog::BeginShutdownWatch()
{
    if (!g_Running.load(std::memory_order_relaxed))
        return;

    // Teardown destroys the job system; detach before it can.
    DetachJobSystem();

    // Restart the clock: the stall being timed is the shutdown, not whatever
    // held the last frame (a modal dialog, say) before the loop exited.
    NotifyAlive();
    g_Phase.store(kShutdownPhase, std::memory_order_relaxed);
    g_ThresholdMs.store(g_ShutdownThresholdMs.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);

#ifdef _WIN32
    const bool armed = g_ShutdownStacksArmed.load(std::memory_order_relaxed);
    {
        // The switch is synchronous: the loop holds this across decide+capture,
        // so on return no capture is in flight and none can start without
        // seeing the new setting. Arriving mid-capture is the normal case here,
        // not a corner — a terminal device-loss dialog stalls the last frame
        // for as long as it is open, so the loop is already reporting when the
        // dialog is answered and teardown begins.
        std::lock_guard<std::mutex> modeLock(g_CaptureModeMutex);
        g_StackCaptureArmed = armed;
    }

    if (armed)
        Logger::Log::Warning(
            "[HangWatchdog] GE_HANG_WATCHDOG_SHUTDOWN_STACKS=1: a teardown "
            "stall will suspend the main thread and walk it with dbghelp. "
            "dbghelp allocates and touches the loader, so a main thread "
            "suspended while holding the CRT heap or loader lock deadlocks "
            "against the capture and the process never exits — and teardown, "
            "which is a burst of frees and module unloads, is where it holds "
            "those locks most. Diagnostic runs only.");
#endif
}

void MainThreadHangWatchdog::Stop()
{
    if (!g_Running.exchange(false, std::memory_order_relaxed))
        return;
    if (g_Thread.joinable())
        g_Thread.join();
    DetachJobSystem();
#ifdef _WIN32
    if (g_MainThread)
    {
        CloseHandle(g_MainThread);
        g_MainThread = nullptr;
    }
#endif
}

void MainThreadHangWatchdog::NotifyAlive()
{
    g_LastBeatNs.store(NowNs(), std::memory_order_relaxed);
}

HangWatchdogPhase::HangWatchdogPhase(const char* phase)
    : m_Previous(g_Phase.exchange(phase, std::memory_order_relaxed))
{
}

HangWatchdogPhase::~HangWatchdogPhase()
{
    g_Phase.store(m_Previous, std::memory_order_relaxed);
}

} // namespace GameEngine::Editor

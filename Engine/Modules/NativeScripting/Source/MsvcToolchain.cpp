#include "NativeScripting/MsvcToolchain.h"

#include "ExecutablePathScan.h" // FindExecutableOnPath (fork-free $PATH scan)
#include "Engine/Build/CancellableShellProcess.h"
#include "JobSystem/JobChannel.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "Types/StringUtils.h" // ToLowerAscii / ContainsIgnoreCase

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#include <process.h> // _getpid
#else
#include <unistd.h> // getpid
#endif

namespace GameEngine
{
namespace NativeScripting
{

namespace
{
// Current process id, for a per-process cache dir (see WarmAsync).
int CurrentProcessId()
{
#if defined(_WIN32)
    return _getpid();
#else
    return getpid();
#endif
}

// Run a shell command and return its captured stdout (no cancellation).
std::string RunCapture(const std::string& command)
{
    CancellableShellProcess proc;
    ShellProcessResult result = proc.Run(command, [] { return false; }, {});
    return result.exitCode == 0 ? result.output : std::string{};
}


// First line of `text` that names an existing file, or empty.
std::filesystem::path FirstExistingPathLine(const std::string& text)
{
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        std::error_code ec;
        if (std::filesystem::exists(line, ec))
            return std::filesystem::path(line);
    }
    return {};
}

#if defined(_WIN32)
// The Visual Studio install path via vswhere (-latest), or empty.
std::filesystem::path FindVisualStudio()
{
    const std::filesystem::path vswhere =
        "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe";
    std::error_code ec;
    if (!std::filesystem::exists(vswhere, ec))
        return {};
    const std::string out = RunCapture("\"" + vswhere.generic_string() + "\" -latest -property installationPath");
    std::istringstream in(out);
    std::string line;
    if (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::error_code ec2;
        if (!line.empty() && std::filesystem::exists(line, ec2))
            return std::filesystem::path(line);
    }
    return {};
}

// Run vcvars64.bat (banner suppressed) + `set`, writing the resulting environment to a cached
// batch of `set "VAR=value"` lines. Returns true only if the captured environment is COMPLETE —
// it carries the Windows SDK UCRT include path (which provides crtdbg.h etc.) and a LIB path. An
// incomplete capture (a partial vcvars run, or a value our quote-skip dropped) is rejected so the
// Ninja build never runs against a half-set environment and fails with "cannot open crtdbg.h";
// instead the build falls back to the default generator, which sets up its own complete env.
bool CaptureMsvcEnvToBatch(const std::filesystem::path& vcvars, const std::filesystem::path& batchPath)
{
    const std::string out = RunCapture("\"" + vcvars.generic_string() + "\" >nul 2>&1 && set");
    if (out.empty())
        return false;

    std::ofstream bat(batchPath, std::ios::trunc);
    if (!bat)
        return false;
    bat << "@echo off\n";
    std::istringstream in(out);
    std::string line;
    int written = 0;
    bool includeHasUcrt = false;
    bool libPresent = false;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const auto eq = line.find('=');
        if (eq == std::string::npos || eq == 0)
            continue;
        const std::string name = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (value.find('"') != std::string::npos)
            continue; // can't safely emit set "VAR=value" when value holds a quote (rare)
        const std::string lname = ToLowerAscii(name);
        if (lname == "include" && ContainsIgnoreCase(value, "ucrt"))
            includeHasUcrt = true;
        else if (lname == "lib" && !value.empty())
            libPresent = true;
        bat << "set \"" << name << "=" << value << "\"\n";
        ++written;
    }
    return written > 0 && includeHasUcrt && libPresent;
}
#endif // _WIN32
} // namespace

MsvcToolchain::MsvcToolchain() = default;

MsvcToolchain::~MsvcToolchain() = default;

void MsvcToolchain::WarmAsync(JobSystem::WorkStealingThreadPool* jobSystem, std::filesystem::path cacheDir)
{
    // No-op without a job system (e.g. unit tests): detection shells out to vswhere/vcvars
    // (~seconds), so we never run it on a caller's thread. Builds simply fall back to the
    // default generator until a warmed toolchain is available.
    if (!jobSystem)
        return;
    if (cacheDir.empty())
    {
        std::error_code ec;
        // Per-PROCESS subdir: the cached vcvars batch must NOT be shared across editor instances.
        // Multiple instances (e.g. a concurrent session) each warm at init and write msvc_env.bat;
        // a shared path lets one truncate the file mid-read, so a build's `call msvc_env.bat` sees
        // a half-written INCLUDE → cl fails with "cannot open crtdbg.h". A per-process dir gives
        // each instance its own batch — no cross-instance write race.
        cacheDir = std::filesystem::temp_directory_path(ec) / "GameEngineNativeScripting" /
                   std::to_string(CurrentProcessId());
    }
    m_WarmStarted.store(true, std::memory_order_release);
    m_DetectionChannel = std::make_unique<JobSystem::JobChannel>(
        *jobSystem, JobSystem::JobChannelDesc{.Name = "MSVC detection", .MaxRunning = 1});
    m_WarmTask = m_DetectionChannel->Submit([this, cacheDir]() mutable { Detect(std::move(cacheDir)); });
}

void MsvcToolchain::JoinWarm()
{
    if (m_WarmTask.IsValid() && !m_WarmTask.IsDone())
    {
        m_WarmTask.Wait();
    }
    m_WarmTask = JobSystem::TaskHandle{};
    m_DetectionChannel.reset();
}

bool MsvcToolchain::WaitUntilReady(std::chrono::milliseconds timeout) const
{
    // No warm in flight (e.g. no job system): nothing to wait for.
    if (!m_WarmStarted.load(std::memory_order_acquire))
        return NinjaReady();

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!m_DetectionComplete.load(std::memory_order_acquire))
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            // Detection unusually slow/hung (vswhere/vcvars stuck?) — surface it, then fall back
            // to the default generator so the build still proceeds (slower, but it builds).
            Logger::Log::Warning("[NativeScripting] fast-build toolchain detection timed out after {}s; "
                                 "this build uses the default generator (slower)",
                                 std::chrono::duration_cast<std::chrono::seconds>(timeout).count());
            break;
        }
        // Coarse poll: detection is a one-time cold-start step (vswhere + vcvars ~seconds), so a
        // ~100ms granularity adds negligible latency while avoiding hundreds of needless wakeups.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return NinjaReady();
}

void MsvcToolchain::Detect(std::filesystem::path cacheDir)
{
    std::error_code ec;
    std::filesystem::create_directories(cacheDir, ec);

    std::filesystem::path ninja;
    std::filesystem::path envBatch;

#if defined(_WIN32)
    // 1. Ninja: PATH, then the Visual Studio bundled copy.
    ninja = FirstExistingPathLine(RunCapture("where ninja"));
    const std::filesystem::path vs = FindVisualStudio();
    if (ninja.empty() && !vs.empty())
    {
        const std::filesystem::path bundled =
            vs / "Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe";
        if (std::filesystem::exists(bundled, ec))
            ninja = bundled;
    }

    // 2. MSVC environment from vcvars64.bat → cached batch.
    if (!vs.empty())
    {
        const std::filesystem::path vcvars = vs / "VC/Auxiliary/Build/vcvars64.bat";
        if (std::filesystem::exists(vcvars, ec))
        {
            const std::filesystem::path batch = cacheDir / "msvc_env.bat";
            if (CaptureMsvcEnvToBatch(vcvars, batch))
                envBatch = batch;
        }
    }

    const bool ready = !ninja.empty() && !envBatch.empty();
#else
    // POSIX: the compiler environment is ambient; just locate ninja on PATH.
    // Scan PATH directly instead of `command -v ninja` — shelling out forks, and a
    // fork from this worker during engine init can deadlock the main thread (see
    // FindExecutableOnPath). cmake's default generator is the fallback when ninja
    // is absent, so a miss is harmless.
    ninja = FindExecutableOnPath("ninja");
    const bool ready = !ninja.empty();
#endif

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_NinjaExe = ninja;
        m_EnvBatch = envBatch;
    }
    m_NinjaReady.store(ready, std::memory_order_release);
    m_DetectionComplete.store(true, std::memory_order_release); // unblocks WaitUntilReady

    if (ready)
        Logger::Log::Info("[NativeScripting] fast build toolchain ready: ninja={}", ninja.string());
    else
        Logger::Log::Info("[NativeScripting] no Ninja/MSVC env detected — user builds use the default generator");
}

std::filesystem::path MsvcToolchain::NinjaExe() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_NinjaExe;
}

std::filesystem::path MsvcToolchain::EnvBatch() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_EnvBatch;
}

} // namespace NativeScripting
} // namespace GameEngine

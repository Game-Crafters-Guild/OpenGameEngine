#pragma once

// MsvcToolchain — detects a fast build toolchain for user-DLL builds and caches the
// captured MSVC environment so warm rebuilds avoid re-running vcvars.
//
// Windows: locates a Ninja executable (PATH → Visual Studio bundled → none) and captures
// the MSVC environment from vcvars64.bat once into a cached .bat, so the Ninja build can
// prepend it (cl.exe needs INCLUDE/LIB/PATH). Other platforms: Ninja-on-PATH only (the
// compiler environment is ambient), no env batch.
//
// Warmed ASYNCHRONOUSLY at manager init (vswhere + vcvars take a couple seconds), so it is
// ready before the first user build. Until it is ready — or if no Ninja / MSVC env is
// found — the build falls back to the platform default generator (Visual Studio on Windows).

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>

#include "JobSystem/TaskHandle.h"

namespace JobSystem
{
class JobChannel;
class WorkStealingThreadPool;
}

namespace GameEngine
{
namespace NativeScripting
{

class MsvcToolchain
{
public:
    MsvcToolchain();
    ~MsvcToolchain();

    // Kick detection as a job of the toolchain's "MSVC detection" channel (cap 1) on the
    // job system: detection waits on vswhere and vcvars processes, so it holds no compute
    // worker. No-op without a job system (detection must not run on a caller's thread).
    // cacheDir is a user-writable directory for the captured env batch; empty → the OS
    // temp dir. Call once at init.
    void WarmAsync(JobSystem::WorkStealingThreadPool* jobSystem, std::filesystem::path cacheDir = {});

    // True once detection completed AND a usable Ninja (+ MSVC env on Windows) was found.
    bool NinjaReady() const { return m_NinjaReady.load(std::memory_order_acquire); }

    // Block (on a build job, never the UI thread) until the one-time async detection
    // finishes, up to `timeout`. Returns NinjaReady(). If no warm was started (no job system,
    // e.g. tests) it returns immediately. This closes the startup race where the first user
    // build kicks before WarmAsync finishes vcvars and would otherwise fall back to the slow
    // default generator for that one build.
    bool WaitUntilReady(std::chrono::milliseconds timeout) const;

    // Join the in-flight detection job, if any, and release the detection channel. MUST be
    // called before this object (or the job system it was warmed on) is destroyed: Detect()
    // touches `this` throughout, so tearing the toolchain down mid-detection is a
    // use-after-free (a fast engine shutdown right after init can otherwise beat the
    // vswhere/vcvars probes), and a channel must not outlive its job system.
    void JoinWarm();

    std::filesystem::path NinjaExe() const;
    std::filesystem::path EnvBatch() const; // empty on non-Windows

private:
    void Detect(std::filesystem::path cacheDir); // the "MSVC detection" job

    mutable std::mutex m_Mutex;
    std::atomic<bool> m_WarmStarted{false};      // a detection job was submitted
    std::atomic<bool> m_DetectionComplete{false}; // Detect() ran to completion (found or not)
    std::atomic<bool> m_NinjaReady{false};
    std::filesystem::path m_NinjaExe;  // guarded by m_Mutex
    std::filesystem::path m_EnvBatch;  // guarded by m_Mutex
    std::unique_ptr<JobSystem::JobChannel> m_DetectionChannel; // from WarmAsync until JoinWarm
    JobSystem::TaskHandle m_WarmTask;  // valid while a detection job may be in flight
};

} // namespace NativeScripting
} // namespace GameEngine

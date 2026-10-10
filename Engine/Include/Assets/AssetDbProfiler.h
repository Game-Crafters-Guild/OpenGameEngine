#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

// Diagnostic timers for the asset pipeline. Gated by env var GE_ASSET_DB_PROFILE=1.
// When disabled, all instrumentation reduces to an atomic-load + branch.
// Accumulates counts and nanoseconds across threads via lock-free atomics,
// then emits a summary line through the logger once quiescence is reached.

namespace GameEngine::AssetDbProfiler
{

enum class Bucket : uint8_t
{
    StageFileIO,
    StageProcessing,
    StageRegistration,
    LockLoadedAssetsMain,
    LockLoadedAssetsWorker,
    UploadMeshMain,
    UploadMeshWorker,
    Count
};

bool IsEnabled();

// Call once from the main thread at engine init so the profiler can tell
// main-thread work apart from worker-thread work.
void LatchMainThread();

// Adds one sample to a bucket. Safe to call from any thread.
void Accumulate(Bucket bucket, uint64_t durationNs, uint64_t waitNs = 0);

// Call every main-thread frame. Emits a log summary roughly every 2 seconds
// if counters advanced since the last emission.
void TickMainThread();

// Force an immediate summary emission (e.g. on shutdown).
void EmitSummary(const char* label);

// true if std::this_thread matches the latched main thread id.
bool IsOnMainThread();

// Scoped timer for a pipeline stage (Discovery/FileIO/Processing/Registration)
// or a GPU upload site. Does nothing if profiling is disabled.
struct StageScope
{
    Bucket bucket;
    std::chrono::high_resolution_clock::time_point start;
    bool active;

    explicit StageScope(Bucket b) : bucket(b), active(IsEnabled())
    {
        if (active)
            start = std::chrono::high_resolution_clock::now();
    }

    ~StageScope()
    {
        if (!active)
            return;
        auto now = std::chrono::high_resolution_clock::now();
        uint64_t ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count());
        Accumulate(bucket, ns);
    }
};

// Scoped timer that separates "time waiting to acquire a lock" from
// "time holding a lock." Usage:
//
//   LockScope s(Bucket::LockLoadedAssetsMain);
//   std::lock_guard lk(m_LoadedAssetsMutex);
//   s.OnAcquired();
//   /* critical section */
//
// Call OnAcquired() immediately after the lock is taken. On destruction
// the scope records (wait = acquire - construct) and (hold = now - acquire).
struct LockScope
{
    Bucket bucket;
    std::chrono::high_resolution_clock::time_point ctor;
    std::chrono::high_resolution_clock::time_point acquired;
    bool active;
    bool recordedAcquire;

    explicit LockScope(Bucket b) : bucket(b), active(IsEnabled()), recordedAcquire(false)
    {
        if (active)
            ctor = std::chrono::high_resolution_clock::now();
    }

    void OnAcquired()
    {
        if (!active || recordedAcquire)
            return;
        acquired = std::chrono::high_resolution_clock::now();
        recordedAcquire = true;
    }

    ~LockScope()
    {
        if (!active)
            return;
        if (!recordedAcquire)
            return;
        auto now = std::chrono::high_resolution_clock::now();
        uint64_t waitNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(acquired - ctor).count());
        uint64_t holdNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - acquired).count());
        Accumulate(bucket, holdNs, waitNs);
    }
};

} // namespace GameEngine::AssetDbProfiler

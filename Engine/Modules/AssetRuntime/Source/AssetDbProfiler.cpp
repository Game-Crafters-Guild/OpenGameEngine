#include "Assets/AssetDbProfiler.h"

#include "Logger/Logger.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace GameEngine::AssetDbProfiler
{

namespace
{

struct BucketState
{
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> totalNs{0};
    std::atomic<uint64_t> waitNs{0};  // only meaningful for Lock* buckets
    std::atomic<uint64_t> peakNs{0};  // longest single sample
};

std::array<BucketState, static_cast<size_t>(Bucket::Count)> g_Buckets;

// Env var parsed once on first IsEnabled() call. std::atomic so multiple
// threads don't race during first-use.
std::atomic<int8_t> g_EnabledState{-1}; // -1 = unset, 0 = off, 1 = on

std::atomic<std::thread::id> g_MainThreadId{};
std::atomic<bool> g_MainThreadLatched{false};

std::atomic<uint64_t> g_LastEmitSnapshotCount{0};
std::atomic<int64_t> g_LastEmitTimeMs{0};

const char* BucketName(Bucket b)
{
    switch (b)
    {
    case Bucket::StageFileIO: return "stage.FileIO";
    case Bucket::StageProcessing: return "stage.Processing";
    case Bucket::StageRegistration: return "stage.Registration";
    case Bucket::LockLoadedAssetsMain: return "lock.LoadedAssets.main";
    case Bucket::LockLoadedAssetsWorker: return "lock.LoadedAssets.worker";
    case Bucket::UploadMeshMain: return "gpu.UploadMesh.main";
    case Bucket::UploadMeshWorker: return "gpu.UploadMesh.worker";
    default: return "?";
    }
}

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

uint64_t TotalSamples()
{
    uint64_t sum = 0;
    for (auto& b : g_Buckets)
        sum += b.count.load(std::memory_order_relaxed);
    return sum;
}

} // namespace

bool IsEnabled()
{
    int8_t state = g_EnabledState.load(std::memory_order_relaxed);
    if (state < 0)
    {
        const char* v = std::getenv("GE_ASSET_DB_PROFILE");
        bool on = v && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
        g_EnabledState.store(on ? 1 : 0, std::memory_order_relaxed);
        state = on ? 1 : 0;
    }
    return state == 1;
}

void LatchMainThread()
{
    if (g_MainThreadLatched.exchange(true))
        return;
    g_MainThreadId.store(std::this_thread::get_id(), std::memory_order_release);
    if (IsEnabled())
    {
        Logger::Log::Info("[AssetDbProfiler] enabled; main thread latched");
    }
}

bool IsOnMainThread()
{
    if (!g_MainThreadLatched.load(std::memory_order_acquire))
        return false;
    return std::this_thread::get_id() == g_MainThreadId.load(std::memory_order_acquire);
}

void Accumulate(Bucket bucket, uint64_t durationNs, uint64_t waitNs)
{
    if (!IsEnabled())
        return;
    auto& state = g_Buckets[static_cast<size_t>(bucket)];
    state.count.fetch_add(1, std::memory_order_relaxed);
    state.totalNs.fetch_add(durationNs, std::memory_order_relaxed);
    state.waitNs.fetch_add(waitNs, std::memory_order_relaxed);
    uint64_t prev = state.peakNs.load(std::memory_order_relaxed);
    while (durationNs > prev && !state.peakNs.compare_exchange_weak(
                                    prev, durationNs, std::memory_order_relaxed))
        ;
}

void EmitSummary(const char* label)
{
    if (!IsEnabled())
        return;

    uint64_t total = TotalSamples();
    Logger::Log::Info("[AssetDbProfiler] ==== {} (totalSamples={}) ====", label ? label : "snapshot", total);
    for (size_t i = 0; i < static_cast<size_t>(Bucket::Count); ++i)
    {
        auto& s = g_Buckets[i];
        uint64_t count = s.count.load(std::memory_order_relaxed);
        if (count == 0)
            continue;
        uint64_t tNs = s.totalNs.load(std::memory_order_relaxed);
        uint64_t wNs = s.waitNs.load(std::memory_order_relaxed);
        uint64_t pNs = s.peakNs.load(std::memory_order_relaxed);
        double totMs = static_cast<double>(tNs) / 1'000'000.0;
        double waitMs = static_cast<double>(wNs) / 1'000'000.0;
        double peakMs = static_cast<double>(pNs) / 1'000'000.0;
        double avgUs = static_cast<double>(tNs) / static_cast<double>(count) / 1000.0;

        auto name = BucketName(static_cast<Bucket>(i));
        if (wNs > 0)
        {
            Logger::Log::Info(
                "  {:<28} count={:>7} total={:>8.2f}ms wait={:>8.2f}ms avg={:>8.2f}us peak={:>7.2f}ms",
                name, count, totMs, waitMs, avgUs, peakMs);
        }
        else
        {
            Logger::Log::Info(
                "  {:<28} count={:>7} total={:>8.2f}ms avg={:>8.2f}us peak={:>7.2f}ms",
                name, count, totMs, avgUs, peakMs);
        }
    }
}

void TickMainThread()
{
    if (!IsEnabled())
        return;

    uint64_t samples = TotalSamples();
    uint64_t lastSnap = g_LastEmitSnapshotCount.load(std::memory_order_relaxed);
    int64_t now = NowMs();
    int64_t lastEmit = g_LastEmitTimeMs.load(std::memory_order_relaxed);

    if (samples == lastSnap)
        return;                              // no new activity
    if (now - lastEmit < 2000)
        return;                              // emit at most every 2s

    g_LastEmitSnapshotCount.store(samples, std::memory_order_relaxed);
    g_LastEmitTimeMs.store(now, std::memory_order_relaxed);
    EmitSummary("tick");
}

} // namespace GameEngine::AssetDbProfiler

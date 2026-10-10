// TextureService's GPU texture cache is reached from more than one thread, so it
// owns its lock instead of documenting a contract the call sites do not keep.
//
// GetOrUpload resolves textures from the ECS extraction wave — TerrainExtraction,
// OceanExtraction, LensFlareExtraction and EZTreeExtraction all call it, and
// Systems.h:304 dispatches a wave's systems onto JobSystem workers — while the
// render thread uploads, evicts and hot-swaps through the same map. As a bare
// std::unordered_map that was a rehashing insert running under a concurrent
// reader.
//
// These pin the structural property that a racy map cannot hold (every key that
// was published is still present, and readers never miss a key that is), and the
// publish contract that keeps a lost upload race from orphaning a handle other
// callers already hold.
//
// Deliberately CPU-only: the cache is a map behind a mutex, so no device and no
// GPU work — the concurrency arms are plain thread hammers.

#include "Engine/Rendering/TextureGPUCache.h"

#include <atomic>
#include <cstdint>
#include <thread>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

using GameEngine::GUID;
using GameEngine::Engine::Renderer::TextureGPUCache;
using GameEngine::Rendering::TextureHandle;

namespace
{
// Enough distinct keys to drive the backing map through many rehashes, which is
// what makes a concurrent insert visible to a concurrent reader.
constexpr size_t kKeysPerWriter = 2000;

// Deterministic, distinct GUIDs. Only identity matters here.
GUID MakeGuid(uint64_t n)
{
    GUID::Data data{};
    for (size_t byte = 0; byte < sizeof(uint64_t); ++byte)
    {
        data[byte] = static_cast<GameEngine::uint8>((n >> (byte * 8)) & 0xFFu);
    }
    return GUID(data);
}

// Handles are opaque ids to the cache; a non-zero raw id is a valid handle.
TextureHandle MakeHandle(uint64_t n)
{
    return TextureHandle{static_cast<GameEngine::Rendering::Detail::HandleType>(n + 1u)};
}
} // namespace

// --- Single-threaded semantics -----------------------------------------------

TEST(TextureGPUCacheTest, MissingKeyIsNotFoundAndLeavesTheOutputAlone)
{
    TextureGPUCache cache;
    TextureHandle out = MakeHandle(7);

    EXPECT_FALSE(cache.TryGet(MakeGuid(1), out));
    EXPECT_FALSE(cache.Contains(MakeGuid(1)));
    EXPECT_EQ(out, MakeHandle(7)) << "TryGet must not write to out on a miss";
}

// A negative entry (GetOrUpload caching an invalid handle for a GUID with no
// metadata) is CACHED, not absent — that is what stops the GUID re-resolving
// every frame.
TEST(TextureGPUCacheTest, AnInvalidHandleIsStillACachedEntry)
{
    TextureGPUCache cache;
    const GUID guid = MakeGuid(1);

    EXPECT_EQ(cache.PublishOrAdopt(guid, TextureHandle{}), TextureHandle{});

    TextureHandle out = MakeHandle(7);
    EXPECT_TRUE(cache.TryGet(guid, out)) << "a negative entry must read back as present";
    EXPECT_FALSE(out.IsValid());
    EXPECT_TRUE(cache.Contains(guid));
}

TEST(TextureGPUCacheTest, PublishOrAdoptKeepsTheFirstHandleAndSetOverwritesIt)
{
    TextureGPUCache cache;
    const GUID guid = MakeGuid(1);

    EXPECT_EQ(cache.PublishOrAdopt(guid, MakeHandle(10)), MakeHandle(10));
    // The second publisher is told which handle actually won, so it can retire
    // its own rather than overwrite the one already handed to callers.
    EXPECT_EQ(cache.PublishOrAdopt(guid, MakeHandle(20)), MakeHandle(10));

    TextureHandle out{};
    ASSERT_TRUE(cache.TryGet(guid, out));
    EXPECT_EQ(out, MakeHandle(10));

    // Set is the render-thread override (hot-swap restore over a negative entry).
    cache.Set(guid, MakeHandle(30));
    ASSERT_TRUE(cache.TryGet(guid, out));
    EXPECT_EQ(out, MakeHandle(30));
}

TEST(TextureGPUCacheTest, RemoveYieldsTheStoredHandleOnceAndTakeEmptiesTheCache)
{
    TextureGPUCache cache;
    cache.Set(MakeGuid(1), MakeHandle(10));
    cache.Set(MakeGuid(2), MakeHandle(20));

    TextureHandle removed{};
    ASSERT_TRUE(cache.Remove(MakeGuid(1), removed));
    EXPECT_EQ(removed, MakeHandle(10));
    EXPECT_FALSE(cache.Contains(MakeGuid(1)));
    EXPECT_FALSE(cache.Remove(MakeGuid(1), removed)) << "a second Remove must not re-yield the handle";

    const auto taken = cache.Take();
    EXPECT_EQ(taken.size(), 1u);
    EXPECT_EQ(taken.at(MakeGuid(2)), MakeHandle(20));
    EXPECT_FALSE(cache.Contains(MakeGuid(2))) << "Take must leave the cache empty";

    cache.Set(MakeGuid(3), MakeHandle(30));
    cache.Clear();
    EXPECT_FALSE(cache.Contains(MakeGuid(3)));
}

// --- Concurrent contract -----------------------------------------------------

// The reported defect: two extraction workers first-touching different textures
// race a rehashing insert. Structural, not value-comparing — a torn map loses
// entries or corrupts bucket chains, and freed-but-unmodified storage cannot
// disguise a key that is simply gone.
TEST(TextureGPUCacheTest, ConcurrentPublishesOfDistinctKeysAllSurvive)
{
    constexpr size_t kWriters = 4;

    TextureGPUCache cache;
    std::atomic<bool> start{false};

    std::vector<std::thread> writers;
    writers.reserve(kWriters);
    for (size_t w = 0; w < kWriters; ++w)
    {
        writers.emplace_back([&, w] {
            while (!start.load(std::memory_order_acquire)) { std::this_thread::yield(); }
            for (size_t i = 0; i < kKeysPerWriter; ++i)
            {
                const uint64_t n = w * kKeysPerWriter + i;
                cache.PublishOrAdopt(MakeGuid(n), MakeHandle(n));
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : writers) { t.join(); }

    // Every key each writer published is present, with its own handle.
    for (uint64_t n = 0; n < kWriters * kKeysPerWriter; ++n)
    {
        TextureHandle out{};
        ASSERT_TRUE(cache.TryGet(MakeGuid(n), out)) << "entry " << n << " was lost by a concurrent insert";
        EXPECT_EQ(out, MakeHandle(n));
    }
    EXPECT_EQ(cache.Take().size(), kWriters * kKeysPerWriter);
}

// Resolves during a publish storm: the shape that reaches the cache when one
// extraction worker is uploading a new texture while others resolve textures
// they already have. A tracked key is known to be present for the whole run, so
// a miss or a wrong handle is a real observation, not a benign stale read.
TEST(TextureGPUCacheTest, ResolvesAreUndisturbedByConcurrentPublishes)
{
    constexpr size_t kTrackedCount = 64;
    constexpr size_t kReaderThreads = 4;
    constexpr size_t kResolvesPerReader = 20000;
    constexpr uint64_t kGrowthBase = 1000000;

    TextureGPUCache cache;
    for (uint64_t i = 0; i < kTrackedCount; ++i)
    {
        cache.Set(MakeGuid(i), MakeHandle(i));
    }

    std::atomic<bool> start{false};
    std::atomic<bool> writerDone{false};
    std::atomic<uint64_t> misses{0};
    std::atomic<uint64_t> mismatches{0};

    // Writer: grows the map for the whole run, forcing repeated rehashes.
    std::thread writer([&] {
        while (!start.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        for (uint64_t i = 0; i < kKeysPerWriter; ++i)
        {
            cache.PublishOrAdopt(MakeGuid(kGrowthBase + i), MakeHandle(kGrowthBase + i));
        }
        writerDone.store(true, std::memory_order_release);
    });

    std::vector<std::thread> readers;
    readers.reserve(kReaderThreads);
    for (size_t t = 0; t < kReaderThreads; ++t)
    {
        readers.emplace_back([&, t] {
            while (!start.load(std::memory_order_acquire)) { std::this_thread::yield(); }
            for (size_t n = 0; n < kResolvesPerReader; ++n)
            {
                const uint64_t idx = (n + t) % kTrackedCount;
                TextureHandle out{};
                if (!cache.TryGet(MakeGuid(idx), out))
                {
                    misses.fetch_add(1, std::memory_order_relaxed);
                }
                else if (out != MakeHandle(idx))
                {
                    mismatches.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    writer.join();
    for (auto& r : readers) { r.join(); }

    EXPECT_TRUE(writerDone.load(std::memory_order_acquire));
    EXPECT_EQ(misses.load(), 0u) << "a resolve missed a key that was present for the whole run";
    EXPECT_EQ(mismatches.load(), 0u) << "a resolve observed a handle other than the one published";

    // The writer's keys landed too, so the storm was real rather than elided.
    for (uint64_t i = 0; i < kKeysPerWriter; ++i)
    {
        TextureHandle out{};
        ASSERT_TRUE(cache.TryGet(MakeGuid(kGrowthBase + i), out)) << "publish " << i << " was lost";
    }
}

// The publish contract under contention: when many threads upload the same GUID
// concurrently, exactly one candidate is adopted and EVERY caller is handed that
// same handle. Callers that lose keep a duplicate GPU texture to retire; a cache
// that overwrote instead would leave earlier callers holding an orphaned handle.
TEST(TextureGPUCacheTest, ConcurrentPublishesOfOneKeyAgreeOnASingleWinner)
{
    constexpr size_t kThreads = 8;
    constexpr size_t kRounds = 500;

    for (size_t round = 0; round < kRounds; ++round)
    {
        TextureGPUCache cache;
        const GUID contested = MakeGuid(round);

        std::atomic<bool> start{false};
        std::vector<TextureHandle> observed(kThreads);

        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (size_t t = 0; t < kThreads; ++t)
        {
            threads.emplace_back([&, t] {
                while (!start.load(std::memory_order_acquire)) { std::this_thread::yield(); }
                observed[t] = cache.PublishOrAdopt(contested, MakeHandle(t));
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& t : threads) { t.join(); }

        TextureHandle stored{};
        ASSERT_TRUE(cache.TryGet(contested, stored));

        // One entry, and every thread was told about the same winner.
        ASSERT_EQ(cache.Take().size(), 1u);
        for (size_t t = 0; t < kThreads; ++t)
        {
            ASSERT_EQ(observed[t], stored)
                << "thread " << t << " was handed a handle other than the one cached";
        }
    }
}

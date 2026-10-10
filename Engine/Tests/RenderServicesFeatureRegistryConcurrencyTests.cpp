#include <gtest/gtest.h>

#include "Engine/Rendering/IRenderFeature.h"
#include "Engine/Rendering/RenderServices.h"

#include <array>
#include <atomic>
#include <thread>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;

// RenderServices::EnsureFeature is reached from ECS systems, which SystemManager
// dispatches wave-parallel onto JobSystem workers — same-wave systems call it
// concurrently (OceanExtraction and LensFlareExtraction share a wave). These
// tests pin that the registry stays coherent under that access pattern.
//
// No device: the feature registry is pure host state, so the fixture
// default-constructs RenderServices and never calls Initialize(). That keeps the
// suite off the GPU entirely.

namespace
{

// Instance accounting is per-instantiation of the template, so each Tag gets its
// own counters and its own std::type_index — which is what lets one test hammer
// many distinct types through the same registry.
template<int Tag>
struct CountedFeature final : IRenderFeature
{
    static std::atomic<int>& Constructed()
    {
        static std::atomic<int> n{0};
        return n;
    }
    static std::atomic<int>& Destroyed()
    {
        static std::atomic<int> n{0};
        return n;
    }

    CountedFeature()
    {
        Constructed().fetch_add(1, std::memory_order_relaxed);
        // Widen the construct window deliberately. The unsynchronized registry's
        // race lives between its find() miss and its emplace(), and that span is
        // exactly one feature construction — a constructor that returns
        // immediately makes the window too narrow to sample reliably. Real
        // feature constructors do more work than this spin.
        for (volatile int i = 0; i < 2000; ++i)
        {
        }
    }

    ~CountedFeature() override { Destroyed().fetch_add(1, std::memory_order_relaxed); }

    // Written only through the registry-returned reference; a torn value here
    // would mean callers were handed different objects.
    int Marker = 0x5A5A5A5A;
};

unsigned ThreadCount()
{
    const unsigned hw = std::thread::hardware_concurrency();
    return hw < 4u ? 4u : (hw > 16u ? 16u : hw);
}

} // namespace

// Every thread racing EnsureFeature for the SAME type must be handed the one
// canonical instance.
//
// This is an ADDRESS comparison, not a value comparison. The unfixed registry
// returned a reference to an object destroyed before the call returned:
// whichever thread lost the emplace race either had its node constructed and
// then discarded by emplace, or never had it consumed at all — and then its
// local unique_ptr destroyed it at scope exit. Both roads dangle. Freed storage
// keeps its old bytes, so a Marker/value check would happily read 0x5A5A5A5A
// out of a destroyed object and report success.
TEST(RenderServicesFeatureRegistryConcurrency, SameTypeYieldsOneInstanceToEveryThread)
{
    using Feature = CountedFeature<1>;
    const int constructedBefore = Feature::Constructed().load();
    const int destroyedBefore = Feature::Destroyed().load();

    RenderServices rs;
    const unsigned threads = ThreadCount();

    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};
    std::vector<Feature*> observed(threads, nullptr);
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (unsigned t = 0; t < threads; ++t)
    {
        workers.emplace_back(
            [&, t]
            {
                ready.fetch_add(1, std::memory_order_release);
                while (!go.load(std::memory_order_acquire))
                {
                }
                observed[t] = &rs.EnsureFeature<Feature>();
            });
    }

    while (ready.load(std::memory_order_acquire) < threads)
    {
    }
    go.store(true, std::memory_order_release);
    for (auto& w : workers)
        w.join();

    ASSERT_NE(observed[0], nullptr);
    for (unsigned t = 1; t < threads; ++t)
        EXPECT_EQ(observed[t], observed[0])
            << "thread " << t << " was handed a different instance than thread 0";

    // GetFeature must agree with what EnsureFeature published.
    EXPECT_EQ(rs.GetFeature<Feature>(), observed[0]);

    // Losing a simultaneous miss is allowed to cost one throwaway construction,
    // but the loser must be destroyed rather than leaked: at this point exactly
    // one instance is live, whatever the thread interleaving was.
    const int constructed = Feature::Constructed().load() - constructedBefore;
    const int destroyed = Feature::Destroyed().load() - destroyedBefore;
    EXPECT_GE(constructed, 1);
    EXPECT_EQ(constructed - destroyed, 1) << "expected exactly one live instance";
}

// Concurrent inserts of DIFFERENT types are the case that mutates the map's
// bucket array from several threads at once. Each type must end up present
// exactly once and be retrievable afterwards; a lost or corrupted node shows up
// as a null GetFeature or a mismatched address.
TEST(RenderServicesFeatureRegistryConcurrency, DistinctTypesAllSurviveConcurrentInsert)
{
    RenderServices rs;
    const unsigned threads = ThreadCount();

    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};
    // Eight distinct types, each inserted by every thread, so the map both
    // rehashes and takes concurrent hits while it does.
    std::vector<std::array<IRenderFeature*, 8>> observed(threads);
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (unsigned t = 0; t < threads; ++t)
    {
        workers.emplace_back(
            [&, t]
            {
                ready.fetch_add(1, std::memory_order_release);
                while (!go.load(std::memory_order_acquire))
                {
                }
                observed[t][0] = &rs.EnsureFeature<CountedFeature<10>>();
                observed[t][1] = &rs.EnsureFeature<CountedFeature<11>>();
                observed[t][2] = &rs.EnsureFeature<CountedFeature<12>>();
                observed[t][3] = &rs.EnsureFeature<CountedFeature<13>>();
                observed[t][4] = &rs.EnsureFeature<CountedFeature<14>>();
                observed[t][5] = &rs.EnsureFeature<CountedFeature<15>>();
                observed[t][6] = &rs.EnsureFeature<CountedFeature<16>>();
                observed[t][7] = &rs.EnsureFeature<CountedFeature<17>>();
            });
    }

    while (ready.load(std::memory_order_acquire) < threads)
    {
    }
    go.store(true, std::memory_order_release);
    for (auto& w : workers)
        w.join();

    for (unsigned t = 1; t < threads; ++t)
        for (size_t i = 0; i < observed[t].size(); ++i)
            EXPECT_EQ(observed[t][i], observed[0][i])
                << "thread " << t << " disagreed on type slot " << i;

    EXPECT_EQ(rs.GetFeature<CountedFeature<10>>(), observed[0][0]);
    EXPECT_EQ(rs.GetFeature<CountedFeature<11>>(), observed[0][1]);
    EXPECT_EQ(rs.GetFeature<CountedFeature<12>>(), observed[0][2]);
    EXPECT_EQ(rs.GetFeature<CountedFeature<13>>(), observed[0][3]);
    EXPECT_EQ(rs.GetFeature<CountedFeature<14>>(), observed[0][4]);
    EXPECT_EQ(rs.GetFeature<CountedFeature<15>>(), observed[0][5]);
    EXPECT_EQ(rs.GetFeature<CountedFeature<16>>(), observed[0][6]);
    EXPECT_EQ(rs.GetFeature<CountedFeature<17>>(), observed[0][7]);
}

// Readers running against a map that is still growing: half the threads insert
// new types while the other half look up an already-published one. A lookup that
// walks a bucket chain being rehashed underneath it is the corruption this
// guards, and a non-null-but-wrong answer is what the address check catches.
TEST(RenderServicesFeatureRegistryConcurrency, LookupsStayCoherentWhileMapGrows)
{
    using Resident = CountedFeature<20>;

    RenderServices rs;
    Resident* const resident = &rs.EnsureFeature<Resident>();
    ASSERT_NE(resident, nullptr);

    const unsigned threads = ThreadCount();
    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};
    std::atomic<int> mismatches{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (unsigned t = 0; t < threads; ++t)
    {
        workers.emplace_back(
            [&, t]
            {
                ready.fetch_add(1, std::memory_order_release);
                while (!go.load(std::memory_order_acquire))
                {
                }
                if (t % 2u == 0u)
                {
                    // Grow the map from several threads.
                    (void)rs.EnsureFeature<CountedFeature<21>>();
                    (void)rs.EnsureFeature<CountedFeature<22>>();
                    (void)rs.EnsureFeature<CountedFeature<23>>();
                    (void)rs.EnsureFeature<CountedFeature<24>>();
                    (void)rs.EnsureFeature<CountedFeature<25>>();
                    (void)rs.EnsureFeature<CountedFeature<26>>();
                }
                else
                {
                    for (int i = 0; i < 20000; ++i)
                    {
                        if (rs.GetFeature<Resident>() != resident)
                            mismatches.fetch_add(1, std::memory_order_relaxed);
                        if (&rs.EnsureFeature<Resident>() != resident)
                            mismatches.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
    }

    while (ready.load(std::memory_order_acquire) < threads)
    {
    }
    go.store(true, std::memory_order_release);
    for (auto& w : workers)
        w.join();

    EXPECT_EQ(mismatches.load(), 0) << "a lookup returned the wrong instance while the map grew";
}

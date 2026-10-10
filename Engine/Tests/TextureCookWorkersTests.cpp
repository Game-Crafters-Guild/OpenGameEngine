// TextureCookWorkers: how one cook's bands are handed to pool workers — every band
// runs exactly once, helpers encode only within the decode gate's texture share
// (the share texture decodes take too, so cooking threads that hold a texture
// decode's slot are inside it), a full share still lets every cook finish on its
// own thread, and a stop, a failed band or a throw (from a band or from the stop
// callback) ends the run only after the bands already started have finished.

#include <gtest/gtest.h>

#include "Assets/AssetDecodeGate.h"
#include "Assets/TextureCookWorkers.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace GameEngine;

namespace
{

constexpr auto kBandWork = std::chrono::milliseconds(2);

// Eight workers: a gate of six slots, four of them for texture work.
constexpr size_t kPoolWorkers = 8;
constexpr uint32 kTextureShare = 4;

struct GatedPool
{
    JobSystem::WorkStealingThreadPool Pool{kPoolWorkers};
    std::shared_ptr<AssetDecodeGate> Gate = std::make_shared<AssetDecodeGate>(kPoolWorkers, 0, nullptr);
    TextureCookWorkers Workers{Pool, Gate};
};

// The most bands seen running at once, across every probe that shares it.
struct ConcurrencyMeter
{
    void Enter()
    {
        const uint32 now = Running.fetch_add(1) + 1;
        uint32 peak = Peak.load();
        while (now > peak && !Peak.compare_exchange_weak(peak, now))
        {
        }
    }
    void Leave() { Running.fetch_sub(1); }

    std::atomic<uint32> Running{0};
    std::atomic<uint32> Peak{0};
};

// One run's bands: how often each ran, and how many ran at once. `Meter` sees
// every band; `HelperMeter` only the bands encoded off the cooking thread.
struct BandProbe
{
    BandProbe(uint32 bandCount, ConcurrencyMeter& meter, ConcurrencyMeter& helperMeter)
        : Runs(bandCount), Meter(meter), HelperMeter(helperMeter)
    {
    }

    bool Encode(uint32 band)
    {
        const bool onHelper = std::this_thread::get_id() != Caller;
        Meter.Enter();
        if (onHelper)
        {
            HelperMeter.Enter();
            OffCaller.fetch_add(1);
        }
        Runs[band].fetch_add(1);
        std::this_thread::sleep_for(kBandWork);
        if (onHelper)
            HelperMeter.Leave();
        Meter.Leave();
        return true;
    }

    std::vector<std::atomic<uint32>> Runs;
    ConcurrencyMeter& Meter;
    ConcurrencyMeter& HelperMeter;
    std::thread::id Caller = std::this_thread::get_id();
    std::atomic<uint32> OffCaller{0}; // bands encoded by a thread other than the one that built the probe
};

void ExpectEveryBandRanOnce(const BandProbe& probe)
{
    for (size_t band = 0; band < probe.Runs.size(); ++band)
        EXPECT_EQ(probe.Runs[band].load(), 1u) << "band " << band;
}

// Every helper gives its slot back: one that found no band left may do so just
// after RunBands returned, so this waits a bounded time for the share to refill.
bool ShareRefills(const AssetDecodeGate& gate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (gate.FreeTextureSlots() != kTextureShare && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return gate.FreeTextureSlots() == kTextureShare;
}

// A caller off the pool (the build's bake thread) holds no slot: the pool
// workers encoding its bands stay within the texture share, though the pool has
// idle workers past it.
TEST(TextureCookWorkers, HelpersStayWithinTheTextureShare)
{
    constexpr uint32 kBands = 60;
    GatedPool gated;

    ConcurrencyMeter meter, helperMeter;
    BandProbe probe(kBands, meter, helperMeter);
    EXPECT_TRUE(gated.Workers.RunBands(kBands, [&probe](uint32 band) { return probe.Encode(band); }, {}));

    ExpectEveryBandRanOnce(probe);
    EXPECT_LE(helperMeter.Peak.load(), kTextureShare);
    EXPECT_GT(probe.OffCaller.load(), 0u) << "the bands never left the calling thread";
    EXPECT_TRUE(ShareRefills(*gated.Gate)) << "a helper kept its gate slot";
}

// A texture decode's slot: the decode reader waits for one, and so does this, a
// bounded time, while an earlier cook's helpers hold the share.
bool AwaitTextureSlot(AssetDecodeGate& gate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!gate.TryAcquire(AssetDecodeGate::Work::Texture))
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// One cook as the editor runs it: inside a texture decode, which holds its gate
// slot for the cooking thread while the bands are encoded.
void CookInsideATextureDecode(GatedPool& gated, ConcurrencyMeter& meter, ConcurrencyMeter& helperMeter,
                              std::atomic<int>& failures)
{
    constexpr uint32 kBands = 40;
    if (!AwaitTextureSlot(*gated.Gate))
    {
        failures.fetch_add(1);
        return;
    }
    BandProbe probe(kBands, meter, helperMeter);
    if (!gated.Workers.RunBands(kBands, [&probe](uint32 band) { return probe.Encode(band); }, {}))
        failures.fetch_add(1);
    ExpectEveryBandRanOnce(probe);
    gated.Gate->Release(AssetDecodeGate::Work::Texture);
}

// Several cooks at once share the texture share with their helpers: the threads
// encoding, cooking threads included, never pass it.
TEST(TextureCookWorkers, ConcurrentCooksShareOneBudget)
{
    constexpr int kCooks = 3;
    GatedPool gated;

    ConcurrencyMeter meter, helperMeter;
    std::atomic<int> failures{0};
    std::vector<std::thread> cooks;
    for (int i = 0; i < kCooks; ++i)
        cooks.emplace_back(CookInsideATextureDecode, std::ref(gated), std::ref(meter), std::ref(helperMeter),
                           std::ref(failures));
    for (std::thread& cook : cooks)
        cook.join();

    EXPECT_EQ(failures.load(), 0);
    EXPECT_LE(meter.Peak.load(), kTextureShare);
    EXPECT_TRUE(ShareRefills(*gated.Gate)) << "a helper kept its gate slot";
}

// With every texture slot taken, no helper encodes, and the cook still finishes
// on its own thread: a caller never waits for a slot.
TEST(TextureCookWorkers, FullShareStillFinishesOnTheCallingThread)
{
    constexpr uint32 kBands = 30;
    GatedPool gated;
    for (uint32 i = 0; i < kTextureShare; ++i)
        ASSERT_TRUE(gated.Gate->TryAcquire(AssetDecodeGate::Work::Texture));

    ConcurrencyMeter meter, helperMeter;
    BandProbe probe(kBands, meter, helperMeter);
    EXPECT_TRUE(gated.Workers.RunBands(kBands, [&probe](uint32 band) { return probe.Encode(band); }, {}));
    ExpectEveryBandRanOnce(probe);
    EXPECT_EQ(probe.OffCaller.load(), 0u);

    for (uint32 i = 0; i < kTextureShare; ++i)
        gated.Gate->Release(AssetDecodeGate::Work::Texture);
}

// The stop is polled by the calling thread before each band it takes; once seen,
// no band starts, and RunBands returns only after the started ones finished.
TEST(TextureCookWorkers, StopEndsTheRunAfterStartedBandsFinish)
{
    constexpr uint32 kBands = 200;
    GatedPool gated;

    ConcurrencyMeter meter, helperMeter;
    BandProbe probe(kBands, meter, helperMeter);
    int polls = 0;
    EXPECT_FALSE(gated.Workers.RunBands(kBands, [&probe](uint32 band) { return probe.Encode(band); },
                                        [&polls] { return ++polls >= 3; }));
    EXPECT_EQ(meter.Running.load(), 0u) << "RunBands returned while a band was still encoding";

    uint32 ran = 0;
    for (const auto& runs : probe.Runs)
    {
        EXPECT_LE(runs.load(), 1u);
        ran += runs.load();
    }
    EXPECT_LT(ran, kBands);
}

// Band 5 reports a failed encode: the run fails without a throw.
bool FailBandFive(uint32 band)
{
    std::this_thread::sleep_for(kBandWork);
    return band != 5;
}

TEST(TextureCookWorkers, FailedBandFailsTheRun)
{
    GatedPool gated;
    EXPECT_FALSE(gated.Workers.RunBands(50, FailBandFive, {}));
}

// Every band a helper takes throws, as an encoder out of memory would; the
// cooking thread holds its first band until a helper has thrown, so the throw
// has to cross threads to reach the caller.
struct ThrowOnHelpers
{
    bool Encode(uint32)
    {
        Running.Enter();
        if (std::this_thread::get_id() != Caller)
        {
            HelperThrew.store(true);
            Running.Leave();
            throw std::runtime_error("encoder threw on a helper");
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!HelperThrew.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        Running.Leave();
        return true;
    }

    ConcurrencyMeter Running;
    std::atomic<bool> HelperThrew{false};
    std::thread::id Caller = std::this_thread::get_id();
};

// A helper's throw reaches the caller as that throw, what a one-thread encode
// would have thrown, never as a stop that reads like a cancel; it is rethrown
// only after the bands in progress finished.
TEST(TextureCookWorkers, HelperThrowReachesTheCallingThread)
{
    GatedPool gated;
    ThrowOnHelpers bands;
    std::string thrown;
    try
    {
        gated.Workers.RunBands(100, [&bands](uint32 band) { return bands.Encode(band); }, {});
    }
    catch (const std::runtime_error& e)
    {
        thrown = e.what();
    }
    EXPECT_TRUE(bands.HelperThrew.load()) << "no helper encoded a band";
    EXPECT_EQ(thrown, "encoder threw on a helper");
    EXPECT_EQ(bands.Running.Running.load(), 0u) << "RunBands threw while a band was still encoding";
    EXPECT_TRUE(ShareRefills(*gated.Gate)) << "a helper kept its gate slot";
}

// The cooking thread's own throw ends the run the same way.
bool ThrowAtBandZero(uint32 band)
{
    if (band == 0)
        throw std::runtime_error("encoder threw");
    std::this_thread::sleep_for(kBandWork);
    return true;
}

TEST(TextureCookWorkers, CallerThrowReachesTheCallingThread)
{
    GatedPool gated;
    EXPECT_THROW(gated.Workers.RunBands(50, ThrowAtBandZero, {}), std::runtime_error);
    EXPECT_TRUE(ShareRefills(*gated.Gate)) << "a helper kept its gate slot";
}

// A stop callback that throws ends the run too: no band starts once the throw
// has left RunBands, whose caller's frame the bands write into, and none is
// still encoding. The helpers had bands left, so they would have gone on.
TEST(TextureCookWorkers, StopThatThrowsEndsTheRunBeforeReachingTheCaller)
{
    constexpr uint32 kBands = 200;
    GatedPool gated;

    ConcurrencyMeter meter, helperMeter;
    BandProbe probe(kBands, meter, helperMeter);
    int polls = 0;
    const auto throwOnThirdPoll = [&polls]
    {
        if (++polls >= 3)
            throw std::runtime_error("stop callback threw");
        return false;
    };
    EXPECT_THROW(
        gated.Workers.RunBands(kBands, [&probe](uint32 band) { return probe.Encode(band); }, throwOnThirdPoll),
        std::runtime_error);
    EXPECT_EQ(meter.Running.load(), 0u) << "RunBands threw while a band was still encoding";

    const auto ranSoFar = [&probe]
    {
        uint32 ran = 0;
        for (const auto& runs : probe.Runs)
            ran += runs.load();
        return ran;
    };
    const uint32 ranAtThrow = ranSoFar();
    EXPECT_LT(ranAtThrow, kBands);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(ranSoFar(), ranAtThrow) << "helpers started bands after RunBands had thrown";
    EXPECT_TRUE(ShareRefills(*gated.Gate)) << "a helper kept its gate slot";
}

// An inline pool has no workers to spread to: every band runs on the caller.
TEST(TextureCookWorkers, InlinePoolEncodesOnTheCallingThread)
{
    constexpr uint32 kBands = 12;
    JobSystem::WorkStealingThreadPool pool(0);
    TextureCookWorkers workers(pool, std::make_shared<AssetDecodeGate>(0, 0, nullptr));

    ConcurrencyMeter meter, helperMeter;
    BandProbe probe(kBands, meter, helperMeter);
    EXPECT_TRUE(workers.RunBands(kBands, [&probe](uint32 band) { return probe.Encode(band); }, {}));
    ExpectEveryBandRanOnce(probe);
    EXPECT_EQ(probe.OffCaller.load(), 0u);
}

} // namespace

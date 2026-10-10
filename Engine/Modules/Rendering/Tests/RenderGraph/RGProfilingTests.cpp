// Stage 2c step 3: per-pass GPU timestamps over the REAL headless device.
// The device-owned query pool resolves results when the device frame slot
// returns; headless (GetFrameIndex pinned at 0, no device pacing) the tests
// drive the pool's BeginFrame themselves after WaitForIdle — legal per the
// QueryPool contract (idle ⊇ fence wait) — giving exactly 1 iteration latency.

#include "Rendering/Core/QueryPool.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Tests/RenderGraph/RGTestDevice.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace GameEngine::Rendering;
using namespace GameEngine::Rendering::RenderGraph;
using GameEngine::Rendering::RenderGraph::Test::MakeHeadlessDevice;

namespace
{
TextureDesc ColorDesc()
{
    TextureDesc d;
    d.width = 64;
    d.height = 64;
    d.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    d.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::RenderTarget);
    return d;
}

struct FramePools
{
    RGResourcePool Persistent;
    RGTransientPool Transient;
    RGUploadRing Ring;
    explicit FramePools(IDevice* d) : Persistent(d), Transient(d), Ring(d, 2, 4096) {}
};

// One profiled frame: clear-pass chain named by the caller, plus an optional
// culled orphan. Drives the pool's slot collection after the GPU drains.
void RunFrame(IDevice* dev, RGFrame& frame, uint64_t n, const char* nameA, const char* nameB,
              bool declareCulledOrphan = false)
{
    frame.BeginFrame(n); // resolves the previous iteration's timings
    RGTexture a = frame.CreateTexture("A", ColorDesc());
    RGTexture b = frame.CreateTexture("B", ColorDesc());
    frame.AddPass(nameA, 0, [&](RGPassBuilder& p) { p.AttachColor(0, a, {.Load = RGLoadOp::Clear}); },
                  [](RGContext&) {});
    frame.AddPass(nameB, 0,
                  [&](RGPassBuilder& p)
                  {
                      p.Read(a);
                      p.AttachColor(0, b, {.Load = RGLoadOp::DontCare});
                  },
                  [](RGContext&) {});
    if (declareCulledOrphan)
    {
        RGTexture orphan = frame.CreateTexture("Orphan", ColorDesc());
        frame.AddPass("CulledOrphan", 0, [&](RGPassBuilder& p) { p.AttachColor(0, orphan); },
                      [](RGContext&) {});
    }
    frame.MarkOutput(b);
    frame.Execute();
    dev->WaitForIdle();
    dev->GetQueryPool()->BeginFrame(dev->GetFrameIndex()); // collect + reset the slot
}
} // namespace

#define RG_REQUIRE_PROFILING_DEVICE(dev)                                                              \
    auto dev = MakeHeadlessDevice();                                                                   \
    if (!dev)                                                                                          \
        GTEST_SKIP() << "no headless device available";                                                \
    if (!dev->GetQueryPool() || !dev->GetQueryPool()->IsValid() ||                                     \
        dev->GetQueryPool()->GetTimestampPeriod() <= 0.0)                                              \
    GTEST_SKIP() << "no timestamp support on this device"

TEST(RGProfiling, DisabledByDefaultProducesNothing)
{
    RG_REQUIRE_PROFILING_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

    RunFrame(dev.get(), frame, 0, "P0", "P1");
    RunFrame(dev.get(), frame, 1, "P0", "P1");

    EXPECT_TRUE(frame.LastFrameTimings().empty());
    EXPECT_EQ(frame.LastResolveStats().ResolvedPasses, 0u);
}

TEST(RGProfiling, ResolvesExactlyThePreviousFramesTimings)
{
    RG_REQUIRE_PROFILING_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.SetProfilingEnabled(true);

    // Per-frame UNIQUE names: a copy-at-resolve regression (reading the reused
    // arena) or a slot-keying bug would surface the WRONG frame's names here.
    for (uint64_t n = 0; n < 3; ++n)
    {
        char a[32], b[32];
        std::snprintf(a, sizeof(a), "Depth.f%u", static_cast<uint32_t>(n));
        std::snprintf(b, sizeof(b), "World.f%u", static_cast<uint32_t>(n));
        RunFrame(dev.get(), frame, n, a, b);
        if (n == 0)
            continue;
        const auto timings = frame.LastFrameTimings();
        ASSERT_EQ(timings.size(), 2u) << "one timing per live pass";
        char ea[32], eb[32];
        std::snprintf(ea, sizeof(ea), "Depth.f%u", static_cast<uint32_t>(n - 1));
        std::snprintf(eb, sizeof(eb), "World.f%u", static_cast<uint32_t>(n - 1));
        EXPECT_STREQ(timings[0].Name, ea) << "snapshot must be exactly the previous frame's";
        EXPECT_STREQ(timings[1].Name, eb);
    }

    const auto timings = frame.LastFrameTimings();
    const auto stats = frame.LastResolveStats();
    EXPECT_EQ(stats.ResolvedPasses, 2u);
    EXPECT_EQ(stats.ReadFailures, 0u) << "slot was collected before resolve";
    EXPECT_EQ(stats.InvalidQueryIndices, 0u);
    for (const auto& t : timings)
    {
        EXPECT_GE(t.GpuSpanMs, 0.0); // a trivial clear can legally measure 0 ticks
        EXPECT_GT(t.CpuMs, 0.0);
        EXPECT_EQ(t.Queue, RGQueue::Graphics);
    }
    // Entries are in scheduled submission order; PassId joins same-frame data.
    EXPECT_EQ(timings[0].PassId, 0u);
    EXPECT_EQ(timings[1].PassId, 1u);
}

TEST(RGProfiling, ComputePassEntryCarriesItsQueue)
{
    RG_REQUIRE_PROFILING_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.SetProfilingEnabled(true);

    BufferDesc bd;
    bd.size = 256;
    bd.usage = static_cast<uint32_t>(BufferUsage::Storage);
    for (uint64_t n = 0; n < 3; ++n)
    {
        frame.BeginFrame(n);
        RGTexture t = frame.CreateTexture("T", ColorDesc());
        RGBuffer out = frame.CreateBuffer("Out", bd);
        frame.AddPass("Draw", 0, [&](RGPassBuilder& p) { p.AttachColor(0, t, {.Load = RGLoadOp::Clear}); },
                      [](RGContext&) {});
        frame.AddComputePass("Cull", 0,
                             [&](RGPassBuilder& p)
                             {
                                 p.Read(t);
                                 p.Write(out);
                             },
                             [](RGContext&) {});
        frame.MarkOutput(out);
        frame.Execute();
        dev->WaitForIdle();
        dev->GetQueryPool()->BeginFrame(dev->GetFrameIndex());
    }

    const auto timings = frame.LastFrameTimings();
    ASSERT_EQ(timings.size(), 2u);
    EXPECT_EQ(timings[0].Queue, RGQueue::Graphics);
    EXPECT_EQ(timings[1].Queue, RGQueue::Compute);
    EXPECT_STREQ(timings[1].Name, "Cull");
    EXPECT_GE(timings[1].GpuSpanMs, 0.0);
}

// A missed slot collection (the caller-order contract violated) must surface
// as ReadFailures — visible in stats, never a stall or garbage values.
TEST(RGProfiling, MissedCollectionSurfacesAsReadFailures)
{
    RG_REQUIRE_PROFILING_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.SetProfilingEnabled(true);

    RunFrame(dev.get(), frame, 0, "P0", "P1"); // ends with the slot collected + cached
    IQueryPool* qp = dev->GetQueryPool();
    const uint32_t slot = dev->GetFrameIndex();
    // Sabotage: drop the cached results, then reset the slice again — the next
    // resolve's fallback reads hit reset (unavailable) queries.
    qp->InvalidateCachedTimestampResults(slot);
    qp->BeginFrame(slot);
    qp->InvalidateCachedTimestampResults(slot);

    RunFrame(dev.get(), frame, 1, "P0", "P1"); // BeginFrame resolves frame 0's pending

    const auto stats = frame.LastResolveStats();
    if (stats.ResolvedPasses == 2 && stats.ReadFailures == 0)
        GTEST_SKIP() << "device served results despite the reset (no hostQueryReset)";
    EXPECT_EQ(stats.ReadFailures, 2u);
    EXPECT_EQ(stats.ResolvedPasses, 0u);
    for (const auto& t : frame.LastFrameTimings())
        EXPECT_EQ(t.GpuSpanMs, 0.0) << "failed reads must publish zeros, not garbage";
}

// 512 timestamps per device frame slot ⇒ 256 fully-bracketed passes; everything
// past the cap degrades to UINT32_MAX indices counted as InvalidQueryIndices.
TEST(RGProfiling, OverCapPassesDegradeToInvalidIndicesNotCorruption)
{
    RG_REQUIRE_PROFILING_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.SetProfilingEnabled(true);

    constexpr uint32_t kPassCount = 280; // 560 wanted timestamps > 512 cap
    auto bigFrame = [&](uint64_t n)
    {
        frame.BeginFrame(n);
        char name[32];
        for (uint32_t i = 0; i < kPassCount; ++i)
        {
            std::snprintf(name, sizeof(name), "P%03u", i);
            RGTexture t = frame.CreateTexture(name, ColorDesc());
            frame.AddPass(name, 0,
                          [&](RGPassBuilder& p) { p.AttachColor(0, t, {.Load = RGLoadOp::Clear}); },
                          [](RGContext&) {});
            frame.MarkOutput(t);
        }
        frame.Execute();
        dev->WaitForIdle();
        dev->GetQueryPool()->BeginFrame(dev->GetFrameIndex());
    };
    bigFrame(0);
    bigFrame(1);

    const auto stats = frame.LastResolveStats();
    const auto timings = frame.LastFrameTimings();
    ASSERT_EQ(timings.size(), kPassCount) << "every pass keeps its entry, capped or not";
    EXPECT_EQ(stats.ResolvedPasses + stats.InvalidQueryIndices + stats.ReadFailures, kPassCount);
    EXPECT_EQ(stats.InvalidQueryIndices, kPassCount - 256u);
    EXPECT_STREQ(timings[kPassCount - 1].Name, "P279") << "over-cap entries keep their names";
    EXPECT_EQ(timings[kPassCount - 1].GpuSpanMs, 0.0);
}

// Pass names are ARENA memory reset every frame, while results resolve a full
// latency window later — entries must carry their own copies. This pins
// pointer-retention of caller storage; copy-at-RESOLVE regressions are pinned
// by ResolvesExactlyThePreviousFramesTimings' per-frame-unique names.
TEST(RGProfiling, NamesSurviveArenaResetAcrossLatencyWindow)
{
    RG_REQUIRE_PROFILING_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.SetProfilingEnabled(true);

    {
        std::string alpha = "Alpha";
        std::string beta = "Beta";
        RunFrame(dev.get(), frame, 0, alpha.c_str(), beta.c_str());
    } // caller name storage gone too

    // Next frame declares DIFFERENT names into the recycled arena, then
    // resolves frame 0's timings.
    RunFrame(dev.get(), frame, 1, "GammaGammaGamma", "DeltaDeltaDelta");

    const auto timings = frame.LastFrameTimings();
    ASSERT_EQ(timings.size(), 2u);
    EXPECT_STREQ(timings[0].Name, "Alpha") << "dangling arena name (copy-at-write missed)";
    EXPECT_STREQ(timings[1].Name, "Beta");
}

TEST(RGProfiling, CulledPassNeverAppearsInTimings)
{
    RG_REQUIRE_PROFILING_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.SetProfilingEnabled(true);

    for (uint64_t n = 0; n < 3; ++n)
        RunFrame(dev.get(), frame, n, "Live0", "Live1", /*declareCulledOrphan=*/true);

    const auto timings = frame.LastFrameTimings();
    ASSERT_EQ(timings.size(), 2u);
    for (const auto& t : timings)
        EXPECT_STRNE(t.Name, "CulledOrphan");
}

TEST(RGProfiling, DisableClearsPublishedState)
{
    RG_REQUIRE_PROFILING_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.SetProfilingEnabled(true);

    for (uint64_t n = 0; n < 3; ++n)
        RunFrame(dev.get(), frame, n, "P0", "P1");
    ASSERT_FALSE(frame.LastFrameTimings().empty());

    frame.SetProfilingEnabled(false);
    EXPECT_TRUE(frame.LastFrameTimings().empty());
    EXPECT_EQ(frame.LastResolveStats().ResolvedPasses, 0u);

    RunFrame(dev.get(), frame, 3, "P0", "P1"); // stays inert while disabled
    EXPECT_TRUE(frame.LastFrameTimings().empty());
}

// Whatever the backend's timing granularity, the frame's distinct-span total is
// the sum over MEASUREMENTS, never over passes: a backend that hands several
// passes one encoder span must fold them, and one that times each pass on its
// own must fold nothing.
TEST(RGProfiling, DistinctSpanTotalCountsEachMeasurementOnce)
{
    RG_REQUIRE_PROFILING_DEVICE(dev);
    FramePools pools(dev.get());
    RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.SetProfilingEnabled(true);

    RunFrame(dev.get(), frame, 0, "A", "B");
    RunFrame(dev.get(), frame, 1, "A", "B");

    const auto timings = frame.LastFrameTimings();
    const auto stats = frame.LastResolveStats();
    ASSERT_EQ(timings.size(), 2u);
    const bool pipelinePoint = frame.TimingSemantics() == TimestampSemantics::PipelinePoint;

    double perPassSum = 0.0;
    uint32_t sharedFlagged = 0;
    for (const auto& t : timings)
    {
        perPassSum += t.GpuSpanMs;
        if (t.SpanShared)
        {
            ++sharedFlagged;
            // A shared span is shared WITH someone: another pass must report
            // the same work-unit pair.
            bool foundPartner = false;
            for (const auto& other : timings)
            {
                if (&other != &t && other.BeginUnit == t.BeginUnit && other.EndUnit == t.EndUnit)
                    foundPartner = true;
            }
            EXPECT_TRUE(foundPartner) << "SpanShared without a partner: " << t.Name;
        }
        if (pipelinePoint)
        {
            EXPECT_EQ(t.BeginUnit, IQueryPool::kInvalidTimestampUnit)
                << "per-command timestamps have no coarser work unit";
            EXPECT_FALSE(t.SpanShared);
        }
    }
    EXPECT_LE(stats.DistinctSpanGpuMs, perPassSum + 1e-9)
        << "folding duplicates can only shrink the total";
    EXPECT_LE(stats.SharedSpanPasses, sharedFlagged);
    if (pipelinePoint)
    {
        EXPECT_EQ(stats.SharedSpanPasses, 0u);
        EXPECT_NEAR(stats.DistinctSpanGpuMs, perPassSum, 1e-9);
    }
}

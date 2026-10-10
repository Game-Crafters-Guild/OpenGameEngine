// Placed-blade instrumentation: the per-view readback that turns "is there grass
// here" from a screenshot question into a number.
//
// The device-touching tests drive the REAL path end to end — a real headless
// device, a real RGFrame, a real GPU copy out of a buffer shaped like the grass
// indirect args — because the failure this instrument must not have is reporting
// a plausible number that no GPU wrote.
//
// Frame lifetime, deliberately: each suite creates ONE RGFrame and re-begins it
// per simulated frame, which is how the engine actually runs (FrameOrchestrator
// holds a per-window frame stream keyed by RGFrame* and re-begins it every
// frame). A pending cannot resolve after its declaring frame is destroyed, so a
// test that built a throwaway frame per iteration would report zero samples and
// look like an instrument bug.

#include "TerrainGrass/TerrainGrassPlacementStats.h"

#include "Rendering/Core/BarrierMapping.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <gtest/gtest.h>

#include <cstddef>

#include <memory>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Rendering::RenderGraph;
using GameEngine::TerrainGrass::GrassIndirectBlockGPU;
using GameEngine::TerrainGrass::GrassIndirectDrawGPU;
using GameEngine::TerrainGrass::TerrainGrassPlacementStats;

namespace
{

std::unique_ptr<IDevice> MakeDevice()
{
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    dd.enableSwapchain = false;
    dd.enableDebugLayer = false;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd))
        return nullptr;
    return dev;
}

struct FramePools
{
    RGResourcePool Persistent;
    RGTransientPool Transient;
    RGUploadRing Ring;
    explicit FramePools(IDevice* d) : Persistent(d), Transient(d), Ring(d, 2, 4096) {}
};

TextureDesc TinyColorDesc()
{
    TextureDesc d;
    d.width = 8;
    d.height = 8;
    d.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    d.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::RenderTarget);
    return d;
}

// A buffer shaped like TerrainGrass's indirect block: per-LOD draw records first, then the
// placement counters. The copy under test takes the whole block, so every field must arrive at its
// own offset — a recognizable sentinel in IndexCount catches a block that shifted by a word.
constexpr uint32 kIndexCountSentinel = 0xABCDu;
constexpr size_t kIndirectArgsBytes = sizeof(GrassIndirectBlockGPU);
constexpr size_t kLod0InstanceCountOffset = offsetof(GrassIndirectDrawGPU, InstanceCount);
constexpr size_t kLod1InstanceCountOffset =
    sizeof(GrassIndirectDrawGPU) + offsetof(GrassIndirectDrawGPU, InstanceCount);
constexpr size_t kCandidatesConsideredOffset = offsetof(GrassIndirectBlockGPU, CandidatesConsidered);
constexpr size_t kCellsVisibleOffset = offsetof(GrassIndirectBlockGPU, CellsVisible);
constexpr size_t kAcceptedBladesOffset = offsetof(GrassIndirectBlockGPU, AcceptedBlades);
constexpr size_t kFittedRangeScaleOffset = offsetof(GrassIndirectBlockGPU, FittedRangeScaleBits);
constexpr size_t kCapacityOffset = offsetof(GrassIndirectBlockGPU, Capacity);

BufferHandle MakeArgsBuffer(IDevice* dev)
{
    BufferDesc bd{};
    bd.size = kIndirectArgsBytes;
    bd.usage = static_cast<uint32_t>(BufferUsage::Storage)
        | static_cast<uint32_t>(BufferUsage::Indirect)
        | static_cast<uint32_t>(BufferUsage::TransferDst);
    bd.memoryUsage = BufferMemoryUsage::DeviceLocal;
    bd.debugName = "GrassStatsTest_IndirectArgs";
    return dev->CreateBuffer(bd);
}

TerrainGrassPlacementStats::Dispatch MakeDispatch(uint32 capacity)
{
    TerrainGrassPlacementStats::Dispatch d{};
    d.Capacity = capacity;
    d.CellCount = 16129u;
    d.PlannedCandidates = 370000u;
    d.TerrainParamsCount = 1u;
    d.ActiveGrassTerrains = 1u;
    d.NearDensity = 8.0f;
    d.Range = 500.0f;
    return d;
}

// One frame that seeds the args buffer with `instanceCount` and records the exact
// copy the grass render node declares, then submits and stamps the ring.
void RunPlacementFrame(IDevice* dev, RGFrame& frame, TerrainGrassPlacementStats& stats,
                       ViewId viewId, uint32 instanceCount, uint64 frameIndex,
                       const TerrainGrassPlacementStats::Dispatch& dispatched)
{
    frame.BeginFrame(frameIndex);

    const BufferHandle args = MakeArgsBuffer(dev);
    ASSERT_TRUE(args.IsValid());

    const BufferHandle slot = stats.AcquireSlotRG(dev, frame, viewId, dispatched);
    ASSERT_TRUE(slot.IsValid());

    const RGBuffer argsRG = frame.ImportExternalBuffer("GrassStatsTest.Args", args);

    frame.AddPass(
        "GrassStatsTest.Seed", 0,
        [&](RGPassBuilder& p) { p.Write(argsRG, RGBufferWrite::Storage); },
        [args, instanceCount, dispatched](RGContext& ctx)
        {
            ctx.Cmd->FillBuffer(args, 0, kIndirectArgsBytes, 0u);
            ctx.Cmd->FillBuffer(args, 0, sizeof(uint32), kIndexCountSentinel);
            ctx.Cmd->FillBuffer(args, kLod0InstanceCountOffset, sizeof(uint32), instanceCount);
            // Counters the placement compute fills beside the draw records; seeded so the readback
            // is proven to carry the whole block and not just the first draw.
            ctx.Cmd->FillBuffer(args, kCandidatesConsideredOffset, sizeof(uint32), 900000u);
            ctx.Cmd->FillBuffer(args, kCellsVisibleOffset, sizeof(uint32), 5000u);
            ctx.Cmd->FillBuffer(args, kAcceptedBladesOffset, sizeof(uint32), 800000u);
            // The scale rides the block as float bits; 1.0f is 0x3F800000.
            ctx.Cmd->FillBuffer(args, kFittedRangeScaleOffset, sizeof(uint32), 0x3F800000u);
            // The pool capacity travels in the block, stamped by the CPU before placement.
            ctx.Cmd->FillBuffer(args, kCapacityOffset, sizeof(uint32), dispatched.Capacity);
        });

    frame.AddPass(
        "GrassStatsTest.PlacedCountReadback", static_cast<int32_t>(PassPhase::kFinalize),
        [&](RGPassBuilder& p)
        {
            p.Read(argsRG, RGBufferRead::CopySrc);
            p.PreventCulling();
        },
        [args, slot](RGContext& ctx)
        {
            ctx.Cmd->Barrier(ResourceBarrier::CreateMemoryBarrier(
                static_cast<uint64_t>(PipelineStageMask::Transfer),
                static_cast<uint64_t>(PipelineStageMask::Transfer),
                static_cast<uint64_t>(ResourceAccessMask::TransferWrite),
                static_cast<uint64_t>(ResourceAccessMask::TransferRead)));
            // The PRODUCTION offset constant, not a copy of it: a wrong offset must
            // red this suite rather than silently report IndexCount as blades.
            ctx.Cmd->CopyBuffer(args, slot, TerrainGrassPlacementStats::kSlotBytes,
                                TerrainGrassPlacementStats::kIndirectArgsCopyOffset, 0);
        });

    // A cleared colour pass so Execute() mints a graphics submission, and with it
    // the token the ring's completion contract waits on.
    const RGTexture color = frame.CreateTexture("GrassStatsTest.Color", TinyColorDesc());
    frame.AddPass(
        "GrassStatsTest.Touch", 0,
        [&](RGPassBuilder& p)
        {
            RGAttachmentOps ops{};
            ops.Load = RGLoadOp::Clear;
            p.AttachColor(0, color, ops);
        },
        [](RGContext&) {});
    frame.MarkOutput(color);

    frame.Execute();
    stats.OnFrameSubmitted(frame, frame.SubmissionToken());
    dev->WaitForIdle();
    dev->DestroyBuffer(args);
}

} // namespace

// ── Pure arithmetic: the per-view budget ──────────────────────────────────────

TEST(TerrainGrassPlacementStats, NoSampleBeforeAnythingResolves)
{
    TerrainGrassPlacementStats stats;
    // "Nothing measured yet" must not be reported as "measured zero" — that
    // distinction is the whole difference between an instrument and a false one.
    EXPECT_TRUE(stats.LatchedPlacements().empty());
}

// ── The readback, end to end on a real device ─────────────────────────────────

TEST(TerrainGrassPlacementStats, PlacedCountResolvesFromTheIndirectArgs)
{
    auto dev = MakeDevice();
    if (!dev)
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(dev.get());
        RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        TerrainGrassPlacementStats stats;
        constexpr ViewId kView = 3u;
        constexpr uint32 kPlaced = 214553u;

        RunPlacementFrame(dev.get(), frame, stats, kView, kPlaced, 0, MakeDispatch(4194304u));
        stats.Poll(dev.get());

        const auto latched = stats.LatchedPlacements();
        ASSERT_EQ(latched.size(), 1u) << "one written view must latch exactly one sample";
        EXPECT_EQ(latched[0].View, kView);
        // Reads InstanceCount at offset 4, not IndexCount at offset 0.
        EXPECT_EQ(latched[0].Placed.PlacedInstances, kPlaced);
        EXPECT_NE(latched[0].Placed.PlacedInstances, kIndexCountSentinel);
        EXPECT_FALSE(latched[0].Placed.RangeReducedByBudget);
        // The dispatch shape travelled with the slot rather than being re-read.
        EXPECT_EQ(latched[0].Placed.Dispatched.Capacity, 4194304u);
        EXPECT_EQ(latched[0].Placed.Dispatched.PlannedCandidates, 370000u);
        // The whole block travelled, not just the first draw's InstanceCount.
        EXPECT_EQ(latched[0].Placed.CandidatesConsidered, 900000u);
        EXPECT_EQ(latched[0].Placed.CellsVisible, 5000u);
        EXPECT_EQ(latched[0].Placed.AcceptedBlades, 800000u);
        EXPECT_FLOAT_EQ(latched[0].Placed.FittedRangeScale, 1.0f);
        EXPECT_EQ(latched[0].Placed.PlacedPerLod[0], kPlaced);
        EXPECT_EQ(latched[0].Placed.PlacedPerLod[1], 0u);

        stats.Destroy(dev.get());
    }
    dev->Shutdown();
}

TEST(TerrainGrassPlacementStats, ZeroPlacedResolvesAsAMeasuredZero)
{
    auto dev = MakeDevice();
    if (!dev)
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(dev.get());
        RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        TerrainGrassPlacementStats stats;
        constexpr ViewId kView = 1u;

        // Grass switched off / everything culled: the compute places nothing.
        RunPlacementFrame(dev.get(), frame, stats, kView, 0u, 0, MakeDispatch(4194304u));
        stats.Poll(dev.get());

        const auto latched = stats.LatchedPlacements();
        ASSERT_EQ(latched.size(), 1u) << "a zero placement is still a resolved sample";
        EXPECT_EQ(latched[0].Placed.PlacedInstances, 0u);
        EXPECT_FALSE(latched[0].Placed.RangeReducedByBudget)
            << "a scale of 1 means the budget cost this view no range";

        stats.Destroy(dev.get());
    }
    dev->Shutdown();
}

TEST(TerrainGrassPlacementStats, CountMovesWithThePlacedTotal)
{
    auto dev = MakeDevice();
    if (!dev)
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(dev.get());
        RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        TerrainGrassPlacementStats stats;
        constexpr ViewId kView = 2u;

        RunPlacementFrame(dev.get(), frame, stats, kView, 1000u, 0, MakeDispatch(4194304u));
        stats.Poll(dev.get());
        ASSERT_EQ(stats.LatchedPlacements().size(), 1u);
        EXPECT_EQ(stats.LatchedPlacements()[0].Placed.PlacedInstances, 1000u);

        // A second frame placing a different total must be observed, not latched over:
        // an instrument that reported its first reading forever would look healthy.
        RunPlacementFrame(dev.get(), frame, stats, kView, 7u, 1, MakeDispatch(4194304u));
        stats.Poll(dev.get());
        ASSERT_EQ(stats.LatchedPlacements().size(), 1u);
        EXPECT_EQ(stats.LatchedPlacements()[0].Placed.PlacedInstances, 7u);

        stats.Destroy(dev.get());
    }
    dev->Shutdown();
}

TEST(TerrainGrassPlacementStats, ReachingCapacityIsReportedAsARefusal)
{
    auto dev = MakeDevice();
    if (!dev)
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(dev.get());
        RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        TerrainGrassPlacementStats stats;
        constexpr ViewId kView = 5u;
        constexpr uint32 kCapacity = 4096u;

        RunPlacementFrame(dev.get(), frame, stats, kView, kCapacity, 0, MakeDispatch(kCapacity));
        stats.Poll(dev.get());

        const auto latched = stats.LatchedPlacements();
        ASSERT_EQ(latched.size(), 1u);
        // A full pool refuses nothing, so the placed count IS the total; the budget shows up as a
        // shortened field, and at scale 1 the field is not shortened.
        EXPECT_EQ(latched[0].Placed.PlacedInstances, kCapacity);
        EXPECT_FALSE(latched[0].Placed.RangeReducedByBudget);

        stats.Destroy(dev.get());
    }
    dev->Shutdown();
}

TEST(TerrainGrassPlacementStats, TwoViewsLatchIndependently)
{
    auto dev = MakeDevice();
    if (!dev)
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(dev.get());
        RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        TerrainGrassPlacementStats stats;

        // Placement is per view (the camera cull differs), so a scene view and a
        // game view must not share or overwrite one another's count.
        RunPlacementFrame(dev.get(), frame, stats, 1u, 111u, 0, MakeDispatch(4194304u));
        stats.Poll(dev.get());
        RunPlacementFrame(dev.get(), frame, stats, 2u, 222u, 1, MakeDispatch(4194304u));
        stats.Poll(dev.get());

        const auto latched = stats.LatchedPlacements();
        ASSERT_EQ(latched.size(), 2u);
        // LatchedPlacements sorts by view id so two captures can be diffed.
        EXPECT_EQ(latched[0].View, 1u);
        EXPECT_EQ(latched[0].Placed.PlacedInstances, 111u);
        EXPECT_EQ(latched[1].View, 2u);
        EXPECT_EQ(latched[1].Placed.PlacedInstances, 222u);

        stats.Destroy(dev.get());
    }
    dev->Shutdown();
}

TEST(TerrainGrassPlacementStats, GoingIdleReplacesTheLatchedCountWithAMeasuredZero)
{
    auto dev = MakeDevice();
    if (!dev)
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(dev.get());
        RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        TerrainGrassPlacementStats stats;
        constexpr ViewId kView = 6u;

        RunPlacementFrame(dev.get(), frame, stats, kView, 363032u, 0, MakeDispatch(4194304u));
        stats.Poll(dev.get());
        ASSERT_EQ(stats.LatchedPlacements().size(), 1u);
        ASSERT_EQ(stats.LatchedPlacements()[0].Placed.PlacedInstances, 363032u);

        // Grass switched off: the node stops declaring, so no further slot is ever
        // written. The count must become zero rather than keep serving the last
        // healthy reading — the live-editor defect this test pins.
        stats.RecordIdle(kView);
        stats.Poll(dev.get());
        const auto latched = stats.LatchedPlacements();
        ASSERT_EQ(latched.size(), 1u) << "idle is a measured zero, not an absence";
        EXPECT_EQ(latched[0].Placed.PlacedInstances, 0u);
        EXPECT_EQ(latched[0].Placed.Dispatched.Capacity, 0u);

        // Declaring again resumes normal reporting.
        RunPlacementFrame(dev.get(), frame, stats, kView, 42u, 1, MakeDispatch(4194304u));
        stats.Poll(dev.get());
        EXPECT_EQ(stats.LatchedPlacements()[0].Placed.PlacedInstances, 42u);

        stats.Destroy(dev.get());
    }
    dev->Shutdown();
}

TEST(TerrainGrassPlacementStats, AnInFlightSlotCannotReviveAnIdleView)
{
    auto dev = MakeDevice();
    if (!dev)
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(dev.get());
        RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        TerrainGrassPlacementStats stats;
        constexpr ViewId kView = 7u;

        // Write a slot and go idle WITHOUT polling in between, so the completed
        // slot is still unread when the view stops declaring. Polling must not
        // resurrect it: it describes a dispatch that no longer happens.
        RunPlacementFrame(dev.get(), frame, stats, kView, 99999u, 0, MakeDispatch(4194304u));
        stats.RecordIdle(kView);
        stats.Poll(dev.get());

        const auto latched = stats.LatchedPlacements();
        ASSERT_EQ(latched.size(), 1u);
        EXPECT_EQ(latched[0].Placed.PlacedInstances, 0u);

        stats.Destroy(dev.get());
    }
    dev->Shutdown();
}

TEST(TerrainGrassPlacementStats, ABrokenFrameReportsZeroRatherThanTheLastHealthyCount)
{
    auto dev = MakeDevice();
    if (!dev)
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(dev.get());
        RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        TerrainGrassPlacementStats stats;
        constexpr ViewId kView = 9u;

        RunPlacementFrame(dev.get(), frame, stats, kView, 363032u, 0, MakeDispatch(4194304u));
        stats.Poll(dev.get());
        ASSERT_EQ(stats.LatchedPlacements()[0].Placed.PlacedInstances, 363032u);

        // The node has several non-placing early returns that are not "grass is off"
        // but "grass is BROKEN this frame" — no device, no terrain feature, the
        // feature failed to initialise, placement buffers failed to allocate. They all
        // leave without taking a slot, and the node's scope guard reports that as
        // idle. Whatever the reason, the honest answer is zero placed; serving the
        // last healthy count is at its most misleading on exactly these frames.
        stats.RecordIdle(kView);
        stats.Poll(dev.get());

        const auto latched = stats.LatchedPlacements();
        ASSERT_EQ(latched.size(), 1u);
        EXPECT_EQ(latched[0].Placed.PlacedInstances, 0u);
        EXPECT_EQ(latched[0].Placed.Dispatched.Capacity, 0u);

        stats.Destroy(dev.get());
    }
    dev->Shutdown();
}

TEST(TerrainGrassPlacementStats, ResumingAfterIdleCannotServeThePreIdleCount)
{
    auto dev = MakeDevice();
    if (!dev)
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(dev.get());
        RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        TerrainGrassPlacementStats stats;
        constexpr ViewId kView = 8u;

        // Production cadence: a slot is written EVERY frame but Poll only runs when
        // someone asks for the payload, so a completed-but-unconsumed pending at the
        // idle boundary is the normal case, not a corner.
        RunPlacementFrame(dev.get(), frame, stats, kView, 363032u, 0, MakeDispatch(4194304u));

        // Grass switched off. The gate stops that pending resolving...
        stats.RecordIdle(kView);
        stats.Poll(dev.get());
        ASSERT_EQ(stats.LatchedPlacements()[0].Placed.PlacedInstances, 0u);

        // ...and now grass resumes. The gate opens again on the DECLARE, before the
        // resumed frame's own slot can possibly have completed. MapNewestReady scans
        // newest-first and skips the unstamped new pending, so a surviving pre-idle
        // pending is what it hands back — carrying its own dispatch shape, which makes
        // the payload self-consistently wrong and therefore undetectable.
        const BufferHandle resumed =
            stats.AcquireSlotRG(dev.get(), frame, kView, MakeDispatch(4194304u));
        ASSERT_TRUE(resumed.IsValid());
        stats.Poll(dev.get());

        const auto latched = stats.LatchedPlacements();
        ASSERT_EQ(latched.size(), 1u);
        EXPECT_EQ(latched[0].Placed.PlacedInstances, 0u)
            << "the first poll after resume served the count from before the off-period; "
               "going idle must DROP the view's pendings, not merely gate them";

        stats.Destroy(dev.get());
    }
    dev->Shutdown();
}

TEST(TerrainGrassPlacementStats, DeviceRebuildDropsLatchedSamples)
{
    auto dev = MakeDevice();
    if (!dev)
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(dev.get());
        RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        TerrainGrassPlacementStats stats;
        constexpr ViewId kView = 4u;

        RunPlacementFrame(dev.get(), frame, stats, kView, 500u, 0, MakeDispatch(4194304u));
        stats.Poll(dev.get());
        ASSERT_EQ(stats.LatchedPlacements().size(), 1u);

        stats.RecordDropout(kView, TerrainGrassPlacementStats::Dropout::ZeroGrass);

        // After a rebuild nothing has been measured on the new device; continuing to
        // report the old count as current would be a stale reading presented as live.
        // The dropout history goes with it, deliberately: the views are re-created on the new
        // device and a tally carried across would be attributed to whatever view id lands here
        // next, which is a worse lie than starting the count over.
        stats.OnDeviceRebuilt(dev.get());
        EXPECT_TRUE(stats.LatchedPlacements().empty());

        stats.Destroy(dev.get());
    }
    dev->Shutdown();
}

// ── Liveness: a row that stops being refreshed must stop reading as current ───

TEST(TerrainGrassPlacementStats, AViewThatStopsBeingDeclaredStopsReadingAsCurrent)
{
    auto dev = MakeDevice();
    if (!dev)
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(dev.get());
        RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        TerrainGrassPlacementStats stats;
        constexpr ViewId kLive = 9u;    // scene view: keeps declaring
        constexpr ViewId kStopped = 1u; // game view: stops being declared at all
        uint64 rgFrame = 0;

        RunPlacementFrame(dev.get(), frame, stats, kLive, 167551u, rgFrame++,
                          MakeDispatch(4194304u));
        RunPlacementFrame(dev.get(), frame, stats, kStopped, 191406u, rgFrame++,
                          MakeDispatch(4194304u));
        stats.Poll(dev.get());
        ASSERT_EQ(stats.LatchedPlacements().size(), 2u);

        // The node stops being CALLED for this view — its pane stopped rendering, its window went
        // away. Nothing marks it idle: the idle guard is a scope guard INSIDE the declare, so it
        // cannot fire for a declare that never happens, and Poll leaves an unrefreshed view's
        // sample untouched. Meanwhile the rest of the editor keeps rendering, which is what
        // advances the clock.
        for (uint32 i = 0; i <= TerrainGrassPlacementStats::kCurrentWithinFrames; ++i)
            RunPlacementFrame(dev.get(), frame, stats, kLive, 167551u, rgFrame++,
                              MakeDispatch(4194304u));
        stats.Poll(dev.get());

        const auto latched = stats.LatchedPlacements();
        ASSERT_EQ(latched.size(), 2u);
        const auto& stopped = latched[0]; // ascending by view id: 1 before 9
        const auto& live = latched[1];
        ASSERT_EQ(stopped.View, kStopped);
        ASSERT_EQ(live.View, kLive);

        // The control. Without it "stale" could just mean the flag is always false.
        EXPECT_TRUE(live.IsCurrent) << "a view declaring every frame must not be called stale";
        EXPECT_FALSE(stopped.IsCurrent)
            << "a view that stopped being declared went on serving its last healthy count as "
               "current — the live-editor defect this pins (191406 blades for a view that renders "
               "nothing)";
        EXPECT_GT(stopped.FramesSinceDeclared, live.FramesSinceDeclared);
        EXPECT_EQ(live.LastDeclaredFrameIndex, stats.FrameIndex() - 1u);
        // The number is reported stale, not erased. Zeroing it would forge a zero-grass frame,
        // which is a different and real signature; the honest answer is the last measurement
        // plus how old it is.
        EXPECT_EQ(stopped.Placed.PlacedInstances, 191406u);

        stats.Destroy(dev.get());
    }
    dev->Shutdown();
}

// ── Dropout counters: always on, split by what the frame looks like ───────────

TEST(TerrainGrassPlacementStats, DropoutsAreCountedByClassEvenWithNoResolvedSample)
{
    using Dropout = TerrainGrassPlacementStats::Dropout;
    TerrainGrassPlacementStats stats;
    constexpr ViewId kView = 3u;

    stats.RecordDropout(kView, Dropout::StaleArgs);
    stats.RecordDropout(kView, Dropout::ZeroGrass);
    stats.RecordDropout(kView, Dropout::StaleArgs);

    const auto latched = stats.LatchedPlacements();
    // A dropout is the whole reason to look at this payload, so it has to surface for a view that
    // has never resolved a sample too — otherwise the row does not exist and the count is
    // unreachable from get_terrain_debug.
    ASSERT_EQ(latched.size(), 1u);
    EXPECT_EQ(latched[0].View, kView);
    EXPECT_FALSE(latched[0].HasSample)
        << "nothing resolved, so the placement counts on this row are defaults, not a measurement";
    // Split by class: only the zero-grass half shows up as a missing blade count. The stale-args
    // half keeps a healthy count and is invisible to every count-shaped detector.
    EXPECT_EQ(latched[0].StaleArgFrames, 2u);
    EXPECT_EQ(latched[0].ZeroGrassFrames, 1u);
}

TEST(TerrainGrassPlacementStats, DropoutsAreStampedAndSurviveRecovery)
{
    auto dev = MakeDevice();
    if (!dev)
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(dev.get());
        RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        TerrainGrassPlacementStats stats;
        constexpr ViewId kView = 2u;
        uint64 rgFrame = 0;

        RunPlacementFrame(dev.get(), frame, stats, kView, 5000u, rgFrame++, MakeDispatch(4194304u));
        RunPlacementFrame(dev.get(), frame, stats, kView, 5000u, rgFrame++, MakeDispatch(4194304u));
        stats.Poll(dev.get());
        ASSERT_EQ(stats.LatchedPlacements().size(), 1u);
        // Counting has to be free on the frames that work, so healthy frames must move nothing.
        ASSERT_EQ(stats.LatchedPlacements()[0].ZeroGrassFrames, 0u);
        ASSERT_EQ(stats.LatchedPlacements()[0].StaleArgFrames, 0u);

        const uint32 dropoutFrame = stats.FrameIndex();
        stats.RecordDropout(kView, TerrainGrassPlacementStats::Dropout::ZeroGrass);
        RunPlacementFrame(dev.get(), frame, stats, kView, 5000u, rgFrame++, MakeDispatch(4194304u));
        stats.Poll(dev.get());

        const auto latched = stats.LatchedPlacements();
        ASSERT_EQ(latched.size(), 1u);
        EXPECT_EQ(latched[0].ZeroGrassFrames, 1u);
        EXPECT_EQ(latched[0].StaleArgFrames, 0u);
        // A one-frame dropout is over by the time anyone asks, so the stamp is what dates it:
        // grassFrameIndex - lastDropoutFrameIndex is how long ago the flash was.
        EXPECT_EQ(latched[0].LastDropoutFrameIndex, dropoutFrame);
        EXPECT_GT(stats.FrameIndex(), latched[0].LastDropoutFrameIndex);
        // Recovering must not erase the history — a monotonic count is the whole point.
        EXPECT_TRUE(latched[0].IsCurrent);

        stats.Destroy(dev.get());
    }
    dev->Shutdown();
}

TEST(TerrainGrassPlacementStats, GoingIdleKeepsTheDropoutHistory)
{
    auto dev = MakeDevice();
    if (!dev)
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(dev.get());
        RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        TerrainGrassPlacementStats stats;
        constexpr ViewId kView = 5u;

        RunPlacementFrame(dev.get(), frame, stats, kView, 1234u, 0, MakeDispatch(4194304u));
        stats.RecordDropout(kView, TerrainGrassPlacementStats::Dropout::StaleArgs);
        // Idle replaces the SAMPLE with a measured zero. It must not replace the dropout tally:
        // switching grass off is not evidence that the earlier dropped frames did not happen.
        stats.RecordIdle(kView);
        stats.Poll(dev.get());

        const auto latched = stats.LatchedPlacements();
        ASSERT_EQ(latched.size(), 1u);
        EXPECT_TRUE(latched[0].Idle);
        EXPECT_EQ(latched[0].Placed.PlacedInstances, 0u);
        EXPECT_EQ(latched[0].StaleArgFrames, 1u);

        stats.Destroy(dev.get());
    }
    dev->Shutdown();
}

// Tests for FrameBufferAllocator and PerFrameWritePool: double-buffered
// per-frame allocation, frame isolation, capacity limits, and integration
// with RenderServices.

#include <gtest/gtest.h>

#include "Engine/Rendering/PerFrameWritePool.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/FrameBufferAllocator.h"

#include <cstring>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;

#include "PerFrameWritePoolRotation.h"
#include "TestDeviceHelper.h"

// ---- FrameBufferAllocator tests ----

class FrameBufferAllocatorTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_device = CreateVulkanDeviceFast();
        if (!m_device)
            GTEST_SKIP() << "No Vulkan device available";
    }

    void TearDown() override
    {
        if (m_device)
            m_device->Shutdown();
    }

    std::unique_ptr<IDevice> m_device;
};

TEST_F(FrameBufferAllocatorTest, Initialize_CreatesValidAllocator)
{
    FrameBufferAllocator alloc;
    ASSERT_TRUE(alloc.Initialize(m_device.get(), 4096, BufferUsage::Storage, 2));
    EXPECT_TRUE(alloc.IsInitialized());
    EXPECT_EQ(alloc.GetCapacity(), 4096u);
    EXPECT_EQ(alloc.GetSlotCount(), 2u);
    alloc.Shutdown();
}

TEST_F(FrameBufferAllocatorTest, Allocate_ReturnsValidAllocation)
{
    FrameBufferAllocator alloc;
    ASSERT_TRUE(alloc.Initialize(m_device.get(), 4096, BufferUsage::Storage, 2));

    alloc.BeginFrame(0);
    auto a = alloc.Allocate(128);
    EXPECT_TRUE(a.IsValid());
    EXPECT_TRUE(a.buffer.IsValid());
    EXPECT_NE(a.ptr, nullptr);
    EXPECT_EQ(a.size, 128u);

    alloc.Shutdown();
}

TEST_F(FrameBufferAllocatorTest, MultipleAllocations_AreContiguous)
{
    FrameBufferAllocator alloc;
    ASSERT_TRUE(alloc.Initialize(m_device.get(), 4096, BufferUsage::Storage, 2, 4));

    alloc.BeginFrame(0);
    auto a1 = alloc.Allocate(64, 4);
    auto a2 = alloc.Allocate(64, 4);

    EXPECT_TRUE(a1.IsValid());
    EXPECT_TRUE(a2.IsValid());
    EXPECT_EQ(a1.buffer, a2.buffer) << "Same frame slot should use same buffer";
    EXPECT_GE(a2.offset, a1.offset + a1.size) << "Allocations must not overlap";

    alloc.Shutdown();
}

TEST_F(FrameBufferAllocatorTest, DifferentFrames_UseDifferentBuffers)
{
    FrameBufferAllocator alloc;
    ASSERT_TRUE(alloc.Initialize(m_device.get(), 4096, BufferUsage::Storage, 2));

    alloc.BeginFrame(0);
    auto buf0 = alloc.GetCurrentBuffer();

    alloc.BeginFrame(1);
    auto buf1 = alloc.GetCurrentBuffer();

    EXPECT_NE(buf0, buf1) << "Different frame slots must use different buffers";

    alloc.Shutdown();
}

TEST_F(FrameBufferAllocatorTest, BeginFrame_ResetsUsage)
{
    FrameBufferAllocator alloc;
    ASSERT_TRUE(alloc.Initialize(m_device.get(), 4096, BufferUsage::Storage, 2, 4));

    alloc.BeginFrame(0);
    alloc.Allocate(1024);
    EXPECT_GT(alloc.GetCurrentUsage(), 0u);

    // Same frame slot: should reset.
    alloc.BeginFrame(0);
    EXPECT_EQ(alloc.GetCurrentUsage(), 0u);

    alloc.Shutdown();
}

TEST_F(FrameBufferAllocatorTest, WriteAndVerifyData)
{
    FrameBufferAllocator alloc;
    ASSERT_TRUE(alloc.Initialize(m_device.get(), 4096, BufferUsage::Storage, 2, 4));

    alloc.BeginFrame(0);
    auto a = alloc.Allocate(sizeof(uint32_t) * 4);
    ASSERT_TRUE(a.IsValid());

    uint32_t data[] = {10, 20, 30, 40};
    std::memcpy(a.ptr, data, sizeof(data));

    // Verify the data is at the mapped pointer.
    const uint32_t* mapped = reinterpret_cast<const uint32_t*>(a.ptr);
    EXPECT_EQ(mapped[0], 10u);
    EXPECT_EQ(mapped[1], 20u);
    EXPECT_EQ(mapped[2], 30u);
    EXPECT_EQ(mapped[3], 40u);

    alloc.Shutdown();
}

// ---- PerFrameWritePool tests ----

class PerFrameWritePoolTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_device = CreateVulkanDeviceFast();
        if (!m_device)
            GTEST_SKIP() << "No Vulkan device available";
    }

    void TearDown() override
    {
        if (m_device)
            m_device->Shutdown();
    }

    std::unique_ptr<IDevice> m_device;
};

TEST_F(PerFrameWritePoolTest, Initialize_AllUsageClassesReady)
{
    PerFrameWritePool pool;
    PerFrameWritePoolConfig cfg{};
    cfg.FramesInFlight = 2;
    ASSERT_TRUE(pool.Initialize(m_device.get(), cfg));
    EXPECT_TRUE(pool.IsInitialized());

    // Each usage class should have non-zero capacity.
    EXPECT_GT(pool.GetCapacity(FrameWriteUsage::MaterialParams), 0u);
    EXPECT_GT(pool.GetCapacity(FrameWriteUsage::BonePalette), 0u);

    pool.Shutdown();
}

TEST_F(PerFrameWritePoolTest, AllocateMaterialParams_Works)
{
    PerFrameWritePool pool;
    PerFrameWritePoolConfig cfg{};
    cfg.FramesInFlight = 2;
    ASSERT_TRUE(pool.Initialize(m_device.get(), cfg));

    pool.BeginFrame(0);

    // Write 100 words.
    constexpr uint32_t N = 100;
    auto alloc = pool.Allocate(FrameWriteUsage::MaterialParams, N * sizeof(uint32_t));
    ASSERT_TRUE(alloc.IsValid());
    EXPECT_TRUE(alloc.buffer.IsValid());
    EXPECT_EQ(alloc.size, N * sizeof(uint32_t));

    // Write data.
    uint32_t* dst = reinterpret_cast<uint32_t*>(alloc.ptr);
    for (uint32_t i = 0; i < N; ++i)
        dst[i] = i;

    // Verify.
    for (uint32_t i = 0; i < N; ++i)
        EXPECT_EQ(dst[i], i);

    pool.Shutdown();
}

TEST_F(PerFrameWritePoolTest, DifferentUsageClasses_UseIndependentBuffers)
{
    PerFrameWritePool pool;
    PerFrameWritePoolConfig cfg{};
    cfg.FramesInFlight = 2;
    ASSERT_TRUE(pool.Initialize(m_device.get(), cfg));

    pool.BeginFrame(0);

    auto bufMaterial = pool.GetBuffer(FrameWriteUsage::MaterialParams);
    auto bufBones = pool.GetBuffer(FrameWriteUsage::BonePalette);

    // All should be valid and distinct.
    EXPECT_TRUE(bufMaterial.IsValid());
    EXPECT_TRUE(bufBones.IsValid());
    EXPECT_NE(bufMaterial, bufBones);

    pool.Shutdown();
}

TEST_F(PerFrameWritePoolTest, BeginFrame_IsolatesFrameSlots)
{
    PerFrameWritePool pool;
    PerFrameWritePoolConfig cfg{};
    cfg.FramesInFlight = 2;
    ASSERT_TRUE(pool.Initialize(m_device.get(), cfg));

    // Frame 0: allocate some data.
    pool.BeginFrame(0);
    const auto alloc0 = pool.Allocate(FrameWriteUsage::MaterialParams, 256);
    ASSERT_TRUE(alloc0.IsValid());
    EXPECT_GT(pool.GetCurrentUsage(FrameWriteUsage::MaterialParams), 0u);

    // Frame 1: different slot, usage should be 0.
    pool.BeginFrame(1);
    EXPECT_EQ(pool.GetCurrentUsage(FrameWriteUsage::MaterialParams), 0u);

    // Back to frame 0: resets, usage should be 0 again.
    pool.BeginFrame(0);
    EXPECT_EQ(pool.GetCurrentUsage(FrameWriteUsage::MaterialParams), 0u);

    pool.Shutdown();
}

// Integration: RenderServices owns the pool and it initializes correctly.
TEST_F(PerFrameWritePoolTest, RenderServicesOwnsPool)
{
    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(m_device.get()));
    EXPECT_TRUE(rs.GetPerFrameWritePool().IsInitialized());

    // Can allocate through the pool.
    rs.GetPerFrameWritePool().BeginFrame(0);
    auto alloc = rs.GetPerFrameWritePool().Allocate(
        FrameWriteUsage::MaterialParams, 64);
    EXPECT_TRUE(alloc.IsValid());

    rs.Shutdown();
}

// ---- Grow-on-demand tests (FrameBufferAllocator) ----

TEST_F(FrameBufferAllocatorTest, GrowOnDemand_DisabledByDefault)
{
    // Without maxCapacityBytes, an oversized allocation request is just
    // rejected and the ring stays at its initial capacity even after
    // a full BeginFrame cycle.
    FrameBufferAllocator alloc;
    constexpr size_t kInitial = 1024;
    ASSERT_TRUE(alloc.Initialize(m_device.get(), kInitial,
                                 BufferUsage::Storage, /*frames=*/2,
                                 /*align=*/16, "test", /*maxCap=*/0));

    alloc.BeginFrame(0);
    auto over = alloc.Allocate(2048, 16);
    EXPECT_FALSE(over.IsValid()) << "request larger than ring should fail";

    // Cycle through both frame slots back to slot 0; capacity should
    // remain unchanged because grow-on-demand is disabled.
    alloc.BeginFrame(1);
    alloc.BeginFrame(0);
    EXPECT_EQ(alloc.GetCapacity(), kInitial);

    alloc.Shutdown();
}

TEST_F(FrameBufferAllocatorTest, GrowOnDemand_GrowsOnNextCycle)
{
    // With maxCapacityBytes set, an overflow on frame N causes the ring
    // to grow at the next BeginFrame for that slot — for framesInFlight=2
    // that's frame N+2.
    FrameBufferAllocator alloc;
    constexpr size_t kInitial = 1024;
    constexpr size_t kMax = 64u << 10; // 64 KB cap
    ASSERT_TRUE(alloc.Initialize(m_device.get(), kInitial,
                                 BufferUsage::Storage, /*frames=*/2,
                                 /*align=*/16, "test_grow", kMax));

    // Frame 0 (slot 0): try 4 KB request (4x initial), fails. Ring tracks
    // demand = 4 KB.
    alloc.BeginFrame(0);
    EXPECT_EQ(alloc.GetCapacity(), kInitial);
    auto over = alloc.Allocate(4096, 16);
    EXPECT_FALSE(over.IsValid());

    // Frame 1 (slot 1): different slot, no grow.
    alloc.BeginFrame(1);
    EXPECT_EQ(alloc.GetCapacity(), kInitial);

    // Frame 2 (slot 0 again): BeginFrame should grow this slot before
    // resetting because last cycle's demand was 4 KB > 1 KB capacity.
    alloc.BeginFrame(2);
    EXPECT_GE(alloc.GetCapacity(), 4096u)
        << "ring should have grown to cover prior overflow";
    EXPECT_LE(alloc.GetCapacity(), kMax);

    // Now the same 4 KB request fits.
    auto fit = alloc.Allocate(4096, 16);
    EXPECT_TRUE(fit.IsValid());

    alloc.Shutdown();
}

// An allocation that does not fit a ring that served nothing this frame grows
// it at once, so the frame gets its data instead of waiting a ring cycle; once
// the ring has served an allocation, a later one that does not fit is refused.
TEST_F(FrameBufferAllocatorTest, Reserve_GrowsBeforeAllocationAndPreservesLiveRegions)
{
    FrameBufferAllocator alloc;
    constexpr size_t kInitial = 1024;
    constexpr size_t kMax = 64u << 10;
    ASSERT_TRUE(alloc.Initialize(m_device.get(), kInitial,
                                 BufferUsage::Storage, /*frames=*/2,
                                 /*align=*/16, "test_grow_now", kMax));

    alloc.BeginFrame(0);
    const auto initialBuffer = alloc.GetCurrentBuffer();
    EXPECT_FALSE(alloc.Reserve(kMax + 1));
    EXPECT_EQ(alloc.GetCurrentBuffer(), initialBuffer);
    EXPECT_EQ(alloc.GetCurrentUsage(), 0u);
    ASSERT_TRUE(alloc.Reserve(4096));
    EXPECT_EQ(alloc.GetCurrentUsage(), 0u);
    auto grown = alloc.Allocate(4096, 16);
    EXPECT_TRUE(grown.IsValid());
    EXPECT_GE(alloc.GetCapacity(), 4096u);
    EXPECT_EQ(grown.buffer, alloc.GetCurrentBuffer());

    const auto buffer = alloc.GetCurrentBuffer();
    EXPECT_FALSE(alloc.Reserve(8192));
    EXPECT_EQ(alloc.GetCurrentBuffer(), buffer);
    auto afterServe = alloc.Allocate(alloc.GetCapacity(), 16);
    EXPECT_FALSE(afterServe.IsValid()) << "a ring with a live region must not be recreated";

    alloc.Shutdown();
}

TEST_F(FrameBufferAllocatorTest, GrowOnDemand_RespectsMaxCap)
{
    FrameBufferAllocator alloc;
    constexpr size_t kInitial = 1024;
    constexpr size_t kMax = 4096; // grow up to 4x only
    ASSERT_TRUE(alloc.Initialize(m_device.get(), kInitial,
                                 BufferUsage::Storage, /*frames=*/2,
                                 /*align=*/16, "test_cap", kMax));

    alloc.BeginFrame(0);
    // Demand 16 KB — far over cap.
    auto big = alloc.Allocate(16u << 10, 16);
    EXPECT_FALSE(big.IsValid());

    alloc.BeginFrame(1);
    alloc.BeginFrame(2); // back to slot 0; should grow but not past cap.
    EXPECT_EQ(alloc.GetCapacity(), kMax)
        << "growth must clamp at maxCapacityBytes";

    // 16 KB still fails (over cap), but 4 KB fits.
    auto stillBig = alloc.Allocate(16u << 10, 16);
    EXPECT_FALSE(stillBig.IsValid());
    auto inCap = alloc.Allocate(4096, 16);
    EXPECT_TRUE(inCap.IsValid());

    alloc.Shutdown();
}

// ---- Previous-frame slot access (TAA skinned motion vectors) ----
//
// The skinned MV pass reads LAST frame's bone palettes, which extends a ring's
// GPU read lifetime one frame past what the device's fence guarantees. These
// tests pin the two properties that make that safe: the accessor only reports a
// previous frame once one exists, and the BonePalette class carries a slot more
// than the device paces.

TEST_F(FrameBufferAllocatorTest, PreviousFrame_UnavailableUntilSecondBeginFrame)
{
    FrameBufferAllocator alloc;
    ASSERT_TRUE(alloc.Initialize(m_device.get(), 4096, BufferUsage::Storage, 3));

    EXPECT_FALSE(alloc.HasPreviousFrame()) << "no frame has been begun yet";

    alloc.BeginFrame(0);
    EXPECT_FALSE(alloc.HasPreviousFrame())
        << "the first frame has no predecessor to read";

    alloc.BeginFrame(1);
    ASSERT_TRUE(alloc.HasPreviousFrame());
    EXPECT_TRUE(alloc.GetPreviousBuffer().IsValid());
    EXPECT_FALSE(alloc.GetPreviousBuffer() == alloc.GetCurrentBuffer())
        << "previous slot must be a different ring than the one being written";

    alloc.Shutdown();
}

TEST_F(FrameBufferAllocatorTest, PreviousFrame_RepeatedFrameIndexHasNoPredecessor)
{
    FrameBufferAllocator alloc;
    ASSERT_TRUE(alloc.Initialize(m_device.get(), 4096, BufferUsage::Storage, 3));

    alloc.BeginFrame(0);
    alloc.BeginFrame(1);
    ASSERT_TRUE(alloc.HasPreviousFrame());

    // Re-beginning the SAME index resets the ring that would be "previous",
    // so its contents are gone and the accessor must say so.
    alloc.BeginFrame(1);
    EXPECT_FALSE(alloc.HasPreviousFrame())
        << "a repeated frame index resets the slot it would otherwise read";

    alloc.Shutdown();
}

TEST_F(FrameBufferAllocatorTest, PreviousFrame_SingleSlotNeverReportsPredecessor)
{
    FrameBufferAllocator alloc;
    ASSERT_TRUE(alloc.Initialize(m_device.get(), 4096, BufferUsage::Storage, 1));

    alloc.BeginFrame(0);
    alloc.BeginFrame(1);
    EXPECT_FALSE(alloc.HasPreviousFrame())
        << "one ring means current and previous are the same memory";

    alloc.Shutdown();
}

// Drive the allocator with the PRODUCTION frame index domain.
// VulkanDevice::GetFrameIndex() wraps at MAX_FRAMES_IN_FLIGHT=3 (every advance
// site is `(m_CurrentFrame + 1) % MAX_FRAMES_IN_FLIGHT`), so a 4-ring allocator
// fed that index must still rotate through all four rings for the ExtraSlots
// fence margin to exist. Selecting the ring by the index's VALUE caps the
// effective rotation at the device's pacing and makes ring 3 unreachable.
TEST_F(FrameBufferAllocatorTest, PreviousFrame_WrappedDeviceCounterRotatesAllRings)
{
    FrameBufferAllocator alloc;
    // 4 rings = devicePacing + 1, as PerFrameWritePool configures BonePalette.
    ASSERT_TRUE(alloc.Initialize(m_device.get(), 4096, BufferUsage::Storage, 4));

    alloc.BeginFrame(0);
    const BufferHandle ringAtF = alloc.GetCurrentBuffer(); // written at frame F
    alloc.BeginFrame(1); // F+1: the skinned MV prev-read of ringAtF is in flight
    alloc.BeginFrame(2); // F+2
    alloc.BeginFrame(0); // F+3: the wrapped device counter repeats 0

    // With 4 rings the slot written at F must not be rewritten before F+4:
    // at F+3 the device has only fenced frame F, and frame F+1's prev-read
    // of ringAtF may still be executing.
    EXPECT_FALSE(alloc.GetCurrentBuffer() == ringAtF)
        << "wrapped device counter (0,1,2,0,...) reuses the frame-F ring at "
           "F+3: the ExtraSlots fence margin does not exist in production";

    alloc.Shutdown();
}

// The safety property the ExtraSlots ring is bought for, stated directly: over
// a full wrapped-domain cycle every ring is distinct, so the reuse distance is
// the ring count (4) rather than the device's pacing (3).
TEST_F(FrameBufferAllocatorTest, PreviousFrame_ReuseDistanceIsRingCountNotDevicePacing)
{
    constexpr uint32_t kDevicePacing = 3;
    constexpr uint32_t kRings = kDevicePacing + 1;

    FrameBufferAllocator alloc;
    ASSERT_TRUE(alloc.Initialize(m_device.get(), 4096, BufferUsage::Storage, kRings));

    BufferHandle seen[kRings]{};
    for (uint32_t f = 0; f < kRings; ++f)
    {
        alloc.BeginFrame(f % kDevicePacing); // exactly what the device reports
        seen[f] = alloc.GetCurrentBuffer();
    }
    for (uint32_t a = 0; a < kRings; ++a)
    {
        for (uint32_t b = a + 1; b < kRings; ++b)
        {
            EXPECT_FALSE(seen[a] == seen[b])
                << "rings " << a << " and " << b << " are the same buffer: the "
                   "rotation collapsed to fewer than " << kRings << " slots";
        }
    }

    // Frame kRings is the first that may reuse ring 0. Whether that is SAFE depends
    // on the writer's phase, not on this rotation: behind BeginFrame the device has
    // proven through F+1 by then, in the update phase only through F. This test pins
    // the rotation; PerFrameWritePoolTest.RingsCarryTheUpdatePhaseMargin pins the depth.
    alloc.BeginFrame(kRings % kDevicePacing);
    EXPECT_TRUE(alloc.GetCurrentBuffer() == seen[0])
        << "the cursor must cycle, not run off into fresh buffers";

    alloc.Shutdown();
}

// A device that does not advance (window gate closed, present skipped) reports
// the same index twice. The cursor must hold: claiming the next ring inside one
// device frame would spend the rotation margin the fence argument depends on.
TEST_F(FrameBufferAllocatorTest, PreviousFrame_RepeatedIndexHoldsTheCursor)
{
    FrameBufferAllocator alloc;
    ASSERT_TRUE(alloc.Initialize(m_device.get(), 4096, BufferUsage::Storage, 4));

    alloc.BeginFrame(0);
    alloc.BeginFrame(1);
    const BufferHandle ringAtF = alloc.GetCurrentBuffer();
    alloc.BeginFrame(1);
    EXPECT_TRUE(alloc.GetCurrentBuffer() == ringAtF)
        << "a repeated device index is the same frame, not the next one";
    EXPECT_FALSE(alloc.HasPreviousFrame())
        << "the ring just reset is the one a reader would have called previous";

    alloc.Shutdown();
}

// The depth rule for this pool, stated where it can fail. Every usage class is
// filled during the application UPDATE phase (RenderingLoop::Update, before the
// frame's IDevice::BeginFrame), so at the moment of the write the newest frame the
// device has PROVEN complete is devicePacing + 1 frames back, not devicePacing. A
// ring sized at the device's pacing therefore hands the CPU a slot a frame in
// flight is still reading — silent corruption, not a stall. A class whose slot is
// still read by later frames (BonePalette: the previous frame's palettes) needs one
// more ring per such frame.
//
// Measured for every class as distinct buffers over a rotation driven by the
// device's wrapped frame slot, rather than read from a getter: the depth only
// matters through which buffer comes back.
TEST_F(PerFrameWritePoolTest, RingsCarryTheUpdatePhaseMargin)
{
    PerFrameWritePool pool;
    ASSERT_TRUE(pool.Initialize(m_device.get()));

    const uint32_t devicePacing = m_device->GetFramesInFlight();
    ASSERT_GT(devicePacing, 1u) << "a paced device is the premise of this test";

    // {usage, frames beyond the writing one that still read the slot}
    const struct
    {
        FrameWriteUsage Usage;
        uint32_t ExtraReaderFrames;
        const char* Name;
    } kClasses[] = {
        {FrameWriteUsage::MaterialParams, 0, "MaterialParams"},
        {FrameWriteUsage::BonePalette, 1, "BonePalette"},
    };

    for (const auto& c : kClasses)
    {
        const uint32_t required = devicePacing + 1u + c.ExtraReaderFrames;
        std::vector<Rendering::BufferHandle> seen;
        for (uint32_t f = 0; f < required; ++f)
        {
            // Exactly the domain RenderingLoop passes: the device's WRAPPED slot.
            pool.BeginFrame(f % devicePacing);
            const Rendering::BufferHandle buf = pool.GetBuffer(c.Usage);
            ASSERT_TRUE(buf.IsValid()) << c.Name;
            for (const auto& prior : seen)
            {
                EXPECT_FALSE(prior == buf)
                    << c.Name << " reused a ring after " << seen.size()
                    << " frames, but an update-phase write needs " << required
                    << ": the slot being written is one a frame in flight still reads";
            }
            seen.push_back(buf);
        }
    }

    pool.Shutdown();
}

TEST_F(PerFrameWritePoolTest, BonePalette_CarriesAnExtraSlotForPreviousFrameReads)
{
    PerFrameWritePool pool;
    ASSERT_TRUE(pool.Initialize(m_device.get()));

    pool.BeginFrame(0);
    EXPECT_FALSE(pool.HasPreviousFrameBuffer(FrameWriteUsage::BonePalette))
        << "first frame has no previous palette";

    pool.BeginFrame(1);
    EXPECT_TRUE(pool.HasPreviousFrameBuffer(FrameWriteUsage::BonePalette))
        << "BonePalette declares one extra reader frame, so the prior slot is still live";
    EXPECT_TRUE(pool.GetPreviousBuffer(FrameWriteUsage::BonePalette).IsValid());
    EXPECT_FALSE(pool.GetPreviousBuffer(FrameWriteUsage::BonePalette) ==
                 pool.GetBuffer(FrameWriteUsage::BonePalette));

    // Classes without the extra slot must refuse: reading their previous slot
    // would race the CPU write that reuses it while the GPU is still in flight.
    EXPECT_FALSE(pool.HasPreviousFrameBuffer(FrameWriteUsage::MaterialParams))
        << "a class sized at the device's pacing has no safe previous slot";

    pool.Shutdown();
}

// ---- MaterialParams grow-on-demand (PerFrameWritePool defaults) ----
//
// PackMaterialSSBO packs every registered material into ONE allocation of
// materialCount * kMaterialEntryStride bytes, so the whole scene's material
// table either fits a ring slot or nothing is written at all. These pin the
// configured growth envelope: the starting capacity is a soft budget, the
// maxCapacityBytes cap is the hard one.

// "One full rotation" below is the ring's DEPTH, which is deeper than the device's
// pacing (PerFrameWritePool adds the update-phase margin), so a loop counting device
// frames stops short of the slot that recorded the demand. Each step passes the
// device's wrapped frame slot, exactly the domain RenderingLoop passes.

TEST_F(PerFrameWritePoolTest, MaterialParams_GrowsPastItsStartingCapacity)
{
    PerFrameWritePool pool;
    ASSERT_TRUE(pool.Initialize(m_device.get()));

    const size_t startCapacity = pool.GetCapacity(FrameWriteUsage::MaterialParams);
    ASSERT_GT(startCapacity, 0u);
    ASSERT_GT(pool.GetMaxCapacity(FrameWriteUsage::MaterialParams), startCapacity)
        << "MaterialParams must be configured to grow on demand";

    const size_t demand = startCapacity * 2;
    const uint32_t devicePacing = m_device->GetFramesInFlight();

    // The overflowing frame itself still fails — growth happens at the NEXT
    // BeginFrame for this slot, one full rotation later.
    pool.BeginFrame(0);
    EXPECT_FALSE(pool.Allocate(FrameWriteUsage::MaterialParams, demand, 16).IsValid())
        << "a request past the ring slot's capacity must fail, not silently truncate";

    uint32_t frame = 0;
    Testing::RotateBackToCurrentSlot(pool, FrameWriteUsage::MaterialParams,
                                     [&] { pool.BeginFrame(++frame % devicePacing); });

    EXPECT_GE(pool.GetCapacity(FrameWriteUsage::MaterialParams), demand)
        << "the slot must grow to cover the demand it recorded";
    EXPECT_TRUE(pool.Allocate(FrameWriteUsage::MaterialParams, demand, 16).IsValid());

    pool.Shutdown();
}

TEST_F(PerFrameWritePoolTest, MaterialParams_RefusesAllocationsPastTheGrowCap)
{
    PerFrameWritePool pool;
    ASSERT_TRUE(pool.Initialize(m_device.get()));

    const size_t maxCapacity = pool.GetMaxCapacity(FrameWriteUsage::MaterialParams);
    ASSERT_GT(maxCapacity, 0u);

    const size_t overCap = maxCapacity + (1u << 20);
    const uint32_t devicePacing = m_device->GetFramesInFlight();

    pool.BeginFrame(0);
    EXPECT_FALSE(pool.Allocate(FrameWriteUsage::MaterialParams, overCap, 16).IsValid());

    uint32_t frame = 0;
    Testing::RotateBackToCurrentSlot(pool, FrameWriteUsage::MaterialParams,
                                     [&] { pool.BeginFrame(++frame % devicePacing); });

    EXPECT_EQ(pool.GetCapacity(FrameWriteUsage::MaterialParams), maxCapacity)
        << "growth must clamp at the configured cap";
    EXPECT_FALSE(pool.Allocate(FrameWriteUsage::MaterialParams, overCap, 16).IsValid())
        << "past the cap the allocation must keep failing — growth cannot rescue it";

    pool.Shutdown();
}

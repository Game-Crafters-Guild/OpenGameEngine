// Stage 1.2 / 2a isolation tests for the per-frame upload ring and the N-buffer
// rotating import, over the REAL engine IDevice (headless Vulkan). The ring's
// pointers are real persistently-mapped host-visible memory.

#include "Rendering/Core/RenderGraph/RGRotatingImport.h"
#include "Rendering/Core/RenderGraph/RGUploadRing.h"
#include "Tests/RenderGraph/RGTestDevice.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

using namespace GameEngine::Rendering;     // TextureDesc/handles/enums/IDevice
using namespace GameEngine::Rendering::RenderGraph; // RenderGraph ring + rotating import
using GameEngine::Rendering::RenderGraph::Test::MakeHeadlessDevice;

namespace
{
TextureDesc Tex(uint32_t w, uint32_t h)
{
    TextureDesc d;
    d.width = w;
    d.height = h;
    d.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    d.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::RenderTarget);
    return d;
}
} // namespace

#define RG_REQUIRE_DEVICE(dev)                                                                        \
    auto dev = MakeHeadlessDevice();                                                                   \
    if (!dev)                                                                                          \
    GTEST_SKIP() << "no headless device available"

// ── Upload ring ─────────────────────────────────────────────────────────────

TEST(RGUploadRing, RingBuffersCarryVertexUsage)
{
    // The editor overlay passes (7e) bind ring allocs as vertex buffers,
    // folding the alloc offset into firstVertex. Dropping Vertex from the
    // ring usage silently breaks every gizmo draw on drivers without
    // validation — pin the mask.
    EXPECT_TRUE(RGUploadRing::kBufferUsage & static_cast<uint32_t>(BufferUsage::Vertex));
    EXPECT_TRUE(RGUploadRing::kBufferUsage & static_cast<uint32_t>(BufferUsage::Uniform));
    EXPECT_TRUE(RGUploadRing::kBufferUsage & static_cast<uint32_t>(BufferUsage::Storage));
}

TEST(RGUploadRing, SubAllocationsAreAlignedDistinctAndShareSlotBuffer)
{
    RG_REQUIRE_DEVICE(dev);
    RGUploadRing ring(dev.get(), /*framesInFlight=*/2, /*cap=*/4096);
    ring.BeginFrame(0);
    auto a = ring.Allocate(100, 256);
    auto b = ring.Allocate(50, 256);
    auto c = ring.Allocate(10, 256);
    ASSERT_TRUE(a.Valid() && b.Valid() && c.Valid());
    EXPECT_EQ(a.Offset, 0u);
    EXPECT_EQ(b.Offset, 256u);
    EXPECT_EQ(c.Offset, 512u);
    EXPECT_EQ(a.Buffer, b.Buffer);
    EXPECT_EQ(b.Buffer, c.Buffer);
    EXPECT_NE(a.Ptr, b.Ptr);
    EXPECT_EQ(ring.BytesUsedThisFrame(), 160u);
}

TEST(RGUploadRing, RotationUsesDistinctSlotsAndDoesNotClobberInFlight)
{
    RG_REQUIRE_DEVICE(dev);
    RGUploadRing ring(dev.get(), /*framesInFlight=*/2, /*cap=*/1024);

    ring.BeginFrame(0);
    auto a0 = ring.Allocate(64);
    ASSERT_TRUE(a0.Valid());
    std::memset(a0.Ptr, 0xAA, 64);
    const BufferHandle buf0 = a0.Buffer;

    ring.BeginFrame(1);
    auto a1 = ring.Allocate(64);
    ASSERT_TRUE(a1.Valid());
    EXPECT_NE(a1.Buffer, buf0) << "frame 1 uses a different slot";
    std::memset(a1.Ptr, 0xBB, 64);
    EXPECT_EQ(static_cast<uint8_t*>(a0.Ptr)[0], 0xAA) << "in-flight slot 0 untouched";

    ring.BeginFrame(2); // wraps to slot 0
    auto a2 = ring.Allocate(64);
    ASSERT_TRUE(a2.Valid());
    EXPECT_EQ(a2.Buffer, buf0) << "slot 0 reused after a full cycle";
    EXPECT_EQ(a2.Offset, 0u) << "slot rewound";
    EXPECT_EQ(a2.Ptr, a0.Ptr);
}

TEST(RGUploadRing, BeginFrameRewindsUsage)
{
    RG_REQUIRE_DEVICE(dev);
    RGUploadRing ring(dev.get(), 2, 4096);
    ring.BeginFrame(0);
    ring.Allocate(100);
    EXPECT_EQ(ring.BytesUsedThisFrame(), 100u);
    ring.BeginFrame(2);
    EXPECT_EQ(ring.BytesUsedThisFrame(), 0u);
    auto a = ring.Allocate(8);
    EXPECT_EQ(a.Offset, 0u);
}

// Review fix (7): overflow chains a new block — the allocation is served (no
// dropped/corrupt frame) — and total demand converges the main slot in one cycle.
TEST(RGUploadRing, OverflowChainsAndConvergesToHighWater)
{
    RG_REQUIRE_DEVICE(dev);
    RGUploadRing ring(dev.get(), /*framesInFlight=*/2, /*cap=*/256);
    ring.BeginFrame(0);
    auto small = ring.Allocate(64);
    ASSERT_TRUE(small.Valid());
    auto big = ring.Allocate(1024); // exceeds the 256-byte main block
    ASSERT_TRUE(big.Valid()) << "overflow must chain a block, never drop the allocation";
    EXPECT_NE(big.Buffer, small.Buffer) << "served from a chained overflow block";
    EXPECT_EQ(ring.OverflowBlocksThisFrame(), 1u);
    std::memset(big.Ptr, 0xCD, 1024); // really writable

    // Re-review fix (4): convergence is judged by REPLAYING THE SAME PATTERN —
    // the high-water mark must include the alignment padding the workload needs
    // at its converged position (64 aligned up to 256, then 1024 ⇒ 1280), not
    // the padding-less per-block sum (1088), which would overflow forever.
    ring.BeginFrame(1); // other slot grows to the aligned demand
    EXPECT_GE(ring.SlotCapacity(), 1280u);
    EXPECT_TRUE(ring.Allocate(64).Valid());
    EXPECT_TRUE(ring.Allocate(1024, 256).Valid());
    EXPECT_EQ(ring.OverflowBlocksThisFrame(), 0u) << "same workload must fit the main block";

    ring.BeginFrame(2); // slot 0 again: overflow destroyed, main grown, same replay
    EXPECT_GE(ring.SlotCapacity(), 1280u);
    EXPECT_TRUE(ring.Allocate(64).Valid());
    EXPECT_TRUE(ring.Allocate(1024, 256).Valid());
    EXPECT_EQ(ring.OverflowBlocksThisFrame(), 0u) << "no permanent create/destroy churn";
}

// Q6 slice 4: an in-place device rebuild frees every slot buffer and unmaps its
// persistent base pointer while the cached handle still reads IsValid(). Without
// ReprovisionAfterDeviceRebuild the next Allocate hands out an Alloc whose Ptr is a
// dangling map into freed memory (the ScheduleHzbCullPass use-after-free). After it,
// each slot must carry a freshly created buffer and a live, writable map.
TEST(RGUploadRing, ReprovisionRecreatesSlotBuffersAndStaysWritable)
{
    RG_REQUIRE_DEVICE(dev);
    RGUploadRing ring(dev.get(), /*framesInFlight=*/2, /*cap=*/4096);

    ring.BeginFrame(0);
    auto before = ring.Allocate(64);
    ASSERT_TRUE(before.Valid());
    const BufferHandle slot0Before = before.Buffer;

    ring.ReprovisionAfterDeviceRebuild();

    // The forgotten slot buffer is not destroyed (the real teardown already freed
    // it), so the fresh CreateBlock must produce a DIFFERENT handle — proof the ring
    // re-created rather than reused the dead slot.
    ring.BeginFrame(0);
    auto after = ring.Allocate(64);
    ASSERT_TRUE(after.Valid()) << "ring must be usable after reprovision";
    EXPECT_NE(after.Buffer, slot0Before) << "slot buffer must be a freshly created handle";
    EXPECT_NE(after.Ptr, nullptr) << "fresh persistent map, not a dangling pointer";
    std::memset(after.Ptr, 0xEE, 64); // writable through the fresh map — no UAF
    EXPECT_EQ(ring.BytesUsedThisFrame(), 64u);
}

// ── Rotating import ─────────────────────────────────────────────────────────

TEST(RGRotatingTexture, DoubleBufferAlternatesAndNeverWritesInFlightSlot)
{
    RG_REQUIRE_DEVICE(dev);
    RGRotatingTexture hist;
    hist.Init(dev.get(), Tex(1920, 1080), /*count=*/2);
    EXPECT_EQ(hist.Count(), 2u);

    const TextureHandle h0 = hist.Current(0);
    const TextureHandle h1 = hist.Current(1);
    EXPECT_NE(h0, h1);
    EXPECT_EQ(hist.Current(2), h0);
    EXPECT_EQ(hist.Current(3), h1);
    EXPECT_EQ(hist.Previous(1), h0);
    EXPECT_EQ(hist.Previous(2), h1);
    for (uint64_t f = 1; f < 8; ++f)
        EXPECT_NE(hist.Current(f), hist.Previous(f));

    hist.Destroy(dev.get());
}

TEST(RGRotatingTexture, CyclesModuloCount)
{
    RG_REQUIRE_DEVICE(dev);
    RGRotatingTexture ring;
    ring.Init(dev.get(), Tex(512, 512), /*count=*/3);
    const TextureHandle a = ring.Current(0);
    const TextureHandle b = ring.Current(1);
    const TextureHandle c = ring.Current(2);
    EXPECT_NE(a, b);
    EXPECT_NE(b, c);
    EXPECT_NE(a, c);
    EXPECT_EQ(ring.Current(3), a);
    EXPECT_EQ(ring.Current(5), c);
    EXPECT_EQ(ring.Previous(0), c);
    ring.Destroy(dev.get());
}

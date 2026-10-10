#include <gtest/gtest.h>
#include <cstdint>
#include <vector>
#include <cstring>
#include "Rendering/Core/Device.h"
#include "Rendering/Core/BufferRing.h"
#include "Source/Vulkan/VulkanDevice.h"

using namespace GameEngine::Rendering;

static size_t AlignUp(size_t x, size_t a) { return (x + (a - 1)) & ~(a - 1); }

TEST(BufferRing, BasicAllocateAndAlignment)
{
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev && dev->Initialize(dd));

    BufferRing ring;
    ASSERT_TRUE(ring.Initialize(dev.get(), 4096, BufferUsage::Uniform, 256, "BR_Test"));

    auto a0 = ring.Allocate(64); // default align 256
    ASSERT_GT(a0.Size, 0u);
    EXPECT_EQ(a0.Offset, 0u);
    EXPECT_NE(a0.Ptr, nullptr);

    auto a1 = ring.Allocate(32);
    ASSERT_GT(a1.Size, 0u);
    EXPECT_EQ(a1.Offset, AlignUp(a0.Offset + a0.Size, 256));

    // Custom alignment
    auto a2 = ring.Allocate(16, 64);
    ASSERT_GT(a2.Size, 0u);
    EXPECT_EQ(a2.Offset, AlignUp(a1.Offset + a1.Size, 64));

    // Write a bit pattern and verify it sticks in mapped memory
    std::memset(a0.Ptr, 0xAB, a0.Size);
    std::memset(a1.Ptr, 0xCD, a1.Size);
    std::memset(a2.Ptr, 0xEF, a2.Size);

    uint8_t* base = static_cast<uint8_t*>(ring.GetBasePtr());
    for (size_t i=0;i<a0.Size;i++) EXPECT_EQ(base[a0.Offset + i], 0xAB);
    for (size_t i=0;i<a1.Size;i++) EXPECT_EQ(base[a1.Offset + i], 0xCD);
    for (size_t i=0;i<a2.Size;i++) EXPECT_EQ(base[a2.Offset + i], 0xEF);
}

// An allocation that does not fit in the space left THIS frame must be refused.
// Rewinding the head to 0 and re-issuing it does not recycle space: the first
// allocation's bytes are still live — its owner has already handed that
// (buffer, offset) pair to the GPU — so the second caller memcpys over them and
// both read the same region. The allocation reports success either way, which is
// what makes the aliasing silent.
TEST(BufferRing, OverflowRefusesInsteadOfAliasingLiveBytes)
{
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev && dev->Initialize(dd));

    // Sized so that space genuinely remains after the refused request: the head
    // lands at 400, aligns up to 512, and 1024 - 512 leaves room for a small
    // follow-up. (A ring where alignment pushes the head to exactly capacity would
    // refuse that follow-up for an unrelated reason.)
    const size_t kCap = 1024;
    BufferRing ring;
    ASSERT_TRUE(ring.Initialize(dev.get(), kCap, BufferUsage::Uniform, 128, "BR_Overflow"));

    // Consume part of the buffer, then ask for more than the remainder. The request
    // fits the ring on its own, which is exactly the case a rewind satisfies by
    // handing back a region overlapping a0.
    auto a0 = ring.Allocate(400);
    ASSERT_GT(a0.Size, 0u);

    auto a1 = ring.Allocate(800);
    if (a1.Size > 0)
    {
        // Name the consequence, not just the return value: these two ranges
        // overlap, so one of the two owners' GPU reads is another's data.
        const bool overlaps = a1.Offset < a0.Offset + a0.Size &&
                              a0.Offset < a1.Offset + a1.Size;
        EXPECT_FALSE(overlaps) << "the second allocation aliases the first's live bytes";
    }
    EXPECT_EQ(a1.Size, 0u) << "an overflowing allocation must be refused, not wrapped";
    EXPECT_EQ(a1.Ptr, nullptr);

    // The refusal still counts as demand — that is what grows the ring on the next
    // cycle (FrameBufferAllocator::MaybeGrowCurrentSlot reads exactly this).
    EXPECT_EQ(ring.GetRequestedThisFrame(), 1200u);

    // ...and it must not have moved the head, so a request that still fits lands
    // after a0 rather than on top of it.
    auto a2 = ring.Allocate(64);
    ASSERT_GT(a2.Size, 0u);
    EXPECT_GE(a2.Offset, a0.Offset + a0.Size)
        << "a refused allocation rewound the head and then handed out live bytes";
}

// CreateBuffer can succeed while MapBuffer refuses: VulkanDevice::MapBuffer
// returns null for memory that is not host-visible. Initialize must not leave
// that buffer behind — FrameBufferAllocator retries Initialize at a different
// capacity on both its recovery paths (the post-device-rebuild reprovision and
// the failed-growth restore), and a second Initialize overwrites m_Buffer.
namespace
{
class MapRefusingDevice : public VulkanDevice
{
  public:
    BufferHandle CreateBuffer(const BufferDesc& desc) override
    {
        const BufferHandle handle = VulkanDevice::CreateBuffer(desc);
        if (handle.IsValid())
            ++CreatedBuffers;
        return handle;
    }

    void* MapBuffer(BufferHandle handle) override
    {
        return RefuseMaps ? nullptr : VulkanDevice::MapBuffer(handle);
    }

    void DestroyBuffer(BufferHandle handle) override
    {
        ++DestroyedBuffers;
        VulkanDevice::DestroyBuffer(handle);
    }

    bool RefuseMaps = false;
    uint32_t CreatedBuffers = 0;
    uint32_t DestroyedBuffers = 0;
};
} // namespace

TEST(BufferRing, InitializeReleasesTheBufferWhenMappingFails)
{
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan;
    MapRefusingDevice dev;
    ASSERT_TRUE(dev.Initialize(dd));

    dev.RefuseMaps = true;
    const uint32_t createdBefore = dev.CreatedBuffers;
    const uint32_t destroyedBefore = dev.DestroyedBuffers;

    BufferRing ring;
    EXPECT_FALSE(ring.Initialize(&dev, 4096, BufferUsage::Uniform, 256, "BR_MapFail"));

    ASSERT_EQ(dev.CreatedBuffers - createdBefore, 1u)
        << "premise: the buffer was created and only the mapping failed";
    EXPECT_EQ(dev.DestroyedBuffers - destroyedBefore, 1u)
        << "Initialize failed without destroying the buffer it created; the retry "
           "sites in FrameBufferAllocator overwrite the handle and leak it";
    EXPECT_FALSE(ring.GetBuffer().IsValid())
        << "a failed Initialize must leave the ring owning nothing";

    // The ring is reusable: the retry that follows a failure is the whole reason
    // the release above matters.
    dev.RefuseMaps = false;
    EXPECT_TRUE(ring.Initialize(&dev, 4096, BufferUsage::Uniform, 256, "BR_MapFail"));
    ring.Shutdown();
    dev.Shutdown();
}

TEST(BufferRing, ResetForNewFrame)
{
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev && dev->Initialize(dd));

    BufferRing ring; ASSERT_TRUE(ring.Initialize(dev.get(), 1024, BufferUsage::Uniform, 256, "BR_Frame"));

    auto a0 = ring.Allocate(128); ASSERT_GT(a0.Size, 0u);
    EXPECT_TRUE(ring.AnyAllocThisFrame());

    ring.ResetForNewFrame();

    EXPECT_FALSE(ring.AnyAllocThisFrame());
    auto a1 = ring.Allocate(64); ASSERT_GT(a1.Size, 0u);
    EXPECT_EQ(a1.Offset, 0u) << "Ring cursor should reset each frame for transient usage";
}


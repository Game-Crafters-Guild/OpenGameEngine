// A BufferCreateFlags::FrameSlotted buffer is one slot of a per-frame ring the
// owner indexes with IDevice::GetFrameIndex(). Only BeginFrame waits the fence
// that proves the frame which last used that slot has finished reading it, so a
// host write is valid only inside an acquired frame. Developer builds diagnose a
// write outside that window; both halves are pinned here, because a silence test
// on its own passes just as well against a guard that never fires.

#include <gtest/gtest.h>

#include "Rendering/Core/Device.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

using namespace GameEngine::Rendering;

namespace
{
constexpr size_t kProbeBufferBytes = 256;

std::unique_ptr<IDevice> CreateHeadlessDevice()
{
    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::Vulkan;
    desc.enableDynamicRendering = true;
    auto device = DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc))
        return nullptr;
    return device;
}

BufferHandle CreateFrameSlottedProbe(IDevice& device)
{
    BufferDesc desc{};
    desc.size = kProbeBufferBytes;
    desc.usage = static_cast<uint32_t>(BufferUsage::Uniform);
    desc.memoryUsage = BufferMemoryUsage::Upload;
    desc.flags = BufferCreateFlags::PersistentlyMapped | BufferCreateFlags::FrameSlotted;
    desc.debugName = "FrameSlottedWriteProbe";
    return device.CreateBuffer(desc);
}

// Offscreen FinalizeFrame arms the current slot's fence and rotates onto the
// next one, which no BeginFrame has waited since — the state a host write must
// refuse.
void RotateOntoAnUnfencedSlot(IDevice& device)
{
    device.FinalizeFrame();
}
} // namespace

// The control. It only means something next to the death test below: together
// they say the guard fires on an unfenced slot and stops firing once BeginFrame
// has waited it, rather than never firing at all.
TEST(FrameSlottedBufferWrite, WriteInsideAnAcquiredFrameIsSilent)
{
    auto device = CreateHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    const BufferHandle probe = CreateFrameSlottedProbe(*device);
    ASSERT_TRUE(probe.IsValid());

    RotateOntoAnUnfencedSlot(*device);
    ASSERT_TRUE(device->BeginFrame()) << "the write below must happen inside an acquired frame";

    const std::array<uint32_t, 4> payload{1u, 2u, 3u, 4u};
    device->UpdateBuffer(probe, 0, sizeof(payload), payload.data());

    device->DestroyBuffer(probe);
    device->Shutdown();
}

TEST(FrameSlottedBufferWriteDeath, WriteOutsideAnAcquiredFrameAborts)
{
#if !defined(GE_DEV_DIAG)
    GTEST_SKIP() << "The diagnostic is compiled out without GE_DEV_DIAG (Release/MinSizeRel)";
#elif !GTEST_HAS_DEATH_TEST
    GTEST_SKIP() << "Death tests are not supported on this platform";
#else
    auto device = CreateHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    const BufferHandle probe = CreateFrameSlottedProbe(*device);
    ASSERT_TRUE(probe.IsValid());

    RotateOntoAnUnfencedSlot(*device);

    const std::array<uint32_t, 4> payload{1u, 2u, 3u, 4u};
    ASSERT_DEATH({ device->UpdateBuffer(probe, 0, sizeof(payload), payload.data()); },
                 "frame-slotted");

    device->DestroyBuffer(probe);
    device->Shutdown();
#endif
}

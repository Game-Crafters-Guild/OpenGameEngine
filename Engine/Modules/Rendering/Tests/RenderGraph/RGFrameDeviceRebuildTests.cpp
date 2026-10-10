// RGFrame across an in-place device rebuild: BeginFrame's rebuild-generation
// block must forget the per-queue timeline semaphores (the teardown destroyed
// them) so EnsureTimeline recreates them on the rebuilt device. Left stale,
// every post-rebuild SubmissionToken() names a semaphore the device no longer
// knows — QueryGpuSyncToken can never answer Complete, so every readback
// consumer stamped with such a token polls forever (the editor's post-rebuild
// "readback never completed" screenshot wedge).
//
// Device-gated (skips without Vulkan); the loss is injected
// (GE_VK_FORCE_DEVICE_LOST), never a real TDR.

#include "Rendering/Core/RenderGraph/RGFrame.h"

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Tests/ScopedEnvVar.h"
#if defined(RENDERING_HAS_VULKAN)
#include "Vulkan/VulkanDevice.h"
#endif

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <vector>

namespace
{
using namespace GameEngine::Rendering;
using namespace GameEngine::Rendering::RenderGraph;
using GameEngine::Rendering::Tests::ScopedEnvVar;

#if defined(RENDERING_HAS_VULKAN)
// Counts the frame's timeline waits through the real backend so a retirement
// test can tell "waited on the submission" from "skipped a dead handle".
class WaitObservingDevice final : public VulkanDevice
{
  public:
    unsigned Waits = 0;
    bool WaitTimelineSemaphoreValue(SemaphoreHandle semaphore, uint64_t value,
                                    uint64_t timeout = ~0ull) override
    {
        ++Waits;
        return VulkanDevice::WaitTimelineSemaphoreValue(semaphore, value, timeout);
    }
};

unsigned TimelineWaits(IDevice& dev)
{
    return static_cast<WaitObservingDevice&>(dev).Waits;
}
#endif

std::unique_ptr<IDevice> MakeHeadlessDevice()
{
    // Deliberately unscoped: every device in this process wants headless mode.
    GameEngine::Rendering::Tests::SetEnvVar("GE_HEADLESS_TEST", "1");
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
#if defined(RENDERING_HAS_VULKAN)
    std::unique_ptr<IDevice> dev = std::make_unique<WaitObservingDevice>();
#else
    auto dev = DeviceFactory::CreateDevice(dd);
#endif
    if (!dev || !dev->Initialize(dd))
        return nullptr;
    return dev;
}

// Mirror the render loop: TickDeviceRecovery every tick before BeginFrame so
// the injected loss's rebuild retry is driven exactly as EditorApplication does.
void RunOneDeviceFrame(IDevice& dev)
{
    dev.TickDeviceRecovery();
    if (!dev.BeginFrame())
        return;
    auto cl = dev.CreateCommandList(IDevice::QueueType::Graphics);
    if (cl)
    {
        cl->Begin();
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        dev.ExecuteCommandLists(lists);
    }
    dev.Present();
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

// One cleared color pass so Execute() mints a graphics submission (and
// therefore a SubmissionToken).
void RunTrivialFrame(RGFrame& frame)
{
    RGTexture color = frame.CreateTexture("RebuildTest.Color", TinyColorDesc());
    frame.AddPass(
        "RebuildTest.Touch", 0,
        [&](RGPassBuilder& p)
        {
            RGAttachmentOps ops{};
            ops.Load = RGLoadOp::Clear;
            p.AttachColor(0, color, ops);
        },
        [](RGContext&) {});
    frame.MarkOutput(color);
    frame.Execute();
}
} // namespace

TEST(RGFrameDeviceRebuild, SubmissionTokenSignalableAfterRebuild)
{
    // Frame ordinals are device BeginFrame counts (1-based): frame 1 is the
    // pre-loss control; the loss fires on the first graphics submit at or
    // after device frame 3.
    ScopedEnvVar forcedLoss("GE_VK_FORCE_DEVICE_LOST", "3");
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(dev.get());
        RGFrame frame(dev.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

        // Pre-loss control: a live token must be observable as signaled —
        // proves the fixture can distinguish signaled from wedged.
        dev->TickDeviceRecovery();
        ASSERT_TRUE(dev->BeginFrame());
        frame.BeginFrame(0);
        RunTrivialFrame(frame);
        const IDevice::GpuSyncToken preToken = frame.SubmissionToken();
        ASSERT_TRUE(preToken.IsValid()) << "control arm: the trivial frame must submit";
        dev->Present();
        dev->WaitForIdle();
        ASSERT_EQ(dev->QueryGpuSyncToken(preToken), IDevice::GpuSyncStatus::Complete)
            << "control arm: a live token must signal after WaitForIdle";
        ASSERT_EQ(dev->GetDeviceHealth(), DeviceHealth::Healthy)
            << "the injected loss must not fire before the control arm completes";

        // Injected loss -> in-place rebuild -> AwaitingReprovision.
        bool reachedAwaiting = false;
        for (int i = 0; i < 12 && !reachedAwaiting; ++i)
        {
            RunOneDeviceFrame(*dev);
            reachedAwaiting = (dev->GetDeviceHealth() == DeviceHealth::AwaitingReprovision);
        }
        ASSERT_TRUE(reachedAwaiting) << "injected loss should rebuild to AwaitingReprovision";
        // Retirement can precede the first BeginFrame on the rebuilt device.
        // Old timeline handles are dead; discard must not wait through them.
#if defined(RENDERING_HAS_VULKAN)
        const unsigned waitsBeforeDiscard = TimelineWaits(*dev);
#endif
        frame.DiscardRecordedPasses();
#if defined(RENDERING_HAS_VULKAN)
        EXPECT_EQ(TimelineWaits(*dev), waitsBeforeDiscard)
            << "retirement after a device rebuild must not wait on timelines the old device destroyed";
#endif
        EXPECT_EQ(frame.Graph().PassCount(), 0);
        frame.Execute();
        dev->NotifyReprovisionComplete();
        ASSERT_EQ(dev->GetDeviceHealth(), DeviceHealth::Healthy);

        // Post-rebuild: the SAME RGFrame must mint a token the REBUILT device
        // can observe. Stale queue timelines make this token permanently
        // unsignalable (the semaphore registry no longer knows the handle).
        dev->TickDeviceRecovery();
        ASSERT_TRUE(dev->BeginFrame());
        frame.BeginFrame(1);
        RunTrivialFrame(frame);
        const IDevice::GpuSyncToken postToken = frame.SubmissionToken();
        ASSERT_TRUE(postToken.IsValid()) << "post-rebuild frame must submit";
        dev->Present();
        dev->WaitForIdle();
        EXPECT_EQ(dev->QueryGpuSyncToken(postToken), IDevice::GpuSyncStatus::Complete)
            << "post-rebuild SubmissionToken must be observable by the rebuilt device; "
               "a stale RGFrame queue timeline wedges every readback consumer forever";
    }
    dev->Shutdown();
}

// Declared after SubmissionTokenSignalableAfterRebuild, and gtest runs a suite's
// tests in declaration order: the injection above must not outlive its test on
// ANY exit path, or every later device-creating test in this process silently
// runs under injected device loss.
TEST(RGFrameDeviceRebuild, ForcedDeviceLossDoesNotLeakToLaterTests)
{
    EXPECT_EQ(std::getenv("GE_VK_FORCE_DEVICE_LOST"), nullptr)
        << "GE_VK_FORCE_DEVICE_LOST outlived its test — later device-creating tests in this "
           "process would run under an injected loss they never asked for";
}

#if defined(RENDERING_HAS_VULKAN)
TEST(RGFrameDeviceRebuild, EmptyRecordingRetirementStillWaitsForPreviousSubmission)
{
    auto device = MakeHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(device.get());
        RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        RunTrivialFrame(frame);
        ASSERT_TRUE(frame.SubmissionToken().IsValid());
        frame.BeginFrame(1);
        ASSERT_EQ(frame.Graph().PassCount(), 0);
        const unsigned waits = TimelineWaits(*device);
        frame.DiscardRecordedPasses();
        EXPECT_GT(TimelineWaits(*device), waits)
            << "clearing declarations does not retire the previous GPU submission";
        EXPECT_EQ(device->QueryGpuSyncToken(frame.SubmissionToken()), IDevice::GpuSyncStatus::Complete);
    }
    device->Shutdown();
}
#endif

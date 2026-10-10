// Staging-ring behaviour for VideoFrameUploader, independent of decoding.
//
// The uploader owns a persistently-mapped ring shared by every video entity in a
// tick. Two properties matter and neither is observable from the system-level
// tests, which only count submissions:
//   - Two entities staging in one tick must get DISJOINT regions. A ring sized for
//     one frame that hands the second entity the first one's offset produces a
//     perfectly healthy-looking upload of the wrong video.
//   - A slot must not be rewritten while its previous copy is in flight, and
//     "no completion token exists" is not evidence that it is not.

#include <gtest/gtest.h>

#include "Engine/Video/VideoFrameUploader.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <vector>

#include "TestDeviceHelper.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{
// Small enough to keep the test cheap, large enough that one frame fills the
// ring's minimum slot (kMinStagingSlotBytes is 1 MiB) so a second frame in the
// same tick genuinely does not fit.
constexpr uint32_t kFrameWidth = 512;
constexpr uint32_t kFrameHeight = 512;
constexpr size_t kBytesPerPixel = 4;
constexpr size_t kRowPitch = static_cast<size_t>(kFrameWidth) * kBytesPerPixel;

void SetEnvVar(const char* key, const char* value)
{
#if defined(_WIN32)
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}
void UnsetEnvVar(const char* key)
{
#if defined(_WIN32)
    _putenv_s(key, "");
#else
    unsetenv(key);
#endif
}

// A trivial device frame, matching RenderServicesDeviceRecoveryTests. The empty
// graphics submit is load-bearing: the injected loss fires ON a graphics submit,
// so a frame without one would never trip it and this test would skip silently.
// TickDeviceRecovery drives the rebuild retry, mirroring the render loop.
void RunDeviceFrame(IDevice& dev)
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

TextureHandle CreateVideoTexture(IDevice& dev)
{
    TextureDesc desc{};
    desc.width = kFrameWidth;
    desc.height = kFrameHeight;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.format = static_cast<uint32_t>(TextureFormat::RGBA8_SRGB);
    desc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
    desc.sampleCount = 1;
    desc.persistent = true;
    desc.initialState = ResourceState::ShaderResource;
    desc.debugName = "VideoFrameUploaderTestTexture";
    return dev.CreateTexture(desc);
}

std::vector<uint8_t> MakeFramePixels()
{
    return std::vector<uint8_t>(kRowPitch * kFrameHeight, 0x7Fu);
}
} // namespace

class VideoFrameUploaderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
        m_Pixels = MakeFramePixels();
        m_Uploader = std::make_unique<Video::VideoFrameUploader>(m_Device.get());
    }

    void TearDown() override
    {
        m_Uploader.reset();
        if (m_Device)
        {
            m_Device->WaitForIdle();
            m_Device->Shutdown();
        }
    }

    TextureHandle MakeVideoTexture() { return CreateVideoTexture(*m_Device); }

    bool Stage(TextureHandle texture)
    {
        return m_Uploader->Stage(texture, m_Pixels.data(), kFrameWidth, kFrameHeight, kRowPitch);
    }

    std::unique_ptr<IDevice> m_Device;
    std::unique_ptr<Video::VideoFrameUploader> m_Uploader;
    std::vector<uint8_t> m_Pixels;
};

// B1. The ring is sized from the first frame it ever sees. A second entity staging
// in the same tick does not fit — and must be REFUSED. The failure this pins is not
// a dropped frame but a silent one: the allocator used to rewind its head and hand
// back the first entity's offset, so entity B's memcpy landed on entity A's staged
// bytes and the command list copied the same bytes into both textures. Entity A
// showed entity B's video, with both Stage() calls reporting success.
TEST_F(VideoFrameUploaderTest, SecondEntityInOneTickIsRefusedRatherThanAliasingTheFirst)
{
    const TextureHandle texA = MakeVideoTexture();
    const TextureHandle texB = MakeVideoTexture();
    ASSERT_TRUE(texA.IsValid());
    ASSERT_TRUE(texB.IsValid());

    m_Uploader->BeginFrame();
    ASSERT_TRUE(Stage(texA)) << "the first frame of a tick must always stage";
    EXPECT_FALSE(Stage(texB))
        << "the second entity was handed staging the first is still using — both "
           "textures receive the same bytes";
}

// ...and the refusal is temporary. The refused request still records its demand, so
// the slot grows on a later cycle and both entities stage from then on. Without
// this, "refuse on overflow" would be a permanent one-video-only limit.
TEST_F(VideoFrameUploaderTest, RefusedSlotGrowsSoBothEntitiesStageLater)
{
    const TextureHandle texA = MakeVideoTexture();
    const TextureHandle texB = MakeVideoTexture();
    ASSERT_TRUE(texA.IsValid());
    ASSERT_TRUE(texB.IsValid());

    // Bounded: every slot records the two-frame demand on its own cycle and grows
    // on its next one, so this converges within a couple of rotations however the
    // device reports its frame index.
    constexpr int kMaxCycles = 16;
    bool bothStaged = false;
    for (int cycle = 0; cycle < kMaxCycles && !bothStaged; ++cycle)
    {
        m_Uploader->BeginFrame();
        const bool a = Stage(texA);
        const bool b = Stage(texB);
        bothStaged = a && b;
        m_Uploader->Submit();
        // Retire the submit so the next cycle's slot gate is deterministic.
        m_Device->WaitForIdle();
    }
    EXPECT_TRUE(bothStaged)
        << "the staging slot never grew to hold both entities' frames; refusing on "
           "overflow became a permanent limit instead of a one-cycle cost";
}

// B2. The slot-reuse gate reads a completion token, and an absent token is not
// evidence of completion. Only VulkanDevice overrides LastGraphicsSubmissionToken();
// D3D12Device and MetalDevice inherit IDevice's `return {}`, so on those backends no
// slot ever holds one.
//
// Both arms of the rule are pinned here, because a two-valued answer cannot satisfy
// them at once: an absent token must never read as Complete, AND a slot that has
// genuinely never been written must stay usable. That is why the device query is
// three-valued — Unknown is a distinct answer the gate resolves with ring-rotation
// evidence rather than with the timeline.
TEST_F(VideoFrameUploaderTest, AbsentCompletionTokenIsNotEvidenceOfCompletion)
{
    // Arm 1: the device refuses to call an absent token complete.
    const IDevice::GpuSyncToken absent{};
    ASSERT_FALSE(absent.IsValid());
    EXPECT_EQ(m_Device->QueryGpuSyncToken(absent), IDevice::GpuSyncStatus::Unknown)
        << "an absent token must not read as completion — it is absence of "
           "information, and cannot be the sole evidence that a slot is free";

    // Positive companion: the same device answers Complete for a token it can
    // observe, so the Unknown above is a verdict rather than a dead instrument.
    m_Device->WaitForIdle();
    const IDevice::GpuSyncToken live = m_Device->LastGraphicsSubmissionToken();
    if (live.IsValid())
    {
        EXPECT_EQ(m_Device->QueryGpuSyncToken(live), IDevice::GpuSyncStatus::Complete)
            << "a retired graphics token must read Complete";
    }

    // Arm 2: a slot no submit has ever touched is genuinely free, and must stay
    // usable — the Unknown answer resolves through ring rotation, not the timeline.
    const TextureHandle tex = MakeVideoTexture();
    ASSERT_TRUE(tex.IsValid());
    m_Uploader->BeginFrame();
    EXPECT_TRUE(Stage(tex))
        << "a never-written slot was refused; the reuse gate cannot tell 'never "
           "armed' from 'still in flight'";
}

// SF3. An in-place device rebuild frees the ring's buffers and unmaps its base
// pointers while the allocator still reports them valid. Staging after a rebuild
// without re-provisioning writes through a raw pointer into freed VMA memory — a
// use-after-free that handle generations do not catch, because the wholesale VMA
// teardown does not bump them.
// Not a fixture test: the fault injection is read when the device is CREATED, so
// the variable has to be set before CreateVulkanDeviceFast, which a fixture's
// SetUp has already run by the time a test body executes. Getting this wrong is
// invisible — the loss simply never fires and the test skips itself.
TEST(VideoFrameUploaderDeviceRebuild, StagingAfterDeviceRebuildWritesLiveMemory)
{
    // Frame ordinals are device BeginFrame counts; the loss fires on the first
    // graphics submit at or after device frame 2.
    SetEnvVar("GE_VK_FORCE_DEVICE_LOST", "2");
    struct EnvGuard
    {
        ~EnvGuard() { UnsetEnvVar("GE_VK_FORCE_DEVICE_LOST"); }
    } guard;

    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    const std::vector<uint8_t> pixels = MakeFramePixels();
    auto uploader = std::make_unique<Video::VideoFrameUploader>(device.get());
    auto stage = [&](TextureHandle tex) {
        return uploader->Stage(tex, pixels.data(), kFrameWidth, kFrameHeight, kRowPitch);
    };

    TextureHandle tex = CreateVideoTexture(*device);
    ASSERT_TRUE(tex.IsValid());

    // Establish the ring on the pre-loss device, so the rebuild has live buffers
    // and live mapped pointers to invalidate rather than nothing at all.
    uploader->BeginFrame();
    ASSERT_TRUE(stage(tex)) << "control arm: staging must work before the loss";
    uploader->Submit();

    bool reachedAwaiting = false;
    for (int i = 0; i < 16 && !reachedAwaiting; ++i)
    {
        RunDeviceFrame(*device);
        reachedAwaiting = (device->GetDeviceHealth() == DeviceHealth::AwaitingReprovision);
    }
    // Asserted, never skipped: a rebuild that does not happen leaves this test
    // measuring nothing while reporting success.
    ASSERT_TRUE(reachedAwaiting) << "the injected device loss never rebuilt";

    // What the owning system does when it sees the rebuild generation change.
    uploader->ReprovisionAfterDeviceRebuild();
    device->NotifyReprovisionComplete();
    ASSERT_EQ(device->GetDeviceHealth(), DeviceHealth::Healthy);

    // The pre-rebuild texture died with the device; mint one on the rebuilt device.
    tex = CreateVideoTexture(*device);
    ASSERT_TRUE(tex.IsValid());

    // The write below goes through the ring's mapped base pointer. Un-reprovisioned
    // that pointer names freed VMA memory, which no handle generation guards.
    uploader->BeginFrame();
    EXPECT_TRUE(stage(tex))
        << "staging refused after the rebuild: the ring was left dead instead of "
           "being re-created on the rebuilt device";
    uploader->Submit();
    device->WaitForIdle();

    uploader.reset();
    device->Shutdown();
}

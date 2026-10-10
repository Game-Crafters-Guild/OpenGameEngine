// A device rebuild destroys every live texture and frees its generational slot,
// but TextureHandle::IsValid() is `id != 0` and keeps answering true. Any editor
// cache that reuses such a handle imports a texture that resolves to nothing:
// the descriptor write is skipped, the descriptor keeps its previous contents,
// and the GPU samples freed memory — a real device fault with no validation
// message, because VK_EXT_descriptor_buffer disables shader instrumentation.
//
// ViewSnapshotTexture is the editor's device-scoped texture cache; every editor
// snapshot the popup or the pane samples on a LATER frame is stored in one,
// wrapped by ViewPresentationSnapshot (the scene view's last presentation and
// the camera-bookmark popup's frozen preview). These cases pin that the storage
// reuses within a device generation and creates a fresh texture across a
// rebuild, and that the wrapper's read — the only read its consumers make —
// carries the same generation check after a real write.
//
// Device cases skip when no Vulkan device is available (headless CI without a GPU).

#include <gtest/gtest.h>

#include "SceneView/ViewPresentationSnapshot.h"
#include "SceneView/ViewSnapshotTexture.h"

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include "TestDeviceHelper.h"

#include <cstdlib>
#include <vector>

using namespace GameEngine;
using GameEngine::Editor::ViewPresentationSnapshot;
using GameEngine::Editor::ViewSnapshotTexture;

namespace
{

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

// Mirrors the render loop closely enough to drive recovery: TickDeviceRecovery
// is the unconditional per-tick poll the rebuild retry hangs off.
void RunDeviceFrame(Rendering::IDevice& dev)
{
    dev.TickDeviceRecovery();
    if (!dev.BeginFrame())
        return;
    auto cl = dev.CreateCommandList(Rendering::IDevice::QueueType::Graphics);
    if (cl)
    {
        cl->Begin();
        cl->End();
        std::vector<Rendering::CommandList*> lists{cl.get()};
        dev.ExecuteCommandLists(lists);
    }
    dev.Present();
}

// Runs frames until the armed fault injection has rebuilt the device. False
// when this adapter never got there — the caller skips rather than assert.
bool ForceDeviceRebuild(Rendering::IDevice& dev)
{
    const uint64_t genBefore = dev.GetDeviceRebuildGeneration();
    for (int i = 0; i < 16; ++i)
    {
        RunDeviceFrame(dev);
        if (dev.GetDeviceRebuildGeneration() != genBefore)
            return true;
    }
    return false;
}

// TextureDesc::format is the engine TextureFormat enum widened to uint32_t
// (the render graph's resource desc carries it the same way).
constexpr uint32_t kRgba8 = static_cast<uint32_t>(Rendering::TextureFormat::RGBA8_UNORM);
constexpr uint32_t kWidth = 64u;
constexpr uint32_t kHeight = 32u;

} // namespace

TEST(ViewSnapshotTexture, ReusesTheSameTextureWithinADeviceGeneration)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    ViewSnapshotTexture cache;
    const auto first = cache.Ensure(*device, kWidth, kHeight, kRgba8, "Test.Snapshot");
    ASSERT_TRUE(first.IsValid());
    EXPECT_EQ(cache.Ensure(*device, kWidth, kHeight, kRgba8, "Test.Snapshot"), first)
        << "an unchanged shape on an unchanged device must not churn the texture";
    EXPECT_EQ(cache.Width(), kWidth);
    EXPECT_EQ(cache.Height(), kHeight);

    // A shape change still recreates.
    const auto resized = cache.Ensure(*device, kWidth * 2, kHeight, kRgba8, "Test.Snapshot");
    ASSERT_TRUE(resized.IsValid());
    EXPECT_NE(resized, first);

    cache.Destroy(device.get());
    EXPECT_FALSE(cache.Texture(*device).IsValid());
    device->Shutdown();
}

TEST(ViewSnapshotTexture, DeviceRebuildForcesANewTexture)
{
    SetEnvVar("GE_VK_FORCE_DEVICE_LOST", "2");
    auto device = CreateVulkanDeviceFast();
    if (!device)
    {
        UnsetEnvVar("GE_VK_FORCE_DEVICE_LOST");
        GTEST_SKIP() << "No Vulkan device available";
    }

    ViewSnapshotTexture cache;
    const auto beforeLoss = cache.Ensure(*device, kWidth, kHeight, kRgba8, "Test.Snapshot");
    ASSERT_TRUE(beforeLoss.IsValid());

    if (!ForceDeviceRebuild(*device))
    {
        UnsetEnvVar("GE_VK_FORCE_DEVICE_LOST");
        device->Shutdown();
        GTEST_SKIP() << "injected loss did not rebuild the device on this adapter";
    }

    // THE READ, and deliberately before any Ensure — the read is how this
    // reaches the GPU. The editor's waiting-for-extraction arm imports the
    // cached handle without ever calling Ensure (DeclareTargetsRG early-returns
    // ahead of the snapshot update on any frame whose extraction is not
    // current), and RGFrame::ImportExternalTexture records whatever id it is
    // handed as the physical, with no liveness check. A guard that only ran on
    // the write path would satisfy every assertion below and still publish a
    // freed id here.
    EXPECT_FALSE(cache.Texture(*device).IsValid())
        << "a read after a device rebuild handed back an id the rebuild freed";

    // The rebuild destroyed the image behind `beforeLoss` and freed its slot.
    // Handing that id back would import a texture that no longer exists.
    const auto afterRebuild = cache.Ensure(*device, kWidth, kHeight, kRgba8, "Test.Snapshot");
    ASSERT_TRUE(afterRebuild.IsValid());
    EXPECT_NE(afterRebuild, beforeLoss)
        << "the cache handed back a handle the device rebuild destroyed";
    EXPECT_EQ(cache.Texture(*device), afterRebuild);

    // Destroy must be safe here: it may only destroy the live handle, never the
    // one the rebuild already freed.
    cache.Destroy(device.get());
    EXPECT_FALSE(cache.Texture(*device).IsValid());

    device->Shutdown();
    UnsetEnvVar("GE_VK_FORCE_DEVICE_LOST");
}

// The wrapper every consumer actually reads through. Both users retain the
// frozen copy well past the frame that wrote it — the pane's
// waiting-for-extraction arm re-imports it, and the camera-bookmark popup binds
// it into a UI texture slot hover cycles later — and neither reaches Update on
// the frame it reads. So the wrapper's read is the only thing between a freed
// id and a live descriptor, and it must carry the generation check even after a
// real write went through Update.
TEST(ViewPresentationSnapshot, DeviceRebuildInvalidatesTheFrozenCopy)
{
    SetEnvVar("GE_VK_FORCE_DEVICE_LOST", "2");
    auto device = CreateVulkanDeviceFast();
    if (!device)
    {
        UnsetEnvVar("GE_VK_FORCE_DEVICE_LOST");
        GTEST_SKIP() << "No Vulkan device available";
    }

    // Stands in for a pipeline output: Update takes the snapshot's shape from
    // the source's graph desc, which an import fills from the device.
    Rendering::TextureDesc sourceDesc{};
    sourceDesc.width = kWidth;
    sourceDesc.height = kHeight;
    sourceDesc.depth = 1;
    sourceDesc.mipLevels = 1;
    sourceDesc.arrayLayers = 1;
    sourceDesc.sampleCount = 1;
    sourceDesc.format = kRgba8;
    sourceDesc.usage = static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource) |
                       static_cast<uint32_t>(Rendering::TextureUsage::TransferSrc);
    sourceDesc.debugName = "Test.PresentationSnapshot.Source";
    const auto sourceTexture = device->CreateTexture(sourceDesc);
    ASSERT_TRUE(sourceTexture.IsValid());

    ViewPresentationSnapshot snapshot;
    {
        Rendering::RenderGraph::RGResourcePool persistent(device.get());
        Rendering::RenderGraph::RGTransientPool transient(device.get());
        Rendering::RenderGraph::RGUploadRing ring(device.get(), 2, 4096);
        Rendering::RenderGraph::RGFrame frame(device.get(), &persistent, &transient, &ring);
        frame.BeginFrame(0);
        const auto source = frame.ImportExternalTexture(
            "Test.PresentationSnapshot.Source", sourceTexture,
            Rendering::ResourceState::ShaderResource, Rendering::TextureFormat::RGBA8_UNORM);
        ASSERT_TRUE(source.IsValid());
        ASSERT_TRUE(snapshot.Update(frame, *device, source,
                                    UI::UITextureSpace::DisplayLinearSdr(), "Test.Snapshot"))
            << "the write path must produce a frozen copy before the rebuild case means anything";
    }

    const auto beforeLoss = snapshot.Texture(*device);
    ASSERT_TRUE(beforeLoss.IsValid());
    EXPECT_EQ(snapshot.Width(), kWidth);
    EXPECT_EQ(snapshot.Height(), kHeight);

    if (!ForceDeviceRebuild(*device))
    {
        snapshot.Destroy(device.get());
        UnsetEnvVar("GE_VK_FORCE_DEVICE_LOST");
        device->Shutdown();
        GTEST_SKIP() << "injected loss did not rebuild the device on this adapter";
    }

    // THE READ, and deliberately with no Update in between — that is the shape
    // every consumer has.
    EXPECT_FALSE(snapshot.Texture(*device).IsValid())
        << "a read after a device rebuild handed back an id the rebuild freed";

    // Destroy may only free the live handle, never the slot the rebuild freed.
    snapshot.Destroy(device.get());
    EXPECT_FALSE(snapshot.Texture(*device).IsValid());

    device->Shutdown();
    UnsetEnvVar("GE_VK_FORCE_DEVICE_LOST");
}

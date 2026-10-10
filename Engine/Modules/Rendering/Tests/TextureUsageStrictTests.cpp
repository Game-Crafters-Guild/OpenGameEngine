#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "../Source/Vulkan/TextureUsagePolicy.h"

using namespace GameEngine::Rendering;

namespace
{

// Both flags resolve once per process, so they must be set before anything reads
// them. This test owns its executable for that reason: the audit suite needs the
// permissive backend to demonstrate an undeclared copy still succeeding, and this
// one needs the strict backend.
void EnableStrict()
{
#ifdef _WIN32
    _putenv_s("GE_STRICT_TEXTURE_USAGE", "1");
    _putenv_s("GE_TEXTURE_USAGE_AUDIT", "1");
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_STRICT_TEXTURE_USAGE", "1", 1);
    setenv("GE_TEXTURE_USAGE_AUDIT", "1", 1);
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif
}

} // namespace

// Strict mode's contract is narrow: an image gets exactly the usage it declared.
// The thing that must not regress is that a TRUE declaration is sufficient — the
// image still creates, the transfer commands still run, and the pixels still land.
// (A false declaration under strict mode is a Vulkan validation error by
// construction; provoking one here would be testing the validation layer.)
TEST(TextureUsageStrict, DeclaredTransferUsageIsSufficientWithoutTheBlanketOr)
{
    EnableStrict();
    ASSERT_TRUE(TextureUsagePolicy::IsStrict()) << "strict flag resolved before the test set it";

    DeviceDesc deviceDesc{};
    deviceDesc.applicationName = "TextureUsageStrictTests";
    deviceDesc.preferredAPI = GraphicsAPI::Vulkan;
    deviceDesc.enableDynamicRendering = true;
    auto device = DeviceFactory::CreateDevice(deviceDesc);
    if (!device || !device->Initialize(deviceDesc))
    {
        GTEST_SKIP() << "Device init failed";
    }

    constexpr uint32_t kWidth = 64;
    constexpr uint32_t kHeight = 64;
    constexpr size_t kReadbackBytes = static_cast<size_t>(kWidth) * kHeight * 4u;

    TextureDesc desc{};
    desc.width = kWidth;
    desc.height = kHeight;
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    desc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget) |
                 static_cast<uint32_t>(TextureUsage::TransferSrc) |
                 static_cast<uint32_t>(TextureUsage::TransferDst);
    desc.persistent = false;
    desc.debugName = "StrictConformingTex";

    const TextureHandle texture = device->CreateTexture(desc);
    ASSERT_NE(texture, INVALID_HANDLE) << "strict mode must still create a correctly declared image";

    const BufferHandle readback = device->CreateReadbackBuffer(kReadbackBytes);
    ASSERT_NE(readback, INVALID_HANDLE);

    auto commandList = device->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(commandList);
    commandList->Begin();
    const float red[4] = {1.f, 0.f, 0.f, 1.f};
    commandList->ClearColorImageSubresource(texture, 0, 0, red);
    commandList->CopyTextureSubresourceToBuffer(texture, 0, 0, readback, kWidth, kHeight, 0, 0, 0, 0);
    commandList->End();

    device->ExecuteCommandLists({commandList.get()});
    device->FinalizeFrame();
    device->WaitForIdle();

    const TextureUsageSnapshot snapshot = TextureUsagePolicy::Snapshot();
    std::cout << "[policy] strict observed=" << snapshot.ObservedUses
              << " violations=" << snapshot.ViolationKeys.size() << "\n";
    // The hook ran, so a clean snapshot means clean rather than dead.
    EXPECT_GE(snapshot.ObservedUses, 2u);
    EXPECT_TRUE(snapshot.ViolationKeys.empty()) << "a true declaration must not be reported";

    const uint8_t* pixels = static_cast<const uint8_t*>(device->MapBuffer(readback));
    ASSERT_NE(pixels, nullptr);
    EXPECT_GE(pixels[0], 200);
    EXPECT_LE(pixels[1], 80);
    EXPECT_LE(pixels[2], 80);
    EXPECT_GE(pixels[3], 200);
    device->UnmapBuffer(readback);

    device->DestroyBuffer(readback);
    device->DestroyTexture(texture);
}

// The pipeline-output shape under strict mode: a pool import whose importer
// declares RenderTarget|ShaderResource, required as TransferSrc a frame ahead
// by the output policy, then captured. Strict maps exactly the declaration, so
// the capture is valid only if the frame really rematerialized the import with
// the requirement — the audit (which reads the created usage) and the pixels
// both say so. The first frame's physical, created before anything could
// require it, is never copied.
TEST(TextureUsageStrict, PoolImportRequiredAheadIsSufficientForItsCapture)
{
    EnableStrict();
    ASSERT_TRUE(TextureUsagePolicy::IsStrict()) << "strict flag resolved before the test set it";

    DeviceDesc deviceDesc{};
    deviceDesc.applicationName = "TextureUsageStrictTests";
    deviceDesc.preferredAPI = GraphicsAPI::Vulkan;
    deviceDesc.enableDynamicRendering = true;
    deviceDesc.enableSwapchain = false;
    auto device = DeviceFactory::CreateDevice(deviceDesc);
    if (!device || !device->Initialize(deviceDesc))
    {
        GTEST_SKIP() << "Device init failed";
    }

    constexpr uint32_t kWidth = 64;
    constexpr uint32_t kHeight = 64;
    constexpr size_t kRowPitch = static_cast<size_t>(kWidth) * 4u;
    constexpr size_t kReadbackBytes = kRowPitch * kHeight;
    constexpr const char* kPoolName = "Strict.Output";

    RenderGraph::RGResourcePool persistent(device.get());
    RenderGraph::RGTransientPool transient(device.get());
    RenderGraph::RGUploadRing ring(device.get(), 2, 4096);
    RenderGraph::RGFrame frame(device.get(), &persistent, &transient, &ring);

    const BufferHandle readback = device->CreateReadbackBuffer(kReadbackBytes);
    ASSERT_NE(readback, INVALID_HANDLE);

    auto renderFrame = [&](uint64_t frameIndex, bool capture)
    {
        TextureDesc desc{};
        desc.width = kWidth;
        desc.height = kHeight;
        desc.mipLevels = 1;
        desc.arrayLayers = 1;
        desc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
        desc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
        desc.debugName = kPoolName;

        frame.BeginFrame(frameIndex);
        const RenderGraph::RGTexture output = frame.ImportPersistentTexture(kPoolName, desc);
        frame.AddPass(
            "Fill", 0,
            [&](RenderGraph::RGPassBuilder& p)
            {
                RenderGraph::RGAttachmentOps ops{};
                ops.Load = RenderGraph::RGLoadOp::Clear;
                ops.Store = RenderGraph::RGStoreOp::Store;
                ops.Clear.Color[0] = 1.0f;
                ops.Clear.Color[3] = 1.0f;
                p.AttachColor(0, output, ops);
            },
            [](RenderGraph::RGContext&) {});
        frame.MarkOutput(output, RenderGraph::RGImageLayout::ShaderReadOnly);
        frame.RequireTransferUsage(output, TextureUsage::TransferSrc);
        if (capture)
        {
            frame.AddPass(
                "Capture", 1,
                [&](RenderGraph::RGPassBuilder& p)
                {
                    p.Read(output, RenderGraph::RGTextureRead::CopySrc);
                    p.PreventCulling();
                },
                [output, readback](RenderGraph::RGContext& ctx)
                {
                    ctx.Cmd->CopyTextureToBuffer(ctx.GetTexture(output), readback, kWidth, kHeight, 0, 0,
                                                 0, kRowPitch);
                });
        }
        const TextureHandle physical = frame.PhysicalTexture(output);
        frame.Execute();
        return physical;
    };

    const TextureHandle firstPhysical = renderFrame(0, /*capture*/ false);
    const TextureHandle capturedPhysical = renderFrame(1, /*capture*/ true);
    frame.WaitForPendingWork();
    device->WaitForIdle();
    EXPECT_NE(capturedPhysical, firstPhysical)
        << "the requirement must have rematerialized the import before the capture";

    const TextureUsageSnapshot snapshot = TextureUsagePolicy::Snapshot();
    std::cout << "[policy] strict observed=" << snapshot.ObservedUses
              << " violations=" << snapshot.ViolationKeys.size() << "\n";
    EXPECT_GE(snapshot.ObservedUses, 1u) << "the capture never reached the audit hook";
    EXPECT_FALSE(std::any_of(snapshot.ViolationKeys.begin(), snapshot.ViolationKeys.end(),
                             [&](const std::string& key) { return key.rfind(kPoolName, 0) == 0; }))
        << "the captured physical does not carry the required TransferSrc";

    const uint8_t* pixels = static_cast<const uint8_t*>(device->MapBuffer(readback));
    ASSERT_NE(pixels, nullptr);
    EXPECT_GE(pixels[0], 200);
    EXPECT_LE(pixels[1], 80);
    EXPECT_LE(pixels[2], 80);
    EXPECT_GE(pixels[3], 200);
    device->UnmapBuffer(readback);

    device->DestroyBuffer(readback);
}

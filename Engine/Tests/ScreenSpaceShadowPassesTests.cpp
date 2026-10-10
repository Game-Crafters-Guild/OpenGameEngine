#include <gtest/gtest.h>
#include "Components/Rendering/PostProcessEffects/ShadowSettingsEffect.h"
#include "ECS/World.h"
#include "ECS/ECSTemplates.h"
#include "Engine/Rendering/PostProcessEffectRegistry.h"
#include "Engine/Rendering/PostProcessVolumeExtract.h"
#include "Engine/Rendering/ScreenSpaceShadows/ScreenSpaceShadowPasses.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "TestDeviceHelper.h"
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
namespace RG = GameEngine::Rendering::RenderGraph;
namespace
{
struct Pools
{
    RG::RGResourcePool Persistent;
    RG::RGTransientPool Transient;
    RG::RGUploadRing Upload;
    explicit Pools(IDevice* device) : Persistent(device), Transient(device), Upload(device, 2, 262144) {}
};
CameraData PerspectiveCamera()
{
    CameraData camera{};
    for (int i = 0; i < 16; i += 5) camera.view[i] = 1;
    const auto projection = Mathematics::MakePerspectiveLH_ZO_ReverseZ(1.0471976f, 1.5f, 0.1f, 200.0f);
    std::memcpy(camera.proj, projection.Data(), sizeof(camera.proj));
    std::memcpy(camera.viewProj, camera.proj, sizeof(camera.proj));
    std::memcpy(camera.viewProjRel, camera.viewProj, sizeof(camera.viewProj));
    return camera;
}
TextureDesc DepthDesc(uint32_t width, uint32_t height, uint32_t samples = 1)
{
    TextureDesc desc{};
    desc.width = width; desc.height = height;
    desc.depth = desc.mipLevels = desc.arrayLayers = 1;
    desc.sampleCount = samples;
    desc.format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
    desc.usage = static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource);
    return desc;
}
}

TEST(ScreenSpaceShadowSettings, DefaultsOffAndExtractionClampsAuthoredValues)
{
    Components::ShadowSettingsEffect effect{};
    EXPECT_FALSE(effect.ScreenSpaceShadows);
    EXPECT_FALSE(ResolvedShadowSettings{}.ScreenSpaceShadows);
    const auto* descriptor = PostProcessEffectRegistry::Find(ECS::GetComponentTypeId<Components::ShadowSettingsEffect>());
    ASSERT_NE(descriptor, nullptr);
    ASSERT_NE(descriptor->Extract, nullptr);
    ECS::World world;
    auto entity = world.Create();
    effect.ScreenSpaceShadows = true;
    effect.ScreenSpaceShadowThickness = -1;
    entity.Set(effect); world.ProcessCommands();
    PostProcessExtractedVolume output{};
    descriptor->Extract(world, entity.GetHandle(), {}, output);
    EXPECT_TRUE(output.ShadowSettings.ScreenSpaceShadows);
    EXPECT_FLOAT_EQ(output.ShadowSettings.ScreenSpaceShadowThickness, 0.0001f);
    effect.Enabled = false; entity.Set(effect); world.ProcessCommands();
    output = {};
    descriptor->Extract(world, entity.GetHandle(), {}, output);
    EXPECT_FALSE(output.HasShadowSettings);
    EXPECT_FALSE(output.ShadowSettings.ScreenSpaceShadows);
}

TEST(ScreenSpaceShadowPasses, InvalidInputsDeclareNoPassOrTexture)
{
    auto device = CreateVulkanDeviceFast();
    if (!device) GTEST_SKIP() << "No Vulkan device";
    Pools pools(device.get());
    RG::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Upload);
    ScreenSpaceShadowPasses service(device.get());
    frame.BeginFrame(0);
    auto depth = frame.ImportPersistentTexture("SSS.Depth", DepthDesc(17, 13));
    auto camera = PerspectiveCamera();
    const float light[3] = {0.3f, 0.1f, -1.0f};
    EXPECT_FALSE(service.DeclareMaskPass(frame, 1, {}, camera, light, 0.005f).IsValid());
    EXPECT_FALSE(service.DeclareMaskPass(frame, 1, depth, camera, nullptr, 0.005f).IsValid());
    EXPECT_FALSE(service.DeclareMaskPass(frame, 1, depth, camera, light,
        std::numeric_limits<float>::quiet_NaN()).IsValid());
    camera.proj[15] = 1; // orthographic
    EXPECT_FALSE(service.DeclareMaskPass(frame, 1, depth, camera, light, 0.005f).IsValid());
    camera = {};
    EXPECT_FALSE(service.DeclareMaskPass(frame, 1, depth, camera, light, 0.005f).IsValid());
    EXPECT_EQ(frame.Graph().PassCount(), 0u);
    EXPECT_FALSE(frame.FindTexture("ScreenSpaceShadows.View1").IsValid());
    device->WaitForIdle();
}

TEST(ScreenSpaceShadowPasses, PackagedPassesExecuteAndKeepViewsAndResizesSeparate)
{
    auto device = CreateVulkanDeviceFast();
    if (!device) GTEST_SKIP() << "No Vulkan device";
    Pools pools(device.get());
    RG::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Upload);
    ScreenSpaceShadowPasses service(device.get());
    auto camera = PerspectiveCamera();
    const float light[3] = {0.3f, 0.1f, -1.0f};
    uint32_t frameIndex = 0;
    for (auto size : {std::array<uint32_t, 2>{17, 13}, {17, 13}, {17, 13}, {17, 13},
                      {129, 67}, {129, 67}, {129, 67}, {129, 67},
                      {17, 13}, {17, 13}, {17, 13}, {17, 13}})
    {
        frame.BeginFrame(frameIndex++);
        const auto depth = frame.ImportPersistentTexture("SSS.Depth", DepthDesc(size[0], size[1]));
        frame.AddPass("SSS.ClearDepth", 0, [=](RG::RGPassBuilder& pass)
            {
                RG::RGAttachmentOps ops{};
                ops.Load = RG::RGLoadOp::Clear;
                ops.Clear.Depth = 0;
                pass.AttachDepth(depth, ops);
            }, [](RG::RGContext&) {});
        auto first = service.DeclareMaskPass(frame, 1, depth, camera, light, 0.005f);
        auto constants = frame.AllocUpload<ShadowDataGPU>();
        ASSERT_TRUE(constants.Valid());
        *constants.Ptr = {};
        for (int i = 0; i < 16; i += 5) constants.Ptr->shadowVP[0][i] = 1;
        constants.Ptr->shadowSplits[0] = 100;
        constants.Ptr->shadowParams[2] = 1;
        ScreenSpaceShadowFilter filter{constants.Buffer, constants.Offset, sizeof(ShadowDataGPU), 64};
        auto second = service.DeclareMaskPass(frame, 2, depth, camera, light, 0.01f, &filter);
        ASSERT_TRUE(first.IsValid() && second.IsValid()) << "Required shader packages failed to load";
        EXPECT_NE(frame.PhysicalTexture(first), frame.PhysicalTexture(second));
        EXPECT_EQ(frame.Graph().ResourceDesc(first.Id).Width, size[0]);
        EXPECT_EQ(frame.Graph().PassCount(), 8u);
        EXPECT_FALSE(frame.FindTexture("ScreenSpaceShadows.View1.History0").IsValid());
        EXPECT_FALSE(frame.FindTexture("ScreenSpaceShadows.View1.History1").IsValid());
        EXPECT_FALSE(frame.FindTexture("ScreenSpaceShadows.View2.History0").IsValid());
        EXPECT_FALSE(frame.FindTexture("ScreenSpaceShadows.View2.History1").IsValid());
        // External diagnostic readback is a next-frame consumer. Persistent
        // pools retain the requested copy usage on their next materialization.
        frame.RequireTransferUsage(first, TextureUsage::TransferSrc);
        frame.RequireTransferUsage(second, TextureUsage::TransferSrc);
        frame.MarkOutput(first); frame.MarkOutput(second);
        frame.Execute(); device->WaitForIdle();
        // Both prepare passes must survive graph culling: atomics consume them.
        EXPECT_EQ(frame.Graph().ScheduledOrder().size(), 8u);
        if ((frameIndex & 3u) != 0u) continue;
        for (auto mask : {first, second})
        {
            BufferDesc desc{};
            desc.size = size[0] * size[1] * 4;
            desc.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
            desc.memoryUsage = BufferMemoryUsage::Readback;
            auto buffer = device->CreateBuffer(desc);
            ASSERT_TRUE(buffer.IsValid());
            auto command = device->CreateCommandList(IDevice::QueueType::Graphics);
            command->Begin();
            command->Barrier(ResourceBarrier::CreateTextureBarrier(frame.PhysicalTexture(mask),
                ResourceState::UnorderedAccess, ResourceState::CopySource));
            command->CopyTextureToBuffer(frame.PhysicalTexture(mask), buffer, size[0], size[1]);
            command->Barrier(ResourceBarrier::CreateTextureBarrier(frame.PhysicalTexture(mask),
                ResourceState::CopySource, ResourceState::UnorderedAccess));
            command->End(); device->ExecuteCommandLists({command.get()}); device->WaitForIdle();
            auto* values = static_cast<const uint32_t*>(device->MapBuffer(buffer));
            ASSERT_NE(values, nullptr);
            for (uint32_t i = 0; i < size[0] * size[1]; ++i)
                ASSERT_EQ(values[i], 255u) << "sky pixel " << i;
            device->UnmapBuffer(buffer); device->DestroyBuffer(buffer);
        }
    }
    device->WaitForIdle();
}

TEST(ScreenSpaceShadowPasses, PackagedMatchingChangesTheProducedContactFootprint)
{
    auto device = CreateVulkanDeviceFast();
    if (!device) GTEST_SKIP() << "No GPU device";
    constexpr uint32_t w = 129, h = 67;
    std::vector<float> values(w * h, 0.4f);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w / 2; ++x) values[y * w + x] = 0.405f;
    TextureDesc td = DepthDesc(w, h);
    td.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
    const auto texture = device->CreateTexture(td);
    BufferDesc bd{};
    bd.size = values.size() * 4;
    bd.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
    bd.memoryUsage = BufferMemoryUsage::Upload;
    const auto upload = device->CreateBuffer(bd);
    device->UpdateBuffer(upload, 0, values.size() * 4, values.data());
    auto command = device->CreateCommandList(IDevice::QueueType::Graphics);
    command->Begin();
    command->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::Undefined, ResourceState::CopyDest));
    command->CopyBufferToTextureSubresource(upload, texture, 0, 0, w, h, 0);
    command->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::CopyDest, ResourceState::ShaderResource));
    command->End(); device->ExecuteCommandLists({command.get()}); device->WaitForIdle();
    {
        Pools pools(device.get());
        RG::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Upload);
        ScreenSpaceShadowPasses service(device.get());
        const auto camera = PerspectiveCamera();
        const float light[3] = {-4 / camera.proj[0], 0, -1};
        std::array<RG::RGTexture, 2> masks;
        for (uint32_t index = 0; index < 4; ++index)
        {
            frame.BeginFrame(index);
            const auto depth = frame.ImportExternalTexture("ContactStep", texture,
                ResourceState::ShaderResource, TextureFormat::R32_FLOAT);
            auto constants = frame.AllocUpload<ShadowDataGPU>();
            ASSERT_TRUE(constants.Valid());
            *constants.Ptr = {};
            const float inverseW = (0.4f - camera.proj[10]) / camera.proj[14];
            constants.Ptr->shadowVP[0][0] = w * camera.proj[0] * inverseW / (64 * 3);
            constants.Ptr->shadowVP[0][5] = h * camera.proj[5] * inverseW / (64 * 3);
            constants.Ptr->shadowVP[0][10] = constants.Ptr->shadowVP[0][15] = 1;
            constants.Ptr->shadowSplits[0] = 100;
            constants.Ptr->shadowParams[2] = 1;
            ScreenSpaceShadowFilter filter{constants.Buffer, constants.Offset, sizeof(ShadowDataGPU), 64};
            masks[0] = service.DeclareMaskPass(frame, 1, depth, camera, light, 0.005f);
            masks[1] = service.DeclareMaskPass(frame, 2, depth, camera, light, 0.005f, &filter);
            for (auto mask : masks)
            {
                ASSERT_TRUE(mask.IsValid());
                frame.RequireTransferUsage(mask, TextureUsage::TransferSrc);
                frame.MarkOutput(mask);
            }
            frame.Execute(); device->WaitForIdle();
        }
        bd.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
        bd.memoryUsage = BufferMemoryUsage::Readback;
        std::array<std::vector<uint32_t>, 2> output;
        for (uint32_t i = 0; i < 2; ++i)
        {
            const auto readback = device->CreateBuffer(bd);
            command->Begin();
            command->Barrier(ResourceBarrier::CreateTextureBarrier(frame.PhysicalTexture(masks[i]),
                ResourceState::UnorderedAccess, ResourceState::CopySource));
            command->CopyTextureToBuffer(frame.PhysicalTexture(masks[i]), readback, w, h);
            command->End(); device->ExecuteCommandLists({command.get()}); device->WaitForIdle();
            const auto* mapped = static_cast<const uint32_t*>(device->MapBuffer(readback));
            ASSERT_NE(mapped, nullptr);
            output[i].assign(mapped, mapped + w * h);
            device->UnmapBuffer(readback); device->DestroyBuffer(readback);
        }
        uint32_t changedReceivers = 0, shadowedReceivers = 0;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = w / 2; x < w; ++x)
            {
                const auto a = output[0][y * w + x] & 255u;
                const auto b = output[1][y * w + x] & 255u;
                changedReceivers += a != b;
                shadowedReceivers += a < 230;
            }
        EXPECT_GT(shadowedReceivers, h);
        EXPECT_GT(changedReceivers, h) << "Matching must consume the live reflected camera/cascade bindings";
    }
    device->DestroyBuffer(upload); device->DestroyTexture(texture);
}


TEST(ScreenSpaceShadowSettings, CascadeDepthBoundsAdoptContractionWithoutTemporalSettling)
{
    ShadowMapRenderFeature feature;
    feature.UpdateSDSMBounds(1, 0.5f, 100.0f);
    feature.UpdateSDSMBounds(1, 2.0f, 20.0f);
    const auto contracted = feature.GetSDSMBounds(1);
    EXPECT_TRUE(contracted.valid);
    EXPECT_FLOAT_EQ(contracted.nearDepth, 2.0f);
    EXPECT_FLOAT_EQ(contracted.farDepth, 20.0f);
    for (int i = 0; i < 10; ++i)
    {
        feature.UpdateSDSMBounds(1, 2.0f, 20.0f);
        EXPECT_FLOAT_EQ(feature.GetSDSMBounds(1).nearDepth, contracted.nearDepth);
        EXPECT_FLOAT_EQ(feature.GetSDSMBounds(1).farDepth, contracted.farDepth);
    }
    feature.UpdateSDSMBounds(2, 1.0f, 40.0f);
    EXPECT_FLOAT_EQ(feature.GetSDSMBounds(1).farDepth, 20.0f);
    EXPECT_FLOAT_EQ(feature.GetSDSMBounds(2).farDepth, 40.0f);
    feature.UpdateSDSMBounds(1, 0.25f, 120.0f);
    EXPECT_FLOAT_EQ(feature.GetSDSMBounds(1).nearDepth, 0.25f);
    EXPECT_FLOAT_EQ(feature.GetSDSMBounds(1).farDepth, 120.0f);
}

TEST(ScreenSpaceShadowSettings, CascadeDepthBoundsDoNotJumpAtTheLastReverseZBucket)
{
    // The reported zoom crosses this device-depth boundary with a 1 cm near
    // plane. Rounding the lower value to zero used to expand a ~10 m fit to
    // the camera far plane, capped later at the 200 m shadow distance.
    constexpr float boundary = 1.0f / 1024.0f;
    const auto before = ShadowMapRenderFeature::ResolveSDSMBounds(
        boundary * 1.0001f, 0.003f, 0.01f, 3000.0f, false, true);
    const auto after = ShadowMapRenderFeature::ResolveSDSMBounds(
        boundary * 0.9999f, 0.003f, 0.01f, 3000.0f, false, true);
    ASSERT_TRUE(before.valid && after.valid);
    EXPECT_GT(before.farDepth, 10.0f);
    EXPECT_LT(after.farDepth, 11.0f);
    EXPECT_LT(after.farDepth - before.farDepth, 0.02f);
}

TEST(ScreenSpaceShadowSettings, CascadeDepthRoundingStaysConservativeAndRelativeToDistance)
{
    for (const bool orthographic : {false, true})
        for (const float nearPlane : {0.01f, 0.1f})
            for (const float distance : {0.25f, 0.999f, 1.001f, 3.999f, 4.001f,
                                         10.21f, 100.0f, 200.0f, 2999.0f})
            {
                SCOPED_TRACE(::testing::Message() << orthographic << ", " << nearPlane << ", " << distance);
                constexpr float farPlane = 3000.0f;
                const auto projection = orthographic
                    ? Mathematics::MakeOrthographicLH_ZO_ReverseZ(-1, 1, -1, 1, nearPlane, farPlane)
                    : Mathematics::MakePerspectiveLH_ZO_ReverseZ(1.0471976f, 1.5f, nearPlane, farPlane);
                const auto project = [&](float z)
                {
                    return (projection.Data()[10] * z + projection.Data()[14]) /
                           (projection.Data()[11] * z + projection.Data()[15]);
                };
                const float minNdc = project(distance);
                const float maxNdc = project(std::max(nearPlane, distance * 0.5f));
                const auto raw = ShadowMapRenderFeature::ResolveSDSMBounds(
                    minNdc, maxNdc, nearPlane, farPlane, orthographic, false);
                const auto rounded = ShadowMapRenderFeature::ResolveSDSMBounds(
                    minNdc, maxNdc, nearPlane, farPlane, orthographic, true);
                ASSERT_TRUE(raw.valid && rounded.valid);
                EXPECT_LE(rounded.nearDepth, raw.nearDepth);
                EXPECT_GE(rounded.farDepth, raw.farDepth);
                EXPECT_LE(raw.nearDepth - rounded.nearDepth, raw.nearDepth / 1024.0f);
                EXPECT_LE(rounded.farDepth - raw.farDepth, raw.farDepth / 1024.0f);
                EXPECT_GE(rounded.nearDepth, nearPlane);
                EXPECT_LE(rounded.farDepth, farPlane);
            }
}

TEST(ScreenSpaceShadowSettings, CascadeDepthRoundingRejectsJitterWithoutConvergence)
{
    constexpr float nearPlane = 0.01f, farPlane = 3000.0f;
    const auto projection = Mathematics::MakePerspectiveLH_ZO_ReverseZ(1.0471976f, 1.5f, nearPlane, farPlane);
    const auto project = [&](float z) { return projection.Data()[10] + projection.Data()[14] / z; };
    const auto resolve = [&](float jitter)
    {
        return ShadowMapRenderFeature::ResolveSDSMBounds(
            project(10.002f + jitter), project(2.001f + jitter), nearPlane, farPlane, false, true);
    };
    const auto target = resolve(0.0f);
    ASSERT_TRUE(target.valid);
    ShadowMapRenderFeature feature;
    feature.UpdateSDSMBounds(1, 0.5f, 200.0f);
    for (const float jitter : {-0.0001f, 0.0001f, -0.00005f, 0.00005f, 0.0f})
    {
        const auto resolved = resolve(jitter);
        ASSERT_TRUE(resolved.valid);
        feature.UpdateSDSMBounds(1, resolved.nearDepth, resolved.farDepth);
        EXPECT_FLOAT_EQ(feature.GetSDSMBounds(1).nearDepth, target.nearDepth);
        EXPECT_FLOAT_EQ(feature.GetSDSMBounds(1).farDepth, target.farDepth);
    }
}

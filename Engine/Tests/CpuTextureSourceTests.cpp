#include <gtest/gtest.h>
#if defined(RENDERING_HAS_VULKAN) && RENDERING_HAS_VULKAN
#include "Assets/RenderPipelineAsset.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "CpuTextureSources.h"
#include "ECS/ECSTemplates.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/Pipeline/Nodes/CpuTextureInputNode.h"
#include "Engine/Rendering/Pipeline/Nodes/EngineNodeTypes.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/TextureService.h"
#include "NativeScripting/UserSystemRegistry.h"
#include "PathfindingECS/Components/NavigationAgent.h"
#include "PathfindingECS/Components/NavigationAgentState.h"
#include "PathfindingECS/Components/NavigationGrid.h"
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/Systems/NavigationBuildSystem.h"
#include "PathfindingECS/Systems/NavigationMovementSystem.h"
#include "PathfindingECS/Systems/NavigationPathfindingSystem.h"
#include "PathfindingECS/Systems/NavigationWorldHooks.h"
#include "Rendering/Utils/TextureUploadHelpers.h"
#include "Scripting/NativePostSimulationSystem.h"
#include "Source/Vulkan/VulkanDevice.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;

namespace
{
TEST(CpuTexturePipeline, AuthoredOutputsResolveForFullscreenConsumers)
{
    using namespace Pipeline;
    // The engine registrations carry each type's reference and publish keys,
    // which is what lets the consumer's refs resolve against the source's names.
    RenderPipelineNodeRegistry registry;
    Nodes::RegisterEngineNodeTypes(registry);
    const std::string json = R"({
        "schemaVersion":2,"pipelineName":"CpuMask",
        "passes":[
            {"id":"Source","type":"CpuTextureInput","source":"Sight","output":"Mask","parameters":"Bounds"},
            {"id":"Apply","type":"FullscreenShader","shaderPkg":"unused.shaderpkg",
             "inputs":{"uMask":"Mask"},"buffers":{"uBounds":"Bounds"},"output":"View.Resolve"}
        ]
    })";
    RenderPipelineAsset asset(GUID::Null(), std::filesystem::path("cpu-mask.rendergraph"));
    asset.LoadFromData(Vector<uint8>(json.begin(), json.end()));
    const auto blueprint = RenderPipelineCompiler().Compile(asset, registry);
    for (const auto& issue : blueprint.issues)
        EXPECT_NE(issue.severity, PipelineIssueSeverity::Error) << issue.message;
    ASSERT_EQ(blueprint.passes.size(), 2u);
    EXPECT_EQ(blueprint.passes.front().type, "CpuTextureInput");
}

class UploadDevice : public VulkanDevice
{
  public:
    bool RefuseStaging = false, RefuseCommands = false, SimulateLost = false;
    bool ThrowDuringStaging = false, ThrowAfterDispatch = false;
    uint32_t Submits = 0, Created = 0, Destroyed = 0, Buffers = 0, RetiredBuffers = 0;
    uint32_t StagingRequests = 0, RefuseStagingAt = 0;
    DeviceHealth GetDeviceHealth() const override
    {
        return SimulateLost ? DeviceHealth::Rebuilding : VulkanDevice::GetDeviceHealth();
    }
    BufferHandle CreateBuffer(const BufferDesc& desc) override
    {
        if (desc.memoryUsage == BufferMemoryUsage::Upload)
        {
            ++StagingRequests;
            if (ThrowDuringStaging)
                throw std::bad_alloc();
            if (RefuseStaging || StagingRequests == RefuseStagingAt)
                return {};
        }
        auto handle = VulkanDevice::CreateBuffer(desc);
        if (handle.IsValid())
            ++Buffers;
        return handle;
    }
    void DestroyBuffer(BufferHandle handle) override
    {
        ++RetiredBuffers;
        VulkanDevice::DestroyBuffer(handle);
    }
    TextureHandle CreateTexture(const TextureDesc& desc) override
    {
        auto handle = VulkanDevice::CreateTexture(desc);
        if (handle.IsValid())
            ++Created;
        return handle;
    }
    void DestroyTexture(TextureHandle handle) override
    {
        ++Destroyed;
        VulkanDevice::DestroyTexture(handle);
    }
    std::unique_ptr<CommandList> CreateCommandList(QueueType type) override
    {
        return RefuseCommands ? nullptr : VulkanDevice::CreateCommandList(type);
    }
    void ExecuteCommandLists(const std::vector<CommandList*>& lists) override
    {
        ++Submits;
        VulkanDevice::ExecuteCommandLists(lists);
        if (ThrowAfterDispatch)
            throw std::bad_alloc();
    }
};

struct Params
{
    float X, Z, Width, Height;
};
auto Bytes(const Params& params)
{
    return std::as_bytes(std::span(&params, 1));
}

class CpuTextureTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        DeviceDesc desc{};
        desc.enableSwapchain = false;
        desc.enableDebugLayer = false;
        desc.enableDynamicRendering = true;
        ASSERT_TRUE(Device.Initialize(desc));
        Sources.Open();
    }
    void TearDown() override
    {
        Sources.Shutdown(&Device);
        Device.WaitForIdle();
        Device.Shutdown();
    }
    CpuTextureSourceDesc Desc(uint64_t world = 21, uint64_t reset = 0) const
    {
        return {{world, reset}, HashStringId("Sight"), TextureFormat::R8G8_UNORM, sizeof(Params)};
    }
    CpuTextureBinding Binding(uint64_t world = 21) { return Sources.Lookup(world, HashStringId("Sight")); }
    std::vector<std::byte> Read(const CpuTextureBinding& binding)
    {
        const size_t size = size_t(binding.Width) * binding.Height * BytesPerPixel(binding.Format);
        auto buffer = Device.CreateReadbackBuffer(size, "CpuTextureOracle");
        EXPECT_TRUE(buffer.IsValid());
        auto commands = Device.CreateCommandList(IDevice::QueueType::Graphics);
        commands->Begin();
        commands->Barrier(ResourceBarrier::CreateTextureBarrier(binding.Texture,
                                                                ResourceState::ShaderResource, ResourceState::CopySource));
        commands->CopyTextureToBuffer(binding.Texture, buffer, binding.Width, binding.Height);
        commands->Barrier(ResourceBarrier::CreateTextureBarrier(binding.Texture,
                                                                ResourceState::CopySource, ResourceState::ShaderResource));
        commands->End();
        Device.ExecuteCommandLists({commands.get()});
        Device.WaitForIdle();
        std::vector<std::byte> result(size);
        const void* mapped = Device.MapBuffer(buffer);
        EXPECT_NE(mapped, nullptr);
        if (mapped)
            std::memcpy(result.data(), mapped, size);
        Device.UnmapBuffer(buffer);
        Device.DestroyBuffer(buffer);
        return result;
    }
    UploadDevice Device;
    CpuTextureSources Sources;
};

TEST_F(CpuTextureTest, OwnedLatestRevisionUploadsOnceForAllViews)
{
    auto source = Sources.Create(Desc());
    ASSERT_TRUE(source);
    std::array<std::byte, 8> pixels{std::byte{1}, std::byte{2}, std::byte{3}};
    const Params params{10, -20, 256, 128};
    ASSERT_TRUE(source.Publish(2, 2, pixels, Bytes(params)));
    pixels.fill(std::byte{7});
    ASSERT_TRUE(source.Publish(2, 2, pixels, Bytes(params)));
    pixels.fill(std::byte{9}); // Caller storage is not retained.
    Sources.Flush(Device);
    EXPECT_FALSE(Binding().Texture.IsValid()); // Exact world acknowledgement required.
    Sources.ObserveWorld(Desc().Scope);
    const auto before = Device.Submits;
    Sources.Flush(Device);
    EXPECT_EQ(Device.Submits, before + 1);
    ASSERT_TRUE(Binding().Texture.IsValid());
    EXPECT_EQ(Read(Binding()), std::vector<std::byte>(8, std::byte{7}));
    EXPECT_EQ(std::memcmp(Binding().Parameters.data(), &params, sizeof(params)), 0);
    const auto after = Device.Submits;
    for (int i = 0; i < 5; ++i)
    {
        Sources.ObserveWorld(Desc().Scope);
        Sources.Flush(Device);
        EXPECT_TRUE(Binding().Texture.IsValid());
    }
    EXPECT_EQ(Device.Submits, after);
}

TEST_F(CpuTextureTest, FailedStagingAndCommandCreationRetainPixelsAndMetadata)
{
    auto source = Sources.Create(Desc());
    Sources.ObserveWorld(Desc().Scope);
    const std::array<std::byte, 8> oldPixels{std::byte{17}};
    const Params oldParams{1, 2, 3, 4}, newParams{100, 200, 300, 400};
    ASSERT_TRUE(source.Publish(2, 2, oldPixels, Bytes(oldParams)));
    Sources.Flush(Device);
    const auto original = Binding();
    for (int failure = 0; failure < 3; ++failure)
    {
        const std::array<std::byte, 8> newPixels{std::byte{99}};
        ASSERT_TRUE(source.Publish(2, 2, newPixels, Bytes(newParams)));
        Device.RefuseStaging = failure == 0;
        Device.RefuseCommands = failure == 1;
        Device.ThrowDuringStaging = failure == 2;
        const auto before = Device.Submits, buffers = Device.Buffers, retired = Device.RetiredBuffers;
        Sources.Flush(Device);
        EXPECT_EQ(Device.Submits, before);
        EXPECT_EQ(Device.Buffers - buffers, Device.RetiredBuffers - retired);
        EXPECT_EQ(Binding().Texture, original.Texture);
        EXPECT_EQ(std::memcmp(Binding().Parameters.data(), &oldParams, sizeof(oldParams)), 0);
        Device.RefuseStaging = Device.RefuseCommands = Device.ThrowDuringStaging = false;
        EXPECT_EQ(Read(Binding()), std::vector<std::byte>(oldPixels.begin(), oldPixels.end()));
    }
    Sources.Flush(Device);
    EXPECT_EQ(Read(Binding()).front(), std::byte{99});
    EXPECT_EQ(std::memcmp(Binding().Parameters.data(), &newParams, sizeof(newParams)), 0);
}

TEST_F(CpuTextureTest, AmbiguousDispatchInvalidatesInPlaceImageAndRetriesCompleteRevision)
{
    auto source = Sources.Create(Desc());
    Sources.ObserveWorld(Desc().Scope);
    const Params oldParams{1, 2, 3, 4}, newParams{11, 22, 33, 44};
    ASSERT_TRUE(source.Publish(2, 2, std::array<std::byte, 8>{std::byte{5}}, Bytes(oldParams)));
    Sources.Flush(Device);
    ASSERT_TRUE(source.Publish(2, 2, std::array<std::byte, 8>{std::byte{9}}, Bytes(newParams)));
    Device.ThrowAfterDispatch = true;
    const auto before = Device.Submits;
    EXPECT_THROW(Sources.Flush(Device), std::bad_alloc); // The real GPU copy ran, then dispatch threw.
    EXPECT_EQ(Device.Submits, before + 1);
    EXPECT_FALSE(Binding().Texture.IsValid());
    Device.ThrowAfterDispatch = false;
    Sources.Flush(Device);
    ASSERT_TRUE(Binding().Texture.IsValid());
    EXPECT_EQ(Read(Binding()).front(), std::byte{9});
    EXPECT_EQ(std::memcmp(Binding().Parameters.data(), &newParams, sizeof(newParams)), 0);

    const auto previous = Binding();
    ASSERT_TRUE(source.Publish(3, 1, std::array<std::byte, 6>{std::byte{19}}, Bytes(oldParams)));
    Device.ThrowAfterDispatch = true;
    EXPECT_THROW(Sources.Flush(Device), std::bad_alloc); // A replacement cannot mutate the old image.
    Device.ThrowAfterDispatch = false;
    EXPECT_EQ(Binding().Texture, previous.Texture);
    EXPECT_EQ(Read(Binding()).front(), std::byte{9});
    EXPECT_EQ(std::memcmp(Binding().Parameters.data(), &newParams, sizeof(newParams)), 0);
    Sources.Flush(Device);
    EXPECT_EQ(Binding().Width, 3u);
    EXPECT_EQ(Read(Binding()).front(), std::byte{19});
    EXPECT_EQ(std::memcmp(Binding().Parameters.data(), &oldParams, sizeof(oldParams)), 0);
}

TEST_F(CpuTextureTest, ResizeFailureKeepsOldHandleThenRetiresItOnSuccess)
{
    auto source = Sources.Create(Desc());
    Sources.ObserveWorld(Desc().Scope);
    const Params params{};
    ASSERT_TRUE(source.Publish(1, 1, std::array<std::byte, 2>{}, Bytes(params)));
    Sources.Flush(Device);
    const auto old = Binding();
    ASSERT_TRUE(source.Publish(2, 2, std::array<std::byte, 8>{}, Bytes(params)));
    const auto created = Device.Created, destroyed = Device.Destroyed;
    Device.RefuseStaging = true;
    Sources.Flush(Device);
    EXPECT_EQ(Binding().Texture, old.Texture);
    EXPECT_EQ(Device.Created, created + 1);
    EXPECT_EQ(Device.Destroyed, destroyed + 1); // Failed replacement only.
    Device.RefuseStaging = false;
    Sources.Flush(Device);
    EXPECT_NE(Binding().Texture, old.Texture);
    EXPECT_EQ(Binding().Width, 2u);
    EXPECT_EQ(Device.Destroyed, destroyed + 2);
}

TEST_F(CpuTextureTest, ScopeResetDuplicateLeaseAndShutdownCannotReviveOldPublisher)
{
    auto source = Sources.Create(Desc());
    EXPECT_FALSE(Sources.Create(Desc()));
    auto other = Sources.Create(Desc(22));
    const Params params{};
    ASSERT_TRUE(source.Publish(1, 1, std::array<std::byte, 2>{}, Bytes(params)));
    ASSERT_TRUE(other.Publish(1, 1, std::array<std::byte, 2>{}, Bytes(params)));
    Sources.ObserveWorld(Desc().Scope);
    Sources.ObserveWorld(Desc(22).Scope);
    Sources.Flush(Device);
    EXPECT_TRUE(Binding(21).Texture.IsValid());
    EXPECT_TRUE(Binding(22).Texture.IsValid());
    Sources.ObserveWorld(Desc(21, 1).Scope);
    EXPECT_FALSE(source);
    EXPECT_FALSE(source.Publish(1, 1, std::array<std::byte, 2>{}, Bytes(params)));
    EXPECT_FALSE(Binding(21).Texture.IsValid());
    EXPECT_TRUE(Binding(22).Texture.IsValid());
    auto replacement = Sources.Create(Desc(21, 1));
    ASSERT_TRUE(replacement);
    source.Reset(); // Retired old lease cannot close replacement.
    ASSERT_TRUE(replacement.Publish(1, 1, std::array<std::byte, 2>{}, Bytes(params)));
    Sources.ObserveWorld(Desc(21, 1).Scope);
    Sources.Flush(Device);
    EXPECT_TRUE(Binding(21).Texture.IsValid());
    replacement.Reset();
    EXPECT_FALSE(Binding(21).Texture.IsValid());
    auto sameKey = Sources.Create(Desc(21, 1));
    EXPECT_TRUE(sameKey);
    Sources.Shutdown(&Device);
    EXPECT_FALSE(sameKey);
    EXPECT_FALSE(other);
    EXPECT_FALSE(Sources.Create(Desc()));
    EXPECT_FALSE(other.Publish(1, 1, std::array<std::byte, 2>{}, Bytes(params)));
}

TEST_F(CpuTextureTest, InvalidPayloadCannotReplaceLastGoodAndFormatsAreExplicit)
{
    auto desc = Desc();
    desc.ParameterBytes = 17;
    EXPECT_FALSE(Sources.Create(desc));
    desc = Desc();
    desc.Format = TextureFormat::RGBA8_SRGB;
    EXPECT_FALSE(Sources.Create(desc));
    desc = Desc();
    desc.Scope.WorldId = 0;
    EXPECT_FALSE(Sources.Create(desc));
    auto source = Sources.Create(Desc());
    const Params params{};
    std::array<std::byte, 8> data{};
    ASSERT_TRUE(source.Publish(2, 2, data, Bytes(params)));
    EXPECT_FALSE(source.Publish(0, 2, data, Bytes(params)));
    EXPECT_FALSE(source.Publish(0xffffffff, 2, data, Bytes(params)));
    EXPECT_FALSE(source.Publish(2, 2, data));
    EXPECT_FALSE(source.Publish(2, 3, data, Bytes(params)));
    Sources.ObserveWorld(Desc().Scope);
    Sources.Flush(Device);
    ASSERT_TRUE(Binding().Texture.IsValid());
    EXPECT_EQ(Read(Binding()), std::vector<std::byte>(8));
}

TEST_F(CpuTextureTest, DeviceReprovisionDiscardsDeadHandlesAndReuploadsRetainedBytes)
{
    auto source = Sources.Create(Desc());
    const Params params{5, 6, 7, 8};
    std::array<std::byte, 8> data{std::byte{73}};
    ASSERT_TRUE(source.Publish(2, 2, data, Bytes(params)));
    Sources.ObserveWorld(Desc().Scope);
    Sources.Flush(Device);
    const auto old = Binding();
    Device.WaitForIdle();
    Device.DestroyTexture(old.Texture); // Stand in for teardown of this owned resource.
    const auto destroyed = Device.Destroyed;
    Sources.Reprovision();
    EXPECT_EQ(Device.Destroyed, destroyed); // Never destroy reissued IDs.
    EXPECT_FALSE(Binding().Texture.IsValid());
    Sources.Flush(Device);
    ASSERT_TRUE(Binding().Texture.IsValid());
    EXPECT_NE(Binding().Texture, old.Texture);
    EXPECT_EQ(Read(Binding()), std::vector<std::byte>(data.begin(), data.end()));
    EXPECT_EQ(std::memcmp(Binding().Parameters.data(), &params, sizeof(params)), 0);
}

TEST_F(CpuTextureTest, ScalarAndRgbaImagesDoNotRequireMetadata)
{
    for (auto format : {TextureFormat::R8_UNORM, TextureFormat::RGBA8_UNORM})
    {
        auto desc = Desc();
        desc.Format = format;
        desc.ParameterBytes = 0;
        auto source = Sources.Create(desc);
        ASSERT_TRUE(source);
        std::vector<std::byte> data(4 * BytesPerPixel(format), std::byte{43});
        ASSERT_TRUE(source.Publish(2, 2, data));
        Sources.ObserveWorld(desc.Scope);
        Sources.Flush(Device);
        EXPECT_EQ(Binding().ParameterBytes, 0u);
        EXPECT_EQ(Read(Binding()), data);
        source.Reset();
        Sources.Flush(Device);
    }
}

TEST_F(CpuTextureTest, CheckedBatchDoesNotSubmitAnyCopiesWhenOneStagingBufferFails)
{
    TextureDesc desc{};
    desc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    desc.usage = static_cast<uint32_t>(TextureUsage::TransferDst | TextureUsage::ShaderResource |
                                       TextureUsage::TransferSrc);
    auto a = Device.CreateTexture(desc), b = Device.CreateTexture(desc);
    const std::array<std::byte, 4> oldData{std::byte{11}}, newData{std::byte{77}};
    TextureUploadEntry entries[] = {{a, oldData.data(), 1, 1, 1, 4},
                                    {b, oldData.data(), 1, 1, 1, 4}};
    UploadTexturesBatched(&Device, entries, 2);
    for (auto& entry : entries)
    {
        entry.Pixels = newData.data();
        entry.SourceState = ResourceState::ShaderResource;
    }
    Device.RefuseStagingAt = Device.StagingRequests + 2;
    const auto before = Device.Submits;
    const auto buffers = Device.Buffers, retired = Device.RetiredBuffers;
    // Deliberately ignore the return value: this oracle also compiles against
    // the old void helper and fails there because it submits the first copy.
    UploadTexturesBatched(&Device, entries, 2);
    EXPECT_EQ(Device.Submits, before);
    EXPECT_EQ(Device.Buffers - buffers, 1u);
    EXPECT_EQ(Device.RetiredBuffers - retired, 1u);
    for (auto texture : {a, b})
    {
        EXPECT_EQ(Read({texture, TextureFormat::RGBA8_UNORM, 1, 1}),
                  std::vector<std::byte>(oldData.begin(), oldData.end()));
        Device.DestroyTexture(texture);
    }
}

TEST_F(CpuTextureTest, StaleLeaseCreatedAfterObservationCannotBorrowCurrentWorldScope)
{
    auto current = Sources.Create(Desc(21, 1));
    const Params params{};
    ASSERT_TRUE(current.Publish(1, 1, std::array<std::byte, 2>{std::byte{8}}, Bytes(params)));
    Sources.ObserveWorld(Desc(21, 1).Scope);
    Sources.Flush(Device);
    const auto binding = Binding();
    auto stale = Sources.Create(Desc(21, 0));
    ASSERT_TRUE(stale); // Not observed yet; acceptance does not imply eligibility.
    ASSERT_TRUE(stale.Publish(1, 1, std::array<std::byte, 2>{std::byte{99}}, Bytes(params)));
    const auto before = Device.Submits;
    Sources.Flush(Device);
    EXPECT_EQ(Device.Submits, before);
    EXPECT_EQ(Binding().Texture, binding.Texture);
    EXPECT_EQ(Read(Binding()).front(), std::byte{8});
    Sources.ObserveWorld(Desc(21, 1).Scope);
    EXPECT_FALSE(stale);
    EXPECT_TRUE(current);
    Sources.Flush(Device);
    EXPECT_EQ(Binding().Texture, binding.Texture);
}

TEST_F(CpuTextureTest, PublisherThreadRacingTheDrainNeverExposesATornPair)
{
    auto source = Sources.Create(Desc());
    Sources.ObserveWorld(Desc().Scope);
    constexpr uint32_t kSide = 512;
    std::atomic<bool> stop{false};
    std::thread publisher(
        [&]
        {
            std::vector<std::byte> pixels(size_t(kSide) * kSide * 2);
            for (uint32_t revision = 1; !stop.load(std::memory_order_relaxed); ++revision)
            {
                std::fill(pixels.begin(), pixels.end(), std::byte(revision & 255u));
                const Params params{float(revision & 255u), 0, float(kSide), float(kSide)};
                (void)source.Publish(kSide, kSide, pixels, Bytes(params));
            }
        });
    // Each iteration is one render-thread frame; the first upload waits for the
    // publisher's first revision, so nothing is checked until one has landed.
    constexpr uint32_t kFramesToCheck = 40;
    uint32_t checked = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (checked < kFramesToCheck && std::chrono::steady_clock::now() < deadline)
    {
        Sources.ObserveWorld(Desc().Scope);
        Sources.Flush(Device);
        const auto binding = Binding();
        if (!binding.Texture.IsValid())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        Params params{};
        std::memcpy(&params, binding.Parameters.data(), sizeof(params));
        const auto expected = std::byte(uint32_t(params.X) & 255u);
        const auto pixels = Read(binding);
        ASSERT_EQ(binding.Width, kSide);
        ASSERT_EQ(pixels.size(), size_t(kSide) * kSide * 2);
        // Image and metadata must come from the same revision, whatever the publisher is doing.
        EXPECT_TRUE(std::all_of(pixels.begin(), pixels.end(), [&](std::byte b) { return b == expected; }))
            << "check " << checked << " revision " << int(expected);
        ++checked;
    }
    stop.store(true, std::memory_order_relaxed);
    publisher.join();
    EXPECT_EQ(checked, kFramesToCheck);
}

TEST_F(CpuTextureTest, ActualTextureServiceAndPipelinePublishOnlyCoherentViewScopedPairs)
{
    using namespace Pipeline;
    RenderServices services;
    ASSERT_TRUE(services.Initialize(&Device));
    auto source = services.Textures().CreateCpuTextureSource(Desc());
    ASSERT_TRUE(source);
    const Params params{11, 22, 128, 256};
    ASSERT_TRUE(source.Publish(2, 2, std::array<std::byte, 8>{}, Bytes(params)));
    services.Textures().ObserveCpuTextureWorld(Desc().Scope);
    services.Textures().FlushPendingUploads();
    RenderPipelineNodeRegistry registry;
    ASSERT_TRUE(registry.Register("CpuTextureInput", []
                                  { return std::make_unique<Nodes::CpuTextureInputNode>(); }, true));
    RenderPipelineBlueprint blueprint;
    blueprint.pipelineName = "CpuTexturePair";
    RenderPipelineBlueprint::Pass pass;
    pass.id = "Source";
    pass.type = "CpuTextureInput";
    pass.perView = true;
    pass.enabled = true;
    pass.passJson = R"({"source":"Sight","output":"Mask","parameters":"Bounds"})";
    blueprint.passes.push_back(pass);
    RenderPipelineInstance instance(services, registry);
    instance.SetBlueprint(blueprint);
    std::vector<ViewDesc> views(3);
    for (uint32_t i = 0; i < views.size(); ++i)
    {
        views[i].id = i + 1;
        views[i].worldId = 21;
        views[i].purpose = ViewPurpose::Game;
    }
    views[1].worldId = 22;
    views[2].purpose = ViewPurpose::EditorScene;
    const std::vector<ViewTargetsRG> targets{{1}, {2}, {3}};
    {
        RenderGraph::RGResourcePool persistent(&Device);
        RenderGraph::RGTransientPool transient(&Device);
        RenderGraph::RGUploadRing ring(&Device, 2, 4096);
        RenderGraph::RGFrame frame(&Device, &persistent, &transient, &ring);
        frame.BeginFrame(0);
        instance.Declare(frame, targets, views);
        const auto* resources = instance.FrameResourcesFor(&frame);
        ASSERT_NE(resources, nullptr);
        EXPECT_TRUE(resources->Textures.contains({1, "Mask"}));
        EXPECT_TRUE(resources->Buffers.contains({1, "Bounds"}));
        EXPECT_FALSE(resources->Textures.contains({2, "Mask"}));
        EXPECT_FALSE(resources->Textures.contains({3, "Mask"}));
        const auto& buffer = resources->Buffers.at({1, "Bounds"});
        const auto* memory = static_cast<const std::byte*>(Device.MapBuffer(buffer.Buffer));
        ASSERT_NE(memory, nullptr);
        EXPECT_EQ(std::memcmp(memory + buffer.Offset, &params, sizeof(params)), 0);
        Device.SimulateLost = true;
        frame.BeginFrame(1);
        instance.Declare(frame, targets, views);
        resources = instance.FrameResourcesFor(&frame);
        EXPECT_FALSE(resources->Textures.contains({1, "Mask"}));
        EXPECT_FALSE(resources->Buffers.contains({1, "Bounds"}));
        Device.SimulateLost = false;
        source.Reset();
        frame.BeginFrame(2);
        EXPECT_EQ(instance.FrameResourcesFor(&frame), nullptr); // No prior-frame resource reuse.
        instance.Declare(frame, targets, views);
        resources = instance.FrameResourcesFor(&frame);
        EXPECT_FALSE(resources->Textures.contains({1, "Mask"}));
        EXPECT_FALSE(resources->Buffers.contains({1, "Bounds"}));
    }
    source = services.Textures().CreateCpuTextureSource(Desc());
    ASSERT_TRUE(source.Publish(2, 2, std::array<std::byte, 8>{}, Bytes(params)));
    services.Textures().ObserveCpuTextureWorld(Desc().Scope);
    services.Textures().FlushPendingUploads();
    {
        Device.RefuseStaging = true; // Refuse per-frame upload-ring allocation.
        RenderGraph::RGResourcePool persistent(&Device);
        RenderGraph::RGTransientPool transient(&Device);
        RenderGraph::RGUploadRing ring(&Device, 2, 4096);
        RenderGraph::RGFrame frame(&Device, &persistent, &transient, &ring);
        frame.BeginFrame(3);
        instance.Declare(frame, targets, views);
        const auto* resources = instance.FrameResourcesFor(&frame);
        ASSERT_NE(resources, nullptr);
        EXPECT_FALSE(resources->Textures.contains({1, "Mask"}));
        EXPECT_FALSE(resources->Buffers.contains({1, "Bounds"}));
        Device.RefuseStaging = false;
    }
    services.Shutdown();
    EXPECT_FALSE(source);
}

TEST_F(CpuTextureTest, ActualNavigationThenLateHookUploadsItsFirstRevisionInTheSameFrame)
{
    namespace ns = NativeScripting;
    struct Lifecycle
    {
        Lifecycle()
        {
            ns::ClearUserSystems();
            PathfindingECS::NavigationService::Initialize();
        }
        ~Lifecycle()
        {
            ns::ClearUserSystems();
            PathfindingECS::NavigationService::Shutdown();
        }
    } lifecycle;
    ECS::World world;
    PathfindingECS::RegisterNavigationWorldHooks(world);
    RenderServices services;
    ASSERT_TRUE(services.Initialize(&Device));
    auto grid = world.Create();
    Components::NavigationGrid desc;
    desc.Width = 16;
    desc.Depth = 16;
    desc.CellSize = 1;
    grid.Set(desc);
    world.ProcessCommands();
    PathfindingECS::NavigationBuildSystem{}.Update(world, .1f);
    world.ProcessCommands();
    const auto sourceGrid = *grid.Get<Components::NavigationGrid>();
    ASSERT_TRUE(sourceGrid.Initialized);
    auto actor = world.Create();
    Components::Transform transform;
    transform.matrix[12] = 2.5f;
    transform.matrix[14] = 2.5f;
    Components::WorldTransform worldTransform;
    std::memcpy(worldTransform.matrix, transform.matrix, sizeof(transform.matrix));
    Components::NavigationAgent agent;
    agent.Speed = 3.5f;
    agent.Acceleration = 100.f;
    agent.StoppingDistance = .3f;
    agent.Radius = .25f;
    agent.AgentId = actor.GetHandle().id;
    agent.NavMapIndex = sourceGrid.NavMapIndex;
    agent.NavMapGeneration = sourceGrid.NavMapGeneration;
    Components::NavigationAgentState nav;
    nav.HasDestination = true;
    nav.PathDirty = true;
    nav.DestinationX = 10.5f;
    nav.DestinationZ = 10.5f;
    actor.Set(transform);
    actor.Set(worldTransform);
    actor.Set(agent);
    actor.Set(nav);
    actor.Set(Components::MeshRenderer{});
    world.ProcessCommands();
    PathfindingECS::NavigationPathfindingSystem{}.Update(world, .1f);
    world.ProcessCommands();

    struct Publisher : ns::IUserSystem
    {
        RenderServices* Services;
        ECS::EntityHandle Actor;
        CpuTextureSource Lease;
        float InputX = 0, PresentedX = 0;
        unsigned Posts = 0;
        Publisher(RenderServices* services, ECS::EntityHandle actor) : Services(services), Actor(actor) {}
        const char* Name() const override { return "LateVisibility"; }
        bool HasPostSimulation() const override { return true; }
        void OnStart(ECS::World&) override {}
        void OnDestroy(ECS::World&) override {}
        void OnUpdate(ECS::World& world, float) override
        {
            InputX = world.GetComponent<Components::WorldTransform>(Actor)->matrix[12];
        }
        void OnPostSimulation(ECS::World& world, float) override
        {
            ++Posts;
            PresentedX = world.GetComponent<Components::WorldTransform>(Actor)->matrix[12];
            world.GetComponentForWrite<Components::MeshRenderer>(Actor)->renderLayerMask = 2;
            CpuTextureSourceDesc desc{{world.GetWorldId(), world.GetLifecycleResetGeneration()},
                                      HashStringId("LateSight"),
                                      TextureFormat::R8G8_UNORM,
                                      sizeof(Params)};
            Lease = Services->Textures().CreateCpuTextureSource(desc);
            const Params parameters{PresentedX, 0, 1, 1};
            EXPECT_TRUE(Lease.Publish(1, 1, std::array{std::byte{17}, std::byte{255}}, Bytes(parameters)));
        }
    } publisher(&services, actor.GetHandle());
    ns::RegisterUserSystem(&publisher);
    ns::StartUserSystems(world);
    ns::TickUserSystems(world, .1f);
    // This is the real early drain: the late publisher has no lease yet.
    services.Textures().ObserveCpuTextureWorld({world.GetWorldId(), world.GetLifecycleResetGeneration()});
    services.Textures().FlushPendingUploads();
    PathfindingECS::NavigationMovementSystem{}.Update(world, .1f);
    ASSERT_GT(actor.Get<Components::WorldTransform>()->matrix[12], publisher.InputX);
    NativePostSimulationSystem bridge(&services);
    bridge.Update(world, .1f);
    EXPECT_GT(publisher.PresentedX, publisher.InputX);
    EXPECT_EQ(actor.Get<Components::MeshRenderer>()->renderLayerMask, 2u);
    const auto binding = services.Textures().GetCpuTextureForView(world.GetWorldId(), HashStringId("LateSight"));
    ASSERT_TRUE(binding.Texture.IsValid());
    EXPECT_EQ(Read(binding), (std::vector{std::byte{17}, std::byte{255}}));
    Params uploaded;
    std::memcpy(&uploaded, binding.Parameters.data(), sizeof(uploaded));
    EXPECT_FLOAT_EQ(uploaded.X, publisher.PresentedX);
    const auto submits = Device.Submits;
    bridge.Update(world, .1f); // another view must not run publication/upload again
    EXPECT_EQ(publisher.Posts, 1u);
    EXPECT_EQ(Device.Submits, submits);
    ns::StopUserSystems(world);
    publisher.Lease.Reset();
    services.Shutdown();
}
} // namespace
#else
TEST(CpuTextureSourceTests, VulkanBackendUnavailable)
{
    GTEST_SKIP() << "Vulkan backend was not compiled; GPU publication checks are unavailable";
}
#endif

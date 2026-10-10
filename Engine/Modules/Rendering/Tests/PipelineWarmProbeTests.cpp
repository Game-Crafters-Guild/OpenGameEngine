// IDevice::TryGetWarmGraphicsPipeline — the non-building cache probe the
// render-graph record path uses so a cold (pipeline, pass format) pair costs a
// skipped draw instead of a backend shader compile on the record thread — and
// IDevice::Request*Pipeline, which hands that build to the device's dispatcher
// and builds each pipeline once.
#include <gtest/gtest.h>

#include "Rendering/Common/Utils.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineCache.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "TestUtils.h"

#include <functional>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{
std::unique_ptr<IDevice> CreateHeadlessDevice(DeviceDesc& outDesc)
{
    // Headless: no swapchain render pass, so dynamic rendering is required for
    // graphics pipeline creation (same setup as PipelineLayoutDedupTests).
    outDesc = DeviceDesc{};
    outDesc.preferredAPI = GraphicsAPI::Vulkan;
    outDesc.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(outDesc);
    if (!dev || !dev->Initialize(outDesc))
        return nullptr;
    return dev;
}

// Interned graphics pipeline over the stock triangle shaders, with one UBO at
// set 0 / binding 0.
bool InternTriangle(IDevice& dev, GraphicsPipelineId& outId)
{
    std::vector<uint8_t> vs = Utils::LoadShaderFile("triangle.vert.spv");
    std::vector<uint8_t> fs = Utils::LoadShaderFile("triangle.frag.spv");
    if (vs.empty() || fs.empty())
        return false;

    DescriptorSetLayoutDesc set0{};
    DescriptorBinding b0{};
    b0.binding = 0;
    b0.type = DescriptorType::UniformBuffer;
    b0.count = 1;
    b0.shaderStages = kShaderStageVertex;
    set0.bindings.push_back(b0);

    GraphicsPipelineDesc gd{};
    gd.DebugName = "WarmProbeTriangle";
    gd.VertexShader = std::make_shared<std::vector<uint8_t>>(std::move(vs));
    gd.PixelShader = std::make_shared<std::vector<uint8_t>>(std::move(fs));
    gd.DescriptorSetLayouts = {dev.InternDescriptorSetLayout(set0)};
    gd.ColorBlend.attachments.resize(1);

    outId = dev.InternGraphicsPipeline(gd);
    return outId.IsValid();
}

// Interned compute pipeline over the stock minimal kernel (one storage buffer at
// set 0 / binding 0).
ComputePipelineId InternMinimalCompute(IDevice& dev)
{
    std::vector<uint8_t> cs = Utils::LoadShaderFile("minimal_test.comp.spv");
    if (cs.empty())
        return {};
    DescriptorSetLayoutDesc set0{};
    DescriptorBinding b0{};
    b0.binding = 0;
    b0.type = DescriptorType::StorageBuffer;
    b0.count = 1;
    b0.shaderStages = kShaderStageCompute;
    set0.bindings.push_back(b0);

    ComputePipelineDesc cd{};
    cd.DebugName = "WarmProbeCompute";
    cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(cs));
    cd.DescriptorSetLayouts = {dev.InternDescriptorSetLayout(set0)};
    return dev.InternComputePipeline(cd);
}

// Stands in for the engine's worker dispatcher: keeps every build job until the
// test runs it, so "not built on the calling thread" is observable. Declared
// after the device, so jobs never run are destroyed while the device lives.
struct HeldBuilds
{
    std::vector<std::function<void()>> Jobs;

    void Install(IDevice& dev)
    {
        dev.SetPipelineBuildDispatcher([this](std::function<void()> job) { Jobs.push_back(std::move(job)); });
    }
    void RunAll()
    {
        std::vector<std::function<void()>> jobs = std::move(Jobs);
        Jobs.clear();
        for (std::function<void()>& job : jobs)
            job();
    }
};

PipelineFormatKey MakeFormatKey(TextureFormat color, uint8_t samples)
{
    PipelineFormatKey fk{};
    fk.ColorCount = 1;
    fk.ColorFormats[0] = color;
    fk.RasterizationSamples = samples;
    return fk;
}
} // namespace

TEST(PipelineWarmProbe, ColdUntilBuiltThenWarmForThatFormatKeyOnly)
{
    DeviceDesc desc{};
    auto dev = CreateHeadlessDevice(desc);
    ASSERT_TRUE(dev) << "headless device unavailable";

    GraphicsPipelineId id{};
    ASSERT_TRUE(InternTriangle(*dev, id));

    const PipelineFormatKey fk = MakeFormatKey(TextureFormat::BGRA8_UNORM, 1);
    const PipelineFormatKey otherFk = MakeFormatKey(TextureFormat::R16G16B16A16_FLOAT, 1);

    // Nothing built yet: the probe must report cold and must NOT build.
    EXPECT_FALSE(dev->TryGetWarmGraphicsPipeline(id, fk).IsValid());
    EXPECT_FALSE(dev->TryGetWarmGraphicsPipeline(id, fk).IsValid())
        << "the probe built a pipeline as a side effect";

    const PipelineHandle built = dev->GetOrCreateGraphicsPipeline(id, fk);
    ASSERT_TRUE(built.IsValid()) << dev->GetLastGraphicsPipelineFailure();

    // Now warm, and the probe returns the very handle the cache holds.
    const PipelineHandle probed = dev->TryGetWarmGraphicsPipeline(id, fk);
    EXPECT_TRUE(probed.IsValid());
    EXPECT_EQ(probed, built);

    // Warmth is per (id, formatKey): a pass with different attachment formats
    // is still cold, which is exactly what makes the record path request a
    // separate warm for it rather than binding the wrong pipeline.
    EXPECT_FALSE(dev->TryGetWarmGraphicsPipeline(id, otherFk).IsValid());
}

TEST(PipelineWarmProbe, PrewarmMakesTheProbeWarm)
{
    DeviceDesc desc{};
    auto dev = CreateHeadlessDevice(desc);
    ASSERT_TRUE(dev) << "headless device unavailable";

    GraphicsPipelineId id{};
    ASSERT_TRUE(InternTriangle(*dev, id));

    const PipelineFormatKey fk = MakeFormatKey(TextureFormat::BGRA8_UNORM, 1);
    EXPECT_FALSE(dev->TryGetWarmGraphicsPipeline(id, fk).IsValid());

    // This is the hand-off the record path performs on a cold probe: the build
    // runs on a worker, and the next frame's probe finds it warm.
    const PipelineFormatKey keys[] = {fk};
    EXPECT_EQ(dev->PrewarmGraphicsPipeline(id, keys), 1u);
    EXPECT_TRUE(dev->TryGetWarmGraphicsPipeline(id, fk).IsValid());

    // Already warm: a second prewarm creates nothing.
    EXPECT_EQ(dev->PrewarmGraphicsPipeline(id, keys), 0u);
}

TEST(PipelineWarmProbe, InvalidIdIsCold)
{
    DeviceDesc desc{};
    auto dev = CreateHeadlessDevice(desc);
    ASSERT_TRUE(dev) << "headless device unavailable";

    EXPECT_FALSE(dev->TryGetWarmGraphicsPipeline(GraphicsPipelineId{},
                                                 MakeFormatKey(TextureFormat::BGRA8_UNORM, 1))
                     .IsValid());
}

TEST(PipelineBuildRequest, RequestedPipelineIsNotBuiltOnTheCallerAndIsWarmOnceItsBuildRuns)
{
    DeviceDesc desc{};
    auto dev = CreateHeadlessDevice(desc);
    ASSERT_TRUE(dev) << "headless device unavailable";
    HeldBuilds held;
    held.Install(*dev);
    const ComputePipelineId id = InternMinimalCompute(*dev);
    ASSERT_TRUE(id.IsValid()) << "minimal_test.comp.spv missing";

    EXPECT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Pending);
    EXPECT_FALSE(dev->TryGetWarmComputePipeline(id).IsValid()) << "the request built on the calling thread";
    EXPECT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Pending);
    EXPECT_EQ(held.Jobs.size(), 1u) << "a second request of a pending pipeline queued a second build";

    held.RunAll();
    EXPECT_TRUE(dev->TryGetWarmComputePipeline(id).IsValid());
    EXPECT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Warm);
    EXPECT_TRUE(held.Jobs.empty());
}

TEST(PipelineBuildRequest, GetOrCreateTakesAQueuedBuildOverAndTheQueuedJobBuildsNothing)
{
    DeviceDesc desc{};
    auto dev = CreateHeadlessDevice(desc);
    ASSERT_TRUE(dev) << "headless device unavailable";
    HeldBuilds held;
    held.Install(*dev);
    const ComputePipelineId id = InternMinimalCompute(*dev);
    ASSERT_TRUE(id.IsValid()) << "minimal_test.comp.spv missing";

    ASSERT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Pending);
    const PipelineHandle built = dev->GetOrCreateComputePipeline(id);
    ASSERT_TRUE(built.IsValid());

    // A second build would replace the cached handle, leaking the first (or
    // freeing one a frame has bound).
    held.RunAll();
    EXPECT_EQ(dev->TryGetWarmComputePipeline(id), built) << "the queued job built the pipeline a second time";
}

TEST(PipelineBuildRequest, FailedBuildIsNotRequestedAgainUntilTheCacheIsInvalidated)
{
    DeviceDesc desc{};
    auto dev = CreateHeadlessDevice(desc);
    ASSERT_TRUE(dev) << "headless device unavailable";
    HeldBuilds held;
    held.Install(*dev);
    const ComputePipelineId id = InternMinimalCompute(*dev);
    ASSERT_TRUE(id.IsValid()) << "minimal_test.comp.spv missing";
    // A cleared cache frees the unpinned desc behind the id: its build cannot succeed.
    dev->GetMutablePipelineCache().Clear();

    ASSERT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Pending);
    held.RunAll();
    EXPECT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Failed);
    EXPECT_TRUE(held.Jobs.empty()) << "a failed pipeline was queued again";

    // Hot reload invalidates the cache: what failed may build now, so it is asked for again.
    dev->GetMutablePipelineCache().Clear();
    EXPECT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Pending);
    EXPECT_EQ(held.Jobs.size(), 1u);
}

TEST(PipelineBuildRequest, BuildDroppedWithoutRunningIsRequestedAgain)
{
    DeviceDesc desc{};
    auto dev = CreateHeadlessDevice(desc);
    ASSERT_TRUE(dev) << "headless device unavailable";
    HeldBuilds held;
    held.Install(*dev);
    const ComputePipelineId id = InternMinimalCompute(*dev);
    ASSERT_TRUE(id.IsValid()) << "minimal_test.comp.spv missing";

    ASSERT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Pending);
    held.Jobs.clear(); // the dispatcher destroyed the job unrun, as a shutting-down pool does

    EXPECT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Pending);
    ASSERT_EQ(held.Jobs.size(), 1u) << "the dropped build left the pipeline pending with nothing building it";
    held.RunAll();
    EXPECT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Warm);
}

TEST(PipelineBuildRequest, BuildRequestedBeforeAnInvalidationDoesNotPublish)
{
    DeviceDesc desc{};
    auto dev = CreateHeadlessDevice(desc);
    ASSERT_TRUE(dev) << "headless device unavailable";
    HeldBuilds held;
    held.Install(*dev);
    const ComputePipelineId id = InternMinimalCompute(*dev);
    ASSERT_TRUE(id.IsValid()) << "minimal_test.comp.spv missing";

    ASSERT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Pending);
    // Device-rebuild invalidation: the desc survives, every concrete pipeline is forgotten.
    dev->GetMutablePipelineCache().ClearConcreteOnly();
    held.RunAll();
    EXPECT_FALSE(dev->TryGetWarmComputePipeline(id).IsValid())
        << "a build from before the invalidation published into the cleared cache";

    EXPECT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Pending);
    held.RunAll();
    EXPECT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Warm);
}

TEST(PipelineBuildRequest, DeviceShutdownCancelsQueuedBuilds)
{
    DeviceDesc desc{};
    auto dev = CreateHeadlessDevice(desc);
    ASSERT_TRUE(dev) << "headless device unavailable";
    HeldBuilds held;
    held.Install(*dev);
    const ComputePipelineId id = InternMinimalCompute(*dev);
    ASSERT_TRUE(id.IsValid()) << "minimal_test.comp.spv missing";

    ASSERT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Pending);
    const uint64_t insertsBefore = dev->GetPipelineCacheStats().Inserts;
    dev->Shutdown();
    // A job the pool runs after the device's objects are gone must not reach the backend.
    held.RunAll();
    EXPECT_EQ(dev->GetPipelineCacheStats().Inserts, insertsBefore);
}

TEST(PipelineBuildRequest, WithoutADispatcherTheBuildRunsInline)
{
    DeviceDesc desc{};
    auto dev = CreateHeadlessDevice(desc);
    ASSERT_TRUE(dev) << "headless device unavailable";
    const ComputePipelineId id = InternMinimalCompute(*dev);
    ASSERT_TRUE(id.IsValid()) << "minimal_test.comp.spv missing";

    EXPECT_EQ(dev->RequestComputePipeline(id), PipelineBuildState::Warm);
    EXPECT_TRUE(dev->TryGetWarmComputePipeline(id).IsValid());
}

#include <gtest/gtest.h>

#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderServices.h"
#include "Mathematics/Matrix4x4.h"
#include "Ocean/OceanRenderFeature.h"
#include "Ocean/OceanRenderNode.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "TestDeviceHelper.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Engine::Renderer::Pipeline;
using namespace GameEngine::Ocean;

namespace
{
constexpr uint32_t kTargetExtent = 64u;
// The camera looks along +Z from 50 m above sea level, so a water body 150 m
// ahead is on screen and one 150 m behind is not.
constexpr float kCameraHeight = 50.0f;
constexpr float kWaterDistance = 150.0f;
constexpr float kWaterHalfExtent = 40.0f;

struct FramePools
{
    RenderGraph::RGResourcePool Persistent;
    RenderGraph::RGTransientPool Transient;
    RenderGraph::RGUploadRing Ring;
    explicit FramePools(IDevice* device) : Persistent(device), Transient(device), Ring(device, 2, 262144) {}
};

TextureDesc TargetDesc(TextureFormat format, uint32_t usage)
{
    TextureDesc desc{};
    desc.width = kTargetExtent;
    desc.height = kTargetExtent;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.sampleCount = 1;
    desc.format = static_cast<uint32_t>(format);
    desc.usage = usage;
    return desc;
}

RenderPipelineBlueprint::Pass MakeOceanPass(const char* id, const char* json)
{
    RenderPipelineBlueprint::Pass pass;
    pass.id = id;
    pass.type = "OceanRender";
    pass.enabled = true;
    pass.perView = true;
    pass.passJson = json;
    return pass;
}

CameraData ForwardCamera()
{
    const Mathematics::Matrix4x4 view = Mathematics::Matrix4x4::LookAt(
        Mathematics::Vector3(0.0f, kCameraHeight, 0.0f), Mathematics::Vector3(0.0f, kCameraHeight, 1.0f),
        Mathematics::Vector3(0.0f, 1.0f, 0.0f));
    const Mathematics::Matrix4x4 projection =
        Mathematics::Matrix4x4::PerspectiveReverseZ(1.0471976f, 16.0f / 9.0f, 0.5f, 600.0f);
    const Mathematics::Matrix4x4 viewProj = projection * view;
    CameraData camera{};
    std::memcpy(camera.view, view.Data(), sizeof(camera.view));
    std::memcpy(camera.proj, projection.Data(), sizeof(camera.proj));
    std::memcpy(camera.viewProj, viewProj.Data(), sizeof(camera.viewProj));
    camera.cameraPos[1] = kCameraHeight;
    return camera;
}

// Declares one frame of the ocean's pre-world and post-world nodes for a single
// view and returns the names of the passes they declared.
std::vector<std::string> DeclareOceanFrame(IDevice* device, float waterCenterZ, bool gameplayQueriesSurface)
{
    std::vector<std::string> names;
    RenderServices rs;
    EXPECT_TRUE(rs.Initialize(device));
    const CameraId cameraId = rs.Views().AllocateCamera("OceanInViewCamera");
    rs.Views().SetCameraData(cameraId, ForwardCamera());
    const ViewId viewId = rs.Views().AllocateView("OceanInViewView", cameraId);
    rs.Views().SetViewRenderLayerMask(viewId, 1u);
    ViewClearConfig clear{};
    clear.clearColor = true;
    clear.clearDepth = true;
    rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);

    auto& feature = rs.EnsureFeature<OceanRenderFeature>();
    EXPECT_TRUE(feature.Initialize(device));
    feature.SetHasOcean(true);
    OceanRenderFeature::WaterBodyBox water{};
    water.CenterZ = waterCenterZ;
    water.HalfX = kWaterHalfExtent;
    water.HalfZ = kWaterHalfExtent;
    feature.SetWaterBodies({water}, true);
    if (feature.IsFoamReady())
        feature.GetFoamSim().SetFrameInputs(feature.GetDisplacementTexture(), feature.GetDisplacementSampler(),
                                            1u, 0.8f, 0.55f, 1.0f, 1.0f / 60.0f, 1.0f, 15.0f, 10.0f, 1.0f,
                                            10.0f, 0.0f, 0.0f);
    if (gameplayQueriesSurface)
        (void)feature.SampleSurface(0.0f, waterCenterZ);

    RenderPipelineNodeRegistry registry;
    EXPECT_TRUE(registry.Register(
        "OceanRender", [] { return std::make_unique<OceanRenderNode>(); }, true));
    RenderPipelineBlueprint blueprint;
    blueprint.pipelineName = "OceanInViewTest";
    blueprint.passes.push_back(MakeOceanPass("Ocean", R"({"id":"Ocean","type":"OceanRender"})"));
    blueprint.passes.push_back(MakeOceanPass(
        "OceanUnderwater", R"({"id":"OceanUnderwater","type":"OceanRender","mode":"underwater"})"));
    RenderPipelineInstance instance(rs, registry);
    instance.SetBlueprint(blueprint);

    FramePools pools(device);
    RenderGraph::RGFrame frame(device, &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);
    const auto color = frame.ImportPersistentTexture(
        "OceanInView.Color",
        TargetDesc(TextureFormat::RGBA8_UNORM,
                   static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource)));
    const auto depth = frame.ImportPersistentTexture(
        "OceanInView.Depth",
        TargetDesc(TextureFormat::D32_FLOAT,
                   static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource)));
    rs.BeginWorldDrawFrame();
    rs.BuildWorldBatchKeys();
    const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{viewId, color, depth, {}}};
    const std::vector<ViewDesc> views(rs.Views().GetViews().begin(), rs.Views().GetViews().end());
    instance.Declare(frame, targets, views);

    for (size_t pass = 0; pass < frame.Graph().PassCount(); ++pass)
        names.emplace_back(frame.Graph().PassName(static_cast<RenderGraph::RGPassId>(pass)));
    device->WaitForIdle();
    rs.Shutdown();
    return names;
}

bool Declared(const std::vector<std::string>& names, const std::string& prefix)
{
    for (const std::string& name : names)
        if (name.rfind(prefix, 0) == 0)
            return true;
    return false;
}

std::string Join(const std::vector<std::string>& names)
{
    std::string joined;
    for (const std::string& name : names)
        joined += name + ' ';
    return joined;
}
} // namespace

// With no water in view the ocean declares no simulation or scene-grab pass; a
// gameplay surface query keeps the wave simulation and its height-field bake.
// The surface draw needs content this harness does not build, so the grab, which
// shares its gate, stands for the per-view passes.
TEST(OceanWaterInViewTest, OceanWorkFollowsTheWaterInViewAndGameplayQueries)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        const std::vector<std::string> ahead = DeclareOceanFrame(device.get(), kWaterDistance, false);
        for (const char* pass : {"OceanFFTGenerate", "OceanHeightFieldBake", "OceanFoamSim", "OceanSceneGrab"})
            EXPECT_TRUE(Declared(ahead, pass)) << pass << " with water in view; declared: " << Join(ahead);

        const std::vector<std::string> behind = DeclareOceanFrame(device.get(), -kWaterDistance, false);
        EXPECT_FALSE(Declared(behind, "Ocean")) << "no water in view; declared: " << Join(behind);

        const std::vector<std::string> queried = DeclareOceanFrame(device.get(), -kWaterDistance, true);
        for (const char* pass : {"OceanFFTGenerate", "OceanHeightFieldBake"})
            EXPECT_TRUE(Declared(queried, pass)) << pass << " for a gameplay query; declared: " << Join(queried);
        for (const char* pass : {"OceanFoamSim", "OceanSceneGrab"})
            EXPECT_FALSE(Declared(queried, pass)) << pass << " with no water in view; declared: " << Join(queried);
    }
    device->Shutdown();
}

// The surface's depth is a function of the surface and FFT spectrum blocks it is
// drawn from, so the ocean claims dynamic depth only while those change and a
// view sees the water: still water under a fixed ocean time lets the
// depth-derived passes settle, and so does a running ocean nobody sees.
TEST(OceanWaterInViewTest, FixedOceanTimeOrNoWaterInViewClaimsNoDynamicDepth)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        EXPECT_TRUE(rs.Initialize(device.get()));
        const CameraId cameraId = rs.Views().AllocateCamera("OceanClaimCamera");
        rs.Views().SetCameraData(cameraId, ForwardCamera());
        const ViewId viewId = rs.Views().AllocateView("OceanClaimView", cameraId);
        rs.Views().SetViewRenderLayerMask(viewId, 1u);
        ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);

        auto& feature = rs.EnsureFeature<OceanRenderFeature>();
        EXPECT_TRUE(feature.Initialize(device.get()));
        feature.SetHasOcean(true);
        OceanRenderFeature::WaterBodyBox water{};
        water.CenterZ = kWaterDistance;
        water.HalfX = kWaterHalfExtent;
        water.HalfZ = kWaterHalfExtent;
        feature.SetWaterBodies({water}, true);

        RenderPipelineNodeRegistry registry;
        EXPECT_TRUE(registry.Register(
            "OceanRender", [] { return std::make_unique<OceanRenderNode>(); }, true));
        RenderPipelineBlueprint blueprint;
        blueprint.pipelineName = "OceanClaimTest";
        blueprint.passes.push_back(MakeOceanPass(
            "OceanUnderwater", R"({"id":"OceanUnderwater","type":"OceanRender","mode":"underwater"})"));
        RenderPipelineInstance instance(rs, registry);
        instance.SetBlueprint(blueprint);

        // One frame as the engine orders it: the extraction publishes the claim
        // for the views declared since its previous update, then the views declare.
        uint32 frameIndex = 0;
        const auto frameAtOceanTime = [&](float oceanTime)
        {
            OceanParamsGPU params = feature.GetParams();
            params.Time = oceanTime;
            feature.SetParams(params);
            feature.PublishDepthClaim();
            const bool claimed = feature.WritesDynamicDepth();
            FramePools pools(device.get());
            RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
            frame.BeginFrame(frameIndex++);
            const auto color = frame.ImportPersistentTexture(
                "OceanClaim.Color",
                TargetDesc(TextureFormat::RGBA8_UNORM,
                           static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource)));
            const auto depth = frame.ImportPersistentTexture(
                "OceanClaim.Depth",
                TargetDesc(TextureFormat::D32_FLOAT,
                           static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource)));
            rs.BeginWorldDrawFrame();
            rs.BuildWorldBatchKeys();
            const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{viewId, color, depth, {}}};
            const std::vector<ViewDesc> views(rs.Views().GetViews().begin(), rs.Views().GetViews().end());
            instance.Declare(frame, targets, views);
            return claimed;
        };

        (void)frameAtOceanTime(1.0f);
        EXPECT_TRUE(frameAtOceanTime(1.5f)) << "running ocean time with water in view";
        EXPECT_FALSE(frameAtOceanTime(1.5f)) << "fixed ocean time: the surface depth is unchanged";
        EXPECT_FALSE(frameAtOceanTime(1.5f)) << "fixed ocean time, a further frame";
        OceanFFTParamsGPU spectrum{};
        spectrum.WindSpeed = 12.0f;
        feature.SetFFTParams(spectrum, true);
        EXPECT_TRUE(frameAtOceanTime(1.5f)) << "a spectrum edit under fixed ocean time";
        EXPECT_FALSE(frameAtOceanTime(1.5f)) << "the edited spectrum, unchanged since";
        EXPECT_TRUE(frameAtOceanTime(2.0f)) << "ocean time running again";

        water.CenterZ = -kWaterDistance;
        feature.SetWaterBodies({water}, true);
        (void)frameAtOceanTime(2.5f);
        EXPECT_FALSE(frameAtOceanTime(3.0f)) << "running ocean time with no water in view";

        device->WaitForIdle();
        rs.Shutdown();
    }
    device->Shutdown();
}

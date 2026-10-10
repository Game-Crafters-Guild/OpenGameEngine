#include <gtest/gtest.h>

#include "Components/Rendering/Light.h"
#include "Engine/Rendering/Pipeline/Nodes/ShadowMapNode.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderServices.h"
#include "Ocean/OceanFrameStamp.h"
#include "Ocean/OceanShadowSim.h"
#include "Ocean/OceanTypes.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "TestDeviceHelper.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <set>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Engine::Renderer::Pipeline;
using namespace GameEngine::Ocean;

namespace
{
// Three frames in flight on every backend: seven frames wrap the device slot twice.
constexpr uint32_t kFramesAcrossTwoSlotWraps = 7u;
constexpr uint32_t kTargetExtent = 64u;
constexpr uint32_t kShadowResolution = 32u;
// An authored shadow covers the field until this graph frame, which lands on a
// device-slot wrap: the first frame whose slot is lower than its predecessor's.
constexpr uint64 kShadowRemovedFrame = 3u;
// With history the field keeps most of the removed shadow for a frame; without it
// the field is fully lit at once.
constexpr float kLitWithoutHistory = 0.5f;

float HalfToFloat(uint16_t half)
{
    const uint32_t sign = (half >> 15) & 1u, exponent = (half >> 10) & 0x1fu, mantissa = half & 0x3ffu;
    const float magnitude = exponent == 0u ? std::ldexp(static_cast<float>(mantissa), -24)
                                           : std::ldexp(static_cast<float>(mantissa | 0x400u),
                                                        static_cast<int>(exponent) - 25);
    return sign ? -magnitude : magnitude;
}

// The hard-shadow visibility at the centre texel of cascade 0 (R16G16F: lit
// hard, lit soft).
float ReadCentreVisibility(IDevice& device, TextureHandle field)
{
    const size_t bytes = size_t(kShadowResolution) * kShadowResolution * 4u;
    const BufferHandle readback = device.CreateReadbackBuffer(bytes);
    auto commands = device.CreateCommandList(IDevice::QueueType::Graphics);
    commands->Begin();
    commands->Barrier(ResourceBarrier::CreateTextureBarrier(field, ResourceState::ShaderResource,
                                                            ResourceState::CopySource));
    commands->CopyTextureToBuffer(field, readback, kShadowResolution, kShadowResolution);
    commands->Barrier(ResourceBarrier::CreateTextureBarrier(field, ResourceState::CopySource,
                                                            ResourceState::ShaderResource));
    commands->End();
    device.ExecuteCommandLists({commands.get()});
    device.WaitForIdle();
    float visibility = -1.0f;
    if (const auto* mapped = static_cast<const uint8_t*>(device.MapBuffer(readback)))
    {
        const size_t centre = (size_t(kShadowResolution / 2u) * kShadowResolution + kShadowResolution / 2u) * 4u;
        uint16_t half = 0;
        std::memcpy(&half, mapped + centre, 2u);
        visibility = HalfToFloat(half);
        device.UnmapBuffer(readback);
    }
    device.DestroyBuffer(readback);
    return visibility;
}

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

// Declares the ocean's temporal shadow accumulation after the engine shadow
// pass, as the ocean node does, and records whether it declared.
struct OceanShadowProbeNode final : IRenderPipelineNode
{
    static OceanShadowSim* s_Sim;
    static bool s_Declared;

    const char* GetTypeName() const override { return "OceanShadowProbe"; }
    bool Initialize(std::string, std::string, std::string*) override { return true; }
    void DeclareForView(ViewDeclare& d) override
    {
        OceanParamsGPU params{};
        s_Declared = s_Sim && s_Sim->DeclareForView(d, params);
    }
};
OceanShadowSim* OceanShadowProbeNode::s_Sim = nullptr;
bool OceanShadowProbeNode::s_Declared = false;

RenderPipelineBlueprint::Pass MakePass(const char* id, const char* type, const char* json)
{
    RenderPipelineBlueprint::Pass pass;
    pass.id = id;
    pass.type = type;
    pass.enabled = true;
    pass.perView = true;
    pass.passJson = json;
    return pass;
}
} // namespace

TEST(OceanFrameStamp, ConsecutiveGraphFramesContinueAcrossDeviceSlotWraps)
{
    OceanFrameStamp stamp;
    EXPECT_FALSE(stamp.PrecedesFrame(0u)) << "a view that never declared has no history";
    for (uint64 frame = 0; frame < kFramesAcrossTwoSlotWraps; ++frame)
    {
        if (frame > 0u)
            EXPECT_TRUE(stamp.PrecedesFrame(frame)) << "frame " << frame;
        EXPECT_FALSE(stamp.IsFrame(frame));
        stamp.Frame = frame;
        EXPECT_TRUE(stamp.IsFrame(frame));
        EXPECT_FALSE(stamp.PrecedesFrame(frame)) << "the frame itself is not its predecessor";
    }
    EXPECT_FALSE(stamp.PrecedesFrame(kFramesAcrossTwoSlotWraps + 1u)) << "a skipped frame breaks history";
}

// The ocean shadow history is identified by the render graph's frame index,
// not by the device slot: across two slot wraps every frame declares, and the
// surface finds this frame's field under the index the graph was begun with.
TEST(OceanShadowSimTest, DeclaresEveryFrameAcrossDeviceSlotWraps)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        const CameraId cameraId = rs.Views().AllocateCamera("OceanShadowCamera");
        CameraData camera{};
        camera.proj[0] = 1.0f;
        camera.proj[5] = 1.0f;
        camera.proj[10] = 0.001f;
        camera.proj[11] = 1.0f;
        camera.proj[14] = 0.1f;
        for (int i = 0; i < 16; i += 5)
        {
            camera.view[i] = 1.0f;
            camera.viewProj[i] = 1.0f;
        }
        rs.Views().SetCameraData(cameraId, camera);
        const ViewId viewId = rs.Views().AllocateView("OceanShadowView", cameraId);
        rs.Views().SetViewRenderLayerMask(viewId, 1u);
        ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);
        auto depthEmit = rs.RegisterDepthEmit(
            [&](DepthEmitContext& ctx, DepthPassType passType)
            {
                if (ctx.ViewId == viewId && passType == DepthPassType::ShadowCascade)
                    ctx.Services->EmitDepthCommand(ctx.ViewId, passType, DrawCommand{});
            });

        RenderPipelineNodeRegistry registry;
        ASSERT_TRUE(registry.Register(
            "ShadowMap", [] { return std::make_unique<Nodes::ShadowMapNode>(); }, true));
        ASSERT_TRUE(registry.Register(
            "OceanShadowProbe", [] { return std::make_unique<OceanShadowProbeNode>(); }, true));
        RenderPipelineBlueprint blueprint;
        blueprint.pipelineName = "OceanShadowTest";
        blueprint.passes.push_back(
            MakePass("CSM", "ShadowMap", R"({"id":"CSM","type":"ShadowMap","buffer":"ShadowData"})"));
        blueprint.passes.push_back(
            MakePass("Ocean", "OceanShadowProbe", R"({"id":"Ocean","type":"OceanShadowProbe"})"));
        RenderPipelineInstance instance(rs, registry);
        instance.SetBlueprint(blueprint);

        OceanShadowSim sim;
        OceanShadowSettings settings{};
        settings.Enabled = true;
        settings.Resolution = kShadowResolution;
        settings.TemporalWeight = 0.95f;
        settings.SimulationFrequency = 60.0f;
        settings.CascadeCount = 1u;
        sim.SetSettings(settings);
        OceanShadowProbeNode::s_Sim = &sim;

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        std::set<uint32_t> slotsSeen;
        for (uint64 index = 0; index < kFramesAcrossTwoSlotWraps; ++index)
        {
            slotsSeen.insert(device->GetFrameIndex());
            frame.BeginFrame(index);
            const auto color = frame.ImportPersistentTexture(
                "OceanShadow.Color",
                TargetDesc(TextureFormat::RGBA8_UNORM, static_cast<uint32_t>(TextureUsage::RenderTarget)));
            const auto depth = frame.ImportPersistentTexture(
                "OceanShadow.Depth",
                TargetDesc(TextureFormat::D32_FLOAT, static_cast<uint32_t>(TextureUsage::DepthStencil |
                                                                           TextureUsage::ShaderResource)));
            rs.BeginWorldDrawFrame();
            rs.BuildWorldBatchKeys();
            ExtractedLight sun{};
            sun.type = GameEngine::Components::LightType::Directional;
            sun.castsShadows = 1;
            sun.castsLight = 1;
            sun.directionWS[1] = -1.0f;
            sun.cascadeCount = 1;
            rs.SubmitLight(0u, sun);

            OceanShadowProbeNode::s_Declared = false;
            const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{viewId, color, depth, {}}};
            const std::vector<ViewDesc> views(rs.Views().GetViews().begin(), rs.Views().GetViews().end());
            // A full shadow over the field for the first frames, then none: from
            // the removal frame on, only history carries the shadow forward.
            std::vector<OceanInputDrawPacket> shadow;
            if (index < kShadowRemovedFrame)
            {
                OceanInputDrawPacket packet{};
                packet.Family = OceanInputFamily::Shadow;
                packet.ExtentX = 1000.0f;
                packet.ExtentZ = 1000.0f;
                packet.Value[0] = 1.0f;
                packet.Value[1] = 1.0f;
                shadow.push_back(packet);
            }
            sim.SetInputs(std::move(shadow));
            instance.Declare(frame, targets, views);
            ASSERT_TRUE(OceanShadowProbeNode::s_Declared) << "graph frame " << index;
            frame.MarkOutput(sim.ImportRG(frame, viewId), RenderGraph::RGImageLayout::ShaderReadOnly);

            OceanShadowSamplingGPU sampling{};
            EXPECT_TRUE(sim.FillSampling(viewId, frame.FrameIndex(), sampling))
                << "graph frame " << index << " on device slot " << device->GetFrameIndex();
            EXPECT_EQ(sampling.Channels[2], 1.0f);

            frame.MarkOutput(color);
            frame.Execute();
            device->FinalizeFrame();
            device->WaitForIdle();
            if (index + 1u == kShadowRemovedFrame)
                EXPECT_LT(ReadCentreVisibility(*device, sim.GetTexture(viewId)), 0.05f)
                    << "the authored shadow must reach the field before the history check means anything";
            if (index == kShadowRemovedFrame)
                EXPECT_LT(ReadCentreVisibility(*device, sim.GetTexture(viewId)), kLitWithoutHistory)
                    << "history must carry the shadow across the device-slot wrap at graph frame " << index;
        }
        EXPECT_EQ(slotsSeen.size(), device->GetFramesInFlight())
            << "the test must drive the device slot through a wrap to mean anything";
        EXPECT_LT(slotsSeen.size(), kFramesAcrossTwoSlotWraps);

        OceanShadowProbeNode::s_Sim = nullptr;
        depthEmit.Reset();
        rs.Shutdown();
    }
    device->Shutdown();
}

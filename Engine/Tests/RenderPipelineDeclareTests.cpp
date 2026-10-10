#include "Core/CpuProfiler.h"
#include "ScopedPipelineFrame.h"
#include "EngineLogCapture.h"
#include <map>
#include "WorldOnlyPipeline.h"
#include "EngineTestShaderSetup.h"
#include <gtest/gtest.h>

#include "Mathematics/MatrixOps.h"
#include "Engine/Rendering/ExposureReadbackFeature.h"
#include "Engine/Rendering/DDGIProbeFeature.h"
#include "Engine/Rendering/SceneAccelerationStructureService.h"
#include "Engine/Rendering/RTShadowMaskService.h"
#include "Engine/Rendering/ScreenSpaceShadows/ScreenSpaceShadowPasses.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "ECSModules/Rendering/Systems/RenderExtractionSystem.h"
#include "ECS/World.h"
#include "ECS/ECSTemplates.h"
#include "Components/Transform.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Rendering/PostProcessEffects/ShadowSettingsEffect.h"
#include "Rendering/Core/GPUCulling.h"
#include "Engine/Rendering/Pipeline/Nodes/AONode.h"
#include "Engine/Rendering/Pipeline/Nodes/AutoExposureNode.h"
#include "Engine/Rendering/Pipeline/Nodes/ShadowMapNode.h"
#include "Engine/Rendering/Pipeline/Nodes/ComputeShaderNode.h"
#include "Engine/Rendering/DepthDrawRecorder.h" // ContributorDepthCommands
#include "Engine/Rendering/Pipeline/Nodes/DepthPrepassNode.h"
#include "Engine/Rendering/Pipeline/Nodes/DepthResolveNode.h"
#include "Engine/Rendering/Pipeline/Nodes/FullscreenShaderNode.h"
#include "Engine/Rendering/Pipeline/Nodes/HZBBuildNode.h"
#include "Engine/Rendering/Pipeline/Nodes/FidelityFXDofNode.h"
#include "Engine/Rendering/Pipeline/Nodes/ScreenSpaceReflectionsNode.h"
#include "Engine/Rendering/Pipeline/Nodes/ViewParamsUploadNode.h"
#include "Engine/Rendering/Pipeline/Nodes/VolumetricFogNode.h"
#include "Engine/Rendering/IEnvironmentSource.h" // complete type for ImageBasedLightingFeature's unique_ptr
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Rendering/Core/ViewParamsLayout.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/VolumetricFogRenderer.h"
#include "TerrainECS/TerrainRenderFeature.h"
#include "TerrainECS/TerrainAtlas.h"
#include "TerrainECS/TerrainGpuBake.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/BarrierMapping.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/RenderGraph/RGBarrier.h"
#include "RGPassQuery.h"
#include "StagedTestPaths.h"
#include "StorageImageLayoutHelper.h"
#include "TestDeviceHelper.h"
#include "Components/Terrain/TerrainGrass.h"
#include "ECS/ComponentFieldRegistry.h"
#include "TerrainECS/TerrainGrassAlpha.h"
#include "Rendering/Materials/MaterialBuildContext.h"
#include "Terrain/Heightfield.h"
#include "TerrainECS/TerrainUploadNode.h"
#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/Systems/TerrainExtractionSystem.h"
#include "TerrainECS/TerrainModifierSystem.h" // ResolvedModifier + PackSurfaceRulesForGpuSplat (splat gate)
// The CPU authorities the device-side surface-rule parity oracle compares the GPU splat against.
#include "TerrainECS/TerrainDefaultSurfaceRules.h"
#include "TerrainECS/TerrainRuleNoise.h"
#include "TerrainECS/TerrainSplatComposite.h"
#include "TerrainECS/TerrainSurfaceRuleEval.h"
#include "TerrainGrass/TerrainGrassRenderFeature.h"
#include "TerrainGrass/TerrainGrassRenderNode.h"
#include "TerrainGrass/RegisterTerrainGrass.h"
#include "CBTTerrainECS/CBTRenderFeature.h"
#include "CBTTerrainECS/CBTTerrainMaterial.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "ScopedCompatShaderProfile.h"
#include "Mathematics/VectorOps.h"
#include "Mathematics/HalfFloat.h"
#include "CBTTerrain/TerrainShadowGrid.h"
#include "CBTTerrainECS/Systems/RegisterCBTSystems.h"
#include "Components/Terrain/Terrain.h"
#include "Terrain/TerrainTypes.h"
#include "Engine/Rendering/Pipeline/Nodes/LightUploadNode.h"
#include "Engine/Rendering/Pipeline/Nodes/RenderScaleUpscaleNode.h"
#include "Engine/Rendering/Pipeline/Nodes/TemporalAANode.h"
#include "SssrShaderSource.h"
#include "Engine/Rendering/Pipeline/Nodes/TemporalFxaaNode.h"
#include "Engine/Rendering/Pipeline/Nodes/ViewParamsUploadNode.h"
#include "Engine/Rendering/Pipeline/Nodes/WorldRenderNode.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/WorldDrawTypes.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Assets/RenderPipelineAsset.h"
#include "Assets/ModelAsset.h" // Mesh/Vertex — the tint-activation test registers a resident mesh
#include "Rendering/Passes/PixelPerfectUpscalePass.h"
#include "Rendering/Passes/SRGBEncodePass.h"
#include "Rendering/Passes/TonemapPass.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
// EngineCore-backed hot-reload fixture (A1.4 S3 L7).
#include "Core/Engine.h"
#include "Core/Application.h"
#include "Scripting/ScriptsConfig.h"
#include "Assets/AssetManager.h"
#include "AssetCore/AssetEvents.h"
#include "AssetCore/GUID.h"
#include "Engine/Rendering/ForwardDrainOrder.h"

#include <algorithm>
#include <array>
#include <optional>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Engine::Renderer::Pipeline;
namespace RGQuery = GameEngine::Testing::RGQuery;

#include "TestDeviceHelper.h"

// Slice-4a pins: the RenderGraph Declare skeleton — blueprint-order node iteration
// with the mask gate, the per-view pre-pass seeding, blackboard
// publish/resolve, GetOutputRG, the frame-identity guard, and the
// BuildFrameGraph(RGFrame&) spine's idempotence.

namespace
{

struct FramePools
{
    RenderGraph::RGResourcePool Persistent;
    RenderGraph::RGTransientPool Transient;
    RenderGraph::RGUploadRing Ring;
    explicit FramePools(Rendering::IDevice* d) : Persistent(d), Transient(d), Ring(d, 2, 262144) {}
};

TextureDesc ColorTargetDesc(uint32_t samples = 1)
{
    TextureDesc d{};
    d.width = 64;
    d.height = 64;
    d.depth = 1;
    d.mipLevels = 1;
    d.arrayLayers = 1;
    d.sampleCount = samples;
    d.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    d.usage = static_cast<uint32_t>(TextureUsage::RenderTarget);
    return d;
}

TextureDesc DepthTargetDesc(uint32_t samples = 1)
{
    TextureDesc d = ColorTargetDesc(samples);
    d.format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
    d.usage = static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource);
    return d;
}

// Records every DeclareForView invocation; optionally publishes/resolves
// blackboard entries so tests can pin the cross-node threading.
struct DeclareProbeNode final : IRenderPipelineNode
{
    struct Call
    {
        std::string NodeId;
        Rendering::ViewId View = 0;
        RenderGraph::RGTexture SeenColor{};
        RenderGraph::RGTexture SeenDepth{};
        RenderGraph::RGTexture SeenResolve{};
        RenderGraph::RGTexture SeenOutputColor{};
        RenderGraph::RGTexture SeenOutputDepth{};
        RenderGraph::RGTexture ResolvedPublished{};
        uint32_t RenderWidth = 0;
        uint32_t OutputWidth = 0;
        std::string PassName;
        bool ResolveFromPipeline = false;
        RenderGraph::RGTexture MaterializedPostTex{}; // "PostTex" (extent.basis=output)
        RenderGraph::RGTexture MaterializedTex{};
        RenderGraph::RGTexture MaterializedTexRepeat{};
        PipelineBufferBindingRG MaterializedBuf{};
        PipelineBufferBindingRG MaterializedBufRepeat{};
        PipelineBufferBindingRG UploadKindBuf{};
    };
    static std::vector<Call>* s_Calls;
    static RenderGraph::RGTexture s_ToPublish; // published by node id "a" under "Probe.Tex"

    std::string m_Id;

    const char* GetTypeName() const override { return "DeclareProbe"; }
    bool Initialize(std::string nodeId, std::string, std::string*) override
    {
        m_Id = std::move(nodeId);
        return true;
    }
    void DeclareForView(ViewDeclare& d) override
    {
        Call c;
        c.NodeId = d.NodeId;
        c.View = d.View.id;
        c.SeenColor = d.ViewColor;
        c.SeenDepth = d.ViewDepth;
        c.SeenResolve = d.ViewResolve;
        c.SeenOutputColor = d.ViewOutputColor;
        c.SeenOutputDepth = d.ViewOutputDepth;
        c.RenderWidth = d.RenderWidth;
        c.OutputWidth = d.OutputWidth;
        c.PassName = d.PassName();
        c.ResolveFromPipeline = d.ResolveFromPipeline;
        if (m_Id == "a" && s_ToPublish.IsValid())
            d.PublishTexture("Probe.Tex", s_ToPublish);
        if (m_Id == "b")
        {
            c.ResolvedPublished = d.ResolveTexture("Probe.Tex");
            c.MaterializedTex = d.ResolveTexture("SceneColor");
            c.MaterializedTexRepeat = d.ResolveTexture("SceneColor");
            c.MaterializedBuf = d.ResolveBuffer("ClusterBuffer");
            c.MaterializedBufRepeat = d.ResolveBuffer("ClusterBuffer");
            c.UploadKindBuf = d.ResolveBuffer("ViewParams");
            c.MaterializedPostTex = d.ResolveTexture("PostTex"); // absent in most blueprints
        }
        if (s_Calls)
            s_Calls->push_back(std::move(c));
    }
};
std::vector<DeclareProbeNode::Call>* DeclareProbeNode::s_Calls = nullptr;
RenderGraph::RGTexture DeclareProbeNode::s_ToPublish{};

// Resolves a named blueprint buffer (materializing it) and declares a compute
// pass writing it — a stand-in for the cluster-cull writer in the seam test.
struct BufferWriterNode final : IRenderPipelineNode
{
    static RenderGraph::RGPass s_Pass;
    static PipelineBufferBindingRG s_Binding;

    std::string m_Id;
    const char* GetTypeName() const override { return "BufferWriter"; }
    bool Initialize(std::string nodeId, std::string, std::string*) override
    {
        m_Id = std::move(nodeId);
        return true;
    }
    void DeclareForView(ViewDeclare& d) override
    {
        s_Binding = d.ResolveBuffer("ClusterBuffer");
        if (!s_Binding.Graph.IsValid())
            return;
        const RenderGraph::RGBuffer target = s_Binding.Graph;
        s_Pass = d.Frame.AddComputePass(
            d.PassName("Write").c_str(), Rendering::PassPhase::kEarlySetup,
            [&](RenderGraph::RGPassBuilder& p) { p.Write(target, RenderGraph::RGBufferWrite::Storage); },
            [](RenderGraph::RGContext&) {});
    }
};
RenderGraph::RGPass BufferWriterNode::s_Pass{};
PipelineBufferBindingRG BufferWriterNode::s_Binding{};

// Counts construction/destruction so a test can observe node re-instantiation
// across a device rebuild (RenderPipelineInstance::OnDeviceRebuilt).
struct LifecycleProbeNode final : IRenderPipelineNode
{
    static int s_Constructed;
    static int s_Live;
    LifecycleProbeNode()
    {
        ++s_Constructed;
        ++s_Live;
    }
    ~LifecycleProbeNode() override { --s_Live; }
    const char* GetTypeName() const override { return "LifecycleProbe"; }
    bool Initialize(std::string, std::string, std::string*) override { return true; }
    void DeclareForView(ViewDeclare&) override {}
};
int LifecycleProbeNode::s_Constructed = 0;
int LifecycleProbeNode::s_Live = 0;

RenderPipelineBlueprint::Pass MakePass(const char* id, bool enabled = true, bool perView = true)
{
    RenderPipelineBlueprint::Pass p;
    p.id = id;
    p.type = "DeclareProbe";
    p.enabled = enabled;
    p.perView = perView;
    p.passJson = "{}";
    return p;
}

Rendering::ViewDesc MakeViewDesc(Rendering::ViewId id, uint32_t mask)
{
    Rendering::ViewDesc v{};
    v.id = id;
    v.renderLayerMask = mask;
    return v;
}

size_t ScheduledIndexOf(const RenderGraph::RGGraph& g, RenderGraph::RGPassId pass)
{
    const auto& order = g.ScheduledOrder();
    for (size_t i = 0; i < order.size(); ++i)
        if (order[i] == pass)
            return i;
    return SIZE_MAX;
}

/// What the two crossfade depth tests below read back off one declaration.
struct CrossfadeDepthDecl
{
    bool PrepassClearsSharedDepth = false;
    bool WorldLoadsSharedDepth    = false; // clear suppression survived
    bool WorldDepthReadOnly       = false;
    bool WorldSamplesDepth        = false; // a pass also reads the shared depth as a sampled texture
};

/// DepthPrepass + WorldRender over one view with the crossfade duration set,
/// declared into one frame. Zero Mask batches, so the prepass path computes
/// read-only depth unless something carves it back out.
///
/// `scheduleCulling` runs the frame spine's culling entry into the same frame
/// first, which is what resolves the crossfade liveness verdict
/// (ScheduleViewCullingDispatches -> UpdateIdleElisionFrameState ->
/// RefreshScatterElisionContext) and therefore separates "the setting is on"
/// from "a tail can hold a live pair". Both arms must now declare the SAME depth
/// access: that verdict feeds elision suppression only, and no longer reaches
/// the declaration at all.
CrossfadeDepthDecl DeclareCrossfadePrepassWorld(Rendering::IDevice* device, bool scheduleCulling,
                                                std::span<const Material* const> submitted = {},
                                                std::optional<ForwardDrawDepth> forwardDraw = std::nullopt)
{
    CrossfadeDepthDecl out{};
    RenderServices rs;
    if (!rs.Initialize(device))
        return out;
    rs.SetLODCrossfadeDuration(0.25f);

    const CameraId camId = rs.Views().AllocateCamera("XfCam");
    CameraData cd{};
    for (int i = 0; i < 16; i += 5)
    {
        cd.view[i] = 1.0f;
        cd.proj[i] = 1.0f;
        cd.viewProj[i] = 1.0f;
    }
    rs.Views().SetCameraData(camId, cd);
    const ViewId viewId = rs.Views().AllocateView("XfView", camId);
    rs.Views().SetViewRenderLayerMask(viewId, 1u);
    Rendering::ViewClearConfig clear{};
    clear.clearColor = true;
    clear.clearColorValue[3] = 1.0f;
    clear.clearDepth = true;
    clear.clearDepthValue = 0.0f;
    rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);

    RenderPipelineNodeRegistry registry;
    if (!registry.Register("DepthPrepass", [] { return std::make_unique<Nodes::DepthPrepassNode>(); },
                           true)
        || !registry.Register("WorldRender",
                              [] { return std::make_unique<Nodes::WorldRenderNode>(); }, true))
    {
        rs.Shutdown();
        return out;
    }

    RenderPipelineBlueprint bp;
    bp.pipelineName = "XfNodeTest";
    {
        RenderPipelineBlueprint::Pass p;
        p.id = "Prepass";
        p.type = "DepthPrepass";
        p.enabled = true;
        p.perView = true;
        p.passJson = R"({"id":"Prepass","type":"DepthPrepass","clearDepthValue":0.0})";
        bp.passes.push_back(p);
    }
    {
        RenderPipelineBlueprint::Pass p;
        p.id = "World";
        p.type = "WorldRender";
        p.enabled = true;
        p.perView = true;
        p.passJson = R"({"id":"World","type":"WorldRender"})";
        bp.passes.push_back(p);
    }
    bp.outputs.push_back({"FinalColor", "View.Resolve"});

    RenderPipelineInstance instance(rs, registry);
    instance.SetBlueprint(bp);

    FramePools pools(device);
    RenderGraph::RGFrame frame(device, &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    RenderGraph::RGTexture color = frame.ImportPersistentTexture("XF.Color", ColorTargetDesc());
    RenderGraph::RGTexture depth = frame.ImportPersistentTexture("XF.Depth", DepthTargetDesc());

    rs.BeginWorldDrawFrame();
    if (!submitted.empty())
    {
        Mesh m{};
        m.Name = "DeclareTriangle";
        Vertex v0{}, v1{}, v2{};
        v0.Position[1] = 1.0f;
        v1.Position[0] = -1.0f;
        v1.Position[1] = -1.0f;
        v2.Position[0] = 1.0f;
        v2.Position[1] = -1.0f;
        v0.Normal[2] = v1.Normal[2] = v2.Normal[2] = 1.0f;
        m.Vertices = {v0, v1, v2};
        m.Indices = {0, 1, 2};
        const Rendering::MeshGPUHandle mesh = rs.GetMeshGPURegistry().RegisterSubmesh({GUID::Generate(), 0}, m);
        std::vector<WorldSubmissionRecord> records;
        for (const Material* material : submitted)
        {
            WorldSubmissionRecord rec{};
            rec.viewId = viewId;
            rec.meshHandle = mesh;
            rec.material = material;
            rec.renderLayerMask = 1u;
            records.push_back(rec);
        }
        rs.SubmitWorldSubmissions(std::span<const WorldSubmissionRecord>(records));
    }
    rs.BuildWorldBatchKeys();
    // One forward contributor draw, emitted the way a producer node emits it: with its prepass head when its
    // depth goes into the prepass.
    if (forwardDraw)
    {
        const DrawCommand head{};
        const bool carriesHead =
            *forwardDraw == ForwardDrawDepth::Prepass || *forwardDraw == ForwardDrawDepth::PrepassNonOccluding;
        rs.EmitForwardCommand(viewId, DrawCommand{}, *forwardDraw, carriesHead ? &head : nullptr);
    }
    // Frame order the spine uses: culling, then the pipeline declaration whose
    // world pass reads the verdict culling resolved.
    if (scheduleCulling)
        rs.ScheduleViewCullingDispatches(frame, 1.0f / 60.0f);

    const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{viewId, color, depth, {}}};
    const std::vector<Rendering::ViewDesc> views(rs.Views().GetViews().begin(),
                                                 rs.Views().GetViews().end());
    instance.Declare(frame, targets, views);

    frame.MarkOutput(color);
    frame.Execute();
    device->WaitForIdle();

    for (const RenderGraph::RGAccessRecord& a : frame.Graph().Accesses())
        out.WorldSamplesDepth |= a.Resource == depth.Id && a.Access == RenderGraph::RGAccess::Sampled;
    for (const auto& rec : frame.Attachments())
    {
        if (rec.Tex != depth.Id || !rec.IsDepth)
            continue;
        if (rec.Ops.Load == RenderGraph::RGLoadOp::Clear)
        {
            out.PrepassClearsSharedDepth = true;
        }
        else
        {
            out.WorldLoadsSharedDepth = true;
            out.WorldDepthReadOnly = rec.ReadOnly;
        }
    }

    rs.Shutdown();
    return out;
}

/// Where the camera prepass, the screen-space passes' depth copy (DepthResolve), the non-occluding
/// prepass and the world pass land in one view's declared order, for one forward draw of `forwardDraw`.
struct PrepassOrderDecl
{
    bool Declared = false; // the resolve's shaders loaded (else the test skips)
    int CameraPrepass = -1;
    int Resolve = -1;
    int NonOccludingPrepass = -1;
    int World = -1;
    bool ResolveReadsViewDepth = false;
    bool NonOccludingLoadsViewDepth = false;
    bool NonOccludingDeclaredOnView = false;
    // The blackboard after the declaration: View.DepthResolved and View.OccluderDepthResolved, each with
    // the pass that writes it (-1 when none does) and whether that pass reads the view depth.
    bool SceneDepthPublished = false;
    bool OccluderDepthPublished = false;
    bool SceneAndOccluderDepthAreOneTexture = false;
    int SceneDepthWriter = -1;
    int OccluderDepthWriter = -1;
    bool SceneDepthWriterReadsViewDepth = false;
    bool WorldReadsSceneDepth = false;
    int ResolvePasses = 0;
    // Every pass that reads View.OccluderDepthResolved where it is a texture of its own (the blueprint runs GTAO
    // and the contact shadows), and whether each of those two reads it.
    std::vector<std::string> OccluderDepthReaders;
    bool GtaoReadsOccluderDepth = false;
    bool ContactShadowsReadOccluderDepth = false;
    std::string AmbientOcclusionPassPrefix; // the AO node's pass names start with this
    std::string ContactShadowPass;          // ScreenSpaceShadowPasses' mask pass for the view
};

// The occlusion passes allowed to read View.OccluderDepthResolved, by exact name: the AO node's GTAO passes
// (AONode.cpp) and the contact shadows' passes (ScreenSpaceShadowPasses.cpp). A pass merely named like one of
// them is not on the list.
bool IsOccluderDepthReader(const PrepassOrderDecl& decl, const std::string& pass)
{
    static constexpr const char* kGtaoPasses[] = {"GTAOPrepare",   "GTAODepthMip1", "GTAODepthMip2", "GTAODepthMip3",
                                                  "GTAODepthMip4", "GTAODepthMip5", "GTAO",          "GTAOBlur",
                                                  "GTAOTemporal",  "GTAOUpsample"};
    for (const char* gtao : kGtaoPasses)
        if (pass == decl.AmbientOcclusionPassPrefix + gtao)
            return true;
    return pass == decl.ContactShadowPass || pass == decl.ContactShadowPass + ".Prepare" ||
           pass == decl.ContactShadowPass + ".Resolve";
}

PrepassOrderDecl DeclarePrepassResolveWorld(Rendering::IDevice* device, bool withResolve,
                                            ForwardDrawDepth forwardDraw)
{
    PrepassOrderDecl out{};
    RenderServices rs;
    if (!rs.Initialize(device))
        return out;
    const CameraId camId = rs.Views().AllocateCamera("NoCam");
    // A perspective camera: the contact shadows decline an orthographic one.
    CameraData cd{};
    for (int i = 0; i < 16; i += 5)
        cd.view[i] = 1.0f;
    const auto projection = Mathematics::MakePerspectiveLH_ZO_ReverseZ(1.0471976f, 1.0f, 0.1f, 200.0f);
    std::memcpy(cd.proj, projection.Data(), sizeof(cd.proj));
    std::memcpy(cd.viewProj, cd.proj, sizeof(cd.proj));
    std::memcpy(cd.viewProjRel, cd.viewProj, sizeof(cd.viewProj));
    rs.Views().SetCameraData(camId, cd);
    const ViewId viewId = rs.Views().AllocateView("NoView", camId);
    rs.Views().SetViewRenderLayerMask(viewId, 1u);
    Rendering::ViewClearConfig clear{};
    clear.clearDepth = true;
    rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);
    PostProcessSettings ambientOcclusion{};
    ambientOcclusion.AOIntensity = 1.0f;
    rs.Views().SetViewPostProcessOverride(viewId, ambientOcclusion);

    RenderPipelineNodeRegistry registry;
    if (!registry.Register("DepthPrepass", [] { return std::make_unique<Nodes::DepthPrepassNode>(); }, true) ||
        !registry.Register("DepthResolve", [] { return std::make_unique<Nodes::DepthResolveNode>(); }, true) ||
        !registry.Register("ViewParamsUpload", [] { return std::make_unique<Nodes::ViewParamsUploadNode>(); }, true) ||
        !registry.Register("AmbientOcclusion", [] { return std::make_unique<Nodes::AONode>(); }, true) ||
        !registry.Register("WorldRender", [] { return std::make_unique<Nodes::WorldRenderNode>(); }, true))
    {
        rs.Shutdown();
        return out;
    }
    RenderPipelineBlueprint bp;
    bp.pipelineName = "NoOccl";
    const auto addPass = [&bp](const char* id, const char* type, const char* json)
    {
        RenderPipelineBlueprint::Pass p;
        p.id = id;
        p.type = type;
        p.enabled = true;
        p.perView = true;
        p.passJson = json;
        bp.passes.push_back(p);
    };
    addPass("Params", "ViewParamsUpload", R"({"buffer":"ViewParams"})");
    addPass("Prepass", "DepthPrepass", R"({"id":"Prepass","type":"DepthPrepass","clearDepthValue":0.0})");
    if (withResolve)
        addPass("DepthResolve", "DepthResolve", R"({"id":"DepthResolve","type":"DepthResolve"})");
    addPass("AO", "AmbientOcclusion", R"({"quality":"medium","temporal":false})");
    addPass("World", "WorldRender", R"({"id":"World","type":"WorldRender","keywords":["Shadows"]})");
    bp.outputs.push_back({"FinalColor", "View.Resolve"});

    RenderPipelineInstance instance(rs, registry);
    instance.SetBlueprint(bp);
    FramePools pools(device);
    RenderGraph::RGFrame frame(device, &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);
    RenderGraph::RGTexture color = frame.ImportPersistentTexture("NO.Color", ColorTargetDesc());
    RenderGraph::RGTexture depth = frame.ImportPersistentTexture("NO.Depth", DepthTargetDesc());

    rs.BeginWorldDrawFrame();
    rs.BuildWorldBatchKeys();
    // One forward draw with its head, emitted the way a producer node emits it.
    const DrawCommand head{};
    rs.EmitForwardCommand(viewId, DrawCommand{}, forwardDraw, &head);
    // A shadow-casting sun and the screen-space contact shadows on, so the world pass declares their mask pass.
    ExtractedLight sun{};
    sun.type = GameEngine::Components::LightType::Directional;
    sun.castsShadows = 1;
    sun.castsLight = 1;
    sun.directionWS[0] = 0.48f; // down and across the view, so the light projects onto the screen
    sun.directionWS[1] = -0.6f;
    sun.directionWS[2] = 0.64f;
    rs.SubmitLight(0u, sun);
    ResolvedShadowSettings contactShadows{};
    contactShadows.HasOverride = true;
    contactShadows.ScreenSpaceShadows = true;
    rs.SetWorldShadowSettings(0u, contactShadows);

    const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{viewId, color, depth, {}}};
    const std::vector<Rendering::ViewDesc> views(rs.Views().GetViews().begin(), rs.Views().GetViews().end());
    instance.Declare(frame, targets, views);

    const auto& g = frame.Graph();
    for (size_t i = 0; i < g.PassCount(); ++i)
    {
        const auto passId = static_cast<RenderGraph::RGPassId>(i);
        const std::string name = g.PassName(passId);
        if (name.rfind("DepthPrepass[", 0) == 0)
        {
            if (name.find("NonOccluding") != std::string::npos)
                out.NonOccludingPrepass = static_cast<int>(i);
            else
                out.CameraPrepass = static_cast<int>(i);
        }
        else if (name.find("DepthResolve") != std::string::npos && ++out.ResolvePasses == 1)
        {
            out.Resolve = static_cast<int>(i);
            out.ResolveReadsViewDepth = g.HasReadAccess(passId, depth.Id);
        }
        else if (out.World < 0 && name.rfind("RenderEntities[", 0) == 0)
        {
            out.World = static_cast<int>(i);
        }
    }
    for (const auto& rec : frame.Attachments())
    {
        if (out.NonOccludingPrepass >= 0 &&
            rec.Pass == static_cast<RenderGraph::RGPassId>(out.NonOccludingPrepass) && rec.Tex == depth.Id &&
            rec.IsDepth)
            out.NonOccludingLoadsViewDepth = rec.Ops.Load == RenderGraph::RGLoadOp::Load && !rec.ReadOnly;
    }
    const auto* pv = rs.Views().FindPerView(viewId);
    out.NonOccludingDeclaredOnView = pv != nullptr && pv->NonOccludingPrepassDeclared;
    if (const auto* resources = instance.FrameResourcesFor(&frame))
    {
        const auto published = [&](const char* name) -> RenderGraph::RGTexture
        {
            const auto it = resources->Textures.find({viewId, name});
            return it != resources->Textures.end() ? it->second : RenderGraph::RGTexture{};
        };
        const RenderGraph::RGTexture scene = published(Names::View::DepthResolved);
        const RenderGraph::RGTexture occluders = published(Names::View::OccluderDepthResolved);
        out.SceneDepthPublished = scene.IsValid() && scene.Id != depth.Id;
        out.OccluderDepthPublished = occluders.IsValid() && occluders.Id != depth.Id;
        out.SceneAndOccluderDepthAreOneTexture = scene.IsValid() && occluders.IsValid() && scene.Id == occluders.Id;
        for (size_t i = 0; i < g.PassCount(); ++i)
        {
            const auto passId = static_cast<RenderGraph::RGPassId>(i);
            if (out.SceneDepthPublished && g.HasWriteAccess(passId, scene.Id))
            {
                out.SceneDepthWriter = static_cast<int>(i);
                out.SceneDepthWriterReadsViewDepth = g.HasReadAccess(passId, depth.Id);
            }
            if (out.OccluderDepthPublished && g.HasWriteAccess(passId, occluders.Id))
                out.OccluderDepthWriter = static_cast<int>(i);
        }
        out.WorldReadsSceneDepth = out.World >= 0 && out.SceneDepthPublished &&
                                   g.HasReadAccess(static_cast<RenderGraph::RGPassId>(out.World), scene.Id);
        const std::string view = std::to_string(static_cast<uint32_t>(viewId));
        out.AmbientOcclusionPassPrefix = "Pipeline.NoOccl.AO.View" + view + ".";
        out.ContactShadowPass = "ScreenSpaceShadows.View" + view;
        for (size_t i = 0; occluders.IsValid() && !out.SceneAndOccluderDepthAreOneTexture && i < g.PassCount(); ++i)
        {
            const auto passId = static_cast<RenderGraph::RGPassId>(i);
            if (!g.HasReadAccess(passId, occluders.Id))
                continue;
            const std::string name = g.PassName(passId);
            out.OccluderDepthReaders.push_back(name);
            out.GtaoReadsOccluderDepth =
                out.GtaoReadsOccluderDepth || name == out.AmbientOcclusionPassPrefix + "GTAOPrepare";
            out.ContactShadowsReadOccluderDepth =
                out.ContactShadowsReadOccluderDepth || name == out.ContactShadowPass + ".Prepare";
        }
    }
    out.Declared = !withResolve || out.Resolve >= 0;
    rs.Shutdown();
    return out;
}

// The level count the min/max pyramid declares for `viewId`: enough levels for
// the widest PCSS kernel of its rendered cascades plus the query margin.
float ExpectedPyramidLevels(const ShadowMapRenderFeature& feature, ViewId viewId)
{
    const CascadeFrameData* fd = feature.GetCachedFrameData(viewId);
    if (!fd)
        return 0.0f;
    const uint32_t cascades = std::min(fd->NumCascades, kMaxShadowCascades);
    return static_cast<float>(ShadowMinMaxPyramid::LevelsForQuery(
        feature.GetConfig().Resolution >> ShadowMinMaxPyramid::kBaseDownshift,
        feature.PcssWidestKernelTexels(viewId, *fd, cascades) +
            ShadowMinMaxPyramid::kQueryMarginTexels));
}

} // namespace

// Q6 slice 4, §8 class 7: after a device rebuild the pipeline instance's nodes
// hold dangling device handles. OnDeviceRebuilt must force a full node
// re-instantiation on the next Declare (old nodes torn down, fresh ones built).
TEST(RenderPipelineDeclareTests, OnDeviceRebuiltReinstantiatesNodes)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        RenderPipelineNodeRegistry registry;
        ASSERT_TRUE(registry.Register(
            "LifecycleProbe", [] { return std::make_unique<LifecycleProbeNode>(); }, true));

        RenderPipelineBlueprint bp;
        bp.pipelineName = "RebuildTest";
        RenderPipelineBlueprint::Pass p;
        p.id = "probe";
        p.type = "LifecycleProbe";
        p.enabled = true;
        p.perView = true;
        p.passJson = "{}";
        bp.passes.push_back(p);

        LifecycleProbeNode::s_Constructed = 0;
        LifecycleProbeNode::s_Live = 0;

        RenderPipelineInstance instance(rs, registry);
        instance.SetBlueprint(bp);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("RB.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("RB.Depth", DepthTargetDesc());
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{7u, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views = {MakeViewDesc(7u, 1u)};

        // First Declare builds the node once.
        instance.Declare(frame, targets, views);
        EXPECT_EQ(LifecycleProbeNode::s_Constructed, 1);
        EXPECT_EQ(LifecycleProbeNode::s_Live, 1);

        // A second Declare without a rebuild reuses the existing instance.
        instance.Declare(frame, targets, views);
        EXPECT_EQ(LifecycleProbeNode::s_Constructed, 1);
        EXPECT_EQ(LifecycleProbeNode::s_Live, 1);

        // OnDeviceRebuilt marks nodes stale; the next Declare tears down the old
        // node (its cached device handles are dead) and builds a fresh one.
        instance.OnDeviceRebuilt();
        instance.Declare(frame, targets, views);
        EXPECT_EQ(LifecycleProbeNode::s_Constructed, 2);
        EXPECT_EQ(LifecycleProbeNode::s_Live, 1);
    }
}

TEST(RenderPipelineDeclareTests, BlueprintOrderMaskGateAndBlackboardThreading)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs; // Declare-path instance tests never touch device state on rs
        RenderPipelineNodeRegistry registry;
        ASSERT_TRUE(registry.Register(
            "DeclareProbe", [] { return std::make_unique<DeclareProbeNode>(); }, true));

        RenderPipelineBlueprint bp;
        bp.pipelineName = "DeclareTest";
        bp.passes.push_back(MakePass("a"));
        bp.passes.push_back(MakePass("disabled", /*enabled=*/false));
        bp.passes.push_back(MakePass("b"));
        bp.outputs.push_back({"FinalColor", "View.Resolve"});

        RenderPipelineInstance instance(rs, registry);
        instance.SetBlueprint(bp);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);

        RenderGraph::RGTexture color = frame.ImportPersistentTexture("DT.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("DT.Depth", DepthTargetDesc());
        RenderGraph::RGTexture published = frame.ImportPersistentTexture("DT.Published", ColorTargetDesc());

        std::vector<DeclareProbeNode::Call> calls;
        DeclareProbeNode::s_Calls = &calls;
        DeclareProbeNode::s_ToPublish = published;

        const std::vector<ViewTargetsRG> targets = {
            ViewTargetsRG{7u, color, depth, {}},
            ViewTargetsRG{9u, color, depth, {}}, // masked off below
        };
        const std::vector<Rendering::ViewDesc> views = {MakeViewDesc(7u, 1u),
                                                        MakeViewDesc(9u, 0u)};

        instance.Declare(frame, targets, views);
        DeclareProbeNode::s_Calls = nullptr;
        DeclareProbeNode::s_ToPublish = {};

        // Blueprint order, active view only: a then b; the disabled node and
        // the masked view declare nothing.
        ASSERT_EQ(calls.size(), 2u);
        EXPECT_EQ(calls[0].NodeId, "a");
        EXPECT_EQ(calls[1].NodeId, "b");
        EXPECT_EQ(calls[0].View, 7u);
        EXPECT_EQ(calls[1].View, 7u);

        // Pre-pass seeding from VALUES (no redirect without a pipeline
        // resolve target): color/depth as passed, resolve falls back to color.
        EXPECT_EQ(calls[0].SeenColor.Id, color.Id);
        EXPECT_EQ(calls[0].SeenDepth.Id, depth.Id);
        EXPECT_EQ(calls[0].SeenResolve.Id, color.Id);
        EXPECT_EQ(calls[0].RenderWidth, 64u);

        // Blackboard threading: a published, the LATER node b resolves it.
        EXPECT_EQ(calls[1].ResolvedPublished.Id, published.Id);

        // Framework pass names match the old BuildForView convention.
        EXPECT_EQ(calls[0].PassName, "Pipeline.DeclareTest.a.View7");

        // Output resolution through the blackboard.
        EXPECT_EQ(instance.GetOutputRG(7u, "FinalColor").Id, color.Id);
        EXPECT_FALSE(instance.GetOutputRG(9u, "FinalColor").IsValid()) << "masked view seeded nothing";

        // Frame-identity guard.
        EXPECT_NE(instance.FrameResourcesFor(&frame), nullptr);
        EXPECT_EQ(instance.FrameResourcesFor(nullptr), nullptr);

        device->WaitForIdle();
    }
    device->Shutdown();
}

TEST(RenderPipelineDeclareTests, MaterializerImportsPoolResourcesSizedToTheViewExtent)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        RenderPipelineNodeRegistry registry;
        ASSERT_TRUE(registry.Register(
            "DeclareProbe", [] { return std::make_unique<DeclareProbeNode>(); }, true));

        RenderPipelineBlueprint bp;
        bp.pipelineName = "MatTest";
        bp.passes.push_back(MakePass("a"));
        bp.passes.push_back(MakePass("b"));
        bp.worldColorResolveTargetRef = "SceneColor";
        bp.resources.push_back(
            {"SceneColor",
             R"({"kind":"texture","scope":"perView","format":"r16g16b16a16_float","extent":{"scale":[1,1]},"usage":["renderTarget","shaderResource"]})"});
        bp.resources.push_back(
            {"ClusterBuffer",
             R"({"kind":"buffer","scope":"perView","memoryUsage":"deviceLocal","usage":["storage"],"size":{"expression":"ceil(renderWidth / 32) * ceil(renderHeight / 32) * 24 * 132"}})"});
        bp.resources.push_back(
            {"ViewParams",
             R"({"kind":"buffer","scope":"perView","memoryUsage":"upload","usage":["constantBuffer"],"size":{"bytes":320}})"});

        RenderPipelineInstance instance(rs, registry);
        instance.SetBlueprint(bp);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);

        RenderGraph::RGTexture color = frame.ImportPersistentTexture("MT.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("MT.Depth", DepthTargetDesc());

        std::vector<DeclareProbeNode::Call> calls;
        DeclareProbeNode::s_Calls = &calls;

        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{7u, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views = {MakeViewDesc(7u, 1u)};
        instance.Declare(frame, targets, views);
        DeclareProbeNode::s_Calls = nullptr;

        ASSERT_EQ(calls.size(), 2u);

        // The pre-pass collapse: single-sample caller color + a pipeline
        // resolve target => the scene renders straight into SceneColor.
        const auto& a = calls[0];
        EXPECT_TRUE(a.ResolveFromPipeline);
        EXPECT_NE(a.SeenColor.Id, color.Id) << "MSAA-off collapse redirects View.Color";
        EXPECT_EQ(a.SeenColor.Id, a.SeenResolve.Id) << "View.Resolve = the pipeline target";
        {
            const auto& sd = frame.Graph().ResourceDesc(a.SeenColor.Id);
            EXPECT_EQ(sd.Width, 64u) << "materialized at the view extent";
            EXPECT_EQ(sd.Height, 64u);
            EXPECT_EQ(sd.Format, static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT));
        }

        // Materializer caching: repeats return the SAME id/binding (never two
        // descs for one pool name within a frame).
        const auto& b = calls[1];
        EXPECT_TRUE(b.MaterializedTex.IsValid());
        EXPECT_EQ(b.MaterializedTex.Id, a.SeenColor.Id) << "SceneColor resolves to ONE import";
        EXPECT_EQ(b.MaterializedTex.Id, b.MaterializedTexRepeat.Id);

        // Buffer expression vs the SAME extent dispatch sizing uses:
        // ceil(64/32)^2 * 24 * 132 = 2*2*24*132.
        ASSERT_TRUE(b.MaterializedBuf.IsValid());
        EXPECT_EQ(b.MaterializedBuf.Size, 2ull * 2ull * 24ull * 132ull);
        EXPECT_TRUE(b.MaterializedBuf.Graph.IsValid()) << "device-local => needs declared edges";
        EXPECT_EQ(b.MaterializedBuf.Buffer, b.MaterializedBufRepeat.Buffer);
        EXPECT_EQ(b.MaterializedBuf.Graph.Id, b.MaterializedBufRepeat.Graph.Id);

        // Upload-kind blueprint buffers are NOT materialized — they dissolve
        // into AllocUpload at their owning node (absent until published).
        EXPECT_FALSE(b.UploadKindBuf.IsValid());

        device->WaitForIdle();
    }
    device->Shutdown();
}

class PipelineResourceParseTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        auto& profiler = Profiling::CpuProfiler::Get();
        m_WasEnabled = profiler.IsEnabled();
#if !GE_ENABLE_CPU_PROFILING
        GTEST_SKIP() << "Resource parse-count instrument requires CPU profiling";
#endif
        profiler.SetEnabled(true);
        profiler.BeginFrame();
        profiler.BeginFrame();
    }

    void TearDown() override
    {
        DeclareProbeNode::s_Calls = nullptr;
        auto& profiler = Profiling::CpuProfiler::Get();
        profiler.BeginFrame();
        profiler.BeginFrame();
        profiler.SetEnabled(m_WasEnabled);
    }

    uint32_t TakeParseCount(const char* label)
    {
        auto& profiler = Profiling::CpuProfiler::Get();
        profiler.BeginFrame();
        std::vector<std::pair<std::string_view, Profiling::CpuProfiler::Sample>> samples;
        profiler.CopyFrameSamples(samples);
        uint32_t count = 0;
        for (const auto& [name, sample] : samples)
            if (name == "RenderPipeline.ParseResource")
                count += sample.count;
        RecordProperty(label, static_cast<int>(count));
        return count;
    }

  private:
    bool m_WasEnabled = false;
};

TEST_F(PipelineResourceParseTest, ParsesOnceAcrossViewsAndFramesAndInvalidatesOnReplacement)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices services;
        RenderPipelineNodeRegistry registry;
        ASSERT_TRUE(registry.Register(
            "DeclareProbe", [] { return std::make_unique<DeclareProbeNode>(); }, true));
        RenderPipelineBlueprint blueprint;
        blueprint.pipelineName = "ParseCache";
        blueprint.passes.push_back(MakePass("b"));
        blueprint.worldColorResolveTargetRef = "SceneColor";
        blueprint.resources = {
            {"SceneColor", R"({"kind":"texture","scope":"perView","format":"r16g16b16a16_float","extent":{"scale":[1,1]}})"},
            {"ClusterBuffer", R"({"kind":"buffer","scope":"perView","memoryUsage":"deviceLocal","usage":["storage"],"size":{"expression":"renderWidth * 16"}})"},
            {"ViewParams", R"({"kind":"buffer","scope":"perView","memoryUsage":"upload","sizeBytes":320})"},
            {"PostTex", R"({"kind":"texture","scope":"perView","extent":{"basis":"output","scale":[0.5,0.5]}})"}};
        RenderPipelineInstance instance(services, registry);
        instance.SetBlueprint(blueprint);
        EXPECT_EQ(TakeParseCount("initial_blueprint_parses"), 4u) << "the parse instrument must be live";
        FramePools pools(device.get());
        bool sceneValid = true;
        uint32_t absoluteWidth = 0;
        TextureFormat expectedFormat = TextureFormat::R16G16B16A16_FLOAT;
        auto declare = [&](uint64_t frameIndex, uint32_t width)
        {
            RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
            frame.BeginFrame(frameIndex);
            std::vector<ViewTargetsRG> targets;
            for (uint32_t index = 0; index < 2; ++index)
            {
                auto description = ColorTargetDesc();
                description.width = width * (index + 1);
                const std::string name = "ParseCache.Caller" + std::to_string(index);
                const auto color = frame.ImportPersistentTexture(name.c_str(), description);
                targets.push_back(ViewTargetsRG{7u + index, color, {}, {}});
            }
            const std::vector<ViewDesc> views{MakeViewDesc(7u, 1u), MakeViewDesc(8u, 1u)};
            std::vector<DeclareProbeNode::Call> calls;
            DeclareProbeNode::s_Calls = &calls;
            instance.Declare(frame, targets, views);
            DeclareProbeNode::s_Calls = nullptr;
            ASSERT_EQ(calls.size(), 2u);
            for (uint32_t index = 0; index < calls.size(); ++index)
            {
                const auto& call = calls[index];
                EXPECT_EQ(call.MaterializedTex.IsValid(), sceneValid);
                if (call.MaterializedTex.IsValid())
                {
                    const auto& description = frame.Graph().ResourceDesc(call.MaterializedTex.Id);
                    EXPECT_EQ(description.Width, absoluteWidth ? absoluteWidth : width * (index + 1));
                    EXPECT_EQ(description.Format, static_cast<uint32_t>(expectedFormat));
                    EXPECT_EQ(call.MaterializedTex.Id, call.MaterializedTexRepeat.Id);
                }
                EXPECT_EQ(call.MaterializedBuf.Size, width * (index + 1) * 16u);
                EXPECT_EQ(call.MaterializedBuf.Graph.Id, call.MaterializedBufRepeat.Graph.Id);
                EXPECT_FALSE(call.UploadKindBuf.IsValid());
                ASSERT_TRUE(call.MaterializedPostTex.IsValid());
                EXPECT_EQ(frame.Graph().ResourceDesc(call.MaterializedPostTex.Id).Width,
                          width * (index + 1) / 2);
            }
            if (sceneValid && absoluteWidth)
                EXPECT_EQ(calls[0].MaterializedTex.Id, calls[1].MaterializedTex.Id) << "replacement is frame-scoped";
            device->WaitForIdle();
        };
        declare(0, 64);
        EXPECT_EQ(TakeParseCount("first_frame_parses"), 0u);
        declare(1, 96);
        EXPECT_EQ(TakeParseCount("steady_frame_parses"), 0u);

        // Same identity, different format, scope and extent; live buffer dimensions still change.
        blueprint.resources[0].resourceJson = R"({"kind":"texture","scope":"frame","format":"rgba8_unorm","extent":{"width":13,"height":17}})";
        instance.SetBlueprint(blueprint);
        EXPECT_EQ(TakeParseCount("replacement_parses"), 4u);
        absoluteWidth = 13;
        expectedFormat = TextureFormat::RGBA8_UNORM;
        declare(2, 128);
        EXPECT_EQ(TakeParseCount("replacement_frame_parses"), 0u);

        blueprint.resources[0].resourceJson = "{";
        instance.SetBlueprint(blueprint);
        EXPECT_EQ(TakeParseCount("invalid_blueprint_parses"), 4u);
        sceneValid = false;
        declare(3, 64);
        EXPECT_EQ(TakeParseCount("invalid_frame_parses"), 0u) << "a refused description is cached too";
        blueprint.resources.erase(blueprint.resources.begin());
        instance.SetBlueprint(blueprint);
        EXPECT_EQ(TakeParseCount("removed_blueprint_parses"), 3u);
        declare(4, 96);
        EXPECT_EQ(TakeParseCount("removed_frame_parses"), 0u);
        blueprint.resources.push_back({"SceneColor", R"({"kind":"texture","scope":"frame","extent":{"width":19,"height":23}})"});
        instance.SetBlueprint(blueprint);
        EXPECT_EQ(TakeParseCount("readded_blueprint_parses"), 4u);
        sceneValid = true;
        absoluteWidth = 19;
        declare(5, 64);
        EXPECT_EQ(TakeParseCount("readded_frame_parses"), 0u);
    }
    device->Shutdown();
}


// The internal-resolution split is NOT gated on temporal AA. A view with no
// ViewAntiAliasing state at all (AA mode Off — the editor default) splits on its
// per-view render scale exactly like a TAA view does: the crossing back to the
// display extent is a different node, but the pre-pass shape is identical. With
// the old TAA gate an AA-Off view silently ignored render scale and no upscale
// node existed anywhere in the graph.
TEST(RenderPipelineDeclareTests, PrePassSplitsWithoutTemporalAA)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        RenderPipelineNodeRegistry registry;
        ASSERT_TRUE(registry.Register(
            "DeclareProbe", [] { return std::make_unique<DeclareProbeNode>(); }, true));

        ASSERT_TRUE(registry.Register(
            "RenderScaleUpscale",
            [] { return std::make_unique<Nodes::RenderScaleUpscaleNode>(); }, true));

        RenderPipelineBlueprint bp;
        bp.pipelineName = "SplitNoTaaTest";
        bp.passes.push_back(MakePass("a"));
        bp.passes.push_back(MakePass("b"));
        {
            // The split requires a crossing to exist; this node's presence is
            // what makes the view eligible (it declines on its own terms here).
            RenderPipelineBlueprint::Pass cross;
            cross.id = "RenderScaleUpscale";
            cross.type = "RenderScaleUpscale";
            cross.enabled = true;
            cross.perView = true;
            cross.passJson =
                R"({"id":"RenderScaleUpscale","type":"RenderScaleUpscale","input":"SceneColor","output":"PostTex"})";
            bp.passes.push_back(cross);
        }
        bp.worldColorResolveTargetRef = "SceneColor";
        bp.resources.push_back({"SceneColor", R"({"kind":"texture","scope":"perView","format":"r16g16b16a16_float","extent":{"scale":[1,1]},"usage":["renderTarget","shaderResource"]})"});
        bp.resources.push_back({"PostTex", R"({"kind":"texture","scope":"perView","format":"r16g16b16a16_float","extent":{"basis":"output"},"usage":["renderTarget","shaderResource"]})"});

        RenderPipelineInstance instance(rs, registry);
        instance.SetBlueprint(bp);

        // Per-view scale, and deliberately no SetViewAntiAliasing in this test.
        rs.Views().SetViewRenderScale(7u, 0.76f);
        ASSERT_EQ(rs.Views().FindViewAntiAliasing(7u), nullptr) << "the view must NOT run TAA";

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);

        RenderGraph::RGTexture color = frame.ImportPersistentTexture("SN.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("SN.Depth", DepthTargetDesc());

        std::vector<DeclareProbeNode::Call> calls;
        DeclareProbeNode::s_Calls = &calls;
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{7u, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views = {MakeViewDesc(7u, 1u)};
        instance.Declare(frame, targets, views);
        DeclareProbeNode::s_Calls = nullptr;
        ASSERT_EQ(calls.size(), 2u);

        // 64 * 0.76 = 48.64 -> floor 48, even-snapped 48 (the TAAU arithmetic).
        const auto& a = calls[0];
        EXPECT_EQ(a.RenderWidth, 48u) << "internal extent = even-snapped scale * display";
        EXPECT_EQ(a.OutputWidth, 64u) << "display extent preserved";
        EXPECT_NE(a.SeenDepth.Id, depth.Id) << "raster depth redirected to the internal depth";
        EXPECT_EQ(frame.Graph().ResourceDesc(a.SeenDepth.Id).Width, 48u);
        EXPECT_EQ(frame.Graph().ResourceDesc(a.SeenColor.Id).Width, 48u)
            << "SceneColor (render basis) materializes internal";
        EXPECT_EQ(a.SeenOutputColor.Id, color.Id) << "caller color preserved for the crossing";
        EXPECT_EQ(a.SeenOutputDepth.Id, depth.Id) << "caller depth preserved for the upsample";

        const auto& b = calls[1];
        ASSERT_TRUE(b.MaterializedPostTex.IsValid());
        EXPECT_EQ(frame.Graph().ResourceDesc(b.MaterializedPostTex.Id).Width, 64u)
            << "extent.basis=output materializes at the display extent";

        device->WaitForIdle();
    }
    device->Shutdown();
}

// A blueprint with nowhere to cross must NOT split. Older pipeline assets
// predate the crossing node; splitting them would leave the internal-to-display
// resample to happen implicitly at whichever output-basis target the post chain
// binds first, which is the one thing the whole slice exists to prevent. Such a
// blueprint ignores render scale entirely — exactly its pre-split behaviour.
TEST(RenderPipelineDeclareTests, SplitRequiresACrossingToExist)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        RenderPipelineNodeRegistry registry;
        ASSERT_TRUE(registry.Register(
            "DeclareProbe", [] { return std::make_unique<DeclareProbeNode>(); }, true));

        RenderPipelineBlueprint bp;
        bp.pipelineName = "NoCrossingTest";
        bp.passes.push_back(MakePass("a"));
        bp.worldColorResolveTargetRef = "SceneColor";
        bp.resources.push_back({"SceneColor", R"({"kind":"texture","scope":"perView","format":"r16g16b16a16_float","extent":{"scale":[1,1]},"usage":["renderTarget","shaderResource"]})"});
        bp.resources.push_back({"PostTex", R"({"kind":"texture","scope":"perView","format":"r16g16b16a16_float","extent":{"basis":"output"},"usage":["renderTarget","shaderResource"]})"});

        RenderPipelineInstance instance(rs, registry);
        instance.SetBlueprint(bp);

        rs.Views().SetViewRenderScale(7u, 0.5f);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("NC.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("NC.Depth", DepthTargetDesc());

        std::vector<DeclareProbeNode::Call> calls;
        DeclareProbeNode::s_Calls = &calls;
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{7u, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views = {MakeViewDesc(7u, 1u)};
        instance.Declare(frame, targets, views);
        DeclareProbeNode::s_Calls = nullptr;
        ASSERT_EQ(calls.size(), 1u);

        const auto& a = calls[0];
        EXPECT_EQ(a.RenderWidth, 64u) << "no crossing node and no TAA => no split at any scale";
        EXPECT_EQ(a.OutputWidth, 64u);
        EXPECT_EQ(a.SeenDepth.Id, depth.Id) << "raster depth NOT redirected";
        EXPECT_FALSE(a.SeenOutputColor.IsValid()) << "no split table entries";
        EXPECT_FALSE(a.SeenOutputDepth.IsValid());

        // The same view becomes eligible the moment a crossing exists — here the
        // TAA resolve, without touching the blueprint.
        rs.Views().SetViewAntiAliasing(7u, true, AntiAliasingMode::TAA, 8u);
        std::vector<DeclareProbeNode::Call> taaCalls;
        DeclareProbeNode::s_Calls = &taaCalls;
        instance.Declare(frame, targets, views);
        DeclareProbeNode::s_Calls = nullptr;
        ASSERT_EQ(taaCalls.size(), 1u);
        EXPECT_EQ(taaCalls[0].RenderWidth, 32u)
            << "the TAA resolve is a crossing, so the split applies";

        device->WaitForIdle();
    }
    device->Shutdown();
}

// Per-view scale beats the engine default in both directions: a view that opts
// out at exactly 1.0 stays native while the default splits everything else, and
// a view that opts in splits while the default is native. Without this the
// editor could not scale one pane and leave its siblings alone.
TEST(RenderPipelineDeclareTests, PerViewRenderScaleOverridesEngineDefault)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        RenderPipelineNodeRegistry registry;
        ASSERT_TRUE(registry.Register(
            "DeclareProbe", [] { return std::make_unique<DeclareProbeNode>(); }, true));

        ASSERT_TRUE(registry.Register(
            "RenderScaleUpscale",
            [] { return std::make_unique<Nodes::RenderScaleUpscaleNode>(); }, true));

        RenderPipelineBlueprint bp;
        bp.pipelineName = "PerViewScaleTest";
        bp.passes.push_back(MakePass("a"));
        {
            // The split requires a crossing to exist; this node's presence is
            // what makes the view eligible (it declines on its own terms here).
            RenderPipelineBlueprint::Pass cross;
            cross.id = "RenderScaleUpscale";
            cross.type = "RenderScaleUpscale";
            cross.enabled = true;
            cross.perView = true;
            cross.passJson =
                R"({"id":"RenderScaleUpscale","type":"RenderScaleUpscale","input":"SceneColor","output":"SceneColor"})";
            bp.passes.push_back(cross);
        }
        bp.worldColorResolveTargetRef = "SceneColor";
        bp.resources.push_back({"SceneColor", R"({"kind":"texture","scope":"perView","format":"r16g16b16a16_float","extent":{"scale":[1,1]},"usage":["renderTarget","shaderResource"]})"});

        RenderPipelineInstance instance(rs, registry);
        instance.SetBlueprint(bp);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        RenderGraph::RGTexture colorA =
            frame.ImportPersistentTexture("PV.ColorA", ColorTargetDesc());
        RenderGraph::RGTexture depthA =
            frame.ImportPersistentTexture("PV.DepthA", DepthTargetDesc());
        RenderGraph::RGTexture colorB =
            frame.ImportPersistentTexture("PV.ColorB", ColorTargetDesc());
        RenderGraph::RGTexture depthB =
            frame.ImportPersistentTexture("PV.DepthB", DepthTargetDesc());
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{7u, colorA, depthA, {}},
                                                   ViewTargetsRG{8u, colorB, depthB, {}}};
        const std::vector<Rendering::ViewDesc> views = {MakeViewDesc(7u, 1u), MakeViewDesc(8u, 1u)};

        uint32_t wA = 0;
        uint32_t wB = 0;
        auto declareAndWidths = [&]()
        {
            std::vector<DeclareProbeNode::Call> calls;
            DeclareProbeNode::s_Calls = &calls;
            instance.Declare(frame, targets, views);
            DeclareProbeNode::s_Calls = nullptr;
            ASSERT_EQ(calls.size(), 2u);
            wA = calls[0].View == 7u ? calls[0].RenderWidth : calls[1].RenderWidth;
            wB = calls[0].View == 7u ? calls[1].RenderWidth : calls[0].RenderWidth;
        };

        // Default native, view 7 opts in.
        rs.SetDefaultRenderScale(1.0f);
        rs.Views().SetViewRenderScale(7u, 0.5f);
        declareAndWidths();
        EXPECT_EQ(wA, 32u) << "the per-view override splits view 7";
        EXPECT_EQ(wB, 64u) << "view 8 follows the native default";

        // Default splits, view 7 opts OUT at exactly 1.0.
        rs.SetDefaultRenderScale(0.5f);
        rs.Views().SetViewRenderScale(7u, 1.0f);
        declareAndWidths();
        EXPECT_EQ(wA, 64u) << "an explicit 1.0 override pins the view native";
        EXPECT_EQ(wB, 32u) << "view 8 follows the split default";

        // Clearing the override hands the view back to the default.
        rs.Views().SetViewRenderScale(7u, std::nullopt);
        declareAndWidths();
        EXPECT_EQ(wA, 32u) << "a cleared override follows the default again";

        device->WaitForIdle();
    }
    device->Shutdown();
}

// TAAU: with a TAA view at render scale < 1 the pre-pass splits internal vs
// display extents — View.Depth redirects to a scaled internal depth,
// SceneColor materializes internal, "extent.basis":"output" resources
// materialize at the display extent, and the caller's targets survive as
// View.OutputColor/OutputDepth. At scale 1.0 the pre-pass is exactly the
// native path (the byte-neutrality shape).
TEST(RenderPipelineDeclareTests, TaauPrePassSplitsInternalAndOutputExtents)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        RenderPipelineNodeRegistry registry;
        ASSERT_TRUE(registry.Register(
            "DeclareProbe", [] { return std::make_unique<DeclareProbeNode>(); }, true));

        RenderPipelineBlueprint bp;
        bp.pipelineName = "TaauTest";
        bp.passes.push_back(MakePass("a"));
        bp.passes.push_back(MakePass("b"));
        bp.worldColorResolveTargetRef = "SceneColor";
        bp.resources.push_back(
            {"SceneColor",
             R"({"kind":"texture","scope":"perView","format":"r16g16b16a16_float","extent":{"scale":[1,1]},"usage":["renderTarget","shaderResource"]})"});
        bp.resources.push_back(
            {"PostTex",
             R"({"kind":"texture","scope":"perView","format":"r16g16b16a16_float","extent":{"basis":"output"},"usage":["renderTarget","shaderResource"]})"});

        RenderPipelineInstance instance(rs, registry);
        instance.SetBlueprint(bp);

        rs.SetDefaultRenderScale(0.76f);
        rs.Views().SetViewAntiAliasing(7u, true, AntiAliasingMode::TAA, 8u);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);

        RenderGraph::RGTexture color = frame.ImportPersistentTexture("TU.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("TU.Depth", DepthTargetDesc());

        std::vector<DeclareProbeNode::Call> calls;
        DeclareProbeNode::s_Calls = &calls;
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{7u, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views = {MakeViewDesc(7u, 1u)};
        instance.Declare(frame, targets, views);
        DeclareProbeNode::s_Calls = nullptr;
        ASSERT_EQ(calls.size(), 2u);

        // 64 * 0.76 = 48.64 -> floor 48, even-snapped 48.
        const auto& a = calls[0];
        EXPECT_EQ(a.RenderWidth, 48u) << "internal extent = even-snapped scale * display";
        EXPECT_EQ(a.OutputWidth, 64u) << "display extent preserved";

        EXPECT_NE(a.SeenDepth.Id, depth.Id) << "raster depth redirected to the internal depth";
        {
            const auto& dd = frame.Graph().ResourceDesc(a.SeenDepth.Id);
            EXPECT_EQ(dd.Width, 48u);
            EXPECT_EQ(dd.Height, 48u);
            EXPECT_EQ(dd.Format, static_cast<uint32_t>(TextureFormat::D32_FLOAT));
        }
        {
            const auto& sd = frame.Graph().ResourceDesc(a.SeenColor.Id);
            EXPECT_EQ(sd.Width, 48u) << "SceneColor (render basis) materializes internal";
        }
        EXPECT_EQ(a.SeenOutputColor.Id, color.Id) << "caller color preserved for the upscale point";
        EXPECT_EQ(a.SeenOutputDepth.Id, depth.Id) << "caller depth preserved for the depth upsample";

        const auto& b = calls[1];
        ASSERT_TRUE(b.MaterializedPostTex.IsValid());
        {
            const auto& pd = frame.Graph().ResourceDesc(b.MaterializedPostTex.Id);
            EXPECT_EQ(pd.Width, 64u) << "extent.basis=output materializes at display extent";
            EXPECT_EQ(pd.Height, 64u);
        }

        // Scale 1.0: the native path — no redirect, no output entries, one extent.
        rs.SetDefaultRenderScale(1.0f);
        std::vector<DeclareProbeNode::Call> nativeCalls;
        DeclareProbeNode::s_Calls = &nativeCalls;
        instance.Declare(frame, targets, views);
        DeclareProbeNode::s_Calls = nullptr;
        ASSERT_EQ(nativeCalls.size(), 2u);
        const auto& n = nativeCalls[0];
        EXPECT_EQ(n.RenderWidth, 64u);
        EXPECT_EQ(n.OutputWidth, 64u);
        EXPECT_EQ(n.SeenDepth.Id, depth.Id) << "no redirect at scale 1.0";
        EXPECT_FALSE(n.SeenOutputColor.IsValid()) << "no TAAU table entries at scale 1.0";
        EXPECT_FALSE(n.SeenOutputDepth.IsValid());

        device->WaitForIdle();
    }
    device->Shutdown();
}

// The ge_mipBiasParams[0] derivation as a pure function: log2(render/output)
// on upscale, exactly 0.0 otherwise, floored at -2.0. The floor IS reachable
// through Declare: SetDefaultRenderScale clamps to [kMinRenderScale, 1.0] with
// kMinRenderScale = 0.25, and the internal extent is an even-snapped floor of
// output*scale, so the realised ratio can sit below 0.25 (width 1366 at scale
// 0.25 -> internal 340 -> raw bias -2.0064) and the clamp engages — which is
// why the floor is pinned here at the unit level against the production
// function.
TEST(RenderPipelineDeclareTests, TaauMipBiasClampsAndZeroesOutsideUpscale)
{
    using Nodes::ViewParamsUploadNode;
    EXPECT_EQ(ViewParamsUploadNode::ComputeTaauMipBias(64u, 64u), 0.0f) << "scale 1.0 neutrality";
    EXPECT_EQ(ViewParamsUploadNode::ComputeTaauMipBias(64u, 48u), 0.0f) << "render > output is not upscaling";
    EXPECT_EQ(ViewParamsUploadNode::ComputeTaauMipBias(0u, 64u), 0.0f);
    EXPECT_EQ(ViewParamsUploadNode::ComputeTaauMipBias(64u, 0u), 0.0f);
    EXPECT_FLOAT_EQ(ViewParamsUploadNode::ComputeTaauMipBias(48u, 64u),
                    std::log2(48.0f / 64.0f));
    EXPECT_FLOAT_EQ(ViewParamsUploadNode::ComputeTaauMipBias(17u, 64u),
                    std::log2(17.0f / 64.0f)) << "just inside the floor stays exact";
    EXPECT_EQ(ViewParamsUploadNode::ComputeTaauMipBias(16u, 64u), -2.0f) << "log2(0.25) sits ON the floor";
    EXPECT_EQ(ViewParamsUploadNode::ComputeTaauMipBias(4u, 64u), -2.0f) << "raw -4.0 clamps to -2.0";
    EXPECT_EQ(ViewParamsUploadNode::ComputeTaauMipBias(1u, 4096u), -2.0f);
}

// TAA history validity as a pure function. The whole point of the predicate is
// what it does NOT gate on: the internal (raster) extent. History and output
// textures are created at the display extent, so only that pair decides whether
// the persistent pool kept the physical — and nothing the resolve reads across
// frames lives in internal space (history is display-UV Catmull-Rom, motion is
// a dimensionless UV delta, stored disocclusion depth is NDC). Without this,
// every dynamic-resolution step would throw away valid history and show a
// one-frame un-accumulated bilinear frame.
TEST(RenderPipelineDeclareTests, TaaHistoryValidityIgnoresInternalExtent)
{
    using Nodes::TemporalAANode;
    constexpr uint64_t kPrev = 40u;
    constexpr uint64_t kNow = 41u;

    constexpr bool kResident = false; // pool returned the physical we wrote
    constexpr bool kFresh = true;     // pool returned a recycled/undefined physical

    EXPECT_TRUE(TemporalAANode::ComputeHistoryValid(kPrev, kNow, 1920u, 1080u, 1920u, 1080u, true,
                                                    kResident))
        << "steady state";

    // The DRS case: display extent pinned, internal extent moving underneath.
    // The history texture's desc never changed, so the history is still there.
    EXPECT_TRUE(TemporalAANode::ComputeHistoryValid(kPrev, kNow, 1920u, 1080u, 1920u, 1080u, true,
                                                    kResident))
        << "render-scale step keeps the display extent, so history survives";

    // A display-extent change IS a history realloc (uninitialized pool memory).
    EXPECT_FALSE(TemporalAANode::ComputeHistoryValid(kPrev, kNow, 1920u, 1080u, 1600u, 1080u, true,
                                                     kResident))
        << "viewport resize must invalidate — the history physical was reallocated";
    EXPECT_FALSE(TemporalAANode::ComputeHistoryValid(kPrev, kNow, 1920u, 1080u, 1920u, 900u, true,
                                                     kResident));

    // Outside TAAU the display extent IS the render extent, so the same rule
    // still catches a native-path resize.
    EXPECT_FALSE(
        TemporalAANode::ComputeHistoryValid(kPrev, kNow, 800u, 600u, 640u, 480u, true, kResident));

    // A frame GAP is no longer staleness. The graph frame index is a per-window
    // stream counter; an OnDemand view that lapses (hidden tab, collapsed pane)
    // resumes with an arbitrary jump in it, and the old lastWrittenFrame + 1
    // rule discarded valid surviving history on every such resume. Residency is
    // the pool's answer now, not the frame counter's.
    EXPECT_TRUE(TemporalAANode::ComputeHistoryValid(kPrev, kNow + 1u, 1920u, 1080u, 1920u, 1080u,
                                                    true, kResident))
        << "a lapsed view's frame gap must NOT invalidate history";

    // ...and the condition the frame counter could never see: the pool aged the
    // entry out (or a device rebuild dropped it) and handed back a recycled
    // physical under the same name and desc.
    EXPECT_FALSE(TemporalAANode::ComputeHistoryValid(kPrev, kNow, 1920u, 1080u, 1920u, 1080u, true,
                                                     kFresh))
        << "a recycled physical must invalidate even at a steady extent and frame";

    EXPECT_FALSE(TemporalAANode::ComputeHistoryValid(kPrev, kNow, 1920u, 1080u, 1920u, 1080u, false,
                                                     kResident))
        << "no previous camera means no reprojection";
    EXPECT_FALSE(
        TemporalAANode::ComputeHistoryValid(~0ull, 0u, 0u, 0u, 1920u, 1080u, true, kResident))
        << "first frame for a view";
}

// A view that draws content moving without an instance or an epoch (moving particles) is
// never certified stationary: its history is clipped (+1) the frame it is reported and the
// frame after, so the last moving copy is cleared, then the still view is certified again (-1).
// The report is per view: another view of the same world keeps the certified path.
TEST(RenderPipelineDeclareTests, TaaCertificationIsRevokedForAViewWithUnversionedMotion)
{
    using Nodes::TemporalAANode;
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    const CameraId camera = rs.Views().AllocateCamera("TaaReportCamera");
    const ViewId reported = rs.Views().AllocateView("TaaReportedView", camera);
    const ViewId other = rs.Views().AllocateView("TaaOtherView", camera);
    constexpr uint64_t kWorld = 7u;
    // A still camera with unchanged epochs and no instance movers.
    const auto mode = [&rs](ViewId view)
    {
        return TemporalAANode::HistoryClipMode(TemporalAANode::ViewHasMovers(rs, view, kWorld),
                                               false, true);
    };

    rs.BeginWorldDrawFrame();
    EXPECT_EQ(mode(reported), -1.0f) << "nothing moves: the still view is certified";

    rs.NotifyUnversionedMotion(reported);
    EXPECT_EQ(mode(reported), 1.0f) << "moving particles in view: history is clipped";
    EXPECT_EQ(mode(other), -1.0f) << "the report is per view";

    rs.BeginWorldDrawFrame();
    EXPECT_EQ(mode(reported), 1.0f) << "the frame after the last report still clips";

    rs.BeginWorldDrawFrame();
    EXPECT_EQ(mode(reported), -1.0f) << "two frames without motion: certified again";

    rs.Shutdown();
    device->Shutdown();
}

// The two-frame FXAA keeps history across idle gaps by design (no frame
// adjacency), so residency is the ONLY thing standing between it and a
// recycled physical: the pool hands one back under the same name and desc on
// age-out, device rebuild, and the usage-widening realloc a capture of the
// history triggers. A fresh physical must invalidate even in steady state.
TEST(RenderPipelineDeclareTests, TemporalFxaaHistoryValidityRejectsAFreshPhysical)
{
    using Nodes::TemporalFxaaNode;
    constexpr uint64_t kSecondFrame = 1u;
    constexpr bool kResident = false;
    constexpr bool kFresh = true;

    EXPECT_TRUE(TemporalFxaaNode::ComputeHistoryValid(kSecondFrame, 1920u, 1080u, 1920u, 1080u,
                                                      true, kResident))
        << "steady state";
    EXPECT_FALSE(TemporalFxaaNode::ComputeHistoryValid(kSecondFrame, 1920u, 1080u, 1920u, 1080u,
                                                       true, kFresh))
        << "a recycled physical must invalidate at a steady extent";
    EXPECT_FALSE(TemporalFxaaNode::ComputeHistoryValid(kSecondFrame, 1920u, 1080u, 1600u, 1080u,
                                                       true, kResident))
        << "resize";
    EXPECT_FALSE(TemporalFxaaNode::ComputeHistoryValid(kSecondFrame, 1920u, 1080u, 1920u, 1080u,
                                                       false, kResident))
        << "no previous camera means no reprojection";
    EXPECT_FALSE(
        TemporalFxaaNode::ComputeHistoryValid(0u, 0u, 0u, 1920u, 1080u, true, kResident))
        << "first frame for a view";
}

// A temporal fullscreen stage's published output IS its history-write buffer,
// so when it is the last live LDR stage the output policy widens that history
// (TransferSrc for captures) and the next frame reads the other parity fresh.
// Its own continuity rule (frame-adjacent, same extent, same world) cannot
// see that; the pool's answer must.
TEST(RenderPipelineDeclareTests, TemporalFullscreenHistoryValidityRejectsAFreshPhysical)
{
    using Nodes::TemporalFullscreenShaderNode;
    constexpr uint64_t kPrev = 40u;
    constexpr uint64_t kNow = 41u;
    constexpr uint64_t kWorld = 7u;
    constexpr bool kResident = false;
    constexpr bool kFresh = true;

    EXPECT_TRUE(TemporalFullscreenShaderNode::ComputeHistoryValid(
        1u, kPrev, kNow, 1920u, 1080u, 1920u, 1080u, kWorld, kWorld, kResident))
        << "steady state";
    EXPECT_FALSE(TemporalFullscreenShaderNode::ComputeHistoryValid(
        1u, kPrev, kNow, 1920u, 1080u, 1920u, 1080u, kWorld, kWorld, kFresh))
        << "a recycled physical must invalidate even when continuous";
    EXPECT_FALSE(TemporalFullscreenShaderNode::ComputeHistoryValid(
        1u, kPrev, kNow + 1u, 1920u, 1080u, 1920u, 1080u, kWorld, kWorld, kResident))
        << "a frame gap breaks feedback continuity";
    EXPECT_FALSE(TemporalFullscreenShaderNode::ComputeHistoryValid(
        1u, kPrev, kNow, 1920u, 1080u, 1600u, 1080u, kWorld, kWorld, kResident))
        << "resize";
    EXPECT_FALSE(TemporalFullscreenShaderNode::ComputeHistoryValid(
        1u, kPrev, kNow, 1920u, 1080u, 1920u, 1080u, kWorld, kWorld + 1u, kResident))
        << "world change";
    EXPECT_FALSE(TemporalFullscreenShaderNode::ComputeHistoryValid(
        0u, ~0ull, 0u, 0u, 0u, 1920u, 1080u, kWorld, kWorld, kResident))
        << "first frame for a view";
}

// Which movers may take the dual-skinned MV draw. The MV pass depth-tests
// read-only GreaterOrEqual against the prepass depth the WORLD pass wrote, so a
// draw whose raster position does not reproduce that surface still passes
// wherever the two coincide — and then writes an EXACT motion vector, which the
// resolve trusts over its analytic camera reprojection. Every case that cannot
// reproduce the world position must therefore skip the draw and leave the MV
// clear sentinel, not approximate it with a different one.
TEST(RenderPipelineDeclareTests, TaaMoverMotionPathRejectsInexactSkinnedDraws)
{
    using Path = ViewMotionVectors::MoverMotionPath;
    using GameEngine::Rendering::VertexAttributeFlags;

    constexpr auto kSkinned4 = VertexAttributeFlags::SkinnedMesh;
    constexpr auto kSkinned8 = kSkinned4 | VertexAttributeFlags::Skinned8;

    // No live palette: the transform is the whole motion, whatever streams the
    // mesh carries. A bind-pose skinned mesh moved by its transform belongs here.
    EXPECT_EQ(ViewMotionVectors::ClassifyMoverMotionPath(kSkinned4, false, true, true), Path::Rigid);
    EXPECT_EQ(ViewMotionVectors::ClassifyMoverMotionPath(kSkinned8, false, true, true), Path::Rigid);
    EXPECT_EQ(ViewMotionVectors::ClassifyMoverMotionPath(VertexAttributeFlags::StandardMesh, false,
                                                      false, true),
              Path::Rigid);

    // The one case the dual-skinned stage actually reproduces.
    EXPECT_EQ(ViewMotionVectors::ClassifyMoverMotionPath(kSkinned4, true, true, true), Path::Skinned);

    // SKINNED_8: the shared skinned MV stage blends FOUR influences with
    // non-normalized weights0, so its surface diverges from the 8-influence
    // world/prepass one. DepthDrawRecorder keeps SKINNED_8 off the shared
    // 4-influence depth VS for exactly this reason.
    EXPECT_EQ(ViewMotionVectors::ClassifyMoverMotionPath(kSkinned8, true, true, true), Path::Skip)
        << "8-influence content must not be drawn through the 4-influence MV stage";
    EXPECT_EQ(ViewMotionVectors::ClassifyMoverMotionPath(
                  kSkinned4 | VertexAttributeFlags::HasJoints1, true, true, true),
              Path::Skip);
    EXPECT_EQ(ViewMotionVectors::ClassifyMoverMotionPath(
                  kSkinned4 | VertexAttributeFlags::HasWeights1, true, true, true),
              Path::Skip);

    // Skinned path unavailable (streams unbound, or the pkg is not staged): the
    // rigid draw would raster the BIND POSE against skinned depth. Skipping
    // leaves the sentinel, which is what the LoadShaders warning promises.
    EXPECT_EQ(ViewMotionVectors::ClassifyMoverMotionPath(kSkinned4, true, false, true), Path::Skip)
        << "no joint/weight streams means the rigid draw would be the bind pose";
    EXPECT_EQ(ViewMotionVectors::ClassifyMoverMotionPath(kSkinned4, true, true, false), Path::Skip)
        << "unstaged skinned pkg must fall back to camera reprojection, not bind pose";
}

namespace
{
// The view's ViewParams upload as the real node wrote it, read back from the host-visible ring.
bool ReadBackViewParams(const RenderPipelineInstance& instance, const RenderGraph::RGFrame& frame,
                        Rendering::IDevice& device, ViewId viewId, ViewParamsUBO& out)
{
    const auto* resources = instance.FrameResourcesFor(&frame);
    if (resources == nullptr)
        return false;
    const auto it = resources->Buffers.find({viewId, "ViewParams"});
    if (it == resources->Buffers.end() || it->second.Size != sizeof(ViewParamsUBO))
        return false;
    void* mapped = device.MapBuffer(it->second.Buffer);
    if (mapped == nullptr)
        return false;
    std::memcpy(&out, static_cast<const uint8_t*>(mapped) + it->second.Offset, sizeof(out));
    device.UnmapBuffer(it->second.Buffer);
    return true;
}
} // namespace

// TAAU bias readback: the ViewParams upload the real node writes carries
// ge_mipBiasParams[0] == log2(RenderWidth/OutputWidth) derived from the SAME
// even-snapped extent split the pre-pass publishes (48/64 at scale 0.76), and
// exactly 0.0 at scale 1.0 — the 1.0-neutrality invariant, pinned by reading
// the host-visible ring allocation back.
TEST(RenderPipelineDeclareTests, TaauMipBiasUploadReadsBackExtentRatio)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        const CameraId camId = rs.Views().AllocateCamera("BiasCam");
        CameraData cd{};
        for (int i = 0; i < 16; i += 5)
        {
            cd.view[i] = 1.0f;
            cd.proj[i] = 1.0f;
            cd.viewProj[i] = 1.0f;
        }
        rs.Views().SetCameraData(camId, cd);
        const ViewId viewId = rs.Views().AllocateView("BiasView", camId);
        rs.Views().SetViewRenderLayerMask(viewId, 1u);
        rs.Views().SetViewTargets(viewId, 0, 0, 0, Rendering::ViewClearConfig{});
        rs.Views().SetViewAntiAliasing(viewId, true, AntiAliasingMode::TAA, 8u);
        rs.SetDefaultRenderScale(0.76f);

        RenderPipelineNodeRegistry registry;
        ASSERT_TRUE(registry.Register(
            "ViewParamsUpload", [] { return std::make_unique<Nodes::ViewParamsUploadNode>(); },
            true));

        RenderPipelineBlueprint bp;
        bp.pipelineName = "BiasReadbackTest";
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "ViewParams";
            p.type = "ViewParamsUpload";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"ViewParams","type":"ViewParamsUpload"})";
            bp.passes.push_back(p);
        }
        // The TAAU split requires a materialized world-color resolve target.
        bp.worldColorResolveTargetRef = "SceneColor";
        bp.resources.push_back(
            {"SceneColor",
             R"({"kind":"texture","scope":"perView","format":"r16g16b16a16_float","extent":{"scale":[1,1]},"usage":["renderTarget","shaderResource"]})"});

        RenderPipelineInstance instance(rs, registry);
        instance.SetBlueprint(bp);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);

        RenderGraph::RGTexture color = frame.ImportPersistentTexture("MB.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("MB.Depth", DepthTargetDesc());

        rs.BeginWorldDrawFrame();

        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{viewId, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(rs.Views().GetViews().begin(),
                                                     rs.Views().GetViews().end());

        instance.Declare(frame, targets, views);
        ViewParamsUBO scaled{};
        ASSERT_TRUE(ReadBackViewParams(instance, frame, *device, viewId, scaled));
        // 64 * 0.76 -> floor 48, even-snapped 48: the SAME extent split the
        // pre-pass publishes (ge_screenSize ties the readback to it).
        EXPECT_FLOAT_EQ(scaled.ge_screenSize[0], 48.0f);
        EXPECT_FLOAT_EQ(scaled.ge_screenSize[1], 48.0f);
        EXPECT_FLOAT_EQ(scaled.ge_mipBiasParams[0], std::log2(48.0f / 64.0f)); // ~ -0.415
        EXPECT_EQ(scaled.ge_mipBiasParams[0],
                  Nodes::ViewParamsUploadNode::ComputeTaauMipBias(48u, 64u))
            << "upload must carry exactly the production derivation";

        // Scale 1.0: no split, bias must be EXACTLY 0.0 — not merely small.
        rs.SetDefaultRenderScale(1.0f);
        instance.Declare(frame, targets, views);
        ViewParamsUBO native{};
        ASSERT_TRUE(ReadBackViewParams(instance, frame, *device, viewId, native));
        EXPECT_FLOAT_EQ(native.ge_screenSize[0], 64.0f);
        EXPECT_EQ(native.ge_mipBiasParams[0], 0.0f) << "scale-1.0 neutrality invariant";

        device->WaitForIdle();
        rs.Shutdown();
    }
    device->Shutdown();
}

// Forward shading that is display-referred (unlit particles) divides by the exposure the tonemap
// applies, which it reads from the view's ViewParams: the static scale the effective settings
// resolve, and whether auto exposure meters the view instead.
TEST(RenderPipelineDeclareTests, ViewParamsUploadCarriesTheViewsExposure)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        const CameraId camId = rs.Views().AllocateCamera("ExposureCam");
        CameraData cd{};
        for (int i = 0; i < 16; i += 5)
        {
            cd.view[i] = 1.0f;
            cd.proj[i] = 1.0f;
            cd.viewProj[i] = 1.0f;
        }
        rs.Views().SetCameraData(camId, cd);
        const ViewId viewId = rs.Views().AllocateView("ExposureView", camId);
        rs.Views().SetViewRenderLayerMask(viewId, 1u);
        rs.Views().SetViewTargets(viewId, 0, 0, 0, Rendering::ViewClearConfig{});

        RenderPipelineNodeRegistry registry;
        ASSERT_TRUE(registry.Register(
            "ViewParamsUpload", [] { return std::make_unique<Nodes::ViewParamsUploadNode>(); },
            true));
        RenderPipelineBlueprint bp;
        bp.pipelineName = "ExposureReadbackTest";
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "ViewParams";
            p.type = "ViewParamsUpload";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"ViewParams","type":"ViewParamsUpload"})";
            bp.passes.push_back(p);
        }
        RenderPipelineInstance instance(rs, registry);
        instance.SetBlueprint(bp);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("EX.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("EX.Depth", DepthTargetDesc());
        rs.BeginWorldDrawFrame();
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{viewId, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(rs.Views().GetViews().begin(),
                                                     rs.Views().GetViews().end());

        // A sunlit Manual exposure: EV 15 resolves to about 1/161.
        PostProcessSettings sunlit;
        sunlit.Exposure = 0.0062f;
        sunlit.AutoExposureActive = false;
        rs.Views().SetViewPostProcessOverride(viewId, sunlit);
        instance.Declare(frame, targets, views);
        ViewParamsUBO manual{};
        ASSERT_TRUE(ReadBackViewParams(instance, frame, *device, viewId, manual));
        EXPECT_EQ(manual.ge_exposureParams[0], 0.0062f) << "the static scale the tonemap applies";
        EXPECT_EQ(manual.ge_exposureParams[1], 0.0f) << "nothing meters this view";

        PostProcessSettings metered = sunlit;
        metered.AutoExposureActive = true;
        rs.Views().SetViewPostProcessOverride(viewId, metered);
        instance.Declare(frame, targets, views);
        ViewParamsUBO automatic{};
        ASSERT_TRUE(ReadBackViewParams(instance, frame, *device, viewId, automatic));
        EXPECT_EQ(automatic.ge_exposureParams[1], 1.0f)
            << "auto exposure meters the view: shaders read the metered scale from ExposureHistory";

        device->WaitForIdle();
        rs.Shutdown();
    }
    device->Shutdown();
}

TEST(RenderPipelineDeclareTests, RealNodesShareOneDepthAndSuppressTheWorldClear)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        const CameraId camId = rs.Views().AllocateCamera("NodeCam");
        const ViewId viewId = rs.Views().AllocateView("NodeView", camId);
        rs.Views().SetViewRenderLayerMask(viewId, 1u);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearColorValue[3] = 1.0f;
        clear.clearDepth = true;
        clear.clearDepthValue = 0.0f;
        rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);

        RenderPipelineNodeRegistry registry;
        ASSERT_TRUE(registry.Register(
            "DepthPrepass", [] { return std::make_unique<Nodes::DepthPrepassNode>(); }, true));
        ASSERT_TRUE(registry.Register(
            "WorldRender", [] { return std::make_unique<Nodes::WorldRenderNode>(); }, true));

        RenderPipelineBlueprint bp;
        bp.pipelineName = "NodeTest";
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "Prepass";
            p.type = "DepthPrepass";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"Prepass","type":"DepthPrepass","clearDepthValue":0.0})";
            bp.passes.push_back(p);
        }
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "World";
            p.type = "WorldRender";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"World","type":"WorldRender"})";
            bp.passes.push_back(p);
        }
        bp.outputs.push_back({"FinalColor", "View.Resolve"});

        RenderPipelineInstance instance(rs, registry);
        instance.SetBlueprint(bp);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);

        RenderGraph::RGTexture color = frame.ImportPersistentTexture("NT.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("NT.Depth", DepthTargetDesc());

        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();

        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{viewId, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(rs.Views().GetViews().begin(), rs.Views().GetViews().end());
        instance.Declare(frame, targets, views);

        frame.MarkOutput(color);
        frame.Execute();
        device->WaitForIdle();

        // Both passes attach ONE depth id; world depth is read-only + Load
        // (the declared prepass clears, so the world's clear is suppressed).
        const RenderGraph::RGAttachmentRec* prepassDepth = nullptr;
        const RenderGraph::RGAttachmentRec* worldDepth = nullptr;
        const RenderGraph::RGAttachmentRec* worldColor = nullptr;
        for (const auto& rec : frame.Attachments())
        {
            if (rec.Tex == depth.Id && rec.IsDepth)
            {
                if (rec.Ops.Load == RenderGraph::RGLoadOp::Clear)
                    prepassDepth = &rec;
                else
                    worldDepth = &rec;
            }
            if (rec.Tex == color.Id && !rec.IsDepth)
                worldColor = &rec;
        }
        ASSERT_NE(prepassDepth, nullptr) << "prepass attaches + clears the shared depth";
        ASSERT_NE(worldDepth, nullptr) << "world attaches the SAME depth id";
        EXPECT_TRUE(worldDepth->ReadOnly) << "prepass declared => world depth read-only";
        ASSERT_NE(worldColor, nullptr);
        EXPECT_EQ(worldColor->Ops.Load, RenderGraph::RGLoadOp::Clear) << "color clear NOT suppressed";

        // Prepass scheduled before world.
        EXPECT_LT(ScheduledIndexOf(frame.Graph(), prepassDepth->Pass),
                  ScheduledIndexOf(frame.Graph(), worldDepth->Pass));

        // The world published its EffectiveColor; FinalColor resolves to the
        // caller color (View.Resolve fell back, no pipeline resolve target).
        const auto* fr = instance.FrameResourcesFor(&frame);
        ASSERT_NE(fr, nullptr);
        const auto effIt = fr->Textures.find({viewId, "View.EffectiveColor"});
        ASSERT_NE(effIt, fr->Textures.end());
        EXPECT_EQ(effIt->second.Id, color.Id);
        EXPECT_EQ(instance.GetOutputRG(viewId, "FinalColor").Id, color.Id);

        rs.Shutdown();
    }
    device->Shutdown();
}

// Depth stays READ-ONLY under a prepass while a crossfade is live. This is the
// contract the per-segment prepass variant bought: the prepass now draws each
// fading tail through the same dither the colour pass runs, so the fading
// instance's depth is already in the attachment when the colour pass loads it,
// and the whole-pass depth-write carve-out that used to stand in for it is gone.
//
// Making the pass writable instead would cost every opaque fragment in the scene
// a redundant depth write for the length of any fade, and it never repaired the
// pre-world consumers (GTAO, SDSM, cluster bounds) that read the resolve taken
// before the colour pass runs — which was the artifact.
//
// The verdict is driven, not assumed: `scheduleCulling` resolves it LIVE on the
// first culling frame (the rule's settling frame, pinned by
// LodCrossfadeLivenessTests). If anything re-couples the declaration to that
// verdict, this reds.
TEST(RenderPipelineDeclareTests, CrossfadeKeepsWorldDepthReadOnlyUnderThePrepass)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        const CrossfadeDepthDecl decl =
            DeclareCrossfadePrepassWorld(device.get(), /*scheduleCulling=*/true);
        ASSERT_TRUE(decl.PrepassClearsSharedDepth) << "prepass attaches + clears the shared depth";
        ASSERT_TRUE(decl.WorldLoadsSharedDepth)
            << "world attaches the SAME depth id, loading (clear suppression survives)";
        EXPECT_TRUE(decl.WorldDepthReadOnly)
            << "a live crossfade verdict must NOT reopen depth writes: the prepass draws the "
               "fading tail through the same dither, so its depth is already there";
    }
    device->Shutdown();
}

// The prepass draws a Mask material's heads through its own alpha test, so it already wrote the view's
// cutout depth: a Mask batch in view keeps the world pass's depth read-only. A parallax material beside it
// then reads the prepass's relief depth from that attachment (ParallaxDepthFromPrepass), which the world
// pass declares as a sampled read of the same depth. A writable depth here would send the parallax colour
// pass back to marching against the prepass's march.
TEST(RenderPipelineDeclareTests, AMaskBatchKeepsTheWorldDepthReadOnlyForTheReliefRead)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        Material cutout = Material::TestFactory::Create(GUID::Generate(), "CutoutMat", 32u);
        Material::TestFactory::SetGraphicsPipelineId(cutout, Rendering::GraphicsPipelineId{1u});
        Material::TestFactory::SetGpuSceneMaterialIndex(cutout, 1u);
        Material::TestFactory::SetAlphaMode(cutout, MaterialAlphaMode::Mask);
        {
            Rendering::ShaderVariantKey key{};
            key.materialKeywords = Rendering::MaterialKeyword::AlphaTest;
            Material::TestFactory::SetVariantKey(cutout, key);
        }
        Material relief = Material::TestFactory::Create(GUID::Generate(), "ReliefMat", 32u);
        Material::TestFactory::SetGraphicsPipelineId(relief, Rendering::GraphicsPipelineId{2u});
        Material::TestFactory::SetGpuSceneMaterialIndex(relief, 2u);
        {
            Rendering::ShaderVariantKey key{};
            key.materialKeywords = Rendering::MaterialKeyword::Parallax;
            Material::TestFactory::SetVariantKey(relief, key);
        }
        const Material* const submitted[] = {&cutout, &relief};
        const CrossfadeDepthDecl decl =
            DeclareCrossfadePrepassWorld(device.get(), /*scheduleCulling=*/false, submitted);
        ASSERT_TRUE(decl.PrepassClearsSharedDepth);
        ASSERT_TRUE(decl.WorldLoadsSharedDepth);
        EXPECT_TRUE(decl.WorldDepthReadOnly) << "a Mask batch must not reopen depth writes under the prepass";
        EXPECT_TRUE(decl.WorldSamplesDepth) << "the relief's colour pass reads the prepass's depth";
    }
    device->Shutdown();
}

// The world pass's depth access is decided per forward draw (#2433). A contributor whose producer drew its
// depth into the camera prepass (CBT terrain, grass) leaves the world depth read-only; one that writes its
// own depth (a draw whose head pipeline the device has not built yet) keeps it writable, or that surface
// would test against a depth nothing wrote and drop out (holes, far terrain over near).
TEST(RenderPipelineDeclareTests, AForwardDrawDecidesTheWorldDepthAccessByWhereItsDepthIsWritten)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        const CrossfadeDepthDecl headInPrepass =
            DeclareCrossfadePrepassWorld(device.get(), /*scheduleCulling=*/false, {}, ForwardDrawDepth::Prepass);
        ASSERT_TRUE(headInPrepass.WorldLoadsSharedDepth);
        EXPECT_TRUE(headInPrepass.WorldDepthReadOnly)
            << "a forward draw whose head the prepass drew must not reopen depth writes";

        const CrossfadeDepthDecl writesOwnDepth =
            DeclareCrossfadePrepassWorld(device.get(), /*scheduleCulling=*/false, {}, ForwardDrawDepth::ColourPass);
        ASSERT_TRUE(writesOwnDepth.WorldLoadsSharedDepth);
        EXPECT_FALSE(writesOwnDepth.WorldDepthReadOnly)
            << "a forward draw that writes its own depth needs the world depth writable";

        const CrossfadeDepthDecl writesNoDepth =
            DeclareCrossfadePrepassWorld(device.get(), /*scheduleCulling=*/false, {}, ForwardDrawDepth::None);
        ASSERT_TRUE(writesNoDepth.WorldLoadsSharedDepth);
        EXPECT_TRUE(writesNoDepth.WorldDepthReadOnly) << "a blended forward draw writes no depth";
    }
    device->Shutdown();
}

// Grass receives neither GTAO nor the contact shadows, so it must not occlude them either: the
// screen-space passes read the copy DepthResolve takes, and a PrepassNonOccluding head (grass) joins the
// view depth only after that copy, in its own pass before the world pass that depth-tests against it.
TEST(RenderPipelineDeclareTests, NonOccludingHeadsJoinTheDepthAfterTheScreenSpaceCopy)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        const PrepassOrderDecl grass =
            DeclarePrepassResolveWorld(device.get(), /*withResolve=*/true, ForwardDrawDepth::PrepassNonOccluding);
        if (!grass.Declared)
            GTEST_SKIP() << "depth_resolve shaders unavailable in this environment";
        ASSERT_GE(grass.CameraPrepass, 0);
        ASSERT_GE(grass.World, 0);
        EXPECT_TRUE(grass.ResolveReadsViewDepth);
        ASSERT_GE(grass.NonOccludingPrepass, 0) << "the grass heads must draw in their own pass";
        EXPECT_LT(grass.CameraPrepass, grass.Resolve);
        EXPECT_LT(grass.Resolve, grass.NonOccludingPrepass)
            << "the copy GTAO and the contact shadows read must be taken before the grass heads draw";
        EXPECT_LT(grass.NonOccludingPrepass, grass.World) << "the colour pass depth-tests against the grass heads";
        EXPECT_TRUE(grass.NonOccludingLoadsViewDepth) << "the heads add to the camera prepass's depth";
        EXPECT_TRUE(grass.NonOccludingDeclaredOnView) << "the camera prepass must not draw the heads a second time";
        // The pass draws no entity batch, so it takes no edge from the scatter or the skinning.

        const PrepassOrderDecl terrain =
            DeclarePrepassResolveWorld(device.get(), /*withResolve=*/true, ForwardDrawDepth::Prepass);
        EXPECT_LT(terrain.NonOccludingPrepass, 0) << "an occluding head (terrain) stays in the camera prepass";
    }
    device->Shutdown();
}

// Every reader of View.DepthResolved other than GTAO and the contact shadows decides visibility or
// distance from it as from the depth the colour shows (the ocean's underwater composite paints water
// wherever the ray to that depth crosses the water body), so it must hold the non-occluding heads:
// DepthResolve copies the depth a second time after the non-occluding prepass and publishes that copy
// as View.DepthResolved, and the copy taken before the heads as View.OccluderDepthResolved.
TEST(RenderPipelineDeclareTests, ResolvedDepthHoldsTheNonOccludingHeadsAndTheOccludersDepthDoesNot)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        const PrepassOrderDecl grass =
            DeclarePrepassResolveWorld(device.get(), /*withResolve=*/true, ForwardDrawDepth::PrepassNonOccluding);
        if (!grass.Declared)
            GTEST_SKIP() << "depth_resolve shaders unavailable in this environment";
        ASSERT_GE(grass.NonOccludingPrepass, 0);
        ASSERT_TRUE(grass.SceneDepthPublished);
        EXPECT_GT(grass.SceneDepthWriter, grass.NonOccludingPrepass)
            << "View.DepthResolved is copied after the grass heads draw, so its readers see the blades";
        EXPECT_TRUE(grass.SceneDepthWriterReadsViewDepth);
        EXPECT_LT(grass.SceneDepthWriter, grass.World);
        EXPECT_TRUE(grass.WorldReadsSceneDepth) << "the world pass binds View.DepthResolved as ge_sceneDepth";
        EXPECT_TRUE(grass.OccluderDepthPublished) << "GTAO and the contact shadows read their own copy";
        EXPECT_FALSE(grass.SceneAndOccluderDepthAreOneTexture);
        EXPECT_EQ(grass.OccluderDepthWriter, grass.Resolve);
        EXPECT_LT(grass.OccluderDepthWriter, grass.NonOccludingPrepass)
            << "the occluders' depth is copied before the grass heads draw";
        EXPECT_EQ(grass.ResolvePasses, 2);
        // The occluders' depth is for the occlusion passes alone: any other reader would treat the blades
        // as absent, the way the underwater composite drew the lake over them. A new pass reads
        // View.DepthResolved unless it is added here on purpose.
        EXPECT_TRUE(grass.GtaoReadsOccluderDepth);
        EXPECT_TRUE(grass.ContactShadowsReadOccluderDepth) << "the contact shadows march the occluders' depth";
        for (const std::string& reader : grass.OccluderDepthReaders)
            EXPECT_TRUE(IsOccluderDepthReader(grass, reader))
                << reader << " reads the occluders' depth, which only GTAO and the contact shadows may read";
        // The list matches whole names: a pass that only shares a word with an allowed one is refused.
        EXPECT_FALSE(IsOccluderDepthReader(grass, grass.AmbientOcclusionPassPrefix + "GTAOSomethingElse"));
        EXPECT_FALSE(IsOccluderDepthReader(grass, "Pipeline.NoOccl.World.GTAOPrepare"));
        EXPECT_FALSE(IsOccluderDepthReader(grass, grass.ContactShadowPass + ".Composite"));

        // Without non-occluding heads there is one copy, published under both names.
        const PrepassOrderDecl terrain =
            DeclarePrepassResolveWorld(device.get(), /*withResolve=*/true, ForwardDrawDepth::Prepass);
        EXPECT_LT(terrain.NonOccludingPrepass, 0);
        EXPECT_TRUE(terrain.SceneAndOccluderDepthAreOneTexture);
        EXPECT_EQ(terrain.SceneDepthWriter, terrain.Resolve);
        EXPECT_EQ(terrain.ResolvePasses, 1) << "no heads, no second copy";
    }
    device->Shutdown();
}

// A pipeline that takes no screen-space depth copy declares no non-occluding prepass, and the view
// records that none was declared, which is what makes the camera prepass draw those heads itself. This
// checks the declaration only; the fallback's drawing is pinned by the grass prepass depth tests
// (OpaqueGrassPrepassDepthIsTheDepthItsColourDrawsWrite, DitheredGrassPrepassDepthIsTheDepthItsColourDrawsWrite,
// AlphaToCoverageGrassPrepassKeepsTheSamplesItsColourDrawsKeep), whose blueprints have no DepthResolve.
TEST(RenderPipelineDeclareTests, NonOccludingHeadsDeclareNoPassWithoutADepthResolve)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        const PrepassOrderDecl grass =
            DeclarePrepassResolveWorld(device.get(), /*withResolve=*/false, ForwardDrawDepth::PrepassNonOccluding);
        ASSERT_GE(grass.CameraPrepass, 0);
        EXPECT_LT(grass.NonOccludingPrepass, 0);
        EXPECT_FALSE(grass.NonOccludingDeclaredOnView);
    }
    device->Shutdown();
}

// The non-occluding prepass draws no entity batch, so it takes no edge from the entity-batch inputs: it
// reads neither the draw-stream ordering the scatter writes nor the skin palette the skinning writes.
// Driven through the frame spine, which publishes both, with the camera prepass as the control.
TEST(RenderPipelineDeclareTests, NonOccludingPrepassTakesNoEntityBatchEdges)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        RenderPipelineBlueprint bp{};
        bp.schemaVersion = 2;
        bp.pipelineName = "NoOcclSpine";
        bp.sourcePath = "<test>";
        bp.contentHash = 0x2753u;
        const auto addPass = [&bp](const char* id, const char* type, const char* json)
        {
            RenderPipelineBlueprint::Pass p;
            p.id = id;
            p.type = type;
            p.enabled = true;
            p.perView = true;
            p.passJson = json;
            bp.passes.push_back(p);
        };
        addPass("Prepass", "DepthPrepass", R"({"id":"Prepass","type":"DepthPrepass","clearDepthValue":0.0})");
        addPass("DepthResolve", "DepthResolve", R"({"id":"DepthResolve","type":"DepthResolve"})");
        addPass("World", "WorldRender", R"({"id":"World","type":"WorldRender"})");
        bp.outputs.push_back({Names::Output::FinalColor, Names::View::Resolve});
        rs.Spine().SetActiveRenderPipelineBlueprint(std::move(bp));

        const CameraId camId = rs.Views().AllocateCamera("NoOcclSpineCam");
        CameraData cd{};
        for (int i = 0; i < 16; i += 5)
        {
            cd.view[i] = 1.0f;
            cd.proj[i] = 1.0f;
            cd.viewProj[i] = 1.0f;
        }
        rs.Views().SetCameraData(camId, cd);
        const ViewId viewId = rs.Views().AllocateView("NoOcclSpineView", camId);
        rs.Views().SetViewRenderLayerMask(viewId, 1u);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);
        // One GPUScene instance, so the culling and the scatter run and publish the ordering.
        ASSERT_NE(rs.GetGPUScene(), nullptr);
        Rendering::GPUInstance inst{};
        inst.boundingRadius = 1.0f;
        inst.flags = ~0u;
        rs.GetGPUScene()->AddInstance(inst);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        GameEngine::Testing::ScopedPipelineFrame frameRegistration(rs.Spine(), frame);
        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("NoOcclSpine.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("NoOcclSpine.Depth", DepthTargetDesc());
        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();
        const DrawCommand head{};
        rs.EmitForwardCommand(viewId, DrawCommand{}, ForwardDrawDepth::PrepassNonOccluding, &head);
        const ViewTargetsRG vt{viewId, color, depth, {}};
        RenderServices::FrameGraphBuildParamsRG params{};
        params.ViewTargets = std::span<const ViewTargetsRG>(&vt, 1);
        rs.Spine().BuildFrameGraph(frame, params);

        const RenderGraph::RGBuffer ordering = rs.FrameRG().DrawStreamOrdering;
        const RenderGraph::RGBuffer skinPalette = rs.FrameRG().SkinPaletteAtlas;
        const auto& g = frame.Graph();
        int cameraPrepass = -1;
        int nonOccluding = -1;
        for (size_t i = 0; i < g.PassCount(); ++i)
        {
            const std::string name = g.PassName(static_cast<RenderGraph::RGPassId>(i));
            if (name.rfind("DepthPrepass[", 0) != 0)
                continue;
            (name.find("NonOccluding") != std::string::npos ? nonOccluding : cameraPrepass) = static_cast<int>(i);
        }
        if (nonOccluding < 0 && cameraPrepass >= 0)
        {
            rs.Shutdown();
            GTEST_SKIP() << "depth_resolve shaders unavailable in this environment";
        }
        ASSERT_GE(cameraPrepass, 0);
        ASSERT_TRUE(ordering.IsValid() || skinPalette.IsValid())
            << "neither entity-batch input is published: the checks below are vacuous";
        const auto camera = static_cast<RenderGraph::RGPassId>(cameraPrepass);
        const auto late = static_cast<RenderGraph::RGPassId>(nonOccluding);
        if (ordering.IsValid())
        {
            EXPECT_TRUE(g.HasReadAccess(camera, ordering.Id)) << "the control: the camera prepass draws the batches";
            EXPECT_FALSE(g.HasReadAccess(late, ordering.Id));
        }
        if (skinPalette.IsValid())
        {
            EXPECT_TRUE(g.HasReadAccess(camera, skinPalette.Id)) << "the control";
            EXPECT_FALSE(g.HasReadAccess(late, skinPalette.Id));
        }
        frame.Execute();
        device->WaitForIdle();
        rs.Shutdown();
    }
    device->Shutdown();
}

// The complement, and together with the test above the pin that the depth
// declaration is INDEPENDENT of the crossfade: with no culling entry there is no
// verdict at all, and the prepass read-only win-back must stand for the same
// reason it stands with one.
TEST(RenderPipelineDeclareTests, CrossfadeDurationAloneDoesNotCarveOutReadOnlyDepth)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        const CrossfadeDepthDecl decl =
            DeclareCrossfadePrepassWorld(device.get(), /*scheduleCulling=*/false);
        ASSERT_TRUE(decl.PrepassClearsSharedDepth) << "prepass attaches + clears the shared depth";
        ASSERT_TRUE(decl.WorldLoadsSharedDepth)
            << "world attaches the SAME depth id, loading (clear suppression survives)";
        EXPECT_TRUE(decl.WorldDepthReadOnly)
            << "no liveness verdict => the prepass read-only win-back must stand";
    }
    device->Shutdown();
}

TEST(RenderPipelineDeclareTests, UploadNodesDissolveIntoTheRing)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        const CameraId camId = rs.Views().AllocateCamera("UploadCam");
        CameraData cd{};
        for (int i = 0; i < 16; i += 5)
        {
            cd.view[i] = 1.0f;
            cd.proj[i] = 1.0f;
            cd.viewProj[i] = 1.0f;
        }
        rs.Views().SetCameraData(camId, cd);
        const ViewId viewId = rs.Views().AllocateView("UploadView", camId);
        rs.Views().SetViewRenderLayerMask(viewId, 1u);
        rs.Views().SetViewTargets(viewId, 0, 0, 0, Rendering::ViewClearConfig{});

        RenderPipelineNodeRegistry registry;
        ASSERT_TRUE(registry.Register(
            "ViewParamsUpload", [] { return std::make_unique<Nodes::ViewParamsUploadNode>(); },
            true));
        ASSERT_TRUE(registry.Register(
            "LightUpload", [] { return std::make_unique<Nodes::LightUploadNode>(); }, true));

        RenderPipelineBlueprint bp;
        bp.pipelineName = "UploadTest";
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "ViewParams";
            p.type = "ViewParamsUpload";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"ViewParams","type":"ViewParamsUpload"})";
            bp.passes.push_back(p);
        }
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "Lights";
            p.type = "LightUpload";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"Lights","type":"LightUpload"})";
            bp.passes.push_back(p);
        }

        RenderPipelineInstance instance(rs, registry);
        instance.SetBlueprint(bp);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);

        RenderGraph::RGTexture color = frame.ImportPersistentTexture("UP.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("UP.Depth", DepthTargetDesc());

        rs.BeginWorldDrawFrame();

        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{viewId, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(rs.Views().GetViews().begin(), rs.Views().GetViews().end());
        instance.Declare(frame, targets, views);

        // No passes — the uploads ARE the declaration.
        EXPECT_EQ(frame.Graph().PassCount(), 0u) << "upload nodes must not declare passes";

        const auto* fr = instance.FrameResourcesFor(&frame);
        ASSERT_NE(fr, nullptr);
        const auto vpIt = fr->Buffers.find({viewId, "ViewParams"});
        ASSERT_NE(vpIt, fr->Buffers.end());
        EXPECT_EQ(vpIt->second.Size, static_cast<uint32_t>(sizeof(GameEngine::Rendering::ViewParamsUBO)))
            << "sized from the struct";
        EXPECT_FALSE(vpIt->second.Graph.IsValid()) << "upload allocs carry no graph edge";

        const auto lbIt = fr->Buffers.find({viewId, "LightBuffer"});
        ASSERT_NE(lbIt, fr->Buffers.end());
        // Header (16) plus one zeroed GPULightPacked (144). WebGPU validates the
        // binding against the WGSL struct's minimum size, which counts one
        // runtime-array element; count=0 keeps the shader from reading it.
        EXPECT_EQ(lbIt->second.Size, 160u)
            << "exact-size: header + one dummy record (WebGPU min binding size)";
        // The ring is host-visible: the header's count word must be 0.
        if (void* p = device->MapBuffer(lbIt->second.Buffer))
        {
            const uint32_t count =
                *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(p) +
                                                   lbIt->second.Offset);
            device->UnmapBuffer(lbIt->second.Buffer);
            EXPECT_EQ(count, 0u);
        }

        device->WaitForIdle();
        rs.Shutdown();
    }
    device->Shutdown();
}

TEST(RenderPipelineDeclareTests, ComputeNodeWithoutItsPipelineDeclaresNothing)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        const ViewId viewId = rs.Views().AllocateView("CSView", rs.Views().AllocateCamera("CSCam"));
        rs.Views().SetViewRenderLayerMask(viewId, 1u);

        RenderPipelineNodeRegistry registry;
        ASSERT_TRUE(registry.Register(
            "ComputeShader", [] { return std::make_unique<Nodes::ComputeShaderNode>(); }, true));

        RenderPipelineBlueprint bp;
        bp.pipelineName = "CSTest";
        RenderPipelineBlueprint::Pass p;
        p.id = "Cull";
        p.type = "ComputeShader";
        p.enabled = true;
        p.perView = true;
        p.passJson =
            R"json({"id":"Cull","type":"ComputeShader","shaderPkg":"does_not_exist.shaderpkg","dispatch":{"x":"ceil(renderWidth / 32)","y":"ceil(renderHeight / 32)","z":24}})json";
        bp.passes.push_back(p);

        RenderPipelineInstance instance(rs, registry);
        instance.SetBlueprint(bp);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("CS.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("CS.Depth", DepthTargetDesc());

        rs.BeginWorldDrawFrame();
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{viewId, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(rs.Views().GetViews().begin(), rs.Views().GetViews().end());
        instance.Declare(frame, targets, views);

        // No shader package => no pipeline => the node declares NOTHING (the
        // graceful-skip contract; a real pkg is exercised at gate 1).
        EXPECT_EQ(frame.Graph().PassCount(), 0u);

        device->WaitForIdle();
        rs.Shutdown();
    }
    device->Shutdown();
}

TEST(RenderPipelineDeclareTests, CsmNodeCachesFrameDataBeforeTheCascadeArms)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        const CameraId camId = rs.Views().AllocateCamera("CsmCam");
        CameraData cd{};
        // A plausible perspective proj (nonzero [0]/[5] passes the thumbnail
        // guard; near/far extractable).
        cd.proj[0] = 1.0f;
        cd.proj[5] = 1.0f;
        cd.proj[10] = 0.001f;
        cd.proj[11] = 1.0f;
        cd.proj[14] = 0.1f;
        for (int i = 0; i < 16; i += 5)
        {
            cd.view[i] = 1.0f;
            cd.viewProj[i] = 1.0f;
        }
        rs.Views().SetCameraData(camId, cd);
        const ViewId viewId = rs.Views().AllocateView("CsmView", camId);
        rs.Views().SetViewRenderLayerMask(viewId, 1u);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);

        // A caster + a shadow-casting directional light with cascadeCount 2 —
        // pins the min() against the node's 4 configured cascades.
        auto depHandle = rs.RegisterDepthEmit(
            [&](DepthEmitContext& ctx, DepthPassType passType)
            {
                if (ctx.ViewId == viewId && passType == DepthPassType::ShadowCascade)
                    ctx.Services->EmitDepthCommand(ctx.ViewId, passType, DrawCommand{});
            });

        RenderPipelineNodeRegistry registry;
        ASSERT_TRUE(registry.Register(
            "ShadowMap", [] { return std::make_unique<Nodes::ShadowMapNode>(); },
            true));
        ASSERT_TRUE(registry.Register(
            "WorldRender", [] { return std::make_unique<Nodes::WorldRenderNode>(); }, true));

        RenderPipelineBlueprint bp;
        bp.pipelineName = "CsmTest";
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "CSM";
            p.type = "ShadowMap";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"CSM","type":"ShadowMap","buffer":"ShadowData"})";
            bp.passes.push_back(p);
        }
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "World";
            p.type = "WorldRender";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"World","type":"WorldRender","keywords":["Shadows"]})";
            bp.passes.push_back(p);
        }

        RenderPipelineInstance instance(rs, registry);
        instance.SetBlueprint(bp);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("CSM.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("CSM.Depth", DepthTargetDesc());

        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();
        ExtractedLight sun{};
        sun.type = GameEngine::Components::LightType::Directional;
        sun.castsShadows = 1;
        sun.castsLight = 1;
        sun.directionWS[1] = -1.0f;
        sun.cascadeCount = 2;
        rs.SubmitLight(0u, sun);

        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{viewId, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(rs.Views().GetViews().begin(), rs.Views().GetViews().end());
        instance.Declare(frame, targets, views);

        frame.MarkOutput(color);
        frame.Execute();
        device->WaitForIdle();

        // Frame data cached at declaration, clamped to the light's count.
        auto* feature = rs.GetFeature<ShadowMapRenderFeature>();
        ASSERT_NE(feature, nullptr);
        const auto* fd = feature->GetCachedFrameData(viewId);
        ASSERT_NE(fd, nullptr) << "the node must CacheFrameData BEFORE the cascade arms";
        EXPECT_EQ(fd->NumCascades, 2u) << "min(config 4, light cascadeCount 2)";

        // Exactly the cached count of cascade passes declared, one per layer.
        uint32_t cascadeAttaches = 0;
        for (const auto& rec : frame.Attachments())
            if (rec.IsDepth && rec.Range.LayerCount == 1 && rec.Range.BaseLayer < 4 &&
                rec.Tex != depth.Id)
                ++cascadeAttaches;
        EXPECT_EQ(cascadeAttaches, 2u) << "out-of-range cascade arms decline at declaration";

        // A1.1 golden pass-name lock: the cascade pass names must be byte-identical
        // to the framework PassName format (Pipeline.<pipeline>.<passId>.View<N>.<suffix>)
        // across the move — the node builds "Cascade%u" suffixes, and after the body
        // moves into the feature it builds the SAME suffixes through the same
        // ViewDeclare::PassName forwarder. No transmissive casters here => no GlassTint.
        {
            const std::string base =
                "Pipeline.CsmTest.CSM.View" + std::to_string(static_cast<uint32_t>(viewId));
            const std::vector<std::string> golden = {base + ".Cascade0", base + ".Cascade1"};
            std::vector<std::string> declaredCascades;
            uint32_t tintCount = 0;
            for (const RenderGraph::RGPassId p : frame.Graph().ScheduledOrder())
            {
                const std::string name = frame.Graph().PassName(p);
                // Restrict the family query to this node's passes; other shadow
                // producers can also use a Cascade segment in their names.
                if (name.rfind(base, 0) == 0 && RGQuery::Matches(name, RGQuery::Family{"Cascade"}))
                    declaredCascades.push_back(name);
                if (RGQuery::Matches(name, RGQuery::Family{"GlassTint"}))
                    ++tintCount;
            }
            EXPECT_EQ(declaredCascades, golden) << "cascade pass names must match the golden list";
            EXPECT_EQ(tintCount, 0u) << "no transmissive casters => no tint passes declared";
        }

        // ShadowData published into the blackboard (the world table binds it).
        const auto* fr = instance.FrameResourcesFor(&frame);
        ASSERT_NE(fr, nullptr);
        const auto sdIt = fr->Buffers.find({viewId, "ShadowData"});
        ASSERT_NE(sdIt, fr->Buffers.end());
        EXPECT_EQ(sdIt->second.Size, sizeof(ShadowDataGPU));
        EXPECT_FALSE(sdIt->second.Graph.IsValid()) << "ring alloc carries no graph edge";

        depHandle.Reset();
        rs.Shutdown();
    }
    device->Shutdown();
}

namespace
{
// What one frame of a one-view ShadowMap pipeline fitted and published.
struct SceneFitFrame
{
    float CachedDistance = 0.0f;
    float PublishedDistance = 0.0f;
    float PublishedFade = 0.0f;
};

// A one-view ShadowMap pipeline over a scene of one instance, the sphere at
// (0, 0, centerZ) of `radius`, declared frame after frame with the view's
// state carried between frames. The camera sits on the Z axis (near 0.1,
// far 1000) and looks along +Z or, turned, along -Z.
class SceneFitRig
{
  public:
    SceneFitRig(Rendering::IDevice& device, const char* passJson, float centerZ, float radius)
        : m_Device(device), m_Pools(&device)
    {
        m_Ready = m_Services.Initialize(&device);
        if (!m_Ready)
            return;
        m_CameraId = m_Services.Views().AllocateCamera("SceneFitCam");
        SetCamera(0.0f, false);
        m_ViewId = m_Services.Views().AllocateView("SceneFitView", m_CameraId);
        m_Services.Views().SetViewRenderLayerMask(m_ViewId, 1u);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        m_Services.Views().SetViewTargets(m_ViewId, 0, 0, 0, clear);

        Rendering::GPUInstance instance{};
        instance.boundingCenter = Mathematics::Vector3{0.0f, 0.0f, centerZ};
        instance.boundingRadius = radius;
        instance.flags = ~0u;
        m_Services.GetGPUScene()->AddInstance(instance);

        m_Registry.Register("ShadowMap", [] { return std::make_unique<Nodes::ShadowMapNode>(); }, true);
        RenderPipelineBlueprint blueprint;
        blueprint.pipelineName = "SceneFit";
        RenderPipelineBlueprint::Pass pass;
        pass.id = "CSM";
        pass.type = "ShadowMap";
        pass.enabled = true;
        pass.perView = true;
        pass.passJson = passJson;
        blueprint.passes.push_back(pass);
        m_Instance = std::make_unique<RenderPipelineInstance>(m_Services, m_Registry);
        m_Instance->SetBlueprint(blueprint);
    }

    ~SceneFitRig()
    {
        if (m_Ready)
            m_Services.Shutdown();
    }
    SceneFitRig(const SceneFitRig&) = delete;
    SceneFitRig& operator=(const SceneFitRig&) = delete;

    bool Ready() const { return m_Ready; }

    // The camera at (0, 0, z), looking along +Z, or along -Z when `turned`.
    void SetCamera(float z, bool turned)
    {
        Rendering::CameraData camera{};
        camera.proj[0] = 1.0f;
        camera.proj[5] = 1.0f;
        camera.proj[10] = 0.001f;
        camera.proj[11] = 1.0f;
        camera.proj[14] = 0.1f;
        const float axis = turned ? -1.0f : 1.0f;
        camera.view[0] = axis;
        camera.view[5] = 1.0f;
        camera.view[10] = axis;
        camera.view[15] = 1.0f;
        camera.view[14] = -axis * z;
        for (int i = 0; i < 16; i += 5)
            camera.viewProj[i] = 1.0f;
        camera.cameraPos[2] = z;
        m_Services.Views().SetCameraData(m_CameraId, camera);
    }

    SceneFitFrame DeclareFrame()
    {
        SceneFitFrame result{};
        RenderGraph::RGFrame frame(&m_Device, &m_Pools.Persistent, &m_Pools.Transient, &m_Pools.Ring);
        frame.BeginFrame(m_FrameIndex++);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("SceneFit.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("SceneFit.Depth", DepthTargetDesc());
        m_Services.BeginWorldDrawFrame();
        m_Services.BuildWorldBatchKeys();
        ExtractedLight sun{};
        sun.type = GameEngine::Components::LightType::Directional;
        sun.castsShadows = 1;
        sun.castsLight = 1;
        sun.directionWS[1] = -1.0f;
        sun.cascadeCount = 4;
        m_Services.SubmitLight(0u, sun);
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{m_ViewId, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(m_Services.Views().GetViews().begin(),
                                                     m_Services.Views().GetViews().end());
        m_Instance->Declare(frame, targets, views);
        frame.MarkOutput(color);
        frame.Execute();
        m_Device.WaitForIdle();

        if (const auto* feature = m_Services.GetFeature<ShadowMapRenderFeature>())
            if (const CascadeFrameData* cached = feature->GetCachedFrameData(m_ViewId))
                result.CachedDistance = cached->MaxShadowDistance;
        const auto* resources = m_Instance->FrameResourcesFor(&frame);
        if (resources == nullptr)
            return result;
        const auto binding = resources->Buffers.find({m_ViewId, "ShadowData"});
        if (binding == resources->Buffers.end())
            return result;
        const auto* mapped = static_cast<const uint8_t*>(m_Device.MapBuffer(binding->second.Buffer));
        if (mapped == nullptr)
            return result;
        ShadowDataGPU data{};
        std::memcpy(&data, mapped + binding->second.Offset, sizeof(data));
        m_Device.UnmapBuffer(binding->second.Buffer);
        result.PublishedDistance = data.shadowParams[3];
        result.PublishedFade = data.shadowFilterParams[1];
        return result;
    }

  private:
    Rendering::IDevice& m_Device;
    RenderServices m_Services;
    RenderPipelineNodeRegistry m_Registry;
    std::unique_ptr<RenderPipelineInstance> m_Instance;
    FramePools m_Pools;
    CameraId m_CameraId{};
    ViewId m_ViewId{};
    uint64_t m_FrameIndex = 0;
    bool m_Ready = false;
};

SceneFitFrame DeclareSceneFitFrame(Rendering::IDevice& device, const char* passJson, float centerZ,
                                   float radius)
{
    SceneFitRig rig(device, passJson, centerZ, radius);
    return rig.Ready() ? rig.DeclareFrame() : SceneFitFrame{};
}

// The far side, along +Z, of the sphere around the box of the sphere at
// (0, 0, centerZ) of `radius`, seen from (0, 0, cameraZ).
float SceneFitReach(float centerZ, float radius, float cameraZ)
{
    return centerZ - cameraZ + radius * std::sqrt(3.0f);
}

// The range a scene fit reaches for `reach`: the fade band starts
// kSceneFitHeadroom beyond it.
float ExpectedSceneFitDistance(float reach, float fadeFraction)
{
    return reach * ShadowMapRenderFeature::kSceneFitHeadroom / (1.0f - fadeFraction);
}

constexpr const char* kSceneFitPass =
    R"({"id":"CSM","type":"ShadowMap","buffer":"ShadowData","maxShadowDistance":200.0,"fitShadowDistanceToScene":true})";
} // namespace

// #3463: a ShadowMap pass with "fitShadowDistanceToScene" fits the directional
// range to the scene the view sees, whatever its scale, and the fade the GPU
// applies follows that range; without the key the authored distance stands.
TEST(RenderPipelineDeclareTests, ShadowMapSceneFitFollowsTheSceneAndTheOptOutKeepsTheAuthoredDistance)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        constexpr const char* kAuthored =
            R"({"id":"CSM","type":"ShadowMap","buffer":"ShadowData","maxShadowDistance":200.0})";
        const float fade = Components::ShadowSettingsEffect{}.DistanceFadeFraction;

        // A 2 m model a few metres ahead: the range shrinks to the scene.
        const SceneFitFrame small = DeclareSceneFitFrame(*device, kSceneFitPass, 5.0f, 1.0f);
        EXPECT_NEAR(small.CachedDistance, ExpectedSceneFitDistance(SceneFitReach(5.0f, 1.0f, 0.0f), fade), 1e-3f);
        EXPECT_EQ(small.PublishedDistance, small.CachedDistance)
            << "the GPU fade must run over the fitted range";
        EXPECT_FLOAT_EQ(small.PublishedFade, fade);

        // A 500 m scene: the range grows past the authored 200.
        const SceneFitFrame large = DeclareSceneFitFrame(*device, kSceneFitPass, 300.0f, 250.0f);
        EXPECT_NEAR(large.CachedDistance, ExpectedSceneFitDistance(SceneFitReach(300.0f, 250.0f, 0.0f), fade),
                    1e-1f);
        EXPECT_GT(large.CachedDistance, 200.0f);
        EXPECT_EQ(large.PublishedDistance, large.CachedDistance);

        // The opt-out keeps the authored distance for either scene.
        EXPECT_EQ(DeclareSceneFitFrame(*device, kAuthored, 5.0f, 1.0f).PublishedDistance, 200.0f);
        EXPECT_EQ(DeclareSceneFitFrame(*device, kAuthored, 300.0f, 250.0f).PublishedDistance, 200.0f);
    }
    device->Shutdown();
}

// #3463: a view's scene fit carries from frame to frame. A reach that moves
// inside the band keeps the range, a reach that would enter the fade grows it,
// and a camera turned away from the scene keeps the previous fit rather than
// falling back to the authored distance.
TEST(RenderPipelineDeclareTests, ShadowMapSceneFitCarriesTheRangeAcrossFrames)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        const float fade = Components::ShadowSettingsEffect{}.DistanceFadeFraction;
        SceneFitRig rig(*device, kSceneFitPass, 5.0f, 1.0f);
        ASSERT_TRUE(rig.Ready());

        const float first = rig.DeclareFrame().PublishedDistance;
        EXPECT_NEAR(first, ExpectedSceneFitDistance(SceneFitReach(5.0f, 1.0f, 0.0f), fade), 1e-3f);
        const float fadeStart = first * (1.0f - fade);

        // Half a metre back: the reach stays short of the fade, so the range holds.
        ASSERT_LT(SceneFitReach(5.0f, 1.0f, -0.5f), fadeStart);
        rig.SetCamera(-0.5f, false);
        EXPECT_EQ(rig.DeclareFrame().PublishedDistance, first) << "a reach inside the band keeps the range";

        // A metre back: the reach would enter the fade, so the range grows.
        ASSERT_GT(SceneFitReach(5.0f, 1.0f, -1.0f), fadeStart);
        rig.SetCamera(-1.0f, false);
        const float grown = rig.DeclareFrame().PublishedDistance;
        EXPECT_NEAR(grown, ExpectedSceneFitDistance(SceneFitReach(5.0f, 1.0f, -1.0f), fade), 1e-3f);

        // Turned away: nothing ahead of the camera, so the previous fit holds.
        rig.SetCamera(-1.0f, true);
        EXPECT_EQ(rig.DeclareFrame().PublishedDistance, grown)
            << "a frame with nothing ahead keeps the previous fit, not the authored 200";
    }
    device->Shutdown();
}

// #1014 — the glass-tint cascade activates on transmissive CASTER presence, not on
// a transmissive material merely being visible to the camera. A non-casting glass
// instance is dropped by the shadow cull (frustum_culling.comp:110), so a tint
// layer declared for it can only clear to the exact texel the 1x1 fallback already
// carries — 64 MiB of array and four passes that cannot change a pixel.
//
// Both arms share one scene shape: same material, same mesh, same light, same
// contributor caster keeping the depth cascades alive. Only WorldSubmissionRecord
// flags bit 0 (castsShadows) differs.
TEST(RenderPipelineDeclareTests, GlassTintDeclaresOnlyForTransmissiveCasters)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    // Declares one CSM pipeline frame with a single transmissive submission and
    // returns how many GlassTint passes the graph scheduled.
    auto tintPassCountForCaster = [&device](bool transmissiveCastsShadows) -> uint32_t
    {
        RenderServices rs;
        EXPECT_TRUE(rs.Initialize(device.get()));
        const CameraId camId = rs.Views().AllocateCamera("TintCam");
        CameraData cd{};
        cd.proj[0] = 1.0f;
        cd.proj[5] = 1.0f;
        cd.proj[10] = 0.001f;
        cd.proj[11] = 1.0f;
        cd.proj[14] = 0.1f;
        for (int i = 0; i < 16; i += 5)
        {
            cd.view[i] = 1.0f;
            cd.viewProj[i] = 1.0f;
        }
        rs.Views().SetCameraData(camId, cd);
        const ViewId viewId = rs.Views().AllocateView("TintView", camId);
        rs.Views().SetViewRenderLayerMask(viewId, 1u);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);

        // Contributor caster: keeps ViewNeedsCascades true in BOTH arms, so the
        // only thing the arms differ by is the transmissive instance's own flag.
        auto depHandle = rs.RegisterDepthEmit(
            [&](DepthEmitContext& ctx, DepthPassType passType)
            {
                if (ctx.ViewId == viewId && passType == DepthPassType::ShadowCascade)
                    ctx.Services->EmitDepthCommand(ctx.ViewId, passType, DrawCommand{});
            });

        Mesh mesh{};
        mesh.Name = "TintTri";
        Vertex v0{}, v1{}, v2{};
        v0.Position[1] = 1.0f;
        v1.Position[0] = -1.0f;
        v1.Position[1] = -1.0f;
        v2.Position[0] = 1.0f;
        v2.Position[1] = -1.0f;
        v0.Normal[2] = 1.0f;
        v1.Normal[2] = 1.0f;
        v2.Normal[2] = 1.0f;
        mesh.Vertices = {v0, v1, v2};
        mesh.Indices = {0, 1, 2};
        const Rendering::MeshGPUHandle meshHandle =
            rs.GetMeshGPURegistry().RegisterSubmesh({GUID::Generate(), 0}, mesh);

        Material glass = Material::TestFactory::Create(GUID::Generate(), "TintGlass", 64u);
        Material::TestFactory::SetGraphicsPipelineId(glass, Rendering::GraphicsPipelineId{1u});
        Material::TestFactory::SetGpuSceneMaterialIndex(glass, 1u);
        Rendering::ShaderVariantKey key{};
        key.materialKeywords = Rendering::MaterialKeyword::Transmission;
        Material::TestFactory::SetVariantKey(glass, key);

        RenderPipelineNodeRegistry registry;
        EXPECT_TRUE(registry.Register(
            "ShadowMap", [] { return std::make_unique<Nodes::ShadowMapNode>(); }, true));
        EXPECT_TRUE(registry.Register(
            "WorldRender", [] { return std::make_unique<Nodes::WorldRenderNode>(); }, true));

        RenderPipelineBlueprint bp;
        bp.pipelineName = "TintTest";
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "CSM";
            p.type = "ShadowMap";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"CSM","type":"ShadowMap","buffer":"ShadowData"})";
            bp.passes.push_back(p);
        }
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "World";
            p.type = "WorldRender";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"World","type":"WorldRender","keywords":["Shadows"]})";
            bp.passes.push_back(p);
        }

        RenderPipelineInstance instance(rs, registry);
        instance.SetBlueprint(bp);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        RenderGraph::RGTexture color =
            frame.ImportPersistentTexture("Tint.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth =
            frame.ImportPersistentTexture("Tint.Depth", DepthTargetDesc());

        rs.BeginWorldDrawFrame();
        WorldSubmissionRecord rec{};
        rec.viewId = viewId;
        rec.meshHandle = meshHandle;
        rec.material = &glass;
        rec.instanceIndex = 0u;
        rec.renderLayerMask = 1u;
        rec.flags = transmissiveCastsShadows ? 1u : 0u; // bit 0 == castsShadows
        rs.SubmitWorldSubmissions(std::span<const WorldSubmissionRecord>(&rec, 1u));
        rs.BuildWorldBatchKeys();

        // Both arms must actually carry the transmissive BATCH KEY — otherwise a
        // zero tint count would only prove the submission never landed.
        EXPECT_EQ(rs.GetEntityBatchKeys(viewId).size(), 1u)
            << "the transmissive submission must reach the batch keys in both arms";
        EXPECT_TRUE(rs.HasTransmissionInView(viewId))
            << "material-presence predicate stays true in both arms (it gates the colour pass)";
        EXPECT_EQ(rs.HasTransmissiveCasterInView(viewId), transmissiveCastsShadows)
            << "caster predicate must follow the instance's castsShadows flag";

        ExtractedLight sun{};
        sun.type = GameEngine::Components::LightType::Directional;
        sun.castsShadows = 1;
        sun.castsLight = 1;
        sun.directionWS[1] = -1.0f;
        sun.cascadeCount = 2;
        rs.SubmitLight(0u, sun);

        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{viewId, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(rs.Views().GetViews().begin(),
                                                     rs.Views().GetViews().end());
        instance.Declare(frame, targets, views);

        // ScheduledOrder is only populated once the graph compiles, which Execute
        // drives — counting before this point reads an empty order and every
        // family looks absent.
        frame.MarkOutput(color);
        frame.Execute();
        device->WaitForIdle();

        uint32_t tintCount = 0;
        uint32_t cascadeCount = 0;
        std::string scheduled;
        const std::string base =
            "Pipeline.TintTest.CSM.View" + std::to_string(static_cast<uint32_t>(viewId));
        for (const RenderGraph::RGPassId p : frame.Graph().ScheduledOrder())
        {
            const std::string name = frame.Graph().PassName(p);
            scheduled += name;
            scheduled += ' ';
            if (RGQuery::Matches(name, RGQuery::Family{"GlassTint"}))
                ++tintCount;
            if (name.rfind(base, 0) == 0 && RGQuery::Matches(name, RGQuery::Family{"Cascade"}))
                ++cascadeCount;
        }
        // The depth cascades are the control: they must be unaffected by the flag,
        // so a zero tint count can never be "the whole shadow family went away".
        EXPECT_EQ(cascadeCount, 2u)
            << "depth cascades must declare in both arms; scheduled: " << scheduled;

        depHandle.Reset();
        rs.Shutdown();
        return tintCount;
    };

    EXPECT_EQ(tintPassCountForCaster(false), 0u)
        << "transmissive material with no shadow-casting instance must declare no tint cascade";
    EXPECT_EQ(tintPassCountForCaster(true), 2u)
        << "a transmissive caster must still get one tint pass per rendered cascade";

    device->Shutdown();
}

// 5d — the SDSM reduce declares only with shadow casters this frame (the old
// exec-time activation predicate, resolved at declaration), over a
// single-sample or a multisampled view depth alike: it reads sample 0 of the
// view's own depth after the world pass, never a resolved prepass copy.
TEST(RenderPipelineDeclareTests, SdsmReduceDeclaresOnlyWithCastersOverAnyDepthSampleCount)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        const CameraId camId = rs.Views().AllocateCamera("SdsmCam");
        CameraData cd{};
        cd.proj[0] = 1.0f;
        cd.proj[5] = 1.0f;
        cd.proj[10] = 0.001f;
        cd.proj[11] = 1.0f;
        cd.proj[14] = 0.1f;
        for (int i = 0; i < 16; i += 5)
        {
            cd.view[i] = 1.0f;
            cd.viewProj[i] = 1.0f;
        }
        rs.Views().SetCameraData(camId, cd);
        const ViewId viewId = rs.Views().AllocateView("SdsmView", camId);
        rs.Views().SetViewRenderLayerMask(viewId, 1u);
        rs.Views().SetViewTargets(viewId, 0, 0, 0, Rendering::ViewClearConfig{});

        // The reduce gate requires the staged depth_reduce.shaderpkg and its
        // multisampled twin. The suite's resolver finds them in the configured
        // build tree, independent of the working directory. Cascade arms can
        // declare without a pipeline, so PassCount() alone does not establish
        // shader readiness. Probe the reduce packages.
        for (const char* package : {"Shaders/depth_reduce.shaderpkg", "Shaders/depth_reduce_ms.shaderpkg"})
        {
            Rendering::ShaderPackage probe{};
            std::string pkgErr;
            if (!Rendering::LoadShaderPkg(package, Rendering::ShaderSourceKind::SpirV, probe, &pkgErr))
                GTEST_SKIP() << package << " unavailable (" << pkgErr << ") — build CompileShaderPkgs";
        }

        auto depHandle = rs.RegisterDepthEmit(
            [&](DepthEmitContext& ctx, DepthPassType passType)
            {
                if (ctx.ViewId == viewId && passType == DepthPassType::ShadowCascade)
                    ctx.Services->EmitDepthCommand(ctx.ViewId, passType, DrawCommand{});
            });

        RenderPipelineNodeRegistry registry;
        ASSERT_TRUE(registry.Register(
            "ShadowMap", [] { return std::make_unique<Nodes::ShadowMapNode>(); },
            true));

        RenderPipelineBlueprint bp;
        bp.pipelineName = "SdsmTest";
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "CSM";
            p.type = "ShadowMap";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"CSM","type":"ShadowMap","buffer":"ShadowData"})";
            bp.passes.push_back(p);
        }

        RenderPipelineInstance instance(rs, registry);
        instance.SetBlueprint(bp);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

        auto countReducePasses = [&]() -> uint32_t
        { return RGQuery::CountDeclared(frame.Graph(), RGQuery::Family{"DepthReduce"}); };

        ExtractedLight sun{};
        sun.type = GameEngine::Components::LightType::Directional;
        sun.castsShadows = 1;
        sun.castsLight = 1;
        sun.directionWS[1] = -1.0f;
        sun.cascadeCount = 2;

        const std::vector<Rendering::ViewDesc> views(rs.Views().GetViews().begin(), rs.Views().GetViews().end());

        // Frame 0: caster + single-sample depth => the reduce declares.
        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("SD.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("SD.Depth", DepthTargetDesc());
        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();
        rs.SubmitLight(0u, sun);
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{viewId, color, depth, {}}};
        instance.Declare(frame, targets, views);
        if (!frame.Graph().PassCount())
            GTEST_SKIP() << "shadow shaders unavailable in this environment";
        EXPECT_EQ(countReducePasses(), 1u) << "caster + 1-sample depth: the reduce declares";
        frame.MarkOutput(color);
        frame.Execute();
        device->WaitForIdle();
        // Stamp + signal the pending so the next declare exercises a resolve.
        // The pendings were keyed by the production node to this frame's
        // identity — the generic post-submit hook stamps them.
        auto* feature = rs.GetFeature<ShadowMapRenderFeature>();
        ASSERT_NE(feature, nullptr);
        feature->OnFrameSubmittedRG(frame, frame.SubmissionToken());

        // Frame 1: no caster => no reduce (the old predicate, at declaration).
        depHandle.Reset();
        frame.BeginFrame(1);
        color = frame.ImportPersistentTexture("SD.Color", ColorTargetDesc());
        depth = frame.ImportPersistentTexture("SD.Depth", DepthTargetDesc());
        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();
        rs.SubmitLight(0u, sun);
        const std::vector<ViewTargetsRG> targets1 = {ViewTargetsRG{viewId, color, depth, {}}};
        instance.Declare(frame, targets1, views);
        EXPECT_EQ(countReducePasses(), 0u) << "no casters: the reduce must not declare";
        frame.MarkOutput(color);
        frame.Execute();
        device->WaitForIdle();

        // Frame 2: caster again over a MULTISAMPLED view depth: the reduce
        // declares, through its multisampled variant.
        auto depHandle2 = rs.RegisterDepthEmit(
            [&](DepthEmitContext& ctx, DepthPassType passType)
            {
                if (ctx.ViewId == viewId && passType == DepthPassType::ShadowCascade)
                    ctx.Services->EmitDepthCommand(ctx.ViewId, passType, DrawCommand{});
            });
        frame.BeginFrame(2);
        color = frame.ImportPersistentTexture("SD.ColorMS", ColorTargetDesc(4));
        depth = frame.ImportPersistentTexture("SD.DepthMS", DepthTargetDesc(4));
        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();
        rs.SubmitLight(0u, sun);
        const std::vector<ViewTargetsRG> targets2 = {ViewTargetsRG{viewId, color, depth, {}}};
        instance.Declare(frame, targets2, views);
        EXPECT_EQ(countReducePasses(), 1u) << "multisampled depth: the reduce declares";
        frame.MarkOutput(color);
        frame.Execute();
        device->WaitForIdle();

        depHandle2.Reset();
        rs.Shutdown();
    }
    device->Shutdown();
}

// The SDSM reduce is a graphics-queue pass that records a DISPATCH:
// depth_reduce.comp samples DepthTex, the view's resolved depth, written by this
// frame's depth pass. RGTextureRead::Sampled resolves to RGStage::FragmentShader
// on a graphics-queue pass, so the barrier out of those depth writes would name
// the fragment stage while the consumer is the reduce dispatch — the depth is
// neither ordered before the dispatch nor made visible to it. Read off the real
// declared graph, so a future pass that samples anything else here is covered
// too.
TEST(RenderPipelineDeclareTests, SdsmDepthReduceDeclaresComputeScopedSampledReads)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        const CameraId camId = rs.Views().AllocateCamera("SdsmScopeCam");
        CameraData cd{};
        cd.proj[0] = 1.0f;
        cd.proj[5] = 1.0f;
        cd.proj[10] = 0.001f;
        cd.proj[11] = 1.0f;
        cd.proj[14] = 0.1f;
        for (int i = 0; i < 16; i += 5)
        {
            cd.view[i] = 1.0f;
            cd.viewProj[i] = 1.0f;
        }
        rs.Views().SetCameraData(camId, cd);
        const ViewId viewId = rs.Views().AllocateView("SdsmScopeView", camId);
        rs.Views().SetViewRenderLayerMask(viewId, 1u);
        rs.Views().SetViewTargets(viewId, 0, 0, 0, Rendering::ViewClearConfig{});

        // The reduce declares only with depth_reduce.shaderpkg resolvable; a
        // missing pkg would leave the pass out and the pin covering nothing.
        {
            Rendering::ShaderPackage probe{};
            std::string pkgErr;
            if (!Rendering::LoadShaderPkg("Shaders/depth_reduce.shaderpkg", Rendering::ShaderSourceKind::SpirV, probe, &pkgErr))
                GTEST_SKIP() << "depth_reduce.shaderpkg unavailable (" << pkgErr
                             << ") — build CompileShaderPkgs";
        }

        auto depHandle = rs.RegisterDepthEmit(
            [&](DepthEmitContext& ctx, DepthPassType passType)
            {
                if (ctx.ViewId == viewId && passType == DepthPassType::ShadowCascade)
                    ctx.Services->EmitDepthCommand(ctx.ViewId, passType, DrawCommand{});
            });

        RenderPipelineNodeRegistry registry;
        ASSERT_TRUE(registry.Register(
            "ShadowMap", [] { return std::make_unique<Nodes::ShadowMapNode>(); }, true));

        RenderPipelineBlueprint bp;
        bp.pipelineName = "SdsmScopeTest";
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "CSM";
            p.type = "ShadowMap";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"CSM","type":"ShadowMap","buffer":"ShadowData"})";
            bp.passes.push_back(p);
        }

        RenderPipelineInstance instance(rs, registry);
        instance.SetBlueprint(bp);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

        ExtractedLight sun{};
        sun.type = GameEngine::Components::LightType::Directional;
        sun.castsShadows = 1;
        sun.castsLight = 1;
        sun.directionWS[1] = -1.0f;
        sun.cascadeCount = 2;

        const std::vector<Rendering::ViewDesc> views(rs.Views().GetViews().begin(),
                                                     rs.Views().GetViews().end());

        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("SDS.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("SDS.Depth", DepthTargetDesc());
        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();
        rs.SubmitLight(0u, sun);
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{viewId, color, depth, {}}};
        instance.Declare(frame, targets, views);
        if (!frame.Graph().PassCount())
            GTEST_SKIP() << "shadow shaders unavailable in this environment";

        const RenderGraph::RGGraph& g = frame.Graph();
        uint32_t checked = 0;
        for (const RenderGraph::RGAccessRecord& a : g.Accesses())
        {
            const char* nm = g.PassName(a.Pass);
            if (!nm || !RGQuery::Matches(nm, RGQuery::Family{"DepthReduce"}))
                continue;
            const bool sampledRead = a.Access == RenderGraph::RGAccess::Sampled ||
                                     a.Access == RenderGraph::RGAccess::SampledCompute ||
                                     a.Access == RenderGraph::RGAccess::SampledVertex;
            if (!sampledRead)
                continue;
            ++checked;
            const uint32_t stage = RenderGraph::MapAccess(a.Access, g.PassQueue(a.Pass)).Stage;
            EXPECT_TRUE(stage & RenderGraph::RGStage::ComputeShader)
                << nm << " samples " << g.ResourceName(a.Resource)
                << " under a barrier scoped to stage mask " << stage
                << ", which omits ComputeShader — depth_reduce.comp reads memory the "
                   "barrier never made visible to it";
        }
        EXPECT_EQ(checked, 1u) << "the reduce's sampled depth read is the pin's subject; "
                                  "found " << checked << " sampled reads on it";

        frame.MarkOutput(color);
        frame.Execute();
        device->WaitForIdle();

        depHandle.Reset();
        rs.Shutdown();
    }
    device->Shutdown();
}

// The cascades are sampled by every surface that writes the view depth, and
// the depth prepass the ShadowMap node follows does not hold them all: the
// ocean writes its depth from its own node, later in the pipeline. SDSM must
// therefore measure the view depth after the nodes declared AFTER ShadowMap
// have written it — here the world pass, the only depth writer in the
// blueprint, kept writable by a draw that writes its own depth — or every
// receiver those nodes draw falls outside the measured range and the boxes
// fitted to it.
TEST(RenderPipelineDeclareTests, SdsmReduceReadsTheViewDepthAfterLaterNodesWriteIt)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        const CameraId camId = rs.Views().AllocateCamera("SdsmLateCam");
        CameraData cd{};
        cd.proj[0] = 1.0f;
        cd.proj[5] = 1.0f;
        cd.proj[10] = 0.001f;
        cd.proj[11] = 1.0f;
        cd.proj[14] = 0.1f;
        for (int i = 0; i < 16; i += 5)
        {
            cd.view[i] = 1.0f;
            cd.viewProj[i] = 1.0f;
        }
        rs.Views().SetCameraData(camId, cd);
        const ViewId viewId = rs.Views().AllocateView("SdsmLateView", camId);
        rs.Views().SetViewRenderLayerMask(viewId, 1u);
        rs.Views().SetViewTargets(viewId, 0, 0, 0, Rendering::ViewClearConfig{});

        {
            Rendering::ShaderPackage probe{};
            std::string pkgErr;
            if (!Rendering::LoadShaderPkg("Shaders/depth_reduce.shaderpkg", Rendering::ShaderSourceKind::SpirV, probe, &pkgErr))
                GTEST_SKIP() << "depth_reduce.shaderpkg unavailable (" << pkgErr
                             << ") — build CompileShaderPkgs";
        }

        auto depHandle = rs.RegisterDepthEmit(
            [&](DepthEmitContext& ctx, DepthPassType passType)
            {
                if (ctx.ViewId == viewId && passType == DepthPassType::ShadowCascade)
                    ctx.Services->EmitDepthCommand(ctx.ViewId, passType, DrawCommand{});
            });

        RenderPipelineNodeRegistry registry;
        ASSERT_TRUE(registry.Register(
            "ShadowMap", [] { return std::make_unique<Nodes::ShadowMapNode>(); }, true));
        ASSERT_TRUE(registry.Register(
            "WorldRender", [] { return std::make_unique<Nodes::WorldRenderNode>(); }, true));

        RenderPipelineBlueprint bp;
        bp.pipelineName = "SdsmLateTest";
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "CSM";
            p.type = "ShadowMap";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"CSM","type":"ShadowMap","buffer":"ShadowData"})";
            bp.passes.push_back(p);
        }
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "World";
            p.type = "WorldRender";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"World","type":"WorldRender","keywords":["Shadows"]})";
            bp.passes.push_back(p);
        }

        RenderPipelineInstance instance(rs, registry);
        instance.SetBlueprint(bp);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

        ExtractedLight sun{};
        sun.type = GameEngine::Components::LightType::Directional;
        sun.castsShadows = 1;
        sun.castsLight = 1;
        sun.directionWS[1] = -1.0f;
        sun.cascadeCount = 2;

        const std::vector<Rendering::ViewDesc> views(rs.Views().GetViews().begin(),
                                                     rs.Views().GetViews().end());

        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("SDL.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("SDL.Depth", DepthTargetDesc());
        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();
        // A forward draw with no prepass head writes its own depth, which keeps
        // the world pass's depth writable (EmitForwardCommand).
        rs.EmitForwardCommand(viewId, DrawCommand{}, ForwardDrawDepth::ColourPass);
        rs.SubmitLight(0u, sun);
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{viewId, color, depth, {}}};
        instance.Declare(frame, targets, views);
        if (!frame.Graph().PassCount())
            GTEST_SKIP() << "shadow shaders unavailable in this environment";
        frame.MarkOutput(color);
        frame.Execute(); // compiles the schedule

        const RenderGraph::RGGraph& g = frame.Graph();
        size_t reduceIndex = SIZE_MAX;
        size_t lastDepthWriter = SIZE_MAX;
        uint32_t depthWriters = 0;
        for (const RenderGraph::RGAccessRecord& a : g.Accesses())
        {
            if (a.Resource != depth.Id)
                continue;
            const size_t index = ScheduledIndexOf(g, a.Pass);
            const char* nm = g.PassName(a.Pass);
            if (nm && RGQuery::Matches(nm, RGQuery::Family{"DepthReduce"}) &&
                a.Access == RenderGraph::RGAccess::SampledCompute)
                reduceIndex = index;
            else if (RenderGraph::IsWrite(a.Access))
            {
                ++depthWriters;
                lastDepthWriter = lastDepthWriter == SIZE_MAX ? index : std::max(lastDepthWriter, index);
            }
        }
        ASSERT_NE(reduceIndex, SIZE_MAX) << "the reduce must sample the view depth";
        ASSERT_GT(depthWriters, 0u) << "the world pass must write the view depth in this blueprint";
        EXPECT_GT(reduceIndex, lastDepthWriter)
            << "the reduce reads the view depth before the world pass declared after the "
               "ShadowMap node has written it";

        device->WaitForIdle();
        depHandle.Reset();
        rs.Shutdown();
    }
    device->Shutdown();
}

namespace
{
// Records what the shadow feature holds for one view when the caster culls are
// scheduled. Registered after ShadowMapRenderFeature, so it runs after that
// feature's own OnScheduleCulling in the same fan-out.
class CullStageCascadeProbe final : public IRenderFeature
{
  public:
    CullStageCascadeProbe(RenderServices& rs, ViewId viewId) : m_Services(rs), m_ViewId(viewId) {}

    void OnScheduleCulling(const FeatureCullingContext& /*ctx*/) override
    {
        const auto* shadow = m_Services.GetFeature<ShadowMapRenderFeature>();
        const CascadeFrameData* fit = shadow ? shadow->GetCachedFrameData(m_ViewId) : nullptr;
        Seen = fit != nullptr;
        if (fit)
            AtCull = *fit;
    }

    bool Seen = false;
    CascadeFrameData AtCull{};

  private:
    RenderServices& m_Services;
    ViewId m_ViewId;
};

CameraData MakeLookCamera(const Mathematics::Vector3& position, const Mathematics::Vector3& target)
{
    CameraData cam{};
    const Mathematics::Matrix4x4 view =
        Mathematics::MakeLookAtLH(position, target, Mathematics::Vector3{0.0f, 1.0f, 0.0f});
    const Mathematics::Matrix4x4 proj =
        Mathematics::MakePerspectiveLH_ZO_ReverseZ(0.8f, 16.0f / 9.0f, 0.5f, 1000.0f);
    const Mathematics::Matrix4x4 viewProj = proj * view;
    std::memcpy(cam.view, view.Data(), sizeof(cam.view));
    std::memcpy(cam.proj, proj.Data(), sizeof(cam.proj));
    std::memcpy(cam.viewProj, viewProj.Data(), sizeof(cam.viewProj));
    cam.cameraPos[0] = position.x;
    cam.cameraPos[1] = position.y;
    cam.cameraPos[2] = position.z;
    return cam;
}
} // namespace

// The cascade caster cull for a frame must run on that frame's cascade fit:
// the casters it keeps are the ones the cascades then rasterize, and a cull
// one fit behind drops the casters at a moving cascade's leading edge. A probe
// feature records the fit the shadow feature holds when the culls are
// scheduled; after the pipeline declared, the cascades' fit must be the same,
// on every frame the camera moves.
TEST(RenderPipelineDeclareTests, CascadeCasterCullRunsOnTheFitTheCascadesRenderWith)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        auto& feature = rs.EnsureFeature<ShadowMapRenderFeature>();
        ASSERT_TRUE(feature.Initialize(device.get(), CascadedShadowConfig{}));

        const CameraId camId = rs.Views().AllocateCamera("CullStageCam");
        const ViewId viewId = rs.Views().AllocateView("CullStageView", camId);
        rs.Views().SetViewRenderLayerMask(viewId, 1u);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);
        auto& probe = rs.EnsureFeature<CullStageCascadeProbe>(rs, viewId);

        RenderPipelineBlueprint bp;
        bp.schemaVersion = 2;
        bp.pipelineName = "CullStageTest";
        bp.sourcePath = "<test>";
        bp.contentHash = 0xC5C0u;
        for (const auto& [id, type, json] :
             {std::tuple<const char*, const char*, const char*>{
                  "CSM", "ShadowMap", R"({"id":"CSM","type":"ShadowMap","buffer":"ShadowData"})"},
              std::tuple<const char*, const char*, const char*>{
                  "World", "WorldRender",
                  R"({"id":"World","type":"WorldRender","keywords":["Shadows"]})"}})
        {
            RenderPipelineBlueprint::Pass p;
            p.id = id;
            p.type = type;
            p.enabled = true;
            p.perView = true;
            p.passJson = json;
            bp.passes.push_back(p);
        }
        RenderPipelineBlueprint::Output output{};
        output.name = Names::Output::FinalColor;
        output.resourceRef = Names::View::Resolve;
        bp.outputs.push_back(output);
        rs.Spine().SetActiveRenderPipelineBlueprint(std::move(bp));

        ExtractedLight sun{};
        sun.type = GameEngine::Components::LightType::Directional;
        sun.castsShadows = 1;
        sun.castsLight = 1;
        sun.directionWS[0] = 0.35f;
        sun.directionWS[1] = -0.65f;
        sun.directionWS[2] = 0.45f;

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        GameEngine::Testing::ScopedPipelineFrame registration(rs.Spine(), frame);

        uint32_t comparedFrames = 0;
        for (uint32_t f = 0; f < 4; ++f)
        {
            // The camera pans 30 m a frame: every cascade's fit moves.
            const Mathematics::Vector3 pivot{30.0f * static_cast<float>(f), 0.0f, 0.0f};
            rs.Views().SetCameraData(camId, MakeLookCamera(pivot + Mathematics::Vector3{0.0f, 40.0f, -48.0f}, pivot));

            rs.BeginWorldDrawFrame();
            rs.BuildWorldBatchKeys();
            rs.SubmitLight(0u, sun);
            frame.BeginFrame(f);
            RenderGraph::RGTexture color = frame.ImportPersistentTexture("CullStage.Color", ColorTargetDesc());
            RenderGraph::RGTexture depth = frame.ImportPersistentTexture("CullStage.Depth", DepthTargetDesc());
            const ViewTargetsRG targets{viewId, color, depth, {}};
            RenderServices::FrameGraphBuildParamsRG params{};
            params.ViewTargets = std::span<const ViewTargetsRG>(&targets, 1);
            probe.Seen = false;
            rs.Spine().BuildFrameGraph(frame, params);
            const CascadeFrameData* declared = feature.GetCachedFrameData(viewId);
            if (!frame.Graph().PassCount() || !declared)
                GTEST_SKIP() << "the pipeline did not declare in this environment";
            if (f > 0)
            {
                // The first frame seeds the view; from then on the cull runs first.
                ASSERT_TRUE(probe.Seen) << "frame " << f << ": no cascade fit when the culls were scheduled";
                ASSERT_GT(declared->NumCascades, 0u);
                for (uint32_t c = 0; c < declared->NumCascades; ++c)
                {
                    EXPECT_EQ(std::memcmp(probe.AtCull.LightVP[c].Data(), declared->LightVP[c].Data(),
                                          16 * sizeof(float)),
                              0)
                        << "frame " << f << " cascade " << c
                        << ": the casters were culled with another fit than the one rendered";
                }
                ++comparedFrames;
            }
            frame.MarkOutput(color);
            frame.Execute();
            device->WaitForIdle();
        }
        EXPECT_EQ(comparedFrames, 3u);
        rs.Shutdown();
    }
    device->Shutdown();
}

// Exercise real ECS volume extraction and the shared feature's GPU declaration:
// volume edits must not overwrite the debug/base filter or leak between worlds.
TEST(RenderPipelineDeclareTests, ShadowVolumeFiltersRestoreBaseAndFollowViewWorlds)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        Rendering::ShaderPackage msmProbe{};
        std::string packageError;
        if (!Rendering::LoadShaderPkg("Shaders/msm_cascade.shaderpkg", Rendering::ShaderSourceKind::SpirV, msmProbe, &packageError))
            GTEST_SKIP() << "MSM package unavailable: " << packageError;

        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        ECS::World worldA(nullptr), worldB(nullptr), emptyWorld(nullptr);
        RenderExtractionSystem extractionA(&rs), extractionB(&rs), extractionEmpty(&rs);
        const std::array<uint64_t, 3> worldIds{
            worldA.GetWorldId(), worldB.GetWorldId(), emptyWorld.GetWorldId()};
        std::array<ViewId, 3> viewIds{};
        std::array<uint32_t, 3> viewWorlds{0u, 1u, 2u};
        const CameraId camera = rs.Views().AllocateCamera("ShadowVolumeCamera");
        CameraData cameraData{};
        cameraData.proj[0] = cameraData.proj[5] = 1.0f;
        cameraData.proj[10] = 0.001f;
        cameraData.proj[11] = 1.0f;
        cameraData.proj[14] = 0.1f;
        for (uint32_t i = 0; i < 16; i += 5)
            cameraData.view[i] = cameraData.viewProj[i] = 1.0f;
        rs.Views().SetCameraData(camera, cameraData);
        for (uint32_t i = 0; i < viewIds.size(); ++i)
        {
            viewIds[i] = rs.Views().AllocateView("ShadowVolumeView", camera);
            rs.Views().SetViewWorldId(viewIds[i], worldIds[i]);
            rs.Views().SetViewRenderLayerMask(viewIds[i], 1u);
            Rendering::ViewClearConfig clear{};
            clear.clearColor = clear.clearDepth = true;
            rs.Views().SetViewTargets(viewIds[i], 0, 0, 0, clear);
        }

        using Q = ShadowFilterQuality;
        using F = Components::DirectionalShadowFilter;
        Components::PostProcessVolume lowerVolume{};
        Components::PostProcessVolume upperVolume{};
        upperVolume.Priority = 10;
        Components::PostProcessVolume otherVolume{};
        Components::ShadowSettingsEffect lowerEffect{};
        lowerEffect.Filter = F::Grid5x5;
        lowerEffect.DistanceFadeFraction = -0.5f;
        Components::ShadowSettingsEffect upperEffect{};
        upperEffect.Filter = F::MSM4;
        upperEffect.DistanceFadeFraction = 1.5f;
        Components::ShadowSettingsEffect otherEffect{};
        otherEffect.Filter = F::PoissonPCF;
        otherEffect.DistanceFadeFraction = 0.25f;
        const auto makeVolume = [](ECS::World& world,
                                   const Components::PostProcessVolume& volume,
                                   const Components::ShadowSettingsEffect& effect)
        {
            auto entity = world.Create();
            entity.Set(Components::WorldTransform{});
            entity.Set(volume);
            entity.Set(effect);
            return entity;
        };
        auto lower = makeVolume(worldA, lowerVolume, lowerEffect);
        auto upper = makeVolume(worldA, upperVolume, upperEffect);
        auto other = makeVolume(worldB, otherVolume, otherEffect);

        auto& feature = rs.EnsureFeature<ShadowMapRenderFeature>();
        feature.SetFilterQuality(Q::Grid3x3);
        auto depthEmitter = rs.RegisterDepthEmit(
            [](DepthEmitContext& ctx, DepthPassType type)
            {
                if (type == DepthPassType::ShadowCascade)
                    ctx.Services->EmitDepthCommand(ctx.ViewId, type, DrawCommand{});
            });

        RenderPipelineNodeRegistry registry;
        ASSERT_TRUE(registry.Register(
            "ShadowMap", [] { return std::make_unique<Nodes::ShadowMapNode>(); }, true));
        ASSERT_TRUE(registry.Register(
            "WorldRender", [] { return std::make_unique<Nodes::WorldRenderNode>(); }, true));
        RenderPipelineBlueprint blueprint;
        blueprint.pipelineName = "ShadowVolumes";
        RenderPipelineBlueprint::Pass shadows;
        shadows.id = "Shadows";
        shadows.type = "ShadowMap";
        shadows.enabled = shadows.perView = true;
        shadows.passJson = R"({"id":"Shadows","type":"ShadowMap","cascades":2,"resolution":128,"momentsResolution":512,"buffer":"ShadowData"})";
        blueprint.passes.push_back(shadows);
        RenderPipelineBlueprint::Pass worldPass;
        worldPass.id = "World";
        worldPass.type = "WorldRender";
        worldPass.enabled = worldPass.perView = true;
        worldPass.passJson = R"({"id":"World","type":"WorldRender","keywords":["Shadows"]})";
        blueprint.passes.push_back(worldPass);
        RenderPipelineInstance instance(rs, registry);
        instance.SetBlueprint(blueprint);
        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        ExtractedLight sun{};
        sun.type = Components::LightType::Directional;
        sun.castsShadows = sun.castsLight = 1;
        sun.directionWS[1] = -1.0f;
        sun.cascadeCount = 2;

        uint32_t frameIndex = 0;
        const bool rawDepthSupported = rs.Textures().IsBindlessEnabled() &&
                                      !device->GetCapabilities().prefersStableShadowFiltering;
        const auto effectiveQuality = [rawDepthSupported](Q requested, bool hadMoments)
        {
            if (requested == Q::MSM4 && !hadMoments)
                requested = Q::PCSS;
            if (!rawDepthSupported && (requested == Q::PCSS || requested == Q::DPCF))
                requested = Q::PoissonPCF;
            return requested;
        };
        const auto runFrame = [&](std::optional<Q> expectedA, std::optional<Q> expectedB, Q base)
        {
            SCOPED_TRACE(frameIndex);
            frame.BeginFrame(frameIndex++);
            rs.BeginWorldDrawFrame();
            worldA.ProcessCommands();
            worldB.ProcessCommands();
            emptyWorld.ProcessCommands();
            extractionA.Update(worldA, 1.0f / 60.0f);
            extractionB.Update(worldB, 1.0f / 60.0f);
            extractionEmpty.Update(emptyWorld, 1.0f / 60.0f);
            worldA.SwapComponentDirtyFeed();
            worldB.SwapComponentDirtyFeed();
            emptyWorld.SwapComponentDirtyFeed();

            const std::array<std::optional<Q>, 3> overrides{expectedA, expectedB, std::nullopt};
            const std::array<float, 3> expectedFade{
                expectedA ? (*expectedA == Q::Grid5x5 ? 0.0f : 0.5f) : 0.1f,
                expectedB ? 0.25f : 0.1f, 0.1f};
            for (uint32_t i = 0; i < worldIds.size(); ++i)
            {
                const auto& settings = rs.GetWorldShadowSettings(worldIds[i]);
                ASSERT_EQ(settings.HasOverride, overrides[i].has_value()) << "world " << i;
                EXPECT_FLOAT_EQ(settings.DistanceFadeFraction, expectedFade[i]) << "world " << i;
                if (overrides[i])
                    ASSERT_EQ(static_cast<Q>(settings.Filter), *overrides[i]) << "world " << i;
                rs.SubmitLight(worldIds[i], sun);
            }
            rs.BuildWorldBatchKeys();
            ASSERT_EQ(feature.GetFilterQuality(), base);
            // Invalid views must not accidentally inherit a valid world-0 override.
            constexpr ViewId invalidView = 0xFFFFFFFFu;
            ASSERT_EQ(rs.Views().FindViewDesc(invalidView), nullptr);
            ResolvedShadowSettings worldZero{};
            worldZero.HasOverride = true;
            worldZero.Filter = F::MSM4;
            rs.SetWorldShadowSettings(0u, worldZero);
            EXPECT_EQ(feature.ResolveRequestedFilterQuality(rs, invalidView), base);

            std::array<Q, 3> requested{};
            std::array<Q, 3> shaderQuality{};
            std::vector<ViewTargetsRG> targets;
            for (uint32_t i = 0; i < viewIds.size(); ++i)
            {
                const ViewId view = viewIds[i];
                rs.Views().SetViewWorldId(view, worldIds[viewWorlds[i]]);
                requested[i] = overrides[viewWorlds[i]].value_or(base);
                // ShadowData precedes lazy MSM allocation in the existing node:
                // a first MSM frame falls back, then the next frame uses MSM.
                shaderQuality[i] = effectiveQuality(
                    requested[i], feature.GetMsmMomentsTexture(view).IsValid());
                const std::string suffix = std::to_string(i);
                auto color = frame.ImportPersistentTexture(
                    ("ShadowVolumes.Color" + suffix).c_str(), ColorTargetDesc());
                auto depth = frame.ImportPersistentTexture(
                    ("ShadowVolumes.Depth" + suffix).c_str(), DepthTargetDesc());
                targets.push_back({view, color, depth, {}});
            }
            // Reverse declaration order on alternating frames as well as moving
            // views between worlds: no "last declared world" may own the filter.
            if (frameIndex % 2u == 0u)
                std::reverse(targets.begin(), targets.end());
            const std::vector<Rendering::ViewDesc> views(
                rs.Views().GetViews().begin(), rs.Views().GetViews().end());
            instance.Declare(frame, targets, views);
            ASSERT_TRUE(feature.IsInitialized());
            ASSERT_GT(frame.Graph().PassCount(), 0u);
            EXPECT_EQ(feature.GetFilterQuality(), base)
                << "volume declaration must not write the explicit base selection";
            for (uint32_t i = 0; i < viewIds.size(); ++i)
            {
                const ViewId view = viewIds[i];
                EXPECT_EQ(feature.ResolveRequestedFilterQuality(rs, view), requested[i]);
                EXPECT_EQ(feature.ResolveEffectiveFilterQuality(rs, view),
                          effectiveQuality(requested[i], requested[i] == Q::MSM4));
                const std::string msmName = "Pipeline.ShadowVolumes.Shadows.View" +
                                           std::to_string(static_cast<uint32_t>(view)) +
                                           ".MsmCascade_Compute";
                EXPECT_EQ(RGQuery::CountDeclared(frame.Graph(), RGQuery::Subtree{msmName}),
                          requested[i] == Q::MSM4 ? 2u : 0u);
                EXPECT_EQ(feature.GetMsmMomentsTexture(view).IsValid(), requested[i] == Q::MSM4)
                    << "only this view's selected MSM filter owns a moments array";
            }
            for (const auto& target : targets)
                frame.MarkOutput(target.Color);
            frame.Execute();
            device->WaitForIdle();

            // Inspect the actual per-view upload after Execute has flushed it;
            // rebuilding ShadowData here would hide an incorrect declaration.
            const auto* resources = instance.FrameResourcesFor(&frame);
            ASSERT_NE(resources, nullptr);
            for (uint32_t i = 0; i < viewIds.size(); ++i)
            {
                const auto binding = resources->Buffers.find({viewIds[i], "ShadowData"});
                ASSERT_NE(binding, resources->Buffers.end());
                ASSERT_EQ(binding->second.Size, sizeof(ShadowDataGPU));
                const auto* mapped = static_cast<const uint8_t*>(
                    device->MapBuffer(binding->second.Buffer));
                ASSERT_NE(mapped, nullptr);
                ShadowDataGPU data{};
                std::memcpy(&data, mapped + binding->second.Offset, sizeof(data));
                device->UnmapBuffer(binding->second.Buffer);
                EXPECT_FLOAT_EQ(data.shadowFilterParams[1], expectedFade[viewWorlds[i]])
                    << "authored fade for view " << i;
                EXPECT_EQ(data.shadowDebug[3], static_cast<float>(shaderQuality[i]))
                    << "GPU filter for view " << i;
                EXPECT_EQ(data.shadowPcssPyramid[0][1],
                          shaderQuality[i] == Q::PCSS ? ExpectedPyramidLevels(feature, viewIds[i])
                                                      : 0.0f);
            }
        };

        ASSERT_NO_FATAL_FAILURE(runFrame(Q::MSM4, Q::PoissonPCF, Q::Grid3x3));
        ASSERT_NO_FATAL_FAILURE(runFrame(Q::MSM4, Q::PoissonPCF, Q::Grid3x3));
        feature.SetPcfQuality(static_cast<int>(Q::PoissonPCF));
        ASSERT_NO_FATAL_FAILURE(runFrame(Q::MSM4, Q::PoissonPCF, Q::PoissonPCF));
        upperEffect.Filter = F::DilatedPCF;
        upper.Set(upperEffect);
        ASSERT_NO_FATAL_FAILURE(runFrame(Q::DPCF, Q::PoissonPCF, Q::PoissonPCF));
        upperEffect.Enabled = false;
        upper.Set(upperEffect);
        ASSERT_NO_FATAL_FAILURE(runFrame(Q::Grid5x5, Q::PoissonPCF, Q::PoissonPCF));
        lower.SetEnabled<Components::PostProcessVolume>(false);
        ASSERT_NO_FATAL_FAILURE(runFrame(std::nullopt, Q::PoissonPCF, Q::PoissonPCF));
        lower.SetEnabled<Components::PostProcessVolume>(true);
        upperEffect.Enabled = true;
        upperEffect.Filter = F::MSM4;
        upper.Set(upperEffect);
        ASSERT_NO_FATAL_FAILURE(runFrame(Q::MSM4, Q::PoissonPCF, Q::PoissonPCF));
        upper.Remove<Components::ShadowSettingsEffect>();
        ASSERT_NO_FATAL_FAILURE(runFrame(Q::Grid5x5, Q::PoissonPCF, Q::PoissonPCF));
        lower.Destroy();
        ASSERT_NO_FATAL_FAILURE(runFrame(std::nullopt, Q::PoissonPCF, Q::PoissonPCF));
        upper.Set(upperEffect);
        ASSERT_NO_FATAL_FAILURE(runFrame(Q::MSM4, Q::PoissonPCF, Q::PoissonPCF));
        std::swap(viewWorlds[0], viewWorlds[1]);
        ASSERT_NO_FATAL_FAILURE(runFrame(Q::MSM4, Q::PoissonPCF, Q::PoissonPCF));
        std::swap(viewWorlds[0], viewWorlds[1]);
        ASSERT_NO_FATAL_FAILURE(runFrame(Q::MSM4, Q::PoissonPCF, Q::PoissonPCF));
        upper.Destroy();
        ASSERT_NO_FATAL_FAILURE(runFrame(std::nullopt, Q::PoissonPCF, Q::PoissonPCF));
        feature.SetPcssEnabled(true);
        ASSERT_NO_FATAL_FAILURE(runFrame(std::nullopt, Q::PoissonPCF, Q::PCSS));
        otherVolume.Weight = 0.0f;
        other.Set(otherVolume);
        ASSERT_NO_FATAL_FAILURE(runFrame(std::nullopt, std::nullopt, Q::PCSS));
        feature.SetPcssEnabled(false);
        ASSERT_NO_FATAL_FAILURE(runFrame(std::nullopt, std::nullopt, Q::Grid5x5));

        depthEmitter.Reset();
        rs.Shutdown();
    }
    device->Shutdown();
}


// 5e — the MSM moments chain declares only when MSM4 is the live filter:
// one compute write per cascade onto per-layer ranges of ONE imported
// moments id (handle-dedup shared with the world arm's read), each reading
// the cascade arm's pooled depth array. PCSS frames declare none.
TEST(RenderPipelineDeclareTests, MsmDeclaresPerCascadeWritesOnlyWhenMsm4Active)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        const CameraId camId = rs.Views().AllocateCamera("MsmCam");
        CameraData cd{};
        cd.proj[0] = 1.0f;
        cd.proj[5] = 1.0f;
        cd.proj[10] = 0.001f;
        cd.proj[11] = 1.0f;
        cd.proj[14] = 0.1f;
        for (int i = 0; i < 16; i += 5)
        {
            cd.view[i] = 1.0f;
            cd.viewProj[i] = 1.0f;
        }
        rs.Views().SetCameraData(camId, cd);
        const ViewId viewId = rs.Views().AllocateView("MsmView", camId);
        rs.Views().SetViewRenderLayerMask(viewId, 1u);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);

        auto depHandle = rs.RegisterDepthEmit(
            [&](DepthEmitContext& ctx, DepthPassType passType)
            {
                if (ctx.ViewId == viewId && passType == DepthPassType::ShadowCascade)
                    ctx.Services->EmitDepthCommand(ctx.ViewId, passType, DrawCommand{});
            });

        RenderPipelineNodeRegistry registry;
        ASSERT_TRUE(registry.Register(
            "ShadowMap", [] { return std::make_unique<Nodes::ShadowMapNode>(); },
            true));
        ASSERT_TRUE(registry.Register(
            "WorldRender", [] { return std::make_unique<Nodes::WorldRenderNode>(); }, true));

        RenderPipelineBlueprint bp;
        bp.pipelineName = "MsmTest";
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "CSM";
            p.type = "ShadowMap";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"CSM","type":"ShadowMap","buffer":"ShadowData"})";
            bp.passes.push_back(p);
        }
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "World";
            p.type = "WorldRender";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"World","type":"WorldRender","keywords":["Shadows"]})";
            bp.passes.push_back(p);
        }

        RenderPipelineInstance instance(rs, registry);
        instance.SetBlueprint(bp);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

        ExtractedLight sun{};
        sun.type = GameEngine::Components::LightType::Directional;
        sun.castsShadows = 1;
        sun.castsLight = 1;
        sun.directionWS[1] = -1.0f;
        sun.cascadeCount = 2;

        const std::vector<Rendering::ViewDesc> views(rs.Views().GetViews().begin(), rs.Views().GetViews().end());

        auto countMsmPasses = [&]() -> uint32_t
        { return RGQuery::CountDeclared(frame.Graph(), RGQuery::Family{"MsmCascade_Compute"}); };

        // Frame 0 (PCSS default): no MSM writes.
        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("MSM.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("MSM.Depth", DepthTargetDesc());
        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();
        rs.SubmitLight(0u, sun);
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{viewId, color, depth, {}}};
        instance.Declare(frame, targets, views);
        if (!frame.Graph().PassCount())
            GTEST_SKIP() << "shadow shaders unavailable in this environment";
        EXPECT_EQ(countMsmPasses(), 0u) << "PCSS frames declare no moments writes";
        frame.MarkOutput(color);
        frame.Execute();
        device->WaitForIdle();

        auto* feature = rs.GetFeature<ShadowMapRenderFeature>();
        ASSERT_NE(feature, nullptr);
        feature->SetFilterQuality(ShadowFilterQuality::MSM4);

        // Frame 1 (MSM4): one compute write per cascade, on per-layer ranges
        // of ONE moments id, each chaining from the pooled depth array.
        frame.BeginFrame(1);
        color = frame.ImportPersistentTexture("MSM.Color", ColorTargetDesc());
        depth = frame.ImportPersistentTexture("MSM.Depth", DepthTargetDesc());
        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();
        rs.SubmitLight(0u, sun);
        const std::vector<ViewTargetsRG> targets1 = {ViewTargetsRG{viewId, color, depth, {}}};
        instance.Declare(frame, targets1, views);
        // Skip on the CAUSE (the package genuinely absent), never on the
        // SYMPTOM (a zero count): a zero-count skip also fires when the
        // family is renamed or the emitter breaks, turning a regression into
        // a silent skip.
        {
            Rendering::ShaderPackage msmProbe{};
            std::string pkgErr;
            if (!Rendering::LoadShaderPkg("Shaders/msm_cascade.shaderpkg", Rendering::ShaderSourceKind::SpirV, msmProbe, &pkgErr))
                GTEST_SKIP() << "msm_cascade.shaderpkg unavailable (" << pkgErr
                             << ") — build CompileShaderPkgs";
        }
        EXPECT_EQ(countMsmPasses(), 2u) << "one moments write per cached cascade";

        const RenderGraph::RGTexture depthArr = rs.GetShadowMapArrayRG(frame, viewId);
        ASSERT_TRUE(depthArr.IsValid()) << "consume-only accessor sees the cascade arm's import";
        for (const RenderGraph::RGPassId p :
             RGQuery::DeclaredIds(frame.Graph(), RGQuery::Family{"MsmCascade_Compute"}))
            EXPECT_TRUE(frame.Graph().HasReadAccess(p, depthArr.Id))
                << "each moments write reads this frame's cascade depth";

        // 5e hardening pin (WorldReadsMomentsImportWhenMsmActive): the node's
        // moments import and the world arm's fallback land on ONE deduped id —
        // a re-import of the same physical returns the EXISTING resource
        // (first-import-wins contract), the world pass READS that id (the only
        // ordering/transition edge for the raw ge_shadowMomentsArray bind),
        // and every cascade compute WRITES the same id.
        const auto momentsPhys = feature->GetMsmMomentsTexture(viewId);
        ASSERT_TRUE(momentsPhys.IsValid());
        const RenderGraph::RGTexture moments = frame.ImportExternalTexture(
            "MsmMoments.DedupProbe", momentsPhys, Rendering::ResourceState::ShaderResource,
            Rendering::TextureFormat::R16G16B16A16_UNORM, 1,
            feature->GetConfig().NumCascades);
        const std::vector<RenderGraph::RGPassId> worldPassIds =
            RGQuery::DeclaredIds(frame.Graph(), RGQuery::Subtree{"RenderEntities"});
        ASSERT_EQ(worldPassIds.size(), 1u)
            << "exactly one world pass declares (view clears are set; no transmissive "
               "casters => no Transmissive/phase-B variants); declared: ["
            << RGQuery::DeclaredNames(frame.Graph(), RGQuery::Subtree{"RenderEntities"}) << "]";
        const RenderGraph::RGPassId momentsWorldPass = worldPassIds.front();
        EXPECT_TRUE(frame.Graph().HasReadAccess(momentsWorldPass, moments.Id))
            << "the world pass reads the deduped moments import";
        for (const RenderGraph::RGPassId p :
             RGQuery::DeclaredIds(frame.Graph(), RGQuery::Family{"MsmCascade_Compute"}))
            EXPECT_TRUE(frame.Graph().HasWriteAccess(p, moments.Id))
                << "each cascade compute writes the SAME deduped moments id";

        frame.MarkOutput(color);
        frame.Execute();
        device->WaitForIdle();

        for (const RenderGraph::RGPassId p :
             RGQuery::DeclaredIds(frame.Graph(), RGQuery::Family{"MsmCascade_Compute"}))
            EXPECT_LT(ScheduledIndexOf(frame.Graph(), p),
                      ScheduledIndexOf(frame.Graph(), momentsWorldPass))
                << "moments writes schedule before the world's sampled read";

        // Frame 2 (5f): thumbnails on — the overlay LATE-declares (after the
        // node loop), attaches the chain's final output with Load, reads the
        // cascade array, and schedules AFTER the world pass.
        feature->SetFilterQuality(ShadowFilterQuality::PCSS);
        feature->SetShowThumbnails(true);
        frame.BeginFrame(2);
        color = frame.ImportPersistentTexture("MSM.Color", ColorTargetDesc());
        depth = frame.ImportPersistentTexture("MSM.Depth", DepthTargetDesc());
        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();
        rs.SubmitLight(0u, sun);
        const std::vector<ViewTargetsRG> targets2 = {ViewTargetsRG{viewId, color, depth, {}}};
        instance.Declare(frame, targets2, views);

        // Family, not substring: the shipped ForwardPlus blueprints carry a
        // ClusterDebugOverlay node whose name CONTAINS "DebugOverlay" — a
        // substring needle in a fixture that ever grows that node would pin
        // the wrong pass.
        const RenderGraph::RGPassId overlayPass =
            RGQuery::FindDeclared(frame.Graph(), RGQuery::Family{"DebugOverlay"});
        if (overlayPass != RenderGraph::kInvalidId)
        {
            const RenderGraph::RGTexture arr2 = rs.GetShadowMapArrayRG(frame, viewId);
            ASSERT_TRUE(arr2.IsValid());
            EXPECT_TRUE(frame.Graph().HasReadAccess(overlayPass, arr2.Id))
                << "the overlay samples the cascade array";
            // Attaches the FINAL output (here: the caller color — no chain).
            bool attachesFinal = false;
            for (const auto& rec : frame.Attachments())
                if (rec.Pass == overlayPass && rec.Tex == color.Id &&
                    rec.Ops.Load == RenderGraph::RGLoadOp::Load)
                    attachesFinal = true;
            EXPECT_TRUE(attachesFinal) << "overlay composites over the chain's final output";

            frame.MarkOutput(color);
            frame.Execute();
            device->WaitForIdle();

            // World pass before the overlay in the schedule.
            RenderGraph::RGPassId worldPass = 0;
            bool foundWorld = false;
            for (const auto& rec : frame.Attachments())
                if (rec.Tex == color.Id && !rec.IsDepth && rec.Pass != overlayPass)
                {
                    worldPass = rec.Pass;
                    foundWorld = true;
                }
            ASSERT_TRUE(foundWorld);
            EXPECT_LT(ScheduledIndexOf(frame.Graph(), worldPass),
                      ScheduledIndexOf(frame.Graph(), overlayPass))
                << "the late declare orders the overlay after the world's write";
        }
        else
        {
            // Overlay shaders unavailable headless — the gate declined; the
            // declare-time absence is still the no-stale-overlay contract.
            frame.MarkOutput(color);
            frame.Execute();
            device->WaitForIdle();
        }

        depHandle.Reset();
        rs.Shutdown();
    }
    device->Shutdown();
}

// 5d — the feature's token-gated SDSM readback rides RGReadbackRing:
// un-stamped pendings never resolve; a foreign frame stream's submit stamps
// nothing; the declaring frame's submit stamps its pendings and resolve is
// newest-wins (dropping everything older); a re-begun frame's dead declare
// is dropped at stamp time, never adopted.
TEST(RenderPipelineDeclareTests, SdsmResolveConsumesNewestSignaledAndDropsOlder)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        auto& feature = rs.EnsureFeature<ShadowMapRenderFeature>();
        CascadedShadowConfig cfg{};
        ASSERT_TRUE(feature.Initialize(device.get(), cfg));
        const ViewId viewId = 7u;

        // The depth words of the slot's two result sides (the min side, then
        // the max side, ShadowReceiverMeasurement::kWordsPerSide words each).
        auto writeSlot = [&](Rendering::BufferHandle buf, float minNdc, float maxNdc)
        {
            void* mapped = device->MapBuffer(buf);
            ASSERT_NE(mapped, nullptr);
            auto* words = static_cast<uint8_t*>(mapped);
            std::memcpy(words, &minNdc, sizeof(float));
            std::memcpy(words + ShadowReceiverMeasurement::kWordsPerSide * sizeof(uint32_t), &maxNdc,
                        sizeof(float));
            device->UnmapBuffer(buf);
        };

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);

        // Two pendings, distinct payloads, both keyed to `frame`'s identity.
        // Each carries the context its reduce measured under.
        ShadowReceiverMeasurement::Context contextA{};
        contextA.BinFar = 100.0f;
        ShadowReceiverMeasurement::Context contextB{};
        contextB.BinFar = 200.0f;
        const auto slotA = feature.AcquireSdsmSlotRG(frame, viewId, contextA);
        const auto slotB = feature.AcquireSdsmSlotRG(frame, viewId, contextB);
        ASSERT_TRUE(slotA.IsValid());
        ASSERT_TRUE(slotB.IsValid());
        ASSERT_NE(slotA.id, slotB.id) << "ring slots rotate";
        writeSlot(slotA, 0.1f, 0.9f);
        writeSlot(slotB, 0.2f, 0.8f);

        ShadowReceiverReadback readback{};
        EXPECT_FALSE(feature.TryResolveSdsmRG(viewId, readback))
            << "un-stamped pendings must never resolve";

        // Execute a trivial pass so the frame's SubmissionToken is real + signaled.
        RenderGraph::RGTexture t = frame.ImportPersistentTexture("SR.T", ColorTargetDesc());
        frame.AddPass(
            "SR.Touch", 0,
            [&](RenderGraph::RGPassBuilder& p)
            {
                RenderGraph::RGAttachmentOps ops{};
                ops.Load = RenderGraph::RGLoadOp::Clear;
                p.AttachColor(0, t, ops);
            },
            [](RenderGraph::RGContext&) {});
        frame.MarkOutput(t);
        frame.Execute();
        device->WaitForIdle();

        // Frame-identity pin: a DIFFERENT stream's submit must not stamp
        // these pendings (a foreign token would lie about when the GPU
        // finished writing these slots).
        FramePools foreignPools(device.get());
        RenderGraph::RGFrame foreign(device.get(), &foreignPools.Persistent,
                                     &foreignPools.Transient, &foreignPools.Ring);
        foreign.BeginFrame(0);
        feature.OnFrameSubmittedRG(foreign, frame.SubmissionToken());
        EXPECT_FALSE(feature.TryResolveSdsmRG(viewId, readback))
            << "a foreign frame stream's submit must not stamp these pendings";

        feature.OnFrameSubmittedRG(frame, frame.SubmissionToken());

        // Newest signaled wins; both pendings consumed.
        float minNdc = 0.0f, maxNdc = 0.0f;
        ASSERT_TRUE(feature.TryResolveSdsmRG(viewId, readback));
        std::memcpy(&minNdc, &readback.MinWords[0], sizeof(float));
        std::memcpy(&maxNdc, &readback.MaxWords[0], sizeof(float));
        EXPECT_FLOAT_EQ(minNdc, 0.2f) << "the NEWEST pending's payload";
        EXPECT_FLOAT_EQ(maxNdc, 0.8f);
        EXPECT_FLOAT_EQ(readback.Measured.BinFar, 200.0f)
            << "the newest pending's measuring context travels with it";
        EXPECT_FALSE(feature.TryResolveSdsmRG(viewId, readback))
            << "resolve drops the resolved pending AND everything older";

        // Dead declare: a pending whose frame was RE-BEGUN before its stamp
        // died with the old graph (its dispatch never recorded) — the stamp
        // drops it; it must never adopt the new incarnation's token.
        const auto slotC = feature.AcquireSdsmSlotRG(frame, viewId, contextA);
        ASSERT_TRUE(slotC.IsValid());
        writeSlot(slotC, 0.3f, 0.7f);
        frame.BeginFrame(1); // re-begin: slotC's declare is dead
        feature.OnFrameSubmittedRG(frame, frame.SubmissionToken());
        EXPECT_FALSE(feature.TryResolveSdsmRG(viewId, readback))
            << "a re-begun frame's dead pendings are dropped, never stamped";

        rs.Shutdown();
    }
    device->Shutdown();
}

// The exposure readback rides the same ring contract as SDSM (covered above);
// this covers what the feature adds ON TOP of the ring: the valid-flag gate
// (a zero-initialized slot must not resolve as "scale 0") and last-good-value
// latching (consumed pendings keep answering until fresher data lands).
TEST(RenderPipelineDeclareTests, ExposureReadbackGatesOnValidAndLatchesLastGoodScale)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        auto& feature = rs.EnsureFeature<ExposureReadbackFeature>();
        const ViewId viewId = 7u;

        EXPECT_FALSE(feature.IsReadbackEnabled(viewId));
        feature.SetReadbackEnabled(viewId, true);
        EXPECT_TRUE(feature.IsReadbackEnabled(viewId));

        auto writeSlot = [&](Rendering::BufferHandle buf, float scale, uint32_t valid)
        {
            void* mapped = device->MapBuffer(buf);
            ASSERT_NE(mapped, nullptr);
            std::memcpy(mapped, &scale, sizeof(float));
            std::memcpy(static_cast<uint8_t*>(mapped) + sizeof(float), &valid, sizeof(uint32_t));
            device->UnmapBuffer(buf);
        };

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(7);

        const auto slotA = feature.AcquireSlotRG(device.get(), frame, viewId);
        const auto slotB = feature.AcquireSlotRG(device.get(), frame, viewId);
        ASSERT_TRUE(slotA.IsValid());
        ASSERT_TRUE(slotB.IsValid());
        ASSERT_NE(slotA.id, slotB.id) << "ring slots rotate";
        writeSlot(slotA, 2.0f, 1u);
        writeSlot(slotB, 4.0f, 1u);

        float scale = 123.0f;
        uint64_t sampleFrame = 456;
        EXPECT_FALSE(feature.TryResolveAdaptedExposure(viewId, scale, &sampleFrame))
            << "un-stamped pendings must never resolve";
        EXPECT_FLOAT_EQ(scale, 123.0f);
        EXPECT_EQ(sampleFrame, 456u);

        // Each tagged frame gets a real, signaled submission token.
        auto submit = [&]
        {
            RenderGraph::RGTexture t = frame.ImportPersistentTexture("ER.T", ColorTargetDesc());
            frame.AddPass(
                "ER.Touch", 0,
                [&](RenderGraph::RGPassBuilder& p)
                {
                    RenderGraph::RGAttachmentOps ops{};
                    ops.Load = RenderGraph::RGLoadOp::Clear;
                    p.AttachColor(0, t, ops);
                },
                [](RenderGraph::RGContext&) {});
            frame.MarkOutput(t);
            frame.Execute();
            device->WaitForIdle();
            feature.OnFrameSubmittedRG(frame, frame.SubmissionToken());
        };
        submit();

        ASSERT_TRUE(feature.TryResolveAdaptedExposure(viewId, scale, &sampleFrame));
        EXPECT_EQ(sampleFrame, 7u);
        EXPECT_FLOAT_EQ(scale, 4.0f) << "the NEWEST pending's payload";

        scale = 0.0f;
        EXPECT_TRUE(feature.TryResolveAdaptedExposure(viewId, scale, &sampleFrame))
            << "the last good value latches between ring arrivals";
        EXPECT_EQ(sampleFrame, 7u);
        EXPECT_FLOAT_EQ(scale, 4.0f);

        // A slot whose valid flag is unset (zero-initialized state: the view
        // metered for less than a ring revolution) must not disturb the latch.
        frame.BeginFrame(8);
        const auto slotC = feature.AcquireSlotRG(device.get(), frame, viewId);
        ASSERT_TRUE(slotC.IsValid());
        writeSlot(slotC, 8.0f, 0u);
        submit();
        scale = 0.0f;
        EXPECT_TRUE(feature.TryResolveAdaptedExposure(viewId, scale, &sampleFrame));
        EXPECT_EQ(sampleFrame, 7u);
        EXPECT_FLOAT_EQ(scale, 4.0f) << "an invalid state must not overwrite the latch";

        feature.SetReadbackEnabled(viewId, false);
        EXPECT_FALSE(feature.IsReadbackEnabled(viewId));
        scale = 123.0f;
        sampleFrame = 456;
        EXPECT_FALSE(feature.TryResolveAdaptedExposure(viewId, scale, &sampleFrame));
        EXPECT_FLOAT_EQ(scale, 123.0f);
        EXPECT_EQ(sampleFrame, 456u);

        rs.Shutdown();
    }
    device->Shutdown();
}

TEST(RenderPipelineDeclareTests, SpineIsIdempotentPerFrameAndMarksOutput)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        GameEngine::Testing::PinWorldOnlyPipeline(rs.Spine());
        const CameraId camId = rs.Views().AllocateCamera("SpineCam");
        CameraData cd{};
        for (int i = 0; i < 16; i += 5)
        {
            cd.view[i] = 1.0f;
            cd.proj[i] = 1.0f;
            cd.viewProj[i] = 1.0f;
        }
        rs.Views().SetCameraData(camId, cd);
        const ViewId viewId = rs.Views().AllocateView("SpineView", camId);
        rs.Views().SetViewRenderLayerMask(viewId, 1u);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);

        // One GPUScene instance so the GPU-driven stages (culling → bucketer
        // → union/aggregate) don't all vacuously early-out at instanceCount
        // 0 — those are exactly the stages without per-stage repeat guards.
        ASSERT_NE(rs.GetGPUScene(), nullptr);
        Rendering::GPUInstance inst{};
        inst.boundingRadius = 1.0f;
        inst.flags = ~0u;
        rs.GetGPUScene()->AddInstance(inst);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        GameEngine::Testing::ScopedPipelineFrame frameRegistration(rs.Spine(), frame);
        frame.BeginFrame(0);

        RenderGraph::RGTexture color = frame.ImportPersistentTexture("Spine.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("Spine.Depth", DepthTargetDesc());

        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();

        const ViewTargetsRG vt{viewId, color, depth, {}};
        RenderServices::FrameGraphBuildParamsRG params{};
        params.ViewTargets = std::span<const ViewTargetsRG>(&vt, 1);

        rs.Spine().BuildFrameGraph(frame, params);
        const size_t passesAfterFirst = frame.Graph().PassCount();
        EXPECT_GT(passesAfterFirst, 0u)
            << "the pinned pipeline's WorldRender node must declare the world pass";

        // Spine step 7: the view's FinalColor (the pinned blueprint resolves
        // View.Resolve → the color target) is an anchored external sink.
        EXPECT_TRUE(frame.Graph().IsExternal(color.Id))
            << "BuildFrameGraph must MarkOutput the view's FinalColor";

        // Second spine call in the same frame: the whole-spine guard makes it
        // a no-op — zero new passes (the bucketer/union/aggregate stages have
        // no per-stage guards; only the top-of-spine early-return protects
        // them from declaring duplicate dispatches).
        rs.Spine().BuildFrameGraph(frame, params);
        EXPECT_EQ(frame.Graph().PassCount(), passesAfterFirst)
            << "the spine must be idempotent per frame";

        frame.Execute();
        device->WaitForIdle();

        rs.Shutdown();
    }
    device->Shutdown();
}

// 8e-3 quad: ONE spine call carries four views (perspective + three ortho
// panes). Every view must get its own world pass and an externally anchored
// FinalColor, and the F4 consumer accessor must resolve per view — this is
// the contract BindRG's per-pane loop binds the UI against.
TEST(RenderPipelineDeclareTests, SpineDeclaresFourQuadViewsInOneCall)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        GameEngine::Testing::PinWorldOnlyPipeline(rs.Spine());
        const CameraId camId = rs.Views().AllocateCamera("QuadCam");
        CameraData cd{};
        for (int i = 0; i < 16; i += 5)
        {
            cd.view[i] = 1.0f;
            cd.proj[i] = 1.0f;
            cd.viewProj[i] = 1.0f;
        }
        rs.Views().SetCameraData(camId, cd);

        ASSERT_NE(rs.GetGPUScene(), nullptr);
        Rendering::GPUInstance inst{};
        inst.boundingRadius = 1.0f;
        inst.flags = ~0u;
        rs.GetGPUScene()->AddInstance(inst);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        GameEngine::Testing::ScopedPipelineFrame frameRegistration(rs.Spine(), frame);
        frame.BeginFrame(0);

        std::array<ViewId, 4> viewIds{};
        // ViewDesc borrows its name until the view registry is torn down.
        std::array<std::array<char, 32>, 4> viewNames{};
        std::array<RenderGraph::RGTexture, 4> colors{};
        std::vector<ViewTargetsRG> targets;
        for (size_t i = 0; i < viewIds.size(); ++i)
        {
            auto& viewName = viewNames[i];
            std::snprintf(viewName.data(), viewName.size(), "QuadView%zu", i);
            viewIds[i] = rs.Views().AllocateView(viewName.data(), camId);
            rs.Views().SetViewRenderLayerMask(viewIds[i], 1u);
            Rendering::ViewClearConfig clear{};
            clear.clearColor = true;
            clear.clearDepth = true;
            rs.Views().SetViewTargets(viewIds[i], 0, 0, 0, clear);

            char colorName[32];
            std::snprintf(colorName, sizeof(colorName), "Quad.Color%zu", i);
            char depthName[32];
            std::snprintf(depthName, sizeof(depthName), "Quad.Depth%zu", i);
            colors[i] = frame.ImportPersistentTexture(colorName, ColorTargetDesc());
            RenderGraph::RGTexture depth = frame.ImportPersistentTexture(depthName, DepthTargetDesc());
            targets.push_back(ViewTargetsRG{viewIds[i], colors[i], depth, {}});
        }

        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();

        RenderServices::FrameGraphBuildParamsRG params{};
        params.ViewTargets = std::span<const ViewTargetsRG>(targets.data(), targets.size());
        rs.Spine().BuildFrameGraph(frame, params);

        EXPECT_EQ(RGQuery::CountDeclared(frame.Graph(), RGQuery::Subtree{"RenderEntities"}), 4u)
            << "each quad pane declares its own world pass; declared: ["
            << RGQuery::DeclaredNames(frame.Graph(), RGQuery::Subtree{"RenderEntities"}) << "]";

        for (size_t i = 0; i < viewIds.size(); ++i)
        {
            EXPECT_TRUE(frame.Graph().IsExternal(colors[i].Id))
                << "view " << i << ": FinalColor must be an anchored external sink";
            const auto out = rs.GetPipelineOutputRG(frame, viewIds[i]);
            ASSERT_TRUE(out.IsValid()) << "view " << i << ": GetPipelineOutputRG must resolve";
            EXPECT_EQ(out.Out.Id, colors[i].Id)
                << "view " << i << ": pinned blueprint output is the view's color target";
        }

        frame.Execute();
        device->WaitForIdle();

        rs.Shutdown();
    }
    device->Shutdown();
}

// 8e-4 — the PixelPerfectUpscale RenderGraph twin: declares one kFinalize pass
// reading Src and attaching Dst, with push constants fixed at declaration.
// Guard shape: identical Src/Dst is refused. The Finalize contract
// (FinalizeContract.h) is exercised on the Linear input and on both
// destination arms of the EncodedSrgb input (UNORM raw passthrough, _SRGB
// D(c)) — the point-sampled integer upscale never filters, so encoded bytes
// survive it. The HDR-violation clause is structural here (an offscreen dst
// never has an active HDR mode; a backbuffer one is refused inside the pass,
// same shape as SRGBEncodePass).
TEST(RenderPipelineDeclareTests, PixelPerfectUpscaleTwinDeclaresAndGuards)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);

        RenderGraph::RGTexture src = frame.ImportPersistentTexture("PP.Src", ColorTargetDesc());
        RenderGraph::RGTexture dst = frame.ImportPersistentTexture("PP.Dst", ColorTargetDesc());

        Rendering::Passes::PixelPerfectUpscaleParamsRG params{};
        params.Src = src;
        params.Dst = dst;
        params.PassthroughLinear = true;
        params.ReferenceWidth = 32;
        params.ReferenceHeight = 32;
        params.SourceWidth = 34;
        params.SourceHeight = 34;
        params.Zoom = 2;

        const size_t passesBefore = frame.Graph().PassCount();
        const RenderGraph::RGPass pass = Rendering::Passes::AddPixelPerfectUpscalePassRG(
            frame, params, Rendering::Passes::FinalizeInputSpace::Linear);
        if (!pass.IsValid())
            GTEST_SKIP() << "pixelperfect_upscale.shaderpkg unavailable in this environment";
        EXPECT_EQ(frame.Graph().PassCount(), passesBefore + 1);

        // Same-texture src/dst is refused at declaration.
        Rendering::Passes::PixelPerfectUpscaleParamsRG bad = params;
        bad.Dst = src;
        EXPECT_FALSE(Rendering::Passes::AddPixelPerfectUpscalePassRG(
                         frame, bad, Rendering::Passes::FinalizeInputSpace::Linear)
                         .IsValid());

        // Encoded input (#767), both destination arms of the contract: a UNORM
        // destination takes the bytes raw, an _SRGB one takes D(c) so the ROP's
        // re-encode round-trips. Both must DECLARE — refusing the _SRGB arm
        // would leave a Player port silently without its upscale.
        RenderGraph::RGTexture dstEnc =
            frame.ImportPersistentTexture("PP.DstEncoded", ColorTargetDesc());
        Rendering::Passes::PixelPerfectUpscaleParamsRG enc = params;
        enc.Dst = dstEnc;
        enc.PassthroughLinear = false; // ignored for an encoded input
        EXPECT_TRUE(Rendering::Passes::AddPixelPerfectUpscalePassRG(
                        frame, enc, Rendering::Passes::FinalizeInputSpace::EncodedSrgb)
                        .IsValid());

        TextureDesc srgbDstDesc = ColorTargetDesc();
        srgbDstDesc.format = static_cast<uint32_t>(TextureFormat::RGBA8_SRGB);
        RenderGraph::RGTexture dstEncSrgb =
            frame.ImportPersistentTexture("PP.DstEncodedSrgb", srgbDstDesc);
        Rendering::Passes::PixelPerfectUpscaleParamsRG encSrgb = enc;
        encSrgb.Dst = dstEncSrgb;
        EXPECT_TRUE(Rendering::Passes::AddPixelPerfectUpscalePassRG(
                        frame, encSrgb, Rendering::Passes::FinalizeInputSpace::EncodedSrgb)
                        .IsValid());

        frame.MarkOutput(dst);
        frame.MarkOutput(dstEnc);
        frame.MarkOutput(dstEncSrgb);
        frame.Execute();
        device->WaitForIdle();
    }
    device->Shutdown();
}

// #767 — the terminal encode's EncodedSrgb arms, which the editor's game view
// now depends on: its world finalizes first, its HUD blends on the encoded
// bytes, and the movie capture then encodes THAT image rather than linear light.
//
// Two clauses are load-bearing there and neither had a gate:
//   - a quantizer choice. FinalizeQuantizer::None states the source already
//     holds destination code values (a same-depth recording), Destination states
//     this pass owns the step (a 10-bit screen recorded to 8-bit). Both must
//     declare, or one of the two recording configurations silently loses frames.
//   - the HDR refusal. An already-encoded source carries the sRGB curve, which
//     no HDR arm can requantize, so an `encodeOverride` of PQ/HLG must be
//     refused OUTRIGHT. That refusal is exactly why a game-view frame recording
//     an HDR movie has to keep the whole chain linear: were the flip left on,
//     the movie encode would declare nothing and the recording would lose every
//     frame it took.
//
// The Linear + PQ override case is the control that keeps this honest: without
// it the test would pass just as well against a pass that refused EVERY
// override, and would say nothing about the input space at all.
TEST(RenderPipelineDeclareTests, EncodedFinalizeRefusesHdrOverrideAndTakesBothQuantizers)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);

        TextureDesc srcDesc = ColorTargetDesc();
        srcDesc.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
        srcDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget) |
                        static_cast<uint32_t>(TextureUsage::ShaderResource);
        const RenderGraph::RGTexture src = frame.ImportPersistentTexture("Enc.Src", srcDesc);

        TextureDesc dst8Desc = ColorTargetDesc();
        dst8Desc.format = static_cast<uint32_t>(TextureFormat::BGRA8_UNORM);
        TextureDesc dst10Desc = ColorTargetDesc();
        dst10Desc.format = static_cast<uint32_t>(TextureFormat::RGB10A2_UNORM);

        const auto declare = [&](const char* name, const TextureDesc& desc,
                                 Rendering::Passes::FinalizeInputSpace space,
                                 Rendering::Passes::FinalizeQuantizer quantizer,
                                 std::optional<Rendering::HdrOutputMode> encodeOverride)
        {
            const RenderGraph::RGTexture dst = frame.ImportPersistentTexture(name, desc);
            const size_t before = frame.Graph().PassCount();
            const bool declared =
                Rendering::Passes::AddSRGBEncodePassRG(frame, src, dst,
                                                       {.InputSpace = space,
                                                        .Quantizer = quantizer,
                                                        .EncodeOverride = encodeOverride,
                                                        .VolumeDebandThresholdLsb = 0.0f},
                                                       name)
                    .IsValid();
            // A pass that returns invalid must also have added nothing — a
            // half-declared pass would execute while the caller believes it did
            // not.
            EXPECT_EQ(frame.Graph().PassCount(), before + (declared ? 1u : 0u)) << name;
            if (declared)
                frame.MarkOutput(dst);
            return declared;
        };

        // Baseline: the ordinary linear terminal encode. Doubles as the shader
        // availability probe — everything below is meaningless without it.
        if (!declare("Enc.LinearBaseline", dst8Desc, Rendering::Passes::FinalizeInputSpace::Linear,
                     Rendering::Passes::FinalizeQuantizer::Destination, std::nullopt))
            GTEST_SKIP() << "encode_srgb.shaderpkg unavailable in this environment";

        // Both quantizer arms of an encoded source.
        EXPECT_TRUE(declare("Enc.EncodedNone", dst8Desc,
                            Rendering::Passes::FinalizeInputSpace::EncodedSrgb,
                            Rendering::Passes::FinalizeQuantizer::None, std::nullopt))
            << "a same-depth recording off the encoded chain must still declare";
        EXPECT_TRUE(declare("Enc.EncodedDestination", dst8Desc,
                            Rendering::Passes::FinalizeInputSpace::EncodedSrgb,
                            Rendering::Passes::FinalizeQuantizer::Destination, std::nullopt))
            << "a coarser-stepped recording must be able to requantize the encoded chain";

        // The control: an HDR override is fine on a LINEAR source, so the
        // refusals below are about the input space and nothing else.
        EXPECT_TRUE(declare("Enc.LinearPq", dst10Desc,
                            Rendering::Passes::FinalizeInputSpace::Linear,
                            Rendering::Passes::FinalizeQuantizer::Destination,
                            Rendering::HdrOutputMode::HDR10_PQ))
            << "HDR movie capture off a linear source is the shipping path";

        // The refusal that forces the game view's HDR-movie fallback.
        EXPECT_FALSE(declare("Enc.EncodedPq", dst10Desc,
                             Rendering::Passes::FinalizeInputSpace::EncodedSrgb,
                             Rendering::Passes::FinalizeQuantizer::Destination,
                             Rendering::HdrOutputMode::HDR10_PQ))
            << "an sRGB-encoded source must never feed a PQ encode";
        EXPECT_FALSE(declare("Enc.EncodedHlg", dst10Desc,
                             Rendering::Passes::FinalizeInputSpace::EncodedSrgb,
                             Rendering::Passes::FinalizeQuantizer::Destination,
                             Rendering::HdrOutputMode::HLG))
            << "an sRGB-encoded source must never feed an HLG encode";

        frame.Execute();
        device->WaitForIdle();
    }
    device->Shutdown();
}

// Slice-7b — multi-stream submit stamping: two RGFrames declared against one
// RenderServices in ONE app frame; submitting B must stamp ONLY B's pendings.
// CSM's SDSM ring keys pendings by RGFrame identity (RGReadbackRing), and the
// spine's per-RGFrame declare-seq lookup still gates GPU culling — without
// either, window A's readbacks would adopt window B's token, resolving
// buffers the GPU may still write.
TEST(RenderPipelineDeclareTests, SubmitStampsOnlyTheSubmittedFrameStreamsPendings)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        GameEngine::Testing::PinWorldOnlyPipeline(rs.Spine());
        auto& feature = rs.EnsureFeature<ShadowMapRenderFeature>();
        CascadedShadowConfig cfg{};
        ASSERT_TRUE(feature.Initialize(device.get(), cfg));

        const CameraId camId = rs.Views().AllocateCamera("StreamCam");
        const ViewId viewA = rs.Views().AllocateView("StreamViewA", camId);
        const ViewId viewB = rs.Views().AllocateView("StreamViewB", camId);
        rs.Views().SetViewRenderLayerMask(viewA, 1u);
        rs.Views().SetViewRenderLayerMask(viewB, 1u);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        rs.Views().SetViewTargets(viewA, 0, 0, 0, clear);
        rs.Views().SetViewTargets(viewB, 0, 0, 0, clear);

        // Two independent frame streams (per-window shape: own pools + ring
        // per stream here; production shares pools but never rings).
        FramePools poolsA(device.get());
        FramePools poolsB(device.get());
        RenderGraph::RGFrame frameA(device.get(), &poolsA.Persistent, &poolsA.Transient, &poolsA.Ring);
        GameEngine::Testing::ScopedPipelineFrame frameARegistration(rs.Spine(), frameA);
        RenderGraph::RGFrame frameB(device.get(), &poolsB.Persistent, &poolsB.Transient, &poolsB.Ring);
        GameEngine::Testing::ScopedPipelineFrame frameBRegistration(rs.Spine(), frameB);

        rs.BeginWorldDrawFrame(); // ONE app frame
        rs.BuildWorldBatchKeys();

        frameA.BeginFrame(0);
        RenderGraph::RGTexture colorA = frameA.ImportPersistentTexture("StreamA.Color", ColorTargetDesc());
        RenderGraph::RGTexture depthA = frameA.ImportPersistentTexture("StreamA.Depth", DepthTargetDesc());
        const ViewTargetsRG vtA{viewA, colorA, depthA, {}};
        RenderServices::FrameGraphBuildParamsRG paramsA{};
        paramsA.ViewTargets = std::span<const ViewTargetsRG>(&vtA, 1);
        rs.Spine().BuildFrameGraph(frameA, paramsA);
        const uint64_t seqA = rs.RGDeclareSeq();
        const auto slotA = feature.AcquireSdsmSlotRG(frameA, viewA, {});
        ASSERT_TRUE(slotA.IsValid());

        frameB.BeginFrame(0);
        RenderGraph::RGTexture colorB = frameB.ImportPersistentTexture("StreamB.Color", ColorTargetDesc());
        RenderGraph::RGTexture depthB = frameB.ImportPersistentTexture("StreamB.Depth", DepthTargetDesc());
        const ViewTargetsRG vtB{viewB, colorB, depthB, {}};
        RenderServices::FrameGraphBuildParamsRG paramsB{};
        paramsB.ViewTargets = std::span<const ViewTargetsRG>(&vtB, 1);
        rs.Spine().BuildFrameGraph(frameB, paramsB);
        const uint64_t seqB = rs.RGDeclareSeq();
        ASSERT_NE(seqA, seqB) << "each stream's spine mints its own sequence";
        const auto slotB = feature.AcquireSdsmSlotRG(frameB, viewB, {});
        ASSERT_TRUE(slotB.IsValid());

        // The depth words of the slot's two result sides (the min side, then
        // the max side, ShadowReceiverMeasurement::kWordsPerSide words each).
        auto writeSlot = [&](Rendering::BufferHandle buf, float minNdc, float maxNdc)
        {
            void* mapped = device->MapBuffer(buf);
            ASSERT_NE(mapped, nullptr);
            auto* words = static_cast<uint8_t*>(mapped);
            std::memcpy(words, &minNdc, sizeof(float));
            std::memcpy(words + ShadowReceiverMeasurement::kWordsPerSide * sizeof(uint32_t), &maxNdc,
                        sizeof(float));
            device->UnmapBuffer(buf);
        };
        writeSlot(slotA, 0.1f, 0.9f);
        writeSlot(slotB, 0.2f, 0.8f);

        // Submit B FIRST: only B's pendings may be stamped.
        frameB.Execute();
        rs.Spine().OnFrameSubmittedRG(frameB, frameB.SubmissionToken());
        device->WaitForIdle();

        ShadowReceiverReadback readback{};
        EXPECT_FALSE(feature.TryResolveSdsmRG(viewA, readback))
            << "stream A's pending must NOT adopt stream B's token";
        EXPECT_TRUE(feature.TryResolveSdsmRG(viewB, readback))
            << "stream B's pending resolves under its own token";

        frameA.Execute();
        rs.Spine().OnFrameSubmittedRG(frameA, frameA.SubmissionToken());
        device->WaitForIdle();
        EXPECT_TRUE(feature.TryResolveSdsmRG(viewA, readback))
            << "stream A resolves once ITS submit stamps it";

        rs.Shutdown();
    }
    device->Shutdown();
}

namespace
{
// Pins "declared exactly once, in the owner frame only" as ONE unit, so a
// non-owner zero-check can never exist without its owner-side positive
// control. A bare zero-check is satisfied by a needle that names NOTHING —
// a renamed production pass then reads as still-absent instead of failing.
// With the pair, a name move fails the owner arm at 0 instead of every
// count silently emptying.
// Wrap call sites in SCOPED_TRACE: the failure line lands in here.
void ExpectDeclaredInOwnerFrameOnly(const RenderGraph::RGFrame& ownerFrame,
                                    const RenderGraph::RGFrame& nonOwnerFrame,
                                    std::string_view exactName, std::string_view diagnosticRoot)
{
    EXPECT_EQ(RGQuery::CountDeclared(ownerFrame.Graph(), RGQuery::Exact{exactName}), 1u)
        << "owner frame must declare '" << exactName << "' exactly once; it declared ["
        << RGQuery::DeclaredNames(ownerFrame.Graph(), RGQuery::Subtree{diagnosticRoot}) << "]";
    EXPECT_EQ(RGQuery::CountDeclared(nonOwnerFrame.Graph(), RGQuery::Exact{exactName}), 0u)
        << "non-owner frame must not re-declare '" << exactName << "'; it declared ["
        << RGQuery::DeclaredNames(nonOwnerFrame.Graph(), RGQuery::Subtree{diagnosticRoot}) << "]";
}

// The world bucketer's render-graph pass name, matched by EQUALITY.
// ScheduleWorldBucketerDispatches calls ScheduleUnifiedScatter with the "World"
// suffix and the builder formats "GPUDrawStream.Scatter.%s"
// (GPUDrawStreamBuilder.cpp). Two SIBLINGS share that prefix and are different
// passes: "GPUDrawStream.Scatter.World.B" (the phase-B occlusion-recover
// scatter) and "GPUDrawStream.Scatter.View<N>" (the per-view seam). ".World" is
// a strict PREFIX of ".World.B", so a substring match counts the sibling as if
// it were the bucketer — a rename of this pass to any name extending it would
// read as still present. Only equality separates them.
constexpr const char* kWorldBucketerPass = "GPUDrawStream.Scatter.World";
// Diagnostic subtree root for the paired pin's failure listing.
constexpr const char* kDrawStreamRoot = "GPUDrawStream";

// Loads compiled .shaderpkg artifacts from the executable-anchored build root,
// where CompileShaderPkgs emits "Shaders/<name>.shaderpkg". The production loader installed
// by RenderServices::Initialize goes through the AssetManager, which isn't
// mounted in this fixture — without a working loader the culling compute
// pipeline never creates and no "GPUCulling." dispatch passes exist to
// count. Install AFTER rs.Initialize (which overwrites the static loader);
// every other test's Initialize reinstalls the production one.
std::vector<uint8_t> StagedCullingShaderLoader(const char* name)
{
    if (!name || !name[0])
        return {};
    const std::filesystem::path p = TestPaths::StagedRoot() / name;
    std::error_code ec;
    if (!std::filesystem::exists(p, ec))
        return {};
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
} // namespace

// Slice-8a — two frame streams in ONE app frame. Stage G (skinning, culling,
// bucketer, visibility union/aggregate) declares ONCE into the first stream
// that calls BuildFrameGraph (the "owner"); the second stream re-imports the
// shared physicals and runs the pipeline declare on its OWN instance and
// blackboard. Pre-8a a single shared instance meant B's declare re-aimed the
// one blackboard and killed A's GetPipelineOutputRG — the headline fix.
TEST(RenderPipelineDeclareTests, TwoFrameStreamsOneAppFrameIsolateAndShareGlobalStages)
{
    ASSERT_FALSE(StagedCullingShaderLoader("Shaders/frustum_culling.shaderpkg").empty())
        << "Required frustum_culling.shaderpkg is missing; build CompileShaderPkgs";
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        GameEngine::Testing::PinWorldOnlyPipeline(rs.Spine());
        Rendering::GPUScene::SetCullingShaderLoader(&StagedCullingShaderLoader);
        const CameraId camId = rs.Views().AllocateCamera("TwoStreamCam");
        CameraData cd{};
        for (int i = 0; i < 16; i += 5)
        {
            cd.view[i] = 1.0f;
            cd.proj[i] = 1.0f;
            cd.viewProj[i] = 1.0f;
        }
        rs.Views().SetCameraData(camId, cd);
        const ViewId viewA = rs.Views().AllocateView("TwoStreamViewA", camId);
        const ViewId viewB = rs.Views().AllocateView("TwoStreamViewB", camId);
        rs.Views().SetViewRenderLayerMask(viewA, 1u);
        rs.Views().SetViewRenderLayerMask(viewB, 1u);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        rs.Views().SetViewTargets(viewA, 0, 0, 0, clear);
        rs.Views().SetViewTargets(viewB, 0, 0, 0, clear);

        // One GPUScene instance so culling/bucketer don't vacuously
        // early-out — stage-G once-total is exactly what this test pins.
        ASSERT_NE(rs.GetGPUScene(), nullptr);
        Rendering::GPUInstance inst{};
        inst.boundingRadius = 1.0f;
        inst.flags = ~0u;
        rs.GetGPUScene()->AddInstance(inst);

        FramePools poolsA(device.get());
        FramePools poolsB(device.get());
        RenderGraph::RGFrame frameA(device.get(), &poolsA.Persistent, &poolsA.Transient, &poolsA.Ring);
        GameEngine::Testing::ScopedPipelineFrame frameARegistration(rs.Spine(), frameA);
        RenderGraph::RGFrame frameB(device.get(), &poolsB.Persistent, &poolsB.Transient, &poolsB.Ring);
        GameEngine::Testing::ScopedPipelineFrame frameBRegistration(rs.Spine(), frameB);

        rs.BeginWorldDrawFrame(); // ONE app frame
        rs.BuildWorldBatchKeys();

        frameA.BeginFrame(0);
        RenderGraph::RGTexture colorA = frameA.ImportPersistentTexture("TwoStreamA.Color", ColorTargetDesc());
        RenderGraph::RGTexture depthA = frameA.ImportPersistentTexture("TwoStreamA.Depth", DepthTargetDesc());
        const ViewTargetsRG vtA{viewA, colorA, depthA, {}};
        RenderServices::FrameGraphBuildParamsRG paramsA{};
        paramsA.ViewTargets = std::span<const ViewTargetsRG>(&vtA, 1);
        rs.Spine().BuildFrameGraph(frameA, paramsA);

        const auto outA = rs.GetPipelineOutputRG(frameA, viewA);
        ASSERT_TRUE(outA.Out.IsValid()) << "owner stream's pipeline output must resolve";

        frameB.BeginFrame(0);
        RenderGraph::RGTexture colorB = frameB.ImportPersistentTexture("TwoStreamB.Color", ColorTargetDesc());
        RenderGraph::RGTexture depthB = frameB.ImportPersistentTexture("TwoStreamB.Depth", DepthTargetDesc());
        const ViewTargetsRG vtB{viewB, colorB, depthB, {}};
        RenderServices::FrameGraphBuildParamsRG paramsB{};
        paramsB.ViewTargets = std::span<const ViewTargetsRG>(&vtB, 1);
        rs.Spine().BuildFrameGraph(frameB, paramsB);

        // (a) The headline fix: B's declare must not kill A's output, and B
        //     resolves its own — on distinct physicals.
        const auto outA2 = rs.GetPipelineOutputRG(frameA, viewA);
        EXPECT_TRUE(outA2.Out.IsValid())
            << "stream B's declare must not re-aim stream A's blackboard (pre-8a bug)";
        const auto outB = rs.GetPipelineOutputRG(frameB, viewB);
        ASSERT_TRUE(outB.Out.IsValid()) << "second stream's pipeline output must resolve";
        EXPECT_TRUE(outA2.Physical.IsValid());
        EXPECT_TRUE(outB.Physical.IsValid());
        EXPECT_NE(outA2.Physical, outB.Physical)
            << "the two streams render to distinct view targets";

        // (b) Stage G ran once, in the owner frame only.
        EXPECT_GT(RGQuery::CountDeclared(frameA.Graph(), RGQuery::Subtree{"GPUCulling"}), 0u)
            << "owner frame carries the culling dispatches";
        EXPECT_EQ(RGQuery::CountDeclared(frameB.Graph(), RGQuery::Subtree{"GPUCulling"}), 0u)
            << "non-owner must not re-run culling (it would wipe the shared "
               "visibility-range table and double the GPU work)";
        {
            SCOPED_TRACE("stage G: world bucketer declares in the owner frame only");
            ExpectDeclaredInOwnerFrameOnly(frameA, frameB, kWorldBucketerPass, kDrawStreamRoot);
        }

        // (c) B consumed the shared products by RE-IMPORT. Pin: importing the
        //     same physicals under the same names AFTER the spine must mint
        //     ZERO new resources — dedup-by-physical only collapses onto ids
        //     the non-owner branch already imported. (Asserting validity of a
        //     test-side import would pass even if the spine imported nothing.)
        const size_t resourcesAfterSpine = frameB.Graph().ResourceCount();
        const auto sceneRGB = rs.GetGPUScene()->ImportFrameResources(frameB);
        EXPECT_TRUE(sceneRGB.Instances.IsValid());
        if (auto* culling = rs.GetGPUCullingPipeline())
            (void)culling->ImportVisibility(frameB);
        RenderGraph::RGBuffer rgAtlasB{};
        if (const auto atlasBuf = rs.GetSkinPaletteAtlas().GetBuffer(); atlasBuf.IsValid())
            rgAtlasB = frameB.ImportExternalBuffer("SkinPaletteAtlas", atlasBuf);
        EXPECT_EQ(frameB.Graph().ResourceCount(), resourcesAfterSpine)
            << "the non-owner spine must have re-imported the shared products "
               "(GPUScene buffers, visibility, atlas) — a fresh id here means "
               "the re-import branch didn't run";
        // And B's world pass actually READS the re-imported atlas (the
        // compute→VS ordering edge), when the fixture has an atlas at all.
        if (rgAtlasB.IsValid())
        {
            const RenderGraph::RGPassId worldB =
                RGQuery::FindDeclared(frameB.Graph(), RGQuery::Subtree{"RenderEntities"});
            ASSERT_NE(worldB, RenderGraph::kInvalidId) << "non-owner world pass never declared";
            EXPECT_TRUE(frameB.Graph().HasReadAccess(worldB, rgAtlasB.Id))
                << "the non-owner world pass must Read the re-imported SkinPaletteAtlas";
        }

        // (d) Submit choreography: both streams execute + stamp cleanly.
        frameA.Execute();
        rs.Spine().OnFrameSubmittedRG(frameA, frameA.SubmissionToken());
        frameB.Execute();
        rs.Spine().OnFrameSubmittedRG(frameB, frameB.SubmissionToken());
        device->WaitForIdle();

        rs.Shutdown();
    }
    device->Shutdown();
}

// Slice-8a epoch stability (A1.4 S0 lock, spine F6): BuildFrameGraph must NEVER
// bump the app-frame epoch — only BeginWorldDrawFrame does. Two spine calls on
// two different frames in ONE app frame (no intervening BeginWorldDrawFrame): the
// second frame is a NON-OWNER (zero stage-G passes), and a repeat call on the
// first frame is a whole-spine no-op (its pass count is unchanged). If a spine
// call ever bumped the epoch, the second call would re-arm stage G (the
// non-owner assertions fail) and the repeat would re-declare (the no-op fails).
TEST(RenderPipelineDeclareTests, BuildFrameGraphNeverBumpsAppFrameEpoch)
{
    ASSERT_FALSE(StagedCullingShaderLoader("Shaders/frustum_culling.shaderpkg").empty())
        << "Required frustum_culling.shaderpkg is missing; build CompileShaderPkgs";
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        GameEngine::Testing::PinWorldOnlyPipeline(rs.Spine());
        Rendering::GPUScene::SetCullingShaderLoader(&StagedCullingShaderLoader);
        const CameraId camId = rs.Views().AllocateCamera("EpochCam");
        CameraData cd{};
        for (int i = 0; i < 16; i += 5)
        {
            cd.view[i] = 1.0f;
            cd.proj[i] = 1.0f;
            cd.viewProj[i] = 1.0f;
        }
        rs.Views().SetCameraData(camId, cd);
        const ViewId viewA = rs.Views().AllocateView("EpochViewA", camId);
        const ViewId viewB = rs.Views().AllocateView("EpochViewB", camId);
        rs.Views().SetViewRenderLayerMask(viewA, 1u);
        rs.Views().SetViewRenderLayerMask(viewB, 1u);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        rs.Views().SetViewTargets(viewA, 0, 0, 0, clear);
        rs.Views().SetViewTargets(viewB, 0, 0, 0, clear);

        ASSERT_NE(rs.GetGPUScene(), nullptr);
        Rendering::GPUInstance inst{};
        inst.boundingRadius = 1.0f;
        inst.flags = ~0u;
        rs.GetGPUScene()->AddInstance(inst);

        FramePools poolsA(device.get());
        FramePools poolsB(device.get());
        RenderGraph::RGFrame frameA(device.get(), &poolsA.Persistent, &poolsA.Transient, &poolsA.Ring);
        GameEngine::Testing::ScopedPipelineFrame frameARegistration(rs.Spine(), frameA);
        RenderGraph::RGFrame frameB(device.get(), &poolsB.Persistent, &poolsB.Transient, &poolsB.Ring);
        GameEngine::Testing::ScopedPipelineFrame frameBRegistration(rs.Spine(), frameB);

        rs.BeginWorldDrawFrame(); // ONE app frame — the only epoch bump
        rs.BuildWorldBatchKeys();

        frameA.BeginFrame(0);
        RenderGraph::RGTexture colorA = frameA.ImportPersistentTexture("Epoch.ColorA", ColorTargetDesc());
        RenderGraph::RGTexture depthA = frameA.ImportPersistentTexture("Epoch.DepthA", DepthTargetDesc());
        const ViewTargetsRG vtA{viewA, colorA, depthA, {}};
        RenderServices::FrameGraphBuildParamsRG paramsA{};
        paramsA.ViewTargets = std::span<const ViewTargetsRG>(&vtA, 1);
        rs.Spine().BuildFrameGraph(frameA, paramsA);
        EXPECT_GT(RGQuery::CountDeclared(frameA.Graph(), RGQuery::Subtree{"GPUCulling"}), 0u)
            << "the first frame owns stage G this app frame";

        // Second stream, SAME app frame, NO intervening BeginWorldDrawFrame.
        frameB.BeginFrame(0);
        RenderGraph::RGTexture colorB = frameB.ImportPersistentTexture("Epoch.ColorB", ColorTargetDesc());
        RenderGraph::RGTexture depthB = frameB.ImportPersistentTexture("Epoch.DepthB", DepthTargetDesc());
        const ViewTargetsRG vtB{viewB, colorB, depthB, {}};
        RenderServices::FrameGraphBuildParamsRG paramsB{};
        paramsB.ViewTargets = std::span<const ViewTargetsRG>(&vtB, 1);
        rs.Spine().BuildFrameGraph(frameB, paramsB);
        EXPECT_EQ(RGQuery::CountDeclared(frameB.Graph(), RGQuery::Subtree{"GPUCulling"}), 0u)
            << "no epoch bump between the two calls, so the second frame is a "
               "non-owner: stage G stays one-shot per app frame";
        {
            SCOPED_TRACE("epoch stability: world bucketer declares in the owner frame only");
            ExpectDeclaredInOwnerFrameOnly(frameA, frameB, kWorldBucketerPass, kDrawStreamRoot);
        }

        // Third call, back on the FIRST frame, still the SAME app frame: the
        // per-stream repeat guard makes it a whole-spine no-op.
        const size_t passesA = frameA.Graph().PassCount();
        rs.Spine().BuildFrameGraph(frameA, paramsA);
        EXPECT_EQ(frameA.Graph().PassCount(), passesA)
            << "re-declaring the first frame in the same app frame declares "
               "nothing — BuildFrameGraph never bumps the epoch";

        frameA.Execute();
        frameB.Execute();
        device->WaitForIdle();

        rs.Shutdown();
    }
    device->Shutdown();
}

// Slice-8a — stream-slot lifetime: (1) a re-begun frame (passive
// RenderSingle: BeginFrame again WITHOUT a new app frame) is a STALE
// incarnation — its outputs must not validate until re-declared, the
// re-declare goes through the non-owner branch (stage G stays one-shot per
// app frame), and a new app frame re-arms stage G; (2) the close-path reap
// drops the stream slot without disturbing the surviving stream.
TEST(RenderPipelineDeclareTests, ReBegunFrameInvalidatesUntilRedeclareAndReapDropsStream)
{
    ASSERT_FALSE(StagedCullingShaderLoader("Shaders/frustum_culling.shaderpkg").empty())
        << "Required frustum_culling.shaderpkg is missing; build CompileShaderPkgs";
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        GameEngine::Testing::PinWorldOnlyPipeline(rs.Spine());
        Rendering::GPUScene::SetCullingShaderLoader(&StagedCullingShaderLoader);
        const CameraId camId = rs.Views().AllocateCamera("ReBeginCam");
        CameraData cd{};
        for (int i = 0; i < 16; i += 5)
        {
            cd.view[i] = 1.0f;
            cd.proj[i] = 1.0f;
            cd.viewProj[i] = 1.0f;
        }
        rs.Views().SetCameraData(camId, cd);
        const ViewId viewId = rs.Views().AllocateView("ReBeginView", camId);
        rs.Views().SetViewRenderLayerMask(viewId, 1u);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);
        ASSERT_NE(rs.GetGPUScene(), nullptr);
        Rendering::GPUInstance inst{};
        inst.boundingRadius = 1.0f;
        inst.flags = ~0u;
        rs.GetGPUScene()->AddInstance(inst);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        GameEngine::Testing::ScopedPipelineFrame frameRegistration(rs.Spine(), frame);

        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();

        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("ReBegin.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("ReBegin.Depth", DepthTargetDesc());
        const ViewTargetsRG vt{viewId, color, depth, {}};
        RenderServices::FrameGraphBuildParamsRG params{};
        params.ViewTargets = std::span<const ViewTargetsRG>(&vt, 1);
        rs.Spine().BuildFrameGraph(frame, params);
        ASSERT_TRUE(rs.GetPipelineOutputRG(frame, viewId).Out.IsValid());
        EXPECT_GT(RGQuery::CountDeclared(frame.Graph(), RGQuery::Subtree{"GPUCulling"}), 0u);
        frame.Execute();
        device->WaitForIdle();

        // Re-begin the SAME frame, same app frame (the passive RenderSingle
        // shape). The previous incarnation's ids are dead.
        frame.BeginFrame(1);
        EXPECT_FALSE(rs.GetPipelineOutputRG(frame, viewId).Out.IsValid())
            << "a re-begun incarnation must not validate the previous one's ids";

        // Re-declare: enters the NON-OWNER branch (stage G already ran this
        // app frame), so the re-begun graph has pipeline passes but no
        // culling dispatches.
        RenderGraph::RGTexture color1 = frame.ImportPersistentTexture("ReBegin.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth1 = frame.ImportPersistentTexture("ReBegin.Depth", DepthTargetDesc());
        const ViewTargetsRG vt1{viewId, color1, depth1, {}};
        params.ViewTargets = std::span<const ViewTargetsRG>(&vt1, 1);
        rs.Spine().BuildFrameGraph(frame, params);
        EXPECT_TRUE(rs.GetPipelineOutputRG(frame, viewId).Out.IsValid())
            << "re-declaring the re-begun incarnation restores its outputs";
        EXPECT_EQ(RGQuery::CountDeclared(frame.Graph(), RGQuery::Subtree{"GPUCulling"}), 0u)
            << "stage G is one-shot per app frame, even for the owner stream";
        frame.Execute();
        device->WaitForIdle();

        // New app frame: stage G re-arms.
        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();
        frame.BeginFrame(2);
        RenderGraph::RGTexture color2 = frame.ImportPersistentTexture("ReBegin.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth2 = frame.ImportPersistentTexture("ReBegin.Depth", DepthTargetDesc());
        const ViewTargetsRG vt2{viewId, color2, depth2, {}};
        params.ViewTargets = std::span<const ViewTargetsRG>(&vt2, 1);
        rs.Spine().BuildFrameGraph(frame, params);
        EXPECT_GT(RGQuery::CountDeclared(frame.Graph(), RGQuery::Subtree{"GPUCulling"}), 0u)
            << "a new app frame re-arms the global stages";
        frame.Execute();
        device->WaitForIdle();

        // Reap: a scoped second stream declares, dies, and is reaped — the
        // surviving stream keeps working on the next app frame.
        {
            FramePools poolsB(device.get());
            RenderGraph::RGFrame frameB(device.get(), &poolsB.Persistent, &poolsB.Transient, &poolsB.Ring);
            GameEngine::Testing::ScopedPipelineFrame frameBRegistration(rs.Spine(), frameB);
            frameB.BeginFrame(0);
            RenderGraph::RGTexture colorB =
                frameB.ImportPersistentTexture("ReBeginB.Color", ColorTargetDesc());
            RenderGraph::RGTexture depthB =
                frameB.ImportPersistentTexture("ReBeginB.Depth", DepthTargetDesc());
            const ViewTargetsRG vtB{viewId, colorB, depthB, {}};
            RenderServices::FrameGraphBuildParamsRG paramsB{};
            paramsB.ViewTargets = std::span<const ViewTargetsRG>(&vtB, 1);
            rs.Spine().BuildFrameGraph(frameB, paramsB);
            frameB.Execute();
            device->WaitForIdle();
            rs.Spine().RemovePipelineInstanceForFrame(&frameB);
        }

        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();
        frame.BeginFrame(3);
        RenderGraph::RGTexture color3 = frame.ImportPersistentTexture("ReBegin.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth3 = frame.ImportPersistentTexture("ReBegin.Depth", DepthTargetDesc());
        const ViewTargetsRG vt3{viewId, color3, depth3, {}};
        params.ViewTargets = std::span<const ViewTargetsRG>(&vt3, 1);
        rs.Spine().BuildFrameGraph(frame, params);
        EXPECT_TRUE(rs.GetPipelineOutputRG(frame, viewId).Out.IsValid())
            << "reaping a dead stream must not disturb the survivors";
        frame.Execute();
        device->WaitForIdle();

        rs.Shutdown();
    }
    device->Shutdown();
}

// Slice-7d — the editor window-driver contract: ONE spine call carries the
// frame's COMPLETE view set. Every carried view's pipeline declares against
// the same RGFrame and resolves through GetPipelineOutputRG; a follow-up
// call in the same frame (a second driver re-entering with fewer views) is
// a whole-spine no-op — it must neither redeclare nor drop the omitted
// view's already-declared output.
TEST(RenderPipelineDeclareTests, TwoViewsOneSpineCallBothResolveAndRepeatOmissionIsNoOp)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        GameEngine::Testing::PinWorldOnlyPipeline(rs.Spine());
        const CameraId camId = rs.Views().AllocateCamera("EdCam");
        CameraData cd{};
        for (int i = 0; i < 16; i += 5)
        {
            cd.view[i] = 1.0f;
            cd.proj[i] = 1.0f;
            cd.viewProj[i] = 1.0f;
        }
        rs.Views().SetCameraData(camId, cd);
        const ViewId viewA = rs.Views().AllocateView("EdViewA", camId);
        const ViewId viewB = rs.Views().AllocateView("EdViewB", camId);
        rs.Views().SetViewRenderLayerMask(viewA, 1u);
        rs.Views().SetViewRenderLayerMask(viewB, 1u);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        rs.Views().SetViewClearConfig(viewA, clear);
        rs.Views().SetViewClearConfig(viewB, clear);

        ASSERT_NE(rs.GetGPUScene(), nullptr);
        Rendering::GPUInstance inst{};
        inst.boundingRadius = 1.0f;
        inst.flags = ~0u;
        rs.GetGPUScene()->AddInstance(inst);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        GameEngine::Testing::ScopedPipelineFrame frameRegistration(rs.Spine(), frame);
        frame.BeginFrame(0);
        RenderGraph::RGTexture colorA = frame.ImportPersistentTexture("EdA.Color", ColorTargetDesc());
        RenderGraph::RGTexture depthA = frame.ImportPersistentTexture("EdA.Depth", DepthTargetDesc());
        RenderGraph::RGTexture colorB = frame.ImportPersistentTexture("EdB.Color", ColorTargetDesc());
        RenderGraph::RGTexture depthB = frame.ImportPersistentTexture("EdB.Depth", DepthTargetDesc());

        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();

        const ViewTargetsRG vts[2] = {{viewA, colorA, depthA, {}},
                                      {viewB, colorB, depthB, {}}};
        RenderServices::FrameGraphBuildParamsRG params{};
        params.ViewTargets = std::span<const ViewTargetsRG>(vts, 2);
        rs.Spine().BuildFrameGraph(frame, params);

        const size_t passesAfterFull = frame.Graph().PassCount();
        const auto outA = rs.GetPipelineOutputRG(frame, viewA);
        const auto outB = rs.GetPipelineOutputRG(frame, viewB);
        EXPECT_TRUE(outA.IsValid()) << "view A must resolve a pipeline output";
        EXPECT_TRUE(outB.IsValid()) << "view B must resolve a pipeline output";
        EXPECT_NE(outA.Out.Id, outB.Out.Id)
            << "each view's FinalColor is its own resource";
        EXPECT_NE(outA.Physical, outB.Physical);

        // Re-entry with a SUBSET of views: whole-spine guard → no-op.
        params.ViewTargets = std::span<const ViewTargetsRG>(vts, 1);
        rs.Spine().BuildFrameGraph(frame, params);
        EXPECT_EQ(frame.Graph().PassCount(), passesAfterFull)
            << "a same-frame repeat call must not redeclare";
        EXPECT_TRUE(rs.GetPipelineOutputRG(frame, viewB).IsValid())
            << "the omitted view keeps its declared output";

        frame.Execute();
        device->WaitForIdle();
        rs.Shutdown();
    }
    device->Shutdown();
}

// Slice-7d — a renderLayerMask==0 view carried in the view set contributes
// no per-view pipeline (the suspended-controller shape: the view exists in
// the registry but renders nothing); the active view is unaffected.
TEST(RenderPipelineDeclareTests, ZeroLayerMaskViewDeclaresNoPipelineOutput)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        GameEngine::Testing::PinWorldOnlyPipeline(rs.Spine());
        const CameraId camId = rs.Views().AllocateCamera("MaskCam");
        CameraData cd{};
        for (int i = 0; i < 16; i += 5)
        {
            cd.view[i] = 1.0f;
            cd.proj[i] = 1.0f;
            cd.viewProj[i] = 1.0f;
        }
        rs.Views().SetCameraData(camId, cd);
        const ViewId viewA = rs.Views().AllocateView("MaskViewA", camId);
        const ViewId viewB = rs.Views().AllocateView("MaskViewB", camId);
        rs.Views().SetViewRenderLayerMask(viewA, 1u);
        rs.Views().SetViewRenderLayerMask(viewB, 0u);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        rs.Views().SetViewClearConfig(viewA, clear);
        rs.Views().SetViewClearConfig(viewB, clear);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        GameEngine::Testing::ScopedPipelineFrame frameRegistration(rs.Spine(), frame);
        frame.BeginFrame(0);
        RenderGraph::RGTexture colorA = frame.ImportPersistentTexture("MaskA.Color", ColorTargetDesc());
        RenderGraph::RGTexture depthA = frame.ImportPersistentTexture("MaskA.Depth", DepthTargetDesc());
        RenderGraph::RGTexture colorB = frame.ImportPersistentTexture("MaskB.Color", ColorTargetDesc());
        RenderGraph::RGTexture depthB = frame.ImportPersistentTexture("MaskB.Depth", DepthTargetDesc());

        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();

        const ViewTargetsRG vts[2] = {{viewA, colorA, depthA, {}},
                                      {viewB, colorB, depthB, {}}};
        RenderServices::FrameGraphBuildParamsRG params{};
        params.ViewTargets = std::span<const ViewTargetsRG>(vts, 2);
        rs.Spine().BuildFrameGraph(frame, params);

        EXPECT_TRUE(rs.GetPipelineOutputRG(frame, viewA).IsValid());
        EXPECT_FALSE(rs.GetPipelineOutputRG(frame, viewB).IsValid())
            << "a mask-0 view must declare no pipeline output";

        frame.Execute();
        device->WaitForIdle();
        rs.Shutdown();
    }
    device->Shutdown();
}

// Slice-7f — the editor's scene+game shape: two views in ONE spine call with
// ASYMMETRIC MSAA (game cameras carry per-camera sample counts). The MSAA
// view's output must resolve to a single-sample texture — the UI-bindability
// contract BindGameViewRG depends on.
TEST(RenderPipelineDeclareTests, TwoViewsAsymmetricMsaaOneSpineCall)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        GameEngine::Testing::PinWorldOnlyPipeline(rs.Spine());
        const CameraId camId = rs.Views().AllocateCamera("AsymCam");
        CameraData cd{};
        for (int i = 0; i < 16; i += 5)
        {
            cd.view[i] = 1.0f;
            cd.proj[i] = 1.0f;
            cd.viewProj[i] = 1.0f;
        }
        rs.Views().SetCameraData(camId, cd);
        const ViewId viewA = rs.Views().AllocateView("AsymViewA", camId);
        const ViewId viewB = rs.Views().AllocateView("AsymViewB", camId);
        rs.Views().SetViewRenderLayerMask(viewA, 1u);
        rs.Views().SetViewRenderLayerMask(viewB, 1u);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        rs.Views().SetViewClearConfig(viewA, clear);
        rs.Views().SetViewClearConfig(viewB, clear);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        GameEngine::Testing::ScopedPipelineFrame frameRegistration(rs.Spine(), frame);
        frame.BeginFrame(0);
        RenderGraph::RGTexture colorA = frame.ImportPersistentTexture("AsymA.Color", ColorTargetDesc());
        RenderGraph::RGTexture depthA = frame.ImportPersistentTexture("AsymA.Depth", DepthTargetDesc());

        TextureDesc colorMsaa = ColorTargetDesc();
        colorMsaa.sampleCount = 4;
        TextureDesc depthMsaa = DepthTargetDesc();
        depthMsaa.sampleCount = 4;
        TextureDesc resolveDesc = ColorTargetDesc();
        RenderGraph::RGTexture colorB = frame.ImportPersistentTexture("AsymB.Color", colorMsaa);
        RenderGraph::RGTexture depthB = frame.ImportPersistentTexture("AsymB.Depth", depthMsaa);
        RenderGraph::RGTexture resolveB = frame.ImportPersistentTexture("AsymB.Resolve", resolveDesc);

        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();

        const ViewTargetsRG vts[2] = {{viewA, colorA, depthA, {}},
                                      {viewB, colorB, depthB, resolveB}};
        RenderServices::FrameGraphBuildParamsRG params{};
        params.ViewTargets = std::span<const ViewTargetsRG>(vts, 2);
        rs.Spine().BuildFrameGraph(frame, params);

        const auto outA = rs.GetPipelineOutputRG(frame, viewA);
        const auto outB = rs.GetPipelineOutputRG(frame, viewB);
        EXPECT_TRUE(outA.IsValid());
        ASSERT_TRUE(outB.IsValid());
        EXPECT_EQ(frame.Graph().ResourceDesc(outB.Out.Id).SampleCount, 1u)
            << "the MSAA view's output must land on a single-sample resolve";

        frame.Execute();
        device->WaitForIdle();
        rs.Shutdown();
    }
    device->Shutdown();
}

// Slice-7f — ClearViewTargets is the old-arm→RenderGraph handoff discriminator: the
// old graph's retained world pass activates on the REGISTRY targets the old
// arm published; an RenderGraph-owned view must be able to drop them (without the
// null-target diagnostic SetViewTargets carries).
TEST(RenderPipelineDeclareTests, ClearViewTargetsDropsPublishedRegistryTargets)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        const CameraId camId = rs.Views().AllocateCamera("ClrCam");
        const ViewId viewId = rs.Views().AllocateView("ClrView", camId);
        rs.Views().SetViewRenderLayerMask(viewId, 1u);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        rs.Views().SetViewTargets(viewId, /*color*/ 7u, /*depth*/ 9u, /*resolve*/ 0u, clear);

        auto findView = [&]() -> const Rendering::ViewDesc*
        {
            for (const auto& v : rs.Views().GetViews())
                if (v.id == viewId)
                    return &v;
            return nullptr;
        };
        const Rendering::ViewDesc* v = findView();
        ASSERT_NE(v, nullptr);
        ASSERT_NE(v->targets.color, 0u);

        rs.Views().ClearViewTargets(viewId);
        v = findView();
        ASSERT_NE(v, nullptr);
        EXPECT_EQ(v->targets.color, 0u);
        EXPECT_EQ(v->targets.depth, 0u);
        EXPECT_EQ(v->targets.resolve, 0u);
        // Clear config survives (the RenderGraph arm republishes it separately).
        EXPECT_TRUE(v->targets.clearColor);

        rs.Shutdown();
    }
    device->Shutdown();
}

// Slice-7f — the game view's no-camera frame: a clear-only pass with an empty
// exec survives culling because its output is MARKED (export anchors the
// chain, replacing the old arm's PreventCulling), and the pooled physical is
// available at declaration for the same-frame UI bind.
TEST(RenderPipelineDeclareTests, NoCameraClearPassSurvivesCullingViaExport)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);

        RenderGraph::RGTexture color = frame.ImportPersistentTexture("NoCam.Color", ColorTargetDesc());
        const RenderGraph::RGPass pass = frame.AddPass(
            "NoCameraClearRG", 100,
            [&](RenderGraph::RGPassBuilder& p)
            {
                RenderGraph::RGAttachmentOps ops{};
                ops.Load = RenderGraph::RGLoadOp::Clear;
                ops.Store = RenderGraph::RGStoreOp::Store;
                p.AttachColor(0, color, ops);
            },
            [](RenderGraph::RGContext&) {});
        frame.MarkOutput(color, RenderGraph::RGImageLayout::ShaderReadOnly);

        const TextureHandle physical = frame.PhysicalTexture(color);
        EXPECT_TRUE(physical.IsValid()) << "pool imports carry physicals at declaration";

        frame.Execute();
        EXPECT_FALSE(frame.Graph().IsCulled(pass.Id))
            << "the export must anchor the clear pass through cull";
        device->WaitForIdle();
    }
    device->Shutdown();
}

// Slice-5a backfill — MSAA-on through the pre-pass + world arm: a
// multisampled caller color must NOT collapse into the single-sample
// pipeline resolve target; the world declares the resolve PAIR instead and
// EffectiveColor/FinalColor land on the resolve.
TEST(RenderPipelineDeclareTests, MsaaOnKeepsViewColorMultisampledAndWorldDeclaresResolvePair)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        const CameraId camId = rs.Views().AllocateCamera("MsaaCam");
        const ViewId viewId = rs.Views().AllocateView("MsaaView", camId);
        rs.Views().SetViewRenderLayerMask(viewId, 1u);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);

        RenderPipelineNodeRegistry registry;
        ASSERT_TRUE(registry.Register(
            "WorldRender", [] { return std::make_unique<Nodes::WorldRenderNode>(); }, true));

        RenderPipelineBlueprint bp;
        bp.pipelineName = "MsaaTest";
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "World";
            p.type = "WorldRender";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"World","type":"WorldRender"})";
            bp.passes.push_back(p);
        }
        bp.worldColorResolveTargetRef = "SceneColor";
        bp.resources.push_back(
            {"SceneColor",
             R"({"kind":"texture","scope":"perView","format":"rgba8_unorm","extent":{"scale":[1,1]},"usage":["renderTarget","shaderResource"]})"});
        bp.outputs.push_back({"FinalColor", "View.Resolve"});

        RenderPipelineInstance instance(rs, registry);
        instance.SetBlueprint(bp);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);

        RenderGraph::RGTexture color = frame.ImportPersistentTexture("MSAA.Color", ColorTargetDesc(4));
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("MSAA.Depth", DepthTargetDesc(4));

        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();

        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{viewId, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(rs.Views().GetViews().begin(), rs.Views().GetViews().end());
        instance.Declare(frame, targets, views);

        const auto* fr = instance.FrameResourcesFor(&frame);
        ASSERT_NE(fr, nullptr);
        const auto colorIt = fr->Textures.find({viewId, "View.Color"});
        ASSERT_NE(colorIt, fr->Textures.end());
        EXPECT_EQ(colorIt->second.Id, color.Id)
            << "MSAA-on: the pre-pass must NOT collapse View.Color into the resolve target";
        const auto resolveIt = fr->Textures.find({viewId, "View.Resolve"});
        ASSERT_NE(resolveIt, fr->Textures.end());
        const RenderGraph::RGTexture sceneColor = resolveIt->second;
        EXPECT_NE(sceneColor.Id, color.Id) << "View.Resolve = the pipeline SceneColor";
        EXPECT_EQ(frame.Graph().ResourceDesc(sceneColor.Id).SampleCount, 1u);

        const RenderGraph::RGAttachmentRec* worldColor = nullptr;
        for (const auto& rec : frame.Attachments())
            if (rec.Tex == color.Id && !rec.IsDepth)
                worldColor = &rec;
        ASSERT_NE(worldColor, nullptr) << "world attaches the multisampled caller color";
        EXPECT_EQ(worldColor->Resolve, sceneColor.Id)
            << "the world declares AttachColorResolve into SceneColor";

        const auto effIt = fr->Textures.find({viewId, "View.EffectiveColor"});
        ASSERT_NE(effIt, fr->Textures.end());
        EXPECT_EQ(effIt->second.Id, sceneColor.Id) << "EffectiveColor = the resolve";
        EXPECT_EQ(instance.GetOutputRG(viewId, "FinalColor").Id, sceneColor.Id);

        frame.MarkOutput(sceneColor);
        frame.Execute();
        device->WaitForIdle();
        rs.Shutdown();
    }
    device->Shutdown();
}

// ── SSSR world-MRT gate: the *.Written positive signal ──────────────────────
namespace
{
// ViewParamsUpload + WorldRender + ScreenSpaceReflections with the SSR
// G-buffer slices blueprint-declared, mirroring the shipped ForwardPlus graph:
// the raw View.NormalRoughness/View.SSRSpecularWeight names materialize valid whether
// or not anything writes them, which is exactly the ambiguity the provider's
// *.Written publishes disambiguate. View.SSRHiZ is declared here too (the
// shipped graph gets it from HZBBuild) so the node's non-G-buffer guards pass.
struct SssrGateRig
{
    RenderServices Services;
    RenderPipelineNodeRegistry Registry;
    std::unique_ptr<RenderPipelineInstance> Instance;
    std::unique_ptr<FramePools> Pools;
    std::unique_ptr<RenderGraph::RGFrame> Frame;
    Rendering::ViewId View = 0;
    Rendering::ViewId ViewB = 0; // second view when Init's samplesB > 0

    bool Init(Rendering::IDevice* device, uint32_t samples, uint32_t samplesB = 0,
              bool letterboxed = false)
    {
        if (!Services.Initialize(device))
            return false;
        const CameraId camId = Services.Views().AllocateCamera("SssrGateCam");
        CameraData cd{};
        for (int i = 0; i < 16; i += 5)
        {
            cd.view[i] = 1.0f;
            cd.proj[i] = 1.0f;
            cd.viewProj[i] = 1.0f;
        }
        Services.Views().SetCameraData(camId, cd);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        PostProcessSettings pp{};
        pp.SSSRIntensity = 1.0f; // defaults keep MaxDistance/MaxSteps active

        View = Services.Views().AllocateView("SssrGateView", camId);
        if (letterboxed)
        {
            ViewLetterbox box{};
            box.active = true;
            Services.Views().SetViewLetterbox(View, box);
        }
        Services.Views().SetViewRenderLayerMask(View, 1u);
        Services.Views().SetViewTargets(View, 0, 0, 0, clear);
        Services.Views().SetViewPostProcessOverride(View, pp);
        if (samplesB > 0)
        {
            ViewB = Services.Views().AllocateView("SssrGateViewB", camId);
            Services.Views().SetViewRenderLayerMask(ViewB, 1u);
            Services.Views().SetViewTargets(ViewB, 0, 0, 0, clear);
            Services.Views().SetViewPostProcessOverride(ViewB, pp);
        }
        Services.EnsureFeature<ImageBasedLightingFeature>().Initialize(device);

        if (!Registry.Register(
                "ViewParamsUpload",
                [] { return std::make_unique<Nodes::ViewParamsUploadNode>(); }, true))
            return false;
        if (!Registry.Register(
                "WorldRender", [] { return std::make_unique<Nodes::WorldRenderNode>(); }, true))
            return false;
        if (!Registry.Register(
                "ScreenSpaceReflections",
                [] { return std::make_unique<Nodes::ScreenSpaceReflectionsNode>(); }, true))
            return false;

        RenderPipelineBlueprint bp;
        bp.pipelineName = "SssrGate";
        auto addPass = [&bp](const char* id, const char* type, const char* json)
        {
            RenderPipelineBlueprint::Pass p;
            p.id = id;
            p.type = type;
            p.enabled = true;
            p.perView = true;
            p.passJson = json;
            bp.passes.push_back(p);
        };
        addPass("ViewParams", "ViewParamsUpload",
                R"({"id":"ViewParams","type":"ViewParamsUpload"})");
        addPass("World", "WorldRender", R"({"id":"World","type":"WorldRender"})");
        addPass("Sssr", "ScreenSpaceReflections",
                R"({"id":"Sssr","type":"ScreenSpaceReflections","input":"SceneColor","output":"HDRSSSR"})");
        bp.worldColorResolveTargetRef = "SceneColor";
        bp.resources.push_back(
            {"SceneColor",
             R"({"kind":"texture","scope":"perView","format":"rgba8_unorm","extent":{"scale":[1,1]},"usage":["renderTarget","shaderResource"]})"});
        bp.resources.push_back(
            {"HDRSSSR",
             R"({"kind":"texture","scope":"perView","format":"R16G16B16A16_FLOAT","extent":{"scale":[1,1]},"usage":["renderTarget","shaderResource","unorderedAccess"]})"});
        bp.resources.push_back(
            {"View.NormalRoughness",
             R"({"kind":"texture","scope":"perView","format":"R16G16B16A16_FLOAT","extent":{"scale":[1,1]},"usage":["renderTarget","shaderResource"]})"});
        bp.resources.push_back(
            {"View.SSRSpecularWeight",
             R"({"kind":"texture","scope":"perView","format":"R16G16B16A16_FLOAT","extent":{"scale":[1,1]},"usage":["renderTarget","shaderResource"]})"});
        bp.resources.push_back(
            {"View.SSRHiZ",
             R"({"kind":"texture","scope":"perView","format":"R32_FLOAT","extent":{"scale":[1,1]},"usage":["shaderResource","unorderedAccess"]})"});
        bp.outputs.push_back({"FinalColor", "HDRSSSR"});

        Instance = std::make_unique<RenderPipelineInstance>(Services, Registry);
        Instance->SetBlueprint(bp);

        Pools = std::make_unique<FramePools>(device);
        Frame = std::make_unique<RenderGraph::RGFrame>(device, &Pools->Persistent,
                                                       &Pools->Transient, &Pools->Ring);
        Frame->BeginFrame(0);

        const RenderGraph::RGTexture color =
            Frame->ImportPersistentTexture("SssrGate.Color", ColorTargetDesc(samples));
        const RenderGraph::RGTexture depth =
            Frame->ImportPersistentTexture("SssrGate.Depth", DepthTargetDesc(samples));

        Services.BeginWorldDrawFrame();
        Services.BuildWorldBatchKeys();

        std::vector<ViewTargetsRG> targets = {ViewTargetsRG{View, color, depth, {}}};
        if (ViewB != 0)
        {
            const RenderGraph::RGTexture colorB =
                Frame->ImportPersistentTexture("SssrGate.ColorB", ColorTargetDesc(samplesB));
            const RenderGraph::RGTexture depthB =
                Frame->ImportPersistentTexture("SssrGate.DepthB", DepthTargetDesc(samplesB));
            targets.push_back(ViewTargetsRG{ViewB, colorB, depthB, {}});
        }
        const std::vector<Rendering::ViewDesc> views(Services.Views().GetViews().begin(),
                                                     Services.Views().GetViews().end());
        Instance->Declare(*Frame, targets, views);
        return true;
    }

    RenderGraph::RGTexture Tex(const char* name) const { return TexFor(View, name); }

    RenderGraph::RGTexture TexFor(Rendering::ViewId view, const char* name) const
    {
        const auto* fr = Instance->FrameResourcesFor(Frame.get());
        if (!fr)
            return {};
        const auto it = fr->Textures.find({view, name});
        return it != fr->Textures.end() ? it->second : RenderGraph::RGTexture{};
    }

    bool Attached(RenderGraph::RGTexture t) const
    {
        for (const auto& rec : Frame->Attachments())
            if (!rec.IsDepth && rec.Tex == t.Id)
                return true;
        return false;
    }

    void Finish(Rendering::IDevice* device)
    {
        const auto out = Instance->GetOutputRG(View, "FinalColor");
        if (out.IsValid())
            Frame->MarkOutput(out);
        if (ViewB != 0)
        {
            const auto outB = Instance->GetOutputRG(ViewB, "FinalColor");
            if (outB.IsValid())
                Frame->MarkOutput(outB);
        }
        Frame->Execute();
        device->WaitForIdle();
        Services.Shutdown();
    }
};
} // namespace

// The MSAA refusal must be POSITIVE: under a multisampled world color the
// provider withholds the SSR MRT and the *.Written signal, and the SSSR node
// stitches its input through — even though the blueprint-declared G-buffer
// names still materialize as valid (never-written) textures.
TEST(RenderPipelineDeclareTests, SssrMsaaWithholdsWorldMrtAndWrittenSignalAndNodeStitches)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        SssrGateRig rig;
        ASSERT_TRUE(rig.Init(device.get(), 4));

        // The hole's precondition holds in this rig: the raw blueprint-declared
        // name resolves to a valid texture that no pass writes.
        const auto rawNr = rig.Tex("View.NormalRoughness");
        ASSERT_TRUE(rawNr.IsValid());

        EXPECT_FALSE(rig.Tex("View.NormalRoughness.Written").IsValid())
            << "provider must not publish the written signal under MSAA";
        EXPECT_FALSE(rig.Tex("View.SSRSpecularWeight.Written").IsValid());
        EXPECT_FALSE(rig.Tex("View.SSRSpecularRadiance.Written").IsValid());
        EXPECT_FALSE(rig.Attached(rawNr)) << "the SSR MRT must not attach under MSAA";

        const auto out = rig.Instance->GetOutputRG(rig.View, "FinalColor");
        const auto resolve = rig.Tex("View.Resolve");
        ASSERT_TRUE(out.IsValid());
        ASSERT_TRUE(resolve.IsValid());
        EXPECT_EQ(out.Id, resolve.Id)
            << "the SSSR node must stitch through, not run over the unwritten G-buffer";

        rig.Finish(device.get());
    }
    device->Shutdown();
}

// Single-sample control for the gate above: the provider attaches both slices
// and publishes the *.Written signal carrying the very textures it attached.
TEST(RenderPipelineDeclareTests, SssrSingleSampleAttachesMrtAndPublishesWrittenSignal)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        SssrGateRig rig;
        ASSERT_TRUE(rig.Init(device.get(), 1));

        const auto rawNr = rig.Tex("View.NormalRoughness");
        const auto rawSpecularWeight = rig.Tex("View.SSRSpecularWeight");
        ASSERT_TRUE(rawNr.IsValid());
        ASSERT_TRUE(rawSpecularWeight.IsValid());

        const auto writtenNr = rig.Tex("View.NormalRoughness.Written");
        const auto writtenSpecularWeight = rig.Tex("View.SSRSpecularWeight.Written");
        ASSERT_TRUE(writtenNr.IsValid())
            << "single-sample: the provider must publish the written signal";
        ASSERT_TRUE(writtenSpecularWeight.IsValid());
        EXPECT_EQ(writtenNr.Id, rawNr.Id) << "the signal carries the attached texture itself";
        EXPECT_EQ(writtenSpecularWeight.Id, rawSpecularWeight.Id);
        EXPECT_TRUE(rig.Attached(rawNr));
        EXPECT_TRUE(rig.Attached(rawSpecularWeight));
        const auto radiance = rig.Tex("View.SSRSpecularRadiance.Written");
        ASSERT_TRUE(radiance.IsValid());
        EXPECT_TRUE(rig.Attached(radiance));
        EXPECT_EQ(rig.Frame->Graph().ResourceDesc(radiance.Id).Format,
                  static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT));

        rig.Finish(device.get());
    }
    device->Shutdown();
}

// The node consumes the G-buffer ONLY through the written signal: with the
// signal present (and its other inputs met) it declares the chain and
// publishes the real HDRSSSR output instead of the stitch-through.
TEST(RenderPipelineDeclareTests, SssrNodeDeclaresChainOffWrittenSignalSingleSample)
{
    Rendering::ShaderPackage pkg{};
    std::string pkgError;
    if (!Rendering::LoadShaderPkg("Shaders/sssr_classify.shaderpkg", Rendering::ShaderSourceKind::SpirV, pkg, &pkgError))
        GTEST_SKIP() << "sssr shader packages not staged: " << pkgError;

    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        SssrGateRig rig;
        ASSERT_TRUE(rig.Init(device.get(), 1));

        auto* ibl = rig.Services.GetFeature<ImageBasedLightingFeature>();
        ASSERT_NE(ibl, nullptr);
        ASSERT_TRUE(ibl->IsInitialized() && ibl->GetPrefilterCube().IsValid())
            << "IBL fallback cubes must seed on Initialize (SSSR env fallback)";

        const auto out = rig.Instance->GetOutputRG(rig.View, "FinalColor");
        const auto resolve = rig.Tex("View.Resolve");
        ASSERT_TRUE(out.IsValid());
        ASSERT_TRUE(resolve.IsValid());
        EXPECT_NE(out.Id, resolve.Id)
            << "with the written signal present the node must declare its chain and "
               "publish the real HDRSSSR output, not the stitch-through";

        rig.Finish(device.get());
    }
    device->Shutdown();
}

TEST(RenderPipelineDeclareTests, SssrLetterboxedViewKeepsChainAndMotionSentinel)
{
    Rendering::ShaderPackage pkg{};
    std::string error;
    if (!Rendering::LoadShaderPkg("Shaders/sssr_classify.shaderpkg", Rendering::ShaderSourceKind::SpirV, pkg, &error))
        GTEST_SKIP() << "sssr shader packages not staged: " << error;
    auto device = CreateVulkanDeviceFast();
    if (!device) GTEST_SKIP() << "No Vulkan device available";
    {
        SssrGateRig rig;
        ASSERT_TRUE(rig.Init(device.get(), 1, 0, true));
        const auto out = rig.Instance->GetOutputRG(rig.View, "FinalColor");
        const auto resolve = rig.Tex("View.Resolve");
        ASSERT_TRUE(out.IsValid());
        EXPECT_NE(out.Id, resolve.Id);
        EXPECT_TRUE(rig.Tex("View.MotionVectors").IsValid());
        rig.Finish(device.get());
    }
    device->Shutdown();
}

// A letterboxed view rasterizes the world into a sub-rectangle of a full-size
// target, so a target UV is not an NDC of the camera's projection. The SSR
// kernels convert between the two on every ray, and they are handed this
// rectangle rather than assuming the viewport fills the target: without it the
// ray origin and the screen-space march disagree with the reflection direction
// the forward pass exports, and reflections drift toward the frame centre.
TEST(RenderPipelineDeclareTests, SssrViewportRectFollowsTheLetterbox)
{
    using Nodes::ScreenSpaceReflectionsNode;
    using GameEngine::Engine::Renderer::ViewLetterbox;

    // No letterbox: the whole target, so every conversion is the identity.
    const auto full = ScreenSpaceReflectionsNode::ComputeViewportRect(ViewLetterbox{}, 1920u, 1080u);
    EXPECT_FLOAT_EQ(full.X, 0.0f);
    EXPECT_FLOAT_EQ(full.Y, 0.0f);
    EXPECT_FLOAT_EQ(full.Width, 1.0f);
    EXPECT_FLOAT_EQ(full.Height, 1.0f);

    // 4:3 pillarboxed into a 16:9 target: bars left and right, full height.
    ViewLetterbox pillar{};
    pillar.active = true;
    pillar.x = 240u;
    pillar.y = 0u;
    pillar.width = 1440u;
    pillar.height = 1080u;
    const auto rect = ScreenSpaceReflectionsNode::ComputeViewportRect(pillar, 1920u, 1080u);
    EXPECT_FLOAT_EQ(rect.X, 0.125f);
    EXPECT_FLOAT_EQ(rect.Y, 0.0f);
    EXPECT_FLOAT_EQ(rect.Width, 0.75f);
    EXPECT_FLOAT_EQ(rect.Height, 1.0f);
    // The rectangle must be the one a UV round trip through the kernels uses:
    // the target UV of the viewport's left edge maps to NDC -1, its right to +1.
    EXPECT_FLOAT_EQ(rect.X + rect.Width, 0.875f);

    // A letterbox the view registered but never sized cannot scale anything;
    // falling back to the whole target keeps the kernels' conversions finite.
    ViewLetterbox degenerate{};
    degenerate.active = true;
    const auto fallback =
        ScreenSpaceReflectionsNode::ComputeViewportRect(degenerate, 1920u, 1080u);
    EXPECT_FLOAT_EQ(fallback.Width, 1.0f);
    EXPECT_FLOAT_EQ(fallback.Height, 1.0f);
}

// The kernels' half of the same contract: the UV/NDC conversions and the edge
// fade must read the rectangle, or the C++ above is handing it to nobody.
TEST(RenderPipelineDeclareTests, SssrKernelsConvertUvThroughTheViewportRect)
{
    const std::string common =
        GE::Tests::ReadSssrShaderSource("ScreenSpaceReflections/sssr_common.glsl");
    ASSERT_FALSE(common.empty()) << "sssr_common.glsl not found via GE_RENDERER_REPO_ROOT";
    EXPECT_NE(common.find("vec4 viewportRect;"), std::string::npos);
    EXPECT_NE(common.find("vec2 v = (uv - sssr.viewportRect.xy) / sssr.viewportRect.zw;"),
              std::string::npos)
        << "GE_UvToNdc must map the target UV through the viewport rectangle";
    EXPECT_NE(common.find("return sssr.viewportRect.xy + v * sssr.viewportRect.zw;"),
              std::string::npos)
        << "GE_NdcToUv must map back into the viewport rectangle";
    const std::string intersect =
        GE::Tests::ReadSssrShaderSource("ScreenSpaceReflections/sssr_intersect.comp");
    ASSERT_FALSE(intersect.empty());
    EXPECT_NE(intersect.find("ge_screenSize.y * sssr.viewportRect.w"), std::string::npos)
        << "the refine budget's pixel size must use the viewport height, not the target's";
}

// Per-(view,name) isolation of the written signal: two views in ONE Declare,
// view A multisampled and view B single-sample. B's publish must not satisfy
// A — A stitches through while B, in the same frame, attaches the MRT and
// carries the signal for its own textures. A frame-scope (view 0) publish or
// any cross-view key collapse would run A's chain over its unwritten G-buffer.
TEST(RenderPipelineDeclareTests, SssrWrittenSignalIsPerViewAcrossMixedSampleViews)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        SssrGateRig rig;
        ASSERT_TRUE(rig.Init(device.get(), 4, 1));

        // View A (MSAA): no signal, no attach, stitched output — even though
        // view B published the same names this frame.
        EXPECT_FALSE(rig.TexFor(rig.View, "View.NormalRoughness.Written").IsValid())
            << "view B's publish must not appear under view A's key";
        const auto rawNrA = rig.TexFor(rig.View, "View.NormalRoughness");
        ASSERT_TRUE(rawNrA.IsValid());
        EXPECT_FALSE(rig.Attached(rawNrA));
        const auto outA = rig.Instance->GetOutputRG(rig.View, "FinalColor");
        const auto resolveA = rig.TexFor(rig.View, "View.Resolve");
        ASSERT_TRUE(outA.IsValid());
        ASSERT_TRUE(resolveA.IsValid());
        EXPECT_EQ(outA.Id, resolveA.Id)
            << "view A must stitch through despite view B's same-frame signal";

        // View B (single-sample): signal present and carrying B's own texture.
        const auto writtenNrB = rig.TexFor(rig.ViewB, "View.NormalRoughness.Written");
        const auto rawNrB = rig.TexFor(rig.ViewB, "View.NormalRoughness");
        ASSERT_TRUE(writtenNrB.IsValid());
        ASSERT_TRUE(rawNrB.IsValid());
        EXPECT_EQ(writtenNrB.Id, rawNrB.Id);
        EXPECT_NE(rawNrB.Id, rawNrA.Id) << "per-view G-buffer slices must be distinct";
        EXPECT_TRUE(rig.Attached(rawNrB));

        rig.Finish(device.get());
    }
    device->Shutdown();
}

// Slice-5a backfill — DepthResolveNode's Declare shape (F7/F8 pins): the
// node publishes View.DepthResolved as a SINGLE-SAMPLE R32F pool texture
// distinct from the raw depth, for both the MSAA resolve variant and the
// single-sample copy variant (ge_sceneDepth must always be sampler2D-safe).
TEST(RenderPipelineDeclareTests, DepthResolveDeclaresComputeAndPublishesDepthResolved)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        const CameraId camId = rs.Views().AllocateCamera("DrCam");
        const ViewId viewId = rs.Views().AllocateView("DrView", camId);
        rs.Views().SetViewRenderLayerMask(viewId, 1u);
        rs.Views().SetViewTargets(viewId, 0, 0, 0, Rendering::ViewClearConfig{});

        RenderPipelineNodeRegistry registry;
        ASSERT_TRUE(registry.Register(
            "DepthResolve", [] { return std::make_unique<Nodes::DepthResolveNode>(); }, true));

        RenderPipelineBlueprint bp;
        bp.pipelineName = "DrTest";
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "DepthResolve";
            p.type = "DepthResolve";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"DepthResolve","type":"DepthResolve"})";
            bp.passes.push_back(p);
        }

        RenderPipelineInstance instance(rs, registry);
        instance.SetBlueprint(bp);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

        auto declareWith = [&](uint64_t frameIdx, uint32_t depthSamples) -> RenderGraph::RGTexture
        {
            frame.BeginFrame(frameIdx);
            RenderGraph::RGTexture color = frame.ImportPersistentTexture("DR.Color", ColorTargetDesc());
            RenderGraph::RGTexture depth =
                frame.ImportPersistentTexture(depthSamples > 1 ? "DR.DepthMS" : "DR.Depth",
                                              DepthTargetDesc(depthSamples));
            rs.BeginWorldDrawFrame();
            const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{viewId, color, depth, {}}};
            const std::vector<Rendering::ViewDesc> views(rs.Views().GetViews().begin(),
                                                         rs.Views().GetViews().end());
            instance.Declare(frame, targets, views);
            return depth;
        };

        // MSAA depth: the resolve variant declares + publishes.
        const RenderGraph::RGTexture depthMS = declareWith(0, 4);
        if (frame.Graph().PassCount() == 0)
            GTEST_SKIP() << "depth_resolve shaders unavailable in this environment";
        EXPECT_EQ(frame.Graph().PassCount(), 1u);
        {
            const auto* fr = instance.FrameResourcesFor(&frame);
            ASSERT_NE(fr, nullptr);
            const auto it = fr->Textures.find({viewId, "View.DepthResolved"});
            ASSERT_NE(it, fr->Textures.end()) << "F7: View.DepthResolved must be PUBLISHED";
            EXPECT_NE(it->second.Id, depthMS.Id) << "published texture, not the raw alias";
            const auto& rd = frame.Graph().ResourceDesc(it->second.Id);
            EXPECT_EQ(rd.SampleCount, 1u) << "F8: the resolved depth is single-sample";
            EXPECT_EQ(rd.Format, static_cast<uint32_t>(TextureFormat::R32_FLOAT));
            EXPECT_TRUE(frame.Graph().HasReadAccess(0, depthMS.Id))
                << "the resolve pass reads the raw depth";
        }
        frame.Execute();
        device->WaitForIdle();

        // Single-sample depth: the COPY variant still declares (ge_sceneDepth
        // samples the R32F copy at any MSAA level) and publishes the same
        // single-sample contract.
        const RenderGraph::RGTexture depth1 = declareWith(1, 1);
        EXPECT_EQ(frame.Graph().PassCount(), 1u)
            << "single-sample depth declares the copy variant";
        {
            const auto* fr = instance.FrameResourcesFor(&frame);
            ASSERT_NE(fr, nullptr);
            const auto it = fr->Textures.find({viewId, "View.DepthResolved"});
            ASSERT_NE(it, fr->Textures.end());
            EXPECT_NE(it->second.Id, depth1.Id);
            EXPECT_EQ(frame.Graph().ResourceDesc(it->second.Id).SampleCount, 1u);
        }
        frame.Execute();
        device->WaitForIdle();
        rs.Shutdown();
    }
    device->Shutdown();
}

// Slice-5a backfill — the binding-table seam, spine-driven: a device-local
// blueprint buffer written by an earlier node must reach the world pass as a
// declared Read (RAW edge through BuildPassResourcesRG + the world's
// ForEachBufferBinding loop), which only happens via the spine's pipeline
// instance (m_PipelineInstanceRG) — local-instance tests can't see this.
TEST(RenderPipelineDeclareTests, DeviceLocalPipelineBufferComputeWriterOrdersBeforeWorldRead)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        const CameraId camId = rs.Views().AllocateCamera("SeamCam");
        const ViewId viewId = rs.Views().AllocateView("SeamView", camId);
        rs.Views().SetViewRenderLayerMask(viewId, 1u);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);

        ASSERT_TRUE(rs.Spine().GetPipelineNodeRegistry()->Register(
            "BufferWriter", [] { return std::make_unique<BufferWriterNode>(); }, true));

        RenderPipelineBlueprint bp;
        bp.pipelineName = "SeamTest";
        bp.contentHash = 0x5EA75EA7ull;
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "Writer";
            p.type = "BufferWriter";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"Writer","type":"BufferWriter"})";
            bp.passes.push_back(p);
        }
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "World";
            p.type = "WorldRender";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"World","type":"WorldRender"})";
            bp.passes.push_back(p);
        }
        bp.resources.push_back(
            {"ClusterBuffer",
             R"({"kind":"buffer","scope":"perView","memoryUsage":"deviceLocal","usage":["storage"],"size":{"bytes":4096}})"});
        bp.outputs.push_back({"FinalColor", "View.Resolve"});
        rs.Spine().SetActiveRenderPipelineBlueprint(bp);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        GameEngine::Testing::ScopedPipelineFrame frameRegistration(rs.Spine(), frame);
        frame.BeginFrame(0);

        RenderGraph::RGTexture color = frame.ImportPersistentTexture("Seam.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("Seam.Depth", DepthTargetDesc());

        BufferWriterNode::s_Pass = {};
        BufferWriterNode::s_Binding = {};

        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();

        const ViewTargetsRG vt{viewId, color, depth, {}};
        RenderServices::FrameGraphBuildParamsRG params{};
        params.ViewTargets = std::span<const ViewTargetsRG>(&vt, 1);
        rs.Spine().BuildFrameGraph(frame, params);

        ASSERT_TRUE(BufferWriterNode::s_Pass.IsValid()) << "writer declared its compute pass";
        ASSERT_TRUE(BufferWriterNode::s_Binding.Graph.IsValid())
            << "device-local blueprint buffer materialized with a graph id";

        // Find the world pass via its color attachment.
        RenderGraph::RGPassId worldPass = 0;
        bool foundWorld = false;
        for (const auto& rec : frame.Attachments())
        {
            if (rec.Tex == color.Id && !rec.IsDepth)
            {
                worldPass = rec.Pass;
                foundWorld = true;
            }
        }
        ASSERT_TRUE(foundWorld);

        EXPECT_TRUE(frame.Graph().HasReadAccess(worldPass, BufferWriterNode::s_Binding.Graph.Id))
            << "the world pass declares the RAW read on the pipeline buffer";

        frame.Execute();
        device->WaitForIdle();

        EXPECT_LT(ScheduledIndexOf(frame.Graph(), BufferWriterNode::s_Pass.Id),
                  ScheduledIndexOf(frame.Graph(), worldPass))
            << "compute writer scheduled before the world's read";

        rs.Shutdown();
    }
    device->Shutdown();
}

namespace
{
// Shared stand-up for the 5b FullscreenShader chain tests: World (collapsing
// into the pipeline SceneColor) + N FullscreenShader stages from raw JSON.
struct FxChainFixture
{
    std::unique_ptr<Rendering::IDevice> Device;
    std::unique_ptr<RenderServices> Rs;
    RenderPipelineNodeRegistry Registry;
    ViewId View{};

    bool Up()
    {
        Device = CreateVulkanDeviceFast();
        if (!Device)
            return false;
        Rs = std::make_unique<RenderServices>();
        if (!Rs->Initialize(Device.get()))
            return false;
        const CameraId camId = Rs->Views().AllocateCamera("FxCam");
        View = Rs->Views().AllocateView("FxView", camId);
        Rs->Views().SetViewRenderLayerMask(View, 1u);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        Rs->Views().SetViewTargets(View, 0, 0, 0, clear);
        if (!Registry.Register(
                "WorldRender", [] { return std::make_unique<Nodes::WorldRenderNode>(); }, true))
            return false;
        if (!Registry.Register(
                "FullscreenShader", [] { return std::make_unique<Nodes::FullscreenShaderNode>(); },
                true))
            return false;
        if (!Registry.Register(
                "RenderScaleUpscale",
                [] { return std::make_unique<Nodes::RenderScaleUpscaleNode>(); }, true))
            return false;
        return true;
    }

    static RenderPipelineBlueprint::Pass FxPass(const char* id, const std::string& json)
    {
        RenderPipelineBlueprint::Pass p;
        p.id = id;
        p.type = "FullscreenShader";
        p.enabled = true;
        p.perView = true;
        p.passJson = json;
        return p;
    }

    static RenderPipelineBlueprint::Pass WorldPass()
    {
        RenderPipelineBlueprint::Pass p;
        p.id = "World";
        p.type = "WorldRender";
        p.enabled = true;
        p.perView = true;
        p.passJson = R"({"id":"World","type":"WorldRender"})";
        return p;
    }

    static void AddTexResource(RenderPipelineBlueprint& bp, const char* name)
    {
        bp.resources.push_back(
            {name,
             R"({"kind":"texture","scope":"perView","format":"rgba8_unorm","extent":{"scale":[1,1]},"usage":["renderTarget","shaderResource"]})"});
    }

    // Display-extent sibling of AddTexResource: under an active split these
    // materialize at the caller's extent while the render-basis ones stay
    // internal, which is what puts a stage on one side of the crossing.
    static void AddOutputBasisTexResource(RenderPipelineBlueprint& bp, const char* name)
    {
        bp.resources.push_back(
            {name,
             R"({"kind":"texture","scope":"perView","format":"rgba8_unorm","extent":{"basis":"output"},"usage":["renderTarget","shaderResource"]})"});
    }

    void Down()
    {
        Rs->Shutdown();
        Device->Shutdown();
    }
};
} // namespace

// 5b — multi-hop chain stitching: every gated stage skipped at declaration
// threads its input through, so FinalColor resolves all the way back to the
// world's SceneColor and no skipped stage materializes a pool texture.
TEST(RenderPipelineDeclareTests, PostFxChainStitchesSkippedStagesInBlueprintOrder)
{
    FxChainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderPipelineBlueprint bp;
        bp.pipelineName = "ChainTest";
        bp.passes.push_back(FxChainFixture::WorldPass());
        bp.passes.push_back(FxChainFixture::FxPass(
            "Fx1",
            R"({"id":"Fx1","type":"FullscreenShader","inputs":{"uTex":"SceneColor"},"output":"PostA","shaderPkg":"Shaders/copy.shaderpkg","skipWhen":{"crtIntensity":0.0}})"));
        bp.passes.push_back(FxChainFixture::FxPass(
            "Fx2",
            R"({"id":"Fx2","type":"FullscreenShader","inputs":{"uTex":"PostA"},"output":"View.Resolve","shaderPkg":"Shaders/copy.shaderpkg","skipWhen":{"crtIntensity":0.0}})"));
        bp.worldColorResolveTargetRef = "SceneColor";
        FxChainFixture::AddTexResource(bp, "SceneColor");
        FxChainFixture::AddTexResource(bp, "PostA");
        bp.outputs.push_back({"FinalColor", "View.Resolve"});

        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);

        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("CH.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("CH.Depth", DepthTargetDesc());

        f.Rs->BeginWorldDrawFrame();
        f.Rs->BuildWorldBatchKeys();
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{f.View, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                     f.Rs->Views().GetViews().end());
        instance.Declare(frame, targets, views);

        EXPECT_EQ(frame.Graph().PassCount(), 1u) << "only the world declares — both FX skipped";

        // FinalColor resolves THROUGH both stitches to the world's write.
        const auto* fr = instance.FrameResourcesFor(&frame);
        ASSERT_NE(fr, nullptr);
        const auto effIt = fr->Textures.find({f.View, "View.EffectiveColor"});
        ASSERT_NE(effIt, fr->Textures.end());
        const RenderGraph::RGTexture sceneColor = effIt->second;
        EXPECT_EQ(instance.GetOutputRG(f.View, "FinalColor").Id, sceneColor.Id)
            << "View.Resolve -> PostA -> SceneColor through the stitches";

        // No dead pool alloc for the skipped stage's output: caller color +
        // depth + SceneColor only.
        EXPECT_EQ(pools.Persistent.Size(), 3u) << "skipped PostA never materializes";

        frame.MarkOutput(sceneColor);
        frame.Execute();
        f.Device->WaitForIdle();
    }
    f.Down();
}

// Exposure-history contract: "ExposureHistory" is a blueprint RESOURCE, so a
// consumer that binds it BEFORE the AutoExposure node declares (BloomThreshold)
// materializes it on first resolve — stage survival no longer depends on a
// declare node appearing earlier in blueprint order. The creation frame
// schedules a declared zero-fill (recycled pool memory must never read as a
// plausible exposure); steady-state frames must NOT re-fill (the buffer is the
// cross-frame adaptation state).
TEST(RenderPipelineDeclareTests, ExposureHistoryResourceMaterializesAndZeroInitsOnCreate)
{
    FxChainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderPipelineBlueprint bp;
        bp.pipelineName = "ExpRes";
        bp.passes.push_back(FxChainFixture::WorldPass());
        // BloomThreshold-shaped consumer: binds the buffer with NO AutoExposure
        // node anywhere in the blueprint — the resource entry alone must carry it.
        bp.passes.push_back(FxChainFixture::FxPass(
            "Bloomish",
            R"({"id":"Bloomish","type":"FullscreenShader","inputs":{"uTex":"SceneColor"},"buffers":{"uExposure":"ExposureHistory"},"output":"PostA","shaderPkg":"Shaders/copy.shaderpkg"})"));
        bp.worldColorResolveTargetRef = "SceneColor";
        FxChainFixture::AddTexResource(bp, "SceneColor");
        FxChainFixture::AddTexResource(bp, "PostA");
        bp.resources.push_back(
            {"ExposureHistory",
             R"({"kind":"buffer","scope":"perView","size":{"bytes":16},"usage":["storage"],"zeroOnCreate":true})"});

        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);
        FramePools pools(f.Device.get());

        struct ExpFrameResult
        {
            uint32_t ZeroInitPasses = 0;
            uint32_t FxPasses = 0;
            uint64_t BufferSize = 0;
            bool BufferGraphValid = false;
        };
        auto declareFrame = [&](uint64_t frameIndex, bool execute) -> ExpFrameResult
        {
            RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient,
                                       &pools.Ring);
            frame.BeginFrame(frameIndex);
            RenderGraph::RGTexture color =
                frame.ImportPersistentTexture("ER.Color", ColorTargetDesc());
            RenderGraph::RGTexture depth =
                frame.ImportPersistentTexture("ER.Depth", DepthTargetDesc());
            f.Rs->BeginWorldDrawFrame();
            f.Rs->BuildWorldBatchKeys();
            const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{f.View, color, depth, {}}};
            const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                         f.Rs->Views().GetViews().end());
            instance.Declare(frame, targets, views);

            ExpFrameResult r;
            for (size_t p = 0; p < frame.Graph().PassCount(); ++p)
            {
                const std::string name(
                    frame.Graph().PassName(static_cast<RenderGraph::RGPassId>(p)));
                if (RGQuery::Matches(name, RGQuery::Subtree{"ZeroInit.ExposureHistory"}))
                    ++r.ZeroInitPasses;
                if (RGQuery::Matches(name, RGQuery::Family{"Bloomish"}))
                    ++r.FxPasses;
            }
            if (const auto* fr = instance.FrameResourcesFor(&frame))
            {
                if (auto it = fr->Buffers.find({f.View, "ExposureHistory"});
                    it != fr->Buffers.end())
                {
                    r.BufferSize = it->second.Size;
                    r.BufferGraphValid = it->second.Graph.IsValid();
                }
            }
            if (execute)
            {
                frame.MarkOutput(color);
                frame.Execute();
                f.Device->WaitForIdle();
            }
            return r;
        };

        // Frame 0 declares the creation-frame fill but is ABANDONED before
        // Execute (the swapchain-acquire-failure shape) — the pool arm must
        // survive so a later executed frame still zero-fills.
        const ExpFrameResult f0 = declareFrame(0, /*execute=*/false);
        if (f0.FxPasses == 0)
            GTEST_SKIP() << "copy.shaderpkg unavailable in this environment";

        EXPECT_EQ(f0.BufferSize, 16u) << "resource entry materialized at its declared size";
        EXPECT_TRUE(f0.BufferGraphValid) << "materialized buffer carries a graph edge";
        EXPECT_EQ(f0.ZeroInitPasses, 1u) << "creation frame zero-fills the pool buffer";

        const ExpFrameResult f1 = declareFrame(1, /*execute=*/true);
        EXPECT_EQ(f1.FxPasses, 1u);
        EXPECT_EQ(f1.ZeroInitPasses, 1u)
            << "an abandoned creation frame must NOT discharge the arm — the first "
               "EXECUTED frame re-schedules the fill";

        const ExpFrameResult f2 = declareFrame(2, /*execute=*/true);
        EXPECT_EQ(f2.FxPasses, 1u) << "steady-state frame still declares the consumer";
        EXPECT_EQ(f2.BufferSize, 16u);
        EXPECT_EQ(f2.ZeroInitPasses, 0u)
            << "reused pool buffer must NOT re-fill — it is cross-frame adaptation state";
    }
    f.Down();
}

// Unlit particles and emitters with an exposure weight below 1 divide by the view's exposure
// (view_exposure.glsl), whose metered scale is in the view's ExposureHistory. The world pass and the
// late transparent pass run before the metering node declares, so each must materialize the buffer
// itself: then the pass binds it by name and declares the read that orders it against the metering
// write. Without that the pass binds the zero fallback and auto exposure never reaches what it
// draws. Declared through the spine, which owns the pipeline instance the passes look their buffers
// up in.
TEST(RenderPipelineDeclareTests, WorldAndLateTransparentPassesReadTheViewsExposureHistory)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        const CameraId camId = rs.Views().AllocateCamera("LateExposureCam");
        const ViewId viewId = rs.Views().AllocateView("LateExposureView", camId);
        rs.Views().SetViewRenderLayerMask(viewId, 1u);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);

        RenderPipelineBlueprint bp;
        bp.pipelineName = "LateExposure";
        bp.contentHash = 0x1A7E0E4Bull;
        for (const char* type : {"WorldRender", "TransmissiveRender"})
        {
            RenderPipelineBlueprint::Pass p;
            p.id = type;
            p.type = type;
            p.enabled = true;
            p.perView = true;
            p.passJson = std::string(R"({"id":")") + type + R"(","type":")" + type + R"("})";
            bp.passes.push_back(p);
        }
        bp.resources.push_back(
            {"ExposureHistory",
             R"({"kind":"buffer","scope":"perView","size":{"bytes":16},"usage":["storage"],"zeroOnCreate":true})"});
        bp.outputs.push_back({"FinalColor", "View.Resolve"});
        rs.Spine().SetActiveRenderPipelineBlueprint(bp);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        GameEngine::Testing::ScopedPipelineFrame frameRegistration(rs.Spine(), frame);
        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("LE.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("LE.Depth", DepthTargetDesc());
        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();
        // Declaration never inspects a late command's payload; one in the stream declares the pass.
        rs.EmitLateForwardCommand(viewId, DrawCommand{});
        const ViewTargetsRG vt{viewId, color, depth, {}};
        RenderServices::FrameGraphBuildParamsRG params{};
        params.ViewTargets = std::span<const ViewTargetsRG>(&vt, 1);
        rs.Spine().BuildFrameGraph(frame, params);

        const auto latePasses = RGQuery::DeclaredIds(frame.Graph(), RGQuery::Subtree{"LateTransparent"});
        ASSERT_EQ(latePasses.size(), 1u) << "positive control: the late command declares the pass";
        const auto* instance = rs.Spine().PipelineInstanceForFrame(frame);
        ASSERT_NE(instance, nullptr);
        const auto* resources = instance->FrameResourcesFor(&frame);
        ASSERT_NE(resources, nullptr);
        const auto history = resources->Buffers.find({viewId, "ExposureHistory"});
        ASSERT_NE(history, resources->Buffers.end())
            << "the late pass materializes the view's exposure history for the particles it draws";
        ASSERT_TRUE(history->second.Graph.IsValid());
        EXPECT_TRUE(frame.Graph().HasReadAccess(latePasses.front(), history->second.Graph.Id))
            << "the read orders the pass against the metering node's write";
        const auto worldPasses = RGQuery::DeclaredIds(frame.Graph(), RGQuery::Subtree{"RenderEntities"});
        ASSERT_EQ(worldPasses.size(), 1u) << "positive control: the view clears declare the world pass";
        EXPECT_TRUE(frame.Graph().HasReadAccess(worldPasses.front(), history->second.Graph.Id))
            << "the world pass binds the metered exposure, not the zero fallback";

        frame.Execute();
        device->WaitForIdle();
        rs.Shutdown();
    }
    device->Shutdown();
}

// Legacy-graph fallback: without a resource entry, AutoExposureNode itself
// imports + publishes "ExposureHistory" (zero-filled on creation), so a
// Tonemap-shaped consumer declared AFTER it still resolves the buffer and its
// stage survives. This is what keeps tonemapping alive for rendergraphs that
// predate the resource entry.
TEST(RenderPipelineDeclareTests, AutoExposureFallbackPublishesExposureHistoryWithoutResourceEntry)
{
    FxChainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(f.Registry.Register(
        "AutoExposure", [] { return std::make_unique<Nodes::AutoExposureNode>(); }, true));
    {
        RenderPipelineBlueprint bp;
        bp.pipelineName = "ExpFallback";
        bp.passes.push_back(FxChainFixture::WorldPass());
        RenderPipelineBlueprint::Pass ae;
        ae.id = "AutoExposure";
        ae.type = "AutoExposure";
        ae.enabled = true;
        ae.perView = true;
        ae.passJson = R"({"id":"AutoExposure","type":"AutoExposure"})";
        bp.passes.push_back(ae);
        bp.passes.push_back(FxChainFixture::FxPass(
            "Tonemapish",
            R"({"id":"Tonemapish","type":"FullscreenShader","inputs":{"uTex":"SceneColor"},"buffers":{"uExposure":"ExposureHistory"},"output":"PostA","shaderPkg":"Shaders/copy.shaderpkg"})"));
        bp.worldColorResolveTargetRef = "SceneColor";
        FxChainFixture::AddTexResource(bp, "SceneColor");
        FxChainFixture::AddTexResource(bp, "PostA");
        // Deliberately NO "ExposureHistory" resource entry.

        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);
        FramePools pools(f.Device.get());

        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient,
                                   &pools.Ring);
        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("EF.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("EF.Depth", DepthTargetDesc());
        f.Rs->BeginWorldDrawFrame();
        f.Rs->BuildWorldBatchKeys();
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{f.View, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                     f.Rs->Views().GetViews().end());
        instance.Declare(frame, targets, views);

        uint32_t fxPasses = 0;
        uint32_t zeroInitPasses = 0;
        for (size_t p = 0; p < frame.Graph().PassCount(); ++p)
        {
            const std::string name(frame.Graph().PassName(static_cast<RenderGraph::RGPassId>(p)));
            if (RGQuery::Matches(name, RGQuery::Family{"Tonemapish"}))
                ++fxPasses;
            if (RGQuery::Matches(name, RGQuery::Subtree{"ZeroInit.Pipeline.AutoExposure.History"}))
                ++zeroInitPasses;
        }
        if (fxPasses == 0)
            GTEST_SKIP() << "copy.shaderpkg unavailable in this environment";

        const auto* fr = instance.FrameResourcesFor(&frame);
        ASSERT_NE(fr, nullptr);
        const auto it = fr->Buffers.find({f.View, "ExposureHistory"});
        ASSERT_NE(it, fr->Buffers.end()) << "fallback publish must exist for legacy graphs";
        EXPECT_EQ(it->second.Size, 16u);
        EXPECT_EQ(zeroInitPasses, 1u) << "fallback import zero-fills on creation too";

        frame.MarkOutput(color);
        frame.Execute();
        f.Device->WaitForIdle();
    }
    f.Down();
}

// 1b under-size guard: a blueprint that declares "ExposureHistory" SMALLER than
// AutoExposureNode's fixed 16-byte state struct trips a warn-once (log-only —
// there is no exposed counter, so the log line itself isn't asserted here). The
// observable contract this pins: the node still resolves + publishes the
// (undersized) buffer and Declare survives across frames — the guard warns
// without dropping the stage or crashing. The buffer materializing at the
// too-small 8 bytes proves the under-size branch was reached. Frames are
// declared but NOT executed (no GPU write to the tiny buffer).
TEST(RenderPipelineDeclareTests, AutoExposureUnderSizedExposureHistoryWarnsButStillDeclares)
{
    FxChainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(f.Registry.Register(
        "AutoExposure", [] { return std::make_unique<Nodes::AutoExposureNode>(); }, true));
    {
        RenderPipelineBlueprint bp;
        bp.pipelineName = "ExpUnderSize";
        bp.passes.push_back(FxChainFixture::WorldPass());
        RenderPipelineBlueprint::Pass ae;
        ae.id = "AutoExposure";
        ae.type = "AutoExposure";
        ae.enabled = true;
        ae.perView = true;
        ae.passJson = R"({"id":"AutoExposure","type":"AutoExposure"})";
        bp.passes.push_back(ae);
        // ExposureHistory declared DELIBERATELY too small (8 < the 16-byte state).
        bp.resources.push_back(
            {"ExposureHistory",
             R"({"kind":"buffer","scope":"perView","size":{"bytes":8},"usage":["storage"],"zeroOnCreate":true})"});

        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);
        FramePools pools(f.Device.get());

        auto declareOnly = [&](uint64_t frameIndex) -> uint64_t
        {
            RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient,
                                       &pools.Ring);
            frame.BeginFrame(frameIndex);
            RenderGraph::RGTexture color =
                frame.ImportPersistentTexture("EU.Color", ColorTargetDesc());
            RenderGraph::RGTexture depth =
                frame.ImportPersistentTexture("EU.Depth", DepthTargetDesc());
            f.Rs->BeginWorldDrawFrame();
            f.Rs->BuildWorldBatchKeys();
            const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{f.View, color, depth, {}}};
            const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                         f.Rs->Views().GetViews().end());
            instance.Declare(frame, targets, views); // not executed
            uint64_t size = 0;
            if (const auto* fr = instance.FrameResourcesFor(&frame))
                if (auto it = fr->Buffers.find({f.View, "ExposureHistory"}); it != fr->Buffers.end())
                    size = it->second.Size;
            return size;
        };

        EXPECT_EQ(declareOnly(0), 8u) << "ExposureHistory materialized undersized -> guard reached";
        EXPECT_EQ(declareOnly(1), 8u) << "warn-once repeat frame stays stable";
    }
    f.Down();
}

// 5c — FinalCopy's structural elision: a pure single-input copy whose
// chain-var input is already single-sample threads the chain instead of
// declaring (FinalColor = the last-written intermediate; no copy pass, no
// extra fullscreen blit). The flag alone is not sufficient — a multi-input
// stage with the same flag still declares.
TEST(RenderPipelineDeclareTests, FinalCopyElidesWhenChainVarIsSingleSample)
{
    FxChainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderPipelineBlueprint bp;
        bp.pipelineName = "ElideTest";
        bp.passes.push_back(FxChainFixture::WorldPass());
        bp.passes.push_back(FxChainFixture::FxPass(
            "Fx",
            R"({"id":"Fx","type":"FullscreenShader","inputs":{"uTex":"SceneColor"},"output":"LDRFinal","shaderPkg":"Shaders/copy.shaderpkg"})"));
        bp.passes.push_back(FxChainFixture::FxPass(
            "FinalCopy",
            R"({"id":"FinalCopy","type":"FullscreenShader","elideWhenIdentity":true,"inputs":{"uTex":"LDRFinal"},"output":"View.Resolve","shaderPkg":"Shaders/copy.shaderpkg"})"));
        bp.passes.push_back(FxChainFixture::FxPass(
            "NotElidable",
            R"({"id":"NotElidable","type":"FullscreenShader","elideWhenIdentity":true,"inputs":{"uA":"LDRFinal","uB":"SceneColor"},"output":"PostB","shaderPkg":"Shaders/copy.shaderpkg"})"));
        bp.worldColorResolveTargetRef = "SceneColor";
        FxChainFixture::AddTexResource(bp, "SceneColor");
        FxChainFixture::AddTexResource(bp, "LDRFinal");
        FxChainFixture::AddTexResource(bp, "PostB");
        bp.outputs.push_back({"FinalColor", "View.Resolve"});

        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);

        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("EL.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("EL.Depth", DepthTargetDesc());

        f.Rs->BeginWorldDrawFrame();
        f.Rs->BuildWorldBatchKeys();
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{f.View, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                     f.Rs->Views().GetViews().end());
        instance.Declare(frame, targets, views);

        if (frame.Graph().PassCount() < 2u)
            GTEST_SKIP() << "copy.shaderpkg unavailable in this environment";

        // World + Fx + NotElidable declared; FinalCopy elided.
        EXPECT_EQ(frame.Graph().PassCount(), 3u)
            << "the identity copy elides; the multi-input stage with the flag does NOT";

        // FinalColor = the last-written intermediate, not a copy of it.
        const RenderGraph::RGTexture ldrFinal = instance.GetOutputRG(f.View, "FinalColor");
        ASSERT_TRUE(ldrFinal.IsValid());
        const auto* fr = instance.FrameResourcesFor(&frame);
        ASSERT_NE(fr, nullptr);
        const auto ldrIt = fr->Textures.find({f.View, "LDRFinal"});
        ASSERT_NE(ldrIt, fr->Textures.end());
        EXPECT_EQ(ldrFinal.Id, ldrIt->second.Id)
            << "View.Resolve threads to LDRFinal — the copy added nothing";

        frame.MarkOutput(ldrFinal);
        frame.Execute();
        f.Device->WaitForIdle();
    }
    f.Down();
}

// The identity elision must not swallow a RESAMPLE. Under an active split the
// terminal copy's input sits at the display extent (output basis) while
// View.Resolve is still the internal SceneColor: eliding there would publish the
// finished display-res image under a name every consumer reads at the internal
// extent — in this blueprint's shape, a downscale of the final frame. Same
// blueprint at scale 1.0 is the control: extents agree, the copy elides, and the
// pre-split behaviour is untouched.
TEST(RenderPipelineDeclareTests, IdentityElisionDoesNotElideAResample)
{
    FxChainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderPipelineBlueprint bp;
        bp.pipelineName = "ElideResampleTest";
        bp.passes.push_back(FxChainFixture::WorldPass());
        bp.passes.push_back(FxChainFixture::FxPass(
            "Fx",
            R"({"id":"Fx","type":"FullscreenShader","inputs":{"uTex":"SceneColor"},"output":"LDROut","shaderPkg":"Shaders/copy.shaderpkg"})"));
        bp.passes.push_back(FxChainFixture::FxPass(
            "FinalCopy",
            R"({"id":"FinalCopy","type":"FullscreenShader","elideWhenIdentity":true,"inputs":{"uTex":"LDROut"},"output":"View.Resolve","shaderPkg":"Shaders/copy.shaderpkg"})"));
        bp.worldColorResolveTargetRef = "SceneColor";
        FxChainFixture::AddTexResource(bp, "SceneColor");          // render basis
        FxChainFixture::AddOutputBasisTexResource(bp, "LDROut");   // display basis
        bp.outputs.push_back({"FinalColor", "View.Resolve"});

        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);

        // The blueprint declares no crossing node, so the view needs the other
        // kind of crossing to be split-eligible at all. TAA supplies it — and
        // because the TAA node is likewise absent from this blueprint, nothing
        // actually crosses, which is exactly the straddle this test is about:
        // the terminal stage's input sits at the display extent while
        // View.Resolve is still the internal SceneColor.
        f.Rs->Views().SetViewAntiAliasing(f.View, true, AntiAliasingMode::TAA, 8u);

        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient,
                                   &pools.Ring);
        const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                     f.Rs->Views().GetViews().end());

        auto declareOnce = [&](uint64_t frameIndex, float scale)
        {
            f.Rs->Views().SetViewRenderScale(f.View, scale);
            frame.BeginFrame(frameIndex);
            RenderGraph::RGTexture color =
                frame.ImportPersistentTexture("ER.Color", ColorTargetDesc());
            RenderGraph::RGTexture depth =
                frame.ImportPersistentTexture("ER.Depth", DepthTargetDesc());
            f.Rs->BeginWorldDrawFrame();
            f.Rs->BuildWorldBatchKeys();
            const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{f.View, color, depth, {}}};
            instance.Declare(frame, targets, views);
        };

        // Control first: native scale, extents agree, the copy elides.
        declareOnce(0, 1.0f);
        if (frame.Graph().PassCount() < 2u)
            GTEST_SKIP() << "copy.shaderpkg unavailable in this environment";
        EXPECT_EQ(frame.Graph().PassCount(), 2u)
            << "at scale 1.0 the terminal copy is a true identity and still elides";
        {
            const auto* fr = instance.FrameResourcesFor(&frame);
            ASSERT_NE(fr, nullptr);
            const auto it = fr->Textures.find({f.View, "LDROut"});
            ASSERT_NE(it, fr->Textures.end());
            EXPECT_EQ(instance.GetOutputRG(f.View, "FinalColor").Id, it->second.Id)
                << "elided: View.Resolve threads to the stage's own input";
        }

        // Split active: the extents straddle the crossing, so the copy must run.
        declareOnce(1, 0.5f);
        EXPECT_EQ(frame.Graph().PassCount(), 3u)
            << "an extent mismatch is not an identity — the copy declares the resample";
        {
            const auto* fr = instance.FrameResourcesFor(&frame);
            ASSERT_NE(fr, nullptr);
            const auto ldr = fr->Textures.find({f.View, "LDROut"});
            const auto scene = fr->Textures.find({f.View, "SceneColor"});
            ASSERT_NE(ldr, fr->Textures.end());
            ASSERT_NE(scene, fr->Textures.end());
            EXPECT_EQ(frame.Graph().ResourceDesc(ldr->second.Id).Width, 64u)
                << "output-basis input sits at the display extent";
            EXPECT_EQ(frame.Graph().ResourceDesc(scene->second.Id).Width, 32u)
                << "render-basis View.Resolve sits at the internal extent";
            EXPECT_NE(instance.GetOutputRG(f.View, "FinalColor").Id, ldr->second.Id)
                << "not elided: FinalColor is the stage's own output, not its input";
        }

        f.Device->WaitForIdle();
    }
    f.Down();
}

// The crossing tests skip only when the upscale package is genuinely absent.
// A staged package plus a declined crossing is a real defect (e.g. the output
// publish shadowing its own resolve) and must FAIL — probing availability
// through the same loader the node uses keeps the two cases distinguishable.
static bool SpatialUpscalePackageStaged()
{
    ShaderPackage pkg{};
    std::string err;
    return LoadShaderPkg("Shaders/spatial_upscale.shaderpkg", ShaderSourceKind::SpirV, pkg, &err);
}

// Odd display extents. The Player sizes its targets from the swapchain, which
// is whatever the window is — 1281x721 is as legitimate as 1280x720. The
// internal extent is even-snapped (the half/quarter post chains must divide
// cleanly), so an odd output extent is exactly the case where internal and
// output cannot be a clean ratio: the crossing has to resample 960x540 into
// 1281x721, and every output-basis resource has to land on the odd extent
// rather than an even rounding of it.
TEST(RenderPipelineDeclareTests, CrossingHandlesOddDisplayExtents)
{
    FxChainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        constexpr uint32_t kOddW = 1281;
        constexpr uint32_t kOddH = 721;

        RenderPipelineBlueprint bp;
        bp.pipelineName = "OddExtentTest";
        bp.passes.push_back(FxChainFixture::WorldPass());
        RenderPipelineBlueprint::Pass cross;
        cross.id = "RenderScaleUpscale";
        cross.type = "RenderScaleUpscale";
        cross.enabled = true;
        cross.perView = true;
        cross.passJson =
            R"({"id":"RenderScaleUpscale","type":"RenderScaleUpscale","input":"SceneColor","output":"HDRUpscaled"})";
        bp.passes.push_back(cross);
        bp.worldColorResolveTargetRef = "SceneColor";
        FxChainFixture::AddTexResource(bp, "SceneColor");
        FxChainFixture::AddOutputBasisTexResource(bp, "HDRUpscaled");
        bp.outputs.push_back({"FinalColor", "View.Resolve"});

        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);
        f.Rs->Views().SetViewRenderScale(f.View, 0.75f);

        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient,
                                   &pools.Ring);
        frame.BeginFrame(0);

        TextureDesc cd = ColorTargetDesc();
        cd.width = kOddW;
        cd.height = kOddH;
        TextureDesc dd = DepthTargetDesc();
        dd.width = kOddW;
        dd.height = kOddH;
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("OD.Color", cd);
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("OD.Depth", dd);
        ASSERT_TRUE(color.IsValid());
        ASSERT_TRUE(depth.IsValid());

        f.Rs->BeginWorldDrawFrame();
        f.Rs->BuildWorldBatchKeys();
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{f.View, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                     f.Rs->Views().GetViews().end());
        instance.Declare(frame, targets, views);

        const auto* fr = instance.FrameResourcesFor(&frame);
        ASSERT_NE(fr, nullptr);
        const auto vi = fr->Views.find(f.View);
        ASSERT_NE(vi, fr->Views.end());
        // 1281 * 0.75 = 960.75 -> floor 960 (already even); 721 * 0.75 = 540.75 -> 540.
        EXPECT_EQ(vi->second.RenderWidth, 960u) << "internal extent is even-snapped";
        EXPECT_EQ(vi->second.RenderHeight, 540u);
        EXPECT_EQ(vi->second.RenderWidth % 2u, 0u);
        EXPECT_EQ(vi->second.RenderHeight % 2u, 0u);
        EXPECT_EQ(vi->second.OutputWidth, kOddW) << "the odd display extent is preserved exactly";
        EXPECT_EQ(vi->second.OutputHeight, kOddH);

        const auto scene = fr->Textures.find({f.View, "SceneColor"});
        ASSERT_NE(scene, fr->Textures.end());
        EXPECT_EQ(frame.Graph().ResourceDesc(scene->second.Id).Width, 960u);
        EXPECT_EQ(frame.Graph().ResourceDesc(scene->second.Id).Height, 540u);

        const auto up = fr->Textures.find({f.View, "HDRUpscaled"});
        ASSERT_NE(up, fr->Textures.end());
        if (up->second.Id != scene->second.Id)
        {
            // Crossing declared: its target must be the ODD extent, not a rounding.
            EXPECT_EQ(frame.Graph().ResourceDesc(up->second.Id).Width, kOddW);
            EXPECT_EQ(frame.Graph().ResourceDesc(up->second.Id).Height, kOddH);
            const auto resolve = fr->Textures.find({f.View, "View.Resolve"});
            ASSERT_NE(resolve, fr->Textures.end());
            EXPECT_EQ(resolve->second.Id, color.Id)
                << "View.Resolve republished to the caller's odd-extent colour";
        }
        else
        {
            ASSERT_FALSE(SpatialUpscalePackageStaged())
                << "spatial_upscale.shaderpkg is staged yet the crossing declined";
            GTEST_SKIP() << "spatial_upscale.shaderpkg unavailable in this environment";
        }

        f.Device->WaitForIdle();
    }
    f.Down();
}

// The crossing node: with the split active and no TAA it declares exactly two
// passes (the colour resample and the display-depth reconstitution), publishes
// its output at the display extent, and republishes View.Resolve to the caller's
// colour so the terminal stage writes the display target. With the split
// inactive it declares nothing and stitches its input through — which is what
// makes render scale 1.0 free.
TEST(RenderPipelineDeclareTests, RenderScaleUpscaleCrossesOnceAndStitchesWhenNative)
{
    FxChainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderPipelineBlueprint bp;
        bp.pipelineName = "CrossingTest";
        bp.passes.push_back(FxChainFixture::WorldPass());
        RenderPipelineBlueprint::Pass cross;
        cross.id = "RenderScaleUpscale";
        cross.type = "RenderScaleUpscale";
        cross.enabled = true;
        cross.perView = true;
        cross.passJson =
            R"({"id":"RenderScaleUpscale","type":"RenderScaleUpscale","input":"SceneColor","output":"HDRUpscaled"})";
        bp.passes.push_back(cross);
        bp.worldColorResolveTargetRef = "SceneColor";
        FxChainFixture::AddTexResource(bp, "SceneColor");             // render basis
        FxChainFixture::AddOutputBasisTexResource(bp, "HDRUpscaled"); // display basis
        bp.outputs.push_back({"FinalColor", "View.Resolve"});

        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);

        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient,
                                   &pools.Ring);
        const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                     f.Rs->Views().GetViews().end());
        RenderGraph::RGTexture color{};
        RenderGraph::RGTexture depth{};

        auto declareOnce = [&](uint64_t frameIndex, float scale)
        {
            f.Rs->Views().SetViewRenderScale(f.View, scale);
            frame.BeginFrame(frameIndex);
            color = frame.ImportPersistentTexture("CR.Color", ColorTargetDesc());
            depth = frame.ImportPersistentTexture("CR.Depth", DepthTargetDesc());
            f.Rs->BeginWorldDrawFrame();
            f.Rs->BuildWorldBatchKeys();
            const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{f.View, color, depth, {}}};
            instance.Declare(frame, targets, views);
        };

        // Native: the node is inert and stitches through.
        declareOnce(0, 1.0f);
        const size_t worldOnlyPasses = frame.Graph().PassCount();
        {
            const auto* fr = instance.FrameResourcesFor(&frame);
            ASSERT_NE(fr, nullptr);
            const auto up = fr->Textures.find({f.View, "HDRUpscaled"});
            const auto scene = fr->Textures.find({f.View, "SceneColor"});
            ASSERT_NE(up, fr->Textures.end());
            ASSERT_NE(scene, fr->Textures.end());
            EXPECT_EQ(up->second.Id, scene->second.Id)
                << "at scale 1.0 the crossing stitches its input through — no pass, no pool texture";
        }

        // Split active: exactly one colour resample plus the depth upsample.
        declareOnce(1, 0.5f);
        {
            // Pin the split state independently of shader availability, so a
            // missing package cannot be confused with a pre-pass that declined.
            const auto* fr = instance.FrameResourcesFor(&frame);
            ASSERT_NE(fr, nullptr);
            const auto vi = fr->Views.find(f.View);
            ASSERT_NE(vi, fr->Views.end());
            EXPECT_EQ(vi->second.RenderWidth, 32u) << "the pre-pass split the view";
            EXPECT_EQ(vi->second.OutputWidth, 64u);
            const auto outColor = fr->Textures.find({f.View, "View.OutputColor"});
            ASSERT_NE(outColor, fr->Textures.end())
                << "the split must preserve the caller's colour for the crossing";
        }
        if (frame.Graph().PassCount() == worldOnlyPasses)
        {
            ASSERT_FALSE(SpatialUpscalePackageStaged())
                << "spatial_upscale.shaderpkg is staged yet the crossing declined";
            GTEST_SKIP() << "spatial_upscale.shaderpkg unavailable in this environment";
        }
        EXPECT_EQ(frame.Graph().PassCount(), worldOnlyPasses + 2u)
            << "one colour resample + one depth upsample, and nothing else";
        {
            const auto* fr = instance.FrameResourcesFor(&frame);
            ASSERT_NE(fr, nullptr);
            const auto up = fr->Textures.find({f.View, "HDRUpscaled"});
            const auto scene = fr->Textures.find({f.View, "SceneColor"});
            ASSERT_NE(up, fr->Textures.end());
            ASSERT_NE(scene, fr->Textures.end());
            EXPECT_NE(up->second.Id, scene->second.Id) << "the crossing declared a real output";
            EXPECT_EQ(frame.Graph().ResourceDesc(up->second.Id).Width, 64u)
                << "the crossing output sits at the display extent";
            EXPECT_EQ(frame.Graph().ResourceDesc(scene->second.Id).Width, 32u)
                << "its input stayed at the internal extent";
            const auto resolve = fr->Textures.find({f.View, "View.Resolve"});
            ASSERT_NE(resolve, fr->Textures.end());
            EXPECT_EQ(resolve->second.Id, color.Id)
                << "View.Resolve republished to the caller's display-res colour";
        }

        f.Device->WaitForIdle();
    }
    f.Down();
}

// Slice-6 hardening — a stage whose resolved input EQUALS its resolved output
// (reachable when chain threading desynchronizes a ping-pong pair: Fx1's
// stitch makes Fx2's input thread back to Fx2's own output) must DECLINE and
// stitch, never declare a sample-while-attached feedback loop. The ungated
// Control stage is the positive control: it proves copy.shaderpkg loads, so
// Fx2's absence is the guard, not a missing shader.
TEST(RenderPipelineDeclareTests, SelfReferentialOutputDeclinesAndStitches)
{
    FxChainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderPipelineBlueprint bp;
        bp.pipelineName = "SelfRefTest";
        bp.passes.push_back(FxChainFixture::WorldPass());
        bp.passes.push_back(FxChainFixture::FxPass(
            "Fx1",
            R"({"id":"Fx1","type":"FullscreenShader","inputs":{"uTex":"SceneColor"},"output":"PostA","shaderPkg":"Shaders/copy.shaderpkg","skipWhen":{"crtIntensity":0.0}})"));
        bp.passes.push_back(FxChainFixture::FxPass(
            "Fx2",
            R"({"id":"Fx2","type":"FullscreenShader","inputs":{"uTex":"PostA"},"output":"SceneColor","shaderPkg":"Shaders/copy.shaderpkg"})"));
        bp.passes.push_back(FxChainFixture::FxPass(
            "Control",
            R"({"id":"Control","type":"FullscreenShader","inputs":{"uTex":"SceneColor"},"output":"PostB","shaderPkg":"Shaders/copy.shaderpkg"})"));
        bp.worldColorResolveTargetRef = "SceneColor";
        FxChainFixture::AddTexResource(bp, "SceneColor");
        FxChainFixture::AddTexResource(bp, "PostA");
        FxChainFixture::AddTexResource(bp, "PostB");
        bp.outputs.push_back({"FinalColor", "PostB"});

        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);

        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("SRF.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("SRF.Depth", DepthTargetDesc());

        f.Rs->BeginWorldDrawFrame();
        f.Rs->BuildWorldBatchKeys();
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{f.View, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                     f.Rs->Views().GetViews().end());
        instance.Declare(frame, targets, views);

        if (frame.Graph().PassCount() < 2u)
            GTEST_SKIP() << "copy.shaderpkg unavailable in this environment";

        // World + Control only: Fx1 stitched by its gate, Fx2 declined by the
        // in==out guard (its input PostA threads back to SceneColor).
        EXPECT_EQ(frame.Graph().PassCount(), 2u)
            << "the self-referential stage must decline, not declare";

        const auto* fr = instance.FrameResourcesFor(&frame);
        ASSERT_NE(fr, nullptr);
        const RenderGraph::RGTexture sceneColor = fr->Textures.at({f.View, "View.EffectiveColor"});

        // The feedback-loop shape is absent: no pass both samples and
        // attaches the same texture.
        for (size_t p = 0; p < frame.Graph().PassCount(); ++p)
        {
            const auto pass = static_cast<RenderGraph::RGPassId>(p);
            if (!frame.Graph().HasReadAccess(pass, sceneColor.Id))
                continue;
            for (const auto& rec : frame.Attachments())
                EXPECT_FALSE(rec.Pass == pass && rec.Tex == sceneColor.Id)
                    << "pass '" << frame.Graph().PassName(pass)
                    << "' samples the texture it attaches";
        }

        frame.MarkOutput(instance.GetOutputRG(f.View, "FinalColor"));
        frame.Execute();
        f.Device->WaitForIdle();
    }
    f.Down();
}

// Slice-6 hardening — a multisampled Sampled input cannot bind to the FX
// sampler2D: the stage must DECLINE and stitch (diagnosable), not declare
// undefined MSAA sampling. Same blueprint declared twice: single-sample
// targets prove the copy declares (positive control + the missing-shaderpkg
// skip gate), MSAA targets prove the decline.
TEST(RenderPipelineDeclareTests, MsaaInputDeclinesCopyAndStitches)
{
    FxChainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderPipelineBlueprint bp;
        bp.pipelineName = "MsaaDeclineTest";
        bp.passes.push_back(FxChainFixture::WorldPass());
        // Output is a chain texture (the caller targets carry no View.Resolve
        // in this fixture); the input is the caller's View.Color whose sample
        // count the two frames vary.
        bp.passes.push_back(FxChainFixture::FxPass(
            "Copy",
            R"({"id":"Copy","type":"FullscreenShader","inputs":{"uTex":"View.Color"},"output":"PostOut","shaderPkg":"Shaders/copy.shaderpkg"})"));
        FxChainFixture::AddTexResource(bp, "PostOut");
        bp.outputs.push_back({"FinalColor", "PostOut"});

        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);

        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                     f.Rs->Views().GetViews().end());

        // Frame 0 — single-sample targets: the copy DECLARES (control).
        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("MSD.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("MSD.Depth", DepthTargetDesc());
        f.Rs->BeginWorldDrawFrame();
        f.Rs->BuildWorldBatchKeys();
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{f.View, color, depth, {}}};
        instance.Declare(frame, targets, views);
        if (frame.Graph().PassCount() < 2u)
            GTEST_SKIP() << "copy.shaderpkg unavailable in this environment";
        EXPECT_EQ(frame.Graph().PassCount(), 2u) << "single-sample input: the copy declares";
        frame.MarkOutput(instance.GetOutputRG(f.View, "FinalColor"));
        frame.Execute();
        f.Device->WaitForIdle();

        // Frame 1 — MSAA targets, no resolve-capable chain: the copy DECLINES
        // and the chain threads the (multisampled) input through.
        frame.BeginFrame(1);
        color = frame.ImportPersistentTexture("MSD.ColorMS", ColorTargetDesc(4));
        depth = frame.ImportPersistentTexture("MSD.DepthMS", DepthTargetDesc(4));
        f.Rs->BeginWorldDrawFrame();
        f.Rs->BuildWorldBatchKeys();
        const std::vector<ViewTargetsRG> targets1 = {ViewTargetsRG{f.View, color, depth, {}}};
        instance.Declare(frame, targets1, views);
        EXPECT_EQ(frame.Graph().PassCount(), 1u)
            << "multisampled input: the copy declines (no undefined MSAA sampling)";
        const RenderGraph::RGTexture finalColor = instance.GetOutputRG(f.View, "FinalColor");
        ASSERT_TRUE(finalColor.IsValid());
        EXPECT_EQ(finalColor.Id, color.Id) << "the chain threads through to the MSAA input";
        frame.MarkOutput(color);
        frame.Execute();
        f.Device->WaitForIdle();
    }
    f.Down();
}

// 5b — the chain's first declared FX reads the SAME resource id the world
// arm wrote (the SceneColorReady tag's replacement is a plain RAW edge).
TEST(RenderPipelineDeclareTests, FirstFxReadsTheWorldArmsSceneColorWrite)
{
    FxChainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderPipelineBlueprint bp;
        bp.pipelineName = "RawTest";
        bp.passes.push_back(FxChainFixture::WorldPass());
        bp.passes.push_back(FxChainFixture::FxPass(
            "Fx",
            R"({"id":"Fx","type":"FullscreenShader","inputs":{"uTex":"SceneColor"},"output":"PostOut","shaderPkg":"Shaders/copy.shaderpkg"})"));
        bp.worldColorResolveTargetRef = "SceneColor";
        FxChainFixture::AddTexResource(bp, "SceneColor");
        FxChainFixture::AddTexResource(bp, "PostOut");
        bp.outputs.push_back({"FinalColor", "PostOut"});

        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);

        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("RAW.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("RAW.Depth", DepthTargetDesc());

        f.Rs->BeginWorldDrawFrame();
        f.Rs->BuildWorldBatchKeys();
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{f.View, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                     f.Rs->Views().GetViews().end());
        instance.Declare(frame, targets, views);

        if (frame.Graph().PassCount() < 2u)
            GTEST_SKIP() << "copy.shaderpkg unavailable in this environment";

        const auto* fr = instance.FrameResourcesFor(&frame);
        ASSERT_NE(fr, nullptr);
        const RenderGraph::RGTexture sceneColor = fr->Textures.at({f.View, "View.EffectiveColor"});
        const RenderGraph::RGTexture postOut = instance.GetOutputRG(f.View, "FinalColor");
        ASSERT_TRUE(postOut.IsValid());
        EXPECT_NE(postOut.Id, sceneColor.Id);

        RenderGraph::RGPassId fxPass = 0;
        RenderGraph::RGPassId worldPass = 0;
        bool foundFx = false, foundWorld = false;
        for (const auto& rec : frame.Attachments())
        {
            if (rec.Tex == postOut.Id && !rec.IsDepth)
            {
                fxPass = rec.Pass;
                foundFx = true;
            }
            if (rec.Tex == sceneColor.Id && !rec.IsDepth)
            {
                worldPass = rec.Pass;
                foundWorld = true;
            }
        }
        ASSERT_TRUE(foundFx);
        ASSERT_TRUE(foundWorld);
        EXPECT_TRUE(frame.Graph().HasReadAccess(fxPass, sceneColor.Id))
            << "the FX stage reads the world's write — the RAW edge that replaced the tag";

        frame.MarkOutput(postOut);
        frame.Execute();
        f.Device->WaitForIdle();
        EXPECT_LT(ScheduledIndexOf(frame.Graph(), worldPass),
                  ScheduledIndexOf(frame.Graph(), fxPass));
    }
    f.Down();
}

// 5b — the skipWhen gate both ways at declaration: matched fields stitch
// (and never materialize the output), a mismatch declares the pass.
TEST(RenderPipelineDeclareTests, SkipWhenDeclaresOnMismatchAndStitchesOnMatch)
{
    FxChainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderPipelineBlueprint bp;
        bp.pipelineName = "GateTest";
        bp.passes.push_back(FxChainFixture::WorldPass());
        bp.passes.push_back(FxChainFixture::FxPass(
            "Fx",
            R"({"id":"Fx","type":"FullscreenShader","inputs":{"uTex":"SceneColor"},"output":"PostOut","shaderPkg":"Shaders/copy.shaderpkg","skipWhen":{"crtIntensity":0.0}})"));
        bp.worldColorResolveTargetRef = "SceneColor";
        FxChainFixture::AddTexResource(bp, "SceneColor");
        FxChainFixture::AddTexResource(bp, "PostOut");
        bp.outputs.push_back({"FinalColor", "PostOut"});

        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);

        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

        const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                     f.Rs->Views().GetViews().end());

        // Frame 0: default settings (CrtIntensity == 0) => skip + stitch.
        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("GT.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("GT.Depth", DepthTargetDesc());
        f.Rs->BeginWorldDrawFrame();
        f.Rs->BuildWorldBatchKeys();
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{f.View, color, depth, {}}};
        instance.Declare(frame, targets, views);
        EXPECT_EQ(frame.Graph().PassCount(), 1u) << "matched skipWhen: only the world declares";
        const auto* fr = instance.FrameResourcesFor(&frame);
        ASSERT_NE(fr, nullptr);
        const RenderGraph::RGTexture sceneColor = fr->Textures.at({f.View, "View.EffectiveColor"});
        EXPECT_EQ(instance.GetOutputRG(f.View, "FinalColor").Id, sceneColor.Id) << "stitched";
        EXPECT_EQ(pools.Persistent.Size(), 3u) << "skipped PostOut never materializes";
        frame.MarkOutput(sceneColor);
        frame.Execute();
        f.Device->WaitForIdle();

        // Frame 1: live effect => the pass declares and owns the chain.
        PostProcessSettings s{};
        s.CrtIntensity = 1.0f;
        f.Rs->Views().SetViewPostProcessOverride(f.View, s);
        frame.BeginFrame(1);
        color = frame.ImportPersistentTexture("GT.Color", ColorTargetDesc());
        depth = frame.ImportPersistentTexture("GT.Depth", DepthTargetDesc());
        f.Rs->BeginWorldDrawFrame();
        f.Rs->BuildWorldBatchKeys();
        const std::vector<ViewTargetsRG> targets1 = {ViewTargetsRG{f.View, color, depth, {}}};
        instance.Declare(frame, targets1, views);
        if (frame.Graph().PassCount() < 2u)
            GTEST_SKIP() << "copy.shaderpkg unavailable in this environment";
        const RenderGraph::RGTexture postOut = instance.GetOutputRG(f.View, "FinalColor");
        ASSERT_TRUE(postOut.IsValid());
        const auto* fr1 = instance.FrameResourcesFor(&frame);
        ASSERT_NE(fr1, nullptr);
        EXPECT_NE(postOut.Id, fr1->Textures.at({f.View, "View.EffectiveColor"}).Id)
            << "mismatch: the pass declared, FinalColor = its own output";
        EXPECT_EQ(pools.Persistent.Size(), 4u) << "PostOut materialized this frame";
        frame.MarkOutput(postOut);
        frame.Execute();
        f.Device->WaitForIdle();
    }
    f.Down();
}

namespace
{
// One declared frame of the shipped bloom and scattering passes, copied verbatim
// from Assets/RenderPipelines/ForwardPlus.rendergraph and fed by a world pass that
// stands in for HDRUpscaled.
struct ShippedBloomFrame
{
    bool ShadersStaged = true;
    std::vector<std::string> DeclaredNodes;       // bloom-family node ids that declared a pass
    std::vector<std::string> CombineSampledNames; // last segment of each texture BloomCombine samples
};

bool IsBloomFamilyNode(const std::string& id)
{
    return id.starts_with("Bloom") || id.starts_with("Scattering");
}

std::string LastNameSegment(std::string_view name)
{
    const size_t dot = name.rfind('.');
    return std::string(dot == std::string_view::npos ? name : name.substr(dot + 1));
}

ShippedBloomFrame DeclareShippedBloomFrame(FxChainFixture& f, const PostProcessSettings& settings)
{
    std::ifstream stream(std::filesystem::path(GE_RENDERER_REPO_ROOT) /
                         "Assets/RenderPipelines/ForwardPlus.rendergraph");
    const nlohmann::json graph = nlohmann::json::parse(stream);

    ShippedBloomFrame result;
    RenderPipelineBlueprint bp;
    bp.pipelineName = "ShippedBloom";
    std::vector<std::string> bloomNodes;
    for (const auto& pass : graph.at("passes"))
    {
        const std::string id = pass.at("id").get<std::string>();
        if (id == "ViewParamsUpload")
        {
            RenderPipelineBlueprint::Pass viewParams;
            viewParams.id = id;
            viewParams.type = "ViewParamsUpload";
            viewParams.enabled = true;
            viewParams.perView = true;
            viewParams.passJson = pass.dump();
            bp.passes.push_back(viewParams);
            bp.passes.push_back(FxChainFixture::WorldPass());
        }
        if (!IsBloomFamilyNode(id))
            continue;
        if (Rendering::Utils::ResolveShaderPath(pass.at("shaderPkg").get<std::string>().c_str()).empty())
            result.ShadersStaged = false;
        bp.passes.push_back(FxChainFixture::FxPass(id.c_str(), pass.dump()));
        bloomNodes.push_back(id);
    }
    for (const auto& [name, resource] : graph.at("resources").items())
        bp.resources.push_back({name, resource.dump()});
    bp.worldColorResolveTargetRef = "HDRUpscaled";
    bp.outputs.push_back({"FinalColor", "HDRBloomWithDirt"});
    if (!result.ShadersStaged)
        return result;

    f.Rs->Views().SetViewPostProcessOverride(f.View, settings);
    RenderPipelineInstance instance(*f.Rs, f.Registry);
    instance.SetBlueprint(bp);
    FramePools pools(f.Device.get());
    RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);
    RenderGraph::RGTexture color = frame.ImportPersistentTexture("SB.Color", ColorTargetDesc());
    RenderGraph::RGTexture depth = frame.ImportPersistentTexture("SB.Depth", DepthTargetDesc());
    f.Rs->BeginWorldDrawFrame();
    f.Rs->BuildWorldBatchKeys();
    const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{f.View, color, depth, {}}};
    const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                 f.Rs->Views().GetViews().end());
    instance.Declare(frame, targets, views);

    const RenderGraph::RGGraph& g = frame.Graph();
    for (size_t p = 0; p < g.PassCount(); ++p)
    {
        const std::string_view passName = g.PassName(static_cast<RenderGraph::RGPassId>(p));
        for (const std::string& node : bloomNodes)
        {
            if (!RGQuery::Matches(passName, RGQuery::Family{node}))
                continue;
            result.DeclaredNodes.push_back(node);
            if (node != "BloomCombine")
                continue;
            for (const RenderGraph::RGAccessRecord& access : g.Accesses())
            {
                if (access.Pass == p && access.Access == RenderGraph::RGAccess::Sampled)
                    result.CombineSampledNames.push_back(LastNameSegment(g.ResourceName(access.Resource)));
            }
        }
    }
    frame.MarkOutput(instance.GetOutputRG(f.View, "FinalColor"));
    frame.Execute();
    f.Device->WaitForIdle();
    return result;
}

bool Contains(const std::vector<std::string>& names, std::string_view name)
{
    return std::find(names.begin(), names.end(), name) != names.end();
}

bool AnyStartsWith(const std::vector<std::string>& names, std::string_view prefix)
{
    return std::any_of(names.begin(), names.end(),
                       [prefix](const std::string& name) { return name.starts_with(prefix); });
}

std::string Joined(const std::vector<std::string>& names)
{
    std::string out;
    for (const std::string& name : names)
        out += (out.empty() ? "" : ", ") + name;
    return out;
}
} // namespace

// The Scene View post-processing and Bloom switches apply DisableBloom: the frame
// declares no bloom or scattering pass, the depth veil and lens dirt included.
TEST(RenderPipelineDeclareTests, DisabledBloomDeclaresNoBloomPassWithDepthVeilOn)
{
    FxChainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(f.Registry.Register(
        "ViewParamsUpload", [] { return std::make_unique<Nodes::ViewParamsUploadNode>(); }, true));
    {
        PostProcessSettings settings{};
        settings.BloomIntensity = 0.45f;
        settings.BloomScatteringAmount = 0.3f;
        settings.BloomDepthVeilEnabled = 1;
        settings.BloomDepthVeilIntensity = 1.0f;
        settings.BloomDepthVeilStart = 0.0f;
        settings.BloomDepthVeilEnd = 15.0f;
        settings.BloomLensDirtEnabled = 1;
        settings.BloomLensDirtIntensity = 1.0f;

        const ShippedBloomFrame on = DeclareShippedBloomFrame(f, settings);
        if (!on.ShadersStaged)
            GTEST_SKIP() << "bloom shader packages are not staged (build CompileShaderPkgs)";
        ASSERT_TRUE(Contains(on.DeclaredNodes, "BloomThreshold")) << Joined(on.DeclaredNodes);
        ASSERT_TRUE(Contains(on.DeclaredNodes, "ScatteringThreshold")) << Joined(on.DeclaredNodes);
        ASSERT_TRUE(Contains(on.DeclaredNodes, "BloomLensDirtComposite")) << Joined(on.DeclaredNodes);

        settings.DisableBloom();
        const ShippedBloomFrame off = DeclareShippedBloomFrame(f, settings);
        EXPECT_TRUE(off.DeclaredNodes.empty()) << "declared with bloom off: " << Joined(off.DeclaredNodes);
    }
    f.Down();
}

// One threshold decides scattering: just below it no scattering pass declares and
// BloomCombine samples no scattering texture; just above it the chain feeds the
// combine; scattering alone feeds the combine without the highlight pyramid; and
// scattering alone below it declares no bloom pass at all.
TEST(RenderPipelineDeclareTests, ScatteringDeclaresAndFeedsCombineOnlyAboveOneThreshold)
{
    // PostProcessSettings' default gate epsilon, shared by every bloom gate.
    constexpr float kGateEpsilon = 1.0f / 1024.0f;
    FxChainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(f.Registry.Register(
        "ViewParamsUpload", [] { return std::make_unique<Nodes::ViewParamsUploadNode>(); }, true));
    {
        PostProcessSettings settings{};
        settings.BloomIntensity = 0.45f;
        for (const float amount : {0.0f, 0.9f * kGateEpsilon})
        {
            settings.BloomScatteringAmount = amount;
            const ShippedBloomFrame frame = DeclareShippedBloomFrame(f, settings);
            if (!frame.ShadersStaged)
                GTEST_SKIP() << "bloom shader packages are not staged (build CompileShaderPkgs)";
            ASSERT_TRUE(Contains(frame.DeclaredNodes, "BloomCombine")) << amount;
            EXPECT_FALSE(AnyStartsWith(frame.DeclaredNodes, "Scattering"))
                << amount << ": " << Joined(frame.DeclaredNodes);
            EXPECT_FALSE(AnyStartsWith(frame.CombineSampledNames, "Scattering"))
                << amount << ": " << Joined(frame.CombineSampledNames);
        }

        settings.BloomScatteringAmount = 1.1f * kGateEpsilon;
        const ShippedBloomFrame above = DeclareShippedBloomFrame(f, settings);
        EXPECT_TRUE(Contains(above.DeclaredNodes, "ScatteringThreshold")) << Joined(above.DeclaredNodes);
        EXPECT_TRUE(Contains(above.DeclaredNodes, "ScatteringOctaveGather")) << Joined(above.DeclaredNodes);
        EXPECT_TRUE(Contains(above.CombineSampledNames, "ScatteringNormalized"))
            << Joined(above.CombineSampledNames);

        settings.BloomIntensity = 0.0f;
        settings.BloomScatteringAmount = 0.3f;
        const ShippedBloomFrame scatteringOnly = DeclareShippedBloomFrame(f, settings);
        EXPECT_TRUE(Contains(scatteringOnly.CombineSampledNames, "ScatteringNormalized"))
            << Joined(scatteringOnly.CombineSampledNames);
        EXPECT_FALSE(AnyStartsWith(scatteringOnly.CombineSampledNames, "Bloom"))
            << Joined(scatteringOnly.CombineSampledNames);

        settings.BloomIntensity = 0.0f;
        settings.BloomScatteringAmount = 0.9f * kGateEpsilon;
        const ShippedBloomFrame unconsumed = DeclareShippedBloomFrame(f, settings);
        EXPECT_TRUE(unconsumed.DeclaredNodes.empty()) << Joined(unconsumed.DeclaredNodes);
    }
    f.Down();
}

TEST(RenderPipelineDeclareTests, RequiredPackageUnavailableStitchesFullscreenStage)
{
    FxChainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        f.Rs->SetPackageAvailabilityQuery(
            [](std::string_view name) { return name != "optional-fx"; });

        RenderPipelineBlueprint bp;
        bp.pipelineName = "PackageGateTest";
        bp.passes.push_back(FxChainFixture::WorldPass());
        bp.passes.push_back(FxChainFixture::FxPass(
            "OptionalFx",
            R"({"id":"OptionalFx","type":"FullscreenShader","inputs":{"uTex":"SceneColor"},"output":"PostOut","shaderPkg":"Shaders/copy.shaderpkg","requiresPackage":"optional-fx"})"));
        bp.worldColorResolveTargetRef = "SceneColor";
        FxChainFixture::AddTexResource(bp, "SceneColor");
        FxChainFixture::AddTexResource(bp, "PostOut");
        bp.outputs.push_back({"FinalColor", "PostOut"});

        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);
        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient,
                                   &pools.Ring);
        frame.BeginFrame(0);
        const RenderGraph::RGTexture color =
            frame.ImportPersistentTexture("PG.Color", ColorTargetDesc());
        const RenderGraph::RGTexture depth =
            frame.ImportPersistentTexture("PG.Depth", DepthTargetDesc());
        f.Rs->BeginWorldDrawFrame();
        f.Rs->BuildWorldBatchKeys();
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{f.View, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                     f.Rs->Views().GetViews().end());
        instance.Declare(frame, targets, views);

        EXPECT_EQ(frame.Graph().PassCount(), 1u)
            << "an unavailable package must not declare its fullscreen pass";
        const auto* fr = instance.FrameResourcesFor(&frame);
        ASSERT_NE(fr, nullptr);
        const RenderGraph::RGTexture sceneColor =
            fr->Textures.at({f.View, "View.EffectiveColor"});
        EXPECT_EQ(instance.GetOutputRG(f.View, "FinalColor").Id, sceneColor.Id);
        EXPECT_EQ(pools.Persistent.Size(), 3u)
            << "the unavailable package output must not materialize";
        frame.MarkOutput(sceneColor);
        frame.Execute();
        f.Device->WaitForIdle();
    }
    f.Down();
}

TEST(RenderPipelineDeclareTests, PhysicalDofFollowsPackageAvailabilityAtZeroIntensity)
{
    FxChainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        bool packageAvailable = true;
        f.Rs->SetPackageAvailabilityQuery(
            [&packageAvailable](std::string_view name)
            { return name != "fidelityfx-dof" || packageAvailable; });
        ASSERT_TRUE(f.Registry.Register(
            "ViewParamsUpload", [] { return std::make_unique<Nodes::ViewParamsUploadNode>(); }, true));
        ASSERT_TRUE(f.Registry.Register(
            "FidelityFXDepthOfField", [] { return std::make_unique<Nodes::FidelityFXDofNode>(); }, true));

        RenderPipelineBlueprint bp;
        bp.pipelineName = "PhysicalDofDebugTest";
        RenderPipelineBlueprint::Pass viewParams;
        viewParams.id = "ViewParams";
        viewParams.type = "ViewParamsUpload";
        viewParams.enabled = true;
        viewParams.perView = true;
        viewParams.passJson = R"({"id":"ViewParams","type":"ViewParamsUpload","buffer":"ViewParams"})";
        bp.passes.push_back(viewParams);

        RenderPipelineBlueprint::Pass dof;
        dof.id = "PhysicalDoF";
        dof.type = "FidelityFXDepthOfField";
        dof.enabled = true;
        dof.perView = true;
        dof.passJson = R"({"id":"PhysicalDoF","type":"FidelityFXDepthOfField","input":"View.Color","depth":"View.Depth","exposureBuffer":"ExposureHistory","output":"HDRPhysicalDoF"})";
        bp.passes.push_back(dof);
        bp.resources.push_back(
            {"ExposureHistory",
             R"({"kind":"buffer","scope":"perView","memoryUsage":"deviceLocal","usage":["storage"],"size":{"bytes":16},"zeroOnCreate":true})"});
        bp.resources.push_back(
            {"HDRPhysicalDoF",
             R"({"kind":"texture","scope":"perView","format":"r16g16b16a16_float","extent":{"scale":[1,1]},"usage":["unorderedAccess","shaderResource"]})"});
        bp.outputs.push_back({"FinalColor", "HDRPhysicalDoF"});

        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);
        FramePools pools(f.Device.get());

        PostProcessSettings settings{};
        settings.DofIntensity = 0.0f;
        settings.DofDebugMode = 1;
        settings.DofDebugAlpha = 1.0f;
        f.Rs->Views().SetViewPostProcessOverride(f.View, settings);

        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        TextureDesc colorDesc = ColorTargetDesc();
        colorDesc.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
        colorDesc.usage |= static_cast<uint32_t>(TextureUsage::ShaderResource);
        const RenderGraph::RGTexture color = frame.ImportPersistentTexture("Dof.Color", colorDesc);
        const RenderGraph::RGTexture depth = frame.ImportPersistentTexture("Dof.Depth", DepthTargetDesc());
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{f.View, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                     f.Rs->Views().GetViews().end());
        instance.Declare(frame, targets, views);

        size_t dofPasses = RGQuery::CountDeclared(frame.Graph(), RGQuery::Family{"PhysicalDoF"});
        EXPECT_EQ(dofPasses, 5u)
            << "focus debug must declare prepare/tile/dilate/blur/composite even with blur intensity zero";
        const RenderGraph::RGTexture result = instance.GetOutputRG(f.View, "FinalColor");
        ASSERT_TRUE(result.IsValid());
        frame.MarkOutput(result);
        frame.Execute();
        f.Device->WaitForIdle();

        packageAvailable = false;
        frame.BeginFrame(1);
        const RenderGraph::RGTexture disabledColor =
            frame.ImportPersistentTexture("Dof.DisabledColor", colorDesc);
        const RenderGraph::RGTexture disabledDepth =
            frame.ImportPersistentTexture("Dof.DisabledDepth", DepthTargetDesc());
        const std::vector<ViewTargetsRG> disabledTargets = {
            ViewTargetsRG{f.View, disabledColor, disabledDepth, {}}};
        instance.Declare(frame, disabledTargets, views);

        dofPasses = RGQuery::CountDeclared(frame.Graph(), RGQuery::Family{"PhysicalDoF"});
        EXPECT_EQ(dofPasses, 0u);
        EXPECT_EQ(instance.GetOutputRG(f.View, "FinalColor").Id, disabledColor.Id)
            << "disabled DoF package must stitch the HDR input through";
        frame.MarkOutput(disabledColor);
        frame.Execute();
        f.Device->WaitForIdle();
    }
    f.Down();
}

// 5b — an FX stage targeting the multisampled View.Color declares the
// resolve PAIR (into the pipeline SceneColor) and, because it blends, Loads
// the attachment (deriving the read that keeps the world alive).
TEST(RenderPipelineDeclareTests, MsaaPairAndBlendLoadForViewColorTargets)
{
    FxChainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderPipelineBlueprint bp;
        bp.pipelineName = "PairTest";
        bp.passes.push_back(FxChainFixture::WorldPass());
        bp.passes.push_back(FxChainFixture::FxPass(
            "Overlay",
            R"({"id":"Overlay","type":"FullscreenShader","alphaBlend":true,"inputs":{"uTex":"DummyIn"},"output":"View.Color","shaderPkg":"Shaders/copy.shaderpkg"})"));
        bp.worldColorResolveTargetRef = "SceneColor";
        FxChainFixture::AddTexResource(bp, "SceneColor");
        FxChainFixture::AddTexResource(bp, "DummyIn");
        bp.outputs.push_back({"FinalColor", "View.Resolve"});

        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);

        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("PT.Color", ColorTargetDesc(4));
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("PT.Depth", DepthTargetDesc(4));

        f.Rs->BeginWorldDrawFrame();
        f.Rs->BuildWorldBatchKeys();
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{f.View, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                     f.Rs->Views().GetViews().end());
        instance.Declare(frame, targets, views);

        if (frame.Graph().PassCount() < 2u)
            GTEST_SKIP() << "copy.shaderpkg unavailable in this environment";

        const auto* fr = instance.FrameResourcesFor(&frame);
        ASSERT_NE(fr, nullptr);
        const RenderGraph::RGTexture sceneColor = fr->Textures.at({f.View, "View.Resolve"});

        // The overlay's attachment: the MSAA caller color with Load (blend)
        // and a resolve into SceneColor. The world's rec on the same texture
        // clears — distinguish by Load op.
        const RenderGraph::RGAttachmentRec* overlayRec = nullptr;
        for (const auto& rec : frame.Attachments())
            if (rec.Tex == color.Id && !rec.IsDepth && rec.Ops.Load == RenderGraph::RGLoadOp::Load)
                overlayRec = &rec;
        ASSERT_NE(overlayRec, nullptr) << "blend => Load on the overlay attachment";
        EXPECT_EQ(overlayRec->Resolve, sceneColor.Id) << "MSAA pair into the pipeline resolve";
        EXPECT_TRUE(frame.Graph().HasReadAccess(overlayRec->Pass, color.Id))
            << "Load derives the read that chains from the world's write";

        frame.MarkOutput(sceneColor);
        frame.Execute();
        f.Device->WaitForIdle();
    }
    f.Down();
}

namespace
{
// A perspective-ish camera + view, plus a fog pass blueprint. The fog node is
// registered locally (the module normally registers it via RenderServices).
struct FogFixture
{
    std::shared_ptr<Rendering::IDevice> Device;
    std::unique_ptr<RenderServices> Rs;
    RenderPipelineNodeRegistry Registry;
    ViewId View = 0;

    bool Up()
    {
        Device = CreateVulkanDeviceFast();
        if (!Device)
            return false;
        Rs = std::make_unique<RenderServices>();
        EXPECT_TRUE(Rs->Initialize(Device.get()));
        const CameraId cam = Rs->Views().AllocateCamera("FogCam");
        CameraData cd{};
        cd.proj[0] = 1.0f;
        cd.proj[5] = 1.0f;
        cd.proj[10] = 0.001f;
        cd.proj[11] = 1.0f;
        cd.proj[14] = 0.1f;
        for (int i = 0; i < 16; i += 5)
        {
            cd.view[i] = 1.0f;
            cd.viewProj[i] = 1.0f;
        }
        Rs->Views().SetCameraData(cam, cd);
        View = Rs->Views().AllocateView("FogView", cam);
        Rs->Views().SetViewRenderLayerMask(View, 1u);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        Rs->Views().SetViewTargets(View, 0, 0, 0, clear);

        EXPECT_TRUE(Registry.Register(
            "VolumetricFog", [] { return std::make_unique<Nodes::VolumetricFogNode>(); }, true));
        return true;
    }

    void SetFog(bool enabled, bool temporal)
    {
        PostProcessSettings pp{};
        pp.VolumetricFogIntensity = enabled ? 1.0f : 0.0f;
        pp.VolumetricFogDensity = 0.02f;
        pp.VolumetricFogTemporalEnabled = temporal ? 1 : 0;
        pp.VolumetricFogTemporalBlend = temporal ? 0.9f : 0.0f;
        Rs->Views().SetViewPostProcessOverride(View, pp);
    }

    RenderPipelineBlueprint MakeBlueprint()
    {
        RenderPipelineBlueprint bp;
        bp.pipelineName = "FogTest";
        bp.worldColorResolveTargetRef = "SceneColor";
        bp.resources.push_back(
            {"SceneColor",
             R"({"kind":"texture","scope":"perView","format":"r16g16b16a16_float","extent":{"scale":[1,1]},"usage":["renderTarget","shaderResource"]})"});
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "Fog";
            p.type = "VolumetricFog";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"Fog","type":"VolumetricFog","output":"SceneColor"})";
            bp.passes.push_back(p);
        }
        bp.outputs.push_back({"FinalColor", "View.Resolve"});
        return bp;
    }

    void Down()
    {
        Rs->Shutdown();
        Device->Shutdown();
    }

    static size_t CountFogPasses(const RenderGraph::RGGraph& g)
    {
        return RGQuery::CountDeclared(g, RGQuery::Subtree{"VolumetricFog"});
    }

    // The id of the History<parity> pool import (or invalid when absent).
    static RenderGraph::RGResourceId HistoryId(const RenderGraph::RGGraph& g, uint32_t parity)
    {
        const std::string want = ".History" + std::to_string(parity);
        for (size_t r = 0; r < g.ResourceCount(); ++r)
        {
            const char* nm = g.ResourceName(static_cast<RenderGraph::RGResourceId>(r));
            if (nm && std::string(nm).find(want) != std::string::npos)
                return static_cast<RenderGraph::RGResourceId>(r);
        }
        return RenderGraph::kInvalidId;
    }
};

struct FogFrameResult
{
    size_t FogPasses = 0;
    RenderGraph::RGResourceId History0 = RenderGraph::kInvalidId;
    RenderGraph::RGResourceId History1 = RenderGraph::kInvalidId;
};
} // namespace

// 5g — a disabled view declares NOTHING and resets the renderer's history (the
// pool ages out; the old AddPassesForView early-out + ResetHistory).
TEST(RenderPipelineDeclareTests, FogDisabledFrameDeclaresNothingAndResetsHistory)
{
    FogFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        // Frame 0: fog ON + temporal so the renderer accrues history state.
        f.SetFog(/*enabled=*/true, /*temporal=*/true);
        RenderPipelineBlueprint bp = f.MakeBlueprint();
        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);

        FramePools pools(f.Device.get());
        {
            RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
            frame.BeginFrame(0);
            RenderGraph::RGTexture color = frame.ImportPersistentTexture("FG.Color", ColorTargetDesc());
            RenderGraph::RGTexture depth = frame.ImportPersistentTexture("FG.Depth", DepthTargetDesc());
            f.Rs->BeginWorldDrawFrame();
            f.Rs->BuildWorldBatchKeys();
            const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{f.View, color, depth, {}}};
            const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                         f.Rs->Views().GetViews().end());
            instance.Declare(frame, targets, views);
            frame.MarkOutput(color);
            frame.Execute();
            f.Device->WaitForIdle();
        }

        auto* feature = f.Rs->GetFeature<VolumetricFogRenderer>();
        ASSERT_NE(feature, nullptr);

        // Frame 1: fog OFF — the node gates at declaration: no fog passes, and
        // the stage is disabled + history reset for the view.
        f.SetFog(/*enabled=*/false, /*temporal=*/true);
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(1);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("FG.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("FG.Depth", DepthTargetDesc());
        f.Rs->BeginWorldDrawFrame();
        f.Rs->BuildWorldBatchKeys();
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{f.View, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                     f.Rs->Views().GetViews().end());
        instance.Declare(frame, targets, views);

        EXPECT_EQ(FogFixture::CountFogPasses(frame.Graph()), 0u)
            << "a disabled view declares nothing";
        EXPECT_FALSE(feature->IsEnabled(f.View)) << "the disabled stage is off";

        frame.MarkOutput(color);
        frame.Execute();
        f.Device->WaitForIdle();
    }
    f.Down();
}

// 5g — the per-view history pool import rotates History0/History1 ONLY when
// temporal is enabled (state.historyFrame advances only then); a temporal-off
// frame imports no history at all (the pool ages out).
TEST(RenderPipelineDeclareTests, FogHistoryParityRotatesOnlyWhenTemporalEnabled)
{
    FogFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderPipelineBlueprint bp = f.MakeBlueprint();
        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);
        FramePools pools(f.Device.get());

        auto declareFogFrame = [&](uint64_t frameIndex) -> FogFrameResult
        {
            RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
            frame.BeginFrame(frameIndex);
            RenderGraph::RGTexture color = frame.ImportPersistentTexture("FH.Color", ColorTargetDesc());
            RenderGraph::RGTexture depth = frame.ImportPersistentTexture("FH.Depth", DepthTargetDesc());
            f.Rs->BeginWorldDrawFrame();
            f.Rs->BuildWorldBatchKeys();
            const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{f.View, color, depth, {}}};
            const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                         f.Rs->Views().GetViews().end());
            instance.Declare(frame, targets, views);

            FogFrameResult res;
            res.FogPasses = FogFixture::CountFogPasses(frame.Graph());
            res.History0 = FogFixture::HistoryId(frame.Graph(), 0);
            res.History1 = FogFixture::HistoryId(frame.Graph(), 1);
            frame.MarkOutput(color);
            frame.Execute();
            f.Device->WaitForIdle();
            return res;
        };

        // ── Temporal ON: two frames. The write parity flips (History0 frame,
        // History1 frame) and history imports are present each frame. ──
        f.SetFog(/*enabled=*/true, /*temporal=*/true);
        const FogFrameResult t0 = declareFogFrame(0);
        const FogFrameResult t1 = declareFogFrame(1);

        if (t0.FogPasses == 0 && t1.FogPasses == 0)
            GTEST_SKIP() << "fog chain did not declare (headless decline)";

        // Frame 0 writes History0 (historyFrame starts 0) and reads History1;
        // frame 1 writes History1 and reads History0 — the parity rotated. Both
        // ping-pong ids exist each frame (one write, one read).
        EXPECT_NE(t0.History0, RenderGraph::kInvalidId) << "frame 0 imports History0";
        EXPECT_NE(t0.History1, RenderGraph::kInvalidId) << "frame 0 imports History1 (read side)";
        EXPECT_NE(t1.History0, RenderGraph::kInvalidId) << "frame 1 imports History0 (read side)";
        EXPECT_NE(t1.History1, RenderGraph::kInvalidId) << "frame 1 imports History1";

        // ── Temporal OFF: history rotation freezes — no history imports at all
        // (the renderer imports nothing; the pool ages out). ──
        f.SetFog(/*enabled=*/true, /*temporal=*/false);
        const FogFrameResult off = declareFogFrame(2);
        if (off.FogPasses > 0)
        {
            EXPECT_EQ(off.History0, RenderGraph::kInvalidId)
                << "temporal-off frames import no history (pool ages out)";
            EXPECT_EQ(off.History1, RenderGraph::kInvalidId)
                << "temporal-off frames import no history (pool ages out)";
        }
    }
    f.Down();
}

// Every fog pass but Composite records a DISPATCH, so every texture it samples
// must be declared with a compute-scoped read. RGTextureRead::Sampled resolves
// to the FRAGMENT stage on a graphics-queue pass, which is the stage the
// generated barrier's destination scope names — a dispatch reading under that
// barrier reads memory nothing made visible to it. The cascade shadow array
// (ge_shadowMapArray in volumetric_fog_light.comp) is the sharpest case: it is
// written by this frame's cascade depth passes and read RAW here.
//
// Composite is excluded deliberately: it is a fragment pass (AttachColor +
// fullscreen draw), and Sampled is correct there.
TEST(RenderPipelineDeclareTests, FogComputePassesDeclareComputeScopedSampledReads)
{
    FogFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderPipelineBlueprint bp = f.MakeBlueprint();
        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);
        FramePools pools(f.Device.get());

        f.SetFog(/*enabled=*/true, /*temporal=*/true);

        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("FCS.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("FCS.Depth", DepthTargetDesc());
        f.Rs->BeginWorldDrawFrame();
        f.Rs->BuildWorldBatchKeys();
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{f.View, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                     f.Rs->Views().GetViews().end());
        instance.Declare(frame, targets, views);

        const RenderGraph::RGGraph& g = frame.Graph();
        if (FogFixture::CountFogPasses(g) == 0)
        {
            frame.MarkOutput(color);
            frame.Execute();
            f.Device->WaitForIdle();
            f.Down();
            GTEST_SKIP() << "fog chain did not declare (headless decline)";
        }

        uint32_t checked = 0;
        for (const RenderGraph::RGAccessRecord& a : g.Accesses())
        {
            const char* nm = g.PassName(a.Pass);
            if (!nm)
                continue;
            const std::string passName(nm);
            if (!RGQuery::Matches(passName, RGQuery::Subtree{"VolumetricFog"}))
                continue;
            if (RGQuery::Matches(passName, RGQuery::Family{"Composite"}))
                continue;
            const bool sampledRead = a.Access == RenderGraph::RGAccess::Sampled ||
                                     a.Access == RenderGraph::RGAccess::SampledCompute ||
                                     a.Access == RenderGraph::RGAccess::SampledVertex;
            if (!sampledRead)
                continue;
            ++checked;
            const uint32_t stage =
                RenderGraph::MapAccess(a.Access, g.PassQueue(a.Pass)).Stage;
            EXPECT_TRUE(stage & RenderGraph::RGStage::ComputeShader)
                << passName << " samples " << g.ResourceName(a.Resource)
                << " under a barrier scoped to stage mask " << stage
                << ", which omits ComputeShader — the dispatch reads memory the "
                   "barrier never made visible to it";
        }
        EXPECT_GT(checked, 0u) << "no sampled reads found on the fog compute passes; "
                                  "the pin stopped covering anything";

        frame.MarkOutput(color);
        frame.Execute();
        f.Device->WaitForIdle();
    }
    f.Down();
}

// The cascade shadow array is the read the fog fix exists for, and it is the one
// read FogComputePassesDeclareComputeScopedSampledReads cannot see: that test's
// blueprint holds the fog node alone, so no cascade producer publishes into the
// frame, GetShadowMapArrayRG returns invalid, and `if (shadowArr.IsValid())`
// skips the declaration outright. A pin that loops over whatever the fog happens
// to declare therefore stays green with the cascade read reverted to
// RGTextureRead::Sampled.
//
// So declare a real cascade producer ahead of the fog: the ShadowMap node plus a
// shadow-casting sun make the array a graph resource written by this frame's
// cascade depth passes, and volumetric_fog_light.comp samples it from the
// Lighting pass's DISPATCH. Sampled resolves to RGStage::FragmentShader on a
// graphics-queue pass, which is the destination scope of the barrier out of
// those depth writes — the dispatch would read depth no barrier made visible to
// compute.
TEST(RenderPipelineDeclareTests, FogLightingReadsCascadeShadowArrayComputeScoped)
{
    FogFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(f.Registry.Register(
        "ShadowMap", [] { return std::make_unique<Nodes::ShadowMapNode>(); }, true));
    {
        // CSM ahead of Fog: the fog reads the array through the consume-only
        // accessor, which only sees a publish that already happened this frame.
        RenderPipelineBlueprint bp = f.MakeBlueprint();
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "CSM";
            p.type = "ShadowMap";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"CSM","type":"ShadowMap","buffer":"ShadowData"})";
            bp.passes.insert(bp.passes.begin(), p);
        }
        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);
        FramePools pools(f.Device.get());

        f.SetFog(/*enabled=*/true, /*temporal=*/true);

        // Cascades declare only with caster work in the shadow bucket.
        auto depHandle = f.Rs->RegisterDepthEmit(
            [&](DepthEmitContext& ctx, DepthPassType passType)
            {
                if (ctx.ViewId == f.View && passType == DepthPassType::ShadowCascade)
                    ctx.Services->EmitDepthCommand(ctx.ViewId, passType, DrawCommand{});
            });

        ExtractedLight sun{};
        sun.type = GameEngine::Components::LightType::Directional;
        sun.castsShadows = 1;
        sun.castsLight = 1;
        sun.directionWS[1] = -1.0f;
        sun.cascadeCount = 2;

        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("FCA.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("FCA.Depth", DepthTargetDesc());
        f.Rs->BeginWorldDrawFrame();
        f.Rs->BuildWorldBatchKeys();
        f.Rs->SubmitLight(0u, sun);
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{f.View, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                     f.Rs->Views().GetViews().end());
        instance.Declare(frame, targets, views);

        const RenderGraph::RGGraph& g = frame.Graph();
        const size_t cascadePasses = RGQuery::CountDeclared(g, RGQuery::Family{"Cascade"});
        if (FogFixture::CountFogPasses(g) == 0 || cascadePasses == 0)
        {
            frame.MarkOutput(color);
            frame.Execute();
            f.Device->WaitForIdle();
            depHandle.Reset();
            f.Down();
            GTEST_SKIP() << "fog or cascade shaders unavailable in this environment (fog passes "
                         << FogFixture::CountFogPasses(g) << ", cascade passes " << cascadePasses
                         << ") — the pin covers nothing here; build CompileShaderPkgs";
        }

        // With cascades declared, an absent array is a producer bug, not an
        // environment decline — and it would silently empty this pin.
        const RenderGraph::RGTexture shadowArr = f.Rs->GetShadowMapArrayRG(frame, f.View);
        ASSERT_TRUE(shadowArr.IsValid())
            << cascadePasses << " cascade passes declared but the array was never published";

        uint32_t checked = 0;
        for (const RenderGraph::RGAccessRecord& a : g.Accesses())
        {
            const char* nm = g.PassName(a.Pass);
            if (!nm)
                continue;
            const std::string passName(nm);
            if (!RGQuery::Matches(passName, RGQuery::Subtree{"VolumetricFog"}) ||
                !RGQuery::Matches(passName, RGQuery::Family{"Lighting"}))
                continue;
            if (a.Resource != shadowArr.Id)
                continue;
            ++checked;
            const uint32_t stage = RenderGraph::MapAccess(a.Access, g.PassQueue(a.Pass)).Stage;
            EXPECT_TRUE(stage & RenderGraph::RGStage::ComputeShader)
                << passName << " samples the cascade array " << g.ResourceName(a.Resource)
                << " under a barrier scoped to stage mask " << stage
                << ", which omits ComputeShader — volumetric_fog_light.comp reads this frame's "
                   "cascade depth writes with nothing making them visible to the dispatch";
        }
        EXPECT_EQ(checked, 1u)
            << "the Lighting pass's read of the cascade array is this pin's whole subject; found "
            << checked << " such reads";

        frame.MarkOutput(color);
        frame.Execute();
        f.Device->WaitForIdle();

        // The GENERATED barrier, not a second derivation of the declaration: this
        // is the destination scope the backend hands Vulkan, so the pin also
        // covers a MapAccess regression that leaves the declaration intact.
        uint32_t transitions = 0;
        for (const RenderGraph::RGBarrier& b : g.Barriers())
        {
            if (b.Resource != shadowArr.Id ||
                b.NewLayout != RenderGraph::RGImageLayout::ShaderReadOnly)
                continue;
            ++transitions;
            EXPECT_TRUE(b.DstStage & RenderGraph::RGStage::ComputeShader)
                << "the barrier handing the cascade array to its shader reader has destination "
                   "stage mask "
                << b.DstStage << ", which omits ComputeShader — the fog dispatch is not ordered "
                                 "after the cascade depth writes";
        }
        EXPECT_GT(transitions, 0u)
            << "no ShaderReadOnly transition on the cascade array; the barrier-level pin covers "
               "nothing";

        depHandle.Reset();
    }
    f.Down();
}

namespace
{
// A camera + two views + a TerrainUpload pass blueprint, with the terrain node
// and service set up. The module normally registers the node via registrars.
struct TerrainFixture
{
    std::shared_ptr<Rendering::IDevice> Device;
    std::unique_ptr<RenderServices> Rs;
    RenderPipelineNodeRegistry Registry;
    CameraId Cam = 0;

    bool m_Up = false;

    bool Up(bool enableDebugLayer = false)
    {
        if (enableDebugLayer)
        {
            DeviceDesc desc{};
            desc.preferredAPI = GraphicsAPI::Vulkan;
            desc.enableDebugLayer = true;
            desc.enableDynamicRendering = true;
            Device = DeviceFactory::CreateDevice(desc);
            if (Device && !Device->Initialize(desc))
                Device.reset();
        }
        else
            Device = CreateVulkanDeviceFast();
        if (!Device)
            return false;
        Rs = std::make_unique<RenderServices>();
        EXPECT_TRUE(Rs->Initialize(Device.get()));
        TerrainECS::TerrainService::Initialize();
        Cam = Rs->Views().AllocateCamera("TerrainCam");
        CameraData cd{};
        cd.proj[0] = 1.0f;
        cd.proj[5] = 1.0f;
        cd.proj[10] = 0.001f;
        cd.proj[11] = 1.0f;
        cd.proj[14] = 0.1f;
        for (int i = 0; i < 16; i += 5)
        {
            cd.view[i] = 1.0f;
            cd.viewProj[i] = 1.0f;
        }
        Rs->Views().SetCameraData(Cam, cd);
        EXPECT_TRUE(Registry.Register(
            "TerrainUpload", [] { return std::make_unique<TerrainECS::TerrainUploadNode>(); },
            true));
        m_Up = true;
        return true;
    }

    // Grass placement is camera-relative and frustum-culled at cell granularity, so a placement
    // test must supply a REAL projection: the fixture's default identity view-projection is a unit
    // box around the world origin and rejects every cell of a terrain-scale window. 60 deg vertical
    // FOV, reverse-Z LH, far past any range these tests author.
    void SetCameraLookingAt(const Mathematics::Vector3& eye, const Mathematics::Vector3& target)
    {
        CameraData cd{};
        const Mathematics::Matrix4x4 view =
            Mathematics::MakeLookAtLH(eye, target, Mathematics::Vector3(0.0f, 1.0f, 0.0f));
        const Mathematics::Matrix4x4 proj =
            Mathematics::MakePerspectiveLH_ZO_ReverseZ(1.0471976f, 16.0f / 9.0f, 0.1f, 8000.0f);
        const Mathematics::Matrix4x4 viewProj = proj * view;
        std::memcpy(cd.view, view.Data(), sizeof(float) * 16);
        std::memcpy(cd.proj, proj.Data(), sizeof(float) * 16);
        std::memcpy(cd.viewProj, viewProj.Data(), sizeof(float) * 16);
        std::memcpy(cd.viewRel, view.Data(), sizeof(float) * 16);
        std::memcpy(cd.viewProjRel, viewProj.Data(), sizeof(float) * 16);
        cd.cameraPos[0] = eye.x;
        cd.cameraPos[1] = eye.y;
        cd.cameraPos[2] = eye.z;
        cd.cameraPos[3] = 0.0f;
        Rs->Views().SetCameraData(Cam, cd);
    }

    ViewId AddView(const char* name, uint32_t mask)
    {
        const ViewId v = Rs->Views().AllocateView(name, Cam);
        Rs->Views().SetViewRenderLayerMask(v, mask);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        Rs->Views().SetViewTargets(v, 0, 0, 0, clear);
        return v;
    }

    RenderPipelineBlueprint MakeBlueprint()
    {
        RenderPipelineBlueprint bp;
        bp.pipelineName = "TerrainTest";
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "Terrain";
            p.type = "TerrainUpload";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"Terrain","type":"TerrainUpload"})";
            bp.passes.push_back(p);
        }
        bp.outputs.push_back({"FinalColor", "View.Resolve"});
        return bp;
    }

    void Down()
    {
        if (!m_Up)
            return;
        m_Up = false;
        Rs->Shutdown();
        TerrainECS::TerrainService::Shutdown();
        Device->Shutdown();
    }

    ~TerrainFixture() { Down(); }

};
} // namespace

// 5h — the heightmap upload pass declares at most ONCE per frame across views
// (a feature-owned frame stamp, NOT a name dedup that spanned views). The
// stamp is the mechanism the node relies on; assert it directly (deterministic,
// shader-independent) AND the node-level pass count when the pipeline readied.
TEST(RenderPipelineDeclareTests, HeightmapUploadDeclaredOncePerFrameAcrossViews)
{
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        // The claim keys on the render graph's MONOTONIC per-frame index, shared by
        // every window's graph in one engine frame. Two sentinel "graph" identities
        // stand in for a multi-window frame: both windows drive the same RS.
        int graphA = 0, graphB = 0;
        const void* const gA = &graphA;
        const void* const gB = &graphB;
        auto& feature = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();

        // Frame 0 (multi-window): window A's graph wins the flush; window B's graph
        // shares the frame index and is refused -> single flush, no double copy.
        EXPECT_TRUE(feature.TryClaimHeightmapUploadFrameRG(0, gA)) << "window A claims frame 0";
        EXPECT_FALSE(feature.TryClaimHeightmapUploadFrameRG(0, gA)) << "A's second view sees it taken";
        EXPECT_FALSE(feature.TryClaimHeightmapUploadFrameRG(0, gB))
            << "window B shares the monotonic frame index -> refused (single flush)";
        // Healthy multi-window: the flush and CBT.Update land in the SAME graph (A) ->
        // phase-ordered, must NOT trip.
        EXPECT_FALSE(feature.HeightmapUploadClaimedInOtherGraph(0, gA))
            << "same graph as the upload flush -> ordered by phase, no hazard";

        // Frame 1 re-arms (monotonic index advances). A wins the flush again.
        EXPECT_TRUE(feature.TryClaimHeightmapUploadFrameRG(1, gA)) << "the next frame re-arms";
        EXPECT_FALSE(feature.TryClaimHeightmapUploadFrameRG(1, gA));
        // Genuine split: CBT.Update in graph B while the flush ran in A -> the hazard.
        EXPECT_TRUE(feature.HeightmapUploadClaimedInOtherGraph(1, gB))
            << "update graph differs from the upload-flush graph -> the ordering hazard";
        // Monotonic index self-invalidates the stamp: a later frame with no claim can
        // never false-match, so an upload-less frame is inert (no cyclic-wrap misfire).
        EXPECT_FALSE(feature.HeightmapUploadClaimedInOtherGraph(2, gB))
            << "no upload claimed this frame -> nothing to order against";

        // Node-level: two active views in one frame declare at most ONE upload
        // pass (zero when the pipeline didn't ready headless or no uploads
        // pending — the once-per-frame contract is what's pinned).
        const ViewId v0 = f.AddView("TerrainView0", 1u);
        const ViewId v1 = f.AddView("TerrainView1", 1u);
        RenderPipelineBlueprint bp = f.MakeBlueprint();
        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);

        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("TR.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("TR.Depth", DepthTargetDesc());
        f.Rs->BeginWorldDrawFrame();
        f.Rs->BuildWorldBatchKeys();
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{v0, color, depth, {}},
                                                    ViewTargetsRG{v1, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                     f.Rs->Views().GetViews().end());
        instance.Declare(frame, targets, views);

        EXPECT_LE(RGQuery::CountDeclared(frame.Graph(), RGQuery::Exact{"TerrainHeightmapUpload"}), 1u)
            << "the upload pass declares at most once per frame across views";

        frame.MarkOutput(color);
        frame.Execute();
        f.Device->WaitForIdle();

        // Execution ORDER is a separate contract from this once-per-frame one, pinned by
        // TerrainUploadSchedulesBeforeItsBindlessConsumers — which declares a consumer for the
        // flush to be ordered against. Nothing to assert here: this blueprint has no consumer,
        // so the flush has no one to precede.
    }
    f.Down();
}

// The flush must EXECUTE before the passes that sample what it writes, and the pass phase does
// not deliver that. Those textures are bindless, so the flush shares no declared access with its
// consumers and forms a scheduling component of one; the scheduler ranks whole components by the
// minimum (phase, pass id) within them and emits a winning component entirely before it
// reconsiders, so a consumer component holding any earlier kEarlySetup pass carries the consumer
// ahead of the flush. The guarantee is the ordering edge the consumer adds — remove that edge and
// this test reds while every gate test stays green.
TEST(RenderPipelineDeclareTests, TerrainUploadSchedulesBeforeItsBindlessConsumers)
{
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        ASSERT_TRUE(terrain.Initialize(f.Device.get()));
        // TerrainFixture::Up() already registers the "TerrainUpload" factory; these are ours.
        // WorldRender is here to JOIN the components: it reads the indirect args grass placement
        // writes and writes the colour target, so placement, the world draw and colour end up in
        // one scheduling component — the shape production has and the shape the ordering bug
        // needs. Without it, placement is its own component and the upload wins on pass id alone,
        // which is a graph that cannot fail and therefore cannot pin anything.
        EXPECT_TRUE(f.Registry.Register(
            "TerrainGrass", [] { return std::make_unique<TerrainGrass::TerrainGrassRenderNode>(); },
            true));
        EXPECT_TRUE(f.Registry.Register(
            "WorldRender", [] { return std::make_unique<Nodes::WorldRenderNode>(); }, true));

        const uint32 dim = 65;
        const std::vector<float32> heights(static_cast<size_t>(dim) * dim, 0.25f);
        const std::vector<uint8> splat(static_cast<size_t>(dim) * dim * 4u, 0xFFu);
        const TerrainECS::TerrainHandle h{3u, 1u};
        terrain.UploadHeightmap(h, heights.data(), dim, dim);
        terrain.UploadSplatmap(h, splat.data(), dim, dim);
        ASSERT_TRUE(terrain.HasPendingUploads())
            << "with nothing pending no flush pass declares and this test proves nothing";

        // Grass must be active for its Place pass to declare at all — it is the consumer whose
        // ordering against the flush is the whole point.
        Terrain::TerrainGPUParams params{};
        params.WorldSizeX = 512.0f;
        params.WorldSizeZ = 512.0f;
        params.HeightScale = 60.0f;
        params.GrassEnabled = 1u;
        params.GrassMaskThreshold = 0.02f;
        params.GrassDensity = 1.0f;
        terrain.UploadTerrainParamsArray(&params, 1u, GameEngine::Terrain::kDefaultTerrainMaterials,
                                         GameEngine::Terrain::kTerrainLayerRoleCount, 0u);

        RenderPipelineBlueprint bp;
        bp.pipelineName = "TerrainUploadOrderTest";
        {
            RenderPipelineBlueprint::Pass up;
            up.id = "Upload";
            up.type = "TerrainUpload";
            up.enabled = true;
            up.perView = true;
            up.passJson = R"({"id":"Upload","type":"TerrainUpload"})";
            bp.passes.push_back(up);
        }
        {
            RenderPipelineBlueprint::Pass g;
            g.id = "Grass";
            g.type = "TerrainGrass";
            g.enabled = true;
            g.perView = true;
            g.passJson = R"({"id":"Grass","type":"TerrainGrass"})";
            bp.passes.push_back(g);
        }
        {
            RenderPipelineBlueprint::Pass w;
            w.id = "World";
            w.type = "WorldRender";
            w.enabled = true;
            w.perView = true;
            w.passJson = R"({"id":"World","type":"WorldRender"})";
            bp.passes.push_back(w);
        }
        bp.outputs.push_back({"FinalColor", "View.Resolve"});

        const ViewId v = f.AddView("TerrainOrderView", 1u);
        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);

        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("TUO.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("TUO.Depth", DepthTargetDesc());

        // The pass that makes the failing shape: kEarlySetup, declared BEFORE any pipeline node so
        // it takes the lowest pass id, and writing the colour target so it lands in the consumer's
        // component. That component then ranks below the upload's one-pass component, and the
        // scheduler drains it — grass placement included — before the upload unless an ordering
        // edge says otherwise. Production gets this for free from GPUCulling / Sky / shadow passes.
        frame.AddPass(
            "TUO.EarlyWorldSetup", Rendering::PassPhase::kEarlySetup,
            [&](RenderGraph::RGPassBuilder& p) { p.AttachColor(0, color); },
            [](RenderGraph::RGContext&) {});

        f.Rs->BeginWorldDrawFrame();
        f.Rs->BuildWorldBatchKeys();
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{v, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                     f.Rs->Views().GetViews().end());
        instance.Declare(frame, targets, views);

        const RenderGraph::RGPassId uploadPass =
            RGQuery::FindDeclared(frame.Graph(), RGQuery::Exact{"TerrainHeightmapUpload"});
        const RenderGraph::RGPassId placePass =
            RGQuery::FindDeclared(frame.Graph(), RGQuery::Family{"Place"});
        ASSERT_NE(uploadPass, RenderGraph::kInvalidId) << "the flush pass must declare";
        ASSERT_NE(placePass, RenderGraph::kInvalidId)
            << "grass placement must declare — with no consumer there is no order to pin";
        // The control that keeps this test falsifiable. Its red state needs placement and the
        // colour target in ONE component, and what joins them is RenderEntities reading the
        // indirect args placement writes. If the world arm stops declaring — a fixture or node
        // change, not a scheduler change — placement becomes its own component, the flush wins on
        // pass id alone, and the assertion below would pass with the ordering edge deleted.
        ASSERT_NE(RGQuery::FindDeclared(frame.Graph(), RGQuery::Subtree{"RenderEntities"}),
                  RenderGraph::kInvalidId)
            << "no RenderEntities pass: placement is unjoined, so this test can no longer fail";

        frame.MarkOutput(color);
        frame.Execute();
        f.Device->WaitForIdle();

        const size_t uploadIdx = ScheduledIndexOf(frame.Graph(), uploadPass);
        const size_t placeIdx = ScheduledIndexOf(frame.Graph(), placePass);
        ASSERT_NE(uploadIdx, SIZE_MAX) << "the flush pass was culled";
        ASSERT_NE(placeIdx, SIZE_MAX) << "grass placement was culled";
        EXPECT_LT(uploadIdx, placeIdx)
            << "grass placement samples the heightmap this flush writes, so the flush must be "
               "scheduled first (upload="
            << uploadIdx << " place=" << placeIdx << ")";
    }
    f.Down();
}

namespace
{
// One extraction tick, as TerrainExtractionSystem::Update closes every tick — the call that
// advances the feature's extraction clock. Pass a fresh device frame index per tick: the clock is
// device-frame-idempotent, so repeating an index deliberately holds it.
void TickExtraction(TerrainECS::TerrainRenderFeature& terrain, uint32 frame)
{
    Terrain::TerrainGPUParams params{};
    terrain.UploadTerrainParamsArray(&params, 1u, GameEngine::Terrain::kDefaultTerrainMaterials,
                                     GameEngine::Terrain::kTerrainLayerRoleCount, frame);
}
} // namespace

// The GPU-initialization gate. FlushPendingUploads has exactly one caller — the "TerrainUpload"
// pass — so a pipeline that omits it leaves terrain textures created but never written and never
// transitioned out of VK_IMAGE_LAYOUT_UNDEFINED. Sampling those is undefined behaviour that fails
// silently and per-driver (zeros on MoltenVK, which reads downstream as flat terrain and zero
// grass blades rather than as an error, and has cost two rounds of debugging).
//
// The gate withholds the bindless slot while nothing is proven to be draining the upload queue,
// and publishes it the moment the flush either has landed or is certain to land this frame ahead
// of every consumer. All four arms are here, because they are one rule: a texture created before
// any pass has declared is withheld; one created while the pass is live publishes on its own
// creation tick, before its own flush; one whose own initializer never reached the queue is
// withheld even while the pass is live; and one created after the extraction clock has moved past
// the pass's last declare is withheld again.
TEST(RenderPipelineDeclareTests, TerrainBindlessSlotWaitsForTheUploadFlush)
{
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        ASSERT_TRUE(terrain.Initialize(f.Device.get()));
        if (!f.Rs->Textures().IsBindlessEnabled())
            GTEST_SKIP() << "bindless disabled on this device";

        const uint32 dim = 33;
        const std::vector<float32> heights(static_cast<size_t>(dim) * dim, 0.5f);
        const TerrainECS::TerrainHandle h{3u, 1u};
        terrain.UploadHeightmap(h, heights.data(), dim, dim);

        // Arm 1 — no pass has ever declared, so nothing is going to write this image.
        terrain.RegisterHeightmapBindless(h, *f.Rs);
        EXPECT_EQ(terrain.GetHeightmapBindlessIndex(h), 0u)
            << "a slot published before any pass declared would point shaders at an UNDEFINED image";
        TickExtraction(terrain, 0u);

        const ViewId v = f.AddView("TerrainGateView", 1u);
        RenderPipelineBlueprint bp = f.MakeBlueprint();
        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);

        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("TGate.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("TGate.Depth", DepthTargetDesc());
        f.Rs->BeginWorldDrawFrame();
        f.Rs->BuildWorldBatchKeys();
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{v, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                     f.Rs->Views().GetViews().end());
        instance.Declare(frame, targets, views);
        ASSERT_EQ(RGQuery::CountDeclared(frame.Graph(), RGQuery::Exact{"TerrainHeightmapUpload"}), 1u)
            << "a pending upload must declare the flush pass — without it this test proves nothing";
        frame.MarkOutput(color);
        frame.Execute();
        f.Device->WaitForIdle();

        terrain.RegisterHeightmapBindless(h, *f.Rs);
        EXPECT_NE(terrain.GetHeightmapBindlessIndex(h), 0u)
            << "the flush landed, so the next re-registration must publish the slot";

        // Arm 2 — a texture created on the tick after a declared frame, which is every scene
        // open, resolution edit and play-mode exit in a pipeline that has the pass. The
        // registration runs before this tick's params upload, exactly as extraction orders it, so
        // the clock still reads the value the declare stamped: the pass will declare again this
        // frame and flush ahead of every consumer, so the slot must publish NOW. Withholding it
        // costs a frame with no splat and grass roots at the terrain's origin height.
        const TerrainECS::TerrainHandle h2{4u, 1u};
        terrain.UploadHeightmap(h2, heights.data(), dim, dim);
        terrain.RegisterHeightmapBindless(h2, *f.Rs);
        EXPECT_TRUE(terrain.HasPendingUploads())
            << "the point of this arm is a slot published BEFORE its own flush — if the queue is "
               "already drained it proves nothing";
        EXPECT_NE(terrain.GetHeightmapBindlessIndex(h2), 0u)
            << "the pass is live and this texture's initializer is queued, so the slot must "
               "publish on the creation tick — one withheld frame is a visible one";

        // Arm 3 — the pass is live, but THIS texture's staging allocation failed, so the flush
        // that is coming has nothing to record for it. A live pass is not enough on its own: the
        // upload queue is still non-empty (h2's copy is in it), so a gate that asked
        // HasPendingUploads() instead of asking per texture would publish this slot against an
        // image nothing will ever write.
        const TerrainECS::TerrainHandle hStarved{6u, 1u};
        terrain.FailNextStagingAllocationForTests();
        terrain.UploadHeightmap(hStarved, heights.data(), dim, dim);
        terrain.RegisterHeightmapBindless(hStarved, *f.Rs);
        EXPECT_EQ(terrain.GetHeightmapBindlessIndex(hStarved), 0u)
            << "no initializer is queued for this texture, so no flush will define it";

        // The control for that arm: same tick, same clock, same stamp, initializer queued. If it
        // were withheld too, the assertion above would prove nothing about the staging failure.
        const TerrainECS::TerrainHandle hControl{7u, 1u};
        terrain.UploadHeightmap(hControl, heights.data(), dim, dim);
        terrain.RegisterHeightmapBindless(hControl, *f.Rs);
        EXPECT_NE(terrain.GetHeightmapBindlessIndex(hControl), 0u)
            << "the pass is still live on this tick — without this the starved arm above could be "
               "green because the gate re-armed, not because the initializer was missing";

        // Arm 4 — the clock moves on with no declare behind it (a pipeline that lost the pass, a
        // Player with no enabled camera). The stamp goes stale and the gate re-arms, so the next
        // fresh texture is withheld again rather than riding a flush that is not coming.
        TickExtraction(terrain, 1u);
        TickExtraction(terrain, 2u);
        const TerrainECS::TerrainHandle h3{5u, 1u};
        terrain.UploadHeightmap(h3, heights.data(), dim, dim);
        terrain.RegisterHeightmapBindless(h3, *f.Rs);
        EXPECT_EQ(terrain.GetHeightmapBindlessIndex(h3), 0u)
            << "no pass has declared since the clock moved, so nothing is draining the queue";
    }
    f.Down();
}

namespace
{
// Counts the missing-"TerrainUpload"-pass diagnostic as the feature logs it. Installed on the
// process logger, so it sees the Error the same way a user's log does.
struct MissingUploadPassReports
{
    std::shared_ptr<std::atomic<int>> Count = std::make_shared<std::atomic<int>>(0);
    Logger::CallbackSink* Sink = nullptr;
    uint64 CallbackId = 0;

    MissingUploadPassReports()
    {
        Logger::Log::Initialize({});
        auto sink = Logger::MakeUnique<Logger::CallbackSink>();
        Sink = sink.get();
        auto count = Count;
        CallbackId = Sink->RegisterCallback(
            [count](const Logger::LogMessage& msg)
            {
                if (msg.Level == Logger::LogLevel::Error &&
                    msg.Message.find("declares no \"TerrainUpload\" pass") != Logger::String::npos)
                    count->fetch_add(1);
            });
        Logger::Log::AddSink(std::move(sink));
    }
    ~MissingUploadPassReports() { Sink->UnregisterCallback(CallbackId); }

    int Read()
    {
        Logger::Log::Flush();
        return Count->load();
    }
};
} // namespace

// The missing-pass diagnostic fires exactly once, on the eighth extraction tick that finds a
// texture still awaiting its GPU initialization with no flush ever recorded, and names the pass.
TEST(RenderPipelineDeclareTests, TerrainMissingUploadPassIsReportedOnceOnTheEighthTick)
{
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        ASSERT_TRUE(terrain.Initialize(f.Device.get()));
        MissingUploadPassReports reports;

        const uint32 dim = 33;
        const std::vector<float32> heights(static_cast<size_t>(dim) * dim, 0.5f);
        terrain.UploadHeightmap(TerrainECS::TerrainHandle{3u, 1u}, heights.data(), dim, dim);

        for (uint32 frame = 0; frame < 7; ++frame)
        {
            TickExtraction(terrain, frame);
            ASSERT_EQ(reports.Read(), 0) << "reported after only " << (frame + 1) << " tick(s)";
        }
        TickExtraction(terrain, 7u);
        EXPECT_EQ(reports.Read(), 1) << "the eighth tick with no flush ever recorded must report";
        for (uint32 frame = 8; frame < 16; ++frame)
            TickExtraction(terrain, frame);
        EXPECT_EQ(reports.Read(), 1) << "the diagnostic is once-only";
    }
    f.Down();
}

// A pipeline that declares the pass flushes on its first declared frame, which disarms the
// diagnostic for the rest of the session: later ticks, and later texture creations, stay silent.
TEST(RenderPipelineDeclareTests, TerrainMissingUploadPassIsSilentWhenThePassDeclares)
{
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        ASSERT_TRUE(terrain.Initialize(f.Device.get()));
        MissingUploadPassReports reports;

        const uint32 dim = 33;
        const std::vector<float32> heights(static_cast<size_t>(dim) * dim, 0.5f);
        terrain.UploadHeightmap(TerrainECS::TerrainHandle{3u, 1u}, heights.data(), dim, dim);
        TickExtraction(terrain, 0u);

        const ViewId v = f.AddView("TerrainDiagView", 1u);
        RenderPipelineBlueprint bp = f.MakeBlueprint();
        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);
        FramePools pools(f.Device.get());
        {
            RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
            frame.BeginFrame(0);
            RenderGraph::RGTexture color = frame.ImportPersistentTexture("TDiag.Color", ColorTargetDesc());
            RenderGraph::RGTexture depth = frame.ImportPersistentTexture("TDiag.Depth", DepthTargetDesc());
            f.Rs->BeginWorldDrawFrame();
            f.Rs->BuildWorldBatchKeys();
            const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{v, color, depth, {}}};
            const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                         f.Rs->Views().GetViews().end());
            instance.Declare(frame, targets, views);
            ASSERT_EQ(RGQuery::CountDeclared(frame.Graph(), RGQuery::Exact{"TerrainHeightmapUpload"}), 1u)
                << "the control needs the flush to actually record";
            frame.MarkOutput(color);
            frame.Execute();
            f.Device->WaitForIdle();
        }

        // A second texture created after the first flush, then far more ticks than the limit.
        terrain.UploadHeightmap(TerrainECS::TerrainHandle{4u, 1u}, heights.data(), dim, dim);
        for (uint32 frame = 1; frame < 24; ++frame)
            TickExtraction(terrain, frame);
        EXPECT_EQ(reports.Read(), 0) << "a pipeline that has flushed once must never be reported";
    }
    f.Down();
}

namespace
{
// A grass-active terrain: upload one default TerrainGPUParams (grass fields
// default to active) so the gate's ParamsSlot + active-grass count are current.
void MakeGrassActive(TerrainECS::TerrainRenderFeature& terrain, uint32 frameIndex, bool active)
{
    Terrain::TerrainGPUParams params{};
    if (!active)
        params.GrassEnabled = 0; // gate: GrassEnabled bit0 must be set
    terrain.UploadTerrainParamsArray(&params, 1u, GameEngine::Terrain::kDefaultTerrainMaterials,
                                     GameEngine::Terrain::kTerrainLayerRoleCount, frameIndex);
}
} // namespace

// 5i — the grassless gate (main 40c7ff984, folded into the declaration): when
// no terrain has active grass, the node declares NOTHING (no compact pass, no
// imported buffers, no published indirect args). This is the regression pin —
// a grassless scene would otherwise dispatch ~8M candidates + 100-200MB/view.
TEST(RenderPipelineDeclareTests, GrassDeclaresNothingWhenInactive)
{
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        EXPECT_TRUE(terrain.Initialize(f.Device.get()));

        const ViewId v = f.AddView("GrassView", 1u);
        EXPECT_TRUE(f.Registry.Register(
            "TerrainGrass", [] { return std::make_unique<TerrainGrass::TerrainGrassRenderNode>(); },
            true));
        RenderPipelineBlueprint bp;
        bp.pipelineName = "GrassTest";
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "Grass";
            p.type = "TerrainGrass";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"Grass","type":"TerrainGrass"})";
            bp.passes.push_back(p);
        }
        bp.outputs.push_back({"FinalColor", "View.Resolve"});
        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);

        // Grass INACTIVE for this frame.
        MakeGrassActive(terrain, 0u, /*active=*/false);

        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("GR.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("GR.Depth", DepthTargetDesc());
        f.Rs->BeginWorldDrawFrame();
        f.Rs->BuildWorldBatchKeys();
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{v, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                     f.Rs->Views().GetViews().end());
        instance.Declare(frame, targets, views);

        // The whole node subtree, not just the compact pass: the contract is
        // "declares NOTHING", and pinning the subtree keeps a renamed or added
        // grass pass from slipping past a single-name zero-check.
        EXPECT_EQ(RGQuery::CountDeclared(frame.Graph(), RGQuery::Subtree{"Pipeline.GrassTest.Grass"}),
                  0u)
            << "no grass passes when grass is inactive; declared: ["
            << RGQuery::DeclaredNames(frame.Graph(), RGQuery::Subtree{"Pipeline.GrassTest"}) << "]";
        // No indirect-args published into the per-view frame record.
        EXPECT_FALSE(f.Rs->GetShadowMapArrayRG(frame, v).IsValid()); // sanity: nothing shadow either

        frame.MarkOutput(color);
        frame.Execute();
        f.Device->WaitForIdle();
    }
    f.Down();
}

namespace
{
// The grass indirect-args buffer the view's world pass reads this frame (the grass
// node emits it as an Indirect forward-sampled buffer read), or invalid when the
// node declared nothing for the view.
RenderGraph::RGBuffer GrassWorldReadArgs(const RenderServices& rs, RenderGraph::RGFrame& frame,
                                         ViewId viewId)
{
    const auto* pv = rs.Views().FindPerView(viewId);
    if (!pv)
        return {};
    for (const auto& read : pv->ForwardSampledBufferRG)
    {
        if (read.Access == RenderGraph::RGBufferRead::Indirect && read.For.IsFor(frame))
            return read.Buffer;
    }
    return {};
}

// The grass instance buffer as this frame's graph tracks it. External imports
// dedup by handle, so this is the resource the placement pass wrote.
RenderGraph::RGBuffer GrassInstancesInFrame(RenderServices& rs, RenderGraph::RGFrame& frame,
                                            ViewId viewId)
{
    auto* grass = rs.GetFeature<TerrainGrass::TerrainGrassRenderFeature>();
    if (!grass || !rs.GetDevice())
        return {};
    const auto handle = grass->GetInstanceBuffer(viewId, rs.GetDevice()->GetFrameIndex());
    if (!handle.IsValid())
        return {};
    return frame.ImportExternalBuffer("TerrainGrass.Instances.Probe", handle);
}
} // namespace

// 5i — grass-active: the compact pass declares on the GRAPHICS queue, imports
// and WRITES the three external buffers (instances/args/count), and the world
// arm declares a Read(Indirect) on the published indirect-args buffer (the
// decision-2b ordering edge through the spine).
TEST(RenderPipelineDeclareTests, GrassPlaceWritesImportedBuffersAndWorldReadsArgsIndirect)
{
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        EXPECT_TRUE(terrain.Initialize(f.Device.get()));

        const ViewId v = f.AddView("GrassWorldView", 1u);
        EXPECT_TRUE(f.Registry.Register(
            "TerrainGrass", [] { return std::make_unique<TerrainGrass::TerrainGrassRenderNode>(); },
            true));
        EXPECT_TRUE(f.Registry.Register(
            "WorldRender", [] { return std::make_unique<Nodes::WorldRenderNode>(); }, true));
        RenderPipelineBlueprint bp;
        bp.pipelineName = "GrassWorldTest";
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "Grass";
            p.type = "TerrainGrass";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"Grass","type":"TerrainGrass"})";
            bp.passes.push_back(p);
        }
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "World";
            p.type = "WorldRender";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"World","type":"WorldRender"})";
            bp.passes.push_back(p);
        }
        bp.outputs.push_back({"FinalColor", "View.Resolve"});
        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);

        MakeGrassActive(terrain, 0u, /*active=*/true);

        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("GW.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("GW.Depth", DepthTargetDesc());
        f.Rs->BeginWorldDrawFrame();
        f.Rs->BuildWorldBatchKeys();
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{v, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                     f.Rs->Views().GetViews().end());
        instance.Declare(frame, targets, views);

        // The compact pass declared.
        const RenderGraph::RGPassId compactPass =
            RGQuery::FindDeclared(frame.Graph(), RGQuery::Family{"Place"});
        ASSERT_NE(compactPass, RenderGraph::kInvalidId)
            << "grass-active => the placement pass declares";

        // The published indirect-args buffer: the world arm reads it as
        // Indirect (the decision-2b edge); the compact's Write + the world's
        // Read + the scheduling order below prove the RAW edge.
        const RenderGraph::RGBuffer argsRG = GrassWorldReadArgs(*f.Rs, frame, v);
        ASSERT_TRUE(argsRG.IsValid()) << "the node published the indirect-args id";

        // Find the world pass (attaches the scene color) and assert it reads
        // the args buffer.
        // The world arm is the RenderEntities pass — pin it by its own root.
        // (The old needle here was "World", which matched only through the
        // VIEW's debugName inside "RenderEntities[GrassWorldView#N]" — rename
        // the test view and the pin silently empties.)
        RenderGraph::RGPassId worldPass = RenderGraph::kInvalidId;
        bool foundWorld = false;
        for (const auto& rec : frame.Attachments())
            if (!rec.IsDepth && rec.Pass != compactPass)
            {
                const char* nm = frame.Graph().PassName(rec.Pass);
                if (nm && RGQuery::Matches(nm, RGQuery::Subtree{"RenderEntities"}))
                {
                    worldPass = rec.Pass;
                    foundWorld = true;
                }
            }
        if (foundWorld)
        {
            EXPECT_TRUE(frame.Graph().HasReadAccess(worldPass, argsRG.Id))
                << "the world arm reads the grass indirect args (decision 2b)";
            frame.MarkOutput(color);
            frame.Execute();
            f.Device->WaitForIdle();
            EXPECT_LT(ScheduledIndexOf(frame.Graph(), compactPass),
                      ScheduledIndexOf(frame.Graph(), worldPass))
                << "compact orders before the world's indirect draw";
        }
        else
        {
            frame.MarkOutput(color);
            frame.Execute();
            f.Device->WaitForIdle();
        }
    }
    f.Down();
}

namespace
{
// The grass materials compile through the real composer inside the emit, and the fixture's
// RenderServices carries no MaterialBuildContext (the host normally provides it): without this
// the compile is refused, the material's pipeline stays invalid, and the contributor emits
// nothing forever. Adapters come from the staged source mirror; the grass surface + modifier
// from this suite's own exe-side staging. Returns false (after GTEST_SKIP bookkeeping by the
// caller) when the staged sources are absent.
bool ArmGrassMaterialCompile(TerrainFixture& f)
{
    namespace fs = std::filesystem;
    const fs::path adapterDir = GameEngine::TestPaths::StagedRenderingShadersDir();
    const fs::path grassPkgRoot =
        GameEngine::TestPaths::ExecutableDirectory() / "Assets" / "Shaders";
    if (!fs::exists(adapterDir / "Adapters") || !fs::exists(grassPkgRoot / "TerrainGrass"))
        return false;
    Rendering::MaterialBuildContext mbc{};
    mbc.AdapterShaderDir = adapterDir;
    mbc.CacheRoot = fs::temp_directory_path() / "ge_grass_order_cache";
    mbc.IncludeDirs = {adapterDir};
    mbc.PackageShaderDirs = {grassPkgRoot};
    f.Rs->Materials().SetMaterialBuildContext(mbc);
    return true;
}

// Drives frames until the grass forward contributor emits its draws (the first declared frame
// only registers the contributor, and the material may compile asynchronously), then returns the
// view's forward commands. Empty after `maxFrames` = the contributor never emitted.
std::span<const DrawCommand> PumpGrassForwardCommands(TerrainFixture& f,
                                                      RenderPipelineInstance& instance,
                                                      ViewId v, uint32 maxFrames)
{
    for (uint32 i = 0; i < maxFrames; ++i)
    {
        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient,
                                   &pools.Ring);
        frame.BeginFrame(i);
        RenderGraph::RGTexture color =
            frame.ImportPersistentTexture("GO.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth =
            frame.ImportPersistentTexture("GO.Depth", DepthTargetDesc());
        f.Rs->BeginWorldDrawFrame();
        f.Rs->BuildWorldBatchKeys();
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{v, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                     f.Rs->Views().GetViews().end());
        instance.Declare(frame, targets, views);
        frame.MarkOutput(color);
        frame.Execute();
        f.Device->WaitForIdle();

        const auto cmds = f.Rs->GetForwardCommands(v);
        if (!cmds.empty())
            return cmds;

        if (i + 1 == maxFrames)
        {
            // The contributor emitted nothing for the whole pump: dump the stop-chain inputs so
            // the failure says WHY, not just "0 commands".
            auto& terrainF = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
            auto* grassF = f.Rs->GetFeature<TerrainGrass::TerrainGrassRenderFeature>();
            const uint32 slot = terrainF.GetLastTerrainParamsSlot();
            const uint32 fi = f.Device->GetFrameIndex();
            auto* blendMat = f.Rs->Materials().Registry().Find(
                GUID::Derive(GUID{}, "terrain_grass/default_material"));
            auto* ditherMat = f.Rs->Materials().Registry().Find(
                GUID::Derive(GUID{}, "terrain_grass/dither_material"));
            std::printf(
                "PumpGrassForwardCommands: no commands after %u frames — slot=%u params=%u "
                "active=%u grassInit=%d instValid=%d capacity=%u blendMat=%p "
                "ditherMat=%p blendPipe=%d\n",
                maxFrames, slot, terrainF.GetTerrainParamsCount(slot),
                terrainF.GetTerrainGrassActiveCount(slot),
                grassF && grassF->IsInitialized() ? 1 : 0,
                grassF && grassF->GetInstanceBuffer(v, fi).IsValid() ? 1 : 0,
                grassF ? grassF->GetPlan(v, fi).Capacity : 0u,
                static_cast<void*>(blendMat), static_cast<void*>(ditherMat),
                blendMat && f.Device->LookupGraphicsPipeline(blendMat->GetGraphicsPipelineId())
                    ? 1 : 0);
        }
    }
    return {};
}
} // namespace

// The Blend band-order contract: blended grass writes no depth and composites in draw order, so
// the FAR band (LOD 1, indirect record at offset sizeof-one-record) must be emitted BEFORE the
// near band (LOD 0, record offset 0) — otherwise the far carpet composites over near blades
// wherever they overlap on screen (the band-inversion artifact this pins).
//
// The params row uses raw GrassEnabled bit values on purpose: bit0 = enabled, bit3 = the authored
// Blend render mode, bit4 = the row's texture carries alpha. Raw literals keep this test
// compilable on the pre-fix tree, where bit3 is ignored and grass always blends — there it emits
// LOD 0 first and this test is RED.
//
// bit4 is required for the authored mode to be consulted AT ALL: with no alpha anywhere in the
// view the draw takes the opaque fast path regardless of what was authored, which is what
// GrassNoAlphaNeededTakesTheOpaqueFastPath below pins.
TEST(RenderPipelineDeclareTests, GrassBlendModeEmitsFarLodBandFirst)
{
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        EXPECT_TRUE(terrain.Initialize(f.Device.get()));

        const ViewId v = f.AddView("GrassOrderView", 1u);
        EXPECT_TRUE(f.Registry.Register(
            "TerrainGrass", [] { return std::make_unique<TerrainGrass::TerrainGrassRenderNode>(); },
            true));
        RenderPipelineBlueprint bp;
        bp.pipelineName = "GrassOrderTest";
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "Grass";
            p.type = "TerrainGrass";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"Grass","type":"TerrainGrass"})";
            bp.passes.push_back(p);
        }
        bp.outputs.push_back({"FinalColor", "View.Resolve"});
        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);

        Terrain::TerrainGPUParams params{};
        // bit0 enabled, bit3 authored Blend render mode, bit4 the texture carries alpha
        params.GrassEnabled = 1u | 8u | 16u;
        terrain.UploadTerrainParamsArray(&params, 1u, GameEngine::Terrain::kDefaultTerrainMaterials,
                                         GameEngine::Terrain::kTerrainLayerRoleCount, 0u);

        if (!ArmGrassMaterialCompile(f))
        {
            f.Down();
            GTEST_SKIP() << "staged shader sources not found";
        }

        // EXPECT (not ASSERT) throughout: an early return here would skip f.Down() and leak
        // the terrain service singleton into the next fixture in this process.
        const auto cmds = PumpGrassForwardCommands(f, instance, v, 64u);
        EXPECT_EQ(cmds.size(), 2u) << "the grass contributor emits one indirect draw per LOD band";
        if (cmds.size() == 2u && cmds[0].Material != nullptr)
        {
            EXPECT_EQ(cmds[0].Material->GetAlphaMode(), MaterialAlphaMode::Blend)
                << "an all-Blend-authored view draws with the blend material";
            EXPECT_EQ(cmds[0].Material->GetName(), "Terrain/Grass")
                << "the blend material, not one of the three Opaque-class ones";
            // One indirect record per LOD, 20 bytes each (GrassIndirectDrawGPU): LOD 1's record
            // sits at offset 20, LOD 0's at 0.
            EXPECT_EQ(cmds[0].IndirectCommandOffset, 20u) << "far band (LOD 1) must draw first";
            EXPECT_EQ(cmds[1].IndirectCommandOffset, 0u) << "near band (LOD 0) draws second";
        }
        else if (cmds.size() == 2u)
        {
            ADD_FAILURE() << "grass draw carries no material";
        }
    }
    f.Down();
}

// The Dither default: a params row that authors no Blend bit but DOES carry texture alpha (bit4)
// draws with the depth-writing dither material, near band first (early-Z rejects the far band
// behind near blades).
//
// bit4 is set explicitly rather than left to MakeGrassActive's default row, because without it
// this view has no alpha to resolve and takes the opaque fast path — which asserts identically on
// both alpha mode and band order, so the coverage would be lost silently rather than turn red.
TEST(RenderPipelineDeclareTests, GrassDitherModeEmitsNearLodBandFirstWithOpaqueMaterial)
{
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        EXPECT_TRUE(terrain.Initialize(f.Device.get()));

        const ViewId v = f.AddView("GrassDitherOrderView", 1u);
        EXPECT_TRUE(f.Registry.Register(
            "TerrainGrass", [] { return std::make_unique<TerrainGrass::TerrainGrassRenderNode>(); },
            true));
        RenderPipelineBlueprint bp;
        bp.pipelineName = "GrassDitherOrderTest";
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "Grass";
            p.type = "TerrainGrass";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"Grass","type":"TerrainGrass"})";
            bp.passes.push_back(p);
        }
        bp.outputs.push_back({"FinalColor", "View.Resolve"});
        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);

        {
            Terrain::TerrainGPUParams params{};
            params.GrassEnabled = 1u | 16u; // bit0 enabled, bit4 the texture carries alpha
            terrain.UploadTerrainParamsArray(&params, 1u,
                                             GameEngine::Terrain::kDefaultTerrainMaterials,
                                             GameEngine::Terrain::kTerrainLayerRoleCount, 0u);
        }

        if (!ArmGrassMaterialCompile(f))
        {
            f.Down();
            GTEST_SKIP() << "staged shader sources not found";
        }

        // EXPECT (not ASSERT) throughout — see the Blend test above.
        const auto cmds = PumpGrassForwardCommands(f, instance, v, 64u);
        EXPECT_EQ(cmds.size(), 2u) << "the grass contributor emits one indirect draw per LOD band";
        if (cmds.size() == 2u && cmds[0].Material != nullptr)
        {
            EXPECT_EQ(cmds[0].Material->GetAlphaMode(), MaterialAlphaMode::Opaque)
                << "dither-mode grass draws with the depth-writing Opaque-class material";
            // The alpha MODE cannot tell the dither material from the opaque fast-path one — both
            // are Opaque-class — so the name is what pins which of the two this view resolved to.
            EXPECT_EQ(cmds[0].Material->GetName(), "Terrain/GrassDither")
                << "a single-sample view with texture alpha takes the screen-door material";
            EXPECT_EQ(cmds[0].IndirectCommandOffset, 0u) << "near band (LOD 0) draws first";
            EXPECT_EQ(cmds[1].IndirectCommandOffset, 20u) << "far band (LOD 1) draws second";
        }
        else if (cmds.size() == 2u)
        {
            ADD_FAILURE() << "grass draw carries no material";
        }
    }
    f.Down();
}

// The engagement rule's predicate, on the cases that need no device. One predicate serves both the
// draw gate (which sets the GPU bit) and the inspector (which shows or hides the alpha controls),
// so a disagreement here is a control that is visible but inert, or inert but hidden.
TEST(RenderPipelineDeclareTests, TerrainGrassNeedsAlphaGeometricBladesNeverNeedAlpha)
{
    Components::TerrainGrass grass{};
    grass.TextureGrass = false;
    // Even with both maps bound: geometric ribbon blades sample no texture at all, so nothing can
    // soften them. This is the case the whole rule exists to keep free of an alpha path.
    grass.AlbedoTextureAssetGuid.Set(GUID::Derive(GUID{}, "test/grass_albedo"));
    grass.AlphaTextureAssetGuid.Set(GUID::Derive(GUID{}, "test/grass_alpha"));
    EXPECT_FALSE(TerrainECS::TerrainGrassNeedsAlpha(nullptr, grass, true));
    EXPECT_FALSE(TerrainECS::TerrainGrassNeedsAlpha(nullptr, grass, false));
}

TEST(RenderPipelineDeclareTests, TerrainGrassNeedsAlphaBoundAlphaMapAlwaysCounts)
{
    Components::TerrainGrass grass{};
    grass.TextureGrass = true;
    grass.AlphaTextureAssetGuid.Set(GUID::Derive(GUID{}, "test/grass_alpha"));
    // A separate alpha map is sampled through RED; the probe measures a texture's ALPHA channel and
    // so cannot answer for it. Taken at its word rather than probed, in both fallback directions.
    EXPECT_TRUE(TerrainECS::TerrainGrassNeedsAlpha(nullptr, grass, true));
    EXPECT_TRUE(TerrainECS::TerrainGrassNeedsAlpha(nullptr, grass, false));
}

TEST(RenderPipelineDeclareTests, TerrainGrassNeedsAlphaCardWithNoTexturesIsOpaque)
{
    Components::TerrainGrass grass{};
    grass.TextureGrass = true;
    // Card mode but nothing bound: the surface leaves textureAlpha at 1.0, so the blade is solid.
    EXPECT_FALSE(TerrainECS::TerrainGrassNeedsAlpha(nullptr, grass, true));
    EXPECT_FALSE(TerrainECS::TerrainGrassNeedsAlpha(nullptr, grass, false));
}

TEST(RenderPipelineDeclareTests, TerrainGrassNeedsAlphaUnaskableAlbedoTakesTheCallersDirection)
{
    Components::TerrainGrass grass{};
    grass.TextureGrass = true;
    grass.AlbedoTextureAssetGuid.Set(GUID::Derive(GUID{}, "test/grass_albedo"));
    // With no renderer to ask, the two callers want opposite safe answers: extraction says "no
    // texture resolved either, so it is opaque", the inspector says "keep the control visible".
    EXPECT_FALSE(TerrainECS::TerrainGrassNeedsAlpha(nullptr, grass, /*answerWithoutServices=*/false));
    EXPECT_TRUE(TerrainECS::TerrainGrassNeedsAlpha(nullptr, grass, /*answerWithoutServices=*/true));
}

// The engagement rule at the draw level: a grass row that binds no alpha-carrying texture (bit4
// clear) has nothing for a cutout, a screen door or alpha-to-coverage to resolve, so the view
// draws with the opaque fast-path material — no alpha-to-coverage, and no discard taken — whatever
// mode was authored. The discard instruction is not merely unreached: that material composes under
// the GRASS_OPAQUE keyword, which compiles the cutout out, so the module carries no OpKill and can
// early-Z — asserted against the compiled SPIR-V by ShippedSurfaceCompose, which is where a claim
// about a compiled module belongs. This test's own subject is the MATERIAL SELECTION, nothing more.
// bit3 (Blend) is set here precisely to prove the rule OUTRANKS the authored mode;
// the Blend test above is the control, differing only in bit4.
//
// This is the case geometric ribbon grass is in by construction: it samples no texture at all.
TEST(RenderPipelineDeclareTests, GrassNoAlphaNeededTakesTheOpaqueFastPath)
{
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        EXPECT_TRUE(terrain.Initialize(f.Device.get()));

        const ViewId v = f.AddView("GrassOpaqueFastPathView", 1u);
        EXPECT_TRUE(f.Registry.Register(
            "TerrainGrass", [] { return std::make_unique<TerrainGrass::TerrainGrassRenderNode>(); },
            true));
        RenderPipelineBlueprint bp;
        bp.pipelineName = "GrassOpaqueFastPathTest";
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "Grass";
            p.type = "TerrainGrass";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"Grass","type":"TerrainGrass"})";
            bp.passes.push_back(p);
        }
        bp.outputs.push_back({"FinalColor", "View.Resolve"});
        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);

        {
            Terrain::TerrainGPUParams params{};
            // bit0 enabled, bit3 authored Blend, bit4 DELIBERATELY CLEAR: no alpha anywhere.
            params.GrassEnabled = 1u | 8u;
            terrain.UploadTerrainParamsArray(&params, 1u,
                                             GameEngine::Terrain::kDefaultTerrainMaterials,
                                             GameEngine::Terrain::kTerrainLayerRoleCount, 0u);
        }

        if (!ArmGrassMaterialCompile(f))
        {
            f.Down();
            GTEST_SKIP() << "staged shader sources not found";
        }

        // EXPECT (not ASSERT) throughout — see the Blend test above.
        const auto cmds = PumpGrassForwardCommands(f, instance, v, 64u);
        EXPECT_EQ(cmds.size(), 2u) << "the grass contributor emits one indirect draw per LOD band";
        if (cmds.size() == 2u && cmds[0].Material != nullptr)
        {
            EXPECT_EQ(cmds[0].Material->GetName(), "Terrain/GrassOpaque")
                << "no alpha anywhere in the view must outrank the authored Blend mode";
            EXPECT_EQ(cmds[0].Material->GetAlphaMode(), MaterialAlphaMode::Opaque)
                << "the opaque fast path writes and tests depth";
            // Depth-writing, so near band first — the Blend ordering does not apply once the
            // authored Blend has been overridden.
            EXPECT_EQ(cmds[0].IndirectCommandOffset, 0u) << "near band (LOD 0) draws first";
            EXPECT_EQ(cmds[1].IndirectCommandOffset, 20u) << "far band (LOD 1) draws second";
        }
        else if (cmds.size() == 2u)
        {
            ADD_FAILURE() << "grass draw carries no material";
        }
    }
    f.Down();
}

namespace
{
// Compiles a test blueprint against the fixture's registry.
RenderPipelineBlueprint CompileTestBlueprint(TerrainFixture& f, const std::string& json, const char* fileName)
{
    RenderPipelineAsset asset(GUID::Null(), std::filesystem::path(fileName));
    Vector<uint8> data(json.begin(), json.end());
    asset.LoadFromData(data);
    RenderPipelineCompiler compiler;
    return compiler.Compile(asset, f.Registry);
}

// Brings up the CBT terrain the prepass tests draw: one active 512 m terrain, 64 m high, seen from the south
// edge, with its kernels loaded and its material compiling from the staged sources. Registers the CBT, prepass
// and world nodes. False when the staged sources or the kernels are missing (the caller skips).
bool ActivateCbtTerrain(TerrainFixture& f, TerrainECS::TerrainRenderFeature& terrain)
{
    EXPECT_TRUE(terrain.Initialize(f.Device.get()));
    f.SetCameraLookingAt(Mathematics::Vector3(256.0f, 120.0f, -40.0f), Mathematics::Vector3(256.0f, 0.0f, 256.0f));
    CBTTerrainECS::RegisterCBTPipelineNodes(f.Registry);
    EXPECT_TRUE(f.Registry.Register(
        "DepthPrepass", [] { return std::make_unique<Nodes::DepthPrepassNode>(); }, true));
    EXPECT_TRUE(f.Registry.Register(
        "WorldRender", [] { return std::make_unique<Nodes::WorldRenderNode>(); }, true));
    if (!ArmGrassMaterialCompile(f))
        return false;
    TerrainECS::TerrainInstanceInfo info{};
    info.Handle = TerrainECS::TerrainHandle{3u, 1u};
    info.SizeX = 512.0f;
    info.SizeZ = 512.0f;
    info.HeightScale = 64.0f;
    info.LODRangeScale = 1.0f;
    info.LODLevels = 1u;
    std::vector<TerrainECS::TerrainInstanceInfo> infos;
    infos.push_back(info);
    terrain.SetActiveTerrains(std::move(infos));
    auto& cbt = f.Rs->EnsureFeature<CBTTerrainECS::CBTRenderFeature>();
    cbt.SetActive(true, CBTTerrain::CBTClassifyDesc{}, 8.0f, Components::kNoTerrainSeaLevel,
                  CBTTerrainECS::CBTRenderFeature::DomainConfig{}, 0u);
    return cbt.EnsureInitialized(*f.Device, *f.Rs);
}

// The terrain parameters ActivateCbtTerrain's terrain draws with, uploaded every frame.
void UploadCbtTerrainParams(TerrainECS::TerrainRenderFeature& terrain, uint32 frameIndex)
{
    Terrain::TerrainGPUParams params{};
    params.WorldSizeX = 512.0f;
    params.WorldSizeZ = 512.0f;
    params.HeightScale = 64.0f;
    terrain.UploadTerrainParamsArray(&params, 1u, GameEngine::Terrain::kDefaultTerrainMaterials,
                                     GameEngine::Terrain::kTerrainLayerRoleCount, frameIndex);
}
} // namespace

namespace
{
// One frame of a prepass + grass + world view, as the frame declared it.
struct GrassPrepassFrame
{
    std::vector<DrawCommand> PrepassHeads;
    std::vector<DrawCommand> ForwardDraws;
    bool WorldDepthReadOnly = false;
    bool WorldSamplesDepth = false;     // the parallax batch's colour draw reads the prepass depth
    bool PrepassReadsGrassArgs = false; // the prepass declares its read of the placement's indirect args
};

// Drives frames until the view's camera-prepass stream holds draws (the grass's depth-only variant
// compiles asynchronously after its first request) and returns that frame. `relief` is submitted
// every frame as a parallax batch, so the world pass decides whether its colour draw reads the
// prepass depth. The heads stay empty after `maxFrames` when no prepass draw was emitted.
GrassPrepassFrame PumpGrassPrepassFrames(TerrainFixture& f, RenderPipelineInstance& instance, ViewId v,
                                         Rendering::MeshGPUHandle mesh, const Material& relief,
                                         uint32 maxFrames)
{
    GrassPrepassFrame out{};
    for (uint32 i = 0; i < maxFrames; ++i)
    {
        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(i);
        const RenderGraph::RGTexture color = frame.ImportPersistentTexture("GP.Color", ColorTargetDesc());
        const RenderGraph::RGTexture depth = frame.ImportPersistentTexture("GP.Depth", DepthTargetDesc());
        f.Rs->BeginWorldDrawFrame();
        WorldSubmissionRecord record{};
        record.viewId = v;
        record.meshHandle = mesh;
        record.material = &relief;
        record.renderLayerMask = 1u;
        f.Rs->SubmitWorldSubmissions(std::span<const WorldSubmissionRecord>(&record, 1u));
        f.Rs->BuildWorldBatchKeys();
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{v, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                     f.Rs->Views().GetViews().end());
        instance.Declare(frame, targets, views);
        frame.MarkOutput(color);
        frame.Execute();
        f.Device->WaitForIdle();

        const auto heads = NonOccludingPrepassHeads(f.Rs->Views(), v);
        if (heads.empty() && i + 1 < maxFrames)
            continue;
        out.PrepassHeads.assign(heads.begin(), heads.end());
        const auto forward = f.Rs->GetForwardCommands(v);
        out.ForwardDraws.assign(forward.begin(), forward.end());
        const RenderGraph::RGGraph& graph = frame.Graph();
        for (const RenderGraph::RGAccessRecord& access : graph.Accesses())
        {
            out.WorldSamplesDepth |= access.Resource == depth.Id && access.Access == RenderGraph::RGAccess::Sampled;
            const char* passName = graph.PassName(access.Pass);
            const char* resourceName = graph.ResourceName(access.Resource);
            out.PrepassReadsGrassArgs |= passName && resourceName &&
                                         std::string_view(passName).starts_with("DepthPrepass[") &&
                                         std::string_view(resourceName).starts_with("TerrainGrass.IndirectArgs");
        }
        for (const auto& attachment : frame.Attachments())
        {
            if (attachment.Tex == depth.Id && attachment.IsDepth && attachment.Ops.Load != RenderGraph::RGLoadOp::Clear)
                out.WorldDepthReadOnly = attachment.ReadOnly;
        }
        break;
    }
    return out;
}
} // namespace

// Grass draws its depth into the camera prepass (#2433), so every reader of the prepass depth (GTAO,
// the screen-space shadows, the light-cull bounds, a parallax material's colour pass) sees the
// blades, and the world pass only tests against that depth. The pipeline is the one a game authors
// with the grass listed after the prepass: the compiler declares it ahead of the prepass, which then
// reads the placement's buffers this frame. Each LOD draw carries a head over the same indirect
// record and bindings, drawn with its own depth-only pipeline that runs the colour draw's vertex
// program over the colour draw's vertex input, once the device has built it; with the heads drawn,
// the world depth is attached read-only and the parallax batch beside the grass reads the prepass's
// relief depth.
TEST(RenderPipelineDeclareTests, GrassDrawsItsDepthInThePrepassSoTheWorldDepthStaysReadOnly)
{
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        EXPECT_TRUE(terrain.Initialize(f.Device.get()));

        const ViewId v = f.AddView("GrassPrepassView", 1u);
        TerrainGrass::RegisterTerrainGrassPipelineNodes(f.Registry);
        EXPECT_TRUE(f.Registry.Register(
            "DepthPrepass", [] { return std::make_unique<Nodes::DepthPrepassNode>(); }, true));
        EXPECT_TRUE(f.Registry.Register(
            "WorldRender", [] { return std::make_unique<Nodes::WorldRenderNode>(); }, true));

        const std::string json = R"json({
          "schemaVersion": 2, "pipelineName": "GrassPrepassTest",
          "passes": [
            { "id": "Prepass", "type": "DepthPrepass", "clearDepthValue": 0.0 },
            { "id": "Grass", "type": "TerrainGrass" },
            { "id": "World", "type": "WorldRender" }
          ]
        })json";
        RenderPipelineAsset asset(GUID::Null(), std::filesystem::path("grass_prepass.rendergraph"));
        Vector<uint8> data(json.begin(), json.end());
        asset.LoadFromData(data);
        RenderPipelineCompiler compiler;
        RenderPipelineBlueprint bp = compiler.Compile(asset, f.Registry);
        EXPECT_FALSE(bp.HasErrors()) << "a feeder listed after the prepass must not reject the pipeline";
        ASSERT_EQ(bp.passes.size(), 3u);
        EXPECT_EQ(bp.passes[0].type, "TerrainGrass") << "the compiler declares the grass ahead of the prepass";
        EXPECT_EQ(bp.passes[1].type, "DepthPrepass");
        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);

        {
            Terrain::TerrainGPUParams params{};
            params.GrassEnabled = 1u; // enabled, no alpha anywhere: the opaque blades
            terrain.UploadTerrainParamsArray(&params, 1u, GameEngine::Terrain::kDefaultTerrainMaterials,
                                             GameEngine::Terrain::kTerrainLayerRoleCount, 0u);
        }
        if (!ArmGrassMaterialCompile(f))
        {
            f.Down();
            GTEST_SKIP() << "staged shader sources not found";
        }

        Mesh triangle{};
        triangle.Name = "GrassPrepassReliefTriangle";
        Vertex v0{}, v1{}, v2{};
        v0.Position[1] = 1.0f;
        v1.Position[0] = -1.0f;
        v1.Position[1] = -1.0f;
        v2.Position[0] = 1.0f;
        v2.Position[1] = -1.0f;
        v0.Normal[2] = v1.Normal[2] = v2.Normal[2] = 1.0f;
        triangle.Vertices = {v0, v1, v2};
        triangle.Indices = {0, 1, 2};
        const Rendering::MeshGPUHandle mesh = f.Rs->GetMeshGPURegistry().RegisterSubmesh({GUID::Generate(), 0}, triangle);
        Material relief = Material::TestFactory::Create(GUID::Generate(), "GrassPrepassRelief", 32u);
        Material::TestFactory::SetGraphicsPipelineId(relief, Rendering::GraphicsPipelineId{2u});
        Material::TestFactory::SetGpuSceneMaterialIndex(relief, 2u);
        {
            Rendering::ShaderVariantKey key{};
            key.materialKeywords = Rendering::MaterialKeyword::Parallax;
            Material::TestFactory::SetVariantKey(relief, key);
        }

        // EXPECT (not ASSERT) from here: an early return would skip f.Down() and leak the terrain
        // service singleton into the next fixture in this process.
        const GrassPrepassFrame decl = PumpGrassPrepassFrames(f, instance, v, mesh, relief, 96u);
        EXPECT_EQ(decl.ForwardDraws.size(), 2u) << "one grass draw per LOD band";
        EXPECT_EQ(decl.PrepassHeads.size(), decl.ForwardDraws.size())
            << "every grass draw carries its depth-only head into the non-occluding prepass";
        for (size_t i = 0; i < std::min(decl.PrepassHeads.size(), decl.ForwardDraws.size()); ++i)
        {
            const DrawCommand& head = decl.PrepassHeads[i];
            const DrawCommand& colour = decl.ForwardDraws[i];
            EXPECT_EQ(head.IndirectCommandBuffer, colour.IndirectCommandBuffer) << "draw " << i;
            EXPECT_EQ(head.IndirectCountBuffer, colour.IndirectCountBuffer) << "draw " << i;
            EXPECT_EQ(head.IndirectCommandOffset, colour.IndirectCommandOffset)
                << "draw " << i << ": the head draws the colour draw's own LOD record";
            EXPECT_EQ(head.Bindings.Buffers.data(), colour.Bindings.Buffers.data())
                << "draw " << i << ": the head reads the colour draw's instance pool and bindings";
            EXPECT_TRUE(head.InternedPipeline.IsValid()) << "draw " << i;
            EXPECT_NE(head.InternedPipeline, colour.InternedPipeline)
                << "draw " << i << ": the head draws with the depth-only pipeline, not the colour one";
            // The vertex stage reads the blade instances from set 2, so the depth-only pipeline declares sets 0 to 2
            // and the binder binds exactly those (a set-0-only layout does not translate on Metal).
            const Rendering::GraphicsPipelineDesc* headDesc = f.Device->LookupGraphicsPipeline(head.InternedPipeline);
            EXPECT_TRUE(headDesc != nullptr && headDesc->PixelShader == nullptr)
                << "draw " << i << ": the opaque blades' head has no fragment stage";
            EXPECT_GE(head.PipelineSetCount, 3u) << "draw " << i;
            EXPECT_EQ(headDesc ? headDesc->DescriptorSetLayouts.size() : 0u, size_t{head.PipelineSetCount}) << "draw " << i;
            // The head places each blade where the colour draw does: the same vertex program over the same
            // vertex input (the colour pipeline's own layout, DepthHeadVertexFlags' vertex-modified rule), reading
            // the same record and buffers (above). Positions are invariant across the two programs' builds.
            const Rendering::GraphicsPipelineDesc* colourDesc = f.Device->LookupGraphicsPipeline(colour.InternedPipeline);
            ASSERT_TRUE(headDesc != nullptr && colourDesc != nullptr) << "draw " << i;
            ASSERT_TRUE(headDesc->VertexShader && colourDesc->VertexShader) << "draw " << i;
            EXPECT_EQ(*headDesc->VertexShader, *colourDesc->VertexShader)
                << "draw " << i << ": the head's vertex stage is the colour draw's own";
            ASSERT_EQ(headDesc->VertexBindings.size(), colourDesc->VertexBindings.size()) << "draw " << i;
            for (size_t b = 0; b < headDesc->VertexBindings.size(); ++b)
            {
                EXPECT_EQ(headDesc->VertexBindings[b].binding, colourDesc->VertexBindings[b].binding);
                EXPECT_EQ(headDesc->VertexBindings[b].stride, colourDesc->VertexBindings[b].stride);
                EXPECT_EQ(headDesc->VertexBindings[b].inputRate, colourDesc->VertexBindings[b].inputRate);
            }
            ASSERT_EQ(headDesc->VertexAttributes.size(), colourDesc->VertexAttributes.size()) << "draw " << i;
            for (size_t a = 0; a < headDesc->VertexAttributes.size(); ++a)
            {
                EXPECT_EQ(headDesc->VertexAttributes[a].location, colourDesc->VertexAttributes[a].location);
                EXPECT_EQ(headDesc->VertexAttributes[a].format, colourDesc->VertexAttributes[a].format);
                EXPECT_EQ(headDesc->VertexAttributes[a].offset, colourDesc->VertexAttributes[a].offset);
            }
            // The device built the head's pipeline for the view's prepass before the producer emitted it: a head
            // the device cannot build is never emitted, and its colour draw writes its own depth instead.
            const Rendering::PipelineFormatKey prepassFormats = f.Rs->Views().GetViewPrepassFormatKey(v);
            EXPECT_NE(prepassFormats.DepthFormat, Rendering::TextureFormat{}) << "the prepass published its formats";
            EXPECT_TRUE(f.Device->TryGetWarmGraphicsPipeline(head.InternedPipeline, prepassFormats).IsValid())
                << "draw " << i << ": the device built the head's pipeline for the prepass";
        }
        EXPECT_TRUE(decl.PrepassReadsGrassArgs)
            << "the prepass declares its read of the placement's indirect args, which orders placement first";
        EXPECT_TRUE(decl.WorldDepthReadOnly)
            << "with the grass's depth in the prepass, the world pass attaches that depth read-only";
        EXPECT_TRUE(decl.WorldSamplesDepth)
            << "the parallax colour draw beside the grass reads the prepass's relief depth";
    }
    f.Down();
}

namespace
{
// One frame of a prepass + CBT terrain + world view, as the frame declared it.
struct CbtPrepassFrame
{
    std::vector<DrawCommand> PrepassHeads;
    std::vector<DrawCommand> ForwardDraws;
    bool WorldDepthReadOnly = false;
    bool PrepassReadsCbtStreams = false; // the prepass declares its reads of CBT.Update's outputs
    size_t UpdateOrder = SIZE_MAX;       // CBT.Update's place in the scheduled order
    size_t PrepassOrder = SIZE_MAX;      // the phase-A prepass's place in it
};

// Drives frames until the view's camera-prepass stream holds CBT's head (the material and its depth variant
// compile asynchronously, and the head waits for the device to build its pipeline) and returns that frame.
CbtPrepassFrame PumpCbtPrepassFrames(TerrainFixture& f, RenderPipelineInstance& instance, ViewId v,
                                     TerrainECS::TerrainRenderFeature& terrain, uint32 maxFrames)
{
    CbtPrepassFrame out{};
    for (uint32 i = 0; i < maxFrames; ++i)
    {
        UploadCbtTerrainParams(terrain, i);
        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(i);
        const RenderGraph::RGTexture color = frame.ImportPersistentTexture("CbtP.Color", ColorTargetDesc());
        const RenderGraph::RGTexture depth = frame.ImportPersistentTexture("CbtP.Depth", DepthTargetDesc());
        f.Rs->BeginWorldDrawFrame();
        f.Rs->BuildWorldBatchKeys();
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{v, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                     f.Rs->Views().GetViews().end());
        instance.Declare(frame, targets, views);
        frame.MarkOutput(color);
        frame.Execute();
        f.Device->WaitForIdle();

        const auto heads = f.Rs->GetDepthCommands(v, DepthPassType::Prepass);
        if (heads.empty() && i + 1 < maxFrames)
            continue;
        out.PrepassHeads.assign(heads.begin(), heads.end());
        const auto forward = f.Rs->GetForwardCommands(v);
        out.ForwardDraws.assign(forward.begin(), forward.end());
        const RenderGraph::RGGraph& graph = frame.Graph();
        const auto& order = graph.ScheduledOrder();
        for (size_t k = 0; k < order.size(); ++k)
        {
            const char* passName = graph.PassName(order[k]);
            if (passName && std::string_view(passName) == "CBT.Update")
                out.UpdateOrder = k;
            if (passName && std::string_view(passName).starts_with("DepthPrepass[") &&
                !std::string_view(passName).ends_with("B") && out.PrepassOrder == SIZE_MAX)
                out.PrepassOrder = k;
        }
        for (const RenderGraph::RGAccessRecord& access : graph.Accesses())
        {
            const char* passName = graph.PassName(access.Pass);
            const char* resourceName = graph.ResourceName(access.Resource);
            out.PrepassReadsCbtStreams |= passName && resourceName &&
                                          std::string_view(passName).starts_with("DepthPrepass[") &&
                                          std::string_view(resourceName).starts_with("CBT.IndicesVisible");
        }
        for (const auto& attachment : frame.Attachments())
        {
            if (attachment.Tex == depth.Id && attachment.IsDepth && attachment.Ops.Load != RenderGraph::RGLoadOp::Clear)
                out.WorldDepthReadOnly = attachment.ReadOnly;
        }
        break;
    }
    return out;
}
} // namespace

// CBT terrain draws its depth into the camera prepass (#2433). The pipeline is one a game authors with the
// terrain listed after the prepass: the compiler declares CBTRender ahead of it, so CBT.Update, which
// CBTRender declares, is scheduled before the prepass, and the prepass reads this frame's VISIBLE stream and
// vertex cache, not last frame's. The terrain's colour draw carries a head over the same VISIBLE-stream
// record, identity index buffer and set-2 buffers, drawn with the material's depth variant (no fragment
// stage, the sets up to set 2 that the vertex stage reads), whose pipeline the device has built; with it
// drawn, the world pass attaches the depth read-only.
TEST(RenderPipelineDeclareTests, CbtTerrainDrawsItsDepthInThePrepassAfterItsUpdate)
{
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        const ViewId v = f.AddView("CbtPrepassView", 1u);
        if (!ActivateCbtTerrain(f, terrain))
        {
            f.Down();
            GTEST_SKIP() << "staged shader sources or CBT kernels unavailable";
        }
        const RenderPipelineBlueprint bp = CompileTestBlueprint(f, R"json({
          "schemaVersion": 2, "pipelineName": "CbtPrepassTest",
          "passes": [
            { "id": "Upload", "type": "TerrainUpload" },
            { "id": "Prepass", "type": "DepthPrepass", "clearDepthValue": 0.0 },
            { "id": "Terrain", "type": "CBTRender" },
            { "id": "World", "type": "WorldRender" }
          ]
        })json", "cbt_prepass.rendergraph");
        EXPECT_FALSE(bp.HasErrors()) << "a feeder listed after the prepass must not reject the pipeline";
        ASSERT_EQ(bp.passes.size(), 4u);
        EXPECT_EQ(bp.passes[1].type, "CBTRender") << "the compiler declares CBTRender ahead of the prepass";
        EXPECT_EQ(bp.passes[2].type, "DepthPrepass");
        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);

        // EXPECT (not ASSERT) from here: an early return would skip f.Down() and leak the terrain
        // service singleton into the next fixture in this process.
        const CbtPrepassFrame decl = PumpCbtPrepassFrames(f, instance, v, terrain, 96u);
        EXPECT_EQ(decl.ForwardDraws.size(), 1u) << "one terrain draw";
        EXPECT_EQ(decl.PrepassHeads.size(), 1u) << "the terrain draw carries its depth-only head into the camera prepass";
        if (decl.PrepassHeads.size() == 1u && decl.ForwardDraws.size() == 1u)
        {
            const DrawCommand& head = decl.PrepassHeads[0];
            const DrawCommand& colour = decl.ForwardDraws[0];
            EXPECT_TRUE(head.UseIndirect);
            EXPECT_EQ(head.IndirectCommandBuffer, colour.IndirectCommandBuffer);
            EXPECT_EQ(head.IndirectCountBuffer, colour.IndirectCountBuffer);
            EXPECT_EQ(head.IndirectCommandOffset, colour.IndirectCommandOffset)
                << "the head draws the colour draw's VISIBLE-stream record";
            EXPECT_EQ(head.Geometry.AltGeom.IB, colour.Geometry.AltGeom.IB) << "the same identity index buffer";
            EXPECT_EQ(head.Bindings.Buffers.data(), colour.Bindings.Buffers.data())
                << "the head reads the colour draw's gIdxVis and gVertex";
            EXPECT_EQ(head.VertexFlags, colour.VertexFlags) << "the colour draw's layout (no vertex buffer)";
            const Rendering::GraphicsPipelineDesc* headDesc = f.Device->LookupGraphicsPipeline(head.InternedPipeline);
            EXPECT_TRUE(headDesc != nullptr && headDesc->PixelShader == nullptr) << "the head has no fragment stage";
            EXPECT_GE(head.PipelineSetCount, 3u) << "the vertex stage reads set 2";
            EXPECT_EQ(headDesc ? headDesc->DescriptorSetLayouts.size() : 0u, size_t{head.PipelineSetCount});
            EXPECT_TRUE(
                f.Device->TryGetWarmGraphicsPipeline(head.InternedPipeline, f.Rs->Views().GetViewPrepassFormatKey(v))
                    .IsValid())
                << "the device built the head's pipeline for the prepass";
        }
        EXPECT_TRUE(decl.PrepassReadsCbtStreams) << "the prepass declares its read of CBT.Update's VISIBLE stream";
        EXPECT_LT(decl.UpdateOrder, decl.PrepassOrder) << "CBT.Update is scheduled before the depth prepass";
        EXPECT_TRUE(decl.WorldDepthReadOnly)
            << "with the terrain's depth in the prepass, the world pass attaches that depth read-only";
    }
    f.Down();
}

// The CBT kernel pipelines build off the declaring thread (#2723). While they build, the terrain declares
// nothing and brings nothing up, and the first frame after they land declares its update and draw.
TEST(RenderPipelineDeclareTests, CbtTerrainDeclaresNothingUntilItsKernelPipelinesHaveBuilt)
{
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        // Stands in for the engine's worker dispatcher, in place of the inline one the headless
        // material system installs.
        std::vector<std::function<void()>> heldBuilds;
        f.Device->SetPipelineBuildDispatcher([&heldBuilds](std::function<void()> build)
                                             { heldBuilds.push_back(std::move(build)); });
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        const ViewId v = f.AddView("CbtPendingView", 1u);
        const bool initializedOnActivation = ActivateCbtTerrain(f, terrain);
        auto& cbt = f.Rs->EnsureFeature<CBTTerrainECS::CBTRenderFeature>();
        if (heldBuilds.empty() && !initializedOnActivation)
        {
            f.Device->SetPipelineBuildDispatcher({});
            f.Down();
            GTEST_SKIP() << "staged shader sources or CBT kernels unavailable";
        }
        EXPECT_FALSE(initializedOnActivation) << "the kernel pipelines were built on the declaring thread";
        const RenderPipelineBlueprint bp = CompileTestBlueprint(f, R"json({
          "schemaVersion": 2, "pipelineName": "CbtPendingTest",
          "passes": [ { "id": "Terrain", "type": "CBTRender" } ]
        })json", "cbt_pending.rendergraph");
        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);

        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        std::array<size_t, 2> declaredPasses{};
        for (uint32 i = 0; i < 2u; ++i)
        {
            UploadCbtTerrainParams(terrain, i);
            frame.BeginFrame(i);
            const RenderGraph::RGTexture color = frame.ImportPersistentTexture("CbtPending.Color", ColorTargetDesc());
            const RenderGraph::RGTexture depth = frame.ImportPersistentTexture("CbtPending.Depth", DepthTargetDesc());
            f.Rs->BeginWorldDrawFrame();
            f.Rs->BuildWorldBatchKeys();
            const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{v, color, depth, {}}};
            const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                         f.Rs->Views().GetViews().end());
            instance.Declare(frame, targets, views);
            declaredPasses[i] = frame.Graph().PassCount();
            if (i == 0u)
            {
                EXPECT_FALSE(cbt.IsInitialized()) << "the terrain came up before its kernel pipelines built";
                EXPECT_EQ(cbt.GetInstance().GetResources().GetPersistentByteSize(), 0u)
                    << "the instance buffers were allocated while the kernels were building";
            }
            frame.MarkOutput(color);
            frame.Execute();
            cbt.OnFrameSubmittedRG(frame, frame.SubmissionToken());
            f.Device->WaitForIdle();
            // The builds land between the frames, as the workers finish them.
            std::vector<std::function<void()>> builds = std::move(heldBuilds);
            heldBuilds.clear();
            for (std::function<void()>& build : builds)
                build();
        }
        EXPECT_EQ(declaredPasses[0], 0u) << "the terrain declared passes while its kernel pipelines were building";
        EXPECT_GT(declaredPasses[1], 0u) << "the terrain declared nothing on the frame after its kernels built";
        EXPECT_TRUE(cbt.IsInitialized());
        f.Device->SetPipelineBuildDispatcher({});
    }
    f.Down();
}

namespace
{
// The camera-prepass depth variant a vertex-modified head draws with (no fragment stage), compiled and
// published as BuildPrepassHead and the grass head request it. Null if it never published.
const PipelineVariantCache::VariantServeUnit* CompileFragmentlessPrepassHead(RenderServices& rs,
                                                                           const Material& material,
                                                                           Rendering::VertexAttributeFlags vertexFlags)
{
    const DepthSegmentPipelineChoice choice =
        ChooseDepthHeadPipeline(Rendering::MaterialKeyword::Instanced, DepthPassType::Prepass, /*alphaTest=*/false,
                                /*vertexModified=*/true, /*sharedDepthOffered=*/false, /*writesReliefDepth=*/false);
    EXPECT_FALSE(choice.ComposesFragment);
    // MaterialSystem::BeginFrame readies the build context, submits the requested compile (inline: the
    // fixture runs no job system) and publishes it, as frame begin does in the engine.
    PipelineVariantCache& variants = rs.Materials().Variants();
    for (int frame = 0; frame < 4; ++frame)
    {
        if (const auto* unit = variants.FindOrRequestPrepassVariant(
                material, vertexFlags, Rendering::PrimitiveTopology::TriangleList, choice.Keywords,
                Rendering::FrontFace::CounterClockwise))
            return unit;
        rs.Materials().BeginFrame();
    }
    return nullptr;
}

// The binding names in `unit`'s published meta that no vertex stage reads, outside the material texture
// set (set 1, whose layout the pipeline patches in whole). Also checks that each published set's layout is
// the one the pipeline declares, so the set the binder builds from the meta binds to the pipeline.
std::vector<std::string> FragmentOnlyBindingsTheHeadDeclares(Rendering::IDevice& device,
                                                             const PipelineVariantCache::VariantServeUnit& unit)
{
    std::vector<std::string> names;
    EXPECT_TRUE(unit.VariantMeta != nullptr);
    if (!unit.VariantMeta)
        return names;
    for (const Rendering::DescriptorSetMeta& set : unit.VariantMeta->Sets)
    {
        if (set.Set >= unit.SetLayouts.size())
            continue;
        if (set.Set != 1u)
            EXPECT_EQ(device.InternDescriptorSetLayout(Rendering::MaterialBuilder::BuildSetLayout(set)),
                      unit.SetLayouts[set.Set])
                << "set " << set.Set << ": the pipeline declares the layout of the meta the binder walks";
        for (const Rendering::DescriptorBindingMeta& binding : set.Bindings)
        {
            if (set.Set != 1u && (binding.StagesMask & Rendering::ShaderMetaStage::kVertex) == 0u)
                names.push_back(binding.Name);
        }
    }
    return names;
}

bool NamesAny(const std::vector<std::string>& names, std::initializer_list<std::string_view> wanted)
{
    return std::any_of(names.begin(), names.end(), [&](const std::string& name) {
        return std::find(wanted.begin(), wanted.end(), std::string_view(name)) != wanted.end();
    });
}

constexpr std::initializer_list<std::string_view> kShadowAndIblSlots = {
    "ge_shadowMapArray", "ge_shadowMomentsArray", "ge_transmittanceShadowArray", "ge_areaShadowMap",
    "ge_spotShadowMap",  "ge_pointShadowMap",     "ge_irradianceCubeTex",        "ge_prefilterCubeTex",
    "ge_brdfLUTTex"};

// What a fragment-less prepass head declares: the meta its binder walks and the bindings in it that no
// vertex stage reads.
struct PrepassHeadDeclaration
{
    std::shared_ptr<Rendering::ShaderMeta> Meta;
    std::vector<std::string> FragmentOnly;
};

// Registers the material and compiles its fragment-less prepass head under the profile the caller's scope
// sets (CompatVertexStageDepthMeta under the compatibility profile).
PrepassHeadDeclaration DeclarePrepassHead(TerrainFixture& f, const MaterialDocument& doc,
                                          Rendering::MaterialKeyword keywords,
                                          Rendering::VertexAttributeFlags vertexFlags)
{
    Material* material = f.Rs->Materials().RegisterMaterialFromDocument(GUID::Generate(), doc, keywords);
    EXPECT_NE(material, nullptr) << doc.materialName;
    if (!material)
        return {};
    const PipelineVariantCache::VariantServeUnit* head = CompileFragmentlessPrepassHead(*f.Rs, *material, vertexFlags);
    EXPECT_NE(head, nullptr) << doc.materialName << ": the prepass depth variant published";
    if (!head)
        return {};
    const Rendering::GraphicsPipelineDesc* desc = f.Device->LookupGraphicsPipeline(head->PipelineId);
    EXPECT_TRUE(desc != nullptr && desc->PixelShader == nullptr) << doc.materialName << ": no fragment stage";
    EXPECT_GE(head->SetLayouts.size(), 3u) << doc.materialName << ": the vertex stage reads set 2";
    return {head->VariantMeta, FragmentOnlyBindingsTheHeadDeclares(*f.Device, *head)};
}

// The bindings `reference` declares for its vertex stage (outside set 1, which the pipeline patches in
// whole) that `head` does not declare at the same set and binding with the same name and type.
std::vector<std::string> VertexReadBindingsMissingFrom(const Rendering::ShaderMeta& head,
                                                       const Rendering::ShaderMeta& reference)
{
    std::vector<std::string> missing;
    for (const Rendering::DescriptorSetMeta& set : reference.Sets)
    {
        if (set.Set == 1u)
            continue;
        const auto headSet = std::find_if(head.Sets.begin(), head.Sets.end(),
                                          [&](const Rendering::DescriptorSetMeta& s) { return s.Set == set.Set; });
        for (const Rendering::DescriptorBindingMeta& binding : set.Bindings)
        {
            if ((binding.StagesMask & Rendering::ShaderMetaStage::kVertex) == 0u)
                continue;
            const bool kept = headSet != head.Sets.end() &&
                              std::any_of(headSet->Bindings.begin(), headSet->Bindings.end(),
                                          [&](const Rendering::DescriptorBindingMeta& b) {
                                              return b.Binding == binding.Binding && b.Name == binding.Name &&
                                                     b.Type == binding.Type;
                                          });
            if (!kept)
                missing.push_back(binding.Name);
        }
    }
    return missing;
}

// The rule both prepass heads are held to (#3367). Under the compatibility profile the head declares no
// binding that only the fragment stage reads, and every binding the desktop head's vertex stage reads is
// still declared. The desktop head keeps the variant's whole reflected layout, shadow and IBL slots included.
void ExpectCompatHeadDeclaresOnlyVertexReads(TerrainFixture& f, const MaterialDocument& doc,
                                             Rendering::MaterialKeyword keywords,
                                             Rendering::VertexAttributeFlags vertexFlags)
{
    PrepassHeadDeclaration compat;
    {
        TestSupport::ScopedCompatShaderProfile compatProfile;
        compat = DeclarePrepassHead(f, doc, keywords, vertexFlags);
    }
    const PrepassHeadDeclaration desktop = DeclarePrepassHead(f, doc, keywords, vertexFlags);
    EXPECT_TRUE(compat.FragmentOnly.empty())
        << doc.materialName << ": the compat head declares " << compat.FragmentOnly.size()
        << " binding(s) only the fragment stage reads, first '"
        << (compat.FragmentOnly.empty() ? std::string{} : compat.FragmentOnly.front()) << "'";
    EXPECT_TRUE(NamesAny(desktop.FragmentOnly, kShadowAndIblSlots))
        << doc.materialName << ": the desktop head keeps the variant's whole reflected layout";
    if (!compat.Meta || !desktop.Meta)
        return;
    const std::vector<std::string> missing = VertexReadBindingsMissingFrom(*compat.Meta, *desktop.Meta);
    EXPECT_TRUE(missing.empty()) << doc.materialName << ": the compat head drops " << missing.size()
                                 << " binding(s) the vertex stage reads, first '"
                                 << (missing.empty() ? std::string{} : missing.front()) << "'";
}

// The grass's opaque-blade material as TerrainGrassRenderFeature registers it (GrassDrawMode::Opaque).
MaterialDocument OpaqueGrassMaterialDocument()
{
    MaterialDocument doc{};
    doc.materialName = "Terrain/GrassOpaque";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "TerrainGrass/terrain_grass_surface.glsl";
    doc.vertexModifier = "TerrainGrass/terrain_grass_vertex_modifier.glsl";
    doc.alphaMode = MaterialAlphaMode::Opaque;
    doc.doubleSided = true;
    doc.properties["roughness"] = 0.9f;
    doc.keywords = {"GRASS_OPAQUE"};
    return doc;
}

constexpr Rendering::MaterialKeyword kGrassMaterialKeywords =
    Rendering::MaterialKeyword::ForwardPlus | Rendering::MaterialKeyword::Shadows |
    Rendering::MaterialKeyword::Instanced | Rendering::MaterialKeyword::IBL |
    Rendering::MaterialKeyword::ProceduralVertexOutput;
} // namespace

// The CBT terrain's camera-prepass head has no fragment stage, but its material is composed lit
// (ForwardPlus | Shadows | IBL), so the variant reflects the fragment's shadow arrays and IBL cubes in set 0.
// Under the compatibility profile the head declares only what its vertex stage reads: WebGPU validates
// every entry of the bind group against the layout, and the prepass provides no shadow map or
// environment, so a declared depth-array slot sank the frame's command buffer (#3367).
TEST(RenderPipelineDeclareTests, CompatCbtPrepassHeadDeclaresOnlyWhatItsVertexStageReads)
{
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    if (!ArmGrassMaterialCompile(f))
    {
        f.Down();
        GTEST_SKIP() << "staged shader sources not found";
    }
    ExpectCompatHeadDeclaresOnlyVertexReads(f, CBTTerrainECS::BuildCBTTerrainMaterialDocument(),
                                            CBTTerrainECS::CBTTerrainMaterialKeywords(),
                                            Rendering::VertexAttributeFlags::None);
    f.Down();
}

// The same rule for the grass's opaque-blade head, which is composed lit the same way and reads its blade
// instances from set 2 (#3367).
TEST(RenderPipelineDeclareTests, CompatGrassPrepassHeadDeclaresOnlyWhatItsVertexStageReads)
{
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    if (!ArmGrassMaterialCompile(f))
    {
        f.Down();
        GTEST_SKIP() << "staged shader sources not found";
    }
    ExpectCompatHeadDeclaresOnlyVertexReads(f, OpaqueGrassMaterialDocument(), kGrassMaterialKeywords,
                                            Rendering::VertexAttributeFlags::None);
    f.Down();
}

// The frame spine reads CBTRenderFeature::WritesDynamicDepth before the CBT node declares, so a change
// that only the node acts on (here a terrain retire, which re-provisions the height source and restarts
// the tree) must already read as dynamic depth before that declare, or the depth-derived readers keep
// last frame's results for the frame the tree is replaced in.
TEST(RenderPipelineDeclareTests, CbtTerrainAtRestReportsARetireBeforeItsNextDeclare)
{
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        const ViewId v = f.AddView("CbtRestView", 1u);
        if (!ActivateCbtTerrain(f, terrain))
        {
            f.Down();
            GTEST_SKIP() << "staged shader sources or CBT kernels unavailable";
        }
        auto& cbt = f.Rs->EnsureFeature<CBTTerrainECS::CBTRenderFeature>();
        const RenderPipelineBlueprint bp = CompileTestBlueprint(f, R"json({
          "schemaVersion": 2, "pipelineName": "CbtRestTest",
          "passes": [ { "id": "Terrain", "type": "CBTRender" } ]
        })json", "cbt_rest.rendergraph");
        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);

        // EXPECT (not ASSERT) from here: an early return would skip f.Down() and leak the terrain
        // service singleton into the next fixture in this process.
        // One frame stream across the frames, as a window keeps: the update's readback completes
        // on that stream's submission timeline.
        FramePools pools(f.Device.get());
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        bool atRest = false;
        for (uint32 i = 0; i < 120u && !atRest; ++i)
        {
            UploadCbtTerrainParams(terrain, i);
            frame.BeginFrame(i);
            const RenderGraph::RGTexture color = frame.ImportPersistentTexture("CbtRest.Color", ColorTargetDesc());
            const RenderGraph::RGTexture depth = frame.ImportPersistentTexture("CbtRest.Depth", DepthTargetDesc());
            f.Rs->BeginWorldDrawFrame();
            f.Rs->BuildWorldBatchKeys();
            const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{v, color, depth, {}}};
            const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                         f.Rs->Views().GetViews().end());
            instance.Declare(frame, targets, views);
            frame.MarkOutput(color);
            frame.Execute();
            cbt.OnFrameSubmittedRG(frame, frame.SubmissionToken());
            f.Device->WaitForIdle();
            atRest = !cbt.WritesDynamicDepth();
        }
        EXPECT_TRUE(atRest) << "the still terrain never came to rest";

        terrain.ReleaseTerrainResources(TerrainECS::TerrainHandle{3u, 1u});
        EXPECT_TRUE(cbt.WritesDynamicDepth())
            << "a retire the CBT node has not consumed yet read as unchanged depth";
    }
    f.Down();
}

namespace
{
// One frame of ActivateCbtTerrain's terrain through `instance`, with the view's depth target read back after
// every pass that writes it (D32: one float per pixel).
struct CbtDepthFrame
{
    std::vector<float> Depth;
    size_t PrepassHeads = 0;
    bool WorldDepthReadOnly = false; // the world pass loaded the prepass depth and attached it read-only
};

CbtDepthFrame RenderCbtDepthFrame(TerrainFixture& f, RenderPipelineInstance& instance, ViewId v,
                                  TerrainECS::TerrainRenderFeature& terrain, uint32 frameIndex)
{
    CbtDepthFrame out{};
    UploadCbtTerrainParams(terrain, frameIndex);
    FramePools pools(f.Device.get());
    RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(frameIndex);
    TextureDesc depthDesc = DepthTargetDesc();
    depthDesc.usage |= static_cast<uint32_t>(TextureUsage::TransferSrc);
    const RenderGraph::RGTexture color = frame.ImportPersistentTexture("CbtDepth.Color", ColorTargetDesc());
    const RenderGraph::RGTexture depth = frame.ImportPersistentTexture("CbtDepth.Depth", depthDesc);
    f.Rs->BeginWorldDrawFrame();
    f.Rs->BuildWorldBatchKeys();
    const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{v, color, depth, {}}};
    const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(), f.Rs->Views().GetViews().end());
    instance.Declare(frame, targets, views);
    const auto ticket = Rendering::RequestTextureReadbackRG(f.Device.get(), frame, depth, "CbtDepth.Depth");
    frame.MarkOutput(color);
    frame.Execute();
    Rendering::OnFrameSubmittedReadbacksRG(frame, frame.SubmissionToken());
    f.Device->WaitForIdle();

    out.PrepassHeads = f.Rs->GetDepthCommands(v, DepthPassType::Prepass).size();
    for (const auto& attachment : frame.Attachments())
    {
        if (attachment.Tex == depth.Id && attachment.IsDepth && attachment.Ops.Load != RenderGraph::RGLoadOp::Clear)
            out.WorldDepthReadOnly = attachment.ReadOnly;
    }
    Rendering::ViewReadbackResult result{};
    if (ticket && ticket->TryGet(result) && result.format == TextureFormat::D32_FLOAT)
    {
        out.Depth.resize(result.pixels.size() / sizeof(float));
        std::memcpy(out.Depth.data(), result.pixels.data(), out.Depth.size() * sizeof(float));
    }
    return out;
}

bool SameBits(const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}
} // namespace

// The terrain's prepass depth is the depth its colour draw writes (#2433; T6 of the terrain LOD-pool design).
// The prepass head and the colour draw are two programs over the same VISIBLE stream, the material's depth
// variant with no fragment stage and its lit colour variant, so they must place every vertex at the same
// clip position. With the head drawn, the world pass attaches the depth read-only and tests GreaterOrEqual
// against it: a head a hair in front of the colour draw rejects that terrain pixel's colour (a hole in the
// ground), and one a hair behind leaves every depth reader a ground that is not where it is drawn. The same settled tree is drawn with the prepass (the depth
// is the head's: the world pass only reads it) and without one (the depth is the colour draw's), and every
// pixel must hold the same depth, bit for bit. The world pass is keyed as the engine's and the games'
// ForwardPlus pipelines key it.
TEST(RenderPipelineDeclareTests, CbtTerrainPrepassDepthIsTheDepthItsColourDrawWrites)
{
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        const ViewId v = f.AddView("CbtDepthView", 1u);
        if (!ActivateCbtTerrain(f, terrain))
        {
            f.Down();
            GTEST_SKIP() << "staged shader sources or CBT kernels unavailable";
        }
        const RenderPipelineBlueprint withPrepass = CompileTestBlueprint(f, R"json({
          "schemaVersion": 2, "pipelineName": "CbtDepthWithPrepass",
          "passes": [
            { "id": "Upload", "type": "TerrainUpload" },
            { "id": "Terrain", "type": "CBTRender" },
            { "id": "Prepass", "type": "DepthPrepass", "clearDepthValue": 0.0 },
            { "id": "World", "type": "WorldRender", "keywords": ["ForwardPlus", "Instanced", "Shadows", "IBL"] }
          ]
        })json", "cbt_depth_prepass.rendergraph");
        const RenderPipelineBlueprint colourOnly = CompileTestBlueprint(f, R"json({
          "schemaVersion": 2, "pipelineName": "CbtDepthColourOnly",
          "passes": [
            { "id": "Upload", "type": "TerrainUpload" },
            { "id": "Terrain", "type": "CBTRender" },
            { "id": "World", "type": "WorldRender", "keywords": ["ForwardPlus", "Instanced", "Shadows", "IBL"] }
          ]
        })json", "cbt_depth_colour.rendergraph");
        EXPECT_FALSE(withPrepass.HasErrors());
        EXPECT_FALSE(colourOnly.HasErrors());
        RenderPipelineInstance prepassInstance(*f.Rs, f.Registry);
        prepassInstance.SetBlueprint(withPrepass);
        RenderPipelineInstance colourInstance(*f.Rs, f.Registry);
        colourInstance.SetBlueprint(colourOnly);

        // EXPECT (not ASSERT) from here: an early return would skip f.Down() and leak the terrain
        // service singleton into the next fixture in this process.
        // Draw with the prepass until the head is drawn and the tree has settled (two frames in a row with the
        // same depth); the material, its depth variant and the head's pipeline compile on the way.
        uint32 frameIndex = 0;
        CbtDepthFrame prepass{};
        bool settled = false;
        for (; frameIndex < 192u && !settled; ++frameIndex)
        {
            CbtDepthFrame next = RenderCbtDepthFrame(f, prepassInstance, v, terrain, frameIndex);
            settled = next.PrepassHeads == 1u && next.WorldDepthReadOnly && !next.Depth.empty() &&
                      SameBits(next.Depth, prepass.Depth);
            prepass = std::move(next);
        }
        EXPECT_TRUE(settled) << "the head never drew, or the tree never settled, in " << frameIndex << " frames";

        const CbtDepthFrame colour = RenderCbtDepthFrame(f, colourInstance, v, terrain, frameIndex++);
        const CbtDepthFrame control = RenderCbtDepthFrame(f, prepassInstance, v, terrain, frameIndex++);
        EXPECT_TRUE(SameBits(control.Depth, prepass.Depth)) << "the tree moved during the comparison";
        EXPECT_EQ(colour.Depth.size(), prepass.Depth.size());
        size_t covered = 0;
        size_t differing = 0;
        for (size_t i = 0; i < std::min(prepass.Depth.size(), colour.Depth.size()); ++i)
        {
            covered += prepass.Depth[i] != 0.0f ? 1u : 0u;
            differing += std::memcmp(&prepass.Depth[i], &colour.Depth[i], sizeof(float)) != 0 ? 1u : 0u;
        }
        EXPECT_GT(covered, prepass.Depth.size() / 4) << "the terrain covers the view";
        EXPECT_EQ(differing, 0u) << "of " << covered
                                 << " terrain pixels, these hold a prepass depth the colour draw does not write";
    }
    f.Down();
}

namespace
{
// The grass the prepass-depth tests draw: one 1536 m terrain row with no heightmap or splat (flat ground at
// y 0, the procedural grass mask), wide still blades (no wind), seen from 2.5 m up looking ahead and down, so
// the blades cover much of the 64 x 64 view. `grassBits` picks the draw mode (kTerrainGrassBit*);
// `albedoBindless` binds a blade texture whose alpha the dithered and alpha-to-coverage modes resolve.
Terrain::TerrainGPUParams StillGrassParams(uint32 grassBits, uint32 albedoBindless)
{
    Terrain::TerrainGPUParams params{};
    params.WorldOriginX = 0.0f;
    params.WorldOriginZ = 0.0f;
    params.WorldSizeX = 1536.0f;
    params.WorldSizeZ = 1536.0f;
    params.HeightScale = 1.0f;
    params.GrassEnabled = grassBits;
    params.GrassDensity = 60.0f;
    params.GrassBladeHeight = 0.72f;
    params.GrassBladeWidth = 0.3f;
    params.GrassMaxWidthRatio = 0.6f;
    params.GrassRange = 60.0f;
    params.GrassWindStrength = 0.0f;
    params.GrassWindFlutterAmount = 0.0f;
    params.GrassWindGustScale = 0.0f;
    params.GrassAlbedoBindless = albedoBindless;
    return params;
}

void LookAtStillGrass(TerrainFixture& f)
{
    f.SetCameraLookingAt(Mathematics::Vector3(768.0f, 2.5f, 760.0f), Mathematics::Vector3(768.0f, 0.0f, 765.0f));
}

// An 8 x 8 blade texture: a green albedo whose alpha steps from 0.1 to 0.97 across it, so the blade's
// dither threshold (or its alpha-to-coverage mask) removes part of every blade. Bindless index 0 when the
// device has no bindless (the caller skips).
uint32 UploadSteppedAlphaBladeTexture(TerrainFixture& f, TextureHandle& out, std::string* why = nullptr)
{
    constexpr uint32 kDim = 8;
    TextureDesc desc{};
    desc.width = kDim;
    desc.height = kDim;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    desc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
    out = f.Device->CreateTexture(desc);
    if (!out.IsValid())
    {
        if (why)
            *why = "texture not created";
        return 0u;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(kDim) * kDim * 4);
    for (uint32 y = 0; y < kDim; ++y)
    {
        for (uint32 x = 0; x < kDim; ++x)
        {
            uint8_t* px = &pixels[(static_cast<size_t>(y) * kDim + x) * 4];
            px[0] = 70;
            px[1] = 150;
            px[2] = 40;
            px[3] = static_cast<uint8_t>(25u + (x * kDim + y) * 222u / (kDim * kDim - 1u));
        }
    }
    const BufferHandle upload = f.Device->CreateUploadBuffer(pixels.size(), "GrassBladeTexture.Upload");
    if (!upload.IsValid())
    {
        if (why)
            *why = "staging buffer not created";
        return 0u;
    }
    f.Device->UpdateBuffer(upload, 0, pixels.size(), pixels.data());
    auto cl = f.Device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(out, ResourceState::Undefined, ResourceState::CopyDest));
    cl->CopyBufferToTextureSubresource(upload, out, 0, 0, kDim, kDim, 0, kDim * 4);
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(out, ResourceState::CopyDest, ResourceState::ShaderResource));
    cl->End();
    f.Device->ExecuteCommandLists({cl.get()});
    f.Device->WaitForIdle();
    f.Device->DestroyBuffer(upload);
    const uint32 index = f.Rs->Textures().GetBindlessIndex(out);
    if (index == 0u && why)
        *why = f.Rs->Textures().IsBindlessEnabled() ? "bindless registration returned 0" : "bindless disabled";
    return index;
}

// Registers the grass, prepass and world nodes, arms the grass material compile and initializes the
// terrain feature. False when the staged shader sources are missing (the caller skips).
bool ActivateStillGrass(TerrainFixture& f, TerrainECS::TerrainRenderFeature& terrain)
{
    EXPECT_TRUE(terrain.Initialize(f.Device.get()));
    TerrainGrass::RegisterTerrainGrassPipelineNodes(f.Registry);
    EXPECT_TRUE(f.Registry.Register(
        "DepthPrepass", [] { return std::make_unique<Nodes::DepthPrepassNode>(); }, true));
    EXPECT_TRUE(f.Registry.Register(
        "WorldRender", [] { return std::make_unique<Nodes::WorldRenderNode>(); }, true));
    LookAtStillGrass(f);
    return ArmGrassMaterialCompile(f);
}

// One frame of the still grass through `instance`. Single-sample: the view's depth target read back (D32, one
// float per pixel) after every pass that writes it. Multisampled: which samples of each pixel hold a depth,
// read by the sample probe below (a multisampled depth target cannot be copied out).
struct GrassDepthFrame
{
    std::vector<float> Depth;
    // View.OccluderDepthResolved, the copy GTAO and the contact shadows read, and View.DepthResolved, the
    // copy every other reader takes; empty when the blueprint has no DepthResolve.
    std::vector<float> ScreenSpaceDepth;
    std::vector<float> ResolvedDepth;
    // Heads of either kind: the camera prepass's and the non-occluding prepass's.
    std::vector<uint8_t> SampleBits;
    size_t PrepassHeads = 0;
    size_t ForwardDraws = 0;
    bool WorldDepthReadOnly = false;
    bool ForwardAlphaToCoverage = false;
};

constexpr uint32 kGrassProbeSamples = 4;

// Reads per-sample coverage out of a 4-sample depth target, as MetalSpirvPipeline.
// DepthOnlyAlphaToCoverageKeepsTheSamplesAColourTargetKeeps does: probe pass k draws a full-screen triangle at a
// depth nearer the far plane than any blade (reverse-Z: 1e-6, beyond 7 km here) with a Greater test against the
// depth, so it passes only on samples that still hold the clear depth, into a cleared colour target with
// gl_SampleMask = 1 << k, and resolves it. A resolved texel that stayed black means sample k holds a blade's depth.
struct GrassSampleProbe
{
    Rendering::IDevice* Device = nullptr;
    PipelineHandle Pipelines[kGrassProbeSamples];
    std::string Why;

    bool Create(Rendering::IDevice& device)
    {
        Device = &device;
        if (!ShaderCompileService::IsCompilerAvailable())
        {
            Why = "no shader compiler in this build";
            return false;
        }
        namespace fs = std::filesystem;
        const fs::path root = fs::temp_directory_path() / ("ge_grass_sample_probe_" + GUID::Generate().ToString());
        for (uint32 k = 0; k < kGrassProbeSamples; ++k)
        {
            ShaderProgramCompileRequest req{};
            req.debugName = "GrassSampleProbe";
            req.baseDirectory = root;
            req.cacheRoot = root / ".Cache" / "Shaders";
            ShaderStageCompileSpec vs{};
            vs.stage = "vs";
            vs.sourcePath = root / "grass_sample_probe.vert";
            vs.inlineSource = R"(#version 450
void main()
{
    const vec2 p[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(p[gl_VertexIndex], 1.0e-6, 1.0);
}
)";
            ShaderStageCompileSpec fsSpec{};
            fsSpec.stage = "fs";
            fsSpec.sourcePath = root / "grass_sample_probe.frag";
            fsSpec.inlineSource = "#version 450\nlayout(location = 0) out vec4 oColor;\nvoid main()\n{\n"
                                  "    gl_SampleMask[0] = " + std::to_string(1u << k) + ";\n"
                                  "    oColor = vec4(1.0);\n}\n";
            req.stages = {vs, fsSpec};
            ShaderProgramCompileResult result{};
            std::string error;
            if (!ShaderCompileService::CompileProgramToCache(req, ShaderSourceKind::SpirV, result, &error) ||
                !result.stageBytes.count("vs") || !result.stageBytes.count("fs"))
            {
                Why = "probe compile failed: " + error;
                break;
            }
            PipelineDesc desc{};
            desc.type = PipelineType::Graphics;
            desc.vertexShader = result.stageBytes.at("vs");
            desc.pixelShader = result.stageBytes.at("fs");
            desc.colorAttachmentFormats = {static_cast<uint32_t>(TextureFormat::RGBA8_UNORM)};
            desc.depthAttachmentFormat = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
            desc.rasterizationSamples = kGrassProbeSamples;
            desc.rasterizationState.cullMode = CullModeFlagBits::None;
            desc.depthStencilState.depthTestEnable = true;
            desc.depthStencilState.depthWriteEnable = false;
            desc.depthStencilState.depthCompareOp = CompareOp::Greater;
            desc.debugName = "GrassSampleProbe";
            Pipelines[k] = device.CreatePipeline(desc);
            if (!Pipelines[k].IsValid())
            {
                Why = "probe pipeline not created";
                break;
            }
        }
        std::error_code ec;
        fs::remove_all(root, ec);
        return Why.empty();
    }

    // Declares the four probe passes over `depth` after everything declared so far, and their readbacks.
    std::vector<std::shared_ptr<Rendering::RGReadbackTicket>> Declare(RenderGraph::RGFrame& frame,
                                                                      RenderGraph::RGTexture depth) const
    {
        std::vector<std::shared_ptr<Rendering::RGReadbackTicket>> tickets;
        TextureDesc resolveDesc = ColorTargetDesc();
        resolveDesc.usage |= static_cast<uint32_t>(TextureUsage::TransferSrc);
        static const char* kTargets[kGrassProbeSamples] = {"GrassProbe.Ms0", "GrassProbe.Ms1", "GrassProbe.Ms2",
                                                           "GrassProbe.Ms3"};
        static const char* kResolves[kGrassProbeSamples] = {"GrassProbe.Resolve0", "GrassProbe.Resolve1",
                                                            "GrassProbe.Resolve2", "GrassProbe.Resolve3"};
        for (uint32 k = 0; k < kGrassProbeSamples; ++k)
        {
            const RenderGraph::RGTexture target = frame.ImportPersistentTexture(kTargets[k], ColorTargetDesc(kGrassProbeSamples));
            const RenderGraph::RGTexture resolve = frame.ImportPersistentTexture(kResolves[k], resolveDesc);
            const PipelineHandle pipeline = Pipelines[k];
            frame.AddPass(
                kTargets[k], Rendering::PassPhase::kPostProcess,
                [&](RenderGraph::RGPassBuilder& p)
                {
                    RenderGraph::RGAttachmentOps clear{};
                    clear.Load = RenderGraph::RGLoadOp::Clear;
                    p.AttachColorResolve(0, target, resolve, clear);
                    RenderGraph::RGAttachmentOps load{};
                    load.Load = RenderGraph::RGLoadOp::Load;
                    p.AttachDepth(depth, load, RenderGraph::RGDepthAccess::ReadOnly);
                    p.PreventCulling();
                },
                [pipeline](RenderGraph::RGContext& ctx)
                {
                    ctx.Cmd->SetPipeline(pipeline);
                    ctx.Cmd->SetViewport(0.0f, 0.0f, 64.0f, 64.0f);
                    ctx.Cmd->SetScissor(0, 0, 64, 64);
                    ctx.Cmd->Draw(3);
                });
            tickets.push_back(Rendering::RequestTextureReadbackRG(Device, frame, resolve, kResolves[k]));
        }
        return tickets;
    }

    // Bit k of each pixel set when sample k holds a depth. Empty when a readback did not arrive.
    static std::vector<uint8_t> Collect(const std::vector<std::shared_ptr<Rendering::RGReadbackTicket>>& tickets)
    {
        std::vector<uint8_t> bits;
        for (uint32 k = 0; k < tickets.size(); ++k)
        {
            Rendering::ViewReadbackResult result{};
            if (!tickets[k] || !tickets[k]->TryGet(result) || result.format != TextureFormat::RGBA8_UNORM)
                return {};
            bits.resize(result.pixels.size() / 4, 0u);
            for (size_t i = 0; i < bits.size(); ++i)
                bits[i] |= result.pixels[i * 4] == 0u ? static_cast<uint8_t>(1u << k) : 0u;
        }
        return bits;
    }

    void Destroy()
    {
        for (PipelineHandle& pipeline : Pipelines)
        {
            if (pipeline.IsValid())
                Device->DestroyPipeline(pipeline);
            pipeline = {};
        }
    }
};

GrassDepthFrame RenderGrassDepthFrame(TerrainFixture& f, RenderPipelineInstance& instance, ViewId v,
                                      TerrainECS::TerrainRenderFeature& terrain,
                                      const Terrain::TerrainGPUParams& params, uint32 frameIndex,
                                      const GrassSampleProbe* probe = nullptr)
{
    GrassDepthFrame out{};
    terrain.UploadTerrainParamsArray(&params, 1u, GameEngine::Terrain::kDefaultTerrainMaterials,
                                     GameEngine::Terrain::kTerrainLayerRoleCount, frameIndex);
    FramePools pools(f.Device.get());
    RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(frameIndex);
    const uint32_t samples = probe ? kGrassProbeSamples : 1u;
    TextureDesc depthDesc = DepthTargetDesc(samples);
    if (!probe)
        depthDesc.usage |= static_cast<uint32_t>(TextureUsage::TransferSrc);
    const RenderGraph::RGTexture color = frame.ImportPersistentTexture("GrassDepth.Color", ColorTargetDesc(samples));
    const RenderGraph::RGTexture depth = frame.ImportPersistentTexture("GrassDepth.Depth", depthDesc);
    const RenderGraph::RGTexture resolve =
        probe ? frame.ImportPersistentTexture("GrassDepth.Resolve", ColorTargetDesc()) : RenderGraph::RGTexture{};
    f.Rs->BeginWorldDrawFrame();
    f.Rs->BuildWorldBatchKeys();
    const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{v, color, depth, resolve}};
    const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(), f.Rs->Views().GetViews().end());
    instance.Declare(frame, targets, views);
    std::shared_ptr<Rendering::RGReadbackTicket> ticket;
    std::vector<std::shared_ptr<Rendering::RGReadbackTicket>> probeTickets;
    std::shared_ptr<Rendering::RGReadbackTicket> screenSpaceTicket;
    std::shared_ptr<Rendering::RGReadbackTicket> resolvedTicket;
    if (probe)
        probeTickets = probe->Declare(frame, depth);
    else
        ticket = Rendering::RequestTextureReadbackRG(f.Device.get(), frame, depth, "GrassDepth.Depth");
    if (const auto* resources = instance.FrameResourcesFor(&frame); resources && !probe)
    {
        const auto occluders = resources->Textures.find({v, Names::View::OccluderDepthResolved});
        if (occluders != resources->Textures.end() && occluders->second.Id != depth.Id)
            screenSpaceTicket =
                Rendering::RequestTextureReadbackRG(f.Device.get(), frame, occluders->second, "GrassDepth.ScreenSpace");
        const auto resolved = resources->Textures.find({v, Names::View::DepthResolved});
        if (resolved != resources->Textures.end() && resolved->second.Id != depth.Id)
            resolvedTicket =
                Rendering::RequestTextureReadbackRG(f.Device.get(), frame, resolved->second, "GrassDepth.Resolved");
    }
    frame.MarkOutput(probe ? resolve : color);
    frame.Execute();
    Rendering::OnFrameSubmittedReadbacksRG(frame, frame.SubmissionToken());
    f.Device->WaitForIdle();

    out.PrepassHeads = f.Rs->GetDepthCommands(v, DepthPassType::Prepass).size();
    if (const auto* pv = f.Rs->Views().FindPerView(v))
        out.PrepassHeads += pv->NonOccludingPrepassHeads.size();
    const auto draws = f.Rs->GetForwardCommands(v);
    out.ForwardDraws = draws.size();
    out.ForwardAlphaToCoverage = !draws.empty();
    for (const DrawCommand& draw : draws)
    {
        const Rendering::GraphicsPipelineDesc* desc = f.Device->LookupGraphicsPipeline(draw.InternedPipeline);
        out.ForwardAlphaToCoverage = out.ForwardAlphaToCoverage && desc && desc->ColorBlend.alphaToCoverageEnable;
    }
    for (const auto& attachment : frame.Attachments())
    {
        // The world pass's attach: it loads the depth and draws into the view's colour target.
        if (attachment.Tex == depth.Id && attachment.IsDepth && attachment.Ops.Load != RenderGraph::RGLoadOp::Clear &&
            std::string_view(frame.Graph().PassName(attachment.Pass)).find("GrassProbe") == std::string_view::npos)
            out.WorldDepthReadOnly = attachment.ReadOnly;
    }
    if (probe)
    {
        out.SampleBits = GrassSampleProbe::Collect(probeTickets);
        return out;
    }
    Rendering::ViewReadbackResult result{};
    if (ticket && ticket->TryGet(result) && result.format == TextureFormat::D32_FLOAT)
    {
        out.Depth.resize(result.pixels.size() / sizeof(float));
        std::memcpy(out.Depth.data(), result.pixels.data(), out.Depth.size() * sizeof(float));
    }
    Rendering::ViewReadbackResult screenSpace{};
    if (screenSpaceTicket && screenSpaceTicket->TryGet(screenSpace) && screenSpace.format == TextureFormat::R32_FLOAT)
    {
        out.ScreenSpaceDepth.resize(screenSpace.pixels.size() / sizeof(float));
        std::memcpy(out.ScreenSpaceDepth.data(), screenSpace.pixels.data(), out.ScreenSpaceDepth.size() * sizeof(float));
    }
    Rendering::ViewReadbackResult resolvedDepth{};
    if (resolvedTicket && resolvedTicket->TryGet(resolvedDepth) && resolvedDepth.format == TextureFormat::R32_FLOAT)
    {
        out.ResolvedDepth.resize(resolvedDepth.pixels.size() / sizeof(float));
        std::memcpy(out.ResolvedDepth.data(), resolvedDepth.pixels.data(), out.ResolvedDepth.size() * sizeof(float));
    }
    return out;
}

// The grass half of T6, for one draw mode: the same still blades drawn with the prepass (the depth is the
// heads': the world pass only reads it) and without one (the depth is the colour draws'), bit for bit. With
// `alphaToCoverage` the view is 4-sample (the dithered mode then resolves by alpha-to-coverage) and what is
// compared is which samples of each pixel hold a depth, read by GrassSampleProbe.
void ExpectGrassPrepassDepthIsTheColourDepth(const char* mode, uint32 grassBits, bool bladeTexture,
                                             bool alphaToCoverage = false)
{
    SCOPED_TRACE(mode);
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        const ViewId v = f.AddView("GrassDepthView", 1u);
        if (!ActivateStillGrass(f, terrain))
        {
            f.Down();
            GTEST_SKIP() << "staged shader sources not found";
        }
        TextureHandle bladeTex{};
        uint32 albedo = 0u;
        if (bladeTexture)
        {
            std::string why;
            albedo = UploadSteppedAlphaBladeTexture(f, bladeTex, &why);
            if (albedo == 0u)
            {
                f.Down();
                GTEST_SKIP() << "no bindless blade texture on this device: " << why;
            }
        }
        GrassSampleProbe probeStore{};
        const GrassSampleProbe* probe = nullptr;
        if (alphaToCoverage)
        {
            if (!probeStore.Create(*f.Device))
            {
                const std::string why = probeStore.Why;
                probeStore.Destroy();
                if (bladeTex.IsValid())
                    f.Device->DestroyTexture(bladeTex);
                f.Down();
                GTEST_SKIP() << "no sample probe on this device: " << why;
            }
            probe = &probeStore;
        }
        const Terrain::TerrainGPUParams params = StillGrassParams(grassBits, albedo);
        const RenderPipelineBlueprint withPrepass = CompileTestBlueprint(f, R"json({
          "schemaVersion": 2, "pipelineName": "GrassDepthWithPrepass",
          "passes": [
            { "id": "Upload", "type": "TerrainUpload" },
            { "id": "Grass", "type": "TerrainGrass" },
            { "id": "Prepass", "type": "DepthPrepass", "clearDepthValue": 0.0 },
            { "id": "World", "type": "WorldRender", "keywords": ["ForwardPlus", "Instanced", "Shadows", "IBL"] }
          ]
        })json", "grass_depth_prepass.rendergraph");
        const RenderPipelineBlueprint colourOnly = CompileTestBlueprint(f, R"json({
          "schemaVersion": 2, "pipelineName": "GrassDepthColourOnly",
          "passes": [
            { "id": "Upload", "type": "TerrainUpload" },
            { "id": "Grass", "type": "TerrainGrass" },
            { "id": "World", "type": "WorldRender", "keywords": ["ForwardPlus", "Instanced", "Shadows", "IBL"] }
          ]
        })json", "grass_depth_colour.rendergraph");
        EXPECT_FALSE(withPrepass.HasErrors());
        EXPECT_FALSE(colourOnly.HasErrors());
        RenderPipelineInstance prepassInstance(*f.Rs, f.Registry);
        prepassInstance.SetBlueprint(withPrepass);
        RenderPipelineInstance colourInstance(*f.Rs, f.Registry);
        colourInstance.SetBlueprint(colourOnly);

        // EXPECT (not ASSERT) from here: an early return would skip f.Down() and leak the terrain
        // service singleton into the next fixture in this process.
        // Draw with the prepass until both LOD draws carry their heads and the blades have settled (two
        // frames in a row with the same depth); the materials, their depth variants and the heads'
        // pipelines compile on the way.
        uint32 frameIndex = 0;
        GrassDepthFrame prepass{};
        bool settled = false;
        for (; frameIndex < 192u && !settled; ++frameIndex)
        {
            GrassDepthFrame next = RenderGrassDepthFrame(f, prepassInstance, v, terrain, params, frameIndex, probe);
            settled = next.ForwardDraws > 0u && next.PrepassHeads == next.ForwardDraws && next.WorldDepthReadOnly &&
                      (probe ? !next.SampleBits.empty() && next.SampleBits == prepass.SampleBits
                             : !next.Depth.empty() && SameBits(next.Depth, prepass.Depth));
            prepass = std::move(next);
        }
        EXPECT_TRUE(settled) << "the heads never drew, or the blades never settled, in " << frameIndex << " frames";

        const GrassDepthFrame colour = RenderGrassDepthFrame(f, colourInstance, v, terrain, params, frameIndex++, probe);
        const GrassDepthFrame control = RenderGrassDepthFrame(f, prepassInstance, v, terrain, params, frameIndex++, probe);
        EXPECT_FALSE(colour.WorldDepthReadOnly) << "with no prepass, the colour draws write the depth";
        if (probe)
        {
            // Every pixel's samples: the head keeps exactly the samples its colour draw keeps. A head that kept
            // more (an alpha of 1) is a hole over the ground behind in the samples the blade does not shade.
            EXPECT_TRUE(colour.ForwardAlphaToCoverage) << "the colour draws resolve their alpha by alpha-to-coverage";
            EXPECT_TRUE(control.SampleBits == prepass.SampleBits) << "the blades moved during the comparison";
            EXPECT_EQ(colour.SampleBits.size(), prepass.SampleBits.size());
            size_t covered = 0;
            size_t partial = 0;
            size_t differing = 0;
            size_t headKeepsMore = 0;
            for (size_t i = 0; i < std::min(prepass.SampleBits.size(), colour.SampleBits.size()); ++i)
            {
                const uint8_t c = colour.SampleBits[i];
                const uint8_t h = prepass.SampleBits[i];
                covered += c != 0u ? 1u : 0u;
                partial += c != 0u && c != 0xFu ? 1u : 0u;
                differing += c != h ? 1u : 0u;
                headKeepsMore += (h & ~c) != 0u ? 1u : 0u;
            }
            // In the test's XML: the backend and the counts the comparison ran over.
            ::testing::Test::RecordProperty("backend", f.Device->GetAPI() == GraphicsAPI::Metal ? "Metal" : "Vulkan");
            ::testing::Test::RecordProperty("bladePixels", static_cast<int>(covered));
            ::testing::Test::RecordProperty("partiallyCovered", static_cast<int>(partial));
            ::testing::Test::RecordProperty("differing", static_cast<int>(differing));
            EXPECT_GT(covered, prepass.SampleBits.size() / 8) << "the blades cover the view";
            EXPECT_GT(partial, covered / 4)
                << "alpha-to-coverage leaves a quarter or more of the blade pixels partially covered (of " << covered << ")";
            EXPECT_EQ(differing, 0u) << "of " << covered << " blade pixels, these keep samples the colour draws do not ("
                                     << headKeepsMore << " keep a sample the colour draws leave empty)";
        }
        else
        {
            EXPECT_TRUE(SameBits(control.Depth, prepass.Depth)) << "the blades moved during the comparison";
            EXPECT_EQ(colour.Depth.size(), prepass.Depth.size());
            size_t covered = 0;
            size_t differing = 0;
            for (size_t i = 0; i < std::min(prepass.Depth.size(), colour.Depth.size()); ++i)
            {
                covered += colour.Depth[i] != 0.0f ? 1u : 0u;
                differing += std::memcmp(&prepass.Depth[i], &colour.Depth[i], sizeof(float)) != 0 ? 1u : 0u;
            }
            EXPECT_GT(covered, prepass.Depth.size() / 8) << "the blades cover the view";
            EXPECT_EQ(differing, 0u) << "of " << covered
                                     << " blade pixels, these hold a prepass depth the colour draws do not write";
        }
        probeStore.Destroy();
        if (bladeTex.IsValid())
            f.Device->DestroyTexture(bladeTex);
    }
    f.Down();
}
} // namespace

// The grass's prepass depth is the depth its colour draws write (#2433, the grass half of T6). Each LOD's head
// draws the colour draw's record over the same instance pool, through its own depth-only pipeline, and must
// cover exactly the pixels the colour draw keeps at exactly its depth: the world pass attaches the depth
// read-only and tests against it, so a head pixel the colour draw does not keep is a blade-shaped hole over
// the ground behind, and one it lacks lets the colour draw test against the ground instead. The same still
// blades are drawn with the prepass and without one, and every pixel must hold the same depth, bit for bit.
// Opaque blades: the head has no fragment stage.
TEST(RenderPipelineDeclareTests, OpaqueGrassPrepassDepthIsTheDepthItsColourDrawsWrite)
{
    ExpectGrassPrepassDepthIsTheColourDepth("opaque", GameEngine::Terrain::kTerrainGrassBitEnabled, /*bladeTexture=*/false);
}

// Dithered blades (a single-sample view with soft alpha): the head runs the colour draw's screen-door discard,
// here over a blade texture whose alpha removes part of every blade.
TEST(RenderPipelineDeclareTests, DitheredGrassPrepassDepthIsTheDepthItsColourDrawsWrite)
{
    ExpectGrassPrepassDepthIsTheColourDepth(
        "dither", GameEngine::Terrain::kTerrainGrassBitEnabled | GameEngine::Terrain::kTerrainGrassBitAlphaNeeded,
        /*bladeTexture=*/true);
}

// Grass under GTAO and the contact shadows (#2311). The blades receive neither pass (their colour draw is
// the material's base pipeline), so they must not occlude what lies around them either. Both passes see
// geometry only through View.OccluderDepthResolved (AONode, RenderServicesWorldPass's contact-shadow view),
// so that copy must be the same with the grass drawn and without it, bit for bit, while the view depth the
// colour pass tests against still holds every blade. Every other reader takes View.DepthResolved, which
// decides what is in front for them (#2822: the ocean's underwater composite painted the lake over the
// blades), so that copy must be the view depth bit for bit, blades included. The scene is grass alone, so a non-occluding pass
// that cleared the depth instead of loading it would pass here; NonOccludingHeadsJoinTheDepthAfterTheScreenSpaceCopy
// asserts the load.
TEST(RenderPipelineDeclareTests, GrassIsNotAnOccluderForTheScreenSpacePasses)
{
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        const ViewId v = f.AddView("GrassOcclusionView", 1u);
        if (!ActivateStillGrass(f, terrain))
        {
            f.Down();
            GTEST_SKIP() << "staged shader sources not found";
        }
        EXPECT_TRUE(f.Registry.Register(
            "DepthResolve", [] { return std::make_unique<Nodes::DepthResolveNode>(); }, true));
        const RenderPipelineBlueprint blueprint = CompileTestBlueprint(f, R"json({
          "schemaVersion": 2, "pipelineName": "GrassScreenSpaceDepth",
          "passes": [
            { "id": "Upload", "type": "TerrainUpload" },
            { "id": "Grass", "type": "TerrainGrass" },
            { "id": "Prepass", "type": "DepthPrepass", "clearDepthValue": 0.0 },
            { "id": "DepthResolve", "type": "DepthResolve" },
            { "id": "World", "type": "WorldRender", "keywords": ["ForwardPlus", "Instanced", "Shadows", "IBL"] }
          ]
        })json", "grass_screen_space_depth.rendergraph");
        EXPECT_FALSE(blueprint.HasErrors());
        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(blueprint);

        // EXPECT (not ASSERT) from here: an early return would skip f.Down().
        const Terrain::TerrainGPUParams grass = StillGrassParams(GameEngine::Terrain::kTerrainGrassBitEnabled, 0u);
        uint32 frameIndex = 0;
        GrassDepthFrame withGrass{};
        bool settled = false;
        for (; frameIndex < 192u && !settled; ++frameIndex)
        {
            GrassDepthFrame next = RenderGrassDepthFrame(f, instance, v, terrain, grass, frameIndex);
            settled = next.ForwardDraws > 0u && next.PrepassHeads == next.ForwardDraws && next.WorldDepthReadOnly &&
                      !next.Depth.empty() && SameBits(next.Depth, withGrass.Depth);
            withGrass = std::move(next);
        }
        EXPECT_TRUE(settled) << "the heads never drew, or the blades never settled, in " << frameIndex << " frames";
        if (withGrass.ScreenSpaceDepth.empty())
        {
            f.Down();
            GTEST_SKIP() << "depth_resolve shaders unavailable in this environment";
        }

        const Terrain::TerrainGPUParams noGrass = StillGrassParams(0u, 0u);
        GrassDepthFrame without = RenderGrassDepthFrame(f, instance, v, terrain, noGrass, frameIndex++);
        without = RenderGrassDepthFrame(f, instance, v, terrain, noGrass, frameIndex++);

        size_t blades = 0;
        for (const float d : withGrass.Depth)
            blades += d != 0.0f ? 1u : 0u;
        EXPECT_GT(blades, withGrass.Depth.size() / 8) << "the blades cover the view depth";
        EXPECT_EQ(without.ForwardDraws, 0u) << "the grass is off in the control frame";
        ASSERT_EQ(withGrass.ScreenSpaceDepth.size(), without.ScreenSpaceDepth.size());
        size_t occluding = 0;
        for (size_t i = 0; i < withGrass.ScreenSpaceDepth.size(); ++i)
            occluding += std::memcmp(&withGrass.ScreenSpaceDepth[i], &without.ScreenSpaceDepth[i], sizeof(float)) != 0
                             ? 1u
                             : 0u;
        EXPECT_EQ(occluding, 0u) << "of " << blades
                                 << " blade pixels, these reach the depth GTAO and the contact shadows read";
        EXPECT_TRUE(SameBits(withGrass.ResolvedDepth, withGrass.Depth))
            << "View.DepthResolved must hold the blades the colour pass depth-tests against";
    }
    f.Down();
}

// Alpha-to-coverage blades (the dithered mode in a 4-sample view): the head writes the colour draw's alpha at
// location 0 with alpha-to-coverage on, so the device keeps the same samples of every pixel. The same still
// blades, over the stepped-alpha blade texture, are drawn with the prepass and without one, and every pixel
// must have the same samples covered. AlphaToCoverageGrassDrawsAHeadThatKeepsItsSamples pins the head's
// pipeline; this pins the value it writes.
TEST(RenderPipelineDeclareTests, AlphaToCoverageGrassPrepassKeepsTheSamplesItsColourDrawsKeep)
{
    ExpectGrassPrepassDepthIsTheColourDepth(
        "alpha-to-coverage",
        GameEngine::Terrain::kTerrainGrassBitEnabled | GameEngine::Terrain::kTerrainGrassBitAlphaNeeded,
        /*bladeTexture=*/true, /*alphaToCoverage=*/true);
}

namespace
{
// Whether a SPIR-V module declares an Output variable at Location 0 (a colour output).
bool SpirvWritesLocation0(const std::vector<uint8_t>& bytes)
{
    if (bytes.size() < 20 || bytes.size() % 4 != 0)
        return false;
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    constexpr uint32_t kOpVariable = 59, kOpDecorate = 71, kDecorationLocation = 30, kStorageOutput = 3;
    std::unordered_set<uint32_t> location0;
    std::unordered_set<uint32_t> outputs;
    for (size_t i = 5; i < words.size();)
    {
        const uint32_t count = words[i] >> 16;
        const uint32_t op = words[i] & 0xFFFFu;
        if (count == 0 || i + count > words.size())
            return false;
        if (op == kOpDecorate && count >= 4 && words[i + 2] == kDecorationLocation && words[i + 3] == 0u)
            location0.insert(words[i + 1]);
        if (op == kOpVariable && count >= 4 && words[i + 3] == kStorageOutput)
            outputs.insert(words[i + 2]);
        i += count;
    }
    for (uint32_t id : location0)
    {
        if (outputs.count(id))
            return true;
    }
    return false;
}
} // namespace

// Alpha-to-coverage grass (the dithered mode in a multisampled view) draws a prepass head too (#2433). Its
// colour draw keeps the samples the hardware derives from the alpha it writes at location 0, so the head's
// depth-only pipeline enables alpha-to-coverage and its fragment stage writes that alpha at location 0, into
// a pass with no colour target (MetalSpirvPipeline.DepthOnlyAlphaToCoverageKeepsTheSamplesAColourTargetKeeps
// pins that the device keeps the same samples). With the heads drawn, the world depth is read-only.
TEST(RenderPipelineDeclareTests, AlphaToCoverageGrassDrawsAHeadThatKeepsItsSamples)
{
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        const ViewId v = f.AddView("GrassA2CView", 1u);
        if (!ActivateStillGrass(f, terrain))
        {
            f.Down();
            GTEST_SKIP() << "staged shader sources not found";
        }
        TextureHandle bladeTex{};
        std::string why;
        const uint32 albedo = UploadSteppedAlphaBladeTexture(f, bladeTex, &why);
        if (albedo == 0u)
        {
            f.Down();
            GTEST_SKIP() << "no bindless blade texture on this device: " << why;
        }
        const Terrain::TerrainGPUParams params = StillGrassParams(
            GameEngine::Terrain::kTerrainGrassBitEnabled | GameEngine::Terrain::kTerrainGrassBitAlphaNeeded, albedo);
        const RenderPipelineBlueprint bp = CompileTestBlueprint(f, R"json({
          "schemaVersion": 2, "pipelineName": "GrassA2CPrepass",
          "passes": [
            { "id": "Upload", "type": "TerrainUpload" },
            { "id": "Grass", "type": "TerrainGrass" },
            { "id": "Prepass", "type": "DepthPrepass", "clearDepthValue": 0.0 },
            { "id": "World", "type": "WorldRender", "keywords": ["ForwardPlus", "Instanced", "Shadows", "IBL"] }
          ]
        })json", "grass_a2c_prepass.rendergraph");
        EXPECT_FALSE(bp.HasErrors());
        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);

        // EXPECT (not ASSERT) from here: an early return would skip f.Down() and leak the terrain
        // service singleton into the next fixture in this process.
        std::vector<DrawCommand> heads;
        std::vector<DrawCommand> draws;
        bool worldDepthReadOnly = false;
        for (uint32 i = 0; i < 192u; ++i)
        {
            terrain.UploadTerrainParamsArray(&params, 1u, GameEngine::Terrain::kDefaultTerrainMaterials,
                                             GameEngine::Terrain::kTerrainLayerRoleCount, i);
            FramePools pools(f.Device.get());
            RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
            frame.BeginFrame(i);
            const RenderGraph::RGTexture color = frame.ImportPersistentTexture("GrassA2C.Color", ColorTargetDesc(4));
            const RenderGraph::RGTexture depth = frame.ImportPersistentTexture("GrassA2C.Depth", DepthTargetDesc(4));
            const RenderGraph::RGTexture resolve = frame.ImportPersistentTexture("GrassA2C.Resolve", ColorTargetDesc());
            f.Rs->BeginWorldDrawFrame();
            f.Rs->BuildWorldBatchKeys();
            const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{v, color, depth, resolve}};
            const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                         f.Rs->Views().GetViews().end());
            instance.Declare(frame, targets, views);
            frame.MarkOutput(resolve);
            frame.Execute();
            f.Device->WaitForIdle();
            const auto h = NonOccludingPrepassHeads(f.Rs->Views(), v);
            const auto d = f.Rs->GetForwardCommands(v);
            if (d.empty() || h.size() != d.size())
                continue;
            heads.assign(h.begin(), h.end());
            draws.assign(d.begin(), d.end());
            for (const auto& attachment : frame.Attachments())
            {
                if (attachment.Tex == depth.Id && attachment.IsDepth && attachment.Ops.Load != RenderGraph::RGLoadOp::Clear)
                    worldDepthReadOnly = attachment.ReadOnly;
            }
            break;
        }
        EXPECT_FALSE(draws.empty()) << "the grass never drew";
        EXPECT_EQ(heads.size(), draws.size()) << "every alpha-to-coverage grass draw carries a prepass head";
        for (size_t i = 0; i < std::min(heads.size(), draws.size()); ++i)
        {
            const Rendering::GraphicsPipelineDesc* headDesc = f.Device->LookupGraphicsPipeline(heads[i].InternedPipeline);
            const Rendering::GraphicsPipelineDesc* colourDesc = f.Device->LookupGraphicsPipeline(draws[i].InternedPipeline);
            EXPECT_TRUE(headDesc && colourDesc) << "draw " << i;
            if (!headDesc || !colourDesc)
                continue;
            EXPECT_TRUE(colourDesc->ColorBlend.alphaToCoverageEnable)
                << "draw " << i << ": the colour draw resolves its alpha by alpha-to-coverage (the mode under test)";
            EXPECT_TRUE(headDesc->ColorBlend.alphaToCoverageEnable)
                << "draw " << i << ": the head's pipeline keeps the samples the same alpha selects";
            EXPECT_TRUE(headDesc->ColorBlend.attachments.empty()) << "draw " << i << ": the head writes no colour target";
            EXPECT_TRUE(headDesc->PixelShader && SpirvWritesLocation0(*headDesc->PixelShader))
                << "draw " << i << ": the head's fragment stage writes the alpha at location 0";
            EXPECT_TRUE(headDesc->VertexShader && colourDesc->VertexShader &&
                        *headDesc->VertexShader == *colourDesc->VertexShader)
                << "draw " << i << ": the head's vertex stage is the colour draw's own";
            EXPECT_TRUE(f.Device->TryGetWarmGraphicsPipeline(heads[i].InternedPipeline,
                                                             f.Rs->Views().GetViewPrepassFormatKey(v)).IsValid())
                << "draw " << i << ": the device built the head's pipeline for the multisampled prepass";
        }
        EXPECT_EQ(uint32_t{f.Rs->Views().GetViewPrepassFormatKey(v).RasterizationSamples}, 4u)
            << "the prepass is multisampled";
        EXPECT_TRUE(worldDepthReadOnly) << "with the grass's depth in the prepass, the world depth is read-only";
        f.Device->DestroyTexture(bladeTex);
    }
    f.Down();
}

// grass-on-atlas follow-up: reproduce the tiled-grass placement question in CI. Drives the REAL
// compact on the device with TILED-SHAPED params (a large WorldSize the tiled path emits) and reads
// back the GPU-written instance count. Distance fade is OFF (so the radial camera cull is skipped)
// and no splatmap is bound (SplatmapBindless == 0 -> the procedural grass mask, which the mask/
// dominance gate cannot reject) — so the ONLY thing under test is whether the compact's placement
// DOMAIN (grid -> world via WorldOrigin/WorldSize) yields instances at tiled scale. A nonzero count
// for both a tiled-scale (6144 m) and an untiled-scale (512 m) WorldSize proves the placement math
// is size-agnostic, so a runtime tiled-grass absence is NOT in the compact's placement domain (it is
// either the splatmap dominance gate on real tiled splat content, or the draw consuming the output).
TEST(RenderPipelineDeclareTests, GrassPlacementIsIndependentOfTerrainSize)
{
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        ASSERT_TRUE(terrain.Initialize(f.Device.get()));

        // The same camera pose for both terrain sizes, inside both footprints. Placement is
        // world-anchored and camera-relative, so the near field a camera sees must not depend on how
        // large the terrain under it happens to be — that is the whole claim of the rewrite.
        f.SetCameraLookingAt(Mathematics::Vector3(256.0f, 60.0f, 256.0f),
                             Mathematics::Vector3(256.0f, 40.0f, 456.0f));
        const ViewId v = f.AddView("GrassCountView", 1u);
        ASSERT_TRUE(f.Registry.Register(
            "TerrainGrass", [] { return std::make_unique<TerrainGrass::TerrainGrassRenderNode>(); },
            true));
        RenderPipelineBlueprint bp;
        bp.pipelineName = "GrassCountTest";
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "Grass";
            p.type = "TerrainGrass";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"Grass","type":"TerrainGrass"})";
            bp.passes.push_back(p);
        }
        bp.outputs.push_back({"FinalColor", "View.Resolve"});
        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);

        FramePools pools(f.Device.get());

        auto placedCountForWorldSize = [&](float worldSize, uint32 frameIdx) -> uint32
        {
            Terrain::TerrainGPUParams params{};
            params.WorldOriginX = 0.0f;
            params.WorldOriginZ = 0.0f;
            params.WorldSizeX = worldSize;
            params.WorldSizeZ = worldSize;
            params.HeightScale = 100.0f;
            params.GrassEnabled = 1u;       // bit0 only: procedural blades, no texture card
            params.GrassDensity = 1.0f;     // 1 blade/m2 at the camera
            params.GrassMaskThreshold = 0.02f;
            // SplatmapBindless stays 0 -> procedural grass (weight 1, dominant) -> the mask/dominance
            // gate cannot reject; Scale/BladeHeight/BladeWidth keep their active defaults.
            terrain.UploadTerrainParamsArray(&params, 1u, GameEngine::Terrain::kDefaultTerrainMaterials,
                                             GameEngine::Terrain::kTerrainLayerRoleCount, frameIdx);
            if (terrain.GetTerrainGrassActiveCount(frameIdx) == 0)
                return 0xFFFFFFFFu; // params not counted active -> not what this test drives

            RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
            frame.BeginFrame(frameIdx);
            RenderGraph::RGTexture color = frame.ImportPersistentTexture("GC.Color", ColorTargetDesc());
            RenderGraph::RGTexture depth = frame.ImportPersistentTexture("GC.Depth", DepthTargetDesc());
            f.Rs->BeginWorldDrawFrame();
            f.Rs->BuildWorldBatchKeys();
            const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{v, color, depth, {}}};
            const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                         f.Rs->Views().GetViews().end());
            instance.Declare(frame, targets, views);

            const RenderGraph::RGBuffer argsRG = GrassWorldReadArgs(*f.Rs, frame, v);
            if (!argsRG.IsValid())
                return 0xFFFFFFFEu; // the node declined to declare the compact

            // InstanceCount is the 2nd u32 of DrawIndexedIndirectCommand (offset 4).
            auto ticket = Rendering::RequestBufferReadbackRG(f.Device.get(), frame, argsRG,
                                                             /*srcOffset*/ 4u, /*byteCount*/ 4u,
                                                             "GrassInstanceCount");
            frame.MarkOutput(color);
            frame.Execute();
            Rendering::OnFrameSubmittedReadbacksRG(frame, frame.SubmissionToken());
            f.Device->WaitForIdle();

            Rendering::BufferReadbackResult res{};
            if (!ticket || !ticket->TryGet(res) || res.bytes.size() < 4)
                return 0xFFFFFFFDu; // readback never resolved
            uint32 count = 0;
            std::memcpy(&count, res.bytes.data(), sizeof(count));
            return count;
        };

        const uint32 tiledCount = placedCountForWorldSize(/*tiled scale*/ 6144.0f, 0u);
        const uint32 untiledCount = placedCountForWorldSize(/*untiled scale*/ 512.0f, 1u);

        EXPECT_GT(tiledCount, 0u) << "placed 0 for a tiled-scale WorldSize";
        EXPECT_GT(untiledCount, 0u) << "placed 0 for an untiled-scale WorldSize";
        // The falsifiable core, and the property the fixed candidate lattice could not have at any
        // parameter value: identical camera, identical settings, 12x the terrain -> the SAME near
        // field. This is read from LOD 0, whose whole disc sits inside both footprints, so the two
        // runs place the same cells from the same world-anchored lattice and must agree exactly.
        EXPECT_EQ(tiledCount, untiledCount)
            << "near-field placement changed with terrain size (tiled=" << tiledCount
            << " untiled=" << untiledCount << ")";
    }
    f.Down();
}

// Validate the real placement commands: the per-frame clear of what the compute accumulates, and
// the CPU-owned draw fields seeded once per ring slot. A deliberately invalid, unsubmitted scratch
// recording proves synchronization validation is active before a clean grass run can count as
// evidence. The readback then holds the seeded fields to their expected values across repeated
// frames on one buffer slot (they must survive frames that never rewrite them) and across a
// blade-mesh change (the slot must pick the new records up).
TEST(RenderPipelineDeclareTests, GrassPlacementIndirectSeedsHaveNoSynchronizationHazards)
{
    struct ValidationEnvironment
    {
        const char* Name;
        std::string Previous;
        ValidationEnvironment(const char* name, const char* value) : Name(name)
        {
            if (const char* prior = std::getenv(Name))
                Previous = prior;
            Set(value);
        }
        ~ValidationEnvironment() { Set(Previous.c_str()); }
        void Set(const char* value)
        {
#if defined(_WIN32)
            _putenv_s(Name, value);
#else
            if (*value)
                setenv(Name, value, 1);
            else
                unsetenv(Name);
#endif
        }
    } syncEnvironment("GE_VK_SYNC_VALIDATION", "1"),
      noValidationAssert("GE_VK_VALIDATION_ASSERT", "0");

    TerrainFixture f;
    if (!f.Up(/*enableDebugLayer=*/true))
        GTEST_SKIP() << "No Vulkan device available";
    if (!f.Device->GetValidationStats().Enabled)
    {
        f.Down();
        GTEST_SKIP() << "Vulkan validation layer unavailable";
    }

    BufferDesc scratchDesc{};
    scratchDesc.size = 16;
    scratchDesc.usage = static_cast<uint32>(BufferUsage::TransferDst);
    scratchDesc.debugName = "GrassSyncValidation.PositiveControl";
    const BufferHandle scratch = f.Device->CreateBuffer(scratchDesc);
    ASSERT_TRUE(scratch.IsValid());
    f.Device->ResetValidationStats();
    {
        auto cmd = f.Device->CreateCommandList(IDevice::QueueType::Graphics);
        ASSERT_TRUE(cmd);
        cmd->Begin();
        cmd->FillBuffer(scratch, 0, scratchDesc.size, 0u);
        cmd->FillBuffer(scratch, 0, sizeof(uint32), 0u);
        cmd->End();
        // Do not submit: only the layer's command-recording hazard check is needed.
    }
    f.Device->DestroyBuffer(scratch);
    bool controlDetected = false;
    for (const auto& entry : f.Device->GetValidationStats().Vuids)
        if (entry.Vuid == "SYNC-HAZARD-WRITE-AFTER-WRITE")
            controlDetected = true;
    if (!controlDetected)
    {
        f.Down();
        GTEST_SKIP() << "Synchronization validation did not detect the unsubmitted positive control";
    }
    f.Device->ResetValidationStats();

    {
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        ASSERT_TRUE(terrain.Initialize(f.Device.get()));
        f.SetCameraLookingAt(Mathematics::Vector3(256.0f, 60.0f, 256.0f),
                             Mathematics::Vector3(256.0f, 40.0f, 456.0f));
        const ViewId view = f.AddView("GrassSyncView", 1u);
        ASSERT_TRUE(f.Registry.Register(
            "TerrainGrass", [] { return std::make_unique<TerrainGrass::TerrainGrassRenderNode>(); },
            true));
        RenderPipelineBlueprint bp;
        bp.pipelineName = "GrassSyncTest";
        RenderPipelineBlueprint::Pass pass;
        pass.id = "Grass";
        pass.type = "TerrainGrass";
        pass.enabled = true;
        pass.perView = true;
        pass.passJson = R"({"id":"Grass","type":"TerrainGrass"})";
        bp.passes.push_back(pass);
        bp.outputs.push_back({"FinalColor", "View.Resolve"});
        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);
        FramePools pools(f.Device.get());

        // One frame of real placement, read back and held to what the CPU is supposed to own in the
        // block. bladeSegments is the authored field that moves the per-LOD sub-mesh records, so it
        // is what drives a re-seed below.
        auto runFrame = [&](uint32 frameIndex, uint32 bladeSegments)
        {
            Terrain::TerrainGPUParams params{};
            params.WorldSizeX = 512.0f;
            params.WorldSizeZ = 512.0f;
            params.HeightScale = 100.0f;
            params.GrassEnabled = 1u;
            params.GrassDensity = 1.0f;
            params.GrassMaskThreshold = 0.02f;
            params.GrassBladeSegments = bladeSegments;
            terrain.UploadTerrainParamsArray(&params, 1u, Terrain::kDefaultTerrainMaterials,
                                             Terrain::kTerrainLayerRoleCount, frameIndex);
            ASSERT_GT(terrain.GetTerrainGrassActiveCount(frameIndex), 0u);

            RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
            frame.BeginFrame(frameIndex);
            const auto color = frame.ImportPersistentTexture("GS.Color", ColorTargetDesc());
            const auto depth = frame.ImportPersistentTexture("GS.Depth", DepthTargetDesc());
            f.Rs->BeginWorldDrawFrame();
            f.Rs->BuildWorldBatchKeys();
            const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{view, color, depth, {}}};
            const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                         f.Rs->Views().GetViews().end());
            instance.Declare(frame, targets, views);
            const auto args = GrassWorldReadArgs(*f.Rs, frame, view);
            ASSERT_TRUE(args.IsValid());
            auto ticket = Rendering::RequestBufferReadbackRG(
                f.Device.get(), frame, args, 0u, sizeof(TerrainGrass::GrassIndirectBlockGPU),
                "GrassSyncArgs");
            ASSERT_TRUE(ticket);
            frame.MarkOutput(color);
            frame.Execute();
            Rendering::OnFrameSubmittedReadbacksRG(frame, frame.SubmissionToken());
            f.Device->WaitForIdle();

            Rendering::BufferReadbackResult result{};
            ASSERT_TRUE(ticket->TryGet(result));
            ASSERT_EQ(result.bytes.size(), sizeof(TerrainGrass::GrassIndirectBlockGPU));
            TerrainGrass::GrassIndirectBlockGPU block{};
            std::memcpy(&block, result.bytes.data(), sizeof(block));
            uint32 placed = 0;
            for (const auto& draw : block.Draws)
                placed += draw.InstanceCount;
            EXPECT_GT(placed, 0u) << "The placement compute must run for the hazard test to cover it";
            EXPECT_LE(placed, block.Capacity);

            // The CPU-owned half of the block. The per-frame commands touch only the instance counts
            // and the counters, so these must read back as the blade mesh and the fitted plan say —
            // on the frame that seeded the slot, and on every later frame that reuses it.
            auto* grass = f.Rs->GetFeature<TerrainGrass::TerrainGrassRenderFeature>();
            ASSERT_NE(grass, nullptr);
            // The feature keys its buffer ring on the DEVICE frame index, not on the render graph's,
            // so that is what indexes its per-slot state from out here too.
            const uint32 ringSlot = f.Device->GetFrameIndex();
            for (uint32 lod = 0; lod < TerrainGrass::kGrassLodCount; ++lod)
            {
                const auto& lodMesh = grass->GetBladeLod(lod);
                // Without a built mesh both sides would be zero and the comparison would pass
                // while proving nothing.
                ASSERT_GT(lodMesh.IndexCount, 0u) << "frame " << frameIndex << " lod " << lod;
                EXPECT_EQ(block.Draws[lod].IndexCount, lodMesh.IndexCount)
                    << "frame " << frameIndex << " lod " << lod;
                EXPECT_EQ(block.Draws[lod].FirstIndex, lodMesh.FirstIndex)
                    << "frame " << frameIndex << " lod " << lod;
                EXPECT_EQ(block.Draws[lod].VertexOffset, lodMesh.VertexOffset)
                    << "frame " << frameIndex << " lod " << lod;
            }
            // LOD 0 fills the shared pool from the bottom, so its record always starts at 0; LOD 1's
            // is the finalize compute's output and is checked through the placed total above.
            EXPECT_EQ(block.Draws[0].FirstInstance, 0u) << "frame " << frameIndex;
            EXPECT_EQ(block.Capacity, grass->GetPlan(view, ringSlot).Capacity)
                << "frame " << frameIndex;
        };

        // The device frame index does not advance here — nothing presents a swapchain — so all of
        // these frames land on ONE slot of the feature's ring rather than walking it. That is the
        // case the change has to survive: the first frame seeds the slot and the four after it must
        // leave every seeded field exactly as they found it while only the counts are cleared.
        constexpr uint32 kBaseBladeSegments = 5u;
        for (uint32 frameIndex = 0; frameIndex < 5; ++frameIndex)
            runFrame(frameIndex, kBaseBladeSegments);

        // A different blade count rebuilds the shared blade mesh, moving every LOD's sub-mesh
        // record. The slot must re-seed against the new mesh rather than keep the records it was
        // created with, and the frames after the re-seed must then leave those alone in turn.
        constexpr uint32 kReseedBladeSegments = 9u;
        for (uint32 frameIndex = 5; frameIndex < 9; ++frameIndex)
            runFrame(frameIndex, kReseedBladeSegments);
        const auto validation = f.Device->GetValidationStats();
        EXPECT_EQ(validation.OverflowCount, 0u);
        for (const auto& entry : validation.Vuids)
            EXPECT_NE(entry.Vuid.find("SYNC-HAZARD"), 0u)
                << entry.Vuid << ": " << entry.FirstMessage;
    }
    f.Down();
}

// grass-on-atlas follow-up (coordinator step 1): drive the compact with the REAL production-baked
// tiled splat, bound to the terrain params, and read back the instance count. If the compact places
// a nonzero count with the actual grass-dominant procedural splat, the compact's sampling + mask
// gate are correct on tiled content, so a runtime tiled-grass absence is upstream (the tiled params
// not carrying the right bindless index / height) — not in the compact.
namespace
{
// A realistic splat for a heightfield: the unbaked (all-zero) base with the SHIPPED
// DEFAULT rows composited over it, through the same evaluator and compositor
// ApplySplatModifiers walks. This is what a terrain actually carries, so a test
// wanting "real baked splat content" wants this — an all-zero splat would place
// grass uniformly (the surface resolves zero to channel 0) and prove nothing about
// placement following material.
std::vector<uint8> BakeDefaultRulesSplat(const Terrain::HeightfieldData& hf, float32 worldSize,
                                         float32 heightScale, uint32& outW, uint32& outH)
{
    std::vector<uint8> splat;
    TerrainECS::ResetSplatmap(hf, splat, outW, outH);
    const uint32 dim = hf.GetWidth();
    const float32 spacing = worldSize / static_cast<float32>(dim - 1);
    float32 minH = 0.0f, maxH = 0.0f;
    hf.GetMinMax(0, 0, static_cast<int32>(dim), static_cast<int32>(dim), minH, maxH);
    const float32 range = std::max(maxH - minH, 1e-3f);
    const auto rules = TerrainECS::MakeDefaultTerrainSurfaceRules();
    for (uint32 z = 0; z < dim; ++z)
        for (uint32 x = 0; x < dim; ++x)
        {
            const auto n = hf.ComputeNormal(static_cast<int32>(x), static_cast<int32>(z),
                                            spacing, spacing);
            const TerrainECS::TerrainRuleSample sample = TerrainECS::MakeTerrainRuleSample(
                n.x, n.y, n.z, hf.GetSample(x, z), heightScale, minH, range,
                static_cast<float32>(x) * spacing, static_cast<float32>(z) * spacing);
            uint8* pixel = &splat[(static_cast<size_t>(z) * dim + x) * 4];
            for (uint32 r = 0; r < rules.RuleCount; ++r)
            {
                const float32 w = TerrainECS::EvaluateTerrainSurfaceRuleWeight(
                    rules.Rules[r], sample, TerrainECS::SurfaceRuleNoiseSample);
                if (w > 0.0f)
                    TerrainECS::CompositeSplatTexel(pixel, rules.Rules[r].MaterialSlot, w,
                                                    rules.Rules[r].Replace);
            }
        }
    return splat;
}
} // namespace

TEST(RenderPipelineDeclareTests, GrassPlacesWithRealBakedTiledSplat)
{
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        ASSERT_TRUE(terrain.Initialize(f.Device.get()));
        if (!f.Rs->Textures().IsBindlessEnabled())
            GTEST_SKIP() << "bindless disabled on this device";

        f.SetCameraLookingAt(Mathematics::Vector3(768.0f, 80.0f, 768.0f),
                             Mathematics::Vector3(768.0f, 50.0f, 968.0f));
        const ViewId v = f.AddView("GrassRealSplatView", 1u);
        ASSERT_TRUE(f.Registry.Register(
            "TerrainGrass", [] { return std::make_unique<TerrainGrass::TerrainGrassRenderNode>(); },
            true));

        // Bake the shipped DEFAULT RULES + a normalized height for a 1536 m / HeightScale-60 terrain.
        const uint32 dim = 65;
        Terrain::HeightfieldData hf(dim, dim, 0.0f);
        hf.FillWithNoise(2.5f, 60.0f, 4, 7);
        uint32 sw = 0, sh = 0;
        std::vector<uint8> splat = BakeDefaultRulesSplat(hf, 1536.0f, 60.0f, sw, sh);
        float32 minH = 0, maxH = 0;
        hf.GetMinMax(0, 0, static_cast<int32>(dim), static_cast<int32>(dim), minH, maxH);
        const float32 range = std::max(maxH - minH, 1e-3f);
        std::vector<float32> normHeight(static_cast<size_t>(dim) * dim);
        for (uint32 i = 0; i < dim * dim; ++i)
            normHeight[i] = (hf.GetRawSamples()[i] - minH) / range;

        const TerrainECS::TerrainHandle h{7u, 1u};
        terrain.UploadSplatmap(h, splat.data(), dim, dim);
        terrain.UploadHeightmap(h, normHeight.data(), dim, dim);

        RenderPipelineBlueprint bp;
        bp.pipelineName = "GrassRealSplatTest";
        {
            // TerrainUpload is the only caller of FlushPendingUploads — the pass that both clears
            // the fresh splat out of UNDEFINED and copies the baked texels in. Without it the
            // bindless slot resolves to an image the GPU never wrote, every grass weight reads
            // whatever that memory holds, and the mask gate rejects every candidate.
            RenderPipelineBlueprint::Pass up;
            up.id = "Upload"; up.type = "TerrainUpload"; up.enabled = true; up.perView = true;
            up.passJson = R"({"id":"Upload","type":"TerrainUpload"})";
            bp.passes.push_back(up);
        }
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "Grass"; p.type = "TerrainGrass"; p.enabled = true; p.perView = true;
            p.passJson = R"({"id":"Grass","type":"TerrainGrass"})";
            bp.passes.push_back(p);
        }
        bp.outputs.push_back({"FinalColor", "View.Resolve"});
        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);

        FramePools pools(f.Device.get());

        // Frame 0 records the upload copy; the compact samples via bindless, so give the copy a
        // full frame + idle before frame 1's compact reads the splat.
        Terrain::TerrainGPUParams params{};
        params.WorldOriginX = 0.0f; params.WorldOriginZ = 0.0f;
        params.WorldSizeX = 1536.0f; params.WorldSizeZ = 1536.0f;
        params.HeightScale = 60.0f; params.WorldOriginY = 0.0f;
        params.GrassEnabled = 1u;        // bit0 only: procedural blades, no texture card
        params.GrassMaskThreshold = 0.02f;
        params.GrassDensity = 1.0f;      // 1 blade/m2 at the camera

        uint32 placed = 0;
        uint32 splatIdx = 0;
        TerrainGrass::GrassIndirectBlockGPU block{};
        for (uint32 frameIdx = 0; frameIdx < 2; ++frameIdx)
        {
            // A bindless slot is withheld until the texture's first whole-texture copy has been
            // recorded, so registration is retried every frame exactly as the extraction system
            // does it: frame 0 flushes, frame 1 publishes and samples.
            terrain.RegisterSplatmapBindless(h, *f.Rs);
            terrain.RegisterHeightmapBindless(h, *f.Rs);
            splatIdx = terrain.GetSplatmapBindlessIndex(h);
            params.SplatmapBindless = splatIdx;
            params.HeightmapBindless = terrain.GetHeightmapBindlessIndex(h);
            terrain.UploadTerrainParamsArray(&params, 1u, GameEngine::Terrain::kDefaultTerrainMaterials,
                                             GameEngine::Terrain::kTerrainLayerRoleCount, frameIdx);
            RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
            frame.BeginFrame(frameIdx);
            RenderGraph::RGTexture color = frame.ImportPersistentTexture("GR2.Color", ColorTargetDesc());
            RenderGraph::RGTexture depth = frame.ImportPersistentTexture("GR2.Depth", DepthTargetDesc());
            f.Rs->BeginWorldDrawFrame();
            f.Rs->BuildWorldBatchKeys();
            const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{v, color, depth, {}}};
            const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                         f.Rs->Views().GetViews().end());
            instance.Declare(frame, targets, views);

            std::shared_ptr<Rendering::RGBufferReadbackTicket> ticket;
            const RenderGraph::RGBuffer argsRG = GrassWorldReadArgs(*f.Rs, frame, v);
            if (frameIdx == 1 && argsRG.IsValid())
                ticket = Rendering::RequestBufferReadbackRG(f.Device.get(), frame, argsRG, 0u,
                                                            sizeof(TerrainGrass::GrassIndirectBlockGPU),
                                                            "GrassCount2");

            frame.MarkOutput(color);
            frame.Execute();
            Rendering::OnFrameSubmittedReadbacksRG(frame, frame.SubmissionToken());
            f.Device->WaitForIdle();

            if (ticket)
            {
                Rendering::BufferReadbackResult res{};
                if (ticket->TryGet(res) &&
                    res.bytes.size() >= sizeof(TerrainGrass::GrassIndirectBlockGPU))
                {
                    std::memcpy(&block, res.bytes.data(), sizeof(block));
                    for (const auto& draw : block.Draws)
                        placed += draw.InstanceCount;
                }
            }
        }

        ASSERT_NE(splatIdx, 0u) << "splat bindless registration failed";

        // The falsifiable core: the compact MUST place grass with the real grass-dominant procedural
        // splat bound. Summed over both LOD records — this camera frames the ground beyond the LOD 0
        // disc, so a LOD-0-only count reads zero however much was placed. The counters separate "the
        // mask refused every candidate" from "no cell was ever visible to consider".
        EXPECT_GT(placed, 0u)
            << "compact placed 0 with the real production-baked tiled splat — sampling/gate bug"
            << " (lod0=" << block.Draws[0].InstanceCount << " lod1=" << block.Draws[1].InstanceCount
            << " considered=" << block.CandidatesConsidered
            << " cellsVisible=" << block.CellsVisible
            << " accepted=" << block.AcceptedBlades << ")";
        // The two LOD ranges share one pool: LOD 0 owns [0, count0) and LOD 1 owns
        // [capacity - count1, capacity), both written by the plan dispatch. A wrong first instance
        // would draw LOD 1 over LOD 0's slots and read as duplicated blades.
        EXPECT_GT(block.Capacity, 0u) << "the CPU did not stamp the pool capacity into the args block";
        EXPECT_EQ(block.Draws[0].FirstInstance, 0u);
        EXPECT_EQ(block.Draws[1].FirstInstance, block.Capacity - block.Draws[1].InstanceCount);
        EXPECT_LE(block.Draws[0].InstanceCount + block.Draws[1].InstanceCount, block.Capacity);
        EXPECT_EQ(block.CandidatesConsidered,
                  block.Draws[0].InstanceCount + block.Draws[1].InstanceCount)
            << "every reserved slot is accounted for exactly once";
        // The pool holds candidate slots; a rejected candidate keeps its own slot as a zero-size
        // blade, so accepted can never exceed what was reserved.
        EXPECT_LE(block.AcceptedBlades, block.CandidatesConsidered);
    }
    f.Down();
}

// Payload-honest root oracle (#508 lesson): a nonzero placed count says nothing about WHERE the
// blades sit. Read back the first N blade transforms and assert each root is ON the terrain surface —
// XZ inside the terrain domain and RootY within tolerance of the CPU-sampled height at that XZ. This
// pins the "blades placed but rendered at wrong world positions / floating arcs" class the reviewer
// saw, which a count-only oracle cannot catch.
TEST(RenderPipelineDeclareTests, GrassRootsSitOnTerrainSurface)
{
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        ASSERT_TRUE(terrain.Initialize(f.Device.get()));
        if (!f.Rs->Textures().IsBindlessEnabled())
            GTEST_SKIP() << "bindless disabled on this device";

        // Steeply down, so the cells directly under the camera are both inside the frustum and
        // inside the LOD 0 radius — the readback reads LOD 0's slice of the instance buffer.
        f.SetCameraLookingAt(Mathematics::Vector3(768.0f, 200.0f, 600.0f),
                             Mathematics::Vector3(768.0f, 0.0f, 900.0f));
        const ViewId v = f.AddView("GrassRootView", 1u);
        ASSERT_TRUE(f.Registry.Register(
            "TerrainGrass", [] { return std::make_unique<TerrainGrass::TerrainGrassRenderNode>(); },
            true));

        const uint32 dim = 65;
        Terrain::HeightfieldData hf(dim, dim, 0.0f);
        hf.FillWithNoise(2.5f, 60.0f, 4, 7);
        uint32 sw = 0, sh = 0;
        std::vector<uint8> splat = BakeDefaultRulesSplat(hf, 1536.0f, 60.0f, sw, sh);
        float32 minH = 0, maxH = 0;
        hf.GetMinMax(0, 0, static_cast<int32>(dim), static_cast<int32>(dim), minH, maxH);
        const float32 range = std::max(maxH - minH, 1e-3f);
        std::vector<float32> normHeight(static_cast<size_t>(dim) * dim);
        for (uint32 i = 0; i < dim * dim; ++i)
            normHeight[i] = (hf.GetRawSamples()[i] - minH) / range;

        const TerrainECS::TerrainHandle h{7u, 1u};
        terrain.UploadSplatmap(h, splat.data(), dim, dim);
        terrain.UploadHeightmap(h, normHeight.data(), dim, dim);

        RenderPipelineBlueprint bp;
        bp.pipelineName = "GrassRootTest";
        {
            // The upload pass is what pushes the heightmap and splatmap to the GPU, and until it
            // does the feature withholds their bindless slots. Without it the params name the
            // sentinel, every height tap reads 0, and every root collapses onto WorldOriginY. The
            // atlas sibling declares it for the same reason.
            RenderPipelineBlueprint::Pass up;
            up.id = "Upload"; up.type = "TerrainUpload"; up.enabled = true; up.perView = true;
            up.passJson = R"({"id":"Upload","type":"TerrainUpload"})";
            bp.passes.push_back(up);
        }
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "Grass"; p.type = "TerrainGrass"; p.enabled = true; p.perView = true;
            p.passJson = R"({"id":"Grass","type":"TerrainGrass"})";
            bp.passes.push_back(p);
        }
        bp.outputs.push_back({"FinalColor", "View.Resolve"});
        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);
        FramePools pools(f.Device.get());

        const float32 kWorldSize = 1536.0f;
        const float32 kHeightScale = 60.0f;
        const float32 kWorldOriginY = 5.0f; // nonzero so a RootY==0 bug can't accidentally pass
        Terrain::TerrainGPUParams params{};
        params.WorldOriginX = 0.0f; params.WorldOriginZ = 0.0f;
        params.WorldSizeX = kWorldSize; params.WorldSizeZ = kWorldSize;
        params.HeightScale = kHeightScale; params.WorldOriginY = kWorldOriginY;
        params.GrassEnabled = 1u;
        params.GrassMaskThreshold = 0.02f;
        params.GrassDensity = 1.0f;      // 1 blade/m2 at the camera
        // A long range widens the LOD 0 disc (it starts at 0.18 * range), so the readback — which
        // reads LOD 0's slice of the instance buffer — sees a wide swath of ground rather than the
        // few cells directly under the camera, whose splat may legitimately carry no grass.
        params.GrassRange = 2000.0f;

        const uint32 kSampleCount = 8u;
        uint32 placed = 0;
        uint32 splatIdx = 0;
        uint32 heightIdx = 0;
        std::vector<float32> roots; // flat WorldX,WorldZ,RootY per blade
        for (uint32 frameIdx = 0; frameIdx < 2; ++frameIdx)
        {
            // A bindless slot is withheld until the texture's first whole-texture copy has been
            // recorded by the upload pass, so registration is retried every frame exactly as the
            // extraction system does it: frame 0 flushes, frame 1 publishes and samples.
            terrain.RegisterSplatmapBindless(h, *f.Rs);
            terrain.RegisterHeightmapBindless(h, *f.Rs);
            splatIdx = terrain.GetSplatmapBindlessIndex(h);
            heightIdx = terrain.GetHeightmapBindlessIndex(h);
            params.SplatmapBindless = splatIdx;
            params.HeightmapBindless = heightIdx;
            terrain.UploadTerrainParamsArray(&params, 1u, GameEngine::Terrain::kDefaultTerrainMaterials,
                                             GameEngine::Terrain::kTerrainLayerRoleCount, frameIdx);
            RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
            frame.BeginFrame(frameIdx);
            RenderGraph::RGTexture color = frame.ImportPersistentTexture("GRoot.Color", ColorTargetDesc());
            RenderGraph::RGTexture depth = frame.ImportPersistentTexture("GRoot.Depth", DepthTargetDesc());
            f.Rs->BeginWorldDrawFrame();
            f.Rs->BuildWorldBatchKeys();
            const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{v, color, depth, {}}};
            const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                         f.Rs->Views().GetViews().end());
            instance.Declare(frame, targets, views);

            std::shared_ptr<Rendering::RGBufferReadbackTicket> countT, instT;
            const RenderGraph::RGBuffer argsRG = GrassWorldReadArgs(*f.Rs, frame, v);
            const RenderGraph::RGBuffer instRG = GrassInstancesInFrame(*f.Rs, frame, v);
            if (frameIdx == 1 && argsRG.IsValid())
                countT = Rendering::RequestBufferReadbackRG(f.Device.get(), frame, argsRG, 4u, 4u, "GRootCount");
            if (frameIdx == 1 && instRG.IsValid())
                instT = Rendering::RequestBufferReadbackRG(f.Device.get(), frame, instRG, 0u,
                                                           48u * kSampleCount, "GRootInst");
            frame.MarkOutput(color);
            frame.Execute();
            Rendering::OnFrameSubmittedReadbacksRG(frame, frame.SubmissionToken());
            f.Device->WaitForIdle();

            if (countT)
            {
                Rendering::BufferReadbackResult res{};
                if (countT->TryGet(res) && res.bytes.size() >= 4)
                    std::memcpy(&placed, res.bytes.data(), sizeof(placed));
            }
            if (instT)
            {
                Rendering::BufferReadbackResult res{};
                if (instT->TryGet(res) && res.bytes.size() >= 48u * kSampleCount)
                {
                    const float32* fp = reinterpret_cast<const float32*>(res.bytes.data());
                    for (uint32 i = 0; i < kSampleCount; ++i)
                    {
                        roots.push_back(fp[i * 12 + 0]); // WorldX
                        roots.push_back(fp[i * 12 + 1]); // WorldZ
                        roots.push_back(fp[i * 12 + 2]); // RootY
                    }
                }
            }
        }

        ASSERT_NE(splatIdx, 0u) << "splat bindless registration failed";
        // The oracle compares RootY against the sampled heightfield, so an unpublished heightmap
        // slot would make every root land on WorldOriginY and read as a placement bug.
        ASSERT_NE(heightIdx, 0u) << "heightmap bindless registration failed";
        ASSERT_GT(placed, 0u) << "compact placed 0 — cannot check roots";
        ASSERT_FALSE(roots.empty()) << "instance readback failed";

        const uint32 checkN = std::min<uint32>(kSampleCount, placed);
        const float32 tol = kHeightScale * 0.15f; // ~9 m: catches buried/floating, absorbs float-precision diffs
        // Sample the uploaded normalized heightmap the way the GPU does — texel-centered bilinear with
        // clamp-to-edge — not the heightfield's grid-convention SampleBilinear. The compact appends placed
        // blades in a nondeterministic order, so the read-back sample can land entirely against the domain
        // edge; there the two conventions diverge by more than a texel and the GPU (which snaps to the edge
        // texel, the true surface the terrain also renders) is correct. Comparing against the same sampling
        // removes that false negative.
        auto sampleHeightTex = [&](float32 u, float32 vv) -> float32
        {
            const float32 fx = std::clamp(u, 0.0f, 1.0f) * static_cast<float32>(dim) - 0.5f;
            const float32 fy = std::clamp(vv, 0.0f, 1.0f) * static_cast<float32>(dim) - 0.5f;
            const int32 x0 = static_cast<int32>(std::floor(fx));
            const int32 y0 = static_cast<int32>(std::floor(fy));
            const float32 tx = fx - static_cast<float32>(x0);
            const float32 ty = fy - static_cast<float32>(y0);
            auto cl = [&](int32 i) { return std::clamp(i, 0, static_cast<int32>(dim) - 1); };
            const float32 h00 = normHeight[static_cast<size_t>(cl(y0)) * dim + cl(x0)];
            const float32 h10 = normHeight[static_cast<size_t>(cl(y0)) * dim + cl(x0 + 1)];
            const float32 h01 = normHeight[static_cast<size_t>(cl(y0 + 1)) * dim + cl(x0)];
            const float32 h11 = normHeight[static_cast<size_t>(cl(y0 + 1)) * dim + cl(x0 + 1)];
            return std::lerp(std::lerp(h00, h10, tx), std::lerp(h01, h11, tx), ty);
        };
        uint32 checked = 0;
        constexpr float32 kEdgeMargin = 0.03f; // one+ texel of a 65-sample field
        for (uint32 i = 0; i < checkN; ++i)
        {
            const float32 wx = roots[i * 3 + 0];
            const float32 wz = roots[i * 3 + 1];
            const float32 rootY = roots[i * 3 + 2];
            const float32 u = wx / kWorldSize;
            const float32 vv = wz / kWorldSize;
            // XZ must land inside the terrain domain — arcs spilling outside are a placement-domain bug.
            EXPECT_GE(u, -0.001f); EXPECT_LE(u, 1.001f);
            EXPECT_GE(vv, -0.001f); EXPECT_LE(vv, 1.001f);
            // Every blade must sit within the terrain height band. This is the append-order-robust check:
            // the compact appends placed blades nondeterministically, so the read-back sample can be any
            // region, but a root parked at altitude (the collapse this oracle guards) always falls outside
            // [WorldOriginY, WorldOriginY + HeightScale].
            EXPECT_GE(rootY, kWorldOriginY - tol) << "blade " << i << " sank below the terrain: RootY=" << rootY;
            EXPECT_LE(rootY, kWorldOriginY + kHeightScale + tol)
                << "blade " << i << " floats above the terrain: RootY=" << rootY << " at uv(" << u << "," << vv << ")";
            ++checked;
            // Interior blades additionally match the sampled surface tightly. The extreme edge strip samples
            // the heightmap's clamp/border height, which the grid-space expected cannot reproduce, so blades
            // within a texel of the border are band-checked only.
            if (u < kEdgeMargin || u > 1.0f - kEdgeMargin || vv < kEdgeMargin || vv > 1.0f - kEdgeMargin)
                continue;
            const float32 expectedRootY = kWorldOriginY + sampleHeightTex(u, vv) * kHeightScale;
            EXPECT_NEAR(rootY, expectedRootY, tol)
                << "blade " << i << " root off the surface: RootY=" << rootY
                << " expected~" << expectedRootY << " at uv(" << u << "," << vv << ")";
        }
        EXPECT_GT(checked, 0u);
    }
    f.Down();
}

namespace
{
// Deterministic per-GLOBAL-sample height (mirrors TerrainAtlasTests::WorldHeight) so adjacent tiles
// share boundary samples exactly and the CPU sampler matches the packed atlas the GPU reads.
float32 AtlasOracleWorldHeight(int32 gx, int32 gz)
{
    return 0.5f + 0.25f * std::sin(gx * 0.31f) * std::cos(gz * 0.27f);
}
std::vector<float32> AtlasOracleFullTile(uint32 tileRes, int32 tx, int32 tz)
{
    std::vector<float32> h(static_cast<size_t>(tileRes) * tileRes);
    const int32 interior = static_cast<int32>(tileRes) - 1;
    for (uint32 z = 0; z < tileRes; ++z)
        for (uint32 x = 0; x < tileRes; ++x)
            h[static_cast<size_t>(z) * tileRes + x] =
                AtlasOracleWorldHeight(tx * interior + static_cast<int32>(x), tz * interior + static_cast<int32>(z));
    return h;
}
struct AtlasOracleBuilt
{
    TerrainECS::AtlasGeometry Geo;
    std::vector<float32> Atlas;
    TerrainECS::AtlasIndirectionTable Table;
};
// A fully-resident 2x2 Full-tile atlas (every tile has a valid slot => CBT_AtlasResolve.Resident == true).
AtlasOracleBuilt BuildAtlasOracle2x2(uint32 tileRes)
{
    AtlasOracleBuilt b;
    b.Geo = TerrainECS::MakeAtlasGeometry(tileRes, /*slots*/ 4u, /*tilesX*/ 2u, /*tilesZ*/ 2u);
    b.Atlas.assign(static_cast<size_t>(b.Geo.AtlasDim) * b.Geo.AtlasDim, 0.0f);
    b.Table.Resize(b.Geo);
    uint32 slot = 0;
    for (int32 tz = 0; tz < 2; ++tz)
        for (int32 tx = 0; tx < 2; ++tx)
        {
            const std::vector<float32> heights = AtlasOracleFullTile(tileRes, tx, tz);
            TerrainECS::PackTileHeightIntoSlot(b.Atlas.data(), b.Geo, slot, heights.data(),
                                               /*tileIsFull*/ true, TerrainECS::AtlasTileNeighbors{},
                                               TerrainECS::AtlasNeighborEdges{});
            TerrainECS::TileAtlasSlot& row = b.Table.Row(tx, tz);
            row.Slot = slot;
            row.Generation = 1;
            row.LodBias = TerrainECS::kAtlasLodBiasFull;
            ++slot;
        }
    return b;
}
} // namespace

// Hypothesis A (independent of the drain-order fix): a placed atlas blade's baked RootY must sit on the
// resident-window atlas surface, not collapse to WorldOriginY. The compact bakes
// rootY = WorldOriginY + sampleAtlasHeight01(uv) * HeightScale; if the atlas resolve/rows binding is
// wrong the sample returns 0 and every blade flattens to WorldOriginY. This drives the real atlas grass
// path on device (2x2 fully-resident atlas, TerrainUpload flush) and checks the readback roots against
// the CPU AtlasHeightSampler mirror. WorldOriginY is nonzero so a collapse cannot pass by coincidence.
TEST(RenderPipelineDeclareTests, GrassAtlasRootsSitOnTerrainSurface)
{
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        ASSERT_TRUE(terrain.Initialize(f.Device.get()));
        if (!f.Rs->Textures().IsBindlessEnabled())
            GTEST_SKIP() << "bindless disabled on this device";

        f.SetCameraLookingAt(Mathematics::Vector3(1024.0f, 300.0f, 1024.0f),
                             Mathematics::Vector3(1024.0f, 260.0f, 1224.0f));
        const ViewId v = f.AddView("GrassAtlasRootView", 1u);
        ASSERT_TRUE(f.Registry.Register(
            "TerrainGrass", [] { return std::make_unique<TerrainGrass::TerrainGrassRenderNode>(); }, true));
        // TerrainFixture::Up() already registers the "TerrainUpload" node factory; the blueprint's
        // Upload pass (which flushes the atlas region upload) resolves through it.

        const uint32 tileRes = 33u;
        const AtlasOracleBuilt atlas = BuildAtlasOracle2x2(tileRes);
        const TerrainECS::TerrainHandle h{7u, 1u};
        terrain.EnsureAtlasHeightTexture(h, atlas.Geo.AtlasDim);
        // The whole atlas uploads as one square region (dst 0,0, stride = AtlasDim).
        terrain.UploadAtlasHeightSlot(h, atlas.Atlas.data(), atlas.Geo.AtlasDim, 0u, 0u);
        // No pass has declared yet in this test, so the gate cannot know a flush is coming and the
        // atlas slot is withheld on the first registration (the general rule is the one
        // TerrainBindlessSlotWaitsForTheUploadFlush pins: a live pass publishes on the creation
        // tick). Registration and the info it feeds are therefore refreshed every frame exactly as
        // the extraction system does it: frame 0 flushes, frame 1 publishes and samples.
        uint32 heightIdx = 0;
        auto publishAtlasTerrain = [&] {
            terrain.RegisterAtlasSurfaceBindless(h, *f.Rs);
            heightIdx = terrain.GetAtlasHeightBindlessIndex(h);
            // Feed the grass feature the atlas source it reads via TryGetAtlasGrassSource.
            TerrainECS::TerrainInstanceInfo info{};
            info.Handle = h;
            info.AtlasBacked = true;
            info.AtlasDim = atlas.Geo.AtlasDim;
            info.AtlasSlotStride = atlas.Geo.SlotStride;
            info.AtlasSlotsPerRow = atlas.Geo.SlotsPerRow;
            info.AtlasTileRes = tileRes;
            info.AtlasTilesPerAxisX = 2u;
            info.AtlasTilesPerAxisZ = 2u;
            info.AtlasHeightBindlessIndex = heightIdx;
            info.AtlasTableVersion = 1u;
            info.AtlasRowCount = static_cast<uint32>(atlas.Table.Rows.size());
            info.AtlasRowBytes.resize(atlas.Table.Rows.size() * sizeof(TerrainECS::TileAtlasSlot));
            std::memcpy(info.AtlasRowBytes.data(), atlas.Table.Rows.data(), info.AtlasRowBytes.size());
            std::vector<TerrainECS::TerrainInstanceInfo> infos;
            infos.push_back(std::move(info));
            terrain.SetActiveTerrains(std::move(infos));
        };

        RenderPipelineBlueprint bp;
        bp.pipelineName = "GrassAtlasRootTest";
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "Upload"; p.type = "TerrainUpload"; p.enabled = true; p.perView = true;
            p.passJson = R"({"id":"Upload","type":"TerrainUpload"})";
            bp.passes.push_back(p);
        }
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "Grass"; p.type = "TerrainGrass"; p.enabled = true; p.perView = true;
            p.passJson = R"({"id":"Grass","type":"TerrainGrass"})";
            bp.passes.push_back(p);
        }
        bp.outputs.push_back({"FinalColor", "View.Resolve"});
        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);
        FramePools pools(f.Device.get());

        const float32 kWorldSize = 2048.0f;
        const float32 kHeightScale = 256.0f;
        const float32 kWorldOriginY = 5.0f; // nonzero so a RootY==WorldOriginY collapse cannot pass
        Terrain::TerrainGPUParams params{};
        params.WorldOriginX = 0.0f; params.WorldOriginZ = 0.0f;
        params.WorldSizeX = kWorldSize; params.WorldSizeZ = kWorldSize;
        params.HeightScale = kHeightScale; params.WorldOriginY = kWorldOriginY;
        params.SplatmapBindless = 0u; params.HeightmapBindless = 0u; // atlas path: no unified textures
        params.Flags = Terrain::kTerrainFlagAtlasBacked;
        params.GrassEnabled = 1u;
        params.GrassMaskThreshold = 0.0f;
        params.GrassDensity = 1.0f;

        const uint32 kSampleCount = 8u;
        uint32 placed = 0;
        std::vector<float32> roots; // flat WorldX,WorldZ,RootY per blade
        // Frame 0 flushes the atlas region upload through the TerrainUpload node; the compact reads it on
        // the following frames. Read back on the last frame.
        for (uint32 frameIdx = 0; frameIdx < 3; ++frameIdx)
        {
            publishAtlasTerrain();
            terrain.UploadTerrainParamsArray(&params, 1u, GameEngine::Terrain::kDefaultTerrainMaterials,
                                             GameEngine::Terrain::kTerrainLayerRoleCount, frameIdx);
            RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
            frame.BeginFrame(frameIdx);
            RenderGraph::RGTexture color = frame.ImportPersistentTexture("GAtlasRoot.Color", ColorTargetDesc());
            RenderGraph::RGTexture depth = frame.ImportPersistentTexture("GAtlasRoot.Depth", DepthTargetDesc());
            f.Rs->BeginWorldDrawFrame();
            f.Rs->BuildWorldBatchKeys();
            const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{v, color, depth, {}}};
            const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                         f.Rs->Views().GetViews().end());
            instance.Declare(frame, targets, views);

            std::shared_ptr<Rendering::RGBufferReadbackTicket> countT, instT;
            const RenderGraph::RGBuffer argsRG = GrassWorldReadArgs(*f.Rs, frame, v);
            const RenderGraph::RGBuffer instRG = GrassInstancesInFrame(*f.Rs, frame, v);
            const bool lastFrame = (frameIdx == 2u);
            if (lastFrame && argsRG.IsValid())
                countT = Rendering::RequestBufferReadbackRG(f.Device.get(), frame, argsRG, 4u, 4u, "GAtlasRootCount");
            if (lastFrame && instRG.IsValid())
                instT = Rendering::RequestBufferReadbackRG(f.Device.get(), frame, instRG, 0u,
                                                           48u * kSampleCount, "GAtlasRootInst");
            frame.MarkOutput(color);
            frame.Execute();
            Rendering::OnFrameSubmittedReadbacksRG(frame, frame.SubmissionToken());
            f.Device->WaitForIdle();

            if (countT)
            {
                Rendering::BufferReadbackResult res{};
                if (countT->TryGet(res) && res.bytes.size() >= 4)
                    std::memcpy(&placed, res.bytes.data(), sizeof(placed));
            }
            if (instT)
            {
                Rendering::BufferReadbackResult res{};
                if (instT->TryGet(res) && res.bytes.size() >= 48u * kSampleCount)
                {
                    const float32* fp = reinterpret_cast<const float32*>(res.bytes.data());
                    for (uint32 i = 0; i < kSampleCount; ++i)
                    {
                        roots.push_back(fp[i * 12 + 0]); // WorldX
                        roots.push_back(fp[i * 12 + 1]); // WorldZ
                        roots.push_back(fp[i * 12 + 2]); // RootY
                    }
                }
            }
        }

        ASSERT_NE(heightIdx, 0u) << "atlas height bindless registration failed";
        ASSERT_GT(placed, 0u) << "atlas compact placed 0 — cannot check roots";
        ASSERT_FALSE(roots.empty()) << "atlas instance readback failed";

        const TerrainECS::AtlasHeightSampler sampler{atlas.Geo, atlas.Table.Rows.data(), atlas.Atlas.data(), nullptr};
        const uint32 checkN = std::min<uint32>(kSampleCount, placed);
        const float32 tol = kHeightScale * 0.06f; // ~15 m: catches a collapse-to-origin, absorbs texel/filter diffs
        uint32 checked = 0;
        for (uint32 i = 0; i < checkN; ++i)
        {
            const float32 wx = roots[i * 3 + 0];
            const float32 wz = roots[i * 3 + 1];
            const float32 rootY = roots[i * 3 + 2];
            const float32 u = wx / kWorldSize;
            const float32 vv = wz / kWorldSize;
            EXPECT_GE(u, -0.001f); EXPECT_LE(u, 1.001f);
            EXPECT_GE(vv, -0.001f); EXPECT_LE(vv, 1.001f);
            const float32 h01 = sampler.SampleHeightNormalized(std::clamp(u, 0.0f, 1.0f), std::clamp(vv, 0.0f, 1.0f));
            const float32 expectedRootY = kWorldOriginY + h01 * kHeightScale;
            EXPECT_NEAR(rootY, expectedRootY, tol)
                << "atlas blade " << i << " root off the surface: RootY=" << rootY
                << " expected~" << expectedRootY << " at uv(" << u << "," << vv
                << ") — a collapse to WorldOriginY=" << kWorldOriginY << " means the atlas resolve returned 0";
            ++checked;
        }
        EXPECT_GT(checked, 0u);
    }
    f.Down();
}

// Forward-contributor drain order: the world pass drains ForwardCommands in emission order, which put
// producer-emitted blended grass AHEAD of node-emitted opaque CBT terrain. Opaque terrain (depthWrite
// on) then overwrote the depthWrite-off blade pixels wherever it covered them, so grass survived only
// against the sky, most visibly at grazing angles. The drain now records every depth-writing command
// before every blended one (stable within each group). This pins that: a revert to emission order
// fails the index expectations, and deleting the helper breaks the world pass build.
TEST(RenderPipelineDeclareTests, ForwardDrainDrawsDepthWritingBeforeBlended)
{
    using namespace GameEngine::Engine::Renderer;
    using GameEngine::MaterialAlphaMode;

    Material opaque = Material::TestFactory::Create(GUID::Derive(GUID{}, "drain/opaque"), "opaque", 64u);
    Material blend = Material::TestFactory::Create(GUID::Derive(GUID{}, "drain/blend"), "blend", 64u);
    Material::TestFactory::SetAlphaMode(blend, MaterialAlphaMode::Blend);
    ASSERT_EQ(opaque.GetAlphaMode(), MaterialAlphaMode::Opaque);
    ASSERT_EQ(blend.GetAlphaMode(), MaterialAlphaMode::Blend);

    // Grass (blend) is emitted by the producer sweep ahead of CBT terrain (opaque)
    // from its node — reproduce that interleave: blend, opaque, blend, opaque.
    DrawCommand cBlend{}; cBlend.Material = &blend;
    DrawCommand cOpaque{}; cOpaque.Material = &opaque;
    const std::vector<DrawCommand> stream = {cBlend, cOpaque, cBlend, cOpaque};

    const std::vector<uint32_t> order = ForwardDrainOrderOpaqueFirst(stream);
    ASSERT_EQ(order.size(), 4u);
    // Depth-writing (opaque) indices 1,3 first, then blended indices 0,2 — relative order preserved.
    EXPECT_EQ(order[0], 1u);
    EXPECT_EQ(order[1], 3u);
    EXPECT_EQ(order[2], 0u);
    EXPECT_EQ(order[3], 2u);
    // No blended command may precede a depth-writing one in the drained order.
    bool sawBlended = false;
    for (uint32_t idx : order)
    {
        const bool blended = IsBlendedContributor(stream[idx]);
        if (blended)
            sawBlended = true;
        else
            EXPECT_FALSE(sawBlended) << "opaque command drained after a blended one";
    }
}

// Phase B is the HZB occlusion-recovery slice for GPU-cullable entity batches.
// Explicit contributors are not in that scatter stream: terrain, grass, ocean,
// and fog already rendered in phase A. A forward command by itself must therefore
// not keep the recover world pass alive (which used to draw the whole terrain twice),
// whether it writes its own depth or its head went into the prepass. The recover
// prepass draws no contributor head either: the heads are phase A's
// (ContributorDepthCommands, the selection the depth recorder draws).
TEST(RenderPipelineDeclareTests, HzbRecoverDoesNotRedrawForwardContributors)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));

        const CameraId camId = rs.Views().AllocateCamera("RecoverForwardCam");
        const ViewId viewId = rs.Views().AllocateView("RecoverForwardView", camId);
        Rendering::ViewClearConfig clear{};
        rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        const RenderGraph::RGTexture color =
            frame.ImportPersistentTexture("RecoverForward.Color", ColorTargetDesc());
        const RenderGraph::RGTexture depth =
            frame.ImportPersistentTexture("RecoverForward.Depth", DepthTargetDesc());

        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();
        // One contributor that writes its own depth and one whose head went into the prepass (#2433).
        rs.EmitForwardCommand(viewId, DrawCommand{}, ForwardDrawDepth::ColourPass);
        const DrawCommand head{};
        rs.EmitForwardCommand(viewId, DrawCommand{}, ForwardDrawDepth::Prepass, &head);

        RenderServices::WorldPassTargetsRG targets{};
        targets.Color = color;
        targets.Depth = depth;
        const auto recover = rs.AddWorldColorRecoverPassForView(
            frame, viewId, targets, Rendering::MaterialKeyword::None,
            RenderServices::WorldPassDrawScope::All);
        EXPECT_FALSE(recover.Pass.IsValid())
            << "forward-only terrain-style contributors belong to phase A, not HZB recovery";

        using Phase = Rendering::GPUDrawStreamBuilder::SlicePhase;
        EXPECT_EQ(ContributorDepthCommands(rs.Views(), viewId, DepthPassType::Prepass, Phase::A).size(), 1u)
            << "the phase-A prepass draws the head";
        EXPECT_TRUE(ContributorDepthCommands(rs.Views(), viewId, DepthPassType::Prepass, Phase::B).empty())
            << "the recover prepass must not draw the head again";
        EXPECT_TRUE(ContributorDepthCommands(rs.Views(), viewId, DepthPassType::DeformationMotion, Phase::A).empty())
            << "the deforming-motion pass has no motion variant for a contributor";

        rs.Shutdown();
    }
    device->Shutdown();
}

// The shipped grass defaults, pinned against a silent drift. Density is blades per square metre at
// the camera, so it is terrain-size independent: the same value reads the same on a 512 m and a
// 10 km terrain. Height and width are pinned together because the aspect ceiling ties them: the
// ceiling is a fraction of the height, so moving the height alone can put the authored width out of
// reach — see GrassAspectCeilingLeavesTheAuthoredWidthReachable directly below.
TEST(RenderPipelineDeclareTests, GrassDefaultsCalibrated)
{
    const GameEngine::Components::TerrainGrass g{};
    EXPECT_FLOAT_EQ(g.BladesPerSquareMeter, 21.0f) << "default blades/m2 at the camera regressed";
    EXPECT_FLOAT_EQ(g.Range, 500.0f) << "default grass range regressed";
    EXPECT_FLOAT_EQ(g.DensityFalloff, 2.0f) << "default density falloff regressed";
    EXPECT_FLOAT_EQ(g.BladeWidth, 0.043f) << "default blade width regressed";
    EXPECT_FLOAT_EQ(g.BladeHeight, 0.72f) << "default blade height regressed";
    EXPECT_FLOAT_EQ(g.MaxWidthRatio, 0.06f) << "default blade aspect ceiling regressed";
    EXPECT_FLOAT_EQ(g.ClumpSize, 1.1f) << "default clump size regressed";
    EXPECT_FLOAT_EQ(g.ClumpHeightVariance, 0.3f) << "default clump height variance regressed";
    EXPECT_FLOAT_EQ(g.ClumpAlignment, 0.45f) << "default clump alignment regressed";
    EXPECT_FLOAT_EQ(g.ClumpGather, 0.25f) << "default clump gather regressed";
    // The two levers a noon field's blade-to-blade variety comes off, pinned together because they
    // are chosen together: RandomBrightness carries the tonal separation and HueVariation the
    // chroma spread, and a field with one and not the other reads either as one colour lit unevenly
    // or as two species mixed. Chosen by sweeping both, one at a time and then in combination, at
    // two noon and two low-sun poses over a fixed geometric grass mask; the numbers and the frames
    // are in the grass-look-design.html changelog under 2026-09-04.
    EXPECT_FLOAT_EQ(g.RandomBrightness, 0.42f) << "default per-blade value jitter regressed";
    EXPECT_FLOAT_EQ(g.HueVariation, 0.45f) << "default hue variation regressed";
    // Not a look choice at the shipped gradient span: there the root-shade ramp acts only where the
    // colour schedule's weight is near zero, so no value of it moves the field. Pinned to hold it
    // steady, not because this value was selected over another.
    EXPECT_FLOAT_EQ(g.RootShade, 0.82f) << "default root shade regressed";
    // The gradient ships as the whole blade: a shorter span reads as a band across it.
    EXPECT_FLOAT_EQ(g.RootFadeStart, 0.0f) << "default gradient start regressed";
    EXPECT_FLOAT_EQ(g.RootFadeEnd, 1.0f) << "default gradient end regressed";
    // Clumping ships ON: a zero size would leave the golf-lawn field the slice exists to end.
    EXPECT_GT(g.ClumpSize, 0.0f) << "clumping is off by default";
}

// What the shipped defaults actually place, once the per-view instance budget has had its say. The
// authored pair over-subscribes the budget, and the fit spends DISTANCE to pay for it: the near
// density an author reads off the component is the density they get, and the range is not. Pinned
// because it is the one place the two shipped numbers interact, and a drift in either the budget or
// the assumed visible fraction moves the range with nothing else to notice.
TEST(RenderPipelineDeclareTests, GrassShippedDefaultsFitTheBudgetByShorteningRange)
{
    using namespace GameEngine::TerrainGrass;

    const GameEngine::Components::TerrainGrass g{};
    GrassPlacementParams params;
    params.NearDensity = g.BladesPerSquareMeter;
    params.FarRadius = g.Range;
    params.Falloff = g.DensityFalloff;

    const GrassPlacementPlan plan = GrassFitPlacementToBudget(
        params, TerrainGrassRenderFeature::kMaxInstancesPerView);

    EXPECT_FLOAT_EQ(plan.Params.NearDensity, g.BladesPerSquareMeter)
        << "the fit thinned the near field; it must only ever shorten range";
    EXPECT_TRUE(plan.RangeReduced)
        << "the shipped defaults no longer over-subscribe the budget — re-derive the range below";
    // R_fit is where the planned candidate count meets the budget:
    //   2*pi*D*R^2 / ((p+1)(p+2)) * kGrassPlannedVisibleFraction = budget
    //   => R_fit = sqrt(budget * (p+1)(p+2) / (2*pi * D * f))
    // At D = 21, p = 2, f = 0.35, budget = 524288 that is 369.10 m.
    EXPECT_NEAR(plan.Params.FarRadius, 369.1f, 1.0f)
        << "effective grass range at the shipped defaults moved";
    EXPECT_LT(plan.Params.FarRadius, g.Range);
    EXPECT_LE(plan.PlannedCandidates, TerrainGrassRenderFeature::kMaxInstancesPerView);
}

namespace
{
// Components::TerrainGrass is the authored dial; Terrain::TerrainGPUParams is the GPU twin the
// extraction fills from it. Both carry their own default for the SAME number, and the twin's is
// unreachable at runtime — extraction assigns every grass field on both of its paths — so a drift
// between them is silent everywhere except where a bare TerrainGPUParams is built, which is what
// tests and oracles do. One number authored twice needs a pin, and nothing else links the two.
struct GrassFloatTwin
{
    const char* AuthoredName;
    const char* GpuName;
    float32 Components::TerrainGrass::* Authored;
    float32 Terrain::TerrainGPUParams::* Gpu;
};

struct GrassUintTwin
{
    const char* AuthoredName;
    const char* GpuName;
    uint32 Components::TerrainGrass::* Authored;
    uint32 Terrain::TerrainGPUParams::* Gpu;
};

constexpr GrassFloatTwin kGrassFloatTwins[] = {
    {"BladesPerSquareMeter", "GrassDensity",
     &Components::TerrainGrass::BladesPerSquareMeter, &Terrain::TerrainGPUParams::GrassDensity},
    {"Range", "GrassRange",
     &Components::TerrainGrass::Range, &Terrain::TerrainGPUParams::GrassRange},
    {"DensityFalloff", "GrassDensityFalloff",
     &Components::TerrainGrass::DensityFalloff, &Terrain::TerrainGPUParams::GrassDensityFalloff},
    {"PlacementSeed", "GrassPlacementSeed",
     &Components::TerrainGrass::PlacementSeed, &Terrain::TerrainGPUParams::GrassPlacementSeed},
    {"ClumpSize", "GrassClumpSize",
     &Components::TerrainGrass::ClumpSize, &Terrain::TerrainGPUParams::GrassClumpSize},
    {"ClumpHeightVariance", "GrassClumpHeightVariance",
     &Components::TerrainGrass::ClumpHeightVariance,
     &Terrain::TerrainGPUParams::GrassClumpHeightVariance},
    {"ClumpAlignment", "GrassClumpAlignment",
     &Components::TerrainGrass::ClumpAlignment, &Terrain::TerrainGPUParams::GrassClumpAlignment},
    {"ClumpGather", "GrassClumpGather",
     &Components::TerrainGrass::ClumpGather, &Terrain::TerrainGPUParams::GrassClumpGather},
    {"BladeHeight", "GrassBladeHeight",
     &Components::TerrainGrass::BladeHeight, &Terrain::TerrainGPUParams::GrassBladeHeight},
    {"BladeWidth", "GrassBladeWidth",
     &Components::TerrainGrass::BladeWidth, &Terrain::TerrainGPUParams::GrassBladeWidth},
    {"MaxWidthRatio", "GrassMaxWidthRatio",
     &Components::TerrainGrass::MaxWidthRatio, &Terrain::TerrainGPUParams::GrassMaxWidthRatio},
    {"RandomScale", "GrassRandomScale",
     &Components::TerrainGrass::RandomScale, &Terrain::TerrainGPUParams::GrassRandomScale},
    {"MaskThreshold", "GrassMaskThreshold",
     &Components::TerrainGrass::MaskThreshold, &Terrain::TerrainGPUParams::GrassMaskThreshold},
    {"WindDirection", "GrassWindDirection",
     &Components::TerrainGrass::WindDirection, &Terrain::TerrainGPUParams::GrassWindDirection},
    {"WindGustSpeed", "GrassWindGustSpeed",
     &Components::TerrainGrass::WindGustSpeed, &Terrain::TerrainGPUParams::GrassWindGustSpeed},
    {"WindGustScale", "GrassWindGustScale",
     &Components::TerrainGrass::WindGustScale, &Terrain::TerrainGPUParams::GrassWindGustScale},
    {"WindStrength", "GrassWindStrength",
     &Components::TerrainGrass::WindStrength, &Terrain::TerrainGPUParams::GrassWindStrength},
    {"WindRestingLean", "GrassWindRestingLean",
     &Components::TerrainGrass::WindRestingLean, &Terrain::TerrainGPUParams::GrassWindRestingLean},
    {"WindFlutterAmount", "GrassWindFlutterAmount",
     &Components::TerrainGrass::WindFlutterAmount,
     &Terrain::TerrainGPUParams::GrassWindFlutterAmount},
    {"WindFlutterSpeed", "GrassWindFlutterSpeed",
     &Components::TerrainGrass::WindFlutterSpeed,
     &Terrain::TerrainGPUParams::GrassWindFlutterSpeed},
    {"WindSeed", "GrassWindSeed",
     &Components::TerrainGrass::WindSeed, &Terrain::TerrainGPUParams::GrassWindSeed},
    {"Brightness", "GrassBrightness",
     &Components::TerrainGrass::Brightness, &Terrain::TerrainGPUParams::GrassBrightness},
    {"RandomBrightness", "GrassRandomBrightness",
     &Components::TerrainGrass::RandomBrightness,
     &Terrain::TerrainGPUParams::GrassRandomBrightness},
    {"HueVariation", "GrassHueVariation",
     &Components::TerrainGrass::HueVariation, &Terrain::TerrainGPUParams::GrassHueVariation},
    {"RootShade", "GrassRootShade",
     &Components::TerrainGrass::RootShade, &Terrain::TerrainGPUParams::GrassRootShade},
    {"RootFadeStart", "GrassRootFadeStart",
     &Components::TerrainGrass::RootFadeStart, &Terrain::TerrainGPUParams::GrassRootFadeStart},
    {"RootFadeEnd", "GrassRootFadeEnd",
     &Components::TerrainGrass::RootFadeEnd, &Terrain::TerrainGPUParams::GrassRootFadeEnd},
    {"BladeNormalForm", "GrassBladeNormalForm",
     &Components::TerrainGrass::BladeNormalForm, &Terrain::TerrainGPUParams::GrassBladeNormalForm},
    {"BladeScatterGain", "GrassBladeScatterGain",
     &Components::TerrainGrass::BladeScatterGain,
     &Terrain::TerrainGPUParams::GrassBladeScatterGain},
    {"GroundingStrength", "GrassGroundingStrength",
     &Components::TerrainGrass::GroundingStrength,
     &Terrain::TerrainGPUParams::GrassGroundingStrength},
    {"Translucency", "GrassTranslucency",
     &Components::TerrainGrass::Translucency, &Terrain::TerrainGPUParams::GrassTranslucency},
    {"TextureCardsPerSquareMeter", "GrassTextureCardsPerSquareMeter",
     &Components::TerrainGrass::TextureCardsPerSquareMeter,
     &Terrain::TerrainGPUParams::GrassTextureCardsPerSquareMeter},
    {"TextureSize", "GrassTextureSize",
     &Components::TerrainGrass::TextureSize, &Terrain::TerrainGPUParams::GrassTextureSize},
    {"AlphaCutoff", "GrassAlphaCutoff",
     &Components::TerrainGrass::AlphaCutoff, &Terrain::TerrainGPUParams::GrassAlphaCutoff},
    {"NormalStrength", "GrassNormalStrength",
     &Components::TerrainGrass::NormalStrength, &Terrain::TerrainGPUParams::GrassNormalStrength},
};

constexpr GrassUintTwin kGrassUintTwins[] = {
    {"BladeSegments", "GrassBladeSegments",
     &Components::TerrainGrass::BladeSegments, &Terrain::TerrainGPUParams::GrassBladeSegments},
    {"LayerIndex", "GrassLayerIndex",
     &Components::TerrainGrass::LayerIndex, &Terrain::TerrainGPUParams::GrassLayerIndex},
    {"RootColor", "GrassRootColor",
     &Components::TerrainGrass::RootColor, &Terrain::TerrainGPUParams::GrassRootColor},
    {"TipColor", "GrassTipColor",
     &Components::TerrainGrass::TipColor, &Terrain::TerrainGPUParams::GrassTipColor},
    {"BacklightColor", "GrassBacklightColor",
     &Components::TerrainGrass::BacklightColor, &Terrain::TerrainGPUParams::GrassBacklightColor},
    {"AtlasColumns", "GrassAtlasColumns",
     &Components::TerrainGrass::AtlasColumns, &Terrain::TerrainGPUParams::GrassAtlasColumns},
    {"AtlasRows", "GrassAtlasRows",
     &Components::TerrainGrass::AtlasRows, &Terrain::TerrainGPUParams::GrassAtlasRows},
    {"AtlasTileCount", "GrassAtlasTileCount",
     &Components::TerrainGrass::AtlasTileCount, &Terrain::TerrainGPUParams::GrassAtlasTileCount},
};

// The component fields with no scalar twin: RenderMode, TextureGrass and UseSplatRootColor are
// packed into GrassEnabled's bits, and the three texture refs upload as bindless indices.
constexpr size_t kGrassFieldsWithoutAScalarTwin = 6;
} // namespace

// Every authored default and its GPU twin are the same number. A drift here is invisible at runtime
// and shows up only where a bare TerrainGPUParams stands in for an extracted one.
TEST(RenderPipelineDeclareTests, GrassGpuParamDefaultsMatchTheComponent)
{
    const Components::TerrainGrass authored{};
    const Terrain::TerrainGPUParams gpu{};

    for (const GrassFloatTwin& twin : kGrassFloatTwins)
        EXPECT_FLOAT_EQ(gpu.*twin.Gpu, authored.*twin.Authored)
            << "TerrainGPUParams::" << twin.GpuName << " defaults away from TerrainGrass::"
            << twin.AuthoredName;

    for (const GrassUintTwin& twin : kGrassUintTwins)
        EXPECT_EQ(gpu.*twin.Gpu, authored.*twin.Authored)
            << "TerrainGPUParams::" << twin.GpuName << " defaults away from TerrainGrass::"
            << twin.AuthoredName;

    // The THIRD carrier of the same number. GrassPlacementParams is a fit input, always assigned
    // before use on the production path, so nothing else notices when it drifts — it was set back
    // to 8 while the other two read 21 and the whole suite stayed green. It still reads as an
    // authority on the shipped density to anyone who opens the header, so it is pinned here rather
    // than left to be believed.
    EXPECT_FLOAT_EQ(GameEngine::TerrainGrass::GrassPlacementParams{}.NearDensity,
                    authored.BladesPerSquareMeter)
        << "GrassPlacementParams::NearDensity defaults away from "
           "TerrainGrass::BladesPerSquareMeter";
}

// The twin tables are hand-written, and a hand-written enumeration over a duplicated declaration is
// exactly what goes stale. Every reflected TerrainGrass field is either twinned above or counted as
// deliberately twinless, so a new dial reds this until someone decides which it is.
TEST(RenderPipelineDeclareTests, EveryGrassFieldIsTwinnedOrDeliberatelyNot)
{
    const ECS::ComponentTypeId typeId = ECS::ComponentFieldRegistry::FindByName("TerrainGrass");
    ASSERT_NE(typeId, 0u) << "TerrainGrass is not reflected in this binary — the registry is not "
                             "populated and this test would assert nothing";

    EXPECT_EQ(std::size(kGrassFloatTwins) + std::size(kGrassUintTwins)
                  + kGrassFieldsWithoutAScalarTwin,
              ECS::ComponentFieldRegistry::Get(typeId).size())
        << "a TerrainGrass field is neither pinned to a GPU twin nor counted as twinless";
}

// The aspect ceiling is a sanity ceiling, not the width control. If it is tight enough to clamp the
// shipped BladeWidth then the component lies about what it renders: the author sets a width, every
// blade comes out narrower, and no amount of authoring reaches the value in the field. Coverage is
// proportional to width, so a ceiling that binds by default is a field of whiskers by default.
TEST(RenderPipelineDeclareTests, GrassAspectCeilingLeavesTheAuthoredWidthReachable)
{
    const GameEngine::Components::TerrainGrass g{};
    ASSERT_GT(g.MaxWidthRatio, 0.0f) << "a zero aspect ceiling clamps every blade to zero width";

    const float ceiling = g.BladeHeight * g.MaxWidthRatio;
    EXPECT_LE(g.BladeWidth, ceiling)
        << "shipped BladeWidth " << g.BladeWidth << " m is unreachable under the aspect ceiling ("
        << g.MaxWidthRatio << " x BladeHeight " << g.BladeHeight << " m = " << ceiling
        << " m): the ceiling is setting the width, not the author";

    // The ceiling is still a real bound, not a disabled one: a blade authored as wide as it is tall
    // must come back clamped, so short grass cannot degenerate into squares.
    EXPECT_LT(ceiling, g.BladeHeight)
        << "aspect ceiling no longer clamps a blade authored as wide as it is tall";
}

// Range gate: density falls to zero at GrassRange metres from the camera, so Range is the hard
// visibility radius. A km-scale terrain is framed from hundreds of metres away, so a range shorter
// than the framing distance places NOTHING while the whole terrain is on screen — the "tiled grass
// invisible" report. With the camera parked 300 m from the grass, a 180 m range places zero and the
// 500 m default places a band. This locks the default against a regression to a too-short radius.
TEST(RenderPipelineDeclareTests, GrassRangePlacesAtTerrainFramingDistance)
{
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        ASSERT_TRUE(terrain.Initialize(f.Device.get()));
        if (!f.Rs->Textures().IsBindlessEnabled())
            GTEST_SKIP() << "bindless disabled on this device";

        // Park the camera 300 m south of the terrain's z=0 edge (grass spans z in [0,1536]): every
        // blade is >= 300 m away, so a 180 m range places none of them and a 500 m range keeps a band.
        f.SetCameraLookingAt(Mathematics::Vector3(768.0f, 40.0f, -300.0f),
                             Mathematics::Vector3(768.0f, 20.0f, 400.0f));

        const ViewId v = f.AddView("GrassFadeView", 1u);
        ASSERT_TRUE(f.Registry.Register(
            "TerrainGrass", [] { return std::make_unique<TerrainGrass::TerrainGrassRenderNode>(); },
            true));

        const uint32 dim = 65;
        Terrain::HeightfieldData hf(dim, dim, 0.0f);
        hf.FillWithNoise(2.5f, 60.0f, 4, 7);
        uint32 sw = 0, sh = 0;
        std::vector<uint8> splat = BakeDefaultRulesSplat(hf, 1536.0f, 60.0f, sw, sh);
        float32 minH = 0, maxH = 0;
        hf.GetMinMax(0, 0, static_cast<int32>(dim), static_cast<int32>(dim), minH, maxH);
        const float32 range = std::max(maxH - minH, 1e-3f);
        std::vector<float32> normHeight(static_cast<size_t>(dim) * dim);
        for (uint32 i = 0; i < dim * dim; ++i)
            normHeight[i] = (hf.GetRawSamples()[i] - minH) / range;

        const TerrainECS::TerrainHandle h{7u, 1u};
        terrain.UploadSplatmap(h, splat.data(), dim, dim);
        terrain.UploadHeightmap(h, normHeight.data(), dim, dim);

        RenderPipelineBlueprint bp;
        bp.pipelineName = "GrassFadeTest";
        {
            // Without TerrainUpload the baked splat never reaches the GPU (it is the only caller of
            // FlushPendingUploads), so the mask gate rejects every candidate and BOTH range arms
            // read zero — which would let the short-range arm pass for the wrong reason.
            RenderPipelineBlueprint::Pass up;
            up.id = "Upload"; up.type = "TerrainUpload"; up.enabled = true; up.perView = true;
            up.passJson = R"({"id":"Upload","type":"TerrainUpload"})";
            bp.passes.push_back(up);
        }
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "Grass"; p.type = "TerrainGrass"; p.enabled = true; p.perView = true;
            p.passJson = R"({"id":"Grass","type":"TerrainGrass"})";
            bp.passes.push_back(p);
        }
        bp.outputs.push_back({"FinalColor", "View.Resolve"});
        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);
        FramePools pools(f.Device.get());

        uint32 splatIdx = 0;
        auto placedWithRange = [&](float32 range) -> uint32 {
            Terrain::TerrainGPUParams params{};
            params.WorldOriginX = 0.0f; params.WorldOriginZ = 0.0f;
            params.WorldSizeX = 1536.0f; params.WorldSizeZ = 1536.0f;
            params.HeightScale = 60.0f; params.WorldOriginY = 0.0f;
            params.GrassEnabled = 1u;        // bit0 enabled
            params.GrassMaskThreshold = 0.02f;
            params.GrassDensity = 1.0f;      // 1 blade/m2 at the camera
            params.GrassDensityFalloff = 1.0f; // linear to Range, so the whole band carries density
            params.GrassRange = range;

            uint32 placed = 0;
            for (uint32 frameIdx = 0; frameIdx < 2; ++frameIdx)
            {
                // A bindless slot is withheld until the texture's first whole-texture copy has
                // been recorded, so registration is retried every frame exactly as the extraction
                // system does it: frame 0 flushes, frame 1 publishes and samples.
                terrain.RegisterSplatmapBindless(h, *f.Rs);
                terrain.RegisterHeightmapBindless(h, *f.Rs);
                splatIdx = terrain.GetSplatmapBindlessIndex(h);
                params.SplatmapBindless = splatIdx;
                params.HeightmapBindless = terrain.GetHeightmapBindlessIndex(h);
                terrain.UploadTerrainParamsArray(&params, 1u, GameEngine::Terrain::kDefaultTerrainMaterials,
                                                 GameEngine::Terrain::kTerrainLayerRoleCount, frameIdx);
                RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
                frame.BeginFrame(frameIdx);
                RenderGraph::RGTexture color = frame.ImportPersistentTexture("GF.Color", ColorTargetDesc());
                RenderGraph::RGTexture depth = frame.ImportPersistentTexture("GF.Depth", DepthTargetDesc());
                f.Rs->BeginWorldDrawFrame();
                f.Rs->BuildWorldBatchKeys();
                const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{v, color, depth, {}}};
                const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                             f.Rs->Views().GetViews().end());
                instance.Declare(frame, targets, views);
                std::shared_ptr<Rendering::RGBufferReadbackTicket> ticket;
                const RenderGraph::RGBuffer argsRG = GrassWorldReadArgs(*f.Rs, frame, v);
                if (frameIdx == 1 && argsRG.IsValid())
                    // Both LOD records: this grass is 300 m out, past the LOD 1 split, so a
                    // LOD-0-only count would read zero however much was placed.
                    ticket = Rendering::RequestBufferReadbackRG(f.Device.get(), frame, argsRG, 0u,
                                                                sizeof(TerrainGrass::GrassIndirectBlockGPU),
                                                                "GrassRangeCount");
                frame.MarkOutput(color);
                frame.Execute();
                Rendering::OnFrameSubmittedReadbacksRG(frame, frame.SubmissionToken());
                f.Device->WaitForIdle();
                if (ticket)
                {
                    Rendering::BufferReadbackResult res{};
                    if (ticket && ticket->TryGet(res) &&
                        res.bytes.size() >= sizeof(TerrainGrass::GrassIndirectBlockGPU))
                    {
                        TerrainGrass::GrassIndirectBlockGPU block{};
                        std::memcpy(&block, res.bytes.data(), sizeof(block));
                        for (const auto& draw : block.Draws)
                            placed += draw.InstanceCount;
                    }
                }
            }
            return placed;
        };

        const uint32 placedShortRange = placedWithRange(180.0f);
        const uint32 placedDefaultRange = placedWithRange(500.0f);

        // Both arms would read zero if the splat slot never published, which would make the
        // short-range expectation below pass for the wrong reason.
        ASSERT_NE(splatIdx, 0u) << "splat bindless registration failed";
        EXPECT_EQ(placedShortRange, 0u)
            << "a 180 m range must place no blade at a 300 m framing distance (the invisibility bug)";
        EXPECT_GT(placedDefaultRange, 0u)
            << "the 500 m default range must place grass at a 300 m framing distance";
    }
    f.Down();
}

TEST(RenderPipelineDeclareTests, ReadbackStripsDeviceRowPaddingForFullRegionAndSubresource)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    auto& caps = const_cast<RenderingDeviceCapabilities&>(device->GetCapabilities());
    const auto originalAlignment = caps.textureCopyRowPitchAlignment;
    struct RestoreAlignment
    {
        RenderingDeviceCapabilities& Caps;
        uint32_t Value;
        ~RestoreAlignment() { Caps.textureCopyRowPitchAlignment = Value; }
    } restore{caps, originalAlignment};
    // Vulkan accepts a padded pitch too, so exercise the WebGPU copy contract
    // on the native test device and verify that no padding escapes to callers.
    caps.textureCopyRowPitchAlignment = 256;
    {
        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(1);
        TextureDesc td{};
        td.width = 7;
        td.height = 5;
        td.depth = 1;
        td.mipLevels = 1;
        td.arrayLayers = 1;
        td.sampleCount = 1;
        td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
        td.usage = static_cast<uint32_t>(TextureUsage::RenderTarget) |
                   static_cast<uint32_t>(TextureUsage::TransferSrc);
        const auto src = frame.CreateTexture("PaddedReadback.Src", td);
        ASSERT_TRUE(src.IsValid());
        frame.AddPass(
            "PaddedReadback.Fill", static_cast<int32_t>(PassPhase::kWorldRender),
            [&](RenderGraph::RGPassBuilder& p)
            {
                RenderGraph::RGAttachmentOps ops{};
                ops.Load = RenderGraph::RGLoadOp::Clear;
                ops.Store = RenderGraph::RGStoreOp::Store;
                ops.Clear.Color[0] = 0.25f;
                ops.Clear.Color[1] = 0.5f;
                ops.Clear.Color[2] = 0.75f;
                ops.Clear.Color[3] = 1.0f;
                p.AttachColor(0, src, ops);
            },
            [](RenderGraph::RGContext&) {});
        const std::array tickets = {
            Rendering::RequestTextureReadbackRG(device.get(), frame, src, "Full"),
            Rendering::RequestTextureRegionReadbackRG(device.get(), frame, src, 1, 1, 3, 3, "Region"),
            Rendering::RequestTextureSubresourceReadbackRG(device.get(), frame, src,
                                                            0, 0, 2, 1, 3, 3, "Subresource")};
        for (const auto& ticket : tickets)
            ASSERT_NE(ticket, nullptr);
        frame.Execute();
        Rendering::OnFrameSubmittedReadbacksRG(frame, frame.SubmissionToken());
        device->WaitForIdle();
        for (size_t t = 0; t < tickets.size(); ++t)
        {
            SCOPED_TRACE(t);
            Rendering::ViewReadbackResult result{};
            ASSERT_TRUE(tickets[t]->TryGet(result));
            EXPECT_EQ(result.width, t == 0 ? 7u : 3u);
            EXPECT_EQ(result.height, t == 0 ? 5u : 3u);
            ASSERT_EQ(result.pixels.size(), result.width * result.height * 4u);
            for (size_t i = 0; i < result.pixels.size(); i += 4)
            {
                EXPECT_NEAR(result.pixels[i], 64, 2);
                EXPECT_NEAR(result.pixels[i + 1], 128, 2);
                EXPECT_NEAR(result.pixels[i + 2], 191, 2);
                EXPECT_EQ(result.pixels[i + 3], 255);
            }
        }
    }
    device->Shutdown();
}

// A readback of a persistent sampled texture (a thumbnail slot) leaves the image in
// TRANSFER_SRC after the copy; the next frame imports it at ShaderResource again, so
// the helper must end the frame with the image back in its declared sampled layout.
TEST(RenderPipelineDeclareTests, DeviceTextureReadbackRestoresTheDeclaredSampledLayout)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        TextureDesc td{};
        td.width = 4;
        td.height = 4;
        td.depth = 1;
        td.mipLevels = 1;
        td.arrayLayers = 1;
        td.sampleCount = 1;
        td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
        td.usage = static_cast<uint32_t>(TextureUsage::RenderTarget) |
                   static_cast<uint32_t>(TextureUsage::ShaderResource) |
                   static_cast<uint32_t>(TextureUsage::TransferSrc);
        td.debugName = "RestoreReadback.Slot";
        const TextureHandle slot = device->CreateTexture(td);
        ASSERT_TRUE(slot.IsValid());

        // Frame 1 renders the slot and leaves it sampled, as a thumbnail render does.
        frame.BeginFrame(1);
        const RenderGraph::RGTexture target = frame.ImportExternalTexture(
            "RestoreReadback.Slot", slot, Rendering::ResourceState::Undefined, TextureFormat::RGBA8_UNORM);
        ASSERT_TRUE(target.IsValid());
        frame.AddPass(
            "RestoreReadback.Fill", static_cast<int32_t>(PassPhase::kWorldRender),
            [&](RenderGraph::RGPassBuilder& p)
            {
                RenderGraph::RGAttachmentOps ops{};
                ops.Load = RenderGraph::RGLoadOp::Clear;
                ops.Store = RenderGraph::RGStoreOp::Store;
                p.AttachColor(0, target, ops);
            },
            [](RenderGraph::RGContext&) {});
        frame.MarkOutput(target, RenderGraph::RGImageLayout::ShaderReadOnly);
        frame.Execute();
        device->WaitForIdle();

        // Frame 2 reads it back from that sampled state.
        frame.BeginFrame(2);
        const auto ticket = Rendering::RequestDeviceTextureReadbackRG(
            device.get(), frame, slot, Rendering::ResourceState::ShaderResource, "RestoreReadback");
        ASSERT_NE(ticket, nullptr);
        const RenderGraph::RGTexture imported = frame.FindTexture("RGReadback.Import.RestoreReadback");
        ASSERT_TRUE(imported.IsValid());
        frame.Execute();
        Rendering::OnFrameSubmittedReadbacksRG(frame, frame.SubmissionToken());
        device->WaitForIdle();
        EXPECT_EQ(frame.Graph().FinalLayout(imported.Id), RenderGraph::RGImageLayout::ShaderReadOnly)
            << "the readback must put the slot back in the sampled layout its next import claims";
        Rendering::ViewReadbackResult result{};
        EXPECT_TRUE(ticket->TryGet(result));

        device->DestroyTexture(slot);
    }
    device->Shutdown();
}

// Slice-8c-1 — the RenderGraph readback utility's completion contract: a ticket
// resolves ONLY when its declaring frame incarnation was submitted (stamped
// post-Execute) AND the GPU finished; an unstamped/re-begun incarnation can
// never resolve. No frame-count fallback exists by design.
TEST(RenderPipelineDeclareTests, ReadbackTicketResolvesOnlyAfterStampAndSignal)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(1);

        TextureDesc td{};
        td.width = 4;
        td.height = 4;
        td.depth = 1;
        td.mipLevels = 1;
        td.arrayLayers = 1;
        td.sampleCount = 1;
        td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
        td.usage = static_cast<uint32_t>(TextureUsage::RenderTarget) |
                   static_cast<uint32_t>(TextureUsage::TransferSrc);
        td.debugName = "Readback.Src";
        const RenderGraph::RGTexture src = frame.CreateTexture("Readback.Src", td);
        ASSERT_TRUE(src.IsValid());

        // Clear-only producer: the attachment ops write a known color.
        frame.AddPass(
            "Readback.Fill", static_cast<int32_t>(PassPhase::kWorldRender),
            [&](RenderGraph::RGPassBuilder& p)
            {
                RenderGraph::RGAttachmentOps ops{};
                ops.Load = RenderGraph::RGLoadOp::Clear;
                ops.Store = RenderGraph::RGStoreOp::Store;
                ops.Clear.Color[0] = 0.25f;
                ops.Clear.Color[1] = 0.5f;
                ops.Clear.Color[2] = 0.75f;
                ops.Clear.Color[3] = 1.0f;
                p.AttachColor(0, src, ops);
            },
            [](RenderGraph::RGContext&) {});

        auto ticket = Rendering::RequestTextureReadbackRG(device.get(), frame, src, "Test");
        ASSERT_NE(ticket, nullptr);

        Rendering::ViewReadbackResult result{};
        EXPECT_FALSE(ticket->TryGet(result)) << "must not resolve before Execute";

        frame.Execute();
        EXPECT_FALSE(ticket->TryGet(result)) << "must not resolve before the stamp";

        Rendering::OnFrameSubmittedReadbacksRG(frame, frame.SubmissionToken());
        EXPECT_TRUE(ticket->IsStamped());
        device->WaitForIdle();

        ASSERT_TRUE(ticket->TryGet(result));
        ASSERT_EQ(result.width, 4u);
        ASSERT_EQ(result.height, 4u);
        ASSERT_EQ(result.pixels.size(), 4u * 4u * 4u);
        // UNORM rounding tolerance (0.5*255 sits exactly between codes).
        EXPECT_NEAR(result.pixels[0], 64, 2);  // R = 0.25
        EXPECT_NEAR(result.pixels[1], 128, 2); // G = 0.5
        EXPECT_NEAR(result.pixels[2], 191, 2); // B = 0.75
        EXPECT_EQ(result.pixels[3], 255);      // A = 1
        EXPECT_FALSE(ticket->TryGet(result)) << "a ticket resolves exactly once";

        // Negative arm: a readback declared into an incarnation that is
        // RE-BEGUN before its stamp is dead — stamping the new incarnation
        // must drop (not adopt) it, and the ticket never resolves.
        frame.BeginFrame(2);
        const RenderGraph::RGTexture src2 = frame.CreateTexture("Readback.Src", td);
        frame.AddPass(
            "Readback.Fill", static_cast<int32_t>(PassPhase::kWorldRender),
            [&](RenderGraph::RGPassBuilder& p)
            {
                RenderGraph::RGAttachmentOps ops{};
                ops.Load = RenderGraph::RGLoadOp::Clear;
                ops.Store = RenderGraph::RGStoreOp::Store;
                p.AttachColor(0, src2, ops);
            },
            [](RenderGraph::RGContext&) {});
        auto staleTicket = Rendering::RequestTextureReadbackRG(device.get(), frame, src2, "Stale");
        ASSERT_NE(staleTicket, nullptr);

        frame.BeginFrame(3); // re-begin WITHOUT executing incarnation 2
        const RenderGraph::RGTexture src3 = frame.CreateTexture("Readback.Src", td);
        frame.AddPass(
            "Readback.Fill", static_cast<int32_t>(PassPhase::kWorldRender),
            [&](RenderGraph::RGPassBuilder& p)
            {
                RenderGraph::RGAttachmentOps ops{};
                ops.Load = RenderGraph::RGLoadOp::Clear;
                ops.Store = RenderGraph::RGStoreOp::Store;
                p.AttachColor(0, src3, ops);
            },
            [](RenderGraph::RGContext&) {});
        frame.Execute();
        Rendering::OnFrameSubmittedReadbacksRG(frame, frame.SubmissionToken());
        EXPECT_FALSE(staleTicket->IsStamped())
            << "a re-begun incarnation's pendings must be dropped, never adopted";
        device->WaitForIdle();
        EXPECT_FALSE(staleTicket->TryGet(result));
    }
    device->Shutdown();
}

// GPU-resource-lifetime policy slice (c): a readback consumer that must block
// waits its OWN submission, never the device. Two arms, and the instrument is
// proven before either reading is taken with it — GetIdleDrainCount has a
// base-class default of 0, so a "no drains happened" assertion would pass
// vacuously on a backend that does not track them.
TEST(RenderPipelineDeclareTests, ReadbackTicketWaitsItsOwnSubmissionNotTheDevice)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        // Positive control: the counter moves on a real drain. Without this the
        // zero deltas below are absence of evidence, not evidence of absence.
        const uint64_t beforeControl = device->GetIdleDrainCount();
        device->WaitForIdle();
        ASSERT_EQ(device->GetIdleDrainCount(), beforeControl + 1)
            << "drain counter must track WaitForIdle for the oracles below to mean anything";

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(1);

        TextureDesc td{};
        td.width = 4;
        td.height = 4;
        td.depth = 1;
        td.mipLevels = 1;
        td.arrayLayers = 1;
        td.sampleCount = 1;
        td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
        td.usage = static_cast<uint32_t>(TextureUsage::RenderTarget) |
                   static_cast<uint32_t>(TextureUsage::TransferSrc);
        td.debugName = "WaitReadback.Src";
        const RenderGraph::RGTexture src = frame.CreateTexture("WaitReadback.Src", td);
        ASSERT_TRUE(src.IsValid());

        frame.AddPass(
            "WaitReadback.Fill", static_cast<int32_t>(PassPhase::kWorldRender),
            [&](RenderGraph::RGPassBuilder& p)
            {
                RenderGraph::RGAttachmentOps ops{};
                ops.Load = RenderGraph::RGLoadOp::Clear;
                ops.Store = RenderGraph::RGStoreOp::Store;
                ops.Clear.Color[0] = 0.25f;
                ops.Clear.Color[1] = 0.5f;
                ops.Clear.Color[2] = 0.75f;
                ops.Clear.Color[3] = 1.0f;
                p.AttachColor(0, src, ops);
            },
            [](RenderGraph::RGContext&) {});

        auto ticket = Rendering::RequestTextureReadbackRG(device.get(), frame, src, "WaitTest");
        ASSERT_NE(ticket, nullptr);

        // Arm 1: an unstamped ticket can never resolve, so the wait must fail
        // immediately rather than block until its timeout. The movie-capture
        // caller relies on this to fall through to its retry pacing.
        EXPECT_FALSE(ticket->WaitUntilReady())
            << "an unstamped ticket must fail the wait, not block on it";

        frame.Execute();
        Rendering::OnFrameSubmittedReadbacksRG(frame, frame.SubmissionToken());
        ASSERT_TRUE(ticket->IsStamped());

        // Arm 2: after the stamp the wait retires exactly this submission and
        // the next TryGet resolves — with no device drain anywhere in between.
        const uint64_t beforeWait = device->GetIdleDrainCount();
        ASSERT_TRUE(ticket->WaitUntilReady());
        Rendering::ViewReadbackResult result{};
        ASSERT_TRUE(ticket->TryGet(result))
            << "the ticket must be resolvable the moment its own submission retired";
        EXPECT_EQ(device->GetIdleDrainCount(), beforeWait)
            << "waiting a readback must not drain the device";

        ASSERT_EQ(result.pixels.size(), 4u * 4u * 4u);
        // UNORM rounding tolerance (0.5*255 sits exactly between codes).
        EXPECT_NEAR(result.pixels[0], 64, 2);  // R = 0.25
        EXPECT_NEAR(result.pixels[1], 128, 2); // G = 0.5
        EXPECT_NEAR(result.pixels[2], 191, 2); // B = 0.75
        EXPECT_EQ(result.pixels[3], 255);      // A = 1

        // Arm 3: a consumed ticket has nothing left to wait on.
        EXPECT_FALSE(ticket->WaitUntilReady())
            << "a consumed ticket must fail the wait, not block on it";
        EXPECT_EQ(device->GetIdleDrainCount(), beforeWait);
    }
    device->Shutdown();
}

// Slice-8c-2b — the thumbnail declaration shape: a second view declared
// MID-FRAME (after the spine) chains world → tonemap → encode into an
// imported handler-owned device texture, with the per-view bucketer twin a
// graceful no-op when the view has no batch keys. The Instanced-only world
// pass must not allocate a shadow array (keyword-less view contract). The
// bucketer→world ordering edge needs real entity batch keys (mesh+material
// registries) and is covered by the live-editor gate instead.
TEST(RenderPipelineDeclareTests, ThumbnailShapedViewChainsWorldTonemapEncode)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        GameEngine::Testing::PinWorldOnlyPipeline(rs.Spine());
        const CameraId camId = rs.Views().AllocateCamera("MainCam");
        CameraData cd{};
        for (int i = 0; i < 16; i += 5)
        {
            cd.view[i] = 1.0f;
            cd.proj[i] = 1.0f;
            cd.viewProj[i] = 1.0f;
        }
        rs.Views().SetCameraData(camId, cd);
        const ViewId mainView = rs.Views().AllocateView("MainView", camId);
        rs.Views().SetViewRenderLayerMask(mainView, 1u);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        rs.Views().SetViewTargets(mainView, 0, 0, 0, clear);

        ASSERT_NE(rs.GetGPUScene(), nullptr);
        Rendering::GPUInstance inst{};
        inst.boundingRadius = 1.0f;
        inst.flags = ~0u;
        rs.GetGPUScene()->AddInstance(inst);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        GameEngine::Testing::ScopedPipelineFrame frameRegistration(rs.Spine(), frame);
        frame.BeginFrame(0);

        RenderGraph::RGTexture color = frame.ImportPersistentTexture("Thumb8c.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("Thumb8c.Depth", DepthTargetDesc());

        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();
        const ViewTargetsRG vt{mainView, color, depth, {}};
        RenderServices::FrameGraphBuildParamsRG params{};
        params.ViewTargets = std::span<const ViewTargetsRG>(&vt, 1);
        rs.Spine().BuildFrameGraph(frame, params);

        // ── Thumbnail-shaped second view, declared mid-frame. ──
        const CameraId thumbCam = rs.Views().AllocateCamera("ThumbCam");
        rs.Views().SetCameraData(thumbCam, cd);
        const ViewId thumbView = rs.Views().AllocateView("ThumbView", thumbCam);
        Rendering::ViewClearConfig thumbClear{};
        thumbClear.clearColor = true;
        thumbClear.clearDepth = true;
        thumbClear.clearDepthValue = 0.0f;
        rs.Views().SetViewClearConfig(thumbView, thumbClear);
        rs.BuildWorldBatchKeysForView(thumbView);

        // Bucketer twin: no batch keys for this view ⇒ graceful no-op that
        // reports success (a clear-only render is legitimate).
        const size_t passesBeforeBucketer = frame.Graph().PassCount();
        EXPECT_TRUE(rs.ScheduleBucketerDispatchesForView(frame, thumbView))
            << "spine ran this frame + no keys => legitimate no-op, not a refusal";
        EXPECT_EQ(frame.Graph().PassCount(), passesBeforeBucketer)
            << "no batch keys => the bucketer twin declares nothing";

        // A frame incarnation the spine never stamped must be REFUSED — the
        // caller defers the render instead of baking a blank slot (review
        // finding: the bucketer twin requires spine-published frame-locals).
        {
            RenderGraph::RGFrame noSpine(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
            noSpine.BeginFrame(7);
            EXPECT_FALSE(rs.ScheduleBucketerDispatchesForView(noSpine, thumbView))
                << "no spine stamp for this incarnation => refuse";
        }

        // World targets are POOL imports (BuildPassResourcesRG resolves the
        // depth physical at declaration — transients assert); the tonemapped
        // intermediate is exec-only and stays transient.
        TextureDesc hdrDesc = ColorTargetDesc();
        hdrDesc.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
        hdrDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget) |
                        static_cast<uint32_t>(TextureUsage::ShaderResource);
        const RenderGraph::RGTexture hdr = frame.ImportPersistentTexture("Thumb8c.HDR", hdrDesc);
        const RenderGraph::RGTexture thumbDepth =
            frame.ImportPersistentTexture("Thumb8c.TDepth", DepthTargetDesc());
        const RenderGraph::RGTexture tonemapped = frame.CreateTexture("Thumb8c.Tonemapped", hdrDesc);

        const size_t resourcesBeforeWorld = frame.Graph().ResourceCount();
        RenderServices::WorldPassTargetsRG targets{};
        targets.Color = hdr;
        targets.Depth = thumbDepth;
        const auto world =
            rs.AddWorldPassForView(frame, thumbView, targets, Rendering::MaterialKeyword::Instanced);
        ASSERT_TRUE(world.Pass.IsValid()) << "clear-requested world pass must declare";
        EXPECT_EQ(world.EffectiveColor.Id, hdr.Id);
        // The keyword-less view contract: no shadow-array (layered depth
        // texture) allocation — the world binds the dummy instead.
        for (size_t r = resourcesBeforeWorld; r < frame.Graph().ResourceCount(); ++r)
        {
            const auto& d = frame.Graph().ResourceDesc(static_cast<RenderGraph::RGResourceId>(r));
            EXPECT_FALSE(d.Kind == RenderGraph::RGResourceKind::Texture && d.ArrayLayers > 1)
                << "Instanced-only world pass allocated a layered texture "
                   "(shadow array?) named '" << (d.Name ? d.Name : "<null>") << "'";
        }

        const RenderGraph::RGPass tonePass = Passes::AddTonemapPassRG(
            frame, world.EffectiveColor, tonemapped, Passes::TonemapParams{}, "Thumb8c.Tonemap");
        if (!tonePass.IsValid())
            GTEST_SKIP() << "tonemap.shaderpkg unavailable in this environment";
        EXPECT_TRUE(frame.Graph().HasReadAccess(tonePass.Id, world.EffectiveColor.Id));

        // Handler-owned slot device texture, imported at Undefined (discard).
        TextureDesc slotDesc{};
        slotDesc.width = 64;
        slotDesc.height = 64;
        slotDesc.depth = 1;
        slotDesc.mipLevels = 1;
        slotDesc.arrayLayers = 1;
        slotDesc.sampleCount = 1;
        slotDesc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
        slotDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget) |
                         static_cast<uint32_t>(TextureUsage::ShaderResource);
        slotDesc.debugName = "Thumb8c.Slot";
        const TextureHandle slotHandle = device->CreateTexture(slotDesc);
        ASSERT_TRUE(slotHandle.IsValid());
        const RenderGraph::RGTexture slotTex =
            frame.ImportExternalTexture("Thumb8c.Slot", slotHandle,
                                        Rendering::ResourceState::Undefined,
                                        TextureFormat::RGBA8_UNORM);
        ASSERT_TRUE(slotTex.IsValid());
        const RenderGraph::RGPass encodePass =
            Rendering::Passes::AddSRGBEncodePassRG(
                frame, tonemapped, slotTex,
                {.InputSpace = Rendering::Passes::FinalizeInputSpace::Linear,
                 .Quantizer = Rendering::Passes::FinalizeQuantizer::Destination},
                "Thumb8c.Encode");
        ASSERT_TRUE(encodePass.IsValid());
        frame.MarkOutput(slotTex, RenderGraph::RGImageLayout::ShaderReadOnly);
        EXPECT_TRUE(frame.Graph().IsExternal(slotTex.Id));

        frame.Execute();
        device->WaitForIdle();

        // The chain scheduled in dependency order.
        EXPECT_LT(ScheduledIndexOf(frame.Graph(), world.Pass.Id),
                  ScheduledIndexOf(frame.Graph(), tonePass.Id));
        EXPECT_LT(ScheduledIndexOf(frame.Graph(), tonePass.Id),
                  ScheduledIndexOf(frame.Graph(), encodePass.Id));

        device->DestroyTexture(slotHandle);
        rs.Shutdown();
    }
    device->Shutdown();
}

// Slice-8c-2a — AddTonemapPassRG declare-level pin: a valid src/dst pair
// declares ONE finalize pass reading src (Sampled) and attaching dst at color
// slot 0 with DontCare/Store; src==dst declines without declaring anything.
// Executing the frame also exercises the real pipeline-variant + draw path
// (the valid-id happy path the slice-5 review flagged as uncovered).
TEST(RenderPipelineDeclareTests, TonemapPassRGDeclaresReadAttachAndDeclinesSelfTarget)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(1);

        TextureDesc srcDesc = ColorTargetDesc();
        srcDesc.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
        srcDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget) |
                        static_cast<uint32_t>(TextureUsage::ShaderResource);
        srcDesc.debugName = "Tonemap.Src";
        const RenderGraph::RGTexture src = frame.CreateTexture("Tonemap.Src", srcDesc);
        const RenderGraph::RGTexture dst = frame.CreateTexture("Tonemap.Dst", ColorTargetDesc());
        ASSERT_TRUE(src.IsValid());
        ASSERT_TRUE(dst.IsValid());

        // Producer for src so the read edge has a real upstream.
        frame.AddPass(
            "Tonemap.Fill", static_cast<int32_t>(PassPhase::kWorldRender),
            [&](RenderGraph::RGPassBuilder& p)
            {
                RenderGraph::RGAttachmentOps ops{};
                ops.Load = RenderGraph::RGLoadOp::Clear;
                ops.Store = RenderGraph::RGStoreOp::Store;
                p.AttachColor(0, src, ops);
            },
            [](RenderGraph::RGContext&) {});

        const size_t passesBefore = frame.Graph().PassCount();
        const RenderGraph::RGPass pass =
            Passes::AddTonemapPassRG(frame, src, dst, Passes::TonemapParams{}, "Tonemap.RGTest");
        if (!pass.IsValid())
            GTEST_SKIP() << "tonemap.shaderpkg unavailable in this environment";
        EXPECT_EQ(frame.Graph().PassCount(), passesBefore + 1);
        EXPECT_TRUE(frame.Graph().HasReadAccess(pass.Id, src.Id))
            << "the tonemap pass reads the HDR source";

        bool foundAttach = false;
        for (const auto& rec : frame.Attachments())
            if (rec.Pass == pass.Id)
            {
                EXPECT_FALSE(rec.IsDepth);
                EXPECT_EQ(rec.Slot, 0u);
                EXPECT_EQ(rec.Tex, dst.Id);
                EXPECT_EQ(rec.Ops.Load, RenderGraph::RGLoadOp::DontCare);
                EXPECT_EQ(rec.Ops.Store, RenderGraph::RGStoreOp::Store);
                foundAttach = true;
            }
        EXPECT_TRUE(foundAttach) << "dst attached at color slot 0";

        // src == dst declines: no pass, no accesses, no attachment.
        const size_t passesAfterValid = frame.Graph().PassCount();
        const RenderGraph::RGPass self =
            Passes::AddTonemapPassRG(frame, src, src, Passes::TonemapParams{}, "Tonemap.Self");
        EXPECT_FALSE(self.IsValid());
        EXPECT_EQ(frame.Graph().PassCount(), passesAfterValid);

        frame.MarkOutput(dst);
        frame.Execute();
        device->WaitForIdle();
    }
    device->Shutdown();
}

// A1.4 S3 — the pipeline hot-reload lock (L7, re-pointed per design §0a-A11). The
// steady-state per-frame pipeline-asset poll is gone; a source change to the
// active .rendergraph must still recompile the blueprint. That path is reachable
// only with an initialized EngineCore (the asset-lookup gate in
// EnsureActiveRenderPipelineBlueprint; every other test in this suite runs
// EngineCore-less and pins its pipeline). So this is the suite's one
// EngineCore-backed integration fixture: it boots a scripting-free EngineCore
// over a temp asset root, resolves a real .rendergraph through the spine, then
// drives reloads two ways.
//
//   POLL detector (primary): the editor inspector saves via a direct in-place
//     RenderPipelineAsset::Reload() with NO event; ReloadAssetNow mirrors that.
//     The cached-asset GetSourceHash() poll is the ONLY possible trigger here.
//   EVENT invalidator (redundant): AssetReloaded flips the refresh flag. Its guid
//     filter is locked in isolation — a reload event for an UNRELATED guid, on an
//     unchanged source, must NOT recompile (steady state makes the poll blind, so
//     only the handler could fire) — then the active guid drives an end-to-end
//     recompile.
TEST(RenderPipelineDeclareTests, ActivePipelineHotReloadRecompilesOffPollAndReloadEvent)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    namespace fs = std::filesystem;
    const fs::path tempRoot =
        fs::temp_directory_path() /
        ("ge_s3_pipe_hotreload_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path pipeRel = fs::path("RenderPipelines") / "S3HotReload.rendergraph";
    const fs::path pipeFile = tempRoot / "Assets" / pipeRel;

    std::error_code mkec;
    fs::create_directories(pipeFile.parent_path(), mkec);
    ASSERT_FALSE(mkec) << "failed to create temp asset dir: " << mkec.message();

    auto writePipe = [&](const char* json)
    {
        std::ofstream ofs(pipeFile, std::ios::binary | std::ios::trunc);
        ofs << json;
    };
    // v1 — minimal valid schema-v2 pipeline (WorldRender is a registered node).
    writePipe(
        R"({"schemaVersion":2,"pipelineName":"S3HotReloadV1","passes":[{"id":"World","type":"WorldRender","enabled":true}],"outputs":{"FinalColor":"View.Resolve"}})");

    // Scripting-free EngineCore: disableClr + all hot-reload/auto-gen off makes
    // scripting "fully disabled", so ScriptManager (CoreCLR host) is skipped and
    // this Engine-only test target boots without a .NET runtime; the AssetManager
    // (the poll-path gate + reload machinery) still comes up.
    ScriptsConfig scriptsConfig{};
    scriptsConfig.disableClr = true;
    scriptsConfig.enableHotReload = false;
    scriptsConfig.enableAsyncHotReload = false;
    scriptsConfig.enableAutoProjectGeneration = false;
    GameEngine::EngineCore::GetInstance().SetScriptsConfig(scriptsConfig);

    ApplicationConfig cfg{};
    cfg.Name = "S3PipelineHotReloadTest";
    cfg.WorkspaceDirectory = tempRoot.string();
    cfg.AssetDirectory = "Assets";
    // Initialize moves the process working directory to the workspace root.
    std::error_code cwdEc;
    const fs::path callerWorkingDirectory = fs::current_path(cwdEc);
    ASSERT_FALSE(cwdEc) << "failed to read the working directory: " << cwdEc.message();
    const bool engineUp = GameEngine::EngineCore::GetInstance().Initialize(cfg);

    // Tear down the engine on every return path before the device. Engine shutdown
    // restores its shader hooks; this fixture owns the working directory and temp files.
    struct EngineGuard
    {
        bool Up;
        fs::path Temp;
        fs::path CallerWorkingDirectory;
        ~EngineGuard()
        {
            if (Up)
                GameEngine::EngineCore::GetInstance().Shutdown();
            std::error_code ec;
            fs::current_path(CallerWorkingDirectory, ec);
            fs::remove_all(Temp, ec);
        }
    } engineGuard{engineUp, tempRoot, callerWorkingDirectory};

    ASSERT_TRUE(engineUp) << "EngineCore failed to initialize (scripting-free config)";
    AssetManager& am = GameEngine::EngineCore::GetInstance().GetAssetManager();

    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        const CameraId camId = rs.Views().AllocateCamera("S3Cam");
        CameraData cd{};
        for (int i = 0; i < 16; i += 5)
        {
            cd.view[i] = 1.0f;
            cd.proj[i] = 1.0f;
            cd.viewProj[i] = 1.0f;
        }
        rs.Views().SetCameraData(camId, cd);
        const ViewId viewId = rs.Views().AllocateView("S3View", camId);
        rs.Views().SetViewRenderLayerMask(viewId, 1u);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);
        ASSERT_NE(rs.GetGPUScene(), nullptr);
        Rendering::GPUInstance inst{};
        inst.boundingRadius = 1.0f;
        inst.flags = ~0u;
        rs.GetGPUScene()->AddInstance(inst);

        rs.Spine().SetActiveRenderPipelinePath(pipeRel);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        GameEngine::Testing::ScopedPipelineFrame frameRegistration(rs.Spine(), frame);
        uint32_t frameIdx = 0;
        auto buildOnce = [&]()
        {
            frame.BeginFrame(frameIdx++);
            RenderGraph::RGTexture color = frame.ImportPersistentTexture("S3.Color", ColorTargetDesc());
            RenderGraph::RGTexture depth = frame.ImportPersistentTexture("S3.Depth", DepthTargetDesc());
            rs.BeginWorldDrawFrame();
            rs.BuildWorldBatchKeys();
            const ViewTargetsRG vt{viewId, color, depth, {}};
            RenderServices::FrameGraphBuildParamsRG params{};
            params.ViewTargets = std::span<const ViewTargetsRG>(&vt, 1);
            rs.Spine().BuildFrameGraph(frame, params);
            frame.Execute();
            device->WaitForIdle();
        };

        // Frame 1: the EngineCore-backed poll path resolves + compiles the real
        // temp asset. The pipeline name proves it's OUR asset, not the engine's
        // ForwardPlus standing in — i.e. the gated lookup ran.
        buildOnce();
        const uint64_t genV1 = rs.Spine().ActiveBlueprintGen();
        ASSERT_GT(genV1, 0u) << "first build must apply a blueprint";
        ASSERT_NE(rs.Spine().ActiveBlueprint(), nullptr);
        ASSERT_EQ(rs.Spine().ActiveBlueprint()->pipelineName, "S3HotReloadV1")
            << "the temp .rendergraph must resolve through the asset path (the "
               "EngineCore-backed poll path is genuinely reached, not the fallback)";

        // Steady state: a second app-frame with no source change does ZERO
        // refresh work — the whole point of the slice. The blueprint gen is
        // stable because the spine no longer re-resolves the asset per frame.
        buildOnce();
        EXPECT_EQ(rs.Spine().ActiveBlueprintGen(), genV1)
            << "unchanged source => no per-frame refresh (blueprint gen stable)";

        // POLL DETECTOR — eventless in-place reload (the inspector-save path).
        // ReloadAssetNow reloads the resident object in place with NO dispatch;
        // the cached-asset source-hash poll is the only thing that can notice.
        writePipe(
            R"({"schemaVersion":2,"pipelineName":"S3HotReloadV2","passes":[{"id":"World","type":"WorldRender","enabled":true}],"outputs":{"FinalColor":"View.Resolve"}})");
        const GUID guid = am.ResolveAssetGuid(pipeRel);
        ASSERT_FALSE(guid.IsNull());
        ASSERT_EQ(am.ReloadAssetNow(guid), ReloadOutcome::Reloaded)
            << "in-place reload of the active pipeline asset";
        buildOnce();
        const uint64_t genV2 = rs.Spine().ActiveBlueprintGen();
        EXPECT_GT(genV2, genV1)
            << "eventless in-place reload must recompile via the cached-asset source-hash poll";
        EXPECT_EQ(rs.Spine().ActiveBlueprint()->pipelineName, "S3HotReloadV2");

        // EVENT GUID FILTER (isolated) — a reload event for an UNRELATED guid, on
        // an unchanged source, must not recompile. Steady state makes the poll
        // blind (cached hash == recorded), so the invalidator handler is the only
        // path that could set the flag; its guid filter rejects the stray guid.
        am.GetEventDispatcher().DispatchEvent(AssetEvents::AssetReloaded(
            GUID::Generate(), AssetType::RenderPipeline, "RenderPipelines/Unrelated.rendergraph"));
        buildOnce();
        EXPECT_EQ(rs.Spine().ActiveBlueprintGen(), genV2)
            << "a reload event for an unrelated guid must not recompile the active pipeline";

        // EVENT PATH (end-to-end) — the active guid's AssetReloaded flips the
        // refresh flag; with an in-place reload behind it the blueprint
        // recompiles on the next spine call (poll + invalidator both active here,
        // which is exactly the design's redundancy).
        writePipe(
            R"({"schemaVersion":2,"pipelineName":"S3HotReloadV3","passes":[{"id":"World","type":"WorldRender","enabled":true}],"outputs":{"FinalColor":"View.Resolve"}})");
        ASSERT_EQ(am.ReloadAssetNow(guid), ReloadOutcome::Reloaded);
        am.GetEventDispatcher().DispatchEvent(
            AssetEvents::AssetReloaded(guid, AssetType::RenderPipeline, pipeFile.string()));
        buildOnce();
        EXPECT_GT(rs.Spine().ActiveBlueprintGen(), genV2)
            << "AssetReloaded for the active guid drives the recompile";
        EXPECT_EQ(rs.Spine().ActiveBlueprint()->pipelineName, "S3HotReloadV3");

        rs.Shutdown();
    }
    device->Shutdown();
}

// ---------------------------------------------------------------------------
// Terrain GPU height-bake device oracle (slice 1).
//
// Dispatches terrain_height_bake.comp on a real Vulkan device into a single-slot
// R32F atlas image (write-through-indirection), reads the slot back, and pins it to:
//   * the CPU base value-noise fill (HeightfieldData::FillWithNoiseWorldSpace) within
//     epsilon — the design "Risk 1" CPU<->GPU noise-parity headline;
//   * a Flatten modifier reference (ComputeWeight circle + Set blend) within epsilon;
//   * itself across two identical runs, bit-for-bit (the C3 hash-stability pattern).
// Skips cleanly where no device / shader is available (TDR-degraded or headless CI).
// ---------------------------------------------------------------------------
namespace
{
using namespace GameEngine::TerrainECS;

// IEEE 754 float16 -> float32 (mirror of Ocean's HalfToFloat / the inverse of
// TerrainService.cpp FloatToHalf). Used to unpack the RG16F normal atlas readback.
float HalfToFloatOracle(uint16_t h)
{
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t man = h & 0x3FFu;
    uint32_t f;
    if (exp == 0u)
    {
        if (man == 0u)
        {
            f = sign; // +/- zero
        }
        else
        {
            exp = 1u; // subnormal: normalize into a float32 exponent
            while ((man & 0x400u) == 0u)
            {
                man <<= 1;
                --exp;
            }
            man &= 0x3FFu;
            f = sign | ((exp + 112u) << 23) | (man << 13);
        }
    }
    else if (exp == 0x1Fu)
    {
        f = sign | 0x7F800000u | (man << 13); // inf / nan
    }
    else
    {
        f = sign | ((exp + 112u) << 23) | (man << 13); // 15->127 bias diff = 112
    }
    float out;
    std::memcpy(&out, &f, sizeof(out));
    return out;
}

// One height+normal bake's readback: the R32F height atlas plus the raw RG16F normal
// atlas (nx, nz interleaved, one uint16 pair per texel). Raw halves are kept so the
// boundary oracle can assert bit-equality on the exact stored bit patterns.
struct NormalBakeResult
{
    std::vector<float> Height;        // kAtlasDim^2, R32F
    std::vector<uint16_t> NormalHalf; // kAtlasDim^2 * 2, RG16F (nx, nz)
};

// One height+splat bake's readback: the R32F height atlas plus the raw RGBA8 splat atlas
// (grass, rock, dirt, snow — 4 bytes per texel). Raw bytes kept so the oracles assert on the
// exact stored bytes (the CPU static_cast<uint8> truncation the shader mirrors).
struct SplatBakeResult
{
    std::vector<float> Height;     // kAtlasDim^2, R32F
    std::vector<uint8_t> SplatRGBA; // kAtlasDim^2 * 4, RGBA8
};

// One height+splat pass in an accumulating sequence: the rect+geometry push and the modifier list
// that pass evaluates. Models a drag tick — pass 0 is the initial full-tile bake, later passes are
// region re-bakes over the moved modifier's old-union-new footprint (the eval-skip dispatch).
struct SplatBakePass
{
    TerrainHeightBakePush Push;
    std::vector<ModifierGpu> Modifiers;
};

struct HeightBakeHarness
{
    GameEngine::Rendering::IDevice* Device = nullptr;
    PipelineHandle Pipeline{};
    DescriptorSetLayoutDesc Layout{};
    PipelineHandle NormalPipeline{};
    DescriptorSetLayoutDesc NormalLayout{};
    PipelineHandle SplatPipeline{};
    DescriptorSetLayoutDesc SplatLayout{};

    // tileRes interior samples, 1-texel apron -> slotStride = tileRes + 2, single slot.
    static constexpr uint32_t kTileRes = 32u;
    static constexpr uint32_t kSlotStride = kTileRes + 2u; // apron = 1
    static constexpr uint32_t kAtlasDim = kSlotStride;     // slotsPerRow = 1

    bool Init(GameEngine::Rendering::IDevice* device, std::string& skipReason)
    {
        Device = device;
        namespace fs = std::filesystem;
        const fs::path shaderDir = GameEngine::PathUtils::GetExecutableDirectory()
                                   / "Assets" / "Shaders" / "CBT";
        if (!fs::exists(shaderDir / "terrain_height_bake.comp"))
        {
            skipReason = "terrain_height_bake.comp not staged at " + shaderDir.string();
            return false;
        }

        ShaderProgramCompileRequest req{};
        req.debugName = "terrain_height_bake_oracle";
        req.baseDirectory = shaderDir;
        req.cacheRoot = fs::path(".Cache") / "Shaders";
        req.includeDirs = {shaderDir};
        req.stages = {{"cs", "terrain_height_bake.comp", "main", {}}};
        ShaderProgramCompileResult result{};
        std::string err;
        if (!ShaderCompileService::CompileProgramToCache(req, ShaderSourceKind::SpirV, result, &err))
        {
            skipReason = "shader compile failed: " + err;
            return false;
        }
        auto itCs = result.stageBytes.find("cs");
        if (itCs == result.stageBytes.end())
        {
            skipReason = "no compute SPIR-V produced";
            return false;
        }

        Layout.debugName = "HeightBakeOracle_DSLayout";
        DescriptorBinding mods{};
        mods.binding = 0u;
        mods.type = DescriptorType::StorageBuffer;
        mods.count = 1u;
        mods.shaderStages = kShaderStageCompute;
        DescriptorBinding img{};
        img.binding = 1u;
        img.type = DescriptorType::StorageImage;
        img.count = 1u;
        img.shaderStages = kShaderStageCompute;
        Layout.bindings = {mods, img};

        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(itCs->second));
        cd.DescriptorSetLayouts.push_back(Device->InternDescriptorSetLayout(Layout));
        cd.PushConstants.Size = sizeof(TerrainHeightBakePush);
        cd.PushConstants.StageMask = kShaderStageCompute;
        cd.DebugName = "HeightBakeOracle_Pipeline";
        Pipeline = Device->GetOrCreateComputePipeline(Device->InternComputePipeline(cd));
        return Pipeline.IsValid();
    }

    // Compile the KERNEL_NORMAL variant of the same shader and build the normal-derive
    // pipeline (height storage image in, RG16F normal storage image out). Called after Init
    // by the slice-2 oracles; the height oracles don't need it.
    bool InitNormal(std::string& skipReason)
    {
        namespace fs = std::filesystem;
        const fs::path shaderDir = GameEngine::PathUtils::GetExecutableDirectory()
                                   / "Assets" / "Shaders" / "CBT";
        ShaderProgramCompileRequest req{};
        req.debugName = "terrain_normal_bake_oracle";
        req.baseDirectory = shaderDir;
        req.cacheRoot = fs::path(".Cache") / "Shaders";
        req.includeDirs = {shaderDir};
        req.stages = {{"cs", "terrain_height_bake.comp", "main", {"KERNEL_NORMAL"}}};
        ShaderProgramCompileResult result{};
        std::string err;
        if (!ShaderCompileService::CompileProgramToCache(req, ShaderSourceKind::SpirV, result, &err))
        {
            skipReason = "normal shader compile failed: " + err;
            return false;
        }
        auto itCs = result.stageBytes.find("cs");
        if (itCs == result.stageBytes.end())
        {
            skipReason = "no KERNEL_NORMAL SPIR-V produced";
            return false;
        }

        NormalLayout.debugName = "NormalBakeOracle_DSLayout";
        DescriptorBinding heightRead{};
        heightRead.binding = 0u;
        heightRead.type = DescriptorType::StorageImage;
        heightRead.count = 1u;
        heightRead.shaderStages = kShaderStageCompute;
        DescriptorBinding normalWrite{};
        normalWrite.binding = 1u;
        normalWrite.type = DescriptorType::StorageImage;
        normalWrite.count = 1u;
        normalWrite.shaderStages = kShaderStageCompute;
        NormalLayout.bindings = {heightRead, normalWrite};

        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(itCs->second));
        cd.DescriptorSetLayouts.push_back(Device->InternDescriptorSetLayout(NormalLayout));
        cd.PushConstants.Size = sizeof(TerrainHeightBakePush);
        cd.PushConstants.StageMask = kShaderStageCompute;
        cd.DebugName = "NormalBakeOracle_Pipeline";
        NormalPipeline = Device->GetOrCreateComputePipeline(Device->InternComputePipeline(cd));
        return NormalPipeline.IsValid();
    }

    // Compile the KERNEL_SPLAT variant and build the splat-derive pipeline (height storage image
    // in, RGBA8 splat storage image out). Called after Init by the slice-3 oracles.
    bool InitSplat(std::string& skipReason)
    {
        namespace fs = std::filesystem;
        const fs::path shaderDir = GameEngine::PathUtils::GetExecutableDirectory()
                                   / "Assets" / "Shaders" / "CBT";
        ShaderProgramCompileRequest req{};
        req.debugName = "terrain_splat_bake_oracle";
        req.baseDirectory = shaderDir;
        req.cacheRoot = fs::path(".Cache") / "Shaders";
        req.includeDirs = {shaderDir};
        req.stages = {{"cs", "terrain_height_bake.comp", "main", {"KERNEL_SPLAT"}}};
        ShaderProgramCompileResult result{};
        std::string err;
        if (!ShaderCompileService::CompileProgramToCache(req, ShaderSourceKind::SpirV, result, &err))
        {
            skipReason = "splat shader compile failed: " + err;
            return false;
        }
        auto itCs = result.stageBytes.find("cs");
        if (itCs == result.stageBytes.end())
        {
            skipReason = "no KERNEL_SPLAT SPIR-V produced";
            return false;
        }

        SplatLayout.debugName = "SplatBakeOracle_DSLayout";
        DescriptorBinding heightRead{};
        heightRead.binding = 0u;
        heightRead.type = DescriptorType::StorageImage;
        heightRead.count = 1u;
        heightRead.shaderStages = kShaderStageCompute;
        DescriptorBinding splatWrite{};
        splatWrite.binding = 1u;
        splatWrite.type = DescriptorType::StorageImage;
        splatWrite.count = 1u;
        splatWrite.shaderStages = kShaderStageCompute;
        // Surface rule rows + their conditions. The kernel references both statically, so they
        // belong in the layout whether or not a given bake carries any rules.
        DescriptorBinding ruleRows{};
        ruleRows.binding = 2u;
        ruleRows.type = DescriptorType::StorageBuffer;
        ruleRows.count = 1u;
        ruleRows.shaderStages = kShaderStageCompute;
        DescriptorBinding ruleConditions{};
        ruleConditions.binding = 3u;
        ruleConditions.type = DescriptorType::StorageBuffer;
        ruleConditions.count = 1u;
        ruleConditions.shaderStages = kShaderStageCompute;
        SplatLayout.bindings = {heightRead, splatWrite, ruleRows, ruleConditions};

        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(itCs->second));
        cd.DescriptorSetLayouts.push_back(Device->InternDescriptorSetLayout(SplatLayout));
        cd.PushConstants.Size = sizeof(TerrainHeightBakePush);
        cd.PushConstants.StageMask = kShaderStageCompute;
        cd.DebugName = "SplatBakeOracle_Pipeline";
        SplatPipeline = Device->GetOrCreateComputePipeline(Device->InternComputePipeline(cd));
        return SplatPipeline.IsValid();
    }

    // Bake one tile into slot 0 and read the whole atlas (kAtlasDim^2 texels) back.
    std::vector<float> Bake(const TerrainHeightBakePush& push,
                            const std::vector<ModifierGpu>& modifiers)
    {
        TextureDesc td{};
        td.width = kAtlasDim;
        td.height = kAtlasDim;
        td.depth = 1u;
        td.mipLevels = 1u;
        td.arrayLayers = 1u;
        td.sampleCount = 1u;
        td.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
        td.usage = static_cast<uint32_t>(TextureUsage::UnorderedAccess | TextureUsage::ShaderResource |
                                         TextureUsage::TransferSrc | TextureUsage::TransferDst);
        td.debugName = "HeightBakeOracle.Atlas";
        TextureHandle tex = Device->CreateTexture(td);

        TextureViewDesc vd{};
        vd.viewType = TextureViewType::View2D;
        vd.baseMip = 0u;
        vd.levelCount = 1u;
        vd.baseLayer = 0u;
        vd.layerCount = 1u;
        vd.debugName = "HeightBakeOracle.AtlasView";
        TextureViewHandle view = Device->CreateTextureView(tex, vd);

        // Modifier SSBO (>=1 element so the binding is never zero-sized).
        const size_t modCount = std::max<size_t>(1u, modifiers.size());
        BufferDesc bd{};
        bd.size = modCount * sizeof(ModifierGpu);
        bd.usage = static_cast<uint32_t>(BufferUsage::Storage);
        bd.memoryUsage = BufferMemoryUsage::Upload;
        bd.debugName = "HeightBakeOracle.Modifiers";
        BufferHandle modBuf = Device->CreateBuffer(bd);
        if (void* p = Device->MapBuffer(modBuf))
        {
            std::memset(p, 0, bd.size);
            if (!modifiers.empty())
                std::memcpy(p, modifiers.data(), modifiers.size() * sizeof(ModifierGpu));
            Device->UnmapBuffer(modBuf);
        }

        // Zero the atlas first so unwritten apron/corner texels are deterministic.
        std::vector<float> zeros(static_cast<size_t>(kAtlasDim) * kAtlasDim, 0.0f);
        BufferDesc zd{};
        zd.size = zeros.size() * sizeof(float);
        zd.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
        zd.memoryUsage = BufferMemoryUsage::Upload;
        zd.debugName = "HeightBakeOracle.Zero";
        BufferHandle zeroBuf = Device->CreateBuffer(zd);
        if (void* p = Device->MapBuffer(zeroBuf))
        {
            std::memcpy(p, zeros.data(), zd.size);
            Device->UnmapBuffer(zeroBuf);
        }

        BufferHandle readback =
            Device->CreateReadbackBuffer(static_cast<size_t>(kAtlasDim) * kAtlasDim * sizeof(float));

        auto cl = Device->CreateCommandList(GameEngine::Rendering::IDevice::QueueType::Graphics);
        cl->Begin();
        cl->CopyBufferToTextureSubresource(zeroBuf, tex, 0u, 0u, kAtlasDim, kAtlasDim);
        // CopyDest -> storage: journals VK_IMAGE_LAYOUT_GENERAL and orders transfer -> compute.
        TransitionForStorageAccess(cl.get(), tex, ResourceState::CopyDest, 0u, 1u);

        DescriptorSetDesc dsDesc{};
        dsDesc.layout = Layout;
        dsDesc.transient = true;
        dsDesc.debugName = "HeightBakeOracle.DS";
        DescriptorSetHandle ds = Device->CreateDescriptorSet(dsDesc);
        Device->UpdateStorageBufferBinding(ds, 0u, modBuf, 0u, bd.size);
        Device->UpdateStorageImageBinding(ds, 1u, view);

        cl->SetPipeline(Pipeline);
        cl->BindDescriptorSet(0u, ds, Pipeline);
        cl->SetPushConstants(push);
        const uint32_t total = push.RectW * push.RectH;
        cl->Dispatch((total + 63u) / 64u, 1u, 1u);

        ResourceBarrier rb = ResourceBarrier::CreateMemoryBarrier(
            static_cast<uint64_t>(PipelineStageMask::ComputeShader),
            static_cast<uint64_t>(PipelineStageMask::Transfer),
            static_cast<uint64_t>(ResourceAccessMask::ShaderWrite),
            static_cast<uint64_t>(ResourceAccessMask::TransferRead));
        cl->Barrier(rb);
        cl->CopyTextureSubresourceToBuffer(tex, 0u, 0u, readback, kAtlasDim, kAtlasDim, 0, 0, 0, 0);
        cl->End();
        Device->ExecuteCommandLists({cl.get()});
        Device->FinalizeFrame();
        Device->WaitForIdle();

        std::vector<float> out(static_cast<size_t>(kAtlasDim) * kAtlasDim, 0.0f);
        if (const void* p = Device->MapBuffer(readback))
        {
            std::memcpy(out.data(), p, out.size() * sizeof(float));
            Device->UnmapBuffer(readback);
        }

        Device->DestroyTextureView(view);
        Device->DestroyTexture(tex);
        Device->DestroyBuffer(modBuf);
        Device->DestroyBuffer(zeroBuf);
        Device->DestroyBuffer(readback);
        return out;
    }

    // Bake height (slice 1) then derive normals (slice 2) in one command list, ordered by
    // the same height-write -> normal-read memory barrier the live FlushPendingBakes uses.
    // Reads back both the R32F height atlas and the RG16F normal atlas. Requires InitNormal.
    NormalBakeResult BakeHeightAndNormal(const TerrainHeightBakePush& push,
                                         const std::vector<ModifierGpu>& modifiers)
    {
        const uint32_t texels = kAtlasDim * kAtlasDim;

        // Height atlas (R32F) — storage write target of the height pass, storage read source
        // of the normal pass.
        TextureDesc htd{};
        htd.width = kAtlasDim;
        htd.height = kAtlasDim;
        htd.depth = 1u;
        htd.mipLevels = 1u;
        htd.arrayLayers = 1u;
        htd.sampleCount = 1u;
        htd.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
        htd.usage = static_cast<uint32_t>(TextureUsage::UnorderedAccess | TextureUsage::ShaderResource |
                                          TextureUsage::TransferSrc | TextureUsage::TransferDst);
        htd.debugName = "NormalBakeOracle.Height";
        TextureHandle htex = Device->CreateTexture(htd);

        TextureDesc ntd = htd;
        ntd.format = static_cast<uint32_t>(TextureFormat::R16G16_FLOAT);
        ntd.debugName = "NormalBakeOracle.Normal";
        TextureHandle ntex = Device->CreateTexture(ntd);

        TextureViewDesc vd{};
        vd.viewType = TextureViewType::View2D;
        vd.baseMip = 0u;
        vd.levelCount = 1u;
        vd.baseLayer = 0u;
        vd.layerCount = 1u;
        vd.debugName = "NormalBakeOracle.HeightView";
        TextureViewHandle hview = Device->CreateTextureView(htex, vd);
        vd.debugName = "NormalBakeOracle.NormalView";
        TextureViewHandle nview = Device->CreateTextureView(ntex, vd);

        // Modifier SSBO (>=1 element so the binding is never zero-sized).
        const size_t modCount = std::max<size_t>(1u, modifiers.size());
        BufferDesc bd{};
        bd.size = modCount * sizeof(ModifierGpu);
        bd.usage = static_cast<uint32_t>(BufferUsage::Storage);
        bd.memoryUsage = BufferMemoryUsage::Upload;
        bd.debugName = "NormalBakeOracle.Modifiers";
        BufferHandle modBuf = Device->CreateBuffer(bd);
        if (void* p = Device->MapBuffer(modBuf))
        {
            std::memset(p, 0, bd.size);
            if (!modifiers.empty())
                std::memcpy(p, modifiers.data(), modifiers.size() * sizeof(ModifierGpu));
            Device->UnmapBuffer(modBuf);
        }

        // Zero both atlases so any texel a full-tile bake does not touch is deterministic.
        std::vector<uint8_t> zeros(static_cast<size_t>(texels) * 4u, 0u); // 4 bytes/texel for R32F and RG16F
        BufferDesc zd{};
        zd.size = zeros.size();
        zd.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
        zd.memoryUsage = BufferMemoryUsage::Upload;
        zd.debugName = "NormalBakeOracle.Zero";
        BufferHandle zeroBuf = Device->CreateBuffer(zd);
        if (void* p = Device->MapBuffer(zeroBuf))
        {
            std::memcpy(p, zeros.data(), zd.size);
            Device->UnmapBuffer(zeroBuf);
        }

        BufferHandle heightReadback = Device->CreateReadbackBuffer(static_cast<size_t>(texels) * sizeof(float));
        BufferHandle normalReadback = Device->CreateReadbackBuffer(static_cast<size_t>(texels) * 2u * sizeof(uint16_t));

        auto cl = Device->CreateCommandList(GameEngine::Rendering::IDevice::QueueType::Graphics);
        cl->Begin();
        cl->CopyBufferToTextureSubresource(zeroBuf, htex, 0u, 0u, kAtlasDim, kAtlasDim);
        cl->CopyBufferToTextureSubresource(zeroBuf, ntex, 0u, 0u, kAtlasDim, kAtlasDim);
        // CopyDest -> storage: journals VK_IMAGE_LAYOUT_GENERAL and orders transfer -> compute.
        TransitionForStorageAccess(cl.get(), htex, ResourceState::CopyDest, 0u, 1u);
        TransitionForStorageAccess(cl.get(), ntex, ResourceState::CopyDest, 0u, 1u);

        // Height pass.
        DescriptorSetDesc hDesc{};
        hDesc.layout = Layout;
        hDesc.transient = true;
        hDesc.debugName = "NormalBakeOracle.HeightDS";
        DescriptorSetHandle hds = Device->CreateDescriptorSet(hDesc);
        Device->UpdateStorageBufferBinding(hds, 0u, modBuf, 0u, bd.size);
        Device->UpdateStorageImageBinding(hds, 1u, hview);

        cl->SetPipeline(Pipeline);
        cl->BindDescriptorSet(0u, hds, Pipeline);
        cl->SetPushConstants(push);
        const uint32_t total = push.RectW * push.RectH;
        cl->Dispatch((total + 63u) / 64u, 1u, 1u);

        // Height-write -> normal-read ordering: the exact memory barrier FlushPendingBakes
        // inserts between the two dispatches (an image dependency, not a CPU fence).
        cl->Barrier(ResourceBarrier::CreateMemoryBarrier(
            static_cast<uint64_t>(PipelineStageMask::ComputeShader),
            static_cast<uint64_t>(PipelineStageMask::ComputeShader),
            static_cast<uint64_t>(ResourceAccessMask::ShaderWrite),
            static_cast<uint64_t>(ResourceAccessMask::ShaderRead)));

        // Normal pass: reads the just-written height storage image, writes RG16F normals.
        DescriptorSetDesc nDesc{};
        nDesc.layout = NormalLayout;
        nDesc.transient = true;
        nDesc.debugName = "NormalBakeOracle.NormalDS";
        DescriptorSetHandle nds = Device->CreateDescriptorSet(nDesc);
        Device->UpdateStorageImageBinding(nds, 0u, hview);
        Device->UpdateStorageImageBinding(nds, 1u, nview);

        cl->SetPipeline(NormalPipeline);
        cl->BindDescriptorSet(0u, nds, NormalPipeline);
        cl->SetPushConstants(push);
        cl->Dispatch((total + 63u) / 64u, 1u, 1u);

        // Make all shader writes (height + normal) visible to the readback copies.
        cl->Barrier(ResourceBarrier::CreateMemoryBarrier(
            static_cast<uint64_t>(PipelineStageMask::ComputeShader),
            static_cast<uint64_t>(PipelineStageMask::Transfer),
            static_cast<uint64_t>(ResourceAccessMask::ShaderWrite),
            static_cast<uint64_t>(ResourceAccessMask::TransferRead)));
        cl->CopyTextureSubresourceToBuffer(htex, 0u, 0u, heightReadback, kAtlasDim, kAtlasDim, 0, 0, 0, 0);
        cl->CopyTextureSubresourceToBuffer(ntex, 0u, 0u, normalReadback, kAtlasDim, kAtlasDim, 0, 0, 0, 0);
        cl->End();
        Device->ExecuteCommandLists({cl.get()});
        Device->FinalizeFrame();
        Device->WaitForIdle();

        NormalBakeResult out;
        out.Height.assign(texels, 0.0f);
        out.NormalHalf.assign(static_cast<size_t>(texels) * 2u, 0u);
        if (const void* p = Device->MapBuffer(heightReadback))
        {
            std::memcpy(out.Height.data(), p, out.Height.size() * sizeof(float));
            Device->UnmapBuffer(heightReadback);
        }
        if (const void* p = Device->MapBuffer(normalReadback))
        {
            std::memcpy(out.NormalHalf.data(), p, out.NormalHalf.size() * sizeof(uint16_t));
            Device->UnmapBuffer(normalReadback);
        }

        Device->DestroyTextureView(hview);
        Device->DestroyTextureView(nview);
        Device->DestroyTexture(htex);
        Device->DestroyTexture(ntex);
        Device->DestroyBuffer(modBuf);
        Device->DestroyBuffer(zeroBuf);
        Device->DestroyBuffer(heightReadback);
        Device->DestroyBuffer(normalReadback);
        return out;
    }

    // Bake height (slice 1) then derive the procedural splat (slice 3) in one command list,
    // ordered by the same height-write -> splat-read memory barrier the live FlushPendingBakes
    // shares between the normal and splat passes. `splatMinH`/`splatMaxH` are the committed global
    // range the splat kernel normalizes altitude against. Reads back R32F height + RGBA8 splat.
    // An upload SSBO holding `items`, never zero-sized (a zero-sized binding is invalid), zero
    // filled when the vector is empty. Used for the surface-rule row and condition buffers, which
    // the splat kernel binds on every dispatch even when the bake carries no rules.
    template <typename T>
    BufferHandle MakeRuleBuffer(const std::vector<T>& items, const char* debugName, size_t& outBytes)
    {
        outBytes = std::max<size_t>(1u, items.size()) * sizeof(T);
        BufferDesc bd{};
        bd.size = outBytes;
        bd.usage = static_cast<uint32_t>(BufferUsage::Storage);
        bd.memoryUsage = BufferMemoryUsage::Upload;
        bd.debugName = debugName;
        BufferHandle buf = Device->CreateBuffer(bd);
        if (void* p = Device->MapBuffer(buf))
        {
            std::memset(p, 0, outBytes);
            if (!items.empty())
                std::memcpy(p, items.data(), items.size() * sizeof(T));
            Device->UnmapBuffer(buf);
        }
        return buf;
    }

    // Requires InitSplat.
    SplatBakeResult BakeHeightAndSplat(const TerrainHeightBakePush& pushIn,
                                       const std::vector<ModifierGpu>& modifiers,
                                       float splatMinH, float splatMaxH,
                                       const std::vector<SurfaceRuleGpu>& surfaceRules = {},
                                       const std::vector<SurfaceRuleConditionGpu>& surfaceRuleConditions = {})
    {
        const uint32_t texels = kAtlasDim * kAtlasDim;

        TextureDesc htd{};
        htd.width = kAtlasDim;
        htd.height = kAtlasDim;
        htd.depth = 1u;
        htd.mipLevels = 1u;
        htd.arrayLayers = 1u;
        htd.sampleCount = 1u;
        htd.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
        htd.usage = static_cast<uint32_t>(TextureUsage::UnorderedAccess | TextureUsage::ShaderResource |
                                          TextureUsage::TransferSrc | TextureUsage::TransferDst);
        htd.debugName = "SplatBakeOracle.Height";
        TextureHandle htex = Device->CreateTexture(htd);

        TextureDesc std_ = htd;
        std_.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
        std_.debugName = "SplatBakeOracle.Splat";
        TextureHandle stex = Device->CreateTexture(std_);

        TextureViewDesc vd{};
        vd.viewType = TextureViewType::View2D;
        vd.baseMip = 0u;
        vd.levelCount = 1u;
        vd.baseLayer = 0u;
        vd.layerCount = 1u;
        vd.debugName = "SplatBakeOracle.HeightView";
        TextureViewHandle hview = Device->CreateTextureView(htex, vd);
        vd.debugName = "SplatBakeOracle.SplatView";
        TextureViewHandle sview = Device->CreateTextureView(stex, vd);

        const size_t modCount = std::max<size_t>(1u, modifiers.size());
        BufferDesc bd{};
        bd.size = modCount * sizeof(ModifierGpu);
        bd.usage = static_cast<uint32_t>(BufferUsage::Storage);
        bd.memoryUsage = BufferMemoryUsage::Upload;
        bd.debugName = "SplatBakeOracle.Modifiers";
        BufferHandle modBuf = Device->CreateBuffer(bd);
        if (void* p = Device->MapBuffer(modBuf))
        {
            std::memset(p, 0, bd.size);
            if (!modifiers.empty())
                std::memcpy(p, modifiers.data(), modifiers.size() * sizeof(ModifierGpu));
            Device->UnmapBuffer(modBuf);
        }

        // Zero both atlases so any texel a full-tile bake does not touch is deterministic (4
        // bytes/texel for R32F and RGBA8 alike).
        std::vector<uint8_t> zeros(static_cast<size_t>(texels) * 4u, 0u);
        BufferDesc zd{};
        zd.size = zeros.size();
        zd.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
        zd.memoryUsage = BufferMemoryUsage::Upload;
        zd.debugName = "SplatBakeOracle.Zero";
        BufferHandle zeroBuf = Device->CreateBuffer(zd);
        if (void* p = Device->MapBuffer(zeroBuf))
        {
            std::memcpy(p, zeros.data(), zd.size);
            Device->UnmapBuffer(zeroBuf);
        }

        BufferHandle heightReadback = Device->CreateReadbackBuffer(static_cast<size_t>(texels) * sizeof(float));
        BufferHandle splatReadback = Device->CreateReadbackBuffer(static_cast<size_t>(texels) * 4u);

        // Height push (no range), splat push (same rect + committed range in SplatMinH/MaxH).
        TerrainHeightBakePush hpush = pushIn;
        hpush.SplatMinH = 0.0f;
        hpush.SplatMaxH = 0.0f;
        TerrainHeightBakePush spush = pushIn;
        spush.SplatMinH = splatMinH;
        spush.SplatMaxH = splatMaxH;
        spush.SurfaceRuleCount = static_cast<uint32_t>(surfaceRules.size());

        size_t ruleBytes = 0, condBytes = 0;
        BufferHandle ruleBuf = MakeRuleBuffer(surfaceRules, "SplatBakeOracle.SurfaceRules", ruleBytes);
        BufferHandle condBuf =
            MakeRuleBuffer(surfaceRuleConditions, "SplatBakeOracle.SurfaceRuleConditions", condBytes);

        auto cl = Device->CreateCommandList(GameEngine::Rendering::IDevice::QueueType::Graphics);
        cl->Begin();
        cl->CopyBufferToTextureSubresource(zeroBuf, htex, 0u, 0u, kAtlasDim, kAtlasDim);
        cl->CopyBufferToTextureSubresource(zeroBuf, stex, 0u, 0u, kAtlasDim, kAtlasDim);
        // CopyDest -> storage: journals VK_IMAGE_LAYOUT_GENERAL and orders transfer -> compute.
        TransitionForStorageAccess(cl.get(), htex, ResourceState::CopyDest, 0u, 1u);
        TransitionForStorageAccess(cl.get(), stex, ResourceState::CopyDest, 0u, 1u);

        // Height pass.
        DescriptorSetDesc hDesc{};
        hDesc.layout = Layout;
        hDesc.transient = true;
        hDesc.debugName = "SplatBakeOracle.HeightDS";
        DescriptorSetHandle hds = Device->CreateDescriptorSet(hDesc);
        Device->UpdateStorageBufferBinding(hds, 0u, modBuf, 0u, bd.size);
        Device->UpdateStorageImageBinding(hds, 1u, hview);

        cl->SetPipeline(Pipeline);
        cl->BindDescriptorSet(0u, hds, Pipeline);
        cl->SetPushConstants(hpush);
        const uint32_t total = hpush.RectW * hpush.RectH;
        cl->Dispatch((total + 63u) / 64u, 1u, 1u);

        // Height-write -> splat-read ordering (the shared post-height memory barrier).
        cl->Barrier(ResourceBarrier::CreateMemoryBarrier(
            static_cast<uint64_t>(PipelineStageMask::ComputeShader),
            static_cast<uint64_t>(PipelineStageMask::ComputeShader),
            static_cast<uint64_t>(ResourceAccessMask::ShaderWrite),
            static_cast<uint64_t>(ResourceAccessMask::ShaderRead)));

        // Splat pass: reads the just-written height storage image, writes RGBA8 splat.
        DescriptorSetDesc sDesc{};
        sDesc.layout = SplatLayout;
        sDesc.transient = true;
        sDesc.debugName = "SplatBakeOracle.SplatDS";
        DescriptorSetHandle sds = Device->CreateDescriptorSet(sDesc);
        Device->UpdateStorageImageBinding(sds, 0u, hview);
        Device->UpdateStorageImageBinding(sds, 1u, sview);
        Device->UpdateStorageBufferBinding(sds, 2u, ruleBuf, 0u, ruleBytes);
        Device->UpdateStorageBufferBinding(sds, 3u, condBuf, 0u, condBytes);

        cl->SetPipeline(SplatPipeline);
        cl->BindDescriptorSet(0u, sds, SplatPipeline);
        cl->SetPushConstants(spush);
        cl->Dispatch((total + 63u) / 64u, 1u, 1u);

        cl->Barrier(ResourceBarrier::CreateMemoryBarrier(
            static_cast<uint64_t>(PipelineStageMask::ComputeShader),
            static_cast<uint64_t>(PipelineStageMask::Transfer),
            static_cast<uint64_t>(ResourceAccessMask::ShaderWrite),
            static_cast<uint64_t>(ResourceAccessMask::TransferRead)));
        cl->CopyTextureSubresourceToBuffer(htex, 0u, 0u, heightReadback, kAtlasDim, kAtlasDim, 0, 0, 0, 0);
        cl->CopyTextureSubresourceToBuffer(stex, 0u, 0u, splatReadback, kAtlasDim, kAtlasDim, 0, 0, 0, 0);
        cl->End();
        Device->ExecuteCommandLists({cl.get()});
        Device->FinalizeFrame();
        Device->WaitForIdle();

        SplatBakeResult out;
        out.Height.assign(texels, 0.0f);
        out.SplatRGBA.assign(static_cast<size_t>(texels) * 4u, 0u);
        if (const void* p = Device->MapBuffer(heightReadback))
        {
            std::memcpy(out.Height.data(), p, out.Height.size() * sizeof(float));
            Device->UnmapBuffer(heightReadback);
        }
        if (const void* p = Device->MapBuffer(splatReadback))
        {
            std::memcpy(out.SplatRGBA.data(), p, out.SplatRGBA.size());
            Device->UnmapBuffer(splatReadback);
        }

        Device->DestroyTextureView(hview);
        Device->DestroyTextureView(sview);
        Device->DestroyTexture(htex);
        Device->DestroyTexture(stex);
        Device->DestroyBuffer(modBuf);
        Device->DestroyBuffer(ruleBuf);
        Device->DestroyBuffer(condBuf);
        Device->DestroyBuffer(zeroBuf);
        Device->DestroyBuffer(heightReadback);
        Device->DestroyBuffer(splatReadback);
        return out;
    }

    // Accumulate a SEQUENCE of height+splat passes into ONE atlas (zeroed once), then read the
    // whole atlas back. Models the moving-modifier drag: the initial full-tile bake followed by
    // region re-bakes over the old-union-new footprint, so a later pass's rect leaves the texels
    // outside it holding the earlier pass's content (exactly the eval-skip atlas-write model). Every
    // pass's splat step normalizes against the same committed range (splatMinH/MaxH).
    SplatBakeResult BakeHeightAndSplatSequence(const std::vector<SplatBakePass>& passes,
                                               float splatMinH, float splatMaxH,
                                               const std::vector<SurfaceRuleGpu>& surfaceRules = {},
                                               const std::vector<SurfaceRuleConditionGpu>& surfaceRuleConditions = {})
    {
        const uint32_t texels = kAtlasDim * kAtlasDim;

        TextureDesc htd{};
        htd.width = kAtlasDim;
        htd.height = kAtlasDim;
        htd.depth = 1u;
        htd.mipLevels = 1u;
        htd.arrayLayers = 1u;
        htd.sampleCount = 1u;
        htd.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
        htd.usage = static_cast<uint32_t>(TextureUsage::UnorderedAccess | TextureUsage::ShaderResource |
                                          TextureUsage::TransferSrc | TextureUsage::TransferDst);
        htd.debugName = "SplatSeqOracle.Height";
        TextureHandle htex = Device->CreateTexture(htd);

        TextureDesc std_ = htd;
        std_.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
        std_.debugName = "SplatSeqOracle.Splat";
        TextureHandle stex = Device->CreateTexture(std_);

        TextureViewDesc vd{};
        vd.viewType = TextureViewType::View2D;
        vd.baseMip = 0u;
        vd.levelCount = 1u;
        vd.baseLayer = 0u;
        vd.layerCount = 1u;
        vd.debugName = "SplatSeqOracle.HeightView";
        TextureViewHandle hview = Device->CreateTextureView(htex, vd);
        vd.debugName = "SplatSeqOracle.SplatView";
        TextureViewHandle sview = Device->CreateTextureView(stex, vd);

        std::vector<uint8_t> zeros(static_cast<size_t>(texels) * 4u, 0u);
        BufferDesc zd{};
        zd.size = zeros.size();
        zd.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
        zd.memoryUsage = BufferMemoryUsage::Upload;
        zd.debugName = "SplatSeqOracle.Zero";
        BufferHandle zeroBuf = Device->CreateBuffer(zd);
        if (void* p = Device->MapBuffer(zeroBuf))
        {
            std::memcpy(p, zeros.data(), zd.size);
            Device->UnmapBuffer(zeroBuf);
        }

        // One modifier SSBO per pass (each pass has its own list); kept alive until submit.
        std::vector<BufferHandle> modBufs;
        modBufs.reserve(passes.size());
        for (const auto& pass : passes)
        {
            const size_t modCount = std::max<size_t>(1u, pass.Modifiers.size());
            BufferDesc bd{};
            bd.size = modCount * sizeof(ModifierGpu);
            bd.usage = static_cast<uint32_t>(BufferUsage::Storage);
            bd.memoryUsage = BufferMemoryUsage::Upload;
            bd.debugName = "SplatSeqOracle.Modifiers";
            BufferHandle modBuf = Device->CreateBuffer(bd);
            if (void* p = Device->MapBuffer(modBuf))
            {
                std::memset(p, 0, bd.size);
                if (!pass.Modifiers.empty())
                    std::memcpy(p, pass.Modifiers.data(), pass.Modifiers.size() * sizeof(ModifierGpu));
                Device->UnmapBuffer(modBuf);
            }
            modBufs.push_back(modBuf);
        }

        BufferHandle heightReadback = Device->CreateReadbackBuffer(static_cast<size_t>(texels) * sizeof(float));
        BufferHandle splatReadback = Device->CreateReadbackBuffer(static_cast<size_t>(texels) * 4u);

        // The rule bindings are part of the splat layout, so every pass binds them. Every pass in a
        // drag sequence composites the SAME authored rows (the rows are not what the drag edits),
        // which is what makes the accumulated result comparable to a single full bake.
        size_t seqRuleBytes = 0, seqCondBytes = 0;
        BufferHandle seqRuleBuf =
            MakeRuleBuffer(surfaceRules, "SplatSeqOracle.SurfaceRules", seqRuleBytes);
        BufferHandle seqCondBuf = MakeRuleBuffer(surfaceRuleConditions,
                                                 "SplatSeqOracle.SurfaceRuleConditions", seqCondBytes);

        auto computeToComputeBarrier = [] {
            return ResourceBarrier::CreateMemoryBarrier(
                static_cast<uint64_t>(PipelineStageMask::ComputeShader),
                static_cast<uint64_t>(PipelineStageMask::ComputeShader),
                static_cast<uint64_t>(ResourceAccessMask::ShaderWrite) |
                    static_cast<uint64_t>(ResourceAccessMask::ShaderRead),
                static_cast<uint64_t>(ResourceAccessMask::ShaderWrite) |
                    static_cast<uint64_t>(ResourceAccessMask::ShaderRead));
        };

        auto cl = Device->CreateCommandList(GameEngine::Rendering::IDevice::QueueType::Graphics);
        cl->Begin();
        cl->CopyBufferToTextureSubresource(zeroBuf, htex, 0u, 0u, kAtlasDim, kAtlasDim);
        cl->CopyBufferToTextureSubresource(zeroBuf, stex, 0u, 0u, kAtlasDim, kAtlasDim);
        // CopyDest -> storage: journals VK_IMAGE_LAYOUT_GENERAL and orders transfer -> compute.
        TransitionForStorageAccess(cl.get(), htex, ResourceState::CopyDest, 0u, 1u);
        TransitionForStorageAccess(cl.get(), stex, ResourceState::CopyDest, 0u, 1u);

        for (size_t i = 0; i < passes.size(); ++i)
        {
            const SplatBakePass& pass = passes[i];
            const BufferHandle modBuf = modBufs[i];
            const uint32_t total = pass.Push.RectW * pass.Push.RectH;
            if (total == 0)
                continue;

            // Height pass (no committed range in the push).
            TerrainHeightBakePush hpush = pass.Push;
            hpush.SplatMinH = 0.0f;
            hpush.SplatMaxH = 0.0f;
            DescriptorSetDesc hDesc{};
            hDesc.layout = Layout;
            hDesc.transient = true;
            hDesc.debugName = "SplatSeqOracle.HeightDS";
            DescriptorSetHandle hds = Device->CreateDescriptorSet(hDesc);
            Device->UpdateStorageBufferBinding(hds, 0u, modBuf, 0u, std::max<size_t>(1u, pass.Modifiers.size()) * sizeof(ModifierGpu));
            Device->UpdateStorageImageBinding(hds, 1u, hview);
            cl->SetPipeline(Pipeline);
            cl->BindDescriptorSet(0u, hds, Pipeline);
            cl->SetPushConstants(hpush);
            cl->Dispatch((total + 63u) / 64u, 1u, 1u);

            cl->Barrier(computeToComputeBarrier());

            // Splat pass (committed range in the push), reads the just-written height storage image.
            TerrainHeightBakePush spush = pass.Push;
            spush.SplatMinH = splatMinH;
            spush.SplatMaxH = splatMaxH;
            spush.SurfaceRuleCount = static_cast<uint32_t>(surfaceRules.size());
            DescriptorSetDesc sDesc{};
            sDesc.layout = SplatLayout;
            sDesc.transient = true;
            sDesc.debugName = "SplatSeqOracle.SplatDS";
            DescriptorSetHandle sds = Device->CreateDescriptorSet(sDesc);
            Device->UpdateStorageImageBinding(sds, 0u, hview);
            Device->UpdateStorageImageBinding(sds, 1u, sview);
            Device->UpdateStorageBufferBinding(sds, 2u, seqRuleBuf, 0u, seqRuleBytes);
            Device->UpdateStorageBufferBinding(sds, 3u, seqCondBuf, 0u, seqCondBytes);
            cl->SetPipeline(SplatPipeline);
            cl->BindDescriptorSet(0u, sds, SplatPipeline);
            cl->SetPushConstants(spush);
            cl->Dispatch((total + 63u) / 64u, 1u, 1u);

            // Order the next pass's height write after this pass's splat read (WAR) and make this
            // pass's writes visible to it.
            if (i + 1 < passes.size())
                cl->Barrier(computeToComputeBarrier());
        }

        cl->Barrier(ResourceBarrier::CreateMemoryBarrier(
            static_cast<uint64_t>(PipelineStageMask::ComputeShader),
            static_cast<uint64_t>(PipelineStageMask::Transfer),
            static_cast<uint64_t>(ResourceAccessMask::ShaderWrite),
            static_cast<uint64_t>(ResourceAccessMask::TransferRead)));
        cl->CopyTextureSubresourceToBuffer(htex, 0u, 0u, heightReadback, kAtlasDim, kAtlasDim, 0, 0, 0, 0);
        cl->CopyTextureSubresourceToBuffer(stex, 0u, 0u, splatReadback, kAtlasDim, kAtlasDim, 0, 0, 0, 0);
        cl->End();
        Device->ExecuteCommandLists({cl.get()});
        Device->FinalizeFrame();
        Device->WaitForIdle();

        SplatBakeResult out;
        out.Height.assign(texels, 0.0f);
        out.SplatRGBA.assign(static_cast<size_t>(texels) * 4u, 0u);
        if (const void* p = Device->MapBuffer(heightReadback))
        {
            std::memcpy(out.Height.data(), p, out.Height.size() * sizeof(float));
            Device->UnmapBuffer(heightReadback);
        }
        if (const void* p = Device->MapBuffer(splatReadback))
        {
            std::memcpy(out.SplatRGBA.data(), p, out.SplatRGBA.size());
            Device->UnmapBuffer(splatReadback);
        }

        Device->DestroyTextureView(hview);
        Device->DestroyTextureView(sview);
        Device->DestroyTexture(htex);
        Device->DestroyTexture(stex);
        for (BufferHandle b : modBufs)
            Device->DestroyBuffer(b);
        Device->DestroyBuffer(seqRuleBuf);
        Device->DestroyBuffer(seqCondBuf);
        Device->DestroyBuffer(zeroBuf);
        Device->DestroyBuffer(heightReadback);
        Device->DestroyBuffer(splatReadback);
        return out;
    }

    // Interior atlas texel (x,z in [0, kTileRes-1]) -> slot-0 index with apron=1.
    static size_t InteriorIndex(uint32_t x, uint32_t z)
    {
        return static_cast<size_t>(z + 1u) * kAtlasDim + (x + 1u);
    }

    static TerrainHeightBakePush FullTilePush(float originX, float originZ, float tileSize,
                                              float heightScale, uint32_t modifierCount)
    {
        TerrainHeightBakePush pc{};
        pc.RectMinX = 0;
        pc.RectMinZ = 0;
        pc.RectW = kTileRes;
        pc.RectH = kTileRes;
        pc.OriginX = originX;
        pc.OriginZ = originZ;
        pc.TileSize = tileSize;
        pc.HeightScale = heightScale;
        pc.BaseFreq = GameEngine::TerrainECS::kTileNoiseFrequency;
        pc.BaseAmp = GameEngine::TerrainECS::kTileNoiseAmplitude;
        pc.BaseOctaves = GameEngine::TerrainECS::kTileNoiseOctaves;
        pc.BaseSeed = GameEngine::TerrainECS::kTileNoiseSeed;
        pc.Slot = 0u;
        pc.ModifierCount = modifierCount;
        pc.TileRes = kTileRes;
        pc.SlotStride = kSlotStride;
        pc.SlotsPerRow = 1u;
        return pc;
    }
};
} // namespace

TEST(TerrainGpuHeightBake, BaseNoiseMatchesCpuFillWithinEpsilon)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    HeightBakeHarness h;
    std::string skip;
    if (!h.Init(device.get(), skip))
        GTEST_SKIP() << skip;

    const float originX = 0.0f, originZ = 0.0f, tileSize = 100.0f, heightScale = 50.0f;
    const auto push = HeightBakeHarness::FullTilePush(originX, originZ, tileSize, heightScale, 0u);
    const std::vector<float> gpu = h.Bake(push, {});

    // CPU reference: the real base fill the modifier bake would run.
    GameEngine::Terrain::HeightfieldData hf(HeightBakeHarness::kTileRes, HeightBakeHarness::kTileRes);
    hf.FillWithNoiseWorldSpace(GameEngine::TerrainECS::kTileNoiseFrequency,
                               GameEngine::TerrainECS::kTileNoiseAmplitude,
                               originX, originZ, tileSize, tileSize,
                               GameEngine::TerrainECS::kTileNoiseOctaves,
                               GameEngine::TerrainECS::kTileNoiseSeed);

    double maxErr = 0.0;
    for (uint32_t z = 0; z < HeightBakeHarness::kTileRes; ++z)
        for (uint32_t x = 0; x < HeightBakeHarness::kTileRes; ++x)
        {
            const float g = gpu[HeightBakeHarness::InteriorIndex(x, z)];
            const float c = hf.GetSample(x, z);
            maxErr = std::max(maxErr, std::abs(static_cast<double>(g) - c));
        }
    // Normalized height in [0,1]; same integer hash, only fp octave-sum ULPs diverge.
    EXPECT_LT(maxErr, 1e-3) << "GPU base fill diverged from CPU beyond epsilon (max=" << maxErr << ")";

    device->Shutdown();
}

TEST(TerrainGpuHeightBake, IsBitStableAcrossIdenticalRuns)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    HeightBakeHarness h;
    std::string skip;
    if (!h.Init(device.get(), skip))
        GTEST_SKIP() << skip;

    const auto push = HeightBakeHarness::FullTilePush(0.0f, 0.0f, 100.0f, 50.0f, 0u);
    const std::vector<float> a = h.Bake(push, {});
    const std::vector<float> b = h.Bake(push, {});
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i)
        ASSERT_EQ(a[i], b[i]) << "GPU bake not bit-stable at texel " << i;

    device->Shutdown();
}

TEST(TerrainGpuHeightBake, FlattenModifierMatchesReferenceWithinEpsilon)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    HeightBakeHarness h;
    std::string skip;
    if (!h.Init(device.get(), skip))
        GTEST_SKIP() << skip;

    const float originX = 0.0f, originZ = 0.0f, tileSize = 100.0f, heightScale = 50.0f;
    const float centerX = 50.0f, centerZ = 50.0f, radius = 20.0f, targetHeight = 25.0f;

    ModifierGpu m{};
    m.Type = static_cast<uint32>(ModifierGpuType::Flatten);
    m.Shape = static_cast<uint32>(ModifierGpuShape::Circle);
    m.CenterX = centerX;
    m.CenterZ = centerZ;
    m.Radius = radius;
    m.Falloff = 0.0f;
    m.NoiseFreqOrTarget = targetHeight;

    const auto push = HeightBakeHarness::FullTilePush(originX, originZ, tileSize, heightScale, 1u);
    const std::vector<float> gpu = h.Bake(push, {m});

    GameEngine::Terrain::HeightfieldData hf(HeightBakeHarness::kTileRes, HeightBakeHarness::kTileRes);
    hf.FillWithNoiseWorldSpace(GameEngine::TerrainECS::kTileNoiseFrequency,
                               GameEngine::TerrainECS::kTileNoiseAmplitude,
                               originX, originZ, tileSize, tileSize,
                               GameEngine::TerrainECS::kTileNoiseOctaves,
                               GameEngine::TerrainECS::kTileNoiseSeed);

    const float spacing = tileSize / static_cast<float>(HeightBakeHarness::kTileRes - 1u);
    const float targetNorm = targetHeight / heightScale;
    double maxErr = 0.0;
    for (uint32_t z = 0; z < HeightBakeHarness::kTileRes; ++z)
        for (uint32_t x = 0; x < HeightBakeHarness::kTileRes; ++x)
        {
            const float wx = originX + static_cast<float>(x) * spacing;
            const float wz = originZ + static_cast<float>(z) * spacing;
            const float dx = wx - centerX, dz = wz - centerZ;
            const float dist = std::sqrt(dx * dx + dz * dz);
            const float w = (radius - dist > 0.0f) ? 1.0f : 0.0f; // falloff = 0
            const float base = hf.GetSample(x, z);
            const float ref = base + (targetNorm - base) * w;
            const float g = gpu[HeightBakeHarness::InteriorIndex(x, z)];
            maxErr = std::max(maxErr, std::abs(static_cast<double>(g) - ref));
        }
    EXPECT_LT(maxErr, 1e-3) << "GPU flatten modifier diverged from reference (max=" << maxErr << ")";

    device->Shutdown();
}

// TargetHeight is a WORLD Y, so a terrain that does not sit at Y = 0 must still flatten to the
// height the author typed. The kernel writes NORMALIZED samples and every consumer reads them
// back as `normalized * HeightScale + TerrainOriginY`, so the assertion is made in world Y — the
// only unit in which "the author typed 25" is a statement about the result.
//
// The normalized reference comes from NormalizedHeightForWorldY, the same function the CPU
// bake's flatten branch calls, rather than from a re-typed expression: a copy would go on
// agreeing with itself after the shipped one changed. The TerrainOriginY = 0 arm is the control
// — it pins any divergence to the origin term rather than to the rest of the stack.
TEST(TerrainGpuHeightBake, FlattenTargetIsAWorldHeightOnATranslatedTerrain)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    HeightBakeHarness h;
    std::string skip;
    if (!h.Init(device.get(), skip))
        GTEST_SKIP() << skip;

    const float originX = 0.0f, originZ = 0.0f, tileSize = 100.0f, heightScale = 50.0f;
    const float centerX = 50.0f, centerZ = 50.0f, radius = 20.0f, targetHeight = 25.0f;
    const float terrainOriginY = 40.0f;

    ModifierGpu m{};
    m.Type = static_cast<uint32>(ModifierGpuType::Flatten);
    m.Shape = static_cast<uint32>(ModifierGpuShape::Circle);
    m.CenterX = centerX;
    m.CenterZ = centerZ;
    m.Radius = radius;
    m.Falloff = 0.0f;
    m.NoiseFreqOrTarget = targetHeight; // the AUTHORED world Y, unshifted

    const auto pushAtOrigin =
        HeightBakeHarness::FullTilePush(originX, originZ, tileSize, heightScale, 1u);
    TerrainHeightBakePush pushRaised = pushAtOrigin;
    pushRaised.TerrainOriginY = terrainOriginY;

    const std::vector<float> gpuAtOrigin = h.Bake(pushAtOrigin, {m});
    const std::vector<float> gpuRaised = h.Bake(pushRaised, {m});

    const float expectedNorm = GameEngine::TerrainECS::NormalizedHeightForWorldY(
        targetHeight, terrainOriginY, heightScale);
    const float spacing = tileSize / static_cast<float>(HeightBakeHarness::kTileRes - 1u);
    size_t covered = 0;
    double maxWorldErrAtOrigin = 0.0;
    double maxWorldErrRaised = 0.0;
    double maxNormErr = 0.0;
    for (uint32_t z = 0; z < HeightBakeHarness::kTileRes; ++z)
        for (uint32_t x = 0; x < HeightBakeHarness::kTileRes; ++x)
        {
            const float wx = originX + static_cast<float>(x) * spacing;
            const float wz = originZ + static_cast<float>(z) * spacing;
            const float dx = wx - centerX, dz = wz - centerZ;
            if (radius - std::sqrt(dx * dx + dz * dz) <= 0.0f)
                continue; // outside the disc: falloff = 0, so the weight is 0 and nothing is set
            ++covered;

            // Set at weight 1 lands exactly on the target, so each arm's baked sample IS the
            // target expressed in that arm's own normalized frame.
            const size_t idx = HeightBakeHarness::InteriorIndex(x, z);
            maxNormErr = std::max(maxNormErr,
                                  std::abs(static_cast<double>(gpuRaised[idx]) - expectedNorm));
            maxWorldErrAtOrigin = std::max(
                maxWorldErrAtOrigin,
                std::abs(static_cast<double>(gpuAtOrigin[idx]) * heightScale - targetHeight));
            maxWorldErrRaised = std::max(
                maxWorldErrRaised,
                std::abs(static_cast<double>(gpuRaised[idx]) * heightScale + terrainOriginY
                         - targetHeight));
        }

    ASSERT_GT(covered, 0u) << "the disc covered no sample — every assertion below is vacuous";
    EXPECT_LT(maxWorldErrAtOrigin, 1e-3)
        << "control arm (TerrainOriginY = 0) missed the target (max=" << maxWorldErrAtOrigin << ")";
    EXPECT_LT(maxWorldErrRaised, 1e-3)
        << "flatten on a terrain at Y=" << terrainOriginY << " landed at the wrong WORLD height "
           "(max=" << maxWorldErrRaised << ") — the kernel is reading TargetHeight as a height "
           "ABOVE the terrain";
    EXPECT_LT(maxNormErr, 1e-6)
        << "the kernel's normalized flatten target diverged from NormalizedHeightForWorldY, the "
           "expression the CPU bake's flatten branch uses (max=" << maxNormErr << ")";

    device->Shutdown();
}

// The union operators on the GPU flatten branch, against the same CPU reference the arm
// above uses. Flatten is where the operators matter (valley carve / dome union) and, with
// Noise, one of the two GPU-eligible effects — so this is the CPU/GPU lockstep gate for
// them. Min is the discriminating case: it must leave below-target ground UNTOUCHED, which
// the pre-operator kernel (an unconditional lerp) provably does not.
TEST(TerrainGpuHeightBake, UnionBlendFlattenMatchesCpuReferenceWithinEpsilon)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    HeightBakeHarness h;
    std::string skip;
    if (!h.Init(device.get(), skip))
        GTEST_SKIP() << skip;

    const float originX = 0.0f, originZ = 0.0f, tileSize = 100.0f, heightScale = 50.0f;
    const float centerX = 50.0f, centerZ = 50.0f, radius = 40.0f;
    const float smoothingM = 4.0f;

    GameEngine::Terrain::HeightfieldData hf(HeightBakeHarness::kTileRes, HeightBakeHarness::kTileRes);
    hf.FillWithNoiseWorldSpace(GameEngine::TerrainECS::kTileNoiseFrequency,
                               GameEngine::TerrainECS::kTileNoiseAmplitude,
                               originX, originZ, tileSize, tileSize,
                               GameEngine::TerrainECS::kTileNoiseOctaves,
                               GameEngine::TerrainECS::kTileNoiseSeed);

    const float spacing = tileSize / static_cast<float>(HeightBakeHarness::kTileRes - 1u);
    auto covers = [&](uint32_t x, uint32_t z) {
        const float dx = originX + static_cast<float>(x) * spacing - centerX;
        const float dz = originZ + static_cast<float>(z) * spacing - centerZ;
        return radius - std::sqrt(dx * dx + dz * dz) > 0.0f;
    };

    // Calibrate the target to the MIDPOINT of the covered base heights rather than to the
    // nominal noise band: the band is a bound, not the realised range, and a target outside
    // the realised range makes Min or Max indistinguishable from Set. The guards below
    // assert the calibration worked rather than trusting it.
    float lo = 1e30f, hi = -1e30f;
    for (uint32_t z = 0; z < HeightBakeHarness::kTileRes; ++z)
        for (uint32_t x = 0; x < HeightBakeHarness::kTileRes; ++x)
            if (covers(x, z))
            {
                lo = std::min(lo, hf.GetSample(x, z));
                hi = std::max(hi, hf.GetSample(x, z));
            }
    ASSERT_LT(lo, hi) << "the covered base heights are flat — every arm below would be vacuous";
    const float targetNorm = 0.5f * (lo + hi);
    const float targetHeight = targetNorm * heightScale;
    const float kNorm = smoothingM / heightScale;

    // Mirror of PolynomialSmoothMin in TerrainModifierSystem.cpp.
    auto smoothMin = [](float a, float b, float k) {
        const float t = std::clamp(0.5f + 0.5f * (b - a) / k, 0.0f, 1.0f);
        return (b + (a - b) * t) - k * t * (1.0f - t);
    };

    struct Arm
    {
        const char* Name;
        GameEngine::Components::TerrainModifierBlend Blend;
    };
    const Arm arms[] = {
        {"Min", GameEngine::Components::TerrainModifierBlend::Min},
        {"Max", GameEngine::Components::TerrainModifierBlend::Max},
        {"SmoothMin", GameEngine::Components::TerrainModifierBlend::SmoothMin},
        {"SmoothMax", GameEngine::Components::TerrainModifierBlend::SmoothMax},
    };

    for (const Arm& arm : arms)
    {
        ModifierGpu m{};
        m.Type = static_cast<uint32>(ModifierGpuType::Flatten);
        m.Shape = static_cast<uint32>(ModifierGpuShape::Circle);
        m.Blend = static_cast<uint32>(arm.Blend);
        m.BlendSmoothing = smoothingM;
        m.CenterX = centerX;
        m.CenterZ = centerZ;
        m.Radius = radius;
        m.Falloff = 0.0f;
        m.NoiseFreqOrTarget = targetHeight;

        const auto push = HeightBakeHarness::FullTilePush(originX, originZ, tileSize, heightScale, 1u);
        const std::vector<float> gpu = h.Bake(push, {m});

        double maxErr = 0.0;
        std::size_t inCircle = 0, below = 0, above = 0;
        for (uint32_t z = 0; z < HeightBakeHarness::kTileRes; ++z)
            for (uint32_t x = 0; x < HeightBakeHarness::kTileRes; ++x)
            {
                const float w = covers(x, z) ? 1.0f : 0.0f;
                const float base = hf.GetSample(x, z);
                if (w > 0.0f)
                {
                    ++inCircle;
                    if (base < targetNorm) ++below; else if (base > targetNorm) ++above;
                }

                float unioned = base;
                switch (arm.Blend)
                {
                case GameEngine::Components::TerrainModifierBlend::Min:
                    unioned = std::min(base, targetNorm); break;
                case GameEngine::Components::TerrainModifierBlend::Max:
                    unioned = std::max(base, targetNorm); break;
                case GameEngine::Components::TerrainModifierBlend::SmoothMin:
                    unioned = smoothMin(base, targetNorm, kNorm); break;
                default:
                    unioned = -smoothMin(-base, -targetNorm, kNorm); break;
                }
                const float ref = base + (unioned - base) * w;
                const float g = gpu[HeightBakeHarness::InteriorIndex(x, z)];
                maxErr = std::max(maxErr, std::abs(static_cast<double>(g) - ref));
            }

        EXPECT_GT(inCircle, 0u) << arm.Name << ": the circle covered no sample";
        EXPECT_GT(below, 0u) << arm.Name << ": no covered sample sits below the target — the "
                                            "operator had nothing to distinguish it from Set";
        EXPECT_GT(above, 0u) << arm.Name << ": no covered sample sits above the target — the "
                                            "operator had nothing to distinguish it from Set";
        EXPECT_LT(maxErr, 1e-3) << "GPU " << arm.Name
                                << " flatten diverged from the CPU reference (max=" << maxErr << ")";
    }

    device->Shutdown();
}

// ---------------------------------------------------------------------------
// Terrain GPU normal-derive device oracle (slice 2).
//
// Bakes the height atlas then runs the KERNEL_NORMAL pass over it (ordered by the same
// height-write -> normal-read memory barrier the live FlushPendingBakes uses), and pins
// the packed RG16F normal atlas to:
//   * NormalMatchesCpuDerivationWithinEpsilon — the exact GenerateNormalmapRegionFromHeightfield
//     derivation (ComputeNormalWithNeighbor) over the GPU-baked heights, within the RG16F
//     quantization epsilon;
//   * SharedEdgeNormalsBitEqualAcrossAdjacentTiles — two real, divergent adjacent tiles bake
//     shared-edge normals that are bit-equal texel-for-texel (the world-eval apron seam contract;
//     edge-duplication would diverge here — the #508 lesson);
//   * IsBitStableAcrossIdenticalRuns — the normal pass is deterministic (a race across the
//     height->normal barrier would surface as run-to-run divergence).
// ---------------------------------------------------------------------------
TEST(TerrainGpuNormalBake, NormalMatchesCpuDerivationWithinEpsilon)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    HeightBakeHarness h;
    std::string skip;
    if (!h.Init(device.get(), skip))
        GTEST_SKIP() << skip;
    if (!h.InitNormal(skip))
        GTEST_SKIP() << skip;

    // Exact-spacing geometry: tileSize is a multiple of (kTileRes-1)=31, so the world sample
    // spacing is exactly 3.0 and every sample lands on a bit-exact grid point.
    const float originX = 0.0f, originZ = 0.0f, tileSize = 93.0f, heightScale = 50.0f;

    // A high-frequency Noise modifier gives real relief across the whole tile, so the derived
    // normals span a meaningful magnitude range (not a near-flat field that would make the
    // parity check vacuous).
    ModifierGpu m{};
    m.Type = static_cast<uint32>(ModifierGpuType::Noise);
    m.Shape = static_cast<uint32>(ModifierGpuShape::Circle);
    m.Blend = 1u; // ADD
    m.Octaves = 4u;
    m.CenterX = 46.5f;
    m.CenterZ = 46.5f;
    m.Radius = 80.0f; // covers the tile + apron
    m.Falloff = 0.0f;
    m.NoiseFreqOrTarget = 30.0f; // world-unit frequency (shader divides by TileSize)
    m.NoiseAmp = 20.0f;          // world-unit amplitude (shader divides by HeightScale)
    m.NoiseLacunarity = 2.0f;
    m.NoisePersistence = 0.5f;
    m.Seed = 777u;

    const auto push = HeightBakeHarness::FullTilePush(originX, originZ, tileSize, heightScale, 1u);
    const NormalBakeResult r = h.BakeHeightAndNormal(push, {m});

    // Rebuild a CPU heightfield from the GPU-baked heights, then derive normals with the exact
    // ComputeNormalWithNeighbor math GenerateNormalmapRegionFromHeightfield runs. Feeding the GPU
    // heights in isolates the NORMAL kernel (central-diff + RG16F pack) from the height-noise ULP
    // slop already bounded by the height oracles; the epsilon is then just RG16F quantization.
    GameEngine::Terrain::HeightfieldData hf(HeightBakeHarness::kTileRes, HeightBakeHarness::kTileRes);
    for (uint32_t z = 0; z < HeightBakeHarness::kTileRes; ++z)
        for (uint32_t x = 0; x < HeightBakeHarness::kTileRes; ++x)
            hf.SetSample(x, z, r.Height[HeightBakeHarness::InteriorIndex(x, z)]);

    // World-scale gradient spacing == the shader's (worldSize / ((res-1) * heightScale)).
    const float spacing =
        tileSize / (static_cast<float>(HeightBakeHarness::kTileRes - 1u) * heightScale);

    // Compare over the strict interior [1, res-2] where both GPU and CPU derive the gradient from
    // in-tile neighbours (identical inputs). The apron-fed slot edges are covered by the boundary
    // bit-equality oracle.
    double maxErr = 0.0;
    double maxComponent = 0.0;
    for (uint32_t z = 1; z + 1 < HeightBakeHarness::kTileRes; ++z)
    {
        for (uint32_t x = 1; x + 1 < HeightBakeHarness::kTileRes; ++x)
        {
            const auto n = hf.ComputeNormalWithNeighbor(static_cast<int32_t>(x), static_cast<int32_t>(z),
                                                        spacing, spacing, nullptr, nullptr, nullptr, nullptr);
            const size_t idx = HeightBakeHarness::InteriorIndex(x, z);
            const float gpuNx = HalfToFloatOracle(r.NormalHalf[idx * 2u + 0u]);
            const float gpuNz = HalfToFloatOracle(r.NormalHalf[idx * 2u + 1u]);
            maxErr = std::max(maxErr, std::abs(static_cast<double>(gpuNx) - n.x));
            maxErr = std::max(maxErr, std::abs(static_cast<double>(gpuNz) - n.z));
            maxComponent = std::max({maxComponent, std::abs(static_cast<double>(gpuNx)),
                                     std::abs(static_cast<double>(gpuNz))});
        }
    }

    // Teeth: the field must actually have relief, else parity is vacuous.
    EXPECT_GT(maxComponent, 0.05) << "normal field is near-flat; parity check would be vacuous";

    // RG16F carries 10 explicit mantissa bits: a normal component of magnitude <= 1 round-trips
    // with <= 2^-11 (~4.9e-4) half-ULP error; fp32 gradient/normalize ordering (GPU rsqrt is
    // Vulkan-spec <= 2 ULP) adds a negligible ~1e-6. 1.5e-3 (~3x the worst-case half-ULP) bounds it.
    EXPECT_LT(maxErr, 1.5e-3) << "GPU normal diverged from CPU derivation beyond RG16F epsilon (max="
                              << maxErr << ")";
    GTEST_LOG_(INFO) << "TerrainGpuNormalBake parity: max component error=" << maxErr
                     << ", max |component|=" << maxComponent;

    device->Shutdown();
}

TEST(TerrainGpuNormalBake, SharedEdgeNormalsBitEqualAcrossAdjacentTiles)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    HeightBakeHarness h;
    std::string skip;
    if (!h.Init(device.get(), skip))
        GTEST_SKIP() << skip;
    if (!h.InitNormal(skip))
        GTEST_SKIP() << skip;

    // Two adjacent tiles, production tiling (origin advances by exactly TileWorldSize). Exact
    // spacing (tileSize=93 => spacing 3.0) makes the shared-column world positions bit-identical
    // between the tiles — the precondition for a bit-exact seam. Tile A samples world x in
    // [0,93], tile B in [93,186]; the shared vertex column is world x=93.
    const float tileSize = 93.0f, heightScale = 64.0f;
    const float originAx = 0.0f, originBx = 93.0f, originZ = 0.0f;

    // A shared modifier straddling the seam (in BOTH tile lists) plus a tile-local modifier in
    // each interior that does NOT reach the seam. The lists genuinely differ (A has aOnly, B has
    // bOnly), yet the world field is globally consistent at the seam — so the shared-edge normals
    // must be bit-equal. Edge-duplication (the pre-slice-2 apron) would read tile A's own edge
    // for hR instead of the neighbour continuation and diverge here.
    ModifierGpu shared{};
    shared.Type = static_cast<uint32>(ModifierGpuType::Noise);
    shared.Shape = static_cast<uint32>(ModifierGpuShape::Circle);
    shared.Blend = 1u; // ADD
    shared.Octaves = 4u;
    shared.CenterX = 93.0f;
    shared.CenterZ = 46.5f;
    shared.Radius = 18.0f;
    shared.Falloff = 0.0f;
    shared.NoiseFreqOrTarget = 25.0f;
    shared.NoiseAmp = 15.0f;
    shared.NoiseLacunarity = 2.0f;
    shared.NoisePersistence = 0.5f;
    shared.Seed = 101u;

    ModifierGpu aOnly{};
    aOnly.Type = static_cast<uint32>(ModifierGpuType::Noise);
    aOnly.Shape = static_cast<uint32>(ModifierGpuShape::Circle);
    aOnly.Blend = 1u; // ADD
    aOnly.Octaves = 3u;
    aOnly.CenterX = 30.0f; // deep in tile A; radius 15 stops at world x=45, far from the seam (93)
    aOnly.CenterZ = 46.5f;
    aOnly.Radius = 15.0f;
    aOnly.Falloff = 0.0f;
    aOnly.NoiseFreqOrTarget = 40.0f;
    aOnly.NoiseAmp = 25.0f;
    aOnly.NoiseLacunarity = 2.0f;
    aOnly.NoisePersistence = 0.5f;
    aOnly.Seed = 202u;

    ModifierGpu bOnly{};
    bOnly.Type = static_cast<uint32>(ModifierGpuType::Flatten);
    bOnly.Shape = static_cast<uint32>(ModifierGpuShape::Circle);
    bOnly.CenterX = 156.0f; // deep in tile B; radius 15 stops at world x=141, far from the seam (93)
    bOnly.CenterZ = 46.5f;
    bOnly.Radius = 15.0f;
    bOnly.Falloff = 0.0f;
    bOnly.NoiseFreqOrTarget = 40.0f; // flatten target height (world units)

    const auto pushA = HeightBakeHarness::FullTilePush(originAx, originZ, tileSize, heightScale, 2u);
    const auto pushB = HeightBakeHarness::FullTilePush(originBx, originZ, tileSize, heightScale, 2u);
    const NormalBakeResult rA = h.BakeHeightAndNormal(pushA, {aOnly, shared});
    const NormalBakeResult rB = h.BakeHeightAndNormal(pushB, {shared, bOnly});

    // Tile A's rightmost interior column (sx = res-1) and tile B's leftmost interior column
    // (sx = 0) are the same world column: their normals must be bit-equal texel-for-texel.
    const uint32_t lastX = HeightBakeHarness::kTileRes - 1u;
    uint16_t edgeMinNx = 0xFFFFu, edgeMaxNx = 0u;
    bool sawVariation = false;
    for (uint32_t z = 0; z < HeightBakeHarness::kTileRes; ++z)
    {
        const size_t idxA = HeightBakeHarness::InteriorIndex(lastX, z);
        const size_t idxB = HeightBakeHarness::InteriorIndex(0u, z);
        const uint16_t aNx = rA.NormalHalf[idxA * 2u + 0u];
        const uint16_t aNz = rA.NormalHalf[idxA * 2u + 1u];
        const uint16_t bNx = rB.NormalHalf[idxB * 2u + 0u];
        const uint16_t bNz = rB.NormalHalf[idxB * 2u + 1u];
        ASSERT_EQ(aNx, bNx) << "shared-edge normal.x not bit-equal at z=" << z;
        ASSERT_EQ(aNz, bNz) << "shared-edge normal.z not bit-equal at z=" << z;
        edgeMinNx = std::min(edgeMinNx, aNx);
        edgeMaxNx = std::max(edgeMaxNx, aNx);
        if (std::abs(HalfToFloatOracle(aNx)) > 0.02f)
            sawVariation = true;
    }

    // Teeth: the shared edge must carry real relief (from the straddling modifier), not a constant
    // straight-up field that would make bit-equality vacuous.
    EXPECT_NE(edgeMinNx, edgeMaxNx) << "shared edge normals are constant; test would be vacuous";
    EXPECT_TRUE(sawVariation) << "shared edge has no tilted normals; test would be vacuous";

    device->Shutdown();
}

TEST(TerrainGpuNormalBake, IsBitStableAcrossIdenticalRuns)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    HeightBakeHarness h;
    std::string skip;
    if (!h.Init(device.get(), skip))
        GTEST_SKIP() << skip;
    if (!h.InitNormal(skip))
        GTEST_SKIP() << skip;

    ModifierGpu m{};
    m.Type = static_cast<uint32>(ModifierGpuType::Noise);
    m.Shape = static_cast<uint32>(ModifierGpuShape::Circle);
    m.Blend = 1u; // ADD
    m.Octaves = 4u;
    m.CenterX = 46.5f;
    m.CenterZ = 46.5f;
    m.Radius = 80.0f;
    m.Falloff = 0.0f;
    m.NoiseFreqOrTarget = 30.0f;
    m.NoiseAmp = 20.0f;
    m.NoiseLacunarity = 2.0f;
    m.NoisePersistence = 0.5f;
    m.Seed = 777u;

    const auto push = HeightBakeHarness::FullTilePush(0.0f, 0.0f, 93.0f, 50.0f, 1u);
    const NormalBakeResult a = h.BakeHeightAndNormal(push, {m});
    const NormalBakeResult b = h.BakeHeightAndNormal(push, {m});
    ASSERT_EQ(a.NormalHalf.size(), b.NormalHalf.size());
    for (size_t i = 0; i < a.NormalHalf.size(); ++i)
        ASSERT_EQ(a.NormalHalf[i], b.NormalHalf[i]) << "GPU normal bake not bit-stable at half " << i;

    device->Shutdown();
}

// ---------------------------------------------------------------------------
// TerrainGpuSplatBake — the GPU splat pass.
// Bakes the height atlas then runs KERNEL_SPLAT over it (ordered by the same shared post-height
// memory barrier FlushPendingBakes uses for the normal + splat passes), and pins the RGBA8 splat
// atlas to:
//   * UnbakedSplatIsAllZeroOnTheDevice — with NO rows authored the kernel places NOTHING. Material
//     comes from authored rules, and this is the pin against a kernel quietly re-deriving it from
//     slope or altitude (two such copies shipped, and both disagreed with the bake they stood in for);
//   * SurfaceRulesMatchCpuDerivationWithinEpsilon — authored rows composited over that zero base
//     reproduce the CPU bake within the RGBA8 quantization epsilon (≤1 LSB — the shader mirrors the
//     CPU's static_cast<uint8> truncation, so the only residual divergence is fp evaluation);
//   * BoundaryContinuityAcrossAdjacentTiles — two real, divergent adjacent tiles: each tile's slot-
//     edge splat matches its OWN per-tile CPU derivation (tile-local slope clamp, NOT the neighbour-
//     continuation apron the height/normal passes fill — the INVERSE of the normal seam contract, #508);
//   * IsBitStableAcrossIdenticalRuns — the splat pass is deterministic across runs.
// ---------------------------------------------------------------------------

// Compute the interior [0,res-1] min/max of a height readback (the committed global range a single
// tile would contribute), mirroring the CPU ComputeResidentGlobalHeightRange scan.
namespace
{
void InteriorHeightRange(const std::vector<float>& height, float& outMin, float& outMax)
{
    outMin = 1e30f;
    outMax = -1e30f;
    for (uint32_t z = 0; z < HeightBakeHarness::kTileRes; ++z)
        for (uint32_t x = 0; x < HeightBakeHarness::kTileRes; ++x)
        {
            const float s = height[HeightBakeHarness::InteriorIndex(x, z)];
            outMin = std::min(outMin, s);
            outMax = std::max(outMax, s);
        }
}
} // namespace

TEST(TerrainGpuSplatBake, UnbakedSplatIsAllZeroOnTheDevice)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    HeightBakeHarness h;
    std::string skip;
    if (!h.Init(device.get(), skip))
        GTEST_SKIP() << skip;
    if (!h.InitSplat(skip))
        GTEST_SKIP() << skip;

    // Same geometry as the rules oracle below, so the two read the same ground: a moderate-
    // wavelength noise over a small heightScale gives flat tops, steep flanks and low valleys.
    const float originX = 0.0f, originZ = 0.0f, tileSize = 93.0f, heightScale = 8.0f;

    TerrainECS::ModifierGpu m{};
    m.Type = static_cast<uint32>(TerrainECS::ModifierGpuType::Noise);
    m.Shape = static_cast<uint32>(TerrainECS::ModifierGpuShape::Circle);
    m.Blend = 1u; // ADD
    m.Octaves = 4u;
    m.CenterX = 46.5f;
    m.CenterZ = 46.5f;
    m.Radius = 90.0f;
    m.Falloff = 0.0f;
    m.NoiseFreqOrTarget = 6.0f;
    m.NoiseAmp = 35.0f;
    m.NoiseLacunarity = 2.0f;
    m.NoisePersistence = 0.5f;
    m.Seed = 909u;

    const auto push = HeightBakeHarness::FullTilePush(originX, originZ, tileSize, heightScale, 1u);
    const std::vector<float> probe = h.Bake(push, {m});
    float minH = 0.0f, maxH = 0.0f;
    InteriorHeightRange(probe, minH, maxH);
    ASSERT_LT(minH, maxH) << "probe range is degenerate";

    // NO surface rules pushed: nothing has been authored, so nothing may be placed. The kernel
    // must leave the base it started from, and that base is zero — the GPU mirror of
    // ResetSplatmap. Zero is not "no data": the surface resolves an all-zero texel to channel 0,
    // so this is exactly "unbaked reads as the first material" and NOT a black terrain.
    //
    // This is the pin that would catch a kernel quietly re-deriving material from slope or
    // altitude. Two previous copies of that derivation shipped, and both disagreed with the bake
    // they stood in for.
    const SplatBakeResult r = h.BakeHeightAndSplat(push, {m}, minH, maxH);

    size_t nonZeroTexels = 0;
    int worstByte = 0;
    for (uint32_t z = 0; z < HeightBakeHarness::kTileRes; ++z)
        for (uint32_t x = 0; x < HeightBakeHarness::kTileRes; ++x)
        {
            const size_t idx = HeightBakeHarness::InteriorIndex(x, z) * 4u;
            int texelMax = 0;
            for (uint32_t c = 0; c < 4u; ++c)
                texelMax = std::max(texelMax, static_cast<int>(r.SplatRGBA[idx + c]));
            if (texelMax > 0)
                ++nonZeroTexels;
            worstByte = std::max(worstByte, texelMax);
        }

    EXPECT_EQ(nonZeroTexels, 0u)
        << nonZeroTexels << " texels carry weight with no rule authored (worst byte " << worstByte
        << "): the splat kernel is guessing material from the heightfield";

    // The height pass really did produce varied ground, so the zero above is the kernel declining
    // to classify rather than a flat probe with nothing to classify.
    ASSERT_GT(maxH - minH, 0.0f);

    device->Shutdown();
}

// SURFACE RULES on the device (F10 S3). The host gate
// (TerrainSurfaceRuleGpuParityTests) proves the two SOURCES compute the same expressions, exactly,
// by executing the shipped GLSL as C++. It cannot prove the DRIVER agrees: a GPU's transcendentals
// (atan/sqrt/sin/cos) are only accurate to a few ULP by specification, and the rule sample's slope
// in degrees runs through atan. This test closes that half — a real device, real rule rows in a
// real SSBO — against the same CPU derivation, at the same RGBA8 quantization floor the procedural
// oracle above uses.
TEST(TerrainGpuSplatBake, SurfaceRulesMatchCpuDerivationWithinEpsilon)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    HeightBakeHarness h;
    std::string skip;
    if (!h.Init(device.get(), skip))
        GTEST_SKIP() << skip;
    if (!h.InitSplat(skip))
        GTEST_SKIP() << skip;

    // Same geometry as the procedural oracle: a small height scale is what makes the normalized
    // gradient large enough to cross the slope bands, and puts SlopeDegrees in an authorable range.
    const float originX = 0.0f, originZ = 0.0f, tileSize = 93.0f, heightScale = 8.0f;

    TerrainECS::ModifierGpu m{};
    m.Type = static_cast<uint32>(TerrainECS::ModifierGpuType::Noise);
    m.Shape = static_cast<uint32>(TerrainECS::ModifierGpuShape::Circle);
    m.Blend = 1u; // ADD
    m.Octaves = 4u;
    m.CenterX = 46.5f;
    m.CenterZ = 46.5f;
    m.Radius = 90.0f;
    m.Falloff = 0.0f;
    m.NoiseFreqOrTarget = 6.0f;
    m.NoiseAmp = 35.0f;
    m.NoiseLacunarity = 2.0f;
    m.NoisePersistence = 0.5f;
    m.Seed = 909u;

    const auto push = HeightBakeHarness::FullTilePush(originX, originZ, tileSize, heightScale, 1u);
    const std::vector<float> probe = h.Bake(push, {m});
    float minH = 0.0f, maxH = 0.0f;
    InteriorHeightRange(probe, minH, maxH);
    ASSERT_LT(minH, maxH) << "probe range is degenerate";

    // Two authored rows through the shipped packer, so the bytes the kernel reads are the bytes the
    // modifier system would have produced: a global row banding slope, and a circle row banding
    // noise with the other paint semantics.
    GameEngine::Components::TerrainSurfaceRule steep{};
    steep.MaterialSlot = 1;
    steep.Strength = 1.0f;
    steep.ConditionCount = 2;
    steep.Conditions[0].Kind = GameEngine::Components::TerrainRuleConditionKind::SlopeDegrees;
    steep.Conditions[0].Min = 25.0f;
    steep.Conditions[0].Max = 90.0f;
    steep.Conditions[0].Feather = 10.0f;
    steep.Conditions[1].Kind = GameEngine::Components::TerrainRuleConditionKind::HeightNormalized;
    steep.Conditions[1].Min = 0.2f;
    steep.Conditions[1].Max = 1.0f;
    steep.Conditions[1].Feather = 0.15f;

    GameEngine::Components::TerrainSurfaceRule blotch{};
    blotch.MaterialSlot = 2;
    blotch.Strength = 0.9f;
    blotch.Replace = true;
    blotch.ConditionCount = 1;
    blotch.Conditions[0].Kind = GameEngine::Components::TerrainRuleConditionKind::Noise;
    blotch.Conditions[0].Min = 0.55f;
    blotch.Conditions[0].Max = 1.0f;
    blotch.Conditions[0].Feather = 0.2f;
    blotch.Conditions[0].NoiseFrequency = 0.04f;
    blotch.Conditions[0].NoiseSeed = 17u;

    std::vector<GameEngine::Components::TerrainSurfaceRule> globalRows = {steep};
    std::vector<GameEngine::Components::TerrainSurfaceRule> patchRows = {blotch};

    TerrainECS::ResolvedModifier globalVol{};
    globalVol.ModType = TerrainECS::ResolvedModifier::Type::Volume;
    globalVol.Shape = GameEngine::Components::TerrainModifierShape::Rectangle;
    globalVol.GlobalScope = true;
    globalVol.Weight = 1.0f;
    TerrainECS::ResolvedEffect globalFx{};
    globalFx.EffectKind = TerrainECS::ResolvedEffect::Kind::Rules;
    globalFx.Rules.Rules = globalRows.data();
    globalFx.Rules.RuleCount = 1;
    globalVol.Effects.push_back(globalFx);

    TerrainECS::ResolvedModifier patchVol{};
    patchVol.ModType = TerrainECS::ResolvedModifier::Type::Volume;
    patchVol.Shape = GameEngine::Components::TerrainModifierShape::Circle;
    patchVol.Position = GameEngine::Mathematics::Vector3(46.5f, 0.0f, 46.5f);
    patchVol.Radius = 34.0f;
    patchVol.Falloff = 14.0f;
    patchVol.Weight = 0.85f;
    TerrainECS::ResolvedEffect patchFx{};
    patchFx.EffectKind = TerrainECS::ResolvedEffect::Kind::Rules;
    patchFx.Rules.Rules = patchRows.data();
    patchFx.Rules.RuleCount = 1;
    patchVol.Effects.push_back(patchFx);

    std::vector<TerrainECS::SurfaceRuleGpu> gpuRules;
    std::vector<TerrainECS::SurfaceRuleConditionGpu> gpuConditions;
    ASSERT_TRUE(TerrainECS::PackSurfaceRulesForGpuSplat({globalVol, patchVol}, gpuRules,
                                                        gpuConditions));
    ASSERT_EQ(gpuRules.size(), 2u);

    const SplatBakeResult r =
        h.BakeHeightAndSplat(push, {m}, minH, maxH, gpuRules, gpuConditions);

    // CPU reference: the unbaked (all-zero) base over the GPU-baked heights — isolating the splat
    // kernel from the height pass's noise ULP, as the oracle above does — then the same rows
    // composited on top through the shipped evaluator and the shipped splat write.
    Terrain::HeightfieldData hf(HeightBakeHarness::kTileRes, HeightBakeHarness::kTileRes);
    for (uint32_t z = 0; z < HeightBakeHarness::kTileRes; ++z)
        for (uint32_t x = 0; x < HeightBakeHarness::kTileRes; ++x)
            hf.SetSample(x, z, r.Height[HeightBakeHarness::InteriorIndex(x, z)]);

    std::vector<uint8> cpuSplat;
    uint32 sw = 0, sh = 0;
    TerrainECS::ResetSplatmap(hf, cpuSplat, sw, sh);
    ASSERT_EQ(sw, HeightBakeHarness::kTileRes);

    const float spacing = tileSize / static_cast<float>(HeightBakeHarness::kTileRes - 1u);
    const float heightRange = std::max(maxH - minH, 0.001f);
    size_t rowFired[2] = {0, 0};
    for (uint32_t z = 0; z < HeightBakeHarness::kTileRes; ++z)
    {
        for (uint32_t x = 0; x < HeightBakeHarness::kTileRes; ++x)
        {
            const float wx = originX + static_cast<float>(x) * spacing;
            const float wz = originZ + static_cast<float>(z) * spacing;
            const auto n = hf.ComputeNormal(static_cast<int32>(x), static_cast<int32>(z),
                                            spacing, spacing);
            const TerrainECS::TerrainRuleSample sample = TerrainECS::MakeTerrainRuleSample(
                n.x, n.y, n.z, hf.GetSample(x, z), heightScale, minH, heightRange, wx, wz);

            uint8* pixel = &cpuSplat[(static_cast<size_t>(z) * sw + x) * 4];
            size_t rowIndex = 0;
            for (const TerrainECS::ResolvedModifier* vol : {&globalVol, &patchVol})
            {
                const float volumeWeight =
                    TerrainECS::ComputeWeight(*vol, wx, wz);
                for (const auto& fx : vol->Effects)
                {
                    for (uint32 i = 0; i < fx.Rules.RuleCount; ++i)
                    {
                        const size_t thisRow = rowIndex++;
                        if (volumeWeight <= 0.0f)
                            continue;
                        const auto& row = fx.Rules.Rules[i];
                        const float weight =
                            TerrainECS::EvaluateTerrainSurfaceRuleWeight(
                                row, sample, TerrainECS::SurfaceRuleNoiseSample) * volumeWeight;
                        if (weight <= 0.0f)
                            continue;
                        ++rowFired[thisRow];
                        TerrainECS::CompositeSplatTexel(pixel, row.MaterialSlot, weight,
                                                        row.Replace);
                    }
                }
            }
        }
    }

    // Teeth: both rows must have written somewhere, or the comparison is about the base only.
    EXPECT_GT(rowFired[0], 50u) << "the slope+altitude row never fired; the gate would be vacuous";
    EXPECT_GT(rowFired[1], 50u) << "the noise row never fired; the gate would be vacuous";

    int maxDiff = 0;
    size_t twoPlus = 0, diffTexels = 0;
    for (uint32_t z = 0; z < HeightBakeHarness::kTileRes; ++z)
    {
        for (uint32_t x = 0; x < HeightBakeHarness::kTileRes; ++x)
        {
            const size_t gpuIdx = HeightBakeHarness::InteriorIndex(x, z) * 4u;
            const size_t cpuIdx = (static_cast<size_t>(z) * HeightBakeHarness::kTileRes + x) * 4u;
            bool differed = false;
            for (uint32_t c = 0; c < 4u; ++c)
            {
                const int d = std::abs(static_cast<int>(r.SplatRGBA[gpuIdx + c]) -
                                       static_cast<int>(cpuSplat[cpuIdx + c]));
                maxDiff = std::max(maxDiff, d);
                if (d > 0) differed = true;
                if (d >= 2) ++twoPlus;
            }
            if (differed) ++diffTexels;
        }
    }

    // The bound, and why it is 1 rather than 0: the shader packs each weight with the identical
    // truncating uint conversion the CPU uses, so the only residual is a driver-vs-libm ULP in the
    // rule maths (atan for SlopeDegrees, the noise lattice's interpolation) straddling a truncation
    // boundary. A rule row composites through those bytes, so a ULP can move ONE step and no more.
    // A texel off by >=2 LSB is a math mismatch, not quantization, and fails.
    EXPECT_EQ(twoPlus, 0u) << "a rules texel diverged by >=2 LSB (a math mismatch, not quantization)";
    EXPECT_LE(maxDiff, 1) << "GPU rules splat diverged from the CPU derivation beyond the RGBA8 "
                             "quantization floor (max=" << maxDiff << ")";
    GTEST_LOG_(INFO) << "TerrainGpuSplatBake surface-rule parity: max byte divergence=" << maxDiff
                     << " LSB, diverging texels=" << diffTexels << "/"
                     << (HeightBakeHarness::kTileRes * HeightBakeHarness::kTileRes)
                     << ", rows fired=" << rowFired[0] << "/" << rowFired[1];

    device->Shutdown();
}

TEST(TerrainGpuSplatBake, BoundaryContinuityAcrossAdjacentTiles)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    HeightBakeHarness h;
    std::string skip;
    if (!h.Init(device.get(), skip))
        GTEST_SKIP() << skip;
    if (!h.InitSplat(skip))
        GTEST_SKIP() << skip;

    // Two adjacent tiles, production tiling (origin advances by exactly TileWorldSize), exact
    // spacing (3.0). Divergent modifier lists (A has aOnly, B has bOnly) plus a shared seam-
    // straddling modifier: the world height field is globally consistent so the height APRON of
    // tile A holds tile B's continuation content (≠ A's own edge column). The splat must still
    // match A's per-tile CPU derivation at its right edge — proving it reads the tile-local slope
    // clamp, NOT that apron (the inverse of the normal pass; #508).
    const float tileSize = 93.0f, heightScale = 64.0f;
    const float originAx = 0.0f, originBx = 93.0f, originZ = 0.0f;

    TerrainECS::ModifierGpu shared{};
    shared.Type = static_cast<uint32>(TerrainECS::ModifierGpuType::Noise);
    shared.Shape = static_cast<uint32>(TerrainECS::ModifierGpuShape::Circle);
    shared.Blend = 1u; // ADD
    shared.Octaves = 5u;
    shared.CenterX = 93.0f; // on the seam
    shared.CenterZ = 46.5f;
    shared.Radius = 40.0f;  // reaches both interiors + the seam
    shared.Falloff = 0.0f;
    shared.NoiseFreqOrTarget = 35.0f;
    shared.NoiseAmp = 28.0f;
    shared.NoiseLacunarity = 2.0f;
    shared.NoisePersistence = 0.5f;
    shared.Seed = 101u;

    TerrainECS::ModifierGpu aOnly{};
    aOnly.Type = static_cast<uint32>(TerrainECS::ModifierGpuType::Noise);
    aOnly.Shape = static_cast<uint32>(TerrainECS::ModifierGpuShape::Circle);
    aOnly.Blend = 1u; // ADD
    aOnly.Octaves = 4u;
    aOnly.CenterX = 25.0f; // deep in tile A
    aOnly.CenterZ = 46.5f;
    aOnly.Radius = 20.0f;
    aOnly.Falloff = 0.0f;
    aOnly.NoiseFreqOrTarget = 50.0f;
    aOnly.NoiseAmp = 35.0f;
    aOnly.NoiseLacunarity = 2.0f;
    aOnly.NoisePersistence = 0.5f;
    aOnly.Seed = 202u;

    TerrainECS::ModifierGpu bOnly{};
    bOnly.Type = static_cast<uint32>(TerrainECS::ModifierGpuType::Noise);
    bOnly.Shape = static_cast<uint32>(TerrainECS::ModifierGpuShape::Circle);
    bOnly.Blend = 1u; // ADD
    bOnly.Octaves = 4u;
    bOnly.CenterX = 161.0f; // deep in tile B
    bOnly.CenterZ = 46.5f;
    bOnly.Radius = 20.0f;
    bOnly.Falloff = 0.0f;
    bOnly.NoiseFreqOrTarget = 50.0f;
    bOnly.NoiseAmp = 35.0f;
    bOnly.NoiseLacunarity = 2.0f;
    bOnly.NoisePersistence = 0.5f;
    bOnly.Seed = 303u;

    const auto pushA = HeightBakeHarness::FullTilePush(originAx, originZ, tileSize, heightScale, 2u);
    const auto pushB = HeightBakeHarness::FullTilePush(originBx, originZ, tileSize, heightScale, 2u);

    // Shared committed global range across BOTH tiles (as production computes it).
    const std::vector<float> probeA = h.Bake(pushA, {aOnly, shared});
    const std::vector<float> probeB = h.Bake(pushB, {shared, bOnly});
    float minA, maxA, minB, maxB;
    InteriorHeightRange(probeA, minA, maxA);
    InteriorHeightRange(probeB, minB, maxB);
    const float minH = std::min(minA, minB), maxH = std::max(maxA, maxB);

    // A SLOPE-driven global row is what makes the tile-clamp contract observable: the splat only
    // carries an edge value at all because a rule measured the slope there, and slope is precisely
    // the quantity that would change if the shader read the neighbour apron instead of clamping.
    GameEngine::Components::TerrainSurfaceRule slopeRow{};
    slopeRow.MaterialSlot = 1;
    slopeRow.Strength = 1.0f;
    slopeRow.ConditionCount = 1;
    // A degenerate band at 0 with a 90-degree feather: weight ramps linearly from 1 at flat to 0
    // at vertical. Every texel gets some weight (so the comparison is never zeros-to-zeros) AND
    // the weight is monotone in slope, so reading the apron instead of the tile-local clamp would
    // move the byte. A narrow band would be dead here - this terrain is HeightScale 64 with a
    // 30 m noise amplitude, so its true slopes crowd the steep end.
    slopeRow.Conditions[0].Kind = GameEngine::Components::TerrainRuleConditionKind::SlopeDegrees;
    slopeRow.Conditions[0].Min = 0.0f;
    slopeRow.Conditions[0].Max = 0.0f;
    slopeRow.Conditions[0].Feather = 90.0f;
    std::vector<GameEngine::Components::TerrainSurfaceRule> slopeRows = {slopeRow};

    TerrainECS::ResolvedModifier slopeVol{};
    slopeVol.ModType = TerrainECS::ResolvedModifier::Type::Volume;
    slopeVol.Shape = GameEngine::Components::TerrainModifierShape::Rectangle;
    slopeVol.GlobalScope = true;
    slopeVol.Weight = 1.0f;
    TerrainECS::ResolvedEffect slopeFx{};
    slopeFx.EffectKind = TerrainECS::ResolvedEffect::Kind::Rules;
    slopeFx.Rules.Rules = slopeRows.data();
    slopeFx.Rules.RuleCount = 1;
    slopeVol.Effects.push_back(slopeFx);

    std::vector<TerrainECS::SurfaceRuleGpu> gpuRules;
    std::vector<TerrainECS::SurfaceRuleConditionGpu> gpuConditions;
    ASSERT_TRUE(TerrainECS::PackSurfaceRulesForGpuSplat({slopeVol}, gpuRules, gpuConditions));

    const SplatBakeResult rA =
        h.BakeHeightAndSplat(pushA, {aOnly, shared}, minH, maxH, gpuRules, gpuConditions);
    const SplatBakeResult rB =
        h.BakeHeightAndSplat(pushB, {shared, bOnly}, minH, maxH, gpuRules, gpuConditions);

    auto cpuSplatFor = [&](const SplatBakeResult& r, float tileOriginX) {
        Terrain::HeightfieldData hf(HeightBakeHarness::kTileRes, HeightBakeHarness::kTileRes);
        for (uint32_t z = 0; z < HeightBakeHarness::kTileRes; ++z)
            for (uint32_t x = 0; x < HeightBakeHarness::kTileRes; ++x)
                hf.SetSample(x, z, r.Height[HeightBakeHarness::InteriorIndex(x, z)]);
        std::vector<uint8> splat;
        uint32 sw = 0, sh = 0;
        TerrainECS::ResetSplatmap(hf, splat, sw, sh);
        const float spacing = tileSize / static_cast<float>(HeightBakeHarness::kTileRes - 1u);
        const float heightRange = std::max(maxH - minH, 0.001f);
        for (uint32_t z = 0; z < HeightBakeHarness::kTileRes; ++z)
            for (uint32_t x = 0; x < HeightBakeHarness::kTileRes; ++x)
            {
                const float wx = tileOriginX + static_cast<float>(x) * spacing;
                const float wz = originZ + static_cast<float>(z) * spacing;
                const auto n = hf.ComputeNormal(static_cast<int32>(x), static_cast<int32>(z),
                                                spacing, spacing);
                const TerrainECS::TerrainRuleSample sample = TerrainECS::MakeTerrainRuleSample(
                    n.x, n.y, n.z, hf.GetSample(x, z), heightScale, minH, heightRange, wx, wz);
                const float w = TerrainECS::EvaluateTerrainSurfaceRuleWeight(
                    slopeRows[0], sample, TerrainECS::SurfaceRuleNoiseSample);
                if (w > 0.0f)
                    TerrainECS::CompositeSplatTexel(&splat[(static_cast<size_t>(z) * sw + x) * 4],
                                                    slopeRows[0].MaterialSlot, w,
                                                    slopeRows[0].Replace);
            }
        return splat;
    };
    const std::vector<uint8> cpuA = cpuSplatFor(rA, originAx);
    const std::vector<uint8> cpuB = cpuSplatFor(rB, originBx);

    // Each tile's slot-edge column (A right = res-1, B left = 0) must match its own CPU derivation
    // within the quantization floor. If the shader read the height APRON for the slope, tile A's
    // right edge would use B's continuation and diverge from A's tile-clamped CPU reference.
    const uint32_t lastX = HeightBakeHarness::kTileRes - 1u;
    int maxDiff = 0;
    bool apronDiffersFromEdge = false;
    for (uint32_t z = 0; z < HeightBakeHarness::kTileRes; ++z)
    {
        // Tile A right edge.
        const size_t gaIdx = HeightBakeHarness::InteriorIndex(lastX, z) * 4u;
        const size_t caIdx = (static_cast<size_t>(z) * HeightBakeHarness::kTileRes + lastX) * 4u;
        // Tile B left edge.
        const size_t gbIdx = HeightBakeHarness::InteriorIndex(0u, z) * 4u;
        const size_t cbIdx = (static_cast<size_t>(z) * HeightBakeHarness::kTileRes + 0u) * 4u;
        for (uint32_t c = 0; c < 4u; ++c)
        {
            maxDiff = std::max(maxDiff, std::abs(int(rA.SplatRGBA[gaIdx + c]) - int(cpuA[caIdx + c])));
            maxDiff = std::max(maxDiff, std::abs(int(rB.SplatRGBA[gbIdx + c]) - int(cpuB[cbIdx + c])));
        }
        // Teeth: A's right apron (atlas col res+1) holds B's continuation; confirm it genuinely
        // differs from A's own edge height, so tile-clamp vs apron is a real, testable distinction.
        const size_t edgeH = (static_cast<size_t>(z + 1u)) * HeightBakeHarness::kAtlasDim + (lastX + 1u);
        const size_t apronH = (static_cast<size_t>(z + 1u)) * HeightBakeHarness::kAtlasDim + (lastX + 2u);
        if (std::abs(rA.Height[edgeH] - rA.Height[apronH]) > 0.01f)
            apronDiffersFromEdge = true;
    }

    EXPECT_TRUE(apronDiffersFromEdge)
        << "the height apron equals the tile edge everywhere; the tile-clamp vs apron distinction "
           "is untestable here (test would be vacuous)";

    // Second positive control, for the base: the compared edges must actually carry weight. An
    // unbaked splat is all zero, so a rule that never fired would make the parity above compare
    // zeros to zeros and pass without measuring anything.
    size_t weightedEdgeTexels = 0;
    for (uint32_t z = 0; z < HeightBakeHarness::kTileRes; ++z)
    {
        const size_t gaIdx = HeightBakeHarness::InteriorIndex(lastX, z) * 4u;
        const size_t gbIdx = HeightBakeHarness::InteriorIndex(0u, z) * 4u;
        for (uint32_t c = 0; c < 4u; ++c)
            if (rA.SplatRGBA[gaIdx + c] || rB.SplatRGBA[gbIdx + c])
            {
                ++weightedEdgeTexels;
                break;
            }
    }
    EXPECT_GT(weightedEdgeTexels, HeightBakeHarness::kTileRes / 4u)
        << "the slope row placed almost nothing on the compared edges - the parity check above "
           "is comparing unbaked zeros";
    EXPECT_LE(maxDiff, 1) << "GPU splat slot-edge diverged from the per-tile CPU derivation (max="
                          << maxDiff << ") — the slope may be reading the apron instead of the "
                             "tile-local clamp";
    GTEST_LOG_(INFO) << "TerrainGpuSplatBake boundary: max slot-edge byte divergence=" << maxDiff
                     << " LSB (tile-clamp parity), range=[" << minH << "," << maxH << "]";

    device->Shutdown();
}

TEST(TerrainGpuSplatBake, IsBitStableAcrossIdenticalRuns)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    HeightBakeHarness h;
    std::string skip;
    if (!h.Init(device.get(), skip))
        GTEST_SKIP() << skip;
    if (!h.InitSplat(skip))
        GTEST_SKIP() << skip;

    TerrainECS::ModifierGpu m{};
    m.Type = static_cast<uint32>(TerrainECS::ModifierGpuType::Noise);
    m.Shape = static_cast<uint32>(TerrainECS::ModifierGpuShape::Circle);
    m.Blend = 1u; // ADD
    m.Octaves = 5u;
    m.CenterX = 46.5f;
    m.CenterZ = 46.5f;
    m.Radius = 90.0f;
    m.Falloff = 0.0f;
    m.NoiseFreqOrTarget = 45.0f;
    m.NoiseAmp = 30.0f;
    m.NoiseLacunarity = 2.0f;
    m.NoisePersistence = 0.55f;
    m.Seed = 909u;

    const auto push = HeightBakeHarness::FullTilePush(0.0f, 0.0f, 93.0f, 50.0f, 1u);
    const SplatBakeResult a = h.BakeHeightAndSplat(push, {m}, 0.1f, 1.5f);
    const SplatBakeResult b = h.BakeHeightAndSplat(push, {m}, 0.1f, 1.5f);
    ASSERT_EQ(a.SplatRGBA.size(), b.SplatRGBA.size());
    for (size_t i = 0; i < a.SplatRGBA.size(); ++i)
        ASSERT_EQ(a.SplatRGBA[i], b.SplatRGBA[i]) << "GPU splat bake not bit-stable at byte " << i;

    device->Shutdown();
}

// MOVING-MODIFIER parity (the drag/eval-skip sequence, not a single bake): a flatten that MOVES
// from A to B is baked the way the eval-skip drag path bakes it — an initial full-tile pass at A,
// then a REGION pass over the old-union-new footprint at B (leaving the texels outside that rect
// holding the pass-A content, exactly the atlas-write model). That accumulated result must equal a
// single full-tile bake at B for BOTH height and splat: the region pass restores flatten-A's old
// footprint to base terrain and lays down flatten-B, and every texel outside the rect (outside both
// footprints) is base terrain either way. Byte-identical because the GPU eval is stateless per world
// position. Then the accumulated splat is tied to the CPU derivation at the RGBA8 quantization floor
// so the moving-modifier splat is CPU-parity, not just self-consistent.
TEST(TerrainGpuSplatBake, MovingFlattenRegionSequenceMatchesFullBakeAndCpu)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    HeightBakeHarness h;
    std::string skip;
    if (!h.Init(device.get(), skip))
        GTEST_SKIP() << skip;
    if (!h.InitSplat(skip))
        GTEST_SKIP() << skip;

    // Exact spacing (3.0) + small heightScale so the slope classification crosses all bands; a
    // tile-covering base Noise gives varied relief, and the flatten carves a plateau that moves.
    const float originX = 0.0f, originZ = 0.0f, tileSize = 93.0f, heightScale = 8.0f;
    const float spacing = tileSize / static_cast<float>(HeightBakeHarness::kTileRes - 1u); // 3.0

    TerrainECS::ModifierGpu noise{};
    noise.Type = static_cast<uint32>(TerrainECS::ModifierGpuType::Noise);
    noise.Shape = static_cast<uint32>(TerrainECS::ModifierGpuShape::Circle);
    noise.Blend = 1u; // ADD
    noise.Octaves = 4u;
    noise.CenterX = 46.5f;
    noise.CenterZ = 46.5f;
    noise.Radius = 90.0f; // covers the tile + apron
    noise.Falloff = 0.0f;
    noise.NoiseFreqOrTarget = 6.0f;
    noise.NoiseAmp = 35.0f;
    noise.NoiseLacunarity = 2.0f;
    noise.NoisePersistence = 0.5f;
    noise.Seed = 909u;

    auto makeFlatten = [&](float cx) {
        TerrainECS::ModifierGpu f{};
        f.Type = static_cast<uint32>(TerrainECS::ModifierGpuType::Flatten);
        f.Shape = static_cast<uint32>(TerrainECS::ModifierGpuShape::Circle);
        f.CenterX = cx;
        f.CenterZ = 46.5f;
        f.Radius = 15.0f;
        f.Falloff = 0.0f;
        f.NoiseFreqOrTarget = 20.0f; // target height (in-band for heightScale 8 -> norm 2.5, above band top)
        return f;
    };
    const float aCx = 30.0f, bCx = 63.0f;
    const TerrainECS::ModifierGpu flattenA = makeFlatten(aCx);
    const TerrainECS::ModifierGpu flattenB = makeFlatten(bCx);

    // old-union-new footprint in sample space (both circles' world AABBs, +2 sample pad, clamped).
    const int last = static_cast<int>(HeightBakeHarness::kTileRes) - 1;
    const float r = 15.0f;
    const float unionMinX = std::min(aCx, bCx) - r, unionMaxX = std::max(aCx, bCx) + r;
    const float unionMinZ = 46.5f - r, unionMaxZ = 46.5f + r;
    const int rectMinX = std::max(0, static_cast<int>(std::floor((unionMinX - originX) / spacing)) - 2);
    const int rectMaxX = std::min(last, static_cast<int>(std::ceil((unionMaxX - originX) / spacing)) + 2);
    const int rectMinZ = std::max(0, static_cast<int>(std::floor((unionMinZ - originZ) / spacing)) - 2);
    const int rectMaxZ = std::min(last, static_cast<int>(std::ceil((unionMaxZ - originZ) / spacing)) + 2);
    // Teeth: the region must be a proper sub-rect, else it degenerates to a full re-bake and the
    // "outside-rect texels inherited from pass A" property this test exists to prove is vacuous.
    ASSERT_GT(rectMinX, 0);
    ASSERT_LT(rectMaxX, last);

    const auto fullPush = HeightBakeHarness::FullTilePush(originX, originZ, tileSize, heightScale, 2u);
    TerrainHeightBakePush regionPush = fullPush;
    regionPush.RectMinX = rectMinX;
    regionPush.RectMinZ = rectMinZ;
    regionPush.RectW = static_cast<uint32>(rectMaxX - rectMinX + 1);
    regionPush.RectH = static_cast<uint32>(rectMaxZ - rectMinZ + 1);

    // Probe the committed range from a plain full bake at B (both the reference and the accumulated
    // sequence normalize splat against it — parity is independent of the value).
    const std::vector<float> probe = h.Bake(fullPush, {noise, flattenB});
    float minH = 0.0f, maxH = 0.0f;
    InteriorHeightRange(probe, minH, maxH);
    ASSERT_LT(minH, maxH) << "probe range is degenerate";

    // The SHIPPED DEFAULT ROWS, as a global volume. Rules are the only thing that puts material
    // on the splat now, so without them both arms would be all zero and every splat assertion
    // below would hold for the trivial reason. The defaults also exercise all four channels.
    auto defaults = TerrainECS::MakeDefaultTerrainSurfaceRules();
    std::vector<GameEngine::Components::TerrainSurfaceRule> defaultRows(
        defaults.Rules, defaults.Rules + defaults.RuleCount);

    TerrainECS::ResolvedModifier rulesVol{};
    rulesVol.ModType = TerrainECS::ResolvedModifier::Type::Volume;
    rulesVol.Shape = GameEngine::Components::TerrainModifierShape::Rectangle;
    rulesVol.GlobalScope = true;
    rulesVol.Weight = 1.0f;
    TerrainECS::ResolvedEffect rulesFx{};
    rulesFx.EffectKind = TerrainECS::ResolvedEffect::Kind::Rules;
    rulesFx.Rules.Rules = defaultRows.data();
    rulesFx.Rules.RuleCount = static_cast<uint32>(defaultRows.size());
    rulesVol.Effects.push_back(rulesFx);

    std::vector<TerrainECS::SurfaceRuleGpu> gpuRules;
    std::vector<TerrainECS::SurfaceRuleConditionGpu> gpuConditions;
    ASSERT_TRUE(TerrainECS::PackSurfaceRulesForGpuSplat({rulesVol}, gpuRules, gpuConditions));

    // Reference: one full-tile bake at B.
    const SplatBakeResult full =
        h.BakeHeightAndSplat(fullPush, {noise, flattenB}, minH, maxH, gpuRules, gpuConditions);
    // Accumulated drag: full bake at A, then a region re-bake at B over old-union-new.
    const SplatBakeResult seq = h.BakeHeightAndSplatSequence(
        {{fullPush, {noise, flattenA}}, {regionPush, {noise, flattenB}}}, minH, maxH,
        gpuRules, gpuConditions);

    // (1) Moving-modifier restore: the accumulated atlas equals the full bake at B, bit-exact for
    // height (stateless per world position) and byte-exact for the derived splat.
    double maxHeightDiff = 0.0;
    int maxSplatDiff = 0;
    uint32_t layerHits[4] = {0, 0, 0, 0};
    for (uint32_t z = 0; z < HeightBakeHarness::kTileRes; ++z)
    {
        for (uint32_t x = 0; x < HeightBakeHarness::kTileRes; ++x)
        {
            const size_t hi = HeightBakeHarness::InteriorIndex(x, z);
            maxHeightDiff = std::max(maxHeightDiff,
                std::abs(static_cast<double>(seq.Height[hi]) - full.Height[hi]));
            for (uint32_t c = 0; c < 4u; ++c)
            {
                const int s = seq.SplatRGBA[hi * 4u + c];
                const int f = full.SplatRGBA[hi * 4u + c];
                maxSplatDiff = std::max(maxSplatDiff, std::abs(s - f));
                if (f > 20) ++layerHits[c];
            }
        }
    }
    const uint32_t layersPresent = (layerHits[0] > 0) + (layerHits[1] > 0) +
                                   (layerHits[2] > 0) + (layerHits[3] > 0);
    EXPECT_GE(layersPresent, 3u) << "splat field uses <3 layers; the parity check would be vacuous";
    EXPECT_EQ(maxHeightDiff, 0.0)
        << "moving-modifier region sequence diverged from the full bake at B (height max="
        << maxHeightDiff << ") -> the old footprint did not fully restore";
    EXPECT_EQ(maxSplatDiff, 0)
        << "moving-modifier region sequence splat diverged from the full bake at B (max="
        << maxSplatDiff << " LSB)";

    // (2) CPU-parity anchor at the RGBA8 floor: rebuild a CPU heightfield from the accumulated GPU
    // heights and bake the SAME rows over the same unbaked base and committed range.
    Terrain::HeightfieldData hf(HeightBakeHarness::kTileRes, HeightBakeHarness::kTileRes);
    for (uint32_t z = 0; z < HeightBakeHarness::kTileRes; ++z)
        for (uint32_t x = 0; x < HeightBakeHarness::kTileRes; ++x)
            hf.SetSample(x, z, seq.Height[HeightBakeHarness::InteriorIndex(x, z)]);
    std::vector<uint8> cpuSplat;
    uint32 sw = 0, sh = 0;
    TerrainECS::ResetSplatmap(hf, cpuSplat, sw, sh);
    {
        const float heightRange = std::max(maxH - minH, 0.001f);
        for (uint32_t z = 0; z < HeightBakeHarness::kTileRes; ++z)
            for (uint32_t x = 0; x < HeightBakeHarness::kTileRes; ++x)
            {
                const float wx = originX + static_cast<float>(x) * spacing;
                const float wz = originZ + static_cast<float>(z) * spacing;
                const auto n = hf.ComputeNormal(static_cast<int32>(x), static_cast<int32>(z),
                                                spacing, spacing);
                const TerrainECS::TerrainRuleSample sample = TerrainECS::MakeTerrainRuleSample(
                    n.x, n.y, n.z, hf.GetSample(x, z), heightScale, minH, heightRange, wx, wz);
                uint8* pixel = &cpuSplat[(static_cast<size_t>(z) * sw + x) * 4];
                for (const auto& row : defaultRows)
                {
                    const float w = TerrainECS::EvaluateTerrainSurfaceRuleWeight(
                        row, sample, TerrainECS::SurfaceRuleNoiseSample);
                    if (w > 0.0f)
                        TerrainECS::CompositeSplatTexel(pixel, row.MaterialSlot, w, row.Replace);
                }
            }
    }
    int maxCpuDiff = 0;
    size_t twoPlus = 0;
    for (uint32_t z = 0; z < HeightBakeHarness::kTileRes; ++z)
        for (uint32_t x = 0; x < HeightBakeHarness::kTileRes; ++x)
        {
            const size_t gi = HeightBakeHarness::InteriorIndex(x, z) * 4u;
            const size_t ci = (static_cast<size_t>(z) * HeightBakeHarness::kTileRes + x) * 4u;
            for (uint32_t c = 0; c < 4u; ++c)
            {
                const int d = std::abs(static_cast<int>(seq.SplatRGBA[gi + c]) - cpuSplat[ci + c]);
                maxCpuDiff = std::max(maxCpuDiff, d);
                if (d >= 2) ++twoPlus;
            }
        }
    EXPECT_EQ(twoPlus, 0u) << "moving-modifier splat diverged from CPU by >=2 LSB (a math mismatch)";
    EXPECT_LE(maxCpuDiff, 1) << "moving-modifier splat diverged from CPU beyond the RGBA8 floor (max="
                             << maxCpuDiff << ")";
    GTEST_LOG_(INFO) << "MovingFlatten parity: height diff=" << maxHeightDiff
                     << ", splat-vs-fullB=" << maxSplatDiff << " LSB, splat-vs-CPU=" << maxCpuDiff
                     << " LSB, region=[" << rectMinX << "," << rectMinZ << ".." << rectMaxX << ","
                     << rectMaxZ << "], range=[" << minH << "," << maxH << "]";

    device->Shutdown();
}

// Painted-content fallback gate (no device): a bake carrying a paint-layer effect or a PaintZone
// is splat-ineligible, so the GPU splat pass is skipped and the CPU-composited splat stands
// (byte-identical) — the same predicate FlushPendingBakes gates the splat dispatch on. A pure
// height-effect (Noise/Flatten volume, or empty base-only) list is eligible, and so is a
// surface-rules volume, whose rows the packer hands to the kernel.
TEST(TerrainGpuSplatEligibility, PaintModifierForcesCpuFallback)
{
    using RM = TerrainECS::ResolvedModifier;
    auto volumeWithEffect = [](TerrainECS::ResolvedEffect::Kind kind) {
        RM vol{};
        vol.ModType = RM::Type::Volume;
        TerrainECS::ResolvedEffect fx{};
        fx.EffectKind = kind;
        vol.Effects.push_back(fx);
        return vol;
    };
    const RM noise = volumeWithEffect(TerrainECS::ResolvedEffect::Kind::Noise);
    const RM flatten = volumeWithEffect(TerrainECS::ResolvedEffect::Kind::Flatten);
    const RM paintL = volumeWithEffect(TerrainECS::ResolvedEffect::Kind::PaintLayer);
    RM paintZ{};  paintZ.ModType = RM::Type::PaintZone;

    std::vector<TerrainECS::SurfaceRuleGpu> rules;
    std::vector<TerrainECS::SurfaceRuleConditionGpu> conditions;
    auto allows = [&](const std::vector<RM>& mods) {
        return TerrainECS::PackSurfaceRulesForGpuSplat(mods, rules, conditions);
    };

    EXPECT_TRUE(allows({})) << "empty (base-only) bake must be splat-eligible";
    EXPECT_TRUE(allows({noise, flatten}))
        << "pure procedural (Noise/Flatten volume) bake must be splat-eligible";
    EXPECT_FALSE(allows({noise, paintL}))
        << "a paint-layer effect must force the CPU splat fallback";
    EXPECT_FALSE(allows({paintZ})) << "a PaintZone modifier must force the CPU splat fallback";
    EXPECT_FALSE(allows({noise, flatten, paintZ}))
        << "any paint writer in the list must force the CPU splat fallback";

    // A rules volume is expressible now, and packs its row rather than being refused.
    GameEngine::Components::TerrainSurfaceRule row{};
    row.MaterialSlot = 3;
    RM rulesVolume{};
    rulesVolume.ModType = RM::Type::Volume;
    rulesVolume.Shape = GameEngine::Components::TerrainModifierShape::Circle;
    TerrainECS::ResolvedEffect fx{};
    fx.EffectKind = TerrainECS::ResolvedEffect::Kind::Rules;
    fx.Rules.Rules = &row;
    fx.Rules.RuleCount = 1;
    rulesVolume.Effects.push_back(fx);
    EXPECT_TRUE(allows({noise, rulesVolume}))
        << "a surface-rules volume must be splat-eligible — the kernel reads its rows";
    EXPECT_EQ(rules.size(), 1u);
}

// ── C12 module-reload live fire (device-backed) ────────────────────────────
// A module-registered node type whose module reloads while a spine-owned
// pipeline instance holds its node object. The reconcile must destroy the
// instance BEFORE the unload decision (the node's vtable lives in the
// superseded image) and the next app frame must rebuild the instance from
// the REPLACED (new-generation) factory. Companion to the registry-level
// RenderPipelineNodeModuleReloadTests — this is the spine/teardown arm.
namespace
{
struct ReloadProbeCounters
{
    static inline int Constructed[3] = {0, 0, 0}; // indexed by module generation
    static inline int Destroyed[3] = {0, 0, 0};
    static void Reset()
    {
        for (int g = 0; g < 3; ++g)
            Constructed[g] = Destroyed[g] = 0;
    }
};

class ReloadProbeNode final : public IRenderPipelineNode
{
public:
    explicit ReloadProbeNode(int generation) : m_Generation(generation)
    {
        ++ReloadProbeCounters::Constructed[m_Generation];
    }
    ~ReloadProbeNode() override { ++ReloadProbeCounters::Destroyed[m_Generation]; }
    const char* GetTypeName() const override { return "ReloadProbe"; }
    bool Initialize(std::string, std::string, std::string*) override { return true; }

private:
    int m_Generation;
};
} // namespace

TEST(RenderPipelineDeclareTests, ModuleReloadReconcileTearsDownAndRebuildsInstances)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        const CameraId camId = rs.Views().AllocateCamera("ReloadCam");
        const ViewId viewId = rs.Views().AllocateView("ReloadView", camId);
        rs.Views().SetViewRenderLayerMask(viewId, 1u);
        rs.Views().SetViewTargets(viewId, 0, 0, 0, Rendering::ViewClearConfig{});

        ReloadProbeCounters::Reset();
        {
            ECS::SetActiveRegistrationModule("ProbePack", 1);
            ASSERT_TRUE(rs.Spine().GetPipelineNodeRegistry()->Register(
                "ReloadProbe", [] { return std::make_unique<ReloadProbeNode>(1); }, true));
            ECS::ClearActiveRegistrationModule();
        }

        RenderPipelineBlueprint bp;
        bp.pipelineName = "ReloadTest";
        bp.contentHash = 0xC12C12ull;
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "Probe";
            p.type = "ReloadProbe";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"Probe","type":"ReloadProbe"})";
            bp.passes.push_back(p);
        }
        rs.Spine().SetActiveRenderPipelineBlueprint(bp);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        GameEngine::Testing::ScopedPipelineFrame frameRegistration(rs.Spine(), frame);
        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("RL.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("RL.Depth", DepthTargetDesc());

        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();
        const ViewTargetsRG vt0{viewId, color, depth, {}};
        RenderServices::FrameGraphBuildParamsRG params{};
        params.ViewTargets = std::span<const ViewTargetsRG>(&vt0, 1);
        rs.Spine().BuildFrameGraph(frame, params);

        EXPECT_EQ(ReloadProbeCounters::Constructed[1], 1)
            << "first declare must instantiate the generation-1 node";
        EXPECT_EQ(ReloadProbeCounters::Destroyed[1], 0);
        EXPECT_NE(rs.Spine().PipelineInstanceForFrame(frame), nullptr);
        frame.Execute();
        device->WaitForIdle();

        // Module reload: the replay re-registers the type under generation 2
        // (replace-in-place), then the loader's reconcile fan-out fires —
        // exactly the LoadModule ordering (replay, reconcile, unload check).
        {
            ECS::SetActiveRegistrationModule("ProbePack", 2);
            EXPECT_TRUE(rs.Spine().GetPipelineNodeRegistry()->Register(
                "ReloadProbe", [] { return std::make_unique<ReloadProbeNode>(2); }, true));
            ECS::ClearActiveRegistrationModule();
        }
        rs.Spine().ReconcileModuleNodeRegistrations("ProbePack", 2);

        EXPECT_EQ(ReloadProbeCounters::Destroyed[1], 1)
            << "reconcile must destroy the gen-1 node while its image is still mapped";
        EXPECT_EQ(rs.Spine().PipelineInstanceForFrame(frame), nullptr)
            << "the live instance goes down with its nodes";
        EXPECT_EQ(rs.Spine().CountSupersededModulePipelineNodes("ProbePack", 2), 0u)
            << "the re-owned entry must not block the unload ledger";

        // Next app frame: the stream slot re-creates and instantiates from the
        // reconciled registry — the NEW generation's factory.
        frame.BeginFrame(1);
        color = frame.ImportPersistentTexture("RL.Color", ColorTargetDesc());
        depth = frame.ImportPersistentTexture("RL.Depth", DepthTargetDesc());
        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();
        const ViewTargetsRG vt1{viewId, color, depth, {}};
        params.ViewTargets = std::span<const ViewTargetsRG>(&vt1, 1);
        rs.Spine().BuildFrameGraph(frame, params);

        EXPECT_EQ(ReloadProbeCounters::Constructed[2], 1)
            << "rebuild must dispatch the generation-2 factory";
        EXPECT_EQ(ReloadProbeCounters::Destroyed[2], 0);
        EXPECT_EQ(ReloadProbeCounters::Constructed[1], 1)
            << "the superseded factory must never run again";
        EXPECT_NE(rs.Spine().PipelineInstanceForFrame(frame), nullptr);
        frame.Execute();
        device->WaitForIdle();
        rs.Shutdown();
    }
    device->Shutdown();
}

// The declaration-time binding table binds ge_gtao / ge_sceneColor
// descriptor-direct — the ONLY render-graph edge for those samples is the
// keyword-gated Read the world pass declares (merged #82 for GTAO; the
// transmission grab followed). Without the read the AO chain's storage write
// (and the grab copy) have no RAW edge into the draw: under the async-compute
// queue map nothing creates the graphics-queue wait, and the texture stays
// GENERAL under a SHADER_READ_ONLY claim (VUID-vkCmdDraw-None-09600). These
// pins lock the reads in BOTH pass arms, plus the negative (keyword off ->
// no read, matching the binding gate).
TEST(RenderPipelineDeclareTests, WorldPassDeclaresGtaoAndSceneGrabReads)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        const CameraId camId = rs.Views().AllocateCamera("GtaoReadCam");
        const ViewId viewId = rs.Views().AllocateView("GtaoReadView", camId);
        rs.Views().SetViewRenderLayerMask(viewId, 1u);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        clear.clearDepthValue = 0.0f;
        rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

        auto gtaoDesc = ColorTargetDesc();
        gtaoDesc.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
        gtaoDesc.usage = static_cast<uint32_t>(TextureUsage::UnorderedAccess | TextureUsage::ShaderResource);
        auto grabDesc = ColorTargetDesc();
        grabDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource);

        const Rendering::MaterialKeyword kBoth =
            Rendering::MaterialKeyword::GTAO | Rendering::MaterialKeyword::SceneColorGrab;

        struct FrameSetup
        {
            RenderGraph::RGTexture Color, Depth, Gtao, Grab;
            RenderServices::WorldPassTargetsRG Targets;
        };
        auto beginFrame = [&](uint32_t index) -> FrameSetup
        {
            frame.BeginFrame(index);
            FrameSetup s{};
            s.Color = frame.ImportPersistentTexture("GR.Color", ColorTargetDesc());
            s.Depth = frame.ImportPersistentTexture("GR.Depth", DepthTargetDesc());
            s.Gtao = frame.ImportPersistentTexture("GR.GTAO", gtaoDesc);
            s.Grab = frame.ImportPersistentTexture("GR.SceneGrab", grabDesc);
            rs.BeginWorldDrawFrame();
            rs.BuildWorldBatchKeys();
            s.Targets.Color = s.Color;
            s.Targets.Depth = s.Depth;
            s.Targets.GTAO = s.Gtao;
            s.Targets.SceneGrab = s.Grab;
            return s;
        };

        // Frame 0: entity world pass, keywords ON -> both reads declared.
        {
            const FrameSetup s = beginFrame(0);
            const auto r = rs.AddWorldPassForView(frame, viewId, s.Targets, kBoth);
            ASSERT_TRUE(r.Pass.IsValid()) << "world pass declares (view clears are set)";
            EXPECT_TRUE(frame.Graph().HasReadAccess(r.Pass.Id, s.Gtao.Id))
                << "GTAO keyword + valid target must declare the sampled read";
            EXPECT_TRUE(frame.Graph().HasReadAccess(r.Pass.Id, s.Grab.Id))
                << "SceneColorGrab keyword + valid target must declare the sampled read";
        }

        // Frame 1: entity world pass, keywords OFF -> no reads (matches the
        // binding gate: no ge_gtao / ge_sceneColor binding, no edge needed).
        {
            const FrameSetup s = beginFrame(1);
            const auto r =
                rs.AddWorldPassForView(frame, viewId, s.Targets, Rendering::MaterialKeyword::None);
            ASSERT_TRUE(r.Pass.IsValid());
            EXPECT_FALSE(frame.Graph().HasReadAccess(r.Pass.Id, s.Gtao.Id))
                << "keyword off: the pass must not read (or transition) the AO texture";
            EXPECT_FALSE(frame.Graph().HasReadAccess(r.Pass.Id, s.Grab.Id))
                << "keyword off: the pass must not read (or transition) the grab";
        }

        // Frame 2: forward-command drain arm, keywords ON -> same reads. A
        // default-constructed command is enough — declaration never inspects
        // the command payload (exec-time resolution skips invalid handles).
        {
            const FrameSetup s = beginFrame(2);
            const DrawCommand dummy{};
            const auto pass = rs.AddForwardCommandPassForView(
                frame, viewId, s.Targets, kBoth, std::span<const DrawCommand>(&dummy, 1),
                {}, "GtaoReadProbe");
            ASSERT_TRUE(pass.IsValid());
            EXPECT_TRUE(frame.Graph().HasReadAccess(pass.Id, s.Gtao.Id))
                << "drain arm: GTAO read must mirror the entity world pass";
            EXPECT_TRUE(frame.Graph().HasReadAccess(pass.Id, s.Grab.Id))
                << "drain arm: SceneColorGrab read must mirror the entity world pass";
        }

        // Frame 3: drain arm, keywords OFF -> no reads.
        {
            const FrameSetup s = beginFrame(3);
            const DrawCommand dummy{};
            const auto pass = rs.AddForwardCommandPassForView(
                frame, viewId, s.Targets, Rendering::MaterialKeyword::None,
                std::span<const DrawCommand>(&dummy, 1), {}, "GtaoReadProbeOff");
            ASSERT_TRUE(pass.IsValid());
            EXPECT_FALSE(frame.Graph().HasReadAccess(pass.Id, s.Gtao.Id));
            EXPECT_FALSE(frame.Graph().HasReadAccess(pass.Id, s.Grab.Id));
        }

        device->WaitForIdle();
        rs.Shutdown();
    }
    device->Shutdown();
}

// ── HZB mip-view lifetime: two HZB views through ONE HZBBuildNode ───────────
// perView=true on a pipeline node means "declare once per view", NOT "one node
// object per view" (RenderPipeline.cpp's node loop calls DeclareForView on the
// single instance for every view target). The editor puts the Scene View and
// the Game View into ONE BuildFrameGraph call, so both views' HZB pyramids are
// built by the same node object in the same frame — the shape pinned here.
namespace
{
// Both views run SSSR, so each imports its own MAX Hi-Z pyramid under its own
// per-view pool name and the node builds both in one frame. A probe node
// downstream reads each pyramid so the culler keeps the build passes alive.
struct HzbTwoViewRig
{
    RenderServices Services;
    RenderPipelineNodeRegistry Registry;
    std::unique_ptr<RenderPipelineInstance> Instance;
    std::unique_ptr<FramePools> Pools;
    Rendering::ViewId ViewA = 0;
    Rendering::ViewId ViewB = 0;

    bool Init(Rendering::IDevice* device)
    {
        if (!Services.Initialize(device))
            return false;
        const CameraId camId = Services.Views().AllocateCamera("HzbCam");
        CameraData cd{};
        for (int i = 0; i < 16; i += 5)
        {
            cd.view[i] = 1.0f;
            cd.proj[i] = 1.0f;
            cd.viewProj[i] = 1.0f;
        }
        Services.Views().SetCameraData(camId, cd);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        PostProcessSettings pp{};
        pp.SSSRIntensity = 1.0f;

        ViewA = Services.Views().AllocateView("HzbViewA", camId);
        ViewB = Services.Views().AllocateView("HzbViewB", camId);
        for (Rendering::ViewId v : {ViewA, ViewB})
        {
            Services.Views().SetViewRenderLayerMask(v, 1u);
            Services.Views().SetViewTargets(v, 0, 0, 0, clear);
            Services.Views().SetViewPostProcessOverride(v, pp);
        }

        if (!Registry.Register(
                "HZBBuild", [] { return std::make_unique<Nodes::HZBBuildNode>(); }, true))
            return false;

        RenderPipelineBlueprint bp;
        bp.pipelineName = "HzbTwoView";
        RenderPipelineBlueprint::Pass hzb;
        hzb.id = "HZB";
        hzb.type = "HZBBuild";
        hzb.enabled = true;
        hzb.perView = true;
        hzb.passJson = R"({"id":"HZB","type":"HZBBuild"})";
        bp.passes.push_back(hzb);
        bp.resources.push_back(
            {"View.DepthResolved",
             R"({"kind":"texture","scope":"perView","format":"R32_FLOAT","extent":{"scale":[1,1]},"usage":["shaderResource","unorderedAccess"]})"});

        Instance = std::make_unique<RenderPipelineInstance>(Services, Registry);
        Instance->SetBlueprint(bp);
        Pools = std::make_unique<FramePools>(device);
        return true;
    }

    // Declares + executes one frame with BOTH views in a single view span.
    // Returns the number of texture-view destroys the device queued during
    // Execute — the lifetime signal: a mip view still referenced by this
    // frame's recorded descriptor sets must not be destroyed mid-frame.
    struct FrameResult
    {
        size_t QueuedViewDestroysDuringExecute = 0;
        // Widened bracket: a desc-change realloc happens at IMPORT, before
        // Execute, so a resize that frees the outgoing views too early is
        // invisible to the Execute-only counter above.
        size_t QueuedViewDestroysDuringFrame = 0;
        size_t ViewsCreatedDuringExecute = 0;
        size_t LiveTextureViewsAfter = 0;
        size_t HzbPassCount = 0;
    };

    // The HZB extent follows the view's depth target — View.DepthResolved
    // publishes the raster depth — so a per-view target size is how a resize is
    // driven from here.
    static TextureDesc TargetAt(TextureDesc d, uint32_t size)
    {
        d.width = size;
        d.height = size;
        return d;
    }

    FrameResult RunFrame(Rendering::IDevice* device, uint64_t frameIndex, uint32_t sizeA = 64u,
                         uint32_t sizeB = 64u)
    {
        FrameResult r{};
        const auto frameStart = device->GetResourcePoolStats();
        RenderGraph::RGFrame frame(device, &Pools->Persistent, &Pools->Transient, &Pools->Ring);
        frame.BeginFrame(frameIndex);

        const RenderGraph::RGTexture colorA =
            frame.ImportPersistentTexture("Hzb.ColorA", TargetAt(ColorTargetDesc(), sizeA));
        const RenderGraph::RGTexture depthA =
            frame.ImportPersistentTexture("Hzb.DepthA", TargetAt(DepthTargetDesc(), sizeA));
        const RenderGraph::RGTexture colorB =
            frame.ImportPersistentTexture("Hzb.ColorB", TargetAt(ColorTargetDesc(), sizeB));
        const RenderGraph::RGTexture depthB =
            frame.ImportPersistentTexture("Hzb.DepthB", TargetAt(DepthTargetDesc(), sizeB));

        Services.BeginWorldDrawFrame();
        Services.BuildWorldBatchKeys();

        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{ViewA, colorA, depthA, {}},
                                                    ViewTargetsRG{ViewB, colorB, depthB, {}}};
        const std::vector<Rendering::ViewDesc> views(Services.Views().GetViews().begin(),
                                                     Services.Views().GetViews().end());
        Instance->Declare(frame, targets, views);

        const auto* fr = Instance->FrameResourcesFor(&frame);
        if (fr)
        {
            for (Rendering::ViewId v : {ViewA, ViewB})
            {
                const auto it = fr->Textures.find({v, std::string(Names::View::SSRHiZ)});
                if (it != fr->Textures.end() && it->second.IsValid())
                    frame.MarkOutput(it->second);
            }
        }
        r.HzbPassCount = RGQuery::CountDeclared(frame.Graph(), RGQuery::Family{"HZBBuild"});

        const auto before = device->GetResourcePoolStats();
        frame.Execute();
        const auto after = device->GetResourcePoolStats();
        r.QueuedViewDestroysDuringExecute =
            after.deferredTextureViews > before.deferredTextureViews
                ? after.deferredTextureViews - before.deferredTextureViews
                : 0;
        r.QueuedViewDestroysDuringFrame =
            after.deferredTextureViews > frameStart.deferredTextureViews
                ? after.deferredTextureViews - frameStart.deferredTextureViews
                : 0;
        r.ViewsCreatedDuringExecute = after.liveTextureViews > before.liveTextureViews
                                          ? after.liveTextureViews - before.liveTextureViews
                                          : 0;
        r.LiveTextureViewsAfter = after.liveTextureViews;
        device->WaitForIdle();
        return r;
    }
};
} // namespace

// The lifetime pin. Two views, one node, one frame: the second view's execute
// callback must not destroy the first view's single-mip image views — they are
// already written into this frame's recorded descriptor sets, so destroying
// them makes the submitted command buffer reference a dead VkImageView.
TEST(RenderPipelineDeclareTests, TwoHzbViewsInOneFrameKeepBothViewsMipViewsAlive)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        HzbTwoViewRig rig;
        ASSERT_TRUE(rig.Init(device.get()));

        const auto f0 = rig.RunFrame(device.get(), 0);
        ASSERT_GT(f0.HzbPassCount, 0u)
            << "the rig declared no HZB build passes — the shaderpkgs or the SSSR gate did not "
               "resolve, so this test proves nothing";

        EXPECT_EQ(f0.QueuedViewDestroysDuringExecute, 0u)
            << "executing two views' HZB builds destroyed image views mid-frame: the second "
               "view's build evicted the first view's mip views while this frame's descriptor "
               "sets still reference them";

        // Positive control, and it has to be here: HzbPassCount is counted from
        // DECLARED pass names, so it stays >0 even when every execute callback
        // bails on an invalid view, and "zero destroys" is satisfied just as
        // well by a frame that never created one. Both views must actually
        // build a pyramid — two 64x64 chains, 7 single-mip views each. The pool
        // is the render-graph layer's only view-creation site, so this counts
        // the mip views and nothing else the graph owns.
        constexpr size_t kPooledMipViewsPerFrameZero = 14u; // 2 views x 7 mips
        ASSERT_GE(f0.ViewsCreatedDuringExecute, kPooledMipViewsPerFrameZero)
            << "the first frame created " << f0.ViewsCreatedDuringExecute
            << " image views, fewer than the " << kPooledMipViewsPerFrameZero
            << " single-mip views two HZB pyramids need — the passes ran but built nothing, so "
               "the zero-destroy result above means nothing was at risk, not that nothing was "
               "destroyed";

        // Steady state: a second and third frame over unchanged physicals must
        // create nothing and destroy nothing — the per-frame churn the shared
        // cache produced (a full mip chain recreated per view per frame).
        const auto f1 = rig.RunFrame(device.get(), 1);
        const auto f2 = rig.RunFrame(device.get(), 2);
        EXPECT_EQ(f1.QueuedViewDestroysDuringExecute, 0u);
        EXPECT_EQ(f2.QueuedViewDestroysDuringExecute, 0u);
        EXPECT_EQ(f1.ViewsCreatedDuringExecute, 0u)
            << "the pooled mip views were rebuilt on an unchanged physical — the pool is not "
               "caching, it is churning";
        EXPECT_EQ(f2.ViewsCreatedDuringExecute, 0u);
        EXPECT_EQ(f2.LiveTextureViewsAfter, f1.LiveTextureViewsAfter)
            << "the live image-view count must reach steady state across identical frames";

        rig.Services.Shutdown();
    }
    device->Shutdown();
}

// Resize one of two live views. The pyramid the resize replaces is reallocated
// during DECLARE, but the image views of the outgoing physical are still named
// by descriptor sets recorded into frames the GPU has not finished — so they
// must survive the resize frame, and be released once the in-flight window has
// passed. Neither half is visible to validation on the shipped descriptor-buffer
// path: with no VkDescriptorSet to attribute, an early free raises nothing at
// all, so the destroy count is the instrument, not the validation log.
TEST(RenderPipelineDeclareTests, HzbMipViewsOfAResizedViewOutliveTheFrameThatReplacedThem)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        HzbTwoViewRig rig;
        ASSERT_TRUE(rig.Init(device.get()));
        const uint32_t framesInFlight = rig.Pools->Ring.FramesInFlight();
        ASSERT_EQ(framesInFlight, 2u)
            << "the retire arithmetic below is written for a 2-frame window";

        constexpr uint32_t kSmall = 64u;  // 7 mips
        constexpr uint32_t kLarge = 128u; // 8 mips
        constexpr size_t kSmallChain = 7u;

        const auto f0 = rig.RunFrame(device.get(), 0, kSmall, kSmall);
        ASSERT_GT(f0.HzbPassCount, 0u)
            << "the rig declared no HZB build passes — the shaderpkgs or the SSSR gate did not "
               "resolve, so this test proves nothing";
        const auto f1 = rig.RunFrame(device.get(), 1, kSmall, kSmall);
        ASSERT_EQ(f1.ViewsCreatedDuringExecute, 0u) << "not in steady state before the resize";
        const size_t steadyBefore = f1.LiveTextureViewsAfter;

        // View A resizes; view B keeps rendering at its old size in the same frame.
        const auto f2 = rig.RunFrame(device.get(), 2, kLarge, kSmall);
        EXPECT_EQ(f2.QueuedViewDestroysDuringFrame, 0u)
            << "the resize frame destroyed image views: the outgoing pyramid's mip views are "
               "still referenced by descriptor sets in frames the GPU has not retired";
        ASSERT_GE(f2.ViewsCreatedDuringExecute, kSmallChain + 1u)
            << "the resized view did not rebuild a pyramid, so nothing was actually at risk";

        // Still inside the in-flight window: the outgoing chain stays alive.
        const auto f3 = rig.RunFrame(device.get(), 3, kLarge, kSmall);
        EXPECT_EQ(f3.QueuedViewDestroysDuringFrame, 0u)
            << "the outgoing pyramid was freed before its in-flight window elapsed";
        EXPECT_EQ(f3.ViewsCreatedDuringExecute, 0u)
            << "the resized pyramid is churning instead of caching";

        // ...and then it must actually go: held forever is a leak, not safety.
        size_t retired = f3.QueuedViewDestroysDuringFrame;
        size_t lastLive = 0;
        size_t lastCreated = 0;
        for (uint64_t f = 4; f <= 6; ++f)
        {
            const auto r = rig.RunFrame(device.get(), f, kLarge, kSmall);
            retired += r.QueuedViewDestroysDuringFrame;
            lastLive = r.LiveTextureViewsAfter;
            lastCreated = r.ViewsCreatedDuringExecute;
        }
        EXPECT_EQ(retired, kSmallChain)
            << "expected the outgoing 7-mip chain to be released exactly once after its retire "
               "window; got "
            << retired;
        EXPECT_EQ(lastCreated, 0u) << "steady state never returned after the resize";
        EXPECT_EQ(lastLive, steadyBefore + 1u)
            << "a 64x64 chain (7 mips) became a 128x128 chain (8 mips), so exactly one more "
               "image view should be live; "
            << lastLive << " vs " << steadyBefore;

        rig.Services.Shutdown();
    }
    device->Shutdown();
}

// The PCSS early-out reaches the pyramid through a BINDLESS index, and a
// bindless fetch declares nothing. Without a Read on the pyramid, the
// reduction's storage writes and the fragment's fetches have no memory
// dependency between them at all — a race, not a stall, and one that renders
// perfectly on whichever GPU happens to finish the dispatches first. Pixel
// comparison is therefore no evidence either way, which is exactly why the edge
// is pinned structurally: the pass declares the read, and the barrier the graph
// derives from it is compute-write -> fragment-read.
//
// BOTH world arms are pinned. AddWorldPassImpl draws the entity batches;
// AddForwardCommandPassForView drains contributor commands (ocean, transmissive,
// sorted transparents) through the same adapter_forward include and therefore
// the same early-out. The two carry identical code, which is exactly why a test
// covering one of them is no test at all for the other.
//
// Each arm gets its OWN frame, and in that frame it is the pyramid's only
// consumer. That is not tidiness: barrier batches are per pass, so a second
// reader in the same frame is read-after-read at an unchanged layout and gets no
// barrier of its own — two consumers in one frame would let either arm pass on
// the other's edge.
//
// GENERAL on both sides, not a transition: the pyramid is storage-written and
// sampled through descriptors that claim GENERAL
// (TextureDesc::sampledInGeneralLayout), so a ShaderReadOnly transition here
// would contradict the bindless descriptor's own claim.
TEST(RenderPipelineDeclareTests, BothWorldArmsReadThePcssPyramidSoTheReductionOrdersBeforeThem)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        if (!rs.Textures().IsBindlessEnabled())
        {
            rs.Shutdown();
            device->Shutdown();
            GTEST_SKIP() << "PCSS demotes to Poisson without bindless — no pyramid to order";
        }

        const CameraId camId = rs.Views().AllocateCamera("PyramidEdgeCam");
        CameraData cd{};
        cd.proj[0] = 1.0f;
        cd.proj[5] = 1.0f;
        cd.proj[10] = 0.001f;
        cd.proj[11] = 1.0f;
        cd.proj[14] = 0.1f;
        for (int i = 0; i < 16; i += 5)
        {
            cd.view[i] = 1.0f;
            cd.viewProj[i] = 1.0f;
        }
        rs.Views().SetCameraData(camId, cd);
        const ViewId viewId = rs.Views().AllocateView("PyramidEdgeView", camId);
        rs.Views().SetViewRenderLayerMask(viewId, 1u);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);

        auto depHandle = rs.RegisterDepthEmit(
            [&](DepthEmitContext& ctx, DepthPassType passType)
            {
                if (ctx.ViewId == viewId && passType == DepthPassType::ShadowCascade)
                    ctx.Services->EmitDepthCommand(ctx.ViewId, passType, DrawCommand{});
            });

        RenderPipelineNodeRegistry registry;
        ASSERT_TRUE(registry.Register(
            "ShadowMap", [] { return std::make_unique<Nodes::ShadowMapNode>(); }, true));
        ASSERT_TRUE(registry.Register(
            "WorldRender", [] { return std::make_unique<Nodes::WorldRenderNode>(); }, true));

        RenderPipelineBlueprint::Pass shadowPass;
        shadowPass.id = "CSM";
        shadowPass.type = "ShadowMap";
        shadowPass.enabled = true;
        shadowPass.perView = true;
        shadowPass.passJson = R"({"id":"CSM","type":"ShadowMap","buffer":"ShadowData"})";

        RenderPipelineBlueprint::Pass worldPass;
        worldPass.id = "World";
        worldPass.type = "WorldRender";
        worldPass.enabled = true;
        worldPass.perView = true;
        worldPass.passJson = R"({"id":"World","type":"WorldRender","keywords":["Shadows"]})";

        RenderPipelineBlueprint bpFull;
        bpFull.pipelineName = "PyramidEdgeTest";
        bpFull.passes.push_back(shadowPass);
        bpFull.passes.push_back(worldPass);

        // Cascades and pyramid, no entity world pass: the forward arm below is
        // then the pyramid's only consumer, so the barrier it carries is
        // unambiguously its own. ViewNeedsShadowCascadePasses asks only for a
        // live render-layer mask and a shadow caster, so dropping WorldRender
        // does not disarm the pyramid.
        RenderPipelineBlueprint bpShadowOnly;
        bpShadowOnly.pipelineName = "PyramidEdgeTestShadowOnly";
        bpShadowOnly.passes.push_back(shadowPass);

        RenderPipelineInstance instanceFull(rs, registry);
        instanceFull.SetBlueprint(bpFull);
        RenderPipelineInstance instanceShadowOnly(rs, registry);
        instanceShadowOnly.SetBlueprint(bpShadowOnly);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

        ExtractedLight sun{};
        sun.type = GameEngine::Components::LightType::Directional;
        sun.castsShadows = 1;
        sun.castsLight = 1;
        sun.directionWS[1] = -1.0f;
        sun.cascadeCount = 4;

        const std::vector<Rendering::ViewDesc> views(rs.Views().GetViews().begin(),
                                                     rs.Views().GetViews().end());

        auto nameHas = [](const char* name, const char* needle)
        { return name && std::string(name).find(needle) != std::string::npos; };

        auto findPyramid = [&]() -> RenderGraph::RGResourceId
        {
            for (size_t r = 0; r < frame.Graph().ResourceCount(); ++r)
            {
                const auto id = static_cast<RenderGraph::RGResourceId>(r);
                if (nameHas(frame.Graph().ResourceName(id), "ShadowMinMaxPyramid"))
                    return id;
            }
            return RenderGraph::kInvalidId;
        };
        auto findPass = [&](const char* needle) -> RenderGraph::RGPassId
        {
            for (size_t p = 0; p < frame.Graph().PassCount(); ++p)
            {
                const auto id = static_cast<RenderGraph::RGPassId>(p);
                if (nameHas(frame.Graph().PassName(id), needle))
                    return id;
            }
            return RenderGraph::kInvalidId;
        };

        // One arm's whole contract: it declares the read, it survives cull, and
        // the batch emitted before it carries the reduction's compute-write ->
        // fragment-read edge at GENERAL. Called AFTER Execute — declared accesses
        // stay queryable until the next BeginFrame, and the barriers exist only
        // once the frame has compiled.
        auto expectOrdersTheReduction = [&](RenderGraph::RGPassId pass,
                                            RenderGraph::RGResourceId pyramid, const char* arm)
        {
            const auto& graph = frame.Graph();
            ASSERT_NE(pass, RenderGraph::kInvalidId) << arm << ": no pass to carry the edge";
            ASSERT_NE(pyramid, RenderGraph::kInvalidId)
                << arm << ": no pyramid was declared, so this would pass without proving anything";
            EXPECT_FALSE(graph.IsCulled(pass)) << arm << ": a culled pass carries no barriers";
            EXPECT_TRUE(graph.HasReadAccess(pass, pyramid))
                << arm
                << ": the pass must declare the pyramid read — the shader's bindless fetch forms "
                   "no producer->consumer edge, so nothing else can order the reduction before it";

            bool sawComputeToFragment = false;
            for (const RenderGraph::RGBarrierBatch& batch : graph.BarrierBatches())
            {
                if (batch.Pass != pass)
                    continue;
                for (uint32_t i = batch.First; i < batch.First + batch.Count; ++i)
                {
                    const RenderGraph::RGBarrier& b = graph.Barriers()[i];
                    if (b.Resource != pyramid)
                        continue;
                    if ((b.SrcAccess & RenderGraph::RGAccessMask::ShaderWrite) != 0u &&
                        (b.DstAccess & RenderGraph::RGAccessMask::ShaderRead) != 0u &&
                        (b.SrcStage & RenderGraph::RGStage::ComputeShader) != 0u &&
                        (b.DstStage & RenderGraph::RGStage::FragmentShader) != 0u)
                    {
                        sawComputeToFragment = true;
                        EXPECT_EQ(b.NewLayout, RenderGraph::RGImageLayout::General)
                            << arm
                            << ": the pyramid's sampled descriptors claim GENERAL; a "
                               "ShaderReadOnly transition would contradict the bindless descriptor";
                    }
                }
            }
            EXPECT_TRUE(sawComputeToFragment)
                << arm
                << ": the reduction's storage writes and this pass's fetches ended up with no "
                   "memory dependency between them";
        };

        // Arm 1 — the entity world pass (AddWorldPassImpl).
        frame.BeginFrame(0);
        RenderGraph::RGTexture color =
            frame.ImportPersistentTexture("PyrEdge.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth =
            frame.ImportPersistentTexture("PyrEdge.Depth", DepthTargetDesc());
        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();
        rs.SubmitLight(0u, sun);
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{viewId, color, depth, {}}};
        instanceFull.Declare(frame, targets, views);
        if (!frame.Graph().PassCount())
            GTEST_SKIP() << "shadow shaders unavailable in this environment";
        const RenderGraph::RGPassId entityPass = findPass("RenderEntities");
        const RenderGraph::RGResourceId pyramidEntityFrame = findPyramid();
        frame.MarkOutput(color);
        frame.Execute();
        expectOrdersTheReduction(entityPass, pyramidEntityFrame, "entity world pass");
        device->WaitForIdle();

        // Arm 2 — the forward-contributor drain (AddForwardCommandPassForView),
        // declared the way the ocean and transmissive nodes declare it. A
        // default-constructed command is enough: declaration never inspects the
        // payload.
        frame.BeginFrame(1);
        color = frame.ImportPersistentTexture("PyrEdge.Color", ColorTargetDesc());
        depth = frame.ImportPersistentTexture("PyrEdge.Depth", DepthTargetDesc());
        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();
        rs.SubmitLight(0u, sun);
        const std::vector<ViewTargetsRG> targets1 = {ViewTargetsRG{viewId, color, depth, {}}};
        instanceShadowOnly.Declare(frame, targets1, views);
        ASSERT_EQ(findPass("RenderEntities"), RenderGraph::kInvalidId)
            << "the shadow-only arm must declare no entity world pass, or the forward pass could "
               "ride its barrier instead of proving its own";
        RenderServices::WorldPassTargetsRG fwdTargets{};
        fwdTargets.Color = color;
        fwdTargets.Depth = depth;
        const DrawCommand fwdCommand{};
        const auto forwardPass = rs.AddForwardCommandPassForView(
            frame, viewId, fwdTargets, Rendering::MaterialKeyword::Shadows,
            std::span<const DrawCommand>(&fwdCommand, 1), {}, "PyramidEdgeForwardProbe");
        ASSERT_TRUE(forwardPass.IsValid());
        const RenderGraph::RGResourceId pyramidForwardFrame = findPyramid();
        frame.MarkOutput(color);
        frame.Execute();
        expectOrdersTheReduction(forwardPass.Id, pyramidForwardFrame, "forward-contributor pass");
        device->WaitForIdle();

        depHandle.Reset();
        rs.Shutdown();
    }
    device->Shutdown();
}
// The per-cascade PCSS light size the shader multiplies a depth delta by is the
// primary light's tan(half angular diameter) — one physical value that does not
// vary by cascade, because a light's angular size does not depend on which
// cascade a fragment lands in. This pins both halves: that the value comes from
// the frame rather than a feature-level scalar, and that nothing decays it per
// cascade.
//
// Asserting the exact value matters more than asserting uniformity alone: the
// deleted per-cascade falloff defaulted to 1.0, so a uniformity-only test would
// have passed against the old global light size and proved nothing.
TEST(RenderPipelineDeclareTests, PcssCascadeLightSizeIsTheFrameTangentOnEveryCascade)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    const CameraId camId = rs.Views().AllocateCamera("PenumbraCam");
    CameraData cd{};
    cd.proj[0] = 1.0f;
    cd.proj[5] = 1.0f;
    cd.proj[10] = 0.001f;
    cd.proj[11] = 1.0f;
    cd.proj[14] = 0.1f;
    for (int i = 0; i < 16; i += 5)
    {
        cd.view[i] = 1.0f;
        cd.viewProj[i] = 1.0f;
    }
    rs.Views().SetCameraData(camId, cd);
    const ViewId viewId = rs.Views().AllocateView("PenumbraView", camId);

    FramePools pools(device.get());
    RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

    // ~10 degrees full diameter — deliberately far from both the 0.53 solar
    // default and the deleted global's 0.005, so a stale source cannot pass.
    const float kTanHalf = ResolveShadowTanHalfAngle(10.0f);

    ShadowMapRenderFeature feature;
    CascadeFrameData fd{};
    fd.NumCascades = kMaxShadowCascades;
    fd.ShadowTanHalfAngle = kTanHalf;
    for (uint32_t i = 0; i < kMaxShadowCascades; ++i)
    {
        // Deliberately unequal per cascade: anything leaking from these into
        // the light size shows up as a per-cascade difference below.
        fd.OrthoHalfExtent[i] = 8.0f * static_cast<float>(i + 1);
        fd.DepthSpan[i] = 40.0f * static_cast<float>(i + 1);
        fd.SplitDistances[i] = 25.0f * static_cast<float>(i + 1);
    }

    ShadowDataGPU data{};
    feature.BuildShadowDataGPU(fd, viewId, rs, frame, 0u, data);

    for (uint32_t i = 0; i < kMaxShadowCascades; ++i)
        EXPECT_FLOAT_EQ(data.shadowPcssCascades[i][3], kTanHalf)
            << "cascade " << i << " must carry the frame's tangent, undecayed";

    rs.Shutdown();
    device->Shutdown();
}

// Read back the actual batched reductions: a dispatch-count assertion alone
// cannot detect missing layers, cross-layer reads or dropped clear-depth texels.
TEST(RenderPipelineDeclareTests, ShadowMinMaxPyramidBatchesLayersWithExactDepthBounds)
{
    DeviceDesc deviceDesc{};
    deviceDesc.preferredAPI = GraphicsAPI::Vulkan;
    deviceDesc.enableDebugLayer = true;
    deviceDesc.enableDynamicRendering = true;
    auto device = DeviceFactory::CreateDevice(deviceDesc);
    if (!device || !device->Initialize(deviceDesc))
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        ShadowMinMaxPyramid feature;
        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

        constexpr uint32_t kResolution = 64;
        constexpr uint32_t kSourceLayers = 4;
        constexpr size_t kLayerTexels = kResolution * kResolution;
        // Exercise every layer, shrink below the depth array's layer count,
        // force a single-layer array view, then grow the pooled image again.
        const uint32_t activeCounts[] = {4, 2, 1, 4};
        for (uint32_t frameIndex = 0; frameIndex < std::size(activeCounts); ++frameIndex)
        {
            SCOPED_TRACE(frameIndex);
            frame.BeginFrame(frameIndex);
            // The buffer-upload API accepts color images only. R32_FLOAT supplies the same
            // depth-valued scalar reads to sampler2DArray without an invalid D32 color-aspect copy;
            // the production D32 binding remains covered by the shadow declaration tests.
            TextureDesc depthDesc{};
            depthDesc.width = kResolution;
            depthDesc.height = kResolution;
            depthDesc.arrayLayers = kSourceLayers;
            depthDesc.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
            depthDesc.flags = TextureCreateFlags::ForceArrayView;
            depthDesc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource |
                                                   TextureUsage::TransferDst);
            const auto depth = frame.CreateTexture("PyramidOracle.Depth", depthDesc);
            ASSERT_TRUE(depth.IsValid());

            std::vector<float> source(kLayerTexels * kSourceLayers);
            for (uint32_t layer = 0; layer < kSourceLayers; ++layer)
                for (uint32_t y = 0; y < kResolution; ++y)
                    for (uint32_t x = 0; x < kResolution; ++x)
                    {
                        // Exact binary fractions, distinct per cascade and frame.
                        // Clear-depth holes must lower the minimum to zero; ignoring
                        // them would make PCSS classify mixed footprints as shadowed.
                        const bool clear = (x + y + layer) % 13u == 0u;
                        source[layer * kLayerTexels + y * kResolution + x] =
                            clear ? 0.0f
                                  : float((17u * x + 29u * y + 43u * layer + 31u * frameIndex) %
                                          251u) /
                                        256.0f;
                    }
            const auto upload = frame.AllocUpload(source.size() * sizeof(float));
            ASSERT_TRUE(upload.Valid());
            std::memcpy(upload.Ptr, source.data(), source.size() * sizeof(float));
            frame.AddPass(
                "PyramidOracle.Upload", PassPhase::kEarlySetup,
                [&](RenderGraph::RGPassBuilder& p)
                { p.Write(depth, RenderGraph::RGTextureWrite::CopyDst); },
                [depth, upload](RenderGraph::RGContext& ctx)
                {
                    for (uint32_t layer = 0; layer < kSourceLayers; ++layer)
                        ctx.Cmd->CopyBufferToTextureSubresource(
                            upload.Buffer, ctx.GetTexture(depth), 0, layer, kResolution, kResolution,
                            upload.Offset + layer * kLayerTexels * sizeof(float));
                });

            const uint32_t activeLayers = activeCounts[frameIndex];
            const size_t passesBefore = frame.Graph().PassCount();
            // A kernel this wide needs every level the 16-texel base can hold.
            constexpr float kKernelTexels = 60.0f;
            const uint32_t expectedLevels = ShadowMinMaxPyramid::LevelsForQuery(
                kResolution >> ShadowMinMaxPyramid::kBaseDownshift,
                kKernelTexels + ShadowMinMaxPyramid::kQueryMarginTexels);
            const auto pyramid =
                feature.Declare(frame, rs, 1u, depth, kResolution, activeLayers, kKernelTexels, true);
            ASSERT_TRUE(pyramid.IsValid()) << "shadow_minmax_reduce.shaderpkg must be staged";
            EXPECT_EQ(frame.Graph().PassCount() - passesBefore, expectedLevels)
                << "one pass per mip, independent of the active cascade count";
            const auto state = feature.StateFor(1u, frame);
            ASSERT_EQ(state.Layers, activeLayers);
            ASSERT_EQ(state.Levels, static_cast<int>(expectedLevels));

            std::vector<std::shared_ptr<Rendering::RGReadbackTicket>> depthTickets;
            for (uint32_t layer = 0; layer < kSourceLayers; ++layer)
                depthTickets.push_back(RequestTextureSubresourceReadbackRG(
                    device.get(), frame, depth, 0, layer, 0, 0, 0, 0, "PyramidOracle.Depth"));
            std::vector<std::shared_ptr<Rendering::RGReadbackTicket>> tickets;
            for (uint32_t level = 0; level < static_cast<uint32_t>(state.Levels); ++level)
                for (uint32_t layer = 0; layer < activeLayers; ++layer)
                {
                    auto ticket = RequestTextureSubresourceReadbackRG(
                        device.get(), frame, pyramid, level, layer, 0, 0, 0, 0, "PyramidOracle");
                    ASSERT_NE(ticket, nullptr);
                    tickets.push_back(std::move(ticket));
                }
            frame.Execute();
            OnFrameSubmittedReadbacksRG(frame, frame.SubmissionToken());
            device->WaitForIdle();

            for (uint32_t pass = 0; pass < frame.Graph().PassCount(); ++pass)
                ASSERT_FALSE(frame.Graph().IsCulled(pass));
            for (uint32_t layer = 0; layer < kSourceLayers; ++layer)
            {
                ViewReadbackResult result{};
                ASSERT_TRUE(depthTickets[layer]);
                ASSERT_TRUE(depthTickets[layer]->TryGet(result));
                ASSERT_EQ(result.pixels.size(), kLayerTexels * sizeof(float));
                ASSERT_EQ(std::memcmp(result.pixels.data(), source.data() + layer * kLayerTexels,
                                      result.pixels.size()), 0)
                    << "depth input upload differs at layer " << layer;
            }
            size_t ticketIndex = 0;
            for (uint32_t level = 0; level < static_cast<uint32_t>(state.Levels); ++level)
                for (uint32_t layer = 0; layer < activeLayers; ++layer)
                {
                    SCOPED_TRACE(level);
                    SCOPED_TRACE(layer);
                    ViewReadbackResult result{};
                    ASSERT_TRUE(tickets[ticketIndex++]->TryGet(result));
                    const uint32_t footprint = 4u << level;
                    const uint32_t extent = kResolution / footprint;
                    ASSERT_EQ(result.width, extent);
                    ASSERT_EQ(result.height, extent);
                    ASSERT_EQ(result.format, TextureFormat::R32G32_FLOAT);
                    ASSERT_EQ(result.pixels.size(), extent * extent * 2u * sizeof(float));
                    std::vector<float> bounds(extent * extent * 2u);
                    std::memcpy(bounds.data(), result.pixels.data(), result.pixels.size());
                    for (uint32_t y = 0; y < extent; ++y)
                        for (uint32_t x = 0; x < extent; ++x)
                        {
                            float lo = 1.0f, hi = 0.0f;
                            for (uint32_t sy = y * footprint; sy < (y + 1u) * footprint; ++sy)
                                for (uint32_t sx = x * footprint; sx < (x + 1u) * footprint; ++sx)
                                {
                                    const float d = source[layer * kLayerTexels + sy * kResolution + sx];
                                    lo = std::min(lo, d);
                                    hi = std::max(hi, d);
                                }
                            const size_t index = (y * extent + x) * 2u;
                            ASSERT_EQ(bounds[index], lo) << "minimum at " << x << ',' << y;
                            ASSERT_EQ(bounds[index + 1u], hi) << "maximum at " << x << ',' << y;
                        }
                }
        }
        const auto validation = device->GetValidationStats();
        EXPECT_EQ(validation.ErrorCount, 0u);
        EXPECT_EQ(validation.OverflowCount, 0u);
        for (const auto& entry : validation.Vuids)
            EXPECT_FALSE(entry.IsError) << entry.Vuid << ": " << entry.FirstMessage;
        rs.Shutdown();
    }
    device->Shutdown();
}

// The gate is structural: ineffective PCSS or the runtime toggle declares no
// pyramid passes. Enabled frames declare four levels with all active cascades
// batched into Z; publishing independently pins the light's active layer count.
TEST(RenderPipelineDeclareTests, ShadowMinMaxPyramidDeclaresAndPublishesOnlyForEffectivePcss)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        const CameraId camId = rs.Views().AllocateCamera("PyramidCam");
        CameraData cd{};
        cd.proj[0] = 1.0f;
        cd.proj[5] = 1.0f;
        cd.proj[10] = 0.001f;
        cd.proj[11] = 1.0f;
        cd.proj[14] = 0.1f;
        for (int i = 0; i < 16; i += 5)
        {
            cd.view[i] = 1.0f;
            cd.viewProj[i] = 1.0f;
        }
        rs.Views().SetCameraData(camId, cd);
        const ViewId viewId = rs.Views().AllocateView("PyramidView", camId);
        rs.Views().SetViewRenderLayerMask(viewId, 1u);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);

        auto depHandle = rs.RegisterDepthEmit(
            [&](DepthEmitContext& ctx, DepthPassType passType)
            {
                if (ctx.ViewId == viewId && passType == DepthPassType::ShadowCascade)
                    ctx.Services->EmitDepthCommand(ctx.ViewId, passType, DrawCommand{});
            });

        RenderPipelineNodeRegistry registry;
        ASSERT_TRUE(registry.Register(
            "ShadowMap", [] { return std::make_unique<Nodes::ShadowMapNode>(); }, true));
        ASSERT_TRUE(registry.Register(
            "WorldRender", [] { return std::make_unique<Nodes::WorldRenderNode>(); }, true));

        RenderPipelineBlueprint bp;
        bp.pipelineName = "PyramidTest";
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "CSM";
            p.type = "ShadowMap";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"CSM","type":"ShadowMap","buffer":"ShadowData"})";
            bp.passes.push_back(p);
        }
        {
            RenderPipelineBlueprint::Pass p;
            p.id = "World";
            p.type = "WorldRender";
            p.enabled = true;
            p.perView = true;
            p.passJson = R"({"id":"World","type":"WorldRender","keywords":["Shadows"]})";
            bp.passes.push_back(p);
        }

        RenderPipelineInstance instance(rs, registry);
        instance.SetBlueprint(bp);

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);

        ExtractedLight sun{};
        sun.type = GameEngine::Components::LightType::Directional;
        sun.castsShadows = 1;
        sun.castsLight = 1;
        sun.directionWS[1] = -1.0f;
        sun.cascadeCount = 4;

        const std::vector<Rendering::ViewDesc> views(rs.Views().GetViews().begin(),
                                                     rs.Views().GetViews().end());

        auto countPyramidPasses = [&]() -> uint32_t
        {
            uint32_t n = 0;
            for (size_t p = 0; p < frame.Graph().PassCount(); ++p)
                if (std::string(frame.Graph().PassName(static_cast<RenderGraph::RGPassId>(p)))
                        .find("ShadowMinMaxPyramid") != std::string::npos)
                    ++n;
            return n;
        };

        ShadowMapRenderFeature* feature = nullptr;

        // The publish half of the same gate. BuildShadowDataGPU is the sole
        // writer of shadowPcssPyramid and runs against the frame that just
        // declared, so a cascade with no pyramid must be left at the NEGATIVE
        // sentinel — index 0 is a legal-looking bindless slot, which is why the
        // "absent" value cannot be the zero the UBO starts at.
        auto publishedShadowData = [&]() -> ShadowDataGPU
        {
            ShadowDataGPU data{};
            if (const CascadeFrameData* fd = feature->GetCachedFrameData(viewId))
                feature->BuildShadowDataGPU(*fd, viewId, rs, frame, 0u, data);
            return data;
        };
        auto expectPyramidPublished = [&](const ShadowDataGPU& data, uint32_t cascade)
        {
            EXPECT_GT(data.shadowPcssPyramid[cascade][0], 0.0f)
                << "cascade " << cascade << " must carry a live bindless index";
            EXPECT_EQ(data.shadowPcssPyramid[cascade][1], ExpectedPyramidLevels(*feature, viewId))
                << "cascade " << cascade << " level count (the widest kernel's)";
            EXPECT_EQ(data.shadowPcssPyramid[cascade][2],
                      static_cast<float>(ShadowMinMaxPyramid::kBaseDownshift))
                << "cascade " << cascade << " base downshift";
        };
        auto expectPyramidAbsent = [](const ShadowDataGPU& data, uint32_t cascade)
        {
            EXPECT_LT(data.shadowPcssPyramid[cascade][0], 0.0f)
                << "cascade " << cascade << " must publish the negative no-pyramid sentinel";
        };

        // Frame 0 — PCSS is the default filter quality, so this is the ON arm.
        frame.BeginFrame(0);
        RenderGraph::RGTexture color = frame.ImportPersistentTexture("Pyr.Color", ColorTargetDesc());
        RenderGraph::RGTexture depth = frame.ImportPersistentTexture("Pyr.Depth", DepthTargetDesc());
        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();
        rs.SubmitLight(0u, sun);
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{viewId, color, depth, {}}};
        instance.Declare(frame, targets, views);
        if (!frame.Graph().PassCount())
            GTEST_SKIP() << "shadow shaders unavailable in this environment";

        // PCSS reads raw depth through bindless, so it demotes to Poisson
        // without it — and the gate follows the EFFECTIVE quality. On such a
        // device zero is the correct answer, so the expectation flips with the
        // capability rather than the arm being skipped: the false case is
        // coverage of the demotion itself.
        const bool pcssEffective = rs.Textures().IsBindlessEnabled();
        feature = rs.GetFeature<ShadowMapRenderFeature>();
        ASSERT_NE(feature, nullptr);
        EXPECT_EQ(static_cast<float>(countPyramidPasses()),
                  pcssEffective ? ExpectedPyramidLevels(*feature, viewId) : 0.0f)
            << "effective PCSS: one pass per level the widest kernel needs, cascades batched; "
               "no bindless: PCSS demotes to Poisson and nothing declares";
        {
            ASSERT_NE(feature->GetCachedFrameData(viewId), nullptr);
            const ShadowDataGPU published = publishedShadowData();
            for (uint32_t c = 0; c < kMaxShadowCascades; ++c)
            {
                if (pcssEffective)
                    expectPyramidPublished(published, c);
                else
                    expectPyramidAbsent(published, c);
            }
            EXPECT_EQ(feature->HasPyramidBindlessForTesting(viewId), pcssEffective);
        }
        frame.MarkOutput(color);
        frame.Execute();
        device->WaitForIdle();

        feature->SetFilterQuality(ShadowFilterQuality::PoissonPCF);

        // Frame 1 — the OFF arm. Unconditional: no device prerequisite to
        // satisfy, and this is the direction the A/B's validity rests on.
        frame.BeginFrame(1);
        color = frame.ImportPersistentTexture("Pyr.Color", ColorTargetDesc());
        depth = frame.ImportPersistentTexture("Pyr.Depth", DepthTargetDesc());
        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();
        rs.SubmitLight(0u, sun);
        const std::vector<ViewTargetsRG> targets1 = {ViewTargetsRG{viewId, color, depth, {}}};
        instance.Declare(frame, targets1, views);
        ASSERT_GT(frame.Graph().PassCount(), 0u)
            << "the pipeline must still declare, or zero pyramid passes proves nothing";
        EXPECT_EQ(countPyramidPasses(), 0u)
            << "a non-PCSS frame declares no pyramid passes at all — not passes that early-return";
        {
            const ShadowDataGPU published = publishedShadowData();
            for (uint32_t c = 0; c < kMaxShadowCascades; ++c)
                expectPyramidAbsent(published, c);
            // The slots must be RELEASED, not merely unpublished. The pool
            // destroys a persistent texture 300 idle frames after its last
            // import and notifies nobody, so a registration that survives the
            // switch away from PCSS ends up naming a view of a freed image.
            EXPECT_FALSE(feature->HasPyramidBindlessForTesting(viewId))
                << "a non-PCSS frame must release the pyramid's bindless slots";
        }
        frame.MarkOutput(color);
        frame.Execute();
        device->WaitForIdle();

        // Frame 2 — PCSS again, but the light asks for fewer cascades than the
        // pipeline is configured for. The pyramid must follow the RENDERED
        // count: the upper array layers are never rasterized, so reducing them
        // would spend a full level chain per layer on undefined depth, and every
        // bit of that lands on the arm the A/B is measuring.
        feature->SetFilterQuality(ShadowFilterQuality::PCSS);
        sun.cascadeCount = 2;
        frame.BeginFrame(2);
        color = frame.ImportPersistentTexture("Pyr.Color", ColorTargetDesc());
        depth = frame.ImportPersistentTexture("Pyr.Depth", DepthTargetDesc());
        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();
        rs.SubmitLight(0u, sun);
        const std::vector<ViewTargetsRG> targets2 = {ViewTargetsRG{viewId, color, depth, {}}};
        instance.Declare(frame, targets2, views);
        {
            const auto* fd = feature->GetCachedFrameData(viewId);
            ASSERT_NE(fd, nullptr);
            EXPECT_EQ(fd->NumCascades, 2u) << "min(config 4, light cascadeCount 2)";
        }
        EXPECT_EQ(static_cast<float>(countPyramidPasses()), pcssEffective ? ExpectedPyramidLevels(*feature, viewId) : 0.0f)
            << "the widest kernel's levels with only the light's 2 rendered cascades in each dispatch";
        // The publish follows the same bound. Cascades 2-3 have no pyramid LAYER
        // this frame, so they keep the sentinel even though PCSS is live — and
        // the shrink reallocated the pooled pyramid, which is the release path
        // (old physical -> InvalidateBindless -> re-register) running for real.
        {
            const ShadowDataGPU published = publishedShadowData();
            for (uint32_t c = 0; c < kMaxShadowCascades; ++c)
            {
                if (pcssEffective && c < 2u)
                    expectPyramidPublished(published, c);
                else
                    expectPyramidAbsent(published, c);
            }
            // Re-registered after frame 1 released them.
            EXPECT_EQ(feature->HasPyramidBindlessForTesting(viewId), pcssEffective);
        }
        frame.MarkOutput(color);
        frame.Execute();
        device->WaitForIdle();

        depHandle.Reset();
        rs.Shutdown();
    }
    device->Shutdown();
}

TEST(RenderPipelineDeclareTests, FogEmissionGridIsAllocatedOnlyForAVolumeThatChangesEmission)
{
    // A froxel's emission equals the view's unless a local volume changes it, so
    // the full rgba16f grid is only allocated when one does; otherwise the grid
    // is one texel and the lighting pass reads the emission uniform. At a real
    // viewport the difference is tens of megabytes of transient every frame.
    FogFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    struct Shutdown
    {
        FogFixture& Fixture;
        ~Shutdown() { Fixture.Down(); }
    } shutdown{f};

    const auto emissionWidth = [&](const std::vector<VolumetricFogLocalVolume>& volumes)
    {
        f.Rs->SetWorldVolumetricFogVolumes(0u, volumes);
        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(f.MakeBlueprint());
        FramePools pools(f.Device.get());
        f.SetFog(true, false);
        RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        const auto color = frame.ImportPersistentTexture("FogEmission.Color", ColorTargetDesc());
        const auto depth = frame.ImportPersistentTexture("FogEmission.Depth", DepthTargetDesc());
        const std::vector<ViewTargetsRG> targets{ViewTargetsRG{f.View, color, depth, {}}};
        const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(),
                                                    f.Rs->Views().GetViews().end());
        instance.Declare(frame, targets, views);
        const auto& graph = frame.Graph();
        uint32_t width = 0;
        for (const auto& access : graph.Accesses())
        {
            const char* name = graph.ResourceName(access.Resource);
            if (name && std::string(name).ends_with(".Emission"))
                width = graph.ResourceDesc(access.Resource).Width;
        }
        frame.MarkOutput(color);
        frame.Execute();
        f.Device->WaitForIdle();
        return width;
    };

    VolumetricFogLocalVolume dark{};
    dark.enabled = true;
    dark.densityMode = VolumetricFogDensityMode::Additive;
    const uint32_t gated = emissionWidth({dark});
    EXPECT_EQ(gated, 1u) << "a non-emitting additive volume must not allocate the grid";

    VolumetricFogLocalVolume emissive = dark;
    emissive.emission[0] = 2.0f;
    const uint32_t allocated = emissionWidth({emissive});
    EXPECT_GT(allocated, 1u) << "an emitting volume must allocate the full grid";

    // An override volume rewrites emission toward its own value even when that
    // value is zero, so it needs the grid as much as an emitter does.
    VolumetricFogLocalVolume override = dark;
    override.densityMode = VolumetricFogDensityMode::Override;
    const uint32_t overridden = emissionWidth({override});
    EXPECT_GT(overridden, 1u) << "an override volume must allocate the grid";

    VolumetricFogLocalVolume disabled = emissive;
    disabled.enabled = false;
    const uint32_t ignored = emissionWidth({disabled});
    EXPECT_EQ(ignored, 1u) << "a disabled volume must not allocate the grid";
    f.Rs->SetWorldVolumetricFogVolumes(0u, {});
}

TEST(RenderPipelineDeclareTests, FogLightingReadsLocalShadowsAndEmissionComputeScoped)
{
    FogFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    struct Shutdown
    {
        FogFixture& Fixture;
        ~Shutdown() { Fixture.Down(); }
    } shutdown{f};
    ASSERT_TRUE(f.Registry.Register(
        "ShadowMap", [] { return std::make_unique<Nodes::ShadowMapNode>(); }, true));
    ASSERT_TRUE(f.Registry.Register(
        "LightUpload", [] { return std::make_unique<Nodes::LightUploadNode>(); }, true));
    auto bp = f.MakeBlueprint();
    RenderPipelineBlueprint::Pass upload;
    upload.id = "FogLights";
    upload.type = "LightUpload";
    upload.enabled = upload.perView = true;
    upload.passJson = R"({"id":"FogLights","type":"LightUpload","buffer":"LightBuffer"})";
    bp.passes.insert(bp.passes.begin(), upload);
    RenderPipelineBlueprint::Pass shadows;
    shadows.id = "LocalShadows";
    shadows.type = "ShadowMap";
    shadows.enabled = shadows.perView = true;
    shadows.passJson = R"({"id":"LocalShadows","type":"ShadowMap","buffer":"ShadowData","punctualResolution":64})";
    bp.passes.insert(bp.passes.begin(), shadows);
    RenderPipelineInstance instance(*f.Rs, f.Registry);
    instance.SetBlueprint(bp);
    FramePools pools(f.Device.get());
    f.SetFog(true, true);
    auto emit = f.Rs->RegisterDepthEmit([&](DepthEmitContext& ctx, DepthPassType type)
    {
        if (ctx.ViewId == f.View && (type == DepthPassType::SpotShadow || type == DepthPassType::PointShadow))
            ctx.Services->EmitDepthCommand(ctx.ViewId, type, DrawCommand{});
    });
    RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);
    EXPECT_FALSE(f.Rs->GetLocalShadowInputsRG(frame, f.View).SpotMap.IsValid());
    EXPECT_FALSE(f.Rs->GetLocalShadowInputsRG(frame, f.View).PointMap.IsValid());
    const auto color = frame.ImportPersistentTexture("FogLocal.Color", ColorTargetDesc());
    const auto depth = frame.ImportPersistentTexture("FogLocal.Depth", DepthTargetDesc());
    f.Rs->BeginWorldDrawFrame();
    f.Rs->BuildWorldBatchKeys();
    ExtractedLight light{};
    light.type = GameEngine::Components::LightType::Spot;
    light.castsShadows = 1;
    light.positionWS[2] = -2.0f;
    light.directionWS[1] = 0.0f;
    light.directionWS[2] = 1.0f;
    light.SortId = 1;
    f.Rs->SubmitLight(0u, light);
    light.type = GameEngine::Components::LightType::Point;
    light.SortId = 2;
    f.Rs->SubmitLight(0u, light);
    const std::vector<ViewTargetsRG> targets{ViewTargetsRG{f.View, color, depth, {}}};
    const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(), f.Rs->Views().GetViews().end());
    instance.Declare(frame, targets, views);
    const auto inputs = f.Rs->GetLocalShadowInputsRG(frame, f.View);
    ASSERT_TRUE(inputs.SpotMap.IsValid());
    ASSERT_TRUE(inputs.PointMap.IsValid());
    EXPECT_TRUE(inputs.SpotData.IsValid());
    EXPECT_TRUE(inputs.PointData.IsValid());
    EXPECT_GE(inputs.PointBytes, sizeof(PointShadowSlotGPU));
    const auto& graph = frame.Graph();
    uint32_t spotReads = 0, pointReads = 0, emissionReads = 0, emissionWrites = 0;
    uint32_t lightListReads = 0, lightListWrites = 0, clusterZeroFills = 0;
    for (const auto& access : graph.Accesses())
    {
        const char* passName = graph.PassName(access.Pass);
        const char* resourceName = graph.ResourceName(access.Resource);
        if (!passName || !resourceName || !RGQuery::Matches(passName, RGQuery::Subtree{"VolumetricFog"}))
            continue;
        const bool lightList = std::string(resourceName).ends_with(".LightIndices") ||
                               std::string(resourceName).ends_with(".LightClusters");
        if (RGQuery::Matches(passName, RGQuery::Family{"LightCull"}) && lightList)
            ++lightListWrites;
        // Recycled transient memory can spell a plausible cluster header. The
        // declared zero-fill is what makes a skipped cull dispatch fall back to
        // the full-light scan instead of reading a previous frame's lists.
        if (RGQuery::Matches(passName, RGQuery::Family{"ClearLightClusters"}) && lightList)
        {
            ++clusterZeroFills;
            EXPECT_EQ(access.Access, RenderGraph::RGAccess::CopyDst) << resourceName;
            EXPECT_TRUE(std::string(resourceName).ends_with(".LightClusters")) << resourceName;
        }
        const bool emission = std::string(resourceName).ends_with(".Emission");
        if (RGQuery::Matches(passName, RGQuery::Family{"Media"}) && emission)
            ++emissionWrites;
        if (!RGQuery::Matches(passName, RGQuery::Family{"Lighting"}))
            continue;
        if (access.Resource == inputs.SpotMap.Id) ++spotReads;
        else if (access.Resource == inputs.PointMap.Id) ++pointReads;
        else if (emission) ++emissionReads;
        else if (lightList) ++lightListReads;
        else continue;
        EXPECT_TRUE(RenderGraph::MapAccess(access.Access, graph.PassQueue(access.Pass)).Stage &
                    RenderGraph::RGStage::ComputeShader) << resourceName;
    }
    EXPECT_EQ(spotReads, 1u);
    EXPECT_EQ(pointReads, 1u);
    EXPECT_EQ(emissionReads, 1u);
    EXPECT_EQ(emissionWrites, 1u);
    EXPECT_EQ(lightListReads, 2u);
    EXPECT_EQ(lightListWrites, 2u);
    EXPECT_EQ(clusterZeroFills, 1u);
    frame.MarkOutput(color);
    frame.Execute();
    f.Device->WaitForIdle();
}

// The diffuse-only resolve must keep native allocation unchanged while a
// backend that prohibits writable aliases gets distinct, bounded placeholders.
// Exercise the real declaration so every placeholder must also have its graph
// access declared; missing edges previously made the proposed Web fix fail on Vulkan.
TEST(RenderPipelineDeclareTests, DdgiInactiveLobesRespectStorageAliasingCapability)
{
    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::Vulkan;
    desc.enableDebugLayer = true;
    desc.enableDynamicRendering = true;
    auto device = DeviceFactory::CreateDevice(desc);
    if (device && !device->Initialize(desc))
        device.reset();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    if (!device->GetValidationStats().Enabled)
    {
        device->Shutdown();
        GTEST_SKIP() << "Vulkan validation layer unavailable";
    }
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        {
            DDGIProbeFeature feature;
            ASSERT_TRUE(feature.Initialize(device.get(), nullptr, rs.GetGPUScene(),
                                           &rs.GetMeshGPURegistry(), &rs.Materials()));
            DDGIVolumeDesc volume{};
            volume.Enabled = true;
            volume.EnableGlossy = false;
            volume.GlossyResolveScale = Components::DDGIGlossyResolveScale::Half;
            feature.SetActiveVolume(volume);
            ASSERT_FALSE(feature.GlossyLobesActive());

            for (bool aliasesAllowed : {true, false})
            {
                SCOPED_TRACE(aliasesAllowed ? "native aliases" : "distinct bindings required");
                // Model only this backend constraint, retaining Vulkan's real
                // device and kernels for the resource/declaration checks.
                auto& caps = const_cast<RenderingDeviceCapabilities&>(device->GetCapabilities());
                struct RestoreCapability
                {
                    bool& Value;
                    bool Original;
                    ~RestoreCapability() { Value = Original; }
                } restore{caps.supportsAliasedStorageTextureBindings,
                          caps.supportsAliasedStorageTextureBindings};
                caps.supportsAliasedStorageTextureBindings = aliasesAllowed;

                FramePools pools(device.get());
                RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient,
                                            &pools.Ring);
                frame.BeginFrame(0);
                const auto depth = frame.ImportPersistentTexture("DDGI.Test.Depth", DepthTargetDesc());
                auto params = frame.AllocUpload<ViewParamsUBO>();
                ASSERT_TRUE(params.Valid());
                *params.Ptr = {};
                RenderGraph::RGTexture rough, glossy, irradiance, normal;
                ASSERT_TRUE(feature.DeclareGlossyResolveForView(
                    frame, rs, params.Buffer, params.Offset, sizeof(ViewParamsUBO), depth,
                    64, 64, 42, 1.0f, "DDGI.Test.Resolve", rough, glossy, irradiance, normal));

                const auto& graph = frame.Graph();
                EXPECT_EQ(graph.ResourceDesc(irradiance.Id).Width, 32u);
                EXPECT_EQ(graph.ResourceDesc(irradiance.Id).Height, 32u);
                EXPECT_EQ(rough.Id == irradiance.Id, aliasesAllowed);
                EXPECT_EQ(glossy.Id == irradiance.Id, aliasesAllowed);
                if (!aliasesAllowed)
                    EXPECT_NE(rough.Id, glossy.Id);

                const auto resolve = RGQuery::FindDeclared(graph, RGQuery::Exact{"DDGI.Test.Resolve"});
                const auto blur = RGQuery::FindDeclared(graph, RGQuery::Exact{"DDGI.Test.ResolveBlur"});
                ASSERT_NE(resolve, RenderGraph::kInvalidId);
                ASSERT_NE(blur, RenderGraph::kInvalidId);
                for (const char* lobe : {"Rough", "Glossy", "RoughBlur", "GlossyBlur"})
                {
                    const std::string name = std::string("DDGIGlossyResolve.") + lobe + ".42";
                    RenderGraph::RGResourceId found = RenderGraph::kInvalidId;
                    for (uint32_t r = 0; r < graph.ResourceCount(); ++r)
                        if (graph.ResourceName(r) && name == graph.ResourceName(r))
                            found = r;
                    if (aliasesAllowed)
                    {
                        EXPECT_EQ(found, RenderGraph::kInvalidId) << name;
                        continue;
                    }
                    ASSERT_NE(found, RenderGraph::kInvalidId) << name;
                    EXPECT_EQ(graph.ResourceDesc(found).Width, 1u) << name;
                    EXPECT_EQ(graph.ResourceDesc(found).Height, 1u) << name;
                    const bool blurred = std::string_view(lobe).ends_with("Blur");
                    EXPECT_TRUE(graph.HasWriteAccess(blurred ? blur : resolve, found)) << name;
                    if (!blurred)
                        EXPECT_TRUE(graph.HasReadAccess(blur, found)) << name;
                }
                // Execute both capability arms: declaration alone cannot catch
                // invalid descriptor byte ranges in the resolve callback.
                frame.MarkOutput(irradiance);
                device->ResetValidationStats();
                frame.Execute();
                device->WaitForIdle();
                EXPECT_EQ(device->GetValidationStats().ErrorCount, 0u);
            }
        }
        rs.Shutdown();
    }
    device->Shutdown();
}

// The resolve blur blends each frame into a ping-pong irradiance history. A
// buffer the pool has just allocated holds undefined memory, and when that
// memory held NaN the blend kept it forever: NaN blocks in the view that
// persist (#2768). Under GE_VK_FILL_NEW_TARGETS_NAN every new target starts as
// NaN, so a first frame that reads its fresh history shows NaN deterministically.
TEST(RenderPipelineDeclareTests, DdgiResolveFirstFrameNeverBlendsUndefinedHistory)
{
    struct FillEnvironment
    {
        std::string Previous;
        FillEnvironment()
        {
            if (const char* prior = std::getenv("GE_VK_FILL_NEW_TARGETS_NAN"))
                Previous = prior;
            Set("1");
        }
        ~FillEnvironment() { Set(Previous.c_str()); }
        static void Set(const char* value)
        {
#if defined(_WIN32)
            _putenv_s("GE_VK_FILL_NEW_TARGETS_NAN", value);
#else
            if (*value)
                setenv("GE_VK_FILL_NEW_TARGETS_NAN", value, 1);
            else
                unsetenv("GE_VK_FILL_NEW_TARGETS_NAN");
#endif
        }
    } fill;
    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::Vulkan;
    desc.enableDynamicRendering = true;
    auto device = DeviceFactory::CreateDevice(desc);
    if (device && !device->Initialize(desc))
        device.reset();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        {
            DDGIProbeFeature feature;
            ASSERT_TRUE(feature.Initialize(device.get(), nullptr, rs.GetGPUScene(), &rs.GetMeshGPURegistry(),
                                           &rs.Materials()));
            DDGIVolumeDesc volume{};
            volume.Enabled = true;
            volume.EnableGlossy = false;
            feature.SetActiveVolume(volume);

            FramePools pools(device.get());
            RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
            frame.BeginFrame(0);
            // A surface at every pixel (depth > 0) and an identity camera, so the
            // reprojection lands on screen and the blur takes its temporal path.
            const auto depth = frame.CreateTexture("DDGI.Test.Depth", DepthTargetDesc());
            frame.AddPass(
                "DDGI.Test.DepthClear", 0,
                [&](RenderGraph::RGPassBuilder& p)
                {
                    RenderGraph::RGAttachmentOps ops{};
                    ops.Load = RenderGraph::RGLoadOp::Clear;
                    ops.Clear.Depth = 0.5f;
                    p.AttachDepth(depth, ops);
                },
                [](RenderGraph::RGContext&) {});
            auto params = frame.AllocUpload<ViewParamsUBO>();
            ASSERT_TRUE(params.Valid());
            *params.Ptr = {};
            for (float* m : {params.Ptr->ge_invProj, params.Ptr->ge_invView, params.Ptr->ge_prevViewProj})
                for (int i = 0; i < 4; ++i)
                    m[i * 5] = 1.0f;
            RenderGraph::RGTexture rough, glossy, irradiance, normal;
            ASSERT_TRUE(feature.DeclareGlossyResolveForView(frame, rs, params.Buffer, params.Offset,
                                                            sizeof(ViewParamsUBO), depth, 64, 64, 42, 1.0f,
                                                            "DDGI.Test.Resolve", rough, glossy, irradiance,
                                                            normal));
            const auto& irradianceDesc = frame.Graph().ResourceDesc(irradiance.Id);
            const uint32_t w = irradianceDesc.Width;
            const uint32_t h = irradianceDesc.Height;
            const BufferHandle readback =
                device->CreateReadbackBuffer(static_cast<uint64_t>(w) * h * 8u, "DDGI.Test.IrradianceRead");
            ASSERT_TRUE(readback.IsValid());
            const auto readbackRG = frame.ImportExternalBuffer("DDGI.Test.IrradianceRead", readback);
            frame.AddPass(
                "DDGI.Test.ReadIrradiance", 0,
                [&](RenderGraph::RGPassBuilder& p)
                {
                    p.Read(irradiance, RenderGraph::RGTextureRead::CopySrc);
                    p.Write(readbackRG, RenderGraph::RGBufferWrite::CopyDst);
                    p.PreventCulling();
                },
                [irradiance, readback, w, h](RenderGraph::RGContext& ctx)
                {
                    if (ctx.Cmd)
                        ctx.Cmd->CopyTextureSubresourceToBuffer(ctx.GetTexture(irradiance), 0, 0, readback, w, h);
                });
            frame.Execute();
            device->WaitForIdle();

            const auto* texels = static_cast<const uint16_t*>(device->MapBuffer(readback));
            ASSERT_NE(texels, nullptr);
            uint32_t nonFinite = 0;
            for (uint32_t i = 0; i < w * h; ++i)
                for (uint32_t c = 0; c < 3; ++c)
                    nonFinite += (texels[i * 4 + c] & 0x7C00u) == 0x7C00u ? 1u : 0u;
            device->UnmapBuffer(readback);
            EXPECT_EQ(nonFinite, 0u) << "the first frame blended an unwritten history into the irradiance";
            device->DestroyBuffer(readback);
        }
        rs.Shutdown();
    }
    device->Shutdown();
}

namespace
{
// Arms GE_VK_FORCE_DEVICE_LOST for the device created in its scope: the
// injected loss fires on the second device frame and the device rebuilds in place.
struct ScopedForcedDeviceLoss
{
    ScopedForcedDeviceLoss() { Set("2"); }
    ~ScopedForcedDeviceLoss() { Set(""); }
    static void Set(const char* value)
    {
#if defined(_WIN32)
        _putenv_s("GE_VK_FORCE_DEVICE_LOST", value);
#else
        if (*value)
            setenv("GE_VK_FORCE_DEVICE_LOST", value, 1);
        else
            unsetenv("GE_VK_FORCE_DEVICE_LOST");
#endif
    }
};

// Runs empty device frames until the armed loss has rebuilt the device; true once it has.
bool RunUntilDeviceRebuilt(IDevice& device)
{
    for (int i = 0; i < 16 && device.GetDeviceRebuildGeneration() == 0u; ++i)
    {
        device.TickDeviceRecovery();
        if (!device.BeginFrame())
            continue;
        auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        device.ExecuteCommandLists(lists);
        device.Present();
    }
    return device.GetDeviceRebuildGeneration() != 0u;
}

// Counts the descriptor writes the device dropped for a handle that outlived its
// device. The sentinel proves the capture was live, so an empty capture cannot pass.
constexpr const char* kRebuildCaptureSentinel = "RenderPipelineDeclareTests: rebuild capture live";
void ExpectNoDroppedDescriptorWrites(const std::vector<std::string>& errors)
{
    EXPECT_EQ(std::count_if(errors.begin(), errors.end(),
                            [](const std::string& line) { return line.find(kRebuildCaptureSentinel) != std::string::npos; }),
              1) << "the error capture was not live";
    for (const std::string& line : errors)
        EXPECT_EQ(line.find("did not resolve"), std::string::npos) << line;
}

// Declares the glossy resolve over a cleared depth target and executes it.
void ExecuteGlossyResolveOnce(IDevice& device, RenderServices& rs, DDGIProbeFeature& feature, uint64_t frameIndex)
{
    FramePools pools(&device);
    RenderGraph::RGFrame frame(&device, &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(frameIndex);
    const auto depth = frame.CreateTexture("DDGI.Rebuild.Depth", DepthTargetDesc());
    frame.AddPass(
        "DDGI.Rebuild.DepthClear", 0,
        [&](RenderGraph::RGPassBuilder& p)
        {
            RenderGraph::RGAttachmentOps ops{};
            ops.Load = RenderGraph::RGLoadOp::Clear;
            ops.Clear.Depth = 0.5f;
            p.AttachDepth(depth, ops);
        },
        [](RenderGraph::RGContext&) {});
    auto params = frame.AllocUpload<ViewParamsUBO>();
    ASSERT_TRUE(params.Valid());
    *params.Ptr = {};
    RenderGraph::RGTexture rough, glossy, irradiance, normal;
    ASSERT_TRUE(feature.DeclareGlossyResolveForView(frame, rs, params.Buffer, params.Offset, sizeof(ViewParamsUBO),
                                                    depth, 64, 64, 42, 1.0f, "DDGI.Rebuild.Resolve", rough, glossy,
                                                    irradiance, normal));
    frame.MarkOutput(irradiance);
    frame.Execute();
    device.WaitForIdle();
}
} // namespace

// The resolve binds handles the feature caches across frames: its point-clamp
// depth sampler and the probe-state fallback buffer. An in-place device rebuild
// frees both, so a cached handle that survives it names nothing: its descriptor
// writes are dropped and the kernel reads whatever descriptor the recycled
// memory held, which faulted the GPU on every recovered device.
TEST(RenderPipelineDeclareTests, DdgiResolveBindsLiveHandlesAfterADeviceRebuild)
{
    const ScopedForcedDeviceLoss loss;
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    if (!device->IsDescriptorBufferEnabled())
    {
        device->Shutdown();
        GTEST_SKIP() << "dropped descriptor writes are reported only on the descriptor-buffer path";
    }
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        {
            DDGIProbeFeature feature;
            ASSERT_TRUE(feature.Initialize(device.get(), nullptr, rs.GetGPUScene(), &rs.GetMeshGPURegistry(),
                                           &rs.Materials()));
            DDGIVolumeDesc volume{};
            volume.Enabled = true;
            volume.EnableGlossy = false;
            feature.SetActiveVolume(volume);
            ExecuteGlossyResolveOnce(*device, rs, feature, 0);

            ASSERT_TRUE(RunUntilDeviceRebuilt(*device)) << "injected loss should rebuild the device in place";
            // A standalone feature is outside RenderServices' feature sweep.
            feature.OnDeviceRebuilt(device.get());

            std::vector<std::string> errors;
            {
                GameEngine::TestLog::ScopedEngineLogCapture capture(&errors, Logger::LogLevel::Error);
                Logger::Log::Error(kRebuildCaptureSentinel);
                ExecuteGlossyResolveOnce(*device, rs, feature, 1);
                Logger::Log::Flush();
            }
            ExpectNoDroppedDescriptorWrites(errors);
        }
        rs.Shutdown();
    }
    device->Shutdown();
}

TEST(RenderPipelineDeclareTests, SceneAsEmptyGeometryPublishesBindableZeroRows)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        {
            SceneAccelerationStructureService scene(device.get(), &rs.GetMeshGPURegistry(),
                                                     rs.GetGPUScene());
            const auto geometry = scene.PublishMeshGeometry();
            ASSERT_TRUE(geometry.IsValid())
                << "an empty TLAS still requires the trace shader's geometry binding";
            const auto bytes = scene.GetMeshGeometryCapacityBytes();
            ASSERT_GT(bytes, 0u);
            const auto* rows = static_cast<const unsigned char*>(device->MapBuffer(geometry));
            ASSERT_NE(rows, nullptr);
            for (uint64_t i = 0; i < bytes; ++i)
                ASSERT_EQ(rows[i], 0u) << "unused row byte " << i;
            EXPECT_EQ(scene.PublishMeshGeometry(), geometry);
            EXPECT_EQ(scene.GetMeshGeometryCapacityBytes(), bytes);
        }
        rs.Shutdown();
    }
    device->Shutdown();
}

namespace
{
// Vulkan where it reaches ray query, else Metal (Apple Silicon); null when no
// backend on this machine has an acceleration-structure backend.
std::unique_ptr<IDevice> CreateRayQueryDevice()
{
    if (auto vulkan = CreateVulkanDeviceFast(); vulkan && vulkan->GetCapabilities().supportsRayQuery)
        return vulkan;
    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::Metal;
    auto metal = DeviceFactory::CreateDevice(desc);
    if (metal && metal->Initialize(desc) && metal->GetAPI() == GraphicsAPI::Metal &&
        metal->GetCapabilities().supportsRayQuery)
        return metal;
    return nullptr;
}

// One static triangle in the mesh registry: the pool's sweep creates exactly
// one BLAS candidate for it. A retained CPU mesh survives a device rebuild
// (re-uploaded in place); without it the rebuild tombstones the entry.
MeshGPUHandle RegisterTestTriangle(RenderServices& rs, bool retainCpuMesh)
{
    Mesh mesh{};
    mesh.Name = "SceneAsTriangle";
    Vertex v0{}, v1{}, v2{};
    v0.Position[1] = 1.0f;
    v1.Position[0] = -1.0f;
    v1.Position[1] = -1.0f;
    v2.Position[0] = 1.0f;
    v2.Position[1] = -1.0f;
    v0.Normal[2] = v1.Normal[2] = v2.Normal[2] = 1.0f;
    mesh.Vertices = {v0, v1, v2};
    mesh.Indices = {0, 1, 2};
    return rs.GetMeshGPURegistry().RegisterSubmesh({GUID::Generate(), 0}, mesh, retainCpuMesh);
}

// Mirrors kUnclaimedFramesBeforePurge in SceneAccelerationStructureService.cpp.
constexpr int kSceneAsPurgeFrames = 900;
}  // namespace

// A cascade (re)allocation clears its probe state with a compute pass recorded
// before the views. The resolve reads both cascades' buffers, and only a
// declared read orders it after each clear: without one the resolve could read the uncleared
// allocation, and its one non-finite frame stayed in the irradiance history
// for good (#2768).
TEST(RenderPipelineDeclareTests, DdgiResolveDeclaresItsProbeStateReads)
{
    auto device = CreateRayQueryDevice();
    if (!device)
        GTEST_SKIP() << "No device with ray query and an acceleration-structure backend";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        {
            SceneAccelerationStructureService scene(device.get(), &rs.GetMeshGPURegistry(), rs.GetGPUScene());
            DDGIProbeFeature feature;
            ASSERT_TRUE(feature.Initialize(device.get(), &scene, rs.GetGPUScene(), &rs.GetMeshGPURegistry(),
                                           &rs.Materials()));
            DDGIVolumeDesc volume{};
            volume.Enabled = true;
            volume.EnableGlossy = false;
            volume.EnableFineCascade = true;
            feature.SetActiveVolume(volume);

            FramePools pools(device.get());
            RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
            frame.BeginFrame(0);
            scene.BeginFrame();
            // Allocates both cascades and records their probe-state clears ahead of
            // the view, as DDGIGen does before WorldRender.
            feature.DeclareProbePasses(frame, rs, 1.0f / 60.0f, false);
            const auto probeState = feature.GetProbeStateBinding();
            const auto probeStateFine = feature.GetProbeStateBindingFine();
            ASSERT_NE(probeState.Buffer, probeStateFine.Buffer) << "both cascades must own a probe-state buffer";
            const auto depth = frame.ImportPersistentTexture("DDGI.Test.Depth", DepthTargetDesc());
            auto params = frame.AllocUpload<ViewParamsUBO>();
            ASSERT_TRUE(params.Valid());
            *params.Ptr = {};
            RenderGraph::RGTexture rough, glossy, irradiance, normal;
            ASSERT_TRUE(feature.DeclareGlossyResolveForView(frame, rs, params.Buffer, params.Offset,
                                                            sizeof(ViewParamsUBO), depth, 64, 64, 42, 1.0f,
                                                            "DDGI.Test.Resolve", rough, glossy, irradiance,
                                                            normal));
            // Imports dedup by physical, so these return the resources the clears wrote.
            const auto probeStateRG = frame.ImportExternalBuffer("DDGI.Test.ProbeState", probeState.Buffer);
            const auto probeStateFineRG =
                frame.ImportExternalBuffer("DDGI.Test.ProbeStateFine", probeStateFine.Buffer);
            ASSERT_NE(probeStateRG.Id, probeStateFineRG.Id);

            // A write recorded before a read of the same resource is a read-after-write
            // edge (hazards derive from declared accesses in recording order), so each
            // clear ordering before the resolve is the order the fix exists for.
            const auto& graph = frame.Graph();
            const auto resolve = RGQuery::FindDeclared(graph, RGQuery::Exact{"DDGI.Test.Resolve"});
            ASSERT_NE(resolve, RenderGraph::kInvalidId);
            const std::pair<const char*, RenderGraph::RGResourceId> clears[] = {
                {"DDGI.C0.Clear.ProbeState", probeStateRG.Id}, {"DDGI.C1.Clear.ProbeState", probeStateFineRG.Id}};
            for (const auto& [clearName, probeStateId] : clears)
            {
                const auto clear = RGQuery::FindDeclared(graph, RGQuery::Exact{clearName});
                ASSERT_NE(clear, RenderGraph::kInvalidId) << clearName;
                EXPECT_TRUE(graph.HasWriteAccess(clear, probeStateId)) << clearName;
                EXPECT_LT(clear, resolve) << clearName << " must be recorded before the resolve";
                EXPECT_TRUE(graph.HasReadAccess(resolve, probeStateId)) << clearName;
            }
        }
        rs.Shutdown();
    }
    device->Shutdown();
}

// The resolve samples every cascade's irradiance, depth and reflection atlases,
// which DDGIGen's upload passes write earlier in the same frame. Only declared
// accesses order passes, so each atlas must have a writer recorded before the
// resolve and a read by the resolve; two cascades with distinct atlases make
// each cascade's read observable on its own (#2837).
TEST(RenderPipelineDeclareTests, DdgiResolveDeclaresItsAtlasReads)
{
    auto device = CreateRayQueryDevice();
    if (!device)
        GTEST_SKIP() << "No device with ray query and an acceleration-structure backend";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        {
            SceneAccelerationStructureService scene(device.get(), &rs.GetMeshGPURegistry(), rs.GetGPUScene());
            DDGIProbeFeature feature;
            ASSERT_TRUE(feature.Initialize(device.get(), &scene, rs.GetGPUScene(), &rs.GetMeshGPURegistry(),
                                           &rs.Materials()));
            DDGIVolumeDesc volume{};
            volume.Enabled = true;
            volume.EnableGlossy = true;
            volume.EnableFineCascade = true;
            feature.SetActiveVolume(volume);

            FramePools pools(device.get());
            RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
            frame.BeginFrame(0);
            scene.BeginFrame();
            feature.DeclareProbePasses(frame, rs, 1.0f / 60.0f, false);
            const std::pair<const char*, TextureHandle> atlases[] = {
                {"C0 irradiance", feature.GetIrradianceAtlas()},
                {"C0 depth", feature.GetDepthAtlas()},
                {"C1 irradiance", feature.GetIrradianceAtlasFine()},
                {"C1 depth", feature.GetDepthAtlasFine()},
                {"C0 rough", feature.GetRoughAtlas()},
                {"C0 glossy", feature.GetGlossyAtlas()},
                {"C1 rough", feature.GetRoughAtlasFine()},
                {"C1 glossy", feature.GetGlossyAtlasFine()}};
            for (const auto& [label, atlas] : atlases)
                ASSERT_TRUE(atlas.IsValid()) << label << " atlas was not allocated";

            const auto depth = frame.ImportPersistentTexture("DDGI.Test.Depth", DepthTargetDesc());
            auto params = frame.AllocUpload<ViewParamsUBO>();
            ASSERT_TRUE(params.Valid());
            *params.Ptr = {};
            RenderGraph::RGTexture rough, glossy, irradiance, normal;
            ASSERT_TRUE(feature.DeclareGlossyResolveForView(frame, rs, params.Buffer, params.Offset,
                                                            sizeof(ViewParamsUBO), depth, 64, 64, 42, 1.0f,
                                                            "DDGI.Test.Resolve", rough, glossy, irradiance,
                                                            normal));
            const auto& graph = frame.Graph();
            const auto resolve = RGQuery::FindDeclared(graph, RGQuery::Exact{"DDGI.Test.Resolve"});
            ASSERT_NE(resolve, RenderGraph::kInvalidId);
            std::vector<RenderGraph::RGResourceId> ids;
            for (const auto& [label, atlas] : atlases)
            {
                // Imports dedup by physical, so this returns the resource the upload wrote.
                const auto atlasRG = frame.ImportExternalTexture(label, atlas, ResourceState::UnorderedAccess);
                ids.push_back(atlasRG.Id);
                // A write recorded before a read of the same resource is a read-after-write
                // edge (hazards derive from declared accesses in recording order).
                bool writtenBefore = false;
                for (RenderGraph::RGPassId pass = 0; pass < resolve; ++pass)
                    writtenBefore = writtenBefore || graph.HasWriteAccess(pass, atlasRG.Id);
                EXPECT_TRUE(writtenBefore) << label << " atlas has no upload recorded before the resolve";
                EXPECT_TRUE(graph.HasReadAccess(resolve, atlasRG.Id)) << label << " atlas read is not declared";
            }
            std::sort(ids.begin(), ids.end());
            EXPECT_EQ(std::adjacent_find(ids.begin(), ids.end()), ids.end()) << "two atlases share one resource";
        }
        rs.Shutdown();
    }
    device->Shutdown();
}

namespace
{
// One frame of WorldPassDeclaresDdgiGatherReads: DDGIGen, then (at Half) the
// view's resolve, then the entity world pass and a forward-command drain pass
// (sorted transparent, glass, ocean) under the DDGI keyword.
void ExpectWorldPassDeclaresDdgiGatherReads(IDevice* device, Components::DDGIGlossyResolveScale scale)
{
    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device));
    {
        SceneAccelerationStructureService scene(device, &rs.GetMeshGPURegistry(), rs.GetGPUScene());
        auto& feature = rs.EnsureFeature<DDGIProbeFeature>();
        ASSERT_TRUE(feature.Initialize(device, &scene, rs.GetGPUScene(), &rs.GetMeshGPURegistry(), &rs.Materials()));
        DDGIVolumeDesc volume{};
        volume.Enabled = true;
        volume.EnableGlossy = true;
        volume.EnableFineCascade = true;
        volume.GlossyResolveScale = scale;
        feature.SetActiveVolume(volume);

        const CameraId camId = rs.Views().AllocateCamera("DdgiGatherCam");
        const ViewId viewId = rs.Views().AllocateView("DdgiGatherView", camId);
        rs.Views().SetViewRenderLayerMask(viewId, 1u);
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        clear.clearDepthValue = 0.0f;
        rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);

        FramePools pools(device);
        RenderGraph::RGFrame frame(device, &pools.Persistent, &pools.Transient, &pools.Ring);
        frame.BeginFrame(0);
        scene.BeginFrame();
        feature.DeclareProbePasses(frame, rs, 1.0f / 60.0f, false);
        const auto probeState = feature.GetProbeStateBinding();
        const auto probeStateFine = feature.GetProbeStateBindingFine();
        ASSERT_NE(probeState.Buffer, probeStateFine.Buffer) << "both cascades must own a probe-state buffer";

        RenderServices::WorldPassTargetsRG targets;
        targets.Color = frame.ImportPersistentTexture("DdgiGather.Color", ColorTargetDesc());
        targets.Depth = frame.ImportPersistentTexture("DdgiGather.Depth", DepthTargetDesc());
        if (scale != Components::DDGIGlossyResolveScale::Full)
        {
            auto params = frame.AllocUpload<ViewParamsUBO>();
            ASSERT_TRUE(params.Valid());
            *params.Ptr = {};
            RenderGraph::RGTexture normal;
            ASSERT_TRUE(feature.DeclareGlossyResolveForView(
                frame, rs, params.Buffer, params.Offset, sizeof(ViewParamsUBO), targets.Depth, 64, 64, viewId, 1.0f,
                "DDGI.Test.Resolve", targets.DDGIResolveRough, targets.DDGIResolveGlossy,
                targets.DDGIResolveIrradiance, normal));
        }
        rs.BeginWorldDrawFrame();
        rs.BuildWorldBatchKeys();
        const auto world = rs.AddWorldPassForView(frame, viewId, targets, Rendering::MaterialKeyword::DDGI);
        ASSERT_TRUE(world.Pass.IsValid()) << "world pass declares (view clears are set)";
        // A default-constructed command is enough: declaration never inspects the payload.
        const DrawCommand dummy{};
        const auto drain = rs.AddForwardCommandPassForView(frame, viewId, targets, Rendering::MaterialKeyword::DDGI,
                                                           std::span<const DrawCommand>(&dummy, 1), {},
                                                           "DdgiGatherDrain");
        ASSERT_TRUE(drain.IsValid());

        const std::pair<const char*, TextureHandle> atlases[] = {
            {"C0 irradiance", feature.GetIrradianceAtlas()}, {"C0 depth", feature.GetDepthAtlas()},
            {"C1 irradiance", feature.GetIrradianceAtlasFine()}, {"C1 depth", feature.GetDepthAtlasFine()},
            {"C0 rough", feature.GetRoughAtlas()},           {"C0 glossy", feature.GetGlossyAtlas()},
            {"C1 rough", feature.GetRoughAtlasFine()},       {"C1 glossy", feature.GetGlossyAtlasFine()}};
        // Imports dedup by physical, so these return the resources DDGIGen's passes declared.
        std::vector<std::pair<const char*, RenderGraph::RGResourceId>> reads;
        for (const auto& [label, atlas] : atlases)
        {
            ASSERT_TRUE(atlas.IsValid()) << label << " atlas was not allocated";
            reads.emplace_back(label, frame.ImportExternalTexture(label, atlas, ResourceState::UnorderedAccess).Id);
        }
        reads.emplace_back("C0 probe state", frame.ImportExternalBuffer("C0 probe state", probeState.Buffer).Id);
        reads.emplace_back("C1 probe state", frame.ImportExternalBuffer("C1 probe state", probeStateFine.Buffer).Id);
        const auto& graph = frame.Graph();
        const std::pair<const char*, RenderGraph::RGPassId> passes[] = {{"world pass", world.Pass.Id},
                                                                        {"forward-command drain", drain.Id}};
        for (const auto& [passLabel, passId] : passes)
            for (const auto& [label, id] : reads)
            {
                // A write recorded before a read of the same resource is a read-after-write
                // edge (hazards derive from declared accesses in recording order).
                bool writtenBefore = false;
                for (RenderGraph::RGPassId pass = 0; pass < passId; ++pass)
                    writtenBefore = writtenBefore || graph.HasWriteAccess(pass, id);
                EXPECT_TRUE(writtenBefore) << label << " has no writer recorded before the " << passLabel;
                EXPECT_TRUE(graph.HasReadAccess(passId, id)) << label << " read is not declared by the " << passLabel;
            }
        rs.Shutdown();
    }
}
} // namespace

// The forward world pass and the forward-command drain sample every cascade's
// atlases and probe state through their DDGI-keyword bindings, which DDGIGen writes earlier in the frame. At
// GlossyResolveScale::Full no resolve is declared, so only the world pass's own
// reads order them after those writers; at Half they declare them as well (#2837).
TEST(RenderPipelineDeclareTests, WorldPassDeclaresDdgiGatherReads)
{
    auto device = CreateRayQueryDevice();
    if (!device)
        GTEST_SKIP() << "No device with ray query and an acceleration-structure backend";
    for (const auto scale : {Components::DDGIGlossyResolveScale::Full, Components::DDGIGlossyResolveScale::Half})
    {
        SCOPED_TRACE(scale == Components::DDGIGlossyResolveScale::Full ? "Full" : "Half");
        ExpectWorldPassDeclaresDdgiGatherReads(device.get(), scale);
    }
    device->Shutdown();
}

// The default pipeline's DDGI node swept the registry every frame even with no
// DDGI volume, so every static mesh held BLAS storage that nothing ever built.
// The pool now sweeps only while a consumer holds a channel.
TEST(RenderPipelineDeclareTests, SceneAsUnclaimedPoolCreatesNoBlas)
{
    auto device = CreateRayQueryDevice();
    if (!device)
        GTEST_SKIP() << "No device with ray query and an acceleration-structure backend";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        {
            SceneAccelerationStructureService scene(device.get(), &rs.GetMeshGPURegistry(),
                                                     rs.GetGPUScene());
            RegisterTestTriangle(rs, false);
            for (int frame = 0; frame < 3; ++frame)
                scene.BeginFrame();
            EXPECT_EQ(scene.GetBlasCount(), 0u) << "an unclaimed pool allocated BLAS storage";
            EXPECT_EQ(scene.GetLiveBlasMemoryBytes(), 0u);

            const TlasSlotHandle channel = scene.AcquireTlasChannel("Test.Consumer");
            ASSERT_TRUE(channel.IsValid());
            scene.BeginFrame();
            EXPECT_EQ(scene.GetBlasCount(), 1u) << "a claimed pool must sweep";
            scene.ReleaseTlasChannel(channel);
        }
        rs.Shutdown();
    }
    device->Shutdown();
}

// Both consumers used to sweep, and each sweep bumped the claim frame, so the
// second consumer claimed and recorded the first one's pending builds again.
TEST(RenderPipelineDeclareTests, SceneAsPendingBuildIsClaimedOncePerFrame)
{
    auto device = CreateRayQueryDevice();
    if (!device)
        GTEST_SKIP() << "No device with ray query and an acceleration-structure backend";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        {
            SceneAccelerationStructureService scene(device.get(), &rs.GetMeshGPURegistry(),
                                                     rs.GetGPUScene());
            const TlasSlotHandle shadow = scene.AcquireTlasChannel("Test.Shadow");
            const TlasSlotHandle gi = scene.AcquireTlasChannel("Test.GI");
            RegisterTestTriangle(rs, false);
            scene.BeginFrame();

            std::shared_ptr<SceneAccelerationStructureService::BuildConfirmToken> first;
            std::shared_ptr<SceneAccelerationStructureService::BuildConfirmToken> second;
            EXPECT_EQ(scene.CollectPendingBuilds(first).size(), 1u);
            EXPECT_TRUE(scene.CollectPendingBuilds(second).empty())
                << "the second consumer claimed a build the first already records";
            EXPECT_EQ(second, nullptr);

            // An unconfirmed build is offered again on the next frame.
            scene.BeginFrame();
            std::shared_ptr<SceneAccelerationStructureService::BuildConfirmToken> retry;
            EXPECT_EQ(scene.CollectPendingBuilds(retry).size(), 1u);
            ASSERT_NE(retry, nullptr);
            retry->MarkExecuted();
            scene.BeginFrame();
            EXPECT_TRUE(scene.BlasBecameReadyThisFrame());
            scene.BeginFrame();
            EXPECT_FALSE(scene.BlasBecameReadyThisFrame());

            scene.ReleaseTlasChannel(shadow);
            scene.ReleaseTlasChannel(gi);
        }
        rs.Shutdown();
    }
    device->Shutdown();
}

// After an in-place device rebuild every BLAS died with the old device and the
// backend restarted its ids, so the pool must forget its map: a kept id could
// name a fresh BLAS. The next frame then offers the cold rebuild.
TEST(RenderPipelineDeclareTests, SceneAsForgetsItsBlasMapOnADeviceRebuild)
{
    const ScopedForcedDeviceLoss loss;
    auto device = CreateRayQueryDevice();
    if (!device)
        GTEST_SKIP() << "No device with ray query and an acceleration-structure backend";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        {
            SceneAccelerationStructureService scene(device.get(), &rs.GetMeshGPURegistry(), rs.GetGPUScene());
            const TlasSlotHandle channel = scene.AcquireTlasChannel("Test.Rebuild");
            RegisterTestTriangle(rs, true);
            scene.BeginFrame();
            std::shared_ptr<SceneAccelerationStructureService::BuildConfirmToken> token;
            ASSERT_EQ(scene.CollectPendingBuilds(token).size(), 1u);
            token->MarkExecuted();
            scene.BeginFrame();
            ASSERT_EQ(scene.GetBlasCount(), 1u);

            ASSERT_TRUE(RunUntilDeviceRebuilt(*device)) << "injected loss should rebuild the device in place";
            // A standalone pool is outside RenderServices' rebuild sweep.
            scene.OnDeviceRebuilt();
            EXPECT_EQ(scene.GetBlasCount(), 0u) << "the pool kept BLAS handles from the lost device";
            scene.BeginFrame();
            std::shared_ptr<SceneAccelerationStructureService::BuildConfirmToken> rebuild;
            EXPECT_EQ(scene.CollectPendingBuilds(rebuild).size(), 1u) << "the cold rebuild was not offered";

            scene.ReleaseTlasChannel(channel);
        }
        rs.Shutdown();
    }
    device->Shutdown();
}

namespace
{
// Identity view looking down +Z through a reverse-Z perspective projection.
Rendering::CameraData RebuildTestCamera()
{
    Rendering::CameraData camera{};
    for (int i = 0; i < 16; i += 5)
        camera.view[i] = 1.0f;
    const auto projection = Mathematics::MakePerspectiveLH_ZO_ReverseZ(1.0471976f, 1.0f, 0.1f, 200.0f);
    std::memcpy(camera.proj, projection.Data(), sizeof(camera.proj));
    std::memcpy(camera.viewProj, camera.proj, sizeof(camera.proj));
    std::memcpy(camera.viewProjRel, camera.viewProj, sizeof(camera.viewProj));
    return camera;
}

// Declares the screen-space shadow mask over a cleared depth target and executes it.
void ExecuteScreenSpaceShadowsOnce(IDevice& device, ScreenSpaceShadowPasses& passes, uint64_t frameIndex)
{
    FramePools pools(&device);
    RenderGraph::RGFrame frame(&device, &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(frameIndex);
    const auto depth = frame.CreateTexture("SSShadow.Rebuild.Depth", DepthTargetDesc());
    frame.AddPass(
        "SSShadow.Rebuild.DepthClear", 0,
        [&](RenderGraph::RGPassBuilder& p)
        {
            RenderGraph::RGAttachmentOps ops{};
            ops.Load = RenderGraph::RGLoadOp::Clear;
            ops.Clear.Depth = 0.5f;
            p.AttachDepth(depth, ops);
        },
        [](RenderGraph::RGContext&) {});
    const float towardLight[3] = {0.3f, 0.1f, -1.0f};
    const auto mask = passes.DeclareMaskPass(frame, 1, depth, RebuildTestCamera(), towardLight, 0.005f);
    ASSERT_TRUE(mask.IsValid());
    frame.MarkOutput(mask);
    frame.Execute();
    device.WaitForIdle();
}

// A terrain clearance map and height texture for the mask pass to bind, created on the
// current device in ShaderResource. A device rebuild frees them with everything else.
struct TestTerrainShadowTextures
{
    TextureHandle Map{};
    TextureHandle Height{};
};

TestTerrainShadowTextures CreateTestTerrainShadowTextures(IDevice& device)
{
    TextureDesc desc{};
    desc.width = 8;
    desc.height = 8;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.format = static_cast<uint32_t>(TextureFormat::R16G16_FLOAT);
    desc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource);
    desc.initialState = ResourceState::ShaderResource;
    desc.debugName = "RTShadow.Rebuild.TerrainMap";
    TestTerrainShadowTextures out{};
    out.Map = device.CreateTexture(desc);
    desc.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
    desc.debugName = "RTShadow.Rebuild.TerrainHeight";
    out.Height = device.CreateTexture(desc);
    return out;
}

// What one ray-traced shadow frame did: whether the mask pass was declared and
// executed, and whether a pass of the frame read the terrain's clearance map (the
// mask binds the terrain only when it declares that read).
struct RayTracedShadowFrame
{
    bool MaskRan = false;
    bool TerrainRead = false;
};

// One frame of the ray-traced shadow lane: the pool sweep, the service's BLAS and
// TLAS builds, and, once the TLAS holds content, the mask and denoise passes over a
// cleared depth target, the mask also reading `terrain`'s clearance map when given.
RayTracedShadowFrame ExecuteRayTracedShadowFrame(IDevice& device, SceneAccelerationStructureService& scene,
                                                 RTShadowMaskService& service, uint64_t frameIndex,
                                                 const TestTerrainShadowTextures* terrain = nullptr)
{
    FramePools pools(&device);
    RenderGraph::RGFrame frame(&device, &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(frameIndex);
    scene.BeginFrame();
    service.Schedule(frame, 1);
    RenderGraph::RGTexture mask{};
    RenderGraph::RGResourceId mapId{};
    if (service.CanDeclareMaskPass())
    {
        const auto depth = frame.CreateTexture("RTShadow.Rebuild.Depth", DepthTargetDesc());
        frame.AddPass(
            "RTShadow.Rebuild.DepthClear", 0,
            [&](RenderGraph::RGPassBuilder& p)
            {
                RenderGraph::RGAttachmentOps ops{};
                ops.Load = RenderGraph::RGLoadOp::Clear;
                ops.Clear.Depth = 0.5f;
                p.AttachDepth(depth, ops);
            },
            [](RenderGraph::RGContext&) {});
        const Rendering::CameraData camera = RebuildTestCamera();
        const float lightTowardLight[3] = {0.3f, 1.0f, -0.2f};
        TerrainShadowMap map{};
        if (terrain)
        {
            map.Map = frame.ImportExternalTexture("RTShadow.Rebuild.TerrainMap", terrain->Map,
                                                  ResourceState::ShaderResource);
            map.MapTexture = terrain->Map;
            map.MapSide = 8;
            map.HeightTexture = terrain->Height;
            map.TerrainSizeX = 64.0f;
            map.TerrainSizeZ = 64.0f;
            map.HeightScale = 1.0f;
            map.SunX = 1.0f;
            map.TanElevation = 0.5f;
            map.Texel = 8.0f;
            map.UMin = -32.0f;
            map.VMin = -32.0f;
            map.SamplesU = 8.0f;
            map.SamplesV = 8.0f;
        }
        mask = service.DeclareMaskPass(frame, 1, depth, camera, lightTowardLight, 100.0f, 0.1f, 0.5f, false,
                                       Components::RayTracedShadowQuality::Performance, terrain ? &map : nullptr);
        if (mask.IsValid())
            frame.MarkOutput(mask);
        mapId = map.Map.IsValid() ? map.Map.Id : mapId;
    }
    frame.Execute();
    device.WaitForIdle();
    RayTracedShadowFrame out{};
    out.MaskRan = mask.IsValid();
    if (terrain && out.MaskRan)
        for (const RenderGraph::RGPassId pass : frame.Graph().ScheduledOrder())
            out.TerrainRead |= frame.Graph().HasReadAccess(pass, mapId);
    return out;
}

// Runs ray-traced shadow frames until the mask pass executes; MaskRan false if it never does.
RayTracedShadowFrame ExecuteUntilMaskPass(IDevice& device, SceneAccelerationStructureService& scene,
                                          RTShadowMaskService& service, uint64_t& frameIndex,
                                          const TestTerrainShadowTextures* terrain = nullptr)
{
    for (int i = 0; i < 8; ++i)
    {
        const RayTracedShadowFrame frame = ExecuteRayTracedShadowFrame(device, scene, service, frameIndex++, terrain);
        if (frame.MaskRan)
            return frame;
    }
    return {};
}
}  // namespace

// The mask and denoise passes bind the service's point-clamp sampler, the mask
// also its linear-clamp sampler over the terrain's clearance map and heights, and
// its TLAS is rebuilt from content state the service tracks. An in-place device
// rebuild frees the samplers and empties the TLAS: a cached sampler handle names
// nothing, so its descriptor writes are dropped and the kernels read stale
// descriptors, which faulted the GPU twice after every recovery.
TEST(RenderPipelineDeclareTests, RayTracedShadowMaskBindsLiveHandlesAfterADeviceRebuild)
{
    const ScopedForcedDeviceLoss loss;
    auto device = CreateRayQueryDevice();
    if (!device)
        GTEST_SKIP() << "No device with ray query and an acceleration-structure backend";
    if (!device->IsDescriptorBufferEnabled())
    {
        device->Shutdown();
        GTEST_SKIP() << "dropped descriptor writes are reported only on the descriptor-buffer path";
    }
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        {
            SceneAccelerationStructureService* scene = rs.EnsureSceneAccelerationStructureService();
            ASSERT_NE(scene, nullptr);
            const MeshGPUHandle triangle = RegisterTestTriangle(rs, true);
            const MeshGPUEntry* entry = rs.GetMeshGPURegistry().Find(triangle);
            ASSERT_NE(entry, nullptr);
            Rendering::GPUInstance caster{};
            caster.transform = Mathematics::Matrix4x4::Identity();
            caster.transform.Data()[14] = 5.0f;
            caster.boundingRadius = 1.0f;
            caster.meshIndex = entry->gpuMeshIndex;
            caster.flags = 1u; // casts shadows
            rs.GetGPUScene()->AddInstance(caster);

            RTShadowMaskService service(scene, rs.GetGPUScene());
            uint64_t frameIndex = 0;
            TestTerrainShadowTextures terrain = CreateTestTerrainShadowTextures(*device);
            ASSERT_TRUE(terrain.Map.IsValid() && terrain.Height.IsValid());
            const RayTracedShadowFrame first = ExecuteUntilMaskPass(*device, *scene, service, frameIndex, &terrain);
            ASSERT_TRUE(first.MaskRan) << "the mask pass never ran";
            EXPECT_TRUE(first.TerrainRead) << "the mask pass bound the terrain's clearance map";

            ASSERT_TRUE(RunUntilDeviceRebuilt(*device)) << "injected loss should rebuild the device in place";
            // RenderServices forgot the pool's BLASes; the service here is standalone.
            service.OnDeviceRebuilt();
            // The terrain's map and heights died with the old device. The test creates new ones on the
            // rebuilt device itself, standing in for the terrain's own resources, so the check below is
            // the mask's recovery alone.
            terrain = CreateTestTerrainShadowTextures(*device);
            ASSERT_TRUE(terrain.Map.IsValid() && terrain.Height.IsValid());

            std::vector<std::string> errors;
            RayTracedShadowFrame after{};
            {
                GameEngine::TestLog::ScopedEngineLogCapture capture(&errors, Logger::LogLevel::Error);
                Logger::Log::Error(kRebuildCaptureSentinel);
                after = ExecuteUntilMaskPass(*device, *scene, service, frameIndex, &terrain);
                Logger::Log::Flush();
            }
            EXPECT_TRUE(after.MaskRan) << "the mask pass never ran on the rebuilt device";
            // Without its terrain sampler the mask skips the terrain without a dropped write, and the
            // terrain casts no shadow in ray-traced mode on the rebuilt device.
            EXPECT_TRUE(after.TerrainRead) << "the mask pass bound the terrain's clearance map on the rebuilt device";
            ExpectNoDroppedDescriptorWrites(errors);
            device->DestroyTexture(terrain.Map);
            device->DestroyTexture(terrain.Height);
        }
        rs.Shutdown();
    }
    device->Shutdown();
}

namespace
{
// Counts the descriptor writes dropped for a handle that outlived its device, by set and binding,
// from the device's dropped-write log lines.
std::map<std::string, int> DroppedWritesBySetAndBinding(const std::vector<std::string>& errors)
{
    std::map<std::string, int> counts;
    for (const std::string& line : errors)
    {
        if (line.find("did not resolve") == std::string::npos)
            continue;
        const size_t set = line.find("set='");
        const size_t binding = line.find(", type=", set);
        counts[set == std::string::npos ? line : line.substr(set, binding - set)]++;
    }
    return counts;
}
} // namespace

// The terrain draws through device handles the terrain feature keeps across frames: its height
// texture and bindless slot (CBT's gHeight), its parameters and material-table buffers (the
// material's set 2). An in-place device rebuild frees all of them; a feature that keeps them binds
// handles that name nothing, so the descriptor writes are dropped and the terrain draws from stale
// descriptors (#2867). After the rebuild the feature must forget them, the heights must come back
// from their CPU source, and the terrain must draw with every write landing.
TEST(RenderPipelineDeclareTests, CbtTerrainBindsLiveHandlesAfterADeviceRebuild)
{
    const ScopedForcedDeviceLoss loss;
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    if (!f.Device->IsDescriptorBufferEnabled())
    {
        f.Down();
        GTEST_SKIP() << "dropped descriptor writes are reported only on the descriptor-buffer path";
    }
    {
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        const ViewId v = f.AddView("CbtRebuildView", 1u);
        if (!ActivateCbtTerrain(f, terrain))
        {
            f.Down();
            GTEST_SKIP() << "staged shader sources or CBT kernels unavailable";
        }
        const TerrainECS::TerrainHandle handle{3u, 1u};
        constexpr uint32 kDim = 65;
        const std::vector<float32> heights(static_cast<size_t>(kDim) * kDim, 0.25f);
        terrain.UploadHeightmap(handle, heights.data(), kDim, kDim);
        const RenderPipelineBlueprint bp = CompileTestBlueprint(f, R"json({
          "schemaVersion": 2, "pipelineName": "CbtRebuildTest",
          "passes": [
            { "id": "Upload", "type": "TerrainUpload" },
            { "id": "Prepass", "type": "DepthPrepass", "clearDepthValue": 0.0 },
            { "id": "Terrain", "type": "CBTRender" },
            { "id": "World", "type": "WorldRender" }
          ]
        })json", "cbt_rebuild.rendergraph");
        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);
        // EXPECT (not ASSERT) from here: an early return would skip f.Down().
        EXPECT_EQ(PumpCbtPrepassFrames(f, instance, v, terrain, 96u).ForwardDraws.size(), 1u)
            << "the terrain draws before the loss";

        EXPECT_TRUE(RunUntilDeviceRebuilt(*f.Device)) << "injected loss should rebuild the device in place";
        // The extraction re-uploads a terrain's heights from the terrain service's heightfield
        // once the feature has dropped its texture; this test holds the heights itself.
        EXPECT_FALSE(terrain.GetHeightmapTexture(handle).IsValid())
            << "the feature kept a height texture from the lost device";
        terrain.UploadHeightmap(handle, heights.data(), kDim, kDim);

        std::vector<std::string> errors;
        size_t forwardDraws = 0;
        {
            GameEngine::TestLog::ScopedEngineLogCapture capture(&errors, Logger::LogLevel::Error);
            Logger::Log::Error(kRebuildCaptureSentinel);
            forwardDraws = PumpCbtPrepassFrames(f, instance, v, terrain, 96u).ForwardDraws.size();
            Logger::Log::Flush();
        }
        EXPECT_EQ(forwardDraws, 1u) << "the terrain draws after the rebuild";
        EXPECT_EQ(std::count_if(errors.begin(), errors.end(),
                                [](const std::string& line) { return line.find(kRebuildCaptureSentinel) != std::string::npos; }),
                  1) << "the error capture was not live";
        for (const auto& [setAndBinding, count] : DroppedWritesBySetAndBinding(errors))
            ADD_FAILURE() << count << " dropped descriptor write(s) at " << setAndBinding;
    }
    f.Down();
}

namespace
{
void FlushTerrainUploads(TerrainFixture& f, TerrainECS::TerrainRenderFeature& terrain)
{
    auto cl = f.Device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    terrain.FlushPendingUploads(cl.get());
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    f.Device->ExecuteCommandLists(lists);
    f.Device->WaitForIdle();
}

// Every device object the terrain feature keeps across frames for one terrain, read through its
// getters, by name.
std::vector<std::pair<std::string, bool>> TerrainDeviceObjectsValid(const TerrainECS::TerrainRenderFeature& terrain,
                                                                    TerrainECS::TerrainHandle handle)
{
    using TerrainECS::TerrainGrassMap;
    using TerrainECS::TerrainCbtHeightTexture;
    std::vector<std::pair<std::string, bool>> objects{
        {"heightmap", terrain.GetHeightmapTexture(handle).IsValid()},
        {"splat map", terrain.GetSplatmapTexture(handle).IsValid()},
        {"normal map", terrain.GetNormalmapTexture(handle).IsValid()},
        {"atlas height", terrain.GetInitializedCbtHeightTexture(handle, TerrainCbtHeightTexture::AtlasHeight).IsValid()},
        {"atlas coarse height", terrain.GetInitializedCbtHeightTexture(handle, TerrainCbtHeightTexture::AtlasCoarse).IsValid()},
        {"atlas splat", terrain.GetAtlasSplatTexture(handle).IsValid()},
        {"atlas normal", terrain.GetAtlasNormalTexture(handle).IsValid()},
        {"atlas coarse splat", terrain.GetAtlasSplatCoarseTexture(handle).IsValid()},
        {"atlas coarse normal", terrain.GetAtlasNormalCoarseTexture(handle).IsValid()},
        {"unified grass field", terrain.GetGrassFieldTexture(handle, TerrainGrassMap::Unified).IsValid()},
        {"atlas grass field", terrain.GetGrassFieldTexture(handle, TerrainGrassMap::Atlas).IsValid()},
        {"coarse grass field", terrain.GetGrassFieldTexture(handle, TerrainGrassMap::Coarse).IsValid()},
    };
    for (uint32 slot = 0; slot < GrassWindVolumeRing::kSlots; ++slot)
        objects.emplace_back("wind volume ring slot " + std::to_string(slot),
                             terrain.GrassWindVolumes().Buffer(slot).IsValid());
    return objects;
}
} // namespace

// The extraction owns a terrain's uploads from the terrain service's CPU data. After a device
// rebuild the terrain feature has dropped every texture (CbtTerrainBindsLiveHandlesAfterADeviceRebuild);
// the extraction must see the feature's device-resource epoch move and upload the heights and the
// surface maps again, though nothing in the terrain changed (#2867).
TEST(RenderPipelineDeclareTests, TerrainExtractionUploadsAgainAfterADeviceRebuild)
{
    const ScopedForcedDeviceLoss loss;
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        EXPECT_TRUE(terrain.Initialize(f.Device.get()));
        ECS::World world(nullptr);
        auto entity = world.Create();
        entity.Set(Components::WorldTransform{});
        Components::Terrain authored{};
        authored.SizeX = 64.0f;
        authored.SizeZ = 64.0f;
        authored.HeightScale = 8.0f;
        authored.SamplesPerMeter = 1.0f;
        entity.Set(authored);
        world.ProcessCommands();
        TerrainECS::TerrainExtractionSystem extraction(f.Rs.get());

        // The terrain is provisioned and its first upload queued within a few ticks.
        TerrainECS::TerrainHandle handle{};
        for (int tick = 0; tick < 8 && !terrain.GetHeightmapTexture(handle).IsValid(); ++tick)
        {
            extraction.Update(world, 1.0f / 60.0f);
            const auto* provisioned = world.GetComponent<Components::Terrain>(entity.GetHandle());
            handle = TerrainECS::TerrainHandle{provisioned->TerrainDataHandle, provisioned->TerrainDataGeneration};
        }
        EXPECT_TRUE(terrain.GetHeightmapTexture(handle).IsValid()) << "the terrain uploads before the loss";
        EXPECT_TRUE(terrain.GetSplatmapTexture(handle).IsValid());

        EXPECT_TRUE(RunUntilDeviceRebuilt(*f.Device)) << "injected loss should rebuild the device in place";
        EXPECT_FALSE(terrain.GetHeightmapTexture(handle).IsValid()) << "the feature kept a texture from the lost device";

        extraction.Update(world, 1.0f / 60.0f);
        EXPECT_TRUE(terrain.GetHeightmapTexture(handle).IsValid()) << "the heights were not uploaded again";
        EXPECT_TRUE(terrain.GetSplatmapTexture(handle).IsValid()) << "the splat map was not uploaded again";
        EXPECT_TRUE(terrain.GetNormalmapTexture(handle).IsValid()) << "the normal map was not uploaded again";
        // The tick that queues the new texture's initializer must not hand it to the CBT renderer
        // (TerrainCbtHeightTexturesArePublishedOnlyOnceInitialized); the tick after the upload pass does.
        const auto queued = terrain.GetActiveHeightSource();
        EXPECT_TRUE(queued.Present) << "the extraction did not publish the terrain";
        EXPECT_FALSE(queued.HeightmapTexture.IsValid())
            << "the extraction published the height texture in the tick that queues its initializer";
        FlushTerrainUploads(f, terrain);
        extraction.Update(world, 1.0f / 60.0f);
        EXPECT_EQ(terrain.GetActiveHeightSource().HeightmapTexture, terrain.GetHeightmapTexture(handle))
            << "the extraction did not publish the height texture once it was initialized";
    }
    f.Down();
}

// The CBT renderer binds the terrain's height textures (the unified heightmap, or the resident atlas
// and its coarse field) through its own descriptor ring, with no render-graph edge from the upload
// pass, so it must only ever see a texture whose initializer has been recorded. After a device
// rebuild every height texture is new; publishing one in the frame its zero-clear is queued sampled
// it in its undefined layout (#2867, VUID-vkCmdDraw-None-09600).
TEST(RenderPipelineDeclareTests, TerrainCbtHeightTexturesArePublishedOnlyOnceInitialized)
{
    using TerrainECS::TerrainCbtHeightTexture;
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        EXPECT_TRUE(terrain.Initialize(f.Device.get()));
        const TerrainECS::TerrainHandle handle{3u, 1u};
        constexpr uint32 kDim = 65;
        const std::vector<float32> heights(static_cast<size_t>(kDim) * kDim, 0.25f);
        terrain.UploadHeightmap(handle, heights.data(), kDim, kDim);
        terrain.EnsureAtlasHeightTexture(handle, 64u);
        terrain.EnsureAtlasCoarseTexture(handle, 16u);
        EXPECT_TRUE(terrain.GetHeightmapTexture(handle).IsValid());
        for (const TerrainCbtHeightTexture source :
             {TerrainCbtHeightTexture::Heightmap, TerrainCbtHeightTexture::AtlasHeight, TerrainCbtHeightTexture::AtlasCoarse})
            EXPECT_FALSE(terrain.GetInitializedCbtHeightTexture(handle, source).IsValid())
                << "height source " << static_cast<int>(source) << " was published before its initializer was recorded";

        FlushTerrainUploads(f, terrain);
        EXPECT_EQ(terrain.GetInitializedCbtHeightTexture(handle, TerrainCbtHeightTexture::Heightmap),
                  terrain.GetHeightmapTexture(handle))
            << "the recorded copy initializes the texture";
        for (const TerrainCbtHeightTexture source : {TerrainCbtHeightTexture::AtlasHeight, TerrainCbtHeightTexture::AtlasCoarse})
            EXPECT_TRUE(terrain.GetInitializedCbtHeightTexture(handle, source).IsValid())
                << "the recorded clear initializes height source " << static_cast<int>(source);
    }
    f.Down();
}

// An in-place device rebuild frees every device object the terrain feature holds. Each one the
// rebuild hook leaves out keeps a handle that names nothing, or a reissued object of the new device
// (#2867), so right after the rebuild every getter must read invalid until the extraction uploads.
TEST(RenderPipelineDeclareTests, TerrainFeatureForgetsEveryDeviceObjectAtARebuild)
{
    const ScopedForcedDeviceLoss loss;
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        EXPECT_TRUE(terrain.Initialize(f.Device.get()));
        const TerrainECS::TerrainHandle handle{3u, 1u};
        constexpr uint32 kDim = 65;
        const std::vector<float32> heights(static_cast<size_t>(kDim) * kDim, 0.25f);
        const std::vector<uint8> fourBytesPerTexel(static_cast<size_t>(kDim) * kDim * 4u, 0u);
        terrain.UploadHeightmap(handle, heights.data(), kDim, kDim);
        terrain.UploadSplatmap(handle, fourBytesPerTexel.data(), kDim, kDim);
        terrain.UploadNormalmap(handle, fourBytesPerTexel.data(), kDim, kDim);
        terrain.EnsureAtlasHeightTexture(handle, 64u);
        terrain.EnsureAtlasCoarseTexture(handle, 16u);
        terrain.EnsureAtlasSplatTexture(handle, 64u);
        terrain.EnsureAtlasNormalTexture(handle, 64u);
        terrain.EnsureAtlasSplatCoarseTexture(handle, 16u);
        terrain.EnsureAtlasNormalCoarseTexture(handle, 16u);
        for (const TerrainECS::TerrainGrassMap map :
             {TerrainECS::TerrainGrassMap::Unified, TerrainECS::TerrainGrassMap::Atlas, TerrainECS::TerrainGrassMap::Coarse})
            EXPECT_TRUE(terrain.EnsureGrassFieldTexture(handle, map, 16u, 16u));
        const Rendering::WindVolumeGPU volume{};
        for (uint32 slot = 0; slot < GrassWindVolumeRing::kSlots; ++slot)
            terrain.GrassWindVolumes().Upload(*f.Device, slot, &volume, 1u);
        FlushTerrainUploads(f, terrain);
        for (const auto& [name, valid] : TerrainDeviceObjectsValid(terrain, handle))
            EXPECT_TRUE(valid) << name << " was not created before the loss";

        EXPECT_TRUE(RunUntilDeviceRebuilt(*f.Device)) << "injected loss should rebuild the device in place";
        for (const auto& [name, valid] : TerrainDeviceObjectsValid(terrain, handle))
            EXPECT_FALSE(valid) << "the feature kept the " << name << " from the lost device";
    }
    f.Down();
}

// The screen-space shadow passes cache their point-clamp sampler. An in-place
// device rebuild frees it, so a kept handle drops the passes' sampler writes.
TEST(RenderPipelineDeclareTests, ScreenSpaceShadowsBindLiveHandlesAfterADeviceRebuild)
{
    const ScopedForcedDeviceLoss loss;
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    if (!device->IsDescriptorBufferEnabled())
    {
        device->Shutdown();
        GTEST_SKIP() << "dropped descriptor writes are reported only on the descriptor-buffer path";
    }
    {
        ScreenSpaceShadowPasses passes(device.get());
        ExecuteScreenSpaceShadowsOnce(*device, passes, 0);
        ASSERT_TRUE(RunUntilDeviceRebuilt(*device)) << "injected loss should rebuild the device in place";
        // A standalone instance is outside RenderServices' rebuild sweep.
        passes.OnDeviceRebuilt();

        std::vector<std::string> errors;
        {
            GameEngine::TestLog::ScopedEngineLogCapture capture(&errors, Logger::LogLevel::Error);
            Logger::Log::Error(kRebuildCaptureSentinel);
            ExecuteScreenSpaceShadowsOnce(*device, passes, 1);
            Logger::Log::Flush();
        }
        ExpectNoDroppedDescriptorWrites(errors);
    }
    device->Shutdown();
}

// The RT shadow mask's idle purge ran while DDGI still traced the shared pool:
// it released DDGI's TLAS too, and on Metal the rebuilt BLAS pool reproduced
// the same instance hash, so DDGI traced an unwritten TLAS binding every frame.
// The pool now purges only after it has gone unclaimed, never on a consumer's
// request.
TEST(RenderPipelineDeclareTests, SceneAsPurgeWaitsForTheLastClaim)
{
    if (std::getenv("GE_SCENE_AS_PURGE_FRAMES"))
        GTEST_SKIP() << "GE_SCENE_AS_PURGE_FRAMES overrides the purge threshold this test counts";
    auto device = CreateRayQueryDevice();
    if (!device)
        GTEST_SKIP() << "No device with ray query and an acceleration-structure backend";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        {
            SceneAccelerationStructureService scene(device.get(), &rs.GetMeshGPURegistry(),
                                                     rs.GetGPUScene());
            IAccelerationStructureBackend* backend = scene.GetBackend();
            ASSERT_NE(backend, nullptr);
            const TlasSlotHandle shadow = scene.AcquireTlasChannel("Test.Shadow");
            const TlasSlotHandle gi = scene.AcquireTlasChannel("Test.GI");
            ASSERT_TRUE(shadow.IsValid());
            ASSERT_TRUE(gi.IsValid());
            RegisterTestTriangle(rs, false);
            scene.BeginFrame();
            ASSERT_TRUE(backend->PrepareTlas(gi, 1));
            const uint64_t giTlas = backend->GetTlasDeviceAddress(gi);
            ASSERT_NE(giTlas, 0u);

            scene.ReleaseTlasChannel(shadow);
            for (int frame = 0; frame <= kSceneAsPurgeFrames; ++frame)
                scene.BeginFrame();
            EXPECT_EQ(scene.GetBlasCount(), 1u) << "the pool was purged under a held claim";
            EXPECT_EQ(backend->GetTlasDeviceAddress(gi), giTlas)
                << "an idle consumer released another consumer's TLAS";

            scene.ReleaseTlasChannel(gi);
            for (int frame = 1; frame < kSceneAsPurgeFrames; ++frame)
                scene.BeginFrame();
            EXPECT_EQ(scene.GetBlasCount(), 1u) << "the pool was purged inside the grace window";
            scene.BeginFrame();
            EXPECT_EQ(scene.GetBlasCount(), 0u) << "an unclaimed pool was never purged";
            EXPECT_EQ(scene.GetLiveBlasMemoryBytes(), 0u);
        }
        rs.Shutdown();
    }
    device->Shutdown();
}

// The trace kernels sample the IBL irradiance cube on every miss. The sky bake
// writes that cube (storage write, then sampled) in the frames after a scene
// opens, which are the same frames DDGI first traces in; with the read undeclared
// the render graph neither ordered the trace after the bake nor waited for it
// across queues (#2532).
TEST(RenderPipelineDeclareTests, DdgiTraceDeclaresItsSkyIrradianceRead)
{
    auto device = CreateRayQueryDevice();
    if (!device)
        GTEST_SKIP() << "No device with ray query and an acceleration-structure backend";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        ASSERT_TRUE(rs.EnsureFeature<ImageBasedLightingFeature>().Initialize(device.get()));
        {
            SceneAccelerationStructureService scene(device.get(), &rs.GetMeshGPURegistry(),
                                                     rs.GetGPUScene());
            DDGIProbeFeature feature;
            ASSERT_TRUE(feature.Initialize(device.get(), &scene, rs.GetGPUScene(), &rs.GetMeshGPURegistry(),
                                           &rs.Materials()));
            DDGIVolumeDesc volume{};
            volume.Enabled = true;
            feature.SetActiveVolume(volume);

            FramePools pools(device.get());
            RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
            frame.BeginFrame(0);
            scene.BeginFrame();
            feature.DeclareProbePasses(frame, rs, 1.0f / 60.0f, false);

            const auto& graph = frame.Graph();
            RenderGraph::RGResourceId sky = RenderGraph::kInvalidId;
            for (uint32_t r = 0; r < graph.ResourceCount(); ++r)
                if (graph.ResourceName(r) && std::string_view(graph.ResourceName(r)) == "IBL_Irradiance")
                    sky = r;
            uint32_t traces = 0;
            for (uint32_t p = 0; p < graph.PassCount(); ++p)
            {
                const std::string_view name = graph.PassName(p) ? graph.PassName(p) : "";
                if (!name.ends_with(".TraceHW"))
                    continue;
                ++traces;
                ASSERT_NE(sky, RenderGraph::kInvalidId) << name << " samples a sky cube the graph never saw";
                EXPECT_TRUE(graph.HasReadAccess(p, sky)) << name;
            }
            EXPECT_GT(traces, 0u) << "no trace pass was declared; the check is void";
        }
        rs.Shutdown();
    }
    device->Shutdown();
}

// The trace and classify passes ray-query the TLAS that DDGI.ASBuild builds in the
// same frame. With no resource connecting them the graph was free to schedule a
// trace ahead of the build, and the first traces after a scene opened traversed a
// TLAS that had never been built (#2532). Every reader must read the structure the
// build writes; RGSubmission.AccelerationStructureReaderWaitsForItsBuildAcrossQueues
// pins that a read of a written structure orders the reader and waits for it.
TEST(RenderPipelineDeclareTests, DdgiTlasReadersReadWhatTheBuildWrites)
{
    auto device = CreateRayQueryDevice();
    if (!device)
        GTEST_SKIP() << "No device with ray query and an acceleration-structure backend";
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        ASSERT_TRUE(rs.EnsureFeature<ImageBasedLightingFeature>().Initialize(device.get()));
        {
            SceneAccelerationStructureService scene(device.get(), &rs.GetMeshGPURegistry(),
                                                     rs.GetGPUScene());
            DDGIProbeFeature feature;
            ASSERT_TRUE(feature.Initialize(device.get(), &scene, rs.GetGPUScene(), &rs.GetMeshGPURegistry(),
                                           &rs.Materials()));
            DDGIVolumeDesc volume{};
            volume.Enabled = true;
            feature.SetActiveVolume(volume);

            FramePools pools(device.get());
            RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
            frame.BeginFrame(0);
            scene.BeginFrame();
            feature.DeclareProbePasses(frame, rs, 1.0f / 60.0f, false);

            const auto& graph = frame.Graph();
            RenderGraph::RGPassId build = RenderGraph::kInvalidId;
            for (uint32_t p = 0; p < graph.PassCount(); ++p)
                if (graph.PassName(p) && std::string_view(graph.PassName(p)) == "DDGI.ASBuild")
                    build = p;
            ASSERT_NE(build, RenderGraph::kInvalidId) << "the first tick must build the TLAS";

            uint32_t readers = 0;
            for (uint32_t p = 0; p < graph.PassCount(); ++p)
            {
                const std::string_view name = graph.PassName(p) ? graph.PassName(p) : "";
                if (!name.ends_with(".TraceHW") && !name.ends_with(".Classify"))
                    continue;
                ++readers;
                bool readsTheBuild = false;
                for (uint32_t r = 0; r < graph.ResourceCount(); ++r)
                    readsTheBuild |= graph.HasWriteAccess(build, r) && graph.HasReadAccess(p, r) &&
                                     graph.ResourceDesc(r).Kind == RenderGraph::RGResourceKind::AccelerationStructure;
                EXPECT_TRUE(readsTheBuild) << name << " ray-queries a TLAS it does not read from DDGI.ASBuild";
            }
            EXPECT_GE(readers, 2u) << "the trace and classify passes were not declared; the check is void";
        }
        rs.Shutdown();
    }
    device->Shutdown();
}

namespace
{
// A 65 x 65 R32F height texture holding a ridge along z (normalized heights), uploaded and in
// ShaderResource, with its bindless index (0 when the device has no bindless: the caller skips).
uint32 UploadRidgeHeightTexture(TerrainFixture& f, TextureHandle& out)
{
    constexpr uint32 kDim = 65;
    TextureDesc desc{};
    desc.width = kDim;
    desc.height = kDim;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
    desc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
    desc.debugName = "TerrainShadowTest.Heights";
    out = f.Device->CreateTexture(desc);
    if (!out.IsValid())
        return 0u;
    std::vector<float> heights(static_cast<size_t>(kDim) * kDim);
    for (uint32 z = 0; z < kDim; ++z)
        for (uint32 x = 0; x < kDim; ++x)
            heights[static_cast<size_t>(z) * kDim + x] = x == kDim / 2u ? 0.5f : 0.0f;
    const size_t bytes = heights.size() * sizeof(float);
    const BufferHandle upload = f.Device->CreateUploadBuffer(bytes, "TerrainShadowTest.Upload");
    if (!upload.IsValid())
        return 0u;
    f.Device->UpdateBuffer(upload, 0, bytes, heights.data());
    auto cl = f.Device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(out, ResourceState::Undefined, ResourceState::CopyDest));
    cl->CopyBufferToTextureSubresource(upload, out, 0, 0, kDim, kDim, 0, kDim * sizeof(float));
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(out, ResourceState::CopyDest, ResourceState::ShaderResource));
    cl->End();
    f.Device->ExecuteCommandLists({cl.get()});
    f.Device->WaitForIdle();
    f.Device->DestroyBuffer(upload);
    return f.Rs->Textures().GetBindlessIndex(out);
}

// The terrain ActivateCbtTerrain brings up, with the ridge as its height source.
void SetShadowTestTerrain(TerrainECS::TerrainRenderFeature& terrain, TextureHandle heights, uint32 heightIndex,
                          bool castShadows)
{
    TerrainECS::TerrainInstanceInfo info{};
    info.Handle = TerrainECS::TerrainHandle{3u, 1u};
    info.SizeX = 512.0f;
    info.SizeZ = 512.0f;
    info.HeightScale = 64.0f;
    info.LODRangeScale = 1.0f;
    info.LODLevels = 1u;
    info.HeightmapTexture = heights;
    info.HeightmapBindlessIndex = heightIndex;
    info.CastShadows = castShadows;
    std::vector<TerrainECS::TerrainInstanceInfo> infos;
    infos.push_back(info);
    terrain.SetActiveTerrains(std::move(infos));
}

// One declared and executed frame of the terrain's shadow: whether the bake was scheduled, its place
// and the world pass's, and whether the map was published for the view.
struct TerrainShadowFrame
{
    bool Baked = false;
    bool Published = false;
    size_t BakeOrder = SIZE_MAX;
    size_t WorldOrder = SIZE_MAX;
    // The map as the GPU baked it (MapSide x MapSide clearances and occluder distances), when the frame
    // read it back.
    std::vector<float> Map;
    std::vector<float> Distances;
    uint32 MapSide = 0;
    // ShadowData's terrain presence word for the view (terrainShadowSource.z), -1 when the frame had none.
    float ShadowDataTerrainPresence = -1.0f;
};

// The terrain shadow frames' sun disc (degrees, full): the bake follows its lower edge too. Wide, so that
// at 15 degrees the ridge's lit penumbra band (where the lower edge's caster is the ridge and the centre
// ray's is the ground just upwind) spans about two of the map's 8 m samples per line, on the terrain; at
// 5 degrees that band falls off the terrain's edge.
constexpr float kTerrainShadowSunDiameterDeg = 4.0f;

// `pools` outlives the frames: the map is a persistent pool texture, and a new pool would hand out a
// fresh one (and a full bake) every frame.
TerrainShadowFrame DeclareTerrainShadowFrame(TerrainFixture& f, FramePools& pools, RenderPipelineInstance& instance,
                                             ViewId v, TerrainECS::TerrainRenderFeature& terrain, uint32 frameIndex,
                                             float sunElevationDeg, bool readMap = false)
{
    // The bake publishes to the shadow feature, which must exist when the frame is declared.
    auto& shadows = f.Rs->EnsureFeature<ShadowMapRenderFeature>();
    UploadCbtTerrainParams(terrain, frameIndex);
    RenderGraph::RGFrame frame(f.Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(frameIndex);
    const RenderGraph::RGTexture color = frame.ImportPersistentTexture("TS.Color", ColorTargetDesc());
    const RenderGraph::RGTexture depth = frame.ImportPersistentTexture("TS.Depth", DepthTargetDesc());
    f.Rs->BeginWorldDrawFrame();
    f.Rs->BuildWorldBatchKeys();
    ExtractedLight sun{};
    sun.type = GameEngine::Components::LightType::Directional;
    sun.castsShadows = 1;
    sun.castsLight = 1;
    const float e = sunElevationDeg * GameEngine::Mathematics::Pi / 180.0f;
    sun.directionWS[0] = -std::cos(e); // the sun on the +X side, its light travelling toward -X
    sun.directionWS[1] = -std::sin(e);
    sun.directionWS[2] = 0.0f;
    sun.shadowAngularDiameter = kTerrainShadowSunDiameterDeg;
    f.Rs->SubmitLight(0u, sun);
    const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{v, color, depth, {}}};
    const std::vector<Rendering::ViewDesc> views(f.Rs->Views().GetViews().begin(), f.Rs->Views().GetViews().end());
    instance.Declare(frame, targets, views);
    frame.MarkOutput(color);
    TerrainShadowFrame out{};
    const TerrainShadowMap* published = shadows.FindTerrainShadowMap(v, frame.FrameIndex());
    out.Published = published != nullptr;
    std::shared_ptr<Rendering::RGReadbackTicket> mapTicket;
    if (readMap && published)
    {
        mapTicket = Rendering::RequestTextureReadbackRG(f.Device.get(), frame, published->Map, "TerrainShadowTest.Map");
        out.MapSide = published->MapSide;
    }
    frame.Execute();
    if (mapTicket)
        Rendering::OnFrameSubmittedReadbacksRG(frame, frame.SubmissionToken());
    f.Device->WaitForIdle();
    Rendering::ViewReadbackResult mapResult{};
    if (mapTicket && mapTicket->TryGet(mapResult) && mapResult.pixels.size() >= size_t(out.MapSide) * out.MapSide * 4u)
    {
        out.Map.resize(size_t(out.MapSide) * out.MapSide);
        out.Distances.resize(out.Map.size());
        for (size_t i = 0; i < out.Map.size(); ++i)
        {
            uint16_t bits[2] = {};
            std::memcpy(bits, mapResult.pixels.data() + i * 4u, sizeof(bits));
            out.Map[i] = GameEngine::Mathematics::HalfToFloat(bits[0]);
            out.Distances[i] = GameEngine::Mathematics::HalfToFloat(bits[1]);
        }
    }
    if (const auto* resources = instance.FrameResourcesFor(&frame))
    {
        const auto binding = resources->Buffers.find({v, "ShadowData"});
        if (binding != resources->Buffers.end())
        {
            const auto* mapped = static_cast<const uint8_t*>(f.Device->MapBuffer(binding->second.Buffer));
            if (mapped)
            {
                ShadowDataGPU data{};
                std::memcpy(&data, mapped + binding->second.Offset, sizeof(data));
                out.ShadowDataTerrainPresence = static_cast<float>(data.terrainShadowSource[2]);
                f.Device->UnmapBuffer(binding->second.Buffer);
            }
        }
    }
    const RenderGraph::RGGraph& graph = frame.Graph();
    const auto& order = graph.ScheduledOrder();
    for (size_t k = 0; k < order.size(); ++k)
    {
        const char* name = graph.PassName(order[k]);
        if (!name)
            continue;
        if (std::string_view(name) == "TerrainShadow.Bake")
            out.BakeOrder = k;
        if (std::string_view(name).starts_with("RenderEntities[") && out.WorldOrder == SIZE_MAX)
            out.WorldOrder = k;
    }
    out.Baked = out.BakeOrder != SIZE_MAX;
    return out;
}
// The clearance the bake must store for UploadRidgeHeightTexture's terrain (512 m, 64 cells, the ridge 32 m
// high along x = 256) and a sun `sunElevationDeg` up on the +X side: a brute-force maximum over the casters
// upwind of each grid sample, independent of the kernel's runs and scan. NaN marks a sample whose sign the
// comparison skips (within 0.5 m of zero, or next to a sample of the other sign: the edge). `outDistances`
// holds the distance to the caster that attains the maximum, NaN where a second caster comes within 1 cm.
// `outLit` holds, for a clearly lit sample, what the bake stores there: the margin under the centre ray and
// the distance of the caster that blocks the sun disc's lower edge most (an independent maximum along the
// slope TanLowerEdge), and whether that caster differs from the centre ray's; NaN elsewhere.
struct RidgeLitReference
{
    std::vector<float> Margin;
    std::vector<float> Distance;
    std::vector<uint8_t> OtherCaster;
};
std::vector<float> ReferenceRidgeClearanceSigns(float sunElevationDeg, uint32& outSide, std::vector<float>& outDistances,
                                                RidgeLitReference& outLit)
{
    const float e = sunElevationDeg * GameEngine::Mathematics::Pi / 180.0f;
    const float towardSun[3] = {std::cos(e), std::sin(e), 0.0f};
    const CBTTerrain::TerrainShadowExtent extent{512.0f, 512.0f, 64u, 64u};
    const CBTTerrain::TerrainShadowGrid grid = *CBTTerrain::ComputeTerrainShadowGrid(
        extent, towardSun, ResolveShadowTanHalfAngle(kTerrainShadowSunDiameterDeg));
    outSide = CBTTerrain::TerrainShadowMapSide(extent);
    auto ground = [](float x, float z)
    {
        const float lx = std::clamp(x / 512.0f, 0.0f, 1.0f) * 64.0f;
        (void)z; // the ridge does not vary along z
        const float i = std::floor(std::min(lx, 63.0f));
        const float fx = lx - i;
        auto h = [](float column) { return column == 32.0f ? 32.0f : 0.0f; };
        return h(i) + (h(i + 1.0f) - h(i)) * fx;
    };
    const float tanE = grid.TanElevation;
    std::vector<float> clearance(size_t(outSide) * outSide, std::nanf(""));
    outDistances.assign(clearance.size(), std::nanf(""));
    outLit.Margin.assign(clearance.size(), std::nanf(""));
    outLit.Distance.assign(clearance.size(), std::nanf(""));
    outLit.OtherCaster.assign(clearance.size(), 0u);
    const float tanL = grid.TanLowerEdge;
    for (uint32 line = 0; line < grid.SamplesV; ++line)
    {
        std::vector<float> caster(grid.SamplesU), base(grid.SamplesU);
        for (uint32 k = 0; k < grid.SamplesU; ++k)
        {
            const float u = grid.UMin + float(k) * grid.Texel;
            const float vv = grid.VMin + float(line) * grid.Texel;
            const float x = 256.0f + u * grid.SunX - vv * grid.SunZ;
            const float z = 256.0f + u * grid.SunZ + vv * grid.SunX;
            base[k] = ground(x, z);
            const bool onTerrain = x >= 0.0f && x <= 512.0f && z >= 0.0f && z <= 512.0f;
            caster[k] = onTerrain ? base[k] : -1.0e30f;
        }
        for (uint32 k = 0; k < grid.SamplesU; ++k)
        {
            float top = -1.0e30f;
            float runnerUp = -1.0e30f;
            uint32 argmax = k;
            for (uint32 j = k + 1; j < grid.SamplesU; ++j)
            {
                const float candidate = caster[j] - float(j - k) * grid.Texel * tanE;
                if (candidate > top)
                {
                    runnerUp = top;
                    top = candidate;
                    argmax = j;
                }
                else
                {
                    runnerUp = std::max(runnerUp, candidate);
                }
            }
            clearance[size_t(line) * outSide + k] = top - base[k];
            if (top > -1.0e29f && top - runnerUp > 0.01f)
                outDistances[size_t(line) * outSide + k] = float(argmax - k) * grid.Texel;
            float lowerTop = -1.0e30f;
            float lowerRunnerUp = -1.0e30f;
            uint32 lowerArgmax = k;
            for (uint32 j = k + 1; j < grid.SamplesU; ++j)
            {
                const float candidate = caster[j] - float(j - k) * grid.Texel * tanL;
                if (candidate > lowerTop)
                {
                    lowerRunnerUp = lowerTop;
                    lowerTop = candidate;
                    lowerArgmax = j;
                }
                else
                {
                    lowerRunnerUp = std::max(lowerRunnerUp, candidate);
                }
            }
            const float c = top - base[k];
            if (c < -0.01f && lowerTop > -1.0e29f && lowerTop - lowerRunnerUp > 0.01f)
            {
                const float d = float(lowerArgmax - k) * grid.Texel;
                outLit.Margin[size_t(line) * outSide + k] = std::min(lowerTop - d * (tanE - tanL) - base[k], c);
                outLit.Distance[size_t(line) * outSide + k] = d;
                outLit.OtherCaster[size_t(line) * outSide + k] = lowerArgmax != argmax ? 1u : 0u;
            }
        }
    }
    std::vector<float> signs(clearance.size(), std::nanf(""));
    for (uint32 line = 1; line + 1 < grid.SamplesV; ++line)
    {
        for (uint32 k = 1; k + 1 < grid.SamplesU; ++k)
        {
            const float c = clearance[size_t(line) * outSide + k];
            if (std::abs(c) < 0.5f)
                continue;
            bool edge = false;
            for (int dl = -1; dl <= 1; ++dl)
                for (int dk = -1; dk <= 1; ++dk)
                    edge |= (clearance[size_t(int(line) + dl) * outSide + size_t(int(k) + dk)] > 0.0f) != (c > 0.0f);
            if (!edge)
                signs[size_t(line) * outSide + k] = c > 0.0f ? 1.0f : -1.0f;
        }
    }
    return signs;
}
// The map the GPU baked for the ridge (read back) against ReferenceRidgeClearanceSigns: the same sign at
// every sample away from an edge, with shadowed and lit samples both present; in the ridge's shadow the
// distance to the ridge that sizes the penumbra; on lit samples the margin and distance of the caster that
// blocks the sun disc's lower edge most, at least `minLitOtherCaster` of them where that caster is not the
// centre ray's (the ridge's lit penumbra band, where a bake that follows the centre ray twice differs).
void ExpectBakedRidgeMatchesReference(const TerrainShadowFrame& frame, float sunElevationDeg, size_t minLitOtherCaster)
{
    uint32 referenceSide = 0;
    std::vector<float> referenceDistances;
    RidgeLitReference lit;
    const std::vector<float> reference =
        ReferenceRidgeClearanceSigns(sunElevationDeg, referenceSide, referenceDistances, lit);
    EXPECT_EQ(frame.MapSide, referenceSide);
    ASSERT_EQ(frame.Map.size(), reference.size()) << "the baked map was read back";
    size_t compared = 0, shadowed = 0, mismatches = 0;
    for (size_t i = 0; i < reference.size(); ++i)
    {
        if (std::isnan(reference[i]))
            continue;
        ++compared;
        shadowed += reference[i] > 0.0f ? 1u : 0u;
        mismatches += ((frame.Map[i] > 0.0f) != (reference[i] > 0.0f)) ? 1u : 0u;
    }
    EXPECT_GT(shadowed, 20u) << "the ridge's shadow covers samples, so a lit-everywhere bake shows";
    EXPECT_GT(compared, shadowed + 20u);
    EXPECT_EQ(mismatches, 0u) << sunElevationDeg << " degrees: " << mismatches << " of " << compared
                              << " samples disagree with the brute-force maximum";

    ASSERT_EQ(frame.Distances.size(), reference.size());
    size_t distancesCompared = 0, distanceMismatches = 0;
    for (size_t i = 0; i < reference.size(); ++i)
    {
        if (std::isnan(reference[i]) || reference[i] < 0.0f || std::isnan(referenceDistances[i]))
            continue;
        ++distancesCompared;
        // RG16F holds a distance of up to 512 m to a quarter metre.
        distanceMismatches += std::abs(frame.Distances[i] - referenceDistances[i]) > 0.25f ? 1u : 0u;
    }
    EXPECT_GT(distancesCompared, 20u) << "samples in the ridge's shadow carry its distance";
    EXPECT_EQ(distanceMismatches, 0u) << sunElevationDeg << " degrees: " << distanceMismatches << " of "
                                      << distancesCompared << " shadowed samples store another occluder distance";

    size_t litCompared = 0, litOtherCaster = 0, litMismatches = 0;
    for (size_t i = 0; i < lit.Distance.size(); ++i)
    {
        if (std::isnan(lit.Distance[i]))
            continue;
        ++litCompared;
        litOtherCaster += lit.OtherCaster[i];
        // RG16F: a quarter metre of distance up to 512 m; the margin to about a part in a thousand.
        const bool distanceOff = std::abs(frame.Distances[i] - lit.Distance[i]) > 0.25f;
        const bool marginOff = std::abs(frame.Map[i] - lit.Margin[i]) > 0.05f + 2.0e-3f * std::abs(lit.Margin[i]);
        litMismatches += (distanceOff || marginOff) ? 1u : 0u;
    }
    EXPECT_GE(litOtherCaster, minLitOtherCaster) << sunElevationDeg << " degrees: lit samples whose lower-edge caster "
                                                 << "is not the centre ray's (of " << litCompared << " lit)";
    EXPECT_EQ(litMismatches, 0u) << sunElevationDeg << " degrees: " << litMismatches << " of " << litCompared
                                 << " lit samples store another lower-edge occluder";
}
} // namespace

// The terrain's clearance map (TerrainShadowBake) is baked when its inputs change and only then: the first
// frame bakes it ahead of the world pass that samples it, frames with the same sun and heights declare no bake
// and keep publishing the map, a sun step bakes again, Terrain.CastShadows off publishes nothing (the receivers
// read no terrain term and stay lit), and an edit's rect bakes again. The trigger does not depend on CBT.Update,
// which is skipped at rest.
TEST(RenderPipelineDeclareTests, TerrainShadowBakesOnlyWhenTheSunOrTheHeightsChange)
{
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        const ViewId v = f.AddView("TerrainShadowView", 1u);
        if (!ActivateCbtTerrain(f, terrain))
        {
            f.Down();
            GTEST_SKIP() << "staged shader sources or CBT kernels unavailable";
        }
        TextureHandle heights{};
        const uint32 heightIndex = UploadRidgeHeightTexture(f, heights);
        if (heightIndex == 0u)
        {
            f.Down();
            GTEST_SKIP() << "the device has no bindless textures";
        }
        SetShadowTestTerrain(terrain, heights, heightIndex, true);
        const RenderPipelineBlueprint bp = CompileTestBlueprint(f, R"json({
          "schemaVersion": 2, "pipelineName": "TerrainShadowTest",
          "passes": [
            { "id": "Upload", "type": "TerrainUpload" },
            { "id": "Terrain", "type": "CBTRender" },
            { "id": "Prepass", "type": "DepthPrepass", "clearDepthValue": 0.0 },
            { "id": "World", "type": "WorldRender" }
          ]
        })json", "terrain_shadow.rendergraph");
        EXPECT_FALSE(bp.HasErrors());
        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);
        FramePools pools(f.Device.get());

        // EXPECT (not ASSERT) from here: an early return would skip f.Down() and leak the terrain service
        // singleton into the next fixture in this process.
        uint32 frameIndex = 0u;
        TerrainShadowFrame first{};
        for (; frameIndex < 16u && !first.Baked; ++frameIndex)
            first = DeclareTerrainShadowFrame(f, pools, instance, v, terrain, frameIndex, 15.0f, true);
        EXPECT_TRUE(first.Baked) << "the first frame with a terrain and a sun bakes the map";
        EXPECT_TRUE(first.Published);
        EXPECT_LT(first.BakeOrder, first.WorldOrder) << "the bake runs before the world pass samples the map";

        // The GPU kernel's own result equals a brute-force maximum over the casters upwind (the kernel's runs,
        // their combination and the walk all reach it); at 15 degrees the shadow spans one or two runs.
        ExpectBakedRidgeMatchesReference(first, 15.0f, 20u);

        for (uint32 i = 0; i < 3u; ++i, ++frameIndex)
        {
            const TerrainShadowFrame rest = DeclareTerrainShadowFrame(f, pools, instance, v, terrain, frameIndex, 15.0f);
            EXPECT_FALSE(rest.Baked) << "the same sun and heights declare no bake (frame " << i << ")";
            EXPECT_TRUE(rest.Published) << "the map stays published at rest";
        }

        // A sun step bakes again. At 5 degrees the ridge's shadow runs about 46 samples downwind, across
        // several of the kernel's 16-sample runs, so every run must take the casters of all runs upwind.
        const TerrainShadowFrame step = DeclareTerrainShadowFrame(f, pools, instance, v, terrain, frameIndex++, 5.0f, true);
        EXPECT_TRUE(step.Baked) << "a sun step bakes the map again";
        ExpectBakedRidgeMatchesReference(step, 5.0f, 0u);

        SetShadowTestTerrain(terrain, heights, heightIndex, false);
        const TerrainShadowFrame off = DeclareTerrainShadowFrame(f, pools, instance, v, terrain, frameIndex++, 5.0f);
        EXPECT_FALSE(off.Baked);
        EXPECT_FALSE(off.Published) << "Terrain.CastShadows off publishes no map, so every receiver stays lit";

        SetShadowTestTerrain(terrain, heights, heightIndex, true);
        const TerrainShadowFrame back = DeclareTerrainShadowFrame(f, pools, instance, v, terrain, frameIndex++, 5.0f);
        EXPECT_FALSE(back.Baked) << "the map baked for this sun is still current";
        EXPECT_TRUE(back.Published);

        CBTTerrain::CBTClassifyDesc edit{};
        edit.DirtyMinU = 0.40f;
        edit.DirtyMinV = 0.40f;
        edit.DirtyMaxU = 0.45f;
        edit.DirtyMaxV = 0.45f;
        auto& cbt = f.Rs->EnsureFeature<CBTTerrainECS::CBTRenderFeature>();
        cbt.SetActive(true, edit, 8.0f, Components::kNoTerrainSeaLevel, CBTTerrainECS::CBTRenderFeature::DomainConfig{},
                      0u);
        const TerrainShadowFrame edited = DeclareTerrainShadowFrame(f, pools, instance, v, terrain, frameIndex++, 5.0f);
        EXPECT_TRUE(edited.Baked) << "an edit bakes the lines crossing its rect";

        f.Device->DestroyTexture(heights);
    }
    f.Down();
}

// A pipeline may declare ShadowMap before the terrain: the map published later in the frame's declaration
// still reaches that frame's ShadowData, so the terrain's shadow does not depend on the node order.
TEST(RenderPipelineDeclareTests, TerrainShadowReachesShadowDataWhicheverNodeDeclaresFirst)
{
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    {
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        const ViewId v = f.AddView("TerrainShadowOrderView", 1u);
        if (!ActivateCbtTerrain(f, terrain))
        {
            f.Down();
            GTEST_SKIP() << "staged shader sources or CBT kernels unavailable";
        }
        TextureHandle heights{};
        const uint32 heightIndex = UploadRidgeHeightTexture(f, heights);
        if (heightIndex == 0u)
        {
            f.Down();
            GTEST_SKIP() << "the device has no bindless textures";
        }
        SetShadowTestTerrain(terrain, heights, heightIndex, true);
        EXPECT_TRUE(f.Registry.Register("ShadowMap", [] { return std::make_unique<Nodes::ShadowMapNode>(); }, true));
        for (const bool shadowMapFirst : {true, false})
        {
            const RenderPipelineBlueprint bp = CompileTestBlueprint(f, shadowMapFirst ? R"json({
              "schemaVersion": 2, "pipelineName": "TerrainShadowOrderShadowMapFirst",
              "passes": [
                { "id": "Upload", "type": "TerrainUpload" },
                { "id": "CSM", "type": "ShadowMap", "buffer": "ShadowData" },
                { "id": "Terrain", "type": "CBTRender" },
                { "id": "Prepass", "type": "DepthPrepass", "clearDepthValue": 0.0 },
                { "id": "World", "type": "WorldRender" }
              ]
            })json" : R"json({
              "schemaVersion": 2, "pipelineName": "TerrainShadowOrderTerrainFirst",
              "passes": [
                { "id": "Upload", "type": "TerrainUpload" },
                { "id": "Terrain", "type": "CBTRender" },
                { "id": "CSM", "type": "ShadowMap", "buffer": "ShadowData" },
                { "id": "Prepass", "type": "DepthPrepass", "clearDepthValue": 0.0 },
                { "id": "World", "type": "WorldRender" }
              ]
            })json", shadowMapFirst ? "terrain_shadow_csm_first.rendergraph" : "terrain_shadow_terrain_first.rendergraph");
            EXPECT_FALSE(bp.HasErrors());
            size_t csmAt = SIZE_MAX, terrainAt = SIZE_MAX;
            for (size_t i = 0; i < bp.passes.size(); ++i)
            {
                csmAt = bp.passes[i].type == "ShadowMap" ? i : csmAt;
                terrainAt = bp.passes[i].type == "CBTRender" ? i : terrainAt;
            }
            EXPECT_EQ(csmAt < terrainAt, shadowMapFirst) << "the compiled order is the one under test";
            RenderPipelineInstance instance(*f.Rs, f.Registry);
            instance.SetBlueprint(bp);
            FramePools pools(f.Device.get());
            TerrainShadowFrame frame{};
            for (uint32 i = 0; i < 16u && !frame.Published; ++i)
                frame = DeclareTerrainShadowFrame(f, pools, instance, v, terrain, i, 15.0f);
            EXPECT_TRUE(frame.Published) << (shadowMapFirst ? "ShadowMap first" : "CBTRender first");
            EXPECT_EQ(frame.ShadowDataTerrainPresence, 1.0f)
                << "the view's ShadowData carries the terrain's map (" << (shadowMapFirst ? "ShadowMap first" : "CBTRender first") << ")";
        }
        f.Device->DestroyTexture(heights);
    }
    f.Down();
}

// An in-place device rebuild frees the clearance map, its bindless slot, the bake's pipeline and sampler, and
// the terrain's height texture. The bake forgets them (CBTRenderFeature::OnDeviceRebuilt, through the
// RenderServices feature sweep the rebuild runs), so on the rebuilt device the map is created again, bakes
// in full with the same sun and heights, matches the brute-force reference and reaches ShadowData, and no
// write to the bake's descriptor set names a handle of the old device. The test re-uploads the heights
// itself after the rebuild, in place of the terrain's missing rebuild hook. The check is scoped to that set:
// TerrainECS::TerrainRenderFeature has no rebuild hook, so the terrain surface's material-table binding
// drops its write on the rebuilt device independently of the shadow.
TEST(RenderPipelineDeclareTests, TerrainShadowRebakesOnTheRebuiltDevice)
{
    const ScopedForcedDeviceLoss loss;
    TerrainFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    if (!f.Device->IsDescriptorBufferEnabled())
    {
        f.Down();
        GTEST_SKIP() << "dropped descriptor writes are reported only on the descriptor-buffer path";
    }
    {
        auto& terrain = f.Rs->EnsureFeature<TerrainECS::TerrainRenderFeature>();
        const ViewId v = f.AddView("TerrainShadowRebuildView", 1u);
        if (!ActivateCbtTerrain(f, terrain))
        {
            f.Down();
            GTEST_SKIP() << "staged shader sources or CBT kernels unavailable";
        }
        TextureHandle heights{};
        uint32 heightIndex = UploadRidgeHeightTexture(f, heights);
        if (heightIndex == 0u)
        {
            f.Down();
            GTEST_SKIP() << "the device has no bindless textures";
        }
        SetShadowTestTerrain(terrain, heights, heightIndex, true);
        EXPECT_TRUE(f.Registry.Register("ShadowMap", [] { return std::make_unique<Nodes::ShadowMapNode>(); }, true));
        const RenderPipelineBlueprint bp = CompileTestBlueprint(f, R"json({
          "schemaVersion": 2, "pipelineName": "TerrainShadowRebuild",
          "passes": [
            { "id": "Upload", "type": "TerrainUpload" },
            { "id": "Terrain", "type": "CBTRender" },
            { "id": "CSM", "type": "ShadowMap", "buffer": "ShadowData" },
            { "id": "Prepass", "type": "DepthPrepass", "clearDepthValue": 0.0 },
            { "id": "World", "type": "WorldRender" }
          ]
        })json", "terrain_shadow_rebuild.rendergraph");
        EXPECT_FALSE(bp.HasErrors());
        RenderPipelineInstance instance(*f.Rs, f.Registry);
        instance.SetBlueprint(bp);

        // EXPECT (not ASSERT) throughout: an early return would skip f.Down() and leak the terrain service
        // singleton into the next fixture in this process.
        uint32 frameIndex = 0u;
        {
            FramePools pools(f.Device.get());
            TerrainShadowFrame before{};
            for (; frameIndex < 16u && !before.Baked; ++frameIndex)
                before = DeclareTerrainShadowFrame(f, pools, instance, v, terrain, frameIndex, 15.0f, true);
            EXPECT_TRUE(before.Baked) << "the map bakes on the first device";
            EXPECT_EQ(f.Device->GetDeviceRebuildGeneration(), 0u) << "the loss fired before the first bake";
        }

        const bool rebuilt = RunUntilDeviceRebuilt(*f.Device);
        EXPECT_TRUE(rebuilt) << "injected loss should rebuild the device in place";
        if (rebuilt)
        {
            // The height texture died with the old device. Production does not re-upload it: the terrain has
            // no rebuild hook (#2867). The test re-uploads the heights itself in that hook's place, so it
            // proves the bake's own recovery, not the live path.
            heightIndex = UploadRidgeHeightTexture(f, heights);
            EXPECT_NE(heightIndex, 0u);
            SetShadowTestTerrain(terrain, heights, heightIndex, true);

            FramePools pools(f.Device.get());
            std::vector<std::string> errors;
            TerrainShadowFrame after{};
            {
                GameEngine::TestLog::ScopedEngineLogCapture capture(&errors, Logger::LogLevel::Error);
                Logger::Log::Error(kRebuildCaptureSentinel);
                const uint32 firstAfter = frameIndex;
                for (; frameIndex < firstAfter + 16u && !after.Baked; ++frameIndex)
                    after = DeclareTerrainShadowFrame(f, pools, instance, v, terrain, frameIndex, 15.0f, true);
                Logger::Log::Flush();
            }
            EXPECT_TRUE(after.Baked) << "the same sun and heights bake again on the rebuilt device";
            EXPECT_TRUE(after.Published);
            EXPECT_EQ(after.ShadowDataTerrainPresence, 1.0f) << "the rebuilt device's ShadowData carries the map";
            ExpectBakedRidgeMatchesReference(after, 15.0f, 20u);
            EXPECT_EQ(std::count_if(errors.begin(), errors.end(),
                                    [](const std::string& line)
                                    { return line.find(kRebuildCaptureSentinel) != std::string::npos; }),
                      1) << "the error capture was not live";
            for (const std::string& line : errors)
                EXPECT_EQ(line.find("set='TerrainShadow.Bake.Set0'"), std::string::npos) << line;
        }
        f.Device->DestroyTexture(heights);
    }
    f.Down();
}

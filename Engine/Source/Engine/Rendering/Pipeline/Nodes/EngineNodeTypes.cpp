#include "Engine/Rendering/Pipeline/Nodes/EngineNodeTypes.h"

#include "Engine/Rendering/Pipeline/Nodes/AONode.h"
#include "Engine/Rendering/Pipeline/Nodes/AutoExposureNode.h"
#include "Engine/Rendering/Pipeline/Nodes/ColorGradeParamsUploadNode.h"
#include "Engine/Rendering/Pipeline/Nodes/ComputeShaderNode.h"
#include "Engine/Rendering/Pipeline/Nodes/CpuTextureInputNode.h"
#include "Engine/Rendering/Pipeline/Nodes/DDGINode.h"
#include "Engine/Rendering/Pipeline/Nodes/DepthPrepassNode.h"
#include "Engine/Rendering/Pipeline/Nodes/DepthResolveNode.h"
#include "Engine/Rendering/Pipeline/Nodes/FidelityFXDofNode.h"
#include "Engine/Rendering/Pipeline/Nodes/FullscreenShaderNode.h"
#include "Engine/Rendering/Pipeline/Nodes/HeightFogParamsUploadNode.h"
#include "Engine/Rendering/Pipeline/Nodes/HZBBuildNode.h"
#include "Engine/Rendering/Pipeline/Nodes/IBLGenNode.h"
#include "Engine/Rendering/Pipeline/Nodes/LensFlareRenderNode.h"
#include "Engine/Rendering/Pipeline/Nodes/LightUploadNode.h"
#include "Engine/Rendering/Pipeline/Nodes/OcclusionCullP2Node.h"
#include "Engine/Rendering/Pipeline/Nodes/RenderScaleUpscaleNode.h"
#include "Engine/Rendering/Pipeline/Nodes/ScatterBNode.h"
#include "Engine/Rendering/Pipeline/Nodes/ScreenSpaceReflectionsNode.h"
#include "Engine/Rendering/Pipeline/Nodes/ShadowMapNode.h"
#include "Engine/Rendering/Pipeline/Nodes/SkyRenderNode.h"
#include "Engine/Rendering/Pipeline/Nodes/SmaaNode.h"
#include "Engine/Rendering/Pipeline/Nodes/SortedTransparentNode.h"
#include "Engine/Rendering/Pipeline/Nodes/SunGlareRenderNode.h"
#include "Engine/Rendering/Pipeline/Nodes/TemporalAANode.h"
#include "Engine/Rendering/Pipeline/Nodes/TemporalFxaaNode.h"
#include "Engine/Rendering/Pipeline/Nodes/TransmissivePassNode.h"
#include "Engine/Rendering/Pipeline/Nodes/ViewParamsUploadNode.h"
#include "Engine/Rendering/Pipeline/Nodes/VolumetricCloudsParamsUploadNode.h"
#include "Engine/Rendering/Pipeline/Nodes/VolumetricFogNode.h"
#include "Engine/Rendering/Pipeline/Nodes/WorldRenderNode.h"
#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

#include <memory>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
namespace
{
RenderPipelineNodeResourceFields FullscreenShaderResourceFields()
{
    return {.RefKeys = {"output"},
            .RefMapKeys = {"inputs", "buffers"},
            .InputKey = "passthroughInput",
            .PublishKeys = {{"output", Names::View::Resolve}}};
}

// An empty "buffer" ref still publishes ShadowData at runtime, so the default
// keeps that name a valid target.
RenderPipelineNodeResourceFields ShadowMapResourceFields()
{
    return {.RefKeys = {"buffer"}, .PublishKeys = {{"buffer", Names::Res::ShadowData}}};
}
} // namespace

void RegisterEngineNodeTypes(RenderPipelineNodeRegistry& registry)
{
    (void)registry.Register(
        "WorldRender",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<WorldRenderNode>(); },
        /*perView*/ true,
        RenderPipelineNodeResourceFields{.StaticPublishNames = {Names::View::EffectiveColor}});

    (void)registry.Register(
        "TransmissiveRender",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<TransmissivePassNode>(); },
        /*perView*/ true);

    (void)registry.Register(
        "DepthPrepass",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<DepthPrepassNode>(); },
        /*perView*/ true,
        RenderPipelineNodeResourceFields{.RefKeys = {"depth"}, .PublishKeys = {{"depth", Names::View::Depth}}});

    (void)registry.Register(
        "DepthResolve",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<DepthResolveNode>(); },
        /*perView*/ true,
        RenderPipelineNodeResourceFields{.RefKeys = {"output"}, .PublishKeys = {{"output", Names::View::DepthResolved}}});

    (void)registry.Register(
        "AmbientOcclusion",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<AONode>(); },
        /*perView*/ true,
        RenderPipelineNodeResourceFields{.RefKeys = {"output"}, .PublishKeys = {{"output", Names::View::GTAO}}});

    (void)registry.Register(
        "ScreenSpaceReflections",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<ScreenSpaceReflectionsNode>(); },
        /*perView*/ true,
        RenderPipelineNodeResourceFields{.RefKeys = {"input", "output"}, .PublishKeys = {{"output", "HDRSSSR"}}});

    (void)registry.Register(
        "FidelityFXDepthOfField",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<FidelityFXDofNode>(); },
        /*perView*/ true);

    (void)registry.Register(
        "AutoExposure",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<AutoExposureNode>(); },
        /*perView*/ true,
        RenderPipelineNodeResourceFields{.RefKeys = {"input"}, .StaticPublishNames = {Names::Res::ExposureHistory}});

    // TemporalAA, TemporalFxaa, Smaa and RenderScaleUpscale publish their output
    // name whether they run or not: the passthrough stitches the input through
    // under the same name.
    (void)registry.Register(
        "TemporalAA",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<TemporalAANode>(); },
        /*perView*/ true,
        RenderPipelineNodeResourceFields{.RefKeys = {"input", "output"}, .PublishKeys = {{"output", "HDRTemporalAA"}}});

    (void)registry.Register(
        "TemporalFxaa",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<TemporalFxaaNode>(); },
        /*perView*/ true,
        RenderPipelineNodeResourceFields{.RefKeys = {"input", "output"}, .PublishKeys = {{"output", "LDRFxaa"}}});

    (void)registry.Register(
        "Smaa",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<SmaaNode>(); },
        /*perView*/ true,
        RenderPipelineNodeResourceFields{.RefKeys = {"input", "output"}, .PublishKeys = {{"output", "LDRSmaa"}}});

    (void)registry.Register(
        "RenderScaleUpscale",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<RenderScaleUpscaleNode>(); },
        /*perView*/ true,
        RenderPipelineNodeResourceFields{.RefKeys = {"input", "output"}, .PublishKeys = {{"output", "HDRUpscaled"}}});

    (void)registry.Register(
        "LightUpload",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<LightUploadNode>(); },
        /*perView*/ true,
        RenderPipelineNodeResourceFields{.RefKeys = {"buffer"}, .PublishKeys = {{"buffer", Names::Res::LightBuffer}}});

    (void)registry.Register(
        "ViewParamsUpload",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<ViewParamsUploadNode>(); },
        /*perView*/ true,
        RenderPipelineNodeResourceFields{.RefKeys = {"buffer"}, .PublishKeys = {{"buffer", Names::Res::ViewParams}}});

    (void)registry.Register(
        "HeightFogParamsUpload",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<HeightFogParamsUploadNode>(); },
        /*perView*/ true,
        RenderPipelineNodeResourceFields{.RefKeys = {"buffer"}, .PublishKeys = {{"buffer", Names::Res::HeightFogParams}}});

    (void)registry.Register(
        "CpuTextureInput",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<CpuTextureInputNode>(); },
        /*perView*/ true,
        RenderPipelineNodeResourceFields{.RefKeys = {"output", "parameters"}, .PublishKeys = {{"output", ""}, {"parameters", ""}}});

    (void)registry.Register(
        "VolumetricCloudsParamsUpload",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<VolumetricCloudsParamsUploadNode>(); },
        /*perView*/ true,
        RenderPipelineNodeResourceFields{.RefKeys = {"buffer"}, .PublishKeys = {{"buffer", Names::Res::CloudParams}}});

    (void)registry.Register(
        "ColorGradeParamsUpload",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<ColorGradeParamsUploadNode>(); },
        /*perView*/ true,
        RenderPipelineNodeResourceFields{.RefKeys = {"buffer"}, .PublishKeys = {{"buffer", Names::Res::ColorGradeParams}}});

    (void)registry.Register(
        "FullscreenShader",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<FullscreenShaderNode>(); },
        /*perView*/ true,
        FullscreenShaderResourceFields());

    (void)registry.Register(
        "TemporalFullscreenShader",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<TemporalFullscreenShaderNode>(); },
        /*perView*/ true,
        FullscreenShaderResourceFields());

    (void)registry.Register(
        "ComputeShader",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<ComputeShaderNode>(); },
        /*perView*/ true,
        RenderPipelineNodeResourceFields{.RefKeys = {"targetTexture", "targetBuffer"},
         .RefMapKeys = {"inputs", "buffers"},
         .PublishKeys = {{"targetTexture", ""}, {"targetBuffer", ""}}});

    (void)registry.Register(
        "SkyRender",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<SkyRenderNode>(); },
        /*perView*/ true,
        RenderPipelineNodeResourceFields{.RefKeys = {"output"}, .PublishKeys = {{"output", Names::View::Color}}});

    (void)registry.Register(
        "ShadowMap",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<ShadowMapNode>(); },
        /*perView*/ true,
        ShadowMapResourceFields());

    // Legacy alias: pre-Q4 projects serialize the type as "CascadedShadowMap".
    // Registered identically so those blueprints still compile; ValidateBlueprint
    // emits a deprecation warning nudging authors to the "ShadowMap" key.
    (void)registry.Register(
        "CascadedShadowMap",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<ShadowMapNode>(); },
        /*perView*/ true,
        ShadowMapResourceFields());

    // IBL bake (environment cubes + BRDF LUT) is view-independent, so it
    // declares once per frame at frame scope — perView=false.
    (void)registry.Register(
        "IBLGen",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<IBLGenNode>(); },
        /*perView*/ false);

    // DDGI probe field is world-space, view-independent — same reasoning
    // as IBLGen: declares once per frame at frame scope, perView=false.
    (void)registry.Register(
        "DDGIGen",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<DDGINode>(); },
        /*perView*/ false);

    (void)registry.Register(
        "LensFlare",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<LensFlareRenderNode>(); },
        /*perView*/ true,
        RenderPipelineNodeResourceFields{.RefKeys = {"output"}, .PublishKeys = {{"output", Names::View::Resolve}}});

    (void)registry.Register(
        "SunGlare",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<SunGlareRenderNode>(); },
        /*perView*/ true,
        RenderPipelineNodeResourceFields{.RefKeys = {"output"}, .PublishKeys = {{"output", Names::View::Resolve}}});

    (void)registry.Register(
        "VolumetricFog",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<VolumetricFogNode>(); },
        /*perView*/ true,
        RenderPipelineNodeResourceFields{.RefKeys = {"output"},
         .PublishKeys = {{"output", Names::View::Resolve}},
         .StaticPublishNames = {Names::View::DepthResolvedPostOcean}});

    // Two-phase HZB occlusion (R2.1) nodes, wired into the ForwardPlus
    // blueprints after WorldRender. HZBBuild builds the MIN/occlusion
    // pyramid for a view that opted in via HzbCullingStrategy, and the
    // MAX/nearest SSR pyramid for a view with SSSR active — each
    // independently, neither when a view needs neither. The phase-B
    // cull/scatter nodes stay inert without a slicePhase==1 reservation, so
    // probes and thumbnails still pay zero cost; scene panes and the Game
    // View DO opt in, so one frame commonly carries several HZB views
    // through the single node object this registration creates.
    (void)registry.Register(
        "HZBBuild",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<HZBBuildNode>(); },
        /*perView*/ true,
        RenderPipelineNodeResourceFields{.RefKeys = {"input", "output"}, .PublishKeys = {{"output", Names::View::HZB}}});

    (void)registry.Register(
        "OcclusionCullP2",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<OcclusionCullP2Node>(); },
        /*perView*/ true,
        RenderPipelineNodeResourceFields{.RefKeys = {"input"}});

    (void)registry.Register(
        "ScatterB",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<ScatterBNode>(); },
        /*perView*/ true);

    // Sorted transparent (order-dependent Blend) drain, relocated out of
    // WorldRenderNode so it runs after HZBBuild/OcclusionCullP2/ScatterB
    // (transparency-scale S1). Placed after ScatterB in the ForwardPlus
    // blueprints; inert for views whose sorted set did not engage.
    (void)registry.Register(
        "SortedTransparent",
        []() -> std::unique_ptr<IRenderPipelineNode>
        { return std::make_unique<SortedTransparentNode>(); },
        /*perView*/ true);
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes

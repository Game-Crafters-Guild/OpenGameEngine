#include "Engine/Rendering/Pipeline/Nodes/ShadowMapNode.h"

#include "Engine/Rendering/CameraUtils.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Engine/Rendering/Pipeline/Nodes/PipelineNodeUtils.h"
#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderServices.h"

#include "Components/Rendering/Light.h"
#include "Logger/Logger.h"
#include "Mathematics/Vector3.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <nlohmann/json.hpp>

#include <string_view>

namespace GameEngine::Engine::Renderer { using namespace ::GameEngine::Rendering; }

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

namespace
{
bool IsPreviewOrThumbnailViewName(const char* debugName)
{
    if (!debugName)
        return false;

    const std::string_view name(debugName);
    return name.find("Thumbnail") != std::string_view::npos ||
           name.find("Preview") != std::string_view::npos;
}
} // namespace

ShadowMapNode::~ShadowMapNode()
{
    if (m_OverlayDevice)
    {
        if (m_OverlaySampler.IsValid())
            m_OverlayDevice->DestroySampler(m_OverlaySampler);
    }
    m_OverlayMeta.reset();
}

void ShadowMapNode::EnsureOverlayResources(Rendering::IDevice* device)
{
    if (m_OverlayLoadAttempted)
        return; // Already attempted (succeeded or failed); don't retry.
    m_OverlayLoadAttempted = true;

    m_OverlayDevice = device;

    // Create a regular (non-comparison) sampler for reading depth values.
    if (!m_OverlaySampler.IsValid())
    {
        m_OverlaySampler = device->CreateSampler(
            Rendering::SamplerDesc::MaterialLinearClamp("ShadowDebugOverlay_Sampler"));
    }

    // Load the shadow debug overlay shaderpkg.
    if (m_OverlayVS.empty() || m_OverlayFS.empty())
    {
        Rendering::ShaderPackage pkg{};
        std::string loadErr;
        if (!Rendering::LoadShaderPkg("Shaders/shadow_debug_overlay.shaderpkg",
                                     device->PreferredShaderSource(), pkg, &loadErr))
        {
            LOG_WARNING("ShadowMapNode: failed to load shadow_debug_overlay.shaderpkg: {}", loadErr);
            return;
        }
        auto itVs = pkg.stageBytes.find("vs");
        auto itFs = pkg.stageBytes.find("fs");
        if (itVs == pkg.stageBytes.end() || itFs == pkg.stageBytes.end())
        {
            LOG_WARNING("ShadowMapNode: shadow_debug_overlay.shaderpkg missing vs/fs");
            return;
        }
        m_OverlayVS = std::move(itVs->second);
        m_OverlayFS = std::move(itFs->second);
        m_OverlayMeta = std::make_unique<Rendering::ShaderMeta>(std::move(pkg.meta));
    }

    // Build the typed graphics pipeline desc and intern eagerly.
    Rendering::GraphicsPipelineDesc gd{};
    gd.Kind = Rendering::GraphicsPipelineKind::VertexFragment;
    gd.VertexShader = std::make_shared<const std::vector<uint8_t>>(m_OverlayVS);
    gd.PixelShader  = std::make_shared<const std::vector<uint8_t>>(m_OverlayFS);
    gd.DebugName = "ShadowDebugOverlay";
    gd.Rasterization.cullMode = Rendering::CullModeFlagBits::None;
    gd.DepthStencil.depthTestEnable = false;
    gd.DepthStencil.depthWriteEnable = false;
    Rendering::DynamicStateInfo dyn{};
    dyn.states = {Rendering::DynamicState::Viewport, Rendering::DynamicState::Scissor};
    gd.DynamicState = dyn;
    {
        Rendering::ColorBlendAttachmentState blend{};
        blend.blendEnable = true;
        blend.srcColorBlendFactor = Rendering::BlendFactor::SrcAlpha;
        blend.dstColorBlendFactor = Rendering::BlendFactor::OneMinusSrcAlpha;
        blend.srcAlphaBlendFactor = Rendering::BlendFactor::SrcAlpha;
        blend.dstAlphaBlendFactor = Rendering::BlendFactor::OneMinusSrcAlpha;
        gd.ColorBlend.attachments = {blend};
    }

    if (m_OverlayMeta)
    {
        // Snapshot set-0 layout for execute-time descriptor allocation.
        m_OverlaySet0Layout = Rendering::DescriptorSetLayoutDesc{};
        auto patchLayout = [this](uint32_t setIndex, Rendering::DescriptorSetLayoutDesc& dsl) {
            if (setIndex == 0)
                m_OverlaySet0Layout = dsl;
        };

        std::string err;
        Rendering::MaterialHelper::ApplyShaderMetaToGraphicsDesc(
            *device, *m_OverlayMeta, gd,
            Rendering::MaterialBuilder::MergeMode::Auto,
            {true, 128}, patchLayout, &err);
    }

    m_OverlayPipelineId = device->InternGraphicsPipeline(std::move(gd));
}

void ShadowMapNode::EnsureMsmResources(Rendering::IDevice* device)
{
    if (m_MsmLoadAttempted)
        return;
    m_MsmLoadAttempted = true;

    // Linear-clamp sampler for the cascade-depth-array reads inside
    // msm_write.frag. Point-sampling there would be more correct (we use
    // texelFetch in the shader, which ignores the sampler), but a valid
    // sampler must still be bound to the descriptor.
    if (!m_MsmDepthSampler.IsValid())
    {
        m_MsmDepthSampler = device->CreateSampler(
            Rendering::SamplerDesc::PointClamp("MsmDepthPointClamp"));
    }

    // Helper: load one shaderpkg + extract VS/FS bytes + meta.
    const ShaderSourceKind sourceKind = device->PreferredShaderSource();
    auto loadGfx = [sourceKind](const char* path,
                      std::vector<uint8_t>& outVs,
                      std::vector<uint8_t>& outFs,
                      std::unique_ptr<Rendering::ShaderMeta>& outMeta) -> bool
    {
        Rendering::ShaderPackage pkg{};
        std::string err;
        if (!Rendering::LoadShaderPkg(path, sourceKind, pkg, &err))
        {
            LOG_WARNING("ShadowMapNode: failed to load {}: {}", path, err);
            return false;
        }
        auto itVs = pkg.stageBytes.find("vs");
        auto itFs = pkg.stageBytes.find("fs");
        if (itVs == pkg.stageBytes.end() || itFs == pkg.stageBytes.end())
        {
            LOG_WARNING("ShadowMapNode: {} missing vs/fs", path);
            return false;
        }
        outVs = std::move(itVs->second);
        outFs = std::move(itFs->second);
        outMeta = std::make_unique<Rendering::ShaderMeta>(std::move(pkg.meta));
        return true;
    };

    // The three pipelines all share fullscreen_noinput.vert as the VS;
    // the shaderpkg compiler bundles the configured VS into each pkg.
    if (m_MsmWriteVS.empty()
        && !loadGfx("Shaders/msm_write.shaderpkg", m_MsmWriteVS, m_MsmWriteFS, m_MsmWriteMeta))
        return;
    if (m_MsmBlurVS.empty()
        && !loadGfx("Shaders/msm_blur_h.shaderpkg", m_MsmBlurVS, m_MsmBlurHFS, m_MsmBlurHMeta))
        return;
    {
        // BlurV uses the same VS bytes; only its FS differs from BlurH. The
        // helper still loads VS each time, but storing in m_MsmBlurVS once
        // keeps the descriptors below consistent.
        std::vector<uint8_t> dummyVs;
        std::unique_ptr<Rendering::ShaderMeta> meta;
        if (m_MsmBlurVFS.empty()
            && !loadGfx("Shaders/msm_blur_v.shaderpkg", dummyVs, m_MsmBlurVFS, meta))
            return;
        m_MsmBlurVMeta = std::move(meta);
    }

    // Build a typed fullscreen graphics pipeline desc, apply meta, intern
    // eagerly, and stash set-0 for execute-time descriptor allocation.
    auto buildFullscreenId = [&](const std::vector<uint8_t>& vs,
                                  const std::vector<uint8_t>& fs,
                                  Rendering::ShaderMeta* meta,
                                  const char* dbgName,
                                  Rendering::DescriptorSetLayoutDesc& outSet0Layout) -> Rendering::GraphicsPipelineId
    {
        Rendering::GraphicsPipelineDesc gd{};
        gd.Kind = Rendering::GraphicsPipelineKind::VertexFragment;
        gd.VertexShader = std::make_shared<const std::vector<uint8_t>>(vs);
        gd.PixelShader  = std::make_shared<const std::vector<uint8_t>>(fs);
        gd.DebugName = dbgName;
        gd.Rasterization.cullMode = Rendering::CullModeFlagBits::None;
        gd.DepthStencil.depthTestEnable = false;
        gd.DepthStencil.depthWriteEnable = false;
        Rendering::DynamicStateInfo dyn{};
        dyn.states = {Rendering::DynamicState::Viewport, Rendering::DynamicState::Scissor};
        gd.DynamicState = dyn;

        outSet0Layout = Rendering::DescriptorSetLayoutDesc{};
        if (meta)
        {
            auto patchLayout = [&outSet0Layout](uint32_t setIndex, Rendering::DescriptorSetLayoutDesc& dsl) {
                if (setIndex == 0)
                    outSet0Layout = dsl;
            };
            std::string err;
            Rendering::MaterialHelper::ApplyShaderMetaToGraphicsDesc(
                *device, *meta, gd,
                Rendering::MaterialBuilder::MergeMode::Auto,
                {true, 128}, patchLayout, &err);
        }
        return device->InternGraphicsPipeline(std::move(gd));
    };

    m_MsmWritePipelineId = buildFullscreenId(m_MsmWriteVS, m_MsmWriteFS, m_MsmWriteMeta.get(), "MsmWrite", m_MsmWriteSet0Layout);
    m_MsmBlurHPipelineId = buildFullscreenId(m_MsmBlurVS, m_MsmBlurHFS, m_MsmBlurHMeta.get(), "MsmBlurH", m_MsmBlurHSet0Layout);
    m_MsmBlurVPipelineId = buildFullscreenId(m_MsmBlurVS, m_MsmBlurVFS, m_MsmBlurVMeta.get(), "MsmBlurV", m_MsmBlurVSet0Layout);

    // Fused compute pipeline (msm_cascade.comp). Optional: every shipping
    // device takes this compute path; if the package fails to load (older
    // build / partial hot-reload) the node falls back to PCSS for MSM4 frames.
    {
        Rendering::ShaderPackage pkg{};
        std::string err;
        if (Rendering::LoadShaderPkg("Shaders/msm_cascade.shaderpkg", device->PreferredShaderSource(), pkg, &err))
        {
            auto itCs = pkg.stageBytes.find("cs");
            if (itCs != pkg.stageBytes.end() && !itCs->second.empty())
            {
                m_MsmCascadeComputeMeta = std::make_unique<Rendering::ShaderMeta>(std::move(pkg.meta));

                Rendering::ComputePipelineDesc cd{};
                cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(itCs->second));
                cd.DebugName = "MsmCascade_Compute";

                m_MsmCascadeComputeSet0Layout = Rendering::DescriptorSetLayoutDesc{};
                if (m_MsmCascadeComputeMeta)
                {
                    auto patchLayout = [this](uint32_t setIndex, Rendering::DescriptorSetLayoutDesc& dsl) {
                        if (setIndex == 0)
                            m_MsmCascadeComputeSet0Layout = dsl;
                    };
                    std::string applyErr;
                    Rendering::MaterialHelper::ApplyShaderMetaToComputeDesc(
                        *device, *m_MsmCascadeComputeMeta, cd,
                        Rendering::MaterialBuilder::MergeMode::Auto,
                        {true, 128}, patchLayout, &applyErr);
                }
                m_MsmCascadeComputePipelineId = device->InternComputePipeline(std::move(cd));
            }
            else
            {
                LOG_WARNING("ShadowMapNode: msm_cascade.shaderpkg missing cs stage");
            }
        }
        else
        {
            LOG_WARNING("ShadowMapNode: failed to load msm_cascade.shaderpkg: {}", err);
        }
    }
}

bool ShadowMapNode::Initialize(std::string nodeId, std::string nodeJson, std::string* outError)
{
    m_Id = std::move(nodeId);
    m_Json = std::move(nodeJson);

    try
    {
        auto j = nlohmann::json::parse(m_Json);
        if (!j.is_object())
            return true;

        if (j.contains("cascades") && j["cascades"].is_number_unsigned())
            m_NumCascades = std::min(j["cascades"].get<uint32_t>(), kMaxShadowCascades);
        if (j.contains("resolution") && j["resolution"].is_number_unsigned())
            m_Resolution = j["resolution"].get<uint32_t>();
        if (j.contains("punctualResolution") && j["punctualResolution"].is_number_unsigned())
            m_PunctualResolution = j["punctualResolution"].get<uint32_t>();
        if (j.contains("pointShadowBudget") && j["pointShadowBudget"].is_number_unsigned())
            m_PointShadowBudget = j["pointShadowBudget"].get<uint32_t>();
        if (j.contains("momentsResolution") && j["momentsResolution"].is_number_unsigned())
        {
            const uint32_t r = j["momentsResolution"].get<uint32_t>();
            // Snap to the supported {512, 1024, 2048} set so the inspector
            // dropdown and the JSON config can't disagree on legal values.
            m_MomentsResolution = (r <= 768)  ? 512u
                                : (r <= 1536) ? 1024u
                                              : 2048u;
        }
        if (j.contains("splitLambda") && j["splitLambda"].is_number())
            m_SplitLambda = j["splitLambda"].get<float>();
        if (j.contains("maxShadowDistance") && j["maxShadowDistance"].is_number())
            m_MaxShadowDistance = j["maxShadowDistance"].get<float>();
        if (j.contains("fitShadowDistanceToScene") && j["fitShadowDistanceToScene"].is_boolean())
            m_FitShadowDistanceToScene = j["fitShadowDistanceToScene"].get<bool>();
        if (j.contains("depthBias") && j["depthBias"].is_number())
            m_DepthBias = j["depthBias"].get<float>();
        if (j.contains("normalBias") && j["normalBias"].is_number())
            m_NormalBias = j["normalBias"].get<float>();
        if (j.contains("buffer") && j["buffer"].is_string())
            m_BufferRef = j["buffer"].get<std::string>();
    }
    catch (const std::exception& e)
    {
        if (outError)
            *outError = std::string("ShadowMapNode JSON parse failed: ") + e.what();
        return false;
    }

    return true;
}

void ShadowMapNode::Declare(RenderPipelineInstance& /*instance*/,
                                    const PipelineDeclareContext& ctx)
{
    // Feature init + the SDSM reduce shader (slice 5). The MSM moments chain
    // and the debug overlay are still the remaining slice-5 halves — MSM4
    // silently renders as PCSS in RenderGraph frames until they land.
    auto* device = ctx.Services.GetDevice();
    if (!device)
        return;

    auto& feature = ctx.Services.EnsureFeature<ShadowMapRenderFeature>();
    CascadedShadowConfig config{};
    config.NumCascades = m_NumCascades;
    // Editor project Rendering Settings can override the pipeline-authored
    // tier. Standalone runtimes leave the feature override empty and keep the
    // asset. The override belongs to the feature that owns the cascade maps.
    config.Resolution = feature.GetProjectResolutionOverride().value_or(m_Resolution);
    config.MomentsResolution = m_MomentsResolution;
    config.SplitLambda = m_SplitLambda;
    config.MaxShadowDistance = m_MaxShadowDistance;
    config.DepthBias = m_DepthBias;
    config.NormalBias = m_NormalBias;
    // Preserve what the project settings already applied. Initialize replaces
    // m_Config wholesale, so a field the node does not carry is silently reset
    // to the struct default — which made rendering.directionalShadowProjection
    // a no-op: the setting applied, then the first Declare overwrote it.
    config.Projection = feature.GetShadowProjection();

    if (!feature.IsInitialized() && !feature.Initialize(device, config))
        LOG_ERROR("ShadowMapNode: failed to initialize ShadowMapRenderFeature");
    else if (feature.IsInitialized())
        feature.SetResolution(config.Resolution);

    EnsureMsmResources(device);
    EnsureOverlayResources(device);
}

void ShadowMapNode::DeclareForView(ViewDeclare& d)
{
    auto* rs = &d.Services;
    const Rendering::ViewId viewId = d.View.id;
    auto* feature = rs->GetFeature<ShadowMapRenderFeature>();
    if (!feature || !feature->IsInitialized())
        return;

    // M1: the node owns the point-shadow atlas budget config. Applied once per
    // declare; a change takes effect next frame (this frame's cull scheduling,
    // which ran earlier, already resolved the assignment for the current budget).
    rs->SetPointShadowBudget(m_PointShadowBudget);

    // ShadowData: ring alloc the world's binding table consumes — ALWAYS
    // published, zeroed by default (numCascades 0) so shadowed materials
    // read a valid UBO even with no light/camera (the old upload exec's
    // zero-write branches).
    auto sd = d.Frame.AllocUpload<ShadowDataGPU>();
    if (!sd.Valid())
        return;
    *sd.Ptr = ShadowDataGPU{};
    // A terrain map published after this node (CBTRender declared later) is written into this
    // upload by the feature, so the declare order does not decide whether the term arrives.
    feature->AttachShadowDataUpload(viewId, d.Frame.FrameIndex(), sd.Ptr);
    d.PublishBuffer(m_BufferRef.empty() ? std::string(Names::Res::ShadowData) : m_BufferRef,
                    {sd.Buffer, sd.Offset, sizeof(ShadowDataGPU), /*Graph*/ {}});

    // Cascades belong to THE primary directional (SelectPrimaryDirectional —
    // the same light WriteViewLightBuffer shades with), and only when that
    // light itself casts. Scanning for the first CASTER instead would render
    // cascades for a light the shading UBO is not using whenever a stronger
    // non-casting directional exists — shadows from a light nobody samples.
    const auto* viewDesc = rs->Views().FindViewDesc(viewId);
    const uint64_t worldId = viewDesc ? viewDesc->worldId : 0u;
    const auto lights = rs->GetWorldLights(worldId);
    const ExtractedLight* primary = SelectPrimaryDirectional(lights);
    const ExtractedLight* shadowLight =
        (primary && primary->castsShadows != 0) ? primary : nullptr;
    if (!shadowLight)
    {
        if (!IsPreviewOrThumbnailViewName(viewDesc ? viewDesc->debugName : nullptr) &&
            !feature->HasWarnedNoLight())
        {
            LOG_WARNING(
                "ShadowMapNode: no shadow-casting directional light found for view '{}'",
                viewDesc && viewDesc->debugName ? viewDesc->debugName : "<unnamed>");
            feature->SetWarnedNoLight(true);
        }
        // No shadow-casting directional light: declare the punctual families
        // only (no cascades, no tint) through the SAME feature->Declare entry —
        // a null DirectionalLightDirWS makes Declare skip cascade+tint and emit
        // area/spot/point, matching the pre-fold direct rs->Add* calls exactly.
        rs->EmitProducerDepthCommandsForView(viewId);
        FeatureDeclareContext fctx = rs->MakeFeatureDeclareContext(
            d.Frame, viewId, m_NumCascades, PunctualResolution(), /*directionalLightDirWS=*/nullptr,
            [&d](const char* suffix) { return d.PassName(suffix); });
        feature->Declare(d.Frame, *rs, fctx);
        return; // zeroed ShadowData already published
    }
    feature->SetWarnedNoLight(false);

    if (!viewDesc || viewDesc->cameraId == 0)
        return;
    const auto* camData = rs->Views().FindCameraData(viewDesc->cameraId);
    if (!camData)
        return;
    // Thumbnail cameras before their first update: inverting a zero viewProj
    // produces NaN that propagates everywhere.
    if (camData->proj[0] == 0.0f && camData->proj[5] == 0.0f)
        return;

    // This frame's fit: computed when the caster cull was scheduled, ahead of
    // this declaration, and reused here unless an input changed since; the
    // feature resolves the newest SDSM readback and applies the world's shadow
    // settings over this node's authored ones.
    const ShadowMapRenderFeature::DirectionalShadowSettings authored{
        m_MaxShadowDistance, m_SplitLambda, m_DepthBias, m_NormalBias, m_FitShadowDistanceToScene};
    const CascadeFrameData cascadeFrame =
        feature->FitViewCascades(*rs, viewId, worldId, *camData, *shadowLight, authored);
    rs->EmitProducerDepthCommandsForView(viewId);

    // SDSM DepthReduce — GRAPHICS queue deliberately: the frame driver's
    // SubmissionToken contract stamps the GRAPHICS timeline (GPUCulling.h), and
    // a compute-queue dispatch gated on a graphics token would resolve a slot
    // the compute queue may still be writing. Declared only with shadow casters
    // this frame, and declared LATE: it measures the view depth after every
    // surface has written it, which the depth prepass this node follows does
    // not hold (ShadowReceiverReduce).
    if (ShadowMapRenderFeature::SupportsSdsm(rs->GetDevice()) &&
        rs->ViewNeedsShadowCascadePasses(viewId))
    {
        float nearPlane = 0.0f;
        float farPlane = 0.0f;
        ExtractNearFarLH_ZO(camData->proj, nearPlane, farPlane);
        const Mathematics::Vector3 lightDir{shadowLight->directionWS[0], shadowLight->directionWS[1],
                                            shadowLight->directionWS[2]};
        m_ReceiverReduce.DeclareForView(d, *feature, *camData, nearPlane, farPlane,
                                        IsOrthographicProjectionLH_ZO(camData->proj),
                                        lightDir.Normalize(), cascadeFrame.MaxShadowDistance);
    }

    // All five shadow producer families now live on the feature that owns the
    // shadow textures (A1.1 S1 cascade + tint; S2 area/spot/point). The node
    // hands it a per-(feature, view) declaration snapshot (frame-validated spine
    // values + the WorldDeclared tripwire + the ShadowDeclareSeam + the resolved
    // punctual light infos) and Declare runs the cascade loop, the tint loop, then
    // the area/spot/point families in the SAME order, through the SAME
    // ViewDeclare::PassName forwarder — so pass names, recording order, and the
    // read/write sets are unchanged. The FIRST cascade's pooled adopt may still
    // invalidate the cache on a physical-change frame (intended one-frame skip);
    // nothing caches after Declare.
    FeatureDeclareContext fctx = rs->MakeFeatureDeclareContext(
        d.Frame, viewId, m_NumCascades, PunctualResolution(), shadowLight->directionWS,
        [&d](const char* suffix) { return d.PassName(suffix); });
    feature->Declare(d.Frame, *rs, fctx);

    // ShadowData content AFTER Declare: the cascade declares just committed
    // this frame's content fits (which layers rendered, which the motion
    // round-robin deferred), and BuildShadowDataGPU uploads the fit matching
    // each layer's RETAINED content — built earlier it would sample a
    // rendering cascade through last frame's fit. The published buffer ref
    // (sd, declared above) is position-independent: consumers bind
    // {buffer, offset}; this CPU write lands before RGFrame::Execute.
    ShadowDataGPU gpuData{};
    feature->BuildShadowDataGPU(cascadeFrame, viewId, *rs, d.Frame, m_RGFrameCounter++, gpuData);
    *sd.Ptr = gpuData;

    // MSM moments chain (5e): compute-preferred, declared only when MSM4 is
    // the live filter. PCSS frames keep the world arm's read-only import —
    // nothing writes, so its declared ShaderResource state stays truthful.
    // The old graph's sticky-import + world-pass-invalidation dance was made structural
    // now: imports dedup by physical handle (the node's import here and the
    // world arm's land on ONE resource id), a recreated physical is simply a
    // new import next frame, and the world's declared moments Read schedules
    // the General -> ShaderReadOnly transition the old comments agonized
    // over. The graphics 3-pass fallback is NOT ported: every shipping
    // device takes the compute path; PCSS covers the remainder until a
    // counterexample shows up.
    const ShadowFilterQuality requestedFilter = feature->ResolveRequestedFilterQuality(*rs, viewId);
    const bool msmActive = requestedFilter == ShadowFilterQuality::MSM4 &&
                           m_MsmCascadeComputePipelineId.IsValid();
    // EnsureMsmMomentsForView's not-MSM4 branch is what FREES a previously
    // allocated per-view moments array (32-128 MB) — call it when MSM is
    // active OR when the quality moved OFF MSM4, but never for
    // MSM4-with-no-usable-pipeline (headless/shader decline), which would
    // allocate an array with no producer.
    if (msmActive || requestedFilter != ShadowFilterQuality::MSM4)
    {
        bool momentsRecreated = false;
        feature->EnsureMsmMomentsForView(viewId, *rs, &momentsRecreated);
        (void)momentsRecreated; // structural under handle-dedup
    }
    if (msmActive)
    {
        const auto momentsTex = feature->GetMsmMomentsTexture(viewId);
        const RenderGraph::RGTexture depthArr = rs->GetShadowMapArrayRG(d.Frame, viewId);
        if (momentsTex.IsValid() && depthArr.IsValid())
        {
            const auto& cfgLive = feature->GetConfig();
            const std::string momentsName =
                "MsmMoments.View" + std::to_string(static_cast<uint32_t>(viewId));
            const RenderGraph::RGTexture moments = d.Frame.ImportExternalTexture(
                momentsName.c_str(), momentsTex, Rendering::ResourceState::ShaderResource,
                Rendering::TextureFormat::R16G16B16A16_UNORM, 1, cfgLive.NumCascades);
            if (moments.IsValid())
            {
                // Layout truthfulness: the import declares ShaderResource, so
                // the frame must END there even when no world pass samples the
                // moments this frame (empty view, keyword-less world) — the
                // storage writes are cull-exempt and would otherwise strand
                // written layers in General while next frame's import again
                // claims ShaderReadOnly. The export transition homogenizes;
                // on steady frames the world's read already did, and it
                // collapses to a no-op.
                d.Frame.MarkOutput(moments, RenderGraph::RGImageLayout::ShaderReadOnly);
                const uint32_t momentsRes = std::max(1u, cfgLive.MomentsResolution);
                const uint32_t depthRes = std::max(1u, cfgLive.Resolution);
                const int blurMode = static_cast<int>(feature->GetMsmBlurMode());
                for (uint32_t c = 0; c < cascadeFrame.NumCascades; ++c)
                {
                    char suffix[40];
                    snprintf(suffix, sizeof(suffix), "MsmCascade_Compute_%u", c);
                    // GRAPHICS queue deliberately (same rationale as the SDSM
                    // DepthReduce above): the export transition requires a
                    // graphics-owned final toucher, and async-compute overlap
                    // is moot under the production single-queue map. The
                    // depth read is stage-qualified — the consumer is the
                    // compute dispatch, not a fragment shader.
                    d.Frame.AddPass(
                        d.PassName(suffix).c_str(), Rendering::PassPhase::kDefault,
                        [&](RenderGraph::RGPassBuilder& p)
                        {
                            // This frame's cascade depth writes feed the
                            // moments resolve (RAW through the shared id).
                            p.Read(depthArr, RenderGraph::RGTextureRead::SampledCompute);
                            RenderGraph::RGRange layer = RenderGraph::RGRange::All();
                            layer.BaseLayer = c;
                            layer.LayerCount = 1;
                            p.Write(moments, RenderGraph::RGTextureWrite::Storage, layer);
                        },
                        [this, depthArr, moments, c, momentsRes, depthRes,
                         blurMode](RenderGraph::RGContext& ctx)
                        {
                            auto* device = ctx.GetDevice();
                            auto* cl = ctx.Cmd;
                            if (!device || !cl || !m_MsmCascadeComputePipelineId.IsValid())
                                return;
                            const auto momentsPhysical = ctx.GetTexture(moments);
                            const auto depthPhysical = ctx.GetTexture(depthArr);
                            if (!momentsPhysical.IsValid() || !depthPhysical.IsValid())
                                return;
                            const auto pipe =
                                ctx.GetOrCreatePipelineVariant(m_MsmCascadeComputePipelineId);
                            if (!pipe.IsValid())
                                return;
                            cl->SetPipeline(pipe);

                            struct CascadePC
                            {
                                int CascadeIdx;
                                int MomentsResolution;
                                int DepthResolution;
                                int BlurMode;
                            } pc{static_cast<int>(c), static_cast<int>(momentsRes),
                                 static_cast<int>(depthRes), blurMode};
                            cl->SetPushConstants(pc);

                            if (m_MsmCascadeComputeMeta &&
                                !m_MsmCascadeComputeSet0Layout.bindings.empty())
                            {
                                Rendering::DescriptorSetDesc ds0{};
                                ds0.layout = m_MsmCascadeComputeSet0Layout;
                                ds0.transient = true;
                                ds0.debugName = "MsmCascade.Set0";
                                auto set0 = device->CreateDescriptorSet(ds0);
                                Rendering::NamedDescriptorWriter wdesc(
                                    device, set0, *m_MsmCascadeComputeMeta, 0);
                                if (wdesc.Has("uDepth") && m_MsmDepthSampler.IsValid())
                                    wdesc.AddCombinedImageSampler("uDepth", depthPhysical,
                                                                  m_MsmDepthSampler);
                                wdesc.Flush();
                                // Moments write target (uMoments) — storage image,
                                // outside NamedDescriptorWriter's coverage; resolve
                                // its set0 index by reflected name.
                                Detail::BindStorageImageByName(device, set0,
                                                               *m_MsmCascadeComputeMeta,
                                                               "uMoments", momentsPhysical);
                                cl->BindDescriptorSet(0, set0, pipe);
                            }

                            const uint32_t groups = (momentsRes + 15u) / 16u;
                            cl->Dispatch(groups, groups, 1);
                        });
                }
            }
        }
    }

    // The comparison sampler is view-independent engine state; the cascade
    // texture is NOT published here. It is frame-local (the pool owns and can
    // free it), so the world pass reads it from this frame's import or from the
    // frame-stamped feature cache — a persistent copy in the registry would be
    // a second, unstamped door to a handle that outlives its image, and this
    // publish is not even reached on the paths that early-return above.
    rs->Views().SetViewShadowSampler(viewId, feature->GetShadowSampler());

    // Shadow-cascade thumbnails overlay (5f): a LATE declare. In-node the
    // overlay's write would order BEFORE the world/post-FX writes
    // (declaration order is the ordering; phase is only a tiebreak), so it
    // queues onto the instance and declares after the node loop, attaching
    // the chain's FINAL output. Deliberate change vs the old arm (which
    // composited onto the pre-post-FX view color): the thumbnails now draw
    // over the post-tonemap image the UI samples — gate-2 sign-off item.
    // Old machinery that dies here: m_OverlayPassNames, the mask-transition
    // DisablePass sweep (an inactive view simply doesn't declare), the
    // version-hash dedup, and the ShowThumbnails activation predicate
    // (evaluated here, at declaration).
    if (feature->GetShowThumbnails() && m_OverlayPipelineId.IsValid())
    {
        const RenderGraph::RGTexture overlayShadowArr = rs->GetShadowMapArrayRG(d.Frame, viewId);
        if (overlayShadowArr.IsValid())
        {
            const std::string overlayPassName = d.PassName("DebugOverlay");
            d.DeferDeclare(
                [this, viewId, overlayShadowArr, overlayPassName](
                    RenderGraph::RGFrame& frame, RenderPipelineInstance& inst)
                {
                    const RenderGraph::RGTexture target = inst.GetOutputRG(viewId, Names::Output::FinalColor);
                    if (!target.IsValid())
                        return;
                    const RenderGraph::RGResourceDesc td = frame.Graph().ResourceDesc(target.Id);
                    const uint32_t w = td.Width;
                    const uint32_t h = td.Height;
                    if (w == 0 || h == 0)
                        return;

                    frame.AddPass(
                        overlayPassName.c_str(), Rendering::PassPhase::kOverlay,
                        [&](RenderGraph::RGPassBuilder& p)
                        {
                            RenderGraph::RGAttachmentOps ops{};
                            ops.Load = RenderGraph::RGLoadOp::Load; // composite over the final image
                            ops.Store = RenderGraph::RGStoreOp::Store;
                            p.AttachColor(0, target, ops);
                            p.Read(overlayShadowArr, RenderGraph::RGTextureRead::Sampled);
                        },
                        [this, overlayShadowArr, w, h](RenderGraph::RGContext& ctx)
                        {
                            auto* cl = ctx.Cmd;
                            auto* dev = ctx.GetDevice();
                            if (!cl || !dev || !m_OverlayPipelineId.IsValid() || !m_OverlayMeta)
                                return;
                            const auto shadowTex = ctx.GetTexture(overlayShadowArr);
                            if (!shadowTex.IsValid())
                                return;
                            const auto pipe = ctx.GetOrCreatePipelineVariant(m_OverlayPipelineId);
                            if (!pipe.IsValid())
                                return;
                            cl->SetPipeline(pipe);

                            // Negative-height viewport: the overlay shader
                            // expects the old arm's y-flip (ported verbatim).
                            cl->SetViewport(0.0f, static_cast<float>(h), static_cast<float>(w),
                                            -static_cast<float>(h));
                            cl->SetScissor(0, 0, static_cast<int>(w), static_cast<int>(h));

                            if (!m_OverlaySet0Layout.bindings.empty())
                            {
                                Rendering::DescriptorSetDesc ds0{};
                                ds0.layout = m_OverlaySet0Layout;
                                ds0.transient = true;
                                ds0.debugName = "ShadowDebugOverlay.Set0";
                                auto set0 = dev->CreateDescriptorSet(ds0);
                                Rendering::NamedDescriptorWriter wdesc(dev, set0, *m_OverlayMeta,
                                                                       0);
                                if (wdesc.Has("ShadowMapArray") && m_OverlaySampler.IsValid())
                                    wdesc.AddCombinedImageSampler("ShadowMapArray", shadowTex,
                                                                  m_OverlaySampler);
                                wdesc.Flush();
                                cl->BindDescriptorSet(0, set0, pipe);
                            }

                            cl->Draw(3, 1);
                        });
                });
        }
    }
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes

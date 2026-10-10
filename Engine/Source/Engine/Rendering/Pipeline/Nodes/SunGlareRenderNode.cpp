#include "Engine/Rendering/Pipeline/Nodes/SunGlareRenderNode.h"

#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SkyRenderFeature.h"
#include "Engine/Rendering/SunGlareRenderFeature.h"
#include "Engine/Rendering/SunScreenProjection.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Sky/SkyRenderer.h"
#include "Rendering/Sky/SkySettings.h"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <string>

namespace GameEngine::Engine::Renderer { using namespace ::GameEngine::Rendering; }

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{

namespace RenderGraph = ::GameEngine::Rendering::RenderGraph;
using namespace ::GameEngine::Rendering;

namespace
{

// Occlusion probe radius, in units of the sun's angular radius. The ring must not reach much
// past the disc: at three radii the taps on the occluded side leave the sum while the disc is
// still fully hidden, which showed up as a bright blob sitting on a hillside with no sun in it.
// Just over one radius keeps the probe asking about the sun rather than its surroundings, and
// the fade still spans the disc's own width because the taps are area-distributed inside it.
constexpr float kProbeRadiusInSunRadii = 1.4f;



// Night fade at which the sun stops being drawn at all. The sky fades the disc out with
// `1 - nightSkyBlend` as the sun goes below the horizon (SkyEnvironmentSystem computes the
// blend, sky_render.frag multiplies the disc by it), and the halo has to follow the SAME
// curve: a hard elevation cutoff would pop the glare off while the disc was still visibly
// fading. Past this the disc is gone and the pass is not declared.
constexpr float kNightFadeCutoff = 0.98f;

} // namespace

bool SunGlareRenderNode::Initialize(std::string nodeId, std::string nodeJson,
                                    std::string* outError)
{
    m_Id = std::move(nodeId);
    m_OutputRef = Names::View::Resolve;

    try
    {
        const auto j = nlohmann::json::parse(nodeJson);
        if (j.is_object() && j.contains("output") && j["output"].is_string())
            m_OutputRef = j["output"].get<std::string>();
    }
    catch (const std::exception& ex)
    {
        if (outError)
            *outError = std::string("SunGlare node JSON parse failed: ") + ex.what();
        return false;
    }

    return true;
}

void SunGlareRenderNode::DeclareForView(ViewDeclare& d)
{
    auto& rs = d.Services;
    auto* device = rs.GetDevice();
    if (!device)
        return;

    // No sky, no sun, no glare. The same gate the disc is behind: showSunDisk is what the
    // user turns off to hide the sun, and the halo of a hidden sun is a contradiction.
    auto* skyFeat = rs.GetFeature<SkyRenderFeature>();
    if (!skyFeat || !skyFeat->IsInitialized() || !skyFeat->HasActiveSettings())
        return;
    const Rendering::SkySettings& sky = skyFeat->GetSettings();
    if (!sky.showSunDisk || !(sky.primarySunIntensity > 0.0f))
        return;

    const float nightAmount = std::clamp(sky.nightSkyBlend, 0.0f, 1.0f);
    if (nightAmount >= kNightFadeCutoff)
        return;

    const float sunDir[3] = {sky.scatteringSunDir[0], sky.scatteringSunDir[1],
                             sky.scatteringSunDir[2]};

    auto& renderer = skyFeat->GetRenderer();
    if (!renderer.IsTransmittanceComputed())
        return;

    const CameraData* camera = rs.Views().FindCameraData(d.View.cameraId);
    if (!camera)
        return;

    // Orthographic views have no glare. Every ray is parallel, so the angle to the sun is
    // the same at every pixel and the kernel would evaluate to one constant — a flat wash
    // over the whole viewport rather than a halo. The sky already treats these views
    // separately, drawing a 2D backdrop instead of the atmosphere. A projection matrix's
    // (3,3) element is 0 when it divides by w and 1 when it does not.
    if (std::fabs(camera->proj[15]) > 0.5f)
        return;

    RenderGraph::RGTexture target{};
    if (m_OutputRef == Names::View::Resolve)
        target = d.ViewResolve.IsValid() ? d.ViewResolve : d.ResolveTexture(Names::View::Resolve);
    else if (m_OutputRef == Names::View::Color)
        target = d.ViewColor.IsValid() ? d.ViewColor : d.ResolveTexture(Names::View::Color);
    else
        target = d.ResolveTexture(m_OutputRef);
    if (!target.IsValid() && d.ViewColor.IsValid())
        target = d.ViewColor;
    if (!target.IsValid())
        return;

    const PipelineBufferBindingRG viewParams = d.ResolveBuffer(Names::Res::ViewParams);
    if (!viewParams.IsValid())
        return;

    auto& feature = rs.EnsureFeature<SunGlareRenderFeature>();
    if (!feature.IsInitialized() && !feature.Initialize(device))
        return;

    // The scene depth ATTACHMENT, not a resolve of it. The probe reads sixteen texels, which
    // does not justify a resolve pass of its own — and a resolve is a pass that has to be
    // scheduled and paid for every frame whether the sun is up or not.
    //
    // The attachment also holds every depth writer's depth, a forward draw that writes its own
    // in the World pass included (ForwardDrawDepth::ColourPass), which the PREPASS resolve does
    // not: a probe reading that resolve would let the sun glare through such a surface. This
    // pass declares at kPostProcess, after every depth writer, so the read-after-write edge the
    // graph derives from it is what orders the two — no explicit pass ordering is needed or wanted.
    //
    // Multisampled whenever MSAA is on, which is why there are two pipelines: a sampler2D
    // and a sampler2DMS variant, chosen from the attachment's own sample count below.
    RenderGraph::RGTexture depth = d.ViewDepth;
    if (!depth.IsValid())
        depth = d.ResolveTexture(Names::View::Depth);

    const RenderGraph::RGTexture trans = d.Frame.ImportExternalTexture(
        "Sky_Transmittance_LUT", renderer.GetTransmittanceLutTexture(),
        Rendering::ResourceState::ShaderResource);
    if (!trans.IsValid())
        return;

    // Shadow cascades: the array the shadow pass rendered plus the block carrying its
    // per-cascade matrices. Both absent (no directional shadows this frame) simply disables
    // the cascade term — the pass still runs on the screen probe alone.
    const RenderGraph::RGTexture shadowArray = rs.GetShadowMapArrayRG(d.Frame, d.View.id);
    const PipelineBufferBindingRG shadowData = d.ResolveBuffer(Names::Res::ShadowData);
    const bool cascadesReady = shadowArray.IsValid() && shadowData.IsValid();

    SunGlarePushConstants pc{};
    pc.SunDirRadius[0] = sunDir[0];
    pc.SunDirRadius[1] = sunDir[1];
    pc.SunDirRadius[2] = sunDir[2];
    // The width of the SOURCE, which sets the glare kernel's core: scattered light carries no
    // structure finer than the thing that made it, so the halo is flat across the sun instead of
    // adding a second spike on top of the drawn disc.
    //
    // Deliberately the physical radius rather than the radius the sky DRAWS the disc at: the two
    // part company once SkyEnvironment::SunSize is moved off 1, and it is the source that
    // scatters. A stylistically enlarged disc therefore keeps a halo sized for the real sun,
    // which is the intent — but it also means the halo stops spreading visibly outside a disc a
    // few times oversized.
    pc.SunDirRadius[3] = kSunAngularRadiusRad;
    pc.SunColorIrradiance[0] = sky.primarySunColor[0];
    pc.SunColorIrradiance[1] = sky.primarySunColor[1];
    pc.SunColorIrradiance[2] = sky.primarySunColor[2];
    pc.SunColorIrradiance[3] = sky.primarySunIntensity;

    // Everything the sky scales the drawn sun by, so the halo tracks the disc exactly: the
    // exposure trim it applies to the whole dome, times the day-to-night fade that takes the
    // disc out as the sun sets.
    pc.Params[0] = std::exp2(sky.exposureEV) * (1.0f - nightAmount);

    // Transmittance LUT coordinates for the ray to the sun. The LUT is keyed on the ray's
    // zenith cosine and the observer's normalised altitude; both are one value per frame, so
    // the shader takes them as scalars and does the single tap in its vertex stage.
    Rendering::AtmosphereParametersGPU atmo{};
    Rendering::SkyRenderer::FillDefaultAtmosphere(atmo);
    const float planetCenterY = -atmo.planetRadius;
    const float dx = camera->cameraPos[0];
    const float dy = camera->cameraPos[1] - planetCenterY;
    const float dz = camera->cameraPos[2];
    const float r = std::max(std::sqrt(dx * dx + dy * dy + dz * dz), 1e-5f);
    const float upX = dx / r, upY = dy / r, upZ = dz / r;
    pc.Params[1] =
        std::clamp(sunDir[0] * upX + sunDir[1] * upY + sunDir[2] * upZ, -1.0f, 1.0f);
    pc.Params[2] = std::clamp(
        (r - atmo.planetRadius) / std::max(atmo.atmosphereRadius - atmo.planetRadius, 1e-3f), 0.0f,
        1.0f);

    // Where the sun lands on screen, for the occlusion probe. Off-screen the probe has
    // nothing to ask about, so it is skipped and the halo continues from beyond the frame
    // edge — which is what a bright source just outside the field really does. The kernel's
    // own falloff is the only thing that fades it; there is no separate edge dial.
    float ndcX = 0.0f, ndcY = 0.0f, ndcDepth = 0.0f;
    const bool projected = ProjectSunDirectionToNdc(*camera, sunDir, ndcX, ndcY, ndcDepth);
    const bool onScreen =
        projected && std::fabs(ndcX) <= 1.0f && std::fabs(ndcY) <= 1.0f && depth.IsValid();
    pc.Probe[0] = ndcX * 0.5f + 0.5f;
    pc.Probe[1] = 0.5f - ndcY * 0.5f;
    // Ring radius PER AXIS. The projection's [0][0] and [1][1] terms are the reciprocals of
    // the frustum's horizontal and vertical half-extents, so scaling the same angular radius
    // by each gives a ring that is a circle in PIXELS. One scalar on both axes stretches it
    // by the viewport aspect — on a 3.4:1 view the horizontal taps land three times further
    // from the sun than the vertical ones, and anything narrower can never occlude it.
    const float projXX = std::fabs(camera->proj[0]);
    const float projYY = std::fabs(camera->proj[5]);
    pc.Probe[2] = kProbeRadiusInSunRadii * kSunAngularRadiusRad * projXX * 0.5f;
    pc.Probe[3] = kProbeRadiusInSunRadii * kSunAngularRadiusRad * projYY * 0.5f;

    // The depth attachment's sample count decides the variant, and it also IS the probe's
    // enable flag: the shader reads params.w as the count and treats 0 as "no probe this
    // frame", so a sun off screen and a missing depth texture take the same branch.
    const uint32_t depthSamples =
        depth.IsValid() ? std::max(d.Frame.Graph().ResourceDesc(depth.Id).SampleCount, 1u) : 1u;
    pc.Params[3] = onScreen ? static_cast<float>(depthSamples) : 0.0f;

    // The terrain skyline along the sun's own azimuth, on the CPU, from the heightfield.
    //
    // This is the term that does not care where the camera points. The screen probe is gated on
    // the sun projecting inside the viewport, so a sun one degree outside the frame edge cannot
    // be occluded by anything at all -- the halo's edge glow and far veil stayed at full
    // strength behind a hill and then switched off the frame after the sun's centre crossed in.
    // A skyline is a property of the terrain and the eye, so it holds on- and off-frame, and
    // past whatever distance the shadow cascades reach.
    //
    // Compared as TANGENTS, and faded over the sun's own angular radius rather than a chosen
    // width: the disc is hidden when the skyline is one radius above its centre and clear when
    // it is one radius below, which is exactly the geometry of a disc setting behind a ridge.
    float horizonVisibility = 1.0f;
    {
        // Marched on the terrain side (TerrainExtractionSystem), which is the only layer that
        // can see both the heightfield and the renderer. Absent means "no terrain to report
        // on", which must read as fully visible rather than as flat ground.
        const float horizonTangent = feature.GetSkylineTangent(d.View.id);
        if (horizonTangent > SunGlareRenderFeature::kNoSkyline)
        {
            // Both sides as tangents of the elevation above the horizontal. sunDir is a unit
            // vector, so its Y IS the sine of the sun's elevation and the horizontal component
            // is the cosine.
            const float sunHoriz =
                std::sqrt(std::max(sunDir[0] * sunDir[0] + sunDir[2] * sunDir[2], 1e-12f));
            const float sunTangent = sunDir[1] / sunHoriz;
            const float radiusTangent = kSunAngularRadiusRad; // tan(r) == r at this size
            const float t = std::clamp(
                (sunTangent - (horizonTangent - radiusTangent)) / (2.0f * radiusTangent), 0.0f,
                1.0f);
            horizonVisibility = t * t * (3.0f - 2.0f * t); // smoothstep over the disc's diameter
        }
    }
    pc.Horizon[0] = horizonVisibility;

    // The cascade term needs the eye and the cascade count; the shadow data block carries the
    // matrices. Zero cascades disables it, leaving the screen probe as the only gate.
    pc.CameraShadow[0] = camera->cameraPos[0];
    pc.CameraShadow[1] = camera->cameraPos[1];
    pc.CameraShadow[2] = camera->cameraPos[2];
    // 1 = the cascade bindings are real this frame. The COUNT is not passed: the shadow
    // block already carries it (ge_shadowParams.z), and a second copy could disagree with it.
    pc.CameraShadow[3] = cascadesReady ? 1.0f : 0.0f;

    // Draw into the view's letterboxed region, the way the sky pass does. A fullscreen draw
    // over the whole target would tint the bars the sky deliberately left cleared, and the
    // NDC the vertex stage emits is mapped onto whatever this viewport is — so the halo
    // lands where the frame is either way.
    uint32_t vpX = 0, vpY = 0, vpW = d.RenderWidth, vpH = d.RenderHeight;
    const ViewLetterbox letterbox = rs.Views().GetViewLetterbox(d.View.id);
    if (letterbox.active)
    {
        vpX = letterbox.x;
        vpY = letterbox.y;
        vpW = letterbox.width;
        vpH = letterbox.height;
    }
    // One of the two variants, by the attachment's sample count. An MSAA view whose
    // multisampled variant failed to load gets no glare rather than a multisampled image bound
    // to a sampler2D, which is undefined — the feature logs that case at initialization.
    const GraphicsPipelineId pipe = feature.GetPipelineId(depthSamples);
    if (!pipe.IsValid())
        return;
    const DescriptorSetLayoutDesc layout = feature.GetLayout(depthSamples);
    const SamplerHandle sampler = feature.GetSampler();
    const uint32_t transBinding = feature.GetTransmittanceBinding();
    const uint32_t depthBinding = feature.GetDepthBinding();
    const uint32_t viewParamsBinding = feature.GetViewParamsBinding();
    const uint32_t shadowArrayBinding = feature.GetShadowArrayBinding();
    const uint32_t shadowDataBinding = feature.GetShadowDataBinding();
    const SamplerHandle shadowSampler = feature.GetShadowSampler();
    // A view without casters still needs a depth array in the shadow slot: WebGPU
    // bakes the depth sample type into the layout, so the LUT cannot stand in.
    const TextureHandle shadowFallback = rs.GetCascadeShadowFallbackTexture();

    d.Frame.AddPass(
        d.PassName().c_str(), Rendering::PassPhase::kPostProcess,
        [&](RenderGraph::RGPassBuilder& p)
        {
            RenderGraph::RGAttachmentOps ops{};
            ops.Load = RenderGraph::RGLoadOp::Load; // additive over the scene
            ops.Store = RenderGraph::RGStoreOp::Store;
            p.AttachColor(0, target, ops);
            p.Read(trans, RenderGraph::RGTextureRead::Sampled);
            if (depth.IsValid())
                p.Read(depth, RenderGraph::RGTextureRead::Sampled);
            // A blueprint buffer backed by upload memory dissolves into AllocUpload and
            // carries no graph id: the binding is valid while the edge is not. Declaring an
            // invalid edge asserts, so gate on the graph handle the way every other
            // buffer-reading node does.
            if (viewParams.Graph.IsValid())
                p.Read(viewParams.Graph, RenderGraph::RGBufferRead::Uniform);
            if (shadowArray.IsValid())
                p.Read(shadowArray, RenderGraph::RGTextureRead::Sampled);
            if (shadowData.Graph.IsValid())
                p.Read(shadowData.Graph, RenderGraph::RGBufferRead::Uniform);
        },
        [pipe, layout, sampler, shadowSampler, shadowFallback, pc, trans, depth, viewParams,
         shadowArray, shadowData, vpX, vpY, vpW, vpH, transBinding, depthBinding,
         viewParamsBinding, shadowArrayBinding, shadowDataBinding](RenderGraph::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl)
                return;
            const auto pso = ctx.GetOrCreatePipelineVariant(pipe);
            if (!pso.IsValid())
                return;

            const TextureHandle transPhys = ctx.GetTexture(trans);
            if (!transPhys.IsValid())
                return;
            // The probe is disabled in the push constants when depth is unavailable, so a
            // type-compatible dummy is safe here (the LUT is the one texture always bound).
            // The dummy is single-sample, and it can only be reached on the single-sample
            // pipeline: the multisampled variant is selected from a VALID depth attachment.
            const TextureHandle depthPhys = depth.IsValid() ? ctx.GetTexture(depth) : TextureHandle{};

            DescriptorSetDesc dsDesc{};
            dsDesc.layout = layout;
            dsDesc.debugName = "SunGlare.DS";
            dsDesc.transient = true;
            auto ds = dev->CreateDescriptorSet(dsDesc);
            dev->UpdateCombinedImageSamplerBinding(ds, transBinding, transPhys, sampler);
            dev->UpdateCombinedImageSamplerBinding(
                ds, depthBinding, depthPhys.IsValid() ? depthPhys : transPhys, sampler);
            dev->UpdateBufferBinding(ds, viewParamsBinding, viewParams.Buffer,
                                     static_cast<size_t>(viewParams.Offset),
                                     static_cast<size_t>(viewParams.Size));
            // The cascade term is disabled through the push constants when these are absent,
            // but the descriptors still have to be filled or the set is incomplete: the
            // engine's cascade fallback (a depth array) and the view params stand in.
            const TextureHandle shadowPhys =
                shadowArray.IsValid() ? ctx.GetTexture(shadowArray) : TextureHandle{};
            const TextureHandle shadowBound = shadowPhys.IsValid() ? shadowPhys : shadowFallback;
            if (!shadowBound.IsValid())
                return;
            dev->UpdateCombinedImageSamplerBinding(ds, shadowArrayBinding, shadowBound, shadowSampler);
            dev->UpdateBufferBinding(
                ds, shadowDataBinding,
                shadowData.IsValid() ? shadowData.Buffer : viewParams.Buffer,
                static_cast<size_t>(shadowData.IsValid() ? shadowData.Offset : viewParams.Offset),
                static_cast<size_t>(shadowData.IsValid() ? shadowData.Size : viewParams.Size));

            cl->SetPipeline(pso);
            cl->SetViewport(static_cast<float>(vpX), static_cast<float>(vpY),
                            static_cast<float>(vpW), static_cast<float>(vpH));
            cl->SetScissor(vpX, vpY, vpW, vpH);
            cl->BindDescriptorSet(0, ds, pso);
            cl->SetPushConstants(pc);
            cl->Draw(3, 1);
        });
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes

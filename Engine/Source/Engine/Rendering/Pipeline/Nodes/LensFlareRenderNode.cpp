#include "Engine/Rendering/Pipeline/Nodes/LensFlareRenderNode.h"

#include "Engine/Rendering/ExposureReadbackFeature.h"
#include "Engine/Rendering/LensFlareRenderFeature.h"
#include "Mathematics/Curve.h" // ::GameEngine::Math::EvaluateCurveKeys
#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SunScreenProjection.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace GameEngine::Engine::Renderer { using namespace ::GameEngine::Rendering; }

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{

namespace RenderGraph = ::GameEngine::Rendering::RenderGraph;
using namespace ::GameEngine::Rendering;

namespace
{
constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

// Converts a flare "scale unit" to an NDC half-size. Derived, not tuned: the
// source tool multiplies (size × Scale × GlobalScale) by 0.01 and renders in an
// orthographic space whose half-HEIGHT is exactly 1 unit — i.e. NDC Y.
constexpr float kNdcPerScaleUnit = 0.01f;

// Default bell curve applied to the raw edge amount (keys (0,0) (0.5,1) (1,0)
// in the authoring tool): the boost peaks exactly at the screen border and
// falls off again as the source moves further off screen. Piecewise-linear
// stand-in until keyframed curves are imported.
float EvalDefaultEdgeCurve(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return t <= 0.5f ? t * 2.0f : 2.0f - t * 2.0f;
}

// Raw edge amount in viewport coords ([0,1], source convention): 0 inside the
// [bias+range .. 1-bias-range] box, ramping to 0.5 as an axis reaches the
// screen border and to 1.0 one range-width beyond it; the max of the two axes
// wins. Mirrors the source's zone test per axis.
float EdgeAmountForAxis(float v, float range, float bias)
{
    const float lo = bias + range;
    const float hi = 1.0f - bias - range;
    const bool inside = v > lo && v < hi;
    const bool inBand = v + range > bias && v - range < 1.0f - bias;
    if (inside || !inBand)
        return 0.0f;
    const float raw = v > 0.5f ? (v - 1.0f + bias) + range : std::fabs(v - bias - range);
    return raw / std::max(range, 1e-3f) * 0.5f;
}

// Flares are authored in display-referred terms with values in [0,1], but the
// pass draws additively into the scene-linear HDR buffer before
// exposure + tonemap. Pre-dividing instance color by the exposure the tonemap
// will apply keeps the flare's apparent brightness equal to its authored value
// regardless of scene luminance — the same pre-exposure compensation HDRP's
// SRP lens flares use. Clamp keeps a degenerate metered scale from producing
// inf/NaN instance colors.
constexpr float kMinExposureScale = 1.0e-4f;
constexpr float kMaxExposureScale = 1.0e4f;

// One draw's worth of instances sharing an atlas texture.
struct AtlasBatch
{
    ::GameEngine::Rendering::TextureHandle Atlas{};
    bool HasAuthoredAlpha = false;
    std::vector<FlareInstanceGPU> Instances;
};

// Build one view's instance batches from the view-independent resolved flares,
// grouped by atlas (flares on different atlases become separate draws).
// `depthAvailable` gates the per-source occlusion probe. `invExposure`
// pre-compensates instance color for the view's exposure.
void BuildViewInstances(const std::vector<ResolvedFlare>& flares,
                        const ::GameEngine::Rendering::CameraData& cam, bool depthAvailable,
                        float time, float invExposure, float viewportAspect,
                        std::vector<AtlasBatch>& outBatches)
{
    for (const ResolvedFlare& rf : flares)
    {
        if (!rf.Atlas.IsValid())
            continue;
        AtlasBatch* batch = nullptr;
        for (AtlasBatch& b : outBatches)
            if (b.Atlas == rf.Atlas)
            {
                batch = &b;
                break;
            }
        if (!batch)
        {
            outBatches.push_back(AtlasBatch{rf.Atlas, rf.AtlasHasAuthoredAlpha, {}});
            batch = &outBatches.back();
        }
        std::vector<FlareInstanceGPU>& out = batch->Instances;

        float ndcX = 0.0f, ndcY = 0.0f, ndcDepth = 0.0f;
        const bool projected = rf.SunMode
                                   ? ProjectSunDirectionToNdc(
                                         cam, rf.Forward, ndcX, ndcY, ndcDepth)
                                   : ProjectToNdc(
                                         cam.viewProj, rf.WorldPos[0], rf.WorldPos[1],
                                         rf.WorldPos[2], ndcX, ndcY, ndcDepth);
        if (!projected)
            continue;

        // Optical axis: source -> screen-center (NDC origin).
        const float axisX = -ndcX;
        const float axisY = -ndcY;

        const LensFlare::FlareGlobals& g = rf.Globals;
        float flareBright = g.GlobalBrightness * rf.Intensity;
        float flareSize = g.GlobalScale * rf.Scale;
        if (g.MultiplyScaleByTransformScale)
            flareSize *= rf.TransformScale;

        // Camera->source vector; the distance fade uses VIEW DEPTH (projection
        // onto the camera forward), not euclidean distance, per the source's
        // dot(heading, cam.forward). Forward = view matrix Z row (LH).
        const float dx = rf.SunMode ? rf.Forward[0] : rf.WorldPos[0] - cam.cameraPos[0];
        const float dy = rf.SunMode ? rf.Forward[1] : rf.WorldPos[1] - cam.cameraPos[1];
        const float dz = rf.SunMode ? rf.Forward[2] : rf.WorldPos[2] - cam.cameraPos[2];
        const float camFwdX = cam.view[2];
        const float camFwdY = cam.view[6];
        const float camFwdZ = cam.view[10];

        const float maxDistance =
            rf.MaxDistanceOverride > 0.0f ? rf.MaxDistanceOverride : g.MaxDistance;
        float distanceFalloff = 1.0f;
        if (!rf.SunMode && g.UseMaxDistance && maxDistance > 0.0f)
        {
            const float viewDepth = dx * camFwdX + dy * camFwdY + dz * camFwdZ;
            distanceFalloff = 1.0f - viewDepth / maxDistance;
            if (distanceFalloff < 0.001f && !g.NeverCull)
                continue;
            distanceFalloff = std::clamp(distanceFalloff, 0.0f, 1.0f);
        }

        // Angle falloff: angle between the flare's own forward and the vector
        // to the camera; falls to 0 at HALF the max angle (source convention),
        // hard 0 beyond the max.
        float angleFalloff = 1.0f;
        if (!rf.SunMode && g.UseAngleLimit && g.MaxAngle > 0.0f)
        {
            const float toCamLen = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (toCamLen > 1e-6f)
            {
                const float cosA = std::clamp((-dx * rf.Forward[0] - dy * rf.Forward[1] -
                                               dz * rf.Forward[2]) / toCamLen,
                                              -1.0f, 1.0f);
                const float angleDeg = std::acos(cosA) / kDegToRad;
                angleFalloff = angleDeg > g.MaxAngle
                                   ? 0.0f
                                   : std::clamp(1.0f - angleDeg / (g.MaxAngle * 0.5f), 0.0f, 1.0f);
                if (g.UseAngleCurve && !rf.AngleCurve.empty())
                    angleFalloff = std::max(
                        0.0f, ::GameEngine::Math::EvaluateCurveKeys(rf.AngleCurve.data(),
                                                      static_cast<uint32_t>(rf.AngleCurve.size()),
                                                      angleFalloff));
            }
        }

        // Dynamic edge/center amounts in viewport coords (source convention).
        const float vpX = ndcX * 0.5f + 0.5f;
        const float vpY = ndcY * 0.5f + 0.5f;
        float edgeAmount = 0.0f;
        if (g.UseDynamicEdgeBoost || g.UseDynamicEdgeScale)
        {
            const float raw = std::max(EdgeAmountForAxis(vpX, g.DynamicEdgeRange, g.DynamicEdgeBias),
                                       EdgeAmountForAxis(vpY, g.DynamicEdgeRange, g.DynamicEdgeBias));
            edgeAmount = rf.DynamicEdgeCurve.empty()
                             ? EvalDefaultEdgeCurve(raw)
                             : std::max(0.0f, ::GameEngine::Math::EvaluateCurveKeys(
                                                  rf.DynamicEdgeCurve.data(),
                                                  static_cast<uint32_t>(rf.DynamicEdgeCurve.size()),
                                                  raw));
        }
        float centerBoost = 0.0f;
        if (g.UseDynamicCenterBoost && g.DynamicCenterRange > 0.0f)
        {
            const float d = std::max(std::fabs(vpX - 0.5f), std::fabs(vpY - 0.5f));
            if (d < g.DynamicCenterRange)
                centerBoost = 1.0f - d / g.DynamicCenterRange;
        }

        // Off-screen fade (rectangular NDC distance past the frame edge).
        const float rEdge = std::max(std::fabs(ndcX), std::fabs(ndcY));
        float offScreenFade = 1.0f;
        if (rEdge > 1.0f && !g.NeverCull)
            offScreenFade =
                std::clamp(1.0f - (rEdge - 1.0f) / std::max(g.OffScreenFadeDist, 1e-3f), 0.0f, 1.0f);

        // Dynamic boosts are per ELEMENT (an element may override the flare's
        // coefficient): add the boost to the element's alpha term, then apply
        // the flare-wide multiplicative fades. `brightFadeMul`/`scaleFadeMul`
        // gather those fades once per flare.
        float brightFadeMul = offScreenFade;
        if (g.UseAngleBrightness)
            brightFadeMul *= angleFalloff;
        if (g.UseDistanceFade)
            brightFadeMul *= distanceFalloff;

        float scaleFadeMul = 1.0f;
        if (g.UseAngleScale)
            scaleFadeMul *= angleFalloff;
        if (g.UseDistanceScale)
            scaleFadeMul *= distanceFalloff;

        // Whole flare dead only when no element can contribute (base brightness
        // zero and no active boost zone).
        const bool boostsPossible = (g.UseDynamicEdgeBoost || g.UseDynamicEdgeScale) && edgeAmount > 0.0f;
        const bool centerPossible = g.UseDynamicCenterBoost && centerBoost > 0.0f;
        if (!g.NeverCull &&
            (brightFadeMul <= 0.0f || (flareBright <= 0.0f && !boostsPossible && !centerPossible)))
            continue;

        const auto coefOr = [](float elementOverride, float global)
        {
            return elementOverride != LensFlare::kInheritGlobalBoost ? elementOverride : global;
        };

        const float probeU = ndcX * 0.5f + 0.5f;
        const float probeV = 0.5f - ndcY * 0.5f;
        // Probe only when this source wants occlusion AND scene depth is bound for
        // this view (otherwise the dummy depth bind would be sampled as garbage).
        const float probeDepth = (rf.Occlude && depthAvailable) ? ndcDepth : -1.0f;

        for (const ResolvedFlareElement& e : rf.Elements)
        {
            FlareInstanceGPU inst;
            // Anamorphic is a per-axis pull of the element's position back
            // toward the source (1 = pinned to the source on that axis), which
            // is what smears a ghost trail into an axis-aligned streak; it is
            // not a size stretch.
            float posX = ndcX + axisX * e.Position;
            float posY = ndcY + axisY * e.Position;
            posX += (ndcX - posX) * std::clamp(e.AnamorphicX, 0.0f, 1.0f) + e.OffsetX;
            posY += (ndcY - posY) * std::clamp(e.AnamorphicY, 0.0f, 1.0f) + e.OffsetY;
            // Per-element size boost (element override or the flare's global).
            float elemScaleBoost = 0.0f;
            if (g.UseDynamicEdgeScale)
                elemScaleBoost += coefOr(e.EdgeScaleBoost, g.DynamicEdgeScale) * edgeAmount;
            if (g.UseDynamicCenterBoost)
                elemScaleBoost += coefOr(e.CenterScaleBoost, g.DynamicCenterScale) * centerBoost;

            // Multi-element star angles are already baked into e.Angle when the
            // source asset is flattened by LensFlareImporter. UseStarRotation is
            // retained for round-tripping, but must not rotate the completed star
            // again as the source moves around the screen.
            float rot = e.Angle * kDegToRad;
            if (e.RotateToFlare)
            {
                // Face the source in pixel space. NDC X must be weighted by the
                // viewport aspect before atan2 or wide views skew the angle.
                const float tox = ndcX - posX;
                const float toy = ndcY - posY;
                if (tox * tox + toy * toy > 1e-12f)
                    rot += std::atan2(toy, tox * viewportAspect);
            }
            if (e.RotationSpeed != 0.0f)
                rot += e.RotationSpeed * kDegToRad * time;

            // Shape the sprite first, then rotate that shaped quad in pixel
            // space. Store the resulting basis in NDC so the vertex shader only
            // needs a pair of multiply-adds and cannot reintroduce S*R shear.
            const float half = flareSize * std::max(0.0f, 1.0f + elemScaleBoost) * scaleFadeMul *
                               e.Scale * kNdcPerScaleUnit;
            const float halfX = half * e.SizeX;
            const float halfY = half * e.SizeY;

            const float invAspect = 1.0f / std::max(viewportAspect, 1e-6f);
            const float sinRot = std::sin(rot);
            const float cosRot = std::cos(rot);
            inst.PosBasisX[0] = posX;
            inst.PosBasisX[1] = posY;
            inst.PosBasisX[2] = cosRot * halfX * invAspect;
            inst.PosBasisX[3] = sinRot * halfX;
            inst.BasisYProbe[0] = -sinRot * halfY * invAspect;
            inst.BasisYProbe[1] = cosRot * halfY;
            inst.BasisYProbe[2] = probeU;
            inst.BasisYProbe[3] = probeV;

            inst.UVRect[0] = e.UVRect[0];
            inst.UVRect[1] = e.UVRect[1];
            inst.UVRect[2] = e.UVRect[2];
            inst.UVRect[3] = e.UVRect[3];

            // Per-element brightness boost added to the element's alpha term
            // (source convention: boosts add AFTER the tint-alpha product),
            // then flare-wide fades + exposure compensation multiply in.
            float elemBrightBoost = 0.0f;
            if (g.UseDynamicEdgeBoost)
                elemBrightBoost += coefOr(e.EdgeBrightnessBoost, g.DynamicEdgeBrightness) * edgeAmount;
            if (g.UseDynamicCenterBoost)
                elemBrightBoost +=
                    coefOr(e.CenterBrightnessBoost, g.DynamicCenterBrightness) * centerBoost;

            const float alphaTerm =
                flareBright * e.Brightness * e.Tint[3] * g.GlobalTint.A * rf.Tint[3] +
                elemBrightBoost;
            const float bright = std::max(0.0f, alphaTerm) * brightFadeMul * invExposure;
            if (bright <= 0.0f)
                continue;
            inst.ColorDepth[0] = g.GlobalTint.R * e.Tint[0] * rf.Tint[0] * bright;
            inst.ColorDepth[1] = g.GlobalTint.G * e.Tint[1] * rf.Tint[1] * bright;
            inst.ColorDepth[2] = g.GlobalTint.B * e.Tint[2] * rf.Tint[2] * bright;
            inst.ColorDepth[3] = probeDepth;

            out.push_back(inst);
        }
    }
}
} // namespace

bool LensFlareRenderNode::Initialize(std::string nodeId, std::string nodeJson,
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
            *outError = std::string("LensFlare node JSON parse failed: ") + ex.what();
        return false;
    }

    return true;
}

void LensFlareRenderNode::DeclareForView(ViewDeclare& d)
{
    auto& rs = d.Services;
    auto* device = rs.GetDevice();
    if (!device)
        return;

    auto& feature = rs.EnsureFeature<LensFlareRenderFeature>();
    if (!feature.IsInitialized())
    {
        if (!feature.Initialize(device))
            return;
    }
    if (!feature.HasFlares())
        return;

    // Draw additively onto the selected color texture, preserving scene contents.
    RenderGraph::RGTexture target{};
    if (m_OutputRef == Names::View::Resolve)
    {
        target = d.ViewResolve.IsValid() ? d.ViewResolve : d.ResolveTexture(Names::View::Resolve);
    }
    else if (m_OutputRef == Names::View::Color)
    {
        target = d.ViewColor.IsValid() ? d.ViewColor : d.ResolveTexture(Names::View::Color);
    }
    else
    {
        target = d.ResolveTexture(m_OutputRef);
    }

    if (!target.IsValid() && d.ViewColor.IsValid())
        target = d.ViewColor;
    if (!target.IsValid())
        return;

    // Scene depth for the occlusion probe (optional).
    RenderGraph::RGTexture depth = d.ViewDepthResolved.IsValid() ? d.ViewDepthResolved : d.ViewDepth;
    if (!depth.IsValid())
        depth = d.ResolveTexture(Names::View::DepthResolved);
    const bool depthAvailable = depth.IsValid();

    // Project + size + fade against THIS view's camera so multi-view is correct.
    const CameraData* camera = rs.Views().FindCameraData(d.View.cameraId);
    if (!camera)
        return;

    // Exposure the tonemap will apply to this view, from the same effective
    // post-process settings the AutoExposure/Tonemap nodes consume (covers the
    // editor Scene View, the Game view, and the Player uniformly). When auto
    // exposure meters this view, use last frame's adapted scale via the GPU
    // readback (enabled on demand — a view showing flares keeps the 16-byte/
    // frame readback on); until the first readback lands, the settings' base
    // exposure keeps the flare merely dim for a frame or two rather than wrong.
    const PostProcessSettings& pp = rs.GetEffectivePostProcessSettings(d.View.id, d.View.worldId);
    float exposureScale = pp.Exposure;
    if (pp.IsAutoExposureActive())
    {
        auto& readback = rs.EnsureFeature<ExposureReadbackFeature>();
        readback.SetReadbackEnabled(d.View.id, true);
        float adapted = 0.0f;
        if (readback.TryResolveAdaptedExposure(d.View.id, adapted))
            exposureScale = adapted;
    }
    exposureScale = std::clamp(exposureScale, kMinExposureScale, kMaxExposureScale);
    const float invExposure = 1.0f / exposureScale;

    const float viewportAspect =
        d.RenderHeight > 0 ? static_cast<float>(d.RenderWidth) / static_cast<float>(d.RenderHeight)
                           : 1.0f;

    std::vector<AtlasBatch> batches;
    BuildViewInstances(feature.GetFlares(), *camera, depthAvailable, feature.GetTime(), invExposure,
                       viewportAspect, batches);
    if (batches.empty())
        return;

    // Concatenate all batches into ONE upload buffer; each batch starts on a
    // 256-byte boundary so the per-draw storage-buffer offset satisfies any
    // backend's minimum offset alignment (256B == 4 instances of padding max).
    constexpr uint32_t kBatchAlignBytes = 256;
    static_assert(kBatchAlignBytes % sizeof(FlareInstanceGPU) == 0,
                  "batch alignment must be a whole number of instances");
    struct BatchDraw
    {
        TextureHandle Atlas{};
        bool HasAuthoredAlpha = false;
        uint32_t ByteOffset = 0;
        uint32_t Count = 0;
    };
    std::vector<FlareInstanceGPU> instances;
    std::vector<BatchDraw> draws;
    draws.reserve(batches.size());
    for (const AtlasBatch& b : batches)
    {
        if (b.Instances.empty())
            continue;
        const uint32_t alignInstances = kBatchAlignBytes / sizeof(FlareInstanceGPU);
        while ((instances.size() % alignInstances) != 0)
            instances.push_back(FlareInstanceGPU{});
        draws.push_back(BatchDraw{
            b.Atlas, b.HasAuthoredAlpha,
            static_cast<uint32_t>(instances.size() * sizeof(FlareInstanceGPU)),
            static_cast<uint32_t>(b.Instances.size())});
        instances.insert(instances.end(), b.Instances.begin(), b.Instances.end());
    }
    if (draws.empty())
        return;

    const uint32_t frameIndex = device->GetFrameIndex();
    uint32_t instanceCount = 0;
    const BufferHandle instanceBuf = feature.UploadInstances(
        *device, static_cast<uint32_t>(d.View.id), frameIndex, instances, instanceCount);
    if (!instanceBuf.IsValid() || instanceCount == 0)
        return;

    const GraphicsPipelineId pipe = feature.GetPipelineId();
    const DescriptorSetLayoutDesc layout = feature.GetLayout();
    const SamplerHandle sampler = feature.GetSampler();
    // Set0 binding indices resolved from lens_flare's reflected meta by the feature.
    const uint32_t atlasBinding = feature.GetAtlasBinding();
    const uint32_t instanceBinding = feature.GetInstanceBinding();
    const uint32_t depthBinding = feature.GetDepthBinding();
    const uint32_t renderW = d.RenderWidth;
    const uint32_t renderH = d.RenderHeight;

    const std::string passName = "LensFlare.View" + std::to_string(static_cast<uint32_t>(d.View.id));

    d.Frame.AddPass(
        passName.c_str(), Rendering::PassPhase::kPostProcess,
        [&](RenderGraph::RGPassBuilder& p)
        {
            RenderGraph::RGAttachmentOps ops{};
            ops.Load = RenderGraph::RGLoadOp::Load; // preserve the scene
            ops.Store = RenderGraph::RGStoreOp::Store;
            p.AttachColor(0, target, ops);
            if (depth.IsValid())
                p.Read(depth, RenderGraph::RGTextureRead::Sampled);
        },
        [pipe, layout, sampler, draws, depth, instanceBuf, renderW, renderH, atlasBinding,
         instanceBinding, depthBinding](RenderGraph::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl)
                return;
            const auto pso = ctx.GetOrCreatePipelineVariant(pipe);
            if (!pso.IsValid())
                return;

            // Scene depth physical handle for the occlusion probe; fall back to the
            // batch atlas as a type-compatible dummy when depth is unavailable (the
            // shader skips probing because BuildViewInstances set source depth < 0).
            TextureHandle depthPhys{};
            if (depth.IsValid())
                depthPhys = ctx.GetTexture(depth);

            cl->SetPipeline(pso);
            cl->SetViewport(0.0f, 0.0f, static_cast<float>(renderW), static_cast<float>(renderH));
            cl->SetScissor(0, 0, renderW, renderH);

            // One draw per atlas batch: same pipeline, a transient descriptor set
            // binding that batch's atlas + its aligned slice of the instance buffer.
            for (const auto& drawBatch : draws)
            {
                DescriptorSetDesc dsDesc{};
                dsDesc.layout = layout;
                dsDesc.debugName = "LensFlare.DS";
                dsDesc.transient = true;
                auto ds = dev->CreateDescriptorSet(dsDesc);
                dev->UpdateCombinedImageSamplerBinding(ds, atlasBinding, drawBatch.Atlas, sampler);
                dev->UpdateStorageBufferBinding(
                    ds, instanceBinding, instanceBuf, drawBatch.ByteOffset,
                    static_cast<size_t>(drawBatch.Count) * sizeof(FlareInstanceGPU));
                dev->UpdateCombinedImageSamplerBinding(
                    ds, depthBinding, depthPhys.IsValid() ? depthPhys : drawBatch.Atlas, sampler);

                cl->BindDescriptorSet(0, ds, pso);
                const int32_t atlasHasAuthoredAlpha = drawBatch.HasAuthoredAlpha ? 1 : 0;
                cl->SetPushConstants(atlasHasAuthoredAlpha);
                cl->Draw(6, drawBatch.Count); // 6 verts (2 tris) per instanced quad
            }
        });
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes

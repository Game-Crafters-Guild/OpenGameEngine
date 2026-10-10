#include "Engine/Rendering/Pipeline/Nodes/TemporalAANode.h"

#include "Engine/Rendering/AntiAliasing.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderServices.h"

#include "Logger/Logger.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/GPUInstanceWorldKey.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/NamedPushConstantWriter.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Geometry/VertexLayoutBuilder.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/Passes/TemporalAAOptions.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

namespace
{
// Resolve tuning (P0 defaults; revisited at the quality gates). Weight and
// sharpen are env-overridable per launch for A/B tuning sessions.
constexpr float kBaseHistoryWeight = 0.95f;  // exponential blend factor (gate-night A/B tuning)
constexpr float kVarianceClipGamma = 1.0f;   // mu +/- gamma*sigma AABB
constexpr float kDepthRejectRelative = 0.08f; // reverse-Z NDC relative tolerance
constexpr float kVelocityFalloffPerPx = 0.01f;
// Below this per-pixel screen motion the depth-disocclusion reject is skipped
// (static jittered edges accumulate = anti-alias, instead of false-rejecting).
constexpr float kDisocclusionMotionGatePx = 2.0f;
// Current-sample unsharp vs the 3x3 mean: compensates the jitter-integration
// box filter so converged stills match MSAA crispness (gate-a evidence).
constexpr float kSharpenStrength = 0.35f;

float EnvTunedFloat(const char* name, float fallback)
{
    if (const char* env = std::getenv(name))
    {
        const float v = static_cast<float>(std::atof(env));
        if (v >= 0.0f && v <= 4.0f)
            return v;
    }
    return fallback;
}

float TunedHistoryWeight()
{
    static const float kValue = EnvTunedFloat("GE_TAA_WEIGHT", kBaseHistoryWeight);
    return kValue;
}

float TunedSharpen()
{
    static const float kValue = EnvTunedFloat("GE_TAA_SHARPEN", kSharpenStrength);
    return kValue;
}

// Variance-clip AABB half-width in sigmas. Lower = tighter = less ghosting but
// more surviving shimmer; higher = looser = more stable but more ghost trails.
float TunedClipGamma()
{
    static const float kValue = EnvTunedFloat("GE_TAA_GAMMA", kVarianceClipGamma);
    return kValue;
}

// Reverse-Z NDC relative disocclusion tolerance. The floor (1e-4 in the shader)
// dominates at the far plane where NDC depth -> 0, so this scalar mostly gates
// near/mid-field; raise it to stop distant reprojected-depth jitter from
// rejecting otherwise-valid history (distant-shimmer lever).
float TunedDepthReject()
{
    static const float kValue = EnvTunedFloat("GE_TAA_DEPTHREJECT", kDepthRejectRelative);
    return kValue;
}

// History-weight attenuation per pixel of screen motion. Higher = fast movers
// trust history less (fresher, less ghosting); lower = steadier under motion.
float TunedVelocityFalloff()
{
    static const float kValue = EnvTunedFloat("GE_TAA_VELFALLOFF", kVelocityFalloffPerPx);
    return kValue;
}

// Disocclusion motion gate (px). Depth-based history rejection is meaningful
// only under motion; below this screen-motion the depth reject is skipped so
// static jittered silhouettes accumulate (edge anti-aliasing) instead of
// falsely rejecting every frame (the dominant static-camera TAA shimmer). Too
// high risks ghosting on genuine mid-motion disocclusions (the color clamp is
// the backstop there).
float TunedRejectGate()
{
    static const float kValue = EnvTunedFloat("GE_TAA_REJECTGATE", kDisocclusionMotionGatePx);
    return kValue;
}

// Dejitter sign/scale knob for the current-sample reconstruction (default +1;
// -1 flips both axes — the one-variable runtime check for the negative-
// viewport Y convention).
float TunedDejitter()
{
    if (const char* env = std::getenv("GE_TAA_DEJITTER"))
    {
        const float v = static_cast<float>(std::atof(env));
        if (v >= -1.0f && v <= 1.0f)
            return v;
    }
    return 1.0f;
}

// GLSL std140 mirror of taa_resolve.comp TaaResolveParams.
struct TaaResolveParamsCPU
{
    float InvViewProj[16];
    float PrevViewProj[16];
    float Extent[4];
    float Params0[4];
    float Params1[4];
    float Params2[4];
};
static_assert(sizeof(TaaResolveParamsCPU) == 192, "must match taa_resolve.comp TaaResolveParams");

// GLSL std140 mirror of taa_resolve_upscale.comp TaaResolveParams (the native
// block + uOutExtent between uExtent and uParams0).
struct TaaUpscaleParamsCPU
{
    float InvViewProj[16];
    float PrevViewProj[16];
    float Extent[4];    // internal
    float OutExtent[4]; // display
    float Params0[4];
    float Params1[4];
    float Params2[4];
};
static_assert(sizeof(TaaUpscaleParamsCPU) == 208,
              "must match taa_resolve_upscale.comp TaaResolveParams");

// Debug mode: 0 = off, 1 = MV visualization, 2 = history-rejection view.
// Env-latched once (evaluation sessions relaunch with the mode they need).
uint32_t TaaDebugMode()
{
    static const uint32_t kMode = []() -> uint32_t
    {
        if (const char* env = std::getenv("GE_TAA_DEBUG"))
        {
            const int v = std::atoi(env);
            if (v >= 0 && v <= 2)
                return static_cast<uint32_t>(v);
        }
        return 0u;
    }();
    return kMode;
}

} // namespace

bool TemporalAANode::ViewHasMovers(const RenderServices& rs, Rendering::ViewId viewId,
                                   uint64_t worldId)
{
    if (rs.HasUnversionedMotion(viewId))
        return true;
    const auto movers = rs.GetFrameMovers();
    if (movers.empty())
        return false;
    if (worldId == 0u)
        return true;
    const auto* scene = rs.GetGPUScene();
    if (scene == nullptr)
        return true;
    const uint32_t viewWorldKey = Rendering::PackWorldKey16(worldId);
    const auto& instances = scene->GetInstances();
    for (const auto& mover : movers)
    {
        if (mover.InstanceIndex >= instances.size())
            continue;
        if (((instances[mover.InstanceIndex].flags >> 16u) & 0xFFFFu) == viewWorldKey)
            return true;
    }
    return false;
}

float TemporalAANode::HistoryClipMode(bool hasMovers, bool contentChanged, bool stationaryScene)
{
    return hasMovers || contentChanged ? 1.0f : stationaryScene ? -1.0f : 0.0f;
}

bool TemporalAANode::Initialize(std::string nodeId, std::string nodeJson, std::string* /*outError*/)
{
    m_Id = std::move(nodeId);
    try
    {
        auto j = nlohmann::json::parse(nodeJson);
        if (j.is_object())
        {
            if (j.contains("input") && j["input"].is_string())
                m_InputKey = j["input"].get<std::string>();
            if (j.contains("output") && j["output"].is_string())
                m_OutputKey = j["output"].get<std::string>();
        }
    }
    catch (const std::exception&)
    {
        // Keep defaults on malformed config.
    }
    return true;
}

void TemporalAANode::Declare(RenderPipelineInstance& instance, const PipelineDeclareContext& /*ctx*/)
{
    if (auto* dev = instance.GetRenderServices().GetDevice())
        LoadShaders(dev);
}

bool TemporalAANode::ComputeHistoryValid(uint64_t lastWrittenFrame, uint64_t frameIndex,
                                         uint32_t prevOutWidth, uint32_t prevOutHeight,
                                         uint32_t outWidth, uint32_t outHeight,
                                         bool prevCameraValid, bool historyFresh)
{
    // A frame GAP is not staleness. The graph frame index is a per-window
    // stream counter, and an OnDemand view that lapses (hidden tab, collapsed
    // pane, inactive split) resumes with an arbitrary jump in it — requiring
    // lastWrittenFrame + 1 == frameIndex threw away valid surviving history on
    // every such resume. What actually matters is whether the physical about
    // to be read still holds what this view wrote, and that is the pool's
    // answer (historyFresh) — it covers idle age-out, resize realloc and
    // device rebuild alike, none of which a frame counter can see.
    const bool historyRendered = lastWrittenFrame != ~0ull && lastWrittenFrame != frameIndex;
    const bool historyExtentStable = prevOutWidth == outWidth && prevOutHeight == outHeight;
    return historyRendered && historyExtentStable && prevCameraValid && !historyFresh;
}

void TemporalAANode::DeclareForView(ViewDeclare& d)
{
    const RenderGraph::RGTexture input = d.ResolveTexture(m_InputKey);
    // Stitch-through: the output name must resolve for downstream consumers
    // whether TAA runs or not (the FullscreenShader chain binds it
    // unconditionally). Overwritten below when the resolve actually declares.
    if (input.IsValid())
        d.PublishTexture(m_OutputKey, input);

    const auto* taa = d.Services.Views().FindViewAntiAliasing(d.View.id);
    if (taa == nullptr)
        return; // AA mode is not TAA for this view: passthrough
    if (taa->Mode != AntiAliasingMode::TAA)
        return; // FXAA jitters the same raster but resolves in the LDR chain: passthrough here
    if (!input.IsValid())
    {
        if (!m_WarnedUnresolvedInput)
        {
            m_WarnedUnresolvedInput = true;
            Logger::Log::Warning(
                "TemporalAANode '{}': input '{}' does not resolve in this rendergraph; TAA is "
                "inactive",
                m_Id, m_InputKey);
        }
        return;
    }
    if (!m_ShadersLoaded)
        return; // not staged yet: passthrough, retry next frame
    if (!d.ViewDepthResolved.IsValid() || !d.ViewDepth.IsValid())
        return;
    // Letterboxed views render into a sub-rect; the P0 resolve assumes the
    // full extent. Passthrough (documented limitation, game-view aspect
    // presets keep MSAA-or-off semantics for now).
    if (d.Services.Views().GetViewLetterbox(d.View.id).active)
        return;

    const uint64_t frameIndex = d.Frame.FrameIndex();
    const uint32_t w = d.RenderWidth;
    const uint32_t h = d.RenderHeight;
    if (w == 0 || h == 0)
        return;

    // TAAU (render scale < 1): the pre-pass redirected the raster to the
    // internal extent (w, h) and preserved the caller's display targets. The
    // resolve becomes resolve+upscale: history/output at the display extent,
    // the current frame jitter-sampled from the internal extent. Outside TAAU
    // every value below equals the native path.
    const bool taau = d.ViewOutputColor.IsValid() && d.OutputWidth > 0 && d.OutputHeight > 0 &&
                      (d.OutputWidth != w || d.OutputHeight != h);
    const uint32_t outW = taau ? d.OutputWidth : w;
    const uint32_t outH = taau ? d.OutputHeight : h;
    if (taau && !m_TaauShadersLoaded)
    {
        LoadTaauShaders(d.Services.GetDevice());
        if (!m_TaauShadersLoaded)
        {
            // Passthrough already published above: the internal-res input
            // reaches the display-res consumers, which upscale bilinearly —
            // soft but on screen. Retries next frame.
            if (!m_WarnedTaauUnstaged)
            {
                m_WarnedTaauUnstaged = true;
                Logger::Log::Warning(
                    "TemporalAANode '{}': TAAU shaders not staged yet; temporal upscaling "
                    "inactive (bilinear passthrough) until they load",
                    m_Id);
            }
            return;
        }
    }

    // Advance the per-view jitter state (idempotent — the raster seams already
    // rotated this frame) and snapshot what the resolve needs.
    const auto* state = d.Services.Views().AdvanceViewAntiAliasing(d.View.id, frameIndex);
    if (state == nullptr)
        return;

    ViewHistory& vh = m_ViewHistory[static_cast<uint32_t>(d.View.id)];
    // Folded with the pool's freshness answer once the history import below has
    // reported it; declared here because the snapshot fields are overwritten
    // further down.
    const uint64_t prevWrittenFrame = vh.LastWrittenFrame;
    const uint32_t prevOutWidth = vh.OutWidth;
    const uint32_t prevOutHeight = vh.OutHeight;
    const bool prevCameraValid = state->PrevValid;
    const uint32_t parity = static_cast<uint32_t>(vh.HistoryFrame & 1ull);
    vh.HistoryFrame++;
    vh.LastWrittenFrame = frameIndex;
    vh.OutWidth = outW;
    vh.OutHeight = outH;

    // ── Resources ──────────────────────────────────────────────────────────
    const std::string base = "TAA.View" + std::to_string(static_cast<uint32_t>(d.View.id));

    TextureDesc historyDesc{};
    historyDesc.width = outW;
    historyDesc.height = outH;
    historyDesc.mipLevels = 1;
    historyDesc.arrayLayers = 1;
    historyDesc.sampleCount = 1;
    historyDesc.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    historyDesc.usage = static_cast<uint32_t>(TextureUsage::UnorderedAccess) |
                        static_cast<uint32_t>(TextureUsage::ShaderResource);
    historyDesc.persistent = true;
    historyDesc.debugName = "TAA.History";
    const std::string writeName = base + ".History" + std::to_string(parity);
    const std::string readName = base + ".History" + std::to_string(parity ^ 1u);
    const RenderGraph::RGTexture historyWrite =
        d.Frame.ImportPersistentTexture(writeName.c_str(), historyDesc);
    bool historyReadFresh = false;
    const RenderGraph::RGTexture historyRead =
        d.Frame.ImportPersistentTexture(readName.c_str(), historyDesc, &historyReadFresh);
    const bool historyValid =
        ComputeHistoryValid(prevWrittenFrame, frameIndex, prevOutWidth, prevOutHeight, outW, outH,
                            prevCameraValid, historyReadFresh);
    if (!historyWrite.IsValid() || !historyRead.IsValid())
        return;

    const auto mvTex = m_MotionVectors.Declare(d);

    // The output is POOL-backed (not a transient): downstream FullscreenShader
    // consumers bind their inputs by physical handle at declare time, which
    // only pool/imported textures have. Same contract as the blueprint's
    // materialized HDR chain resources.
    TextureDesc outDesc = historyDesc;
    outDesc.debugName = "TAA.Output";
    const RenderGraph::RGTexture outColor =
        d.Frame.ImportPersistentTexture((base + ".Output").c_str(), outDesc);
    if (!mvTex.IsValid() || !outColor.IsValid())
        return;

    // Certified-stationary history: raster jitter can miss a whole subpixel
    // object over finite-depth ground, and depth alone cannot distinguish that
    // from removal. Extraction's world epochs cover structural/material/LOD
    // changes, transforms, vertex animation and skin palettes; light edits also
    // revoke the exemption.
    const auto* viewDesc = d.Services.Views().FindViewDesc(d.View.id);
    const uint64_t worldId = viewDesc != nullptr ? viewDesc->worldId : 0u;
    const bool hasMovers = ViewHasMovers(d.Services, d.View.id, worldId);
    const uint64_t renderVersion = d.Services.RenderContentVersion(worldId);
    const uint64_t casterVersion = d.Services.ShadowCasterContentVersion(worldId);
    const uint64_t lightVersion = d.Services.WorldLightListVersion(worldId);
    // Procedural depth contributors (terrain/ocean/grass) are outside the
    // instance extraction epochs; their frame-spine signal is conservative.
    const uint64_t dynamicEpoch = d.Services.GetIdleElisionFrameState().DepthDynamicEpoch;
    const bool contentUnchanged = historyValid && worldId != 0u && vh.WorldId == worldId &&
        vh.RenderContentVersion == renderVersion &&
        vh.ShadowCasterContentVersion == casterVersion && vh.LightListVersion == lightVersion &&
        vh.DepthDynamicEpoch == dynamicEpoch;
    const bool stationaryScene = contentUnchanged && !hasMovers &&
        vh.RenderWidth == w && vh.RenderHeight == h &&
        std::memcmp(state->CurrCamera.viewProj, state->PrevCamera.viewProj, sizeof(float) * 16) == 0;
    // ── Resolve ────────────────────────────────────────────────────────────
    // Common fields once; the native and TAAU upscale blocks differ only by
    // the extra OutExtent (and the shader that consumes them).
    TaaResolveParamsCPU params{};
    {
        // UNJITTERED inverse view-proj — reprojection must be phase-stable
        // (see the shader-side comment on uInvViewProj).
        GameEngine::Rendering::Matrix4x4 vp;
        std::memcpy(vp.Data(), state->CurrCamera.viewProj, sizeof(float) * 16);
        const GameEngine::Rendering::Matrix4x4 invVp = GameEngine::Mathematics::Inverse(vp);
        std::memcpy(params.InvViewProj, invVp.Data(), sizeof(float) * 16);
        std::memcpy(params.PrevViewProj,
                    state->PrevValid ? state->PrevCamera.viewProj : state->CurrCamera.viewProj,
                    sizeof(float) * 16);
        params.Extent[0] = static_cast<float>(w);
        params.Extent[1] = static_cast<float>(h);
        params.Extent[2] = 1.0f / static_cast<float>(w);
        params.Extent[3] = 1.0f / static_cast<float>(h);
        params.Params0[0] = TunedHistoryWeight();
        params.Params0[1] = historyValid ? 1.0f : 0.0f;
        params.Params0[2] = static_cast<float>(TaaDebugMode());
        params.Params0[3] = TunedVelocityFalloff();
        params.Params1[0] = TunedClipGamma();
        params.Params1[1] = TunedDepthReject();
        params.Params1[2] = TunedSharpen();
        params.Params1[3] = TunedRejectGate();
        // Current-frame jitter as a viewport-UV offset (NDC->UV: x/2, -y/2
        // for the Y-up-NDC to Y-down-viewport convention). Normalized UV, so
        // the same value serves both extents under TAAU.
        static const float kDejitter = TunedDejitter();
        params.Params2[0] = kDejitter * 0.5f * state->NdcJitterX;
        params.Params2[1] = kDejitter * -0.5f * state->NdcJitterY;
        // z = sub-pixel correction toggle (Editor > Experimental), read every
        // tick rather than cached: a checkbox flip should take effect on the
        // next frame, not the next TAA history reset.
        params.Params2[2] = Passes::IsTaaSubpixelCorrectionEnabled() ? 1.0f : 0.0f;
        // Static receivers still change shading when a caster moves. Their zero
        // motion vectors cannot authorize unclipped stationary color history.
        // -1 certifies unchanged content AND camera; 0 uses pixel validation;
        // +1 clips stale coverage on content changes, including deleted objects.
        const bool contentChanged = historyValid && worldId != 0u && !contentUnchanged;
        params.Params2[3] = HistoryClipMode(hasMovers, contentChanged, stationaryScene);
    }

    Rendering::BufferHandle paramsBuffer{};
    uint64_t paramsOffset = 0;
    uint64_t paramsSize = 0;
    if (taau)
    {
        auto alloc = d.Frame.AllocUpload<TaaUpscaleParamsCPU>();
        if (!alloc.Valid())
            return;
        TaaUpscaleParamsCPU up{};
        std::memcpy(up.InvViewProj, params.InvViewProj, sizeof(up.InvViewProj));
        std::memcpy(up.PrevViewProj, params.PrevViewProj, sizeof(up.PrevViewProj));
        std::memcpy(up.Extent, params.Extent, sizeof(up.Extent));
        up.OutExtent[0] = static_cast<float>(outW);
        up.OutExtent[1] = static_cast<float>(outH);
        up.OutExtent[2] = 1.0f / static_cast<float>(outW);
        up.OutExtent[3] = 1.0f / static_cast<float>(outH);
        std::memcpy(up.Params0, params.Params0, sizeof(up.Params0));
        std::memcpy(up.Params1, params.Params1, sizeof(up.Params1));
        std::memcpy(up.Params2, params.Params2, sizeof(up.Params2));
        *alloc.Ptr = up;
        paramsBuffer = alloc.Buffer;
        paramsOffset = alloc.Offset;
        paramsSize = sizeof(TaaUpscaleParamsCPU);
    }
    else
    {
        auto alloc = d.Frame.AllocUpload<TaaResolveParamsCPU>();
        if (!alloc.Valid())
            return;
        *alloc.Ptr = params;
        paramsBuffer = alloc.Buffer;
        paramsOffset = alloc.Offset;
        paramsSize = sizeof(TaaResolveParamsCPU);
    }

    const RenderGraph::RGTexture depthResolved = d.ViewDepthResolved;
    d.Frame.AddComputePass(
        d.PassName("TAAResolve").c_str(), Rendering::PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(input, RenderGraph::RGTextureRead::Sampled);
            p.Read(depthResolved, RenderGraph::RGTextureRead::Sampled);
            p.Read(mvTex, RenderGraph::RGTextureRead::Sampled);
            p.Read(historyRead, RenderGraph::RGTextureRead::Sampled);
            p.Write(historyWrite, RenderGraph::RGTextureWrite::Storage);
            p.Write(outColor, RenderGraph::RGTextureWrite::Storage);
        },
        [this, input, depthResolved, mvTex, historyRead, historyWrite, outColor, paramsBuffer,
         paramsOffset, paramsSize, taau, outW, outH](RenderGraph::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            const ShaderMeta* meta = taau ? m_UpscaleMeta.get() : m_ResolveMeta.get();
            if (!dev || !cl || !meta)
                return;
            const auto inputTex = ctx.GetTexture(input);
            const auto depthTex = ctx.GetTexture(depthResolved);
            const auto mv = ctx.GetTexture(mvTex);
            const auto histR = ctx.GetTexture(historyRead);
            const auto histW = ctx.GetTexture(historyWrite);
            const auto outTex = ctx.GetTexture(outColor);
            if (!inputTex.IsValid() || !depthTex.IsValid() || !mv.IsValid() || !histR.IsValid() ||
                !histW.IsValid() || !outTex.IsValid() || !m_LinearClampSampler.IsValid())
                return;

            DescriptorSetDesc dsDesc{};
            dsDesc.layout = taau ? m_UpscaleSet0Layout : m_ResolveSet0Layout;
            dsDesc.transient = true;
            dsDesc.debugName = "TAAResolve.Set0";
            auto ds = dev->CreateDescriptorSet(dsDesc);
            dev->UpdateStorageImageBinding(ds, 0, histW);
            dev->UpdateStorageImageBinding(ds, 1, outTex);
            {
                // Reflection names the params UBO by INSTANCE ("Taa"), not the
                // block name. TryAdd + warn-once (see the MV pass note).
                NamedDescriptorWriter wd(dev, ds, *meta, 0);
                bool ok = true;
                ok &= wd.TryAddCombinedImageSampler("uSceneColor", inputTex, m_LinearClampSampler);
                ok &= wd.TryAddCombinedImageSampler("uDepth", depthTex, m_LinearClampSampler);
                ok &= wd.TryAddCombinedImageSampler("uMV", mv, m_LinearClampSampler);
                ok &= wd.TryAddCombinedImageSampler("uHistory", histR, m_LinearClampSampler);
                ok &= wd.TryAddUniformBuffer("Taa", paramsBuffer, paramsOffset, paramsSize);
                if (!ok)
                {
                    if (!m_WarnedBindingMismatch)
                    {
                        m_WarnedBindingMismatch = true;
                        Logger::Log::Error(
                            "TemporalAANode: resolve descriptor name mismatch vs taa_resolve "
                            "reflection — dispatch skipped (would run with unwritten "
                            "descriptors)");
                    }
                    return;
                }
                wd.Flush();
            }

            const PipelineHandle pipe =
                ctx.GetOrCreatePipelineVariant(taau ? m_UpscalePipelineId : m_ResolvePipelineId);
            if (!pipe.IsValid())
                return;
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            // Dispatch over the OUTPUT extent (== render extent outside TAAU).
            cl->Dispatch((outW + 7) / 8, (outH + 7) / 8, 1);
        });
    // Mark only now that the resolve pass DECLARING the full-surface write
    // exists: marking before the early-outs above would discharge the pool's
    // freshness arm on a frame that never declared the rewrite, and the next
    // frame would blend against an undefined physical as "resident" history.
    d.Frame.MarkPersistentTextureInitialized(historyWrite);
    vh.WorldId = worldId;
    vh.RenderContentVersion = renderVersion;
    vh.ShadowCasterContentVersion = casterVersion;
    vh.LightListVersion = lightVersion;
    vh.DepthDynamicEpoch = dynamicEpoch;
    vh.RenderWidth = w;
    vh.RenderHeight = h;

    d.PublishTexture(m_OutputKey, outColor);

    if (taau)
    {
        // Redirect the final-output slot to the caller's display-res color:
        // View.Resolve served the world half as the internal SceneColor up to
        // here; from the upscale point on (bloom -> ... -> FinalCopy, and the
        // blueprint's outputs.FinalColor) it must be the display-res target.
        d.PublishTexture(Names::View::Resolve, d.ViewOutputColor);

        // Reconstitute the caller's display-res DEPTH from the internal raster
        // depth: the editor's overlay/gizmo passes attach it for depth testing
        // at display extent.
        m_DepthUpsample.Declare(d.Frame, d.ViewDepth, d.ViewOutputDepth, outW, outH,
                                d.PassName("TAAUDepthUpsample"));
    }
}

TemporalAANode::~TemporalAANode()
{
    if (m_Device && m_LinearClampSampler.IsValid())
        m_Device->DestroySampler(m_LinearClampSampler);
}

void TemporalAANode::LoadShaders(IDevice* device)
{
    if (m_ShadersLoaded)
        return;

    bool resolveOk = m_ResolvePipelineId.IsValid();
    if (!resolveOk)
    {
        ShaderPackage pkg{};
        std::string loadErr;
        if (LoadShaderPkg("Shaders/taa_resolve.shaderpkg", device->PreferredShaderSource(), pkg, &loadErr))
        {
            auto itCs = pkg.stageBytes.find("cs");
            if (itCs != pkg.stageBytes.end() && !itCs->second.empty())
            {
                m_ResolveMeta = std::make_unique<ShaderMeta>(std::move(pkg.meta));
                ComputePipelineDesc cd{};
                cd.ComputeShader =
                    std::make_shared<const std::vector<uint8_t>>(std::move(itCs->second));
                cd.DebugName = "TAAResolve";
                m_ResolveSet0Layout = DescriptorSetLayoutDesc{};
                auto patchLayout = [this](uint32_t setIndex, DescriptorSetLayoutDesc& dsl)
                {
                    if (setIndex != 0)
                        return;
                    // taa_resolve.comp filters uSceneColor (binding 2) and
                    // uHistory (binding 5) through Catmull-Rom texture() taps;
                    // the reflected compute layout defaults them to
                    // UnfilterableFloat and WebGPU rejects the pipeline for
                    // pairing that with their filtering samplers — every
                    // command buffer then dies and the canvas goes black.
                    // Both are rgba16f. uDepth/uMV stay unfilterable:
                    // texelFetch-only, and uDepth is a depth-format view a
                    // filterable layout would reject outright.
                    for (auto& b : dsl.bindings)
                        if (b.binding == 2 || b.binding == 5)
                            b.imageFilterableFloat = true;
                    m_ResolveSet0Layout = dsl;
                };
                std::string err;
                MaterialHelper::ApplyShaderMetaToComputeDesc(*device, *m_ResolveMeta, cd,
                                                             MaterialBuilder::MergeMode::Auto,
                                                             {true, 128}, patchLayout, &err);
                m_ResolvePipelineId = device->InternComputePipeline(std::move(cd));
                resolveOk = m_ResolvePipelineId.IsValid();
            }
            else
                LOG_WARNING("TemporalAANode: taa_resolve.shaderpkg missing cs stage");
        }
        else
            LOG_WARNING("TemporalAANode: failed to load taa_resolve.shaderpkg: {}", loadErr);
    }

    // Latch only on success so a not-yet-staged pkg retries next frame.
    m_ShadersLoaded = resolveOk;

    if (!m_LinearClampSampler.IsValid())
        m_LinearClampSampler =
            device->CreateSampler(SamplerDesc::MaterialLinearClamp("TAA.LinearClamp"));
    m_Device = device;
}

void TemporalAANode::LoadTaauShaders(IDevice* device)
{
    if (m_TaauShadersLoaded || device == nullptr)
        return;

    bool upscaleOk = m_UpscalePipelineId.IsValid();
    if (!upscaleOk)
    {
        ShaderPackage pkg{};
        std::string loadErr;
        if (LoadShaderPkg("Shaders/taa_resolve_upscale.shaderpkg", device->PreferredShaderSource(), pkg, &loadErr))
        {
            auto itCs = pkg.stageBytes.find("cs");
            if (itCs != pkg.stageBytes.end() && !itCs->second.empty())
            {
                m_UpscaleMeta = std::make_unique<ShaderMeta>(std::move(pkg.meta));
                ComputePipelineDesc cd{};
                cd.ComputeShader =
                    std::make_shared<const std::vector<uint8_t>>(std::move(itCs->second));
                cd.DebugName = "TAAResolveUpscale";
                m_UpscaleSet0Layout = DescriptorSetLayoutDesc{};
                auto patchLayout = [this](uint32_t setIndex, DescriptorSetLayoutDesc& dsl)
                {
                    if (setIndex != 0)
                        return;
                    // Same statically-filtered pair as taa_resolve.comp —
                    // see the resolve patchLayout above.
                    for (auto& b : dsl.bindings)
                        if (b.binding == 2 || b.binding == 5)
                            b.imageFilterableFloat = true;
                    m_UpscaleSet0Layout = dsl;
                };
                std::string err;
                MaterialHelper::ApplyShaderMetaToComputeDesc(*device, *m_UpscaleMeta, cd,
                                                             MaterialBuilder::MergeMode::Auto,
                                                             {true, 128}, patchLayout, &err);
                m_UpscalePipelineId = device->InternComputePipeline(std::move(cd));
                upscaleOk = m_UpscalePipelineId.IsValid();
            }
            else
                LOG_WARNING("TemporalAANode: taa_resolve_upscale.shaderpkg missing cs stage");
        }
        else
            LOG_WARNING("TemporalAANode: failed to load taa_resolve_upscale.shaderpkg: {}",
                        loadErr);
    }

    const bool depthUpOk = m_DepthUpsample.EnsureLoaded(device);

    m_TaauShadersLoaded = upscaleOk && depthUpOk;
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes

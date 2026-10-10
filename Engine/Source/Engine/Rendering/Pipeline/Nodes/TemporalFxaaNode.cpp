#include "Engine/Rendering/Pipeline/Nodes/TemporalFxaaNode.h"

#include "Engine/Rendering/AntiAliasing.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderServices.h"

#include "Logger/Logger.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Common/Math.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

namespace
{
// Resolve tuning. Sharpen counters the edge-jitter softening. The two-frame
// merge has no gate: every gate that keyed on how the two jitter positions
// disagree at an edge (colour box, depth tolerance) fired there on alternate
// frames and read as flicker. Env-overridable per launch for A/B sessions.
constexpr float kFxaaSharpenStrength = 0.25f; // negative-lobe unsharp vs the 3x3 mean

float FxaaEnvTunedFloat(const char* name, float fallback, float lo, float hi)
{
    if (const char* env = std::getenv(name))
    {
        const float v = static_cast<float>(std::atof(env));
        if (v >= lo && v <= hi)
            return v;
    }
    return fallback;
}

float FxaaTunedSharpen()
{
    static const float kValue =
        FxaaEnvTunedFloat("GE_FXAA_SHARPEN", kFxaaSharpenStrength, 0.0f, 4.0f);
    return kValue;
}

// De-jitter sign/scale knob for the current-sample reconstruction (default +1;
// -1 flips both axes — the one-variable runtime check for the negative-viewport
// Y convention).
float FxaaTunedDejitter()
{
    static const float kValue = FxaaEnvTunedFloat("GE_FXAA_DEJITTER", 1.0f, -1.0f, 1.0f);
    return kValue;
}

// GLSL std140 mirror of fxaa_resolve.comp FxaaResolveParams.
struct FxaaResolveParamsCPU
{
    float InvViewProj[16];
    float PrevViewProj[16];
    float Extent[4];
    float Params0[4];
    float Params1[4];
};
static_assert(sizeof(FxaaResolveParamsCPU) == 176,
              "must match fxaa_resolve.comp FxaaResolveParams");

ComputePipelineId LoadComputePkg(IDevice& device, const char* pkgPath, const char* debugName,
                                 std::unique_ptr<ShaderMeta>& outMeta,
                                 DescriptorSetLayoutDesc& outSet0)
{
    ShaderPackage pkg{};
    std::string loadErr;
    if (!LoadShaderPkg(pkgPath, device.PreferredShaderSource(), pkg, &loadErr))
    {
        LOG_WARNING("TemporalFxaaNode: failed to load {}: {}", pkgPath, loadErr);
        return {};
    }
    auto itCs = pkg.stageBytes.find("cs");
    if (itCs == pkg.stageBytes.end() || itCs->second.empty())
    {
        LOG_WARNING("TemporalFxaaNode: {} missing cs stage", pkgPath);
        return {};
    }
    outMeta = std::make_unique<ShaderMeta>(std::move(pkg.meta));
    ComputePipelineDesc cd{};
    cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(itCs->second));
    cd.DebugName = debugName;
    outSet0 = DescriptorSetLayoutDesc{};
    auto patchLayout = [&outSet0](uint32_t setIndex, DescriptorSetLayoutDesc& dsl)
    {
        if (setIndex == 0)
            outSet0 = dsl;
    };
    std::string err;
    MaterialHelper::ApplyShaderMetaToComputeDesc(device, *outMeta, cd,
                                                 MaterialBuilder::MergeMode::Auto, {true, 128},
                                                 patchLayout, &err);
    return device.InternComputePipeline(std::move(cd));
}
} // namespace

bool TemporalFxaaNode::Initialize(std::string nodeId, std::string nodeJson, std::string* /*outError*/)
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

void TemporalFxaaNode::Declare(RenderPipelineInstance& instance,
                               const PipelineDeclareContext& /*ctx*/)
{
    if (auto* dev = instance.GetRenderServices().GetDevice())
        LoadShaders(dev);
}

void TemporalFxaaNode::DeclareForView(ViewDeclare& d)
{
    const RenderGraph::RGTexture input = d.ResolveTexture(m_InputKey);
    // Stitch-through: the output name must resolve for FinalCopy whether FXAA
    // runs or not. Overwritten below when the resolve actually declares.
    if (input.IsValid())
        d.PublishTexture(m_OutputKey, input);

    const auto* taa = d.Services.Views().FindViewAntiAliasing(d.View.id);
    if (taa == nullptr ||
        (taa->Mode != AntiAliasingMode::FXAA && taa->Mode != AntiAliasingMode::TemporalFXAA))
        return; // neither FXAA mode owns this view: passthrough
    // Single-frame FXAA is pass 1 alone: no depth, no history, no resolve. It
    // writes the pool-backed output directly and leaves the history bookkeeping
    // untouched, so a later switch to the two-frame mode sees a discontinuous
    // history and rebuilds it rather than reprojecting a stale one.
    const bool spatial = taa->Mode == AntiAliasingMode::FXAA;
    if (!input.IsValid())
    {
        if (!m_WarnedUnresolvedInput)
        {
            m_WarnedUnresolvedInput = true;
            Logger::Log::Warning(
                "TemporalFxaaNode '{}': input '{}' does not resolve in this rendergraph; FXAA is "
                "inactive",
                m_Id, m_InputKey);
        }
        return;
    }
    if (!m_ShadersLoaded)
        return; // not staged yet: passthrough, retry next frame
    // Letterboxed views render into a sub-rect; the resolve assumes full extent.
    if (d.Services.Views().GetViewLetterbox(d.View.id).active)
        return;

    const uint64_t frameIndex = d.Frame.FrameIndex();
    // FXAA runs at the LDR chain's actual extent. With render scaling enabled,
    // RenderScaleUpscale has already crossed from Render* to Output* before
    // this node, so using d.RenderWidth/Height would shrink the finished frame
    // back to the internal resolution.
    const auto& inputDesc = d.Frame.Graph().ResourceDesc(input.Id);
    const uint32_t w = inputDesc.Width;
    const uint32_t h = inputDesc.Height;
    if (w == 0 || h == 0)
        return;

    // Match depth to the same raster domain as the LDR input. The upscale
    // crossing reconstructs ViewOutputDepth for display-resolution consumers.
    RenderGraph::RGTexture depthResolved = d.ViewDepthResolved;
    if (d.ViewOutputDepth.IsValid() && w == d.OutputWidth && h == d.OutputHeight &&
        (d.RenderWidth != d.OutputWidth || d.RenderHeight != d.OutputHeight))
    {
        depthResolved = d.ViewOutputDepth;
    }
    if (!spatial)
    {
        if (!depthResolved.IsValid())
            return;
        const auto& depthDesc = d.Frame.Graph().ResourceDesc(depthResolved.Id);
        if (depthDesc.Width != w || depthDesc.Height != h)
            return;
    }

    const auto* state = d.Services.Views().AdvanceViewAntiAliasing(d.View.id, frameIndex);
    if (state == nullptr)
        return;

    // Validity is decided once the history READ is imported (ComputeHistoryValid
    // needs the pool's freshness answer); the bookkeeping that feeds it is
    // snapshotted here, before this frame advances it.
    bool historyValid = false;
    uint32_t parity = 0u;
    uint64_t historyFrame = 0ull;
    uint32_t prevWidth = 0u;
    uint32_t prevHeight = 0u;
    if (!spatial)
    {
        ViewHistory& vh = m_ViewHistory[static_cast<uint32_t>(d.View.id)];
        historyFrame = vh.HistoryFrame;
        prevWidth = vh.Width;
        prevHeight = vh.Height;
        parity = static_cast<uint32_t>(vh.HistoryFrame & 1ull);
        vh.HistoryFrame++;
        vh.Width = w;
        vh.Height = h;
    }

    // ── Resources ──────────────────────────────────────────────────────────
    const std::string base = "FXAA.View" + std::to_string(static_cast<uint32_t>(d.View.id));

    TextureDesc rgbaDesc{};
    rgbaDesc.width = w;
    rgbaDesc.height = h;
    rgbaDesc.mipLevels = 1;
    rgbaDesc.arrayLayers = 1;
    rgbaDesc.sampleCount = 1;
    rgbaDesc.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    rgbaDesc.usage = static_cast<uint32_t>(TextureUsage::UnorderedAccess) |
                     static_cast<uint32_t>(TextureUsage::ShaderResource);

    // POOL-backed output: FinalCopy (a FullscreenShader) binds its input by
    // physical handle at declare time, which only pool/imported textures have.
    TextureDesc outDesc = rgbaDesc;
    outDesc.persistent = true;
    outDesc.debugName = "FXAA.Output";
    const RenderGraph::RGTexture outColor =
        d.Frame.ImportPersistentTexture((base + ".Output").c_str(), outDesc);
    if (!outColor.IsValid())
        return;

    // FXAA'd current frame. Two-frame: transient, consumed by the resolve this
    // frame. Single-frame: it IS the output, so pass 1 writes the pool texture.
    RenderGraph::RGTexture fxaaTex = outColor;
    RenderGraph::RGTexture historyWrite{};
    RenderGraph::RGTexture historyRead{};
    if (!spatial)
    {
        TextureDesc fxaaDesc = rgbaDesc;
        fxaaDesc.debugName = "FXAA.Edge";
        fxaaTex = d.Frame.CreateTexture((base + ".Edge").c_str(), fxaaDesc);

        // Ping-pong history (persistent): rgb = sharpened LDR, a = raster depth.
        TextureDesc historyDesc = rgbaDesc;
        historyDesc.persistent = true;
        historyDesc.debugName = "FXAA.History";
        const std::string writeName = base + ".History" + std::to_string(parity);
        const std::string readName = base + ".History" + std::to_string(parity ^ 1u);
        historyWrite = d.Frame.ImportPersistentTexture(writeName.c_str(), historyDesc);
        bool historyReadFresh = false;
        historyRead =
            d.Frame.ImportPersistentTexture(readName.c_str(), historyDesc, &historyReadFresh);
        if (!fxaaTex.IsValid() || !historyWrite.IsValid() || !historyRead.IsValid())
            return;
        historyValid = ComputeHistoryValid(historyFrame, prevWidth, prevHeight, w, h,
                                           state->PrevValid, historyReadFresh);
    }

    // ── Pass 1: FXAA (Fast or Quality spatial variant; same bindings) ─────
    const bool useQualityFxaa =
        d.Services.GetFxaaQuality() == Engine::Renderer::FxaaQuality::Quality;
    d.Frame.AddComputePass(
        d.PassName("FXAA").c_str(), Rendering::PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(input, RenderGraph::RGTextureRead::Sampled);
            p.Write(fxaaTex, RenderGraph::RGTextureWrite::Storage);
        },
        [this, input, fxaaTex, w, h, useQualityFxaa](RenderGraph::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            const ShaderMeta* meta = useQualityFxaa ? m_FxaaQualityMeta.get() : m_FxaaMeta.get();
            if (!dev || !cl || !meta)
                return;
            const auto inputTex = ctx.GetTexture(input);
            const auto outTex = ctx.GetTexture(fxaaTex);
            if (!inputTex.IsValid() || !outTex.IsValid() || !m_LinearClampSampler.IsValid())
                return;

            DescriptorSetDesc dsDesc{};
            dsDesc.layout = useQualityFxaa ? m_FxaaQualitySet0Layout : m_FxaaSet0Layout;
            dsDesc.transient = true;
            dsDesc.debugName = "FXAA.Set0";
            auto ds = dev->CreateDescriptorSet(dsDesc);
            dev->UpdateStorageImageBinding(ds, 0, outTex);
            {
                NamedDescriptorWriter wd(dev, ds, *meta, 0);
                if (!wd.TryAddCombinedImageSampler("uSceneColor", inputTex, m_LinearClampSampler))
                {
                    if (!m_WarnedBindingMismatch)
                    {
                        m_WarnedBindingMismatch = true;
                        Logger::Log::Error(
                            "TemporalFxaaNode: fxaa descriptor name mismatch vs reflection — "
                            "dispatch skipped");
                    }
                    return;
                }
                wd.Flush();
            }

            const PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(
                useQualityFxaa ? m_FxaaQualityPipelineId : m_FxaaPipelineId);
            if (!pipe.IsValid())
                return;
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        });

    if (spatial)
    {
        d.PublishTexture(m_OutputKey, outColor);
        return;
    }

    // ── Pass 2: sharpen + reproject/reject resolve ─────────────────────────
    auto resolveAlloc = d.Frame.AllocUpload<FxaaResolveParamsCPU>();
    if (!resolveAlloc.Valid())
        return;
    {
        FxaaResolveParamsCPU params{};
        // UNJITTERED matrices: reprojection must be phase-stable so a static
        // camera maps a pixel to itself exactly (else the image crawls).
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
        params.Params0[0] = historyValid ? 1.0f : 0.0f;
        params.Params0[1] = FxaaTunedSharpen();
        params.Params0[2] = 0.0f; // unused
        params.Params0[3] = 0.0f; // unused
        // De-jitter offset: reconstruct the current colour at the UNJITTERED
        // pixel centre (NDC->UV: x/2, -y/2 for the Y-up-NDC / Y-down-viewport
        // convention). Without it the whole image wobbles by the jitter every
        // frame. Sign knob (GE_FXAA_DEJITTER, -1 flips both axes) is the
        // one-variable runtime check for the negative-viewport convention.
        const float dejitter = FxaaTunedDejitter();
        params.Params1[0] = dejitter * 0.5f * state->NdcJitterX;
        params.Params1[1] = dejitter * -0.5f * state->NdcJitterY;
        *resolveAlloc.Ptr = params;
    }

    // Persistent history/output can become the final exported image when
    // FinalCopy elides. Record the compute dispatch on the graphics queue so
    // the render-graph export batch owns the last touch.
    d.Frame.AddPass(
        d.PassName("FXAAResolve").c_str(), Rendering::PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(fxaaTex, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(depthResolved, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(historyRead, RenderGraph::RGTextureRead::SampledCompute);
            p.Write(historyWrite, RenderGraph::RGTextureWrite::Storage);
            p.Write(outColor, RenderGraph::RGTextureWrite::Storage);
        },
        [this, fxaaTex, depthResolved, historyRead, historyWrite, outColor, resolveAlloc, w,
         h](RenderGraph::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl || !m_ResolveMeta)
                return;
            const auto curTex = ctx.GetTexture(fxaaTex);
            const auto depthTex = ctx.GetTexture(depthResolved);
            const auto histR = ctx.GetTexture(historyRead);
            const auto histW = ctx.GetTexture(historyWrite);
            const auto outTex = ctx.GetTexture(outColor);
            if (!curTex.IsValid() || !depthTex.IsValid() || !histR.IsValid() || !histW.IsValid() ||
                !outTex.IsValid() || !m_LinearClampSampler.IsValid())
                return;

            DescriptorSetDesc dsDesc{};
            dsDesc.layout = m_ResolveSet0Layout;
            dsDesc.transient = true;
            dsDesc.debugName = "FXAAResolve.Set0";
            auto ds = dev->CreateDescriptorSet(dsDesc);
            dev->UpdateStorageImageBinding(ds, 0, histW);
            dev->UpdateStorageImageBinding(ds, 1, outTex);
            {
                NamedDescriptorWriter wd(dev, ds, *m_ResolveMeta, 0);
                bool ok = true;
                ok &= wd.TryAddCombinedImageSampler("uCurrent", curTex, m_LinearClampSampler);
                ok &= wd.TryAddCombinedImageSampler("uDepth", depthTex, m_LinearClampSampler);
                ok &= wd.TryAddCombinedImageSampler("uHistory", histR, m_LinearClampSampler);
                ok &= wd.TryAddUniformBuffer("Fxaa", resolveAlloc.Buffer, resolveAlloc.Offset,
                                             sizeof(FxaaResolveParamsCPU));
                if (!ok)
                {
                    if (!m_WarnedBindingMismatch)
                    {
                        m_WarnedBindingMismatch = true;
                        Logger::Log::Error(
                            "TemporalFxaaNode: resolve descriptor name mismatch vs fxaa_resolve "
                            "reflection — dispatch skipped (would run with unwritten descriptors)");
                    }
                    return;
                }
                wd.Flush();
            }

            const PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(m_ResolvePipelineId);
            if (!pipe.IsValid())
                return;
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        });

    // The resolve rewrites every texel of this parity's history; discharge the
    // pool's freshness arm so the next frame's read of it is trusted.
    d.Frame.MarkPersistentTextureInitialized(historyWrite);
    d.PublishTexture(m_OutputKey, outColor);
}

bool TemporalFxaaNode::ComputeHistoryValid(uint64_t historyFrame, uint32_t prevWidth,
                                           uint32_t prevHeight, uint32_t width, uint32_t height,
                                           bool prevCameraValid, bool historyFresh)
{
    // Continuity means the PREVIOUS RENDER OF THIS VIEW wrote the history, not
    // the previous window frame. The editor advances the frame index for every
    // window frame but re-renders a scene view only when something changed, so
    // a frame-adjacency test would reject all history after every idle gap:
    // the next render shows the raw single-phase image, the one after the
    // other phase, and the two-frame blend never appears while idle — it reads
    // as jitter. PrevCamera is the camera that rendered the history
    // (ViewRegistry advances only when this view declares), so a gap
    // invalidates nothing; reprojection and the depth/colour rejects cover
    // whatever moved in between. Whether the physical still holds what this
    // view wrote is the pool's answer (historyFresh): idle age-out, a resize or
    // usage-widening realloc and a device rebuild all hand back undefined
    // memory under the same name, and no counter can see any of them.
    const bool historyWritten = historyFrame > 0ull;
    const bool extentStable = prevWidth == width && prevHeight == height;
    return historyWritten && extentStable && prevCameraValid && !historyFresh;
}

TemporalFxaaNode::~TemporalFxaaNode()
{
    if (m_Device && m_LinearClampSampler.IsValid())
        m_Device->DestroySampler(m_LinearClampSampler);
}

void TemporalFxaaNode::LoadShaders(IDevice* device)
{
    if (m_ShadersLoaded || device == nullptr)
        return;

    if (!m_FxaaPipelineId.IsValid())
        m_FxaaPipelineId =
            LoadComputePkg(*device, "Shaders/fxaa.shaderpkg", "FXAA", m_FxaaMeta, m_FxaaSet0Layout);
    if (!m_FxaaQualityPipelineId.IsValid())
        m_FxaaQualityPipelineId =
            LoadComputePkg(*device, "Shaders/fxaa_quality.shaderpkg", "FXAAQuality",
                           m_FxaaQualityMeta, m_FxaaQualitySet0Layout);
    if (!m_ResolvePipelineId.IsValid())
        m_ResolvePipelineId = LoadComputePkg(*device, "Shaders/fxaa_resolve.shaderpkg",
                                             "FXAAResolve", m_ResolveMeta, m_ResolveSet0Layout);

    // Latch only on success so a not-yet-staged pkg retries next frame.
    m_ShadersLoaded = m_FxaaPipelineId.IsValid() && m_FxaaQualityPipelineId.IsValid() &&
                      m_ResolvePipelineId.IsValid();

    if (!m_LinearClampSampler.IsValid())
        m_LinearClampSampler =
            device->CreateSampler(SamplerDesc::MaterialLinearClamp("FXAA.LinearClamp"));
    m_Device = device;
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes

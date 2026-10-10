// DDGIProbeFeature's per-view scaled glossy-reflection resolve
// (Shaders/ddgi_glossy_resolve.comp): the probe reflection gather evaluated
// once per resolve pixel instead of once per forward fragment, consumed by
// Includes/ibl.glsl through ge_ddgiResolveRough/ge_ddgiResolveGlossy when the
// C0 volume UBO's uParams2.w is set (GlossyResolveConsumeActive). Split out
// of DDGIProbeFeature.cpp: this is the feature's only per-VIEW declaration —
// everything there is view-independent field maintenance.

#include "Engine/Rendering/DDGIProbeFeature.h"

#include <algorithm>
#include <cstdlib>
#include <string>

#include "Engine/Rendering/Pipeline/Nodes/PipelineNodeUtils.h"  // Pipeline::Nodes::Detail::BindStorageImageByName
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/TextureService.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

namespace GameEngine::Engine::Renderer
{

namespace
{
namespace RG = ::GameEngine::Rendering::RenderGraph;

// Numerator/denominator per Components::DDGIGlossyResolveScale step. Full is
// 1:1, not "no pass": the forward shader has no gather to fall back to, and a
// full-extent resolve is the exact-resolution option.
void ResolveScaleFraction(Components::DDGIGlossyResolveScale scale, uint32_t& outNum,
                          uint32_t& outDen)
{
    switch (scale)
    {
        case Components::DDGIGlossyResolveScale::Quarter:      outNum = 1; outDen = 4; break;
        case Components::DDGIGlossyResolveScale::ThreeQuarter: outNum = 3; outDen = 4; break;
        case Components::DDGIGlossyResolveScale::Full:         outNum = 1; outDen = 1; break;
        case Components::DDGIGlossyResolveScale::Half:
        default:                                               outNum = 1; outDen = 2; break;
    }
}

// Matches ddgi_glossy_resolve.comp's ResolvePush push block.
struct ResolvePush
{
    float DebugInvExposure;
    uint32_t PrevNormalValid;
};

// Matches ddgi_glossy_resolve_blur.comp's BlurParams push block.
struct BlurPush
{
    uint32_t LobesActive;
    float TemporalAlpha;
};

// History retention for the resolve's temporal accumulation (see the blur
// shader). Averages the DDGI solve's per-frame probe-batch bursts out of the
// resolve so a sparse Manual-fit field stops jittering on bright surfaces;
// 0.9 is ~10-frame smoothing, low enough that diffuse GI still tracks a slow
// lighting change. Depth rejection in the shader drops stale history, so this
// only smooths where the surface persists frame to frame. GE_DDGI_RESOLVE_TEMPORAL
// overrides it (0 disables, for A/B and as a kill-switch); read once.
float ResolveTemporalAlpha()
{
    static const float alpha = []
    {
        if (const char* env = std::getenv("GE_DDGI_RESOLVE_TEMPORAL"))
            return std::clamp(static_cast<float>(std::atof(env)), 0.0f, 0.99f);
        return 0.9f;
    }();
    return alpha;
}
}  // namespace

bool DDGIProbeFeature::DeclareGlossyResolveForView(
    Rendering::RenderGraph::RGFrame& frame, RenderServices& services,
    Rendering::BufferHandle viewParams, uint64_t viewParamsOffset, uint64_t viewParamsSize,
    Rendering::RenderGraph::RGTexture viewDepth, uint32_t renderWidth, uint32_t renderHeight,
    uint32_t viewId, float viewExposureScale, const char* passName,
    Rendering::RenderGraph::RGTexture& outRough, Rendering::RenderGraph::RGTexture& outGlossy,
    Rendering::RenderGraph::RGTexture& outIrradiance,
    Rendering::RenderGraph::RGTexture& outShadingNormal)
{
    // NOT gated on EnableGlossy. This pass resolves the diffuse irradiance as
    // well as the two specular lobes, and Includes/ibl.glsl deliberately keeps
    // no per-fragment diffuse fallback — so gating the pass on a specular
    // toggle left every volume that did not opt into glossy with no probe
    // diffuse at all. EnableGlossy gates the lobe blend/upload work and the
    // shader's specular composite (uParams1.z); it must not gate diffuse. On the
    // Compatibility profile this remains the only DDGI diffuse path (ibl.glsl is
    // resolve-only there); running unconditionally covers that case too.
    if (!m_Device)
        return false;
    if (!viewParams.IsValid() || !viewDepth.IsValid() || renderWidth == 0 || renderHeight == 0)
        return false;
    // The kernel reconstructs positions from a sampler2D depth; an MSAA depth
    // (a pipeline without a DepthResolve node) cannot feed it — the view
    // keeps the inline per-fragment gather instead.
    if (frame.Graph().ResourceDesc(viewDepth.Id).SampleCount > 1)
        return false;
    if (!LoadKernel("Shaders/ddgi_glossy_resolve.shaderpkg", m_GlossyResolve, "DDGI.GlossyResolve"))
        return false;
    // The blur is part of the path's quality contract (an unblurred reduced
    // resolve shows square texel steps on curved reflectors), so a missing
    // blur kernel disables the whole scaled path rather than shipping the
    // blocky look.
    if (!LoadKernel("Shaders/ddgi_glossy_resolve_blur.shaderpkg", m_GlossyResolveBlur,
                    "DDGI.GlossyResolveBlur"))
        return false;
    if (!m_GlossyResolveDepthSampler.IsValid())
        m_GlossyResolveDepthSampler = m_Device->CreateSampler(
            Rendering::SamplerDesc::PointClamp("DDGI.GlossyResolve.DepthSampler"));

    // With the specular composite off, the resolve carries irradiance only.
    // Native backends alias the unused lobe bindings to irradiance. Devices
    // that reject writable aliases need distinct placeholders instead; these
    // are only 1x1 because the kernels skip every lobe tap and store.
    // Includes/ibl.glsl reads the lobes only under the same C0 UBO flag.
    // Same expression UploadVolumeData writes into that flag.
    const bool lobes = GlossyLobesActive();
    const bool separateLobeBindings =
        lobes || !m_Device->GetCapabilities().supportsAliasedStorageTextureBindings;

    uint32_t num = 0;
    uint32_t den = 1;
    ResolveScaleFraction(m_Volume.GlossyResolveScale, num, den);
    const uint32_t w = std::max(1u, (renderWidth * num + den / 2) / den);
    const uint32_t h = std::max(1u, (renderHeight * num + den / 2) / den);

    // POOL imports, not transients: both textures enter the world pass's
    // declaration-time binding table (BuildPassResourcesRG needs the physical
    // at declaration) — same reason DepthResolve/GTAO pool their outputs.
    Rendering::TextureDesc td{};
    td.width = w;
    td.height = h;
    td.depth = 1;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.sampleCount = 1;
    td.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
    td.usage = static_cast<uint32_t>(Rendering::TextureUsage::UnorderedAccess |
                                     Rendering::TextureUsage::ShaderResource);
    const std::string irrPool = "DDGIGlossyResolve.Irradiance." + std::to_string(viewId);
    td.debugName = irrPool.c_str();
    const RG::RGTexture irradiance = frame.ImportPersistentTexture(irrPool.c_str(), td);
    // Blurred copies — what the world pass actually samples. The raw gather
    // targets stay private to this file's two passes.
    // The fresh flags say whether the pool kept each buffer's memory. A fresh
    // buffer holds undefined contents (pool memory is never zeroed, and freed
    // memory can hold NaN), so it must never be read as history.
    const std::string irrBlurPool = "DDGIGlossyResolve.IrradianceBlur." + std::to_string(viewId);
    td.debugName = irrBlurPool.c_str();
    bool irradianceBlurFresh = false;
    const RG::RGTexture irradianceBlur = frame.ImportPersistentTexture(irrBlurPool.c_str(), td, &irradianceBlurFresh);
    // Second buffer for the irradiance temporal accumulation: each frame reads
    // the previous result (reprojected) from one and writes this frame's into
    // the other, so the reprojected read can never touch a texel another thread
    // wrote this frame. Ping-pong by frame parity.
    const std::string irrBlurBPool = "DDGIGlossyResolve.IrradianceBlurB." + std::to_string(viewId);
    td.debugName = irrBlurBPool.c_str();
    bool irradianceBlurBFresh = false;
    const RG::RGTexture irradianceBlurB =
        frame.ImportPersistentTexture(irrBlurBPool.c_str(), td, &irradianceBlurBFresh);
    if (!irradiance.IsValid() || !irradianceBlur.IsValid() || !irradianceBlurB.IsValid())
        return false;
    const bool evenFrame = (frame.FrameIndex() & 1ull) == 0ull;
    const RG::RGTexture irrHistoryRead = evenFrame ? irradianceBlur : irradianceBlurB;
    const RG::RGTexture irrHistoryWrite = evenFrame ? irradianceBlurB : irradianceBlur;
    const bool irrHistoryAbsent = evenFrame ? irradianceBlurFresh : irradianceBlurBFresh;

    // Shading-normal history at RENDER extent: the world pass rasterizes its
    // normal MRT slice into one buffer while the resolve reads the other,
    // ping-pong by the same parity as the irradiance history (whose alpha
    // carries the depth the reprojection tests against). Persistent, not
    // transient, so last frame's write survives into this frame. Same
    // format and clear contract as ReflectionsProvider's own slice.
    Rendering::TextureDesc nd{};
    nd.width = renderWidth;
    nd.height = renderHeight;
    nd.depth = 1;
    nd.mipLevels = 1;
    nd.arrayLayers = 1;
    nd.sampleCount = 1;
    nd.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
    nd.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget |
                                     Rendering::TextureUsage::ShaderResource);
    // The fresh flags say whether the pool kept each buffer's memory: a
    // resize or age-out hands back undefined contents under the same name.
    const std::string normalPool = "DDGIGlossyResolve.Normal." + std::to_string(viewId);
    nd.debugName = normalPool.c_str();
    bool normalAFresh = false;
    const RG::RGTexture normalA = frame.ImportPersistentTexture(normalPool.c_str(), nd, &normalAFresh);
    const std::string normalBPool = "DDGIGlossyResolve.NormalB." + std::to_string(viewId);
    nd.debugName = normalBPool.c_str();
    bool normalBFresh = false;
    const RG::RGTexture normalB = frame.ImportPersistentTexture(normalBPool.c_str(), nd, &normalBFresh);
    if (!normalA.IsValid() || !normalB.IsValid())
        return false;
    const RG::RGTexture normalRead = evenFrame ? normalA : normalB;
    const RG::RGTexture normalWrite = evenFrame ? normalB : normalA;
    const bool normalReadFresh = evenFrame ? normalAFresh : normalBFresh;
    const bool prevNormalValid =
        !normalReadFresh && frame.FrameIndex() > 0ull &&
        GlossyResolveNormalWrittenFrame(viewId) == frame.FrameIndex() - 1ull;
    RG::RGTexture rough = irradiance;
    RG::RGTexture glossy = irradiance;
    RG::RGTexture roughBlur = irrHistoryWrite;
    RG::RGTexture glossyBlur = irrHistoryWrite;
    if (separateLobeBindings)
    {
        if (!lobes)
        {
            td.width = 1;
            td.height = 1;
        }
        const std::string roughPool = "DDGIGlossyResolve.Rough." + std::to_string(viewId);
        td.debugName = roughPool.c_str();
        rough = frame.ImportPersistentTexture(roughPool.c_str(), td);
        const std::string glossyPool = "DDGIGlossyResolve.Glossy." + std::to_string(viewId);
        td.debugName = glossyPool.c_str();
        glossy = frame.ImportPersistentTexture(glossyPool.c_str(), td);
        const std::string roughBlurPool = "DDGIGlossyResolve.RoughBlur." + std::to_string(viewId);
        td.debugName = roughBlurPool.c_str();
        roughBlur = frame.ImportPersistentTexture(roughBlurPool.c_str(), td);
        const std::string glossyBlurPool = "DDGIGlossyResolve.GlossyBlur." + std::to_string(viewId);
        td.debugName = glossyBlurPool.c_str();
        glossyBlur = frame.ImportPersistentTexture(glossyBlurPool.c_str(), td);
        if (!rough.IsValid() || !glossy.IsValid() || !roughBlur.IsValid() || !glossyBlur.IsValid())
            return false;
    }

    // Everything the gather include (Includes/ddgi_probes.glsl) binds,
    // resolved with the SAME fallback contract RenderServicesWorldPass uses
    // for the forward pass: black textures / zero-weighted UBO / all-zero
    // probe state whenever a cascade's resources are absent, so the
    // descriptor set is always complete and the UBO enabled flags remain the
    // only sampling gate.
    auto& textures = services.Textures();
    const Rendering::TextureHandle blackTex = textures.GetDefaultBlackTexture();
    const Rendering::SamplerHandle fallbackSampler =
        textures.GetSampler(Rendering::SamplerPreset::LinearClamp);
    Rendering::SamplerHandle atlasSampler = m_AtlasSampler;
    if (!atlasSampler.IsValid())
        atlasSampler = fallbackSampler;
    auto atlasOrBlack = [&](Rendering::TextureHandle atlas)
    { return atlas.IsValid() ? atlas : blackTex; };

    const Rendering::BufferHandle volumeData = UploadVolumeData(m_Device);
    const Rendering::BufferHandle volumeDataFine = UploadVolumeDataFine(m_Device);
    const ProbeStateBinding probeState = GetProbeStateBinding();
    const ProbeStateBinding probeStateFine = GetProbeStateBindingFine();
    const Rendering::TextureHandle irradianceAtlas = atlasOrBlack(m_C0.IrradianceAtlas);
    const Rendering::TextureHandle depthAtlas = atlasOrBlack(m_C0.DepthAtlas);
    const Rendering::TextureHandle irradianceAtlasFine = atlasOrBlack(m_C1.IrradianceAtlas);
    const Rendering::TextureHandle depthAtlasFine = atlasOrBlack(m_C1.DepthAtlas);
    const Rendering::TextureHandle roughAtlas = atlasOrBlack(m_C0Refl.RoughAtlas);
    const Rendering::TextureHandle glossyAtlas = atlasOrBlack(m_C0Refl.GlossyAtlas);
    const Rendering::TextureHandle roughAtlasFine = atlasOrBlack(m_C1Refl.RoughAtlas);
    const Rendering::TextureHandle glossyAtlasFine = atlasOrBlack(m_C1Refl.GlossyAtlas);
    if (!volumeData.IsValid() || !volumeDataFine.IsValid())
        return false;
    // Each cascade's upload passes write the atlases in DDGIGen, recorded
    // before the views, and a cascade (re)allocation clears its probe state
    // with a compute pass in the same frame. Only declared reads order the
    // resolve after them: without the probe-state read the resolve could read
    // the uncleared allocation, and one non-finite frame of irradiance stays
    // in the temporal history for good.
    const GatherReadsRG gatherReads = ImportGatherReads(frame);

    const Rendering::ComputePipelineId pipelineId = m_GlossyResolve.PipelineId;
    const Rendering::DescriptorSetLayoutDesc set0Layout = m_GlossyResolve.Set0Layout;
    const Rendering::ShaderMeta* meta = m_GlossyResolve.Meta.get();
    const Rendering::SamplerHandle depthSampler = m_GlossyResolveDepthSampler;
    // The static (Fixed/Manual/Physical) scale only: the debug overlay is a
    // diagnostic and does not chase auto-exposure adaptation.
    const ResolvePush resolvePush{1.0f / std::max(viewExposureScale, 1.0e-6f),
                                  prevNormalValid ? 1u : 0u};

    frame.AddComputePass(
        passName, Rendering::PassPhase::kDefault,
        [&](RG::RGPassBuilder& p)
        {
            p.Read(viewDepth, RG::RGTextureRead::Sampled);
            // Declared reads even when the kernel is told not to sample them
            // (prevNormalValid 0): the descriptor set stays complete and the
            // graph keeps the persistent textures in a sampled layout.
            p.Read(normalRead, RG::RGTextureRead::Sampled);
            p.Read(irrHistoryRead, RG::RGTextureRead::Sampled);
            for (const RG::RGTexture& atlas : gatherReads.Atlases)
                if (atlas.IsValid())
                    p.Read(atlas, RG::RGTextureRead::Sampled);
            for (const RG::RGBuffer& probeStateRG : gatherReads.ProbeStates)
                if (probeStateRG.IsValid())
                    p.Read(probeStateRG, RG::RGBufferRead::Storage);
            if (separateLobeBindings)
            {
                p.Write(rough, RG::RGTextureWrite::Storage);
                p.Write(glossy, RG::RGTextureWrite::Storage);
            }
            p.Write(irradiance, RG::RGTextureWrite::Storage);
        },
        [viewDepth, rough, glossy, irradiance, normalRead, irrHistoryRead, viewParams,
         viewParamsOffset, viewParamsSize, volumeData,
         volumeDataFine, probeState, probeStateFine, irradianceAtlas, depthAtlas,
         irradianceAtlasFine, depthAtlasFine, roughAtlas, glossyAtlas, roughAtlasFine,
         glossyAtlasFine, atlasSampler, depthSampler, pipelineId, set0Layout, meta, resolvePush,
         w, h](RG::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl || !meta)
                return;
            const auto depthTex = ctx.GetTexture(viewDepth);
            const auto roughTex = ctx.GetTexture(rough);
            const auto glossyTex = ctx.GetTexture(glossy);
            const auto irradianceTex = ctx.GetTexture(irradiance);
            const auto prevNormalTex = ctx.GetTexture(normalRead);
            const auto prevIrradianceTex = ctx.GetTexture(irrHistoryRead);
            if (!depthTex.IsValid() || !roughTex.IsValid() || !glossyTex.IsValid() ||
                !irradianceTex.IsValid() || !prevNormalTex.IsValid() ||
                !prevIrradianceTex.IsValid())
                return;

            Rendering::DescriptorSetDesc dsDesc{};
            dsDesc.layout = set0Layout;
            dsDesc.transient = true;
            dsDesc.debugName = "DDGI.GlossyResolve.Set0";
            auto ds = dev->CreateDescriptorSet(dsDesc);

            Rendering::NamedDescriptorWriter wd(dev, ds, *meta, 0);
            wd.AddCombinedImageSampler("uViewDepth", depthTex, depthSampler);
            // Point-sampled like the depth: an octahedral code and a depth
            // guide both stop being themselves under a bilinear mix.
            wd.AddCombinedImageSampler("uPrevNormal", prevNormalTex, depthSampler);
            wd.AddCombinedImageSampler("uPrevIrradiance", prevIrradianceTex, depthSampler);
            wd.AddUniformBuffer("ViewParams", viewParams, viewParamsOffset, viewParamsSize);
            // Instance names for the UBOs, block names for the SSBOs — what
            // SPIRV-Reflect surfaces (RenderServicesWorldPass's doc on
            // "DDGIVolume"). Try* for the samplers the gather never touches
            // (e.g. the irradiance atlases): the compiler may strip them.
            wd.AddUniformBuffer("DDGIVolume", volumeData, 0, GetVolumeDataSize());
            wd.AddUniformBuffer("DDGIVolumeFine", volumeDataFine, 0, GetVolumeDataSize());
            wd.AddStorageBuffer("DDGIProbeStateRO", probeState.Buffer, 0, probeState.Bytes);
            wd.AddStorageBuffer("DDGIProbeStateFineRO", probeStateFine.Buffer, 0,
                                probeStateFine.Bytes);
            wd.TryAddCombinedImageSampler("ge_ddgiIrradianceAtlas", irradianceAtlas, atlasSampler);
            wd.TryAddCombinedImageSampler("ge_ddgiIrradianceAtlasFine", irradianceAtlasFine,
                                          atlasSampler);
            wd.AddCombinedImageSampler("ge_ddgiDepthAtlas", depthAtlas, atlasSampler);
            wd.AddCombinedImageSampler("ge_ddgiDepthAtlasFine", depthAtlasFine, atlasSampler);
            wd.AddCombinedImageSampler("ge_ddgiRoughAtlas", roughAtlas, atlasSampler);
            wd.AddCombinedImageSampler("ge_ddgiGlossyAtlas", glossyAtlas, atlasSampler);
            wd.AddCombinedImageSampler("ge_ddgiRoughAtlasFine", roughAtlasFine, atlasSampler);
            wd.AddCombinedImageSampler("ge_ddgiGlossyAtlasFine", glossyAtlasFine, atlasSampler);
            wd.Flush();
            Pipeline::Nodes::Detail::BindStorageImageByName(dev, ds, *meta, "uResolveRough",
                                                            roughTex);
            Pipeline::Nodes::Detail::BindStorageImageByName(dev, ds, *meta, "uResolveGlossy",
                                                            glossyTex);
            Pipeline::Nodes::Detail::BindStorageImageByName(dev, ds, *meta, "uResolveIrradiance",
                                                            irradianceTex);

            Rendering::PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(pipelineId);
            if (!pipe.IsValid())
                return;
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            cl->SetPushConstants(resolvePush);
            cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        });

    // 3x3 tent over both lobes at resolve resolution (see
    // ddgi_glossy_resolve_blur.comp for why the pre-blur exists). One
    // dispatch handles both textures.
    const Rendering::ComputePipelineId blurPipelineId = m_GlossyResolveBlur.PipelineId;
    const Rendering::DescriptorSetLayoutDesc blurSet0Layout = m_GlossyResolveBlur.Set0Layout;
    const Rendering::ShaderMeta* blurMeta = m_GlossyResolveBlur.Meta.get();
    // RGBA16F supports filtering on every backend. Preserve the fractional
    // blur taps and history reprojection; only the depth guide uses point reads.
    const Rendering::SamplerHandle blurSampler = fallbackSampler;
    const std::string blurPassName = std::string(passName) + "Blur";
    frame.AddComputePass(
        blurPassName.c_str(), Rendering::PassPhase::kDefault,
        [&](RG::RGPassBuilder& p)
        {
            if (separateLobeBindings)
            {
                p.Read(rough, RG::RGTextureRead::Sampled);
                p.Read(glossy, RG::RGTextureRead::Sampled);
            }
            // The kernel rejects taps whose depth disagrees with the centre's,
            // so it needs the same 1-sample depth the resolve reconstructed from.
            p.Read(viewDepth, RG::RGTextureRead::Sampled);
            p.Read(irradiance, RG::RGTextureRead::Sampled);
            if (separateLobeBindings)
            {
                // Lobes take the spatial blur only (reflections would ghost).
                p.Write(roughBlur, RG::RGTextureWrite::Storage);
                p.Write(glossyBlur, RG::RGTextureWrite::Storage);
            }
            // Reproject last frame's irradiance (read) into this frame's buffer
            // (write); the two are distinct so the read is race-free.
            p.Read(irrHistoryRead, RG::RGTextureRead::Sampled);
            p.Write(irrHistoryWrite, RG::RGTextureWrite::Storage);
        },
        [rough, glossy, irradiance, roughBlur, glossyBlur, irrHistoryRead, irrHistoryWrite,
         viewParams, viewParamsOffset, viewParamsSize, viewDepth, depthSampler,
         blurPipelineId, blurSet0Layout, blurMeta, blurSampler, lobes, irrHistoryAbsent, w,
         h](RG::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl || !blurMeta)
                return;
            const auto roughIn = ctx.GetTexture(rough);
            const auto glossyIn = ctx.GetTexture(glossy);
            const auto roughOut = ctx.GetTexture(roughBlur);
            const auto glossyOut = ctx.GetTexture(glossyBlur);
            const auto depthTex = ctx.GetTexture(viewDepth);
            const auto irradianceIn = ctx.GetTexture(irradiance);
            const auto historyIn = ctx.GetTexture(irrHistoryRead);
            const auto irradianceOut = ctx.GetTexture(irrHistoryWrite);
            if (!roughIn.IsValid() || !glossyIn.IsValid() || !roughOut.IsValid() ||
                !glossyOut.IsValid() || !depthTex.IsValid() || !irradianceIn.IsValid() ||
                !historyIn.IsValid() || !irradianceOut.IsValid())
                return;

            Rendering::DescriptorSetDesc dsDesc{};
            dsDesc.layout = blurSet0Layout;
            dsDesc.transient = true;
            dsDesc.debugName = "DDGI.GlossyResolveBlur.Set0";
            auto ds = dev->CreateDescriptorSet(dsDesc);
            Rendering::NamedDescriptorWriter wd(dev, ds, *blurMeta, 0);
            wd.AddCombinedImageSampler("uRoughIn", roughIn, blurSampler);
            wd.AddCombinedImageSampler("uGlossyIn", glossyIn, blurSampler);
            wd.AddCombinedImageSampler("uViewDepth", depthTex, depthSampler);
            wd.AddCombinedImageSampler("uIrradianceIn", irradianceIn, blurSampler);
            wd.AddCombinedImageSampler("uHistoryIrr", historyIn, blurSampler);
            wd.AddUniformBuffer("ViewParams", viewParams, viewParamsOffset, viewParamsSize);
            wd.Flush();
            Pipeline::Nodes::Detail::BindStorageImageByName(dev, ds, *blurMeta, "uRoughOut",
                                                            roughOut);
            Pipeline::Nodes::Detail::BindStorageImageByName(dev, ds, *blurMeta, "uGlossyOut",
                                                            glossyOut);
            Pipeline::Nodes::Detail::BindStorageImageByName(dev, ds, *blurMeta, "uIrradianceOut",
                                                            irradianceOut);

            Rendering::PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(blurPipelineId);
            if (!pipe.IsValid())
                return;
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            // No history to blend on a fresh buffer: the frame is the pure spatial blur.
            const BlurPush pc{lobes ? 1u : 0u, irrHistoryAbsent ? 0.0f : ResolveTemporalAlpha()};
            cl->SetPushConstants(pc);
            cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        });

    // The blur stores every texel of the irradiance target, so this frame's
    // write is the buffer's full contents and discharges its fresh arm.
    frame.MarkPersistentTextureInitialized(irrHistoryWrite);

    outRough = roughBlur;
    outGlossy = glossyBlur;
    outIrradiance = irrHistoryWrite;
    outShadingNormal = normalWrite;
    return true;
}

void DDGIProbeFeature::MarkGlossyResolveNormalWritten(uint32_t viewId, uint64_t frameIndex)
{
    for (auto& v : m_GlossyResolveViews)
    {
        if (v.ViewId == viewId)
        {
            v.NormalWrittenFrame = frameIndex;
            return;
        }
    }
    m_GlossyResolveViews.push_back({viewId, frameIndex});
}

uint64_t DDGIProbeFeature::GlossyResolveNormalWrittenFrame(uint32_t viewId) const
{
    for (const auto& v : m_GlossyResolveViews)
        if (v.ViewId == viewId)
            return v.NormalWrittenFrame;
    return 0ull;
}

}  // namespace GameEngine::Engine::Renderer

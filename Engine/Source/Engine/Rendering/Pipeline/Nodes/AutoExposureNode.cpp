#include "Engine/Rendering/Pipeline/Nodes/AutoExposureNode.h"

#include "Engine/Rendering/ExposureReadbackFeature.h"
#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/RenderServices.h"

#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/NamedPushConstantWriter.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <nlohmann/json.hpp>

#include <cstdint>

namespace GameEngine::Engine::Renderer { using namespace ::GameEngine::Rendering; }

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

namespace
{
constexpr uint32_t kBinCount = 256u;
// Two histograms, geometry then sky (far plane); layout owned by auto_exposure_histogram.comp.
constexpr uint32_t kHistogramCount = 2u;
constexpr uint32_t kHistogramBytes = kHistogramCount * kBinCount * sizeof(uint32_t); // 2048
// {float scale; uint valid; float pad0,pad1} — layout owned by the CPU mirror.
constexpr uint32_t kExposureStateBytes = ExposureReadbackFeature::kStateBytes;
constexpr float kMaxAdaptDt = 0.1f;                                // clamp dt so a hitch can't snap exposure

// The canonical publish name. New rendergraphs declare "ExposureHistory" as a blueprint resource
// (perView 16-byte Storage buffer, zeroOnCreate) and consumers materialize it on first resolve;
// this node's own import below exists only for rendergraphs that predate the resource entry.
constexpr const char* kExposurePublishName = Names::Res::ExposureHistory;
constexpr const char* kHistoryPrefix = "Pipeline.AutoExposure.History.View"; // + per-view id suffix

// Legacy-graph fallback: import the persistent per-view exposure-history buffer and publish it as
// "ExposureHistory". Persistent (never transient) because transient pool buffers alias across
// frames and would corrupt the cross-frame adaptation state; zero-filled on creation so a reader
// can never observe recycled pool garbage as a plausible exposure. Returns an invalid RGBuffer if
// the import fails.
RenderGraph::RGBuffer ImportAndPublishExposureHistory(ViewDeclare& d)
{
    const std::string viewSuffix = std::to_string(static_cast<uint32_t>(d.View.id));

    Rendering::BufferDesc expDesc{};
    expDesc.size = kExposureStateBytes;
    expDesc.usage = static_cast<uint32_t>(BufferUsage::Storage) |
                    static_cast<uint32_t>(BufferUsage::TransferDst);
    expDesc.memoryUsage = BufferMemoryUsage::DeviceLocal;
    const std::string expName = std::string(kHistoryPrefix) + viewSuffix;
    expDesc.debugName = expName.c_str();
    bool needsZeroInit = false;
    const RenderGraph::RGBuffer exposure =
        d.Frame.ImportPersistentBuffer(expName.c_str(), expDesc, &needsZeroInit);
    if (!exposure.IsValid())
        return {};
    if (needsZeroInit)
        d.Frame.AddBufferZeroInit(exposure, ("ZeroInit." + expName).c_str());

    PipelineBufferBindingRG eb{};
    eb.Buffer = d.Frame.PhysicalBuffer(exposure);
    eb.Offset = 0;
    eb.Size = kExposureStateBytes;
    eb.Graph = exposure;
    d.PublishBuffer(kExposurePublishName, eb);
    return exposure;
}
}

bool AutoExposureNode::Initialize(std::string nodeId, std::string nodeJson, std::string* /*outError*/)
{
    m_Id = std::move(nodeId);
    try
    {
        auto j = nlohmann::json::parse(nodeJson);
        if (j.is_object() && j.contains("input") && j["input"].is_string())
            m_InputKey = j["input"].get<std::string>();
    }
    catch (const std::exception&)
    {
        // Keep defaults on malformed config.
    }
    return true;
}

void AutoExposureNode::Declare(RenderPipelineInstance& instance, const PipelineDeclareContext& ctx)
{
    // DeclareForView carries no dt, so latch it here (Declare runs once/frame before the view loop).
    m_DeltaTimeSeconds = ctx.DeltaTimeSeconds;
    if (auto* dev = instance.GetRenderServices().GetDevice())
        LoadShaders(dev);
}

void AutoExposureNode::DeclareForView(ViewDeclare& d)
{
    // The exposure-history buffer must always exist as a resolvable resource, even when
    // auto-exposure is off: the Tonemap node unconditionally declares it, and FullscreenShaderNode
    // SKIPS ITS ENTIRE STAGE if a declared buffer fails to resolve — so a missing buffer would drop
    // tonemapping on every scene. Rendergraphs declare "ExposureHistory" in their resources section
    // (materialized on first resolve, order-independent); for older graphs without the entry, fall
    // back to importing + publishing it here. The buffer is PERSISTENT either way: transient pool
    // buffers alias across frames and would corrupt the cross-frame adaptation state.
    RenderGraph::RGBuffer exposure;
    const PipelineBufferBindingRG resolved = d.ResolveBuffer(kExposurePublishName);
    if (resolved.IsValid() && resolved.Graph.IsValid())
    {
        exposure = resolved.Graph; // blueprint resource (or an earlier consumer's materialization)
        // 1b under-size guard: a blueprint that declares "ExposureHistory" smaller
        // than this node's fixed state struct would silently truncate the
        // adaptation state on write. The materializer sizes buffers from the
        // blueprint and can't know a consumer's stride, so the consuming node is
        // the practical authority. Warn-once (persistent, so it can't spam per
        // frame) keyed on the single buffer this node consumes.
        if (resolved.Size < kExposureStateBytes && !m_WarnedExposureHistoryUnderSize)
        {
            m_WarnedExposureHistoryUnderSize = true;
            Logger::Log::Warning(
                "AutoExposureNode '{}': resolved '{}' buffer is {} bytes but the exposure state "
                "needs {} — declared size is too small; adaptation state will be truncated",
                m_Id, kExposurePublishName, resolved.Size, kExposureStateBytes);
        }
    }
    else
        exposure = ImportAndPublishExposureHistory(d); // no resource entry: own it (legacy graphs)
    if (!exposure.IsValid())
        return;

    // The metering passes only run when auto-exposure is the active mode (and the shaders loaded /
    // the HDR source exists). When off, the published buffer is simply not updated and the tonemap
    // ignores it (useAutoExposure == 0).
    if (!m_HistogramPipelineId.IsValid() || !m_ResolvePipelineId.IsValid())
        return;
    const PostProcessSettings settings =
        d.Services.GetEffectivePostProcessSettings(d.View.id, d.View.worldId);
    if (!settings.IsAutoExposureActive())
        return;

    // Meter the configured input (pre-exposure, so the histogram sees un-exposed scene luminance —
    // no exposure feedback loop). Shipped graphs point this at the last pre-bloom chain value so
    // bloom energy and HDR grading don't bias adaptation.
    const RenderGraph::RGTexture hdr = d.ResolveTexture(m_InputKey);
    if (!hdr.IsValid())
    {
        // Loud once: a typo'd/missing input silently freezing eye adaptation
        // is otherwise undiagnosable from the outside.
        if (!m_WarnedUnresolvedInput)
        {
            m_WarnedUnresolvedInput = true;
            Logger::Log::Warning(
                "AutoExposureNode '{}': metering input '{}' does not resolve in this rendergraph; "
                "auto-exposure will not update",
                m_Id, m_InputKey);
        }
        return;
    }
    // The view's resolved depth splits the frame into geometry and sky (the far plane). The depth
    // after the world and the water when the ocean surface step or the fog resolved it this frame
    // (the sea is scene, not sky), else the view's resolved depth, which every shipped rendergraph
    // publishes.
    RenderGraph::RGTexture depth = d.ResolveTexture(Names::View::DepthResolvedPostOcean);
    if (!depth.IsValid())
        depth = d.ResolveTexture(Names::View::DepthResolved);
    if (!depth.IsValid())
    {
        if (!m_WarnedUnresolvedDepth)
        {
            m_WarnedUnresolvedDepth = true;
            Logger::Log::Warning(
                "AutoExposureNode '{}': '{}' does not resolve in this rendergraph; auto-exposure "
                "needs the view depth to tell the sky from the scene and will not update",
                m_Id, Names::View::DepthResolved);
        }
        return;
    }
    const auto& hd = d.Frame.Graph().ResourceDesc(hdr.Id);
    const uint32_t w = hd.Width;
    const uint32_t h = hd.Height;
    if (w == 0 || h == 0)
        return;

    // Per-view histogram (also persistent; zero-filled at the top of the histogram
    // pass each frame, so first-use garbage and pool-recycled contents never meter).
    const std::string viewSuffix = std::to_string(static_cast<uint32_t>(d.View.id));
    Rendering::BufferDesc histDesc{};
    histDesc.size = kHistogramBytes;
    // TransferDst: zero-filled via FillBuffer at the top of the histogram pass.
    histDesc.usage = static_cast<uint32_t>(BufferUsage::Storage) |
                     static_cast<uint32_t>(BufferUsage::TransferDst);
    histDesc.memoryUsage = BufferMemoryUsage::DeviceLocal;
    const std::string histName = m_HistogramPrefix + viewSuffix;
    histDesc.debugName = histName.c_str();
    const RenderGraph::RGBuffer histogram = d.Frame.ImportPersistentBuffer(histName.c_str(), histDesc);
    if (!histogram.IsValid())
        return;

    const float dt = m_DeltaTimeSeconds;

    // Pass 1: HDR colour + depth -> geometry and sky log-luminance histograms (atomicAdd into the
    // persistent buffer).
    d.Frame.AddComputePass(
        d.PassName("AutoExposureHistogram").c_str(), Rendering::PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(hdr, RenderGraph::RGTextureRead::Sampled);
            p.Read(depth, RenderGraph::RGTextureRead::Sampled);
            p.Write(histogram, RenderGraph::RGBufferWrite::Storage);
        },
        [this, hdr, depth, histogram, w, h](RenderGraph::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl)
                return;
            const auto hdrTex = ctx.GetTexture(hdr);
            const auto depthTex = ctx.GetTexture(depth);
            const auto histBuf = ctx.GetBuffer(histogram);
            if (!hdrTex.IsValid() || !depthTex.IsValid() || !histBuf.IsValid() || !m_Sampler.IsValid())
                return; // skip rather than dispatch with an unwritten texture binding

            // Zero the histogram before accumulating: the persistent pool buffer is
            // never zero-initialized on creation and can be recycled from an evicted
            // view with stale counts — an end-of-frame clear cannot cover either.
            cl->Barrier(Rendering::ResourceBarrier::CreateBufferBarrier(
                histBuf, Rendering::ResourceState::UnorderedAccess,
                Rendering::ResourceState::CopyDest));
            cl->FillBuffer(histBuf, 0, kHistogramBytes, 0u);
            cl->Barrier(Rendering::ResourceBarrier::CreateBufferBarrier(
                histBuf, Rendering::ResourceState::CopyDest,
                Rendering::ResourceState::UnorderedAccess));

            DescriptorSetDesc dsDesc{};
            dsDesc.layout = m_HistogramSet0Layout;
            dsDesc.transient = true;
            dsDesc.debugName = "AutoExposureHistogram.Set0";
            auto ds = dev->CreateDescriptorSet(dsDesc);

            if (m_HistogramMeta)
            {
                NamedDescriptorWriter wd(dev, ds, *m_HistogramMeta, 0);
                wd.AddCombinedImageSampler("uHDRColor", hdrTex, m_Sampler);
                wd.AddCombinedImageSampler("uSceneDepth", depthTex, m_Sampler);
                wd.AddStorageBuffer("HistogramBuffer", histBuf, 0, kHistogramBytes);
                wd.Flush();
            }

            PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(m_HistogramPipelineId);
            if (!pipe.IsValid())
                return;
            cl->SetPipeline(pipe);
            if (m_HistogramMeta && !m_HistogramMeta->PushConstants.empty())
            {
                NamedPushConstantWriter pcw(*m_HistogramMeta, m_HistogramMeta->PushConstants[0].Name);
                if (pcw.IsValid())
                {
                    pcw.Add("pixelCountX", w);
                    pcw.Add("pixelCountY", h);
                    pcw.Flush(cl);
                }
            }
            cl->BindDescriptorSet(0, ds, pipe);
            cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        });

    // Pass 2: histograms -> metered average (geometry, with the sky as a ceiling) -> adapted LINEAR
    // exposure (single thread). Reads the histograms + previous exposure, writes the new exposure.
    d.Frame.AddComputePass(
        d.PassName("AutoExposureResolve").c_str(), Rendering::PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(histogram, RenderGraph::RGBufferRead::Storage);
            p.Read(exposure, RenderGraph::RGBufferRead::Storage);
            p.Write(exposure, RenderGraph::RGBufferWrite::Storage);
        },
        [this, histogram, exposure, settings, dt](RenderGraph::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl)
                return;
            const auto histBuf = ctx.GetBuffer(histogram);
            const auto expBuf = ctx.GetBuffer(exposure);
            if (!histBuf.IsValid() || !expBuf.IsValid())
                return;

            DescriptorSetDesc dsDesc{};
            dsDesc.layout = m_ResolveSet0Layout;
            dsDesc.transient = true;
            dsDesc.debugName = "AutoExposureResolve.Set0";
            auto ds = dev->CreateDescriptorSet(dsDesc);
            if (m_ResolveMeta)
            {
                NamedDescriptorWriter wd(dev, ds, *m_ResolveMeta, 0);
                wd.AddStorageBuffer("HistogramBuffer", histBuf, 0, kHistogramBytes);
                wd.AddStorageBuffer("state", expBuf, 0, kExposureStateBytes);
                wd.Flush();
            }

            PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(m_ResolvePipelineId);
            if (!pipe.IsValid())
                return;
            cl->SetPipeline(pipe);
            if (m_ResolveMeta && !m_ResolveMeta->PushConstants.empty())
            {
                NamedPushConstantWriter pcw(*m_ResolveMeta, m_ResolveMeta->PushConstants[0].Name);
                if (pcw.IsValid())
                {
                    const float clampedDt = dt < 0.0f ? 0.0f : (dt > kMaxAdaptDt ? kMaxAdaptDt : dt);
                    for (const auto& member : m_ResolveMeta->PushConstants[0].Block.Members)
                    {
                        if (member.Name == "deltaTime")
                            pcw.Add("deltaTime", clampedDt);
                        else
                            (void)settings.TryWriteField(member.Name, pcw);
                    }
                    pcw.Flush(cl);
                }
            }
            cl->BindDescriptorSet(0, ds, pipe);
            cl->Dispatch(1, 1, 1);
        });

    // Opted-in views only (the editor Scene View's Fixed-EV handoff): copy the
    // adaptation state into the feature's readback ring so the CPU can observe
    // the metered exposure. Declared after the resolve so the copy reads THIS
    // frame's adapted value; the ring slot is host-visible and resolves on the
    // submission token (RGReadbackRing contract).
    if (auto* readback = d.Services.GetFeature<ExposureReadbackFeature>();
        readback && readback->IsReadbackEnabled(d.View.id))
    {
        if (const Rendering::BufferHandle slot =
                readback->AcquireSlotRG(d.Services.GetDevice(), d.Frame, d.View.id);
            slot.IsValid())
        {
            d.Frame.AddPass(
                d.PassName("AutoExposureReadback").c_str(), Rendering::PassPhase::kDefault,
                [&](RenderGraph::RGPassBuilder& p)
                {
                    p.Read(exposure, RenderGraph::RGBufferRead::CopySrc);
                    // CPU-consumed output — the ring slot is not a graph resource.
                    p.PreventCulling();
                },
                [exposure, slot](RenderGraph::RGContext& ctx)
                {
                    auto* cl = ctx.Cmd;
                    if (!cl)
                        return;
                    const auto src = ctx.GetBuffer(exposure);
                    if (src.IsValid())
                        cl->CopyBuffer(src, slot, kExposureStateBytes);
                });
        }
    }
}

void AutoExposureNode::LoadShaders(IDevice* device)
{
    if (m_ShadersLoaded)
        return;

    auto loadCompute = [&](const char* path,
                           ComputePipelineId& outPipeline,
                           std::unique_ptr<ShaderMeta>& outMeta,
                           DescriptorSetLayoutDesc& outLayout,
                           const char* debugName) -> bool
    {
        ShaderPackage pkg{};
        std::string loadErr;
        if (!LoadShaderPkg(path, device->PreferredShaderSource(), pkg, &loadErr))
        {
            LOG_WARNING("AutoExposureNode: failed to load {}: {}", path, loadErr);
            return false;
        }
        auto itCs = pkg.stageBytes.find("cs");
        if (itCs == pkg.stageBytes.end() || itCs->second.empty())
        {
            LOG_WARNING("AutoExposureNode: {} missing cs stage", path);
            return false;
        }

        outMeta = std::make_unique<ShaderMeta>(std::move(pkg.meta));

        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(itCs->second));
        cd.DebugName = debugName;

        outLayout = DescriptorSetLayoutDesc{};
        auto patchLayout = [&](uint32_t setIndex, DescriptorSetLayoutDesc& dsl) {
            if (setIndex == 0)
                outLayout = dsl;
        };
        std::string err;
        MaterialHelper::ApplyShaderMetaToComputeDesc(
            *device, *outMeta, cd, MaterialBuilder::MergeMode::Auto,
            {true, 128}, patchLayout, &err);

        outPipeline = device->InternComputePipeline(std::move(cd));
        return true;
    };

    loadCompute("Shaders/auto_exposure_histogram.shaderpkg", m_HistogramPipelineId, m_HistogramMeta,
                m_HistogramSet0Layout, "AutoExposureHistogram");
    loadCompute("Shaders/auto_exposure_resolve.shaderpkg", m_ResolvePipelineId, m_ResolveMeta,
                m_ResolveSet0Layout, "AutoExposureResolve");

    // Latch only on success: a shaderpkg that isn't staged yet on the first
    // frame (startup race) must retry next frame, not silently disable
    // auto-exposure until restart.
    m_ShadersLoaded = m_HistogramPipelineId.IsValid() && m_ResolvePipelineId.IsValid();

    if (!m_Sampler.IsValid())
        m_Sampler = device->CreateSampler(SamplerDesc::PointClamp("AutoExposure.Sampler"));
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes

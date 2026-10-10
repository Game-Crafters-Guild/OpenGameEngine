#include "Engine/Rendering/Pipeline/Nodes/AONode.h"

#include "Engine/Rendering/Pipeline/Nodes/PipelineNodeUtils.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/NamedPushConstantWriter.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <initializer_list>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

namespace
{
enum class Access
{
    Sampled,
    StorageRead,
    StorageWrite
};
struct Binding
{
    const char* Name;
    RenderGraph::RGTexture Texture;
    Access Usage;
    uint32_t Mip = ~0u;
};
// The temporal pass binds the most: current, guide, depth, history, history
// surface and the two outputs.
constexpr size_t kMaxBindings = 7;
struct PassBindings
{
    std::array<Binding, kMaxBindings> Items{};
    size_t Count = 0;
    PassBindings(std::initializer_list<Binding> bindings)
    {
        assert(bindings.size() <= Items.size());
        for (const auto& binding : bindings)
            Items.at(Count++) = binding;
    }
};
// GtaoQualitySettings::MaxDepthMip caps the chain, so mip pass names are literals.
constexpr const char* kDepthMipPassNames[] = {"GTAODepthMip1", "GTAODepthMip2", "GTAODepthMip3",
                                             "GTAODepthMip4", "GTAODepthMip5"};
constexpr char kPoolPrefix[] = "Pipeline.GTAO.View";

bool CameraContinuous(const CameraData& previous, const CameraData& current, float radius)
{
    float distanceSquared = 0.0f;
    for (size_t i = 0; i < 3; ++i)
    {
        const float delta = current.cameraPos[i] - previous.cameraPos[i];
        distanceSquared += delta * delta;
    }
    const float maxTranslation = std::max(2.0f * radius, 1.0f);
    if (distanceSquared > maxTranslation * maxTranslation)
        return false;
    // Projection changes invalidate depth interpretation. Stored cameras are
    // unjittered, so the usual TAA sample sequence does not trigger this gate.
    for (size_t i = 0; i < 16; ++i)
        if (std::abs(previous.proj[i] - current.proj[i]) > 1e-5f)
            return false;
    float rotationTrace = 0.0f;
    for (size_t column = 0; column < 3; ++column)
        for (size_t row = 0; row < 3; ++row)
            rotationTrace += previous.view[column * 4 + row] * current.view[column * 4 + row];
    return (rotationTrace - 1.0f) * 0.5f > 0.9f;
}
} // namespace

bool AONode::Initialize(std::string nodeId, std::string nodeJson, std::string* outError)
{
    m_Id = std::move(nodeId);
    try
    {
        const auto j = nlohmann::json::parse(nodeJson);
        m_OutputKey = j.value("output", std::string(Names::View::GTAO));
        m_Temporal = j.value("temporal", true);
        if (!ParseGtaoQuality(j.value("quality", std::string("medium")), m_Quality))
        {
            if (outError)
                *outError = "GTAO quality must be medium, high or ultra";
            return false;
        }
    }
    catch (const std::exception& e)
    {
        if (outError)
            *outError = std::string("Invalid GTAO settings: ") + e.what();
        return false;
    }
    return true;
}

void AONode::Declare(RenderPipelineInstance& instance, const PipelineDeclareContext&)
{
    if (auto* dev = instance.GetRenderServices().GetDevice())
        LoadShaders(dev);
}

AONode::ViewState& AONode::ViewStateFor(uint32_t viewId)
{
    auto [it, inserted] = m_Views.try_emplace(viewId);
    if (inserted)
    {
        const std::string base = kPoolPrefix + std::to_string(viewId);
        it->second.OutputPool = base;
        it->second.DepthMipsPool = base + ".DepthMips";
        it->second.HistoryPool = {base + ".History0", base + ".History1"};
        it->second.SurfacePool = {base + ".Surface0", base + ".Surface1"};
    }
    return it->second;
}

void AONode::DeclareForView(ViewDeclare& d)
{
    const auto quality = GetGtaoQualitySettings(m_Quality);
    if (!m_Sampler.IsValid())
        return;
    for (size_t i = 0; i < StageCount; ++i)
    {
        if ((i == Temporal && !m_Temporal) || (i == Upsample && quality.ResolutionDivisor == 1))
            continue;
        if (!m_Shaders[i].Pipeline.IsValid())
            return;
    }
    const auto settings = d.Services.GetEffectivePostProcessSettings(d.View.id, d.View.worldId);
    const uint32_t viewId = static_cast<uint32_t>(d.View.id);
    ViewState& view = ViewStateFor(viewId);
    if (!settings.IsAOActive())
    {
        view.Committed = {};
        return;
    }
    // The occluders' depth: grass heads neither occlude GTAO nor receive it (DepthResolveNode).
    const auto depth = d.ViewOccluderDepthResolved;
    if (!depth.IsValid())
        return;
    const auto& dd = d.Frame.Graph().ResourceDesc(depth.Id);
    const uint32_t w = dd.Width;
    const uint32_t h = dd.Height;
    if (w == 0 || h == 0)
        return;
    const auto viewParams = d.ResolveBuffer(Names::Res::ViewParams);
    if (!viewParams.IsValid())
        return;
    // Subrect depth has a different UV-to-projection mapping.
    if (d.Services.Views().GetViewLetterbox(d.View.id).active)
        return;

    const uint32_t aoW = (w + quality.ResolutionDivisor - 1) / quality.ResolutionDivisor;
    const uint32_t aoH = (h + quality.ResolutionDivisor - 1) / quality.ResolutionDivisor;
    uint32_t levels = 1;
    for (uint32_t extent = std::max(w, h); extent > 1 && levels <= quality.MaxDepthMip; extent >>= 1)
        ++levels;
    auto desc = [](uint32_t width, uint32_t height, TextureFormat format)
    {
        TextureDesc result{};
        result.width = width;
        result.height = height;
        result.depth = result.mipLevels = result.arrayLayers = result.sampleCount = 1;
        result.format = static_cast<uint32_t>(format);
        result.usage = static_cast<uint32_t>(TextureUsage::UnorderedAccess | TextureUsage::ShaderResource);
        return result;
    };
    auto mipDesc = desc(w, h, TextureFormat::R32_FLOAT);
    mipDesc.mipLevels = levels;
    // Pooled ownership also provides stable per-mip storage views.
    const auto depthMips = d.Frame.ImportPersistentTexture(view.DepthMipsPool.c_str(), mipDesc);
    const auto guide = d.Frame.CreateTexture("GTAO.Guide", desc(aoW, aoH, TextureFormat::R32G32_FLOAT));
    const auto aoDesc = desc(aoW, aoH, TextureFormat::R16G16B16A16_FLOAT);
    const auto fullDesc = desc(w, h, TextureFormat::R16G16B16A16_FLOAT);
    const auto rawAO = d.Frame.CreateTexture("GTAO.Raw", aoDesc);
    // Half-res AO keeps its temporal history half-res too; only the final
    // upsample allocates a full-res output. Full-res temporal publishes history.
    const bool needsSpatialOutput = quality.ResolutionDivisor == 2 || !m_Temporal;
    const auto spatial = needsSpatialOutput ? d.Frame.ImportPersistentTexture(view.OutputPool.c_str(), fullDesc)
                                            : RenderGraph::RGTexture{};
    const auto denoised = m_Temporal || quality.ResolutionDivisor == 2 ? d.Frame.CreateTexture("GTAO.Denoised", aoDesc) : spatial;
    if (!depthMips.IsValid() || !guide.IsValid() || !rawAO.IsValid() || !denoised.IsValid() ||
        (needsSpatialOutput && !spatial.IsValid()))
        return;

    const uint64_t frame = d.Frame.FrameIndex();
    RenderGraph::RGTexture historyRead{}, historyWrite{}, surfaceRead{}, surfaceWrite{};
    bool historyValid = false;
    if (m_Temporal)
    {
        const auto* currentCamera = d.Services.Views().FindCameraData(d.View.cameraId);
        if (!currentCamera)
            return;
        const GtaoHistoryKey historyKey{w, h, static_cast<uint32_t>(d.View.cameraId), m_Quality,
                                        settings.AORadius, settings.AOThickness, settings.AOIntensity, d.View.worldId};
        const uint32_t parity = view.Committed.Parity ^ 1u;
        bool freshAO = false;
        bool freshSurface = false;
        historyWrite = d.Frame.ImportPersistentTexture(view.HistoryPool[parity].c_str(), aoDesc);
        historyRead = d.Frame.ImportPersistentTexture(view.HistoryPool[parity ^ 1u].c_str(), aoDesc, &freshAO);
        const auto surfaceDesc = desc(aoW, aoH, TextureFormat::R32G32_FLOAT);
        surfaceWrite = d.Frame.ImportPersistentTexture(view.SurfacePool[parity].c_str(), surfaceDesc);
        surfaceRead = d.Frame.ImportPersistentTexture(view.SurfacePool[parity ^ 1u].c_str(), surfaceDesc, &freshSurface);
        if (!historyRead.IsValid() || !historyWrite.IsValid() || !surfaceRead.IsValid() || !surfaceWrite.IsValid())
            return;
        historyValid = CanReuseGtaoHistory(view.Committed.WrittenFrame, frame, view.Committed.Key, historyKey,
                                           freshAO || freshSurface,
                                           CameraContinuous(view.Committed.Camera, *currentCamera, settings.AORadius));
        view.Pending = {frame, parity, historyKey, *currentCamera};
    }
    else
        view.Committed = {};

    view.ChainValid = true;
    auto addPass = [&](Stage stage, const char* name, PassBindings bindings, uint32_t width, uint32_t height,
                       bool commitsHistory = false)
    {
        d.Frame.AddComputePass(d.PassName(name).c_str(), PassPhase::kDefault, [&](RenderGraph::RGPassBuilder& p)
                               {
                for (size_t i = 0; i < bindings.Count; ++i)
                {
                    const auto& b = bindings.Items[i];
                    RenderGraph::RGRange range{};
                    if (b.Mip != ~0u) { range.BaseMip = b.Mip; range.MipCount = 1; }
                    if (b.Usage == Access::StorageWrite) p.Write(b.Texture, RenderGraph::RGTextureWrite::Storage, range);
                    else p.Read(b.Texture, b.Usage == Access::Sampled ? RenderGraph::RGTextureRead::Sampled : RenderGraph::RGTextureRead::Storage, range);
                } }, [this, stage, bindings, viewParams, width, height, quality, settings, frame, historyValid, commitsHistory, viewId](RenderGraph::RGContext& ctx)
                               {
                ViewState& view = ViewStateFor(viewId);
                if (!view.ChainValid) return;
                // Set true again only after a complete dispatch was submitted.
                view.ChainValid = false;
                auto* dev = ctx.GetDevice();
                auto* cl = ctx.Cmd;
                if (!dev || !cl) return;
                const auto& shader = m_Shaders[stage];
                auto pipe = ctx.GetOrCreatePipelineVariant(shader.Pipeline);
                if (!pipe.IsValid() || !shader.Meta) return;
                DescriptorSetDesc setDesc{};
                setDesc.layout = shader.Layout;
                setDesc.transient = true;
                setDesc.debugName = "GTAO.Set0";
                auto ds = dev->CreateDescriptorSet(setDesc);
                if (!ds.IsValid()) return;
                NamedDescriptorWriter wd(dev, ds, *shader.Meta, 0);
                if (wd.Has("ViewParams")) wd.AddUniformBuffer("ViewParams", viewParams.Buffer, viewParams.Offset, viewParams.Size);
                for (size_t i = 0; i < bindings.Count; ++i)
                {
                    const auto& b = bindings.Items[i];
                    if (b.Usage == Access::Sampled)
                    {
                        const auto texture = ctx.GetTexture(b.Texture);
                        if (!texture.IsValid()) return;
                        wd.AddCombinedImageSampler(b.Name, texture, m_Sampler);
                    }
                    else
                    {
                        uint32_t slot = 0;
                        DescriptorType type{};
                        if (!Detail::TryGetSet0BindingByName(*shader.Meta, b.Name, slot, type)) return;
                        if (b.Mip != ~0u)
                        {
                            const auto mipView = ctx.GetOrCreatePooledMipView(b.Texture, b.Mip);
                            if (!mipView.IsValid()) return;
                            dev->UpdateStorageImageBinding(ds, slot, mipView);
                        }
                        else
                        {
                            const auto texture = ctx.GetTexture(b.Texture);
                            if (!texture.IsValid()) return;
                            dev->UpdateStorageImageBinding(ds, slot, texture);
                        }
                    }
                }
                wd.Flush();
                cl->SetPipeline(pipe);
                if (!shader.Meta->PushConstants.empty())
                {
                    NamedPushConstantWriter pcw(*shader.Meta, shader.Meta->PushConstants[0].Name);
                    if (!pcw.IsValid()) return;
                    for (const auto& member : shader.Meta->PushConstants[0].Block.Members)
                    {
                        if (member.Name == "directions") pcw.Add(member.Name, quality.Directions);
                        else if (member.Name == "steps") pcw.Add(member.Name, kGtaoSweepSteps);
                        else if (member.Name == "noiseFrame") pcw.Add(member.Name, m_Temporal ? static_cast<uint32_t>(frame & 7u) : 0u);
                        else if (member.Name == "historyValid") pcw.Add(member.Name, historyValid ? 1u : 0u);
                        else if (!settings.TryWriteField(member.Name, pcw)) return;
                    }
                    pcw.Flush(cl);
                }
                cl->BindDescriptorSet(0, ds, pipe);
                cl->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
                view.ChainValid = true;
                if (commitsHistory) view.Committed = view.Pending; });
    };
    addPass(Prepare, "GTAOPrepare", {{"uDepth", depth, Access::Sampled}, {"uGuide", guide, Access::StorageWrite}, {"uDepthOut", depthMips, Access::StorageWrite, 0}}, w, h);
    static_assert(GetGtaoQualitySettings(GtaoQuality::Medium).MaxDepthMip <= std::size(kDepthMipPassNames) &&
                      GetGtaoQualitySettings(GtaoQuality::High).MaxDepthMip <= std::size(kDepthMipPassNames) &&
                      GetGtaoQualitySettings(GtaoQuality::Ultra).MaxDepthMip <= std::size(kDepthMipPassNames),
                  "one pass name literal per producible depth mip");
    for (uint32_t mip = 1; mip < levels; ++mip)
        addPass(DepthMip, kDepthMipPassNames[mip - 1],
                {{"uDepthIn", depthMips, Access::StorageRead, mip - 1}, {"uDepthOut", depthMips, Access::StorageWrite, mip}},
                std::max(1u, w >> mip), std::max(1u, h >> mip));
    addPass(Sweep, "GTAO", {{"uDepth", depth, Access::Sampled}, {"uGuide", guide, Access::Sampled}, {"uDepthMips", depthMips, Access::Sampled}, {"uAO", rawAO, Access::StorageWrite}}, aoW, aoH);
    addPass(Blur, "GTAOBlur", {{"uAOIn", rawAO, Access::Sampled}, {"uDepth", depth, Access::Sampled}, {"uGuide", guide, Access::Sampled}, {"uAOOut", denoised, Access::StorageWrite}}, aoW, aoH);
    if (m_Temporal)
    {
        addPass(Temporal, "GTAOTemporal", {{"uCurrent", denoised, Access::Sampled}, {"uGuide", guide, Access::Sampled}, {"uDepth", depth, Access::Sampled}, {"uHistory", historyRead, Access::Sampled}, {"uHistorySurface", surfaceRead, Access::Sampled}, {"uAOOut", historyWrite, Access::StorageWrite}, {"uSurfaceOut", surfaceWrite, Access::StorageWrite}}, aoW, aoH,
                /*commitsHistory=*/true);
        d.Frame.MarkPersistentTextureInitialized(historyWrite);
        d.Frame.MarkPersistentTextureInitialized(surfaceWrite);
    }
    const auto filtered = m_Temporal ? historyWrite : denoised;
    if (quality.ResolutionDivisor == 2)
        addPass(Upsample, "GTAOUpsample", {{"uAOIn", filtered, Access::Sampled}, {"uDepth", depth, Access::Sampled}, {"uGuide", guide, Access::Sampled}, {"uAOOut", spatial, Access::StorageWrite}}, w, h);
    d.PublishTexture(m_OutputKey, quality.ResolutionDivisor == 2 ? spatial : filtered);
}

void AONode::LoadShaders(IDevice* device)
{
    if (m_LoadAttempted)
        return;
    m_LoadAttempted = true;
    if (!device->GetCapabilities().supportsFilterableFloat32)
    {
        // Reflected float depth samplers require float32-filterable on WebGPU.
        // Keep the effect inactive instead of invalidating the shared encoder.
        LOG_INFO("AONode: GTAO needs float32-filterable; the effect stays inactive.");
        return;
    }
    static constexpr const char* paths[StageCount] = {
        "Shaders/gtao_prepare.shaderpkg", "Shaders/gtao_depth_mip.shaderpkg", "Shaders/gtao.shaderpkg",
        "Shaders/gtao_blur.shaderpkg", "Shaders/gtao_upsample.shaderpkg", "Shaders/gtao_temporal.shaderpkg"};
    for (size_t i = 0; i < StageCount; ++i)
    {
        auto& shader = m_Shaders[i];
        ShaderPackage pkg{};
        std::string err;
        if (!LoadShaderPkg(paths[i], device->PreferredShaderSource(), pkg, &err))
        {
            LOG_WARNING("AONode: failed to load {}: {}", paths[i], err);
            continue;
        }
        const auto it = pkg.stageBytes.find("cs");
        if (it == pkg.stageBytes.end() || it->second.empty())
        {
            LOG_WARNING("AONode: {} missing cs stage", paths[i]);
            continue;
        }
        shader.Meta = std::make_unique<ShaderMeta>(std::move(pkg.meta));
        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(it->second));
        cd.DebugName = paths[i];
        auto patchLayout = [&](uint32_t set, DescriptorSetLayoutDesc& layout)
        { if (set == 0) shader.Layout = layout; };
        if (!MaterialHelper::ApplyShaderMetaToComputeDesc(*device, *shader.Meta, cd, MaterialBuilder::MergeMode::Auto,
                                                          {true, 128}, patchLayout, &err))
        {
            LOG_WARNING("AONode: failed to reflect {}: {}", paths[i], err);
            continue;
        }
        shader.Pipeline = device->InternComputePipeline(std::move(cd));
    }
    if (!m_Sampler.IsValid())
        m_Sampler = device->CreateSampler(SamplerDesc::PointClamp("GTAO.Sampler"));
}
} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes

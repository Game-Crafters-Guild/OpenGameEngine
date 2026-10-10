#include "Engine/Rendering/Pipeline/Nodes/SmaaNode.h"

#include "Engine/Rendering/AntiAliasing.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderServices.h"

#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

namespace
{
ComputePipelineId LoadComputePkg(IDevice& device, const char* pkgPath, const char* debugName,
                                 std::unique_ptr<ShaderMeta>& outMeta,
                                 DescriptorSetLayoutDesc& outSet0)
{
    ShaderPackage pkg{};
    std::string loadErr;
    if (!LoadShaderPkg(pkgPath, device.PreferredShaderSource(), pkg, &loadErr))
    {
        LOG_WARNING("SmaaNode: failed to load {}: {}", pkgPath, loadErr);
        return {};
    }
    auto itCs = pkg.stageBytes.find("cs");
    if (itCs == pkg.stageBytes.end() || itCs->second.empty())
    {
        LOG_WARNING("SmaaNode: {} missing cs stage", pkgPath);
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

bool SmaaNode::Initialize(std::string nodeId, std::string nodeJson, std::string* /*outError*/)
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

void SmaaNode::Declare(RenderPipelineInstance& instance, const PipelineDeclareContext& /*ctx*/)
{
    if (auto* dev = instance.GetRenderServices().GetDevice())
        LoadShaders(dev);
}

void SmaaNode::DeclareForView(ViewDeclare& d)
{
    const RenderGraph::RGTexture input = d.ResolveTexture(m_InputKey);
    // Stitch-through: the output name must resolve for FinalCopy whether SMAA
    // runs or not. Overwritten below when the blend actually declares.
    if (input.IsValid())
        d.PublishTexture(m_OutputKey, input);

    const auto* taa = d.Services.Views().FindViewAntiAliasing(d.View.id);
    if (taa == nullptr || taa->Mode != AntiAliasingMode::SMAA)
        return; // AA mode is not SMAA for this view: passthrough
    if (!input.IsValid())
    {
        if (!m_WarnedUnresolvedInput)
        {
            m_WarnedUnresolvedInput = true;
            Logger::Log::Warning(
                "SmaaNode '{}': input '{}' does not resolve in this rendergraph; SMAA is "
                "inactive",
                m_Id, m_InputKey);
        }
        return;
    }
    if (!m_ShadersLoaded)
        return; // not staged yet: passthrough, retry next frame
    // Letterboxed views render into a sub-rect; the passes assume full extent.
    if (d.Services.Views().GetViewLetterbox(d.View.id).active)
        return;

    // SMAA runs at the LDR chain's actual extent (post any render-scale
    // crossing), same as the FXAA node.
    const auto& inputDesc = d.Frame.Graph().ResourceDesc(input.Id);
    const uint32_t w = inputDesc.Width;
    const uint32_t h = inputDesc.Height;
    if (w == 0 || h == 0)
        return;

    // ── Resources (all transient: every pass fully overwrites its target) ──
    const std::string base = "SMAA.View" + std::to_string(static_cast<uint32_t>(d.View.id));

    TextureDesc maskDesc{};
    maskDesc.width = w;
    maskDesc.height = h;
    maskDesc.mipLevels = 1;
    maskDesc.arrayLayers = 1;
    maskDesc.sampleCount = 1;
    maskDesc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    maskDesc.usage = static_cast<uint32_t>(TextureUsage::UnorderedAccess) |
                     static_cast<uint32_t>(TextureUsage::ShaderResource);
    maskDesc.debugName = "SMAA.Edges";
    const RenderGraph::RGTexture edgesTex =
        d.Frame.CreateTexture((base + ".Edges").c_str(), maskDesc);

    TextureDesc weightsDesc = maskDesc;
    weightsDesc.debugName = "SMAA.Weights";
    const RenderGraph::RGTexture weightsTex =
        d.Frame.CreateTexture((base + ".Weights").c_str(), weightsDesc);

    // POOL-backed output: FinalCopy (a FullscreenShader) binds its input by
    // physical handle at declare time, which only pool/imported textures have.
    TextureDesc outDesc{};
    outDesc.width = w;
    outDesc.height = h;
    outDesc.mipLevels = 1;
    outDesc.arrayLayers = 1;
    outDesc.sampleCount = 1;
    outDesc.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    outDesc.usage = static_cast<uint32_t>(TextureUsage::UnorderedAccess) |
                    static_cast<uint32_t>(TextureUsage::ShaderResource);
    outDesc.persistent = true;
    outDesc.debugName = "SMAA.Output";
    const RenderGraph::RGTexture outColor =
        d.Frame.ImportPersistentTexture((base + ".Output").c_str(), outDesc);
    if (!edgesTex.IsValid() || !weightsTex.IsValid() || !outColor.IsValid())
        return;

    const auto reportBindingMismatch = [this](const char* pass)
    {
        if (!m_WarnedBindingMismatch)
        {
            m_WarnedBindingMismatch = true;
            Logger::Log::Error(
                "SmaaNode: {} descriptor name mismatch vs reflection — dispatch skipped", pass);
        }
    };

    // ── Pass 1: edge detection ─────────────────────────────────────────────
    d.Frame.AddComputePass(
        d.PassName("SMAAEdges").c_str(), Rendering::PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(input, RenderGraph::RGTextureRead::Sampled);
            p.Write(edgesTex, RenderGraph::RGTextureWrite::Storage);
        },
        [this, input, edgesTex, w, h, reportBindingMismatch](RenderGraph::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl || !m_EdgesMeta)
                return;
            const auto inputTex = ctx.GetTexture(input);
            const auto outTex = ctx.GetTexture(edgesTex);
            if (!inputTex.IsValid() || !outTex.IsValid() || !m_LinearClampSampler.IsValid())
                return;

            DescriptorSetDesc dsDesc{};
            dsDesc.layout = m_EdgesSet0Layout;
            dsDesc.transient = true;
            dsDesc.debugName = "SMAA.Edges.Set0";
            auto ds = dev->CreateDescriptorSet(dsDesc);
            dev->UpdateStorageImageBinding(ds, 0, outTex);
            {
                NamedDescriptorWriter wd(dev, ds, *m_EdgesMeta, 0);
                if (!wd.TryAddCombinedImageSampler("uSceneColor", inputTex, m_LinearClampSampler))
                {
                    reportBindingMismatch("smaa_edges");
                    return;
                }
                wd.Flush();
            }

            const PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(m_EdgesPipelineId);
            if (!pipe.IsValid())
                return;
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        });

    // ── Pass 2: blend weights ──────────────────────────────────────────────
    d.Frame.AddComputePass(
        d.PassName("SMAAWeights").c_str(), Rendering::PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(edgesTex, RenderGraph::RGTextureRead::Sampled);
            p.Write(weightsTex, RenderGraph::RGTextureWrite::Storage);
        },
        [this, edgesTex, weightsTex, w, h, reportBindingMismatch](RenderGraph::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl || !m_WeightsMeta)
                return;
            const auto inTex = ctx.GetTexture(edgesTex);
            const auto outTex = ctx.GetTexture(weightsTex);
            if (!inTex.IsValid() || !outTex.IsValid())
                return;

            DescriptorSetDesc dsDesc{};
            dsDesc.layout = m_WeightsSet0Layout;
            dsDesc.transient = true;
            dsDesc.debugName = "SMAA.Weights.Set0";
            auto ds = dev->CreateDescriptorSet(dsDesc);
            dev->UpdateStorageImageBinding(ds, 0, outTex);
            {
                NamedDescriptorWriter wd(dev, ds, *m_WeightsMeta, 0);
                if (!wd.TryAddCombinedImageSampler("uEdges", inTex, m_LinearClampSampler))
                {
                    reportBindingMismatch("smaa_weights");
                    return;
                }
                wd.Flush();
            }

            const PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(m_WeightsPipelineId);
            if (!pipe.IsValid())
                return;
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        });

    // ── Pass 3: neighborhood blend ─────────────────────────────────────────
    d.Frame.AddComputePass(
        d.PassName("SMAABlend").c_str(), Rendering::PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(input, RenderGraph::RGTextureRead::Sampled);
            p.Read(weightsTex, RenderGraph::RGTextureRead::Sampled);
            p.Write(outColor, RenderGraph::RGTextureWrite::Storage);
        },
        [this, input, weightsTex, outColor, w, h, reportBindingMismatch](RenderGraph::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl || !m_BlendMeta)
                return;
            const auto colorTex = ctx.GetTexture(input);
            const auto wTex = ctx.GetTexture(weightsTex);
            const auto outTex = ctx.GetTexture(outColor);
            if (!colorTex.IsValid() || !wTex.IsValid() || !outTex.IsValid())
                return;

            DescriptorSetDesc dsDesc{};
            dsDesc.layout = m_BlendSet0Layout;
            dsDesc.transient = true;
            dsDesc.debugName = "SMAA.Blend.Set0";
            auto ds = dev->CreateDescriptorSet(dsDesc);
            dev->UpdateStorageImageBinding(ds, 0, outTex);
            {
                NamedDescriptorWriter wd(dev, ds, *m_BlendMeta, 0);
                const bool ok =
                    wd.TryAddCombinedImageSampler("uSceneColor", colorTex, m_LinearClampSampler) &&
                    wd.TryAddCombinedImageSampler("uWeights", wTex, m_LinearClampSampler);
                if (!ok)
                {
                    reportBindingMismatch("smaa_blend");
                    return;
                }
                wd.Flush();
            }

            const PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(m_BlendPipelineId);
            if (!pipe.IsValid())
                return;
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        });

    d.PublishTexture(m_OutputKey, outColor);
}

SmaaNode::~SmaaNode()
{
    if (m_Device && m_LinearClampSampler.IsValid())
        m_Device->DestroySampler(m_LinearClampSampler);
}

void SmaaNode::LoadShaders(IDevice* device)
{
    if (m_ShadersLoaded || device == nullptr)
        return;

    if (!m_EdgesPipelineId.IsValid())
        m_EdgesPipelineId = LoadComputePkg(*device, "Shaders/smaa_edges.shaderpkg", "SMAAEdges",
                                           m_EdgesMeta, m_EdgesSet0Layout);
    if (!m_WeightsPipelineId.IsValid())
        m_WeightsPipelineId = LoadComputePkg(*device, "Shaders/smaa_weights.shaderpkg",
                                             "SMAAWeights", m_WeightsMeta, m_WeightsSet0Layout);
    if (!m_BlendPipelineId.IsValid())
        m_BlendPipelineId = LoadComputePkg(*device, "Shaders/smaa_blend.shaderpkg", "SMAABlend",
                                           m_BlendMeta, m_BlendSet0Layout);

    // Latch only on success so a not-yet-staged pkg retries next frame.
    m_ShadersLoaded = m_EdgesPipelineId.IsValid() && m_WeightsPipelineId.IsValid() &&
                      m_BlendPipelineId.IsValid();

    if (!m_LinearClampSampler.IsValid())
        m_LinearClampSampler =
            device->CreateSampler(SamplerDesc::MaterialLinearClamp("SMAA.LinearClamp"));
    m_Device = device;
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes

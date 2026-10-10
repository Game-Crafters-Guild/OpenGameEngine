#include "Engine/Rendering/Pipeline/Nodes/FidelityFXDofNode.h"

#include "Engine/Rendering/Pipeline/Nodes/PipelineNodeUtils.h"
#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/RenderServices.h"

#include "Logger/Logger.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/NamedPushConstantWriter.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <nlohmann/json.hpp>

namespace GameEngine::Engine::Renderer { using namespace ::GameEngine::Rendering; }

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

namespace
{
void WriteDofPushConstants(const ShaderMeta* meta, CommandList* cl,
                           const PostProcessSettings& settings)
{
    if (!meta || !cl || meta->PushConstants.empty())
        return;
    NamedPushConstantWriter writer(*meta, meta->PushConstants[0].Name);
    if (!writer.IsValid())
        return;
    for (const auto& member : meta->PushConstants[0].Block.Members)
        (void)settings.TryWriteField(member.Name, writer);
    writer.Flush(cl);
}

TextureDesc MakeTransientDesc(uint32_t width, uint32_t height, TextureFormat format,
                              const char* debugName)
{
    TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.sampleCount = 1;
    desc.format = static_cast<uint32_t>(format);
    desc.usage = static_cast<uint32_t>(TextureUsage::UnorderedAccess |
                                       TextureUsage::ShaderResource);
    desc.debugName = debugName;
    return desc;
}
} // namespace

bool FidelityFXDofNode::Initialize(std::string nodeId, std::string nodeJson,
                                   std::string* outError)
{
    m_Id = std::move(nodeId);
    m_RequiredPackage = "fidelityfx-dof";
    try
    {
        const auto j = nlohmann::json::parse(nodeJson);
        if (!j.is_object())
        {
            if (outError) *outError = "FidelityFXDepthOfField node JSON is not an object";
            return false;
        }
        auto read = [&](const char* key, std::string& value)
        {
            if (j.contains(key) && j[key].is_string())
                value = j[key].get<std::string>();
        };
        read("input", m_InputKey);
        read("depth", m_DepthKey);
        read("output", m_OutputKey);
        read("exposureBuffer", m_ExposureBufferKey);
        read("requiresPackage", m_RequiredPackage);
        if (m_RequiredPackage.empty())
        {
            if (outError) *outError = "FidelityFXDepthOfField requiresPackage must not be empty";
            return false;
        }
    }
    catch (const std::exception& e)
    {
        if (outError) *outError = std::string("FidelityFXDepthOfField JSON parse failed: ") + e.what();
        return false;
    }
    return true;
}

void FidelityFXDofNode::Declare(RenderPipelineInstance& instance,
                                const PipelineDeclareContext& /*ctx*/)
{
    if (!instance.GetRenderServices().IsPackageAvailable(m_RequiredPackage))
        return;
    if (auto* device = instance.GetRenderServices().GetDevice())
        LoadShaders(device);
}

void FidelityFXDofNode::DeclareForView(ViewDeclare& d)
{
    const RenderGraph::RGTexture source = d.ResolveTexture(m_InputKey);
    if (!source.IsValid())
        return;

    if (!d.Services.IsPackageAvailable(m_RequiredPackage))
    {
        d.PublishTexture(m_OutputKey, source);
        return;
    }

    const PostProcessSettings settings =
        d.Services.GetEffectivePostProcessSettings(d.View.id, d.View.worldId);
    if (!settings.IsDofActive())
    {
        d.PublishTexture(m_OutputKey, source);
        return;
    }

    for (const auto& stage : m_Stages)
    {
        if (!stage.Pipeline.IsValid())
        {
            d.PublishTexture(m_OutputKey, source);
            return;
        }
    }

    const RenderGraph::RGTexture depth = d.ResolveTexture(m_DepthKey);
    const RenderGraph::RGTexture output = d.ResolveTexture(m_OutputKey);
    const PipelineBufferBindingRG viewParams = d.ResolveBuffer(Names::Res::ViewParams);
    const PipelineBufferBindingRG exposure = d.ResolveBuffer(m_ExposureBufferKey);
    if (!depth.IsValid() || !output.IsValid() || !viewParams.IsValid() || !exposure.IsValid())
    {
        d.PublishTexture(m_OutputKey, source);
        return;
    }

    const auto& srcDesc = d.Frame.Graph().ResourceDesc(source.Id);
    const uint32_t width = srcDesc.Width;
    const uint32_t height = srcDesc.Height;
    if (width == 0 || height == 0)
    {
        d.PublishTexture(m_OutputKey, source);
        return;
    }

    const uint32_t halfW = (width + 1u) / 2u;
    const uint32_t halfH = (height + 1u) / 2u;
    const uint32_t tileW = (halfW + 7u) / 8u;
    const uint32_t tileH = (halfH + 7u) / 8u;

    const RenderGraph::RGTexture half = d.Frame.CreateTexture(
        "FidelityFXDoF.HalfColorCoc",
        MakeTransientDesc(halfW, halfH, TextureFormat::R16G16B16A16_FLOAT,
                          "FidelityFXDoF.HalfColorCoc"));
    // The package owns the storage format; runtime feature overrides do not
    // change the format declared by an already cooked shader.
    const TextureFormat cocFormat = m_CocFormat;
    const RenderGraph::RGTexture tile = d.Frame.CreateTexture(
        "FidelityFXDoF.TileCoc",
        MakeTransientDesc(tileW, tileH, cocFormat, "FidelityFXDoF.TileCoc"));
    const RenderGraph::RGTexture dilated = d.Frame.CreateTexture(
        "FidelityFXDoF.DilatedCoc",
        MakeTransientDesc(tileW, tileH, cocFormat, "FidelityFXDoF.DilatedCoc"));
    const RenderGraph::RGTexture nearField = d.Frame.CreateTexture(
        "FidelityFXDoF.NearField",
        MakeTransientDesc(halfW, halfH, TextureFormat::R16G16B16A16_FLOAT,
                          "FidelityFXDoF.NearField"));
    const RenderGraph::RGTexture farField = d.Frame.CreateTexture(
        "FidelityFXDoF.FarField",
        MakeTransientDesc(halfW, halfH, TextureFormat::R16G16B16A16_FLOAT,
                          "FidelityFXDoF.FarField"));
    if (!half.IsValid() || !tile.IsValid() || !dilated.IsValid() ||
        !nearField.IsValid() || !farField.IsValid())
    {
        d.PublishTexture(m_OutputKey, source);
        return;
    }

    const uint32_t halfGX = (halfW + 7u) / 8u;
    const uint32_t halfGY = (halfH + 7u) / 8u;
    const uint32_t tileGX = (tileW + 7u) / 8u;
    const uint32_t tileGY = (tileH + 7u) / 8u;
    const uint32_t fullGX = (width + 7u) / 8u;
    const uint32_t fullGY = (height + 7u) / 8u;

    // 1. Bilateral half-resolution color + signed CoC reduction.
    d.Frame.AddComputePass(
        d.PassName(".Prepare").c_str(), PassPhase::kPostProcess,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(source, RenderGraph::RGTextureRead::Sampled);
            p.Read(depth, RenderGraph::RGTextureRead::Sampled);
            p.Write(half, RenderGraph::RGTextureWrite::Storage);
        },
        [this, source, depth, half, viewParams, settings, halfGX, halfGY](RenderGraph::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice(); auto* cl = ctx.Cmd;
            if (!dev || !cl) return;
            const auto src = ctx.GetTexture(source), dep = ctx.GetTexture(depth), dst = ctx.GetTexture(half);
            if (!src.IsValid() || !dep.IsValid() || !dst.IsValid()) return;
            const auto& stage = m_Stages[0];
            DescriptorSetDesc dd{}; dd.layout = stage.Set0; dd.transient = true; dd.debugName = "FidelityFXDoF.Prepare.Set0";
            auto ds = dev->CreateDescriptorSet(dd);
            NamedDescriptorWriter wd(dev, ds, *stage.Meta, 0);
            wd.AddCombinedImageSampler("uSceneColor", src, m_LinearSampler)
              .AddCombinedImageSampler("uDepth", dep, m_LinearSampler)
              .AddUniformBuffer("ViewParams", viewParams.Buffer, viewParams.Offset, viewParams.Size);
            wd.Flush();
            Detail::BindStorageImageByName(dev, ds, *stage.Meta, "uHalfColorCoc", dst);
            auto pipe = ctx.GetOrCreatePipelineVariant(stage.Pipeline); if (!pipe.IsValid()) return;
            cl->SetPipeline(pipe); WriteDofPushConstants(stage.Meta.get(), cl, settings);
            cl->BindDescriptorSet(0, ds, pipe); cl->Dispatch(halfGX, halfGY, 1);
        });

    // 2. Tile extrema/classification input.
    d.Frame.AddComputePass(
        d.PassName(".Tile").c_str(), PassPhase::kPostProcess,
        [&](RenderGraph::RGPassBuilder& p)
        { p.Read(half, RenderGraph::RGTextureRead::Sampled); p.Write(tile, RenderGraph::RGTextureWrite::Storage); },
        [this, half, tile, tileW, tileH](RenderGraph::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice(); auto* cl = ctx.Cmd; if (!dev || !cl) return;
            const auto src = ctx.GetTexture(half), dst = ctx.GetTexture(tile); if (!src.IsValid() || !dst.IsValid()) return;
            const auto& stage = m_Stages[1]; DescriptorSetDesc dd{}; dd.layout = stage.Set0; dd.transient = true; dd.debugName = "FidelityFXDoF.Tile.Set0";
            auto ds = dev->CreateDescriptorSet(dd); NamedDescriptorWriter wd(dev, ds, *stage.Meta, 0);
            wd.AddCombinedImageSampler("uHalfColorCoc", src, m_LinearSampler); wd.Flush();
            Detail::BindStorageImageByName(dev, ds, *stage.Meta, "uTileCoc", dst);
            auto pipe = ctx.GetOrCreatePipelineVariant(stage.Pipeline); if (!pipe.IsValid()) return;
            cl->SetPipeline(pipe); cl->BindDescriptorSet(0, ds, pipe); cl->Dispatch(tileW, tileH, 1);
        });

    // 3. Scatter-as-gather tile dilation.
    d.Frame.AddComputePass(
        d.PassName(".Dilate").c_str(), PassPhase::kPostProcess,
        [&](RenderGraph::RGPassBuilder& p)
        { p.Read(tile, RenderGraph::RGTextureRead::Sampled); p.Write(dilated, RenderGraph::RGTextureWrite::Storage); },
        [this, tile, dilated, settings, tileGX, tileGY](RenderGraph::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice(); auto* cl = ctx.Cmd; if (!dev || !cl) return;
            const auto src = ctx.GetTexture(tile), dst = ctx.GetTexture(dilated); if (!src.IsValid() || !dst.IsValid()) return;
            const auto& stage = m_Stages[2]; DescriptorSetDesc dd{}; dd.layout = stage.Set0; dd.transient = true; dd.debugName = "FidelityFXDoF.Dilate.Set0";
            auto ds = dev->CreateDescriptorSet(dd); NamedDescriptorWriter wd(dev, ds, *stage.Meta, 0);
            wd.AddCombinedImageSampler("uTileCoc", src, m_LinearSampler); wd.Flush();
            Detail::BindStorageImageByName(dev, ds, *stage.Meta, "uDilatedCoc", dst);
            auto pipe = ctx.GetOrCreatePipelineVariant(stage.Pipeline); if (!pipe.IsValid()) return;
            cl->SetPipeline(pipe); WriteDofPushConstants(stage.Meta.get(), cl, settings);
            cl->BindDescriptorSet(0, ds, pipe); cl->Dispatch(tileGX, tileGY, 1);
        });

    // 4. Classified near/far ring blur with custom aperture samples.
    d.Frame.AddComputePass(
        d.PassName(".Blur").c_str(), PassPhase::kPostProcess,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(half, RenderGraph::RGTextureRead::Sampled); p.Read(dilated, RenderGraph::RGTextureRead::Sampled);
            p.Write(nearField, RenderGraph::RGTextureWrite::Storage); p.Write(farField, RenderGraph::RGTextureWrite::Storage);
        },
        [this, half, dilated, nearField, farField, settings, halfGX, halfGY](RenderGraph::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice(); auto* cl = ctx.Cmd; if (!dev || !cl) return;
            const auto src = ctx.GetTexture(half), tiles = ctx.GetTexture(dilated), nearTex = ctx.GetTexture(nearField), farTex = ctx.GetTexture(farField);
            if (!src.IsValid() || !tiles.IsValid() || !nearTex.IsValid() || !farTex.IsValid()) return;
            const auto& stage = m_Stages[3]; DescriptorSetDesc dd{}; dd.layout = stage.Set0; dd.transient = true; dd.debugName = "FidelityFXDoF.Blur.Set0";
            auto ds = dev->CreateDescriptorSet(dd); NamedDescriptorWriter wd(dev, ds, *stage.Meta, 0);
            wd.AddCombinedImageSampler("uHalfColorCoc", src, m_LinearSampler)
              .AddCombinedImageSampler("uDilatedCoc", tiles, m_LinearSampler); wd.Flush();
            Detail::BindStorageImageByName(dev, ds, *stage.Meta, "uNearField", nearTex);
            Detail::BindStorageImageByName(dev, ds, *stage.Meta, "uFarField", farTex);
            auto pipe = ctx.GetOrCreatePipelineVariant(stage.Pipeline); if (!pipe.IsValid()) return;
            cl->SetPipeline(pipe); WriteDofPushConstants(stage.Meta.get(), cl, settings);
            cl->BindDescriptorSet(0, ds, pipe); cl->Dispatch(halfGX, halfGY, 1);
        });

    // 5. Filter, upscale and compose near/far layers at full resolution.
    d.Frame.AddComputePass(
        d.PassName(".Composite").c_str(), PassPhase::kPostProcess,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(source, RenderGraph::RGTextureRead::Sampled); p.Read(depth, RenderGraph::RGTextureRead::Sampled);
            p.Read(nearField, RenderGraph::RGTextureRead::Sampled); p.Read(farField, RenderGraph::RGTextureRead::Sampled);
            if (exposure.Graph.IsValid()) p.Read(exposure.Graph, RenderGraph::RGBufferRead::Storage);
            p.Write(output, RenderGraph::RGTextureWrite::Storage);
        },
        [this, source, depth, nearField, farField, output, viewParams, exposure, settings, fullGX, fullGY](RenderGraph::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice(); auto* cl = ctx.Cmd; if (!dev || !cl) return;
            const auto src = ctx.GetTexture(source), dep = ctx.GetTexture(depth), nearTex = ctx.GetTexture(nearField), farTex = ctx.GetTexture(farField), dst = ctx.GetTexture(output);
            if (!src.IsValid() || !dep.IsValid() || !nearTex.IsValid() || !farTex.IsValid() || !dst.IsValid()) return;
            const auto& stage = m_Stages[4]; DescriptorSetDesc dd{}; dd.layout = stage.Set0; dd.transient = true; dd.debugName = "FidelityFXDoF.Composite.Set0";
            auto ds = dev->CreateDescriptorSet(dd); NamedDescriptorWriter wd(dev, ds, *stage.Meta, 0);
            wd.AddCombinedImageSampler("uSceneColor", src, m_LinearSampler)
              .AddCombinedImageSampler("uDepth", dep, m_LinearSampler)
              .AddCombinedImageSampler("uNearField", nearTex, m_LinearSampler)
              .AddCombinedImageSampler("uFarField", farTex, m_LinearSampler)
              .AddUniformBuffer("ViewParams", viewParams.Buffer, viewParams.Offset, viewParams.Size)
              .AddStorageBuffer("uExposure", exposure.Buffer, exposure.Offset, exposure.Size);
            wd.Flush(); Detail::BindStorageImageByName(dev, ds, *stage.Meta, "uOutput", dst);
            auto pipe = ctx.GetOrCreatePipelineVariant(stage.Pipeline); if (!pipe.IsValid()) return;
            cl->SetPipeline(pipe); WriteDofPushConstants(stage.Meta.get(), cl, settings);
            cl->BindDescriptorSet(0, ds, pipe); cl->Dispatch(fullGX, fullGY, 1);
        });

    d.PublishTexture(m_OutputKey, output);
}

void FidelityFXDofNode::LoadShaders(IDevice* device)
{
    if (m_LoadAttempted || !device) return;
    m_LoadAttempted = true;
    if (!device->GetCapabilities().supportsFilterableFloat32)
    {
        // Prepare and composite sample the resolved R32F depth through a
        // filtering sampler; binding an R32F view to a Float layout needs
        // WebGPU's float32-filterable feature. Without it the bind fails and an
        // invalid pass poisons the shared per-frame encoder — decline instead,
        // the same way SSSR and GTAO do.
        LOG_INFO("FidelityFXDofNode: depth of field needs float32-filterable, which this "
                 "adapter lacks; the effect stays inactive.");
        return;
    }
    static constexpr const char* kPaths[] = {
        "Shaders/ffx_dof_prepare.shaderpkg", "Shaders/ffx_dof_tile.shaderpkg",
        "Shaders/ffx_dof_dilate.shaderpkg", "Shaders/ffx_dof_blur.shaderpkg",
        "Shaders/ffx_dof_composite.shaderpkg"};
    static constexpr const char* kNames[] = {
        "FidelityFXDoF.Prepare", "FidelityFXDoF.Tile", "FidelityFXDoF.Dilate",
        "FidelityFXDoF.Blur", "FidelityFXDoF.Composite"};

    for (size_t i = 0; i < m_Stages.size(); ++i)
    {
        ShaderPackage pkg{}; std::string error;
        if (!LoadShaderPkg(kPaths[i], device->PreferredShaderSource(), pkg, &error))
        {
            LOG_WARNING("FidelityFXDofNode: failed to load {}: {}", kPaths[i], error);
            continue;
        }
        auto cs = pkg.stageBytes.find("cs");
        if (cs == pkg.stageBytes.end() || cs->second.empty()) continue;
        auto& stage = m_Stages[i]; stage.Meta = std::make_unique<ShaderMeta>(std::move(pkg.meta));
        ComputePipelineDesc desc{};
        desc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(cs->second));
        desc.DebugName = kNames[i];
        auto patch = [&](uint32_t setIndex, DescriptorSetLayoutDesc& layout)
        { if (setIndex == 0) stage.Set0 = layout; };
        MaterialHelper::ApplyShaderMetaToComputeDesc(*device, *stage.Meta, desc,
            MaterialBuilder::MergeMode::Auto, {true, 128}, patch, &error);
        if (i == 1 || i == 2)
        {
            TextureFormat format = TextureFormat::Unknown;
            for (const auto& binding : stage.Set0.bindings)
                if (binding.binding == 1 && binding.type == DescriptorType::StorageImage)
                    format = static_cast<TextureFormat>(binding.storageTexelFormat);
            if ((format != TextureFormat::R16G16_FLOAT &&
                 format != TextureFormat::R16G16B16A16_FLOAT) ||
                (i == 2 && format != m_CocFormat))
            {
                LOG_WARNING("FidelityFXDofNode: incompatible CoC storage format in {}", kPaths[i]);
                return;
            }
            m_CocFormat = format;
        }
        stage.Pipeline = device->InternComputePipeline(std::move(desc));
    }
    if (!m_LinearSampler.IsValid())
        m_LinearSampler = device->CreateSampler(
            SamplerDesc::MaterialLinearClamp("FidelityFXDoF.LinearClamp"));
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes

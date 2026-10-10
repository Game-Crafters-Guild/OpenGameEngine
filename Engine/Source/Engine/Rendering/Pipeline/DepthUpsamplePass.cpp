#include "Engine/Rendering/Pipeline/DepthUpsamplePass.h"

#include "Logger/Logger.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <utility>

namespace GameEngine::Engine::Renderer::Pipeline
{
using namespace ::GameEngine::Rendering;

bool DepthUpsamplePass::LoadVariant(IDevice& device, const char* package, const char* debugName, Variant& out)
{
    ShaderPackage pkg{};
    std::string loadErr;
    if (!LoadShaderPkg(package, device.PreferredShaderSource(), pkg, &loadErr))
    {
        Logger::Log::Warning("DepthUpsamplePass: failed to load {}: {}", package, loadErr);
        return false;
    }
    auto itVs = pkg.stageBytes.find("vs");
    auto itFs = pkg.stageBytes.find("fs");
    if (itVs == pkg.stageBytes.end() || itFs == pkg.stageBytes.end())
    {
        Logger::Log::Warning("DepthUpsamplePass: {} missing vs/fs", package);
        return false;
    }

    out.Meta = std::make_unique<ShaderMeta>(std::move(pkg.meta));
    GraphicsPipelineDesc gd{};
    gd.Kind = GraphicsPipelineKind::VertexFragment;
    gd.VertexShader = std::make_shared<const std::vector<uint8_t>>(std::move(itVs->second));
    gd.PixelShader = std::make_shared<const std::vector<uint8_t>>(std::move(itFs->second));
    gd.DebugName = debugName;
    gd.Rasterization.cullMode = CullModeFlagBits::None;
    // Unconditional depth write: writes require the test enabled, Always makes
    // every fragment pass.
    gd.DepthStencil.depthTestEnable = true;
    gd.DepthStencil.depthWriteEnable = true;
    gd.DepthStencil.depthCompareOp = CompareOp::Always;
    DynamicStateInfo dyn{};
    dyn.states = {DynamicState::Viewport, DynamicState::Scissor};
    gd.DynamicState = dyn;
    out.Set0Layout = DescriptorSetLayoutDesc{};
    auto patchLayout = [&out](uint32_t setIndex, DescriptorSetLayoutDesc& dsl)
    {
        if (setIndex == 0)
            out.Set0Layout = dsl;
    };
    std::string err;
    MaterialHelper::ApplyShaderMetaToGraphicsDesc(device, *out.Meta, gd, MaterialBuilder::MergeMode::Auto,
                                                  {true, 128}, patchLayout, &err);
    out.PipelineId = device.InternGraphicsPipeline(std::move(gd));
    return out.PipelineId.IsValid();
}

bool DepthUpsamplePass::EnsureLoaded(IDevice* device, bool multisampledSource)
{
    if (device == nullptr)
        return false;
    if (!m_Sampler.IsValid())
        m_Sampler = device->CreateSampler(SamplerDesc::MaterialLinearClamp("DepthUpsample.Clamp"));
    if (!m_SingleSample.PipelineId.IsValid() &&
        !LoadVariant(*device, "Shaders/taau_depth_upsample.shaderpkg", "DepthUpsample", m_SingleSample))
        return false;
    if (multisampledSource && !m_Multisampled.PipelineId.IsValid() &&
        !LoadVariant(*device, "Shaders/depth_copy_sample0.shaderpkg", "DepthCopySample0", m_Multisampled))
        return false;
    return true;
}

void DepthUpsamplePass::Declare(RenderGraph::RGFrame& frame,
                                RenderGraph::RGTexture internalDepth,
                                RenderGraph::RGTexture outputDepth, uint32_t outWidth,
                                uint32_t outHeight, const std::string& passName)
{
    if (!internalDepth.IsValid() || !outputDepth.IsValid() || outWidth == 0 || outHeight == 0)
        return;
    const bool multisampled = frame.Graph().ResourceDesc(internalDepth.Id).SampleCount > 1u;
    const Variant& variant = multisampled ? m_Multisampled : m_SingleSample;
    if (!variant.PipelineId.IsValid())
        return;

    frame.AddPass(
        passName.c_str(), Rendering::PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            RenderGraph::RGAttachmentOps depthOps{};
            depthOps.Load = RenderGraph::RGLoadOp::DontCare;
            depthOps.Store = RenderGraph::RGStoreOp::Store;
            p.AttachDepth(outputDepth, depthOps, RenderGraph::RGDepthAccess::ReadWrite);
            p.Read(internalDepth, RenderGraph::RGTextureRead::Sampled);
        },
        [this, &variant, internalDepth, outWidth, outHeight](RenderGraph::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl || !variant.Meta)
                return;
            const auto depthTex = ctx.GetTexture(internalDepth);
            if (!depthTex.IsValid() || !m_Sampler.IsValid())
                return;
            cl->SetViewport(0.0f, 0.0f, static_cast<float>(outWidth),
                            static_cast<float>(outHeight));
            cl->SetScissor(0, 0, outWidth, outHeight);

            DescriptorSetDesc dsDesc{};
            dsDesc.layout = variant.Set0Layout;
            dsDesc.transient = true;
            dsDesc.debugName = "DepthUpsample.Set0";
            auto ds = dev->CreateDescriptorSet(dsDesc);
            {
                NamedDescriptorWriter wd(dev, ds, *variant.Meta, 0);
                if (!wd.TryAddCombinedImageSampler("uDepth", depthTex, m_Sampler))
                {
                    if (!m_WarnedBindingMismatch)
                    {
                        m_WarnedBindingMismatch = true;
                        Logger::Log::Error(
                            "DepthUpsamplePass: descriptor name mismatch vs the depth copy "
                            "shader's reflection — pass skipped");
                    }
                    return;
                }
                wd.Flush();
            }
            const PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(variant.PipelineId);
            if (!pipe.IsValid())
                return;
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            cl->Draw(3, 1);
        });
}

} // namespace GameEngine::Engine::Renderer::Pipeline

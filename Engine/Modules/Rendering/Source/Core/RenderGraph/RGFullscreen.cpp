#include "Rendering/Core/RenderGraph/RGFullscreen.h"

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/ShaderLayoutShape.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <array>
#include <cassert>

namespace GameEngine::Rendering::RenderGraph
{

PipelineDesc MakeFullscreenPipelineDesc(const char* debugName)
{
    PipelineDesc pd{};
    pd.type = PipelineType::Graphics;
    pd.rasterizationSamples = 1;
    pd.EnableDepthTest(false);
    pd.SetCullingMode(CullModeFlagBits::None);
    pd.AddDynamicState(DynamicState::Viewport);
    pd.AddDynamicState(DynamicState::Scissor);
    pd.debugName = debugName;
    return pd;
}

RGPass AddFullscreenPass(RGFrame& frame, const RGFullscreenDesc& desc)
{
    assert(desc.Name && desc.Target.IsValid() && "AddFullscreenPass: name + target required");
    assert(desc.Textures.size() <= kMaxFullscreenTextureInputs &&
           "AddFullscreenPass: raise kMaxFullscreenTextureInputs");
    assert(desc.Buffers.size() <= kMaxFullscreenBufferInputs &&
           "AddFullscreenPass: raise kMaxFullscreenBufferInputs");

    // The spans die with the caller's frame; the exec lambda carries copies.
    std::array<RGFullscreenTextureInput, kMaxFullscreenTextureInputs> textures{};
    const uint32_t textureCount =
        static_cast<uint32_t>(std::min(desc.Textures.size(), textures.size()));
    for (uint32_t i = 0; i < textureCount; ++i)
        textures[i] = desc.Textures[i];
    std::array<RGFullscreenBufferInput, kMaxFullscreenBufferInputs> buffers{};
    const uint32_t bufferCount = static_cast<uint32_t>(std::min(desc.Buffers.size(), buffers.size()));
    for (uint32_t i = 0; i < bufferCount; ++i)
        buffers[i] = desc.Buffers[i];

    uint32_t width = desc.Width;
    uint32_t height = desc.Height;
    if (width == 0 || height == 0)
    {
        const RGResourceDesc& td = frame.Graph().ResourceDesc(desc.Target.Id);
        width = td.Width;
        height = td.Height;
    }

    return frame.AddPass(
        desc.Name, desc.Phase,
        [&](RGPassBuilder& p)
        {
            for (uint32_t i = 0; i < textureCount; ++i)
            {
                if (textures[i].Texture.IsValid())
                    p.Read(textures[i].Texture, RGTextureRead::Sampled);
            }
            for (uint32_t i = 0; i < bufferCount; ++i)
            {
                if (buffers[i].Declare.IsValid())
                    p.Read(buffers[i].Declare,
                           buffers[i].IsStorage ? RGBufferRead::Storage : RGBufferRead::Uniform);
            }
            p.AttachColor(0, desc.Target, desc.Ops);
        },
        [textures, textureCount, buffers, bufferCount, pipeline = desc.Pipeline,
         layout = desc.Layout, width, height](RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl)
                return;
            const auto pso = ctx.GetOrCreatePipelineVariant(pipeline);
            if (!pso.IsValid())
                return;

            // Resolve every texture input BEFORE creating the descriptor set: a
            // required input whose physical is missing skips the draw entirely
            // (the graph's cull already dropped truly-dead reads; this covers
            // declined imports), and a binding that would end up unwritten also
            // skips rather than feeding the driver an invalid descriptor.
            std::array<TextureHandle, kMaxFullscreenTextureInputs> resolved{};
            std::array<SamplerHandle, kMaxFullscreenTextureInputs> samplers{};
            for (uint32_t i = 0; i < textureCount; ++i)
            {
                const RGFullscreenTextureInput& in = textures[i];
                TextureHandle physical{};
                if (in.Texture.IsValid())
                {
                    physical = ctx.GetTexture(in.Texture);
                    if (!physical.IsValid() && in.Required)
                        return;
                }
                if (!physical.IsValid())
                    physical = in.RawTexture;
                SamplerHandle sampler = in.Sampler;
                if (!physical.IsValid())
                {
                    physical = in.Fallback;
                    if (in.FallbackSampler.IsValid())
                        sampler = in.FallbackSampler;
                }
                if (!physical.IsValid() || !sampler.IsValid())
                    return;
                resolved[i] = physical;
                samplers[i] = sampler;
            }
            for (uint32_t i = 0; i < bufferCount; ++i)
            {
                if (!buffers[i].Buffer.IsValid())
                    return; // e.g. an upload-ring allocation that failed at declare
            }

            DescriptorSetDesc dsDesc{};
            dsDesc.layout = layout;
            dsDesc.transient = true;
            dsDesc.debugName = "RGFullscreen.DS";
            const auto ds = dev->CreateDescriptorSet(dsDesc);
            for (uint32_t i = 0; i < textureCount; ++i)
                dev->UpdateCombinedImageSamplerBinding(ds, textures[i].Binding, resolved[i],
                                                       samplers[i]);
            for (uint32_t i = 0; i < bufferCount; ++i)
            {
                const RGFullscreenBufferInput& b = buffers[i];
                if (b.IsStorage)
                    dev->UpdateStorageBufferBinding(ds, b.Binding, b.Buffer,
                                                    static_cast<size_t>(b.Offset),
                                                    static_cast<size_t>(b.Size));
                else
                    dev->UpdateBufferBinding(ds, b.Binding, b.Buffer,
                                             static_cast<size_t>(b.Offset),
                                             static_cast<size_t>(b.Size));
            }

            cl->SetPipeline(pso);
            cl->SetViewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
            cl->SetScissor(0, 0, width, height);
            cl->BindDescriptorSet(0, ds, pso);
            cl->Draw(3, 1);
        });
}

RGPass AddCopyPass(RGFrame& frame, RGTexture source, RGTexture dest, const char* name,
                   int32_t phase, GraphicsPipelineId pipeline, GraphicsPipelineId msaaPipeline,
                   const DescriptorSetLayoutDesc& layout, const DescriptorSetLayoutDesc& msaaLayout,
                   SamplerHandle sampler, uint32_t width, uint32_t height)
{
    const RGResourceDesc& sd = frame.Graph().ResourceDesc(source.Id);
    const bool msaa = sd.SampleCount > 1;

    RGFullscreenTextureInput input{};
    input.Binding = 0;
    input.Texture = source;
    input.Sampler = sampler;
    input.Required = true;

    RGFullscreenDesc desc{};
    desc.Name = name;
    desc.Phase = phase;
    desc.Pipeline = msaa ? msaaPipeline : pipeline;
    desc.Layout = msaa ? msaaLayout : layout;
    desc.Textures = std::span<const RGFullscreenTextureInput>(&input, 1);
    desc.Target = dest;
    desc.Ops.Load = RGLoadOp::DontCare;
    desc.Ops.Store = RGStoreOp::Store;
    desc.Width = width;
    desc.Height = height;
    return AddFullscreenPass(frame, desc);
}

bool LoadCopyPipelineDesc(ShaderSourceKind kind, const char* pipelineName, const char* layoutName,
                          DescriptorSetLayoutDesc& outLayout, PipelineDesc& outPipeline)
{
    ShaderPackage pkg{};
    if (!LoadShaderPkg("Shaders/copy.shaderpkg", kind, pkg))
        return false;
    const auto vs = pkg.stageBytes.find("vs");
    const auto fs = pkg.stageBytes.find("fs");
    if (vs == pkg.stageBytes.end() || fs == pkg.stageBytes.end() || vs->second.empty() ||
        fs->second.empty())
        return false;

    outLayout = {};
    outLayout.debugName = layoutName;
    outLayout.bindings = {{0, DescriptorType::CombinedImageSampler, 1, kShaderStageFragment}};
    ApplyMetaImageShapeToLayout(pkg.meta, outLayout);

    outPipeline = MakeFullscreenPipelineDesc(pipelineName);
    outPipeline.vertexShader = vs->second;
    outPipeline.pixelShader = fs->second;
    outPipeline.descriptorSetLayouts.push_back(outLayout);
    return true;
}

} // namespace GameEngine::Rendering::RenderGraph

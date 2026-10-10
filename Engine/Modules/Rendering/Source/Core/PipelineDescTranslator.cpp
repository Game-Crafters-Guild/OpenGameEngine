#include "Rendering/Core/PipelineDescTranslator.h"

#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"

#include <algorithm>
#include <utility>

namespace GameEngine::Rendering::PipelineDescTranslator
{

namespace
{

void CopyNamedPushConstantRanges(const PipelineDesc& base,
                                 std::vector<NamedPushConstantRange>& out)
{
    out.reserve(base.pushConstantRanges.size());
    for (const auto& r : base.pushConstantRanges)
    {
        NamedPushConstantRange nr{};
        nr.Name = r.name;
        nr.Offset = r.offset;
        nr.Size = r.size;
        nr.StageMask = r.stagesMask;
        out.push_back(std::move(nr));
    }
}

} // anonymous namespace

GraphicsPipelineId InternGraphics(IDevice& device, const PipelineDesc& base)
{
    GraphicsPipelineDesc gd{};
    gd.Kind = (base.type == PipelineType::Mesh)
                  ? GraphicsPipelineKind::MeshFragment
                  : GraphicsPipelineKind::VertexFragment;
    if (!base.vertexShader.empty())
        gd.VertexShader = std::make_shared<const std::vector<uint8_t>>(base.vertexShader);
    if (!base.pixelShader.empty())
        gd.PixelShader = std::make_shared<const std::vector<uint8_t>>(base.pixelShader);
    if (!base.meshShader.empty())
        gd.MeshShader = std::make_shared<const std::vector<uint8_t>>(base.meshShader);
    if (!base.amplificationShader.empty())
        gd.AmplificationShader = std::make_shared<const std::vector<uint8_t>>(base.amplificationShader);
    gd.Rasterization = base.rasterizationState;
    gd.DepthStencil  = base.depthStencilState;
    gd.ColorBlend    = base.colorBlendState;
    gd.Topology      = base.topology;
    if (!base.dynamicState.states.empty())
        gd.DynamicState = base.dynamicState;
    gd.PushConstants.Size      = base.pushConstantSize;
    gd.PushConstants.StageMask = base.pushConstantStagesMask;
    CopyNamedPushConstantRanges(base, gd.NamedPushConstantRanges);
    gd.VertexBindings   = base.vertexBindings;
    gd.VertexAttributes = base.vertexAttributes;
    gd.DescriptorSetLayouts.reserve(base.descriptorSetLayouts.size());
    for (const auto& dsl : base.descriptorSetLayouts)
        gd.DescriptorSetLayouts.push_back(device.InternDescriptorSetLayout(dsl));
    if (base.debugName)
        gd.DebugName = base.debugName;
    return device.InternGraphicsPipeline(std::move(gd));
}

ComputePipelineId InternCompute(IDevice& device, const PipelineDesc& base)
{
    ComputePipelineDesc cd{};
    if (!base.computeShader.empty())
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(base.computeShader);
    cd.PushConstants.Size      = base.pushConstantSize;
    cd.PushConstants.StageMask = base.pushConstantStagesMask;
    CopyNamedPushConstantRanges(base, cd.NamedPushConstantRanges);
    cd.DescriptorSetLayouts.reserve(base.descriptorSetLayouts.size());
    for (const auto& dsl : base.descriptorSetLayouts)
        cd.DescriptorSetLayouts.push_back(device.InternDescriptorSetLayout(dsl));
    if (base.debugName)
        cd.DebugName = base.debugName;
    return device.InternComputePipeline(std::move(cd));
}

PipelineFormatKey BuildFormatKey(const PipelineDesc& base)
{
    PipelineFormatKey fk{};
    fk.ColorCount = static_cast<uint8_t>(std::min<size_t>(base.colorAttachmentFormats.size(),
                                                          PipelineFormatKey::kMaxColors));
    for (uint8_t i = 0; i < fk.ColorCount; ++i)
        fk.ColorFormats[i] = static_cast<TextureFormat>(base.colorAttachmentFormats[i]);
    fk.DepthFormat   = static_cast<TextureFormat>(base.depthAttachmentFormat);
    fk.StencilFormat = static_cast<TextureFormat>(base.stencilAttachmentFormat);
    fk.RasterizationSamples = static_cast<uint8_t>(base.rasterizationSamples == 0u ? 1u : base.rasterizationSamples);
    return fk;
}

} // namespace GameEngine::Rendering::PipelineDescTranslator

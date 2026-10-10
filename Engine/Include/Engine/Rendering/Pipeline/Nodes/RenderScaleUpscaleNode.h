#pragma once

#include "Engine/Rendering/Pipeline/DepthUpsamplePass.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Materials/ShaderMeta.h"

#include <memory>
#include <string>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
// The internal-resolution crossing for views that do NOT run temporal AA.
//
// Under an internal-resolution split the world half of the chain rasterizes at
// the internal extent and everything after the crossing works at the display
// extent (the blueprint's extent.basis distinction). A TAA view crosses inside
// its temporal resolve; every other view crosses here, and nowhere else — the
// node exists so the boundary is one named, inspectable pass instead of an
// implicit resample wherever the first output-basis target happens to be bound.
//
// Three things happen at the crossing, matching the temporal resolve exactly:
//   * colour is resampled from the internal extent to the display extent
//     (spatial_upscale.frag, Catmull-Rom);
//   * View.Resolve is republished to the caller's display-res colour, so the
//     terminal stage writes the display target rather than the internal one;
//   * the caller's depth is reconstituted at the display extent, because the
//     editor's overlay and gizmo passes depth-test against it.
//
// Inert whenever the split is inactive or another node already crossed: the
// input is stitched through under the output name and no pass is declared. At
// render scale 1.0 that is every frame, so the node costs nothing.
//
// JSON shape: { "id": "RenderScaleUpscale", "type": "RenderScaleUpscale",
//               "input": "HDRTemporalAA", "output": "HDRUpscaled" }
class RenderScaleUpscaleNode final : public IRenderPipelineNode
{
  public:
    const char* GetTypeName() const override { return "RenderScaleUpscale"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void DeclareForView(ViewDeclare& d) override;

  private:
    bool LoadShaders(::GameEngine::Rendering::IDevice* device);

    std::string m_InputKey = "HDRTemporalAA";
    std::string m_OutputKey = "HDRUpscaled";
    std::string m_Id;

    ::GameEngine::Rendering::GraphicsPipelineId m_UpscalePipelineId{};
    std::unique_ptr<::GameEngine::Rendering::ShaderMeta> m_UpscaleMeta;
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_UpscaleSet0Layout{};
    ::GameEngine::Rendering::SamplerHandle m_LinearClampSampler{};
    DepthUpsamplePass m_DepthUpsample;

    bool m_WarnedOutputBasis = false;
    bool m_WarnedBindingMismatch = false;
    bool m_WarnedShadersUnstaged = false;
};

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes

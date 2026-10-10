#pragma once

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Materials/ShaderMeta.h"

#include <memory>
#include <string>

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
using ::GameEngine::Rendering::ComputePipelineId;
using ::GameEngine::Rendering::DescriptorSetLayoutDesc;
using ::GameEngine::Rendering::SamplerHandle;
using ::GameEngine::Rendering::ShaderMeta;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
// SMAA 1x (Jimenez 2012), run at the END of the LDR post chain like the FXAA
// node. Purely spatial — no jitter, no history, no temporal state — which
// makes it the temporally rock-solid post AA: static content is bit-stable
// frame to frame. Three compute passes per SMAA-enabled view:
//   smaa_edges   — luma edge detection with local contrast adaptation.
//   smaa_weights — edge-end searches + analytic revectorization areas
//                  (LUT-free; orthogonal patterns only, no diagonal pass).
//   smaa_blend   — neighborhood blending along the dominant axis.
//
// The node is the SMAA gate for the LDR tail: when the view has no
// ViewAntiAliasing state, or its mode is not SMAA, it stitches its input
// through under the output name so downstream consumers bind one name
// unconditionally.
//
// JSON shape: { "id": "Smaa", "type": "Smaa",
//               "input": "LDRFxaa", "output": "LDRSmaa" }
class SmaaNode final : public IRenderPipelineNode
{
  public:
    ~SmaaNode() override;

    const char* GetTypeName() const override { return "Smaa"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void Declare(RenderPipelineInstance& instance, const PipelineDeclareContext& ctx) override;
    void DeclareForView(ViewDeclare& d) override;

  private:
    void LoadShaders(IDevice* device);

    std::string m_InputKey = "LDRFxaa";
    std::string m_OutputKey = "LDRSmaa";
    std::string m_Id;

    bool m_ShadersLoaded = false; // latches on SUCCESS; failed loads retry next frame
    bool m_WarnedUnresolvedInput = false;
    bool m_WarnedBindingMismatch = false;

    ComputePipelineId m_EdgesPipelineId{};
    std::unique_ptr<ShaderMeta> m_EdgesMeta;
    DescriptorSetLayoutDesc m_EdgesSet0Layout{};

    ComputePipelineId m_WeightsPipelineId{};
    std::unique_ptr<ShaderMeta> m_WeightsMeta;
    DescriptorSetLayoutDesc m_WeightsSet0Layout{};

    ComputePipelineId m_BlendPipelineId{};
    std::unique_ptr<ShaderMeta> m_BlendMeta;
    DescriptorSetLayoutDesc m_BlendSet0Layout{};

    SamplerHandle m_LinearClampSampler{};
    // The device the sampler above was created on, so the destructor can
    // return it; null until LoadShaders succeeds.
    IDevice* m_Device = nullptr;
};

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes

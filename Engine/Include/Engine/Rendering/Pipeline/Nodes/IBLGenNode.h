#pragma once

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Rendering/Core/PipelineIdentifiers.h" // ComputePipelineId

#include <string>

namespace GameEngine
{
namespace Rendering
{
class IDevice;
} // namespace Rendering

namespace Engine::Renderer
{
class ImageBasedLightingFeature;
} // namespace Engine::Renderer
} // namespace GameEngine

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
using ::GameEngine::Rendering::ComputePipelineId;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{

// Frame-scope node that owns the engine-shared image-based-lighting bake:
// it ensures the ImageBasedLightingFeature exists, bakes the environment-
// independent split-sum BRDF LUT once, wires the active environment source
// (the analytic sky), and asks that source to (re)bake the irradiance +
// prefilter cubes whenever the sky changes. Registered with perView=false —
// the environment is view-independent, so one bake is shared across all views.
class IBLGenNode final : public IRenderPipelineNode
{
  public:
    const char* GetTypeName() const override { return "IBLGen"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void Declare(RenderPipelineInstance& instance, const PipelineDeclareContext& ctx) override;

  private:
    // One-shot bake of the 512x512 split-sum BRDF LUT (environment-independent).
    void ScheduleBrdfLutBake(Rendering::RenderGraph::RGFrame& frame,
                             GameEngine::Rendering::IDevice& device,
                             Engine::Renderer::ImageBasedLightingFeature& feature);

    std::string m_Id;
    Rendering::ComputePipelineId m_BrdfPipelineId{};
    bool m_BrdfShaderTried = false;
};

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes

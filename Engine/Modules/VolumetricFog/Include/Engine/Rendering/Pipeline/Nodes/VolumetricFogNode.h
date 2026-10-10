#pragma once

#include "Engine/Rendering/Pipeline/Nodes/DepthResolveNode.h"
#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

#include <string>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{

class VolumetricFogNode final : public IRenderPipelineNode
{
  public:
    const char* GetTypeName() const override { return "VolumetricFog"; }
    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void Declare(RenderPipelineInstance& instance, const PipelineDeclareContext& ctx) override;
    void DeclareForView(ViewDeclare& d) override;

  private:
    std::string m_Id;
    std::string m_Json;
    std::string m_OutputRef = Names::View::Resolve;
    // The view depth after every world depth writer (the phase-B recover, forward draws that write
    // their own depth, the water), published as View.DepthResolvedPostOcean. The ocean surface step
    // publishes it on frames the water draws; on other fog frames this resolve takes it, so the fog
    // never reads the copy taken right after the prepass, and the auto-exposure meter after the fog
    // reads the same depth.
    DepthResolveNode m_PostWorldDepthResolve;
};

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes

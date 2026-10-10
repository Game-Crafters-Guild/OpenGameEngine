#pragma once

#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

#include <string>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{

// Per-view node that draws screen-space lens flares as one additive instanced
// quad batch, after the world pass. Ensures the LensFlareRenderFeature exists and
// consumes the per-frame instance data the extraction system pushed.
//
// Configured via .rendergraph JSON:
// { "type": "LensFlare", "enabled": true, "output": "View.Resolve" }
class LensFlareRenderNode final : public IRenderPipelineNode
{
  public:
    const char* GetTypeName() const override { return "LensFlare"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void DeclareForView(ViewDeclare& d) override;

  private:
    std::string m_Id;
    std::string m_OutputRef = Names::View::Resolve;
};

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes

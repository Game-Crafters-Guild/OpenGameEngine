#pragma once

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
// Imports an engine-owned CPU texture revision into the current view's graph.
// {"type":"CpuTextureInput", "source":"Visibility", "output":"SightMask",
//  "parameters":"SightBounds", "viewPurpose":"Game"}
// parameters is optional, viewPurpose defaults to Game. Missing/unready sources
// publish neither resource. The consuming composition owns its missing-input
// policy; this generic producer does not supply a fog/visibility fallback.
class CpuTextureInputNode final : public IRenderPipelineNode
{
  public:
    const char* GetTypeName() const override { return "CpuTextureInput"; }
    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void DeclareForView(ViewDeclare& d) override;

  private:
    StringId m_Source = 0;
    std::string m_Output, m_Parameters;
    Rendering::ViewPurpose m_Purpose = Rendering::ViewPurpose::Game;
};
} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes

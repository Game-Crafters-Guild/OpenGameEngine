#pragma once

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{

// Uploads ColorGradeParamsUBO from resolved PostProcessSettings each frame.
// JSON:
// {
//   "id": "ColorGradeParamsUpload",
//   "type": "ColorGradeParamsUpload",
//   "buffer": "ColorGradeParams"
// }
class ColorGradeParamsUploadNode final : public IRenderPipelineNode
{
  public:
    const char* GetTypeName() const override { return "ColorGradeParamsUpload"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void DeclareForView(ViewDeclare& d) override;

  private:
    std::string m_Id;
    std::string m_Json;
    std::string m_BufferRef = "ColorGradeParams";
};

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes

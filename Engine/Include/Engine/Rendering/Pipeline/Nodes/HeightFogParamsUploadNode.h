#pragma once

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{

// Uploads HeightFogParamsUBO from resolved PostProcessSettings each frame.
// JSON:
// {
//   "id": "HeightFogParamsUpload",
//   "type": "HeightFogParamsUpload",
//   "buffer": "HeightFogParams"
// }
class HeightFogParamsUploadNode final : public IRenderPipelineNode
{
  public:
    const char* GetTypeName() const override { return "HeightFogParamsUpload"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void DeclareForView(ViewDeclare& d) override;

  private:
    std::string m_Id;
    std::string m_Json;
    std::string m_BufferRef = "HeightFogParams";
};

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes

#pragma once

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

#include <string>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
// Uploads the per-frame extracted light list into a pipeline-defined buffer (per-view).
//
// This node is intentionally CPU-driven (maps an Upload buffer) as a bootstrap step for Forward+.
// Later iterations can switch to staging buffers + Copy passes, and per-world/per-view filtering.
//
// JSON shape (schemaVersion 2):
// {
//   "id": "LightUpload",
//   "type": "LightUpload",
//   "enabled": true,
//   "buffer": "LightBuffer" // optional (default "LightBuffer")
// }
class LightUploadNode final : public IRenderPipelineNode
{
  public:
    const char* GetTypeName() const override { return "LightUpload"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void DeclareForView(ViewDeclare& d) override;

  private:
    std::string m_Id;
    std::string m_Json;
    std::string m_BufferRef = "LightBuffer";
};
} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes

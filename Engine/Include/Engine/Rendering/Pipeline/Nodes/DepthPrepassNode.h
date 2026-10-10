#pragma once

#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

#include <string>
#include <unordered_set>

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
using ::GameEngine::Rendering::ViewId;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
// Minimal depth prepass node (placeholder):
// - Attaches View.Depth and clears it.
// - Does not render geometry yet (future work: depth-only draw of world meshes).
//
// JSON shape (schemaVersion 2):
// {
//   "id": "DepthPrepass",
//   "type": "DepthPrepass",
//   "enabled": true,
//   "depth": "View.Depth",     // optional ref override
//   "clearDepthValue": 0.0     // optional (default 0.0 — reverse-Z far)
// }
class DepthPrepassNode final : public IRenderPipelineNode
{
  public:
    const char* GetTypeName() const override { return "DepthPrepass"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void DeclareForView(ViewDeclare& d) override;

  private:
    std::string m_Id;
    std::string m_Json;

    std::string m_DepthRef = Names::View::Depth;
    float m_ClearDepthValue = 0.0f;  // reverse-Z: clear to far

    // Views for which a sample-count/size-mismatched depth override was already
    // reported, so the per-view declare doesn't re-log every frame. Empty
    // unless a pipeline declares a malformed "depth" override.
    std::unordered_set<ViewId> m_DepthMismatchWarned;
};
} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes

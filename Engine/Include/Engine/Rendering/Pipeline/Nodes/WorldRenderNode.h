#pragma once

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include "Rendering/Core/RenderGraph/RGBarrier.h"

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
using ::GameEngine::Rendering::MaterialKeyword;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
// Parse the optional "keywords" string array out of a WorldRender pass's JSON
// into a MaterialKeyword mask. This is the single source of truth for the
// blueprint->keyword mapping: WorldRenderNode applies it at declaration time,
// and material prewarm reads it from the active blueprint so the variants it
// compiles match the keywords the world draw will actually request.
Rendering::MaterialKeyword ParseWorldPassKeywords(const std::string& passJson);

// Per-view node that schedules the standard world/entities pass for each active view.
// Parses optional "keywords" from .rendergraph JSON to inject pass-level shader
// keywords (e.g. ForwardPlus, Instanced) into material variant selection at draw time.
class WorldRenderNode final : public IRenderPipelineNode
{
  public:
    const char* GetTypeName() const override { return "WorldRender"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void DeclareForView(ViewDeclare& d) override;

  private:
    std::string m_Id;
    std::string m_Json;
    Rendering::MaterialKeyword m_PassKeywords = Rendering::MaterialKeyword::None;
};

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes

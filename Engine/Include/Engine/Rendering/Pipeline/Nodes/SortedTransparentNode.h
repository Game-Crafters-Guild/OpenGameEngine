#pragma once

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Rendering/Materials/ShaderVariantKey.h"

#include <string>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
// Per-view node that drains the sorted transparent (order-dependent Blend) draw
// set, relocated out of WorldRenderNode so it renders AFTER HZBBuild /
// OcclusionCullP2 / ScatterB (transparency-scale design S1). Culling transparents
// against the current-frame opaque HZB requires the pass to run after the HZB is
// built; keeping the drain inside WorldRenderNode drew it before HZBBuild, which
// could not observe the HZB. WorldRenderNode still BUILDS the sorted set (so the
// opaque batch peel sees the active flag); this node declares the drain.
//
// S2: the drain is GPU-driven — a fused gather+key+sort+scatter dispatch
// (sorted_transparent_drain.comp) followed by one DrawIndexedIndirectCount per
// (surface×blend) run, drawing through the GE_INSTANCED buffer-reference path
// into the same HDR SceneColor + depth the world pass wrote. See
// RenderServices::AddSortedTransparentDrainForView.
//
// JSON shape (place AFTER the ScatterB / Transmissive entries):
// {
//   "id": "SortedTransparent",
//   "type": "SortedTransparent",
//   "enabled": true,
//   "keywords": ["ForwardPlus", "Instanced", "Shadows", "IBL"]
// }
// The "keywords" array MUST stay in sync with the WorldRender node's: the drain
// selects the material color variant from these keywords, so a mismatch compiles
// a different variant than the opaque pass and the transparents shade
// inconsistently.
class SortedTransparentNode final : public IRenderPipelineNode
{
  public:
    const char* GetTypeName() const override { return "SortedTransparent"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void DeclareForView(ViewDeclare& d) override;

  private:
    std::string m_Id;
    std::string m_Json;
    Rendering::MaterialKeyword m_PassKeywords = Rendering::MaterialKeyword::None;
};

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes

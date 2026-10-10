#pragma once

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

#include <string>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
// Two-phase HZB occlusion, phase-B scatter (R2.1, design §1 SCATTER B). The
// second ScheduleUnifiedScatter of the app frame for the main view: registers a
// phase-B slice bound to the reserved phase-B visibility slice and appends a
// scatter into the shared arena, publishing phase-B draw ranges the recover
// consumers look up (AddWorldDepthRecoverPassForView; the color recover pass
// lands with the P2 arc). Delegates the whole registration to
// RenderServices::ScheduleWorldOcclusionRecoverScatterForView, which no-ops for
// any view without a phase-B reservation.
//
// Declaration order: after OcclusionCullP2 (it consumes the phase-B visibility
// that pass writes). Wired and enabled in the shipped blueprints
// (Assets/RenderPipelines/ForwardPlus.rendergraph and
// ForwardPlus_DebugOverlay.rendergraph); a headless fixture with no
// AssetManager pins a blueprint of its own that carries only WorldRender, so it
// never declares it.
//
// The pass it declares is "GPUDrawStream.Scatter.World.B", and the spine
// bucketer's "GPUDrawStream.Scatter.World" is a strict PREFIX of that: any
// matcher over these pass names must compare by equality, not substring.
//
// JSON shape:
// {
//   "id": "ScatterB",
//   "type": "ScatterB",
//   "enabled": true
// }
class ScatterBNode final : public IRenderPipelineNode
{
  public:
    const char* GetTypeName() const override { return "ScatterB"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void DeclareForView(ViewDeclare& d) override;

  private:
    std::string m_Id;
};
} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes

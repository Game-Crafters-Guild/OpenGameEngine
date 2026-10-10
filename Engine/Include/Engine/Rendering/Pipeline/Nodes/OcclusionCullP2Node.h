#pragma once

#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

#include <string>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
// Two-phase HZB occlusion, phase-2 cull dispatch (R2.1, design §5-A1). Fires
// GPUCullingPipeline::ScheduleOcclusionCullPass for the view, which dispatches
// the deferred phase-B cull into the visibility slice EndFrameRG reserved
// (ViewCullingInput::reserveOcclusionSlice). The pass no-ops for any view that
// did not reserve a phase-B slice (i.e. every non-HZB view), so it is safe to
// declare per view.
//
// Declaration order (when wired into a blueprint): after HZBBuild (the cull
// samples the freshly-built pyramid once the P2 cull variant lands) and before
// ScatterB (which consumes the phase-B visibility this writes). The seam's RG
// edges are real today; the occlusion TEST itself arrives with the P2 cull
// shader variant.
//
// Scaffold: registered but added to no blueprint (default OFF).
//
// JSON shape:
// {
//   "id": "OcclusionCullP2",
//   "type": "OcclusionCullP2",
//   "enabled": true
// }
class OcclusionCullP2Node final : public IRenderPipelineNode
{
  public:
    const char* GetTypeName() const override { return "OcclusionCullP2"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void DeclareForView(ViewDeclare& d) override;

  private:
    std::string m_Id;
    // Blackboard key of the HZB pyramid (HZBBuild publishes "View.HZB"). This
    // node must sit after HZBBuild in the blueprint so the ref resolves.
    std::string m_InputKey = Names::View::HZB;
};
} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes

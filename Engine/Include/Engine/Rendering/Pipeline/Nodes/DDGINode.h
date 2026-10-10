#pragma once

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

#include <string>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{

// Frame-scope node that drives the engine-shared DDGI probe field: ensures
// DDGIProbeFeature + its shared-BLAS-pool TLAS channel exist, sweeps the
// shared SceneAccelerationStructureService once for this frame (upstream of
// DDGIProbeFeature's own BLAS/TLAS use), then asks the feature to declare
// its trace/blend/upload passes. Registered with perView=false — the probe
// field is world-space and view-independent, same reasoning as IBLGenNode.
// Declares nothing (zero cost) while no world has an enabled DDGIVolume.
class DDGINode final : public IRenderPipelineNode
{
  public:
    const char* GetTypeName() const override { return "DDGIGen"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void Declare(RenderPipelineInstance& instance, const PipelineDeclareContext& ctx) override;

  private:
    std::string m_Id;
};

}  // namespace GameEngine::Engine::Renderer::Pipeline::Nodes

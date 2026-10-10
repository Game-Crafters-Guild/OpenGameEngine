#pragma once

#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

#include <string>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{

// Per-view node that composites the sun's veiling glare: one additive fullscreen draw whose
// radiance is an analytic point-spread function of the sun's UNCLAMPED irradiance, centred
// on the sun's direction and gated by whether scene geometry hides the sun.
//
// It is analytic rather than a blur of the frame because SceneColor cannot carry the sun:
// the drawn disc is clamped to the fp16 storage bound, which is a fraction of a percent of
// the sun's energy, so there is nothing in the buffer for a bloom pyramid to spread.
//
// No user dials. The term is on whenever the sky draws a sun disc, and off — the pass is not
// even declared — when it does not, when the sun is below the horizon, or when the sun
// carries no irradiance.
//
// Configured via .rendergraph JSON:
// { "type": "SunGlare", "enabled": true, "output": "HDRFogGlow" }
class SunGlareRenderNode final : public IRenderPipelineNode
{
  public:
    const char* GetTypeName() const override { return "SunGlare"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void DeclareForView(ViewDeclare& d) override;

  private:
    std::string m_Id;
    std::string m_OutputRef = Names::View::Resolve;
};

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes

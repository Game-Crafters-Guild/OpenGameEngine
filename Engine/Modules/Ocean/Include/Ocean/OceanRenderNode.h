#pragma once

#include "Engine/Rendering/Pipeline/Nodes/DepthResolveNode.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Ocean/OceanUnderwater.h"

#include <string>

namespace GameEngine::Ocean
{

// Per-view pipeline node for the ocean. Ensures the OceanRenderFeature and its
// forward contributor exist; from the FFT phase onward it also schedules the
// wave-simulation compute passes. The actual surface draw is injected into the
// world forward pass by OceanForwardContributor.
//
// Configured via .rendergraph JSON: { "type": "OceanRender", "enabled": true }
class OceanRenderNode final : public Engine::Renderer::Pipeline::IRenderPipelineNode
{
public:
    ~OceanRenderNode() override;

    const char* GetTypeName() const override { return "OceanRender"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void Declare(Engine::Renderer::Pipeline::RenderPipelineInstance& instance,
                 const Engine::Renderer::Pipeline::PipelineDeclareContext& ctx) override;
    void DeclareForView(Engine::Renderer::Pipeline::ViewDeclare& d) override;

private:
    std::string m_Id;
    std::string m_Json;
    // A second instance of this node placed AFTER the World node (id contains
    // "Underwater") only declares the submerged fullscreen overlay — it must run
    // post-world to read the fully-drawn scene colour. The pre-world instance does
    // all the setup/sims and stamps the submersion depth onto the feature.
    bool m_UnderwaterMode = false;
    // Reused every frame by the post-world instance for the underwater pass.
    OceanUnderwaterPortalData m_PortalData;
    // The post-world instance, which draws the surface, resolves the view depth once right after
    // the surface pass and publishes it as View.DepthResolvedPostOcean: the view depth after the
    // world and the water, which the volumetric fog (the water's own distance) and the
    // auto-exposure meter (the sea is scene, not sky) read. Declared only on frames the surface
    // draws for the view; on other frames the fog, when it runs, takes the same resolve itself.
    Engine::Renderer::Pipeline::Nodes::DepthResolveNode m_PostOceanDepthResolve;
};

} // namespace GameEngine::Ocean

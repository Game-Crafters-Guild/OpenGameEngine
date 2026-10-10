#include "Engine/Rendering/Pipeline/Nodes/OcclusionCullP2Node.h"

#include "Engine/Rendering/RenderServices.h"

#include "Rendering/Core/GPUCulling.h"
#include "Rendering/Core/RenderGraph/RGGraph.h"

#include <nlohmann/json.hpp>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

bool OcclusionCullP2Node::Initialize(std::string nodeId, std::string nodeJson,
                                     std::string* /*outError*/)
{
    m_Id = std::move(nodeId);
    try
    {
        auto j = nlohmann::json::parse(nodeJson);
        if (j.is_object() && j.contains("input") && j["input"].is_string())
            m_InputKey = j["input"].get<std::string>();
    }
    catch (const std::exception&)
    {
        // Keep the default input key on malformed config.
    }
    return true;
}

void OcclusionCullP2Node::DeclareForView(ViewDeclare& d)
{
    GPUCullingPipeline* cull = d.Services.GetGPUCullingPipeline();
    GPUScene* scene = d.Services.GetGPUScene();
    if (!cull || !scene)
        return;
    // Resolve the pyramid HZBBuild published (declaration order: this node must
    // sit after HZBBuild in the blueprint). Its mip count drives the shader's
    // mip clamp. When "View.HZB" is unpublished (node unwired) the texture is
    // invalid and ScheduleOcclusionCullPass no-ops, leaving the reservation
    // unconsumed — safe while ScatterB is unwired.
    const RenderGraph::RGTexture hzb = d.ResolveTexture(m_InputKey);
    uint32_t mipCount = 0u;
    if (hzb.IsValid())
        mipCount = d.Frame.Graph().ResourceDesc(hzb.Id).MipLevels;
    // No-ops for any view without a phase-B reservation (every non-HZB view).
    cull->ScheduleOcclusionCullPass(d.Frame, scene, d.View.id, hzb, mipCount);
}
} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes

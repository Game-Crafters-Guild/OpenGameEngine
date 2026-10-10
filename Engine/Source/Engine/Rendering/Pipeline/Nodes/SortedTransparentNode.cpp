#include "Engine/Rendering/Pipeline/Nodes/SortedTransparentNode.h"

#include "Engine/Rendering/Pipeline/Nodes/WorldRenderNode.h" // ParseWorldPassKeywords
#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/RenderServices.h"

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

bool SortedTransparentNode::Initialize(std::string nodeId, std::string nodeJson, std::string* /*outError*/)
{
    m_Id = std::move(nodeId);
    m_Json = std::move(nodeJson);
    // Same keyword source as WorldRenderNode so the drained variant matches the
    // world pass. Instanced STAYS (S2): the run draws fetch per-record data
    // through the GE_INSTANCED buffer-reference path.
    m_PassKeywords = ParseWorldPassKeywords(m_Json);
    return true;
}

void SortedTransparentNode::DeclareForView(ViewDeclare& d)
{
    if (!d.ViewColor.IsValid() && !d.ViewDepth.IsValid())
        return;

    // WorldRenderNode built the sorted set earlier in the frame (before its
    // opaque peel). If the sort did not engage for this view (no
    // order-dependent Blend in sight), Active is false and the Blend draws
    // rode the batched path — nothing to drain here. The drain itself
    // re-checks under the same frame guard.
    if (!d.Services.IsSortedTransparentActive(d.View.id))
        return;

    // Reconstruct the world targets from this node's own view-declare (same
    // pattern as ScatterBNode) so the drain writes the same MSAA colour + depth
    // and re-resolves into SceneColor. Because this node is declared after
    // HZBBuild / OcclusionCullP2 / ScatterB, the transparents blend over both
    // opaque phases (phase A + the phase-B recover) instead of before the recover.
    Engine::Renderer::RenderServices::WorldPassTargetsRG t{};
    t.Color = d.ViewColor;
    t.Depth = d.ViewDepth;
    t.Resolve = d.ViewResolve;
    t.DepthResolved = d.ViewDepthResolved;
    t.GTAO = d.ResolveTexture(Names::View::GTAO);
    t.ResolveFromPipeline = d.ResolveFromPipeline;

    d.Services.AddSortedTransparentDrainForView(d.Frame, d.View.id, t, m_PassKeywords);
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes

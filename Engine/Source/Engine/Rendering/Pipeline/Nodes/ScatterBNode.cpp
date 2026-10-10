#include "Engine/Rendering/Pipeline/Nodes/ScatterBNode.h"

#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/RenderServices.h"

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

bool ScatterBNode::Initialize(std::string nodeId, std::string /*nodeJson*/, std::string* /*outError*/)
{
    m_Id = std::move(nodeId);
    return true;
}

void ScatterBNode::DeclareForView(ViewDeclare& d)
{
    // Register + schedule the phase-B recover scatter. Returns false for any
    // view without a reserved phase-B slice (every non-HZB view) — nothing to
    // recover, so declare no recover passes.
    if (!d.Services.ScheduleWorldOcclusionRecoverScatterForView(d.Frame, d.View.id))
        return;

    // Phase-B depth recover: loads the phase-A depth and depth-tests the
    // newly-revealed fragments into it (GreaterOrEqual), consuming SlicePhase::B
    // ranges. Declared BEFORE the colour recover so the colour pass sees the
    // depth prepass and binds read-only depth.
    d.Services.AddWorldDepthRecoverPassForView(d.Frame, d.View.id, d.ViewDepth);

    // Phase-B colour recover into the SAME targets + keywords the phase-A world
    // pass used (targets reconstructed from the view-declare exactly like
    // WorldRenderNode; keywords read back from what phase A recorded). Additive:
    // loads colour+depth, no clears, publishes nothing.
    const auto keywords = d.Services.GetWorldPassKeywords(d.View.id);
    if (!keywords)
        return; // the phase-A world pass never ran for this view

    Engine::Renderer::RenderServices::WorldPassTargetsRG t{};
    t.Color = d.ViewColor;
    t.Depth = d.ViewDepth;
    t.Resolve = d.ViewResolve;
    t.DepthResolved = d.ViewDepthResolved;
    t.OccluderDepthResolved = d.ViewOccluderDepthResolved;
    t.GTAO = d.ResolveTexture(Names::View::GTAO);
    // Same DDGI glossy-resolve outputs the phase-A pass bound (published by
    // WorldRenderNode only on frames the resolve pass was declared), so
    // recovered geometry reads the same scaled reflections. Invalid when the
    // resolve is inactive — the binder then supplies the black fallback and
    // the UBO flag keeps the shader on the inline gather.
    t.DDGIResolveRough = d.ResolveTexture(Names::View::DDGIResolveRough);
    t.DDGIResolveGlossy = d.ResolveTexture(Names::View::DDGIResolveGlossy);
    t.ResolveFromPipeline = d.ResolveFromPipeline;

    const auto scope =
        (d.Services.HasTransmissionInView(d.View.id) && d.PipelineHasTransmissivePass)
            ? Engine::Renderer::RenderServices::WorldPassDrawScope::OpaqueOnly
            : Engine::Renderer::RenderServices::WorldPassDrawScope::All;

    d.Services.AddWorldColorRecoverPassForView(d.Frame, d.View.id, t, *keywords, scope);
}
} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes

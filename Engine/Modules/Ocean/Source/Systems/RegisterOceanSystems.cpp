#include "Ocean/Systems/RegisterOceanSystems.h"
#include "Ocean/Systems/OceanExtractionSystem.h"
#include "Ocean/Systems/OceanBuoyancySystem.h"
#include "Ocean/OceanFieldRanges.h"
#include "Ocean/OceanRenderNode.h"

#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderServices.h"
#include "ECS/Systems.h"
#include "ECS/SystemScheduling.h"

#include <memory>

namespace GameEngine::Ocean
{

void AddOceanSystemsToSchedule(ECS::SystemScheduleBuilder& builder,
                               Engine::Renderer::RenderServices* renderServices)
{
    // Extraction phase: reads OceanSurface + camera (needs TransformHierarchy +
    // Camera) and pushes per-frame wave params to the render feature.
    // It also samples OceanSplineInput splines through pointers into
    // SplineService storage, which SplineExtraction grows (CreateSpline) and
    // rewrites (RebuildCache). Systems within one wave execute in parallel, so
    // that ordering has to be a declared edge, not a by-product of which other
    // dependencies happen to push the two apart.
    builder.Add<OceanExtractionSystem>("OceanExtraction", ECS::SystemPhase::Extraction, 1,
                                       {"TransformHierarchy", "Camera", "SplineExtraction"},
                                       renderServices);
    // Buoyancy runs after the physics pipeline (PhysicsWriteback) so it never
    // touches Jolt concurrently with the step; forces apply on the next step
    // (one-frame latency, imperceptible for floating bodies).
    builder.Add<OceanBuoyancySystem>("OceanBuoyancy", ECS::SystemPhase::Extraction, 6,
                                     {"OceanExtraction", "PhysicsWriteback"}, renderServices);
}

void RegisterOceanPipelineNodes(Engine::Renderer::Pipeline::RenderPipelineNodeRegistry& registry)
{
    // Bind the ocean components' inspector slider ranges once (after the generated
    // reflection has registered the component field tables at static init).
    static const bool s_RangesRegistered = []
    {
        RegisterOceanFieldRanges();
        return true;
    }();
    (void)s_RangesRegistered;

    registry.Register(
        "OceanRender",
        []() -> std::unique_ptr<Engine::Renderer::Pipeline::IRenderPipelineNode>
        { return std::make_unique<OceanRenderNode>(); },
        /*perView*/ true,
        Engine::Renderer::Pipeline::RenderPipelineNodeResourceFields{
            .StaticPublishNames = {Engine::Renderer::Pipeline::Names::View::DepthResolvedPostOcean}});
}

} // namespace GameEngine::Ocean

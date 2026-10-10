#include "CBTTerrainECS/Systems/RegisterCBTSystems.h"

#include "CBTTerrainECS/CBTRenderNode.h"
#include "CBTTerrainECS/Systems/CBTUpdateSystem.h"

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderServices.h"

#include "ECS/SystemScheduling.h"
#include "ECS/Systems.h"

#include <memory>

namespace GameEngine::CBTTerrainECS
{

void AddCBTSystemsToSchedule(ECS::SystemScheduleBuilder& builder,
                             Engine::Renderer::RenderServices* renderServices)
{
    // Extraction phase, no ordering deps: it only reads the Terrain component and pushes the
    // active/classify flag onto the feature for the render thread to consume.
    builder.Add<CBTUpdateSystem>("CBTUpdate", ECS::SystemPhase::Extraction, 1, {}, renderServices);
}

void RegisterCBTPipelineNodes(Engine::Renderer::Pipeline::RenderPipelineNodeRegistry& registry)
{
    registry.Register(
        "CBTRender",
        []() -> std::unique_ptr<Engine::Renderer::Pipeline::IRenderPipelineNode>
        { return std::make_unique<CBTRenderNode>(); },
        /*perView*/ true, /*resourceFields*/ {}, /*feedsDepthPrepass*/ true);
}

} // namespace GameEngine::CBTTerrainECS

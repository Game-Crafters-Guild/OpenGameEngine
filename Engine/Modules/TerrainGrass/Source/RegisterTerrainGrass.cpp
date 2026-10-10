#include "TerrainGrass/RegisterTerrainGrass.h"
#include "TerrainGrass/TerrainGrassRenderNode.h"

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

namespace GameEngine::TerrainGrass
{

void RegisterTerrainGrassPipelineNodes(Engine::Renderer::Pipeline::RenderPipelineNodeRegistry& registry)
{
    registry.Register(
        "TerrainGrass",
        []() -> std::unique_ptr<Engine::Renderer::Pipeline::IRenderPipelineNode>
        {
            return std::make_unique<TerrainGrassRenderNode>();
        },
        /*perView*/ true, /*resourceFields*/ {}, /*feedsDepthPrepass*/ true);
}

} // namespace GameEngine::TerrainGrass

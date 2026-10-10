#pragma once

namespace GameEngine::Engine::Renderer::Pipeline
{
class RenderPipelineNodeRegistry;
} // namespace GameEngine::Engine::Renderer::Pipeline

namespace GameEngine::TerrainGrass
{

// Registers the "TerrainGrass" pipeline node type.
void RegisterTerrainGrassPipelineNodes(Engine::Renderer::Pipeline::RenderPipelineNodeRegistry& registry);

} // namespace GameEngine::TerrainGrass

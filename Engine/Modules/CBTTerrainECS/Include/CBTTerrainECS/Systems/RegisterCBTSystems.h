#pragma once

// Registration entry points for the CBT terrain integration (plan §8 C3), called
// from Engine.cpp alongside the terrain/ocean equivalents. RegisterCBTPipelineNodes
// registers the "CBTRender" node type (referenced by the .rendergraph blueprint);
// AddCBTSystemsToSchedule adds the CBTUpdateSystem to
// the ECS world.

namespace GameEngine::ECS
{
class SystemScheduleBuilder;
} // namespace GameEngine::ECS

namespace GameEngine::Engine::Renderer
{
class RenderServices;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Engine::Renderer::Pipeline
{
class RenderPipelineNodeRegistry;
} // namespace GameEngine::Engine::Renderer::Pipeline

namespace GameEngine::CBTTerrainECS
{

void AddCBTSystemsToSchedule(ECS::SystemScheduleBuilder& builder,
                             Engine::Renderer::RenderServices* renderServices);
void RegisterCBTPipelineNodes(Engine::Renderer::Pipeline::RenderPipelineNodeRegistry& registry);

} // namespace GameEngine::CBTTerrainECS

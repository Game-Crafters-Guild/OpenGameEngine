#pragma once

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

namespace GameEngine::TerrainECS
{

// Add terrain systems to an external schedule builder so they share ordering
// constraints with physics systems (e.g. PhysicsStep depends on TerrainPhysics).
void AddTerrainSystemsToSchedule(ECS::SystemScheduleBuilder& b,
                                 Engine::Renderer::RenderServices* renderServices);

// Registers the "TerrainUpload" pipeline node type.
void RegisterTerrainPipelineNodes(Engine::Renderer::Pipeline::RenderPipelineNodeRegistry& registry);

} // namespace GameEngine::TerrainECS

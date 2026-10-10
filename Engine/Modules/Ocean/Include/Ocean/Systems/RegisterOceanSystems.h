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

namespace GameEngine::Ocean
{

// Add the ocean ECS systems to a schedule builder (Extraction phase).
void AddOceanSystemsToSchedule(ECS::SystemScheduleBuilder& builder,
                               Engine::Renderer::RenderServices* renderServices);

// Registers the "OceanRender" pipeline node type.
void RegisterOceanPipelineNodes(Engine::Renderer::Pipeline::RenderPipelineNodeRegistry& registry);

} // namespace GameEngine::Ocean

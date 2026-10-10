#pragma once

namespace GameEngine::ECS
{
class SystemScheduleBuilder;
}

namespace GameEngine::Engine::Renderer
{
class RenderServices;
}

namespace GameEngine::EZTreeECS
{

void AddEZTreeSystemsToSchedule(ECS::SystemScheduleBuilder& builder,
                                Engine::Renderer::RenderServices* renderServices);

} // namespace GameEngine::EZTreeECS

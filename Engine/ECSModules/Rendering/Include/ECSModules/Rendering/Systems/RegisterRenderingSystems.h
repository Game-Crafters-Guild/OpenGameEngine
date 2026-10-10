#pragma once

namespace GameEngine::ECS { class SystemScheduleBuilder; }

namespace GameEngine { namespace Engine::Renderer {

class RenderServices;

// Adds rendering systems to a dependency-aware schedule builder (does not build/register).
void AddRenderingSystemsToSchedule(ECS::SystemScheduleBuilder& schedule, RenderServices* renderServices);

} } // namespace GameEngine::Engine::Renderer

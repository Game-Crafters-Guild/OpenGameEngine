#pragma once

#include "ECS/Systems.h"

namespace GameEngine::Engine::Renderer
{
class RenderServices;
}

namespace GameEngine
{
// Engine-owned bridge. User callbacks remain in the existing module registry;
// this object retains neither game code nor a world pointer.
class NativePostSimulationSystem : public ECS::ISystem
{
  public:
    explicit NativePostSimulationSystem(Engine::Renderer::RenderServices* services)
        : m_RenderServices(services) {}
    const char* GetName() const override { return "NativePostSimulation"; }
    bool RequiresExclusiveUpdate() const override { return true; }
    void Update(ECS::World& world, float32 deltaTime) override;

  private:
    Engine::Renderer::RenderServices* m_RenderServices = nullptr;
};
} // namespace GameEngine

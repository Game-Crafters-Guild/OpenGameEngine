#pragma once

#include "ECS/Systems.h"
#include "Types/Types.h"

namespace GameEngine { namespace Engine::Renderer {

class RenderServices;

class ReflectionProbeSystem : public ECS::ISystem
{
  public:
    explicit ReflectionProbeSystem(RenderServices* renderServices)
        : m_RenderServices(renderServices)
    {
    }

    const char* GetName() const override { return "ReflectionProbeSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

  private:
    RenderServices* m_RenderServices = nullptr;
    // Latch for the no-probe-selected warning: set when a frame ends with every enabled
    // probe rejecting the active camera (a lost-reflections episode), cleared when a probe
    // is accepted again — one warning per episode, not per frame or per rejected probe.
    bool m_LoggedNoProbeSelected = false;
};

} } // namespace GameEngine::Engine::Renderer

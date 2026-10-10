#pragma once

#include "ECS/Systems.h"
#include "Types/Types.h"

namespace GameEngine { namespace Engine::Renderer {

// Opens the animation wave's event dispatch: gives every Animator its event collector (a new one, or a new one in
// place of a stale id) and empties it, so what the wave's playback fires is that frame's events alone. Runs before
// AnimationGraph, the first system in the wave that fires events.
class AnimationEventCollectorSystem : public ECS::ISystem
{
public:
    const char* GetName() const override { return "AnimationEventCollector"; }
    void Update(ECS::World& world, float32 deltaTime) override;
};

} } // namespace GameEngine::Engine::Renderer

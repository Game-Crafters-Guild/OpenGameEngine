#pragma once

#include "ECS/Systems.h"

namespace GameEngine { namespace Engine::Renderer {

// Stamps Speed (XZ wish length) and Grounded onto Graph-source Animators so
// Evaluate sees them this animation tick. Animation runs before physics
// writeback, so Grounded is the previous Extraction tick's contact flag.
// No-ops unless source is Graph.
class CharacterGraphParamSystem : public ECS::ISystem
{
public:
    const char* GetName() const override { return "CharacterGraphParams"; }
    void Update(ECS::World& world, float32 deltaTime) override;
};

} } // namespace GameEngine::Engine::Renderer

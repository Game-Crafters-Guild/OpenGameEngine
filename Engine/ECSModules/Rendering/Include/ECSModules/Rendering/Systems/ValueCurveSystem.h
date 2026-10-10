#pragma once

#include "ECS/Systems.h"

namespace GameEngine
{
namespace Engine::Renderer
{

// Evaluates ValueCurve components and optionally applies the output
// to selected target properties each frame.
class ValueCurveSystem : public ECS::ISystem
{
  public:
    const char* GetName() const override { return "ValueCurveSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;
};

} // namespace Engine::Renderer
} // namespace GameEngine

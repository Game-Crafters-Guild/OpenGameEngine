#pragma once

#include "ECS/Systems.h"
#include "Types/Types.h"
#include "Engine/Rendering/AnimationSampling.h"

#include <vector>

namespace GameEngine
{
namespace Engine::Renderer
{

// Applies imported node animation to rigid mesh entities by updating local Transform.
class RigidAnimationSystem : public ECS::ISystem
{
public:
    const char* GetName() const override { return "RigidAnimationSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

private:
    PoseSampleWorkspace m_Workspace;
    std::vector<float> m_NodeWorldMatrices; // retained between calls
};

} // namespace Engine::Renderer
} // namespace GameEngine

#pragma once

#include "ECS/Systems.h"
#include "Input/InputSystem.h"

namespace GameEngine::Engine::Renderer
{

// Fly camera driven by the first connected gamepad (Steam Deck / Xbox layout).
// Runs before TransformHierarchy so local camera motion reaches WorldTransform the same frame.
class GamepadCameraControllerSystem : public ECS::ISystem
{
public:
    explicit GamepadCameraControllerSystem(Input::InputSystem* inputSystem)
        : m_InputSystem(inputSystem)
    {
    }

    const char* GetName() const override { return "GamepadCameraController"; }

    void Update(ECS::World& world, float32 deltaTime) override;

private:
    Input::InputSystem* m_InputSystem = nullptr;
    float m_SmoothedMoveX = 0.0f;
    float m_SmoothedMoveY = 0.0f;
    float m_SmoothedMoveZ = 0.0f;
    float m_YawRadians = 0.0f;
    float m_PitchRadians = 0.0f;
    bool m_OrientationInitialized = false;
};

} // namespace GameEngine::Engine::Renderer

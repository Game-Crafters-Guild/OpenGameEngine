#include "ECSModules/Rendering/Systems/GamepadCameraControllerSystem.h"

#include "Components/Rendering/Camera.h"
#include "Components/Transform.h"
#include "Components/TransformDirtyFeed.h"
#include "ECS/Query.h"
#include "Input/GamepadCodes.h"
#include "Input/InputSystem.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Engine::Renderer
{
namespace
{
using GameEngine::Components::Transform;
using GameEngine::Components::WorldTransform;
using GameEngine::Input::GamepadAxis;
using GameEngine::Input::InputSystem;
using GameEngine::Mathematics::Quaternion;
using GameEngine::Mathematics::Vector3;

constexpr float kMoveStickDeadzone = 0.22f;
constexpr float kLookStickDeadzone = 0.12f;
constexpr float kMoveSpeed = 8.0f;
constexpr float kVerticalSpeed = 6.0f;
constexpr float kYawSpeedDegrees = 260.0f;
constexpr float kPitchSpeedDegrees = 180.0f;
constexpr float kMoveInputSmoothRate = 14.0f;
constexpr float kPitchLimitDegrees = 89.0f;
constexpr float kPi = 3.1415926535f;
constexpr float kDegToRad = kPi / 180.0f;

float SmoothToward(float current, float target, float rate, float deltaTime)
{
    if (deltaTime <= 0.0f)
        return target;
    const float alpha = 1.0f - std::exp(-rate * deltaTime);
    return current + (target - current) * alpha;
}

float ApplyDeadzone(float value, float deadzone)
{
    if (std::abs(value) < deadzone)
        return 0.0f;
    const float sign = value >= 0.0f ? 1.0f : -1.0f;
    return sign * (std::abs(value) - deadzone) / (1.0f - deadzone);
}

void ExtractYawPitch(const Quaternion& rotation, float& yawRadians, float& pitchRadians)
{
    const Vector3 forward = rotation.Rotate(Vector3{0.0f, 0.0f, 1.0f});
    pitchRadians = -std::asin(std::clamp(forward.y, -1.0f, 1.0f));
    yawRadians = std::atan2(forward.x, forward.z);
}

Quaternion RotationFromYawPitch(float yawRadians, float pitchRadians)
{
    const Quaternion yaw = Quaternion::FromAxisAngle(Vector3{0.0f, 1.0f, 0.0f}, yawRadians);
    const Quaternion pitch = Quaternion::FromAxisAngle(Vector3{1.0f, 0.0f, 0.0f}, pitchRadians);
    return yaw * pitch;
}

} // namespace

void GamepadCameraControllerSystem::Update(ECS::World& world, float32 deltaTime)
{
    if (!m_InputSystem || !m_InputSystem->IsGamepadConnected(0))
    {
        m_SmoothedMoveX = 0.0f;
        m_SmoothedMoveY = 0.0f;
        m_SmoothedMoveZ = 0.0f;
        m_OrientationInitialized = false;
        return;
    }

    const float dt = std::max(deltaTime, 0.0f);

    const float rawMoveX = ApplyDeadzone(m_InputSystem->GetGamepadAxis(0, GamepadAxis::LeftX), kMoveStickDeadzone);
    const float rawMoveZ = ApplyDeadzone(-m_InputSystem->GetGamepadAxis(0, GamepadAxis::LeftY), kMoveStickDeadzone);
    const float lookX = ApplyDeadzone(m_InputSystem->GetGamepadAxis(0, GamepadAxis::RightX), kLookStickDeadzone);
    const float lookY = ApplyDeadzone(-m_InputSystem->GetGamepadAxis(0, GamepadAxis::RightY), kLookStickDeadzone);
    const float leftTrigger = m_InputSystem->GetGamepadAxis(0, GamepadAxis::LeftTrigger);
    const float rightTrigger = m_InputSystem->GetGamepadAxis(0, GamepadAxis::RightTrigger);
    const float rawMoveY = ApplyDeadzone(rightTrigger - leftTrigger, kMoveStickDeadzone);

    m_SmoothedMoveX = SmoothToward(m_SmoothedMoveX, rawMoveX, kMoveInputSmoothRate, dt);
    m_SmoothedMoveZ = SmoothToward(m_SmoothedMoveZ, rawMoveZ, kMoveInputSmoothRate, dt);
    m_SmoothedMoveY = SmoothToward(m_SmoothedMoveY, rawMoveY, kMoveInputSmoothRate, dt);

    const float moveX = m_SmoothedMoveX;
    const float moveZ = m_SmoothedMoveZ;
    const float moveY = m_SmoothedMoveY;

    constexpr float kMoveEpsilon = 0.02f;
    const bool hasInput = (std::abs(moveX) > kMoveEpsilon || std::abs(moveZ) > kMoveEpsilon ||
                           std::abs(moveY) > kMoveEpsilon || std::abs(lookX) > kMoveEpsilon ||
                           std::abs(lookY) > kMoveEpsilon);
    if (!hasInput || dt <= 0.0f)
        return;

    bool updated = false;
    world.Query<ECS::Write<Transform>, ECS::Write<WorldTransform>, ECS::Read<GameEngine::Components::Camera>>().Each(
        [&](ECS::EntityHandle entity, Transform& transform, WorldTransform& worldTransform, const GameEngine::Components::Camera&)
        {
            if (updated)
                return;

            if (!m_OrientationInitialized)
            {
                ExtractYawPitch(transform.GetRotation(), m_YawRadians, m_PitchRadians);
                m_OrientationInitialized = true;
            }

            Vector3 position = transform.GetPosition();
            const Vector3 scale = transform.GetScale();

            m_YawRadians += lookX * kYawSpeedDegrees * kDegToRad * dt;
            m_PitchRadians += lookY * kPitchSpeedDegrees * kDegToRad * dt;
            const float pitchLimit = kPitchLimitDegrees * kDegToRad;
            m_PitchRadians = std::clamp(m_PitchRadians, -pitchLimit, pitchLimit);

            const Quaternion rotation = RotationFromYawPitch(m_YawRadians, m_PitchRadians);
            const Vector3 forward = rotation.Rotate(Vector3{0.0f, 0.0f, 1.0f});
            const Vector3 right = rotation.Rotate(Vector3{1.0f, 0.0f, 0.0f});
            const Vector3 up{0.0f, 1.0f, 0.0f};

            position = position + forward * (moveZ * kMoveSpeed * dt);
            position = position + right * (moveX * kMoveSpeed * dt);
            position = position + up * (moveY * kVerticalSpeed * dt);

            transform = Transform::FromTRS(position, rotation, scale);
            for (int i = 0; i < 16; ++i)
                worldTransform.matrix[i] = transform.matrix[i];
            GameEngine::Components::BumpWorldTransform(world, entity, worldTransform);
            updated = true;
        });
}

} // namespace GameEngine::Engine::Renderer

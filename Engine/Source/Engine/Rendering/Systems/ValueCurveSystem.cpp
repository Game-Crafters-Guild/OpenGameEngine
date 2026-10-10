#include "ECSModules/Rendering/Systems/ValueCurveSystem.h"

#include "ECS/Query.h"
#include "ECS/World.h"

#include "Components/Animation/ValueCurve.h"
#include "Components/Transform.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/PostProcessVolume.h"

#include <algorithm>

namespace GameEngine
{
namespace Engine::Renderer
{

namespace
{

void ApplyCurveTargetValue(Components::ValueCurve& curve,
                           Components::Transform* transform,
                           Components::Light* light,
                           Components::PostProcessVolume* postProcess)
{
    const float32 value = curve.CurrentValue;
    switch (curve.Target)
    {
    case Components::ValueCurveTarget::None:
        return;
    case Components::ValueCurveTarget::TransformPositionX:
    case Components::ValueCurveTarget::TransformPositionY:
    case Components::ValueCurveTarget::TransformPositionZ:
    {
        if (!transform)
            return;
        Mathematics::Vector3 position = transform->GetPosition();
        if (curve.Target == Components::ValueCurveTarget::TransformPositionX) position.x = value;
        if (curve.Target == Components::ValueCurveTarget::TransformPositionY) position.y = value;
        if (curve.Target == Components::ValueCurveTarget::TransformPositionZ) position.z = value;
        *transform = Components::Transform::FromTRS(position, transform->GetRotation(), transform->GetScale());
        return;
    }
    case Components::ValueCurveTarget::TransformScaleX:
    case Components::ValueCurveTarget::TransformScaleY:
    case Components::ValueCurveTarget::TransformScaleZ:
    {
        if (!transform)
            return;
        Mathematics::Vector3 scale = transform->GetScale();
        if (curve.Target == Components::ValueCurveTarget::TransformScaleX) scale.x = value;
        if (curve.Target == Components::ValueCurveTarget::TransformScaleY) scale.y = value;
        if (curve.Target == Components::ValueCurveTarget::TransformScaleZ) scale.z = value;
        *transform = Components::Transform::FromTRS(transform->GetPosition(), transform->GetRotation(), scale);
        return;
    }
    case Components::ValueCurveTarget::LightIntensity:
    {
        if (light)
            light->Intensity = value;
        return;
    }
    case Components::ValueCurveTarget::PostProcessWeight:
    {
        if (postProcess)
            postProcess->Weight = value;
        return;
    }
    default:
        return;
    }
}

} // namespace

void ValueCurveSystem::Update(ECS::World& world, float32 deltaTime)
{
    auto q = world.Query<ECS::Write<Components::ValueCurve>,
                         ECS::Optional<Components::Transform>,
                         ECS::Optional<Components::Light>,
                         ECS::Optional<Components::PostProcessVolume>>();
    q.Each([deltaTime](ECS::EntityHandle /*entity*/,
                       Components::ValueCurve& curve,
                       Components::Transform* transform,
                       Components::Light* light,
                       Components::PostProcessVolume* postProcess)
    {
        if (curve.Enabled && curve.Playing)
        {
            curve.ElapsedSeconds += deltaTime;
            if (!curve.Loop)
            {
                constexpr float32 kMinDurationSeconds = 0.0001f;
                const float32 durationSeconds = std::max(curve.DurationSeconds, kMinDurationSeconds);
                const float32 endSeconds = curve.PingPong ? durationSeconds * 2.0f : durationSeconds;
                if (curve.ElapsedSeconds >= endSeconds)
                {
                    curve.ElapsedSeconds = endSeconds;
                    curve.Playing = false;
                }
            }
        }

        curve.CurrentValue = Components::EvaluateValueCurveValue(curve, curve.ElapsedSeconds);
        if (!curve.ApplyToTarget)
            return;

        ApplyCurveTargetValue(curve, transform, light, postProcess);
    });
}

} // namespace Engine::Renderer
} // namespace GameEngine

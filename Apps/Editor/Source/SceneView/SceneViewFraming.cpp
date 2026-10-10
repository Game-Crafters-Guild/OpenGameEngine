#include "SceneView/SceneViewFraming.h"

#include "Components/Hierarchy.h"
#include "Components/HierarchyQueries.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Mathematics/VectorOps.h"
#include "SceneViewController.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace GameEngine::Editor
{
namespace
{

void AccumulateWorldBounds(ECS::World& world, ECS::EntityHandle entity, SubtreeWorldBounds& out)
{
    if (!entity.IsValid() || !world.IsValid(entity))
        return;

    const auto* bounds = world.GetComponent<Components::LocalBounds>(entity);
    if (!bounds || bounds->Box.RadiusSquared() <= 0.0f)
        return;

    const auto* worldXf = world.GetComponent<Components::WorldTransform>(entity);
    if (!worldXf)
        return;

    const Mathematics::AABB aabb = bounds->Box.TransformToAABB(worldXf->matrix);
    if (!out.HasBounds)
    {
        out.Box = aabb;
        out.HasBounds = true;
        return;
    }

    out.Box.min.x = std::min(out.Box.min.x, aabb.min.x);
    out.Box.min.y = std::min(out.Box.min.y, aabb.min.y);
    out.Box.min.z = std::min(out.Box.min.z, aabb.min.z);
    out.Box.max.x = std::max(out.Box.max.x, aabb.max.x);
    out.Box.max.y = std::max(out.Box.max.y, aabb.max.y);
    out.Box.max.z = std::max(out.Box.max.z, aabb.max.z);
}

} // namespace

bool ComputeSubtreeWorldBounds(ECS::World& world, ECS::EntityHandle root, SubtreeWorldBounds& out)
{
    out = {};
    if (!root.IsValid() || !world.IsValid(root))
        return false;

    // Fallback target for the no-bounds case. WorldTransform is the rendered
    // truth; the local Transform position covers entities the hierarchy
    // system has not visited yet.
    if (const auto* worldXf = world.GetComponent<Components::WorldTransform>(root))
    {
        out.FallbackPosition = {worldXf->matrix[12], worldXf->matrix[13], worldXf->matrix[14]};
    }
    else if (const auto* xf = world.GetComponent<Components::Transform>(root))
    {
        out.FallbackPosition = xf->GetPosition();
    }

    AccumulateWorldBounds(world, root, out);

    std::vector<ECS::EntityHandle> descendants;
    Components::DescendantsOf(world, root, descendants);
    for (ECS::EntityHandle descendant : descendants)
        AccumulateWorldBounds(world, descendant, out);

    return true;
}

bool ComputeLookAtPose(const Mathematics::Vector3& target, const Mathematics::Vector3& direction, float distance,
                       SceneViewCameraPose& out)
{
    const float length = std::sqrt(Mathematics::Vector3::Dot(direction, direction));
    if (length < 1.0e-6f)
        return false;
    const Mathematics::Vector3 look(direction.x / length, direction.y / length, direction.z / length);
    constexpr float kDegreesPerRadian = 180.0f / Mathematics::Pi;
    out.Pos[0] = target.x - look.x * distance;
    out.Pos[1] = target.y - look.y * distance;
    out.Pos[2] = target.z - look.z * distance;
    out.YawDeg = std::atan2(look.z, look.x) * kDegreesPerRadian;
    out.PitchDeg = std::asin(look.y) * kDegreesPerRadian;
    out.Distance = distance;
    return true;
}

} // namespace GameEngine::Editor

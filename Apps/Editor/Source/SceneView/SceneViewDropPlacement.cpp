#include "SceneView/SceneViewDropPlacement.h"

#include "Components/Hierarchy.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/ECS.h"
#include "ECS/Components.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Ray.h"
#include "Physics/PhysicsQuery.h"
#include "PhysicsECS/PhysicsWorldService.h"
#include "Picking/MeshPickingService.h"

#include <cmath>

namespace GameEngine::Editor
{

namespace
{

constexpr float kFallbackAlongRayDistance = 10.0f;

// The result becomes a pick ray, so a non-finite direction has to take the
// +Z fallback rather than pass through: NaN compares false against everything
// and would slip a bare `len2 < eps` test, and an infinite len2 slips it too,
// yielding d * (1/inf).
Mathematics::Vector3 NormalizeDir(const Mathematics::Vector3& d)
{
    const float len2 = d.x * d.x + d.y * d.y + d.z * d.z;
    if (!std::isfinite(len2) || len2 < 1e-12f)
        return Mathematics::Vector3(0.0f, 0.0f, 1.0f);
    const float inv = 1.0f / std::sqrt(len2);
    return Mathematics::Vector3(d.x * inv, d.y * inv, d.z * inv);
}

bool HasDisabledInAncestryOrSelf(ECS::World& world, ECS::EntityHandle entity)
{
    using GameEngine::Components::Parent;
    if (!entity.IsValid())
        return false;
    if (world.GetComponent<ECS::Disabled>(entity) != nullptr)
        return true;
    int safety = 64;
    const Parent* p = world.GetComponent<Parent>(entity);
    while (p && p->parent.IsValid() && safety-- > 0)
    {
        if (world.GetComponent<ECS::Disabled>(p->parent) != nullptr)
            return true;
        p = world.GetComponent<Parent>(p->parent);
    }
    return false;
}

} // namespace

SceneViewDropPoint ResolveSceneViewDropPoint(const Mathematics::Ray3D& cameraRay,
                                             ECS::World& world,
                                             std::span<const ECS::EntityHandle> ignoreEntities)
{
    using namespace GameEngine::Mathematics;

    Ray3D ray{};
    ray.origin = cameraRay.origin;
    ray.direction = NormalizeDir(cameraRay.direction);
    SceneViewDropPoint point;

    // Physics first: takes priority over the visible mesh because a collision
    // shape often models the "intended drop surface" (e.g. a flat platform
    // with a decorative mesh on top). Drop wherever the physics ray lands.
    if (auto* phys = PhysicsECS::PhysicsWorldService::TryGet())
    {
        Physics::RayCastQuery q{};
        q.ray.origin = ray.origin;
        q.ray.direction = ray.direction;
        q.maxDistance = 1.0e6f;
        Physics::RayCastResult hit{};
        if (phys->RayCast(q, hit) && hit.HasHit())
        {
            point.Position = Vector3(hit.hitPoint.x, hit.hitPoint.y, hit.hitPoint.z);
            return point;
        }
    }

    // Per-triangle mesh fallback. Drop wherever the cursor hits — no
    // orientation gating. Matches Godot/Unreal behavior: the user is
    // intentionally aiming at the surface they want to drop on. Future
    // follow-up: optional modifier to align the placed object's up-axis
    // to the hit normal (Unreal's "Surface Snap" feature).
    Picking::PickOptions pickOpts;
    pickOpts.IncludeTerrain  = true;
    pickOpts.IgnoreEntities  = ignoreEntities;
    const auto pick = Picking::RaycastScene(ray, world, pickOpts);
    if (pick.Hit)
    {
        point.Position = pick.Best.WorldPosition;
        point.OnPendingMesh = pick.Best.MeshBvhPending;
        return point;
    }

    float tPlane = 0.0f;
    Vector3 planeHit{};
    const Vector3 planeN(0.0f, 1.0f, 0.0f);
    const Vector3 planeO(0.0f, 0.0f, 0.0f);
    if (IntersectRayPlane(ray, planeO, planeN, tPlane, planeHit) && tPlane >= 0.0f)
    {
        point.Position = planeHit;
        return point;
    }

    point.Position = RayPointAt(ray, kFallbackAlongRayDistance);
    return point;
}

SceneViewDropMesh PickSceneViewDropMesh(const Mathematics::Ray3D& cameraRay,
                                        ECS::World& world,
                                        std::span<const ECS::EntityHandle> ignoreEntities)
{
    Mathematics::Ray3D ray{};
    ray.origin = cameraRay.origin;
    ray.direction = NormalizeDir(cameraRay.direction);

    Picking::PickOptions pickOpts;
    pickOpts.IncludeTerrain = false;
    pickOpts.IgnoreEntities = ignoreEntities;
    const auto pick = Picking::RaycastScene(ray, world, pickOpts);
    SceneViewDropMesh mesh;
    if (!pick.Hit || HasDisabledInAncestryOrSelf(world, pick.Best.Entity))
        return mesh;
    mesh.Entity = pick.Best.Entity;
    mesh.Pending = pick.Best.MeshBvhPending;
    return mesh;
}

} // namespace GameEngine::Editor

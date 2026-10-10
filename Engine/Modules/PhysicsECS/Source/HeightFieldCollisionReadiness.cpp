#include "PhysicsECS/HeightFieldCollisionReadiness.h"

#include "PhysicsECS/Components/HeightFieldColliderShape.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "PhysicsECS/Components/PhysicsColliderOwner.h"
#include "PhysicsECS/HeightFieldDataProvider.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "Physics/PhysicsWorld.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace GameEngine::PhysicsECS
{
namespace
{
std::vector<PendingHeightFieldSource>& PendingSources()
{
    static std::vector<PendingHeightFieldSource> sources;
    return sources;
}

bool Finite(float32 value)
{
    return std::isfinite(value);
}
} // namespace

void AddPendingHeightFieldSource(PendingHeightFieldSource source)
{
    auto& sources = PendingSources();
    if (source && std::find(sources.begin(), sources.end(), source) == sources.end())
        sources.push_back(source);
}

void RemovePendingHeightFieldSource(PendingHeightFieldSource source)
{
    auto& sources = PendingSources();
    sources.erase(std::remove(sources.begin(), sources.end(), source), sources.end());
}

bool HeightFieldFootprint(const float32 worldMatrix[16], float32 sizeX, float32 sizeZ,
                          float32 heightScale, Physics::AABB& footprint)
{
    if (!std::all_of(worldMatrix, worldMatrix + 16, Finite) || !Finite(sizeX) || !Finite(sizeZ) ||
        !Finite(heightScale))
        return false;
    const float32 halfHeight = std::abs(heightScale) * 0.5f;
    const Mathematics::BoundingBox local{{0.0f, halfHeight, 0.0f},
                                         {std::abs(sizeX) * 0.5f, halfHeight, std::abs(sizeZ) * 0.5f}};
    footprint = local.TransformToAABB(worldMatrix);
    return true;
}

bool BoxesOverlap(const Physics::AABB& a, const Physics::AABB& b)
{
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y &&
           a.min.z <= b.max.z && a.max.z >= b.min.z;
}

bool IsHeightFieldCollisionCurrent(ECS::World& world, const Physics::PhysicsWorld& physics,
                                   const Physics::AABB& box, HeightFieldCollisionWait& wait)
{
    const auto provider = GetHeightFieldDataProvider();
    bool current = true;
    world.Query<ECS::Read<Components::HeightFieldColliderShape>, ECS::Read<Components::PhysicsCollider>,
                ECS::Optional<Components::WorldTransform>, ECS::Optional<Components::PhysicsColliderOwner>>()
        .Each([&](ECS::EntityHandle entity, const Components::HeightFieldColliderShape& shape,
                  const Components::PhysicsCollider&, const Components::WorldTransform* transform,
                  const Components::PhysicsColliderOwner* owner)
        {
            if (!current || !provider)
                return;
            Physics::AABB footprint;
            if (transform &&
                HeightFieldFootprint(transform->matrix, shape.sizeX, shape.sizeZ, shape.heightScale, footprint) &&
                !BoxesOverlap(footprint, box))
                return;

            // A switched-off body has no backend body to wait for.
            const ECS::EntityHandle bodyEntity = (owner && owner->body.IsValid()) ? owner->body : entity;
            const ECS::Entity bodyOwner(&world, bodyEntity);
            if (!bodyOwner.IsEnabledInHierarchy() || !bodyOwner.IsEnabled<Components::PhysicsBody>())
                return;
            const Components::PhysicsBody* body = world.GetComponent<Components::PhysicsBody>(bodyEntity);
            if (!body || !body->initialized || !physics.IsBodyValid(body->body) ||
                !physics.IsShapeValid(body->shape))
            {
                current = false;
                wait = {entity, "heightfield collider has no built body yet"};
                return;
            }
            const auto data = provider(shape.dataHandle, shape.dataGeneration, shape.lastBuiltVersion);
            if (!data.samples || data.sampleCount < 4)
            {
                current = false;
                wait = {entity, "heightfield collider's height data is not available"};
                return;
            }
            if (data.version != shape.lastBuiltVersion)
            {
                current = false;
                wait = {entity, "heightfield collider is rebuilding from newer height data"};
            }
        });
    if (!current)
        return false;

    for (const auto source : PendingSources())
        if (source(world, box, wait))
            return false;
    return true;
}

} // namespace GameEngine::PhysicsECS

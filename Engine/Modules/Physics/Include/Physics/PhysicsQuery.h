#pragma once

#include "Physics/PhysicsTypes.h"

#include <vector>

namespace GameEngine::Physics
{
struct CollisionFilter
{
    uint32 layerMask = 0xFFFFFFFFu;
    bool ignoreSensors = false;

    bool PassesLayer(CollisionLayer layer) const
    {
        const uint32 l = static_cast<uint32>(layer);
        if (l >= 32)
        {
            // Out of supported mask range.
            return false;
        }
        return (layerMask & (1u << l)) != 0u;
    }
};

struct RayCastQuery
{
    Ray3D ray;
    float32 maxDistance = 1000.0f;
    CollisionFilter filter{};
};

struct RayCastResult
{
    BodyHandle body{};
    Vector3 hitPoint{0.0f, 0.0f, 0.0f};
    Vector3 hitNormal{0.0f, 1.0f, 0.0f};
    float32 distance = 0.0f;
    uint64 userData = 0;

    bool HasHit() const { return body.IsValid(); }
};

struct BoxOverlapQuery
{
    Vector3 center{0.0f, 0.0f, 0.0f};
    Vector3 halfExtents{0.5f, 0.5f, 0.5f};
    Quaternion rotation = Quaternion::Identity();
    CollisionFilter filter{};
};

struct OverlapResult
{
    BodyHandle body{};
    uint64 userData = 0;

    bool HasHit() const { return body.IsValid(); }
};

} // namespace GameEngine::Physics


#pragma once

#include "Physics/PhysicsTypes.h"

#include <algorithm>
#include <variant>
#include <vector>

namespace GameEngine::Physics
{
struct SphereShapeDef
{
    float32 radius = 0.5f;

    SphereShapeDef Scaled(const Vector3& scale) const
    {
        const float32 s = std::max({scale.x, scale.y, scale.z});
        return SphereShapeDef{radius * s};
    }
};

struct BoxShapeDef
{
    Vector3 halfExtents{0.5f, 0.5f, 0.5f};

    BoxShapeDef Scaled(const Vector3& scale) const
    {
        return BoxShapeDef{Vector3(halfExtents.x * scale.x, halfExtents.y * scale.y, halfExtents.z * scale.z)};
    }
};

enum class CapsuleAxis : uint8
{
    X,
    Y,
    Z,
};

struct CapsuleShapeDef
{
    float32 radius = 0.5f;
    float32 halfHeight = 0.5f;
    CapsuleAxis axis = CapsuleAxis::Y;

    CapsuleShapeDef Scaled(const Vector3& scale) const
    {
        float32 heightScale = 1.0f;
        float32 radiusScale = 1.0f;
        switch (axis)
        {
        case CapsuleAxis::X:
            heightScale = scale.x;
            radiusScale = (scale.y + scale.z) * 0.5f;
            break;
        case CapsuleAxis::Y:
            heightScale = scale.y;
            radiusScale = (scale.x + scale.z) * 0.5f;
            break;
        case CapsuleAxis::Z:
            heightScale = scale.z;
            radiusScale = (scale.x + scale.y) * 0.5f;
            break;
        }
        return CapsuleShapeDef{radius * radiusScale, halfHeight * heightScale, axis};
    }
};

// Infinite plane (negative half-space is solid), bounded by halfExtent for broadphase.
// Uses the convention normal . x + d = 0.
struct PlaneShapeDef
{
    Vector3 normal{0.0f, 1.0f, 0.0f};
    float32 d = 0.0f;
    float32 halfExtent = 1000.0f;
};

struct ConvexHullShapeDef
{
    std::vector<Vector3> points;
};

struct MeshShapeDef
{
    std::vector<Vector3> vertices;
    std::vector<uint32> indices;
};

struct CompoundShapeChild
{
    ShapeHandle shape{};
    Vector3 position{0.0f, 0.0f, 0.0f};
    Quaternion rotation = Quaternion::Identity();
    Vector3 bakedScale{1.0f, 1.0f, 1.0f};
};

struct CompoundShapeDef
{
    std::vector<CompoundShapeChild> children;
};

struct HeightFieldShapeDef
{
    std::vector<float32> Samples;    // row-major height values (NxN)
    uint32 SampleCount = 0;         // N for NxN grid
    Vector3 Offset{0.0f, 0.0f, 0.0f};
    Vector3 Scale{1.0f, 1.0f, 1.0f};
};

using ShapeDefinition = std::variant<
    SphereShapeDef,
    BoxShapeDef,
    CapsuleShapeDef,
    PlaneShapeDef,
    ConvexHullShapeDef,
    MeshShapeDef,
    CompoundShapeDef,
    HeightFieldShapeDef>;

} // namespace GameEngine::Physics


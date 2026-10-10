#pragma once

#include "Types/Types.h"

#include "Mathematics/Geometry.h"
#include "Mathematics/Ray.h"
#include "Mathematics/Types.h"

#include "GenerationalVector/GenerationalHandle.h"

#include "Physics/PhysicsMaterial.h"

namespace GameEngine::Physics
{
// Reuse canonical math types (no duplication)
using Mathematics::AABB;
using Mathematics::Quaternion;
using Mathematics::Ray3D;
using Mathematics::Vector3;

// Reuse canonical scalar/primitive aliases
using GameEngine::float32;
using GameEngine::int32;
using GameEngine::uint16;
using GameEngine::uint32;
using GameEngine::uint64;

// Handles: use existing generational handle implementation (stable across hot-reload/ABI).
// NOTE: GenerationalVector::Handle uses 0 as invalid.
using BodyHandle = GenerationalVector::Handle;
using ShapeHandle = GenerationalVector::Handle;
using ConstraintHandle = GenerationalVector::Handle;
using MaterialHandle = GenerationalVector::Handle;
using CharacterHandle = GenerationalVector::Handle;

// Physics transform (no scale; scale is baked into shapes)
struct Transform
{
    Vector3 position{0.0f, 0.0f, 0.0f};
    Quaternion rotation = Quaternion::Identity();
};

enum class MotionType : uint8
{
    Static,
    Kinematic,
    Dynamic,
};

// Motor for CharacterController. Names are engine-level, not backend-specific:
// Kinematic = depenetrating mover; Dynamic = rigid-body character.
enum class CharacterMotor : uint8
{
    Kinematic = 0,
    Dynamic = 1,
};

enum class ActivationMode : uint8
{
    Activate,
    DontActivate,
};

// Collision layers are kept small (0..31 recommended) so masks can stay 32-bit.
using CollisionLayer = uint16;

static constexpr uint32 kMaxCollisionLayers = 32u;

namespace Layers
{
constexpr CollisionLayer Static = 0;
constexpr CollisionLayer Dynamic = 1;
constexpr CollisionLayer Kinematic = 2;
constexpr CollisionLayer Trigger = 3;
constexpr CollisionLayer UserStart = 8;
} // namespace Layers

struct CharacterSettings
{
    CharacterMotor motor = CharacterMotor::Kinematic;

    Vector3 position{0.0f, 0.0f, 0.0f};
    Quaternion rotation = Quaternion::Identity();

    // Capsule: total height including caps. Must be greater than 2 * radius.
    float32 radius = 0.4f;
    float32 height = 1.8f;

    float32 maxSlopeAngleDegrees = 45.0f;
    float32 maxStepHeight = 0.4f;
    float32 skinWidth = 0.02f;

    float32 mass = 80.0f;
    float32 gravityScale = 1.0f;

    CollisionLayer layer = Layers::Kinematic;
    uint64 userData = 0;
};

// Global collision filtering: per-layer bitmask matrix.
// mask[layerA] bit(layerB) == 1 => A collides with B.
// Defaults to "collide with everything" for all 0..31 layers.
struct LayerCollisionMatrix
{
    uint32 mask[kMaxCollisionLayers]{};

    LayerCollisionMatrix()
    {
        for (uint32 i = 0; i < kMaxCollisionLayers; ++i)
            mask[i] = 0xFFFFFFFFu;
    }

    void SetAll(bool enabled)
    {
        const uint32 v = enabled ? 0xFFFFFFFFu : 0u;
        for (uint32 i = 0; i < kMaxCollisionLayers; ++i)
            mask[i] = v;
    }

    void SetPair(CollisionLayer a, CollisionLayer b, bool enabled)
    {
        const uint32 ia = static_cast<uint32>(a);
        const uint32 ib = static_cast<uint32>(b);
        if (ia >= kMaxCollisionLayers || ib >= kMaxCollisionLayers)
            return;

        const uint32 bitB = (1u << ib);
        const uint32 bitA = (1u << ia);
        if (enabled)
        {
            mask[ia] |= bitB;
            mask[ib] |= bitA;
        }
        else
        {
            mask[ia] &= ~bitB;
            mask[ib] &= ~bitA;
        }
    }

    bool CanCollide(CollisionLayer a, CollisionLayer b) const
    {
        const uint32 ia = static_cast<uint32>(a);
        const uint32 ib = static_cast<uint32>(b);
        if (ia >= kMaxCollisionLayers || ib >= kMaxCollisionLayers)
            return false;
        return (mask[ia] & (1u << ib)) != 0u;
    }
};

struct CollisionGroup
{
    uint16 groupId = 0;
    uint16 subGroupId = 0;
};

struct BodySettings
{
    // Pose
    Vector3 position{0.0f, 0.0f, 0.0f};
    Quaternion rotation = Quaternion::Identity();

    // Shape
    ShapeHandle shape{};

    // Motion
    MotionType motionType = MotionType::Dynamic;
    CollisionLayer layer = Layers::Dynamic;
    CollisionGroup collisionGroup{};

    // Physical params
    float32 mass = 1.0f;
    float32 linearDamping = 0.05f;
    float32 angularDamping = 0.05f;
    float32 gravityScale = 1.0f;
    Vector3 centerOfMassOffset{0.0f, 0.0f, 0.0f};

    // Contact material (applied to the body in the backend)
    // Friction: 0 = no friction, higher = more friction (typical ~0.2..1.0)
    // Restitution: 0 = no bounce, 1 = perfectly elastic
    float32 friction = 0.5f;
    float32 restitution = 0.1f;

    // Initial velocities
    Vector3 linearVelocity{0.0f, 0.0f, 0.0f};
    Vector3 angularVelocity{0.0f, 0.0f, 0.0f};

    // Flags
    bool isSensor = false;
    bool allowSleep = true;
    bool startAwake = true;
    bool continuousCollision = false;

    // User data (typically packed EntityId)
    uint64 userData = 0;
};

struct PhysicsWorldSettings
{
    Vector3 gravity{0.0f, -9.81f, 0.0f};

    // Global layer filtering (used by the backend's object-vs-object collision filter).
    LayerCollisionMatrix layerCollisionMatrix{};

    // Simulation stepping policy (engine-side).
    //
    // If fixedTimeStep > 0: the engine should accumulate frame time and step physics in
    // discrete substeps of fixedTimeStep (up to maxSubSteps per frame).
    //
    // If fixedTimeStep <= 0: step once per frame using the frame delta time.
    float32 fixedTimeStep = 1.0f / 60.0f;
    int32 maxSubSteps = 4;

    // Backend collision iterations per step (passed through to Jolt's PhysicsSystem::Update).
    // Jolt recommends 1 for most games; increase for fast-moving objects that tunnel.
    int32 collisionSteps = 1;

    uint32 maxBodies = 65536;
    uint32 maxBodyPairs = 65536;
    uint32 maxContactConstraints = 65536;

    // Jolt-specific: per-step temporary allocator budget.
    // If too small, Jolt will trigger a debugbreak/assert when stepping.
    uint32 tempAllocatorBytes = 64u * 1024u * 1024u; // 64 MiB

    int32 numThreads = -1; // -1 => auto
};

} // namespace GameEngine::Physics

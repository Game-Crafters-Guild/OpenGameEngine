#pragma once

#include "Mathematics/Vector3.h"
#include "Types/Types.h"

namespace GameEngine::Particles
{
struct ParticleProcessorDescriptor;

enum class ParticleCollisionSource : uint8
{
    /// An infinite plane in world space.
    Plane,
    /// A swept query against the physics world.
    Physics
};

/// Bounces particles off a plane or the physics world and raises Collision event rules.
struct ParticleCollisionParameters
{
    ParticleCollisionSource Source = ParticleCollisionSource::Plane;
    Mathematics::Vector3 PlaneNormal{0.0f, 1.0f, 0.0f};
    /// Signed distance of the plane from the world origin along its normal.
    float PlaneOffset = 0.0f;
    /// Contact radius as a multiple of particle size.
    float Radius = 1.0f;
    /// Fraction of the normal speed kept after contact.
    float Bounce = 0.0f;
    /// Fraction of the whole velocity lost at contact; with no bounce, 1 settles the particle.
    float Damping = 0.0f;
    bool KillOnContact = false;
    /// Physics layers the sweep hits.
    uint32 LayerMask = 0xFFFFFFFFu;
};

/// The registered "collision" processor.
const ParticleProcessorDescriptor& ParticleCollisionProcessor();
} // namespace GameEngine::Particles

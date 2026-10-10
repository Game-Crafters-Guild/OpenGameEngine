#pragma once

#include "Mathematics/Vector3.h"
#include "Particles/ParticleValue.h"

namespace GameEngine::Particles
{
struct ParticleProcessorDescriptor;

/// Accelerates particles away from the emitter's axis and around it: swirls, vortices, orbits.
struct ParticleOrbitParameters
{
    /// Axis through the emitter the particles turn around, in the emitter's space.
    Mathematics::Vector3 Axis{0.0f, 1.0f, 0.0f};
    ParticleValueInput Input{};
    /// Units per second squared away from the axis; negative pulls towards it.
    ParticleValue Radial{0.0f};
    /// Units per second squared around the axis, in the direction of Axis cross the outward direction.
    ParticleValue Tangential{1.0f};
};

/// The registered "orbit" processor.
const ParticleProcessorDescriptor& ParticleOrbitProcessor();
} // namespace GameEngine::Particles

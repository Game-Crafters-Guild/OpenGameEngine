#pragma once

#include "Mathematics/Vector3.h"
#include "Particles/ParticleValue.h"
#include "Particles/Processors/ParticleProcessorTypes.h"

namespace GameEngine::Particles
{
struct ParticleProcessorDescriptor;

/// Launches newborn particles in a random direction within a cone around Direction, uniformly over
/// the cone's solid angle, at Speed, plus a share of the emitter's own velocity.
struct ParticleVelocityConeParameters
{
    /// Local turns the cone with the emitter; World keeps it fixed however the emitter turns.
    ParticleSpace Space = ParticleSpace::Local;
    Mathematics::Vector3 Direction{0.0f, 1.0f, 0.0f};
    /// Half angle of the cone in degrees: 0 launches along Direction, 180 in every direction.
    float Spread = 25.0f;
    ParticleValueInput Input{};
    /// Units per second.
    ParticleValue Speed{1.0f, 2.0f};
    /// Fraction of the emitter's velocity each particle starts with.
    float InheritEmitterVelocity = 0.0f;
};

/// The registered "velocityCone" processor.
const ParticleProcessorDescriptor& ParticleVelocityConeProcessor();
} // namespace GameEngine::Particles

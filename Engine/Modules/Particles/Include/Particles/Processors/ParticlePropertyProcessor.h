#pragma once

#include "Particles/ParticleChannels.h"
#include "Particles/ParticleValue.h"
#include "Particles/Processors/ParticleProcessorTypes.h"

namespace GameEngine::Particles
{
struct ParticleProcessorDescriptor;

/// Sets, adds to or multiplies one particle attribute by a value. The basis is the attribute's
/// current value, its value at birth or its value when the particle entered the phase, so a
/// multiply over lifetime scales the birth size instead of compounding every tick.
struct ParticlePropertyParameters
{
    ParticleAttribute Target = ParticleAttribute::Size;
    ParticleOperation Operation = ParticleOperation::Multiply;
    ParticleBasis Basis = ParticleBasis::Birth;
    /// Space of Position and Velocity values; other attributes ignore it.
    ParticleSpace Space = ParticleSpace::Simulation;
    ParticleValueInput Input{};
    /// One value per component of the target: X, Y, Z for vectors, R, G, B, A for color.
    ParticleValue Value[4]{1.0f, 1.0f, 1.0f, 1.0f};
};

/// The registered "property" processor.
const ParticleProcessorDescriptor& ParticlePropertyProcessor();
} // namespace GameEngine::Particles

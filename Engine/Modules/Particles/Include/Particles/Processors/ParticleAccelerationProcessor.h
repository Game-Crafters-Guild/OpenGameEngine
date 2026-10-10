#pragma once

#include "Particles/ParticleValue.h"
#include "Particles/Processors/ParticleProcessorTypes.h"

namespace GameEngine::Particles
{
struct ParticleProcessorDescriptor;

/// Adds an acceleration to particle velocity every tick: gravity, wind, thrust.
struct ParticleAccelerationParameters
{
    ParticleSpace Space = ParticleSpace::World;
    ParticleValueInput Input{};
    ParticleValue Acceleration[3]{0.0f, -9.81f, 0.0f};
};

/// The registered "acceleration" processor.
const ParticleProcessorDescriptor& ParticleAccelerationProcessor();
} // namespace GameEngine::Particles

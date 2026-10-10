#pragma once

#include "Particles/ParticleValue.h"

namespace GameEngine::Particles
{
struct ParticleProcessorDescriptor;

/// What an emission rate curve is evaluated at.
enum class ParticleEmissionDriver : uint8
{
    /// Seconds since the emitter started.
    EmitterAge,
    /// Emitter speed in world units per second.
    EmitterSpeed
};

/// Spawns particles continuously.
struct ParticleEmitRateParameters
{
    ParticleEmissionDriver Driver = ParticleEmissionDriver::EmitterAge;
    /// Driver values mapped to the curve domain's 0 and 1.
    float InputMinimum = 0.0f;
    float InputMaximum = 1.0f;
    /// Particles per second.
    ParticleValue Rate{20.0f};
};

/// The registered "emitRate" processor.
const ParticleProcessorDescriptor& ParticleEmitRateProcessor();
} // namespace GameEngine::Particles

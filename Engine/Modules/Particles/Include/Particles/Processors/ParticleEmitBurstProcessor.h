#pragma once

#include "Types/Types.h"

namespace GameEngine::Particles
{
struct ParticleProcessorDescriptor;

/// Spawns a number of particles at once, optionally repeating.
struct ParticleEmitBurstParameters
{
    /// Emitter time of the first burst, in seconds.
    float Time = 0.0f;
    uint32 Count = 10;
    /// Seconds between repeats; 0 bursts once.
    float RepeatInterval = 0.0f;
};

/// The registered "emitBurst" processor.
const ParticleProcessorDescriptor& ParticleEmitBurstProcessor();
} // namespace GameEngine::Particles

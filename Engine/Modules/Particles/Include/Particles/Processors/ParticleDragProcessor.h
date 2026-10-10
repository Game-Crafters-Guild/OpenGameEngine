#pragma once

#include "Particles/ParticleValue.h"

namespace GameEngine::Particles
{
struct ParticleProcessorDescriptor;

/// How drag takes speed away.
enum class ParticleDragMode : uint8
{
    /// Velocity loses Drag of itself per second: fast particles slow quickly, then ever less.
    Exponential,
    /// Speed falls by Drag units per second until the particle stops: friction.
    Linear
};

/// Slows particles down.
struct ParticleDragParameters
{
    ParticleDragMode Mode = ParticleDragMode::Exponential;
    ParticleValueInput Input{};
    ParticleValue Drag{1.0f};
};

/// The registered "drag" processor.
const ParticleProcessorDescriptor& ParticleDragProcessor();
} // namespace GameEngine::Particles

#pragma once

#include "Particles/ParticleValue.h"

namespace GameEngine::Particles
{
struct ParticleProcessorDescriptor;

/// Caps particle speed, keeping direction.
struct ParticleLimitSpeedParameters
{
    ParticleValueInput Input{};
    ParticleValue Speed{5.0f};
};

/// The registered "limitSpeed" processor.
const ParticleProcessorDescriptor& ParticleLimitSpeedProcessor();
} // namespace GameEngine::Particles

#pragma once

#include "Particles/ParticleValue.h"

namespace GameEngine::Particles
{
struct ParticleProcessorDescriptor;

/// Makes each particle a point light of its color while the processor is active.
struct ParticleLightParameters
{
    ParticleValueInput Input{};
    ParticleValue Intensity{1.0f};
    ParticleValue Range{1.0f};
};

/// The registered "light" processor.
const ParticleProcessorDescriptor& ParticleLightProcessor();
} // namespace GameEngine::Particles

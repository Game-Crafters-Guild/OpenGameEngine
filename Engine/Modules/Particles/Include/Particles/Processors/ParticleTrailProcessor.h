#pragma once

#include "Types/Types.h"

namespace GameEngine::Particles
{
struct ParticleProcessorDescriptor;

inline constexpr uint32 kMaxTrailPoints = 256;

/// Records each particle's recent path for ribbon rendering.
struct ParticleTrailParameters
{
    /// Seconds a recorded point stays in the trail.
    float Lifetime = 0.3f;
    /// Most points one trail holds.
    uint32 Points = 16;
    /// Distance the particle travels before the moving head becomes a kept point.
    float Spacing = 0.0f;
    /// Remove the trail with its particle instead of letting it fade out.
    bool DieWithParticle = false;
};

/// The registered "trail" processor.
const ParticleProcessorDescriptor& ParticleTrailProcessor();
} // namespace GameEngine::Particles

#pragma once

#include "Types/Types.h"

namespace GameEngine::Particles
{

/// The space a processor's vectors are authored in. Simulation is the space particles are stored
/// in: world space, or emitter-local space when the emitter simulates locally.
enum class ParticleSpace : uint8
{
    Simulation,
    World,
    Local
};

/// How a Property processor combines its value with the basis.
enum class ParticleOperation : uint8
{
    Set,
    Add,
    Multiply
};

} // namespace GameEngine::Particles

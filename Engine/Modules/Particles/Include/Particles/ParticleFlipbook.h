#pragma once

#include "Types/Types.h"

namespace GameEngine::Components
{
struct ParticleRenderer;
}

namespace GameEngine::Particles
{

/// The texture sheet cell (fractional, for blending) a particle of age `age` shows. With a frame
/// rate the sheet plays at that rate, looping when set; without one it plays once over the
/// particle's lifetime. `speed` and `offset` are the particle's animation speed and offset.
float FlipbookFrame(const Components::ParticleRenderer& renderer, float age, float lifetime, float speed = 1.0f,
                    float offset = 0.0f);

} // namespace GameEngine::Particles

#include "Components/Rendering/Particles.h"

#include "Components/Rendering/ParticleRenderer.h"
#include "Particles/ParticleFlipbook.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Components
{

ParticleEmitter3D MakeParticleEmitter2DDefaults()
{
    ParticleEmitter3D emitter{};
    emitter.Dimension = ParticleEmitterDimension::World2D;
    return emitter;
}

} // namespace GameEngine::Components

namespace GameEngine::Particles
{
using namespace Components;

float FlipbookFrame(const ParticleRenderer& renderer, float age, float lifetime, float speed, float offset)
{
    const uint32 cells = std::clamp(renderer.Columns, 1u, 256u) * std::clamp(renderer.Rows, 1u, 256u);
    const uint32 frames = renderer.FrameCount ? std::min(cells, renderer.FrameCount) : cells;
    // Once over the lifetime occupies [0, frames) so the last cell is shown before death; scaling by
    // frames - 1 would reach that cell only at death.
    const float phase = renderer.FrameRate > 0.0f
                            ? std::max(0.0f, age) * renderer.FrameRate
                            : std::clamp(age / std::max(lifetime, 0.001f), 0.0f, 1.0f) * static_cast<float>(frames);
    float frame = renderer.StartFrame + offset * frames + phase * speed;
    if (!std::isfinite(frame))
        return 0.0f;
    if (renderer.Loop && renderer.FrameRate > 0.0f)
        frame -= std::floor(frame / frames) * frames;
    else
        frame = std::clamp(frame, 0.0f, static_cast<float>(frames - 1));
    return frame;
}

} // namespace GameEngine::Particles

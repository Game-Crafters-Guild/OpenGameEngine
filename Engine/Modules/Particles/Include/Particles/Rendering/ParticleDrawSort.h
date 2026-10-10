#pragma once

#include "Components/Rendering/ParticleRenderer.h"
#include "Types/Types.h"

namespace GameEngine::Particles
{

/// Where one particle draws among a view's blended particles.
struct ParticleSortKey
{
    /// View-space depth used for the sort: the particle's own for ViewDepth, its emitter origin's
    /// for the other orders.
    float Depth = 0.0f;
    uint64 Emitter = 0;
    Components::ParticleDrawOrder Order = Components::ParticleDrawOrder::ViewDepth;
    float Age = 0.0f;
    uint32 SpawnIndex = 0;
};

/// Whether `a` draws before `b`: the farther first, so blending composites back to front; at one
/// depth, emitters in id order and one emitter's particles by its draw order.
inline bool DrawsBefore(const ParticleSortKey& a, const ParticleSortKey& b)
{
    if (a.Depth != b.Depth)
        return a.Depth > b.Depth;
    if (a.Emitter != b.Emitter)
        return a.Emitter < b.Emitter;
    if (a.Order == Components::ParticleDrawOrder::Lifetime && a.Age != b.Age)
        return a.Age < b.Age;
    if (a.Order == Components::ParticleDrawOrder::ReverseLifetime && a.Age != b.Age)
        return a.Age > b.Age;
    return a.SpawnIndex < b.SpawnIndex;
}

} // namespace GameEngine::Particles

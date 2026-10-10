#pragma once

#include "Types/Types.h"
#include <memory>

namespace GameEngine::ECS { class World; }
namespace GameEngine::Particles { struct ParticleWorldState; }
namespace GameEngine::Engine::Renderer
{
class RenderServices;
struct ForwardEmitContext;
}

namespace GameEngine::Particles
{
/// What the last extraction drew and what the simulation it drew from cost.
struct ParticleExtractionStats
{
    /// Emitters with particles or trails in the frame.
    uint32 EmitterCount = 0;
    /// Particles alive across every emitter after the simulation's tick.
    uint32 LiveParticles = 0;
    /// Wall time of the particle simulation system's last update, in milliseconds.
    float32 SimulationMs = 0.0f;
};

/// Extracts compact particle snapshots and sorts draw runs separately for each view. The
/// scheduled simulation owns particle state independently of cameras.
class ParticleExtraction
{
  public:
    ParticleExtraction();
    ~ParticleExtraction();
    ParticleExtraction(const ParticleExtraction&) = delete;
    ParticleExtraction& operator=(const ParticleExtraction&) = delete;

    /// Uploads the particles of every emitter the simulation state holds and queues their draws
    /// for each view of the frame.
    void Extract(ECS::World& world, uint64 worldId, Engine::Renderer::RenderServices* services, float32 deltaTime);
    /// The simulation state to draw from. Without one, extraction runs a simulation of its own.
    void SetSimulationState(std::shared_ptr<ParticleWorldState> state);
    /// What the last Extract drew and what the simulation it drew from cost.
    ParticleExtractionStats Stats() const;

  private:
    void EmitForwardCommands(Engine::Renderer::ForwardEmitContext& context);
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};
} // namespace GameEngine::Particles

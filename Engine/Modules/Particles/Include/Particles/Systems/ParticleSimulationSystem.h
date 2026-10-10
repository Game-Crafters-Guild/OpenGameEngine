#pragma once

#include "AssetCore/GUID.h"
#include "Components/Rendering/Particles.h"
#include "ECS/Systems.h"
#include "Mathematics/Matrix4x4.h"
#include "Particles/CompiledParticleStack.h"
#include "Particles/ParticleRuntime.h"

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
class AssetManager;
}

namespace GameEngine::Particles
{

/// What the simulation keeps for one emitter entity between frames.
struct ParticleEmitterState
{
    ECS::EntityHandle Entity{};
    ParticleRuntime Simulation;
    PreviewSeek PreviewSeek;
    Components::ParticleEmitter3D Emitter{};
    Mathematics::Matrix4x4 Transform;
    /// The compiled stack the simulation runs, and the reason it runs none when it cannot.
    std::shared_ptr<const CompiledParticleStack> Stack;
    std::string StackError;
    uint64 Seen = 0;
    uint32 Restart = 0, SingleStep = 0, Seek = 0;
    /// A seek the runtime was restarted under (a structural stack edit, a change of simulation
    /// space) starts again on the new runtime.
    bool ResumeSeek = false;
};

/// Shared by the simulation and render extraction, owned by their SystemManager. No global
/// singleton or graphics-device lifetime participates in simulation ownership.
struct ParticleWorldState
{
    std::unordered_map<ECS::EntityId, ParticleEmitterState> Emitters;
    /// Where emitters' stack assets are resolved; without one only the default stack runs.
    AssetManager* Assets = nullptr;
    /// A stack asset the simulation asked the asset manager to load.
    struct StackLoad
    {
        /// The frame the load may next be requested.
        uint64 RetryFrame = 0;
        /// Whether the last load failed; written by the loader thread.
        std::shared_ptr<std::atomic<bool>> Failed = std::make_shared<std::atomic<bool>>(false);
    };
    std::unordered_map<GUID, StackLoad> StackLoads;
    uint64 WorldId = 0, Frame = 0;
    /// Clearing a world recycles entity handles without changing its identity.
    uint64 LifecycleResetGeneration = 0;
    uint32 LiveParticles = 0, Budget = 65536u;
    float SimulationMs = 0.0f;
};

/// Advances every ParticleEmitter3D before render extraction, one emitter after another in entity
/// order, within the world's particle budget.
class ParticleSimulationSystem : public ECS::ISystem
{
  public:
    explicit ParticleSimulationSystem(std::shared_ptr<ParticleWorldState> state = std::make_shared<ParticleWorldState>())
        : m_State(std::move(state))
    {
    }
    const char* GetName() const override { return "ParticleSimulationSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;
    const std::shared_ptr<ParticleWorldState>& State() const { return m_State; }

  private:
    void RouteSpawnEvents(ECS::World& world, ParticleEmitterState& source);

    std::shared_ptr<ParticleWorldState> m_State;
    std::vector<ECS::EntityId> m_Order;
};

} // namespace GameEngine::Particles

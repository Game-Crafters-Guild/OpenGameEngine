#pragma once

#include "Components/AssetRef.h"
#include "ECS/Entity.h"
#include "Mathematics/Vector3.h"
#include "Types/StringId.h"
#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

/// Child emitters one emitter can spawn into; a stack's spawn rules name them by slot.
inline constexpr uint32 ParticleMaxSubEmitters = 4u;

enum class ParticleEmitterDimension : uint32
{
    /// Particles stay on the emitter's XY plane.
    World2D = 0,
    World3D = 1,
};

/// One child emitter slot of a ParticleEmitter3D.
// @ge-no-add  A slot nested in ParticleEmitter3D, not a component of its own.
struct ParticleSubEmitter
{
    // @ge-tooltip The emitter a spawn rule naming this slot spawns its particles in
    ECS::EntityHandle Emitter{};
};

/// A particle emitter: when and how fast its simulation runs. What the particles do is the processor
/// stack asset it references; how they draw is the entity's ParticleRenderer.
struct ParticleEmitter3D
{
    // @ge-tooltip The processor stack the particles run; none runs the default stack
    ParticleStackRef Stack{};
    // @ge-tooltip Spawn new particles. Particles already alive keep simulating when off
    bool Emitting = true;
    // @ge-tooltip Most live particles
    // @ge-range 0 4096
    uint32 Amount = 8u;
    // @ge-tooltip Seconds simulated before the effect is first shown
    // @ge-range 0 60
    float32 PrewarmSeconds = 0.0f;
    // @ge-tooltip Multiplies the simulation clock
    // @ge-range 0
    float32 SimulationSpeed = 1.0f;
    // @ge-tooltip 2D keeps particles on the emitter's XY plane
    ParticleEmitterDimension Dimension = ParticleEmitterDimension::World3D;
    // @ge-tooltip Particles move with the emitter instead of staying where they were born
    bool LocalSpace = false;
    // @ge-tooltip Simulation steps per second; 0 steps once per frame, in bounded steps
    // @ge-range 0 240
    uint32 TicksPerSecond = 30u;
    // @ge-tooltip Draw particles between simulation steps. Off shows each step as it lands
    bool Interpolate = true;
    // @ge-tooltip Random sequence of the effect; 0 derives one from the entity
    uint32 Seed = 0u;
    // @ge-tooltip Child emitters the stack's spawn rules name by slot
    ParticleSubEmitter SubEmitters[ParticleMaxSubEmitters]{};
};

/// Most named events a ParticlePlayback holds until the emitter's next update.
inline constexpr uint32 ParticleMaxPendingEvents = 8u;

/// Commands to a running emitter and what it reports back: pause, restart, step, seek, and named
/// events for the stack's game-event rules. Counters let a paused system consume each request once.
// [DoNotSerialize] — runtime commands and live counters, not scene authoring.
// @ge-no-add  Added by the particle preview controls, or by a game that commands an emitter.
struct ParticlePlayback
{
    bool Paused = false;
    uint32 Restart = 0u, SingleStep = 0u, Seek = 0u;
    float64 SeekTime = 0.0;
    /// Events SendEvent queued, delivered to every live particle on the emitter's next update.
    StringId PendingEvents[ParticleMaxPendingEvents]{};
    uint32 PendingEventCount = 0u;
    uint32 LiveCount = 0u, SimulatedSteps = 0u;
    float64 SimulatedTime = 0.0, DroppedTime = 0.0;
    float32 SimulationMs = 0.0f;

    /// Queues `name` for the emitter's game-event rules. False when ParticleMaxPendingEvents events
    /// already wait for the next update; the event is then dropped.
    bool SendEvent(StringId name)
    {
        if (PendingEventCount >= ParticleMaxPendingEvents)
            return false;
        PendingEvents[PendingEventCount++] = name;
        return true;
    }
};

/// The world's particle budget: the most live particles every emitter together simulates.
struct ParticleWorldSettings
{
    // @ge-tooltip Most live particles every emitter in the world together simulates
    // @ge-range 0 65536
    uint32 MaxParticles = 65536u;
};

// @ge-no-add  Per-collision event record stored in ParticleCollisionEventsBuffer, not a standalone component.
// [DoNotSerialize] — per-frame collision record written fresh each frame during simulation; never authored.
struct ParticleCollisionEvent
{
    ECS::EntityHandle Emitter{};
    uint32 ParticleId = 0u;
    bool KilledParticle = false;
    Mathematics::Vector3 Position = {0.0f, 0.0f, 0.0f};
    Mathematics::Vector3 Normal = {0.0f, 1.0f, 0.0f};
    Mathematics::Vector3 Velocity = {0.0f, 0.0f, 0.0f};
    float32 Speed = 0.0f;
};

// Singleton-style per-world buffer for particle collision events.
// Attach this to any entity to opt into collision event collection for the frame.
// @ge-no-add  Runtime-only (Count/Events reset+written per frame); added via the
// ParticleCollisionEvents preset, not as a standalone Add-menu single.
// [DoNotSerialize] — per-frame buffer cleared and repopulated every frame during simulation.
struct ParticleCollisionEventsBuffer
{
    static constexpr uint32 kMaxEvents = 64u;
    uint32 Count = 0u;
    ParticleCollisionEvent Events[kMaxEvents]{};

    void Clear()
    {
        Count = 0u;
    }

    bool Push(const ParticleCollisionEvent& event)
    {
        if (Count >= kMaxEvents)
            return false;
        Events[Count++] = event;
        return true;
    }
};

/// A 2D emitter: particles on the emitter's XY plane.
ParticleEmitter3D MakeParticleEmitter2DDefaults();

static_assert(std::is_trivially_copyable_v<ParticleEmitter3D>, "ParticleEmitter3D must be trivially copyable for ECS");
static_assert(std::is_standard_layout_v<ParticleEmitter3D>, "ParticleEmitter3D must be standard layout for ECS");
static_assert(std::is_trivially_copyable_v<ParticlePlayback>, "ParticlePlayback must be trivially copyable for ECS");
static_assert(std::is_trivially_copyable_v<ParticleCollisionEvent>, "ParticleCollisionEvent must be trivially copyable for ECS");
static_assert(std::is_trivially_copyable_v<ParticleCollisionEventsBuffer>,
              "ParticleCollisionEventsBuffer must be trivially copyable for ECS");

} // namespace GameEngine::Components

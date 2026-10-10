#pragma once

#include "Types/Types.h"

namespace GameEngine::Particles
{
struct ParticleProcessorDescriptor;

/// The moment an event rule is evaluated.
enum class ParticleEventTrigger : uint8
{
    Birth,
    Death,
    Collision,
    /// Once, when the particle's age reaches Seconds.
    Age,
    /// Once per phase entry, when the phase age reaches Seconds.
    PhaseAge,
    Enter,
    Exit,
    /// When the game sends the named event.
    External,
    /// Every Spacing units the particle travels.
    Distance,
    /// Every Seconds of the particle's phase age.
    Interval
};

enum class ParticleEventAction : uint8
{
    /// Move the particle to another phase.
    Transition,
    /// Spawn particles in a child emitter.
    Emit,
    Kill
};

/// What a child particle takes from its parent, one bit per property.
enum ParticleInherit : uint32
{
    kParticleInheritVelocity = 1u << 0,
    kParticleInheritSize = 1u << 1,
    kParticleInheritColor = 1u << 2,
    kParticleInheritRotation = 1u << 3,
    kParticleInheritLifetime = 1u << 4,
    kParticleInheritAll = (1u << 5) - 1u
};

inline constexpr uint32 kParticleEventNameCapacity = 64;

/// An event rule: when Trigger fires, with Probability, run Action.
struct ParticleEventParameters
{
    ParticleEventTrigger Trigger = ParticleEventTrigger::PhaseAge;
    ParticleEventAction Action = ParticleEventAction::Transition;
    float Seconds = 1.0f;
    float Spacing = 1.0f;
    float Probability = 1.0f;
    char EventName[kParticleEventNameCapacity] = {};
    /// Phase id a Transition moves the particle to.
    uint32 Destination = 0;
    /// Sub-emitter slot of the emitter (ParticleEmitter3D::SubEmitters) an Emit spawns into.
    uint8 SubEmitter = 0;
    uint32 Count = 1;
    uint32 Inherit = kParticleInheritVelocity | kParticleInheritSize;
    float VelocityScale = 1.0f;
    bool AlignToNormal = false;
    /// Kill the particle after an Emit.
    bool KillSource = false;
};

/// The registered "event" processor.
const ParticleProcessorDescriptor& ParticleEventProcessor();
} // namespace GameEngine::Particles

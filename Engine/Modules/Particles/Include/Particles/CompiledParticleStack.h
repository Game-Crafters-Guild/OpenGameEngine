#pragma once

#include "Particles/ParticleChannels.h"
#include "Particles/ParticleProcessorRegistry.h"
#include "Particles/ParticleStackDocument.h"
#include "Particles/Processors/ParticleEventProcessor.h"
#include "Particles/Processors/ParticleTrailProcessor.h"
#include "Types/StringId.h"
#include "Types/Types.h"

#include <array>
#include <limits>
#include <memory>
#include <vector>

namespace GameEngine::Particles
{

inline constexpr uint32 kParticleEventTriggerCount = 10;

/// A processor resolved for execution: its descriptor, its parameter block and the data its Compile
/// function derived. Parameter pointers refer into the compiled stack's document.
struct CompiledProcessor
{
    const ParticleProcessorDescriptor* Descriptor = nullptr;
    const void* Parameters = nullptr;
    std::shared_ptr<const void> Compiled;
    uint32 Id = 0;
    /// Position among the phase's processors (collision contact bits are per position).
    uint32 Ordinal = 0;
    ParticleClock Clock = ParticleClock::PhaseAge;
    float Start = 0.0f;
    float End = std::numeric_limits<float>::infinity();
    /// False when the processor runs for every particle of its phase every tick.
    bool Bounded = false;
};

/// An event rule resolved for evaluation.
struct CompiledEventRule
{
    const ParticleEventParameters* Parameters = nullptr;
    uint32 ProcessorId = 0;
    /// Bit of the particle's FiredEvents channel that marks a once-per-phase rule as fired.
    uint32 Bit = 0;
    /// Index of the destination phase in CompiledParticleStack::Phases.
    uint32 DestinationPhase = 0;
    StringId EventName = 0;
};

/// A phase's processors grouped by stage in execution order, and its rules grouped by trigger.
struct CompiledPhase
{
    uint32 Id = 0;
    std::vector<CompiledProcessor> Birth;
    std::vector<CompiledProcessor> Enter;
    std::vector<CompiledProcessor> UpdateBeforeIntegration;
    std::vector<CompiledProcessor> UpdateAfterIntegration;
    std::vector<CompiledProcessor> Exit;
    std::array<std::vector<CompiledEventRule>, kParticleEventTriggerCount> Rules;

    const std::vector<CompiledEventRule>& RulesFor(ParticleEventTrigger trigger) const
    {
        return Rules[static_cast<uint32>(trigger)];
    }
};

/// A validated stack in the form the runtime executes: flat per-phase processor lists, rule tables,
/// and the channels its processors declared. Immutable and shared by every emitter using it.
struct CompiledParticleStack
{
    std::shared_ptr<const StackDocument> Document;
    std::vector<CompiledPhase> Phases;
    uint32 EntryPhase = 0;
    std::vector<CompiledProcessor> Emission;
    ParticleChannelMask Channels = 0;
    float Lifetime = 1.0f;
    bool HasTrails = false;
    ParticleTrailParameters Trail{};
    bool HasLights = false;

    /// Index of the phase with `id`, or Phases.size().
    uint32 PhaseIndex(uint32 id) const;
};

/// Validates `document` and resolves it for execution. Returns null with the reasons in
/// `diagnostics` when the document is invalid.
std::shared_ptr<const CompiledParticleStack> CompileParticleStack(std::shared_ptr<const StackDocument> document,
                                                                  std::vector<StackDiagnostic>& diagnostics);

} // namespace GameEngine::Particles

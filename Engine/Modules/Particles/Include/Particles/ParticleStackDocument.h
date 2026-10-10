#pragma once

#include "Mathematics/Vector3.h"
#include "Particles/ParticleProcessorRegistry.h"
#include "Types/Types.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::Particles
{

inline constexpr uint32 kStackSchemaVersion = 1;
inline constexpr uint32 kMaxStackProcessors = 64;
inline constexpr uint32 kMaxStackPhases = 16;
inline constexpr uint32 kMaxGeometryVertices = 65536;

/// A problem found while reading or validating a stack. Path names the phase or processor.
struct StackDiagnostic
{
    std::string Path;
    std::string Message;
};

/// The clock a processor's active interval is measured on.
enum class ParticleClock : uint8
{
    Age,
    PhaseAge
};

/// Vertices and triangle indices a mesh-shaped processor emits from, in the processor's space.
struct ParticleProcessorGeometry
{
    std::vector<Mathematics::Vector3> Vertices;
    std::vector<uint32> Indices;
};

/// One processor of a phase: its type, its common header and its parameter block. The block is
/// the descriptor's parameter struct, stored as bytes so every processor type shares one layout.
struct ParticleProcessorInstance
{
    uint32 Id = 0;
    std::string Label;
    bool Enabled = true;
    ParticleStage Stage = ParticleStage::Update;
    /// Active from Start until End seconds on Clock; no End keeps it active for the rest of life.
    ParticleClock Clock = ParticleClock::PhaseAge;
    float Start = 0.0f;
    std::optional<float> End;
    const ParticleProcessorDescriptor* Descriptor = nullptr;
    std::vector<std::byte> Parameters;
    ParticleProcessorGeometry Geometry;

    template <typename T>
    T& Params()
    {
        return *reinterpret_cast<T*>(Parameters.data());
    }

    template <typename T>
    const T& Params() const
    {
        return *reinterpret_cast<const T*>(Parameters.data());
    }
};

/// A new instance of `descriptor` with default parameters, its default stage and no id.
ParticleProcessorInstance MakeProcessorInstance(const ParticleProcessorDescriptor& descriptor);

/// A group of processors particles run while they are in it; event rules move particles between
/// phases.
struct StackPhase
{
    uint32 Id = 1;
    std::string Label = "Spawn";
    std::vector<ParticleProcessorInstance> Processors;
};

/// A particle effect: phases of processors, the phase particles are born into, and the lifetime a
/// particle has before any processor sets one.
struct StackDocument
{
    uint32 Version = kStackSchemaVersion;
    uint32 EntryPhase = 1;
    float Lifetime = 1.0f;
    std::vector<StackPhase> Phases;
};

/// Checks limits, ids, stages, references and every parameter against its declared bounds.
/// Returns false when any error was found; `diagnostics` lists them all.
bool ValidateStack(const StackDocument& document, std::vector<StackDiagnostic>& diagnostics);

/// True when both documents have the same phases and processors (ids, types, stages, order) and
/// differ at most in parameter values and labels: live particles can continue under `updated`.
bool HaveSameStructure(const StackDocument& previous, const StackDocument& updated);

/// The stack an emitter uses before it references one: continuous emission, an upward launch in a
/// cone and gravity.
StackDocument MakeDefaultStack();

/// The most particles the stack's continuous emission keeps alive at once: every entry-phase
/// Emission Rate at its highest rate, times the longest lifetime a particle is born with (the
/// stack's lifetime, or the highest a Birth property sets). Bursts and phase rules are not counted;
/// 0 when the entry phase emits nothing continuously.
float ContinuousEmissionPeak(const StackDocument& document);

/// The phase with `id`, or null.
StackPhase* FindPhase(StackDocument& document, uint32 id);
/// The phase with `id`, or null.
const StackPhase* FindPhase(const StackDocument& document, uint32 id);
/// The processor with `id` in any phase, or null.
ParticleProcessorInstance* FindProcessor(StackDocument& document, uint32 id);
/// The processor with `id` in any phase, or null.
const ParticleProcessorInstance* FindProcessor(const StackDocument& document, uint32 id);

/// Appends an empty phase named `label`, giving it the next free id. Fails when the stack holds
/// its most phases; `error` says why.
bool AddPhase(StackDocument& document, std::string_view label, uint32& id, std::string& error);
/// Removes the phase with `id`. Refuses the entry phase and a phase an event rule moves particles
/// to; `error` says which.
bool RemovePhase(StackDocument& document, uint32 id, std::string& error);
/// Appends `processor` to `phase`, giving it the next free id.
bool AddProcessor(StackDocument& document, uint32 phase, ParticleProcessorInstance processor, uint32& id,
                  std::string& error);
/// Removes the processor with `id`; false when no phase holds it.
bool RemoveProcessor(StackDocument& document, uint32 id);
/// Moves a processor to just before (or after) `target` in the same phase.
bool ReorderProcessor(StackDocument& document, uint32 phase, uint32 processor, uint32 target, bool after);

} // namespace GameEngine::Particles

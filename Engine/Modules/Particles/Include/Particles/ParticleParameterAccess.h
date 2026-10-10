#pragma once

#include "Particles/ParticleProcessorRegistry.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace GameEngine::Particles
{

/// One parameter of a processor: its reflected storage and its presentation.
struct ParticleParameter
{
    const ECS::FieldInfo* Field = nullptr;
    const ParticleParameterField* Presentation = nullptr;

    ParticleParameterKind Kind() const { return Presentation->Kind; }
    /// Number of values in a Value field (C arrays hold up to four).
    uint32 ValueCount() const;
};

/// Visits the parameters of `descriptor` in reflected order.
void ForEachParticleParameter(const ParticleProcessorDescriptor& descriptor,
                              const std::function<void(const ParticleParameter&)>& visit);

/// The saved key of a reflected field: its name with a lowercase first letter.
std::string ParticleParameterKey(std::string_view fieldName);

/// Typed access to one field of a parameter block.
template <typename T>
T* ParticleParameterPointer(void* parameters, const ECS::FieldInfo& field, uint32 element = 0)
{
    return reinterpret_cast<T*>(static_cast<std::byte*>(parameters) + field.Offset) + element;
}

template <typename T>
const T* ParticleParameterPointer(const void* parameters, const ECS::FieldInfo& field, uint32 element = 0)
{
    return reinterpret_cast<const T*>(static_cast<const std::byte*>(parameters) + field.Offset) + element;
}

/// Integer value of an Enum field (stored as one byte) or a UInt, Phase or Flags field.
uint32 ReadParticleParameterInteger(const void* parameters, const ParticleParameter& parameter);
void WriteParticleParameterInteger(void* parameters, const ParticleParameter& parameter, uint32 value);

/// The option of an Enum field whose value is `value`, or null.
const ParticleEnumOption* FindParticleOption(std::span<const ParticleEnumOption> options, uint32 value);
const ParticleEnumOption* FindParticleOption(std::span<const ParticleEnumOption> options, std::string_view wireName);

} // namespace GameEngine::Particles

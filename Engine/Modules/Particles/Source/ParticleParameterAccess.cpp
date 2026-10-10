#include "Particles/ParticleParameterAccess.h"

#include <cctype>
#include <cstring>

namespace GameEngine::Particles
{

uint32 ParticleParameter::ValueCount() const
{
    return Presentation->Kind == ParticleParameterKind::Value
               ? Field->Size / static_cast<uint32>(sizeof(ParticleValue))
               : 1;
}

void ForEachParticleParameter(const ParticleProcessorDescriptor& descriptor,
                              const std::function<void(const ParticleParameter&)>& visit)
{
    for (const auto& field : descriptor.Fields)
        for (const auto& presentation : descriptor.Parameters)
            if (presentation.Name == field.Name)
            {
                visit({&field, &presentation});
                break;
            }
}

std::string ParticleParameterKey(std::string_view fieldName)
{
    std::string key(fieldName);
    if (!key.empty())
        key[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(key[0])));
    return key;
}

uint32 ReadParticleParameterInteger(const void* parameters, const ParticleParameter& parameter)
{
    if (parameter.Kind() == ParticleParameterKind::Enum)
        return *ParticleParameterPointer<uint8>(parameters, *parameter.Field);
    uint32 value = 0;
    std::memcpy(&value, ParticleParameterPointer<std::byte>(parameters, *parameter.Field), sizeof(value));
    return value;
}

void WriteParticleParameterInteger(void* parameters, const ParticleParameter& parameter, uint32 value)
{
    if (parameter.Kind() == ParticleParameterKind::Enum)
    {
        *ParticleParameterPointer<uint8>(parameters, *parameter.Field) = static_cast<uint8>(value);
        return;
    }
    std::memcpy(ParticleParameterPointer<std::byte>(parameters, *parameter.Field), &value, sizeof(value));
}

const ParticleEnumOption* FindParticleOption(std::span<const ParticleEnumOption> options, uint32 value)
{
    for (const auto& option : options)
        if (option.Value == value)
            return &option;
    return nullptr;
}

const ParticleEnumOption* FindParticleOption(std::span<const ParticleEnumOption> options, std::string_view wireName)
{
    for (const auto& option : options)
        if (option.WireName == wireName)
            return &option;
    return nullptr;
}

} // namespace GameEngine::Particles

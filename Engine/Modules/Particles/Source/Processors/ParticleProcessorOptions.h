#pragma once

#include "Particles/ParticleChannels.h"
#include "Particles/ParticleProcessorRegistry.h"
#include "Particles/Processors/ParticleProcessorTypes.h"

#include <array>
#include <string_view>

namespace GameEngine::Particles
{
// Option tables several built-in processors share. Wire names are the saved identifiers.

inline constexpr std::array<ParticleEnumOption, 3> kParticleSpaceOptions = {{
    {"simulation", "Simulation", static_cast<uint32>(ParticleSpace::Simulation), "particle-choice-move"},
    {"world", "World", static_cast<uint32>(ParticleSpace::World), "particle-choice-move"},
    {"local", "Emitter Local", static_cast<uint32>(ParticleSpace::Local), "particle-choice-move"},
}};

inline constexpr std::array<ParticleEnumOption, 3> kParticleOperationOptions = {{
    {"set", "Set", static_cast<uint32>(ParticleOperation::Set)},
    {"add", "Add", static_cast<uint32>(ParticleOperation::Add)},
    {"multiply", "Multiply", static_cast<uint32>(ParticleOperation::Multiply)},
}};

inline constexpr std::array<ParticleEnumOption, 3> kParticleBasisOptions = {{
    {"current", "Current Value", static_cast<uint32>(ParticleBasis::Current)},
    {"birth", "Value at Birth", static_cast<uint32>(ParticleBasis::Birth)},
    {"entry", "Value at Phase Entry", static_cast<uint32>(ParticleBasis::Entry)},
}};

inline constexpr std::array<ParticleEnumOption, kParticleAttributeCount> kParticleAttributeOptions = {{
    {"position", "Position", static_cast<uint32>(ParticleAttribute::Position), "particle-choice-move"},
    {"velocity", "Velocity", static_cast<uint32>(ParticleAttribute::Velocity), "particle-choice-move"},
    {"size", "Size", static_cast<uint32>(ParticleAttribute::Size), "particle-choice-size"},
    {"scale", "Scale", static_cast<uint32>(ParticleAttribute::Scale), "particle-choice-size"},
    {"color", "Color", static_cast<uint32>(ParticleAttribute::Color), "particle-choice-color"},
    {"alpha", "Alpha", static_cast<uint32>(ParticleAttribute::Alpha), "particle-choice-color"},
    {"rotation", "Rotation", static_cast<uint32>(ParticleAttribute::Rotation), "particle-choice-loop"},
    {"spin", "Spin", static_cast<uint32>(ParticleAttribute::Spin), "particle-choice-loop"},
    {"lifetime", "Lifetime", static_cast<uint32>(ParticleAttribute::Lifetime), "particle-choice-time"},
    {"animationSpeed", "Animation Speed", static_cast<uint32>(ParticleAttribute::AnimationSpeed), "particle-choice-animation"},
    {"animationOffset", "Animation Offset", static_cast<uint32>(ParticleAttribute::AnimationOffset), "particle-choice-animation"},
    {"custom0", "Custom 1", static_cast<uint32>(ParticleAttribute::Custom0), "particle-choice-custom"},
    {"custom1", "Custom 2", static_cast<uint32>(ParticleAttribute::Custom1), "particle-choice-custom"},
    {"custom2", "Custom 3", static_cast<uint32>(ParticleAttribute::Custom2), "particle-choice-custom"},
    {"custom3", "Custom 4", static_cast<uint32>(ParticleAttribute::Custom3), "particle-choice-custom"},
}};

inline constexpr std::array<std::string_view, 3> kParticleVectorLabels = {"X", "Y", "Z"};
inline constexpr std::array<std::string_view, 4> kParticleColorLabels = {"Red", "Green", "Blue", "Alpha"};

// The declared channel set of one ParticleValueInput driver.
ParticleChannelMask DriverChannels(ParticleDriver driver);
} // namespace GameEngine::Particles

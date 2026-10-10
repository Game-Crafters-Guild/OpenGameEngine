#pragma once

#include "Particles/ParticleStackDocument.h"
#include "Types/Types.h"

#include <span>
#include <string>
#include <vector>

namespace GameEngine::Particles
{

/// Leading bytes of a cooked particle stack.
inline constexpr char kParticleStackBinaryMagic[4] = {'G', 'E', 'P', 'S'};
/// Largest cooked stack the reader accepts.
inline constexpr uint32 kMaxParticleStackBinaryBytes = 16u * 1024u * 1024u;

/// True when `bytes` start with the cooked stack magic.
bool IsParticleStackBinary(std::span<const uint8> bytes);

/// Writes the cooked form of a valid `document`: little-endian, every processor's parameters in its
/// reflected field order, no JSON. Fails without touching `out` when the document is invalid.
bool WriteParticleStackBinary(const StackDocument& document, std::vector<uint8>& out, std::string& error);

/// Reads a cooked stack, checking every length before it allocates, then validates it. Failure
/// leaves `output` untouched and explains why in `diagnostics`.
bool ReadParticleStackBinary(std::span<const uint8> bytes, StackDocument& output,
                             std::vector<StackDiagnostic>& diagnostics);

} // namespace GameEngine::Particles

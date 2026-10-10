#pragma once

#include "Particles/ParticleStackDocument.h"
#include "Types/Types.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::Particles
{

/// Reads the JSON authoring form of a stack. Every processor type and parameter must be known;
/// documents nested deeper than a stack can be are refused before they are walked. Failure leaves
/// `output` untouched and explains why in `diagnostics`.
bool ParseParticleStack(std::string_view source, StackDocument& output, std::vector<StackDiagnostic>& diagnostics);

/// Writes the JSON authoring form, indented by `indent` spaces per level (-1 writes one line);
/// parameters equal to their processor's defaults are left out.
std::string SerializeParticleStack(const StackDocument& document, int indent);

/// Reads either form: the cooked binary when the bytes carry its magic, otherwise JSON.
bool LoadParticleStack(std::span<const uint8> bytes, StackDocument& output, std::vector<StackDiagnostic>& diagnostics);

} // namespace GameEngine::Particles

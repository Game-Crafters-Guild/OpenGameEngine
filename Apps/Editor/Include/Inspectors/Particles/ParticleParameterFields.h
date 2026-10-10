#pragma once

#include "InspectorRegistry.h"
#include "Particles/ParticleStackDocument.h"

#include <memory>

namespace GameEngine
{
class UIElement;
}

namespace GameEngine::ParticleInspectors
{
class ParticleStackEditor;

/// Builds the rows of one processor's parameters from its descriptor: a row per visible field, in
/// the descriptor's order, each editing the processor through `editor`. Nothing here knows a
/// processor type, so a processor a game registers gets the same rows.
void AddParticleParameterFields(UIElement* parent, const InspectorContext& context,
                                const std::shared_ptr<ParticleStackEditor>& editor,
                                const Particles::StackDocument& document,
                                const Particles::ParticleProcessorInstance& processor);

} // namespace GameEngine::ParticleInspectors

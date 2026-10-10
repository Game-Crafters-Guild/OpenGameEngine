#pragma once

#include "InspectorRegistry.h"

namespace GameEngine::Particles
{
class ParticleStackAsset;
}

namespace GameEngine
{
/// Registers the inspector of particle stack assets: their phases and, for every processor, a
/// section built from the processor's registered descriptor.
void RegisterParticleStackInspector();

namespace ParticleInspectors
{
/// Gives `element` the particle stack's stylesheet (processor icons, value and curve rows) for its
/// subtree. The stack inspector's holders call it: the stack asset's inspector, the emitter's
/// Processor Stack section and the Add Processor picker.
void ApplyParticleStackStyle(UIElement* element);

/// Adds the rows that edit `asset` under `context.Parent`: the rows the stack's asset inspector shows,
/// which the emitter inspector also shows for the stack its emitter runs.
void AddParticleStackRows(const InspectorContext& context, Particles::ParticleStackAsset& asset);
} // namespace ParticleInspectors
} // namespace GameEngine

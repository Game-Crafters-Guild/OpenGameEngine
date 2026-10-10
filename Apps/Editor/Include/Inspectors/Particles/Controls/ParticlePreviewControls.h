#pragma once

#include "InspectorRegistry.h"

namespace GameEngine::ParticleInspectors
{
/// Adds the emitter's preview controls to the inspector: Restart, Pause and Step, a Preview Time
/// slider that scrubs, and a card with the live particle count and the simulation's cost. The first
/// action adds the ParticlePlayback component the controls drive; opening the inspector changes
/// nothing in the world.
void AddParticlePreview(const InspectorContext& ctx);
}

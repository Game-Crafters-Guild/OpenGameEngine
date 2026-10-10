#pragma once

#include "InspectorRegistry.h"

namespace GameEngine::ParticleInspectors
{

/// Adds the emitter's Processor Stack section under `context.Parent`: the stack asset the emitter
/// runs, a way to create one, and the rows that edit it. Editing the stack edits every emitter that
/// runs it.
void AddEmitterStackSection(const InspectorContext& context);

} // namespace GameEngine::ParticleInspectors

#pragma once

#include "Particles/ParticleProcessorRegistry.h"

#include <vector>

namespace GameEngine::Particles
{
// One factory per built-in processor, each defined in that processor's own file.
ParticleProcessorDescriptor MakePropertyProcessorDescriptor();
ParticleProcessorDescriptor MakeAccelerationProcessorDescriptor();
ParticleProcessorDescriptor MakeDragProcessorDescriptor();
ParticleProcessorDescriptor MakeLimitSpeedProcessorDescriptor();
ParticleProcessorDescriptor MakeNoiseProcessorDescriptor();
ParticleProcessorDescriptor MakeCollisionProcessorDescriptor();
ParticleProcessorDescriptor MakeLightProcessorDescriptor();
ParticleProcessorDescriptor MakeShapeProcessorDescriptor();
ParticleProcessorDescriptor MakeVelocityConeProcessorDescriptor();
ParticleProcessorDescriptor MakeOrbitProcessorDescriptor();
ParticleProcessorDescriptor MakeEmitRateProcessorDescriptor();
ParticleProcessorDescriptor MakeEmitBurstProcessorDescriptor();
ParticleProcessorDescriptor MakeEventProcessorDescriptor();
ParticleProcessorDescriptor MakeTrailProcessorDescriptor();

// The built-in descriptors in picker order.
std::vector<ParticleProcessorDescriptor> MakeBuiltInParticleProcessors();

// The descriptor registered under a built-in id; asserts that it exists.
const ParticleProcessorDescriptor& FindBuiltInParticleProcessor(std::string_view id);
} // namespace GameEngine::Particles

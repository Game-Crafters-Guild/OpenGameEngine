#include "Processors/ParticleBuiltInProcessors.h"

#include <cassert>

namespace GameEngine::Particles
{
std::vector<ParticleProcessorDescriptor> MakeBuiltInParticleProcessors()
{
    return {
        MakeEmitRateProcessorDescriptor(),
        MakeEmitBurstProcessorDescriptor(),
        MakeShapeProcessorDescriptor(),
        MakeVelocityConeProcessorDescriptor(),
        MakePropertyProcessorDescriptor(),
        MakeAccelerationProcessorDescriptor(),
        MakeDragProcessorDescriptor(),
        MakeLimitSpeedProcessorDescriptor(),
        MakeNoiseProcessorDescriptor(),
        MakeOrbitProcessorDescriptor(),
        MakeCollisionProcessorDescriptor(),
        MakeLightProcessorDescriptor(),
        MakeTrailProcessorDescriptor(),
        MakeEventProcessorDescriptor(),
    };
}

const ParticleProcessorDescriptor& FindBuiltInParticleProcessor(std::string_view id)
{
    const auto* descriptor = ParticleProcessorRegistry::Find(id);
    assert(descriptor && "Built-in particle processors are registered on first lookup");
    return *descriptor;
}
} // namespace GameEngine::Particles

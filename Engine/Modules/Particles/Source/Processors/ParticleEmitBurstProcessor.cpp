#include "Particles/Processors/ParticleEmitBurstProcessor.h"

#include "Processors/ParticleBuiltInProcessors.h"

#include <cmath>

GE_REFLECT(GameEngine::Particles::ParticleEmitBurstParameters, Time, Count, RepeatInterval);

namespace GameEngine::Particles
{
namespace
{
using Parameters = ParticleEmitBurstParameters;

constexpr double kTimeTolerance = 1e-6;
constexpr uint32 kMaximumBurstsPerTick = 64;
constexpr float kMaximumBurstCount = 4096.0f;

constexpr ParticleParameterField kFields[] = {
    {.Name = "Time", .Label = "Time", .Tooltip = "Emitter time of the first burst, in seconds", .Kind = ParticleParameterKind::Float, .Minimum = 0.0f},
    {.Name = "Count", .Label = "Count", .Tooltip = "Particles per burst", .Kind = ParticleParameterKind::UInt, .Minimum = 0.0f, .Maximum = kMaximumBurstCount},
    {.Name = "RepeatInterval", .Label = "Repeat Every", .Tooltip = "Seconds between bursts; 0 bursts once", .Kind = ParticleParameterKind::Float, .Minimum = 0.0f},
};

// State[0] counts the bursts already fired since emission started.
double Emit(const ParticleEmissionContext& context)
{
    const auto& parameters = *static_cast<const Parameters*>(context.Parameters);
    double& fired = context.State[0];
    const double end = context.Begin + context.Duration + kTimeTolerance;
    double count = 0.0;
    for (uint32 burst = 0; burst < kMaximumBurstsPerTick; ++burst)
    {
        if (fired > 0.0 && parameters.RepeatInterval <= 0.0f)
            break;
        const double time = parameters.Time + fired * parameters.RepeatInterval;
        if (time > end)
            break;
        fired += 1.0;
        // A burst whose time passed before the emission window opened is skipped, not fired late.
        if (time + kTimeTolerance >= context.Begin)
            count += parameters.Count;
    }
    return count;
}
} // namespace

ParticleProcessorDescriptor MakeEmitBurstProcessorDescriptor()
{
    ParticleProcessorDescriptor descriptor;
    descriptor.Id = "emitBurst";
    descriptor.DisplayName = "Emission Burst";
    descriptor.Description = "Spawns a number of particles at once, optionally repeating";
    descriptor.IconClass = "particle-choice-emission";
    descriptor.Category = "Spawn";
    descriptor.Role = ParticleProcessorRole::Emission;
    descriptor.Stages = StageBit(ParticleStage::Emission);
    descriptor.DefaultStage = ParticleStage::Emission;
    descriptor.Emit = Emit;
    BindParticleParameters<Parameters>(descriptor, kFields);
    return descriptor;
}

const ParticleProcessorDescriptor& ParticleEmitBurstProcessor()
{
    return FindBuiltInParticleProcessor("emitBurst");
}

} // namespace GameEngine::Particles

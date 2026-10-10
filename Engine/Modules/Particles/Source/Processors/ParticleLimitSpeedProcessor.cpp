#include "Particles/Processors/ParticleLimitSpeedProcessor.h"

#include "Processors/ParticleBuiltInProcessors.h"
#include "Processors/ParticleValueSamples.h"

#include <algorithm>

GE_REFLECT(GameEngine::Particles::ParticleLimitSpeedParameters, Input, Speed);

namespace GameEngine::Particles
{
namespace
{
using Parameters = ParticleLimitSpeedParameters;

bool UsesCurves(const void* parameters)
{
    return static_cast<const Parameters*>(parameters)->Speed.UsesCurve();
}

constexpr ParticleParameterField kFields[] = {
    {.Name = "Input", .Label = "Curve Input", .Tooltip = "What the curve is evaluated at", .Kind = ParticleParameterKind::ValueInput, .Visible = UsesCurves},
    {.Name = "Speed", .Label = "Maximum Speed", .Tooltip = "Units per second", .Kind = ParticleParameterKind::Value, .Minimum = 0.0f},
};

void Execute(ParticleProcessorContext& context)
{
    const auto& parameters = context.Params<Parameters>();
    const auto samples = SampleParticleValues(context, parameters.Input, std::span<const ParticleValue>(&parameters.Speed, 1));
    auto velocities = context.Channels.Velocities();
    for (size_t k = 0; k < context.Particles.size(); ++k)
    {
        const uint32 i = context.Particles[k];
        const float limit = std::max(0.0f, samples.Get(0, k));
        const float speed = velocities[i].Length();
        if (speed > limit && speed > 0.0f)
            velocities[i] = velocities[i] * (limit / speed);
    }
}
} // namespace

ParticleProcessorDescriptor MakeLimitSpeedProcessorDescriptor()
{
    ParticleProcessorDescriptor descriptor;
    descriptor.Id = "limitSpeed";
    descriptor.DisplayName = "Limit Speed";
    descriptor.Description = "Caps particle speed, keeping the direction of travel";
    descriptor.IconClass = "particle-choice-size";
    descriptor.Category = "Forces";
    descriptor.Stages = StageBit(ParticleStage::Update);
    descriptor.DefaultStage = ParticleStage::Update;
    descriptor.Reads = ChannelBit(ParticleChannel::Velocity);
    descriptor.Writes = ChannelBit(ParticleChannel::Velocity);
    descriptor.Execute = Execute;
    BindParticleParameters<Parameters>(descriptor, kFields);
    return descriptor;
}

const ParticleProcessorDescriptor& ParticleLimitSpeedProcessor()
{
    return FindBuiltInParticleProcessor("limitSpeed");
}

} // namespace GameEngine::Particles

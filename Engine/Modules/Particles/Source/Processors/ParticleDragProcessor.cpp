#include "Particles/Processors/ParticleDragProcessor.h"

#include "Processors/ParticleBuiltInProcessors.h"
#include "Processors/ParticleValueSamples.h"

#include <algorithm>
#include <array>
#include <cmath>

GE_REFLECT(GameEngine::Particles::ParticleDragParameters, Mode, Input, Drag);

namespace GameEngine::Particles
{
namespace
{
using Parameters = ParticleDragParameters;

bool UsesCurves(const void* parameters)
{
    return static_cast<const Parameters*>(parameters)->Drag.UsesCurve();
}

constexpr std::array<ParticleEnumOption, 2> kModes = {{
    {"exponential", "Exponential", static_cast<uint32>(ParticleDragMode::Exponential), "particle-choice-drag"},
    {"linear", "Linear (Friction)", static_cast<uint32>(ParticleDragMode::Linear), "particle-choice-drag"},
}};

constexpr ParticleParameterField kFields[] = {
    {.Name = "Mode", .Label = "Mode", .Tooltip = "Exponential loses a fraction of the velocity per second; Linear loses a fixed speed per second", .Kind = ParticleParameterKind::Enum, .Options = kModes},
    {.Name = "Input", .Label = "Curve Input", .Tooltip = "What the curve is evaluated at", .Kind = ParticleParameterKind::ValueInput, .Visible = UsesCurves},
    {.Name = "Drag", .Label = "Drag", .Tooltip = "Fraction of velocity (Exponential) or units of speed (Linear) lost per second", .Kind = ParticleParameterKind::Value, .Minimum = 0.0f},
};

void Execute(ParticleProcessorContext& context)
{
    const auto& parameters = context.Params<Parameters>();
    auto velocities = context.Channels.Velocities();
    const float dt = context.DeltaTime;
    if (parameters.Mode == ParticleDragMode::Linear)
    {
        const auto samples = SampleParticleValues(context, parameters.Input, std::span<const ParticleValue>(&parameters.Drag, 1));
        for (size_t k = 0; k < context.Particles.size(); ++k)
        {
            const uint32 i = context.Particles[k];
            const float speed = velocities[i].Length();
            const float slowed = std::max(0.0f, speed - std::max(0.0f, samples.Get(0, k)) * dt);
            velocities[i] = speed > 0.0f ? velocities[i] * (slowed / speed) : velocities[i];
        }
        return;
    }
    if (parameters.Drag.Mode == ParticleValueMode::Constant)
    {
        const float factor = std::exp(-std::max(0.0f, parameters.Drag.Minimum) * dt);
        for (const uint32 i : context.Particles)
            velocities[i] = velocities[i] * factor;
        return;
    }
    const auto samples = SampleParticleValues(context, parameters.Input, std::span<const ParticleValue>(&parameters.Drag, 1));
    for (size_t k = 0; k < context.Particles.size(); ++k)
    {
        const uint32 i = context.Particles[k];
        velocities[i] = velocities[i] * std::exp(-std::max(0.0f, samples.Get(0, k)) * dt);
    }
}
} // namespace

ParticleProcessorDescriptor MakeDragProcessorDescriptor()
{
    ParticleProcessorDescriptor descriptor;
    descriptor.Id = "drag";
    descriptor.DisplayName = "Drag";
    descriptor.Description = "Slows particles down, exponentially or by friction";
    descriptor.IconClass = "particle-choice-drag";
    descriptor.Category = "Forces";
    descriptor.Stages = StageBit(ParticleStage::Update);
    descriptor.DefaultStage = ParticleStage::Update;
    descriptor.Reads = ChannelBit(ParticleChannel::Velocity);
    descriptor.Writes = ChannelBit(ParticleChannel::Velocity);
    descriptor.Execute = Execute;
    BindParticleParameters<Parameters>(descriptor, kFields);
    return descriptor;
}

const ParticleProcessorDescriptor& ParticleDragProcessor()
{
    return FindBuiltInParticleProcessor("drag");
}

} // namespace GameEngine::Particles

#include "Particles/Processors/ParticleLightProcessor.h"

#include "Processors/ParticleBuiltInProcessors.h"
#include "Processors/ParticleValueSamples.h"

#include <algorithm>

GE_REFLECT(GameEngine::Particles::ParticleLightParameters, Input, Intensity, Range);

namespace GameEngine::Particles
{
namespace
{
using Parameters = ParticleLightParameters;

constexpr float kMaximumIntensity = 1000000.0f;
constexpr float kMaximumRange = 1000.0f;

bool UsesCurves(const void* parameters)
{
    const auto& settings = *static_cast<const Parameters*>(parameters);
    return settings.Intensity.UsesCurve() || settings.Range.UsesCurve();
}

constexpr ParticleParameterField kFields[] = {
    {.Name = "Input", .Label = "Curve Input", .Tooltip = "What the curves are evaluated at", .Kind = ParticleParameterKind::ValueInput, .Visible = UsesCurves},
    {.Name = "Intensity", .Label = "Intensity", .Tooltip = "Light intensity; the light takes the particle's color", .Kind = ParticleParameterKind::Value, .Minimum = 0.0f, .Maximum = kMaximumIntensity},
    {.Name = "Range", .Label = "Range", .Tooltip = "Light radius in world units", .Kind = ParticleParameterKind::Value, .Minimum = 0.0f, .Maximum = kMaximumRange},
};

void Execute(ParticleProcessorContext& context)
{
    const auto& parameters = context.Params<Parameters>();
    const ParticleValue values[] = {parameters.Intensity, parameters.Range};
    const auto samples = SampleParticleValues(context, parameters.Input, values);
    auto intensities = context.Channels.Get<float>(ParticleChannel::LightIntensity);
    auto ranges = context.Channels.Get<float>(ParticleChannel::LightRange);
    for (size_t k = 0; k < context.Particles.size(); ++k)
    {
        const uint32 i = context.Particles[k];
        intensities[i] = std::clamp(samples.Get(0, k), 0.0f, kMaximumIntensity);
        ranges[i] = std::clamp(samples.Get(1, k), 0.0f, kMaximumRange);
    }
}
} // namespace

ParticleProcessorDescriptor MakeLightProcessorDescriptor()
{
    ParticleProcessorDescriptor descriptor;
    descriptor.Id = "light";
    descriptor.DisplayName = "Light";
    descriptor.Description = "Makes each particle a point light of its color while the processor is active";
    descriptor.IconClass = "particle-choice-light";
    descriptor.Category = "Rendering";
    descriptor.Stages = StageBit(ParticleStage::Update);
    descriptor.DefaultStage = ParticleStage::Update;
    descriptor.Writes = ChannelBits(ParticleChannel::LightIntensity, ParticleChannel::LightRange);
    descriptor.Execute = Execute;
    BindParticleParameters<Parameters>(descriptor, kFields);
    return descriptor;
}

const ParticleProcessorDescriptor& ParticleLightProcessor()
{
    return FindBuiltInParticleProcessor("light");
}

} // namespace GameEngine::Particles

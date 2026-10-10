#include "Particles/Processors/ParticleEmitRateProcessor.h"

#include "Mathematics/Interpolation.h"
#include "Processors/ParticleBuiltInProcessors.h"

#include <algorithm>
#include <array>

GE_REFLECT(GameEngine::Particles::ParticleEmitRateParameters, Driver, InputMinimum, InputMaximum, Rate);

namespace GameEngine::Particles
{
namespace
{
using Parameters = ParticleEmitRateParameters;

constexpr uint32 kRateStream = 1;

constexpr std::array<ParticleEnumOption, 2> kDrivers = {{
    {"emitterAge", "Emitter Age (seconds)", static_cast<uint32>(ParticleEmissionDriver::EmitterAge), "particle-choice-time"},
    {"emitterSpeed", "Emitter Speed", static_cast<uint32>(ParticleEmissionDriver::EmitterSpeed), "particle-choice-move"},
}};

bool UsesCurve(const void* parameters)
{
    return static_cast<const Parameters*>(parameters)->Rate.UsesCurve();
}

constexpr ParticleParameterField kFields[] = {
    {.Name = "Driver", .Label = "Curve Input", .Tooltip = "What the rate curve is evaluated at", .Kind = ParticleParameterKind::Enum, .Options = kDrivers, .Visible = UsesCurve},
    {.Name = "InputMinimum", .Label = "Input From", .Tooltip = "Input value at the curve's start", .Kind = ParticleParameterKind::Float, .Visible = UsesCurve},
    {.Name = "InputMaximum", .Label = "Input To", .Tooltip = "Input value at the curve's end", .Kind = ParticleParameterKind::Float, .Visible = UsesCurve},
    {.Name = "Rate", .Label = "Rate", .Tooltip = "Particles per second", .Kind = ParticleParameterKind::Value, .Minimum = 0.0f},
};

void Validate(const ParticleValidationContext& context)
{
    const auto& parameters = *static_cast<const Parameters*>(context.Parameters);
    if (parameters.Rate.UsesCurve() && !(parameters.InputMaximum > parameters.InputMinimum))
        context.Error("The rate curve input range is empty");
}

double Emit(const ParticleEmissionContext& context)
{
    const auto& parameters = *static_cast<const Parameters*>(context.Parameters);
    const float driver = parameters.Driver == ParticleEmissionDriver::EmitterSpeed ? context.EmitterSpeed
                                                                                   : static_cast<float>(context.Begin);
    ParticleValueInput input;
    input.Minimum = parameters.InputMinimum;
    input.Maximum = parameters.InputMaximum;
    const float rate = EvaluateParticleValue(parameters.Rate, RemapParticleInput(input, driver),
                                             ParticleRandom(context.Seed, 0, context.ProcessorId, kRateStream));
    return std::max(0.0f, rate) * context.Duration;
}
} // namespace

ParticleProcessorDescriptor MakeEmitRateProcessorDescriptor()
{
    ParticleProcessorDescriptor descriptor;
    descriptor.Id = "emitRate";
    descriptor.DisplayName = "Emission Rate";
    descriptor.Description = "Spawns particles continuously";
    descriptor.IconClass = "particle-choice-emission";
    descriptor.Category = "Spawn";
    descriptor.Role = ParticleProcessorRole::Emission;
    descriptor.Stages = StageBit(ParticleStage::Emission);
    descriptor.DefaultStage = ParticleStage::Emission;
    descriptor.Validate = Validate;
    descriptor.Emit = Emit;
    BindParticleParameters<Parameters>(descriptor, kFields);
    return descriptor;
}

const ParticleProcessorDescriptor& ParticleEmitRateProcessor()
{
    return FindBuiltInParticleProcessor("emitRate");
}

} // namespace GameEngine::Particles

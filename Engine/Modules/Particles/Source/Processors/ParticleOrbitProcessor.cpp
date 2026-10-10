#include "Particles/Processors/ParticleOrbitProcessor.h"

#include "Processors/ParticleBuiltInProcessors.h"
#include "Processors/ParticleProcessorOptions.h"
#include "Processors/ParticleValueSamples.h"

#include <cmath>

GE_REFLECT(GameEngine::Particles::ParticleOrbitParameters, Axis, Input, Radial, Tangential);

namespace GameEngine::Particles
{
namespace
{
using Mathematics::Vector3;
using Parameters = ParticleOrbitParameters;

constexpr float kMinimumLength = 1e-6f;

bool UsesCurves(const void* parameters)
{
    const auto& settings = *static_cast<const Parameters*>(parameters);
    return settings.Radial.UsesCurve() || settings.Tangential.UsesCurve();
}

constexpr ParticleParameterField kFields[] = {
    {.Name = "Axis", .Label = "Axis", .Tooltip = "Axis through the emitter the particles turn around", .Kind = ParticleParameterKind::Vector3, .ComponentLabels = kParticleVectorLabels},
    {.Name = "Input", .Label = "Curve Input", .Tooltip = "What the curves are evaluated at", .Kind = ParticleParameterKind::ValueInput, .Visible = UsesCurves},
    {.Name = "Radial", .Label = "Radial", .Tooltip = "Acceleration away from the axis; negative pulls towards it", .Kind = ParticleParameterKind::Value},
    {.Name = "Tangential", .Label = "Tangential", .Tooltip = "Acceleration around the axis", .Kind = ParticleParameterKind::Value},
};

void Validate(const ParticleValidationContext& context)
{
    if (static_cast<const Parameters*>(context.Parameters)->Axis.Length() < kMinimumLength)
        context.Error("The orbit needs an axis");
}

void Execute(ParticleProcessorContext& context)
{
    const auto& parameters = context.Params<Parameters>();
    const ParticleValue values[] = {parameters.Radial, parameters.Tangential};
    const auto samples = SampleParticleValues(context, parameters.Input, values);
    const auto positions = context.Channels.Positions();
    auto velocities = context.Channels.Velocities();
    const auto& frame = context.Emitter;
    const float dt = context.DeltaTime;
    // The axis and the emitter's position once per batch, in simulation space.
    Vector3 axis = frame.LocalToSimulationVector(parameters.Axis);
    const float length = axis.Length();
    axis = length < kMinimumLength ? Vector3{0.0f, 1.0f, 0.0f} : axis / length;
    const Vector3 center = frame.LocalSpace ? Vector3{} : frame.Origin;
    for (size_t k = 0; k < context.Particles.size(); ++k)
    {
        const uint32 i = context.Particles[k];
        const Vector3 offset = positions[i] - center;
        const Vector3 outward = offset - axis * Vector3::Dot(offset, axis);
        const float distance = outward.Length();
        if (distance < kMinimumLength)
            continue;
        const Vector3 radial = outward / distance;
        const Vector3 tangent = Vector3::Cross(axis, radial);
        velocities[i] = velocities[i] + (radial * samples.Get(0, k) + tangent * samples.Get(1, k)) * dt;
    }
}
} // namespace

ParticleProcessorDescriptor MakeOrbitProcessorDescriptor()
{
    ParticleProcessorDescriptor descriptor;
    descriptor.Id = "orbit";
    descriptor.DisplayName = "Orbit";
    descriptor.Description = "Accelerates particles away from and around an axis through the emitter";
    descriptor.IconClass = "particle-choice-loop";
    descriptor.Category = "Forces";
    descriptor.Stages = StageBit(ParticleStage::Update);
    descriptor.DefaultStage = ParticleStage::Update;
    descriptor.Reads = ChannelBits(ParticleChannel::Position, ParticleChannel::Velocity);
    descriptor.Writes = ChannelBit(ParticleChannel::Velocity);
    descriptor.Validate = Validate;
    descriptor.Execute = Execute;
    BindParticleParameters<Parameters>(descriptor, kFields);
    return descriptor;
}

const ParticleProcessorDescriptor& ParticleOrbitProcessor()
{
    return FindBuiltInParticleProcessor("orbit");
}

} // namespace GameEngine::Particles

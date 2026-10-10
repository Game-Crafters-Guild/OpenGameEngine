#include "Particles/Processors/ParticleAccelerationProcessor.h"

#include "Processors/ParticleBuiltInProcessors.h"
#include "Processors/ParticleProcessorOptions.h"
#include "Processors/ParticleValueSamples.h"

GE_REFLECT(GameEngine::Particles::ParticleAccelerationParameters, Space, Input, Acceleration);

namespace GameEngine::Particles
{
namespace
{
using Mathematics::Vector3;
using Parameters = ParticleAccelerationParameters;

bool UsesCurves(const void* parameters)
{
    for (const auto& value : static_cast<const Parameters*>(parameters)->Acceleration)
        if (value.UsesCurve())
            return true;
    return false;
}

constexpr ParticleParameterField kFields[] = {
    {.Name = "Space", .Label = "Space", .Tooltip = "World keeps gravity pointing down however the emitter turns", .Kind = ParticleParameterKind::Enum, .Options = kParticleSpaceOptions},
    {.Name = "Input", .Label = "Curve Input", .Tooltip = "What the curves are evaluated at", .Kind = ParticleParameterKind::ValueInput, .Visible = UsesCurves},
    {.Name = "Acceleration", .Label = "Acceleration", .Tooltip = "Units per second squared", .Kind = ParticleParameterKind::Value, .ComponentLabels = kParticleVectorLabels},
};

Vector3 ToSimulation(const ParticleEmitterFrame& frame, ParticleSpace space, const Vector3& vector)
{
    if (space == ParticleSpace::World)
        return frame.WorldToSimulationVector(vector);
    if (space == ParticleSpace::Local)
        return frame.LocalToSimulationVector(vector);
    return vector;
}

void Execute(ParticleProcessorContext& context)
{
    const auto& parameters = context.Params<Parameters>();
    const auto samples = SampleParticleValues(context, parameters.Input, parameters.Acceleration);
    auto velocities = context.Channels.Velocities();
    const float dt = context.DeltaTime;
    if (samples.IsUniform[0] && samples.IsUniform[1] && samples.IsUniform[2])
    {
        // The common case (gravity, constant wind): one transformed vector for the whole batch.
        const Vector3 delta = ToSimulation(context.Emitter, parameters.Space,
                                           {samples.Uniform[0], samples.Uniform[1], samples.Uniform[2]}) *
                              dt;
        for (const uint32 i : context.Particles)
            velocities[i] = velocities[i] + delta;
        return;
    }
    for (size_t k = 0; k < context.Particles.size(); ++k)
    {
        const Vector3 acceleration{samples.Get(0, k), samples.Get(1, k), samples.Get(2, k)};
        const uint32 i = context.Particles[k];
        velocities[i] = velocities[i] + ToSimulation(context.Emitter, parameters.Space, acceleration) * dt;
    }
}
} // namespace

ParticleProcessorDescriptor MakeAccelerationProcessorDescriptor()
{
    ParticleProcessorDescriptor descriptor;
    descriptor.Id = "acceleration";
    descriptor.DisplayName = "Acceleration";
    descriptor.Description = "Accelerates particles: gravity, wind or thrust";
    descriptor.IconClass = "particle-choice-move";
    descriptor.Category = "Forces";
    descriptor.Stages = StageBit(ParticleStage::Update);
    descriptor.DefaultStage = ParticleStage::Update;
    descriptor.Reads = ChannelBit(ParticleChannel::Velocity);
    descriptor.Writes = ChannelBit(ParticleChannel::Velocity);
    descriptor.Execute = Execute;
    BindParticleParameters<Parameters>(descriptor, kFields);
    return descriptor;
}

const ParticleProcessorDescriptor& ParticleAccelerationProcessor()
{
    return FindBuiltInParticleProcessor("acceleration");
}

} // namespace GameEngine::Particles

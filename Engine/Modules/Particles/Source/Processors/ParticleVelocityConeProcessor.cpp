#include "Particles/Processors/ParticleVelocityConeProcessor.h"

#include "Particles/ParticleStackDocument.h"
#include "Processors/ParticleBuiltInProcessors.h"
#include "Processors/ParticleProcessorOptions.h"
#include "Processors/ParticleValueSamples.h"

#include <algorithm>
#include <cmath>

GE_REFLECT(GameEngine::Particles::ParticleVelocityConeParameters, Space, Direction, Spread, Input, Speed,
           InheritEmitterVelocity);

namespace GameEngine::Particles
{
namespace
{
using Mathematics::Vector3;
using Parameters = ParticleVelocityConeParameters;

constexpr float kPi = 3.14159265358979323846f;
constexpr float kMinimumDirectionLength = 1e-6f;
// Random streams of the direction; the speed uses stream 1 through SampleParticleValues.
constexpr uint32 kPolarStream = 8;
constexpr uint32 kAzimuthStream = 9;

bool UsesCurves(const void* parameters)
{
    return static_cast<const Parameters*>(parameters)->Speed.UsesCurve();
}

constexpr ParticleParameterField kFields[] = {
    {.Name = "Space", .Label = "Space", .Tooltip = "Local turns the cone with the emitter; World keeps it fixed", .Kind = ParticleParameterKind::Enum, .Options = kParticleSpaceOptions},
    {.Name = "Direction", .Label = "Direction", .Tooltip = "Axis of the cone", .Kind = ParticleParameterKind::Vector3, .ComponentLabels = kParticleVectorLabels},
    {.Name = "Spread", .Label = "Spread", .Tooltip = "Half angle of the cone in degrees; 180 launches in every direction", .Kind = ParticleParameterKind::Float, .Minimum = 0.0f, .Maximum = 180.0f},
    {.Name = "Input", .Label = "Curve Input", .Tooltip = "What the curve is evaluated at", .Kind = ParticleParameterKind::ValueInput, .Visible = UsesCurves},
    {.Name = "Speed", .Label = "Speed", .Tooltip = "Units per second", .Kind = ParticleParameterKind::Value, .Minimum = 0.0f},
    {.Name = "InheritEmitterVelocity", .Label = "Inherit Emitter Velocity", .Tooltip = "Fraction of the emitter's velocity each particle starts with", .Kind = ParticleParameterKind::Float},
};

void Validate(const ParticleValidationContext& context)
{
    const auto& parameters = *static_cast<const Parameters*>(context.Parameters);
    if (parameters.Direction.Length() < kMinimumDirectionLength)
        context.Error("The cone needs a direction");
}

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
    const auto samples = SampleParticleValues(context, parameters.Input, std::span<const ParticleValue>(&parameters.Speed, 1));
    auto velocities = context.Channels.Velocities();
    const auto spawns = context.Channels.SpawnIndices();
    const auto& frame = context.Emitter;
    // The cone's frame once per batch: its axis in simulation space and two axes across it.
    Vector3 axis = ToSimulation(frame, parameters.Space, parameters.Direction);
    const float length = axis.Length();
    axis = length < kMinimumDirectionLength ? Vector3{0.0f, 1.0f, 0.0f} : axis / length;
    const Vector3 helper = std::fabs(axis.y) < 0.99f ? Vector3{0.0f, 1.0f, 0.0f} : Vector3{1.0f, 0.0f, 0.0f};
    const Vector3 across = Vector3::Cross(helper, axis).Normalize();
    const Vector3 up = Vector3::Cross(axis, across);
    const float minimumCosine = std::cos(std::clamp(parameters.Spread, 0.0f, 180.0f) * kPi / 180.0f);
    const Vector3 inherited = frame.WorldToSimulationVector(frame.Velocity) * parameters.InheritEmitterVelocity;
    for (size_t k = 0; k < context.Particles.size(); ++k)
    {
        const uint32 i = context.Particles[k];
        // Uniform over the cap: the cosine of the polar angle is uniform in [cos(spread), 1].
        const float cosine = 1.0f - ParticleRandom(frame.Seed, spawns[i], context.ProcessorId, kPolarStream) *
                                        (1.0f - minimumCosine);
        const float sine = std::sqrt(std::max(0.0f, 1.0f - cosine * cosine));
        const float azimuth = 2.0f * kPi * ParticleRandom(frame.Seed, spawns[i], context.ProcessorId, kAzimuthStream);
        const Vector3 direction = axis * cosine + (across * std::cos(azimuth) + up * std::sin(azimuth)) * sine;
        velocities[i] = direction * std::max(0.0f, samples.Get(0, k)) + inherited;
    }
}
} // namespace

ParticleProcessorDescriptor MakeVelocityConeProcessorDescriptor()
{
    ParticleProcessorDescriptor descriptor;
    descriptor.Id = "velocityCone";
    descriptor.DisplayName = "Velocity Cone";
    descriptor.Description = "Launches particles in random directions within a cone";
    descriptor.IconClass = "particle-choice-move";
    descriptor.Category = "Spawn";
    descriptor.Stages = StageBit(ParticleStage::Birth) | StageBit(ParticleStage::Enter);
    descriptor.DefaultStage = ParticleStage::Birth;
    descriptor.Reads = ChannelBit(ParticleChannel::SpawnIndex);
    descriptor.Writes = ChannelBit(ParticleChannel::Velocity);
    descriptor.Validate = Validate;
    descriptor.Execute = Execute;
    BindParticleParameters<Parameters>(descriptor, kFields);
    return descriptor;
}

const ParticleProcessorDescriptor& ParticleVelocityConeProcessor()
{
    return FindBuiltInParticleProcessor("velocityCone");
}

} // namespace GameEngine::Particles

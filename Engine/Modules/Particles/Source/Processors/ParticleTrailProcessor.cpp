#include "Particles/Processors/ParticleTrailProcessor.h"

#include "Particles/ParticleTrailStore.h"
#include "Processors/ParticleBuiltInProcessors.h"

GE_REFLECT(GameEngine::Particles::ParticleTrailParameters, Lifetime, Points, Spacing, DieWithParticle);

namespace GameEngine::Particles
{
namespace
{
using Mathematics::Vector3;
using Parameters = ParticleTrailParameters;

constexpr float kMaximumLifetime = 60.0f;

constexpr ParticleParameterField kFields[] = {
    {.Name = "Lifetime", .Label = "Trail Lifetime", .Tooltip = "Seconds a recorded point stays in the trail", .Kind = ParticleParameterKind::Float, .Minimum = 0.0f, .Maximum = kMaximumLifetime},
    {.Name = "Points", .Label = "Points", .Tooltip = "Most points one trail keeps", .Kind = ParticleParameterKind::UInt, .Minimum = 2.0f, .Maximum = static_cast<float>(kMaxTrailPoints)},
    {.Name = "Spacing", .Label = "Point Spacing", .Tooltip = "Distance the particle travels before the moving head becomes a kept point", .Kind = ParticleParameterKind::Float, .Minimum = 0.0f},
    {.Name = "DieWithParticle", .Label = "Remove with Particle", .Tooltip = "Remove the trail when its particle dies instead of letting it fade out", .Kind = ParticleParameterKind::Bool},
};

void Execute(ParticleProcessorContext& context)
{
    const auto& parameters = context.Params<Parameters>();
    auto& trails = *context.Trails;
    const auto positions = context.Channels.Positions();
    const auto previous = context.Channels.Get<Vector3>(ParticleChannel::PreviousPosition);
    const auto sizes = context.Channels.Sizes();
    const auto colors = context.Channels.Colors();
    const auto spawns = context.Channels.SpawnIndices();
    auto slots = context.Channels.Get<uint32>(ParticleChannel::TrailSlot);
    const auto& frame = context.Emitter;
    const double end = frame.Elapsed + context.DeltaTime;
    for (const uint32 i : context.Particles)
    {
        ParticleTrailPoint point;
        point.Size = sizes[i];
        point.Color = colors[i];
        if (slots[i] == ParticleTrailStore::kNoTrail)
        {
            slots[i] = trails.Acquire(spawns[i], parameters.Lifetime, parameters.DieWithParticle);
            // A new trail starts where the particle was at the start of the tick.
            point.Position = frame.SimulationToWorldPoint(previous[i]);
            point.Time = frame.Elapsed;
            trails.Record(slots[i], point, 0.0f);
        }
        point.Position = frame.SimulationToWorldPoint(positions[i]);
        point.Time = end;
        trails.Record(slots[i], point, parameters.Spacing);
    }
}
} // namespace

ParticleProcessorDescriptor MakeTrailProcessorDescriptor()
{
    ParticleProcessorDescriptor descriptor;
    descriptor.Id = "trail";
    descriptor.DisplayName = "Trail";
    descriptor.Description = "Records each particle's recent path for ribbon rendering";
    descriptor.IconClass = "particle-choice-move";
    descriptor.Category = "Rendering";
    descriptor.Role = ParticleProcessorRole::Trail;
    descriptor.Stages = StageBit(ParticleStage::Update);
    descriptor.DefaultStage = ParticleStage::Update;
    descriptor.Order = ParticleUpdateOrder::AfterIntegration;
    descriptor.Reads = ChannelBits(ParticleChannel::Position, ParticleChannel::PreviousPosition,
                                   ParticleChannel::Size, ParticleChannel::Color, ParticleChannel::SpawnIndex);
    descriptor.Writes = ChannelBit(ParticleChannel::TrailSlot);
    descriptor.Execute = Execute;
    BindParticleParameters<Parameters>(descriptor, kFields);
    return descriptor;
}

const ParticleProcessorDescriptor& ParticleTrailProcessor()
{
    return FindBuiltInParticleProcessor("trail");
}

} // namespace GameEngine::Particles

#include "Particles/Processors/ParticleCollisionProcessor.h"

#include "Particles/ParticleRuntime.h"
#include "Processors/ParticleBuiltInProcessors.h"
#include "Processors/ParticleProcessorOptions.h"

#include <algorithm>
#include <array>
#include <cmath>

GE_REFLECT(GameEngine::Particles::ParticleCollisionParameters, Source, PlaneNormal, PlaneOffset, Radius, Bounce,
           Damping, KillOnContact, LayerMask);

namespace GameEngine::Particles
{
namespace
{
using Mathematics::Vector3;
using Parameters = ParticleCollisionParameters;

constexpr float kMinimumNormalLength = 1e-6f;

constexpr std::array<ParticleEnumOption, 2> kSources = {{
    {"plane", "Plane", static_cast<uint32>(ParticleCollisionSource::Plane), "particle-choice-collision"},
    {"physics", "Physics World", static_cast<uint32>(ParticleCollisionSource::Physics), "particle-choice-collision"},
}};

bool UsesPlane(const void* parameters)
{
    return static_cast<const Parameters*>(parameters)->Source == ParticleCollisionSource::Plane;
}

bool UsesPhysics(const void* parameters)
{
    return !UsesPlane(parameters);
}

constexpr ParticleParameterField kFields[] = {
    {.Name = "Source", .Label = "Collide With", .Tooltip = "An infinite plane, or everything in the physics world", .Kind = ParticleParameterKind::Enum, .Options = kSources},
    {.Name = "PlaneNormal", .Label = "Plane Normal", .Tooltip = "World-space direction the plane faces", .Kind = ParticleParameterKind::Vector3, .ComponentLabels = kParticleVectorLabels, .Visible = UsesPlane},
    {.Name = "PlaneOffset", .Label = "Plane Offset", .Tooltip = "Distance of the plane from the world origin along its normal", .Kind = ParticleParameterKind::Float, .Visible = UsesPlane},
    {.Name = "Radius", .Label = "Radius", .Tooltip = "Contact radius as a multiple of particle size", .Kind = ParticleParameterKind::Float, .Minimum = 0.0f},
    {.Name = "Bounce", .Label = "Bounce", .Tooltip = "Fraction of the approach speed kept after contact", .Kind = ParticleParameterKind::Float, .Minimum = 0.0f, .Maximum = 1.0f},
    {.Name = "Damping", .Label = "Damping", .Tooltip = "Fraction of the velocity lost at contact; with no bounce, 1 settles the particle", .Kind = ParticleParameterKind::Float, .Minimum = 0.0f, .Maximum = 1.0f},
    {.Name = "KillOnContact", .Label = "Kill on Contact", .Tooltip = "Kill the particle at its first contact", .Kind = ParticleParameterKind::Bool},
    {.Name = "LayerMask", .Label = "Layer Mask", .Tooltip = "Physics layers the particles collide with", .Kind = ParticleParameterKind::UInt, .Visible = UsesPhysics},
};

void Validate(const ParticleValidationContext& context)
{
    const auto& parameters = *static_cast<const Parameters*>(context.Parameters);
    if (parameters.Source == ParticleCollisionSource::Plane && parameters.PlaneNormal.Length() < kMinimumNormalLength)
        context.Error("The collision plane needs a normal");
}

void Execute(ParticleProcessorContext& context)
{
    const auto& parameters = context.Params<Parameters>();
    if (parameters.Source == ParticleCollisionSource::Physics && (!context.Collision || !context.Collision->Sweep))
        return;
    auto positions = context.Channels.Positions();
    auto velocities = context.Channels.Velocities();
    const auto previous = context.Channels.Get<Vector3>(ParticleChannel::PreviousPosition);
    const auto sizes = context.Channels.Sizes();
    auto contacts = context.Channels.Get<uint64>(ParticleChannel::Contacts);
    auto settled = context.Channels.Get<uint32>(ParticleChannel::Settled);
    const uint64 contactBit = uint64{1} << context.ProcessorOrdinal;
    const Vector3 planeNormal = parameters.PlaneNormal.Length() < kMinimumNormalLength ? Vector3{0, 1, 0} : parameters.PlaneNormal.Normalize();
    const float bounce = std::clamp(parameters.Bounce, 0.0f, 1.0f);
    const float keep = 1.0f - std::clamp(parameters.Damping, 0.0f, 1.0f);
    const bool settles = parameters.Damping >= 1.0f && bounce <= 0.0f;
    const auto& frame = context.Emitter;
    for (const uint32 i : context.Particles)
    {
        if (settled[i])
            continue;
        const Vector3 from = frame.SimulationToWorldPoint(previous[i]);
        const Vector3 to = frame.SimulationToWorldPoint(positions[i]);
        const float radius = std::max(0.0f, parameters.Radius) * sizes[i];
        CollisionHit hit;
        bool contact = false;
        if (parameters.Source == ParticleCollisionSource::Physics)
            contact = context.Collision->Sweep(context.Collision->Context, from, to, radius, parameters.LayerMask, hit);
        else
        {
            const float distance = Vector3::Dot(to, planeNormal) - parameters.PlaneOffset;
            contact = distance <= radius;
            hit.Normal = planeNormal;
            hit.Position = to - planeNormal * distance;
        }
        if (!contact)
        {
            contacts[i] &= ~contactBit;
            continue;
        }
        const bool firstContact = (contacts[i] & contactBit) == 0;
        contacts[i] |= contactBit;
        if (firstContact && !context.Events->RaiseCollision(i, hit, parameters.KillOnContact))
            continue;
        if (parameters.KillOnContact)
        {
            context.Events->Kill(i, &hit);
            continue;
        }
        Vector3 velocity = frame.SimulationToWorldVector(velocities[i]) * keep;
        const float approach = Vector3::Dot(velocity, hit.Normal);
        if (approach < 0.0f)
            velocity = velocity - hit.Normal * ((1.0f + bounce) * approach);
        positions[i] = frame.WorldToSimulationPoint(hit.Position + hit.Normal * radius);
        velocities[i] = settles ? Vector3{} : frame.WorldToSimulationVector(velocity);
        if (settles)
            settled[i] = 1;
    }
}
} // namespace

ParticleProcessorDescriptor MakeCollisionProcessorDescriptor()
{
    ParticleProcessorDescriptor descriptor;
    descriptor.Id = "collision";
    descriptor.DisplayName = "Collision";
    descriptor.Description = "Bounces particles off a plane or the physics world and raises Collision events";
    descriptor.IconClass = "particle-choice-collision";
    descriptor.Category = "Forces";
    descriptor.Stages = StageBit(ParticleStage::Update);
    descriptor.DefaultStage = ParticleStage::Update;
    descriptor.Order = ParticleUpdateOrder::AfterIntegration;
    descriptor.Reads = ChannelBits(ParticleChannel::Position, ParticleChannel::PreviousPosition,
                                   ParticleChannel::Velocity, ParticleChannel::Size);
    descriptor.Writes = ChannelBits(ParticleChannel::Position, ParticleChannel::Velocity, ParticleChannel::Contacts,
                                    ParticleChannel::Settled);
    descriptor.Validate = Validate;
    descriptor.Execute = Execute;
    BindParticleParameters<Parameters>(descriptor, kFields);
    return descriptor;
}

const ParticleProcessorDescriptor& ParticleCollisionProcessor()
{
    return FindBuiltInParticleProcessor("collision");
}

} // namespace GameEngine::Particles

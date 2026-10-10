#include "Particles/Processors/ParticlePropertyProcessor.h"

#include "Processors/ParticleBuiltInProcessors.h"
#include "Processors/ParticleProcessorOptions.h"
#include "Processors/ParticleValueSamples.h"

#include <algorithm>
#include <array>
#include <limits>
#include <span>

GE_REFLECT(GameEngine::Particles::ParticlePropertyParameters, Target, Operation, Basis, Space, Input, Value);

namespace GameEngine::Particles
{
namespace
{
using Mathematics::Vector3;
using Mathematics::Vector4;
using Parameters = ParticlePropertyParameters;

constexpr float kMaximumMagnitude = 10000.0f;

bool TargetsVector(const void* parameters)
{
    const auto target = static_cast<const Parameters*>(parameters)->Target;
    return target == ParticleAttribute::Position || target == ParticleAttribute::Velocity;
}

bool UsesCurves(const void* parameters)
{
    const auto& property = *static_cast<const Parameters*>(parameters);
    const uint32 components = AttributeComponents(property.Target);
    for (uint32 component = 0; component < components; ++component)
        if (property.Value[component].UsesCurve())
            return true;
    return false;
}

constexpr std::array<std::string_view, 1> kScalarLabel = {""};

std::span<const std::string_view> ValueLabels(const void* parameters)
{
    switch (static_cast<const Parameters*>(parameters)->Target)
    {
    case ParticleAttribute::Position:
    case ParticleAttribute::Velocity:
    case ParticleAttribute::Scale:
        return kParticleVectorLabels;
    case ParticleAttribute::Color:
        return kParticleColorLabels;
    default:
        return kScalarLabel;
    }
}

bool TargetsColor(const void* parameters)
{
    return static_cast<const Parameters*>(parameters)->Target == ParticleAttribute::Color;
}

void Validate(const ParticleValidationContext& context)
{
    const auto& property = *static_cast<const Parameters*>(context.Parameters);
    if (property.Target == ParticleAttribute::Lifetime && context.Stage == ParticleStage::Update)
        context.Error("Lifetime is set at birth, phase entry or exit, not every update");
}

constexpr ParticleParameterField kFields[] = {
    {.Name = "Target", .Label = "Attribute", .Tooltip = "The particle attribute this processor writes", .Kind = ParticleParameterKind::Enum, .Options = kParticleAttributeOptions},
    {.Name = "Operation", .Label = "Operation", .Tooltip = "How the value combines with the basis", .Kind = ParticleParameterKind::Enum, .Options = kParticleOperationOptions},
    {.Name = "Basis", .Label = "Basis", .Tooltip = "The attribute value the operation starts from; birth and phase-entry values do not compound", .Kind = ParticleParameterKind::Enum, .Options = kParticleBasisOptions},
    {.Name = "Space", .Label = "Space", .Tooltip = "The space position and velocity values are authored in", .Kind = ParticleParameterKind::Enum, .Options = kParticleSpaceOptions, .Visible = TargetsVector},
    {.Name = "Input", .Label = "Curve Input", .Tooltip = "What the curves are evaluated at", .Kind = ParticleParameterKind::ValueInput, .Visible = UsesCurves},
    {.Name = "Value", .Label = "Value", .Tooltip = "The value per component of the attribute", .Kind = ParticleParameterKind::Value, .ComponentLabelsFor = ValueLabels, .IsColor = TargetsColor},
};

float Combine(ParticleOperation operation, float base, float value)
{
    switch (operation)
    {
    case ParticleOperation::Set:
        return value;
    case ParticleOperation::Add:
        return base + value;
    case ParticleOperation::Multiply:
    default:
        return base * value;
    }
}

ParticleChannelMask Channels(const void* parameters)
{
    const auto& property = *static_cast<const Parameters*>(parameters);
    ParticleChannelMask mask = ChannelBit(AttributeChannel(property.Target, ParticleBasis::Current));
    if (property.Operation != ParticleOperation::Set)
        mask |= ChannelBit(AttributeChannel(property.Target, property.Basis));
    if (property.Target == ParticleAttribute::Lifetime)
        mask |= ChannelBit(ParticleChannel::Age);
    return mask;
}

// The basis a non-Set operation starts from. Set reads none, and declares none (Channels), so the
// basis channel is only fetched when the operation reads it.
template <typename T>
std::span<const T> BasisOf(ParticleProcessorContext& context, const Parameters& property)
{
    if (property.Operation == ParticleOperation::Set)
        return {};
    return context.Channels.Get<T>(AttributeChannel(property.Target, property.Basis));
}

// Position and velocity are combined in the space they are authored in: the basis is taken into
// that space, combined component by component, and the result returned to simulation space.
Vector3 ToAuthoredSpace(const ParticleEmitterFrame& frame, ParticleSpace space, bool point, const Vector3& value)
{
    if (space == ParticleSpace::World && frame.LocalSpace)
        return point ? frame.LocalToWorldPoint(value) : frame.LocalToWorldVector(value);
    if (space == ParticleSpace::Local && !frame.LocalSpace)
        return point ? frame.WorldToLocalPoint(value) : frame.WorldToLocalVector(value);
    return value;
}

Vector3 FromAuthoredSpace(const ParticleEmitterFrame& frame, ParticleSpace space, bool point, const Vector3& value)
{
    if (space == ParticleSpace::World && frame.LocalSpace)
        return point ? frame.WorldToLocalPoint(value) : frame.WorldToLocalVector(value);
    if (space == ParticleSpace::Local && !frame.LocalSpace)
        return point ? frame.LocalToWorldPoint(value) : frame.LocalToWorldVector(value);
    return value;
}

void ExecuteVector(ParticleProcessorContext& context, const Parameters& property, const ParticleValueSamples& samples)
{
    const bool point = property.Target == ParticleAttribute::Position;
    const auto current = AttributeChannel(property.Target, ParticleBasis::Current);
    auto out = context.Channels.Get<Vector3>(current);
    const auto base = BasisOf<Vector3>(context, property);
    const bool convert = (property.Space == ParticleSpace::World && context.Emitter.LocalSpace) ||
                         (property.Space == ParticleSpace::Local && !context.Emitter.LocalSpace);
    for (size_t k = 0; k < context.Particles.size(); ++k)
    {
        const uint32 i = context.Particles[k];
        Vector3 authored = property.Operation == ParticleOperation::Set ? Vector3{} : base[i];
        if (convert)
            authored = ToAuthoredSpace(context.Emitter, property.Space, point, authored);
        for (uint32 component = 0; component < 3; ++component)
            authored[component] = Combine(property.Operation, authored[component], samples.Get(component, k));
        out[i] = convert ? FromAuthoredSpace(context.Emitter, property.Space, point, authored) : authored;
    }
}

void ExecuteScalar(ParticleProcessorContext& context, const Parameters& property, const ParticleValueSamples& samples,
                   float minimum, float maximum)
{
    auto out = context.Channels.Get<float>(AttributeChannel(property.Target, ParticleBasis::Current));
    const auto base = BasisOf<float>(context, property);
    const bool lifetime = property.Target == ParticleAttribute::Lifetime;
    const auto ages = context.Channels.Ages();
    for (size_t k = 0; k < context.Particles.size(); ++k)
    {
        const uint32 i = context.Particles[k];
        float value = std::clamp(Combine(property.Operation, property.Operation == ParticleOperation::Set ? 0.0f : base[i],
                                         samples.Get(0, k)),
                                 minimum, maximum);
        // A lifetime shorter than the age already lived ends the particle this tick, not in the past.
        if (lifetime)
            value = std::max(ages[i] + 0.001f, value);
        out[i] = value;
    }
}

void Execute(ParticleProcessorContext& context)
{
    const auto& property = context.Params<Parameters>();
    const uint32 components = AttributeComponents(property.Target);
    const auto samples = SampleParticleValues(context, property.Input, std::span<const ParticleValue>(property.Value, components));
    switch (property.Target)
    {
    case ParticleAttribute::Position:
    case ParticleAttribute::Velocity:
        ExecuteVector(context, property, samples);
        break;
    case ParticleAttribute::Scale:
    {
        auto out = context.Channels.Get<Vector3>(ParticleChannel::Scale);
        const auto base = BasisOf<Vector3>(context, property);
        for (size_t k = 0; k < context.Particles.size(); ++k)
        {
            const uint32 i = context.Particles[k];
            for (uint32 component = 0; component < 3; ++component)
                out[i][component] = std::clamp(
                    Combine(property.Operation, property.Operation == ParticleOperation::Set ? 0.0f : base[i][component],
                            samples.Get(component, k)),
                    0.0f, kMaximumMagnitude);
        }
        break;
    }
    case ParticleAttribute::Color:
    case ParticleAttribute::Alpha:
    {
        auto out = context.Channels.Colors();
        const auto base = BasisOf<Vector4>(context, property);
        const bool alphaOnly = property.Target == ParticleAttribute::Alpha;
        for (size_t k = 0; k < context.Particles.size(); ++k)
        {
            const uint32 i = context.Particles[k];
            const Vector4 from = property.Operation == ParticleOperation::Set ? Vector4{} : base[i];
            if (alphaOnly)
                out[i].w = Combine(property.Operation, from.w, samples.Get(0, k));
            else
                for (uint32 component = 0; component < 4; ++component)
                    out[i][component] = Combine(property.Operation, from[component], samples.Get(component, k));
            out[i].w = std::clamp(out[i].w, 0.0f, 1.0f);
        }
        break;
    }
    case ParticleAttribute::Size:
        ExecuteScalar(context, property, samples, 0.0f, kMaximumMagnitude);
        break;
    default:
        ExecuteScalar(context, property, samples, std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max());
        break;
    }
}
} // namespace

ParticleProcessorDescriptor MakePropertyProcessorDescriptor()
{
    ParticleProcessorDescriptor descriptor;
    descriptor.Id = "property";
    descriptor.DisplayName = "Property";
    descriptor.Description = "Sets, adds to or multiplies a particle attribute by a constant, a random range or a curve";
    descriptor.IconClass = "particle-choice-curve";
    descriptor.Category = "Properties";
    descriptor.Stages = StageBit(ParticleStage::Birth) | StageBit(ParticleStage::Enter) |
                        StageBit(ParticleStage::Update) | StageBit(ParticleStage::Exit);
    descriptor.DefaultStage = ParticleStage::Update;
    descriptor.ParameterChannels = Channels;
    descriptor.Validate = Validate;
    descriptor.Execute = Execute;
    BindParticleParameters<Parameters>(descriptor, kFields);
    return descriptor;
}

const ParticleProcessorDescriptor& ParticlePropertyProcessor()
{
    return FindBuiltInParticleProcessor("property");
}

} // namespace GameEngine::Particles

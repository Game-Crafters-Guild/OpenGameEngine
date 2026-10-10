#include "Particles/Processors/ParticleEventProcessor.h"

#include "Components/Rendering/Particles.h"
#include "Particles/ParticleStackDocument.h"
#include "Processors/ParticleBuiltInProcessors.h"

#include <array>

GE_REFLECT(GameEngine::Particles::ParticleEventParameters, Trigger, Action, Seconds, Spacing, Probability, EventName,
           Destination, SubEmitter, Count, Inherit, VelocityScale, AlignToNormal, KillSource);

namespace GameEngine::Particles
{
namespace
{
using Parameters = ParticleEventParameters;

constexpr float kMinimumSpacing = 0.0001f;
constexpr float kMaximumCount = 4096.0f;

constexpr std::array<ParticleEnumOption, 10> kTriggers = {{
    {"birth", "On Birth", static_cast<uint32>(ParticleEventTrigger::Birth), "particle-choice-emission"},
    {"death", "On Death", static_cast<uint32>(ParticleEventTrigger::Death), "particle-choice-time"},
    {"collision", "On Collision", static_cast<uint32>(ParticleEventTrigger::Collision), "particle-choice-collision"},
    {"age", "At Age", static_cast<uint32>(ParticleEventTrigger::Age), "particle-choice-time"},
    {"phaseAge", "At Phase Age", static_cast<uint32>(ParticleEventTrigger::PhaseAge), "particle-choice-time"},
    {"enter", "On Phase Enter", static_cast<uint32>(ParticleEventTrigger::Enter), "particle-choice-emission"},
    {"exit", "On Phase Exit", static_cast<uint32>(ParticleEventTrigger::Exit), "particle-choice-emission"},
    {"external", "On Game Event", static_cast<uint32>(ParticleEventTrigger::External), "particle-choice-custom"},
    {"distance", "Every Distance", static_cast<uint32>(ParticleEventTrigger::Distance), "particle-choice-move"},
    {"interval", "Every Interval", static_cast<uint32>(ParticleEventTrigger::Interval), "particle-choice-loop"},
}};

constexpr std::array<ParticleEnumOption, 3> kActions = {{
    {"transition", "Move to Phase", static_cast<uint32>(ParticleEventAction::Transition), "particle-choice-loop"},
    {"emit", "Spawn in Child Emitter", static_cast<uint32>(ParticleEventAction::Emit), "particle-choice-emission"},
    {"kill", "Kill", static_cast<uint32>(ParticleEventAction::Kill), "particle-choice-time"},
}};

constexpr std::array<ParticleEnumOption, Components::ParticleMaxSubEmitters> kSubEmitters = {{
    {"subEmitter1", "Sub-emitter 1", 0},
    {"subEmitter2", "Sub-emitter 2", 1},
    {"subEmitter3", "Sub-emitter 3", 2},
    {"subEmitter4", "Sub-emitter 4", 3},
}};

constexpr std::array<ParticleEnumOption, 5> kInherits = {{
    {"velocity", "Velocity", kParticleInheritVelocity},
    {"size", "Size", kParticleInheritSize},
    {"color", "Color", kParticleInheritColor},
    {"rotation", "Rotation", kParticleInheritRotation},
    {"lifetime", "Lifetime", kParticleInheritLifetime},
}};

const Parameters& Params(const void* parameters)
{
    return *static_cast<const Parameters*>(parameters);
}

bool UsesSeconds(const void* parameters)
{
    const auto trigger = Params(parameters).Trigger;
    return trigger == ParticleEventTrigger::Age || trigger == ParticleEventTrigger::PhaseAge ||
           trigger == ParticleEventTrigger::Interval;
}

bool UsesSpacing(const void* parameters)
{
    return Params(parameters).Trigger == ParticleEventTrigger::Distance;
}

bool UsesEventName(const void* parameters)
{
    return Params(parameters).Trigger == ParticleEventTrigger::External;
}

bool Transitions(const void* parameters)
{
    return Params(parameters).Action == ParticleEventAction::Transition;
}

bool Emits(const void* parameters)
{
    return Params(parameters).Action == ParticleEventAction::Emit;
}

bool EmitsInheritingVelocity(const void* parameters)
{
    return Emits(parameters) && (Params(parameters).Inherit & kParticleInheritVelocity) != 0;
}

constexpr ParticleParameterField kFields[] = {
    {.Name = "Trigger", .Label = "When", .Kind = ParticleParameterKind::Enum, .Options = kTriggers},
    {.Name = "Action", .Label = "Do", .Kind = ParticleParameterKind::Enum, .Options = kActions},
    {.Name = "Seconds", .Label = "Seconds", .Tooltip = "Age, phase age or interval in seconds", .Kind = ParticleParameterKind::Float, .Minimum = 0.0f, .Visible = UsesSeconds},
    {.Name = "Spacing", .Label = "Spacing", .Tooltip = "Distance travelled between events", .Kind = ParticleParameterKind::Float, .Minimum = kMinimumSpacing, .Visible = UsesSpacing},
    {.Name = "Probability", .Label = "Probability", .Tooltip = "Chance the action runs each time the rule fires", .Kind = ParticleParameterKind::Float, .Minimum = 0.0f, .Maximum = 1.0f},
    {.Name = "EventName", .Label = "Game Event", .Tooltip = "Name the game sends to the emitter", .Kind = ParticleParameterKind::Text, .Visible = UsesEventName},
    {.Name = "Destination", .Label = "Phase", .Tooltip = "Phase the particle moves to", .Kind = ParticleParameterKind::Phase, .Visible = Transitions},
    {.Name = "SubEmitter", .Label = "Child Emitter", .Tooltip = "The emitter's sub-emitter slot the children spawn in", .Kind = ParticleParameterKind::Enum, .Options = kSubEmitters, .Visible = Emits},
    {.Name = "Count", .Label = "Count", .Tooltip = "Child particles per event", .Kind = ParticleParameterKind::UInt, .Minimum = 0.0f, .Maximum = kMaximumCount, .Visible = Emits},
    {.Name = "Inherit", .Label = "Inherit", .Tooltip = "What each child particle takes from this particle", .Kind = ParticleParameterKind::Flags, .Options = kInherits, .Visible = Emits},
    {.Name = "VelocityScale", .Label = "Velocity Scale", .Tooltip = "Multiplier on the inherited velocity", .Kind = ParticleParameterKind::Float, .Visible = EmitsInheritingVelocity},
    {.Name = "AlignToNormal", .Label = "Align to Contact Normal", .Tooltip = "Turn child velocity along the collision normal, keeping its speed", .Kind = ParticleParameterKind::Bool, .Visible = Emits},
    {.Name = "KillSource", .Label = "Kill This Particle", .Tooltip = "Kill the particle after spawning the children", .Kind = ParticleParameterKind::Bool, .Visible = Emits},
};

void Validate(const ParticleValidationContext& context)
{
    const auto& parameters = Params(context.Parameters);
    const bool transition = parameters.Action == ParticleEventAction::Transition;
    if (transition && (parameters.Trigger == ParticleEventTrigger::Exit || parameters.Trigger == ParticleEventTrigger::Death))
        context.Error("Exit and death rules cannot move the particle to another phase");
    const bool repeating = parameters.Trigger == ParticleEventTrigger::Distance || parameters.Trigger == ParticleEventTrigger::Interval;
    if (repeating && (parameters.Action != ParticleEventAction::Emit || parameters.KillSource))
        context.Error("Distance and interval rules spawn children and keep the particle alive");
    if (parameters.Trigger == ParticleEventTrigger::Interval && parameters.Seconds <= 0.0f)
        context.Error("An interval rule needs a positive interval");
    if (parameters.Action == ParticleEventAction::Emit && parameters.SubEmitter >= Components::ParticleMaxSubEmitters)
        context.Error("A spawn rule names one of the emitter's four sub-emitter slots");
    if (parameters.Trigger == ParticleEventTrigger::External && parameters.EventName[0] == '\0')
        context.Error("A game event rule needs the event name");
}
} // namespace

ParticleProcessorDescriptor MakeEventProcessorDescriptor()
{
    ParticleProcessorDescriptor descriptor;
    descriptor.Id = "event";
    descriptor.DisplayName = "Event";
    descriptor.Description = "When something happens to a particle, move it to another phase, spawn children or kill it";
    descriptor.IconClass = "particle-choice-loop";
    descriptor.Category = "Events";
    descriptor.Role = ParticleProcessorRole::Event;
    descriptor.Stages = StageBit(ParticleStage::Event);
    descriptor.DefaultStage = ParticleStage::Event;
    descriptor.Validate = Validate;
    BindParticleParameters<Parameters>(descriptor, kFields);
    return descriptor;
}

const ParticleProcessorDescriptor& ParticleEventProcessor()
{
    return FindBuiltInParticleProcessor("event");
}

} // namespace GameEngine::Particles

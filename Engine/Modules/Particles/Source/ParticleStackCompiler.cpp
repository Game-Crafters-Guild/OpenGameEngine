#include "Particles/CompiledParticleStack.h"

#include "Particles/ParticleParameterAccess.h"
#include "Particles/Processors/ParticleEventProcessor.h"
#include "Particles/Processors/ParticleTrailProcessor.h"
#include "Processors/ParticleProcessorOptions.h"

#include <algorithm>

namespace GameEngine::Particles
{
namespace
{
constexpr uint32 kMaximumRulesPerPhase = 64;

ParticleChannelMask ParameterDriverChannels(const ParticleProcessorInstance& processor)
{
    ParticleChannelMask mask = 0;
    ForEachParticleParameter(*processor.Descriptor, [&](const ParticleParameter& parameter)
                             {
        if (parameter.Kind() != ParticleParameterKind::ValueInput)
            return;
        const auto& input = *ParticleParameterPointer<ParticleValueInput>(processor.Parameters.data(), *parameter.Field);
        mask |= DriverChannels(input.Driver); });
    return mask;
}

ParticleChannelMask ProcessorChannels(const ParticleProcessorInstance& processor)
{
    const auto& descriptor = *processor.Descriptor;
    ParticleChannelMask mask = descriptor.Reads | descriptor.Writes | ParameterDriverChannels(processor);
    if (descriptor.ParameterChannels)
        mask |= descriptor.ParameterChannels(processor.Parameters.data());
    return mask;
}

CompiledProcessor Resolve(const ParticleProcessorInstance& processor, uint32 ordinal)
{
    CompiledProcessor compiled;
    compiled.Descriptor = processor.Descriptor;
    compiled.Parameters = processor.Parameters.data();
    compiled.Id = processor.Id;
    compiled.Ordinal = ordinal;
    compiled.Clock = processor.Clock;
    compiled.Start = processor.Start;
    compiled.End = processor.End.value_or(std::numeric_limits<float>::infinity());
    if (processor.Descriptor->Compile)
        compiled.Compiled = processor.Descriptor->Compile({processor.Parameters.data(), &processor.Geometry});
    return compiled;
}

std::vector<CompiledProcessor>* StageList(CompiledPhase& phase, const ParticleProcessorInstance& processor)
{
    switch (processor.Stage)
    {
    case ParticleStage::Birth:
        return &phase.Birth;
    case ParticleStage::Enter:
        return &phase.Enter;
    case ParticleStage::Update:
        return processor.Descriptor->Order == ParticleUpdateOrder::AfterIntegration ? &phase.UpdateAfterIntegration
                                                                                    : &phase.UpdateBeforeIntegration;
    case ParticleStage::Exit:
        return &phase.Exit;
    default:
        return nullptr;
    }
}
} // namespace

ParticleChannelMask DriverChannels(ParticleDriver driver)
{
    switch (driver)
    {
    case ParticleDriver::NormalizedAge:
        return ChannelBits(ParticleChannel::Age, ParticleChannel::Lifetime);
    case ParticleDriver::Age:
        return ChannelBit(ParticleChannel::Age);
    case ParticleDriver::PhaseAge:
        return ChannelBit(ParticleChannel::PhaseAge);
    case ParticleDriver::Random:
        return ChannelBit(ParticleChannel::SpawnIndex);
    case ParticleDriver::Speed:
        return ChannelBit(ParticleChannel::Velocity);
    case ParticleDriver::Size:
        return ChannelBit(ParticleChannel::Size);
    case ParticleDriver::Custom0:
        return ChannelBit(ParticleChannel::Custom0);
    case ParticleDriver::Custom1:
        return ChannelBit(ParticleChannel::Custom1);
    case ParticleDriver::Custom2:
        return ChannelBit(ParticleChannel::Custom2);
    case ParticleDriver::Custom3:
        return ChannelBit(ParticleChannel::Custom3);
    case ParticleDriver::EmitterSpeed:
    default:
        return 0;
    }
}

uint32 CompiledParticleStack::PhaseIndex(uint32 id) const
{
    for (uint32 index = 0; index < Phases.size(); ++index)
        if (Phases[index].Id == id)
            return index;
    return static_cast<uint32>(Phases.size());
}

std::shared_ptr<const CompiledParticleStack> CompileParticleStack(std::shared_ptr<const StackDocument> document,
                                                                  std::vector<StackDiagnostic>& diagnostics)
{
    if (!document || !ValidateStack(*document, diagnostics))
        return nullptr;
    auto stack = std::make_shared<CompiledParticleStack>();
    stack->Document = document;
    stack->Lifetime = document->Lifetime;
    stack->Phases.reserve(document->Phases.size());
    for (const auto& phase : document->Phases)
        stack->Phases.push_back(CompiledPhase{phase.Id});
    stack->EntryPhase = stack->PhaseIndex(document->EntryPhase);

    ParticleChannelMask channels = 0;
    for (size_t phaseIndex = 0; phaseIndex < document->Phases.size(); ++phaseIndex)
    {
        const auto& phase = document->Phases[phaseIndex];
        auto& compiledPhase = stack->Phases[phaseIndex];
        uint32 rules = 0;
        for (uint32 ordinal = 0; ordinal < phase.Processors.size(); ++ordinal)
        {
            const auto& processor = phase.Processors[ordinal];
            if (!processor.Enabled)
                continue;
            const auto& descriptor = *processor.Descriptor;
            channels |= ProcessorChannels(processor);
            switch (descriptor.Role)
            {
            case ParticleProcessorRole::Emission:
            {
                auto compiled = Resolve(processor, ordinal);
                compiled.Bounded = processor.Start > 0.0f || processor.End.has_value();
                stack->Emission.push_back(std::move(compiled));
                break;
            }
            case ParticleProcessorRole::Event:
            {
                if (rules >= kMaximumRulesPerPhase)
                {
                    diagnostics.push_back({"phase." + std::to_string(phase.Id), "A phase holds at most 64 event rules"});
                    return nullptr;
                }
                const auto& parameters = processor.Params<ParticleEventParameters>();
                CompiledEventRule rule;
                rule.Parameters = &parameters;
                rule.ProcessorId = processor.Id;
                rule.Bit = rules++;
                rule.DestinationPhase = stack->PhaseIndex(parameters.Destination);
                rule.EventName = parameters.EventName[0] ? HashStringId(parameters.EventName) : 0;
                if (parameters.Trigger == ParticleEventTrigger::Distance)
                    channels |= ChannelBits(ParticleChannel::Distance, ParticleChannel::PreviousDistance);
                compiledPhase.Rules[static_cast<uint32>(parameters.Trigger)].push_back(rule);
                break;
            }
            case ParticleProcessorRole::Trail:
            {
                const auto& trail = processor.Params<ParticleTrailParameters>();
                stack->Trail.Points = stack->HasTrails ? std::max(stack->Trail.Points, trail.Points) : trail.Points;
                stack->HasTrails = true;
                [[fallthrough]];
            }
            case ParticleProcessorRole::Particle:
            default:
            {
                auto* list = StageList(compiledPhase, processor);
                if (!list)
                    break;
                auto compiled = Resolve(processor, ordinal);
                compiled.Bounded = processor.Stage == ParticleStage::Update &&
                                   (processor.Start > 0.0f || processor.End.has_value());
                list->push_back(std::move(compiled));
                break;
            }
            }
        }
    }
    stack->Channels = channels;
    stack->HasLights = (channels & ChannelBit(ParticleChannel::LightIntensity)) != 0;
    return stack;
}

} // namespace GameEngine::Particles

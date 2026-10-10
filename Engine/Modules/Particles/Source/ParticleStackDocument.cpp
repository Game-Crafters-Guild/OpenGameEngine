#include "Particles/ParticleStackDocument.h"

#include "Components/Rendering/Particles.h"
#include "Particles/Processors/ParticleAccelerationProcessor.h"
#include "Particles/Processors/ParticleEmitRateProcessor.h"
#include "Particles/Processors/ParticleEventProcessor.h"
#include "Particles/Processors/ParticlePropertyProcessor.h"
#include "Particles/Processors/ParticleVelocityConeProcessor.h"

#include <algorithm>
#include <cstring>

namespace GameEngine::Particles
{
namespace
{
bool SameInterval(const ParticleProcessorInstance& a, const ParticleProcessorInstance& b)
{
    return a.Clock == b.Clock && a.Start == b.Start && a.End == b.End;
}

// Phases and processors share one id space; 0 when it is exhausted.
uint32 NextStackId(const StackDocument& document)
{
    uint32 id = 0;
    for (const auto& phase : document.Phases)
    {
        id = std::max(id, phase.Id);
        for (const auto& processor : phase.Processors)
            id = std::max(id, processor.Id);
    }
    return id == UINT32_MAX ? 0 : id + 1;
}
} // namespace

ParticleProcessorInstance MakeProcessorInstance(const ParticleProcessorDescriptor& descriptor)
{
    ParticleProcessorInstance instance;
    instance.Descriptor = &descriptor;
    instance.Label = std::string(descriptor.DisplayName);
    instance.Stage = descriptor.DefaultStage;
    instance.Parameters.resize(descriptor.ParameterSize);
    descriptor.Construct(instance.Parameters.data());
    return instance;
}

bool HaveSameStructure(const StackDocument& previous, const StackDocument& updated)
{
    if (previous.EntryPhase != updated.EntryPhase || previous.Phases.size() != updated.Phases.size())
        return false;
    for (size_t phase = 0; phase < previous.Phases.size(); ++phase)
    {
        const auto& before = previous.Phases[phase];
        const auto& after = updated.Phases[phase];
        if (before.Id != after.Id || before.Processors.size() != after.Processors.size())
            return false;
        for (size_t index = 0; index < before.Processors.size(); ++index)
        {
            const auto& a = before.Processors[index];
            const auto& b = after.Processors[index];
            if (a.Id != b.Id || a.Descriptor != b.Descriptor || a.Stage != b.Stage || a.Enabled != b.Enabled ||
                !SameInterval(a, b))
                return false;
        }
    }
    return true;
}

StackDocument MakeDefaultStack()
{
    StackDocument document;
    StackPhase phase;
    // As many particles a second as a default emitter holds, living one second each.
    auto rate = MakeProcessorInstance(ParticleEmitRateProcessor());
    rate.Id = 2;
    rate.Label = "Continuous Emission";
    rate.Params<ParticleEmitRateParameters>().Rate = static_cast<float>(Components::ParticleEmitter3D{}.Amount);
    phase.Processors.push_back(std::move(rate));
    auto launch = MakeProcessorInstance(ParticleVelocityConeProcessor());
    launch.Id = 3;
    launch.Label = "Launch";
    phase.Processors.push_back(std::move(launch));
    auto gravity = MakeProcessorInstance(ParticleAccelerationProcessor());
    gravity.Id = 4;
    gravity.Label = "Gravity";
    phase.Processors.push_back(std::move(gravity));
    document.Phases.push_back(std::move(phase));
    return document;
}

namespace
{
template <uint8_t N>
float HighestKey(const Math::CurveBase<N>& curve)
{
    float highest = curve.KeyCount > 0 ? curve.Keys[0].Value : 0.0f;
    for (uint32 index = 1; index < curve.KeyCount; ++index)
        highest = std::max(highest, curve.Keys[index].Value);
    return highest;
}

// The highest value a parameter takes: its range's top, or its curves' highest key.
float HighestValue(const ParticleValue& value)
{
    switch (value.Mode)
    {
    case ParticleValueMode::Constant:
    case ParticleValueMode::Range:
        return std::max(value.Minimum, value.Maximum);
    case ParticleValueMode::Curve:
        return HighestKey(value.MinimumCurve);
    case ParticleValueMode::CurveRange:
        return std::max(HighestKey(value.MinimumCurve), HighestKey(value.MaximumCurve));
    }
    return value.Maximum;
}
} // namespace

float ContinuousEmissionPeak(const StackDocument& document)
{
    const StackPhase* entry = FindPhase(document, document.EntryPhase);
    if (!entry)
        return 0.0f;
    float rate = 0.0f;
    float lifetime = document.Lifetime;
    for (const auto& processor : entry->Processors)
    {
        if (!processor.Enabled)
            continue;
        if (processor.Descriptor == &ParticleEmitRateProcessor())
            rate += std::max(HighestValue(processor.Params<ParticleEmitRateParameters>().Rate), 0.0f);
        else if (processor.Descriptor == &ParticlePropertyProcessor() && processor.Stage == ParticleStage::Birth)
        {
            const auto& property = processor.Params<ParticlePropertyParameters>();
            if (property.Target == ParticleAttribute::Lifetime && property.Operation == ParticleOperation::Set)
                lifetime = HighestValue(property.Value[0]);
        }
    }
    return rate * std::max(lifetime, 0.0f);
}

StackPhase* FindPhase(StackDocument& document, uint32 id)
{
    for (auto& phase : document.Phases)
        if (phase.Id == id)
            return &phase;
    return nullptr;
}

const StackPhase* FindPhase(const StackDocument& document, uint32 id)
{
    for (const auto& phase : document.Phases)
        if (phase.Id == id)
            return &phase;
    return nullptr;
}

ParticleProcessorInstance* FindProcessor(StackDocument& document, uint32 id)
{
    for (auto& phase : document.Phases)
        for (auto& processor : phase.Processors)
            if (processor.Id == id)
                return &processor;
    return nullptr;
}

const ParticleProcessorInstance* FindProcessor(const StackDocument& document, uint32 id)
{
    for (const auto& phase : document.Phases)
        for (const auto& processor : phase.Processors)
            if (processor.Id == id)
                return &processor;
    return nullptr;
}

bool AddPhase(StackDocument& document, std::string_view label, uint32& id, std::string& error)
{
    if (document.Phases.size() >= kMaxStackPhases || (id = NextStackId(document)) == 0)
    {
        error = "A stack holds at most " + std::to_string(kMaxStackPhases) + " phases";
        return false;
    }
    StackPhase phase;
    phase.Id = id;
    phase.Label = std::string(label);
    document.Phases.push_back(std::move(phase));
    return true;
}

bool RemovePhase(StackDocument& document, uint32 id, std::string& error)
{
    if (document.EntryPhase == id)
    {
        error = "Choose another entry phase before removing this phase";
        return false;
    }
    for (const auto& phase : document.Phases)
    {
        if (phase.Id == id)
            continue;
        for (const auto& processor : phase.Processors)
        {
            if (processor.Descriptor != &ParticleEventProcessor())
                continue;
            const auto& rule = processor.Params<ParticleEventParameters>();
            if (rule.Action == ParticleEventAction::Transition && rule.Destination == id)
            {
                error = "A rule in " + phase.Label + " moves particles to this phase";
                return false;
            }
        }
    }
    return std::erase_if(document.Phases, [id](const StackPhase& phase)
                         { return phase.Id == id; }) != 0;
}

bool AddProcessor(StackDocument& document, uint32 phase, ParticleProcessorInstance processor, uint32& id,
                  std::string& error)
{
    auto* target = FindPhase(document, phase);
    size_t count = 0;
    for (const auto& existing : document.Phases)
        count += existing.Processors.size();
    if (!target || count >= kMaxStackProcessors || (id = NextStackId(document)) == 0)
    {
        error = target ? "A stack holds at most " + std::to_string(kMaxStackProcessors) + " processors"
                       : "The phase does not exist";
        return false;
    }
    processor.Id = id;
    target->Processors.push_back(std::move(processor));
    return true;
}

bool RemoveProcessor(StackDocument& document, uint32 id)
{
    for (auto& phase : document.Phases)
        if (std::erase_if(phase.Processors, [id](const ParticleProcessorInstance& processor)
                          { return processor.Id == id; }) != 0)
            return true;
    return false;
}

bool ReorderProcessor(StackDocument& document, uint32 phaseId, uint32 processor, uint32 target, bool after)
{
    auto* phase = FindPhase(document, phaseId);
    if (!phase)
        return false;
    auto& processors = phase->Processors;
    auto from = std::find_if(processors.begin(), processors.end(),
                             [processor](const auto& candidate)
                             { return candidate.Id == processor; });
    auto to = std::find_if(processors.begin(), processors.end(),
                           [target](const auto& candidate)
                           { return candidate.Id == target; });
    if (from == processors.end() || to == processors.end() || from == to)
        return false;
    if (after)
        ++to;
    if (from < to)
        std::rotate(from, from + 1, to);
    else
        std::rotate(to, from, from + 1);
    return true;
}

} // namespace GameEngine::Particles

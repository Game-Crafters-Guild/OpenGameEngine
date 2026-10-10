#include "Particles/ParticleProcessorRegistry.h"

#include "Particles/ParticleStackDocument.h"
#include "Processors/ParticleBuiltInProcessors.h"

#include "ECS/ModuleRegistration.h"
#include "Logger/Logger.h"

#include <array>
#include <cassert>
#include <deque>
#include <mutex>
#include <optional>

namespace GameEngine::Particles
{
namespace
{
struct StageName
{
    std::string_view Wire;
    std::string_view Display;
};

constexpr std::array<StageName, kParticleStageCount> kStageNames = {{
    {"emission", "Emission"},
    {"birth", "Birth"},
    {"enter", "Enter"},
    {"update", "Update"},
    {"exit", "Exit"},
    {"event", "Event"},
}};

std::mutex& RegistryMutex()
{
    static std::mutex mutex;
    return mutex;
}

// One registered processor: the descriptor stacks point at, the module load that registered it,
// and what it replaced, which an aborted load gives back.
struct RegisteredProcessor
{
    ParticleProcessorDescriptor Descriptor;
    ECS::ModuleRegistrationStamp Module;
    std::optional<ParticleProcessorDescriptor> Replaced;
    ECS::ModuleRegistrationStamp ReplacedModule;
    // Hidden from lookups once the load that added it aborted; stays in place so no pointer dangles.
    bool Purged = false;
};

// A deque keeps descriptor addresses stable: documents hold descriptor pointers for the process.
std::deque<RegisteredProcessor>& Registrations()
{
    static std::deque<RegisteredProcessor> registrations;
    return registrations;
}

// Two registrations of an Id agree on how a stack's parameter bytes are laid out and read.
bool SameParameterLayout(const ParticleProcessorDescriptor& a, const ParticleProcessorDescriptor& b)
{
    if (a.ParameterSize != b.ParameterSize || a.ParameterAlignment != b.ParameterAlignment ||
        a.Fields.size() != b.Fields.size() || a.Parameters.size() != b.Parameters.size())
        return false;
    for (size_t index = 0; index < a.Fields.size(); ++index)
    {
        const ECS::FieldInfo& left = a.Fields[index];
        const ECS::FieldInfo& right = b.Fields[index];
        if (left.Name != right.Name || left.Offset != right.Offset || left.Size != right.Size ||
            left.Type != right.Type)
            return false;
    }
    for (size_t index = 0; index < a.Parameters.size(); ++index)
        if (a.Parameters[index].Name != b.Parameters[index].Name || a.Parameters[index].Kind != b.Parameters[index].Kind)
            return false;
    return true;
}

uint32 KindSize(ParticleParameterKind kind)
{
    switch (kind)
    {
    case ParticleParameterKind::Bool:
        return 1;
    case ParticleParameterKind::Float:
    case ParticleParameterKind::UInt:
    case ParticleParameterKind::Phase:
    case ParticleParameterKind::Flags:
        return 4;
    case ParticleParameterKind::Enum:
        return 1;
    case ParticleParameterKind::Vector3:
        return static_cast<uint32>(sizeof(Mathematics::Vector3));
    case ParticleParameterKind::Value:
        return static_cast<uint32>(sizeof(ParticleValue));
    case ParticleParameterKind::ValueInput:
        return static_cast<uint32>(sizeof(ParticleValueInput));
    case ParticleParameterKind::Text:
    default:
        return 0;
    }
}

// Every reflected field needs exactly one presentation entry whose kind matches its storage; a
// value field may be a C array of values. A mismatch is a programming error in the registering
// module, caught the first time the descriptor is registered.
bool DescribesEveryField(const ParticleProcessorDescriptor& descriptor)
{
    if (descriptor.Fields.size() != descriptor.Parameters.size())
        return false;
    for (const auto& field : descriptor.Fields)
    {
        const ParticleParameterField* presentation = nullptr;
        for (const auto& candidate : descriptor.Parameters)
            if (candidate.Name == field.Name)
                presentation = &candidate;
        if (!presentation)
            return false;
        const uint32 size = KindSize(presentation->Kind);
        if (presentation->Kind == ParticleParameterKind::Text)
        {
            if (field.Type != ECS::FieldTypeId::String || field.Size < 2)
                return false;
        }
        else if (presentation->Kind == ParticleParameterKind::Value)
        {
            if (field.Size == 0 || field.Size % size != 0 || field.Size / size > 4)
                return false;
        }
        else if (field.Size != size)
            return false;
        if ((presentation->Kind == ParticleParameterKind::Enum || presentation->Kind == ParticleParameterKind::Flags) &&
            presentation->Options.empty())
            return false;
    }
    return true;
}

bool StoreDescriptor(const ParticleProcessorDescriptor& descriptor)
{
    assert(!descriptor.Id.empty() && "A particle processor needs a stable id");
    assert(descriptor.Stages != 0 && (descriptor.Stages & StageBit(descriptor.DefaultStage)) != 0);
    assert(descriptor.ParameterAlignment <= alignof(std::max_align_t));
    assert(descriptor.Construct && DescribesEveryField(descriptor) &&
           "Describe every reflected parameter field with a matching kind");
    assert((descriptor.Role != ParticleProcessorRole::Particle || descriptor.Execute) &&
           (descriptor.Role != ParticleProcessorRole::Emission || descriptor.Emit));
    const ECS::ModuleRegistrationStamp& module = ECS::GetActiveRegistrationModule();
    std::lock_guard lock(RegistryMutex());
    for (auto& existing : Registrations())
    {
        if (existing.Descriptor.Id != descriptor.Id)
            continue;
        if (!SameParameterLayout(existing.Descriptor, descriptor))
        {
            Logger::Log::Error("Particle processor '{}' was registered again with a different parameter struct; "
                               "the first registration stays. Stacks hold parameter bytes laid out for it, so "
                               "give the new struct a new processor id, or restart to load stacks with it.",
                               descriptor.Id);
            return false;
        }
        existing.Replaced = existing.Descriptor;
        existing.ReplacedModule = existing.Module;
        existing.Descriptor = descriptor;
        existing.Module = module;
        existing.Purged = false;
        return true;
    }
    Registrations().push_back(RegisteredProcessor{descriptor, module});
    return true;
}

// Built-ins register on first use (linker-strip proof, no initialisation-order dependency), before
// any descriptor a game registers, so they keep the first places in the picker.
void EnsureBuiltInsRegistered()
{
    static std::once_flag once;
    std::call_once(once, []
                   {
        for (const auto& descriptor : MakeBuiltInParticleProcessors())
            StoreDescriptor(descriptor); });
}
} // namespace

std::string_view StageWireName(ParticleStage stage)
{
    return kStageNames[static_cast<uint32>(stage)].Wire;
}

std::string_view StageDisplayName(ParticleStage stage)
{
    return kStageNames[static_cast<uint32>(stage)].Display;
}

bool TryParseStage(std::string_view wireName, ParticleStage& stage)
{
    for (uint32 index = 0; index < kParticleStageCount; ++index)
        if (kStageNames[index].Wire == wireName)
        {
            stage = static_cast<ParticleStage>(index);
            return true;
        }
    return false;
}

void ParticleValidationContext::Error(std::string message) const
{
    if (Diagnostics)
        Diagnostics->push_back({Path, std::move(message)});
}

bool ParticleProcessorRegistry::Register(const ParticleProcessorDescriptor& descriptor)
{
    EnsureBuiltInsRegistered();
    return StoreDescriptor(descriptor);
}

const ParticleProcessorDescriptor* ParticleProcessorRegistry::Find(std::string_view id)
{
    EnsureBuiltInsRegistered();
    std::lock_guard lock(RegistryMutex());
    for (const auto& registration : Registrations())
        if (!registration.Purged && registration.Descriptor.Id == id)
            return &registration.Descriptor;
    return nullptr;
}

void ParticleProcessorRegistry::ForEach(const std::function<void(const ParticleProcessorDescriptor&)>& visit)
{
    EnsureBuiltInsRegistered();
    std::vector<const ParticleProcessorDescriptor*> snapshot;
    {
        std::lock_guard lock(RegistryMutex());
        snapshot.reserve(Registrations().size());
        for (const auto& registration : Registrations())
            if (!registration.Purged)
                snapshot.push_back(&registration.Descriptor);
    }
    for (const auto* descriptor : snapshot)
        visit(*descriptor);
}

std::size_t ParticleProcessorRegistry::PurgeModuleProcessors(std::string_view moduleId, uint64 generation)
{
    if (moduleId.empty())
        return 0;
    std::lock_guard lock(RegistryMutex());
    std::size_t undone = 0;
    for (auto& registration : Registrations())
    {
        if (registration.Purged || !registration.Module.Matches(moduleId, generation))
            continue;
        if (registration.Replaced)
        {
            registration.Descriptor = *registration.Replaced;
            registration.Module = registration.ReplacedModule;
            registration.Replaced.reset();
        }
        else
        {
            registration.Purged = true;
        }
        ++undone;
    }
    return undone;
}

std::size_t ParticleProcessorRegistry::CountSupersededModuleProcessors(std::string_view moduleId,
                                                                       uint64 currentGeneration)
{
    if (moduleId.empty())
        return 0;
    std::lock_guard lock(RegistryMutex());
    std::size_t superseded = 0;
    for (const auto& registration : Registrations())
    {
        const bool olderServes = registration.Module.ModuleId == moduleId && registration.Module.Generation < currentGeneration;
        const bool olderServed = registration.ReplacedModule.ModuleId == moduleId &&
                                 registration.ReplacedModule.Generation < currentGeneration;
        if (!registration.Purged && (olderServes || olderServed))
            ++superseded;
    }
    return superseded;
}

void ParticleScratch::Configure(uint32 capacity)
{
    for (auto& floats : m_Floats)
        floats.assign(capacity, 0.0f);
}

std::span<float> ParticleScratch::Floats(uint32 index)
{
    assert(index < kArrayCount);
    return m_Floats[index];
}

} // namespace GameEngine::Particles

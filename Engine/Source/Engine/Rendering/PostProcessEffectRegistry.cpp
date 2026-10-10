#include "Engine/Rendering/PostProcessEffectRegistry.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace GameEngine::Rendering
{

// Defined in PostProcessEffectDescriptors.cpp, next to the descriptor tables.
void RegisterBuiltInPostProcessEffectDescriptors();

namespace
{
std::mutex& RegistryMutex()
{
    static std::mutex m;
    return m;
}

// Values are stable across rehash (node-based map), so Find may hand out
// pointers into the map; Register overwrites values in place.
std::unordered_map<ECS::ComponentTypeId, PostProcessEffectDescriptor>& Descriptors()
{
    static std::unordered_map<ECS::ComponentTypeId, PostProcessEffectDescriptor> s;
    return s;
}

// First-registration order, for deterministic ForEach iteration.
std::vector<ECS::ComponentTypeId>& RegistrationOrder()
{
    static std::vector<ECS::ComponentTypeId> s;
    return s;
}

// Built-ins register on first read access (linker-strip proof, no init-order dance).
void EnsureBuiltInsRegistered()
{
    static std::once_flag builtIns;
    std::call_once(builtIns, [] { RegisterBuiltInPostProcessEffectDescriptors(); });
}

// Flat shader-name index for the per-frame TryWriteField/TryReadField path
// (push constants, skipWhen — every pass, every frame). Built lazily under the
// registry mutex, read lock-free. Register retires the current index instead
// of freeing it so a concurrent reader never sees freed memory; keys are
// string_views over the descriptor tables' static literals.
struct SettingsNameIndex
{
    std::unordered_map<std::string_view, EffectSettingsField> Fields;
    std::unordered_map<std::string_view, EffectSettingsGate> Gates;
};
std::atomic<const SettingsNameIndex*>& NameIndexSlot()
{
    static std::atomic<const SettingsNameIndex*> s{nullptr};
    return s;
}
std::vector<std::unique_ptr<const SettingsNameIndex>>& RetiredNameIndices()
{
    static std::vector<std::unique_ptr<const SettingsNameIndex>> s;
    return s;
}

std::unique_ptr<SettingsNameIndex> BuildNameIndex()
{
    auto built = std::make_unique<SettingsNameIndex>();
    for (const auto& [type, descriptor] : Descriptors())
    {
        for (const EffectSettingsField& field : descriptor.SettingsFields)
        {
            if (!field.ShaderName.empty()) // blend-only members have no name
                built->Fields.emplace(field.ShaderName, field);
        }
        for (const EffectSettingsGate& gate : descriptor.Gates)
            built->Gates.emplace(gate.ShaderName, gate);
    }
    return built;
}

const SettingsNameIndex& NameIndex()
{
    EnsureBuiltInsRegistered();
    const SettingsNameIndex* index = NameIndexSlot().load(std::memory_order_acquire);
    if (index)
        return *index;
    std::lock_guard<std::mutex> lk(RegistryMutex());
    index = NameIndexSlot().load(std::memory_order_relaxed);
    if (!index)
    {
        std::unique_ptr<SettingsNameIndex> built = BuildNameIndex();
        NameIndexSlot().store(built.get(), std::memory_order_release);
        index = built.release();
    }
    return *index;
}

// Flat per-frame blend table (same lazy-build/retire discipline as the name
// index): BlendPostProcessSettings folds over it once per volume per view.
struct SettingsFieldTable
{
    std::vector<EffectSettingsField> Fields;
};
std::atomic<const SettingsFieldTable*>& FieldTable()
{
    static std::atomic<const SettingsFieldTable*> s{nullptr};
    return s;
}
std::vector<std::unique_ptr<const SettingsFieldTable>>& RetiredFieldTables()
{
    static std::vector<std::unique_ptr<const SettingsFieldTable>> s;
    return s;
}
} // namespace

void PostProcessEffectRegistry::Register(const PostProcessEffectDescriptor& descriptor)
{
    if (descriptor.Type == 0 || descriptor.ComponentName.empty())
        return;
    std::lock_guard<std::mutex> lk(RegistryMutex());
    auto& m = Descriptors();
    if (m.find(descriptor.Type) == m.end())
        RegistrationOrder().push_back(descriptor.Type);
    m[descriptor.Type] = descriptor;
    if (const SettingsNameIndex* stale = NameIndexSlot().exchange(nullptr, std::memory_order_acq_rel))
        RetiredNameIndices().emplace_back(stale);
    if (const SettingsFieldTable* stale = FieldTable().exchange(nullptr, std::memory_order_acq_rel))
        RetiredFieldTables().emplace_back(stale);
}

const PostProcessEffectDescriptor* PostProcessEffectRegistry::Find(ECS::ComponentTypeId type)
{
    EnsureBuiltInsRegistered();
    std::lock_guard<std::mutex> lk(RegistryMutex());
    auto& m = Descriptors();
    const auto it = m.find(type);
    return it == m.end() ? nullptr : &it->second;
}

void PostProcessEffectRegistry::ForEach(const std::function<void(const PostProcessEffectDescriptor&)>& fn)
{
    if (!fn)
        return;
    EnsureBuiltInsRegistered();
    // Snapshot stable descriptor pointers under the lock, invoke outside it so
    // the callback may call Find without self-deadlocking.
    std::vector<const PostProcessEffectDescriptor*> snapshot;
    {
        std::lock_guard<std::mutex> lk(RegistryMutex());
        auto& m = Descriptors();
        snapshot.reserve(RegistrationOrder().size());
        for (ECS::ComponentTypeId type : RegistrationOrder())
        {
            const auto it = m.find(type);
            if (it != m.end())
                snapshot.push_back(&it->second);
        }
    }
    for (const PostProcessEffectDescriptor* d : snapshot)
        fn(*d);
}

const EffectSettingsField* PostProcessEffectRegistry::FindSettingsField(std::string_view shaderName)
{
    const SettingsNameIndex& index = NameIndex();
    const auto it = index.Fields.find(shaderName);
    return it == index.Fields.end() ? nullptr : &it->second;
}

const EffectSettingsGate* PostProcessEffectRegistry::FindGate(std::string_view shaderName)
{
    const SettingsNameIndex& index = NameIndex();
    const auto it = index.Gates.find(shaderName);
    return it == index.Gates.end() ? nullptr : &it->second;
}

std::span<const EffectSettingsField> PostProcessEffectRegistry::AllSettingsFields()
{
    EnsureBuiltInsRegistered();
    const SettingsFieldTable* table = FieldTable().load(std::memory_order_acquire);
    if (!table)
    {
        std::lock_guard<std::mutex> lk(RegistryMutex());
        table = FieldTable().load(std::memory_order_relaxed);
        if (!table)
        {
            auto built = std::make_unique<SettingsFieldTable>();
            auto& m = Descriptors();
            for (ECS::ComponentTypeId type : RegistrationOrder())
            {
                const auto it = m.find(type);
                if (it == m.end())
                    continue;
                for (const EffectSettingsField& field : it->second.SettingsFields)
                    built->Fields.push_back(field);
            }
            FieldTable().store(built.get(), std::memory_order_release);
            table = built.release();
        }
    }
    return table->Fields;
}

const EffectFieldIO* PostProcessEffectRegistry::FindFieldIO(const PostProcessEffectDescriptor& descriptor,
                                                            std::string_view fieldName)
{
    for (const EffectFieldIO& io : descriptor.FieldIO)
    {
        if (io.FieldName == fieldName)
            return &io;
    }
    return nullptr;
}

} // namespace GameEngine::Rendering

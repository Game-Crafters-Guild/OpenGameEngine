#include "Ocean/OceanPresetOverrideValidator.h"

#include "Components/Rendering/Ocean.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Logger/Logger.h"
#include "Types/Fnv1a.h"

#include <algorithm>
#include <iterator>
#include <string_view>

namespace GameEngine::Ocean
{
namespace
{
struct PresetSection
{
    std::string_view Prefix;
    ECS::ComponentTypeId TypeId;
};

// The identities of every field under the sections OceanExtractionSystem applies a
// preset to, hashed the way an override names them: HashStringId("Prefix.Field").
std::vector<StringId> CollectPresetFieldIdentities()
{
    const PresetSection sections[] = {
        {"Renderer", ECS::GetComponentTypeId<Components::OceanRenderer>()},
        {"Surface", ECS::GetComponentTypeId<Components::OceanSurface>()},
        {"Spectrum", ECS::GetComponentTypeId<Components::OceanWaveSpectrum>()},
        {"WaterBody", ECS::GetComponentTypeId<Components::OceanWaterBody>()},
        {"WaterBody", ECS::GetComponentTypeId<Components::OceanPolygonWaterBody>()},
    };
    std::vector<StringId> identities;
    for (const PresetSection& section : sections)
    {
        // A string_view, not a literal: a literal binds to the (data, size) overload.
        const StringId root = Hashing::Fnv1a64(std::string_view("."), HashStringId(section.Prefix));
        for (const ECS::FieldInfo& field : ECS::ComponentFieldRegistry::Get(section.TypeId))
            identities.push_back(Hashing::Fnv1a64(field.Name, root));
    }
    std::ranges::sort(identities);
    return identities;
}
} // namespace

void OceanPresetOverrideValidator::Validate(ECS::World& world)
{
    // Entry-sample the gate before iterating (ChangeGate contract): a writer that
    // stamps during this tick is seen on the next run, never skipped.
    const uint64 entryVersion = world.GetGlobalSystemVersion();
    const std::size_t structuralVersion = world.GetStructuralChangeVersion();
    if (!HasChangedSince(world))
        return;

    // A cleared world reuses entity ids, so what was reported for the old scene no
    // longer names the same bindings.
    const uint64 resetGeneration = world.GetLifecycleResetGeneration();
    if (resetGeneration != m_ResetGeneration)
    {
        m_Reported.clear();
        m_ResetGeneration = resetGeneration;
    }
    if (m_PresetFields.empty())
        m_PresetFields = CollectPresetFieldIdentities();

    world.Query<ECS::Read<Components::OceanPresetBinding>>().Each(
        [this](ECS::EntityHandle entity, const Components::OceanPresetBinding& binding)
        {
            ReportUnknownOverrides(entity, binding);
        });
    m_Gate.LastRunVersion = entryVersion;
    m_StructuralVersion = structuralVersion;
    m_Primed = true;
}

bool OceanPresetOverrideValidator::HasChangedSince(ECS::World& world) const
{
    if (!m_Primed || !ECS::ChangeFilter::Enabled())
        return true;
    // Adding or removing a binding is an archetype move: the structural version
    // covers it because column stamps cannot.
    if (world.GetStructuralChangeVersion() != m_StructuralVersion)
        return true;
    bool changed = false;
    auto query = world.Query<ECS::Read<Components::OceanPresetBinding>>();
    query.Changed<Components::OceanPresetBinding>(m_Gate);
    query.Each([&](const Components::OceanPresetBinding&) { changed = true; });
    return changed;
}

void OceanPresetOverrideValidator::ReportUnknownOverrides(
    ECS::EntityHandle entity, const Components::OceanPresetBinding& binding)
{
    for (size_t slot = 0u; slot < std::size(binding.Overrides); ++slot)
    {
        const StringId identity = binding.Overrides[slot].FieldIdentifier;
        if (identity == 0u || std::ranges::binary_search(m_PresetFields, identity))
            continue;
        if (!m_Reported.emplace(entity.id, identity).second)
            continue;
        // Named as the scene file writes it, so the line can be found and removed.
        Logger::Log::Warning(
            "OceanPresetBinding on entity {}: scene line "
            "\"OceanPresetBinding.Overrides{}.FieldIdentifier = {}\" names no field an ocean "
            "preset drives, so the override has no effect. Remove the line from the scene to "
            "drop it.",
            entity.id, slot, identity);
    }
}

} // namespace GameEngine::Ocean

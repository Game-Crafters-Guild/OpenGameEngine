#pragma once

#include "ECS/ChangeFilter.h"
#include "Types/StringId.h"
#include "Types/Types.h"

#include <cstddef>
#include <set>
#include <utility>
#include <vector>

namespace GameEngine::ECS
{
class World;
struct EntityHandle;
}

namespace GameEngine::Components
{
struct OceanPresetBinding;
}

namespace GameEngine::Ocean
{

/// Warns about scene preset overrides that name no field an ocean preset drives.
///
/// Such an override changes nothing, yet the scene keeps saving it, so the warning
/// is the only place it surfaces. Each binding entity and identity is reported once.
/// Bindings are re-read only when an OceanPresetBinding column was written or the
/// world's archetype layout changed (a scene load, a binding added); an idle frame
/// runs one change-filtered query that visits no binding.
class OceanPresetOverrideValidator
{
public:
    void Validate(ECS::World& world);

private:
    bool HasChangedSince(ECS::World& world) const;
    void ReportUnknownOverrides(ECS::EntityHandle entity,
                                const Components::OceanPresetBinding& binding);

    // Identities of every field the preset sections drive, sorted.
    std::vector<StringId> m_PresetFields;
    // (entity id, field identity) pairs already reported.
    std::set<std::pair<uint32, StringId>> m_Reported;
    ECS::ChangeGate m_Gate{};
    std::size_t m_StructuralVersion = 0;
    uint64 m_ResetGeneration = 0;
    bool m_Primed = false;
};

} // namespace GameEngine::Ocean

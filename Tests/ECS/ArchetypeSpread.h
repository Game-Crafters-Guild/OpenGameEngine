#pragma once
// A world whose entities spread over many archetypes: one entity per bit mask, each in an
// archetype of its own, every one carrying Position and Velocity so a query on those two
// matches all of them. The shape the query allocation and construction measurements share:
// the archetype match and the snapshot dominate, not the entity loop.
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "TestComponents.h"
#include "Types/Types.h"

#include <cstddef>
#include <utility>

namespace GameEngine::ECS::test
{
// One distinct component type per bit of the archetype mask.
template <int Bit>
struct ArchetypeTag
{
    int32 Value = 0;
};

constexpr std::size_t kArchetypeSpreadTagCount = 8;
// Every mask below this is a distinct archetype.
constexpr unsigned kArchetypeSpreadMaxCount = 1u << kArchetypeSpreadTagCount;

template <std::size_t... Bits>
void AddArchetypeSpreadTags(Entity& entity, unsigned mask, std::index_sequence<Bits...>)
{
    (((mask & (1u << Bits)) != 0u ? void(entity.Set(ArchetypeTag<static_cast<int>(Bits)>{}))
                                  : void()),
     ...);
}

// One entity per mask below `archetypeCount`, each landing in its own archetype.
inline void PopulateArchetypeSpread(World& world, unsigned archetypeCount)
{
    for (unsigned mask = 0; mask < archetypeCount; ++mask)
    {
        auto entity = world.Create();
        entity.Set(Position{float(mask), 0.0f, 0.0f});
        entity.Set(Velocity{1.0f, 0.0f, 0.0f});
        AddArchetypeSpreadTags(entity, mask, std::make_index_sequence<kArchetypeSpreadTagCount>{});
    }
    world.ProcessCommands();
}
} // namespace GameEngine::ECS::test

#include "Editor/Hierarchy/HierarchyOrdering.h"

#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

#include <algorithm>
#include <vector>

namespace GameEngine {
namespace Editor {

namespace {

std::int32_t MaxHierarchyOrder(ECS::World& world)
{
    std::int32_t maxOrder = 0;
    std::vector<ECS::EntityHandle> alive;
    world.GetAliveEntitiesSnapshot(alive);
    for (const ECS::EntityHandle ent : alive)
    {
        if (!world.IsValid(ent))
            continue;
        if (const auto* o = world.GetComponent<Components::HierarchyOrder>(ent))
            maxOrder = std::max(maxOrder, o->order);
    }
    return maxOrder;
}

} // namespace

Components::HierarchyOrder NextHierarchyOrderAtBottom(ECS::World* world)
{
    Components::HierarchyOrder ho{};
    if (!world)
        return ho;
    ho.order = MaxHierarchyOrder(*world) + 1;
    return ho;
}

void AppendUnorderedRootsToHierarchyEnd(ECS::World& world, std::span<const ECS::EntityHandle> roots)
{
    std::int32_t next = MaxHierarchyOrder(world) + 1;
    for (const ECS::EntityHandle root : roots)
    {
        if (!root.IsValid() || !world.IsValid(root))
            continue;
        if (world.GetComponent<Components::HierarchyOrder>(root))
            continue;
        Components::HierarchyOrder ho{};
        ho.order = next++;
        world.AddComponentImmediate(root, ho);
    }
}

} // namespace Editor
} // namespace GameEngine

#include <gtest/gtest.h>

#include "SceneView/SelectionOutlineContributors.h"

#include "Components/Hierarchy.h"
#include "Components/Transform.h"
#include "ECS/DisabledInHierarchySystem.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"

#include <vector>

namespace
{
using GameEngine::Components::Parent;
using GameEngine::Components::Transform;
using GameEngine::ECS::DisabledInHierarchySystem;
using GameEngine::ECS::Entity;
using GameEngine::ECS::EntityHandle;
using GameEngine::ECS::World;
using GameEngine::Editor::CollectSelectionOutlineContributors;

EntityHandle MakeOutlinedNode(World& world, EntityHandle parent = {})
{
    const EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate<Transform>(entity, Transform{});
    if (parent.IsValid())
        world.AddComponentImmediate<Parent>(entity, Parent{parent});
    return entity;
}
} // namespace

// A selected parent outlines its subtree less the children switched off on their own. Switched off
// itself while it stays selected or hovered, it outlines nothing, and neither do its children, which
// the hierarchy pass takes out with it.
TEST(SelectionOutlineContributorsTests, EntitiesInactiveInTheHierarchyDrawNoOutline)
{
    World world;
    DisabledInHierarchySystem pass;
    const EntityHandle parent = MakeOutlinedNode(world);
    const EntityHandle child = MakeOutlinedNode(world, parent);
    const EntityHandle switchedOffChild = MakeOutlinedNode(world, parent);
    Entity(&world, switchedOffChild).SetEnabled(false);
    const EntityHandle selection[] = {parent};
    const EntityHandle descendants[] = {child, switchedOffChild};
    std::vector<EntityHandle> contributors;

    CollectSelectionOutlineContributors(world, {selection, descendants, {}, {}, true}, contributors);
    EXPECT_EQ(contributors, (std::vector<EntityHandle>{parent, child}));

    Entity(&world, parent).SetEnabled(false);
    pass.Update(world, 0.0f);
    CollectSelectionOutlineContributors(world, {selection, descendants, {}, {}, true}, contributors);
    EXPECT_TRUE(contributors.empty()) << "a switched-off parent and its children draw no outline";

    CollectSelectionOutlineContributors(world, {{}, {}, parent, descendants, true}, contributors);
    EXPECT_TRUE(contributors.empty()) << "hovering it outlines nothing either";
}

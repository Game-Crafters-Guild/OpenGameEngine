// get_scene_hierarchy's enable state. A child under a switched-off parent is on itself and inactive
// through the hierarchy pass; a report that carried the entity's own state alone would read it as
// enabled.

#include <gtest/gtest.h>

#include "DebugServer/SceneHierarchyReport.h"

#include "Components/Hierarchy.h"
#include "Components/Transform.h"
#include "ECS/DisabledInHierarchySystem.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"

using namespace GameEngine;

TEST(SceneHierarchyReportTests, AChildUnderASwitchedOffParentReportsItsDerivedState)
{
    ECS::World world(nullptr);
    ECS::DisabledInHierarchySystem pass;
    const ECS::EntityHandle root = world.CreateHandle(Components::Transform{});
    const ECS::EntityHandle child = world.CreateHandle(Components::Transform{});
    world.AddComponentImmediate<Components::Parent>(child, Components::Parent{root});
    ECS::Entity(&world, root).SetEnabled(false);
    pass.Update(world, 0.0f);

    const nlohmann::json report = Editor::DescribeSceneHierarchy(world);
    ASSERT_EQ(report["entities"].size(), 1u);
    const nlohmann::json& rootNode = report["entities"][0];
    EXPECT_FALSE(rootNode.value("enabled", true));
    EXPECT_FALSE(rootNode.value("enabledInHierarchy", true));
    ASSERT_EQ(rootNode["children"].size(), 1u);
    const nlohmann::json& childNode = rootNode["children"][0];
    EXPECT_TRUE(childNode.value("enabled", false)) << "the child's own state is on";
    EXPECT_FALSE(childNode.value("enabledInHierarchy", true)) << "it is inactive through its parent";
}

#include <gtest/gtest.h>

#include "Graph/GraphPortDropSlot.h"
#include "UI/Interaction/DropTarget.h"

#include <string>
#include <vector>

using namespace GameEngine;

TEST(GraphPortDropSlotTests, HashNodeIdIsStableAndNeverZero)
{
    const std::vector<std::string> ids = {"node-b", "node-a", "node-c"};
    const UI::Interaction::ItemId hashB = GraphPortDropSlot::HashNodeId(ids[0]);
    const UI::Interaction::ItemId hashA = GraphPortDropSlot::HashNodeId(ids[1]);
    const UI::Interaction::ItemId hashC = GraphPortDropSlot::HashNodeId(ids[2]);

    std::vector<std::string> reordered = {ids[2], ids[0], ids[1]};
    EXPECT_EQ(hashB, GraphPortDropSlot::HashNodeId(reordered[1]));
    EXPECT_EQ(hashA, GraphPortDropSlot::HashNodeId(reordered[2]));
    EXPECT_EQ(hashC, GraphPortDropSlot::HashNodeId(reordered[0]));

    EXPECT_NE(hashA, 0u);
    EXPECT_NE(hashB, 0u);
    EXPECT_NE(hashC, 0u);
    EXPECT_NE(GraphPortDropSlot::HashNodeId({}), 0u);
    EXPECT_EQ(GraphPortDropSlot::HashNodeId("node-a"), hashA);
    EXPECT_NE(GraphPortDropSlot::HashNodeId("node_0"), 1u);
    EXPECT_NE(GraphPortDropSlot::HashNodeId("node_1"), 2u);
}

TEST(GraphPortDropSlotTests, HitTestDropTargetReturnsStableHash)
{
    GraphPortDropSlot slot;
    UI::Interaction::DropHit unbound{};
    EXPECT_FALSE(slot.HitTestDropTarget(8.0f, 12.0f, unbound));

    slot.Bind("stable-node", nullptr, {}, {});
    UI::Interaction::DropHit hit{};
    EXPECT_TRUE(slot.HitTestDropTarget(8.0f, 12.0f, hit));
    EXPECT_EQ(hit.TargetId, GraphPortDropSlot::HashNodeId("stable-node"));
    EXPECT_EQ(hit.Location, UI::Interaction::DropLocation::OnItem);
    EXPECT_NE(hit.TargetId, 0u);

    slot.Unbind();
    UI::Interaction::DropHit afterUnbind{};
    EXPECT_FALSE(slot.HitTestDropTarget(8.0f, 12.0f, afterUnbind));
}

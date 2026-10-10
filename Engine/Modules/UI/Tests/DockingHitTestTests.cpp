#include <gtest/gtest.h>

#include "UI/Layout/Docking.h"
#include "UI/Layout/DockingHitTest.h"

using namespace GameEngine;

TEST(DockingHitTest, CenterIsTabOnSingleLeaf) {
    DockingManager dm;
    auto root = DockNode::MakeLeaf();
    root->AddTab("A");
    dm.SetRoot(std::move(root));

    DockDropTarget t = DockingHitTest::Compute(dm, 1000.0f, 800.0f, 500.0f, 400.0f);
    EXPECT_EQ(t.Kind, DockDropTarget::TargetKind::Tab);
    EXPECT_EQ(t.Path, ""); // root leaf path is empty
}

TEST(DockingHitTest, BottomInnerEdgeIsLeafSplit) {
    DockingManager dm;
    auto root = DockNode::MakeLeaf();
    root->AddTab("A");
    dm.SetRoot(std::move(root));

    // Near bottom edge inside the leaf
    // Choose y so we are inside the inner band but outside the root lock (db=41 > 40)
    DockDropTarget t = DockingHitTest::Compute(dm, 600.0f, 400.0f, 300.0f, 359.0f);
    EXPECT_EQ(t.Kind, DockDropTarget::TargetKind::LeafSplit);
    EXPECT_EQ(t.Edge, DockPosition::Bottom);
}



TEST(DockingHitTest, RootPreferredOverLeafNearOuterLeftEdge) {
    DockingManager dm;
    auto root = DockNode::MakeLeaf();
    root->AddTab("A");
    dm.SetRoot(std::move(root));

    const float W = 1000.0f, H = 800.0f;
    // Near the extreme left border, with Root-before-Leaf ordering, Root should win
    DockDropTarget t = DockingHitTest::Compute(dm, W, H, 10.0f, H * 0.5f);
    EXPECT_EQ(t.Kind, DockDropTarget::TargetKind::RootSplit);
    EXPECT_EQ(t.Edge, DockPosition::Left);
    EXPECT_TRUE(t.Path.empty());
}

TEST(DockingHitTest, RegionPreferredOverLeafInRegionBand) {
    DockingManager dm;

    // Build: root split Left/Right, with the Left side split Top/Bottom (nested region)
    auto leftTopBottom = DockNode::MakeSplit(DockPosition::Top, 0.5f);
    auto rightLeaf = DockNode::MakeLeaf();

    auto root = std::make_unique<DockNode>();
    root->SetSplit(DockPosition::Left, 0.5f, std::move(leftTopBottom), std::move(rightLeaf));
    dm.SetRoot(std::move(root));

    const float W = 1000.0f, H = 800.0f;
    // Hover within the left region's top band but outside the root lock band so Region wins over Leaf
    // Choose x in the middle of the left region to make Top the nearest edge
    const float x = W * 0.25f; // center of left half
    const float y = DockingHitTest::kOuterEdgeLockPx + 5.0f; // just inside region band above root band

    DockDropTarget t = DockingHitTest::Compute(dm, W, H, x, y);
    EXPECT_EQ(t.Kind, DockDropTarget::TargetKind::RegionSplit);
    EXPECT_EQ(t.Edge, DockPosition::Top);
    EXPECT_EQ(t.Path, "0"); // left region path
}

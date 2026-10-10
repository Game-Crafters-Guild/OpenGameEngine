#include <gtest/gtest.h>
#include "UI/Layout/Docking.h"
#include "UI/UIElement.h"

using namespace GameEngine;

TEST(DockingModel, SplitInitialization) {
    auto root = DockNode::MakeSplit(DockPosition::Left, 0.3f);
    ASSERT_TRUE(root);
    EXPECT_TRUE(root->IsSplit());
    EXPECT_EQ(root->GetSplitDirection(), DockPosition::Left);
    EXPECT_NEAR(root->GetSplitRatio(), 0.3f, 1e-6f);
}

TEST(DockingModel, RegisterAndGetPanel) {
    DockingManager dm;
    UIElement panel("DockPanel");
    UIElement* raw = &panel;
    dm.RegisterPanel("Assets", raw);
    UIElement* fetched = dm.GetPanel("Assets");
    EXPECT_EQ(raw, fetched);
}

TEST(DockingModel, SetRootAndRetrieve) {
    DockingManager dm;
    auto root = DockNode::MakeLeaf();
    root->AddTab("Assets");
    dm.SetRoot(std::move(root));
    const DockNode* r = dm.GetRoot();
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(r->IsLeaf());
    EXPECT_EQ(r->GetTabs().size(), 1u);
}


TEST(DockingModel, ComputeLeafLayouts_HorizontalSplit)
{
    DockingManager dm;
    auto root = DockNode::MakeSplit(DockPosition::Left, 0.25f);
    root->First()->AddTab("LeftPanel");
    root->Second()->AddTab("RightPanel");
    dm.SetRoot(std::move(root));

    std::vector<DockingManager::LeafLayout> out;
    dm.ComputeLeafLayouts(800.f, 600.f, out);
    ASSERT_EQ(out.size(), 2u);

    // Expect 200px left, 600px height; 600px right
    // Order is traversal order: first then second
    EXPECT_NEAR(out[0].x, 0.f, 1e-4f);
    EXPECT_NEAR(out[0].y, 0.f, 1e-4f);
    EXPECT_NEAR(out[0].width, 200.f, 1e-4f);
    EXPECT_NEAR(out[0].height, 600.f, 1e-4f);

    EXPECT_NEAR(out[1].x, 200.f, 1e-4f);
    EXPECT_NEAR(out[1].y, 0.f, 1e-4f);
    EXPECT_NEAR(out[1].width, 600.f, 1e-4f);
    EXPECT_NEAR(out[1].height, 600.f, 1e-4f);
}

TEST(DockingModel, ComputeLeafLayouts_VerticalSplit)
{
    DockingManager dm;
    auto root = DockNode::MakeSplit(DockPosition::Top, 0.5f);
    root->First()->AddTab("TopPanel");
    root->Second()->AddTab("BottomPanel");
    dm.SetRoot(std::move(root));

    std::vector<DockingManager::LeafLayout> out;
    dm.ComputeLeafLayouts(1024.f, 768.f, out);
    ASSERT_EQ(out.size(), 2u);

    EXPECT_NEAR(out[0].x, 0.f, 1e-4f);
    EXPECT_NEAR(out[0].y, 0.f, 1e-4f);
    EXPECT_NEAR(out[0].width, 1024.f, 1e-4f);
    EXPECT_NEAR(out[0].height, 384.f, 1e-4f);

    EXPECT_NEAR(out[1].x, 0.f, 1e-4f);
    EXPECT_NEAR(out[1].y, 384.f, 1e-4f);
    EXPECT_NEAR(out[1].width, 1024.f, 1e-4f);
    EXPECT_NEAR(out[1].height, 384.f, 1e-4f);
}
